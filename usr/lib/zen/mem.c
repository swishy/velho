#include <errno.h>
#include <zen/sys/unix.h>
#include <zen/_hash.h>
#include <zen/mem.h>
#include <zen/bitop.h>

struct tabhashtab              *g_memhashtab[TABHASH_SLOTS];
struct memglob                  g_mem ALIGNED(PAGESIZE);
THREADLOCAL struct memtls      *t_memtls;
struct tabhashtab              *g_memhashbuf;

#define memblkslabsize(qid)                                             \
    (((qid) <= MEM_BLK_MID_QUEUE_MIN)                                   \
     ? MEM_BLK_MIN_SLAB                                                \
     : (((qid) <= MEM_BLK_BIG_QUEUE_MIN)                                \
        ? MEM_BLK_MID_SLAB                                             \
        : MEM_BLK_BIG_SLAB))
static void
meminitblktabs(struct memglob *mem)
{
    for (unsigned long qid = 1; qid < MEM_BLK_QUEUES; ++qid) {
        mem->blktab[qid].qid = qid;
        mem->nblktab[qid] = memblkslabsize(qid) / memblksize(qid);
    }
}

#define memrunslabsize(qid)                                             \
    (((qid) < MEM_RUN_MID_QUEUE_MIN)                                    \
     ? MEM_RUN_MIN_SLAB                                                 \
     : (((qid) < MEM_RUN_BIG_QUEUE_MIN)                                 \
        ? MEM_RUN_MID_SLAB                                              \
        : MEM_RUN_BIG_SLAB))

/* Within each slab tier, slot count decreases as the class grows.
 * Prove both bounds at compile time, with no checks in the hot path.
 * Configurations exceeding one word (including current 32-bit BLK
 * geometry) must change their slab geometry before using this engine. */
#define MEM_CHECK_TIER(slab, stride, first, last)                         \
    _Static_assert((slab(first) / stride(first)) <= MEM_BUF_MAP_BITS     \
                   && (slab(last) / stride(last)) > 0,                  \
                   "every slab must contain 1..word-bits slots")
MEM_CHECK_TIER(memblkslabsize, memblksize, 1, MEM_BLK_MID_QUEUE_MIN);
MEM_CHECK_TIER(memblkslabsize, memblksize, MEM_BLK_MID_QUEUE_MIN + 1,
               MEM_BLK_BIG_QUEUE_MIN);
MEM_CHECK_TIER(memblkslabsize, memblksize, MEM_BLK_BIG_QUEUE_MIN + 1,
               MEM_BLK_QUEUES - 1);
MEM_CHECK_TIER(memrunslabsize, memrunsize, 1, MEM_RUN_MID_QUEUE_MIN - 1);
MEM_CHECK_TIER(memrunslabsize, memrunsize, MEM_RUN_MID_QUEUE_MIN,
               MEM_RUN_BIG_QUEUE_MIN - 1);
MEM_CHECK_TIER(memrunslabsize, memrunsize, MEM_RUN_BIG_QUEUE_MIN,
               MEM_RUN_QUEUES - 1);
#undef MEM_CHECK_TIER

static void
meminitruntabs(struct memglob *mem)
{
    for (unsigned long qid = 1; qid < MEM_RUN_QUEUES; ++qid) {
        mem->runtab[qid].qid = qid;
        mem->nruntab[qid] = memrunslabsize(qid) / memrunsize(qid);
    }
}

void
meminit(struct memglob *mem)
{
    mtlkfmtx(&mem->mtx);
    if (!(__atomic_load_n(&mem->flg, __ATOMIC_RELAXED) & MEM_INIT_BIT)) {
        meminitblktabs(mem);
        meminitruntabs(mem);
        __atomic_store_n(&mem->flg, MEM_INIT_BIT, __ATOMIC_RELEASE);
    }
    mtunlkfmtx(&mem->mtx);

    return;
}

/* Headers share one pool: BLK and RUN now have the same fixed layout. */
static struct membuf *
memgetbuf(void)
{
    volatile struct membuf **hdrq = &g_mem.bufhdrq;
    struct membuf *buf;

    mtlkbit((m_atomic_t *)hdrq, MEM_LK_BIT_OFS);
    buf = (void *)(__atomic_load_n((m_atomic_t *)hdrq, __ATOMIC_RELAXED)
                   & ~(uintptr_t)MEM_LK_BIT);
    if (!buf) {
        int8_t *region = mapanon(-1, MEM_CACHE_BUFS * membufsize(), 0);
        if (region == MAP_FAILED) {
            mtunlkval((m_atomic_t *)hdrq, 0);
            return NULL;
        }
        buf = (struct membuf *)region;
        for (size_t i = 0; i < MEM_CACHE_BUFS; ++i) {
            struct membuf *cur = (struct membuf *)(region + i * membufsize());
            cur->next = i + 1 < MEM_CACHE_BUFS
                ? (struct membuf *)(region + (i + 1) * membufsize()) : NULL;
        }
    }
    mtunlkval((m_atomic_t *)hdrq, (m_atomic_t)buf->next);
    return buf;
}

/* Only an empty, detached buffer returns its header to the pool. */
static void
memputbuf(struct membuf *buf)
{
    volatile struct membuf **hdrq = &g_mem.bufhdrq;

    mtlkbit((m_atomic_t *)hdrq, MEM_LK_BIT_OFS);
    buf->next = (void *)(__atomic_load_n((m_atomic_t *)hdrq, __ATOMIC_RELAXED)
                         & ~(uintptr_t)MEM_LK_BIT);
    mtunlkval((m_atomic_t *)hdrq, (m_atomic_t)buf);
}

/* Head accesses are atomic because lock waiters also read this word.
 * All helpers below require the queue lock, except lock/unlock themselves. */
static INLINE struct membuf *
memqhead(struct membufq *q)
{
    return (struct membuf *)(__atomic_load_n((m_atomic_t *)&q->head,
                                            __ATOMIC_RELAXED)
                            & ~(uintptr_t)MEM_LK_BIT);
}

static INLINE void
memqsethead(struct membufq *q, struct membuf *head)
{
    __atomic_store_n((m_atomic_t *)&q->head,
                     (m_atomic_t)((uintptr_t)head | MEM_LK_BIT),
                     __ATOMIC_RELAXED);
}

static INLINE void
memqlock(struct membufq *q)
{
    mtlkbit((m_atomic_t *)&q->head, MEM_LK_BIT_OFS);
}

static INLINE void
memqunlock(struct membufq *q)
{
    mtunlkval((m_atomic_t *)&q->head, (m_atomic_t)memqhead(q));
}

static void
memqueuebuf(struct membufq *q, struct membuf *buf)
{
    buf->prev = q->tail;
    buf->next = NULL;
    if (q->tail) {
        q->tail->next = buf;
    } else {
        memqsethead(q, buf);
    }
    q->tail = buf;
}

static void
memdequebuf(struct membufq *q, struct membuf *buf)
{
    if (buf->prev) {
        buf->prev->next = buf->next;
    } else {
        memqsethead(q, buf->next);
    }
    if (buf->next) {
        buf->next->prev = buf->prev;
    } else {
        q->tail = buf->prev;
    }
    buf->prev = buf->next = NULL;
}

static struct membuf *
memnewbuf(unsigned long qid, long type)
{
    size_t size = type == MEM_BLK ? memblkslabsize(qid) : memrunslabsize(qid);
    struct membuf *buf = memgetbuf();
    if (!buf) {
        return NULL;
    }
    buf->adr = mapanon(-1, size, 0);
    if (buf->adr == MAP_FAILED) {
        memputbuf(buf);
        return NULL;
    }
    buf->size = size;
    buf->max = type == MEM_BLK ? memnblk(qid) : memnrun(qid);
    buf->bits = UINTPTR_MAX >> (MEM_BUF_MAP_BITS - buf->max);
    return buf;
}

static void *
memgetsmall(unsigned long qid, long type, size_t align, size_t *retsize)
{
    struct membufq *q = type == MEM_BLK
        ? &t_memtls->blktab[qid] : &t_memtls->runtab[qid];
    size_t size = type == MEM_BLK ? memblksize(qid) : memrunsize(qid);
    memqlock(q);
    struct membuf *buf = memqhead(q);
    if (!buf) {
        memqunlock(q);
        struct membufq *glob = type == MEM_BLK
            ? &g_mem.blktab[qid] : &g_mem.runtab[qid];
        memqlock(glob);
        buf = memqhead(glob);
        if (buf) {
            memdequebuf(glob, buf);
            glob->nbuf--;
        }
        memqunlock(glob);
        if (!buf) {
            buf = memnewbuf(qid, type);
            if (!buf) {
                return NULL;
            }
        }
        memqlock(q);
        buf->queue = q;
        q->nbuf++;
        memqueuebuf(q, buf);
        buf = memqhead(q);
    }
    unsigned long index = m_ctz(buf->bits);
    int8_t *ptr = buf->adr + index * size;
    buf->bits &= buf->bits - 1;
    if (!buf->bits) {
        memdequebuf(q, buf);
    }
    memqunlock(q);
    if (align > (type == MEM_BLK ? CLSIZE : PAGESIZE)) {
        int8_t *aligned = memalignptr(ptr, align);
        if (retsize) {
            *retsize = size - (aligned - ptr);
        }
        ptr = aligned;
    } else if (retsize) {
        *retsize = size;
    }
    tabhashaddref(g_memhashtab, mempageadr(ptr), memsethashbuf(buf, qid, type));
    return ptr;
}

/* Caller holds the owner lock. The last free is the only operation that
 * can transfer or unmap a slab; all page references are already gone.
 * Nested lock order is local -> global. Allocation drops global first. */
static void
memreleaseempty(struct membuf *buf, long type)
{
    struct membufq *q = buf->queue;
    unsigned long qid = q->qid;
    long limit = type == MEM_BLK ? memnblktlsbuf(qid) : memnruntlsbuf(qid);
    if (q->nbuf <= limit) {
        return;
    }
    memdequebuf(q, buf);
    q->nbuf--;
    struct membufq *glob = type == MEM_BLK
        ? &g_mem.blktab[qid] : &g_mem.runtab[qid];
    limit = type == MEM_BLK ? memnblkbuf(qid) : memnrunbuf(qid);
    memqlock(glob);
    if (glob->nbuf < limit) {
        buf->queue = glob;
        glob->nbuf++;
        memqueuebuf(glob, buf);
        memqunlock(glob);
    } else {
        memqunlock(glob);
        memfreebuf(buf);
        memputbuf(buf);
    }
}

/* acquire big allocation */
static void *
memgetbig(size_t size, size_t align, size_t *retsize)
{
    /* GAP-FILL(vs): the acquired buffer header was vestigial - never
     * stored anywhere, never recycled: one leaked header per BIG
     * allocation drained the pools (malloc NULLs under perl). BIG is a
     * pure mmap path; no buffer header needed. */
    size_t              mapsz = roundup2(size, PAGESIZE);
    int8_t             *ptr;
    int8_t             *ret = NULL;
    size_t              aln;
    uintptr_t           page;
    uintptr_t           val;

    ptr = mapanon(-1, mapsz, 0);
    if (ptr == MAP_FAILED) {
        ptr = NULL;
    } else {
        ret = memalignptr(ptr, align);
        aln = ret - ptr;
        /* An over-aligned BIG mapping has a page-sized prefix. Drop it
         * now so the returned pointer is also the mapping's base. */
        if (aln) {
            unmapanon(ptr, aln);
            mapsz -= aln;
        }
        page = mempageadr(ret);
        val = mapsz | (MEM_BIG << MEM_BUF_QUEUE_BITS);
        tabhashadd(g_memhashtab, page, val);
        if (retsize) {
            *retsize = mapsz;
        }
    }

    return ret;
}

/* Allocate from the size class directly; only explicit callers request
 * usable capacity. malloc's ordinary path does not need to calculate it. */
void *
memget(size_t size, size_t align, size_t *retsize)
{
    size_t aln = max(align, MEM_ALIGN_MIN);
    size_t alnsz = memalnsize(size, aln);
    unsigned long qid;

    if (!size) {
        return NULL;
    }
    if (alnsz > SIZE_MAX - (PAGESIZE - 1)) {
        errno = ENOMEM;
        return NULL;
    }
    if (!(__atomic_load_n(&g_mem.flg, __ATOMIC_ACQUIRE) & MEM_INIT_BIT)) {
        meminit(&g_mem);
    }
    qid = memblkqid(alnsz);
    if (qid < MEM_BLK_QUEUES) {
        return memgetsmall(qid, MEM_BLK, aln, retsize);
    }
    qid = memrunqid(alnsz);
    if (qid < MEM_RUN_QUEUES) {
        return memgetsmall(qid, MEM_RUN, aln, retsize);
    }

    return memgetbig(alnsz, aln, retsize);
}

void
memput(void *ptr)
{
    uintptr_t page = mempageadr(ptr);
    struct memhash hash = tabhashrm(g_memhashtab, page);
    if (!hash.page) {
        return;
    }
    long type = memhashtype(&hash);
    if (type == MEM_BIG) {
        unmapanon((void *)page, hash.val & ~(uintptr_t)(PAGESIZE - 1));
        return;
    }
    /* This allocation's clear bit keeps its header and owner alive until
     * we hold the owner lock and mark it free. */
    struct membuf *buf = memhashbuf(&hash);
    struct membufq *q = buf->queue;
    size_t size = type == MEM_BLK ? memblksize(q->qid) : memrunsize(q->qid);
    size_t index = ((uintptr_t)ptr - (uintptr_t)buf->adr) / size;
    memqlock(q);
    uintptr_t old = buf->bits;
    buf->bits = old | ((uintptr_t)1 << index);
    if (!old) {
        memqueuebuf(q, buf);
    }
    if (buf->bits == (UINTPTR_MAX >> (MEM_BUF_MAP_BITS - buf->max))) {
        memreleaseempty(buf, type);
    }
    memqunlock(q);
}

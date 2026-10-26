#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <mt/mtx.h>
#include <zen/sys/unix.h>
#include <zen/_hash.h>
#include <zen/mem.h>
#include <zen/bitop.h>

struct tabhashtab              *g_memhashtab[TABHASH_SLOTS];
struct memglob                  g_mem ALIGNED(PAGESIZE);
THREADLOCAL struct memtls      *t_memtls;
struct memtls                  *t_memtls_p;
struct tabhashtab              *g_memhashbuf;

#define memblkslabsize(qid)                                             \
    (((qid) <= MEM_BLK_MID_QUEUE_MIN)                                   \
     ? (MEM_BLK_MIN_SLAB >> 2)                                          \
     : (((qid) <= MEM_BLK_BIG_QUEUE_MIN)                                \
        ? (MEM_BLK_MID_SLAB >> 1)                                       \
        : MEM_BLK_BIG_SLAB))
static void
meminitblktabs(struct memglob *mem)
{
    for (unsigned long qid = 1; qid < MEM_BLK_QUEUES; ++qid) {
        mem->nblktab[qid] = memblkslabsize(qid) / memblksize(qid);
    }
}

#define memrunslabsize(qid)                                             \
    (((qid) < MEM_RUN_MID_QUEUE_MIN)                                    \
     ? MEM_RUN_MIN_SLAB                                                 \
     : (((qid) < MEM_RUN_BIG_QUEUE_MIN)                                 \
        ? MEM_RUN_MID_SLAB                                              \
        : MEM_RUN_BIG_SLAB))
static void
meminitruntabs(struct memglob *mem)
{
    for (unsigned long qid = 1; qid < MEM_RUN_QUEUES; ++qid) {
        mem->nruntab[qid] = memrunslabsize(qid) / memrunsize(qid);
    }
}

void
meminit(struct memglob *mem)
{
    mtlkfmtx(&mem->mtx);
    if (!(mem->flg & MEM_INIT_BIT)) {
        meminitblktabs(mem);
        meminitruntabs(mem);
        /* GAP-FILL(vs): the init bit was never set, so every memget
         * re-ran this and early allocations could observe half-initialised
         * state */
        mem->flg |= MEM_INIT_BIT;
    }
    mtunlkfmtx(&mem->mtx);

    return;
}

#define MEM_FIND_PAGE   0
#define MEM_FIND_VAL    1
uintptr_t
memfindbuf(void *ptr, long which, uintptr_t mask)
{
    TABHASH_ITEM_T      item = tabhashfind(g_memhashtab, (uintptr_t)ptr);
    uintptr_t           ret = 0;

    if (item.page) {
        switch (which) {
            case MEM_FIND_PAGE:
                ret = item.page;

                break;
            case MEM_FIND_VAL:
                ret = item.val;
            default:

                break;
        }
    }
    ret &= mask;

    return ret;
}

/* acquire buffer header */
static struct membuf *
memgetbuf(long type)
{
    struct membufq               *queue = NULL;
    volatile struct membuf      **hdrq;
    long                          ntry = 32;
    size_t                        bufsz;
    size_t                        nbuf;
    struct membuf                *ret;
    int8_t                       *ptr;
    struct membuf                *buf;
    struct membuf                *head;
    struct membuf                *prev;

    ret = NULL;
    ptr = MAP_FAILED;
    if (type == MEM_BLK) {
        bufsz = memblkbufsize();
        nbuf = MEM_CACHE_BLK_BUFS;
        hdrq = &g_mem.blkhdrq;
    } else {
        bufsz = membufsize();
        nbuf = MEM_CACHE_BUFS;
        hdrq = &g_mem.bufhdrq;
    }
    do {
        mtlkbit((m_atomic_t *)hdrq, MEM_LK_BIT_OFS);
        ret = (void *)((uintptr_t)*hdrq & ~MEM_LK_BIT);
        if (!ret) {
            if (ptr == MAP_FAILED) {
                /* map header region */
                ptr = mapanon(-1, nbuf * bufsz, 0);
                if (ptr != MAP_FAILED) {
                    buf = (struct membuf *)ptr;
                    ret = (struct membuf *)ptr;
                    buf->queue = queue;
                    head = (struct membuf *)((int8_t *)buf + bufsz);
                    head->queue = queue;
                    head->prev = NULL;
                    prev = head;
                    while (--nbuf) {
                        buf = (struct membuf *)((int8_t *)buf + bufsz);
                        buf->queue = queue;
                        buf->prev = prev;
                        prev->next = buf;
                        prev = buf;
                    }
                    buf->next = NULL;
                    /* GAP-FILL(vs): the issued header was never linked to
                     * the remainder chain, so the new hdrq head read NULL
                     * and every region orphaned all but one header */
                    ret->next = head;
                }
            }
            if (ptr != MAP_FAILED) {
                ret = (struct membuf *)ptr;
            }
        }
        if (ret) {
            head = ret->next;
            if (head) {
                head->prev = NULL;
            }
        } else {
            head = NULL;
        }
        mtunlkval((m_atomic_t *)hdrq, (m_atomic_t)(uintptr_t)head);   /* MT: paired */
        if (!ret) {
            m_waitspin();
        }
    } while (!ret && --ntry);

    return ret;
}

/* release buffer header */
static void
memputbuf(struct membuf *buf, long type)
{
    volatile struct membuf    **hdrq;
    struct membuf              *head;

    /* GAP-FILL(vs): buf->adr is the in-header liveness reference - it is
     * NULL exactly when the header is on the freelist (this function sets
     * it, every acquire path assigns the slab). A second push of an
     * already-free header would alias it onto two lists, so make the
     * push idempotent on that pointer state. No refcount, no generation,
     * no side tables. */
    if (!buf->adr) {

        return;
    }
    buf->adr = NULL;
    buf->queue = NULL;
    buf->prev = NULL;
    if (type == MEM_BLK) {
        hdrq = &g_mem.blkhdrq;
    } else {
        hdrq = &g_mem.bufhdrq;
    }
    mtlkbit((m_atomic_t *)hdrq, MEM_LK_BIT_OFS);
    head = (void *)((uintptr_t)*hdrq & ~MEM_LK_BIT);
    buf->next = head;
    if (head) {
        head->prev = buf;
    }
    mtunlkval((m_atomic_t *)hdrq, (m_atomic_t)buf);   /* MT: paired */

    return;
}

/* Queues contain only buffers with free allocations. Their head stays in
 * place until exhausted; nbuf counts all owned buffers, including full ones. */
static void
memqueuebuf(struct membufq *queue, struct membuf *buf)
{
    buf->prev = queue->tail;
    buf->next = NULL;
    if (queue->tail) {
        queue->tail->next = buf;
    } else {
        queue->head = buf;
    }
    queue->tail = buf;
}

static void
memdequebuf(struct membufq *queue, struct membuf *buf)
{
    if (buf->prev) {
        buf->prev->next = buf->next;
    } else {
        queue->head = buf->next;
    }
    if (buf->next) {
        buf->next->prev = buf->prev;
    } else {
        queue->tail = buf->prev;
    }
    buf->prev = buf->next = NULL;
}

static struct membuf *
memnewbuf(unsigned long qid, long type)
{
    size_t size = type == MEM_BLK ? memblkslabsize(qid) : memrunslabsize(qid);
    struct membuf *buf = memgetbuf(type);
    if (!buf) {
        return NULL;
    }
    buf->adr = mapanon(-1, size, 0);
    if (buf->adr == MAP_FAILED) {
        memputbuf(buf, type);
        return NULL;
    }
    buf->size = size;
    buf->ofs = 0;
    if (type == MEM_BLK) {
        buf->max = memnblk(qid);
        for (size_t i = 0; i < buf->max; ++i) {
            buf->tab[i] = (uintptr_t)buf->adr + i * memblksize(qid);
        }
    } else {
        buf->max = memnrun(qid);
        buf->bits = UINTPTR_MAX >> (MEM_BUF_MAP_BITS - buf->max);
    }
    return buf;
}

static void *
memgetsmall(unsigned long qid, long type, size_t align, size_t *retsize)
{
    struct membufq *queue = type == MEM_BLK
        ? &t_memtls->blktab[qid] : &t_memtls->runtab[qid];
    struct membuf *buf = queue->head;
    int8_t *ptr;
    size_t size = type == MEM_BLK ? memblksize(qid) : memrunsize(qid);
    if (!buf) {
        struct membufq *glob = type == MEM_BLK
            ? &g_mem.blktab[qid] : &g_mem.runtab[qid];
        mtlkfmtx(&glob->mtx);
        buf = glob->head;
        if (buf) {
            /* Only empty buffers enter the global reserve. Transfer the
             * whole buffer once, then serve it locally. */
            memdequebuf(glob, buf);
            glob->nbuf--;
        }
        mtunlkfmtx(&glob->mtx);
        if (!buf) {
            buf = memnewbuf(qid, type);
            if (!buf) {
                return NULL;
            }
        }
        buf->queue = queue;
        queue->nbuf++;
        memqueuebuf(queue, buf);
    }
    if (type == MEM_BLK) {
        ptr = (int8_t *)buf->tab[buf->ofs++];
        if (buf->ofs == buf->max) {
            memdequebuf(queue, buf);
        }
    } else {
        unsigned long index = m_ctz(buf->bits);
        ptr = buf->adr + index * size;
        buf->bits &= buf->bits - 1;
        if (!buf->bits) {
            memdequebuf(queue, buf);
        }
    }
    /* Ordinary blocks/runs already satisfy the minimum alignment. */
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

/* An empty buffer is either retained, transferred to the global reserve,
 * or unmapped. All its page references have already been released. */
static void
memreleaseempty(struct membuf *buf, long type)
{
    struct membufq *queue = buf->queue;
    unsigned long qid = queue->qid;
    long limit = type == MEM_BLK ? memnblktlsbuf(qid) : memnruntlsbuf(qid);
    if (queue->nbuf <= limit) {
        return;
    }
    memdequebuf(queue, buf);
    queue->nbuf--;
    struct membufq *glob = type == MEM_BLK
        ? &g_mem.blktab[qid] : &g_mem.runtab[qid];
    limit = type == MEM_BLK ? memnblkbuf(qid) : memnrunbuf(qid);
    mtlkfmtx(&glob->mtx);
    if (glob->nbuf < limit) {
        buf->queue = glob;
        glob->nbuf++;
        memqueuebuf(glob, buf);
        mtunlkfmtx(&glob->mtx);
    } else {
        mtunlkfmtx(&glob->mtx);
        memfreebuf(buf);
        memputbuf(buf, type);
    }
}

/* acquire big allocation */
void *
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
        page = memsethashpage(mempageadr(ret), MEM_BIG);
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
static void memdeferdrain(struct memtls *tls);

void *
memget(size_t size, size_t align, size_t *retsize)
{
    /* owning thread activity: drain remote frees in passing (two
     * compares when empty; allocators that never free must still drain) */
    if (t_memtls && UNLIKELY(t_memtls->defertail != t_memtls->deferhead)) {
        memdeferdrain(t_memtls);
    }

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
    if (!(g_mem.flg & MEM_INIT_BIT)) {
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

#define memsize(ptr) 0 /* FIXME */
void *
memresize(void *ptr, size_t size, size_t align, long flg)
{
    size_t      alnsz = memalnsize(size, align);
    size_t      srcsz = (ptr) ? memsize(ptr) : 0;
    void       *adr = (ptr) && srcsz >= alnsz ? ptr : NULL;
    size_t      cpysz = 0;
    size_t      destsz;

    if (!adr) {
        if (ptr) {
            adr = memget(size, align, &destsz);
        }

        return NULL;
    }
#if 0
    if ((!ptr && (flg & MEM_ALLOC_ON_NULL))
        || !adr) {
        adr = memget(size, align, &destsz);
    } else {
        adr = ptr;
    }
#endif
    if (adr) {
        if ((ptr) && adr != ptr) {
            cpysz = min(srcsz, destsz);
            memcpy(adr, ptr, cpysz);
        }
    }
    if (!adr) {
        errno = ENOMEM;
        if (flg & MEM_FREE_ON_FAILURE) {
            memput(ptr);
        }
    }

    return adr;
}

/* Kept for the existing stress harness's diagnostic ABI. */
static struct bigunmap { uintptr_t page; size_t size; } g_bigu[256];
static unsigned g_bigui;
struct bigunmap *g_bigu_pub = g_bigu;
unsigned *g_bigui_pub = &g_bigui;

/* remote-free: apply a deferred free on the owning thread */
static void
memdeferapply(struct memtls *tls, struct membuf *buf, void *ptr, long type)
{
    struct membufq *queue = buf->queue;
    unsigned long   qid = queue->qid;

    if (type == MEM_BLK) {
        size_t index = memblknum(qid, buf, ptr);
        if (buf->ofs == buf->max) {
            memqueuebuf(queue, buf);
        }
        buf->tab[--buf->ofs] = (uintptr_t)buf->adr + index * memblksize(qid);
        if (!buf->ofs) {
            memreleaseempty(buf, type);
        }
    } else {
        size_t index = memrunnum(qid, buf, ptr);
        if (!buf->bits) {
            memqueuebuf(queue, buf);
        }
        buf->bits |= (uintptr_t)1 << index;
        if (buf->bits == (UINTPTR_MAX >> (MEM_BUF_MAP_BITS - buf->max))) {
            memreleaseempty(buf, type);
        }
    }
}

static void
memdeferdrain(struct memtls *tls)
{
    if (tls->defertail == tls->deferhead) {

        return;
    }
    while (__atomic_test_and_set(&tls->deferlk, __ATOMIC_ACQUIRE)) {
        m_waitspin();
    }
    while (tls->defertail != tls->deferhead) {
        struct memdefer *d = &tls->defer[tls->defertail % MEM_DEFER_N];

        if (d->buf && d->buf->queue) {
            memdeferapply(tls, d->buf, d->ptr, d->type);
        }
        tls->defertail++;
    }
    __atomic_clear(&tls->deferlk, __ATOMIC_RELEASE);

    return;
}

/* MPSC push, spinlocked: the remote-free path is the slow path - an
 * obviously-correct lock beats a clever lock-free ring. A full ring
 * waits for the owner to drain (the owner never blocks on producers). */
static void
memdeferpush(struct memtls *tls, struct membuf *buf, void *ptr, long type)
{
    while (__atomic_test_and_set(&tls->deferlk, __ATOMIC_ACQUIRE)) {
        m_waitspin();
    }
    long spins = 0;
    while (tls->deferhead - tls->defertail >= MEM_DEFER_N) {
        if (++spins > (1L << 20)
            && !__atomic_load_n(&tls->alive, __ATOMIC_ACQUIRE)) {
            /* owner exited: nobody will drain - apply directly under the
             * ring lock (serialised against other dead-owner applies) */
            memdeferapply(tls, buf, ptr, type);
            __atomic_clear(&tls->deferlk, __ATOMIC_RELEASE);

            return;
        }
        __atomic_clear(&tls->deferlk, __ATOMIC_RELEASE);
        m_waitspin();
        while (__atomic_test_and_set(&tls->deferlk, __ATOMIC_ACQUIRE)) {
            m_waitspin();
        }
    }
    {
        struct memdefer *d = &tls->defer[tls->deferhead % MEM_DEFER_N];

        d->buf = buf;
        d->ptr = ptr;
        d->type = type;
        tls->deferhead++;
    }
    __atomic_clear(&tls->deferlk, __ATOMIC_RELEASE);

    return;
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
        size_t size = (size_t)(hash.val & ~(uintptr_t)(PAGESIZE - 1));
        unmapanon((void *)page, size);
        return;
    }
    struct membuf *buf = memhashbuf(&hash);
    struct membufq *queue = buf->queue;
    /* Removal returned the packed owner. Decode it directly, before a
     * last free can release the slab; no second hash lookup is needed. */
    if (queue->tls && queue->tls != (struct memtls *)t_memtls) {
        /* remote free: the buffer belongs to another thread's domain -
         * defer; the owner applies it (cross-thread mutation of its
         * buffers is unsynchronised otherwise) */
        memdeferpush(queue->tls, buf, ptr, type);

        return;
    }
    if (!queue->tls) {
        /* global-reserve buffer: mutate under the queue lock (mirrors
         * the allocation path) */
        mtlkfmtx(&queue->mtx);
        memdeferapply(t_memtls, buf, ptr, type);
        mtunlkfmtx(&queue->mtx);

        return;
    }
    memdeferapply(t_memtls, buf, ptr, type);
    /* own domain: drain remote frees in passing (bounded, cheap when
     * empty) */
    memdeferdrain(t_memtls);
}

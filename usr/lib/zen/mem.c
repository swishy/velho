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

#define MEM_SEALED ((m_atomic_t)-1)

static void *
memgetsmall(unsigned long qid, long type, size_t align, size_t *retsize)
{
    struct membufq *queue = type == MEM_BLK
        ? &t_memtls->blktab[qid] : &t_memtls->runtab[qid];
    struct membuf *buf;
    int8_t *ptr;
    size_t size = type == MEM_BLK ? memblksize(qid) : memrunsize(qid);
retry:
    if (!queue->head) {
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
        buf->ofs = 0;
        mtlkfmtx(&queue->mtx);
        buf->queue = queue;
        queue->nbuf++;
        memqueuebuf(queue, buf);
        mtunlkfmtx(&queue->mtx);
    }
    /* The take pairs with the free/release side under ONE hold of the
     * queue lock: the head-usable invariant (a queued buffer always has
     * capacity) is only released with the lock. Failed increments are
     * undone before retry - a leaked overshoot drifts ofs past max and
     * every later take misreads it as sealed (the 8-worker livelock). */
    mtlkfmtx(&queue->mtx);
    buf = queue->head;
    if (!buf) {
        mtunlkfmtx(&queue->mtx);
        goto retry;
    }
    if (type == MEM_BLK) {
        m_atomic_t old = __atomic_fetch_add(&buf->ofs, 1, __ATOMIC_ACQ_REL);
        if (old >= (m_atomic_t)buf->max) {
            __atomic_fetch_sub(&buf->ofs, 1, __ATOMIC_ACQ_REL);
            if (old == (m_atomic_t)buf->max && buf == queue->head) {
                memdequebuf(queue, buf);   /* exhausted: leave now */
            }
            mtunlkfmtx(&queue->mtx);
            goto retry;
        }
        ptr = (int8_t *)buf->tab[old];
        mtunlkfmtx(&queue->mtx);
    } else {
        uintptr_t bits = __atomic_load_n(&buf->bits, __ATOMIC_ACQUIRE);
        if (!bits || buf->ofs == MEM_SEALED) {
            if (buf == queue->head) {
                memdequebuf(queue, buf);   /* exhausted or sealed */
            }
            mtunlkfmtx(&queue->mtx);
            goto retry;
        }
        unsigned long index = m_ctz(bits);
        uintptr_t next = bits & (bits - 1);
        if (!__atomic_compare_exchange_n(&buf->bits, &bits, next, 1,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            mtunlkfmtx(&queue->mtx);
            goto retry;
        }
        ptr = buf->adr + index * size;
        if (!next && buf == queue->head) {
            memdequebuf(queue, buf);       /* exhausted */
        }
        mtunlkfmtx(&queue->mtx);
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
    /* caller holds the owning queue lock; the state was re-derived from
     * the atomic mark, so the buffer is genuinely empty */
    struct membufq *queue = buf->queue;
    unsigned long qid = queue->qid;
    long limit = type == MEM_BLK ? memnblktlsbuf(qid) : memnruntlsbuf(qid);
    if (queue->nbuf <= limit) {
        return;                     /* keep the empty buffer local */
    }
    m_atomic_t exp = 0;
    if (type == MEM_BLK) {
        if (!__atomic_compare_exchange_n(&buf->ofs, &exp, MEM_SEALED, 0,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            return;                 /* an allocator took a block */
        }
    } else {
        uintptr_t bits = __atomic_load_n(&buf->bits, __ATOMIC_ACQUIRE);
        uintptr_t full = UINTPTR_MAX >> (MEM_BUF_MAP_BITS - buf->max);
        if (bits != full || buf->ofs == MEM_SEALED) {
            return;
        }
        buf->ofs = MEM_SEALED;      /* under the queue lock */
    }
    memdequebuf(queue, buf);
    queue->nbuf--;
    struct membufq *glob = type == MEM_BLK
        ? &g_mem.blktab[qid] : &g_mem.runtab[qid];
    limit = type == MEM_BLK ? memnblkbuf(qid) : memnrunbuf(qid);
    mtlkfmtx(&glob->mtx);
    if (glob->nbuf < limit) {
        buf->ofs = 0;               /* unseal: reusable from the reserve */
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
    unsigned long qid = queue->qid;
    /* Removal returned the packed owner. The mark itself is atomic
     * (bits/ofs are RMW words - lock bts/btr is atomic on any silicon),
     * so ANY thread applies the free directly; the block is allocatable
     * immediately. Queue membership changes only at lifecycle
     * transitions, under the queue lock. */
    mtlkfmtx(&queue->mtx);
    if (type == MEM_BLK) {
        size_t index = memblknum(qid, buf, ptr);
        m_atomic_t old = __atomic_fetch_sub(&buf->ofs, 1, __ATOMIC_ACQ_REL);
        buf->tab[old - 1] = (uintptr_t)buf->adr + index * memblksize(qid);
        if (old == (m_atomic_t)buf->max
            && queue->head != buf && !buf->prev && !buf->next) {
            memqueuebuf(queue, buf);       /* revival: exactly-once */
        }
        if (old == 1) {
            memreleaseempty(buf, type);    /* last free */
        }
    } else {
        size_t index = memrunnum(qid, buf, ptr);
        uintptr_t bit = (uintptr_t)1 << index;
        uintptr_t old = __atomic_fetch_or(&buf->bits, bit, __ATOMIC_ACQ_REL);
        uintptr_t full = UINTPTR_MAX >> (MEM_BUF_MAP_BITS - buf->max);
        if (!old && queue->head != buf && !buf->prev && !buf->next) {
            memqueuebuf(queue, buf);       /* revival: exactly-once */
        }
        if ((old | bit) == full) {
            memreleaseempty(buf, type);    /* last free */
        }
    }
    mtunlkfmtx(&queue->mtx);
}

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
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

struct qop { char op; const struct membufq *q; const struct membuf *b; long nbuf; };
static struct qop g_qops[512];
static unsigned g_qopi;
struct qop2 *g_qops2_alias = (struct qop2 *)g_qops;
unsigned *g_qopi2 = &g_qopi;
struct qop2 { char op; const void *q; const void *b; long nbuf; };
struct qop2 *g_qops2 = (struct qop2 *)0;
static void
qlog(char op, const struct membufq *q, const struct membuf *b)
{
    (void)op; (void)q; (void)b;   /* diagnostics compiled out */
}
static void mempurgepage(uintptr_t page, struct membuf *own);
static int  membufowns(struct membuf *buf, uintptr_t page);

#define memblkslabsize(qid)                                             \
    (((qid) <= MEM_BLK_MID_QUEUE_MIN)                                   \
     ? (MEM_BLK_MIN_SLAB >> 2)                                          \
     : (((qid) <= MEM_BLK_BIG_QUEUE_MIN)                                \
        ? (MEM_BLK_MID_SLAB >> 1)                                       \
        : MEM_BLK_BIG_SLAB))
static void
meminitblktabs(struct memglob *mem)
{
    double              slabsz;
    double              blksz;
    double              nblk;
    double              multi;
    unsigned long       qid;

    /* GAP-FILL(vs): the tables were built with (qid + 1) while every
     * accessor strides by qid (memblksize(qid) = CLSIZE * qid, qid >= 1
     * from memblkqid) - an index-shifted table that made memblknum and
     * memrunnum compute WRONG block/run indices, so frees set the wrong
     * bitmap bits: premature all-free, live slabs released and re-handed
     * (the perl corruption). Build them for the accessor stride; index 0
     * is unused (division guard). */
    for (qid = 1 ; qid < MEM_BLK_QUEUES ; qid++) {
        slabsz = memblkslabsize(qid);
        blksz = MEM_BLK_MIN * (double)qid;
        nblk = slabsz / blksz;
        multi = 1.0 / blksz;
        mem->nblktab[qid] = (long)nblk;
        mem->blkdivtab[qid] = multi;
    }

    return;
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
    double              slabsz;
    double              runsz;
    double              nrun;
    double              multi;
    unsigned long       qid;

    /* GAP-FILL(vs): same index shift as the block tables - see above */
    for (qid = 1 ; qid < MEM_RUN_QUEUES ; qid++) {
        slabsz = (double)memrunslabsize(qid);
        runsz = PAGESIZE * (double)qid;
        nrun = slabsz / runsz;
        multi = 1.0 / runsz;
        mem->nruntab[qid] = (long)nrun;
        mem->rundivtab[qid] = multi;
    }

    return;
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
    qlog('R', NULL, buf);
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

/* push buffer in front of queue */
static void
mempushbuf(struct membufq *queue, struct membuf *buf)
{
    qlog('U', queue, buf);
    struct membuf       *head = queue->head;

    /* GAP-FILL(vs): queue==NULL marks a header resident on the header
     * freelist - pushing one onto a buffer queue would alias it */
    if (!buf->queue) {

        return;
    }
    buf->prev = NULL;
    buf->next = head;
    queue->nbuf++;
    if (head) {
        head->prev = buf;
    } else {
        queue->tail = buf;
    }
    queue->head = buf;

    return;
}

static void
mempopbuf(struct membufq *queue, struct membuf *buf)
{
    qlog('P', queue, buf);
    struct membuf       *next;

    /* GAP-FILL(vs): memdequebuf decrements nbuf but this pop did not, so
     * queue counts only ever grew and the lim gates misfired */
    queue->nbuf--;
    next = buf->next;
    if (next) {
        next->prev = NULL;
    } else {
        queue->tail = NULL;
    }
    queue->head = next;

    return;
}

/* queue buffer in back of queue */
static void
memqueuebuf(struct membufq *queue, struct membuf *buf)
{
    qlog('Q', queue, buf);
    struct membuf      *tail = queue->tail;

    buf->next = NULL;
    buf->prev = tail;
    queue->nbuf++;
    if (tail) {
        tail->next = buf;
    } else {
        queue->head = buf;
    }
    queue->tail = buf;

    return;
}

/* remove buffer from queue */
static void
memdequebuf(struct membufq *queue, struct membuf *buf)
{
    qlog('D', queue, buf);
    struct membuf      *prev;
    struct membuf      *next;

    prev = buf->prev;
    next = buf->next;
    queue->nbuf--;
    if (prev) {
        prev->next = buf->next;
    } else {
        queue->head = next;
    }
    if (next) {
        next->prev = buf->prev;
    } else {
        queue->tail = prev;
    }

    return;
}

/* GAP-FILL(vs): rewritten queue interaction - iterate, don't pop. The
 * upstream design unlinked a buffer at exhaustion, leaving live slabs
 * resident on NO queue: invisible to every later consistency check (and
 * to memrangebusy). Buffers now stay linked for their whole lifetime and
 * allocation simply skips exhausted ones. */
static void *
memgetblk(unsigned long qid, size_t align, size_t *retsize)
{
    int8_t             *ptr = NULL;
    int8_t             *ret = NULL;
    int8_t             *gptr = NULL;
    struct membuf      *gbuf = NULL;
    struct membuf      *buf;
    long                ntry = 32;
    size_t              slabsz;
    size_t              blksz = memblksize(qid);
    uintptr_t          *tab;
    long                nblk;
    long                n;
    size_t              ofs;
    size_t              lim;
    size_t              diff;
    uintptr_t           page;
    uintptr_t           val;

    buf = t_memtls ? t_memtls->blktab[qid].head : NULL;
    while (buf && buf->ofs >= buf->max && buf->next != buf) {
        if (UNLIKELY(!buf->adr || buf->ofs > buf->max
                     || buf->queue != &t_memtls->blktab[qid])) {
            char _b[160]; int _o = snprintf(_b, 150,
                "TLSWALK-POISON qid=%lu buf=%p adr=%p ofs=%ld max=%ld q=%p\n",
                qid, (void *)buf, (void *)buf->adr,
                (long)buf->ofs, (long)buf->max, (void *)buf->queue);
            if (write(2, _b, _o)) {}
            abort();
        }
        buf = buf->next;                /* exhausted - stay linked, skip */
    }
    if (buf && buf->ofs >= buf->max) {
        buf = NULL;
    }
    if (!buf) {
        mtlkfmtx(&g_mem.blktab[qid].mtx);
        for (buf = g_mem.blktab[qid].head; buf && buf->next != buf;
             buf = buf->next) {
            if (buf->ofs < buf->max) {
                break;
            }
        }
        if (buf && buf->ofs >= buf->max) {
            buf = NULL;
        }
        if (buf) {
            /* MT: allocate the block while still holding the global
             * queue lock - releasing first let two threads walk to the
             * same buffer and hand out the same block */
            ofs = buf->ofs;
            gptr = (int8_t *)buf->tab[ofs];
            buf->ofs = ofs + 1;
            gbuf = buf;
        }
        if (!buf) {
            slabsz = memblkslabsize(qid);
            mtunlkfmtx(&g_mem.blktab[qid].mtx);
            buf = memgetbuf(MEM_BLK);
            if (!buf) {

                return NULL;
            }
            do {
                ptr = mapanon(-1, slabsz, 0);
                if (ptr == MAP_FAILED) {
                    m_waitspin();
                }
            } while (ptr == MAP_FAILED && --ntry);
            if (ptr == MAP_FAILED) {
                buf->prev = buf;  /* GAP-FILL(vs): kill stale links */
                buf->next = buf;
                memputbuf(buf, MEM_BLK);
                buf = NULL;
            } else {
                nblk = memnblk(qid);
                buf->adr = ptr;
                buf->size = slabsz;
                buf->queue = &t_memtls->blktab[qid];
                tab = buf->tab;
                buf->max = nblk;
                n = nblk >> 3;
                nblk -= 8 * n;
                while (n--) {
                    tab[0] = (uintptr_t)ptr;
                    tab[1] = (uintptr_t)ptr + blksz;
                    tab[2] = (uintptr_t)ptr + 2 * blksz;
                    tab[3] = (uintptr_t)ptr + 3 * blksz;
                    tab[4] = (uintptr_t)ptr + 4 * blksz;
                    tab[5] = (uintptr_t)ptr + 5 * blksz;
                    tab[6] = (uintptr_t)ptr + 6 * blksz;
                    tab[7] = (uintptr_t)ptr + 7 * blksz;
                    ptr += 8 * blksz;
                    tab += 8;
                }
                n = min(8, nblk);
                switch (nblk) {
                    case 8: tab[7] = (uintptr_t)ptr + 7 * blksz;
                    case 7: tab[6] = (uintptr_t)ptr + 6 * blksz;
                    case 6: tab[5] = (uintptr_t)ptr + 5 * blksz;
                    case 5: tab[4] = (uintptr_t)ptr + 4 * blksz;
                    case 4: tab[3] = (uintptr_t)ptr + 3 * blksz;
                    case 3: tab[2] = (uintptr_t)ptr + 2 * blksz;
                    case 2: tab[1] = (uintptr_t)ptr + blksz;
                    case 1: tab[0] = (uintptr_t)ptr;
                    case 0:
                    default:

                        break;
                }
                ptr = (int8_t *)buf->tab[0];
                buf->ofs = 1;
                mempushbuf(&t_memtls->blktab[qid], buf);
            }
        } else {
            /* GAP-FILL(vs): a usable global buffer stays on the global
             * queue (visible to checks); allocate from it in place */
            mtunlkfmtx(&g_mem.blktab[qid].mtx);
        }
    }
    if (gbuf) {
        buf = gbuf;
        ptr = gptr;
        ret = memalignptr(ptr, align);  /* block taken under global lock */
    } else if (buf && ptr) {
        /* fresh buffer: first block already selected above */
        ret = memalignptr(ptr, align);
    } else if (buf) {
        ofs = buf->ofs;
        ptr = (int8_t *)buf->tab[ofs];
        buf->ofs = ofs + 1;             /* exhausted buffers stay linked */
        ret = memalignptr(ptr, align);
    }
    if (ret) {
        if (UNLIKELY((uintptr_t)ret < (uintptr_t)buf->adr
                     || (uintptr_t)ret
                        >= (uintptr_t)buf->adr + buf->size)) {

            return NULL;
        }
        ofs = buf->max;
        n = memblknum(qid, buf, ret);
        ofs += n;
        diff = ret - ptr;
        page = memsethashpage(mempageadr(ret), MEM_BLK);
        val = memsethashbuf(buf, qid, MEM_BLK);
        buf->tab[ofs] = (uintptr_t)diff;
        /* GAP-FILL(vs): blocks sharing a page REF one page entry (nref)
         * - vendu's tabhashaddref/tabhashref machinery - instead of
         * piling duplicate entries per block */
        if (!tabhashaddref(g_memhashtab, page, val)) {
            tabhashadd(g_memhashtab, page, val);
        }
        if (retsize) {
            *retsize = blksz;
        }
    }

    return ret;
}

/* GAP-FILL(vs): rewritten like memgetblk - iterate, don't pop. Buffers
 * stay linked for their whole lifetime; allocation skips exhausted ones,
 * so every live slab remains visible to consistency checks. */
static void *
memgetrun(unsigned long qid, size_t align, size_t *retsize)
{
    int8_t             *ret = NULL;
    int8_t             *ptr = NULL;
    int8_t             *gptr = NULL;
    struct membuf      *gbuf = NULL;
    long                gbits = 0;
    long                gofs = 0;
    long                gmask = 0;
    struct membuf      *buf;
    long                ntry = 32;
    size_t              nrun;
    size_t              slabsz;
    size_t              runsz = memrunsize(qid);
    long                bits;
    long                mask;
    long                ofs;
    uintptr_t           page;
    uintptr_t           val;
    uintptr_t           diff;

    buf = t_memtls ? t_memtls->runtab[qid].head : NULL;
    while (buf && !buf->bits && buf->next != buf) {
        buf = buf->next;                /* exhausted - stay linked, skip */
    }
    if (buf && !buf->bits) {
        buf = NULL;
    }
    if (!buf) {
        mtlkfmtx(&g_mem.runtab[qid].mtx);
        for (buf = g_mem.runtab[qid].head; buf && buf->next != buf;
             buf = buf->next) {
            if (buf->bits) {
                break;
            }
        }
        if (buf && !buf->bits) {
            buf = NULL;
        }
        if (buf) {
            /* MT: select the run while holding the global queue lock */
            gbits = buf->bits;
            gofs = m_ctz(gbits);
            gmask = ~(1L << gofs);
            gofs <<= PAGESIZELOG2;
            gofs *= qid;
            gbits &= gmask;
            gptr = buf->adr + gofs;
            buf->bits = gbits;
            gbuf = buf;
        }
        if (!buf) {
            slabsz = memrunslabsize(qid);
            nrun = memnrun(qid);
            mtunlkfmtx(&g_mem.runtab[qid].mtx);
            buf = memgetbuf(MEM_RUN);
            if (!buf) {

                return NULL;
            }
            do {
                ptr = mapanon(-1, slabsz, 0);
                if (ptr == MAP_FAILED) {
                    m_waitspin();
                }
            } while (ptr == MAP_FAILED && --ntry);
            bits = 1L << nrun;
            if (ptr == MAP_FAILED) {
                buf->prev = buf;  /* GAP-FILL(vs): kill stale links */
                buf->next = buf;
                memputbuf(buf, MEM_RUN);
                buf = NULL;
            } else {
                bits--;
                /* first run is handed out below: clear its bit */
                bits &= ~(uintptr_t)1;
                buf->adr = ptr;
                buf->size = slabsz;
                buf->bits = bits;
                buf->max = nrun;
                buf->queue = &t_memtls->runtab[qid];
                mempushbuf(&t_memtls->runtab[qid], buf);
            }
        } else {
            mtunlkfmtx(&g_mem.runtab[qid].mtx);
        }
    }
    if (buf && ptr) {
        /* fresh buffer: first run selected above */
        ret = memalignptr(ptr, align);
    } else if (buf) {
        bits = buf->bits;
        ofs = m_ctz(bits);
        mask = ~(1L << ofs);
        ofs <<= PAGESIZELOG2;
        ofs *= qid;
        bits &= mask;
        ptr = buf->adr + ofs;
        buf->bits = bits;               /* exhausted stays linked */
        ret = memalignptr(ptr, align);
    }
    if (gbuf && !ret) {
        buf = gbuf;
        ptr = gptr;
        ret = memalignptr(ptr, align);
    }
    if (ret) {
        ofs = memrunnum(qid, buf, ret);
        diff = ret - ptr;
        page = memsethashpage(mempageadr(ret), MEM_RUN);
        val = memsethashbuf(buf, qid, MEM_RUN);
        buf->tab[ofs] = (uintptr_t)diff;
        if (!tabhashaddref(g_memhashtab, page, val)) {
            tabhashadd(g_memhashtab, page, val);
        }
        if (retsize) {
            *retsize = runsz;
        }
    }

    return ret;
}

/* GAP-FILL(vs): page-hash entries are fungible per page - freeing a block
 * removes *an* entry of its page, not necessarily its own. When a buffer is
 * released and unmapped, any entries surviving for its pages would alias
 * whatever gets mapped there next, so release purges them. */
static void
mempurgebufentries(struct membuf *buf, size_t stride)
{
    size_t n;
    size_t i;

    if (!stride) {
        return;
    }
    n = buf->size / stride;
    if (!n) {
        n = 1;
    }
    for (i = 0; i < n; i++) {
        uintptr_t page = mempageadr(buf->adr + i * stride);
        while (tabhashfind(g_memhashtab, page).page) {
            tabhashrm(g_memhashtab, page);
        }
    }

    return;
}

/* GAP-FILL(vs): ownership check from in-context pointer refs alone - the
 * page-hash entry names a buffer header, and the header names its slab
 * ([adr, adr+size)) and its queue. An entry that fails this check has
 * outlived its buffer (stale); callers drop it from the table, which
 * makes the hash self-healing without any external tracking. */
/* GAP-FILL(vs): the allocator adding an entry for a page owns the ground
 * truth; drop any foreign entries (different buffer, or BIG leftovers)
 * still keyed there before adding. */
static void
mempurgepage(uintptr_t page, struct membuf *own)
{
    while (1) {
        struct memhash item = tabhashfind(g_memhashtab, page);
        long           type;
        struct membuf  *buf;

        if (!item.page) {

            return;
        }
        type = memhashtype(&item);
        buf = (struct membuf *)memhashbuf(&item);
        if (type != MEM_BIG && buf == own && membufowns(buf, page)) {

            return;             /* our own entry - keep it */
        }
        tabhashrm(g_memhashtab, page);
    }
}

/* GAP-FILL(vs): a BIG entry cannot self-validate (it packs size, not an
 * owner), so before honoring its unmap, verify that no live buffer on any
 * queue intersects the claimed range - a stale BIG entry that happens to
 * win a page lookup must not be able to mass-unmap recycled pages. */
static int
memrangebusy(uintptr_t lo, size_t len)
{
    struct membuf *buf;
    uintptr_t     hi = lo + len;
    int           t;
    unsigned long q;

    for (t = 0; t < 2; t++) {
        for (q = 0; q < MEM_BLK_QUEUES; q++) {
            struct membufq *queue = t
                ? &g_mem.runtab[q] : &g_mem.blktab[q];
            for (buf = queue->head; buf; buf = buf->next) {
                if ((uintptr_t)buf->adr < hi
                    && (uintptr_t)buf->adr + buf->size > lo) {

                    return 1;
                }
                if (buf == buf->next) {
                    break;          /* unlinked marker */
                }
            }
        }
        if (t_memtls) {
            for (q = 0; q < MEM_BLK_QUEUES; q++) {
                struct membufq *queue = t
                    ? &t_memtls->runtab[q] : &t_memtls->blktab[q];
                for (buf = queue->head; buf; buf = buf->next) {
                    if ((uintptr_t)buf->adr < hi
                        && (uintptr_t)buf->adr + buf->size > lo) {

                        return 1;
                    }
                    if (buf == buf->next) {
                        break;      /* unlinked marker */
                    }
                }
            }
        }
    }

    return 0;
}

/* GAP-FILL(vs): at run-buffer release, bits may lie (a wrong-index free
 * can complete the bitmap while a run is live). Page-hash entries cannot:
 * a live run's entry is only removed by its own free. Returns 1 when the
 * slab is provably free of entries (safe to release), 0 otherwise. */
static int
mempreleasesafe(struct membuf *buf, size_t stride)
{
    size_t n;
    size_t i;

    if (!stride) {
        return 1;
    }
    n = buf->size / stride;
    if (!n) {
        n = 1;
    }
    for (i = 0; i < n; i++) {
        uintptr_t page = mempageadr(buf->adr + i * stride);
        if (tabhashfind(g_memhashtab, page).page) {

            return 0;
        }
    }

    return 1;
}

static int
membufowns(struct membuf *buf, uintptr_t page)
{
    if (!buf || !buf->adr || !buf->queue) {

        return 0;
    }
    if (page < (uintptr_t)buf->adr
        || page >= (uintptr_t)buf->adr + buf->size) {

        return 0;
    }

    return 1;
}

/* release allocation block */
static void
memputblk(struct membuf *buf, void *ptr)
{
    struct membufq     *queue = buf->queue;
    unsigned long       qid = queue->qid;
    struct memtls      *tls = queue->tls;
    long                lim = (tls) ? memnblktlsbuf(qid) : memnblkbuf(qid);
    struct membufq     *glob;
    void               *adr;
    long                ndx;
    long                max;
    long                nbuf;

    if (!tls) {
        mtlkfmtx(&queue->mtx);
    }
    adr = memgetptr(ptr);
    if (UNLIKELY(!adr)) {
        /* undecodable (stale) entry - drop the free rather than push a
         * NULL onto the allocation stack (GAP-FILL(vs)) */
        if (!tls) {
            mtunlkfmtx(&queue->mtx);
        }

        return;
    }
    ndx = buf->ofs;
    max = buf->max;
    ndx--;
    nbuf = queue->nbuf;
    max--;
    buf->tab[ndx] = (uintptr_t)adr;
    buf->ofs = ndx;
    /* GAP-FILL(vs): with buffers permanently linked there is nothing to
     * requeue - the old tail-reinsert double-linked live buffers */
    if (!ndx && nbuf > lim) {
        memdequebuf(queue, buf);
        if (!tls) {
            mempurgebufentries(buf, PAGESIZE);
            memfreebuf(buf);           /* GAP-FILL(vs): unmap before the */
            buf->prev = buf;  /* GAP-FILL(vs): kill stale links */
            buf->next = buf;
            memputbuf(buf, MEM_BLK);   /* GAP-FILL(vs): header goes home */
        } else {
            glob = &g_mem.blktab[qid];
            /* MT: global queue mutations take the global queue lock */
            mtlkfmtx(&glob->mtx);
            nbuf = glob->nbuf;
            lim = memnblkbuf(qid);
            if (nbuf > lim) {
                mempurgebufentries(buf, PAGESIZE);
                memfreebuf(buf);           /* GAP-FILL(vs): unmap before the */
                buf->prev = buf;  /* GAP-FILL(vs): kill stale links */
                buf->next = buf;
                memputbuf(buf, MEM_BLK);   /* GAP-FILL(vs): header goes home */
            } else {
                buf->queue = glob;   /* MT: rebind on handoff - frees
                                      * must route to the queue that
                                      * now owns the buffer */
                mempushbuf(glob, buf);
            }
            mtunlkfmtx(&glob->mtx);
        }
    }
    if (!tls) {
        mtunlkfmtx(&queue->mtx);
    }

    return;
}

/* release allocation run */
static struct membuf *
memputrun(struct membuf *buf, void *adr)
{
    struct membufq     *queue = buf->queue;
    unsigned long       qid = queue->qid;
    struct memtls      *tls = queue->tls;
    long                ndx = memrunnum(qid, buf, adr);
    uintptr_t           bits = buf->bits;
    uintptr_t           mask = (uintptr_t)1 << ndx;
    long                nrun = buf->max;
    long                nbuf = queue->nbuf;
    long                lim = 2;
    struct membufq     *glob;

    if (!tls) {
        mtlkfmtx(&queue->mtx);
    }
    /* GAP-FILL(vs): duplicate free of a run is tolerated (bit already
     * set) exactly as upstream did */
    bits |= mask;
    buf->bits = bits;
    /* GAP-FILL(vs): requeue only buffers actually unlinked - marked by
     * self-links where the allocator popped them at exhaustion. A
     * partially-allocated buffer is still on its queue; requeueing it
     * would double-link it. */
    if (bits == mask && buf->next == buf && buf->prev == buf) {
        memqueuebuf(queue, buf);
    } else if (bits != mask) {
        mask = (uintptr_t)1 << nrun;
        mask--;
        if (bits == mask && nbuf > lim) {
            memdequebuf(queue, buf);
            if (!tls) {
                if (UNLIKELY(buf->bits != (((uintptr_t)1 << buf->max) - 1))) {
                }
                if (!mempreleasesafe(buf, PAGESIZE * qid)) {
                    if (!tls) { mtunlkfmtx(&queue->mtx); }
                    return buf;
                }
                mempurgebufentries(buf, PAGESIZE * qid);
                memfreebuf(buf);           /* GAP-FILL(vs): unmap before the */
                buf->prev = buf;  /* GAP-FILL(vs): kill stale links */
                buf->next = buf;
                memputbuf(buf, MEM_RUN);   /* GAP-FILL(vs): header goes home */
            } else {
                glob = &g_mem.runtab[qid];
                /* MT: global queue mutations take the global queue lock */
                mtlkfmtx(&glob->mtx);
                nbuf = glob->nbuf;
                mask = (uintptr_t)1 << nrun;
                lim = memnrunbuf(qid);
                mask--;
                if (bits == mask && nbuf > lim) {
                    mempurgebufentries(buf, PAGESIZE * qid);
                    memfreebuf(buf);       /* GAP-FILL(vs): unmap before the */
                    buf->prev = buf;  /* GAP-FILL(vs): kill stale links */
                    buf->next = buf;
                    memputbuf(buf, MEM_RUN);   /* GAP-FILL(vs): header home */
                } else {
                    buf->queue = glob;   /* MT: rebind on handoff */
                    mempushbuf(glob, buf);
                }
                mtunlkfmtx(&glob->mtx);
            }
        }
    }
    if (!tls) {
        mtunlkfmtx(&queue->mtx);
    }

    return buf;
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
    size_t              cnt;
    uintptr_t           page;
    uintptr_t           val;

    ptr = mapanon(-1, mapsz, 0);
    if (ptr == MAP_FAILED) {
        ptr = NULL;
    } else {
        ret = memalignptr(ptr, align);
        aln = ret - ptr;
        page = memsethashpage(mempageadr(ret), MEM_BIG);
        /* GAP-FILL(vs): aln is 0 for page-aligned mappings; ctz(0) is UB */
        cnt = aln ? m_ctz(aln) : 0;
        val = memsethashval(mapsz, cnt) | (MEM_BIG << MEM_BUF_QUEUE_BITS);
        tabhashadd(g_memhashtab, page, val);
        mapsz -= aln;
        if (retsize) {
            *retsize = mapsz;
        }
    }

    return ret;
}

/* free (unmap) big allocation */
static void
memfreebig(void *ptr, size_t size)
{
    tabhashdel(g_memhashtab, (uintptr_t)ptr);
    unmapanon(ptr, size);

    return;
}

/* allocate size bytes, align to boundary of align bytes */
void *
memget(size_t size, size_t align, size_t *retsize)
{
    void               *ptr = NULL;
    size_t              aln = max(align, MEM_ALIGN_MIN);
    size_t              alnsz = memalnsize(size, aln);
    uintptr_t           type = membuftype(alnsz);
    size_t              blksz = 0;
    unsigned long       qid;

    if (!(g_mem.flg & MEM_INIT_BIT)) {
        meminit(&g_mem);
    }
    switch (type) {
        case MEM_BLK:
            blksz = roundup2(alnsz, MEM_BLK_MIN);
            qid = memblkqid(blksz);
            ptr = memgetblk(qid, aln, &blksz);

            break;
        case MEM_RUN:
            blksz = roundup2(alnsz, PAGESIZE);
            qid = memrunqid(blksz);
            ptr = memgetrun(qid, aln, &blksz);

            break;
        case MEM_BIG:
            blksz = roundup2(alnsz, PAGESIZE);
            ptr = memgetbig(blksz, aln, &blksz);
        default:

            break;
    }
    if ((ptr) && (retsize)) {
        *retsize = blksz;
    }

    return ptr;
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

/* MT: release workers called with the owning queue lock already held
 * (queue -> slot is the global lock order; the reverse never occurs).
 * The block address is decoded from the buffer's delta table directly -
 * no second hash lookup (a re-find between find and queue-lock was the
 * window where another thread could recycle the header). */
static void
memputblk_locked(struct membuf *buf, void *ptr, unsigned long qid)
{
    struct membufq     *queue = buf->queue;
    struct memtls      *tls = queue->tls;
    long                lim = (tls) ? memnblktlsbuf(qid) : memnblkbuf(qid);
    struct membufq     *glob;
    void               *adr;
    long                ndx;
    long                max;
    long                nbuf;
    long                slot;

    slot = buf->max + memblknum(qid, buf, ptr);
    adr = (void *)((uintptr_t)ptr - buf->tab[slot]);
    buf->tab[slot] = 0;           /* delta consumed */
    ndx = buf->ofs - 1;
    max = buf->max - 1;
    nbuf = queue->nbuf;
    buf->tab[ndx] = (uintptr_t)adr;   /* block returns to the free-stack */
    buf->ofs = ndx;
    if (!ndx && nbuf > lim) {
        memdequebuf(queue, buf);
        if (!tls) {
            mempurgebufentries(buf, PAGESIZE);
            memfreebuf(buf);
            buf->prev = buf;
            buf->next = buf;
            memputbuf(buf, MEM_BLK);
        } else {
            glob = &g_mem.blktab[qid];
            mtlkfmtx(&glob->mtx);
            if (glob->nbuf > memnblkbuf(qid)) {
                mempurgebufentries(buf, PAGESIZE);
                memfreebuf(buf);
                buf->prev = buf;
                buf->next = buf;
                memputbuf(buf, MEM_BLK);
            } else {
                buf->queue = glob;   /* MT: rebind on handoff - frees
                                      * must route to the queue that
                                      * now owns the buffer */
                mempushbuf(glob, buf);
            }
            mtunlkfmtx(&glob->mtx);
        }
    }

    return;
}

static void
memputrun_locked(struct membuf *buf, void *ptr, unsigned long qid)
{
    struct membufq     *queue = buf->queue;
    struct memtls      *tls = queue->tls;
    long                nrun = buf->max;
    long                nbuf = queue->nbuf;
    long                lim = 2;
    long                ndx = memrunnum(qid, buf, ptr);
    uintptr_t           bits = buf->bits;
    uintptr_t           mask = (uintptr_t)1 << ndx;
    struct membufq     *glob;

    bits |= mask;
    buf->bits = bits;
    mask = ((uintptr_t)1 << nrun) - 1;
    if (bits == mask && nbuf > lim) {
        memdequebuf(queue, buf);
        if (!tls) {
            mempurgebufentries(buf, PAGESIZE * qid);
            memfreebuf(buf);
            buf->prev = buf;
            buf->next = buf;
            memputbuf(buf, MEM_RUN);
        } else {
            glob = &g_mem.runtab[qid];
            mtlkfmtx(&glob->mtx);
            if (glob->nbuf > memnrunbuf(qid)) {
                mempurgebufentries(buf, PAGESIZE * qid);
                memfreebuf(buf);
                buf->prev = buf;
                buf->next = buf;
                memputbuf(buf, MEM_RUN);
            } else {
                buf->queue = glob;   /* MT: rebind on handoff - frees
                                      * must route to the queue that
                                      * now owns the buffer */
                mempushbuf(glob, buf);
            }
            mtunlkfmtx(&glob->mtx);
        }
    }

    return;
}

/* GAP-FILL(vs): entries are fungible per base page, so a page can carry
 * entries of several eras (and types) at once. A stale BIG entry sharing
 * a page with live BLK/RUN entries used to win the lookup and mass-unmap
 * everything above it. Release is now type-aware and validating: pull
 * entries for the page one at a time, drop stale ones, and honour a BIG
 * unmap only once the page provably holds nothing else. */
static struct bigunmap { uintptr_t page; size_t size; } g_bigu[256];
static unsigned g_bigui;
struct bigunmap *g_bigu_pub = (struct bigunmap *)g_bigu;
unsigned *g_bigui_pub = &g_bigui;
void
memput(void *ptr)
{
    uintptr_t           page = mempageadr(ptr);

    if (!(g_mem.flg & MEM_INIT_BIT)) {
        abort();
    }
    {
        struct memhash hash = tabhashfind(g_memhashtab, page);
        long           type;
        struct membuf  *buf;
        struct membufq *queue;

        if (!hash.page) {

            return;
        }
        type = memhashtype(&hash);
        buf = (struct membuf *)memhashbuf(&hash);

        if (type == MEM_BIG) {
            size_t size = (size_t)(hash.val & ~(uintptr_t)0xFFF);

            if (size >= MEM_RUN_MAX && !memrangebusy(page, size)) {
                unmapanon((void *)page, size);
            }
            tabhashrm(g_memhashtab, page);

            return;
        }
        if ((uintptr_t)buf < (uintptr_t)0x10000
            || !buf->adr || !buf->queue
            || (uintptr_t)ptr < (uintptr_t)buf->adr
            || (uintptr_t)ptr
               >= (uintptr_t)buf->adr + buf->size) {
            /* raced or stale: NEVER remove the entry on a failed
             * validation - it belongs to whichever free validates it */
            return;
        }
        queue = buf->queue;
        if (!queue->tls) {
            mtlkfmtx(&queue->mtx);
        }
        if (UNLIKELY(!buf->adr || buf->queue != queue
                     || (uintptr_t)ptr < (uintptr_t)buf->adr
                     || (uintptr_t)ptr
                        >= (uintptr_t)buf->adr + buf->size)) {
            if (!queue->tls) {
                mtunlkfmtx(&queue->mtx);
            }

            return;
        }
        if (type == MEM_BLK) {
            memputblk_locked(buf, ptr, queue->qid);
        } else {
            memputrun_locked(buf, ptr, queue->qid);
        }
        tabhashrm(g_memhashtab, page);   /* slot under queue: lock order */
        if (!queue->tls) {
            mtunlkfmtx(&queue->mtx);
        }
    }
}


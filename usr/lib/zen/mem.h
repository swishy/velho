#ifndef __ZEN_MEM_H__
#define __ZEN_MEM_H__

#include <limits.h>
#include <stdint.h>
#include <stddef.h>
#include <mt/lk.h>
#include <mach/param.h>
#include <mach/types.h>
#include <mt/mtx.h>
#include <zero/trix.h>

#define MEM_LK_BIT_OFS                  0
#define MEM_LK_BIT                      ((m_atomic_t)1L << MEM_LK_BIT_OFS)

#define MEM_ALIGN_MIN                   16

#define memalignptr(ptr, aln)   ((void *)roundup2((uintptr_t)ptr, aln))

#define MEM_BLK                 0
#define MEM_RUN                 1
#define MEM_BLK_UNIT            MEM_ALIGN_MIN
#define MEM_BLK_MIN     	MEM_BLK_UNIT
#define MEM_BLK_MAX     	PAGESIZE
#define MEM_BLK_QUEUES          (MEM_BLK_MAX / MEM_BLK_UNIT)
#define MEM_BLK_MID_QUEUE_MIN   (MEM_BLK_QUEUES >> 2)
#define MEM_BLK_BIG_QUEUE_MIN   (MEM_BLK_QUEUES >> 1)
/* Queue lock covers summaries and leaf transitions, not interior bit changes.
 * One non-slot bit in leaf zero carries its head lease. */
#define MEM_BUF_MAP_BITS        (CHAR_BIT * sizeof(uintptr_t))
#define MEM_BUF_MAP_WORDS       (4 * MEM_BUF_MAP_BITS)
#define MEM_BUF_SUM_WORDS       4
#define MEM_BUF_HEAD_BIT        ((uintptr_t)1 << (MEM_BUF_MAP_BITS - 1))
#define MEM_BUF_FLAGS           MEM_BUF_HEAD_BIT
#define MEM_SLAB_SIZE           (4 * MEM_BLK_UNIT * MEM_BUF_MAP_BITS * MEM_BUF_MAP_BITS)
/* Subtract one: an over-aligned BIG pointer can lie at the next boundary. */
#define memptrbuf(ptr)                                                  \
    ((struct membuf *)(((uintptr_t)(ptr) - 1) & ~(uintptr_t)(MEM_SLAB_SIZE - 1)))
#define MEM_RUN_QUEUES          (MEM_SLAB_SIZE / PAGESIZE)
#define MEM_RUN_MID_QUEUE_MIN   (MEM_RUN_QUEUES >> 2)
#define MEM_RUN_BIG_QUEUE_MIN   (MEM_RUN_QUEUES >> 1)
#define memblkqid(sz)           (((sz) + MEM_BLK_MIN - 1) / MEM_BLK_UNIT)
#define memblksize(qid)         (MEM_BLK_UNIT * (qid))
#define memrunqid(sz)           (((sz) + PAGESIZE - 1) >> PAGESIZELOG2)
#define memrunsize(qid)         (PAGESIZE * (qid))
#define memnblktlsbuf(qid)                                              \
    (((qid) <= MEM_BLK_MID_QUEUE_MIN)                                   \
     ? 4                                                                \
     : (((qid) <= MEM_BLK_BIG_QUEUE_MIN)                                \
        ? 2                                                             \
        : 1))
#define memnblkbuf(qid)                                                 \
    (((qid) <= MEM_BLK_MID_QUEUE_MIN)                                   \
     ? 8                                                                \
     : (((qid) <= MEM_BLK_BIG_QUEUE_MIN)                                \
        ? 4                                                             \
        : 2))
#define memnruntlsbuf(qid)                                              \
    (((qid) <= MEM_RUN_MID_QUEUE_MIN)                                   \
     ? 2                                                                \
     : (((qid) <= MEM_RUN_BIG_QUEUE_MIN)                                \
        ? 1                                                             \
        : 0))
#define memnrunbuf(qid)                                                 \
    (((qid) <= MEM_RUN_MID_QUEUE_MIN)                                   \
     ? 4                                                                \
     : (((qid) <= MEM_RUN_BIG_QUEUE_MIN)                                \
        ? 2                                                             \
        : 1))

/* Header is embedded at the aligned mapping base. */
#define membufsize()            roundup2(sizeof(struct membuf), CLSIZE)

struct membuf {
    int8_t             *adr;    /* first allocation */
    size_t              size;   /* mapped slab size */
    unsigned short      max;    /* physical slot index limit */
    unsigned short      busy;   /* non-full leaves excluding zero; queue lock */
    uintptr_t           lastmask; /* immutable valid bits in final leaf */
    uintptr_t           local;  /* owner-only leaf-zero bits; leased head only */
    uintptr_t           bits;   /* summary of nonempty bitmap words */
    uintptr_t           wordmap[MEM_BUF_SUM_WORDS];
    uintptr_t           map[MEM_BUF_MAP_WORDS];
    struct membufq     *queue;  /* owner, including while full */
    struct membuf      *prev;
    struct membuf      *next;
};

/* Available buffers only; nbuf includes full buffers owned by this queue.
 * The low bit of head locks queue/lifetime and leaf zero/full transitions.
 * A retained queue's head is leased until its allocator removes it. */
struct membufq {
    size_t              size;   /* allocation stride identifies the class */
    long                nbuf;
    struct membuf      *head;
    struct membuf      *tail;
    int                 retired; /* owner exited; no local reserve */
} ALIGNED(CLSIZE);

struct memtls {
    struct membufq      blktab[MEM_BLK_QUEUES];
    struct membufq      runtab[MEM_RUN_QUEUES];
};

/* engine-internal */
/* The interposer is loaded at process startup (preload or direct link). */
#if defined(__GNUC__)
extern THREADLOCAL struct memtls *t_memtls
    __attribute__((tls_model("initial-exec")));
#else
extern THREADLOCAL struct memtls *t_memtls;
#endif

/* flg-member */
#define MEM_INIT_BIT            ((m_atomic_t)(1 << 0))
struct memglob {
    /* allocation queue tables */
    struct membufq              blktab[MEM_BLK_QUEUES];
    struct membufq              runtab[MEM_RUN_QUEUES];
    mtmtx                       mtx;
    m_atomic_t                  flg;
};

void    meminit(struct memglob *mem);
/* Engine entry: shim_init has initialized globals and this thread's TLS. */
void   *memget(size_t size, size_t align, size_t *retsize);
void    memput(void *ptr);
void    memretire(struct memtls *tls);

_Static_assert(MEM_ALIGN_MIN >= _Alignof(max_align_t),
               "ordinary malloc must satisfy fundamental alignment");
_Static_assert(sizeof(struct membuf) <= membufsize()
               && membufsize() % _Alignof(struct membuf) == 0
               && membufsize() < PAGESIZE,
               "the embedded header must fit before the first RUN page");
_Static_assert(sizeof(struct membufq) == CLSIZE,
               "each queue must occupy one cache line");
_Static_assert(sizeof(m_atomic_t) == sizeof(uintptr_t),
               "pointer bit locks require a pointer-sized atomic word");
_Static_assert((MEM_SLAB_SIZE & (MEM_SLAB_SIZE - 1)) == 0
               && MEM_SLAB_SIZE % PAGESIZE == 0
               && MEM_BUF_SUM_WORDS <= MEM_BUF_MAP_BITS,
               "aligned slabs and bitmap summaries must cover every slot");
_Static_assert(MEM_SLAB_SIZE / MEM_BLK_UNIT <= USHRT_MAX,
               "physical slot indices must fit in the header");

extern struct memglob g_mem ALIGNED(PAGESIZE);

static __inline__ size_t
memalnsize(size_t size, size_t align)
{
    if (align <= MEM_BLK_UNIT || (size >= MEM_BLK_MAX && align <= PAGESIZE)) {
        return size;
    }
    /* Reserve only the displacement beyond ordinary block alignment,
     * keeping an explicitly aligned pointer inside its original slot. */
    return size > SIZE_MAX - (align - MEM_BLK_UNIT)
        ? SIZE_MAX : size + align - MEM_BLK_UNIT;
}

#endif /* __ZEN_MEM_H__ */

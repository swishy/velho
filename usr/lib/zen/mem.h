#ifndef __ZEN_MEM_H__
#define __ZEN_MEM_H__

#include <limits.h>
#include <stdint.h>
#include <mach/param.h>
#include <mach/types.h>
#include <mt/mtx.h>
#include <stdio.h>
#include <unistd.h>
#include <zero/trix.h>

#define MEM_LK_BIT_OFS                  0
#define MEM_LK_BIT                      ((m_atomic_t)1L << MEM_LK_BIT_OFS)

#define memfreebuf(buf)                 unmapanon(buf->adr, buf->size)

#define memhashpage(hash)               ((hash)->page & MEM_HASH_PAGE_MASK)
#define memhashtype(hash)               ((((hash)->val) >> MEM_BUF_QUEUE_BITS) \
                                         & MEM_TYPE_MASK)
#define memhashbuf(hash)                ((void *)((hash)->val           \
                                                  & MEM_BUF_ADR_MASK))
#define memhashqid(hash)                ((hash)->val & MEM_BUF_QUEUE_MASK)
/* Header alignment reserves low bits for the size class and type. */
#define memsethashbuf(buf, qid, type)   ((uintptr_t)(buf) | (qid) \
                                         | ((type) << MEM_BUF_QUEUE_BITS))

#define MEM_ALIGN_MIN                   CLSIZE

#define mempageadr(adr)         ((uintptr_t)(adr) & ~(PAGESIZE - 1))
#define memalignptr(ptr, aln)   ((void *)roundup2((uintptr_t)ptr, aln))

#define MEM_BLK                 0
#define MEM_RUN                 1
#define MEM_BIG                 2
#define MEM_TYPE_BITS           2
#define MEM_TYPE_MASK           ((1L << MEM_TYPE_BITS) - 1)
#define MEM_BLK_BIG_SLAB        (16L * PAGESIZE)
#define MEM_BLK_MID_SLAB        (4L * PAGESIZE)
#define MEM_BLK_MIN_SLAB        PAGESIZE
#define MEM_BLK_UNIT            CLSIZE
#define MEM_BLK_MIN     	MEM_BLK_UNIT
#define MEM_BLK_MAX     	PAGESIZE
#define MEM_BLK_QUEUES          (MEM_BLK_MAX / MEM_BLK_UNIT)
#define MEM_BLK_MID_QUEUE_MIN   (MEM_BLK_QUEUES >> 2)
#define MEM_BLK_BIG_QUEUE_MIN   (MEM_BLK_QUEUES >> 1)
#define MEM_RUN_BIG_SLAB        (2L * MEM_RUN_QUEUES * PAGESIZE)
#define MEM_RUN_MID_SLAB        (MEM_RUN_QUEUES * PAGESIZE)
#define MEM_RUN_MIN_SLAB        (MEM_RUN_QUEUES * PAGESIZE >> 1)
#define MEM_RUN_UNIT            PAGESIZE
#define MEM_RUN_MIN             MEM_RUN_UNIT
#define MEM_RUN_MAX             (MEM_RUN_QUEUES * MEM_RUN_UNIT)
/* Both block and run slabs fit a single bitmap word. */
#define MEM_BUF_MAP_BITS        (CHAR_BIT * sizeof(uintptr_t))
#define MEM_RUN_QUEUES          MEM_BUF_MAP_BITS
#define MEM_RUN_MID_QUEUE_MIN   (MEM_RUN_QUEUES >> 2)
#define MEM_RUN_BIG_QUEUE_MIN   (MEM_RUN_QUEUES >> 1)
#define memblkqid(sz)           (((sz) + MEM_BLK_MIN - 1) >> CLSIZELOG2)
#define memblksize(qid)         (CLSIZE * (qid))
#define memrunqid(sz)           (((sz) + PAGESIZE - 1) >> PAGESIZELOG2)
#define memrunsize(qid)         (PAGESIZE * (qid))
#define memnblk(qid)            (g_mem.nblktab[(qid)])
#define memnrun(qid)            (g_mem.nruntab[(qid)])
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

/* Header stride reserves the class/type tag bits; payloads are separate. */
#define membufsize()            ((size_t)1 << (MEM_BUF_QUEUE_BITS + MEM_TYPE_BITS))

struct membuf {
    int8_t             *adr;    /* first allocation */
    size_t              size;   /* mapped slab size */
    size_t              max;    /* number of allocation slots */
    uintptr_t           bits;   /* one bit per slot; 1 = free */
    struct membufq     *queue;  /* owner, including while full */
    struct membuf      *prev;
    struct membuf      *next;
};

/* Available buffers only; nbuf includes full buffers owned by this queue.
 * The low bit of head locks the queue and every owned buffer's bitmap. */
struct membufq {
    unsigned long       qid;
    long                nbuf;
    struct membuf      *head;
    struct membuf      *tail;
} ALIGNED(CLSIZE);

struct memtls {
    struct membufq      blktab[MEM_BLK_QUEUES];
    struct membufq      runtab[MEM_RUN_QUEUES];
};

/* engine-internal */
extern THREADLOCAL struct memtls *t_memtls;

/* flg-member */
#define MEM_INIT_BIT            ((m_atomic_t)(1 << 0))
struct memglob {
    /* allocation queue tables */
    struct membufq              blktab[MEM_BLK_QUEUES];
    long                        nblktab[MEM_BLK_QUEUES];
    struct membufq              runtab[MEM_RUN_QUEUES];
    long                        nruntab[MEM_RUN_QUEUES];
    volatile struct membuf     *bufhdrq;
    mtmtx                       mtx;
    m_atomic_t                  flg;
};

void    meminit(struct memglob *mem);
void   *memget(size_t size, size_t align, size_t *retsize);
void    memput(void *ptr);

/* Two-word page metadata:
 *   page = allocation's starting page | active reference count (low bits).
 *   val  = BLK/RUN header pointer | qid (low 6 bits) | type (next 2 bits),
 *          or BIG mapping length | type. Header pointers and mapping
 *          lengths leave the tag bits clear (headers: 256-byte alignment).
 * The hash key is always the untagged page address. BIG allocations trim
 * any alignment prefix, so this page is also the address to unmap.
 */
/* page-member */
#define MEM_HASH_NREF_BITS              (PAGESIZELOG2 - MEM_TYPE_BITS)
/* GAP-FILL(vs): 1U makes ~mask ZERO-extend, so nref clear-then-set
 * truncated the page word's high 32 bits (entries silently unfindable
 * after the first reference count write) */
#define MEM_HASH_NREF_MASK              (((uintptr_t)1 << MEM_HASH_NREF_BITS) - 1)
#define MEM_HASH_PAGE_MASK              (~(PAGESIZE - 1))
/* val-member */
#define MEM_BUF_ADR_MASK                (~(((uintptr_t)1 << (MEM_BUF_QUEUE_BITS + MEM_TYPE_BITS)) - 1))
#define MEM_BUF_QUEUE_MASK              (((uintptr_t)1 << MEM_BUF_QUEUE_BITS) \
                                         - 1)
#define MEM_BUF_QUEUE_BITS              6
#define MEM_CACHE_BUFS                  32

_Static_assert(MEM_BLK_QUEUES <= (1UL << MEM_BUF_QUEUE_BITS)
               && MEM_RUN_QUEUES <= (1UL << MEM_BUF_QUEUE_BITS),
               "size classes must fit the packed header tag");
_Static_assert(sizeof(struct membuf) <= membufsize()
               && membufsize() % _Alignof(struct membuf) == 0
               && PAGESIZE % membufsize() == 0,
               "mapped header strides must preserve size and alignment");
_Static_assert(sizeof(struct membufq) == CLSIZE,
               "each queue must occupy one cache line");
_Static_assert(sizeof(m_atomic_t) == sizeof(uintptr_t),
               "pointer bit locks require a pointer-sized atomic word");

struct memhash {
    uintptr_t   page;           // block page address + reference count
    uintptr_t   val;            // queue ID + allocation buffer address + flags
};

#define TABHASH_SLOTS           16384
#define TABHASH_INVALID         { 0, 0 }
#define TABHASH_BUF             g_memhashbuf
#define TABHASH_TAB_T           struct tabhashtab
#define TABHASH_ITEM_T          struct memhash
#define TABHASH_ITEM_WORDS      2
#define TABHASH_HDR_WORDS       4
#define TABHASH_TAB_ITEMS       ((64 - TABHASH_HDR_WORDS) / TABHASH_ITEM_WORDS)
#define TABHASH_KEY(item)       ((item)->page)
#if (WORDSIZE == 8)
/* GAP-FILL(vs): slot-mask the 64-bit hash or hashtab[] is indexed
 * out of bounds; upstream vnd config never landed this */
#define TABHASH_HASH(key)       (tmhash64(key) & (TABHASH_SLOTS - 1))
#define TABHASH_HASH_ITEM(item) TABHASH_HASH((item)->page)
#else
#define TABHASH_HASH(key)       (tmhash32(key) & (TABHASH_SLOTS - 1))
#define TABHASH_HASH_ITEM(item) TABHASH_HASH((item)->page)
#endif
/* GAP-FILL(vs): THE original sin - the comparator returned TRUE on match
 * while every scan acts on !CMP (nonzero = mismatch, memcmp-style); with
 * == the scans skipped real matches and processed the first wrong item,
 * returning garbage and mangling tables wholesale. */
#define TABHASH_CMP(item, key)  (memhashpage(item) != (key))
#define TABHASH_COPY(src, dest) (*(dest) = *(src))
#define TABHASH_GET_NREF(item)  ((item)->page & MEM_HASH_NREF_MASK)
#define TABHASH_PUT_NREF(item, n)                                       \
    ((item)->page |= (n))
#define TABHASH_CHK(item)       ((item)->page && (item)->val)
#include <zen/tabhash.h>

extern struct tabhashtab        *g_memhashtab[TABHASH_SLOTS];
extern struct memglob            g_mem ALIGNED(PAGESIZE);

static __inline__ size_t
memalnsize(size_t size, size_t align)
{
    if (align <= CLSIZE || (size >= MEM_BLK_MAX && align <= PAGESIZE)) {
        return size;
    }
    /* Slab blocks already have cache-line alignment. Reserve only the
     * maximum additional displacement, keeping the pointer in its block. */
    return size > SIZE_MAX - (align - CLSIZE)
        ? SIZE_MAX : size + align - CLSIZE;
}

#endif /* __ZEN_MEM_H__ */

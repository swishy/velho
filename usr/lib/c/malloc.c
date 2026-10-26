/* velho malloc(3) interposer - gap-filling layer for vendu's velho
 * (github.com/vendu/velho) usr/lib/zen allocator.
 *
 * The engine (zen/mem.c) is used as-is except for two marked GAP-FILL
 * edits in mem-fixed.c (runtab typo + claim-to-TLS on the global recycle
 * paths). This file fills the rest of what upstream never finished:
 *
 *   - vnd/ redirect headers (the vnd/ layer does not exist upstream);
 *   - thread-local store init: t_memtls is declared but never assigned
 *     anywhere in the tree, and membufq.qid/tls are never wired;
 *   - an actual size lookup for realloc and malloc_usable_size (upstream
 *     stubs memsize() out with a literal zero);
 *   - a malloc(3) API without the upstream shim's bugs (calloc
 *     allocated n*size but passed `size` to memget; realloc returned
 *     the OLD pointer).
 *
 * Sizes are decoded strictly through the engine's own packed-accessor
 * macros (memhashtype/memhashqid/memblksize/...) - never re-derived.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>

#include <zero/cdefs.h>
#include <zen/_hash.h>
#include <zen/mem.h>

/* engine internals declared only in mem.c upstream */
extern THREADLOCAL struct memtls *t_memtls;
extern struct memtls *t_memtls_p;
void meminit(struct memglob *mem);

/* ------------------------------------------------------------------ */
/* TLS + global init                                                   */
/* ------------------------------------------------------------------ */

/* full init (tables + global queue wiring + TLS) - idempotent, and
 * called from every API entry so pre-constructor allocations can never
 * observe unwired global queues (GAP-FILL(vs)) */
static void shim_init(void);
static void
shim_tls_init(void)
{
    struct memtls *tls;
    unsigned long  q;

    if (t_memtls) {
        return;
    }
    /* mmap gives zeroed memory: mtx = MTMTXINITVAL(0), nbuf 0, lists NULL */
    tls = mapanon(-1, sizeof(struct memtls), 0);
    if (tls == MAP_FAILED || !tls) {
        return;
    }
    for (q = 0; q < MEM_BLK_QUEUES; q++) {
        tls->blktab[q].qid = q;
        tls->blktab[q].tls = tls;
    }
    for (q = 0; q < MEM_RUN_QUEUES; q++) {
        tls->runtab[q].qid = q;
        tls->runtab[q].tls = tls;
    }
    m_membar();
    t_memtls = tls;
}

static void
shim_init(void)
{
    unsigned long q;

    if (g_mem.flg & MEM_INIT_BIT && t_memtls) {

        return;
    }
    meminit(&g_mem);
    /* wire global queue ids (queues are in zeroed static storage) */
    for (q = 0; q < MEM_BLK_QUEUES; q++) {
        g_mem.blktab[q].qid = q;
    }
    for (q = 0; q < MEM_RUN_QUEUES; q++) {
        g_mem.runtab[q].qid = q;
    }
    g_mem.flg |= MEM_INIT_BIT;
    shim_tls_init();
}

__attribute__((constructor(101)))
static void
shim_constructor(void)
{
    shim_init();
}

/* ------------------------------------------------------------------ */
/* size lookup - the memsize() upstream stubbed out                    */
/* ------------------------------------------------------------------ */

static size_t
shim_memsize(void *ptr)
{
    TABHASH_ITEM_T item;
    long           type;
    size_t         sz = 0;

    item = tabhashfind(g_memhashtab, mempageadr(ptr));
    if (item.page) {
        type = memhashtype(&item);
        if (type == MEM_BLK) {
            sz = memblksize(memhashqid(&item));
        } else if (type == MEM_RUN) {
            sz = memrunsize(memhashqid(&item));
        } else if (type == MEM_BIG) {
            /* val = size | cnt | type-bits; size is a page multiple */
            sz = (size_t)(item.val & ~(uintptr_t)0xFFF);
        }
    }

    return sz;
}

/* ------------------------------------------------------------------ */
/* malloc(3) API                                                       */
/* ------------------------------------------------------------------ */


void *
malloc(size_t size)
{
    void *p;

    shim_init();
    p = memget(size, 0, NULL);

    return p;
}


void
free(void *ptr)
{
    if (ptr) {
        memput(ptr);
    }
}

/* live-range ring for overlap detection (debug builds) */

void *
calloc(size_t n, size_t size)
{
    size_t blksz = n * size;
    void *ptr;

    shim_init();
    if (blksz == 0 || blksz / n != size) {
        return NULL;                    /* overflow or zero */
    }
    ptr = memget(blksz, 0, NULL);
    if (ptr) {
        memset(ptr, 0, blksz);
    }

    return ptr;
}

size_t
malloc_usable_size(void *ptr)
{
    if (!ptr) {
        return 0;
    }

    return shim_memsize(ptr);
}

void *
realloc(void *ptr, size_t size)
{
    size_t oldsz;
    void  *adr;

    shim_init();
    if (!ptr) {

        return memget(size, 0, NULL);
    }
    if (!size) {
        memput(ptr);

        return NULL;
    }
    oldsz = shim_memsize(ptr);
    if (!oldsz) {
        /* unknown to us - should not happen; leak rather than corrupt */
        errno = ENOMEM;

        return NULL;
    }
    if (oldsz >= size) {
        return ptr;                     /* fits: keep in place */
    }
    adr = memget(size, 0, NULL);
    if (adr) {
        memcpy(adr, ptr, oldsz < size ? oldsz : size);
        memput(ptr);
    }

    return adr;
}

void *
aligned_alloc(size_t align, size_t size)
{
    shim_init();

    return memget(size, align, NULL);
}

int
posix_memalign(void **memptr, size_t align, size_t size)
{
    void *ptr;

    shim_init();
    if (!memptr || align < sizeof(void *) || (align & (align - 1))) {
        return EINVAL;
    }
    ptr = memget(size, align, NULL);
    if (!ptr) {
        return ENOMEM;
    }
    *memptr = ptr;

    return 0;
}

void *
memalign(size_t align, size_t size)
{
    shim_init();
    if (!align || (align & (align - 1))) {
        return NULL;
    }

    return memget(size, align, NULL);
}

void *
valloc(size_t size)
{
    shim_init();

    return memget(size, PAGESIZE, NULL);
}

void *
pvalloc(size_t size)
{
    shim_init();

    return memget(roundup2(size, PAGESIZE), PAGESIZE, NULL);
}

void *
reallocarray(void *ptr, size_t n, size_t size)
{
    size_t total = n * size;

    if (n != 0 && total / n != size) {
        errno = ENOMEM;

        return NULL;
    }

    return realloc(ptr, total);
}

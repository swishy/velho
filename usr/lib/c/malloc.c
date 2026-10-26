/* malloc(3) interposer for vendu's zen allocator.
 * The engine owns slabs, size-class queues and packed page metadata;
 * this shim supplies process/TLS initialization and libc entry points.
 * Usable capacity is decoded from the same metadata as the free path.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
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

/* thread exit: mark this TLS domain dead - remote frees onto a full
 * ring may then apply directly instead of waiting for a drain that will
 * never come */
static void
thread_dtor(void *p)
{
    struct memtls *tls = p;

    if (tls) {
        __atomic_store_n(&tls->alive, 0, __ATOMIC_RELEASE);
    }
}

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
    /* MT: publish once via CAS - a racing loser keeps the winner's TLS
     * and leaks its own (unused) copy rather than swapping a live one */
    {
        struct memtls *expect = NULL;
        if (!__atomic_compare_exchange_n(&t_memtls, &expect, tls, 0,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            unmapanon(tls, sizeof(struct memtls));

            return;
        }
        __atomic_store_n(&tls->alive, 1, __ATOMIC_RELEASE);
        { static pthread_key_t dk;
          if (!dk) {
              if (pthread_key_create(&dk, thread_dtor)) {
                  /* no TSS: treat as always-alive (rings drain at alloc) */
              }
          }
          if (dk) {
              pthread_setspecific(dk, tls);
          }
        }
    }
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
        if (type == MEM_BLK || type == MEM_RUN) {
            struct membuf *buf = memhashbuf(&item);
            sz -= ((uintptr_t)ptr - (uintptr_t)buf->adr) % sz;
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

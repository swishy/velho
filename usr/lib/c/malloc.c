/* malloc(3) interposer for vendu's zen allocator.
 * The engine owns aligned slabs, size-class queues and allocation bitmaps;
 * this shim supplies process/TLS initialization and libc entry points.
 * Usable capacity is decoded from the same metadata as the free path.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <pthread.h>

#include <zero/cdefs.h>
#include <zen/sys/unix.h>
#include <zen/mem.h>

/* ------------------------------------------------------------------ */
/* TLS + global init                                                   */
/* ------------------------------------------------------------------ */

static pthread_key_t shim_tls_key;
static pthread_once_t shim_tls_once = PTHREAD_ONCE_INIT;
static int shim_tls_key_ready;

static void
shim_tls_exit(void *ptr)
{
    t_memtls = NULL;
    memretire(ptr);
}

static void
shim_tls_key_init(void)
{
    shim_tls_key_ready = pthread_key_create(&shim_tls_key, shim_tls_exit) == 0;
}

static void
shim_tls_init(void)
{
    struct memtls *tls;
    unsigned long  q;

    /* mmap gives empty queues with the head lock bit clear. */
    tls = mapanon(-1, sizeof(struct memtls), 0);
    if (tls == MAP_FAILED || !tls) {
        return;
    }
    for (q = 0; q < MEM_BLK_QUEUES; q++) {
        tls->blktab[q].size = memblksize(q);
    }
    for (q = 0; q < MEM_RUN_QUEUES; q++) {
        tls->runtab[q].size = memrunsize(q);
    }
    t_memtls = tls;
    /* Publish TLS first: pthread key setup may itself allocate. Failure to
     * register only forfeits exit reclamation; it cannot corrupt another key. */
    pthread_once(&shim_tls_once, shim_tls_key_init);
    if (shim_tls_key_ready) {
        pthread_setspecific(shim_tls_key, tls);
    }
}

static void
shim_init(void)
{
    /* Published TLS proves global initialization has already completed.
     * This also handles allocations before the constructor runs. */
    if (!t_memtls) {
        if (!(__atomic_load_n(&g_mem.flg, __ATOMIC_ACQUIRE) & MEM_INIT_BIT)) {
            meminit(&g_mem);
        }
        shim_tls_init();
    }
}

__attribute__((constructor(101)))
static void
shim_constructor(void)
{
    shim_init();
}

/* ------------------------------------------------------------------ */
/* Usable capacity from the same slab geometry as free.                 */
/* ------------------------------------------------------------------ */

static size_t
shim_memsize(void *ptr)
{
    struct membuf *buf = memptrbuf(ptr);
    if (!buf->queue) {
        return buf->size - ((uintptr_t)ptr - (uintptr_t)buf);
    }
    size_t size = buf->queue->size;
    return size - ((uintptr_t)ptr - (uintptr_t)buf->adr) % size;
}

/* ------------------------------------------------------------------ */
/* malloc(3) API                                                       */
/* ------------------------------------------------------------------ */

void *
malloc(size_t size)
{
    shim_init();
    return memget(size, 0, NULL);
}

void
free(void *ptr)
{
    if (ptr) {
        memput(ptr);
    }
}

void *
calloc(size_t n, size_t size)
{
    size_t blksz = n * size;
    void *ptr;

    if (blksz == 0 || blksz / n != size) {
        return NULL;                    /* overflow or zero */
    }
    shim_init();
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

    if (!ptr) {
        shim_init();
        return memget(size, 0, NULL);
    }
    if (!size) {
        memput(ptr);

        return NULL;
    }
    oldsz = shim_memsize(ptr);
    if (oldsz >= size) {
        return ptr;                     /* fits: keep in place */
    }
    shim_init();
    adr = memget(size, 0, NULL);
    if (adr) {
        memcpy(adr, ptr, oldsz);
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

    if (align < sizeof(void *) || (align & (align - 1))) {
        return EINVAL;
    }
    shim_init();
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
    if (!align || (align & (align - 1))) {
        return NULL;
    }
    shim_init();

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

/* Keep hot payloads live while a peer releases a slab's cold allocations.
 * The owner concurrently reuses its hot allocations. Public malloc API only. */
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *blocks[8192];
static size_t count, size;
static atomic_int done;
static pthread_barrier_t start;

static unsigned char fill(size_t i) { return (unsigned char)(i % 251 + 1); }
static void check(const unsigned char *p, size_t n, unsigned char value)
{
    for (size_t i = 0; i < n; ++i)
        if (p[i] != value) abort();
}
static void *release(void *unused)
{
    (void)unused;
    pthread_barrier_wait(&start);
    /* Reverse order makes the cold tail disappear before the early slots. */
    for (size_t i = count; i--;) {
        check(blocks[i], size, fill(i));
        free(blocks[i]);
    }
    atomic_store_explicit(&done, 1, memory_order_release);
    return NULL;
}

int main(void)
{
    const size_t sizes[] = {16, 32, 48, 80, 256, 1024, 2048, 4080, 4096, 8192};
    for (size_t s = 0; s < sizeof sizes / sizeof *sizes; ++s) {
        size = sizes[s];
        count = 98304 / size;
        for (unsigned round = 0; round < 8; ++round) {
            unsigned char *anchor = malloc(size), *reuse = malloc(size);
            if (!anchor || !reuse) abort();
            memset(anchor, 0xd3, size);
            for (size_t i = 0; i < count; ++i) {
                blocks[i] = malloc(size);
                if (!blocks[i]) abort();
                memset(blocks[i], fill(i), size);
            }
            free(reuse);
            atomic_store_explicit(&done, 0, memory_order_relaxed);
            if (pthread_barrier_init(&start, NULL, 2)) abort();
            pthread_t peer;
            if (pthread_create(&peer, NULL, release, NULL)) abort();
            pthread_barrier_wait(&start);
            for (unsigned i = 0; i < 2000
                 || !atomic_load_explicit(&done, memory_order_acquire); ++i) {
                unsigned char *p = malloc(size);
                if (!p) abort();
                memset(p, 0xa7, size);
                check(anchor, size, 0xd3);
                check(p, size, 0xa7);
                free(p);
            }
            if (pthread_join(peer, NULL)) abort();
            check(anchor, size, 0xd3);
            free(anchor);
            pthread_barrier_destroy(&start);
        }
    }
    puts("hot reuse with concurrent cold release: PASS");
    return 0;
}

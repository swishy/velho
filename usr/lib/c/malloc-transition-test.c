/* Allocate before worker creation, then free across threads while the owner
 * keeps allocating in the same classes. Verify every handed-off payload. */
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 65536
#define WORKERS 4
static unsigned char *blocks[N];
static pthread_barrier_t start;

static size_t blocksize(size_t i) { return 1 + (i * 37) % 2048; }
static void fail(void) { abort(); }
static void *worker(void *arg)
{
    size_t id = (uintptr_t)arg;
    pthread_barrier_wait(&start);
    for (size_t i = id; i < N; i += WORKERS) {
        size_t n = blocksize(i);
        unsigned char fill = (unsigned char)(i % 251 + 1);
        for (size_t j = 0; j < n; ++j)
            if (blocks[i][j] != fill) fail();
        free(blocks[i]);
        blocks[i] = NULL;
        unsigned char *p = malloc(n);
        if (!p) fail();
        memset(p, fill, n);
        if (p[n - 1] != fill) fail();
        free(p);
    }
    return NULL;
}
int main(void)
{
    for (size_t i = 0; i < N; ++i) {
        size_t n = blocksize(i);
        blocks[i] = malloc(n);
        if (!blocks[i]) fail();
        memset(blocks[i], (unsigned char)(i % 251 + 1), n);
    }
    if (pthread_barrier_init(&start, NULL, WORKERS + 1)) fail();
    pthread_t threads[WORKERS];
    for (size_t i = 0; i < WORKERS; ++i)
        if (pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i)) fail();
    pthread_barrier_wait(&start);
    for (size_t i = 0; i < N; ++i) {
        size_t n = blocksize(i);
        unsigned char *p = malloc(n);
        if (!p) fail();
        memset(p, 0x5a, n);
        if (p[n - 1] != 0x5a) fail();
        free(p);
    }
    for (size_t i = 0; i < WORKERS; ++i)
        if (pthread_join(threads[i], NULL)) fail();
    for (size_t i = 0; i < N; ++i)
        if (blocks[i]) fail();
    pthread_barrier_destroy(&start);
    puts("thread transition and owner reuse: PASS");
    return 0;
}

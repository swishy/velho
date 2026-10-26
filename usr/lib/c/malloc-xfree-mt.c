/* cross-thread free stress: producer threads allocate+fill, consumer
 * threads verify+free OTHER threads' allocations */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

#define NPROD 4
#define NCONS 4
#define QUEUE_N 8192
#define ITERS 120000

/* simple SPSC handoff ring per (prod,cons) pair via one mutex-protected pool */
static void *pool[QUEUE_N];
static unsigned char fills[QUEUE_N];
static size_t szs[QUEUE_N];
static volatile unsigned head, tail;
static pthread_mutex_t plk = PTHREAD_MUTEX_INITIALIZER;
static volatile long produced, consumed;
static volatile int failures;

static void *producer(void *arg) {
    long id = (long)arg;
    uint64_t rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)id;
    for (int i = 0; i < ITERS; i++) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        size_t sz = 1 + (rng % 65536);
        unsigned char *p = malloc(sz);
        if (!p) { fprintf(stderr, "P%ld NULL\n", id); __sync_fetch_and_add(&failures, 1); return 0; }
        unsigned char f = (unsigned char)(rng >> 40) | 1;
        memset(p, f, sz);
        int spins = 0;
        for (;;) {
            pthread_mutex_lock(&plk);
            unsigned h = head, t = tail;
            if (h - t < QUEUE_N) {
                pool[h % QUEUE_N] = p; fills[h % QUEUE_N] = f; szs[h % QUEUE_N] = sz;
                head = h + 1;
                pthread_mutex_unlock(&plk);
                __sync_fetch_and_add(&produced, 1);
                break;
            }
            pthread_mutex_unlock(&plk);
            if (++spins > 100000000) { free(p); return 0; }
            sched_yield();
        }
    }
    return 0;
}

static void *consumer(void *arg) {
    long id = (long)arg;
    (void)id;
    for (;;) {
        pthread_mutex_lock(&plk);
        unsigned t = tail, h = head;
        void *p = NULL; unsigned char f = 0; size_t sz = 0;
        if (t != h) {
            p = pool[t % QUEUE_N]; f = fills[t % QUEUE_N]; sz = szs[t % QUEUE_N];
            tail = t + 1;
        }
        pthread_mutex_unlock(&plk);
        if (p) {
            unsigned char *q = p;
            if (q[0] != f || q[sz-1] != f || q[sz/2] != f) {
                fprintf(stderr, "CORRUPT %p sz=%zu\n", p, sz);
                __sync_fetch_and_add(&failures, 1);
            }
            free(p);
            __sync_fetch_and_add(&consumed, 1);
        } else {
            if (__atomic_load_n(&produced, __ATOMIC_ACQUIRE) >= NPROD * ITERS
                && __atomic_load_n(&consumed, __ATOMIC_ACQUIRE)
                   >= __atomic_load_n(&produced, __ATOMIC_ACQUIRE)) {
                return 0;
            }
            sched_yield();
        }
    }
}

int main(void) {
    pthread_t th[NPROD + NCONS];
    for (long i = 0; i < NPROD; i++) pthread_create(&th[i], NULL, producer, (void*)(i+1));
    for (long i = 0; i < NCONS; i++) pthread_create(&th[NPROD+i], NULL, consumer, (void*)(i+1));
    for (int i = 0; i < NPROD; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < NCONS; i++) pthread_join(th[NPROD+i], NULL);
    printf("XFREE produced=%ld consumed=%ld failures=%d\n",
           produced, consumed, failures);
    return failures ? 1 : 0;
}

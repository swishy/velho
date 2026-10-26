/* the livelock corner: owner parks (joins) with a full ring while
 * others keep freeing its allocations - free must not block */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#define N 5
static void *hold[N * 20000];
static pthread_barrier_t bar;
static void *owner(void *a) {
    (void)a;
    for (int i = 0; i < N * 20000; i++) {
        hold[i] = malloc(1 + (i % 512));
        if (!hold[i]) return 0;
    }
    pthread_barrier_wait(&bar);
    return 0;   /* parks (exits) with allocations live */
}
static void *freer(void *a) {
    long id = (long)a;
    pthread_barrier_wait(&bar);
    for (int i = id; i < N * 20000; i += N) {
        free(hold[i]);   /* deferred onto the parked owner's ring */
    }
    return 0;
}
int main(void) {
    pthread_t o, f[N];
    pthread_barrier_init(&bar, NULL, N + 1);
    pthread_create(&o, NULL, owner, 0);
    for (long i = 0; i < N; i++) pthread_create(&f[i], NULL, freer, (void*)(i+1));
    pthread_join(o, NULL);
    for (int i = 0; i < N; i++) pthread_join(f[i], NULL);
    printf("PARKED OK (frees completed without blocking)\n");
    return 0;
}

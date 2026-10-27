/* Diagnostic RSS samples after ten sequential worker bursts. Compile without
 * an allocator dependency and select an interposer with LD_PRELOAD. There is
 * deliberately no RSS pass/fail threshold: process/OS layouts differ. */
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 100000
#define WORKERS 10
static void *slots[N];

static void *
worker(void *unused)
{
    (void)unused;
    for (int i = 0; i < N; ++i) {
        slots[i] = malloc(32);
        if (!slots[i]) abort();
        memset(slots[i], i, 32);
    }
    for (int i = 0; i < N; ++i)
        free(slots[i]);
    return NULL;
}

static long
rss(void)
{
    FILE *file = fopen("/proc/self/status", "r");
    char line[256];
    long value = 0;
    if (!file) abort();
    while (fgets(line, sizeof(line), file))
        if (sscanf(line, "VmRSS: %ld kB", &value) == 1) break;
    fclose(file);
    return value;
}

int
main(void)
{
    printf("start %ld\n", rss());
    for (int i = 0; i < WORKERS; ++i) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, worker, NULL)
            || pthread_join(thread, NULL)) abort();
        printf("after %d %ld\n", i + 1, rss());
    }
    return 0;
}

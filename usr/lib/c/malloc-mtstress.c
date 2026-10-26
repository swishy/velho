/* MT stress: N threads x churn with content verification */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
static void onsegv(int sig, siginfo_t *si, void *uc) {
    ucontext_t *u = uc;
    char b[160];
    int o = snprintf(b, 150, "SEGV addr=%p rip=%llx\n", si->si_addr,
        (unsigned long long)u->uc_mcontext.gregs[REG_RIP]);
    if (write(2, b, o)) {}
    _exit(91);
}

#define NTHR 8
#define ITERS 150000

static void *worker(void *argp) {
    long id = (long)argp;
    uint64_t rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)id;
    for (int i = 0; i < ITERS; i++) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        size_t sz = 1 + (rng % 4096);
        unsigned char *p = malloc(sz);
        if (!p) { fprintf(stderr, "T%ld malloc NULL\n", id); return (void*)1; }
        unsigned char f = (unsigned char)(rng >> 32);
        memset(p, f, sz);
        if (p[0] != f || p[sz-1] != f) { fprintf(stderr, "T%ld CORRUPT\n", id); return (void*)1; }
        /* sometimes realloc */
        if ((rng & 7) == 0) {
            size_t nsz = 1 + (rng % 8192);
            unsigned char *q = realloc(p, nsz);
            if (!q) { fprintf(stderr, "T%ld realloc NULL\n", id); return (void*)1; }
            if (q[0] != f) { fprintf(stderr, "T%ld RCORRUPT\n", id); return (void*)1; }
            p = q; sz = nsz;
            memset(p, f, sz);
        }
        /* a live set: hold several, verify, free */
        if ((rng & 15) == 0) {
            void *hold[4]; size_t hsz[4];
            for (int h = 0; h < 4; h++) {
                hsz[h] = 1 + (rng % 65536);
                hold[h] = malloc(hsz[h]);
                if (!hold[h]) { fprintf(stderr, "T%ld hold NULL\n", id); return (void*)1; }
                memset(hold[h], 0xA7 ^ h, hsz[h]);
            }
            for (int h = 0; h < 4; h++)
                if (((unsigned char*)hold[h])[hsz[h]/2] != (0xA7 ^ h)) {
                    fprintf(stderr, "T%ld HOLD CORRUPT\n", id); return (void*)1;
                }
            for (int h = 0; h < 4; h++) free(hold[h]);
        }
        free(p);
    }
    return NULL;
}

int main(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = onsegv; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    pthread_t th[NTHR];
    for (long i = 0; i < NTHR; i++) pthread_create(&th[i], NULL, worker, (void*)i);
    for (int i = 0; i < NTHR; i++) {
        void *r; pthread_join(th[i], &r);
        if (r) { fprintf(stderr, "FAILED\n"); return 1; }
    }
    printf("MT OK (%d threads x %d iters)\n", NTHR, ITERS);
    return 0;
}

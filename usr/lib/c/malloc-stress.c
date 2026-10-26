/* final stress suite for libvelhomalloc.so */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <malloc.h>

#define CHK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t
rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

int
main(void)
{
    alarm(30);

    void *p;
    /* stage 1: class edges */
    fprintf(stderr, "stage 1\n");
    p = malloc(1); CHK(p); memset(p, 0xAB, 1); free(p);
    p = malloc(64); CHK(p); memset(p, 0xAB, 64); free(p);
    p = malloc(4095); CHK(p); memset(p, 0xAB, 4095); free(p);
    p = malloc(4096); CHK(p); memset(p, 0xAB, 4096); free(p);
    p = malloc(64 * 1024); CHK(p); memset(p, 0xAB, 64 * 1024); free(p);
    p = malloc(1024 * 1024); CHK(p); memset(p, 0xAB, 1024 * 1024); free(p);
    p = malloc(0); free(p);
    p = calloc(16, 16); CHK(p); memset(p, 0, 256); free(p);
    /* alarm stays armed */

    /* stage 2: churn with content verification */
    fprintf(stderr, "stage 2\n");
    for (int i = 0; i < 200000; i++) {
        size_t sz = 1 + (rnd() & 4095);
        unsigned char *q = malloc(sz);
        if (!q) { fprintf(stderr, "NULL i=%d sz=%zu\n", i, sz); exit(1); }
        memset(q, (int)(i & 0xFF), sz);
        for (size_t j = 0; j < sz; j += 97) {
            if (q[j] != (i & 0xFF)) {
                fprintf(stderr, "CORRUPT i=%d sz=%zu j=%zu got=%02x want=%02x ptr=%p\n",
                        i, sz, j, q[j], (unsigned)(i & 0xFF), (void *)q);
                exit(1);
            }
        }
        free(q);
    }

    /* stage 3: many live, random free order */
    fprintf(stderr, "stage 3\n");
    enum { NLIVE = 4096 };
    unsigned char *live[NLIVE];
    size_t szs[NLIVE];
    unsigned char fills[NLIVE];
    for (int i = 0; i < NLIVE; i++) {
        szs[i] = 1 + (rnd() % (256 * 1024));
        fills[i] = i & 0xFF;
        live[i] = malloc(szs[i]);
        CHK(live[i]);
        memset(live[i], fills[i], szs[i]);
    }
    for (int i = NLIVE - 1; i > 0; i--) {
        int j = rnd() % (i + 1);
        unsigned char *tp = live[i]; live[i] = live[j]; live[j] = tp;
        size_t ts = szs[i]; szs[i] = szs[j]; szs[j] = ts;
        unsigned char tf = fills[i]; fills[i] = fills[j]; fills[j] = tf;
    }
    for (int i = 0; i < NLIVE; i++) {
        for (size_t j = 0; j < szs[i]; j += 131) CHK(live[i][j] == fills[i]);
        free(live[i]);
    }

    /* stage 4: realloc growth chain */
    fprintf(stderr, "stage 4\n");
    p = malloc(7); CHK(p); memset(p, 0x5A, 7);
    size_t cur = 7;
    for (int i = 0; i < 18; i++) {
        size_t nsz = cur * 3 + 11;
        void *np = realloc(p, nsz);
        if (!np) { fprintf(stderr, "REALLOC-NULL at %zu\n", nsz); exit(1); }
        CHK(((unsigned char *)np)[0] == 0x5A);
        CHK(((unsigned char *)np)[6] == 0x5A);
        p = np; cur = nsz;
    }
    free(p);

    /* stage 5: realloc shrink */
    fprintf(stderr, "stage 5\n");
    p = malloc(4096); CHK(p);
    strcpy((char *)p, "hello velho");
    p = realloc(p, 32); CHK(p);
    CHK(strcmp((char *)p, "hello velho") == 0);
    free(p);

    /* stage 6: alignment */
    fprintf(stderr, "stage 6\n");
    p = aligned_alloc(256, 1000); CHK(p); CHK(((uintptr_t)p & 255) == 0); free(p);
    CHK(posix_memalign(&p, 4096, 100) == 0); CHK(((uintptr_t)p & 4095) == 0); free(p);

    /* stage 7: usable size */
    fprintf(stderr, "stage 7\n");
    p = malloc(100); CHK(p);
    CHK(malloc_usable_size(p) >= 100);
    free(p);

    /* stage 8: randomized mix */
    fprintf(stderr, "stage 8\n");
    alarm(60);
    for (int i = 0; i < 200000; i++) {
        size_t sz = 1 + (rnd() % (128 * 1024));
        unsigned char *q = malloc(sz);
        if (!q) { fprintf(stderr, "NULL i=%d sz=%zu\n", i, sz); exit(1); }
        q[sz - 1] = 0x7C; q[0] = 0xC7;
        if ((rnd() & 3) == 0) {
            size_t nsz = 1 + (rnd() % (128 * 1024));
            q = realloc(q, nsz);
            CHK(q);
            CHK(q[0] == 0xC7);
        }
        CHK(q[0] == 0xC7);
        free(q);
    }
    /* alarm stays armed */

    printf("ALL STAGES PASS\n");
    return 0;
}

/* final stress suite for libvelhomalloc.so */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <malloc.h>

#define CHK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t
rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

static void *g_cur_ptr;
static int g_cur_i;
static size_t g_cur_sz;
extern __thread struct memtls *t_memtls;
extern struct memglob g_mem;
struct membufq2 { volatile long mtx; unsigned long qid; long nbuf; void *tls; struct membuf2 *head; struct membuf2 *tail; long pad[2]; };
struct membuf2 { char *adr; size_t size; size_t ofs; size_t max; uintptr_t bits; void *queue; struct membuf2 *prev; struct membuf2 *next; };
static void
onsegv2(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)uc;
    char b[256];
    int o = snprintf(b, 200,
        "VERIFY-SEGV addr=%p i=%d ptr=%p sz=%zu\n",
        si->si_addr, g_cur_i, g_cur_ptr, g_cur_sz);
    if (o > 0) { ssize_t w = write(2, b, o); (void)w; }
    {
        extern struct bigunmap2 { unsigned long page; size_t size; } *g_bigu_pub;
        extern unsigned *g_bigui_pub;
        unsigned n = *g_bigui_pub;
        for (unsigned k = n > 64 ? n - 64 : 0; k < n; k++) {
            struct bigunmap2 *u = &g_bigu_pub[k % 256];
            unsigned long vp = (unsigned long)g_cur_ptr;
            if (vp >= u->page && vp < u->page + u->size) {
                char b2[160];
                int o2 = snprintf(b2, 150,
                    "KILLED-BY-BIG page=%lx size=%zu (unmap #%u)\n",
                    u->page, u->size, k);
                if (o2 > 0) { ssize_t w2 = write(2, b2, o2); (void)w2; }
            }
        }
    }
    if (t_memtls) {
        for (int tbl = 0; tbl < 2; tbl++) {
            for (int q = 0; q < 64; q++) {
                struct membufq2 *qu = tbl
                    ? (struct membufq2 *)((char *)t_memtls + 64 * 64 * 8 /*placeholder*/)
                    : (struct membufq2 *)((char *)t_memtls + (size_t)q * 64);
                (void)qu;
            }
        }
        /* walk both tables via byte offsets (membufq = 64 bytes) */
        for (int tbl = 0; tbl < 2; tbl++) {
            for (int q = 0; q < 64; q++) {
                struct membufq2 *qu = (struct membufq2 *)
                    ((char *)t_memtls + tbl * 4096 + q * 64);
                for (struct membuf2 *bf = qu->head; bf && bf != bf->next; bf = bf->next) {
                    if ((char *)g_cur_ptr >= bf->adr
                        && (char *)g_cur_ptr < bf->adr + bf->size) {
                        o = snprintf(b, 240,
                            "OWNER tbl=%d q=%lu buf=%p adr=%p size=0x%zx bits=%lx max=%zx nbuf=%ld\n",
                            tbl, qu->qid, (void *)bf, (void *)bf->adr,
                            bf->size, (unsigned long)bf->bits, bf->max, qu->nbuf);
                        if (o > 0) { ssize_t w2 = write(2, b, o); (void)w2; }
                    }
                }
            }
        }
    }
    _exit(93);
}

static void
onalrm(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si;
    ucontext_t *u = uc;
    char b[128];
    unsigned long long rp = u->uc_mcontext.gregs[REG_RIP];
    unsigned long long rc = u->uc_mcontext.gregs[REG_RCX];
    unsigned long long wd = *(unsigned long long *)rc;
    int o = snprintf(b, 120, "ALARM rip=%llx word=%llx\n", rp, wd);
    if (o > 0) { ssize_t w = write(2, b, o); (void)w; }
    _exit(97);
}

int
main(void)
{
    struct sigaction sa = {0};
    sa.sa_sigaction = onalrm; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGALRM, &sa, NULL);
    alarm(3);
    sa.sa_sigaction = onsegv2;
    sigaction(SIGSEGV, &sa, NULL);
    { void *m = dlsym(RTLD_DEFAULT, "malloc"); char b[80];
      int o = snprintf(b, 70, "malloc-sym=%p\n", m);
      if (o > 0) { ssize_t w = write(2, b, o); (void)w; }
      FILE *f = fopen("/proc/self/maps", "r");
      if (f) { char line[512];
          while (fgets(line, sizeof line, f))
              if (strstr(line, "velho") || strstr(line, "mimalloc"))
                  fputs(line, stderr);
          fclose(f); } }
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
        g_cur_ptr = live[i]; g_cur_i = i; g_cur_sz = szs[i];
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

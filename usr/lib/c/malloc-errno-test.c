#define _GNU_SOURCE 1
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

/* A valid free must preserve errno even if optional cold-page reclamation
 * cannot discard an mlocked payload page. No private allocator layout used. */
int main(void)
{
    enum { N = 1024 };
    void *blocks[N];
    for (int i = 0; i < N; ++i) {
        blocks[i] = malloc(32);
        if (!blocks[i]) return 1;
    }
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    void *page = (void *)((uintptr_t)blocks[900] & ~(uintptr_t)(page_size - 1));
    if (mlock(page, page_size)) {
        for (int i = 0; i < N; ++i) free(blocks[i]);
        puts("free errno with locked payload: SKIP (mlock unavailable)");
        return 0;
    }
    for (int i = 0; i < N - 1; ++i) free(blocks[i]);
    errno = EDOM;
    free(blocks[N - 1]);
    int actual = errno;
    munlock(page, page_size);
    if (actual != EDOM) {
        fprintf(stderr, "free changed errno: got %d expected %d\n", actual, EDOM);
        return 1;
    }
    puts("free errno with locked payload: PASS");
    return 0;
}

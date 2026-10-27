#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

int main(void)
{
    enum { N = 192 };
    unsigned char *p[N];
    size_t capacity[N];
    /* Fill the entire reported capacity, with many simultaneous live
     * allocations, to expose overlap and overreported usable sizes. */
    for (size_t align = 16; align <= 1048576; align *= 2) {
        for (unsigned round = 0; round < 4; ++round) {
            for (unsigned i = 0; i < N; ++i) {
                static const size_t edges[] = {
                    4095, 4096, 4097, 65535, 65536, 65537,
                    258047, 258048, 258049, 262143, 262144, 262145
                };
                size_t size = round == 3
                    ? edges[i % (sizeof(edges) / sizeof(edges[0]))]
                    : 1 + ((i * 131 + round * 37) % 9000);
                assert(posix_memalign((void **)&p[i], align, size) == 0);
                assert(((uintptr_t)p[i] & (align - 1)) == 0);
                capacity[i] = malloc_usable_size(p[i]);
                assert(capacity[i] >= size);
                memset(p[i], (unsigned char)(i + 1), capacity[i]);
            }
            /* Odd permutation traverses every allocation. */
            for (unsigned j = 0; j < N; ++j) {
                unsigned i = (j * 73) % N;
                for (size_t k = 0; k < capacity[i]; ++k)
                    assert(p[i][k] == (unsigned char)(i + 1));
                unsigned char *q = realloc(p[i], capacity[i] + 8192);
                assert(q);
                for (size_t k = 0; k < capacity[i]; ++k)
                    assert(q[k] == (unsigned char)(i + 1));
                free(q);
            }
        }
    }
    puts("aligned capacity, overlap, growth, reuse: PASS");
    return 0;
}

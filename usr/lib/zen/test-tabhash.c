/* Page sharing, packed bucket chains, and subtable recycling. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <zen/_hash.h>
#include <mach/param.h>

/* Standalone configuration for the general tabhash utility. The allocator
 * derives ownership from aligned addresses and does not use this table. */
struct memhash { uintptr_t page, val; };
#define MEM_HASH_NREF_MASK (((uintptr_t)1 << (PAGESIZELOG2 - 2)) - 1)
#define memhashpage(item) ((item)->page & ~(uintptr_t)(PAGESIZE - 1))
#define TABHASH_SLOTS 16384
#define TABHASH_INVALID { 0, 0 }
#define TABHASH_BUF g_memhashbuf
#define TABHASH_TAB_T struct tabhashtab
#define TABHASH_ITEM_T struct memhash
#define TABHASH_TAB_ITEMS 30
#if WORDSIZE == 8
#define TABHASH_HASH(key) (tmhash64(key) & (TABHASH_SLOTS - 1))
#else
#define TABHASH_HASH(key) (tmhash32(key) & (TABHASH_SLOTS - 1))
#endif
#define TABHASH_CMP(item, key) (memhashpage(item) != (key))
#define TABHASH_COPY(src, dst) (*(dst) = *(src))
#define TABHASH_GET_NREF(item) ((item)->page & MEM_HASH_NREF_MASK)
#define TABHASH_PUT_NREF(item, n) ((item)->page |= (n))
#define TABHASH_CHK(item) ((item)->page && (item)->val)
#include <zen/tabhash.h>
struct tabhashtab *g_memhashtab[TABHASH_SLOTS];
struct tabhashtab *g_memhashbuf;

int main(void)
{
    uintptr_t page = 0x100000, val = 0x200001;
    for (int i = 0; i < 64; ++i)
        assert(tabhashaddref(g_memhashtab, page, val));
    struct tabhashtab *tab = g_memhashtab[TABHASH_HASH(page)];
    assert(tab->ncur == 1);
    assert(TABHASH_GET_NREF(&tab->items[0]) == 64);
    for (uintptr_t i = 63; i > 0; --i) {
        assert(tabhashrm(g_memhashtab, page).page);
        struct memhash item = tabhashfind(g_memhashtab, page);
        assert(TABHASH_GET_NREF(&item) == i);
    }
    assert(tabhashrm(g_memhashtab, page).page);
    assert(!tabhashfind(g_memhashtab, page).page);
    assert(g_memhashbuf == tab);
    assert(tabhashgettab() == tab);
    tabhashputtab(tab);

    /* Fill three subtables in one bucket, promote each by lookup, then
     * remove them in insertion order. This exercises both list links. */
    enum { N = TABHASH_TAB_ITEMS * 3 };
    uintptr_t keys[N];
    size_t slot = TABHASH_HASH(page);
    int count = 0;
    for (uintptr_t key = PAGESIZE; count < N; key += PAGESIZE) {
        if (TABHASH_HASH(key) == slot) {
            keys[count++] = key;
            assert(tabhashaddref(g_memhashtab, key, val));
        }
    }
    for (int i = N - 1; i >= 0; --i) {
        struct memhash item = tabhashfind(g_memhashtab, keys[i]);
        assert(memhashpage(&item) == keys[i]);
        assert(item.val == val);
    }
    for (int i = 0; i < N; ++i) {
        assert(tabhashrm(g_memhashtab, keys[i]).page);
        assert(!tabhashfind(g_memhashtab, keys[i]).page);
    }
    assert(!g_memhashtab[slot]);
    puts("page sharing, collision chains, recycling: PASS");
    return 0;
}

/* Page sharing, packed bucket chains, and subtable recycling. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <zen/_hash.h>
#include <zen/mem.h>
struct tabhashtab *g_memhashtab[TABHASH_SLOTS];
struct tabhashtab *g_memhashbuf;
struct memglob g_mem;

int main(void)
{
    uintptr_t page = 0x100000, val = 0x200001;
    for (int i = 0; i < 64; ++i)
        assert(tabhashaddref(g_memhashtab, page, val));
    struct tabhashtab *tab = g_memhashtab[TABHASH_HASH(page)];
    assert(tab->ncur == 1);
    assert(TABHASH_GET_NREF(&tab->items[0]) == 64);
    for (int i = 63; i > 0; --i) {
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

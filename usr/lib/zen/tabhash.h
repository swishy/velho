#ifndef __ZEN_TABHASH_H__
#define __ZEN_TABHASH_H__

/*
 * cache-friendly hash table
 * -------------------------
 * - instead of chaining single hash-items into linked lists, we chain tables
 *   of a few dozen items with a few words of book-keeping information in order
 *   to leverage massive cache-prefetching and avoid pointer-chasing/cache-
 *   fetching on per-item basis
 */

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <mach/param.h>
#include <mt/lk.h>
#include <zero/trix.h>
#include <zen/sys/unix.h>
#include <zen/hash.h>

#define TABHASH_LK_BIT_OFS 0
#define TABHASH_LK_BIT ((uintptr_t)1 << TABHASH_LK_BIT_OFS)

extern TABHASH_TAB_T   *TABHASH_BUF;

#define tabhashfind(hash, key)  tabhashop(hash, key, TABHASH_FIND)
#define tabhashrm(hash, key)    tabhashop(hash, key, TABHASH_REMOVE)
#define tabhashref(hash, key)   tabhashop(hash, key, TABHASH_ADD_REF)
#if defined(TABHASH_FREE)
#define tabhashdel(hash, key)   TABHASH_FREE(tabhashrm(hash, key))
#else
#define tabhashdel(hash, key)   tabhashrm(hash, key)
#endif
#if !defined(TABHASH_COPY)
#define TABHASH_COPY(src, dest) memcpy(dest, src, sizeof(TABHASH_ITEM_T))
#endif
#if !defined(TABHASH_CLEAR)
#define TABHASH_CLEAR(ptr)      memset(ptr, 0, sizeof(TABHASH_ITEM_T))
#endif

#define TABHASH_TAB_SIZE        (roundup2(sizeof(struct tabhashtab), CLSIZE))
#define TABHASH_BUF_SIZE        (2 * PAGESIZE)
#define TABHASH_CACHE_TABS      (TABHASH_BUF_SIZE / TABHASH_TAB_SIZE)
struct tabhashtab {
    long                ncur;
    long                nmax;
    TABHASH_TAB_T      *prev;
    TABHASH_TAB_T      *next;
    TABHASH_ITEM_T      items[TABHASH_TAB_ITEMS];
};

static __inline__ void
tabhashputtab(TABHASH_TAB_T *tab)
{
    TABHASH_TAB_T      *head;
    mtlkbit((m_atomic_t *)&TABHASH_BUF, TABHASH_LK_BIT_OFS);
    head = TABHASH_BUF;
    head = (void *)((uintptr_t)head & ~TABHASH_LK_BIT);
    if (head) {
        head->prev = tab;
    }
    tab->next = head;
    tab->prev = NULL;   /* GAP-FILL(vs): BUF-resident prev is meaningless */
    mtunlkval((m_atomic_t *)&TABHASH_BUF, (m_atomic_t)tab);

    return;
}

/* get hash subtable */
static __inline__ TABHASH_TAB_T *
tabhashgettab(void)
{
    TABHASH_TAB_T      *head = NULL;
    TABHASH_TAB_T      *tab;
    long                ntab;

    /* lock buffer */
    mtlkbit((m_atomic_t *)&TABHASH_BUF, TABHASH_LK_BIT_OFS);
    tab = TABHASH_BUF;
    tab = (void *)((uintptr_t)tab & ~TABHASH_LK_BIT);
    if (!tab) {
        int8_t         *ptr = mapanon(-1, TABHASH_BUF_SIZE, 0);
        TABHASH_TAB_T  *cur;
        TABHASH_TAB_T  *prev;

        /* allocate more subtables */
        if (ptr == MAP_FAILED) {
            tab = NULL;
            mtunlkval((m_atomic_t *)&TABHASH_BUF, (m_atomic_t)head);
        } else {
            tab = (TABHASH_TAB_T *)ptr;         // first table
            ntab = TABHASH_CACHE_TABS - 1;
            ptr += TABHASH_TAB_SIZE;
            head = (TABHASH_TAB_T *)ptr;       // second table
            prev = head;
            while (--ntab) {
                /* chain newly-allocated table structures */
                ptr += TABHASH_TAB_SIZE;
                cur = (TABHASH_TAB_T *)ptr;
                prev->next = cur;
                cur->prev = prev;
                prev = cur;
            }
            /* queue newly-allocated chain of tables */
            prev->next = NULL;
            mtunlkval((m_atomic_t *)&TABHASH_BUF, (m_atomic_t)head);
        }
    } else {
        /* remove head from queue */
        head = tab->next;
        if (head) {
            head->prev = NULL;
        }
        mtunlkval((m_atomic_t *)&TABHASH_BUF, (m_atomic_t)head);
    }
    if (tab) {
        tab->nmax = TABHASH_TAB_ITEMS;
        /* GAP-FILL(vs): recycled subtables kept their old ncur, so
         * re-added items landed at stale indices */
        tab->ncur = 0;
        memset(&tab->items, 0, sizeof(tab->items));
    }

    return tab;
}

static __inline__ long
tabhashadd(TABHASH_TAB_T **hashtab, const uintptr_t key, const uintptr_t val)
{
    TABHASH_TAB_T      *head;
    TABHASH_TAB_T      *tab;
    TABHASH_TAB_T      *tail = NULL;
    long                slot;
    long                ndx;
    long                stored = 0;
    TABHASH_ITEM_T      item = { key, val };

    /* GAP-FILL(vs): rewritten. Upstream reused ndx as the item index and
     * unlocked the wrong slot, read `loop` uninitialized, and - the
     * layout-dependent killer - when a bucket chain was entirely full the
     * walk fell out and the function returned success WITHOUT storing the
     * item, leaving allocations with no page-hash entry at all. Walk with
     * an explicit stored flag, append a fresh subtable at the tail on
     * exhaustion, keep the slot separate for lock/unlock. */
    slot = TABHASH_HASH(key);
    mtlkbit((m_atomic_t *)&hashtab[slot], TABHASH_LK_BIT_OFS);
    head = (void *)((uintptr_t)hashtab[slot] & ~TABHASH_LK_BIT);
    tab = head;
    while (tab) {
        ndx = tab->ncur;
        if (ndx < tab->nmax) {
            TABHASH_COPY(&item, &tab->items[ndx]);
            TABHASH_PUT_NREF(&tab->items[ndx], 1);
            ndx++;
            tab->ncur = ndx;
            stored = 1;

            break;
        }
        tail = tab;
        tab = tab->next;
    }
    if (!stored) {
        tab = tabhashgettab();
        if (!tab) {
            mtunlkval((m_atomic_t *)&hashtab[slot], (m_atomic_t)head);   /* MT: paired unlock, store possibly-updated head */

            return 0;
        }
        tab->nmax = TABHASH_TAB_ITEMS;
        tab->ncur = 0;
        TABHASH_COPY(&item, &tab->items[0]);
        TABHASH_PUT_NREF(&tab->items[0], 1);
        tab->ncur = 1;
        tab->prev = tail;
        tab->next = NULL;
        if (tail) {
            tail->next = tab;
        } else {
            head = tab;
            tab->prev = NULL;   /* GAP-FILL(vs): chain head owns no prev */
        }
    }
    mtunlkval((m_atomic_t *)&hashtab[slot], (m_atomic_t)head);   /* MT: paired unlock, store possibly-updated head */

    return 1;
}

#define TABHASH_REMOVE          (-1L)
#define TABHASH_FIND            0
#define TABHASH_ADD_REF         (1L)
static __inline__ TABHASH_ITEM_T
tabhashop(TABHASH_TAB_T **hashtab, const uintptr_t val, long cmd)

{
    TABHASH_ITEM_T      ret = TABHASH_INVALID;
    TABHASH_TAB_T      *tab;
    TABHASH_TAB_T      *head;
    TABHASH_TAB_T      *prev;
    TABHASH_TAB_T      *next;
    TABHASH_ITEM_T     *item;
    long                ndx;
    long                n;
    long                nref;
    long                lim;
    long                cur;

    ndx = TABHASH_HASH(val);
    prev = NULL;
    mtlkbit((m_atomic_t *)&hashtab[ndx], TABHASH_LK_BIT_OFS);
    head = hashtab[ndx];
    tab = (void *)((uintptr_t)head & ~TABHASH_LK_BIT);
    head = tab;
    while (tab) {
        lim = tab->ncur;
        item = &tab->items[0];
        for (cur = 0 ; cur < lim ; cur++) {
            if (!TABHASH_CMP(item, val)) {
                TABHASH_COPY(item, &ret);
                nref = TABHASH_GET_NREF(item);
                if (cmd == TABHASH_REMOVE && nref == 1) {
                    /* remove item from hash */
                    n = tab->ncur;
                    n--;
                    if (cur != n) {
                        /* copy last item in table over removed one */
                        TABHASH_COPY(&tab->items[n], &tab->items[cur]);
                    }
#if defined(TABHASH_FREE)
                    TABHASH_FREE(tab->items[n]);
#endif
                    TABHASH_CLEAR(&tab->items[n]);
                    tab->ncur = n;
                    prev = tab->prev;
                    next = tab->next;
                    if (!n) {
                        /* free empty table */
                        if (prev) {
                            prev->next = next;
                        } else {
                            head = next;
                        }
                        if (next) {
                            next->prev = prev;
                        }
                        tabhashputtab(tab);
                    }
                } else {
                    nref += cmd;
                    /* GAP-FILL(vs): PUT_NREF is an OR - decrements corrupt
                     * the count so entries never reach zero (leak,
                     * quadratic chains). Clear then set. */
                    (item)->page = ((item)->page & ~MEM_HASH_NREF_MASK)
                                   | (uintptr_t)nref;
                    if (prev) {
                        /* insert table into front of queue */
                        next = tab->next;
                        prev->next = next;
                        if (next) {
                            next->prev = prev;
                        }
                        /* GAP-FILL(vs): hashtab[ndx] is read while its
                         * lock bit is set, so the raw slot value (head|1)
                         * was copied into the chain - a misaligned pseudo-
                         * pointer that corrupts the list and leaves bucket
                         * locks stuck forever. The unmodified chain head
                         * is what belongs here. */
                        tab->next = head;
                        tab->prev = NULL;  /* GAP-FILL(vs): new chain head */
                        head->prev = tab;
                    }
                    head = tab;
                }
                mtunlkval((m_atomic_t *)&hashtab[ndx], (m_atomic_t)head);   /* MT: paired unlock */

                return ret;
            }
            item++;
        }
        prev = tab;
        tab = tab->next;
    }
    mtunlkval((m_atomic_t *)&hashtab[ndx], (m_atomic_t)head);   /* MT: paired unlock */

    return ret;
}

static __inline__ long
tabhashaddref(TABHASH_TAB_T **hashtab, const uintptr_t key, const uintptr_t val)
{
    TABHASH_ITEM_T      item = tabhashop(hashtab, key, TABHASH_ADD_REF);
    long                retval = 0;

    if (!TABHASH_CHK(&item)) {
        retval = tabhashadd(hashtab, key, val);
    } else {
        retval = 1;
    }

    return retval;
}

#endif /* __ZEN_TABHASH_H__ */


#ifndef __MT_LK_H__
#define __MT_LK_H__

/* velho lock routines - GAP-FILL(vs) override
 * The upstream mtlkbit spins on a plain load of the lock word; the engine
 * re-enters these locks within a single thread (the original m_lkbit macro
 * never tested the word at all, so re-entrancy silently worked everywhere).
 * Single-threaded semantics here: RMW acquire without self-spinning - a
 * set bit is always our own hold. Multi-threaded use needs the recursion
 * untangled first (documented in FINDINGS.md). Unlock is an atomic clear. */

#include <stdint.h>
#include <stdio.h>
#include <mach/asm.h>

#define MT_MEM_LK_BIT          ((uintptr_t)1 << MT_MEM_LK_BIT_OFS)
#define MT_MEM_LK_BIT_OFS      1

/* MT-DEBUG: per-thread held-word registry. A recursive acquisition of a
 * word this thread already holds is logged once per site (caller) and
 * tolerated - after the call sites are fixed structurally this becomes
 * an abort. */
#define MTREG_MAX 32
static THREADLOCAL void *mt_reg[MTREG_MAX];
static THREADLOCAL int   mt_regn;
static void
mtreg_add(void *p)
{
    if (mt_regn < MTREG_MAX) {
        mt_reg[mt_regn++] = p;
    }
}
static void
mtreg_rm(void *p)
{
    for (int i = 0; i < mt_regn; i++) {
        if (mt_reg[i] == p) {
            mt_reg[i] = mt_reg[--mt_regn];
            return;
        }
    }
}
static int
mtreg_has(void *p)
{
    for (int i = 0; i < mt_regn; i++) {
        if (mt_reg[i] == p) {
            return 1;
        }
    }
    return 0;
}
static INLINE long
mtlkbit(volatile m_atomic_t *lp, long pos)
{
    if (UNLIKELY(mtreg_has((void *)lp))) {
        extern void mt_reenter_log(void *lp, void *caller);
        mt_reenter_log((void *)lp, __builtin_return_address(0));
    } else {
        while (__atomic_load_n(lp, __ATOMIC_ACQUIRE)
               & ((m_atomic_t)1 << pos)) {
            m_waitspin();
        }
        (void)__atomic_fetch_or(lp, (m_atomic_t)1 << pos, __ATOMIC_ACQ_REL);
        mtreg_add((void *)lp);
    }

    return 0;
}

/* MT: paired unlock that stores a NEW value (chain head may have moved
 * under the lock - clearing the bit alone would lose the update) */
static INLINE void
mtunlkval(volatile m_atomic_t *lp, m_atomic_t val)
{
    mtreg_rm((void *)lp);
    __atomic_store_n(lp, val, __ATOMIC_RELEASE);
    m_endspin();
}

static INLINE void
mtunlkbit(volatile m_atomic_t *lp, long pos)
{
    mtreg_rm((void *)lp);
    __atomic_and_fetch(lp, ~((m_atomic_t)1 << pos), __ATOMIC_RELEASE);
    m_endspin();

    return;
}

#endif /* __MT_LK_H__ */

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
#include <mach/asm.h>

#define MT_MEM_LK_BIT          ((uintptr_t)1 << MT_MEM_LK_BIT_OFS)
#define MT_MEM_LK_BIT_OFS      1

static INLINE long
mtlkbit(volatile m_atomic_t *lp, long pos)
{
    (void)__atomic_fetch_or(lp, (m_atomic_t)1 << pos, __ATOMIC_ACQ_REL);

    return 0;
}

static INLINE void
mtunlkbit(volatile m_atomic_t *lp, long pos)
{
    __atomic_and_fetch(lp, ~((m_atomic_t)1 << pos), __ATOMIC_RELEASE);
    m_endspin();

    return;
}

#endif /* __MT_LK_H__ */

#ifndef __MT_LK_H__
#define __MT_LK_H__

/* velho lock routines
 *
 * GAP-FILL(vs): the upstream m_cmpsetbit family declared the result
 * register as output-only ("=r") while writing it only on the contended
 * path - the acquired path returned an undefined register, so callers
 * saw "already locked" after acquiring and vice versa (the root cause
 * behind every "recursive lock" diagnosis; the engine never truly
 * re-entered). The primitive is fixed ("+r") upstream of this header;
 * these wrappers are the honest acquire/release it always expected. */

#include <stdint.h>
#include <mach/asm.h>

#define MT_MEM_LK_BIT          ((uintptr_t)1 << MT_MEM_LK_BIT_OFS)
#define MT_MEM_LK_BIT_OFS      1

static INLINE long
mtlkbit(volatile m_atomic_t *lp, long pos)
{
    m_atomic_t  bit = (m_atomic_t)1 << pos;
    m_atomic_t  old;

    do {
        while (__atomic_load_n(lp, __ATOMIC_ACQUIRE) & bit) {
            m_waitspin();
        }
        old = __atomic_fetch_or(lp, bit, __ATOMIC_ACQ_REL);
    } while (old & bit);

    return 0;
}

/* unlock that stores a NEW value (chain heads move under the lock;
 * clearing the bit alone would lose the update) */
static INLINE void
mtunlkval(volatile m_atomic_t *lp, m_atomic_t val)
{
    __atomic_store_n(lp, val, __ATOMIC_RELEASE);
    m_endspin();
}

static INLINE void
mtunlkbit(volatile m_atomic_t *lp, long pos)
{
    __atomic_and_fetch(lp, ~((m_atomic_t)1 << pos), __ATOMIC_RELEASE);
    m_endspin();
}

#endif /* __MT_LK_H__ */

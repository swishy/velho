#ifndef __MACH_ASM_H__
#define __MACH_ASM_H__

#include <mach/atomic.h>
#include <mach/mem.h>
#if defined(_WIN64)
#include <zero/msc/win64.h>
#elif defined(_WIN32)
#include <zero/msc/win32.h>
#elif (defined(__x86_64__) || defined(__amd64__))
#include <mach/x86/asm.h>
#include <mach/x86-64/asm.h>
#elif (defined(__i386__) || defined(__i486__)                           \
       || defined(__i586__) || defined(__i686__))
#include <mach/x86/asm.h>
#include <mach/ia32/asm.h>
#elif defined(__arm__)
#include <mach/arm/asm.h>
#elif defined(__ppc__)
#include <mach/ppc/asm.h>
#endif

/* GAP-FILL(vs): the upstream macro's guard tested the POINTER, not the
 * lock word, and its cmpsetbit result was undefined on the acquired
 * path (see mt/lk.h). Honest acquire with the repaired primitive. */
#define m_trylkbit(p, ndx)                                              \
    (!(__atomic_fetch_or((m_atomic_t *)(p), (m_atomic_t)1 << (ndx),     \
                         __ATOMIC_ACQ_REL) & ((m_atomic_t)1 << (ndx))))
#define m_unlkbit(p, ndx)                                               \
    __atomic_and_fetch((m_atomic_t *)(p), ~((m_atomic_t)1 << (ndx)),    \
                       __ATOMIC_RELEASE)
#define m_lkbit(p, ndx)                                                 \
    do {                                                                \
        m_atomic_t _bit = (m_atomic_t)1 << (ndx);                       \
        m_atomic_t _old;                                                \
                                                                        \
        do {                                                            \
            while (__atomic_load_n((volatile m_atomic_t *)(p),          \
                                   __ATOMIC_ACQUIRE) & _bit) {          \
                m_waitspin();                                           \
            }                                                           \
            _old = m_cmpsetbit((p), (ndx));                             \
        } while (_old & _bit);                                          \
    } while (0)

#endif /* __MACH_ASM_H__ */


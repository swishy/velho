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

/* GAP-FILL(vs): the guard tested the POINTER value, not the lock word,
 * and the bit ops were hand-rolled asm the optimiser was free to cache
 * at -O1+ (eternal spin on an already-locked word). Atomic builtins. */
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
        /* GAP-FILL(vs): the engine re-enters these locks on the same word
         * within one thread (the original primitive never actually tested
         * the word, so re-entrancy silently worked). Single-threaded
         * semantics: RMW-acquire without self-spinning; a set bit is
         * always our own hold. (Multi-threaded use needs the recursion
         * untangled first - see FINDINGS.) */ \
        _old = __atomic_fetch_or((m_atomic_t *)(p), _bit,               \
                                 __ATOMIC_ACQ_REL);                     \
    } while (0)

#endif /* __MACH_ASM_H__ */


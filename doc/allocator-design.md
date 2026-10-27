# Zen allocator design and maintenance

This describes the maintained implementation in `usr/lib/zen/mem.c`,
`usr/lib/zen/mem.h` and `usr/lib/c/malloc.c`. Read it before changing allocation,
free, alignment or ownership handling. See [CI and releases](allocator-ci.md)
for automated checks and [performance](allocator-performance.md) for the current
measurements and reproduction commands.

The working mandate is concise pointer/bit arithmetic, little metadata and
minimal work per allocation. It follows the project's reconstruction of Vendu's
intentions, not a recovered original specification. Prefer deriving state from
addresses and existing ownership. Add state for a demonstrated cost or invariant,
and explain its lifetime. Keep independently measurable changes reviewable.
Vendu's [original allocator sketch](../usr/lib/c/malloc.txt) is retained as
background; its proposed internal API is historical.

## Ownership is encoded by alignment

Regular slabs are power-of-two aligned mappings, **256 KiB on the tested x86-64
configuration**. Each slab contains its own header and allocation bitmaps.
`memptrbuf(ptr)` derives that header with `(ptr - 1) & ~(MEM_SLAB_SIZE - 1)`.
Regular allocations have no individual header, page hash, page reference count, radix tree,
ownership cache or separate header pool. The class stride is in the owner queue.

The subtraction is part of the representation. A BIG allocation can return an
over-aligned pointer exactly on a slab boundary; its header is in the preceding
aligned region. Do not simplify this to masking `ptr` itself. BIG mappings retain
the required prefix and trim the excess outside their header/payload mapping.
Each BIG mapping has its own prefix header with `queue == NULL`, the returned
address and the mapping length. Alignment space is in that prefix; only the
requested payload size is reserved after the returned pointer.
A regular slab's header has a nonnull queue. Free unmaps the header's mapping
base, not its payload address. BIG usable capacity excludes the retained prefix.

Ordinary allocation alignment and small size-class spacing are 16 bytes. Cache
lines align queue metadata, not every tiny payload. Page-sized RUN classes keep
page-aligned slot bases. Explicit alignment reserves sufficient displacement
inside the selected slot; index recovery floors the returned pointer's offset
from the payload base by the class stride. Usable size subtracts that displacement.

Compile-time checks cover fundamental alignment, header size, slab alignment,
summary coverage, slot-counter capacity and cache-line queue size. Geometry is
word-size dependent; only Linux x86-64 has been exercised in this investigation.
The library is a startup interposer: preload or link it at process startup.
Its TLS pointer uses the initial-exec model. Live heap swapping, late allocator
injection and mixing pointers between allocator instances are outside this model.

## Bitmaps and queue invariants

1. Set shared leaf bits identify available slots. `bits` and `wordmap` summarize
   nonempty shared leaves. Leaf zero's top bit is a head lease, never a slot:
   physical slot 63 is unused on x86-64. Mask the lease before testing capacity,
   selecting a slot, or deciding that a leaf is full.
2. `busy` counts non-full leaves **excluding leaf zero**, under the queue lock.
   It changes only when a leaf leaves/returns to its all-free state. Together
   with leaf zero's valid mask it detects a wholly free shared slab without
   scanning the bitmap. `max` bounds physical slot indices, including the
   reserved lease position; it is not a count of user-visible slots.
3. Each retained queue's current head has an owner-only `local` bitmap for
   leaf-zero slots. A locked refill removes a batch from the shared bitmap,
   leaves one shared slot available, and returns the remainder through `local`.
   Owner allocations pop a local bit; owner frees return a bit to `local`.
   Neither operation needs a locked instruction. Foreign threads never access
   `local`; they publish to the shared leaf. Queue-address membership in the
   current TLS table identifies owner frees without an allocation header.
4. Available queues contain slabs with nonzero shared availability. Full slabs
   stay owned/count in `nbuf`, but unlink. A retained head cannot be removed by
   a foreign free, even if empty. Only its allocating thread retires that head,
   after exhausting `local`; promotion/removal updates the lease bit under the
   queue lock. Queues with a zero local retention limit have no leased head and
   use the locked allocation path. This lease protects the pointer-to-bitmap
   interval without per-allocation references. A cached bit is reserved from
   the shared bitmap until its owner uses it or exits.
5. Bit zero of `queue->head` locks summaries, leaf zero/full transitions, queue
   membership and release. Interior foreign frees publish one bit with a
   release CAS. A failed CAS retries against the new leaf; a zero leaf or final
   free in an ordinary leaf takes the queue lock. Leaf zero's final shared free
   may use CAS while its lease remains set. Shared takes synchronize with free
   publication. No fast free accesses its header after its successful CAS.
   Head accesses are atomic because contenders access the same word; unlock
   release-stores the untagged head. Shared leaf accesses are atomic even under
   the queue lock because interior frees do not take that lock.
6. On glibc 2.32+, the documented `__libc_single_threaded` indicator permits
   queue/leaf updates without a locked RMW while definitely single threaded.
   The same bits and publication state are maintained. Never write or override
   libc's indicator; other targets always use the shared atomic operations.
7. A live allocation keeps its header, mapping and queue alive until it returns
   its bit. Private-cache frees keep the head lease; shared frees finish all
   accesses before publication can permit another thread to reclaim the slab.
   Only a wholly free, unleased slab can transfer or unmap.
8. Global reserves contain only empty slabs. Nested queue order is local then
   global on release. Allocation detaches from global and unlocks it before
   acquiring local. The detached empty slab is private during transfer.
9. Cross-thread frees operate directly on the owning slab. They need no work
   from the allocating thread, including after it exits. They may spin waiting
   for a lock holder; this allocator is not wait-free.

A per-buffer lock alone does not protect selection followed by locking. Keep
the head lease and the lifetime argument together when changing this interval.

## Empty memory and zero-copy scope

Retained local slabs keep their backing pages: returning an allocation to its
bitmap does not discard those pages or force their next user to fault them in.
Per-class slab-count limits still bound the local reserve. A leased current
head is retained until its owner exhausts it; this can temporarily retain one
extra empty head while other owned slabs are live.

On an actual ownership release, surplus slabs unmap or enter the global reserve.
Before global transfer, `MADV_DONTNEED` discards every payload page after the
header page. Preserve caller errno if this optional discard fails, such as on
an mlocked page. Global reserves may retain that payload when discard fails;
no live payload or header page may be discarded.

A pthread key registers retirement once per allocator-using thread. The exiting
owner clears its head leases, publishes remaining private bits, and marks each
queue retired under its lock. Its empty slabs leave immediately. A retired
queue has zero local reserve, so live slabs leave on their final remote free.
Full slabs need no registry or owner-side draining to finish later.

TLS queue mappings still persist so outstanding cross-thread allocations retain
valid owners. Complete TLS metadata reclamation is not implemented. If pthread
key creation/registration fails, allocation remains usable but exit reclamation
is forfeited. Active workers can retain more resident payload between bursts
than the earlier eager-discard policy; thread exit now reclaims their caches.
Virtual reservations, peak RSS, sampled RSS and idle retention are distinct.

Allocation indexing, ownership recovery, free, whole-slab transfers and empty
page discards do not copy live payloads. Growing `realloc` can still require a new
slot and a payload copy; arbitrary reallocations do not have a zero-copy promise.

## Changing and validating the design

Start from a concrete invariant violation or measured cost. Explain why existing
address geometry, bitmaps and ownership do not already solve it before adding
metadata, caches, counters or synchronization. Keep the representation and its
lifetime proof together. Global queues initialize before release/acquire
publication of the ready flag; TLS initialization is thread-local.
The shim checks global readiness only when creating a thread's TLS. Nonnull
`t_memtls` proves global initialization already completed, including during
recursive allocations from pthread-key setup. Engine `memget` requires that
initialization and does not repeat it. In-place `realloc` and `realloc(ptr, 0)`
do not initialize a new allocating-thread context. Existing calloc zero/overflow
and posix_memalign/memalign alignment rejections precede thread setup.

The generic tabhash utility owns its lock-bit constants and does not depend on
the allocator header. Removed payload-based unmap/page-index macros must not
be resurrected: slabs release their aligned header base, not `buf->adr`.

Run `usr/lib/c/build-malloc.sh` and `usr/lib/c/test-malloc.sh` at affected
optimization levels. Coverage includes content/overlap, alignment and whole
reported capacity, realloc growth, eight-thread churn, cross-thread frees,
parked owners, and allocating before thread creation while workers free and the
owner continues allocating. Keep alignment tests across slab/BIG thresholds and
large explicit alignments, and errno preservation when reclamation meets a
locked payload page after its owner exits. Exercise owner reuse while a peer
frees the same slab's earlier allocations. The general tabhash utility remains independently
tested, but is not allocator metadata.

Compare against the same Perl build with fixed hash settings and rotated order
over multiple rounds; retain raw observations. Run timings after tests finish.
Measure both sampled live RSS and worker/burst retention. For Perl integration,
run its full harness as well as focused cases. Document tested configurations
and skips; extend to debugging/RC-stack variants and real applications before
claiming those results. Update this guide when representation or invariants change.

A Perl binary built with ithreads does not make its workload concurrent. Use
`bench-malloc.py --threads 4 --count 75000` for actual worker threads. Its partial
free/refill checkpoints retain interleaved payloads and pool capacities, then
separately measure full payload cleanup and worker exit. Perl also reuses its
own arenas; these measurements do not attribute every reused slot to malloc.

The single-thread indicator follows the
[glibc execution-mode contract](https://sourceware.org/glibc/manual/2.44/html_node/Single_002dThreaded.html).

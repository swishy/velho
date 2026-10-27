# Zen allocator design and maintenance

This describes the maintained implementation in `usr/lib/zen/mem.c`,
`usr/lib/zen/mem.h` and `usr/lib/c/malloc.c`. Read it before changing allocation,
free, alignment or ownership handling. Dated reports describe earlier versions;
[CI and releases](allocator-ci.md) documents automated checks.

The working mandate is concise pointer/bit arithmetic, little metadata and
minimal work per allocation. It follows the project's reconstruction of Vendu's
intentions, not a recovered original specification. Prefer deriving state from
addresses and existing ownership. Add state for a demonstrated cost or invariant,
and explain its lifetime. Keep independently measurable changes reviewable.

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

1. A set leaf bit means a free slot. `bits` identifies nonempty summary words;
   `wordmap` identifies nonempty leaf words. Allocation follows three `ctz`
   selections, clears a leaf bit, and clears empty summaries upward. Free sets
   the corresponding bits. The header is initialized from zero-filled mappings.
2. `nfree` caches the leaf population; `max` is capacity. Both are protected by
   the same owner lock as the bitmaps. The counter avoids scanning whole
   bitmaps to recognize the last free. It is not a second synchronization
   protocol: no atomic counter, CAS loop or separate seal accompanies it.
3. Available queues contain exactly owned slabs with nonzero free capacity.
   Full slabs stay owned and counted in `nbuf` but are unlinked. A head serves
   allocations until exhausted; its last successful take unlinks it. The first
   subsequent free appends it exactly once.
4. Bit zero of `queue->head` locks selection, bitmap/counter changes, membership
   and release. Mask the bit on head reads and preserve it on writes. Head
   accesses are atomic because contenders access the same word. Unlock
   release-stores the untagged head.
5. On glibc 2.32+, the documented `__libc_single_threaded` indicator permits
   acquisition without a locked RMW while execution is definitely single
   threaded. The same head lock bit is still set and cleared. When libc cannot
   prove single-threaded execution, use the ordinary bit lock. Never write or
   override libc's indicator; other targets always use the ordinary lock.
6. An outstanding allocation keeps its embedded header, mapping and queue alive
   until its free bit is set under that queue lock. Only the last free can
   transfer or unmap the slab. After release, do not dereference its header.
7. Global reserves contain only empty slabs. Nested queue order is local then
   global on release. Allocation detaches from global and unlocks it before
   acquiring local. The detached empty slab is private during transfer.
8. Cross-thread frees operate directly on the owning slab. They need no work
   from the allocating thread, including after it exits. They may spin waiting
   for a lock holder; this allocator is not wait-free.

A per-buffer lock is not a drop-in replacement for queue locking: selecting a
header and then locking it must prevent intervening removal/reuse. Any such
change needs a lifetime argument for that interval.

## Empty memory and zero-copy scope

Larger mappings amortize kernel operations but must not turn every empty cache
entry into a resident 256 KiB allocation. Empty retained slabs keep a small hot
payload window; pages beyond it are discarded with `MADV_DONTNEED` under the
owner lock. The header stays mapped and resident. `high` records the highest
slot issued since purging, avoiding a syscall when the window was not exceeded.
Preserve caller errno if optional reclamation fails (for example on locked
payload pages). Only reset the mark after successful discard; never discard header pages or
pages containing live slots. The window depends on the class stride and retains
at least one slot. Completely surplus slabs are unmapped normally.

The TLS queue mappings still persist after worker exit so outstanding cross-thread
allocations retain valid owners. This change bounds cached payload residency;
it does not implement complete TLS lifetime reclamation. Virtual reservations,
RSS, live payload and post-burst retention are different measurements.

Allocation indexing, ownership recovery, free, whole-slab transfers and empty
page discards do not copy live payloads. Growing `realloc` can still require a new
slot and a payload copy; arbitrary reallocations do not have a zero-copy promise.

## Changing and validating the design

Start from a concrete invariant violation or measured cost. Explain why existing
address geometry, bitmaps and ownership do not already solve it before adding
metadata, caches, counters or synchronization. Keep the representation and its
lifetime proof together. Global queues initialize before release/acquire
publication of the ready flag; TLS initialization is thread-local.

Run `usr/lib/c/build-malloc.sh` and `usr/lib/c/test-malloc.sh` at affected
optimization levels. Coverage includes content/overlap, alignment and whole
reported capacity, realloc growth, eight-thread churn, cross-thread frees,
parked owners, and allocating before thread creation while workers free and the
owner continues allocating. Keep alignment tests across slab/BIG thresholds and
large explicit alignments, and errno preservation when reclamation meets a
locked payload page. The general tabhash utility remains independently
tested, but is not allocator metadata.

Compare against the same Perl build with fixed hash settings and rotated order
over multiple rounds; retain raw observations. Run timings after tests finish.
Measure both sampled live RSS and worker/burst retention. For Perl integration,
run its full harness as well as focused cases. Document tested configurations
and skips; extend to debugging/RC-stack variants and real applications before
claiming those results. Update this guide when representation or invariants change.

[Direct-owner investigation](allocator-direct-owner-2026-10-27.md) records the
profiling, alternatives, comparisons and validation. The earlier
[bitmap adoption report](bit-lock-adoption-2026-10-27.md) is historical evidence
for the page-hash version, not the current ownership representation.

The single-thread indicator follows the
[glibc execution-mode contract](https://sourceware.org/glibc/manual/2.44/html_node/Single_002dThreaded.html).

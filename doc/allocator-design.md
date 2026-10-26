# Zen allocator design and maintenance

This is the current design of `usr/lib/zen/mem.c`, `usr/lib/zen/mem.h` and the
libc interposer in `usr/lib/c/malloc.c`. Read it before changing these paths.
The dated review and benchmark reports record experiments; this document
describes the maintained approach.

[CI and release documentation](allocator-ci.md) describes the automated checks,
commit-specific main releases and local test runner.

The working mandate is concise pointer/bit arithmetic, little metadata and
minimal work per allocation. This follows the project's reconstruction of
Vendu's intentions; it is not a recovered original specification. Prefer
removing redundant work and deriving state from existing words. Introduce
additional state only for a demonstrated requirement, with its ownership and
lifetime explained.

## Allocation representation

BLK and RUN slabs use one `uintptr_t` bitmap. A set bit means a free slot;
a clear bit means an allocation is outstanding. Allocation selects `ctz(bits)`
and clears that bit. Free derives the slot by flooring the returned pointer's
displacement from the slab base by the class stride, then sets its bit.
An explicitly aligned pointer remains inside its original slot; usable size
subtracts that displacement. There is no per-allocation header or delta table.

Page metadata contains two words: starting page plus active reference count,
and slab-header pointer plus class/type tags. Free consumes the metadata
returned by `tabhashrm`; it does not perform another ownership lookup. Every
allocation adds one page reference and every free removes one. BIG allocations
store their mapping length and type instead of a slab-header pointer; alignment
prefixes are trimmed so their returned page is also the mapping base.

The class uses six low bits and the type uses two, requiring a 256-byte header
stride. The header is separate from payload memory. Both slab types share one
header pool. Current layout checks enforce class/tag fit, header alignment and
size, and one cache line per queue. Slab-tier checks prove that every bitmap has
between one and word-bits slots. Current 32-bit BLK geometry fails that proof;
change the geometry and validate it before enabling that configuration.

Slot allocation, free, header packing and whole-slab transfers do not copy live
payloads. The libc shim still copies when a growing `realloc` needs a new slot;
the design does not promise zero copies for arbitrary reallocations.

## Queue and lifetime invariants

1. A local available queue contains exactly its owned slabs with nonzero free
   bitmaps. Full slabs remain owned and counted in `nbuf`, but are unlinked.
   The head serves allocations until exhausted. The last successful take
   unlinks it immediately; the first subsequent free appends it exactly once.
2. Bit zero of the queue head is its lock. Hold it across head selection,
   bitmap changes, list changes and empty-slab release. Mask the bit on head
   reads and preserve it on head writes. Head accesses remain atomic even
   while locked because contenders also access that word. Unlock uses a
   release store of the untagged head.
3. An outstanding allocation's clear bit keeps its slab and header alive.
   A freeing thread can obtain the owner and compute its index before taking
   the queue lock because its own bit has not yet been set. After marking the
   last free, release may recycle the header: do not access that header after
   `memreleaseempty` returns.
4. A global reserve contains only completely empty slabs. Only the last free
   can retain, transfer or unmap a slab. Bitmap marking and release occur under
   the same owner lock, so no separate seal or bitmap CAS is needed.
5. Nested queue locking is local then global on release. Allocation detaches
   from global and unlocks it before taking local. Preserve this ordering.
   The detached empty slab is private during the transfer.
6. Cross-thread frees mark the owning slab directly. They require no work from
   the allocating thread, including after that thread exits. Queue locks can
   spin while another thread holds them: this is not a wait-free allocator.

A per-buffer lock is not a drop-in replacement for queue locking. Selecting a
header and then locking it must also prevent intervening removal and reuse.
Any such change needs a lifetime argument covering that interval.

TLS and header-pool mappings currently persist. Empty payload slabs follow the
local/global retention limits. Keep these distinct when evaluating thread churn
and RSS; the existing measurements do not establish thread-exit reclamation.

## Changing the implementation

Start from a concrete missed operation, invariant violation or measured cost.
Explain why the current owner lock, bitmap or page reference does not already
solve it before adding counters, seals, caches, remote-free queues or another
synchronization layer. The bitmap is ordinary state protected by its queue;
extra locked RMWs inside that critical section duplicate synchronization.

Keep queue selection and header lifetime in the same reasoning as bit updates.
Avoid speculative unlocked head reads or publishing a head with its lock bit
accidentally cleared. Global queue IDs and geometry initialize together before
the release/acquire ready flag is published; TLS initialization is thread-local.

Per-allocation page-hash traffic is still a profiling candidate. An earlier
slab-lifetime metadata/cache experiment produced stale ownership metadata and
was reverted. Any replacement must explain removal, address reuse and lookup
lifetime, and pass the same content checks before performance claims are made.

For allocator changes, run `usr/lib/c/build-malloc.sh` and the hash, eight-stage
stress, alignment/capacity, eight-thread churn, cross-thread free and parked-owner
checks at the affected optimization levels. The
[adoption report](bit-lock-adoption-2026-10-27.md#validation) gives commands and
the tested Perl subset. Test actual overlap, content preservation, release and
cross-thread progress rather than private struct layouts.

For performance changes, compare the same Perl build and workload with fixed
hash settings, rotate allocator order over multiple rounds, and retain raw
observations. Run benchmarks after stress tests finish. Report both time and
memory, specify sampled versus peak RSS, and identify the tested scope. Update
this note if the representation or invariants change.

## Evidence

- [Initial review and prototype](bit-lock-review-2026-10-27.md).
- [Adopted implementation, validation and measurements](bit-lock-adoption-2026-10-27.md).
- [Raw nine-round adoption observations](bit-lock-adoption-2026-10-27-results.json).

The adoption comparison measured 31.0% lower sampled RSS and 8.8% less process
time than `dcb3dac` on the recorded Perl churn workload. Those are workload
results, not general throughput or scalability guarantees.

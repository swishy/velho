# Bit-lock bitmap adoption — 27 October 2026

Applied the reviewed bitmap/packed-header prototype on top of `dcb3dac`, then
removed the implementation and diagnostic state it made obsolete. This is a
change on `mt-resurrection`. The maintained approach and contributor guidance
are documented in [allocator design and maintenance](allocator-design.md).

## Implementation

BLK and RUN now use the same one-word free bitmap. Allocation selects the first
free bit and dequeues a slab on its last successful take. Free derives its index
from the pointer displacement, sets that bit, and requeues on the zero-to-nonzero
transition. The queue head's low bit protects selection, bitmap changes and slab
lifetime. Head reads and writes use relaxed atomic accesses while locked because
contending threads also access that word; unlocking publishes the untagged head
with a release store. There are no extra locked bitmap RMWs, seals or retry loops.

A header occupies a 256-byte stride: six class bits and two type bits are packed
into its pointer. The live structure is 56 bytes on this x86-64 build. Payload
geometry and alignment are unchanged, and this change does not move or copy live
payloads. Both allocation types now share one header pool and one refill path.

Removed:

- The offset/free-pointer stack, sealing constant, separate queue mutex and unused
  queue-to-TLS pointer; queues remain cache-line aligned.
- Dead `memgetptr`, `memfindbuf`, `memresize` declaration, old configuration/tag
  macros, duplicate TLS global, and unused `zen/bits/mem.h` allocator.
- The unused re-entry logger and its build dependencies, and hard-coded private
  allocator structs/register-dereferencing signal diagnostics in the stress test.
  The stress test retains its content/overlap checks and alarm timeout.
- Redundant per-thread CAS publication and repeated global queue wiring. Queue
  IDs and geometry initialize together under the existing initialization lock;
  the ready flag uses release/acquire publication. TLS assignment is thread-local.
- A duplicate empty macro definition in `zero/trix.h`, the implicit return type
  on `shim_tls_init`, and a redundant null comparison for the nonnull
  `posix_memalign` output parameter.

The three main files shrink from 1,154 to 837 lines, including the new layout
checks and invariant comments. All six slab tiers are checked at compile time
for 1..word-bits slots; class tags, header stride and queue size are also checked.
These checks add no allocation-path branches. Current 32-bit BLK geometry does
not fit one word and is deliberately rejected at compilation; this change has
only been exercised on x86-64.

## Validation

Final source builds without warnings at O0, O2 and O3. The engine and shim also
compile with `-Wall -Wextra -Werror` at O2.

At all three optimization levels:

- Page sharing, collision-chain and subtable-recycling checks pass.
- All eight allocator stress stages pass.
- Alignment, usable capacity, overlap, realloc growth and reuse checks pass.
- Eight threads × 150,000 local-churn iterations pass.
- 480,000 cross-thread frees complete with zero reported failures.
- Parked/exited-owner frees complete without owner assistance. The worker-index
  bug is fixed, and the test now checks that all 100,000 pointers were processed.
- The separate 200,000-handoff producer/consumer reproducer also passes.

The local-churn, cross-thread, parked-owner and handoff tests additionally passed
three rounds at O2 before the final warning/atomic-head-access cleanup.

The final O2 interposer passes the Perl `opbasic`, `op` and `perf` subset:
**237 files, 70,417 assertions**. This is not a full Perl suite or Firefox run.
Direct free still uses a spinlock, so it may wait for a lock holder; it does not
require the allocating thread to drain a remote-free queue. TLS and header-pool
mappings still persist, so this is not a thread-churn reclamation change.

Build and run the checked-in tests from the Velho root:

```sh
usr/lib/c/build-malloc.sh /tmp/velho-malloc-build -O2
/tmp/velho-malloc-build/test-tabhash
timeout 90 /tmp/velho-malloc-build/malloc-stress
timeout 90 env LD_PRELOAD=/tmp/velho-malloc-build/libvelhomalloc.so \
    /tmp/velho-malloc-build/malloc-alignment-test
timeout 90 /tmp/velho-malloc-build/malloc-mtstress
timeout 90 /tmp/velho-malloc-build/malloc-xfree-mt
timeout 90 /tmp/velho-malloc-build/malloc-parked-test
```

Validation builds/logs: `/tmp/velho-adopt/{O0,O2,O3}`, with Perl output in
`/tmp/velho-adopt/perl.log`. The independent handoff reproducer remains at
`/tmp/velho-review2/remote-free.c`; it is additional local evidence, not part of
the checked-in suite.

## Benchmark

Final-source observations are recorded in
[the adoption results](bit-lock-adoption-2026-10-27-results.json). Nine rounds
rotate glibc, the pre-patch `dcb3dac` interposer and the cleaned O2 interposer,
using the same threaded Perl and fixed hash seed. Tests were finished before
starting the benchmark. RSS is sampled after string sorting, not peak RSS.

Baseline library SHA-256:
`080964a485fabbf24ebd5f6cb58c39fef33aea2c237dff89b4778f9e705e17f9`.
Final O2 library SHA-256:
`390d1f379964a92c446a033adf10284c330942f9181a327c47ac18c977cda05c`.

Nine-round medians, pre-patch → cleaned implementation:

- Hash construction: **486 → 435 ms**.
- Scan: **54 → 53 ms**.
- Strings/sort: **145 → 137 ms**.
- Whole process: **1.249 → 1.139 s**, 8.8% less time.
- Sampled RSS: **493,252 → 340,340 KiB**, 31.0% lower.

The matching glibc medians were 283/52/118 ms,
0.880 s overall and 263,980 KiB. The cleaned implementation remains
approximately 1.29× glibc's time and 1.29× its sampled RSS.
The memory reduction agrees with the earlier prototype batches; timing remains
workload-specific and varied across batches. Per-allocation page-hash traffic
is unchanged and remains the next profiling candidate.

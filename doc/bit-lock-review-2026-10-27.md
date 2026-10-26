# Bit-lock review — 27 October 2026

Reviewed `dcb3dac8d875c97f0f55a7782249f23ec5a11150`. This records the initial
prototype review and measurements, made before changing production sources.
The patch has since been applied and cleaned up; see the
[adoption record](bit-lock-adoption-2026-10-27.md) for the final implementation
and its validation. The original experimental patch is retained for provenance.

## Findings in the current path

1. Allocation and free both hold `queue->mtx` while modifying `ofs`, `bits`,
   the free stack and list membership (`mem.c:299`, `mem.c:493`). Their additional
   atomic fetch-add/sub/or and compare-exchange operations do not supply mutual
   exclusion beyond that lock. Likewise, sealing cannot race a take while both
   hold this same lock. These are overlapping synchronization mechanisms.
2. The BLK path does not dequeue on its last successful take. Its next take
   increments an already exhausted counter, undoes the increment, dequeues,
   unlocks, and retries. The exhaustion transition can happen at the successful
   take instead (`mem.c:306–316`).
3. The initial `queue->head` read at line 269 is outside the queue lock, while
   cross-thread frees can modify that head. Passing stress does not make that
   unsynchronized C access valid. The prototype reads and changes it under its
   pointer bit lock.
4. Current x86-64 BLK slabs contain at most 64 blocks: the smallest class has
   4096/64 = 64; the larger-slab classes have fewer. RUN slabs already fit one
   word. A bitmap can therefore represent both without a separate block stack.
5. Page alignment is stronger than the packed header value requires. There are
   64 class indices, so six bits hold `qid`; two more hold the type. A 256-byte
   header stride leaves all eight bits available. With the block stack removed,
   the current 64-byte fixed header fits comfortably in that stride.

The existing `mtlkfmtx` is already a small spinlock, not a pthread mutex.
Changing its name or using a bit instruction alone is not a compelling speed
argument. The useful changes remove state, duplicate operations and unused
header space.

## Prototype

- Low bit of `queue->head` is the queue lock. While held, every head read masks
  the bit and every head update preserves it. Unlock release-stores the new
  untagged head, as the hash buckets already do.
- Allocation takes `ctz(bits)` and clears that bit. It removes the buffer from
  the available queue immediately when no free bits remain.
- Free computes the index from pointer displacement and size-class stride,
  sets its bit, and requeues only on the zero-to-nonzero transition.
- Queue lock covers selection, bitmap change, membership and empty-buffer
  release. The atomic offset, CAS retry and sealing protocol disappear.
- Global queues still contain only empty buffers. Only the final free can
  transfer/unmap a buffer: other outstanding blocks still have clear bits.
  Global reserve selection releases its lock before taking a local queue lock;
  release takes local then global. There is no reverse nested acquisition.
- Header alignment becomes 256 bytes; payload alignment is unchanged. Packing
  does not move or copy live payloads. Hash page-reference machinery is unchanged.

This is a blocking spinlock design: a preempted lock holder can delay another
thread. Direct cross-thread free and parked-owner progress do not mean wait-free.
A per-buffer lock would need a separate proof that selecting a buffer and then
locking it cannot race its removal/reuse; merely replacing the queue lock with
that lock would leave a lifetime hole.

At review time the prototype retained unused struct fields for the existing diagnostic ABI and
used compact experimental source formatting. Adoption removed those fields,
normalized the source, added compile-time configuration assertions and updated
the header-layout documentation. The evidence here is for the
current x86-64 configuration, not all supported architectures.

## Measurements

Same threaded Perl build, fixed hash settings, unchanged 300k-entry churn
workload. The confirmation run rotated three allocators over nine rounds.
Medians for current code → bit-lock/bitmap/packed-header prototype:

- Hash build: **460 → 408 ms**, 11.3% less time.
- Scan: **51 → 51 ms**.
- Strings plus sort: **141 → 134 ms**, 5.0% less time.
- Whole process: **1.206 → 1.126 s**, 6.7% less time.
- Post-sort RSS: **493,268 → 340,312 KiB**, 31.0% lower.
- glibc: **0.858 s** whole process and **263,944 KiB** sampled RSS. The prototype
  remains approximately **1.31×** glibc's time and **1.29×** its sampled RSS.

The first five-round comparison found little whole-process improvement
(1.196 → 1.184 s). Its bitmap-only variant, retaining 4 KiB headers, also did not
establish an overall speed win. Both batches consistently showed the packed
header's memory reduction. Treat timing as a modest, workload-specific result;
this does not close the runtime gap or isolate all causes of that gap.

Raw observations: [confirmation](bit-lock-review-2026-10-27-results.json) and
[first batch](bit-lock-review-2026-10-27-first-results.json).

## Validation performed

- Plain bit-lock/bitmap variant: eight-stage stress and alignment test pass;
  eight-thread local churn, 480k cross-thread frees, parked-owner test and a
  separate concurrent producer/consumer free test each pass three runs at O2.
- Packed-header variant: eight-stage stress, alignment/usable-capacity/realloc,
  eight-thread churn and 480k cross-thread frees pass at O0, O2 and O3. The
  parked-owner and separate producer/consumer tests also pass at O2.
- Packed O2 library: **237 Perl files / 70,417 assertions pass**, covering
  `opbasic`, `op` and `perf`. This is not a full Perl suite or Firefox test.
- The existing parked-owner test misses allocation index zero because its
  worker IDs start at one. Its passing result is a progress check, not proof
  of complete reclamation; the separate producer/consumer test checks every
  handoff in its 200k-allocation sequence.

[Experimental patch](bit-lock-prototype-2026-10-27.patch). It applies to the
reviewed commit. The working allocator now includes it plus the cleanup recorded
in the adoption report; do not reapply this historical patch.

The remaining per-allocation page-hash operations are untouched. Profile those
next before claiming that further changes to lock representation will remove
the remaining gap. The failed slab-lifetime cache experiment mentioned in
FINDINGS needs its stale-entry bug resolved independently; this prototype does
not depend on it.

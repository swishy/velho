# Allocator restoration, 26 October 2026

Starting revision: `e738a4172563d2070a85ad1fa6ba2e08e1d9c5b1`
(`mt-resurrection`). The changes are in the working tree.

The aim of this pass is to complete the surviving pointer/size-class design
and remove reconstruction workarounds. Original author intent is inferred
from the existing code; no complete original design specification is available.

## Changes

- `tabhashaddref` looks up the page key, not the packed buffer value. The
  previous code produced two entries with count 1 for two allocations on the
  same page. The corrected code produces one entry with count 2.
- `tabhashputtab` publishes the returned table as the free-pool head. Previously
  it republished the old head and lost the returned table.
- Bucket lookup scans `ncur` populated entries. Moving a subtable to the front
  also repairs the previous head's back-link.
- Each size class has one queue of usable buffers. Its head supplies allocations
  until exhausted. The first free into an exhausted buffer appends it to the
  tail. `nbuf` counts all owned buffers, including exhausted ones off the queue.
- Only empty buffers enter the global reserve. Allocation transfers a whole
  reserved buffer into TLS once, instead of serving individual allocations
  repeatedly under the global queue lock.
- Removed available stacks, sticky-cursor arrays, availability flags/counts,
  queue walks, duplicate free workers, stale-page purges and the BIG range scan.
- Free uses the metadata returned by `tabhashrm` directly, eliminating its
  preceding `tabhashfind`. The last page reference is removed before the slab
  can be released.
- Allocation no longer calculates and stores alignment deltas. Free recovers
  the block/run index by integer division of its offset within the slab.
  Reciprocal tables are gone. Ordinary allocations need no alignment rounding.
- Alignment padding reserves enough capacity, and usable-size lookup subtracts
  the returned pointer's displacement within its block. Over-aligned BIG maps
  discard their unused prefix, making the returned pointer the mapping base.
- The unused half of block-header storage is gone. The maximum header footprint
  rounds to 4 KiB instead of 8 KiB on this x86-64 build. This is reserved header
  capacity, not a claim that every old header consumed 8 KiB resident memory.
- Size lookup performs one lookup; the 200,000-iteration retry/yield loop is gone.

No payload copying was added. Growing realloc still uses the existing copy
fallback; this pass does not implement remapping or in-place cross-class growth.

## Reproduction

From the repository root:

```sh
usr/lib/c/build-malloc.sh /tmp/velho-malloc-build -O2
/tmp/velho-malloc-build/test-tabhash
LD_PRELOAD=/tmp/velho-malloc-build/libvelhomalloc.so /tmp/velho-malloc-build/malloc-alignment-test
/tmp/velho-malloc-build/malloc-stress
```

The build script takes an output directory and optimization level. Repeat with
`-O0`, `-O1` and `-O3` to check the other builds. It includes the previously
out-of-tree recursive-lock diagnostic hook; it does not repair lock recursion.

Benchmark one Perl build with interleaved allocators:

```sh
python3 usr/lib/c/bench-malloc.py \
  --perl-root ../perl_investigation/perl-5.44.0-threads \
  --rounds 5 --output /tmp/velho-results.json \
  glibc= before=/tmp/velho-review/libvelho-before.so \
  after=/tmp/velho-review/final-v2/libvelhomalloc.so
```

The saved before-library was built from the starting revision before any edits,
using the same compiler, `-O2`, include tree and recursive-lock diagnostic hook.
The same threaded Perl executable is used throughout; no Perl source changes
are part of this pass. Hash seed and perturbation settings are fixed. The script
rotates run order and records every process's output, return code and wall time.
The phase timers exclude process teardown; process wall time includes it.
RSS is sampled after sorting and is in KiB, not peak RSS. The earlier temporary
benchmark mislabeled this number as MB; the checked-in script corrects the label.

## Validation scope

The allocator tests exercise shared-page reference counts, collision-chain
promotion/removal, table recycling, live payload preservation, randomized free
order, realloc growth/shrink and usable capacity. The alignment test fills every
reported usable byte of many live allocations and checks preservation across
realloc, for power-of-two alignments from 64 bytes through 1 MiB.

Multithread correctness remains unresolved. The existing eight-thread stress
fails on both the starting library and this implementation. This pass does not
add remote-free queues or repair the existing bit-lock acquisition protocol.

## Results

Five interleaved process runs per allocator, 300,000 nested-hash entries and
200,000 strings; medians, same threaded Perl 5.44.0 executable:

- Hash build: **666 → 466 ms**, 30.0% less time. glibc: 302 ms.
- Scan: **56 → 54 ms**; too small a difference here to establish a useful gain.
  glibc: 52 ms.
- String construction plus sort: **213 → 146 ms**, 31.5% less time.
  glibc: 119 ms.
- Whole process, including startup and destruction: **1.788 → 1.236 s**,
  30.9% less time (1.45× throughput for this fixed workload). glibc: 0.881 s.
- RSS after sort: **529,340 → 493,324 KiB**, 6.8% lower. glibc: 263,968 KiB.

Raw observations: [results JSON](allocator-restoration-2026-10-26-results.json).
This is a workload-specific improvement, not a claim of a general Perl speedup.
Velho is still 1.54× glibc's time in hash construction, 1.23× in strings/sort,
and approximately 1.87× its sampled RSS.

The engine shrank from 1,439 to 496 lines (943 fewer). This includes deletion of
comments and obsolete implementations as well as executed code; line count is
not a measurement of machine-code size or performance.

## Completed checks

- Hash reference/collision/recycling tests: pass at `-O0`, `-O1`, `-O2`, `-O3`.
- Eight-stage allocator stress: pass at all four optimization levels.
- Alignment/capacity/overlap/realloc tests (64 B through 1 MiB): pass at all four.
- Final library under the Perl harness: **237 files / 70,417 assertions pass**,
  covering `opbasic/*.t`, `op/*.t`, `perf/*.t`.
- `git diff --check`: clean.

The final Perl subset command, run from `perl-5.44.0-threads/t`, was:

```sh
LD_PRELOAD=/tmp/velho-review/final-v2/libvelhomalloc.so TEST_JOBS=4 PERL_SKIP_TTY_TEST=1 HARNESS_NOTTY=1 PERL_TEST_STATE_FILE=/tmp/velho-review/perl-op-state ./perl harness 'opbasic/*.t' 'op/*.t' 'perf/*.t'
```

A broader harness attempt on the preceding build was interrupted after a
threaded-regex failure and stalled progress. It also encountered a missing-TTY
failure; the final subset above sets the harness's TTY-skip variable. It is not
a full-core-suite pass. The standalone eight-thread stress reproduced failure
on both the starting and simplified versions (NULL reallocs/corruption). These
results do not establish multithread safety or complete malloc ABI conformance.

## Next measurement targets

The remaining gap should be profiled with the corrected page-reference and
queue paths. In particular, quantify metadata hash cost and actual versus
requested bytes before changing the representation or size classes. Header
packing and removal of unnecessary shared-table writes are plausible next
candidates; the measurements here do not isolate their potential benefit.

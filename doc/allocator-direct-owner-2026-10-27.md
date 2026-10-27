# Direct ownership investigation — 27 October 2026

Starting point: `9d6ef62`, the bitmap/packed-header allocator. This investigation
keeps the same Perl 5.44.0 threaded binary for allocator comparisons; the
separately ported blead core changes are not part of those timing comparisons.
The maintained representation is in [allocator-design.md](allocator-design.md).

## What was overlooked

The reconstructed path imposed cache-line alignment on tiny payloads, mapped
small slabs individually, and routed frees through global page metadata even
though aligned slab geometry can identify the owner directly.

Baseline diagnostic counters counted 2,089,287 allocations in the first 64-byte
class: 53,932,674 requested bytes and 133,714,368 bytes of slot capacity. Across
BLK/RUN classes there were 2,342,295 allocations and 40,761 new slab mappings.
These are cumulative counters, not simultaneous live bytes or sampled RSS.
Callgrind also showed substantial page-hash and lock work. Instrumented timings
were not used as performance results. The raw counter output is retained in the
[profile record](allocator-direct-owner-2026-10-27-profile.txt).

## Chosen implementation

Ordinary alignment/class spacing becomes 16 bytes. A hierarchical bitmap supports
larger slabs while retaining `ctz`/bit-clear allocation and bit-set free under the
queue-head bit lock. `nfree` caches the bitmap population for last-free detection.

A slab is aligned to its mapping size, with its header inside the mapping. Owner
recovery masks `(ptr - 1)` to that boundary. The class stride lives in the owner
queue. This removes the ownership table, page references, packed class tags and
separate header pool. BIG allocations retain a header prefix compatible with
the same decoding rule, including pointers exactly on an alignment boundary.

On x86-64 the selected mapping size is 256 KiB. Compared alternatives included
16-byte classes retaining the page hash, mapping-lifetime hash entries, a sparse
radix page index, 64 KiB direct-owner slabs, ordinary TLS/locking, and startup TLS
with the documented glibc single-thread fast path. Those prototypes remain in
`/tmp/velho-next`; they are not parallel production implementations.

A nine-round pre-reclamation comparison found the 256 KiB direct-owner version
at 0.787 s / 240,092 KiB, against glibc at 0.883 s / 263,852 KiB and the starting
allocator at 1.136 s / 340,172 KiB. The 64 KiB version was 0.802 s / 244,928 KiB;
its ordinary-lock/general-TLS variant was 0.831 s / 244,968 KiB. These samples
motivated the selected geometry; final results are recorded separately below.

## Reclamation is part of the change

Larger cached slabs initially regressed worker-exit retention. Ten sequential
workers each allocating/filling/freeing 100,000 32-byte objects ended near
13,444 KiB, versus 4,212 KiB in the starting allocator and 7,280 KiB with glibc.
Consequently, empty retained slabs discard cold payload pages while keeping the
header and a class-sized hot window. A high-water slot avoids discards for
slabs that never exceeded that window. The same scenario then ended at
3,256 KiB. Process/OS layout can shift the absolute numbers; this is one
retention workload, not a general fragmentation guarantee.

[Raw worker-retention samples](allocator-direct-owner-2026-10-27-retention.txt).

Reproduce the diagnostic with the checked-in benchmark, compiled without an
allocator dependency so either allocator can be preloaded:

```sh
cc -std=gnu11 -O2 -fno-builtin -pthread usr/lib/c/bench-malloc-retention.c \
    -o /tmp/bench-malloc-retention
/tmp/bench-malloc-retention
LD_PRELOAD=/absolute/path/libvelhomalloc.so /tmp/bench-malloc-retention
```

## Final comparison

[Raw observations](allocator-direct-owner-2026-10-27-results.json) contain nine
rotating rounds of glibc, starting allocator and final direct-owner allocator.
Same Perl binary, fixed hash seed, 300,000-entry workload. The sampled RSS is
after sorting; it is not peak RSS. Tests and diagnostic tracing were not running
during the timing comparison.

Medians, starting allocator -> direct owner:

- Hash build: **430 -> 301 ms**.
- Scan: **53 -> 53 ms**.
- Strings/sort: **142 -> 123 ms**.
- Whole process: **1.198 -> 0.839 s**, 30.0% less time.
- Sampled RSS: **340,152 -> 239,992 KiB**, 29.4% lower.

Matching glibc medians: **307/54/123 ms**, **0.914 s**, **263,840 KiB**.
The final implementation used **8.2% less process time and 9.0% less sampled
RSS than glibc on this workload**. This is a measured workload result, not proof
that either allocator dominates all allocation patterns or applications.

## Syscalls and final validation

`strace -c` on the same 300k workload counted `brk`, `mmap`, `munmap` and
`madvise`. The previous allocator made **43,114 mmap + 39,297 munmap + 1 brk**
calls (82,412 total). The final allocator made **1,019 mmap + 1,245 munmap +
53 madvise + 1 brk** calls (2,318 total). Raw syscall summaries: [previous](allocator-direct-owner-2026-10-27-syscalls-previous.txt)
and [direct owner](allocator-direct-owner-2026-10-27-syscalls-direct.txt).
Tracer timings were not used for the
performance comparison. This confirms that batching physical mapping ownership
into aligned slabs removed work, rather than merely changing a lookup's spelling.

Final implementation:

- Standalone hash-utility checks, all eight allocator stress stages, extended
  16-byte through 1-MiB alignment/whole-capacity/realloc checks, eight-thread
  local churn, 480k cross-thread frees, parked owners, pre-thread allocations
  with concurrent owner reuse, and the system-Perl smoke test pass at O0/O2/O3.
- Engine/shim compile with `-Wall -Wextra -Werror` at O2.
- The final O2 interposer passes the complete downstream blead archive-build
  harness: **2,996 files / 1,435,571 assertions**, 236 seconds. Socket access was
  enabled. Archive builds skip Git-dependent porting checks. This establishes
  the tested threaded O2 configuration, not all architectures, debug/RC-stack
  variants or applications such as Firefox.
- The separate producer/consumer handoff reproducer passed on the direct-owner
  prototype before the cold-tail reclamation change. Final reclamation is
  additionally covered by the checked-in owner-transition/reuse test.

The implementation and sizeof/tag checks were normalized before final tests.
There is no retained page-table prototype in the production engine. Generic
`zen/tabhash.h` remains a separate utility with a standalone test configuration.
TLS queue contexts still persist; the cold-tail policy limits cached payload
residency but does not provide full thread-context reclamation.

Builds/logs: `/tmp/velho-next/ready/{O0,O2,O3}`, `perl-ready-full.log` and
`tests-ready*.log`. The comparison library was built from baseline `9d6ef62`.

Previous O2 library SHA-256: `390d1f379964a92c446a033adf10284c330942f9181a327c47ac18c977cda05c`.

Final O2 library SHA-256: `848dd40cb35b816f24e9092bc0c989e10110ea881996bab4adb869236c1ee20b`.

A focused locked-payload test exposed one reclamation error-path defect:
`MADV_DONTNEED` can fail on an mlocked page, changing caller errno through free.
The final implementation preserves errno around this optional syscall and keeps
the high-water mark on failure. The regression passes with glibc and at all
three optimization levels with Velho; it failed before the fix. No extra errno
save/restore was added to every ordinary allocation or free.

BIG allocations pass the original requested payload size to the mapping helper;
class-selection alignment padding is not reserved a second time as payload.
The retained prefix supplies alignment and the embedded header independently.

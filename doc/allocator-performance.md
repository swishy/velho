# Allocator performance and validation

Measurements recorded **28 October 2026**, Linux x86-64, GCC 13.3.0, glibc 2.39.
The measured O2 allocator has SHA-256
`3b57cbefb8c369e7f63d8759e0890e74ace3a54594fb55707da70ee35cb603b4`.
The [design guide](allocator-design.md) defines the maintained representation;
the [CI guide](allocator-ci.md) covers building, testing and releases.

## Current comparison

Nine rounds rotate glibc, pre-cleanup Velho and current Velho. Builds and tests
finished before timing. Perl comparisons use the **same unmodified Perl 5.44.0
binary** with thread support, `-O2`, Perl's own malloc disabled, and fixed hash
settings. The separately optimized blead core is not part of these timings.

Median whole-process time, **glibc -> current Velho**:

- Single-thread Perl, 300,000 entries: **0.845 -> 0.770 s**, 8.9% less time.
- Four-worker Perl, 75,000 entries each, including partial free/refill and
  teardown: **0.785 -> 0.626 s**, 20.3% less time.
- Eight-thread C allocation/reallocation churn: **0.243 -> 0.234 s**.

The cleanup alone changes the single-thread median from 0.780 to 0.770 s,
threaded Perl from 0.625 to 0.626 s, and threaded C from 0.244 to 0.234 s.
These are small effects with overlapping run ranges. They do not establish a
general speedup from the cleanup.

## Memory and reuse

Single-thread Perl samples RSS after sorting: **263,892 -> 239,768 KiB**, 9.1%
lower with Velho. This is a live checkpoint, not peak RSS, and that workload
does not create threads merely because its Perl binary supports them.

The actual threaded workload creates four independent Perl interpreters and
stops every worker at each checkpoint. RSS in KiB, **glibc -> current Velho**:

- All payloads live: **267,712 -> 247,464**.
- Half the interleaved payloads freed: **267,712 -> 247,468**.
- Missing payloads replaced and verified: **279,568 -> 263,264**.
- Payload scope exited, workers still alive: **279,568 -> 207,104**.
- Workers exited and joined: **251,644 -> 11,616**.

Partial free keeps odd hash entries, alternating strings, hash buckets and array
capacities alive. Sorted aliases are cleared so selected strings can actually
be released. Refill then recreates the missing entries before full teardown.
Both full checkpoints use identical graph/string verification routines.

This exercises holes in occupied allocation pools. It does not inspect every
slab's occupancy, and Perl's own arenas also contribute to reuse. RSS barely
changes on partial free; the freed space remains available within the process.
Refill increases Velho's RSS by 6.4% and glibc's by 4.4%. Velho remains 7.6%
smaller with the original payload live and 5.8% smaller after refill, but these
results do not establish zero growth or universally better hole reuse.

The C churn workload has a different allocation/lifetime distribution. Its
**peak RSS is 4,748 KiB with glibc and 48,428 KiB with Velho**. Retaining backing
for local reuse trades active-worker residency for less reclamation/reuse work.
Perl's lower live RSS does not imply a memory advantage on every workload.
Peak RSS, occupied-pool RSS, idle workers and joined workers are distinct.

## Validation

The measured library passes nine standalone C checks and both single-thread and
four-worker Perl smoke workloads at O0/O2/O3. Coverage includes alignment and
usable capacity, realloc, threaded churn, remote frees, exited owners, transition
from single-thread execution, concurrent slab reuse, and errno preservation when
ownership-release reclamation encounters locked pages. The independent tabhash
test also passes without allocator-header dependencies.

Musl 1.2.6 builds pass the native C checks and threaded Perl smoke. Current
musl performance is not claimed from the glibc measurements above.

The optimized blead 5.45.5 threaded build passes its full core harness with the
measured allocator: **2,996 files / 1,435,560 tests**, 234 seconds. TTY tests are
disabled; Git-dependent porting checks skip in the archive build. This is
integration validation, separate from the unmodified 5.44.0 performance build.
The library and generic hash check compile with `-Wall -Wextra -Werror`;
actionlint passes. CI smoke tests are not this full core harness.

## Reproduction and evidence

Build and run checks using the [CI guide](allocator-ci.md). For timing, select
one Perl build and an absolute path to a library built for its libc:

```sh
python3 usr/lib/c/bench-malloc.py \
  --perl-root /path/to/perl-5.44.0-threads \
  --rounds 9 --count 300000 --output /tmp/perl-single.json \
  glibc= velho=/absolute/path/libvelhomalloc.so

python3 usr/lib/c/bench-malloc.py \
  --perl-root /path/to/perl-5.44.0-threads \
  --threads 4 --count 75000 --rounds 9 --output /tmp/perl-threaded.json \
  glibc= velho=/absolute/path/libvelhomalloc.so
```

`--count` is per worker in threaded mode. Phase times are mean worker times;
`seconds` is whole-process elapsed time. The driver fixes `PERL_HASH_SEED=42`
and `PERL_PERTURB_KEYS=0`, rotates variant order and saves raw output.

Compile the C workload without allocator linkage, then select the interposer:

```sh
cc -std=gnu11 -O2 -fno-builtin -pthread usr/lib/c/malloc-mtstress.c \
  -o /tmp/velho-mtstress
/usr/bin/time -f '%e s; %M KiB peak RSS' /tmp/velho-mtstress
/usr/bin/time -f '%e s; %M KiB peak RSS' \
  env LD_PRELOAD=/absolute/path/libvelhomalloc.so /tmp/velho-mtstress
```

Repeat in rotating order. The retained C observations time the wrapper process
from Python and use `/usr/bin/time` for peak RSS; its `%e` is a child elapsed
measurement, so it need not match the recorded `seconds` exactly.

Latest evidence, 81 successful observations plus validation output:

- [Single-thread Perl](benchmarks/2026-10-28/single-thread-perl.json).
- [Threaded Perl, including partial free/refill](benchmarks/2026-10-28/threaded-perl.json).
- [Threaded C and peak RSS](benchmarks/2026-10-28/threaded-c.json).
- [Validation output](benchmarks/2026-10-28/validation.txt).

Raw `cleanup` rows select the measured library; `before` selects the preceding
bitmap-reuse build, SHA-256
`153573b1b4adbbebf7d9cf72e9befa96f31e7d3b0387497fa37337a25dead27c`.
Paths in those records identify the original local experiment. Superseded
investigation reports and prototype patches are not maintained instructions.

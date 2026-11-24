# Allocator performance and validation

Measurements recorded **28 October 2026**, Linux x86-64, GCC 13.3.0, glibc 2.39.
The measured O2 allocator has SHA-256
`3b57cbefb8c369e7f63d8759e0890e74ace3a54594fb55707da70ee35cb603b4`.
The [design guide](allocator-design.md) defines the maintained representation;
the [CI guide](allocator-ci.md) covers building, testing and releases.

The comparison below describes the baseline before the mask simplification.
The latest implementation and its measurements are recorded under “Simplified
capacity masks” at the end of this document.

## Baseline comparison

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

## Generalized leaf batching experiment

On 28 October 2026, an experimental change extended the owner cache from leaf
zero to any selected bitmap leaf. The leaf index fit in existing header padding.
Owner frees checked the retained head and selected leaf; thread exit published
the cached leaf and updated its summary and busy count.

Nine rotating rounds compared the same freshly built O2 baseline and experiment.
Median elapsed seconds, baseline -> experiment:

- Single-thread Perl: 1.0511 -> 1.0158 (3.4% lower).
- Four-worker Perl: 0.6167 -> 0.6284 (1.9% higher).
- Eight-thread C churn: 0.1968 -> 0.2001 (1.7% higher).

Perl sampled RSS was essentially unchanged. Contemporaneous glibc medians were
1.0015, 0.7793 and 0.2319 seconds respectively. Absolute times differ from the
earlier run above; compare variants within this experiment. These small mixed
effects do not establish a general improvement or identify its cause. All nine
standalone checks and both Perl smoke workloads passed at O0/O2/O3. No full Perl
core harness or musl validation was run for this experiment.

The implementation was restored to the baseline. The
[experimental patch](benchmarks/2026-10-28/leaf-experiment/experiment.patch) and
raw [single-thread](benchmarks/2026-10-28/leaf-experiment/single.json),
[threaded Perl](benchmarks/2026-10-28/leaf-experiment/threaded.json), and
[C churn](benchmarks/2026-10-28/leaf-experiment/c.json) observations preserve the
result for follow-up.

## Instruction profiling under competing host load

Callgrind 3.22.0 profiled the unchanged O2 library above with branch simulation.
The host was also running another C project; native timing differences are not
used to rank candidates here. Callgrind counts instructions, not CPU cycles,
and serializes threads: it cannot establish native contention or divide latency.
Runs use the same Perl 5.44.0 build, fixed hash settings, 30,000 single-thread
entries and 4 x 3,000 threaded entries. These smaller workloads include startup
and teardown and must not be compared directly with the timing workloads.

Single-thread total: 395,540,538 instructions. Source-attributed self counts:
`memgetsmall` 28,652,510 (7.24%); `memput` 13,862,350 (3.50%);
`memget` 8,184,571 (2.07%). Inlined lock helpers may be attributed separately.
The cached allocation return executes 6,804 times versus 233,900 shared-path
selections: about 2.8% of small allocations use the cache-only path.
`memwordmask` source lines account for 4,916,207 instructions (1.24% of total),
aggregated across callers and already included in their costs.

Threaded total: 628,355,686 instructions. Corresponding source-attributed
self counts are 21,098,980 (3.36%), 11,445,482 (1.82%) and 6,302,994 (1.00%).
Cached allocations: 11,703 versus 173,646 shared-path selections (6.3%).
Both workloads completed successfully; instrumented RSS and elapsed time are
not native allocator performance measurements.

Disassembly confirms `memput` executes a variable `divq` for slot decoding,
and constructs the valid mask before the owner-only early return. Candidate
experiments are simplifying full-leaf masks and replacing repeated division
with exact precomputed class arithmetic. Neither has a demonstrated speedup.
The generalized cache experiment remains inconclusive under competing load.

Raw [single-thread](benchmarks/2026-10-28/callgrind/single.out) and
[threaded](benchmarks/2026-10-28/callgrind/threaded.out) Callgrind profiles retain
function, source-line and branch counts. Use `callgrind_annotate --auto=no
--show=Ir PROFILE usr/lib/zen/mem.c` from the repository to inspect them.

The full eight-thread C Callgrind run was interrupted without a completed
profile. A reduced 8 x 2,000 iteration attempt also exceeded a 60-second limit.
No C profiling conclusions are drawn; instrumentation/scheduling behavior needs
investigation before using this workload under Callgrind. Native C tests passed
in the preceding experiment.

## Simplified capacity masks

The maintained implementation now stores the final leaf's valid mask once at
slab creation (`lastmask`). Other leaves use all ones; leaf zero still masks its
lease bit. Owner-only frees return before shared-mask calculation. No ownership,
publication or reclamation rules change. The x86-64 header grows from 2144 to
2152 bytes, both rounded to 2176 bytes: slab capacity is unchanged. The final O2
library SHA-256 is
`12a3c8c8d223173f54f3671f2e30af658f1e354b82bf0666725c45736952777c`.

An initial branch-around-shift variant was also checked, then superseded by
precomputation. The retained version passes all nine C checks and both Perl
smoke workloads at O0/O2/O3, and the O2 library compiles with
`-Wall -Wextra -Werror`. An exhaustive comparison of every valid leaf for
capacities 1..16384 matches the previous formula, including single-leaf, exact
word-boundary and partial final leaves. No full Perl core harness or musl run was
performed for this change. [Validation](benchmarks/2026-10-28/mask/validation.txt)
and the [equivalence check](benchmarks/2026-10-28/mask/mask-equivalence.c) are saved.

Against the preceding Callgrind baseline, single-thread total instructions fall
from 395,540,538 to 394,819,228 (0.18%). `memgetsmall` self instructions fall
from 28,652,510 to 27,953,241 (2.44%). Whole-program simulated branch misses fall
from 3,917,955 to 3,860,793. These are instruction/simulation results, not elapsed
speedups. Threaded whole-program counts vary substantially with scheduling and
Perl's checkpoint polling, so their total difference is not attributed to masks.
Assembly confirms that owner-only free returns precede mask work.

Nine rotating native rounds, baseline -> new median elapsed seconds:

- Single-thread Perl, 300,000 entries: 0.8194 -> 0.8059 (1.6% lower).
- Four-worker Perl, 75,000 entries each: 0.7266 -> 0.7033 (3.2% lower).
- Eight-thread C churn: 0.3406 -> 0.3430 (0.7% higher).

The host concurrently runs another C project. Ranges overlap; these medians
do not establish a reliable native speedup or regression. Sampled Perl RSS is
essentially unchanged: 239,836 -> 239,816 KiB single-thread and
247,484 -> 247,512 KiB at the threaded live checkpoint. The change is retained
for removing repeated mask construction with unchanged slab capacity.

Raw [single-thread timings](benchmarks/2026-10-28/mask/single.json),
[threaded timings](benchmarks/2026-10-28/mask/threaded.json),
[C timings](benchmarks/2026-10-28/mask/c.json),
[single-thread profile](benchmarks/2026-10-28/mask/single.callgrind), and
[threaded profile](benchmarks/2026-10-28/mask/threaded.callgrind) preserve results.

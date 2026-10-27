#!/usr/bin/env python3
"""Interleave allocator preloads against one Perl build; save raw observations."""
import argparse
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--perl-root', type=Path, required=True)
parser.add_argument('--rounds', type=int, default=5)
parser.add_argument('--count', type=int, default=300000)
parser.add_argument('--threads', type=int, default=0,
                    help='actual worker threads; --count is per worker (0 uses the single-thread workload)')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('allocators', nargs='+', help='name=/absolute/library.so or glibc=')
args = parser.parse_args()
if args.count < 1 or args.rounds < 1 or args.threads < 0:
    parser.error('count/rounds must be positive and threads must be nonnegative')
variants = [value.split('=', 1) for value in args.allocators]
rows = []
for round_number in range(args.rounds):
    offset = round_number % len(variants)
    for name, library in variants[offset:] + variants[:offset]:
        env = dict(os.environ, PERL_HASH_SEED='42', PERL_PERTURB_KEYS='0')
        env.pop('LD_PRELOAD', None)
        if library:
            env['LD_PRELOAD'] = str(Path(library).resolve())
        script = 'bench-perl-threads.pl' if args.threads else 'bench-perl-churn.pl'
        command = [str(args.perl_root.resolve() / 'perl'),
                   '-I' + str(args.perl_root.resolve() / 'lib'),
                   str(Path(__file__).with_name(script).resolve()),
                   str(args.count)]
        if args.threads:
            command.append(str(args.threads))
        start = time.monotonic()
        result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=90)
        row = dict(round=round_number + 1, allocator=name, library=library,
                   seconds=time.monotonic() - start, returncode=result.returncode,
                   stdout=result.stdout, stderr=result.stderr)
        if args.threads:
            row.update(workers=args.threads, count_per_worker=args.count)
        values = re.search(r'hash-build ([\d.]+) ms, scan ([\d.]+) ms, str\+sort ([\d.]+) ms', result.stdout)
        rss = re.search(r'RSS: (\d+) kB', result.stdout)
        if result.returncode == 0 and values and rss:
            row.update(zip(('hash_ms', 'scan_ms', 'strings_ms'), map(float, values.groups())))
            row['rss_kib'] = int(rss.group(1))
            if args.threads:
                phases = re.search(r'partial-free ([\d.]+) ms, refill ([\d.]+) ms, cleanup ([\d.]+) ms', result.stdout)
                if not phases:
                    raise SystemExit('threaded phase timings missing')
                row.update(zip(('partial_ms', 'refill_ms', 'cleanup_ms'), map(float, phases.groups())))
                for phase in ('partial', 'refill', 'freed', 'joined'):
                    match = re.search(r'RSS_' + phase.upper() + r': (\d+) kB', result.stdout)
                    if not match:
                        raise SystemExit('threaded checkpoint missing: ' + phase)
                    row['rss_' + phase + '_kib'] = int(match.group(1))
        rows.append(row)
        args.output.write_text(json.dumps(rows, indent=2) + '\n')
        print(name, result.returncode, result.stdout.strip().replace('\n', '; '), flush=True)
        if result.returncode or not values or not rss:
            raise SystemExit(result.stderr or 'benchmark output missing')
for name, _ in variants:
    samples = [r for r in rows if r['allocator'] == name]
    keys = ['hash_ms', 'scan_ms', 'strings_ms', 'rss_kib', 'seconds']
    if args.threads:
        keys += ['partial_ms', 'refill_ms', 'cleanup_ms']
        keys += ['rss_' + phase + '_kib' for phase in ('partial', 'refill', 'freed', 'joined')]
    medians = {key: statistics.median(r[key] for r in samples) for key in keys}
    print(name, json.dumps(medians), flush=True)

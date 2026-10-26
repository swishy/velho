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
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('allocators', nargs='+', help='name=/absolute/library.so or glibc=')
args = parser.parse_args()
variants = [value.split('=', 1) for value in args.allocators]
rows = []
for round_number in range(args.rounds):
    offset = round_number % len(variants)
    for name, library in variants[offset:] + variants[:offset]:
        env = dict(os.environ, PERL_HASH_SEED='42', PERL_PERTURB_KEYS='0')
        env.pop('LD_PRELOAD', None)
        if library:
            env['LD_PRELOAD'] = str(Path(library).resolve())
        command = [str(args.perl_root.resolve() / 'perl'),
                   '-I' + str(args.perl_root.resolve() / 'lib'),
                   str(Path(__file__).with_name('bench-perl-churn.pl').resolve()),
                   str(args.count)]
        start = time.monotonic()
        result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=90)
        row = dict(round=round_number + 1, allocator=name, library=library,
                   seconds=time.monotonic() - start, returncode=result.returncode,
                   stdout=result.stdout, stderr=result.stderr)
        values = re.search(r'hash-build ([\d.]+) ms, scan ([\d.]+) ms, str\+sort ([\d.]+) ms', result.stdout)
        rss = re.search(r'RSS: (\d+) kB', result.stdout)
        if result.returncode == 0 and values and rss:
            row.update(zip(('hash_ms', 'scan_ms', 'strings_ms'), map(float, values.groups())))
            row['rss_kib'] = int(rss.group(1))
        rows.append(row)
        args.output.write_text(json.dumps(rows, indent=2) + '\n')
        print(name, result.returncode, result.stdout.strip().replace('\n', '; '), flush=True)
        if result.returncode or not values or not rss:
            raise SystemExit(result.stderr or 'benchmark output missing')
for name, _ in variants:
    samples = [r for r in rows if r['allocator'] == name]
    medians = {key: statistics.median(r[key] for r in samples)
               for key in ('hash_ms', 'scan_ms', 'strings_ms', 'rss_kib', 'seconds')}
    print(name, json.dumps(medians), flush=True)

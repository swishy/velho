#!/bin/sh
# Run the standalone allocator suite against an already-built interposer.
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:-/tmp/velho-malloc-build}
out=$(CDPATH= cd -- "$out" && pwd)
ulimit -c 0

timeout -k 5s 90s "$out/test-tabhash"
timeout -k 5s 90s "$out/malloc-stress"
timeout -k 5s 90s env LD_PRELOAD="$out/libvelhomalloc.so" \
    "$out/malloc-alignment-test"
timeout -k 5s 90s "$out/malloc-mtstress"
timeout -k 5s 90s "$out/malloc-xfree-mt"
timeout -k 5s 90s "$out/malloc-parked-test"
# Exercise a real consumer as well as the C tests. This is a smoke test,
# not the separate source-built Perl core harness or a performance gate.
timeout -k 5s 90s env LD_PRELOAD="$out/libvelhomalloc.so" \
    PERL_HASH_SEED=42 PERL_PERTURB_KEYS=0 \
    perl "$src/c/bench-perl-churn.pl" 30000

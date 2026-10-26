#!/bin/sh
# Standalone Linux malloc interposer and focused allocator checks.
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:-/tmp/velho-malloc-build}
opt=${2:--O2}
mkdir -p "$out"
out=$(CDPATH= cd -- "$out" && pwd)
cc=${CC:-cc}
"$cc" -std=gnu11 "$opt" -g -fPIC -shared -I "$src" \
    "$src/zen/mem.c" "$src/c/malloc.c" "$src/c/malloc-reenter.c" \
    -pthread -lm -o "$out/libvelhomalloc.so"
"$cc" -std=gnu11 "$opt" -g "$src/c/malloc-stress.c" \
    -L "$out" -Wl,-rpath,"$out" -lvelhomalloc -ldl -o "$out/malloc-stress"
"$cc" -std=gnu11 "$opt" -g -I "$src" "$src/zen/test-tabhash.c" \
    "$src/c/malloc-reenter.c" -o "$out/test-tabhash"
"$cc" -std=gnu11 "$opt" -g -fno-builtin "$src/c/malloc-alignment-test.c" \
    -o "$out/malloc-alignment-test"
"$cc" -std=gnu11 "$opt" -g -pthread "$src/c/malloc-xfree-mt.c" \
    -L "$out" -Wl,-rpath,"$out" -lvelhomalloc -o "$out/malloc-xfree-mt"
printf '%s\n' "$out/libvelhomalloc.so"

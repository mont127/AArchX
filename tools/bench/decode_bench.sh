#!/bin/bash
# Build tools/bench/decode_bench.c against one or more trees and print a
# side-by-side comparison.  Usage: decode_bench.sh [tree ...]
# Defaults to ~/AArchX-c (C reference) and ~/AArchX.
set -u
cd "$(dirname "$0")/../.."
SELF="$(pwd)"
TREES=${@:-"$HOME/AArchX-c $HOME/AArchX"}
CACHE=${DECODE_BENCH_CACHE:-/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64}
OFF=${DECODE_BENCH_OFF:-$((1<<20))}
LEN=${DECODE_BENCH_LEN:-$((64<<20))}

tmp=$(mktemp -d /tmp/decode_bench.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

i=0
for tree in $TREES; do
    i=$((i + 1))
    name=$(basename "$tree")_$i
    ( cd "$tree" && make -s ocerz ) || { echo "build failed in $tree"; exit 1; }
    objs=$(ls "$tree"/src/*.o | grep -v '/main\.o$' | tr '\n' ' ')
    rustlib=""
    [ -f "$tree/rust/target/release/libocerz_rs.a" ] && rustlib="$tree/rust/target/release/libocerz_rs.a"
    ( cd "$tree" && clang -arch arm64 -std=c11 -O2 -g -Iinclude -o "$tmp/bench_$name" \
        "$SELF/tools/bench/decode_bench.c" $objs $rustlib -lcompression -lc -lm ) \
        || { echo "bench build failed in $tree"; exit 1; }
    "$tmp/bench_$name" "$CACHE" "$OFF" "$LEN" > "$tmp/out_$name.txt"
    sed "s/^/$name: /" "$tmp/out_$name.txt"
done

if [ "$(grep -c 'mode=x64' $tmp/out_*.txt 2>/dev/null | grep -cv ':0')" -gt 1 ]; then
    for mode in x64 i386; do
        if [ "$(grep "mode=$mode" "$tmp"/out_*.txt | awk '{print $5}' | sort -u | wc -l | tr -d ' ')" != 1 ]; then
            echo "HASH MISMATCH"
            exit 1
        fi
    done
    echo "HASH MATCH"
fi

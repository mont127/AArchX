#!/bin/bash
# Compare C and Rust shared-cache resolution, mapping lookup, and startup.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
C_TREE="${C_TREE:-$HOME/AArchX-c}"
RUST_TREE="${RUST_TREE:-$ROOT}"
TMP="$(mktemp -d /tmp/cache_bench.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

build_tree() {
    local tree=$1 label=$2
    local rustlib=""
    if [ -f "$tree/rust/target/release/libocerz_rs.a" ]; then
        rustlib="$tree/rust/target/release/libocerz_rs.a"
    fi
    if ! (cd "$tree" && {
        if [ -n "$rustlib" ]; then
            rm -f src/cache.o src/cache.d
        fi
        make -s ocerz
    }) >"$TMP/build_$label.log" 2>&1; then
        cat "$TMP/build_$label.log"
        echo "build failed in $tree" >&2
        exit 1
    fi
    local -a objs=()
    local obj module
    for obj in "$tree"/src/*.o; do
        [ -f "$obj" ] || continue
        module=${obj##*/}
        module=${module%.o}
        [ "$module" = main ] && continue
        if [ -n "$rustlib" ] && {
            [ -f "$tree/rust/src/ported/$module.rs" ] ||
                [ -f "$tree/rust/src/ported/$module/mod.rs" ]
        }; then
            continue
        fi
        objs+=("$obj")
    done
    local -a link=()
    [ -z "$rustlib" ] || link+=("$rustlib")
    if ! clang -arch arm64 -std=c11 -O2 -g -I"$tree/include" \
        -o "$TMP/bench_$label" "$ROOT/tools/bench/cache_bench.c" \
        "${objs[@]}" "${link[@]}" -lcompression -lc -lm \
        >"$TMP/link_$label.log" 2>&1; then
        cat "$TMP/link_$label.log"
        echo "benchmark link failed in $tree" >&2
        exit 1
    fi
}

build_tree "$C_TREE" c
build_tree "$RUST_TREE" rust

for run in 1 2 3; do
    for label in c rust; do
        echo "run=$run tree=$label"
        "$TMP/bench_$label" | tee "$TMP/out_${run}_${label}.txt"
    done
done

hashes="$(awk '/^hash=/ { split($1, kv, "="); print kv[2] }' "$TMP"/out_*.txt | sort -u)"
if [ "$(printf '%s\n' "$hashes" | wc -l | tr -d ' ')" -ne 1 ]; then
    echo "HASH MISMATCH"
    printf '%s\n' "$hashes"
    exit 1
fi
echo "HASH MATCH"

for label in c rust; do
    tree=$C_TREE
    [ "$label" != rust ] || tree=$RUST_TREE
    python3 -c 'import os, subprocess, sys, time
tree = sys.argv[1]
env = os.environ.copy()
env["OCERZ_TCACHE"] = "off"
start = time.monotonic_ns()
for _ in range(20):
    subprocess.run(["./ocerz", "/bin/echo", "hi"], cwd=tree, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
elapsed = time.monotonic_ns() - start
print(f"startup tree={sys.argv[2]} runs=20 ns_per_start={elapsed / 20:.0f}")' \
        "$tree" "$label"
done

#!/bin/bash
# Per-call cost of ocerz's guest syscall boundary, C reference vs this tree.
# Usage: syscall_bench.sh [tree ...] (default ~/AArchX-c ~/AArchX).  For each
# tree and workload it times ITERS calls and 0 calls under the JIT, best of
# RUNS each, and prints (t_ITERS - t_0) / ITERS in ns per call.
set -u
cd "$(dirname "$0")/../.."
SELF="$(pwd)"
TREES=${@:-"$HOME/AArchX-c $HOME/AArchX"}
ITERS=${SYSCALL_BENCH_ITERS:-2000000}
RUNS=${SYSCALL_BENCH_RUNS:-5}
tmp=$(mktemp -d /tmp/syscall_bench.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
clang -arch x86_64 -nostdlib -static -fno-stack-protector -O2 -Wl,-e,_start \
    -o "$tmp/guest" tests/guest/crt0.s tests/guest/libmini.c tools/bench/syscall_bench.c \
    || { echo "guest build failed"; exit 1; }
best() {
    local b=""
    for _ in $(seq "$RUNS"); do
        local t0 t1
        t0=$(perl -MTime::HiRes=time -e 'printf "%.6f", time')
        "$1" "$tmp/guest" "$2" "$3" > /dev/null 2>&1
        t1=$(perl -MTime::HiRes=time -e 'printf "%.6f", time')
        b=$(perl -e "my \$d=$t1-$t0; print((\"$b\" eq \"\" || \$d < \"$b\") ? \$d : \"$b\")")
    done
    echo "$b"
}
for tree in $TREES; do
    for w in getpid badwrite gtod machself; do
        tn=$(best "$tree/ocerz" "$w" "$ITERS")
        t0=$(best "$tree/ocerz" "$w" 0)
        perl -e "printf \"%-12s %-9s %8.1f ns/call  (t=%.3fs t0=%.3fs)\n\", '$(basename "$tree")', '$w', ($tn-$t0)*1e9/$ITERS, $tn, $t0"
    done
done

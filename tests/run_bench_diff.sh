#!/usr/bin/env bash
# Interp-vs-JIT differential for the benchmark binary, which run_diff_test.sh skips for time.

set -u
cd "$(dirname "$0")/.."
OCERZ=./ocerz
BENCH=tests/guest/benchbin/bench
pass=0
fail=0

if [ ! -x "$BENCH" ]; then
    echo "run_bench_diff: $BENCH not built (make -C tests/guest bench)" >&2
    exit 1
fi

WORKLOADS=${*:-call alu mem br}

for w in $WORKLOADS; do
    "$OCERZ" -no-jit "$BENCH" "$w" >/tmp/ocerz_bi.$$ 2>/dev/null
    ic=$?
    "$OCERZ" "$BENCH" "$w" >/tmp/ocerz_bj.$$ 2>/dev/null
    jc=$?
    if [ "$ic" = "$jc" ] && cmp -s /tmp/ocerz_bi.$$ /tmp/ocerz_bj.$$; then
        echo "PASS bench $w (interp==jit, exit=$ic, out=$(cat /tmp/ocerz_bi.$$))"
        pass=$((pass + 1))
    else
        echo "FAIL bench $w (interp exit=$ic, jit exit=$jc)"
        echo "  interp: $(cat /tmp/ocerz_bi.$$)"
        echo "  jit   : $(cat /tmp/ocerz_bj.$$)"
        fail=$((fail + 1))
    fi
done

rm -f /tmp/ocerz_bi.$$ /tmp/ocerz_bj.$$
echo "----------------------------------------"
echo "bench differential: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

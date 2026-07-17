#!/usr/bin/env bash
#
# tests/run_bench_diff.sh
#
# The differential gate FOR THE BENCHMARK. tests/run_diff_test.sh deliberately
# does not cover tests/guest/benchbin/bench (see tests/guest/Makefile: it would
# add ~50s of interpreter time to every `make check`), but bench is precisely
# the code the throughput work changes, so it needs its own interp-vs-jit check.
#
# Each workload is run under -no-jit and under the JIT and the outputs must be
# byte-identical, exactly as run_diff_test.sh does for the correctness corpus.
# This is the check that catches a missed flag materialization in the hot loops:
# 'alu' and 'br' are dense flag producers and 'br' actually CONSUMES flags on a
# data-dependent Jcc chain.
#
# Slow by construction (the interpreter runs the full workload: ~60s+ for alu).
# Pass a workload list to narrow it, e.g.  bash tests/run_bench_diff.sh call mem
#
# Run from the repo root; exits non-zero on any divergence.

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

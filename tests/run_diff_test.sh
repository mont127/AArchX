#!/usr/bin/env bash
#
# tests/run_diff_test.sh
#
# Differential test: run every guest binary twice, once under the pure
# interpreter (-no-jit) and once under the JIT, and require byte-identical
# stdout AND identical exit codes. This holds the JIT to the interpreter as
# its reference without depending on any golden file, so a JIT codegen bug
# that happens to also be wrong in the golden cannot hide. Run from the repo
# root; exits non-zero on any divergence.

set -u
cd "$(dirname "$0")/.."
OCERZ=./ocerz
BIN=tests/guest/bin
ARGS="one two three"
pass=0
fail=0

# Per-binary extra environment. interrupt_test spins forever in a syscall-free
# self-loop that only an asynchronous cross-thread stop can break, so it MUST be
# launched with the async-stop hook armed (exactly as run_guest_tests.sh does)
# or it hangs the differential until the process is killed. Armed for BOTH the
# interpreter and the JIT run so the differential still verifies interp==jit on
# the cross-thread-exit path rather than skipping it. Emits one NAME=VALUE token
# or empty.
prog_env() {
    case "$1" in
        interrupt_test) echo "OCERZ_TEST_ASYNC_STOP_ICOUNT=500000" ;;
        link_spin) echo "OCERZ_TEST_ASYNC_STOP_MS=200" ;;
        *) echo "" ;;
    esac
}

# Bounded watchdog. The differential has no golden and glob-runs bin/*, so a
# spinning test whose async-stop is mis-wired (or a chaining bug that defeats the
# interrupt poll) would hang the whole differential -- previously for an hour at
# 99% CPU. Wrap every run in a hard timeout so such a bug surfaces as a FAIL, not
# a hang. GNU coreutils `timeout`/`gtimeout` when present, else a portable
# pure-bash watchdog. 30s is far above any correct run (the spinning gates stop in
# <1s via their 200ms async stop; the heaviest static test is a few seconds).
TIMEOUT_BIN=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_BIN="gtimeout"
fi
DIFF_TIMEOUT=30
run_bounded() {
    # run_bounded OUTFILE ENVTOKEN CMD...
    local outf="$1"; shift
    local etok="$1"; shift
    if [ -n "$TIMEOUT_BIN" ]; then
        env $etok "$TIMEOUT_BIN" "${DIFF_TIMEOUT}s" "$@" >"$outf" 2>/dev/null
        return $?
    fi
    env $etok "$@" >"$outf" 2>/dev/null &
    local pid=$!
    local waited=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$waited" -ge "$DIFF_TIMEOUT" ]; then
            kill -KILL "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
            return 124
        fi
        sleep 1
        waited=$((waited + 1))
    done
    wait "$pid"
    return $?
}

for prog in "$BIN"/*; do
    name=$(basename "$prog")
    penv=$(prog_env "$name")
    run_bounded /tmp/ocerz_interp.$$ "$penv" "$OCERZ" -no-jit "$prog" $ARGS
    ic=$?
    run_bounded /tmp/ocerz_jit.$$ "$penv" "$OCERZ" "$prog" $ARGS
    jc=$?
    if [ "$ic" = "$jc" ] && cmp -s /tmp/ocerz_interp.$$ /tmp/ocerz_jit.$$; then
        echo "PASS $name (interp==jit, exit=$ic)"
        pass=$((pass + 1))
    else
        echo "FAIL $name (interp exit=$ic, jit exit=$jc)"
        diff /tmp/ocerz_interp.$$ /tmp/ocerz_jit.$$ | head -6
        fail=$((fail + 1))
    fi
done

rm -f /tmp/ocerz_interp.$$ /tmp/ocerz_jit.$$
echo "----------------------------------------"
echo "differential: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

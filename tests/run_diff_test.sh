#!/usr/bin/env bash
# Differential gate: every guest binary under -no-jit and under the JIT must match byte for byte.

set -u
cd "$(dirname "$0")/.."
OCERZ=./ocerz
BIN=tests/guest/bin
ARGS="one two three"
pass=0
fail=0

prog_env() {
    case "$1" in
        interrupt_test) echo "OCERZ_TEST_ASYNC_STOP_ICOUNT=500000" ;;
        link_spin) echo "OCERZ_TEST_ASYNC_STOP_MS=200" ;;
        *) echo "" ;;
    esac
}

TIMEOUT_BIN=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_BIN="gtimeout"
fi
DIFF_TIMEOUT=30
run_bounded() {
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

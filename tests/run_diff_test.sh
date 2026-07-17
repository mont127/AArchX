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
        *) echo "" ;;
    esac
}

for prog in "$BIN"/*; do
    name=$(basename "$prog")
    penv=$(prog_env "$name")
    env $penv "$OCERZ" -no-jit "$prog" $ARGS >/tmp/ocerz_interp.$$ 2>/dev/null
    ic=$?
    env $penv "$OCERZ" "$prog" $ARGS >/tmp/ocerz_jit.$$ 2>/dev/null
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

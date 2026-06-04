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

for prog in "$BIN"/*; do
    name=$(basename "$prog")
    "$OCERZ" -no-jit "$prog" $ARGS >/tmp/ocerz_interp.$$ 2>/dev/null
    ic=$?
    "$OCERZ" "$prog" $ARGS >/tmp/ocerz_jit.$$ 2>/dev/null
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

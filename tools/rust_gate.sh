#!/bin/bash
# The rust-branch gate.  Builds ocerz and the unit binaries, then runs every
# phase of the check suite and compares failures against agents/baseline.md.
# A phase passes when it produces no NEW failure versus the baseline, not
# when it is fully green.  `--fast` runs units, both guest modes, diff and
# diff32 only.
#
# Each phase fails in two ways, and both are recorded as NEW failures unless
# agents/expected/<phase>.fail already lists them:
#   - a FAIL line (or `unit:<bin> rc=N` line for the unit binaries) its log
#     gains that the baseline did not have;
#   - the runner itself exiting nonzero, or missing the pass marker it prints
#     on success, so a phase that dies before printing any FAIL line (e.g. a
#     link failure) can never read "ok".
#
# Two runners exit nonzero on the baseline itself: run_dynamic_tests.sh ends
# with `[ "$fail" -eq 0 ]` while carrying 7 KNOWN-PENDING failures, and
# run_native_tests.sh the same over its sys_proc environment failure. Their
# exit status is therefore meaningless for regression purposes and only their
# FAIL lines are compared -- everything else must exit zero.
set -u
cd "$(dirname "$0")/.."
FAST=0
[ "${1:-}" = "--fast" ] && FAST=1
LOG=/tmp/rust_gate
mkdir -p "$LOG"
FAIL=0

note() { printf '%-28s %s\n' "$1" "$2"; }

expect_fail() {
    # $1 = phase name, $2 = log file, $3 = grep -E pattern for FAIL lines,
    # $4 = pass-marker regex required in the log (empty = none),
    # $5 = runner exit status (empty = not checked)
    local phase=$1 file=$2 pat=${3:-'^FAIL'} marker=${4:-} rc=${5:-}
    grep -E "$pat" "$file" 2>/dev/null | sort > "$LOG/$phase.fail"
    if [ -n "$rc" ] && [ "$rc" != 0 ]; then
        echo "runner:$phase rc=$rc" >> "$LOG/$phase.fail"
    fi
    if [ -n "$marker" ] && ! grep -qE "$marker" "$file" 2>/dev/null; then
        echo "runner:$phase missing pass marker" >> "$LOG/$phase.fail"
    fi
    sort -o "$LOG/$phase.fail" "$LOG/$phase.fail"
    comm -13 <(sort agents/expected/$phase.fail 2>/dev/null) "$LOG/$phase.fail" > "$LOG/$phase.new" || true
    if [ -s "$LOG/$phase.new" ]; then
        note "$phase" "NEW FAILURES:"
        sed 's/^/    /' "$LOG/$phase.new" | head -20
        FAIL=1
    else
        local n=$(wc -l < "$LOG/$phase.fail" | tr -d ' ')
        note "$phase" "ok ($n known failure(s))"
    fi
}

mkdir -p agents/expected

echo "== build =="
make -j12 ocerz > "$LOG/build.log" 2>&1 || { note build FAILED; tail -20 "$LOG/build.log"; exit 1; }
make -j12 $(ls tests/unit/*.c | sed 's|tests/unit/|tests/unit/bin/|;s|\.c$||') >> "$LOG/build.log" 2>&1 \
    || { note unit-build FAILED; tail -20 "$LOG/build.log"; exit 1; }
make -s guest >> "$LOG/build.log" 2>&1 || { note guest-build FAILED; tail -20 "$LOG/build.log"; exit 1; }
make -s apis >> "$LOG/build.log" 2>&1 || { note apis FAILED; tail -20 "$LOG/build.log"; exit 1; }
note build ok

echo "== unit =="
: > "$LOG/unit.log"
for t in tests/unit/bin/test_*; do
    [ -x "$t" ] && [ -f "$t" ] || continue
    case "$t" in *.d|*.dSYM) continue;; esac
    n=$(basename "$t")
    OCERZ_NO_ARM_EXEC=1 "$t" >> "$LOG/unit.log" 2>&1
    echo "unit:$n rc=$?" >> "$LOG/unit.log"
done
expect_fail unit "$LOG/unit.log" '^(FAIL|unit:.*rc=[1-9])' 'checks,'

echo "== guest --no-jit =="
bash tests/run_guest_tests.sh --no-jit > "$LOG/g_nojit.log" 2>&1
rc=$?
note "guest --no-jit" "$(grep -c '^PASS' "$LOG/g_nojit.log") pass / $(grep -c '^FAIL' "$LOG/g_nojit.log") fail"
expect_fail g_nojit "$LOG/g_nojit.log" '^FAIL' 'guest tests: [0-9]+/[0-9]+ passed' "$rc"

echo "== guest jit =="
bash tests/run_guest_tests.sh > "$LOG/g_jit.log" 2>&1
rc=$?
note "guest jit" "$(grep -c '^PASS' "$LOG/g_jit.log") pass / $(grep -c '^FAIL' "$LOG/g_jit.log") fail"
expect_fail g_jit "$LOG/g_jit.log" '^FAIL' 'guest tests: [0-9]+/[0-9]+ passed' "$rc"

echo "== diff =="
bash tests/run_diff_test.sh > "$LOG/diff.log" 2>&1
rc=$?
tail -1 "$LOG/diff.log" | sed 's/^/    /'
expect_fail diff "$LOG/diff.log" '^FAIL' 'differential: [0-9]+ passed' "$rc"

echo "== diff32 =="
bash tests/run_diff32.sh . > "$LOG/diff32.log" 2>&1
rc=$?
tail -1 "$LOG/diff32.log" | sed 's/^/    /'
expect_fail diff32 "$LOG/diff32.log" '^FAIL' 'differential32:' "$rc"

if [ "$FAST" = 0 ]; then
    echo "== dynamic =="
    bash tests/run_dynamic_tests.sh > "$LOG/dyn.log" 2>&1
    tail -2 "$LOG/dyn.log" | sed 's/^/    /'
    expect_fail dyn "$LOG/dyn.log" '^FAIL' 'dynamic tests: [0-9]+ passed'

    if [ -d runtime/apis ]; then
        for s in run_native_tests run_guest_library_tests run_native_cxx_tests \
                 run_native_framework_tests run_native_format_tests run_native_swift_tests; do
            echo "== $s =="
            bash "tests/$s.sh" > "$LOG/$s.log" 2>&1
            rc=$?
            tail -2 "$LOG/$s.log" | sed 's/^/    /'
            case "$s" in
                run_native_tests)
                    expect_fail "$s" "$LOG/$s.log" '^FAIL' 'native tests: [0-9]+ passed' ;;
                *)
                    expect_fail "$s" "$LOG/$s.log" '^FAIL' 'PASS|passed|SKIP' "$rc" ;;
            esac
        done
    else
        note native "skipped (no runtime/apis)"
    fi
fi

if [ "$FAIL" = 0 ]; then
    echo "GATE: PASS (no new failures vs baseline)"
else
    echo "GATE: FAIL (new failures vs baseline, see /tmp/rust_gate/*.new)"
fi
exit $FAIL

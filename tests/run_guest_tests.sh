#!/usr/bin/env bash
#
# tests/run_guest_tests.sh
#
# End-to-end guest test runner for Ocerz. For every prebuilt x86_64 guest
# binary in tests/guest/bin it runs the binary UNDER ./ocerz (never under
# Rosetta — this script must work on a machine with Rosetta absent), captures
# stdout, and compares it against the deterministic golden in
# tests/guest/expect/<name>.out, while also checking the process exit code
# against the per-test expected value. Each test prints a PASS or FAIL line;
# a final summary reports the totals and the script exits non-zero if any test
# failed, so it composes with `make check` and CI.
#
# Usage (run from the repo root):
#   tests/run_guest_tests.sh [--no-jit]
# --no-jit forwards -no-jit to ocerz, pinning the interpreter tier so the same
# goldens validate both execution tiers.
#
# The args test is invoked with the fixed arguments "one two three"; its
# golden was generated with argv[0] equal to the exact path this script
# passes (tests/guest/bin/args, relative to the repo root), so the script must
# be run from the repo root. Each run is bounded to 120 seconds: GNU/coreutils
# timeout or gtimeout is used when present, otherwise a portable pure-bash
# watchdog backgrounds the run and kills it after the deadline. Ocerz is
# located by an absolute path so the script is insensitive to cwd for the
# emulator itself even though the golden paths are repo-root relative.
#
# Implementation note: the optional -no-jit flag is carried as a scalar
# string (empty or the single token "-no-jit"), not a bash array, and is
# expanded unquoted on the ocerz command line together with the per-test
# extra arguments (empty or "one two three"). Both are controlled, space-free
# tokens, so unquoted word-splitting passes exactly the intended argv and
# adds no stray empty argument. This is deliberate: an empty bash array
# expanded under `set -u` errors as an unbound variable on the system bash
# 3.2 that ships with macOS, which the scalar form sidesteps.

set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OCERZ="$REPO_ROOT/ocerz"
GUEST_DIR="tests/guest"
BIN_DIR="$GUEST_DIR/bin"
EXPECT_DIR="$GUEST_DIR/expect"
TIMEOUT_SECS=120

JIT_FLAG=""
if [ "${1:-}" = "--no-jit" ]; then
    JIT_FLAG="-no-jit"
    shift
fi

if [ ! -x "$OCERZ" ]; then
    echo "error: ocerz binary not found or not executable at $OCERZ" >&2
    echo "build it first with: make ocerz" >&2
    exit 2
fi

TIMEOUT_BIN=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_BIN="gtimeout"
fi

run_with_timeout() {
    local out_file="$1"
    local err_file="$2"
    shift 2
    if [ -n "$TIMEOUT_BIN" ]; then
        "$TIMEOUT_BIN" "${TIMEOUT_SECS}s" "$@" >"$out_file" 2>"$err_file"
        return $?
    fi
    "$@" >"$out_file" 2>"$err_file" &
    local pid=$!
    local waited=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$waited" -ge "$TIMEOUT_SECS" ]; then
            kill -TERM "$pid" 2>/dev/null
            sleep 1
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

# fault_regs is the register-file-at-fault gate: it takes a SIGSEGV mid-block
# with every data GPR live and prints them from the ucontext. Its golden was
# generated from the NATIVE x86_64 run, so it holds ocerz to real hardware, not
# just to itself. See tests/guest/fault_regs.c.
# imul_flags gates src/jit.c:emit_imul (the inlined IMUL). It is listed here,
# unlike flags/flags_live/flags_carry, because it deliberately SPLITS its output
# so that a golden can exist at all: its "arch=" checksum folds in only CF and OF
# — the sole flags x86 defines after IMUL/MUL — and that line's golden was
# generated from the NATIVE x86_64 run, holding ocerz to real hardware. Its
# "pinned=" checksum folds in all six, including the architecturally UNDEFINED
# SF/ZF/AF/PF that src/flags.c deliberately pins to values native silicon does
# not reproduce; that line is ocerz's own convention and is what
# tests/run_diff_test.sh (interpreter vs JIT) enforces. See tests/guest/imul_flags.c.
# fault_resume is the RESUME-AND-RE-EXECUTE gate: its handler fixes the fault by
# editing a GPR in the mcontext and returns WITHOUT advancing rip, so the
# faulting instruction runs a second time and must succeed with exactly the
# hardware result and flags. Golden generated from the NATIVE x86_64 run. It
# uniquely exercises crash_handler's rip rewind, sys_sigreturn's GPR write-back,
# and lazy-flag materialization at a faulting flag producer. See
# tests/guest/fault_resume.c.
# interrupt_test is the JIT-chaining cross-thread-exit safety gate: it spins
# forever in a syscall-free self-loop that only an asynchronous cross-thread stop
# can break. It is launched with OCERZ_TEST_ASYNC_STOP_ICOUNT set (see test_env),
# which makes Ocerz spawn a separate host thread that requests exit once the
# guest is past startup; the run loop's cpu->interrupt poll must observe that and
# exit 0. Without the poll (or its broadcast) it would run until the runner's
# timeout -- a FAIL -- which is exactly the hang chaining would otherwise hide.
# link_spin is the GENERAL two-way-Jcc-linking cross-thread-exit gate: a hot loop
# whose body is a data-dependent if/else DIAMOND, so once every cross-block edge
# chains, insn_count FREEZES and only a wall-clock async stop can break it. It is
# launched with OCERZ_TEST_ASYNC_STOP_MS (see test_env), NOT the ICOUNT hook, for
# exactly that reason. See tests/guest/link_spin.c.
# fault_link is the cross-block-link FAULT-SEAM gate: a two-way loop across blocks
# with DIFFERENT hot-register subsets that takes a branch-free NULL-store fault
# deep in the loop; its golden is the NATIVE x86_64 run, so a dropped pin spill or
# a body-entry shortcut on a cross-block edge misassigns the accumulators and
# fails against real hardware. Bounded/self-terminating: needs no async-stop arming.
# fault_xlive is the CROSS-SEAM flag-at-fault gate that the 31-binary PUSHF
# differential CANNOT see: it faults immediately after a JMP-imm block seam and
# reads the faulting rflags from the ucontext, so if XLIVE ever dropped a
# producer flag its successor "does not read" the masked flags would diverge from
# the NATIVE golden. Green at HEAD and under correct XLIVE (the fault barrier
# keeps the producer eager); the STEP-2 mutation that kills the barrier makes it
# FAIL, proving the barrier is load-bearing. Bounded/self-terminating.
declare -a NAMES=(hello exit42 args alu branches strings fib sse mmap_test fileio memstress longblock signal_test signal_jump0 rip_test stack_test popmem_test fault_regs fault_resume fault_chain callret_fault imul_flags interrupt_test ras_stress link_spin fault_link fault_xlive)

expected_exit() {
    case "$1" in
        exit42) echo 42 ;;
        *) echo 0 ;;
    esac
}

test_args() {
    case "$1" in
        args) echo "one two three" ;;
        *) echo "" ;;
    esac
}

# Per-test environment. Emits a single NAME=VALUE token (no spaces) or empty.
# interrupt_test needs the cross-thread async-stop hook armed; the threshold is
# well past a static test's startup so its "start" line is always emitted first,
# keeping the golden deterministic.
test_env() {
    case "$1" in
        interrupt_test) echo "OCERZ_TEST_ASYNC_STOP_ICOUNT=500000" ;;
        link_spin) echo "OCERZ_TEST_ASYNC_STOP_MS=200" ;;
        *) echo "" ;;
    esac
}

PASS=0
FAIL=0
ACTUAL_OUT="$(mktemp /tmp/ocerz_guest_actual.XXXXXX)"
ACTUAL_ERR="$(mktemp /tmp/ocerz_guest_stderr.XXXXXX)"
trap 'rm -f "$ACTUAL_OUT" "$ACTUAL_ERR"' EXIT

for name in "${NAMES[@]}"; do
    bin="$BIN_DIR/$name"
    golden="$EXPECT_DIR/$name.out"
    if [ ! -x "$bin" ]; then
        echo "FAIL $name (missing binary $bin; run: make -C tests/guest)"
        FAIL=$((FAIL + 1))
        continue
    fi
    if [ ! -f "$golden" ]; then
        echo "FAIL $name (missing golden $golden)"
        FAIL=$((FAIL + 1))
        continue
    fi

    extra="$(test_args "$name")"
    tenv="$(test_env "$name")"
    if [ -n "$tenv" ]; then
        export "$tenv"
    fi
    run_with_timeout "$ACTUAL_OUT" "$ACTUAL_ERR" "$OCERZ" $JIT_FLAG "$bin" $extra
    rc=$?
    if [ -n "$tenv" ]; then
        unset "${tenv%%=*}"
    fi

    want_rc="$(expected_exit "$name")"
    ok=1
    reason=""

    if [ "$rc" -eq 124 ]; then
        ok=0
        reason="timed out after ${TIMEOUT_SECS}s"
    elif [ "$rc" -ne "$want_rc" ]; then
        ok=0
        reason="exit code $rc, expected $want_rc"
    elif ! cmp -s "$ACTUAL_OUT" "$golden"; then
        ok=0
        reason="stdout differs from golden"
    fi

    if [ "$ok" -eq 1 ]; then
        echo "PASS $name"
        PASS=$((PASS + 1))
    else
        echo "FAIL $name ($reason)"
        FAIL=$((FAIL + 1))
        if [ "$reason" = "stdout differs from golden" ]; then
            echo "  --- expected (golden) ---" >&2
            sed 's/^/  | /' "$golden" >&2
            echo "  --- actual ---" >&2
            sed 's/^/  | /' "$ACTUAL_OUT" >&2
        fi
        if [ -s "$ACTUAL_ERR" ]; then
            echo "  --- stderr ---" >&2
            sed 's/^/  | /' "$ACTUAL_ERR" >&2
        fi
    fi
done

TOTAL=$((PASS + FAIL))
echo "----------------------------------------"
echo "guest tests: $PASS/$TOTAL passed, $FAIL failed"

if [ "$FAIL" -ne 0 ]; then
    exit 1
fi
exit 0

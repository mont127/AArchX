#!/usr/bin/env bash
# Runs every prebuilt guest binary under ./ocerz and diffs stdout against its golden.

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

declare -a NAMES=(hello exit42 args alu branches strings fib sse mmap_test fileio memstress longblock signal_test signal_jump0 rip_test stack_test popmem_test fault_regs fault_resume fault_chain callret_fault imul_flags interrupt_test ras_stress link_spin fault_link fault_xlive fork_order fork_exec shared_order unaligned_order jit_ops jit_div jit_sse jit_cvt jit_sse2 nan_rules jit_mul jit_scan jit_rmw jit_bt jit_movq jit_narrow jit_misc2 jit_misc3 jit_align jit_align2 jit_nanabs jit_divtrap sigmask_test int3_test sse42_test rsp_ops fcntl_locks cvt_nan_batch fp_loop_nan fpb_defer_nan cvt_merge_nan machdep_rdx ret_flags mov_sreg far_sel_bits filemap_private bigenv stack_align jcc_chain_rec atomic_unaligned)

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

test_env() {
    case "$1" in
        interrupt_test) echo "OCERZ_TEST_ASYNC_STOP_MS=200" ;;   # insn_count only ticks in the interpreter; fully-JITed loops never reach an ICOUNT
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

# Ordered-memory (multi-observer) model: the RMW/atomic emitters take the LSE
# and ldapr/stlr paths there; run the tests that exercise them under
# OCERZ_NO_PLAIN_MEM=1 as well.
for name in jit_rmw jit_scan jit_ops jit_bt jit_align jit_align2 jit_movq jit_sse jit_misc2 unaligned_order; do
    bin="$BIN_DIR/$name"; golden="$EXPECT_DIR/$name.out"
    [ -x "$bin" ] && [ -f "$golden" ] || continue
    export OCERZ_NO_PLAIN_MEM=1
    run_with_timeout "$ACTUAL_OUT" "$ACTUAL_ERR" "$OCERZ" $JIT_FLAG "$bin"
    rc=$?
    unset OCERZ_NO_PLAIN_MEM
    if [ "$rc" -eq 0 ] && cmp -s "$ACTUAL_OUT" "$golden"; then
        echo "PASS $name (ordered)"; PASS=$((PASS + 1))
    else
        echo "FAIL $name (ordered: rc=$rc)"; FAIL=$((FAIL + 1))
        diff "$golden" "$ACTUAL_OUT" | head -20 | sed 's/^/  | /' >&2
    fi
done

# Interpreted-block path (jit_interp_block: blocks without code once the arena
# is full).  A tiny arena makes almost every block run through it; superblock
# side exits taken mid-block must stop the block (bug fixed 2026-08-17: the
# rest of the block ran after a taken jcc -> guest crashes 1/150 wine runs).
for name in branches jit_ops jit_misc2 fib strings; do
    bin="$BIN_DIR/$name"; golden="$EXPECT_DIR/$name.out"
    [ -x "$bin" ] && [ -f "$golden" ] || continue
    export OCERZ_JIT_CODE_KB=64
    run_with_timeout "$ACTUAL_OUT" "$ACTUAL_ERR" "$OCERZ" $JIT_FLAG "$bin"
    rc=$?
    unset OCERZ_JIT_CODE_KB
    if [ "$rc" -eq 0 ] && cmp -s "$ACTUAL_OUT" "$golden"; then
        echo "PASS $name (tiny arena)"; PASS=$((PASS + 1))
    else
        echo "FAIL $name (tiny arena: rc=$rc)"; FAIL=$((FAIL + 1))
        diff "$golden" "$ACTUAL_OUT" | head -20 | sed 's/^/  | /' >&2
    fi
done

# Dynamic-mode smoke test: a real Mach-O from the x86_64 shared cache world.
# The synthetic guests all run in plain/static mode; this is the only test
# that exercises the commpage guard, dyld-cache-sized block counts and the
# hostwq path.  Skipped (not counted) when no x86_64 Wine is installed.
WINE_BIN="${OCERZ_WINE:-$HOME/Wine Devel.app/Contents/Resources/wine/bin/wine}"
if [ -x "$WINE_BIN" ]; then
    export OCERZ_HOSTWQ=1
    saved_timeout=$TIMEOUT_SECS
    TIMEOUT_SECS=60
    run_with_timeout "$ACTUAL_OUT" "$ACTUAL_ERR" "$OCERZ" $JIT_FLAG "$WINE_BIN" --version
    rc=$?
    TIMEOUT_SECS=$saved_timeout
    unset OCERZ_HOSTWQ
    if [ "$rc" -eq 0 ] && grep -q '^wine-' "$ACTUAL_OUT"; then
        echo "PASS wine_version ($(head -1 "$ACTUAL_OUT"))"
        PASS=$((PASS + 1))
    else
        if [ "$rc" -eq 124 ]; then reason="timed out (dynamic-mode hang)"; else reason="exit code $rc / no wine- banner"; fi
        echo "FAIL wine_version ($reason)"
        FAIL=$((FAIL + 1))
        if [ -s "$ACTUAL_ERR" ]; then
            echo "  --- stderr ---" >&2
            tail -20 "$ACTUAL_ERR" | sed 's/^/  | /' >&2
        fi
    fi
    # the same run with a 512 KB code arena: ~650k blocks go through
    # jit_interp_block (superblock side exits, records, redirects)
    export OCERZ_HOSTWQ=1 OCERZ_JIT_CODE_KB=512
    TIMEOUT_SECS=120
    run_with_timeout "$ACTUAL_OUT" "$ACTUAL_ERR" "$OCERZ" $JIT_FLAG "$WINE_BIN" --version
    rc=$?
    TIMEOUT_SECS=$saved_timeout
    unset OCERZ_HOSTWQ OCERZ_JIT_CODE_KB
    if [ "$rc" -eq 0 ] && grep -q '^wine-' "$ACTUAL_OUT"; then
        echo "PASS wine_version (tiny arena)"
        PASS=$((PASS + 1))
    else
        echo "FAIL wine_version (tiny arena: rc=$rc)"
        FAIL=$((FAIL + 1))
        if [ -s "$ACTUAL_ERR" ]; then
            echo "  --- stderr ---" >&2
            tail -20 "$ACTUAL_ERR" | sed 's/^/  | /' >&2
        fi
    fi
else
    echo "SKIP wine_version (no x86_64 wine at $WINE_BIN; set OCERZ_WINE)"
fi

TOTAL=$((PASS + FAIL))
echo "----------------------------------------"
echo "guest tests: $PASS/$TOTAL passed, $FAIL failed"

if [ "$FAIL" -ne 0 ]; then
    exit 1
fi
exit 0

#!/usr/bin/env bash
# Gate for the native execution mode (M0): the mode exists, it is selectable
# from the command line and from the environment, and with the shared cache
# switched off the imports nothing resolves turn into a named report instead of
# a crash.
#
# In native mode ocerz maps no shared cache at all, so nothing answers the
# guest's libSystem imports yet -- the virtual frameworks that will answer them
# are not written. M0 stops exactly at that boundary on purpose: every
# unresolved import is collected at the end of the main image's fixups, printed
# as an "ocerz: native: no bridge for <sym> in <dylib>" line, and the process
# exits 71. So a native-mode run that exits 71 carrying the full list is the
# pass here, and one that exits 0 would mean the cache leaked back in. The list
# is the whole deliverable of the milestone, so each expected symbol is
# asserted by name rather than by counting lines.
#
# The fixture is tests/guest/benchbin/xbench_dyn: dynamically linked, LC_MAIN,
# one dependency (/usr/lib/libSystem.B.dylib) and exactly four imports, so the
# report is small enough to pin whole. It writes its checksum with a raw write
# syscall rather than libc, so cache mode still gives deterministic stdout.
#
# Cache mode must come out of all this untouched, and it is checked the way the
# rest of the suite checks translation: the JIT and the interpreter running the
# same command must agree byte for byte. That needs no Rosetta on the box.
#
# The cases that need a mappable shared cache are skipped, not failed, where
# there is none. The native cases still run there -- not needing a cache is the
# entire point of the mode.

set -u
cd "$(dirname "$0")/.."
OCERZ=./ocerz
TMP="${TMPDIR:-/tmp}/ocerz_native.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

DYN=tests/guest/benchbin/xbench_dyn
STATIC=tests/guest/bin/exit42
KERNEL=depchain
SCALE=1000
SYMS="___bzero _memcpy _strcmp _strlen"
SUMMARY="ocerz: native: 4 unresolved imports, no virtual frameworks are implemented yet"

unset OCERZ_MODE

if [ ! -x "$OCERZ" ]; then
    echo "error: ocerz binary not found or not executable at $OCERZ" >&2
    echo "build it first with: make ocerz" >&2
    exit 2
fi
if [ ! -x "$DYN" ] || [ ! -x "$STATIC" ]; then
    echo "run_native_tests: SKIP (guest fixtures missing; run: make -C tests/guest)"
    exit 0
fi

pass=0
fail=0

TIMEOUT_BIN=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_BIN="gtimeout"
fi
NATIVE_TIMEOUT=60

run_bounded() {
    local out_file="$1" err_file="$2"
    shift 2
    if [ -n "$TIMEOUT_BIN" ]; then
        "$TIMEOUT_BIN" "${NATIVE_TIMEOUT}s" "$@" >"$out_file" 2>"$err_file"
        return $?
    fi
    "$@" >"$out_file" 2>"$err_file" &
    local pid=$!
    local waited=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$waited" -ge "$NATIVE_TIMEOUT" ]; then
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

record() {
    local name="$1" reason="$2" detail="${3:-}"
    if [ -z "$reason" ]; then
        echo "PASS $name${detail:+ ($detail)}"; pass=$((pass+1))
    else
        echo "FAIL $name ($reason)"; fail=$((fail+1))
    fi
}

cache_line_seen() {
    grep -q 'shared cache mapped' "$@"
}

# empty on success, otherwise the first missing report line
native_report_reason() {
    local sym
    for sym in $SYMS; do
        if ! grep -Fq "ocerz: native: no bridge for $sym in " "$@"; then
            echo "no 'no bridge for $sym' line in the report"
            return
        fi
    done
    echo ""
}

CACHE_OK=1
run_bounded "$TMP/probe.out" "$TMP/probe.err" "$OCERZ" "$DYN" "$KERNEL" 1
case $? in
    65|70) CACHE_OK=0 ;;
esac

NOUT="$TMP/native_dyn.out"
NERR="$TMP/native_dyn.err"
CACHE_OUT="$TMP/cache_dyn.jit.out"

case_native_dyn() {
    local name=native_dyn rc reason=""
    run_bounded "$NOUT" "$NERR" "$OCERZ" -v -native "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    if [ "$rc" -ne 71 ]; then
        reason="exit $rc, want 71"
    elif cache_line_seen "$NOUT" "$NERR"; then
        reason="native mode mapped the shared cache"
    else
        reason="$(native_report_reason "$NOUT" "$NERR")"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_native_summary() {
    local name=native_summary reason=""
    if ! grep -Fq "$SUMMARY" "$NOUT" "$NERR"; then
        reason="no '4 unresolved imports' summary line"
    fi
    record "$name" "$reason"
}

case_cache_dyn() {
    local name=cache_dyn rc_jit rc_nojit reason=""
    if [ "$CACHE_OK" -ne 1 ]; then
        echo "SKIP $name (shared cache not mappable here)"; return
    fi
    run_bounded "$CACHE_OUT" "$TMP/$name.jit.err" "$OCERZ" "$DYN" "$KERNEL" "$SCALE"
    rc_jit=$?
    run_bounded "$TMP/$name.nojit.out" "$TMP/$name.nojit.err" "$OCERZ" -no-jit "$DYN" "$KERNEL" "$SCALE"
    rc_nojit=$?
    if [ "$rc_jit" -ne 0 ]; then
        reason="jit exit $rc_jit, want 0"
    elif [ "$rc_nojit" -ne 0 ]; then
        reason="no-jit exit $rc_nojit, want 0"
    elif [ ! -s "$CACHE_OUT" ]; then
        reason="no stdout"
    elif ! cmp -s "$CACHE_OUT" "$TMP/$name.nojit.out"; then
        reason="jit and no-jit stdout differ"
    fi
    record "$name" "$reason" "out='$(head -1 "$CACHE_OUT")'"
}

case_env_native() {
    local name=env_native rc reason="" out="$TMP/env_native.out" err="$TMP/env_native.err"
    run_bounded "$out" "$err" env OCERZ_MODE=native "$OCERZ" -v "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    if [ "$rc" -ne 71 ]; then
        reason="exit $rc, want 71"
    elif cache_line_seen "$out" "$err"; then
        reason="OCERZ_MODE=native mapped the shared cache"
    else
        reason="$(native_report_reason "$out" "$err")"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_flag_beats_env() {
    local name=flag_beats_env rc reason="" out="$TMP/flag_beats_env.out" err="$TMP/flag_beats_env.err"
    if [ "$CACHE_OK" -ne 1 ]; then
        echo "SKIP $name (shared cache not mappable here)"; return
    fi
    run_bounded "$out" "$err" env OCERZ_MODE=native "$OCERZ" -v -cache "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        reason="exit $rc, want 0"
    elif ! cache_line_seen "$out" "$err"; then
        reason="-cache did not map the shared cache"
    elif [ -s "$CACHE_OUT" ] && ! cmp -s "$out" "$CACHE_OUT"; then
        reason="stdout differs from the plain cache-mode run"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_last_flag_native() {
    local name=last_flag_native rc reason="" out="$TMP/last_flag_native.out" err="$TMP/last_flag_native.err"
    run_bounded "$out" "$err" "$OCERZ" -v -cache -native "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    if [ "$rc" -ne 71 ]; then
        reason="'-cache -native' exit $rc, want 71"
    elif cache_line_seen "$out" "$err"; then
        reason="'-cache -native' mapped the shared cache"
    else
        reason="$(native_report_reason "$out" "$err")"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_last_flag_cache() {
    local name=last_flag_cache rc reason="" out="$TMP/last_flag_cache.out" err="$TMP/last_flag_cache.err"
    if [ "$CACHE_OK" -ne 1 ]; then
        echo "SKIP $name (shared cache not mappable here)"; return
    fi
    run_bounded "$out" "$err" "$OCERZ" -v -native -cache "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        reason="'-native -cache' exit $rc, want 0"
    elif ! cache_line_seen "$out" "$err"; then
        reason="'-native -cache' did not map the shared cache"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_bad_mode() {
    local name=bad_mode rc reason="" out="$TMP/bad_mode.out" err="$TMP/bad_mode.err"
    run_bounded "$out" "$err" env OCERZ_MODE=hybrid "$OCERZ" "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    if [ "$rc" -ne 64 ]; then
        reason="exit $rc, want 64"
    elif ! grep -q 'ocerz:' "$out" "$err"; then
        reason="refused without a named message"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_empty_mode() {
    local name=empty_mode rc reason="" out="$TMP/empty_mode.out" err="$TMP/empty_mode.err"
    if [ "$CACHE_OK" -ne 1 ]; then
        echo "SKIP $name (shared cache not mappable here)"; return
    fi
    run_bounded "$out" "$err" env OCERZ_MODE= "$OCERZ" -v "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        reason="exit $rc, want 0"
    elif ! cache_line_seen "$out" "$err"; then
        reason="an empty OCERZ_MODE did not fall back to cache mode"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_native_static() {
    local name=native_static rc reason="" out="$TMP/native_static.out" err="$TMP/native_static.err"
    run_bounded "$out" "$err" "$OCERZ" -v -native "$STATIC"
    rc=$?
    if [ "$rc" -eq 71 ]; then
        reason="exit 71: reached the import fixups instead of refusing a static image"
    elif [ "$rc" -ne 64 ]; then
        reason="exit $rc, want 64"
    elif ! grep -q 'ocerz:' "$out" "$err"; then
        reason="refused without a named message"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_native_dyn
case_native_summary
case_cache_dyn
case_env_native
case_flag_beats_env
case_last_flag_native
case_last_flag_cache
case_bad_mode
case_empty_mode
case_native_static

echo "----------------------------------------"
echo "native tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

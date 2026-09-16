#!/usr/bin/env bash
# Gate for the native execution mode (M1): the mode exists, it is selectable
# from the command line and from the environment, and with the shared cache
# switched off a dynamically linked guest binds every one of its system imports
# against a libSystem that ocerz synthesizes in memory, reaches main, and stops
# at the first call that would need a bridge nobody has written yet.
#
# That last step is what M1 moved, and it is why the pass for the native cases
# is now exit 72. Under M0 nothing answered a libSystem import at all: every
# one was collected at the end of the main image's fixups, printed as an
# "ocerz: native: no bridge for <sym> in <dylib>" line, and the process exited
# 71 without the guest running an instruction of its own. Under M1 the imports
# resolve to stubs inside a virtual /usr/lib/libSystem.B.dylib, so the guest
# runs, and the first call through one of those stubs prints a single line and
# exits 72:
#
#     ocerz: bridge: /usr/lib/libSystem.B.dylib _strcmp not implemented
#
# Keeping both codes is the point of keeping both messages. 71 means nothing
# bound, so the loader never handed control to the guest at all; 72 means
# everything bound, the guest ran, and what is missing is the bridge behind one
# export rather than the export itself. After M1 a native run that came back 71
# would be a binding regression wearing the same clothes as a pass, so the
# native cases assert exit 72 AND that no "no bridge for" line was printed at
# all -- either alone would let the other failure through. No committed fixture
# still fails to bind, xbench_dyn's four imports all being exports the virtual
# libSystem guarantees, so native_unbound compiles a two-line one at test time
# that calls getpwnam, which is deliberately outside the export list, and
# requires 71 with that symbol named. It skips where there is no x86_64 clang,
# the way the dynamic gate does. Without it the whole 71 path would go
# untested the moment M1 landed.
#
# Which symbol trips the bridge first is deliberately not pinned. It is
# whichever system function the compiled code for main reaches first, so it
# moves with the fixture's optimisation level and with what libc inlined into
# it, and pinning it would make this gate fail for a reason that has nothing to
# do with the mode. What is asserted instead is the shape of the line, the
# library it names, and that the symbol it names is one the fixture imports.
#
# The fixture is tests/guest/benchbin/xbench_dyn: dynamically linked, LC_MAIN,
# one dependency (/usr/lib/libSystem.B.dylib) and exactly four imports, all
# four of them exports the virtual libSystem guarantees. It writes its checksum
# with a raw write syscall rather than libc, so cache mode still gives
# deterministic stdout.
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
LIB=/usr/lib/libSystem.B.dylib
SYMS="___bzero _memcpy _strcmp _strlen"
BRIDGE_RE='^ocerz: bridge: [^ ]+ [^ ]+ not implemented$'
NOBIND='ocerz: native: no bridge for '
M0_SUMMARY='unresolved imports, no virtual frameworks are implemented yet'

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

bridge_sym() {
    grep -hE "$BRIDGE_RE" "$@" 2>/dev/null | head -1 | awk '{print $4}'
}

# empty on success, otherwise what is wrong with the bridge report
native_bridge_reason() {
    local line lib sym
    if grep -Fq "$NOBIND" "$@"; then
        echo "an import went unresolved: $(grep -hF "$NOBIND" "$@" | head -1)"
        return
    fi
    line=$(grep -hE "$BRIDGE_RE" "$@" | head -1)
    if [ -z "$line" ]; then
        echo "no 'ocerz: bridge: <dylib> <sym> not implemented' line"
        return
    fi
    lib=$(printf '%s\n' "$line" | awk '{print $3}')
    sym=$(printf '%s\n' "$line" | awk '{print $4}')
    if [ "$lib" != "$LIB" ]; then
        echo "bridge named library $lib, want $LIB"
        return
    fi
    case " $SYMS " in
        *" $sym "*) echo "" ;;
        *) echo "bridge named $sym, which the fixture does not import" ;;
    esac
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
    local name=native_dyn rc reason="" sym
    run_bounded "$NOUT" "$NERR" "$OCERZ" -v -native "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    if [ "$rc" -ne 72 ]; then
        reason="exit $rc, want 72"
    elif cache_line_seen "$NOUT" "$NERR"; then
        reason="native mode mapped the shared cache"
    else
        reason="$(native_bridge_reason "$NOUT" "$NERR")"
    fi
    sym=$(bridge_sym "$NOUT" "$NERR")
    record "$name" "$reason" "exit=$rc${sym:+ sym=$sym}"
}

case_native_bound() {
    local name=native_bound reason="" n
    n=$(grep -hF "$NOBIND" "$NOUT" "$NERR" 2>/dev/null | wc -l | tr -d ' ')
    if [ "$n" != "0" ]; then
        reason="$n imports did not bind against the virtual libSystem"
    elif grep -Fq "$M0_SUMMARY" "$NOUT" "$NERR"; then
        reason="the M0 unresolved-import summary is still printed"
    fi
    record "$name" "$reason" "unbound=$n"
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
    local name=env_native rc reason="" sym out="$TMP/env_native.out" err="$TMP/env_native.err"
    run_bounded "$out" "$err" env OCERZ_MODE=native "$OCERZ" -v "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    if [ "$rc" -ne 72 ]; then
        reason="exit $rc, want 72"
    elif cache_line_seen "$out" "$err"; then
        reason="OCERZ_MODE=native mapped the shared cache"
    else
        reason="$(native_bridge_reason "$out" "$err")"
    fi
    sym=$(bridge_sym "$out" "$err")
    record "$name" "$reason" "exit=$rc${sym:+ sym=$sym}"
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
    local name=last_flag_native rc reason="" sym out="$TMP/last_flag_native.out" err="$TMP/last_flag_native.err"
    run_bounded "$out" "$err" "$OCERZ" -v -cache -native "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    if [ "$rc" -ne 72 ]; then
        reason="'-cache -native' exit $rc, want 72"
    elif cache_line_seen "$out" "$err"; then
        reason="'-cache -native' mapped the shared cache"
    else
        reason="$(native_bridge_reason "$out" "$err")"
    fi
    sym=$(bridge_sym "$out" "$err")
    record "$name" "$reason" "exit=$rc${sym:+ sym=$sym}"
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

case_native_unbound() {
    local name=native_unbound rc reason="" src="$TMP/unbound.c" bin="$TMP/unbound"
    local out="$TMP/native_unbound.out" err="$TMP/native_unbound.err"
    cat > "$src" <<'EOC'
#include <pwd.h>
int main(void) { return getpwnam("root") != 0; }
EOC
    if ! clang -arch x86_64 -fno-stack-protector -o "$bin" "$src" >/dev/null 2>&1; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    run_bounded "$out" "$err" "$OCERZ" -v -native "$bin"
    rc=$?
    if [ "$rc" -ne 71 ]; then
        reason="exit $rc, want 71"
    elif ! grep -Fq "${NOBIND}_getpwnam" "$out" "$err"; then
        reason="no 'no bridge for _getpwnam' line"
    elif ! grep -Fq "$M0_SUMMARY" "$out" "$err"; then
        reason="no unresolved-import summary line"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_native_static() {
    local name=native_static rc reason="" out="$TMP/native_static.out" err="$TMP/native_static.err"
    run_bounded "$out" "$err" "$OCERZ" -v -native "$STATIC"
    rc=$?
    if [ "$rc" -eq 71 ] || [ "$rc" -eq 72 ]; then
        reason="exit $rc: reached the loader instead of refusing a static image"
    elif [ "$rc" -ne 64 ]; then
        reason="exit $rc, want 64"
    elif ! grep -q 'ocerz:' "$out" "$err"; then
        reason="refused without a named message"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_native_dyn
case_native_bound
case_cache_dyn
case_env_native
case_flag_beats_env
case_last_flag_native
case_last_flag_cache
case_bad_mode
case_empty_mode
case_native_static
case_native_unbound

echo "----------------------------------------"
echo "native tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

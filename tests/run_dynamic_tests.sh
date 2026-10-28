#!/usr/bin/env bash
# End-to-end tests for the mini-dyld: real dynamically-linked Mach-O programs
# against the shared cache. Unlike the static guest tests, these exercise the
# dyld cache mapping, the initializer ordering and the host workqueue bridge.
# Skipped, not failed, when there is no x86_64 clang toolchain or no mappable
# shared cache.

set -u
cd "$(dirname "$0")/.."
export OCERZ_TCACHE="${OCERZ_TCACHE:-off}"
OCERZ=./ocerz
OCERZ_ABS="$(pwd)/ocerz"
TMP="${TMPDIR:-/tmp}/ocerz_dyn.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

if ! clang -arch x86_64 -x c -o "$TMP/probe" - >/dev/null 2>&1 <<<'int main(void){return 0;}'; then
    echo "run_dynamic_tests: SKIP (no x86_64 clang toolchain)"
    exit 0
fi
if ! "$OCERZ" "$TMP/probe" >/dev/null 2>&1; then
    rc=$?
    if [ "$rc" = 70 ] || [ "$rc" = 65 ]; then
        echo "run_dynamic_tests: SKIP (shared cache not mappable here)"
        exit 0
    fi
fi

pass=0
fail=0

TIMEOUT_BIN=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_BIN="gtimeout"
fi
DYNAMIC_TIMEOUT=30

run_bounded() {
    local out_file="$1" err_file="$2"
    shift 2
    if [ -n "$TIMEOUT_BIN" ]; then
        "$TIMEOUT_BIN" "${DYNAMIC_TIMEOUT}s" "$@" >"$out_file" 2>"$err_file"
        return $?
    fi
    "$@" >"$out_file" 2>"$err_file" &
    local pid=$!
    local waited=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$waited" -ge "$DYNAMIC_TIMEOUT" ]; then
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

run_case() {
    local name="$1" src="$2" want_out="$3" want_code="$4"
    printf '%s' "$src" > "$TMP/$name.c"
    if ! clang -arch x86_64 -o "$TMP/$name" "$TMP/$name.c" 2>/dev/null; then
        echo "FAIL $name (compile)"; fail=$((fail+1)); return
    fi
    local got_out got_code
    got_out=$("$OCERZ" "$TMP/$name" 2>/dev/null)
    got_code=$?
    if [ "$got_out" = "$want_out" ] && [ "$got_code" = "$want_code" ]; then
        echo "PASS $name (out='$got_out' exit=$got_code)"; pass=$((pass+1))
    else
        echo "FAIL $name (got out='$got_out' exit=$got_code; want out='$want_out' exit=$want_code)"; fail=$((fail+1))
    fi
}

# The same, under -native, where the program's libSystem calls are the host's, crossed by the bridge.
run_native_file_case() {
    local name="$1" src="$2" want_out="$3"
    shift 3
    if ! clang -arch x86_64 -pthread -o "$TMP/$name" "$src" "$@" 2>/dev/null; then
        echo "FAIL $name (compile)"; fail=$((fail+1)); return
    fi
    local mode got_out got_code out_file err_file
    for mode in native native-no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = native-no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -native -no-jit "$TMP/$name"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" -native "$TMP/$name"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"
            fail=$((fail+1))
        fi
    done
}

run_file_case() {
    local name="$1" src="$2" want_out="$3"
    shift 3
    if ! clang -arch x86_64 -pthread -o "$TMP/$name" "$src" "$@" 2>/dev/null; then
        echo "FAIL $name (compile)"; fail=$((fail+1)); return
    fi

    local mode got_out got_code out_file err_file
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$TMP/$name"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$TMP/$name"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"
            fail=$((fail+1))
        fi
    done
}

# like run_file_case but a Swift source, for the Swift runtime and Swift
# concurrency; skips cleanly if no x86_64 Swift toolchain is present.
run_swift_file_case() {
    local name="$1" src="$2" want_out="$3"
    shift 3
    if ! xcrun swiftc -target x86_64-apple-macos13 -O -o "$TMP/$name" "$src" "$@" 2>/dev/null; then
        echo "SKIP $name (no x86_64 swift toolchain)"; return
    fi

    local mode got_out got_code out_file err_file
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$TMP/$name"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$TMP/$name"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"
            fail=$((fail+1))
        fi
    done
}

# like run_file_case but a C++ source, so the Itanium unwinder and libc++ are
# exercised; skips cleanly if no x86_64 C++ toolchain is present.
run_cpp_file_case() {
    local name="$1" src="$2" want_out="$3"
    shift 3
    if ! clang++ -arch x86_64 -std=c++17 -pthread -o "$TMP/$name" "$src" "$@" 2>/dev/null; then
        echo "SKIP $name (no x86_64 c++ toolchain)"; return
    fi

    local mode got_out got_code out_file err_file
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$TMP/$name"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$TMP/$name"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"
            fail=$((fail+1))
        fi
    done
}

run_relpath_case() {
    local name="$1" src="$2" want_out="$3"
    if ! clang -arch x86_64 -o "$TMP/$name" "$src" 2>/dev/null; then
        echo "FAIL $name (compile)"; fail=$((fail+1)); return
    fi
    local got_out got_code
    got_out=$( cd "$TMP" && "$OCERZ_ABS" "./$name" 2>/dev/null )
    got_code=$?
    if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
        echo "PASS $name (out='$got_out' exit=$got_code)"; pass=$((pass+1))
    else
        echo "FAIL $name (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
    fi
}

run_alias_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    mkdir -p "$dir/real"
    if ! clang -arch x86_64 -dynamiclib -install_name @executable_path/real/libalias_t.dylib \
            -o "$dir/real/libalias_t.dylib" tests/dynamic/dlopen_alias_lib.c 2>/dev/null ||
       ! ln "$dir/real/libalias_t.dylib" "$dir/real/libalias_hard.dylib" ||
       ! ln -s real "$dir/link" ||
       ! clang -arch x86_64 -o "$dir/$name" tests/dynamic/dlopen_alias.c "$dir/real/libalias_t.dylib" 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local mode got_out got_code
    for mode in jit no-jit; do
        if [ "$mode" = no-jit ]; then
            got_out=$( cd "$dir" && "$OCERZ_ABS" -no-jit "./$name" 2>/dev/null )
        else
            got_out=$( cd "$dir" && "$OCERZ_ABS" "./$name" 2>/dev/null )
        fi
        got_code=$?
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

# flat lookups from the loaded images only, and dlsym(NULL) refused (tests/dynamic/flat_lookup.c)
run_flat_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    mkdir -p "$dir"
    if ! clang -arch x86_64 -dynamiclib -install_name @executable_path/libflatdep.dylib \
            -o "$dir/libflatdep.dylib" tests/dynamic/flat_lookup_lib.c 2>/dev/null ||
       ! clang -arch x86_64 -bundle -undefined dynamic_lookup -o "$dir/flat_lookup.bundle" \
            tests/dynamic/flat_lookup_bundle.c 2>/dev/null ||
       ! clang -arch x86_64 -o "$dir/$name" tests/dynamic/flat_lookup.c "$dir/libflatdep.dylib" 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local mode got_out got_code
    for mode in jit no-jit; do
        if [ "$mode" = no-jit ]; then
            got_out=$( cd "$dir" && "$OCERZ_ABS" -no-jit "./$name" 2>/dev/null )
        else
            got_out=$( cd "$dir" && "$OCERZ_ABS" "./$name" 2>/dev/null )
        fi
        got_code=$?
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

run_dlopen_cf_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    mkdir -p "$dir"
    if ! clang -arch x86_64 -dynamiclib -install_name @executable_path/libcf_user.dylib \
            -framework CoreFoundation -o "$dir/libcf_user.dylib" tests/dynamic/dlopen_cf_lib.c 2>/dev/null ||
       ! clang -arch x86_64 -o "$dir/$name" tests/dynamic/dlopen_cf.c -lz 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local mode out_file err_file got_out got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$dir/$name"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$dir/$name"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

run_load_phase_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    mkdir -p "$dir"
    if ! clang -arch x86_64 -dynamiclib -install_name @executable_path/libload_phase.dylib \
            -framework CoreServices -o "$dir/libload_phase.dylib" tests/dynamic/dlopen_load_phase_lib.c 2>/dev/null ||
       ! clang -arch x86_64 -o "$dir/$name" tests/dynamic/dlopen_load_phase.c 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local mode out_file err_file got_out got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$dir/$name"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$dir/$name"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

run_caller_rpath_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    mkdir -p "$dir/sub/CallerRpath.framework"
    if ! clang -arch x86_64 -dynamiclib -install_name @rpath/CallerRpath.framework/CallerRpath \
            -o "$dir/sub/CallerRpath.framework/CallerRpath" tests/dynamic/dlopen_caller_rpath_fw.c 2>/dev/null ||
       ! clang -arch x86_64 -dynamiclib -install_name @rpath/libcaller_rpath.dylib -Wl,-rpath,@loader_path \
            -o "$dir/sub/libcaller_rpath.dylib" tests/dynamic/dlopen_caller_rpath_lib.c 2>/dev/null ||
       ! clang -arch x86_64 -o "$dir/$name" tests/dynamic/dlopen_caller_rpath.c 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local mode out_file err_file got_out got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$dir/$name"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$dir/$name"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

run_chain_rpath_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    mkdir -p "$dir/sub" "$dir/plug"
    if ! clang -arch x86_64 -dynamiclib -install_name @rpath/libchain_dep.dylib \
            -o "$dir/sub/libchain_dep.dylib" tests/dynamic/dlopen_chain_rpath_dep.c 2>/dev/null ||
       ! clang -arch x86_64 -dynamiclib -install_name @rpath/libchain_leaf.dylib \
            -o "$dir/sub/libchain_leaf.dylib" tests/dynamic/dlopen_chain_rpath_dep.c 2>/dev/null ||
       ! clang -arch x86_64 -dynamiclib -install_name "$dir/plug/libchain_plugin.dylib" \
            -o "$dir/plug/libchain_plugin.dylib" tests/dynamic/dlopen_chain_rpath_plugin.c \
            "$dir/sub/libchain_dep.dylib" 2>/dev/null ||
       ! clang -arch x86_64 -dynamiclib -install_name @rpath/libchain_loader.dylib -Wl,-rpath,@loader_path \
            -o "$dir/sub/libchain_loader.dylib" tests/dynamic/dlopen_chain_rpath_loader.c 2>/dev/null ||
       ! clang -arch x86_64 -o "$dir/$name" tests/dynamic/dlopen_chain_rpath.c 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local mode out_file err_file got_out got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$dir/$name"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$dir/$name"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

run_rpath_known_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    mkdir -p "$dir/link"
    if ! clang -arch x86_64 -dynamiclib -install_name @rpath/libknown.dylib \
            -o "$dir/link/libknown.dylib" tests/dynamic/rpath_known_lib.c 2>/dev/null ||
       ! clang -arch x86_64 -dynamiclib -install_name libknown.dylib \
            -o "$dir/libknown.dylib" tests/dynamic/rpath_known_lib.c 2>/dev/null ||
       ! clang -arch x86_64 -dynamiclib -install_name @rpath/libknown_user.dylib -Wl,-rpath,@loader_path \
            -o "$dir/libknown_user.dylib" tests/dynamic/rpath_known_user.c "$dir/link/libknown.dylib" 2>/dev/null ||
       ! clang -arch x86_64 -o "$dir/$name" tests/dynamic/rpath_known.c 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local mode out_file err_file got_out got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$dir/$name"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$dir/$name"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

run_wine_segment_case() {
    local name="$1"
    local dir="$TMP/$name"
    mkdir -p "$dir"
    printf x > "$dir/reserve"
    if ! clang -arch x86_64 -o "$dir/cmd.exe" tests/dynamic/wine_segment.c \
            -Wl,-sectcreate,WINE_RESERVE,__reserve,"$dir/reserve" 2>/dev/null ||
       ! clang -arch x86_64 -o "$dir/plain.exe" tests/dynamic/wine_segment.c 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local exe want out_file err_file got_out got_code
    for exe in cmd.exe plain.exe; do
        want=wine
        [ "$exe" = plain.exe ] && want=plain
        out_file="$TMP/$name.$exe.out"
        err_file="$TMP/$name.$exe.err"
        run_bounded "$out_file" "$err_file" env -u OCERZ_PRELOAD_OBJC "$OCERZ" "$dir/$exe"
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$exe (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$exe (got out='$got_out' exit=$got_code; want out='$want' exit=0)"; fail=$((fail+1))
        fi
    done
}

run_mac_syscall_low_stack_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    mkdir -p "$dir"
    if ! clang -arch x86_64 -dynamiclib -install_name @executable_path/libmac_syscall_low_stack.dylib \
            -o "$dir/libmac_syscall_low_stack.dylib" tests/dynamic/mac_syscall_low_stack_lib.c 2>/dev/null ||
       ! clang -arch x86_64 -o "$dir/$name" tests/dynamic/mac_syscall_low_stack.c \
            -Wl,-no_pie -Wl,-pagezero_size,0x1000 -Wl,-image_base,0x200000000 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local mode out_file err_file got_out got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$dir/$name"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$dir/$name"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

run_built_case() {
    local name="$1" want_out="$2" dir="$3"
    local mode out_file err_file got_out got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$dir/$name"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$dir/$name"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

run_weak_unloaded_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    mkdir -p "$dir"
    if ! clang -arch x86_64 -dynamiclib -install_name @executable_path/libweak_unloaded.dylib \
            -o "$dir/libweak_unloaded.dylib" tests/dynamic/weak_unloaded_lib.c 2>/dev/null ||
       ! clang -arch x86_64 -o "$dir/$name" tests/dynamic/weak_unloaded.c 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    run_built_case "$name" "$want_out" "$dir"
}

run_metal_nocopy_low_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    mkdir -p "$dir"
    if ! clang -arch x86_64 -fobjc-arc -framework Metal -framework Foundation -o "$dir/$name" \
            tests/dynamic/metal_nocopy_low.m \
            -Wl,-no_pie -Wl,-pagezero_size,0x1000 -Wl,-image_base,0x200000000 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    run_built_case "$name" "$want_out" "$dir"
}

run_rpath_system_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    mkdir -p "$dir/bundled"
    if ! clang -arch x86_64 -dynamiclib -install_name @rpath/libswiftCore.dylib \
            -o "$dir/bundled/libswiftCore.dylib" tests/dynamic/native_swift_bundled_lib.c 2>/dev/null ||
       ! clang -arch x86_64 -o "$dir/$name" tests/dynamic/native_swift_bundled.c -L"$dir/bundled" -lswiftCore \
            -Wl,-rpath,/usr/lib/swift -Wl,-rpath,@executable_path/bundled 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    run_built_case "$name" "$want_out" "$dir"
}

run_iosurface_low_stack_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    mkdir -p "$dir"
    if ! clang -arch x86_64 -framework IOSurface -framework CoreFoundation -o "$dir/$name" \
            tests/dynamic/iosurface_low_stack.c \
            -Wl,-no_pie -Wl,-pagezero_size,0x1000 -Wl,-image_base,0x200000000 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    run_built_case "$name" "$want_out" "$dir"
}

run_dlsym_deps_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    mkdir -p "$dir"
    local lib=tests/dynamic/dlsym_deps_lib.c
    if ! clang -arch x86_64 -dynamiclib -DLIB_C -install_name @rpath/libdc.dylib -o "$dir/libdc.dylib" "$lib" 2>/dev/null ||
       ! clang -arch x86_64 -dynamiclib -DLIB_U -install_name @rpath/libdu.dylib -o "$dir/libdu.dylib" "$lib" 2>/dev/null ||
       ! clang -arch x86_64 -dynamiclib -DLIB_B -install_name @rpath/libdb.dylib -o "$dir/libdb.dylib" "$lib" \
            -L"$dir" -ldc -Wl,-rpath,@loader_path 2>/dev/null ||
       ! clang -arch x86_64 -dynamiclib -install_name @rpath/libda.dylib -o "$dir/libda.dylib" "$lib" \
            -L"$dir" -ldb -Wl,-upward-ldu -Wl,-rpath,@loader_path 2>/dev/null ||
       ! clang -arch x86_64 -o "$dir/$name" tests/dynamic/dlsym_deps.c 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local mode out_file err_file got_out got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$dir/$name" "$dir/libda.dylib"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$dir/$name" "$dir/libda.dylib"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

# A test whose expected output is a golden file, Rosetta's stdout, too long to
# echo into every PASS line; a failure names the first differing line.
run_golden_case() {
    local name="$1" src="$2" golden="$3"
    if ! clang -arch x86_64 -O1 -o "$TMP/$name" "$src" 2>/dev/null; then
        echo "FAIL $name (compile)"; fail=$((fail+1)); return
    fi
    local mode out_file err_file got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$TMP/$name"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$TMP/$name"
        fi
        got_code=$?
        if [ "$got_code" = 0 ] && cmp -s "$out_file" "$golden"; then
            echo "PASS $name-$mode"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (exit=$got_code; first difference: $(diff "$golden" "$out_file" | sed -n 2p))"
            fail=$((fail+1))
        fi
    done
}

run_low_golden_case() {
    local name="$1" src="$2" golden="$3"
    local dir="$TMP/$name"
    mkdir -p "$dir"
    if ! clang -arch x86_64 -O2 -Itests/guest -o "$dir/$name" "$src" \
            -Wl,-no_pie -Wl,-pagezero_size,0x1000 -Wl,-image_base,0x200000000 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local mode out_file err_file got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$dir/$name"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$dir/$name"
        fi
        got_code=$?
        if [ "$got_code" = 0 ] && cmp -s "$out_file" "$golden"; then
            echo "PASS $name-$mode"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (exit=$got_code, output differs from $golden)"; fail=$((fail+1))
        fi
    done
}

run_dlopen_arch_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    mkdir -p "$dir"
    if ! clang -arch arm64 -dynamiclib -o "$dir/libarch_arm64.dylib" tests/dynamic/dlopen_arch_lib.c 2>/dev/null ||
       ! clang -arch x86_64 -arch arm64 -dynamiclib -o "$dir/libarch_fat.dylib" tests/dynamic/dlopen_arch_lib.c 2>/dev/null ||
       ! clang -arch x86_64 -o "$dir/$name" tests/dynamic/dlopen_arch.c 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local mode out_file err_file got_out got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$dir/$name" "$dir/libarch_arm64.dylib" "$dir/libarch_fat.dylib"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$dir/$name" "$dir/libarch_arm64.dylib" "$dir/libarch_fat.dylib"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

run_rpath_bare_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    mkdir -p "$dir/sub"
    if ! clang -arch x86_64 -dynamiclib -install_name @rpath/libexe_dep.dylib \
            -o "$dir/libexe_dep.dylib" tests/dynamic/rpath_bare_dep.c 2>/dev/null ||
       ! clang -arch x86_64 -dynamiclib -install_name @rpath/libuser_dep.dylib \
            -o "$dir/sub/libuser_dep.dylib" tests/dynamic/rpath_bare_dep.c 2>/dev/null ||
       ! clang -arch x86_64 -dynamiclib -install_name @rpath/librpath_user.dylib -Wl,-rpath,@loader_path \
            -o "$dir/sub/librpath_user.dylib" tests/dynamic/rpath_bare_user.c "$dir/sub/libuser_dep.dylib" 2>/dev/null ||
       ! clang -arch x86_64 -Wl,-rpath,@executable_path -o "$dir/$name" \
            tests/dynamic/rpath_bare_main.c "$dir/libexe_dep.dylib" 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local mode out_file err_file got_out got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$dir/$name" "$dir"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$dir/$name" "$dir"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

run_init_order_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    mkdir -p "$dir"
    if ! clang -arch x86_64 -dynamiclib -install_name @rpath/libinit_order.dylib \
            -o "$dir/libinit_order.dylib" tests/dynamic/init_order_lib.c 2>/dev/null ||
       ! clang -arch x86_64 -Wl,-rpath,@executable_path -o "$dir/$name" \
            tests/dynamic/init_order_main.c "$dir/libinit_order.dylib" 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local mode out_file err_file got_out got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$dir/$name"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$dir/$name"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

run_upward_defer_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    local lib="-arch x86_64 -dynamiclib"
    mkdir -p "$dir"
    if ! clang $lib -install_name @rpath/libud_log.dylib -o "$dir/libud_log.dylib" \
            tests/dynamic/upward_defer_log.c 2>/dev/null ||
       ! clang $lib -install_name @rpath/libud_top.dylib -o "$dir/libud_top.dylib" -DUD_NAME='"top"' \
            tests/dynamic/upward_defer_lib.c "$dir/libud_log.dylib" 2>/dev/null ||
       ! clang $lib -install_name @rpath/libud_mid.dylib -o "$dir/libud_mid.dylib" -DUD_NAME='"mid"' \
            tests/dynamic/upward_defer_lib.c "$dir/libud_log.dylib" \
            -Wl,-upward_library,"$dir/libud_top.dylib" 2>/dev/null ||
       ! clang $lib -install_name @rpath/libud_core.dylib -o "$dir/libud_core.dylib" -DUD_NAME='"core"' \
            tests/dynamic/upward_defer_lib.c "$dir/libud_log.dylib" "$dir/libud_mid.dylib" 2>/dev/null ||
       ! clang $lib -install_name @rpath/libud_top.dylib -o "$dir/libud_top.dylib" -DUD_NAME='"top"' \
            tests/dynamic/upward_defer_lib.c "$dir/libud_log.dylib" "$dir/libud_core.dylib" 2>/dev/null ||
       ! clang -arch x86_64 -Wl,-rpath,@executable_path -o "$dir/$name" tests/dynamic/upward_defer.c \
            "$dir/libud_log.dylib" "$dir/libud_core.dylib" 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local mode out_file err_file got_out got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$dir/$name"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$dir/$name"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

run_asm_case() {
    local name="$1" c_src="$2" s_src="$3" want_out="$4"
    if ! clang -arch x86_64 -O2 -o "$TMP/$name" "$c_src" "$s_src" 2>/dev/null; then
        echo "FAIL $name (compile)"; fail=$((fail+1)); return
    fi
    local mode out_file err_file got_out got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$TMP/$name"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$TMP/$name"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

run_spawn_argv_case() {
    local name="$1"
    local dir="$TMP/$name"
    mkdir -p "$dir"
    if ! clang -arch x86_64 -o "$dir/Steam Helper" tests/dynamic/spawn_argv_child.c 2>/dev/null ||
       ! clang -arch x86_64 -o "$dir/Other Helper" tests/dynamic/spawn_argv_child.c 2>/dev/null ||
       ! clang -arch x86_64 -o "$dir/$name" tests/dynamic/spawn_argv_parent.c 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local label child knob want got
    for label in steam other optout; do
        child="$dir/Steam Helper"; knob=""; want="--use-mock-keychain,--type=renderer"
        [ "$label" = other ] && { child="$dir/Other Helper"; want="--type=renderer"; }
        [ "$label" = optout ] && { knob="OCERZ_NO_MOCK_KEYCHAIN=1"; want="--type=renderer"; }
        got=$(env $knob "$OCERZ_ABS" "$dir/$name" "$child" 2>/dev/null)
        if [ "$got" = "$want" ]; then
            echo "PASS $name-$label (out='$got')"; pass=$((pass+1))
        else
            echo "FAIL $name-$label (got out='$got'; want out='$want')"; fail=$((fail+1))
        fi
    done
}

run_legacy_format_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    mkdir -p "$dir"
    if ! clang -arch x86_64 -mmacosx-version-min=10.5 -dynamiclib -o "$dir/liblegacy.dylib" \
            tests/dynamic/legacy_format_lib.c 2>/dev/null ||
       ! clang -arch x86_64 -o "$dir/$name" tests/dynamic/legacy_format.c 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    if otool -l "$dir/liblegacy.dylib" | grep -qE 'LC_DYLD_INFO|LC_DYLD_CHAINED_FIXUPS'; then
        echo "FAIL $name (the linker emitted compressed fixups, so the case tests nothing)"; fail=$((fail+1)); return
    fi
    local mode out_file err_file got_out got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$dir/$name" "$dir/liblegacy.dylib"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$dir/$name" "$dir/liblegacy.dylib"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

run_app_bundle_case() {
    local name="$1" want_out="$2"
    local app="$TMP/$name/Probe.app"
    mkdir -p "$app/Contents/MacOS"
    if ! clang -arch x86_64 -o "$app/Contents/MacOS/probe_exe" tests/dynamic/app_bundle.c 2>/dev/null; then
        echo "FAIL $name (compile)"; fail=$((fail+1)); return
    fi
    cat > "$app/Contents/Info.plist" <<'EOP'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key>
	<string>probe_exe</string>
	<key>CFBundleIdentifier</key>
	<string>org.aarchx.probe</string>
</dict>
</plist>
EOP
    local mode out_file err_file got_out got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$app"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$app"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

run_insert_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    mkdir -p "$dir"
    if ! clang -arch x86_64 -arch arm64 -dynamiclib -o "$dir/libinsert.dylib" tests/dynamic/insert_lib.c 2>/dev/null ||
       ! clang -arch x86_64 -o "$dir/$name" tests/dynamic/insert_main.c 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local mode out_file err_file got_out got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" env DYLD_INSERT_LIBRARIES="$dir/libinsert.dylib" "$OCERZ" -no-jit "$dir/$name"
        else
            run_bounded "$out_file" "$err_file" env DYLD_INSERT_LIBRARIES="$dir/libinsert.dylib" "$OCERZ" "$dir/$name"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

run_legacy_entry_case() {
    local name="$1" want_out="$2"
    if ! clang -arch x86_64 -mmacosx-version-min=10.6 -o "$TMP/$name" tests/dynamic/legacy_entry.c 2>/dev/null; then
        echo "FAIL $name (compile)"; fail=$((fail+1)); return
    fi
    if ! otool -l "$TMP/$name" | grep -q LC_UNIXTHREAD; then
        echo "FAIL $name (linker did not emit LC_UNIXTHREAD)"; fail=$((fail+1)); return
    fi
    local mode out_file err_file got_out got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" env LEGACY_ENTRY_PROBE=yes "$OCERZ" -no-jit "$TMP/$name" one two
        else
            run_bounded "$out_file" "$err_file" env LEGACY_ENTRY_PROBE=yes "$OCERZ" "$TMP/$name" one two
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

run_idname_case() {
    local name="$1" want_out="$2"
    local dir="$TMP/$name"
    mkdir -p "$dir/real" "$dir/decoy" "$dir/stub"
    if ! clang -arch x86_64 -dynamiclib -install_name @rpath/libidname_base.dylib \
            -o "$dir/real/libidname_base.dylib" tests/dynamic/idname_base.c 2>/dev/null ||
       ! clang -arch x86_64 -dynamiclib -install_name @rpath/libidname_decoy.dylib \
            -o "$dir/decoy/libidname_decoy.dylib" tests/dynamic/idname_decoy.c 2>/dev/null ||
       ! clang -arch x86_64 -dynamiclib -install_name @rpath/libidname_user.dylib -Wl,-rpath,@loader_path/stub \
            -o "$dir/libidname_user.dylib" tests/dynamic/idname_user.c "$dir/real/libidname_base.dylib" 2>/dev/null ||
       ! clang -arch x86_64 -o "$dir/$name" tests/dynamic/idname_main.c 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    printf 'link libidname_base.dylib' > "$dir/stub/libidname_base.dylib"
    local mode out_file err_file got_out got_code
    for mode in jit no-jit; do
        out_file="$TMP/$name.$mode.out"
        err_file="$TMP/$name.$mode.err"
        if [ "$mode" = no-jit ]; then
            run_bounded "$out_file" "$err_file" "$OCERZ" -no-jit "$dir/$name"
        else
            run_bounded "$out_file" "$err_file" "$OCERZ" "$dir/$name"
        fi
        got_code=$?
        got_out=$(cat "$out_file")
        if [ "$got_out" = "$want_out" ] && [ "$got_code" = 0 ]; then
            echo "PASS $name-$mode (out='$got_out' exit=$got_code)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (got out='$got_out' exit=$got_code; want out='$want_out' exit=0)"; fail=$((fail+1))
        fi
    done
}

run_case dret 'int main(void){return 42;}' '' 42
# The x86 commpage carries this Mac's CPU counts and memory size, the ones sysctl answers (it carried another
# machine's: 12 CPUs and 32 GB on a 10-CPU, 16 GB Mac).
run_case dcommpage_counts '
#include <stdint.h>
#include <stdio.h>
#include <sys/sysctl.h>
int main(void) {
  volatile uint8_t *cp = (volatile uint8_t *)0x7fffffe00000ull;
  int lc = 0, ac = 0, pc = 0; uint64_t mem = 0; size_t l;
  l = sizeof lc; sysctlbyname("hw.logicalcpu_max", &lc, &l, 0, 0);
  l = sizeof ac; sysctlbyname("hw.activecpu", &ac, &l, 0, 0);
  l = sizeof pc; sysctlbyname("hw.physicalcpu_max", &pc, &l, 0, 0);
  l = sizeof mem; sysctlbyname("hw.memsize", &mem, &l, 0, 0);
  int ok = cp[0x22] == lc && cp[0x36] == lc && cp[0x34] == ac && cp[0x35] == pc &&
           *(volatile uint64_t *)(cp + 0x38) == mem && ((*(volatile uint32_t *)(cp + 0x20) >> 16) & 0xff) == (uint32_t)lc;
  puts(ok ? "commpage counts ok" : "commpage counts differ");
  return 0;
}' 'commpage counts ok' 0
# gettimeofday reads the commpage's time-of-day block, which the syscall fills, instead of making the syscall
# every time: against the raw syscall it stays within 2 ms, it never goes backwards, and a 1.3 s sleep reads as one.
run_case dcommpage_tod '
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
static long us(struct timeval t) { return t.tv_sec * 1000000L + t.tv_usec; }
int main(void) {
  struct timeval a, b, k; long back = 0, worst = 0;
  gettimeofday(&a, 0);
  for (int i = 0; i < 200000; i++) {
    gettimeofday(&b, 0);
    if (us(b) < us(a) || b.tv_usec < 0 || b.tv_usec >= 1000000) back++;
    a = b;
    if ((i & 1023) == 0) {
      syscall(SYS_gettimeofday, &k, 0, 0);
      long e = us(k) - us(b); if (e < 0) e = -e;
      if (e > worst) worst = e;
    }
  }
  usleep(1300000);
  gettimeofday(&b, 0);
  long jump = us(b) - us(a);
  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
  long e = ts.tv_sec * 1000000L + ts.tv_nsec / 1000 - us(b);
  printf("%s\n", back == 0 && worst < 2000 && jump > 1250000 && jump < 1800000 && e >= 0 && e < 2000 ? "tod ok" : "tod wrong");
  return 0;
}' 'tod ok' 0
run_case dwrite '
int main(void){
  const char m[]="dyn raw syscall ok\n"; long r;
  __asm__ volatile("syscall":"=a"(r):"a"(0x2000004),"D"(1),"S"(m),"d"(sizeof(m)-1):"rcx","r11","memory");
  return 9;
}' 'dyn raw syscall ok' 9

run_case dlinkver '
#include <stdint.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
extern const struct mach_header_64 _mh_execute_header;
static int same(const char *a, const char *b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}
int main(void) {
  const struct mach_header_64 *h = &_mh_execute_header;
  const unsigned char *p = (const unsigned char *)(h + 1);
  uint32_t expected_link = 0, expected_runtime = 0;
  for (uint32_t i = 0; i < h->ncmds; ++i) {
    const struct load_command *lc = (const void *)p;
    if (lc->cmd == LC_LOAD_DYLIB || lc->cmd == LC_LOAD_WEAK_DYLIB ||
        lc->cmd == LC_REEXPORT_DYLIB || lc->cmd == LC_LOAD_UPWARD_DYLIB) {
      const struct dylib_command *dc = (const void *)p;
      const char *name = (const char *)p + dc->dylib.name.offset;
      if (same(name, "/usr/lib/libSystem.B.dylib"))
        expected_link = dc->dylib.current_version;
    }
    p += lc->cmdsize;
  }
  for (uint32_t image = 0; image < _dyld_image_count(); ++image) {
    h = (const struct mach_header_64 *)_dyld_get_image_header(image);
    if (!h)
      continue;
    p = (const unsigned char *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; ++i) {
      const struct load_command *lc = (const void *)p;
      if (lc->cmd == LC_ID_DYLIB) {
        const struct dylib_command *dc = (const void *)p;
        const char *name = (const char *)p + dc->dylib.name.offset;
        if (same(name, "/usr/lib/libSystem.B.dylib"))
          expected_runtime = dc->dylib.current_version;
      }
      p += lc->cmdsize;
    }
  }
  int32_t got_link = NSVersionOfLinkTimeLibrary("System");
  int32_t got_runtime = NSVersionOfRunTimeLibrary("System");
  return expected_link && expected_runtime &&
         (uint32_t)got_link == expected_link &&
         (uint32_t)got_runtime == expected_runtime ? 0 : 91;
}' '' 0

# A forked child's clock is its parent's: under -fex macOS runs the arm64 counter FEXCore read at 24 MHz in a
# forked child and at 1 GHz in an executed program, the child's time ran half again too fast, and a daemonized
# wineserver timed out every wait at once.
run_case dfork_clock '
#include <stdint.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <mach/mach_time.h>
int main(void) {
  int fd[2];
  if (pipe(fd)) return 1;
  mach_timebase_info_data_t tb;
  mach_timebase_info(&tb);
  uint64_t before = mach_absolute_time(), c = 0;
  pid_t p = fork();
  if (p == 0) { usleep(100000); c = mach_absolute_time(); write(fd[1], &c, sizeof c); _exit(0); }
  read(fd[0], &c, sizeof c);
  waitpid(p, 0, 0);
  uint64_t after = mach_absolute_time();
  double ms = (double)(c - before) * tb.numer / tb.denom / 1e6;
  puts(c > before && c <= after && ms >= 90 ? "fork clock ok" : "fork clock differs");
  return 0;
}' 'fork clock ok' 0
# A call through a null pointer is a SIGSEGV at address 0 the handler can leave, each time: under -fex it was a
# fault in FEXCore's compiler reading code at 0, then a branch to host address 0 from an empty lookup entry.
run_case dnull_call '
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
static sigjmp_buf jb;
static void *volatile addr = (void *)1;
static volatile int sig;
static void h(int s, siginfo_t *si, void *uc) { (void)uc; sig = s; addr = si->si_addr; siglongjmp(jb, 1); }
int main(void) {
  struct sigaction sa = {0};
  sa.sa_sigaction = h;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, 0);
  sigaction(SIGBUS, &sa, 0);
  void (*volatile f)(void) = 0;
  for (int i = 0; i < 3; i++) {
    sig = 0;
    if (!sigsetjmp(jb, 1))
      f();
    if (sig != SIGSEGV || addr != 0) { printf("null call %d: signal %d at %p\n", i, sig, addr); return 1; }
  }
  puts("null call SIGSEGV at 0");
  return 0;
}' 'null call SIGSEGV at 0' 0

run_file_case dfork_signal tests/dynamic/fork_signal.c 'fork signal ok'
run_file_case dthread_signal tests/dynamic/thread_signal.c 'OK'
run_file_case dsignal_restart tests/dynamic/signal_restart.c 'OK'
run_native_file_case dsignal_restart tests/dynamic/signal_restart.c 'OK'
run_file_case dx87_top_entry tests/dynamic/x87_top_entry.c 'OK'
run_file_case dshmem_coherence tests/dynamic/shmem_coherence.c 'OK'
run_file_case drsp_rmw tests/dynamic/rsp_rmw.c '554ae911bb61770a' -mno-red-zone
run_file_case dcvt_packed tests/dynamic/cvt_packed.c '144 25523f4977e3d6c4'
run_golden_case dx87_arith tests/dynamic/x87_arith.c tests/dynamic/x87_arith.out
run_golden_case dx87_compare tests/dynamic/x87_compare.c tests/dynamic/x87_compare.out
run_golden_case dx87_int tests/dynamic/x87_int.c tests/dynamic/x87_int.out
run_golden_case dx87_trans tests/dynamic/x87_trans.c tests/dynamic/x87_trans.out
run_golden_case dx87_state tests/dynamic/x87_state.c tests/dynamic/x87_state.out
run_golden_case drep_string tests/dynamic/rep_string.c tests/dynamic/rep_string.out
run_golden_case dcomis_mem tests/dynamic/comis_mem.c tests/dynamic/comis_mem.out
run_golden_case dstack_pair tests/dynamic/stack_pair.c tests/dynamic/stack_pair.out
run_golden_case dstack_run tests/dynamic/stack_run.c tests/dynamic/stack_run.out
run_golden_case drot_flags tests/dynamic/rot_flags.c tests/dynamic/rot_flags.out
run_golden_case dincdec_flags tests/dynamic/incdec_flags.c tests/dynamic/incdec_flags.out
run_golden_case dinline_misc tests/dynamic/inline_misc.c tests/dynamic/inline_misc.out
run_golden_case dblock_cap tests/dynamic/block_cap.c tests/dynamic/block_cap.out
run_golden_case dptest_flags tests/dynamic/ptest_flags.c tests/dynamic/ptest_flags.out
run_golden_case dshift_cl_flags tests/dynamic/shift_cl_flags.c tests/dynamic/shift_cl_flags.out
run_golden_case dret_flags_seam tests/dynamic/ret_flags_seam.c tests/dynamic/ret_flags_seam.out
run_golden_case dshufps_self tests/dynamic/shufps_self.c tests/dynamic/shufps_self.out
run_golden_case dcmp_mem_setcc tests/dynamic/cmp_mem_setcc.c tests/dynamic/cmp_mem_setcc.out
run_golden_case dleaf_low tests/dynamic/leaf_low.c tests/dynamic/leaf_low.out
run_golden_case dslot_jump tests/dynamic/slot_jump.c tests/dynamic/slot_jump.out
run_golden_case dwine_guard tests/dynamic/wine_guard.c tests/dynamic/wine_guard.out
run_golden_case dlowstack_disp tests/dynamic/lowstack_disp.c tests/dynamic/lowstack_disp.out
run_golden_case dlowstack_ptr tests/dynamic/lowstack_ptr.c tests/dynamic/lowstack_ptr.out
run_golden_case dwine_base tests/dynamic/wine_base.c tests/dynamic/wine_base.out
run_golden_case dlazy_rsp tests/dynamic/lazy_rsp.c tests/dynamic/lazy_rsp.out
run_golden_case dnzcv_window tests/dynamic/nzcv_window.c tests/dynamic/nzcv_window.out
run_golden_case dnzcv_regen_fault tests/dynamic/nzcv_regen_fault.c tests/dynamic/nzcv_regen_fault.out
run_golden_case dxlive_self tests/dynamic/xlive_self.c tests/dynamic/xlive_self.out
export OCERZ_NO_PROMO=1
run_golden_case dlazy_rsp_nopromo tests/dynamic/lazy_rsp.c tests/dynamic/lazy_rsp.out
unset OCERZ_NO_PROMO
run_golden_case dtest_jcc_gap tests/dynamic/test_jcc_gap.c tests/dynamic/test_jcc_gap.out
run_golden_case dsetcc_zx tests/dynamic/setcc_zx.c tests/dynamic/setcc_zx.out
run_golden_case dhoist_forms tests/dynamic/hoist_forms.c tests/dynamic/hoist_forms.out
run_golden_case dlow_loop_hoist tests/dynamic/low_loop_hoist.c tests/dynamic/low_loop_hoist.out
run_golden_case dfist_rc tests/dynamic/fist_rc.c tests/dynamic/fist_rc.out
run_golden_case dflip_phase tests/dynamic/flip_phase.c tests/dynamic/flip_phase.out
run_file_case dunaligned_atomics tests/dynamic/unaligned_atomics.c 'OK'
run_file_case dlane_fault_guard tests/dynamic/lane_fault_guard.c 'OK' -Wl,-no_pie
run_file_case dsmc_io tests/dynamic/smc_io.c 'OK'
run_file_case dsmc_high tests/dynamic/smc_high.c 'OK'
run_file_case dsmc_ras tests/dynamic/smc_ras.c 'OK'
run_file_case dpsc_call_retire tests/dynamic/psc_call_retire.c 'OK'
run_file_case dsmc_chain tests/dynamic/smc_chain.c 'OK'
# every chain through a veneer, as far targets are chained in a big process
export OCERZ_FORCE_VENEER=1
run_file_case dsmc_chain_veneer tests/dynamic/smc_chain.c 'OK'
run_file_case dsmc_ras_veneer tests/dynamic/smc_ras.c 'OK'
run_golden_case dflip_phase_veneer tests/dynamic/flip_phase.c tests/dynamic/flip_phase.out
run_golden_case dxlive_self_veneer tests/dynamic/xlive_self.c tests/dynamic/xlive_self.out
unset OCERZ_FORCE_VENEER
run_low_golden_case dsmc_ras_low tests/dynamic/smc_ras.c tests/dynamic/smc_ras.out
run_low_golden_case dsmc_chain_low tests/dynamic/smc_chain.c tests/dynamic/smc_chain.out
run_file_case dtc_policy tests/dynamic/tc_policy.c 'OK'
run_low_golden_case dtc_policy_low tests/dynamic/tc_policy.c tests/dynamic/tc_policy.out
run_file_case dbyte_atomics tests/dynamic/byte_atomics.c 'OK'
run_file_case dspawn_pipe tests/dynamic/spawn_pipe.c 'OK'
run_file_case dread_block tests/dynamic/read_block.c 'OK'
run_file_case dthread_suspend tests/dynamic/thread_suspend.c 'OK'
run_file_case dfileport tests/dynamic/fileport.c 'OK'
run_file_case dx86_sysctl tests/dynamic/x86_sysctl.c 'OK'
run_file_case ditimer tests/dynamic/itimer.c 'OK'
run_file_case ddyld_apis tests/dynamic/dyld_apis.c 'OK'
run_file_case dsignal_wait tests/dynamic/signal_wait.c 'OK'
run_file_case dpreadv tests/dynamic/preadv.c 'OK'
run_file_case dsysv_sem tests/dynamic/sysv_sem.c 'OK'
run_file_case drel_acq_order tests/dynamic/rel_acq_order.c 'OK'
run_file_case ddep_order tests/dynamic/dep_order.c 'OK'
# dep_order names its three loads; in ordered mode only the first, whose result the second is
# addressed through, may be made plain, and the last of the chain must stay an acquire.
if run_bounded "$TMP/ddep_order.log.out" "$TMP/ddep_order.log.err" env OCERZ_DEP_PLAIN_LOG=1 "$OCERZ" "$TMP/ddep_order" &&
   read -r _ dep_l1 dep_l2 dep_l3 < <(grep '^loads ' "$TMP/ddep_order.log.err") &&
   grep -q "DEP_PLAIN $dep_l1\$" "$TMP/ddep_order.log.err" &&
   ! grep -q -e "DEP_PLAIN $dep_l2\$" -e "DEP_PLAIN $dep_l3\$" "$TMP/ddep_order.log.err"; then
    echo "PASS ddep_order_plain (only the load the next one depends on is plain)"; pass=$((pass+1))
else
    echo "FAIL ddep_order_plain (loads ${dep_l1:-?} ${dep_l2:-?} ${dep_l3:-?}; $(grep -c DEP_PLAIN "$TMP/ddep_order.log.err") plain)"
    fail=$((fail+1))
fi
# Between the first two loads, a store to a global, a release in ordered memory, keeps the first one
# plain; a store through rsp, which stays plain, does not.
if read -r _ dep_r1 dep_s1 < <(grep '^stores ' "$TMP/ddep_order.log.err") &&
   grep -q "DEP_PLAIN $dep_r1\$" "$TMP/ddep_order.log.err" &&
   ! grep -q "DEP_PLAIN $dep_s1\$" "$TMP/ddep_order.log.err"; then
    echo "PASS ddep_order_store (a release store between keeps the load plain, a stack store does not)"; pass=$((pass+1))
else
    echo "FAIL ddep_order_store (stores ${dep_r1:-?} ${dep_s1:-?})"
    fail=$((fail+1))
fi
# sr_switch.c under the single runner (OCERZ_SINGLE_RUNNER=1, src/jit.c): the
# litmus must hold, and OCERZ_SR_VERIFY's sampler must have looked while two
# threads ran and found none of them in a plain block; the process must have
# switched and stayed able to go plain, except after a writable shared mapping,
# which makes it ordered for good.  Those runs switch back to plain at once
# (OCERZ_SR_HOLD_US=0); "held" keeps the default hold, under which the reader's
# short sleeps hardly ever let the process go plain.  Without the knob the
# litmus runs as is.
run_sr_case() {
    local name="$1" dir="$TMP/$1" arg want_sticky hold prog_arg line why
    mkdir -p "$dir"
    if ! clang -arch x86_64 -O1 -o "$dir/$name" tests/dynamic/sr_switch.c 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    for arg in plain fork shared held; do
        want_sticky=0 hold=0 prog_arg=$arg
        [ $arg = shared ] && want_sticky=1
        [ $arg = held ] && hold=500 prog_arg=plain
        OCERZ_TCACHE=off OCERZ_SINGLE_RUNNER=1 OCERZ_SR_VERIFY=1 OCERZ_SR_LOG=1 OCERZ_SR_HOLD_US=$hold \
            run_bounded "$dir/$arg.out" "$dir/$arg.err" "$OCERZ" "$dir/$name" $prog_arg
        line=$(grep -h 'SINGLE-RUNNER' "$dir/$arg.err" | tail -1)
        why=""
        if [ "$(cat "$dir/$arg.out")" != OK ]; then
            why="got out='$(cat "$dir/$arg.out")'"
        elif ! echo "$line" | grep -q "sticky=$want_sticky "; then
            why="want sticky=$want_sticky: $line"
        elif [ $want_sticky = 0 ] && [ $hold = 0 ] && ! echo "$line" | grep -Eq 'switches=[0-9]{2,}'; then
            why="no switches: $line"
        elif [ $want_sticky = 0 ] && ! echo "$line" | grep -Eq 'samples=[0-9]{2,}'; then
            why="the sampler never looked: $line"
        elif ! echo "$line" | grep -q 'violations=0$'; then
            why="a thread ran a plain block next to another: $line"
        fi
        if [ -z "$why" ]; then
            echo "PASS $name-$arg"; pass=$((pass+1))
        else
            echo "FAIL $name-$arg ($why)"; fail=$((fail+1))
        fi
    done
    OCERZ_TCACHE=off run_bounded "$dir/off.out" "$dir/off.err" "$OCERZ" "$dir/$name"
    if [ "$(cat "$dir/off.out")" = OK ]; then
        echo "PASS $name-off"; pass=$((pass+1))
    else
        echo "FAIL $name-off (got out='$(cat "$dir/off.out")')"; fail=$((fail+1))
    fi
}

run_sr_case dsr_switch
# hw_tso.c and hardware TSO (src/cpu.c; src/jit.c, "memory ordering").  -v
# names the decision once, and a multithreaded run still goes ordered and keeps
# x86's order whatever it was: without the entitlement the kernel refuses
# thread_set_x86_64_compat (kr=5 on macOS 27) and software keeps it.
# OCERZ_NO_HW_TSO=1 never asks.  Under the unsafe OCERZ_PRETEND_HW_TSO ordered
# code loads and stores plainly and only its atomics must still hold
# ("atomic"); with OCERZ_PRETEND_HW_TSO_REFUSE=2 the first new thread is
# refused, the process gives the order up, and all of it must hold again, here
# and in native mode.  Then three launches against one store, the knobs being
# outside its fingerprint: a pretending one writes ordered records keyed for the
# order (OCERZ_TC_KEY_HWTSO in the shared cache's), one that never asks shares
# the store but must look none of them up, and one refused mid-run may look
# them up only before it is refused; both litmus runs must find nothing.
hwtso_result() {
    if [ -z "$2" ]; then
        echo "PASS $1"; pass=$((pass+1))
    else
        echo "FAIL $1 ($2)"; fail=$((fail+1))
    fi
}
run_hwtso_case() {
    local name="$1" dir="$TMP/$1" why n m
    mkdir -p "$dir"
    if ! clang -arch x86_64 -O1 -o "$dir/$name" tests/dynamic/hw_tso.c 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    OCERZ_ORDERLOG=1 run_bounded "$dir/def.out" "$dir/def.err" "$OCERZ" -v "$dir/$name"
    why=""
    n=$(grep -cE '^ocerz: hardware TSO: (on|off)' "$dir/def.err")
    if [ "$(cat "$dir/def.out")" != OK ]; then
        why="got out='$(cat "$dir/def.out")'"
    elif [ "$n" != 1 ]; then
        why="$n decision lines"
    elif ! grep -qE '^ocerz: hardware TSO: (on$|off \(kr=[0-9-]+\)$|off \(.*(macOS|kernel))' "$dir/def.err"; then
        why="decision: $(grep -E '^ocerz: hardware TSO' "$dir/def.err" | head -1)"
    elif ! grep -q 'ORDERED memory required' "$dir/def.err"; then
        why="its threads did not make it ordered"
    fi
    hwtso_result "$name-default" "$why"
    OCERZ_NO_HW_TSO=1 run_bounded "$dir/no.out" "$dir/no.err" "$OCERZ" -v "$dir/$name"
    why=""
    if [ "$(cat "$dir/no.out")" != OK ]; then
        why="got out='$(cat "$dir/no.out")'"
    elif ! grep -q '^ocerz: hardware TSO: off (OCERZ_NO_HW_TSO)$' "$dir/no.err"; then
        why="no 'off (OCERZ_NO_HW_TSO)' line"
    fi
    hwtso_result "$name-off" "$why"
    OCERZ_PRETEND_HW_TSO=1 run_bounded "$dir/at.out" "$dir/at.err" "$OCERZ" -v "$dir/$name" atomic
    why=""
    if [ "$(cat "$dir/at.out")" != OK ]; then
        why="the atomics broke: got out='$(cat "$dir/at.out")'"
    elif ! grep -q '^ocerz: hardware TSO: on (pretended' "$dir/at.err"; then
        why="not pretended"
    fi
    hwtso_result "$name-atomic" "$why"
    for m in jit native; do
        if [ $m = native ]; then
            OCERZ_PRETEND_HW_TSO=1 OCERZ_PRETEND_HW_TSO_REFUSE=2 \
                run_bounded "$dir/rf.out" "$dir/rf.err" "$OCERZ" -v -native "$dir/$name"
        else
            OCERZ_PRETEND_HW_TSO=1 OCERZ_PRETEND_HW_TSO_REFUSE=2 \
                run_bounded "$dir/rf.out" "$dir/rf.err" "$OCERZ" -v "$dir/$name"
        fi
        why=""
        if [ "$(cat "$dir/rf.out")" != OK ]; then
            why="got out='$(cat "$dir/rf.out")'"
        elif ! grep -q '^ocerz: hardware TSO: refused to thread' "$dir/rf.err"; then
            why="no thread was refused"
        elif ! grep -q '^ocerz: hardware TSO: given up' "$dir/rf.err"; then
            why="the process kept the order"
        fi
        hwtso_result "$name-refused-$m" "$why"
    done
    OCERZ_TCACHE=on OCERZ_TCACHE_DIR="$dir/store" OCERZ_TCACHE_MIN_FREE_MB=0 OCERZ_TCACHE_LOG=1 OCERZ_TCACHE_TRACE=1 \
        OCERZ_PRETEND_HW_TSO=1 run_bounded "$dir/t1.out" "$dir/t1.err" "$OCERZ" -v "$dir/$name" count
    OCERZ_TCACHE=on OCERZ_TCACHE_DIR="$dir/store" OCERZ_TCACHE_MIN_FREE_MB=0 OCERZ_TCACHE_LOG=1 OCERZ_TCACHE_TRACE=1 \
        OCERZ_NO_HW_TSO=1 run_bounded "$dir/t2.out" "$dir/t2.err" "$OCERZ" -v "$dir/$name" count
    OCERZ_TCACHE=on OCERZ_TCACHE_DIR="$dir/store" OCERZ_TCACHE_MIN_FREE_MB=0 OCERZ_TCACHE_LOG=1 OCERZ_TCACHE_TRACE=1 \
        OCERZ_PRETEND_HW_TSO=1 OCERZ_PRETEND_HW_TSO_REFUSE=2 \
        run_bounded "$dir/t3.out" "$dir/t3.err" "$OCERZ" -v "$dir/$name" count
    local salted='key=0x1[0-9a-f]{15} ' s1 s2 gave
    s1=$(grep -h 'TCACHE.* store ' "$dir/t1.err" | sed 's/.* store //; s/ .*//')
    s2=$(grep -h 'TCACHE.* store ' "$dir/t2.err" | sed 's/.* store //; s/ .*//')
    gave=$(grep -n '^ocerz: hardware TSO: given up' "$dir/t3.err" | head -1 | cut -d: -f1)
    why=""
    if ! grep -qE "$salted" "$dir/t1.err"; then
        why="the pretending launch keyed no record for the order"
    elif [ -z "$s1" ] || [ "$s1" != "$s2" ]; then
        why="the launches used different stores ('$s1', '$s2')"
    elif [ "$(cat "$dir/t2.out")" != 0 ] || [ "$(cat "$dir/t3.out")" != 0 ]; then
        why="forbidden outcomes: '$(cat "$dir/t2.out")' without the order, '$(cat "$dir/t3.out")' refused"
    elif grep -qE "$salted" "$dir/t2.err"; then
        why="a launch without the order looked up a record made for it"
    elif ! grep -q ' HIT ' "$dir/t2.err"; then
        why="the launch without the order loaded nothing from the shared store"
    elif [ -z "$gave" ]; then
        why="the refused launch kept the order"
    elif tail -n +"$gave" "$dir/t3.err" | grep -qE "$salted"; then
        why="the refused launch looked up a record made for the order after giving it up"
    fi
    hwtso_result "$name-tcache" "$why"
}

run_hwtso_case dhw_tso
# src/syslink.c: an ocerz an import of which loads NULL (a macOS that moved it to another of libSystem's
# libraries) executes ocerz-umbrella beside its real file, found through a symbolic link, with its arguments, and
# with none there says so and exits 127; OCERZ_TEST_SYSLINK_FALLBACK=1 takes that path with every import there.
# The ocerz-umbrella the build makes must run as ocerz does.
run_syslink_case() {
    local dir="$TMP/dsyslink" out rc
    mkdir -p "$dir/real" "$dir/bin"
    cp "$OCERZ" "$dir/real/ocerz"
    ln -sf ../real/ocerz "$dir/bin/ocerz"
    printf '#!/bin/sh\necho "umbrella $*"\n' > "$dir/real/ocerz-umbrella"
    chmod +x "$dir/real/ocerz-umbrella"
    out=$(OCERZ_TEST_SYSLINK_FALLBACK=1 "$dir/bin/ocerz" --version x 2>&1)
    if [ "$out" = "umbrella --version x" ]; then
        echo "PASS dsyslink-fallback"; pass=$((pass+1))
    else
        echo "FAIL dsyslink-fallback (got '$out')"; fail=$((fail+1))
    fi
    rm -f "$dir/real/ocerz-umbrella"
    out=$(OCERZ_TEST_SYSLINK_FALLBACK=1 "$dir/bin/ocerz" --version 2>&1)
    rc=$?
    if [ $rc = 127 ] && echo "$out" | grep -q 'reinstall AArchX'; then
        echo "PASS dsyslink-missing"; pass=$((pass+1))
    else
        echo "FAIL dsyslink-missing (rc=$rc out='$out')"; fail=$((fail+1))
    fi
    out=$("$(dirname "$OCERZ")/ocerz-umbrella" --version 2>&1)
    if [ "$out" = "$("$OCERZ" --version 2>&1)" ] && [ -n "$out" ]; then
        echo "PASS dsyslink-umbrella"; pass=$((pass+1))
    else
        echo "FAIL dsyslink-umbrella (got '$out')"; fail=$((fail+1))
    fi
}

# The cache holder (src/cache_holder.c), with its lock in a directory of the
# test's own and a 3-second idle: the first launch starts one, the guest's wait
# for all its children does not wait for it (it is nobody's child but
# launchd's), a second launch starts none and only touches the lock, the
# holder goes by itself once no launch comes, and OCERZ_NO_CACHE_HOLDER=1
# starts none.  Only a holder whose command line names this test's directory is
# ever stopped.
hold_stop() {
    local p
    for p in $(pgrep -f -- "-hold-cache $1/" 2>/dev/null); do kill "$p" 2>/dev/null; done
}
run_holder_case() {
    local dir="$TMP/dholder" lock pid pid2 out t0 t1 m1 m2 gone=0 i
    mkdir -p "$dir/a" "$dir/b"
    t0=$(date +%s)
    out=$(env -u OCERZ_NO_CACHE_HOLDER OCERZ_CACHE_HOLDER_DIR="$dir/a" OCERZ_CACHE_HOLDER_IDLE=3 \
          "$OCERZ" /bin/sh -c 'wait; echo waited' 2>&1)
    t1=$(date +%s)
    lock=$(ls "$dir/a"/ocerz-cache-holder-* 2>/dev/null | head -1)
    sleep 0.5
    pid=$([ -n "$lock" ] && head -1 "$lock")
    if [ "$out" = waited ] && [ $((t1 - t0)) -le 2 ] && [ -n "$pid" ] &&
       ps -o ppid=,command= -p "$pid" 2>/dev/null | grep -q -- "^ *1 .*-hold-cache $dir/a/"; then
        echo "PASS dholder-start"; pass=$((pass+1))
    else
        echo "FAIL dholder-start (out='$out' secs=$((t1 - t0)) pid='$pid' ps='$(ps -o ppid=,command= -p "${pid:-0}" 2>&1)')"
        fail=$((fail+1))
    fi
    m1=$(stat -f %m "$lock" 2>/dev/null)
    sleep 1.2
    out=$(env -u OCERZ_NO_CACHE_HOLDER OCERZ_CACHE_HOLDER_DIR="$dir/a" OCERZ_CACHE_HOLDER_IDLE=3 \
          "$OCERZ" /usr/bin/uname -m 2>&1)
    m2=$(stat -f %m "$lock" 2>/dev/null)
    pid2=$(head -1 "$lock" 2>/dev/null)
    if [ "$out" = x86_64 ] && [ "$pid2" = "$pid" ] && [ -n "$m1" ] && [ "${m2:-0}" -gt "$m1" ] &&
       [ "$(pgrep -f -- "-hold-cache $dir/a/" | wc -l | tr -d ' ')" = 1 ]; then
        echo "PASS dholder-again"; pass=$((pass+1))
    else
        echo "FAIL dholder-again (out='$out' pid=$pid pid2=$pid2 mtime $m1 -> $m2)"; fail=$((fail+1))
    fi
    for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        if ! kill -0 "$pid" 2>/dev/null; then gone=1; break; fi
        sleep 0.5
    done
    if [ $gone = 1 ]; then
        echo "PASS dholder-idle"; pass=$((pass+1))
    else
        echo "FAIL dholder-idle (holder $pid still there 10 s after the last launch)"; fail=$((fail+1))
    fi
    out=$(OCERZ_NO_CACHE_HOLDER=1 OCERZ_CACHE_HOLDER_DIR="$dir/b" "$OCERZ" /usr/bin/uname -m 2>&1)
    if [ "$out" = x86_64 ] && [ -z "$(ls "$dir/b")" ]; then
        echo "PASS dholder-off"; pass=$((pass+1))
    else
        echo "FAIL dholder-off (out='$out' dir: $(ls "$dir/b"))"; fail=$((fail+1))
    fi
    hold_stop "$dir/a"
    hold_stop "$dir/b"
    # the store: the holder maps its index once it is there, and lets go of it
    # once the store is gone (the lock's time kept fresh by hand meanwhile)
    if [ -x /usr/sbin/lsof ]; then
        mkdir -p "$dir/c"
        out=$(env -u OCERZ_NO_CACHE_HOLDER OCERZ_TCACHE=on OCERZ_TCACHE_DIR="$dir/tc" OCERZ_CACHE_HOLDER_DIR="$dir/c" \
              OCERZ_CACHE_HOLDER_IDLE=8 "$OCERZ" /usr/bin/uname -m 2>&1)
        lock=$(ls "$dir/c"/ocerz-cache-holder-* 2>/dev/null | head -1)
        sleep 5
        pid=$([ -n "$lock" ] && head -1 "$lock")
        local before after real
        real=$(cd "$dir" && pwd -P)   # lsof names files by their resolved paths (/private/var/folders/...)
        before=$(/usr/sbin/lsof -p "${pid:-0}" 2>/dev/null | grep -c "$real/tc/tc-[0-9a-f]*/index")
        rm -rf "$dir/tc"
        touch "$lock" 2>/dev/null
        sleep 5
        after=$(/usr/sbin/lsof -p "${pid:-0}" 2>/dev/null | grep -c "$real/tc/")
        if [ "$out" = x86_64 ] && [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && [ "$before" = 1 ] && [ "$after" = 0 ]; then
            echo "PASS dholder-store"; pass=$((pass+1))
        else
            echo "FAIL dholder-store (out='$out' pid='$pid' index mapped before=$before after=$after)"; fail=$((fail+1))
        fi
        hold_stop "$dir/c"
    else
        echo "SKIP dholder-store: no lsof"
    fi
}

run_syslink_case
run_holder_case
run_file_case datomic_counter tests/dynamic/atomic_counter.c 'OK'
run_swift_file_case dswift_task_sync tests/dynamic/swift_task_sync.swift 'sum 40425'
run_file_case dfp_rounding tests/dynamic/fp_rounding.c 'OK'
export OCERZ_TSO_NARROW=1
run_file_case dtso_narrow tests/dynamic/tso_narrow.c 'OK'
unset OCERZ_TSO_NARROW
run_file_case dnan_contexts tests/dynamic/nan_contexts.c 'OK'
run_file_case dupward_init tests/dynamic/upward_init.c 'OK' -framework CoreFoundation
run_file_case dcache_symlink_dep tests/dynamic/cache_symlink_dep.c 'OK'
run_file_case dsysv_shm tests/dynamic/sysv_shm.c 'OK'
run_file_case ddlopen_image_list tests/dynamic/dlopen_image_list.c 'OK'
run_file_case ddlopen_cryptex tests/dynamic/dlopen_cryptex.c 'OK'
run_file_case ddlopen_cache_alias tests/dynamic/dlopen_cache_alias.c 'OK'
run_legacy_format_case dlegacy_format 'OK'
run_app_bundle_case dapp_bundle 'MacOS probe_exe'
run_insert_case dinsert_libraries "$(printf 'inserted\nmain env=kept carrier=gone')"
dyldslots_out=$(tools/dyldslots.sh --check 2>&1)
case "$dyldslots_out" in
    PASS*) echo "$dyldslots_out"; pass=$((pass+1)) ;;
    SKIP*) echo "$dyldslots_out" ;;
    *) echo "$dyldslots_out"; fail=$((fail+1)) ;;
esac
run_file_case dspawn_arm64_only tests/dynamic/spawn_arm64_only.c 'OK'
run_file_case dsocket_echo tests/dynamic/socket_echo.c 'OK'
run_cpp_file_case dcpp_exceptions tests/dynamic/cpp_exceptions.cpp 'OK'
run_cpp_file_case dcpp_global_ctor tests/dynamic/cpp_global_ctor.cpp 'OK'
run_cpp_file_case dweak_main_first tests/dynamic/weak_main_first.cpp 'OK'
run_cpp_file_case dweak_main_first_classic tests/dynamic/weak_main_first.cpp 'OK' -Wl,-no_fixup_chains
# tcache_work.c against one fresh translation cache directory, five runs (with
# no free-space floor, so a full disk on the test machine is not a failure): one
# that records, and leaves its program's image record keys in its profile's
# .keys for the warm thread, one that must load what the first stored, one under
# OCERZ_TCACHE=verify that must find no block differing from its record except
# in shape, one under OCERZ_TCACHE=roundtrip that must find no reference its
# relocations miss, one more verify with the program and its dylibs moved by
# OCERZ_GUEST_SLIDE, so a record of the program's own code is checked at
# another slide, and a last plain run at a third slide, whose warm thread
# hands out and whose loader takes ahead records of an image that moved.  The second run of the slid variant must load records of the
# program itself (images in=), the low variant's are kept by absolute address.
run_tcache_case() {
    local name="$1" want_out="$2"
    shift 2
    local dir="$TMP/$name"
    mkdir -p "$dir"
    if ! clang -arch x86_64 -O2 -o "$dir/$name" tests/dynamic/tcache_work.c "$@" 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local i=0 mode log got_code got_out why slide
    for mode in on on verify roundtrip verify on; do
        i=$((i + 1))
        log="$dir/run$i.log"
        slide=0
        [ $i = 5 ] && slide=0x400000
        [ $i = 6 ] && slide=0x800000
        OCERZ_TCACHE=$mode OCERZ_TCACHE_DIR="$dir/store" OCERZ_TCACHE_LOG="$log" OCERZ_TCACHE_MIN_FREE_MB=0 \
            OCERZ_GUEST_SLIDE=$slide run_bounded "$dir/run$i.out" "$dir/run$i.err" "$OCERZ" "$dir/$name"
        got_code=$?
        got_out=$(cat "$dir/run$i.out")
        why=""
        if [ "$got_out" != "$want_out" ] || [ "$got_code" != 0 ]; then
            why="got out='$got_out' exit=$got_code"
        elif [ $i = 2 ] && ! awk '/TCACHE.*loaded=/ { for (f = 3; f <= NF; f++) { split($f, a, "="); t[a[1]] += a[2] } }
                                  END { exit !(t["loaded"] > 0 && t["rejected"] == 0) }' "$log"; then
            why="nothing loaded: $(grep -h 'loaded=' "$log" | head -1)"
        elif [ $i = 2 ] && [ "$name" = dtcache ] && ! grep -q 'images asked=[0-9]* in=[1-9]' "$log"; then
            why="no image record loaded: $(grep -h 'images asked=' "$log" | head -1)"
        elif [ $i = 2 ] && [ "$name" = dtcache_low ] && ! grep -q 'loaded=[0-9]* (ahead [1-9]' "$log"; then
            why="the Wine layout loaded nothing ahead from its profile: $(grep -h 'loaded=' "$log" | head -1)"
        elif [ $i = 1 ] && [ "$name" = dtcache ] &&
             [ "$(cat "$dir"/store/profiles/*.keys 2>/dev/null | od -An -v -tx8 | tr -s ' ' '\n' | grep -c '^[2367abef]')" = 0 ]; then
            why="no image record key in the profile's .keys"
        elif { [ $i = 3 ] || [ $i = 5 ]; } && ! awk '/TCACHE.*loaded=/ { for (f = 3; f <= NF; f++) { split($f, a, "="); t[a[1]] += a[2] } }
                                  END { exit !(t["verify_ok"] > 0 && t["verify_bad"] == 0) }' "$log"; then
            why="verify: $(grep -h 'VERIFY\|loaded=' "$log" | head -2 | tr '\n' ' ')"
        elif [ $i = 4 ] && grep -q "PCREL\|SHAPE\|MISMATCH" "$log"; then
            why="roundtrip: $(grep -h 'PCREL\|SHAPE\|MISMATCH' "$log" | head -1)"
        fi
        if [ -z "$why" ]; then
            echo "PASS $name-$i-$mode"; pass=$((pass+1))
        else
            echo "FAIL $name-$i-$mode ($why)"; fail=$((fail+1))
        fi
    done
}

run_tcache_case dtcache 'OK'
# tcache_ordstart.c four times against one fresh store: a launch with a thread
# made at once must leave a .ord beside its profile, the next must start
# ordered, a "single" launch must take the note away and the one after it
# must start plain.
run_tcache_ord_case() {
    local name="$1" dir="$TMP/$1" i arg want why
    mkdir -p "$dir"
    if ! clang -arch x86_64 -O1 -o "$dir/$name" tests/dynamic/tcache_ordstart.c 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    for i in 1 2 3 4; do
        arg=threads
        [ $i -ge 3 ] && arg=single
        OCERZ_TCACHE=on OCERZ_TCACHE_DIR="$dir/store" OCERZ_TCACHE_LOG=1 OCERZ_TCACHE_MIN_FREE_MB=0 \
            run_bounded "$dir/run$i.out" "$dir/run$i.err" "$OCERZ" "$dir/$name" $arg
        why=""
        if [ "$(cat "$dir/run$i.out")" != OK ]; then
            why="got out='$(cat "$dir/run$i.out")'"
        else
            case $i in
                1) grep -q 'TCACHE.* wrote .*\.ord' "$dir/run$i.err" || why="no .ord written" ;;
                2) grep -q 'starting ordered' "$dir/run$i.err" || why="did not start ordered" ;;
                3) grep -q 'TCACHE.* removed .*\.ord' "$dir/run$i.err" || why="the .ord was not removed" ;;
                4) ! grep -q 'starting ordered' "$dir/run$i.err" || why="started ordered with no .ord" ;;
            esac
        fi
        if [ -z "$why" ]; then
            echo "PASS $name-$i-$arg"; pass=$((pass+1))
        else
            echo "FAIL $name-$i-$arg ($why)"; fail=$((fail+1))
        fi
    done
}

run_tcache_ord_case dtcache_ordstart
# tcache_patch.c against one fresh store: plain (atoi's records kept
# pristine), patched (atoi's page made writable and rewritten before the first
# call, which must reach the patch; with nothing loaded ahead, so atoi's record
# is found at that call, after the patch), then "wait" five times, each from
# the store the patched run left: the program sleeps until the warm thread has
# read every record and the next miss loads all of them ahead, the patched
# atoi's too, whose dep in the program (where the patched run had it) faults in
# its deps check inside the run of loads, which must leave the JIT's pages
# writable; most records must load without their deps check.  The installer
# (src/jit.c) loads them while the program sleeps, so the fault is its own, and
# it must have loaded some: told to start for a profile this small, which by
# itself starts none.  Whether atoi's
# record is among the ones loaded ahead depends on what loaded before the sleep,
# hence five tries.  A copy on APFS is a clone whose pages are not in memory,
# which leaves the warm thread behind the program and loads far fewer records
# ahead, so each copy is read through first.
run_tcache_patch_case() {
    local name="$1" dir="$TMP/$1" i arg why
    mkdir -p "$dir"
    if ! clang -arch x86_64 -O1 -o "$dir/$name" tests/dynamic/tcache_patch.c 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    for i in 1 2 3 4 5 6 7; do
        arg=wait
        [ $i = 1 ] && arg=plain
        [ $i = 2 ] && arg=patch
        if [ $i = 2 ]; then
            OCERZ_TCACHE=on OCERZ_TCACHE_DIR="$dir/store" OCERZ_TCACHE_LOG=1 OCERZ_TCACHE_MIN_FREE_MB=0 \
                OCERZ_TCACHE_AHEAD=0 OCERZ_NO_TCACHE_WARM=1 \
                run_bounded "$dir/run$i.out" "$dir/run$i.err" "$OCERZ" "$dir/$name" $arg
            rm -rf "$dir/patched"
            cp -R "$dir/store" "$dir/patched"
        elif [ $i = 1 ]; then
            OCERZ_TCACHE=on OCERZ_TCACHE_DIR="$dir/store" OCERZ_TCACHE_LOG=1 OCERZ_TCACHE_MIN_FREE_MB=0 \
                run_bounded "$dir/run$i.out" "$dir/run$i.err" "$OCERZ" "$dir/$name" $arg
        else
            rm -rf "$dir/store"
            cp -R "$dir/patched" "$dir/store"
            cat "$dir/store"/*/* >/dev/null
            OCERZ_TCACHE=on OCERZ_TCACHE_DIR="$dir/store" OCERZ_TCACHE_LOG=1 OCERZ_TCACHE_MIN_FREE_MB=0 \
                OCERZ_TCACHE_AHEAD=100000 OCERZ_TCACHE_INSTALLER_MIN=0 \
                run_bounded "$dir/run$i.out" "$dir/run$i.err" "$OCERZ" "$dir/$name" $arg
        fi
        why=""
        if [ "$(cat "$dir/run$i.out")" != OK ]; then
            why="got out='$(cat "$dir/run$i.out")' $(grep -h 'guest crash' "$dir/run$i.err" | head -1)"
        elif [ $i -ge 3 ] && ! grep -q 'loaded=.*pristine [1-9]' "$dir/run$i.err"; then
            why="no record skipped its deps check: $(grep -h 'loaded=' "$dir/run$i.err" | head -1)"
        elif [ $i -ge 3 ] && ! grep -q 'loaded=.*installer [1-9]' "$dir/run$i.err"; then
            why="the installer loaded nothing while the program slept: $(grep -h 'loaded=' "$dir/run$i.err" | head -1)"
        fi
        if [ -z "$why" ]; then
            echo "PASS $name-$i-$arg"; pass=$((pass+1))
        else
            echo "FAIL $name-$i-$arg ($why)"; fail=$((fail+1))
        fi
    done
}

run_tcache_patch_case dtcache_patch
run_tcache_case dtcache_low 'OK' -Wl,-no_pie -Wl,-pagezero_size,0x1000 -Wl,-image_base,0x200000000
# In the Wine layout a launch's profile is its Windows program's, the first .exe on its command line
# (src/dyld.c, launch_profile_ident): run as a.exe twice, the second launch loads a.exe's records ahead; the
# first launch as b.exe has a profile of its own, empty, and loads none ahead.
wp_dir="$TMP/dtcache_wine_profile"
mkdir -p "$wp_dir"
if ! clang -arch x86_64 -O2 -o "$wp_dir/work" tests/dynamic/tcache_work.c -Wl,-no_pie -Wl,-pagezero_size,0x1000 \
        -Wl,-image_base,0x200000000 2>/dev/null; then
    echo "FAIL dtcache_wine_profile (build)"; fail=$((fail+1))
else
    wp_why=""
    wp_i=0
    for wp_exe in a.exe a.exe b.exe; do
        wp_i=$((wp_i + 1))
        OCERZ_TCACHE=on OCERZ_TCACHE_DIR="$wp_dir/store" OCERZ_TCACHE_LOG=1 OCERZ_TCACHE_MIN_FREE_MB=0 \
            run_bounded "$wp_dir/run$wp_i.out" "$wp_dir/run$wp_i.err" "$OCERZ" "$wp_dir/work" "$wp_exe"
        if [ -z "$wp_why" ] && [ "$(cat "$wp_dir/run$wp_i.out")" != OK ]; then
            wp_why="run $wp_i ($wp_exe) printed '$(cat "$wp_dir/run$wp_i.out")'"
        elif [ -z "$wp_why" ] && [ $wp_i = 2 ] && ! grep -q 'loaded=[0-9]* (ahead [1-9]' "$wp_dir/run$wp_i.err"; then
            wp_why="a.exe's second launch loaded nothing ahead: $(grep -h 'loaded=' "$wp_dir/run$wp_i.err" | head -1)"
        elif [ -z "$wp_why" ] && [ $wp_i = 3 ] && ! grep -q 'loaded=[0-9]* (ahead 0,' "$wp_dir/run$wp_i.err"; then
            wp_why="b.exe's first launch loaded a.exe's profile: $(grep -h 'loaded=' "$wp_dir/run$wp_i.err" | head -1)"
        fi
    done
    if [ -z "$wp_why" ]; then
        echo "PASS dtcache_wine_profile (a.exe warm, b.exe its own)"; pass=$((pass+1))
    else
        echo "FAIL dtcache_wine_profile ($wp_why)"; fail=$((fail+1))
    fi
fi
# ocerz -aot fills a fresh store with tcache_work's own code without running
# it; the launch after it, at another slide, must print OK and find every block
# of the program it asks for in the store.
aot_dir="$TMP/dtcache_aot"
mkdir -p "$aot_dir"
if ! clang -arch x86_64 -O2 -o "$aot_dir/work" tests/dynamic/tcache_work.c 2>/dev/null; then
    echo "FAIL dtcache_aot (build)"; fail=$((fail+1))
else
    OCERZ_TCACHE=on OCERZ_TCACHE_DIR="$aot_dir/store" OCERZ_TCACHE_MIN_FREE_MB=0 \
        run_bounded "$aot_dir/aot.out" "$aot_dir/aot.err" "$OCERZ" -aot "$aot_dir/work"
    aot_code=$?
    OCERZ_TCACHE=on OCERZ_TCACHE_DIR="$aot_dir/store" OCERZ_TCACHE_LOG="$aot_dir/run.log" OCERZ_TCACHE_MIN_FREE_MB=0 \
        OCERZ_GUEST_SLIDE=0x400000 run_bounded "$aot_dir/run.out" "$aot_dir/run.err" "$OCERZ" "$aot_dir/work"
    aot_run_code=$?
    if [ "$aot_code" = 0 ] && [ "$aot_run_code" = 0 ] && [ "$(cat "$aot_dir/run.out")" = OK ] &&
       awk '/images asked=/ { for (f = 3; f <= NF; f++) { split($f, a, "="); t[a[1]] += a[2] } }
            END { exit !(t["asked"] > 0 && t["in"] - t["ahead"] == t["asked"]) }' "$aot_dir/run.log"; then
        echo "PASS dtcache_aot"; pass=$((pass+1))
    else
        echo "FAIL dtcache_aot (aot exit=$aot_code run exit=$aot_run_code out='$(cat "$aot_dir/run.out")'" \
             "$(grep -h 'images asked=' "$aot_dir/run.log" | head -1))"
        fail=$((fail+1))
    fi
    # That launch left a profile of the shared-cache code it ran beside the
    # store.  With the store itself gone, as after a rebuild of ocerz, -aot must
    # seed it from the profile, and the next launch must find (nearly) every
    # shared-cache block it asks for.
    rm -rf "$aot_dir"/store/tc-*
    OCERZ_TCACHE=on OCERZ_TCACHE_DIR="$aot_dir/store" OCERZ_TCACHE_MIN_FREE_MB=0 \
        run_bounded "$aot_dir/aotp.out" "$aot_dir/aotp.err" "$OCERZ" -aot "$aot_dir/work"
    aot_code=$?
    OCERZ_TCACHE=on OCERZ_TCACHE_DIR="$aot_dir/store" OCERZ_TCACHE_LOG="$aot_dir/runp.log" OCERZ_TCACHE_MIN_FREE_MB=0 \
        run_bounded "$aot_dir/runp.out" "$aot_dir/runp.err" "$OCERZ" "$aot_dir/work"
    aot_run_code=$?
    if [ "$aot_code" = 0 ] && [ "$aot_run_code" = 0 ] && [ "$(cat "$aot_dir/runp.out")" = OK ] &&
       grep -q ' [1-9][0-9]* profiled (0 dropped)' "$aot_dir/aotp.err" &&
       awk '/shared asked=/ { split($4, a, "="); split($5, b, "="); asked += a[2]; got += b[2] }
            END { exit !(asked > 0 && got * 100 >= asked * 98) }' "$aot_dir/runp.log"; then
        echo "PASS dtcache_aot_profile"; pass=$((pass+1))
    else
        echo "FAIL dtcache_aot_profile (aot exit=$aot_code run exit=$aot_run_code out='$(cat "$aot_dir/runp.out")'" \
             "$(grep -h 'profiled' "$aot_dir/aotp.err" | head -1) $(grep -h 'shared asked=' "$aot_dir/runp.log" | head -1))"
        fail=$((fail+1))
    fi
fi
# A launch's profile also names the disk images it loaded, a bundle it
# dlopened included (.images), and the program itself (.path).  With the store
# gone, as after a rebuild of ocerz, -aot-all must find the program from its
# profile and do the bundle as well, and the next launch must find every image
# block it asks for.
aoti_dir="$TMP/dtcache_aot_images"
mkdir -p "$aoti_dir"
if ! clang -arch x86_64 -O2 -o "$aoti_dir/main" tests/dynamic/tcache_plugin_main.c 2>/dev/null ||
   ! clang -arch x86_64 -O2 -bundle -o "$aoti_dir/plugin.bundle" tests/dynamic/tcache_plugin.c 2>/dev/null; then
    echo "FAIL dtcache_aot_images (build)"; fail=$((fail+1))
else
    OCERZ_TCACHE=on OCERZ_TCACHE_DIR="$aoti_dir/store" OCERZ_TCACHE_MIN_FREE_MB=0 \
        run_bounded "$aoti_dir/first.out" "$aoti_dir/first.err" "$OCERZ" "$aoti_dir/main" "$aoti_dir/plugin.bundle"
    first_code=$?
    rm -rf "$aoti_dir"/store/tc-*
    OCERZ_TCACHE=on OCERZ_TCACHE_DIR="$aoti_dir/store" OCERZ_TCACHE_MIN_FREE_MB=0 \
        run_bounded "$aoti_dir/all.out" "$aoti_dir/all.err" "$OCERZ" -aot-all
    all_code=$?
    OCERZ_TCACHE=on OCERZ_TCACHE_DIR="$aoti_dir/store" OCERZ_TCACHE_LOG="$aoti_dir/run.log" OCERZ_TCACHE_MIN_FREE_MB=0 \
        run_bounded "$aoti_dir/run.out" "$aoti_dir/run.err" "$OCERZ" "$aoti_dir/main" "$aoti_dir/plugin.bundle"
    run_code=$?
    if [ "$first_code" = 0 ] && [ "$all_code" = 0 ] && [ "$run_code" = 0 ] && [ "$(cat "$aoti_dir/run.out")" = OK ] &&
       grep -q '1 program done' "$aoti_dir/all.err" && grep -q '(1 from the profile)' "$aoti_dir/all.err" &&
       awk '/images asked=/ { for (f = 3; f <= NF; f++) { split($f, a, "="); t[a[1]] += a[2] } }
            END { exit !(t["asked"] > 0 && t["in"] - t["ahead"] == t["asked"]) }' "$aoti_dir/run.log"; then
        echo "PASS dtcache_aot_images"; pass=$((pass+1))
    else
        echo "FAIL dtcache_aot_images (first exit=$first_code -aot-all exit=$all_code run exit=$run_code" \
             "out='$(cat "$aoti_dir/run.out")' $(grep -h 'aot' "$aoti_dir/all.err" | tr '\n' ' ')" \
             "$(grep -h 'images asked=' "$aoti_dir/run.log" | head -1))"
        fail=$((fail+1))
    fi
fi
# With a free-space floor no disk can meet, the cache must not write a byte and
# the program must still run; the floor is what kept the cache from filling a
# nearly full disk.
tcache_floor_dir="$TMP/dtcache_floor"
mkdir -p "$tcache_floor_dir"
if ! clang -arch x86_64 -O2 -o "$tcache_floor_dir/work" tests/dynamic/tcache_work.c 2>/dev/null; then
    echo "FAIL dtcache_floor (build)"; fail=$((fail+1))
else
    OCERZ_TCACHE=on OCERZ_TCACHE_DIR="$tcache_floor_dir/store" OCERZ_TCACHE_MIN_FREE_MB=1000000000 \
        run_bounded "$tcache_floor_dir/out" "$tcache_floor_dir/err" "$OCERZ" "$tcache_floor_dir/work"
    tcache_floor_code=$?
    tcache_floor_data=$(find "$tcache_floor_dir/store" -name 'd-*.td' 2>/dev/null | wc -l | tr -d ' ')
    if [ "$(cat "$tcache_floor_dir/out")" = OK ] && [ "$tcache_floor_code" = 0 ] && [ "$tcache_floor_data" = 0 ]; then
        echo "PASS dtcache_floor"; pass=$((pass+1))
    else
        echo "FAIL dtcache_floor (out='$(cat "$tcache_floor_dir/out")' exit=$tcache_floor_code data files=$tcache_floor_data)"
        fail=$((fail+1))
    fi
fi
# The weak-def name filter kept beside the translation store (src/cache.c),
# three runs against one fresh store: the first must build and keep it, the
# second must read it, and after its bytes are corrupted the third must reject
# it and keep a good one again.  Every run must bind the library's 200
# weak definitions right.
run_weak_bloom_case() {
    local name="$1" dir="$TMP/$1"
    mkdir -p "$dir"
    if ! clang++ -arch x86_64 -O1 -dynamiclib -install_name @rpath/libweak_bloom.dylib \
            -o "$dir/libweak_bloom.dylib" tests/dynamic/weak_bloom_lib.cpp 2>/dev/null ||
       ! clang -arch x86_64 -O1 -o "$dir/$name" tests/dynamic/weak_bloom.c -L"$dir" -lweak_bloom \
            -Wl,-rpath,@executable_path 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local i want why kept
    for i in 1 2 3; do
        case $i in
            1) want='weak filter kept' ;;
            2) want='weak filter read' ;;
            3) want='weak filter rejected'
               kept=$(find "$dir/store" -name 'weakbloom-*.bin' | head -1)
               printf 'corrupt' | dd of="$kept" bs=1 seek=500000 conv=notrunc 2>/dev/null ;;
        esac
        OCERZ_TCACHE=on OCERZ_TCACHE_DIR="$dir/store" OCERZ_TCACHE_LOG=1 OCERZ_TCACHE_MIN_FREE_MB=0 \
            run_bounded "$dir/run$i.out" "$dir/run$i.err" "$OCERZ" "$dir/$name"
        why=""
        if [ "$(cat "$dir/run$i.out")" != OK ]; then
            why="got out='$(cat "$dir/run$i.out")'"
        elif ! grep -q "$want" "$dir/run$i.err"; then
            why="no '$want' in: $(grep -h 'weak filter' "$dir/run$i.err" | head -2 | tr '\n' ' ')"
        elif [ $i = 3 ] && ! grep -q 'weak filter kept' "$dir/run$i.err"; then
            why="the rejected filter was not kept again"
        fi
        if [ -z "$why" ]; then
            echo "PASS $name-$i"; pass=$((pass+1))
        else
            echo "FAIL $name-$i ($why)"; fail=$((fail+1))
        fi
    done
}

run_weak_bloom_case dweak_bloom
# remap_order.c in its three modes with OCERZ_ORDERLOG=1: a private mapping
# moved into the guest must leave the process on plain memory, a shared or a
# writable copy-on-write one must move it to ordered.
run_remap_order_case() {
    local name="$1" dir="$TMP/$1" mode want got
    mkdir -p "$dir"
    if ! clang -arch x86_64 -O1 -o "$dir/$name" tests/dynamic/remap_order.c 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    for mode in private shared cow; do
        want=ordered
        [ $mode = private ] && want=plain
        OCERZ_ORDERLOG=1 run_bounded "$dir/$mode.out" "$dir/$mode.err" "$OCERZ" "$dir/$name" $mode
        got=plain
        grep -q 'ORDERED memory required' "$dir/$mode.err" && got=ordered
        if [ "$(cat "$dir/$mode.out")" = OK ] && [ $got = $want ]; then
            echo "PASS $name-$mode ($got)"; pass=$((pass+1))
        else
            echo "FAIL $name-$mode (out='$(cat "$dir/$mode.out")', memory $got, want $want)"; fail=$((fail+1))
        fi
    done
}

run_remap_order_case dremap_order
run_relpath_case dexec_abspath tests/dynamic/exec_abspath.c 'OK'
run_file_case ddlopen_self tests/dynamic/dlopen_self.c 'OK'
run_alias_case ddlopen_alias 'OK'
run_flat_case dflat_lookup 'OK'
run_file_case dfork_keepjit tests/dynamic/fork_keepjit.c 'OK'
# Ruby forks at every start (rbconfig.rb runs `uname -m`); kept by its fork children, the JIT failed 14 runs in 20
# of this, the child faulting on its first translated instruction, because the forking thread came out of fork()
# with its MAP_JIT pages writable (src/vm.c).  Each run must print hi and no child may report a [BUG].
if [ -x /usr/bin/ruby ]; then
    rb_bad=""
    for rb_i in 1 2 3 4 5 6; do
        run_bounded "$TMP/druby_fork.$rb_i.out" "$TMP/druby_fork.$rb_i.err" env OCERZ_FORK_KEEPJIT=1 "$OCERZ" /usr/bin/ruby -e 'puts `echo hi`'
        if [ "$(cat "$TMP/druby_fork.$rb_i.out")" != hi ] || grep -q '\[BUG\]' "$TMP/druby_fork.$rb_i.err"; then
            rb_bad="run $rb_i: out='$(cat "$TMP/druby_fork.$rb_i.out")' $(grep -m1 '\[BUG\]' "$TMP/druby_fork.$rb_i.err" | cut -c1-120)"
            break
        fi
    done
    if [ -z "$rb_bad" ]; then
        echo "PASS druby_fork_keepjit (6 runs)"; pass=$((pass+1))
    else
        echo "FAIL druby_fork_keepjit ($rb_bad)"; fail=$((fail+1))
    fi
fi
run_dlopen_cf_case ddlopen_cf 'OK'
run_load_phase_case ddlopen_load_phase 'OK'
run_file_case ddlopen_objc_core tests/dynamic/dlopen_objc_core.c 'OK'
run_caller_rpath_case ddlopen_caller_rpath 'OK'
run_chain_rpath_case ddlopen_chain_rpath 'OK'
run_rpath_known_case drpath_known 'OK'
run_wine_segment_case dwine_segment
run_mac_syscall_low_stack_case dmac_syscall_low_stack 'OK'
run_weak_unloaded_case dweak_unloaded 'OK'
run_metal_nocopy_low_case dmetal_nocopy_low 'OK'
run_iosurface_low_stack_case diosurface_low_stack 'OK'
run_rpath_system_case drpath_system 'the system libswiftCore was loaded'
run_low_golden_case dsimd_low tests/guest/simd_pack_jit.c tests/guest/expect/simd_pack_jit.out
run_low_golden_case dmmx_low tests/guest/mmx_jit.c tests/guest/expect/mmx_jit.out
export OCERZ_TSO_NARROW=1
run_low_golden_case dtso_narrow_low tests/dynamic/tso_narrow.c tests/dynamic/tso_narrow.out
unset OCERZ_TSO_NARROW
run_low_golden_case dpromo_callout_low tests/dynamic/promo_callout.c tests/dynamic/promo_callout.out
run_low_golden_case dras_stress_low tests/guest/ras_stress.c tests/guest/expect/ras_stress.out
run_low_golden_case dcallret_fault_low tests/guest/callret_fault.c tests/guest/expect/callret_fault.out
run_low_golden_case dfault_link_low tests/guest/fault_link.c tests/guest/expect/fault_link.out
run_low_golden_case dstack_test_low tests/guest/stack_test.c tests/guest/expect/stack_test.out
run_low_golden_case djcc_chain_rec_low tests/guest/jcc_chain_rec.c tests/guest/expect/jcc_chain_rec.out
run_low_golden_case drsp_ops_low tests/guest/rsp_ops.c tests/guest/expect/rsp_ops.out
run_low_golden_case drsp_rmw_low tests/dynamic/rsp_rmw.c tests/dynamic/rsp_rmw.out
run_low_golden_case dcvt_packed_low tests/dynamic/cvt_packed.c tests/dynamic/cvt_packed.out
run_low_golden_case dx87_arith_low tests/dynamic/x87_arith.c tests/dynamic/x87_arith.out
run_low_golden_case dx87_compare_low tests/dynamic/x87_compare.c tests/dynamic/x87_compare.out
run_low_golden_case dx87_int_low tests/dynamic/x87_int.c tests/dynamic/x87_int.out
run_low_golden_case dx87_trans_low tests/dynamic/x87_trans.c tests/dynamic/x87_trans.out
run_low_golden_case dx87_state_low tests/dynamic/x87_state.c tests/dynamic/x87_state.out
run_low_golden_case dlow_stack_switch tests/dynamic/low_stack_switch.c tests/dynamic/low_stack_switch.out
run_low_golden_case dlow_hoist tests/dynamic/low_hoist.c tests/dynamic/low_hoist.out
run_golden_case dhoist_share tests/dynamic/hoist_share.c tests/dynamic/hoist_share.out
run_low_golden_case dhoist_share_low tests/dynamic/hoist_share.c tests/dynamic/hoist_share.out
# Loops that end in inc or dec and jz/jnz to their own head: straight back to the loop entry when the head reads no
# flags, and a double carried round such a loop in a fixed lane comes out right on that path and on the chained one.
run_golden_case dincdec_loop tests/dynamic/incdec_loop.c tests/dynamic/incdec_loop.out
run_low_golden_case dincdec_loop_low tests/dynamic/incdec_loop.c tests/dynamic/incdec_loop.out
export OCERZ_NO_INCDEC_LOOP=1
run_golden_case dincdec_loop_chained tests/dynamic/incdec_loop.c tests/dynamic/incdec_loop.out
unset OCERZ_NO_INCDEC_LOOP
run_low_golden_case drep_string_low tests/dynamic/rep_string.c tests/dynamic/rep_string.out
run_low_golden_case dcomis_mem_low tests/dynamic/comis_mem.c tests/dynamic/comis_mem.out
run_low_golden_case dstack_pair_low tests/dynamic/stack_pair.c tests/dynamic/stack_pair.out
run_low_golden_case dstack_run_low tests/dynamic/stack_run.c tests/dynamic/stack_run.out
run_low_golden_case drot_flags_low tests/dynamic/rot_flags.c tests/dynamic/rot_flags.out
run_low_golden_case dincdec_flags_low tests/dynamic/incdec_flags.c tests/dynamic/incdec_flags.out
run_low_golden_case dinline_misc_low tests/dynamic/inline_misc.c tests/dynamic/inline_misc.out
run_low_golden_case dblock_cap_low tests/dynamic/block_cap.c tests/dynamic/block_cap.out
run_low_golden_case dptest_flags_low tests/dynamic/ptest_flags.c tests/dynamic/ptest_flags.out
run_low_golden_case dshift_cl_flags_low tests/dynamic/shift_cl_flags.c tests/dynamic/shift_cl_flags.out
run_low_golden_case dret_flags_seam_low tests/dynamic/ret_flags_seam.c tests/dynamic/ret_flags_seam.out
run_low_golden_case dshufps_self_low tests/dynamic/shufps_self.c tests/dynamic/shufps_self.out
run_low_golden_case dcmp_mem_setcc_low tests/dynamic/cmp_mem_setcc.c tests/dynamic/cmp_mem_setcc.out
run_low_golden_case dleaf_low_low tests/dynamic/leaf_low.c tests/dynamic/leaf_low.out
run_low_golden_case dslot_jump_low tests/dynamic/slot_jump.c tests/dynamic/slot_jump.out
run_low_golden_case dwine_guard_low tests/dynamic/wine_guard.c tests/dynamic/wine_guard.out
run_low_golden_case dlowstack_disp_low tests/dynamic/lowstack_disp.c tests/dynamic/lowstack_disp.out
run_low_golden_case dlowstack_ptr_low tests/dynamic/lowstack_ptr.c tests/dynamic/lowstack_ptr.out
run_low_golden_case dwine_base_low tests/dynamic/wine_base.c tests/dynamic/wine_base.out
export OCERZ_NO_LOW_HOIST=1
run_low_golden_case dwine_base_low_nohoist tests/dynamic/wine_base.c tests/dynamic/wine_base.out
unset OCERZ_NO_LOW_HOIST
run_low_golden_case dlazy_rsp_low tests/dynamic/lazy_rsp.c tests/dynamic/lazy_rsp.out
run_low_golden_case dnzcv_window_low tests/dynamic/nzcv_window.c tests/dynamic/nzcv_window.out
run_low_golden_case dnzcv_regen_fault_low tests/dynamic/nzcv_regen_fault.c tests/dynamic/nzcv_regen_fault.out
run_low_golden_case dxlive_self_low tests/dynamic/xlive_self.c tests/dynamic/xlive_self.out
export OCERZ_NO_HIGH8_ARITH=1
run_low_golden_case dlazy_rsp_low_slow tests/dynamic/lazy_rsp.c tests/dynamic/lazy_rsp.out
unset OCERZ_NO_HIGH8_ARITH
run_low_golden_case dtest_jcc_gap_low tests/dynamic/test_jcc_gap.c tests/dynamic/test_jcc_gap.out
run_low_golden_case dsetcc_zx_low tests/dynamic/setcc_zx.c tests/dynamic/setcc_zx.out
run_low_golden_case dhoist_forms_low tests/dynamic/hoist_forms.c tests/dynamic/hoist_forms.out
run_low_golden_case dlow_loop_hoist_low tests/dynamic/low_loop_hoist.c tests/dynamic/low_loop_hoist.out
# The hoist's decisions themselves: the three-array loop keeps three bases, loops that meet a base
# above 12 GB or a negative index move to the wide form, and (under the old guard, below) the loops
# built to defeat that too (an index striding 3 GB, a base just below 12 GB, an index hopping across
# 12 GB) give the hoist up.
if run_bounded "$TMP/dlow_loop_hoist.log.out" "$TMP/dlow_loop_hoist.log.err" env OCERZ_LOW_HOIST_LOG=1 "$OCERZ" "$TMP/dlow_loop_hoist_low/dlow_loop_hoist_low" &&
   grep -q 'LOWHOIST take 0x2[0-9a-f]* bases=3' "$TMP/dlow_loop_hoist.log.err" &&
   [ "$(grep -c 'LOWHOIST widen 0x2[0-9a-f]*$' "$TMP/dlow_loop_hoist.log.err")" -ge 3 ]; then
    echo "PASS dlow_loop_hoist_decisions (three bases kept; the wide form taken where it must be)"; pass=$((pass+1))
else
    echo "FAIL dlow_loop_hoist_decisions ($(grep -c 'LOWHOIST take 0x2' "$TMP/dlow_loop_hoist.log.err") taken, $(grep -c 'LOWHOIST widen 0x2' "$TMP/dlow_loop_hoist.log.err") widened)"
    fail=$((fail+1))
fi
# With 12 GB in x29 the wide form leaves indexed operands to the guard, so nothing in this program makes it give
# up; with the old guard (OCERZ_NO_WINE_LIM=1) it still covers them, and the loops above defeat it.
if run_bounded "$TMP/dlow_loop_hoist.log2.out" "$TMP/dlow_loop_hoist.log2.err" env OCERZ_LOW_HOIST_LOG=1 OCERZ_NO_WINE_LIM=1 "$OCERZ" "$TMP/dlow_loop_hoist_low/dlow_loop_hoist_low" &&
   [ "$(grep -c 'LOWHOIST bail 0x2[0-9a-f]*$' "$TMP/dlow_loop_hoist.log2.err")" -ge 3 ]; then
    echo "PASS dlow_loop_hoist_bail (the wide form given up where it must be, under the old guard)"; pass=$((pass+1))
else
    echo "FAIL dlow_loop_hoist_bail ($(grep -c 'LOWHOIST bail 0x2' "$TMP/dlow_loop_hoist.log2.err") given up)"
    fail=$((fail+1))
fi
run_low_golden_case dfist_rc_low tests/dynamic/fist_rc.c tests/dynamic/fist_rc.out
run_low_golden_case dflip_phase_low tests/dynamic/flip_phase.c tests/dynamic/flip_phase.out
run_low_golden_case dlow_top_strip tests/dynamic/low_top_strip.c tests/dynamic/low_top_strip.out
export OCERZ_NO_PLAIN_MEM=1
run_file_case dalign_ordered tests/dynamic/align_ordered.c '370d1a721afe9c4a'
run_low_golden_case dalign_ordered_low tests/dynamic/align_ordered.c tests/dynamic/align_ordered.out
run_file_case drsp_rmw_ordered tests/dynamic/rsp_rmw.c '554ae911bb61770a' -mno-red-zone
run_low_golden_case drsp_rmw_low_ordered tests/dynamic/rsp_rmw.c tests/dynamic/rsp_rmw.out
run_low_golden_case dsmc_ras_low_ordered tests/dynamic/smc_ras.c tests/dynamic/smc_ras.out
unset OCERZ_NO_PLAIN_MEM
run_low_golden_case dfault_chain_low tests/guest/fault_chain.c tests/guest/expect/fault_chain.out
run_dlsym_deps_case ddlsym_deps 'OK'
run_dlopen_arch_case ddlopen_arch 'OK'
run_rpath_bare_case drpath_bare 'OK'
run_init_order_case dinit_order 'OK'
run_upward_defer_case dupward_defer 'OK'
run_file_case ddlsym_cache_image tests/dynamic/dlsym_cache_image.c 'OK'
run_file_case dsyscalls_extra tests/dynamic/syscalls_extra.c 'OK'
run_file_case dproc_self tests/dynamic/proc_self.c 'OK'
run_file_case dmach_traps_extra tests/dynamic/mach_traps_extra.c 'OK'
run_file_case dthread_act tests/dynamic/thread_act.c 'OK'
run_file_case dwq_thread_exit tests/dynamic/wq_thread_exit.c 'OK'
run_file_case dwq_overcommit tests/dynamic/wq_overcommit.c 'OK' -fblocks
run_file_case dsleep_after_dispatch tests/dynamic/sleep_after_dispatch.c 'OK'
run_file_case jsc tests/dynamic/jsc_context.m 'OK' -fobjc-arc -framework JavaScriptCore -framework Foundation
run_file_case dobjc_late_category tests/dynamic/objc_late_category.c 'OK' -framework Foundation
run_file_case ddelay_init tests/dynamic/delay_init.m 'OK' -fobjc-arc -framework Foundation
run_asm_case dcef_partition tests/dynamic/cef_partition.c tests/dynamic/cef_partition.s 'OK'
run_asm_case dmmx_ops tests/dynamic/mmx_ops.c tests/dynamic/mmx_ops.s 'OK'
run_spawn_argv_case dspawn_mock_keychain
run_legacy_entry_case dlegacy_entry 'OK'
run_idname_case didname 'OK'
run_file_case dsemaphore_wait tests/dynamic/semaphore_wait.c 'OK'
run_file_case dsignal_redzone tests/dynamic/signal_redzone.c 'OK'
run_file_case dsigaltstack_query tests/dynamic/sigaltstack_query.c 'OK'
run_file_case dloop_signal tests/dynamic/loop_signal.c 'OK'
run_file_case davx2_ops tests/dynamic/avx2_ops.c 'OK'
run_file_case davx_fma tests/dynamic/avx_fma.c 'OK'
run_file_case davx_fp tests/dynamic/avx_fp.c 'OK'
run_file_case dbmi_ops tests/dynamic/bmi_ops.c 'OK'
run_file_case dymm_state tests/dynamic/ymm_state.c 'OK'
run_file_case ddeveloper_tools tests/dynamic/developer_tools.c 'OK'

echo "----------------------------------------"
echo "dynamic tests: $pass passed, $fail failed"
echo "KNOWN-PENDING: libc-call programs (printf) reach cache code but need the libSystem-initializer phase"
[ "$fail" -eq 0 ]

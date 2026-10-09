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

run_file_case dfork_signal tests/dynamic/fork_signal.c 'fork signal ok'
run_file_case dthread_signal tests/dynamic/thread_signal.c 'OK'
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
run_golden_case dshufps_self tests/dynamic/shufps_self.c tests/dynamic/shufps_self.out
run_golden_case dcmp_mem_setcc tests/dynamic/cmp_mem_setcc.c tests/dynamic/cmp_mem_setcc.out
run_golden_case dlowstack_disp tests/dynamic/lowstack_disp.c tests/dynamic/lowstack_disp.out
run_golden_case dtest_jcc_gap tests/dynamic/test_jcc_gap.c tests/dynamic/test_jcc_gap.out
run_golden_case dsetcc_zx tests/dynamic/setcc_zx.c tests/dynamic/setcc_zx.out
run_golden_case dhoist_forms tests/dynamic/hoist_forms.c tests/dynamic/hoist_forms.out
run_golden_case dfist_rc tests/dynamic/fist_rc.c tests/dynamic/fist_rc.out
run_golden_case dflip_phase tests/dynamic/flip_phase.c tests/dynamic/flip_phase.out
run_file_case dunaligned_atomics tests/dynamic/unaligned_atomics.c 'OK'
run_file_case dlane_fault_guard tests/dynamic/lane_fault_guard.c 'OK' -Wl,-no_pie
run_file_case dsmc_io tests/dynamic/smc_io.c 'OK'
run_file_case dsmc_high tests/dynamic/smc_high.c 'OK'
run_file_case dsmc_ras tests/dynamic/smc_ras.c 'OK'
run_low_golden_case dsmc_ras_low tests/dynamic/smc_ras.c tests/dynamic/smc_ras.out
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
run_file_case datomic_counter tests/dynamic/atomic_counter.c 'OK'
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
# tcache_work.c against one fresh translation cache directory, four runs (with
# no free-space floor, so a full disk on the test machine is not a failure): one
# that records, one that must load what the first stored, one under
# OCERZ_TCACHE=verify that must find no block differing from its record except
# in shape, and one under OCERZ_TCACHE=roundtrip that must find no reference its
# relocations miss.
run_tcache_case() {
    local name="$1" want_out="$2"
    shift 2
    local dir="$TMP/$name"
    mkdir -p "$dir"
    if ! clang -arch x86_64 -O2 -o "$dir/$name" tests/dynamic/tcache_work.c "$@" 2>/dev/null; then
        echo "FAIL $name (build)"; fail=$((fail+1)); return
    fi
    local i=0 mode log got_code got_out why
    for mode in on on verify roundtrip; do
        i=$((i + 1))
        log="$dir/run$i.log"
        OCERZ_TCACHE=$mode OCERZ_TCACHE_DIR="$dir/store" OCERZ_TCACHE_LOG="$log" OCERZ_TCACHE_MIN_FREE_MB=0 \
            run_bounded "$dir/run$i.out" "$dir/run$i.err" "$OCERZ" "$dir/$name"
        got_code=$?
        got_out=$(cat "$dir/run$i.out")
        why=""
        if [ "$got_out" != "$want_out" ] || [ "$got_code" != 0 ]; then
            why="got out='$got_out' exit=$got_code"
        elif [ $i = 2 ] && ! awk '/TCACHE.*loaded=/ { for (f = 3; f <= NF; f++) { split($f, a, "="); t[a[1]] += a[2] } }
                                  END { exit !(t["loaded"] > 0 && t["rejected"] == 0) }' "$log"; then
            why="nothing loaded: $(grep -h 'loaded=' "$log" | head -1)"
        elif [ $i = 3 ] && ! awk '/TCACHE.*loaded=/ { for (f = 3; f <= NF; f++) { split($f, a, "="); t[a[1]] += a[2] } }
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
run_tcache_case dtcache_low 'OK' -Wl,-no_pie -Wl,-pagezero_size,0x1000 -Wl,-image_base,0x200000000
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
run_relpath_case dexec_abspath tests/dynamic/exec_abspath.c 'OK'
run_file_case ddlopen_self tests/dynamic/dlopen_self.c 'OK'
run_alias_case ddlopen_alias 'OK'
run_dlopen_cf_case ddlopen_cf 'OK'
run_load_phase_case ddlopen_load_phase 'OK'
run_file_case ddlopen_objc_core tests/dynamic/dlopen_objc_core.c 'OK'
run_caller_rpath_case ddlopen_caller_rpath 'OK'
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
run_low_golden_case drep_string_low tests/dynamic/rep_string.c tests/dynamic/rep_string.out
run_low_golden_case dcomis_mem_low tests/dynamic/comis_mem.c tests/dynamic/comis_mem.out
run_low_golden_case dstack_pair_low tests/dynamic/stack_pair.c tests/dynamic/stack_pair.out
run_low_golden_case dstack_run_low tests/dynamic/stack_run.c tests/dynamic/stack_run.out
run_low_golden_case drot_flags_low tests/dynamic/rot_flags.c tests/dynamic/rot_flags.out
run_low_golden_case dincdec_flags_low tests/dynamic/incdec_flags.c tests/dynamic/incdec_flags.out
run_low_golden_case dinline_misc_low tests/dynamic/inline_misc.c tests/dynamic/inline_misc.out
run_low_golden_case dblock_cap_low tests/dynamic/block_cap.c tests/dynamic/block_cap.out
run_low_golden_case dptest_flags_low tests/dynamic/ptest_flags.c tests/dynamic/ptest_flags.out
run_low_golden_case dshufps_self_low tests/dynamic/shufps_self.c tests/dynamic/shufps_self.out
run_low_golden_case dcmp_mem_setcc_low tests/dynamic/cmp_mem_setcc.c tests/dynamic/cmp_mem_setcc.out
run_low_golden_case dlowstack_disp_low tests/dynamic/lowstack_disp.c tests/dynamic/lowstack_disp.out
run_low_golden_case dtest_jcc_gap_low tests/dynamic/test_jcc_gap.c tests/dynamic/test_jcc_gap.out
run_low_golden_case dsetcc_zx_low tests/dynamic/setcc_zx.c tests/dynamic/setcc_zx.out
run_low_golden_case dhoist_forms_low tests/dynamic/hoist_forms.c tests/dynamic/hoist_forms.out
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

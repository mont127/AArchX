#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
repo=$PWD
root=${OCERZ_GUEST_ROOT-"$repo/runtime/guest"}
if [ ! -f "$root/usr/lib/libc++.1.dylib" ]; then
    echo 'SKIP native C++ tests: run bash tools/build_guest_cxx.sh first'
    exit 0
fi
work=$(mktemp -d /tmp/ocerz-native-cxx.XXXXXX)
for arch in x86_64 arm64; do
    clang++ -arch "$arch" -std=c++17 -O1 tests/dynamic/native_cxx.cpp -o "$work/extended.$arch"
    clang++ -arch "$arch" -std=c++17 -O1 -dynamiclib tests/dynamic/native_cxx_dylib.cpp -o "$work/plugin.$arch.dylib"
    clang++ -arch "$arch" -std=c++17 -O1 tests/dynamic/cpp_exceptions.cpp -o "$work/exceptions.$arch"
    clang++ -arch "$arch" -std=c++17 -O1 tests/dynamic/cpp_global_ctor.cpp -o "$work/constructors.$arch"
done
"$work/extended.arm64" "$work/plugin.arm64.dylib" > "$work/extended.expected"
"$work/exceptions.arm64" > "$work/exceptions.expected"
"$work/constructors.arm64" > "$work/constructors.expected"
for engine in jit interpreter; do
    args=(-native -v)
    if [ "$engine" = interpreter ]; then args+=(-no-jit); fi
    for test in extended exceptions constructors; do
        extra=()
        if [ "$test" = extended ]; then extra=("$work/plugin.x86_64.dylib"); fi
        env OCERZ_GUEST_ROOT="$root" OCERZ_BRIDGESTAT=1 OCERZ_JITSTAT=1 \
            /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" "${args[@]}" \
            "$work/$test.x86_64" ${extra[@]+"${extra[@]}"} \
            > "$work/$test.$engine.out" 2> "$work/$test.$engine.err"
        cmp "$work/$test.expected" "$work/$test.$engine.out"
        grep -q 'native mode, shared cache not mapped' "$work/$test.$engine.err"
        grep -Eq 'BRIDGESTAT.*crossings=[1-9][0-9]*' "$work/$test.$engine.err"
        if grep -q 'shared cache mapped at' "$work/$test.$engine.err"; then
            echo 'FAIL: native C++ test mapped the x86 cache' >&2
            exit 1
        fi
        echo "PASS native C++ $test $engine"
    done
    rc=0
    env OCERZ_GUEST_ROOT="$root" "$repo/ocerz" "${args[@]}" "$work/extended.x86_64" unsupported-format \
        > "$work/refusal.$engine.out" 2> "$work/refusal.$engine.err" || rc=$?
    [ "$rc" = 72 ]
    grep -q '_snprintf_l refuses the format' "$work/refusal.$engine.err"
    echo "PASS locale format refusal $engine"
done
grep -Eq 'translate: calls=[1-9][0-9]* ok=[1-9][0-9]*' "$work/extended.jit.err"
if grep -q 'JITSTAT' "$work/extended.interpreter.err"; then
    echo 'FAIL: interpreter test used JIT statistics' >&2
    exit 1
fi
if [ "${OCERZ_TEST_CACHE:-0}" = 1 ]; then
    mkdir -p "$work/cache-root/usr/lib"
    ln -s "$work/absent.dylib" "$work/cache-root/usr/lib/libc++.1.dylib"
    env OCERZ_GUEST_ROOT="$work/cache-root" /usr/bin/perl -e 'alarm 60; exec @ARGV' \
        "$repo/ocerz" -cache -v "$work/exceptions.x86_64" > "$work/cache.out" 2> "$work/cache.err"
    cmp "$work/exceptions.expected" "$work/cache.out"
    grep -q 'shared cache mapped at' "$work/cache.err"
    if grep -q 'guest override' "$work/cache.err"; then exit 1; fi
    echo 'PASS existing cache-mode C++ exceptions'
fi
echo "native C++ tests passed; logs: $work"

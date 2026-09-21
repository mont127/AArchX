#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
repo=$PWD
work=$(mktemp -d /tmp/ocerz-guest-library.XXXXXX)
root="$work/root with spaces"
install=/usr/lib/libncurses.5.4.dylib
mkdir -p "$root/usr/lib" "$work/arm/usr/lib" "$work/bad/usr/lib"
clang -arch x86_64 -O1 -fno-builtin -dynamiclib tests/dynamic/native_guest_library.c \
    -Wl,-install_name,"$install" -o "$root$install"
clang -arch x86_64 -O1 tests/dynamic/native_guest_library_main.c "$root$install" -o "$work/direct"
clang -arch x86_64 -O1 -DDYNAMIC_ONLY tests/dynamic/native_guest_library_main.c -o "$work/dynamic"
clang -arch arm64 -O1 -dynamiclib tests/dynamic/native_guest_library.c \
    -Wl,-install_name,"$install" -o "$work/arm$install"
ln -s "$work/missing.dylib" "$work/bad$install"
for engine in jit interpreter; do
    args=(-native)
    if [ "$engine" = interpreter ]; then args+=(-no-jit); fi
    for mode in direct dynamic; do
        env OCERZ_GUEST_ROOT="$root" "$repo/ocerz" -v "${args[@]}" "$work/$mode" \
            122 "$install" "$root" > "$work/$engine.$mode.out" 2> "$work/$engine.$mode.err"
        grep -q '^native_guest_library ok 122$' "$work/$engine.$mode.out"
        env OCERZ_GUEST_ROOT="$work/absent" "$repo/ocerz" "${args[@]}" "$work/$mode" \
            1792 "$install" "$install" > "$work/$engine.$mode.fallback.out" 2> "$work/$engine.$mode.fallback.err"
        grep -q '^native_guest_library ok 1792$' "$work/$engine.$mode.fallback.out"
        for invalid in arm bad; do
            rc=0
            env OCERZ_GUEST_ROOT="$work/$invalid" "$repo/ocerz" "${args[@]}" "$work/$mode" \
                1792 "$install" "$install" > "$work/$engine.$mode.$invalid.out" \
                2> "$work/$engine.$mode.$invalid.err" || rc=$?
            if [ "$rc" -eq 0 ]; then
                echo "FAIL: $engine $mode silently used native code for $invalid override" >&2
                exit 1
            fi
            if [ "$invalid" = arm ]; then
                grep -q 'no x86_64 slice' "$work/$engine.$mode.$invalid.err"
            else
                grep -Eq 'could not be read|no bridge for' "$work/$engine.$mode.$invalid.err"
            fi
        done
    done
    env OCERZ_GUEST_ROOT="$root" "$repo/ocerz" "${args[@]}" "$work/dynamic" \
        122 "/System/Volumes/Preboot/Cryptexes/OS$install" "$root" \
        > "$work/$engine.cryptex.out" 2> "$work/$engine.cryptex.err"
    grep -q '^native_guest_library ok 122$' "$work/$engine.cryptex.out"
done
echo "guest library tests passed; logs: $work"

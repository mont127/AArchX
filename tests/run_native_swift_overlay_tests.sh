#!/usr/bin/env bash
# Dispatch's Swift overlay as guest code: import Dispatch links
# /usr/lib/swift/libswiftDispatch.dylib, which native mode answers with the
# x86_64 dylib tools/install_guest_swift_overlays.sh copies into runtime/guest.
# The same source is compiled for x86_64 and arm64, and the x86_64 build under
# -native must match the arm64 build's output under both engines.
set -euo pipefail
cd "$(dirname "$0")/.."
repo=$PWD
root=${OCERZ_GUEST_ROOT-"$repo/runtime/guest"}
need="usr/lib/swift/libswiftCore.dylib usr/lib/libc++.1.dylib usr/lib/libc++abi.dylib usr/lib/libunwind.1.dylib usr/lib/swift/libswiftDispatch.dylib usr/lib/swift/libswiftDarwin.dylib usr/lib/swift/libswiftObjectiveC.dylib"
for f in $need; do
    if [ ! -f "$root/$f" ]; then
        echo "SKIP native Swift overlay tests: missing $root/$f (make guest-cxx guest-swift guest-swift-overlays)"
        exit 0
    fi
done
if ! command -v swiftc > /dev/null; then
    echo 'SKIP native Swift overlay tests: no swiftc'
    exit 0
fi
test=native_dispatch_overlay
work=$(mktemp -d /tmp/ocerz-native-swift-overlay.XXXXXX)
for arch in x86_64 arm64; do
    swiftc -module-name "$test" -target "$arch-apple-macos13" -O "tests/dynamic/$test.swift" \
        -o "$work/$test.$arch" 2> "$work/$test.$arch.build"
done
"$work/$test.arm64" > "$work/$test.expected"
for engine in jit interpreter; do
    args=(-native -v)
    if [ "$engine" = interpreter ]; then args+=(-no-jit); fi
    env OCERZ_GUEST_ROOT="$root" OCERZ_BRIDGESTAT=1 \
        /usr/bin/perl -e 'alarm 120; exec @ARGV' "$repo/ocerz" "${args[@]}" "$work/$test.x86_64" \
        > "$work/$test.$engine.out" 2> "$work/$test.$engine.err"
    if ! cmp -s "$work/$test.expected" "$work/$test.$engine.out"; then
        echo "FAIL: native Swift overlay $test $engine differs from arm64" >&2
        diff "$work/$test.expected" "$work/$test.$engine.out" >&2 || true
        exit 1
    fi
    grep -q 'native mode, shared cache not mapped' "$work/$test.$engine.err"
    grep -q 'guest override /usr/lib/swift/libswiftCore.dylib' "$work/$test.$engine.err"
    grep -q 'guest override /usr/lib/swift/libswiftDispatch.dylib' "$work/$test.$engine.err"
    grep -Eq 'BRIDGESTAT.*crossings=[1-9][0-9]*' "$work/$test.$engine.err"
    echo "PASS native Swift overlay $test $engine"
done
echo "native Swift overlay tests passed; logs: $work"

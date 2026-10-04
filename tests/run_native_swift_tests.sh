#!/usr/bin/env bash
# Swift programs in native mode.  Each fixture is compiled twice from the same
# source and module name, for x86_64 and for arm64, and the x86_64 build run
# under -native, with the JIT and with the interpreter, must print exactly what
# the arm64 build prints when it runs as itself.  The module name matters:
# String(reflecting:) and the default description of an enum or a struct inside
# an Optional spell it out.
#
# native_swift covers the language: classes, generics, protocols with
# associated types and existentials, enums, errors, closures, Mirror, key paths,
# Unicode strings, a run-time Regex, and a weak reference that must read nil
# once its object is gone.  native_swift_release covers the end of an object's
# life, which is where native mode has two paths of its own: arrays of class
# instances freed through AnyObject, which the guest's objc_release hands to
# the guest's swift_release, and an autorelease pool drained by the native
# libobjc, whose last release of each object reaches the native Swift runtime
# and from there the guest class's destroy function.  OCERZ_OBJCLOG must show
# that crossing for the three pooled objects, so the second path is known to
# have run rather than to have been avoided.
#
# native_swift_concurrency is an asynchronous main with an actor, task groups,
# async let, a detached task, the main actor, cancellation and an AsyncStream.
# Its tasks sleep through deadlines the runtime builds itself in nanoseconds,
# which only fire at the right time because native mode converts every
# dispatch_time_t between the guest's nanoseconds and the host's ticks, and its
# main thread ends in dispatch_main, which native mode answers by running the
# main run loop.  native_dispatch_time checks that conversion from C, one timed
# call of each libdispatch function that takes or returns a dispatch_time_t, so
# a failure there points at the boundary rather than at the Swift runtime.  Its
# bounds are loose enough for a loaded machine and still far tighter than the
# factor of 41 an unconverted value is off by.
#
# The guest runtime comes from tools/install_guest_swift.sh (make guest-swift),
# and the suite skips without it or without a swiftc that targets both slices.
set -euo pipefail
cd "$(dirname "$0")/.."
repo=$PWD
root=${OCERZ_GUEST_ROOT-"$repo/runtime/guest"}
if [ ! -f "$root/usr/lib/swift/libswiftCore.dylib" ]; then
    echo 'SKIP native Swift tests: run make guest-swift first'
    exit 0
fi
if ! command -v swiftc > /dev/null; then
    echo 'SKIP native Swift tests: no swiftc'
    exit 0
fi
work=$(mktemp -d /tmp/ocerz-native-swift.XXXXXX)
tests="native_swift native_swift_release native_swift_concurrency native_dispatch_time"
for test in $tests; do
    for arch in x86_64 arm64; do
        case $test in
        native_dispatch_time)
            clang -arch "$arch" -O1 "tests/dynamic/$test.c" -o "$work/$test.$arch" ;;
        native_swift_concurrency)
            swiftc -module-name "$test" -parse-as-library -target "$arch-apple-macos13" -O \
                "tests/dynamic/$test.swift" -o "$work/$test.$arch" 2> "$work/$test.$arch.build" ;;
        *)
            swiftc -module-name "$test" -target "$arch-apple-macos13" -O "tests/dynamic/$test.swift" \
                -o "$work/$test.$arch" 2> "$work/$test.$arch.build" ;;
        esac
    done
    "$work/$test.arm64" > "$work/$test.expected"
done
for engine in jit interpreter; do
    args=(-native -v)
    if [ "$engine" = interpreter ]; then args+=(-no-jit); fi
    for test in $tests; do
        env OCERZ_GUEST_ROOT="$root" OCERZ_BRIDGESTAT=1 OCERZ_OBJCLOG=1 \
            /usr/bin/perl -e 'alarm 120; exec @ARGV' "$repo/ocerz" "${args[@]}" "$work/$test.x86_64" \
            > "$work/$test.$engine.out" 2> "$work/$test.$engine.err"
        if ! cmp -s "$work/$test.expected" "$work/$test.$engine.out"; then
            echo "FAIL: native Swift $test $engine differs from arm64" >&2
            diff "$work/$test.expected" "$work/$test.$engine.out" >&2 || true
            exit 1
        fi
        grep -q 'native mode, shared cache not mapped' "$work/$test.$engine.err"
        if [ "$test" != native_dispatch_time ]; then
            grep -q 'guest override /usr/lib/swift/libswiftCore.dylib' "$work/$test.$engine.err"
        fi
        grep -Eq 'BRIDGESTAT.*crossings=[1-9][0-9]*' "$work/$test.$engine.err"
        echo "PASS native Swift $test $engine"
    done
    n=$(grep -ac 'native code destroys guest Swift object' "$work/native_swift_release.$engine.err" || true)
    if [ "$n" -lt 3 ]; then
        echo "FAIL: native Swift release $engine: $n native destroys, want at least 3" >&2
        exit 1
    fi
    echo "PASS native Swift destroy crossing $engine ($n objects)"
done
echo "native Swift tests passed; logs: $work"

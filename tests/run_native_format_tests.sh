#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
repo=$PWD
work=$(mktemp -d /tmp/ocerz-native-formats.XXXXXX)
echo "native format logs: $work"
for arch in x86_64 arm64; do
    clang -arch "$arch" -O1 -Wall -Wextra -Werror -Wno-deprecated-declarations -D_FORTIFY_SOURCE=0 \
        tests/dynamic/native_va_list.c -o "$work/formats.$arch"
done
"$work/formats.arm64" > "$work/expected"
for engine in jit interpreter slow-bridge; do
    args=(-native -v)
    extra=()
    if [ "$engine" = interpreter ]; then args+=(-no-jit); fi
    if [ "$engine" = slow-bridge ]; then extra=(OCERZ_NO_BRIDGE_FASTCALL=1); fi
    env OCERZ_GUEST_ROOT= OCERZ_BRIDGESTAT=1 ${extra[@]+"${extra[@]}"} \
        /usr/bin/perl -e 'alarm 30; exec @ARGV' "$repo/ocerz" "${args[@]}" "$work/formats.x86_64" \
        > "$work/$engine.out" 2> "$work/$engine.err"
    cmp "$work/expected" "$work/$engine.out"
    grep -q 'native mode, shared cache not mapped' "$work/$engine.err"
    grep -Eq 'BRIDGESTAT.*crossings=[1-9][0-9]*' "$work/$engine.err"
    echo "PASS native va_list formats $engine"
    for refusal in positional long-double writeback; do
        rc=0
        env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 30; exec @ARGV' \
            "$repo/ocerz" "${args[@]}" "$work/formats.x86_64" "reject-$refusal" \
            > "$work/$refusal.$engine.out" 2> "$work/$refusal.$engine.err" || rc=$?
        [ "$rc" = 72 ]
        grep -q '_vasprintf refuses the format' "$work/$refusal.$engine.err"
        echo "PASS va_list $refusal refusal $engine"
    done
done
if [ "${OCERZ_TEST_CACHE:-0}" = 1 ]; then
    /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" -cache -v \
        "$work/formats.x86_64" > "$work/cache.out" 2> "$work/cache.err"
    cmp "$work/expected" "$work/cache.out"
    grep -q 'shared cache mapped at' "$work/cache.err"
    echo 'PASS existing cache-mode va_list formats'
fi
echo 'native format tests passed'

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
repo=$PWD
work=$(mktemp -d /tmp/ocerz-native-frameworks.XXXXXX)
echo "native framework logs: $work"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$work/key.pem" -out "$work/cert.pem" \
    -subj '/CN=ocerz framework test' -days 2 > "$work/certificate.log" 2>&1
openssl x509 -in "$work/cert.pem" -outform der -out "$work/cert.der"
for arch in x86_64 arm64; do
    clang -arch "$arch" -O1 -Wall -Wextra -Werror -Wno-deprecated-declarations \
        tests/dynamic/native_frameworks.c -framework CoreFoundation -framework CFNetwork \
        -framework SystemConfiguration -framework Security -framework CoreServices \
        -o "$work/frameworks.$arch"
done
/usr/bin/perl -e 'alarm 60; exec @ARGV' "$work/frameworks.arm64" "$work/cert.der" > "$work/expected" 2> "$work/arm.err"
for engine in jit interpreter slow-bridge; do
    args=(-native -v)
    extra=()
    if [ "$engine" = interpreter ]; then args+=(-no-jit); fi
    if [ "$engine" = slow-bridge ]; then extra=(OCERZ_NO_BRIDGE_FASTCALL=1); fi
    env OCERZ_GUEST_ROOT= OCERZ_BRIDGESTAT=1 OCERZ_PERFSTAT=1 ${extra[@]+"${extra[@]}"} \
        /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" "${args[@]}" \
        "$work/frameworks.x86_64" "$work/cert.der" > "$work/$engine.out" 2> "$work/$engine.err"
    cmp "$work/expected" "$work/$engine.out"
    grep -q 'native mode, shared cache not mapped' "$work/$engine.err"
    grep -Eq 'BRIDGESTAT.*crossings=[1-9][0-9]*' "$work/$engine.err"
    if grep -q 'shared cache mapped at' "$work/$engine.err"; then exit 1; fi
    if [ "$engine" = interpreter ]; then
        if grep -q 'PERFSTAT' "$work/$engine.err"; then exit 1; fi
    else
        grep -Eq 'PERFSTAT.*blocks=[1-9][0-9]* \(compiled=[1-9][0-9]*\)' "$work/$engine.err"
        grep -Eq 'EXECUTED insns: total=[1-9][0-9]*' "$work/$engine.err"
    fi
    echo "PASS native framework calls and callbacks $engine"
    for refusal in context launch; do
        rc=0
        env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 30; exec @ARGV' \
            "$repo/ocerz" "${args[@]}" "$work/frameworks.x86_64" "reject-$refusal" \
            > "$work/$refusal.$engine.out" 2> "$work/$refusal.$engine.err" || rc=$?
        [ "$rc" = 72 ]
        if [ "$refusal" = context ]; then
            grep -q 'SCNetworkReachabilityContext of version 42' "$work/$refusal.$engine.err"
        else
            grep -q '_LSOpenCFURLRef not implemented' "$work/$refusal.$engine.err"
        fi
        echo "PASS native framework $refusal refusal $engine"
    done
done
if [ "${OCERZ_TEST_CACHE:-0}" = 1 ]; then
    /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" -cache -v \
        "$work/frameworks.x86_64" "$work/cert.der" > "$work/cache.out" 2> "$work/cache.err"
    cmp "$work/expected" "$work/cache.out"
    grep -q 'shared cache mapped at' "$work/cache.err"
    echo 'PASS existing cache-mode framework calls and callbacks'
fi
echo 'native framework tests passed'

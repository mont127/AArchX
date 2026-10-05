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
    clang -arch "$arch" -O1 -Wall -Wextra -Werror -Wno-deprecated-declarations -fobjc-arc \
        tests/dynamic/native_frameworks_more.m -framework AppKit -framework Security \
        -framework UniformTypeIdentifiers -framework Network -o "$work/more.$arch"
    clang -arch "$arch" -O1 -Wall -Wextra -Werror -fno-builtin tests/dynamic/native_compat.c \
        -framework CoreFoundation -framework CoreMedia -framework GSS -o "$work/compat.$arch"
    clang -arch "$arch" -O1 -Wall -Wextra -Werror -Wno-deprecated-declarations tests/dynamic/native_kerberos.c \
        -framework Kerberos -o "$work/kerberos.$arch"
    clang -arch "$arch" -O1 -Wall -Wextra -Werror -Wno-deprecated-declarations tests/dynamic/native_gl_carbon.c \
        -framework OpenGL -framework Carbon -o "$work/gl_carbon.$arch"
    clang -arch "$arch" -O1 -Wall -Wextra -Werror -Wno-deprecated-declarations tests/dynamic/native_callback_structs.c \
        -framework CoreFoundation -framework CoreGraphics -framework CoreText -framework ImageIO -framework Carbon \
        -o "$work/callback_structs.$arch"
    clang -arch "$arch" -O1 -Wall -Wextra -Werror -Wno-deprecated-declarations tests/dynamic/native_lapack_asn1.c \
        -framework Accelerate -framework Security -o "$work/lapack_asn1.$arch"
    clang -arch "$arch" -O1 -Wall -Wextra -Werror tests/dynamic/native_audio.c -framework CoreAudio -o "$work/audio.$arch"
    clang -arch "$arch" -O1 -Wall -Wextra -Werror tests/dynamic/native_videotoolbox.c -framework CoreMedia \
        -framework CoreVideo -framework VideoToolbox -framework CoreFoundation -o "$work/videotoolbox.$arch"
    clang -arch "$arch" -O1 -Wall -Wextra -Werror tests/dynamic/native_sqlite.c -lsqlite3 -o "$work/sqlite.$arch"
done
mkdir -p "$work/globdir"
touch "$work/globdir/a.txt" "$work/globdir/b.txt" "$work/globdir/c.log"
/usr/bin/perl -e 'alarm 60; exec @ARGV' "$work/frameworks.arm64" "$work/cert.der" > "$work/expected" 2> "$work/arm.err"
/usr/bin/perl -e 'alarm 60; exec @ARGV' "$work/more.arm64" > "$work/more.expected" 2> "$work/more.arm.err"
/usr/bin/perl -e 'alarm 60; exec @ARGV' "$work/compat.arm64" "$work/globdir" > "$work/compat.expected" \
    2> "$work/compat.arm.err"
sed 's/compat\.arm64/compat/' "$work/compat.arm.err" > "$work/compat.expected.err"
"$work/kerberos.arm64" > "$work/kerberos.expected"
"$work/gl_carbon.arm64" > "$work/gl_carbon.expected"
"$work/callback_structs.arm64" > "$work/callback_structs.expected"
"$work/lapack_asn1.arm64" > "$work/lapack_asn1.expected"
"$work/audio.arm64" > "$work/audio.expected"
"$work/videotoolbox.arm64" > "$work/videotoolbox.expected"
"$work/sqlite.arm64" > "$work/sqlite.expected"
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
    env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" \
        "${args[@]}" "$work/more.x86_64" > "$work/more.$engine.out" 2> "$work/more.$engine.err"
    cmp "$work/more.expected" "$work/more.$engine.out"
    echo "PASS native framework sorts, ciphers, locale formats, CF callbacks, UTType, Network, variable bindings, va_list methods, category properties, block implementations and uncaught handlers $engine"
    env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" \
        "${args[@]}" "$work/compat.x86_64" "$work/globdir" > "$work/compat.$engine.out" 2> "$work/compat.$engine.err"
    cmp "$work/compat.expected" "$work/compat.$engine.out"
    grep -v '^ocerz' "$work/compat.$engine.err" | sed 's/compat\.x86_64/compat/' | cmp "$work/compat.expected.err" -
    echo "PASS native err and warn veneers, long double, glob, CFCalendar, CMTime, CF va_list, zones, fenv, GSS and 128-bit arithmetic $engine"
    env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" \
        "${args[@]}" "$work/kerberos.x86_64" > "$work/kerberos.$engine.out" 2> "$work/kerberos.$engine.err"
    cmp "$work/kerberos.expected" "$work/kerberos.$engine.out"
    echo "PASS native Kerberos object identifier variables $engine"
    env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" \
        "${args[@]}" "$work/gl_carbon.x86_64" > "$work/gl_carbon.$engine.out" 2> "$work/gl_carbon.$engine.err"
    cmp "$work/gl_carbon.expected" "$work/gl_carbon.$engine.out"
    echo "PASS native OpenGL, Carbon time and hot keys $engine"
    env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" \
        "${args[@]}" "$work/callback_structs.x86_64" > "$work/callback_structs.$engine.out" \
        2> "$work/callback_structs.$engine.err"
    cmp "$work/callback_structs.expected" "$work/callback_structs.$engine.out"
    echo "PASS native data consumers, patterns, stream clients, run delegates, ports, sockets and function results $engine"
    env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" \
        "${args[@]}" "$work/lapack_asn1.x86_64" > "$work/lapack_asn1.$engine.out" 2> "$work/lapack_asn1.$engine.err"
    cmp "$work/lapack_asn1.expected" "$work/lapack_asn1.$engine.out"
    echo "PASS native ILP64 BLAS and LAPACK and SecAsn1 coders $engine"
    env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" \
        "${args[@]}" "$work/audio.x86_64" > "$work/audio.$engine.out" 2> "$work/audio.$engine.err"
    cmp "$work/audio.expected" "$work/audio.$engine.out"
    echo "PASS native audio device IOProcs and IO blocks $engine"
    env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" \
        "${args[@]}" "$work/videotoolbox.x86_64" > "$work/videotoolbox.$engine.out" 2> "$work/videotoolbox.$engine.err"
    cmp "$work/videotoolbox.expected" "$work/videotoolbox.$engine.out"
    echo "PASS native H.264 encode and decode through VideoToolbox callbacks and CoreMedia block sources $engine"
    env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" \
        "${args[@]}" "$work/sqlite.x86_64" > "$work/sqlite.$engine.out" 2> "$work/sqlite.$engine.err"
    cmp "$work/sqlite.expected" "$work/sqlite.$engine.out"
    echo "PASS native sqlite3_config and sqlite3_db_config $engine"
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

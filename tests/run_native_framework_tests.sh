#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
repo=$PWD
work=$(mktemp -d /tmp/ocerz-native-frameworks.XXXXXX)
echo "native framework logs: $work"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$work/key.pem" -out "$work/cert.pem" \
    -subj '/CN=ocerz framework test' -days 2 > "$work/certificate.log" 2>&1
openssl x509 -in "$work/cert.pem" -outform der -out "$work/cert.der"
# A framework with an identifier, an Info.plist and a resource, which
# native_bundle_id dlopens and then looks up by identifier.
probe_framework() {
    local arch=$1 fw="$work/fw.$1/OcerzProbe.framework"
    mkdir -p "$fw/Versions/A/Resources"
    printf 'int ocerz_probe(void) { return 7; }\n' > "$work/probe.c"
    clang -arch "$arch" -dynamiclib -install_name @rpath/OcerzProbe.framework/OcerzProbe "$work/probe.c" \
        -o "$fw/Versions/A/OcerzProbe"
    printf '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>CFBundleIdentifier</key><string>org.ocerz.probe</string><key>CFBundleExecutable</key><string>OcerzProbe</string><key>CFBundlePackageType</key><string>FMWK</string></dict></plist>\n' \
        > "$fw/Versions/A/Resources/Info.plist"
    printf 'shaders' > "$fw/Versions/A/Resources/probe.txt"
    ln -sfn A "$fw/Versions/Current"
    ln -sfn Versions/Current/OcerzProbe "$fw/OcerzProbe"
    ln -sfn Versions/Current/Resources "$fw/Resources"
}
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
    clang -arch "$arch" -O1 -Wall -Wextra -Werror tests/dynamic/native_audio.c -framework CoreAudio \
        -framework AudioUnit -framework AudioToolbox -o "$work/audio.$arch"
    clang -arch "$arch" -O1 -Wall -Wextra -Werror tests/dynamic/native_videotoolbox.c -framework CoreMedia \
        -framework CoreVideo -framework VideoToolbox -framework CoreFoundation -o "$work/videotoolbox.$arch"
    clang -arch "$arch" -O1 -Wall -Wextra -Werror tests/dynamic/native_sqlite.c -lsqlite3 -o "$work/sqlite.$arch"
    clang -arch "$arch" -O1 -Wall -Wextra -Werror -fno-objc-arc tests/dynamic/native_metal_events.m \
        -framework Foundation -framework Metal -o "$work/metal_events.$arch"
    clang -arch "$arch" -O1 -Wall -Wextra -Werror tests/dynamic/native_wine_imports.c \
        -framework SystemConfiguration -framework CoreFoundation -lresolv -o "$work/wine_imports.$arch"
    clang -arch "$arch" -O1 -Wall -Wextra -Werror -fno-objc-arc tests/dynamic/native_bundle_id.m \
        -framework Foundation -o "$work/bundle_id.$arch"
    probe_framework "$arch"
    # A library found only through the executable's LC_RPATH, by its leaf name.
    mkdir -p "$work/leaf.$arch"
    printf 'int ocerz_leaf(void) { return 7; }\n' > "$work/leaf.c"
    clang -arch "$arch" -dynamiclib -install_name @rpath/libocerzleaf.dylib "$work/leaf.c" \
        -o "$work/leaf.$arch/libocerzleaf.dylib"
    clang -arch "$arch" -O1 -Wall -Wextra -Werror tests/dynamic/native_game_imports.c -framework CoreFoundation \
        -framework IOKit -Wl,-rpath,"$work/leaf.$arch" -o "$work/game_imports.$arch"
done
# The complex-arithmetic line only proves anything if the builtins are calls.
for builtin in ___divdc3 ___divsc3 ___muldc3 ___mulsc3 ___powidf2 ___powisf2; do
    nm -u "$work/game_imports.x86_64" | grep -qx "$builtin"
done
# -no_pie is x86_64 only, so Rosetta is the oracle again.
clang -arch x86_64 -mmacosx-version-min=10.9 -O1 -Wall -Wextra -Werror -fno-objc-arc tests/dynamic/native_nopie.m \
    -framework Foundation -Wl,-no_pie -o "$work/nopie.x86_64" 2> "$work/nopie.link"
if otool -hv "$work/nopie.x86_64" | grep -qw PIE; then exit 1; fi
echo 'nopie table=3 message=h string=1 method=7' > "$work/nopie.expected"
clang -arch x86_64 -O1 -Wall -Wextra -Werror tests/dynamic/native_ldt.c -o "$work/ldt.x86_64"
# i386_set_ldt is x86_64 only, so Rosetta is the oracle: this is its line.
echo 'ldt set=16 read=17 set=0xcffa000000ffff got=0xcffa000000ffff bad=-1 errno=EINVAL' > "$work/ldt.expected"
clang -arch x86_64 -O1 -Wall -Wextra -Werror tests/dynamic/native_thread_state.c -o "$work/thread_state.x86_64"
echo 'debug64 kr=0 count=16 zero=1; debug kr=0 count=18 header=11/16; set zero kr=0' > "$work/thread_state.expected"
# Linked the way Wine's loader is, so native mode runs it under a low shadow;
# its output is Rosetta's.
clang -arch x86_64 -O1 -Wall -Wextra -Werror -fno-objc-arc tests/dynamic/native_low_wine.m -framework Foundation \
    -Wl,-no_pie -Wl,-pagezero_size,0x1000 -Wl,-image_base,0x200000000 -o "$work/low_wine.x86_64" 2> /dev/null
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
"$work/metal_events.arm64" > "$work/metal_events.expected"
"$work/wine_imports.arm64" > "$work/wine_imports.expected"
"$work/bundle_id.arm64" "$work/fw.arm64/OcerzProbe.framework/OcerzProbe" > "$work/bundle_id.expected"
"$work/game_imports.arm64" > "$work/game_imports.expected"
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
    echo "PASS native framework sorts, ciphers, locale formats, CF callbacks, UTType, Network, variable bindings, va_list methods, category properties, union results, block implementations and uncaught handlers $engine"
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
    echo "PASS native OpenGL, GLU, Carbon time and hot keys $engine"
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
    echo "PASS native audio device IOProcs, IO blocks and output unit render callbacks $engine"
    env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" \
        "${args[@]}" "$work/videotoolbox.x86_64" > "$work/videotoolbox.$engine.out" 2> "$work/videotoolbox.$engine.err"
    cmp "$work/videotoolbox.expected" "$work/videotoolbox.$engine.out"
    echo "PASS native H.264 encode and decode through VideoToolbox callbacks and CoreMedia block sources $engine"
    env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" \
        "${args[@]}" "$work/sqlite.x86_64" > "$work/sqlite.$engine.out" 2> "$work/sqlite.$engine.err"
    cmp "$work/sqlite.expected" "$work/sqlite.$engine.out"
    echo "PASS native sqlite3_config and sqlite3_db_config $engine"
    env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" \
        "${args[@]}" "$work/ldt.x86_64" > "$work/ldt.$engine.out" 2> "$work/ldt.$engine.err"
    cmp "$work/ldt.expected" "$work/ldt.$engine.out"
    echo "PASS native i386_set_ldt and i386_get_ldt $engine"
    env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" \
        "${args[@]}" "$work/thread_state.x86_64" > "$work/thread_state.$engine.out" 2> "$work/thread_state.$engine.err"
    cmp "$work/thread_state.expected" "$work/thread_state.$engine.out"
    echo "PASS native debug-register thread state $engine"
    env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" \
        "${args[@]}" "$work/metal_events.x86_64" > "$work/metal_events.$engine.out" 2> "$work/metal_events.$engine.err"
    cmp "$work/metal_events.expected" "$work/metal_events.$engine.out"
    echo "PASS native Metal shared-event blocks, the shader cache path and swizzle keys $engine"
    env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" \
        "${args[@]}" "$work/wine_imports.x86_64" > "$work/wine_imports.$engine.out" 2> "$work/wine_imports.$engine.err"
    cmp "$work/wine_imports.expected" "$work/wine_imports.$engine.out"
    echo "PASS native ulock signals, iovecs, msghdrs, resolver state, zone statistics, DHCP info and image UUIDs $engine"
    env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" \
        "${args[@]}" "$work/bundle_id.x86_64" "$work/fw.x86_64/OcerzProbe.framework/OcerzProbe" \
        > "$work/bundle_id.$engine.out" 2> "$work/bundle_id.$engine.err"
    cmp "$work/bundle_id.expected" "$work/bundle_id.$engine.out"
    echo "PASS native bundle lookup by identifier for a dlopened framework $engine"
    env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" \
        "${args[@]}" "$work/game_imports.x86_64" > "$work/game_imports.$engine.out" 2> "$work/game_imports.$engine.err"
    cmp "$work/game_imports.expected" "$work/game_imports.$engine.out"
    echo "PASS native complex division, CFUUID stack bytes, refused plug-ins, cleanup handlers, dlsym through dependencies and rpath leaf names $engine"
    env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" \
        "${args[@]}" "$work/nopie.x86_64" > "$work/nopie.$engine.out" 2> "$work/nopie.$engine.err"
    cmp "$work/nopie.expected" "$work/nopie.$engine.out"
    grep -q 'non-PIE .* pointers rebased by scan' "$work/nopie.$engine.err"
    echo "PASS native non-PIE executable slid into the arena and rebased by scan $engine"
    env OCERZ_GUEST_ROOT= ${extra[@]+"${extra[@]}"} /usr/bin/perl -e 'alarm 60; exec @ARGV' "$repo/ocerz" \
        "${args[@]}" "$work/low_wine.x86_64" > "$work/low_wine.$engine.out" 2> "$work/low_wine.$engine.err"
    cmp tests/dynamic/native_low_wine.out "$work/low_wine.$engine.out"
    echo "PASS native Wine layout: pool token, low __block, coherent and claimed mach_vm_map $engine"
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

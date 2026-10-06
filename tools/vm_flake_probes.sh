#!/usr/bin/env bash
# The four measurements that explained the failures of make check on GitHub's
# xcode-27 runner, a virtual machine, each run natively as arm64 and again as
# x86-64 under ocerz, translated and interpreted, so that a difference between
# the three says ocerz is at fault and agreement says the machine is.
#
#   metal     a Metal compute over a no-copy, a copied and a plain shared
#             buffer. On the runner's paravirtual GPU all three are right
#             natively and only the no-copy buffer is wrong under ocerz.
#   ntp       ntp_gettime, the raw syscall 528 and ntp_adjtime. On a machine
#             whose clock is not synchronised ntp_gettime fails with EIO and
#             ntp_adjtime reports TIME_ERROR with STA_UNSYNC, natively and under
#             ocerz alike.
#   dispatch  tests/dynamic/native_dispatch_time.c, run ROUNDS times with the
#             three engines in turn in each round, counting the rounds in which
#             a line reports a wait outside its bounds. On the runner about a
#             third of the rounds did, in every engine.
#   attach    the attach_apply fixture of tests/run_native_tests.sh, extracted
#             from it, run ROUNDS times the same way, with how many of the 384
#             iterations the calling thread ran. Natively and interpreted the
#             three threads share them about evenly; translated, the calling
#             thread ran 382 of them in half the rounds, which leaves two
#             iterations of margin before the fixture's check that a worker took
#             part fails.
#
# On a Mac with a real GPU and a synchronised clock the first two are expected
# to pass in every engine, and they are mostly useful inside a macOS virtual
# machine; the last two measure timing and are worth running on both.
#
#   bash tools/vm_flake_probes.sh            all four
#   bash tools/vm_flake_probes.sh attach     one of them
#   ROUNDS=20 bash tools/vm_flake_probes.sh dispatch
#
# ocerz must be built (make), and dispatch and attach run in native mode, which
# needs the API databases (make apis). ROUNDS defaults to 40 for dispatch and
# 100 for attach.
set -u
cd "$(dirname "$0")/.."
repo=$PWD
ocerz=$repo/ocerz
work=$(mktemp -d "${TMPDIR:-/tmp}/ocerz-probe.XXXXXX")
trap 'rm -rf "$work"' EXIT

if [ ! -x "$ocerz" ]; then
    echo "ocerz is not built: run make first" >&2
    exit 2
fi

need_apis() {
    if [ ! -d runtime/apis ]; then
        echo "native mode needs the API databases: run make apis first" >&2
        exit 2
    fi
}

probe_metal() {
    echo "== metal: a compute over a no-copy, a copied and a plain shared buffer"
    cat > "$work/mprobe.m" <<'SRC'
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static void probe(id<MTLDevice> dev, const char *label, int mode)
{
    @autoreleasepool {
        size_t len = 0x10000;
        size_t n = len / sizeof(float);
        float *mem = mmap((void *)0x240000000, len + 0x10000, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (mem == MAP_FAILED) {
            printf("%s: mmap failed\n", label);
            return;
        }
        for (size_t i = 0; i < n; i++)
            mem[i] = (float)i + 1000.0f;
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@"kernel void k(device const float *in [[buffer(0)]], device float *out [[buffer(1)]], uint i [[thread_position_in_grid]]) { out[i] = in[i] * 2.0f; }" options:nil error:&err];
        if (!lib) {
            printf("%s: newLibrary failed: %s\n", label, err.localizedDescription.UTF8String);
            return;
        }
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&err];
        if (!ps) {
            printf("%s: pipeline failed: %s\n", label, err.localizedDescription.UTF8String);
            return;
        }
        id<MTLBuffer> in = nil;
        if (mode == 0) {
            in = [dev newBufferWithBytesNoCopy:mem length:len options:MTLResourceStorageModeShared deallocator:nil];
        } else if (mode == 1) {
            in = [dev newBufferWithBytes:mem length:len options:MTLResourceStorageModeShared];
        } else {
            in = [dev newBufferWithLength:len options:MTLResourceStorageModeShared];
            memcpy([in contents], mem, len);
        }
        id<MTLBuffer> out = [dev newBufferWithLength:len options:MTLResourceStorageModeShared];
        if (!in || !out) {
            printf("%s: buffer creation failed (in=%d out=%d)\n", label, in != nil, out != nil);
            return;
        }
        memset([out contents], 0x55, len);
        id<MTLCommandBuffer> cb = [[dev newCommandQueue] commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:ps];
        [e setBuffer:in offset:0 atIndex:0];
        [e setBuffer:out offset:0 atIndex:1];
        [e dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const float *o = [out contents];
        size_t bad = 0;
        for (size_t i = 0; i < n; i++)
            if (o[i] != 2.0f * ((float)i + 1000.0f))
                bad++;
        printf("%s: status=%ld error=%s wrong=%zu/%zu first=[%g %g %g] want=[2000 2002 2004]\n",
               label, (long)cb.status, cb.error ? cb.error.localizedDescription.UTF8String : "none",
               bad, n, o[0], o[1], o[2]);
        munmap(mem, len + 0x10000);
    }
}

int main(void)
{
    @autoreleasepool {
        NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
        printf("Metal devices: %lu\n", (unsigned long)all.count);
        for (id<MTLDevice> d in all)
            printf("  %s unified=%d\n", d.name.UTF8String, (int)d.hasUnifiedMemory);
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            printf("default device: none\n");
            return 0;
        }
        probe(dev, "bytesNoCopy, what the repository's case does", 0);
        probe(dev, "bytes, copied in", 1);
        probe(dev, "newBufferWithLength then memcpy", 2);
    }
    return 0;
}
SRC
    local pagezero=(-Wl,-no_pie -Wl,-pagezero_size,0x1000 -Wl,-image_base,0x200000000)
    if ! clang -arch arm64 -fobjc-arc -framework Metal -framework Foundation \
            -o "$work/mprobe_a64" "$work/mprobe.m" 2>/dev/null ||
       ! clang -arch x86_64 -fobjc-arc -framework Metal -framework Foundation \
            -o "$work/mprobe_x64" "$work/mprobe.m" "${pagezero[@]}" 2>/dev/null; then
        echo "  could not build the probe for both architectures"
        return
    fi
    echo "-- native arm64";       "$work/mprobe_a64" || true
    echo "-- ocerz, translated";  "$ocerz" "$work/mprobe_x64" || true
    echo "-- ocerz, interpreted"; "$ocerz" -no-jit "$work/mprobe_x64" || true

    echo "-- the repository's own case, tests/dynamic/metal_nocopy_low.m"
    if clang -arch arm64 -fobjc-arc -framework Metal -framework Foundation \
            -o "$work/real_a64" tests/dynamic/metal_nocopy_low.m 2>/dev/null &&
       clang -arch x86_64 -fobjc-arc -framework Metal -framework Foundation \
            -o "$work/real_x64" tests/dynamic/metal_nocopy_low.m "${pagezero[@]}" 2>/dev/null; then
        echo "   native arm64:       $("$work/real_a64" 2>&1 | tr '\n' ' ')"
        echo "   ocerz, translated:  $("$ocerz" "$work/real_x64" 2>&1 | tr '\n' ' ')"
        echo "   ocerz, interpreted: $("$ocerz" -no-jit "$work/real_x64" 2>&1 | tr '\n' ' ')"
        echo "   (OK means it passes; 'extra 0: 16384 of 16384 wrong' is the failure seen on the runner)"
    else
        echo "   could not build it"
    fi
}

probe_ntp() {
    echo "== ntp: ntp_gettime, the raw syscall and ntp_adjtime"
    cat > "$work/ntp.c" <<'SRC'
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/timex.h>
#include <unistd.h>
int main(void)
{
    struct ntptimeval t;
    memset(&t, 0, sizeof t);
    errno = 0;
    int r = ntp_gettime(&t);
    printf("ntp_gettime:  ret=%d errno=%d (%s) maxerror=%ld esterror=%ld\n", r, errno, errno ? strerror(errno) : "-", (long)t.maxerror, (long)t.esterror);
    memset(&t, 0, sizeof t);
    errno = 0;
    long s = syscall(528, &t);
    printf("syscall(528): ret=%ld errno=%d (%s)\n", s, errno, errno ? strerror(errno) : "-");
    struct timex x;
    memset(&x, 0, sizeof x);
    errno = 0;
    r = ntp_adjtime(&x);
    printf("ntp_adjtime:  ret=%d errno=%d (%s) status=%#x\n", r, errno, errno ? strerror(errno) : "-", (unsigned)x.status);
    return 0;
}
SRC
    clang -arch arm64 -o "$work/ntp_a64" "$work/ntp.c" 2>/dev/null &&
        clang -arch x86_64 -o "$work/ntp_x64" "$work/ntp.c" 2>/dev/null || {
        echo "  could not build the probe for both architectures"
        return
    }
    echo "-- native arm64";       "$work/ntp_a64" || true
    echo "-- ocerz, translated";  "$ocerz" "$work/ntp_x64" || true
    echo "-- ocerz, interpreted"; "$ocerz" -no-jit "$work/ntp_x64" || true
    echo "-- ocerz -strace, the ntp lines"
    "$ocerz" -strace "$work/ntp_x64" 2>&1 | grep -i "ntp" | head -4 || true
    echo "   (ret=-1 errno=5 and TIME_ERROR, ret=5, with status 0x40 means the clock is not synchronised;"
    echo "    a synchronised machine returns 0 from ntp_gettime)"
}

probe_dispatch() {
    local rounds=${ROUNDS:-40} e i
    need_apis
    echo "== dispatch: native_dispatch_time, $rounds rounds, the three engines in turn"
    clang -arch arm64 -O1 tests/dynamic/native_dispatch_time.c -o "$work/ndt_a64" &&
        clang -arch x86_64 -O1 tests/dynamic/native_dispatch_time.c -o "$work/ndt_x64" || {
        echo "  could not build the test for both architectures"
        return
    }
    : > "$work/bad_rounds"
    : > "$work/bad_lines"
    for i in $(seq 1 "$rounds"); do
        "$work/ndt_a64" > "$work/o.native" || true
        "$ocerz" -native "$work/ndt_x64" > "$work/o.translated" 2>/dev/null || true
        "$ocerz" -native -no-jit "$work/ndt_x64" > "$work/o.interpreted" 2>/dev/null || true
        for e in native translated interpreted; do
            if grep -q '=0$' "$work/o.$e"; then
                echo "$e" >> "$work/bad_rounds"
                grep '=0$' "$work/o.$e" | sed "s/ r=.*//; s/^/$e: /" >> "$work/bad_lines"
            fi
        done
    done
    echo "rounds in which some line reported a wait outside its bounds, out of $rounds:"
    for e in native translated interpreted; do
        printf '  %-12s %s\n' "$e" "$(grep -c "^$e\$" "$work/bad_rounds" || true)"
    done
    echo "which lines:"
    sort "$work/bad_lines" | uniq -c | sort -rn | sed 's/^/  /'
    echo "(similar counts in every engine mean the machine's timing is noisy and ocerz is not at fault)"
}

summarise() {
    sort -n "$1" | awk '
        { a[NR] = $1; if ($1 >= 380) c++ }
        END {
            if (NR == 0) { print "no data"; exit }
            printf "calling thread ran min=%d median=%d max=%d of 384; 380 or more in %d of %d runs\n",
                   a[1], a[int((NR + 1) / 2)], a[NR], c, NR
        }'
}

probe_attach() {
    local rounds=${ROUNDS:-100} e i name
    need_apis
    echo "== attach: the attach_apply fixture, $rounds rounds, the three engines in turn"
    mkdir -p "$work/fx"
    python3 - "$work/fx" <<'PY' || { echo "  could not extract the fixture"; return; }
import re, sys
s = open('tests/run_native_tests.sh').read()
for name in ('cb_common.h', 'attach_common.h', 'attach_apply.c'):
    ms = re.findall(r'cat > "\$TMP/' + re.escape(name) + r"\" <<'EOC'\n(.*?)\nEOC\n", s, re.S)
    assert len(ms) == 1, (name, len(ms))
    open(sys.argv[1] + '/' + name, 'w').write(ms[0] + '\n')
PY
    (cd "$work/fx" &&
        clang -arch x86_64 -std=c11 -O1 -fno-builtin -fno-stack-protector -o attach_apply_x64 attach_apply.c &&
        clang -arch arm64 -std=c11 -O1 -fno-builtin -fno-stack-protector -o attach_apply_a64 attach_apply.c) || {
        echo "  could not build the fixture for both architectures"
        return
    }
    for e in native translated interpreted; do
        : > "$work/fx/out.$e"
        : > "$work/fx/calling.$e"
    done
    for i in $(seq 1 "$rounds"); do
        "$work/fx/attach_apply_a64" > "$work/fx/o.native" 2> "$work/fx/e.native" || true
        "$ocerz" -native "$work/fx/attach_apply_x64" > "$work/fx/o.translated" 2> "$work/fx/e.translated" || true
        "$ocerz" -native -no-jit "$work/fx/attach_apply_x64" > "$work/fx/o.interpreted" 2> "$work/fx/e.interpreted" || true
        for e in native translated interpreted; do
            head -1 "$work/fx/o.$e" | sed 's/ n=.*//' >> "$work/fx/out.$e"
            grep -h '^attach_apply: threads=' "$work/fx/e.$e" | head -1 | sed 's/.*calling=//' >> "$work/fx/calling.$e" || true
        done
    done
    echo "the fixture's own checks over $rounds rounds:"
    for e in native translated interpreted; do
        echo "  $e:"
        sort "$work/fx/out.$e" | uniq -c | sed 's/^/    /'
    done
    echo "how the 384 iterations were shared:"
    for e in native translated interpreted; do
        printf '  %-12s ' "$e"
        summarise "$work/fx/calling.$e"
    done
    echo "('bad:00000080' is the failure seen once on the runner: no iteration ran off the calling thread)"
}

what=${1:-all}
case $what in
    metal) probe_metal ;;
    ntp) probe_ntp ;;
    dispatch) probe_dispatch ;;
    attach) probe_attach ;;
    all)
        probe_metal
        echo
        probe_ntp
        echo
        probe_dispatch
        echo
        probe_attach
        ;;
    *)
        echo "usage: bash tools/vm_flake_probes.sh [metal|ntp|dispatch|attach|all]" >&2
        exit 2
        ;;
esac

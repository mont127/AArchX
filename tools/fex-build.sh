#!/usr/bin/env bash
# fex-build.sh -- fetch FEX-Emu, port it to macOS and build FEXCore for
# `make FEX=<dir>`, which links it into ocerz for -fex.
#
#   tools/fex-build.sh <dir>
#
# FEX is fetched at FEX_REV with the submodules FEXCore's build needs into
# <dir>/FEX, tools/fex-macos.patch is applied to it, and the CMake build goes to
# <dir>/build.  Run again on an existing <dir> it only rebuilds.
#
# What the patch changes, in FEX's terms: Darwin passes the platform check;
# Linux-only headers and calls get macOS forms (futex -> os_sync_wait_on_address,
# getdents64, sendfile, prctl, sincos, the memory advice flags); code memory is
# MAP_JIT and every write to it holds a JITWriteScope, since a thread sees such
# memory writable or executable, never both; MADV_DONTNEED zero-fills through
# MADV_ZERO as Linux's does; the 4 KB guard pages after code buffers are left
# out, 16 KB host pages being larger than the space they guard; and the code
# cache, which needs mremap, refuses to run.
set -euo pipefail

FEX_REV=1c4f04b71a4d33091183fd7e1a2c09e6823d27fd
SUBMODULES=(External/fmt External/xxhash External/unordered_dense External/range-v3 External/vixl External/zydis
            Source/Common/cpp-optparse)

dir=${1:?usage: tools/fex-build.sh <dir>}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$dir"
dir=$(cd "$dir" && pwd)

if [ ! -d "$dir/FEX/.git" ]; then
    git init -q "$dir/FEX"
    git -C "$dir/FEX" remote add origin https://github.com/FEX-Emu/FEX.git
    git -C "$dir/FEX" fetch -q --depth 1 origin "$FEX_REV"
    git -C "$dir/FEX" checkout -q FETCH_HEAD
    git -C "$dir/FEX" submodule update -q --init --depth 1 "${SUBMODULES[@]}"
    git -C "$dir/FEX" apply "$here/fex-macos.patch"
fi

if [ ! -f "$dir/build/CMakeCache.txt" ]; then
    cmake -S "$dir/FEX" -B "$dir/build" -DCMAKE_BUILD_TYPE=Release -DTUNE_CPU=apple-m1 -DENABLE_LTO=OFF \
        -DBUILD_FEXCONFIG=OFF -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF -DENABLE_FEX_ALLOCATOR=OFF \
        -DENABLE_OFFLINE_TELEMETRY=OFF -DBUILD_TESTING=OFF -DENABLE_CCACHE=OFF -DBUILD_THUNKS=OFF \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
fi
make -C "$dir/build" -j"$(sysctl -n hw.ncpu)" FEXCore JemallocDummy
echo "FEXCore is built; now: make FEX=$dir"

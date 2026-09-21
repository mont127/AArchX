#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
repo=$PWD
revision=2078da43e25a4623cab2d0d60decddf709aaea28
build=${OCERZ_CXX_BUILD:-"$repo/tools/cxx/build"}
source_dir=${OCERZ_LLVM_SOURCE:-"$build/llvm-project"}
dest=${1:-"$repo/runtime/guest"}
mkdir -p "$build" "$dest/usr/lib"
build=$(cd "$build" && pwd -P)
dest=$(cd "$dest" && pwd -P)
if [ ! -d "$source_dir/.git" ]; then
    git clone --depth 1 --filter=blob:none --sparse --branch llvmorg-21.1.8 \
        https://github.com/llvm/llvm-project.git "$source_dir"
    git -C "$source_dir" sparse-checkout set runtimes cmake libcxx libcxxabi libunwind libc llvm/cmake llvm/utils/llvm-lit llvm/utils/lit
fi
source_dir=$(cd "$source_dir" && pwd -P)
if [ "$(git -C "$source_dir" rev-parse HEAD)" != "$revision" ]; then
    echo "expected LLVM revision $revision at $source_dir" >&2
    exit 2
fi
cmake -S "$source_dir/runtimes" -B "$build/runtime" -G 'Unix Makefiles' \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    "-DCMAKE_CXX_FLAGS=-I\"$source_dir/libc\"" \
    '-DLLVM_ENABLE_RUNTIMES=libunwind;libcxxabi;libcxx' \
    -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBUNWIND_INCLUDE_TESTS=OFF \
    -DLIBCXX_ENABLE_STATIC=OFF -DLIBCXXABI_ENABLE_STATIC=OFF -DLIBUNWIND_ENABLE_STATIC=OFF \
    -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXX_ENABLE_ABI_LINKER_SCRIPT=OFF
cmake --build "$build/runtime" -j "${OCERZ_BUILD_JOBS:-4}"
cp "$build/runtime/lib/libc++.1.0.dylib" "$dest/usr/lib/libc++.1.dylib"
cp "$build/runtime/lib/libc++abi.1.0.dylib" "$dest/usr/lib/libc++abi.dylib"
cp "$build/runtime/lib/libunwind.1.0.dylib" "$dest/usr/lib/libunwind.1.dylib"
install_name_tool -id /usr/lib/libc++.1.dylib \
    -change @rpath/libc++abi.1.dylib /usr/lib/libc++abi.dylib \
    -change @rpath/libunwind.1.dylib /usr/lib/libunwind.1.dylib "$dest/usr/lib/libc++.1.dylib"
install_name_tool -id /usr/lib/libc++abi.dylib \
    -change @rpath/libunwind.1.dylib /usr/lib/libunwind.1.dylib "$dest/usr/lib/libc++abi.dylib"
install_name_tool -id /usr/lib/libunwind.1.dylib "$dest/usr/lib/libunwind.1.dylib"
for component in libcxx libcxxabi libunwind; do
    cp "$source_dir/$component/LICENSE.TXT" "$dest/LICENSE.$component.txt"
done
file "$dest/usr/lib/"*.dylib
echo "LLVM $revision guest C++ runtime installed at $dest"

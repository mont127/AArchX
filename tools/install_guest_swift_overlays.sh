#!/usr/bin/env bash
# Copies the x86_64 Swift overlay dylibs a macOS program's LC_LOAD_DYLIB
# commands name into runtime/guest, beside the swift.org runtime libraries.
# Xcode carries them under the Swift 5 compatibility directory as standalone
# Mach-O files; their @rpath install names are rewritten to /usr/lib/swift so
# native_guest_path can supply them in place of the shared-cache images.
set -euo pipefail
cd "$(dirname "$0")/.."
repo=$PWD
developer_dir=${DEVELOPER_DIR:-$(xcode-select -p)}
overlays="$developer_dir/Toolchains/XcodeDefault.xctoolchain/usr/lib/swift-5.0/macosx"
dest=${1:-"$repo/runtime/guest"}
# libswiftDispatch is the first overlay with a working guest closure here: its
# Swift dependencies are libswiftCore (from tools/install_guest_swift.sh) and
# the Darwin/ObjectiveC overlays below.
libs="libswiftDispatch libswiftDarwin libswiftObjectiveC"
mkdir -p "$dest/usr/lib/swift"
dest=$(cd "$dest" && pwd -P)
for lib in $libs; do
    src="$overlays/$lib.dylib"
    if [ ! -f "$src" ]; then
        echo "missing $src: Xcode's Swift overlay libraries are required" >&2
        exit 2
    fi
    if ! file "$src" | grep -q 'x86_64'; then
        echo "$src is not an x86_64 dylib" >&2
        exit 2
    fi
    cp "$src" "$dest/usr/lib/swift/$lib.dylib"
done
cd "$dest/usr/lib/swift"
for lib in $libs; do
    install_name_tool -id "/usr/lib/swift/$lib.dylib" "$lib.dylib"
done
for lib in $libs; do
    f="$lib.dylib"
    otool -L "$f" | awk '/@rpath\/libswift/ {print $1}' | sort -u |
        while read -r dep; do
            install_name_tool -change "$dep" "/usr/lib/swift/$(basename "$dep")" "$f"
        done
done
file "$dest/usr/lib/swift/"libswiftDispatch.dylib "$dest/usr/lib/swift/"libswiftDarwin.dylib "$dest/usr/lib/swift/"libswiftObjectiveC.dylib
echo "Swift overlay dylibs installed at $dest"

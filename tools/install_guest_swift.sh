#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
repo=$PWD
version=6.4.0
url=https://download.swift.org/swift-$version-release/xcode/swift-$version-RELEASE/swift-$version-RELEASE-osx.pkg
sha256=8fd03185b98fe27f54a54631c2449decf75d5b466ce8e34abbd414141063c6aa
signer="Developer ID Installer: Swift Open Source (V9AUD2URP3)"
component=swift-$version-RELEASE-osx-package.pkg
libs="libswiftCore libswift_Concurrency libswift_StringProcessing libswift_RegexParser libswiftRegexBuilder
      libswift_Differentiation libswiftDistributed libswiftObservation libswiftSynchronization libswift_Volatile
      libswift_Builtin_float libswiftSwiftOnoneSupport libswiftCompatibilitySpan"
work=${OCERZ_SWIFT_BUILD:-"$repo/tools/swift/build"}
pkg=${OCERZ_SWIFT_PKG:-"$work/swift-$version-RELEASE-osx.pkg"}
dest=${1:-"$repo/runtime/guest"}
mkdir -p "$work" "$dest/usr/lib/swift"
work=$(cd "$work" && pwd -P)
dest=$(cd "$dest" && pwd -P)
if [ ! -f "$pkg" ]; then
    curl -fL --retry 3 -o "$pkg.part" "$url"
    mv "$pkg.part" "$pkg"
fi
got=$(shasum -a 256 "$pkg" | awk '{print $1}')
if [ "$got" != "$sha256" ]; then
    echo "$pkg has sha256 $got, expected $sha256" >&2
    exit 2
fi
if ! pkgutil --check-signature "$pkg" | grep -Fq "$signer"; then
    echo "$pkg is not signed by $signer" >&2
    exit 2
fi
rm -rf "$work/payload" "$work/root"
mkdir -p "$work/payload" "$work/root"
(cd "$work/payload" && xar -xf "$pkg" "$component/Payload")
members=(./usr/share/swift/LICENSE.txt)
for lib in $libs; do
    members+=("./usr/lib/swift/macosx/$lib.dylib")
done
tar -xf "$work/payload/$component/Payload" -C "$work/root" "${members[@]}"
for lib in $libs; do
    lipo -thin x86_64 "$work/root/usr/lib/swift/macosx/$lib.dylib" -output "$dest/usr/lib/swift/$lib.dylib"
done
cp "$work/root/usr/share/swift/LICENSE.txt" "$dest/LICENSE.swift.txt"
rm -rf "$work/payload" "$work/root"
file "$dest/usr/lib/swift/"*.dylib
echo "Swift $version guest runtime installed at $dest"

#!/bin/bash
# fetch_guest_gnu_cxx.sh -- the x86_64 GNU libstdc++ native mode (-native) loads for programs built with GCC, or with
# Apple's compilers before libc++, which import it from /usr/lib/libstdc++.6.dylib: macOS on Apple silicon has no such
# library, where Rosetta's x86_64 shared cache still does (VLC 2.2's matroska, DASH, live555, UPnP and taglib plugins
# import it).  Source: the x86_64 slice of the MacPorts package tools/fetch_guest32_cxx.sh takes the i386 one from,
# GCC 15.2's runtime for darwin_10 (GPLv3 with the GCC Runtime Library Exception), and GNU libiconv 1.19 for
# darwin_10 (LGPL), whose libiconv* functions that libstdc++ imports and Apple's libiconv does not export; both pinned
# by SHA-256.  They go to runtime/guest-gnu, which native mode searches after runtime/guest (src/dyld.c,
# native_guest_path); the directory is not tracked and no release carries it.
set -euo pipefail
cd "$(dirname "$0")/.."
G=runtime/guest-gnu/usr/lib
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fetch() {   # fetch <port> <file> <sha256>
  curl -sfL -o "$T/$2" "https://packages.macports.org/$1/$2"
  echo "$3  $T/$2" | shasum -a 256 -c -
  mkdir -p "$T/$1"
  tar -xjf "$T/$2" -C "$T/$1"
}
fetch libgcc15 libgcc15-15.2.0_0+stdlib_flag+universal.darwin_10.i386-x86_64.tbz2 \
  d23b0099c24e4452c848ed2d39d0b3c50d6b23badfdc4d2bd60fa0c84ee2b6d5
fetch libiconv libiconv-1.19_0.darwin_10.x86_64.tbz2 \
  fc0607368701ef2e9aab2aac152179bd4db5aba8e17b8a093493b2cf2c3321ca
mkdir -p $G
for l in libstdc++.6.dylib libgcc_s.1.1.dylib; do
  lipo "$(find "$T/libgcc15/opt/local/lib" -name "$l" -type f | head -1)" -thin x86_64 -output "$G/$l"
done
# GNU libiconv keeps a name of its own, so a program's imports of Apple's iconv from /usr/lib/libiconv.2.dylib
# still reach the native one
cp "$T/libiconv/opt/local/lib/libiconv.2.dylib" "$G/libgnuiconv.2.dylib"
# install names, and their references to each other, are MacPorts' prefix: make them the /usr/lib names native mode
# looks up in a guest root
for l in libstdc++.6.dylib libgcc_s.1.1.dylib libgnuiconv.2.dylib; do
  chmod u+w "$G/$l"
  install_name_tool -id "/usr/lib/$l" "$G/$l" 2>/dev/null
  for dep in $(otool -L "$G/$l" | awk 'NR>1 {print $1}' | grep '^/opt/local'); do
    case $(basename "$dep") in
      libiconv.2.dylib) to=/usr/lib/libgnuiconv.2.dylib ;;
      *) to="/usr/lib/$(basename "$dep")" ;;
    esac
    install_name_tool -change "$dep" "$to" "$G/$l" 2>/dev/null
  done
done
otool -L $G/*.dylib

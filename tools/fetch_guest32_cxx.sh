#!/bin/bash
# fetch_guest32_cxx.sh [root] -- the guest i386 GNU libstdc++ and libgcc_s that m32 programs built with GCC import from
# /usr/lib, which macOS no longer ships (Batman: Arkham Asylum takes the old-ABI std::string, iostreams and
# _Rb_tree_* from libstdc++.6.dylib).  Source: MacPorts' universal build of GCC 15.2's runtime for darwin_10, pinned
# by SHA-256 (GPLv3 with the GCC Runtime Library Exception), with GNU libiconv 1.19, which that libstdc++ imports
# (LGPL).  Games built with Apple's clang import LLVM's libc++ instead (Call of Duty: Modern Warfare 3's std::__1
# strings): MacPorts' universal libc++ 5.0.1 and libc++abi for darwin_10, also pinned (MIT and UIUC licenses).
#
# Nothing here needs Xcode's tools, so the installer runs it on any Mac.  The packages are unpacked as MacPorts lays
# them out, under root/opt/local, where m32 looks first for the /opt/local names they give one another, and m32 reads
# the i386 slice of a universal binary itself.  The /usr/lib names programs import are symlinks to them.  root
# defaults to runtime/guest32 beside this script's directory, which is not tracked and which no release carries.
set -euo pipefail
export LC_ALL=C   # shasum is perl, which warns about a locale the Mac names but perl lacks
cd "$(dirname "$0")/.."
ROOT=${1:-runtime/guest32}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/x"
fetch() {   # port file sha256: unpacked into $T/x
    curl -sfL -o "$T/$2" "https://packages.macports.org/$1/$2"
    echo "$3  $T/$2" | shasum -a 256 -c - >/dev/null
    tar -xjf "$T/$2" -C "$T/x" 2>/dev/null
}
fetch libgcc15 libgcc15-15.2.0_0+stdlib_flag+universal.darwin_10.i386-x86_64.tbz2 \
    d23b0099c24e4452c848ed2d39d0b3c50d6b23badfdc4d2bd60fa0c84ee2b6d5
fetch libiconv libiconv-1.19_0+universal.darwin_17.i386-x86_64.tbz2 \
    fb6a003e6fa35434eee23459cc231bab409cb35ab676023fef01b531598e3b09
fetch libcxx libcxx-5.0.1_5+emulated_tls+universal.darwin_10.i386-x86_64.tbz2 \
    2a1fc50fd469ab7aa2e4efe11574e7590fad186265b83bdc2f3e2ee9ae11e243

mkdir -p "$ROOT/opt/local" "$ROOT/usr/lib"
cp -R "$T/x/opt/local/lib" "$ROOT/opt/local/"
for l in libstdc++.6.dylib libgcc_s.1.dylib libgcc_s.1.1.dylib libgcc_ehs.1.1.dylib; do
    rm -f "$ROOT/usr/lib/$l"
    ln -s "../../opt/local/lib/libgcc/$l" "$ROOT/usr/lib/$l"
done
# libc++: the package holds an archive of /usr/lib, its install names already the /usr/lib ones
mkdir -p "$T/libcxx"
tar -xzf "$T/x/opt/local/var/system_roots/libcxx-5.0.1-5.tgz" -C "$T/libcxx" 2>/dev/null
for l in libc++.1.dylib libc++abi.dylib; do
    rm -f "$ROOT/usr/lib/$l"
    cp "$T/libcxx/usr/lib/$l" "$ROOT/usr/lib/$l"
done
echo "i386 C++ runtimes in $ROOT"

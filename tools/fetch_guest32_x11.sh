#!/bin/bash
# fetch_guest32_x11.sh [root] -- the X11 libraries Mac OS X 10.5 and 10.6 shipped in /usr/X11R6/lib, which i386
# games still link and which no macOS since has had in i386 (Warzone 2100 2.3's QuesoGLC loads fontconfig and
# freetype from there).  Source: the i386 slices of MacPorts' universal builds for darwin_17 of fontconfig 2.18.3,
# freetype 2.14.3 and what they need (zlib, bzip2, libpng, brotli, libiconv, expat, gettext's libintl), pinned by
# SHA-256 (MIT, FreeType License, zlib, BSD-style and LGPL licenses).  Their install names and references to one
# another become /usr/X11R6/lib names, which m32 looks for under runtime/guest32 first, and a fonts.conf there names
# the Mac's font folders (m32 points FONTCONFIG_FILE at it).  root defaults to runtime/guest32, which is not tracked.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=${1:-runtime/guest32}
L=$ROOT/usr/X11R6/lib
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fetch() {   # port version sha256
    local f="$1-$2+universal.darwin_17.i386-x86_64.tbz2"
    curl -sfL -o "$T/$f" "https://packages.macports.org/$1/$f"
    echo "$3  $T/$f" | shasum -a 256 -c - >/dev/null
    tar -xjf "$T/$f" -C "$T"
}
fetch fontconfig 2.18.3_0 28fc8e89242018e1b8f88da676d7c64bd59ebd7386db2ef0eaf370557a50e3e1
fetch freetype 2.14.3_0 0c3cf06eba3575b3beb9e7d5f64b2105b5291fbcba5585b5ddc7d3f9c4b8f900
fetch zlib 1.3.2_0 19964f9ef0a2f66fd8fb499c578867b286ca1cdfb51983e3de28c5b0b4fc27f4
fetch bzip2 1.0.8_1 194c2b371782064f98f3b158417ee59b0f72990708ec4034018ac5933b606e27
fetch libpng 1.6.58_0 b434a1bb5da720010ed51859e3c4ccb552444244675adc0ee1030f9b901d9db4
fetch brotli 1.2.0_0 27ecd1156df7e80a0d7555f3f05dfa87857cd901777e702754e6e2f8d5a465e2
fetch libiconv 1.19_0 fb6a003e6fa35434eee23459cc231bab409cb35ab676023fef01b531598e3b09
fetch expat 2.8.4_0 95a1b05761dbd67826bf7c83f60832dc5dea4f45523792e48dcd7420d7445a58
fetch gettext-runtime 1.0_0 b2efd689c0f6111c6f7d69309fbfe990c5a784cb47b3f6edb1886b7d7ec13dd6
mkdir -p "$L" "$ROOT/usr/X11R6/etc/fonts"
for l in libfontconfig.1.dylib libfreetype.6.dylib libz.1.dylib libbz2.1.0.dylib libpng16.16.dylib \
         libbrotlidec.1.dylib libbrotlicommon.1.dylib libiconv.2.dylib libcharset.1.dylib libexpat.1.dylib \
         libintl.8.dylib; do
    src=$(cd "$T/opt/local/lib" && realpath "$l")
    lipo "$src" -thin i386 -output "$L/$l"
    install_name_tool -id "/usr/X11R6/lib/$l" "$L/$l" 2>/dev/null
    for dep in $(otool -L "$L/$l" | awk 'NR>1 {print $1}' | grep '^/opt/local/lib/'); do
        install_name_tool -change "$dep" "/usr/X11R6/lib/$(basename "$dep")" "$L/$l" 2>/dev/null
    done
done
cat > "$ROOT/usr/X11R6/etc/fonts/fonts.conf" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <dir>/System/Library/Fonts</dir>
  <dir>/Library/Fonts</dir>
  <dir prefix="xdg">fonts</dir>
  <cachedir prefix="xdg">fontconfig</cachedir>
</fontconfig>
EOF
echo "i386 X11 libraries in $L"

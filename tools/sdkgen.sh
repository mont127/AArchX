#!/usr/bin/env bash
# sdkgen.sh -- generate a native-mode API database from the macOS SDK and hold
# its coverage against the committed baseline.
#
#   tools/sdkgen.sh libSystem                      # writes runtime/apis/macos/<ver>/libSystem.B.dylib.api
#   tools/sdkgen.sh CoreFoundation /tmp/apis       # writes /tmp/apis/macos/<ver>/CoreFoundation.api
#   tools/sdkgen.sh --update-baseline libSystem    # accept the new coverage as the baseline
#
# The library names are the ones tools/sdkgen/libraries configures: libSystem,
# libncurses, CoreFoundation, libobjc, Foundation, CoreGraphics and AppKit.
# Each takes a second or two, AppKit's twelve thousand exports and the sixteen
# hundred headers its umbrella reaches included.
#
# The generator is tools/sdkgen/sdkgen.c and tbd.c, compiled here against the
# Command Line Tools' libclang every run, because it takes a second and a stale
# binary is a worse failure than a slow one.  The databases are derived from
# the SDK, so they are not committed: `make apis` runs this script for every
# library with --no-baseline, on the machine that builds ocerz and from that
# machine's own SDK, and `make check` depends on it.  The SDK and its version
# come from xcrun, so the database lands in the directory for the SDK it
# describes.
#
#   tools/sdkgen.sh --no-baseline libSystem        # generate without the coverage check
#
# The coverage the generator prints is compared with
# tools/sdkgen/baseline/<leaf>.coverage.  The run fails if the fn or data count
# fell, or if any stub or omit reason counts more exports than the baseline
# says, including a reason the baseline never had: those are the directions in
# which an export stops crossing.  A change in the other direction is printed
# and passes.  --update-baseline copies the new coverage over the old once the
# change has been looked at.  With no baseline at all the run fails and says so.
# Part of the coverage comes from the machine rather than the SDK: an export no
# header declares is sorted by the section the host library keeps it in, and a
# host symbol the running macOS lacks is counted as host-missing, so a baseline
# written on one macOS release can move a few exports between reasons on
# another.
#
# When a database for the same library and SDK already sits under runtime/apis
# and the output is going somewhere else, every fn record in that file that the
# new file does not repeat word for word is listed, so a regeneration shows
# what it would change about the calls that cross today.
set -euo pipefail
cd "$(dirname "$0")/.."

update=0
nobase=0
if [ "${1:-}" = "--update-baseline" ]; then
    update=1
    shift
elif [ "${1:-}" = "--no-baseline" ]; then
    nobase=1
    shift
fi
if [ $# -lt 1 ] || [ $# -gt 2 ]; then
    echo "usage: tools/sdkgen.sh [--update-baseline | --no-baseline] <library> [output-root]" >&2
    exit 2
fi
lib=$1
root=${2:-runtime/apis}

tool=tools/sdkgen
build=$tool/build
libclang=${SDKGEN_LIBCLANG:-/Library/Developer/CommandLineTools/usr/lib/libclang.dylib}
[ -f "$libclang" ] || { echo "sdkgen.sh: no libclang at $libclang" >&2; exit 2; }
sdk=$(xcrun --show-sdk-path)
ver=$(xcrun --show-sdk-version)

install=$(awk -v l="$lib" '$1 == "library" && $2 == l { print $3 }' $tool/libraries)
[ -n "$install" ] || { echo "sdkgen.sh: $tool/libraries has no library $lib" >&2; exit 2; }
leaf=${install##*/}

mkdir -p "$build"
clang -std=c11 -O2 -Wall -Wextra -Werror -o "$build/sdkgen" $tool/sdkgen.c $tool/tbd.c \
    "$libclang" -Wl,-rpath,"$(dirname "$libclang")"

"$build/sdkgen" --sdk "$sdk" --version "$ver" --tooldir "$PWD/$tool" --library "$lib" \
    --out "$root" --build "$build"

new=$build/$leaf.coverage
base=$tool/baseline/$leaf.coverage
if [ $nobase = 1 ]; then
    exit_code=0
elif [ $update = 1 ]; then
    mkdir -p "$(dirname "$base")"
    cp "$new" "$base"
    echo "sdkgen.sh: baseline $base updated"
    exit_code=0
elif [ ! -f "$base" ]; then
    echo "sdkgen.sh: no baseline $base; look at the coverage above and run with --update-baseline" >&2
    exit_code=1
else
    exit_code=0
    awk '
        function key(   k) { k = $1; if ($1 == "stub" || $1 == "omit") k = $1 " " $2; return k }
        function val() { return $NF + 0 }
        NR == FNR { if (NF >= 2) old[key()] = val(); next }
        NF >= 2 {
            k = key(); v = val(); seen[k] = 1
            o = (k in old) ? old[k] : 0
            if (v != o) {
                worse = ((k == "fn" || k == "data") && v < o) || ((k ~ /^(stub|omit) /) && v > o)
                printf "sdkgen.sh: %s %s: %d -> %d\n", worse ? "REGRESSION" : "changed", k, o, v
                if (worse) bad = 1
            }
        }
        END {
            for (k in old) if (!(k in seen) && old[k] != 0) {
                worse = (k == "fn" || k == "data")
                printf "sdkgen.sh: %s %s: %d -> 0\n", worse ? "REGRESSION" : "changed", k, old[k]
                if (worse) bad = 1
            }
            exit bad
        }' "$base" "$new" || exit_code=1
    if [ $exit_code != 0 ]; then
        echo "sdkgen.sh: coverage fell against $base; rerun with --update-baseline if that is intended" >&2
    else
        echo "sdkgen.sh: coverage holds against $base"
    fi
fi

out=$root/macos/$ver/$leaf.api
installed=runtime/apis/macos/$ver/$leaf.api
if [ -f "$installed" ] && [ "$(cd "$(dirname "$out")" && pwd -P)/$leaf.api" != "$(cd "$(dirname "$installed")" && pwd -P)/$leaf.api" ]; then
    awk '
        NR == FNR { if ($1 ~ /^(fn|data|var|special|stub)$/) rec[$2] = $0; next }
        $1 == "fn" && rec[$2] != $0 {
            printf "sdkgen.sh: %s is \"%s\" in %s, generated \"%s\"\n", $2, $0, FILENAME, ($2 in rec) ? rec[$2] : "no record"
        }' "$out" "$installed"
fi
exit $exit_code

#!/bin/bash
# steamgen.sh -- m32's Steam interface database, runtime/apis32/macos/<SDK version>/steam.api (src/m32_steam.c).
#
#   tools/steamgen.sh [output-root]      (make steam32; output-root defaults to runtime/apis32)
#
# The interface classes come from the Steamworks headers of every SDK release, 0.99 to 1.65, as Valve keeps them in
# Proton (lsteamclient/steamworks_sdk_*): fetched once at the pinned commit below into tools/sdkgen/build, a sparse
# checkout of those directories alone (about 50 MB).  They are Valve's, under the Steamworks SDK license, and stay
# out of the tree; the database tools/sdkgen/steamgen.c writes holds only interface, method names and notations.
set -euo pipefail
cd "$(dirname "$0")/.."
PROTON_COMMIT=e91ca2be0df2cef4c230cbbc0b86604d73a0bbf6   # ValveSoftware/Proton proton_10.0, 2026-07-27
root=${1:-runtime/apis32}
build=tools/sdkgen/build
hdrs=$build/proton-sdk
libclang=${SDKGEN_LIBCLANG:-/Library/Developer/CommandLineTools/usr/lib/libclang.dylib}
[ -f "$libclang" ] || { echo "steamgen.sh: no libclang at $libclang" >&2; exit 2; }
sdk=$(xcrun --show-sdk-path)
ver=$(xcrun --show-sdk-version)

if [ "$(git -C "$hdrs" rev-parse HEAD 2>/dev/null)" != "$PROTON_COMMIT" ]; then
    rm -rf "$hdrs"
    mkdir -p "$hdrs"
    git -C "$hdrs" init -q
    git -C "$hdrs" remote add origin https://github.com/ValveSoftware/Proton.git
    git -C "$hdrs" sparse-checkout set --no-cone '/lsteamclient/steamworks_sdk_*/'
    git -C "$hdrs" fetch -q --depth 1 --filter=blob:none origin "$PROTON_COMMIT"
    git -C "$hdrs" checkout -q FETCH_HEAD
fi

mkdir -p "$build" "$root/macos/$ver"
clang -std=c11 -O2 -Wall -Wextra -Werror -o "$build/steamgen" tools/sdkgen/steamgen.c \
    "$libclang" -Wl,-rpath,"$(dirname "$libclang")"
"$build/steamgen" --sdk "$sdk" --out "$root/macos/$ver/steam.api" $(ls -d "$hdrs"/lsteamclient/steamworks_sdk_* | sort -V)

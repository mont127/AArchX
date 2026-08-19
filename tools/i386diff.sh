#!/bin/sh
# i386diff.sh [tree] [extra args for tools/i386diff.py]
#
# The 32-bit decode gate.  Builds the tree's src/decode.o, links the i386diff
# harness against it, sweeps, and compares every probe against capstone
# CS_MODE_32.  Prints a classified summary and a coverage percentage.
#
#   tools/i386diff.sh                     # this tree, full 2^24 + 2^16 sweep
#   tools/i386diff.sh . --quick           # 2-byte openings only, ~2s
#   tools/i386diff.sh . --mode 64         # calibration: ocerz is known-good in
#                                         # 64-bit, so anything this reports is
#                                         # a gap in the harness, not in ocerz
#   tools/i386diff.sh . --min-coverage 95 # use as a pass/fail CI gate
#   tools/i386diff.sh . --by-opcode       # per-opcode coverage table
#
# Companion gate, and the one that must stay green at all times:
#   tools/decodiff-cmp.sh <pristine> <patched>   -> "IDENTICAL"
# i386diff says how much 32-bit works; decodiff says 64-bit did not move.
# A stage-4/5 patch is only acceptable if BOTH are satisfied.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
TREE="${1:-$(dirname "$HERE")}"
[ $# -gt 0 ] && shift
TREE=$(cd "$TREE" && pwd)
W="${TMPDIR:-/tmp}/i386diff.$$"
mkdir -p "$W"
trap 'rm -rf "$W"' EXIT

# ALWAYS rebuild decode.o: a stale object silently turns this into a
# measurement of an older tree, the same failure mode decodiff-cmp.sh guards.
(cd "$TREE" && touch src/decode.c && make -s src/decode.o)

# Which 32-bit entry point does this decode.o actually export?  Stage 3 adds
# one; until then we fall back to the 64-bit entry and report the baseline.
SYMS=$(nm -gU "$TREE/src/decode.o" 2>/dev/null || nm -g "$TREE/src/decode.o")
ENTRY=0
case "$SYMS" in
    *_ocerz_decode_mode*) ENTRY=1 ;;
    *_ocerz_decode32*)    ENTRY=2 ;;
esac

# The harness assumes a specific signature for whichever symbol it found.  If
# stage 3 landed a different one the call would be silently wrong and every
# number below would be garbage, so print both what we assume and whatever the
# header actually declares, and let the eye catch a mismatch immediately.
case "$ENTRY" in
1) echo "i386diff: entry ocerz_decode_mode -- assumed:" \
        "int ocerz_decode_mode(const uint8_t *, size_t, uint64_t, X86Insn *, int mode32)" ;;
2) echo "i386diff: entry ocerz_decode32 -- assumed:" \
        "int ocerz_decode32(const uint8_t *, size_t, uint64_t, X86Insn *)" ;;
*) echo "i386diff: no 32-bit entry point exported by $TREE/src/decode.o;" \
        "measuring the 64-bit decoder against 32-bit input (stage-3 baseline)" ;;
esac
if [ "$ENTRY" != 0 ]; then
    grep -n "ocerz_decode_mode\|ocerz_decode32" \
         "$TREE/include/ocerz/decode.h" 2>/dev/null \
        | sed 's/^/i386diff: header declares: /' || true
fi

clang -arch arm64 -O2 -Wall -Wextra -DI386DIFF_ENTRY=$ENTRY \
      -I"$TREE/include" -o "$W/i386diff" "$HERE/i386diff.c" "$TREE/src/decode.o"

# Two of the python flags also change what the C sweep must produce, so peek at
# them here.  `case` rather than `[ ] &&` so the loop never ends on a false
# test, which under `set -e` is a foot-gun.
QUICK=""
MODE=32
prev=""
for a in "$@"; do
    case "$a" in --quick) QUICK="quick" ;; esac
    case "$prev" in --mode) MODE="$a" ;; esac
    prev="$a"
done

"$W/i386diff" sweep "$MODE" "$W/ocerz.bin" $QUICK
exec python3 "$HERE/i386diff.py" --harness "$W/i386diff" --recs "$W/ocerz.bin" "$@"

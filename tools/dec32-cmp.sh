#!/bin/sh
# The i386 acceptance gate, and the counterpart to decodiff-cmp.sh.
#
#   decodiff-cmp.sh  proves 64-bit decoding did not CHANGE.
#   dec32-cmp.sh     proves 32-bit decoding is RIGHT, by diffing it against
#                    capstone 5.0.7 (python3 -m pip install capstone).
#
# Both are required: the first is blind to whether the new 32-bit answers are
# correct, the second is blind to a 64-bit regression.
#
# With no suite arguments this runs every suite except the two exhaustive 2^24
# sweeps, which take about a minute each; pass "all" for those too. decode.o is
# always rebuilt -- a stale object silently turns this into a test of whatever
# was last compiled.
set -e
T="${1:-.}"
[ $# -gt 0 ] && shift
W="${TMPDIR:-/tmp}/dec32.$$"
mkdir -p "$W"
# Link exactly the objects the Makefile would for a core consumer, so a
# ported decode.rs is measured instead of a stale src/decode.o. Older trees
# without print-core-objs fall back to the glob.
(cd "$T" && make -s ocerz)
OBJS=$(cd "$T" && make -s print-core-objs 2>/dev/null) || \
    OBJS=$(ls "$T"/src/*.o | grep -v '/main\.o$')
OBJS=$(cd "$T" && for o in $OBJS; do echo "$T/$o"; done)
clang -arch arm64 -O2 -I"$T/include" -o "$W/dec32probe" \
    "$(dirname "$0")/dec32probe.c" $OBJS -lcompression
python3 "$(dirname "$0")/dec32-oracle.py" "$W/dec32probe" "$@"
rc=$?
rm -rf "$W"
exit $rc

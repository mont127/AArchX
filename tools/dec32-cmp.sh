#!/bin/sh
# dec32-cmp.sh [tree] [suite ...]
#
# The i386 acceptance gate, and the counterpart to decodiff-cmp.sh.
#
#   decodiff-cmp.sh  proves 64-bit decoding did not CHANGE.
#   dec32-cmp.sh     proves 32-bit decoding is RIGHT, by diffing it against
#                    capstone 5.0.7 (python3 -m pip install capstone).
#
# Both are required: the first is blind to whether the new 32-bit answers are
# correct, the second is blind to a 64-bit regression.
#
# With no suite arguments this runs every suite except the two exhaustive
# 2^24 sweeps, which take about a minute each; pass "all" for those too.
set -e
T="${1:-.}"
[ $# -gt 0 ] && shift
W="${TMPDIR:-/tmp}/dec32.$$"
mkdir -p "$W"
# Always rebuild decode.o: a stale object silently turns this into a test of
# whatever was last compiled, which is the same failure mode decodiff-cmp.sh
# guards against.
(cd "$T" && touch src/decode.c && make -s src/decode.o)
clang -arch arm64 -O2 -I"$T/include" -o "$W/dec32probe" \
    "$(dirname "$0")/dec32probe.c" "$T/src/decode.o"
python3 "$(dirname "$0")/dec32-oracle.py" "$W/dec32probe" "$@"
rc=$?
rm -rf "$W"
exit $rc

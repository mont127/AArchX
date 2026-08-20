#!/usr/bin/env bash
# run_diff32.sh [tree] [extra args for tests/diff32]
#
# The 32-bit differential gate: the i386 counterpart of run_diff_test.sh.
#
# run_diff_test.sh runs whole guest binaries under -no-jit and under the JIT
# and compares them.  It cannot cover 32-bit code, because there is no i386
# Mach-O to load, so this script builds instruction sequences in guest memory
# instead and runs each one under the interpreter and under the JIT from the
# same architectural state, comparing all 16 GPR slots at 64 bits wide, EFLAGS,
# EIP, the mode, the segment selectors, the FP/SSE file and every byte of guest
# memory the sequence is allowed to touch.  See tests/diff32.c for the rules
# that keep a generated sequence from faulting.
#
#   tests/run_diff32.sh                       # the default gate: hand + 20000 random
#   tests/run_diff32.sh . --cases 200000      # a longer soak
#   tests/run_diff32.sh . --seed 0x1234       # a different corpus
#   tests/run_diff32.sh . --only bcd          # one family
#   tests/run_diff32.sh . --selftest          # prove the comparator is not vacuous
#   tests/run_diff32.sh . --jit-required      # what stage 9 should turn on
#
# STAGE 9 HAS LANDED, so this is a real differential: the JIT side compiles
# 32-bit blocks and the interpreter side does not.  --jit-required is passed
# below, so if a future change makes the JIT decline 32-bit blocks again the
# gate FAILS instead of quietly turning into a second interpreter run that
# passes 100%.  The summary prints the block count either way.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
TREE="${1:-$(dirname "$HERE")}"
[ $# -gt 0 ] && shift
TREE=$(cd "$TREE" && pwd)
W="${TMPDIR:-/tmp}/diff32.$$"
mkdir -p "$W"
trap 'rm -rf "$W"' EXIT

# ALWAYS rebuild the objects.  A stale src/*.o silently turns this into a
# measurement of an older tree -- the same failure mode decodiff-cmp.sh guards
# against, and the one that makes a green gate worthless.
(cd "$TREE" && make -s ocerz)

OBJS=$(ls "$TREE"/src/*.o | grep -v '/main\.o$')

# arm64 natively: ocerz IS the x86_64 emulator, so the host build is arm64 and
# nothing here goes anywhere near Rosetta.
clang -arch arm64 -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter \
      -I"$TREE/include" -o "$W/diff32" "$HERE/diff32.c" $OBJS

# The comparator's own evidence first: while both sides interpret, a 100% pass
# rate would also be what a comparator that compares nothing produces.  Skipped
# when the caller asked for the selftest (or a listing) directly, so it does not
# run twice.
case " $* " in
    *" --selftest "*|*" --list "*|*" --help "*) ;;
    *) "$W/diff32" --selftest; echo ;;
esac

# --jit-required: see the header.  It is passed unless the caller already
# named a mode of its own (--selftest / --list / --help / --bug), which do not
# translate a normal corpus.
case " $* " in
    *" --selftest "*|*" --list "*|*" --help "*|*" --bug "*)
        exec "$W/diff32" "$@" ;;
esac
exec "$W/diff32" --jit-required "$@"

#!/bin/bash
# The i386 (m32) gate. Every tests/m32/<name>.expected is the program's stdout plus a final
# "exit <status>" line. Names on the command line run just those programs.
# The programs are committed in tests/m32/bin, as tests/guest/bin's are, because linking i386 needs Xcode 26's
# ld-classic: M32_REBUILD=1 rebuilds them first (tests/m32/build.sh). The C++ programs need the guest GNU libstdc++
# that tools/fetch_guest32_cxx.sh puts in runtime/guest32, and are skipped without it.
set -uo pipefail
cd "$(dirname "$0")/.."
if [ -n "${M32_REBUILD:-}" ] || [ ! -d tests/m32/bin ]; then
  bash tests/m32/build.sh || { echo "FAIL build"; exit 1; }
fi
names=("$@"); [ ${#names[@]} -gt 0 ] || names=($(cd tests/m32 && ls *.expected | sed 's/\.expected$//'))
log=${TMPDIR:-/tmp}
fail=0
for n in "${names[@]}"; do
  if [ -e "tests/m32/$n.cpp" ] && [ ! -e runtime/guest32/usr/lib/libstdc++.6.dylib ]; then
    echo "SKIP $n (no runtime/guest32: tools/fetch_guest32_cxx.sh)"; continue
  fi
  out=$(cd tests/m32/bin && perl -e "alarm 60; exec @ARGV" ../../../ocerz -native "./$n" 2>"$log/m32-$n.err"; echo "exit $?")
  if [ "$out" == "$(cat tests/m32/$n.expected)" ]; then echo "PASS $n"
  else echo "FAIL $n"; diff <(echo "$out") tests/m32/$n.expected | head -20; tail -5 "$log/m32-$n.err"; fail=1; fi
done
exit $fail

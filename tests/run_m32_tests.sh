#!/bin/bash
# The i386 (m32) gate. Every tests/m32/<name>.expected is the program's stdout plus a final
# "exit <status>" line. Names on the command line run just those programs.
# The programs are committed in tests/m32/bin, as tests/guest/bin's are, because linking i386 needs Xcode 26's
# ld-classic: M32_REBUILD=1 rebuilds them first (tests/m32/build.sh). The C++ programs need the guest GNU libstdc++,
# or LLVM libc++ for those whose .link names it, that tools/fetch_guest32_cxx.sh puts in runtime/guest32, and are
# skipped without it; steambridge needs the Steam
# interface database make steam32 writes, and is skipped without it.
set -uo pipefail
cd "$(dirname "$0")/.."
if [ -n "${M32_REBUILD:-}" ] || [ ! -d tests/m32/bin ]; then
  bash tests/m32/build.sh || { echo "FAIL build"; exit 1; }
fi
names=("$@"); [ ${#names[@]} -gt 0 ] || names=($(cd tests/m32 && ls *.expected | sed 's/\.expected$//'))
log=${TMPDIR:-/tmp}
fail=0
for n in "${names[@]}"; do
  if [ -e "tests/m32/$n.cpp" ]; then
    lib=libstdc++.6.dylib
    grep -qs -- -lc++ "tests/m32/$n.link" && lib=libc++.1.dylib
    if [ ! -e "runtime/guest32/usr/lib/$lib" ]; then
      echo "SKIP $n (no runtime/guest32/usr/lib/$lib: tools/fetch_guest32_cxx.sh)"; continue
    fi
  fi
  if [ "$n" = steambridge ] && ! ls runtime/apis32/macos/*/steam.api >/dev/null 2>&1; then
    echo "SKIP $n (no Steam interface database: make steam32)"; continue
  fi
  out=$(cd tests/m32/bin && perl -e "alarm 60; exec @ARGV" ../../../ocerz -native "./$n" 2>"$log/m32-$n.err"; echo "exit $?")
  if [ "$out" == "$(cat tests/m32/$n.expected)" ]; then echo "PASS $n"
  else echo "FAIL $n"; diff <(echo "$out") tests/m32/$n.expected | head -20; tail -5 "$log/m32-$n.err"; fail=1; fi
done
# an i386 program started without -native runs in native mode all the same (src/main.c): its NaN results come from
# translated checks, not FPCR.AH, whose writes cost every call into the host
if [ ${#names[@]} -eq 0 ] || [ $# -eq 0 ]; then
  vlog=$(cd tests/m32/bin && perl -e "alarm 60; exec @ARGV" ../../../ocerz -v ./ret42 2>&1 >/dev/null)
  if [[ "$vlog" == *"NaN results come from translated checks"* ]]; then
    echo "PASS native-by-default"
  else echo "FAIL native-by-default"; fail=1; fi
fi
# A C++ program and no 32-bit runtime (src/m32_dyld.c): it stops before running, with the line that names the fetch
# script, instead of running on imports that all answer 0.  And an ocerz with no runtime/guest32 beside it, as
# AArchX.zip's app is, finds the one the installer put in ~/.aarchx.
if { [ ${#names[@]} -eq 0 ] || [ $# -eq 0 ]; } && [ -e runtime/guest32/usr/lib/libstdc++.6.dylib ]; then
  t=$(mktemp -d "$log/m32-noruntime.XXXXXX")
  mkdir -p "$t/empty" "$t/app/runtime" "$t/home/.aarchx/runtime"
  rout=$(cd tests/m32/bin && OCERZ_GUEST32_ROOT="$t/empty" perl -e "alarm 60; exec @ARGV" ../../../ocerz -native ./cxx 2>&1; echo "exit $?")
  if [[ "$rout" == *"needs /usr/lib/libstdc++.6.dylib, which only AArchX's 32-bit runtime provides"*"fetch_guest32_cxx.sh"*"exit 65" ]]; then
    echo "PASS cxx-no-runtime"
  else echo "FAIL cxx-no-runtime: $(echo "$rout" | tail -3)"; fail=1; fi
  cp ocerz "$t/app/ocerz"
  for d in apis apis32 guest; do ln -s "$PWD/runtime/$d" "$t/app/runtime/$d"; done
  ln -s "$PWD/runtime/guest32" "$t/home/.aarchx/runtime/guest32"
  hout=$(cd tests/m32/bin && HOME="$t/home" perl -e "alarm 60; exec @ARGV" "$t/app/ocerz" -native ./cxx 2>"$log/m32-home.err"; echo "exit $?")
  if [ "$hout" == "$(cat tests/m32/cxx.expected)" ]; then echo "PASS cxx-home-runtime"
  else echo "FAIL cxx-home-runtime"; diff <(echo "$hout") tests/m32/cxx.expected | head -5; tail -3 "$log/m32-home.err"; fail=1; fi
  rm -rf "$t"
fi
exit $fail

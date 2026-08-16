#!/usr/bin/env bash
# Dynamic-mode gate: every xbench kernel, built as an ordinary dynamically linked
# x86_64 binary (tests/guest/benchbin/xbench_dyn), must print the same output
# under Ocerz (OCERZ_HOSTWQ=1) as under Rosetta.  Skipped when the native run
# is impossible (no Rosetta).
set -u
cd "$(dirname "$0")/.."
XB=tests/guest/benchbin/xbench_dyn
if [ ! -x "$XB" ]; then echo "SKIP dyn: $XB missing (make -C tests/guest bench)"; exit 0; fi
if ! "$XB" depchain 1 >/dev/null 2>&1; then echo "SKIP dyn: cannot run $XB natively"; exit 0; fi
pass=0; fail=0
for k in icall jtab depchain brmiss memcpy str hash idiv fpsse fpvec chase qsort leafcall mixed vm; do
    want=$("$XB" "$k" 300 2>/dev/null)
    got=$(OCERZ_HOSTWQ=1 ./ocerz "$XB" "$k" 300 2>/dev/null)
    if [ "$want" = "$got" ]; then pass=$((pass+1)); else echo "FAIL dyn $k: want '$want' got '$got'"; fail=$((fail+1)); fi
done
echo "dynamic-mode xbench: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

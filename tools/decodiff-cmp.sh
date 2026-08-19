#!/bin/sh
# decodiff-cmp.sh <pristine-tree> <patched-tree>
# Builds the harness against each tree's src/decode.o and reports every
# 64-bit decode difference.  Exit 0 iff there are none.
set -e
A="$1"; B="$2"; W="${TMPDIR:-/tmp}/decodiff.$$"
mkdir -p "$W"
# ALWAYS rebuild: a stale decode.o copied around by cp -R silently produces a
# false "IDENTICAL", which is the worst possible failure mode for a gate.
for T in "$A" "$B"; do (cd "$T" && touch src/decode.c && make -s src/decode.o); done
clang -arch arm64 -O2 -I"$A/include" -o "$W/da" "$(dirname "$0")/decodiff.c" "$A/src/decode.o"
clang -arch arm64 -O2 -I"$B/include" -o "$W/db" "$(dirname "$0")/decodiff.c" "$B/src/decode.o"
"$W/da" digest "$W/a.bin"
"$W/db" digest "$W/b.bin"
if cmp -s "$W/a.bin" "$W/b.bin"; then
    echo "decodiff: IDENTICAL -- 16777216/16777216 64-bit decodes match"
    rm -rf "$W"; exit 0
fi
python3 - "$W" "$W/da" "$W/db" <<'PY'
import sys, struct, subprocess, re
w, da, db = sys.argv[1], sys.argv[2], sys.argv[3]
a = open(w+"/a.bin","rb").read(); b = open(w+"/b.bin","rb").read()
n = len(a)//8
diff = [i for i in range(n) if a[i*8:i*8+8] != b[i*8:i*8+8]]
print("decodiff: DIFFERENT -- %d/%d 64-bit decodes changed" % (len(diff), n))
kinds = {}
for i in diff:
    la = subprocess.run([da,"line","%06x"%i],capture_output=True,text=True).stdout
    lb = subprocess.run([db,"line","%06x"%i],capture_output=True,text=True).stdout
    def f(t,k):
        m=re.search(k+r"=(\S+)",t); return m.group(1) if m else "?"
    kinds[(f(la,"op"),f(la,"addrsize"),f(la,"len")+"->"+f(lb,"len"))] = \
        kinds.get((f(la,"op"),f(la,"addrsize"),f(la,"len")+"->"+f(lb,"len")),0)+1
print("  change classes (op, addrsize, len_before->len_after) x count:")
for k,v in sorted(kinds.items(), key=lambda x:-x[1]):
    print("    %-28s x %d" % (str(k), v))
for i in diff[:6]:
    la = subprocess.run([da,"line","%06x"%i],capture_output=True,text=True).stdout.strip()
    lb = subprocess.run([db,"line","%06x"%i],capture_output=True,text=True).stdout.strip()
    print("  - %s" % la); print("  + %s" % lb)
if len(diff) > 6: print("  ... %d more records" % (len(diff)-6))
PY
rm -rf "$W"; exit 1

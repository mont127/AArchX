#!/bin/zsh
# How the meaning of every slot ocerz answers in its dyld API table is known,
# without reading Apple's code.
#
# Each name libdyld exports is taken from the SDK's libdyld.tbd, a public stub
# file.  tools/dyldslots_probe.c calls it under ocerz with every argument zero,
# and OCERZ_DYLDAPI_TRACE=1 prints where the call first arrives in the table,
# with its arguments and caller.  That pairs a public name with a slot by
# running code.  A slot no export reaches first is identified by its caller in
# a traced run, named with dladdr (tools/dyldslots.callers).  Several exports
# reaching the same slot first either are aliases or share a step: every
# introspection export first dlopens Dyld.framework through 0x68, which is how
# 0x68 is also seen to be dlopen.
#
#     tools/dyldslots.sh           print the observed map
#     tools/dyldslots.sh --check   fail unless every slot ocerz handles has a
#                                  witness and tools/dyldslots.pinned holds
set -u
cd "$(dirname "$0")/.."
check=0
[ "${1:-}" = "--check" ] && check=1
tbd="$(xcrun --show-sdk-path 2>/dev/null)/usr/lib/system/libdyld.tbd"
if [ ! -f "$tbd" ]; then
    echo "SKIP dyldslots (no SDK libdyld.tbd)"
    exit 0
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
clang -arch x86_64 -o "$tmp/probe" tools/dyldslots_probe.c || exit 1
clang -arch x86_64 -o "$tmp/who" tools/dyldslots_who.c || exit 1
awk '/symbols:/,0' "$tbd" | tr -d "[]',\n" | tr ' ' '\n' | grep '^_' | sort -u > "$tmp/names"
: > "$tmp/map"
while read -r n; do
    out=$(OCERZ_DYLDAPI_TRACE=1 perl -e 'alarm 10; exec @ARGV' ./ocerz "$tmp/probe" "$n" 2>&1)
    first=$(print -r -- "$out" | awk '/^PROBE call/{on=1; next} on && /ocerz: DYLDAPI/{sub(/^\+/, "", $3); print $3; exit}')
    [ -n "$first" ] && echo "$n $first" >> "$tmp/map"
done < "$tmp/names"
if [ $check -eq 0 ]; then
    sort -k2 "$tmp/map"
    exit 0
fi
fail=0
while read -r n want; do
    case $n in \#*|'') continue ;; esac
    got=$(awk -v n="$n" '$1==n{print $2}' "$tmp/map")
    if [ "$got" != "$want" ]; then
        echo "FAIL dyldslots ($n reached ${got:-nothing}, pinned $want)"; fail=1
    fi
done < tools/dyldslots.pinned
a=$(grep -n '^int ocerz_dyldapi_dispatch' src/dyldapi.c | cut -d: -f1)
handled=$(awk -v a="$a" 'NR>=a' src/dyldapi.c | grep -oE 'case 0x[0-9a-f]+' | awk '{print $2}' | sort -u)
trace=$(OCERZ_DYLDAPI_TRACE=1 ./ocerz "$tmp/probe" __dyld_image_count 2>&1)
for off in ${(f)handled}; do
    awk -v o="$off" '$2==o{found=1} END{exit !found}' "$tmp/map" && continue
    want=$(awk -v o="$off" '$1==o{print $2}' tools/dyldslots.callers)
    if [ -z "$want" ]; then
        echo "FAIL dyldslots (slot $off is answered but nothing witnesses it)"; fail=1; continue
    fi
    callers=$(print -r -- "$trace" | grep "DYLDAPI +$off " | grep -o 'caller=0x[0-9a-f]*' | cut -d= -f2 | sort -u)
    names=$(./ocerz "$tmp/who" ${(f)callers} 2>/dev/null | awk '{print $2}' | sort -u)
    if ! print -r -- "$names" | grep -qx "$want"; then
        echo "FAIL dyldslots (slot $off called from '${names//$'\n'/ }', expected $want)"; fail=1
    fi
done
[ $fail -eq 0 ] && echo "PASS dyldslots ($(wc -l < "$tmp/map" | tr -d ' ') exports mapped, every handled slot witnessed)"
exit $fail

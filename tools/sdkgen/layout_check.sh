#!/usr/bin/env bash
# layout_check.sh -- prove sdkgen's record layouts against clang's own.
#
#   tools/sdkgen.sh libSystem /tmp/apis && tools/sdkgen.sh CoreFoundation /tmp/apis
#   tools/sdkgen/layout_check.sh            # 200 records from every build/*.layouts
#   tools/sdkgen/layout_check.sh 50         # a smaller sample
#
# Every "layout" refusal the generator makes, and every pointer it lets cross,
# rests on sizes, alignments and field offsets it read through libclang's
# clang_Type_getSizeOf, clang_Type_getAlignOf and clang_Cursor_getOffsetOfField.
# Those are the same numbers the compiler uses, but they reach the generator
# through a C API declared by hand and are walked by code
# that could pair the wrong fields, so this script asks the compiler directly.
# Each generator run writes the records it measured to build/<leaf>.layouts,
# one line per architecture, named the way C can name them - struct tag, union
# tag, or the typedef that names an anonymous one - and followed by the
# spelling clang gives the type on that architecture, which is what the dump
# is keyed by.  The two differ for a typedef of a qualified anonymous struct,
# such as OSQueueHead, which clang dumps as a struct unnamed at its line in the
# header, and that line is not the same for the two architectures.  The script pools every
# such file, sorts the names, and takes the requested number of them at even
# intervals through the sorted list, so the sample is the same on every run
# and spread across the alphabet rather than bunched at its start.
#
# For each library it writes one translation unit that includes the umbrella
# header the generator parsed, with the same defines, and a sizeof of every
# sampled type, and compiles it for x86_64 and for arm64 with
# -Xclang -fdump-record-layouts-simple.  The dump gives each record's Size,
# Alignment and FieldOffsets in bits, and all three must equal what the
# generator wrote for that architecture.  A record the dump never mentions is a
# mismatch too.  Every mismatch is printed, and any at all makes the exit
# status non-zero.
set -euo pipefail
cd "$(dirname "$0")/../.."

count=${1:-200}
build=tools/sdkgen/build
sdk=$(xcrun --show-sdk-path)
ver=$(xcrun --show-sdk-version)
shopt -s nullglob
files=("$build"/*.layouts)
[ ${#files[@]} -gt 0 ] || { echo "layout_check.sh: no $build/*.layouts; run tools/sdkgen.sh first" >&2; exit 2; }

work=$(mktemp -d "${TMPDIR:-/tmp}/sdkgen-layout.XXXXXX")
trap 'rm -rf "$work"' EXIT

for f in "${files[@]}"; do
    awk -v file="$f" '$1 == "record" && $2 == "x86_64" {
        name = $0; sub(/^record [^ ]+ [^ ]+ [^ ]+ [^ ]+ /, "", name); sub(/\t.*$/, "", name)
        print file "\t" name
    }' "$f"
done | LC_ALL=C sort -t "$(printf '\t')" -k2,2 -k1,1 | LC_ALL=C sort -s -u -t "$(printf '\t')" -k2,2 > "$work/all"

total=$(wc -l < "$work/all" | tr -d ' ')
awk -v n="$total" -v want="$count" '
    { line[NR] = $0 }
    END {
        if (want >= n) { for (i = 1; i <= n; i++) print line[i]; exit }
        for (i = 0; i < want; i++) print line[int(i * n / want) + 1]
    }' "$work/all" > "$work/sample"
sampled=$(wc -l < "$work/sample" | tr -d ' ')
echo "layout_check.sh: checking $sampled of $total measured record types"

mismatches=0
for f in "${files[@]}"; do
    awk -F '\t' -v file="$f" '$1 == file { print $2 }' "$work/sample" > "$work/names"
    [ -s "$work/names" ] || continue
    header=$(awk '$1 == "header" { sub(/^header /, ""); print; exit }' "$f")
    defines=$(awk '$1 == "define" { printf "%s ", $2 }' "$f")
    probe=$work/probe.c
    printf '#include "%s"\n' "$header" > "$probe"
    awk '{ printf "typedef char sdkgen_probe_%d[sizeof(%s)];\n", NR, $0 }' "$work/names" >> "$probe"
    for arch in x86_64 arm64; do
        clang -target "$arch-apple-macos$ver" -isysroot "$sdk" -std=gnu17 -w $defines -fsyntax-only \
            -Xclang -fdump-record-layouts-simple "$probe" > "$work/dump" 2>&1 || {
            echo "layout_check.sh: clang failed on the probe for $f ($arch):" >&2
            grep -m 20 error "$work/dump" >&2 || true
            exit 1
        }
        awk '
            /^Type: / { name = substr($0, 7); size = ""; align = ""; offs = ""; inoffs = 0; next }
            name != "" && /^ *Size:/ { sub(/^ *Size:/, ""); size = $0; next }
            name != "" && /^ *Alignment:/ { sub(/^ *Alignment:/, ""); align = $0; next }
            name != "" && !inoffs && /FieldOffsets: \[/ { sub(/^.*FieldOffsets: \[/, ""); inoffs = 1 }
            inoffs {
                offs = offs $0
                if (offs ~ /\]>/) {
                    sub(/\]>.*$/, "", offs); gsub(/[ ]/, "", offs); if (offs == "") offs = "-"
                    print name "\t" size "\t" align "\t" offs
                    name = ""; inoffs = 0
                }
            }' "$work/dump" > "$work/clang.$arch"
        awk -v arch="$arch" '$1 == "record" && $2 == arch {
            rest = $0; sub(/^record [^ ]+ [^ ]+ [^ ]+ [^ ]+ /, "", rest)
            split(rest, part, "\t")
            print part[1] "\t" part[2] "\t" $3 "\t" $4 "\t" $5
        }' "$f" > "$work/gen.$arch"
        result=$(awk -F '\t' -v arch="$arch" -v file="$f" '
            FILENAME == ARGV[1] { want[$0] = 1; next }
            FILENAME == ARGV[2] { if (!($1 in clang)) clang[$1] = $2 "\t" $3 "\t" $4; next }
            ($1 in want) { key[$1] = $2; gen[$1] = $3 "\t" $4 "\t" $5 }
            END {
                bad = 0
                for (n in want) {
                    k = key[n]
                    if (!(k in clang)) { printf "MISMATCH %s %s: clang dumped no layout for %s\n", arch, n, k; bad++; continue }
                    if (gen[n] != clang[k]) {
                        split(gen[n], g, "\t"); split(clang[k], c, "\t")
                        printf "MISMATCH %s %s: sdkgen size %s align %s offsets %s, clang size %s align %s offsets %s\n",
                               arch, n, g[1], g[2], g[3], c[1], c[2], c[3]
                        bad++
                    }
                }
                print "COUNT " bad
            }' "$work/names" "$work/clang.$arch" "$work/gen.$arch")
        printf '%s\n' "$result" | grep '^MISMATCH' | LC_ALL=C sort || true
        n=$(printf '%s\n' "$result" | awk '$1 == "COUNT" { print $2 }')
        mismatches=$((mismatches + n))
    done
done

if [ "$mismatches" -ne 0 ]; then
    echo "layout_check.sh: $mismatches mismatch(es)" >&2
    exit 1
fi
echo "layout_check.sh: all $sampled record types agree with clang on x86_64 and arm64"

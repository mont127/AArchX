#!/bin/bash
set -eo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
C_TREE="${C_TREE:-$HOME/AArchX-c}"
RUST_TREE="${RUST_TREE:-$ROOT}"
FIXED_UUID="00112233-4455-6677-8899-aabbccddeeff"
PERF_RUNS="${PERF_RUNS:-3}"
TMP="$(mktemp -d /tmp/tcache_bench.XXXXXX)"
trap 'rc=$?; if [ "$rc" -eq 0 ]; then rm -rf "$TMP"; else echo "benchmark artifacts: $TMP" >&2; fi' EXIT

build_tree() {
    local tree=$1 label=$2
    local rustlib=""
    if [ -f "$tree/rust/target/release/libocerz_rs.a" ]; then
        rustlib="$tree/rust/target/release/libocerz_rs.a"
    fi
    if ! (
        cd "$tree"
        if [ -n "$rustlib" ]; then
            rm -f src/tcache.o src/tcache.d
        fi
        make -s ocerz
    ) >"$TMP/build_$label.log" 2>&1; then
        cat "$TMP/build_$label.log"
        echo "build failed in $tree" >&2
        exit 1
    fi

    local -a objs=()
    local -a link=()
    local obj module
    for obj in "$tree"/src/*.o; do
        [ -f "$obj" ] || continue
        module=${obj##*/}
        module=${module%.o}
        [ "$module" = main ] && continue
        if [ -n "$rustlib" ] && {
            [ -f "$tree/rust/src/ported/$module.rs" ] ||
                [ -f "$tree/rust/src/ported/$module/mod.rs" ]
        }; then
            continue
        fi
        objs+=("$obj")
    done
    [ -z "$rustlib" ] || link+=("$rustlib")
    if ! clang -arch arm64 -std=c11 -O2 -g -I"$tree/include" \
        -o "$TMP/bench_$label" "$ROOT/tools/bench/tcache_bench.c" \
        "${objs[@]}" "${link[@]}" -lcompression -lc -lm \
        >"$TMP/link_$label.log" 2>&1; then
        cat "$TMP/link_$label.log"
        echo "benchmark link failed in $tree" >&2
        exit 1
    fi
    python3 - "$TMP/bench_$label" "$FIXED_UUID" <<'PY'
import struct
import sys
import uuid

path, fixed_uuid = sys.argv[1:]
data = bytearray(open(path, "rb").read())
magic, = struct.unpack_from("<I", data, 0)
if magic != 0xfeedfacf:
    raise SystemExit(f"unexpected Mach-O magic: {magic:#x}")
ncmds, = struct.unpack_from("<I", data, 16)
offset = 32
found = False
for _ in range(ncmds):
    cmd, cmdsize = struct.unpack_from("<II", data, offset)
    if cmdsize < 8 or offset + cmdsize > len(data):
        raise SystemExit("invalid Mach-O load command")
    if cmd == 0x1b:
        if cmdsize < 24:
            raise SystemExit("short LC_UUID")
        data[offset + 8:offset + 24] = uuid.UUID(fixed_uuid).bytes
        found = True
        break
    offset += cmdsize
if not found:
    raise SystemExit("linked benchmark has no LC_UUID")
with open(path, "r+b") as f:
    f.write(data)
print(fixed_uuid)
PY
    codesign -f -s - "$TMP/bench_$label" >/dev/null
    local binary_uuid
    binary_uuid="$(python3 - "$TMP/bench_$label" <<'PY'
import struct
import sys
import uuid

data = open(sys.argv[1], "rb").read()
ncmds, = struct.unpack_from("<I", data, 16)
offset = 32
for _ in range(ncmds):
    cmd, cmdsize = struct.unpack_from("<II", data, offset)
    if cmd == 0x1b:
        print(uuid.UUID(bytes=data[offset + 8:offset + 24]))
        break
    offset += cmdsize
else:
    raise SystemExit("LC_UUID disappeared after signing")
PY
)"
    if [ "$binary_uuid" != "$FIXED_UUID" ]; then
        echo "fixed UUID mismatch: $label has $binary_uuid" >&2
        exit 1
    fi
    printf 'driver_uuid_%s=%s\n' "$label" "$binary_uuid"
}

run_driver() {
    local label=$1 mode=$2 dir=$3 out=$4 err=$5
    if ! OCERZ_TCACHE=on OCERZ_TCACHE_LOG=1 OCERZ_TCACHE_DIR="$dir" \
        "$TMP/bench_$label" "$mode" >"$out" 2>"$err"; then
        cat "$err" >&2
        echo "driver failed: tree=$label mode=$mode dir=$dir" >&2
        return 1
    fi
}

index_path() {
    local root=$1 index
    for index in "$root"/tc-*/index; do
        if [ -f "$index" ]; then
            printf '%s\n' "$index"
            return 0
        fi
    done
    return 1
}

cache_fingerprint() {
    python3 - "$1" <<'PY'
import re
import sys

text = open(sys.argv[1], errors="replace").read()
match = re.search(r" store .*?(tc-[0-9a-fA-F]+) mode=", text)
print(match.group(1) if match else "")
PY
}

record_hash() {
    sed -n 's/^hash=\([0-9a-f]*\).*/\1/p' "$1"
}

build_tree "$C_TREE" c
build_tree "$RUST_TREE" rust

fp_c=""
fp_rust=""
for pair in "c rust" "rust c"; do
    read_tree=${pair%% *}
    write_tree=${pair##* }
    root="$TMP/${write_tree}_to_${read_tree}"
    run_driver "$write_tree" write "$root" "$TMP/write.out" "$TMP/write.err"
    cat "$TMP/write.err"
    run_driver "$read_tree" read "$root" "$TMP/read.out" "$TMP/read.err"
    cat "$TMP/read.err"
    write_fp="$(cache_fingerprint "$TMP/write.err")"
    read_fp="$(cache_fingerprint "$TMP/read.err")"
    if [ -z "$write_fp" ] || [ "$write_fp" != "$read_fp" ]; then
        echo "FORMAT MISMATCH: fingerprint differs for $write_tree -> $read_tree ($write_fp != $read_fp)"
        exit 1
    fi
    if [ "$write_tree" = c ]; then
        fp_c="$write_fp"
    else
        fp_rust="$write_fp"
    fi
    write_hash="$(record_hash "$TMP/write.out")"
    read_hash="$(record_hash "$TMP/read.out")"
    if [ -z "$write_hash" ] || [ "$write_hash" != "$read_hash" ]; then
        echo "FORMAT MISMATCH: $write_tree -> $read_tree ($write_hash != $read_hash)"
        exit 1
    fi
    write_index="$(index_path "$root")"
    write_fp="$(basename "$(dirname "$write_index")")"
    echo "cross_read=$write_tree->$read_tree records=50000 fingerprint=$write_fp hash=$read_hash"
done
if [ -z "$fp_c" ] || [ "$fp_c" != "$fp_rust" ]; then
    echo "FORMAT MISMATCH: C/Rust fingerprints differ ($fp_c != $fp_rust)"
    exit 1
fi
echo "fingerprint_c=$fp_c fingerprint_rust=$fp_rust"

run_driver c write "$TMP/deterministic_c" "$TMP/det_c.out" "$TMP/det_c.err"
run_driver rust write "$TMP/deterministic_rust" "$TMP/det_rust.out" "$TMP/det_rust.err"
c_index="$(index_path "$TMP/deterministic_c")"
rust_index="$(index_path "$TMP/deterministic_rust")"
c_fp="$(basename "$(dirname "$c_index")")"
rust_fp="$(basename "$(dirname "$rust_index")")"
if [ "$c_fp" != "$rust_fp" ]; then
    echo "FORMAT MISMATCH: LC_UUID-free fingerprints differ ($c_fp != $rust_fp)"
    exit 1
fi
if ! cmp -s "$c_index" "$rust_index" ||
    ! cmp -s "$(dirname "$c_index")/d-1.td" "$(dirname "$rust_index")/d-1.td"; then
    echo "FORMAT MISMATCH: deterministic index/data bytes differ"
    echo "Index/data files contain no pid or timestamp fields; compared verbatim."
    cmp -l "$c_index" "$rust_index" | head -20 || true
    cmp -l "$(dirname "$c_index")/d-1.td" "$(dirname "$rust_index")/d-1.td" | head -20 || true
    exit 1
fi
echo "deterministic_fingerprint=$c_fp index=byte-identical data=d-1.td:byte-identical"

for run in $(seq 1 "$PERF_RUNS"); do
    if [ $((run % 2)) -eq 1 ]; then
        trees=(c rust)
    else
        trees=(rust c)
    fi
    for label in "${trees[@]}"; do
        dir="$TMP/perf_${run}_${label}"
        run_driver "$label" write "$dir" "$TMP/seed.out" "$TMP/seed.err"
        run_driver "$label" measure "$dir" "$TMP/perf.out" "$TMP/perf.err"
        printf 'run=%s tree=%s ' "$run" "$label"
        cat "$TMP/perf.out"
    done
done
echo "HASH MATCH"

#!/bin/bash
# Compare the C emitter with the Rust port for output equivalence and throughput.
set -u
cd "$(dirname "$0")/../.."
SELF="$(pwd)"
TREES=${@:-"$HOME/AArchX-c $HOME/AArchX"}

tmp=$(mktemp -d /tmp/a64emit_bench.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

python3 - include/ocerz/a64emit.h "$tmp/a64emit_calls.h" <<'PY'
from pathlib import Path
import re
import sys

header = Path(sys.argv[1]).read_text()
out_path = Path(sys.argv[2])
protos = re.findall(
    r"(?s)\b(void|int|uint32_t\s*\*)\s*(a64_\w+)\s*\((.*?)\)\s*;",
    re.sub(r"/\*.*?\*/", "", header, flags=re.S),
)
if not protos:
    raise SystemExit("no a64emit prototypes found")

registers = {
    "rd", "rn", "rm", "rt", "rt2", "rs", "ra", "vd", "vn", "vm",
    "vt", "dt", "dt2", "qt", "qt2", "ld",
}
flags = {"sf", "dbl", "hi", "two", "is_signed", "scaled", "neg_mul", "neg_add"}
size_values = {"size"}
small_values = {
    "esz", "esize", "from", "mode", "opc", "cond", "nzcv", "bit", "shift",
    "lsl", "sh", "lsb", "width", "idx", "i1", "i2", "imm", "imm5",
}
patch_names = {
    "a64_patch_b", "a64_try_patch_b", "a64_patch_bcond",
    "a64_patch_cbz", "a64_patch_tbz", "a64_try_patch_tbz",
}

def parse_arg(arg, name):
    arg = arg.strip()
    m = re.fullmatch(r"(A64Buf|uint32_t|uint64_t|uint16_t|int32_t|int|unsigned)\s*(\*)?\s*(\w+)", arg)
    if not m:
        raise SystemExit(f"cannot parse argument {arg!r} in {name}")
    return m.groups()

def value_for(ctype, name):
    if name == "b":
        return "b"
    if name in ("at", "target"):
        return "patch_at" if name == "at" else "patch_target"
    if ctype == "uint64_t":
        return "next_u64()"
    if ctype == "uint32_t":
        return "next_u32()"
    if ctype == "uint16_t":
        return "(uint16_t)next_u32()"
    if ctype == "int32_t":
        return "random_branch_offset()" if name == "off_words" else "(int32_t)((int)(next_u32() & 2047u) - 1024)"
    if ctype in ("int", "unsigned"):
        if name in flags:
            return "random_bool()"
        if name in registers:
            return "random_reg()"
        if name == "mode":
            return "random_mode()"
        if name in size_values:
            return "random_size()"
        if name == "off_words":
            return "random_branch_offset()"
        if name in small_values:
            return "random_small()"
        return "random_small()"
    raise SystemExit(f"unsupported argument type {ctype} in {name}")

lines = [
    f"#define A64EMIT_HEADER_PROTOTYPES {len(protos)}",
    f"#define A64EMIT_COVERED_FUNCTIONS {len(protos)}",
    "_Static_assert(A64EMIT_COVERED_FUNCTIONS == A64EMIT_HEADER_PROTOTYPES, \"a64emit coverage must match the header\");",
    "",
    "static void cover_all_a64emit_functions(A64Buf *b, uint32_t *words, uint32_t *patch_words, uint32_t round)",
    "{",
]

for ret, name, arg_text in protos:
    args = [] if arg_text.strip() == "void" else [parse_arg(arg, name) for arg in arg_text.split(",")]
    lines.append("    {")
    lines.extend([
        "        b->p = words;",
        "        b->end = words + 16;",
        "        b->overflow = 0;",
    ])
    if name in patch_names:
        lines.extend([
            "        int32_t patch_delta;",
            f"        uint32_t *patch_at = &patch_words[PATCH_ORIGIN];",
        ])
        if name == "a64_try_patch_b":
            lines.append("        patch_delta = (round & 1u) ? (int32_t)(next_u32() & 0x03ffffffu) - (1 << 25) : (1 << 25);")
        elif name == "a64_try_patch_tbz":
            lines.append("        patch_delta = (round & 1u) ? (int32_t)(next_u32() & 0x3fffu) - 8192 : 8192;")
        else:
            lines.append("        patch_delta = (int32_t)(next_u32() & 0x03ffffffu) - (1 << 25);")
        lines.extend([
            "        uint32_t *patch_target = patch_at + patch_delta;",
            "        *patch_at = next_u32();",
        ])
    call_args = ", ".join(value_for(ctype, name_) for ctype, ptr, name_ in args)
    call = f"{name}({call_args})"
    if ret == "int":
        lines.append(f"        int result = {call};")
        lines.append("        hash_u32((uint32_t)result);")
    elif ret == "uint32_t *":
        lines.append(f"        uint32_t *result = {call};")
        lines.append("        hash_u32(result == &b->sink ? 1u : 2u);")
    else:
        lines.append(f"        {call};")
    if name in patch_names:
        lines.append("        hash_u32(*patch_at);")
    lines.append("        hash_emitted(b, words);")
    lines.append("    }")
lines.extend(["}", ""])
out_path.write_text("\n".join(lines))
PY

i=0
for tree in $TREES; do
    i=$((i + 1))
    name=$(basename "$tree")_$i
    if ! ( cd "$tree" && make -s ocerz ) >"$tmp/build_$name.log" 2>&1; then
        cat "$tmp/build_$name.log"
        echo "build failed in $tree"
        exit 1
    fi
    rustlib=""
    [ -f "$tree/rust/target/release/libocerz_rs.a" ] && rustlib="$tree/rust/target/release/libocerz_rs.a"
    objs=""
    for obj in "$tree"/src/*.o; do
        module=${obj##*/}
        module=${module%.o}
        [ "$module" = main ] && continue
        if [ -n "$rustlib" ] && {
            [ -f "$tree/rust/src/ported/$module.rs" ] ||
                [ -f "$tree/rust/src/ported/$module/mod.rs" ]
        }; then
            continue
        fi
        objs="$objs $obj"
    done
    ( cd "$tree" && clang -arch arm64 -std=c11 -O2 -g -I"$tmp" -Iinclude \
        -o "$tmp/bench_$name" "$SELF/tools/bench/a64emit_bench.c" \
        $objs $rustlib -lcompression -lc -lm ) \
        || { echo "bench build failed in $tree"; exit 1; }
    "$tmp/bench_$name" > "$tmp/out_$name.txt" || exit 1
    sed "s/^/$name: /" "$tmp/out_$name.txt"
done

hashes=$(awk '/^covered=/ {print $2}' "$tmp"/out_*.txt | sed 's/^hash=//' | sort -u)
if [ "$(printf '%s\n' "$hashes" | wc -l | tr -d ' ')" -ne 1 ]; then
    echo "HASH MISMATCH"
    exit 1
fi
echo "HASH MATCH"

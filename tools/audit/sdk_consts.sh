#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${CONSTAUDIT_DIR:-$HOME/constaudit}"
SDK="${SDKROOT:-$(xcrun --show-sdk-path)}"
CLANG="$(xcrun --find clang)"
C_REF="${C_REF:-$HOME/AArchX-c}"

mkdir -p "$OUT/probe_logs" "$OUT/rust_eval/src"

python3 - "$ROOT" "$OUT" "$SDK" "$CLANG" "$C_REF" <<'PY'
import concurrent.futures
import json
import os
import re
import subprocess
import sys
from pathlib import Path

root, out, sdk = (Path(arg) for arg in sys.argv[1:4])
clang = Path(sys.argv[4])
c_ref = Path(sys.argv[5])
root = root.resolve()
out = out.resolve()
sdk = sdk.resolve()
clang = str(clang)
c_ref = c_ref.resolve()


def skip_literal(text, i):
    n = len(text)
    if text.startswith("//", i):
        end = text.find("\n", i + 2)
        return n if end < 0 else end
    if text.startswith("/*", i):
        depth = 1
        j = i + 2
        while j < n and depth:
            if text.startswith("/*", j):
                depth += 1
                j += 2
            elif text.startswith("*/", j):
                depth -= 1
                j += 2
            else:
                j += 1
        return j
    raw = re.match(r'(?:br|r)(#+)?"', text[i:])
    if raw:
        hashes = raw.group(1) or ""
        start = i + raw.end()
        end = text.find('"' + hashes, start)
        return n if end < 0 else end + len(hashes) + 1
    if text[i] == '"':
        j = i + 1
        while j < n:
            if text[j] == "\\":
                j += 2
            elif text[j] == '"':
                return j + 1
            else:
                j += 1
        return n
    if text[i] == "'":
        j = i + 1
        if j < n and text[j] == "\\":
            j += 2
        else:
            j += 1
        if j < n and text[j] == "'":
            return j + 1
    return i


def find_delimiter(text, start, delimiter, angle=False):
    depths = {"(": 0, "[": 0, "{": 0}
    if angle:
        depths["<"] = 0
    pairs = {")": "(", "]": "[", "}": "{"}
    if angle:
        pairs[">"] = "<"
    i = start
    while i < len(text):
        skipped = skip_literal(text, i)
        if skipped != i:
            i = skipped
            continue
        ch = text[i]
        if ch == delimiter and all(v == 0 for v in depths.values()):
            return i
        if ch in depths:
            depths[ch] += 1
        elif ch in pairs and depths[pairs[ch]]:
            depths[pairs[ch]] -= 1
        i += 1
    return -1


def declarations(text, pattern):
    rows = []
    for match in pattern.finditer(text):
        type_start = match.end()
        equal = find_delimiter(text, type_start, "=", angle=True)
        if equal < 0:
            continue
        semi = find_delimiter(text, equal + 1, ";")
        if semi < 0:
            continue
        rows.append({
            "name": match.group(1),
            "type": text[type_start:equal].strip(),
            "expr": text[equal + 1:semi].strip(),
            "start": match.start(),
            "line": text.count("\n", 0, match.start()) + 1,
        })
    return rows


const_re = re.compile(
    r"(?m)^[ \t]*(?:(?:pub(?:\([^)]*\))?[ \t]+)?const)[ \t]+"
    r"([A-Za-z_]\w*)[ \t]*:"
)
type_re = re.compile(
    r"(?m)^[ \t]*(?:(?:pub(?:\([^)]*\))?[ \t]+)?type)[ \t]+"
    r"([A-Za-z_]\w*)\b"
)


def tsv(value):
    return str(value).replace("\\", "\\\\").replace("\t", "\\t").replace("\r", "\\r").replace("\n", "\\n")


files = sorted((root / "rust/src/ported").rglob("*.rs"))
sites = []
by_file = {}
types_by_file = {}
for path in files:
    text = path.read_text()
    rel = path.relative_to(root).as_posix()
    rows = declarations(text, const_re)
    for row in rows:
        row["file"] = rel
        row["abs_file"] = path
        sites.append(row)
    by_file[rel] = (text, rows)
    aliases = declarations(text, type_re)
    types_by_file[rel] = aliases

with (out / "consts.tsv").open("w") as f:
    f.write("NAME\tTYPE\tEXPR\tFILE_LINE\n")
    for row in sites:
        f.write("\t".join(map(tsv, (
            row["name"], row["type"], row["expr"],
            f'{row["file"]}:{row["line"]}',
        ))) + "\n")

header_list = [
    "mach/mach.h", "mach/mach_vm.h", "mach/vm_statistics.h",
    "mach/thread_status.h", "mach/exception_types.h", "mach/task_info.h",
    "mach/thread_act.h", "mach/vm_region.h", "mach/vm_inherit.h",
    "mach/machine.h", "mach-o/loader.h", "mach-o/fat.h", "mach-o/nlist.h",
    "mach-o/reloc.h", "sys/mman.h", "sys/fcntl.h", "fcntl.h", "unistd.h",
    "signal.h", "sys/signal.h", "errno.h", "sys/errno.h", "sys/stat.h",
    "sys/socket.h", "sys/un.h", "netinet/in.h", "sys/ioctl.h", "termios.h",
    "sys/event.h", "sys/sysctl.h", "sys/resource.h", "sys/wait.h",
    "sys/time.h", "sys/ptrace.h", "sys/ipc.h", "sys/shm.h", "sys/sem.h",
    "sys/attr.h", "sys/mount.h", "sys/xattr.h", "sys/proc_info.h",
    "libproc.h", "pthread.h", "compression.h", "dlfcn.h", "spawn.h",
    "sys/syscall.h", "stdint.h", "stddef.h",
]
header = "\n".join(f"#include <{item}>" for item in header_list)
header += "\n#if __has_include(<sys/ulock.h>)\n#include <sys/ulock.h>\n#endif\n"
(out / "probe.h").write_text(header)

names = sorted({row["name"] for row in sites})
probe_results = {}


def probe_name(name, arch):
    src = out / f"probe_{name}.c"
    log = out / "probe_logs" / f"{name}.{arch}.log"
    src.write_text(f'#include "probe.h"\nlong long v = (long long)({name});\n')
    cmd = [
        clang, "-arch", arch, "-isysroot", str(sdk), "-I", str(out),
        "-fsyntax-only", str(src),
    ]
    result = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        log.write_text(result.stdout + result.stderr)
    return name, arch, result.returncode == 0, log if result.returncode else None


with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
    futures = [pool.submit(probe_name, name, arch)
               for name in names for arch in ("arm64", "x86_64")]
    for future in concurrent.futures.as_completed(futures):
        name, arch, ok, log = future.result()
        probe_results.setdefault(name, {})[arch] = {"ok": ok, "log": str(log) if log else ""}

with (out / "probe_status.tsv").open("w") as f:
    f.write("NAME\tARCH\tSTATUS\tLOG\n")
    for name in names:
        for arch in ("arm64", "x86_64"):
            state = probe_results[name][arch]
            f.write(f'{name}\t{arch}\t{"SDK_MIRRORED" if state["ok"] else "NO"}\t{state["log"]}\n')

mirrored = [name for name in names if any(probe_results[name][a]["ok"] for a in ("arm64", "x86_64"))]
arm_names = [name for name in mirrored if probe_results[name]["arm64"]["ok"]]
x86_names = [name for name in mirrored if probe_results[name]["x86_64"]["ok"]]
signedness = (
    "_Generic((x), char: -1, signed char: 1, unsigned char: 0, "
    "short: 1, unsigned short: 0, int: 1, unsigned int: 0, "
    "long: 1, unsigned long: 0, long long: 1, unsigned long long: 0, default: -1)"
)
arm_source = ['#include "probe.h"', "#include <stdio.h>",
              f"#define AUDIT_SIGNED(x) {signedness}",
              "#define AUDIT_PRINT(x) printf(\"%s %lld %llu %zu %d\\n\", #x, "
              "(long long)(x), (unsigned long long)(x), sizeof(x), AUDIT_SIGNED(x))",
              "int main(void) {"]
for name in arm_names:
    arm_source.append(f"  AUDIT_PRINT({name});")
arm_source.append("  return 0;\n}")
(out / "sdk_arm64.c").write_text("\n".join(arm_source) + "\n")

x86_source = ['#include "probe.h"', f"#define AUDIT_SIGNED(x) {signedness}"]
for idx, name in enumerate(x86_names):
    x86_source.extend([
        f"long long audit_s_{idx} = (long long)({name});",
        f"unsigned long long audit_u_{idx} = (unsigned long long)({name});",
        f"unsigned long long audit_size_{idx} = (unsigned long long)sizeof({name});",
        f"long long audit_sign_{idx} = (long long)AUDIT_SIGNED({name});",
    ])
(out / "sdk_x86_64.c").write_text("\n".join(x86_source) + "\n")

arm_exe = out / "sdk_arm64"
cmd = [clang, "-arch", "arm64", "-isysroot", str(sdk), "-I", str(out),
       str(out / "sdk_arm64.c"), "-o", str(arm_exe)]
arm_compile = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
(out / "sdk_arm64.compile.log").write_text(arm_compile.stdout + arm_compile.stderr)
arm_values = {}
if arm_compile.returncode == 0:
    run = subprocess.run([str(arm_exe)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    (out / "sdk_arm64.run.log").write_text(run.stdout + run.stderr)
    if run.returncode == 0:
        for line in run.stdout.splitlines():
            fields = line.split()
            if len(fields) == 5:
                arm_values[fields[0]] = tuple(map(int, fields[1:]))

x86_asm = out / "sdk_x86_64.s"
cmd = [clang, "-arch", "x86_64", "-isysroot", str(sdk), "-I", str(out),
       "-O0", "-fno-common", "-S", str(out / "sdk_x86_64.c"), "-o", str(x86_asm)]
x86_compile = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
(out / "sdk_x86_64.compile.log").write_text(x86_compile.stdout + x86_compile.stderr)
x86_values = {}
if x86_compile.returncode == 0:
    asm = x86_asm.read_text().splitlines()
    symbols = {}
    current = None
    for line in asm:
        m = re.match(r"^_?(audit_(?:s|u|size|sign)_\d+):", line.strip())
        if m:
            current = m.group(1)
            continue
        zero = re.search(r"\.zerofill\s+[^,]+,[^,]+,([^,]+),(\d+)", line)
        if zero:
            symbols[zero.group(1).lstrip("_")] = 0
            current = None
            continue
        if current is None:
            continue
        quad = re.search(r"\.quad\s+(-?(?:0x[0-9a-fA-F]+|\d+))", line)
        if quad:
            symbols[current] = int(quad.group(1), 0)
            current = None
    for idx, name in enumerate(x86_names):
        try:
            signed = symbols[f"audit_s_{idx}"]
            unsigned = symbols[f"audit_u_{idx}"] % (1 << 64)
            size = symbols[f"audit_size_{idx}"]
            sign = symbols[f"audit_sign_{idx}"]
            if unsigned >= (1 << 63):
                signed = unsigned - (1 << 64)
            x86_values[name] = (signed, unsigned, size, sign)
        except KeyError:
            pass

with (out / "sdk_values.tsv").open("w") as f:
    f.write("NAME\tARM64_SIGNED\tARM64_UNSIGNED\tARM64_SIZE\tARM64_SIGNED_TYPE\tX86_64_SIGNED\tX86_64_UNSIGNED\tX86_64_SIZE\tX86_64_SIGNED_TYPE\n")
    for name in mirrored:
        av = arm_values.get(name)
        xv = x86_values.get(name)
        vals = [name]
        vals += list(av) if av else ["MANUAL"] * 4
        vals += list(xv) if xv else ["MANUAL"] * 4
        f.write("\t".join(map(tsv, vals)) + "\n")

# Build a throwaway Rust evaluator. It copies source types and expressions;
# only simple local aliases and const dependencies are pulled into each module.
ffi_candidates = sorted((root / "rust/target").glob("**/out/ffi.rs"),
                        key=lambda p: p.stat().st_mtime, reverse=True)
ffi_path = ffi_candidates[0] if ffi_candidates else None
ffi_text = ffi_path.read_text() if ffi_path else ""
ffi_type_re = re.compile(r"(?m)^[ \t]*pub[ \t]+type[ \t]+([A-Za-z_]\w*)\b")
ffi_const_re = re.compile(r"(?m)^[ \t]*pub[ \t]+const[ \t]+([A-Za-z_]\w*)[ \t]*:")
ffi_types = {x["name"]: x for x in declarations(ffi_text, ffi_type_re)}
ffi_consts = {x["name"]: x for x in declarations(ffi_text, ffi_const_re)}

def identifiers(s):
    return set(re.findall(r"\b[A-Za-z_]\w*\b", s))

def parse_imports(text, crate):
    items = set()
    if crate == "libc":
        pats = [
            r"(?ms)^[ \t]*use[ \t]+libc::\{(.*?)\}[ \t]*;",
            r"(?m)^[ \t]*use[ \t]+libc::([A-Za-z_]\w*)[ \t]*;",
        ]
    else:
        pats = [
            r"(?ms)^[ \t]*use[ \t]+crate::ffi::\{(.*?)\}[ \t]*;",
            r"(?m)^[ \t]*use[ \t]+crate::ffi::([A-Za-z_]\w*)[ \t]*;",
        ]
    for pat in pats:
        for m in re.finditer(pat, text):
            raw = m.group(1)
            for part in raw.split(","):
                part = part.strip()
                if not part or part in ("self", "*"):
                    continue
                part = part.split(" as ")[0].strip()
                name = part.split("::")[-1]
                if re.fullmatch(r"[A-Za-z_]\w*", name):
                    items.add(name)
    return items

def local_aliases(path):
    found = []
    cur = path
    candidates = [path]
    for parent in path.parents:
        if parent.name == "ported":
            break
        mod = parent / "mod.rs"
        if mod.exists():
            candidates.append(mod)
    for candidate in candidates:
        try:
            text = candidate.read_text()
        except OSError:
            continue
        for row in declarations(text, type_re):
            found.append((row, candidate))
    return found

def local_consts_for(path):
    return by_file.get(path.relative_to(root).as_posix(), ("", []))[1]

def alias_closure(path, wanted):
    aliases = local_aliases(path)
    by_name = {}
    for row, alias_path in aliases:
        by_name.setdefault(row["name"], (row, alias_path))
    result = []
    seen = set()
    pending = list(wanted)
    while pending:
        name = pending.pop()
        if name in seen or name not in by_name:
            continue
        seen.add(name)
        row, alias_path = by_name[name]
        result.append((row, alias_path))
        pending.extend(identifiers(row["expr"]) & by_name.keys())
    return result

def const_dependencies(site):
    text, rows = by_file[site["file"]]
    before = [r for r in rows if r["start"] < site["start"] and r["name"] != site["name"]]
    by_name = {}
    for row in before:
        by_name[row["name"]] = row
    deps = []
    seen = {site["name"]}
    pending = list(identifiers(site["expr"]))
    while pending:
        name = pending.pop()
        if name in seen or name not in by_name:
            continue
        seen.add(name)
        row = by_name[name]
        deps.append(row)
        pending.extend(identifiers(row["expr"]))
    return deps

def rust_module(site, idx):
    text, _ = by_file[site["file"]]
    deps = const_dependencies(site)
    names = identifiers(site["type"] + " " + site["expr"])
    for dep in deps:
        names |= identifiers(dep["type"] + " " + dep["expr"])
    libc_imports = parse_imports(text, "libc")
    ffi_imports = parse_imports(text, "ffi")
    aliases = alias_closure(site["abs_file"], names)
    lines = [f"mod site_{idx} {{", "use super::*;", "use libc::*;", "use core::ptr;", "use crate::ffi::*;"]
    for name in sorted(libc_imports & names):
        lines.append(f"use libc::{name};")
    for name in sorted(ffi_imports & names):
        lines.append(f"use crate::ffi::{name};")
    emitted = set()
    for row, alias_path in aliases:
        if row["name"] in emitted:
            continue
        emitted.add(row["name"])
        lines.append(f'type {row["name"]} = {row["expr"]};')
    for dep in deps:
        if dep["name"] not in emitted:
            lines.append(f'const {dep["name"]}: {dep["type"]} = {dep["expr"]};')
            emitted.add(dep["name"])
    lines.append(f'const {site["name"]}: {site["type"]} = {site["expr"]};')
    lines.append(f'pub fn eval() -> (i128, usize, bool) {{ audit_int({site["name"]}) }}')
    lines.append("}")
    return "\n".join(lines)

rust_sites = []
for site in sites:
    if site["name"] in mirrored:
        rust_sites.append(site)

trait = """
trait AuditInt: Copy {
    fn value(self) -> i128;
    fn width() -> usize;
    fn signed() -> bool;
}
macro_rules! signed_int {
    ($($t:ty),*) => {$(
        impl AuditInt for $t {
            fn value(self) -> i128 { self as i128 }
            fn width() -> usize { core::mem::size_of::<$t>() }
            fn signed() -> bool { true }
        }
    )*};
}
macro_rules! unsigned_int {
    ($($t:ty),*) => {$(
        impl AuditInt for $t {
            fn value(self) -> i128 { self as i128 }
            fn width() -> usize { core::mem::size_of::<$t>() }
            fn signed() -> bool { false }
        }
    )*};
}
signed_int!(i8, i16, i32, i64, i128, isize);
unsigned_int!(u8, u16, u32, u64, u128, usize);
fn audit_int<T: AuditInt>(value: T) -> (i128, usize, bool) {
    (value.value(), T::width(), T::signed())
}
"""

needed_ffi_types = set()
for site in rust_sites:
    site_names = identifiers(site["type"] + " " + site["expr"])
    for dep in const_dependencies(site):
        site_names |= identifiers(dep["type"] + " " + dep["expr"])
    source_text = by_file[site["file"]][0]
    needed_ffi_types |= (site_names | parse_imports(source_text, "ffi")) & ffi_types.keys()
pending = list(needed_ffi_types)
while pending:
    name = pending.pop()
    row = ffi_types.get(name)
    if not row:
        continue
    for dep in identifiers(row["expr"]) & ffi_types.keys():
        if dep not in needed_ffi_types:
            needed_ffi_types.add(dep)
            pending.append(dep)

ffi_lines = ["pub mod ffi {", "use libc::*;"]
# Include only bindgen aliases needed by the audited declarations.
for name, row in ffi_types.items():
    if name not in needed_ffi_types:
        continue
    ffi_lines.append(f'pub type {row["name"]} = {row["expr"]};')
ffi_lines.append("}")
ffi_block = "\n".join(ffi_lines)

rust_dir = out / "rust_eval"
(rust_dir / "Cargo.toml").write_text(
    '[package]\nname = "constaudit-rust"\nversion = "0.1.0"\nedition = "2024"\n'
    '[dependencies]\nlibc = "0.2"\n'
)

rust_eval = {}
if rust_sites:
    modules = [rust_module(site, i) for i, site in enumerate(rust_sites)]
    main_lines = ["fn main() {"]
    for i, site in enumerate(rust_sites):
        main_lines.append(
            f'let (v,w,s) = site_{i}::eval(); println!("{i}\\t{{}}\\t{{}}\\t{{}}", v, w, s);'
        )
    main_lines.append("}")
    main_source = (
        "#![allow(dead_code, non_camel_case_types, non_snake_case, unused_imports)]\n"
        "extern crate libc;\n"
        + trait + "\n" + ffi_block + "\n" + "\n".join(modules) + "\n"
        + "\n".join(main_lines) + "\n"
    )
    (rust_dir / "src/main.rs").write_text(main_source)
    env = dict(os.environ, RUSTUP_TOOLCHAIN="nightly-2026-10-08")
    cmd = ["cargo", "run", "--quiet", "--offline", "--manifest-path", str(rust_dir / "Cargo.toml")]
    run = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    (out / "rust_eval.compile.log").write_text(run.stderr)
    if run.returncode == 0:
        for line in run.stdout.splitlines():
            fields = line.split("\t")
            if len(fields) == 4:
                rust_eval[int(fields[0])] = (int(fields[1]), int(fields[2]), fields[3] == "true", "")
    else:
        # Isolate any declarations the batch evaluator could not compile.
        for i, site in enumerate(rust_sites):
            one = (
                "#![allow(dead_code, non_camel_case_types, non_snake_case, unused_imports)]\n"
                "extern crate libc;\n" + trait + "\n" + ffi_block + "\n"
                + modules[i] + "\nfn main(){ let (v,w,s)=site_0::eval(); println!(\"{}\\t{}\\t{}\",v,w,s); }\n"
            )
            one = one.replace(f"site_{i}", "site_0")
            (rust_dir / "src/main.rs").write_text(one)
            single = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
            if single.returncode == 0:
                fields = single.stdout.strip().split("\t")
                if len(fields) == 3:
                    rust_eval[i] = (int(fields[0]), int(fields[1]), fields[2] == "true", "")
            else:
                log = out / "probe_logs" / f'rust_{site["name"]}_{site["line"]}.log'
                log.write_text(single.stderr)
                rust_eval[i] = ("MANUAL", "MANUAL", "MANUAL", str(log))

with (out / "rust_eval.tsv").open("w") as f:
    f.write("SITE_INDEX\tNAME\tFILE_LINE\tRUST_VALUE_I128\tRUST_WIDTH\tRUST_SIGNED\tSTATUS\tLOG\n")
    for i, site in enumerate(rust_sites):
        ev = rust_eval.get(i)
        state = "EVALUATED" if ev and ev[0] != "MANUAL" else "MANUAL"
        vals = ev[:3] if ev else ("MANUAL", "MANUAL", "MANUAL")
        log = ev[3] if ev else "batch evaluator produced no row"
        f.write("\t".join(map(tsv, (i, site["name"], f'{site["file"]}:{site["line"]}', *vals, state, log))) + "\n")

site_indices = {}
for i, site in enumerate(rust_sites):
    site_indices.setdefault((site["name"], site["file"], site["line"]), i)
sdk_by_name = {}
for name in mirrored:
    sdk_by_name[name] = (arm_values.get(name), x86_values.get(name))

with (out / "audit.tsv").open("w") as f:
    f.write("NAME\tFILE_LINE\tRUST_VALUE\tSDK_ARM64_VALUE\tSDK_X86_64_VALUE\tVERDICT\tRUST_TYPE\tSDK_ARM64_TYPE\tSDK_X86_64_TYPE\tNOTES\n")
    for site in sites:
        name = site["name"]
        if name not in mirrored:
            continue
        idx = site_indices.get((name, site["file"], site["line"]))
        ev = rust_eval.get(idx) if idx is not None else None
        av, xv = sdk_by_name[name]
        rust_value = ev[0] if ev else "MANUAL"
        rust_width = ev[1] if ev else "MANUAL"
        rust_signed = ev[2] if ev else "MANUAL"
        avalue = f"{av[0]}/{av[1]}" if av else "MANUAL"
        xvalue = f"{xv[0]}/{xv[1]}" if xv else "MANUAL"
        atype = f'{av[2]} bytes/{"signed" if av[3] == 1 else "unsigned" if av[3] == 0 else "other"}' if av else "MANUAL"
        xtype = f'{xv[2]} bytes/{"signed" if xv[3] == 1 else "unsigned" if xv[3] == 0 else "other"}' if xv else "MANUAL"
        notes = []
        if av and xv and av[0] != xv[0]:
            verdict = "ARCH-DIFF"
            notes.append("choose host/guest semantics manually")
        elif not av or not xv:
            verdict = "MANUAL"
            notes.append("SDK identifier is available on only one architecture; classify as host/guest")
        elif rust_value == "MANUAL":
            verdict = "MANUAL"
            notes.append("Rust const evaluator could not compile this copied declaration")
        else:
            selected = av[0] if av else None
            if rust_value != selected:
                verdict = "MISMATCH"
            else:
                verdict = "MATCH"
                if av and rust_width != av[2]:
                    notes.append(f"type width differs ({rust_width} Rust vs {av[2]} SDK), but value fits")
                if av and isinstance(rust_signed, bool) and av[3] in (0, 1) and int(rust_signed) != av[3]:
                    notes.append("type signedness differs, but numeric value is preserved")
        f.write("\t".join(map(tsv, (
            name, f'{site["file"]}:{site["line"]}', rust_value, avalue, xvalue,
            verdict, site["type"], atype, xtype, "; ".join(notes),
        ))) + "\n")

sdk_prefix = re.compile(
    r"^(?:VM_|KERN_|MACH_|TASK_|THREAD_|PROT_|MAP_|O_|F_|SIG|SA_|E[A-Z]+|"
    r"LC_|MH_|CPU_|FAT_|SYS_|AT_|S_I|SOCK|AF_|IPC_|SHM_|CTL_|KEVENT|EV|"
    r"NOTE_|RLIMIT_|WNOHANG|SEEK_|CLOCK_|COMPRESSION_)"
)
unmatched_names = sorted({
    site["name"] for site in sites
    if sdk_prefix.match(site["name"]) and not any(probe_results[site["name"]][a]["ok"] for a in ("arm64", "x86_64"))
})
manual_review = {
    "CLOCK_UPTIME_RAW_VALUE": "Not an SDK spelling; mirrors CLOCK_UPTIME_RAW (8), used by C at syscall.c:3774 and bridge.c:1515.",
    "CPU_FPCR_AH": "Internal FPCR bit; matches C's CPU_FPCR_AH=0x2ull at cpu.c:43.",
    "EAGER_MAX": "Internal dyld capacity; matches C's EAGER_MAX=4096 at dyld.c:2319.",
    "EAGER_SET": "Internal dyld hash-set capacity; matches C's EAGER_SET=(EAGER_MAX*2) at dyld.c:2322.",
    "EDGE_BODY": "Internal edge-kind enum; matches C's EDGE_BODY=2 at jit.c:853.",
    "EDGE_XBLOCK": "Internal edge-kind enum; matches C's EDGE_XBLOCK=0 at jit.c:853.",
    "EMPTY": "Internal zero-initialized Rust sentinel; no C symbol with this name.",
    "EMPTY_BSD_ENTRY": "Internal all-zero BSD table entry; C's designated static bsd_table at syscall.c:5454 leaves omitted entries zero.",
    "EUNSUP": "Internal alias for OCERZ_EUNSUP, not an SDK errno; C interp_sse.c returns OCERZ_EUNSUP at unsupported cases.",
    "EXPORT_FLAGS_KIND_ABSOLUTE": "Internal export-trie flag; matches C's 0x02 definition at cache.c:116.",
    "EXPORT_FLAGS_KIND_MASK": "Internal export-trie mask; matches C's 0x03 definition at cache.c:115.",
    "EXPORT_FLAGS_REEXPORT": "Internal export-trie flag; matches C's 0x08 definition at cache.c:113.",
    "F_RDADVISEV": "SDK header is absent on this host; C sysbridge.c supplies fallback 116 at lines 281-282.",
    "KERN_INVALID_ARG": "Local abbreviation for KERN_INVALID_ARGUMENT=4; C bridge.c uses KERN_INVALID_ARGUMENT at line 1816 and related validation returns.",
    "MAP_JIT_LOCAL": "Local cfg alias for macOS MAP_JIT (0x800); C jit.c passes MAP_JIT at line 21908.",
    "VM_REGION_SUBMAP_INFO_64": "No SDK or C counterpart; unused Rust declaration in syscall/mach.rs:11.",
    "VM_REGION_SUBMAP_SHORT_INFO_64": "No SDK or C counterpart; unused Rust declaration in syscall/mach.rs:12.",
}
with (out / "unmatched.tsv").open("w") as f:
    f.write("NAME\tSITES\tC_REFERENCE_MATCHES\tHAND_CHECK\n")
    for name in unmatched_names:
        sites_text = ";".join(f'{s["file"]}:{s["line"]}' for s in sites if s["name"] == name)
        result = subprocess.run(
            ["rg", "-n", "--no-heading", "-w", name, str(c_ref / "src")],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        )
        refs = " | ".join(result.stdout.splitlines())
        f.write("\t".join(map(tsv, (
            name, sites_text, refs, manual_review.get(name, "REVIEW REQUIRED"),
        ))) + "\n")

print(f"const sites: {len(sites)}; distinct names: {len(names)}")
print(f"SDK-mirrored names (available in at least one arch): {len(mirrored)}")
print(f"SDK names available on both arches: {len([n for n in mirrored if probe_results[n]['arm64']['ok'] and probe_results[n]['x86_64']['ok']])}")
print(f"SDK-looking names unmatched on both arches: {len(unmatched_names)}")
print(f"artifacts: {out}")
PY

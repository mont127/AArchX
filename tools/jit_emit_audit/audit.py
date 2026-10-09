#!/usr/bin/env python3
"""Build and compare isolated C/Rust JIT emission oracles on arm64 macOS.

The reference supplies the exact same diff32 corpus source to both builds.
Normal objects come from Makefile's CORE_OBJS, not a stale-object glob. Only
the core is instrumented: a temporary C copy, or a separate cfg-enabled Rust
archive. Neither tree's sources nor normal objects are replaced. Sequential
offset/low runs force fresh roundtrip translations and discard ambient OCERZ
tuning knobs. Files are streamed, never loaded wholesale into memory.

Only validated relocation payloads are masked. Ordering, guest addresses,
instruction boundaries, relocation descriptors/arguments and all other code
bits must agree. Disassembly text is diagnostic, not an equality criterion.
Exit 0 means MATCH, 1 means MISMATCH, 2 means an incomplete/invalid oracle run.
"""
from __future__ import annotations

import argparse
import itertools
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass

HERE = Path(__file__).resolve().parent
MAGIC = b"AXJITA01"
HEADER = struct.Struct("<QIII")
RELOC = struct.Struct("<IBBQ")
INSN = struct.Struct("<QBH")
HOOK = """#ifdef OCERZ_JIT_EMIT_AUDIT
    ocerz_jit_emit_audit(rip, entry, blk->code_words, blk->insns, (uint32_t)n,
                         g_tc_rel, (uint32_t)g_tc_nrel);
#endif
"""
DECL = """#include "ocerz/jit_internal.h"
extern void ocerz_jit_emit_audit(uint64_t, const uint32_t *, uint32_t,
                               const X86Insn *, uint32_t,
                               const TcReloc *, uint32_t);
"""


class AuditError(Exception):
    pass


@dataclass
class Block:
    rip: int
    code: bytes
    relocs: tuple
    insns: tuple


def read_exact(stream, size):
    data = stream.read(size)
    if len(data) != size:
        raise AuditError(f"{stream.name}: truncated record")
    return data


def records(path):
    with path.open("rb") as stream:
        if read_exact(stream, len(MAGIC)) != MAGIC:
            raise AuditError(f"{path}: unsupported audit format")
        count = 0
        while True:
            header = stream.read(HEADER.size)
            if not header:
                if not count:
                    raise AuditError(f"{path}: empty audit; is the hook enabled?")
                return
            if len(header) != HEADER.size:
                raise AuditError(f"{path}: truncated header")
            rip, nwords, nrel, ninsns = HEADER.unpack(header)
            if not (0 < nwords <= 4 * 1024 * 1024 and nrel <= 65536
                    and 0 < ninsns <= 65536):
                raise AuditError(f"{path}: invalid block dimensions at {rip:#x}")
            code = read_exact(stream, nwords * 4)
            relocs = tuple(RELOC.unpack(read_exact(stream, RELOC.size))
                           for _ in range(nrel))
            insns = []
            for _ in range(ninsns):
                pc, length, textlen = INSN.unpack(read_exact(stream, INSN.size))
                if not (0 < length <= 15 and textlen < 256):
                    raise AuditError(f"{path}: invalid instruction at {pc:#x}")
                insns.append((pc, length, read_exact(stream, textlen).decode("utf-8")))
            count += 1
            yield Block(rip, code, relocs, tuple(insns))


def normalized(block):
    code = bytearray(block.code)
    occupied = set()
    for off, kind, form, _arg in block.relocs:
        if form not in (0, 1) or not 1 <= kind <= 11:
            raise AuditError(f"{block.rip:#x}: unsupported relocation {kind}/{form}")
        width = 2 if form == 1 else 4
        if (off + width) * 4 > len(code):
            raise AuditError(f"{block.rip:#x}: relocation outside code at word {off}")
        span = set(range(off, off + width))
        if occupied & span:
            raise AuditError(f"{block.rip:#x}: overlapping relocations at word {off}")
        occupied.update(span)
        if form == 1:
            code[off * 4:(off + 2) * 4] = bytes(8)
        else:
            words = struct.unpack_from("<4I", code, off * 4)
            for i, word in enumerate(words):
                shape = 0xD2800000 if i == 0 else 0xF2800000 | (i << 21)
                if word & 0xFFE00000 != shape or word & 31 != words[0] & 31:
                    raise AuditError(f"{block.rip:#x}: malformed MOVZ/MOVK relocation at {off}")
                struct.pack_into("<I", code, (off + i) * 4, word & ~0x1FFFE0)
    return bytes(code)


def compare(reference, candidate, examples, label, limit=3):
    count = mismatches = words = 0
    for index, (left, right) in enumerate(itertools.zip_longest(records(reference), records(candidate))):
        count += 1
        reasons = []
        lcode = normalized(left) if left else b""
        try:
            rcode = normalized(right) if right else b""
        except AuditError as exc:
            reasons.append(f"invalid candidate relocation: {exc}")
            rcode = right.code
        if left is None or right is None:
            reasons.append("missing block on " + ("reference" if left is None else "candidate"))
        else:
            words += len(left.code) // 4
            if left.rip != right.rip:
                reasons.append("guest RIP/order differs")
            if left.relocs != right.relocs:
                reasons.append("relocation metadata differs")
            if tuple(i[:2] for i in left.insns) != tuple(i[:2] for i in right.insns):
                reasons.append("x86 instruction boundaries differ")
            if lcode != rcode:
                common = min(len(lcode), len(rcode))
                offset = next((i for i in range(common) if lcode[i] != rcode[i]), common)
                reasons.append(f"normalized arm64 differs at byte {offset:#x} (word {offset // 4})")
        if reasons:
            mismatches += 1
            if len(examples) < limit:
                examples.append((label, index, reasons, left, right))
    return count, mismatches, words


def diagnostics(examples):
    for layout, index, reasons, left, right in examples:
        print(f"\n{layout} block #{index}: {'; '.join(reasons)}")
        for name, block in (("reference", left), ("candidate", right)):
            if block is None:
                print(f"  {name}: <missing>")
                continue
            print(f"  {name} guest rip={block.rip:#x}, arm64 words={len(block.code) // 4}")
            print("  x86 disassembly:")
            for pc, length, text in block.insns:
                print(f"    {pc:#x} (+{length}): {text}")
            print(f"  relocations (word offset, kind, form, argument): {block.relocs}")
            print("  raw arm64 byte stream (little-endian; relocation addresses are NOT masked here):")
            for offset in range(0, len(block.code), 32):
                print(f"    {offset:06x}: {block.code[offset:offset + 32].hex(' ')}")


def run(command, tree, log, env=None, timeout=600):
    with log.open("a") as out:
        out.write("$ " + shlex.join(map(str, command)) + "\n")
        out.flush()
        try:
            result = subprocess.run(list(map(str, command)), cwd=tree, env=env,
                                    stdout=out, stderr=subprocess.STDOUT, timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            raise AuditError(f"command timed out; see {log}") from exc
    if result.returncode:
        raise AuditError(f"command exited {result.returncode}; see {log}")


def build(tree, work, corpus):
    work.mkdir()
    log = work / "build.log"
    run(["make", "-j8", "ocerz"], tree, log)
    manifest = work / "inputs.mk"
    manifest.write_text(".PHONY: jit_emit_audit_inputs\n"
                        "jit_emit_audit_inputs:\n"
                        "\t@printf '%s\\n' $(CORE_OBJS) $(RUSTLIB) $(LDLIBS) $(RUST_SYSLIBS)\n")
    result = subprocess.run(["make", "-s", "-f", "Makefile", "-f", str(manifest),
                             "jit_emit_audit_inputs"], cwd=tree, capture_output=True, text=True)
    if result.returncode:
        raise AuditError(f"cannot read {tree}'s Makefile inputs: {result.stderr}")
    objects = result.stdout.splitlines()
    flags = ["clang", "-arch", "arm64", "-std=c11", "-O2", "-g", "-Wall", "-Wextra",
             "-Wno-unused-parameter", "-Iinclude"]
    if "src/jit.o" in objects:
        source = (tree / "src/jit.c").read_text()
        if "ocerz_jit_emit_audit(" not in source:
            needle = "    int tc_save = 0;"
            if source.count(needle) != 1:
                raise AuditError(f"{tree}: cannot find the pre-tc_bind audit point in src/jit.c")
            source = source.replace(needle, HOOK + needle)
        instrumented = work / "jit.c"
        instrumented.write_text(DECL + source)
        obj = work / "jit.o"
        run(flags + ["-DOCERZ_JIT_EMIT_AUDIT=1", "-c", instrumented, "-o", obj], tree, log)
        objects[objects.index("src/jit.o")] = str(obj)
    else:
        if not ((tree / "rust/src/ported/jit.rs").exists()
                or (tree / "rust/src/ported/jit/mod.rs").exists()):
            raise AuditError(f"{tree}: neither C nor Rust jit core found")
        target = work / "rust-target"
        run(["cargo", "rustc", "--release", "--lib", "--target-dir", target,
             "--", "--cfg", "ocerz_jit_emit_audit"], tree / "rust", log)
        archives = [i for i, obj in enumerate(objects) if obj.endswith("libocerz_rs.a")]
        if len(archives) != 1:
            raise AuditError(f"{tree}: expected one Rust staticlib in Makefile inputs")
        objects[archives[0]] = str(target / "release/libocerz_rs.a")
    writer = work / "writer.o"
    run(flags + ["-Werror", "-c", HERE / "writer.c", "-o", writer], tree, log)
    binary = work / "diff32"
    run(flags + ["-o", binary, corpus] + objects + [writer], tree, log)
    return binary


def corpus_run(binary, tree, work, layout):
    record = work / f"{layout}.bin"
    log = work / f"{layout}.log"
    env = {k: v for k, v in os.environ.items() if not k.startswith("OCERZ_") and k != "JIT_AUDIT"}
    env.update(OCERZ_TCACHE="roundtrip", OCERZ_NO_ARM_EXEC="1", OCERZ_JIT_EMIT_AUDIT=str(record))
    command = [binary, "--jit-required", "--seed", "0x1", "--cases", "20000"]
    if layout == "low":
        command.append("--low")
    error = None
    try:
        run(command, tree, log, env=env, timeout=600)
    except AuditError as exc:
        error = str(exc)
    if not record.exists():
        raise AuditError(f"no emission records from {tree}; preserve the audit hook; see {log}")
    output = log.read_text(errors="replace")
    if not re.search(r"^differential32: 40044 passed, 0 failed", output, re.M):
        error = error or f"incomplete/failed corpus; see {log}"
    return record, error


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference_tree", type=Path)
    parser.add_argument("candidate_tree", type=Path, nargs="?", default=HERE.parent.parent)
    args = parser.parse_args(argv)
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        parser.error("the emission audit requires arm64 macOS")
    reference, candidate = args.reference_tree.resolve(), args.candidate_tree.resolve()
    for tree in (reference, candidate):
        if not (tree / "include/ocerz/jit_internal.h").is_file():
            parser.error(f"{tree}: expected a split JIT tree (cac4b33 or later)")
    parent = os.environ.get("OCERZ_JIT_AUDIT_DIR")
    try:
        if parent:
            Path(parent).mkdir(parents=True, exist_ok=True)
        work = Path(tempfile.mkdtemp(prefix="jit-emit-audit-", dir=parent))
    except OSError as exc:
        print(f"ERROR: cannot create audit directory: {exc}", file=sys.stderr)
        return 2
    result = 2
    try:
        corpus = work / "diff32.c"
        shutil.copyfile(reference / "tests/diff32.c", corpus)
        binaries = []
        for name, tree in (("reference", reference), ("candidate", candidate)):
            print(f"Building {name}: {tree}", file=sys.stderr)
            binaries.append(build(tree, work / name, corpus))
        examples, errors = [], []
        count = mismatches = words = 0
        for layout in ("offset", "low"):
            paths = []
            for (name, tree), binary in zip((("reference", reference), ("candidate", candidate)), binaries):
                print(f"Running {name} {layout}", file=sys.stderr)
                path, error = corpus_run(binary, tree, work / name, layout)
                paths.append(path)
                if error:
                    errors.append(error)
            n, bad, nw = compare(*paths, examples, layout)
            count += n
            mismatches += bad
            words += nw
        if mismatches:
            print(f"MISMATCH: {mismatches}/{count} blocks differ (offset + low, seed=1)")
            diagnostics(examples)
            result = 1
        elif errors:
            raise AuditError("; ".join(errors))
        else:
            print(f"MATCH: {count} blocks, {words} arm64 words (offset + low, seed=1; relocation payloads masked)")
            result = 0
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
    except (AuditError, OSError, UnicodeError, subprocess.SubprocessError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
    finally:
        sys.stdout.flush()
        if result == 0 and not parent:
            shutil.rmtree(work)
        else:
            print(f"Audit artifacts: {work}", file=sys.stderr)
    return result


if __name__ == "__main__":
    sys.exit(main())

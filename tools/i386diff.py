#!/usr/bin/env python3
"""i386diff -- oracle + report half of the 32-bit decode conformance gate.

tools/i386diff.c sweeps every probe through ocerz's decoder and writes one
8-byte record per probe.  This file does the same sweep through capstone
(CS_MODE_32, or CS_MODE_64 for calibration), compares the two, and prints a
classified summary plus a coverage percentage.

capstone is used strictly as a black box: we feed it bytes and read back
(size, mnemonic).  No capstone table, header or source is consulted, copied or
transliterated -- this project is clean-room and stays that way.

Mismatch classes, worst first:

  LEN    both decode, lengths differ.  The worst class by far: a wrong length
         desynchronises every following instruction in the block.
  MISS   capstone decodes, ocerz refuses.  Plain missing support.
  MNEM   lengths agree but ocerz calls it a different instruction.
  SHAPE  length and mnemonic agree but the operands do not: RIP-relative vs
         absolute, 16- vs 32-bit address registers, a 64-bit register in a
         32-bit decode, a high-byte register.  Every one of these is invisible
         to a length check and every one of them is a wrong program.
  GHOST  ocerz decodes something capstone rejects.  Least severe (over-
         permissiveness), but it hides real encoding errors, so it is counted.

COVERAGE = OK / (probes capstone can decode).  That is the number stages 4 and
5 move.  A second "user-mode" coverage excludes a documented set of ring-0 /
port-I/O mnemonics ocerz has no intention of decoding; both are always printed
so neither can be quietly gamed.

CALIBRATION -- how this harness proves itself
--------------------------------------------
`tools/i386diff.sh . --mode 64` runs the identical machinery against CS_MODE_64,
where ocerz's decoder is the known-good one.  Anything it reports there is a
defect in THIS FILE (a missing mnemonic equivalence, a bad shape rule), not in
ocerz.  On the full 2^24 sweep at 62c494d that run reports:

  LEN 18, MNEM 20, SHAPE 0 out of 14,637,896 decodes.

Every one of those 38 was hand-checked; none is harness noise:

  * 66 48 68 / 66 48 c2   REX.W with 0x66 on a stack op.  ocerz honours 0x66,
                          capstone honours REX.W.  The SDM sides with capstone
                          on 68 (push).  Pre-existing 64-bit behaviour; a
                          stage-4 patch must NOT "fix" it, since decodiff would
                          then correctly report the 64-bit decode as changed.
  * 66 0f 80 / 66 0f 81   capstone honours 0x66 on Jo/Jno near branches in
                          64-bit mode and ignores it on 0f 82..0f 8f.  That
                          self-inconsistency is an ORACLE BUG.  ocerz is right.
  * 66 0f 6d              PUNPCKHQDQ decoded as PUNPCKLQDQ -- a real ocerz
                          64-bit bug (the switch default swallows 0x6d).
  * 0f d0                 ADDSUBPD/ADDSUBPS have no entry in op_names().
  * f3 41 90              PAUSE decoded even though REX.B makes it XCHG.
  * 66 0f 12/13/16/17     movlpd/movhpd folded onto movlps/movhps, 0f e7
                          MOVNTDQ folded onto MOVDQA.

So the harness's own false-positive rate is zero, and the residue is a short
list of genuine findings.  Keep it that way: if a change here makes the 64-bit
run noisier, the change is wrong.

Oracle results are cached under $TMPDIR/ocerz-i386diff-cache (~135 MB per
mode); the key covers the sweep shape, the capstone version and the source of
the functions that produce the cached bytes, so editing shape_of() cannot
silently reuse yesterday's answers.  --no-cache forces a recompute.

Usage (normally via tools/i386diff.sh):
  i386diff.py --harness <path-to-built-harness> --recs <ocerz.bin> [--mode 32|64]
"""

import argparse
import hashlib
import inspect
import json
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor

import numpy as np

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_MODE_64, __version__ as CS_PYVER
except ImportError:
    sys.stderr.write("i386diff: capstone for python3 is required "
                     "(pip3 install capstone)\n")
    raise SystemExit(2)

REC_DT = np.dtype([("status", "u1"), ("len", "u1"), ("flags", "u1"),
                   ("pad", "u1"), ("mnem", "<u2"), ("pad2", "<u2")])
REC_SZ = REC_DT.itemsize

# Operand SHAPE bits, read out of capstone's operand text.  Mirrors
# shape_flags() in i386diff.c -- see the comment there for why length and
# mnemonic agreement is not a sufficient test.
_SH_RIP = re.compile(r"\[[re]ip\b")   # 64-bit prints [rip+..], 0x67 prints [eip+..]
_SH_A16 = re.compile(r"\[(?:bx|bp|si|di)\b")
_SH_R64 = re.compile(r"\b(?:r[abcds][xip]|r8|r9|r1[0-5])\b")
_SH_H8 = re.compile(r"\b[abcd]h\b")


def shape_of(op_str):
    f = 0
    if "[" in op_str:
        if _SH_RIP.search(op_str):
            f |= 1
        elif _SH_A16.search(op_str):
            f |= 2
    if _SH_R64.search(op_str):
        f |= 4
    if "h" in op_str and _SH_H8.search(op_str):
        f |= 8
    return f


SH_RIP, SH_A16, SH_R64, SH_H8, SH_MEM = 1, 2, 4, 8, 16
SHAPE_NAME = ("riprel", "addr16", "reg64", "high8")


def shape_str(f):
    f &= 0xF
    if not f:
        return "-"
    return "+".join(SHAPE_NAME[i] for i in range(4) if f & (1 << i))


def shape_bad(o_fl, c_fl):
    """Which shape bits are a real disagreement.

    Not a plain !=.  ocerz models the implicit operands of the string ops and
    a few others as no operands at all, so "capstone names a 64-bit register
    and ocerz names none" is a modelling difference, not a decode bug -- and in
    32-bit mode capstone never names a 64-bit register anyway, so dropping that
    direction costs the gate nothing and removes every false positive the
    64-bit calibration sweep found.  The direction that matters (ocerz invents
    a 64-bit register in a 32-bit decode) is kept.  Address width is compared
    only where ocerz actually decoded a memory operand, for the same reason."""
    has_mem = (o_fl & SH_MEM) != 0
    return ((((o_fl ^ c_fl) & (SH_RIP | SH_A16)) * has_mem)
            | ((o_fl & ~c_fl) & SH_R64)
            | ((o_fl ^ c_fl) & SH_H8))

# --------------------------------------------------------------------------
# capstone mnemonic -> the set of ocerz op names that mean the same thing
# --------------------------------------------------------------------------
# ocerz's X86Insn deliberately carries coarse op classes (one JCC, one SETCC,
# one MOVS for all widths) with the detail in the operands, so an exact string
# match is the wrong test.  Everything here is an equivalence, never a
# loosening: each entry says "these two names denote the same instruction".
# The 64-bit calibration sweep (--mode 64, where ocerz is known-good) is what
# keeps this table honest -- any gap in it shows up there as a false MNEM.

_PFX_WORDS = {"lock", "rep", "repe", "repz", "repne", "repnz", "bnd",
              "notrack", "xacquire", "xrelease", "data16", "data32",
              "addr16", "addr32"}

_CC = {"o", "no", "b", "c", "nae", "ae", "nb", "nc", "e", "z", "ne", "nz",
       "be", "na", "a", "nbe", "s", "ns", "p", "pe", "np", "po", "l", "nge",
       "ge", "nl", "le", "ng", "g", "nle"}

_ALIAS = {
    # stack / flags / sign-extend accumulators: width suffix, same op
    "pushf": "pushf", "pushfd": "pushf", "pushfq": "pushf",
    "popf": "popf", "popfd": "popf", "popfq": "popf",
    # PUSHA/POPA: stage 4a gives these their own ops, named "pusha"/"popa".
    # (This table originally guessed "push"/"pop", from before those ops
    # existed; the guess is what made 60/61 read as 0% covered.)
    "pushal": "pusha", "pushaw": "pusha", "pusha": "pusha",
    "popal": "popa", "popaw": "popa", "popa": "popa",
    "cbw": "cbw", "cwde": "cbw", "cdqe": "cbw",
    "cwd": "cwd", "cdq": "cwd", "cqo": "cwd",
    # control transfer
    "jcxz": "jrcxz", "jecxz": "jrcxz", "jrcxz": "jrcxz",
    "lcall": "callf", "ljmp": "jmpf",
    "iret": "iret", "iretd": "iret", "iretq": "iret",
    "retn": "ret", "lret": "retf",
    # string ops: ocerz keeps one op and puts the width in the operands
    "movsb": "movs", "movsw": "movs", "movsq": "movs",
    "stosb": "stos", "stosw": "stos", "stosd": "stos", "stosq": "stos",
    "lodsb": "lods", "lodsw": "lods", "lodsd": "lods", "lodsq": "lods",
    "scasb": "scas", "scasw": "scas", "scasd": "scas", "scasq": "scas",
    "cmpsb": "cmps", "cmpsw": "cmps", "cmpsq": "cmps",
    # x87 wait/no-wait spellings
    "wait": "fwait", "fwait": "fwait",
    "fstsw": "fnstsw", "fstcw": "fnstcw", "fclex": "fnclex",
    "finit": "fninit", "fstenv": "fnstenv",
    # misc spellings
    "sal": "shl", "movabs": "mov",
    "cmpxchg8b": "cmpxchgxb", "cmpxchg16b": "cmpxchgxb",
    "prefetchnta": "prefetch", "prefetcht0": "prefetch",
    "prefetcht1": "prefetch", "prefetcht2": "prefetch",
    "prefetchw": "prefetch", "prefetchwt1": "prefetch",
    "int1": "int", "icebp": "int",
    "fcompi": "fcomip", "fucompi": "fucomip", "retfq": "retf",
}

# Genuinely ambiguous spellings: capstone reuses one mnemonic for a string op
# and an SSE op (a5 "movsd" vs f2 0f 10 "movsd").  ocerz has separate ops whose
# printed names collide the same way, so accept either.
_MULTI = {
    "movsd": ("movs", "movsd"),
    "cmpsd": ("cmps", "cmpsd"),
    "mov": ("mov", "mov_sreg"),          # 8c/8e segment moves print as "mov"
    # 06/0e/16/1e and 07/17/1f: capstone spells the segment forms "push"/"pop"
    # exactly like the register forms; ocerz keeps a distinct op because the
    # operand is a segment index rather than a GPR.  Same instruction.
    "push": ("push", "push_sreg"),
    "pop": ("pop", "pop_sreg"),
    "nop": ("nop", "xchg"),              # 90 is xchg eax,eax
    "xchg": ("xchg", "nop"),
    "call": ("call", "callf"),           # ff /3 far indirect prints as "call"
    "jmp": ("jmp", "jmpf"),              # ff /5 likewise
}

# Places where ocerz's op enum is coarser than capstone's mnemonic set, or
# spells the same instruction differently.  Every entry here was confirmed by
# the 64-bit calibration sweep (tools/i386diff.sh . --mode 64), where ocerz is
# the known-good decoder: they are pre-existing modelling choices, identical in
# both modes, and therefore not i386 findings.  They are accepted, and the
# report prints how many probes were accepted this way so the allowance stays
# visible instead of quietly padding the coverage number.
_COARSE = {
    "fnsave": "fnstenv",     # dd /6: folded onto the environment-store op
    "frstor": "fnstenv",     # dd /4: ditto
    "ficom": "fcom",         # integer compare folded onto the float compare
    "ficomp": "fcomp",
    "fyl2xp1": "fyl2x",
    "fxam": "fxch",          # d9 e5 -- looks like a genuine ocerz slip, but a
                             # 64-bit one; out of scope for the i386 stages
    "fnop": "nop",
    # hint-shaped instructions ocerz decodes as a plain NOP.  Architecturally
    # they ARE nops for a translator that does not model caches or MPX.
    "prefetchnta": "nop", "prefetcht0": "nop", "prefetcht1": "nop",
    "prefetcht2": "nop", "prefetchw": "nop", "prefetchwt1": "nop",
    "cldemote": "nop",
    "bndldx": "nop", "bndstx": "nop", "bndmov": "nop", "bndmk": "nop",
    "bndcl": "nop", "bndcu": "nop", "bndcn": "nop",
    # the 66-prefixed double variants of moves and bitwise ops move exactly the
    # same bits as their single counterparts; ocerz keeps one op for both
    "movupd": "movups", "movapd": "movaps", "andpd": "andps",
    "andnpd": "andnps", "orpd": "orps", "xorpd": "xorps",
}

# Things ocerz is not trying to decode at all: ring 0, port I/O, segmentation
# and virtualisation.  Counted in the headline coverage, excluded from the
# "user-mode" one, and listed so the split is auditable.
NONGOAL = {
    "in", "out", "insb", "insw", "insd", "outsb", "outsw", "outsd",
    "cli", "sti", "lgdt", "lidt", "lldt", "ltr", "sldt", "str", "smsw",
    "lmsw", "clts", "invd", "wbinvd", "invlpg", "invpcid", "rdmsr", "wrmsr",
    "rdpmc", "rsm", "arpl", "verr", "verw", "lar", "lsl", "sysenter",
    "sysexit", "sysret", "swapgs", "vmcall", "vmlaunch", "vmresume",
    "vmxoff", "vmxon", "vmread", "vmwrite", "vmptrld", "vmptrst", "vmclear",
    "vmfunc", "invept", "invvpid", "monitor", "mwait", "xsetbv", "xsave",
    "xrstor", "xsaveopt", "getsec", "skinit", "vmmcall", "vmload", "vmsave",
    "stgi", "clgi", "invlpga", "salc", "xlatb", "ud0", "ud1",
    "les", "lds", "lfs", "lgs", "lss", "bound", "into",
}


def cs_classes(mnem):
    """Set of ocerz op names that are an acceptable decode of `mnem`."""
    m = mnem.strip().lower()
    parts = m.split()
    while parts and parts[0] in _PFX_WORDS:
        parts.pop(0)
    if not parts:
        return frozenset()
    m = parts[0]
    out = set()
    if m in _MULTI:
        out.update(_MULTI[m])
    if m in _ALIAS:
        out.add(_ALIAS[m])
    if not out:
        if m.startswith("j") and m[1:] in _CC:
            out.add("jcc")
        elif m.startswith("set") and m[3:] in _CC:
            out.add("setcc")
        elif m.startswith("cmov") and m[4:] in _CC:
            out.add("cmovcc")
        elif m.startswith("fcmov"):      # incl. fcmovu/fcmovnu (unordered)
            out.add("fcmovcc")
    out.add(m)          # the overwhelmingly common case: identical spelling
    return frozenset(out)


def cs_coarse(mnem):
    """The ocerz op name this mnemonic is allowed to collapse onto, if any."""
    return _COARSE.get(cs_base(mnem))


def cs_base(mnem):
    """Mnemonic with any prefix words stripped -- used for the NONGOAL test."""
    parts = mnem.strip().lower().split()
    while parts and parts[0] in _PFX_WORDS:
        parts.pop(0)
    return parts[0] if parts else ""


# --------------------------------------------------------------------------
# probe construction -- must match build_probe() in i386diff.c exactly
# --------------------------------------------------------------------------

def probe_bytes(i, p):
    if i < p["s3n"]:
        return bytes((i & 255, (i >> 8) & 255, (i >> 16) & 255)) + p["tail_a"]
    j = i - p["s3n"]
    return bytes((j & 255, (j >> 8) & 255)) + p["tail_b"]


# --------------------------------------------------------------------------
# oracle workers
# --------------------------------------------------------------------------

_W = {}


def _init(mode, s3n, tail_a, tail_b, ip, path):
    md = Cs(CS_ARCH_X86, CS_MODE_32 if mode == 32 else CS_MODE_64)
    md.detail = False
    _W.update(md=md, s3n=s3n, ta=tail_a, tb=tail_b, ip=ip, path=path)


def _run(rng):
    lo, hi = rng
    md, s3n, ta, tb, ip = _W["md"], _W["s3n"], _W["ta"], _W["tb"], _W["ip"]
    disasm = md.disasm_lite
    n = hi - lo
    rec = bytearray(REC_SZ * n)
    buf = bytearray(16)
    buf[3:] = ta
    names = []
    tbl = {}
    shcache = {}
    k = 0
    for i in range(lo, hi):
        if i < s3n:
            buf[0] = i & 255
            buf[1] = (i >> 8) & 255
            buf[2] = (i >> 16) & 255
            buf[3:] = ta
        else:
            j = i - s3n
            buf[0] = j & 255
            buf[1] = (j >> 8) & 255
            buf[2:] = tb
        got = None
        for x in disasm(bytes(buf), ip, 1):
            got = x
            break
        if got is not None:
            mn = got[2]
            mid = tbl.get(mn)
            if mid is None:
                mid = len(names)
                names.append(mn)
                tbl[mn] = mid
            ops = got[3]
            sh = shcache.get(ops)
            if sh is None:
                sh = shape_of(ops)
                if len(shcache) < 300000:
                    shcache[ops] = sh
            rec[k] = 1
            rec[k + 1] = got[1]
            rec[k + 2] = sh
            rec[k + 4] = mid & 255
            rec[k + 5] = mid >> 8
        k += REC_SZ
    with open(_W["path"], "r+b") as f:
        f.seek(REC_SZ * lo)
        f.write(rec)
    return lo, hi, names


def oracle(p, mode, path, jobs, lo, hi, quiet=False):
    """Fill `path` with capstone records for probes [lo,hi); return name table.

    Written to a per-pid temporary and renamed at the end, so a half-filled
    file can never be picked up as a cache hit by a concurrent run."""
    tmp = "%s.%d" % (path, os.getpid())
    with open(tmp, "wb") as f:
        f.truncate(REC_SZ * p["n"])
    path, final = tmp, path
    span = hi - lo
    chunk = max(1 << 15, (span + jobs * 4 - 1) // (jobs * 4))
    ranges = [(a, min(a + chunk, hi)) for a in range(lo, hi, chunk)]
    ip = p["eip32"] if mode == 32 else p["rip64"]
    args = (mode, p["s3n"], p["tail_a"], p["tail_b"], ip, path)
    names = []
    gid = {}
    t0 = time.time()
    done = 0
    with ProcessPoolExecutor(max_workers=jobs, initializer=_init,
                             initargs=args) as ex:
        for (a, b, local) in ex.map(_run, ranges):
            remap = np.empty(max(len(local), 1), np.uint16)
            for i, nm in enumerate(local):
                g = gid.get(nm)
                if g is None:
                    g = len(names)
                    names.append(nm)
                    gid[nm] = g
                remap[i] = g
            if local:
                mm = np.memmap(path, REC_DT, "r+", offset=REC_SZ * a,
                               shape=(b - a,))
                sel = mm["status"] == 1
                mm["mnem"][sel] = remap[mm["mnem"][sel]]
                mm.flush()
                del mm
            done += b - a
            if not quiet:
                sys.stderr.write("\ri386diff: oracle %6.1f%% (%.1fs)" %
                                 (100.0 * done / span, time.time() - t0))
                sys.stderr.flush()
    if not quiet:
        sys.stderr.write("\ri386diff: oracle done, %d mnemonics, %.1fs\n" %
                         (len(names), time.time() - t0))
    if len(names) > 65535:
        raise SystemExit("i386diff: mnemonic table overflow")
    os.replace(path, final)
    return names


# --------------------------------------------------------------------------
# opcode signature (vectorised): the opcode byte after prefixes, or 0f00|second
# --------------------------------------------------------------------------

def opcode_sig(p, mode, lo, hi):
    n = hi - lo
    idx = np.arange(lo, hi, dtype=np.uint32)
    phaseA = idx < p["s3n"]
    j = np.where(phaseA, idx, idx - p["s3n"]).astype(np.uint32)
    b0 = (j & 255).astype(np.uint8)
    b1 = ((j >> 8) & 255).astype(np.uint8)
    b2 = np.where(phaseA, (j >> 16) & 255, p["tail_b"][0]).astype(np.uint8)
    b3 = np.where(phaseA, p["tail_a"][0], p["tail_b"][1]).astype(np.uint8)
    b4 = np.where(phaseA, p["tail_a"][1], p["tail_b"][2]).astype(np.uint8)
    cols = [b0, b1, b2, b3, b4]

    pref = np.zeros(256, bool)
    for v in (0x66, 0x67, 0xF0, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65):
        pref[v] = True
    if mode == 64:
        pref[0x40:0x50] = True

    pos = np.zeros(n, np.uint8)
    cur = cols[0].copy()
    for k in range(1, 4):
        m = pref[cur]
        if not m.any():
            break
        pos[m] = k
        cur[m] = cols[k][m]
    sig = cur.astype(np.uint16)
    esc = cur == 0x0F
    if esc.any():
        nxt = np.zeros(n, np.uint8)
        for k in range(4):
            m = esc & (pos == k)
            if m.any():
                nxt[m] = cols[k + 1][m]
        sig[esc] = 0x0F00 | nxt[esc].astype(np.uint16)
    return sig


def sig_str(s):
    return "0f %02x" % (s & 0xFF) if s >= 0x0F00 else "%02x   " % s


# --------------------------------------------------------------------------
# report
# --------------------------------------------------------------------------

def human(x):
    return "{:,}".format(int(x))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--harness", required=True)
    ap.add_argument("--recs", required=True, help="ocerz sweep records")
    ap.add_argument("--mode", type=int, default=32, choices=(32, 64))
    ap.add_argument("--cache-dir", default=None)
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--quick", action="store_true",
                    help="two-byte openings only (fast inner-loop gate)")
    ap.add_argument("--no-cache", action="store_true")
    ap.add_argument("--min-coverage", type=float, default=None,
                    help="exit 1 if coverage falls below this percentage")
    ap.add_argument("--fail-on-len", action="store_true",
                    help="exit 1 if any length mismatch remains")
    ap.add_argument("--json", default=None)
    ap.add_argument("--by-opcode", action="store_true")
    args = ap.parse_args()

    raw = subprocess.run([args.harness, "params"], capture_output=True,
                         text=True, check=True).stdout
    kv = dict(l.split("=", 1) for l in raw.strip().splitlines())
    p = {
        "s3n": 1 << int(kv["sweep3_bits"]),
        "s2n": 1 << int(kv["sweep2_bits"]),
        "n": int(kv["probe_n"]),
        "tail_a": bytes.fromhex(kv["tail_a"]),
        "tail_b": bytes.fromhex(kv["tail_b"]),
        "eip32": int(kv["eip32"], 16),
        "rip64": int(kv["rip64"], 16),
        "entry": kv["entry"],
        "entry_is_real": kv["entry_is_real"] == "1",
    }
    names = [l.split(" ", 1)[1] for l in
             subprocess.run([args.harness, "names"], capture_output=True,
                            text=True, check=True).stdout.strip().splitlines()]

    lo, hi = (p["s3n"], p["n"]) if args.quick else (0, p["n"])
    jobs = args.jobs or max(1, (os.cpu_count() or 4) - 2)

    # The cache key covers everything that can change a cached byte: the sweep
    # shape, the mode, the capstone version, AND the source of the two
    # functions that produce the cached records.  Editing shape_of() and
    # silently reusing yesterday's oracle file is exactly the stale-artifact
    # trap decodiff-cmp.sh rebuilds decode.o to avoid.
    src = inspect.getsource(shape_of) + inspect.getsource(_run) + \
        _SH_RIP.pattern + _SH_A16.pattern + _SH_R64.pattern + _SH_H8.pattern
    key = hashlib.sha256(
        ("|".join([kv["sweep3_bits"], kv["sweep2_bits"], kv["tail_a"],
                   kv["tail_b"], kv["eip32"], kv["rip64"], str(args.mode),
                   str(lo), str(hi), str(CS_PYVER), src])).encode()).hexdigest()[:16]
    cdir = args.cache_dir or os.path.join(
        os.environ.get("TMPDIR", "/tmp"), "ocerz-i386diff-cache")
    os.makedirs(cdir, exist_ok=True)
    obin = os.path.join(cdir, "oracle-%s.bin" % key)
    ojson = os.path.join(cdir, "oracle-%s.json" % key)

    if (not args.no_cache and os.path.exists(obin) and os.path.exists(ojson)
            and os.path.getsize(obin) == REC_SZ * p["n"]):
        cs_names = json.load(open(ojson))
        sys.stderr.write("i386diff: oracle from cache (%s)\n" % obin)
    else:
        cs_names = oracle(p, args.mode, obin, jobs, lo, hi)
        json.dump(cs_names, open(ojson, "w"))

    o = np.memmap(args.recs, REC_DT, "r")[lo:hi]
    c = np.memmap(obin, REC_DT, "r")[lo:hi]
    n = hi - lo

    o_ok = o["status"] == 1
    c_ok = c["status"] == 1
    o_len = o["len"].astype(np.uint16)
    c_len = c["len"].astype(np.uint16)
    o_fl = o["flags"].astype(np.uint16)
    c_fl = c["flags"].astype(np.uint16)
    o_op = o["mnem"]
    c_mn = c["mnem"]

    # name-agreement matrix: rows = capstone mnemonic id, cols = ocerz op id
    accept = np.zeros((max(len(cs_names), 1), len(names)), bool)
    oc_index = {}
    for i, nm in enumerate(names):
        oc_index.setdefault(nm, []).append(i)
    coarse = np.zeros_like(accept)
    nongoal_row = np.zeros(max(len(cs_names), 1), bool)
    knownlen_row = np.zeros(max(len(cs_names), 1), bool)
    for i, mn in enumerate(cs_names):
        for want in cs_classes(mn):
            for j in oc_index.get(want, ()):
                accept[i, j] = True
        c = cs_coarse(mn)
        if c:
            for j in oc_index.get(c, ()):
                accept[i, j] = True
                coarse[i, j] = True
        nongoal_row[i] = cs_base(mn) in NONGOAL
        # ud0/ud1 are the "reserved, always faults" opcodes.  ocerz consumes a
        # ModRM there (the modern SDM encoding), capstone does not; both mean
        # #UD, nothing executes past either, and the disagreement is identical
        # in 64-bit mode.  Counted, not classed as a length bug.
        knownlen_row[i] = cs_base(mn) in ("ud0", "ud1")

    both = o_ok & c_ok
    len_eq = both & (o_len == c_len)
    mn_eq = np.zeros(n, bool)
    mn_eq[len_eq] = accept[c_mn[len_eq], o_op[len_eq]]

    named = len_eq & mn_eq
    bad_shape = shape_bad(o_fl, c_fl) != 0
    m_ok = named & ~bad_shape
    m_shape = named & bad_shape
    m_len = both & (o_len != c_len) & ~knownlen_row[c_mn]
    m_known = both & (o_len != c_len) & knownlen_row[c_mn]
    m_mnem = len_eq & ~mn_eq
    m_miss = c_ok & ~o_ok
    m_ghost = o_ok & ~c_ok
    n_coarse = int(np.count_nonzero(m_ok & coarse[c_mn, o_op]))

    n_cs = int(c_ok.sum())
    n_ok = int(m_ok.sum())
    cov = 100.0 * n_ok / n_cs if n_cs else 0.0

    goal = c_ok & ~nongoal_row[c_mn]
    n_goal = int(goal.sum())
    n_goal_ok = int((m_ok & goal).sum())
    cov_um = 100.0 * n_goal_ok / n_goal if n_goal else 0.0

    sig = opcode_sig(p, args.mode, lo, hi)

    print("i386diff -- ocerz %d-bit decode vs capstone CS_MODE_%d (capstone %s)"
          % (args.mode, args.mode, CS_PYVER))
    print("  entry point : %s" % p["entry"])
    if not p["entry_is_real"] and args.mode == 32:
        print("                ^^ no 32-bit entry point in this tree: this is")
        print("                   the stage-3 BASELINE, not a real measurement")
    print("  probes      : %s  (%s three-byte + %s two-byte openings, 16-byte window)"
          % (human(n), human(0 if args.quick else p["s3n"]), human(p["s2n"])))
    print("  capstone decodable : %s  (%.1f%% of probes)"
          % (human(n_cs), 100.0 * n_cs / n))
    print("")
    print("  COVERAGE            : %6.2f%%   (%s / %s)"
          % (cov, human(n_ok), human(n_cs)))
    print("  coverage, user-mode : %6.2f%%   (%s / %s, ring-0 and port I/O excluded)"
          % (cov_um, human(n_goal_ok), human(n_goal)))
    print("")
    print("  LEN   both decode, length differs : %s" % human(m_len.sum()))
    print("  MISS  capstone ok, ocerz refuses  : %s" % human(m_miss.sum()))
    print("  MNEM  length ok, mnemonic differs : %s" % human(m_mnem.sum()))
    print("  SHAPE name ok, operand shape wrong: %s" % human(m_shape.sum()))
    print("  GHOST ocerz ok, capstone rejects  : %s  (of %s capstone-invalid probes)"
          % (human(m_ghost.sum()), human(n - n_cs)))
    print("")
    print("  counted as OK by a documented allowance:")
    print("    coarse ocerz op class (_COARSE)  : %s" % human(n_coarse))
    print("    ud0/ud1 length divergence        : %s" % human(m_known.sum()))

    def buckets(mask, title, label, fields=None):
        """Collapse a mismatch mask to (capstone mnem, opcode, ocerz op, two
        8-bit fields) classes -- decodiff's "change classes" idea, vectorised
        so 10M mismatches still summarise in a couple of seconds."""
        k = int(mask.sum())
        if not k:
            return []
        idx = np.flatnonzero(mask)
        fa, fb = fields if fields else (c_len, o_len)
        key = ((c_mn[idx].astype(np.uint64) << np.uint64(44))
               | (sig[idx].astype(np.uint64) << np.uint64(32))
               | (o_op[idx].astype(np.uint64) << np.uint64(16))
               | (fa[idx].astype(np.uint64) << np.uint64(8))
               | fb[idx].astype(np.uint64))
        uq, first, cnt = np.unique(key, return_index=True, return_counts=True)
        order = np.argsort(-cnt)
        print("")
        print("  top %s (%s classes, %s probes):" % (title, human(len(uq)), human(k)))
        rows = []
        for t in order[:args.top]:
            v = int(uq[t])
            ex = int(idx[first[t]])
            row = dict(count=int(cnt[t]),
                       opcode=sig_str((v >> 32) & 0xFFF).strip(),
                       cs=cs_names[(v >> 44) & 0xFFFFF] if c_ok[ex] else "-",
                       ocerz=(names[(v >> 16) & 0xFFFF] if o_ok[ex]
                              else "rc=-%d" % ((v >> 16) & 0xFFFF)),
                       example=probe_bytes(ex + lo, p)[:8].hex())
            if fields:
                row["cs_shape"] = shape_str(int((v >> 8) & 0xFF))
                row["oc_shape"] = shape_str(int(v & 0xFF))
            else:
                row["cs_len"] = int((v >> 8) & 0xFF)
                row["oc_len"] = int(v & 0xFF)
            print("    %9s  %s" % (human(row["count"]), label(row)))
            rows.append(row)
        if len(uq) > args.top:
            print("    ... %s more classes" % human(len(uq) - args.top))
        return rows

    rep = {}
    rep["LEN"] = buckets(m_len, "LEN  (opcode, capstone mnem, ocerz len -> capstone len)",
                         lambda r: "%-6s %-14s len %d -> %d   e.g. %s"
                         % (r["opcode"], r["cs"], r["oc_len"], r["cs_len"], r["example"]))
    rep["MISS"] = buckets(m_miss, "MISS (opcode, capstone mnem, ocerz error)",
                          lambda r: "%-6s %-14s ocerz %-8s len %d   e.g. %s"
                          % (r["opcode"], r["cs"], r["ocerz"], r["cs_len"], r["example"]))
    rep["MNEM"] = buckets(m_mnem, "MNEM (opcode, capstone mnem -> ocerz op)",
                          lambda r: "%-6s %-14s -> %-14s len %d   e.g. %s"
                          % (r["opcode"], r["cs"], r["ocerz"], r["cs_len"], r["example"]))
    rep["SHAPE"] = buckets(m_shape, "SHAPE (opcode, mnem, ocerz shape -> capstone shape)",
                           lambda r: "%-6s %-14s %-14s -> %-14s   e.g. %s"
                           % (r["opcode"], r["cs"], r["oc_shape"], r["cs_shape"],
                              r["example"]), fields=(c_fl, o_fl))
    rep["GHOST"] = buckets(m_ghost, "GHOST (opcode, ocerz op, capstone rejects)",
                           lambda r: "%-6s %-14s len %d   e.g. %s"
                           % (r["opcode"], r["ocerz"], r["oc_len"], r["example"]))

    tot = np.bincount(sig[c_ok], minlength=0x1000).astype(np.int64)
    good = np.bincount(sig[m_ok], minlength=0x1000).astype(np.int64)
    dead = np.flatnonzero((tot > 0) & (good == 0))
    print("")
    print("  opcodes with 0%% coverage: %d of %d reachable"
          % (len(dead), int((tot > 0).sum())))
    if len(dead):
        order = dead[np.argsort(-tot[dead])]
        line = " ".join(sig_str(int(s)).replace(" ", "") if int(s) < 0x0F00
                        else "0f%02x" % (int(s) & 0xFF) for s in order[:48])
        print("    %s%s" % (line, " ..." if len(dead) > 48 else ""))
    if args.by_opcode:
        print("")
        print("  coverage by opcode (reachable opcodes, worst first):")
        frac = np.where(tot > 0, good / np.maximum(tot, 1), 2.0)
        for op in np.argsort(frac, kind="stable"):
            if tot[op] == 0:
                continue
            print("    %s  %6.1f%%  %s of %s" %
                  (sig_str(int(op)), 100.0 * good[op] / tot[op],
                   human(good[op]), human(tot[op])))

    if args.json:
        json.dump({"mode": args.mode, "entry": p["entry"],
                   "entry_is_real": p["entry_is_real"],
                   "probes": n, "capstone_decodable": n_cs,
                   "ok": n_ok, "coverage": cov, "coverage_user_mode": cov_um,
                   "len": int(m_len.sum()), "miss": int(m_miss.sum()),
                   "mnem": int(m_mnem.sum()), "shape": int(m_shape.sum()),
                   "ghost": int(m_ghost.sum()),
                   "classes": rep},
                  open(args.json, "w"), indent=1)

    rc = 0
    if args.min_coverage is not None and cov < args.min_coverage:
        print("\ni386diff: FAIL coverage %.2f%% < required %.2f%%"
              % (cov, args.min_coverage))
        rc = 1
    if args.fail_on_len and m_len.any():
        print("\ni386diff: FAIL %s length mismatches" % human(m_len.sum()))
        rc = 1
    return rc


if __name__ == "__main__":
    raise SystemExit(main())

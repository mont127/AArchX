#!/usr/bin/env python3
"""dec32-oracle -- differential 32-bit decode check against capstone.

decodiff proves 64-bit decoding never changes.  This is the other gate: it
proves the *new* 32-bit decoding is RIGHT, by comparing ocerz's decoder
field-for-field against capstone CS_MODE_32 over exhaustive ModRM/SIB sweeps.

capstone is used strictly as a black-box oracle: we run it and diff its
answers.  No capstone source, tables or algorithms are read or reproduced.

usage: dec32-oracle.py <path-to-dec32probe> [suite ...]

suites, in the order they run by default:
  modrm32   every ModRM and SIB byte for 25 r/m32 opcodes
  sib32     the same under 0x66 and each of the six segment overrides
  modrm16   the 16-bit addressing table, reached by 0x67 in 32-bit mode
  byteregs  every 8-bit ModRM form, where AH/CH/DH/BH have to appear
  opsize    the operand/address-size defaults and the 0x66/0x67 swap
  misc      the hand-picked forms from the stage brief, incl. the TEB read
  rip64     64-bit spot check that mod=00 rm=101 is STILL RIP-relative
  detail32  every opcode x every ModRM x eight prefix sets, all fields
opt-in, exhaustive, roughly a minute each:
  sweep32   all 2^24 three-byte openings, lengths only
  sweep64   the same in 64-bit mode
  detail64  informational: where HEAD and capstone already disagreed
Run "all" for every suite; with no suite argument the eight defaults run.

What is compared, per input byte string:
  - decode length
  - memory operands: base, index, scale, displacement, width, segment, and
    whether the form is RIP-relative -- asserted in BOTH directions, since
    mod=00 rm=101 flipping mode is the single most dangerous regression here
  - register operands: identity and width, including AH/CH/DH/BH
  - immediate operands: value, masked to the immediate's own width
Inputs capstone cannot decode are skipped, as are those ocerz has not
implemented (an unimplemented opcode belongs to the opcode-map stage), as are
the few documented divergences listed at excluded() below.
"""
import subprocess
import sys

from capstone import (Cs, CS_ARCH_X86, CS_MODE_32, CS_MODE_64,
                      CS_OP_MEM, CS_OP_REG, CS_OP_IMM)
from capstone.x86 import X86_REG_INVALID

BASE = 0x00401000

# ocerz models only the two segments that can have a nonzero base in the
# memory models it targets: OCERZ_SEG_NONE / FS / GS (include/ocerz/cpu.h).
# CS/DS/ES/SS overrides are folded to NONE because they are flat (base 0) in
# both long mode and i386 Windows.  That is a deliberate modelling choice, so
# the comparator accepts ocerz's NONE against any of the flat four.
OZSEG = [None, "fs", "gs"]
FLAT_SEGS = (None, "cs", "ds", "es", "ss")


def seg_match(ozseg, csseg):
    if ozseg is None:
        return csseg in FLAT_SEGS
    return ozseg == csseg

# ocerz GPR numbering (include/ocerz/cpu.h) -> capstone register-name stem.
GPR = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
       "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]


def ocerz_regname(num, size, high8):
    """Render an ocerz (reg,size,high8) triple the way capstone names it."""
    if num == 0xFF:
        return None
    if high8:
        return ["ah", "ch", "dh", "bh"][num]
    if num < 8:
        stem = GPR[num]
        if size == 8:
            return "r" + stem
        if size == 4:
            return "e" + stem
        if size == 2:
            return stem
        # 1-byte low registers: al/cl/dl/bl, then spl/bpl/sil/dil
        return ["al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil"][num]
    stem = GPR[num]
    return {8: stem, 4: stem + "d", 2: stem + "w", 1: stem + "b"}[size]


def addr_regname(num, addrsize):
    """A base/index register is named at the ADDRESS size, not the operand's."""
    return ocerz_regname(num, addrsize, 0)


# Encodings where ocerz deliberately differs from capstone.  Every entry is a
# decision with a reason, not a bug being papered over; nothing here is in the
# addressing/width lane this gate covers.
#
#   0x63     -- MOVSXD in long mode, ARPL in i386.  Which OPERATION a byte
#               names is the opcode-map stage's business, not this one.
#   0F B9    -- UD1.  ocerz consumes a ModRM byte, capstone stops at two
#               bytes.  Pre-existing and bit-identical in both modes (the
#               decodiff digest pins it), so it is not an i386 question.
#   66 0F C8+r -- BSWAP with a 16-bit operand size.  The SDM states the result
#               is UNDEFINED for 16-bit operands, so there is no correct
#               answer to match; ocerz keeps naming the 32-bit register.
def excluded(seq):
    b = [x for x in seq]
    i = 0
    has66 = False
    while i < len(b) and (b[i] in (0x66, 0x67, 0xF0, 0xF2, 0xF3, 0x2E, 0x36,
                                   0x3E, 0x26, 0x64, 0x65)
                          or 0x40 <= b[i] <= 0x4F):
        has66 = has66 or b[i] == 0x66
        i += 1
    if i >= len(b):
        return False
    if b[i] == 0x63:
        return True
    if b[i] == 0x0F and i + 1 < len(b):
        if b[i + 1] == 0xB9:
            return True
        if has66 and 0xC8 <= b[i + 1] <= 0xCF:
            return True
    return False


def parse_probe(line):
    """Parse one dec32probe record into a dict, or None on ERR."""
    if line.startswith("ERR"):
        return None
    fields = {}
    ops = []
    cur = None
    for tok in line.split():
        if tok == "OK":
            continue
        if tok.startswith("|"):
            cur = {}
            ops.append(cur)
            continue
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        val = v if k == "op" else int(v)
        (cur if cur is not None else fields)[k] = val
    fields["ops"] = ops
    return fields


def run_probe(probe, mode, seqs):
    args = [probe] + (["64"] if mode == 64 else [])
    inp = "".join(" ".join("%02x" % b for b in s) + "\n" for s in seqs)
    out = subprocess.run(args, input=inp, capture_output=True, text=True,
                         check=True).stdout.splitlines()
    assert len(out) == len(seqs), "probe returned %d lines for %d inputs" % (
        len(out), len(seqs))
    return [parse_probe(x) for x in out]


class Checker:
    def __init__(self, probe, mode=32):
        self.probe = probe
        self.mode = mode
        cs_mode = CS_MODE_32 if mode == 32 else CS_MODE_64
        self.md = Cs(CS_ARCH_X86, cs_mode)
        self.md.detail = True
        self.checked = 0
        self.skipped_cs = 0
        self.skipped_unimpl = 0
        self.excluded = 0
        self.fails = []

    def cs_one(self, seq):
        for i in self.md.disasm(bytes(seq), BASE):
            return i
        return None

    def check(self, seqs, want_len=True, want_mem=True, want_reg=True,
              want_imm=True):
        recs = run_probe(self.probe, self.mode, seqs)
        for seq, rec in zip(seqs, recs):
            ci = self.cs_one(seq)
            if ci is None:
                self.skipped_cs += 1
                continue
            if rec is None:
                self.skipped_unimpl += 1
                continue
            if excluded(seq):
                self.excluded += 1
                continue
            self.compare(seq, rec, ci, want_len, want_mem, want_reg, want_imm)

    def fail(self, seq, why):
        self.fails.append((" ".join("%02x" % b for b in seq), why))

    def compare(self, seq, rec, ci, want_len, want_mem, want_reg, want_imm):
        self.checked += 1
        if want_len and rec["len"] != ci.size:
            self.fail(seq, "len %d, capstone %d (%s %s)" %
                      (rec["len"], ci.size, ci.mnemonic, ci.op_str))
            return
        addrsize = rec["addrsize"]
        amask = (1 << (addrsize * 8)) - 1

        cs_mems = [o for o in ci.operands if o.type == CS_OP_MEM]
        oz_mems = [o for o in rec["ops"] if o["k"] == 4]
        if want_mem and len(cs_mems) == 1 and len(oz_mems) == 1:
            c, z = cs_mems[0].mem, oz_mems[0]
            cb = None if c.base == X86_REG_INVALID else ci.reg_name(c.base)
            cx = None if c.index == X86_REG_INVALID else ci.reg_name(c.index)
            # capstone names an absent SIB index "riz"/"eiz", and names the
            # RIP-relative base "rip"/"eip"; ocerz spells both as REG_NONE,
            # the latter flagged by riprel.  Normalise, do not "fix".
            if cx in ("riz", "eiz"):
                cx = None
            if cb in ("rip", "eip"):
                cb = None
                if not z["rip"]:
                    self.fail(seq, "not riprel, capstone is (%s %s)" %
                              (ci.mnemonic, ci.op_str))
                    return
            elif z["rip"]:
                self.fail(seq, "riprel, capstone is not (%s %s)" %
                          (ci.mnemonic, ci.op_str))
                return
            zb = addr_regname(z["b"], addrsize)
            zx = addr_regname(z["ix"], addrsize)
            if cb != zb or cx != zx:
                self.fail(seq, "mem base/index %s/%s, capstone %s/%s (%s %s)" %
                          (zb, zx, cb, cx, ci.mnemonic, ci.op_str))
                return
            zsc = 1 << z["sc"] if zx is not None else 1
            csc = c.scale if cx is not None else 1
            if zsc != csc:
                self.fail(seq, "scale %d, capstone %d (%s %s)" %
                          (zsc, csc, ci.mnemonic, ci.op_str))
                return
            # ocerz folds a RIP-relative disp into an absolute target; capstone
            # reports the raw displacement.  Only 64-bit mode can hit this.
            zd = z["d"]
            if z["rip"]:
                zd -= BASE + ci.size
            if (zd & amask) != (c.disp & amask):
                self.fail(seq, "disp %#x, capstone %#x (%s %s)" %
                          (zd & amask, c.disp & amask, ci.mnemonic, ci.op_str))
                return

            # Memory operand WIDTH is compared only where ocerz and capstone
            # agree on what the field means.  Three documented exclusions,
            # every one of them a pre-existing 64-bit modelling choice rather
            # than an i386 question:
            #   - size 0 is ocerz's "width does not apply" marker (fxsave...)
            #   - SSE/x87 operands: ocerz records the register width, capstone
            #     the memory width (movlps: 16 vs 8)
            #   - callf/jmpf: ocerz's opsize is the far pointer's OFFSET width
            #     (src/interp.c reads sel at ea+opsize), capstone's is the
            #     whole m16:32.  capstone is also self-inconsistent here.
            #   - les/lds (c4/c5): the same far-pointer disagreement seen from
            #     the other side.  The source operand IS a whole m16:32, so the
            #     SDM width is 6 with a 32-bit operand size and 4 with a 16-bit
            #     one, and that is what src/decode.c records; capstone reports
            #     4 for both forms, i.e. the offset half only.  Deliberate
            #     divergence, documented at the decode site as well.
            wide = any(o["k"] in (2, 3) for o in rec["ops"])
            if (z["sz"] and not wide
                    and rec["op"] not in ("callf", "jmpf", "les", "lds")
                    and z["sz"] != cs_mems[0].size):
                self.fail(seq, "mem size %d, capstone %d (%s %s)" %
                          (z["sz"], cs_mems[0].size, ci.mnemonic, ci.op_str))
                return
            cseg = (None if c.segment == X86_REG_INVALID
                    else ci.reg_name(c.segment))
            zseg = OZSEG[rec["seg"]]
            if not seg_match(zseg, cseg):
                self.fail(seq, "seg %s, capstone %s (%s %s)" %
                          (zseg, cseg, ci.mnemonic, ci.op_str))
                return

        cs_regs = [o for o in ci.operands if o.type == CS_OP_REG]
        oz_regs = [o for o in rec["ops"] if o["k"] == 1]
        if want_reg and len(cs_regs) == len(oz_regs) and cs_regs:
            # As a MULTISET: ocerz and capstone disagree on operand ORDER for
            # some encodings (capstone prints "xchg ecx, eax" where ocerz
            # records eAX first), and capstone's print order is not an
            # authority on ocerz's internal order.  Register identity and
            # width, which is what this stage changes, is order-free.
            cn = sorted(ci.reg_name(c.reg) for c in cs_regs)
            zn = sorted(ocerz_regname(z["r"], z["sz"], z["h8"])
                        for z in oz_regs)
            if cn != zn:
                self.fail(seq, "regs %s, capstone %s (%s %s)" %
                          (",".join(zn), ",".join(cn), ci.mnemonic,
                           ci.op_str))
                return

        cs_imms = [o for o in ci.operands if o.type == CS_OP_IMM]
        oz_imms = [o for o in rec["ops"] if o["k"] == 5]
        if want_imm and len(cs_imms) == len(oz_imms) == 1:
            c, z = cs_imms[0], oz_imms[0]
            w = z["sz"] if z["sz"] in (1, 2, 4, 8) else 8
            m = (1 << (w * 8)) - 1
            if (c.imm & m) != (z["imm"] & m):
                self.fail(seq, "imm %#x/%dB, capstone %#x (%s %s)" %
                          (z["imm"] & m, w, c.imm & m, ci.mnemonic,
                           ci.op_str))
                return

    def report(self, name):
        ok = not self.fails
        print("%-10s %-4s %7d checked  %6d skip(capstone)  "
              "%6d skip(unimpl)  %5d excluded  %s" %
              (name, "%db" % self.mode, self.checked, self.skipped_cs,
               self.skipped_unimpl, self.excluded, "OK" if ok else
               "FAIL %d" % len(self.fails)))
        seen = {}
        for seq, why in self.fails:
            key = why.split("(")[0].rstrip()
            seen.setdefault(key, []).append(seq)
        for key, seqs in sorted(seen.items(), key=lambda kv: -len(kv[1]))[:40]:
            print("    x%-6d %-46s e.g. %s" % (len(seqs), key, seqs[0]))
        return ok


# ---------------------------------------------------------------- suites ---

# Opcodes with a ModRM byte and a plain r/m32 or r32 operand, safe to sweep.
MODRM_OPCODES = [
    (0x01,), (0x03,), (0x09,), (0x0B,), (0x21,), (0x23,), (0x29,), (0x2B,),
    (0x31,), (0x33,), (0x39,), (0x3B,), (0x85,), (0x89,), (0x8B,), (0x8D,),
    (0x87,), (0xC7,), (0xF7,), (0xFF,),
    (0x0F, 0xB6), (0x0F, 0xB7), (0x0F, 0xAF), (0x0F, 0x10), (0x0F, 0x28),
]
# Byte-operand ModRM opcodes: these are where AH..BH must appear.
MODRM8_OPCODES = [(0x00,), (0x02,), (0x08,), (0x0A,), (0x30,), (0x32,),
                  (0x38,), (0x3A,), (0x84,), (0x86,), (0x88,), (0x8A,),
                  (0x0F, 0x90), (0x0F, 0x94), (0x0F, 0x9F)]

TAIL = [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB]


def sweep_modrm(opcodes, prefixes=()):
    """Every ModRM byte (and, for rm=100, every SIB byte) for each opcode."""
    out = []
    for opc in opcodes:
        for modrm in range(256):
            head = list(prefixes) + list(opc) + [modrm]
            if (modrm & 0xC0) != 0xC0 and (modrm & 7) == 4:
                for sib in range(256):
                    out.append(head + [sib] + TAIL)
            else:
                out.append(head + TAIL)
    return out


def suite_modrm32(probe):
    c = Checker(probe, 32)
    c.check(sweep_modrm(MODRM_OPCODES))
    return c.report("modrm32")


def suite_sib32(probe):
    """Same sweep with 0x66, plus every segment override, in 32-bit mode."""
    c = Checker(probe, 32)
    for pre in ([0x66], [0x2E], [0x36], [0x3E], [0x26], [0x64], [0x65]):
        c.check(sweep_modrm([(0x8B,), (0x89,), (0x8D,)], pre))
    return c.report("sib32")


def suite_modrm16(probe):
    """0x67 in 32-bit mode selects the 16-bit addressing table.

    ocerz has no free-standing 16-bit mode, so the only way to reach the
    16-bit ModRM table is 32-bit mode with a 0x67 prefix -- which is exactly
    the form wine's 16-bit thunks and DOS-era code use.  capstone is driven in
    CS_MODE_32 with the same prefix, so both sides see the identical bytes.
    """
    c = Checker(probe, 32)
    seqs = sweep_modrm([(0x8B,), (0x89,), (0x8D,), (0x8A,), (0x88,)],
                       prefixes=[0x67])
    c.check(seqs)
    # 0x67 combined with 0x66 (16-bit operand AND 16-bit address) and with a
    # segment override, since those stack on the same path.
    for pre in ([0x66, 0x67], [0x67, 0x66], [0x64, 0x67], [0x67, 0x2E]):
        c.check(sweep_modrm([(0x8B,), (0x8D,)], prefixes=pre))
    return c.report("modrm16")


def suite_byteregs(probe):
    c = Checker(probe, 32)
    c.check(sweep_modrm(MODRM8_OPCODES))
    # ALU imm8 group and mov r8,imm8 reach the 8-bit register path too.
    c.check([[0x80, m] + TAIL for m in range(0xC0, 0x100)])
    c.check([[0xB0 + r, 0x5A] + TAIL for r in range(8)])
    c.check([[0xFE, m] + TAIL for m in range(0xC0, 0x100)])
    return c.report("byteregs")


def suite_opsize(probe):
    """Operand-size defaults and the 0x66/0x67 swap, both modes."""
    c = Checker(probe, 32)
    seqs = []
    for pre in ([], [0x66], [0x67], [0x66, 0x67], [0x67, 0x66]):
        for opc in ([0x8B, 0x00], [0x8B, 0x05], [0x8B, 0x04, 0x8D],
                    [0xA1], [0xA3], [0x05], [0x3D], [0xB8],
                    [0xE8], [0xE9], [0xEB], [0x0F, 0x84], [0x74],
                    [0x50], [0x58], [0x68], [0x6A], [0xC3], [0xC2],
                    [0xC9], [0x9C], [0x9D], [0xFF, 0x30], [0xFF, 0x20],
                    [0xFF, 0x10], [0x8F, 0x00]):
            seqs.append(pre + opc + TAIL)
    c.check(seqs)
    return c.report("opsize")


def suite_misc(probe):
    """Hand-picked forms from the brief plus the TEB read and absolutes."""
    c = Checker(probe, 32)
    seqs = [
        [0xA1, 0x18, 0x00, 0x00, 0x00],
        [0x64, 0xA1, 0x18, 0x00, 0x00, 0x00],
        [0xFF, 0x20],
        [0x66, 0xFF, 0xE0],
        [0x8B, 0x05, 0x01, 0x02, 0x03, 0x04],
        [0x8B, 0x0D, 0xFF, 0xFF, 0xFF, 0xFF],
        [0x8B, 0x04, 0x25, 0x78, 0x56, 0x34, 0x12],
        [0x8B, 0x44, 0x8D, 0x10],
        [0x8B, 0x84, 0xCB, 0x00, 0x10, 0x00, 0x00],
        [0x67, 0x8B, 0x06],
        [0x67, 0x8B, 0x07],
        [0x67, 0x8B, 0x00],
        [0x67, 0x8B, 0x01],
        [0x67, 0x8B, 0x02],
        [0x67, 0x8B, 0x03],
        [0x67, 0x8B, 0x04],
        [0x67, 0x8B, 0x05],
        [0x67, 0x8B, 0x46, 0x10],
        [0x67, 0x8B, 0x86, 0x00, 0x10],
        [0x67, 0x8D, 0x36, 0x34, 0x12],
        [0x88, 0xE0], [0x88, 0xE9], [0x88, 0xF2], [0x88, 0xFB],
        [0x8A, 0x25, 0x00, 0x20, 0x40, 0x00],
        [0x65, 0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00],
    ]
    c.check([s + TAIL[:16 - len(s)] for s in seqs])
    return c.report("misc")


# --- 64-bit spot checks: the same comparator, aimed at the paths this stage
# --- touched, to catch a regression the decodiff digest could only report as
# --- an opaque hash mismatch.
def suite_rip64(probe):
    c = Checker(probe, 64)
    seqs = sweep_modrm([(0x8B,), (0x89,), (0x8D,)])
    seqs += [[0x48] + s for s in sweep_modrm([(0x8B,)])]
    seqs += [[0x67] + s for s in sweep_modrm([(0x8B,)])]
    c.check(seqs, want_reg=False)
    return c.report("rip64")


def suite_detail32(probe, mode=32):
    """Full-detail sweep: every opcode x every ModRM x eight prefix sets.

    The curated suites above pick opcodes by hand; this one takes the whole
    one-byte and 0F-escape maps and compares every field the comparator knows
    about -- length, memory base/index/scale/displacement/width/segment,
    register identity, and immediate value -- against capstone.  It is the
    broadest statement available that the ADDRESSING and WIDTH rules are
    right, because those paths are shared by every ModRM-bearing opcode.
    """
    c = Checker(probe, mode)
    prefix_sets = ([], [0x66], [0x67], [0x66, 0x67], [0xF0], [0xF2], [0xF3],
                   [0x64])
    if mode == 64:
        prefix_sets = prefix_sets + ([0x48], [0x41], [0x4F])
    for pre in prefix_sets:
        for esc in ([], [0x0F]):
            seqs = []
            for opc in range(256):
                for modrm in range(256):
                    seqs.append(pre + esc + [opc, modrm] + TAIL)
            c.check(seqs)
    return c.report("detail%d" % mode)


def suite_detail64(probe):
    """The same sweep in 64-bit mode -- INFORMATIONAL, never a gate.

    Long-mode decoding is frozen by the decodiff digest, so nothing here can
    be "fixed" without failing that gate.  What it does is show, in one place,
    every spot where HEAD and capstone already disagreed before any i386 work
    started, so an integrator can tell a pre-existing quirk from a new one.
    Two classes account for essentially all of it, and HEAD is arguably right
    in both:
      - 0x66 on a near branch (66 0F 8x): HEAD ignores it, per SDM Vol.2
        "In 64-bit mode ... the operand size is forced to 64 bits"; capstone
        shortens the displacement to rel16.
      - 0x66 then REX.W on PUSH/POP (66 48 50): HEAD reports the 16-bit form,
        capstone the 64-bit one.  REX.W should win; this is a real HEAD bug,
        but it is a LONG-MODE bug and fixing it belongs with a decodiff
        baseline bump, not with i386 support.
    """
    suite_detail32(probe, 64)
    print("           (detail64 is informational; long-mode decoding is "
          "frozen by decodiff)")
    return True


SWEEP_TAIL = bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
                    0xAA, 0xBB, 0xCC, 0xDD])


def suite_sweep32(probe, mode=32):
    """Exhaustive length sweep: all 2^24 three-byte openings vs capstone.

    The curated suites above check field detail on hand-picked opcodes; this
    checks ONE field (the instruction length) over literally every opcode,
    prefix combination up to three deep, and ModRM/SIB pairing.  Length is the
    field that catches an operand- or address-size mistake fastest: get the
    default width wrong and the immediate or displacement you consume is the
    wrong number of bytes, so the length moves.

    Openings ocerz refuses (length 0) are counted, not failed -- an
    unimplemented opcode is the opcode stage's business.  Openings capstone
    refuses are likewise skipped.
    """
    import os
    import tempfile
    md = Cs(CS_ARCH_X86, CS_MODE_32 if mode == 32 else CS_MODE_64)
    fd, path = tempfile.mkstemp(suffix=".bin")
    os.close(fd)
    try:
        subprocess.run([probe, "sweep", str(mode), path], check=True,
                       capture_output=True)
        ours = open(path, "rb").read()
    finally:
        os.unlink(path)

    n = len(ours)
    checked = unimpl = only_ours = excl = 0
    fails = {}
    disasm = md.disasm_lite
    tail = SWEEP_TAIL
    for i in range(n):
        b = bytes((i & 0xFF, (i >> 8) & 0xFF, (i >> 16) & 0xFF)) + tail
        cs_size = 0
        for (_a, size, _m, _o) in disasm(b, BASE, count=1):
            cs_size = size
            break
        mine = ours[i]
        if not mine and not cs_size:
            continue
        if not mine:
            unimpl += 1
            continue
        if not cs_size:
            only_ours += 1
            continue
        if excluded(b):
            excl += 1
            continue
        checked += 1
        if mine != cs_size:
            key = "%02x %02x len %d != %d" % (b[0], b[1], mine, cs_size)
            fails[key] = fails.get(key, 0) + 1

    print("sweep%-5d %-4s %8d checked  %7d ocerz-only  %7d unimpl  "
          "%5d excluded  %s"
          % (mode, "%db" % mode, checked, only_ours, unimpl, excl,
             "OK" if not fails else "FAIL %d" % sum(fails.values())))
    for k, v in sorted(fails.items(), key=lambda kv: -kv[1])[:20]:
        print("    x%-6d %s" % (v, k))
    if len(fails) > 20:
        print("    ... %d more distinct classes" % (len(fails) - 20))
    return not fails


def suite_sweep64(probe):
    return suite_sweep32(probe, 64)


SUITES = {
    "modrm32": suite_modrm32,
    "sib32": suite_sib32,
    "modrm16": suite_modrm16,
    "byteregs": suite_byteregs,
    "opsize": suite_opsize,
    "misc": suite_misc,
    "rip64": suite_rip64,
    "detail32": suite_detail32,
    "detail64": suite_detail64,
    "sweep32": suite_sweep32,
    "sweep64": suite_sweep64,
}

# The exhaustive sweeps are slow (~2 min each) and are dominated by opcode-map
# gaps that belong to another stage, so they are opt-in rather than default.
DEFAULT = ["modrm32", "sib32", "modrm16", "byteregs", "opsize", "misc",
           "rip64", "detail32"]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    probe = sys.argv[1]
    names = sys.argv[2:] or DEFAULT
    if names == ["all"]:
        names = list(SUITES)
    ok = True
    for n in names:
        ok = SUITES[n](probe) and ok
    print("dec32-oracle: %s" % ("ALL PASS" if ok else "FAILURES"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

//! The x86_64 and i386 instruction decoder: bytes in, one X86Insn out.
//!
//! ---- the mode32 seam ----
//! The i386 additions had to be made without moving 64-bit decode by a single
//! bit, because the decodiff gate compares the two decoders' 64-bit output and
//! must print IDENTICAL. So rather than sprinkling `if (s->mode32)` through the
//! one-byte map - where one misplaced test would silently change long-mode
//! decode - the whole i386-only slice lives in one function that the main
//! dispatcher calls only when mode32 is set. Every byte it owns is UNDEFINED in
//! long mode, and it sets *handled even for the architecturally invalid forms
//! (BOUND/LES/LDS with a register r/m), which must report OCERZ_EUNDEF from
//! there rather than fall through to a generic path. Where a width differs
//! between the modes, the 64-bit arm keeps the literal the decoder has always
//! written and only the 32-bit arm is new.
//!
//! The widths themselves split three ways. Ops with an EXPLICIT stack operand
//! (PUSH/POP r, PUSH imm, PUSH/POP r/m) take 0x66 as the 16-bit form in both
//! modes. Ops whose stack traffic is IMPLICIT (RET, LEAVE, PUSHF/POPF) are
//! unconditionally 8 in long mode: the RETW/PUSHFW forms are not implemented
//! and the gate requires that stay so. Near branches (JMP/CALL rel and r/m,
//! Jcc, LOOP, JrCXZ) have their operand size FORCED to 64 in long mode with
//! 0x66 ignored, per the SDM, so only the 32-bit arm is live there.
//!
//! ---- things the reference tools get wrong ----
//! With a 16-bit operand size the SDM has the near branch clear the upper two
//! bytes of EIP outright, which capstone does not model; a target-level diff
//! against it therefore disagrees for a 0x66-prefixed branch whose target
//! crosses 64K, which is why the capstone-checked corpus rows sit below that
//! boundary. LES/LDS load a far pointer, so their memory operand is 2+opsize -
//! 6 bytes for the 32-bit form - while capstone reports 4 for both and prints
//! no size keyword, which is capstone declining to model m16:32 rather than a
//! fact about the ISA.
//!
//! ---- prefixes and encodings ----
//! 0x40-0x4f are REX only in long mode; in 32-bit mode they are INC/DEC r32 and
//! the prefix loop must leave them intact. 0x67 swaps a mode's default address
//! size for the other size that mode can name: 64<->32 in long mode, 32<->16
//! in 32-bit mode - and the 16-bit addressing table is nothing like the other
//! one, with no SIB byte, a fixed set of BX/BP bases optionally paired with
//! SI/DI, and the displacement-only form at mod=00 rm=110 instead of rm=101.
//! 0x82 is the i386-only alias of 0x80. VEX (C5 two-byte, C4 three-byte) is
//! decoded onto the legacy SSE opcode maps and the result tagged, so the
//! interpreter can apply the AVX semantics - non-destructive first source,
//! upper zeroing, 256-bit forms - while the JIT simply declines every VEX
//! instruction.
//!
//! ---- representation ----
//! There is no OCERZ_OPK_SREG: MOVSEG already encodes its destination segment
//! register as a size-1 immediate holding the sreg index, and PUSHSEG/POPSEG
//! follow that convention, with the width actually moved on the stack recorded
//! in insn.opsize. `mov r/m, Sreg` used to fold the selector into an immediate
//! (CS=0x2b, SS=0x23), which is only true in long mode: 32-bit code in a WoW64
//! process has an LDT code selector, and Wine's RtlCaptureContext stores what
//! it reads here into the context that later decides which mode an iretq
//! returns to. The direct far CALL/JMP ptr16:32 form is encoded offset-then-
//! selector but recorded selector-first, matching how a far pointer reads as
//! seg:off.
//!
//! A NULL code pointer here is an emulator bug upstream, since every caller
//! goes through ocerz_g2h: say so once and fail the decode rather than fault.
//! And a missing opcode is not academic - LDDQU is just an unaligned 16-byte
//! load, but Chromium's renderer uses it for SIMD UTF-8 scanning and failing to
//! decode it killed every renderer Steam launched.

#![allow(unsafe_op_in_unsafe_fn)]

use core::ffi::{c_char, c_int, c_uint};
use core::{mem, ptr};
use std::sync::atomic::{AtomicBool, Ordering};

use crate::ffi::*;

mod map0f;
mod map0f38;
mod names;
mod onebyte;
mod x87;

const MAND_NONE: c_int = 0;
const MAND_66: c_int = 1;
const MAND_F3: c_int = 2;
const MAND_F2: c_int = 3;

const fn prefix_classes() -> [u8; 256] {
    let mut pfx = [0; 256];
    let mut b = 0x40;
    while b < 0x50 {
        pfx[b] = 1;
        b += 1;
    }
    pfx[0x66] = 2;
    pfx[0x67] = 3;
    pfx[0xf0] = 4;
    pfx[0xf2] = 5;
    pfx[0xf3] = 6;
    pfx[0x2e] = 7;
    pfx[0x36] = 7;
    pfx[0x3e] = 7;
    pfx[0x26] = 7;
    pfx[0x64] = 8;
    pfx[0x65] = 9;
    pfx
}

static PFX: [u8; 256] = prefix_classes();

struct DecState {
    base: *const u8,
    p: *const u8,
    end: *const u8,
    avail: usize,
    rip: u64,
    has_66: c_int,
    has_67: c_int,
    has_f0: c_int,
    rep: c_int,
    mand: c_int,
    seg: c_int,
    rex: c_int,
    rex_present: c_int,
    rex_w: c_int,
    rex_r: c_int,
    rex_x: c_int,
    rex_b: c_int,
    mode32: c_int,
    addr16: c_int,
    vex: c_int,
    vex_l: c_int,
    vex_w: c_int,
    vex_vvvv: c_int,
    out: *mut X86Insn,
}

struct ModRM {
    mod_: c_int,
    reg: c_int,
    rm: c_int,
    mem: X86Operand,
}

#[inline]
fn ocerz_sext(v: u64, size: c_int) -> i64 {
    let shift = 64 - size * 8;
    ((v << shift) as i64) >> shift
}

#[inline(always)]
unsafe fn fetch8(s: &mut DecState, v: *mut u8) -> c_int {
    if s.p >= s.end {
        return OCERZ_ETRUNC as c_int;
    }
    *v = *s.p;
    s.p = s.p.add(1);
    OCERZ_OK as c_int
}

#[inline(always)]
unsafe fn fetch16(s: &mut DecState, v: *mut u16) -> c_int {
    if s.end.offset_from(s.p) < 2 {
        return OCERZ_ETRUNC as c_int;
    }
    *v = u16::from_le(ptr::read_unaligned(s.p.cast::<u16>()));
    s.p = s.p.add(2);
    OCERZ_OK as c_int
}

#[inline(always)]
unsafe fn fetch32(s: &mut DecState, v: *mut u32) -> c_int {
    if s.end.offset_from(s.p) < 4 {
        return OCERZ_ETRUNC as c_int;
    }
    *v = u32::from_le(ptr::read_unaligned(s.p.cast::<u32>()));
    s.p = s.p.add(4);
    OCERZ_OK as c_int
}

#[inline(always)]
unsafe fn fetch64(s: &mut DecState, v: *mut u64) -> c_int {
    if s.end.offset_from(s.p) < 8 {
        return OCERZ_ETRUNC as c_int;
    }
    let r = u64::from_le(ptr::read_unaligned(s.p.cast::<u64>()));
    s.p = s.p.add(8);
    *v = r;
    OCERZ_OK as c_int
}

#[inline(always)]
unsafe fn set_reg(op: *mut X86Operand, reg: c_int, size: c_int) {
    (*op).kind = OCERZ_OPK_REG as u8;
    (*op).reg = reg as u8;
    (*op).size = size as u8;
    (*op).high8 = 0;
    (*op).base = OCERZ_REG_NONE as u8;
    (*op).index = OCERZ_REG_NONE as u8;
    (*op).scale = 0;
    (*op).riprel = 0;
    (*op).disp = 0;
    (*op).imm = 0;
}

#[inline(always)]
unsafe fn set_reg8(s: &DecState, op: *mut X86Operand, enc: c_int) {
    (*op).kind = OCERZ_OPK_REG as u8;
    (*op).size = 1;
    (*op).base = OCERZ_REG_NONE as u8;
    (*op).index = OCERZ_REG_NONE as u8;
    (*op).scale = 0;
    (*op).riprel = 0;
    (*op).disp = 0;
    (*op).imm = 0;
    if s.rex_present == 0 && (4..=7).contains(&enc) {
        (*op).reg = (enc - 4) as u8;
        (*op).high8 = 1;
    } else {
        (*op).reg = enc as u8;
        (*op).high8 = 0;
    }
}

#[inline(always)]
unsafe fn set_xmm(op: *mut X86Operand, reg: c_int, size: c_int) {
    (*op).kind = OCERZ_OPK_XMM as u8;
    (*op).reg = reg as u8;
    (*op).size = size as u8;
    (*op).high8 = 0;
    (*op).base = OCERZ_REG_NONE as u8;
    (*op).index = OCERZ_REG_NONE as u8;
    (*op).scale = 0;
    (*op).riprel = 0;
    (*op).disp = 0;
    (*op).imm = 0;
}

#[inline(always)]
unsafe fn set_mmx(op: *mut X86Operand, reg: c_int) {
    set_xmm(op, reg & 7, 8);
    (*op).kind = OCERZ_OPK_MMX as u8;
}

#[inline(always)]
unsafe fn set_st(op: *mut X86Operand, i: c_int) {
    (*op).kind = OCERZ_OPK_ST as u8;
    (*op).reg = i as u8;
    (*op).size = 10;
    (*op).high8 = 0;
    (*op).base = OCERZ_REG_NONE as u8;
    (*op).index = OCERZ_REG_NONE as u8;
    (*op).scale = 0;
    (*op).riprel = 0;
    (*op).disp = 0;
    (*op).imm = 0;
}

#[inline(always)]
unsafe fn set_imm(op: *mut X86Operand, imm: u64, size: c_int) {
    (*op).kind = OCERZ_OPK_IMM as u8;
    (*op).reg = 0;
    (*op).size = size as u8;
    (*op).high8 = 0;
    (*op).base = OCERZ_REG_NONE as u8;
    (*op).index = OCERZ_REG_NONE as u8;
    (*op).scale = 0;
    (*op).riprel = 0;
    (*op).disp = 0;
    (*op).imm = imm;
}

#[inline(always)]
unsafe fn read_imm8s(s: &mut DecState, op: *mut X86Operand, size: c_int) -> c_int {
    let mut b = 0u8;
    let e = fetch8(s, &mut b);
    if e != 0 {
        return e;
    }
    set_imm(op, ocerz_sext(b as u64, 1) as u64, size);
    OCERZ_OK as c_int
}

#[inline(always)]
unsafe fn read_imm16(s: &mut DecState, op: *mut X86Operand) -> c_int {
    let mut w = 0u16;
    let e = fetch16(s, &mut w);
    if e != 0 {
        return e;
    }
    set_imm(op, w as u64, 2);
    OCERZ_OK as c_int
}

#[inline(always)]
unsafe fn read_imm32s(s: &mut DecState, op: *mut X86Operand, size: c_int) -> c_int {
    let mut d = 0u32;
    let e = fetch32(s, &mut d);
    if e != 0 {
        return e;
    }
    set_imm(op, ocerz_sext(d as u64, 4) as u64, size);
    OCERZ_OK as c_int
}

#[inline(always)]
unsafe fn read_imm_sized(s: &mut DecState, op: *mut X86Operand, opsize: c_int) -> c_int {
    if opsize == 2 {
        read_imm16(s, op)
    } else {
        read_imm32s(s, op, opsize)
    }
}

#[inline(always)]
unsafe fn read_imm64(s: &mut DecState, op: *mut X86Operand) -> c_int {
    let mut q = 0u64;
    let e = fetch64(s, &mut q);
    if e != 0 {
        return e;
    }
    set_imm(op, q, 8);
    OCERZ_OK as c_int
}

#[inline(always)]
unsafe fn cur_len(s: &DecState) -> c_int {
    s.p.offset_from(s.base) as c_int
}

unsafe fn decode_modrm16(s: &mut DecState, m: &mut ModRM, rm: c_int) -> c_int {
    const BASE16: [u8; 8] = [
        OCERZ_RBX as u8,
        OCERZ_RBX as u8,
        OCERZ_RBP as u8,
        OCERZ_RBP as u8,
        OCERZ_RSI as u8,
        OCERZ_RDI as u8,
        OCERZ_RBP as u8,
        OCERZ_RBX as u8,
    ];
    const INDEX16: [u8; 8] = [
        OCERZ_RSI as u8,
        OCERZ_RDI as u8,
        OCERZ_RSI as u8,
        OCERZ_RDI as u8,
        OCERZ_REG_NONE as u8,
        OCERZ_REG_NONE as u8,
        OCERZ_REG_NONE as u8,
        OCERZ_REG_NONE as u8,
    ];
    let mo = &mut m.mem;
    if m.mod_ == 0 && rm == 6 {
        let mut d16 = 0u16;
        let e = fetch16(s, &mut d16);
        if e != 0 {
            return e;
        }
        mo.disp = ocerz_sext(d16 as u64, 2);
        return OCERZ_OK as c_int;
    }
    mo.base = *BASE16.get_unchecked(rm as usize);
    mo.index = *INDEX16.get_unchecked(rm as usize);
    if m.mod_ == 1 {
        let mut d8 = 0u8;
        let e = fetch8(s, &mut d8);
        if e != 0 {
            return e;
        }
        mo.disp = ocerz_sext(d8 as u64, 1);
    } else if m.mod_ == 2 {
        let mut d16 = 0u16;
        let e = fetch16(s, &mut d16);
        if e != 0 {
            return e;
        }
        mo.disp = ocerz_sext(d16 as u64, 2);
    }
    OCERZ_OK as c_int
}

#[inline]
unsafe fn decode_modrm(s: &mut DecState, m: &mut ModRM, mem_size: c_int) -> c_int {
    let mut b = 0u8;
    let e = fetch8(s, &mut b);
    if e != 0 {
        return e;
    }
    m.mod_ = ((b >> 6) & 3) as c_int;
    m.reg = (((b >> 3) & 7) as c_int) | (if s.rex_r != 0 { 8 } else { 0 });
    let rm = (b & 7) as c_int;
    if m.mod_ == 3 {
        m.rm = rm | (if s.rex_b != 0 { 8 } else { 0 });
        m.mem.kind = OCERZ_OPK_NONE as u8;
        return OCERZ_OK as c_int;
    }
    let mo = &mut m.mem;
    mo.kind = OCERZ_OPK_MEM as u8;
    mo.reg = 0;
    mo.size = mem_size as u8;
    mo.high8 = 0;
    mo.base = OCERZ_REG_NONE as u8;
    mo.index = OCERZ_REG_NONE as u8;
    mo.scale = 0;
    mo.riprel = 0;
    mo.disp = 0;
    mo.imm = 0;
    m.rm = -1;
    if s.addr16 != 0 {
        return decode_modrm16(s, m, rm);
    }
    let mut base = -1;
    let mut has_disp8 = false;
    let mut has_disp32 = false;
    let mut riprel = false;
    if rm == 4 {
        let mut sib = 0u8;
        let e = fetch8(s, &mut sib);
        if e != 0 {
            return e;
        }
        let scale = ((sib >> 6) & 3) as c_int;
        let index = (((sib >> 3) & 7) as c_int) | (if s.rex_x != 0 { 8 } else { 0 });
        let sbase = ((sib & 7) as c_int) | (if s.rex_b != 0 { 8 } else { 0 });
        mo.scale = scale as u8;
        if ((sib >> 3) & 7) == 4 && s.rex_x == 0 {
            mo.index = OCERZ_REG_NONE as u8;
        } else {
            mo.index = index as u8;
        }
        if (sib & 7) == 5 && m.mod_ == 0 {
            mo.base = OCERZ_REG_NONE as u8;
            has_disp32 = true;
        } else {
            base = sbase;
        }
    } else if rm == 5 && m.mod_ == 0 {
        riprel = s.mode32 == 0;
        has_disp32 = true;
    } else {
        base = rm | (if s.rex_b != 0 { 8 } else { 0 });
    }
    if m.mod_ == 1 {
        has_disp8 = true;
    } else if m.mod_ == 2 {
        has_disp32 = true;
    }
    if base >= 0 {
        mo.base = base as u8;
    }
    let mut disp = 0i64;
    if has_disp8 {
        let mut d8 = 0u8;
        let e = fetch8(s, &mut d8);
        if e != 0 {
            return e;
        }
        disp = ocerz_sext(d8 as u64, 1);
    } else if has_disp32 {
        let mut d32 = 0u32;
        let e = fetch32(s, &mut d32);
        if e != 0 {
            return e;
        }
        disp = ocerz_sext(d32 as u64, 4);
    }
    if riprel {
        mo.riprel = 1;
        mo.base = OCERZ_REG_NONE as u8;
        mo.index = OCERZ_REG_NONE as u8;
    }
    mo.disp = disp;
    OCERZ_OK as c_int
}

#[inline]
unsafe fn fixup_riprel(s: &mut DecState) {
    for i in 0..(*s.out).nops as usize {
        let op = (*s.out).ops.get_unchecked_mut(i);
        if op.kind == OCERZ_OPK_MEM as u8 && op.riprel != 0 {
            op.disp = (s.rip + cur_len(s) as u64).wrapping_add(op.disp as u64) as i64;
        }
    }
}

#[inline(always)]
unsafe fn modrm_to_reg(m: &ModRM, op: *mut X86Operand, size: c_int) {
    set_reg(op, m.rm, size);
}

#[inline(always)]
unsafe fn modrm_to_xmm(m: &ModRM, op: *mut X86Operand, size: c_int) {
    set_xmm(op, m.rm, size);
}

#[inline(always)]
fn rm_is_reg(m: &ModRM) -> bool {
    m.mod_ == 3
}

#[inline(always)]
unsafe fn place_rm(m: &ModRM, op: *mut X86Operand, size: c_int, gpr: bool) {
    if rm_is_reg(m) {
        if gpr {
            modrm_to_reg(m, op, size);
        } else {
            modrm_to_xmm(m, op, size);
        }
    } else {
        ptr::copy_nonoverlapping(&m.mem, op, 1);
        (*op).size = size as u8;
    }
}

#[inline(always)]
unsafe fn place_rm8(s: &DecState, m: &ModRM, op: *mut X86Operand) {
    if rm_is_reg(m) {
        set_reg8(s, op, m.rm);
    } else {
        ptr::copy_nonoverlapping(&m.mem, op, 1);
        (*op).size = 1;
    }
}

#[inline(always)]
fn opsize_default(s: &DecState) -> c_int {
    if s.rex_w != 0 {
        8
    } else if s.has_66 != 0 {
        2
    } else {
        4
    }
}

#[inline(always)]
fn opsize_stack(s: &DecState) -> c_int {
    if s.has_66 != 0 {
        2
    } else if s.mode32 != 0 {
        4
    } else {
        8
    }
}

#[inline(always)]
fn opsize_stack_implicit(s: &DecState) -> c_int {
    if s.mode32 == 0 {
        8
    } else if s.has_66 != 0 {
        2
    } else {
        4
    }
}

#[inline(always)]
fn opsize_nearbranch(s: &DecState) -> c_int {
    if s.mode32 == 0 {
        8
    } else if s.has_66 != 0 {
        2
    } else {
        4
    }
}

#[inline(always)]
unsafe fn set_op(s: &mut DecState, op: c_int) {
    (*s.out).op = op as u16;
}

#[inline(always)]
fn alu_op_for(idx: c_int) -> c_int {
    match idx {
        0 => OCERZ_OP_ADD as c_int,
        1 => OCERZ_OP_OR as c_int,
        2 => OCERZ_OP_ADC as c_int,
        3 => OCERZ_OP_SBB as c_int,
        4 => OCERZ_OP_AND as c_int,
        5 => OCERZ_OP_SUB as c_int,
        6 => OCERZ_OP_XOR as c_int,
        _ => OCERZ_OP_CMP as c_int,
    }
}

#[inline(always)]
fn shift_op_for(idx: c_int) -> c_int {
    match idx {
        0 => OCERZ_OP_ROL as c_int,
        1 => OCERZ_OP_ROR as c_int,
        2 => OCERZ_OP_RCL as c_int,
        3 => OCERZ_OP_RCR as c_int,
        4 => OCERZ_OP_SHL as c_int,
        5 => OCERZ_OP_SHR as c_int,
        6 => OCERZ_OP_SHL as c_int,
        _ => OCERZ_OP_SAR as c_int,
    }
}

#[inline]
unsafe fn alu_rm_r(s: &mut DecState, idx: c_int, byte_form: bool, reg_is_dst: bool) -> c_int {
    let mut m: ModRM = mem::zeroed();
    let size = if byte_form { 1 } else { opsize_default(s) };
    let e = decode_modrm(s, &mut m, size);
    if e != 0 {
        return e;
    }
    let out = s.out;
    set_op(s, alu_op_for(idx));
    (*out).opsize = size as u8;
    (*out).nops = 2;
    if reg_is_dst {
        if byte_form {
            set_reg8(s, ptr::addr_of_mut!((*out).ops[0]), m.reg);
        } else {
            set_reg(ptr::addr_of_mut!((*out).ops[0]), m.reg, size);
        }
        if byte_form {
            place_rm8(s, &m, ptr::addr_of_mut!((*out).ops[1]));
        } else {
            place_rm(&m, ptr::addr_of_mut!((*out).ops[1]), size, true);
        }
    } else {
        if byte_form {
            place_rm8(s, &m, ptr::addr_of_mut!((*out).ops[0]));
        } else {
            place_rm(&m, ptr::addr_of_mut!((*out).ops[0]), size, true);
        }
        if byte_form {
            set_reg8(s, ptr::addr_of_mut!((*out).ops[1]), m.reg);
        } else {
            set_reg(ptr::addr_of_mut!((*out).ops[1]), m.reg, size);
        }
    }
    OCERZ_OK as c_int
}

unsafe fn alu_acc_imm(s: &mut DecState, idx: c_int, byte_form: bool) -> c_int {
    let size = if byte_form { 1 } else { opsize_default(s) };
    let out = s.out;
    set_op(s, alu_op_for(idx));
    (*out).opsize = size as u8;
    (*out).nops = 2;
    if byte_form {
        set_reg8(s, ptr::addr_of_mut!((*out).ops[0]), 0);
    } else {
        set_reg(ptr::addr_of_mut!((*out).ops[0]), 0, size);
    }
    if byte_form {
        read_imm8s(s, ptr::addr_of_mut!((*out).ops[1]), 1)
    } else {
        read_imm_sized(s, ptr::addr_of_mut!((*out).ops[1]), size)
    }
}

unsafe fn set_cc_from_low(s: &mut DecState, lo4: c_int) -> c_int {
    (*s.out).cc = lo4 as u8;
    OCERZ_OK as c_int
}

unsafe fn branch_rel(s: &mut DecState, op: c_int, rel_size: c_int, lo4_for_cc: c_int) -> c_int {
    let mut tmp: X86Operand = mem::zeroed();
    let osize = opsize_nearbranch(s);
    let e = if rel_size == 1 {
        read_imm8s(s, &mut tmp, 8)
    } else if osize == 2 {
        let mut w = 0u16;
        let e = fetch16(s, &mut w);
        if e == 0 {
            set_imm(&mut tmp, ocerz_sext(w as u64, 2) as u64, 2);
        }
        e
    } else {
        read_imm32s(s, &mut tmp, 8)
    };
    if e != 0 {
        return e;
    }
    set_op(s, op);
    (*s.out).opsize = osize as u8;
    if lo4_for_cc >= 0 {
        set_cc_from_low(s, lo4_for_cc);
    }
    let mut target = s.rip.wrapping_add(cur_len(s) as u64).wrapping_add(tmp.imm);
    if osize == 4 {
        target &= 0xffff_ffff;
    } else if osize == 2 {
        target &= 0xffff;
    }
    let out = s.out;
    set_imm(ptr::addr_of_mut!((*out).ops[0]), target, osize);
    (*out).nops = 1;
    OCERZ_OK as c_int
}

unsafe fn decode_vex(s: &mut DecState, op: u8) -> c_int {
    let mut p1 = 0u8;
    let mut p2 = 0u8;
    let mut e = fetch8(s, &mut p1);
    if e != 0 {
        return e;
    }
    let mut map = 1;
    let mut w = 0;
    if op == 0xc4 {
        e = fetch8(s, &mut p2);
        if e != 0 {
            return e;
        }
        s.rex_r = if (p1 >> 7) & 1 == 0 { 1 } else { 0 };
        s.rex_x = if (p1 >> 6) & 1 == 0 { 1 } else { 0 };
        s.rex_b = if (p1 >> 5) & 1 == 0 { 1 } else { 0 };
        map = (p1 & 0x1f) as c_int;
        w = ((p2 >> 7) & 1) as c_int;
    } else {
        s.rex_r = if (p1 >> 7) & 1 == 0 { 1 } else { 0 };
        s.rex_x = 0;
        s.rex_b = 0;
        p2 = p1;
    }
    s.rex_present = 1;
    s.rex_w = w;
    s.vex = 1;
    s.vex_w = w;
    s.vex_l = ((p2 >> 2) & 1) as c_int;
    s.vex_vvvv = (((!p2) >> 3) & 0xf) as c_int;
    let pp = p2 & 3;
    s.mand = match pp {
        0 => MAND_NONE,
        1 => MAND_66,
        2 => MAND_F3,
        _ => MAND_F2,
    };
    s.has_66 = (pp == 1) as c_int;
    s.rep = match pp {
        2 => OCERZ_REP_REP as c_int,
        3 => OCERZ_REP_REPNE as c_int,
        _ => OCERZ_REP_NONE as c_int,
    };
    let r = match map {
        1 => {
            let mut op2 = 0u8;
            e = fetch8(s, &mut op2);
            if e != 0 {
                return e;
            }
            map0f::decode_0f(s, op2)
        }
        2 => map0f38::decode_0f38(s),
        3 => map0f38::decode_0f3a(s),
        _ => return OCERZ_EUNDEF as c_int,
    };
    if r != OCERZ_OK as c_int {
        return r;
    }
    vex_finish(s)
}

unsafe fn vex_finish(s: &mut DecState) -> c_int {
    let o = s.out;
    (*o).vex = (OCERZ_VEX_PRESENT
        | if s.vex_l != 0 { OCERZ_VEX_L } else { 0 }
        | if s.vex_w != 0 { OCERZ_VEX_W } else { 0 }) as u8;
    (*o).vvvv = s.vex_vvvv as u8;
    match (*o).op as u32 {
        OCERZ_OP_ADDPS | OCERZ_OP_ADDPD | OCERZ_OP_ADDSS | OCERZ_OP_ADDSD | OCERZ_OP_SUBPS
        | OCERZ_OP_SUBPD | OCERZ_OP_SUBSS | OCERZ_OP_SUBSD | OCERZ_OP_MULPS | OCERZ_OP_MULPD
        | OCERZ_OP_MULSS | OCERZ_OP_MULSD | OCERZ_OP_DIVPS | OCERZ_OP_DIVPD | OCERZ_OP_DIVSS
        | OCERZ_OP_DIVSD | OCERZ_OP_MINPS | OCERZ_OP_MINPD | OCERZ_OP_MINSS | OCERZ_OP_MINSD
        | OCERZ_OP_MAXPS | OCERZ_OP_MAXPD | OCERZ_OP_MAXSS | OCERZ_OP_MAXSD | OCERZ_OP_SQRTSS
        | OCERZ_OP_SQRTSD | OCERZ_OP_RSQRTSS | OCERZ_OP_RCPSS | OCERZ_OP_HADDPS
        | OCERZ_OP_HADDPD | OCERZ_OP_HSUBPS | OCERZ_OP_HSUBPD | OCERZ_OP_ADDSUBPS
        | OCERZ_OP_ADDSUBPD | OCERZ_OP_ANDPS | OCERZ_OP_ANDNPS | OCERZ_OP_ORPS | OCERZ_OP_XORPS
        | OCERZ_OP_PAND | OCERZ_OP_PANDN | OCERZ_OP_POR | OCERZ_OP_PXOR | OCERZ_OP_CMPPS
        | OCERZ_OP_CMPPD | OCERZ_OP_CMPSS | OCERZ_OP_CMPSDX | OCERZ_OP_PCMPEQB
        | OCERZ_OP_PCMPEQW | OCERZ_OP_PCMPEQD | OCERZ_OP_PCMPEQQ | OCERZ_OP_PCMPGTB
        | OCERZ_OP_PCMPGTW | OCERZ_OP_PCMPGTD | OCERZ_OP_PCMPGTQ | OCERZ_OP_CVTSI2SS
        | OCERZ_OP_CVTSI2SD | OCERZ_OP_CVTSS2SD | OCERZ_OP_CVTSD2SS | OCERZ_OP_PADDB
        | OCERZ_OP_PADDW | OCERZ_OP_PADDD | OCERZ_OP_PADDQ | OCERZ_OP_PSUBB | OCERZ_OP_PSUBW
        | OCERZ_OP_PSUBD | OCERZ_OP_PSUBQ | OCERZ_OP_PADDSB | OCERZ_OP_PADDSW
        | OCERZ_OP_PADDUSB | OCERZ_OP_PADDUSW | OCERZ_OP_PSUBSB | OCERZ_OP_PSUBSW
        | OCERZ_OP_PSUBUSB | OCERZ_OP_PSUBUSW | OCERZ_OP_PMULLW | OCERZ_OP_PMULLD
        | OCERZ_OP_PMULHW | OCERZ_OP_PMULHUW | OCERZ_OP_PMULUDQ | OCERZ_OP_PMULDQ
        | OCERZ_OP_PMADDWD | OCERZ_OP_PAVGB | OCERZ_OP_PAVGW | OCERZ_OP_PMAXUB
        | OCERZ_OP_PMAXSW | OCERZ_OP_PMINUB | OCERZ_OP_PMINSW | OCERZ_OP_PMAXSB
        | OCERZ_OP_PMAXSD | OCERZ_OP_PMAXUW | OCERZ_OP_PMAXUD | OCERZ_OP_PMINSB
        | OCERZ_OP_PMINSD | OCERZ_OP_PMINUW | OCERZ_OP_PMINUD | OCERZ_OP_PSADBW
        | OCERZ_OP_PHADDW | OCERZ_OP_PHADDD | OCERZ_OP_PHADDSW | OCERZ_OP_PHSUBW
        | OCERZ_OP_PHSUBD | OCERZ_OP_PHSUBSW | OCERZ_OP_PSIGNB | OCERZ_OP_PSIGNW
        | OCERZ_OP_PSIGND | OCERZ_OP_PMADDUBSW | OCERZ_OP_PMULHRSW | OCERZ_OP_PACKSSWB
        | OCERZ_OP_PACKSSDW | OCERZ_OP_PACKUSWB | OCERZ_OP_PACKUSDW | OCERZ_OP_PUNPCKLBW
        | OCERZ_OP_PUNPCKLWD | OCERZ_OP_PUNPCKLDQ | OCERZ_OP_PUNPCKLQDQ | OCERZ_OP_PUNPCKHBW
        | OCERZ_OP_PUNPCKHWD | OCERZ_OP_PUNPCKHDQ | OCERZ_OP_PUNPCKHQDQ | OCERZ_OP_PSHUFB
        | OCERZ_OP_PALIGNR | OCERZ_OP_SHUFPS | OCERZ_OP_SHUFPD | OCERZ_OP_UNPCKLPS
        | OCERZ_OP_UNPCKHPS | OCERZ_OP_UNPCKLPD | OCERZ_OP_UNPCKHPD | OCERZ_OP_PINSRB
        | OCERZ_OP_PINSRW | OCERZ_OP_PINSRD | OCERZ_OP_PINSRQ | OCERZ_OP_INSERTPS
        | OCERZ_OP_PBLENDW | OCERZ_OP_BLENDPS | OCERZ_OP_BLENDPD | OCERZ_OP_ROUNDSS
        | OCERZ_OP_ROUNDSD | OCERZ_OP_DPPS | OCERZ_OP_DPPD | OCERZ_OP_MPSADBW
        | OCERZ_OP_PCLMULQDQ | OCERZ_OP_AESENC | OCERZ_OP_AESENCLAST | OCERZ_OP_AESDEC
        | OCERZ_OP_AESDECLAST | OCERZ_OP_MOVLHPS | OCERZ_OP_MOVHLPS | OCERZ_OP_VINSERTF128
        | OCERZ_OP_VPERM2F128 | OCERZ_OP_VINSERTI128 | OCERZ_OP_VPERM2I128 | OCERZ_OP_VPBLENDD
        | OCERZ_OP_VPERMD | OCERZ_OP_VPERMPS | OCERZ_OP_VPSLLVD | OCERZ_OP_VPSLLVQ
        | OCERZ_OP_VPSRLVD | OCERZ_OP_VPSRLVQ | OCERZ_OP_VPSRAVD | OCERZ_OP_VPMASKMOVD
        | OCERZ_OP_VPMASKMOVQ | OCERZ_OP_VMASKMOVPS | OCERZ_OP_VMASKMOVPD => {
            (*o).vex |= OCERZ_VEX_NDS as u8
        }
        OCERZ_OP_VPERMILPS | OCERZ_OP_VPERMILPD => {
            if (*o).ops[2].kind != OCERZ_OPK_IMM as u8 {
                (*o).vex |= OCERZ_VEX_NDS as u8;
            }
        }
        OCERZ_OP_MOVSS | OCERZ_OP_MOVSDX => {
            if (*o).ops[0].kind == OCERZ_OPK_XMM as u8 && (*o).ops[1].kind == OCERZ_OPK_XMM as u8 {
                (*o).vex |= OCERZ_VEX_NDS as u8;
            }
        }
        OCERZ_OP_MOVLPS | OCERZ_OP_MOVHPS => {
            if (*o).ops[0].kind == OCERZ_OPK_XMM as u8 {
                (*o).vex |= OCERZ_VEX_NDS as u8;
            }
        }
        OCERZ_OP_PSLLW | OCERZ_OP_PSLLD | OCERZ_OP_PSLLQ | OCERZ_OP_PSRLW | OCERZ_OP_PSRLD
        | OCERZ_OP_PSRLQ | OCERZ_OP_PSRAW | OCERZ_OP_PSRAD | OCERZ_OP_PSLLDQ | OCERZ_OP_PSRLDQ => {
            if (*o).ops[1].kind == OCERZ_OPK_IMM as u8 {
                (*o).ops[2] = (*o).ops[0];
                set_xmm(ptr::addr_of_mut!((*o).ops[0]), s.vex_vvvv, 16);
                (*o).nops = 3;
                (*o).vex |= OCERZ_VEX_NDD as u8;
            } else {
                (*o).vex |= OCERZ_VEX_NDS as u8;
            }
        }
        OCERZ_OP_BLENDVPS | OCERZ_OP_BLENDVPD | OCERZ_OP_PBLENDVB => {
            (*o).vex |= (OCERZ_VEX_NDS | OCERZ_VEX_IS4) as u8;
        }
        OCERZ_OP_VPBROADCASTB => {
            if (*o).ops[1].kind == OCERZ_OPK_MEM as u8 {
                (*o).ops[1].size = 1;
            }
        }
        OCERZ_OP_VPBROADCASTW => {
            if (*o).ops[1].kind == OCERZ_OPK_MEM as u8 {
                (*o).ops[1].size = 2;
            }
        }
        OCERZ_OP_VBROADCASTSS | OCERZ_OP_VPBROADCASTD => {
            if (*o).ops[1].kind == OCERZ_OPK_MEM as u8 {
                (*o).ops[1].size = 4;
            }
        }
        OCERZ_OP_VBROADCASTSD | OCERZ_OP_VPBROADCASTQ => {
            if (*o).ops[1].kind == OCERZ_OPK_MEM as u8 {
                (*o).ops[1].size = 8;
            }
        }
        OCERZ_OP_VCVTPH2PS => {
            if (*o).ops[1].kind == OCERZ_OPK_MEM as u8 {
                (*o).ops[1].size = if s.vex_l != 0 { 16 } else { 8 };
            }
        }
        OCERZ_OP_VCVTPS2PH => {
            if (*o).ops[0].kind == OCERZ_OPK_MEM as u8 {
                (*o).ops[0].size = if s.vex_l != 0 { 16 } else { 8 };
            }
        }
        _ => {
            if ((*o).op as u32) >= OCERZ_OP_VFMA_FIRST && ((*o).op as u32) <= OCERZ_OP_VFMA_LAST {
                (*o).vex |= OCERZ_VEX_NDS as u8;
            }
        }
    }
    OCERZ_OK as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_decode_mode(
    code: *const u8,
    avail: usize,
    rip: u64,
    out: *mut X86Insn,
    mode32: c_int,
) -> c_int {
    if code.is_null() {
        static SAID: AtomicBool = AtomicBool::new(false);
        if !SAID.swap(true, Ordering::Relaxed) {
            libc::fprintf(
                crate::log::stderr(),
                b"ocerz: DECODE-NULL[%d] rip=%#llx mode32=%d avail=%zu low_base=%#llx top_base=%#llx guest_base=%#llx\n\0"
                    .as_ptr() as *const c_char,
                libc::getpid(),
                rip as libc::c_ulonglong,
                mode32,
                avail,
                crate::ffi::ocerz_low_base as libc::c_ulonglong,
                crate::ffi::ocerz_top_base as libc::c_ulonglong,
                crate::ffi::ocerz_guest_base as libc::c_ulonglong,
            );
        }
        return OCERZ_ETRUNC as c_int;
    }
    let mut s = DecState {
        mode32: if mode32 != 0 { 1 } else { 0 },
        addr16: 0,
        base: code,
        p: code,
        avail,
        end: code.add(avail.min(16)),
        rip,
        has_66: 0,
        has_67: 0,
        has_f0: 0,
        rep: OCERZ_REP_NONE as c_int,
        mand: MAND_NONE,
        seg: OCERZ_SEG_NONE as c_int,
        rex: 0,
        rex_present: 0,
        rex_w: 0,
        rex_r: 0,
        rex_x: 0,
        rex_b: 0,
        vex: 0,
        vex_l: 0,
        vex_w: 0,
        vex_vvvv: 0,
        out,
    };
    ptr::write_bytes(out, 0, 1);
    (*out).rip = rip;
    (*out).mode32 = s.mode32 as u8;
    for i in 0..3 {
        (*out).ops[i].base = OCERZ_REG_NONE as u8;
        (*out).ops[i].index = OCERZ_REG_NONE as u8;
    }
    let mut last_f23 = 0;
    if s.p >= s.end {
        return if avail >= 16 {
            OCERZ_ETOOLONG as c_int
        } else {
            OCERZ_ETRUNC as c_int
        };
    }
    let first = *s.p;
    let first_class = *PFX.get_unchecked(first as usize);
    let mut op = 0u8;
    let e;
    if first_class == 0 || (first_class == 1 && s.mode32 != 0) {
        op = first;
        s.p = s.p.add(1);
        e = OCERZ_OK as c_int;
    } else {
        loop {
            if s.p >= s.end {
                return if avail >= 16 {
                    OCERZ_ETOOLONG as c_int
                } else {
                    OCERZ_ETRUNC as c_int
                };
            }
            let b = *s.p;
            let class = *PFX.get_unchecked(b as usize);
            if class == 0 || (class == 1 && s.mode32 != 0) {
                break;
            }
            if class == 1 {
                s.rex_present = 1;
                s.rex = b as c_int;
                s.rex_w = ((b >> 3) & 1) as c_int;
                s.rex_r = ((b >> 2) & 1) as c_int;
                s.rex_x = ((b >> 1) & 1) as c_int;
                s.rex_b = (b & 1) as c_int;
                s.p = s.p.add(1);
                continue;
            }
            match class {
                2 => {
                    s.has_66 = 1;
                    if last_f23 == 0 {
                        s.mand = MAND_66;
                    }
                }
                3 => s.has_67 = 1,
                4 => s.has_f0 = 1,
                5 => {
                    s.rep = OCERZ_REP_REPNE as c_int;
                    s.mand = MAND_F2;
                    last_f23 = 1;
                }
                6 => {
                    s.rep = OCERZ_REP_REP as c_int;
                    s.mand = MAND_F3;
                    last_f23 = 1;
                }
                7 => s.seg = OCERZ_SEG_NONE as c_int,
                8 => s.seg = OCERZ_SEG_FS as c_int,
                9 => s.seg = OCERZ_SEG_GS as c_int,
                _ => break,
            }
            s.rex_present = 0;
            s.rex_w = 0;
            s.rex_r = 0;
            s.rex_x = 0;
            s.rex_b = 0;
            s.p = s.p.add(1);
        }
        e = fetch8(&mut s, &mut op);
    }
    (*out).addrsize = if s.mode32 != 0 {
        if s.has_67 != 0 { 2 } else { 4 }
    } else if s.has_67 != 0 {
        4
    } else {
        8
    };
    s.addr16 = ((*out).addrsize == 2) as c_int;
    (*out).seg = s.seg as u8;
    (*out).lock = (s.has_f0 != 0) as u8;
    (*out).rep = OCERZ_REP_NONE as u8;
    let r = if e != 0 {
        e
    } else if s.mode32 == 0 && (op == 0xc4 || op == 0xc5) {
        decode_vex(&mut s, op)
    } else if op == 0x0f {
        let mut op2 = 0u8;
        let e = fetch8(&mut s, &mut op2);
        if e != 0 {
            e
        } else if op2 == 0x38 {
            map0f38::decode_0f38(&mut s)
        } else if op2 == 0x3a {
            map0f38::decode_0f3a(&mut s)
        } else {
            map0f::decode_0f(&mut s, op2)
        }
    } else if (0xd8..=0xdf).contains(&op) {
        x87::decode_x87(&mut s, op)
    } else {
        onebyte::decode_one_byte(&mut s, op)
    };
    if r == OCERZ_ETRUNC as c_int && avail >= 16 {
        return OCERZ_ETOOLONG as c_int;
    }
    if r != OCERZ_OK as c_int {
        return r;
    }
    fixup_riprel(&mut s);
    let len = cur_len(&s);
    if len > 15 {
        return OCERZ_ETOOLONG as c_int;
    }
    (*out).len = len as u8;
    OCERZ_OK as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_decode(
    code: *const u8,
    avail: usize,
    rip: u64,
    out: *mut X86Insn,
) -> c_int {
    ocerz_decode_mode(code, avail, rip, out, 0)
}

//! ---- register pinning ----
//! Pin class 3 keeps every guest GPR permanently in a host register (slot ==
//! guest register number): slots 0-7 in x21-x28 (callee-saved), 8-13 in x3-x8,
//! 14-15 in x1-x2 (caller-saved, spilled and reloaded around every C callout).
//! All blocks share that layout, so every transition is body-to-body: chaining
//! enters the callee's body directly, RAS entries are plain body pointers, and
//! no block boundary pays frame traffic.  xmm0-15 live in V16-V31 for the whole
//! body; only function entry/exit and C callouts touch memory.
//!
//! Class 3 also keeps guest rsp as guest_base + rsp - a host pointer, not a
//! value - so push/pop are single pre/post-indexed accesses and rsp-relative
//! addresses drop the base add.  Reads and writes of the rsp VALUE convert at
//! the accessors; spill/fill and fault recovery convert at the boundaries.
//! OCERZ_RSP_VALUE restores the old value-keeping class 3.  A read-modify-write
//! on an rsp-relative operand takes its address the same way.  It used to go to
//! the interpreter, which in MSVC-built code - locals addressed off rsp, and
//! lock or [rsp], 0 as the memory barrier - made add, cmp and or on memory the
//! hottest interpreted forms on R.E.P.O.'s main thread.
//!
//!
//! The slot is rsp - 8 plus the stack delta; rsp moves only once the store is done.
//!
//! In the Wine layout x0 carries nothing a 32-bit block needs (no guest base,
//! and the stack delta is for 64-bit stacks), so a 32-bit block keeps low_base
//! there, reloaded wherever the stack delta would be, and a 32-bit stack slot is
//! one register-offset access, [x0, w, uxtw], instead of a mov, an orr and the
//! access.  OCERZ_NO_M32_LOWREG=1 goes back to the orr.
//!
//! setcc r8 whose register a movzx of the same register widens a few
//! instructions on (setg al ; setl dl ; movzx edx, dl ; movzx eax, al): with
//! nothing between that reads or writes the register, touches memory or can
//! leave the block, the setcc writes the whole register (cset zero-extends)
//! and the movzx is skipped.  Returns the movzx's index, or -1.

#![allow(unsafe_op_in_unsafe_fn)]
#![allow(clippy::missing_safety_doc)]

use core::ffi::{c_int, c_uint};
use core::ptr::{null, null_mut};

use crate::ffi::*;
use crate::jit_internal::*;
use crate::inline::{ocerz_cc_pack, ocerz_mask};

const JT0: c_int = crate::ffi::JT0 as c_int;
const JT1: c_int = crate::ffi::JT1 as c_int;
const JT2: c_int = crate::ffi::JT2 as c_int;
const JTF: c_int = crate::ffi::JTF as c_int;
const JTT: c_int = crate::ffi::JTT as c_int;
const JTU: c_int = crate::ffi::JTU as c_int;
const JTA: c_int = crate::ffi::JTA as c_int;
const JGB: c_int = crate::ffi::JGB as c_int;
const JRET_GUEST: c_int = crate::ffi::JRET_GUEST as c_int;
const JRET_HOST: c_int = crate::ffi::JRET_HOST as c_int;
const VX0: c_int = crate::ffi::VX0 as c_int;
const A64_ZR: c_int = crate::ffi::A64_ZR as c_int;

const A64_EQ: c_int = crate::ffi::A64_EQ as c_int;
const A64_NE: c_int = crate::ffi::A64_NE as c_int;
const A64_CS: c_int = crate::ffi::A64_CS as c_int;
const A64_CC: c_int = crate::ffi::A64_CC as c_int;
const A64_VS: c_int = crate::ffi::A64_VS as c_int;
const A64_AL: c_int = crate::ffi::A64_AL as c_int;
const A64_NV: c_int = crate::ffi::A64_NV as c_int;

const OCERZ_CF: u64 = crate::inline::OCERZ_CF;
const OCERZ_PF: u64 = crate::inline::OCERZ_PF;
const OCERZ_AF: u64 = crate::inline::OCERZ_AF;
const OCERZ_ZF: u64 = crate::inline::OCERZ_ZF;
const OCERZ_SF: u64 = crate::inline::OCERZ_SF;
const OCERZ_OF: u64 = crate::inline::OCERZ_OF;

const KREG: u8 = OCERZ_OPK_REG as u8;
const KIMM: u8 = OCERZ_OPK_IMM as u8;
const KMEM: u8 = OCERZ_OPK_MEM as u8;
const SEG_NONE: u8 = OCERZ_SEG_NONE as u8;



macro_rules! env_on {
    ($name:literal) => {{
        static mut ON_: c_int = -1;
        if ON_ < 0 {
            ON_ = (!libc::getenv(concat!($name, "\0").as_ptr().cast()).is_null()) as c_int;
        }
        ON_ != 0
    }};
}

#[inline(always)]
fn gpr_off(r: c_uint) -> u32 {
    r.wrapping_mul(8)
}

#[inline(always)]
unsafe fn getenv_set(name: &core::ffi::CStr) -> bool {
    !libc::getenv(name.as_ptr()).is_null()
}

#[inline(always)]
fn c_assert(c: bool) {
    if !c {
        unsafe { libc::abort() }
    }
}

#[inline(always)]
fn cc_pack(kind: c_uint, size: u8, cin: c_int) -> u32 {
    ocerz_cc_pack(kind, size as i32, cin)
}
#[inline(always)]
unsafe fn mov_sink_at(i: c_int) -> i16 {
    *(&raw const g_mov_sink_at).cast::<i16>().add(i as usize)
}

#[inline(always)]
unsafe fn cur_insn(i: c_int) -> &'static X86Insn {
    &*g_cur_insns_fwd().add(i as usize)
}

#[inline(always)]
fn sz(sf: c_int) -> c_int {
    if sf != 0 { 8 } else { 4 }
}

#[unsafe(no_mangle)]
pub static mut g_no_lazyflags: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_nzcv_want: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_nzcv_from: c_int = -1;
#[unsafe(no_mangle)]
pub static mut g_nzcv_kind: c_uint = 0;
#[unsafe(no_mangle)]
pub static mut g_no_addincfuse: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_m32low: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_pin: *mut i8 = null_mut();
#[unsafe(no_mangle)]
pub static mut g_pin_hold: *mut u8 = null_mut();
#[unsafe(no_mangle)]
pub static mut g_n_pinned: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_pin_class: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_defer: c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_mov_sink_at: [i16; JIT_MAX_BLOCK_INSNS as usize] = [0; JIT_MAX_BLOCK_INSNS as usize];
#[unsafe(no_mangle)]
pub static mut g_mov_skip: [u8; JIT_MAX_BLOCK_INSNS as usize] = [0; JIT_MAX_BLOCK_INSNS as usize];
#[unsafe(no_mangle)]
pub static mut g_cur_insn_start: *const u32 = null();

#[unsafe(no_mangle)]
pub unsafe extern "C" fn fullpin_enabled() -> c_int {
    static mut ON: c_int = -1;
    if ON < 0 {
        ON = if getenv_set(c"OCERZ_NO_FULLPIN") { 0 } else { 1 };
    }
    ON
}

#[inline]
unsafe fn emit_gpr_rd_sw(b: *mut A64Buf, dst: c_int, greg: c_uint) {
    let s = pin_slot(greg);
    if s >= 0 {
        a64_sxtw(b, dst, pin_hreg(s));
    } else {
        a64_ldrsw(b, dst, 20, gpr_off(greg));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_pin_prologue(b: *mut A64Buf) {
    let ns = pin_saved_count();
    let mut d = 8;
    while d < 16 {
        a64_stp_d_pre(b, d, d + 1, 31, -16);
        d += 2;
    }
    let mut i = 0;
    while i < ns {
        a64_stp_pre(b, 21 + i, 21 + i + 1, 31, -16);
        i += 2;
    }
    let mut i = 0;
    while i < g_n_pinned {
        a64_ldr(b, 8, pin_hreg(i), 20, gpr_off(*g_pin_hold.add(i as usize) as c_uint));
        i += 1;
    }
    if g_pin_class == 2 {
        a64_stp_pre(b, JRET_GUEST, JRET_HOST, 31, -16);
        a64_mov_imm64(b, JRET_HOST, 0);
    }
}

#[inline]
unsafe fn emit_load_operand(b: *mut A64Buf, op: &X86Operand, sf: c_int, dst: c_int) -> c_int {
    if op.kind == KREG {
        if op.high8 != 0 {
            return 0;
        }
        emit_gpr_rd(b, sf, dst, op.reg as c_uint);
        return 1;
    }
    if op.kind == KIMM {
        let mut v = op.imm;
        if sf == 0 {
            v &= 0xffffffff;
        }
        a64_mov_imm64(b, dst, v);
        return 1;
    }
    0
}

unsafe fn fuse_prev_mov(b: *mut A64Buf, dreg: c_uint, size: c_int, hs_out: *mut c_int, cur: *const X86Insn) -> c_int {
    static mut DIS: c_int = -1;
    if DIS < 0 {
        DIS = if getenv_set(c"OCERZ_NO_MOVFUSE") { 1 } else { 0 };
    }
    if DIS != 0 || g_cur_insns_fwd().is_null() || g_cur_insn_idx < 1 {
        return -1;
    }
    if !cur.is_null() {
        let cur = &*cur;
        let mut oi = 1usize;
        while oi < cur.nops as usize {
            let o = cur.ops.get_unchecked(oi);
            if o.kind == KREG && o.high8 == 0 && o.reg as c_uint == dreg {
                return -1;
            }
            oi += 1;
        }
    }
    if g_cur_insn_start.is_null() || (*b).p as *const u32 != g_cur_insn_start {
        return -1;
    }
    let p = cur_insn(g_cur_insn_idx - 1);
    if p.op as c_uint != OCERZ_OP_MOV || p.nops != 2 {
        return -1;
    }
    let pd = &p.ops[0];
    let ps = &p.ops[1];
    if pd.kind != KREG || ps.kind != KREG || pd.high8 != 0 || ps.high8 != 0 {
        return -1;
    }
    if pd.reg as c_uint != dreg || pd.size as c_int != size || ps.size as c_int != size || (size != 4 && size != 8) {
        return -1;
    }
    if ps.reg as c_uint == dreg {
        return -1;
    }
    if pin_slot(dreg) < 0 || pin_slot(ps.reg as c_uint) < 0 {
        return -1;
    }
    if (rsp_is_ptr() != 0) && (dreg == OCERZ_RSP || ps.reg as c_uint == OCERZ_RSP) {
        return -1;
    }
    let hd = pin_hreg(pin_slot(dreg));
    let hs = pin_hreg(pin_slot(ps.reg as c_uint));
    let want = (if size == 8 { 0xaa0003e0u32 } else { 0x2a0003e0u32 }) | ((hs as u32) << 16) | hd as u32;
    if (*b).p <= (*b).start.wrapping_add(1) || *(*b).p.sub(1) != want {
        return -1;
    }
    (*b).p = (*b).p.sub(1);
    *hs_out = hs;
    hd
}

unsafe fn emit_arith_eager(b: *mut A64Buf, insn: &X86Insn, need: u64) -> c_int {
    let d = &insn.ops[0];
    let s = &insn.ops[1];
    if d.kind != KREG || d.high8 != 0 || (d.size != 4 && d.size != 8) {
        return 0;
    }
    if s.size != d.size {
        return 0;
    }
    let sf = (d.size == 8) as c_int;

    a64_ldr(b, sz(sf), JT0, 20, gpr_off(d.reg as c_uint));
    if emit_load_operand(b, s, sf, JT1) == 0 {
        return 0;
    }

    let op = insn.op as c_uint;
    let is_sub = op == OCERZ_OP_SUB || op == OCERZ_OP_CMP;
    let is_add = op == OCERZ_OP_ADD;
    let is_logic = op == OCERZ_OP_AND || op == OCERZ_OP_OR || op == OCERZ_OP_XOR || op == OCERZ_OP_TEST;
    let writes = op == OCERZ_OP_ADD || op == OCERZ_OP_SUB || op == OCERZ_OP_AND || op == OCERZ_OP_OR || op == OCERZ_OP_XOR;

    match op {
        OCERZ_OP_ADD => a64_adds_reg(b, sf, JT2, JT0, JT1, 0),
        OCERZ_OP_SUB | OCERZ_OP_CMP => a64_subs_reg(b, sf, JT2, JT0, JT1, 0),
        OCERZ_OP_AND | OCERZ_OP_TEST => a64_ands_reg(b, sf, JT2, JT0, JT1, 0),
        OCERZ_OP_OR => a64_orr_reg(b, sf, JT2, JT0, JT1, 0),
        OCERZ_OP_XOR => a64_eor_reg(b, sf, JT2, JT0, JT1, 0),
        _ => return 0,
    }
    if need & (OCERZ_ZF | OCERZ_SF) != 0 && is_logic && op != OCERZ_OP_AND && op != OCERZ_OP_TEST {
        a64_subs_imm(b, sf, A64_ZR, JT2, 0);
    }

    if writes {
        a64_str(b, 8, JT2, 20, gpr_off(d.reg as c_uint));
    }

    if need == 0 {
        return 1;
    }

    a64_mov_imm64(b, JTF, 0);
    emit_zf_sf(b, need);
    if need & OCERZ_PF != 0 {
        emit_pf(b, JT2);
    }
    if is_add || is_sub {
        if need & OCERZ_CF != 0 {
            a64_cset(b, JTT, if is_add { A64_CS } else { A64_CC });
            a64_lsl_imm(b, 0, JTT, JTT, 0);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
        if need & OCERZ_OF != 0 {
            a64_cset(b, JTT, A64_VS);
            a64_lsl_imm(b, 0, JTT, JTT, 11);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
        if need & OCERZ_AF != 0 {
            a64_eor_reg(b, 1, JTT, JT0, JT1, 0);
            a64_eor_reg(b, 1, JTT, JTT, JT2, 0);
            a64_ubfx(b, 1, JTT, JTT, 4, 1);
            a64_lsl_imm(b, 0, JTT, JTT, 4);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
    }
    emit_commit_flags(b, need);
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_arith(b: *mut A64Buf, insn: *const X86Insn, need: u64) -> c_int {
    let insn = &*insn;
    if g_defer == 0 {
        return emit_arith_eager(b, insn, need);
    }
    let d = &insn.ops[0];
    let s = &insn.ops[1];
    if d.kind != KREG || d.high8 != 0 || (d.size != 4 && d.size != 8) {
        return 0;
    }
    if s.size != d.size {
        return 0;
    }
    let sf = (d.size == 8) as c_int;
    let op = insn.op as c_uint;
    let is_sub = op == OCERZ_OP_SUB || op == OCERZ_OP_CMP;
    let is_add = op == OCERZ_OP_ADD;
    let is_logic = op == OCERZ_OP_AND || op == OCERZ_OP_OR || op == OCERZ_OP_XOR || op == OCERZ_OP_TEST;
    let writes = op == OCERZ_OP_ADD || op == OCERZ_OP_SUB || op == OCERZ_OP_AND || op == OCERZ_OP_OR || op == OCERZ_OP_XOR;
    let dreg = d.reg as c_uint;
    let sreg = s.reg as c_uint;

    if (rsp_is_ptr() != 0)
        && dreg == OCERZ_RSP
        && sf != 0
        && need == 0
        && (op == OCERZ_OP_ADD || op == OCERZ_OP_SUB)
        && s.kind == KIMM
        && s.imm <= 4095
        && pin_slot(OCERZ_RSP) >= 0
    {
        let hs = pin_hreg(pin_slot(OCERZ_RSP));
        if op == OCERZ_OP_ADD {
            a64_add_imm(b, 1, hs, hs, s.imm as u32);
        } else {
            a64_sub_imm(b, 1, hs, hs, s.imm as u32);
        }
        return 1;
    }

    if g_nzcv_want != 0
        && pin_slot(dreg) >= 0
        && !((rsp_is_ptr() != 0) && (dreg == OCERZ_RSP || (s.kind == KREG && sreg == OCERZ_RSP)))
        && (s.kind == KIMM || (s.kind == KREG && s.high8 == 0 && pin_slot(sreg) >= 0))
    {
        let rd = pin_hreg(pin_slot(dreg));
        let rm;
        let mut v: u64 = 0;
        let imm = s.kind == KIMM;
        if imm {
            v = s.imm;
            if sf == 0 {
                v &= 0xffffffff;
            }
        }
        if imm && (is_add || is_sub) && need == 0 && v <= 4095 {
            let rdst = if writes { rd } else { A64_ZR };
            if is_add {
                a64_adds_imm(b, sf, rdst, rd, v as u32);
            } else {
                a64_subs_imm(b, sf, rdst, rd, v as u32);
            }
            g_nzcv_kind = if is_add { OCERZ_CC_ADD } else { OCERZ_CC_SUB };
            g_nzcv_from = g_cur_insn_idx;
            return 1;
        }
        {
            let neg = 0u64.wrapping_sub(v) & (if sf != 0 { u64::MAX } else { 0xffffffff });
            if imm && (is_add || is_sub) && need == 0 && neg >= 1 && neg <= 4095 {
                let rdst = if writes { rd } else { A64_ZR };
                if is_add {
                    a64_subs_imm(b, sf, rdst, rd, neg as u32);
                } else {
                    a64_adds_imm(b, sf, rdst, rd, neg as u32);
                }
                g_nzcv_kind = if is_add { OCERZ_CC_ADD } else { OCERZ_CC_SUB };
                g_nzcv_from = g_cur_insn_idx;
                return 1;
            }
        }
        if imm {
            a64_mov_imm64(b, JT1, v);
            rm = JT1;
        } else {
            rm = pin_hreg(pin_slot(sreg));
        }
        if is_add || is_sub {
            if need != 0 {
                if sf != 0 {
                    a64_stp_off(b, rd, rm, 20, CC_SRC_OFF as _);
                } else {
                    a64_mov_reg(b, 0, JT0, rd);
                    a64_mov_reg(b, 0, JT2, rm);
                    a64_stp_off(b, JT0, JT2, 20, CC_SRC_OFF as _);
                }
                a64_mov_imm64(b, JTT, cc_pack(if is_add { OCERZ_CC_ADD } else { OCERZ_CC_SUB }, d.size, 0) as u64);
                a64_str(b, 4, JTT, 20, CC_OP_OFF);
            }
            let rdst = if writes { rd } else { A64_ZR };
            if is_add {
                a64_adds_reg(b, sf, rdst, rd, rm, 0);
            } else {
                a64_subs_reg(b, sf, rdst, rd, rm, 0);
            }
            g_nzcv_kind = if is_add { OCERZ_CC_ADD } else { OCERZ_CC_SUB };
        } else {
            match op {
                OCERZ_OP_TEST => a64_ands_reg(b, sf, JT2, rd, rm, 0),
                OCERZ_OP_AND => a64_ands_reg(b, sf, rd, rd, rm, 0),
                OCERZ_OP_OR => {
                    a64_orr_reg(b, sf, rd, rd, rm, 0);
                    a64_ands_reg(b, sf, A64_ZR, rd, rd, 0);
                }
                _ => {
                    a64_eor_reg(b, sf, rd, rd, rm, 0);
                    a64_ands_reg(b, sf, A64_ZR, rd, rd, 0);
                }
            }
            if need != 0 {
                let rr = if op == OCERZ_OP_TEST { JT2 } else { rd };
                emit_defer_flags(b, cc_pack(OCERZ_CC_LOGIC, d.size, 0), rr, rr);
            }
            g_nzcv_kind = OCERZ_CC_LOGIC;
        }
        g_nzcv_from = g_cur_insn_idx;
        return 1;
    }

    if !writes && need == 0 {
        return 1;
    }

    if writes
        && need != 0
        && pin_slot(dreg) >= 0
        && (is_add || is_sub || is_logic)
        && !((rsp_is_ptr() != 0) && (dreg == OCERZ_RSP || (s.kind == KREG && sreg == OCERZ_RSP)))
        && (s.kind == KIMM || (s.kind == KREG && s.high8 == 0))
    {
        let rd = pin_hreg(pin_slot(dreg));
        let rm;
        if s.kind == KREG {
            let ss = pin_slot(sreg);
            if ss >= 0 {
                rm = pin_hreg(ss);
            } else {
                emit_gpr_rd(b, sf, JT1, sreg);
                rm = JT1;
            }
        } else {
            let mut v = s.imm;
            if sf == 0 {
                v &= 0xffffffff;
            }
            if is_logic {
                let done = match op {
                    OCERZ_OP_AND => a64_try_and_imm(b, sf, rd, rd, v),
                    OCERZ_OP_OR => a64_try_orr_imm(b, sf, rd, rd, v),
                    _ => a64_try_eor_imm(b, sf, rd, rd, v),
                };
                if done != 0 {
                    emit_defer_flags(b, cc_pack(OCERZ_CC_LOGIC, d.size, 0), rd, rd);
                    return 1;
                }
            } else if (is_add || is_sub) && (v <= 4095 || ((v & 0xfff) == 0 && (v >> 12) <= 4095)) {
                a64_mov_imm64(b, JT1, v);
                if sf != 0 {
                    a64_stp_off(b, rd, JT1, 20, CC_SRC_OFF as _);
                } else {
                    a64_mov_reg(b, 0, JT0, rd);
                    a64_stp_off(b, JT0, JT1, 20, CC_SRC_OFF as _);
                }
                a64_mov_imm64(b, JTT, cc_pack(if is_add { OCERZ_CC_ADD } else { OCERZ_CC_SUB }, d.size, 0) as u64);
                a64_str(b, 4, JTT, 20, CC_OP_OFF);
                if v <= 4095 {
                    if is_add {
                        a64_add_imm(b, sf, rd, rd, v as u32);
                    } else {
                        a64_sub_imm(b, sf, rd, rd, v as u32);
                    }
                } else if is_add {
                    a64_add_reg(b, sf, rd, rd, JT1, 0);
                } else {
                    a64_sub_reg(b, sf, rd, rd, JT1, 0);
                }
                return 1;
            }
            a64_mov_imm64(b, JT1, v);
            rm = JT1;
        }
        if is_add || is_sub {
            if sf != 0 {
                a64_stp_off(b, rd, rm, 20, CC_SRC_OFF as _);
            } else {
                a64_mov_reg(b, 0, JT0, rd);
                a64_mov_reg(b, 0, JT2, rm);
                a64_stp_off(b, JT0, JT2, 20, CC_SRC_OFF as _);
            }
            a64_mov_imm64(b, JTT, cc_pack(if is_add { OCERZ_CC_ADD } else { OCERZ_CC_SUB }, d.size, 0) as u64);
            a64_str(b, 4, JTT, 20, CC_OP_OFF);
            if is_add {
                a64_add_reg(b, sf, rd, rd, rm, 0);
            } else {
                a64_sub_reg(b, sf, rd, rd, rm, 0);
            }
            return 1;
        }
        match op {
            OCERZ_OP_AND => a64_and_reg(b, sf, rd, rd, rm, 0),
            OCERZ_OP_OR => a64_orr_reg(b, sf, rd, rd, rm, 0),
            _ => a64_eor_reg(b, sf, rd, rd, rm, 0),
        }
        emit_defer_flags(b, cc_pack(OCERZ_CC_LOGIC, d.size, 0), rd, rd);
        return 1;
    }

    if writes && need == 0 {
        let rsp_d = (rsp_is_ptr() != 0) && dreg == OCERZ_RSP;
        let rsp_s = (rsp_is_ptr() != 0) && s.kind == KREG && sreg == OCERZ_RSP;
        if rsp_d && (sf == 0 || (op != OCERZ_OP_ADD && op != OCERZ_OP_SUB) || rsp_s) {
            return 0;
        }
        let ds = pin_slot(dreg);
        let rd = if ds >= 0 { pin_hreg(ds) } else { JT2 };
        let mut rn = if ds >= 0 { rd } else { JT0 };
        let rm;

        if ds >= 0 && !rsp_d && (s.kind == KIMM || (s.kind == KREG && s.high8 == 0)) {
            let mut hs: c_int = 0;
            if fuse_prev_mov(b, dreg, d.size as c_int, &mut hs, insn) >= 0 {
                rn = hs;
            }
        }
        if ds < 0 {
            emit_gpr_rd(b, sf, JT0, dreg);
        }
        if s.kind == KREG {
            if s.high8 != 0 {
                return 0;
            }
            let ss = pin_slot(sreg);
            if ss >= 0 && !rsp_s {
                rm = pin_hreg(ss);
            } else {
                emit_gpr_rd(b, sf, JT1, sreg);
                rm = JT1;
            }
        } else if s.kind == KIMM {
            let mut v = s.imm;
            if sf == 0 {
                v &= 0xffffffff;
            }
            let width_mask: u64 = if sf != 0 { u64::MAX } else { 0xffffffff };
            let neg = 0u64.wrapping_sub(v) & width_mask;
            let mut emitted = 0;
            match op {
                OCERZ_OP_ADD => {
                    if v <= 4095 {
                        a64_add_imm(b, sf, rd, rn, v as u32);
                        emitted = 1;
                    } else if neg <= 4095 {
                        a64_sub_imm(b, sf, rd, rn, neg as u32);
                        emitted = 1;
                    }
                }
                OCERZ_OP_SUB => {
                    if v <= 4095 {
                        a64_sub_imm(b, sf, rd, rn, v as u32);
                        emitted = 1;
                    } else if neg <= 4095 {
                        a64_add_imm(b, sf, rd, rn, neg as u32);
                        emitted = 1;
                    }
                }
                OCERZ_OP_AND => emitted = a64_try_and_imm(b, sf, rd, rn, v),
                OCERZ_OP_OR => emitted = a64_try_orr_imm(b, sf, rd, rn, v),
                OCERZ_OP_XOR => emitted = a64_try_eor_imm(b, sf, rd, rn, v),
                _ => {}
            }
            if emitted != 0 {
                if ds < 0 {
                    emit_gpr_wr(b, rd, dreg);
                }
                return 1;
            }
            a64_mov_imm64(b, JT1, v);
            rm = JT1;
        } else {
            return 0;
        }

        match op {
            OCERZ_OP_ADD => a64_add_reg(b, sf, rd, rn, rm, 0),
            OCERZ_OP_SUB => a64_sub_reg(b, sf, rd, rn, rm, 0),
            OCERZ_OP_AND => a64_and_reg(b, sf, rd, rn, rm, 0),
            OCERZ_OP_OR => a64_orr_reg(b, sf, rd, rn, rm, 0),
            OCERZ_OP_XOR => a64_eor_reg(b, sf, rd, rn, rm, 0),
            _ => return 0,
        }
        if ds < 0 {
            emit_gpr_wr(b, rd, dreg);
        }
        return 1;
    }

    emit_gpr_rd(b, sf, JT0, dreg);
    if emit_load_operand(b, s, sf, JT1) == 0 {
        return 0;
    }

    match op {
        OCERZ_OP_ADD => a64_add_reg(b, sf, JT2, JT0, JT1, 0),
        OCERZ_OP_SUB | OCERZ_OP_CMP => a64_sub_reg(b, sf, JT2, JT0, JT1, 0),
        OCERZ_OP_AND | OCERZ_OP_TEST => a64_and_reg(b, sf, JT2, JT0, JT1, 0),
        OCERZ_OP_OR => a64_orr_reg(b, sf, JT2, JT0, JT1, 0),
        OCERZ_OP_XOR => a64_eor_reg(b, sf, JT2, JT0, JT1, 0),
        _ => return 0,
    }

    if writes {
        emit_gpr_wr(b, JT2, dreg);
    }

    if need == 0 {
        return 1;
    }

    if is_add {
        emit_defer_flags(b, cc_pack(OCERZ_CC_ADD, d.size, 0), JT0, JT1);
    } else if is_sub {
        emit_defer_flags(b, cc_pack(OCERZ_CC_SUB, d.size, 0), JT0, JT1);
    } else {
        emit_defer_flags(b, cc_pack(OCERZ_CC_LOGIC, d.size, 0), JT2, JT2);
    }
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_cmp_test_narrow(
    b: *mut A64Buf,
    insn: *const X86Insn,
    need: u64,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    static mut NO_NARROW: c_int = -1;
    if NO_NARROW < 0 {
        NO_NARROW = if getenv_set(c"OCERZ_NO_INLINE_NARROW") { 1 } else { 0 };
    }
    if NO_NARROW != 0 {
        return 0;
    }
    let insn = &*insn;
    let op = insn.op as c_uint;
    if op != OCERZ_OP_CMP && op != OCERZ_OP_TEST {
        return 0;
    }
    let d = &insn.ops[0];
    let s = &insn.ops[1];
    if d.size != 1 && d.size != 2 {
        return 0;
    }
    let d_mem = d.kind == KMEM;
    let s_mem = s.kind == KMEM;
    if d_mem && s_mem {
        return 0;
    }
    if d.kind == KREG && d.high8 != 0 {
        return 0;
    }
    if d.kind != KREG && !d_mem {
        return 0;
    }
    if s.kind == KREG {
        if s.high8 != 0 || s.size != d.size {
            return 0;
        }
    } else if s.kind == KMEM {
        if s.size != d.size {
            return 0;
        }
    } else if s.kind != KIMM {
        return 0;
    }
    if (d_mem || s_mem)
        && (g_defer == 0
            || (insn.seg != SEG_NONE && insn.seg != OCERZ_SEG_GS as u8 && insn.seg != OCERZ_SEG_FS as u8))
    {
        return 0;
    }

    if g_defer == 0 && need == 0 {
        return 1;
    }
    if (d_mem || s_mem) && need == 0 && g_nzcv_want == 0 {
        return 1;
    }

    let is_sub = op == OCERZ_OP_CMP;
    let size = d.size as c_int;
    let mask: u64 = if size == 1 { 0xff } else { 0xffff };
    let sh = 32 - 8 * size;
    let dreg = d.reg as c_uint;
    let sreg = s.reg as c_uint;

    if g_defer != 0
        && !d_mem
        && !s_mem
        && pin_slot(dreg) >= 0
        && !((rsp_is_ptr() != 0) && dreg == OCERZ_RSP)
        && (s.kind == KIMM || (pin_slot(sreg) >= 0 && !((rsp_is_ptr() != 0) && sreg == OCERZ_RSP)))
    {
        let rd = pin_hreg(pin_slot(dreg));
        if !is_sub && s.kind == KIMM {
            let v = s.imm & mask;
            if g_nzcv_want != 0 {
                if a64_try_ands_imm(b, 1, JT2, rd, v) == 0 {
                    a64_mov_imm64(b, JT1, v);
                    a64_ands_reg(b, 1, JT2, rd, JT1, 0);
                }
                if need != 0 {
                    emit_defer_flags(b, cc_pack(OCERZ_CC_LOGIC, d.size, 0), JT2, JT2);
                }
                g_nzcv_kind = OCERZ_CC_LOGIC;
                g_nzcv_from = g_cur_insn_idx;
                return 1;
            }
            if need == 0 {
                return 1;
            }
            if a64_try_and_imm(b, 1, JT2, rd, v) == 0 {
                a64_mov_imm64(b, JT1, v);
                a64_and_reg(b, 1, JT2, rd, JT1, 0);
            }
            emit_defer_flags(b, cc_pack(OCERZ_CC_LOGIC, d.size, 0), JT2, JT2);
            return 1;
        }
        if need == 0 && g_nzcv_want == 0 {
            return 1;
        }
        if size == 1 {
            a64_uxtb(b, JT0, rd);
        } else {
            a64_uxth(b, JT0, rd);
        }
        if s.kind == KIMM {
            a64_mov_imm64(b, JT1, s.imm & mask);
        } else {
            let rs = pin_hreg(pin_slot(sreg));
            if size == 1 {
                a64_uxtb(b, JT1, rs);
            } else {
                a64_uxth(b, JT1, rs);
            }
        }
        if g_nzcv_want != 0 && is_sub {
            a64_subs_reg(b, 0, A64_ZR, JT0, JT1, 0);
            if need != 0 {
                emit_defer_flags(b, cc_pack(OCERZ_CC_SUB, d.size, 0), JT0, JT1);
            }
            g_nzcv_kind = OCERZ_CC_SUB;
            g_nzcv_from = g_cur_insn_idx;
            return 1;
        }
        if is_sub {
            emit_defer_flags(b, cc_pack(OCERZ_CC_SUB, d.size, 0), JT0, JT1);
        } else {
            a64_and_reg(b, 1, JT2, JT0, JT1, 0);
            emit_defer_flags(b, cc_pack(OCERZ_CC_LOGIC, d.size, 0), JT2, JT2);
        }
        return 1;
    }

    if s_mem && emit_mem_load_plain(b, insn, s, size, JT1) == 0 {
        if emit_mem_ea(b, insn, s, JTA) == 0 {
            return 0;
        }
        let skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, ocerz_guest_base.wrapping_sub(ea_fold()));
        emit_guest_load_ordered(b, size, JT1, JTA, JTU);
        patch_guard_skip(skip, a64_label(b));
    }
    if d_mem {
        if emit_mem_load_plain(b, insn, d, size, JT0) == 0 {
            if emit_mem_ea(b, insn, d, JTA) == 0 {
                return 0;
            }
            let skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
            emit_add_const(b, JTA, ocerz_guest_base.wrapping_sub(ea_fold()));
            emit_guest_load_ordered(b, size, JT0, JTA, JTU);
            patch_guard_skip(skip, a64_label(b));
        }
    } else {
        emit_gpr_rd(b, 1, JT0, dreg);
        if size == 1 {
            a64_uxtb(b, JT0, JT0);
        } else {
            a64_uxth(b, JT0, JT0);
        }
    }
    if s.kind == KREG {
        let ss = pin_slot(sreg);
        let rs = if ss >= 0 && !((rsp_is_ptr() != 0) && sreg == OCERZ_RSP) { pin_hreg(ss) } else { JT1 };
        if rs == JT1 {
            emit_gpr_rd(b, 1, JT1, sreg);
        }
        if size == 1 {
            a64_uxtb(b, JT1, rs);
        } else {
            a64_uxth(b, JT1, rs);
        }
    } else if !s_mem {
        a64_mov_imm64(b, JT1, s.imm & mask);
    }

    if g_defer != 0 {
        if g_nzcv_want != 0 && is_sub {
            a64_subs_reg(b, 0, A64_ZR, JT0, JT1, 0);
            if need != 0 {
                emit_defer_flags(b, cc_pack(OCERZ_CC_SUB, d.size, 0), JT0, JT1);
            }
            g_nzcv_kind = OCERZ_CC_SUB;
            g_nzcv_from = g_cur_insn_idx;
            return 1;
        }
        if is_sub {
            emit_defer_flags(b, cc_pack(OCERZ_CC_SUB, d.size, 0), JT0, JT1);
        } else {
            a64_and_reg(b, 1, JT2, JT0, JT1, 0);
            emit_defer_flags(b, cc_pack(OCERZ_CC_LOGIC, d.size, 0), JT2, JT2);
        }
        return 1;
    }

    a64_lsl_imm(b, 0, JTA, JT0, sh);
    a64_lsl_imm(b, 0, JTU, JT1, sh);
    if is_sub {
        a64_subs_reg(b, 0, A64_ZR, JTA, JTU, 0);
    } else {
        a64_ands_reg(b, 0, A64_ZR, JTA, JTU, 0);
    }

    if is_sub {
        a64_sub_reg(b, 1, JT2, JT0, JT1, 0);
    } else {
        a64_and_reg(b, 1, JT2, JT0, JT1, 0);
    }

    a64_mov_imm64(b, JTF, 0);
    emit_zf_sf(b, need);
    if need & OCERZ_PF != 0 {
        emit_pf(b, JT2);
    }
    if is_sub {
        if need & OCERZ_CF != 0 {
            a64_cset(b, JTT, A64_CC);
            a64_lsl_imm(b, 0, JTT, JTT, 0);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
        if need & OCERZ_OF != 0 {
            a64_cset(b, JTT, A64_VS);
            a64_lsl_imm(b, 0, JTT, JTT, 11);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
        if need & OCERZ_AF != 0 {
            a64_eor_reg(b, 1, JTT, JT0, JT1, 0);
            a64_eor_reg(b, 1, JTT, JTT, JT2, 0);
            a64_ubfx(b, 1, JTT, JTT, 4, 1);
            a64_lsl_imm(b, 0, JTT, JTT, 4);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
    }
    emit_commit_flags(b, need);
    1
}

unsafe fn emit_incdec_eager(b: *mut A64Buf, insn: &X86Insn, mut need: u64) -> c_int {
    let d = &insn.ops[0];
    if d.kind != KREG || d.high8 != 0 || (d.size != 4 && d.size != 8) {
        return 0;
    }
    let sf = (d.size == 8) as c_int;
    let is_inc = insn.op as c_uint == OCERZ_OP_INC;

    need &= JIT_ARITH_FLAGS & !OCERZ_CF;

    a64_ldr(b, sz(sf), JT0, 20, gpr_off(d.reg as c_uint));
    if is_inc {
        a64_adds_imm(b, sf, JT2, JT0, 1);
    } else {
        a64_subs_imm(b, sf, JT2, JT0, 1);
    }
    a64_str(b, 8, JT2, 20, gpr_off(d.reg as c_uint));

    if need == 0 {
        return 1;
    }

    a64_mov_imm64(b, JTF, 0);
    emit_zf_sf(b, need);
    if need & OCERZ_PF != 0 {
        emit_pf(b, JT2);
    }

    if need & OCERZ_OF != 0 {
        let of_const = if is_inc { 1u64 << (d.size as u32 * 8 - 1) } else { ocerz_mask(d.size as i32) >> 1 };
        a64_mov_imm64(b, JTU, of_const);
        a64_subs_reg(b, 1, A64_ZR, JT2, JTU, 0);
        a64_cset(b, JTT, A64_EQ);
        a64_lsl_imm(b, 0, JTT, JTT, 11);
        a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
    }

    if need & OCERZ_AF != 0 {
        a64_ubfx(b, 1, JTT, JT2, 0, 4);
        if is_inc {
            a64_subs_imm(b, 1, A64_ZR, JTT, 0);
            a64_cset(b, JTT, A64_EQ);
        } else {
            a64_subs_imm(b, 1, A64_ZR, JTT, 0xf);
            a64_cset(b, JTT, A64_EQ);
        }
        a64_lsl_imm(b, 0, JTT, JTT, 4);
        a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
    }

    emit_commit_flags(b, need);
    1
}

unsafe fn emit_incdec_narrow(b: *mut A64Buf, insn: &X86Insn, mut need: u64) -> c_int {
    let d = &insn.ops[0];
    if g_defer == 0 || d.kind != KREG || d.high8 != 0 || (d.size != 1 && d.size != 2) {
        return 0;
    }
    let dreg = d.reg as c_uint;
    if pin_slot(dreg) < 0 || ((rsp_is_ptr() != 0) && dreg == OCERZ_RSP) {
        return 0;
    }
    let rd = pin_hreg(pin_slot(dreg));
    let bits = d.size as c_int * 8;
    let is_inc = insn.op as c_uint == OCERZ_OP_INC;
    need &= JIT_ARITH_FLAGS & !OCERZ_CF;
    if need != 0 {
        emit_cc_predicate(b, OCERZ_CC_B);
        a64_cset(b, JTU, A64_NE);
    }
    if d.size == 1 {
        a64_uxtb(b, JT0, rd);
    } else {
        a64_uxth(b, JT0, rd);
    }
    if is_inc {
        a64_add_imm(b, 0, JT2, JT0, 1);
    } else {
        a64_sub_imm(b, 0, JT2, JT0, 1);
    }
    a64_bfi(b, 1, rd, JT2, 0, bits);
    if need != 0 {
        if d.size == 1 {
            a64_uxtb(b, JT2, JT2);
        } else {
            a64_uxth(b, JT2, JT2);
        }
        emit_defer_flags(b, cc_pack(if is_inc { OCERZ_CC_INC } else { OCERZ_CC_DEC }, d.size, 0), JTU, JT2);
    }
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_incdec(b: *mut A64Buf, insn: *const X86Insn, mut need: u64) -> c_int {
    let insn = &*insn;
    if insn.ops[0].kind == KREG && (insn.ops[0].size == 1 || insn.ops[0].size == 2) {
        return emit_incdec_narrow(b, insn, need);
    }
    if g_defer == 0 {
        return emit_incdec_eager(b, insn, need);
    }
    let d = &insn.ops[0];
    if d.kind != KREG || d.high8 != 0 || (d.size != 4 && d.size != 8) {
        return 0;
    }
    let sf = (d.size == 8) as c_int;
    let is_inc = insn.op as c_uint == OCERZ_OP_INC;
    let dreg = d.reg as c_uint;

    need &= JIT_ARITH_FLAGS & !OCERZ_CF;

    if need == 0 {
        let ds = pin_slot(dreg);
        if ds >= 0 {
            let rd = pin_hreg(ds);
            if is_inc {
                a64_add_imm(b, sf, rd, rd, 1);
            } else {
                a64_sub_imm(b, sf, rd, rd, 1);
            }
            return 1;
        }
    }

    emit_gpr_rd(b, sf, JT0, dreg);
    if is_inc {
        a64_add_imm(b, sf, JT2, JT0, 1);
    } else {
        a64_sub_imm(b, sf, JT2, JT0, 1);
    }
    emit_gpr_wr(b, JT2, dreg);

    if need == 0 {
        return 1;
    }

    emit_cc_predicate(b, OCERZ_CC_B);
    a64_cset(b, JT0, A64_NE);
    emit_gpr_rd(b, 1, JT1, dreg);
    emit_defer_flags(b, cc_pack(if is_inc { OCERZ_CC_INC } else { OCERZ_CC_DEC }, d.size, 0), JT0, JT1);
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_mov_logic_pair(
    b: *mut A64Buf,
    mov: *const X86Insn,
    logic: *const X86Insn,
    logic_need: u64,
    logic_label: *mut *mut u32,
) -> c_int {
    let mov = &*mov;
    let logic = &*logic;
    let lop = logic.op as c_uint;
    if mov.lock != 0
        || logic.lock != 0
        || logic_need != 0
        || mov.op as c_uint != OCERZ_OP_MOV
        || (lop != OCERZ_OP_AND && lop != OCERZ_OP_OR && lop != OCERZ_OP_XOR)
        || mov.nops != 2
        || logic.nops != 2
        || mov.rip.wrapping_add(mov.len as u64) != logic.rip
    {
        return 0;
    }

    let md = &mov.ops[0];
    let ms = &mov.ops[1];
    let ld = &logic.ops[0];
    let ls = &logic.ops[1];
    if md.kind != KREG
        || ms.kind != KREG
        || ld.kind != KREG
        || md.high8 != 0
        || ms.high8 != 0
        || ld.high8 != 0
        || (md.size != 4 && md.size != 8)
        || ms.size != md.size
        || ld.size != md.size
        || ld.reg != md.reg
        || ls.size != md.size
    {
        return 0;
    }
    if ls.kind == KREG {
        if ls.high8 != 0 {
            return 0;
        }
    } else if ls.kind != KIMM {
        return 0;
    }

    let sf = (md.size == 8) as c_int;
    let ds = pin_slot(md.reg as c_uint);
    let ss = pin_slot(ms.reg as c_uint);
    let rd = if ds >= 0 { pin_hreg(ds) } else { JT2 };
    let rn = if ss >= 0 { pin_hreg(ss) } else { JT0 };
    if ss < 0 {
        emit_gpr_rd(b, sf, JT0, ms.reg as c_uint);
    }

    if !logic_label.is_null() {
        *logic_label = a64_label(b);
    }

    let mut rm = JT1;
    if ls.kind == KIMM {
        let mut v = ls.imm;
        if sf == 0 {
            v &= 0xffffffff;
        }
        let emitted = if lop == OCERZ_OP_AND {
            a64_try_and_imm(b, sf, rd, rn, v)
        } else if lop == OCERZ_OP_OR {
            a64_try_orr_imm(b, sf, rd, rn, v)
        } else {
            a64_try_eor_imm(b, sf, rd, rn, v)
        };
        if emitted != 0 {
            if ds < 0 {
                emit_gpr_wr(b, rd, md.reg as c_uint);
            }
            return 1;
        }
        a64_mov_imm64(b, JT1, v);
    } else if ls.reg == md.reg || ls.reg == ms.reg {
        rm = rn;
    } else {
        let ls_slot = pin_slot(ls.reg as c_uint);
        if ls_slot >= 0 {
            rm = pin_hreg(ls_slot);
        } else {
            emit_gpr_rd(b, sf, JT1, ls.reg as c_uint);
        }
    }

    if lop == OCERZ_OP_AND {
        a64_and_reg(b, sf, rd, rn, rm, 0);
    } else if lop == OCERZ_OP_OR {
        a64_orr_reg(b, sf, rd, rn, rm, 0);
    } else {
        a64_eor_reg(b, sf, rd, rn, rm, 0);
    }
    if ds < 0 {
        emit_gpr_wr(b, rd, md.reg as c_uint);
    }
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_add_inc_pair(
    b: *mut A64Buf,
    add: *const X86Insn,
    inc: *const X86Insn,
    add_need: u64,
    inc_need: u64,
    inc_label: *mut *mut u32,
) -> c_int {
    let add = &*add;
    let inc = &*inc;
    if g_defer == 0 || g_no_addincfuse != 0 || g_no_lazyflags != 0 || add.lock != 0 || inc.lock != 0 {
        return 0;
    }
    if add.op as c_uint != OCERZ_OP_ADD
        || inc.op as c_uint != OCERZ_OP_INC
        || add.nops != 2
        || inc.nops != 1
        || add.rip.wrapping_add(add.len as u64) != inc.rip
    {
        return 0;
    }

    let d = &add.ops[0];
    let s = &add.ops[1];
    let id = &inc.ops[0];
    if d.kind != KREG
        || id.kind != KREG
        || d.high8 != 0
        || id.high8 != 0
        || (d.size != 4 && d.size != 8)
        || id.reg != d.reg
        || id.size != d.size
    {
        return 0;
    }
    if s.kind == KREG {
        if s.high8 != 0 || s.size != d.size {
            return 0;
        }
    } else if s.kind != KIMM || s.size != d.size {
        return 0;
    }

    if add_need != OCERZ_CF || inc_need == 0 {
        return 0;
    }

    let sf = (d.size == 8) as c_int;
    let dreg = d.reg as c_uint;
    let ds = pin_slot(dreg);
    let rd = if ds >= 0 { pin_hreg(ds) } else { JT2 };
    if ds < 0 {
        emit_gpr_rd(b, sf, rd, dreg);
    }
    let mut emitted = false;
    let mut rm = JT1;
    if s.kind == KIMM {
        let mut v = s.imm;
        if sf == 0 {
            v &= 0xffffffff;
        }
        if v <= 4095 {
            a64_adds_imm(b, sf, rd, rd, v as u32);
            emitted = true;
        } else {
            a64_mov_imm64(b, JT1, v);
        }
    } else {
        let ss = pin_slot(s.reg as c_uint);
        if s.reg == d.reg {
            rm = rd;
        } else if ss >= 0 {
            rm = pin_hreg(ss);
        } else {
            emit_gpr_rd(b, sf, JT1, s.reg as c_uint);
        }
    }
    if !emitted {
        a64_adds_reg(b, sf, rd, rd, rm, 0);
    }
    a64_cset(b, JT0, A64_CS);
    if !inc_label.is_null() {
        *inc_label = a64_label(b);
    }
    a64_add_imm(b, sf, rd, rd, 1);
    if ds < 0 {
        emit_gpr_wr(b, rd, dreg);
    }
    emit_defer_flags(b, cc_pack(OCERZ_CC_INC, d.size, 0), JT0, rd);
    1
}

unsafe fn emit_shift_eager(b: *mut A64Buf, insn: &X86Insn, need: u64) -> c_int {
    let d = &insn.ops[0];
    let s = &insn.ops[1];
    if d.kind != KREG || d.high8 != 0 || (d.size != 4 && d.size != 8) {
        return 0;
    }
    if s.kind != KIMM {
        return 0;
    }
    let sf = (d.size == 8) as c_int;
    let bits = d.size as c_int * 8;
    let cnt = (s.imm & if sf != 0 { 63 } else { 31 }) as c_uint;
    if cnt == 0 {
        return 1;
    }

    let op = insn.op as c_uint;
    a64_ldr(b, sz(sf), JT0, 20, gpr_off(d.reg as c_uint));

    match op {
        OCERZ_OP_SHL => a64_lsl_imm(b, sf, JT2, JT0, cnt as c_int),
        OCERZ_OP_SHR => a64_lsr_imm(b, sf, JT2, JT0, cnt as c_int),
        OCERZ_OP_SAR => a64_asr_imm(b, sf, JT2, JT0, cnt as c_int),
        _ => return 0,
    }
    a64_str(b, 8, JT2, 20, gpr_off(d.reg as c_uint));

    let mut emit = need;
    if op == OCERZ_OP_SHL && need & OCERZ_OF != 0 {
        emit |= OCERZ_CF | OCERZ_SF;
    }

    if emit == 0 {
        return 1;
    }

    if emit & (OCERZ_ZF | OCERZ_SF) != 0 {
        a64_subs_imm(b, sf, A64_ZR, JT2, 0);
    }
    a64_mov_imm64(b, JTF, 0);
    emit_zf_sf(b, emit);
    if emit & OCERZ_PF != 0 {
        emit_pf(b, JT2);
    }

    if op == OCERZ_OP_SHL {
        if emit & OCERZ_CF != 0 {
            let cf_bit = bits - cnt as c_int;
            if cf_bit >= 0 && cf_bit < bits {
                a64_ubfx(b, sf, JTT, JT0, cf_bit, 1);
                a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
            }
        }
        if emit & OCERZ_OF != 0 {
            a64_ubfx(b, 1, JTT, JTF, 0, 1);
            a64_ubfx(b, 1, JTU, JTF, 7, 1);
            a64_eor_reg(b, 1, JTT, JTT, JTU, 0);
            a64_lsl_imm(b, 0, JTT, JTT, 11);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
    } else if op == OCERZ_OP_SHR {
        if emit & OCERZ_CF != 0 {
            let cf_bit = cnt as c_int - 1;
            a64_ubfx(b, sf, JTT, JT0, cf_bit, 1);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
        if emit & OCERZ_OF != 0 {
            a64_ubfx(b, sf, JTT, JT0, bits - 1, 1);
            a64_lsl_imm(b, 0, JTT, JTT, 11);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
    } else if emit & OCERZ_CF != 0 {
        let mut shift = cnt as c_int - 1;
        if shift > 63 {
            shift = 63;
        }
        if sf != 0 {
            a64_asr_imm(b, 1, JTT, JT0, shift);
        } else {
            a64_sxtw(b, JTT, JT0);
            a64_asr_imm(b, 1, JTT, JTT, shift);
        }
        a64_ubfx(b, 1, JTT, JTT, 0, 1);
        a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
    }
    emit_commit_flags(b, emit);
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_shift(b: *mut A64Buf, insn: *const X86Insn, need: u64) -> c_int {
    let insn = &*insn;
    if g_defer == 0 {
        return emit_shift_eager(b, insn, need);
    }
    let d = &insn.ops[0];
    let s = &insn.ops[1];
    let iop = insn.op as c_uint;
    let dreg = d.reg as c_uint;
    if d.kind == KREG
        && d.high8 == 0
        && (d.size == 1 || d.size == 2)
        && s.kind == KIMM
        && pin_slot(dreg) >= 0
        && !((rsp_is_ptr() != 0) && dreg == OCERZ_RSP)
        && (iop == OCERZ_OP_SHL || iop == OCERZ_OP_SHR || iop == OCERZ_OP_SAR)
    {
        let ncnt = (s.imm & 31) as c_uint;
        if ncnt == 0 {
            return 1;
        }
        let rd = pin_hreg(pin_slot(dreg));
        let nbits = d.size as c_int * 8;
        if d.size == 1 {
            if iop == OCERZ_OP_SAR {
                a64_sxtb(b, 0, JT0, rd);
            } else {
                a64_uxtb(b, JT0, rd);
            }
        } else if iop == OCERZ_OP_SAR {
            a64_sxth(b, 0, JT0, rd);
        } else {
            a64_uxth(b, JT0, rd);
        }
        match iop {
            OCERZ_OP_SHL => a64_lsl_imm(b, 0, JT2, JT0, ncnt as c_int),
            OCERZ_OP_SHR => a64_lsr_imm(b, 0, JT2, JT0, ncnt as c_int),
            _ => a64_asr_imm(b, 0, JT2, JT0, ncnt as c_int),
        }
        a64_bfi(b, 1, rd, JT2, 0, nbits);
        if need != 0 {
            if iop == OCERZ_OP_SAR {
                if d.size == 1 {
                    a64_uxtb(b, JT0, JT0);
                } else {
                    a64_uxth(b, JT0, JT0);
                }
            }
            a64_mov_imm64(b, JT1, ncnt as u64);
            let nk = if iop == OCERZ_OP_SHL {
                OCERZ_CC_SHL
            } else if iop == OCERZ_OP_SHR {
                OCERZ_CC_SHR
            } else {
                OCERZ_CC_SAR
            };
            emit_defer_flags(b, cc_pack(nk, d.size, 0), JT0, JT1);
        }
        return 1;
    }
    if d.kind != KREG || d.high8 != 0 || (d.size != 4 && d.size != 8) {
        return 0;
    }
    if s.kind != KIMM {
        return 0;
    }
    let sf = (d.size == 8) as c_int;
    let cnt = (s.imm & if sf != 0 { 63 } else { 31 }) as c_uint;
    if cnt == 0 {
        return 1;
    }

    let op = iop;
    if need == 0 {
        let ds = pin_slot(dreg);
        if ds >= 0 {
            let rd = pin_hreg(ds);
            let mut rn = rd;
            if op == OCERZ_OP_SHL || op == OCERZ_OP_SHR || op == OCERZ_OP_SAR {
                let mut hs: c_int = 0;
                if fuse_prev_mov(b, dreg, d.size as c_int, &mut hs, insn) >= 0 {
                    rn = hs;
                } else if !g_cur_insns_fwd().is_null() && g_cur_insn_idx >= 0 && mov_sink_at(g_cur_insn_idx) >= 0 {
                    rn = pin_hreg(pin_slot(cur_insn(mov_sink_at(g_cur_insn_idx) as c_int).ops[1].reg as c_uint));
                }
            }
            match op {
                OCERZ_OP_SHL => a64_lsl_imm(b, sf, rd, rn, cnt as c_int),
                OCERZ_OP_SHR => a64_lsr_imm(b, sf, rd, rn, cnt as c_int),
                OCERZ_OP_SAR => a64_asr_imm(b, sf, rd, rn, cnt as c_int),
                _ => return 0,
            }
            return 1;
        }
    }
    emit_gpr_rd(b, sf, JT0, dreg);

    match op {
        OCERZ_OP_SHL => a64_lsl_imm(b, sf, JT2, JT0, cnt as c_int),
        OCERZ_OP_SHR => a64_lsr_imm(b, sf, JT2, JT0, cnt as c_int),
        OCERZ_OP_SAR => a64_asr_imm(b, sf, JT2, JT0, cnt as c_int),
        _ => return 0,
    }
    emit_gpr_wr(b, JT2, dreg);

    if need == 0 {
        return 1;
    }

    a64_mov_imm64(b, JT1, cnt as u64);
    let kind = if op == OCERZ_OP_SHL {
        OCERZ_CC_SHL
    } else if op == OCERZ_OP_SHR {
        OCERZ_CC_SHR
    } else {
        OCERZ_CC_SAR
    };
    emit_defer_flags(b, cc_pack(kind, d.size, 0), JT0, JT1);
    1
}

static mut G_NO_INLINE_IMUL: c_int = -1;

#[inline]
unsafe fn imul_inline_enabled() -> bool {
    if G_NO_INLINE_IMUL < 0 {
        let e = libc::getenv(c"OCERZ_NO_INLINE_IMUL".as_ptr());
        G_NO_INLINE_IMUL = (!e.is_null() && *e != 0 && *e as u8 != b'0') as c_int;
    }
    G_NO_INLINE_IMUL == 0
}

unsafe fn emit_imul_src(b: *mut A64Buf, insn: &X86Insn, op: &X86Operand, dst: c_int) -> c_int {
    if op.kind == KMEM {
        if op.size != 4 && op.size != 8 {
            return 0;
        }
        if emit_mem_load_any(b, insn, op, op.size as c_int, dst) == 0 {
            return 0;
        }
        if op.size == 4 {
            a64_sxtw(b, dst, dst);
        }
        return 1;
    }
    if op.kind == KREG {
        if op.high8 != 0 {
            return 0;
        }
        if op.size == 8 {
            emit_gpr_rd(b, 1, dst, op.reg as c_uint);
        } else if op.size == 4 {
            emit_gpr_rd_sw(b, dst, op.reg as c_uint);
        } else {
            return 0;
        }
        return 1;
    }
    if op.kind == KIMM {
        a64_mov_imm64(b, dst, crate::inline::ocerz_sext(op.imm, op.size as _) as u64);
        return 1;
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_mul_wide(b: *mut A64Buf, insn: *const X86Insn, need: u64, is_signed: c_int) -> c_int {
    let insn = &*insn;
    let o = &insn.ops[0];
    if insn.nops != 1 || (o.size != 4 && o.size != 8) {
        return 0;
    }
    if g_pin_class != 3 || pin_slot(OCERZ_RAX) < 0 || pin_slot(OCERZ_RDX) < 0 {
        return 0;
    }
    if g_defer == 0 {
        return 0;
    }
    let sf = o.size == 8;
    let hax = pin_hreg(pin_slot(OCERZ_RAX));
    let hdx = pin_hreg(pin_slot(OCERZ_RDX));
    let src;
    if o.kind == KREG {
        if o.high8 != 0 || pin_slot(o.reg as c_uint) < 0 {
            return 0;
        }
        src = pin_hreg(pin_slot(o.reg as c_uint));
    } else if o.kind == KMEM {
        if emit_mem_load_any(b, insn, o, o.size as c_int, JT1) == 0 {
            return 0;
        }
        src = JT1;
    } else {
        return 0;
    }
    if sf {
        if is_signed != 0 {
            a64_smulh(b, JT2, hax, src);
        } else {
            a64_umulh(b, JT2, hax, src);
        }
        a64_mul(b, 1, hax, hax, src);
        a64_mov_reg(b, 1, hdx, JT2);
    } else {
        a64_mov_reg(b, 0, JT0, hax);
        a64_mov_reg(b, 0, JT1, src);
        if is_signed != 0 {
            a64_sxtw(b, JT0, JT0);
            a64_sxtw(b, JT1, JT1);
        }
        a64_mul(b, 1, JT2, JT0, JT1);
        a64_mov_reg(b, 0, hax, JT2);
        a64_lsr_imm(b, 1, hdx, JT2, 32);
    }
    if need != 0 {
        emit_defer_flags(b, cc_pack(if is_signed != 0 { OCERZ_CC_IMUL } else { OCERZ_CC_MUL }, o.size, 0), hax, hdx);
    }
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_imul(b: *mut A64Buf, insn: *const X86Insn, need: u64) -> c_int {
    if !imul_inline_enabled() {
        return 0;
    }
    let insn = &*insn;
    if insn.op as c_uint != OCERZ_OP_IMUL || insn.nops < 2 || insn.nops > 3 {
        return 0;
    }

    let d = &insn.ops[0];
    if d.kind != KREG || d.high8 != 0 || (d.size != 4 && d.size != 8) {
        return 0;
    }
    let sf = (d.size == 8) as c_int;
    let dreg = d.reg as c_uint;

    let s1 = if insn.nops == 3 { &insn.ops[1] } else { &insn.ops[0] };
    let s2 = if insn.nops == 3 { &insn.ops[2] } else { &insn.ops[1] };

    if s1.kind != KIMM && s1.size != d.size {
        return 0;
    }
    if s2.kind != KIMM && s2.size != d.size {
        return 0;
    }
    if (rsp_is_ptr() != 0)
        && (dreg == OCERZ_RSP
            || (s1.kind == KREG && s1.reg as c_uint == OCERZ_RSP)
            || (s2.kind == KREG && s2.reg as c_uint == OCERZ_RSP))
    {
        return 0;
    }

    if need == 0 {
        let ds = pin_slot(dreg);
        if ds >= 0 {
            let mut src: [c_int; 2] = [0; 2];
            let ops: [&X86Operand; 2] = [s1, s2];
            let mut fused_hs: c_int = -1;
            if s1.kind == KREG
                && s1.high8 == 0
                && s1.reg == d.reg
                && (s2.kind == KIMM || (s2.kind == KREG && s2.high8 == 0))
            {
                let _ = fuse_prev_mov(b, dreg, d.size as c_int, &mut fused_hs, insn);
            }
            let first_mem = ops[1].kind == KMEM;
            for k in 0..2usize {
                let i = if first_mem { 1 - k } else { k };
                if i == 0 && fused_hs >= 0 {
                    src[0] = fused_hs;
                    continue;
                }
                let oi = ops[i];
                let tmp = if i != 0 { JT1 } else { JT0 };
                if oi.kind == KREG {
                    if oi.high8 != 0 {
                        return 0;
                    }
                    let ps = pin_slot(oi.reg as c_uint);
                    if ps >= 0 {
                        src[i] = pin_hreg(ps);
                    } else {
                        emit_gpr_rd(b, sf, tmp, oi.reg as c_uint);
                        src[i] = tmp;
                    }
                } else if oi.kind == KIMM {
                    let mut v = oi.imm;
                    if sf == 0 {
                        v &= 0xffffffff;
                    }
                    a64_mov_imm64(b, tmp, v);
                    src[i] = tmp;
                } else if oi.kind == KMEM {
                    if emit_mem_load_any(b, insn, oi, d.size as c_int, tmp) == 0 {
                        return 0;
                    }
                    src[i] = tmp;
                } else {
                    return 0;
                }
            }
            a64_mul(b, sf, pin_hreg(ds), src[0], src[1]);
            return 1;
        }
    }

    a64_str(b, 4, A64_ZR, 20, CC_OP_OFF);
    if s2.kind == KMEM && emit_imul_src(b, insn, s2, JT1) == 0 {
        return 0;
    }
    if emit_imul_src(b, insn, s1, JT0) == 0 {
        return 0;
    }
    if s2.kind != KMEM && emit_imul_src(b, insn, s2, JT1) == 0 {
        return 0;
    }

    a64_mul(b, 1, JT2, JT0, JT1);
    if sf != 0 && need & (OCERZ_CF | OCERZ_OF) != 0 {
        a64_smulh(b, JTA, JT0, JT1);
    }

    if sf != 0 {
        emit_gpr_wr(b, JT2, dreg);
    } else {
        a64_mov_reg(b, 0, JTT, JT2);
        emit_gpr_wr(b, JTT, dreg);
    }

    if need == 0 {
        return 1;
    }

    a64_mov_imm64(b, JTF, 0);
    if need & (OCERZ_CF | OCERZ_OF) != 0 {
        if sf != 0 {
            a64_asr_imm(b, 1, JTU, JT2, 63);
            a64_subs_reg(b, 1, A64_ZR, JTA, JTU, 0);
        } else {
            a64_sxtw(b, JTU, JT2);
            a64_subs_reg(b, 1, A64_ZR, JT2, JTU, 0);
        }
        a64_cset(b, JTT, A64_NE);
        if need & OCERZ_CF != 0 {
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
        if need & OCERZ_OF != 0 {
            a64_lsl_imm(b, 0, JTU, JTT, 11);
            a64_orr_reg(b, 1, JTF, JTF, JTU, 0);
        }
    }
    emit_commit_flags(b, need);
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn stack_inline_enabled() -> c_int {
    static mut EN: c_int = -1;
    if EN < 0 {
        EN = if getenv_set(c"OCERZ_NO_INLINE_STACK") { 0 } else { 1 };
    }
    EN
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn m32_lowreg_ok() -> c_int {
    (g_xlat_mode32 != 0
        && ocerz_low_base != 0
        && ocerz_guest_base == 0
        && !(jgb_usable() != 0)
        && low_guard_fast_ok() != 0
        && !env_on!("OCERZ_NO_M32_LOWREG")) as c_int
}

unsafe fn emit_push_pop32(b: *mut A64Buf, insn: &X86Insn) -> c_int {
    let o = &insn.ops[0];
    let size = if insn.opsize != 0 { insn.opsize as c_int } else { 4 };

    if size != 4 {
        return 0;
    }
    if !(m32_stack_ok(insn) != 0) {
        return 0;
    }
    let hs = pin_hreg(pin_slot(OCERZ_RSP));
    let iop = insn.op as c_uint;

    if iop == OCERZ_OP_PUSH {
        if !(mem_native_store_ok() != 0) {
            return 0;
        }
        let rv;
        if o.kind == KREG {
            if o.high8 != 0 || o.size != 4 {
                return 0;
            }
            let vs = pin_slot(o.reg as c_uint);
            if vs >= 0 {
                rv = pin_hreg(vs);
            } else {
                emit_gpr_rd(b, 0, JT1, o.reg as c_uint);
                rv = JT1;
            }
        } else if o.kind == KIMM {
            a64_mov_imm64(b, JT1, o.imm as u32 as u64);
            rv = JT1;
        } else {
            return 0;
        }
        a64_sub_imm(b, 0, JTA, hs, 4);
        m32_stack_st(b, rv, JTA);
        a64_mov_reg(b, 0, hs, JTA);
        return 1;
    }

    if iop == OCERZ_OP_POP {
        if o.kind != KREG || o.high8 != 0 || o.size != 4 {
            return 0;
        }
        if o.reg as c_uint == OCERZ_RSP {
            m32_stack_ld(b, hs, hs);
            return 1;
        }
        let ds = pin_slot(o.reg as c_uint);
        let rd = if ds >= 0 { pin_hreg(ds) } else { JT1 };
        m32_stack_ld(b, rd, hs);
        a64_add_imm(b, 0, hs, hs, 4);
        if ds < 0 {
            emit_gpr_wr(b, JT1, o.reg as c_uint);
        }
        return 1;
    }
    0
}

unsafe fn emit_push_pop_mem32(b: *mut A64Buf, insn: &X86Insn, exit_sites: *mut *mut u32, n_exits: *mut c_int) -> c_int {
    let m = &insn.ops[0];
    if (if insn.opsize != 0 { insn.opsize as c_int } else { 4 }) != 4 || m.size != 4 {
        return 0;
    }
    if !(m32_stack_base_ok() != 0) || !(mem_native_store_ok() != 0) {
        return 0;
    }
    if insn.op as c_uint == OCERZ_OP_POP && (m.base as c_uint == OCERZ_RSP || m.index as c_uint == OCERZ_RSP) {
        return 0;
    }
    let hs = pin_hreg(pin_slot(OCERZ_RSP));
    if emit_mem_ea(b, insn, m, JTA) == 0 {
        return 0;
    }
    let _ = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
    emit_add_const(b, JTA, ocerz_guest_base.wrapping_sub(ea_fold()));
    if insn.op as c_uint == OCERZ_OP_PUSH {
        emit_guest_load_ordered(b, 4, JT1, JTA, JTU);
        a64_sub_imm(b, 0, JTA, hs, 4);
        m32_stack_st(b, JT1, JTA);
        a64_mov_reg(b, 0, hs, JTA);
    } else {
        m32_stack_ld(b, JT1, hs);
        emit_guest_store_ordered(b, 4, JT1, JTA, JTU);
        a64_add_imm(b, 0, hs, hs, 4);
    }
    1
}

unsafe fn emit_leave32(b: *mut A64Buf, insn: &X86Insn) -> c_int {
    if (if insn.opsize != 0 { insn.opsize as c_int } else { 4 }) != 4 {
        return 0;
    }
    if !(m32_stack_ok(insn) != 0) || pin_slot(OCERZ_RBP) < 0 {
        return 0;
    }
    let hs = pin_hreg(pin_slot(OCERZ_RSP));
    let hb = pin_hreg(pin_slot(OCERZ_RBP));
    a64_mov_reg(b, 0, hs, hb);
    m32_stack_ld(b, hb, hs);
    a64_add_imm(b, 0, hs, hs, 4);
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_push_pop(
    b: *mut A64Buf,
    insn: *const X86Insn,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    let insn = &*insn;
    let o = &insn.ops[0];
    let gbase = ocerz_guest_base;
    let oreg = o.reg as c_uint;

    if insn.mode32 != 0 {
        return emit_push_pop32(b, insn);
    }
    if stack_inline_enabled() == 0 {
        return 0;
    }
    if insn.opsize != 8 || insn.seg != SEG_NONE {
        return 0;
    }

    if insn.op as c_uint == OCERZ_OP_PUSH {
        if !(mem_native_store_ok() != 0) {
            return 0;
        }

        if o.kind == KREG {
            if o.high8 != 0 || o.size != 8 {
                return 0;
            }
        } else if o.kind == KIMM {
            if o.size != 8 {
                return 0;
            }
        } else {
            return 0;
        }

        if g_lowstack != 0 && (stack_plain_access_ok() != 0) {
            let hs = pin_hreg(pin_slot(OCERZ_RSP));
            let mut rv = JT1;
            if o.kind == KREG {
                let vs = pin_slot(oreg);
                if vs >= 0 && oreg != OCERZ_RSP {
                    rv = pin_hreg(vs);
                } else {
                    emit_gpr_rd(b, 1, JT1, oreg);
                }
            } else {
                a64_mov_imm64(b, JT1, o.imm);
            }
            a64_sub_imm(b, 1, JTA, hs, 8);
            a64_str_regoff(b, 8, rv, JTA, JGB, 0);
            a64_mov_reg(b, 1, hs, JTA);
            return 1;
        }
        if g_pin_class == 3
            && pin_slot(OCERZ_RSP) >= 0
            && (stack_plain_access_ok() != 0)
            && (jgb_usable() != 0)
            && !(stack_guard_needed() != 0)
        {
            let hs = pin_hreg(pin_slot(OCERZ_RSP));
            let rv;
            if o.kind == KREG {
                let vs = pin_slot(oreg);
                if vs >= 0 && !((rsp_is_ptr() != 0) && oreg == OCERZ_RSP) {
                    rv = pin_hreg(vs);
                } else {
                    emit_gpr_rd(b, 1, JT1, oreg);
                    rv = JT1;
                }
            } else {
                a64_mov_imm64(b, JT1, o.imm);
                rv = JT1;
            }
            if (stack_identity() != 0 || (rsp_is_ptr() != 0)) && rv != hs {
                a64_str_pre64(b, rv, hs, -8);
                return 1;
            }
            if !g_push_entry.is_null() && g_n_push_fix < JIT_MAX_BLOCK_INSNS as c_int && rv != hs {
                a64_sub_imm(b, 1, hs, hs, 8);
                let n = g_n_push_fix;
                g_n_push_fix += 1;
                *(&raw mut g_push_fix).cast::<u32>().add(n as usize) =
                    (a64_label(b) as *const u32).offset_from(g_push_entry as *const u32) as u32;
                a64_str_regoff(b, 8, rv, JGB, hs, 0);
                return 1;
            }
            a64_sub_imm(b, 1, JTA, hs, 8);
            a64_str_regoff(b, 8, rv, JGB, JTA, 0);
            a64_mov_reg(b, 1, hs, JTA);
            return 1;
        }
        if g_pin_class == 2 {
            let rs = pin_slot(OCERZ_RSP);
            let mut rv = JT1;
            c_assert(rs >= 0);
            if o.kind == KREG && oreg != OCERZ_RSP {
                let vs = pin_slot(oreg);
                if vs >= 0 {
                    rv = pin_hreg(vs);
                } else {
                    emit_gpr_rd(b, 1, JT1, oreg);
                }
            } else if o.kind == KREG {
                emit_gpr_rd(b, 1, JT1, oreg);
            } else {
                a64_mov_imm64(b, JT1, o.imm);
            }
            let mut skip: *mut u32 = null_mut();
            if (stack_plain_access_ok() != 0) && !(stack_guard_needed() != 0) {
                a64_str_pre64(b, rv, pin_hreg(rs), -8);
            } else {
                a64_sub_imm(b, 1, JTA, pin_hreg(rs), 8);
                skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
                g_ea_plain = (stack_plain_now() != 0) as _;
                emit_guest_store_ordered(b, 8, rv, JTA, JTU);
                a64_sub_imm(b, 1, pin_hreg(rs), pin_hreg(rs), 8);
            }
            patch_guard_skip(skip, a64_label(b));
            return 1;
        }

        if o.kind == KREG {
            emit_gpr_rd(b, 1, JT1, oreg);
        } else {
            a64_mov_imm64(b, JT1, o.imm);
        }

        emit_gpr_rd(b, 1, JT0, OCERZ_RSP);
        a64_sub_imm(b, 1, JTA, JT0, 8);
        emit_add_const(b, JTA, ea_fold());

        let skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, gbase.wrapping_sub(ea_fold()));

        g_ea_plain = (stack_plain_now() != 0) as _;
        emit_guest_store_ordered(b, 8, JT1, JTA, JTU);
        a64_sub_imm(b, 1, JT0, JT0, 8);
        emit_gpr_wr(b, JT0, OCERZ_RSP);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }

    if insn.op as c_uint == OCERZ_OP_POP {
        if o.kind != KREG || o.high8 != 0 || o.size != 8 {
            return 0;
        }

        if g_lowstack != 0 && (stack_plain_access_ok() != 0) && oreg != OCERZ_RSP {
            let hs = pin_hreg(pin_slot(OCERZ_RSP));
            let ds = pin_slot(oreg);
            let rd = if ds >= 0 { pin_hreg(ds) } else { JT1 };
            a64_ldr_regoff(b, 8, rd, hs, JGB, 0);
            a64_add_imm(b, 1, hs, hs, 8);
            if ds < 0 {
                emit_gpr_wr(b, JT1, oreg);
            }
            return 1;
        }
        if g_pin_class == 3
            && pin_slot(OCERZ_RSP) >= 0
            && (stack_plain_access_ok() != 0)
            && (jgb_usable() != 0)
            && !(stack_guard_needed() != 0)
            && oreg != OCERZ_RSP
            && pin_slot(oreg) >= 0
        {
            let hs = pin_hreg(pin_slot(OCERZ_RSP));
            if stack_identity() != 0 || (rsp_is_ptr() != 0) {
                a64_ldr_post64(b, pin_hreg(pin_slot(oreg)), hs, 8);
                return 1;
            }
            a64_ldr_regoff(b, 8, pin_hreg(pin_slot(oreg)), JGB, hs, 0);
            a64_add_imm(b, 1, hs, hs, 8);
            return 1;
        }
        if g_pin_class == 2 {
            let rs = pin_slot(OCERZ_RSP);
            let ds = if oreg == OCERZ_RSP { -1 } else { pin_slot(oreg) };
            let rd = if ds >= 0 { pin_hreg(ds) } else { JT1 };
            c_assert(rs >= 0);
            let mut skip: *mut u32 = null_mut();
            if (stack_plain_access_ok() != 0) && !(stack_guard_needed() != 0) {
                a64_ldr_post64(b, rd, pin_hreg(rs), 8);
            } else {
                skip = emit_commpage_guard(b, insn, pin_hreg(rs), exit_sites, n_exits);
                g_ea_plain = (stack_plain_now() != 0) as _;
                emit_guest_load_ordered(b, 8, rd, pin_hreg(rs), JTU);
                a64_add_imm(b, 1, pin_hreg(rs), pin_hreg(rs), 8);
            }
            if ds < 0 {
                emit_gpr_wr(b, JT1, oreg);
            }
            patch_guard_skip(skip, a64_label(b));
            return 1;
        }

        emit_gpr_rd(b, 1, JT0, OCERZ_RSP);
        a64_mov_reg(b, 1, JTA, JT0);
        emit_add_const(b, JTA, ea_fold());

        let skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, gbase.wrapping_sub(ea_fold()));
        g_ea_plain = (stack_plain_now() != 0) as _;
        emit_guest_load_ordered(b, 8, JT1, JTA, JTU);

        a64_add_imm(b, 1, JT0, JT0, 8);
        emit_gpr_wr(b, JT0, OCERZ_RSP);
        emit_gpr_wr(b, JT1, oreg);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_movsxd(
    b: *mut A64Buf,
    insn: *const X86Insn,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    let insn = &*insn;
    let d = &insn.ops[0];
    let s = &insn.ops[1];

    if d.kind != KREG || d.high8 != 0 || d.size != 8 {
        return 0;
    }
    if s.size != 4 {
        return 0;
    }
    let dreg = d.reg as c_uint;
    let sreg = s.reg as c_uint;

    let mut ds = pin_slot(dreg);
    if (rsp_is_ptr() != 0) && dreg == OCERZ_RSP {
        ds = -1;
    }
    if s.kind == KREG {
        if s.high8 != 0 {
            return 0;
        }
        let ss = pin_slot(sreg);
        if ds >= 0 && ss >= 0 && !((rsp_is_ptr() != 0) && sreg == OCERZ_RSP) {
            a64_sxtw(b, pin_hreg(ds), pin_hreg(ss));
            return 1;
        }
        emit_gpr_rd(b, 0, JT0, sreg);
        a64_sxtw(b, JT0, JT0);
        emit_gpr_wr(b, JT0, dreg);
        return 1;
    }
    if s.kind == KMEM {
        let mut ra: c_int = 0;
        let mut disp: u32 = 0;
        if ds >= 0 && emit_mem_ea_plain(b, insn, s, 4, &mut ra, &mut disp) != 0 {
            if mem_plain_access_ok(s) != 0 {
                a64_ldrsw(b, pin_hreg(ds), ra, disp);
            } else {
                emit_gpr_lds_at(b, 4, 1, pin_hreg(ds), ra, disp as i32);
            }
            return 1;
        }
        if emit_mem_ea(b, insn, s, JTA) == 0 {
            return 0;
        }
        let skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, ocerz_guest_base.wrapping_sub(ea_fold()));
        emit_guest_load_ordered(b, 4, JT1, JTA, JTU);
        a64_sxtw(b, JT1, JT1);
        emit_gpr_wr(b, JT1, dreg);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    0
}

unsafe fn emit_arith_mem_eager(
    b: *mut A64Buf,
    insn: &X86Insn,
    need: u64,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    let d = &insn.ops[0];
    let s = &insn.ops[1];
    if d.kind != KREG || d.high8 != 0 || (d.size != 4 && d.size != 8) {
        return 0;
    }
    if s.kind != KMEM || s.size != d.size {
        return 0;
    }
    let sf = (d.size == 8) as c_int;

    if emit_mem_ea(b, insn, s, JTA) == 0 {
        return 0;
    }
    let skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
    emit_add_const(b, JTA, ocerz_guest_base.wrapping_sub(ea_fold()));
    emit_guest_load_ordered(b, sz(sf), JT1, JTA, JTU);
    a64_ldr(b, sz(sf), JT0, 20, gpr_off(d.reg as c_uint));

    let op = insn.op as c_uint;
    let is_sub = op == OCERZ_OP_SUB || op == OCERZ_OP_CMP;
    let is_add = op == OCERZ_OP_ADD;
    let is_logic = op == OCERZ_OP_AND || op == OCERZ_OP_OR || op == OCERZ_OP_XOR || op == OCERZ_OP_TEST;
    let writes = op == OCERZ_OP_ADD || op == OCERZ_OP_SUB || op == OCERZ_OP_AND || op == OCERZ_OP_OR || op == OCERZ_OP_XOR;

    match op {
        OCERZ_OP_ADD => a64_adds_reg(b, sf, JT2, JT0, JT1, 0),
        OCERZ_OP_SUB | OCERZ_OP_CMP => a64_subs_reg(b, sf, JT2, JT0, JT1, 0),
        OCERZ_OP_AND | OCERZ_OP_TEST => a64_ands_reg(b, sf, JT2, JT0, JT1, 0),
        OCERZ_OP_OR => a64_orr_reg(b, sf, JT2, JT0, JT1, 0),
        OCERZ_OP_XOR => a64_eor_reg(b, sf, JT2, JT0, JT1, 0),
        _ => return 0,
    }
    if is_logic && op != OCERZ_OP_AND && op != OCERZ_OP_TEST {
        a64_subs_imm(b, sf, A64_ZR, JT2, 0);
    }

    if writes {
        a64_str(b, 8, JT2, 20, gpr_off(d.reg as c_uint));
    }

    if need != 0 {
        a64_mov_imm64(b, JTF, 0);
        emit_zf_sf(b, need);
        if need & OCERZ_PF != 0 {
            emit_pf(b, JT2);
        }
        if is_add || is_sub {
            if need & OCERZ_CF != 0 {
                a64_cset(b, JTT, if is_add { A64_CS } else { A64_CC });
                a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
            }
            if need & OCERZ_OF != 0 {
                a64_cset(b, JTT, A64_VS);
                a64_lsl_imm(b, 0, JTT, JTT, 11);
                a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
            }
            if need & OCERZ_AF != 0 {
                a64_eor_reg(b, 1, JTT, JT0, JT1, 0);
                a64_eor_reg(b, 1, JTT, JTT, JT2, 0);
                a64_ubfx(b, 1, JTT, JTT, 4, 1);
                a64_lsl_imm(b, 0, JTT, JTT, 4);
                a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
            }
        }
        emit_commit_flags(b, need);
    }
    patch_guard_skip(skip, a64_label(b));
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_arith_mem(
    b: *mut A64Buf,
    insn: *const X86Insn,
    need: u64,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    let insn = &*insn;
    if g_defer == 0 {
        return emit_arith_mem_eager(b, insn, need, exit_sites, n_exits);
    }
    let d = &insn.ops[0];
    let s = &insn.ops[1];
    if d.kind != KREG || d.high8 != 0 || (d.size != 4 && d.size != 8) {
        return 0;
    }
    if s.kind != KMEM || s.size != d.size {
        return 0;
    }
    let sf = (d.size == 8) as c_int;
    let dreg = d.reg as c_uint;

    let op = insn.op as c_uint;
    let is_sub = op == OCERZ_OP_SUB || op == OCERZ_OP_CMP;
    let is_add = op == OCERZ_OP_ADD;
    let is_logic = op == OCERZ_OP_AND || op == OCERZ_OP_OR || op == OCERZ_OP_XOR || op == OCERZ_OP_TEST;
    let writes = op == OCERZ_OP_ADD || op == OCERZ_OP_SUB || op == OCERZ_OP_AND || op == OCERZ_OP_OR || op == OCERZ_OP_XOR;

    if pin_slot(dreg) >= 0
        && !((rsp_is_ptr() != 0) && dreg == OCERZ_RSP)
        && emit_mem_load_plain(b, insn, s, sz(sf), JT1) != 0
    {
        let rd = pin_hreg(pin_slot(dreg));
        if g_nzcv_want != 0 {
            if is_add || is_sub {
                if need != 0 {
                    if sf != 0 {
                        a64_stp_off(b, rd, JT1, 20, CC_SRC_OFF as _);
                    } else {
                        a64_mov_reg(b, 0, JT0, rd);
                        a64_stp_off(b, JT0, JT1, 20, CC_SRC_OFF as _);
                    }
                    a64_mov_imm64(b, JTT, cc_pack(if is_add { OCERZ_CC_ADD } else { OCERZ_CC_SUB }, d.size, 0) as u64);
                    a64_str(b, 4, JTT, 20, CC_OP_OFF);
                }
                let rdst = if writes { rd } else { A64_ZR };
                if is_add {
                    a64_adds_reg(b, sf, rdst, rd, JT1, 0);
                } else {
                    a64_subs_reg(b, sf, rdst, rd, JT1, 0);
                }
                g_nzcv_kind = if is_add { OCERZ_CC_ADD } else { OCERZ_CC_SUB };
            } else {
                match op {
                    OCERZ_OP_TEST => a64_ands_reg(b, sf, JT2, rd, JT1, 0),
                    OCERZ_OP_AND => a64_ands_reg(b, sf, rd, rd, JT1, 0),
                    OCERZ_OP_OR => {
                        a64_orr_reg(b, sf, rd, rd, JT1, 0);
                        a64_ands_reg(b, sf, A64_ZR, rd, rd, 0);
                    }
                    _ => {
                        a64_eor_reg(b, sf, rd, rd, JT1, 0);
                        a64_ands_reg(b, sf, A64_ZR, rd, rd, 0);
                    }
                }
                if need != 0 {
                    let rr = if op == OCERZ_OP_TEST { JT2 } else { rd };
                    emit_defer_flags(b, cc_pack(OCERZ_CC_LOGIC, d.size, 0), rr, rr);
                }
                g_nzcv_kind = OCERZ_CC_LOGIC;
            }
            g_nzcv_from = g_cur_insn_idx;
            return 1;
        }
        if !writes {
            if need == 0 {
                return 1;
            }
            if is_sub {
                emit_defer_flags(b, cc_pack(OCERZ_CC_SUB, d.size, 0), rd, JT1);
                return 1;
            }
            a64_and_reg(b, sf, JT2, rd, JT1, 0);
            emit_defer_flags(b, cc_pack(OCERZ_CC_LOGIC, d.size, 0), JT2, JT2);
            return 1;
        }
        if need != 0 && (is_add || is_sub) {
            if sf != 0 {
                a64_stp_off(b, rd, JT1, 20, CC_SRC_OFF as _);
            } else {
                a64_mov_reg(b, 0, JT0, rd);
                a64_stp_off(b, JT0, JT1, 20, CC_SRC_OFF as _);
            }
            a64_mov_imm64(b, JTT, cc_pack(if is_add { OCERZ_CC_ADD } else { OCERZ_CC_SUB }, d.size, 0) as u64);
            a64_str(b, 4, JTT, 20, CC_OP_OFF);
        }
        match op {
            OCERZ_OP_ADD => a64_add_reg(b, sf, rd, rd, JT1, 0),
            OCERZ_OP_SUB => a64_sub_reg(b, sf, rd, rd, JT1, 0),
            OCERZ_OP_AND => a64_and_reg(b, sf, rd, rd, JT1, 0),
            OCERZ_OP_OR => a64_orr_reg(b, sf, rd, rd, JT1, 0),
            _ => a64_eor_reg(b, sf, rd, rd, JT1, 0),
        }
        if need != 0 && is_logic {
            emit_defer_flags(b, cc_pack(OCERZ_CC_LOGIC, d.size, 0), rd, rd);
        }
        return 1;
    }

    if emit_mem_ea(b, insn, s, JTA) == 0 {
        return 0;
    }
    let skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
    emit_add_const(b, JTA, ocerz_guest_base.wrapping_sub(ea_fold()));

    emit_guest_load_ordered(b, sz(sf), JT1, JTA, JTU);
    emit_gpr_rd(b, sf, JT0, dreg);

    match op {
        OCERZ_OP_ADD => a64_add_reg(b, sf, JT2, JT0, JT1, 0),
        OCERZ_OP_SUB | OCERZ_OP_CMP => a64_sub_reg(b, sf, JT2, JT0, JT1, 0),
        OCERZ_OP_AND | OCERZ_OP_TEST => a64_and_reg(b, sf, JT2, JT0, JT1, 0),
        OCERZ_OP_OR => a64_orr_reg(b, sf, JT2, JT0, JT1, 0),
        OCERZ_OP_XOR => a64_eor_reg(b, sf, JT2, JT0, JT1, 0),
        _ => return 0,
    }

    if writes {
        emit_gpr_wr(b, JT2, dreg);
    }

    if need != 0 {
        if is_add {
            emit_defer_flags(b, cc_pack(OCERZ_CC_ADD, d.size, 0), JT0, JT1);
        } else if is_sub {
            emit_defer_flags(b, cc_pack(OCERZ_CC_SUB, d.size, 0), JT0, JT1);
        } else {
            emit_defer_flags(b, cc_pack(OCERZ_CC_LOGIC, d.size, 0), JT2, JT2);
        }
    }

    patch_guard_skip(skip, a64_label(b));
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_lea(b: *mut A64Buf, insn: *const X86Insn) -> c_int {
    let insn = &*insn;
    let d = &insn.ops[0];
    let s = &insn.ops[1];
    if d.kind != KREG || d.high8 != 0 || (d.size != 4 && d.size != 8) {
        return 0;
    }
    if s.kind != KMEM {
        return 0;
    }
    let dreg = d.reg as c_uint;
    let sbase = s.base as c_uint;
    let sindex = s.index as c_uint;

    let ds = pin_slot(dreg);
    let bs = if sbase != OCERZ_REG_NONE { pin_slot(sbase) } else { -1 };
    let is = if sindex != OCERZ_REG_NONE { pin_slot(sindex) } else { -1 };
    let host_rsp_operand = (rsp_is_ptr() != 0) && (dreg == OCERZ_RSP || sbase == OCERZ_RSP || sindex == OCERZ_RSP);
    if ds >= 0 && s.riprel == 0 && bs >= 0 && !host_rsp_operand {
        let sf = (insn.addrsize == 8 && d.size == 8) as c_int;
        let disp = s.disp;
        let rd = pin_hreg(ds);
        if sindex == OCERZ_REG_NONE && (-4095..=4095).contains(&disp) {
            if disp >= 0 {
                a64_add_imm(b, sf, rd, pin_hreg(bs), disp as u32);
            } else {
                a64_sub_imm(b, sf, rd, pin_hreg(bs), disp.wrapping_neg() as u32);
            }
            return 1;
        }
        if is >= 0 && (-4095..=4095).contains(&disp) {
            a64_add_reg(b, sf, rd, pin_hreg(bs), pin_hreg(is), (s.scale & 3) as c_int);
            if disp > 0 {
                a64_add_imm(b, sf, rd, rd, disp as u32);
            } else if disp < 0 {
                a64_sub_imm(b, sf, rd, rd, disp.wrapping_neg() as u32);
            }
            return 1;
        }
    }
    if ds >= 0 && s.riprel == 0 && !host_rsp_operand && insn.addrsize == 8 && s.disp == 0 && bs >= 0 && is >= 0 {
        a64_add_reg(b, (d.size == 8) as c_int, pin_hreg(ds), pin_hreg(bs), pin_hreg(is), (s.scale & 3) as c_int);
        return 1;
    }
    if ds >= 0
        && s.riprel == 0
        && !host_rsp_operand
        && insn.addrsize == 8
        && sbase == OCERZ_REG_NONE
        && is >= 0
        && s.disp >= -4095
        && s.disp <= 4095
    {
        let sf = (d.size == 8) as c_int;
        let rd = pin_hreg(ds);
        if (s.scale & 3) == 0 {
            if s.disp > 0 {
                a64_add_imm(b, sf, rd, pin_hreg(is), s.disp as u32);
            } else if s.disp < 0 {
                a64_sub_imm(b, sf, rd, pin_hreg(is), s.disp.wrapping_neg() as u32);
            } else {
                a64_mov_reg(b, sf, rd, pin_hreg(is));
            }
        } else {
            a64_lsl_imm(b, sf, rd, pin_hreg(is), (s.scale & 3) as c_int);
            if s.disp > 0 {
                a64_add_imm(b, sf, rd, rd, s.disp as u32);
            } else if s.disp < 0 {
                a64_sub_imm(b, sf, rd, rd, s.disp.wrapping_neg() as u32);
            }
        }
        return 1;
    }

    if s.riprel != 0 {
        a64_mov_imm64(b, JT2, s.disp as u64);
    } else {
        a64_mov_imm64(b, JT2, s.disp as u64);
        if sbase != OCERZ_REG_NONE {
            emit_gpr_rd(b, 1, JT0, sbase);
            a64_add_reg(b, 1, JT2, JT2, JT0, 0);
        }
        if sindex != OCERZ_REG_NONE {
            emit_gpr_rd(b, 1, JT0, sindex);
            a64_add_reg(b, 1, JT2, JT2, JT0, (s.scale & 3) as c_int);
        }
        if insn.addrsize == 4 {
            a64_mov_reg(b, 0, JT2, JT2);
        }
    }
    if d.size == 4 {
        a64_mov_reg(b, 0, JT2, JT2);
    }
    emit_gpr_wr(b, JT2, dreg);
    1
}

#[unsafe(no_mangle)]
pub static mut g_cc_direct: c_int = -1;

#[inline(always)]
fn sext_imm(s: &X86Operand, sf: c_int) -> u64 {
    let v = crate::inline::ocerz_sext(s.imm, s.size as _) as u64;
    if sf != 0 { v } else { v & 0xffffffff }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_adc_sbb(b: *mut A64Buf, insn: *const X86Insn, need: u64) -> c_int {
    let insn = &*insn;
    let d = &insn.ops[0];
    let s = &insn.ops[1];
    if g_defer == 0 {
        return 0;
    }
    if d.kind != KREG || d.high8 != 0 || (d.size != 4 && d.size != 8) {
        return 0;
    }
    let dreg = d.reg as c_uint;
    let sreg = s.reg as c_uint;
    if (rsp_is_ptr() != 0) && dreg == OCERZ_RSP {
        return 0;
    }
    if s.kind == KREG {
        if s.high8 != 0 || s.size != d.size {
            return 0;
        }
    } else if s.kind != KIMM {
        return 0;
    }
    let sf = (d.size == 8) as c_int;
    let is_sbb = insn.op as c_uint == OCERZ_OP_SBB;
    if need == 0
        && pin_slot(dreg) >= 0
        && (s.kind == KIMM || (pin_slot(sreg) >= 0 && !((rsp_is_ptr() != 0) && sreg == OCERZ_RSP)))
    {
        let rd = pin_hreg(pin_slot(dreg));
        emit_cc_predicate_ex(b, OCERZ_CC_B as _, 1);
        a64_cset(b, JTT, if g_cc_direct >= 0 { g_cc_direct } else { A64_NE });
        if s.kind == KREG {
            let rs = pin_hreg(pin_slot(sreg));
            if is_sbb {
                a64_sub_reg(b, sf, rd, rd, rs, 0);
            } else {
                a64_add_reg(b, sf, rd, rd, rs, 0);
            }
        } else {
            let v = sext_imm(s, sf);
            if v <= 4095 {
                if is_sbb {
                    a64_sub_imm(b, sf, rd, rd, v as u32);
                } else {
                    a64_add_imm(b, sf, rd, rd, v as u32);
                }
            } else {
                a64_mov_imm64(b, JT1, v);
                if is_sbb {
                    a64_sub_reg(b, sf, rd, rd, JT1, 0);
                } else {
                    a64_add_reg(b, sf, rd, rd, JT1, 0);
                }
            }
        }
        if is_sbb {
            a64_sub_reg(b, sf, rd, rd, JTT, 0);
        } else {
            a64_add_reg(b, sf, rd, rd, JTT, 0);
        }
        return 1;
    }
    emit_cc_predicate(b, OCERZ_CC_B);
    a64_cset(b, JTT, A64_NE);
    emit_gpr_rd(b, sf, JT0, dreg);
    if s.kind == KREG {
        emit_gpr_rd(b, sf, JT1, sreg);
    } else {
        a64_mov_imm64(b, JT1, sext_imm(s, sf));
    }
    if is_sbb {
        a64_sub_reg(b, sf, JT2, JT0, JT1, 0);
        a64_sub_reg(b, sf, JT2, JT2, JTT, 0);
    } else {
        a64_add_reg(b, sf, JT2, JT0, JT1, 0);
        a64_add_reg(b, sf, JT2, JT2, JTT, 0);
    }
    emit_gpr_wr(b, JT2, dreg);
    if need != 0 {
        let k = if is_sbb { OCERZ_CC_SUB } else { OCERZ_CC_ADD };
        a64_mov_imm64(b, JTU, cc_pack(k, d.size, 0) as u64);
        a64_mov_imm64(b, JTA, cc_pack(k, d.size, 1) as u64);
        a64_subs_imm(b, 1, A64_ZR, JTT, 0);
        a64_csel(b, 1, JTU, JTA, JTU, A64_NE);
        a64_stp_off(b, JT0, JT1, 20, CC_SRC_OFF as _);
        a64_str(b, 4, JTU, 20, CC_OP_OFF);
    }
    1
}

#[inline(always)]
unsafe fn narrow_alu(b: *mut A64Buf, op: c_uint, rn: c_int, rm: c_int) {
    match op {
        OCERZ_OP_ADD => a64_add_reg(b, 0, JT2, rn, rm, 0),
        OCERZ_OP_SUB => a64_sub_reg(b, 0, JT2, rn, rm, 0),
        OCERZ_OP_AND => a64_and_reg(b, 0, JT2, rn, rm, 0),
        OCERZ_OP_OR => a64_orr_reg(b, 0, JT2, rn, rm, 0),
        _ => a64_eor_reg(b, 0, JT2, rn, rm, 0),
    }
}

#[inline(always)]
unsafe fn uxt(b: *mut A64Buf, size: c_int, rd: c_int, rn: c_int) {
    if size == 1 {
        a64_uxtb(b, rd, rn);
    } else {
        a64_uxth(b, rd, rn);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_arith_narrow(b: *mut A64Buf, insn: *const X86Insn, need: u64) -> c_int {
    let insn = &*insn;
    let d = &insn.ops[0];
    let s = &insn.ops[1];
    if g_defer == 0 {
        return 0;
    }
    if d.kind != KREG || d.high8 != 0 || (d.size != 1 && d.size != 2) {
        return 0;
    }
    let dreg = d.reg as c_uint;
    let sreg = s.reg as c_uint;
    if (rsp_is_ptr() != 0) && dreg == OCERZ_RSP {
        return 0;
    }
    let mut s_mem = false;
    if s.kind == KREG {
        if s.high8 != 0 || s.size != d.size {
            return 0;
        }
    } else if s.kind == KMEM {
        if s.size != d.size || insn.seg != SEG_NONE {
            return 0;
        }
        s_mem = true;
    } else if s.kind != KIMM {
        return 0;
    }
    let op = insn.op as c_uint;
    if op != OCERZ_OP_ADD && op != OCERZ_OP_SUB && op != OCERZ_OP_AND && op != OCERZ_OP_OR && op != OCERZ_OP_XOR {
        return 0;
    }
    let size = d.size as c_int;
    let bits = size * 8;
    let mask: u64 = if size == 1 { 0xff } else { 0xffff };
    if s_mem {
        let ds = pin_slot(dreg);
        if ds < 0 {
            return 0;
        }
        if emit_mem_load_plain(b, insn, s, size, JT1) == 0 {
            if emit_mem_ea(b, insn, s, JTA) == 0 {
                return 0;
            }
            let _ = emit_commpage_guard(b, insn, JTA, null_mut(), null_mut());
            emit_add_const(b, JTA, ocerz_guest_base.wrapping_sub(ea_fold()));
            emit_guest_load_ordered(b, size, JT1, JTA, JTU);
        }
        let rd = pin_hreg(ds);
        if need == 0 {
            narrow_alu(b, op, rd, JT1);
            a64_bfi(b, 1, rd, JT2, 0, bits);
            return 1;
        }
        uxt(b, size, JTT, rd);
        narrow_alu(b, op, JTT, JT1);
        a64_bfi(b, 1, rd, JT2, 0, bits);
        if op == OCERZ_OP_ADD {
            emit_defer_flags(b, cc_pack(OCERZ_CC_ADD, d.size, 0), JTT, JT1);
        } else if op == OCERZ_OP_SUB {
            emit_defer_flags(b, cc_pack(OCERZ_CC_SUB, d.size, 0), JTT, JT1);
        } else {
            uxt(b, size, JT2, JT2);
            emit_defer_flags(b, cc_pack(OCERZ_CC_LOGIC, d.size, 0), JT2, JT2);
        }
        return 1;
    }
    if need == 0 && pin_slot(dreg) >= 0 && !((rsp_is_ptr() != 0) && dreg == OCERZ_RSP) {
        let rd = pin_hreg(pin_slot(dreg));
        let rm;
        if s.kind == KREG {
            let ss = pin_slot(sreg);
            if ss >= 0 && !((rsp_is_ptr() != 0) && sreg == OCERZ_RSP) {
                rm = pin_hreg(ss);
            } else {
                emit_gpr_rd(b, 1, JT1, sreg);
                rm = JT1;
            }
        } else {
            let v = s.imm & mask;
            match op {
                OCERZ_OP_ADD if v <= 4095 => {
                    a64_add_imm(b, 0, JT2, rd, v as u32);
                    a64_bfi(b, 1, rd, JT2, 0, bits);
                    return 1;
                }
                OCERZ_OP_SUB if v <= 4095 => {
                    a64_sub_imm(b, 0, JT2, rd, v as u32);
                    a64_bfi(b, 1, rd, JT2, 0, bits);
                    return 1;
                }
                _ => {}
            }
            a64_mov_imm64(b, JT1, v);
            rm = JT1;
        }
        narrow_alu(b, op, rd, rm);
        a64_bfi(b, 1, rd, JT2, 0, bits);
        return 1;
    }
    emit_gpr_rd(b, 1, JT0, dreg);
    uxt(b, size, JTT, JT0);
    if s.kind == KREG {
        emit_gpr_rd(b, 1, JT1, sreg);
        uxt(b, size, JT1, JT1);
    } else {
        a64_mov_imm64(b, JT1, s.imm & mask);
    }
    narrow_alu(b, op, JTT, JT1);
    a64_bfi(b, 1, JT0, JT2, 0, bits);
    emit_gpr_wr(b, JT0, dreg);
    if need != 0 {
        if op == OCERZ_OP_ADD {
            emit_defer_flags(b, cc_pack(OCERZ_CC_ADD, d.size, 0), JTT, JT1);
        } else if op == OCERZ_OP_SUB {
            emit_defer_flags(b, cc_pack(OCERZ_CC_SUB, d.size, 0), JTT, JT1);
        } else {
            uxt(b, size, JT2, JT2);
            emit_defer_flags(b, cc_pack(OCERZ_CC_LOGIC, d.size, 0), JT2, JT2);
        }
    }
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_cbw_cwd(b: *mut A64Buf, insn: *const X86Insn) -> c_int {
    let insn = &*insn;
    if g_pin_class == 2 {
        return 0;
    }
    if insn.op as c_uint == OCERZ_OP_CBW {
        emit_gpr_rd(b, 1, JT0, OCERZ_RAX);
        if insn.opsize == 2 {
            a64_sxtb(b, 0, JT1, JT0);
            a64_bfi(b, 1, JT0, JT1, 0, 16);
        } else if insn.opsize == 4 {
            a64_sxth(b, 0, JT0, JT0);
        } else {
            a64_sxtw(b, JT0, JT0);
        }
        emit_gpr_wr(b, JT0, OCERZ_RAX);
        return 1;
    }
    if insn.opsize != 2 && pin_slot(OCERZ_RAX) >= 0 && pin_slot(OCERZ_RDX) >= 0 && g_pin_class == 3 {
        let hax = pin_hreg(pin_slot(OCERZ_RAX));
        let hdx = pin_hreg(pin_slot(OCERZ_RDX));
        if insn.opsize == 4 {
            a64_asr_imm(b, 0, hdx, hax, 31);
        } else {
            a64_asr_imm(b, 1, hdx, hax, 63);
        }
        return 1;
    }
    emit_gpr_rd(b, 1, JT0, OCERZ_RAX);
    if insn.opsize == 2 {
        emit_gpr_rd(b, 1, JT1, OCERZ_RDX);
        a64_sbfx(b, 1, JT0, JT0, 15, 1);
        a64_bfi(b, 1, JT1, JT0, 0, 16);
        emit_gpr_wr(b, JT1, OCERZ_RDX);
    } else if insn.opsize == 4 {
        a64_asr_imm(b, 0, JT0, JT0, 31);
        emit_gpr_wr(b, JT0, OCERZ_RDX);
    } else {
        a64_asr_imm(b, 1, JT0, JT0, 63);
        emit_gpr_wr(b, JT0, OCERZ_RDX);
    }
    1
}

#[unsafe(no_mangle)]
pub static mut g_div_prev_skipped: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_oolslow_pre: u32 = 0;

#[inline(always)]
fn is_xor_rdx_rdx(p: &X86Insn) -> bool {
    p.op as c_uint == OCERZ_OP_XOR
        && p.nops == 2
        && p.ops[0].kind == KREG
        && p.ops[1].kind == KREG
        && p.ops[0].reg as c_uint == OCERZ_RDX
        && p.ops[1].reg as c_uint == OCERZ_RDX
        && p.ops[0].high8 == 0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rdx_prep_skippable(insns: *const X86Insn, i: c_int, n: c_int, need: u64) -> c_int {
    static mut DIS: c_int = -1;
    if DIS < 0 {
        DIS = if libc::getenv(c"OCERZ_NO_RDXSKIP".as_ptr()).is_null() { 0 } else { 1 };
    }
    if DIS != 0 || insns.is_null() || i + 1 >= n || need != 0 || g_pin_class != 3 {
        return 0;
    }
    if pin_slot(OCERZ_RAX) < 0 || pin_slot(OCERZ_RDX) < 0 {
        return 0;
    }
    let p = &*insns.add(i as usize);
    let d = &*insns.add(i as usize + 1);
    if d.seg != SEG_NONE || d.nops < 1 {
        return 0;
    }
    let o = &d.ops[0];
    if o.kind != KREG || o.high8 != 0 || (o.size != 4 && o.size != 8) || pin_slot(o.reg as c_uint) < 0 {
        return 0;
    }
    if env_on!("OCERZ_NO_INLINE_DIV") {
        return 0;
    }
    if d.op as c_uint == OCERZ_OP_DIV {
        return (is_xor_rdx_rdx(p)
            && p.ops[1].high8 == 0
            && (p.ops[0].size == 4 || p.ops[0].size == 8)
            && p.ops[1].size == p.ops[0].size) as c_int;
    }
    if d.op as c_uint == OCERZ_OP_IDIV {
        return (p.op as c_uint == OCERZ_OP_CWD && p.opsize == o.size) as c_int;
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_div(
    b: *mut A64Buf,
    insn: *const X86Insn,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    let insn = &*insn;
    let o = &insn.ops[0];
    if o.size != 4 && o.size != 8 {
        return 0;
    }
    if g_pin_class == 2 {
        return 0;
    }
    if insn.seg != SEG_NONE {
        return 0;
    }
    let sf = (o.size == 8) as c_int;
    let is_idiv = insn.op as c_uint == OCERZ_OP_IDIV;
    let mut rdx_zero = false;
    let mut rdx_sext = false;
    if !g_cur_insns.is_null() && g_cur_insn_idx >= 1 {
        let pv = &*g_cur_insns.add(g_cur_insn_idx as usize - 1);
        if is_xor_rdx_rdx(pv) && (pv.ops[0].size == 4 || pv.ops[0].size == 8) {
            rdx_zero = true;
        }
        if pv.op as c_uint == OCERZ_OP_CWD && ((sf != 0 && pv.opsize == 8) || (sf == 0 && pv.opsize == 4)) {
            rdx_sext = true;
        }
    }
    let prep_skipped = g_div_prev_skipped;
    g_div_prev_skipped = 0;
    let mut pre_word: u32 = 0;
    if prep_skipped != 0 && pin_slot(OCERZ_RDX) >= 0 && pin_slot(OCERZ_RAX) >= 0 {
        let hdx0 = pin_hreg(pin_slot(OCERZ_RDX));
        let hax0 = pin_hreg(pin_slot(OCERZ_RAX));
        if rdx_zero {
            pre_word = 0xaa1f03e0 | hdx0 as u32;
        } else {
            pre_word = (if sf != 0 { 0x9340fc00u32 } else { 0x13007c00u32 }) | ((hax0 as u32) << 5) | hdx0 as u32;
        }
    }
    let mut hdv = JT2;
    if o.kind == KMEM {
        if emit_mem_ea(b, insn, o, JTA) == 0 {
            return 0;
        }
        let skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, ocerz_guest_base.wrapping_sub(ea_fold()));
        emit_guest_load_ordered(b, o.size as c_int, JT2, JTA, JTU);
        patch_guard_skip(skip, a64_label(b));
    } else if o.kind == KREG {
        if o.high8 != 0 {
            return 0;
        }
        if pin_slot(o.reg as c_uint) >= 0 && g_pin_class == 3 && pin_slot(OCERZ_RAX) >= 0 && pin_slot(OCERZ_RDX) >= 0 {
            hdv = pin_hreg(pin_slot(o.reg as c_uint));
        } else {
            emit_gpr_rd(b, sf, JT2, o.reg as c_uint);
        }
    } else {
        return 0;
    }
    if pin_slot(OCERZ_RAX) >= 0 && pin_slot(OCERZ_RDX) >= 0 && g_pin_class == 3 {
        let hax = pin_hreg(pin_slot(OCERZ_RAX));
        let hdx = pin_hreg(pin_slot(OCERZ_RDX));
        let mut sites: [*mut u32; 4] = [null_mut(); 4];
        let mut ns: usize = 0;
        sites[ns] = a64_label(b);
        ns += 1;
        a64_cbz(b, sf, hdv, 0);
        if !is_idiv {
            if !rdx_zero {
                sites[ns] = a64_label(b);
                ns += 1;
                a64_cbnz(b, sf, hdx, 0);
            }
            a64_udiv(b, sf, JTT, hax, hdv);
        } else {
            if !rdx_sext {
                a64_asr_imm(b, sf, JTT, hax, if sf != 0 { 63 } else { 31 });
                a64_subs_reg(b, sf, A64_ZR, hdx, JTT, 0);
                sites[ns] = a64_label(b);
                ns += 1;
                a64_bcond(b, A64_NE, 0);
            }
            a64_subs_imm(b, sf, A64_ZR, hdv, 0);
            (*b).p = (*b).p.sub(1);
            a64_emit32(b, (if sf != 0 { 0xb100041fu32 } else { 0x3100041fu32 }) | ((hdv as u32) << 5));
            let not_m1 = a64_label(b);
            a64_bcond(b, A64_NE, 0);
            a64_try_eor_imm(b, sf, JTT, hax, if sf != 0 { 0x8000000000000000 } else { 0x80000000 });
            sites[ns] = a64_label(b);
            ns += 1;
            a64_cbz(b, sf, JTT, 0);
            a64_patch_bcond(not_m1, a64_label(b));
            a64_sdiv(b, sf, JTT, hax, hdv);
        }
        a64_msub(b, sf, hdx, JTT, hdv, hax);
        a64_mov_reg(b, sf, hax, JTT);
        g_oolslow_pre = pre_word;
        if oolslow_add(insn, sites.as_mut_ptr(), ns as c_int, a64_label(b)) != 0 {
            return 1;
        }
        g_oolslow_pre = 0;
        let done = a64_label(b);
        a64_b(b, 0);
        let slow = a64_label(b);
        for &site in sites.get_unchecked(..ns) {
            patch_any_branch(site, slow);
        }
        if pre_word != 0 {
            a64_emit32(b, pre_word);
        }
        emit_slowcall(b, insn, exit_sites, n_exits);
        a64_patch_b(done, a64_label(b));
        return 1;
    }
    emit_gpr_rd(b, sf, JT0, OCERZ_RAX);
    emit_gpr_rd(b, sf, JT1, OCERZ_RDX);
    let mut to_slow: [*mut u32; 2] = [null_mut(); 2];
    a64_subs_imm(b, sf, A64_ZR, JT2, 0);
    to_slow[0] = a64_label(b);
    a64_bcond(b, A64_EQ, 0);
    if is_idiv {
        a64_asr_imm(b, sf, JTT, JT0, if sf != 0 { 63 } else { 31 });
        a64_subs_reg(b, sf, A64_ZR, JT1, JTT, 0);
    } else {
        a64_subs_imm(b, sf, A64_ZR, JT1, 0);
    }
    to_slow[1] = a64_label(b);
    a64_bcond(b, A64_NE, 0);
    if is_idiv {
        a64_mov_imm64(b, JTT, if sf != 0 { 0x8000000000000000 } else { 0x80000000 });
        a64_subs_reg(b, sf, A64_ZR, JT0, JTT, 0);
        let not_min = a64_label(b);
        a64_bcond(b, A64_NE, 0);
        a64_mov_imm64(b, JTT, if sf != 0 { !0u64 } else { 0xffffffff });
        a64_subs_reg(b, sf, A64_ZR, JT2, JTT, 0);
        let to_slow3 = a64_label(b);
        a64_bcond(b, A64_EQ, 0);
        a64_patch_bcond(not_min, a64_label(b));
        a64_sdiv(b, sf, JTT, JT0, JT2);
        a64_msub(b, sf, JTU, JTT, JT2, JT0);
        emit_gpr_wr(b, JTT, OCERZ_RAX);
        emit_gpr_wr(b, JTU, OCERZ_RDX);
        let done = a64_label(b);
        a64_b(b, 0);
        let slow = a64_label(b);
        a64_patch_bcond(to_slow3, slow);
        for &site in &to_slow {
            a64_patch_bcond(site, slow);
        }
        emit_slowcall(b, insn, exit_sites, n_exits);
        a64_patch_b(done, a64_label(b));
    } else {
        a64_udiv(b, sf, JTT, JT0, JT2);
        a64_msub(b, sf, JTU, JTT, JT2, JT0);
        emit_gpr_wr(b, JTT, OCERZ_RAX);
        emit_gpr_wr(b, JTU, OCERZ_RDX);
        let done = a64_label(b);
        a64_b(b, 0);
        let slow = a64_label(b);
        for &site in &to_slow {
            a64_patch_bcond(site, slow);
        }
        emit_slowcall(b, insn, exit_sites, n_exits);
        a64_patch_b(done, a64_label(b));
    }
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_not_neg(b: *mut A64Buf, insn: *const X86Insn, need: u64) -> c_int {
    let insn = &*insn;
    let d = &insn.ops[0];
    let dreg = d.reg as c_uint;
    let is_not = insn.op as c_uint == OCERZ_OP_NOT;
    if d.kind == KREG
        && d.high8 == 0
        && (d.size == 1 || d.size == 2)
        && g_defer != 0
        && pin_slot(dreg) >= 0
        && !((rsp_is_ptr() != 0) && dreg == OCERZ_RSP)
    {
        let rd = pin_hreg(pin_slot(dreg));
        let bits = d.size as c_int * 8;
        if is_not {
            a64_try_eor_imm(b, 1, rd, rd, if d.size == 1 { 0xff } else { 0xffff });
            return 1;
        }
        uxt(b, d.size as c_int, JT0, rd);
        a64_neg_reg(b, 0, JT2, JT0);
        a64_bfi(b, 1, rd, JT2, 0, bits);
        if need != 0 {
            a64_mov_imm64(b, JT1, 0);
            emit_defer_flags(b, cc_pack(OCERZ_CC_SUB, d.size, 0), JT1, JT0);
        }
        return 1;
    }
    if d.kind != KREG || d.high8 != 0 || (d.size != 4 && d.size != 8) {
        return 0;
    }
    let sf = (d.size == 8) as c_int;
    let ds = pin_slot(dreg);
    if is_not {
        if ds >= 0 && !((rsp_is_ptr() != 0) && dreg == OCERZ_RSP) {
            a64_orn_reg(b, sf, pin_hreg(ds), A64_ZR, pin_hreg(ds), 0);
            return 1;
        }
        emit_gpr_rd(b, sf, JT0, dreg);
        a64_orn_reg(b, sf, JT2, A64_ZR, JT0, 0);
        emit_gpr_wr(b, JT2, dreg);
        return 1;
    }
    if g_defer == 0 && need != 0 {
        return 0;
    }
    emit_gpr_rd(b, sf, JT0, dreg);
    a64_sub_reg(b, sf, JT2, A64_ZR, JT0, 0);
    emit_gpr_wr(b, JT2, dreg);
    if need != 0 {
        a64_mov_imm64(b, JT1, 0);
        emit_defer_flags(b, cc_pack(OCERZ_CC_SUB, d.size, 0), JT1, JT0);
    }
    1
}

unsafe fn emit_shift_count(b: *mut A64Buf, insn: &X86Insn, sf: c_int, dst: c_int, const_cnt: &mut c_uint) -> c_int {
    let s = &insn.ops[1];
    let mask: c_uint = if sf != 0 { 63 } else { 31 };
    if s.kind == KIMM {
        *const_cnt = (s.imm & mask as u64) as c_uint;
        return 1;
    }
    if s.kind == KREG && s.reg as c_uint == OCERZ_RCX && s.high8 == 0 && s.size == 1 {
        emit_gpr_rd(b, 1, dst, OCERZ_RCX);
        a64_mov_imm64(b, JTU, mask as u64);
        a64_and_reg(b, 1, dst, dst, JTU, 0);
        *const_cnt = 0xffffffff;
        return 1;
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_rot(b: *mut A64Buf, insn: *const X86Insn, need: u64) -> c_int {
    let insn = &*insn;
    let d = &insn.ops[0];
    let dreg = d.reg as c_uint;
    if d.kind != KREG || d.high8 != 0 || (d.size != 4 && d.size != 8) {
        return 0;
    }
    if rsp_is_ptr() != 0 && dreg == OCERZ_RSP {
        return 0;
    }
    if insn.mode32 != 0 && insn.ops[1].kind != KIMM {
        return 0;
    }
    let sf = (d.size == 8) as c_int;
    let bits: c_int = if sf != 0 { 64 } else { 32 };
    let is_rol = insn.op as c_uint == OCERZ_OP_ROL;
    let mut cnt: c_uint = 0;
    if emit_shift_count(b, insn, sf, JT1, &mut cnt) == 0 {
        return 0;
    }
    let variable = cnt == 0xffffffff;
    if !variable && cnt == 0 {
        return 1;
    }
    if need != 0 && variable {
        return 0;
    }
    let ds = pin_slot(dreg);
    let rd = if ds >= 0 { pin_hreg(ds) } else { JT2 };
    if ds < 0 {
        emit_gpr_rd(b, sf, JT0, dreg);
    }
    let rn = if ds >= 0 { rd } else { JT0 };
    if variable {
        if is_rol {
            a64_mov_imm64(b, JTU, bits as u64);
            a64_sub_reg(b, 1, JT1, JTU, JT1, 0);
        }
        a64_rorv(b, sf, rd, rn, JT1);
    } else {
        let ub = bits as c_uint;
        let r: c_uint = if is_rol {
            (bits - (cnt % ub) as c_int) as c_uint % ub
        } else {
            cnt % ub
        };
        if r == 0 {
            a64_mov_reg(b, sf, rd, rn);
        } else {
            a64_extr(b, sf, rd, rn, rn, r as c_int);
        }
    }
    if ds < 0 {
        emit_gpr_wr(b, rd, dreg);
    }
    if need == 0 {
        return 1;
    }
    emit_materialize(b);
    a64_ldr(b, 8, JTT, 20, RF_OFF);
    if is_rol {
        a64_ubfx(b, 1, JT1, rd, 0, 1);
    } else {
        a64_ubfx(b, 1, JT1, rd, bits - 1, 1);
    }
    a64_mov_imm64(b, JTU, !OCERZ_CF & (if cnt == 1 { !OCERZ_OF } else { !0u64 }));
    a64_and_reg(b, 1, JTT, JTT, JTU, 0);
    a64_orr_reg(b, 1, JTT, JTT, JT1, 0);
    if cnt == 1 {
        if is_rol {
            a64_ubfx(b, 1, JTU, rd, bits - 1, 1);
            a64_eor_reg(b, 1, JTU, JTU, JT1, 0);
        } else {
            a64_ubfx(b, 1, JTU, rd, bits - 2, 1);
            a64_eor_reg(b, 1, JTU, JTU, JT1, 0);
        }
        a64_lsl_imm(b, 1, JTU, JTU, 11);
        a64_orr_reg(b, 1, JTT, JTT, JTU, 0);
    }
    a64_str(b, 8, JTT, 20, RF_OFF);
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_shift_cl(b: *mut A64Buf, insn: *const X86Insn, need: u64) -> c_int {
    let insn = &*insn;
    let d = &insn.ops[0];
    let s = &insn.ops[1];
    let dreg = d.reg as c_uint;
    if d.kind != KREG || d.high8 != 0 || (d.size != 4 && d.size != 8) {
        return 0;
    }
    if !(s.kind == KREG && s.reg as c_uint == OCERZ_RCX && s.high8 == 0 && s.size == 1) {
        return 0;
    }
    if rsp_is_ptr() != 0 && dreg == OCERZ_RSP {
        return 0;
    }
    if need != 0 {
        return 0;
    }
    if insn.mode32 != 0 {
        return 0;
    }
    let sf = (d.size == 8) as c_int;
    let mut cnt: c_uint = 0;
    if emit_shift_count(b, insn, sf, JT1, &mut cnt) == 0 {
        return 0;
    }
    let ds = pin_slot(dreg);
    let rd = if ds >= 0 { pin_hreg(ds) } else { JT2 };
    if ds < 0 {
        emit_gpr_rd(b, sf, JT0, dreg);
    }
    let rn = if ds >= 0 { rd } else { JT0 };
    match insn.op as c_uint {
        OCERZ_OP_SHL => a64_lslv(b, sf, rd, rn, JT1),
        OCERZ_OP_SHR => a64_lsrv(b, sf, rd, rn, JT1),
        OCERZ_OP_SAR => a64_asrv(b, sf, rd, rn, JT1),
        _ => return 0,
    }
    if ds < 0 {
        emit_gpr_wr(b, rd, dreg);
    }
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_cmov(
    b: *mut A64Buf,
    insn: *const X86Insn,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    let insn = &*insn;
    let d = &insn.ops[0];
    let s = &insn.ops[1];
    let dreg = d.reg as c_uint;
    let sreg = s.reg as c_uint;
    if d.kind != KREG || d.high8 != 0 || (d.size != 4 && d.size != 8) {
        return 0;
    }
    if rsp_is_ptr() != 0 && (dreg == OCERZ_RSP || (s.kind == KREG && sreg == OCERZ_RSP)) {
        return 0;
    }
    let sf = (d.size == 8) as c_int;
    if s.kind == KREG {
        if s.high8 != 0 || s.size != d.size {
            return 0;
        }
    } else if s.kind != KMEM {
        return 0;
    }
    if s.kind == KREG && pin_slot(dreg) >= 0 && pin_slot(sreg) >= 0 {
        emit_cc_predicate_ex(b, insn.cc as _, 1);
        let cond = if g_cc_direct >= 0 { g_cc_direct } else { A64_NE };
        let rd = pin_hreg(pin_slot(dreg));
        let rs = pin_hreg(pin_slot(sreg));
        if cond == A64_AL {
            if sf != 0 {
                a64_mov_reg(b, 1, rd, rs);
            } else {
                a64_mov_reg(b, 0, rd, rs);
            }
        } else if cond == A64_NV {
            if sf == 0 {
                a64_mov_reg(b, 0, rd, rd);
            }
        } else {
            a64_csel(b, sf, rd, rs, rd, cond);
        }
        return 1;
    }
    emit_cc_predicate(b, insn.cc as c_uint);
    a64_cset(b, JTF, A64_NE);
    if s.kind == KREG {
        emit_gpr_rd(b, sf, JT2, sreg);
    } else {
        let mut skip2: *mut u32 = null_mut();
        if emit_sse_mem_addr(b, insn, s, d.size as c_int, exit_sites, n_exits, &mut skip2) == 0 {
            return 0;
        }
        emit_sse_mem_ld_gpr(b, d.size as c_int, JT2);
        patch_guard_skip(skip2, a64_label(b));
    }
    a64_subs_imm(b, 1, A64_ZR, JTF, 0);
    emit_gpr_rd(b, sf, JT0, dreg);
    a64_csel(b, sf, JT0, JT2, JT0, A64_NE);
    if sf == 0 {
        a64_mov_reg(b, 0, JT0, JT0);
    }
    emit_gpr_wr(b, JT0, dreg);
    1
}

unsafe fn setcc_zx_partner(insn: &X86Insn) -> c_int {
    if g_cur_insns.is_null() || g_cur_insn_idx < 0 || env_on!("OCERZ_NO_SETCC_ZX") {
        return -1;
    }
    let r = (insn.ops[0].reg & 15) as c_uint;
    let mut k = g_cur_insn_idx + 1;
    while k < g_cur_insns_n && k <= g_cur_insn_idx + 4 {
        let t = &*g_cur_insns.add(k as usize);
        if t.op as c_uint == OCERZ_OP_MOVZX
            && t.nops == 2
            && t.ops[0].kind == KREG
            && t.ops[1].kind == KREG
            && (t.ops[0].size == 4 || t.ops[0].size == 8)
            && t.ops[1].size == 1
            && t.ops[1].high8 == 0
            && (t.ops[0].reg & 15) as c_uint == r
            && (t.ops[1].reg & 15) as c_uint == r
        {
            return if *(&raw const g_mov_skip).cast::<u8>().add(k as usize) != 0 { -1 } else { k };
        }
        if t.op as c_uint == OCERZ_OP_SETCC && t.ops[0].kind == KREG && t.ops[0].high8 == 0 && (t.ops[0].reg & 15) as c_uint != r {
            k += 1;
            continue;
        }
        if mov_sink_gap_ok(t, r, r) == 0 {
            return -1;
        }
        k += 1;
    }
    -1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_setcc(b: *mut A64Buf, insn: *const X86Insn) -> c_int {
    let insn = &*insn;
    let d = &insn.ops[0];
    let dreg = d.reg as c_uint;
    let seg = insn.seg as c_uint;
    if d.kind == KMEM
        && d.size == 1
        && insn.addrsize == 8
        && g_defer != 0
        && (seg == OCERZ_SEG_NONE as c_uint || seg == OCERZ_SEG_GS as c_uint || seg == OCERZ_SEG_FS as c_uint)
    {
        emit_cc_predicate_ex(b, insn.cc as _, 1);
        a64_cset(b, JT2, if g_cc_direct >= 0 { g_cc_direct } else { A64_NE });
        if mem_native_store_ok() != 0 && emit_plain_mem_fast(b, insn, d, 1, JT2, 1, 0) != 0 {
            return 1;
        }
        if emit_mem_ea(b, insn, d, JTA) == 0 {
            return 0;
        }
        let _ = emit_commpage_guard(b, insn, JTA, null_mut(), null_mut());
        emit_add_const(b, JTA, ocerz_guest_base.wrapping_sub(ea_fold()));
        emit_guest_store_ordered(b, 1, JT2, JTA, JTU);
        return 1;
    }
    if d.kind != KREG || d.high8 != 0 || d.size != 1 {
        return 0;
    }
    if rsp_is_ptr() != 0 && dreg == OCERZ_RSP {
        return 0;
    }
    emit_cc_predicate_ex(b, insn.cc as _, 1);
    let zx = if pin_slot(dreg) >= 0 { setcc_zx_partner(insn) } else { -1 };
    if zx >= 0 {
        a64_cset(b, pin_hreg(pin_slot(dreg)), if g_cc_direct >= 0 { g_cc_direct } else { A64_NE });
        *(&raw mut g_mov_skip).cast::<u8>().add(zx as usize) = 1;
        return 1;
    }
    a64_cset(b, JT2, if g_cc_direct >= 0 { g_cc_direct } else { A64_NE });
    if pin_slot(dreg) >= 0 {
        a64_bfi(b, 1, pin_hreg(pin_slot(dreg)), JT2, 0, 8);
        return 1;
    }
    emit_gpr_rd(b, 1, JT0, dreg);
    a64_bfi(b, 1, JT0, JT2, 0, 8);
    emit_gpr_wr(b, JT0, dreg);
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_bswap(b: *mut A64Buf, insn: *const X86Insn) -> c_int {
    let insn = &*insn;
    let d = &insn.ops[0];
    let dreg = d.reg as c_uint;
    if d.kind != KREG || d.high8 != 0 || (d.size != 4 && d.size != 8) {
        return 0;
    }
    if rsp_is_ptr() != 0 && dreg == OCERZ_RSP {
        return 0;
    }
    let sf = (d.size == 8) as c_int;
    let ds = pin_slot(dreg);
    if ds >= 0 {
        a64_rev(b, sf, pin_hreg(ds), pin_hreg(ds));
        return 1;
    }
    emit_gpr_rd(b, sf, JT0, dreg);
    a64_rev(b, sf, JT2, JT0);
    emit_gpr_wr(b, JT2, dreg);
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_bitscan(b: *mut A64Buf, insn: *const X86Insn, need: u64) -> c_int {
    let insn = &*insn;
    if g_defer == 0 || insn.nops != 2 || insn.seg != SEG_NONE {
        return 0;
    }
    if need != 0 {
        return 0;
    }
    let d = &insn.ops[0];
    let s = &insn.ops[1];
    let dreg = d.reg as c_uint;
    if d.kind != KREG || d.high8 != 0 || (d.size != 4 && d.size != 8) {
        return 0;
    }
    if s.size != d.size {
        return 0;
    }
    let ds = pin_slot(dreg);
    if ds < 0 || (rsp_is_ptr() != 0 && dreg == OCERZ_RSP) {
        return 0;
    }
    let sf = (d.size == 8) as c_int;
    let rs;
    if s.kind == KREG {
        if s.high8 != 0 {
            return 0;
        }
        let ss = pin_slot(s.reg as c_uint);
        if ss < 0 || (rsp_is_ptr() != 0 && s.reg as c_uint == OCERZ_RSP) {
            return 0;
        }
        rs = pin_hreg(ss);
    } else if s.kind == KMEM {
        if emit_mem_load_any(b, insn, s, d.size as c_int, JT1) == 0 {
            return 0;
        }
        rs = JT1;
    } else {
        return 0;
    }
    let rd = pin_hreg(ds);
    match insn.op as c_uint {
        OCERZ_OP_BSF | OCERZ_OP_BSR => {
            if insn.op as c_uint == OCERZ_OP_BSF {
                a64_rbit(b, sf, JT0, rs);
                a64_clz(b, sf, JT0, JT0);
            } else {
                a64_clz(b, sf, JT0, rs);
                if sf != 0 {
                    a64_try_eor_imm(b, 1, JT0, JT0, 63);
                } else {
                    a64_try_eor_imm(b, 0, JT0, JT0, 31);
                }
            }
            a64_subs_imm(b, sf, A64_ZR, rs, 0);
            a64_csel(b, 1, rd, rd, JT0, A64_EQ);
            if g_nzcv_want != 0 {
                g_nzcv_kind = OCERZ_CC_SUB;
                g_nzcv_from = g_cur_insn_idx;
            }
            1
        }
        OCERZ_OP_TZCNT => {
            a64_rbit(b, sf, rd, rs);
            a64_clz(b, sf, rd, rd);
            1
        }
        OCERZ_OP_LZCNT => {
            a64_clz(b, sf, rd, rs);
            1
        }
        OCERZ_OP_POPCNT => {
            a64_fmov_v_from_x(b, sf, VX0 as c_int, rs);
            a64_v_cnt_8b(b, VX0 as c_int, VX0 as c_int);
            a64_addv_b_8b(b, VX0 as c_int, VX0 as c_int);
            a64_umov_w_b(b, rd, VX0 as c_int, 0);
            1
        }
        _ => 0,
    }
}

#[inline(always)]
unsafe fn bt_reg_bad(o: &X86Operand) -> bool {
    o.high8 != 0 || pin_slot(o.reg as c_uint) < 0 || (rsp_is_ptr() != 0 && o.reg as c_uint == OCERZ_RSP)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_bt(
    b: *mut A64Buf,
    insn: *const X86Insn,
    need: u64,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    let insn = &*insn;
    if g_defer == 0 || insn.nops != 2 || insn.seg != SEG_NONE {
        return 0;
    }
    if insn.addrsize != 8 && !(insn.addrsize == 4 && insn.mode32 != 0) {
        return 0;
    }
    let d = &insn.ops[0];
    let o = &insn.ops[1];
    let size = d.size as c_int;
    if size != 2 && size != 4 && size != 8 {
        return 0;
    }
    let op = insn.op as c_uint;
    if op != OCERZ_OP_BT {
        if d.kind != KREG || bt_reg_bad(d) {
            return 0;
        }
        if o.kind == KREG {
            if bt_reg_bad(o) {
                return 0;
            }
        } else if o.kind != KIMM {
            return 0;
        }
        if need != 0 {
            emit_materialize(b);
        }
        let rd = pin_hreg(pin_slot(d.reg as c_uint));
        let sf = (size == 8) as c_int;
        let bits = size as c_uint * 8;
        if o.kind == KIMM {
            let n = (o.imm as c_uint) & (bits - 1);
            a64_ubfx(b, 1, JT0, rd, n as c_int, 1);
            a64_mov_imm64(b, JT1, 1u64 << n);
        } else {
            a64_try_and_imm(b, 1, JT2, pin_hreg(pin_slot(o.reg as c_uint)), (bits - 1) as u64);
            a64_lsrv(b, 1, JT0, rd, JT2);
            a64_try_and_imm(b, 1, JT0, JT0, 1);
            a64_mov_imm64(b, JT1, 1);
            a64_lslv(b, 1, JT1, JT1, JT2);
        }
        if size == 2 {
            a64_uxth(b, JT2, rd);
            if op == OCERZ_OP_BTS {
                a64_orr_reg(b, 0, JT2, JT2, JT1, 0);
            } else if op == OCERZ_OP_BTR {
                a64_bic_reg(b, 0, JT2, JT2, JT1, 0);
            } else {
                a64_eor_reg(b, 0, JT2, JT2, JT1, 0);
            }
            a64_bfi(b, 1, rd, JT2, 0, 16);
        } else if op == OCERZ_OP_BTS {
            a64_orr_reg(b, sf, rd, rd, JT1, 0);
        } else if op == OCERZ_OP_BTR {
            a64_bic_reg(b, sf, rd, rd, JT1, 0);
        } else {
            a64_eor_reg(b, sf, rd, rd, JT1, 0);
        }
        if need != 0 {
            a64_ldr(b, 8, JT1, 20, RFLAGS_OFF);
            a64_bfi(b, 1, JT1, JT0, 0, 1);
            a64_str(b, 8, JT1, 20, RFLAGS_OFF);
        }
        if g_nzcv_want != 0 {
            a64_subs_imm(b, 1, A64_ZR, JT0, 0);
            g_nzcv_kind = NZCV_KIND_BT;
            g_nzcv_from = g_cur_insn_idx;
        }
        return 1;
    }
    if o.kind == KREG {
        if bt_reg_bad(o) {
            return 0;
        }
    } else if o.kind != KIMM {
        return 0;
    }
    let mut mat = need != 0 && g_nzcv_want == 0;
    if need != 0 && g_nzcv_want != 0 {
        mat = true;
    }
    if mat {
        emit_materialize(b);
    }
    let bits = size as c_uint * 8;
    if d.kind == KREG {
        if bt_reg_bad(d) {
            return 0;
        }
        let rd = pin_hreg(pin_slot(d.reg as c_uint));
        if o.kind == KIMM {
            let n = (o.imm as c_uint) & (bits - 1);
            a64_ubfx(b, 1, JT0, rd, n as c_int, 1);
        } else {
            a64_try_and_imm(b, 1, JT1, pin_hreg(pin_slot(o.reg as c_uint)), (bits - 1) as u64);
            a64_lsrv(b, 1, JT0, rd, JT1);
            a64_try_and_imm(b, 1, JT0, JT0, 1);
        }
    } else if d.kind == KMEM {
        if emit_mem_ea(b, insn, d, JTA) == 0 {
            return 0;
        }
        let _ = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, ocerz_guest_base.wrapping_sub(ea_fold()));
        if o.kind == KIMM {
            let n = (o.imm as c_uint) & (bits - 1);
            if n >> 3 != 0 {
                a64_add_imm(b, 1, JTA, JTA, n >> 3);
            }
            emit_guest_load_ordered(b, 1, JT0, JTA, JTU);
            a64_ubfx(b, 1, JT0, JT0, (n & 7) as c_int, 1);
        } else {
            let ro = pin_hreg(pin_slot(o.reg as c_uint));
            if o.size == 2 {
                a64_sxth(b, 1, JT1, ro);
            } else if o.size == 4 {
                a64_sxtw(b, JT1, ro);
            } else {
                a64_mov_reg(b, 1, JT1, ro);
            }
            a64_asr_imm(b, 1, JT2, JT1, 3);
            a64_add_reg(b, 1, JTA, JTA, JT2, 0);
            emit_guest_load_ordered(b, 1, JT0, JTA, JTU);
            a64_try_and_imm(b, 1, JT1, JT1, 7);
            a64_lsrv(b, 1, JT0, JT0, JT1);
            a64_try_and_imm(b, 1, JT0, JT0, 1);
        }
    } else {
        return 0;
    }
    if mat {
        a64_ldr(b, 8, JT1, 20, RFLAGS_OFF);
        a64_bfi(b, 1, JT1, JT0, 0, 1);
        a64_str(b, 8, JT1, 20, RFLAGS_OFF);
    }
    if g_nzcv_want != 0 {
        a64_subs_imm(b, 1, A64_ZR, JT0, 0);
        g_nzcv_kind = NZCV_KIND_BT;
        g_nzcv_from = g_cur_insn_idx;
    }
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_push_pop_mem(
    b: *mut A64Buf,
    insn: *const X86Insn,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    let insn = &*insn;
    if insn.mode32 != 0 {
        return emit_push_pop_mem32(b, insn, exit_sites, n_exits);
    }
    if g_pin_class != 3
        || pin_slot(OCERZ_RSP) < 0
        || stack_plain_access_ok() == 0
        || jgb_usable() == 0
        || stack_guard_needed() != 0
    {
        return 0;
    }
    if insn.nops != 1 || insn.ops[0].kind != KMEM || insn.ops[0].size != 8 {
        return 0;
    }
    let seg = insn.seg as c_uint;
    if insn.addrsize != 8
        || (seg != OCERZ_SEG_NONE as c_uint && seg != OCERZ_SEG_GS as c_uint && seg != OCERZ_SEG_FS as c_uint)
    {
        return 0;
    }
    let m = &insn.ops[0];
    let hs = pin_hreg(pin_slot(OCERZ_RSP));
    if insn.op as c_uint == OCERZ_OP_PUSH {
        if emit_mem_load_plain(b, insn, m, 8, JT1) == 0 {
            if emit_mem_ea(b, insn, m, JTA) == 0 {
                return 0;
            }
            let _ = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
            emit_add_const(b, JTA, ocerz_guest_base.wrapping_sub(ea_fold()));
            emit_guest_load_ordered(b, 8, JT1, JTA, JTU);
        }
        emit_push_pinned(b, hs, JT1);
        return 1;
    }
    if m.base as c_uint == OCERZ_RSP || m.index as c_uint == OCERZ_RSP {
        return 0;
    }
    if rsp_is_ptr() != 0 {
        a64_ldr(b, 8, JT1, hs, 0);
    } else {
        a64_ldr_regoff(b, 8, JT1, JGB, hs, 0);
    }
    if emit_plain_mem_fast(b, insn, m, 8, JT1, 1, 0) == 0 {
        if emit_mem_ea(b, insn, m, JTA) == 0 {
            return 0;
        }
        let _ = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, ocerz_guest_base.wrapping_sub(ea_fold()));
        emit_guest_store_ordered(b, 8, JT1, JTA, JTU);
    }
    a64_add_imm(b, 1, hs, hs, 8);
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_leave(
    b: *mut A64Buf,
    insn: *const X86Insn,
    _exit_sites: *mut *mut u32,
    _n_exits: *mut c_int,
) -> c_int {
    let insn = &*insn;
    if insn.mode32 != 0 {
        return emit_leave32(b, insn);
    }
    if g_pin_class != 3
        || pin_slot(OCERZ_RSP) < 0
        || pin_slot(OCERZ_RBP) < 0
        || stack_plain_access_ok() == 0
        || jgb_usable() == 0
        || stack_guard_needed() != 0
    {
        return 0;
    }
    let hs = pin_hreg(pin_slot(OCERZ_RSP));
    let hb = pin_hreg(pin_slot(OCERZ_RBP));
    if rsp_is_ptr() != 0 {
        a64_add_reg(b, 1, hs, hb, JGB, 0);
        a64_ldr_post64(b, hb, hs, 8);
        return 1;
    }
    a64_mov_reg(b, 1, hs, hb);
    if stack_identity() != 0 {
        a64_ldr_post64(b, hb, hs, 8);
        return 1;
    }
    a64_ldr_regoff(b, 8, hb, JGB, hs, 0);
    a64_add_imm(b, 1, hs, hs, 8);
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_shiftd(b: *mut A64Buf, insn: *const X86Insn, need: u64) -> c_int {
    let insn = &*insn;
    if need != 0 || g_defer == 0 || insn.nops != 3 {
        return 0;
    }
    let d = &insn.ops[0];
    let s = &insn.ops[1];
    let c = &insn.ops[2];
    if d.kind != KREG || d.high8 != 0 || (d.size != 4 && d.size != 8) {
        return 0;
    }
    if s.kind != KREG || s.high8 != 0 || s.size != d.size || c.kind != KIMM {
        return 0;
    }
    let dreg = d.reg as c_uint;
    let sreg = s.reg as c_uint;
    if pin_slot(dreg) < 0 || pin_slot(sreg) < 0 {
        return 0;
    }
    if rsp_is_ptr() != 0 && (dreg == OCERZ_RSP || sreg == OCERZ_RSP) {
        return 0;
    }
    let sf = (d.size == 8) as c_int;
    let bits = d.size as c_uint * 8;
    let cnt = (c.imm as c_uint) & (if sf != 0 { 63 } else { 31 });
    if cnt == 0 {
        return 1;
    }
    let rd = pin_hreg(pin_slot(dreg));
    let rs = pin_hreg(pin_slot(sreg));
    if insn.op as c_uint == OCERZ_OP_SHRD {
        a64_extr(b, sf, rd, rs, rd, cnt as c_int);
    } else {
        a64_extr(b, sf, rd, rd, rs, (bits - cnt) as c_int);
    }
    1
}

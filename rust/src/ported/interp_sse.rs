//! The SSE through SSE4.1 interpreter tier, and the AVX forms layered on it.
//!
//! A VEX.128 instruction is the legacy SSE op with its first source taken from
//! VEX.vvvv instead of the destination, and the upper half of the destination
//! ymm zeroed - so the legacy implementations are reused with the first source
//! chosen per encoding, and the merge forms take their untouched part from vvvv
//! rather than from dst.  A VEX.256 instruction is executed twice: once on the
//! low 128-bit halves, once with the involved registers swapped for their ymm
//! upper halves and memory operands 16 bytes further on.  Ops that cross the
//! 128-bit lanes - widening and narrowing converts, broadcasts, 128-bit
//! insert/extract, mask and test - cannot be done that way and are written out
//! in full.
//!
//! The pcmpXstrY string units follow the architectural imm8 layout directly
//! ([1:0] element format, [3:2] aggregation, [5:4] polarity, [6] index/mask
//! selection) rather than enumerating the named mnemonics, and the horizontal
//! adds are summed in the pairwise order the SDM specifies.  Equal-ordered
//! aggregation compares the needle only as far as the end of the 16-byte block,
//! so a match that starts near the end counts as a partial match there - SSE4.2
//! strstr relies on it to find a needle that straddles two blocks.  ocerz used to
//! call that a mismatch, so Steam never found "%language%" in the path
//! "public\\steambootstrapper_%language%.txt", loaded no localization and
//! stopped on an assert right after logging in; sse42_test's "tail" cases pin
//! it.  The JIT does not translate VEX at all, so everything here is also the
//! only implementation those instructions have.

use crate::ffi::*;
use crate::inline::{OCERZ_AF, OCERZ_CF, OCERZ_OF, OCERZ_PF, OCERZ_SF, OCERZ_ZF, ocerz_flag_assign};
use crate::interp_common::*;

const STEP_OK: i32 = OCERZ_STEP_OK as i32;
const EUNSUP: i32 = OCERZ_EUNSUP as i32;

#[derive(Clone, Copy)]
#[repr(C)]
union Vec {
    q: Ocerz128,
    f: [f32; 4],
    d: [f64; 2],
    i8: [i8; 16],
    u8: [u8; 16],
    i16: [i16; 8],
    u16: [u16; 8],
    i32: [i32; 4],
    u32: [u32; 4],
    i64: [i64; 2],
    u64: [u64; 2],
}

const ZQ: Ocerz128 = Ocerz128 { lo: 0, hi: 0 };
const ZV: Vec = Vec { q: ZQ };

#[inline(always)]
fn vec_of(q: Ocerz128) -> Vec {
    Vec { q }
}

#[inline(always)]
unsafe fn vec_read(cpu: &mut OcerzCPU, insn: &X86Insn, op: &X86Operand) -> Vec {
    unsafe { vec_of(ocerz_read_op128(cpu, insn, op)) }
}

#[inline(always)]
unsafe fn vec_write(cpu: &mut OcerzCPU, insn: &X86Insn, op: &X86Operand, v: Vec) {
    unsafe { ocerz_write_op128(cpu, insn, op, v.q) }
}

#[inline(always)]
fn src1_of(cpu: &OcerzCPU, insn: &X86Insn, d: &X86Operand) -> Vec {
    let r = if (insn.vex as u32 & OCERZ_VEX_NDS) != 0 { insn.vvvv } else { d.reg };
    vec_of(unsafe { *cpu.xmm.get_unchecked(r as usize) })
}

#[inline(always)]
fn xr(cpu: &OcerzCPU, r: u8) -> Ocerz128 {
    unsafe { *cpu.xmm.get_unchecked(r as usize) }
}

#[inline(always)]
fn xw(cpu: &mut OcerzCPU, r: u8) -> &mut Ocerz128 {
    unsafe { cpu.xmm.get_unchecked_mut(r as usize) }
}

#[inline(always)]
fn f2bits(f: f32) -> u32 {
    f.to_bits()
}

#[inline(always)]
fn d2bits(d: f64) -> u64 {
    d.to_bits()
}

#[inline(always)]
fn sat_s8(v: i32) -> i8 {
    v.clamp(-128, 127) as i8
}

#[inline(always)]
fn sat_u8(v: i32) -> u8 {
    v.clamp(0, 255) as u8
}

#[inline(always)]
fn sat_s16(v: i32) -> i16 {
    v.clamp(-32768, 32767) as i16
}

#[inline(always)]
fn sat_u16(v: i32) -> u16 {
    v.clamp(0, 65535) as u16
}

#[inline(always)]
fn minf_x86(a: f32, b: f32) -> f32 {
    if a < b { a } else { b }
}

#[inline(always)]
fn maxf_x86(a: f32, b: f32) -> f32 {
    if a > b { a } else { b }
}

#[inline(always)]
fn mind_x86(a: f64, b: f64) -> f64 {
    if a < b { a } else { b }
}

#[inline(always)]
fn maxd_x86(a: f64, b: f64) -> f64 {
    if a > b { a } else { b }
}

macro_rules! cmp_pred {
    ($a:expr, $b:expr, $pred:expr) => {{
        let a = $a;
        let b = $b;
        let unord = a.is_nan() || b.is_nan();
        match $pred & 15 {
            0 => !unord && a == b,
            1 => !unord && a < b,
            2 => !unord && a <= b,
            3 => unord,
            4 => unord || a != b,
            5 => unord || !(a < b),
            6 => unord || !(a <= b),
            7 => !unord,
            8 => unord || a == b,
            9 => unord || a < b,
            10 => unord || a <= b,
            11 => false,
            12 => !unord && a != b,
            13 => !unord && a >= b,
            14 => !unord && a > b,
            _ => true,
        }
    }};
}

fn cmp_pred_f(a: f32, b: f32, pred: i32) -> bool {
    cmp_pred!(a, b, pred)
}

fn cmp_pred_d(a: f64, b: f64, pred: i32) -> bool {
    cmp_pred!(a, b, pred)
}

fn set_comis_flags(cpu: &mut OcerzCPU, unord: bool, lt: bool, eq: bool) {
    unsafe {
        ocerz_flag_assign(cpu, OCERZ_OF, 0);
        ocerz_flag_assign(cpu, OCERZ_SF, 0);
        ocerz_flag_assign(cpu, OCERZ_AF, 0);
        if unord {
            ocerz_flag_assign(cpu, OCERZ_ZF, 1);
            ocerz_flag_assign(cpu, OCERZ_PF, 1);
            ocerz_flag_assign(cpu, OCERZ_CF, 1);
        } else {
            ocerz_flag_assign(cpu, OCERZ_PF, 0);
            ocerz_flag_assign(cpu, OCERZ_ZF, eq as i32);
            ocerz_flag_assign(cpu, OCERZ_CF, lt as i32);
        }
    }
}

fn cvt_f2i32(f: f32, trunc_mode: bool) -> i32 {
    if f.is_nan() || f >= 2147483648.0f32 || f < -2147483648.0f32 {
        return 0x80000000u32 as i32;
    }
    if trunc_mode { f.trunc() as i32 } else { unsafe { lrintf(f) as i32 } }
}

fn cvt_f2i64(f: f32, trunc_mode: bool) -> i64 {
    if f.is_nan() || f >= 9223372036854775808.0f32 || f < -9223372036854775808.0f32 {
        return 0x8000000000000000u64 as i64;
    }
    if trunc_mode { f.trunc() as i64 } else { unsafe { llrintf(f) } }
}

fn cvt_d2i32(d: f64, trunc_mode: bool) -> i32 {
    if d.is_nan() || d >= 2147483648.0 || d < -2147483648.0 {
        return 0x80000000u32 as i32;
    }
    if trunc_mode { d.trunc() as i32 } else { unsafe { lrint(d) as i32 } }
}

fn cvt_d2i64(d: f64, trunc_mode: bool) -> i64 {
    if d.is_nan() || d >= 9223372036854775808.0 || d < -9223372036854775808.0 {
        return 0x8000000000000000u64 as i64;
    }
    if trunc_mode { d.trunc() as i64 } else { unsafe { llrint(d) } }
}

unsafe extern "C" {
    fn lrintf(x: f32) -> libc::c_long;
    fn llrintf(x: f32) -> i64;
    fn lrint(x: f64) -> libc::c_long;
    fn llrint(x: f64) -> i64;
    fn rintf(x: f32) -> f32;
    fn rint(x: f64) -> f64;
    fn floorf(x: f32) -> f32;
    fn ceilf(x: f32) -> f32;
    fn truncf(x: f32) -> f32;
    fn floor(x: f64) -> f64;
    fn ceil(x: f64) -> f64;
    fn trunc(x: f64) -> f64;
    fn sqrtf(x: f32) -> f32;
    fn sqrt(x: f64) -> f64;
}

fn round_imm_f(f: f32, imm: i32) -> f32 {
    unsafe {
        if imm & 4 != 0 {
            return rintf(f);
        }
        match imm & 3 {
            0 => rintf(f),
            1 => floorf(f),
            2 => ceilf(f),
            _ => truncf(f),
        }
    }
}

fn round_imm_d(d: f64, imm: i32) -> f64 {
    unsafe {
        if imm & 4 != 0 {
            return rint(d);
        }
        match imm & 3 {
            0 => rint(d),
            1 => floor(d),
            2 => ceil(d),
            _ => trunc(d),
        }
    }
}

unsafe fn do_moves(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let s = &insn.ops[1];
        let dk = d.kind as u32;
        let sk = s.kind as u32;
        if (insn.vex as u32 & OCERZ_VEX_NDS) != 0 && dk == OCERZ_OPK_XMM {
            let src = if sk == OCERZ_OPK_XMM { xr(cpu, s.reg) } else { ZQ };
            let mut r = xr(cpu, insn.vvvv);
            match insn.op as u32 {
                OCERZ_OP_MOVSS => r.lo = (r.lo & !0xffffffffu64) | (src.lo as u32 as u64),
                OCERZ_OP_MOVSDX => r.lo = src.lo,
                OCERZ_OP_MOVLPS => r.lo = ocerz_ld(ocerz_ea(cpu, insn, s), 8),
                OCERZ_OP_MOVHPS => r.hi = ocerz_ld(ocerz_ea(cpu, insn, s), 8),
                OCERZ_OP_MOVLHPS => r.hi = src.lo,
                OCERZ_OP_MOVHLPS => r.lo = src.hi,
                _ => return EUNSUP,
            }
            *xw(cpu, d.reg) = r;
            return STEP_OK;
        }
        match insn.op as u32 {
            OCERZ_OP_MOVUPS | OCERZ_OP_MOVAPS | OCERZ_OP_MOVDQA | OCERZ_OP_MOVDQU => {
                let v = ocerz_read_op128(cpu, insn, s);
                ocerz_write_op128(cpu, insn, d, v);
            }
            OCERZ_OP_MOVSS => {
                if dk == OCERZ_OPK_XMM && sk == OCERZ_OPK_XMM {
                    let sv = xr(cpu, s.reg).lo as u32 as u64;
                    let x = xw(cpu, d.reg);
                    x.lo = (x.lo & !0xffffffffu64) | sv;
                } else if dk == OCERZ_OPK_XMM {
                    let lo = ocerz_ld(ocerz_ea(cpu, insn, s), 4) & 0xffffffff;
                    *xw(cpu, d.reg) = Ocerz128 { lo, hi: 0 };
                } else {
                    ocerz_st(ocerz_ea(cpu, insn, d), 4, xr(cpu, s.reg).lo as u32 as u64);
                }
            }
            OCERZ_OP_MOVSDX => {
                if dk == OCERZ_OPK_XMM && sk == OCERZ_OPK_XMM {
                    xw(cpu, d.reg).lo = xr(cpu, s.reg).lo;
                } else if dk == OCERZ_OPK_XMM {
                    let lo = ocerz_ld(ocerz_ea(cpu, insn, s), 8);
                    *xw(cpu, d.reg) = Ocerz128 { lo, hi: 0 };
                } else {
                    ocerz_st(ocerz_ea(cpu, insn, d), 8, xr(cpu, s.reg).lo);
                }
            }
            OCERZ_OP_MOVD => {
                if dk == OCERZ_OPK_XMM {
                    let v = ocerz_read_op(cpu, insn, s) as u32 as u64;
                    *xw(cpu, d.reg) = Ocerz128 { lo: v, hi: 0 };
                } else {
                    ocerz_write_op(cpu, insn, d, xr(cpu, s.reg).lo as u32 as u64);
                }
            }
            OCERZ_OP_MOVQX => {
                if dk == OCERZ_OPK_XMM && sk == OCERZ_OPK_XMM {
                    let lo = xr(cpu, s.reg).lo;
                    *xw(cpu, d.reg) = Ocerz128 { lo, hi: 0 };
                } else if dk == OCERZ_OPK_XMM && sk == OCERZ_OPK_MEM {
                    let lo = ocerz_ld(ocerz_ea(cpu, insn, s), 8);
                    *xw(cpu, d.reg) = Ocerz128 { lo, hi: 0 };
                } else if dk == OCERZ_OPK_XMM {
                    let lo = ocerz_read_op(cpu, insn, s);
                    *xw(cpu, d.reg) = Ocerz128 { lo, hi: 0 };
                } else if dk == OCERZ_OPK_MEM {
                    ocerz_st(ocerz_ea(cpu, insn, d), 8, xr(cpu, s.reg).lo);
                } else {
                    ocerz_write_op(cpu, insn, d, xr(cpu, s.reg).lo);
                }
            }
            OCERZ_OP_MOVLPS => {
                if dk == OCERZ_OPK_XMM {
                    xw(cpu, d.reg).lo = ocerz_ld(ocerz_ea(cpu, insn, s), 8);
                } else {
                    ocerz_st(ocerz_ea(cpu, insn, d), 8, xr(cpu, s.reg).lo);
                }
            }
            OCERZ_OP_MOVHPS => {
                if dk == OCERZ_OPK_XMM {
                    xw(cpu, d.reg).hi = ocerz_ld(ocerz_ea(cpu, insn, s), 8);
                } else {
                    ocerz_st(ocerz_ea(cpu, insn, d), 8, xr(cpu, s.reg).hi);
                }
            }
            OCERZ_OP_MOVLHPS => xw(cpu, d.reg).hi = xr(cpu, s.reg).lo,
            OCERZ_OP_MOVHLPS => xw(cpu, d.reg).lo = xr(cpu, s.reg).hi,
            OCERZ_OP_MOVMSKPS => {
                let v = vec_of(xr(cpu, s.reg));
                let mut m = 0u32;
                for i in 0..4 {
                    m |= ((v.u32[i] >> 31) & 1) << i;
                }
                ocerz_write_gpr(cpu, d.reg as u32, 4, 0, m as u64);
            }
            OCERZ_OP_MOVMSKPD => {
                let v = vec_of(xr(cpu, s.reg));
                let mut m = 0u32;
                for i in 0..2 {
                    m |= (((v.u64[i] >> 63) & 1) as u32) << i;
                }
                ocerz_write_gpr(cpu, d.reg as u32, 4, 0, m as u64);
            }
            OCERZ_OP_PMOVMSKB => {
                let v = vec_of(xr(cpu, s.reg));
                let mut m = 0u32;
                for i in 0..16 {
                    m |= (((v.u8[i] >> 7) & 1) as u32) << i;
                }
                ocerz_write_gpr(cpu, d.reg as u32, 4, 0, m as u64);
            }
            OCERZ_OP_MOVSHDUP => {
                let sv = vec_read(cpu, insn, s);
                let r = Vec { u32: [sv.u32[1], sv.u32[1], sv.u32[3], sv.u32[3]] };
                vec_write(cpu, insn, d, r);
            }
            OCERZ_OP_MOVSLDUP => {
                let sv = vec_read(cpu, insn, s);
                let r = Vec { u32: [sv.u32[0], sv.u32[0], sv.u32[2], sv.u32[2]] };
                vec_write(cpu, insn, d, r);
            }
            OCERZ_OP_MOVDDUP => {
                let lo = if sk == OCERZ_OPK_XMM { xr(cpu, s.reg).lo } else { ocerz_ld(ocerz_ea(cpu, insn, s), 8) };
                vec_write(cpu, insn, d, Vec { u64: [lo, lo] });
            }
            _ => return EUNSUP,
        }
        STEP_OK
    }
}

fn fixnan_d(a: f64, b: f64, res: f64) -> f64 {
    if !res.is_nan() {
        return res;
    }
    let u = if a.is_nan() {
        a.to_bits() | 0x0008000000000000
    } else if b.is_nan() {
        b.to_bits() | 0x0008000000000000
    } else {
        0xfff8000000000000
    };
    f64::from_bits(u)
}

fn fixnan_f(a: f32, b: f32, res: f32) -> f32 {
    if !res.is_nan() {
        return res;
    }
    let u = if a.is_nan() {
        a.to_bits() | 0x00400000
    } else if b.is_nan() {
        b.to_bits() | 0x00400000
    } else {
        0xffc00000
    };
    f32::from_bits(u)
}

unsafe fn do_fp_arith(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let s = &insn.ops[1];
        let a = src1_of(cpu, insn, d);
        let b = vec_read(cpu, insn, s);
        let mut r = a;
        let (af, bf, ad, bd) = (a.f, b.f, a.d, b.d);
        match insn.op as u32 {
            OCERZ_OP_ADDPS => for i in 0..4 { r.f[i] = fixnan_f(af[i], bf[i], af[i] + bf[i]) },
            OCERZ_OP_HADDPS => {
                r.f[0] = fixnan_f(af[0], af[1], af[0] + af[1]);
                r.f[1] = fixnan_f(af[2], af[3], af[2] + af[3]);
                r.f[2] = fixnan_f(bf[0], bf[1], bf[0] + bf[1]);
                r.f[3] = fixnan_f(bf[2], bf[3], bf[2] + bf[3]);
            }
            OCERZ_OP_HSUBPS => {
                r.f[0] = fixnan_f(af[0], af[1], af[0] - af[1]);
                r.f[1] = fixnan_f(af[2], af[3], af[2] - af[3]);
                r.f[2] = fixnan_f(bf[0], bf[1], bf[0] - bf[1]);
                r.f[3] = fixnan_f(bf[2], bf[3], bf[2] - bf[3]);
            }
            OCERZ_OP_HADDPD => {
                r.d[0] = fixnan_d(ad[0], ad[1], ad[0] + ad[1]);
                r.d[1] = fixnan_d(bd[0], bd[1], bd[0] + bd[1]);
            }
            OCERZ_OP_HSUBPD => {
                r.d[0] = fixnan_d(ad[0], ad[1], ad[0] - ad[1]);
                r.d[1] = fixnan_d(bd[0], bd[1], bd[0] - bd[1]);
            }
            OCERZ_OP_ADDSUBPS => {
                r.f[0] = fixnan_f(af[0], bf[0], af[0] - bf[0]);
                r.f[1] = fixnan_f(af[1], bf[1], af[1] + bf[1]);
                r.f[2] = fixnan_f(af[2], bf[2], af[2] - bf[2]);
                r.f[3] = fixnan_f(af[3], bf[3], af[3] + bf[3]);
            }
            OCERZ_OP_ADDSUBPD => {
                r.d[0] = fixnan_d(ad[0], bd[0], ad[0] - bd[0]);
                r.d[1] = fixnan_d(ad[1], bd[1], ad[1] + bd[1]);
            }
            OCERZ_OP_SUBPS => for i in 0..4 { r.f[i] = fixnan_f(af[i], bf[i], af[i] - bf[i]) },
            OCERZ_OP_MULPS => for i in 0..4 { r.f[i] = fixnan_f(af[i], bf[i], af[i] * bf[i]) },
            OCERZ_OP_DIVPS => for i in 0..4 { r.f[i] = fixnan_f(af[i], bf[i], af[i] / bf[i]) },
            OCERZ_OP_MINPS => for i in 0..4 { r.f[i] = minf_x86(af[i], bf[i]) },
            OCERZ_OP_MAXPS => for i in 0..4 { r.f[i] = maxf_x86(af[i], bf[i]) },
            OCERZ_OP_SQRTPS => for i in 0..4 { r.f[i] = fixnan_f(bf[i], bf[i], sqrtf(bf[i])) },
            OCERZ_OP_RSQRTPS => for i in 0..4 { r.f[i] = 1.0f32 / sqrtf(bf[i]) },
            OCERZ_OP_RCPPS => for i in 0..4 { r.f[i] = 1.0f32 / bf[i] },
            OCERZ_OP_ADDPD => for i in 0..2 { r.d[i] = fixnan_d(ad[i], bd[i], ad[i] + bd[i]) },
            OCERZ_OP_SUBPD => for i in 0..2 { r.d[i] = fixnan_d(ad[i], bd[i], ad[i] - bd[i]) },
            OCERZ_OP_MULPD => for i in 0..2 { r.d[i] = fixnan_d(ad[i], bd[i], ad[i] * bd[i]) },
            OCERZ_OP_DIVPD => for i in 0..2 { r.d[i] = fixnan_d(ad[i], bd[i], ad[i] / bd[i]) },
            OCERZ_OP_MINPD => for i in 0..2 { r.d[i] = mind_x86(ad[i], bd[i]) },
            OCERZ_OP_MAXPD => for i in 0..2 { r.d[i] = maxd_x86(ad[i], bd[i]) },
            OCERZ_OP_SQRTPD => for i in 0..2 { r.d[i] = fixnan_d(bd[i], bd[i], sqrt(bd[i])) },
            OCERZ_OP_ADDSS => r.f[0] = fixnan_f(af[0], bf[0], af[0] + bf[0]),
            OCERZ_OP_SUBSS => r.f[0] = fixnan_f(af[0], bf[0], af[0] - bf[0]),
            OCERZ_OP_MULSS => r.f[0] = fixnan_f(af[0], bf[0], af[0] * bf[0]),
            OCERZ_OP_DIVSS => r.f[0] = fixnan_f(af[0], bf[0], af[0] / bf[0]),
            OCERZ_OP_MINSS => r.f[0] = minf_x86(af[0], bf[0]),
            OCERZ_OP_MAXSS => r.f[0] = maxf_x86(af[0], bf[0]),
            OCERZ_OP_SQRTSS => r.f[0] = fixnan_f(bf[0], bf[0], sqrtf(bf[0])),
            OCERZ_OP_RSQRTSS => r.f[0] = 1.0f32 / sqrtf(bf[0]),
            OCERZ_OP_RCPSS => r.f[0] = 1.0f32 / bf[0],
            OCERZ_OP_ADDSD => r.d[0] = fixnan_d(ad[0], bd[0], ad[0] + bd[0]),
            OCERZ_OP_SUBSD => r.d[0] = fixnan_d(ad[0], bd[0], ad[0] - bd[0]),
            OCERZ_OP_MULSD => r.d[0] = fixnan_d(ad[0], bd[0], ad[0] * bd[0]),
            OCERZ_OP_DIVSD => r.d[0] = fixnan_d(ad[0], bd[0], ad[0] / bd[0]),
            OCERZ_OP_MINSD => r.d[0] = mind_x86(ad[0], bd[0]),
            OCERZ_OP_MAXSD => r.d[0] = maxd_x86(ad[0], bd[0]),
            OCERZ_OP_SQRTSD => r.d[0] = fixnan_d(bd[0], bd[0], sqrt(bd[0])),
            _ => return EUNSUP,
        }
        *xw(cpu, d.reg) = r.q;
        STEP_OK
    }
}

unsafe fn do_logic(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let s = &insn.ops[1];
        let a = src1_of(cpu, insn, d).q;
        let b = ocerz_read_op128(cpu, insn, s);
        let r = match insn.op as u32 {
            OCERZ_OP_ANDPS | OCERZ_OP_PAND => Ocerz128 { lo: a.lo & b.lo, hi: a.hi & b.hi },
            OCERZ_OP_ANDNPS | OCERZ_OP_PANDN => Ocerz128 { lo: !a.lo & b.lo, hi: !a.hi & b.hi },
            OCERZ_OP_ORPS | OCERZ_OP_POR => Ocerz128 { lo: a.lo | b.lo, hi: a.hi | b.hi },
            OCERZ_OP_XORPS | OCERZ_OP_PXOR => Ocerz128 { lo: a.lo ^ b.lo, hi: a.hi ^ b.hi },
            _ => return EUNSUP,
        };
        *xw(cpu, d.reg) = r;
        STEP_OK
    }
}

static AES_SBOX: [u8; 256] = [
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
];

static AES_ISBOX: [u8; 256] = [
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
];

static AES_SHIFT_FWD: [u8; 16] = [
    0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11,
];

static AES_SHIFT_INV: [u8; 16] = [
    0, 13, 10, 7, 4, 1, 14, 11, 8, 5, 2, 15, 12, 9, 6, 3,
];

unsafe fn do_cmpfp(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let s = &insn.ops[1];
        let pred = (insn.ops[2].imm & if insn.vex != 0 { 0x1f } else { 7 }) as i32;
        let a = src1_of(cpu, insn, d);
        let b = vec_read(cpu, insn, s);
        let mut r = a;
        match insn.op as u32 {
            OCERZ_OP_CMPPS => {
                for i in 0..4 {
                    r.u32[i] = if cmp_pred_f(a.f[i], b.f[i], pred) { 0xffffffff } else { 0 };
                }
            }
            OCERZ_OP_CMPSS => r.u32[0] = if cmp_pred_f(a.f[0], b.f[0], pred) { 0xffffffff } else { 0 },
            OCERZ_OP_CMPPD => {
                for i in 0..2 {
                    r.u64[i] = if cmp_pred_d(a.d[i], b.d[i], pred) { !0 } else { 0 };
                }
            }
            OCERZ_OP_CMPSDX => r.u64[0] = if cmp_pred_d(a.d[0], b.d[0], pred) { !0 } else { 0 },
            _ => return EUNSUP,
        }
        *xw(cpu, d.reg) = r.q;
        STEP_OK
    }
}

unsafe fn do_comis(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let s = &insn.ops[1];
        let op = insn.op as u32;
        let a = src1_of(cpu, insn, d);
        let b = vec_read(cpu, insn, s);
        if op == OCERZ_OP_COMISS || op == OCERZ_OP_UCOMISS {
            let (x, y) = (a.f[0], b.f[0]);
            let unord = x.is_nan() || y.is_nan();
            set_comis_flags(cpu, unord, !unord && x < y, !unord && x == y);
        } else {
            let (x, y) = (a.d[0], b.d[0]);
            let unord = x.is_nan() || y.is_nan();
            set_comis_flags(cpu, unord, !unord && x < y, !unord && x == y);
        }
        STEP_OK
    }
}

unsafe fn do_pcmp(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let s = &insn.ops[1];
        let a = src1_of(cpu, insn, d);
        let b = vec_read(cpu, insn, s);
        let mut r = ZV;
        match insn.op as u32 {
            OCERZ_OP_PCMPEQB => for i in 0..16 { r.u8[i] = if a.u8[i] == b.u8[i] { 0xff } else { 0 } },
            OCERZ_OP_PCMPEQW => for i in 0..8 { r.u16[i] = if a.u16[i] == b.u16[i] { 0xffff } else { 0 } },
            OCERZ_OP_PCMPEQD => for i in 0..4 { r.u32[i] = if a.u32[i] == b.u32[i] { 0xffffffff } else { 0 } },
            OCERZ_OP_PCMPEQQ => for i in 0..2 { r.u64[i] = if a.u64[i] == b.u64[i] { !0 } else { 0 } },
            OCERZ_OP_PCMPGTB => for i in 0..16 { r.u8[i] = if a.i8[i] > b.i8[i] { 0xff } else { 0 } },
            OCERZ_OP_PCMPGTW => for i in 0..8 { r.u16[i] = if a.i16[i] > b.i16[i] { 0xffff } else { 0 } },
            OCERZ_OP_PCMPGTD => for i in 0..4 { r.u32[i] = if a.i32[i] > b.i32[i] { 0xffffffff } else { 0 } },
            OCERZ_OP_PCMPGTQ => for i in 0..2 { r.u64[i] = if a.i64[i] > b.i64[i] { !0 } else { 0 } },
            _ => return EUNSUP,
        }
        *xw(cpu, d.reg) = r.q;
        STEP_OK
    }
}

unsafe fn do_convert(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let s = &insn.ops[1];
        let op = insn.op as u32;
        if (insn.vex as u32 & OCERZ_VEX_NDS) != 0 && d.kind as u32 == OCERZ_OPK_XMM {
            let mut base = xr(cpu, insn.vvvv);
            if op == OCERZ_OP_CVTSS2SD || op == OCERZ_OP_CVTSD2SS {
                let b = vec_read(cpu, insn, s);
                if op == OCERZ_OP_CVTSS2SD {
                    base.lo = d2bits(b.f[0] as f64);
                } else {
                    base.lo = (base.lo & !0xffffffffu64) | f2bits(b.d[0] as f32) as u64;
                }
                *xw(cpu, d.reg) = base;
                return STEP_OK;
            }
            *xw(cpu, d.reg) = base;
        }
        match op {
            OCERZ_OP_CVTSI2SS => {
                let g = ocerz_read_op(cpu, insn, s);
                let f = if s.size == 8 { g as i64 as f32 } else { g as i32 as f32 };
                let x = xw(cpu, d.reg);
                x.lo = (x.lo & !0xffffffffu64) | f2bits(f) as u64;
            }
            OCERZ_OP_CVTSI2SD => {
                let g = ocerz_read_op(cpu, insn, s);
                let dv = if s.size == 8 { g as i64 as f64 } else { g as i32 as f64 };
                xw(cpu, d.reg).lo = d2bits(dv);
            }
            OCERZ_OP_CVTSS2SI | OCERZ_OP_CVTTSS2SI => {
                let b = vec_read(cpu, insn, s);
                let t = op == OCERZ_OP_CVTTSS2SI;
                if d.size == 8 {
                    ocerz_write_op(cpu, insn, d, cvt_f2i64(b.f[0], t) as u64);
                } else {
                    ocerz_write_op(cpu, insn, d, cvt_f2i32(b.f[0], t) as u32 as u64);
                }
            }
            OCERZ_OP_CVTSD2SI | OCERZ_OP_CVTTSD2SI => {
                let b = vec_read(cpu, insn, s);
                let t = op == OCERZ_OP_CVTTSD2SI;
                if d.size == 8 {
                    ocerz_write_op(cpu, insn, d, cvt_d2i64(b.d[0], t) as u64);
                } else {
                    ocerz_write_op(cpu, insn, d, cvt_d2i32(b.d[0], t) as u32 as u64);
                }
            }
            OCERZ_OP_CVTSS2SD => {
                let b = vec_read(cpu, insn, s);
                xw(cpu, d.reg).lo = d2bits(b.f[0] as f64);
            }
            OCERZ_OP_CVTSD2SS => {
                let b = vec_read(cpu, insn, s);
                let x = xw(cpu, d.reg);
                x.lo = (x.lo & !0xffffffffu64) | f2bits(b.d[0] as f32) as u64;
            }
            OCERZ_OP_CVTPS2PD => {
                let b = vec_read(cpu, insn, s);
                *xw(cpu, d.reg) = Vec { d: [b.f[0] as f64, b.f[1] as f64] }.q;
            }
            OCERZ_OP_CVTPD2PS => {
                let b = vec_read(cpu, insn, s);
                *xw(cpu, d.reg) = Vec { f: [b.d[0] as f32, b.d[1] as f32, 0.0, 0.0] }.q;
            }
            OCERZ_OP_CVTDQ2PS => {
                let b = vec_read(cpu, insn, s);
                let mut r = ZV;
                for i in 0..4 {
                    r.f[i] = b.i32[i] as f32;
                }
                *xw(cpu, d.reg) = r.q;
            }
            OCERZ_OP_CVTPS2DQ | OCERZ_OP_CVTTPS2DQ => {
                let b = vec_read(cpu, insn, s);
                let t = op == OCERZ_OP_CVTTPS2DQ;
                let mut r = ZV;
                for i in 0..4 {
                    r.i32[i] = cvt_f2i32(b.f[i], t);
                }
                *xw(cpu, d.reg) = r.q;
            }
            OCERZ_OP_CVTDQ2PD => {
                let b = vec_read(cpu, insn, s);
                *xw(cpu, d.reg) = Vec { d: [b.i32[0] as f64, b.i32[1] as f64] }.q;
            }
            OCERZ_OP_CVTPD2DQ | OCERZ_OP_CVTTPD2DQ => {
                let b = vec_read(cpu, insn, s);
                let t = op == OCERZ_OP_CVTTPD2DQ;
                *xw(cpu, d.reg) = Vec { i32: [cvt_d2i32(b.d[0], t), cvt_d2i32(b.d[1], t), 0, 0] }.q;
            }
            _ => return EUNSUP,
        }
        STEP_OK
    }
}

unsafe fn do_int_arith(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let s = &insn.ops[1];
        let a = src1_of(cpu, insn, d);
        let b = vec_read(cpu, insn, s);
        let mut r = a;
        match insn.op as u32 {
            OCERZ_OP_PADDB => for i in 0..16 { r.u8[i] = a.u8[i].wrapping_add(b.u8[i]) },
            OCERZ_OP_PADDW => for i in 0..8 { r.u16[i] = a.u16[i].wrapping_add(b.u16[i]) },
            OCERZ_OP_PADDD => for i in 0..4 { r.u32[i] = a.u32[i].wrapping_add(b.u32[i]) },
            OCERZ_OP_PADDQ => for i in 0..2 { r.u64[i] = a.u64[i].wrapping_add(b.u64[i]) },
            OCERZ_OP_PSUBB => for i in 0..16 { r.u8[i] = a.u8[i].wrapping_sub(b.u8[i]) },
            OCERZ_OP_PSUBW => for i in 0..8 { r.u16[i] = a.u16[i].wrapping_sub(b.u16[i]) },
            OCERZ_OP_PSUBD => for i in 0..4 { r.u32[i] = a.u32[i].wrapping_sub(b.u32[i]) },
            OCERZ_OP_PSUBQ => for i in 0..2 { r.u64[i] = a.u64[i].wrapping_sub(b.u64[i]) },
            OCERZ_OP_PADDSB => for i in 0..16 { r.i8[i] = sat_s8(a.i8[i] as i32 + b.i8[i] as i32) },
            OCERZ_OP_PADDSW => for i in 0..8 { r.i16[i] = sat_s16(a.i16[i] as i32 + b.i16[i] as i32) },
            OCERZ_OP_PADDUSB => for i in 0..16 { r.u8[i] = sat_u8(a.u8[i] as i32 + b.u8[i] as i32) },
            OCERZ_OP_PADDUSW => for i in 0..8 { r.u16[i] = sat_u16(a.u16[i] as i32 + b.u16[i] as i32) },
            OCERZ_OP_PSUBSB => for i in 0..16 { r.i8[i] = sat_s8(a.i8[i] as i32 - b.i8[i] as i32) },
            OCERZ_OP_PSUBSW => for i in 0..8 { r.i16[i] = sat_s16(a.i16[i] as i32 - b.i16[i] as i32) },
            OCERZ_OP_PSUBUSB => for i in 0..16 { r.u8[i] = sat_u8(a.u8[i] as i32 - b.u8[i] as i32) },
            OCERZ_OP_PSUBUSW => for i in 0..8 { r.u16[i] = sat_u16(a.u16[i] as i32 - b.u16[i] as i32) },
            OCERZ_OP_PMULLW => for i in 0..8 { r.u16[i] = (a.i16[i] as i32 * b.i16[i] as i32) as u16 },
            OCERZ_OP_PMULLD => for i in 0..4 { r.u32[i] = (a.i32[i] as i64 * b.i32[i] as i64) as u32 },
            OCERZ_OP_PMULHW => for i in 0..8 { r.i16[i] = ((a.i16[i] as i32 * b.i16[i] as i32) >> 16) as i16 },
            OCERZ_OP_PMULHUW => for i in 0..8 { r.u16[i] = ((a.u16[i] as u32 * b.u16[i] as u32) >> 16) as u16 },
            OCERZ_OP_PMULUDQ => {
                r.u64[0] = a.u32[0] as u64 * b.u32[0] as u64;
                r.u64[1] = a.u32[2] as u64 * b.u32[2] as u64;
            }
            OCERZ_OP_PMULDQ => {
                r.i64[0] = a.i32[0] as i64 * b.i32[0] as i64;
                r.i64[1] = a.i32[2] as i64 * b.i32[2] as i64;
            }
            OCERZ_OP_PMADDWD => {
                for i in 0..4 {
                    r.i32[i] = (a.i16[2 * i] as i32 * b.i16[2 * i] as i32)
                        .wrapping_add(a.i16[2 * i + 1] as i32 * b.i16[2 * i + 1] as i32);
                }
            }
            OCERZ_OP_PAVGB => for i in 0..16 { r.u8[i] = ((a.u8[i] as u32 + b.u8[i] as u32 + 1) >> 1) as u8 },
            OCERZ_OP_PAVGW => for i in 0..8 { r.u16[i] = ((a.u16[i] as u32 + b.u16[i] as u32 + 1) >> 1) as u16 },
            OCERZ_OP_PMAXUB => for i in 0..16 { r.u8[i] = if a.u8[i] > b.u8[i] { a.u8[i] } else { b.u8[i] } },
            OCERZ_OP_PMINUB => for i in 0..16 { r.u8[i] = if a.u8[i] < b.u8[i] { a.u8[i] } else { b.u8[i] } },
            OCERZ_OP_PMAXSW => for i in 0..8 { r.i16[i] = if a.i16[i] > b.i16[i] { a.i16[i] } else { b.i16[i] } },
            OCERZ_OP_PMINSW => for i in 0..8 { r.i16[i] = if a.i16[i] < b.i16[i] { a.i16[i] } else { b.i16[i] } },
            OCERZ_OP_PMAXSB => for i in 0..16 { r.i8[i] = if a.i8[i] > b.i8[i] { a.i8[i] } else { b.i8[i] } },
            OCERZ_OP_PMINSB => for i in 0..16 { r.i8[i] = if a.i8[i] < b.i8[i] { a.i8[i] } else { b.i8[i] } },
            OCERZ_OP_PMAXUW => for i in 0..8 { r.u16[i] = if a.u16[i] > b.u16[i] { a.u16[i] } else { b.u16[i] } },
            OCERZ_OP_PMINUW => for i in 0..8 { r.u16[i] = if a.u16[i] < b.u16[i] { a.u16[i] } else { b.u16[i] } },
            OCERZ_OP_PMAXSD => for i in 0..4 { r.i32[i] = if a.i32[i] > b.i32[i] { a.i32[i] } else { b.i32[i] } },
            OCERZ_OP_PMINSD => for i in 0..4 { r.i32[i] = if a.i32[i] < b.i32[i] { a.i32[i] } else { b.i32[i] } },
            OCERZ_OP_PMAXUD => for i in 0..4 { r.u32[i] = if a.u32[i] > b.u32[i] { a.u32[i] } else { b.u32[i] } },
            OCERZ_OP_PMINUD => for i in 0..4 { r.u32[i] = if a.u32[i] < b.u32[i] { a.u32[i] } else { b.u32[i] } },
            OCERZ_OP_PSADBW => {
                let mut lo = 0u32;
                let mut hi = 0u32;
                for i in 0..8 {
                    lo += (a.u8[i] as i32 - b.u8[i] as i32).unsigned_abs();
                    hi += (a.u8[i + 8] as i32 - b.u8[i + 8] as i32).unsigned_abs();
                }
                r = ZV;
                r.u16[0] = lo as u16;
                r.u16[4] = hi as u16;
            }
            OCERZ_OP_PABSB => for i in 0..16 { r.u8[i] = (b.i8[i] as i32).abs() as u8 },
            OCERZ_OP_PABSW => for i in 0..8 { r.u16[i] = (b.i16[i] as i32).abs() as u16 },
            OCERZ_OP_PABSD => for i in 0..4 { r.u32[i] = (b.i32[i] as i64).abs() as u32 },
            OCERZ_OP_PHADDW => {
                for i in 0..4 { r.u16[i] = (a.i16[2 * i] as i32 + a.i16[2 * i + 1] as i32) as u16 }
                for i in 0..4 { r.u16[i + 4] = (b.i16[2 * i] as i32 + b.i16[2 * i + 1] as i32) as u16 }
            }
            OCERZ_OP_PHADDD => {
                for i in 0..2 { r.u32[i] = a.i32[2 * i].wrapping_add(a.i32[2 * i + 1]) as u32 }
                for i in 0..2 { r.u32[i + 2] = b.i32[2 * i].wrapping_add(b.i32[2 * i + 1]) as u32 }
            }
            OCERZ_OP_PHADDSW => {
                for i in 0..4 { r.i16[i] = sat_s16(a.i16[2 * i] as i32 + a.i16[2 * i + 1] as i32) }
                for i in 0..4 { r.i16[i + 4] = sat_s16(b.i16[2 * i] as i32 + b.i16[2 * i + 1] as i32) }
            }
            OCERZ_OP_PHSUBW => {
                for i in 0..4 { r.u16[i] = (a.i16[2 * i] as i32 - a.i16[2 * i + 1] as i32) as u16 }
                for i in 0..4 { r.u16[i + 4] = (b.i16[2 * i] as i32 - b.i16[2 * i + 1] as i32) as u16 }
            }
            OCERZ_OP_PHSUBD => {
                for i in 0..2 { r.u32[i] = a.i32[2 * i].wrapping_sub(a.i32[2 * i + 1]) as u32 }
                for i in 0..2 { r.u32[i + 2] = b.i32[2 * i].wrapping_sub(b.i32[2 * i + 1]) as u32 }
            }
            OCERZ_OP_PHSUBSW => {
                for i in 0..4 { r.i16[i] = sat_s16(a.i16[2 * i] as i32 - a.i16[2 * i + 1] as i32) }
                for i in 0..4 { r.i16[i + 4] = sat_s16(b.i16[2 * i] as i32 - b.i16[2 * i + 1] as i32) }
            }
            OCERZ_OP_PSIGNB => {
                for i in 0..16 {
                    r.i8[i] = if b.i8[i] < 0 { a.i8[i].wrapping_neg() } else if b.i8[i] == 0 { 0 } else { a.i8[i] };
                }
            }
            OCERZ_OP_PSIGNW => {
                for i in 0..8 {
                    r.i16[i] = if b.i16[i] < 0 { a.i16[i].wrapping_neg() } else if b.i16[i] == 0 { 0 } else { a.i16[i] };
                }
            }
            OCERZ_OP_PSIGND => {
                for i in 0..4 {
                    r.i32[i] = if b.i32[i] < 0 { a.i32[i].wrapping_neg() } else if b.i32[i] == 0 { 0 } else { a.i32[i] };
                }
            }
            OCERZ_OP_PMADDUBSW => {
                for i in 0..8 {
                    r.i16[i] = sat_s16(
                        a.u8[2 * i] as i32 * b.i8[2 * i] as i32 + a.u8[2 * i + 1] as i32 * b.i8[2 * i + 1] as i32,
                    );
                }
            }
            OCERZ_OP_PMULHRSW => {
                for i in 0..8 {
                    r.i16[i] = ((((a.i16[i] as i32 * b.i16[i] as i32) >> 14) + 1) >> 1) as i16;
                }
            }
            _ => return EUNSUP,
        }
        *xw(cpu, d.reg) = r.q;
        STEP_OK
    }
}

unsafe fn do_pack_shuffle(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let s = &insn.ops[1];
        let a = src1_of(cpu, insn, d);
        let b = vec_read(cpu, insn, s);
        let mut r = ZV;
        match insn.op as u32 {
            OCERZ_OP_PACKSSWB => {
                for i in 0..8 { r.i8[i] = sat_s8(a.i16[i] as i32) }
                for i in 0..8 { r.i8[i + 8] = sat_s8(b.i16[i] as i32) }
            }
            OCERZ_OP_PACKSSDW => {
                for i in 0..4 { r.i16[i] = sat_s16(a.i32[i]) }
                for i in 0..4 { r.i16[i + 4] = sat_s16(b.i32[i]) }
            }
            OCERZ_OP_PACKUSWB => {
                for i in 0..8 { r.u8[i] = sat_u8(a.i16[i] as i32) }
                for i in 0..8 { r.u8[i + 8] = sat_u8(b.i16[i] as i32) }
            }
            OCERZ_OP_PACKUSDW => {
                for i in 0..4 { r.u16[i] = sat_u16(a.i32[i]) }
                for i in 0..4 { r.u16[i + 4] = sat_u16(b.i32[i]) }
            }
            OCERZ_OP_PUNPCKLBW => for i in 0..8 { r.u8[2 * i] = a.u8[i]; r.u8[2 * i + 1] = b.u8[i]; },
            OCERZ_OP_PUNPCKHBW => for i in 0..8 { r.u8[2 * i] = a.u8[i + 8]; r.u8[2 * i + 1] = b.u8[i + 8]; },
            OCERZ_OP_PUNPCKLWD => for i in 0..4 { r.u16[2 * i] = a.u16[i]; r.u16[2 * i + 1] = b.u16[i]; },
            OCERZ_OP_PUNPCKHWD => for i in 0..4 { r.u16[2 * i] = a.u16[i + 4]; r.u16[2 * i + 1] = b.u16[i + 4]; },
            OCERZ_OP_PUNPCKLDQ | OCERZ_OP_UNPCKLPS => r.u32 = [a.u32[0], b.u32[0], a.u32[1], b.u32[1]],
            OCERZ_OP_PUNPCKHDQ | OCERZ_OP_UNPCKHPS => r.u32 = [a.u32[2], b.u32[2], a.u32[3], b.u32[3]],
            OCERZ_OP_PUNPCKLQDQ | OCERZ_OP_UNPCKLPD => r.u64 = [a.u64[0], b.u64[0]],
            OCERZ_OP_PUNPCKHQDQ | OCERZ_OP_UNPCKHPD => r.u64 = [a.u64[1], b.u64[1]],
            OCERZ_OP_PSHUFD => {
                let imm = insn.ops[2].imm as i32;
                for i in 0..4 { r.u32[i] = b.u32[((imm >> (2 * i)) & 3) as usize] }
            }
            OCERZ_OP_PSHUFLW => {
                let imm = insn.ops[2].imm as i32;
                for i in 0..4 { r.u16[i] = b.u16[((imm >> (2 * i)) & 3) as usize] }
                r.u64[1] = b.u64[1];
            }
            OCERZ_OP_PSHUFHW => {
                let imm = insn.ops[2].imm as i32;
                r.u64[0] = b.u64[0];
                for i in 0..4 { r.u16[i + 4] = b.u16[4 + ((imm >> (2 * i)) & 3) as usize] }
            }
            OCERZ_OP_PSHUFB => {
                for i in 0..16 {
                    let ctl = b.u8[i];
                    r.u8[i] = if ctl & 0x80 != 0 { 0 } else { a.u8[(ctl & 0x0f) as usize] };
                }
            }
            OCERZ_OP_PALIGNR => {
                let imm = (insn.ops[2].imm & 0xff) as usize;
                let mut cat = [0u8; 32];
                cat[..16].copy_from_slice(&b.u8);
                cat[16..].copy_from_slice(&a.u8);
                for i in 0..16 {
                    let idx = imm + i;
                    r.u8[i] = if idx < 32 { cat[idx] } else { 0 };
                }
            }
            OCERZ_OP_SHUFPS => {
                let imm = insn.ops[2].imm as i32;
                r.u32 = [
                    a.u32[(imm & 3) as usize],
                    a.u32[((imm >> 2) & 3) as usize],
                    b.u32[((imm >> 4) & 3) as usize],
                    b.u32[((imm >> 6) & 3) as usize],
                ];
            }
            OCERZ_OP_SHUFPD => {
                let imm = insn.ops[2].imm as i32;
                r.u64 = [a.u64[(imm & 1) as usize], b.u64[((imm >> 1) & 1) as usize]];
            }
            _ => return EUNSUP,
        }
        *xw(cpu, d.reg) = r.q;
        STEP_OK
    }
}

unsafe fn shift_count(cpu: &mut OcerzCPU, insn: &X86Insn) -> u32 {
    unsafe {
        let s = &insn.ops[1];
        if s.kind as u32 == OCERZ_OPK_IMM {
            return (s.imm & 0xff) as u32;
        }
        let b = vec_read(cpu, insn, s);
        if b.u64[0] > 255 { 256 } else { b.u64[0] as u32 }
    }
}

unsafe fn do_shift(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let a = if (insn.vex as u32 & OCERZ_VEX_NDD) != 0 {
            vec_read(cpu, insn, &insn.ops[2])
        } else {
            src1_of(cpu, insn, d)
        };
        let mut r = a;
        let c = shift_count(cpu, insn);
        match insn.op as u32 {
            OCERZ_OP_PSLLW => for i in 0..8 { r.u16[i] = if c < 16 { a.u16[i] << c } else { 0 } },
            OCERZ_OP_PSLLD => for i in 0..4 { r.u32[i] = if c < 32 { a.u32[i] << c } else { 0 } },
            OCERZ_OP_PSLLQ => for i in 0..2 { r.u64[i] = if c < 64 { a.u64[i] << c } else { 0 } },
            OCERZ_OP_PSRLW => for i in 0..8 { r.u16[i] = if c < 16 { a.u16[i] >> c } else { 0 } },
            OCERZ_OP_PSRLD => for i in 0..4 { r.u32[i] = if c < 32 { a.u32[i] >> c } else { 0 } },
            OCERZ_OP_PSRLQ => for i in 0..2 { r.u64[i] = if c < 64 { a.u64[i] >> c } else { 0 } },
            OCERZ_OP_PSRAW => {
                let sh = if c < 16 { c } else { 15 };
                for i in 0..8 { r.i16[i] = a.i16[i] >> sh }
            }
            OCERZ_OP_PSRAD => {
                let sh = if c < 32 { c } else { 31 };
                for i in 0..4 { r.i32[i] = a.i32[i] >> sh }
            }
            OCERZ_OP_PSLLDQ => {
                for i in 0..16u32 {
                    r.u8[i as usize] = if i >= c && c <= 16 { a.u8[(i - c) as usize] } else { 0 };
                }
            }
            OCERZ_OP_PSRLDQ => {
                for i in 0..16u32 {
                    r.u8[i as usize] = if i + c < 16 && c <= 16 { a.u8[(i + c) as usize] } else { 0 };
                }
            }
            _ => return EUNSUP,
        }
        *xw(cpu, d.reg) = r.q;
        STEP_OK
    }
}

unsafe fn ext_store(cpu: &mut OcerzCPU, insn: &X86Insn, d: &X86Operand, size: i32, gsize: i32, e: u64) {
    unsafe {
        if d.kind as u32 == OCERZ_OPK_MEM {
            ocerz_st(ocerz_ea(cpu, insn, d), size, e);
        } else {
            ocerz_write_gpr(cpu, d.reg as u32, gsize, 0, e);
        }
    }
}

unsafe fn ins_src(cpu: &mut OcerzCPU, insn: &X86Insn, s: &X86Operand, size: i32) -> u64 {
    unsafe {
        if s.kind as u32 == OCERZ_OPK_MEM {
            ocerz_ld(ocerz_ea(cpu, insn, s), size)
        } else {
            ocerz_read_op(cpu, insn, s)
        }
    }
}

unsafe fn do_insert_extract(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let s = &insn.ops[1];
        let imm = insn.ops[2].imm;
        match insn.op as u32 {
            OCERZ_OP_PEXTRB => {
                let e = vec_of(xr(cpu, s.reg)).u8[(imm & 0x0f) as usize];
                ext_store(cpu, insn, d, 1, 4, e as u64);
            }
            OCERZ_OP_PEXTRW => {
                let e = vec_of(xr(cpu, s.reg)).u16[(imm & 0x07) as usize];
                ext_store(cpu, insn, d, 2, 4, e as u64);
            }
            OCERZ_OP_PEXTRD | OCERZ_OP_EXTRACTPS => {
                let e = vec_of(xr(cpu, s.reg)).u32[(imm & 0x03) as usize];
                ext_store(cpu, insn, d, 4, 4, e as u64);
            }
            OCERZ_OP_PEXTRQ => {
                let e = vec_of(xr(cpu, s.reg)).u64[(imm & 0x01) as usize];
                ext_store(cpu, insn, d, 8, 8, e);
            }
            OCERZ_OP_PINSRB => {
                let e = ins_src(cpu, insn, s, 1) as u8;
                let mut v = src1_of(cpu, insn, d);
                v.u8[(imm & 0x0f) as usize] = e;
                *xw(cpu, d.reg) = v.q;
            }
            OCERZ_OP_PINSRW => {
                let e = ins_src(cpu, insn, s, 2) as u16;
                let mut v = src1_of(cpu, insn, d);
                v.u16[(imm & 0x07) as usize] = e;
                *xw(cpu, d.reg) = v.q;
            }
            OCERZ_OP_PINSRD => {
                let e = ins_src(cpu, insn, s, 4) as u32;
                let mut v = src1_of(cpu, insn, d);
                v.u32[(imm & 0x03) as usize] = e;
                *xw(cpu, d.reg) = v.q;
            }
            OCERZ_OP_PINSRQ => {
                let e = ins_src(cpu, insn, s, 8);
                let mut v = src1_of(cpu, insn, d);
                v.u64[(imm & 0x01) as usize] = e;
                *xw(cpu, d.reg) = v.q;
            }
            OCERZ_OP_INSERTPS => {
                let imm = (imm & 0xff) as i32;
                let count_s = ((imm >> 6) & 3) as usize;
                let count_d = ((imm >> 4) & 3) as usize;
                let zmask = imm & 0x0f;
                let sel = if s.kind as u32 == OCERZ_OPK_MEM {
                    ocerz_ld(ocerz_ea(cpu, insn, s), 4) as u32
                } else {
                    vec_of(xr(cpu, s.reg)).u32[count_s]
                };
                let mut v = src1_of(cpu, insn, d);
                v.u32[count_d] = sel;
                for i in 0..4 {
                    if zmask & (1 << i) != 0 {
                        v.u32[i] = 0;
                    }
                }
                *xw(cpu, d.reg) = v.q;
            }
            _ => return EUNSUP,
        }
        STEP_OK
    }
}

fn str_elem(v: &Vec, i: i32, words: bool, sgn: bool, out: &mut i32) -> bool {
    unsafe {
        let i = i as usize;
        *out = if words {
            if sgn { v.i16[i] as i32 } else { v.u16[i] as i32 }
        } else if sgn {
            v.i8[i] as i32
        } else {
            v.u8[i] as i32
        };
    }
    *out == 0
}

fn str_implicit_len(v: &Vec, n: i32, words: bool, sgn: bool) -> i32 {
    let mut e = 0;
    for i in 0..n {
        if str_elem(v, i, words, sgn, &mut e) {
            return i;
        }
    }
    n
}

unsafe fn do_pcmpstr(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let imm = (insn.ops[2].imm & 0xff) as i32;
        let words = imm & 1 != 0;
        let sgn = (imm >> 1) & 1 != 0;
        let n: i32 = if words { 8 } else { 16 };
        let op = insn.op as u32;
        let is_index = op == OCERZ_OP_PCMPESTRI || op == OCERZ_OP_PCMPISTRI;
        let is_expl = op == OCERZ_OP_PCMPESTRI || op == OCERZ_OP_PCMPESTRM;

        let a = src1_of(cpu, insn, &insn.ops[0]);
        let b = vec_read(cpu, insn, &insn.ops[1]);

        let (la, lb);
        if is_expl {
            let va = (cpu.gpr[OCERZ_RAX as usize] as i32 as i64).abs();
            let vb = (cpu.gpr[OCERZ_RDX as usize] as i32 as i64).abs();
            la = if va > n as i64 { n } else { va as i32 };
            lb = if vb > n as i64 { n } else { vb as i32 };
        } else {
            la = str_implicit_len(&a, n, words, sgn);
            lb = str_implicit_len(&b, n, words, sgn);
        }

        let mut res1: u32 = 0;
        let agg = (imm >> 2) & 3;
        for j in 0..n {
            let mut bit = false;
            let mut ea = 0;
            let mut eb = 0;
            match agg {
                0 => {
                    if j < lb {
                        str_elem(&b, j, words, sgn, &mut eb);
                        for i in 0..la {
                            str_elem(&a, i, words, sgn, &mut ea);
                            if ea == eb {
                                bit = true;
                                break;
                            }
                        }
                    }
                }
                1 => {
                    if j < lb {
                        str_elem(&b, j, words, sgn, &mut eb);
                        let mut i = 0;
                        while i + 1 < la {
                            let mut lo = 0;
                            let mut hi = 0;
                            str_elem(&a, i, words, sgn, &mut lo);
                            str_elem(&a, i + 1, words, sgn, &mut hi);
                            if eb >= lo && eb <= hi {
                                bit = true;
                                break;
                            }
                            i += 2;
                        }
                    }
                }
                2 => {
                    if j < la && j < lb {
                        str_elem(&a, j, words, sgn, &mut ea);
                        str_elem(&b, j, words, sgn, &mut eb);
                        bit = ea == eb;
                    } else if j >= la && j >= lb {
                        bit = true;
                    }
                }
                _ => {
                    bit = true;
                    let mut i = 0;
                    while i < la && j + i < n {
                        if j + i >= lb {
                            bit = false;
                            break;
                        }
                        str_elem(&a, i, words, sgn, &mut ea);
                        str_elem(&b, j + i, words, sgn, &mut eb);
                        if ea != eb {
                            bit = false;
                            break;
                        }
                        i += 1;
                    }
                }
            }
            if bit {
                res1 |= 1u32 << j;
            }
        }

        let valid: u32 = if n == 16 { 0xffff } else { 0xff };
        let mut res2 = res1;
        match (imm >> 4) & 3 {
            1 => res2 = !res1 & valid,
            3 => res2 = res1 ^ if lb >= n { valid } else { (1u32 << lb) - 1 },
            _ => {}
        }
        res2 &= valid;

        if is_index {
            let mut idx = n as u32;
            if res2 != 0 {
                idx = if imm & 0x40 != 0 { 31 - res2.leading_zeros() } else { res2.trailing_zeros() };
            }
            cpu.gpr[OCERZ_RCX as usize] = idx as u64;
        } else {
            let mut m = ZV;
            if imm & 0x40 != 0 {
                for j in 0..n as usize {
                    if res2 & (1u32 << j) != 0 {
                        if words {
                            m.u16[j] = 0xffff;
                        } else {
                            m.u8[j] = 0xff;
                        }
                    }
                }
            } else {
                m.q.lo = res2 as u64;
            }
            cpu.xmm[0] = m.q;
        }

        ocerz_flag_assign(cpu, OCERZ_CF, (res2 != 0) as i32);
        ocerz_flag_assign(cpu, OCERZ_ZF, (lb < n) as i32);
        ocerz_flag_assign(cpu, OCERZ_SF, (la < n) as i32);
        ocerz_flag_assign(cpu, OCERZ_OF, (res2 & 1) as i32);
        ocerz_flag_assign(cpu, OCERZ_AF, 0);
        ocerz_flag_assign(cpu, OCERZ_PF, 0);
        STEP_OK
    }
}

unsafe fn do_sse41_misc(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let a = src1_of(cpu, insn, d);
        let b = vec_read(cpu, insn, &insn.ops[1]);
        let mut r = ZV;
        match insn.op as u32 {
            OCERZ_OP_PHMINPOSUW => {
                let mut best = 0usize;
                for i in 1..8 {
                    if b.u16[i] < b.u16[best] {
                        best = i;
                    }
                }
                r.u16[0] = b.u16[best];
                r.u16[1] = best as u16;
            }
            OCERZ_OP_MPSADBW => {
                let imm = (insn.ops[2].imm & 0xff) as usize;
                let soff = (imm & 3) * 4;
                let doff = ((imm >> 2) & 1) * 4;
                for i in 0..8 {
                    let mut sum = 0i32;
                    for k in 0..4 {
                        sum += (a.u8[doff + i + k] as i32 - b.u8[soff + k] as i32).abs();
                    }
                    r.u16[i] = sum as u16;
                }
            }
            OCERZ_OP_DPPS => {
                let imm = (insn.ops[2].imm & 0xff) as i32;
                let mut t = [0.0f32; 4];
                for (i, ti) in t.iter_mut().enumerate() {
                    *ti = if imm & (0x10 << i) != 0 { a.f[i] * b.f[i] } else { 0.0 };
                }
                let sum = (t[0] + t[1]) + (t[2] + t[3]);
                for i in 0..4 {
                    r.f[i] = if imm & (1 << i) != 0 { sum } else { 0.0 };
                }
            }
            OCERZ_OP_DPPD => {
                let imm = (insn.ops[2].imm & 0xff) as i32;
                let t0 = if imm & 0x10 != 0 { a.d[0] * b.d[0] } else { 0.0 };
                let t1 = if imm & 0x20 != 0 { a.d[1] * b.d[1] } else { 0.0 };
                let sum = t0 + t1;
                for i in 0..2 {
                    r.d[i] = if imm & (1 << i) != 0 { sum } else { 0.0 };
                }
            }
            _ => return EUNSUP,
        }
        *xw(cpu, d.reg) = r.q;
        STEP_OK
    }
}

unsafe fn do_ptest_blend(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let s = &insn.ops[1];
        let op = insn.op as u32;
        if op == OCERZ_OP_PTEST {
            let a = xr(cpu, d.reg);
            let b = ocerz_read_op128(cpu, insn, s);
            let zf = (a.lo & b.lo) == 0 && (a.hi & b.hi) == 0;
            let cf = (!a.lo & b.lo) == 0 && (!a.hi & b.hi) == 0;
            ocerz_flag_assign(cpu, OCERZ_ZF, zf as i32);
            ocerz_flag_assign(cpu, OCERZ_CF, cf as i32);
            ocerz_flag_assign(cpu, OCERZ_PF, 0);
            ocerz_flag_assign(cpu, OCERZ_AF, 0);
            ocerz_flag_assign(cpu, OCERZ_SF, 0);
            ocerz_flag_assign(cpu, OCERZ_OF, 0);
            return STEP_OK;
        }
        if !matches!(
            op,
            OCERZ_OP_PBLENDW | OCERZ_OP_BLENDPS | OCERZ_OP_BLENDPD | OCERZ_OP_PBLENDVB | OCERZ_OP_BLENDVPS
                | OCERZ_OP_BLENDVPD
        ) {
            return EUNSUP;
        }
        let a = src1_of(cpu, insn, d);
        let b = vec_read(cpu, insn, s);
        let mut r = a;
        let mreg = if (insn.vex as u32 & OCERZ_VEX_IS4) != 0 { insn.ops[2].reg } else { 0 };
        match op {
            OCERZ_OP_PBLENDW => {
                let imm = (insn.ops[2].imm & 0xff) as i32;
                for i in 0..8 {
                    if imm & (1 << i) != 0 {
                        r.u16[i] = b.u16[i];
                    }
                }
            }
            OCERZ_OP_BLENDPS => {
                let imm = (insn.ops[2].imm & 0x0f) as i32;
                for i in 0..4 {
                    if imm & (1 << i) != 0 {
                        r.u32[i] = b.u32[i];
                    }
                }
            }
            OCERZ_OP_BLENDPD => {
                let imm = (insn.ops[2].imm & 0x03) as i32;
                for i in 0..2 {
                    if imm & (1 << i) != 0 {
                        r.u64[i] = b.u64[i];
                    }
                }
            }
            OCERZ_OP_PBLENDVB => {
                let m = vec_of(xr(cpu, mreg));
                for i in 0..16 {
                    if m.u8[i] & 0x80 != 0 {
                        r.u8[i] = b.u8[i];
                    }
                }
            }
            OCERZ_OP_BLENDVPS => {
                let m = vec_of(xr(cpu, mreg));
                for i in 0..4 {
                    if m.u32[i] & 0x80000000 != 0 {
                        r.u32[i] = b.u32[i];
                    }
                }
            }
            _ => {
                let m = vec_of(xr(cpu, mreg));
                for i in 0..2 {
                    if m.u64[i] & 0x8000000000000000 != 0 {
                        r.u64[i] = b.u64[i];
                    }
                }
            }
        }
        *xw(cpu, d.reg) = r.q;
        STEP_OK
    }
}

unsafe fn do_pmovx(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let s = &insn.ops[1];
        let op = insn.op as u32;
        let srcw = match op {
            OCERZ_OP_PMOVZXBQ | OCERZ_OP_PMOVSXBQ => 2,
            OCERZ_OP_PMOVZXBD | OCERZ_OP_PMOVSXBD | OCERZ_OP_PMOVZXWQ | OCERZ_OP_PMOVSXWQ => 4,
            _ => 8,
        };
        let b = if s.kind as u32 == OCERZ_OPK_XMM {
            vec_of(xr(cpu, s.reg))
        } else {
            vec_of(Ocerz128 { lo: ocerz_ld(ocerz_ea(cpu, insn, s), srcw), hi: 0 })
        };
        let mut r = ZV;
        match op {
            OCERZ_OP_PMOVZXBW => for i in 0..8 { r.u16[i] = b.u8[i] as u16 },
            OCERZ_OP_PMOVZXBD => for i in 0..4 { r.u32[i] = b.u8[i] as u32 },
            OCERZ_OP_PMOVZXBQ => for i in 0..2 { r.u64[i] = b.u8[i] as u64 },
            OCERZ_OP_PMOVZXWD => for i in 0..4 { r.u32[i] = b.u16[i] as u32 },
            OCERZ_OP_PMOVZXWQ => for i in 0..2 { r.u64[i] = b.u16[i] as u64 },
            OCERZ_OP_PMOVZXDQ => for i in 0..2 { r.u64[i] = b.u32[i] as u64 },
            OCERZ_OP_PMOVSXBW => for i in 0..8 { r.i16[i] = b.i8[i] as i16 },
            OCERZ_OP_PMOVSXBD => for i in 0..4 { r.i32[i] = b.i8[i] as i32 },
            OCERZ_OP_PMOVSXBQ => for i in 0..2 { r.i64[i] = b.i8[i] as i64 },
            OCERZ_OP_PMOVSXWD => for i in 0..4 { r.i32[i] = b.i16[i] as i32 },
            OCERZ_OP_PMOVSXWQ => for i in 0..2 { r.i64[i] = b.i16[i] as i64 },
            OCERZ_OP_PMOVSXDQ => for i in 0..2 { r.i64[i] = b.i32[i] as i64 },
            _ => return EUNSUP,
        }
        *xw(cpu, d.reg) = r.q;
        STEP_OK
    }
}

unsafe fn do_round(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let s = &insn.ops[1];
        let imm = (insn.ops[2].imm & 0xff) as i32;
        let b = vec_read(cpu, insn, s);
        match insn.op as u32 {
            OCERZ_OP_ROUNDPS => {
                let mut r = ZV;
                for i in 0..4 {
                    r.f[i] = round_imm_f(b.f[i], imm);
                }
                *xw(cpu, d.reg) = r.q;
            }
            OCERZ_OP_ROUNDPD => {
                let mut r = ZV;
                for i in 0..2 {
                    r.d[i] = round_imm_d(b.d[i], imm);
                }
                *xw(cpu, d.reg) = r.q;
            }
            OCERZ_OP_ROUNDSS => {
                let mut a = src1_of(cpu, insn, d);
                a.f[0] = round_imm_f(b.f[0], imm);
                *xw(cpu, d.reg) = a.q;
            }
            OCERZ_OP_ROUNDSD => {
                let mut a = src1_of(cpu, insn, d);
                a.d[0] = round_imm_d(b.d[0], imm);
                *xw(cpu, d.reg) = a.q;
            }
            _ => return EUNSUP,
        }
        STEP_OK
    }
}

fn aes_xtime(a: u8) -> u8 {
    (a << 1) ^ ((a >> 7).wrapping_mul(0x1b))
}

fn aes_gmul(mut a: u8, mut b: u8) -> u8 {
    let mut p = 0u8;
    for _ in 0..8 {
        if b & 1 != 0 {
            p ^= a;
        }
        b >>= 1;
        a = aes_xtime(a);
    }
    p
}

fn aes_subbytes(s: Vec, bx: &[u8; 256]) -> Vec {
    let mut r = ZV;
    unsafe {
        for i in 0..16 {
            r.u8[i] = bx[s.u8[i] as usize];
        }
    }
    r
}

fn aes_shiftrows(s: Vec, perm: &[u8; 16]) -> Vec {
    let mut r = ZV;
    unsafe {
        for i in 0..16 {
            r.u8[i] = s.u8[perm[i] as usize];
        }
    }
    r
}

fn aes_mixcolumns(s: Vec) -> Vec {
    let mut r = ZV;
    unsafe {
        for c in 0..4 {
            let col = &s.u8[c * 4..c * 4 + 4];
            r.u8[c * 4] = aes_gmul(col[0], 2) ^ aes_gmul(col[1], 3) ^ col[2] ^ col[3];
            r.u8[c * 4 + 1] = col[0] ^ aes_gmul(col[1], 2) ^ aes_gmul(col[2], 3) ^ col[3];
            r.u8[c * 4 + 2] = col[0] ^ col[1] ^ aes_gmul(col[2], 2) ^ aes_gmul(col[3], 3);
            r.u8[c * 4 + 3] = aes_gmul(col[0], 3) ^ col[1] ^ col[2] ^ aes_gmul(col[3], 2);
        }
    }
    r
}

fn aes_invmixcolumns(s: Vec) -> Vec {
    let mut r = ZV;
    unsafe {
        for c in 0..4 {
            let col = &s.u8[c * 4..c * 4 + 4];
            r.u8[c * 4] = aes_gmul(col[0], 14) ^ aes_gmul(col[1], 11) ^ aes_gmul(col[2], 13) ^ aes_gmul(col[3], 9);
            r.u8[c * 4 + 1] = aes_gmul(col[0], 9) ^ aes_gmul(col[1], 14) ^ aes_gmul(col[2], 11) ^ aes_gmul(col[3], 13);
            r.u8[c * 4 + 2] = aes_gmul(col[0], 13) ^ aes_gmul(col[1], 9) ^ aes_gmul(col[2], 14) ^ aes_gmul(col[3], 11);
            r.u8[c * 4 + 3] = aes_gmul(col[0], 11) ^ aes_gmul(col[1], 13) ^ aes_gmul(col[2], 9) ^ aes_gmul(col[3], 14);
        }
    }
    r
}

fn aes_subword(w: u32) -> u32 {
    AES_SBOX[(w & 0xff) as usize] as u32
        | (AES_SBOX[((w >> 8) & 0xff) as usize] as u32) << 8
        | (AES_SBOX[((w >> 16) & 0xff) as usize] as u32) << 16
        | (AES_SBOX[((w >> 24) & 0xff) as usize] as u32) << 24
}

fn aes_rotr8(w: u32) -> u32 {
    w.rotate_right(8)
}

unsafe fn do_aes(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let a = src1_of(cpu, insn, d);
        let op = insn.op as u32;
        match op {
            OCERZ_OP_AESENC | OCERZ_OP_AESENCLAST => {
                let k = vec_read(cpu, insn, &insn.ops[1]);
                let mut t = aes_subbytes(aes_shiftrows(a, &AES_SHIFT_FWD), &AES_SBOX);
                if op == OCERZ_OP_AESENC {
                    t = aes_mixcolumns(t);
                }
                t.q = Ocerz128 { lo: t.q.lo ^ k.q.lo, hi: t.q.hi ^ k.q.hi };
                *xw(cpu, d.reg) = t.q;
            }
            OCERZ_OP_AESDEC | OCERZ_OP_AESDECLAST => {
                let k = vec_read(cpu, insn, &insn.ops[1]);
                let mut t = aes_subbytes(aes_shiftrows(a, &AES_SHIFT_INV), &AES_ISBOX);
                if op == OCERZ_OP_AESDEC {
                    t = aes_invmixcolumns(t);
                }
                t.q = Ocerz128 { lo: t.q.lo ^ k.q.lo, hi: t.q.hi ^ k.q.hi };
                *xw(cpu, d.reg) = t.q;
            }
            OCERZ_OP_AESIMC => {
                let b = vec_read(cpu, insn, &insn.ops[1]);
                *xw(cpu, d.reg) = aes_invmixcolumns(b).q;
            }
            OCERZ_OP_AESKEYGENASSIST => {
                let b = vec_read(cpu, insn, &insn.ops[1]);
                let rcon = (insn.ops[2].imm & 0xff) as u32;
                let s1 = aes_subword(b.u32[1]);
                let s3 = aes_subword(b.u32[3]);
                *xw(cpu, d.reg) = Vec { u32: [s1, aes_rotr8(s1) ^ rcon, s3, aes_rotr8(s3) ^ rcon] }.q;
            }
            _ => return EUNSUP,
        }
        STEP_OK
    }
}

unsafe fn do_pclmul(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let a = src1_of(cpu, insn, d);
        let b = vec_read(cpu, insn, &insn.ops[1]);
        let imm = (insn.ops[2].imm & 0xff) as i32;
        let x = if imm & 0x01 != 0 { a.u64[1] } else { a.u64[0] };
        let y = if imm & 0x10 != 0 { b.u64[1] } else { b.u64[0] };
        let mut lo = 0u64;
        let mut hi = 0u64;
        for i in 0..64 {
            if (y >> i) & 1 != 0 {
                lo ^= x << i;
                if i != 0 {
                    hi ^= x >> (64 - i);
                }
            }
        }
        *xw(cpu, d.reg) = Ocerz128 { lo, hi };
        STEP_OK
    }
}

unsafe fn sse_exec(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        match insn.op as u32 {
            OCERZ_OP_MOVUPS | OCERZ_OP_MOVAPS | OCERZ_OP_MOVDQA | OCERZ_OP_MOVDQU | OCERZ_OP_MOVSS | OCERZ_OP_MOVSDX | OCERZ_OP_MOVD | OCERZ_OP_MOVQX | OCERZ_OP_MOVLPS | OCERZ_OP_MOVHPS | OCERZ_OP_MOVLHPS | OCERZ_OP_MOVHLPS | OCERZ_OP_MOVMSKPS | OCERZ_OP_MOVMSKPD | OCERZ_OP_PMOVMSKB | OCERZ_OP_MOVSHDUP | OCERZ_OP_MOVSLDUP | OCERZ_OP_MOVDDUP => do_moves(cpu, insn),
            OCERZ_OP_ADDPS | OCERZ_OP_ADDPD | OCERZ_OP_ADDSS | OCERZ_OP_ADDSD | OCERZ_OP_HADDPS | OCERZ_OP_HADDPD | OCERZ_OP_HSUBPS | OCERZ_OP_HSUBPD | OCERZ_OP_ADDSUBPS | OCERZ_OP_ADDSUBPD | OCERZ_OP_SUBPS | OCERZ_OP_SUBPD | OCERZ_OP_SUBSS | OCERZ_OP_SUBSD | OCERZ_OP_MULPS | OCERZ_OP_MULPD | OCERZ_OP_MULSS | OCERZ_OP_MULSD | OCERZ_OP_DIVPS | OCERZ_OP_DIVPD | OCERZ_OP_DIVSS | OCERZ_OP_DIVSD | OCERZ_OP_MINPS | OCERZ_OP_MINPD | OCERZ_OP_MINSS | OCERZ_OP_MINSD | OCERZ_OP_MAXPS | OCERZ_OP_MAXPD | OCERZ_OP_MAXSS | OCERZ_OP_MAXSD | OCERZ_OP_SQRTPS | OCERZ_OP_SQRTPD | OCERZ_OP_SQRTSS | OCERZ_OP_SQRTSD | OCERZ_OP_RSQRTPS | OCERZ_OP_RSQRTSS | OCERZ_OP_RCPPS | OCERZ_OP_RCPSS => do_fp_arith(cpu, insn),
            OCERZ_OP_ANDPS | OCERZ_OP_ANDNPS | OCERZ_OP_ORPS | OCERZ_OP_XORPS | OCERZ_OP_PAND | OCERZ_OP_PANDN | OCERZ_OP_POR | OCERZ_OP_PXOR => do_logic(cpu, insn),
            OCERZ_OP_CMPPS | OCERZ_OP_CMPPD | OCERZ_OP_CMPSS | OCERZ_OP_CMPSDX => do_cmpfp(cpu, insn),
            OCERZ_OP_COMISS | OCERZ_OP_COMISD | OCERZ_OP_UCOMISS | OCERZ_OP_UCOMISD => do_comis(cpu, insn),
            OCERZ_OP_PCMPEQB | OCERZ_OP_PCMPEQW | OCERZ_OP_PCMPEQD | OCERZ_OP_PCMPEQQ | OCERZ_OP_PCMPGTB | OCERZ_OP_PCMPGTW | OCERZ_OP_PCMPGTD | OCERZ_OP_PCMPGTQ => do_pcmp(cpu, insn),
            OCERZ_OP_CVTSI2SS | OCERZ_OP_CVTSI2SD | OCERZ_OP_CVTSS2SI | OCERZ_OP_CVTSD2SI | OCERZ_OP_CVTTSS2SI | OCERZ_OP_CVTTSD2SI | OCERZ_OP_CVTSS2SD | OCERZ_OP_CVTSD2SS | OCERZ_OP_CVTPS2PD | OCERZ_OP_CVTPD2PS | OCERZ_OP_CVTDQ2PS | OCERZ_OP_CVTPS2DQ | OCERZ_OP_CVTTPS2DQ | OCERZ_OP_CVTDQ2PD | OCERZ_OP_CVTPD2DQ | OCERZ_OP_CVTTPD2DQ => do_convert(cpu, insn),
            OCERZ_OP_PADDB | OCERZ_OP_PADDW | OCERZ_OP_PADDD | OCERZ_OP_PADDQ | OCERZ_OP_PSUBB | OCERZ_OP_PSUBW | OCERZ_OP_PSUBD | OCERZ_OP_PSUBQ | OCERZ_OP_PADDSB | OCERZ_OP_PADDSW | OCERZ_OP_PADDUSB | OCERZ_OP_PADDUSW | OCERZ_OP_PSUBSB | OCERZ_OP_PSUBSW | OCERZ_OP_PSUBUSB | OCERZ_OP_PSUBUSW | OCERZ_OP_PMULLW | OCERZ_OP_PMULLD | OCERZ_OP_PMULHW | OCERZ_OP_PMULHUW | OCERZ_OP_PMULUDQ | OCERZ_OP_PMULDQ | OCERZ_OP_PMADDWD | OCERZ_OP_PAVGB | OCERZ_OP_PAVGW | OCERZ_OP_PMAXUB | OCERZ_OP_PMAXSW | OCERZ_OP_PMINUB | OCERZ_OP_PMINSW | OCERZ_OP_PMAXSB | OCERZ_OP_PMAXSD | OCERZ_OP_PMAXUW | OCERZ_OP_PMAXUD | OCERZ_OP_PMINSB | OCERZ_OP_PMINSD | OCERZ_OP_PMINUW | OCERZ_OP_PMINUD | OCERZ_OP_PSADBW | OCERZ_OP_PABSB | OCERZ_OP_PABSW | OCERZ_OP_PABSD | OCERZ_OP_PHADDW | OCERZ_OP_PHADDD | OCERZ_OP_PHADDSW | OCERZ_OP_PHSUBW | OCERZ_OP_PHSUBD | OCERZ_OP_PHSUBSW | OCERZ_OP_PSIGNB | OCERZ_OP_PSIGNW | OCERZ_OP_PSIGND | OCERZ_OP_PMADDUBSW | OCERZ_OP_PMULHRSW => do_int_arith(cpu, insn),
            OCERZ_OP_PACKSSWB | OCERZ_OP_PACKSSDW | OCERZ_OP_PACKUSWB | OCERZ_OP_PACKUSDW | OCERZ_OP_PUNPCKLBW | OCERZ_OP_PUNPCKLWD | OCERZ_OP_PUNPCKLDQ | OCERZ_OP_PUNPCKLQDQ | OCERZ_OP_PUNPCKHBW | OCERZ_OP_PUNPCKHWD | OCERZ_OP_PUNPCKHDQ | OCERZ_OP_PUNPCKHQDQ | OCERZ_OP_PSHUFD | OCERZ_OP_PSHUFLW | OCERZ_OP_PSHUFHW | OCERZ_OP_PSHUFB | OCERZ_OP_PALIGNR | OCERZ_OP_SHUFPS | OCERZ_OP_SHUFPD | OCERZ_OP_UNPCKLPS | OCERZ_OP_UNPCKHPS | OCERZ_OP_UNPCKLPD | OCERZ_OP_UNPCKHPD => do_pack_shuffle(cpu, insn),
            OCERZ_OP_PSLLW | OCERZ_OP_PSLLD | OCERZ_OP_PSLLQ | OCERZ_OP_PSRLW | OCERZ_OP_PSRLD | OCERZ_OP_PSRLQ | OCERZ_OP_PSRAW | OCERZ_OP_PSRAD | OCERZ_OP_PSLLDQ | OCERZ_OP_PSRLDQ => do_shift(cpu, insn),
            OCERZ_OP_PEXTRB | OCERZ_OP_PEXTRW | OCERZ_OP_PEXTRD | OCERZ_OP_PEXTRQ | OCERZ_OP_PINSRB | OCERZ_OP_PINSRW | OCERZ_OP_PINSRD | OCERZ_OP_PINSRQ | OCERZ_OP_EXTRACTPS | OCERZ_OP_INSERTPS => do_insert_extract(cpu, insn),
            OCERZ_OP_PTEST | OCERZ_OP_PBLENDW | OCERZ_OP_BLENDPS | OCERZ_OP_BLENDPD | OCERZ_OP_PBLENDVB | OCERZ_OP_BLENDVPS | OCERZ_OP_BLENDVPD => do_ptest_blend(cpu, insn),
            OCERZ_OP_PMOVZXBW | OCERZ_OP_PMOVZXBD | OCERZ_OP_PMOVZXBQ | OCERZ_OP_PMOVZXWD | OCERZ_OP_PMOVZXWQ | OCERZ_OP_PMOVZXDQ | OCERZ_OP_PMOVSXBW | OCERZ_OP_PMOVSXBD | OCERZ_OP_PMOVSXBQ | OCERZ_OP_PMOVSXWD | OCERZ_OP_PMOVSXWQ | OCERZ_OP_PMOVSXDQ => do_pmovx(cpu, insn),
            OCERZ_OP_ROUNDPS | OCERZ_OP_ROUNDPD | OCERZ_OP_ROUNDSS | OCERZ_OP_ROUNDSD => do_round(cpu, insn),
            OCERZ_OP_PCMPESTRM | OCERZ_OP_PCMPESTRI | OCERZ_OP_PCMPISTRM | OCERZ_OP_PCMPISTRI => do_pcmpstr(cpu, insn),
            OCERZ_OP_PHMINPOSUW | OCERZ_OP_MPSADBW | OCERZ_OP_DPPS | OCERZ_OP_DPPD => do_sse41_misc(cpu, insn),
            OCERZ_OP_AESENC | OCERZ_OP_AESENCLAST | OCERZ_OP_AESDEC | OCERZ_OP_AESDECLAST | OCERZ_OP_AESIMC | OCERZ_OP_AESKEYGENASSIST => do_aes(cpu, insn),
            OCERZ_OP_PCLMULQDQ => do_pclmul(cpu, insn),
            OCERZ_OP_MOVNTI => {
                let v = ocerz_read_op(cpu, insn, &insn.ops[1]);
                ocerz_write_op(cpu, insn, &insn.ops[0], v);
                STEP_OK
            }
            _ => do_avx_only(cpu, insn),
        }
    }
}

fn f2h(f: f32) -> u16 {
    let x = f2bits(f);
    let sign = (x >> 16) & 0x8000;
    let exp = ((x >> 23) & 0xff) as i32 - 127 + 15;
    let mut mant = x & 0x7fffff;
    if ((x >> 23) & 0xff) == 0xff {
        return (sign | 0x7c00 | if mant != 0 { 0x200 | (mant >> 13) } else { 0 }) as u16;
    }
    if exp >= 0x1f {
        return (sign | 0x7c00) as u16;
    }
    if exp <= 0 {
        if exp < -10 {
            return sign as u16;
        }
        mant |= 0x800000;
        let shift = (14 - exp) as u32;
        let mut hm = mant >> shift;
        let rem = mant & ((1u32 << shift) - 1);
        let half = 1u32 << (shift - 1);
        if rem > half || (rem == half && (hm & 1) != 0) {
            hm += 1;
        }
        return (sign | hm) as u16;
    }
    let hm = mant >> 13;
    let rem = mant & 0x1fff;
    let mut h = sign | ((exp as u32) << 10) | hm;
    if rem > 0x1000 || (rem == 0x1000 && (hm & 1) != 0) {
        h += 1;
    }
    h as u16
}

fn h2f(h: u16) -> f32 {
    let sign = ((h & 0x8000) as u32) << 16;
    let exp = ((h >> 10) & 0x1f) as u32;
    let mut mant = (h & 0x3ff) as u32;
    let x = if exp == 0x1f {
        sign | 0x7f800000 | (mant << 13)
    } else if exp == 0 {
        if mant == 0 {
            sign
        } else {
            let mut e: i32 = -1;
            loop {
                e += 1;
                mant <<= 1;
                if mant & 0x400 != 0 {
                    break;
                }
            }
            sign | (((127 - 15 - e) as u32) << 23) | ((mant & 0x3ff) << 13)
        }
    } else {
        sign | ((exp + 127 - 15) << 23) | (mant << 13)
    };
    f32::from_bits(x)
}

#[inline(always)]
unsafe fn mem_read16(cpu: &mut OcerzCPU, insn: &X86Insn, op: &X86Operand, off: i32) -> Ocerz128 {
    unsafe {
        let mut t = *op;
        t.disp = t.disp.wrapping_add(off as _);
        t.size = 16;
        ocerz_read_op128(cpu, insn, &t)
    }
}

#[inline(always)]
unsafe fn mem_write16(cpu: &mut OcerzCPU, insn: &X86Insn, op: &X86Operand, off: i32, v: Ocerz128) {
    unsafe {
        let mut t = *op;
        t.disp = t.disp.wrapping_add(off as _);
        t.size = 16;
        ocerz_write_op128(cpu, insn, &t, v)
    }
}

#[inline(always)]
unsafe fn read256(cpu: &mut OcerzCPU, insn: &X86Insn, op: &X86Operand, lo: &mut Vec, hi: &mut Vec) {
    unsafe {
        if op.kind as u32 == OCERZ_OPK_XMM {
            *lo = vec_of(xr(cpu, op.reg));
            *hi = vec_of(*cpu.ymmh.get_unchecked(op.reg as usize));
        } else {
            *lo = vec_of(mem_read16(cpu, insn, op, 0));
            *hi = vec_of(mem_read16(cpu, insn, op, 16));
        }
    }
}

#[inline(always)]
fn write_ymm(cpu: &mut OcerzCPU, reg: usize, lo: Vec, hi: Vec) {
    unsafe {
        cpu.xmm[reg] = lo.q;
        cpu.ymmh[reg] = hi.q;
    }
    cpu.ymmh_all_zero = 0;
}

#[inline(always)]
fn write_xmm_zero_hi(cpu: &mut OcerzCPU, reg: usize, v: Vec) {
    unsafe {
        cpu.xmm[reg] = v.q;
    }
    cpu.ymmh[reg] = ZQ;
}

#[derive(Clone, Copy)]
#[repr(C)]
union W256 {
    u8: [u8; 32],
    u32: [u32; 8],
    i32: [i32; 8],
    u64: [u64; 4],
    i64: [i64; 4],
    q: [Ocerz128; 2],
}

#[inline(always)]
fn reg256(cpu: &OcerzCPU, reg: usize) -> W256 {
    W256 { q: [cpu.xmm[reg], cpu.ymmh[reg]] }
}

#[inline(always)]
unsafe fn src256(cpu: &mut OcerzCPU, insn: &X86Insn, op: &X86Operand, l: bool) -> W256 {
    unsafe {
        if op.kind as u32 == OCERZ_OPK_XMM {
            return reg256(cpu, op.reg as usize);
        }
        let lo = mem_read16(cpu, insn, op, 0);
        let hi = if l { mem_read16(cpu, insn, op, 16) } else { ZQ };
        W256 { q: [lo, hi] }
    }
}

#[inline(always)]
fn put256(cpu: &mut OcerzCPU, reg: usize, l: bool, w: &W256) {
    unsafe {
        cpu.xmm[reg] = w.q[0];
        if l {
            cpu.ymmh[reg] = w.q[1];
            cpu.ymmh_all_zero = 0;
        } else {
            cpu.ymmh[reg] = ZQ;
        }
    }
}

unsafe fn do_avx_only(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let s = &insn.ops[1];
        let l = (insn.vex as u32 & OCERZ_VEX_L) != 0;
        let dr = d.reg as usize;
        let mut r = ZV;
        let mut lo = ZV;
        let mut hi = ZV;
        let op = insn.op as u32;
        match op {
            OCERZ_OP_VBROADCASTSS => {
                let v = if s.kind as u32 == OCERZ_OPK_XMM {
                    xr(cpu, s.reg).lo as u32
                } else {
                    ocerz_ld(ocerz_ea(cpu, insn, s), 4) as u32
                };
                r.u32 = [v; 4];
                if l { write_ymm(cpu, dr, r, r) } else { write_xmm_zero_hi(cpu, dr, r) }
                STEP_OK
            }
            OCERZ_OP_VBROADCASTSD => {
                let v = if s.kind as u32 == OCERZ_OPK_XMM { xr(cpu, s.reg).lo } else { ocerz_ld(ocerz_ea(cpu, insn, s), 8) };
                r.u64 = [v; 2];
                if l { write_ymm(cpu, dr, r, r) } else { write_xmm_zero_hi(cpu, dr, r) }
                STEP_OK
            }
            OCERZ_OP_VBROADCASTF128 | OCERZ_OP_VBROADCASTI128 => {
                r = vec_of(mem_read16(cpu, insn, s, 0));
                write_ymm(cpu, dr, r, r);
                STEP_OK
            }
            OCERZ_OP_VPERMILPS | OCERZ_OP_VPERMILPD => {
                let pd = op == OCERZ_OP_VPERMILPD;
                let immform = insn.ops[2].kind as u32 == OCERZ_OPK_IMM;
                let (mut dlo, mut dhi, mut clo, mut chi) = (ZV, ZV, ZV, ZV);
                if immform {
                    read256(cpu, insn, s, &mut dlo, &mut dhi);
                } else {
                    let v = insn.vvvv as usize;
                    dlo = vec_of(cpu.xmm[v]);
                    dhi = vec_of(cpu.ymmh[v]);
                    read256(cpu, insn, s, &mut clo, &mut chi);
                }
                let imm = if immform { insn.ops[2].imm as u32 } else { 0 };
                for lane in 0..(if l { 2 } else { 1 }) {
                    let dd = if lane != 0 { dhi } else { dlo };
                    let cc = if lane != 0 { chi } else { clo };
                    let mut rr = ZV;
                    if pd {
                        for i in 0..2 {
                            let sel = if immform { (imm >> (lane * 2 + i)) & 1 } else { ((cc.u64[i] >> 1) & 1) as u32 };
                            rr.d[i] = dd.d[sel as usize];
                        }
                    } else {
                        for i in 0..4 {
                            let sel = if immform { (imm >> (2 * i)) & 3 } else { cc.u32[i] & 3 };
                            rr.f[i] = dd.f[sel as usize];
                        }
                    }
                    if lane != 0 { hi = rr } else { lo = rr }
                }
                if l { write_ymm(cpu, dr, lo, hi) } else { write_xmm_zero_hi(cpu, dr, lo) }
                STEP_OK
            }
            OCERZ_OP_VTESTPS | OCERZ_OP_VTESTPD => {
                let alo = vec_of(cpu.xmm[dr]);
                let ahi = vec_of(cpu.ymmh[dr]);
                let (mut blo, mut bhi) = (ZV, ZV);
                read256(cpu, insn, s, &mut blo, &mut bhi);
                let m: u64 = if op == OCERZ_OP_VTESTPS { 0x8000000080000000 } else { 0x8000000000000000 };
                let mut and_ = ((alo.u64[0] & blo.u64[0]) | (alo.u64[1] & blo.u64[1])) & m;
                let mut andn = ((!alo.u64[0] & blo.u64[0]) | (!alo.u64[1] & blo.u64[1])) & m;
                if l {
                    and_ |= ((ahi.u64[0] & bhi.u64[0]) | (ahi.u64[1] & bhi.u64[1])) & m;
                    andn |= ((!ahi.u64[0] & bhi.u64[0]) | (!ahi.u64[1] & bhi.u64[1])) & m;
                }
                ocerz_flag_assign(cpu, OCERZ_ZF, (and_ == 0) as i32);
                ocerz_flag_assign(cpu, OCERZ_CF, (andn == 0) as i32);
                ocerz_flag_assign(cpu, OCERZ_PF, 0);
                ocerz_flag_assign(cpu, OCERZ_AF, 0);
                ocerz_flag_assign(cpu, OCERZ_SF, 0);
                ocerz_flag_assign(cpu, OCERZ_OF, 0);
                STEP_OK
            }
            OCERZ_OP_VCVTPH2PS => {
                let b = if s.kind as u32 == OCERZ_OPK_XMM {
                    vec_of(xr(cpu, s.reg))
                } else {
                    let lo64 = ocerz_ld(ocerz_ea(cpu, insn, s), 8);
                    let hi64 = if l { ocerz_ld(ocerz_ea(cpu, insn, s).wrapping_add(8), 8) } else { 0 };
                    vec_of(Ocerz128 { lo: lo64, hi: hi64 })
                };
                for i in 0..4 {
                    lo.f[i] = h2f(b.u16[i]);
                }
                if l {
                    for i in 0..4 {
                        hi.f[i] = h2f(b.u16[4 + i]);
                    }
                    write_ymm(cpu, dr, lo, hi);
                } else {
                    write_xmm_zero_hi(cpu, dr, lo);
                }
                STEP_OK
            }
            OCERZ_OP_VCVTPS2PH => {
                let sr = s.reg as usize;
                let alo = vec_of(cpu.xmm[sr]);
                let ahi = vec_of(cpu.ymmh[sr]);
                for i in 0..4 {
                    r.u16[i] = f2h(alo.f[i]);
                }
                if l {
                    for i in 0..4 {
                        r.u16[4 + i] = f2h(ahi.f[i]);
                    }
                }
                if d.kind as u32 == OCERZ_OPK_XMM {
                    write_xmm_zero_hi(cpu, dr, r);
                } else {
                    ocerz_st(ocerz_ea(cpu, insn, d), 8, r.q.lo);
                    if l {
                        ocerz_st(ocerz_ea(cpu, insn, d).wrapping_add(8), 8, r.q.hi);
                    }
                }
                STEP_OK
            }
            OCERZ_OP_VINSERTF128 | OCERZ_OP_VINSERTI128 => {
                let v = insn.vvvv as usize;
                let mut base_lo = vec_of(cpu.xmm[v]);
                let mut base_hi = vec_of(cpu.ymmh[v]);
                let ins = vec_of(if s.kind as u32 == OCERZ_OPK_XMM { xr(cpu, s.reg) } else { mem_read16(cpu, insn, s, 0) });
                if insn.ops[2].imm & 1 != 0 { base_hi = ins } else { base_lo = ins }
                write_ymm(cpu, dr, base_lo, base_hi);
                STEP_OK
            }
            OCERZ_OP_VEXTRACTF128 | OCERZ_OP_VEXTRACTI128 => {
                let sr = s.reg as usize;
                let v = vec_of(if insn.ops[2].imm & 1 != 0 { cpu.ymmh[sr] } else { cpu.xmm[sr] });
                if d.kind as u32 == OCERZ_OPK_XMM {
                    write_xmm_zero_hi(cpu, dr, v);
                } else {
                    mem_write16(cpu, insn, d, 0, v.q);
                }
                STEP_OK
            }
            OCERZ_OP_VPERM2F128 | OCERZ_OP_VPERM2I128 => {
                let v = insn.vvvv as usize;
                let s1lo = vec_of(cpu.xmm[v]);
                let s1hi = vec_of(cpu.ymmh[v]);
                let (mut s2lo, mut s2hi) = (ZV, ZV);
                read256(cpu, insn, s, &mut s2lo, &mut s2hi);
                let imm = insn.ops[2].imm as u32;
                let pick = [s1lo, s1hi, s2lo, s2hi];
                lo = pick[(imm & 3) as usize];
                hi = pick[((imm >> 4) & 3) as usize];
                if imm & 0x08 != 0 {
                    lo = ZV;
                }
                if imm & 0x80 != 0 {
                    hi = ZV;
                }
                write_ymm(cpu, dr, lo, hi);
                STEP_OK
            }
            OCERZ_OP_VPBROADCASTB | OCERZ_OP_VPBROADCASTW | OCERZ_OP_VPBROADCASTD | OCERZ_OP_VPBROADCASTQ => {
                let esz: usize = match op {
                    OCERZ_OP_VPBROADCASTB => 1,
                    OCERZ_OP_VPBROADCASTW => 2,
                    OCERZ_OP_VPBROADCASTD => 4,
                    _ => 8,
                };
                let v = if s.kind as u32 == OCERZ_OPK_XMM { xr(cpu, s.reg).lo } else { ocerz_ld(ocerz_ea(cpu, insn, s), esz as i32) };
                let vb = v.to_le_bytes();
                let mut w = W256 { u8: [0; 32] };
                let mut i = 0;
                while i < 32 {
                    w.u8[i..i + esz].copy_from_slice(&vb[..esz]);
                    i += esz;
                }
                put256(cpu, dr, l, &w);
                STEP_OK
            }
            OCERZ_OP_VPBLENDD => {
                let mut a = reg256(cpu, insn.vvvv as usize);
                let b = src256(cpu, insn, s, l);
                for i in 0..8 {
                    if (insn.ops[2].imm >> i) & 1 != 0 {
                        a.u32[i] = b.u32[i];
                    }
                }
                put256(cpu, dr, l, &a);
                STEP_OK
            }
            OCERZ_OP_VPERMD | OCERZ_OP_VPERMPS => {
                let ix = reg256(cpu, insn.vvvv as usize);
                let t = src256(cpu, insn, s, true);
                let mut w = W256 { u8: [0; 32] };
                for i in 0..8 {
                    w.u32[i] = t.u32[(ix.u32[i] & 7) as usize];
                }
                put256(cpu, dr, true, &w);
                STEP_OK
            }
            OCERZ_OP_VPERMQ | OCERZ_OP_VPERMPD => {
                let t = src256(cpu, insn, s, true);
                let mut w = W256 { u8: [0; 32] };
                for i in 0..4 {
                    w.u64[i] = t.u64[((insn.ops[2].imm >> (2 * i)) & 3) as usize];
                }
                put256(cpu, dr, true, &w);
                STEP_OK
            }
            OCERZ_OP_VPSLLVD | OCERZ_OP_VPSRLVD | OCERZ_OP_VPSRAVD => {
                let mut a = reg256(cpu, insn.vvvv as usize);
                let c = src256(cpu, insn, s, l);
                for i in 0..8 {
                    let n = c.u32[i];
                    if op == OCERZ_OP_VPSRAVD {
                        a.i32[i] >>= if n > 31 { 31 } else { n };
                    } else {
                        a.u32[i] = if n > 31 {
                            0
                        } else if op == OCERZ_OP_VPSLLVD {
                            a.u32[i] << n
                        } else {
                            a.u32[i] >> n
                        };
                    }
                }
                put256(cpu, dr, l, &a);
                STEP_OK
            }
            OCERZ_OP_VPSLLVQ | OCERZ_OP_VPSRLVQ => {
                let mut a = reg256(cpu, insn.vvvv as usize);
                let c = src256(cpu, insn, s, l);
                for i in 0..4 {
                    let n = c.u64[i];
                    a.u64[i] = if n > 63 {
                        0
                    } else if op == OCERZ_OP_VPSLLVQ {
                        a.u64[i] << n
                    } else {
                        a.u64[i] >> n
                    };
                }
                put256(cpu, dr, l, &a);
                STEP_OK
            }
            OCERZ_OP_VPMASKMOVD | OCERZ_OP_VPMASKMOVQ | OCERZ_OP_VMASKMOVPS | OCERZ_OP_VMASKMOVPD => {
                let esz: usize = if op == OCERZ_OP_VPMASKMOVQ || op == OCERZ_OP_VMASKMOVPD { 8 } else { 4 };
                let n = (if l { 32 } else { 16 }) / esz;
                let m = reg256(cpu, insn.vvvv as usize);
                let on = |i: usize| if esz == 8 { m.u64[i] >> 63 != 0 } else { m.u32[i] >> 31 != 0 };
                if d.kind as u32 == OCERZ_OPK_MEM {
                    let w = reg256(cpu, s.reg as usize);
                    let ea = ocerz_ea(cpu, insn, d);
                    for i in 0..n {
                        if on(i) {
                            ocerz_st(
                                ea.wrapping_add((i * esz) as u64),
                                esz as i32,
                                if esz == 8 { w.u64[i] } else { w.u32[i] as u64 },
                            );
                        }
                    }
                    return STEP_OK;
                }
                let ea = ocerz_ea(cpu, insn, s);
                let mut w = W256 { u8: [0; 32] };
                for i in 0..n {
                    if !on(i) {
                        continue;
                    }
                    if esz == 8 {
                        w.u64[i] = ocerz_ld(ea.wrapping_add((i * 8) as u64), 8);
                    } else {
                        w.u32[i] = ocerz_ld(ea.wrapping_add((i * 4) as u64), 4) as u32;
                    }
                }
                put256(cpu, dr, l, &w);
                STEP_OK
            }
            OCERZ_OP_VPGATHERDD | OCERZ_OP_VPGATHERDQ | OCERZ_OP_VPGATHERQD | OCERZ_OP_VPGATHERQQ
            | OCERZ_OP_VGATHERDPS | OCERZ_OP_VGATHERDPD | OCERZ_OP_VGATHERQPS | OCERZ_OP_VGATHERQPD => {
                let k = if op >= OCERZ_OP_VGATHERDPS { op - OCERZ_OP_VGATHERDPS } else { op - OCERZ_OP_VPGATHERDD };
                let q = k & 1 != 0;
                let qidx = (k >> 1) & 1 != 0;
                let esz: usize = if q { 8 } else { 4 };
                let n: usize = if q || qidx { if l { 4 } else { 2 } } else if l { 8 } else { 4 };
                let mut w = reg256(cpu, dr);
                let mut m = reg256(cpu, insn.vvvv as usize);
                let ix = reg256(cpu, s.index as usize);
                let seg = if insn.seg as u32 == OCERZ_SEG_FS {
                    cpu.fs_base
                } else if insn.seg as u32 == OCERZ_SEG_GS {
                    cpu.gs_base
                } else {
                    0
                };
                let base = (s.disp as u64).wrapping_add(if s.base as u32 != OCERZ_REG_NONE { cpu.gpr[s.base as usize] } else { 0 });
                for i in 0..n {
                    if !(if q { m.u64[i] >> 63 != 0 } else { m.u32[i] >> 31 != 0 }) {
                        continue;
                    }
                    let idx = if qidx { ix.i64[i] } else { ix.i32[i] as i64 };
                    let mut a = base.wrapping_add((idx as u64) << s.scale);
                    if insn.addrsize == 4 {
                        a = a as u32 as u64;
                    }
                    let v = ocerz_ld(a.wrapping_add(seg), esz as i32);
                    if q { w.u64[i] = v } else { w.u32[i] = v as u32 }
                }
                for b in &mut w.u8[n * esz..] {
                    *b = 0;
                }
                m = W256 { u8: [0; 32] };
                put256(cpu, dr, true, &w);
                put256(cpu, insn.vvvv as usize, true, &m);
                STEP_OK
            }
            OCERZ_OP_VZEROUPPER => {
                cpu.ymmh = [ZQ; 16];
                cpu.ymmh_all_zero = 1;
                STEP_OK
            }
            OCERZ_OP_VZEROALL => {
                cpu.ymmh = [ZQ; 16];
                cpu.xmm = [ZQ; 16];
                cpu.ymmh_all_zero = 1;
                STEP_OK
            }
            _ => EUNSUP,
        }
    }
}

unsafe fn do_avx256_cross(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let s = &insn.ops[1];
        let dr = d.reg as usize;
        let mut lo = ZV;
        let mut hi = ZV;
        let mut r = ZV;
        let op = insn.op as u32;
        match op {
            OCERZ_OP_CVTPD2PS | OCERZ_OP_CVTPD2DQ | OCERZ_OP_CVTTPD2DQ => {
                read256(cpu, insn, s, &mut lo, &mut hi);
                let v = [lo.d[0], lo.d[1], hi.d[0], hi.d[1]];
                for i in 0..4 {
                    if op == OCERZ_OP_CVTPD2PS {
                        r.f[i] = v[i] as f32;
                    } else {
                        r.i32[i] = cvt_d2i32(v[i], op == OCERZ_OP_CVTTPD2DQ);
                    }
                }
                write_xmm_zero_hi(cpu, dr, r);
                STEP_OK
            }
            OCERZ_OP_CVTPS2PD | OCERZ_OP_CVTDQ2PD => {
                let b = vec_of(if s.kind as u32 == OCERZ_OPK_XMM { xr(cpu, s.reg) } else { mem_read16(cpu, insn, s, 0) });
                let mut rl = ZV;
                let mut rh = ZV;
                for i in 0..2 {
                    if op == OCERZ_OP_CVTPS2PD {
                        rl.d[i] = b.f[i] as f64;
                        rh.d[i] = b.f[2 + i] as f64;
                    } else {
                        rl.d[i] = b.i32[i] as f64;
                        rh.d[i] = b.i32[2 + i] as f64;
                    }
                }
                write_ymm(cpu, dr, rl, rh);
                STEP_OK
            }
            OCERZ_OP_MOVMSKPS | OCERZ_OP_MOVMSKPD | OCERZ_OP_PMOVMSKB => {
                let sr = s.reg as usize;
                let a = vec_of(cpu.xmm[sr]);
                let b = vec_of(cpu.ymmh[sr]);
                let mut m = 0u64;
                if op == OCERZ_OP_MOVMSKPS {
                    for i in 0..4 {
                        m |= ((a.u32[i] >> 31) as u64) << i;
                        m |= ((b.u32[i] >> 31) as u64) << (4 + i);
                    }
                } else if op == OCERZ_OP_MOVMSKPD {
                    for i in 0..2 {
                        m |= (a.u64[i] >> 63) << i;
                        m |= (b.u64[i] >> 63) << (2 + i);
                    }
                } else {
                    for i in 0..16 {
                        m |= ((a.u8[i] >> 7) as u64) << i;
                        m |= ((b.u8[i] >> 7) as u64) << (16 + i);
                    }
                }
                ocerz_write_op(cpu, insn, d, m);
                STEP_OK
            }
            OCERZ_OP_PTEST => {
                let alo = vec_of(cpu.xmm[dr]);
                let ahi = vec_of(cpu.ymmh[dr]);
                let (mut blo, mut bhi) = (ZV, ZV);
                read256(cpu, insn, s, &mut blo, &mut bhi);
                let zf = ((alo.u64[0] & blo.u64[0]) | (alo.u64[1] & blo.u64[1]) | (ahi.u64[0] & bhi.u64[0]) | (ahi.u64[1] & bhi.u64[1])) == 0;
                let cf = ((!alo.u64[0] & blo.u64[0]) | (!alo.u64[1] & blo.u64[1]) | (!ahi.u64[0] & bhi.u64[0]) | (!ahi.u64[1] & bhi.u64[1])) == 0;
                ocerz_flag_assign(cpu, OCERZ_ZF, zf as i32);
                ocerz_flag_assign(cpu, OCERZ_CF, cf as i32);
                ocerz_flag_assign(cpu, OCERZ_PF, 0);
                ocerz_flag_assign(cpu, OCERZ_AF, 0);
                ocerz_flag_assign(cpu, OCERZ_SF, 0);
                ocerz_flag_assign(cpu, OCERZ_OF, 0);
                STEP_OK
            }
            OCERZ_OP_PMOVZXBW | OCERZ_OP_PMOVZXBD | OCERZ_OP_PMOVZXBQ | OCERZ_OP_PMOVZXWD | OCERZ_OP_PMOVZXWQ
            | OCERZ_OP_PMOVZXDQ | OCERZ_OP_PMOVSXBW | OCERZ_OP_PMOVSXBD | OCERZ_OP_PMOVSXBQ | OCERZ_OP_PMOVSXWD
            | OCERZ_OP_PMOVSXWQ | OCERZ_OP_PMOVSXDQ => {
                let (inw, outw, sgn): (usize, usize, bool) = match op {
                    OCERZ_OP_PMOVZXBW => (1, 2, false),
                    OCERZ_OP_PMOVSXBW => (1, 2, true),
                    OCERZ_OP_PMOVZXBD => (1, 4, false),
                    OCERZ_OP_PMOVSXBD => (1, 4, true),
                    OCERZ_OP_PMOVZXBQ => (1, 8, false),
                    OCERZ_OP_PMOVSXBQ => (1, 8, true),
                    OCERZ_OP_PMOVZXWD => (2, 4, false),
                    OCERZ_OP_PMOVSXWD => (2, 4, true),
                    OCERZ_OP_PMOVZXWQ => (2, 8, false),
                    OCERZ_OP_PMOVSXWQ => (2, 8, true),
                    _ => (4, 8, op == OCERZ_OP_PMOVSXDQ),
                };
                let n = 32 / outw;
                let srcbytes = (n * inw) as i32;
                let b = if s.kind as u32 == OCERZ_OPK_XMM {
                    vec_of(xr(cpu, s.reg))
                } else {
                    let qlo = ocerz_ld(ocerz_ea(cpu, insn, s), if srcbytes > 8 { 8 } else { srcbytes });
                    let qhi = if srcbytes > 8 { ocerz_ld(ocerz_ea(cpu, insn, s).wrapping_add(8), srcbytes - 8) } else { 0 };
                    vec_of(Ocerz128 { lo: qlo, hi: qhi })
                };
                let mut outb = [0u8; 32];
                for i in 0..n {
                    let v: i64 = match (inw, sgn) {
                        (1, true) => b.i8[i] as i64,
                        (1, false) => b.u8[i] as i64,
                        (2, true) => b.i16[i] as i64,
                        (2, false) => b.u16[i] as i64,
                        (_, true) => b.i32[i] as i64,
                        _ => b.u32[i] as i64,
                    };
                    outb[i * outw..i * outw + outw].copy_from_slice(&v.to_le_bytes()[..outw]);
                }
                let w = W256 { u8: outb };
                write_ymm(cpu, dr, vec_of(w.q[0]), vec_of(w.q[1]));
                STEP_OK
            }
            _ => EUNSUP,
        }
    }
}

fn fma_elem_d(a: f64, b: f64, c: f64, negmul: bool, negadd: bool) -> f64 {
    if a.is_nan() || b.is_nan() || c.is_nan() {
        let n = if a.is_nan() { a } else if b.is_nan() { b } else { c };
        return fixnan_d(n, n, n);
    }
    fixnan_d(0.0, 0.0, (if negmul { -a } else { a }).mul_add(b, if negadd { -c } else { c }))
}

fn fma_elem_f(a: f32, b: f32, c: f32, negmul: bool, negadd: bool) -> f32 {
    if a.is_nan() || b.is_nan() || c.is_nan() {
        let n = if a.is_nan() { a } else if b.is_nan() { b } else { c };
        return fixnan_f(n, n, n);
    }
    fixnan_f(0.0, 0.0, (if negmul { -a } else { a }).mul_add(b, if negadd { -c } else { c }))
}

unsafe fn do_fma(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let s = &insn.ops[1];
        let idx = insn.op as i32 - OCERZ_OP_VFMA_FIRST as i32;
        let pd = idx & 1 != 0;
        let kind = (idx >> 1) % 10;
        let order = (idx >> 1) / 10;
        let scalar = kind >= 3 && (kind & 1) != 0;
        let l = !scalar && (insn.vex as u32 & OCERZ_VEX_L) != 0;
        let n = if scalar { 1 } else { (if l { 4 } else { 2 }) * (if pd { 1 } else { 2 }) };
        let o1 = reg256(cpu, d.reg as usize);
        let o2 = reg256(cpu, insn.vvvv as usize);
        let o3 = if scalar && s.kind as u32 == OCERZ_OPK_MEM {
            let v = ocerz_ld(ocerz_ea(cpu, insn, s), if pd { 8 } else { 4 });
            W256 { u64: [v, 0, 0, 0] }
        } else {
            src256(cpu, insn, s, l)
        };
        let a = if order == 0 { &o1 } else { &o2 };
        let b = if order == 1 { &o1 } else { &o3 };
        let c = if order == 0 { &o2 } else if order == 1 { &o3 } else { &o1 };
        let negmul = kind >= 6;
        let mut r = o1;
        for i in 0..n as usize {
            let negadd = match kind {
                0 => i & 1 == 0,
                1 => i & 1 != 0,
                _ => kind == 4 || kind == 5 || kind >= 8,
            };
            if pd {
                let v = fma_elem_d(
                    f64::from_bits(a.u64[i]),
                    f64::from_bits(b.u64[i]),
                    f64::from_bits(c.u64[i]),
                    negmul,
                    negadd,
                );
                r.u64[i] = v.to_bits();
            } else {
                let v = fma_elem_f(
                    f32::from_bits(a.u32[i]),
                    f32::from_bits(b.u32[i]),
                    f32::from_bits(c.u32[i]),
                    negmul,
                    negadd,
                );
                r.u32[i] = v.to_bits();
            }
        }
        put256(cpu, d.reg as usize, l, &r);
        STEP_OK
    }
}

fn vex_lig(op: u32) -> bool {
    matches!(
        op,
        OCERZ_OP_ADDSS | OCERZ_OP_ADDSD | OCERZ_OP_SUBSS | OCERZ_OP_SUBSD | OCERZ_OP_MULSS | OCERZ_OP_MULSD
            | OCERZ_OP_DIVSS | OCERZ_OP_DIVSD | OCERZ_OP_MINSS | OCERZ_OP_MINSD | OCERZ_OP_MAXSS | OCERZ_OP_MAXSD
            | OCERZ_OP_SQRTSS | OCERZ_OP_SQRTSD | OCERZ_OP_RSQRTSS | OCERZ_OP_RCPSS | OCERZ_OP_CMPSS
            | OCERZ_OP_CMPSDX | OCERZ_OP_ROUNDSS | OCERZ_OP_ROUNDSD | OCERZ_OP_COMISS | OCERZ_OP_COMISD
            | OCERZ_OP_UCOMISS | OCERZ_OP_UCOMISD | OCERZ_OP_CVTSI2SS | OCERZ_OP_CVTSI2SD | OCERZ_OP_CVTSS2SI
            | OCERZ_OP_CVTSD2SI | OCERZ_OP_CVTTSS2SI | OCERZ_OP_CVTTSD2SI | OCERZ_OP_CVTSS2SD
            | OCERZ_OP_CVTSD2SS | OCERZ_OP_MOVSS | OCERZ_OP_MOVSDX
    )
}

fn vex_writes_xmm_dst(insn: &X86Insn) -> bool {
    if insn.ops[0].kind as u32 != OCERZ_OPK_XMM {
        return false;
    }
    !matches!(
        insn.op as u32,
        OCERZ_OP_COMISS | OCERZ_OP_COMISD | OCERZ_OP_UCOMISS | OCERZ_OP_UCOMISD | OCERZ_OP_PTEST
            | OCERZ_OP_VTESTPS | OCERZ_OP_VTESTPD
    )
}

fn vex_is_shift(op: u32) -> bool {
    matches!(
        op,
        OCERZ_OP_PSLLW | OCERZ_OP_PSLLD | OCERZ_OP_PSLLQ | OCERZ_OP_PSRLW | OCERZ_OP_PSRLD | OCERZ_OP_PSRLQ
            | OCERZ_OP_PSRAW | OCERZ_OP_PSRAD | OCERZ_OP_PSLLDQ | OCERZ_OP_PSRLDQ
    )
}

fn mmx_packs_halves(op: u32) -> bool {
    matches!(
        op,
        OCERZ_OP_PACKSSWB | OCERZ_OP_PACKSSDW | OCERZ_OP_PACKUSWB | OCERZ_OP_PHADDW | OCERZ_OP_PHADDD
            | OCERZ_OP_PHADDSW | OCERZ_OP_PHSUBW | OCERZ_OP_PHSUBD | OCERZ_OP_PHSUBSW
    )
}

#[inline(always)]
unsafe fn mmx_read(cpu: &mut OcerzCPU, insn: &X86Insn, op: &X86Operand) -> u64 {
    unsafe {
        if op.kind as u32 == OCERZ_OPK_MMX {
            return *cpu.mmx.get_unchecked(op.reg as usize);
        }
        if op.kind as u32 == OCERZ_OPK_XMM {
            return xr(cpu, op.reg).lo;
        }
        ocerz_read_op(cpu, insn, op)
    }
}

unsafe fn mmx_exec(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let d = &insn.ops[0];
        let s = &insn.ops[1];
        let dr = d.reg as usize;
        let sr = s.reg as usize;
        let op = insn.op as u32;
        match op {
            OCERZ_OP_MOVD | OCERZ_OP_MOVQX => {
                let v = mmx_read(cpu, insn, s);
                if d.kind as u32 == OCERZ_OPK_MMX {
                    cpu.mmx[dr] = v;
                } else if d.kind as u32 == OCERZ_OPK_XMM {
                    cpu.xmm[dr] = Ocerz128 { lo: v, hi: 0 };
                } else {
                    ocerz_write_op(cpu, insn, d, v);
                }
            }
            OCERZ_OP_PINSRW => {
                let sh = 16 * (insn.ops[2].imm & 3) as u32;
                let w = mmx_read(cpu, insn, s) & 0xffff;
                cpu.mmx[dr] = (cpu.mmx[dr] & !(0xffffu64 << sh)) | (w << sh);
            }
            OCERZ_OP_PEXTRW => {
                let v = (cpu.mmx[sr] >> (16 * (insn.ops[2].imm & 3))) & 0xffff;
                ocerz_write_gpr(cpu, d.reg as u32, 4, 0, v);
            }
            OCERZ_OP_PMOVMSKB => {
                let mut m = 0u64;
                for i in 0..8 {
                    m |= ((cpu.mmx[sr] >> (8 * i + 7)) & 1) << i;
                }
                ocerz_write_gpr(cpu, d.reg as u32, 4, 0, m);
            }
            OCERZ_OP_CVTPI2PS => {
                let v = mmx_read(cpu, insn, s);
                let mut r = vec_of(cpu.xmm[dr]);
                r.f[0] = v as i32 as f32;
                r.f[1] = (v >> 32) as i32 as f32;
                cpu.xmm[dr] = r.q;
            }
            OCERZ_OP_CVTPI2PD => {
                let v = mmx_read(cpu, insn, s);
                let r = Vec { d: [v as i32 as f64, (v >> 32) as i32 as f64] };
                cpu.xmm[dr] = r.q;
            }
            OCERZ_OP_CVTPS2PI | OCERZ_OP_CVTTPS2PI => {
                let b = vec_read(cpu, insn, s);
                let t = op == OCERZ_OP_CVTTPS2PI;
                cpu.mmx[dr] = cvt_f2i32(b.f[0], t) as u32 as u64 | (cvt_f2i32(b.f[1], t) as u32 as u64) << 32;
            }
            OCERZ_OP_CVTPD2PI | OCERZ_OP_CVTTPD2PI => {
                let b = vec_read(cpu, insn, s);
                let t = op == OCERZ_OP_CVTTPD2PI;
                cpu.mmx[dr] = cvt_d2i32(b.d[0], t) as u32 as u64 | (cvt_d2i32(b.d[1], t) as u32 as u64) << 32;
            }
            _ => {
                if d.kind as u32 != OCERZ_OPK_MMX {
                    return EUNSUP;
                }
                let has_src = s.kind as u32 == OCERZ_OPK_MMX || s.kind as u32 == OCERZ_OPK_MEM;
                let mut a = Ocerz128 { lo: cpu.mmx[dr], hi: 0 };
                let mut bv = Ocerz128 { lo: if has_src { mmx_read(cpu, insn, s) } else { 0 }, hi: 0 };
                let mut t = *insn;
                match op {
                    OCERZ_OP_PUNPCKHBW => {
                        t.op = OCERZ_OP_PUNPCKLBW as _;
                        a.lo >>= 32;
                        bv.lo >>= 32;
                    }
                    OCERZ_OP_PUNPCKHWD => {
                        t.op = OCERZ_OP_PUNPCKLWD as _;
                        a.lo >>= 32;
                        bv.lo >>= 32;
                    }
                    OCERZ_OP_PUNPCKHDQ => {
                        t.op = OCERZ_OP_PUNPCKLDQ as _;
                        a.lo >>= 32;
                        bv.lo >>= 32;
                    }
                    OCERZ_OP_PSHUFB => bv.lo &= 0x8787878787878787,
                    OCERZ_OP_PALIGNR => {
                        bv.hi = a.lo;
                        a.lo = 0;
                    }
                    _ => {}
                }
                t.ops[0].kind = OCERZ_OPK_XMM as _;
                t.ops[0].reg = 0;
                t.ops[0].size = 16;
                if has_src {
                    t.ops[1] = t.ops[0];
                    t.ops[1].reg = 1;
                }
                let keep0 = cpu.xmm[0];
                let keep1 = cpu.xmm[1];
                cpu.xmm[0] = a;
                cpu.xmm[1] = bv;
                let rc = sse_exec(cpu, &t);
                let r = cpu.xmm[0];
                cpu.xmm[0] = keep0;
                cpu.xmm[1] = keep1;
                if rc != STEP_OK {
                    return rc;
                }
                cpu.mmx[dr] = if mmx_packs_halves(op) { (r.lo & 0xffffffff) | ((r.hi & 0xffffffff) << 32) } else { r.lo };
            }
        }
        if insn_has_mmx(insn) {
            cpu.ftop = 0;
            cpu.ftw = 0xff;
        }
        STEP_OK
    }
}

#[inline(always)]
fn insn_has_mmx(insn: &X86Insn) -> bool {
    for i in 0..insn.nops as usize {
        if insn.ops[i].kind as u32 == OCERZ_OPK_MMX {
            return true;
        }
    }
    false
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_interp_sse(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, insn: *const X86Insn) -> i32 {
    unsafe {
        let cpu = &mut *cpu;
        let insn = &*insn;
        let op = insn.op as u32;
        if insn.vex == 0 {
            if insn_has_mmx(insn) || op == OCERZ_OP_CVTPI2PS || op == OCERZ_OP_CVTPI2PD {
                return mmx_exec(cpu, insn);
            }
            return sse_exec(cpu, insn);
        }
        match op {
            OCERZ_OP_VBROADCASTSS | OCERZ_OP_VBROADCASTSD | OCERZ_OP_VBROADCASTF128 | OCERZ_OP_VPERMILPS
            | OCERZ_OP_VPERMILPD | OCERZ_OP_VTESTPS | OCERZ_OP_VTESTPD | OCERZ_OP_VCVTPH2PS | OCERZ_OP_VCVTPS2PH
            | OCERZ_OP_VINSERTF128 | OCERZ_OP_VEXTRACTF128 | OCERZ_OP_VPERM2F128 | OCERZ_OP_VZEROUPPER
            | OCERZ_OP_VZEROALL | OCERZ_OP_VPBROADCASTB | OCERZ_OP_VPBROADCASTW | OCERZ_OP_VPBROADCASTD
            | OCERZ_OP_VPBROADCASTQ | OCERZ_OP_VBROADCASTI128 | OCERZ_OP_VINSERTI128 | OCERZ_OP_VEXTRACTI128
            | OCERZ_OP_VPERM2I128 | OCERZ_OP_VPBLENDD | OCERZ_OP_VPERMD | OCERZ_OP_VPERMPS | OCERZ_OP_VPERMQ
            | OCERZ_OP_VPERMPD | OCERZ_OP_VPSLLVD | OCERZ_OP_VPSLLVQ | OCERZ_OP_VPSRLVD | OCERZ_OP_VPSRLVQ
            | OCERZ_OP_VPSRAVD | OCERZ_OP_VPMASKMOVD | OCERZ_OP_VPMASKMOVQ | OCERZ_OP_VMASKMOVPS
            | OCERZ_OP_VMASKMOVPD | OCERZ_OP_VPGATHERDD | OCERZ_OP_VPGATHERDQ | OCERZ_OP_VPGATHERQD
            | OCERZ_OP_VPGATHERQQ | OCERZ_OP_VGATHERDPS | OCERZ_OP_VGATHERDPD | OCERZ_OP_VGATHERQPS
            | OCERZ_OP_VGATHERQPD => return do_avx_only(cpu, insn),
            _ => {}
        }
        if op >= OCERZ_OP_VFMA_FIRST && op <= OCERZ_OP_VFMA_LAST {
            return do_fma(cpu, insn);
        }
        if (insn.vex as u32 & OCERZ_VEX_L) == 0 || vex_lig(op) {
            let rc = sse_exec(cpu, insn);
            if rc != STEP_OK {
                return rc;
            }
            if vex_writes_xmm_dst(insn) {
                cpu.ymmh[insn.ops[0].reg as usize] = ZQ;
            }
            return STEP_OK;
        }
        match op {
            OCERZ_OP_CVTPD2PS | OCERZ_OP_CVTPD2DQ | OCERZ_OP_CVTTPD2DQ | OCERZ_OP_CVTPS2PD | OCERZ_OP_CVTDQ2PD
            | OCERZ_OP_MOVMSKPS | OCERZ_OP_MOVMSKPD | OCERZ_OP_PMOVMSKB | OCERZ_OP_PTEST | OCERZ_OP_PMOVZXBW
            | OCERZ_OP_PMOVZXBD | OCERZ_OP_PMOVZXBQ | OCERZ_OP_PMOVZXWD | OCERZ_OP_PMOVZXWQ | OCERZ_OP_PMOVZXDQ
            | OCERZ_OP_PMOVSXBW | OCERZ_OP_PMOVSXBD | OCERZ_OP_PMOVSXBQ | OCERZ_OP_PMOVSXWD | OCERZ_OP_PMOVSXWQ
            | OCERZ_OP_PMOVSXDQ => return do_avx256_cross(cpu, insn),
            _ => {}
        }
        let rc = sse_exec(cpu, insn);
        if rc != STEP_OK {
            return rc;
        }
        let mut hi = *insn;
        let mut regs = [0usize; 5];
        let mut nregs = 0usize;
        let shift = vex_is_shift(op);
        for i in 0..insn.nops as usize {
            if shift && i == 1 {
                continue;
            }
            let k = insn.ops[i].kind as u32;
            if k == OCERZ_OPK_XMM {
                let r = insn.ops[i].reg as usize;
                if !regs[..nregs].contains(&r) {
                    regs[nregs] = r;
                    nregs += 1;
                }
            } else if k == OCERZ_OPK_MEM {
                hi.ops[i].disp = hi.ops[i].disp.wrapping_add(16);
            }
        }
        if (insn.vex as u32 & OCERZ_VEX_NDS) != 0 {
            let r = insn.vvvv as usize;
            if !regs[..nregs].contains(&r) {
                regs[nregs] = r;
                nregs += 1;
            }
        }
        let imm = insn.ops[2].imm;
        match op {
            OCERZ_OP_BLENDPS => hi.ops[2].imm = (imm >> 4) & 0xf,
            OCERZ_OP_BLENDPD => hi.ops[2].imm = (imm >> 2) & 0x3,
            OCERZ_OP_SHUFPD => hi.ops[2].imm = (imm >> 2) & 0x3,
            OCERZ_OP_MPSADBW => hi.ops[2].imm = (imm >> 3) & 0x7,
            _ => {}
        }
        for &r in &regs[..nregs] {
            core::mem::swap(&mut cpu.xmm[r], &mut cpu.ymmh[r]);
        }
        let rc = sse_exec(cpu, &hi);
        for &r in &regs[..nregs] {
            core::mem::swap(&mut cpu.xmm[r], &mut cpu.ymmh[r]);
        }
        cpu.ymmh_all_zero = 0;
        rc
    }
}

//! The x87 unit, interpreted.
//!
//! ---- registers ----
//! The eight data registers are IEEE doubles in OcerzCPU.fpr, indexed by
//! physical register; ST(i) is fpr[(ftop + i) & 7].  The real registers are
//! 80-bit extended, and so are m80 operands and the FNSAVE and FXSAVE images,
//! but a double keeps x87 arithmetic on the host's floating-point unit, and
//! rosettax87_jit, which 32-bit games ran on before, makes the same choice.
//! What a double cannot hold is a value loaded from memory as more than a
//! double: a 64-bit integer, which is not a matter of precision at all -
//! Delphi's and Free Pascal's Move() copy eight bytes at a time as fild qword /
//! fistp qword, so a rounded integer is corrupted memory (OMSI 2 died reading
//! its own form resource) - and an 80-bit value moved with fld tbyte / fstp
//! tbyte, which is how compilers copy a long double.  Each register therefore
//! also has an exact 80-bit image in fpr_xm and fpr_xe, which counts while its
//! bit in fpr_x_ok is set AND the double is still, bit for bit, the image
//! rounded to double.  FILD, FBLD, the constant loads, an m80 load and the
//! FRSTOR and FXRSTOR images fill it; FLD ST(i), FST ST(i), FXCH and FCMOVcc
//! carry it; every other write clears the bit.  Integer stores, FBSTP, the
//! 80-bit images, FXAM and the tag word read it, so an integer, a long double, a
//! denormal or a NaN payload goes through loads, stores and context switches
//! unchanged.  The second condition keeps the image safe from a writer outside
//! this file that forgets the bit.
//!
//! ftw is the abridged tag word, bit p set while physical register p holds a
//! value, as FXSAVE stores it; FNSTENV and FNSAVE expand it to the two-bit form
//! from the register contents.  Pushes, pops, FFREE and FXCH keep it, which
//! FXAM's "empty" class needs.  A push onto a full register or a read of an
//! empty one is not a stack fault: the value passes through, since a tag that
//! went wrong somewhere outside the unit must never turn data into NaNs.  TOP is
//! ftop; the TOP field of fsw is ignored and synthesized whenever fsw is read.
//!
//! ---- arithmetic ----
//! Each operation runs on the host's floating-point unit with FPCR set from the
//! control word - rounding from RC, flush-to-zero off - whatever MXCSR asked of
//! SSE, and FPSR is saved around it, so the flags it raises go to fsw only and
//! x87 arithmetic never shows in MXCSR, as on x86.  Precision control 24, which
//! Direct3D 9 sets, rounds FADD, FSUB, FMUL, FDIV and FSQRT results to a
//! 24-bit significand without narrowing the exponent; 53 and 64 both leave the
//! double.  NaNs follow the x87 rules rather than arm64's or SSE's: an invalid
//! operation with no NaN operand gives the negative default NaN, a single NaN
//! operand comes back quieted, and of two NaNs a QNaN beats an SNaN and
//! otherwise the larger significand wins, the positive one on a tie, as Rosetta
//! does.  FCHS and FABS change the sign bit of
//! anything, NaNs included.  Unmasked exceptions do not trap, C1 does not say
//! which way a result was rounded, and the instruction and operand pointers in
//! the environment images are zero.
//!
//! ---- the rest ----
//! FIST, FISTP and FISTTP store the integer indefinite (0x8000, 0x80000000 or
//! 0x8000000000000000) for a NaN or a value out of range, with IE.  FPREM and
//! FPREM1 reduce in one step when the exponents differ by less than 64, with
//! C2 clear and the low three quotient bits in C0, C3 and C1, and otherwise
//! take a partial step and set C2, as the hardware does; the partial remainder
//! is fmod against the divisor scaled by a power of two, which is exact.  FSIN,
//! FCOS, FPTAN and FSINCOS leave an operand of magnitude 2^63 or more alone and
//! set C2.  The control word's bit 6 always reads as one.  Where Rosetta and the
//! SDM part, Rosetta wins, since it is what these programs ran on: FNSTENV
//! leaves the exception masks alone, and the pad words of the environment
//! images are zero.
//!
//! ---- the translated forms ----
//! src/jit.c translates the common forms with this file as their
//! specification.  A translated instruction leaves every value, image, image
//! bit, tag, TOP and fsw bit that this file would, exception flags included -
//! it computes them rather than reading FPSR - and comes here for whatever it
//! cannot reproduce: NaNs, infinite results, tiny products and quotients,
//! rounding other than to nearest, integer stores out of range or from a valid
//! image.  tests/diff32.c compares the two bit for bit, so a change to the
//! semantics here is a change there.
//!
//! In Rust the C file's `#pragma STDC FENV_ACCESS ON` has no equivalent, so
//! every operation that must observe the FPCR set from the control word, or
//! raise FPSR flags inside the bracket, passes its operands and result through
//! `black_box`; that pins the computation between the `msr` instructions and
//! keeps LLVM from constant-folding it under round-to-nearest.  The
//! mode-dependent roundings (`rint`, `nearbyint`) are the single arm64
//! instructions they compile to in C.
//! The four basic arithmetic operations are inline `fadd`/`fsub`/`fmul`/`fdiv`
//! rather than a `match` over Rust operators: LLVM treats float arithmetic as
//! side-effect free and would otherwise compute every arm and select one,
//! raising the overflow and inexact flags of operations the guest never ran.

use core::arch::asm;
use core::hint::black_box;

use crate::ffi::*;
use crate::inline::{OCERZ_AF, OCERZ_CF, OCERZ_OF, OCERZ_PF, OCERZ_SF, OCERZ_ZF, ocerz_sext};
use crate::interp_common::{ocerz_ea, ocerz_g2h, ocerz_ld, ocerz_st, ocerz_write_gpr};
use crate::ported::flags::ocerz_cc_eval;

const X87_IE: u16 = 0x0001;
const X87_ZE: u16 = 0x0004;
const X87_OE: u16 = 0x0008;
const X87_UE: u16 = 0x0010;
const X87_PE: u16 = 0x0020;
const X87_FLAGS: u16 = X87_IE | X87_ZE | X87_OE | X87_UE | X87_PE;
const X87_C0: u16 = 0x0100;
const X87_C1: u16 = 0x0200;
const X87_C2: u16 = 0x0400;
const X87_C3: u16 = 0x4000;
const X87_CC: u16 = X87_C0 | X87_C1 | X87_C2 | X87_C3;

const DBL_SIGN: u64 = 0x8000000000000000;
const DBL_EXP: u64 = 0x7ff0000000000000;
const DBL_FRAC: u64 = 0x000fffffffffffff;
const DBL_QUIET: u64 = 0x0008000000000000;
const X87_DEFAULT_NAN: u64 = 0xfff8000000000000;

const STEP_OK: i32 = OCERZ_STEP_OK as i32;
const EUNSUP: i32 = OCERZ_EUNSUP as i32;

unsafe extern "C" {
    fn ilogb(x: f64) -> i32;
    fn ldexp(x: f64, e: i32) -> f64;
    fn scalbn(x: f64, e: i32) -> f64;
    fn fmod(x: f64, y: f64) -> f64;
    fn remquo(x: f64, y: f64, q: *mut i32) -> f64;
    fn expm1(x: f64) -> f64;
    fn log2(x: f64) -> f64;
    fn log1p(x: f64) -> f64;
    fn atan2(y: f64, x: f64) -> f64;
    fn sin(x: f64) -> f64;
    fn cos(x: f64) -> f64;
    fn tan(x: f64) -> f64;
}

#[inline(always)]
fn dbits(d: f64) -> u64 {
    d.to_bits()
}

#[inline(always)]
fn bitsd(u: u64) -> f64 {
    f64::from_bits(u)
}

#[inline(always)]
fn is_nan_bits(u: u64) -> bool {
    (u & DBL_EXP) == DBL_EXP && (u & DBL_FRAC) != 0
}

#[inline(always)]
fn is_snan(d: f64) -> bool {
    let u = dbits(d);
    is_nan_bits(u) && (u & DBL_QUIET) == 0
}

#[inline(always)]
fn quieted(d: f64) -> f64 {
    bitsd(dbits(d) | DBL_QUIET)
}

fn nan1(a: f64) -> f64 {
    if a.is_nan() { quieted(a) } else { bitsd(X87_DEFAULT_NAN) }
}

fn nan2(a: f64, b: f64) -> f64 {
    let an = a.is_nan();
    let bn = b.is_nan();
    if an && bn {
        let as_ = is_snan(a);
        let bs = is_snan(b);
        if as_ != bs {
            return quieted(if as_ { b } else { a });
        }
        let fa = dbits(a) & DBL_FRAC;
        let fb = dbits(b) & DBL_FRAC;
        if fa == fb {
            return quieted(if a.is_sign_negative() { b } else { a });
        }
        return quieted(if fb > fa { b } else { a });
    }
    if an {
        return quieted(a);
    }
    if bn {
        return quieted(b);
    }
    bitsd(X87_DEFAULT_NAN)
}

#[derive(Clone, Copy)]
struct X87Env {
    fpcr: u64,
    fpsr: u64,
    want: u64,
}

const FPCR_FIZ: u64 = 0x1;
const FPCR_RMODE: u64 = 3 << 22;
const FPCR_FZ: u64 = 1 << 24;
const FPCR_DN: u64 = 1 << 25;

#[inline(always)]
fn rd_fpcr() -> u64 {
    let v: u64;
    unsafe { asm!("mrs {}, fpcr", out(reg) v, options(nostack)) };
    v
}

#[inline(always)]
fn rd_fpsr() -> u64 {
    let v: u64;
    unsafe { asm!("mrs {}, fpsr", out(reg) v, options(nostack)) };
    v
}

#[inline(always)]
fn wr_fpcr(v: u64) {
    unsafe { asm!("msr fpcr, {}", in(reg) v, options(nostack)) };
}

#[inline(always)]
fn wr_fpsr(v: u64) {
    unsafe { asm!("msr fpsr, {}", in(reg) v, options(nostack)) };
}

#[inline(always)]
fn env_enter(cpu: &OcerzCPU) -> X87Env {
    const RMODE: [u64; 4] = [0, 2 << 22, 1 << 22, 3 << 22];
    let fpcr = rd_fpcr();
    let fpsr = rd_fpsr();
    let want = (fpcr & !(FPCR_RMODE | FPCR_FZ | FPCR_DN | FPCR_FIZ)) | RMODE[((cpu.fcw >> 10) & 3) as usize];
    if want != fpcr {
        wr_fpcr(want);
    }
    wr_fpsr(0);
    X87Env { fpcr, fpsr, want }
}

#[inline(always)]
fn env_leave(cpu: &mut OcerzCPU, e: X87Env, keep: u16) {
    let f = rd_fpsr();
    let mut x: u16 = 0;
    if f & 0x01 != 0 {
        x |= X87_IE;
    }
    if f & 0x02 != 0 {
        x |= X87_ZE;
    }
    if f & 0x04 != 0 {
        x |= X87_OE;
    }
    if f & 0x08 != 0 {
        x |= X87_UE;
    }
    if f & 0x10 != 0 {
        x |= X87_PE;
    }
    cpu.fsw |= x & keep;
    wr_fpsr(e.fpsr);
    if e.want != e.fpcr {
        wr_fpcr(e.fpcr);
    }
}

#[inline(always)]
fn frintx(v: f64) -> f64 {
    let r: f64;
    unsafe { asm!("frintx {:d}, {:d}", out(vreg) r, in(vreg) v, options(nomem, nostack)) };
    black_box(r)
}

#[inline(always)]
fn frinti(v: f64) -> f64 {
    let r: f64;
    unsafe { asm!("frinti {:d}, {:d}", out(vreg) r, in(vreg) v, options(nomem, nostack)) };
    black_box(r)
}

fn pc_round(cpu: &OcerzCPU, r: f64) -> f64 {
    if ((cpu.fcw >> 8) & 3) != 0 || r == 0.0 || !r.is_finite() {
        return r;
    }
    unsafe {
        let e = ilogb(r);
        let f = black_box(black_box(ldexp(r, -e)) as f32);
        ldexp(f as f64, e)
    }
}

fn f80_to_dbits(mant: u64, se: u32) -> u64 {
    let sign = ((se >> 15) as u64) << 63;
    let e = (se & 0x7fff) as i32;
    let mut mant = mant;
    if e == 0x7fff {
        if (mant << 1) == 0 {
            return sign | DBL_EXP;
        }
        let f = (mant >> 11) & DBL_FRAC;
        return sign | DBL_EXP | if f != 0 { f } else { 1 };
    }
    if mant == 0 {
        return sign;
    }
    let lz = mant.leading_zeros() as i32;
    mant <<= lz;
    let mut be = (if e != 0 { e } else { 1 }) - 16383 - lz + 1023;
    if be >= 0x7ff {
        return sign | DBL_EXP;
    }
    let mut shift = 11;
    if be <= 0 {
        shift += 1 - be;
        be = 0;
    }
    let (mut keep, rest);
    if shift >= 65 {
        keep = 0;
        rest = 1;
    } else if shift == 64 {
        keep = 0;
        rest = mant;
    } else {
        keep = mant >> shift;
        rest = mant << (64 - shift);
    }
    if rest > (1u64 << 63) || (rest == (1u64 << 63) && (keep & 1) != 0) {
        keep += 1;
    }
    let mut bits = if be != 0 { (((be - 1) as u64) << 52).wrapping_add(keep) } else { keep };
    if (bits & DBL_EXP) == DBL_EXP {
        bits = DBL_EXP;
    }
    sign | bits
}

fn int_to_f80(x: i64) -> (u64, u16) {
    if x == 0 {
        return (0, 0);
    }
    let m = if x < 0 { 0u64.wrapping_sub(x as u64) } else { x as u64 };
    let lz = m.leading_zeros();
    (m << lz, ((if x < 0 { 0x8000u32 } else { 0 }) | (16383 + 63 - lz)) as u16)
}

fn f80_to_int(mant: u64, se: u32, rc: i32, out: &mut i64, inexact: &mut i32) -> i32 {
    let neg = (se >> 15) & 1 != 0;
    let e = (se & 0x7fff) as i32;
    *inexact = 0;
    if e == 0x7fff {
        return 0;
    }
    if mant == 0 {
        *out = 0;
        return 1;
    }
    let shift = 16383 + 63 - if e != 0 { e } else { 1 };
    if shift < 0 {
        return 0;
    }
    let (mut ip, frac);
    if shift == 0 {
        ip = mant;
        frac = 0;
    } else if shift < 64 {
        ip = mant >> shift;
        frac = mant << (64 - shift);
    } else {
        ip = 0;
        frac = if shift == 64 { mant } else { 1 };
    }
    *inexact = (frac != 0) as i32;
    let mut up = false;
    if frac != 0 {
        match rc {
            0 => up = frac > (1u64 << 63) || (frac == (1u64 << 63) && (ip & 1) != 0),
            1 => up = neg,
            2 => up = !neg,
            _ => {}
        }
    }
    if up {
        ip = ip.wrapping_add(1);
        if ip == 0 {
            return 0;
        }
    }
    if neg {
        if ip > (1u64 << 63) {
            return 0;
        }
        *out = 0u64.wrapping_sub(ip) as i64;
    } else {
        if ip > i64::MAX as u64 {
            return 0;
        }
        *out = ip as i64;
    }
    1
}

fn f80_class(mant: u64, se: u32) -> u16 {
    let e = se & 0x7fff;
    if e == 0 {
        return if mant != 0 { X87_C3 | X87_C2 } else { X87_C3 };
    }
    if (mant >> 63) == 0 {
        return 0;
    }
    if e == 0x7fff {
        return if (mant << 1) != 0 { X87_C0 } else { X87_C2 | X87_C0 };
    }
    X87_C2
}

#[inline(always)]
fn phys(cpu: &OcerzCPU, i: i32) -> usize {
    ((cpu.ftop as i32 + i) & 7) as usize
}

#[inline(always)]
fn st(cpu: &OcerzCPU, i: i32) -> f64 {
    unsafe { *cpu.fpr.get_unchecked(phys(cpu, i)) }
}

#[inline(always)]
fn set_st(cpu: &mut OcerzCPU, i: i32, v: f64) {
    let p = phys(cpu, i);
    unsafe { *cpu.fpr.get_unchecked_mut(p) = v };
    cpu.fpr_x_ok &= !(1u8 << p);
    cpu.ftw |= 1u8 << p;
}

#[inline(always)]
fn image_ok(cpu: &OcerzCPU, p: usize) -> bool {
    ((cpu.fpr_x_ok >> p) & 1) != 0
        && dbits(cpu.fpr[p]) == f80_to_dbits(cpu.fpr_xm[p], cpu.fpr_xe[p] as u32)
}

#[inline(always)]
fn set_phys_image(cpu: &mut OcerzCPU, p: usize, mant: u64, se: u16) {
    cpu.fpr[p] = bitsd(f80_to_dbits(mant, se as u32));
    cpu.fpr_xm[p] = mant;
    cpu.fpr_xe[p] = se;
    cpu.fpr_x_ok |= 1u8 << p;
}

#[inline(always)]
fn push_slot(cpu: &mut OcerzCPU) {
    cpu.ftop = cpu.ftop.wrapping_sub(1) & 7;
    cpu.ftw |= 1u8 << cpu.ftop;
    cpu.fpr_x_ok &= !(1u8 << cpu.ftop);
}

#[inline(always)]
fn push_d(cpu: &mut OcerzCPU, v: f64) {
    push_slot(cpu);
    cpu.fpr[cpu.ftop as usize] = v;
}

#[inline(always)]
fn push_image(cpu: &mut OcerzCPU, mant: u64, se: u16) {
    push_slot(cpu);
    let p = cpu.ftop as usize;
    set_phys_image(cpu, p, mant, se);
}

#[inline(always)]
fn push_i(cpu: &mut OcerzCPU, x: i64) {
    let (mant, se) = int_to_f80(x);
    push_image(cpu, mant, se);
}

#[inline(always)]
fn pop(cpu: &mut OcerzCPU) {
    cpu.ftw &= !(1u8 << cpu.ftop);
    cpu.ftop = (cpu.ftop + 1) & 7;
}

fn copy_st(cpu: &mut OcerzCPU, dst: i32, src: i32) {
    let d = phys(cpu, dst);
    let s = phys(cpu, src);
    if d == s {
        return;
    }
    cpu.fpr[d] = cpu.fpr[s];
    cpu.fpr_xm[d] = cpu.fpr_xm[s];
    cpu.fpr_xe[d] = cpu.fpr_xe[s];
    cpu.fpr_x_ok = (cpu.fpr_x_ok & !(1u8 << d)) | (((cpu.fpr_x_ok >> s) & 1) << d);
    cpu.ftw |= 1u8 << d;
}

fn exchange(cpu: &mut OcerzCPU, i: i32) {
    let a = phys(cpu, 0);
    let b = phys(cpu, i);
    if a == b {
        return;
    }
    cpu.fpr.swap(a, b);
    cpu.fpr_xm.swap(a, b);
    cpu.fpr_xe.swap(a, b);
    let oa = (cpu.fpr_x_ok >> a) & 1;
    let ob = (cpu.fpr_x_ok >> b) & 1;
    cpu.fpr_x_ok = (cpu.fpr_x_ok & !((1u8 << a) | (1u8 << b))) | (ob << a) | (oa << b);
    cpu.ftw |= (1u8 << a) | (1u8 << b);
}

#[inline(always)]
fn fsw_of(cpu: &OcerzCPU) -> u16 {
    (cpu.fsw & !(7u16 << 11)) | (((cpu.ftop & 7) as u16) << 11)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_x87_fsw(cpu: *const OcerzCPU) -> u16 {
    unsafe { fsw_of(&*cpu) }
}

#[inline(always)]
fn set_cc(cpu: &mut OcerzCPU, cc: u16) {
    cpu.fsw = (cpu.fsw & !X87_CC) | cc;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_x87_push(cpu: *mut OcerzCPU, v: f64) {
    unsafe { push_d(&mut *cpu, v) }
}

fn reset(cpu: &mut OcerzCPU) {
    cpu.fcw = 0x037f;
    cpu.fsw = 0;
    cpu.ftw = 0;
    cpu.ftop = 0;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_x87_reset(cpu: *mut OcerzCPU) {
    unsafe { reset(&mut *cpu) }
}

fn to_f80(cpu: &OcerzCPU, p: usize) -> [u8; 10] {
    let mant: u64;
    let se: u32;
    if image_ok(cpu, p) {
        mant = cpu.fpr_xm[p];
        se = cpu.fpr_xe[p] as u32;
    } else {
        let u = dbits(cpu.fpr[p]);
        let sign = ((u >> 63) as u32) << 15;
        let e = ((u >> 52) & 0x7ff) as u32;
        let f = u & DBL_FRAC;
        if e == 0x7ff {
            mant = (1u64 << 63) | (f << 11);
            se = sign | 0x7fff;
        } else if e == 0 {
            if f == 0 {
                mant = 0;
                se = sign;
            } else {
                let lz = f.leading_zeros();
                mant = f << lz;
                se = sign | (15372 - lz);
            }
        } else {
            mant = (1u64 << 63) | (f << 11);
            se = sign | (e - 1023 + 16383);
        }
    }
    let mut out = [0u8; 10];
    out[..8].copy_from_slice(&mant.to_le_bytes());
    out[8] = se as u8;
    out[9] = (se >> 8) as u8;
    out
}

fn from_f80(cpu: &mut OcerzCPU, p: usize, b: &[u8; 10]) {
    let mant = u64::from_le_bytes([b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]]);
    set_phys_image(cpu, p, mant, (b[8] as u16) | ((b[9] as u16) << 8));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_x87_to_f80(cpu: *const OcerzCPU, p: i32, out: *mut u8) {
    unsafe {
        let b = to_f80(&*cpu, p as usize);
        core::ptr::copy_nonoverlapping(b.as_ptr(), out, 10);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_x87_from_f80(cpu: *mut OcerzCPU, p: i32, inp: *const u8) {
    unsafe {
        let b = core::ptr::read_unaligned(inp as *const [u8; 10]);
        from_f80(&mut *cpu, p as usize, &b);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn ocerz_x87_f80_dbits(mant: u64, se: u32) -> u64 {
    f80_to_dbits(mant, se)
}

#[inline(always)]
unsafe fn g2h_read10(ea: u64) -> [u8; 10] {
    unsafe { core::ptr::read_unaligned(ocerz_g2h(ea) as *const [u8; 10]) }
}

#[inline(always)]
unsafe fn g2h_write(ea: u64, b: &[u8]) {
    unsafe { core::ptr::copy_nonoverlapping(b.as_ptr(), ocerz_g2h(ea), b.len()) }
}

unsafe fn load_real(cpu: &mut OcerzCPU, insn: &X86Insn, op: &X86Operand) -> f64 {
    unsafe {
        let ea = ocerz_ea(cpu, insn, op);
        if op.size == 4 {
            let bits = ocerz_ld(ea, 4) as u32;
            if (bits & 0x7f800000) == 0x7f800000 && (bits & 0x007fffff) != 0 && (bits & 0x00400000) == 0 {
                cpu.fsw |= X87_IE;
            }
            let d = f32::from_bits(bits) as f64;
            return if d.is_nan() { quieted(d) } else { d };
        }
        let mut bits = ocerz_ld(ea, 8);
        if is_nan_bits(bits) && (bits & DBL_QUIET) == 0 {
            cpu.fsw |= X87_IE;
            bits |= DBL_QUIET;
        }
        bitsd(bits)
    }
}

unsafe fn load_int(cpu: &mut OcerzCPU, insn: &X86Insn, op: &X86Operand) -> i64 {
    unsafe { ocerz_sext(ocerz_ld(ocerz_ea(cpu, insn, op), op.size as i32), op.size as i32) }
}

unsafe fn store_real(cpu: &mut OcerzCPU, insn: &X86Insn, op: &X86Operand) {
    unsafe {
        let ea = ocerz_ea(cpu, insn, op);
        let p = phys(cpu, 0);
        let mut v = cpu.fpr[p];
        if op.size == 10 {
            let buf = to_f80(cpu, p);
            g2h_write(ea, &buf);
            return;
        }
        if is_snan(v) {
            cpu.fsw |= X87_IE;
            v = quieted(v);
        }
        if op.size == 8 {
            ocerz_st(ea, 8, dbits(v));
            return;
        }
        let e = env_enter(cpu);
        let f = black_box(black_box(v) as f32);
        env_leave(cpu, e, X87_FLAGS);
        ocerz_st(ea, 4, f.to_bits() as u64);
    }
}

fn round_rc(cpu: &mut OcerzCPU, v: f64) -> f64 {
    let e = env_enter(cpu);
    let r = frinti(black_box(v));
    env_leave(cpu, e, 0);
    r
}

fn st0_to_int(cpu: &mut OcerzCPU, rc: i32, out: &mut i64, inexact: &mut i32) -> i32 {
    let p = phys(cpu, 0);
    if image_ok(cpu, p) {
        return f80_to_int(cpu.fpr_xm[p], cpu.fpr_xe[p] as u32, rc, out, inexact);
    }
    let v = cpu.fpr[p];
    *inexact = 0;
    if v.is_nan() || v.is_infinite() {
        return 0;
    }
    let r = if rc == 3 { v.trunc() } else { round_rc(cpu, v) };
    if !(r >= -9223372036854775808.0 && r < 9223372036854775808.0) {
        return 0;
    }
    *inexact = (r != v) as i32;
    *out = r as i64;
    1
}

unsafe fn store_int(cpu: &mut OcerzCPU, insn: &X86Insn, op: &X86Operand, truncate: bool) {
    unsafe {
        let size = op.size as i32;
        let ea = ocerz_ea(cpu, insn, op);
        let mut x: i64 = 0;
        let mut inexact = 0;
        let rc = if truncate { 3 } else { ((cpu.fcw >> 10) & 3) as i32 };
        let mut ok = st0_to_int(cpu, rc, &mut x, &mut inexact) != 0;
        if ok && size < 8 {
            let lim = 1i64 << (size * 8 - 1);
            ok = x >= -lim && x < lim;
        }
        cpu.fsw &= !X87_C1;
        if !ok {
            x = (1u64 << (size * 8 - 1)) as i64;
            cpu.fsw |= X87_IE;
        } else if inexact != 0 {
            cpu.fsw |= X87_PE;
            let mut t: i64 = 0;
            let mut ti = 0;
            if !truncate && st0_to_int(cpu, 3, &mut t, &mut ti) != 0 && t != x {
                cpu.fsw |= X87_C1;
            }
        }
        ocerz_st(ea, size, x as u64);
    }
}

unsafe fn fbld(cpu: &mut OcerzCPU, ea: u64) {
    unsafe {
        let b = g2h_read10(ea);
        let mut x: i64 = 0;
        for i in (0..=8).rev() {
            x = x.wrapping_mul(100) + ((b[i] >> 4) as i64) * 10 + (b[i] & 15) as i64;
        }
        if b[9] & 0x80 != 0 {
            if x == 0 {
                push_image(cpu, 0, 0x8000);
            } else {
                push_i(cpu, x.wrapping_neg());
            }
        } else {
            push_i(cpu, x);
        }
    }
}

unsafe fn fbstp(cpu: &mut OcerzCPU, ea: u64) {
    unsafe {
        let mut x: i64 = 0;
        let mut inexact = 0;
        let neg = st(cpu, 0).is_sign_negative();
        let rc = ((cpu.fcw >> 10) & 3) as i32;
        let mut ok = st0_to_int(cpu, rc, &mut x, &mut inexact) != 0;
        let mut b = [0u8; 10];
        if ok && (x >= 1000000000000000000 || x <= -1000000000000000000) {
            ok = false;
        }
        if !ok {
            b = [0, 0, 0, 0, 0, 0, 0, 0xc0, 0xff, 0xff];
            cpu.fsw |= X87_IE;
        } else {
            if inexact != 0 {
                cpu.fsw |= X87_PE;
            }
            let mut m = if x < 0 { x.wrapping_neg() as u64 } else { x as u64 };
            for bi in b.iter_mut().take(9) {
                let lo = (m % 10) as u8;
                m /= 10;
                let hi = (m % 10) as u8;
                m /= 10;
                *bi = (hi << 4) | lo;
            }
            b[9] = if x < 0 || (x == 0 && neg) { 0x80 } else { 0 };
        }
        g2h_write(ea, &b);
        pop(cpu);
    }
}

const X87_ADD: i32 = 0;
const X87_SUB: i32 = 1;
const X87_MUL: i32 = 2;
const X87_DIV: i32 = 3;

fn binop(cpu: &mut OcerzCPU, kind: i32, a: f64, b: f64) -> f64 {
    let e = env_enter(cpu);
    let r: f64;
    unsafe {
        match kind {
            X87_ADD => asm!("fadd {:d}, {:d}, {:d}", out(vreg) r, in(vreg) a, in(vreg) b, options(nomem, nostack)),
            X87_SUB => asm!("fsub {:d}, {:d}, {:d}", out(vreg) r, in(vreg) a, in(vreg) b, options(nomem, nostack)),
            X87_MUL => asm!("fmul {:d}, {:d}, {:d}", out(vreg) r, in(vreg) a, in(vreg) b, options(nomem, nostack)),
            _ => asm!("fdiv {:d}, {:d}, {:d}", out(vreg) r, in(vreg) a, in(vreg) b, options(nomem, nostack)),
        }
    }
    let out = black_box(pc_round(cpu, r));
    env_leave(cpu, e, X87_FLAGS);
    if out.is_nan() { nan2(a, b) } else { out }
}

unsafe fn arith(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let (mut rev, mut popit, mut intform) = (false, false, false);
        let kind = match insn.op as u32 {
            OCERZ_OP_FADD => X87_ADD,
            OCERZ_OP_FADDP => {
                popit = true;
                X87_ADD
            }
            OCERZ_OP_FIADD => {
                intform = true;
                X87_ADD
            }
            OCERZ_OP_FSUB => X87_SUB,
            OCERZ_OP_FSUBP => {
                popit = true;
                X87_SUB
            }
            OCERZ_OP_FISUB => {
                intform = true;
                X87_SUB
            }
            OCERZ_OP_FSUBR => {
                rev = true;
                X87_SUB
            }
            OCERZ_OP_FSUBRP => {
                rev = true;
                popit = true;
                X87_SUB
            }
            OCERZ_OP_FISUBR => {
                rev = true;
                intform = true;
                X87_SUB
            }
            OCERZ_OP_FMUL => X87_MUL,
            OCERZ_OP_FMULP => {
                popit = true;
                X87_MUL
            }
            OCERZ_OP_FIMUL => {
                intform = true;
                X87_MUL
            }
            OCERZ_OP_FDIV => X87_DIV,
            OCERZ_OP_FDIVP => {
                popit = true;
                X87_DIV
            }
            OCERZ_OP_FIDIV => {
                intform = true;
                X87_DIV
            }
            OCERZ_OP_FDIVR => {
                rev = true;
                X87_DIV
            }
            OCERZ_OP_FDIVRP => {
                rev = true;
                popit = true;
                X87_DIV
            }
            OCERZ_OP_FIDIVR => {
                rev = true;
                intform = true;
                X87_DIV
            }
            _ => return EUNSUP,
        };

        let mut dst = 0;
        let a;
        let b;
        if insn.nops >= 1 && insn.ops[0].kind as u32 != OCERZ_OPK_ST {
            a = st(cpu, 0);
            b = if intform {
                load_int(cpu, insn, &insn.ops[0]) as f64
            } else {
                load_real(cpu, insn, &insn.ops[0])
            };
        } else if insn.nops >= 2 {
            dst = insn.ops[0].reg as i32;
            a = st(cpu, dst);
            b = st(cpu, insn.ops[1].reg as i32);
        } else {
            dst = popit as i32;
            a = st(cpu, dst);
            b = st(cpu, if popit { 0 } else { 1 });
        }
        let r = if rev { binop(cpu, kind, b, a) } else { binop(cpu, kind, a, b) };
        set_st(cpu, dst, r);
        if popit {
            pop(cpu);
        }
        STEP_OK
    }
}

fn fcom(cpu: &mut OcerzCPU, a: f64, b: f64, quiet: bool) {
    if a.is_nan() || b.is_nan() {
        set_cc(cpu, X87_C0 | X87_C2 | X87_C3);
        if !quiet || is_snan(a) || is_snan(b) {
            cpu.fsw |= X87_IE;
        }
    } else if a < b {
        set_cc(cpu, X87_C0);
    } else if a == b {
        set_cc(cpu, X87_C3);
    } else {
        set_cc(cpu, 0);
    }
}

fn fcomi(cpu: &mut OcerzCPU, a: f64, b: f64, quiet: bool) {
    cpu.rflags &= !(OCERZ_OF | OCERZ_AF | OCERZ_SF | OCERZ_ZF | OCERZ_PF | OCERZ_CF);
    cpu.fsw &= !X87_C1;
    if a.is_nan() || b.is_nan() {
        cpu.rflags |= OCERZ_ZF | OCERZ_PF | OCERZ_CF;
        if !quiet || is_snan(a) || is_snan(b) {
            cpu.fsw |= X87_IE;
        }
    } else if a < b {
        cpu.rflags |= OCERZ_CF;
    } else if a == b {
        cpu.rflags |= OCERZ_ZF;
    }
}

fn st_operand(insn: &X86Insn) -> i32 {
    if insn.nops >= 2 {
        return insn.ops[1].reg as i32;
    }
    if insn.nops == 1 && insn.ops[0].kind as u32 == OCERZ_OPK_ST {
        return insn.ops[0].reg as i32;
    }
    1
}

unsafe fn compare(cpu: &mut OcerzCPU, insn: &X86Insn) -> i32 {
    unsafe {
        let op = insn.op as u32;
        let a = st(cpu, 0);
        let mem = insn.nops >= 1 && insn.ops[0].kind as u32 != OCERZ_OPK_ST;
        match op {
            OCERZ_OP_FCOM | OCERZ_OP_FCOMP | OCERZ_OP_FUCOM | OCERZ_OP_FUCOMP => {
                let b = if mem { load_real(cpu, insn, &insn.ops[0]) } else { st(cpu, st_operand(insn)) };
                fcom(cpu, a, b, op == OCERZ_OP_FUCOM || op == OCERZ_OP_FUCOMP);
                if op == OCERZ_OP_FCOMP || op == OCERZ_OP_FUCOMP {
                    pop(cpu);
                }
                STEP_OK
            }
            OCERZ_OP_FICOM | OCERZ_OP_FICOMP => {
                let b = load_int(cpu, insn, &insn.ops[0]) as f64;
                fcom(cpu, a, b, false);
                if op == OCERZ_OP_FICOMP {
                    pop(cpu);
                }
                STEP_OK
            }
            OCERZ_OP_FCOMPP | OCERZ_OP_FUCOMPP => {
                let b = st(cpu, 1);
                fcom(cpu, a, b, op == OCERZ_OP_FUCOMPP);
                pop(cpu);
                pop(cpu);
                STEP_OK
            }
            OCERZ_OP_FTST => {
                fcom(cpu, a, 0.0, false);
                STEP_OK
            }
            OCERZ_OP_FCOMI | OCERZ_OP_FCOMIP | OCERZ_OP_FUCOMI | OCERZ_OP_FUCOMIP => {
                let b = st(cpu, st_operand(insn));
                fcomi(cpu, a, b, op == OCERZ_OP_FUCOMI || op == OCERZ_OP_FUCOMIP);
                if op == OCERZ_OP_FCOMIP || op == OCERZ_OP_FUCOMIP {
                    pop(cpu);
                }
                STEP_OK
            }
            _ => EUNSUP,
        }
    }
}

fn fxam(cpu: &mut OcerzCPU) {
    let p = phys(cpu, 0);
    let v = cpu.fpr[p];
    let mut cc = if v.is_sign_negative() { X87_C1 } else { 0 };
    if ((cpu.ftw >> p) & 1) == 0 {
        cc |= X87_C3 | X87_C0;
    } else if image_ok(cpu, p) {
        cc |= f80_class(cpu.fpr_xm[p], cpu.fpr_xe[p] as u32);
    } else if v.is_nan() {
        cc |= X87_C0;
    } else if v.is_infinite() {
        cc |= X87_C2 | X87_C0;
    } else if v == 0.0 {
        cc |= X87_C3;
    } else {
        cc |= X87_C2;
    }
    set_cc(cpu, cc);
}

fn out_of_range(cpu: &mut OcerzCPU, v: f64) -> bool {
    if v.is_finite() && v.abs() >= 9223372036854775808.0 {
        cpu.fsw = (cpu.fsw & !X87_CC) | X87_C2;
        return true;
    }
    cpu.fsw &= !(X87_C2 | X87_C1);
    false
}

fn fprem(cpu: &mut OcerzCPU, ieee: bool) {
    let x = st(cpu, 0);
    let y = st(cpu, 1);
    let r;
    let mut q: u32 = 0;
    unsafe {
        if x.is_nan() || y.is_nan() {
            r = nan2(x, y);
            if is_snan(x) || is_snan(y) {
                cpu.fsw |= X87_IE;
            }
        } else if x.is_infinite() || y == 0.0 {
            r = bitsd(X87_DEFAULT_NAN);
            cpu.fsw |= X87_IE;
        } else if y.is_infinite() || x == 0.0 {
            r = x;
        } else if ilogb(x) - ilogb(y) >= 64 {
            let r = fmod(x, ldexp(y, ilogb(x) - ilogb(y) - 32));
            set_st(cpu, 0, r);
            set_cc(cpu, X87_C2);
            return;
        } else {
            let mut quo: i32 = 0;
            let rn = remquo(x, y, &mut quo);
            let qn = (if quo < 0 { -quo } else { quo }) as u32;
            if ieee {
                r = rn;
                q = qn;
            } else {
                r = fmod(x, y);
                q = if r == rn { qn } else { qn.wrapping_sub(1) };
            }
        }
    }
    set_st(cpu, 0, r);
    let mut cc = 0;
    if q & 4 != 0 {
        cc |= X87_C0;
    }
    if q & 2 != 0 {
        cc |= X87_C3;
    }
    if q & 1 != 0 {
        cc |= X87_C1;
    }
    set_cc(cpu, cc);
}

fn fxtract(cpu: &mut OcerzCPU) {
    let v = st(cpu, 0);
    let ex;
    let sig;
    if v.is_nan() {
        ex = quieted(v);
        sig = ex;
        if is_snan(v) {
            cpu.fsw |= X87_IE;
        }
    } else if v.is_infinite() {
        ex = f64::INFINITY;
        sig = v;
    } else if v == 0.0 {
        ex = f64::NEG_INFINITY;
        sig = v;
        cpu.fsw |= X87_ZE;
    } else {
        let e = unsafe { ilogb(v) };
        ex = e as f64;
        sig = unsafe { scalbn(v, -e) };
    }
    set_st(cpu, 0, ex);
    push_d(cpu, sig);
}

fn fscale(cpu: &mut OcerzCPU) {
    let x = st(cpu, 0);
    let n = st(cpu, 1);
    let mut r;
    if x.is_nan() || n.is_nan() {
        r = nan2(x, n);
    } else if n.is_infinite() {
        if (n > 0.0 && x == 0.0) || (n < 0.0 && x.is_infinite()) {
            r = bitsd(X87_DEFAULT_NAN);
            cpu.fsw |= X87_IE;
        } else {
            r = if n > 0.0 {
                black_box(x) * f64::INFINITY
            } else if x.is_infinite() {
                x
            } else {
                0.0f64.copysign(x)
            };
            if x == 0.0 {
                r = x;
            }
        }
    } else {
        let mut t = n.trunc();
        if t > 100000.0 {
            t = 100000.0;
        }
        if t < -100000.0 {
            t = -100000.0;
        }
        let e = env_enter(cpu);
        let vr = black_box(unsafe { ldexp(black_box(x), t as i32) });
        env_leave(cpu, e, X87_OE | X87_UE | X87_PE);
        r = vr;
    }
    set_st(cpu, 0, r);
}

fn transcendental(cpu: &mut OcerzCPU, op: u32) -> i32 {
    let x = st(cpu, 0);
    unsafe {
        match op {
            OCERZ_OP_F2XM1 => {
                let e = env_enter(cpu);
                let r = black_box(expm1(black_box(x) * 0.69314718055994530942));
                env_leave(cpu, e, X87_UE | X87_PE);
                set_st(cpu, 0, if r.is_nan() { nan1(x) } else { r });
                STEP_OK
            }
            OCERZ_OP_FYL2X | OCERZ_OP_FYL2XP1 => {
                let y = st(cpu, 1);
                let e = env_enter(cpu);
                let bx = black_box(x);
                let l = if op == OCERZ_OP_FYL2X { log2(bx) } else { log1p(bx) * 1.44269504088896340736 };
                let r = black_box(black_box(y) * l);
                env_leave(cpu, e, X87_IE | X87_ZE | X87_OE | X87_UE | X87_PE);
                pop(cpu);
                set_st(cpu, 0, if r.is_nan() { nan2(x, y) } else { r });
                STEP_OK
            }
            OCERZ_OP_FPATAN => {
                let y = st(cpu, 1);
                let e = env_enter(cpu);
                let r = black_box(atan2(black_box(y), black_box(x)));
                env_leave(cpu, e, X87_UE | X87_PE);
                pop(cpu);
                set_st(cpu, 0, if r.is_nan() { nan2(x, y) } else { r });
                STEP_OK
            }
            OCERZ_OP_FSIN | OCERZ_OP_FCOS | OCERZ_OP_FPTAN | OCERZ_OP_FSINCOS => {
                if out_of_range(cpu, x) {
                    return STEP_OK;
                }
                if x.is_infinite() {
                    cpu.fsw |= X87_IE;
                }
                let e = env_enter(cpu);
                let bx = black_box(x);
                let mut s = 0.0;
                let mut c = 0.0;
                if op == OCERZ_OP_FSIN || op == OCERZ_OP_FSINCOS {
                    s = sin(bx);
                }
                if op == OCERZ_OP_FCOS || op == OCERZ_OP_FSINCOS {
                    c = cos(bx);
                }
                if op == OCERZ_OP_FPTAN {
                    s = tan(bx);
                }
                let s = black_box(s);
                let c = black_box(c);
                env_leave(cpu, e, X87_UE | X87_PE);
                if op == OCERZ_OP_FCOS {
                    set_st(cpu, 0, if c.is_nan() { nan1(x) } else { c });
                } else {
                    set_st(cpu, 0, if s.is_nan() { nan1(x) } else { s });
                    if op == OCERZ_OP_FPTAN {
                        push_d(cpu, 1.0);
                    } else if op == OCERZ_OP_FSINCOS {
                        push_d(cpu, if c.is_nan() { nan1(x) } else { c });
                    }
                }
                STEP_OK
            }
            _ => EUNSUP,
        }
    }
}

fn tag_of(cpu: &OcerzCPU, p: usize) -> u16 {
    if ((cpu.ftw >> p) & 1) == 0 {
        return 3;
    }
    if image_ok(cpu, p) {
        let c = f80_class(cpu.fpr_xm[p], cpu.fpr_xe[p] as u32);
        return if c == X87_C3 { 1 } else if c == X87_C2 { 0 } else { 2 };
    }
    let v = cpu.fpr[p];
    if v == 0.0 {
        return 1;
    }
    if !v.is_finite() {
        return 2;
    }
    0
}

fn full_tags(cpu: &OcerzCPU) -> u16 {
    let mut t: u16 = 0;
    for p in 0..8 {
        t |= tag_of(cpu, p) << (2 * p);
    }
    t
}

unsafe fn store_env(cpu: &mut OcerzCPU, ea: u64, image16: bool) {
    unsafe {
        if image16 {
            ocerz_st(ea, 2, cpu.fcw as u64);
            ocerz_st(ea + 2, 2, fsw_of(cpu) as u64);
            ocerz_st(ea + 4, 2, full_tags(cpu) as u64);
            let mut i = 6;
            while i < 14 {
                ocerz_st(ea + i, 2, 0);
                i += 2;
            }
            return;
        }
        ocerz_st(ea, 4, cpu.fcw as u64);
        ocerz_st(ea + 4, 4, fsw_of(cpu) as u64);
        ocerz_st(ea + 8, 4, full_tags(cpu) as u64);
        ocerz_st(ea + 12, 4, 0);
        ocerz_st(ea + 16, 4, 0);
        ocerz_st(ea + 20, 4, 0);
        ocerz_st(ea + 24, 4, 0);
    }
}

unsafe fn load_env(cpu: &mut OcerzCPU, ea: u64, image16: bool) {
    unsafe {
        let step: u64 = if image16 { 2 } else { 4 };
        cpu.fcw = (ocerz_ld(ea, 2) | 0x40) as u16;
        let sw = ocerz_ld(ea + step, 2) as u16;
        let tw = ocerz_ld(ea + 2 * step, 2) as u16;
        cpu.fsw = sw;
        cpu.ftop = ((sw >> 11) & 7) as u8;
        let mut abridged: u8 = 0;
        for p in 0..8 {
            if ((tw >> (2 * p)) & 3) != 3 {
                abridged |= 1u8 << p;
            }
        }
        cpu.ftw = abridged;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_x87_fxsave(cpu: *const OcerzCPU, ea: u64) {
    unsafe {
        let cpu = &*cpu;
        ocerz_st(ea, 2, cpu.fcw as u64);
        ocerz_st(ea + 2, 2, fsw_of(cpu) as u64);
        ocerz_st(ea + 4, 1, cpu.ftw as u64);
        ocerz_st(ea + 5, 1, 0);
        ocerz_st(ea + 6, 2, 0);
        ocerz_st(ea + 8, 8, 0);
        ocerz_st(ea + 16, 8, 0);
        for i in 0..8 {
            let mut buf = [0u8; 16];
            buf[..10].copy_from_slice(&to_f80(cpu, phys(cpu, i)));
            g2h_write(ea + 32 + (i as u64) * 16, &buf);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_x87_fxrstor(cpu: *mut OcerzCPU, ea: u64) {
    unsafe {
        let cpu = &mut *cpu;
        cpu.fcw = (ocerz_ld(ea, 2) | 0x40) as u16;
        cpu.fsw = ocerz_ld(ea + 2, 2) as u16;
        cpu.ftop = ((cpu.fsw >> 11) & 7) as u8;
        cpu.ftw = ocerz_ld(ea + 4, 1) as u8;
        for i in 0..8 {
            let buf = g2h_read10(ea + 32 + (i as u64) * 16);
            let p = phys(cpu, i);
            from_f80(cpu, p, &buf);
        }
    }
}

unsafe fn fnsave(cpu: &mut OcerzCPU, ea: u64, image16: bool) {
    unsafe {
        store_env(cpu, ea, image16);
        let regs = ea + if image16 { 14 } else { 28 };
        for i in 0..8 {
            let buf = to_f80(cpu, phys(cpu, i));
            g2h_write(regs + (i as u64) * 10, &buf);
        }
        reset(cpu);
    }
}

unsafe fn frstor(cpu: &mut OcerzCPU, ea: u64, image16: bool) {
    unsafe {
        load_env(cpu, ea, image16);
        let regs = ea + if image16 { 14 } else { 28 };
        for i in 0..8 {
            let buf = g2h_read10(regs + (i as u64) * 10);
            let p = phys(cpu, i);
            from_f80(cpu, p, &buf);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_x87_exec(cpu: *mut OcerzCPU, insn: *const X86Insn) -> i32 {
    unsafe {
        let cpu = &mut *cpu;
        let insn = &*insn;
        let op = insn.op as u32;
        let o = &insn.ops[0];
        let okind = o.kind as u32;

        match op {
            OCERZ_OP_FLD => {
                if okind == OCERZ_OPK_ST {
                    let src = o.reg as i32;
                    push_slot(cpu);
                    copy_st(cpu, 0, src + 1);
                } else if o.size == 10 {
                    let buf = g2h_read10(ocerz_ea(cpu, insn, o));
                    push_slot(cpu);
                    let p = cpu.ftop as usize;
                    from_f80(cpu, p, &buf);
                } else {
                    let v = load_real(cpu, insn, o);
                    push_d(cpu, v);
                }
                STEP_OK
            }
            OCERZ_OP_FILD => {
                let v = load_int(cpu, insn, o);
                push_i(cpu, v);
                STEP_OK
            }
            OCERZ_OP_FST | OCERZ_OP_FSTP => {
                if okind == OCERZ_OPK_ST {
                    copy_st(cpu, o.reg as i32, 0);
                } else {
                    store_real(cpu, insn, o);
                }
                if op == OCERZ_OP_FSTP {
                    pop(cpu);
                }
                STEP_OK
            }
            OCERZ_OP_FIST => {
                store_int(cpu, insn, o, false);
                STEP_OK
            }
            OCERZ_OP_FISTP => {
                store_int(cpu, insn, o, false);
                pop(cpu);
                STEP_OK
            }
            OCERZ_OP_FISTTP => {
                store_int(cpu, insn, o, true);
                pop(cpu);
                STEP_OK
            }
            OCERZ_OP_FBLD => {
                fbld(cpu, ocerz_ea(cpu, insn, o));
                STEP_OK
            }
            OCERZ_OP_FBSTP => {
                fbstp(cpu, ocerz_ea(cpu, insn, o));
                STEP_OK
            }
            OCERZ_OP_FLDCW => {
                cpu.fcw = (ocerz_ld(ocerz_ea(cpu, insn, o), 2) | 0x40) as u16;
                STEP_OK
            }
            OCERZ_OP_FNSTCW => {
                ocerz_st(ocerz_ea(cpu, insn, o), 2, cpu.fcw as u64);
                STEP_OK
            }
            OCERZ_OP_FNSTSW => {
                if insn.nops > 0 && okind == OCERZ_OPK_REG {
                    let v = fsw_of(cpu) as u64;
                    ocerz_write_gpr(cpu, o.reg as u32, 2, 0, v);
                } else {
                    ocerz_st(ocerz_ea(cpu, insn, o), 2, fsw_of(cpu) as u64);
                }
                STEP_OK
            }
            OCERZ_OP_FNSTENV => {
                store_env(cpu, ocerz_ea(cpu, insn, o), insn.opsize == 14);
                STEP_OK
            }
            OCERZ_OP_FLDENV => {
                load_env(cpu, ocerz_ea(cpu, insn, o), insn.opsize == 14);
                STEP_OK
            }
            OCERZ_OP_FNSAVE => {
                fnsave(cpu, ocerz_ea(cpu, insn, o), insn.opsize == 94);
                STEP_OK
            }
            OCERZ_OP_FRSTOR => {
                frstor(cpu, ocerz_ea(cpu, insn, o), insn.opsize == 94);
                STEP_OK
            }
            OCERZ_OP_FXCH => {
                exchange(cpu, if insn.nops > 0 && okind == OCERZ_OPK_ST { o.reg as i32 } else { 1 });
                cpu.fsw &= !X87_C1;
                STEP_OK
            }
            OCERZ_OP_FCHS => {
                let v = bitsd(dbits(st(cpu, 0)) ^ DBL_SIGN);
                set_st(cpu, 0, v);
                cpu.fsw &= !X87_C1;
                STEP_OK
            }
            OCERZ_OP_FABS => {
                let v = bitsd(dbits(st(cpu, 0)) & !DBL_SIGN);
                set_st(cpu, 0, v);
                cpu.fsw &= !X87_C1;
                STEP_OK
            }
            OCERZ_OP_FSQRT => {
                let x = st(cpu, 0);
                let e = env_enter(cpu);
                let vx = black_box(x);
                let r = black_box(pc_round(cpu, black_box(vx.sqrt())));
                env_leave(cpu, e, X87_FLAGS);
                set_st(cpu, 0, if r.is_nan() { nan1(x) } else { r });
                STEP_OK
            }
            OCERZ_OP_FRNDINT => {
                let x = st(cpu, 0);
                let e = env_enter(cpu);
                let r = frintx(black_box(x));
                env_leave(cpu, e, X87_IE | X87_PE);
                set_st(cpu, 0, if r.is_nan() { nan1(x) } else { r });
                STEP_OK
            }
            OCERZ_OP_FLDZ => {
                push_i(cpu, 0);
                STEP_OK
            }
            OCERZ_OP_FLD1 => {
                push_i(cpu, 1);
                STEP_OK
            }
            OCERZ_OP_FLDPI => {
                push_image(cpu, 0xc90fdaa22168c235, 0x4000);
                STEP_OK
            }
            OCERZ_OP_FLDL2E => {
                push_image(cpu, 0xb8aa3b295c17f0bc, 0x3fff);
                STEP_OK
            }
            OCERZ_OP_FLDL2T => {
                push_image(cpu, 0xd49a784bcd1b8afe, 0x4000);
                STEP_OK
            }
            OCERZ_OP_FLDLG2 => {
                push_image(cpu, 0x9a209a84fbcff799, 0x3ffd);
                STEP_OK
            }
            OCERZ_OP_FLDLN2 => {
                push_image(cpu, 0xb17217f7d1cf79ac, 0x3ffe);
                STEP_OK
            }
            OCERZ_OP_F2XM1 | OCERZ_OP_FYL2X | OCERZ_OP_FYL2XP1 | OCERZ_OP_FPATAN | OCERZ_OP_FSIN
            | OCERZ_OP_FCOS | OCERZ_OP_FPTAN | OCERZ_OP_FSINCOS => transcendental(cpu, op),
            OCERZ_OP_FPREM => {
                fprem(cpu, false);
                STEP_OK
            }
            OCERZ_OP_FPREM1 => {
                fprem(cpu, true);
                STEP_OK
            }
            OCERZ_OP_FSCALE => {
                fscale(cpu);
                STEP_OK
            }
            OCERZ_OP_FXTRACT => {
                fxtract(cpu);
                STEP_OK
            }
            OCERZ_OP_FXAM => {
                fxam(cpu);
                STEP_OK
            }
            OCERZ_OP_FCMOVCC => {
                if ocerz_cc_eval(cpu, insn.cc as u32) != 0 {
                    copy_st(cpu, 0, st_operand(insn));
                }
                STEP_OK
            }
            OCERZ_OP_FNINIT => {
                reset(cpu);
                STEP_OK
            }
            OCERZ_OP_FNCLEX => {
                cpu.fsw &= !0x80ffu16;
                STEP_OK
            }
            OCERZ_OP_FFREE => {
                cpu.ftw &= !(1u8 << phys(cpu, o.reg as i32));
                STEP_OK
            }
            OCERZ_OP_FFREEP => {
                cpu.ftw &= !(1u8 << phys(cpu, o.reg as i32));
                pop(cpu);
                STEP_OK
            }
            OCERZ_OP_FINCSTP => {
                cpu.ftop = (cpu.ftop + 1) & 7;
                cpu.fsw &= !X87_C1;
                STEP_OK
            }
            OCERZ_OP_FDECSTP => {
                cpu.ftop = cpu.ftop.wrapping_sub(1) & 7;
                cpu.fsw &= !X87_C1;
                STEP_OK
            }
            OCERZ_OP_FWAIT => STEP_OK,
            OCERZ_OP_FADD | OCERZ_OP_FADDP | OCERZ_OP_FIADD | OCERZ_OP_FSUB | OCERZ_OP_FSUBP
            | OCERZ_OP_FISUB | OCERZ_OP_FSUBR | OCERZ_OP_FSUBRP | OCERZ_OP_FISUBR | OCERZ_OP_FMUL
            | OCERZ_OP_FMULP | OCERZ_OP_FIMUL | OCERZ_OP_FDIV | OCERZ_OP_FDIVP | OCERZ_OP_FIDIV
            | OCERZ_OP_FDIVR | OCERZ_OP_FDIVRP | OCERZ_OP_FIDIVR => arith(cpu, insn),
            OCERZ_OP_FCOM | OCERZ_OP_FCOMP | OCERZ_OP_FCOMPP | OCERZ_OP_FUCOM | OCERZ_OP_FUCOMP
            | OCERZ_OP_FUCOMPP | OCERZ_OP_FICOM | OCERZ_OP_FICOMP | OCERZ_OP_FTST | OCERZ_OP_FCOMI
            | OCERZ_OP_FCOMIP | OCERZ_OP_FUCOMI | OCERZ_OP_FUCOMIP => compare(cpu, insn),
            _ => EUNSUP,
        }
    }
}

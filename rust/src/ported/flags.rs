//! Eager RFLAGS computation: the bit-for-bit x86 flag reference the JIT's
//! deferred-record paths must agree with, and the interpreter's flag source.
//!
//! Where the architecture says "undefined" this file follows Rosetta, since
//! Rosetta is the golden oracle the guest tests are generated against.  MUL and
//! IMUL define CF and OF only - SF, ZF, AF and PF are architecturally
//! undefined, and Rosetta leaves all four clear regardless of the result or
//! the flags going in (probed 2026-09-03), so that is what is produced here.

use core::ffi::{c_int, c_uint};

use crate::ffi::{
    OCERZ_CC_ADD, OCERZ_CC_DEC, OCERZ_CC_IMUL, OCERZ_CC_INC, OCERZ_CC_LOGIC, OCERZ_CC_MUL,
    OCERZ_CC_NONE, OCERZ_CC_SAR, OCERZ_CC_SHL, OCERZ_CC_SHR, OCERZ_CC_SUB, OcerzCPU,
};
use crate::inline::{
    OCERZ_AF, OCERZ_CF, OCERZ_OF, OCERZ_PF, OCERZ_SF, OCERZ_ZF, ocerz_flag_assign, ocerz_mask,
    ocerz_msb, ocerz_sext,
};

const OCERZ_ARITH_FLAGS: u64 = OCERZ_CF | OCERZ_PF | OCERZ_AF | OCERZ_ZF | OCERZ_SF | OCERZ_OF;

fn szp_bits(size: c_int, res: u64) -> u64 {
    let mut f = 0u64;
    let res = res & ocerz_mask(size);
    if res == 0 {
        f |= OCERZ_ZF;
    }
    if ocerz_msb(res, size) != 0 {
        f |= OCERZ_SF;
    }
    if ((res & 0xff) as u32).count_ones() % 2 == 0 {
        f |= OCERZ_PF;
    }
    f
}

unsafe fn put_arith(cpu: *mut OcerzCPU, f: u64) {
    unsafe {
        (*cpu).rflags = ((*cpu).rflags & !OCERZ_ARITH_FLAGS) | f;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_flags_szp(cpu: *mut OcerzCPU, size: c_int, res: u64) {
    unsafe {
        (*cpu).rflags = ((*cpu).rflags & !(OCERZ_PF | OCERZ_ZF | OCERZ_SF)) | szp_bits(size, res);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_flags_add(
    cpu: *mut OcerzCPU,
    size: c_int,
    a: u64,
    b: u64,
    cin: c_int,
    res: u64,
) {
    unsafe {
        let m = ocerz_mask(size);
        let a = a & m;
        let b = b & m;
        let res = res & m;
        let mut f = szp_bits(size, res);
        if res < a || (cin != 0 && res == a) {
            f |= OCERZ_CF;
        }
        if ocerz_msb(!(a ^ b) & (a ^ res), size) != 0 {
            f |= OCERZ_OF;
        }
        if (a ^ b ^ res) & 0x10 != 0 {
            f |= OCERZ_AF;
        }
        put_arith(cpu, f);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_flags_sub(
    cpu: *mut OcerzCPU,
    size: c_int,
    a: u64,
    b: u64,
    cin: c_int,
    res: u64,
) {
    unsafe {
        let m = ocerz_mask(size);
        let a = a & m;
        let b = b & m;
        let res = res & m;
        let mut f = szp_bits(size, res);
        if b > a || (cin != 0 && b == a) {
            f |= OCERZ_CF;
        }
        if ocerz_msb((a ^ b) & (a ^ res), size) != 0 {
            f |= OCERZ_OF;
        }
        if (a ^ b ^ res) & 0x10 != 0 {
            f |= OCERZ_AF;
        }
        put_arith(cpu, f);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_flags_logic(cpu: *mut OcerzCPU, size: c_int, res: u64) {
    unsafe {
        put_arith(cpu, szp_bits(size, res));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_flags_inc(cpu: *mut OcerzCPU, size: c_int, res: u64) {
    unsafe {
        let m = ocerz_mask(size);
        let res = res & m;
        let mut f = szp_bits(size, res) | ((*cpu).rflags & OCERZ_CF);
        if res == 1u64 << (size * 8 - 1) {
            f |= OCERZ_OF;
        }
        if res & 0xf == 0 {
            f |= OCERZ_AF;
        }
        put_arith(cpu, f);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_flags_dec(cpu: *mut OcerzCPU, size: c_int, res: u64) {
    unsafe {
        let m = ocerz_mask(size);
        let res = res & m;
        let mut f = szp_bits(size, res) | ((*cpu).rflags & OCERZ_CF);
        if res == m >> 1 {
            f |= OCERZ_OF;
        }
        if res & 0xf == 0xf {
            f |= OCERZ_AF;
        }
        put_arith(cpu, f);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_flags_shl(
    cpu: *mut OcerzCPU,
    size: c_int,
    val: u64,
    cnt: c_uint,
    res: u64,
) {
    unsafe {
        let bits = size * 8;
        let val = val & ocerz_mask(size);
        let res = res & ocerz_mask(size);
        let mut f = szp_bits(size, res);
        let mut cf = 0u64;
        if cnt <= bits as c_uint {
            cf = (val >> (bits - cnt as i32)) & 1;
        }
        if cf != 0 {
            f |= OCERZ_CF;
        }
        if (cf ^ ocerz_msb(res, size) as u64) & 1 != 0 {
            f |= OCERZ_OF;
        }
        put_arith(cpu, f);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_flags_shr(
    cpu: *mut OcerzCPU,
    size: c_int,
    val: u64,
    cnt: c_uint,
    res: u64,
) {
    unsafe {
        let bits = size * 8;
        let val = val & ocerz_mask(size);
        let res = res & ocerz_mask(size);
        let mut f = szp_bits(size, res);
        if cnt <= bits as c_uint && (val >> (cnt - 1)) & 1 != 0 {
            f |= OCERZ_CF;
        }
        if ocerz_msb(val, size) != 0 {
            f |= OCERZ_OF;
        }
        put_arith(cpu, f);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_flags_sar(
    cpu: *mut OcerzCPU,
    size: c_int,
    val: u64,
    cnt: c_uint,
    res: u64,
) {
    unsafe {
        let res = res & ocerz_mask(size);
        let sval = ocerz_sext(val, size);
        let mut f = szp_bits(size, res);
        let mut shift = cnt - 1;
        if shift > 63 {
            shift = 63;
        }
        if ((sval >> shift) as u64) & 1 != 0 {
            f |= OCERZ_CF;
        }
        put_arith(cpu, f);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_flags_mul(cpu: *mut OcerzCPU, size: c_int, lo: u64, hi: u64) {
    unsafe {
        let _ = lo;
        let mut f = 0u64;
        if hi & ocerz_mask(size) != 0 {
            f |= OCERZ_CF | OCERZ_OF;
        }
        put_arith(cpu, f);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_flags_imul(cpu: *mut OcerzCPU, size: c_int, lo: u64, hi: u64) {
    unsafe {
        let m = ocerz_mask(size);
        let sign = if ocerz_msb(lo, size) != 0 { m } else { 0 };
        let mut f = 0u64;
        if hi & m != sign {
            f |= OCERZ_CF | OCERZ_OF;
        }
        put_arith(cpu, f);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_flags_materialize(cpu: *mut OcerzCPU) {
    unsafe {
        let op = (*cpu).cc_op;
        if op == OCERZ_CC_NONE {
            return;
        }
        let kind = op & 0xff;
        let size = ((op >> 8) & 0xff) as c_int;
        let cin = ((op >> 16) & 1) as c_int;
        let s = (*cpu).cc_src;
        let d = (*cpu).cc_dst;
        let cnt = d as c_uint;
        match kind {
            OCERZ_CC_ADD => ocerz_flags_add(cpu, size, s, d, cin, s.wrapping_add(d).wrapping_add(cin as u64)),
            OCERZ_CC_SUB => ocerz_flags_sub(cpu, size, s, d, cin, s.wrapping_sub(d).wrapping_sub(cin as u64)),
            OCERZ_CC_LOGIC => ocerz_flags_logic(cpu, size, d),
            OCERZ_CC_INC => {
                ocerz_flag_assign(cpu, OCERZ_CF, (s & 1) as c_int);
                ocerz_flags_inc(cpu, size, d);
            }
            OCERZ_CC_DEC => {
                ocerz_flag_assign(cpu, OCERZ_CF, (s & 1) as c_int);
                ocerz_flags_dec(cpu, size, d);
            }
            OCERZ_CC_SHL => ocerz_flags_shl(cpu, size, s, cnt, s << cnt),
            OCERZ_CC_SHR => ocerz_flags_shr(cpu, size, s, cnt, (s & ocerz_mask(size)) >> cnt),
            OCERZ_CC_SAR => ocerz_flags_sar(cpu, size, s, cnt, (ocerz_sext(s, size) >> cnt) as u64),
            OCERZ_CC_MUL => ocerz_flags_mul(cpu, size, s, d),
            OCERZ_CC_IMUL => ocerz_flags_imul(cpu, size, s, d),
            _ => {}
        }
        (*cpu).cc_op = OCERZ_CC_NONE;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cc_eval(cpu: *const OcerzCPU, cc: c_uint) -> c_int {
    unsafe {
        let f = (*cpu).rflags;
        let cf = f & OCERZ_CF != 0;
        let zf = f & OCERZ_ZF != 0;
        let sf = f & OCERZ_SF != 0;
        let of = f & OCERZ_OF != 0;
        let pf = f & OCERZ_PF != 0;
        let mut r = match cc >> 1 {
            0 => of,
            1 => cf,
            2 => zf,
            3 => cf || zf,
            4 => sf,
            5 => pf,
            6 => sf != of,
            _ => zf || sf != of,
        };
        if cc & 1 != 0 {
            r = !r;
        }
        r as c_int
    }
}

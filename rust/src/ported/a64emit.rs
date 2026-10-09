//!  arm64 instruction emitter: one function per instruction, each appending a
//!  word to the buffer.  No scheduling, no register allocation and no peepholes -
//!  the callers in jit.c decide what to emit; this file only knows how to spell
//!  it.
//!
//!  The encoders that carry real logic are the ones where arm64's encoding is not
//!  a straight field packing.  A logical immediate must be a rotated run of ones
//!  replicated at some element size, so the encoder finds the smallest element
//!  that replicates the value and checks that the run (or its complement) does
//!  not wrap.  Such a value always changes bit a power-of-two number of times
//!  going round the register, so any other count is refused before the search:
//!  mov_imm64 offers every 64-bit address it builds to the encoder first, almost
//!  none are logical immediates, and the search was a tenth of translation time.
//!  The rest is field assembly, with the aliases spelled out where the
//!  architecture defines them that way (CSETM is CSINV with XZR twice, ROR is
//!  EXTR with rn == rm).
//!
//!  Two range limits matter to callers rather than to this file: TBZ/TBNZ carry a
//!  signed imm14, which is +-32 KB and far shorter than the imm19 of B.cond, so a
//!  large block can put a stub out of a tbz's reach; and the scaled immediate
//!  forms of LDR/STR cover only non-negative multiples of the access size, which
//!  is why the unscaled LDUR/STUR forms (signed 9-bit) are here too.  LDAPUR and
//!  STLUR are the FEAT_LRCPC2 acquire-load and release-store with that same
//!  unscaled offset, and they are what the JIT's ordered accesses are built from;
//!  LDAPURSB, LDAPURSH and LDAPURSW are the acquire-loads that sign-extend, so an
//!  ordered movsx from memory is one instruction as a plain one is.

use crate::ffi;
use core::ffi::{c_int, c_uint};

const A64_ZR: c_int = 31;

#[inline(always)]
fn emit32(b: *mut ffi::A64Buf, w: u32) {
    unsafe {
        if (*b).p >= (*b).end {
            (*b).overflow = 1;
            return;
        }
        *(*b).p = w;
        (*b).p = (*b).p.add(1);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_emit32(b: *mut ffi::A64Buf, w: u32) -> () {
    unsafe {
        if (*b).p >= (*b).end {
            (*b).overflow = 1;
            return;
        }
        (*(*b).p) = w;
        (*b).p = (*b).p.add(1);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_label(b: *mut ffi::A64Buf) -> *mut u32 {
    unsafe {
        if (*b).p >= (*b).end {
            core::ptr::addr_of_mut!((*b).sink)
        } else {
            (*b).p
        }
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_movz(b: *mut ffi::A64Buf, rd: c_int, imm: u16, hw: c_int) -> () {
    unsafe {
        emit32(
            b,
            0xd2800000u32 | (((hw & 3) as u32) << 21) | ((imm as u32) << 5) | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_movk(b: *mut ffi::A64Buf, rd: c_int, imm: u16, hw: c_int) -> () {
    unsafe {
        emit32(
            b,
            0xf2800000u32 | (((hw & 3) as u32) << 21) | ((imm as u32) << 5) | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_movn(b: *mut ffi::A64Buf, rd: c_int, imm: u16, hw: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x92800000u32 | (((hw & 3) as u32) << 21) | ((imm as u32) << 5) | ((rd & 31) as u32),
        );
    }
}

#[inline(always)]
fn low_mask(bits: c_uint) -> u64 {
    unsafe {
        if bits == 64 {
            u64::MAX
        } else {
            (1u64 << bits) - 1
        }
    }
}

#[inline(always)]
fn ror_element(v: u64, rot: c_uint, bits: c_uint) -> u64 {
    unsafe {
        let mut mask: u64 = low_mask(bits);
        let mut rot = rot;
        rot &= bits - 1;
        if rot == 0 {
            return v & mask;
        }
        return ((v >> rot) | (v << (bits - rot))) & mask;
    }
}

#[inline(always)]
fn try_logic_imm(
    b: *mut ffi::A64Buf,
    base: u32,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    imm: u64,
) -> c_int {
    unsafe {
        let mut fields = 0;
        if logical_imm_fields(sf, imm, &mut fields) == 0 {
            return 0;
        }
        emit32(
            b,
            base | (((sf != 0) as u32) << 31)
                | fields
                | (((rn & 31) as u32) << 5)
                | (rd & 31) as u32,
        );
        1
    }
}

#[inline(always)]
fn logical_imm_fields(sf: c_int, imm: u64, fields: *mut u32) -> c_int {
    unsafe {
        let width: c_uint = if sf != 0 { 64 } else { 32 };
        let width_mask = low_mask(width);
        let imm = imm & width_mask;
        if imm == 0 || imm == width_mask {
            return 0;
        }
        let rot1 = ((imm >> 1) | (imm << (width - 1))) & width_mask;
        let transitions = (imm ^ rot1).count_ones();
        if transitions & transitions.wrapping_sub(1) != 0 {
            return 0;
        }
        let mut esize = width;
        let mut e = 2;
        while e < width {
            let emask = low_mask(e);
            let element = imm & emask;
            let mut replicated = element;
            let mut shift = e;
            while shift < width {
                replicated |= replicated << shift;
                shift <<= 1;
            }
            if replicated & width_mask == imm {
                esize = e;
                break;
            }
            e <<= 1;
        }
        let emask = low_mask(esize);
        let element = imm & emask;
        let ones = element.count_ones();
        if ones == 0 || ones == esize {
            return 0;
        }
        let v = element;
        let lowbit = v & (!v).wrapping_add(1);
        let t = v.wrapping_add(lowbit) & emask;
        let rot;
        if t & t.wrapping_sub(1) == 0 {
            let sbit = v.trailing_zeros();
            rot = esize.wrapping_sub(sbit) % esize;
        } else {
            let nv = !v & emask;
            let nlow = nv & (!nv).wrapping_add(1);
            let nt = nv.wrapping_add(nlow) & emask;
            if nt & nt.wrapping_sub(1) != 0 {
                return 0;
            }
            let sp = nv.trailing_zeros();
            let k = nv.count_ones();
            rot = esize.wrapping_sub(sp.wrapping_add(k)) % esize;
        }
        let run = low_mask(ones);
        if ror_element(run, rot, esize) != element {
            return 0;
        }
        let n = if esize == 64 { 1u32 } else { 0 };
        let imms = (!(esize.wrapping_mul(2).wrapping_sub(1)) & 0x3f) | ones.wrapping_sub(1);
        *fields = (n << 22) | ((rot & 0x3f) << 16) | (imms << 10);
        1
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_try_and_imm(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    imm: u64,
) -> c_int {
    unsafe {
        return try_logic_imm(b, 0x12000000u32, sf, rd, rn, imm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_try_ands_imm(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    imm: u64,
) -> c_int {
    unsafe {
        return try_logic_imm(b, 0x72000000u32, sf, rd, rn, imm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_try_orr_imm(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    imm: u64,
) -> c_int {
    unsafe {
        return try_logic_imm(b, 0x32000000u32, sf, rd, rn, imm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_try_eor_imm(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    imm: u64,
) -> c_int {
    unsafe {
        return try_logic_imm(b, 0x52000000u32, sf, rd, rn, imm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_mov_imm64(b: *mut ffi::A64Buf, rd: c_int, v: u64) -> () {
    unsafe {
        let lane = [
            v as u16,
            (v >> 16) as u16,
            (v >> 32) as u16,
            (v >> 48) as u16,
        ];
        let mut zero = 0;
        let mut ones = 0;
        for i in 0..4 {
            let x = *lane.get_unchecked(i);
            if x == 0 {
                zero += 1;
            }
            if x == 0xffff {
                ones += 1;
            }
        }
        let mut wide_count = 4 - if zero > ones { zero } else { ones };
        if wide_count == 0 {
            wide_count = 1;
        }
        if wide_count > 1 && a64_try_orr_imm(b, 1, rd, A64_ZR, v) != 0 {
            return;
        }
        if ones > zero {
            let mut seeded = false;
            for i in 0..4 {
                let x = *lane.get_unchecked(i);
                if x != 0xffff {
                    if !seeded {
                        a64_movn(b, rd, !x, i as c_int);
                        seeded = true;
                    } else {
                        a64_movk(b, rd, x, i as c_int);
                    }
                }
            }
            if !seeded {
                a64_movn(b, rd, 0, 0);
            }
            return;
        }
        let mut seeded = false;
        for i in 0..4 {
            let x = *lane.get_unchecked(i);
            if x != 0 {
                if !seeded {
                    a64_movz(b, rd, x, i as c_int);
                    seeded = true;
                } else {
                    a64_movk(b, rd, x, i as c_int);
                }
            }
        }
        if !seeded {
            a64_movz(b, rd, 0, 0);
        }
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_mov_reg(b: *mut ffi::A64Buf, sf: c_int, rd: c_int, rm: c_int) -> () {
    unsafe {
        a64_orr_reg(b, sf, rd, A64_ZR, rm, 0);
    }
}

#[inline(always)]
fn ldst_size_bits(size: c_int) -> u32 {
    unsafe {
        match size {
            1 => 0,
            2 => 1,
            4 => 2,
            _ => 3,
        }
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldr(
    b: *mut ffi::A64Buf,
    size: c_int,
    rt: c_int,
    rn: c_int,
    off: u32,
) -> () {
    unsafe {
        let mut sz: u32 = ldst_size_bits(size);
        let mut imm12: u32 = off.checked_div(size as u32).unwrap_or(0);
        emit32(
            b,
            0x39400000u32
                | (sz << 30)
                | (imm12 << 10)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_str(
    b: *mut ffi::A64Buf,
    size: c_int,
    rt: c_int,
    rn: c_int,
    off: u32,
) -> () {
    unsafe {
        let mut sz: u32 = ldst_size_bits(size);
        let mut imm12: u32 = off.checked_div(size as u32).unwrap_or(0);
        emit32(
            b,
            0x39000000u32
                | (sz << 30)
                | (imm12 << 10)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldr_post64(
    b: *mut ffi::A64Buf,
    rt: c_int,
    rn: c_int,
    imm: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0xf8400400u32
                | (((imm as u32) & 0x1ffu32) << 12)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_str_pre64(
    b: *mut ffi::A64Buf,
    rt: c_int,
    rn: c_int,
    imm: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0xf8000c00u32
                | (((imm as u32) & 0x1ffu32) << 12)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldr_regoff(
    b: *mut ffi::A64Buf,
    size: c_int,
    rt: c_int,
    rn: c_int,
    rm: c_int,
    scaled: c_int,
) -> () {
    unsafe {
        let mut sz: u32 = (if size == 8 {
            3u32
        } else {
            (if size == 4 {
                2u32
            } else {
                (if size == 2 { 1u32 } else { 0u32 })
            })
        });
        emit32(
            b,
            0x38606800u32
                | (sz << 30)
                | (((rm & 31) as u32) << 16)
                | (((scaled != 0) as u32) << 12)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldr_regoff_uxtw(
    b: *mut ffi::A64Buf,
    size: c_int,
    rt: c_int,
    rn: c_int,
    rm: c_int,
) -> () {
    unsafe {
        let mut sz: u32 = (if size == 8 {
            3u32
        } else {
            (if size == 4 {
                2u32
            } else {
                (if size == 2 { 1u32 } else { 0u32 })
            })
        });
        emit32(
            b,
            0x38604800u32
                | (sz << 30)
                | (((rm & 31) as u32) << 16)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_str_regoff_uxtw(
    b: *mut ffi::A64Buf,
    size: c_int,
    rt: c_int,
    rn: c_int,
    rm: c_int,
) -> () {
    unsafe {
        let mut sz: u32 = (if size == 8 {
            3u32
        } else {
            (if size == 4 {
                2u32
            } else {
                (if size == 2 { 1u32 } else { 0u32 })
            })
        });
        emit32(
            b,
            0x38204800u32
                | (sz << 30)
                | (((rm & 31) as u32) << 16)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_str_regoff(
    b: *mut ffi::A64Buf,
    size: c_int,
    rt: c_int,
    rn: c_int,
    rm: c_int,
    scaled: c_int,
) -> () {
    unsafe {
        let mut sz: u32 = (if size == 8 {
            3u32
        } else {
            (if size == 4 {
                2u32
            } else {
                (if size == 2 { 1u32 } else { 0u32 })
            })
        });
        emit32(
            b,
            0x38206800u32
                | (sz << 30)
                | (((rm & 31) as u32) << 16)
                | (((scaled != 0) as u32) << 12)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldar(b: *mut ffi::A64Buf, size: c_int, rt: c_int, rn: c_int) -> () {
    unsafe {
        let mut sz: u32 = ldst_size_bits(size);
        emit32(
            b,
            0x08dffc00u32 | (sz << 30) | (((rn & 31) as u32) << 5) | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_stlr(b: *mut ffi::A64Buf, size: c_int, rt: c_int, rn: c_int) -> () {
    unsafe {
        let mut sz: u32 = ldst_size_bits(size);
        emit32(
            b,
            0x089ffc00u32 | (sz << 30) | (((rn & 31) as u32) << 5) | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldapr(b: *mut ffi::A64Buf, size: c_int, rt: c_int, rn: c_int) -> () {
    unsafe {
        let mut sz: u32 = ldst_size_bits(size);
        emit32(
            b,
            0x38bfc000u32 | (sz << 30) | (((rn & 31) as u32) << 5) | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_dmb_ish(b: *mut ffi::A64Buf) -> () {
    unsafe {
        emit32(b, 0xd5033bbfu32);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldrsb(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rt: c_int,
    rn: c_int,
    off: u32,
) -> () {
    unsafe {
        let mut opc: u32 = (if sf != 0 { 2u32 } else { 3u32 });
        emit32(
            b,
            0x39000000u32
                | (opc << 22)
                | (off << 10)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldrsh(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rt: c_int,
    rn: c_int,
    off: u32,
) -> () {
    unsafe {
        let mut opc: u32 = (if sf != 0 { 2u32 } else { 3u32 });
        emit32(
            b,
            0x79000000u32
                | (opc << 22)
                | ((off / 2) << 10)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldrsw(b: *mut ffi::A64Buf, rt: c_int, rn: c_int, off: u32) -> () {
    unsafe {
        emit32(
            b,
            0xb9800000u32 | ((off / 4) << 10) | (((rn & 31) as u32) << 5) | ((rt & 31) as u32),
        );
    }
}

#[inline(always)]
fn addsub_imm(b: *mut ffi::A64Buf, base: u32, sf: c_int, rd: c_int, rn: c_int, imm12: u32) -> () {
    unsafe {
        emit32(
            b,
            base | ((sf as u32) << 31)
                | ((imm12 & 0xfff) << 10)
                | (((rn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_add_imm(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    imm12: u32,
) -> () {
    unsafe {
        addsub_imm(b, 0x11000000u32, sf, rd, rn, imm12);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_sub_imm(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    imm12: u32,
) -> () {
    unsafe {
        addsub_imm(b, 0x51000000u32, sf, rd, rn, imm12);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_adds_imm(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    imm12: u32,
) -> () {
    unsafe {
        addsub_imm(b, 0x31000000u32, sf, rd, rn, imm12);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_subs_imm(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    imm12: u32,
) -> () {
    unsafe {
        addsub_imm(b, 0x71000000u32, sf, rd, rn, imm12);
    }
}

#[inline(always)]
fn addsub_reg(
    b: *mut ffi::A64Buf,
    base: u32,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
    lsl: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            base | ((sf as u32) << 31)
                | (((rm & 31) as u32) << 16)
                | (((lsl & 63) as u32) << 10)
                | (((rn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_add_reg(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
    lsl: c_int,
) -> () {
    unsafe {
        addsub_reg(b, 0x0b000000u32, sf, rd, rn, rm, lsl);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_adds_reg(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
    lsl: c_int,
) -> () {
    unsafe {
        addsub_reg(b, 0x2b000000u32, sf, rd, rn, rm, lsl);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_sub_reg(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
    lsl: c_int,
) -> () {
    unsafe {
        addsub_reg(b, 0x4b000000u32, sf, rd, rn, rm, lsl);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_subs_reg(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
    lsl: c_int,
) -> () {
    unsafe {
        addsub_reg(b, 0x6b000000u32, sf, rd, rn, rm, lsl);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_add_ext_uxtw(
    b: *mut ffi::A64Buf,
    rd: c_int,
    rn: c_int,
    rm: c_int,
    shift: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x8b200000u32
                | (((rm & 31) as u32) << 16)
                | (2u32 << 13)
                | (((shift & 7) as u32) << 10)
                | (((rn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_adcs_reg(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x3a000000u32
                | ((sf as u32) << 31)
                | (((rm & 31) as u32) << 16)
                | (((rn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_sbcs_reg(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x7a000000u32
                | ((sf as u32) << 31)
                | (((rm & 31) as u32) << 16)
                | (((rn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline(always)]
fn logic_reg(
    b: *mut ffi::A64Buf,
    base: u32,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
    lsl: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            base | ((sf as u32) << 31)
                | (((rm & 31) as u32) << 16)
                | (((lsl & 63) as u32) << 10)
                | (((rn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_and_reg(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
    lsl: c_int,
) -> () {
    unsafe {
        logic_reg(b, 0x0a000000u32, sf, rd, rn, rm, lsl);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ands_reg(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
    lsl: c_int,
) -> () {
    unsafe {
        logic_reg(b, 0x6a000000u32, sf, rd, rn, rm, lsl);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_orr_reg(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
    lsl: c_int,
) -> () {
    unsafe {
        logic_reg(b, 0x2a000000u32, sf, rd, rn, rm, lsl);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_orn_reg(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
    lsl: c_int,
) -> () {
    unsafe {
        logic_reg(b, 0x2a200000u32, sf, rd, rn, rm, lsl);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_eor_reg(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
    lsl: c_int,
) -> () {
    unsafe {
        logic_reg(b, 0x4a000000u32, sf, rd, rn, rm, lsl);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_bic_reg(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
    lsl: c_int,
) -> () {
    unsafe {
        logic_reg(b, 0x0a200000u32, sf, rd, rn, rm, lsl);
    }
}

#[inline(always)]
fn datap2(b: *mut ffi::A64Buf, op: u32, sf: c_int, rd: c_int, rn: c_int, rm: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x1ac00000u32
                | ((sf as u32) << 31)
                | (((rm & 31) as u32) << 16)
                | (op << 10)
                | (((rn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_lslv(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
) -> () {
    unsafe {
        datap2(b, 0x8, sf, rd, rn, rm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_lsrv(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
) -> () {
    unsafe {
        datap2(b, 0x9, sf, rd, rn, rm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_asrv(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
) -> () {
    unsafe {
        datap2(b, 0xa, sf, rd, rn, rm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_rorv(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
) -> () {
    unsafe {
        datap2(b, 0xb, sf, rd, rn, rm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_udiv(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
) -> () {
    unsafe {
        datap2(b, 0x2, sf, rd, rn, rm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_sdiv(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
) -> () {
    unsafe {
        datap2(b, 0x3, sf, rd, rn, rm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_msub(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
    ra: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x1b008000u32
                | ((sf as u32) << 31)
                | (((rm & 31) as u32) << 16)
                | (((ra & 31) as u32) << 10)
                | (((rn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_extr(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
    lsb: c_int,
) -> () {
    unsafe {
        let mut n: u32 = (if sf != 0 { 1u32 } else { 0u32 });
        emit32(
            b,
            0x13800000u32
                | ((sf as u32) << 31)
                | (n << 22)
                | (((rm & 31) as u32) << 16)
                | (((lsb & 63) as u32) << 10)
                | (((rn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline(always)]
fn bfm(
    b: *mut ffi::A64Buf,
    opc: u32,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    immr: c_int,
    imms: c_int,
) -> () {
    unsafe {
        let mut n: u32 = (if sf != 0 { 1u32 } else { 0u32 });
        emit32(
            b,
            0x13000000u32
                | (opc << 29)
                | ((sf as u32) << 31)
                | (n << 22)
                | (((immr & 63) as u32) << 16)
                | (((imms & 63) as u32) << 10)
                | (((rn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ubfm(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    immr: c_int,
    imms: c_int,
) -> () {
    unsafe {
        bfm(b, 2, sf, rd, rn, immr, imms);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_sbfm(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    immr: c_int,
    imms: c_int,
) -> () {
    unsafe {
        bfm(b, 0, sf, rd, rn, immr, imms);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_bfm(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    immr: c_int,
    imms: c_int,
) -> () {
    unsafe {
        bfm(b, 1, sf, rd, rn, immr, imms);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ubfx(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    lsb: c_int,
    width: c_int,
) -> () {
    unsafe {
        a64_ubfm(b, sf, rd, rn, lsb, lsb + width - 1);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_sbfx(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    lsb: c_int,
    width: c_int,
) -> () {
    unsafe {
        a64_sbfm(b, sf, rd, rn, lsb, lsb + width - 1);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_bfi(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    lsb: c_int,
    width: c_int,
) -> () {
    unsafe {
        let mut bits: c_int = (if sf != 0 { 64 } else { 32 });
        a64_bfm(b, sf, rd, rn, (bits - lsb) & (bits - 1), width - 1);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_lsl_imm(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    sh: c_int,
) -> () {
    unsafe {
        let mut bits: c_int = (if sf != 0 { 64 } else { 32 });
        a64_ubfm(b, sf, rd, rn, (bits - sh) & (bits - 1), bits - 1 - sh);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_lsr_imm(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    sh: c_int,
) -> () {
    unsafe {
        let mut bits: c_int = (if sf != 0 { 64 } else { 32 });
        a64_ubfm(b, sf, rd, rn, sh, bits - 1);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_asr_imm(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    sh: c_int,
) -> () {
    unsafe {
        let mut bits: c_int = (if sf != 0 { 64 } else { 32 });
        a64_sbfm(b, sf, rd, rn, sh, bits - 1);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_uxtb(b: *mut ffi::A64Buf, rd: c_int, rn: c_int) -> () {
    unsafe {
        a64_ubfm(b, 0, rd, rn, 0, 7);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_uxth(b: *mut ffi::A64Buf, rd: c_int, rn: c_int) -> () {
    unsafe {
        a64_ubfm(b, 0, rd, rn, 0, 15);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_sxtb(b: *mut ffi::A64Buf, sf: c_int, rd: c_int, rn: c_int) -> () {
    unsafe {
        a64_sbfm(b, sf, rd, rn, 0, 7);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_sxth(b: *mut ffi::A64Buf, sf: c_int, rd: c_int, rn: c_int) -> () {
    unsafe {
        a64_sbfm(b, sf, rd, rn, 0, 15);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_sxtw(b: *mut ffi::A64Buf, rd: c_int, rn: c_int) -> () {
    unsafe {
        a64_sbfm(b, 1, rd, rn, 0, 31);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_cset(b: *mut ffi::A64Buf, rd: c_int, cond: c_int) -> () {
    unsafe {
        let mut inv: c_int = (cond ^ 1);
        emit32(
            b,
            0x1a9f07e0u32 | (((inv & 15) as u32) << 12) | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_csetm(b: *mut ffi::A64Buf, rd: c_int, cond: c_int) -> () {
    unsafe {
        let mut inv: c_int = (cond ^ 1);
        emit32(
            b,
            0xda9f03e0u32 | (((inv & 15) as u32) << 12) | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_csel(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
    cond: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x1a800000u32
                | ((sf as u32) << 31)
                | (((rm & 31) as u32) << 16)
                | (((cond & 15) as u32) << 12)
                | (((rn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_csneg(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
    cond: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x5a800400u32
                | ((sf as u32) << 31)
                | (((rm & 31) as u32) << 16)
                | (((cond & 15) as u32) << 12)
                | (((rn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_mul(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x1b007c00u32
                | ((sf as u32) << 31)
                | (((rm & 31) as u32) << 16)
                | (((rn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_umulh(b: *mut ffi::A64Buf, rd: c_int, rn: c_int, rm: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x9bc07c00u32
                | (((rm & 31) as u32) << 16)
                | (((rn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_smulh(b: *mut ffi::A64Buf, rd: c_int, rn: c_int, rm: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x9b407c00u32
                | (((rm & 31) as u32) << 16)
                | (((rn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_rev(b: *mut ffi::A64Buf, sf: c_int, rd: c_int, rn: c_int) -> () {
    unsafe {
        let mut base: u32 = (if sf != 0 {
            0xdac00c00u32
        } else {
            0x5ac00800u32
        });
        emit32(b, base | (((rn & 31) as u32) << 5) | ((rd & 31) as u32));
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_b(b: *mut ffi::A64Buf, off_words: i32) -> () {
    unsafe {
        emit32(b, 0x14000000u32 | ((off_words as u32) & 0x03ffffffu32));
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_bcond(b: *mut ffi::A64Buf, cond: c_int, off_words: i32) -> () {
    unsafe {
        emit32(
            b,
            0x54000000u32 | (((off_words as u32) & 0x7ffff) << 5) | ((cond & 15) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_cbz(b: *mut ffi::A64Buf, sf: c_int, rt: c_int, off_words: i32) -> () {
    unsafe {
        emit32(
            b,
            0x34000000u32
                | ((sf as u32) << 31)
                | (((off_words as u32) & 0x7ffff) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_cbnz(b: *mut ffi::A64Buf, sf: c_int, rt: c_int, off_words: i32) -> () {
    unsafe {
        emit32(
            b,
            0x35000000u32
                | ((sf as u32) << 31)
                | (((off_words as u32) & 0x7ffff) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_tbz(b: *mut ffi::A64Buf, rt: c_int, bit: c_int, off_words: i32) -> () {
    unsafe {
        emit32(
            b,
            0x36000000u32
                | (((bit & 0x20) as u32) << 26)
                | (((bit & 31) as u32) << 19)
                | (((off_words as u32) & 0x3fff) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_tbnz(
    b: *mut ffi::A64Buf,
    rt: c_int,
    bit: c_int,
    off_words: i32,
) -> () {
    unsafe {
        emit32(
            b,
            0x37000000u32
                | (((bit & 0x20) as u32) << 26)
                | (((bit & 31) as u32) << 19)
                | (((off_words as u32) & 0x3fff) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline(always)]
fn ptr_word_offset(at: *mut u32, target: *mut u32) -> isize {
    ((target as usize).wrapping_sub(at as usize) as isize) / 4
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_patch_b(at: *mut u32, target: *mut u32) -> () {
    unsafe {
        let off = ptr_word_offset(at, target) as i32;
        *at = (*at & 0xfc000000) | ((off as u32) & 0x03ffffff);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_try_patch_b(at: *mut u32, target: *mut u32) -> c_int {
    unsafe {
        let off = ptr_word_offset(at, target);
        if off < -(1isize << 25) || off > (1isize << 25) - 1 {
            return 0;
        }
        *at = (*at & 0xfc000000) | ((off as i32 as u32) & 0x03ffffff);
        1
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_patch_bcond(at: *mut u32, target: *mut u32) -> () {
    unsafe {
        let off = ptr_word_offset(at, target) as i32;
        *at = (*at & 0xff00001f) | (((off as u32) & 0x7ffff) << 5);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_patch_cbz(at: *mut u32, target: *mut u32) -> () {
    unsafe {
        let off = ptr_word_offset(at, target) as i32;
        *at = (*at & 0xff00001f) | (((off as u32) & 0x7ffff) << 5);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_patch_tbz(at: *mut u32, target: *mut u32) -> () {
    unsafe {
        let off = ptr_word_offset(at, target) as i32;
        *at = (*at & 0xfff8001f) | (((off as u32) & 0x3fff) << 5);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_try_patch_tbz(at: *mut u32, target: *mut u32) -> c_int {
    unsafe {
        let off = ptr_word_offset(at, target);
        if off < -(1isize << 13) || off > (1isize << 13) - 1 {
            return 0;
        }
        *at = (*at & 0xfff8001f) | (((off as i32 as u32) & 0x3fff) << 5);
        1
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ret(b: *mut ffi::A64Buf) -> () {
    unsafe {
        emit32(b, 0xd65f03c0u32);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_br(b: *mut ffi::A64Buf, rn: c_int) -> () {
    unsafe {
        emit32(b, 0xd61f0000u32 | (((rn & 31) as u32) << 5));
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_blr(b: *mut ffi::A64Buf, rn: c_int) -> () {
    unsafe {
        emit32(b, 0xd63f0000u32 | (((rn & 31) as u32) << 5));
    }
}

#[inline(always)]
fn ldstp_imm7(imm: c_int) -> u32 {
    unsafe {
        return ((imm / 8) & 0x7f) as u32;
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_stp_pre(
    b: *mut ffi::A64Buf,
    rt: c_int,
    rt2: c_int,
    rn: c_int,
    imm: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0xa9800000u32
                | (ldstp_imm7(imm) << 15)
                | (((rt2 & 31) as u32) << 10)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldp_post(
    b: *mut ffi::A64Buf,
    rt: c_int,
    rt2: c_int,
    rn: c_int,
    imm: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0xa8c00000u32
                | (ldstp_imm7(imm) << 15)
                | (((rt2 & 31) as u32) << 10)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_stp_off(
    b: *mut ffi::A64Buf,
    rt: c_int,
    rt2: c_int,
    rn: c_int,
    imm: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0xa9000000u32
                | (ldstp_imm7(imm) << 15)
                | (((rt2 & 31) as u32) << 10)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldp_off(
    b: *mut ffi::A64Buf,
    rt: c_int,
    rt2: c_int,
    rn: c_int,
    imm: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0xa9400000u32
                | (ldstp_imm7(imm) << 15)
                | (((rt2 & 31) as u32) << 10)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_stp_d_pre(
    b: *mut ffi::A64Buf,
    dt: c_int,
    dt2: c_int,
    rn: c_int,
    imm: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x6d800000u32
                | (ldstp_imm7(imm) << 15)
                | (((dt2 & 31) as u32) << 10)
                | (((rn & 31) as u32) << 5)
                | ((dt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldp_d_post(
    b: *mut ffi::A64Buf,
    dt: c_int,
    dt2: c_int,
    rn: c_int,
    imm: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x6cc00000u32
                | (ldstp_imm7(imm) << 15)
                | (((dt2 & 31) as u32) << 10)
                | (((rn & 31) as u32) << 5)
                | ((dt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_stp_q_pre(
    b: *mut ffi::A64Buf,
    qt: c_int,
    qt2: c_int,
    rn: c_int,
    imm: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0xad800000u32
                | ((((imm / 16) as u32) & 0x7f) << 15)
                | (((qt2 & 31) as u32) << 10)
                | (((rn & 31) as u32) << 5)
                | ((qt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldp_q_post(
    b: *mut ffi::A64Buf,
    qt: c_int,
    qt2: c_int,
    rn: c_int,
    imm: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0xacc00000u32
                | ((((imm / 16) as u32) & 0x7f) << 15)
                | (((qt2 & 31) as u32) << 10)
                | (((rn & 31) as u32) << 5)
                | ((qt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_stp_q_off(
    b: *mut ffi::A64Buf,
    qt: c_int,
    qt2: c_int,
    rn: c_int,
    imm: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0xad000000u32
                | ((((imm / 16) as u32) & 0x7f) << 15)
                | (((qt2 & 31) as u32) << 10)
                | (((rn & 31) as u32) << 5)
                | ((qt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldp_q_off(
    b: *mut ffi::A64Buf,
    qt: c_int,
    qt2: c_int,
    rn: c_int,
    imm: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0xad400000u32
                | ((((imm / 16) as u32) & 0x7f) << 15)
                | (((qt2 & 31) as u32) << 10)
                | (((rn & 31) as u32) << 5)
                | ((qt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldr_v(
    b: *mut ffi::A64Buf,
    size: c_int,
    vt: c_int,
    rn: c_int,
    off: u32,
) -> () {
    unsafe {
        let mut base: u32;
        let mut scale: u32;
        if (size == 16) {
            base = 0x3dc00000u32;
            scale = 16;
        } else if (size == 8) {
            base = 0xfd400000u32;
            scale = 8;
        } else {
            base = 0xbd400000u32;
            scale = 4;
        }
        emit32(
            b,
            base | ((off / scale) << 10) | (((rn & 31) as u32) << 5) | ((vt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_str_v(
    b: *mut ffi::A64Buf,
    size: c_int,
    vt: c_int,
    rn: c_int,
    off: u32,
) -> () {
    unsafe {
        let mut base: u32;
        let mut scale: u32;
        if (size == 16) {
            base = 0x3d800000u32;
            scale = 16;
        } else if (size == 8) {
            base = 0xfd000000u32;
            scale = 8;
        } else {
            base = 0xbd000000u32;
            scale = 4;
        }
        emit32(
            b,
            base | ((off / scale) << 10) | (((rn & 31) as u32) << 5) | ((vt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldur(
    b: *mut ffi::A64Buf,
    size: c_int,
    rt: c_int,
    rn: c_int,
    simm9: i32,
) -> () {
    unsafe {
        let mut sz: u32 = ldst_size_bits(size);
        emit32(
            b,
            0x38400000u32
                | (sz << 30)
                | (((simm9 as u32) & 0x1ffu32) << 12)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_stur(
    b: *mut ffi::A64Buf,
    size: c_int,
    rt: c_int,
    rn: c_int,
    simm9: i32,
) -> () {
    unsafe {
        let mut sz: u32 = ldst_size_bits(size);
        emit32(
            b,
            0x38000000u32
                | (sz << 30)
                | (((simm9 as u32) & 0x1ffu32) << 12)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldur_v(
    b: *mut ffi::A64Buf,
    size: c_int,
    vt: c_int,
    rn: c_int,
    simm9: i32,
) -> () {
    unsafe {
        let mut base: u32 = (if size == 16 {
            0x3cc00000u32
        } else {
            (if size == 8 {
                0xfc400000u32
            } else {
                0xbc400000u32
            })
        });
        emit32(
            b,
            base | (((simm9 as u32) & 0x1ffu32) << 12)
                | (((rn & 31) as u32) << 5)
                | ((vt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_stur_v(
    b: *mut ffi::A64Buf,
    size: c_int,
    vt: c_int,
    rn: c_int,
    simm9: i32,
) -> () {
    unsafe {
        let mut base: u32 = (if size == 16 {
            0x3c800000u32
        } else {
            (if size == 8 {
                0xfc000000u32
            } else {
                0xbc000000u32
            })
        });
        emit32(
            b,
            base | (((simm9 as u32) & 0x1ffu32) << 12)
                | (((rn & 31) as u32) << 5)
                | ((vt & 31) as u32),
        );
    }
}

#[inline(always)]
fn fp2(b: *mut ffi::A64Buf, opc: u32, dbl: c_int, vd: c_int, vn: c_int, vm: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x1e200800u32
                | ((dbl as u32) << 22)
                | (((vm & 31) as u32) << 16)
                | (opc << 12)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fadd_s(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        fp2(b, 0x2, dbl, vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fsub_s(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        fp2(b, 0x3, dbl, vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fmul_s(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        fp2(b, 0x0, dbl, vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fdiv_s(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        fp2(b, 0x1, dbl, vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fmax_s(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        fp2(b, 0x4, dbl, vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fmin_s(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        fp2(b, 0x5, dbl, vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fsqrt_s(b: *mut ffi::A64Buf, dbl: c_int, vd: c_int, vn: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x1e21c000u32 | ((dbl as u32) << 22) | (((vn & 31) as u32) << 5) | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fmov_d_d(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x1e604000u32 | (((vn & 31) as u32) << 5) | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fmov_s_s(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x1e204000u32 | (((vn & 31) as u32) << 5) | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fcmp(b: *mut ffi::A64Buf, dbl: c_int, vn: c_int, vm: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x1e202000u32
                | ((dbl as u32) << 22)
                | (((vm & 31) as u32) << 16)
                | (((vn & 31) as u32) << 5),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fcvt_d2s(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x1e624000u32 | (((vn & 31) as u32) << 5) | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fcvt_s2d(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x1e22c000u32 | (((vn & 31) as u32) << 5) | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fcvtzs(
    b: *mut ffi::A64Buf,
    sf: c_int,
    dbl: c_int,
    rd: c_int,
    vn: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x1e380000u32
                | ((sf as u32) << 31)
                | ((dbl as u32) << 22)
                | (((vn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fcvtns(
    b: *mut ffi::A64Buf,
    sf: c_int,
    dbl: c_int,
    rd: c_int,
    vn: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x1e200000u32
                | ((sf as u32) << 31)
                | ((dbl as u32) << 22)
                | (((vn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fcvtps(
    b: *mut ffi::A64Buf,
    sf: c_int,
    dbl: c_int,
    rd: c_int,
    vn: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x1e280000u32
                | ((sf as u32) << 31)
                | ((dbl as u32) << 22)
                | (((vn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fcvtms(
    b: *mut ffi::A64Buf,
    sf: c_int,
    dbl: c_int,
    rd: c_int,
    vn: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x1e300000u32
                | ((sf as u32) << 31)
                | ((dbl as u32) << 22)
                | (((vn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fcmp_zero(b: *mut ffi::A64Buf, dbl: c_int, vn: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x1e202008u32 | ((dbl as u32) << 22) | (((vn & 31) as u32) << 5),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_scvtf(
    b: *mut ffi::A64Buf,
    sf: c_int,
    dbl: c_int,
    vd: c_int,
    rn: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x1e220000u32
                | ((sf as u32) << 31)
                | ((dbl as u32) << 22)
                | (((rn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fmov_x_from_v(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    vn: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            (if sf != 0 {
                0x9e660000u32
            } else {
                0x1e260000u32
            }) | (((vn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fmov_v_from_x(
    b: *mut ffi::A64Buf,
    sf: c_int,
    vd: c_int,
    rn: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            (if sf != 0 {
                0x9e670000u32
            } else {
                0x1e270000u32
            }) | (((rn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline(always)]
fn v3(b: *mut ffi::A64Buf, base: u32, vd: c_int, vn: c_int, vm: c_int) -> () {
    unsafe {
        emit32(
            b,
            base | (((vm & 31) as u32) << 16) | (((vn & 31) as u32) << 5) | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_fadd(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(
            b,
            (if dbl != 0 {
                0x4e60d400u32
            } else {
                0x4e20d400u32
            }),
            vd,
            vn,
            vm,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fmaxv_4s(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x6e30f800u32 | (((vn & 31) as u32) << 5) | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fmaxp_d(b: *mut ffi::A64Buf, dd: c_int, vn: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x7e70f800u32 | (((vn & 31) as u32) << 5) | ((dd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_fsub(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(
            b,
            (if dbl != 0 {
                0x4ee0d400u32
            } else {
                0x4ea0d400u32
            }),
            vd,
            vn,
            vm,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_fmul(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(
            b,
            (if dbl != 0 {
                0x6e60dc00u32
            } else {
                0x6e20dc00u32
            }),
            vd,
            vn,
            vm,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_fdiv(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(
            b,
            (if dbl != 0 {
                0x6e60fc00u32
            } else {
                0x6e20fc00u32
            }),
            vd,
            vn,
            vm,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_fmax(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(
            b,
            (if dbl != 0 {
                0x4e60f400u32
            } else {
                0x4e20f400u32
            }),
            vd,
            vn,
            vm,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_fmin(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(
            b,
            (if dbl != 0 {
                0x4ee0f400u32
            } else {
                0x4ea0f400u32
            }),
            vd,
            vn,
            vm,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_fsqrt(b: *mut ffi::A64Buf, dbl: c_int, vd: c_int, vn: c_int) -> () {
    unsafe {
        v3(
            b,
            (if dbl != 0 {
                0x6ee1f800u32
            } else {
                0x6ea1f800u32
            }),
            vd,
            vn,
            0,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_and(b: *mut ffi::A64Buf, vd: c_int, vn: c_int, vm: c_int) -> () {
    unsafe {
        v3(b, 0x4e201c00u32, vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_orr(b: *mut ffi::A64Buf, vd: c_int, vn: c_int, vm: c_int) -> () {
    unsafe {
        v3(b, 0x4ea01c00u32, vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_eor(b: *mut ffi::A64Buf, vd: c_int, vn: c_int, vm: c_int) -> () {
    unsafe {
        v3(b, 0x6e201c00u32, vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_bic(b: *mut ffi::A64Buf, vd: c_int, vn: c_int, vm: c_int) -> () {
    unsafe {
        v3(b, 0x4e601c00u32, vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_mov(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        v3(b, 0x4ea01c00u32, vd, vn, vn);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_add(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x4e208400u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_sub(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x6e208400u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_zero(b: *mut ffi::A64Buf, vd: c_int) -> () {
    unsafe {
        emit32(b, 0x6f00e400u32 | ((vd & 31) as u32));
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ins_d_x(b: *mut ffi::A64Buf, vd: c_int, idx: c_int, rn: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x4e081c00u32
                | (((idx & 1) as u32) << 20)
                | (((rn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_umov_x_d(b: *mut ffi::A64Buf, rd: c_int, vn: c_int, idx: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x4e083c00u32
                | (((idx & 1) as u32) << 20)
                | (((vn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ins_d_d(
    b: *mut ffi::A64Buf,
    vd: c_int,
    i1: c_int,
    vn: c_int,
    i2: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x6e080400u32
                | (((i1 & 1) as u32) << 20)
                | (((i2 & 1) as u32) << 14)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ins_s_s(
    b: *mut ffi::A64Buf,
    vd: c_int,
    i1: c_int,
    vn: c_int,
    i2: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x6e040400u32
                | (((i1 & 3) as u32) << 19)
                | (((i2 & 3) as u32) << 13)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_fcvtl(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        v3(b, 0x0e617800u32, vd, vn, 0);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_fcvtn(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        v3(b, 0x0e616800u32, vd, vn, 0);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_scvtf_4s(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        v3(b, 0x4e21d800u32, vd, vn, 0);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_fcvtzs_4s(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        v3(b, 0x4ea1b800u32, vd, vn, 0);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_cmeq(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x6e208c00u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_cmgt(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x4e203400u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_dup_d(b: *mut ffi::A64Buf, vd: c_int, vn: c_int, idx: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x4e080400u32
                | (((idx & 1) as u32) << 20)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_dup_s(b: *mut ffi::A64Buf, vd: c_int, vn: c_int, idx: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x4e040400u32
                | (((idx & 3) as u32) << 19)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_zip1(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x4e003800u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_zip2(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x4e007800u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_uzp1(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x4e001800u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_uzp2(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x4e005800u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_bsl(b: *mut ffi::A64Buf, vd: c_int, vn: c_int, vm: c_int) -> () {
    unsafe {
        v3(b, 0x6e601c00u32, vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_sshr_2d(b: *mut ffi::A64Buf, vd: c_int, vn: c_int, sh: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x4f400400u32
                | (((128 - sh) as u32) << 16)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_sshr_4s(b: *mut ffi::A64Buf, vd: c_int, vn: c_int, sh: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x4f200400u32
                | (((64 - sh) as u32) << 16)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_fcmeq(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(
            b,
            (if dbl != 0 {
                0x4e60e400u32
            } else {
                0x4e20e400u32
            }),
            vd,
            vn,
            vm,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_uminv_4s(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x6eb1a800u32 | (((vn & 31) as u32) << 5) | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_bit(b: *mut ffi::A64Buf, vd: c_int, vn: c_int, vm: c_int) -> () {
    unsafe {
        v3(b, 0x6ea01c00u32, vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_bif(b: *mut ffi::A64Buf, vd: c_int, vn: c_int, vm: c_int) -> () {
    unsafe {
        v3(b, 0x6ee01c00u32, vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fcmeq_s(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            (if dbl != 0 {
                0x5e60e400u32
            } else {
                0x5e20e400u32
            }) | (((vm & 31) as u32) << 16)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fcmgt_s(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            (if dbl != 0 {
                0x7ee0e400u32
            } else {
                0x7ea0e400u32
            }) | (((vm & 31) as u32) << 16)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fcmge_s(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            (if dbl != 0 {
                0x7e60e400u32
            } else {
                0x7e20e400u32
            }) | (((vm & 31) as u32) << 16)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_fcmge(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(
            b,
            (if dbl != 0 {
                0x6e60e400u32
            } else {
                0x6e20e400u32
            }),
            vd,
            vn,
            vm,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_not(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x6e205800u32 | (((vn & 31) as u32) << 5) | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_fcmgt(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(
            b,
            (if dbl != 0 {
                0x6ee0e400u32
            } else {
                0x6ea0e400u32
            }),
            vd,
            vn,
            vm,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fcsel(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
    cond: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            (if dbl != 0 {
                0x1e600c00u32
            } else {
                0x1e200c00u32
            }) | (((vm & 31) as u32) << 16)
                | (((cond & 15) as u32) << 12)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_smull_h(
    b: *mut ffi::A64Buf,
    hi: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(
            b,
            (if hi != 0 {
                0x4e60c000u32
            } else {
                0x0e60c000u32
            }),
            vd,
            vn,
            vm,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_umull_s(b: *mut ffi::A64Buf, vd: c_int, vn: c_int, vm: c_int) -> () {
    unsafe {
        v3(b, 0x2ea0c000u32, vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_addp_4s(b: *mut ffi::A64Buf, vd: c_int, vn: c_int, vm: c_int) -> () {
    unsafe {
        v3(b, 0x4ea0bc00u32, vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_sqxtn_s(b: *mut ffi::A64Buf, hi: c_int, vd: c_int, vn: c_int) -> () {
    unsafe {
        v3(
            b,
            (if hi != 0 {
                0x4e614800u32
            } else {
                0x0e614800u32
            }),
            vd,
            vn,
            0,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_sqxtun_h(
    b: *mut ffi::A64Buf,
    hi: c_int,
    vd: c_int,
    vn: c_int,
) -> () {
    unsafe {
        v3(
            b,
            (if hi != 0 {
                0x6e212800u32
            } else {
                0x2e212800u32
            }),
            vd,
            vn,
            0,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_rshrn_s15(
    b: *mut ffi::A64Buf,
    hi: c_int,
    vd: c_int,
    vn: c_int,
) -> () {
    unsafe {
        v3(
            b,
            (if hi != 0 {
                0x4f118c00u32
            } else {
                0x0f118c00u32
            }),
            vd,
            vn,
            0,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ins_h_h(
    b: *mut ffi::A64Buf,
    vd: c_int,
    i1: c_int,
    vn: c_int,
    i2: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x6e020400u32
                | (((i1 & 7) as u32) << 18)
                | (((i2 & 7) as u32) << 12)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_xtn(b: *mut ffi::A64Buf, esz: c_int, vd: c_int, vn: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x0e212800u32 | ((esz as u32) << 22) | (((vn & 31) as u32) << 5) | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_cmp_ext_sxtw(b: *mut ffi::A64Buf, rn: c_int, rm: c_int) -> () {
    unsafe {
        emit32(
            b,
            0xeb20c01fu32 | (((rm & 31) as u32) << 16) | (((rn & 31) as u32) << 5),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_subs_imm_sh12(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rd: c_int,
    rn: c_int,
    imm12: u32,
) -> () {
    unsafe {
        emit32(
            b,
            0x71400000u32
                | ((sf as u32) << 31)
                | ((imm12 & 0xfffu32) << 10)
                | (((rn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_cmn_imm(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rn: c_int,
    imm12: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x3100001fu32
                | ((sf as u32) << 31)
                | (((imm12 & 0xfff) as u32) << 10)
                | (((rn & 31) as u32) << 5),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ccmn_imm(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rn: c_int,
    imm5: c_int,
    nzcv: c_int,
    cond: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x3a400800u32
                | ((sf as u32) << 31)
                | (((imm5 & 31) as u32) << 16)
                | (((cond & 15) as u32) << 12)
                | (((rn & 31) as u32) << 5)
                | ((nzcv & 15) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ccmp_imm(
    b: *mut ffi::A64Buf,
    sf: c_int,
    rn: c_int,
    imm5: c_int,
    nzcv: c_int,
    cond: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x7a400800u32
                | ((sf as u32) << 31)
                | (((imm5 & 31) as u32) << 16)
                | (((cond & 15) as u32) << 12)
                | (((rn & 31) as u32) << 5)
                | ((nzcv & 15) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldr_v_regoff(
    b: *mut ffi::A64Buf,
    size: c_int,
    vt: c_int,
    rn: c_int,
    rm: c_int,
    scaled: c_int,
) -> () {
    unsafe {
        let mut base: u32 = (if size == 16 {
            0x3ce06800u32
        } else {
            (if size == 8 {
                0xfc606800u32
            } else {
                0xbc606800u32
            })
        });
        emit32(
            b,
            base | (((rm & 31) as u32) << 16)
                | (((scaled != 0) as u32) << 12)
                | (((rn & 31) as u32) << 5)
                | ((vt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_str_v_regoff(
    b: *mut ffi::A64Buf,
    size: c_int,
    vt: c_int,
    rn: c_int,
    rm: c_int,
    scaled: c_int,
) -> () {
    unsafe {
        let mut base: u32 = (if size == 16 {
            0x3ca06800u32
        } else {
            (if size == 8 {
                0xfc206800u32
            } else {
                0xbc206800u32
            })
        });
        emit32(
            b,
            base | (((rm & 31) as u32) << 16)
                | (((scaled != 0) as u32) << 12)
                | (((rn & 31) as u32) << 5)
                | ((vt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_rbit(b: *mut ffi::A64Buf, sf: c_int, rd: c_int, rn: c_int) -> () {
    unsafe {
        emit32(
            b,
            (if sf != 0 {
                0xdac00000u32
            } else {
                0x5ac00000u32
            }) | (((rn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_clz(b: *mut ffi::A64Buf, sf: c_int, rd: c_int, rn: c_int) -> () {
    unsafe {
        emit32(
            b,
            (if sf != 0 {
                0xdac01000u32
            } else {
                0x5ac01000u32
            }) | (((rn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_sshr_16b(
    b: *mut ffi::A64Buf,
    vd: c_int,
    vn: c_int,
    sh: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x4f000400u32
                | (((16 - sh) as u32) << 16)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_addp_16b(
    b: *mut ffi::A64Buf,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x4e20bc00u32
                | (((vm & 31) as u32) << 16)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_umov_w_h(b: *mut ffi::A64Buf, rd: c_int, vn: c_int, idx: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x0e023c00u32 | ((idx as u32) << 18) | (((vn & 31) as u32) << 5) | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_umov_w_b(b: *mut ffi::A64Buf, rd: c_int, vn: c_int, idx: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x0e013c00u32 | ((idx as u32) << 17) | (((vn & 31) as u32) << 5) | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_cnt_8b(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x0e205800u32 | (((vn & 31) as u32) << 5) | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_addv_b_8b(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x0e31b800u32 | (((vn & 31) as u32) << 5) | ((vd & 31) as u32),
        );
    }
}

#[inline(always)]
fn lse_size_bits(size: c_int) -> u32 {
    unsafe {
        if size == 8 {
            3
        } else if size == 4 {
            2
        } else if size == 2 {
            1
        } else {
            0
        }
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldop_al(
    b: *mut ffi::A64Buf,
    size: c_int,
    opc: c_int,
    rs: c_int,
    rt: c_int,
    rn: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x38e00000u32
                | (lse_size_bits(size) << 30)
                | (((rs & 31) as u32) << 16)
                | ((opc as u32) << 12)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_swpal(
    b: *mut ffi::A64Buf,
    size: c_int,
    rs: c_int,
    rt: c_int,
    rn: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x38e08000u32
                | (lse_size_bits(size) << 30)
                | (((rs & 31) as u32) << 16)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_casal(
    b: *mut ffi::A64Buf,
    size: c_int,
    rs: c_int,
    rt: c_int,
    rn: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x08e0fc00u32
                | (lse_size_bits(size) << 30)
                | (((rs & 31) as u32) << 16)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_neg_reg(b: *mut ffi::A64Buf, sf: c_int, rd: c_int, rm: c_int) -> () {
    unsafe {
        emit32(
            b,
            (if sf != 0 {
                0xcb0003e0u32
            } else {
                0x4b0003e0u32
            }) | (((rm & 31) as u32) << 16)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_mvn_reg(b: *mut ffi::A64Buf, sf: c_int, rd: c_int, rm: c_int) -> () {
    unsafe {
        emit32(
            b,
            (if sf != 0 {
                0xaa2003e0u32
            } else {
                0x2a2003e0u32
            }) | (((rm & 31) as u32) << 16)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_tbl1(b: *mut ffi::A64Buf, vd: c_int, vn: c_int, vm: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x4e000000u32
                | (((vm & 31) as u32) << 16)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_ext(
    b: *mut ffi::A64Buf,
    vd: c_int,
    vn: c_int,
    vm: c_int,
    idx: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x6e000000u32
                | (((vm & 31) as u32) << 16)
                | (((idx & 15) as u32) << 11)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_rev64_4s(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x4ea00800u32 | (((vn & 31) as u32) << 5) | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ins_gpr(
    b: *mut ffi::A64Buf,
    esize: c_int,
    vd: c_int,
    idx: c_int,
    rn: c_int,
) -> () {
    unsafe {
        let mut imm5: u32 = (if esize == 1 {
            ((idx as u32) << 1) | 1u32
        } else {
            (if esize == 2 {
                ((idx as u32) << 2) | 2u32
            } else {
                (if esize == 4 {
                    ((idx as u32) << 3) | 4u32
                } else {
                    ((idx as u32) << 4) | 8u32
                })
            })
        });
        emit32(
            b,
            0x4e001c00u32 | (imm5 << 16) | (((rn & 31) as u32) << 5) | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_umov_gpr(
    b: *mut ffi::A64Buf,
    esize: c_int,
    rd: c_int,
    vn: c_int,
    idx: c_int,
) -> () {
    unsafe {
        let mut imm5: u32 = (if esize == 1 {
            ((idx as u32) << 1) | 1u32
        } else {
            (if esize == 2 {
                ((idx as u32) << 2) | 2u32
            } else {
                (if esize == 4 {
                    ((idx as u32) << 3) | 4u32
                } else {
                    ((idx as u32) << 4) | 8u32
                })
            })
        });
        emit32(
            b,
            (if esize == 8 {
                0x4e003c00u32
            } else {
                0x0e003c00u32
            }) | (imm5 << 16)
                | (((vn & 31) as u32) << 5)
                | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_xtl(
    b: *mut ffi::A64Buf,
    is_signed: c_int,
    from: c_int,
    vd: c_int,
    vn: c_int,
) -> () {
    unsafe {
        let mut immh: u32 = (if from == 1 {
            0x08u32
        } else {
            (if from == 2 { 0x10u32 } else { 0x20u32 })
        });
        emit32(
            b,
            (if is_signed != 0 {
                0x0f00a400u32
            } else {
                0x2f00a400u32
            }) | (immh << 16)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_xtl2(
    b: *mut ffi::A64Buf,
    is_signed: c_int,
    from: c_int,
    vd: c_int,
    vn: c_int,
) -> () {
    unsafe {
        let mut immh: u32 = (if from == 1 {
            0x08u32
        } else {
            (if from == 2 { 0x10u32 } else { 0x20u32 })
        });
        emit32(
            b,
            (if is_signed != 0 {
                0x4f00a400u32
            } else {
                0x6f00a400u32
            }) | (immh << 16)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_umin(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x6e206c00u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_umax(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x6e206400u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_smin(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x4e206c00u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_smax(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x4e206400u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_mul(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x4e209c00u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_uqadd(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x6e200c00u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_uqsub(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x6e202c00u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_sqadd(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x4e200c00u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_sqsub(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x4e202c00u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_urhadd(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x6e201400u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_dup_b(b: *mut ffi::A64Buf, vd: c_int, vn: c_int, idx: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x4e010400u32
                | (((idx & 15) as u32) << 17)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_dup_h(b: *mut ffi::A64Buf, vd: c_int, vn: c_int, idx: c_int) -> () {
    unsafe {
        emit32(
            b,
            0x4e020400u32
                | (((idx & 7) as u32) << 18)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_dup_gpr(
    b: *mut ffi::A64Buf,
    esize: c_int,
    vd: c_int,
    rn: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x4e000c00u32 | ((esize as u32) << 16) | (((rn & 31) as u32) << 5) | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_shl_imm(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    sh: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x4f005400u32
                | (((8 << esz) + sh as u32) << 16)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_ushr_imm(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    sh: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x6f000400u32
                | (((16 << esz) - sh as u32) << 16)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_sshr_imm(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    sh: c_int,
) -> () {
    unsafe {
        emit32(
            b,
            0x4f000400u32
                | (((16 << esz) - sh as u32) << 16)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_fmadd_s(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    neg_mul: c_int,
    neg_add: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
    va: c_int,
) -> () {
    unsafe {
        let mut base: u32 = (if dbl != 0 {
            0x1f400000u32
        } else {
            0x1f000000u32
        }) | (if neg_add != 0 { 0x00200000u32 } else { 0 })
            | (if (neg_mul ^ neg_add) != 0 {
                0x00008000u32
            } else {
                0
            });
        emit32(
            b,
            base | (((vm & 31) as u32) << 16)
                | (((va & 31) as u32) << 10)
                | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_fneg(b: *mut ffi::A64Buf, dbl: c_int, vd: c_int, vn: c_int) -> () {
    unsafe {
        emit32(
            b,
            (if dbl != 0 {
                0x6ee0f800u32
            } else {
                0x6ea0f800u32
            }) | (((vn & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_fmla(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(
            b,
            (if dbl != 0 {
                0x4e60cc00u32
            } else {
                0x4e20cc00u32
            }),
            vd,
            vn,
            vm,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_fmls(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(
            b,
            (if dbl != 0 {
                0x4ee0cc00u32
            } else {
                0x4ea0cc00u32
            }),
            vd,
            vn,
            vm,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_frint_s(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    mode: c_int,
    vd: c_int,
    vn: c_int,
) -> () {
    unsafe {
        const OPC: [u32; 5] = [0x1e244000, 0x1e254000, 0x1e24c000, 0x1e25c000, 0x1e27c000];
        let opcode = *OPC.get_unchecked(mode as usize);
        let base = opcode | if dbl != 0 { 0x00400000 } else { 0 };
        emit32(b, base | (((vn & 31) as u32) << 5) | (vd & 31) as u32);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_frint(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    mode: c_int,
    vd: c_int,
    vn: c_int,
) -> () {
    unsafe {
        const OPC: [u32; 5] = [0x4e218800, 0x4e219800, 0x4ea18800, 0x4ea19800, 0x6ea19800];
        let opcode = *OPC.get_unchecked(mode as usize);
        let base = opcode | if dbl != 0 { 0x00400000 } else { 0 };
        emit32(b, base | (((vn & 31) as u32) << 5) | (vd & 31) as u32);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldapur(
    b: *mut ffi::A64Buf,
    size: c_int,
    rt: c_int,
    rn: c_int,
    simm9: i32,
) -> () {
    unsafe {
        let mut sz: u32 = ldst_size_bits(size);
        emit32(
            b,
            0x19400000u32
                | (sz << 30)
                | (((simm9 as u32) & 0x1ffu32) << 12)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_ldapurs(
    b: *mut ffi::A64Buf,
    size: c_int,
    sf: c_int,
    rt: c_int,
    rn: c_int,
    simm9: i32,
) -> () {
    unsafe {
        let mut sz: u32 = ldst_size_bits(size);
        let mut opc: u32 = (if size == 4 || sf != 0 { 2u32 } else { 3u32 });
        emit32(
            b,
            0x19000000u32
                | (sz << 30)
                | (opc << 22)
                | (((simm9 as u32) & 0x1ffu32) << 12)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_stlur(
    b: *mut ffi::A64Buf,
    size: c_int,
    rt: c_int,
    rn: c_int,
    simm9: i32,
) -> () {
    unsafe {
        let mut sz: u32 = ldst_size_bits(size);
        emit32(
            b,
            0x19000000u32
                | (sz << 30)
                | (((simm9 as u32) & 0x1ffu32) << 12)
                | (((rn & 31) as u32) << 5)
                | ((rt & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_dmb_ishld(b: *mut ffi::A64Buf) -> () {
    unsafe {
        emit32(b, 0xd50339bfu32);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_sshl(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x4e204400u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_ushl(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x6e204400u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_umull_h(
    b: *mut ffi::A64Buf,
    hi: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(
            b,
            (if hi != 0 {
                0x6e60c000u32
            } else {
                0x2e60c000u32
            }),
            vd,
            vn,
            vm,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_smull_s(b: *mut ffi::A64Buf, vd: c_int, vn: c_int, vm: c_int) -> () {
    unsafe {
        v3(b, 0x0ea0c000u32, vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_uzp(
    b: *mut ffi::A64Buf,
    esz: c_int,
    two: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(
            b,
            (if two != 0 {
                0x4e005800u32
            } else {
                0x4e001800u32
            }) | ((esz as u32) << 22),
            vd,
            vn,
            vm,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_trn(
    b: *mut ffi::A64Buf,
    esz: c_int,
    two: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(
            b,
            (if two != 0 {
                0x4e006800u32
            } else {
                0x4e002800u32
            }) | ((esz as u32) << 22),
            vd,
            vn,
            vm,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_sqxtn_h(b: *mut ffi::A64Buf, hi: c_int, vd: c_int, vn: c_int) -> () {
    unsafe {
        v3(
            b,
            (if hi != 0 {
                0x4e214800u32
            } else {
                0x0e214800u32
            }),
            vd,
            vn,
            0,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_sqxtun_s(
    b: *mut ffi::A64Buf,
    hi: c_int,
    vd: c_int,
    vn: c_int,
) -> () {
    unsafe {
        v3(
            b,
            (if hi != 0 {
                0x6e612800u32
            } else {
                0x2e612800u32
            }),
            vd,
            vn,
            0,
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_uabd(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x6e207400u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_uaddlp(b: *mut ffi::A64Buf, esz: c_int, vd: c_int, vn: c_int) -> () {
    unsafe {
        v3(b, 0x6e202800u32 | ((esz as u32) << 22), vd, vn, 0);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_abs(b: *mut ffi::A64Buf, esz: c_int, vd: c_int, vn: c_int) -> () {
    unsafe {
        v3(b, 0x4e20b800u32 | ((esz as u32) << 22), vd, vn, 0);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_neg(b: *mut ffi::A64Buf, esz: c_int, vd: c_int, vn: c_int) -> () {
    unsafe {
        v3(b, 0x6e20b800u32 | ((esz as u32) << 22), vd, vn, 0);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_cmeq0(b: *mut ffi::A64Buf, esz: c_int, vd: c_int, vn: c_int) -> () {
    unsafe {
        v3(b, 0x4e209800u32 | ((esz as u32) << 22), vd, vn, 0);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_addp(
    b: *mut ffi::A64Buf,
    esz: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(b, 0x4e20bc00u32 | ((esz as u32) << 22), vd, vn, vm);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_movi_s_lsl24(b: *mut ffi::A64Buf, vd: c_int, imm8: c_uint) -> () {
    unsafe {
        emit32(
            b,
            0x4f006400u32
                | (((imm8 >> 5 & 7) as u32) << 16)
                | (((imm8 & 31) as u32) << 5)
                | ((vd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_scvtf_2d(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        v3(b, 0x4e61d800u32, vd, vn, 0);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_addv_4s(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        v3(b, 0x4eb1b800u32, vd, vn, 0);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_addp_d(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        v3(b, 0x5ef1b800u32, vd, vn, 0);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_crc32c(
    b: *mut ffi::A64Buf,
    size: c_int,
    rd: c_int,
    rn: c_int,
    rm: c_int,
) -> () {
    unsafe {
        let mut base: u32 = (if size == 1 {
            0x1ac05000u32
        } else {
            (if size == 2 {
                0x1ac05400u32
            } else {
                (if size == 4 {
                    0x1ac05800u32
                } else {
                    0x9ac05c00u32
                })
            })
        });
        emit32(
            b,
            base | (((rm & 31) as u32) << 16) | (((rn & 31) as u32) << 5) | ((rd & 31) as u32),
        );
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_aese(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        v3(b, 0x4e284800u32, vd, vn, 0);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_aesd(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        v3(b, 0x4e285800u32, vd, vn, 0);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_aesmc(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        v3(b, 0x4e286800u32, vd, vn, 0);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_aesimc(b: *mut ffi::A64Buf, vd: c_int, vn: c_int) -> () {
    unsafe {
        v3(b, 0x4e287800u32, vd, vn, 0);
    }
}

#[inline]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a64_v_pmull_d(
    b: *mut ffi::A64Buf,
    hi: c_int,
    vd: c_int,
    vn: c_int,
    vm: c_int,
) -> () {
    unsafe {
        v3(
            b,
            (if hi != 0 {
                0x4ee0e000u32
            } else {
                0x0ee0e000u32
            }),
            vd,
            vn,
            vm,
        );
    }
}

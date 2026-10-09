//! Reimplementations of the `static inline` helpers that live in the public
//! headers. bindgen does not emit bindings for them, so any ported module that
//! needs one uses these instead. Keep them byte-for-byte faithful to the C.
#![allow(dead_code)]

use crate::ffi::OcerzCPU;

pub const OCERZ_CF: u64 = 1 << 0;
pub const OCERZ_FLAG_FIXED1: u64 = 1 << 1;
pub const OCERZ_PF: u64 = 1 << 2;
pub const OCERZ_AF: u64 = 1 << 4;
pub const OCERZ_ZF: u64 = 1 << 6;
pub const OCERZ_SF: u64 = 1 << 7;
pub const OCERZ_TF: u64 = 1 << 8;
pub const OCERZ_IF: u64 = 1 << 9;
pub const OCERZ_DF: u64 = 1 << 10;
pub const OCERZ_OF: u64 = 1 << 11;
pub const OCERZ_NT: u64 = 1 << 14;
pub const OCERZ_RF: u64 = 1 << 16;
pub const OCERZ_AC: u64 = 1 << 18;

#[inline(always)]
pub fn ocerz_mask(size: i32) -> u64 {
    if size >= 8 {
        !0u64
    } else {
        (1u64 << (size * 8)) - 1
    }
}

#[inline(always)]
pub fn ocerz_trunc(v: u64, size: i32) -> u64 {
    v & ocerz_mask(size)
}

#[inline(always)]
pub fn ocerz_sext(v: u64, size: i32) -> i64 {
    let shift = 64 - size * 8;
    ((v << shift) as i64) >> shift
}

#[inline(always)]
pub fn ocerz_msb(v: u64, size: i32) -> i32 {
    ((v >> (size * 8 - 1)) & 1) as i32
}

#[inline(always)]
pub unsafe fn ocerz_flag_assign(cpu: *mut OcerzCPU, mask: u64, set: i32) {
    unsafe {
        if set != 0 {
            (*cpu).rflags |= mask;
        } else {
            (*cpu).rflags &= !mask;
        }
    }
}

#[inline(always)]
pub fn ocerz_cc_pack(kind: u32, size: i32, cin: i32) -> u32 {
    kind | ((size as u32) << 8) | (((cin & 1) as u32) << 16)
}

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

#[inline(always)]
pub unsafe fn ocerz_g2h(gaddr: u64) -> *mut core::ffi::c_void {
    unsafe {
        if !crate::ffi::ocerz_commpage.is_null()
            && gaddr >= crate::ffi::OCERZ_COMMPAGE_LO
            && gaddr < crate::ffi::OCERZ_COMMPAGE_HI
        {
            return crate::ffi::ocerz_commpage
                .add((gaddr - crate::ffi::OCERZ_COMMPAGE_LO) as usize)
                .cast();
        }
        if crate::ffi::ocerz_low_base != 0 {
            if gaddr < crate::ffi::OCERZ_LOW_LIMIT {
                if (gaddr < crate::ffi::OCERZ_NULL_LIMIT as u64
                    && !crate::ffi::ocerz_pin_map.is_null())
                    || (!crate::ffi::ocerz_pin_map.is_null()
                        && ((*crate::ffi::ocerz_pin_map.add((gaddr >> 17) as usize)
                            >> ((gaddr >> 14) & 7))
                            & 1)
                            != 0)
                {
                    return gaddr as *mut core::ffi::c_void;
                }
                return gaddr.wrapping_add(crate::ffi::ocerz_low_base) as *mut core::ffi::c_void;
            }
            if gaddr.wrapping_sub(crate::ffi::OCERZ_TOP_LO)
                < crate::ffi::OCERZ_TOP_HI - crate::ffi::OCERZ_TOP_LO
            {
                return gaddr
                    .wrapping_sub(crate::ffi::OCERZ_TOP_LO)
                    .wrapping_add(crate::ffi::ocerz_top_base)
                    as *mut core::ffi::c_void;
            }
        }
        gaddr.wrapping_add(crate::ffi::ocerz_guest_base) as *mut core::ffi::c_void
    }
}

#[inline(always)]
pub unsafe fn ocerz_h2g(haddr: *const core::ffi::c_void) -> u64 {
    unsafe {
        let h = haddr as u64;
        if crate::ffi::ocerz_low_base != 0 {
            if h.wrapping_sub(crate::ffi::ocerz_low_base) < crate::ffi::OCERZ_LOW_LIMIT {
                return h.wrapping_sub(crate::ffi::ocerz_low_base);
            }
            if h.wrapping_sub(crate::ffi::ocerz_top_base)
                < crate::ffi::OCERZ_TOP_HI - crate::ffi::OCERZ_TOP_LO
            {
                return h
                    .wrapping_sub(crate::ffi::ocerz_top_base)
                    .wrapping_add(crate::ffi::OCERZ_TOP_LO);
            }
        }
        h.wrapping_sub(crate::ffi::ocerz_guest_base)
    }
}

#[inline(always)]
pub unsafe fn ocerz_ld(gaddr: u64, size: i32) -> u64 {
    unsafe {
        let p = ocerz_g2h(gaddr).cast::<u8>();
        match size {
            1 => (&*p.cast::<core::sync::atomic::AtomicU8>())
                .load(core::sync::atomic::Ordering::Acquire)
                as u64,
            2 if (p as usize) & 1 == 0 => (&*p.cast::<core::sync::atomic::AtomicU16>())
                .load(core::sync::atomic::Ordering::Acquire)
                as u64,
            4 if (p as usize) & 3 == 0 => (&*p.cast::<core::sync::atomic::AtomicU32>())
                .load(core::sync::atomic::Ordering::Acquire)
                as u64,
            8 if (p as usize) & 7 == 0 => {
                (&*p.cast::<core::sync::atomic::AtomicU64>())
                    .load(core::sync::atomic::Ordering::Acquire)
            }
            _ => {
                let mut v = 0u64;
                core::ptr::copy_nonoverlapping(
                    p,
                    (&mut v as *mut u64).cast::<u8>(),
                    size as usize,
                );
                core::sync::atomic::fence(core::sync::atomic::Ordering::Acquire);
                v
            }
        }
    }
}

#[inline(always)]
pub unsafe fn ocerz_st(gaddr: u64, size: i32, v: u64) {
    unsafe {
        if crate::ffi::ocerz_watch_addr != 0
            && gaddr < crate::ffi::ocerz_watch_addr.wrapping_add(crate::ffi::ocerz_watch_len)
            && gaddr.wrapping_add(size as u64) > crate::ffi::ocerz_watch_addr
        {
            crate::ffi::ocerz_watch_hit(gaddr, size, v, 0);
        }
        if crate::ffi::ocerz_watch_val != 0 && v == crate::ffi::ocerz_watch_val {
            crate::ffi::ocerz_watch_hit(gaddr, size, v, 0);
        }
        if crate::ffi::ocerz_watch_shadow != 0
            && crate::ffi::ocerz_low_base != 0
            && size == 8
            && v.wrapping_sub(crate::ffi::ocerz_low_base) < crate::ffi::OCERZ_LOW_LIMIT
        {
            crate::ffi::ocerz_watch_hit(gaddr, size, v, 0);
        }
        let p = ocerz_g2h(gaddr).cast::<u8>();
        match size {
            1 => (&*p.cast::<core::sync::atomic::AtomicU8>())
                .store(v as u8, core::sync::atomic::Ordering::Release),
            2 if (p as usize) & 1 == 0 => (&*p.cast::<core::sync::atomic::AtomicU16>())
                .store(v as u16, core::sync::atomic::Ordering::Release),
            4 if (p as usize) & 3 == 0 => (&*p.cast::<core::sync::atomic::AtomicU32>())
                .store(v as u32, core::sync::atomic::Ordering::Release),
            8 if (p as usize) & 7 == 0 => (&*p.cast::<core::sync::atomic::AtomicU64>())
                .store(v, core::sync::atomic::Ordering::Release),
            _ => {
                core::sync::atomic::fence(core::sync::atomic::Ordering::Release);
                core::ptr::copy_nonoverlapping(
                    (&v as *const u64).cast::<u8>(),
                    p,
                    size as usize,
                );
            }
        }
    }
}

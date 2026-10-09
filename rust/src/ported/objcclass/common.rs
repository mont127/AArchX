//! Shared helpers the two halves of the module use: the mem.h static inline
//! load/store and address-conversion helpers reimplemented as in
//! src/ported/sysbridge/common.rs, the small guest-word readers built on them,
//! the OCERZ_OBJCLOG lazy environment check, and the fatal oc_stop tail that
//! prints and exits.

use core::ffi::{c_char, c_int, c_void};
use core::sync::atomic::{AtomicU8, AtomicU16, AtomicU32, AtomicU64, Ordering, fence};

use crate::ffi::*;

pub const OC_RO_META: u32 = 0x1;
pub const OC_RO_ROOT: u32 = 0x2;
pub const OC_RO_SWIFT_INIT: u32 = 0x40;
pub const OC_RO_FUTURE: u32 = 0x4000_0000;
pub const OC_RO_REALIZED: u32 = 0x8000_0000;
pub const OC_FAST_SWIFT: u64 = 0x3;
pub const OC_SWIFT_STABLE: u64 = 0x2;
pub const OC_SWIFT_RC: u32 = 0x2;
pub const OC_FAST_DATA: u64 = 0x0000_7fff_ffff_fff8;
pub const OC_METHOD_FLAGS: u32 = 0xffff_0003;
pub const OC_METHOD_RELATIVE: u32 = 0x8000_0000;
pub const OC_METHOD_DIRECT_SEL: u32 = 0x4000_0000;
pub const OC_IMAGE_CLASS_PROPERTIES: u32 = 0x40;
pub const OC_RO_BYTES: usize = 72;
pub const OC_PROTOCOL_BASE: usize = 72;
pub const OC_ATTRS_MAX: usize = 32;

#[inline(always)]
pub unsafe fn ocerz_pinned_page(gaddr: u64) -> bool {
    unsafe {
        !ocerz_pin_map.is_null()
            && gaddr < OCERZ_LOW_LIMIT
            && ((*ocerz_pin_map.add((gaddr >> 17) as usize) >> ((gaddr >> 14) & 7)) & 1) != 0
    }
}

#[inline(always)]
pub unsafe fn ocerz_g2h(gaddr: u64) -> *mut c_void {
    unsafe {
        if !ocerz_commpage.is_null() && gaddr >= OCERZ_COMMPAGE_LO && gaddr < OCERZ_COMMPAGE_HI {
            return ocerz_commpage
                .add((gaddr - OCERZ_COMMPAGE_LO) as usize)
                .cast::<c_void>();
        }
        if ocerz_low_base != 0 {
            if gaddr < OCERZ_LOW_LIMIT {
                if (gaddr < OCERZ_NULL_LIMIT as u64 && !ocerz_pin_map.is_null())
                    || ocerz_pinned_page(gaddr)
                {
                    return gaddr as *mut c_void;
                }
                return gaddr.wrapping_add(ocerz_low_base) as *mut c_void;
            }
            if gaddr.wrapping_sub(OCERZ_TOP_LO) < OCERZ_TOP_HI - OCERZ_TOP_LO {
                return gaddr.wrapping_sub(OCERZ_TOP_LO).wrapping_add(ocerz_top_base)
                    as *mut c_void;
            }
        }
        gaddr.wrapping_add(ocerz_guest_base) as *mut c_void
    }
}

#[inline(always)]
pub unsafe fn ocerz_h2g(haddr: *const c_void) -> u64 {
    unsafe {
        let h = haddr as u64;
        if ocerz_low_base != 0 {
            if h.wrapping_sub(ocerz_low_base) < OCERZ_LOW_LIMIT {
                return h.wrapping_sub(ocerz_low_base);
            }
            if h.wrapping_sub(ocerz_top_base) < OCERZ_TOP_HI - OCERZ_TOP_LO {
                return h.wrapping_sub(ocerz_top_base).wrapping_add(OCERZ_TOP_LO);
            }
        }
        h.wrapping_sub(ocerz_guest_base)
    }
}

#[inline(always)]
pub unsafe fn ocerz_host_in_guest_reservation(haddr: *const c_void) -> c_int {
    unsafe {
        let h = haddr as u64;
        if ocerz_low_base != 0 {
            if h.wrapping_sub(ocerz_low_base) < OCERZ_LOW_LIMIT {
                return 1;
            }
            if h.wrapping_sub(ocerz_top_base) < OCERZ_TOP_HI - OCERZ_TOP_LO {
                return 1;
            }
        }
        let g = h.wrapping_sub(ocerz_guest_base);
        (g >= ocerz_arena_lo && g < ocerz_arena_hi) as c_int
    }
}

#[inline(always)]
pub unsafe fn ocerz_ld(gaddr: u64, size: c_int) -> u64 {
    unsafe {
        let p = ocerz_g2h(gaddr);
        match size {
            1 => return (*p.cast::<AtomicU8>()).load(Ordering::Acquire) as u64,
            2 if (p as usize) & 1 == 0 => {
                return (*p.cast::<AtomicU16>()).load(Ordering::Acquire) as u64
            }
            4 if (p as usize) & 3 == 0 => {
                return (*p.cast::<AtomicU32>()).load(Ordering::Acquire) as u64
            }
            8 if (p as usize) & 7 == 0 => {
                return (*p.cast::<AtomicU64>()).load(Ordering::Acquire)
            }
            _ => {}
        }
        let mut v = 0u64;
        core::ptr::copy_nonoverlapping(p.cast::<u8>(), &mut v as *mut u64 as *mut u8, size as usize);
        fence(Ordering::Acquire);
        v
    }
}

#[inline(always)]
pub unsafe fn ocerz_st(gaddr: u64, size: c_int, v: u64) {
    unsafe {
        if ocerz_watch_addr != 0
            && gaddr < ocerz_watch_addr.wrapping_add(ocerz_watch_len)
            && gaddr.wrapping_add(size as u64) > ocerz_watch_addr
        {
            ocerz_watch_hit(gaddr, size, v, 0);
        }
        if ocerz_watch_val != 0 && v == ocerz_watch_val {
            ocerz_watch_hit(gaddr, size, v, 0);
        }
        if ocerz_watch_shadow != 0
            && ocerz_low_base != 0
            && size == 8
            && v.wrapping_sub(ocerz_low_base) < OCERZ_LOW_LIMIT
        {
            ocerz_watch_hit(gaddr, size, v, 0);
        }
        let p = ocerz_g2h(gaddr);
        match size {
            1 => {
                (*p.cast::<AtomicU8>()).store(v as u8, Ordering::Release);
                return;
            }
            2 if (p as usize) & 1 == 0 => {
                (*p.cast::<AtomicU16>()).store(v as u16, Ordering::Release);
                return;
            }
            4 if (p as usize) & 3 == 0 => {
                (*p.cast::<AtomicU32>()).store(v as u32, Ordering::Release);
                return;
            }
            8 if (p as usize) & 7 == 0 => {
                (*p.cast::<AtomicU64>()).store(v, Ordering::Release);
                return;
            }
            _ => {}
        }
        fence(Ordering::Release);
        core::ptr::copy_nonoverlapping(&v as *const u64 as *const u8, p.cast::<u8>(), size as usize);
    }
}

#[inline(always)]
pub unsafe fn oc_word(addr: u64) -> u64 {
    unsafe { ocerz_ld(addr, 8) }
}

#[inline(always)]
pub unsafe fn oc_u32(addr: u64) -> u32 {
    unsafe { ocerz_ld(addr, 4) as u32 }
}

#[inline(always)]
pub unsafe fn oc_rel(addr: u64) -> i32 {
    unsafe { (ocerz_ld(addr, 4) as u32) as i32 }
}

#[inline(always)]
pub unsafe fn oc_str(addr: u64) -> *const c_char {
    unsafe {
        if addr != 0 {
            ocerz_g2h(addr) as *const c_char
        } else {
            core::ptr::null()
        }
    }
}

#[inline(always)]
pub unsafe fn oc_is_guest(addr: u64) -> bool {
    unsafe { addr != 0 && ocerz_host_in_guest_reservation(ocerz_g2h(addr)) != 0 }
}

pub fn oc_logging() -> bool {
    static EN: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
    let mut en = EN.load(Ordering::Relaxed);
    if en < 0 {
        en = unsafe { !libc::getenv(c"OCERZ_OBJCLOG".as_ptr()).is_null() } as i32;
        EN.store(en, Ordering::Relaxed);
    }
    en != 0
}

macro_rules! oc_stop {
    ($fmt:literal $(, $arg:expr)*) => {{
        unsafe {
            ::libc::fputs(c"ocerz: bridge: ".as_ptr(), $crate::log::stderr());
            ::libc::fprintf($crate::log::stderr(), concat!($fmt, "\n\0").as_ptr() as *const ::core::ffi::c_char $(, $arg)*);
            ::libc::fflush($crate::log::stderr());
            ::libc::exit($crate::ffi::OCERZ_BRIDGE_UNIMPL_EXIT as ::core::ffi::c_int)
        }
    }};
}

pub(crate) use oc_stop;

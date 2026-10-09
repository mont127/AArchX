//! Guest and host address conversion plus the ordered guest-memory primitives used by dyldapi.

use core::ptr;
use core::sync::atomic::{AtomicU8, AtomicU16, AtomicU32, AtomicU64, Ordering};

use crate::ffi::{
    ocerz_commpage, ocerz_guest_base, ocerz_low_base, ocerz_pin_map, ocerz_top_base,
    ocerz_watch_addr, ocerz_watch_hit, ocerz_watch_len, ocerz_watch_shadow, ocerz_watch_val,
};

const COMMPAGE_LO: u64 = 0x00007fffffe00000;
const COMMPAGE_HI: u64 = 0x00007fffffe04000;
const LOW_LIMIT: u64 = 0x0000000300000000;
const NULL_LIMIT: u64 = 0x0000000000010000;
const TOP_LO: u64 = 0x00007ffffe000000;
const TOP_HI: u64 = 0x00007fffffe00000;

#[inline(always)]
unsafe fn pinned_page(gaddr: u64) -> bool {
    unsafe {
        let pin_map = ocerz_pin_map;
        !pin_map.is_null()
            && gaddr < LOW_LIMIT
            && (*pin_map.add((gaddr >> 17) as usize) >> ((gaddr >> 14) & 7)) & 1 != 0
    }
}

#[inline(always)]
pub(crate) unsafe fn ocerz_g2h(gaddr: u64) -> *mut libc::c_void {
    unsafe {
        let commpage = ocerz_commpage;
        if !commpage.is_null() && (COMMPAGE_LO..COMMPAGE_HI).contains(&gaddr) {
            return commpage.add((gaddr - COMMPAGE_LO) as usize).cast();
        }
        let low_base = ocerz_low_base;
        if low_base != 0 {
            if gaddr < LOW_LIMIT {
                if (gaddr < NULL_LIMIT && !ocerz_pin_map.is_null()) || pinned_page(gaddr) {
                    return gaddr as *mut libc::c_void;
                }
                return gaddr.wrapping_add(low_base) as *mut libc::c_void;
            }
            if gaddr.wrapping_sub(TOP_LO) < TOP_HI - TOP_LO {
                return gaddr.wrapping_sub(TOP_LO).wrapping_add(ocerz_top_base)
                    as *mut libc::c_void;
            }
        }
        gaddr.wrapping_add(ocerz_guest_base) as *mut libc::c_void
    }
}

#[inline(always)]
pub(crate) unsafe fn ocerz_h2g(haddr: *const libc::c_void) -> u64 {
    unsafe {
        let h = haddr as u64;
        let low_base = ocerz_low_base;
        if low_base != 0 {
            if h.wrapping_sub(low_base) < LOW_LIMIT {
                return h.wrapping_sub(low_base);
            }
            if h.wrapping_sub(ocerz_top_base) < TOP_HI - TOP_LO {
                return h.wrapping_sub(ocerz_top_base).wrapping_add(TOP_LO);
            }
        }
        h.wrapping_sub(ocerz_guest_base)
    }
}

#[inline(always)]
pub(crate) unsafe fn ocerz_ld(gaddr: u64, size: i32) -> u64 {
    unsafe {
        let p = ocerz_g2h(gaddr).cast::<u8>();
        match size {
            1 => (&*p.cast::<AtomicU8>()).load(Ordering::Acquire) as u64,
            2 if (p as usize) & 1 == 0 => (&*p.cast::<AtomicU16>()).load(Ordering::Acquire) as u64,
            4 if (p as usize) & 3 == 0 => (&*p.cast::<AtomicU32>()).load(Ordering::Acquire) as u64,
            8 if (p as usize) & 7 == 0 => (&*p.cast::<AtomicU64>()).load(Ordering::Acquire),
            _ => {
                let mut v = 0u64;
                ptr::copy_nonoverlapping(p, (&mut v as *mut u64).cast::<u8>(), size as usize);
                core::sync::atomic::fence(Ordering::Acquire);
                v
            }
        }
    }
}

#[inline(always)]
pub(crate) unsafe fn ocerz_st(gaddr: u64, size: i32, value: u64) {
    unsafe {
        let watch_addr = ocerz_watch_addr;
        let watch_len = ocerz_watch_len;
        if watch_addr != 0
            && gaddr < watch_addr.wrapping_add(watch_len)
            && gaddr.wrapping_add(size as u64) > watch_addr
        {
            ocerz_watch_hit(gaddr, size, value, 0);
        }
        if ocerz_watch_val != 0 && value == ocerz_watch_val {
            ocerz_watch_hit(gaddr, size, value, 0);
        }
        let low_base = ocerz_low_base;
        if ocerz_watch_shadow != 0
            && low_base != 0
            && size == 8
            && value.wrapping_sub(low_base) < LOW_LIMIT
        {
            ocerz_watch_hit(gaddr, size, value, 0);
        }
        let p = ocerz_g2h(gaddr).cast::<u8>();
        match size {
            1 => (&*p.cast::<AtomicU8>()).store(value as u8, Ordering::Release),
            2 if (p as usize) & 1 == 0 => {
                (&*p.cast::<AtomicU16>()).store(value as u16, Ordering::Release)
            }
            4 if (p as usize) & 3 == 0 => {
                (&*p.cast::<AtomicU32>()).store(value as u32, Ordering::Release)
            }
            8 if (p as usize) & 7 == 0 => (&*p.cast::<AtomicU64>()).store(value, Ordering::Release),
            _ => {
                core::sync::atomic::fence(Ordering::Release);
                ptr::copy_nonoverlapping((&value as *const u64).cast::<u8>(), p, size as usize);
            }
        }
    }
}

#[inline(always)]
pub(crate) unsafe fn rd16(p: *const u8) -> u16 {
    unsafe { ptr::read_unaligned(p.cast()) }
}

#[inline(always)]
pub(crate) unsafe fn rd32(p: *const u8) -> u32 {
    unsafe { ptr::read_unaligned(p.cast()) }
}

#[inline(always)]
pub(crate) unsafe fn rd64(p: *const u8) -> u64 {
    unsafe { ptr::read_unaligned(p.cast()) }
}

#[inline(always)]
pub(crate) unsafe fn wr64(p: *mut u8, value: u64) {
    unsafe { ptr::write_unaligned(p.cast(), value) }
}

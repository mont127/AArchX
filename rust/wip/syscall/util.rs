//! Byte-faithful copies of the memory and CPU inline helpers used by the
//! syscall implementation, plus per-call-site environment caching.

use core::sync::atomic::{AtomicU8, AtomicU16, AtomicU32, AtomicU64, Ordering};

use crate::ffi::Ocerz128;

const OCERZ_COMMPAGE_LO: u64 = 0x00007fffffe00000;
const OCERZ_COMMPAGE_HI: u64 = 0x00007fffffe04000;
const OCERZ_LOW_LIMIT: u64 = 0x0000000300000000;
const OCERZ_NULL_LIMIT: u64 = 0x0000000000010000;
const OCERZ_TOP_LO: u64 = 0x00007ffffe000000;
const OCERZ_TOP_HI: u64 = 0x00007fffffe00000;

#[inline(always)]
pub(super) unsafe fn ocerz_pinned_page(gaddr: u64) -> bool {
    unsafe {
        let map = crate::ffi::ocerz_pin_map;
        !map.is_null()
            && gaddr < OCERZ_LOW_LIMIT
            && ((*map.add((gaddr >> 17) as usize) >> ((gaddr >> 14) & 7)) & 1) != 0
    }
}

#[inline(always)]
pub(super) unsafe fn ocerz_g2h(gaddr: u64) -> *mut core::ffi::c_void {
    unsafe {
        let commpage = crate::ffi::ocerz_commpage;
        if !commpage.is_null() && (OCERZ_COMMPAGE_LO..OCERZ_COMMPAGE_HI).contains(&gaddr) {
            return commpage.add((gaddr - OCERZ_COMMPAGE_LO) as usize).cast();
        }
        let low_base = crate::ffi::ocerz_low_base;
        if low_base != 0 {
            if gaddr < OCERZ_LOW_LIMIT {
                if (gaddr < OCERZ_NULL_LIMIT && !crate::ffi::ocerz_pin_map.is_null())
                    || ocerz_pinned_page(gaddr)
                {
                    return gaddr as usize as *mut core::ffi::c_void;
                }
                return gaddr.wrapping_add(low_base) as usize as *mut core::ffi::c_void;
            }
            if gaddr.wrapping_sub(OCERZ_TOP_LO) < OCERZ_TOP_HI - OCERZ_TOP_LO {
                return gaddr
                    .wrapping_sub(OCERZ_TOP_LO)
                    .wrapping_add(crate::ffi::ocerz_top_base) as usize
                    as *mut core::ffi::c_void;
            }
        }
        gaddr.wrapping_add(crate::ffi::ocerz_guest_base) as usize as *mut core::ffi::c_void
    }
}

#[inline(always)]
pub(super) unsafe fn ocerz_h2g(haddr: *const core::ffi::c_void) -> u64 {
    unsafe {
        let h = haddr as usize as u64;
        let low_base = crate::ffi::ocerz_low_base;
        if low_base != 0 {
            if h.wrapping_sub(low_base) < OCERZ_LOW_LIMIT {
                return h.wrapping_sub(low_base);
            }
            if h.wrapping_sub(crate::ffi::ocerz_top_base) < OCERZ_TOP_HI - OCERZ_TOP_LO {
                return h
                    .wrapping_sub(crate::ffi::ocerz_top_base)
                    .wrapping_add(OCERZ_TOP_LO);
            }
        }
        h.wrapping_sub(crate::ffi::ocerz_guest_base)
    }
}

#[inline(always)]
pub(super) unsafe fn ocerz_host_in_guest_space(haddr: *const core::ffi::c_void) -> bool {
    unsafe {
        let h = haddr as usize as u64;
        let commpage = crate::ffi::ocerz_commpage;
        if !commpage.is_null() {
            let c = commpage as usize as u64;
            if h.wrapping_sub(c) < OCERZ_COMMPAGE_HI - OCERZ_COMMPAGE_LO {
                return true;
            }
            if crate::ffi::ocerz_guest_base == 0
                && h.wrapping_sub(OCERZ_COMMPAGE_LO) < OCERZ_COMMPAGE_HI - OCERZ_COMMPAGE_LO
            {
                return true;
            }
        }
        let low_base = crate::ffi::ocerz_low_base;
        if low_base != 0 {
            if h.wrapping_sub(low_base) < OCERZ_LOW_LIMIT
                || h.wrapping_sub(crate::ffi::ocerz_top_base) < OCERZ_TOP_HI - OCERZ_TOP_LO
                || h.wrapping_sub(OCERZ_TOP_LO) < OCERZ_COMMPAGE_HI - OCERZ_TOP_LO
            {
                return true;
            }
        }
        h.wrapping_sub(crate::ffi::ocerz_guest_base) < crate::ffi::ocerz_arena_hi
    }
}

#[inline(always)]
pub(super) unsafe fn ocerz_host_in_guest_reservation(haddr: *const core::ffi::c_void) -> bool {
    unsafe {
        let h = haddr as usize as u64;
        let low_base = crate::ffi::ocerz_low_base;
        if low_base != 0
            && (h.wrapping_sub(low_base) < OCERZ_LOW_LIMIT
                || h.wrapping_sub(crate::ffi::ocerz_top_base) < OCERZ_TOP_HI - OCERZ_TOP_LO)
        {
            return true;
        }
        let g = h.wrapping_sub(crate::ffi::ocerz_guest_base);
        g >= crate::ffi::ocerz_arena_lo && g < crate::ffi::ocerz_arena_hi
    }
}

#[inline(always)]
pub(super) unsafe fn ocerz_ld(gaddr: u64, size: i32) -> u64 {
    unsafe {
        let p = ocerz_g2h(gaddr).cast::<u8>();
        match size {
            1 => (&*p.cast::<AtomicU8>()).load(Ordering::Acquire) as u64,
            2 if (p as usize & 1) == 0 => (&*p.cast::<AtomicU16>()).load(Ordering::Acquire) as u64,
            4 if (p as usize & 3) == 0 => (&*p.cast::<AtomicU32>()).load(Ordering::Acquire) as u64,
            8 if (p as usize & 7) == 0 => (&*p.cast::<AtomicU64>()).load(Ordering::Acquire),
            _ => {
                let mut value = 0u64;
                core::ptr::copy_nonoverlapping(
                    p,
                    (&mut value as *mut u64).cast::<u8>(),
                    size as usize,
                );
                core::sync::atomic::fence(Ordering::Acquire);
                value
            }
        }
    }
}

#[inline(always)]
pub(super) unsafe fn ocerz_st(gaddr: u64, size: i32, value: u64) {
    unsafe {
        if crate::ffi::ocerz_watch_addr != 0
            && gaddr < crate::ffi::ocerz_watch_addr.wrapping_add(crate::ffi::ocerz_watch_len)
            && gaddr.wrapping_add(size as u64) > crate::ffi::ocerz_watch_addr
        {
            crate::ffi::ocerz_watch_hit(gaddr, size, value, 0);
        }
        if crate::ffi::ocerz_watch_val != 0 && value == crate::ffi::ocerz_watch_val {
            crate::ffi::ocerz_watch_hit(gaddr, size, value, 0);
        }
        if crate::ffi::ocerz_watch_shadow != 0
            && crate::ffi::ocerz_low_base != 0
            && size == 8
            && value.wrapping_sub(crate::ffi::ocerz_low_base) < OCERZ_LOW_LIMIT
        {
            crate::ffi::ocerz_watch_hit(gaddr, size, value, 0);
        }
        let p = ocerz_g2h(gaddr).cast::<u8>();
        match size {
            1 => (&*p.cast::<AtomicU8>()).store(value as u8, Ordering::Release),
            2 if (p as usize & 1) == 0 => {
                (&*p.cast::<AtomicU16>()).store(value as u16, Ordering::Release)
            }
            4 if (p as usize & 3) == 0 => {
                (&*p.cast::<AtomicU32>()).store(value as u32, Ordering::Release)
            }
            8 if (p as usize & 7) == 0 => (&*p.cast::<AtomicU64>()).store(value, Ordering::Release),
            _ => {
                core::sync::atomic::fence(Ordering::Release);
                core::ptr::copy_nonoverlapping(
                    (&value as *const u64).cast::<u8>(),
                    p,
                    size as usize,
                );
            }
        }
    }
}

#[inline(always)]
pub(super) unsafe fn ocerz_ld128(gaddr: u64) -> Ocerz128 {
    unsafe {
        let p = ocerz_g2h(gaddr).cast::<u8>();
        if p as usize & 7 == 0 {
            Ocerz128 {
                lo: (&*p.cast::<AtomicU64>()).load(Ordering::Acquire),
                hi: (&*p.add(8).cast::<AtomicU64>()).load(Ordering::Acquire),
            }
        } else {
            let mut value = Ocerz128 { lo: 0, hi: 0 };
            core::ptr::copy_nonoverlapping(p, (&mut value as *mut Ocerz128).cast(), 16);
            core::sync::atomic::fence(Ordering::Acquire);
            value
        }
    }
}

#[inline(always)]
pub(super) unsafe fn ocerz_st128(gaddr: u64, value: Ocerz128) {
    unsafe {
        if crate::ffi::ocerz_watch_addr != 0
            && crate::ffi::ocerz_watch_addr.wrapping_sub(gaddr) < 16
        {
            crate::ffi::ocerz_watch_hit(gaddr, 16, value.lo, value.hi);
        }
        if crate::ffi::ocerz_watch_val != 0
            && (value.lo == crate::ffi::ocerz_watch_val || value.hi == crate::ffi::ocerz_watch_val)
        {
            crate::ffi::ocerz_watch_hit(gaddr, 16, value.lo, value.hi);
        }
        if crate::ffi::ocerz_watch_shadow != 0
            && crate::ffi::ocerz_low_base != 0
            && (value.lo.wrapping_sub(crate::ffi::ocerz_low_base) < OCERZ_LOW_LIMIT
                || value.hi.wrapping_sub(crate::ffi::ocerz_low_base) < OCERZ_LOW_LIMIT)
        {
            crate::ffi::ocerz_watch_hit(gaddr, 16, value.lo, value.hi);
        }
        let p = ocerz_g2h(gaddr).cast::<u8>();
        if p as usize & 7 == 0 {
            (&*p.cast::<AtomicU64>()).store(value.lo, Ordering::Release);
            (&*p.add(8).cast::<AtomicU64>()).store(value.hi, Ordering::Release);
        } else {
            core::sync::atomic::fence(Ordering::Release);
            core::ptr::copy_nonoverlapping((&value as *const Ocerz128).cast(), p, 16);
        }
    }
}

#[inline(always)]
pub(super) fn ocerz_gs_is_teb_band(gs: u64) -> bool {
    (0x10000..0x380000000).contains(&gs)
}

macro_rules! env_set {
    ($name:literal) => {{
        static ENV_SET_STATE: ::core::sync::atomic::AtomicI32 =
            ::core::sync::atomic::AtomicI32::new(-1);
        let mut state = ENV_SET_STATE.load(::core::sync::atomic::Ordering::Relaxed);
        if state < 0 {
            let found = unsafe {
                ::libc::getenv(concat!($name, "\0").as_ptr().cast()) != ::core::ptr::null_mut()
            };
            state = if found { 1 } else { 0 };
            ENV_SET_STATE.store(state, ::core::sync::atomic::Ordering::Relaxed);
        }
        state != 0
    }};
}

pub(super) use env_set;

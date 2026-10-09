//! Shared helpers the section modules use: the guest-argument fetch, the
//! guest/host address and load/store inlines src/sysbridge.c gets from mem.h,
//! and the return, errno and refusal tails every handler ends in.

use core::ffi::{c_char, c_int, c_void};
use core::sync::atomic::{AtomicU8, AtomicU16, AtomicU32, AtomicU64, fence};

use crate::ffi::*;

pub const SB_LIB: *const c_char = OCERZ_BRIDGE_LIBSYSTEM.as_ptr() as *const c_char;
pub const SB_ARGV_MAX: usize = 256;
pub const SB_ENV_MAX: usize = 512;

pub fn errno() -> *mut c_int {
    unsafe { libc::__error() }
}

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
pub unsafe fn ocerz_ld(gaddr: u64, size: c_int) -> u64 {
    unsafe {
        let p = ocerz_g2h(gaddr);
        match size {
            1 => return (*p.cast::<AtomicU8>()).load(core::sync::atomic::Ordering::Acquire) as u64,
            2 if (p as usize) & 1 == 0 => {
                return (*p.cast::<AtomicU16>()).load(core::sync::atomic::Ordering::Acquire) as u64
            }
            4 if (p as usize) & 3 == 0 => {
                return (*p.cast::<AtomicU32>()).load(core::sync::atomic::Ordering::Acquire) as u64
            }
            8 if (p as usize) & 7 == 0 => {
                return (*p.cast::<AtomicU64>()).load(core::sync::atomic::Ordering::Acquire)
            }
            _ => {}
        }
        let mut v = 0u64;
        core::ptr::copy_nonoverlapping(p.cast::<u8>(), &mut v as *mut u64 as *mut u8, size as usize);
        fence(core::sync::atomic::Ordering::Acquire);
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
                (*p.cast::<AtomicU8>()).store(v as u8, core::sync::atomic::Ordering::Release);
                return;
            }
            2 if (p as usize) & 1 == 0 => {
                (*p.cast::<AtomicU16>()).store(v as u16, core::sync::atomic::Ordering::Release);
                return;
            }
            4 if (p as usize) & 3 == 0 => {
                (*p.cast::<AtomicU32>()).store(v as u32, core::sync::atomic::Ordering::Release);
                return;
            }
            8 if (p as usize) & 7 == 0 => {
                (*p.cast::<AtomicU64>()).store(v, core::sync::atomic::Ordering::Release);
                return;
            }
            _ => {}
        }
        fence(core::sync::atomic::Ordering::Release);
        core::ptr::copy_nonoverlapping(&v as *const u64 as *const u8, p.cast::<u8>(), size as usize);
    }
}

#[inline(always)]
pub unsafe fn sb_arg(cpu: *const OcerzCPU, i: c_int) -> u64 {
    const REGS: [usize; 6] = [
        OCERZ_RDI as usize,
        OCERZ_RSI as usize,
        OCERZ_RDX as usize,
        OCERZ_RCX as usize,
        OCERZ_R8 as usize,
        OCERZ_R9 as usize,
    ];
    unsafe {
        if i < 6 {
            *(*cpu).gpr.get_unchecked(REGS[i as usize])
        } else {
            ocerz_ld(
                (*cpu).gpr[OCERZ_RSP as usize].wrapping_add(8 * (i as u64 - 5)),
                8,
            )
        }
    }
}

#[inline(always)]
pub unsafe fn sb_ptr(g: u64) -> *mut c_void {
    unsafe {
        if g != 0 { ocerz_g2h(g) } else { core::ptr::null_mut() }
    }
}

pub unsafe fn sb_ret(vm: *mut OcerzVM, cpu: *mut OcerzCPU, r: i64) -> c_int {
    unsafe {
        ocerz_bridge_return(cpu, r as u64);
        ocerz_bridge_settle(vm, cpu)
    }
}

pub unsafe fn sb_posix(vm: *mut OcerzVM, cpu: *mut OcerzCPU, err: c_int) -> c_int {
    unsafe {
        if err != 0 {
            *errno() = err;
            return sb_ret(vm, cpu, -1);
        }
        sb_ret(vm, cpu, 0)
    }
}

pub unsafe fn sb_refuse(sym: *const c_char, why: *const c_char) -> ! {
    unsafe {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: bridge: %s %s %s\n".as_ptr(),
            SB_LIB,
            sym,
            why,
        );
        libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int)
    }
}

pub unsafe fn sb_vector(gv: u64, out: *mut *mut c_char, cap: c_int) -> c_int {
    unsafe {
        let mut n = 0;
        let mut at = gv;
        loop {
            if !(gv != 0 && n < cap - 1) {
                break;
            }
            let p = ocerz_ld(at, 8);
            if p == 0 {
                break;
            }
            *out.add(n as usize) = ocerz_g2h(p) as *mut c_char;
            n += 1;
            at += 8;
        }
        *out.add(n as usize) = core::ptr::null_mut();
        n
    }
}

pub unsafe fn sb_open_mode(flags: c_int, raw: u64) -> c_int {
    let creat = (flags & libc::O_CREAT) != 0;
    if creat { raw as c_int } else { 0 }
}

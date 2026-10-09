//! ---- a crossing says, for as long as it lasts, that it is happening ----
//! A crossing is also the only moment at which a thread the guest is driving is
//! running native code, so a fault taken during one is not the guest's fault the
//! way every fault before this layer was.  ocerz_bridge_in_flight is how a crash
//! report asks what the thread was in the middle of: it answers NULL off the
//! crossing path, and otherwise describes the innermost crossing, counted by a
//! depth because a bridged function may in principle re-enter.  The frame holds
//! pointers rather than copies because it is read from inside a signal handler,
//! which may neither allocate nor take a lock; everything it points at is a
//! string literal or a field of a descriptor, all of static lifetime, so reading
//! it costs a load and nothing else.
//!
//! ocerz_bridge_raise and ocerz_bridge_lower are the same bracket for a
//! crossing that is not a descriptor's, such as a message send or a formatted
//! print (objcbridge.h), whose names and signature are known only once the
//! call has arrived.  The strings handed to raise must outlive the crossing,
//! which a selector name, a notation interned for the process, or a buffer on
//! the stack of the function that lowers the frame all do.
//!
//! A native function may call back into guest code, as qsort calls its
//! comparator, and for as long as that guest code runs the thread is not inside
//! native code at all.  ocerz_bridge_guest_enter saves the thread's frame and
//! clears it, so in_flight answers NULL and a guest fault there is handled as the
//! ordinary guest fault it is; ocerz_bridge_guest_leave puts the frame back when
//! the guest code returns.  A bridged call made from inside that guest code raises
//! its own frame and lowers it again in the usual way.  The recovery point a guest
//! fault jumps to is installed by the guest call itself, so a recovered fault
//! lands inside the callback rather than past it, and the saved frame is still
//! there to restore.
//!
//! The frame guest_enter clears is not left empty: it names the saved one as the
//! crossing it is around, and carries a level, a number no other stretch of guest
//! code in the process is given, so guest code can ask which stretch it is
//! running in.  ocerz_bridge_level answers that for the calling thread,
//! numbering a thread's outermost stretch the first time it is asked, and
//! ocerz_bridge_callback_frame answers the frame of the crossing whose callback
//! is running, or NULL at the outermost stretch; following around from there
//! walks outward through every crossing still open on the thread.  A setjmp
//! records the level, and a longjmp that finds a different one would leave the
//! stretch it was made in, which is how it knows it would skip native frames.
//!
//! ocerz_bridge_return and ocerz_bridge_settle are the two halves of how every
//! export ocerz answers itself ends: the first consumes the return address as a
//! ret would and puts the result in rax, the second delivers any guest signal
//! that became pending and unmasked, on top of that finished state.
//! ocerz_bridge_postfork_child puts this file's locks back to their initial
//! state in a fork child, where a thread that held one no longer exists.

use core::ffi::{c_char, c_int, c_void};
use core::ptr::{null_mut, write_bytes};
use core::sync::atomic::{AtomicU64, Ordering};

use crate::ffi::*;
use crate::ported::bridge::lookup::{G_BR_FN_LOCK, G_BR_HOST_LOCK, G_BR_LIBS_LOCK};

#[inline(always)]
pub unsafe fn gpr(cpu: *mut OcerzCPU, reg: u32) -> u64 {
    unsafe { *(*cpu).gpr.get_unchecked(reg as usize) }
}

#[inline(always)]
pub unsafe fn set_gpr(cpu: *mut OcerzCPU, reg: u32, v: u64) {
    unsafe { *(*cpu).gpr.get_unchecked_mut(reg as usize) = v }
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
                return gaddr
                    .wrapping_sub(OCERZ_TOP_LO)
                    .wrapping_add(ocerz_top_base) as *mut c_void;
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
pub unsafe fn ocerz_pinned_page(gaddr: u64) -> bool {
    unsafe {
        !ocerz_pin_map.is_null()
            && gaddr < OCERZ_LOW_LIMIT
            && ((*ocerz_pin_map.add((gaddr >> 17) as usize) >> ((gaddr >> 14) & 7)) & 1) != 0
    }
}

#[inline(always)]
pub unsafe fn ocerz_ld(gaddr: u64, size: c_int) -> u64 {
    use core::sync::atomic::{AtomicU8, AtomicU16, AtomicU32, AtomicU64 as A64, fence};
    unsafe {
        let p = ocerz_g2h(gaddr);
        match size {
            1 => return (*p.cast::<AtomicU8>()).load(Ordering::Acquire) as u64,
            2 if (p as usize) & 1 == 0 => {
                return (*p.cast::<AtomicU16>()).load(Ordering::Acquire) as u64;
            }
            4 if (p as usize) & 3 == 0 => {
                return (*p.cast::<AtomicU32>()).load(Ordering::Acquire) as u64;
            }
            8 if (p as usize) & 7 == 0 => return (*p.cast::<A64>()).load(Ordering::Acquire),
            _ => {}
        }
        let mut v = 0u64;
        core::ptr::copy_nonoverlapping(
            p.cast::<u8>(),
            &mut v as *mut u64 as *mut u8,
            size as usize,
        );
        fence(Ordering::Acquire);
        v
    }
}

#[inline(always)]
pub unsafe fn ocerz_st(gaddr: u64, size: c_int, v: u64) {
    use core::sync::atomic::{AtomicU8, AtomicU16, AtomicU32, AtomicU64 as A64, fence};
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
                (*p.cast::<A64>()).store(v, Ordering::Release);
                return;
            }
            _ => {}
        }
        fence(Ordering::Release);
        core::ptr::copy_nonoverlapping(
            &v as *const u64 as *const u8,
            p.cast::<u8>(),
            size as usize,
        );
    }
}

#[thread_local]
pub static mut G_BR_FRAME: OcerzBridgeFrame = OcerzBridgeFrame {
    lib: core::ptr::null(),
    sym: core::ptr::null(),
    sig: core::ptr::null(),
    host_fn: core::ptr::null(),
    depth: 0,
    level: 0,
    around: core::ptr::null(),
};

static G_BR_LEVELS: AtomicU64 = AtomicU64::new(0);

#[inline(always)]
fn br_new_level() -> u64 {
    G_BR_LEVELS.fetch_add(1, Ordering::SeqCst) + 1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bridge_in_flight() -> *const OcerzBridgeFrame {
    unsafe {
        if (&raw const G_BR_FRAME).read().depth > 0 {
            &raw const G_BR_FRAME
        } else {
            core::ptr::null()
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bridge_guest_enter(saved: *mut OcerzBridgeFrame) {
    unsafe {
        *saved = G_BR_FRAME;
        write_bytes(&raw mut G_BR_FRAME, 0, 1);
        G_BR_FRAME.around = saved;
        G_BR_FRAME.level = br_new_level();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bridge_guest_leave(saved: *const OcerzBridgeFrame) {
    unsafe { G_BR_FRAME = *saved }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bridge_raise(
    outer: *mut OcerzBridgeFrame,
    lib: *const c_char,
    sym: *const c_char,
    sig: *const c_char,
    host_fn: *const c_void,
) {
    unsafe {
        *outer = G_BR_FRAME;
        G_BR_FRAME.lib = lib;
        G_BR_FRAME.sym = sym;
        G_BR_FRAME.sig = sig;
        G_BR_FRAME.host_fn = host_fn;
        G_BR_FRAME.depth = (*outer).depth + 1;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bridge_lower(outer: *const OcerzBridgeFrame) {
    unsafe { G_BR_FRAME = *outer }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bridge_depth_ptr() -> *const c_int {
    unsafe { &raw const G_BR_FRAME.depth }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bridge_level() -> u64 {
    unsafe {
        if G_BR_FRAME.level == 0 {
            G_BR_FRAME.level = br_new_level();
        }
        G_BR_FRAME.level
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bridge_callback_frame() -> *const OcerzBridgeFrame {
    unsafe { (&raw const G_BR_FRAME).read().around }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bridge_postfork_child() {
    unsafe {
        G_BR_LIBS_LOCK = libc::PTHREAD_MUTEX_INITIALIZER;
        G_BR_FN_LOCK = libc::PTHREAD_MUTEX_INITIALIZER;
        G_BR_HOST_LOCK = libc::PTHREAD_MUTEX_INITIALIZER;
    }
}

#[inline(always)]
pub unsafe fn br_return(cpu: *mut OcerzCPU, rax: u64) {
    unsafe {
        let rsp = gpr(cpu, OCERZ_RSP);
        (*cpu).rip = ocerz_ld(rsp, 8);
        set_gpr(cpu, OCERZ_RSP, rsp + 8);
        set_gpr(cpu, OCERZ_RAX, rax);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bridge_return(cpu: *mut OcerzCPU, rax: u64) {
    unsafe { br_return(cpu, rax) }
}

#[inline(always)]
pub unsafe fn br_settle(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        if ocerz_peek_pending_async_sig() != 0 || ((*cpu).sig_pending & !(*cpu).sig_mask) != 0 {
            ocerz_guest_deliver_pending(vm, cpu);
        }
        OCERZ_STEP_OK as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bridge_settle(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { br_settle(vm, cpu) }
}

#[inline(always)]
pub unsafe fn br_answer(vm: *mut OcerzVM, cpu: *mut OcerzCPU, rax: u64) -> c_int {
    unsafe {
        br_return(cpu, rax);
        br_settle(vm, cpu)
    }
}

#[inline(always)]
pub unsafe fn br_posix(vm: *mut OcerzVM, cpu: *mut OcerzCPU, err: c_int) -> c_int {
    unsafe {
        if err != 0 {
            *libc::__error() = err;
            br_return(cpu, (-1i64) as u64);
        } else {
            br_return(cpu, 0);
        }
        br_settle(vm, cpu)
    }
}

#[inline(always)]
pub unsafe fn br_stack_below(cpu: *const OcerzCPU) -> u64 {
    unsafe { (gpr(cpu as *mut OcerzCPU, OCERZ_RSP) - 128) & !15u64 }
}

#[inline(always)]
pub unsafe fn br_caller(cpu: *const OcerzCPU) -> u64 {
    unsafe { ocerz_ld(gpr(cpu as *mut OcerzCPU, OCERZ_RSP), 8) }
}

pub unsafe fn br_guest_path(g: u64, buf: *mut c_char, n: usize) -> *const c_char {
    unsafe {
        if g == 0 {
            return core::ptr::null();
        }
        libc::snprintf(buf, n, c"%s".as_ptr(), ocerz_g2h(g) as *const c_char);
        buf
    }
}

pub fn br_logging() -> c_int {
    static EN: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
    let mut en = EN.load(Ordering::Relaxed);
    if en < 0 {
        let e = unsafe { libc::getenv(c"OCERZ_BRIDGELOG".as_ptr()) };
        en = if e.is_null() {
            0
        } else if unsafe { libc::atoi(e) } >= 2 {
            2
        } else {
            1
        };
        EN.store(en, Ordering::Relaxed);
    }
    en
}

pub fn br_susp_logging() -> bool {
    static EN: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
    let mut en = EN.load(Ordering::Relaxed);
    if en < 0 {
        en = unsafe { !libc::getenv(c"OCERZ_SUSPLOG".as_ptr()).is_null() } as c_int;
        EN.store(en, Ordering::Relaxed);
    }
    en != 0
}

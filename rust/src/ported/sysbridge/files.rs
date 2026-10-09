//! ---- variadic calls whose optional argument is fixed ----
//! open, openat, their $NOCANCEL forms, open_dprotected_np,
//! openat_dprotected_np, guarded_open_np and guarded_open_dprotected_np are
//! declared with an ellipsis, but what follows the named arguments is not a
//! list: it is one argument whose presence and type the named arguments
//! decide.  x86-64 passes it in the next integer register, where System V puts
//! any next argument, and Apple's arm64 passes every variadic argument in an
//! eight-byte stack slot, so no signature can describe the call.  Each handler
//! reads the optional argument out of the register the guest left it in, and
//! only when the call uses it, then calls the host function from C through its
//! real variadic prototype, so the compiler lays the slot out the way the
//! host's va_arg reads it.  The mode of the open family is read only with
//! O_CREAT, and O_TMPFILE where the SDK defines it, and the host is otherwise
//! passed 0, which is what Apple's own open hands the kernel.
//!
//! These handlers raise a bridge frame around the host call, because the host
//! dereferences guest pointers there, and a fault is reported naming the call.

use core::ffi::{c_char, c_int};

use super::common::*;
use crate::ffi::*;

unsafe extern "C" {
    #[link_name = "open$NOCANCEL"]
    fn host_open_nocancel(path: *const c_char, flags: c_int, ...) -> c_int;
    #[link_name = "openat$NOCANCEL"]
    fn host_openat_nocancel(fd: c_int, path: *const c_char, flags: c_int, ...) -> c_int;
    fn open_dprotected_np(path: *const c_char, flags: c_int, dpclass: c_int, dpflags: c_int, ...)
        -> c_int;
    fn openat_dprotected_np(
        fd: c_int,
        path: *const c_char,
        flags: c_int,
        dpclass: c_int,
        dpflags: c_int,
        ...
    ) -> c_int;
    fn guarded_open_np(
        path: *const c_char,
        guard: *const u64,
        guardflags: u32,
        flags: c_int,
        ...
    ) -> c_int;
    fn guarded_open_dprotected_np(
        path: *const c_char,
        guard: *const u64,
        guardflags: u32,
        flags: c_int,
        dpclass: c_int,
        dpflags: c_int,
        ...
    ) -> c_int;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_open(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let path = sb_ptr(sb_arg(cpu, 0)) as *const c_char;
        let flags = sb_arg(cpu, 1) as c_int;
        let mode = sb_open_mode(flags, sb_arg(cpu, 2));
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_open".as_ptr(), c"i(pii)".as_ptr(), libc::open as *const _);
        let r = libc::open(path, flags, mode);
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_open_nocancel(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let path = sb_ptr(sb_arg(cpu, 0)) as *const c_char;
        let flags = sb_arg(cpu, 1) as c_int;
        let mode = sb_open_mode(flags, sb_arg(cpu, 2));
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_open$NOCANCEL".as_ptr(), c"i(pii)".as_ptr(), host_open_nocancel as *const _);
        let r = host_open_nocancel(path, flags, mode);
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_openat(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let fd = sb_arg(cpu, 0) as c_int;
        let path = sb_ptr(sb_arg(cpu, 1)) as *const c_char;
        let flags = sb_arg(cpu, 2) as c_int;
        let mode = sb_open_mode(flags, sb_arg(cpu, 3));
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_openat".as_ptr(), c"i(ipii)".as_ptr(), libc::openat as *const _);
        let r = libc::openat(fd, path, flags, mode);
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_openat_nocancel(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let fd = sb_arg(cpu, 0) as c_int;
        let path = sb_ptr(sb_arg(cpu, 1)) as *const c_char;
        let flags = sb_arg(cpu, 2) as c_int;
        let mode = sb_open_mode(flags, sb_arg(cpu, 3));
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_openat$NOCANCEL".as_ptr(), c"i(ipii)".as_ptr(), host_openat_nocancel as *const _);
        let r = host_openat_nocancel(fd, path, flags, mode);
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_open_dprotected_np(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let path = sb_ptr(sb_arg(cpu, 0)) as *const c_char;
        let flags = sb_arg(cpu, 1) as c_int;
        let cls = sb_arg(cpu, 2) as c_int;
        let dpflags = sb_arg(cpu, 3) as c_int;
        let mode = sb_open_mode(flags, sb_arg(cpu, 4));
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_open_dprotected_np".as_ptr(), c"i(piiii)".as_ptr(), open_dprotected_np as *const _);
        let r = open_dprotected_np(path, flags, cls, dpflags, mode);
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_openat_dprotected_np(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let fd = sb_arg(cpu, 0) as c_int;
        let path = sb_ptr(sb_arg(cpu, 1)) as *const c_char;
        let flags = sb_arg(cpu, 2) as c_int;
        let cls = sb_arg(cpu, 3) as c_int;
        let dpflags = sb_arg(cpu, 4) as c_int;
        let mode = sb_open_mode(flags, sb_arg(cpu, 5));
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_openat_dprotected_np".as_ptr(), c"i(ipiiii)".as_ptr(), openat_dprotected_np as *const _);
        let r = openat_dprotected_np(fd, path, flags, cls, dpflags, mode);
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_guarded_open_np(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let path = sb_ptr(sb_arg(cpu, 0)) as *const c_char;
        let guard = sb_ptr(sb_arg(cpu, 1)) as *const u64;
        let guardflags = sb_arg(cpu, 2) as u32;
        let flags = sb_arg(cpu, 3) as c_int;
        let mode = sb_open_mode(flags, sb_arg(cpu, 4));
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_guarded_open_np".as_ptr(), c"i(ppuii)".as_ptr(), guarded_open_np as *const _);
        let r = guarded_open_np(path, guard, guardflags, flags, mode);
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_guarded_open_dprotected_np(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let path = sb_ptr(sb_arg(cpu, 0)) as *const c_char;
        let guard = sb_ptr(sb_arg(cpu, 1)) as *const u64;
        let guardflags = sb_arg(cpu, 2) as u32;
        let flags = sb_arg(cpu, 3) as c_int;
        let cls = sb_arg(cpu, 4) as c_int;
        let dpflags = sb_arg(cpu, 5) as c_int;
        let mode = sb_open_mode(flags, sb_arg(cpu, 6));
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_guarded_open_dprotected_np".as_ptr(), c"i(ppuiiii)".as_ptr(), guarded_open_dprotected_np as *const _);
        let r = guarded_open_dprotected_np(path, guard, guardflags, flags, cls, dpflags, mode);
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

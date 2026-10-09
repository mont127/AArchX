//! The rest: glob and globfree, the guest's page size by every name the host
//! answers it under (getpagesize, sysconf, host_page_size, sysctl hw.pagesize
//! and sysctlbyname hw.pagesize/hw.pagesize32/vm.pagesize), the thread stack
//! queries that know guest stacks, and the sandbox entry points, which dlsym
//! the real functions since the sandbox headers ship no declarations.

use core::ffi::{c_char, c_int, c_uint, c_void};

use super::common::*;
use crate::ffi::*;

const SB_SANDBOX_FILTER_MASK: c_int = 0x3fffffff;
const GLOB_ALTDIRFUNC: c_int = 0x0040;
const SB_SANDBOX_LIB: &[u8] = b"/usr/lib/libsandbox.1.dylib\0";

unsafe extern "C" {
    fn sandbox_check(pid: libc::pid_t, operation: *const c_char, type_: c_int, ...) -> c_int;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_glob(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let flags = sb_arg(cpu, 1) as c_int;
        let errfunc = sb_arg(cpu, 2);
        let mut slot = 0u64;
        if flags & GLOB_ALTDIRFUNC != 0 {
            sb_refuse(
                c"_glob".as_ptr(),
                c"was given GLOB_ALTDIRFUNC, whose directory functions in glob_t are x86 code".as_ptr(),
            );
        }
        if errfunc != 0
            && (ocerz_abi_callback_convert(errfunc, c"i(pi)".as_ptr(), &mut slot) != OCERZ_OK as c_int
                || slot == 0)
        {
            sb_refuse(c"_glob".as_ptr(), c"could not bind its error function to a callback".as_ptr());
        }
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_glob".as_ptr(), c"i(pic{i(pi)}p)".as_ptr(), libc::glob as *const _);
        let cb: Option<extern "C" fn(*const c_char, c_int) -> c_int> = if slot != 0 {
            Some(core::mem::transmute::<*mut c_void, extern "C" fn(*const c_char, c_int) -> c_int>(
                ocerz_g2h(slot),
            ))
        } else {
            None
        };
        let r = libc::glob(
            sb_ptr(sb_arg(cpu, 0)) as *const c_char,
            flags,
            cb,
            sb_ptr(sb_arg(cpu, 3)) as *mut libc::glob_t,
        );
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_globfree(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_globfree".as_ptr(), c"v(p)".as_ptr(), libc::globfree as *const _);
        libc::globfree(sb_ptr(sb_arg(cpu, 0)) as *mut libc::glob_t);
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, 0)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_pthread_get_stackaddr_np(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let thread = sb_ptr(sb_arg(cpu, 0));
        let mut lo = 0u64;
        let mut hi = 0u64;
        if ocerz_vm_guest_stack(vm, thread, &mut lo, &mut hi) != 0 {
            return sb_ret(vm, cpu, hi as i64);
        }
        sb_ret(
            vm,
            cpu,
            ocerz_h2g(libc::pthread_get_stackaddr_np(thread as libc::pthread_t)) as i64,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_pthread_get_stacksize_np(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let thread = sb_ptr(sb_arg(cpu, 0));
        let mut lo = 0u64;
        let mut hi = 0u64;
        if ocerz_vm_guest_stack(vm, thread, &mut lo, &mut hi) != 0 {
            return sb_ret(vm, cpu, (hi - lo) as i64);
        }
        sb_ret(vm, cpu, libc::pthread_get_stacksize_np(thread as libc::pthread_t) as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_getpagesize(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { sb_ret(vm, cpu, OCERZ_GUEST_PAGE_SIZE as i64) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_sysconf(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let name = sb_arg(cpu, 0) as c_int;
        if name == libc::_SC_PAGESIZE {
            return sb_ret(vm, cpu, OCERZ_GUEST_PAGE_SIZE as i64);
        }
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_sysconf".as_ptr(), c"l(i)".as_ptr(), libc::sysconf as *const _);
        let r = libc::sysconf(name);
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_host_page_size(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let out = sb_arg(cpu, 1);
        if out != 0 {
            ocerz_st(out, 8, OCERZ_GUEST_PAGE_SIZE as u64);
        }
        sb_ret(vm, cpu, libc::KERN_SUCCESS as i64)
    }
}

unsafe fn sb_sysctl_page_size(is_page_size: c_int, oldp: u64, oldlenp: u64, r: c_int) {
    unsafe {
        if is_page_size == 0 || r != 0 || oldp == 0 || oldlenp == 0 {
            return;
        }
        let len = ocerz_ld(oldlenp, 8);
        if len == 4 || len == 8 {
            ocerz_st(oldp, len as c_int, OCERZ_GUEST_PAGE_SIZE as u64);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_sysctl(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mib = sb_ptr(sb_arg(cpu, 0)) as *mut c_int;
        let n = sb_arg(cpu, 1) as c_uint;
        let oldp = sb_arg(cpu, 2);
        let oldlenp = sb_arg(cpu, 3);
        let page = (!mib.is_null()
            && n == 2
            && *mib == libc::CTL_HW
            && *mib.add(1) == libc::HW_PAGESIZE) as c_int;
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_sysctl".as_ptr(), c"i(pupppL)".as_ptr(), libc::sysctl as *const _);
        let r = libc::sysctl(
            mib,
            n,
            sb_ptr(oldp),
            sb_ptr(oldlenp) as *mut usize,
            sb_ptr(sb_arg(cpu, 4)),
            sb_arg(cpu, 5) as libc::size_t,
        );
        ocerz_bridge_lower(&outer);
        sb_sysctl_page_size(page, oldp, oldlenp, r);
        sb_ret(vm, cpu, r as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_sysctlbyname(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let name = sb_ptr(sb_arg(cpu, 0)) as *const c_char;
        let oldp = sb_arg(cpu, 1);
        let oldlenp = sb_arg(cpu, 2);
        let page = (!name.is_null()
            && (libc::strcmp(name, c"hw.pagesize".as_ptr()) == 0
                || libc::strcmp(name, c"hw.pagesize32".as_ptr()) == 0
                || libc::strcmp(name, c"vm.pagesize".as_ptr()) == 0)) as c_int;
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_sysctlbyname".as_ptr(), c"i(ppppL)".as_ptr(), libc::sysctlbyname as *const _);
        let r = libc::sysctlbyname(
            name,
            sb_ptr(oldp),
            sb_ptr(oldlenp) as *mut usize,
            sb_ptr(sb_arg(cpu, 3)),
            sb_arg(cpu, 4) as libc::size_t,
        );
        ocerz_bridge_lower(&outer);
        sb_sysctl_page_size(page, oldp, oldlenp, r);
        sb_ret(vm, cpu, r as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_sandbox_check(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let pid = sb_arg(cpu, 0) as libc::pid_t;
        let operation = sb_ptr(sb_arg(cpu, 1)) as *const c_char;
        let type_ = sb_arg(cpu, 2) as c_int;
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_sandbox_check".as_ptr(), c"i(ipi)".as_ptr(), sandbox_check as *const _);
        let r = if type_ & SB_SANDBOX_FILTER_MASK != 0 {
            sandbox_check(pid, operation, type_, sb_ptr(sb_arg(cpu, 3)))
        } else {
            sandbox_check(pid, operation, type_)
        };
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

unsafe fn sb_sandbox_sym(lib: *const c_char, name: *const c_char) -> *mut c_void {
    unsafe {
        let h = if !lib.is_null() {
            libc::dlopen(lib, libc::RTLD_LAZY)
        } else {
            libc::RTLD_DEFAULT
        };
        if !h.is_null() { libc::dlsym(h, name) } else { core::ptr::null_mut() }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_sandbox_init(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        type Fn = unsafe extern "C" fn(*const c_char, u64, *mut *mut c_char) -> c_int;
        let fn_ = sb_sandbox_sym(core::ptr::null(), c"sandbox_init".as_ptr());
        let mut outer = OcerzBridgeFrame::default();
        let mut r = -1;
        ocerz_apidb_preload();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_sandbox_init".as_ptr(), c"i(pLp)".as_ptr(), fn_ as *const c_void);
        if !fn_.is_null() {
            r = (core::mem::transmute::<*mut c_void, Fn>(fn_))(
                sb_ptr(sb_arg(cpu, 0)) as *const c_char,
                sb_arg(cpu, 1),
                sb_ptr(sb_arg(cpu, 2)) as *mut *mut c_char,
            );
        }
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_sandbox_init_with_parameters(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        type Fn = unsafe extern "C" fn(*const c_char, u64, *const *const c_char, *mut *mut c_char) -> c_int;
        let fn_ = sb_sandbox_sym(core::ptr::null(), c"sandbox_init_with_parameters".as_ptr());
        let mut outer = OcerzBridgeFrame::default();
        let mut r = -1;
        ocerz_apidb_preload();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_sandbox_init_with_parameters".as_ptr(), c"i(pLpp)".as_ptr(), fn_ as *const c_void);
        if !fn_.is_null() {
            r = (core::mem::transmute::<*mut c_void, Fn>(fn_))(
                sb_ptr(sb_arg(cpu, 0)) as *const c_char,
                sb_arg(cpu, 1),
                sb_ptr(sb_arg(cpu, 2)) as *const *const c_char,
                sb_ptr(sb_arg(cpu, 3)) as *mut *mut c_char,
            );
        }
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_sandbox_ms(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        type Fn = unsafe extern "C" fn(*const c_char, c_int, *mut c_void) -> c_int;
        let fn_ = sb_sandbox_sym(core::ptr::null(), c"__sandbox_ms".as_ptr());
        let mut outer = OcerzBridgeFrame::default();
        let mut r = -1;
        ocerz_apidb_preload();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"___sandbox_ms".as_ptr(), c"i(pip)".as_ptr(), fn_ as *const c_void);
        if !fn_.is_null() {
            r = (core::mem::transmute::<*mut c_void, Fn>(fn_))(
                sb_ptr(sb_arg(cpu, 0)) as *const c_char,
                sb_arg(cpu, 1) as c_int,
                sb_ptr(sb_arg(cpu, 2)),
            );
        }
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_sandbox_apply(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        type Fn = unsafe extern "C" fn(*mut c_void) -> c_int;
        let fn_ = sb_sandbox_sym(SB_SANDBOX_LIB.as_ptr() as *const c_char, c"sandbox_apply".as_ptr());
        let mut outer = OcerzBridgeFrame::default();
        let mut r = -1;
        ocerz_apidb_preload();
        ocerz_bridge_raise(&mut outer, SB_SANDBOX_LIB.as_ptr() as *const c_char, c"_sandbox_apply".as_ptr(), c"i(p)".as_ptr(), fn_ as *const c_void);
        if !fn_.is_null() {
            r = (core::mem::transmute::<*mut c_void, Fn>(fn_))(sb_ptr(sb_arg(cpu, 0)));
        }
        ocerz_bridge_lower(&outer);
        sb_ret(vm, cpu, r as i64)
    }
}

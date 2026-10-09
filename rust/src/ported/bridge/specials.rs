//! Specials: the exports ocerz answers with its own handlers rather than a
//! host call — the process-lifecycle words (exit, abort, the stack probe and
//! TLV fixup), the signal surface (answered through the guest's own signal
//! state so it composes with the machinery that delivers signals to the
//! running guest code), the iovec/msghdr calls (whose arrays of guest
//! pointers have to be translated element by element), dlopen/dladdr and the
//! dyld image queries (answered by the guest image table, not the host's),
//! the exception-port and debug-register answers (translated code keeps its
//! own exceptions and has no hardware breakpoints), and the framework
//! constructors whose callback structures no apidb shape can describe
//! (Crossed per word, bound through the callback bank, or refused).

use core::ffi::{c_char, c_int, c_uint, c_void};
use core::ptr::{null, null_mut};
use core::sync::atomic::{AtomicPtr, Ordering};

use crate::ffi::*;
use crate::ported::bridge::common::{
    br_answer, br_caller, br_guest_path, br_logging, br_posix, br_return, br_settle,
    br_stack_below, br_susp_logging, gpr, ocerz_g2h, ocerz_h2g, ocerz_ld, ocerz_st, set_gpr,
};

unsafe extern "C" fn br_exitlog(cpu: *const OcerzCPU, how: *const c_char) {
    unsafe {
        if libc::getenv(c"OCERZ_EXITLOG".as_ptr()).is_null() {
            return;
        }
        unsafe extern "C" {
            static ocerz_cmdline_summary: [c_char; 0];
        }
        let rsp = gpr(cpu as *mut OcerzCPU, OCERZ_RSP);
        let mut fp = gpr(cpu as *mut OcerzCPU, OCERZ_RBP);
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: EXITLOG[%d \"%s\"] %s code=%d from=%#llx ret-chain:".as_ptr(),
            libc::getpid(),
            ocerz_cmdline_summary.as_ptr(),
            how,
            gpr(cpu as *mut OcerzCPU, OCERZ_RDI) as c_int,
            ocerz_ld(rsp, 8),
        );
        let mut d = 0;
        while d < 8 && fp > 0x10000 && fp < OCERZ_TOP_HI {
            libc::fprintf(
                crate::log::stderr(),
                c" %#llx".as_ptr(),
                ocerz_ld(fp + 8, 8),
            );
            let nf = ocerz_ld(fp, 8);
            if nf <= fp {
                break;
            }
            fp = nf;
            d += 1;
        }
        libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
    }
}

pub unsafe extern "C" fn br_exit(_vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_exitlog(cpu, c"exit".as_ptr());
        libc::exit((gpr(cpu, OCERZ_RDI) & 0xff) as c_int);
    }
}

pub unsafe extern "C" fn br_exit_now(_vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_exitlog(cpu, c"_exit".as_ptr());
        libc::_exit((gpr(cpu, OCERZ_RDI) & 0xff) as c_int);
    }
}

pub unsafe extern "C" fn br_error(_vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let rsp = gpr(cpu, OCERZ_RSP);
        (*cpu).rip = ocerz_ld(rsp, 8);
        set_gpr(cpu, OCERZ_RSP, rsp + 8);
        set_gpr(
            cpu,
            OCERZ_RAX,
            if (*cpu).gs_base != 0 {
                (*cpu).gs_base + OCERZ_ERRNO_SLOT as u64
            } else {
                ocerz_h2g(libc::__error() as *const c_void)
            },
        );
        OCERZ_STEP_OK as c_int
    }
}

pub unsafe extern "C" fn br_nothing(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { br_answer(vm, cpu, 0) }
}

unsafe extern "C" fn host_bzero(addr: *mut c_void, n: usize) {
    unsafe { bzero(addr, n) }
}

pub unsafe extern "C" fn br_bzero(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let dst = gpr(cpu, OCERZ_RDI);
        let mut outer: OcerzBridgeFrame = core::mem::zeroed();
        ocerz_bridge_raise(
            &mut outer,
            OCERZ_BRIDGE_LIBSYSTEM.as_ptr() as *const c_char,
            c"___bzero".as_ptr(),
            c"p(pL)".as_ptr(),
            host_bzero as *const c_void,
        );
        if dst != 0 {
            host_bzero(ocerz_g2h(dst), gpr(cpu, OCERZ_RSI) as usize);
        }
        ocerz_bridge_lower(&outer);
        br_answer(vm, cpu, dst)
    }
}

pub unsafe extern "C" fn br_abort(vm: *mut OcerzVM, _cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: bridge: the guest called abort\n".as_ptr(),
        );
        ocerz_vm_request_exit(vm, 134);
        OCERZ_STEP_EXIT as c_int
    }
}

pub unsafe extern "C" fn br_stack_chk_fail(vm: *mut OcerzVM, _cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: bridge: the guest overran a stack guard (__stack_chk_fail)\n".as_ptr(),
        );
        ocerz_vm_request_exit(vm, 134);
        OCERZ_STEP_EXIT as c_int
    }
}

pub unsafe extern "C" fn br_tlv_bootstrap(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let desc = gpr(cpu, OCERZ_RDI);
        let addr = ocerz_tlv_address(cpu, desc);
        if addr == 0 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: bridge: thread-local variable descriptor %#llx could not be resolved\n"
                    .as_ptr(),
                desc,
            );
            ocerz_vm_request_exit(vm, 134);
            return OCERZ_STEP_EXIT as c_int;
        }
        let rsp = gpr(cpu, OCERZ_RSP);
        set_gpr(cpu, OCERZ_R11, ocerz_ld(rsp, 8));
        (*cpu).rip = ocerz_ld(rsp + 8, 8);
        set_gpr(cpu, OCERZ_RSP, rsp + 16);
        set_gpr(cpu, OCERZ_RAX, addr);
        OCERZ_STEP_OK as c_int
    }
}

pub unsafe extern "C" fn br_chkstk(_vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let rsp = gpr(cpu, OCERZ_RSP);
        set_gpr(cpu, OCERZ_R11, ocerz_ld(rsp, 8));
        (*cpu).rip = ocerz_ld(rsp + 8, 8);
        set_gpr(cpu, OCERZ_RSP, rsp + 16);
        OCERZ_STEP_OK as c_int
    }
}

pub unsafe extern "C" fn br_sigaction(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_posix(
            vm,
            cpu,
            ocerz_guest_sigaction_user(
                vm,
                cpu,
                gpr(cpu, OCERZ_RDI) as c_int,
                gpr(cpu, OCERZ_RSI),
                gpr(cpu, OCERZ_RDX),
            ),
        )
    }
}

pub unsafe extern "C" fn br_signal(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut old = 0u64;
        let err = ocerz_guest_signal(
            vm,
            cpu,
            gpr(cpu, OCERZ_RDI) as c_int,
            gpr(cpu, OCERZ_RSI),
            &mut old,
        );
        if err != 0 {
            *libc::__error() = err;
            old = (-1i64) as u64;
        }
        br_return(cpu, old);
        br_settle(vm, cpu)
    }
}

pub unsafe extern "C" fn br_sigprocmask(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_posix(
            vm,
            cpu,
            ocerz_guest_sigprocmask(
                vm,
                cpu,
                gpr(cpu, OCERZ_RDI) as c_int,
                gpr(cpu, OCERZ_RSI),
                gpr(cpu, OCERZ_RDX),
            ),
        )
    }
}

pub unsafe extern "C" fn br_pthread_sigmask(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let err = ocerz_guest_sigprocmask(
            vm,
            cpu,
            gpr(cpu, OCERZ_RDI) as c_int,
            gpr(cpu, OCERZ_RSI),
            gpr(cpu, OCERZ_RDX),
        );
        br_return(cpu, err as u32 as u64);
        br_settle(vm, cpu)
    }
}

unsafe fn br_suspend(vm: *mut OcerzVM, cpu: *mut OcerzCPU, mask: u64) -> c_int {
    unsafe {
        let caught = ocerz_guest_sigsuspend(vm, cpu, mask);
        *libc::__error() = libc::EINTR;
        br_return(cpu, (-1i64) as u64);
        if caught != 0 {
            ocerz_guest_deliver_now(cpu, caught);
        }
        br_settle(vm, cpu)
    }
}

unsafe fn br_ldt(vm: *mut OcerzVM, cpu: *mut OcerzCPU, num: c_int) -> c_int {
    unsafe {
        let r = ocerz_ldt_call(
            num,
            gpr(cpu, OCERZ_RDI) as i32,
            gpr(cpu, OCERZ_RSI),
            gpr(cpu, OCERZ_RDX) as i32,
        );
        if r < 0 {
            *libc::__error() = (-r) as c_int;
        }
        br_return(
            cpu,
            if r < 0 {
                (-1i64) as u64
            } else {
                r as u32 as u64
            },
        );
        br_settle(vm, cpu)
    }
}

pub unsafe extern "C" fn br_i386_set_ldt(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { br_ldt(vm, cpu, 5) }
}
pub unsafe extern "C" fn br_i386_get_ldt(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { br_ldt(vm, cpu, 6) }
}
pub unsafe extern "C" fn br_sigsuspend(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let set = gpr(cpu, OCERZ_RDI);
        br_suspend(
            vm,
            cpu,
            if set != 0 {
                ocerz_ld(set, 4) as u32 as u64
            } else {
                0
            },
        )
    }
}
pub unsafe extern "C" fn br_pause(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { br_suspend(vm, cpu, u64::MAX) }
}

pub unsafe extern "C" fn br_sigpending(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let set = gpr(cpu, OCERZ_RDI);
        if set != 0 {
            ocerz_st(set, 4, ocerz_guest_sigpending(cpu) as u32 as u64);
        }
        br_return(cpu, 0);
        br_settle(vm, cpu)
    }
}

pub unsafe extern "C" fn br_sigwait(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let set = gpr(cpu, OCERZ_RDI);
        let sigp = gpr(cpu, OCERZ_RSI);
        let got = ocerz_guest_sigwait(vm, cpu, if set != 0 { ocerz_ld(set, 4) as u32 } else { 0 });
        if got != 0 && sigp != 0 {
            ocerz_st(sigp, 4, got as u32 as u64);
        }
        br_return(cpu, if got != 0 { 0 } else { libc::EINTR as u64 });
        br_settle(vm, cpu)
    }
}

pub unsafe extern "C" fn br_sigaltstack(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_posix(
            vm,
            cpu,
            ocerz_guest_sigaltstack(vm, cpu, gpr(cpu, OCERZ_RDI), gpr(cpu, OCERZ_RSI)),
        )
    }
}

pub unsafe extern "C" fn br_raise(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_posix(
            vm,
            cpu,
            ocerz_guest_raise(vm, cpu, gpr(cpu, OCERZ_RDI) as c_int),
        )
    }
}

pub unsafe extern "C" fn br_kill(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let pid = gpr(cpu, OCERZ_RDI) as c_int;
        let sig = gpr(cpu, OCERZ_RSI) as c_int;
        if pid == libc::getpid() && sig != 0 {
            if ocerz_guest_post_to_waiter(sig) != 0 {
                return br_posix(vm, cpu, 0);
            }
            return br_posix(vm, cpu, ocerz_guest_raise(vm, cpu, sig));
        }
        br_posix(
            vm,
            cpu,
            if libc::kill(pid, sig) == 0 {
                0
            } else {
                *libc::__error()
            },
        )
    }
}

pub unsafe extern "C" fn br_pthread_kill(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let err =
            ocerz_guest_pthread_kill(vm, cpu, gpr(cpu, OCERZ_RDI), gpr(cpu, OCERZ_RSI) as c_int);
        br_return(cpu, err as u32 as u64);
        br_settle(vm, cpu)
    }
}

const BR_ULF_NO_ERRNO: u32 = 0x01000000;
const BR_IOV_MAX: i64 = 1024;

unsafe fn br_iov_load(giov: u64, cnt: i64, out: *mut libc::iovec) -> c_int {
    unsafe {
        if cnt < 0 || cnt > BR_IOV_MAX {
            return libc::EINVAL;
        }
        for i in 0..cnt {
            let base = ocerz_ld(giov + i as u64 * 16, 8);
            (*out.add(i as usize)).iov_base = if base != 0 {
                ocerz_g2h(base)
            } else {
                null_mut()
            };
            (*out.add(i as usize)).iov_len = ocerz_ld(giov + i as u64 * 16 + 8, 8) as usize;
        }
        0
    }
}

unsafe fn br_iov_call(vm: *mut OcerzVM, cpu: *mut OcerzCPU, kind: c_int) -> c_int {
    unsafe {
        let fd = gpr(cpu, OCERZ_RDI) as c_int;
        let cnt = gpr(cpu, OCERZ_RDX) as i32 as i64;
        let v = if cnt > 0 && cnt <= BR_IOV_MAX {
            libc::malloc(size_of::<libc::iovec>() * cnt as usize) as *mut libc::iovec
        } else {
            null_mut()
        };
        let mut r: isize = -1;
        let mut e = if v.is_null() {
            libc::EINVAL
        } else {
            br_iov_load(gpr(cpu, OCERZ_RSI), cnt, v)
        };
        if e == 0 {
            let off = gpr(cpu, OCERZ_RCX) as i64;
            r = if kind == 0 {
                libc::readv(fd, v, cnt as c_int)
            } else if kind == 1 {
                libc::writev(fd, v, cnt as c_int)
            } else if kind == 2 {
                libc::preadv(fd, v, cnt as c_int, off)
            } else {
                libc::pwritev(fd, v, cnt as c_int, off)
            };
            e = if r < 0 { *libc::__error() } else { 0 };
        }
        libc::free(v as *mut c_void);
        if e != 0 {
            *libc::__error() = e;
        }
        br_return(cpu, if e != 0 { (-1i64) as u64 } else { r as u64 });
        br_settle(vm, cpu)
    }
}

const NSIG: c_int = 32;
const KERN_INVALID_ARG: i32 = 4;

unsafe extern "C" {
    fn bzero(addr: *mut c_void, n: usize);
    fn clock_gettime_nsec_np(clock: u32) -> u64;
    #[link_name = "__ulock_wait"]
    fn host_ulock_wait(operation: u32, addr: *mut c_void, value: u64, timeout_us: u32) -> c_int;
    #[link_name = "__ulock_wait2"]
    fn host_ulock_wait2(
        operation: u32,
        addr: *mut c_void,
        value: u64,
        timeout_ns: u64,
        value2: u64,
    ) -> c_int;
}

/// A guest signal is delivered when the guest next leaves a crossing, so one
/// that lands after Wine's msync last looked at its word and before the ulock
/// wait reaches the kernel is held until the wait ends, and an APC that would
/// have set the word never runs: a service's first RPC reply waited forever and
/// services.exe gave up on it.  The wait is marked kickable-if-signalled, the
/// unstick monitor (vm.c) interrupts it once a guest signal is pending, and the
/// EINTR it returns lets the handler run before msync waits again.
unsafe fn br_ulock(vm: *mut OcerzVM, cpu: *mut OcerzCPU, two: c_int) -> c_int {
    unsafe {
        let op = gpr(cpu, OCERZ_RDI) as u32;
        let addr = if gpr(cpu, OCERZ_RSI) != 0 {
            ocerz_g2h(gpr(cpu, OCERZ_RSI))
        } else {
            null_mut()
        };
        let value = gpr(cpu, OCERZ_RDX);
        let timeout = gpr(cpu, OCERZ_RCX);
        let r: i64;
        let mut e = 0;
        ocerz_unstick_start();
        (*cpu).block_sigonly = 1;
        core::sync::atomic::fence(Ordering::SeqCst);
        (*cpu).block_since_ns = clock_gettime_nsec_np(libc::CLOCK_UPTIME_RAW);
        core::sync::atomic::fence(Ordering::SeqCst);
        if ((*cpu).sig_pending & !(*cpu).sig_mask) != 0 {
            r = if op & BR_ULF_NO_ERRNO != 0 {
                -(libc::EINTR as i64)
            } else {
                -1
            };
            e = libc::EINTR;
        } else {
            let rr = if two != 0 {
                host_ulock_wait2(op, addr, value, timeout, gpr(cpu, OCERZ_R8))
            } else {
                host_ulock_wait(op, addr, value, timeout as u32)
            };
            e = if rr < 0 {
                if op & BR_ULF_NO_ERRNO != 0 {
                    -rr
                } else {
                    *libc::__error()
                }
            } else {
                0
            };
            r = rr as i64;
        }
        core::sync::atomic::fence(Ordering::SeqCst);
        (*cpu).block_since_ns = 0;
        (*cpu).block_sigonly = 0;
        if e != 0 && op & BR_ULF_NO_ERRNO == 0 {
            *libc::__error() = e;
        }
        br_return(cpu, r as u64);
        br_settle(vm, cpu)
    }
}

pub unsafe extern "C" fn br_ulock_wait(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { br_ulock(vm, cpu, 0) }
}
pub unsafe extern "C" fn br_ulock_wait2(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { br_ulock(vm, cpu, 1) }
}
pub unsafe extern "C" fn br_readv(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { br_iov_call(vm, cpu, 0) }
}
pub unsafe extern "C" fn br_writev(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { br_iov_call(vm, cpu, 1) }
}
pub unsafe extern "C" fn br_preadv(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { br_iov_call(vm, cpu, 2) }
}
pub unsafe extern "C" fn br_pwritev(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { br_iov_call(vm, cpu, 3) }
}

unsafe fn br_msg_call(vm: *mut OcerzVM, cpu: *mut OcerzCPU, recv: c_int) -> c_int {
    unsafe {
        let fd = gpr(cpu, OCERZ_RDI) as c_int;
        let flags = gpr(cpu, OCERZ_RDX) as c_int;
        let gm = gpr(cpu, OCERZ_RSI);
        let mut m: libc::msghdr = core::mem::zeroed();
        let mut e = if gm != 0 { 0 } else { libc::EFAULT };
        let mut v: *mut libc::iovec = null_mut();
        let mut r: isize = -1;
        if e == 0 {
            let name = ocerz_ld(gm, 8);
            let giov = ocerz_ld(gm + 16, 8);
            let control = ocerz_ld(gm + 32, 8);
            let cnt = ocerz_ld(gm + 24, 4) as u32 as i32 as i64;
            m.msg_name = if name != 0 {
                ocerz_g2h(name)
            } else {
                null_mut()
            };
            m.msg_namelen = ocerz_ld(gm + 8, 4) as u32;
            m.msg_iovlen = cnt as c_int;
            m.msg_control = if control != 0 {
                ocerz_g2h(control)
            } else {
                null_mut()
            };
            m.msg_controllen = ocerz_ld(gm + 40, 4) as u32;
            m.msg_flags = ocerz_ld(gm + 44, 4) as u32 as c_int;
            v = if cnt > 0 && cnt <= BR_IOV_MAX {
                libc::malloc(size_of::<libc::iovec>() * cnt as usize) as *mut libc::iovec
            } else {
                null_mut()
            };
            e = if cnt == 0 {
                0
            } else if !v.is_null() {
                br_iov_load(giov, cnt, v)
            } else {
                libc::EINVAL
            };
            m.msg_iov = v;
        }
        if e == 0 {
            r = if recv != 0 {
                libc::recvmsg(fd, &mut m, flags)
            } else {
                libc::sendmsg(fd, &m, flags)
            };
            e = if r < 0 { *libc::__error() } else { 0 };
            if recv != 0 && r >= 0 {
                ocerz_st(gm + 8, 4, m.msg_namelen as u64);
                ocerz_st(gm + 40, 4, m.msg_controllen as u64);
                ocerz_st(gm + 44, 4, m.msg_flags as u32 as u64);
            }
        }
        libc::free(v as *mut c_void);
        if e != 0 {
            *libc::__error() = e;
        }
        br_return(cpu, if e != 0 { (-1i64) as u64 } else { r as u64 });
        br_settle(vm, cpu)
    }
}

pub unsafe extern "C" fn br_sendmsg(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { br_msg_call(vm, cpu, 0) }
}
pub unsafe extern "C" fn br_recvmsg(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { br_msg_call(vm, cpu, 1) }
}

/// __pthread_kill names its thread by Mach port, wineserver's way of signalling
/// its clients' threads: cache mode's system call path routes the caller's own
/// thread through the guest's signal state and sends any other a host signal.
pub unsafe extern "C" fn br_pthread_kill_port(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut err = 0;
        let r = ocerz_guest_thread_port_kill(
            vm,
            cpu,
            gpr(cpu, OCERZ_RDI),
            gpr(cpu, OCERZ_RSI) as c_int,
            &mut err,
        );
        if r != 0 {
            *libc::__error() = err;
        }
        br_return(cpu, if r != 0 { (-1i64) as u64 } else { 0 });
        br_settle(vm, cpu)
    }
}

pub unsafe extern "C" fn br_nsgetexecutablepath(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let path = ocerz_dyld_main_path();
        let buf = gpr(cpu, OCERZ_RDI);
        let size_at = gpr(cpu, OCERZ_RSI);
        if path.is_null() || size_at == 0 {
            br_return(cpu, (-1i32) as u32 as u64);
            return br_settle(vm, cpu);
        }
        let need = libc::strlen(path) as u32 + 1;
        if (ocerz_ld(size_at, 4) as u32) < need {
            ocerz_st(size_at, 4, need as u64);
            br_return(cpu, (-1i32) as u32 as u64);
        } else {
            core::ptr::copy_nonoverlapping(path, ocerz_g2h(buf) as *mut c_char, need as usize);
            br_return(cpu, 0);
        }
        br_settle(vm, cpu)
    }
}

pub unsafe extern "C" fn br_nsgetmachexecuteheader(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_return(cpu, ocerz_main_mh);
        br_settle(vm, cpu)
    }
}

pub unsafe extern "C" fn br_dlopen(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut path = [0i8; 4096];
        let p = br_guest_path(gpr(cpu, OCERZ_RDI), path.as_mut_ptr(), path.len());
        let h = ocerz_dyld_native_dlopen(
            vm,
            p,
            gpr(cpu, OCERZ_RSI) as c_int,
            br_caller(cpu),
            br_stack_below(cpu),
        );
        br_answer(vm, cpu, h)
    }
}

pub unsafe extern "C" fn br_dlopen_from(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut path = [0i8; 4096];
        let p = br_guest_path(gpr(cpu, OCERZ_RDI), path.as_mut_ptr(), path.len());
        let h = ocerz_dyld_native_dlopen(
            vm,
            p,
            gpr(cpu, OCERZ_RSI) as c_int,
            gpr(cpu, OCERZ_RDX),
            br_stack_below(cpu),
        );
        br_answer(vm, cpu, h)
    }
}

pub unsafe extern "C" fn br_dlopen_preflight(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut path = [0i8; 4096];
        let p = br_guest_path(gpr(cpu, OCERZ_RDI), path.as_mut_ptr(), path.len());
        br_answer(
            vm,
            cpu,
            ocerz_dyld_native_dlopen_preflight(p, br_caller(cpu)) as u64,
        )
    }
}

pub unsafe extern "C" fn br_dlsym(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let name = gpr(cpu, OCERZ_RSI);
        br_answer(
            vm,
            cpu,
            ocerz_dyld_native_dlsym(
                gpr(cpu, OCERZ_RDI),
                if name != 0 {
                    ocerz_g2h(name) as *const c_char
                } else {
                    null()
                },
                br_caller(cpu),
            ),
        )
    }
}

pub unsafe extern "C" fn br_dladdr(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_answer(
            vm,
            cpu,
            ocerz_dyld_native_dladdr(gpr(cpu, OCERZ_RDI), gpr(cpu, OCERZ_RSI)) as u64,
        )
    }
}

pub unsafe extern "C" fn br_dyld_unwind_sections(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_answer(
            vm,
            cpu,
            ocerz_dyld_unwind_sections(gpr(cpu, OCERZ_RDI), gpr(cpu, OCERZ_RSI)) as u64,
        )
    }
}

#[repr(C)]
struct BrExcPort {
    task: u32,
    mask: u32,
    port: u32,
    behavior: c_int,
    flavor: c_int,
    valid: c_int,
}
static mut G_BR_EXC_PORTS: [BrExcPort; 8] = [const {
    BrExcPort {
        task: 0,
        mask: 0,
        port: 0,
        behavior: 0,
        flavor: 0,
        valid: 0,
    }
}; 8];

pub unsafe extern "C" fn br_task_set_exception_ports(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        let task = gpr(cpu, OCERZ_RDI) as u32;
        let mask = gpr(cpu, OCERZ_RSI) as u32;
        let port = gpr(cpu, OCERZ_RDX) as u32;
        let behavior = gpr(cpu, OCERZ_RCX) as c_int;
        let flavor = gpr(cpu, OCERZ_R8) as c_int;
        crate::ocerz_log!(
            "bridge: task_set_exception_ports task=%u mask=%#x port=%u behavior=%d flavor=%d recorded, faults stay with translated code\n",
            task,
            mask,
            port,
            behavior,
            flavor
        );
        let mut slot: i32 = -1;
        for k in 0..8 {
            if G_BR_EXC_PORTS[k].valid != 0 && G_BR_EXC_PORTS[k].task == task {
                slot = k as i32;
            }
        }
        if slot < 0 {
            let mut k = 0;
            while k < 8 && slot < 0 {
                if G_BR_EXC_PORTS[k].valid == 0 {
                    slot = k as i32;
                }
                k += 1;
            }
        }
        if slot < 0 {
            slot = 0;
        }
        let s = &raw mut G_BR_EXC_PORTS[slot as usize];
        (*s).task = task;
        (*s).mask = mask;
        (*s).port = port;
        (*s).behavior = behavior;
        (*s).flavor = flavor;
        (*s).valid = 1;
        br_answer(vm, cpu, 0)
    }
}

pub unsafe extern "C" fn br_swap_exception_ports(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let count_at = ocerz_ld(gpr(cpu, OCERZ_RSP) + 8, 8);
        if count_at != 0 {
            ocerz_st(count_at, 4, 0);
        }
        br_task_set_exception_ports(vm, cpu)
    }
}

pub unsafe extern "C" fn br_abort_report(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let fmt = if gpr(cpu, OCERZ_RDI) != 0 {
            ocerz_g2h(gpr(cpu, OCERZ_RDI)) as *const c_char
        } else {
            c"(no message)".as_ptr()
        };
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: bridge: the guest called abort_report_np: %s\n".as_ptr(),
            fmt,
        );
        br_abort(vm, cpu)
    }
}

pub unsafe extern "C" fn br_thread_suspend(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let port = gpr(cpu, OCERZ_RDI) as u32;
        let kr = ocerz_vm_thread_suspend_native(port);
        if br_susp_logging() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: SUSPLOG[%d] suspend port=%#x answer=%d\n".as_ptr(),
                libc::getpid(),
                port,
                kr,
            );
        }
        br_answer(vm, cpu, kr as u32 as u64)
    }
}

pub unsafe extern "C" fn br_thread_resume(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let port = gpr(cpu, OCERZ_RDI) as u32;
        let kr = ocerz_vm_thread_resume_native(port);
        if br_susp_logging() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: SUSPLOG[%d] resume port=%#x answer=%d\n".as_ptr(),
                libc::getpid(),
                port,
                kr,
            );
        }
        br_answer(vm, cpu, kr as u32 as u64)
    }
}

const BR_X86_DEBUG_STATE32: u32 = 10;
const BR_X86_DEBUG_STATE64: u32 = 11;
const BR_X86_DEBUG_STATE: u32 = 12;

fn br_debug_state_count(flavor: u32) -> u32 {
    if flavor == BR_X86_DEBUG_STATE32 {
        8
    } else if flavor == BR_X86_DEBUG_STATE64 {
        16
    } else if flavor == BR_X86_DEBUG_STATE {
        18
    } else {
        0
    }
}

unsafe fn br_debug_state_get(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    flavor: u32,
    state: u64,
    countp: u64,
) -> c_int {
    unsafe {
        let need = br_debug_state_count(flavor);
        if state == 0 || countp == 0 || (ocerz_ld(countp, 4) as u32) < need {
            return br_answer(vm, cpu, KERN_INVALID_ARG as u32 as u64);
        }
        for k in 0..need {
            ocerz_st(state + 4 * k as u64, 4, 0);
        }
        if flavor == BR_X86_DEBUG_STATE {
            ocerz_st(state, 4, BR_X86_DEBUG_STATE64 as u64);
            ocerz_st(state + 4, 4, 16);
        }
        ocerz_st(countp, 4, need as u64);
        br_answer(vm, cpu, libc::KERN_SUCCESS as u64)
    }
}

/// Setting them is accepted while every register stays zero, which is what a
/// context restore or a debugger clearing its breakpoints writes; a breakpoint
/// ocerz cannot raise is refused rather than dropped.
pub unsafe extern "C" fn br_thread_set_state(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let flavor = gpr(cpu, OCERZ_RSI) as u32;
        let state = gpr(cpu, OCERZ_RDX);
        let count = gpr(cpu, OCERZ_RCX) as u32;
        let need = br_debug_state_count(flavor);
        if need == 0 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: bridge: _thread_set_state takes flavor %u, which has no x86 register mapping here\n".as_ptr(),
                flavor,
            );
            libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
        }
        if state == 0 || count < need {
            return br_answer(vm, cpu, KERN_INVALID_ARG as u32 as u64);
        }
        let mut k = if flavor == BR_X86_DEBUG_STATE { 2 } else { 0 };
        while k < need {
            if ocerz_ld(state + 4 * k as u64, 4) != 0 {
                return br_answer(vm, cpu, KERN_INVALID_ARG as u32 as u64);
            }
            k += 1;
        }
        br_answer(vm, cpu, libc::KERN_SUCCESS as u64)
    }
}

pub unsafe extern "C" fn br_thread_get_state(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let port = gpr(cpu, OCERZ_RDI) as u32;
        let flavor = gpr(cpu, OCERZ_RSI) as u32;
        let state = gpr(cpu, OCERZ_RDX);
        let countp = gpr(cpu, OCERZ_RCX);
        if br_debug_state_count(flavor) != 0 {
            return br_debug_state_get(vm, cpu, flavor, state, countp);
        }
        if flavor != 4 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: bridge: _thread_get_state takes flavor %u, which has no x86 register mapping here\n".as_ptr(),
                flavor,
            );
            libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
        }
        if state == 0 || countp == 0 {
            return br_answer(vm, cpu, libc::KERN_FAILURE as u64);
        }
        let mut g = [0u64; 16];
        let mut rip = 0u64;
        let mut rfl = 0u64;
        if ocerz_vm_thread_regs(port, g.as_mut_ptr(), &mut rip, &mut rfl) < 0 {
            return br_answer(vm, cpu, KERN_INVALID_ARG as u32 as u64);
        }
        let want = ocerz_ld(countp, 4) as u32;
        if want < 42 {
            return br_answer(vm, cpu, KERN_INVALID_ARG as u32 as u64);
        }
        let s = [
            g[OCERZ_RAX as usize],
            g[OCERZ_RBX as usize],
            g[OCERZ_RCX as usize],
            g[OCERZ_RDX as usize],
            g[OCERZ_RDI as usize],
            g[OCERZ_RSI as usize],
            g[OCERZ_RBP as usize],
            g[OCERZ_RSP as usize],
            g[8],
            g[9],
            g[10],
            g[11],
            g[12],
            g[13],
            g[14],
            g[15],
            rip,
            rfl | crate::inline::OCERZ_FLAG_FIXED1,
            0x2b,
            0,
            0,
        ];
        for k in 0..21 {
            ocerz_st(state + 8 * k as u64, 8, s[k]);
        }
        ocerz_st(countp, 4, 42);
        br_answer(vm, cpu, 0)
    }
}

// ---- CoreFoundation calendar helpers: variadic component lists ----

static G_BR_CF_CAL_FNS: [AtomicPtr<c_void>; 4] = [
    AtomicPtr::new(null_mut()),
    AtomicPtr::new(null_mut()),
    AtomicPtr::new(null_mut()),
    AtomicPtr::new(null_mut()),
];

unsafe fn br_cf_calendar(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    name: *const c_char,
    notation: *const c_char,
    comp: c_char,
) -> c_int {
    unsafe {
        const NAMES: [*const c_char; 4] = [
            c"CFCalendarComposeAbsoluteTime".as_ptr(),
            c"CFCalendarDecomposeAbsoluteTime".as_ptr(),
            c"CFCalendarAddComponents".as_ptr(),
            c"CFCalendarGetComponentDifference".as_ptr(),
        ];
        let mut which = 0;
        while which < 3 && libc::strcmp(NAMES[which], name) != 0 {
            which += 1;
        }
        let mut f = G_BR_CF_CAL_FNS[which].load(Ordering::SeqCst);
        if f.is_null() {
            f = ocerz_bridge_host_symbol(
                OCERZ_BRIDGE_COREFOUNDATION.as_ptr() as *const c_char,
                name,
            );
            if f.is_null() {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: bridge: _%s has no host symbol\n".as_ptr(),
                    name,
                );
                libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
            }
            G_BR_CF_CAL_FNS[which].store(f, Ordering::SeqCst);
        }
        let mut named: OcerzAbiSig = core::mem::zeroed();
        let mut call: OcerzAbiCall = core::mem::zeroed();
        let mut va: OcerzAbiVaList = core::mem::zeroed();
        if ocerz_abi_parse(notation, &mut named) != OCERZ_OK as c_int
            || ocerz_abi_read_guest(&named, cpu, &mut call) != OCERZ_OK as c_int
            || call.nx < 1
            || ocerz_abi_va_start(&named, cpu, &mut va) != OCERZ_OK as c_int
        {
            return br_answer(vm, cpu, 0);
        }
        let desc = call.x[(call.nx - 1) as usize] as *const c_char;
        let n = if desc.is_null() {
            0
        } else {
            libc::strlen(desc)
        };
        let mut slots = [0u64; 32];
        if n > 32 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: bridge: _%s was given %zu components, more than ocerz carries\n".as_ptr(),
                name,
                n,
            );
            libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
        }
        for k in 0..n {
            let mut w = 0u64;
            ocerz_abi_va_arg(&mut va, cpu, comp, &mut w);
            slots[k] = if comp == b'p' as c_char {
                if w != 0 { ocerz_g2h(w) as u64 } else { 0 }
            } else {
                w as i32 as i64 as u64
            };
        }
        let mut outer: OcerzBridgeFrame = core::mem::zeroed();
        ocerz_bridge_raise(
            &mut outer,
            OCERZ_BRIDGE_COREFOUNDATION.as_ptr() as *const c_char,
            name,
            notation,
            f,
        );
        ocerz_abi_call_native(
            f,
            call.x.as_ptr(),
            call.v.as_ptr(),
            slots.as_ptr(),
            8 * n as u64,
            null_mut(),
            call.rx.as_mut_ptr(),
            call.rv.as_mut_ptr(),
        );
        ocerz_bridge_lower(&outer);
        ocerz_abi_write_result(&named, cpu, &mut call);
        br_settle(vm, cpu)
    }
}

pub unsafe extern "C" fn br_cf_calendar_compose(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_cf_calendar(
            vm,
            cpu,
            c"CFCalendarComposeAbsoluteTime".as_ptr(),
            c"B(ppp)".as_ptr(),
            'i' as c_char,
        )
    }
}
pub unsafe extern "C" fn br_cf_calendar_decompose(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_cf_calendar(
            vm,
            cpu,
            c"CFCalendarDecomposeAbsoluteTime".as_ptr(),
            c"B(pdp)".as_ptr(),
            'p' as c_char,
        )
    }
}
pub unsafe extern "C" fn br_cf_calendar_add(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_cf_calendar(
            vm,
            cpu,
            c"CFCalendarAddComponents".as_ptr(),
            c"B(ppLp)".as_ptr(),
            'i' as c_char,
        )
    }
}
pub unsafe extern "C" fn br_cf_calendar_difference(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_cf_calendar(
            vm,
            cpu,
            c"CFCalendarGetComponentDifference".as_ptr(),
            c"B(pddLp)".as_ptr(),
            'p' as c_char,
        )
    }
}

const BR_SQLITE: &[u8] = b"/usr/lib/libsqlite3.dylib\0";

unsafe fn br_sqlite_config_args(op: c_int, callback: *mut *const c_char) -> *const c_char {
    unsafe { *callback = null() }
    match op {
        1 | 2 | 3 | 14 | 15 => c"".as_ptr(),
        6 | 7 | 8 => c"pii".as_ptr(),
        9 | 17 | 20 | 23 | 26 | 27 | 28 => c"i".as_ptr(),
        13 => c"ii".as_ptr(),
        16 => {
            unsafe { *callback = c"v(pip)".as_ptr() }
            c"cp".as_ptr()
        }
        21 => {
            unsafe { *callback = c"v(pppi)".as_ptr() }
            c"cp".as_ptr()
        }
        22 => c"ll".as_ptr(),
        24 | 30 => c"p".as_ptr(),
        25 => c"u".as_ptr(),
        29 => c"l".as_ptr(),
        4 | 5 | 10 | 11 | 18 | 19 => null(),
        _ => c"".as_ptr(),
    }
}

unsafe fn br_sqlite_db_config_args(op: c_int, callback: *mut *const c_char) -> *const c_char {
    unsafe { *callback = null() }
    if op == 1000 {
        return c"p".as_ptr();
    }
    if op == 1001 {
        return c"pii".as_ptr();
    }
    if op > 1001 && op <= 1023 {
        return c"ip".as_ptr();
    }
    c"".as_ptr()
}

unsafe fn br_sqlite_variadic(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    cache: *const AtomicPtr<c_void>,
    name: *const c_char,
    named_notation: *const c_char,
    op_index: usize,
    classes_of: unsafe fn(c_int, *mut *const c_char) -> *const c_char,
) -> c_int {
    unsafe {
        let mut f = (*cache).load(Ordering::SeqCst);
        if f.is_null() {
            f = ocerz_bridge_host_symbol(BR_SQLITE.as_ptr() as *const c_char, name);
            if f.is_null() {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: bridge: _%s has no host symbol\n".as_ptr(),
                    name,
                );
                libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
            }
            (*cache).store(f, Ordering::SeqCst);
        }
        let mut named: OcerzAbiSig = core::mem::zeroed();
        let mut call: OcerzAbiCall = core::mem::zeroed();
        let mut va: OcerzAbiVaList = core::mem::zeroed();
        if ocerz_abi_parse(named_notation, &mut named) != OCERZ_OK as c_int
            || ocerz_abi_read_guest(&named, cpu, &mut call) != OCERZ_OK as c_int
            || ocerz_abi_va_start(&named, cpu, &mut va) != OCERZ_OK as c_int
        {
            return br_answer(vm, cpu, 1);
        }
        let op = call.x[op_index] as c_int;
        let mut callback: *const c_char = null();
        let classes = classes_of(op, &mut callback);
        if classes.is_null() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: bridge: %s _%s op %d installs a table of functions, which does not cross\n".as_ptr(),
                BR_SQLITE.as_ptr() as *const c_char,
                name,
                op,
            );
            libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
        }
        let mut slots = [0u64; 4];
        let mut n = 0;
        let mut c = classes;
        while *c != 0 {
            let mut w = 0u64;
            ocerz_abi_va_arg(
                &mut va,
                cpu,
                if *c == b'c' as c_char {
                    b'p' as c_char
                } else {
                    *c
                },
                &mut w,
            );
            if *c == b'c' as c_char
                && w != 0
                && ocerz_abi_callback_convert(w, callback, &mut w) != OCERZ_OK as c_int
            {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: bridge: _%s op %d could not bind guest function %#llx\n".as_ptr(),
                    name,
                    op,
                    w,
                );
                libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
            }
            if *c == b'p' as c_char || *c == b'c' as c_char {
                w = if w != 0 { ocerz_g2h(w) as u64 } else { 0 };
            } else if *c == b'i' as c_char {
                w = w as i32 as i64 as u64;
            } else if *c == b'u' as c_char {
                w = w as u32 as u64;
            }
            slots[n] = w;
            n += 1;
            c = c.add(1);
        }
        let mut outer: OcerzBridgeFrame = core::mem::zeroed();
        ocerz_bridge_raise(
            &mut outer,
            BR_SQLITE.as_ptr() as *const c_char,
            name,
            named_notation,
            f,
        );
        ocerz_abi_call_native(
            f,
            call.x.as_ptr(),
            call.v.as_ptr(),
            slots.as_ptr(),
            8 * n as u64,
            null_mut(),
            call.rx.as_mut_ptr(),
            call.rv.as_mut_ptr(),
        );
        ocerz_bridge_lower(&outer);
        ocerz_abi_write_result(&named, cpu, &mut call);
        br_settle(vm, cpu)
    }
}

static G_BR_SQLITE3_CONFIG: AtomicPtr<c_void> = AtomicPtr::new(null_mut());
static G_BR_SQLITE3_DB_CONFIG: AtomicPtr<c_void> = AtomicPtr::new(null_mut());

pub unsafe extern "C" fn br_sqlite3_config(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_sqlite_variadic(
            vm,
            cpu,
            &G_BR_SQLITE3_CONFIG,
            c"sqlite3_config".as_ptr(),
            c"i(i)".as_ptr(),
            0,
            br_sqlite_config_args,
        )
    }
}
pub unsafe extern "C" fn br_sqlite3_db_config(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_sqlite_variadic(
            vm,
            cpu,
            &G_BR_SQLITE3_DB_CONFIG,
            c"sqlite3_db_config".as_ptr(),
            c"i(pi)".as_ptr(),
            1,
            br_sqlite_db_config_args,
        )
    }
}

const BR_COREGRAPHICS: &[u8] =
    b"/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics\0";

/// A constructor whose callback structure no shape can describe, having no
/// version word or arriving where no struct record looks: a copy of the
/// guest's structure on this stack, each function word bound to a callback,
/// goes to the host in its place, which the constructor copies in turn.
unsafe fn br_struct_cross(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    lib: *const c_char,
    cache: *const AtomicPtr<c_void>,
    name: *const c_char,
    notation: *const c_char,
    reg: u32,
    words: *const *const c_char,
    nwords: c_int,
) -> c_int {
    unsafe {
        let mut f = (*cache).load(Ordering::SeqCst);
        if f.is_null() {
            f = ocerz_bridge_host_symbol(lib, name);
            if f.is_null() {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: bridge: _%s has no host symbol\n".as_ptr(),
                    name,
                );
                libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
            }
            (*cache).store(f, Ordering::SeqCst);
        }
        let mut sig: OcerzAbiSig = core::mem::zeroed();
        if ocerz_abi_parse(notation, &mut sig) != OCERZ_OK as c_int {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: bridge: _%s has a notation ocerz cannot parse\n".as_ptr(),
                name,
            );
            libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
        }
        let mut copy = [0u64; 8];
        let at = gpr(cpu, reg);
        if at != 0 {
            for w in 0..nwords as usize {
                let mut v = ocerz_ld(at + 8 * w as u64, 8);
                let wn = *words.add(w);
                if !wn.is_null()
                    && v != 0
                    && ocerz_abi_is_guest_code(v) != 0
                    && ocerz_abi_callback_convert(v, wn, &mut v) != OCERZ_OK as c_int
                {
                    libc::fprintf(
                        crate::log::stderr(),
                        c"ocerz: bridge: _%s could not bind guest function %#llx in word %d of its callbacks\n".as_ptr(),
                        name,
                        ocerz_ld(at + 8 * w as u64, 8),
                        w,
                    );
                    libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
                }
                copy[w] = v;
            }
            set_gpr(cpu, reg, ocerz_h2g(copy.as_ptr() as *const c_void));
        }
        let mut outer: OcerzBridgeFrame = core::mem::zeroed();
        ocerz_bridge_raise(&mut outer, lib, name, notation, f);
        let rc = ocerz_abi_perform(&sig, f, cpu);
        ocerz_bridge_lower(&outer);
        if rc != OCERZ_STEP_OK as c_int {
            return rc;
        }
        br_settle(vm, cpu)
    }
}

static G_BR_CG_CONSUMER: AtomicPtr<c_void> = AtomicPtr::new(null_mut());
static G_BR_CG_PATTERN: AtomicPtr<c_void> = AtomicPtr::new(null_mut());

pub unsafe extern "C" fn br_cg_data_consumer_create(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let words: [*const c_char; 2] = [c"L(ppL)".as_ptr(), c"v(p)".as_ptr()];
        br_struct_cross(
            vm,
            cpu,
            BR_COREGRAPHICS.as_ptr() as *const c_char,
            &G_BR_CG_CONSUMER,
            c"CGDataConsumerCreate".as_ptr(),
            c"p(pp)".as_ptr(),
            OCERZ_RSI,
            words.as_ptr(),
            2,
        )
    }
}

pub unsafe extern "C" fn br_cg_pattern_create(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let words: [*const c_char; 3] = [null(), c"v(pp)".as_ptr(), c"v(p)".as_ptr()];
        br_struct_cross(
            vm,
            cpu,
            BR_COREGRAPHICS.as_ptr() as *const c_char,
            &G_BR_CG_PATTERN,
            c"CGPatternCreate".as_ptr(),
            c"p(p{{dd}{dd}}{dddddd}ddiBp)".as_ptr(),
            OCERZ_RCX,
            words.as_ptr(),
            3,
        )
    }
}

const BR_COREMEDIA: &[u8] =
    b"/System/Library/Frameworks/CoreMedia.framework/Versions/A/CoreMedia\0";
const BR_BLOCK_SOURCE_BYTES: usize = 28;

unsafe fn br_block_source_cross(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    cache: *const AtomicPtr<c_void>,
    name: *const c_char,
    notation: *const c_char,
    reg: u32,
) -> c_int {
    unsafe {
        let mut f = (*cache).load(Ordering::SeqCst);
        if f.is_null() {
            f = ocerz_bridge_host_symbol(BR_COREMEDIA.as_ptr() as *const c_char, name);
            if f.is_null() {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: bridge: _%s has no host symbol\n".as_ptr(),
                    name,
                );
                libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
            }
            (*cache).store(f, Ordering::SeqCst);
        }
        let mut sig: OcerzAbiSig = core::mem::zeroed();
        if ocerz_abi_parse(notation, &mut sig) != OCERZ_OK as c_int {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: bridge: _%s has a notation ocerz cannot parse\n".as_ptr(),
                name,
            );
            libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
        }
        #[repr(C)]
        struct Field {
            at: u32,
            notation: *const c_char,
        }
        let fields: [Field; 2] = [
            Field {
                at: 4,
                notation: c"p(pL)".as_ptr(),
            },
            Field {
                at: 12,
                notation: c"v(ppL)".as_ptr(),
            },
        ];
        let mut copy = [0u8; BR_BLOCK_SOURCE_BYTES + 4];
        let at = gpr(cpu, reg);
        if at != 0 {
            core::ptr::copy_nonoverlapping(
                ocerz_g2h(at) as *const u8,
                copy.as_mut_ptr(),
                BR_BLOCK_SOURCE_BYTES,
            );
            for k in 0..2 {
                let mut v: u64 = 0;
                core::ptr::copy_nonoverlapping(
                    copy.as_ptr().add(fields[k].at as usize),
                    &mut v as *mut u64 as *mut u8,
                    8,
                );
                if v != 0
                    && ocerz_abi_is_guest_code(v) != 0
                    && ocerz_abi_callback_convert(v, fields[k].notation, &mut v)
                        != OCERZ_OK as c_int
                {
                    libc::fprintf(
                        crate::log::stderr(),
                        c"ocerz: bridge: _%s could not bind a guest function of its custom block source\n".as_ptr(),
                        name,
                    );
                    libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
                }
                core::ptr::copy_nonoverlapping(
                    &v as *const u64 as *const u8,
                    copy.as_mut_ptr().add(fields[k].at as usize),
                    8,
                );
            }
            set_gpr(cpu, reg, ocerz_h2g(copy.as_ptr() as *const c_void));
        }
        let mut outer: OcerzBridgeFrame = core::mem::zeroed();
        ocerz_bridge_raise(
            &mut outer,
            BR_COREMEDIA.as_ptr() as *const c_char,
            name,
            notation,
            f,
        );
        let rc = ocerz_abi_perform(&sig, f, cpu);
        ocerz_bridge_lower(&outer);
        if rc != OCERZ_STEP_OK as c_int {
            return rc;
        }
        br_settle(vm, cpu)
    }
}

static G_BR_CM_MBLOCK: AtomicPtr<c_void> = AtomicPtr::new(null_mut());
static G_BR_CM_ABLOCK: AtomicPtr<c_void> = AtomicPtr::new(null_mut());
static G_BR_CM_CONTIG: AtomicPtr<c_void> = AtomicPtr::new(null_mut());

pub unsafe extern "C" fn br_cm_block_buffer_create_with_memory_block(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        br_block_source_cross(
            vm,
            cpu,
            &G_BR_CM_MBLOCK,
            c"CMBlockBufferCreateWithMemoryBlock".as_ptr(),
            c"i(ppLppLLup)".as_ptr(),
            OCERZ_R8,
        )
    }
}
pub unsafe extern "C" fn br_cm_block_buffer_append_memory_block(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        br_block_source_cross(
            vm,
            cpu,
            &G_BR_CM_ABLOCK,
            c"CMBlockBufferAppendMemoryBlock".as_ptr(),
            c"i(ppLppLLu)".as_ptr(),
            OCERZ_R8,
        )
    }
}
pub unsafe extern "C" fn br_cm_block_buffer_create_contiguous(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        br_block_source_cross(
            vm,
            cpu,
            &G_BR_CM_CONTIG,
            c"CMBlockBufferCreateContiguous".as_ptr(),
            c"i(ppppLLup)".as_ptr(),
            OCERZ_RCX,
        )
    }
}

const BR_AUDIO_TOOLBOX: &[u8] =
    b"/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox\0";
const BR_AU_SET_RENDER_CALLBACK: u32 = 23;
const BR_AU_HOST_CALLBACKS: u32 = 27;
const BR_AU_MIDI_OUTPUT_CALLBACK: u32 = 48;
const BR_AU_INPUT_SAMPLES_IN_OUTPUT: u32 = 49;
const BR_AU_SET_INPUT_CALLBACK: u32 = 2005;

static G_BR_AU_SET_PROPERTY: AtomicPtr<c_void> = AtomicPtr::new(null_mut());

pub unsafe extern "C" fn br_audio_unit_set_property(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut f = G_BR_AU_SET_PROPERTY.load(Ordering::SeqCst);
        if f.is_null() {
            f = ocerz_bridge_host_symbol(
                BR_AUDIO_TOOLBOX.as_ptr() as *const c_char,
                c"AudioUnitSetProperty".as_ptr(),
            );
            if f.is_null() {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: bridge: _AudioUnitSetProperty has no host symbol\n".as_ptr(),
                );
                libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
            }
            G_BR_AU_SET_PROPERTY.store(f, Ordering::SeqCst);
        }
        let unit = gpr(cpu, OCERZ_RDI);
        let data = gpr(cpu, OCERZ_R8);
        let id = gpr(cpu, OCERZ_RSI) as u32;
        let scope = gpr(cpu, OCERZ_RDX) as u32;
        let element = gpr(cpu, OCERZ_RCX) as u32;
        let size = gpr(cpu, OCERZ_R9) as u32;
        let mut bytes = if data != 0 {
            ocerz_g2h(data) as *const c_void
        } else {
            null()
        };
        #[repr(C)]
        struct Cb {
            proc_: u64,
            refcon: u64,
        }
        let mut cb = Cb {
            proc_: 0,
            refcon: 0,
        };
        if (id == BR_AU_SET_RENDER_CALLBACK || id == BR_AU_SET_INPUT_CALLBACK)
            && data != 0
            && size as usize >= size_of::<Cb>()
        {
            let mut proc_: u64 = 0;
            if ocerz_abi_callback_convert(ocerz_ld(data, 8), c"i(pppuup)".as_ptr(), &mut proc_)
                != OCERZ_OK as c_int
            {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: bridge: _AudioUnitSetProperty: no callback slot is left for property %u\n".as_ptr(),
                    id,
                );
                libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
            }
            cb.proc_ = if proc_ != 0 {
                ocerz_g2h(proc_) as u64
            } else {
                0
            };
            cb.refcon = ocerz_ld(data + 8, 8);
            bytes = &cb as *const Cb as *const c_void;
        } else if id == BR_AU_HOST_CALLBACKS
            || id == BR_AU_MIDI_OUTPUT_CALLBACK
            || id == BR_AU_INPUT_SAMPLES_IN_OUTPUT
        {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: bridge: _AudioUnitSetProperty: property %u holds callbacks ocerz does not convert\n".as_ptr(),
                id,
            );
            libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
        }
        let mut outer: OcerzBridgeFrame = core::mem::zeroed();
        ocerz_bridge_raise(
            &mut outer,
            BR_AUDIO_TOOLBOX.as_ptr() as *const c_char,
            c"_AudioUnitSetProperty".as_ptr(),
            c"i(puuupu)".as_ptr(),
            f,
        );
        let f: unsafe extern "C" fn(*mut c_void, u32, u32, u32, *const c_void, u32) -> i32 =
            core::mem::transmute(f);
        let st = f(
            if unit != 0 {
                ocerz_g2h(unit)
            } else {
                null_mut()
            },
            id,
            scope,
            element,
            bytes,
            size,
        );
        ocerz_bridge_lower(&outer);
        br_answer(vm, cpu, st as i64 as u64)
    }
}

static G_BR_VT_DECOMP: AtomicPtr<c_void> = AtomicPtr::new(null_mut());

pub unsafe extern "C" fn br_vt_decompression_session_create(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        let words: [*const c_char; 2] = [c"v(ppiup{liul}{liul})".as_ptr(), null()];
        br_struct_cross(
            vm,
            cpu,
            c"/System/Library/Frameworks/VideoToolbox.framework/Versions/A/VideoToolbox".as_ptr(),
            &G_BR_VT_DECOMP,
            c"VTDecompressionSessionCreate".as_ptr(),
            c"i(pppppp)".as_ptr(),
            OCERZ_R8,
            words.as_ptr(),
            2,
        )
    }
}

const BR_SECURITY: &[u8] = b"/System/Library/Frameworks/Security.framework/Versions/A/Security\0";

unsafe fn br_security_symbol(name: *const c_char) -> *mut c_void {
    unsafe {
        let f = ocerz_bridge_host_symbol(BR_SECURITY.as_ptr() as *const c_char, name);
        if f.is_null() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: bridge: _%s has no host symbol\n".as_ptr(),
                name,
            );
            libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
        }
        f
    }
}

unsafe fn br_ssl_ciphers_out(vm: *mut OcerzVM, cpu: *mut OcerzCPU, name: *const c_char) -> c_int {
    unsafe {
        let f = br_security_symbol(name);
        let f: unsafe extern "C" fn(*mut c_void, *mut u16, *mut usize) -> i32 =
            core::mem::transmute(f);
        let ctx = gpr(cpu, OCERZ_RDI);
        let ciphers = gpr(cpu, OCERZ_RSI);
        let nump = gpr(cpu, OCERZ_RDX);
        let cap = if nump != 0 {
            ocerz_ld(nump, 8) as usize
        } else {
            0
        };
        let mut n = cap;
        let buf = if ciphers != 0 && cap != 0 {
            libc::calloc(cap, size_of::<u16>()) as *mut u16
        } else {
            null_mut()
        };
        if ciphers != 0 && cap != 0 && buf.is_null() {
            return br_answer(vm, cpu, (-108i64) as u64);
        }
        let mut outer: OcerzBridgeFrame = core::mem::zeroed();
        ocerz_bridge_raise(
            &mut outer,
            BR_SECURITY.as_ptr() as *const c_char,
            name,
            c"i(ppp)".as_ptr(),
            f as *const c_void,
        );
        let st = f(
            if ctx != 0 { ocerz_g2h(ctx) } else { null_mut() },
            if ciphers != 0 { buf } else { null_mut() },
            if nump != 0 { &mut n } else { null_mut() },
        );
        ocerz_bridge_lower(&outer);
        let mut k = 0;
        while !buf.is_null() && k < n && k < cap {
            ocerz_st(ciphers + 4 * k as u64, 4, *buf.add(k) as u64);
            k += 1;
        }
        if nump != 0 {
            ocerz_st(nump, 8, n as u64);
        }
        libc::free(buf as *mut c_void);
        br_answer(vm, cpu, st as i64 as u64)
    }
}

pub unsafe extern "C" fn br_ssl_get_supported_ciphers(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe { br_ssl_ciphers_out(vm, cpu, c"SSLGetSupportedCiphers".as_ptr()) }
}
pub unsafe extern "C" fn br_ssl_get_enabled_ciphers(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { br_ssl_ciphers_out(vm, cpu, c"SSLGetEnabledCiphers".as_ptr()) }
}

pub unsafe extern "C" fn br_ssl_set_enabled_ciphers(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let f = br_security_symbol(c"SSLSetEnabledCiphers".as_ptr());
        let f: unsafe extern "C" fn(*mut c_void, *const u16, usize) -> i32 =
            core::mem::transmute(f);
        let ctx = gpr(cpu, OCERZ_RDI);
        let ciphers = gpr(cpu, OCERZ_RSI);
        let n = gpr(cpu, OCERZ_RDX) as usize;
        let buf = if ciphers != 0 && n != 0 {
            libc::calloc(n, size_of::<u16>()) as *mut u16
        } else {
            null_mut()
        };
        if ciphers != 0 && n != 0 && buf.is_null() {
            return br_answer(vm, cpu, (-108i64) as u64);
        }
        let mut k = 0;
        while !buf.is_null() && k < n {
            *buf.add(k) = ocerz_ld(ciphers + 4 * k as u64, 4) as u16;
            k += 1;
        }
        let mut outer: OcerzBridgeFrame = core::mem::zeroed();
        ocerz_bridge_raise(
            &mut outer,
            BR_SECURITY.as_ptr() as *const c_char,
            c"SSLSetEnabledCiphers".as_ptr(),
            c"i(ppL)".as_ptr(),
            f as *const c_void,
        );
        let st = f(
            if ctx != 0 { ocerz_g2h(ctx) } else { null_mut() },
            if ciphers != 0 { buf } else { null() },
            n,
        );
        ocerz_bridge_lower(&outer);
        libc::free(buf as *mut c_void);
        br_answer(vm, cpu, st as i64 as u64)
    }
}

pub unsafe extern "C" fn br_ssl_get_negotiated_cipher(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        let f = br_security_symbol(c"SSLGetNegotiatedCipher".as_ptr());
        let f: unsafe extern "C" fn(*mut c_void, *mut u16) -> i32 = core::mem::transmute(f);
        let ctx = gpr(cpu, OCERZ_RDI);
        let out = gpr(cpu, OCERZ_RSI);
        let mut v: u16 = 0;
        let mut outer: OcerzBridgeFrame = core::mem::zeroed();
        ocerz_bridge_raise(
            &mut outer,
            BR_SECURITY.as_ptr() as *const c_char,
            c"SSLGetNegotiatedCipher".as_ptr(),
            c"i(pp)".as_ptr(),
            f as *const c_void,
        );
        let st = f(
            if ctx != 0 { ocerz_g2h(ctx) } else { null_mut() },
            if out != 0 { &mut v } else { null_mut() },
        );
        ocerz_bridge_lower(&outer);
        if out != 0 {
            ocerz_st(out, 4, v as u64);
        }
        br_answer(vm, cpu, st as i64 as u64)
    }
}

type BrCFUUIDFn = unsafe extern "C" fn(
    *const c_void,
    c_uint,
    c_uint,
    c_uint,
    c_uint,
    c_uint,
    c_uint,
    c_uint,
    c_uint,
    c_uint,
    c_uint,
    c_uint,
    c_uint,
    c_uint,
    c_uint,
    c_uint,
    c_uint,
) -> *const c_void;

static G_BR_CFUUID_FN: AtomicPtr<c_void> = AtomicPtr::new(null_mut());

pub unsafe extern "C" fn br_cfuuid_constant(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut f = G_BR_CFUUID_FN.load(Ordering::SeqCst);
        if f.is_null() {
            f = ocerz_bridge_host_symbol(
                OCERZ_BRIDGE_COREFOUNDATION.as_ptr() as *const c_char,
                c"CFUUIDGetConstantUUIDWithBytes".as_ptr(),
            );
            if f.is_null() {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: bridge: _CFUUIDGetConstantUUIDWithBytes has no host symbol\n".as_ptr(),
                );
                libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
            }
            G_BR_CFUUID_FN.store(f, Ordering::SeqCst);
        }
        let mut named: OcerzAbiSig = core::mem::zeroed();
        if ocerz_abi_parse(c"p".as_ptr(), &mut named) != OCERZ_OK as c_int {
            return br_answer(vm, cpu, 0);
        }
        let mut va: OcerzAbiVaList = core::mem::zeroed();
        if ocerz_abi_va_start(&named, cpu, &mut va) != OCERZ_OK as c_int {
            return br_answer(vm, cpu, 0);
        }
        let raw = gpr(cpu, OCERZ_RDI);
        let alloc = if raw != 0 {
            ocerz_g2h(raw) as *const c_void
        } else {
            null()
        };
        let mut b = [0u32; 16];
        for k in 0..16 {
            let mut v = 0u64;
            if ocerz_abi_va_arg(&mut va, cpu, b'u' as c_char, &mut v) != OCERZ_OK as c_int {
                return br_answer(vm, cpu, 0);
            }
            b[k] = (v & 0xff) as u32;
        }
        let f: BrCFUUIDFn = core::mem::transmute(f);
        let r = f(
            alloc, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12],
            b[13], b[14], b[15],
        );
        br_answer(vm, cpu, if r.is_null() { 0 } else { ocerz_h2g(r) })
    }
}

pub unsafe extern "C" fn br_availability_version_check(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        let want = gpr(cpu, OCERZ_RDI) as u32;
        let mut buf = [0i8; 32];
        let mut len = buf.len();
        let mut have = 0u32;
        if libc::sysctlbyname(
            c"kern.osproductversion".as_ptr(),
            buf.as_mut_ptr() as *mut c_void,
            &mut len,
            null_mut(),
            0,
        ) == 0
            && len > 0
            && len < 32
        {
            let mut maj = 0u32;
            let mut min = 0u32;
            let mut pat = 0u32;
            libc::sscanf(
                buf.as_ptr(),
                c"%u.%u.%u".as_ptr(),
                &mut maj,
                &mut min,
                &mut pat,
            );
            if maj > 0xffff {
                maj = 0xffff;
            }
            if min > 0xff {
                min = 0xff;
            }
            if pat > 0xff {
                pat = 0xff;
            }
            have = (maj << 16) | (min << 8) | pat;
        }
        br_answer(vm, cpu, if have >= want { 1 } else { 0 })
    }
}

pub unsafe extern "C" fn br_dlclose(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_answer(
            vm,
            cpu,
            ocerz_dyld_native_dlclose(gpr(cpu, OCERZ_RDI)) as u32 as u64,
        )
    }
}
pub unsafe extern "C" fn br_dlerror(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { br_answer(vm, cpu, ocerz_dyld_native_dlerror()) }
}
pub unsafe extern "C" fn br_dyld_image_count(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { br_answer(vm, cpu, ocerz_dyld_image_count() as u64) }
}
pub unsafe extern "C" fn br_dyld_image_header(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut mh = 0u64;
        ocerz_dyld_image_at(gpr(cpu, OCERZ_RDI) as u32, &mut mh, null_mut(), null_mut());
        br_answer(vm, cpu, mh)
    }
}
pub unsafe extern "C" fn br_dyld_image_name(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut name = 0u64;
        ocerz_dyld_image_at(
            gpr(cpu, OCERZ_RDI) as u32,
            null_mut(),
            null_mut(),
            &mut name,
        );
        br_answer(vm, cpu, name)
    }
}
pub unsafe extern "C" fn br_dyld_image_vmaddr_slide(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut slide = 0u64;
        ocerz_dyld_image_at(
            gpr(cpu, OCERZ_RDI) as u32,
            null_mut(),
            &mut slide,
            null_mut(),
        );
        br_answer(vm, cpu, slide)
    }
}
pub unsafe extern "C" fn br_dyld_image_slide(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut slide = 0u64;
        ocerz_dyld_image_slide(gpr(cpu, OCERZ_RDI), &mut slide);
        br_answer(vm, cpu, slide)
    }
}

/// _dyld_get_image_uuid answers the LC_UUID of the image whose header it is
/// given, read from that header as dyld reads it; host dyld has never heard of
/// the images ocerz loads (D3DMetal asks for its own).
pub unsafe extern "C" fn br_dyld_image_uuid(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        const MH_MAGIC_64: u32 = 0xfeedfacf;
        const MH_MAGIC: u32 = 0xfeedface;
        const LC_UUID: u32 = 0x1b;
        let mh = gpr(cpu, OCERZ_RDI);
        let out = gpr(cpu, OCERZ_RSI);
        if mh == 0 || out == 0 || ocerz_addr_readable(mh) == 0 || ocerz_addr_readable(mh + 31) == 0
        {
            return br_answer(vm, cpu, 0);
        }
        let magic = ocerz_ld(mh, 4) as u32;
        let mut lc = mh
            + (if magic == MH_MAGIC_64 {
                32
            } else if magic == MH_MAGIC {
                28
            } else {
                0
            });
        if lc == mh {
            return br_answer(vm, cpu, 0);
        }
        let ncmds = ocerz_ld(mh + 16, 4) as u32;
        let mut room = ocerz_ld(mh + 20, 4) as u32;
        let mut k = 0;
        while k < ncmds && room >= 8 && ocerz_addr_readable(lc + 7) != 0 {
            let cmd = ocerz_ld(lc, 4) as u32;
            let size = ocerz_ld(lc + 4, 4) as u32;
            if size < 8 || size > room {
                break;
            }
            if cmd == LC_UUID && size >= 24 {
                for b in 0..16 {
                    ocerz_st(out + b, 1, ocerz_ld(lc + 8 + b, 1));
                }
                return br_answer(vm, cpu, 1);
            }
            lc += size as u64;
            room -= size;
            k += 1;
        }
        br_answer(vm, cpu, 0)
    }
}

pub unsafe extern "C" fn br_dyld_register_add_image(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        ocerz_dyld_native_add_image_func(vm, gpr(cpu, OCERZ_RDI), br_stack_below(cpu));
        br_answer(vm, cpu, 0)
    }
}
pub unsafe extern "C" fn br_objc_add_load_image_func(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        ocerz_dyld_native_objc_load_func(vm, gpr(cpu, OCERZ_RDI), br_stack_below(cpu));
        br_answer(vm, cpu, 0)
    }
}
pub unsafe extern "C" fn br_dyld_register_remove_image(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        ocerz_dyld_native_remove_image_func(gpr(cpu, OCERZ_RDI));
        br_answer(vm, cpu, 0)
    }
}
pub unsafe extern "C" fn br_dyld_image_header_containing(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        let mut mh = 0u64;
        ocerz_dyld_image_containing(gpr(cpu, OCERZ_RDI), &mut mh, null_mut());
        br_answer(vm, cpu, mh)
    }
}
pub unsafe extern "C" fn br_dyld_image_path_containing(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        let mut name = 0u64;
        ocerz_dyld_image_containing(gpr(cpu, OCERZ_RDI), null_mut(), &mut name);
        br_answer(vm, cpu, name)
    }
}
pub unsafe extern "C" fn br_dyld_image_containing(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_answer(
            vm,
            cpu,
            ocerz_dyld_image_containing(gpr(cpu, OCERZ_RDI), null_mut(), null_mut()) as u64,
        )
    }
}
pub unsafe extern "C" fn br_dyld_prog_image_header(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { br_answer(vm, cpu, ocerz_main_mh) }
}

unsafe fn br_dyld_build_field(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    mh: u64,
    field: usize,
) -> c_int {
    unsafe {
        let mut v = [0u32; 3];
        ocerz_dyld_build_version(mh, &mut v[0], &mut v[1], &mut v[2]);
        br_answer(vm, cpu, v[field] as u64)
    }
}
pub unsafe extern "C" fn br_dyld_program_sdk_version(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe { br_dyld_build_field(vm, cpu, ocerz_main_mh, 2) }
}
pub unsafe extern "C" fn br_dyld_program_min_os_version(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe { br_dyld_build_field(vm, cpu, ocerz_main_mh, 1) }
}
pub unsafe extern "C" fn br_dyld_sdk_version(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { br_dyld_build_field(vm, cpu, gpr(cpu, OCERZ_RDI), 2) }
}
pub unsafe extern "C" fn br_dyld_min_os_version(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { br_dyld_build_field(vm, cpu, gpr(cpu, OCERZ_RDI), 1) }
}
pub unsafe extern "C" fn br_dyld_active_platform(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut platform = 0u32;
        let mut minos = 0u32;
        let mut sdk = 0u32;
        ocerz_dyld_build_version(ocerz_main_mh, &mut platform, &mut minos, &mut sdk);
        br_answer(vm, cpu, if platform != 0 { platform as u64 } else { 1 })
    }
}
pub unsafe extern "C" fn br_dyld_program_sdk_at_least(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        br_answer(
            vm,
            cpu,
            ocerz_dyld_version_at_least(ocerz_main_mh, gpr(cpu, OCERZ_RDI), 1) as u64,
        )
    }
}
pub unsafe extern "C" fn br_dyld_program_minos_at_least(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        br_answer(
            vm,
            cpu,
            ocerz_dyld_version_at_least(ocerz_main_mh, gpr(cpu, OCERZ_RDI), 0) as u64,
        )
    }
}
pub unsafe extern "C" fn br_dyld_sdk_at_least(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_answer(
            vm,
            cpu,
            ocerz_dyld_version_at_least(gpr(cpu, OCERZ_RDI), gpr(cpu, OCERZ_RSI), 1) as u64,
        )
    }
}
pub unsafe extern "C" fn br_dyld_minos_at_least(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        br_answer(
            vm,
            cpu,
            ocerz_dyld_version_at_least(gpr(cpu, OCERZ_RDI), gpr(cpu, OCERZ_RSI), 0) as u64,
        )
    }
}
pub unsafe extern "C" fn br_dyld_is_memory_immutable(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        br_answer(
            vm,
            cpu,
            ocerz_dyld_is_memory_immutable(gpr(cpu, OCERZ_RDI), gpr(cpu, OCERZ_RSI)) as u64,
        )
    }
}
pub unsafe extern "C" fn br_dyld_cache_some_image_overridden(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe { br_answer(vm, cpu, 0) }
}
pub unsafe extern "C" fn br_dyld_cache_contains_path(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        let mut path = [0i8; 4096];
        let p = br_guest_path(gpr(cpu, OCERZ_RDI), path.as_mut_ptr(), path.len());
        br_answer(vm, cpu, ocerz_dyld_native_names_library(p) as u64)
    }
}

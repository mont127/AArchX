//! Guest signal state, signal-frame construction, and signal syscall shims.

use super::util::*;
use super::*;

use core::ffi::{c_char, c_int};
use core::ptr;
use core::sync::atomic::{AtomicI32, AtomicU64, Ordering};

const GUEST_SELFSIG_HOST: c_int = 0;
const GUEST_SELFSIG_PENDING: c_int = 1;
const GUEST_SELFSIG_DELIVERED: c_int = 2;
const OCERZ_UC_RESET_ALT_STACK: u32 = 0x8000_0000;
const OCERZ_MCTX_SIZE: u32 = 1032;
const OCERZ_MCTX_FULL_SIZE: u32 = 1064;
const OCERZ_MCTX_FP_OFF: u64 = 184;
const OCERZ_MCTX_FULL_FP_OFF: u64 = 216;
const OCERZ_MCTX_FULL_DS_OFF: u64 = 184;
const OCERZ_FP_MXCSR_OFF: u64 = 32;
const OCERZ_FP_XMM_OFF: u64 = 168;
const OCERZ_FP_YMMH_OFF: u64 = 588;
const OCERZ_REDZONE: u64 = 128;
const OCERZ_SEL_USER_CS64: u64 = 0x2b;
const OCERZ_SEL_USER_DS64: u64 = 0x23;
const OCERZ_UCTX_SIZE: u64 = 768;
const OCERZ_SIGINFO_SIZE: u64 = 104;
const OCERZ_UCTX_SEGBASE_COOKIE: u64 = 0x4f43_4552_5a53_4547;
const OCERZ_UC_SET_ALT_STACK: u32 = 0x4000_0000;
const GUEST_WAITERS: usize = 64;
const CLOCK_UPTIME_RAW: libc::clockid_t = 8;

pub(super) type SysRingEntry = crate::ffi::OcerzCPU__bindgen_ty_2;

#[repr(C)]
#[derive(Clone, Copy)]
struct GuestWaiter {
    cpu: *mut OcerzCPU,
    accept: u64,
}

static mut GUEST_WAITERS_LIST: [GuestWaiter; GUEST_WAITERS] = [GuestWaiter {
    cpu: ptr::null_mut(),
    accept: 0,
}; GUEST_WAITERS];
static mut GUEST_WAITERS_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
pub(super) static mut G_BIGRING: *mut SysRingEntry = ptr::null_mut();
pub(super) static mut G_BIGRING_N: u32 = 0;
pub(super) static mut G_BIGRING_MASK: u32 = 0;

#[unsafe(no_mangle)]
#[thread_local]
pub static mut g_ocerz_deliver_src: c_int = 0;

static G_NATIVE_SIGTRAMP: AtomicU64 = AtomicU64::new(0);
static mut G_NATIVE_SIGTRAMP_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

unsafe extern "C" {
    fn ocerz_peek_dump(tag: *const c_char);
    fn ocerz_peek_pending_async_sig() -> u32;
    fn mach_thread_self() -> mach_port_t;
    fn mach_port_deallocate(task: mach_port_t, name: mach_port_t) -> c_int;
    fn pthread_mach_thread_np(thread: libc::pthread_t) -> mach_port_t;
    fn clock_gettime_nsec_np(clock_id: libc::clockid_t) -> u64;
    static mach_task_self_: mach_port_t;
}

#[inline(always)]
unsafe fn guest_sigact_ptr(sig: usize) -> *mut GuestSigact {
    unsafe {
        ptr::addr_of_mut!(super::guest_sigact)
            .cast::<GuestSigact>()
            .add(sig)
    }
}

#[inline(always)]
unsafe fn sig_pending_atomic(cpu: *mut OcerzCPU) -> &'static AtomicU64 {
    unsafe { AtomicU64::from_ptr(ptr::addr_of_mut!((*cpu).sig_pending)) }
}

unsafe fn guest_sigact_install(sig: c_int, handler: u64, tramp: u64, mask: u32, flags: u32) {
    unsafe {
        let sa = &mut *guest_sigact_ptr(sig as usize);
        sa.handler = handler;
        sa.tramp = tramp;
        sa.mask = mask as u64;
        sa.flags = flags;
        crate::ffi::ocerz_vm_mirror_host_signal(
            sig,
            if handler == 0 {
                0
            } else if handler == 1 {
                1
            } else {
                2
            },
        );
    }
}

#[inline(always)]
pub(super) unsafe fn guest_sig_catchable(sig: c_int) -> bool {
    if sig <= 0 || sig as usize >= OCERZ_NSIG {
        return false;
    }
    unsafe {
        let sa = &*guest_sigact_ptr(sig as usize);
        sa.handler > 1 && sa.tramp != 0
    }
}

pub(super) unsafe fn sys_sigaction(
    _vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    a: *mut [u64; 8],
) -> c_int {
    unsafe {
        let a = &*a;
        let sig = a[0] as c_int;
        let act = a[1];
        let oact = a[2];
        if oact != 0 && sig >= 0 && (sig as usize) < OCERZ_NSIG {
            super::guest_sigact_store_user(oact, guest_sigact_ptr(sig as usize));
        }
        if act != 0 && sig >= 0 && (sig as usize) < OCERZ_NSIG {
            guest_sigact_install(
                sig,
                ocerz_ld(act, 8),
                ocerz_ld(act.wrapping_add(8), 8),
                ocerz_ld(act.wrapping_add(16), 4) as u32,
                ocerz_ld(act.wrapping_add(20), 4) as u32,
            );
        }
        ret_ok(cpu, 0);
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

unsafe fn guest_selfkill_routed() -> c_int {
    static ROUTE: AtomicI32 = AtomicI32::new(-1);
    let mut route = ROUTE.load(Ordering::Relaxed);
    if route < 0 {
        route = c_int::from(libc::getenv(c"OCERZ_NO_GUEST_SELFKILL".as_ptr()).is_null());
        ROUTE.store(route, Ordering::Relaxed);
    }
    route
}

unsafe fn guest_abort_note(cpu: *mut OcerzCPU) {
    unsafe {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: GUEST-ABORT[%d] cpu#%u rip=%#llx bt:".as_ptr(),
            libc::getpid(),
            (*cpu).cpu_number,
            (*cpu).rip as libc::c_ulonglong,
        );
        let mut fp = (*cpu).gpr[crate::ffi::OCERZ_RBP as usize];
        for _ in 0..10 {
            if fp <= 0x1000 || crate::ffi::ocerz_addr_readable(fp.wrapping_add(8)) == 0 {
                break;
            }
            libc::fprintf(
                crate::log::stderr(),
                c" %#llx".as_ptr(),
                ocerz_ld(fp.wrapping_add(8), 8) as libc::c_ulonglong,
            );
            let nf = ocerz_ld(fp, 8);
            if nf <= fp {
                break;
            }
            fp = nf;
        }
        libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
        libc::fflush(crate::log::stderr());
    }
}

unsafe fn guest_self_signal(cpu: *mut OcerzCPU, signo: c_int, defer: c_int) -> c_int {
    unsafe {
        let bit = 1u64 << (signo - 1);
        if (*cpu).sig_mask & bit != 0 || (defer != 0 && guest_sig_catchable(signo)) {
            sig_pending_atomic(cpu).fetch_or(bit, Ordering::SeqCst);
            return GUEST_SELFSIG_PENDING;
        }
        if defer == 0 && ocerz_signal_deliver(cpu, signo, 0, 0, 0) != 0 {
            return GUEST_SELFSIG_DELIVERED;
        }
        let fatal = matches!(signo, 4 | 5 | 6 | 8 | 10 | 11 | 3 | 7);
        if fatal {
            ocerz_peek_dump(c"guest-abort".as_ptr());
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: guest self-signal %llu rip=%#llx bt:".as_ptr(),
                signo as libc::c_ulonglong,
                (*cpu).rip as libc::c_ulonglong,
            );
            let mut fp = (*cpu).gpr[crate::ffi::OCERZ_RBP as usize];
            for _ in 0..16 {
                if fp <= 0x1000 || crate::ffi::ocerz_addr_readable(fp.wrapping_add(8)) == 0 {
                    break;
                }
                libc::fprintf(
                    crate::log::stderr(),
                    c" %#llx".as_ptr(),
                    ocerz_ld(fp.wrapping_add(8), 8) as libc::c_ulonglong,
                );
                let nf = ocerz_ld(fp, 8);
                if nf <= fp {
                    break;
                }
                fp = nf;
            }
            libc::fprintf(
                crate::log::stderr(),
                c"\nocerz: guest self-signal %llu, no handler; exiting %d\n".as_ptr(),
                signo as libc::c_ulonglong,
                128 + signo,
            );
            libc::fflush(crate::log::stderr());
            libc::_exit(128 + signo);
        }
        GUEST_SELFSIG_HOST
    }
}

pub(super) unsafe fn sys_pthread_kill(
    _vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    a: *mut [u64; 8],
) -> c_int {
    unsafe {
        let a = &*a;
        let signo = a[1];
        if !libc::getenv(c"OCERZ_PTKILL".as_ptr()).is_null() {
            let selfport = raw::ocerz_host_mach_trap(27, a);
            let handler = if signo < OCERZ_NSIG as u64 {
                (*guest_sigact_ptr(signo as usize)).handler
            } else {
                0
            };
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: PTKILL[%d] target=%#llx signo=%llu self=%#llx cross=%d handler=%#llx\n"
                    .as_ptr(),
                libc::getpid(),
                a[0] as libc::c_ulonglong,
                signo as libc::c_ulonglong,
                selfport as libc::c_ulonglong,
                c_int::from(a[0] != 0 && a[0] != selfport as u64),
                handler as libc::c_ulonglong,
            );
        }
        if signo == 6 {
            guest_abort_note(cpu);
        }
        let route = guest_selfkill_routed();
        let selfport = mach_thread_self();
        let is_self = a[0] == 0 || a[0] == selfport as u64;
        if selfport != 0 {
            mach_port_deallocate(mach_task_self_, selfport);
        }
        if route != 0 && is_self && signo > 0 && signo < OCERZ_NSIG as u64 {
            ret_ok(cpu, 0);
            let how = guest_self_signal(cpu, signo as c_int, 0);
            if how == GUEST_SELFSIG_PENDING || how == GUEST_SELFSIG_DELIVERED {
                return crate::ffi::OCERZ_STEP_OK as c_int;
            }
        }
        let mut err = 0;
        let r = raw::ocerz_host_syscall(328, a, ptr::null_mut(), &mut err);
        if err != 0 {
            ret_err(cpu, r);
        } else {
            ret_ok(cpu, r);
        }
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_thread_port_kill(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    port: u64,
    sig: c_int,
    err: *mut c_int,
) -> c_int {
    unsafe {
        let mut a = [port, sig as u32 as u64, 0, 0, 0, 0, 0, 0];
        let saved_rax = (*cpu).gpr[crate::ffi::OCERZ_RAX as usize];
        let saved_flags = (*cpu).rflags;
        sys_pthread_kill(vm, cpu, &mut a);
        *err = if (*cpu).rflags & 1 != 0 {
            (*cpu).gpr[crate::ffi::OCERZ_RAX as usize] as c_int
        } else {
            0
        };
        (*cpu).gpr[crate::ffi::OCERZ_RAX as usize] = saved_rax;
        (*cpu).rflags = saved_flags;
        if *err != 0 { -1 } else { 0 }
    }
}

unsafe fn sigmask_trace_addr(addr: u64) {
    unsafe {
        let mut base = 0;
        let name = crate::ffi::ocerz_dyld_name_for_addr(addr, &mut base);
        if !name.is_null() {
            let slash = libc::strrchr(name, b'/' as c_int);
            libc::fprintf(
                crate::log::stderr(),
                c" %s+%#llx".as_ptr(),
                if slash.is_null() { name } else { slash.add(1) },
                addr.wrapping_sub(base) as libc::c_ulonglong,
            );
        } else {
            libc::fprintf(
                crate::log::stderr(),
                c" %#llx".as_ptr(),
                addr as libc::c_ulonglong,
            );
        }
    }
}

unsafe fn sigmask_trace(cpu: *mut OcerzCPU, how: *const c_char, old: u64, now: u64) {
    static ON: AtomicI32 = AtomicI32::new(-1);
    unsafe {
        let mut on = ON.load(Ordering::Relaxed);
        if on < 0 {
            on = c_int::from(!libc::getenv(c"OCERZ_SIGMASKLOG".as_ptr()).is_null());
            ON.store(on, Ordering::Relaxed);
        }
        if on == 0 || old == now || (now as u32).count_ones() < 16 {
            return;
        }
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: SIGMASK[%d] cpu#%u tid=%#llx %s %#llx -> %#llx at".as_ptr(),
            libc::getpid(),
            (*cpu).cpu_number,
            (*cpu).host_tid as libc::c_ulonglong,
            how,
            old as libc::c_ulonglong,
            now as libc::c_ulonglong,
        );
        sigmask_trace_addr((*cpu).rip);
        let sp = (*cpu).gpr[crate::ffi::OCERZ_RSP as usize];
        let mut fp = (*cpu).gpr[crate::ffi::OCERZ_RBP as usize];
        if crate::ffi::ocerz_addr_readable(sp) != 0
            && crate::ffi::ocerz_addr_readable(sp.wrapping_add(7)) != 0
        {
            libc::fprintf(crate::log::stderr(), c" ret:".as_ptr());
            sigmask_trace_addr(ocerz_ld(sp, 8));
        }
        libc::fprintf(crate::log::stderr(), c" chain:".as_ptr());
        for _ in 0..12 {
            if fp <= 0x1000
                || fp & 7 != 0
                || crate::ffi::ocerz_addr_readable(fp.wrapping_add(15)) == 0
            {
                break;
            }
            sigmask_trace_addr(ocerz_ld(fp.wrapping_add(8), 8));
            let nf = ocerz_ld(fp, 8);
            if nf <= fp {
                break;
            }
            fp = nf;
        }
        libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
        super::threads::ocerz_pe_stack_dump(cpu, c"SIGMASK-PE".as_ptr());
    }
}

unsafe fn guest_sigmask_apply(cpu: *mut OcerzCPU, how: c_int, set: u64, oset: u64) {
    unsafe {
        let before = (*cpu).sig_mask;
        if oset != 0 {
            ocerz_st(oset, 4, (*cpu).sig_mask as u32 as u64);
        }
        if set != 0 {
            let value = ocerz_ld(set, 4) as u32 as u64;
            if how == 1 {
                (*cpu).sig_mask |= value;
            } else if how == 2 {
                (*cpu).sig_mask &= !value;
            } else {
                (*cpu).sig_mask = value;
            }
        }
        let label = if how == 1 {
            c"BLOCK".as_ptr()
        } else if how == 2 {
            c"UNBLOCK".as_ptr()
        } else {
            c"SETMASK".as_ptr()
        };
        sigmask_trace(cpu, label, before, (*cpu).sig_mask);
    }
}

pub(super) unsafe fn sys_sigprocmask(
    _vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    a: *mut [u64; 8],
) -> c_int {
    unsafe {
        let a = &*a;
        guest_sigmask_apply(cpu, a[0] as c_int, a[1], a[2]);
        ret_ok(cpu, 0);
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

unsafe fn guest_altstack_flags(cpu: *const OcerzCPU) -> u32 {
    unsafe {
        c_uint::from((*cpu).sig_on_stack != 0) * 1
            | c_uint::from((*cpu).sig_altstack_sp == 0 && (*cpu).sig_altstack_size == 0) * 4
    }
}

unsafe fn guest_sigaltstack_apply(cpu: *mut OcerzCPU, ss: u64, oss: u64) -> c_int {
    unsafe {
        if oss != 0 {
            ocerz_st(oss, 8, (*cpu).sig_altstack_sp);
            ocerz_st(oss.wrapping_add(8), 8, (*cpu).sig_altstack_size);
            ocerz_st(oss.wrapping_add(16), 4, guest_altstack_flags(cpu) as u64);
        }
        if ss != 0 {
            let flags = ocerz_ld(ss.wrapping_add(16), 4) as u32;
            if flags & 4 != 0 {
                (*cpu).sig_altstack_sp = 0;
                (*cpu).sig_altstack_size = 0;
            } else {
                let size = ocerz_ld(ss.wrapping_add(8), 8);
                if size < DARWIN_MINSIGSTKSZ as u64 {
                    return libc::ENOMEM;
                }
                (*cpu).sig_altstack_sp = ocerz_ld(ss, 8);
                (*cpu).sig_altstack_size = size;
                if (*cpu).sig_altstack_sp != 0 && size != 0 {
                    crate::ffi::ocerz_protect((*cpu).sig_altstack_sp, size, 3);
                }
            }
        }
        0
    }
}

pub(super) unsafe fn sys_sigaltstack(
    _vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    a: *mut [u64; 8],
) -> c_int {
    unsafe {
        let a = &*a;
        let err = guest_sigaltstack_apply(cpu, a[0], a[1]);
        if err != 0 {
            ret_err(cpu, err as u64);
        } else {
            ret_ok(cpu, 0);
        }
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_signal_deliver(
    cpu: *mut OcerzCPU,
    sig: c_int,
    fault_addr: u64,
    si_code: c_int,
    err: u32,
) -> c_int {
    unsafe {
        if !guest_sig_catchable(sig) {
            return 0;
        }
        let sa = &*guest_sigact_ptr(sig as usize);
        let src = ptr::read_volatile(ptr::addr_of!(g_ocerz_deliver_src));
        ptr::write_volatile(ptr::addr_of_mut!(g_ocerz_deliver_src), 0);
        if env_set!("OCERZ_WSIG")
            && (*cpu).sig_altstack_sp == 0
            && !ocerz_gs_is_teb_band((*cpu).gs_base)
        {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: WSIG sig=%d src=%d faddr=%#llx code=%d gs=%#llx rsp=%#llx rip=%#llx handler=%#llx wteb=%#llx\n".as_ptr(),
                sig,
                src,
                fault_addr as libc::c_ulonglong,
                si_code,
                (*cpu).gs_base as libc::c_ulonglong,
                (*cpu).gpr[crate::ffi::OCERZ_RSP as usize] as libc::c_ulonglong,
                (*cpu).rip as libc::c_ulonglong,
                sa.handler as libc::c_ulonglong,
                (*cpu).wine_teb_base as libc::c_ulonglong,
            );
        }
        let asp = (*cpu).sig_altstack_sp;
        let asz = (*cpu).sig_altstack_size;
        let sp_on_alt =
            asp != 0 && (*cpu).gpr[crate::ffi::OCERZ_RSP as usize].wrapping_sub(asp) < asz;
        let old_on_stack = (*cpu).sig_on_stack != 0;
        let use_alt = sa.flags & DARWIN_SA_ONSTACK != 0 && asp != 0 && !old_on_stack;
        if env_set!("OCERZ_SIGTRACE") {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz:   altstk sig=%d flags=%#x ONSTACK=%d altsp=%#llx altsz=%#llx on_stack=%d sp_on_alt=%d -> use_alt=%d\n".as_ptr(),
                sig,
                sa.flags,
                c_int::from(sa.flags & DARWIN_SA_ONSTACK != 0),
                asp as libc::c_ulonglong,
                asz as libc::c_ulonglong,
                c_int::from(old_on_stack),
                c_int::from(sp_on_alt),
                c_int::from(use_alt),
            );
        }
        let full = super::ldt::ocerz_ldt_installed() != 0;
        let mcsize = if full {
            OCERZ_MCTX_FULL_SIZE
        } else {
            OCERZ_MCTX_SIZE
        };
        let fpoff = if full {
            OCERZ_MCTX_FULL_FP_OFF
        } else {
            OCERZ_MCTX_FP_OFF
        };
        let top = (if use_alt {
            asp.wrapping_add(asz)
        } else {
            (*cpu).gpr[crate::ffi::OCERZ_RSP as usize]
        })
        .wrapping_sub(OCERZ_REDZONE);
        let mc = top.wrapping_sub(mcsize as u64) & !15;
        let uc = mc.wrapping_sub(OCERZ_UCTX_SIZE) & !15;
        let si = uc.wrapping_sub(OCERZ_SIGINFO_SIZE) & !15;
        let newsp = si.wrapping_sub(8);
        ptr::write_bytes(ocerz_g2h(mc).cast::<u8>(), 0, mcsize as usize);
        ptr::write_bytes(ocerz_g2h(uc).cast::<u8>(), 0, OCERZ_UCTX_SIZE as usize);
        ptr::write_bytes(ocerz_g2h(si).cast::<u8>(), 0, OCERZ_SIGINFO_SIZE as usize);
        let trapno = if sig == libc::SIGSEGV || sig == libc::SIGBUS {
            14
        } else if sig == libc::SIGILL {
            6
        } else if sig == crate::ffi::OCERZ_SIGTRAP as c_int {
            3
        } else {
            0
        };
        ocerz_st(mc, 4, trapno);
        ocerz_st(mc.wrapping_add(4), 4, err as u64);
        ocerz_st(mc.wrapping_add(8), 8, fault_addr);
        ocerz_st(
            mc.wrapping_add(16),
            8,
            (*cpu).gpr[crate::ffi::OCERZ_RAX as usize],
        );
        ocerz_st(
            mc.wrapping_add(24),
            8,
            (*cpu).gpr[crate::ffi::OCERZ_RBX as usize],
        );
        ocerz_st(
            mc.wrapping_add(32),
            8,
            (*cpu).gpr[crate::ffi::OCERZ_RCX as usize],
        );
        ocerz_st(
            mc.wrapping_add(40),
            8,
            (*cpu).gpr[crate::ffi::OCERZ_RDX as usize],
        );
        ocerz_st(
            mc.wrapping_add(48),
            8,
            (*cpu).gpr[crate::ffi::OCERZ_RDI as usize],
        );
        ocerz_st(
            mc.wrapping_add(56),
            8,
            (*cpu).gpr[crate::ffi::OCERZ_RSI as usize],
        );
        ocerz_st(
            mc.wrapping_add(64),
            8,
            (*cpu).gpr[crate::ffi::OCERZ_RBP as usize],
        );
        ocerz_st(
            mc.wrapping_add(72),
            8,
            (*cpu).gpr[crate::ffi::OCERZ_RSP as usize],
        );
        ocerz_st(
            mc.wrapping_add(80),
            8,
            (*cpu).gpr[crate::ffi::OCERZ_R8 as usize],
        );
        ocerz_st(
            mc.wrapping_add(88),
            8,
            (*cpu).gpr[crate::ffi::OCERZ_R9 as usize],
        );
        ocerz_st(
            mc.wrapping_add(96),
            8,
            (*cpu).gpr[crate::ffi::OCERZ_R10 as usize],
        );
        ocerz_st(
            mc.wrapping_add(104),
            8,
            (*cpu).gpr[crate::ffi::OCERZ_R11 as usize],
        );
        ocerz_st(
            mc.wrapping_add(112),
            8,
            (*cpu).gpr[crate::ffi::OCERZ_R12 as usize],
        );
        ocerz_st(
            mc.wrapping_add(120),
            8,
            (*cpu).gpr[crate::ffi::OCERZ_R13 as usize],
        );
        ocerz_st(
            mc.wrapping_add(128),
            8,
            (*cpu).gpr[crate::ffi::OCERZ_R14 as usize],
        );
        ocerz_st(
            mc.wrapping_add(136),
            8,
            (*cpu).gpr[crate::ffi::OCERZ_R15 as usize],
        );
        ocerz_st(mc.wrapping_add(144), 8, (*cpu).rip);
        ocerz_st(mc.wrapping_add(152), 8, (*cpu).rflags);
        ocerz_st(mc.wrapping_add(160), 8, (*cpu).cs_sel as u64);
        if full {
            ocerz_st(
                mc.wrapping_add(OCERZ_MCTX_FULL_DS_OFF),
                8,
                OCERZ_SEL_USER_DS64,
            );
            ocerz_st(
                mc.wrapping_add(OCERZ_MCTX_FULL_DS_OFF + 8),
                8,
                OCERZ_SEL_USER_DS64,
            );
            ocerz_st(
                mc.wrapping_add(OCERZ_MCTX_FULL_DS_OFF + 16),
                8,
                OCERZ_SEL_USER_DS64,
            );
            ocerz_st(
                mc.wrapping_add(OCERZ_MCTX_FULL_DS_OFF + 24),
                8,
                (*cpu).gs_base,
            );
        }
        ocerz_st(
            mc.wrapping_add(fpoff + OCERZ_FP_MXCSR_OFF),
            4,
            (*cpu).mxcsr as u64,
        );
        for i in 0..16usize {
            ocerz_st128(
                mc.wrapping_add(fpoff + OCERZ_FP_XMM_OFF + i as u64 * 16),
                (*cpu).xmm[i],
            );
        }
        for i in 0..16usize {
            ocerz_st128(
                mc.wrapping_add(fpoff + OCERZ_FP_YMMH_OFF + i as u64 * 16),
                (*cpu).ymmh[i],
            );
        }
        let old_mask = (*cpu).sig_mask;
        ocerz_st(uc, 4, u64::from(old_on_stack));
        ocerz_st(uc.wrapping_add(4), 4, old_mask as u32 as u64);
        ocerz_st(uc.wrapping_add(8), 8, asp);
        ocerz_st(uc.wrapping_add(16), 8, asz);
        ocerz_st(uc.wrapping_add(24), 4, u64::from((*cpu).sig_on_stack != 0));
        ocerz_st(uc.wrapping_add(40), 8, mcsize as u64);
        ocerz_st(uc.wrapping_add(48), 8, mc);
        ocerz_st(uc.wrapping_add(56), 8, (*cpu).gs_base);
        ocerz_st(uc.wrapping_add(64), 8, (*cpu).fs_base);
        ocerz_st(uc.wrapping_add(72), 8, OCERZ_UCTX_SEGBASE_COOKIE);
        ocerz_st(si, 4, sig as u32 as u64);
        ocerz_st(si.wrapping_add(8), 4, si_code as u32 as u64);
        ocerz_st(si.wrapping_add(24), 8, fault_addr);
        (*cpu).gpr[crate::ffi::OCERZ_RDI as usize] = sa.handler;
        (*cpu).gpr[crate::ffi::OCERZ_RDX as usize] = sig as u32 as u64;
        (*cpu).gpr[crate::ffi::OCERZ_RCX as usize] = si;
        (*cpu).gpr[crate::ffi::OCERZ_R8 as usize] = uc;
        (*cpu).gpr[crate::ffi::OCERZ_R9 as usize] = uc;
        (*cpu).gpr[crate::ffi::OCERZ_RSP as usize] = newsp;
        (*cpu).rip = sa.tramp;
        (*cpu).mode32 = 0;
        (*cpu).cs_sel = OCERZ_SEL_USER_CS64 as u16;
        (*cpu).seg_sel[crate::ffi::OCERZ_SREG_CS as usize] = OCERZ_SEL_USER_CS64 as u16;
        if use_alt {
            (*cpu).sig_on_stack = 1;
        }
        (*cpu).sig_mask = old_mask | sa.mask;
        if sa.flags & DARWIN_SA_NODEFER == 0 && sig > 0 {
            (*cpu).sig_mask |= 1u64 << (sig - 1);
        }
        sigmask_trace(
            cpu,
            if sig == 30 {
                c"DELIVER-USR1".as_ptr()
            } else {
                c"DELIVER".as_ptr()
            },
            old_mask,
            (*cpu).sig_mask,
        );
        1
    }
}

pub(super) unsafe fn deliver_async_signals(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    taken: u32,
) -> c_int {
    unsafe {
        if taken >> 1 != 0 {
            sig_pending_atomic(cpu).fetch_or((taken >> 1) as u64, Ordering::SeqCst);
        }
        let mut n = 0;
        loop {
            let ready = sig_pending_atomic(cpu).load(Ordering::SeqCst) & !(*cpu).sig_mask;
            if ready == 0 {
                break;
            }
            let s = ready.trailing_zeros() as c_int + 1;
            sig_pending_atomic(cpu).fetch_and(!(1u64 << (s - 1)), Ordering::SeqCst);
            ptr::write_volatile(ptr::addr_of_mut!(g_ocerz_deliver_src), 1);
            if env_set!("OCERZ_ASIGLOG") {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: ASYNCSIG[%d] cpu#%u sig=%d rip=%#llx ic=%#llx\n".as_ptr(),
                    libc::getpid(),
                    (*cpu).cpu_number,
                    s,
                    (*cpu).rip as libc::c_ulonglong,
                    if vm.is_null() {
                        0
                    } else {
                        (*vm).insn_count as libc::c_ulonglong
                    },
                );
            }
            let at = (*cpu).sysring_n;
            let e = ptr::addr_of_mut!((*cpu).sysring)
                .cast::<SysRingEntry>()
                .wrapping_add((at % 24) as usize);
            (*cpu).sysring_n = at.wrapping_add(1);
            (*e).t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            (*e).num = -s;
            (*e).a0 = (*cpu).rip;
            (*e).a1 = (*cpu).sig_mask;
            (*e).a2 = (*cpu).in_sighandler as u64;
            (*e).ret = 0;
            (*e).peek = 0;
            (*e).peek2 = 0;
            (*e).peek3 = 0;
            (*e).peek4 = 0;
            super::entry::ocerz_bigring_push(cpu, e);
            if ocerz_signal_deliver(cpu, s, 0, 0, 0) != 0 {
                n += 1;
                let delivered = ptr::addr_of_mut!((*cpu).sig_delivered)
                    .cast::<u32>()
                    .add(s as usize);
                *delivered = (*delivered).wrapping_add(1);
                (*cpu).in_sighandler += 1;
                if env_set!("OCERZ_SIGCTXLOG") {
                    libc::fprintf(
                        crate::log::stderr(),
                        c"ocerz: SIGENTER[%d] cpu#%u sig=%d depth=%u handler-rip=%#llx rsp=%#llx\n"
                            .as_ptr(),
                        libc::getpid(),
                        (*cpu).cpu_number,
                        s,
                        (*cpu).in_sighandler,
                        (*cpu).rip as libc::c_ulonglong,
                        (*cpu).gpr[crate::ffi::OCERZ_RSP as usize] as libc::c_ulonglong,
                    );
                }
            }
        }
        n
    }
}

unsafe fn guest_signal_ready(cpu: *mut OcerzCPU) -> bool {
    unsafe {
        (ocerz_peek_pending_async_sig() & super::hostwq::async_accept((*cpu).sig_mask)) != 0
            || sig_pending_atomic(cpu).load(Ordering::SeqCst) & !(*cpu).sig_mask != 0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_signal_before_syscall(cpu: *mut OcerzCPU, insn_rip: u64) -> c_int {
    unsafe {
        if !guest_signal_ready(cpu) {
            return 0;
        }
        let resume = (*cpu).rip;
        (*cpu).rip = insn_rip;
        if deliver_async_signals(
            (*cpu).vm,
            cpu,
            crate::ffi::ocerz_take_pending_async_sig_mask(super::hostwq::async_accept(
                (*cpu).sig_mask,
            )),
        ) != 0
        {
            return 1;
        }
        (*cpu).rip = resume;
        0
    }
}

pub(super) unsafe fn sys_sigreturn(
    _vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    a: *mut [u64; 8],
) -> c_int {
    unsafe {
        let a = &*a;
        let uc = a[0];
        if a[1] as u32 == OCERZ_UC_SET_ALT_STACK || a[1] as u32 == OCERZ_UC_RESET_ALT_STACK {
            (*cpu).sig_on_stack = c_int::from(a[1] as u32 == OCERZ_UC_SET_ALT_STACK);
            ret_ok(cpu, 0);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        if uc == 0 {
            ret_err(cpu, libc::EINVAL as u64);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        let mc = ocerz_ld(uc.wrapping_add(48), 8);
        if mc == 0 {
            ret_err(cpu, libc::EINVAL as u64);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        if (*cpu).in_sighandler != 0 {
            (*cpu).in_sighandler -= 1;
        }
        if env_set!("OCERZ_SIGCTXLOG") {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: SIGRET[%d] cpu#%u depth=%u -> rip=%#llx rsp=%#llx mask=%#x\n".as_ptr(),
                libc::getpid(),
                (*cpu).cpu_number,
                (*cpu).in_sighandler,
                ocerz_ld(mc.wrapping_add(144), 8) as libc::c_ulonglong,
                ocerz_ld(mc.wrapping_add(72), 8) as libc::c_ulonglong,
                ocerz_ld(uc.wrapping_add(4), 4) as u32,
            );
        }
        let mcsize = ocerz_ld(uc.wrapping_add(40), 8);
        let full = mcsize >= OCERZ_MCTX_FULL_SIZE as u64;
        let fpoff = if full {
            OCERZ_MCTX_FULL_FP_OFF
        } else {
            OCERZ_MCTX_FP_OFF
        };
        (*cpu).gpr[crate::ffi::OCERZ_RAX as usize] = ocerz_ld(mc.wrapping_add(16), 8);
        (*cpu).gpr[crate::ffi::OCERZ_RBX as usize] = ocerz_ld(mc.wrapping_add(24), 8);
        (*cpu).gpr[crate::ffi::OCERZ_RCX as usize] = ocerz_ld(mc.wrapping_add(32), 8);
        (*cpu).gpr[crate::ffi::OCERZ_RDX as usize] = ocerz_ld(mc.wrapping_add(40), 8);
        (*cpu).gpr[crate::ffi::OCERZ_RDI as usize] = ocerz_ld(mc.wrapping_add(48), 8);
        (*cpu).gpr[crate::ffi::OCERZ_RSI as usize] = ocerz_ld(mc.wrapping_add(56), 8);
        (*cpu).gpr[crate::ffi::OCERZ_RBP as usize] = ocerz_ld(mc.wrapping_add(64), 8);
        (*cpu).gpr[crate::ffi::OCERZ_RSP as usize] = ocerz_ld(mc.wrapping_add(72), 8);
        (*cpu).gpr[crate::ffi::OCERZ_R8 as usize] = ocerz_ld(mc.wrapping_add(80), 8);
        (*cpu).gpr[crate::ffi::OCERZ_R9 as usize] = ocerz_ld(mc.wrapping_add(88), 8);
        (*cpu).gpr[crate::ffi::OCERZ_R10 as usize] = ocerz_ld(mc.wrapping_add(96), 8);
        (*cpu).gpr[crate::ffi::OCERZ_R11 as usize] = ocerz_ld(mc.wrapping_add(104), 8);
        (*cpu).gpr[crate::ffi::OCERZ_R12 as usize] = ocerz_ld(mc.wrapping_add(112), 8);
        (*cpu).gpr[crate::ffi::OCERZ_R13 as usize] = ocerz_ld(mc.wrapping_add(120), 8);
        (*cpu).gpr[crate::ffi::OCERZ_R14 as usize] = ocerz_ld(mc.wrapping_add(128), 8);
        (*cpu).gpr[crate::ffi::OCERZ_R15 as usize] = ocerz_ld(mc.wrapping_add(136), 8);
        (*cpu).rip = ocerz_ld(mc.wrapping_add(144), 8);
        (*cpu).rflags = ocerz_ld(mc.wrapping_add(152), 8) | 2;
        for i in 0..16usize {
            (*cpu).xmm[i] = ocerz_ld128(mc.wrapping_add(fpoff + OCERZ_FP_XMM_OFF + i as u64 * 16));
        }
        if mcsize >= OCERZ_MCTX_SIZE as u64 {
            for i in 0..16usize {
                (*cpu).ymmh[i] =
                    ocerz_ld128(mc.wrapping_add(fpoff + OCERZ_FP_YMMH_OFF + i as u64 * 16));
            }
            (*cpu).ymmh_all_zero = 0;
        }
        (*cpu).mxcsr = ocerz_ld(mc.wrapping_add(fpoff + OCERZ_FP_MXCSR_OFF), 4) as u32;
        crate::ffi::ocerz_apply_mxcsr_round((*cpu).mxcsr);
        let before = (*cpu).sig_mask;
        (*cpu).sig_mask = ocerz_ld(uc.wrapping_add(4), 4) as u32 as u64;
        sigmask_trace(cpu, c"SIGRETURN".as_ptr(), before, (*cpu).sig_mask);
        let cs = ocerz_ld(mc.wrapping_add(160), 8) as u32;
        if cs != 0 {
            (*cpu).cs_sel = cs as u16;
            (*cpu).seg_sel[crate::ffi::OCERZ_SREG_CS as usize] = cs as u16;
            (*cpu).mode32 = c_int::from(
                super::ldt::ocerz_ldt_is_long(cs) == 0 && super::ldt::ocerz_ldt_is_big(cs) != 0,
            ) as u8;
            if (*cpu).mode32 != 0 {
                (*cpu).rip = (*cpu).rip as u32 as u64;
            }
        }
        let restore_segbases = ocerz_ld(uc.wrapping_add(72), 8) == OCERZ_UCTX_SEGBASE_COOKIE;
        if restore_segbases {
            (*cpu).fs_base = ocerz_ld(uc.wrapping_add(64), 8);
            ocerz_st(uc.wrapping_add(72), 8, 0);
        }
        if !libc::getenv(c"OCERZ_GSTRACE".as_ptr()).is_null() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: GS sigreturn[%d] cpu#%u gs %#llx rip=%#llx%s\n".as_ptr(),
                libc::getpid(),
                (*cpu).cpu_number,
                (*cpu).gs_base as libc::c_ulonglong,
                (*cpu).rip as libc::c_ulonglong,
                if restore_segbases {
                    c" (restored)".as_ptr()
                } else {
                    c" (unchanged)".as_ptr()
                },
            );
        }
        (*cpu).sig_on_stack = c_int::from(ocerz_ld(uc, 4) != 0);
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

static NATIVE_SIGTRAMP_CODE: [u8; 47] = [
    0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xe4, 0xf0, 0x48, 0x89, 0xf8, 0x4c, 0x89, 0xc3, 0x4d, 0x89,
    0xcc, 0x89, 0xd7, 0x48, 0x89, 0xce, 0x4c, 0x89, 0xc2, 0xff, 0xd0, 0x48, 0x89, 0xdf, 0xbe, 0x1e,
    0x00, 0x00, 0x00, 0x4c, 0x89, 0xe2, 0xb8, 0xb8, 0x00, 0x00, 0x02, 0x0f, 0x05, 0x0f, 0x0b,
];

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_native_sigtramp(_vm: *mut OcerzVM) -> u64 {
    unsafe {
        let mut tramp = G_NATIVE_SIGTRAMP.load(Ordering::Acquire);
        if tramp != 0 {
            return tramp;
        }
        libc::pthread_mutex_lock(ptr::addr_of_mut!(G_NATIVE_SIGTRAMP_LOCK));
        tramp = G_NATIVE_SIGTRAMP.load(Ordering::Acquire);
        if tramp == 0 {
            let page = crate::ffi::ocerz_map_anywhere(
                crate::ffi::OCERZ_GUEST_PAGE_SIZE as u64,
                libc::PROT_READ | libc::PROT_WRITE,
            );
            if page != 0 {
                ptr::write_bytes(
                    ocerz_g2h(page).cast::<u8>(),
                    0xcc,
                    crate::ffi::OCERZ_GUEST_PAGE_SIZE as usize,
                );
                ptr::copy_nonoverlapping(
                    NATIVE_SIGTRAMP_CODE.as_ptr(),
                    ocerz_g2h(page).cast::<u8>(),
                    NATIVE_SIGTRAMP_CODE.len(),
                );
                if crate::ffi::ocerz_protect(
                    page,
                    crate::ffi::OCERZ_GUEST_PAGE_SIZE as u64,
                    libc::PROT_READ | libc::PROT_EXEC,
                ) == crate::ffi::OCERZ_OK
                {
                    tramp = page;
                    G_NATIVE_SIGTRAMP.store(tramp, Ordering::Release);
                } else {
                    crate::ffi::ocerz_unmap(page, crate::ffi::OCERZ_GUEST_PAGE_SIZE as u64);
                }
            }
            if tramp == 0 {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: syscall: no read-only guest page could be set up for the native signal trampoline, so no signal handler can be installed\n".as_ptr(),
                );
            }
        }
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_NATIVE_SIGTRAMP_LOCK));
        tramp
    }
}

unsafe fn native_sig_settable(sig: c_int) -> bool {
    sig > 0 && sig < DARWIN_NSIG && sig != libc::SIGKILL && sig != libc::SIGSTOP
}

unsafe fn native_sigaction(
    vm: *mut OcerzVM,
    sig: c_int,
    nsa: *const GuestSigact,
    oact: u64,
    old_handler: *mut u64,
) -> c_int {
    unsafe {
        let mut tramp = 0;
        if !nsa.is_null() {
            tramp = ocerz_native_sigtramp(vm);
            if tramp == 0 {
                return libc::ENOMEM;
            }
        }
        if oact != 0 {
            super::guest_sigact_store_user(oact, guest_sigact_ptr(sig as usize));
        }
        if !old_handler.is_null() {
            *old_handler = (*guest_sigact_ptr(sig as usize)).handler;
        }
        if !nsa.is_null() {
            let sa = &*nsa;
            guest_sigact_install(sig, sa.handler, tramp, sa.mask as u32, sa.flags);
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_sigaction_user(
    vm: *mut OcerzVM,
    _cpu: *mut OcerzCPU,
    sig: c_int,
    act: u64,
    oact: u64,
) -> c_int {
    unsafe {
        if !native_sig_settable(sig) {
            return libc::EINVAL;
        }
        let mut nsa = GuestSigact::default();
        if act != 0 {
            nsa.handler = ocerz_ld(act, 8);
            nsa.mask = ocerz_ld(act.wrapping_add(8), 4) as u32 as u64;
            nsa.flags = ocerz_ld(act.wrapping_add(12), 4) as u32;
        }
        native_sigaction(
            vm,
            sig,
            if act != 0 { &nsa } else { ptr::null() },
            oact,
            ptr::null_mut(),
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_signal(
    vm: *mut OcerzVM,
    _cpu: *mut OcerzCPU,
    sig: c_int,
    handler: u64,
    old_handler: *mut u64,
) -> c_int {
    unsafe {
        if !native_sig_settable(sig) {
            return libc::EINVAL;
        }
        let nsa = GuestSigact {
            handler,
            tramp: 0,
            mask: 0,
            flags: DARWIN_SA_RESTART,
        };
        native_sigaction(vm, sig, &nsa, 0, old_handler)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_sigprocmask(
    _vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    how: c_int,
    set: u64,
    oset: u64,
) -> c_int {
    unsafe {
        guest_sigmask_apply(cpu, how, set, oset);
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_sigaltstack(
    _vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    ss: u64,
    oss: u64,
) -> c_int {
    unsafe { guest_sigaltstack_apply(cpu, ss, oss) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_altstack_flags(cpu: *const OcerzCPU) -> u32 {
    unsafe { guest_altstack_flags(cpu) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_set_onstack(cpu: *mut OcerzCPU, on: c_int) {
    unsafe { (*cpu).sig_on_stack = c_int::from(on != 0) }
}

unsafe fn native_kill_host(port: mach_port_t, sig: c_int) -> c_int {
    unsafe {
        let a = [port as u64, sig as u32 as u64, 0, 0, 0, 0, 0, 0];
        let mut err = 0;
        let r = raw::ocerz_host_syscall(328, &a, ptr::null_mut(), &mut err);
        if err != 0 { r as c_int } else { 0 }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_raise(
    _vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    sig: c_int,
) -> c_int {
    unsafe {
        if sig < 0 || sig >= DARWIN_NSIG {
            return libc::EINVAL;
        }
        if sig == 0 {
            return 0;
        }
        if sig == 6 {
            guest_abort_note(cpu);
        }
        if guest_selfkill_routed() != 0 && guest_self_signal(cpu, sig, 1) != GUEST_SELFSIG_HOST {
            return 0;
        }
        native_kill_host(pthread_mach_thread_np(libc::pthread_self()), sig)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_pthread_kill(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    thread: u64,
    sig: c_int,
) -> c_int {
    unsafe {
        if sig < 0 || sig > DARWIN_NSIG {
            return libc::EINVAL;
        }
        let target = thread as usize as libc::pthread_t;
        if target != 0 && libc::pthread_equal(target, libc::pthread_self()) != 0 {
            return ocerz_guest_raise(vm, cpu, sig);
        }
        let port = if target == 0 {
            0
        } else {
            pthread_mach_thread_np(target)
        };
        let mut gpr = [0u64; 16];
        let mut rip = 0;
        let mut rflags = 0;
        if port == 0
            || crate::ffi::ocerz_vm_thread_regs(port, gpr.as_mut_ptr(), &mut rip, &mut rflags) != 0
        {
            return libc::ESRCH;
        }
        if sig == DARWIN_NSIG {
            return libc::EINVAL;
        }
        if sig == 0 {
            return 0;
        }
        if sig == 6 {
            guest_abort_note(cpu);
        }
        native_kill_host(port, sig)
    }
}

unsafe fn guest_waiter(cpu: *mut OcerzCPU, accept: u64) {
    unsafe {
        libc::pthread_mutex_lock(ptr::addr_of_mut!(GUEST_WAITERS_LOCK));
        let waiters = ptr::addr_of_mut!(GUEST_WAITERS_LIST).cast::<GuestWaiter>();
        let mut free_at = -1;
        for k in 0..GUEST_WAITERS {
            let waiter = waiters.add(k);
            if (*waiter).cpu == cpu {
                (*waiter).accept = accept;
                if accept == 0 {
                    (*waiter).cpu = ptr::null_mut();
                }
                libc::pthread_mutex_unlock(ptr::addr_of_mut!(GUEST_WAITERS_LOCK));
                return;
            }
            if free_at < 0 && (*waiter).cpu.is_null() {
                free_at = k as c_int;
            }
        }
        if accept != 0 && free_at >= 0 {
            let waiter = waiters.add(free_at as usize);
            (*waiter).cpu = cpu;
            (*waiter).accept = accept;
        }
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(GUEST_WAITERS_LOCK));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_post_to_waiter(sig: c_int) -> c_int {
    unsafe {
        if sig <= 0 || sig > 64 {
            return 0;
        }
        let bit = 1u64 << (sig - 1);
        let mut posted = 0;
        libc::pthread_mutex_lock(ptr::addr_of_mut!(GUEST_WAITERS_LOCK));
        let waiters = ptr::addr_of_mut!(GUEST_WAITERS_LIST).cast::<GuestWaiter>();
        for k in 0..GUEST_WAITERS {
            let waiter = waiters.add(k);
            if !(*waiter).cpu.is_null() && (*waiter).accept & bit != 0 {
                sig_pending_atomic((*waiter).cpu).fetch_or(bit, Ordering::SeqCst);
                posted = 1;
                break;
            }
        }
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(GUEST_WAITERS_LOCK));
        posted
    }
}

unsafe fn guest_suspend_wait(vm: *mut OcerzVM, cpu: *mut OcerzCPU, suspend: u64) -> c_int {
    unsafe {
        let saved = (*cpu).sig_mask;
        guest_waiter(cpu, !suspend & 0xffff_ffff);
        (*cpu).sig_mask = suspend;
        (*cpu).block_nokick = 1;
        (*cpu).block_since_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        let mut caught = 0;
        loop {
            sig_pending_atomic(cpu).fetch_or(
                (crate::ffi::ocerz_take_pending_async_sig_mask(super::hostwq::async_accept(
                    suspend,
                )) >> 1) as u64,
                Ordering::SeqCst,
            );
            let ready = sig_pending_atomic(cpu).load(Ordering::SeqCst) & !suspend;
            if ready != 0 {
                caught = ready.trailing_zeros() as c_int + 1;
                break;
            }
            if AtomicI32::from_ptr(ptr::addr_of_mut!((*vm).exited)).load(Ordering::Acquire) != 0
                || (*cpu).interrupt != 0
            {
                break;
            }
            let ts = libc::timespec {
                tv_sec: 0,
                tv_nsec: 2 * 1000 * 1000,
            };
            libc::nanosleep(&ts, ptr::null_mut());
        }
        guest_waiter(cpu, 0);
        (*cpu).block_since_ns = 0;
        (*cpu).block_nokick = 0;
        (*cpu).sig_mask = saved;
        if caught != 0 {
            sig_pending_atomic(cpu).fetch_and(!(1u64 << (caught - 1)), Ordering::SeqCst);
        }
        caught
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_sigsuspend(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    mask: u64,
) -> c_int {
    unsafe {
        guest_suspend_wait(
            vm,
            cpu,
            if mask == u64::MAX {
                (*cpu).sig_mask
            } else {
                mask as u32 as u64
            },
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_deliver_now(cpu: *mut OcerzCPU, sig: c_int) {
    unsafe {
        ptr::write_volatile(ptr::addr_of_mut!(g_ocerz_deliver_src), 1);
        ocerz_signal_deliver(cpu, sig, 0, 0, 0);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_sigpending(cpu: *mut OcerzCPU) -> u32 {
    unsafe { sig_pending_atomic(cpu).load(Ordering::SeqCst) as u32 }
}

unsafe fn guest_sigwait_loop(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    want: u32,
    waiter: bool,
) -> c_int {
    unsafe {
        if waiter {
            guest_waiter(cpu, want as u64);
        }
        (*cpu).block_nokick = 1;
        (*cpu).block_since_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        let mut got = 0;
        loop {
            sig_pending_atomic(cpu).fetch_or(
                (crate::ffi::ocerz_take_pending_async_sig_mask(want.wrapping_shl(1)) >> 1) as u64,
                Ordering::SeqCst,
            );
            let hit = sig_pending_atomic(cpu).load(Ordering::SeqCst) as u32 & want;
            if hit != 0 {
                got = hit.trailing_zeros() as c_int + 1;
                sig_pending_atomic(cpu).fetch_and(!(1u64 << (got - 1)), Ordering::SeqCst);
                break;
            }
            if AtomicI32::from_ptr(ptr::addr_of_mut!((*vm).exited)).load(Ordering::Acquire) != 0
                || (*cpu).interrupt != 0
            {
                break;
            }
            let ts = libc::timespec {
                tv_sec: 0,
                tv_nsec: 2 * 1000 * 1000,
            };
            libc::nanosleep(&ts, ptr::null_mut());
        }
        if waiter {
            guest_waiter(cpu, 0);
        }
        (*cpu).block_since_ns = 0;
        (*cpu).block_nokick = 0;
        got
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_sigwait(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    want: u32,
) -> c_int {
    unsafe { guest_sigwait_loop(vm, cpu, want, true) }
}

pub(super) unsafe fn sys_sigpending(
    _vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    a: *mut [u64; 8],
) -> c_int {
    unsafe {
        let set = (*a)[0];
        if set != 0 {
            ocerz_st(
                set,
                4,
                sig_pending_atomic(cpu).load(Ordering::SeqCst) as u32 as u64,
            );
        }
        ret_ok(cpu, 0);
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

pub(super) unsafe fn sys_sigsuspend(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    a: *mut [u64; 8],
) -> c_int {
    unsafe {
        let saved = (*cpu).sig_mask;
        let suspend = (*a)[0] as u32 as u64;
        (*cpu).sig_mask = suspend;
        (*cpu).block_nokick = 1;
        (*cpu).block_since_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        let mut caught = 0;
        loop {
            sig_pending_atomic(cpu).fetch_or(
                (crate::ffi::ocerz_take_pending_async_sig_mask(super::hostwq::async_accept(
                    suspend,
                )) >> 1) as u64,
                Ordering::SeqCst,
            );
            let ready = sig_pending_atomic(cpu).load(Ordering::SeqCst) & !suspend;
            if ready != 0 {
                caught = ready.trailing_zeros() as c_int + 1;
                break;
            }
            if AtomicI32::from_ptr(ptr::addr_of_mut!((*vm).exited)).load(Ordering::Acquire) != 0
                || (*cpu).interrupt != 0
            {
                break;
            }
            let ts = libc::timespec {
                tv_sec: 0,
                tv_nsec: 2 * 1000 * 1000,
            };
            libc::nanosleep(&ts, ptr::null_mut());
        }
        (*cpu).block_since_ns = 0;
        (*cpu).block_nokick = 0;
        (*cpu).sig_mask = saved;
        ret_err(cpu, libc::EINTR as u64);
        if caught != 0 {
            sig_pending_atomic(cpu).fetch_and(!(1u64 << (caught - 1)), Ordering::SeqCst);
            ptr::write_volatile(ptr::addr_of_mut!(g_ocerz_deliver_src), 1);
            ocerz_signal_deliver(cpu, caught, 0, 0, 0);
        }
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

pub(super) unsafe fn sys_sigwait(vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    unsafe {
        let setp = (*a)[0];
        let sigp = (*a)[1];
        let want = if setp != 0 {
            ocerz_ld(setp, 4) as u32
        } else {
            0
        };
        let got = guest_sigwait_loop(vm, cpu, want, false);
        if got == 0 {
            ret_err(cpu, libc::EINTR as u64);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        if sigp != 0 {
            ocerz_st(sigp, 4, got as u32 as u64);
        }
        ret_ok(cpu, 0);
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_deliver_pending(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        if !guest_signal_ready(cpu) {
            return 0;
        }
        c_int::from(
            deliver_async_signals(
                vm,
                cpu,
                crate::ffi::ocerz_take_pending_async_sig_mask(super::hostwq::async_accept(
                    (*cpu).sig_mask,
                )),
            ) > 0,
        )
    }
}

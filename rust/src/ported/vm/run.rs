use core::ffi::{c_char, c_int, c_long, c_ulonglong, c_void};
use core::ptr;
use core::sync::atomic::{AtomicI32, AtomicU32, AtomicU64, Ordering};

use super::*;
use crate::ffi::{OcerzCPU, OcerzGuestCall, OcerzVM, sigjmp_buf};

core::arch::global_asm!(
    ".section __TEXT,__text",
    ".private_extern _ocerz_vm_setjmp_run",
    ".globl _ocerz_vm_setjmp_run",
    ".p2align 2",
    "_ocerz_vm_setjmp_run:",
    "stp x29, x30, [sp, #-48]!",
    "mov x29, sp",
    "stp x19, x20, [sp, #16]",
    "stp x21, x22, [sp, #32]",
    "mov x20, x2",
    "mov x21, x3",
    "bl _sigsetjmp",
    "mov w1, w0",
    "mov x0, x21",
    "blr x20",
    "ldp x21, x22, [sp, #32]",
    "ldp x19, x20, [sp, #16]",
    "ldp x29, x30, [sp], #48",
    "ret",
);

unsafe extern "C" {
    fn ocerz_vm_setjmp_run(
        buf: *mut sigjmp_buf,
        savemask: c_int,
        body: unsafe extern "C" fn(*mut c_void, c_int) -> c_int,
        ctx: *mut c_void,
    ) -> c_int;
    fn sigsetjmp(env: *mut sigjmp_buf, savemask: c_int) -> c_int;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_init(vm: *mut OcerzVM) -> c_int {
    unsafe {
        G_IMAGE_SLIDE = _dyld_get_image_vmaddr_slide(0) as u64;
        ptr::write_bytes(vm, 0, 1);
        (*vm).cpu.vm = vm;
        ffi::ocerz_cpu_reset(&raw mut (*vm).cpu);
        (*vm).jit_enabled = 1;
        ffi::OCERZ_OK
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_install_handlers(vm: *mut OcerzVM) {
    unsafe {
        sig::ocerz_install_kick_handler();
        ocerz_cftrap_on = (!libc::getenv(c"OCERZ_CFTRAP".as_ptr()).is_null()) as c_int;
        G_CRASH_STACK = (!libc::getenv(c"OCERZ_CRASH_STACK".as_ptr()).is_null()) as c_int;
        G_SIGTRACE = (!libc::getenv(c"OCERZ_SIGTRACE".as_ptr()).is_null()) as c_int;
        G_WINEFAULTLOG = (!libc::getenv(c"OCERZ_WINEFAULTLOG".as_ptr()).is_null()) as c_int;
        let w = libc::getenv(c"OCERZ_WATCH".as_ptr());
        {
            let wl = libc::getenv(c"OCERZ_WATCHLEN".as_ptr());
            if !wl.is_null() {
                ocerz_watch_len = libc::strtoull(wl, ptr::null_mut(), 0);
            }
        }
        if !w.is_null() {
            ocerz_watch_addr = libc::strtoull(w, ptr::null_mut(), 0);
        }
        let wv = libc::getenv(c"OCERZ_STVAL".as_ptr());
        if !wv.is_null() {
            ocerz_watch_val = libc::strtoull(wv, ptr::null_mut(), 0);
        }
        ocerz_watch_shadow = (!libc::getenv(c"OCERZ_SHADOWST".as_ptr()).is_null()) as u64;
        let et = libc::getenv(c"OCERZ_EXCTRAP".as_ptr());
        if !et.is_null() {
            ocerz_exc_trap = libc::strtoull(et, ptr::null_mut(), 0);
        }
        let st_ = libc::getenv(c"OCERZ_SELTRAP".as_ptr());
        if !st_.is_null() {
            ocerz_sel_trap = libc::strtoull(st_, ptr::null_mut(), 0);
        }
        let gt = libc::getenv(c"OCERZ_ARGTRAP".as_ptr());
        if !gt.is_null() {
            ocerz_arg_trap = libc::strtoull(gt, ptr::null_mut(), 0);
        }
        let ct = libc::getenv(c"OCERZ_CTXTRAP".as_ptr());
        if !ct.is_null() {
            ocerz_ctx_trap = libc::strtoull(ct, ptr::null_mut(), 0);
        }
        let bl = libc::getenv(c"OCERZ_BT_LO".as_ptr());
        let bh = libc::getenv(c"OCERZ_BT_HI".as_ptr());
        if !bl.is_null() && !bh.is_null() {
            ocerz_bt_lo = libc::strtoull(bl, ptr::null_mut(), 0);
            ocerz_bt_hi = libc::strtoull(bh, ptr::null_mut(), 0);
        }
        static mut ALTSS: libc::stack_t = libc::stack_t {
            ss_sp: ptr::null_mut(),
            ss_size: 0,
            ss_flags: 0,
        };
        if ALTSS.ss_sp.is_null() {
            ALTSS.ss_size = if SIGSTKSZ < 0x10000 {
                0x10000
            } else {
                SIGSTKSZ as usize
            };
            ALTSS.ss_sp = libc::mmap(
                ptr::null_mut(),
                ALTSS.ss_size,
                PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANON,
                -1,
                0,
            );
            if ALTSS.ss_sp != MAP_FAILED {
                libc::sigaltstack(&raw const ALTSS, ptr::null_mut());
            } else {
                ALTSS.ss_sp = ptr::null_mut();
            }
        }
        let mut sa: libc::sigaction = core::mem::zeroed();
        sa.sa_sigaction = sig::crash_handler as libc::sighandler_t;
        sa.sa_flags = (SA_SIGINFO | SA_NODEFER) as c_int
            | if !ALTSS.ss_sp.is_null() {
                SA_ONSTACK as c_int
            } else {
                0
            };
        libc::sigaction(SIGSEGV, &sa, ptr::null_mut());
        libc::sigaction(SIGBUS, &sa, ptr::null_mut());
        libc::sigaction(SIGILL, &sa, ptr::null_mut());
        libc::sigaction(SIGTRAP, &sa, ptr::null_mut());
        libc::sigaction(SIGSYS, &sa, ptr::null_mut());

        let mut as_: libc::sigaction = core::mem::zeroed();
        as_.sa_sigaction = sig::async_sig_handler as libc::sighandler_t;
        as_.sa_flags = (SA_SIGINFO | SA_NODEFER) as c_int;
        libc::sigaction(SIGQUIT, &as_, ptr::null_mut());
        if !libc::getenv(c"OCERZ_RIPDUMP".as_ptr()).is_null() {
            let mut su: libc::sigaction = core::mem::zeroed();
            su.sa_sigaction = sig::ripdump_handler as libc::sighandler_t;
            su.sa_flags = (SA_SIGINFO | SA_NODEFER) as c_int;
            libc::sigaction(SIGUSR1, &su, ptr::null_mut());
        } else {
            libc::sigaction(SIGUSR1, &as_, ptr::null_mut());
        }
        if !libc::getenv(c"OCERZ_PORTDUMP".as_ptr()).is_null() {
            let mut sp: libc::sigaction = core::mem::zeroed();
            sp.sa_sigaction = sig::portdump_handler as libc::sighandler_t;
            sp.sa_flags = (SA_SIGINFO | SA_NODEFER) as c_int;
            libc::sigaction(SIGUSR2, &sp, ptr::null_mut());
            libc::fprintf(
                stderr(),
                c"ocerz: PORTDUMP[%d] armed\n".as_ptr(),
                libc::getpid(),
            );
        }
        {
            let mut st: libc::sigaction = core::mem::zeroed();
            st.sa_sigaction = sig::threaddump_handler as libc::sighandler_t;
            st.sa_flags = (SA_SIGINFO | SA_NODEFER) as c_int;
            libc::sigaction(SIGINFO, &st, ptr::null_mut());
        }
        G_VM = vm;
        if (*vm).jit_enabled != 0 && (*vm).jit.is_null() {
            (*vm).jit = ffi::ocerz_jit_create(vm);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_peek_dump(tag: *const c_char) {
    unsafe {
        let pk = libc::getenv(c"OCERZ_PEEK".as_ptr());
        if pk.is_null() {
            return;
        }
        let mut pk = pk;
        libc::fprintf(
            stderr(),
            c"ocerz: peek(%s):".as_ptr(),
            if !tag.is_null() { tag } else { c"".as_ptr() },
        );
        while *pk != 0 {
            let a = libc::strtoull(pk, &mut pk, 0);
            if *pk == ',' as c_char {
                pk = pk.add(1);
            }
            libc::fprintf(stderr(), c" [%#llx]=".as_ptr(), a as c_ulonglong);
            if ocerz_addr_readable(a) != 0 || ffi::ocerz_cache_region(a as usize) != 0 {
                libc::fprintf(stderr(), c"%#llx".as_ptr(), ocerz_ld(a, 8) as c_ulonglong);
            } else {
                libc::fprintf(stderr(), c"uncommitted".as_ptr());
            }
            let mut ra: mach_vm_address_t = a;
            let mut rs: mach_vm_size_t = 0;
            let mut bi: vm_region_basic_info_data_64 = core::mem::zeroed();
            let mut bc: mach_msg_type_number_t = VM_REGION_BASIC_INFO_COUNT_64;
            let mut ob: mach_port_t = MACH_PORT_NULL;
            if mach_vm_region(
                mach_task_self(),
                &mut ra,
                &mut rs,
                VM_REGION_BASIC_INFO_64,
                &mut bi as *mut _ as vm_region_info_t,
                &mut bc,
                &mut ob,
            ) == KERN_SUCCESS
            {
                if ob != MACH_PORT_NULL {
                    mach_port_deallocate(mach_task_self(), ob);
                }
                libc::fprintf(
                    stderr(),
                    c"{region %#llx+%#llx %c%c%c}".as_ptr(),
                    ra as c_ulonglong,
                    rs as c_ulonglong,
                    if bi.protection & VM_PROT_READ != 0 {
                        'r' as c_int
                    } else {
                        '-' as c_int
                    },
                    if bi.protection & VM_PROT_WRITE != 0 {
                        'w' as c_int
                    } else {
                        '-' as c_int
                    },
                    if bi.protection & VM_PROT_EXECUTE != 0 {
                        'x' as c_int
                    } else {
                        '-' as c_int
                    },
                );
            }
        }
        libc::fprintf(stderr(), c"\n".as_ptr());
        libc::fflush(stderr());
    }
}

static mut G_CALL_SENTINEL: u64 = 0;
static mut G_CALL_SENTINEL_ONCE: libc::pthread_once_t = libc::PTHREAD_ONCE_INIT;

unsafe extern "C" fn call_sentinel_init() {
    unsafe {
        let want = 0x500000000usize as *mut c_void;
        let p = libc::mmap(
            want,
            0x1000,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANON,
            -1,
            0,
        );
        if p == want {
            ptr::write_bytes(p, 0xcc, 0x1000);
            G_CALL_SENTINEL = 0x500000000;
        } else {
            if p != MAP_FAILED {
                libc::munmap(p, 0x1000);
            }
            G_CALL_SENTINEL = OCERZ_CALL_SENTINEL;
        }
        ocerz_log!(
            "vm: call sentinel page at %#llx\n",
            G_CALL_SENTINEL as c_ulonglong
        );
    }
}

static mut ICAP: u64 = 0;
static mut PROF: u64 = 0;
static mut ICAP_PROF_INIT: c_int = 0;
static mut RIPTRAP: [u64; 16] = [0; 16];
static mut RIPTRAP_N: i32 = -1;
static mut RIPTRAP_HIT: [u8; 16] = [0; 16];
static mut MTRACE_LO: u64 = 0;
static mut MTRACE_HI: u64 = 0;
static mut MTRACE_INIT: c_int = 0;
static FORCETOL: AtomicI32 = AtomicI32::new(-1);

#[repr(C)]
struct CallCtx {
    vm: *mut OcerzVM,
    func: u64,
    call: *mut OcerzGuestCall,
    sentinel: u64,
    prev_cpu: *mut OcerzCPU,
    prev_kport: u32,
    prev_recover: *mut sigjmp_buf,
    jmark: u64,
    local: OcerzCPU,
    jb: sigjmp_buf,
    any_diag: c_int,
    prof_next: u64,
    esc_r: c_int,
}

unsafe extern "C" fn vm_call_body(ctx_: *mut c_void, rc: c_int) -> c_int {
    unsafe {
        let ctx = ctx_ as *mut CallCtx;
        let c = &mut *ctx;
        c.esc_r = if rc == 2 { T_JIT_ESCAPE_R } else { 0 };
        T_JIT_ESCAPE_R = 0;
        ffi::ocerz_jit_thread_restore(c.jmark);
        G_CUR_CPU = &mut c.local;
        ffi::ocerz_apply_mxcsr_round(c.local.mxcsr);
        if !c.prev_cpu.is_null() {
            libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
            (*c.prev_cpu).host_kport = 0;
            libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
        }
        ocerz_cpu_register(&mut c.local);
        let local = &mut c.local as *mut OcerzCPU;
        let vm = c.vm;
        while (*local).rip != c.sentinel && (*vm).exited == 0 && (*local).terminated == 0 {
            ocerz_vm_suspend_point(local);
            G_RIPHIST[(G_RIPHIST_N & 31) as usize] = (*local).rip;
            G_RIPHIST_N = G_RIPHIST_N.wrapping_add(1);
            let mut r: c_int;
            let mut mtrace_hit = false;
            if c.any_diag != 0 {
                if ocerz_exc_trap != 0 && (*local).rip == ocerz_exc_trap {
                    ocerz_exc_report(local);
                }
                if ocerz_arg_trap != 0 && (*local).rip == ocerz_arg_trap {
                    arg_trap_report(local);
                }
                if ocerz_sel_trap != 0 && (*local).rip == ocerz_sel_trap {
                    sel_trap_report(local);
                }
                if ocerz_ctx_trap != 0 && (*local).rip == ocerz_ctx_trap {
                    ctx_trap_report(local);
                }
                if ocerz_bt_lo != 0 && (*local).rip >= ocerz_bt_lo && (*local).rip < ocerz_bt_hi {
                    ocerz_bt_report(local);
                }
                if PROF != 0 && (*vm).insn_count >= c.prof_next {
                    c.prof_next = (*vm).insn_count + PROF;
                    libc::fprintf(
                        stderr(),
                        c"ocerz: PROFILE icount=%llu rip=%#llx\n".as_ptr(),
                        (*vm).insn_count as c_ulonglong,
                        (*local).rip as c_ulonglong,
                    );
                }
                for t in 0..RIPTRAP_N {
                    if (*local).rip == *((&raw const RIPTRAP) as *const u64).add(t as usize)
                        && *((&raw mut RIPTRAP_HIT) as *mut u8).add(t as usize) < 40
                    {
                        *((&raw mut RIPTRAP_HIT) as *mut u8).add(t as usize) += 1;
                        libc::fprintf(
                            stderr(),
                            c"ocerz: RIPLOG hit %#llx (#%d) icount=%llu rdi=%#llx rsi=%#llx rdx=%#llx rbx=%#llx r14=%#llx\n"
                                .as_ptr(),
                            *((&raw const RIPTRAP) as *const u64).add(t as usize) as c_ulonglong,
                            *((&raw mut RIPTRAP_HIT) as *mut u8).add(t as usize) as c_int,
                            (*vm).insn_count as c_ulonglong,
                            (*local).gpr[OCERZ_RDI] as c_ulonglong,
                            (*local).gpr[OCERZ_RSI] as c_ulonglong,
                            (*local).gpr[OCERZ_RDX] as c_ulonglong,
                            (*local).gpr[OCERZ_RBX] as c_ulonglong,
                            (*local).gpr[OCERZ_R14] as c_ulonglong,
                        );
                    }
                }
                if ICAP != 0 && (*vm).insn_count > ICAP {
                    ffi::ocerz_cpu_dump(local, stderr() as *mut ffi::FILE);
                    let mut fp = (*local).gpr[OCERZ_RBP];
                    libc::fprintf(stderr(), c"ocerz: rbp-chain:".as_ptr());
                    let mut d = 0;
                    while d < 200 && fp >= 0x300000000 {
                        let ret = ocerz_ld(fp + 8, 8);
                        libc::fprintf(stderr(), c" %#llx".as_ptr(), ret as c_ulonglong);
                        let nf = ocerz_ld(fp, 8);
                        if nf <= fp {
                            break;
                        }
                        fp = nf;
                        d += 1;
                    }
                    libc::fprintf(stderr(), c"\n".as_ptr());
                    libc::fprintf(
                        stderr(),
                        c"ocerz: ICAP hit at %llu instructions, func=%#llx\n".as_ptr(),
                        (*vm).insn_count as c_ulonglong,
                        c.func as c_ulonglong,
                    );
                    libc::_exit(126);
                }
                if MTRACE_LO != 0 && (*local).rip >= MTRACE_LO && (*local).rip < MTRACE_HI {
                    libc::fprintf(
                        stderr(),
                        c"MT %#llx rax=%#llx rdi=%#llx rsi=%#llx rsp=%#llx [rsp]=%#llx rbx=%#llx rbp=%#llx r12=%#llx r13=%#llx r14=%#llx r15=%#llx\n"
                            .as_ptr(),
                        (*local).rip as c_ulonglong,
                        (*local).gpr[OCERZ_RAX] as c_ulonglong,
                        (*local).gpr[OCERZ_RDI] as c_ulonglong,
                        (*local).gpr[OCERZ_RSI] as c_ulonglong,
                        (*local).gpr[OCERZ_RSP] as c_ulonglong,
                        ocerz_ld((*local).gpr[OCERZ_RSP], 8) as c_ulonglong,
                        (*local).gpr[OCERZ_RBX] as c_ulonglong,
                        (*local).gpr[OCERZ_RBP] as c_ulonglong,
                        (*local).gpr[OCERZ_R12] as c_ulonglong,
                        (*local).gpr[OCERZ_R13] as c_ulonglong,
                        (*local).gpr[OCERZ_R14] as c_ulonglong,
                        (*local).gpr[OCERZ_R15] as c_ulonglong,
                    );
                    mtrace_hit = true;
                }
            }

            if c.esc_r != 0 {
                r = c.esc_r;
                c.esc_r = 0;
                if r == ffi::OCERZ_EUNSUP as c_int {
                    r = ffi::ocerz_interp_step(vm, local);
                }
            } else if mtrace_hit || (*local).interp_once != 0 {
                let was_once = (*local).interp_once;
                (*local).interp_once = 0;
                r = ffi::ocerz_interp_step(vm, local);
                if was_once != 0 && !libc::getenv(c"OCERZ_CPFAULTLOG".as_ptr()).is_null() {
                    libc::fprintf(
                        stderr(),
                        c"ocerz: INTERP-ONCE done -> rip=%#llx rax=%#llx rcx=%#llx rdx=%#llx r=%d\n"
                            .as_ptr(),
                        (*local).rip as c_ulonglong,
                        (*local).gpr[OCERZ_RAX] as c_ulonglong,
                        (*local).gpr[OCERZ_RCX] as c_ulonglong,
                        (*local).gpr[OCERZ_RDX] as c_ulonglong,
                        r,
                    );
                }
            } else if (*vm).jit_enabled != 0
                && (!(*vm).jit.is_null() || {
                    (*vm).jit = ffi::ocerz_jit_create(vm);
                    !(*vm).jit.is_null()
                })
            {
                r = ffi::ocerz_jit_step(vm, local);
                if r == ffi::OCERZ_EUNSUP as c_int {
                    r = ffi::ocerz_interp_step(vm, local);
                }
            } else {
                r = ffi::ocerz_interp_step(vm, local);
            }
            if r == ffi::OCERZ_STEP_EXIT as c_int {
                break;
            }
            if r == ffi::OCERZ_STEP_FATAL as c_int {
                if FORCETOL.load(Ordering::Relaxed) < 0 {
                    FORCETOL.store(
                        (!libc::getenv(c"OCERZ_INITTOL".as_ptr()).is_null()) as i32,
                        Ordering::Relaxed,
                    );
                }
                if ocerz_init_tolerant != 0 || FORCETOL.load(Ordering::Relaxed) != 0 {
                    libc::fprintf(
                        stderr(),
                        c"ocerz: init-skip: initializer %#llx faulted at rip=%#llx (skipped, process continues)\n"
                            .as_ptr(),
                        c.func as c_ulonglong,
                        (*local).rip as c_ulonglong,
                    );
                    break;
                }
                {
                    let mut frames = [0u64; 24];
                    let mut nframes = 0usize;
                    libc::fprintf(
                        stderr(),
                        c"ocerz: initabort-bt rip=%#llx".as_ptr(),
                        (*local).rip as c_ulonglong,
                    );
                    let mut fp = (*local).gpr[OCERZ_RBP];
                    let mut d = 0;
                    while d < 24 && fp >= 0x10000 {
                        let ra = if ocerz_addr_readable(fp + 8) != 0 {
                            ocerz_ld(fp + 8, 8)
                        } else {
                            0
                        };
                        libc::fprintf(stderr(), c" %#llx".as_ptr(), ra as c_ulonglong);
                        *frames.as_mut_ptr().add(nframes) = ra;
                        nframes += 1;
                        let nf = if ocerz_addr_readable(fp) != 0 {
                            ocerz_ld(fp, 8)
                        } else {
                            0
                        };
                        if nf <= fp {
                            break;
                        }
                        fp = nf;
                        d += 1;
                    }
                    libc::fprintf(stderr(), c"\n".as_ptr());
                    let mut rb: u64 = 0;
                    let rn = ocerz_dyld_name_for_addr((*local).rip, &mut rb);
                    if !rn.is_null() {
                        libc::fprintf(
                            stderr(),
                            c"    frame %#llx %s+%#llx\n".as_ptr(),
                            (*local).rip as c_ulonglong,
                            rn,
                            ((*local).rip - rb) as c_ulonglong,
                        );
                    }
                    for d in 0..nframes {
                        let mut b_: u64 = 0;
                        let n = ocerz_dyld_name_for_addr(*frames.as_mut_ptr().add(d), &mut b_);
                        if !n.is_null() {
                            libc::fprintf(
                                stderr(),
                                c"    frame %#llx %s+%#llx\n".as_ptr(),
                                *frames.as_mut_ptr().add(d) as c_ulonglong,
                                n,
                                (*frames.as_mut_ptr().add(d) - b_) as c_ulonglong,
                            );
                        }
                    }
                }
                ffi::ocerz_cpu_dump(local, stderr() as *mut ffi::FILE);
                ocerz_peek_dump(c"fatal".as_ptr());
                ocerz_fatal!(
                    "initializer call to %#llx aborted after %llu instructions\n",
                    c.func as c_ulonglong,
                    (*vm).insn_count as c_ulonglong
                );
                libc::_exit(125);
            }
        }
        ocerz_cpu_unregister(local);
        if !c.prev_cpu.is_null() {
            libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
            (*c.prev_cpu).host_kport = c.prev_kport;
            libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
        }
        G_SIG_RECOVER = c.prev_recover;
        if !c.prev_cpu.is_null() && (*vm).jit_ordered_required != 0 {
            AtomicU32::from_ptr(&raw mut (*c.prev_cpu).ras_top).store(0, Ordering::Release);
        }
        G_CUR_CPU = c.prev_cpu;
        {
            let left = AtomicU64::from_ptr(&raw mut (*local).sig_pending).swap(0, Ordering::SeqCst);
            if !c.prev_cpu.is_null() {
                if left != 0 {
                    AtomicU64::from_ptr(&raw mut (*c.prev_cpu).sig_pending)
                        .fetch_or(left, Ordering::SeqCst);
                    (*c.prev_cpu).nested_sig_handback =
                        (*c.prev_cpu).nested_sig_handback.wrapping_add(1);
                }
                for s in 1..32usize {
                    AtomicU32::from_ptr(
                        ((&raw mut (*c.prev_cpu).sig_host_rcvd) as *mut u32).add(s),
                    )
                    .fetch_add(*(*local).sig_host_rcvd.get_unchecked(s), Ordering::Relaxed);
                    AtomicU32::from_ptr(
                        ((&raw mut (*c.prev_cpu).sig_delivered) as *mut u32).add(s),
                    )
                    .fetch_add(*(*local).sig_delivered.get_unchecked(s), Ordering::Relaxed);
                }
            } else if left != 0 {
                AtomicU32::from_ptr(&raw mut G_PENDING_ASYNC_MASK_TLS)
                    .fetch_or((left as u32) << 1, Ordering::SeqCst);
            }
        }
        (*c.call).rax = (*local).gpr[OCERZ_RAX];
        (*c.call).xmm0 = (*local).xmm[0].lo;
        (*c.call).rdx = (*local).gpr[OCERZ_RDX];
        (*c.call).xmm1 = (*local).xmm[1].lo;
        ((*local).rip != c.sentinel || (*vm).exited != 0) as c_int
    }
}

unsafe fn vm_call_core(
    vm: *mut OcerzVM,
    func: u64,
    call: *mut OcerzGuestCall,
    ngpr: c_int,
    nxmm: c_int,
    stack_top: u64,
    context: *const u64,
) -> c_int {
    unsafe {
        static AR: [usize; 6] = [
            OCERZ_RDI, OCERZ_RSI, OCERZ_RDX, OCERZ_RCX, OCERZ_R8, OCERZ_R9,
        ];
        libc::pthread_once(&raw mut G_CALL_SENTINEL_ONCE, Some(call_sentinel_init));
        let sentinel = G_CALL_SENTINEL;
        let prev_cpu = G_CUR_CPU;
        let mut local: OcerzCPU = if !prev_cpu.is_null() {
            ptr::read(prev_cpu)
        } else {
            ptr::read(&raw const (*vm).cpu)
        };
        local.sig_pending = if !prev_cpu.is_null() {
            AtomicU64::from_ptr(&raw mut (*prev_cpu).sig_pending).swap(0, Ordering::SeqCst)
        } else {
            0
        };
        ptr::write_bytes(local.sig_host_rcvd.as_mut_ptr(), 0, 32);
        ptr::write_bytes(local.sig_delivered.as_mut_ptr(), 0, 32);
        local.terminated = 0;
        local.suspend_count = 0;
        local.susp_parked = 0;
        local.susp_host = 0;
        local.susp_have_gpr = 0;
        local.host_pthread = pthread_self() as *mut c_void;
        local.host_tsd = host_tsd_self();
        local.host_kport = pthread_mach_thread_np(pthread_self());
        local.bridge_depth = ffi::ocerz_bridge_depth_ptr();
        local.jit_lock_depth = ffi::ocerz_jit_lock_depth_ptr();
        pthread_threadid_np(0, &mut local.host_tid);
        let prev_kport = if !prev_cpu.is_null() {
            (*prev_cpu).host_kport
        } else {
            0
        };
        let mut i = 0;
        while i < ngpr && i < 6 {
            *local.gpr.get_unchecked_mut(*AR.get_unchecked(i as usize)) =
                *(*call).gpr.get_unchecked(i as usize);
            i += 1;
        }
        let mut i = 0;
        while i < nxmm && i < 8 {
            (*local.xmm.as_mut_ptr().add(i as usize)).lo = *(*call).xmm.get_unchecked(i as usize);
            (*local.xmm.as_mut_ptr().add(i as usize)).hi = 0;
            i += 1;
        }
        if !context.is_null() {
            local.gpr[OCERZ_R13] = *context;
        }
        let nstack = if (*call).nstack < 0 {
            0
        } else if (*call).nstack > 16 {
            16
        } else {
            (*call).nstack
        };
        let argbase = ((stack_top & !0xfu64) - 8 * nstack as u64) & !0xfu64;
        let sp = argbase - 8;
        ocerz_st(sp, 8, sentinel);
        let mut i = 0;
        while i < nstack {
            ocerz_st(
                argbase + 8 * i as u64,
                8,
                *(*call).stack.get_unchecked(i as usize),
            );
            i += 1;
        }
        local.gpr[OCERZ_RSP] = sp;
        local.rip = func;

        if ICAP_PROF_INIT == 0 {
            let icap_s = libc::getenv(c"OCERZ_ICAP".as_ptr());
            ICAP = if !icap_s.is_null() {
                libc::strtoull(icap_s, ptr::null_mut(), 0)
            } else {
                0
            };
            let prof_s = libc::getenv(c"OCERZ_PROFILE".as_ptr());
            PROF = if !prof_s.is_null() {
                libc::strtoull(prof_s, ptr::null_mut(), 0)
            } else {
                0
            };
            ICAP_PROF_INIT = 1;
        }
        if RIPTRAP_N < 0 {
            RIPTRAP_N = 0;
            let mut rs = libc::getenv(c"OCERZ_RIPLOG".as_ptr());
            while !rs.is_null() && *rs != 0 && RIPTRAP_N < 16 {
                *((&raw mut RIPTRAP) as *mut u64).add(RIPTRAP_N as usize) =
                    libc::strtoull(rs, &mut rs, 0);
                RIPTRAP_N += 1;
                while *rs == ',' as c_char || *rs == ' ' as c_char {
                    rs = rs.add(1);
                }
            }
        }
        let prof_next = if PROF != 0 {
            (*vm).insn_count + PROF
        } else {
            0
        };

        if MTRACE_INIT == 0 {
            MTRACE_INIT = 1;
            let ml = libc::getenv(c"OCERZ_TRACE_MAIN_LO".as_ptr());
            let mh = libc::getenv(c"OCERZ_TRACE_MAIN_HI".as_ptr());
            if !ml.is_null() && !mh.is_null() {
                MTRACE_LO = libc::strtoull(ml, ptr::null_mut(), 0);
                MTRACE_HI = libc::strtoull(mh, ptr::null_mut(), 0);
            }
        }

        let any_diag = (ocerz_exc_trap != 0
            || ocerz_sel_trap != 0
            || ocerz_arg_trap != 0
            || ocerz_ctx_trap != 0
            || ocerz_bt_lo != 0
            || PROF != 0
            || RIPTRAP_N > 0
            || ICAP != 0
            || MTRACE_LO != 0) as c_int;

        let mut c = CallCtx {
            vm,
            func,
            call,
            sentinel,
            prev_cpu,
            prev_kport,
            prev_recover: G_SIG_RECOVER,
            jmark: 0,
            local,
            jb: [0; 49],
            any_diag,
            prof_next,
            esc_r: 0,
        };
        G_CUR_CPU = &mut c.local;
        G_SIG_RECOVER = &mut c.jb;
        ocerz_host_sigmask_clear(c"callback".as_ptr());
        c.jmark = ffi::ocerz_jit_thread_mark();
        ocerz_vm_setjmp_run(
            &mut c.jb,
            1,
            vm_call_body,
            &mut c as *mut CallCtx as *mut c_void,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_call(
    vm: *mut OcerzVM,
    func: u64,
    args: *const u64,
    nargs: c_int,
    stack_top: u64,
) -> u64 {
    unsafe {
        let mut call: OcerzGuestCall = core::mem::zeroed();
        let mut i = 0;
        while i < nargs && i < 6 {
            *call.gpr.get_unchecked_mut(i as usize) = *args.add(i as usize);
            i += 1;
        }
        call.nstack = 0;
        vm_call_core(vm, func, &mut call, nargs, 0, stack_top, ptr::null());
        call.rax
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_call_abi(
    vm: *mut OcerzVM,
    func: u64,
    call: *mut OcerzGuestCall,
    stack_top: u64,
) -> c_int {
    unsafe { vm_call_core(vm, func, call, 6, 8, stack_top, ptr::null()) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_call_swift_context(
    vm: *mut OcerzVM,
    func: u64,
    context: u64,
    stack_top: u64,
) -> c_int {
    unsafe {
        let mut call: OcerzGuestCall = core::mem::zeroed();
        vm_call_core(vm, func, &mut call, 0, 0, stack_top, &context)
    }
}

pub(super) const OCERZ_ATTACH_REGION: u64 = 0x200000;
pub(super) const OCERZ_ATTACH_BLOCK: u64 = 0x1f0000;
pub(super) const OCERZ_ATTACH_GS: u64 = 0xe0;
pub(super) const OCERZ_ATTACH_MAX: usize = 1024;

#[repr(C)]
pub(super) struct AttachedThread {
    cpu: OcerzCPU,
    region: u64,
}

#[thread_local]
static mut G_ATTACHED: *mut AttachedThread = ptr::null_mut();

#[repr(C)]
#[derive(Clone, Copy)]
struct AttachStack {
    thread: libc::pthread_t,
    region: u64,
}
static mut G_ATTACH_STACKS: [AttachStack; OCERZ_ATTACH_MAX] = [const {
    AttachStack {
        thread: 0,
        region: 0,
    }
}; OCERZ_ATTACH_MAX];
static mut G_ATTACH_STACKS_N: c_int = 0;
static mut G_ATTACH_KEY: libc::pthread_key_t = 0;
static G_ATTACH_KEY_OK: AtomicI32 = AtomicI32::new(0);
static mut G_ATTACH_ONCE: libc::pthread_once_t = libc::PTHREAD_ONCE_INIT;

#[inline(always)]
unsafe fn attach_stacks() -> *mut AttachStack {
    &raw mut G_ATTACH_STACKS as *mut AttachStack
}

unsafe fn attach_release(at: *mut AttachedThread) {
    unsafe {
        let self_ = pthread_self();
        libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
        let mut i = 0;
        while i < G_CPUS_N {
            if *gcpus().add(i as usize) == &mut (*at).cpu
                || pthread_equal(*cputhreads().add(i as usize), self_) != 0
            {
                G_CPUS_N -= 1;
                let last = G_CPUS_N;
                *gcpus().add(i as usize) = *gcpus().add(last as usize);
                *cputhreads().add(i as usize) = *cputhreads().add(last as usize);
                *gcpus().add(last as usize) = ptr::null_mut();
                i -= 1;
            }
            i += 1;
        }
        let mut i = 0;
        while i < G_ATTACH_STACKS_N {
            if (*attach_stacks().add(i as usize)).region == (*at).region {
                G_ATTACH_STACKS_N -= 1;
                (*attach_stacks().add(i as usize)) =
                    (*attach_stacks().add(G_ATTACH_STACKS_N as usize));
                break;
            }
            i += 1;
        }
        libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
        G_ATTACHED = ptr::null_mut();
        G_CUR_CPU = ptr::null_mut();
        ffi::ocerz_tlv_release_thread((*at).cpu.gs_base);
        ffi::ocerz_unmap((*at).region, OCERZ_ATTACH_REGION);
        libc::free((*at).cpu.btrace as *mut c_void);
        libc::free(at as *mut c_void);
    }
}

unsafe extern "C" fn attach_thread_exit(arg: *mut c_void) {
    unsafe {
        if !arg.is_null() {
            attach_release(arg as *mut AttachedThread);
        }
    }
}

unsafe extern "C" fn attach_key_init() {
    unsafe {
        if libc::pthread_key_create(&raw mut G_ATTACH_KEY, Some(attach_thread_exit)) == 0 {
            G_ATTACH_KEY_OK.store(1, Ordering::SeqCst);
        }
    }
}

unsafe fn attach_record() -> *mut AttachedThread {
    unsafe {
        if !G_ATTACHED.is_null() {
            return G_ATTACHED;
        }
        if G_ATTACH_KEY_OK.load(Ordering::SeqCst) == 0 {
            return ptr::null_mut();
        }
        libc::pthread_getspecific(G_ATTACH_KEY) as *mut AttachedThread
    }
}

unsafe fn attach_cpu_number() -> c_int {
    unsafe {
        static SEQ: AtomicI32 = AtomicI32::new(0);
        static NCPU: AtomicI32 = AtomicI32::new(0);
        let mut n = NCPU.load(Ordering::SeqCst);
        if n == 0 {
            let mut sz = core::mem::size_of::<c_int>();
            if sysctlbyname(
                c"hw.activecpu".as_ptr(),
                &mut n as *mut c_int as *mut c_void,
                &mut sz,
                ptr::null(),
                0,
            ) != 0
                || n < 1
            {
                n = 1;
            }
            NCPU.store(n, Ordering::SeqCst);
        }
        let idx = SEQ.fetch_add(1, Ordering::SeqCst);
        1 + idx % (if n > 1 { n - 1 } else { 1 })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_thread_attach(vm: *mut OcerzVM) -> *mut OcerzCPU {
    unsafe {
        if !G_CUR_CPU.is_null() {
            return G_CUR_CPU;
        }
        if vm.is_null() {
            return ptr::null_mut();
        }
        let mut at = attach_record();
        if !at.is_null() {
            G_ATTACHED = at;
            G_CUR_CPU = &mut (*at).cpu;
            return &mut (*at).cpu;
        }
        let mut tid: u64 = 0;
        pthread_threadid_np(0, &mut tid);
        libc::pthread_once(&raw mut G_ATTACH_ONCE, Some(attach_key_init));
        if G_ATTACH_KEY_OK.load(Ordering::SeqCst) == 0 {
            libc::fprintf(
                stderr(),
                c"ocerz: vm: cannot attach host thread %#llx: no pthread key could be created to tear its guest personality down when it exits\n"
                    .as_ptr(),
                tid as c_ulonglong,
            );
            return ptr::null_mut();
        }
        let region = ffi::ocerz_map_anywhere(OCERZ_ATTACH_REGION, PROT_READ | PROT_WRITE);
        if region == 0 {
            libc::fprintf(
                stderr(),
                c"ocerz: vm: cannot attach host thread %#llx: no room in guest memory for its %#llx-byte guest stack and thread block\n"
                    .as_ptr(),
                tid as c_ulonglong,
                OCERZ_ATTACH_REGION as c_ulonglong,
            );
            return ptr::null_mut();
        }
        at = libc::calloc(1, core::mem::size_of::<AttachedThread>()) as *mut AttachedThread;
        if at.is_null() {
            ffi::ocerz_unmap(region, OCERZ_ATTACH_REGION);
            libc::fprintf(
                stderr(),
                c"ocerz: vm: cannot attach host thread %#llx: no memory for its guest cpu\n"
                    .as_ptr(),
                tid as c_ulonglong,
            );
            return ptr::null_mut();
        }
        (*at).region = region;
        let cpu = &mut (*at).cpu as *mut OcerzCPU;
        (*cpu).vm = vm;
        ffi::ocerz_cpu_reset(cpu);
        (*cpu).cpu_number = attach_cpu_number();
        (*cpu).cur_sys_class = -1;
        let block = region + OCERZ_ATTACH_BLOCK;
        let gs = block + OCERZ_ATTACH_GS;
        ocerz_st(gs, 8, block);
        ocerz_st(gs - 8, 8, tid);
        (*cpu).gs_base = gs;
        (*cpu).gpr[OCERZ_RSP] = block & !0xfu64;
        (*cpu).host_pthread = pthread_self() as *mut c_void;
        (*cpu).host_tsd = host_tsd_self();
        (*cpu).host_kport = pthread_mach_thread_np(pthread_self());
        (*cpu).bridge_depth = ffi::ocerz_bridge_depth_ptr();
        (*cpu).jit_lock_depth = ffi::ocerz_jit_lock_depth_ptr();
        (*cpu).host_tid = tid;
        if libc::pthread_setspecific(G_ATTACH_KEY, at as *const c_void) != 0 {
            ffi::ocerz_unmap(region, OCERZ_ATTACH_REGION);
            libc::free(at as *mut c_void);
            libc::fprintf(
                stderr(),
                c"ocerz: vm: cannot attach host thread %#llx: its guest personality could not be recorded for teardown\n"
                    .as_ptr(),
                tid as c_ulonglong,
            );
            return ptr::null_mut();
        }
        ffi::ocerz_jit_require_ordered(vm);
        ocerz_cpu_register(cpu);
        libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
        if G_ATTACH_STACKS_N < OCERZ_ATTACH_MAX as c_int {
            (*attach_stacks().add(G_ATTACH_STACKS_N as usize)).thread = pthread_self();
            (*attach_stacks().add(G_ATTACH_STACKS_N as usize)).region = region;
            G_ATTACH_STACKS_N += 1;
        }
        libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
        G_ATTACHED = at;
        G_CUR_CPU = cpu;
        cpu
    }
}

static mut G_MAIN_STACK_LO: u64 = 0;
static mut G_MAIN_STACK_HI: u64 = 0;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_set_main_stack(lo: u64, hi: u64) {
    unsafe {
        G_MAIN_STACK_LO = lo;
        G_MAIN_STACK_HI = hi;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_guest_stack(
    vm: *mut OcerzVM,
    host_pthread: *mut c_void,
    lo: *mut u64,
    hi: *mut u64,
) -> c_int {
    unsafe {
        let thread = host_pthread as libc::pthread_t;
        let mut found = 0;
        libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
        let mut i = 0;
        while i < G_ATTACH_STACKS_N && found == 0 {
            if pthread_equal((*attach_stacks().add(i as usize)).thread, thread) != 0 {
                *lo = (*attach_stacks().add(i as usize)).region;
                *hi = (*attach_stacks().add(i as usize)).region + OCERZ_ATTACH_BLOCK;
                found = 1;
            }
            i += 1;
        }
        let mut i = 0;
        while i < G_CPUS_N && found == 0 {
            let main_hi = if G_MAIN_STACK_HI != 0 {
                G_MAIN_STACK_HI
            } else if !vm.is_null() {
                (*vm).stack_hi
            } else {
                0
            };
            if !(*gcpus().add(i as usize)).is_null()
                && main_hi != 0
                && pthread_equal(*cputhreads().add(i as usize), thread) != 0
            {
                *lo = if G_MAIN_STACK_HI != 0 {
                    G_MAIN_STACK_LO
                } else {
                    (*vm).stack_lo
                };
                *hi = main_hi;
                found = 1;
            }
            i += 1;
        }
        libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
        found
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_thread_detach() {
    unsafe {
        let at = attach_record();
        if at.is_null() {
            return;
        }
        if !G_CUR_CPU.is_null() && G_CUR_CPU != &mut (*at).cpu {
            let mut tid: u64 = 0;
            pthread_threadid_np(0, &mut tid);
            libc::fprintf(
                stderr(),
                c"ocerz: vm: ocerz_thread_detach was called on host thread %#llx while guest code is running on it; its guest personality, cpu#%d, stays attached\n"
                    .as_ptr(),
                tid as c_ulonglong,
                (*at).cpu.cpu_number,
            );
            return;
        }
        libc::pthread_setspecific(G_ATTACH_KEY, ptr::null());
        attach_release(at);
    }
}

static UNSTICK_LG: AtomicI32 = AtomicI32::new(-1);
static UNSTICK_KICK_ALL: AtomicI32 = AtomicI32::new(-1);
static UNSTICK_WAUTO: AtomicI32 = AtomicI32::new(-1);
static mut UNSTICK_WBASE: u64 = 0;
static mut UNSTICK_WARNED: [u64; OCERZ_MAX_CPUS] = [0; OCERZ_MAX_CPUS];
static mut UNSTICK_BT_QUIET: [u32; OCERZ_MAX_CPUS] = [0; OCERZ_MAX_CPUS];
static mut UNSTICK_BT_LATCHED: c_int = 0;

extern "C" fn ocerz_unstick_thread(_arg: *mut c_void) -> *mut c_void {
    unsafe {
        if UNSTICK_LG.load(Ordering::Relaxed) < 0 {
            UNSTICK_LG.store(
                (!libc::getenv(c"OCERZ_UNSTICKLOG".as_ptr()).is_null()) as i32,
                Ordering::Relaxed,
            );
        }
        if UNSTICK_KICK_ALL.load(Ordering::Relaxed) < 0 {
            UNSTICK_KICK_ALL.store(
                (!libc::getenv(c"OCERZ_UNSTICK_ALL".as_ptr()).is_null()) as i32,
                Ordering::Relaxed,
            );
        }
        if UNSTICK_WAUTO.load(Ordering::Relaxed) < 0 {
            let w = libc::getenv(c"OCERZ_WATCH".as_ptr());
            let b = libc::getenv(c"OCERZ_MACDRVDUMP".as_ptr());
            UNSTICK_WAUTO.store(
                (!w.is_null() && libc::strcmp(w, c"auto".as_ptr()) == 0 && !b.is_null()) as i32,
                Ordering::Relaxed,
            );
            UNSTICK_WBASE = if !b.is_null() {
                libc::strtoull(b, ptr::null_mut(), 0)
            } else {
                0
            };
        }
        loop {
            let ts = libc::timespec {
                tv_sec: 0,
                tv_nsec: 250 * 1000 * 1000,
            };
            libc::nanosleep(&ts, ptr::null_mut());
            if UNSTICK_WAUTO.load(Ordering::Relaxed) == 1 {
                let slot = UNSTICK_WBASE + 0x560f0;
                if ocerz_addr_readable(slot) != 0 {
                    let ctrl = ocerz_ld(slot, 8);
                    if ctrl != 0 && ocerz_addr_readable(ctrl + 0x10) != 0 {
                        let src = ocerz_ld(ctrl + 0x10, 8);
                        if src != 0 && ocerz_addr_readable(src + 0x30) != 0 {
                            ocerz_watch_addr = src + 0x10;
                            ocerz_watch_len = 0x20;
                            libc::fprintf(
                                stderr(),
                                c"ocerz: WATCH-AUTO[%d] resolved %#llx\n".as_ptr(),
                                libc::getpid(),
                                ocerz_watch_addr as c_ulonglong,
                            );
                            UNSTICK_WAUTO.store(2, Ordering::Relaxed);
                        }
                    }
                }
            }
            let now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
            for i in 0..G_CPUS_N {
                let t0 = (**gcpus().add(i as usize)).block_since_ns;
                let sigonly = (**gcpus().add(i as usize)).block_sigonly;
                let due = if sigonly != 0 {
                    ((**gcpus().add(i as usize)).sig_pending
                        & !(**gcpus().add(i as usize)).sig_mask)
                        != 0
                        && now.wrapping_sub(t0) > 50 * 1000 * 1000
                } else {
                    now.wrapping_sub(t0) > 800 * 1000 * 1000
                        && (UNSTICK_KICK_ALL.load(Ordering::Relaxed) != 0
                            || (**gcpus().add(i as usize)).block_nokick == 0)
                };
                if t0 != 0 && due {
                    (**gcpus().add(i as usize)).block_since_ns = now;
                    if UNSTICK_LG.load(Ordering::Relaxed) != 0 {
                        libc::fprintf(
                            stderr(),
                            c"ocerz: UNSTICK[%d] kicking cpu#%u (blocked %llums) what=%d rip=%#llx\n"
                                .as_ptr(),
                            libc::getpid(),
                            (**gcpus().add(i as usize)).cpu_number,
                            ((now - t0) / 1000000) as c_ulonglong,
                            (**gcpus().add(i as usize)).block_what,
                            (**gcpus().add(i as usize)).rip as c_ulonglong,
                        );
                    }
                    libc::pthread_kill(*cputhreads().add(i as usize), SIGEMT);
                }
                let bs = (**gcpus().add(i as usize)).block_started_ns;
                if bs != 0
                    && now > bs
                    && now - bs > 5000000000
                    && *((&raw mut UNSTICK_WARNED) as *mut u64).add(i as usize) != bs
                {
                    *((&raw mut UNSTICK_WARNED) as *mut u64).add(i as usize) = bs;
                    libc::fprintf(
                        stderr(),
                        c"ocerz: BLOCKED[%d] cpu#%u trap=%d for %llus rip=%#llx a0=%#llx a1=%#llx a2=%#llx"
                            .as_ptr(),
                        libc::getpid(),
                        (**gcpus().add(i as usize)).cpu_number,
                        (**gcpus().add(i as usize)).block_what,
                        ((now - bs) / 1000000000) as c_ulonglong,
                        (**gcpus().add(i as usize)).rip as c_ulonglong,
                        (**gcpus().add(i as usize)).gpr[OCERZ_RDI] as c_ulonglong,
                        (**gcpus().add(i as usize)).gpr[OCERZ_RSI] as c_ulonglong,
                        (**gcpus().add(i as usize)).gpr[OCERZ_RDX] as c_ulonglong,
                    );
                    if !libc::getenv(c"OCERZ_BLOCKBT".as_ptr()).is_null() {
                        let sp = (**gcpus().add(i as usize)).gpr[OCERZ_RSP];
                        let mut fp = (**gcpus().add(i as usize)).gpr[5];
                        if sp != 0 && (sp & 7) == 0 && ocerz_addr_readable(sp) != 0 {
                            libc::fprintf(
                                stderr(),
                                c" bt=%#llx".as_ptr(),
                                ocerz_ld(sp, 8) as c_ulonglong,
                            );
                        }
                        let mut fj = 0;
                        while fj < 8
                            && fp != 0
                            && (fp & 7) == 0
                            && ocerz_addr_readable(fp) != 0
                            && ocerz_addr_readable(fp + 8) != 0
                        {
                            libc::fprintf(
                                stderr(),
                                c",%#llx".as_ptr(),
                                ocerz_ld(fp + 8, 8) as c_ulonglong,
                            );
                            fp = ocerz_ld(fp, 8);
                            fj += 1;
                        }
                    }
                    libc::fputc('\n' as c_int, stderr());
                }
            }
            {
                if UNSTICK_BT_LATCHED == 0
                    && G_CPUS_N > 0
                    && !(**gcpus().add(0)).btrace.is_null()
                    && AtomicI32::from_ptr(&raw mut G_BTRACE_REQ).load(Ordering::Acquire) != 0
                {
                    UNSTICK_BT_LATCHED = 1;
                    for i in 0..G_CPUS_N {
                        AtomicU32::from_ptr(&raw mut (**gcpus().add(i as usize)).btrace_mask)
                            .store(0, Ordering::Release);
                    }
                    let e = libc::getenv(c"OCERZ_BTRACE_DEPTH".as_ptr());
                    let depth = if !e.is_null() {
                        libc::strtoul(e, ptr::null_mut(), 0) as u32
                    } else {
                        400
                    };
                    for i in 0..G_CPUS_N {
                        let c = *gcpus().add(i as usize);
                        if (*c).btrace.is_null() {
                            continue;
                        }
                        let bn = (*c).btrace_n;
                        let m = (1u32 << 16) - 1;
                        libc::fprintf(
                            stderr(),
                            c"ocerz: BTRACE[%d] cpu#%u tid=%#llx quiet=%u n=%u\n".as_ptr(),
                            libc::getpid(),
                            (*c).cpu_number,
                            (if ocerz_addr_readable((*c).gs_base + 0x18) != 0 {
                                ocerz_ld((*c).gs_base + 0x18, 8)
                            } else {
                                0
                            }) as c_ulonglong,
                            *((&raw mut UNSTICK_BT_QUIET) as *mut u32).add(i as usize),
                            bn,
                        );
                        let mut k = 1u32;
                        while k <= depth && k <= bn {
                            libc::fprintf(
                                stderr(),
                                c"  %u %#llx\n".as_ptr(),
                                k,
                                *(*c).btrace.add((bn.wrapping_sub(k) & m) as usize) as c_ulonglong,
                            );
                            k += 1;
                        }
                    }
                    libc::fflush(stderr());
                }
            }
            libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_unstick_start() {
    unsafe {
        if !libc::getenv(c"OCERZ_NO_UNSTICK".as_ptr()).is_null() {
            return;
        }
        if G_UNSTICK_STARTED
            .compare_exchange(0, 1, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return;
        }
        let mut t: libc::pthread_t = 0;
        if libc::pthread_create(&mut t, ptr::null(), ocerz_unstick_thread, ptr::null_mut()) == 0 {
            libc::pthread_detach(t);
        } else {
            G_UNSTICK_STARTED.store(0, Ordering::Release);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_request_exit(vm: *mut OcerzVM, code: c_int) {
    unsafe {
        (*vm).exit_code = code;
        AtomicI32::from_ptr(&raw mut (*vm).exited).store(1, Ordering::SeqCst);
        ffi::ocerz_jit_request_stop(vm);
        let self_ = pthread_self();
        libc::pthread_mutex_lock(&raw mut G_CPUS_LOCK);
        for i in 0..G_CPUS_N {
            AtomicI32::from_ptr(&raw mut (**gcpus().add(i as usize)).interrupt)
                .store(1, Ordering::SeqCst);
        }
        for i in 0..G_CPUS_N {
            if pthread_equal(*cputhreads().add(i as usize), self_) == 0 {
                libc::pthread_kill(*cputhreads().add(i as usize), SIGEMT);
            }
        }
        libc::pthread_mutex_unlock(&raw mut G_CPUS_LOCK);
    }
}

unsafe fn vm_fatal_where(cpu: *const OcerzCPU) {
    unsafe {
        let mut at = [0u64; 13];
        let mut n = 0usize;
        *at.as_mut_ptr().add(n) = (*cpu).rip;
        n += 1;
        let mut fp = (*cpu).gpr[OCERZ_RBP];
        while n < 13 && fp >= 0x300000000 {
            *at.as_mut_ptr().add(n) = ocerz_ld(fp + 8, 8);
            n += 1;
            let nf = ocerz_ld(fp, 8);
            if nf <= fp {
                break;
            }
            fp = nf;
        }
        libc::fprintf(stderr(), c"ocerz: where:".as_ptr());
        for k in 0..n {
            let mut base: u64 = 0;
            let name = ocerz_dyld_name_for_addr(*at.as_mut_ptr().add(k), &mut base);
            let leaf = if !name.is_null() {
                libc::strrchr(name, '/' as c_int)
            } else {
                ptr::null()
            };
            if !name.is_null() {
                libc::fprintf(
                    stderr(),
                    c" %s+%#llx".as_ptr(),
                    if !leaf.is_null() { leaf.add(1) } else { name },
                    (*at.as_mut_ptr().add(k) - base) as c_ulonglong,
                );
            } else {
                libc::fprintf(stderr(), c" ?".as_ptr());
            }
        }
        libc::fprintf(stderr(), c"\n".as_ptr());
    }
}

static mut TRACE_INIT: c_int = 0;
static mut TRACE_LO: u64 = 0;
static mut TRACE_HI: u64 = 0;
static mut TRACE_PEEK: c_long = -2;

#[repr(C)]
struct RunCtx {
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    prev_cpu: *mut OcerzCPU,
    prev_recover: *mut sigjmp_buf,
    jb: sigjmp_buf,
    jmark: u64,
    esc_r: c_int,
    result: c_int,
}

unsafe extern "C" fn run_cpu_body(ctx_: *mut c_void, rc: c_int) -> c_int {
    unsafe {
        let ctx = ctx_ as *mut RunCtx;
        let c = &mut *ctx;
        match rc {
            0 => c.esc_r = 0,
            2 => c.esc_r = T_JIT_ESCAPE_R,
            _ => {
                c.esc_r = 0;
                if env_cache!("OCERZ_CPUREG_LOG") != 0 {
                    libc::fprintf(
                        stderr(),
                        c"ocerz: CPUREG recovery #%u (old code would leak a dangling entry here)\n"
                            .as_ptr(),
                        CPUREG_RECOV2.fetch_add(1, Ordering::SeqCst).wrapping_add(1),
                    );
                }
            }
        }
        T_JIT_ESCAPE_R = 0;
        ffi::ocerz_jit_thread_restore(c.jmark);
        let cpu = c.cpu;
        let vm = c.vm;
        G_CUR_CPU = cpu;
        (*cpu).host_pthread = pthread_self() as *mut c_void;
        (*cpu).host_tsd = host_tsd_self();
        (*cpu).host_kport = pthread_mach_thread_np(pthread_self());
        (*cpu).bridge_depth = ffi::ocerz_bridge_depth_ptr();
        (*cpu).jit_lock_depth = ffi::ocerz_jit_lock_depth_ptr();
        ffi::ocerz_apply_mxcsr_round((*cpu).mxcsr);

        'outer: while (*vm).exited == 0 && (*cpu).terminated == 0 && (*cpu).interrupt == 0 {
            ocerz_vm_suspend_point(cpu);
            G_RIPHIST[(G_RIPHIST_N & 31) as usize] = (*cpu).rip;
            G_RIPHIST_N = G_RIPHIST_N.wrapping_add(1);
            let mut r: c_int;
            if ocerz_exc_trap != 0 && (*cpu).rip == ocerz_exc_trap {
                ocerz_exc_report(cpu);
            }
            if ocerz_arg_trap != 0 && (*cpu).rip == ocerz_arg_trap {
                arg_trap_report(cpu);
            }
            if ocerz_sel_trap != 0 && (*cpu).rip == ocerz_sel_trap {
                sel_trap_report(cpu);
            }
            if ocerz_ctx_trap != 0 && (*cpu).rip == ocerz_ctx_trap {
                ctx_trap_report(cpu);
            }
            if ocerz_bt_lo != 0 && (*cpu).rip >= ocerz_bt_lo && (*cpu).rip < ocerz_bt_hi {
                ocerz_bt_report(cpu);
            }
            if TRACE_LO != 0 && (*cpu).rip >= TRACE_LO && (*cpu).rip < TRACE_HI {
                {
                    if TRACE_PEEK == -2 {
                        let e = libc::getenv(c"OCERZ_TRACE_PEEK".as_ptr());
                        TRACE_PEEK = if !e.is_null() {
                            libc::strtol(e, ptr::null_mut(), 0)
                        } else {
                            -1
                        };
                    }
                    let peek = TRACE_PEEK;
                    if peek >= 0 {
                        let r13 = (*cpu).gpr[OCERZ_R13];
                        let rbp = (*cpu).gpr[OCERZ_RBP];
                        let rsp = (*cpu).gpr[OCERZ_RSP];
                        libc::fprintf(
                            stderr(),
                            c"WP [rsp]=%#llx [rsp+8]=%#llx [rsp+16]=%#llx r13[%#lx]=%#llx r13[%#lx]=%#llx [rbp+8]=%#llx rbx=%#llx\n"
                                .as_ptr(),
                            if ocerz_addr_readable(rsp) != 0 { ocerz_ld(rsp, 8) } else { 0 }
                                as c_ulonglong,
                            if ocerz_addr_readable(rsp + 8) != 0 {
                                ocerz_ld(rsp + 8, 8)
                            } else {
                                0
                            } as c_ulonglong,
                            if ocerz_addr_readable(rsp + 16) != 0 {
                                ocerz_ld(rsp + 16, 8)
                            } else {
                                0
                            } as c_ulonglong,
                            peek as c_ulonglong,
                            if ocerz_addr_readable(r13.wrapping_add(peek as u64)) != 0 {
                                ocerz_ld(r13.wrapping_add(peek as u64), 8)
                            } else {
                                0
                            } as c_ulonglong,
                            (peek + 8) as c_ulonglong,
                            if ocerz_addr_readable(r13.wrapping_add(peek as u64 + 8)) != 0 {
                                ocerz_ld(r13.wrapping_add(peek as u64 + 8), 8)
                            } else {
                                0
                            } as c_ulonglong,
                            if ocerz_addr_readable(rbp + 8) != 0 {
                                ocerz_ld(rbp + 8, 8)
                            } else {
                                0
                            } as c_ulonglong,
                            (*cpu).gpr[OCERZ_RBX] as c_ulonglong,
                        );
                    }
                }
                libc::fprintf(
                    stderr(),
                    c"WT %#llx rax=%#llx rcx=%#llx rdi=%#llx rsi=%#llx r8=%#llx r12=%#llx rsp=%#llx rbp=%#llx r13=%#llx r15=%#llx\n"
                        .as_ptr(),
                    (*cpu).rip as c_ulonglong,
                    (*cpu).gpr[OCERZ_RAX] as c_ulonglong,
                    (*cpu).gpr[OCERZ_RCX] as c_ulonglong,
                    (*cpu).gpr[OCERZ_RDI] as c_ulonglong,
                    (*cpu).gpr[OCERZ_RSI] as c_ulonglong,
                    (*cpu).gpr[OCERZ_R8] as c_ulonglong,
                    (*cpu).gpr[OCERZ_R12] as c_ulonglong,
                    (*cpu).gpr[OCERZ_RSP] as c_ulonglong,
                    (*cpu).gpr[OCERZ_RBP] as c_ulonglong,
                    (*cpu).gpr[OCERZ_R13] as c_ulonglong,
                    (*cpu).gpr[OCERZ_R15] as c_ulonglong,
                );
                r = ffi::ocerz_interp_step(vm, cpu);
                if r == ffi::OCERZ_STEP_EXIT as c_int {
                    break;
                }
                if r == ffi::OCERZ_STEP_FATAL as c_int {
                    run_fatal(c);
                    break 'outer;
                }
                continue;
            }
            if c.esc_r != 0 {
                r = c.esc_r;
                c.esc_r = 0;
                if r == ffi::OCERZ_EUNSUP as c_int {
                    r = ffi::ocerz_interp_step(vm, cpu);
                }
            } else if (*cpu).interp_once != 0 {
                (*cpu).interp_once = 0;
                r = ffi::ocerz_interp_step(vm, cpu);
            } else if (*vm).jit_enabled != 0
                && (!(*vm).jit.is_null() || {
                    (*vm).jit = ffi::ocerz_jit_create(vm);
                    !(*vm).jit.is_null()
                })
            {
                r = ffi::ocerz_jit_step(vm, cpu);
                if r == ffi::OCERZ_EUNSUP as c_int {
                    r = ffi::ocerz_interp_step(vm, cpu);
                }
            } else {
                r = ffi::ocerz_interp_step(vm, cpu);
            }
            if r == ffi::OCERZ_STEP_EXIT as c_int {
                break;
            }
            if r == ffi::OCERZ_STEP_FATAL as c_int {
                run_fatal(c);
                break 'outer;
            }
        }
        c.result
    }
}

unsafe fn run_fatal(c: *mut RunCtx) {
    unsafe {
        let cpu = (*c).cpu;
        let vm = (*c).vm;
        ffi::ocerz_cpu_dump(cpu, stderr() as *mut ffi::FILE);
        let mut fp = (*cpu).gpr[OCERZ_RBP];
        libc::fprintf(stderr(), c"ocerz: rbp-chain:".as_ptr());
        let mut d = 0;
        while d < 40 && fp >= 0x300000000 {
            libc::fprintf(
                stderr(),
                c" %#llx".as_ptr(),
                ocerz_ld(fp + 8, 8) as c_ulonglong,
            );
            let nf = ocerz_ld(fp, 8);
            if nf <= fp {
                break;
            }
            fp = nf;
            d += 1;
        }
        libc::fprintf(stderr(), c"\n".as_ptr());
        vm_fatal_where(cpu);
        libc::fprintf(
            stderr(),
            c"ocerz: %llu instructions executed\n".as_ptr(),
            (*vm).insn_count as c_ulonglong,
        );
        if (*cpu).mode32 != 0 {
            libc::exit(125);
        }
        G_SIG_RECOVER = (*c).prev_recover;
        G_CUR_CPU = (*c).prev_cpu;
        ocerz_cpu_unregister(cpu);
        (*c).result = 125;
        return;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_run_cpu(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        if AtomicI32::from_ptr(&raw mut TRACE_INIT).load(Ordering::Acquire) == 0 {
            let tlo = libc::getenv(c"OCERZ_TRACE_LO".as_ptr());
            let thi = libc::getenv(c"OCERZ_TRACE_HI".as_ptr());
            TRACE_LO = if !tlo.is_null() {
                libc::strtoull(tlo, ptr::null_mut(), 0)
            } else {
                0
            };
            TRACE_HI = if !thi.is_null() {
                libc::strtoull(thi, ptr::null_mut(), 0)
            } else {
                0
            };
            AtomicI32::from_ptr(&raw mut TRACE_INIT).store(1, Ordering::Release);
        }
        let mut c = RunCtx {
            vm,
            cpu,
            prev_cpu: G_CUR_CPU,
            prev_recover: G_SIG_RECOVER,
            jb: [0; 49],
            jmark: 0,
            esc_r: 0,
            result: 0,
        };
        G_SIG_RECOVER = &mut c.jb;
        ocerz_cpu_register(cpu);
        pthread_threadid_np(0, &mut (*cpu).host_tid);
        (*cpu).cur_sys_class = -1;
        ocerz_host_sigmask_clear(c"run_cpu".as_ptr());
        c.jmark = ffi::ocerz_jit_thread_mark();
        ocerz_vm_setjmp_run(
            &mut c.jb,
            1,
            run_cpu_body,
            &mut c as *mut RunCtx as *mut c_void,
        );
        G_SIG_RECOVER = c.prev_recover;
        G_CUR_CPU = c.prev_cpu;
        ocerz_cpu_unregister(cpu);
        if c.result != 0 {
            return c.result;
        }
        (*vm).exit_code
    }
}

extern "C" fn ocerz_test_async_stop(p: *mut c_void) -> *mut c_void {
    unsafe {
        let vm = p as *mut OcerzVM;
        let icn = libc::getenv(c"OCERZ_TEST_ASYNC_STOP_ICOUNT".as_ptr());
        let thresh = if !icn.is_null() {
            libc::strtoull(icn, ptr::null_mut(), 0)
        } else {
            500000
        };
        while AtomicI32::from_ptr(&raw mut (*vm).exited).load(Ordering::Relaxed) == 0
            && AtomicU64::from_ptr(&raw mut (*vm).insn_count).load(Ordering::Relaxed) < thresh
        {
            libc::sched_yield();
        }
        ocerz_vm_request_exit(vm, 0);
        ptr::null_mut()
    }
}

extern "C" fn ocerz_test_async_stop_ms(p: *mut c_void) -> *mut c_void {
    unsafe {
        let vm = p as *mut OcerzVM;
        let msn = libc::getenv(c"OCERZ_TEST_ASYNC_STOP_MS".as_ptr());
        let mut ms = if !msn.is_null() {
            libc::strtol(msn, ptr::null_mut(), 0)
        } else {
            200
        };
        if ms < 0 {
            ms = 0;
        }
        let mut req = libc::timespec {
            tv_sec: ms / 1000,
            tv_nsec: (ms % 1000) * 1000000,
        };
        let mut rem: libc::timespec = core::mem::zeroed();
        while libc::nanosleep(&req, &mut rem) != 0 && *libc::__error() == libc::EINTR {
            req = rem;
        }
        ocerz_vm_request_exit(vm, 0);
        ptr::null_mut()
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_vm_run(vm: *mut OcerzVM) -> c_int {
    unsafe {
        ocerz_vm_install_handlers(vm);
        if !libc::getenv(c"OCERZ_TEST_ASYNC_STOP_ICOUNT".as_ptr()).is_null() {
            let mut th: libc::pthread_t = 0;
            let mut at: libc::pthread_attr_t = core::mem::zeroed();
            libc::pthread_attr_init(&mut at);
            libc::pthread_attr_setdetachstate(&mut at, libc::PTHREAD_CREATE_DETACHED);
            libc::pthread_create(&mut th, &at, ocerz_test_async_stop, vm as *mut c_void);
            libc::pthread_attr_destroy(&mut at);
        } else if !libc::getenv(c"OCERZ_TEST_ASYNC_STOP_MS".as_ptr()).is_null() {
            let mut th: libc::pthread_t = 0;
            let mut at: libc::pthread_attr_t = core::mem::zeroed();
            libc::pthread_attr_init(&mut at);
            libc::pthread_attr_setdetachstate(&mut at, libc::PTHREAD_CREATE_DETACHED);
            libc::pthread_create(&mut th, &at, ocerz_test_async_stop_ms, vm as *mut c_void);
            libc::pthread_attr_destroy(&mut at);
        }
        let rc = ocerz_vm_run_cpu(vm, &mut (*vm).cpu);
        ocerz_log!(
            "guest exited with code %d after %llu instructions\n",
            (*vm).exit_code,
            (*vm).insn_count as c_ulonglong
        );
        if !(*vm).jit.is_null() {
            ocerz_log!(
                "jit translated %llu blocks\n",
                ffi::ocerz_jit_blocks((*vm).jit) as c_ulonglong
            );
        }
        rc
    }
}

static CPUREG_RECOV2: AtomicU32 = AtomicU32::new(0);

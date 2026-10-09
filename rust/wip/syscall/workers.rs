//! Guest workqueue workers, workloop serialization, and fork coordination.

use super::util::*;
use super::*;

use core::ffi::{c_int, c_void};
use core::ptr;

const OCERZ_WORKLOOP_COUNT: usize = 128;
const OCERZ_WORKER_STACK_SIZE: usize = 16 * 1024 * 1024;

#[repr(C)]
pub(super) struct OcerzWorkerPub {
    pub(super) m: libc::pthread_mutex_t,
    pub(super) c: libc::pthread_cond_t,
    pub(super) published: c_int,
}

#[repr(C)]
pub(super) struct OcerzWorker {
    pub(super) vm: *mut OcerzVM,
    pub(super) cpu: OcerzCPU,
    pub(super) counts_wq: c_int,
    pub(super) pub_: *mut OcerzWorkerPub,
}

#[thread_local]
pub(super) static mut g_hostwq_tl_events: *mut *mut c_void = core::ptr::null_mut();
#[thread_local]
pub(super) static mut g_hostwq_tl_nevents: *mut c_int = core::ptr::null_mut();
#[thread_local]
pub(super) static mut g_hostwq_tl_evcap: c_int = 0;

static G_WQ_RUNNING: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(0);
pub(super) static mut g_active_wl: [u64; OCERZ_WORKLOOP_COUNT] = [0; OCERZ_WORKLOOP_COUNT];
static mut G_WL_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
static G_CPU_COUNT: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(0);
static G_CPU_SEQ: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(0);
static mut G_FORK_ATFORK_ONCE: libc::pthread_once_t = libc::PTHREAD_ONCE_INIT;
static G_FORK_ATFORK_ERROR: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(0);

unsafe extern "C" {
    fn mach_thread_self() -> libc::mach_port_t;
    fn mach_port_deallocate(task: libc::mach_port_t, name: libc::mach_port_t) -> c_int;
    static mach_task_self_: libc::mach_port_t;
    fn ocerz_vm_atfork_prepare();
    fn ocerz_vm_atfork_parent();
    fn ocerz_vm_atfork_child();
    fn ocerz_jit_prefork();
    fn ocerz_jit_postfork();
    fn ocerz_jit_postfork_child();
    fn ocerz_mem_postfork();
    fn ocerz_mem_pin_refresh();
    fn ocerz_cache_prefork();
    fn ocerz_cache_postfork();
    fn ocerz_bridge_postfork_child();
    fn ocerz_tcache_child();
    fn ocerz_abi_postfork_child();
    fn ocerz_apidb_postfork_child();
    fn ocerz_vdylib_postfork_child();
}

pub(super) unsafe fn g_wq_running_load() -> c_int {
    G_WQ_RUNNING.load(core::sync::atomic::Ordering::SeqCst)
}

pub(super) unsafe fn g_wq_running_add(delta: c_int) -> c_int {
    G_WQ_RUNNING.fetch_add(delta, core::sync::atomic::Ordering::SeqCst)
}

pub(super) unsafe fn ocerz_kev_stride() -> u64 {
    static STRIDE_40: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
    let mut v = STRIDE_40.load(core::sync::atomic::Ordering::Relaxed);
    if v < 0 {
        v = c_int::from(!libc::getenv(c"OCERZ_STRIDE40".as_ptr()).is_null());
        STRIDE_40.store(v, core::sync::atomic::Ordering::Relaxed);
    }
    if v != 0 { 0x40 } else { 0x48 }
}

pub(super) unsafe fn wl_dqdump(tag: *const i8, id: u64, cpun: u32, kp: u32) {
    unsafe {
        if id == 0 {
            return;
        }
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: DQ %-6s id=%#llx cpu#%u kport=%#x |".as_ptr(),
            tag,
            id as libc::c_ulonglong,
            cpun,
            kp,
        );
        let mut off = 0u64;
        while off <= 0x48 {
            libc::fprintf(
                crate::log::stderr(),
                c" +%02llx=%016llx".as_ptr(),
                off as libc::c_ulonglong,
                ocerz_ld(id.wrapping_add(off), 8) as libc::c_ulonglong,
            );
            off += 8;
        }
        libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
    }
}

pub(super) unsafe fn cpu_fresh_thread_state(c: *mut OcerzCPU) {
    unsafe {
        (*c).suspend_count = 0;
        (*c).susp_parked = 0;
        (*c).susp_host = 0;
        (*c).susp_have_gpr = 0;
        (*c).block_since_ns = 0;
        (*c).block_started_ns = 0;
        (*c).block_nokick = 0;
        (*c).unix_gs_base = 0;
    }
}

pub(super) unsafe fn wl_try_acquire(id: u64) -> c_int {
    unsafe {
        if id == 0 {
            return 0;
        }
        libc::pthread_mutex_lock(ptr::addr_of_mut!(G_WL_LOCK));
        let active = ptr::addr_of_mut!(g_active_wl).cast::<u64>();
        let mut slot = -1;
        for i in 0..OCERZ_WORKLOOP_COUNT {
            let value = *active.add(i);
            if value == id {
                libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_WL_LOCK));
                return 0;
            }
            if value == 0 && slot < 0 {
                slot = i as c_int;
            }
        }
        if slot < 0 {
            libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_WL_LOCK));
            return 0;
        }
        *active.add(slot as usize) = id;
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_WL_LOCK));
        1
    }
}

pub(super) unsafe fn wl_release(id: u64) {
    unsafe {
        if id == 0 {
            return;
        }
        libc::pthread_mutex_lock(ptr::addr_of_mut!(G_WL_LOCK));
        let active = ptr::addr_of_mut!(g_active_wl).cast::<u64>();
        for i in 0..OCERZ_WORKLOOP_COUNT {
            if *active.add(i) == id {
                *active.add(i) = 0;
                break;
            }
        }
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_WL_LOCK));
    }
}

unsafe fn ocerz_cpu_count() -> c_int {
    let current = G_CPU_COUNT.load(core::sync::atomic::Ordering::Relaxed);
    if current != 0 {
        return current;
    }
    unsafe {
        let mut sz = core::mem::size_of::<c_int>();
        let mut v = 0;
        if libc::sysctlbyname(
            c"hw.activecpu".as_ptr(),
            (&mut v as *mut c_int).cast::<c_void>(),
            &mut sz,
            ptr::null_mut(),
            0,
        ) != 0
            || v < 1
        {
            v = 1;
        }
        G_CPU_COUNT.store(v, core::sync::atomic::Ordering::Relaxed);
        v
    }
}

pub(super) unsafe fn ocerz_next_cpu_number() -> c_int {
    let idx = G_CPU_SEQ.fetch_add(1, core::sync::atomic::Ordering::SeqCst);
    let ncpu = ocerz_cpu_count();
    1 + (idx % if ncpu > 1 { ncpu - 1 } else { 1 })
}

pub(super) extern "C" fn ocerz_worker_entry(p: *mut c_void) -> *mut c_void {
    unsafe {
        let w = p.cast::<OcerzWorker>();
        crate::ffi::ocerz_init_gate_wait();
        let kp = mach_thread_self();
        (*w).cpu.gpr[crate::ffi::OCERZ_RSI as usize] = kp as u64;
        ocerz_st(
            (*w).cpu.gpr[crate::ffi::OCERZ_RDI as usize].wrapping_add(0xf8),
            4,
            kp as u32 as u64,
        );
        if !(*w).pub_.is_null() {
            let pub_ = (*w).pub_;
            (*w).pub_ = ptr::null_mut();
            libc::pthread_mutex_lock(ptr::addr_of_mut!((*pub_).m));
            (*pub_).published = 1;
            libc::pthread_cond_signal(ptr::addr_of_mut!((*pub_).c));
            libc::pthread_mutex_unlock(ptr::addr_of_mut!((*pub_).m));
        }
        if !libc::getenv(c"OCERZ_THREADLOG".as_ptr()).is_null() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: THREADSTART[%d] cpu#%u rip=%#llx pth=%#llx kp=%#x\n".as_ptr(),
                libc::getpid(),
                (*w).cpu.cpu_number,
                (*w).cpu.rip as libc::c_ulonglong,
                (*w).cpu.gpr[crate::ffi::OCERZ_RDI as usize] as libc::c_ulonglong,
                kp,
            );
        }
        let mut wrc = crate::ffi::ocerz_vm_run_cpu((*w).vm, &mut (*w).cpu);
        if !libc::getenv(c"OCERZ_THREADLOG".as_ptr()).is_null() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: THREADDONE[%d] cpu#%u rip=%#llx rc=%d\n".as_ptr(),
                libc::getpid(),
                (*w).cpu.cpu_number,
                (*w).cpu.rip as libc::c_ulonglong,
                wrc,
            );
        }
        let cmdline = ptr::addr_of!(crate::ported::globals::ocerz_cmdline_summary).cast::<c_char>();
        if wrc == 125 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: fatal on guest thread cpu#%u; exiting process %d \"%s\"\n".as_ptr(),
                (*w).cpu.cpu_number,
                libc::getpid(),
                cmdline,
            );
            libc::exit(125);
        }
        if (*w).counts_wq != 0 && (*w).cpu.wq_returned != 0 && (*(*w).vm).exited == 0 {
            wrc = hostwq::ocerz_wq_run_exit(
                (*w).vm,
                &mut (*w).cpu,
                (*w).cpu.gs_base.wrapping_sub(0xe0),
                kp,
            );
            if wrc == 125 {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: fatal on guest thread cpu#%u; exiting process %d \"%s\"\n".as_ptr(),
                    (*w).cpu.cpu_number,
                    libc::getpid(),
                    cmdline,
                );
                libc::exit(125);
            }
        }
        mach_port_deallocate(mach_task_self_, kp);
        if (*w).counts_wq != 0 {
            wl_release((*w).cpu.wq_workloop_id);
            G_WQ_RUNNING.fetch_sub(1, core::sync::atomic::Ordering::SeqCst);
        }
        libc::free(w.cast());
        ptr::null_mut()
    }
}

pub(super) unsafe fn ocerz_spawn_worker(vm: *mut OcerzVM, tmpl: *const OcerzCPU) -> c_int {
    unsafe {
        crate::ffi::ocerz_jit_require_ordered(vm);
        let w = libc::calloc(1, core::mem::size_of::<OcerzWorker>()).cast::<OcerzWorker>();
        if w.is_null() {
            return -1;
        }
        (*w).vm = vm;
        ptr::copy_nonoverlapping(tmpl, ptr::addr_of_mut!((*w).cpu), 1);
        cpu_fresh_thread_state(ptr::addr_of_mut!((*w).cpu));
        (*w).counts_wq = 1;
        (*w).pub_ = ptr::null_mut();
        (*w).cpu.ras_top = 0;
        ptr::write_bytes(
            ptr::addr_of_mut!((*w).cpu.ras).cast::<u8>(),
            0,
            core::mem::size_of_val(&(*w).cpu.ras),
        );
        let mut attr = core::mem::MaybeUninit::<libc::pthread_attr_t>::uninit();
        libc::pthread_attr_init(attr.as_mut_ptr());
        let mut attr = attr.assume_init();
        libc::pthread_attr_setstacksize(&mut attr, OCERZ_WORKER_STACK_SIZE);
        libc::pthread_attr_setdetachstate(&mut attr, libc::PTHREAD_CREATE_DETACHED);
        let mut th = core::mem::MaybeUninit::<libc::pthread_t>::uninit();
        let rc = libc::pthread_create(th.as_mut_ptr(), &attr, ocerz_worker_entry, w.cast());
        libc::pthread_attr_destroy(&mut attr);
        if rc != 0 {
            libc::free(w.cast());
            return -1;
        }
        crate::ffi::ocerz_unstick_start();
        0
    }
}

unsafe fn ocerz_fork_prepare() {
    unsafe {
        crate::ffi::ocerz_init_gate_prefork();
        ocerz_vm_atfork_prepare();
        libc::pthread_mutex_lock(ptr::addr_of_mut!(G_WL_LOCK));
        ocerz_jit_prefork();
        crate::ffi::ocerz_mem_prefork();
        ocerz_cache_prefork();
    }
}

unsafe fn ocerz_fork_parent() {
    unsafe {
        ocerz_mem_postfork();
        ocerz_jit_postfork();
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_WL_LOCK));
        ocerz_vm_atfork_parent();
        crate::ffi::ocerz_init_gate_postfork_parent();
        ocerz_cache_postfork();
    }
}

unsafe fn ocerz_fork_child() {
    unsafe {
        ocerz_mem_pin_refresh();
        ocerz_mem_postfork();
        ocerz_jit_postfork();
        ocerz_jit_postfork_child();
        ocerz_bridge_postfork_child();
        ocerz_tcache_child();
        ocerz_abi_postfork_child();
        ocerz_apidb_postfork_child();
        ocerz_vdylib_postfork_child();
        ptr::write_bytes(ptr::addr_of_mut!(g_active_wl).cast::<u8>(), 0, 128 * 8);
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_WL_LOCK));
        ocerz_vm_atfork_child();
        G_WQ_RUNNING.store(0, core::sync::atomic::Ordering::SeqCst);
        crate::ffi::ocerz_init_gate_postfork_child();
        ocerz_cache_postfork();
    }
}

unsafe extern "C" fn ocerz_fork_prepare_trampoline() {
    ocerz_fork_prepare();
}

unsafe extern "C" fn ocerz_fork_parent_trampoline() {
    ocerz_fork_parent();
}

unsafe extern "C" fn ocerz_fork_child_trampoline() {
    ocerz_fork_child();
}

unsafe extern "C" fn ocerz_fork_register_atfork() {
    let err = unsafe {
        libc::pthread_atfork(
            Some(ocerz_fork_prepare_trampoline),
            Some(ocerz_fork_parent_trampoline),
            Some(ocerz_fork_child_trampoline),
        )
    };
    G_FORK_ATFORK_ERROR.store(err, core::sync::atomic::Ordering::Relaxed);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_fork_register() {
    unsafe {
        libc::pthread_once(
            ptr::addr_of_mut!(G_FORK_ATFORK_ONCE),
            Some(ocerz_fork_register_atfork),
        );
    }
}

unsafe fn guest_fork_apply(vm: *mut OcerzVM, out: *mut libc::pid_t) -> c_int {
    unsafe {
        crate::ffi::ocerz_jit_require_ordered(vm);
        let once_error = libc::pthread_once(
            ptr::addr_of_mut!(G_FORK_ATFORK_ONCE),
            Some(ocerz_fork_register_atfork),
        );
        let atfork_error = G_FORK_ATFORK_ERROR.load(core::sync::atomic::Ordering::Relaxed);
        if once_error != 0 || atfork_error != 0 {
            return if once_error != 0 {
                once_error
            } else {
                atfork_error
            };
        }
        let pid = libc::fork();
        if pid == 0 && !libc::getenv(c"OCERZ_HOSTMASKLOG".as_ptr()).is_null() {
            let mut hm = core::mem::MaybeUninit::<libc::sigset_t>::uninit();
            let mut hv = 0u32;
            if libc::pthread_sigmask(libc::SIG_BLOCK, ptr::null(), hm.as_mut_ptr()) == 0 {
                let hm = hm.assume_init();
                for sg in 1..32 {
                    if libc::sigismember(&hm, sg) != 0 {
                        hv |= 1u32 << sg;
                    }
                }
            }
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: HOSTMASK-FORKCHILD[%d] mask=%#x\n".as_ptr(),
                libc::getpid(),
                hv,
            );
        }
        if pid < 0 {
            return *libc::__error();
        }
        *out = pid;
        0
    }
}

pub(super) unsafe fn sys_fork(vm: *mut OcerzVM, cpu: *mut OcerzCPU, _a: *mut [u64; 8]) -> c_int {
    unsafe {
        let mut pid = 0;
        let err = guest_fork_apply(vm, &mut pid);
        if err != 0 {
            ret_err(cpu, err as u64);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        if pid == 0 {
            ret_ok2(cpu, 0, 1);
        } else {
            ret_ok2(cpu, pid as u64, 0);
        }
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_fork(
    vm: *mut OcerzVM,
    _cpu: *mut OcerzCPU,
    pid_out: *mut c_int,
) -> c_int {
    unsafe {
        let mut pid = 0;
        let err = guest_fork_apply(vm, &mut pid);
        if err == 0 {
            *pid_out = pid;
        }
        err
    }
}

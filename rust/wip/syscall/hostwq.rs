//! Native workqueue integration, guest worker callbacks, and kevent shims.

use super::util::*;
use super::*;

use core::ffi::{c_char, c_int, c_void};
use core::ptr;

const OCERZ_WQ_KEVENT_LIST_LEN: c_int = 16;
const OCERZ_OOL_COPY_MAX: u64 = 64 * 1024 * 1024;
const OCERZ_KEVENT_QOS_REARM: u64 = 0x48;
const OCERZ_EVFILT_TIMER: c_int = -7;
const OCERZ_EVFILT_WORKLOOP: c_int = -17;
const OCERZ_NOTE_LEEWAY: u32 = 0x10;
const OCERZ_NOTE_MACHTIME: u32 = 0x100;
const OCERZ_WQ_FLAG_BASE: u64 = 0x40000 | 0x200000 | 0x4000;
const OCERZ_WQ_FLAG_WORKLOOP: u64 = 0x400000;
const OCERZ_WQ_FLAG_KEVENT: u64 = 0x80000;
const OCERZ_WQ_FLAG_REUSE: u64 = 0x20000;
const OCERZ_WQ_FLAG_EVENT_MANAGER: u64 = 0x100000;
const OCERZ_WQ_FLAG_OVERCOMMIT: u64 = 0x10000;
const OCERZ_PTHREAD_PRIORITY_OVERCOMMIT: u64 = 0x80000000;
const OCERZ_PTHREAD_PRIORITY_EVENT_MANAGER: usize = 0x02000000;
const OCERZ_PTHREAD_TSD_SLOT_QOS: libc::pthread_key_t = 4;
const OCERZ_PTHREAD_COOKIE: u64 = 0x7ff8436bd690;
const OCERZ_WQ_GUARD_SIZE: u64 = 0x1000;
const OCERZ_UL_UNFAIR_LOCK: u64 = 0x2;
const OCERZ_ULF_WAKE_ALL: u64 = 0x100;
const OCERZ_ULF_WAKE_ALLOW_NON_OWNER: u64 = 0x400;
const CLOCK_UPTIME_RAW_VALUE: libc::clockid_t = 8;

type OcerzPthreadPriority = libc::c_ulong;
type WorkqueueQueueFn = unsafe extern "C" fn(OcerzPthreadPriority);
type WorkqueueKeventFn = unsafe extern "C" fn(*mut *mut c_void, *mut c_int);
type WorkqueueWorkloopFn = unsafe extern "C" fn(*mut u64, *mut *mut c_void, *mut c_int);

static mut G_HOSTWQ_VM: *mut OcerzVM = ptr::null_mut();
#[thread_local]
static mut G_HOSTWQ_TL_REGION: u64 = 0;
#[thread_local]
static mut G_HOSTWQ_TL_FATAL: c_int = 0;
static mut G_HOSTWQ_EXIT_KEY: libc::pthread_key_t = 0;
static mut G_HOSTWQ_EXIT_ONCE: libc::pthread_once_t = libc::PTHREAD_ONCE_INIT;
static mut G_HOSTWQ_REGISTER_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
static mut G_HOSTWQ_REGISTERED: c_int = 0;
static G_SPIN_DISPATCH: core::sync::atomic::AtomicU64 = core::sync::atomic::AtomicU64::new(0);

unsafe extern "C" {
    fn _pthread_workqueue_init_with_workloop(
        queue_func: Option<WorkqueueQueueFn>,
        kevent_func: Option<WorkqueueKeventFn>,
        workloop_func: Option<WorkqueueWorkloopFn>,
        offset: c_int,
        flags: c_int,
    ) -> c_int;
    fn mach_thread_self() -> libc::mach_port_t;
    fn mach_port_deallocate(task: libc::mach_port_t, name: libc::mach_port_t) -> c_int;
    static mach_task_self_: libc::mach_port_t;
    fn ocerz_peek_pending_async_sig() -> u32;
    fn clock_gettime_nsec_np(clock_id: libc::clockid_t) -> u64;
}

#[inline(always)]
pub(super) unsafe fn async_accept(guest_mask: u64) -> u32 {
    !((guest_mask as u32).wrapping_shl(1)) & !1
}

unsafe fn ocerz_hostwq_is_manager() -> c_int {
    let pri = unsafe { libc::pthread_getspecific(OCERZ_PTHREAD_TSD_SLOT_QOS) as usize };
    c_int::from(pri & OCERZ_PTHREAD_PRIORITY_EVENT_MANAGER != 0)
}

unsafe fn ocerz_no_stackbounds() -> c_int {
    static V: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
    let mut v = V.load(core::sync::atomic::Ordering::Relaxed);
    if v < 0 {
        v = c_int::from(!libc::getenv(c"OCERZ_NO_STACKBOUNDS".as_ptr()).is_null());
        V.store(v, core::sync::atomic::Ordering::Relaxed);
    }
    v
}

pub(super) unsafe fn ocerz_shadow_scan(tag: *const c_char, id: u64, gaddr: u64, mut len: u64) {
    static ON: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
    unsafe {
        let mut on = ON.load(core::sync::atomic::Ordering::Relaxed);
        if on < 0 {
            on = c_int::from(!libc::getenv(c"OCERZ_MACHLEAK".as_ptr()).is_null());
            ON.store(on, core::sync::atomic::Ordering::Relaxed);
        }
        if on == 0 || crate::ffi::ocerz_low_base == 0 || gaddr == 0 {
            return;
        }
        len = len.min(0x2000);
        let mut hits = 0;
        let mut off = 0u64;
        while off.wrapping_add(8) <= len && hits < 8 {
            let v = ocerz_ld(gaddr.wrapping_add(off), 8);
            let low = crate::ffi::ocerz_low_base;
            if v.wrapping_sub(low) < crate::ffi::OCERZ_LOW_LIMIT as u64 {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: SHADOWLEAK[%d] %s id=%llu off=%#llx addr=%#llx val=%#llx guest=%#llx\n".as_ptr(),
                    libc::getpid(),
                    tag,
                    id as libc::c_ulonglong,
                    off as libc::c_ulonglong,
                    gaddr.wrapping_add(off) as libc::c_ulonglong,
                    v as libc::c_ulonglong,
                    v.wrapping_sub(low) as libc::c_ulonglong,
                );
                hits += 1;
            }
            off += 4;
        }
    }
}

unsafe fn ocerz_hostwq_queue_worker(pri: OcerzPthreadPriority) {
    unsafe {
        let vm = G_HOSTWQ_VM;
        if vm.is_null() {
            return;
        }
        let wqthread_start =
            core::sync::atomic::AtomicU64::from_ptr(ptr::addr_of_mut!(g_wqthread_start))
                .load(core::sync::atomic::Ordering::Acquire);
        if wqthread_start == 0 {
            return;
        }
        let cookie = ocerz_ld(OCERZ_PTHREAD_COOKIE, 8);
        let mut region = G_HOSTWQ_TL_REGION;
        if region == 0 {
            region = crate::ffi::ocerz_map_anywhere(0x200000, libc::PROT_READ | libc::PROT_WRITE);
            if region == 0 {
                return;
            }
            G_HOSTWQ_TL_REGION = region;
            ocerz_hostwq_mark_region(region);
        }
        let pth = region.wrapping_add(0x1f0000);
        if G_HOSTWQ_TL_FATAL != 0 || !env_set!("OCERZ_NO_DDIRESET") {
            ocerz_st(pth.wrapping_add(0x1c8), 8, 0);
            ocerz_st(pth.wrapping_add(0x1b8), 8, 0);
            G_HOSTWQ_TL_FATAL = 0;
        }
        let kp = mach_thread_self();
        let registered = ocerz_ld(pth.wrapping_add(0xd8), 8) != 0;
        if !registered {
            ocerz_st(pth, 8, pth ^ cookie);
            ocerz_st(pth.wrapping_add(0xe0), 8, pth);
        }
        ocerz_st(pth.wrapping_add(0xf8), 4, kp as u32 as u64);
        if ocerz_no_stackbounds() == 0 {
            ocerz_st(pth.wrapping_add(0xb0), 8, pth);
            ocerz_st(
                pth.wrapping_add(0xb8),
                8,
                region.wrapping_add(OCERZ_WQ_GUARD_SIZE),
            );
        }
        let qosbits = ((pri as u64 >> 8) & 0x3fff) as u32;
        let qos_idx = (if qosbits != 0 {
            qosbits.trailing_zeros() as c_int + 1
        } else {
            4
        })
        .clamp(1, 6);
        let mut t = core::mem::MaybeUninit::<OcerzCPU>::zeroed().assume_init();
        t.vm = vm;
        t.mxcsr = 0x1f80;
        t.fcw = 0x037f;
        t.cpu_number = workers::ocerz_next_cpu_number();
        t.rip = wqthread_start;
        t.gpr[crate::ffi::OCERZ_RSP as usize] = pth.wrapping_sub(0x100);
        t.gpr[crate::ffi::OCERZ_RDI as usize] = pth;
        t.gpr[crate::ffi::OCERZ_RSI as usize] = kp as u64;
        t.gpr[crate::ffi::OCERZ_RDX as usize] = region.wrapping_add(OCERZ_WQ_GUARD_SIZE);
        t.gpr[crate::ffi::OCERZ_RCX as usize] = 0;
        t.gpr[crate::ffi::OCERZ_R8 as usize] = OCERZ_WQ_FLAG_BASE
            | qos_idx as u64
            | if registered { OCERZ_WQ_FLAG_REUSE } else { 0 }
            | if pri as u64 & OCERZ_PTHREAD_PRIORITY_OVERCOMMIT != 0 {
                OCERZ_WQ_FLAG_OVERCOMMIT
            } else {
                0
            };
        t.gpr[crate::ffi::OCERZ_R9 as usize] = 0;
        t.gs_base = pth.wrapping_add(0xe0);
        crate::ffi::ocerz_init_gate_wait();
        if crate::ffi::ocerz_vm_run_cpu(vm, &mut t) == 125 {
            G_HOSTWQ_TL_FATAL = 1;
        }
        mach_port_deallocate(mach_task_self_, kp);
        let _ = cookie;
    }
}

pub(super) unsafe fn ocerz_wq_run_exit(
    vm: *mut OcerzVM,
    t: *mut OcerzCPU,
    pth: u64,
    kp: libc::mach_port_t,
) -> c_int {
    unsafe {
        let wqthread_start =
            core::sync::atomic::AtomicU64::from_ptr(ptr::addr_of_mut!(g_wqthread_start))
                .load(core::sync::atomic::Ordering::Acquire);
        if wqthread_start == 0 || pth == 0 {
            return 0;
        }
        (*t).terminated = 0;
        (*t).wq_returned = 0;
        (*t).ras_top = 0;
        ptr::write_bytes(
            ptr::addr_of_mut!((*t).ras).cast::<u8>(),
            0,
            core::mem::size_of_val(&(*t).ras),
        );
        (*t).rip = wqthread_start;
        (*t).gpr[crate::ffi::OCERZ_RSP as usize] = pth.wrapping_sub(0x100);
        (*t).gpr[crate::ffi::OCERZ_RDI as usize] = pth;
        (*t).gpr[crate::ffi::OCERZ_RSI as usize] = kp as u64;
        (*t).gpr[crate::ffi::OCERZ_RDX as usize] =
            pth.wrapping_sub(0x1f0000).wrapping_add(OCERZ_WQ_GUARD_SIZE);
        (*t).gpr[crate::ffi::OCERZ_RCX as usize] = 0;
        (*t).gpr[crate::ffi::OCERZ_R8 as usize] = OCERZ_WQ_FLAG_BASE | 4 | OCERZ_WQ_FLAG_REUSE;
        (*t).gpr[crate::ffi::OCERZ_R9 as usize] = 0xffffffff;
        (*t).gs_base = pth.wrapping_add(0xe0);
        crate::ffi::ocerz_init_gate_wait();
        crate::ffi::ocerz_vm_run_cpu(vm, t)
    }
}

unsafe extern "C" fn ocerz_hostwq_thread_exit(arg: *mut c_void) {
    unsafe {
        let region = arg as usize as u64;
        let vm = G_HOSTWQ_VM;
        if vm.is_null() || (*vm).exited != 0 || region == 0 {
            return;
        }
        let pth = region.wrapping_add(0x1f0000);
        if ocerz_ld(pth.wrapping_add(0xd8), 8) == 0 {
            return;
        }
        let kp = mach_thread_self();
        ocerz_st(pth.wrapping_add(0xf8), 4, kp as u32 as u64);
        let mut t = core::mem::MaybeUninit::<OcerzCPU>::zeroed().assume_init();
        t.vm = vm;
        t.mxcsr = 0x1f80;
        t.fcw = 0x037f;
        t.cpu_number = workers::ocerz_next_cpu_number();
        ocerz_wq_run_exit(vm, &mut t, pth, kp);
        mach_port_deallocate(mach_task_self_, kp);
    }
}

unsafe extern "C" fn ocerz_hostwq_exit_key_init() {
    unsafe {
        libc::pthread_key_create(
            ptr::addr_of_mut!(G_HOSTWQ_EXIT_KEY),
            Some(ocerz_hostwq_thread_exit),
        );
    }
}

unsafe fn ocerz_hostwq_mark_region(region: u64) {
    unsafe {
        libc::pthread_once(
            ptr::addr_of_mut!(G_HOSTWQ_EXIT_ONCE),
            Some(ocerz_hostwq_exit_key_init),
        );
        libc::pthread_setspecific(G_HOSTWQ_EXIT_KEY, region as usize as *const c_void);
    }
}

unsafe fn ocerz_hostwq_bridge(extra_r8: u64, workloop_id: u64, hev: *const c_void, mut nev: c_int) {
    unsafe {
        let vm = G_HOSTWQ_VM;
        if vm.is_null() || nev <= 0 || hev.is_null() {
            return;
        }
        let wqthread_start =
            core::sync::atomic::AtomicU64::from_ptr(ptr::addr_of_mut!(g_wqthread_start))
                .load(core::sync::atomic::Ordering::Acquire);
        if wqthread_start == 0 {
            return;
        }
        nev = nev.min(OCERZ_WQ_KEVENT_LIST_LEN);
        let cookie = ocerz_ld(OCERZ_PTHREAD_COOKIE, 8);
        let mut region = G_HOSTWQ_TL_REGION;
        if region == 0 {
            region = crate::ffi::ocerz_map_anywhere(0x200000, libc::PROT_READ | libc::PROT_WRITE);
            if region == 0 {
                return;
            }
            G_HOSTWQ_TL_REGION = region;
            ocerz_hostwq_mark_region(region);
        }
        let pth = region.wrapping_add(0x1f0000);
        let evbuf = pth.wrapping_add(0x8000);
        ocerz_st(evbuf.wrapping_sub(8), 8, workloop_id);
        let stride = workers::ocerz_kev_stride();
        for i in 0..nev as usize {
            ptr::copy_nonoverlapping(
                hev.cast::<u8>().add(i * stride as usize),
                ocerz_g2h(evbuf.wrapping_add(i as u64 * stride)).cast::<u8>(),
                stride as usize,
            );
        }
        if env_set!("OCERZ_WSIG") {
            for i in 0..nev as usize {
                let ev = hev.cast::<u8>().add(i * stride as usize);
                let ident = ptr::read_unaligned(ev.cast::<u64>());
                let filt = ptr::read_unaligned(ev.add(8).cast::<i16>());
                let fl = ptr::read_unaligned(ev.add(10).cast::<u16>());
                let udata = ptr::read_unaligned(ev.add(16).cast::<u64>());
                let data = ptr::read_unaligned(ev.add(32).cast::<u64>());
                let ext0 = ptr::read_unaligned(ev.add(40).cast::<u64>());
                let ext1 = ptr::read_unaligned(ev.add(48).cast::<u64>());
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: HOSTWQ-EV[%d] filt=%d flags=%#x ident=%#llx udata=%#llx data=%#llx ext0=%#llx ext1=%#llx\n".as_ptr(),
                    i as c_int,
                    filt as c_int,
                    fl as c_uint,
                    ident as libc::c_ulonglong,
                    udata as libc::c_ulonglong,
                    data as libc::c_ulonglong,
                    ext0 as libc::c_ulonglong,
                    ext1 as libc::c_ulonglong,
                );
            }
        }
        if !env_set!("OCERZ_NO_MACHBRIDGE") {
            for i in 0..nev as usize {
                let dst = evbuf.wrapping_add(i as u64 * stride);
                if ocerz_ld(dst.wrapping_add(8), 2) as u16 as i16 != -8 {
                    continue;
                }
                let hbuf = ocerz_ld(dst.wrapping_add(0x28), 8);
                let sz = ocerz_ld(dst.wrapping_add(0x30), 8);
                let gbuf = machmsg::ocerz_bridge_mach_msg(hbuf, sz);
                if gbuf != 0 {
                    ocerz_st(dst.wrapping_add(0x28), 8, gbuf);
                } else if hbuf != 0 {
                    ocerz_st(dst.wrapping_add(0x28), 8, 0);
                }
            }
        }
        ocerz_shadow_scan(c"wqev".as_ptr(), nev as u64, evbuf, nev as u64 * stride);
        let kp = mach_thread_self();
        if G_HOSTWQ_TL_FATAL != 0 || !env_set!("OCERZ_NO_DDIRESET") {
            ocerz_st(pth.wrapping_add(0x1c8), 8, 0);
            ocerz_st(pth.wrapping_add(0x1b8), 8, 0);
            G_HOSTWQ_TL_FATAL = 0;
        }
        let registered = ocerz_ld(pth.wrapping_add(0xd8), 8) != 0;
        if !registered {
            ocerz_st(pth, 8, pth ^ cookie);
            ocerz_st(pth.wrapping_add(0xe0), 8, pth);
        }
        ocerz_st(pth.wrapping_add(0xf8), 4, kp as u32 as u64);
        if ocerz_no_stackbounds() == 0 {
            ocerz_st(pth.wrapping_add(0xb0), 8, pth);
            ocerz_st(
                pth.wrapping_add(0xb8),
                8,
                region.wrapping_add(OCERZ_WQ_GUARD_SIZE),
            );
        }
        let mut t = core::mem::MaybeUninit::<OcerzCPU>::zeroed().assume_init();
        t.vm = vm;
        t.mxcsr = 0x1f80;
        t.fcw = 0x037f;
        t.cpu_number = workers::ocerz_next_cpu_number();
        t.rip = wqthread_start;
        t.gpr[crate::ffi::OCERZ_RSP as usize] = pth.wrapping_sub(0x100);
        t.gpr[crate::ffi::OCERZ_RDI as usize] = pth;
        t.gpr[crate::ffi::OCERZ_RSI as usize] = kp as u64;
        t.gpr[crate::ffi::OCERZ_RDX as usize] = region.wrapping_add(OCERZ_WQ_GUARD_SIZE);
        t.gpr[crate::ffi::OCERZ_RCX as usize] = evbuf;
        let mgr = ocerz_hostwq_is_manager() != 0 && !env_set!("OCERZ_NO_MGRFLAG");
        let mgr_flag = if mgr { OCERZ_WQ_FLAG_EVENT_MANAGER } else { 0 };
        t.gpr[crate::ffi::OCERZ_R8 as usize] = OCERZ_WQ_FLAG_BASE
            | extra_r8
            | mgr_flag
            | if mgr { 0 } else { 4 }
            | if registered { OCERZ_WQ_FLAG_REUSE } else { 0 };
        t.gpr[crate::ffi::OCERZ_R9 as usize] = nev as u64;
        t.gs_base = pth.wrapping_add(0xe0);
        crate::ffi::ocerz_init_gate_wait();
        let flt0 = if nev > 0 {
            ocerz_ld(evbuf.wrapping_add(8), 2) as u16 as i16
        } else {
            0
        };
        let ident0 = if nev > 0 { ocerz_ld(evbuf, 8) } else { 0 };
        if env_set!("OCERZ_DQDUMP") {
            workers::wl_dqdump(c"ENTER".as_ptr(), workloop_id, t.cpu_number as u32, kp);
        }
        let wrc = crate::ffi::ocerz_vm_run_cpu(vm, &mut t);
        if env_set!("OCERZ_DQDUMP") {
            workers::wl_dqdump(
                if wrc == 125 {
                    c"FATAL".as_ptr()
                } else {
                    c"EXIT".as_ptr()
                },
                workloop_id,
                t.cpu_number as u32,
                kp,
            );
        }
        if wrc == 125 {
            G_HOSTWQ_TL_FATAL = 1;
            let gs = t.gs_base;
            let ddi = ocerz_ld(gs.wrapping_add(0xe8), 8) & !2;
            let sdq = if ddi != 0 {
                ocerz_ld(ddi.wrapping_add(8), 8)
            } else {
                0
            };
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: WQ-FATAL cpu#%u kport=%#x wlid=%#llx rsp_base=%#llx ddi(gs:0xe8)=%#llx sdq=%#llx flt0=%d ident0=%#llx\n".as_ptr(),
                t.cpu_number,
                kp,
                workloop_id as libc::c_ulonglong,
                pth as libc::c_ulonglong,
                ddi as libc::c_ulonglong,
                sdq as libc::c_ulonglong,
                flt0 as c_int,
                ident0 as libc::c_ulonglong,
            );
        }
        mach_port_deallocate(mach_task_self_, kp);
    }
}

unsafe extern "C" fn ocerz_hostwq_queue_cb(pri: OcerzPthreadPriority) {
    ocerz_hostwq_queue_worker(pri);
}

pub(super) unsafe fn sys_workq_kernreturn(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    a: *mut [u64; 8],
) -> c_int {
    unsafe {
        let a = &*a;
        let op = a[0];
        if op == 0x4 {
            (*cpu).wq_returned = 1;
            (*cpu).terminated = 1;
            ret_ok(cpu, 0);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        if op == 0x40 || op == 0x100 {
            static REARM40: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
            let mut rearm40 = REARM40.load(core::sync::atomic::Ordering::Relaxed);
            if rearm40 < 0 {
                rearm40 = c_int::from(!libc::getenv(c"OCERZ_REARM40".as_ptr()).is_null());
                REARM40.store(rearm40, core::sync::atomic::Ordering::Relaxed);
            }
            let rstride = if rearm40 != 0 {
                0x40
            } else {
                OCERZ_KEVENT_QOS_REARM
            };
            let evp = workers::g_hostwq_tl_events;
            let nvp = workers::g_hostwq_tl_nevents;
            if !evp.is_null() && !nvp.is_null() {
                let mut n = a[2] as i64 as c_int;
                if n < 0 {
                    n = 0;
                }
                if env_set!("OCERZ_KRLOG") {
                    libc::fprintf(
                        crate::log::stderr(),
                        c"ocerz: KRET cpu#%u op=%#llx n=%d evcap=%d%s |".as_ptr(),
                        (*cpu).cpu_number,
                        op as libc::c_ulonglong,
                        n,
                        workers::g_hostwq_tl_evcap,
                        if n > workers::g_hostwq_tl_evcap {
                            c"  **TRUNCATED**".as_ptr()
                        } else {
                            c"".as_ptr()
                        },
                    );
                    for i in 0..n.min(8) {
                        let k = a[1].wrapping_add(i as u64 * rstride);
                        let wl = ocerz_ld(k, 8);
                        libc::fprintf(
                            crate::log::stderr(),
                            c" [%d]{id=%#llx filt=%d fl=%#llx fflags=%#llx dqs+38=%#llx}%s"
                                .as_ptr(),
                            i,
                            wl as libc::c_ulonglong,
                            ocerz_ld(k.wrapping_add(8), 2) as u16 as i16 as c_int,
                            ocerz_ld(k.wrapping_add(0xa), 2) as libc::c_ulonglong,
                            ocerz_ld(k.wrapping_add(0x18), 4) as libc::c_ulonglong,
                            if wl != 0 {
                                ocerz_ld(wl.wrapping_add(0x38), 8)
                            } else {
                                0
                            } as libc::c_ulonglong,
                            if i >= workers::g_hostwq_tl_evcap {
                                c"<<DROPPED".as_ptr()
                            } else {
                                c"".as_ptr()
                            },
                        );
                    }
                    libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
                }
                if n > workers::g_hostwq_tl_evcap {
                    libc::fprintf(
                        crate::log::stderr(),
                        c"ocerz: BUG: workq_kernreturn re-arm list n=%d exceeds kernel buffer capacity %d (op=%#llx) -- libdispatch's ddi_maxevents contract is broken on this OS build; re-arms are being DROPPED\n".as_ptr(),
                        n,
                        workers::g_hostwq_tl_evcap,
                        op as libc::c_ulonglong,
                    );
                    n = workers::g_hostwq_tl_evcap;
                }
                let hbuf = *evp;
                if !hbuf.is_null() && a[1] != 0 {
                    for i in 0..n as usize {
                        ptr::copy_nonoverlapping(
                            ocerz_g2h(a[1].wrapping_add(i as u64 * rstride)).cast::<u8>(),
                            hbuf.cast::<u8>().add(i * rstride as usize),
                            rstride as usize,
                        );
                    }
                    machmsg::ocerz_kev_timer_to_host(hbuf, n);
                } else {
                    n = 0;
                }
                *nvp = n;
                workers::g_hostwq_tl_events = ptr::null_mut();
                workers::g_hostwq_tl_nevents = ptr::null_mut();
            }
            (*cpu).wq_returned = 1;
            (*cpu).terminated = 1;
            ret_ok(cpu, 0);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        if op == 0x20 {
            if ocerz_hostwq_on() != 0 {
                ocerz_hostwq_register(vm);
                let fa = *a;
                let mut ret2 = 0u64;
                let mut err = 0;
                let result = raw::ocerz_host_syscall(368, &fa, &mut ret2, &mut err);
                if err != 0 {
                    ret_err(cpu, result as i32 as u64);
                } else {
                    ret_ok(cpu, result);
                }
                return crate::ffi::OCERZ_STEP_OK as c_int;
            }
            let mut reqcount = a[2] as c_int;
            if reqcount < 1 {
                reqcount = 1;
            }
            let prio = a[3];
            let cookie = ocerz_ld(OCERZ_PTHREAD_COOKIE, 8);
            let wqthread_start =
                core::sync::atomic::AtomicU64::from_ptr(ptr::addr_of_mut!(g_wqthread_start))
                    .load(core::sync::atomic::Ordering::Acquire);
            if wqthread_start == 0 {
                ret_err(cpu, OCERZ_ENOTSUP_V as u64);
                return crate::ffi::OCERZ_STEP_OK as c_int;
            }
            let running = workers::g_wq_running_load();
            let mut want = reqcount.min(8);
            if running + want > 32 {
                want = 32 - running;
            }
            for _ in 0..want {
                let region =
                    crate::ffi::ocerz_map_anywhere(0x200000, libc::PROT_READ | libc::PROT_WRITE);
                if region == 0 {
                    break;
                }
                workers::g_wq_running_add(1);
                let pth = region.wrapping_add(0x1f0000);
                ocerz_st(pth, 8, pth ^ cookie);
                ocerz_st(pth.wrapping_add(0xe0), 8, pth);
                let mut t = ptr::read(cpu);
                t.terminated = 0;
                t.cpu_number = workers::ocerz_next_cpu_number();
                t.rip = wqthread_start;
                let qosbits = ((prio >> 8) & 0x3fff) as u32;
                let qos_idx = if qosbits != 0 {
                    qosbits.trailing_zeros() as u64 + 1
                } else {
                    4
                };
                let qos_idx = qos_idx.clamp(1, 6);
                t.gpr[crate::ffi::OCERZ_RSP as usize] = pth.wrapping_sub(0x100);
                t.gpr[crate::ffi::OCERZ_RDI as usize] = pth;
                t.gpr[crate::ffi::OCERZ_RSI as usize] = 0;
                t.gpr[crate::ffi::OCERZ_RDX as usize] = region.wrapping_add(OCERZ_WQ_GUARD_SIZE);
                t.gpr[crate::ffi::OCERZ_RCX as usize] = 0;
                t.gpr[crate::ffi::OCERZ_R8 as usize] = 0x40000
                    | 0x200000
                    | 0x4000
                    | qos_idx
                    | if prio & OCERZ_PTHREAD_PRIORITY_OVERCOMMIT != 0 {
                        OCERZ_WQ_FLAG_OVERCOMMIT
                    } else {
                        0
                    };
                t.gpr[crate::ffi::OCERZ_R9 as usize] = 0;
                t.gs_base = pth.wrapping_add(0xe0);
                t.sig_altstack_sp = 0;
                t.sig_altstack_size = 0;
                t.sig_mask = 0;
                t.sig_pending = 0;
                t.sig_on_stack = 0;
                t.sig_last_fault = 0;
                t.sig_repeat = 0;
                if workers::ocerz_spawn_worker(vm, &t) != 0 {
                    workers::g_wq_running_add(-1);
                    break;
                }
            }
            ret_ok(cpu, 0);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        ret_ok(cpu, 0);
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

unsafe fn ocerz_spawn_workloop_worker(
    vm: *mut OcerzVM,
    cpu: *const OcerzCPU,
    workloop_id: u64,
    changelist: u64,
    mut nchanges: c_int,
) -> c_int {
    unsafe {
        if nchanges > 16 {
            nchanges = 16;
        }
        let stride = workers::ocerz_kev_stride();
        let mut has_wl = false;
        for i in 0..nchanges {
            if ocerz_ld(
                changelist.wrapping_add(i as u64 * stride).wrapping_add(8),
                2,
            ) as u16 as i16 as c_int
                == OCERZ_EVFILT_WORKLOOP
            {
                has_wl = true;
                break;
            }
        }
        if !has_wl {
            return -1;
        }
        let cookie = ocerz_ld(OCERZ_PTHREAD_COOKIE, 8);
        let wqthread_start =
            core::sync::atomic::AtomicU64::from_ptr(ptr::addr_of_mut!(g_wqthread_start))
                .load(core::sync::atomic::Ordering::Acquire);
        if wqthread_start == 0 {
            return -1;
        }
        let region = crate::ffi::ocerz_map_anywhere(0x200000, libc::PROT_READ | libc::PROT_WRITE);
        if region == 0 {
            return -1;
        }
        let pth = region.wrapping_add(0x1f0000);
        let evbuf = pth.wrapping_add(0x8000);
        ocerz_st(evbuf.wrapping_sub(8), 8, workloop_id);
        let mut nev = 0;
        let mut prio = 0u64;
        for i in 0..nchanges {
            let src = changelist.wrapping_add(i as u64 * stride);
            if ocerz_ld(src.wrapping_add(8), 2) as u16 as i16 as c_int != OCERZ_EVFILT_WORKLOOP {
                continue;
            }
            let dst = evbuf.wrapping_add(nev as u64 * stride);
            let mut off = 0u64;
            while off < stride {
                ocerz_st(dst.wrapping_add(off), 8, ocerz_ld(src.wrapping_add(off), 8));
                off += 8;
            }
            let flags = ocerz_ld(dst.wrapping_add(0xa), 2) as u16 & !(1 | 4);
            ocerz_st(dst.wrapping_add(0xa), 2, flags as u64);
            if nev == 0 {
                prio = ocerz_ld(src.wrapping_add(0xc), 4) as u32 as u64;
            }
            nev += 1;
        }
        workers::g_wq_running_add(1);
        ocerz_st(pth, 8, pth ^ cookie);
        ocerz_st(pth.wrapping_add(0xe0), 8, pth);
        let mut t = ptr::read(cpu);
        t.terminated = 0;
        t.cpu_number = workers::ocerz_next_cpu_number();
        t.rip = wqthread_start;
        for r in 0..16 {
            t.gpr[r] = 0;
        }
        let qosbits = ((prio >> 8) & 0x3fff) as u32;
        let mut qos_idx = if qosbits != 0 {
            qosbits.trailing_zeros() as u64 + 1
        } else {
            4
        };
        qos_idx = qos_idx.clamp(1, 6);
        t.gpr[crate::ffi::OCERZ_RSP as usize] = pth.wrapping_sub(0x100);
        t.gpr[crate::ffi::OCERZ_RDI as usize] = pth;
        t.gpr[crate::ffi::OCERZ_RSI as usize] = 0;
        t.gpr[crate::ffi::OCERZ_RDX as usize] = region.wrapping_add(OCERZ_WQ_GUARD_SIZE);
        t.gpr[crate::ffi::OCERZ_RCX as usize] = evbuf;
        t.gpr[crate::ffi::OCERZ_R8 as usize] =
            OCERZ_WQ_FLAG_BASE | OCERZ_WQ_FLAG_WORKLOOP | OCERZ_WQ_FLAG_KEVENT | qos_idx;
        t.gpr[crate::ffi::OCERZ_R9 as usize] = nev as u64;
        t.gs_base = pth.wrapping_add(0xe0);
        t.wq_workloop_id = workloop_id;
        t.sig_altstack_sp = 0;
        t.sig_altstack_size = 0;
        t.sig_mask = 0;
        t.sig_pending = 0;
        t.sig_on_stack = 0;
        t.sig_last_fault = 0;
        t.sig_repeat = 0;
        if workers::ocerz_spawn_worker(vm, &t) != 0 {
            workers::g_wq_running_add(-1);
            return -1;
        }
        0
    }
}

unsafe fn ocerz_hostwq_kevent_and_rearm(events: *mut *mut c_void, nevents: *mut c_int) {
    unsafe {
        workers::g_hostwq_tl_events = events;
        workers::g_hostwq_tl_nevents = nevents;
        workers::g_hostwq_tl_evcap = OCERZ_WQ_KEVENT_LIST_LEN;
        ocerz_hostwq_bridge(
            OCERZ_WQ_FLAG_KEVENT,
            0,
            if events.is_null() {
                ptr::null()
            } else {
                *events
            },
            if nevents.is_null() { 0 } else { *nevents },
        );
        if !workers::g_hostwq_tl_nevents.is_null() {
            if !nevents.is_null() {
                *nevents = 0;
            }
            workers::g_hostwq_tl_events = ptr::null_mut();
            workers::g_hostwq_tl_nevents = ptr::null_mut();
        }
    }
}

unsafe extern "C" fn ocerz_hostwq_kevent_cb(events: *mut *mut c_void, nevents: *mut c_int) {
    unsafe {
        ocerz_hostwq_kevent_and_rearm(events, nevents);
        if env_set!("OCERZ_REARMLOG")
            && !nevents.is_null()
            && *nevents > 0
            && !events.is_null()
            && !(*events).is_null()
        {
            let stride = workers::ocerz_kev_stride() as usize;
            for i in 0..*nevents as usize {
                let e = (*events).cast::<u8>().add(i * stride);
                let ident = ptr::read_unaligned(e.cast::<u64>());
                let filt = ptr::read_unaligned(e.add(8).cast::<i16>());
                let flags = ptr::read_unaligned(e.add(10).cast::<u16>());
                let udata = ptr::read_unaligned(e.add(16).cast::<u64>());
                let fflags = ptr::read_unaligned(e.add(24).cast::<u32>());
                let data = ptr::read_unaligned(e.add(32).cast::<i64>());
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: REARM[%d] filt=%d ident=%#llx flags=%#x fflags=%#x data=%lld udata=%#llx\n".as_ptr(),
                    i as c_int,
                    filt as c_int,
                    ident as libc::c_ulonglong,
                    flags as c_uint,
                    fflags,
                    data as libc::c_longlong,
                    udata as libc::c_ulonglong,
                );
            }
        }
    }
}

unsafe extern "C" fn ocerz_hostwq_workloop_cb(
    workloop_id: *mut u64,
    events: *mut *mut c_void,
    nevents: *mut c_int,
) {
    unsafe {
        workers::g_hostwq_tl_events = events;
        workers::g_hostwq_tl_nevents = nevents;
        workers::g_hostwq_tl_evcap = OCERZ_WQ_KEVENT_LIST_LEN;
        let id = if workloop_id.is_null() {
            0
        } else {
            *workloop_id
        };
        if env_set!("OCERZ_ULOCKLOG") || env_set!("OCERZ_KEVID") {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: HOSTWQ-WORKLOOP wlid=%#llx\n".as_ptr(),
                id as libc::c_ulonglong,
            );
        }
        if env_set!("OCERZ_SPINLOG") {
            let d = G_SPIN_DISPATCH.fetch_add(1, core::sync::atomic::Ordering::Relaxed) + 1;
            if d % 5000 == 0 {
                let ne = if nevents.is_null() { 0 } else { *nevents };
                let ev = if events.is_null() {
                    ptr::null()
                } else {
                    (*events).cast::<u8>()
                };
                let mut f0 = 0i16;
                let mut id0 = 0u64;
                let mut dt0 = 0u64;
                let mut ff0 = 0u32;
                if !ev.is_null() && ne > 0 {
                    id0 = ptr::read_unaligned(ev.cast::<u64>());
                    f0 = ptr::read_unaligned(ev.add(8).cast::<i16>());
                    ff0 = ptr::read_unaligned(ev.add(0xc).cast::<u32>());
                    dt0 = ptr::read_unaligned(ev.add(0x20).cast::<u64>());
                }
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: SPINLOG disp=%llu wlid=%#llx dq=%#llx nev=%d ev0{filt=%d fflags=%#x id=%#llx data=%#llx}\n".as_ptr(),
                    d as libc::c_ulonglong,
                    id as libc::c_ulonglong,
                    if id != 0 { ocerz_ld(id.wrapping_add(0x38), 8) } else { 0 }
                        as libc::c_ulonglong,
                    ne,
                    f0 as c_int,
                    ff0,
                    id0 as libc::c_ulonglong,
                    dt0 as libc::c_ulonglong,
                );
            }
        }
        ocerz_hostwq_bridge(
            OCERZ_WQ_FLAG_WORKLOOP | OCERZ_WQ_FLAG_KEVENT,
            id,
            if events.is_null() {
                ptr::null()
            } else {
                *events
            },
            if nevents.is_null() { 0 } else { *nevents },
        );
        if !workers::g_hostwq_tl_nevents.is_null() {
            if !nevents.is_null() {
                *nevents = 0;
            }
            workers::g_hostwq_tl_events = ptr::null_mut();
            workers::g_hostwq_tl_nevents = ptr::null_mut();
        }
    }
}

unsafe fn ocerz_hostwq_register(vm: *mut OcerzVM) {
    unsafe {
        libc::pthread_mutex_lock(ptr::addr_of_mut!(G_HOSTWQ_REGISTER_LOCK));
        if G_HOSTWQ_REGISTERED == 0 {
            crate::ffi::ocerz_jit_require_ordered(vm);
            G_HOSTWQ_REGISTERED = 1;
            G_HOSTWQ_VM = vm;
            let rc = _pthread_workqueue_init_with_workloop(
                Some(ocerz_hostwq_queue_cb),
                Some(ocerz_hostwq_kevent_cb),
                Some(ocerz_hostwq_workloop_cb),
                0,
                0,
            );
            if rc == 0 {
                crate::ffi::ocerz_unstick_start();
            }
            if !libc::getenv(c"OCERZ_HOSTWQ_LOG".as_ptr()).is_null() {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: HOSTWQ registered rc=%d\n".as_ptr(),
                    rc,
                );
            }
        }
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_HOSTWQ_REGISTER_LOCK));
    }
}

pub(super) unsafe fn sys_kevent_id(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    a: *mut [u64; 8],
) -> c_int {
    unsafe {
        let a = &*a;
        if ocerz_hostwq_on() != 0 {
            ocerz_hostwq_register(vm);
            let mut fa = *a;
            fa[6] = ocerz_ld(
                (*cpu).gpr[crate::ffi::OCERZ_RSP as usize].wrapping_add(8),
                8,
            );
            fa[7] = ocerz_ld(
                (*cpu).gpr[crate::ffi::OCERZ_RSP as usize].wrapping_add(16),
                8,
            );
            if env_set!("OCERZ_WSIG") && a[1] != 0 && a[2] as i64 > 0 {
                for i in 0..(a[2] as i64).min(8) as u64 {
                    let k = a[1].wrapping_add(i * workers::ocerz_kev_stride());
                    let filt = ocerz_ld(k.wrapping_add(8), 2) as u16 as i16;
                    if filt == -8 || filt as c_int == OCERZ_EVFILT_TIMER {
                        libc::fprintf(
                            crate::log::stderr(),
                            c"ocerz: KEVREG[%d] filt=%d flags=%#llx fflags=%#llx ident=%#llx udata=%#llx ext0=%#llx ext1=%#llx (ext0 g=%d)\n".as_ptr(),
                            i as c_int,
                            filt as c_int,
                            ocerz_ld(k.wrapping_add(0xa), 2) as libc::c_ulonglong,
                            ocerz_ld(k.wrapping_add(0x18), 4) as libc::c_ulonglong,
                            ocerz_ld(k, 8) as libc::c_ulonglong,
                            ocerz_ld(k.wrapping_add(0x10), 8) as libc::c_ulonglong,
                            ocerz_ld(k.wrapping_add(0x28), 8) as libc::c_ulonglong,
                            ocerz_ld(k.wrapping_add(0x30), 8) as libc::c_ulonglong,
                            crate::ffi::ocerz_addr_committed(ocerz_ld(k.wrapping_add(0x28), 8)),
                        );
                    }
                }
            }
            for i in [1usize, 3, 5, 6] {
                if fa[i] != 0 {
                    fa[i] = ocerz_g2h(fa[i]) as usize as u64;
                }
            }
            let mut ret2 = 0u64;
            let mut err = 0;
            let kid_log = env_set!("OCERZ_ULOCKLOG");
            let mut chg0f = 0i16;
            let mut chg0fl = 0u16;
            let mut chg0id = 0u64;
            if kid_log && a[1] != 0 && a[2] as i64 > 0 {
                chg0f = ocerz_ld(a[1].wrapping_add(8), 2) as u16 as i16;
                chg0fl = ocerz_ld(a[1].wrapping_add(0xa), 2) as u16;
                chg0id = ocerz_ld(a[1], 8);
            }
            if env_set!("OCERZ_KEVLOG") && a[2] as i32 == 0 && a[4] as i64 > 0 {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: KEVIDWAIT-ENTER[%d] cpu#%u id=%#llx nev=%lld flags=%#llx caller=%#llx\n".as_ptr(),
                    libc::getpid(),
                    (*cpu).cpu_number,
                    a[0] as libc::c_ulonglong,
                    a[4] as libc::c_longlong,
                    fa[7] as libc::c_ulonglong,
                    ocerz_ld((*cpu).gpr[crate::ffi::OCERZ_RSP as usize], 8) as libc::c_ulonglong,
                );
            }
            (*cpu).block_since_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW_VALUE);
            let r = raw::ocerz_host_syscall(375, &fa, &mut ret2, &mut err);
            (*cpu).block_since_ns = 0;
            if kid_log {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: KEVID[%d] cpu#%u id=%#llx nchg=%lld nev=%lld flags=%#llx chg0{id=%#llx filt=%d fl=%#x} -> r=%lld err=%d\n".as_ptr(),
                    libc::getpid(),
                    (*cpu).cpu_number,
                    a[0] as libc::c_ulonglong,
                    a[2] as libc::c_longlong,
                    a[4] as libc::c_longlong,
                    fa[7] as libc::c_ulonglong,
                    chg0id as libc::c_ulonglong,
                    chg0f as c_int,
                    chg0fl as c_uint,
                    r as i64,
                    err,
                );
            }
            if env_set!("OCERZ_DQDUMP") && chg0f == OCERZ_EVFILT_WORKLOOP as i16 {
                workers::wl_dqdump(c"ARM".as_ptr(), a[0], (*cpu).cpu_number as u32, 0);
            }
            if err == 0 && r as i64 > 0 && a[3] != 0 {
                let got = (r as i64).min(64) as u64;
                for i in 0..got {
                    let ev = a[3].wrapping_add(i * workers::ocerz_kev_stride());
                    if ocerz_ld(ev.wrapping_add(8), 2) as u16 as i16 != -8 {
                        continue;
                    }
                    let hb = ocerz_ld(ev.wrapping_add(0x28), 8);
                    let sz = ocerz_ld(ev.wrapping_add(0x30), 8);
                    let gb = machmsg::ocerz_bridge_mach_msg(hb, sz);
                    if gb != 0 {
                        ocerz_st(ev.wrapping_add(0x28), 8, gb);
                    } else if hb != 0 {
                        ocerz_st(ev.wrapping_add(0x28), 8, 0);
                    }
                }
                ocerz_shadow_scan(
                    c"kevid".as_ptr(),
                    r,
                    a[3],
                    got * workers::ocerz_kev_stride(),
                );
            }
            if err != 0 {
                ret_err(cpu, r as i32 as u64);
            } else {
                ret_ok(cpu, r);
            }
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        if !libc::getenv(c"OCERZ_KEVENT_WORKER".as_ptr()).is_null()
            && a[2] as i32 > 0
            && a[1] != 0
            && workers::g_wq_running_load() == 0
            && workers::wl_try_acquire(a[0]) != 0
        {
            if ocerz_spawn_workloop_worker(vm, cpu, a[0], a[1], a[2] as i32) != 0 {
                workers::wl_release(a[0]);
            }
        }
        if env_set!("OCERZ_MACHMSG") {
            let filt0 = if a[1] != 0 && a[2] as i64 > 0 {
                ocerz_ld(a[1].wrapping_add(8), 2) as u16 as i16
            } else {
                0
            };
            let id0 = if a[1] != 0 && a[2] as i64 > 0 {
                ocerz_ld(a[1], 8)
            } else {
                0
            };
            libc::fprintf(
                crate::log::stderr(),
                    c"ocerz: KEVID-STUB[%d] wl=%#llx nchg=%lld nev=%lld filt0=%d ident0=%#llx (dropped)\n".as_ptr(),
                libc::getpid(),
                a[0] as libc::c_ulonglong,
                a[2] as libc::c_longlong,
                a[4] as libc::c_longlong,
                filt0 as c_int,
                id0 as libc::c_ulonglong,
            );
        }
        ret_ok(cpu, 0);
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

pub(super) unsafe fn sys_kevent_qos(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    a: *mut [u64; 8],
) -> c_int {
    unsafe {
        let a = &*a;
        let kq_flags = ocerz_ld(
            (*cpu).gpr[crate::ffi::OCERZ_RSP as usize].wrapping_add(16),
            8,
        );
        if !libc::getenv(c"OCERZ_MGRPROBE".as_ptr()).is_null() && kq_flags & 0x20 != 0 {
            let nch = a[2] as i32;
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: KQWORKQ[%d] flags=%#llx nchg=%d nev=%lld".as_ptr(),
                libc::getpid(),
                kq_flags as libc::c_ulonglong,
                nch,
                a[4] as i64,
            );
            for i in 0..nch.min(4) {
                let k = a[1].wrapping_add(i as u64 * workers::ocerz_kev_stride());
                libc::fprintf(
                    crate::log::stderr(),
                    c" [%d]{id=%#llx filt=%d fl=%#llx qos=%#llx fflags=%#llx}".as_ptr(),
                    i,
                    ocerz_ld(k, 8) as libc::c_ulonglong,
                    ocerz_ld(k.wrapping_add(8), 2) as u16 as i16 as c_int,
                    ocerz_ld(k.wrapping_add(0xa), 2) as libc::c_ulonglong,
                    ocerz_ld(k.wrapping_add(0xc), 4) as libc::c_ulonglong,
                    ocerz_ld(k.wrapping_add(0x18), 4) as libc::c_ulonglong,
                );
            }
            libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
        }
        static FWD_WORKQ: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
        let mut fwd = FWD_WORKQ.load(core::sync::atomic::Ordering::Relaxed);
        if fwd < 0 {
            fwd = c_int::from(libc::getenv(c"OCERZ_NO_FWD_WORKQ".as_ptr()).is_null());
            FWD_WORKQ.store(fwd, core::sync::atomic::Ordering::Relaxed);
        }
        if ocerz_hostwq_on() != 0 && (kq_flags & 0x20 == 0 || fwd != 0) {
            ocerz_hostwq_register(vm);
            let mut fa = *a;
            fa[6] = ocerz_ld(
                (*cpu).gpr[crate::ffi::OCERZ_RSP as usize].wrapping_add(8),
                8,
            );
            fa[7] = kq_flags;
            for i in [1usize, 3, 5, 6] {
                if fa[i] != 0 {
                    fa[i] = ocerz_g2h(fa[i]) as usize as u64;
                }
            }
            let mut ret2 = 0;
            let mut err = 0;
            (*cpu).block_since_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW_VALUE);
            let r = raw::ocerz_host_syscall(374, &fa, &mut ret2, &mut err);
            (*cpu).block_since_ns = 0;
            if err == 0 && r as i64 > 0 && a[3] != 0 {
                let got = (r as i64).min(64) as u64;
                for i in 0..got {
                    let ev = a[3].wrapping_add(i * workers::ocerz_kev_stride());
                    if ocerz_ld(ev.wrapping_add(8), 2) as u16 as i16 != -8 {
                        continue;
                    }
                    let hb = ocerz_ld(ev.wrapping_add(0x28), 8);
                    let sz = ocerz_ld(ev.wrapping_add(0x30), 8);
                    let gb = machmsg::ocerz_bridge_mach_msg(hb, sz);
                    if gb != 0 {
                        ocerz_st(ev.wrapping_add(0x28), 8, gb);
                    } else if hb != 0 {
                        ocerz_st(ev.wrapping_add(0x28), 8, 0);
                    }
                }
                ocerz_shadow_scan(
                    c"kevqos".as_ptr(),
                    r,
                    a[3],
                    got * workers::ocerz_kev_stride(),
                );
            }
            if err != 0 {
                ret_err(cpu, r as i32 as u64);
            } else {
                ret_ok(cpu, r);
            }
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        ret_ok(cpu, 0);
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

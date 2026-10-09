//! Guest pthread startup, semaphore/ulock termination, and PE stack diagnostics.

use super::util::*;
use super::*;

use core::ffi::{c_char, c_int};
use core::ptr;

const OCERZ_UL_UNFAIR_LOCK: u64 = 0x2;
const OCERZ_ULF_WAKE_ALL: u64 = 0x100;
const OCERZ_ULF_WAKE_ALLOW_NON_OWNER: u64 = 0x400;

#[repr(C)]
#[derive(Clone, Copy)]
struct OcerzPeModule {
    base: u64,
    size: u64,
    name: [c_char; 24],
}

unsafe extern "C" {
    fn semaphore_signal(semaphore: libc::mach_port_t) -> c_int;
}

pub(super) unsafe fn sys_bsdthread_create(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    a: *mut [u64; 8],
) -> c_int {
    unsafe {
        let a = &*a;
        if !libc::getenv(c"OCERZ_HOSTMASKLOG".as_ptr()).is_null() {
            let mut cm = core::mem::MaybeUninit::<libc::sigset_t>::zeroed().assume_init();
            let mut cv = 0u32;
            if libc::pthread_sigmask(libc::SIG_BLOCK, ptr::null(), &mut cm) == 0 {
                for sg in 1..32 {
                    if libc::sigismember(&cm, sg) == 1 {
                        cv |= 1u32 << sg;
                    }
                }
            }
            if cv != 0 {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: HOSTMASK-CREATOR[%d] cpu#%u creating a thread with %#x blocked rip=%#llx\n".as_ptr(),
                    libc::getpid(),
                    (*cpu).cpu_number,
                    cv,
                    (*cpu).rip as libc::c_ulonglong,
                );
            }
        }
        let func = a[0];
        let funarg = a[1];
        let stack = a[2];
        let pth = a[3];
        let flags = a[4];
        let pthread_start =
            core::sync::atomic::AtomicU64::from_ptr(ptr::addr_of_mut!(g_pthread_start))
                .load(core::sync::atomic::Ordering::Acquire);
        if pthread_start == 0 {
            ret_err(cpu, OCERZ_ENOTSUP_V as u64);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        crate::ffi::ocerz_jit_require_ordered(vm);
        let w = libc::calloc(1, core::mem::size_of::<workers::OcerzWorker>())
            .cast::<workers::OcerzWorker>();
        if w.is_null() {
            ret_err(cpu, OCERZ_ENOMEM_V as u64);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        (*w).vm = vm;
        (*w).cpu = *cpu;
        workers::cpu_fresh_thread_state(ptr::addr_of_mut!((*w).cpu));
        (*w).cpu.ras_top = 0;
        ptr::write_bytes(
            ptr::addr_of_mut!((*w).cpu.ras).cast::<u8>(),
            0,
            core::mem::size_of_val(&(*w).cpu.ras),
        );
        (*w).cpu.terminated = 0;
        (*w).cpu.cpu_number = workers::ocerz_next_cpu_number();
        (*w).cpu.rip = pthread_start;
        (*w).cpu.gpr[crate::ffi::OCERZ_RSP as usize] = stack;
        (*w).cpu.gpr[crate::ffi::OCERZ_RDI as usize] = pth;
        (*w).cpu.gpr[crate::ffi::OCERZ_RSI as usize] = 0;
        (*w).cpu.gpr[crate::ffi::OCERZ_RDX as usize] = func;
        (*w).cpu.gpr[crate::ffi::OCERZ_RCX as usize] = funarg;
        (*w).cpu.gpr[crate::ffi::OCERZ_R8 as usize] = stack;
        (*w).cpu.gpr[crate::ffi::OCERZ_R9 as usize] = flags | 0x10000000;
        (*w).cpu.gs_base = pth.wrapping_add(0xe0);
        static TERMLOG: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
        let mut tl = TERMLOG.load(core::sync::atomic::Ordering::Relaxed);
        if tl < 0 {
            tl = c_int::from(!libc::getenv(c"OCERZ_TERMLOG".as_ptr()).is_null());
            TERMLOG.store(tl, core::sync::atomic::Ordering::Relaxed);
        }
        if tl != 0 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: TCREATE[%d] cpu#%u stack=%#llx pth=%#llx gs=%#llx\n".as_ptr(),
                libc::getpid(),
                (*w).cpu.cpu_number,
                stack as libc::c_ulonglong,
                pth as libc::c_ulonglong,
                (*w).cpu.gs_base as libc::c_ulonglong,
            );
        }
        (*w).cpu.sig_altstack_sp = 0;
        (*w).cpu.sig_altstack_size = 0;
        (*w).cpu.sig_pending = 0;
        (*w).cpu.sig_on_stack = 0;
        (*w).cpu.sig_last_fault = 0;
        (*w).cpu.sig_repeat = 0;
        if !libc::getenv(c"OCERZ_SIGTRACE".as_ptr()).is_null()
            || !libc::getenv(c"OCERZ_THREADLOG".as_ptr()).is_null()
        {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: THREADCREATE start=%#llx func=%#llx arg=%#llx pth=%#llx comm(pth)=%d stack=%#llx flags=%#llx icount=%#llx\n".as_ptr(),
                pthread_start as libc::c_ulonglong,
                func as libc::c_ulonglong,
                funarg as libc::c_ulonglong,
                pth as libc::c_ulonglong,
                crate::ffi::ocerz_addr_committed(pth),
                stack as libc::c_ulonglong,
                flags as libc::c_ulonglong,
                (*vm).insn_count as libc::c_ulonglong,
            );
        }
        ocerz_st(pth.wrapping_add(0xe0), 8, pth);
        let mut attr = core::mem::MaybeUninit::<libc::pthread_attr_t>::uninit();
        libc::pthread_attr_init(attr.as_mut_ptr());
        libc::pthread_attr_setstacksize(attr.as_mut_ptr(), 16 * 1024 * 1024);
        libc::pthread_attr_setdetachstate(attr.as_mut_ptr(), libc::PTHREAD_CREATE_DETACHED);
        let mut pub_ = core::mem::MaybeUninit::<workers::OcerzWorkerPub>::zeroed().assume_init();
        libc::pthread_mutex_init(ptr::addr_of_mut!(pub_.m), ptr::null());
        libc::pthread_cond_init(ptr::addr_of_mut!(pub_.c), ptr::null());
        pub_.published = 0;
        (*w).pub_ = &mut pub_;
        let mut th = core::mem::MaybeUninit::<libc::pthread_t>::uninit();
        let rc = libc::pthread_create(
            th.as_mut_ptr(),
            attr.as_ptr(),
            workers::ocerz_worker_entry,
            w.cast(),
        );
        libc::pthread_attr_destroy(attr.as_mut_ptr());
        if rc != 0 {
            libc::free(w.cast());
            ret_err(cpu, OCERZ_ENOMEM_V as u64);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        libc::pthread_mutex_lock(ptr::addr_of_mut!(pub_.m));
        while pub_.published == 0 {
            libc::pthread_cond_wait(ptr::addr_of_mut!(pub_.c), ptr::addr_of_mut!(pub_.m));
        }
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(pub_.m));
        libc::pthread_mutex_destroy(ptr::addr_of_mut!(pub_.m));
        libc::pthread_cond_destroy(ptr::addr_of_mut!(pub_.c));
        crate::ffi::ocerz_unstick_start();
        ret_ok(cpu, pth);
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

pub(super) unsafe fn ocerz_bsdthread_sema_is_port(value: u64) -> c_int {
    c_int::from(value == value as u32 as u64 && value & 3 == 3)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_pe_stack_dump(cpu: *mut OcerzCPU, tag: *const c_char) {
    unsafe {
        let tsd = (*cpu).gs_base;
        if tsd == 0 {
            return;
        }
        let mut teb = ocerz_ld(tsd.wrapping_add(0x30), 8);
        if teb < 0x10000 || teb > 0x7ffffffffff || teb & 0xfff != 0 {
            teb = (*cpu).wine_teb_base;
        }
        if teb < 0x10000 || teb > 0x7ffffffffff || teb & 0xfff != 0 {
            return;
        }
        if ocerz_ld(teb.wrapping_add(0x30), 8) != teb {
            return;
        }
        let wtid = ocerz_ld(teb.wrapping_add(0x48), 8);
        let sbase = ocerz_ld(teb.wrapping_add(8), 8);
        let sf = ocerz_ld(teb.wrapping_add(0x378), 8);
        let mut line = [0i8; 4096];
        let mut pos = 0i32;
        pos = libc::snprintf(
            line.as_mut_ptr(),
            line.len(),
            c"ocerz: %s[%d] cpu#%u wtid=%#llx".as_ptr(),
            tag,
            libc::getpid(),
            (*cpu).cpu_number,
            wtid as libc::c_ulonglong,
        );
        if sf == 0 {
            if pos < line.len() as i32 - 1 {
                pos += libc::snprintf(
                    line.as_mut_ptr().add(pos as usize),
                    line.len() - pos as usize,
                    c" no-syscall-frame\n".as_ptr(),
                );
            }
            libc::fputs(line.as_ptr(), crate::log::stderr());
            return;
        }
        let rsp = ocerz_ld(sf.wrapping_add(0x88), 8);
        let rip = ocerz_ld(sf.wrapping_add(0x70), 8);
        let rcx = ocerz_ld(sf.wrapping_add(0x10), 8);
        let rdx = ocerz_ld(sf.wrapping_add(0x18), 8);
        let sid = ocerz_ld(sf.wrapping_add(0xb0), 4) as u32;
        let mut mods = [OcerzPeModule {
            base: 0,
            size: 0,
            name: [0; 24],
        }; 160];
        let peb = ocerz_ld(teb.wrapping_add(0x60), 8);
        let ldr = if peb != 0 {
            ocerz_ld(peb.wrapping_add(0x18), 8)
        } else {
            0
        };
        let mut nmods = 0usize;
        if ldr != 0 {
            let head = ldr.wrapping_add(0x10);
            let mut e = ocerz_ld(head, 8);
            while e != 0 && e != head && nmods < mods.len() {
                mods[nmods].base = ocerz_ld(e.wrapping_add(0x30), 8);
                mods[nmods].size = ocerz_ld(e.wrapping_add(0x40), 8) & 0xffffffff;
                let len = (ocerz_ld(e.wrapping_add(0x58), 2) as u16 as usize) / 2;
                let buf = ocerz_ld(e.wrapping_add(0x60), 8);
                let mut k = 0usize;
                while k < len && k < 23 && buf != 0 {
                    let ch = ocerz_ld(buf.wrapping_add(2 * k as u64), 2) as u16 as u32;
                    mods[nmods].name[k] = if (0x20..0x7f).contains(&ch) {
                        ch as c_char
                    } else {
                        b'?' as c_char
                    };
                    k += 1;
                }
                mods[nmods].name[k] = 0;
                nmods += 1;
                e = ocerz_ld(e, 8);
            }
        }
        if pos < line.len() as i32 - 1 {
            pos += libc::snprintf(
                line.as_mut_ptr().add(pos as usize),
                line.len() - pos as usize,
                c" sid=%#x rcx=%#llx rdx=%#llx rsp=%#llx".as_ptr(),
                sid,
                rcx as libc::c_ulonglong,
                rdx as libc::c_ulonglong,
                rsp as libc::c_ulonglong,
            );
        }
        let mut hits = 0;
        for i in -1..400 {
            if hits >= 48 {
                break;
            }
            let mut w = if i < 0 { rip } else { 0 };
            if i >= 0 {
                let at = rsp.wrapping_add(8 * i as u64);
                if sbase != 0 && at >= sbase {
                    break;
                }
                w = ocerz_ld(at, 8);
            }
            for m in 0..nmods {
                if w >= mods[m].base && w < mods[m].base.wrapping_add(mods[m].size) {
                    if pos < line.len() as i32 - 1 {
                        pos += libc::snprintf(
                            line.as_mut_ptr().add(pos as usize),
                            line.len() - pos as usize,
                            c" %s%s+%#llx".as_ptr(),
                            if i < 0 {
                                c"rip=".as_ptr()
                            } else {
                                c"".as_ptr()
                            },
                            mods[m].name.as_ptr(),
                            w.wrapping_sub(mods[m].base) as libc::c_ulonglong,
                        );
                    }
                    hits += 1;
                    break;
                }
            }
        }
        if pos < line.len() as i32 - 1 {
            libc::snprintf(
                line.as_mut_ptr().add(pos as usize),
                line.len() - pos as usize,
                c"\n".as_ptr(),
            );
        }
        libc::fputs(line.as_ptr(), crate::log::stderr());
    }
}

unsafe fn exitlog_pe_stack(cpu: *mut OcerzCPU) {
    unsafe { ocerz_pe_stack_dump(cpu, c"THREADEXIT-PE".as_ptr()) }
}

pub(super) unsafe fn sys_bsdthread_terminate(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    a: *mut [u64; 8],
) -> c_int {
    unsafe {
        let a = &*a;
        if !libc::getenv(c"OCERZ_EXITLOG".as_ptr()).is_null() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: THREADEXIT[%d] cpu#%u rip=%#llx free=%#llx+%#llx kport=%#llx sema=%#llx gs=%#llx quit=%u/%u usr1=%u/%u bt:".as_ptr(),
                libc::getpid(),
                (*cpu).cpu_number,
                (*cpu).rip as libc::c_ulonglong,
                a[0] as libc::c_ulonglong,
                a[1] as libc::c_ulonglong,
                a[2] as libc::c_ulonglong,
                a[3] as libc::c_ulonglong,
                (*cpu).gs_base as libc::c_ulonglong,
                (*cpu).sig_host_rcvd[libc::SIGQUIT as usize],
                (*cpu).sig_delivered[libc::SIGQUIT as usize],
                (*cpu).sig_host_rcvd[libc::SIGUSR1 as usize],
                (*cpu).sig_delivered[libc::SIGUSR1 as usize],
            );
            let mut fp = (*cpu).gpr[crate::ffi::OCERZ_RBP as usize];
            for _ in 0..8 {
                if fp <= 0x1000 {
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
            exitlog_pe_stack(cpu);
        }
        static TERMLOG: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
        let mut tl = TERMLOG.load(core::sync::atomic::Ordering::Relaxed);
        if tl < 0 {
            tl = c_int::from(!libc::getenv(c"OCERZ_TERMLOG".as_ptr()).is_null());
            TERMLOG.store(tl, core::sync::atomic::Ordering::Relaxed);
        }
        if tl != 0 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: TERM[%d] cpu#%u freeaddr=%#llx freesize=%#llx kport=%#llx sema=%#llx\n"
                    .as_ptr(),
                libc::getpid(),
                (*cpu).cpu_number,
                a[0] as libc::c_ulonglong,
                a[1] as libc::c_ulonglong,
                a[2] as libc::c_ulonglong,
                a[3] as libc::c_ulonglong,
            );
        }
        let sema_or_ulock = a[3];
        if sema_or_ulock != 0 && ocerz_bsdthread_sema_is_port(sema_or_ulock) != 0 {
            semaphore_signal(sema_or_ulock as u32);
        } else if sema_or_ulock != 0 {
            ocerz_st(sema_or_ulock, 4, (a[2] as u32 & !3) as u64);
            let mut wa = [
                OCERZ_UL_UNFAIR_LOCK | OCERZ_ULF_WAKE_ALL | OCERZ_ULF_WAKE_ALLOW_NON_OWNER,
                ocerz_g2h(sema_or_ulock) as usize as u64,
                0,
                0,
                0,
                0,
                0,
                0,
            ];
            let mut err = 0;
            raw::ocerz_host_syscall(516, &wa, ptr::null_mut(), &mut err);
        }
        if a[0] != 0 && a[1] != 0 && a[0].wrapping_add(a[1]) > a[0] {
            mem::invalidate_guest_mapping(vm, a[0], a[1]);
            crate::ffi::ocerz_unmap(a[0], a[1]);
        }
        (*cpu).terminated = 1;
        ret_ok(cpu, 0);
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

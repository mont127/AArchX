//! ---- threads ----
//! A thread the guest creates is a host thread whose start routine is a
//! callback into the guest's, and its guest frames run on a stack ocerz makes
//! for it when it first enters guest code.  The host stack the attributes size
//! holds ocerz's own frames and the native frames of every bridged call
//! instead, and the size the guest asked for was chosen for its own frames:
//! Chromium makes one thread with a 16 KB stack, and the translator alone takes
//! 40 KB of host stack to translate a block, so that thread overflowed its
//! guard page the first time it ran a block nobody had translated.  A guest
//! asking for less than 512 KB, the size a secondary thread gets by default,
//! gets 512 KB of host stack; one that hands over stack memory of its own keeps
//! it.
//!
//! pthread_key_create and its siblings keep the guest's values in the guest
//! thread block, where an inlined gs-relative load finds them (the overrides
//! file explains why), and Darwin's reserved keys 10 to 255 live there too,
//! usable without being created, since the Swift runtime uses one that way;
//! pthread_key_init_np records a destructor for one, which runs with the
//! others when the thread ends.  dispatch_main ends the main thread in the real
//! libdispatch, which then drains the main queue from its thread pool.  Here the
//! main thread is the one the process belongs to, so the handler runs the
//! native CFRunLoopRun on it for good instead, which drains the main queue the
//! way an application's main thread does; the asynchronous main of a Swift
//! program ends there, and its exit comes from a job on that queue.  A call from
//! any other thread stops the process, as libdispatch's own check does.

use core::ffi::{c_char, c_int, c_void};
use core::sync::atomic::{AtomicI32, AtomicU64, Ordering};

use super::common::*;
use crate::ffi::*;

const SB_KEY_FIRST: u64 = 256;
const SB_KEY_END: u64 = 768;
const SB_KEY_ROUNDS: c_int = 4;
const SB_RESERVED_FIRST: u64 = 10;

#[repr(C)]
struct SbKey {
    used: AtomicI32,
    destructor: AtomicU64,
}

const SB_KEY_INIT: SbKey = SbKey { used: AtomicI32::new(0), destructor: AtomicU64::new(0) };
static G_SB_KEYS: [SbKey; (SB_KEY_END - SB_KEY_FIRST) as usize] =
    [SB_KEY_INIT; (SB_KEY_END - SB_KEY_FIRST) as usize];
static G_SB_RESERVED: [SbKey; SB_KEY_FIRST as usize] = [SB_KEY_INIT; SB_KEY_FIRST as usize];
static mut G_SB_KEY_NEXT: u32 = 0;
static G_SB_KEY_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

unsafe fn sb_key(key: u64) -> *const SbKey {
    unsafe {
        if key >= SB_RESERVED_FIRST && key < SB_KEY_FIRST {
            return &raw const G_SB_RESERVED[key as usize];
        }
        if key < SB_KEY_FIRST || key >= SB_KEY_END {
            return core::ptr::null();
        }
        let k = &raw const G_SB_KEYS[(key - SB_KEY_FIRST) as usize];
        if (*k).used.load(Ordering::SeqCst) != 0 { k } else { core::ptr::null() }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_pthread_key_create(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let out = sb_arg(cpu, 0);
        let destructor = sb_arg(cpu, 1);
        let n = (SB_KEY_END - SB_KEY_FIRST) as u32;
        let mut got = n;
        libc::pthread_mutex_lock(&raw const G_SB_KEY_LOCK as *mut _);
        let mut i = 0;
        while i < n && got == n {
            let at = (G_SB_KEY_NEXT + i) % n;
            if G_SB_KEYS[at as usize].used.load(Ordering::SeqCst) == 0 {
                got = at;
            }
            i += 1;
        }
        if got != n {
            G_SB_KEYS[got as usize].destructor.store(destructor, Ordering::SeqCst);
            G_SB_KEYS[got as usize].used.store(1, Ordering::SeqCst);
            G_SB_KEY_NEXT = got + 1;
        }
        libc::pthread_mutex_unlock(&raw const G_SB_KEY_LOCK as *mut _);
        if got == n {
            return sb_ret(vm, cpu, libc::EAGAIN as i64);
        }
        if (*cpu).gs_base != 0 {
            ocerz_st((*cpu).gs_base + 8 * (SB_KEY_FIRST + got as u64), 8, 0);
        }
        ocerz_st(out, 8, SB_KEY_FIRST + got as u64);
        sb_ret(vm, cpu, 0)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_pthread_key_init_np(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let key = sb_arg(cpu, 0) as u32 as u64;
        let destructor = sb_arg(cpu, 1);
        if key < SB_RESERVED_FIRST || key >= SB_KEY_FIRST {
            return sb_ret(vm, cpu, libc::EINVAL as i64);
        }
        G_SB_RESERVED[key as usize].destructor.store(destructor, Ordering::SeqCst);
        G_SB_RESERVED[key as usize].used.store(1, Ordering::SeqCst);
        sb_ret(vm, cpu, 0)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_pthread_key_delete(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let which = sb_arg(cpu, 0);
        let k = if which >= SB_KEY_FIRST { sb_key(which) } else { core::ptr::null() };
        if k.is_null() {
            return sb_ret(vm, cpu, libc::EINVAL as i64);
        }
        (*k).used.store(0, Ordering::SeqCst);
        (*k).destructor.store(0, Ordering::SeqCst);
        sb_ret(vm, cpu, 0)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_pthread_setspecific(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let key = sb_arg(cpu, 0);
        if sb_key(key).is_null() || (*cpu).gs_base == 0 {
            return sb_ret(vm, cpu, libc::EINVAL as i64);
        }
        ocerz_st((*cpu).gs_base + 8 * key, 8, sb_arg(cpu, 1));
        sb_ret(vm, cpu, 0)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_pthread_getspecific(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let key = sb_arg(cpu, 0);
        let v = if !sb_key(key).is_null() && (*cpu).gs_base != 0 {
            ocerz_ld((*cpu).gs_base + 8 * key, 8)
        } else {
            0
        };
        sb_ret(vm, cpu, v as i64)
    }
}

unsafe fn sb_key_destructors(vm: *mut OcerzVM, cpu: *mut OcerzCPU) {
    unsafe {
        static OFF: AtomicI32 = AtomicI32::new(-1);
        let mut off = OFF.load(Ordering::SeqCst);
        if off < 0 {
            off = if !libc::getenv(c"OCERZ_NO_TSD_DTORS".as_ptr()).is_null() { 1 } else { 0 };
            OFF.store(off, Ordering::SeqCst);
        }
        if off != 0 || vm.is_null() || cpu.is_null() || (*cpu).gs_base == 0 {
            return;
        }
        for _round in 0..SB_KEY_ROUNDS {
            let mut ran = 0;
            let mut key = SB_RESERVED_FIRST;
            while key < SB_KEY_END && (*vm).exited == 0 {
                let k = if key < SB_KEY_FIRST {
                    &raw const G_SB_RESERVED[key as usize]
                } else {
                    &raw const G_SB_KEYS[(key - SB_KEY_FIRST) as usize]
                };
                if key < SB_KEY_FIRST && (*k).used.load(Ordering::SeqCst) == 0 {
                    key += 1;
                    continue;
                }
                let destructor = if (*k).used.load(Ordering::SeqCst) != 0 {
                    (*k).destructor.load(Ordering::SeqCst)
                } else {
                    0
                };
                let slot = (*cpu).gs_base + 8 * key;
                let value = ocerz_ld(slot, 8);
                if value == 0 {
                    key += 1;
                    continue;
                }
                ocerz_st(slot, 8, 0);
                if destructor == 0 {
                    key += 1;
                    continue;
                }
                let args: [u64; 1] = [value];
                ocerz_vm_call(
                    vm,
                    destructor,
                    args.as_ptr(),
                    1,
                    ((*cpu).gpr[OCERZ_RSP as usize] - 256) & !0xf,
                );
                ran = 1;
                key += 1;
            }
            if ran == 0 {
                break;
            }
        }
    }
}

#[repr(C)]
#[derive(Copy, Clone)]
struct SbThreadStart {
    entry: extern "C" fn(*mut c_void) -> *mut c_void,
    arg: *mut c_void,
}

extern "C" fn sb_thread_main(p: *mut c_void) -> *mut c_void {
    unsafe {
        let start = *(p as *mut SbThreadStart);
        libc::free(p);
        let result = (start.entry)(start.arg);
        let mut cpu = ocerz_vm_current_cpu();
        if cpu.is_null() {
            cpu = ocerz_thread_attach(ocerz_vm_process());
        }
        if !cpu.is_null() {
            sb_key_destructors((*cpu).vm, cpu);
        }
        result
    }
}

const SB_HOST_STACK_MIN: usize = 512 * 1024;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_pthread_create(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let out = sb_arg(cpu, 0);
        let attr = sb_arg(cpu, 1);
        let routine = sb_arg(cpu, 2);
        let arg = sb_arg(cpu, 3);
        let start = libc::malloc(size_of::<SbThreadStart>()) as *mut SbThreadStart;
        let entry = if routine != 0 {
            ocerz_abi_callback_intern(routine, c"p(p)".as_ptr())
        } else {
            core::ptr::null_mut()
        };
        if start.is_null() || entry.is_null() {
            libc::free(start as *mut c_void);
            return sb_ret(
                vm,
                cpu,
                if routine != 0 { libc::EAGAIN } else { libc::EINVAL } as i64,
            );
        }
        (*start).entry = core::mem::transmute::<*mut c_void, extern "C" fn(*mut c_void) -> *mut c_void>(entry);
        (*start).arg = sb_ptr(arg);
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_pthread_create".as_ptr(), c"i(ppc{p(p)}p)".as_ptr(), libc::pthread_create as *const _);
        let mut use_ = sb_ptr(attr) as *const libc::pthread_attr_t;
        let mut own: libc::pthread_attr_t = core::mem::zeroed();
        let mut want: usize = 0;
        let mut at: *mut c_void = core::ptr::null_mut();
        if !use_.is_null()
            && libc::pthread_attr_getstacksize(use_, &mut want) == 0
            && want < SB_HOST_STACK_MIN
            && libc::pthread_attr_getstackaddr(use_, &mut at) == 0
            && at.is_null()
        {
            own = *use_;
            libc::pthread_attr_setstacksize(&mut own, SB_HOST_STACK_MIN);
            use_ = &own;
        }
        let mut made: libc::pthread_t = 0;
        let rc = libc::pthread_create(&mut made, use_, sb_thread_main, start as *mut c_void);
        ocerz_bridge_lower(&outer);
        if rc != 0 {
            libc::free(start as *mut c_void);
        } else if out != 0 {
            ocerz_st(out, 8, made as u64);
        }
        sb_ret(vm, cpu, rc as i64)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_pthread_exit(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let value = sb_ptr(sb_arg(cpu, 0));
        sb_key_destructors(vm, cpu);
        libc::pthread_exit(value);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_dispatch_main(vm: *mut OcerzVM, _cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        if libc::pthread_main_np() == 0 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: bridge: dispatch_main() must be called on the main thread\n".as_ptr(),
            );
            ocerz_vm_request_exit(vm, 134);
            return OCERZ_STEP_EXIT as c_int;
        }
        let run_sym = ocerz_bridge_host_symbol(
            OCERZ_BRIDGE_COREFOUNDATION.as_ptr() as *const c_char,
            c"CFRunLoopRun".as_ptr(),
        );
        if run_sym.is_null() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: bridge: dispatch_main needs the native CFRunLoopRun, which could not be found\n".as_ptr(),
            );
            libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
        }
        let run = core::mem::transmute::<*mut c_void, extern "C" fn()>(run_sym);
        let mut outer = OcerzBridgeFrame::default();
        ocerz_bridge_raise(&mut outer, SB_LIB, c"_dispatch_main".as_ptr(), c"v()".as_ptr(), run as *const c_void);
        loop {
            run();
        }
    }
}

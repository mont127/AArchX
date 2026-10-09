//! ---- exceptions ----
//! A guest @throw crosses into guest code at the guest's own __cxa_throw, after
//! the exception object has been wrapped in a shape the guest C++ runtime
//! recognizes: a fake exception whose type-info vtable is a guest page of
//! ocerz's own entries, so the guest personality asks ocerz, at the catch
//! clause, whether the types match.  A native exception raised inside a
//! crossing unwinds up to the ocerz_objc_guarded frame (see objcguard.rs),
//! whose personality claims it; the landing pad calls ocerz_objc_guard_landed,
//! which hands an Objective-C exception's object back so the send can throw it
//! on into the guest, and rethrows anything else.

use core::ffi::{c_char, c_int, c_void};
use core::ptr::{null, null_mut};
use core::sync::atomic::{AtomicI32, AtomicPtr, AtomicU32, AtomicU64, Ordering};

use crate::ffi::*;
use crate::ported::objcbridge::common::*;
use crate::ported::objcbridge::send::*;

// libunwind types, not bound by libc on this toolchain.
#[repr(C)]
pub struct UnwindException {
    pub exception_class: u64,
    pub exception_cleanup: u64,
    pub private1: u64,
    pub private2: u64,
}
#[repr(C)]
pub struct UnwindContext {
    _private: [u8; 0],
}
type UnwindReasonCode = c_int;
type UnwindAction = c_int;
const URC_CONTINUE_UNWIND: UnwindReasonCode = 8;
const URC_HANDLER_FOUND: UnwindReasonCode = 6;
const URC_INSTALL_CONTEXT: UnwindReasonCode = 7;
const UA_SEARCH_PHASE: UnwindAction = 1;
const UA_HANDLER_FRAME: UnwindAction = 4;
const UA_FORCE_UNWIND: UnwindAction = 8;

unsafe extern "C" {
    fn _Unwind_SetGR(ctx: *mut UnwindContext, reg: c_int, value: usize);
    fn _Unwind_SetIP(ctx: *mut UnwindContext, value: usize);
    static ocerz_objc_guard_pad: usize;
    fn dlsym(handle: *mut c_void, name: *const c_char) -> *mut c_void;
}
const RTLD_DEFAULT: *mut c_void = -2isize as *mut c_void;

/* ---- the uncaught handler ---- */

static G_OB_NATIVE_PREV: AtomicPtr<c_void> = AtomicPtr::new(null_mut());
static G_OB_GUEST_HANDLER: AtomicU64 = AtomicU64::new(0);
static G_OB_GUEST_HANDLER_NATIVE: AtomicPtr<c_void> = AtomicPtr::new(null_mut());
static mut G_OB_UNCAUGHT_ONCE: libc::pthread_once_t = libc::PTHREAD_ONCE_INIT;

unsafe fn ob_exception_text(exc: *mut c_void, selname: *const c_char, buf: *mut c_char, len: usize) {
    unsafe {
        libc::snprintf(buf, len, c"(none)".as_ptr());
        let sel = ob_sel_register_name(selname);
        let cls = ob_object_get_class(exc);
        if !ob_class_responds_to_selector(cls, sel) {
            libc::snprintf(buf, len, c"(a %s)".as_ptr(), ob_class_get_name(cls));
            return;
        }
        let send: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void =
            core::mem::transmute(ob_need(&raw const G_OB_MSGSEND));
        let str_ = send(exc, sel);
        if str_.is_null() {
            libc::snprintf(buf, len, c"(nil)".as_ptr());
            return;
        }
        let mut t: ObText = core::mem::MaybeUninit::uninit().assume_init();
        ob_text(str_, &mut t, c"the uncaught exception handler".as_ptr());
        libc::snprintf(buf, len, c"%s".as_ptr(), t.s);
        ob_text_free(&mut t);
    }
}

pub unsafe extern "C" fn ob_uncaught(exc: *mut c_void) {
    unsafe {
        let mut name: [c_char; 256] = core::mem::MaybeUninit::uninit().assume_init();
        let mut reason: [c_char; 1024] = core::mem::MaybeUninit::uninit().assume_init();
        let f = ocerz_bridge_in_flight();

        if !exc.is_null() {
            ob_exception_text(exc, c"name".as_ptr(), name.as_mut_ptr(), name.len());
            ob_exception_text(exc, c"reason".as_ptr(), reason.as_mut_ptr(), reason.len());
        } else {
            libc::snprintf(name.as_mut_ptr(), name.len(), c"(nil)".as_ptr());
            libc::snprintf(reason.as_mut_ptr(), reason.len(), c"(nil)".as_ptr());
        }
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: bridge: uncaught Objective-C exception %s: %s during %s\n".as_ptr(),
            name.as_ptr(),
            reason.as_ptr(),
            if !f.is_null() && !(*f).sym.is_null() { (*f).sym } else { c"(no bridged call)".as_ptr() },
        );
        libc::fflush(crate::log::stderr());

        let prev: Option<unsafe extern "C" fn(*mut c_void)> =
            core::mem::transmute(G_OB_NATIVE_PREV.load(Ordering::SeqCst));
        if let Some(prev) = prev {
            prev(exc);
        }
    }
}

unsafe extern "C" fn ob_install_once() {
    unsafe {
        let set = ob_sym(&raw const G_OB_SET_UNCAUGHT);
        if set.is_null() {
            crate::ocerz_log!("objc: the host libobjc has no objc_setUncaughtExceptionHandler\n");
            return;
        }
        let set: unsafe extern "C" fn(*mut c_void) -> *mut c_void = core::mem::transmute(set);
        let prev = set(ob_uncaught as *mut c_void);
        if prev != ob_uncaught as *mut c_void {
            G_OB_NATIVE_PREV.store(prev, Ordering::SeqCst);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objcbridge_install_uncaught() {
    unsafe {
        libc::pthread_once(&raw mut G_OB_UNCAUGHT_ONCE, Some(ob_install_once));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_setUncaughtExceptionHandler(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let guest = gpr(cpu, OCERZ_RDI);
        let set = ob_need(&raw const G_OB_SET_UNCAUGHT);
        let mut native = 0u64;

        ocerz_objcbridge_install_uncaught();
        if guest != 0
            && (ocerz_abi_callback_convert(guest, c"v(p)".as_ptr(), &mut native) != OCERZ_OK as c_int
                || native == 0)
        {
            ob_stop!(
                "objc_setUncaughtExceptionHandler could not bind guest handler %#llx to a callback",
                guest
            );
        }

        let mut outer: OcerzBridgeFrame = core::mem::MaybeUninit::uninit().assume_init();
        ocerz_bridge_raise(&mut outer, OCERZ_OBJC_LIBOBJC.as_ptr() as *const c_char, c"_objc_setUncaughtExceptionHandler".as_ptr(), c"p(c{v(p)})".as_ptr(), set);
        let setf: unsafe extern "C" fn(*mut c_void) = core::mem::transmute(set);
        setf(if guest != 0 { ocerz_g2h(native) } else { ob_uncaught as *mut c_void });
        ocerz_bridge_lower(&outer);

        G_OB_GUEST_HANDLER_NATIVE.store(if guest != 0 { ocerz_g2h(native) } else { null_mut() }, Ordering::SeqCst);
        ob_return(cpu, G_OB_GUEST_HANDLER.swap(guest, Ordering::SeqCst));
        ob_settle(vm, cpu)
    }
}

/* ---- the guest-side EH machinery ---- */

const OB_GUEST_CXXABI: &[u8] = b"/usr/lib/libc++abi.dylib\0";
const OB_EH_VTABLE_WORDS: usize = 10;
const OB_EH_OBJECT_BYTES: u64 = 32;
const OB_EH_NO_CLASS: u64 = 1;

#[repr(C)]
pub struct ObEh {
    pub alloc: u64,
    pub throw_fn: u64,
    pub rethrow: u64,
    pub begin_catch: u64,
    pub end_catch: u64,
    pub terminate: u64,
    pub set_terminate: u64,
    pub personality: u64,
    pub current_type: u64,
}

static mut G_OB_EH: ObEh = ObEh {
    alloc: 0,
    throw_fn: 0,
    rethrow: 0,
    begin_catch: 0,
    end_catch: 0,
    terminate: 0,
    set_terminate: 0,
    personality: 0,
    current_type: 0,
};
static G_OB_EH_READY: AtomicI32 = AtomicI32::new(0);
static G_OB_EH_MISSED_AT: AtomicU32 = AtomicU32::new(u32::MAX);
static mut G_OB_EH_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
static G_OB_EH_VTABLE_PAGE: AtomicU64 = AtomicU64::new(0);
static G_OB_EH_PREV_TERMINATE: AtomicU64 = AtomicU64::new(0);
static G_OB_EH_TERMINATE_SET: AtomicI32 = AtomicI32::new(0);

pub unsafe fn ob_eh() -> *const ObEh {
    unsafe {
        if G_OB_EH_READY.load(Ordering::SeqCst) != 0 {
            return &raw const G_OB_EH;
        }
        let gen_now = ocerz_dyld_generation();
        if G_OB_EH_MISSED_AT.load(Ordering::SeqCst) == gen_now {
            return null();
        }
        struct Want {
            sym: *const c_char,
            at: usize,
        }
        unsafe impl Sync for Want {}
        static WANT: &[Want] = &[
            Want { sym: c"___cxa_allocate_exception".as_ptr(), at: 0 },
            Want { sym: c"___cxa_throw".as_ptr(), at: 8 },
            Want { sym: c"___cxa_rethrow".as_ptr(), at: 16 },
            Want { sym: c"___cxa_begin_catch".as_ptr(), at: 24 },
            Want { sym: c"___cxa_end_catch".as_ptr(), at: 32 },
            Want { sym: c"__ZSt9terminatev".as_ptr(), at: 40 },
            Want { sym: c"__ZSt13set_terminatePFvvE".as_ptr(), at: 48 },
            Want { sym: c"___gxx_personality_v0".as_ptr(), at: 56 },
            Want { sym: c"___cxa_current_exception_type".as_ptr(), at: 64 },
        ];
        libc::pthread_mutex_lock(&raw mut G_OB_EH_LOCK);
        if G_OB_EH_READY.load(Ordering::SeqCst) == 0 {
            let mut e: ObEh = core::mem::zeroed();
            let mut ok = 1;
            let mut k = 0;
            while ok != 0 && k < WANT.len() {
                let mut found = 0;
                let a = ocerz_dyld_guest_export(OB_GUEST_CXXABI.as_ptr() as *const c_char, WANT[k].sym, &mut found);
                ok = (found != 0 && a != 0) as c_int;
                let dst = (&raw mut e).cast::<u8>().add(WANT[k].at) as *mut u64;
                *dst = a;
                k += 1;
            }
            if ok != 0 {
                G_OB_EH = e;
                G_OB_EH_READY.store(1, Ordering::SeqCst);
            } else {
                G_OB_EH_MISSED_AT.store(gen_now, Ordering::SeqCst);
            }
        }
        libc::pthread_mutex_unlock(&raw mut G_OB_EH_LOCK);
        if G_OB_EH_READY.load(Ordering::SeqCst) != 0 { &raw const G_OB_EH } else { null() }
    }
}

unsafe fn ob_eh_vtable_words(slot: *mut u8, size: u32) {
    unsafe {
        let no = ocerz_vdylib_trampoline(OCERZ_VDYLIB_TRAMP_OBJC_EH_FALSE);
        let words: [u64; OB_EH_VTABLE_WORDS] = [
            0,
            0,
            no,
            no,
            no,
            no,
            ocerz_vdylib_trampoline(OCERZ_VDYLIB_TRAMP_OBJC_EH_DO_CATCH),
            no,
            no,
            no,
        ];
        core::ptr::write_bytes(slot, 0, size as usize);
        libc::memcpy(
            slot as *mut c_void,
            words.as_ptr() as *const c_void,
            if (size as usize) < size_of_val(&words) { size as usize } else { size_of_val(&words) },
        );
    }
}

unsafe fn ob_eh_vtable() -> u64 {
    unsafe {
        let mut page = G_OB_EH_VTABLE_PAGE.load(Ordering::SeqCst);
        if page != 0 {
            return page;
        }
        libc::pthread_mutex_lock(&raw mut G_OB_EH_LOCK);
        page = G_OB_EH_VTABLE_PAGE.load(Ordering::SeqCst);
        if page == 0 {
            let made = ocerz_map_anywhere(OCERZ_GUEST_PAGE_SIZE as u64, libc::PROT_READ | libc::PROT_WRITE);
            if made != 0 {
                ob_eh_vtable_words(ocerz_g2h(made) as *mut u8, (OB_EH_VTABLE_WORDS * 8) as u32);
                if ocerz_protect(made, OCERZ_GUEST_PAGE_SIZE as u64, libc::PROT_READ) == OCERZ_OK as c_int {
                    page = made;
                } else {
                    ocerz_unmap(made, OCERZ_GUEST_PAGE_SIZE as u64);
                }
            }
            G_OB_EH_VTABLE_PAGE.store(page, Ordering::SeqCst);
        }
        libc::pthread_mutex_unlock(&raw mut G_OB_EH_LOCK);
        if page == 0 {
            ob_stop!("no guest page could be set up for the Objective-C exception type table");
        }
        page
    }
}

type ObHostSym = Option<unsafe extern "C" fn(*const c_char, *const c_char) -> *mut c_void>;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_fill_ehtype_vtable(
    slot: u64,
    size: u32,
    install_name: *const c_char,
    export_name: *const c_char,
    host_sym: ObHostSym,
) {
    unsafe {
        let _ = install_name;
        let _ = export_name;
        let _ = host_sym;
        ob_eh_vtable_words(ocerz_g2h(slot) as *mut u8, size);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_fill_ehtype(
    slot: u64,
    size: u32,
    install_name: *const c_char,
    export_name: *const c_char,
    host_sym: ObHostSym,
) {
    unsafe {
        let mut words = [ob_eh_vtable() + 16, 0u64, 0u64];
        if libc::strcmp(export_name, c"_OBJC_EHTYPE_id".as_ptr()) == 0 {
            words[1] = ocerz_h2g(c"id".as_ptr() as *const c_void);
        } else {
            let host = match host_sym {
                Some(h) => h(install_name, export_name.add(1)) as *const *const c_void,
                None => null(),
            };
            if !host.is_null() && !(*host.add(2)).is_null() {
                words[1] = if (*host.add(1)).is_null() { 0 } else { ocerz_h2g(*host.add(1)) };
                words[2] = ocerz_h2g(*host.add(2));
            } else {
                crate::ocerz_log!(
                    "objc: %s has no %s on this host, so a @catch naming it catches nothing\n",
                    install_name,
                    export_name
                );
                words[1] = ocerz_h2g(export_name.add(c"_OBJC_EHTYPE_$_".to_bytes_with_nul().len() - 1) as *const c_void);
                words[2] = OB_EH_NO_CLASS;
            }
        }
        libc::memcpy(
            ocerz_g2h(slot),
            words.as_ptr() as *const c_void,
            if (size as usize) < size_of_val(&words) { size as usize } else { size_of_val(&words) },
        );
    }
}

fn ob_eh_absent(sym: *const c_char) -> ! {
    unsafe {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: bridge: %s %s not implemented: Objective-C exceptions in native mode need the guest C++ runtime, which %s does not hold (run make guest-cxx)\n".as_ptr(),
            OCERZ_OBJC_LIBOBJC.as_ptr() as *const c_char,
            sym,
            c"the guest root".as_ptr(),
        );
        libc::fflush(crate::log::stderr());
        libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
    }
}

unsafe fn ob_eh_install_terminate(vm: *mut OcerzVM, eh: *const ObEh, stack_top: u64) {
    unsafe {
        if G_OB_EH_TERMINATE_SET
            .compare_exchange(0, 1, Ordering::SeqCst, Ordering::SeqCst)
            .is_err()
        {
            return;
        }
        let args = [ocerz_vdylib_trampoline(OCERZ_VDYLIB_TRAMP_OBJC_EH_TERMINATE)];
        G_OB_EH_PREV_TERMINATE.store(
            ocerz_vm_call(vm, (*eh).set_terminate, args.as_ptr(), 1, stack_top),
            Ordering::SeqCst,
        );
    }
}

pub unsafe fn ob_eh_throw(vm: *mut OcerzVM, cpu: *mut OcerzCPU, gobj: u64, owned: c_int) -> c_int {
    unsafe {
        let eh = ob_eh();
        if eh.is_null() {
            ob_eh_absent(c"_objc_exception_throw".as_ptr());
        }
        let stack_top = (gpr(cpu, OCERZ_RSP) - 256) & !0xf;
        ob_eh_install_terminate(vm, eh, stack_top);
        let pre = crate::ported::objcbridge::send::ob_guest_preprocessor();
        let mut gobj = gobj;
        if pre != 0 && owned == 0 {
            let args = [gobj];
            gobj = ocerz_vm_call(vm, pre, args.as_ptr(), 1, stack_top);
        }
        let obj = if gobj != 0 { ocerz_g2h(gobj) } else { null_mut() };
        if !obj.is_null() && owned == 0 {
            let retain: unsafe extern "C" fn(*mut c_void) -> *mut c_void =
                core::mem::transmute(ob_need(&raw const G_OB_RETAIN));
            retain(obj);
        }
        let size = [OB_EH_OBJECT_BYTES];
        let exc = ocerz_vm_call(vm, (*eh).alloc, size.as_ptr(), 1, stack_top);
        let cls = if obj.is_null() { null_mut() } else { ob_object_get_class(obj) };
        ocerz_st(exc, 8, gobj);
        ocerz_st(exc + 8, 8, ob_eh_vtable() + 16);
        ocerz_st(exc + 16, 8, ocerz_h2g(if cls.is_null() { c"nil".as_ptr() as *const c_void } else { ob_class_get_name(cls) as *const c_void }));
        ocerz_st(exc + 24, 8, if cls.is_null() { 0 } else { ocerz_h2g(cls) });
        set_gpr(cpu, OCERZ_RDI, exc);
        set_gpr(cpu, OCERZ_RSI, exc + 8);
        set_gpr(cpu, OCERZ_RDX, ocerz_vdylib_trampoline(OCERZ_VDYLIB_TRAMP_OBJC_EH_DESTROY));
        (*cpu).rip = (*eh).throw_fn;
        ob_settle(vm, cpu)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_exception_throw(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_eh_throw(vm, cpu, gpr(cpu, OCERZ_RDI), 0) }
}

unsafe fn ob_eh_jump(vm: *mut OcerzVM, cpu: *mut OcerzCPU, at: usize, sym: *const c_char) -> c_int {
    unsafe {
        let eh = ob_eh();
        if eh.is_null() {
            ob_eh_absent(sym);
        }
        let target = *(eh.cast::<u8>().add(at) as *const u64);
        (*cpu).rip = target;
        ob_settle(vm, cpu)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_exception_rethrow(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_eh_jump(vm, cpu, core::mem::offset_of!(ObEh, rethrow), c"_objc_exception_rethrow".as_ptr()) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_begin_catch(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_eh_jump(vm, cpu, core::mem::offset_of!(ObEh, begin_catch), c"_objc_begin_catch".as_ptr()) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_end_catch(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_eh_jump(vm, cpu, core::mem::offset_of!(ObEh, end_catch), c"_objc_end_catch".as_ptr()) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_terminate(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_eh_jump(vm, cpu, core::mem::offset_of!(ObEh, terminate), c"_objc_terminate".as_ptr()) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_personality_v0(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_eh_jump(vm, cpu, core::mem::offset_of!(ObEh, personality), c"___objc_personality_v0".as_ptr()) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_eh_false(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        ob_return(cpu, 0);
        ob_settle(vm, cpu)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_eh_do_catch(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let catch_ti = gpr(cpu, OCERZ_RDI);
        let throw_ti = gpr(cpu, OCERZ_RSI);
        let objp = gpr(cpu, OCERZ_RDX);
        let mut caught = 0u64;
        if throw_ti != 0 && objp != 0 && ocerz_ld(throw_ti, 8) == ob_eh_vtable() + 16 {
            let thrown = ocerz_ld(objp, 8);
            let gobj = if thrown != 0 { ocerz_ld(thrown, 8) } else { 0 };
            let want = if catch_ti != 0 { ocerz_ld(catch_ti + 16, 8) } else { 0 };
            if want == 0 {
                caught = 1;
            } else if want != OB_EH_NO_CLASS && gobj != 0 {
                let wanted = ocerz_g2h(want);
                let mut c = ob_object_get_class(ocerz_g2h(gobj));
                while !c.is_null() && caught == 0 {
                    caught = (c == wanted) as u64;
                    c = ob_class_get_superclass(c);
                }
            }
            if caught != 0 {
                ocerz_st(objp, 8, gobj);
            }
        }
        ob_return(cpu, caught);
        ob_settle(vm, cpu)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_eh_destroy(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let thrown = gpr(cpu, OCERZ_RDI);
        let gobj = if thrown != 0 { ocerz_ld(thrown, 8) } else { 0 };
        if gobj != 0 {
            let release: unsafe extern "C" fn(*mut c_void) =
                core::mem::transmute(ob_need(&raw const G_OB_RELEASE));
            release(ocerz_g2h(gobj));
        }
        ob_return(cpu, 0);
        ob_settle(vm, cpu)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_eh_terminate(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let eh = ob_eh();
        let stack_top = (gpr(cpu, OCERZ_RSP) - 256) & !0xf;
        let ti = if eh.is_null() { 0 } else { ocerz_vm_call(vm, (*eh).current_type, null(), 0, stack_top) };
        if ti != 0 && ocerz_ld(ti, 8) == ob_eh_vtable() + 16 {
            let gobj = ocerz_ld(ti - 8, 8);
            let obj = if gobj != 0 { ocerz_g2h(gobj) } else { null_mut() };
            let handler: Option<unsafe extern "C" fn(*mut c_void)> =
                core::mem::transmute(G_OB_GUEST_HANDLER_NATIVE.load(Ordering::SeqCst));
            if let Some(h) = handler {
                h(obj);
            } else {
                ob_uncaught(obj);
            }
            libc::fprintf(
                crate::log::stderr(),
                c"libc++abi: terminating due to uncaught exception of type %s\n".as_ptr(),
                if obj.is_null() { c"nil".as_ptr() } else { ob_class_get_name(ob_object_get_class(obj)) },
            );
            libc::fflush(crate::log::stderr());
            libc::abort();
        }
        let prev = G_OB_EH_PREV_TERMINATE.load(Ordering::SeqCst);
        if prev == 0 {
            libc::abort();
        }
        (*cpu).rip = prev;
        ob_settle(vm, cpu)
    }
}

/* ---- the guard: personality and landing ----
 *
 * Every Rust function on the unwind path (ob_guarded_body, ob_send_via,
 * ob_var_bindings_body, ob_perform and down to ocerz_abi_call_native) is a
 * plain extern "C"/Rust fn with no Drop types live and, compiled under the
 * crate's panic=abort profile, no personality or LSDA in its FDE — verified
 * with dwarfdump against the C build's objcbridge.o. */

#[thread_local]
static mut G_OB_GUARD_PASSING: *mut UnwindException = null_mut();

/// The personality ocerz_objc_guarded's CFI names.  Exact _Unwind signature;
/// returns _URC codes.  No panics, no drops.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_guard_personality(
    version: c_int,
    actions: UnwindAction,
    exception_class: u64,
    ue: *mut UnwindException,
    context: *mut UnwindContext,
) -> UnwindReasonCode {
    unsafe {
        let _ = exception_class;
        if version != 1 || (actions & UA_FORCE_UNWIND) != 0 || ue == G_OB_GUARD_PASSING {
            return URC_CONTINUE_UNWIND;
        }
        if (actions & UA_SEARCH_PHASE) != 0 {
            return URC_HANDLER_FOUND;
        }
        if (actions & UA_HANDLER_FRAME) == 0 {
            return URC_CONTINUE_UNWIND;
        }
        _Unwind_SetGR(context, 0, ue as usize);
        _Unwind_SetIP(context, ocerz_objc_guard_pad);
        URC_INSTALL_CONTEXT
    }
}

unsafe fn ob_cxxabi(name: *const c_char) -> *mut c_void {
    unsafe {
        let a = dlsym(RTLD_DEFAULT, name);
        if a.is_null() {
            ob_stop!("the native C++ runtime has no %s, which catching a native exception needs", name);
        }
        a
    }
}

/// Called from ocerz_objc_guarded's landing pad with the exception in ue and
/// the caught-object out-pointer in caught.  Returns 1 when the exception was
/*/ an Objective-C object ocerz can throw on into the guest; otherwise rethrows. */
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_guard_landed(ue: *mut UnwindException, caught: *mut *mut c_void) -> c_int {
    unsafe {
        let begin: unsafe extern "C" fn(*mut c_void) -> *mut c_void =
            core::mem::transmute(ob_cxxabi(c"__cxa_begin_catch".as_ptr()));
        let end: unsafe extern "C" fn() = core::mem::transmute(ob_cxxabi(c"__cxa_end_catch".as_ptr()));
        let type_of: unsafe extern "C" fn() -> *const *const c_void =
            core::mem::transmute(ob_cxxabi(c"__cxa_current_exception_type".as_ptr()));
        let rethrow: unsafe extern "C" fn() = core::mem::transmute(ob_cxxabi(c"__cxa_rethrow".as_ptr()));
        let vtable = ob_need(&raw const G_OB_EHTYPE_VTABLE) as *const *const c_void;
        begin(ue as *mut c_void);
        let type_ = type_of();
        if !type_.is_null() && *type_ == vtable.add(2) as *const c_void {
            let obj = *(ue.add(1) as *mut *mut c_void);
            if !obj.is_null() {
                let retain: unsafe extern "C" fn(*mut c_void) -> *mut c_void =
                    core::mem::transmute(ob_need(&raw const G_OB_RETAIN));
                retain(obj);
            }
            end();
            *caught = obj;
            return 1;
        }
        G_OB_GUARD_PASSING = ue;
        rethrow();
        libc::abort();
    }
}

/* _NSDictionaryOfVariableBindings(keys, first, ...), which the
   NSDictionaryOfVariableBindings macro calls with one value per
   comma-separated key and a nil after them.  Its variadic values go to the
   host on arm64's stack, one per key after the first, up to and including the
   first nil, which is as far as the host reads: it raises when a key's value is
   nil, and that exception is thrown on into the guest. */

#[repr(C)]
struct ObVarBindings {
    f: *const c_void,
    call: *mut OcerzAbiCall,
    slots: *const u64,
    nslots: c_int,
}

unsafe extern "C" fn ob_var_bindings_body(ctx: *mut c_void) {
    unsafe {
        let b = ctx as *mut ObVarBindings;
        ocerz_abi_call_native(
            (*b).f,
            (*(*b).call).x.as_ptr(),
            (*(*b).call).v.as_ptr(),
            (*b).slots,
            8 * (*b).nslots as u64,
            null_mut(),
            (*(*b).call).rx.as_mut_ptr(),
            (*(*b).call).rv.as_mut_ptr(),
        );
    }
}

unsafe extern "C" {
    fn ocerz_objc_guarded(body: unsafe extern "C" fn(*mut c_void), ctx: *mut c_void, caught: *mut *mut c_void) -> c_int;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_dictionary_of_variable_bindings(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        static SYM: ObSym = obsym!(OCERZ_OBJC_FOUNDATION, "_NSDictionaryOfVariableBindings");
        let f = ob_need(&raw const SYM);
        let mut named: OcerzAbiSig = core::mem::MaybeUninit::uninit().assume_init();
        let mut call: OcerzAbiCall = core::mem::MaybeUninit::uninit().assume_init();
        let mut va: OcerzAbiVaList = core::mem::MaybeUninit::uninit().assume_init();
        if ocerz_abi_parse(c"p(pp)".as_ptr(), &mut named) != OCERZ_OK as c_int
            || ocerz_abi_read_guest(&named, cpu, &mut call) != OCERZ_OK as c_int
            || ocerz_abi_va_start(&named, cpu, &mut va) != OCERZ_OK as c_int
        {
            ob_stop!("_NSDictionaryOfVariableBindings could not read its arguments");
        }
        let mut keys: ObText = core::mem::MaybeUninit::uninit().assume_init();
        ob_text(call.x[0] as *mut c_void, &mut keys, c"_NSDictionaryOfVariableBindings".as_ptr());
        let mut nkeys = 1;
        let mut c = keys.s;
        while *c != 0 {
            nkeys += (*c == b',' as c_char) as c_int;
            c = c.add(1);
        }
        ob_text_free(&mut keys);
        let mut slots: [u64; OCERZ_OBJC_VARIADIC_MAX as usize] = core::mem::MaybeUninit::uninit().assume_init();
        let mut n = 0;
        let mut ended = call.x[1] == 0;
        while !ended && n + 1 < nkeys as usize {
            if n == OCERZ_OBJC_VARIADIC_MAX as usize - 1 {
                ob_stop!(
                    "_NSDictionaryOfVariableBindings names more than %d keys",
                    OCERZ_OBJC_VARIADIC_MAX as c_int - 1
                );
            }
            let mut w = 0u64;
            ocerz_abi_va_arg(&mut va, cpu, 'p' as c_char, &mut w);
            slots[n] = if w != 0 { ocerz_g2h(w) as u64 } else { 0 };
            n += 1;
            ended = w == 0;
        }
        slots[n] = 0;
        n += 1;
        let mut outer: OcerzBridgeFrame = core::mem::MaybeUninit::uninit().assume_init();
        ocerz_bridge_raise(&mut outer, OCERZ_OBJC_FOUNDATION.as_ptr() as *const c_char, c"__NSDictionaryOfVariableBindings".as_ptr(), c"p(pp)".as_ptr(), f);
        let mut b = ObVarBindings { f, call: &mut call, slots: slots.as_ptr(), nslots: n as c_int };
        let mut raised: *mut c_void = null_mut();
        if !ob_eh().is_null() {
            if ocerz_objc_guarded(ob_var_bindings_body, &mut b as *mut _ as *mut c_void, &mut raised) != 0 {
                ocerz_bridge_lower(&outer);
                return ob_eh_throw(vm, cpu, if raised.is_null() { 0 } else { ocerz_h2g(raised) }, 1);
            }
        } else {
            ob_var_bindings_body(&mut b as *mut _ as *mut c_void);
        }
        ocerz_bridge_lower(&outer);
        ocerz_abi_write_result(&named, cpu, &mut call);
        ob_settle(vm, cpu)
    }
}

/* NSGetUncaughtExceptionHandler answers the function
   NSSetUncaughtExceptionHandler installed: the guest's own when the native
   one is ocerz's callback onto it, and otherwise a thunk the guest can
   call. */
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_NSGetUncaughtExceptionHandler(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        static FN: ObSym = obsym!(OCERZ_OBJC_FOUNDATION, "NSGetUncaughtExceptionHandler");
        let f: unsafe extern "C" fn() -> *mut c_void = core::mem::transmute(ob_need(&raw const FN));
        let handler = f();
        let mut guest = 0u64;
        if !handler.is_null()
            && !(!ocerz_abi_callback_sig(handler, &mut guest).is_null() && guest != 0)
        {
            guest = ocerz_bridge_native_thunk(handler, c"(uncaught exception handler)".as_ptr(), c"v(p)".as_ptr());
        }
        ob_return(cpu, guest);
        ob_settle(vm, cpu)
    }
}

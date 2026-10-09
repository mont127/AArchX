//! Symbols, refusal, and the shared helpers: every host runtime function the
//! crossings use is reached through an ObSym resolved lazily by name out of
//! the host library, and every refusal prints "ocerz: bridge: " plus the
//! message to stderr and exits OCERZ_BRIDGE_UNIMPL_EXIT.

use core::ffi::{c_char, c_int, c_void};
use core::sync::atomic::{AtomicPtr, Ordering};

use crate::ffi::*;
pub use crate::ported::bridge::common::{br_settle as ob_settle, br_return as ob_return, gpr, set_gpr, ocerz_ld, ocerz_st, ocerz_g2h, ocerz_h2g};

#[repr(C)]
pub struct ObSym {
    pub lib: *const c_char,
    pub name: *const c_char,
    pub addr: AtomicPtr<c_void>,
}
unsafe impl Sync for ObSym {}

macro_rules! obsym {
    ($lib:expr, $name:literal) => {
        $crate::ported::objcbridge::common::ObSym { lib: $lib.as_ptr() as *const core::ffi::c_char, name: concat!($name, "\0").as_ptr() as *const core::ffi::c_char, addr: core::sync::atomic::AtomicPtr::new(core::ptr::null_mut()) }
    };
}
pub(crate) use obsym;

pub static G_OB_MSGSEND: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "objc_msgSend");
pub static G_OB_MSGSEND_SUPER: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "objc_msgSendSuper");
pub static G_OB_MSGSEND_SUPER2: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "objc_msgSendSuper2");
pub static G_OB_SEL_REGISTER_NAME: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "sel_registerName");
pub static G_OB_SEL_GET_NAME: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "sel_getName");
pub static G_OB_OBJECT_GET_CLASS: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "object_getClass");
pub static G_OB_CLASS_GET_INSTANCE_METHOD: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "class_getInstanceMethod");
pub static G_OB_METHOD_GET_TYPE_ENCODING: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "method_getTypeEncoding");
pub static G_OB_CLASS_GET_NAME: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "class_getName");
pub static G_OB_CLASS_IS_META_CLASS: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "class_isMetaClass");
pub static G_OB_CLASS_GET_SUPERCLASS: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "class_getSuperclass");
pub static G_OB_CLASS_RESPONDS_TO_SELECTOR: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "class_respondsToSelector");
pub static G_OB_SET_UNCAUGHT: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "objc_setUncaughtExceptionHandler");
pub static G_OB_CLASS_ADD_METHOD: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "class_addMethod");
pub static G_OB_ALLOCATE_CLASS_PAIR: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "objc_allocateClassPair");
pub static G_OB_CLASS_COPY_METHOD_LIST: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "class_copyMethodList");
pub static G_OB_METHOD_GET_NAME: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "method_getName");
pub static G_OB_METHOD_SET_IMPLEMENTATION: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "method_setImplementation");
pub static G_OB_METHOD_GET_IMPLEMENTATION: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "method_getImplementation");
pub static G_OB_SET_EXCEPTION_PREPROCESSOR: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "objc_setExceptionPreprocessor");
pub static G_OB_CLASS_REPLACE_METHOD: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "class_replaceMethod");
pub static G_OB_CLASS_GET_METHOD_IMPLEMENTATION: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "class_getMethodImplementation");
pub static G_OB_REALIZE_CLASS_FROM_SWIFT: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "_objc_realizeClassFromSwift");
pub static G_OB_READ_CLASS_PAIR: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "objc_readClassPair");
pub static G_OB_SET_HOOK_GET_CLASS: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "objc_setHook_getClass");
pub static G_OB_SET_HOOK_GET_IMAGE_NAME: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "objc_setHook_getImageName");
pub static G_OB_SET_HOOK_LAZY_CLASS_NAMER: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "objc_setHook_lazyClassNamer");
pub static G_OB_OPT_SELF: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "objc_opt_self");
pub static G_OB_OPT_CLASS: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "objc_opt_class");
pub static G_OB_ALLOC: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "objc_alloc");
pub static G_OB_ALLOC_INIT: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "objc_alloc_init");
pub static G_OB_ALLOC_WITH_ZONE: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "objc_allocWithZone");
pub static G_OB_RELEASE: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "objc_release");
pub static G_OB_RETAIN: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "objc_retain");
pub static G_OB_EHTYPE_VTABLE: ObSym = obsym!(OCERZ_OBJC_LIBOBJC, "objc_ehtype_vtable");
pub static G_OB_CFSTRING_GET_LENGTH: ObSym = obsym!(OCERZ_BRIDGE_COREFOUNDATION, "CFStringGetLength");
pub static G_OB_CFSTRING_GET_MAXIMUM_SIZE: ObSym = obsym!(OCERZ_BRIDGE_COREFOUNDATION, "CFStringGetMaximumSizeForEncoding");
pub static G_OB_CFSTRING_GET_CSTRING: ObSym = obsym!(OCERZ_BRIDGE_COREFOUNDATION, "CFStringGetCString");
pub static G_OB_CFSTRING_CREATE_WITH_CSTRING: ObSym = obsym!(OCERZ_BRIDGE_COREFOUNDATION, "CFStringCreateWithCString");
pub static G_OB_CFRELEASE: ObSym = obsym!(OCERZ_BRIDGE_COREFOUNDATION, "CFRelease");

pub unsafe fn ob_sym(s: *const ObSym) -> *mut c_void {
    unsafe {
        let mut a = (*s).addr.load(Ordering::SeqCst);
        if a.is_null() {
            a = ocerz_bridge_host_symbol((*s).lib, (*s).name);
            if !a.is_null() {
                (*s).addr.store(a, Ordering::SeqCst);
            }
        }
        a
    }
}

/// A refusal: "ocerz: bridge: " + the message + newline, then
/// OCERZ_BRIDGE_UNIMPL_EXIT, byte-identical to C's ob_stop.
macro_rules! ob_stop {
    ($fmt:literal $(, $arg:expr)*) => {{
        unsafe {
            libc::fputs(c"ocerz: bridge: ".as_ptr(), $crate::log::stderr());
            libc::fprintf($crate::log::stderr(), concat!($fmt, "\0").as_ptr() as *const i8 $(, $arg)*);
            libc::fputc('\n' as i32, $crate::log::stderr());
            libc::fflush($crate::log::stderr());
            libc::exit($crate::ffi::OCERZ_BRIDGE_UNIMPL_EXIT as i32);
        }
    }};
}
pub(crate) use ob_stop;

/// ob_refuse: like ob_stop but prefixed "ocerz: bridge: +[Class sel] " or
/// "-[...]", resolved through the runtime.
macro_rules! ob_refuse {
    ($cls:expr, $sel:expr, $fmt:literal $(, $arg:expr)*) => {{
        unsafe {
            let cls = $cls;
            let sel = $sel;
            let metacls: unsafe extern "C" fn(*mut core::ffi::c_void) -> bool =
                core::mem::transmute($crate::ported::objcbridge::common::ob_need(
                    &raw const $crate::ported::objcbridge::common::G_OB_CLASS_IS_META_CLASS));
            let getname: unsafe extern "C" fn(*mut core::ffi::c_void) -> *const i8 =
                core::mem::transmute($crate::ported::objcbridge::common::ob_need(
                    &raw const $crate::ported::objcbridge::common::G_OB_CLASS_GET_NAME));
            let selname: unsafe extern "C" fn(*mut core::ffi::c_void) -> *const i8 =
                core::mem::transmute($crate::ported::objcbridge::common::ob_need(
                    &raw const $crate::ported::objcbridge::common::G_OB_SEL_GET_NAME));
            libc::fprintf(
                $crate::log::stderr(),
                c"ocerz: bridge: %c[%s %s] ".as_ptr(),
                if !cls.is_null() && metacls(cls) { '+' as i32 } else { '-' as i32 },
                if cls.is_null() { c"nil".as_ptr() } else { getname(cls) },
                if sel.is_null() { c"(null selector)".as_ptr() } else { selname(sel) },
            );
            libc::fprintf($crate::log::stderr(), concat!($fmt, "\0").as_ptr() as *const i8 $(, $arg)*);
            libc::fputc('\n' as i32, $crate::log::stderr());
            libc::fflush($crate::log::stderr());
            libc::exit($crate::ffi::OCERZ_BRIDGE_UNIMPL_EXIT as i32);
        }
    }};
}
pub(crate) use ob_refuse;

pub unsafe fn ob_need(s: *const ObSym) -> *mut c_void {
    unsafe {
        let a = ob_sym(s);
        if a.is_null() {
            ob_stop!("the host %s has no %s, which crossing this call needs", (*s).lib, (*s).name);
        }
        a
    }
}

pub unsafe fn ob_object_get_class(obj: *mut c_void) -> *mut c_void {
    unsafe {
        let f: unsafe extern "C" fn(*mut c_void) -> *mut c_void =
            core::mem::transmute(ob_need(&raw const G_OB_OBJECT_GET_CLASS));
        f(obj)
    }
}
pub unsafe fn ob_class_get_instance_method(cls: *mut c_void, sel: *mut c_void) -> *mut c_void {
    unsafe {
        let f: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void =
            core::mem::transmute(ob_need(&raw const G_OB_CLASS_GET_INSTANCE_METHOD));
        f(cls, sel)
    }
}
pub unsafe fn ob_method_get_type_encoding(m: *mut c_void) -> *const c_char {
    unsafe {
        let f: unsafe extern "C" fn(*mut c_void) -> *const c_char =
            core::mem::transmute(ob_need(&raw const G_OB_METHOD_GET_TYPE_ENCODING));
        f(m)
    }
}
pub unsafe fn ob_sel_get_name(sel: *mut c_void) -> *const c_char {
    unsafe {
        let f: unsafe extern "C" fn(*mut c_void) -> *const c_char =
            core::mem::transmute(ob_need(&raw const G_OB_SEL_GET_NAME));
        f(sel)
    }
}
pub unsafe fn ob_sel_register_name(name: *const c_char) -> *mut c_void {
    unsafe {
        let f: unsafe extern "C" fn(*const c_char) -> *mut c_void =
            core::mem::transmute(ob_need(&raw const G_OB_SEL_REGISTER_NAME));
        f(name)
    }
}
pub unsafe fn ob_class_get_name(cls: *mut c_void) -> *const c_char {
    unsafe {
        let f: unsafe extern "C" fn(*mut c_void) -> *const c_char =
            core::mem::transmute(ob_need(&raw const G_OB_CLASS_GET_NAME));
        f(cls)
    }
}
pub unsafe fn ob_class_is_meta_class(cls: *mut c_void) -> bool {
    unsafe {
        let f: unsafe extern "C" fn(*mut c_void) -> bool =
            core::mem::transmute(ob_need(&raw const G_OB_CLASS_IS_META_CLASS));
        f(cls)
    }
}
pub unsafe fn ob_class_get_superclass(cls: *mut c_void) -> *mut c_void {
    unsafe {
        let f: unsafe extern "C" fn(*mut c_void) -> *mut c_void =
            core::mem::transmute(ob_need(&raw const G_OB_CLASS_GET_SUPERCLASS));
        f(cls)
    }
}
pub unsafe fn ob_class_responds_to_selector(cls: *mut c_void, sel: *mut c_void) -> bool {
    unsafe {
        let f: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool =
            core::mem::transmute(ob_need(&raw const G_OB_CLASS_RESPONDS_TO_SELECTOR));
        f(cls, sel)
    }
}

pub fn ob_logging() -> c_int {
    static EN: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
    let mut en = EN.load(Ordering::Relaxed);
    if en < 0 {
        en = unsafe { !libc::getenv(c"OCERZ_OBJCLOG".as_ptr()).is_null() } as c_int;
        EN.store(en, Ordering::Relaxed);
    }
    en
}

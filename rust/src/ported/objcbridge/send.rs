//! ---- a send ----
//! x86-64's objc_msgSend takes the receiver in rdi and the selector in rsi, the
//! method's own arguments after them in the usual System V places, so the
//! method signature is exactly the rest of the crossing's signature.  It comes
//! from the native runtime: object_getClass of the receiver, then
//! class_getInstanceMethod of that class and the selector, and
//! method_getTypeEncoding.  The call itself goes to the host's objc_msgSend
//! under that signature with the receiver and selector as its first two pointer
//! arguments: native objc_msgSend leaves every argument register and the stack
//! untouched and jumps to the implementation, which is what makes one engine
//! crossing correct for every method.
//!
//! ---- results the two ABIs return differently, ----
//! ---- super, ----
//! ---- nil ----
//! A message to nil does nothing and answers zero.  rax, rdx, xmm0 and xmm1 are
//! zeroed, which covers every place System V returns a scalar or a small
//! structure.  A _stret send to nil sets rax to the result pointer as the ABI
//! requires, and zeroes the structure only when its size can be known.
//!
//! ---- forwarding ----
//! A selector with no method is forwarded natively, and forwarding still needs
//! the arguments where the signature puts them: forwardingTargetForSelector:
//! is asked (up to eight targets deep), then methodSignatureForSelector:.
//!
//! ---- variadic methods ----
//! The Foundation methods declared with an ellipsis are listed by selector in
//! g_ob_variadic; the variadic arguments are gathered through
//! ocerz_abi_va_start/va_arg and go on the native stack after the named ones.

use core::ffi::{c_char, c_int, c_void};
use core::ptr::{null, null_mut};
use core::sync::atomic::{AtomicPtr, AtomicU64, Ordering};

use crate::ffi::*;
use crate::ported::objcbridge::common::*;
use crate::ported::objcbridge::encode::*;
use crate::ported::objcbridge::imp::*;

const OB_SHAPE_BUCKETS: usize = 512;
const OB_SEND_BUCKETS: usize = 4096;
const OB_UTF8: u32 = 0x08000100;

#[repr(C)]
struct ObShape {
    next: *mut ObShape,
    sig: OcerzAbiSig,
    notation: [c_char; 0],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ObSend {
    next: *mut ObSend,
    cls: *mut c_void,
    sel: *mut c_void,
    shape: *const ObShape,
    variadic: *const OcerzObjcVariadic,
    blocks: u32,
    fnptrs: u32,
    objects: u32,
    generation: u64,
}

static G_OB_SHAPES: [AtomicPtr<ObShape>; OB_SHAPE_BUCKETS] =
    [const { AtomicPtr::new(null_mut()) }; OB_SHAPE_BUCKETS];
static G_OB_SENDS: [AtomicPtr<ObSend>; OB_SEND_BUCKETS] =
    [const { AtomicPtr::new(null_mut()) }; OB_SEND_BUCKETS];
static mut G_OB_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
pub static G_OB_GENERATION: AtomicU64 = AtomicU64::new(0);

unsafe fn ob_str_hash(s: *const c_char) -> u32 {
    unsafe {
        let mut h = 2166136261u32;
        let mut p = s;
        while *p != 0 {
            h = (h ^ *p as u8 as u32).wrapping_mul(16777619);
            p = p.add(1);
        }
        h
    }
}

unsafe fn ob_send_hash(cls: *mut c_void, sel: *mut c_void) -> u32 {
    let k = ((cls as u64) >> 3) ^ ((sel as u64) >> 3).wrapping_mul(0x9e3779b97f4a7c15);
    ((k ^ (k >> 29)) & (OB_SEND_BUCKETS as u64 - 1)) as u32
}

unsafe fn ob_shape(notation: *const c_char) -> *const ObShape {
    unsafe {
        let b = (ob_str_hash(notation) & (OB_SHAPE_BUCKETS as u32 - 1)) as usize;
        let mut s = (*G_OB_SHAPES.as_ptr().add(b)).load(Ordering::SeqCst);
        while !s.is_null() {
            if libc::strcmp((*s).notation.as_ptr(), notation) == 0 {
                return s;
            }
            s = (*s).next;
        }
        let len = libc::strlen(notation);
        let made = libc::calloc(1, size_of::<ObShape>() + len + 1) as *mut ObShape;
        if made.is_null() {
            return null();
        }
        libc::memcpy((*made).notation.as_mut_ptr() as *mut c_void, notation as *const c_void, len + 1);
        if ocerz_abi_parse((*made).notation.as_ptr(), &mut (*made).sig) != OCERZ_OK as c_int {
            libc::free(made as *mut c_void);
            return null();
        }
        libc::pthread_mutex_lock(&raw mut G_OB_LOCK);
        let mut found: *mut ObShape = null_mut();
        let mut s = (*G_OB_SHAPES.as_ptr().add(b)).load(Ordering::SeqCst);
        while !s.is_null() && found.is_null() {
            if libc::strcmp((*s).notation.as_ptr(), notation) == 0 {
                found = s;
            }
            s = (*s).next;
        }
        if found.is_null() {
            (*made).next = (*G_OB_SHAPES.as_ptr().add(b)).load(Ordering::SeqCst);
            (*(&raw const G_OB_SHAPES).cast::<AtomicPtr<ObShape>>().add(b)).store(made, Ordering::SeqCst);
            found = made;
        }
        libc::pthread_mutex_unlock(&raw mut G_OB_LOCK);
        if found != made {
            libc::free(made as *mut c_void);
        }
        found
    }
}

unsafe fn ob_cached(cls: *mut c_void, sel: *mut c_void) -> *const ObSend {
    unsafe {
        let generation = G_OB_GENERATION.load(Ordering::SeqCst);
        let mut e = (*G_OB_SENDS.as_ptr().add(ob_send_hash(cls, sel) as usize)).load(Ordering::SeqCst);
        while !e.is_null() {
            if (*e).cls == cls && (*e).sel == sel && (*e).generation == generation {
                return e;
            }
            e = (*e).next;
        }
        null()
    }
}

unsafe fn ob_remember(scratch: *const ObSend) -> *const ObSend {
    unsafe {
        let b = ob_send_hash((*scratch).cls, (*scratch).sel) as usize;
        let made = libc::malloc(size_of::<ObSend>()) as *mut ObSend;
        if made.is_null() {
            return scratch;
        }
        *made = *scratch;
        libc::pthread_mutex_lock(&raw mut G_OB_LOCK);
        let mut found: *mut ObSend = null_mut();
        let mut e = (*G_OB_SENDS.as_ptr().add(b)).load(Ordering::SeqCst);
        while !e.is_null() && found.is_null() {
            if (*e).cls == (*scratch).cls && (*e).sel == (*scratch).sel
                && (*e).generation == (*scratch).generation
            {
                found = e;
            }
            e = (*e).next;
        }
        if found.is_null() {
            (*made).next = (*G_OB_SENDS.as_ptr().add(b)).load(Ordering::SeqCst);
            (*(&raw const G_OB_SENDS).cast::<AtomicPtr<ObSend>>().add(b)).store(made, Ordering::SeqCst);
            found = made;
        }
        libc::pthread_mutex_unlock(&raw mut G_OB_LOCK);
        if found != made {
            libc::free(made as *mut c_void);
        }
        found
    }
}

pub unsafe fn ob_describe(
    cls: *mut c_void,
    sel: *mut c_void,
    enc: *const c_char,
    source: *const c_char,
    out: *mut ObSend,
) {
    unsafe {
        let mut notation: [c_char; OCERZ_OBJC_NOTATION_MAX as usize] = core::mem::MaybeUninit::uninit().assume_init();
        let mut nargs = 0;
        let mut blocks = 0u32;
        let mut fnptrs = 0u32;
        let mut objects = 0u32;
        let rc = ob_notation(
            enc,
            notation.as_mut_ptr(),
            notation.len(),
            &mut nargs,
            &mut blocks,
            &mut fnptrs,
            &mut objects,
        );
        if rc != OCERZ_OBJC_OK as c_int {
            ob_refuse!(
                cls,
                sel,
                "cannot cross: its %s type encoding %s has %s",
                source,
                if enc.is_null() { c"(none)".as_ptr() } else { enc },
                crate::ported::objcbridge::encode::ocerz_objc_refusal(rc)
            );
        }
        let mut withfn: [c_char; OCERZ_OBJC_NOTATION_MAX as usize] = core::mem::MaybeUninit::uninit().assume_init();
        let use_ = if ob_fnarg_notation(
            ob_sel_get_name(sel),
            notation.as_ptr(),
            &mut fnptrs,
            withfn.as_mut_ptr(),
            withfn.len(),
        ) != 0
        {
            withfn.as_ptr()
        } else {
            notation.as_ptr()
        };
        let shape = ob_shape(use_);
        if shape.is_null() {
            ob_refuse!(
                cls,
                sel,
                "cannot cross: its %s type encoding %s gives the notation %s, which the ABI engine refuses",
                source,
                enc,
                use_
            );
        }
        let mut v = crate::ported::objcbridge::encode::ocerz_objc_variadic(ob_sel_get_name(sel));
        if !v.is_null() {
            let sig = &(*shape).sig;
            let arg = if (*v).kind == OCERZ_OBJC_VA_NIL_TERMINATED as c_int {
                sig.nargs - 1
            } else {
                (*v).arg
            };
            if arg < 2 || arg >= sig.nargs || sig.arg[arg as usize] != b'p' as c_char {
                v = null();
            }
            if !v.is_null() && (*v).kind == OCERZ_OBJC_VA_LIST as c_int {
                for k in 0..=(*v).va {
                    if (*v).va > 5
                        || (*v).va >= sig.nargs
                        || !libc::strchr(c"fd{D".as_ptr(), *sig.arg.as_ptr().add(k as usize) as c_int).is_null()
                        || (k == (*v).va && *sig.arg.as_ptr().add(k as usize) != b'p' as c_char)
                    {
                        v = null();
                        break;
                    }
                }
            }
        }
        core::ptr::write_bytes(out, 0, 1);
        (*out).cls = cls;
        (*out).sel = sel;
        (*out).shape = shape;
        (*out).variadic = v;
        (*out).blocks = blocks;
        (*out).fnptrs = fnptrs;
        (*out).objects = objects;
    }
}

unsafe fn ob_method(cls: *mut c_void, sel: *mut c_void, scratch: *mut ObSend) -> *const ObSend {
    unsafe {
        let e = ob_cached(cls, sel);
        if !e.is_null() {
            return e;
        }
        let generation = G_OB_GENERATION.load(Ordering::SeqCst);
        let m = ob_class_get_instance_method(cls, sel);
        if m.is_null() {
            return null();
        }
        ob_describe(cls, sel, ob_method_get_type_encoding(m), c"method".as_ptr(), scratch);
        (*scratch).generation = generation;
        ob_remember(scratch)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_allocateClassPair(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let f = ob_need(&raw const G_OB_ALLOCATE_CLASS_PAIR);
        let mut outer: OcerzBridgeFrame = core::mem::MaybeUninit::uninit().assume_init();
        ocerz_bridge_raise(&mut outer, OCERZ_OBJC_LIBOBJC.as_ptr() as *const c_char, c"_objc_allocateClassPair".as_ptr(), c"p(ppL)".as_ptr(), f);
        let f: unsafe extern "C" fn(*mut c_void, *const c_char, usize) -> *mut c_void =
            core::mem::transmute(f);
        let cls = f(
            if gpr(cpu, OCERZ_RDI) != 0 { ocerz_g2h(gpr(cpu, OCERZ_RDI)) } else { null_mut() },
            ocerz_g2h(gpr(cpu, OCERZ_RSI)) as *const c_char,
            gpr(cpu, OCERZ_RDX) as usize,
        );
        if !cls.is_null() {
            G_OB_GENERATION.fetch_add(1, Ordering::SeqCst);
        }
        ocerz_bridge_lower(&outer);
        ob_return(cpu, if cls.is_null() { 0 } else { ocerz_h2g(cls) });
        ob_settle(vm, cpu)
    }
}

unsafe fn ob_imp_from_guest_or_bind(
    imp: u64,
    notation: *const c_char,
    cls: *mut c_void,
    sel: *mut c_void,
    who: *const c_char,
) -> *mut c_void {
    unsafe {
        let back = crate::ported::objcbridge::imp::ocerz_objc_imp_from_guest(imp);
        if !back.is_null() {
            return back;
        }
        let mut native = 0u64;
        if ocerz_abi_callback_convert(imp, notation, &mut native) != OCERZ_OK as c_int || native == 0 {
            ob_refuse!(cls, sel, "%s could not bind implementation %#llx", who, imp);
        }
        ocerz_g2h(native)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_class_addMethod(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let cls = if gpr(cpu, OCERZ_RDI) != 0 { ocerz_g2h(gpr(cpu, OCERZ_RDI)) } else { null_mut() };
        let sel = if gpr(cpu, OCERZ_RSI) != 0 { ocerz_g2h(gpr(cpu, OCERZ_RSI)) } else { null_mut() };
        let imp = gpr(cpu, OCERZ_RDX);
        let types = if gpr(cpu, OCERZ_RCX) != 0 {
            ocerz_g2h(gpr(cpu, OCERZ_RCX)) as *const c_char
        } else {
            null()
        };
        let f = ob_need(&raw const G_OB_CLASS_ADD_METHOD);
        let mut outer: OcerzBridgeFrame = core::mem::MaybeUninit::uninit().assume_init();
        ocerz_bridge_raise(&mut outer, OCERZ_OBJC_LIBOBJC.as_ptr() as *const c_char, c"_class_addMethod".as_ptr(), null(), f);
        let mut added = false;
        if !cls.is_null() && !sel.is_null() && imp != 0 {
            let mut count = 0u32;
            let copylist: unsafe extern "C" fn(*mut c_void, *mut u32) -> *mut *mut c_void =
                core::mem::transmute(ob_need(&raw const G_OB_CLASS_COPY_METHOD_LIST));
            let getname: unsafe extern "C" fn(*mut c_void) -> *mut c_void =
                core::mem::transmute(ob_need(&raw const G_OB_METHOD_GET_NAME));
            let methods = copylist(cls, &mut count);
            let mut exists = false;
            let mut i = 0;
            while i < count && !exists {
                exists = getname(*methods.add(i as usize)) == sel;
                i += 1;
            }
            libc::free(methods as *mut c_void);
            if exists {
                ocerz_bridge_lower(&outer);
                ob_return(cpu, 0);
                return ob_settle(vm, cpu);
            }
            let mut notation: [c_char; OCERZ_OBJC_NOTATION_MAX as usize] = core::mem::MaybeUninit::uninit().assume_init();
            let rc = ocerz_objc_method_notation(types, notation.as_mut_ptr(), notation.len());
            if rc != OCERZ_OBJC_OK as c_int {
                ob_refuse!(cls, sel, "class_addMethod cannot cross: %s", crate::ported::objcbridge::encode::ocerz_objc_refusal(rc));
            }
            let native = ob_imp_from_guest_or_bind(imp, notation.as_ptr(), cls, sel, c"class_addMethod".as_ptr());
            let f: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, *const c_char) -> bool =
                core::mem::transmute(f);
            added = f(cls, sel, native, types);
            if added {
                G_OB_GENERATION.fetch_add(1, Ordering::SeqCst);
            }
        }
        ocerz_bridge_lower(&outer);
        ob_return(cpu, added as u64);
        ob_settle(vm, cpu)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_methodSetImplementation(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let m = if gpr(cpu, OCERZ_RDI) != 0 { ocerz_g2h(gpr(cpu, OCERZ_RDI)) } else { null_mut() };
        let imp = gpr(cpu, OCERZ_RSI);
        let f = ob_need(&raw const G_OB_METHOD_SET_IMPLEMENTATION);
        let mut outer: OcerzBridgeFrame = core::mem::MaybeUninit::uninit().assume_init();
        ocerz_bridge_raise(&mut outer, OCERZ_OBJC_LIBOBJC.as_ptr() as *const c_char, c"_method_setImplementation".as_ptr(), null(), f);
        let mut old: *mut c_void = null_mut();
        if !m.is_null() {
            let types = ob_method_get_type_encoding(m);
            let mut notation: [c_char; OCERZ_OBJC_NOTATION_MAX as usize] = core::mem::MaybeUninit::uninit().assume_init();
            let rc = ocerz_objc_method_notation(types, notation.as_mut_ptr(), notation.len());
            if rc != OCERZ_OBJC_OK as c_int {
                ob_refuse!(null_mut() as *mut c_void, null_mut() as *mut c_void, "method_setImplementation cannot cross: %s", crate::ported::objcbridge::encode::ocerz_objc_refusal(rc));
            }
            let native = if imp != 0 {
                ob_imp_from_guest_or_bind(imp, notation.as_ptr(), null_mut(), null_mut(), c"method_setImplementation".as_ptr())
            } else {
                null_mut()
            };
            let f: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void = core::mem::transmute(f);
            old = f(m, native);
            G_OB_GENERATION.fetch_add(1, Ordering::SeqCst);
            ocerz_bridge_lower(&outer);
            ob_return(cpu, crate::ported::objcbridge::imp::ocerz_objc_imp_for_guest(old, types));
            return ob_settle(vm, cpu);
        }
        ocerz_bridge_lower(&outer);
        ob_return(cpu, 0);
        ob_settle(vm, cpu)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_class_replaceMethod(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let cls = if gpr(cpu, OCERZ_RDI) != 0 { ocerz_g2h(gpr(cpu, OCERZ_RDI)) } else { null_mut() };
        let sel = if gpr(cpu, OCERZ_RSI) != 0 { ocerz_g2h(gpr(cpu, OCERZ_RSI)) } else { null_mut() };
        let imp = gpr(cpu, OCERZ_RDX);
        let types = if gpr(cpu, OCERZ_RCX) != 0 {
            ocerz_g2h(gpr(cpu, OCERZ_RCX)) as *const c_char
        } else {
            null()
        };
        let f = ob_need(&raw const G_OB_CLASS_REPLACE_METHOD);
        let mut outer: OcerzBridgeFrame = core::mem::MaybeUninit::uninit().assume_init();
        ocerz_bridge_raise(&mut outer, OCERZ_OBJC_LIBOBJC.as_ptr() as *const c_char, c"_class_replaceMethod".as_ptr(), null(), f);
        let mut answer = 0u64;
        if !cls.is_null() && !sel.is_null() && imp != 0 {
            let was = ob_class_get_instance_method(cls, sel);
            let was_types = if was.is_null() { null() } else { ob_method_get_type_encoding(was) };
            let mut notation: [c_char; OCERZ_OBJC_NOTATION_MAX as usize] = core::mem::MaybeUninit::uninit().assume_init();
            let rc = ocerz_objc_method_notation(types, notation.as_mut_ptr(), notation.len());
            if rc != OCERZ_OBJC_OK as c_int {
                ob_refuse!(cls, sel, "class_replaceMethod cannot cross: %s", crate::ported::objcbridge::encode::ocerz_objc_refusal(rc));
            }
            let native = ob_imp_from_guest_or_bind(imp, notation.as_ptr(), cls, sel, c"class_replaceMethod".as_ptr());
            let f: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, *const c_char) -> *mut c_void =
                core::mem::transmute(f);
            let old = f(cls, sel, native, types);
            G_OB_GENERATION.fetch_add(1, Ordering::SeqCst);
            answer = crate::ported::objcbridge::imp::ocerz_objc_imp_for_guest(old, was_types);
        }
        ocerz_bridge_lower(&outer);
        ob_return(cpu, answer);
        ob_settle(vm, cpu)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_method_getImplementation(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let m = if gpr(cpu, OCERZ_RDI) != 0 { ocerz_g2h(gpr(cpu, OCERZ_RDI)) } else { null_mut() };
        let f = ob_need(&raw const G_OB_METHOD_GET_IMPLEMENTATION);
        let mut outer: OcerzBridgeFrame = core::mem::MaybeUninit::uninit().assume_init();
        ocerz_bridge_raise(&mut outer, OCERZ_OBJC_LIBOBJC.as_ptr() as *const c_char, c"_method_getImplementation".as_ptr(), null(), f);
        let f: unsafe extern "C" fn(*mut c_void) -> *mut c_void = core::mem::transmute(f);
        let imp = if m.is_null() { null_mut() } else { f(m) };
        let answer = crate::ported::objcbridge::imp::ocerz_objc_imp_for_guest(
            imp,
            if m.is_null() { null() } else { ob_method_get_type_encoding(m) },
        );
        ocerz_bridge_lower(&outer);
        ob_return(cpu, answer);
        ob_settle(vm, cpu)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_class_getMethodImplementation(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let cls = if gpr(cpu, OCERZ_RDI) != 0 { ocerz_g2h(gpr(cpu, OCERZ_RDI)) } else { null_mut() };
        let sel = if gpr(cpu, OCERZ_RSI) != 0 { ocerz_g2h(gpr(cpu, OCERZ_RSI)) } else { null_mut() };
        let f = ob_need(&raw const G_OB_CLASS_GET_METHOD_IMPLEMENTATION);
        let mut outer: OcerzBridgeFrame = core::mem::MaybeUninit::uninit().assume_init();
        ocerz_bridge_raise(&mut outer, OCERZ_OBJC_LIBOBJC.as_ptr() as *const c_char, c"_class_getMethodImplementation".as_ptr(), null(), f);
        let f: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void = core::mem::transmute(f);
        let imp = if cls.is_null() || sel.is_null() { null_mut() } else { f(cls, sel) };
        let m = if cls.is_null() || sel.is_null() {
            null_mut()
        } else {
            ob_class_get_instance_method(cls, sel)
        };
        let answer = crate::ported::objcbridge::imp::ocerz_objc_imp_for_guest(
            imp,
            if m.is_null() { null() } else { ob_method_get_type_encoding(m) },
        );
        ocerz_bridge_lower(&outer);
        ob_return(cpu, answer);
        ob_settle(vm, cpu)
    }
}

unsafe fn ob_append(buf: *mut c_char, cap: usize, s: *const c_char, cls: *mut c_void, sel: *mut c_void) {
    unsafe {
        let have = libc::strlen(buf);
        let add = if s.is_null() { 0 } else { libc::strlen(s) };
        if have + add + 1 > cap {
            ob_refuse!(cls, sel, "has a forwarded method signature longer than ocerz reads");
        }
        libc::memcpy(buf.add(have) as *mut c_void, s as *const c_void, add + 1);
    }
}

unsafe fn ob_forwarded(recv: *mut c_void, cls: *mut c_void, sel: *mut c_void, scratch: *mut ObSend, depth: c_int) -> *const ObSend {
    unsafe {
        let send = ob_need(&raw const G_OB_MSGSEND);
        let send3: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void) -> *mut c_void =
            core::mem::transmute(send);
        let fts = ob_sel_register_name(c"forwardingTargetForSelector:".as_ptr());
        if depth < 8 && ob_class_responds_to_selector(cls, fts) {
            let target = send3(recv, fts, sel);
            if !target.is_null() && target != recv {
                let tcls = ob_object_get_class(target);
                let e = ob_cached(tcls, sel);
                if e.is_null() {
                    let m = ob_class_get_instance_method(tcls, sel);
                    if !m.is_null() {
                        ob_describe(tcls, sel, ob_method_get_type_encoding(m), c"forwarding target's".as_ptr(), scratch);
                        return scratch;
                    }
                    return ob_forwarded(target, tcls, sel, scratch, depth + 1);
                }
                *scratch = *e;
                return scratch;
            }
        }

        let msfs = ob_sel_register_name(c"methodSignatureForSelector:".as_ptr());
        if !ob_class_responds_to_selector(cls, msfs) {
            ob_refuse!(
                cls,
                sel,
                "has no method, and the receiver does not answer methodSignatureForSelector:, so there is no signature to forward it under"
            );
        }
        let ms = send3(recv, msfs, sel);
        if ms.is_null() {
            ob_refuse!(
                cls,
                sel,
                "is not a selector the receiver recognizes: there is no method and methodSignatureForSelector: answers nil"
            );
        }
        let send2: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void =
            core::mem::transmute(send);
        let rt = send2(ms, ob_sel_register_name(c"methodReturnType".as_ptr())) as *const c_char;
        let send_ul: unsafe extern "C" fn(*mut c_void, *mut c_void) -> usize =
            core::mem::transmute(send);
        let n = send_ul(ms, ob_sel_register_name(c"numberOfArguments".as_ptr()));
        let at = ob_sel_register_name(c"getArgumentTypeAtIndex:".as_ptr());
        let mut enc: [c_char; 1024] = core::mem::MaybeUninit::uninit().assume_init();
        enc[0] = 0;
        ob_append(enc.as_mut_ptr(), enc.len(), rt, cls, sel);
        let send_at: unsafe extern "C" fn(*mut c_void, *mut c_void, usize) -> *const c_char =
            core::mem::transmute(send);
        for i in 0..n {
            ob_append(enc.as_mut_ptr(), enc.len(), send_at(ms, at, i), cls, sel);
        }
        ob_describe(cls, sel, enc.as_ptr(), c"forwarded".as_ptr(), scratch);
        scratch
    }
}

pub unsafe fn ob_named(sig: *const OcerzAbiSig, cpu: *const OcerzCPU, k: c_int, cls: c_char) -> u64 {
    unsafe {
        let mut head: OcerzAbiSig = *sig;
        let mut va: OcerzAbiVaList = core::mem::MaybeUninit::uninit().assume_init();
        let mut v = 0u64;
        head.nargs = k;
        if ocerz_abi_va_start(&head, cpu as *mut OcerzCPU, &mut va) == OCERZ_OK as c_int {
            ocerz_abi_va_arg(&mut va, cpu, cls, &mut v);
        }
        v
    }
}

pub struct ObText {
    pub s: *const c_char,
    pub heap: *mut c_char,
    pub local: [c_char; 512],
}

pub unsafe fn ob_text(str_: *mut c_void, t: *mut ObText, what: *const c_char) {
    unsafe {
        (*t).heap = null_mut();
        (*t).local[0] = 0;
        (*t).s = (*t).local.as_ptr();
        if str_.is_null() {
            return;
        }
        let getlen: unsafe extern "C" fn(*mut c_void) -> i64 =
            core::mem::transmute(ob_need(&raw const G_OB_CFSTRING_GET_LENGTH));
        let getmax: unsafe extern "C" fn(i64, u32) -> i64 =
            core::mem::transmute(ob_need(&raw const G_OB_CFSTRING_GET_MAXIMUM_SIZE));
        let len = getlen(str_);
        let mut max = getmax(len, OB_UTF8);
        if max < 0 {
            ob_stop!("%s: a format string of %ld characters is too long to read", what, len);
        }
        max += 1;
        let mut buf = (*t).local.as_mut_ptr();
        if max > (*t).local.len() as i64 {
            buf = libc::malloc(max as usize) as *mut c_char;
            (*t).heap = buf;
            if buf.is_null() {
                ob_stop!("%s: no memory to read a format string of %ld characters", what, len);
            }
        }
        let getcstr: unsafe extern "C" fn(*mut c_void, *mut c_char, i64, u32) -> bool =
            core::mem::transmute(ob_need(&raw const G_OB_CFSTRING_GET_CSTRING));
        if !getcstr(str_, buf, max, OB_UTF8) {
            ob_stop!("%s: its format string does not convert to UTF-8", what);
        }
        (*t).s = buf;
    }
}

pub unsafe fn ob_text_wide(wide: *const i32, t: *mut ObText, what: *const c_char) {
    unsafe {
        let mut n = 0usize;
        while !wide.is_null() && *wide.add(n) != 0 {
            n += 1;
        }
        let mut buf = (*t).local.as_mut_ptr();
        (*t).heap = null_mut();
        if n >= (*t).local.len() {
            buf = libc::malloc(n + 1) as *mut c_char;
            (*t).heap = buf;
            if buf.is_null() {
                ob_stop!("%s has a format of %zu wide characters and there is no memory to read it into", what, n);
            }
        }
        for i in 0..n {
            let w = *wide.add(i);
            *buf.add(i) = if w > 0 && w < 0x80 { w as c_char } else { b'?' as c_char };
        }
        *buf.add(n) = 0;
        (*t).s = buf;
    }
}

pub unsafe fn ob_text_free(t: *mut ObText) {
    unsafe {
        libc::free((*t).heap as *mut c_void);
        (*t).heap = null_mut();
    }
}

pub unsafe fn ob_gather_format(
    what: *const c_char,
    text: *const c_char,
    dialect: c_int,
    named: *const OcerzAbiSig,
    cpu: *const OcerzCPU,
    slots: *mut u64,
) -> c_int {
    unsafe {
        let mut classes: [c_char; OCERZ_OBJC_VARIADIC_MAX as usize + 1] = core::mem::MaybeUninit::uninit().assume_init();
        let mut why: *const c_char = null();
        let n = crate::ported::objcbridge::encode::ocerz_objc_format_classes(text, dialect, classes.as_mut_ptr(), classes.len(), &mut why);
        if n < 0 {
            ob_stop!("%s refuses the format \"%.200s\": it has %s", what, text, why);
        }
        let mut va: OcerzAbiVaList = core::mem::MaybeUninit::uninit().assume_init();
        if ocerz_abi_va_start(named, cpu as *mut OcerzCPU, &mut va) != OCERZ_OK as c_int {
            ob_stop!("%s: the ABI engine cannot find where the variadic arguments begin", what);
        }
        for k in 0..n as usize {
            ocerz_abi_va_arg(&mut va, cpu, classes[k], slots.add(k));
        }
        n
    }
}

pub unsafe fn ob_gather_va_format(
    what: *const c_char,
    text: *const c_char,
    dialect: c_int,
    address: u64,
    slots: *mut u64,
) -> c_int {
    unsafe {
        let mut classes: [c_char; OCERZ_OBJC_VARIADIC_MAX as usize + 1] = core::mem::MaybeUninit::uninit().assume_init();
        let mut why: *const c_char = null();
        let n = crate::ported::objcbridge::encode::ocerz_objc_format_classes(text, dialect, classes.as_mut_ptr(), classes.len(), &mut why);
        if n < 0 {
            ob_stop!("%s refuses the format \"%.200s\": it has %s", what, text, why);
        }
        if n == 0 {
            return 0;
        }
        if address == 0 {
            ob_stop!("%s: null guest va_list", what);
        }
        let mut gp = ocerz_ld(address, 4) as u32;
        let mut fp = ocerz_ld(address + 4, 4) as u32;
        let mut overflow = ocerz_ld(address + 8, 8);
        let saved = ocerz_ld(address + 16, 8);
        if gp > 48 || (gp & 7) != 0 || fp < 48 || fp > 176 || ((fp - 48) & 15) != 0 {
            ob_stop!("%s: invalid guest va_list offsets (gp=%u fp=%u)", what, gp, fp);
        }
        for k in 0..n as usize {
            let from: u64;
            if classes[k] == b'd' as c_char && fp < 176 {
                if saved == 0 {
                    ob_stop!("%s: null guest va_list register save area", what);
                }
                from = saved + fp as u64;
                fp += 16;
            } else if classes[k] != b'd' as c_char && gp < 48 {
                if saved == 0 {
                    ob_stop!("%s: null guest va_list register save area", what);
                }
                from = saved + gp as u64;
                gp += 8;
            } else {
                if overflow == 0 {
                    ob_stop!("%s: null guest va_list overflow area", what);
                }
                from = overflow;
                overflow += 8;
            }
            let mut raw = ocerz_ld(from, 8);
            if classes[k] == b'p' as c_char {
                raw = if raw != 0 { ocerz_g2h(raw) as u64 } else { 0 };
            } else if classes[k] == b'i' as c_char {
                raw = raw as i32 as i64 as u64;
            } else if classes[k] == b'u' as c_char {
                raw = raw as u32 as u64;
            }
            *slots.add(k) = raw;
        }
        n
    }
}

#[inline(never)]
unsafe fn ob_perform_general(
    cpu: *mut OcerzCPU,
    sig: *const OcerzAbiSig,
    f: *const c_void,
    slots: *const u64,
    nslots: c_int,
    as_va_list: c_int,
    what: *const c_char,
) -> c_int {
    unsafe {
        let mut call: OcerzAbiCall = core::mem::MaybeUninit::uninit().assume_init();
        let mut stack: [u64; OCERZ_ABI_MAX_STACK as usize + OCERZ_OBJC_VARIADIC_MAX as usize] = core::mem::MaybeUninit::uninit().assume_init();

        if ocerz_abi_read_guest(sig, cpu, &mut call) != OCERZ_OK as c_int {
            ob_stop!("%s: the ABI engine cannot read the guest's arguments", what);
        }

        let mut words = call.nstack as usize;
        core::ptr::copy_nonoverlapping(call.stack.as_ptr(), stack.as_mut_ptr(), words);
        if as_va_list != 0 {
            if call.nx >= 8 {
                ob_stop!("%s: the va_list has no argument register left", what);
            }
            call.x[call.nx as usize] = slots as u64;
            call.nx += 1;
        } else if nslots > 0 {
            if words + nslots as usize > stack.len() {
                ob_stop!("%s: %d variadic arguments do not fit the native stack ocerz builds", what, nslots);
            }
            core::ptr::copy_nonoverlapping(slots, stack.as_mut_ptr().add(words), nslots as usize);
            words += nslots as usize;
        }

        let fpcr = ob_round_swap(OCERZ_ABI_ROUND_NEAREST as u64);
        ocerz_abi_call_native(
            f,
            call.x.as_ptr(),
            call.v.as_ptr(),
            stack.as_ptr(),
            words as u64 * 8,
            call.x8,
            call.rx.as_mut_ptr(),
            call.rv.as_mut_ptr(),
        );
        let err = *libc::__error();
        ob_round_swap(fpcr & OCERZ_ABI_ROUND_MASK as u64);

        call.borrowed = 1;
        ocerz_abi_write_result(sig, cpu, &mut call);
        ocerz_abi_release_owned(&mut call);
        *libc::__error() = err;
        err
    }
}

pub unsafe fn ob_perform(
    cpu: *mut OcerzCPU,
    sig: *const OcerzAbiSig,
    f: *const c_void,
    slots: *const u64,
    nslots: c_int,
    as_va_list: c_int,
    what: *const c_char,
) -> c_int {
    unsafe {
        if nslots <= 0 && as_va_list == 0 && ocerz_abi_register_only(sig as *mut OcerzAbiSig) != 0 {
            ocerz_abi_perform_registers(sig, f, cpu);
            return *libc::__error();
        }
        ob_perform_general(cpu, sig, f, slots, nslots, as_va_list, what)
    }
}

pub const OB_PLAIN: c_int = 0;
pub const OB_SUPER: c_int = 1;
pub const OB_SUPER2: c_int = 2;

struct ObExports([[*const c_char; 2]; 3]);
unsafe impl Sync for ObExports {}
static G_OB_EXPORT: ObExports = ObExports([
    [c"_objc_msgSend".as_ptr(), c"_objc_msgSend_stret".as_ptr()],
    [c"_objc_msgSendSuper".as_ptr(), c"_objc_msgSendSuper_stret".as_ptr()],
    [c"_objc_msgSendSuper2".as_ptr(), c"_objc_msgSendSuper2_stret".as_ptr()],
]);

unsafe fn ob_nil(vm: *mut OcerzVM, cpu: *mut OcerzCPU, stret: c_int, size: u64) -> c_int {
    unsafe {
        let result = gpr(cpu, OCERZ_RDI);
        if stret != 0 && size != 0 && result != 0 {
            core::ptr::write_bytes(ocerz_g2h(result) as *mut u8, 0, size as usize);
        }
        set_gpr(cpu, OCERZ_RDX, 0);
        (*cpu).xmm[0].lo = 0;
        (*cpu).xmm[0].hi = 0;
        (*cpu).xmm[1].lo = 0;
        (*cpu).xmm[1].hi = 0;
        ob_return(cpu, if stret != 0 { result } else { 0 });
        ob_settle(vm, cpu)
    }
}

unsafe fn ob_check_callables(cls: *mut c_void, sel: *mut c_void, send: *const ObSend, cpu: *const OcerzCPU, imp: *mut c_void) {
    unsafe {
        let sig = &(*(*send).shape).sig;
        let imp = if imp.is_null() {
            let f: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void =
                core::mem::transmute(ob_need(&raw const G_OB_CLASS_GET_METHOD_IMPLEMENTATION));
            f(cls, sel)
        } else {
            imp
        };
        if !ocerz_abi_callback_sig(imp, null_mut()).is_null() {
            return;
        }
        let mut k = 0;
        while k < sig.nargs && k < 32 {
            let bit = 1u32 << k;
            if (*send).fnptrs & bit == 0 {
                k += 1;
                continue;
            }
            let v = ob_named(sig, cpu, k, 'L' as c_char);
            if v != 0 && ocerz_abi_is_guest_code(v) != 0 {
                ob_refuse!(
                    cls,
                    sel,
                    "cannot cross: argument %d is an x86 function pointer (%#llx), which native code cannot call and no encoding gives a signature for",
                    k - 2,
                    v
                );
            }
            k += 1;
        }
    }
}

pub unsafe fn ob_arg_at(notation: *const c_char, index: c_int) -> *const c_char {
    unsafe {
        let mut p = libc::strchr(notation, b'(' as c_int);
        if p.is_null() {
            return null();
        }
        p = p.add(1);
        let mut k = 0;
        while *p != 0 && *p != b')' as c_char {
            if k == index {
                return p;
            }
            if *p == b'c' as c_char || *p == b'k' as c_char {
                p = p.add(1);
            }
            if *p == b'{' as c_char {
                let mut depth = 0;
                loop {
                    if *p == b'{' as c_char {
                        depth += 1;
                    } else if *p == b'}' as c_char {
                        depth -= 1;
                    }
                    p = p.add(1);
                    if !(*p != 0 && depth > 0) {
                        break;
                    }
                }
            } else {
                p = p.add(1);
            }
            k += 1;
        }
        null()
    }
}

unsafe fn ob_object_blocks(
    recv: *mut c_void,
    cls: *mut c_void,
    sel: *mut c_void,
    send: *const ObSend,
    cpu: *const OcerzCPU,
    scratch: *mut ObSend,
) -> *const ObSend {
    unsafe {
        let sig = &(*(*send).shape).sig;
        let mut found = 0u32;
        let mut k = 2;
        while k < sig.nargs && k < 32 {
            if (*send).objects & (1 << k) != 0 && *sig.arg.as_ptr().add(k as usize) == b'p' as c_char {
                let v = ob_named(sig, cpu, k, 'L' as c_char);
                if v != 0 && (v >> 63) == 0 && ocerz_block_is_guest_object(v) != 0 {
                    found |= 1 << k;
                }
            }
            k += 1;
        }
        if found == 0 {
            return send;
        }
        let msend = ob_need(&raw const G_OB_MSGSEND);
        let msfs = ob_sel_register_name(c"methodSignatureForSelector:".as_ptr());
        if !ob_class_responds_to_selector(cls, msfs) {
            return send;
        }
        let send3: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void) -> *mut c_void =
            core::mem::transmute(msend);
        let ms = send3(recv, msfs, sel);
        if ms.is_null() {
            return send;
        }
        let at = ob_sel_register_name(c"getArgumentTypeAtIndex:".as_ptr());
        let send_nargs: unsafe extern "C" fn(*mut c_void, *mut c_void) -> usize =
            core::mem::transmute(msend);
        let n = send_nargs(ms, ob_sel_register_name(c"numberOfArguments".as_ptr()));
        let send_t: unsafe extern "C" fn(*mut c_void, *mut c_void, usize) -> *const c_char =
            core::mem::transmute(msend);
        for k in 2..32 {
            if found & (1 << k) == 0 {
                continue;
            }
            let t = if (k as usize) < n { send_t(ms, at, k) } else { null() };
            if t.is_null() || *t != b'@' as c_char || *t.add(1) != b'?' as c_char {
                found &= !(1 << k);
            }
        }
        if found == 0 {
            return send;
        }

        let mut notation: [c_char; OCERZ_OBJC_NOTATION_MAX as usize] = core::mem::MaybeUninit::uninit().assume_init();
        let mut len = 0usize;
        let src = (*(*send).shape).notation.as_ptr();
        let mut mark: [*const c_char; 32] = [null(); 32];
        for k in 2..32 {
            if found & (1 << k) != 0 {
                mark[k] = ob_arg_at(src, k as c_int);
            }
        }
        let mut p = src;
        while *p != 0 {
            let mut hit = false;
            let mut k = 2;
            while k < 32 && !hit {
                hit = mark[k] == p;
                k += 1;
            }
            if len + 4 >= notation.len() {
                return send;
            }
            if hit {
                libc::memcpy(notation.as_mut_ptr().add(len) as *mut c_void, c"k{}".as_ptr() as *const c_void, 3);
                len += 3;
            } else {
                notation[len] = *p;
                len += 1;
            }
            p = p.add(1);
        }
        notation[len] = 0;
        let shape = ob_shape(notation.as_ptr());
        if shape.is_null() {
            return send;
        }
        *scratch = *send;
        (*scratch).shape = shape;
        scratch
    }
}

pub unsafe fn ob_send_via(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    kind: c_int,
    stret: c_int,
    imp: *mut c_void,
    imp_types: *const c_char,
) -> c_int {
    unsafe {
        let export: *const c_char = if !imp.is_null() {
            c"_(native IMP)".as_ptr()
        } else {
            G_OB_EXPORT.0[kind as usize][stret as usize]
        };
        let first = gpr(cpu, if stret != 0 { OCERZ_RSI } else { OCERZ_RDI });
        let sel = gpr(cpu, if stret != 0 { OCERZ_RDX } else { OCERZ_RSI }) as *mut c_void;
        let host = if kind == OB_PLAIN {
            &raw const G_OB_MSGSEND
        } else if kind == OB_SUPER {
            &raw const G_OB_MSGSEND_SUPER
        } else {
            &raw const G_OB_MSGSEND_SUPER2
        };
        let f = if imp.is_null() { ob_need(host) } else { imp };
        let mut outer: OcerzBridgeFrame = core::mem::MaybeUninit::uninit().assume_init();
        let mut recv: *mut c_void;
        let mut cls: *mut c_void;
        let mut scratch: ObSend = core::mem::MaybeUninit::uninit().assume_init();
        let mut send: *const ObSend;

        ocerz_bridge_raise(&mut outer, OCERZ_OBJC_LIBOBJC.as_ptr() as *const c_char, export, null(), f);
        if kind == OB_PLAIN {
            recv = if first != 0 { ocerz_g2h(first) } else { null_mut() };
            if !recv.is_null() {
                ocerz_objcbridge_ensure_object(recv);
            }
            cls = if recv.is_null() { null_mut() } else { ob_object_get_class(recv) };
        } else {
            if first == 0 {
                ob_stop!("%s was handed a null struct objc_super", export);
            }
            let r = ocerz_ld(first, 8);
            let c = ocerz_ld(first + 8, 8);
            recv = if r != 0 { ocerz_g2h(r) } else { null_mut() };
            cls = if c != 0 { ocerz_g2h(c) } else { null_mut() };
            if !cls.is_null() {
                ocerz_objcbridge_ensure_class(cls);
            }
            if kind == OB_SUPER2 && !cls.is_null() {
                cls = ob_class_get_superclass(cls);
            }
        }

        if recv.is_null() {
            let mut size = 0u64;
            if stret != 0 && !cls.is_null() {
                send = ob_method(cls, sel, &mut scratch);
                if !send.is_null() && (*(*send).shape).sig.ret == b'{' as c_char {
                    size = (*(*send).shape).sig.ret_struct.size as u64;
                }
            }
            ocerz_bridge_lower(&outer);
            return ob_nil(vm, cpu, stret, size);
        }
        if cls.is_null() {
            ob_stop!("%s: a super send names no class to start its lookup at", export);
        }

        if kind == OB_PLAIN
            && cls == concrete_stack_block()
            && ocerz_block_is_guest(first) != 0
        {
            let name = ob_sel_get_name(sel);
            if libc::strcmp(name, c"copy".as_ptr()) == 0
                || libc::strcmp(name, c"copyWithZone:".as_ptr()) == 0
            {
                let copy = ocerz_block_copy_guest(first);
                ocerz_bridge_lower(&outer);
                ob_return(cpu, copy);
                return ob_settle(vm, cpu);
            }
        }

        send = ob_method(cls, sel, &mut scratch);
        if send.is_null() && !imp.is_null() && !imp_types.is_null() {
            ob_describe(cls, sel, imp_types, c"implementation".as_ptr(), &mut scratch);
            send = &scratch;
        }
        if send.is_null() {
            send = ob_forwarded(recv, cls, sel, &mut scratch, 0);
        }
        let mut typed: ObSend = core::mem::MaybeUninit::uninit().assume_init();
        if (*send).objects != 0 {
            send = ob_object_blocks(recv, cls, sel, send, cpu, &mut typed);
        }

        let sig = &(*(*send).shape).sig;
        let memory = sig.ret == b'{' as c_char && sig.ret_struct.size as usize > OB_SMALL_STRUCT;
        if stret != 0 && !memory {
            ob_refuse!(
                cls,
                sel,
                "was sent with %s, but its result under %s is not one System V returns in memory",
                export.add(1),
                (*(*send).shape).notation.as_ptr()
            );
        }
        if stret == 0 && memory {
            ob_refuse!(
                cls,
                sel,
                "returns a structure System V returns in memory (%s), and the guest sent it with %s, which passes no result pointer",
                (*(*send).shape).notation.as_ptr(),
                export.add(1)
            );
        }
        if (*send).fnptrs != 0 {
            ob_check_callables(cls, sel, send, cpu, imp);
        }

        let selname = ob_sel_get_name(sel);
        ocerz_bridge_lower(&outer);
        ocerz_bridge_raise(&mut outer, OCERZ_OBJC_LIBOBJC.as_ptr() as *const c_char, selname, (*(*send).shape).notation.as_ptr(), f);

        if ob_logging() != 0 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: OBJCLOG[%d] %c[%s %s] %s%s%s\n".as_ptr(),
                libc::getpid(),
                if ob_class_is_meta_class(cls) { '+' as i32 } else { '-' as i32 },
                ob_class_get_name(cls),
                selname,
                (*(*send).shape).notation.as_ptr(),
                if (*send).variadic.is_null() { c"".as_ptr() } else { c" variadic".as_ptr() },
                if send == &scratch as *const ObSend { c" forwarded".as_ptr() } else { c"".as_ptr() },
            );
        }

        let mut slots: [u64; OCERZ_OBJC_VARIADIC_MAX as usize] = core::mem::MaybeUninit::uninit().assume_init();
        let mut nslots: c_int = 0;
        let v = (*send).variadic;
        if !v.is_null() && (*v).kind == OCERZ_OBJC_VA_NIL_TERMINATED as c_int {
            if ob_named(sig, cpu, sig.nargs - 1, 'p' as c_char) != 0 {
                let mut va: OcerzAbiVaList = core::mem::MaybeUninit::uninit().assume_init();
                if ocerz_abi_va_start(sig, cpu, &mut va) != OCERZ_OK as c_int {
                    ob_refuse!(cls, sel, "cannot cross: the ABI engine cannot find its variadic arguments");
                }
                loop {
                    if nslots == OCERZ_OBJC_VARIADIC_MAX as c_int {
                        ob_refuse!(cls, sel, "cannot cross: no nil among its first %d variadic arguments", OCERZ_OBJC_VARIADIC_MAX as c_int);
                    }
                    ocerz_abi_va_arg(&mut va, cpu, 'p' as c_char, slots.as_mut_ptr().add(nslots as usize));
                    let was = *slots.as_ptr().add(nslots as usize);
                    nslots += 1;
                    if was == 0 {
                        break;
                    }
                }
            }
        } else if !v.is_null() && (*v).kind == OCERZ_OBJC_VA_LIST as c_int {
            static REGS: [u32; 6] = [OCERZ_RDI as u32, OCERZ_RSI as u32, OCERZ_RDX as u32, OCERZ_RCX as u32, OCERZ_R8 as u32, OCERZ_R9 as u32];
            let fmt = ob_named(sig, cpu, (*v).arg, 'p' as c_char);
            let list = ob_named(sig, cpu, (*v).va, 'p' as c_char);
            let mut text: ObText = core::mem::MaybeUninit::uninit().assume_init();
            let mut what: [c_char; 160] = core::mem::MaybeUninit::uninit().assume_init();
            libc::snprintf(
                what.as_mut_ptr(),
                what.len(),
                c"%c[%s %s]".as_ptr(),
                if ob_class_is_meta_class(cls) { '+' as i32 } else { '-' as i32 },
                ob_class_get_name(cls),
                selname,
            );
            let mut obj = fmt as *mut c_void;
            if (*v).attributed != 0 && !obj.is_null() {
                let send2: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void =
                    core::mem::transmute(ob_need(&raw const G_OB_MSGSEND));
                obj = send2(obj, ob_sel_register_name(c"string".as_ptr()));
            }
            ob_text(obj, &mut text, what.as_ptr());
            nslots = ob_gather_va_format(
                what.as_ptr(),
                text.s,
                (*v).dialect,
                if list != 0 { ocerz_h2g(list as *mut c_void) } else { 0 },
                slots.as_mut_ptr(),
            );
            ob_text_free(&mut text);
            set_gpr(cpu, REGS[(*v).va as usize], ocerz_h2g(slots.as_mut_ptr() as *mut c_void));
        } else if !v.is_null() {
            let fmt = ob_named(sig, cpu, (*v).arg, 'p' as c_char);
            let mut text: ObText = core::mem::MaybeUninit::uninit().assume_init();
            let mut what: [c_char; 160] = core::mem::MaybeUninit::uninit().assume_init();
            libc::snprintf(
                what.as_mut_ptr(),
                what.len(),
                c"%c[%s %s]".as_ptr(),
                if ob_class_is_meta_class(cls) { '+' as i32 } else { '-' as i32 },
                ob_class_get_name(cls),
                selname,
            );
            if (*v).dialect == OCERZ_OBJC_FMT_TYPES as c_int {
                text.heap = null_mut();
                text.s = if fmt != 0 { fmt as *const c_char } else { c"".as_ptr() };
            } else {
                let mut obj = fmt as *mut c_void;
                if (*v).attributed != 0 && !obj.is_null() {
                    let send2: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void =
                        core::mem::transmute(ob_need(&raw const G_OB_MSGSEND));
                    obj = send2(obj, ob_sel_register_name(c"string".as_ptr()));
                }
                ob_text(obj, &mut text, what.as_ptr());
            }
            nslots = ob_gather_format(what.as_ptr(), text.s, (*v).dialect, sig, cpu, slots.as_mut_ptr());
            ob_text_free(&mut text);
        }

        let mut asked: *mut c_void = null_mut();
        let answers_imp = sig.ret == b'p' as c_char && sig.nargs == 3 && ob_answers_imp(sel);
        let mut bundle_id: *mut c_void = null_mut();
        let bundle_lookup = kind == OB_PLAIN
            && sig.ret == b'p' as c_char
            && sig.nargs == 3
            && ob_class_is_meta_class(cls)
            && libc::strcmp(selname, c"bundleWithIdentifier:".as_ptr()) == 0;
        if bundle_lookup {
            bundle_id = ob_named(sig, cpu, 2, 'p' as c_char) as *mut c_void;
        }
        if answers_imp {
            asked = ob_named(sig, cpu, 2, 'p' as c_char) as *mut c_void;
        }
        let mut raised: *mut c_void = null_mut();
        if !ob_eh().is_null() {
            let mut g = ObGuarded { cpu, sig, f, slots: slots.as_ptr(), nslots, what: selname };
            if ocerz_objc_guarded(ob_guarded_body, &mut g as *mut _ as *mut c_void, &mut raised) != 0 {
                ocerz_bridge_lower(&outer);
                return ob_eh_throw(vm, cpu, if raised.is_null() { 0 } else { ocerz_h2g(raised) }, 1);
            }
        } else {
            ob_perform(cpu, sig, f, slots.as_ptr(), nslots, 0, selname);
        }
        if bundle_lookup && gpr(cpu, OCERZ_RAX) == 0 && !bundle_id.is_null() {
            set_gpr(cpu, OCERZ_RAX, ocerz_h2g(ob_guest_bundle(recv, ocerz_g2h(bundle_id as u64))));
        }
        if answers_imp && gpr(cpu, OCERZ_RAX) != 0 {
            let of = if sel == G_OB_SEL_INSTANCE_METHOD_FOR.load(Ordering::SeqCst) { recv } else { cls };
            let m = if asked.is_null() || of.is_null() {
                null_mut()
            } else {
                ob_class_get_instance_method(of, asked)
            };
            set_gpr(
                cpu,
                OCERZ_RAX,
                crate::ported::objcbridge::imp::ocerz_objc_imp_for_guest(
                    ocerz_g2h(gpr(cpu, OCERZ_RAX)),
                    if m.is_null() { null() } else { ob_method_get_type_encoding(m) },
                ),
            );
        }
        ocerz_bridge_lower(&outer);
        ob_settle(vm, cpu)
    }
}

unsafe fn ob_send(vm: *mut OcerzVM, cpu: *mut OcerzCPU, kind: c_int, stret: c_int) -> c_int {
    unsafe { ob_send_via(vm, cpu, kind, stret, null_mut(), null()) }
}

static G_OB_GUEST_PREPROCESSOR: AtomicU64 = AtomicU64::new(0);

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_setExceptionPreprocessor(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let wanted = gpr(cpu, OCERZ_RDI);
        let f = ob_need(&raw const G_OB_SET_EXCEPTION_PREPROCESSOR);
        let mut outer: OcerzBridgeFrame = core::mem::MaybeUninit::uninit().assume_init();
        ocerz_bridge_raise(&mut outer, OCERZ_OBJC_LIBOBJC.as_ptr() as *const c_char, c"_objc_setExceptionPreprocessor".as_ptr(), c"p(c{p(p)})".as_ptr(), f);
        let mut native = 0u64;
        if wanted != 0
            && (ocerz_abi_callback_convert(wanted, c"p(p)".as_ptr(), &mut native) != OCERZ_OK as c_int
                || native == 0)
        {
            ob_stop!("objc_setExceptionPreprocessor could not bind preprocessor %#llx", wanted);
        }
        let f: unsafe extern "C" fn(*mut c_void) = core::mem::transmute(f);
        f(if native != 0 { ocerz_g2h(native) } else { null_mut() });
        let before = G_OB_GUEST_PREPROCESSOR.swap(wanted, Ordering::SeqCst);
        ocerz_bridge_lower(&outer);
        ob_return(cpu, before);
        ob_settle(vm, cpu)
    }
}

pub fn ob_guest_preprocessor() -> u64 {
    G_OB_GUEST_PREPROCESSOR.load(Ordering::SeqCst)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_imp_trap(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let k = (gpr(cpu, OCERZ_R10) & 0xffff_ffff) as usize;
        if k >= G_OB_IMPS_N.load(Ordering::SeqCst) as usize {
            ob_stop!("a thunk numbered %u for a native implementation was called, and ocerz made no such thunk", k as u32);
        }
        let e = &G_OB_IMPS[k];
        ob_send_via(vm, cpu, OB_PLAIN, e.stret, e.imp, e.types)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_msgSend(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_send(vm, cpu, OB_PLAIN, 0) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_msgSendSuper(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_send(vm, cpu, OB_SUPER, 0) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_msgSendSuper2(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_send(vm, cpu, OB_SUPER2, 0) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_msgSend_stret(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_send(vm, cpu, OB_PLAIN, 1) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_msgSendSuper_stret(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_send(vm, cpu, OB_SUPER, 1) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_msgSendSuper2_stret(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_send(vm, cpu, OB_SUPER2, 1) }
}

unsafe fn ob_long_double_send(cpu: *mut OcerzCPU, export: *const c_char, what: *const c_char) -> ! {
    unsafe {
        let r = gpr(cpu, OCERZ_RDI);
        let recv = if r != 0 { ocerz_g2h(r) } else { null_mut() };
        let sel = gpr(cpu, OCERZ_RSI) as *mut c_void;
        ob_refuse!(
            if recv.is_null() { null_mut() } else { ob_object_get_class(recv) },
            sel,
            "was sent with %s, which x86-64 uses only for a %s result, and that does not cross",
            export,
            what
        );
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_msgSend_fpret(_vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_long_double_send(cpu, c"objc_msgSend_fpret".as_ptr(), c"long double".as_ptr()) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_msgSend_fp2ret(_vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_long_double_send(cpu, c"objc_msgSend_fp2ret".as_ptr(), c"long double _Complex".as_ptr()) }
}

#[repr(C)]
pub struct ObGuarded {
    cpu: *mut OcerzCPU,
    sig: *const OcerzAbiSig,
    f: *const c_void,
    slots: *const u64,
    nslots: c_int,
    what: *const c_char,
}

unsafe extern "C" fn ob_guarded_body(ctx: *mut c_void) {
    unsafe {
        let g = ctx as *mut ObGuarded;
        ob_perform((*g).cpu, (*g).sig, (*g).f, (*g).slots, (*g).nslots, 0, (*g).what);
    }
}

use crate::ported::objcbridge::eh::{ob_eh, ob_eh_throw};

unsafe extern "C" {
    fn ocerz_objc_guarded(body: unsafe extern "C" fn(*mut c_void), ctx: *mut c_void, caught: *mut *mut c_void) -> c_int;
    static mut _NSConcreteStackBlock: [*mut c_void; 32];
}

unsafe fn ob_round_swap(mode: u64) -> u64 {
    unsafe {
        let fpcr: u64;
        core::arch::asm!("mrs {}, fpcr", out(reg) fpcr, options(nomem, nostack, preserves_flags));
        if (fpcr & OCERZ_ABI_ROUND_MASK as u64) != mode {
            core::arch::asm!(
                "msr fpcr, {}",
                in(reg) (fpcr & !(OCERZ_ABI_ROUND_MASK as u64)) | mode,
                options(nomem, nostack, preserves_flags)
            );
        }
        fpcr
    }
}

#[inline]
unsafe fn concrete_stack_block() -> *mut c_void {
    core::ptr::addr_of_mut!(_NSConcreteStackBlock).cast()
}

unsafe fn ob_guest_bundle(bundle_class: *mut c_void, wanted: *mut c_void) -> *mut c_void {
    unsafe {
        if bundle_class.is_null() || wanted.is_null() {
            return null_mut();
        }
        let send = ob_need(&raw const G_OB_MSGSEND);
        let create: unsafe extern "C" fn(*mut c_void, *const c_char, u32) -> *mut c_void =
            core::mem::transmute(ob_need(&raw const G_OB_CFSTRING_CREATE_WITH_CSTRING));
        let release: unsafe extern "C" fn(*mut c_void) =
            core::mem::transmute(ob_need(&raw const G_OB_CFRELEASE));
        let with_path = ob_sel_register_name(c"bundleWithPath:".as_ptr());
        let identifier = ob_sel_register_name(c"bundleIdentifier".as_ptr());
        let equal = ob_sel_register_name(c"isEqualToString:".as_ptr());
        let n = ocerz_dyld_image_count();
        for k in 0..n {
            let mut name = 0u64;
            if ocerz_dyld_image_at(k, null_mut(), null_mut(), &mut name) == 0 || name == 0 {
                continue;
            }
            let path = ocerz_g2h(name) as *const c_char;
            let fw = libc::strstr(path, c".framework/".as_ptr());
            if fw.is_null() || libc::strncmp(path, c"/System/".as_ptr(), 8) == 0 {
                continue;
            }
            let mut root = [0i8; 1024];
            let len = (fw as usize - path as usize) + b".framework".len();
            if len >= root.len() {
                continue;
            }
            libc::memcpy(root.as_mut_ptr() as *mut c_void, path as *const c_void, len);
            root[len] = 0;
            let str_ = create(null_mut(), root.as_ptr(), OB_UTF8);
            if str_.is_null() {
                continue;
            }
            let send3: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void) -> *mut c_void =
                core::mem::transmute(send);
            let bundle = send3(bundle_class, with_path, str_);
            release(str_);
            let send2: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void =
                core::mem::transmute(send);
            let bid = if bundle.is_null() { null_mut() } else { send2(bundle, identifier) };
            let sendb: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void) -> i8 =
                core::mem::transmute(send);
            if !bid.is_null() && sendb(bid, equal, wanted) != 0 {
                return bundle;
            }
        }
        null_mut()
    }
}

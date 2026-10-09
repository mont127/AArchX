//! ---- the class stays where the guest put it ----
//! objc_readClassPair realizes a compiler-emitted class pair in writable memory
//! without moving it: it answers the same pointer, registers the class by name,
//! realizes it against its superclass and slides its ivars.  So a guest class
//! keeps its address, and no class reference, superclass reference or bound
//! pointer is rewritten.  A probe in a plain arm64 process established the rest
//! before any of this was built.  The runtime realizes an untouched native
//! superclass itself.  A subclass of a class read this way works, super sends
//! included.  An NSView subclass whose ivar offset variable held 152 had it
//! slid to 536, arm64 NSView's size, and cacheDisplayInRect: called its
//! drawRect:.  Raw C function addresses as implementations need no pointer
//! authentication.  A duplicate name is not fatal: the runtime prints that the
//! class is implemented in both places, answers the new class, and keeps the
//! first for lookups by name, as it does for images dyld loads.  A class whose
//! superclass is a class_t the runtime has never seen is not refused either: the
//! call answers the class and the runtime aborts at realization with "Attempt to
//! use unknown class", which is why ocerz orders superclasses first and refuses
//! by name before the call.  class_t, class_ro_t, method_t, ivar_t, property_t,
//! category_t and protocol_t are laid out identically on x86_64 and arm64, both
//! being LP64, so the runtime reads the guest's structures as they are.
//!
//! Three words of the class_t and of its metaclass are written before the call.
//! The cache word becomes the native _objc_empty_cache, which the guest's import
//! already bound it to.  The word after it becomes zero: the x86 ABI calls it the
//! vtable and an older image binds _objc_empty_vtable there, while the arm64
//! runtime keeps its cache mask and flags in it.  And the data word, which on
//! disk points at the guest's class_ro_t, points at a copy of it on the host
//! heap whose method list and protocol list are replaced as below.  The guest's
//! own class_ro_t, in __objc_const, is never written.  The copy keeps the name,
//! flags, instance start and size, ivar list, ivar layouts and property list
//! exactly as the guest's, so the runtime reads ARC's ivarLayout and
//! weakIvarLayout as the compiler wrote them.  The runtime writes the low 32
//! bits of each ivar offset variable when it slides; an x86 compiler emits that
//! variable as 64 bits and reads all of them, and the upper half is zero on disk,
//! so the two agree.  A class whose layout is known when compiled, NSObject's
//! subclasses in one image, uses constant offsets instead, and NSObject is eight
//! bytes on both.
//!
//! ---- methods ----
//! Every method list, instance and class, is copied into an absolute list of
//! 24-byte entries on the host heap.  A relative list has to be copied anyway,
//! because its 32-bit offsets cannot reach a trampoline in ocerz's image, and a
//! relative list built by hand on the heap crashed the runtime's method scanner
//! at +initialize in the probe.  An entry's selector is sel_registerName of its
//! name, reached through the selector reference a relative entry names, already
//! canonical by then, or directly when the list says its selectors are direct.
//! Its types are the guest's own string, so method_getTypeEncoding answers the
//! x86 encoding, and its implementation is a callback slot bound to the guest's
//! function under the notation ocerz_objc_method_notation converts those types
//! to.  That is ocerz_objc_notation, which reads c as b - an x86 BOOL is a
//! signed char, where arm64's bool is B, which it also reads as b - and q as l,
//! the way an LP64 compiler encodes long, and a method's first two arguments,
//! self and _cmd, must be pointers.  A slot is the same slot for the same
//! function and notation, so a getter shared by two lists is one address.
//!
//! A method whose types do not cross - a long double, a union, a bitfield, more
//! arguments than the ABI engine carries - does not keep its class out.  Its
//! implementation is one native function, the same for every such method, and
//! the class, selector, types and reason are recorded beside it; called, it walks
//! the receiver's classes for the record and stops with OCERZ_BRIDGE_UNIMPL_EXIT
//! naming the method and why.  A method the callback bank has no slot left for
//! is bound to it the same way, so a full bank is a named refusal when the method
//! is called and not before.  The records are an append-only list read without
//! a lock.

use core::ffi::{c_char, c_int, c_void};
use core::ptr::{null_mut, write_bytes};

use crate::ffi::*;
use crate::ported::objcclass::common::*;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_read_class(addr: u64, out: *mut OcerzObjcClass) -> c_int {
    unsafe {
        write_bytes(out, 0, 1);
        if addr == 0 {
            return OCERZ_OBJC_NULL as c_int;
        }
        (*out).isa = oc_word(addr);
        (*out).superclass = oc_word(addr + 8);
        (*out).cache = oc_word(addr + 16);
        (*out).vtable = oc_word(addr + 24);
        let bits = oc_word(addr + 32);
        (*out).ro = bits & OC_FAST_DATA;
        (*out).swift = (bits & OC_FAST_SWIFT) as u32;
        if (*out).ro == 0 {
            return OCERZ_OBJC_NULL as c_int;
        }
        OCERZ_OBJC_OK as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_read_ro(addr: u64, out: *mut OcerzObjcRo) -> c_int {
    unsafe {
        write_bytes(out, 0, 1);
        if addr == 0 {
            return OCERZ_OBJC_NULL as c_int;
        }
        (*out).flags = oc_u32(addr);
        (*out).instance_start = oc_u32(addr + 4);
        (*out).instance_size = oc_u32(addr + 8);
        (*out).reserved = oc_u32(addr + 12);
        (*out).ivar_layout = oc_word(addr + 16);
        (*out).name = oc_word(addr + 24);
        (*out).base_methods = oc_word(addr + 32);
        (*out).base_protocols = oc_word(addr + 40);
        (*out).ivars = oc_word(addr + 48);
        (*out).weak_ivar_layout = oc_word(addr + 56);
        (*out).base_properties = oc_word(addr + 64);
        if (*out).flags & OC_RO_SWIFT_INIT != 0 {
            (*out).swift_initializer = oc_word(addr + OC_RO_BYTES as u64);
        }
        OCERZ_OBJC_OK as c_int
    }
}

unsafe fn oc_list(addr: u64, flagmask: u32, minsize: u32, out: *mut OcerzObjcList) -> c_int {
    unsafe {
        write_bytes(out, 0, 1);
        (*out).addr = addr;
        if addr == 0 {
            return OCERZ_OBJC_OK as c_int;
        }
        let word = oc_u32(addr);
        (*out).flags = word & flagmask;
        (*out).entsize = word & !flagmask;
        (*out).count = oc_u32(addr + 4);
        if (*out).count != 0 && (*out).entsize < minsize {
            return OCERZ_OBJC_BAD_LIST as c_int;
        }
        OCERZ_OBJC_OK as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_method_list(addr: u64, out: *mut OcerzObjcList) -> c_int {
    unsafe {
        let rc = oc_list(addr, OC_METHOD_FLAGS, 12, out);
        if rc != OCERZ_OBJC_OK as c_int || (*out).count == 0 {
            return rc;
        }
        if (*out).flags & OC_METHOD_RELATIVE != 0 {
            return if (*out).entsize == 12 {
                OCERZ_OBJC_OK as c_int
            } else {
                OCERZ_OBJC_BAD_LIST as c_int
            };
        }
        if (*out).entsize >= 24 {
            OCERZ_OBJC_OK as c_int
        } else {
            OCERZ_OBJC_BAD_LIST as c_int
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_method_at(
    list: *const OcerzObjcList,
    index: u32,
    out: *mut OcerzObjcMethod,
) -> c_int {
    unsafe {
        write_bytes(out, 0, 1);
        if (*list).addr == 0 || index >= (*list).count {
            return OCERZ_OBJC_BAD_LIST as c_int;
        }
        let e = (*list).addr + 8 + index as u64 * (*list).entsize as u64;
        if (*list).flags & OC_METHOD_RELATIVE == 0 {
            (*out).name = oc_word(e);
            (*out).types = oc_word(e + 8);
            (*out).imp = oc_word(e + 16);
            return OCERZ_OBJC_OK as c_int;
        }
        let ref_ = e.wrapping_add(oc_rel(e) as i64 as u64);
        (*out).name = if (*list).flags & OC_METHOD_DIRECT_SEL != 0 {
            ref_
        } else {
            oc_word(ref_)
        };
        (*out).types = (e + 4).wrapping_add(oc_rel(e + 4) as i64 as u64);
        let imp = oc_rel(e + 8);
        (*out).imp = if imp != 0 {
            (e + 8).wrapping_add(imp as i64 as u64)
        } else {
            0
        };
        OCERZ_OBJC_OK as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_ivar_list(addr: u64, out: *mut OcerzObjcList) -> c_int {
    unsafe { oc_list(addr, 0, 32, out) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_ivar_at(
    list: *const OcerzObjcList,
    index: u32,
    out: *mut OcerzObjcIvar,
) -> c_int {
    unsafe {
        write_bytes(out, 0, 1);
        if (*list).addr == 0 || index >= (*list).count {
            return OCERZ_OBJC_BAD_LIST as c_int;
        }
        let e = (*list).addr + 8 + index as u64 * (*list).entsize as u64;
        (*out).offset = oc_word(e);
        (*out).name = oc_word(e + 8);
        (*out).type_ = oc_word(e + 16);
        (*out).alignment = oc_u32(e + 24);
        (*out).size = oc_u32(e + 28);
        OCERZ_OBJC_OK as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_property_list(addr: u64, out: *mut OcerzObjcList) -> c_int {
    unsafe { oc_list(addr, 0, 16, out) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_property_at(
    list: *const OcerzObjcList,
    index: u32,
    out: *mut OcerzObjcProperty,
) -> c_int {
    unsafe {
        write_bytes(out, 0, 1);
        if (*list).addr == 0 || index >= (*list).count {
            return OCERZ_OBJC_BAD_LIST as c_int;
        }
        let e = (*list).addr + 8 + index as u64 * (*list).entsize as u64;
        (*out).name = oc_word(e);
        (*out).attributes = oc_word(e + 8);
        OCERZ_OBJC_OK as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_protocol_count(list: u64) -> u64 {
    unsafe {
        if list != 0 { oc_word(list) } else { 0 }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_protocol_ref(list: u64, index: u64) -> u64 {
    unsafe { oc_word(list + 8 + 8 * index) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_read_category(
    addr: u64,
    class_properties: c_int,
    out: *mut OcerzObjcCategory,
) -> c_int {
    unsafe {
        write_bytes(out, 0, 1);
        if addr == 0 {
            return OCERZ_OBJC_NULL as c_int;
        }
        (*out).name = oc_word(addr);
        (*out).cls = oc_word(addr + 8);
        (*out).instance_methods = oc_word(addr + 16);
        (*out).class_methods = oc_word(addr + 24);
        (*out).protocols = oc_word(addr + 32);
        (*out).instance_properties = oc_word(addr + 40);
        (*out).class_properties = if class_properties != 0 { oc_word(addr + 48) } else { 0 };
        OCERZ_OBJC_OK as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_read_protocol(
    addr: u64,
    out: *mut OcerzObjcProtocol,
) -> c_int {
    unsafe {
        write_bytes(out, 0, 1);
        if addr == 0 {
            return OCERZ_OBJC_NULL as c_int;
        }
        (*out).name = oc_word(addr + 8);
        (*out).protocols = oc_word(addr + 16);
        (*out).instance_methods = oc_word(addr + 24);
        (*out).class_methods = oc_word(addr + 32);
        (*out).optional_instance_methods = oc_word(addr + 40);
        (*out).optional_class_methods = oc_word(addr + 48);
        (*out).instance_properties = oc_word(addr + 56);
        (*out).size = oc_u32(addr + 64);
        (*out).flags = oc_u32(addr + 68);
        if (*out).size >= (OC_PROTOCOL_BASE + 8) as u32 {
            (*out).extended_types = oc_word(addr + 72);
        }
        if (*out).size >= (OC_PROTOCOL_BASE + 16) as u32 {
            (*out).demangled_name = oc_word(addr + 80);
        }
        if (*out).size >= (OC_PROTOCOL_BASE + 24) as u32 {
            (*out).class_properties = oc_word(addr + 88);
        }
        if (*out).name == 0 {
            return OCERZ_OBJC_NULL as c_int;
        }
        OCERZ_OBJC_OK as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_method_notation(
    types: *const c_char,
    out: *mut c_char,
    outlen: usize,
) -> c_int {
    unsafe {
        let mut nargs: c_int = 0;
        let rc = ocerz_objc_notation(
            types,
            out,
            outlen,
            &mut nargs,
            null_mut(),
            null_mut(),
        );
        if rc != OCERZ_OBJC_OK as c_int {
            return rc;
        }
        let mut sig = OcerzAbiSig::default();
        if ocerz_abi_parse(out, &mut sig) != OCERZ_OK as c_int {
            return OCERZ_OBJC_ENGINE as c_int;
        }
        if sig.nargs < 2 || sig.arg[0] != b'p' as c_char || sig.arg[1] != b'p' as c_char {
            return OCERZ_OBJC_NOT_METHOD as c_int;
        }
        OCERZ_OBJC_OK as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_property_attributes(
    attrs: *const c_char,
    out: *mut OcerzObjcAttribute,
    max: c_int,
    storage: *mut c_char,
    storagelen: usize,
) -> c_int {
    unsafe {
        let mut n: c_int = 0;
        let mut used: usize = 0;
        let mut p = if attrs.is_null() { c"".as_ptr() } else { attrs };
        while *p != 0 {
            while *p == b',' as c_char {
                p = p.add(1);
            }
            if *p == 0 {
                break;
            }
            if n >= max {
                return -1;
            }
            let value = p.add(1);
            let vlen = libc::strcspn(value, c",".as_ptr());
            if used + 2 + vlen + 1 > storagelen {
                return -1;
            }
            (*out.add(n as usize)).name = storage.add(used);
            *storage.add(used) = *p;
            used += 1;
            *storage.add(used) = 0;
            used += 1;
            (*out.add(n as usize)).value = storage.add(used);
            core::ptr::copy_nonoverlapping(value.cast::<u8>(), storage.add(used).cast::<u8>(), vlen);
            used += vlen;
            *storage.add(used) = 0;
            used += 1;
            n += 1;
            p = value.add(vlen);
        }
        n
    }
}

#[repr(C)]
struct OcOrder {
    cls: u64,
    index: c_int,
}

unsafe extern "C" fn oc_order_cmp(a: *const c_void, b: *const c_void) -> c_int {
    unsafe {
        let x = a.cast::<OcOrder>();
        let y = b.cast::<OcOrder>();
        if (*x).cls < (*y).cls {
            -1
        } else if (*x).cls > (*y).cls {
            1
        } else {
            (*x).index - (*y).index
        }
    }
}

unsafe fn oc_order_find(sorted: *const OcOrder, n: c_int, cls: u64) -> c_int {
    unsafe {
        let mut lo: c_int = 0;
        let mut hi: c_int = n;
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if (*sorted.add(mid as usize)).cls < cls {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if lo < n && (*sorted.add(lo as usize)).cls == cls {
            (*sorted.add(lo as usize)).index
        } else {
            -1
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_class_order(
    classes: *const u64,
    supers: *const u64,
    n: c_int,
    order: *mut c_int,
    culprit: *mut c_int,
) -> c_int {
    unsafe {
        if !culprit.is_null() {
            *culprit = -1;
        }
        if n <= 0 {
            return OCERZ_OBJC_OK as c_int;
        }
        let sorted = libc::malloc(n as usize * size_of::<OcOrder>()) as *mut OcOrder;
        let state = libc::calloc(n as usize, 1) as *mut u8;
        let stack = libc::malloc(n as usize * size_of::<c_int>()) as *mut c_int;
        if sorted.is_null() || state.is_null() || stack.is_null() {
            libc::free(sorted as *mut c_void);
            libc::free(state as *mut c_void);
            libc::free(stack as *mut c_void);
            return OCERZ_OBJC_ENGINE as c_int;
        }
        for i in 0..n {
            (*sorted.add(i as usize)).cls = *classes.add(i as usize);
            (*sorted.add(i as usize)).index = i;
        }
        libc::qsort(
            sorted as *mut c_void,
            n as usize,
            size_of::<OcOrder>(),
            Some(oc_order_cmp),
        );

        let mut emitted = 0;
        let mut rc = OCERZ_OBJC_OK as c_int;
        let mut i = 0;
        while i < n && rc == OCERZ_OBJC_OK as c_int {
            if *state.add(i as usize) != 0 {
                i += 1;
                continue;
            }
            let mut depth = 0;
            let mut k = i;
            while k >= 0 && *state.add(k as usize) == 0 {
                *state.add(k as usize) = 1;
                *stack.add(depth as usize) = k;
                depth += 1;
                let s = *supers.add(k as usize);
                k = if s != 0 {
                    oc_order_find(sorted, n, s)
                } else {
                    -1
                };
            }
            if k >= 0 && *state.add(k as usize) == 1 {
                rc = OCERZ_OBJC_CYCLE as c_int;
                if !culprit.is_null() {
                    *culprit = k;
                }
                break;
            }
            while depth > 0 {
                depth -= 1;
                let j = *stack.add(depth as usize);
                *state.add(j as usize) = 2;
                *order.add(emitted) = j;
                emitted += 1;
            }
            i += 1;
        }
        libc::free(sorted as *mut c_void);
        libc::free(state as *mut c_void);
        libc::free(stack as *mut c_void);
        rc
    }
}

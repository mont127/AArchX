//! ---- what is refused ----
//! A class is refused by name, and the process stopped, when its class_ro_t
//! already carries the flags only a running runtime sets; when its metaclass is
//! not a guest class marked as one; and when its superclass is a guest class
//! ocerz has not defined, including one in a chain of superclasses that comes
//! back to itself.  A class whose superclass is null without its class_ro_t
//! saying it is a root, which is what a weak-linked superclass the host lacks
//! leaves, is left out with a log line the way the native runtime leaves it
//! unrealized.  A root class is defined: the Swift runtime's own root,
//! Swift._SwiftObject, is one, and every Swift class descends from it.  Within
//! an image the class list is put in superclass order first: the classes are
//! sorted by address and each chain is walked to the first superclass outside
//! the list, so a subclass listed before its superclass waits for it.  A
//! superclass in another guest image is already defined, since the loader
//! defines a dylib as it loads it and loads dependencies first.
//!
//! ---- Swift classes ----
//! A Swift class is an Objective-C class with Swift metadata around it.  The
//! low bits of its data word say so, 2 for the stable ABI, and Swift's own
//! fields follow the class_t, among them its flags, instance size, type
//! descriptor and vtable.  The word just before the class_t is its value
//! witness table and the one before that its destroy function.  The guest's x86
//! libswiftCore reads and writes all of it (src/objcbridge.c says where that
//! library comes from), and the native runtime needs only the Objective-C part.
//! So a Swift class is defined like any other, and the data word that points at
//! the copy keeps the Swift bits, which the guest's runtime tests to tell a
//! Swift class from a pure Objective-C one and the native runtime to know it
//! has one.  Three things differ.
//!
//! A class whose class_ro_t has a Swift metadata initializer, flag 0x40, is
//! incomplete on disk.  The guest's runtime completes it on first use and then
//! registers it through _objc_realizeClassFromSwift, so it waits instead of
//! being defined at image load, and a subclass of a waiting class in the same
//! list waits with it; ocerz_objcbridge_prepare_class installs the pair when the
//! runtime's call arrives.  The copy of its class_ro_t has the flag cleared, so
//! the native runtime never calls the initializer, an x86 function, itself.
//! ocerz_objc_read_ro reads the initializer's address from the word after the
//! 72 bytes.
//!
//! The native runtime knows only classes it was told about, and the Swift
//! runtime reaches some without passing anything ocerz watches.  A generic
//! class the compiler specialized ahead of time, _ContiguousArrayStorage of one
//! element type for instance, is in no class list: its metadata accessor hands
//! it straight to objc_opt_self, and the native runtime stops with "Attempt to
//! use unknown class".  ocerz_objcbridge_ensure_object and
//! ocerz_objcbridge_ensure_class define such a class, superclasses first, the
//! first time a special or a send hands it, or an object of it, to the native
//! runtime (src/objcbridge.c).
//!
//! The last difference is the end of an object's life.  Both runtimes keep a
//! Swift object's reference count in the same 64-bit header word, and the
//! native libobjc passes a Swift object's retains and releases to the native
//! swift_retain and swift_release.  That is right until a release reaches zero,
//! when the native runtime calls the class's destroy function, which is x86
//! code.  An autorelease pool drained natively ends there, and so does any
//! native holder of a guest Swift object letting go of the last reference.  The
//! call faults, since guest code is not executable to the host, and the fault
//! handler in src/vm.c asks ocerz_objcbridge_swift_destroy_fault whether the
//! fault is exactly this: a pc inside the guest reservation, and an object in
//! x20, the arm64 register Swift passes a destroy function its object in, whose
//! isa has that pc as its destroy word.  The three words are read with
//! mach_vm_read_overwrite, since a signal handler must not fault on a bad x20.
//! When they match, the handler resumes the thread in
//! ocerz_objcbridge_swift_destroy with the object and the function, as though
//! the native runtime had called that: it runs the guest function with the
//! object in R13, the x86-64 register for the same purpose, on the thread's
//! guest cpu, attaching one to a native thread the way a callback does, and
//! returns to the native runtime's return address.  Any other jump into guest
//! memory still stops with the bridge-fault report.  A guest's own objc_release
//! avoids the fault altogether: ocerz_objcbridge_guest_swift_object recognizes
//! a guest Swift object by a raw isa, the stable Swift bit in its data word, the
//! Swift-refcounting flag in its class flags, and a destroy function inside the
//! guest reservation, which separates the guest's classes from native Swift
//! ones a framework hands back, and src/objcbridge.c sends such a release to the
//! guest's swift_release.
//!
//! ---- categories ----
//! __objc_catlist and then __objc_catlist2 are applied in list order.  Each
//! instance method is class_replaceMethod on the class, which adds the method
//! when the class itself has none of that name, overriding a superclass's, and
//! replaces the implementation of the class's own when it has one; the last
//! category in the lists wins, as natively.  Class methods go to the metaclass
//! the same way, protocols through class_addProtocol, properties through
//! class_addProperty, and class properties to the metaclass when the image
//! info's flag says category_t carries them.  A property the class or a
//! superclass already declares keeps that declaration.  class_replaceProperty
//! would rewrite the declaration it finds, on whichever class in the chain it
//! finds it, and a native class's property list lies in the shared cache,
//! read-only: the Swift runtime an app bundles for systems before 10.14.4
//! declares count again on classes under NSArray, NSDictionary and NSSet, and
//! the NSObject protocol's properties again on NSNumber, NSDictionary and Core
//! Data's classes, and iGlance stopped with a bus error while that libswiftCore
//! loaded.  Natively the category's declaration would come first for
//! class_getProperty; here the one already there does, which differs at most
//! in the attribute string.  A category may be on a native class or on a
//! guest class defined earlier.  One whose class is null, as a category on a
//! missing weak-linked class is, is left out with a log line; one on a guest
//! class ocerz has not defined is refused by name.  The static linker already
//! merges a category into its class's own method lists when both are in one
//! image, so such a category arrives as the class's own methods.
//!
//! ---- protocols ----
//! A guest image carries its own protocol_t for every protocol it names,
//! NSObject and NSCopying included, and none of them is an object the native
//! runtime knows.  Each one in __objc_protolist, and each one a list or
//! reference names, is resolved once: objc_getProtocol by name when the host has
//! it, and otherwise objc_allocateProtocol, with the protocols it adopts
//! resolved and registered first - protocol_addProtocol refuses one still under
//! construction - its four method description lists added with the guest's
//! types, its instance and class properties, and objc_registerProtocol.  The
//! runtime has no call that sets a protocol's extended method types, the
//! encodings that spell out a block argument's own arguments and which
//! NSXPCInterface refuses a protocol without, so before the protocol is
//! registered the guest's array of them is copied, as host pointers, into the
//! word the runtime's own protocol_t keeps them in, at offset 72 when the
//! protocol's size says it has that word.  The runtime indexes that array by
//! position across the four method lists in the order they were added, which is
//! the order the compiler lays them down in.  Every
//! word of __objc_protorefs, which is what @protocol compiles to, is rewritten to
//! the native protocol, and a class's protocol list is copied with native
//! protocols in it.
//!
//! ---- +load ----
//! The runtime calls +load only for images dyld loaded, so ocerz queues them.
//! For each class __objc_nlclslist names, its guest superclasses are queued
//! first, each class once, with the +load of the class's own class_ro_t - never
//! a category's - and then each category __objc_nlcatlist names that has a
//! +load of its own.  ocerz_objcbridge_run_loads runs the queue in that order
//! inside one native autorelease pool, calling each guest implementation
//! directly as guest code on the current thread, the class in rdi and the load
//! selector in rsi, which is how the runtime calls one.  The loader runs it
//! once, after every load-time image is defined and before main, so a
//! dependency's +load methods run before its dependents'.  A +load stays in the
//! method lists as well, as natively.  +initialize needs nothing: the runtime
//! sends it lazily through the method list like any other message.
//!
//! ---- lifetime ----
//! An image, by header address, is defined once.  Nothing here is ever freed:
//! the copies are what the runtime reads for as long as the class exists, which
//! is the life of the process.  The tables are guarded by one mutex, which the
//! loader and the Swift paths above both take.  .cxx_construct and .cxx_destruct
//! are ordinary methods; the runtime calls them with the object as the only
//! argument, so the slot hands the guest whatever x1 held as _cmd, which neither
//! reads.  An image a dlopen loads is defined the same way, after all of its
//! fixups have bound, and every +load its definition queues is tagged with that
//! image, so ocerz_objcbridge_run_image_loads can run one image's +load methods
//! just before that image's initializers and leave any other image's queued,
//! which is dyld's order: every new image mapped into the runtime first, then
//! each one's +load and initializers, dependencies first.  A plug-in whose class
//! subclasses a class of the main executable finds it defined, because the main
//! image was defined before main ran.  OCERZ_OBJCLOG prints each class,
//! category, protocol and method as it is defined, each method that cannot
//! cross, and each +load as it runs.

use core::ffi::{c_char, c_int, c_uint, c_void};
use core::mem::{offset_of, size_of, zeroed};
use core::ptr::{copy_nonoverlapping, null, null_mut, write_bytes};
use core::sync::atomic::{AtomicPtr, Ordering};

use crate::ffi::*;
use crate::ported::objcclass::common::*;

unsafe extern "C" {
    fn mach_vm_read_overwrite(
        target: libc::mach_port_t,
        address: libc::mach_vm_address_t,
        size: libc::mach_vm_size_t,
        data: libc::mach_vm_address_t,
        outsize: *mut libc::mach_vm_size_t,
    ) -> libc::kern_return_t;
}

#[repr(C)]
struct OcSym {
    name: *const c_char,
    addr: AtomicPtr<c_void>,
}

unsafe impl Sync for OcSym {}

macro_rules! oc_sym {
    ($name:literal) => {
        OcSym {
            name: concat!($name, "\0").as_ptr() as *const c_char,
            addr: AtomicPtr::new(null_mut()),
        }
    };
}

static G_OC_READ_CLASS_PAIR: OcSym = oc_sym!("objc_readClassPair");
static G_OC_EMPTY_CACHE: OcSym = oc_sym!("_objc_empty_cache");
static G_OC_SEL_REGISTER_NAME: OcSym = oc_sym!("sel_registerName");
static G_OC_SEL_GET_NAME: OcSym = oc_sym!("sel_getName");
static G_OC_OBJECT_GET_CLASS: OcSym = oc_sym!("object_getClass");
static G_OC_CLASS_GET_NAME: OcSym = oc_sym!("class_getName");
static G_OC_CLASS_GET_SUPERCLASS: OcSym = oc_sym!("class_getSuperclass");
static G_OC_CLASS_IS_META_CLASS: OcSym = oc_sym!("class_isMetaClass");
static G_OC_CLASS_REPLACE_METHOD: OcSym = oc_sym!("class_replaceMethod");
static G_OC_CLASS_ADD_PROTOCOL: OcSym = oc_sym!("class_addProtocol");
static G_OC_CLASS_ADD_PROPERTY: OcSym = oc_sym!("class_addProperty");
static G_OC_OBJC_GET_PROTOCOL: OcSym = oc_sym!("objc_getProtocol");
static G_OC_OBJC_ALLOCATE_PROTOCOL: OcSym = oc_sym!("objc_allocateProtocol");
static G_OC_OBJC_REGISTER_PROTOCOL: OcSym = oc_sym!("objc_registerProtocol");
static G_OC_PROTOCOL_ADD_PROTOCOL: OcSym = oc_sym!("protocol_addProtocol");
static G_OC_PROTOCOL_ADD_METHOD_DESCRIPTION: OcSym =
    oc_sym!("protocol_addMethodDescription");
static G_OC_PROTOCOL_ADD_PROPERTY: OcSym = oc_sym!("protocol_addProperty");
static G_OC_POOL_PUSH: OcSym = oc_sym!("objc_autoreleasePoolPush");
static G_OC_POOL_POP: OcSym = oc_sym!("objc_autoreleasePoolPop");

unsafe fn oc_sym(s: *const OcSym) -> *mut c_void {
    unsafe {
        let mut a = (*s).addr.load(Ordering::SeqCst);
        if a.is_null() {
            a = ocerz_bridge_host_symbol(
                OCERZ_OBJC_LIBOBJC.as_ptr() as *const c_char,
                (*s).name,
            );
            if !a.is_null() {
                (*s).addr.store(a, Ordering::SeqCst);
            }
        }
        a
    }
}

unsafe fn oc_need(s: *const OcSym) -> *mut c_void {
    unsafe {
        let a = oc_sym(s);
        if a.is_null() {
            oc_stop!(
                "the host %s has no %s, which defining a guest class needs",
                OCERZ_OBJC_LIBOBJC.as_ptr() as *const c_char,
                (*s).name
            );
        }
        a
    }
}

unsafe fn oc_sel(name: *const c_char) -> *mut c_void {
    unsafe {
        core::mem::transmute::<*mut c_void, unsafe extern "C" fn(*const c_char) -> *mut c_void>(
            oc_need(&raw const G_OC_SEL_REGISTER_NAME),
        )(name)
    }
}

unsafe fn oc_sel_name(sel: *mut c_void) -> *const c_char {
    unsafe {
        core::mem::transmute::<*mut c_void, unsafe extern "C" fn(*mut c_void) -> *const c_char>(
            oc_need(&raw const G_OC_SEL_GET_NAME),
        )(sel)
    }
}

unsafe fn oc_object_getClass(obj: *mut c_void) -> *mut c_void {
    unsafe {
        core::mem::transmute::<*mut c_void, unsafe extern "C" fn(*mut c_void) -> *mut c_void>(
            oc_need(&raw const G_OC_OBJECT_GET_CLASS),
        )(obj)
    }
}

unsafe fn oc_class_name(cls: *mut c_void) -> *const c_char {
    unsafe {
        core::mem::transmute::<*mut c_void, unsafe extern "C" fn(*mut c_void) -> *const c_char>(
            oc_need(&raw const G_OC_CLASS_GET_NAME),
        )(cls)
    }
}

unsafe fn oc_superclass(cls: *mut c_void) -> *mut c_void {
    unsafe {
        core::mem::transmute::<*mut c_void, unsafe extern "C" fn(*mut c_void) -> *mut c_void>(
            oc_need(&raw const G_OC_CLASS_GET_SUPERCLASS),
        )(cls)
    }
}

unsafe fn oc_is_meta(cls: *mut c_void) -> c_int {
    unsafe {
        core::mem::transmute::<*mut c_void, unsafe extern "C" fn(*mut c_void) -> bool>(
            oc_need(&raw const G_OC_CLASS_IS_META_CLASS),
        )(cls) as c_int
    }
}

#[repr(C)]
struct OcMap {
    keys: *mut u64,
    vals: *mut u64,
    cap: usize,
    n: usize,
}

#[inline(always)]
fn oc_hash(mut k: u64, cap: usize) -> usize {
    k = k.wrapping_mul(0x9e3779b97f4a7c15);
    ((k ^ (k >> 29)) as usize) & (cap - 1)
}

unsafe fn oc_map_find(m: *const OcMap, key: u64) -> *mut u64 {
    unsafe {
        if (*m).cap == 0 {
            return null_mut();
        }
        let mut i = oc_hash(key, (*m).cap);
        loop {
            if *(*m).keys.add(i) == key {
                return (*m).vals.add(i);
            }
            if *(*m).keys.add(i) == 0 {
                return null_mut();
            }
            i = (i + 1) & ((*m).cap - 1);
        }
    }
}

unsafe fn oc_map_put(m: *mut OcMap, key: u64, val: u64) -> *mut u64 {
    unsafe {
        if ((*m).n + 1) * 2 > (*m).cap {
            let cap = if (*m).cap != 0 { (*m).cap * 2 } else { 256 };
            let keys = libc::calloc(cap, size_of::<u64>()) as *mut u64;
            let vals = libc::calloc(cap, size_of::<u64>()) as *mut u64;
            if keys.is_null() || vals.is_null() {
                oc_stop!("no memory for the table of guest Objective-C definitions");
            }
            for i in 0..(*m).cap {
                if *(*m).keys.add(i) == 0 {
                    continue;
                }
                let mut j = oc_hash(*(*m).keys.add(i), cap);
                while *keys.add(j) != 0 {
                    j = (j + 1) & (cap - 1);
                }
                *keys.add(j) = *(*m).keys.add(i);
                *vals.add(j) = *(*m).vals.add(i);
            }
            libc::free((*m).keys as *mut c_void);
            libc::free((*m).vals as *mut c_void);
            (*m).keys = keys;
            (*m).vals = vals;
            (*m).cap = cap;
        }
        let mut i = oc_hash(key, (*m).cap);
        while *(*m).keys.add(i) != 0 && *(*m).keys.add(i) != key {
            i = (i + 1) & ((*m).cap - 1);
        }
        if *(*m).keys.add(i) == 0 {
            *(*m).keys.add(i) = key;
            (*m).n += 1;
        }
        *(*m).vals.add(i) = val;
        (*m).vals.add(i)
    }
}

#[repr(C)]
struct OcDefined {
    meta: u64,
    load: u64,
    scheduled: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct OcLoad {
    cls: u64,
    imp: u64,
    image: u64,
    category: c_int,
}

#[repr(C)]
struct OcDead {
    next: *mut OcDead,
    cls: *mut c_void,
    sel: *mut c_void,
    types: *const c_char,
    why: [c_char; 0],
}

static mut G_OC_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
static mut G_OC_IMAGES: OcMap = OcMap {
    keys: null_mut(),
    vals: null_mut(),
    cap: 0,
    n: 0,
};
static mut G_OC_CLASSES: OcMap = OcMap {
    keys: null_mut(),
    vals: null_mut(),
    cap: 0,
    n: 0,
};
static mut G_OC_PROTOCOLS: OcMap = OcMap {
    keys: null_mut(),
    vals: null_mut(),
    cap: 0,
    n: 0,
};
static mut G_OC_SKIPPED: OcMap = OcMap {
    keys: null_mut(),
    vals: null_mut(),
    cap: 0,
    n: 0,
};
static mut G_OC_DEFERRED: OcMap = OcMap {
    keys: null_mut(),
    vals: null_mut(),
    cap: 0,
    n: 0,
};
static mut G_OC_LOADS: *mut OcLoad = null_mut();
static mut G_OC_LOADS_N: usize = 0;
static mut G_OC_LOADS_CAP: usize = 0;
static mut G_OC_DEFINING: u64 = 0;
static G_OC_DEAD: AtomicPtr<OcDead> = AtomicPtr::new(null_mut());

unsafe fn oc_defined(cls: u64) -> *mut OcDefined {
    unsafe {
        let v = oc_map_find(&raw const G_OC_CLASSES, cls);
        if v.is_null() {
            null_mut()
        } else {
            *v as *mut OcDefined
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objcbridge_is_defined(cls: u64) -> c_int {
    unsafe {
        libc::pthread_mutex_lock(&raw mut G_OC_LOCK);
        let yes = !oc_defined(cls).is_null() as c_int;
        libc::pthread_mutex_unlock(&raw mut G_OC_LOCK);
        yes
    }
}

unsafe extern "C" fn oc_dead_imp(selff: *mut c_void, cmd: *mut c_void) {
    unsafe {
        let start = if selff.is_null() {
            null_mut()
        } else {
            oc_object_getClass(selff)
        };
        let mut c = start;
        while !c.is_null() {
            let mut d = G_OC_DEAD.load(Ordering::SeqCst);
            while !d.is_null() {
                if (*d).cls == c && (*d).sel == cmd {
                    oc_stop!(
                        "%c[%s %s] is guest code native code cannot call: its type encoding %s has %s",
                        if oc_is_meta(c) != 0 { b'+' as c_int } else { b'-' as c_int },
                        oc_class_name(c),
                        oc_sel_name(cmd),
                        if (*d).types.is_null() {
                            c"(none)".as_ptr()
                        } else {
                            (*d).types
                        },
                        (*d).why.as_ptr()
                    );
                }
                d = (*d).next;
            }
            c = oc_superclass(c);
        }
        oc_stop!(
            "a guest method native code cannot call was called%s%s",
            if start.is_null() {
                c"".as_ptr()
            } else {
                c" on a ".as_ptr()
            },
            if start.is_null() {
                c"".as_ptr()
            } else {
                oc_class_name(start)
            }
        );
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objcbridge_dead_imp() -> *mut c_void {
    oc_dead_imp as *mut c_void
}

unsafe fn oc_bury(
    cls: *mut c_void,
    clsname: *const c_char,
    meta: c_int,
    sel: *mut c_void,
    types: *const c_char,
    why: *const c_char,
) -> *mut c_void {
    unsafe {
        let len = libc::strlen(why);
        let d = libc::malloc(size_of::<OcDead>() + len + 1) as *mut OcDead;
        if d.is_null() {
            oc_stop!("no memory to record why a guest method cannot be called");
        }
        (*d).cls = cls;
        (*d).sel = sel;
        (*d).types = types;
        copy_nonoverlapping(why.cast::<u8>(), (*d).why.as_mut_ptr().cast::<u8>(), len + 1);
        (*d).next = G_OC_DEAD.load(Ordering::SeqCst);
        G_OC_DEAD.store(d, Ordering::SeqCst);
        if oc_logging() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: OBJCLOG[%d] define %c[%s %s] %s has no crossing: %s\n".as_ptr(),
                libc::getpid(),
                if meta != 0 { b'+' as c_int } else { b'-' as c_int },
                clsname,
                oc_sel_name(sel),
                if types.is_null() { c"(none)".as_ptr() } else { types },
                why,
            );
        }
        oc_dead_imp as *mut c_void
    }
}

unsafe fn oc_imp(
    cls: *mut c_void,
    clsname: *const c_char,
    meta: c_int,
    sel: *mut c_void,
    types: *const c_char,
    imp: u64,
) -> *mut c_void {
    unsafe {
        if imp == 0 {
            return null_mut();
        }
        if ocerz_abi_is_guest_code(imp) == 0 {
            return ocerz_g2h(imp);
        }
        let mut notation = [0 as c_char; OCERZ_OBJC_NOTATION_MAX as usize];
        let rc = ocerz_objc_method_notation(types, notation.as_mut_ptr(), notation.len());
        if rc != OCERZ_OBJC_OK as c_int {
            return oc_bury(cls, clsname, meta, sel, types, ocerz_objc_refusal(rc));
        }
        let tramp = ocerz_abi_callback_intern(imp, notation.as_ptr());
        if tramp.is_null() {
            return oc_bury(
                cls,
                clsname,
                meta,
                sel,
                types,
                c"no slot left in the callback bank".as_ptr(),
            );
        }
        if oc_logging() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: OBJCLOG[%d] define %c[%s %s] %s at %#llx\n".as_ptr(),
                libc::getpid(),
                if meta != 0 { b'+' as c_int } else { b'-' as c_int },
                clsname,
                oc_sel_name(sel),
                notation.as_ptr(),
                imp,
            );
        }
        tramp
    }
}

unsafe fn oc_alloc(bytes: usize, what: *const c_char, clsname: *const c_char) -> *mut c_void {
    unsafe {
        let p = libc::calloc(1, bytes);
        if p.is_null() {
            oc_stop!("no memory for the %s of guest class %s", what, clsname);
        }
        p
    }
}

unsafe fn oc_methods(
    list: u64,
    cls: *mut c_void,
    clsname: *const c_char,
    meta: c_int,
    load: *mut u64,
) -> *mut c_void {
    unsafe {
        let mut l: OcerzObjcList = zeroed();
        let rc = ocerz_objc_method_list(list, &mut l);
        if rc != OCERZ_OBJC_OK as c_int {
            oc_stop!(
                "guest class %s has a %s method list at %#llx whose entries are %u bytes, which ocerz cannot read",
                clsname,
                if meta != 0 {
                    c"class".as_ptr()
                } else {
                    c"instance".as_ptr()
                },
                list,
                l.entsize
            );
        }
        if l.count == 0 {
            return null_mut();
        }
        let copy = oc_alloc(8 + 24 * l.count as usize, c"method list".as_ptr(), clsname)
            .cast::<u8>();
        let head: u32 = 24;
        let count: u32 = l.count;
        copy_nonoverlapping(&head as *const u32 as *const u8, copy, 4);
        copy_nonoverlapping(&count as *const u32 as *const u8, copy.add(4), 4);
        let load_sel = if load.is_null() {
            null_mut()
        } else {
            oc_sel(c"load".as_ptr())
        };
        for i in 0..l.count {
            let mut m: OcerzObjcMethod = zeroed();
            ocerz_objc_method_at(&l, i, &mut m);
            let name = oc_str(m.name);
            if name.is_null() {
                oc_stop!(
                    "guest class %s has a %s method with no selector",
                    clsname,
                    if meta != 0 {
                        c"class".as_ptr()
                    } else {
                        c"instance".as_ptr()
                    }
                );
            }
            let sel = oc_sel(name);
            let types = oc_str(m.types);
            let imp = oc_imp(cls, clsname, meta, sel, types, m.imp);
            if !load.is_null() && sel == load_sel && m.imp != 0 {
                *load = m.imp;
            }
            let w: [u64; 3] = [sel as u64, types as u64, imp as u64];
            copy_nonoverlapping(
                w.as_ptr().cast::<u8>(),
                copy.add(8 + 24 * i as usize),
                size_of::<[u64; 3]>(),
            );
        }
        copy as *mut c_void
    }
}

unsafe fn oc_protocol(ref_: u64, depth: c_int) -> *mut c_void {
    unsafe {
        if ref_ == 0 {
            return null_mut();
        }
        if !oc_is_guest(ref_) {
            return ocerz_g2h(ref_);
        }
        let known = oc_map_find(&raw const G_OC_PROTOCOLS, ref_);
        if !known.is_null() {
            return *known as *mut c_void;
        }

        let mut gp: OcerzObjcProtocol = zeroed();
        if ocerz_objc_read_protocol(ref_, &mut gp) != OCERZ_OBJC_OK as c_int {
            oc_stop!("the guest protocol at %#llx has no name", ref_);
        }
        let name = oc_str(gp.name);
        let native = core::mem::transmute::<
            *mut c_void,
            unsafe extern "C" fn(*const c_char) -> *mut c_void,
        >(oc_need(&raw const G_OC_OBJC_GET_PROTOCOL))(name);
        if !native.is_null() {
            oc_map_put(&raw mut G_OC_PROTOCOLS, ref_, native as u64);
            return native;
        }
        if depth > 64 {
            oc_stop!("guest protocol %s adopts protocols more than 64 deep", name);
        }

        let made = core::mem::transmute::<
            *mut c_void,
            unsafe extern "C" fn(*const c_char) -> *mut c_void,
        >(oc_need(&raw const G_OC_OBJC_ALLOCATE_PROTOCOL))(name);
        if made.is_null() {
            oc_stop!("the native runtime would not allocate guest protocol %s", name);
        }
        oc_map_put(&raw mut G_OC_PROTOCOLS, ref_, made as u64);
        let n = ocerz_objc_protocol_count(gp.protocols);
        let mut i = 0u64;
        while i < n {
            let adopted = oc_protocol(ocerz_objc_protocol_ref(gp.protocols, i), depth + 1);
            if !adopted.is_null() {
                core::mem::transmute::<
                    *mut c_void,
                    unsafe extern "C" fn(*mut c_void, *mut c_void),
                >(oc_need(&raw const G_OC_PROTOCOL_ADD_PROTOCOL))(made, adopted);
            }
            i += 1;
        }
        oc_protocol_methods(made, gp.instance_methods, 1, 1, name);
        oc_protocol_methods(made, gp.class_methods, 1, 0, name);
        oc_protocol_methods(made, gp.optional_instance_methods, 0, 1, name);
        oc_protocol_methods(made, gp.optional_class_methods, 0, 0, name);
        oc_protocol_properties(made, gp.instance_properties, 1, name);
        oc_protocol_properties(made, gp.class_properties, 0, name);
        oc_protocol_extended_types(made, &gp, name);
        core::mem::transmute::<*mut c_void, unsafe extern "C" fn(*mut c_void)>(
            oc_need(&raw const G_OC_OBJC_REGISTER_PROTOCOL),
        )(made);
        if oc_logging() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: OBJCLOG[%d] define protocol %s\n".as_ptr(),
                libc::getpid(),
                name,
            );
        }
        made
    }
}

unsafe fn oc_protocol_methods(
    proto: *mut c_void,
    list: u64,
    required: c_int,
    instance: c_int,
    name: *const c_char,
) {
    unsafe {
        let mut l: OcerzObjcList = zeroed();
        if ocerz_objc_method_list(list, &mut l) != OCERZ_OBJC_OK as c_int {
            oc_stop!(
                "guest protocol %s has a method list at %#llx whose entries are %u bytes, which ocerz cannot read",
                name,
                list,
                l.entsize
            );
        }
        for i in 0..l.count {
            let mut m: OcerzObjcMethod = zeroed();
            ocerz_objc_method_at(&l, i, &mut m);
            if m.name == 0 {
                continue;
            }
            core::mem::transmute::<
                *mut c_void,
                unsafe extern "C" fn(*mut c_void, *mut c_void, *const c_char, bool, bool),
            >(oc_need(&raw const G_OC_PROTOCOL_ADD_METHOD_DESCRIPTION))(
                proto,
                oc_sel(oc_str(m.name)),
                oc_str(m.types),
                required != 0,
                instance != 0,
            );
        }
    }
}

unsafe fn oc_attrs(
    attrs: u64,
    out: *mut OcerzObjcAttribute,
    storage: *mut c_char,
    len: usize,
    owner: *const c_char,
    prop: *const c_char,
) -> c_int {
    unsafe {
        let n = ocerz_objc_property_attributes(oc_str(attrs), out, OC_ATTRS_MAX as c_int, storage, len);
        if n < 0 {
            oc_stop!(
                "the property %s of %s has attributes \"%.200s\" longer than ocerz reads",
                prop,
                owner,
                oc_str(attrs)
            );
        }
        n
    }
}

unsafe fn oc_protocol_properties(
    proto: *mut c_void,
    list: u64,
    instance: c_int,
    name: *const c_char,
) {
    unsafe {
        let mut l: OcerzObjcList = zeroed();
        if ocerz_objc_property_list(list, &mut l) != OCERZ_OBJC_OK as c_int {
            oc_stop!(
                "guest protocol %s has a property list whose entries are %u bytes",
                name,
                l.entsize
            );
        }
        for i in 0..l.count {
            let mut p: OcerzObjcProperty = zeroed();
            let mut attrs: [OcerzObjcAttribute; OC_ATTRS_MAX] = zeroed();
            let mut storage = [0 as c_char; 1024];
            ocerz_objc_property_at(&l, i, &mut p);
            let n = oc_attrs(
                p.attributes,
                attrs.as_mut_ptr(),
                storage.as_mut_ptr(),
                storage.len(),
                name,
                oc_str(p.name),
            );
            core::mem::transmute::<
                *mut c_void,
                unsafe extern "C" fn(
                    *mut c_void,
                    *const c_char,
                    *const OcerzObjcAttribute,
                    c_uint,
                    bool,
                    bool,
                ),
            >(oc_need(&raw const G_OC_PROTOCOL_ADD_PROPERTY))(
                proto,
                oc_str(p.name),
                attrs.as_ptr(),
                n as c_uint,
                true,
                instance != 0,
            );
        }
    }
}

unsafe fn oc_protocol_extended_types(
    proto: *mut c_void,
    gp: *const OcerzObjcProtocol,
    name: *const c_char,
) {
    unsafe {
        let lists: [u64; 4] = [
            (*gp).instance_methods,
            (*gp).class_methods,
            (*gp).optional_instance_methods,
            (*gp).optional_class_methods,
        ];
        let mut n: u64 = 0;
        if (*gp).extended_types == 0 {
            return;
        }
        for k in 0..4 {
            let mut l: OcerzObjcList = zeroed();
            if lists[k] != 0
                && ocerz_objc_method_list(lists[k], &mut l) == OCERZ_OBJC_OK as c_int
            {
                n += l.count as u64;
            }
        }
        let mut size: u32 = 0;
        copy_nonoverlapping(
            (proto as *const u8).add(OC_PROTOCOL_BASE - 8),
            &mut size as *mut u32 as *mut u8,
            size_of::<u32>(),
        );
        if n == 0 || size < (OC_PROTOCOL_BASE + 8) as u32 {
            return;
        }
        let types = oc_alloc(
            n as usize * size_of::<*const c_char>(),
            c"extended method types".as_ptr(),
            name,
        )
        .cast::<*const c_char>();
        for i in 0..n {
            *types.add(i as usize) = oc_str(oc_word((*gp).extended_types + 8 * i));
        }
        copy_nonoverlapping(
            &types as *const *mut *const c_char as *const u8,
            (proto as *mut u8).add(OC_PROTOCOL_BASE),
            size_of::<*const c_char>(),
        );
    }
}

unsafe fn oc_protocol_list(list: u64, owner: *const c_char) -> *mut c_void {
    unsafe {
        let n = ocerz_objc_protocol_count(list);
        if list == 0 || n == 0 {
            return null_mut();
        }
        let copy = oc_alloc(8 * (n as usize + 1), c"protocol list".as_ptr(), owner).cast::<u64>();
        *copy = n;
        for i in 0..n {
            *copy.add(i as usize + 1) =
                oc_protocol(ocerz_objc_protocol_ref(list, i), 0) as u64;
        }
        copy as *mut c_void
    }
}

#[repr(C)]
struct OcSect {
    addr: u64,
    size: u64,
}

#[repr(C)]
struct OcImage {
    classlist: OcSect,
    nlclslist: OcSect,
    catlist: OcSect,
    catlist2: OcSect,
    nlcatlist: OcSect,
    protolist: OcSect,
    protorefs: OcSect,
    imageinfo: OcSect,
}

#[repr(C)]
struct OcMachHeader64 {
    magic: u32,
    cputype: i32,
    cpusubtype: i32,
    filetype: u32,
    ncmds: u32,
    sizeofcmds: u32,
    flags: u32,
    reserved: u32,
}

#[repr(C)]
struct OcLoadCommand {
    cmd: u32,
    cmdsize: u32,
}

#[repr(C)]
struct OcSegmentCommand64 {
    cmd: u32,
    cmdsize: u32,
    segname: [c_char; 16],
    vmaddr: u64,
    vmsize: u64,
    fileoff: u64,
    filesize: u64,
    maxprot: i32,
    initprot: i32,
    nsects: u32,
    flags: u32,
}

#[repr(C)]
struct OcSection64 {
    sectname: [c_char; 16],
    segname: [c_char; 16],
    addr: u64,
    size: u64,
    offset: u32,
    align: u32,
    reloff: u32,
    nreloc: u32,
    flags: u32,
    reserved1: u32,
    reserved2: u32,
    reserved3: u32,
}

const OC_MH_MAGIC_64: u32 = 0xfeed_facf;
const OC_LC_SEGMENT_64: u32 = 0x19;

unsafe fn oc_scan(mh: *const u8, slide: i64, im: *mut OcImage) {
    const K_SECTS: [(&[u8; 16], usize); 8] = [
        (b"__objc_classlist", offset_of!(OcImage, classlist)),
        (b"__objc_nlclslist", offset_of!(OcImage, nlclslist)),
        (b"__objc_catlist\0\0", offset_of!(OcImage, catlist)),
        (b"__objc_catlist2\0", offset_of!(OcImage, catlist2)),
        (b"__objc_nlcatlist", offset_of!(OcImage, nlcatlist)),
        (b"__objc_protolist", offset_of!(OcImage, protolist)),
        (b"__objc_protorefs", offset_of!(OcImage, protorefs)),
        (b"__objc_imageinfo", offset_of!(OcImage, imageinfo)),
    ];
    unsafe {
        write_bytes(im, 0, 1);
        if mh.is_null() {
            return;
        }
        let mut h: OcMachHeader64 = zeroed();
        copy_nonoverlapping(mh, &mut h as *mut OcMachHeader64 as *mut u8, size_of::<OcMachHeader64>());
        if h.magic != OC_MH_MAGIC_64 {
            return;
        }
        let mut lc = mh.add(size_of::<OcMachHeader64>());
        for _ in 0..h.ncmds {
            let mut l: OcLoadCommand = zeroed();
            copy_nonoverlapping(lc, &mut l as *mut OcLoadCommand as *mut u8, size_of::<OcLoadCommand>());
            if (l.cmdsize as usize) < size_of::<OcLoadCommand>() {
                break;
            }
            if l.cmd == OC_LC_SEGMENT_64 {
                let mut seg: OcSegmentCommand64 = zeroed();
                copy_nonoverlapping(
                    lc,
                    &mut seg as *mut OcSegmentCommand64 as *mut u8,
                    size_of::<OcSegmentCommand64>(),
                );
                let mut s = 0u32;
                while libc::strncmp(
                    seg.segname.as_ptr(),
                    c"__DATA".as_ptr(),
                    6,
                ) == 0
                    && s < seg.nsects
                {
                    let mut sc: OcSection64 = zeroed();
                    copy_nonoverlapping(
                        lc.add(size_of::<OcSegmentCommand64>() + s as usize * size_of::<OcSection64>()),
                        &mut sc as *mut OcSection64 as *mut u8,
                        size_of::<OcSection64>(),
                    );
                    for k in 0..K_SECTS.len() {
                        if libc::strncmp(sc.sectname.as_ptr(), K_SECTS[k].0.as_ptr() as *const c_char, 16)
                            != 0
                        {
                            continue;
                        }
                        let out = (im as *mut u8).add(K_SECTS[k].1).cast::<OcSect>();
                        if (*out).addr == 0 {
                            (*out).addr = (sc.addr as i64 + slide) as u64;
                            (*out).size = sc.size;
                        }
                    }
                    s += 1;
                }
            }
            lc = lc.add(l.cmdsize as usize);
        }
    }
}

unsafe fn oc_ro_name(ro: u64) -> *const c_char {
    unsafe {
        let name = if ro != 0 { oc_str(oc_word(ro + 24)) } else { null() };
        if name.is_null() { c"(unnamed)".as_ptr() } else { name }
    }
}

unsafe fn oc_class_label(cls: u64) -> *const c_char {
    unsafe {
        if cls == 0 {
            return c"(null)".as_ptr();
        }
        if !oc_is_guest(cls) {
            return oc_class_name(ocerz_g2h(cls));
        }
        let mut c: OcerzObjcClass = zeroed();
        ocerz_objc_read_class(cls, &mut c);
        oc_ro_name(c.ro)
    }
}

unsafe fn oc_ro_copy(
    ro: u64,
    methods: *mut c_void,
    protocols: *mut c_void,
    clsname: *const c_char,
) -> *mut u8 {
    unsafe {
        let copy = oc_alloc(OC_RO_BYTES, c"class_ro_t".as_ptr(), clsname).cast::<u8>();
        copy_nonoverlapping(ocerz_g2h(ro).cast::<u8>(), copy, OC_RO_BYTES);
        let mut flags: u32 = 0;
        copy_nonoverlapping(copy, &mut flags as *mut u32 as *mut u8, 4);
        flags &= !OC_RO_SWIFT_INIT;
        copy_nonoverlapping(&flags as *const u32 as *const u8, copy, 4);
        let m = methods as u64;
        let p = protocols as u64;
        copy_nonoverlapping(&m as *const u64 as *const u8, copy.add(32), 8);
        copy_nonoverlapping(&p as *const u64 as *const u8, copy.add(40), 8);
        copy
    }
}

unsafe fn oc_install_pair(
    addr: u64,
    c: *const OcerzObjcClass,
    ro: *const OcerzObjcRo,
    mc: *const OcerzObjcClass,
    mro: *const OcerzObjcRo,
    name: *const c_char,
    load: *mut u64,
) {
    unsafe {
        let cls = ocerz_g2h(addr);
        let meta = ocerz_g2h((*c).isa);
        let protos = oc_protocol_list((*ro).base_protocols, name);
        let mprotos = if (*mro).base_protocols == (*ro).base_protocols {
            protos
        } else {
            oc_protocol_list((*mro).base_protocols, name)
        };
        let im = oc_methods((*ro).base_methods, cls, name, 0, null_mut());
        let cm = oc_methods((*mro).base_methods, meta, name, 1, load);
        let rocopy = oc_ro_copy((*c).ro, im, protos, name);
        let mrocopy = oc_ro_copy((*mc).ro, cm, mprotos, name);

        let empty = ocerz_h2g(oc_need(&raw const G_OC_EMPTY_CACHE));
        ocerz_st(addr + 16, 8, empty);
        ocerz_st(addr + 24, 8, 0);
        ocerz_st(addr + 32, 8, ocerz_h2g(rocopy as *const c_void) | (*c).swift as u64);
        ocerz_st((*c).isa + 16, 8, empty);
        ocerz_st((*c).isa + 24, 8, 0);
        ocerz_st(
            (*c).isa + 32,
            8,
            ocerz_h2g(mrocopy as *const c_void) | (*mc).swift as u64,
        );
    }
}

unsafe fn oc_record(addr: u64, meta: u64, load: u64, name: *const c_char) {
    unsafe {
        let d = oc_alloc(size_of::<OcDefined>(), c"record".as_ptr(), name).cast::<OcDefined>();
        (*d).meta = meta;
        (*d).load = load;
        (*d).scheduled = 0;
        oc_map_put(&raw mut G_OC_CLASSES, addr, d as u64);
    }
}

unsafe fn oc_read_pair(
    addr: u64,
    c: *mut OcerzObjcClass,
    ro: *mut OcerzObjcRo,
    mc: *mut OcerzObjcClass,
    mro: *mut OcerzObjcRo,
) -> c_int {
    unsafe {
        (ocerz_objc_read_class(addr, c) == OCERZ_OBJC_OK as c_int
            && ocerz_objc_read_ro((*c).ro, ro) == OCERZ_OBJC_OK as c_int
            && (*c).isa != 0
            && ocerz_objc_read_class((*c).isa, mc) == OCERZ_OBJC_OK as c_int
            && ocerz_objc_read_ro((*mc).ro, mro) == OCERZ_OBJC_OK as c_int
            && (*mro).flags & OC_RO_META != 0) as c_int
    }
}

#[repr(C)]
struct OcImageInfo {
    version: u32,
    flags: u32,
}

unsafe fn oc_define_class(addr: u64, image_flags: u32) {
    unsafe {
        if !oc_defined(addr).is_null() {
            return;
        }
        let mut c: OcerzObjcClass = zeroed();
        let mut mc: OcerzObjcClass = zeroed();
        let mut ro: OcerzObjcRo = zeroed();
        let mut mro: OcerzObjcRo = zeroed();
        let rc = ocerz_objc_read_class(addr, &mut c);
        if rc != OCERZ_OBJC_OK as c_int {
            oc_stop!("guest class at %#llx has no class_ro_t", addr);
        }
        ocerz_objc_read_ro(c.ro, &mut ro);
        let name = oc_ro_name(c.ro);
        if ro.flags & OC_RO_SWIFT_INIT != 0 {
            crate::ocerz_log!(
                "objc: guest class %s at %#llx is a Swift class its runtime initializes, and waits for it\n",
                name,
                addr
            );
            oc_map_put(&raw mut G_OC_DEFERRED, addr, 1);
            return;
        }
        if ro.flags & (OC_RO_FUTURE | OC_RO_REALIZED) != 0 {
            oc_stop!(
                "guest class %s carries class_ro_t flags %#x that only a running runtime sets",
                name,
                ro.flags
            );
        }
        let root = ro.flags & OC_RO_ROOT != 0;
        if !root && c.superclass == 0 {
            crate::ocerz_log!(
                "objc: guest class %s at %#llx has a null superclass, as a class whose weak-linked superclass the host lacks does, and is left out\n",
                name,
                addr
            );
            oc_map_put(&raw mut G_OC_SKIPPED, addr, 1);
            return;
        }
        if !root && oc_is_guest(c.superclass) && oc_defined(c.superclass).is_null() {
            if !oc_map_find(&raw const G_OC_DEFERRED, c.superclass).is_null() {
                crate::ocerz_log!(
                    "objc: guest class %s at %#llx has the superclass %s, which waits for the Swift runtime, and waits with it\n",
                    name,
                    addr,
                    oc_class_label(c.superclass)
                );
                oc_map_put(&raw mut G_OC_DEFERRED, addr, 1);
                return;
            }
            if !oc_map_find(&raw const G_OC_SKIPPED, c.superclass).is_null() {
                crate::ocerz_log!(
                    "objc: guest class %s at %#llx has the superclass %s at %#llx, which was left out, and is left out\n",
                    name,
                    addr,
                    oc_class_label(c.superclass),
                    c.superclass
                );
                oc_map_put(&raw mut G_OC_SKIPPED, addr, 1);
                return;
            }
            oc_stop!(
                "guest class %s has the superclass %s at %#llx, which is not a class ocerz has defined",
                name,
                oc_class_label(c.superclass),
                c.superclass
            );
        }
        if !oc_is_guest(c.isa)
            || ocerz_objc_read_class(c.isa, &mut mc) != OCERZ_OBJC_OK as c_int
            || ocerz_objc_read_ro(mc.ro, &mut mro) != OCERZ_OBJC_OK as c_int
            || mro.flags & OC_RO_META == 0
        {
            oc_stop!(
                "guest class %s has no guest metaclass at %#llx",
                name,
                c.isa
            );
        }

        let cls = ocerz_g2h(addr);
        let mut load: u64 = 0;
        oc_install_pair(addr, &c, &ro, &mc, &mro, name, &mut load);
        let mut info = OcImageInfo {
            version: 0,
            flags: image_flags,
        };
        let got = core::mem::transmute::<
            *mut c_void,
            unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
        >(oc_need(&raw const G_OC_READ_CLASS_PAIR))(cls, &mut info as *mut OcImageInfo as *mut c_void);
        if got != cls {
            oc_stop!(
                "the native runtime would not read guest class %s at %#llx (objc_readClassPair gave %p)",
                name,
                addr,
                got
            );
        }

        oc_record(addr, c.isa, load, name);
        if oc_logging() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: OBJCLOG[%d] define %sclass %s at %#llx, superclass %s, %u bytes\n".as_ptr(),
                libc::getpid(),
                if c.swift != 0 {
                    c"Swift ".as_ptr()
                } else if root {
                    c"root ".as_ptr()
                } else {
                    c"".as_ptr()
                },
                name,
                addr,
                if root {
                    c"(none)".as_ptr()
                } else {
                    oc_class_label(c.superclass)
                },
                ro.instance_size,
            );
        }
    }
}

unsafe fn oc_prepare_locked(addr: u64, depth: c_int) -> c_int {
    unsafe {
        if !oc_defined(addr).is_null() {
            return 1;
        }
        let mut c: OcerzObjcClass = zeroed();
        let mut mc: OcerzObjcClass = zeroed();
        let mut ro: OcerzObjcRo = zeroed();
        let mut mro: OcerzObjcRo = zeroed();
        if depth > 64
            || oc_read_pair(addr, &mut c, &mut ro, &mut mc, &mut mro) == 0
            || ro.flags & OC_RO_REALIZED != 0
        {
            return 0;
        }
        if c.superclass != 0 && !oc_map_find(&raw const G_OC_DEFERRED, c.superclass).is_null() {
            oc_prepare_locked(c.superclass, depth + 1);
        }
        let name = oc_ro_name(c.ro);
        let mut load: u64 = 0;
        oc_install_pair(addr, &c, &ro, &mc, &mro, name, &mut load);
        oc_record(addr, c.isa, 0, name);
        let deferred = oc_map_find(&raw const G_OC_DEFERRED, addr);
        if !deferred.is_null() {
            *deferred = 0;
        }
        if oc_logging() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: OBJCLOG[%d] prepare Swift-initialized class %s at %#llx, superclass %s, %u bytes\n".as_ptr(),
                libc::getpid(),
                name,
                addr,
                oc_class_label(c.superclass),
                ro.instance_size,
            );
        }
        0
    }
}

unsafe fn oc_late_define_locked(addr: u64, depth: c_int) {
    unsafe {
        let mut c: OcerzObjcClass = zeroed();
        let mut mc: OcerzObjcClass = zeroed();
        let mut ro: OcerzObjcRo = zeroed();
        let mut mro: OcerzObjcRo = zeroed();
        if depth > 64
            || !oc_defined(addr).is_null()
            || !oc_map_find(&raw const G_OC_DEFERRED, addr).is_null()
            || !oc_map_find(&raw const G_OC_SKIPPED, addr).is_null()
            || oc_read_pair(addr, &mut c, &mut ro, &mut mc, &mut mro) == 0
            || ro.flags & (OC_RO_SWIFT_INIT | OC_RO_FUTURE | OC_RO_REALIZED) != 0
        {
            return;
        }
        if c.superclass != 0 && oc_is_guest(c.superclass) && oc_defined(c.superclass).is_null() {
            oc_late_define_locked(c.superclass, depth + 1);
        }
        let name = oc_ro_name(c.ro);
        let mut load: u64 = 0;
        oc_install_pair(addr, &c, &ro, &mc, &mro, name, &mut load);
        let mut info = OcImageInfo { version: 0, flags: 0 };
        let cls = ocerz_g2h(addr);
        let got = core::mem::transmute::<
            *mut c_void,
            unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
        >(oc_need(&raw const G_OC_READ_CLASS_PAIR))(cls, &mut info as *mut OcImageInfo as *mut c_void);
        if got != cls {
            oc_stop!(
                "the native runtime would not read guest class %s at %#llx when it was first used (objc_readClassPair gave %p)",
                name,
                addr,
                got
            );
        }
        oc_record(addr, c.isa, 0, name);
        if oc_logging() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: OBJCLOG[%d] define %sclass %s at %#llx when first used, superclass %s, %u bytes\n".as_ptr(),
                libc::getpid(),
                if c.swift != 0 { c"Swift ".as_ptr() } else { c"".as_ptr() },
                name,
                addr,
                oc_class_label(c.superclass),
                ro.instance_size,
            );
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objcbridge_ensure_object(obj: *mut c_void) {
    unsafe {
        let o = obj as u64;
        if o == 0 || (o & 1) != 0 || (o >> 63) != 0 {
            return;
        }
        let g = ocerz_h2g(obj);
        let isa = oc_object_getClass(obj);
        let c = if isa.is_null() { 0 } else { ocerz_h2g(isa) };
        if !oc_is_guest(c) {
            return;
        }
        libc::pthread_mutex_lock(&raw mut G_OC_LOCK);
        if !(oc_is_guest(g) && !oc_defined(g).is_null()) && oc_defined(c).is_null() {
            let mut cc: OcerzObjcClass = zeroed();
            let mut r: OcerzObjcRo = zeroed();
            let meta = ocerz_objc_read_class(c, &mut cc) == OCERZ_OBJC_OK as c_int
                && ocerz_objc_read_ro(cc.ro, &mut r) == OCERZ_OBJC_OK as c_int
                && r.flags & OC_RO_META != 0;
            if meta && oc_is_guest(g) {
                oc_late_define_locked(g, 0);
            } else if !meta {
                oc_late_define_locked(c, 0);
            }
        }
        libc::pthread_mutex_unlock(&raw mut G_OC_LOCK);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objcbridge_ensure_class(cls: *mut c_void) {
    unsafe {
        let c = cls as u64;
        if c == 0 || !oc_is_guest(ocerz_h2g(cls)) {
            return;
        }
        libc::pthread_mutex_lock(&raw mut G_OC_LOCK);
        oc_late_define_locked(ocerz_h2g(cls), 0);
        libc::pthread_mutex_unlock(&raw mut G_OC_LOCK);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objcbridge_prepare_class(addr: u64) -> c_int {
    unsafe {
        if addr == 0 {
            return 0;
        }
        libc::pthread_mutex_lock(&raw mut G_OC_LOCK);
        let known = oc_prepare_locked(addr, 0);
        libc::pthread_mutex_unlock(&raw mut G_OC_LOCK);
        known
    }
}

#[inline(always)]
unsafe fn oc_round_swap(mode: u64) -> u64 {
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

#[inline(always)]
fn oc_round_of_mxcsr(mxcsr: u32) -> u64 {
    const MODE: [u64; 4] = [0, 0x80_0000, 0x40_0000, 0xc0_0000];
    MODE[((mxcsr >> 13) & 3) as usize]
}

unsafe fn oc_peek(haddr: u64, out: *mut u64) -> c_int {
    unsafe {
        let mut got: libc::mach_vm_size_t = 0;
        (mach_vm_read_overwrite(
            libc::mach_task_self(),
            haddr,
            8,
            out as libc::mach_vm_address_t,
            &mut got,
        ) == libc::KERN_SUCCESS
            && got == 8) as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objcbridge_guest_swift_object(obj: *const c_void) -> c_int {
    unsafe {
        let o = obj as u64;
        if o == 0 || (o >> 63) != 0 || (o & 7) != 0 {
            return 0;
        }
        let isa = *(obj as *const u64);
        if isa == 0 || (isa & 7) != 0 {
            return 0;
        }
        let cls = ocerz_g2h(isa) as *const u8;
        if (*(cls.add(32) as *const u64) & OC_SWIFT_STABLE) == 0
            || (*(cls.add(40) as *const u32) & OC_SWIFT_RC) == 0
        {
            return 0;
        }
        ocerz_host_in_guest_reservation(ocerz_g2h(*(cls.sub(16) as *const u64)))
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objcbridge_swift_destroy_fault(pc: u64, context: u64) -> c_int {
    unsafe {
        let mut isa: u64 = 0;
        let mut destroy: u64 = 0;
        if context == 0
            || ocerz_host_in_guest_reservation(pc as *const c_void) == 0
            || oc_peek(context, &mut isa) == 0
            || isa == 0
            || (isa & 7) != 0
            || oc_peek(ocerz_g2h(isa - 16) as u64, &mut destroy) == 0
        {
            return 0;
        }
        (ocerz_g2h(destroy) as u64 == pc) as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objcbridge_swift_destroy(object: *mut c_void, destroy: u64) {
    unsafe {
        let entered = *libc::__error();
        let mut cpu = ocerz_vm_current_cpu();
        if cpu.is_null() {
            cpu = ocerz_thread_attach(ocerz_vm_process());
        }
        if cpu.is_null() || (*cpu).vm.is_null() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: objc: native code released the last reference to guest Swift object %p on a thread no guest personality could be attached to, so its destroy function %#llx cannot run\n".as_ptr(),
                object,
                ocerz_h2g(destroy as *const c_void),
            );
            libc::_exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
        }
        if (*(*cpu).vm).exited != 0 {
            return;
        }
        if oc_logging() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: OBJCLOG[%d] native code destroys guest Swift object %p through %#llx\n".as_ptr(),
                libc::getpid(),
                object,
                ocerz_h2g(destroy as *const c_void),
            );
        }
        let stack_top = ((*cpu).gpr[OCERZ_RSP as usize] - 128) & !0xfu64;
        let mut saved: OcerzBridgeFrame = zeroed();
        ocerz_bridge_guest_enter(&mut saved);
        let fpcr = oc_round_swap(oc_round_of_mxcsr((*cpu).mxcsr));
        ocerz_vm_call_swift_context(
            (*cpu).vm,
            ocerz_h2g(destroy as *const c_void),
            ocerz_h2g(object),
            stack_top,
        );
        oc_round_swap(fpcr & OCERZ_ABI_ROUND_MASK as u64);
        ocerz_bridge_guest_leave(&saved);
        *libc::__error() = entered;
    }
}

unsafe fn oc_define_classes(list: *const OcSect, image_flags: u32) {
    unsafe {
        let n = ((*list).size / 8) as c_int;
        if n <= 0 {
            return;
        }
        let classes = libc::malloc(n as usize * size_of::<u64>()) as *mut u64;
        let supers = libc::malloc(n as usize * size_of::<u64>()) as *mut u64;
        let order = libc::malloc(n as usize * size_of::<c_int>()) as *mut c_int;
        if classes.is_null() || supers.is_null() || order.is_null() {
            oc_stop!("no memory to order %d guest classes", n);
        }
        for i in 0..n {
            *classes.add(i as usize) = oc_word((*list).addr + 8 * i as u64);
            let mut c: OcerzObjcClass = zeroed();
            ocerz_objc_read_class(*classes.add(i as usize), &mut c);
            *supers.add(i as usize) = c.superclass;
        }
        let mut culprit: c_int = -1;
        let rc = ocerz_objc_class_order(classes, supers, n, order, &mut culprit);
        if rc == OCERZ_OBJC_CYCLE as c_int {
            oc_stop!(
                "guest class %s is its own superclass, through a chain of superclasses",
                oc_class_label(*classes.add(culprit as usize))
            );
        }
        if rc != OCERZ_OBJC_OK as c_int {
            oc_stop!(
                "cannot order %d guest classes by superclass: %s",
                n,
                ocerz_objc_refusal(rc)
            );
        }
        for i in 0..n {
            oc_define_class(*classes.add(*order.add(i as usize) as usize), image_flags);
        }
        libc::free(classes as *mut c_void);
        libc::free(supers as *mut c_void);
        libc::free(order as *mut c_void);
    }
}

unsafe fn oc_category_class(cat: *const OcerzObjcCategory, addr: u64) -> *mut c_void {
    unsafe {
        let catname = oc_str((*cat).name);
        if (*cat).cls == 0 {
            crate::ocerz_log!(
                "objc: category %s at %#llx names no class, as a category on a missing weak class does, and is left out\n",
                if catname.is_null() {
                    c"(unnamed)".as_ptr()
                } else {
                    catname
                },
                addr
            );
            return null_mut();
        }
        if oc_is_guest((*cat).cls) && oc_defined((*cat).cls).is_null() {
            oc_stop!(
                "guest category %s is on the class %s at %#llx, which is not a class ocerz has defined",
                if catname.is_null() {
                    c"(unnamed)".as_ptr()
                } else {
                    catname
                },
                oc_class_label((*cat).cls),
                (*cat).cls
            );
        }
        ocerz_g2h((*cat).cls)
    }
}

unsafe fn oc_category_methods(
    cls: *mut c_void,
    list: u64,
    meta: c_int,
    catname: *const c_char,
) {
    unsafe {
        let mut l: OcerzObjcList = zeroed();
        let clsname = oc_class_name(cls);
        if ocerz_objc_method_list(list, &mut l) != OCERZ_OBJC_OK as c_int {
            oc_stop!(
                "guest category %s on %s has a method list whose entries are %u bytes, which ocerz cannot read",
                catname,
                clsname,
                l.entsize
            );
        }
        for i in 0..l.count {
            let mut m: OcerzObjcMethod = zeroed();
            ocerz_objc_method_at(&l, i, &mut m);
            let name = oc_str(m.name);
            if name.is_null() {
                continue;
            }
            let sel = oc_sel(name);
            let types = oc_str(m.types);
            let imp = oc_imp(cls, clsname, meta, sel, types, m.imp);
            core::mem::transmute::<
                *mut c_void,
                unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, *const c_char),
            >(oc_need(&raw const G_OC_CLASS_REPLACE_METHOD))(cls, sel, imp, types);
        }
    }
}

unsafe fn oc_category_properties(cls: *mut c_void, list: u64, catname: *const c_char) {
    unsafe {
        let mut l: OcerzObjcList = zeroed();
        if ocerz_objc_property_list(list, &mut l) != OCERZ_OBJC_OK as c_int {
            oc_stop!(
                "guest category %s has a property list whose entries are %u bytes",
                catname,
                l.entsize
            );
        }
        for i in 0..l.count {
            let mut p: OcerzObjcProperty = zeroed();
            let mut attrs: [OcerzObjcAttribute; OC_ATTRS_MAX] = zeroed();
            let mut storage = [0 as c_char; 1024];
            ocerz_objc_property_at(&l, i, &mut p);
            let n = oc_attrs(
                p.attributes,
                attrs.as_mut_ptr(),
                storage.as_mut_ptr(),
                storage.len(),
                catname,
                oc_str(p.name),
            );
            core::mem::transmute::<
                *mut c_void,
                unsafe extern "C" fn(*mut c_void, *const c_char, *const OcerzObjcAttribute, c_uint) -> bool,
            >(oc_need(&raw const G_OC_CLASS_ADD_PROPERTY))(
                cls, oc_str(p.name), attrs.as_ptr(), n as c_uint,
            );
        }
    }
}

unsafe fn oc_define_category(addr: u64, image_flags: u32) {
    unsafe {
        let mut cat: OcerzObjcCategory = zeroed();
        if ocerz_objc_read_category(
            addr,
            (image_flags & OC_IMAGE_CLASS_PROPERTIES != 0) as c_int,
            &mut cat,
        ) != OCERZ_OBJC_OK as c_int
        {
            return;
        }
        let cls = oc_category_class(&cat, addr);
        if cls.is_null() {
            return;
        }
        let catname_raw = oc_str(cat.name);
        let catname = if catname_raw.is_null() {
            c"(unnamed)".as_ptr()
        } else {
            catname_raw
        };
        let meta = oc_object_getClass(cls);
        oc_category_methods(cls, cat.instance_methods, 0, catname);
        oc_category_methods(meta, cat.class_methods, 1, catname);
        let n = ocerz_objc_protocol_count(cat.protocols);
        let mut i = 0u64;
        while i < n {
            let proto = oc_protocol(ocerz_objc_protocol_ref(cat.protocols, i), 0);
            if !proto.is_null() {
                core::mem::transmute::<
                    *mut c_void,
                    unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
                >(oc_need(&raw const G_OC_CLASS_ADD_PROTOCOL))(cls, proto);
            }
            i += 1;
        }
        oc_category_properties(cls, cat.instance_properties, catname);
        oc_category_properties(meta, cat.class_properties, catname);
        if oc_logging() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: OBJCLOG[%d] define category %s(%s)\n".as_ptr(),
                libc::getpid(),
                oc_class_name(cls),
                catname,
            );
        }
    }
}

unsafe fn oc_queue_load(cls: u64, imp: u64, category: c_int) {
    unsafe {
        if G_OC_LOADS_N == G_OC_LOADS_CAP {
            let cap = if G_OC_LOADS_CAP != 0 { G_OC_LOADS_CAP * 2 } else { 64 };
            let grown = libc::realloc(
                G_OC_LOADS as *mut c_void,
                cap * size_of::<OcLoad>(),
            ) as *mut OcLoad;
            if grown.is_null() {
                oc_stop!("no memory to queue +load methods");
            }
            G_OC_LOADS = grown;
            G_OC_LOADS_CAP = cap;
        }
        (*G_OC_LOADS.add(G_OC_LOADS_N)).cls = cls;
        (*G_OC_LOADS.add(G_OC_LOADS_N)).imp = imp;
        (*G_OC_LOADS.add(G_OC_LOADS_N)).image = G_OC_DEFINING;
        (*G_OC_LOADS.add(G_OC_LOADS_N)).category = category;
        G_OC_LOADS_N += 1;
    }
}

unsafe fn oc_schedule_class(cls: u64) {
    unsafe {
        let d = oc_defined(cls);
        if d.is_null() || (*d).scheduled != 0 {
            return;
        }
        let mut c: OcerzObjcClass = zeroed();
        ocerz_objc_read_class(cls, &mut c);
        if oc_is_guest(c.superclass) {
            oc_schedule_class(c.superclass);
        }
        (*d).scheduled = 1;
        if (*d).load != 0 {
            oc_queue_load(cls, (*d).load, 0);
        }
    }
}

unsafe fn oc_schedule_category(addr: u64, image_flags: u32) {
    unsafe {
        let mut cat: OcerzObjcCategory = zeroed();
        let mut l: OcerzObjcList = zeroed();
        if ocerz_objc_read_category(
            addr,
            (image_flags & OC_IMAGE_CLASS_PROPERTIES != 0) as c_int,
            &mut cat,
        ) != OCERZ_OBJC_OK as c_int
            || cat.cls == 0
            || ocerz_objc_method_list(cat.class_methods, &mut l) != OCERZ_OBJC_OK as c_int
        {
            return;
        }
        for i in 0..l.count {
            let mut m: OcerzObjcMethod = zeroed();
            ocerz_objc_method_at(&l, i, &mut m);
            let name = oc_str(m.name);
            if !name.is_null() && m.imp != 0 && libc::strcmp(name, c"load".as_ptr()) == 0 {
                oc_queue_load(cat.cls, m.imp, 1);
                return;
            }
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objcbridge_define_image(mh: *const u8, slide: i64) -> c_int {
    unsafe {
        let mut im: OcImage = zeroed();
        oc_scan(mh, slide, &mut im);
        if im.classlist.size == 0
            && im.catlist.size == 0
            && im.catlist2.size == 0
            && im.protolist.size == 0
            && im.protorefs.size == 0
        {
            return 0;
        }

        libc::pthread_mutex_lock(&raw mut G_OC_LOCK);
        let key = mh as u64;
        if !oc_map_find(&raw const G_OC_IMAGES, key).is_null() {
            libc::pthread_mutex_unlock(&raw mut G_OC_LOCK);
            return 0;
        }
        oc_map_put(&raw mut G_OC_IMAGES, key, 1);
        G_OC_DEFINING = key;
        let flags = if im.imageinfo.size >= 8 {
            oc_u32(im.imageinfo.addr + 4)
        } else {
            0
        };
        let mut defined = 0;

        let mut off = 0u64;
        while off + 8 <= im.protolist.size {
            oc_protocol(oc_word(im.protolist.addr + off), 0);
            off += 8;
        }
        let mut off = 0u64;
        while off + 8 <= im.protorefs.size {
            let word = im.protorefs.addr + off;
            let ref_ = oc_word(word);
            let native = oc_protocol(ref_, 0);
            if !native.is_null() && ocerz_h2g(native) != ref_ {
                ocerz_st(word, 8, ocerz_h2g(native));
            }
            off += 8;
        }

        oc_define_classes(&im.classlist, flags);
        defined += (im.classlist.size / 8) as c_int;
        let mut off = 0u64;
        while off + 8 <= im.catlist.size {
            oc_define_category(oc_word(im.catlist.addr + off), flags);
            off += 8;
            defined += 1;
        }
        let mut off = 0u64;
        while off + 8 <= im.catlist2.size {
            oc_define_category(oc_word(im.catlist2.addr + off), flags);
            off += 8;
            defined += 1;
        }

        let mut off = 0u64;
        while off + 8 <= im.nlclslist.size {
            oc_schedule_class(oc_word(im.nlclslist.addr + off));
            off += 8;
        }
        let mut off = 0u64;
        while off + 8 <= im.nlcatlist.size {
            oc_schedule_category(oc_word(im.nlcatlist.addr + off), flags);
            off += 8;
        }
        G_OC_DEFINING = 0;
        libc::pthread_mutex_unlock(&raw mut G_OC_LOCK);
        defined
    }
}

unsafe fn oc_run_load_list(
    vm: *mut OcerzVM,
    loads: *const OcLoad,
    n: usize,
    stack_top: u64,
) -> c_int {
    unsafe {
        let mut ran = 0;
        let pool = core::mem::transmute::<*mut c_void, unsafe extern "C" fn() -> *mut c_void>(
            oc_need(&raw const G_OC_POOL_PUSH),
        )();
        let sel = ocerz_h2g(oc_sel(c"load".as_ptr()));
        let mut i = 0usize;
        while i < n && (*vm).exited == 0 {
            if oc_logging() {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: OBJCLOG[%d] +[%s load]%s at %#llx\n".as_ptr(),
                    libc::getpid(),
                    oc_class_name(ocerz_g2h((*loads.add(i)).cls)),
                    if (*loads.add(i)).category != 0 {
                        c" (category)".as_ptr()
                    } else {
                        c"".as_ptr()
                    },
                    (*loads.add(i)).imp,
                );
            }
            let args: [u64; 2] = [(*loads.add(i)).cls, sel];
            ocerz_vm_call(vm, (*loads.add(i)).imp, args.as_ptr(), 2, stack_top);
            ran += 1;
            i += 1;
        }
        if (*vm).exited == 0 {
            core::mem::transmute::<*mut c_void, unsafe extern "C" fn(*mut c_void)>(
                oc_need(&raw const G_OC_POOL_POP),
            )(pool);
        }
        ran
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objcbridge_run_loads(vm: *mut OcerzVM, stack_top: u64) -> c_int {
    unsafe {
        let mut ran = 0;
        loop {
            libc::pthread_mutex_lock(&raw mut G_OC_LOCK);
            let loads = G_OC_LOADS;
            let n = G_OC_LOADS_N;
            G_OC_LOADS = null_mut();
            G_OC_LOADS_N = 0;
            G_OC_LOADS_CAP = 0;
            libc::pthread_mutex_unlock(&raw mut G_OC_LOCK);
            if n == 0 {
                libc::free(loads as *mut c_void);
                return ran;
            }
            ran += oc_run_load_list(vm, loads, n, stack_top);
            libc::free(loads as *mut c_void);
            if (*vm).exited != 0 {
                return ran;
            }
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objcbridge_run_image_loads(
    vm: *mut OcerzVM,
    mh: *const u8,
    stack_top: u64,
) -> c_int {
    unsafe {
        let key = mh as u64;
        libc::pthread_mutex_lock(&raw mut G_OC_LOCK);
        let mut n: usize = 0;
        let mut kept: usize = 0;
        let mine = if G_OC_LOADS_N != 0 {
            libc::malloc(G_OC_LOADS_N * size_of::<OcLoad>()) as *mut OcLoad
        } else {
            null_mut()
        };
        for i in 0..G_OC_LOADS_N {
            if (*G_OC_LOADS.add(i)).image == key && !mine.is_null() {
                *mine.add(n) = *G_OC_LOADS.add(i);
                n += 1;
            } else {
                *G_OC_LOADS.add(kept) = *G_OC_LOADS.add(i);
                kept += 1;
            }
        }
        G_OC_LOADS_N = kept;
        libc::pthread_mutex_unlock(&raw mut G_OC_LOCK);
        let ran = if n != 0 {
            oc_run_load_list(vm, mine, n, stack_top)
        } else {
            0
        };
        libc::free(mine as *mut c_void);
        ran
    }
}

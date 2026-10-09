//! ---- a descriptor is made once, when it is first wanted ----
//! What an export crosses to, and how, is the export's record in the API
//! database (apidb.h): a fn record names the host symbol and the function's
//! real signature, which the ABI engine in abi.h turns into register and stack
//! placement, including which register a double belongs in and how a 32-bit
//! argument or result is extended; a special record names a handler ocerz
//! answers the call with itself; struct and shape records say which pointer
//! arguments carry structures of function pointers and what each word of them
//! is.  ocerz_bridge_lookup makes a descriptor from those records the first
//! time an export is asked for and returns the same one every time after, from
//! any thread.
//!
//! Guest pointers need translating in principle and not at all in practice, at
//! least in the map native mode runs in: ocerz_g2h is identity there, so a
//! guest pointer already is a host pointer and a buffer the guest allocated can
//! be handed straight to a native function.  The conversion is written anyway,
//! both because the map is not native mode's to assume and because a null
//! pointer must stay null rather than become the base of the arena.
//!
//! What this layer deliberately cannot do yet, so that what it does do is
//! trustworthy:
//!
//! - Variadic functions in general.  Apple's arm64 ABI passes variadic
//!   arguments on the stack while x86-64 passes them in registers, so calling
//!   one through a fixed-arity prototype puts every argument in the wrong
//!   place.  The ones whose format string says what follows - the printf
//!   family, NSLog and CoreFoundation's format functions - are special records
//!   answered by veneers in objcbridge.h.  Those whose ellipsis stands for one
//!   fixed argument, open, fcntl and ioctl among them, are special records
//!   answered in sysbridge.h, and the rest are stub records.
//! - A callback whose own signature takes a callback; abi.h describes what a
//!   callback argument can be and which threads it may run on.
//!
//! An export with no descriptor is not an error: it falls back to naming
//! itself and stopping with OCERZ_BRIDGE_UNIMPL_EXIT.
//!
//! ---- the signature is the declared one, not the convenient one ----
//!
//! ---- what is deliberately absent ----
//!
//! ---- fortified string functions ----
//!
//! ---- why null has to survive the conversion ----
//!
//! ---- a descriptor that does not hold up is no descriptor ----
//!
//! ---- counting ----
//! Each descriptor carries its own crossing count, and every descriptor made
//! is linked into one list, so OCERZ_BRIDGESTAT's report at exit walks that
//! list: the crossings, how many descriptors were crossed at least once, and
//! how many fn and special records the libraries the bridge was asked about
//! declare, then the crossed exports busiest first.

use core::ffi::{c_char, c_int, c_void};
use core::ptr::null_mut;
use core::sync::atomic::{AtomicPtr, Ordering};

use crate::ffi::*;
use crate::ported::bridge::common::{
    G_BR_FRAME, br_logging, br_settle, gpr, ocerz_h2g, ocerz_ld, ocerz_st, set_gpr,
};

pub type BrSpecial = unsafe extern "C" fn(*mut OcerzVM, *mut OcerzCPU) -> c_int;

#[repr(C)]
pub struct BrStructBinding {
    pub argpos: c_int,
    pub reg: c_int,
    pub slot: c_int,
    pub shape: *const c_char,
    pub versions: *mut *const OcerzApiShape,
    pub nversions: c_int,
}

#[repr(C)]
pub struct BrInplaceBinding {
    pub reg: c_int,
    pub offset: u32,
    pub sig: *const c_char,
}

#[repr(C)]
pub struct OcerzBridgeFn {
    pub lib: *const c_char,
    pub sym: *const c_char,
    pub host: *const c_char,
    pub sig: *const c_char,
    pub special: Option<BrSpecial>,
    pub addr: *mut c_void,
    pub parsed: OcerzAbiSig,
    pub register_only: c_int,
    pub nstructs: c_int,
    pub structs: [BrStructBinding; OCERZ_APIDB_STRUCT_ARGS as usize],
    pub ninplace: c_int,
    pub inplace: [BrInplaceBinding; OCERZ_APIDB_INPLACE as usize],
    pub calls: u64,
    pub next: *mut OcerzBridgeFn,
}

#[repr(C)]
pub struct BrLib {
    pub api: *const OcerzApiLibrary,
    pub handle: AtomicPtr<c_void>,
    pub fns: *mut AtomicPtr<OcerzBridgeFn>,
    pub next: *mut BrLib,
}

pub static G_BR_LIBS: AtomicPtr<BrLib> = AtomicPtr::new(null_mut());
pub static G_BR_MADE: AtomicPtr<OcerzBridgeFn> = AtomicPtr::new(null_mut());
pub static mut G_BR_LIBS_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
pub static mut G_BR_FN_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
pub static mut G_BR_HOST_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

pub const BR_NONE: *mut OcerzBridgeFn = 1usize as *mut OcerzBridgeFn;

unsafe fn br_stack_slot(sig: *const OcerzAbiSig, argpos: c_int) -> c_int {
    unsafe {
        if argpos < 0
            || argpos >= (*sig).nargs
            || (*sig).arg[argpos as usize] != b'p' as c_char
            || (*sig).ret == b'{' as c_char
        {
            return -1;
        }
        let mut ints = 0;
        let mut fps = 0;
        let mut slot = 0;
        for i in 0..=argpos {
            let a = (*sig).arg[i as usize];
            if a == b'{' as c_char || a == b'D' as c_char {
                return -1;
            }
            let fp = a == b'f' as c_char || a == b'd' as c_char;
            let spilled = if fp {
                fps += 1;
                fps > 8
            } else {
                ints += 1;
                ints > 6
            };
            if i == argpos {
                return if spilled { slot } else { -1 };
            }
            slot += spilled as c_int;
        }
        -1
    }
}

unsafe fn br_int_register(sig: *const OcerzAbiSig, argpos: c_int) -> c_int {
    const REGS: [u32; 6] = [
        OCERZ_RDI, OCERZ_RSI, OCERZ_RDX, OCERZ_RCX, OCERZ_R8, OCERZ_R9,
    ];
    unsafe {
        if argpos < 0
            || argpos >= (*sig).nargs
            || (*sig).arg[argpos as usize] != b'p' as c_char
            || (*sig).ret == b'{' as c_char
        {
            return -1;
        }
        let mut n = 0;
        for i in 0..argpos {
            let a = (*sig).arg[i as usize];
            if a == b'{' as c_char {
                return -1;
            }
            if a != b'f' as c_char && a != b'd' as c_char {
                n += 1;
            }
        }
        if n < 6 { REGS[n] as c_int } else { -1 }
    }
}

unsafe fn br_bind_structs(
    api: *const OcerzApiLibrary,
    e: *const OcerzApiEntry,
    fn_: *mut OcerzBridgeFn,
) -> c_int {
    unsafe {
        let mut k = 0;
        while k < (*e).nstructs && k < OCERZ_APIDB_STRUCT_ARGS as c_int {
            let a = (*e).structs.as_ptr().add(k as usize);
            let b = (*fn_).structs.as_mut_ptr().add(k as usize);
            (*b).argpos = (*a).argpos;
            (*b).shape = (*a).shape;
            (*b).reg = br_int_register(&(*fn_).parsed, (*a).argpos);
            (*b).slot = if (*b).reg < 0 {
                br_stack_slot(&(*fn_).parsed, (*a).argpos)
            } else {
                -1
            };
            if (*b).reg < 0 && (*b).slot < 0 {
                crate::ocerz_log!(
                    "bridge: %s converts a %s in argument %d, which its signature %s does not place in a register or a stack slot\n",
                    (*fn_).sym,
                    (*a).shape,
                    (*a).argpos,
                    (*fn_).sig
                );
                return 0;
            }
            let mut n = 0;
            for s in 0..(*api).nshapes {
                if libc::strcmp((*(*api).shapes.add(s as usize)).name, (*a).shape) == 0 {
                    n += 1;
                }
            }
            if n == 0 {
                crate::ocerz_log!(
                    "bridge: %s converts a %s, which %s describes in no version\n",
                    (*fn_).sym,
                    (*a).shape,
                    (*api).path
                );
                return 0;
            }
            let versions = libc::calloc(n as usize, size_of::<*const OcerzApiShape>())
                as *mut *const OcerzApiShape;
            if versions.is_null() {
                return 0;
            }
            let mut n = 0;
            for s in 0..(*api).nshapes {
                if libc::strcmp((*(*api).shapes.add(s as usize)).name, (*a).shape) == 0 {
                    *versions.add(n) = (*api).shapes.add(s as usize);
                    n += 1;
                }
            }
            (*b).versions = versions;
            (*b).nversions = n as c_int;
            (*fn_).nstructs = k + 1;
            k += 1;
        }
        1
    }
}

unsafe fn br_bind_inplace(e: *const OcerzApiEntry, fn_: *mut OcerzBridgeFn) -> c_int {
    unsafe {
        let mut k = 0;
        while k < (*e).ninplace && k < OCERZ_APIDB_INPLACE as c_int {
            let a = (*e).inplace.as_ptr().add(k as usize);
            let b = (*fn_).inplace.as_mut_ptr().add(k as usize);
            (*b).reg = br_int_register(&(*fn_).parsed, (*a).argpos);
            (*b).offset = (*a).offset;
            (*b).sig = (*a).sig;
            if (*b).reg < 0 {
                crate::ocerz_log!(
                    "bridge: %s converts a function in argument %d in place, which its signature %s does not place in a register\n",
                    (*fn_).sym,
                    (*a).argpos,
                    (*fn_).sig
                );
                return 0;
            }
            (*fn_).ninplace = k + 1;
            k += 1;
        }
        1
    }
}

unsafe fn br_make(api: *const OcerzApiLibrary, e: *const OcerzApiEntry) -> *mut OcerzBridgeFn {
    unsafe {
        let fn_ = libc::calloc(1, size_of::<OcerzBridgeFn>()) as *mut OcerzBridgeFn;
        if fn_.is_null() {
            return null_mut();
        }
        (*fn_).lib = (*api).install_name;
        (*fn_).sym = (*e).export_name;

        if (*e).kind == OCERZ_API_SPECIAL {
            (*fn_).special = br_handler((*e).handler);
            if (*fn_).special.is_none() {
                crate::ocerz_log!(
                    "bridge: %s asks for the handler %s, which ocerz does not have\n",
                    (*fn_).sym,
                    (*e).handler
                );
                libc::free(fn_ as *mut c_void);
                return null_mut();
            }
        } else {
            (*fn_).host = (*e).host;
            (*fn_).sig = (*e).sig;
            (*fn_).addr = ocerz_bridge_host_symbol((*api).install_name, (*e).host);
            if (*fn_).addr.is_null() {
                crate::ocerz_log!(
                    "bridge: %s wants host %s, which does not resolve\n",
                    (*fn_).sym,
                    (*fn_).host
                );
                libc::free(fn_ as *mut c_void);
                return null_mut();
            }
            if ocerz_abi_parse((*fn_).sig, &mut (*fn_).parsed) != OCERZ_OK as c_int {
                crate::ocerz_log!(
                    "bridge: %s is declared %s, which the abi engine will not take\n",
                    (*fn_).sym,
                    if (*fn_).sig.is_null() {
                        c"(nothing)".as_ptr()
                    } else {
                        (*fn_).sig
                    }
                );
                libc::free(fn_ as *mut c_void);
                return null_mut();
            }
            if br_bind_structs(api, e, fn_) == 0 || br_bind_inplace(e, fn_) == 0 {
                for k in 0..OCERZ_APIDB_STRUCT_ARGS as usize {
                    libc::free((*fn_).structs[k].versions as *mut c_void);
                }
                libc::free(fn_ as *mut c_void);
                return null_mut();
            }
            (*fn_).register_only = ocerz_abi_register_only(&(*fn_).parsed);
        }
        (*fn_).next = G_BR_MADE.load(Ordering::SeqCst);
        G_BR_MADE.store(fn_, Ordering::SeqCst);
        fn_
    }
}

pub unsafe fn br_lib(api: *const OcerzApiLibrary) -> *mut BrLib {
    unsafe {
        let mut l = G_BR_LIBS.load(Ordering::SeqCst);
        while !l.is_null() {
            if (*l).api == api {
                return l;
            }
            l = (*l).next;
        }
        libc::pthread_mutex_lock(&raw mut G_BR_LIBS_LOCK);
        let mut lib: *mut BrLib = null_mut();
        let mut l = G_BR_LIBS.load(Ordering::SeqCst);
        while !l.is_null() && lib.is_null() {
            if (*l).api == api {
                lib = l;
            }
            l = (*l).next;
        }
        if lib.is_null() {
            let n = if (*api).nentries > 0 {
                (*api).nentries as usize
            } else {
                1
            };
            let nl = libc::calloc(1, size_of::<BrLib>()) as *mut BrLib;
            let fns = libc::calloc(n, size_of::<AtomicPtr<OcerzBridgeFn>>())
                as *mut AtomicPtr<OcerzBridgeFn>;
            if !nl.is_null() && !fns.is_null() {
                (*nl).api = api;
                (*nl).fns = fns;
                (*nl).next = G_BR_LIBS.load(Ordering::SeqCst);
                G_BR_LIBS.store(nl, Ordering::SeqCst);
                lib = nl;
            } else {
                libc::free(nl as *mut c_void);
                libc::free(fns as *mut c_void);
            }
        }
        libc::pthread_mutex_unlock(&raw mut G_BR_LIBS_LOCK);
        lib
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bridge_lookup(
    lib: *const c_char,
    sym: *const c_char,
) -> *const OcerzBridgeFn {
    unsafe {
        if lib.is_null() || sym.is_null() {
            return core::ptr::null();
        }
        let api = ocerz_apidb_library(lib);
        let e = ocerz_apidb_find(api, sym);
        if e.is_null() || ((*e).kind != OCERZ_API_FN && (*e).kind != OCERZ_API_SPECIAL) {
            return core::ptr::null();
        }
        let bl = br_lib(api);
        if bl.is_null() {
            return core::ptr::null();
        }
        let slot = (*bl)
            .fns
            .add((e as usize - (*api).entries as usize) / size_of::<OcerzApiEntry>());
        let mut fn_ = (*slot).load(Ordering::SeqCst);
        if fn_.is_null() {
            libc::pthread_mutex_lock(&raw mut G_BR_FN_LOCK);
            fn_ = (*slot).load(Ordering::SeqCst);
            if fn_.is_null() {
                fn_ = br_make(api, e);
                if fn_.is_null() {
                    fn_ = BR_NONE;
                }
                (*slot).store(fn_, Ordering::SeqCst);
            }
            libc::pthread_mutex_unlock(&raw mut G_BR_FN_LOCK);
        }
        if fn_ == BR_NONE {
            core::ptr::null()
        } else {
            fn_
        }
    }
}

#[repr(C)]
struct BrInplaceSwap {
    at: u64,
    guest: u64,
    native: u64,
}

unsafe fn br_convert_structs(
    fn_: *const OcerzBridgeFn,
    cpu: *mut OcerzCPU,
    copies: *mut [u64; OCERZ_APIDB_SHAPE_WORDS as usize],
) {
    unsafe {
        for k in 0..(*fn_).nstructs as usize {
            let b = &(*fn_).structs[k];
            let at = if b.reg < 0 {
                gpr(cpu, OCERZ_RSP) + 8 + 8 * b.slot as u64
            } else {
                0
            };
            let gptr = if b.reg < 0 {
                ocerz_ld(at, 8)
            } else {
                gpr(cpu, b.reg as u32)
            };
            if gptr == 0 || ocerz_abi_is_guest_code(gptr) == 0 {
                continue;
            }
            let version = ocerz_ld(gptr, 8);
            let mut shape: *const OcerzApiShape = core::ptr::null();
            let mut i = 0;
            while i < b.nversions && shape.is_null() {
                if (**b.versions.add(i as usize)).version == version {
                    shape = *b.versions.add(i as usize);
                }
                i += 1;
            }
            let mut i = 0;
            while i < b.nversions && shape.is_null() {
                if (**b.versions.add(i as usize)).version == version as u32 as u64 {
                    shape = *b.versions.add(i as usize);
                }
                i += 1;
            }
            if shape.is_null() {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: bridge: %s was handed a %s of version %llu, which ocerz cannot convert\n".as_ptr(),
                    (*fn_).sym,
                    b.shape,
                    version,
                );
                libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
            }
            for w in 0..(*shape).words as usize {
                let mut v = ocerz_ld(gptr + 8 * w as u64, 8);
                if !(*shape).word[w].is_null()
                    && ocerz_abi_callback_convert(v, (*shape).word[w], &mut v) != OCERZ_OK as c_int
                {
                    libc::fprintf(
                        crate::log::stderr(),
                        c"ocerz: bridge: %s could not bind guest function %#llx in word %d of its %s\n".as_ptr(),
                        (*fn_).sym,
                        ocerz_ld(gptr + 8 * w as u64, 8),
                        w,
                        (*shape).name,
                    );
                    libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
                }
                (*copies.add(k))[w] = v;
            }
            if b.reg < 0 {
                ocerz_st(at, 8, ocerz_h2g(copies.add(k) as *const c_void));
            } else {
                set_gpr(cpu, b.reg as u32, ocerz_h2g(copies.add(k) as *const c_void));
            }
        }
    }
}

unsafe fn br_convert_inplace(
    fn_: *const OcerzBridgeFn,
    cpu: *mut OcerzCPU,
    swaps: *mut BrInplaceSwap,
) -> c_int {
    unsafe {
        let mut n = 0;
        for k in 0..(*fn_).ninplace as usize {
            let b = &(*fn_).inplace[k];
            let base = gpr(cpu, b.reg as u32);
            if base == 0 {
                continue;
            }
            let at = base + b.offset as u64;
            let guest = ocerz_ld(at, 8);
            let mut native = guest;
            if guest == 0 || ocerz_abi_is_guest_code(guest) == 0 {
                continue;
            }
            if ocerz_abi_callback_convert(guest, b.sig, &mut native) != OCERZ_OK as c_int {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: bridge: %s could not bind guest function %#llx, %u bytes into its argument\n".as_ptr(),
                    (*fn_).sym,
                    guest,
                    b.offset,
                );
                libc::exit(OCERZ_BRIDGE_UNIMPL_EXIT as c_int);
            }
            ocerz_st(at, 8, native);
            (*swaps.add(n)).at = at;
            (*swaps.add(n)).guest = guest;
            (*swaps.add(n)).native = native;
            n += 1;
        }
        n as c_int
    }
}

#[inline(never)]
unsafe fn br_cross_structs(fn_: *const OcerzBridgeFn, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mut outer: OcerzBridgeFrame = core::mem::zeroed();
        let mut copies =
            [[0u64; OCERZ_APIDB_SHAPE_WORDS as usize]; OCERZ_APIDB_STRUCT_ARGS as usize];
        let mut swaps: [BrInplaceSwap; OCERZ_APIDB_INPLACE as usize] = core::mem::zeroed();

        ocerz_bridge_raise(&mut outer, (*fn_).lib, (*fn_).sym, (*fn_).sig, (*fn_).addr);
        br_convert_structs(fn_, cpu, copies.as_mut_ptr());
        let nswaps = br_convert_inplace(fn_, cpu, swaps.as_mut_ptr());
        let rc = ocerz_abi_perform(&(*fn_).parsed, (*fn_).addr, cpu);
        for k in 0..nswaps as usize {
            if ocerz_ld(swaps[k].at, 8) == swaps[k].native {
                ocerz_st(swaps[k].at, 8, swaps[k].guest);
            }
        }
        ocerz_bridge_lower(&outer);
        rc
    }
}

#[inline]
unsafe fn br_cross(fn_: *const OcerzBridgeFn, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        if (*fn_).nstructs != 0 || (*fn_).ninplace != 0 {
            return br_cross_structs(fn_, cpu);
        }
        let outer = G_BR_FRAME;
        G_BR_FRAME.lib = (*fn_).lib;
        G_BR_FRAME.sym = (*fn_).sym;
        G_BR_FRAME.sig = (*fn_).sig;
        G_BR_FRAME.host_fn = (*fn_).addr;
        G_BR_FRAME.depth = outer.depth + 1;
        let rc = if (*fn_).register_only != 0 {
            ocerz_abi_perform_registers(&(*fn_).parsed, (*fn_).addr, cpu)
        } else {
            ocerz_abi_perform(&(*fn_).parsed, (*fn_).addr, cpu)
        };
        G_BR_FRAME = outer;
        rc
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bridge_invoke(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    fn_: *const OcerzBridgeFn,
) -> c_int {
    unsafe {
        if fn_.is_null() {
            return OCERZ_STEP_FATAL as c_int;
        }
        (*(fn_ as *mut OcerzBridgeFn)).calls += 1;
        if br_logging() != 0 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: BRIDGELOG[%d] %s %s %s from %#llx rdi=%#llx\n".as_ptr(),
                libc::getpid(),
                (*fn_).lib,
                (*fn_).sym,
                if (*fn_).sig.is_null() {
                    c"(nothing)".as_ptr()
                } else {
                    (*fn_).sig
                },
                ocerz_ld(gpr(cpu, OCERZ_RSP), 8),
                gpr(cpu, OCERZ_RDI),
            );
        }
        let mut rc = if let Some(s) = (*fn_).special {
            s(vm, cpu)
        } else {
            br_cross(fn_, cpu)
        };
        if rc == OCERZ_STEP_OK as c_int && (*fn_).special.is_none() {
            rc = br_settle(vm, cpu);
        }
        if br_logging() == 2 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: BRIDGERET[%d] %s rax=%#llx rdx=%#llx rc=%d\n".as_ptr(),
                libc::getpid(),
                (*fn_).sym,
                gpr(cpu, OCERZ_RAX),
                gpr(cpu, OCERZ_RDX),
                rc,
            );
        }
        rc
    }
}

unsafe extern "C" fn br_row_cmp(a: *const c_void, b: *const c_void) -> c_int {
    unsafe {
        let x = (**(a as *const *const OcerzBridgeFn)).calls;
        let y = (**(b as *const *const OcerzBridgeFn)).calls;
        if x < y {
            1
        } else if x > y {
            -1
        } else {
            0
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bridge_report() {
    unsafe {
        let mut made = 0;
        let mut f = G_BR_MADE.load(Ordering::SeqCst);
        while !f.is_null() {
            made += 1;
            f = (*f).next;
        }
        let mut declared = 0;
        let mut l = G_BR_LIBS.load(Ordering::SeqCst);
        while !l.is_null() {
            for i in 0..(*(*l).api).nentries as usize {
                let k = (*(*(*l).api).entries.add(i)).kind;
                if k == OCERZ_API_FN || k == OCERZ_API_SPECIAL {
                    declared += 1;
                }
            }
            l = (*l).next;
        }

        let rows = libc::calloc(
            if made != 0 { made as usize } else { 1 },
            size_of::<*const OcerzBridgeFn>(),
        ) as *mut *const OcerzBridgeFn;
        let mut total: u64 = 0;
        let mut used = 0;
        let mut n = 0;
        let mut f = G_BR_MADE.load(Ordering::SeqCst);
        while !f.is_null() && !rows.is_null() && n < made {
            *rows.add(n) = f;
            n += 1;
            total += (*f).calls;
            if (*f).calls != 0 {
                used += 1;
            }
            f = (*f).next;
        }
        if !rows.is_null() && n > 1 {
            libc::qsort(
                rows as *mut c_void,
                n as usize,
                size_of::<*const OcerzBridgeFn>(),
                Some(br_row_cmp),
            );
        }
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: BRIDGESTAT[%d] crossings=%llu over %d of %d bridged export(s)\n".as_ptr(),
            libc::getpid(),
            total,
            used,
            declared,
        );
        let mut i = 0;
        while !rows.is_null() && i < used {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: BRIDGESTAT[%d]   #%2d %-26s %14llu  %6.2f%%\n".as_ptr(),
                libc::getpid(),
                i + 1,
                (**rows.add(i)).sym,
                (**rows.add(i)).calls,
                if total != 0 {
                    100.0 * (**rows.add(i)).calls as f64 / total as f64
                } else {
                    0.0
                },
            );
            i += 1;
        }
        libc::free(rows as *mut c_void);
    }
}

include!("table.rs");

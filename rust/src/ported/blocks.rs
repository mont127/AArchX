//! Blocks in both directions between x86 guest code and native arm64 code.
//!
//! ocerz/blocks.h states what each conversion builds and why; what follows is
//! what this file knows that the header does not.
//!
//! ---- one layout, two instruction sets ----
//! libclosure's Block_layout is isa, a 32-bit flags word, 32 bits reserved,
//! invoke and descriptor, then the captures; its descriptor is reserved and
//! size, then copy and dispose when BLOCK_HAS_COPY_DISPOSE is set, then the
//! signature and the extended layout when BLOCK_HAS_SIGNATURE is.  A __block
//! variable's Block_byref is isa, forwarding, flags and size, then keep and
//! destroy when BLOCK_BYREF_HAS_COPY_DISPOSE is set, then a layout when
//! BLOCK_BYREF_LAYOUT_EXTENDED is.  clang lays all of these down identically
//! for x86_64 and arm64, with identical flags: an ARC heap block with captures
//! is 0xc1000006 on both, a global block 0x50000000.  So every word here is read
//! the same whichever side made the block, and only the function words need
//! converting.  A block whose flags carry BLOCK_SMALL_DESCRIPTOR has a relative
//! descriptor no x86 compiler emits; its signature is asked of the native
//! _Block_signature, and it is never taken for one of this file's wrappers.
//!
//! ---- the wrappers' descriptors ----
//! A wrapper's descriptor is a libclosure descriptor followed by fields of this
//! file's own, which native code never reads: the notation the wrapped block's
//! invoke is called under, parsed, and for a native wrapper the callback slot
//! already bound for it.  A native wrapper's descriptor is kept per guest
//! descriptor, because every block of one literal shares its descriptor and its
//! invoke function, so the notation is converted and the slot interned once per
//! literal rather than once per crossing; a guest view's is kept per native
//! signature string, or per declared notation for a native block without one.
//! Descriptors are never freed.  A notation that does not convert still makes a
//! descriptor, marked dead with the reason: the wrapper is made and handed over,
//! and only calling it stops the process by name, because native code and guest
//! code both hand blocks around that nobody ever calls.
//!
//! ---- lifetime ----
//! Every wrapper is a heap or a global block with native helpers, so its
//! lifetime is libclosure's own: its count lives in its flags, native
//! _Block_copy raises it, native _Block_release lowers it and at zero calls the
//! dispose helper and frees it.  The dispose helper takes the wrapper out of
//! the table that maps what it wraps to it, but only if the table still names
//! this wrapper, and then releases what it wrapped, outside the table's lock:
//! that release can run a guest dispose helper, which can convert blocks of its
//! own.  A lookup retains a wrapper it finds with _Block_tryRetain, which
//! refuses one whose count has already reached zero, so a wrapper on its way out
//! is replaced in the table rather than revived.  Global blocks, on either side,
//! never die, and neither do their wrappers.
//!
//! ---- a guest stack block ----
//! A stack block cannot be wrapped as it is, because the wrapper may outlive the
//! guest frame the block lives in, and it cannot be handed to the native
//! _Block_copy, which would call its x86 copy helper as arm64 code.  So
//! ocerz_block_copy_guest does what _Block_copy does, in the same order: copy
//! the block's bytes to the heap, reset its count to one, and run the copy
//! helper from the old block to the new, then set the new block's isa.  The
//! helper runs through a callback slot, the same way native code runs a guest
//! callback, so it runs as guest code on the calling thread and its own calls
//! to _Block_object_assign come back here.  The copy's descriptor is a shadow
//! of the guest's, kept per guest descriptor, whose copy and dispose words are
//! slots for the guest's helpers: the guest's own descriptor is in the guest's
//! constant data and is never written, and with the shadow in place the native
//! _Block_release that frees the copy runs the guest's dispose helper as guest
//! code instead of as arm64 code.  The same is done for a __block variable
//! still on the stack whose keep and destroy helpers are guest code, the heap
//! copy holding slots for them, and the stack variable's forwarding pointer is
//! pointed at the copy as libclosure does, so the guest frame's own accesses,
//! which go through forwarding, see the variable the blocks see.  Under a low
//! shadow (native Wine) a variable on a stack below 12 GB forwards to a guest
//! address libclosure cannot follow, so the forwarding word is rewritten to its
//! host alias first, which the guest reads as the same memory.
//!
//! ---- calling a guest view ----
//! A guest view's invoke is a trampoline ocerz wrote into a guest page, twelve
//! bytes of the same shape as a synthesized library's stubs, so the JIT's fast
//! call reaches ocerz_block_invoke_trap from inside translated code as it
//! reaches any bridged function.  The view is in rdi, or in rsi behind a
//! structure result's pointer, and it is found by its invoke word.  The call
//! goes to the native block's invoke under the notation in the view's
//! descriptor, whose first argument unwraps the view, as a crossing like any
//! other: a bridge frame names it while native code runs, a block result is
//! borrowed, and pending guest signals are delivered on the way out.

use core::ffi::{c_char, c_int, c_uint, c_void};
use core::mem::{offset_of, size_of};
use core::ptr;
use core::sync::atomic::{AtomicI32, AtomicPtr, AtomicU64, Ordering};

use crate::ffi;
use crate::inline::{ocerz_g2h, ocerz_h2g, ocerz_ld, ocerz_st};

const BLK_DEALLOCATING: i32 = 0x0001;
const BLK_REFCOUNT_MASK: i32 = 0xfffe;
const BLK_SMALL_DESCRIPTOR: i32 = 1 << 22;
const BLK_NEEDS_FREE: i32 = 1 << 24;
const BLK_HAS_COPY_DISPOSE: i32 = 1 << 25;
const BLK_IS_GLOBAL: i32 = 1 << 28;
const BLK_USE_STRET: i32 = 1 << 29;
const BLK_HAS_SIGNATURE: i32 = 1 << 30;
const BLK_BYREF_NEEDS_FREE: i32 = 1 << 24;
const BLK_BYREF_HAS_COPY_DISPOSE: i32 = 1 << 25;
const BLK_BYREF_LAYOUT_EXTENDED: i32 = 1 << 28;
const BLK_FIELD_IS_BLOCK: i32 = 7;
const BLK_FIELD_IS_BYREF: i32 = 8;
const BLK_FIELD_IS_WEAK: i32 = 16;
const BLK_ALL_COPY_DISPOSE_FLAGS: i32 = 0x9f;
const BLK_BUCKETS: usize = 1024;
const BLK_MAX_SIZE: usize = 1 << 24;

type CopyFn = unsafe extern "C" fn(*mut c_void, *const c_void);
type DisposeFn = unsafe extern "C" fn(*const c_void);

#[repr(C)]
struct BlkLayout {
    isa: *mut c_void,
    flags: AtomicI32,
    reserved: i32,
    invoke: *mut c_void,
    desc: *const c_void,
    inner: u64,
}

#[repr(C)]
struct BlkByref {
    isa: *mut c_void,
    forwarding: *mut BlkByref,
    flags: AtomicI32,
    size: u32,
    keep: u64,
    destroy: u64,
    layout: u64,
}

#[repr(C)]
struct BlkDesc {
    reserved: u64,
    size: u64,
    copy: Option<CopyFn>,
    dispose: Option<DisposeFn>,
    signature: *const c_char,
    layout: *const c_char,
    dead: *const c_char,
    slot: *mut c_void,
    slot_for: u64,
    sig: ffi::OcerzAbiSig,
    notation: [c_char; ffi::OCERZ_BLOCK_NOTATION_MAX as usize],
}

#[repr(C)]
struct BlkNode {
    next: *mut BlkNode,
    key: u64,
    val: *mut c_void,
}

#[repr(C)]
struct BlkMap {
    bucket: [*mut BlkNode; BLK_BUCKETS],
}

const _: () = assert!(size_of::<BlkLayout>() == 40);
const _: () = assert!(offset_of!(BlkLayout, inner) == 32);
const _: () = assert!(size_of::<BlkByref>() == 48);
const _: () = assert!(offset_of!(BlkByref, layout) == 40);
const _: () = assert!(size_of::<BlkDesc>() == 1784);
const _: () = assert!(offset_of!(BlkDesc, notation) == 1524);
const _: () = assert!(size_of::<BlkNode>() == 24);
const _: () = assert!(offset_of!(BlkNode, key) == 8);
const _: () = assert!(offset_of!(BlkNode, val) == 16);
const _: () = assert!(size_of::<BlkMap>() == BLK_BUCKETS * size_of::<*mut BlkNode>());

unsafe extern "C" {
    fn _Block_copy(block: *const c_void) -> *mut c_void;
    fn _Block_release(block: *const c_void);
    fn _Block_object_assign(dst: *mut c_void, src: *const c_void, flags: c_int);
    fn _Block_signature(block: *mut c_void) -> *const c_char;
    fn _Block_tryRetain(block: *const c_void) -> bool;
    static mut _NSConcreteStackBlock: [*mut c_void; 32];
    static mut _NSConcreteMallocBlock: [*mut c_void; 32];
    static mut _NSConcreteGlobalBlock: [*mut c_void; 32];
}

static mut G_BLK_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
static mut G_BLK_LIVE_NATIVE: BlkMap = BlkMap {
    bucket: [ptr::null_mut(); BLK_BUCKETS],
};
static mut G_BLK_LIVE_VIEW: BlkMap = BlkMap {
    bucket: [ptr::null_mut(); BLK_BUCKETS],
};
static mut G_BLK_GLOBAL_NATIVE: BlkMap = BlkMap {
    bucket: [ptr::null_mut(); BLK_BUCKETS],
};
static mut G_BLK_GLOBAL_VIEW: BlkMap = BlkMap {
    bucket: [ptr::null_mut(); BLK_BUCKETS],
};
static mut G_BLK_NATIVE_DESC: BlkMap = BlkMap {
    bucket: [ptr::null_mut(); BLK_BUCKETS],
};
static mut G_BLK_VIEW_DESC: BlkMap = BlkMap {
    bucket: [ptr::null_mut(); BLK_BUCKETS],
};
static mut G_BLK_SHADOW: BlkMap = BlkMap {
    bucket: [ptr::null_mut(); BLK_BUCKETS],
};
static mut G_BLK_NATIVE_DESC_NAMED: *mut BlkNode = ptr::null_mut();
static mut G_BLK_VIEW_DESC_NAMED: *mut BlkNode = ptr::null_mut();
static G_BLK_THUNK: AtomicU64 = AtomicU64::new(0);
static G_AUTORELEASE: AtomicPtr<c_void> = AtomicPtr::new(ptr::null_mut());
static G_AUTORELEASE_LOOKED: AtomicI32 = AtomicI32::new(0);

#[inline]
unsafe fn concrete_stack() -> *mut c_void {
    ptr::addr_of_mut!(_NSConcreteStackBlock).cast()
}

#[inline]
unsafe fn concrete_malloc() -> *mut c_void {
    ptr::addr_of_mut!(_NSConcreteMallocBlock).cast()
}

#[inline]
unsafe fn concrete_global() -> *mut c_void {
    ptr::addr_of_mut!(_NSConcreteGlobalBlock).cast()
}

#[inline]
unsafe fn blk_hash(key: u64) -> usize {
    let k = key.wrapping_mul(0x9e3779b97f4a7c15);
    ((k ^ (k >> 29)) as usize) & (BLK_BUCKETS - 1)
}

unsafe fn blk_map_get(map: *const BlkMap, key: u64) -> *mut c_void {
    let mut n = *(*map).bucket.as_ptr().add(blk_hash(key));
    while !n.is_null() {
        if (*n).key == key {
            return (*n).val;
        }
        n = (*n).next;
    }
    ptr::null_mut()
}

unsafe fn blk_map_put(map: *mut BlkMap, key: u64, val: *mut c_void) -> bool {
    let b = blk_hash(key);
    let bucket = (*map).bucket.as_mut_ptr().add(b);
    let mut n = *bucket;
    while !n.is_null() {
        if (*n).key == key {
            (*n).val = val;
            return true;
        }
        n = (*n).next;
    }
    let n = libc::malloc(size_of::<BlkNode>()) as *mut BlkNode;
    if n.is_null() {
        return false;
    }
    (*n).key = key;
    (*n).val = val;
    (*n).next = *bucket;
    *bucket = n;
    true
}

unsafe fn blk_map_drop(map: *mut BlkMap, key: u64, val: *const c_void) {
    let mut at = (*map).bucket.as_mut_ptr().add(blk_hash(key));
    while !(*at).is_null() {
        let n = *at;
        if (*n).key == key {
            if (*n).val == val as *mut c_void {
                *at = (*n).next;
                libc::free(n as *mut c_void);
            }
            return;
        }
        at = &mut (*n).next;
    }
}

unsafe fn blk_host(block: u64) -> *mut BlkLayout {
    ocerz_g2h(block).cast()
}

unsafe fn blk_guest(host: *const c_void) -> u64 {
    if host.is_null() { 0 } else { ocerz_h2g(host) }
}

unsafe fn blk_thunk() -> u64 {
    let mut t = G_BLK_THUNK.load(Ordering::SeqCst);
    if t == 0 {
        t = ffi::ocerz_vdylib_trampoline(ffi::OCERZ_VDYLIB_TRAMP_BLOCK_INVOKE);
        if t != 0 {
            G_BLK_THUNK.store(t, Ordering::SeqCst);
        }
    }
    t
}

unsafe fn blk_is_view(block: *const BlkLayout) -> bool {
    let t = G_BLK_THUNK.load(Ordering::SeqCst);
    t != 0 && (*block).invoke as u64 == t
}

unsafe fn blk_signature(block: *const BlkLayout, guest: bool) -> *const c_char {
    let flags = (*block).flags.load(Ordering::SeqCst);
    if !guest && flags & BLK_SMALL_DESCRIPTOR != 0 {
        return _Block_signature(block as *mut BlkLayout as *mut c_void);
    }
    if flags & BLK_HAS_SIGNATURE == 0 || (*block).desc.is_null() {
        return ptr::null();
    }
    let d = if guest {
        ocerz_g2h((*block).desc as u64).cast::<u64>()
    } else {
        (*block).desc.cast::<u64>()
    };
    let sig = *d.add(if flags & BLK_HAS_COPY_DISPOSE != 0 {
        4
    } else {
        2
    });
    if sig == 0 {
        ptr::null()
    } else if guest {
        ocerz_g2h(sig).cast()
    } else {
        sig as *const c_char
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_block_native_wrapper(block: u64, inner: *mut u64) -> c_int {
    unsafe {
        if !inner.is_null() {
            *inner = 0;
        }
        if block == 0 {
            return 0;
        }
        let b = blk_host(block);
        let flags = (*b).flags.load(Ordering::SeqCst);
        if flags & BLK_HAS_COPY_DISPOSE == 0
            || flags & BLK_SMALL_DESCRIPTOR != 0
            || (*b).desc.is_null()
        {
            return 0;
        }
        let d = (*b).desc.cast::<BlkDesc>();
        if !matches!((*d).dispose, Some(dispose) if ptr::fn_addr_eq(dispose, blk_native_dispose_helper as DisposeFn))
        {
            return 0;
        }
        if !inner.is_null() {
            *inner = (*b).inner;
        }
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_block_guest_view(block: u64, inner: *mut u64) -> c_int {
    unsafe {
        if !inner.is_null() {
            *inner = 0;
        }
        if block == 0 {
            return 0;
        }
        let b = blk_host(block);
        if !blk_is_view(b) {
            return 0;
        }
        if !inner.is_null() {
            *inner = (*b).inner;
        }
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_block_is_guest_object(obj: u64) -> c_int {
    unsafe {
        if obj == 0 || obj & 7 != 0 {
            return 0;
        }
        let b = blk_host(obj);
        let isa = (*b).isa;
        if isa != concrete_stack() && isa != concrete_malloc() && isa != concrete_global() {
            return 0;
        }
        (!blk_is_view(b) && ffi::ocerz_abi_is_guest_code((*b).invoke as u64) != 0) as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_block_is_guest(block: u64) -> c_int {
    unsafe {
        if block == 0 {
            0
        } else {
            (ffi::ocerz_abi_is_guest_code((*blk_host(block)).invoke as u64) != 0) as c_int
        }
    }
}

unsafe fn blk_insert_self(declared: *const c_char, out: *mut c_char, outlen: usize) -> c_int {
    let len = libc::strlen(declared);
    let mut open = ptr::null();
    let mut depth = 0;
    let mut p = declared;
    while *p != 0 {
        if *p as u8 == b'{' {
            depth += 1;
        } else if *p as u8 == b'}' {
            depth -= 1;
        } else if *p as u8 == b'(' && depth == 0 {
            open = p;
            break;
        }
        p = p.add(1);
    }
    if open.is_null() || len.wrapping_add(4) > outlen {
        return ffi::OCERZ_EFORMAT as c_int;
    }
    let head = open.offset_from(declared) as usize + 1;
    ptr::copy_nonoverlapping(declared, out, head);
    ptr::copy_nonoverlapping(c"k{}".as_ptr().cast::<c_char>(), out.add(head), 3);
    ptr::copy_nonoverlapping(
        open.add(1),
        out.add(head.wrapping_add(3)),
        len.wrapping_sub(head).wrapping_add(1),
    );
    ffi::OCERZ_OK as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_block_invoke_notation(
    encoding: *const c_char,
    declared: *const c_char,
    out: *mut c_char,
    outlen: usize,
) -> c_int {
    unsafe {
        if out.is_null() || outlen == 0 {
            return ffi::OCERZ_EUNDEF as c_int;
        }
        *out = 0;
        if !encoding.is_null() {
            let mut nargs = 0;
            if ffi::ocerz_objc_notation(
                encoding,
                out,
                outlen,
                &mut nargs,
                ptr::null_mut(),
                ptr::null_mut(),
            ) != ffi::OCERZ_OBJC_OK as c_int
            {
                return ffi::OCERZ_EUNSUP as c_int;
            }
        } else if !declared.is_null() && *declared != 0 {
            let r = blk_insert_self(declared, out, outlen);
            if r != ffi::OCERZ_OK as c_int {
                return r;
            }
        } else {
            return ffi::OCERZ_EUNDEF as c_int;
        }
        let mut sig = core::mem::MaybeUninit::<ffi::OcerzAbiSig>::uninit();
        if ffi::ocerz_abi_parse(out, sig.as_mut_ptr()) != ffi::OCERZ_OK as c_int {
            return ffi::OCERZ_EUNSUP as c_int;
        }
        let sig = sig.assume_init();
        if sig.nargs < 1 || *sig.arg.as_ptr() != b'k' as c_char {
            return ffi::OCERZ_EFORMAT as c_int;
        }
        for i in 0..sig.nargs as usize {
            if *sig.arg.as_ptr().add(i) == b'c' as c_char {
                return ffi::OCERZ_EUNSUP as c_int;
            }
        }
        ffi::OCERZ_OK as c_int
    }
}

unsafe fn blk_notation_refusal(rc: c_int) -> *const c_char {
    match rc {
        x if x == ffi::OCERZ_EUNDEF as c_int => {
            c"it has no signature and its declaration gives none".as_ptr()
        }
        x if x == ffi::OCERZ_EFORMAT as c_int => {
            c"its signature does not take the block itself as its first argument".as_ptr()
        }
        _ => c"its signature has a type the ABI engine does not carry".as_ptr(),
    }
}

unsafe fn blk_desc_fill(d: *mut BlkDesc, encoding: *const c_char, declared: *const c_char) {
    let rc = ocerz_block_invoke_notation(
        encoding,
        declared,
        (*d).notation.as_mut_ptr(),
        (*d).notation.len(),
    );
    let mut rc = rc;
    if rc == ffi::OCERZ_OK as c_int
        && ffi::ocerz_abi_parse((*d).notation.as_ptr(), &mut (*d).sig) != ffi::OCERZ_OK as c_int
    {
        rc = ffi::OCERZ_EUNSUP as c_int;
    }
    if rc != ffi::OCERZ_OK as c_int {
        (*d).dead = blk_notation_refusal(rc);
    }
}

unsafe fn blk_desc_locked(
    map: *mut BlkMap,
    named: *mut *mut BlkNode,
    key: u64,
    encoding: *const c_char,
    declared: *const c_char,
    view: bool,
) -> *mut BlkDesc {
    let name = if declared.is_null() {
        c"".as_ptr()
    } else {
        declared
    };
    let mut d = if !encoding.is_null() {
        blk_map_get(map, key).cast()
    } else {
        let mut n = *named;
        let mut found: *mut BlkDesc = ptr::null_mut();
        while !n.is_null() {
            if (*n).key == key && libc::strcmp((n as *const BlkNode).add(1).cast(), name) == 0 {
                found = (*n).val.cast();
                break;
            }
            n = (*n).next;
        }
        found
    };
    if !d.is_null() {
        return d;
    }
    d = libc::calloc(1, size_of::<BlkDesc>()).cast();
    if d.is_null() {
        return d;
    }
    (*d).size = size_of::<BlkLayout>() as u64;
    (*d).copy = Some(if view {
        blk_view_copy_helper
    } else {
        blk_native_copy_helper
    });
    (*d).dispose = Some(if view {
        blk_view_dispose_helper
    } else {
        blk_native_dispose_helper
    });
    (*d).signature = encoding;
    blk_desc_fill(d, encoding, declared);
    if !encoding.is_null() {
        if !blk_map_put(map, key, d.cast()) {
            libc::free(d.cast());
            return ptr::null_mut();
        }
        return d;
    }
    let len = libc::strlen(name);
    let n = libc::malloc(
        size_of::<BlkNode>()
            .wrapping_add(len)
            .wrapping_add(1),
    )
    .cast::<BlkNode>();
    if n.is_null() {
        libc::free(d.cast());
        return ptr::null_mut();
    }
    ptr::copy_nonoverlapping(
        name,
        (n as *mut u8).add(size_of::<BlkNode>()).cast(),
        len.wrapping_add(1),
    );
    (*n).key = key;
    (*n).val = d.cast();
    (*n).next = *named;
    *named = n;
    d
}

unsafe fn blk_native_desc_locked(
    g: *const BlkLayout,
    encoding: *const c_char,
    declared: *const c_char,
) -> *mut BlkDesc {
    blk_desc_locked(
        ptr::addr_of_mut!(G_BLK_NATIVE_DESC),
        ptr::addr_of_mut!(G_BLK_NATIVE_DESC_NAMED),
        (*g).desc as u64,
        encoding,
        declared,
        false,
    )
}

unsafe fn blk_view_desc_locked(encoding: *const c_char, declared: *const c_char) -> *mut BlkDesc {
    blk_desc_locked(
        ptr::addr_of_mut!(G_BLK_VIEW_DESC),
        ptr::addr_of_mut!(G_BLK_VIEW_DESC_NAMED),
        encoding as u64,
        encoding,
        declared,
        true,
    )
}

unsafe fn blk_dead_call(what: *const c_char, d: *const BlkDesc) -> ! {
    libc::fprintf(
        crate::log::stderr(),
        c"ocerz: blocks: %s was called, and it cannot be: %s (signature %s)\n".as_ptr(),
        what,
        if !d.is_null() && !(*d).dead.is_null() {
            (*d).dead
        } else {
            c"it was made without a descriptor".as_ptr()
        },
        if !d.is_null() && !(*d).signature.is_null() {
            (*d).signature
        } else {
            c"none".as_ptr()
        },
    );
    libc::fflush(crate::log::stderr());
    libc::exit(ffi::OCERZ_BRIDGE_UNIMPL_EXIT as c_int)
}

unsafe extern "C" fn blk_dead_invoke(block: *mut c_void) {
    blk_dead_call(
        c"a guest block handed to native code".as_ptr(),
        if block.is_null() {
            ptr::null()
        } else {
            (*block.cast::<BlkLayout>()).desc.cast()
        },
    );
}

unsafe fn blk_native_invoke_locked(d: *mut BlkDesc, g: *const BlkLayout) -> *mut c_void {
    let fn_ = (*g).invoke as u64;
    if !(*d).dead.is_null() {
        return blk_dead_invoke as *mut c_void;
    }
    if !(*d).slot.is_null() && (*d).slot_for == fn_ {
        return (*d).slot;
    }
    let slot = ffi::ocerz_abi_callback_intern(fn_, (*d).notation.as_ptr());
    if slot.is_null() {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: blocks: guest block invoke %#llx under %s got no callback slot\n".as_ptr(),
            fn_,
            (*d).notation.as_ptr(),
        );
        return ptr::null_mut();
    }
    (*d).slot = slot;
    (*d).slot_for = fn_;
    slot
}

unsafe fn blk_make(
    isa: *mut c_void,
    flags: i32,
    invoke: *mut c_void,
    d: *const BlkDesc,
    inner: u64,
) -> *mut BlkLayout {
    let w = libc::malloc(size_of::<BlkLayout>()).cast::<BlkLayout>();
    if w.is_null() {
        return w;
    }
    (*w).isa = isa;
    (*w).flags = AtomicI32::new(flags);
    (*w).reserved = 0;
    (*w).invoke = invoke;
    (*w).desc = d.cast();
    (*w).inner = inner;
    w
}

unsafe fn blk_wrapper_flags(inner: *const BlkLayout, d: *const BlkDesc, global: bool) -> i32 {
    let mut f = BLK_HAS_COPY_DISPOSE | ((*inner).flags.load(Ordering::SeqCst) & BLK_USE_STRET);
    if !(*d).signature.is_null() {
        f |= BLK_HAS_SIGNATURE;
    }
    f | if global {
        BLK_IS_GLOBAL
    } else {
        BLK_NEEDS_FREE | 2
    }
}

unsafe fn blk_wrap_guest(
    g: *mut BlkLayout,
    declared: *const c_char,
    out: *mut u64,
    owned: *mut u64,
) -> c_int {
    let global = (*g).flags.load(Ordering::SeqCst) & BLK_IS_GLOBAL != 0;
    let map = if global {
        ptr::addr_of_mut!(G_BLK_GLOBAL_NATIVE)
    } else {
        ptr::addr_of_mut!(G_BLK_LIVE_NATIVE)
    };
    let key = g as u64;
    let enc = blk_signature(g, true);
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_BLK_LOCK));
    let mut w = blk_map_get(map, key).cast::<BlkLayout>();
    if !w.is_null() && (global || _Block_tryRetain(w.cast())) {
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_BLK_LOCK));
        *out = w as u64;
        *owned = if global { 0 } else { w as u64 };
        return ffi::OCERZ_OK as c_int;
    }
    let d = blk_native_desc_locked(g, enc, declared);
    let invoke = if d.is_null() {
        ptr::null_mut()
    } else {
        blk_native_invoke_locked(d, g)
    };
    if !invoke.is_null() {
        let isa = if global {
            concrete_global()
        } else {
            concrete_malloc()
        };
        w = blk_make(
            isa,
            blk_wrapper_flags(g, d, global),
            invoke,
            d.cast(),
            blk_guest(g.cast()),
        );
        if !w.is_null() && !blk_map_put(map, key, w.cast()) {
            libc::free(w.cast());
            w = ptr::null_mut();
        }
        if !w.is_null() && !global {
            _Block_copy(g.cast());
        }
    }
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_BLK_LOCK));
    if w.is_null() {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: blocks: guest block %#llx could not be given a native wrapper\n".as_ptr(),
            blk_guest(g.cast()) as libc::c_ulonglong,
        );
        return ffi::OCERZ_ENOMEM as c_int;
    }
    *out = w as u64;
    *owned = if global { 0 } else { w as u64 };
    ffi::OCERZ_OK as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_block_to_native(
    gblock: u64,
    declared: *const c_char,
    out: *mut u64,
    owned: *mut u64,
) -> c_int {
    unsafe {
        let mut scratch = 0;
        let owned = if owned.is_null() {
            &mut scratch
        } else {
            &mut *owned
        };
        *owned = 0;
        if out.is_null() {
            return ffi::OCERZ_EUNDEF as c_int;
        }
        *out = 0;
        if gblock == 0 {
            return ffi::OCERZ_OK as c_int;
        }
        let g = blk_host(gblock);
        if blk_is_view(g) {
            *out = (*g).inner;
            return ffi::OCERZ_OK as c_int;
        }
        if ffi::ocerz_abi_is_guest_code((*g).invoke as u64) == 0 {
            *out = g as u64;
            return ffi::OCERZ_OK as c_int;
        }
        let flags = (*g).flags.load(Ordering::SeqCst);
        if flags & (BLK_IS_GLOBAL | BLK_NEEDS_FREE) != 0 {
            return blk_wrap_guest(g, declared, out, owned);
        }
        let copy = ocerz_block_copy_guest(gblock);
        if copy == 0 {
            return ffi::OCERZ_ENOMEM as c_int;
        }
        let r = blk_wrap_guest(blk_host(copy), declared, out, owned);
        _Block_release(blk_host(copy).cast());
        r
    }
}

unsafe fn blk_view_native(
    n: *mut BlkLayout,
    declared: *const c_char,
    out: *mut u64,
    owned: *mut u64,
) -> c_int {
    let global = (*n).flags.load(Ordering::SeqCst) & BLK_IS_GLOBAL != 0;
    let map = if global {
        ptr::addr_of_mut!(G_BLK_GLOBAL_VIEW)
    } else {
        ptr::addr_of_mut!(G_BLK_LIVE_VIEW)
    };
    let enc = blk_signature(n, false);
    let thunk = blk_thunk();
    if thunk == 0 {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: blocks: no guest page holds the trampoline a native block needs\n".as_ptr(),
        );
        return ffi::OCERZ_ENOMEM as c_int;
    }
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_BLK_LOCK));
    let mut v = blk_map_get(map, n as u64).cast::<BlkLayout>();
    if !v.is_null() && (global || _Block_tryRetain(v.cast())) {
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_BLK_LOCK));
        *out = blk_guest(v.cast());
        *owned = if global { 0 } else { v as u64 };
        return ffi::OCERZ_OK as c_int;
    }
    let d = blk_view_desc_locked(enc, declared);
    if !d.is_null() {
        let isa = if global {
            concrete_global()
        } else {
            concrete_malloc()
        };
        v = blk_make(
            isa,
            blk_wrapper_flags(n, d, global),
            thunk as *mut c_void,
            d.cast(),
            n as u64,
        );
        if !v.is_null() && !blk_map_put(map, n as u64, v.cast()) {
            libc::free(v.cast());
            v = ptr::null_mut();
        }
        if !v.is_null() && !global {
            _Block_copy(n.cast());
        }
    }
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_BLK_LOCK));
    if v.is_null() {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: blocks: native block %p could not be given a guest view\n".as_ptr(),
            n,
        );
        return ffi::OCERZ_ENOMEM as c_int;
    }
    *out = blk_guest(v.cast());
    *owned = if global { 0 } else { v as u64 };
    ffi::OCERZ_OK as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_block_to_guest(
    nblock: u64,
    declared: *const c_char,
    out: *mut u64,
    owned: *mut u64,
) -> c_int {
    unsafe {
        let mut scratch = 0;
        let owned = if owned.is_null() {
            &mut scratch
        } else {
            &mut *owned
        };
        *owned = 0;
        if out.is_null() {
            return ffi::OCERZ_EUNDEF as c_int;
        }
        *out = 0;
        if nblock == 0 {
            return ffi::OCERZ_OK as c_int;
        }
        let n = nblock as *mut BlkLayout;
        let mut inner = 0;
        if ocerz_block_native_wrapper(nblock, &mut inner) != 0 {
            *out = inner;
            return ffi::OCERZ_OK as c_int;
        }
        if ffi::ocerz_abi_is_guest_code((*n).invoke as u64) != 0 {
            *out = blk_guest(n.cast());
            return ffi::OCERZ_OK as c_int;
        }
        let f = (*n).flags.load(Ordering::SeqCst);
        if f & (BLK_IS_GLOBAL | BLK_NEEDS_FREE) != 0 {
            return blk_view_native(n, declared, out, owned);
        }
        let copy = _Block_copy(n.cast());
        if copy.is_null() {
            return ffi::OCERZ_ENOMEM as c_int;
        }
        let r = blk_view_native(copy.cast(), declared, out, owned);
        _Block_release(copy);
        r
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_block_release(block: u64) {
    unsafe {
        if block != 0 {
            _Block_release(block as *const c_void);
        }
    }
}

unsafe fn blk_autorelease(block: u64) {
    if G_AUTORELEASE_LOOKED.load(Ordering::SeqCst) == 0 {
        let p = libc::dlsym(libc::RTLD_DEFAULT, c"objc_autorelease".as_ptr());
        G_AUTORELEASE.store(p, Ordering::SeqCst);
        G_AUTORELEASE_LOOKED.store(1, Ordering::SeqCst);
    }
    let p = G_AUTORELEASE.load(Ordering::SeqCst);
    if !p.is_null() {
        core::mem::transmute::<
            *mut c_void,
            unsafe extern "C" fn(*mut c_void) -> *mut c_void,
        >(p)(
            block as *mut c_void,
        );
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_block_result_to_guest(
    nblock: u64,
    declared: *const c_char,
    borrowed: c_int,
    out: *mut u64,
) -> c_int {
    unsafe {
        if out.is_null() {
            return ffi::OCERZ_EUNDEF as c_int;
        }
        *out = 0;
        let mut inner = 0;
        let unwrapped = ocerz_block_native_wrapper(nblock, &mut inner);
        let mut owned = 0;
        let r = ocerz_block_to_guest(nblock, declared, out, &mut owned);
        if r != ffi::OCERZ_OK as c_int {
            return r;
        }
        if borrowed != 0 {
            if owned != 0 {
                blk_autorelease(owned);
            }
        } else if owned != 0 {
            _Block_release(nblock as *const c_void);
        } else if unwrapped != 0 {
            _Block_copy(blk_host(inner).cast());
            _Block_release(nblock as *const c_void);
        }
        ffi::OCERZ_OK as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_block_result_to_native(
    gblock: u64,
    declared: *const c_char,
    out: *mut u64,
) -> c_int {
    unsafe {
        let mut owned = 0;
        let r = ocerz_block_to_native(gblock, declared, out, &mut owned);
        if r == ffi::OCERZ_OK as c_int && owned != 0 {
            blk_autorelease(owned);
        }
        r
    }
}

unsafe extern "C" fn blk_native_copy_helper(dst: *mut c_void, src: *const c_void) {
    let s = src.cast::<BlkLayout>();
    let d = dst.cast::<BlkLayout>();
    (*d).inner = (*s).inner;
    _Block_copy(blk_host((*s).inner).cast());
}
unsafe extern "C" fn blk_native_dispose_helper(block: *const c_void) {
    let w = block.cast::<BlkLayout>();
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_BLK_LOCK));
    blk_map_drop(
        ptr::addr_of_mut!(G_BLK_LIVE_NATIVE),
        blk_host((*w).inner) as u64,
        w.cast(),
    );
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_BLK_LOCK));
    _Block_release(blk_host((*w).inner).cast());
}
unsafe extern "C" fn blk_view_copy_helper(dst: *mut c_void, src: *const c_void) {
    (*dst.cast::<BlkLayout>()).inner =
        _Block_copy((*src.cast::<BlkLayout>()).inner as *const c_void) as u64;
}
unsafe extern "C" fn blk_view_dispose_helper(block: *const c_void) {
    let v = block.cast::<BlkLayout>();
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_BLK_LOCK));
    blk_map_drop(ptr::addr_of_mut!(G_BLK_LIVE_VIEW), (*v).inner, v.cast());
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_BLK_LOCK));
    _Block_release((*v).inner as *const c_void);
}

unsafe fn blk_helper_slot(fn_: u64, notation: *const c_char, out: *mut u64) -> bool {
    if ffi::ocerz_abi_callback_convert(fn_, notation, out) != ffi::OCERZ_OK as c_int {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: blocks: guest helper %#llx could not be bound to a callback slot\n".as_ptr(),
            fn_ as libc::c_ulonglong,
        );
        return false;
    }
    if *out != 0 {
        *out = ocerz_g2h(*out) as u64;
    }
    true
}

unsafe fn blk_shadow(gdesc: u64, flags: i32) -> *const u64 {
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_BLK_LOCK));
    let old = blk_map_get(ptr::addr_of_mut!(G_BLK_SHADOW), gdesc).cast::<u64>();
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_BLK_LOCK));
    if !old.is_null() {
        return old;
    }
    let s = libc::calloc(6, size_of::<u64>()).cast::<u64>();
    if s.is_null() {
        return ptr::null();
    }
    let d = ocerz_g2h(gdesc).cast::<u64>();
    *s = *d;
    *s.add(1) = *d.add(1);
    if !blk_helper_slot(*d.add(2), c"v(pp)".as_ptr(), s.add(2))
        || !blk_helper_slot(*d.add(3), c"v(p)".as_ptr(), s.add(3))
    {
        libc::free(s.cast());
        return ptr::null();
    }
    if flags & BLK_HAS_SIGNATURE != 0 {
        *s.add(4) = if *d.add(4) != 0 {
            ocerz_g2h(*d.add(4)) as u64
        } else {
            0
        };
        *s.add(5) = *d.add(5);
    }
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_BLK_LOCK));
    let had = blk_map_get(ptr::addr_of_mut!(G_BLK_SHADOW), gdesc).cast::<u64>();
    if had.is_null() {
        if !blk_map_put(ptr::addr_of_mut!(G_BLK_SHADOW), gdesc, s.cast()) {
            libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_BLK_LOCK));
            libc::free(s.cast());
            return ptr::null();
        }
    }
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_BLK_LOCK));
    if had.is_null() {
        s
    } else {
        libc::free(s.cast());
        had
    }
}

unsafe fn blk_stop(fmt: *const c_char, a: u64) -> ! {
    libc::fputs(c"ocerz: blocks: ".as_ptr(), crate::log::stderr());
    libc::fprintf(crate::log::stderr(), fmt, a);
    libc::fputc(b'\n' as c_int, crate::log::stderr());
    libc::fflush(crate::log::stderr());
    libc::exit(ffi::OCERZ_BRIDGE_UNIMPL_EXIT as c_int)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_block_copy_guest(gblock: u64) -> u64 {
    unsafe {
        if gblock == 0 {
            return 0;
        }
        let b = blk_host(gblock);
        let flags = (*b).flags.load(Ordering::SeqCst);
        if flags & (BLK_NEEDS_FREE | BLK_IS_GLOBAL) != 0
            || blk_is_view(b)
            || ffi::ocerz_abi_is_guest_code((*b).invoke as u64) == 0
        {
            return blk_guest(_Block_copy(b.cast()));
        }
        let gdesc = (*b).desc as u64;
        let size = if gdesc != 0 {
            ocerz_ld(gdesc.wrapping_add(8), 8) as usize
        } else {
            0
        };
        if size < 32 || size > BLK_MAX_SIZE {
            blk_stop(c"guest stack block %#llx has a descriptor ocerz cannot read, so it cannot be copied".as_ptr(), gblock);
        }
        let r = libc::malloc(size).cast::<BlkLayout>();
        if r.is_null() {
            blk_stop(c"no memory to copy guest block %#llx".as_ptr(), gblock);
        }
        ptr::copy_nonoverlapping(b.cast::<u8>(), r.cast::<u8>(), size);
        (*r).flags.store(
            (flags & !(BLK_REFCOUNT_MASK | BLK_DEALLOCATING)) | BLK_NEEDS_FREE | 2,
            Ordering::SeqCst,
        );
        if flags & BLK_HAS_COPY_DISPOSE != 0 {
            let shadow = blk_shadow(gdesc, flags);
            if shadow.is_null() {
                blk_stop(c"guest block %#llx has helpers that could not be bound, so it cannot be copied".as_ptr(), gblock);
            }
            (*r).desc = shadow.cast();
            let f: CopyFn = core::mem::transmute(*shadow.add(2));
            f(r.cast(), b.cast());
        }
        (*r).isa = concrete_malloc();
        blk_guest(r.cast())
    }
}

unsafe fn blk_byref_copy(gsrc: u64, flags: i32) -> u64 {
    let src = ocerz_g2h(gsrc).cast::<BlkByref>();
    let mut fwd: *mut BlkByref = if !src.is_null() && !(*src).forwarding.is_null() {
        ocerz_g2h((*src).forwarding as u64).cast()
    } else {
        ptr::null_mut()
    };
    let sflags = if src.is_null() {
        0
    } else {
        (*src).flags.load(Ordering::SeqCst)
    };
    if !fwd.is_null() && fwd != (*src).forwarding {
        (*src).forwarding = fwd;
    }
    if !fwd.is_null()
        && (*fwd).flags.load(Ordering::SeqCst) & BLK_REFCOUNT_MASK == 0
        && sflags & BLK_BYREF_HAS_COPY_DISPOSE != 0
        && (ffi::ocerz_abi_is_guest_code((*src).keep) != 0
            || ffi::ocerz_abi_is_guest_code((*src).destroy) != 0)
    {
        let size = (*src).size as usize;
        if size < offset_of!(BlkByref, layout) || size > BLK_MAX_SIZE {
            blk_stop(
                c"guest __block variable %#llx has a size ocerz cannot copy".as_ptr(),
                gsrc,
            );
        }
        let copy = libc::calloc(1, size).cast::<BlkByref>();
        if copy.is_null() {
            blk_stop(
                c"no memory to copy guest __block variable %#llx".as_ptr(),
                gsrc,
            );
        }
        let mut keep = 0;
        let mut destroy = 0;
        if !blk_helper_slot((*src).keep, c"v(pp)".as_ptr(), &mut keep)
            || !blk_helper_slot((*src).destroy, c"v(p)".as_ptr(), &mut destroy)
        {
            blk_stop(
                c"guest __block variable %#llx has helpers that could not be bound".as_ptr(),
                gsrc,
            );
        }
        (*copy).flags = AtomicI32::new(sflags | BLK_BYREF_NEEDS_FREE | 4);
        (*copy).forwarding = copy;
        (*src).forwarding = copy;
        (*copy).size = (*src).size;
        (*copy).keep = keep;
        (*copy).destroy = destroy;
        if sflags & BLK_BYREF_LAYOUT_EXTENDED != 0 && size >= size_of::<BlkByref>() {
            (*copy).layout = (*src).layout;
        }
        let f: unsafe extern "C" fn(*mut c_void, *mut c_void) = core::mem::transmute(keep);
        f(copy.cast(), src.cast());
        return blk_guest(copy.cast());
    }
    let mut dst: *mut c_void = ptr::null_mut();
    _Block_object_assign(
        (&mut dst as *mut *mut c_void).cast(),
        if src.is_null() {
            ptr::null()
        } else {
            src.cast()
        },
        flags,
    );
    blk_guest(dst)
}

unsafe fn blk_return(cpu: *mut ffi::OcerzCPU, rax: u64) {
    let rsp = *(*cpu).gpr.as_ptr().add(ffi::OCERZ_RSP as usize);
    (*cpu).rip = ocerz_ld(rsp, 8);
    *(*cpu).gpr.as_mut_ptr().add(ffi::OCERZ_RSP as usize) = rsp.wrapping_add(8);
    *(*cpu).gpr.as_mut_ptr().add(ffi::OCERZ_RAX as usize) = rax;
}

unsafe fn blk_settle(vm: *mut ffi::OcerzVM, cpu: *mut ffi::OcerzCPU) -> c_int {
    if ffi::ocerz_peek_pending_async_sig() != 0 || ((*cpu).sig_pending & !(*cpu).sig_mask) != 0 {
        let _ = ffi::ocerz_guest_deliver_pending(vm, cpu);
    }
    ffi::OCERZ_STEP_OK as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_block_special_copy(
    vm: *mut ffi::OcerzVM,
    cpu: *mut ffi::OcerzCPU,
) -> c_int {
    unsafe {
        blk_return(
            cpu,
            ocerz_block_copy_guest(*(*cpu).gpr.as_ptr().add(ffi::OCERZ_RDI as usize)),
        );
        blk_settle(vm, cpu)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_block_special_object_assign(
    vm: *mut ffi::OcerzVM,
    cpu: *mut ffi::OcerzCPU,
) -> c_int {
    unsafe {
        let dst = *(*cpu).gpr.as_ptr().add(ffi::OCERZ_RDI as usize);
        let src = *(*cpu).gpr.as_ptr().add(ffi::OCERZ_RSI as usize);
        let flags = *(*cpu).gpr.as_ptr().add(ffi::OCERZ_RDX as usize) as i32;
        match flags & BLK_ALL_COPY_DISPOSE_FLAGS {
            BLK_FIELD_IS_BLOCK => ocerz_st(dst, 8, ocerz_block_copy_guest(src)),
            x if x == BLK_FIELD_IS_BYREF || x == (BLK_FIELD_IS_BYREF | BLK_FIELD_IS_WEAK) => {
                ocerz_st(dst, 8, blk_byref_copy(src, flags))
            }
            _ => _Block_object_assign(
                ocerz_g2h(dst),
                if src == 0 {
                    ptr::null()
                } else {
                    ocerz_g2h(src)
                },
                flags,
            ),
        }
        blk_return(cpu, 0);
        blk_settle(vm, cpu)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_block_invoke_trap(
    vm: *mut ffi::OcerzVM,
    cpu: *mut ffi::OcerzCPU,
) -> c_int {
    unsafe {
        let mut view = *(*cpu).gpr.as_ptr().add(ffi::OCERZ_RDI as usize);
        if ocerz_block_guest_view(view, ptr::null_mut()) == 0 {
            view = *(*cpu).gpr.as_ptr().add(ffi::OCERZ_RSI as usize);
            if ocerz_block_guest_view(view, ptr::null_mut()) == 0 {
                blk_stop(
                    c"the trampoline for native blocks was entered with no native block's guest view in rdi or rsi (rdi %#llx)".as_ptr(),
                    *(*cpu).gpr.as_ptr().add(ffi::OCERZ_RDI as usize),
                );
            }
        }
        let v = blk_host(view);
        let d = (*v).desc.cast::<BlkDesc>();
        let n = (*v).inner as *const BlkLayout;
        if !(*d).dead.is_null() {
            blk_dead_call(c"a native block handed to guest code".as_ptr(), d);
        }
        let mut outer = core::mem::MaybeUninit::<ffi::OcerzBridgeFrame>::uninit();
        ffi::ocerz_bridge_raise(
            outer.as_mut_ptr(),
            ffi::OCERZ_BRIDGE_LIBSYSTEM.as_ptr().cast(),
            c"(native block)".as_ptr(),
            (*d).notation.as_ptr(),
            (*n).invoke,
        );
        let r = ffi::ocerz_abi_perform_borrowed(&(*d).sig, (*n).invoke, cpu);
        ffi::ocerz_bridge_lower(outer.as_ptr());
        if r != ffi::OCERZ_STEP_OK as c_int {
            return r;
        }
        blk_settle(vm, cpu)
    }
}

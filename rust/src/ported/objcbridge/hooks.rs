//! ---- runtime helpers and hooks ----
//! objc_opt_self/objc_opt_class/objc_alloc/objc_alloc_init/objc_allocWithZone
//! are one-argument object calls.  objc_release keeps the guest's Swift release
//! path.  The three libobjc hooks (getClass, getImageName, lazyClassNamer) each
//! chain a guest callback ahead of the previously installed one.
//! ocerz_objcbridge_fix_selrefs rewrites a mapped image's __objc_selrefs and
//! __objc_msgrefs entries to the canonical selectors sel_registerName returns,
//! so is() comparisons the guest does against its own pointers hold.

use core::ffi::{c_char, c_int, c_void};
use core::ptr::null_mut;
use core::sync::atomic::{AtomicPtr, AtomicU64, Ordering};

use crate::ffi::*;
use crate::ported::objcbridge::common::*;
use crate::ported::objcbridge::send::G_OB_GENERATION;

#[repr(C)]
struct MachHeader64 {
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
struct LoadCommand {
    cmd: u32,
    cmdsize: u32,
}
#[repr(C)]
struct SegmentCommand64 {
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
struct Section64 {
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

const MH_MAGIC_64: u32 = 0xfeedfacf;
const LC_SEGMENT_64: u32 = 0x19;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_realizeClassFromSwift(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let cls = gpr(cpu, OCERZ_RDI);
        let previously = gpr(cpu, OCERZ_RSI);
        let f = ob_need(&raw const G_OB_REALIZE_CLASS_FROM_SWIFT);
        let known = if cls != 0 { ocerz_objcbridge_prepare_class(cls) } else { 1 };
        let mut outer: OcerzBridgeFrame = core::mem::MaybeUninit::uninit().assume_init();
        ocerz_bridge_raise(&mut outer, OCERZ_OBJC_LIBOBJC.as_ptr() as *const c_char, c"__objc_realizeClassFromSwift".as_ptr(), c"p(pp)".as_ptr(), f);
        let f: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void = core::mem::transmute(f);
        let got = f(
            if cls != 0 { ocerz_g2h(cls) } else { null_mut() },
            if known != 0 && previously != 0 { ocerz_g2h(previously) } else { null_mut() },
        );
        G_OB_GENERATION.fetch_add(1, Ordering::SeqCst);
        ocerz_bridge_lower(&outer);
        ob_return(cpu, if got.is_null() { 0 } else { ocerz_h2g(got) });
        ob_settle(vm, cpu)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_readClassPair(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let cls = gpr(cpu, OCERZ_RDI);
        let info = gpr(cpu, OCERZ_RSI);
        let f = ob_need(&raw const G_OB_READ_CLASS_PAIR);
        if cls != 0 {
            ocerz_objcbridge_prepare_class(cls);
        }
        let mut outer: OcerzBridgeFrame = core::mem::MaybeUninit::uninit().assume_init();
        ocerz_bridge_raise(&mut outer, OCERZ_OBJC_LIBOBJC.as_ptr() as *const c_char, c"_objc_readClassPair".as_ptr(), c"p(pp)".as_ptr(), f);
        let f: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void = core::mem::transmute(f);
        let got = f(
            if cls != 0 { ocerz_g2h(cls) } else { null_mut() },
            if info != 0 { ocerz_g2h(info) } else { null_mut() },
        );
        G_OB_GENERATION.fetch_add(1, Ordering::SeqCst);
        ocerz_bridge_lower(&outer);
        ob_return(cpu, if got.is_null() { 0 } else { ocerz_h2g(got) });
        ob_settle(vm, cpu)
    }
}

static mut G_OB_NO_ONCE: libc::pthread_once_t = libc::PTHREAD_ONCE_INIT;
static mut G_OB_NO: u64 = 0;

unsafe extern "C" fn ob_make_no() {
    unsafe {
        let page = ocerz_map_anywhere(OCERZ_GUEST_PAGE_SIZE as u64, libc::PROT_READ | libc::PROT_WRITE);
        if page == 0 {
            return;
        }
        let buf = ocerz_g2h(page) as *mut u8;
        core::ptr::write_bytes(buf, 0xcc, OCERZ_GUEST_PAGE_SIZE as usize);
        // xor eax,eax; ret
        static NO: [u8; 3] = [0x31, 0xc0, 0xc3];
        core::ptr::copy_nonoverlapping(NO.as_ptr(), buf, 3);
        if ocerz_protect(page, OCERZ_GUEST_PAGE_SIZE as u64, libc::PROT_READ | libc::PROT_EXEC) == OCERZ_OK as c_int {
            G_OB_NO = page;
        }
    }
}

unsafe fn ob_guest_no() -> u64 {
    unsafe {
        libc::pthread_once(&raw mut G_OB_NO_ONCE, Some(ob_make_no));
        G_OB_NO
    }
}

#[repr(C)]
struct ObHook {
    setter: *const ObSym,
    export: *const c_char,
    notation: *const c_char,
    guest: AtomicPtr<c_void>,
    guest_fn: u64,
    prev: *mut c_void,
    installed: c_int,
}
unsafe impl Sync for ObHook {}

macro_rules! obhook {
    ($setter:expr, $export:literal, $notation:literal) => {
        ObHook {
            setter: $setter,
            export: concat!($export, "\0").as_ptr() as *const c_char,
            notation: concat!($notation, "\0").as_ptr() as *const c_char,
            guest: AtomicPtr::new(null_mut()),
            guest_fn: 0,
            prev: null_mut(),
            installed: 0,
        }
    };
}

static mut G_OB_HOOK_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
static mut G_OB_GETCLASS: ObHook = obhook!(&raw const G_OB_SET_HOOK_GET_CLASS, "_objc_setHook_getClass", "b(pp)");
static mut G_OB_IMAGENAME: ObHook = obhook!(&raw const G_OB_SET_HOOK_GET_IMAGE_NAME, "_objc_setHook_getImageName", "b(pp)");
static mut G_OB_NAMER: ObHook = obhook!(&raw const G_OB_SET_HOOK_LAZY_CLASS_NAMER, "_objc_setHook_lazyClassNamer", "p(p)");

unsafe fn ob_hook2(h: *mut ObHook, a: *const c_void, b: *mut c_void) -> bool {
    unsafe {
        let guest: Option<unsafe extern "C" fn(*const c_void, *mut c_void) -> bool> =
            core::mem::transmute((*h).guest.load(Ordering::SeqCst));
        if let Some(g) = guest {
            if g(a, b) {
                return true;
            }
        }
        if (*h).prev.is_null() {
            false
        } else {
            let prev: unsafe extern "C" fn(*const c_void, *mut c_void) -> bool = core::mem::transmute((*h).prev);
            prev(a, b)
        }
    }
}

unsafe extern "C" fn ob_getclass_hook(name: *const c_void, out: *mut c_void) -> bool {
    unsafe { ob_hook2(&raw mut G_OB_GETCLASS, name, out) }
}
unsafe extern "C" fn ob_imagename_hook(cls: *const c_void, out: *mut c_void) -> bool {
    unsafe { ob_hook2(&raw mut G_OB_IMAGENAME, cls, out) }
}
unsafe extern "C" fn ob_namer_hook(cls: *mut c_void) -> *const c_char {
    unsafe {
        let guest: Option<unsafe extern "C" fn(*mut c_void) -> *const c_char> =
            core::mem::transmute((*(&raw const G_OB_NAMER)).guest.load(Ordering::SeqCst));
        let mut name = match guest {
            Some(g) => g(cls),
            None => null_mut() as *const c_char,
        };
        if name.is_null() && !G_OB_NAMER.prev.is_null() {
            let prev: unsafe extern "C" fn(*mut c_void) -> *const c_char = core::mem::transmute(G_OB_NAMER.prev);
            name = prev(cls);
        }
        name
    }
}

unsafe fn ob_set_hook(vm: *mut OcerzVM, cpu: *mut OcerzCPU, h: *mut ObHook, native_hook: *mut c_void) -> c_int {
    unsafe {
        let guest = gpr(cpu, OCERZ_RDI);
        let out_old = gpr(cpu, OCERZ_RSI);
        let set = ob_need((*h).setter);
        let mut slot = 0u64;
        if guest != 0
            && (ocerz_abi_callback_convert(guest, (*h).notation, &mut slot) != OCERZ_OK as c_int || slot == 0)
        {
            ob_stop!("%s could not bind guest hook %#llx to a callback", (*h).export, guest);
        }
        libc::pthread_mutex_lock(&raw mut G_OB_HOOK_LOCK);
        let chain = if (*h).guest_fn != 0 { (*h).guest_fn } else { ob_guest_no() };
        if (*h).installed == 0 {
            let mut outer: OcerzBridgeFrame = core::mem::MaybeUninit::uninit().assume_init();
            ocerz_bridge_raise(&mut outer, OCERZ_OBJC_LIBOBJC.as_ptr() as *const c_char, (*h).export, core::ptr::null(), set);
            let setf: unsafe extern "C" fn(*mut c_void, *mut *mut c_void) = core::mem::transmute(set);
            setf(native_hook, &mut (*h).prev);
            ocerz_bridge_lower(&outer);
            (*h).installed = 1;
        }
        (*h).guest.store(if slot != 0 { ocerz_g2h(slot) } else { null_mut() }, Ordering::SeqCst);
        (*h).guest_fn = guest;
        libc::pthread_mutex_unlock(&raw mut G_OB_HOOK_LOCK);
        if out_old != 0 {
            ocerz_st(out_old, 8, chain);
        }
        ob_return(cpu, 0);
        ob_settle(vm, cpu)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_setHook_getClass(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_set_hook(vm, cpu, &raw mut G_OB_GETCLASS, ob_getclass_hook as *mut c_void) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_setHook_getImageName(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_set_hook(vm, cpu, &raw mut G_OB_IMAGENAME, ob_imagename_hook as *mut c_void) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_setHook_lazyClassNamer(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_set_hook(vm, cpu, &raw mut G_OB_NAMER, ob_namer_hook as *mut c_void) }
}

unsafe fn ob_object_call(vm: *mut OcerzVM, cpu: *mut OcerzCPU, s: *const ObSym, export: *const c_char) -> c_int {
    unsafe {
        let a = gpr(cpu, OCERZ_RDI);
        let f = ob_need(s);
        if a != 0 {
            ocerz_objcbridge_ensure_object(ocerz_g2h(a));
        }
        let mut outer: OcerzBridgeFrame = core::mem::MaybeUninit::uninit().assume_init();
        ocerz_bridge_raise(&mut outer, OCERZ_OBJC_LIBOBJC.as_ptr() as *const c_char, export, c"p(p)".as_ptr(), f);
        let f: unsafe extern "C" fn(*mut c_void) -> *mut c_void = core::mem::transmute(f);
        let r = f(if a != 0 { ocerz_g2h(a) } else { null_mut() });
        ocerz_bridge_lower(&outer);
        ob_return(cpu, if r.is_null() { 0 } else { ocerz_h2g(r) });
        ob_settle(vm, cpu)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_opt_self(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_object_call(vm, cpu, &raw const G_OB_OPT_SELF, c"_objc_opt_self".as_ptr()) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_opt_class(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_object_call(vm, cpu, &raw const G_OB_OPT_CLASS, c"_objc_opt_class".as_ptr()) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_alloc(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_object_call(vm, cpu, &raw const G_OB_ALLOC, c"_objc_alloc".as_ptr()) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_alloc_init(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_object_call(vm, cpu, &raw const G_OB_ALLOC_INIT, c"_objc_alloc_init".as_ptr()) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_allocWithZone(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { ob_object_call(vm, cpu, &raw const G_OB_ALLOC_WITH_ZONE, c"_objc_allocWithZone".as_ptr()) }
}

unsafe fn ob_guest_swift_release() -> u64 {
    unsafe {
        static AT: AtomicU64 = AtomicU64::new(0);
        let mut a = AT.load(Ordering::Acquire);
        if a == 0 {
            a = ocerz_dyld_native_image_export(
                c"/usr/lib/swift/libswiftCore.dylib".as_ptr(),
                c"_swift_release".as_ptr(),
            );
            if a != 0 && ocerz_abi_is_guest_code(a) != 0 {
                AT.store(a, Ordering::Release);
            } else {
                a = 0;
            }
        }
        a
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objc_release(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let a = gpr(cpu, OCERZ_RDI);
        if a != 0 && ocerz_objcbridge_guest_swift_object(ocerz_g2h(a)) != 0 {
            let to = ob_guest_swift_release();
            if to != 0 {
                (*cpu).rip = to;
                return OCERZ_STEP_OK as c_int;
            }
        }
        let f = ob_need(&raw const G_OB_RELEASE);
        let mut outer: OcerzBridgeFrame = core::mem::MaybeUninit::uninit().assume_init();
        ocerz_bridge_raise(&mut outer, OCERZ_OBJC_LIBOBJC.as_ptr() as *const c_char, c"_objc_release".as_ptr(), c"v(p)".as_ptr(), f);
        let f: unsafe extern "C" fn(*mut c_void) = core::mem::transmute(f);
        f(if a != 0 { ocerz_g2h(a) } else { null_mut() });
        ocerz_bridge_lower(&outer);
        ob_return(cpu, 0);
        ob_settle(vm, cpu)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_objcbridge_fix_selrefs(mh: *const u8, slide: i64) -> c_int {
    unsafe {
        let mut h: MachHeader64 = core::mem::zeroed();
        if mh.is_null() {
            return 0;
        }
        libc::memcpy(&mut h as *mut _ as *mut c_void, mh as *const c_void, size_of::<MachHeader64>());
        if h.magic != MH_MAGIC_64 {
            return 0;
        }
        let mut rewritten = 0;
        let mut lc = mh.add(size_of::<MachHeader64>());
        for _i in 0..h.ncmds {
            let mut l: LoadCommand = core::mem::zeroed();
            libc::memcpy(&mut l as *mut _ as *mut c_void, lc as *const c_void, size_of::<LoadCommand>());
            if l.cmdsize < size_of::<LoadCommand>() as u32 {
                break;
            }
            if l.cmd == LC_SEGMENT_64 {
                let mut seg: SegmentCommand64 = core::mem::zeroed();
                libc::memcpy(&mut seg as *mut _ as *mut c_void, lc as *const c_void, size_of::<SegmentCommand64>());
                let mut s = 0;
                while libc::strncmp(seg.segname.as_ptr(), c"__DATA".as_ptr(), 6) == 0 && s < seg.nsects {
                    let mut sc: Section64 = core::mem::zeroed();
                    libc::memcpy(
                        &mut sc as *mut _ as *mut c_void,
                        lc.add(size_of::<SegmentCommand64>()).add(s as usize * size_of::<Section64>()) as *const c_void,
                        size_of::<Section64>(),
                    );
                    let stride: u64;
                    let at: u64;
                    if libc::strncmp(sc.sectname.as_ptr(), c"__objc_selrefs".as_ptr(), sc.sectname.len()) == 0 {
                        stride = 8;
                        at = 0;
                    } else if libc::strncmp(sc.sectname.as_ptr(), c"__objc_msgrefs".as_ptr(), sc.sectname.len()) == 0 {
                        stride = 16;
                        at = 8;
                    } else {
                        s += 1;
                        continue;
                    }
                    let base = (sc.addr as i64 + slide) as u64;
                    let mut off = 0u64;
                    while off + stride <= sc.size {
                        let word = base + off + at;
                        let name = ocerz_ld(word, 8);
                        if name != 0 {
                            let reg = ob_sym(&raw const G_OB_SEL_REGISTER_NAME);
                            if reg.is_null() {
                                crate::ocerz_log!(
                                    "objc: the host libobjc has no sel_registerName, so the selectors of the image at %p stay the image's own\n",
                                    mh as *const c_void
                                );
                                return -1;
                            }
                            let reg: unsafe extern "C" fn(*const c_char) -> *mut c_void = core::mem::transmute(reg);
                            let sel = reg(ocerz_g2h(name) as *const c_char);
                            let canon = ocerz_h2g(sel);
                            if canon != name {
                                ocerz_st(word, 8, canon);
                                rewritten += 1;
                            }
                        }
                        off += stride;
                    }
                    s += 1;
                }
            }
            lc = lc.add(l.cmdsize as usize);
        }
        rewritten
    }
}

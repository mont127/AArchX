//! Native dyld exports, symbol lookup, dladdr, and native image loading.

use super::*;
use core::ffi::CStr;
use core::sync::atomic::Ordering;

const NDL_RTLD_LOCAL: c_int = 0x4;
const NDL_RTLD_NOLOAD: c_int = 0x10;
const NDL_RTLD_FIRST: c_int = 0x100;
const NDL_NEXT: u64 = u64::MAX;
const NDL_DEFAULT: u64 = u64::MAX - 1;
const NDL_SELF: u64 = u64::MAX - 2;
const NDL_MAIN_ONLY: u64 = u64::MAX - 4;
const NDL_ERR_BYTES: usize = 2048;
const NDL_TRIED_BYTES: usize = 1536;
const PLATFORM_MACOS: u32 = 1;
const VM_PROT_WRITE: u32 = 0x2;

pub(super) static mut g_native_init_args: [u64; 5] = [0; 5];

unsafe fn host_in_guest_reservation(haddr: *const c_void) -> bool {
    let h = haddr as u64;
    if ffi::ocerz_low_base != 0 {
        if h.wrapping_sub(ffi::ocerz_low_base) < ffi::OCERZ_LOW_LIMIT {
            return true;
        }
        if h.wrapping_sub(ffi::ocerz_top_base) < ffi::OCERZ_TOP_HI - ffi::OCERZ_TOP_LO {
            return true;
        }
    }
    let g = h.wrapping_sub(ffi::ocerz_guest_base);
    g >= ffi::ocerz_arena_lo && g < ffi::ocerz_arena_hi
}

#[repr(C)]
struct NdlErr {
    buf: u64,
    pending: c_int,
}

static mut G_NDL_ERR_KEY: libc::pthread_key_t = 0;
static mut G_NDL_ERR_ONCE: libc::pthread_once_t = libc::PTHREAD_ONCE_INIT;

unsafe extern "C" fn ndl_err_release(p: *mut c_void) {
    let e = p.cast::<NdlErr>();
    if e.is_null() {
        return;
    }
    if (*e).buf != 0 {
        ffi::ocerz_unmap((*e).buf, NDL_ERR_BYTES as u64);
    }
    libc::free(e.cast());
}

unsafe extern "C" fn ndl_err_init() {
    libc::pthread_key_create(ptr::addr_of_mut!(G_NDL_ERR_KEY), Some(ndl_err_release));
}

unsafe fn ndl_err_self(make: c_int) -> *mut NdlErr {
    libc::pthread_once(ptr::addr_of_mut!(G_NDL_ERR_ONCE), Some(ndl_err_init));
    let mut e = libc::pthread_getspecific(G_NDL_ERR_KEY).cast::<NdlErr>();
    if !e.is_null() || make == 0 {
        return e;
    }
    e = libc::calloc(1, core::mem::size_of::<NdlErr>()).cast();
    if e.is_null() {
        return ptr::null_mut();
    }
    (*e).buf = ffi::ocerz_map_anywhere(NDL_ERR_BYTES as u64, libc::PROT_READ | libc::PROT_WRITE);
    if (*e).buf == 0 || libc::pthread_setspecific(G_NDL_ERR_KEY, e.cast()) != 0 {
        if (*e).buf != 0 {
            ffi::ocerz_unmap((*e).buf, NDL_ERR_BYTES as u64);
        }
        libc::free(e.cast());
        return ptr::null_mut();
    }
    e
}

unsafe fn ndl_err_clear() {
    let e = ndl_err_self(0);
    if !e.is_null() {
        (*e).pending = 0;
    }
}

macro_rules! ndl_err {
    ($fmt:literal $(, $arg:expr)* $(,)?) => {{
        let mut msg = [0 as c_char; NDL_ERR_BYTES];
        let fmt = CStr::from_bytes_with_nul_unchecked(concat!($fmt, "\0").as_bytes());
        libc::snprintf(msg.as_mut_ptr(), msg.len(), fmt.as_ptr(), $($arg),*);
        if !libc::getenv(cstr_ptr(c"OCERZ_DLPATH")).is_null() {
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(c"ocerz: DLERR %s\n"),
                msg.as_ptr(),
            );
        }
        let e = ndl_err_self(1);
        if !e.is_null() {
            let host = crate::ported::dyldapi::hostmem::ocerz_g2h((*e).buf).cast::<u8>();
            ptr::copy_nonoverlapping(msg.as_ptr().cast::<u8>(), host, libc::strlen(msg.as_ptr()) + 1);
            (*e).pending = 1;
        }
    }};
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_native_dlerror() -> u64 {
    let e = ndl_err_self(0);
    if e.is_null() || (*e).pending == 0 {
        return 0;
    }
    (*e).pending = 0;
    (*e).buf
}

#[repr(C)]
pub(super) struct NdlImage {
    pub(super) mh: u64,
    pub(super) slide: i64,
    pub(super) name: u64,
    pub(super) d: *mut DynImage,
}

unsafe fn ndl_image(index: u32, out: *mut NdlImage) -> c_int {
    ptr::write_bytes(out, 0, 1);
    if ocerz_main_mh == 0 {
        return 0;
    }
    if index == 0 {
        (*out).mh = ocerz_main_mh;
        (*out).slide = super::eager::image_slide_d(ocerz_main_mh);
        (*out).name = if crate::ported::dyldapi::g_main_path != 0 {
            crate::ported::dyldapi::g_main_path
        } else {
            crate::ported::dyldapi::hostmem::ocerz_h2g(
                ptr::addr_of!(super::g_main_hostpath).cast::<c_void>(),
            )
        };
        (*out).d = if super::g_main_dimg_valid != 0 {
            ptr::addr_of_mut!(super::g_main_dimg)
        } else {
            ptr::null_mut()
        };
        return 1;
    }
    let published = ndl_pub();
    if index - 1 >= published {
        return 0;
    }
    let d = ptr::addr_of_mut!(super::g_dimgs)
        .cast::<DynImage>()
        .add((index - 1) as usize);
    (*out).mh = (*d).load_base;
    (*out).slide = (*d).slide as i64;
    (*out).name = crate::ported::dyldapi::hostmem::ocerz_h2g((*d).path.as_ptr().cast());
    (*out).d = d;
    1
}

unsafe fn ndl_covers(mh: u64, addr: u64, len: u64, readonly: *mut c_int) -> c_int {
    let h = crate::ported::dyldapi::hostmem::ocerz_g2h(mh).cast::<u8>();
    if mh == 0 || rd32(h) != MH_MAGIC_64 {
        return 0;
    }
    let slide = super::eager::image_slide_d(mh);
    let ncmds = rd32(h.add(16));
    let mut lc = h.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        if rd32(lc) == LC_SEGMENT_64 {
            let vmaddr = rd64(lc.add(24));
            let vmsize = rd64(lc.add(32));
            let initprot = rd32(lc.add(60));
            let lo = (vmaddr as i64).wrapping_add(slide) as u64;
            if vmsize != 0
                && !(vmaddr == 0 && initprot == 0)
                && addr >= lo
                && addr.wrapping_sub(lo) < vmsize
            {
                if !readonly.is_null() {
                    *readonly = ((initprot & VM_PROT_WRITE == 0)
                        && len <= vmsize - addr.wrapping_sub(lo))
                        as c_int;
                }
                return 1;
            }
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
    0
}

pub(super) unsafe fn ndl_containing(addr: u64, out: *mut NdlImage, index_out: *mut u32) -> c_int {
    let n = ocerz_dyld_image_count();
    for i in 0..n {
        if ndl_image(i, out) != 0 && ndl_covers((*out).mh, addr, 1, ptr::null_mut()) != 0 {
            if !index_out.is_null() {
                *index_out = i;
            }
            return 1;
        }
    }
    ptr::write_bytes(out, 0, 1);
    0
}

pub(super) unsafe fn ndl_pub() -> u32 {
    super::g_dimgs_pub.load(Ordering::Acquire) as u32
}

pub(super) unsafe fn native_publish() {
    super::g_dimgs_pub.store(super::g_dimgs_n, Ordering::Release);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_image_count() -> u32 {
    if ocerz_main_mh != 0 {
        1u32.wrapping_add(ndl_pub())
    } else {
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_image_at(
    index: u32,
    mh: *mut u64,
    slide: *mut u64,
    name: *mut u64,
) -> c_int {
    let mut im = NdlImage {
        mh: 0,
        slide: 0,
        name: 0,
        d: ptr::null_mut(),
    };
    let ok = ndl_image(index, &mut im);
    if !mh.is_null() {
        *mh = im.mh;
    }
    if !slide.is_null() {
        *slide = im.slide as u64;
    }
    if !name.is_null() {
        *name = im.name;
    }
    ok
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_unwind_sections(addr: u64, sections: u64) -> c_int {
    if sections == 0 {
        return 0;
    }
    let mut result = [0u64; 5];
    let mut im = NdlImage {
        mh: 0,
        slide: 0,
        name: 0,
        d: ptr::null_mut(),
    };
    if ndl_containing(addr, &mut im, ptr::null_mut()) == 0
        || (!im.d.is_null() && (*im.d).is_virtual != 0)
    {
        ptr::copy_nonoverlapping(
            result.as_ptr(),
            crate::ported::dyldapi::hostmem::ocerz_g2h(sections).cast::<u64>(),
            result.len(),
        );
        return 0;
    }
    result[0] = im.mh;
    let h = crate::ported::dyldapi::hostmem::ocerz_g2h(im.mh).cast::<u8>();
    let mut lc = h.add(core::mem::size_of::<MachHeader64>());
    let end = lc.add(rd32(h.add(20)) as usize);
    let ncmds = rd32(h.add(16));
    for _ in 0..ncmds {
        let remaining = end.offset_from(lc);
        if remaining < core::mem::size_of::<LoadCommand>() as isize {
            break;
        }
        let cmd = rd32(lc);
        let cmdsize = rd32(lc.add(4));
        if cmdsize < core::mem::size_of::<LoadCommand>() as u32 || cmdsize as isize > remaining {
            break;
        }
        if cmd == LC_SEGMENT_64 && cmdsize >= core::mem::size_of::<SegmentCommand64>() as u32 {
            let nsects = rd32(lc.add(64));
            let count = (cmdsize as usize - core::mem::size_of::<SegmentCommand64>()) / 80;
            let sec = lc.add(core::mem::size_of::<SegmentCommand64>());
            for j in 0..nsects.min(count as u32) {
                let section = sec.add(j as usize * 80);
                if libc::strncmp(section.cast::<c_char>(), cstr_ptr(c"__TEXT"), 16) != 0 {
                    continue;
                }
                let sectname = section.add(16).cast::<c_char>();
                let slot = if libc::strncmp(sectname, cstr_ptr(c"__eh_frame"), 16) == 0 {
                    1
                } else if libc::strncmp(sectname, cstr_ptr(c"__unwind_info"), 16) == 0 {
                    3
                } else {
                    0
                };
                let size = rd64(section.add(40));
                if slot != 0 && size != 0 {
                    result[slot] = rd64(section.add(32)).wrapping_add(im.slide as u64);
                    result[slot + 1] = size;
                }
            }
        }
        lc = lc.add(cmdsize as usize);
    }
    ptr::copy_nonoverlapping(
        result.as_ptr(),
        crate::ported::dyldapi::hostmem::ocerz_g2h(sections).cast::<u64>(),
        result.len(),
    );
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_image_containing(
    addr: u64,
    mh: *mut u64,
    name: *mut u64,
) -> c_int {
    let mut im = NdlImage {
        mh: 0,
        slide: 0,
        name: 0,
        d: ptr::null_mut(),
    };
    let ok = ndl_containing(addr, &mut im, ptr::null_mut());
    if !mh.is_null() {
        *mh = im.mh;
    }
    if !name.is_null() {
        *name = im.name;
    }
    ok
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_image_slide(mh: u64, slide: *mut u64) -> c_int {
    let n = ocerz_dyld_image_count();
    let mut im = NdlImage {
        mh: 0,
        slide: 0,
        name: 0,
        d: ptr::null_mut(),
    };
    for i in 0..n {
        if ndl_image(i, &mut im) != 0 && im.mh == mh {
            *slide = im.slide as u64;
            return 1;
        }
    }
    *slide = 0;
    0
}

unsafe fn ndl_dimg_for_mh(mh: u64) -> *mut DynImage {
    if mh == 0 {
        return ptr::null_mut();
    }
    if mh == ocerz_main_mh {
        return if super::g_main_dimg_valid != 0 {
            ptr::addr_of_mut!(super::g_main_dimg)
        } else {
            ptr::null_mut()
        };
    }
    let published = ndl_pub() as c_int;
    for i in 0..published {
        let d = ptr::addr_of_mut!(super::g_dimgs)
            .cast::<DynImage>()
            .add(i as usize);
        if (*d).load_base == mh {
            return d;
        }
    }
    ptr::null_mut()
}

unsafe fn ndl_stub_only(d: *const DynImage, usym: *const c_char) -> bool {
    if (*d).is_virtual == 0 {
        return false;
    }
    let lib = ffi::ocerz_apidb_library((*d).install_name.as_ptr());
    let entry = if lib.is_null() {
        ptr::null()
    } else {
        ffi::ocerz_apidb_find(lib, usym)
    };
    !entry.is_null() && (*entry).kind == ffi::OCERZ_API_STUB
}

unsafe fn ndl_lookup_in(d: *mut DynImage, usym: *const c_char, found: *mut c_int) -> u64 {
    let mut value = super::exports::ocerz_image_self_resolve_ex(d, usym, found);
    if *found != 0 && ndl_stub_only(d, usym) {
        *found = 0;
        return 0;
    }
    if *found != 0 {
        return value;
    }
    value = super::dlopen::image_symtab_resolve(d, usym);
    *found = (value != 0) as c_int;
    value
}

unsafe fn ndl_dep_of(img: *mut DynImage, name: *const c_char, published: c_int) -> *mut DynImage {
    let mut expanded = [0 as c_char; 1024];
    let alt = if name.read() == b'@' as c_char
        && super::load::expand_at_prefix(img, name, expanded.as_mut_ptr(), expanded.len()) != 0
    {
        expanded.as_ptr()
    } else {
        ptr::null()
    };
    for i in 0..published {
        let d = ptr::addr_of_mut!(super::g_dimgs)
            .cast::<DynImage>()
            .add(i as usize);
        if libc::strcmp((*d).install_name.as_ptr(), name) == 0
            || libc::strcmp((*d).id_name.as_ptr(), name) == 0
            || libc::strcmp((*d).path.as_ptr(), name) == 0
        {
            return d;
        }
        if !alt.is_null()
            && (libc::strcmp((*d).path.as_ptr(), alt) == 0
                || libc::strcmp((*d).install_name.as_ptr(), alt) == 0)
        {
            return d;
        }
    }
    ptr::null_mut()
}

unsafe fn ndl_search_deps(root: *mut DynImage, usym: *const c_char, found: *mut c_int) -> u64 {
    let published = ndl_pub() as c_int;
    let mut queue = [ptr::null_mut::<DynImage>(); DYN_DIMG_MAX + 1];
    let mut qn = 1;
    queue[0] = root;
    let mut qi = 0;
    while qi < qn {
        let d = queue[qi];
        qi += 1;
        let value = ndl_lookup_in(d, usym, found);
        if *found != 0 {
            return value;
        }
        let mh = (*d).slice;
        let ncmds = rd32(mh.add(16));
        let mut lc = mh.add(core::mem::size_of::<MachHeader64>());
        for _ in 0..ncmds {
            let cmd = rd32(lc);
            let noff = rd32(lc.add(8));
            if (cmd == 0xc || cmd == 0x8000_0018 || cmd == 0x8000_001f || cmd == 0x8000_0023)
                && noff < rd32(lc.add(4))
            {
                let dep = ndl_dep_of(d, lc.add(noff as usize).cast(), published);
                let mut seen = dep.is_null();
                for k in 0..qn {
                    if queue[k] == dep {
                        seen = true;
                        break;
                    }
                }
                if !seen && qn < queue.len() {
                    queue[qn] = dep;
                    qn += 1;
                }
            }
            lc = lc.add(rd32(lc.add(4)) as usize);
        }
    }
    *found = 0;
    0
}

unsafe fn ndl_search_from(start: u32, own: u32, usym: *const c_char, found: *mut c_int) -> u64 {
    let n = ocerz_dyld_image_count();
    let mut im = NdlImage {
        mh: 0,
        slide: 0,
        name: 0,
        d: ptr::null_mut(),
    };
    for i in start..n {
        if ndl_image(i, &mut im) == 0 || im.d.is_null() || ((*im.d).local != 0 && i != own) {
            continue;
        }
        let value = ndl_lookup_in(im.d, usym, found);
        if *found != 0 {
            return value;
        }
    }
    *found = 0;
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_native_image_export(
    install_name: *const c_char,
    usym: *const c_char,
) -> u64 {
    let d = super::dimg_find_by_install_name(install_name);
    let mut found = 0;
    let value = if d.is_null() {
        0
    } else {
        ndl_lookup_in(d, usym, &mut found)
    };
    if found != 0 { value } else { 0 }
}

unsafe fn ndl_handle_text(handle: u64, out: *mut c_char, n: usize) {
    let text = match handle {
        NDL_DEFAULT => cstr_ptr(c"RTLD_DEFAULT"),
        NDL_NEXT => cstr_ptr(c"RTLD_NEXT"),
        NDL_SELF => cstr_ptr(c"RTLD_SELF"),
        NDL_MAIN_ONLY => cstr_ptr(c"RTLD_MAIN_ONLY"),
        _ => {
            libc::snprintf(out, n, cstr_ptr(c"%#llx"), handle as c_ulonglong);
            return;
        }
    };
    libc::snprintf(out, n, cstr_ptr(c"%s"), text);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_native_dlsym(
    handle: u64,
    name: *const c_char,
    caller: u64,
) -> u64 {
    let mut htext = [0 as c_char; 32];
    let mut usym = [0 as c_char; 1024];
    let mut found = 0;
    ndl_err_clear();
    ndl_handle_text(handle, htext.as_mut_ptr(), htext.len());
    if name.is_null()
        || libc::snprintf(usym.as_mut_ptr(), usym.len(), cstr_ptr(c"_%s"), name)
            >= usym.len() as c_int
    {
        ndl_err!(
            "dlsym(%s, %s): symbol not found",
            htext.as_ptr(),
            if name.is_null() {
                cstr_ptr(c"(null)")
            } else {
                name
            }
        );
        return 0;
    }
    let value = if handle == NDL_DEFAULT {
        ndl_search_from(0, u32::MAX, usym.as_ptr(), &mut found)
    } else if handle == NDL_MAIN_ONLY {
        if super::g_main_dimg_valid != 0 {
            ndl_lookup_in(
                ptr::addr_of_mut!(super::g_main_dimg),
                usym.as_ptr(),
                &mut found,
            )
        } else {
            0
        }
    } else if handle == NDL_NEXT || handle == NDL_SELF {
        let mut im = NdlImage {
            mh: 0,
            slide: 0,
            name: 0,
            d: ptr::null_mut(),
        };
        let mut index = 0;
        let mut start = 0;
        let mut own = u32::MAX;
        if ndl_containing(caller, &mut im, &mut index) != 0 {
            start = if handle == NDL_NEXT { index + 1 } else { index };
            own = if handle == NDL_SELF { index } else { u32::MAX };
        }
        ndl_search_from(start, own, usym.as_ptr(), &mut found)
    } else {
        let d = ndl_dimg_for_mh(handle & !1);
        if d.is_null() {
            ndl_err!("dlsym(%s, %s): invalid handle", htext.as_ptr(), name);
            return 0;
        }
        if handle & 1 != 0 {
            ndl_lookup_in(d, usym.as_ptr(), &mut found)
        } else {
            ndl_search_deps(d, usym.as_ptr(), &mut found)
        }
    };
    if found == 0 {
        ndl_err!("dlsym(%s, %s): symbol not found", htext.as_ptr(), name);
        return 0;
    }
    value
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_native_dlclose(handle: u64) -> c_int {
    ndl_err_clear();
    if handle == NDL_DEFAULT || handle == NDL_MAIN_ONLY || !ndl_dimg_for_mh(handle & !1).is_null() {
        return 0;
    }
    ndl_err!("dlclose(%#llx): invalid handle", handle as c_ulonglong);
    -1
}

#[repr(C)]
struct NdlSym {
    addr: u64,
    name: *mut c_char,
}

#[repr(C)]
pub(super) struct NdlSyms {
    v: *mut NdlSym,
    n: c_int,
    cap: c_int,
    lo: u64,
    hi: u64,
}

static mut G_NDL_SYMS_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

unsafe extern "C" fn ndl_syms_add(
    ctx: *mut c_void,
    name: *const c_char,
    value: u64,
    flags: u64,
) -> c_int {
    let s = ctx.cast::<NdlSyms>();
    if flags & 0x08 != 0 || flags & 0x03 == 0x02 || value < (*s).lo || value >= (*s).hi {
        return 0;
    }
    if (*s).n == (*s).cap {
        let cap = if (*s).cap != 0 { (*s).cap * 2 } else { 256 };
        let grown = libc::realloc(
            (*s).v.cast(),
            (cap as usize).wrapping_mul(core::mem::size_of::<NdlSym>()),
        )
        .cast::<NdlSym>();
        if grown.is_null() {
            return 1;
        }
        (*s).v = grown;
        (*s).cap = cap;
    }
    let copy = libc::strdup(name);
    if copy.is_null() {
        return 1;
    }
    (*s).v.add((*s).n as usize).write(NdlSym {
        addr: value,
        name: copy,
    });
    (*s).n += 1;
    0
}

unsafe extern "C" fn ndl_sym_cmp(a: *const c_void, b: *const c_void) -> c_int {
    let x = (*a.cast::<NdlSym>()).addr;
    let y = (*b.cast::<NdlSym>()).addr;
    if x < y {
        -1
    } else if x > y {
        1
    } else {
        0
    }
}

unsafe fn ndl_syms_of(d: *mut DynImage) -> *mut NdlSyms {
    let base = ptr::addr_of_mut!(super::g_dimgs).cast::<DynImage>();
    let idx = d.offset_from(base);
    if idx < 0 || idx >= DYN_DIMG_MAX as isize {
        return ptr::null_mut();
    }
    let slots = ptr::addr_of!(super::g_ndl_syms).cast::<AtomicPtr<NdlSyms>>();
    let slot = slots.add(idx as usize);
    let mut s = (*slot).load(Ordering::SeqCst);
    if !s.is_null() {
        return s;
    }
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_NDL_SYMS_LOCK));
    s = (*slot).load(Ordering::SeqCst);
    if s.is_null() {
        s = libc::calloc(1, core::mem::size_of::<NdlSyms>()).cast();
        if !s.is_null() {
            (*s).lo = (*d).map_base;
            (*s).hi = (*d).map_base.wrapping_add((*d).map_size);
            super::exports::ocerz_dyld_trie_each(
                (*d).slice,
                (*d).load_base,
                Some(ndl_syms_add),
                s.cast(),
            );
            if (*s).n > 1 {
                libc::qsort(
                    (*s).v.cast(),
                    (*s).n as usize,
                    core::mem::size_of::<NdlSym>(),
                    Some(ndl_sym_cmp),
                );
            }
            (*slot).store(s, Ordering::SeqCst);
        }
    }
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_NDL_SYMS_LOCK));
    s
}

unsafe fn ndl_trie_nearest(d: *mut DynImage, addr: u64, sname: *mut u64, saddr: *mut u64) -> c_int {
    let s = ndl_syms_of(d);
    if s.is_null() || (*s).n == 0 || addr < (*s).v.read().addr {
        return 0;
    }
    let mut lo = 0;
    let mut hi = (*s).n - 1;
    while lo < hi {
        let mid = lo + (hi - lo + 1) / 2;
        if (*s).v.add(mid as usize).read().addr <= addr {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    let sym = (*s).v.add(lo as usize).read();
    let name = if sym.name.read() == b'_' as c_char {
        sym.name.add(1)
    } else {
        sym.name
    };
    *sname = crate::ported::dyldapi::hostmem::ocerz_h2g(name.cast());
    *saddr = sym.addr;
    1
}

unsafe fn ndl_symtab_nearest(
    mh: u64,
    slide: i64,
    addr: u64,
    sname: *mut u64,
    saddr: *mut u64,
) -> c_int {
    let h = crate::ported::dyldapi::hostmem::ocerz_g2h(mh).cast::<u8>();
    let ncmds = rd32(h.add(16));
    let mut lc = h.add(core::mem::size_of::<MachHeader64>());
    let mut symoff = 0u32;
    let mut nsyms = 0u32;
    let mut stroff = 0u32;
    let mut strsize = 0u32;
    let mut le_vmaddr = 0u64;
    let mut le_fileoff = 0u64;
    let mut le_filesize = 0u64;
    let mut have_le = false;
    for _ in 0..ncmds {
        let cmd = rd32(lc);
        if cmd == LC_SYMTAB {
            symoff = rd32(lc.add(8));
            nsyms = rd32(lc.add(12));
            stroff = rd32(lc.add(16));
            strsize = rd32(lc.add(20));
        } else if cmd == LC_SEGMENT_64
            && libc::strncmp(lc.add(8).cast(), cstr_ptr(c"__LINKEDIT"), 16) == 0
        {
            le_vmaddr = rd64(lc.add(24));
            le_fileoff = rd64(lc.add(40));
            le_filesize = rd64(lc.add(48));
            have_le = true;
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
    if !have_le
        || nsyms == 0
        || (symoff as u64) < le_fileoff
        || (stroff as u64) < le_fileoff
        || (symoff as u64).wrapping_add((nsyms as u64).wrapping_mul(16))
            > le_fileoff.wrapping_add(le_filesize)
        || (stroff as u64).wrapping_add(strsize as u64) > le_fileoff.wrapping_add(le_filesize)
    {
        return 0;
    }
    let le_base = (le_vmaddr as i64).wrapping_add(slide) as u64;
    let symtab = le_base.wrapping_add((symoff as u64).wrapping_sub(le_fileoff));
    let strtab = le_base.wrapping_add((stroff as u64).wrapping_sub(le_fileoff));
    let mut best = 0u64;
    let mut best_strx = 0u32;
    let mut have = false;
    for i in 0..nsyms {
        let e = crate::ported::dyldapi::hostmem::ocerz_g2h(
            symtab.wrapping_add((i as u64).wrapping_mul(16)),
        )
        .cast::<u8>();
        let strx = rd32(e);
        let ty = e.add(4).read();
        if ty & 0xe0 != 0 || ty & 0x0e != 0x0e || strx == 0 || strx >= strsize {
            continue;
        }
        let value = (rd64(e.add(8)) as i64).wrapping_add(slide) as u64;
        if value > addr || (have && value <= best) {
            continue;
        }
        best = value;
        best_strx = strx;
        have = true;
    }
    if !have {
        return 0;
    }
    let mut namep = strtab.wrapping_add(best_strx as u64);
    if crate::ported::dyldapi::hostmem::ocerz_g2h(namep)
        .cast::<c_char>()
        .read()
        == b'_' as c_char
    {
        namep = namep.wrapping_add(1);
    }
    *sname = namep;
    *saddr = best;
    1
}

unsafe fn ndl_host_dladdr(addr: u64, info: u64) -> c_int {
    let h = crate::ported::dyldapi::hostmem::ocerz_g2h(addr);
    let mut di: libc::Dl_info = core::mem::zeroed();
    if host_in_guest_reservation(h) || libc::dladdr(h, &mut di) == 0 {
        return 0;
    }
    crate::ported::dyldapi::hostmem::ocerz_st(
        info,
        8,
        if di.dli_fname.is_null() {
            0
        } else {
            crate::ported::dyldapi::hostmem::ocerz_h2g(di.dli_fname.cast())
        },
    );
    crate::ported::dyldapi::hostmem::ocerz_st(
        info + 8,
        8,
        if di.dli_fbase.is_null() {
            0
        } else {
            crate::ported::dyldapi::hostmem::ocerz_h2g(di.dli_fbase)
        },
    );
    crate::ported::dyldapi::hostmem::ocerz_st(
        info + 16,
        8,
        if di.dli_sname.is_null() {
            0
        } else {
            crate::ported::dyldapi::hostmem::ocerz_h2g(di.dli_sname.cast())
        },
    );
    crate::ported::dyldapi::hostmem::ocerz_st(
        info + 24,
        8,
        if di.dli_saddr.is_null() {
            0
        } else {
            crate::ported::dyldapi::hostmem::ocerz_h2g(di.dli_saddr)
        },
    );
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_native_dladdr(addr: u64, info: u64) -> c_int {
    if info == 0 {
        return 0;
    }
    let mut im = NdlImage {
        mh: 0,
        slide: 0,
        name: 0,
        d: ptr::null_mut(),
    };
    if ndl_containing(addr, &mut im, ptr::null_mut()) == 0 {
        return ndl_host_dladdr(addr, info);
    }
    let mut sname = 0;
    let mut saddr = 0;
    if ndl_symtab_nearest(im.mh, im.slide, addr, &mut sname, &mut saddr) == 0
        && !im.d.is_null()
        && im.d != ptr::addr_of_mut!(super::g_main_dimg)
    {
        ndl_trie_nearest(im.d, addr, &mut sname, &mut saddr);
    }
    crate::ported::dyldapi::hostmem::ocerz_st(info, 8, im.name);
    crate::ported::dyldapi::hostmem::ocerz_st(info + 8, 8, im.mh);
    crate::ported::dyldapi::hostmem::ocerz_st(info + 16, 8, sname);
    crate::ported::dyldapi::hostmem::ocerz_st(info + 24, 8, saddr);
    1
}

#[repr(C)]
#[derive(Clone, Copy)]
enum NdlKind {
    None,
    Main,
    Loaded,
    Virtual,
    File,
    NativeOnly,
    BadFile,
}

#[repr(C)]
struct NdlTarget {
    kind: NdlKind,
    img: *mut DynImage,
    buf: *mut u8,
    len: usize,
    path: [c_char; libc::PATH_MAX as usize],
    why: [c_char; libc::PATH_MAX as usize + 128],
    tried: [c_char; NDL_TRIED_BYTES],
    tried_len: usize,
}

unsafe fn ndl_tried(t: *mut NdlTarget, cand: *const c_char) {
    let room = (*t).tried.len() - (*t).tried_len;
    let dst = (*t).tried.as_mut_ptr().add((*t).tried_len);
    let prefix = if (*t).tried_len != 0 {
        cstr_ptr(c", ")
    } else {
        cstr_ptr(c"")
    };
    let n = libc::snprintf(dst, room, cstr_ptr(c"%s'%s' (no such file)"), prefix, cand);
    if n > 0 {
        (*t).tried_len += (n as usize).min(room.wrapping_sub(1));
    }
}

unsafe fn ndl_is_main_path(path: *const c_char) -> bool {
    let mut rp = [0 as c_char; libc::PATH_MAX as usize];
    super::g_main_hostpath[0] != 0
        && (libc::strcmp(path, ptr::addr_of!(super::g_main_hostpath).cast()) == 0
            || (!libc::realpath(path, rp.as_mut_ptr()).is_null()
                && libc::strcmp(rp.as_ptr(), ptr::addr_of!(super::g_main_hostpath).cast()) == 0))
}

unsafe fn ndl_known(path: *const c_char, t: *mut NdlTarget) -> bool {
    if ndl_is_main_path(path) {
        (*t).kind = NdlKind::Main;
        return true;
    }
    (*t).img = super::dimg_find_by_path(path);
    if (*t).img.is_null() {
        (*t).img = super::dimg_find_by_install_name(path);
    }
    if !(*t).img.is_null() {
        (*t).kind = NdlKind::Loaded;
        return true;
    }
    let mut guest_path = [0 as c_char; libc::PATH_MAX as usize];
    if super::load::native_guest_path(path, guest_path.as_mut_ptr(), guest_path.len()) == 0
        && ffi::ocerz_vdylib_have(path) != 0
    {
        (*t).kind = NdlKind::Virtual;
        libc::snprintf(
            (*t).path.as_mut_ptr(),
            (*t).path.len(),
            cstr_ptr(c"%s"),
            path,
        );
        return true;
    }
    false
}

unsafe fn ndl_try(cand: *const c_char, t: *mut NdlTarget) -> bool {
    let mut canon = [0 as c_char; libc::PATH_MAX as usize];
    let mut guest_path = [0 as c_char; libc::PATH_MAX as usize];
    let mut path = cand;
    if ndl_known(cand, t) {
        return true;
    }
    let guest_override =
        super::load::native_guest_path(cand, guest_path.as_mut_ptr(), guest_path.len()) != 0;
    if guest_override {
        path = guest_path.as_ptr();
    } else if super::load::ocerz_canon_dylib_path(cand, canon.as_mut_ptr(), canon.len()) != 0
        && libc::strcmp(canon.as_ptr(), cand) != 0
    {
        path = canon.as_ptr();
        if ndl_known(path, t) {
            return true;
        }
    }
    let mut dev = 0;
    let mut ino = 0;
    if super::file_identity(path, &mut dev, &mut ino) != 0 {
        if ocerz_main_mh != 0 && dev == super::g_main_dev && ino == super::g_main_ino {
            (*t).kind = NdlKind::Main;
            return true;
        }
        (*t).img = super::dimg_find_by_identity(dev, ino);
        if !(*t).img.is_null() {
            (*t).kind = NdlKind::Loaded;
            return true;
        }
        let mut abs = [0 as c_char; libc::PATH_MAX as usize];
        if libc::realpath(path, abs.as_mut_ptr()).is_null() {
            libc::snprintf(abs.as_mut_ptr(), abs.len(), cstr_ptr(c"%s"), path);
        }
        let mut len = 0;
        let buf = super::read_file(abs.as_ptr(), &mut len);
        if buf.is_null() {
            (*t).kind = NdlKind::BadFile;
            libc::snprintf(
                (*t).why.as_mut_ptr(),
                (*t).why.len(),
                cstr_ptr(c"'%s' could not be read"),
                abs.as_ptr(),
            );
            return true;
        }
        let magic = if len >= 4 { rd32(buf) } else { 0 };
        if !super::select_slice(buf, len).is_null() {
            (*t).kind = NdlKind::File;
            (*t).buf = buf;
            (*t).len = len;
            libc::snprintf(
                (*t).path.as_mut_ptr(),
                (*t).path.len(),
                cstr_ptr(c"%s"),
                abs.as_ptr(),
            );
            return true;
        }
        libc::free(buf.cast());
        if magic == MH_MAGIC_64
            || magic == MH_MAGIC
            || magic == super::FAT_MAGIC
            || magic == super::FAT_CIGAM
            || magic == super::FAT_MAGIC_64
            || magic == super::FAT_CIGAM_64
        {
            (*t).kind = NdlKind::NativeOnly;
            libc::snprintf(
                (*t).why.as_mut_ptr(),
                (*t).why.len(),
                cstr_ptr(c"'%s' has no x86_64 slice"),
                abs.as_ptr(),
            );
        } else {
            (*t).kind = NdlKind::BadFile;
            libc::snprintf(
                (*t).why.as_mut_ptr(),
                (*t).why.len(),
                cstr_ptr(c"'%s' is not a Mach-O file"),
                abs.as_ptr(),
            );
        }
        return true;
    }
    if guest_override {
        (*t).kind = NdlKind::BadFile;
        libc::snprintf(
            (*t).why.as_mut_ptr(),
            (*t).why.len(),
            cstr_ptr(c"guest override '%s' could not be read"),
            path,
        );
        return true;
    }
    let real = super::load::host_cache_real_path(cand);
    if !real.is_null() {
        if ndl_known(real, t) {
            return true;
        }
        (*t).kind = NdlKind::NativeOnly;
        libc::snprintf(
            (*t).why.as_mut_ptr(),
            (*t).why.len(),
            cstr_ptr(c"'%s' is in the host's shared cache"),
            real,
        );
        return true;
    }
    ndl_tried(t, cand);
    false
}

unsafe fn ndl_try_dirs(list: *const c_char, leaf: *const c_char, t: *mut NdlTarget) -> bool {
    let mut p = list;
    while !p.is_null() && p.read() != 0 {
        let colon = libc::strchr(p, b':' as c_int);
        let len = if colon.is_null() {
            libc::strlen(p)
        } else {
            colon.offset_from(p) as usize
        };
        let mut cand = [0 as c_char; libc::PATH_MAX as usize];
        if len > 0
            && libc::snprintf(
                cand.as_mut_ptr(),
                cand.len(),
                cstr_ptr(c"%.*s/%s"),
                len as c_int,
                p,
                leaf,
            ) < cand.len() as c_int
            && ndl_try(cand.as_ptr(), t)
        {
            return true;
        }
        p = p.add(len);
        if p.read() == b':' as c_char {
            p = p.add(1);
        }
    }
    false
}

pub(super) unsafe fn ndl_rpaths(caller: *mut DynImage) -> *mut super::RpathList {
    let own = libc::calloc(1, core::mem::size_of::<super::RpathList>()).cast::<super::RpathList>();
    let all = libc::calloc(1, core::mem::size_of::<super::RpathList>()).cast::<super::RpathList>();
    if own.is_null() || all.is_null() {
        libc::free(own.cast());
        libc::free(all.cast());
        return ptr::null_mut();
    }
    if !caller.is_null() && caller != ptr::addr_of_mut!(super::g_main_dimg) {
        super::load::collect_rpaths(caller, ptr::null(), own);
    }
    if super::g_main_dimg_valid != 0 {
        super::load::collect_rpaths(ptr::addr_of_mut!(super::g_main_dimg), own.cast_const(), all);
    } else {
        ptr::copy_nonoverlapping(own, all, 1);
    }
    libc::free(own.cast());
    all
}

unsafe fn ndl_resolve(path: *const c_char, caller: *mut DynImage, t: *mut NdlTarget) -> bool {
    let mut cand = [0 as c_char; libc::PATH_MAX as usize];
    if libc::strncmp(path, cstr_ptr(c"@rpath/"), 7) == 0 {
        let rp = ndl_rpaths(caller);
        let mut ok = false;
        if !rp.is_null() {
            for i in 0..(*rp).n {
                if !ok {
                    let entry = ptr::addr_of!((*rp).entry)
                        .cast::<[c_char; 1024]>()
                        .add(i as usize)
                        .cast::<c_char>();
                    if libc::snprintf(
                        cand.as_mut_ptr(),
                        cand.len(),
                        cstr_ptr(c"%s/%s"),
                        entry,
                        path.add(7),
                    ) < cand.len() as c_int
                    {
                        ok = ndl_try(cand.as_ptr(), t);
                    }
                }
            }
        }
        libc::free(rp.cast());
        if !ok && (*t).tried_len == 0 {
            ndl_tried(t, path);
        }
        return ok;
    }
    if path.read() == b'@' as c_char {
        if super::load::expand_at_prefix(caller, path, cand.as_mut_ptr(), cand.len()) != 0 {
            return ndl_try(cand.as_ptr(), t);
        }
        ndl_tried(t, path);
        return false;
    }
    if !libc::strchr(path, b'/' as c_int).is_null() {
        return ndl_try(path, t);
    }
    if ndl_try_dirs(libc::getenv(cstr_ptr(c"DYLD_LIBRARY_PATH")), path, t) || ndl_try(path, t) {
        return true;
    }
    let fallback = libc::getenv(cstr_ptr(c"DYLD_FALLBACK_LIBRARY_PATH"));
    if !fallback.is_null() && fallback.read() != 0 {
        ndl_try_dirs(fallback, path, t)
    } else {
        ndl_try_dirs(cstr_ptr(c"/usr/local/lib:/usr/lib"), path, t)
    }
}

unsafe fn rpaths_append(dst: *mut super::RpathList, src: *const super::RpathList) {
    if src.is_null() {
        return;
    }
    for i in 0..(*src).n {
        if (*dst).n >= super::load::RPATH_MAX as c_int {
            break;
        }
        let target = ptr::addr_of_mut!((*dst).entry)
            .cast::<[c_char; 1024]>()
            .add((*dst).n as usize);
        let source = ptr::addr_of!((*src).entry)
            .cast::<[c_char; 1024]>()
            .add(i as usize);
        libc::snprintf(
            target.cast(),
            1024,
            cstr_ptr(c"%s"),
            source.cast::<c_char>(),
        );
        (*dst).n += 1;
    }
}

unsafe fn ndl_load_file(t: *mut NdlTarget, chain: *const super::RpathList) -> *mut DynImage {
    if super::g_dimgs_n >= DYN_DIMG_MAX as c_int {
        super::load::native_dl_reason(
            cstr_ptr(c"the loader holds as many images as it can"),
            ptr::null(),
        );
        return ptr::null_mut();
    }
    let d = ptr::addr_of_mut!(super::g_dimgs)
        .cast::<DynImage>()
        .add(super::g_dimgs_n as usize);
    super::g_dimgs_n += 1;
    ptr::write_bytes(d, 0, 1);
    (*d).slice = super::select_slice((*t).buf, (*t).len);
    (*d).owned_buf = (*t).buf;
    (*t).buf = ptr::null_mut();
    libc::snprintf(
        (*d).path.as_mut_ptr(),
        (*d).path.len(),
        cstr_ptr(c"%s"),
        (*t).path.as_ptr(),
    );
    libc::snprintf(
        (*d).install_name.as_mut_ptr(),
        (*d).install_name.len(),
        cstr_ptr(c"%s"),
        (*t).path.as_ptr(),
    );
    super::file_identity((*t).path.as_ptr(), &mut (*d).file_dev, &mut (*d).file_ino);
    super::dimg_record_id(d);
    if super::map::map_segments(d, 0) != ffi::OCERZ_OK {
        super::load::native_dl_reason(cstr_ptr(c"its segments could not be mapped"), ptr::null());
        super::g_dimgs_n -= 1;
        libc::free((*d).owned_buf.cast());
        ptr::write_bytes(d, 0, 1);
        return ptr::null_mut();
    }
    let rp = libc::calloc(1, core::mem::size_of::<super::RpathList>()).cast::<super::RpathList>();
    if rp.is_null() {
        super::load::native_dl_reason(cstr_ptr(c"no memory for its rpaths"), ptr::null());
        return ptr::null_mut();
    }
    super::load::collect_rpaths(d, ptr::null(), rp);
    rpaths_append(rp, chain);
    super::load::load_disk_deps(super::g_run_cache, d, rp.cast_const());
    libc::free(rp.cast());
    if super::bind::apply_fixups(d, super::g_run_cache) != ffi::OCERZ_OK {
        super::load::native_dl_reason(
            cstr_ptr(c"its chained fixups use a pointer format ocerz does not apply"),
            ptr::null(),
        );
        return ptr::null_mut();
    }
    if (*d).cf_off == 0 {
        super::bind::apply_classic_fixups(d, super::g_run_cache);
    }
    super::map::protect_ro_segments(d);
    super::load::G_DIMG_SEQ = super::load::G_DIMG_SEQ.wrapping_add(1);
    (*d).seq = super::load::G_DIMG_SEQ;
    crate::ocerz_log!(
        "dynamic: native dlopen loaded %s at load_base=%#llx slide=%#llx\n",
        (*d).path.as_ptr(),
        (*d).load_base as c_ulonglong,
        (*d).slide as c_ulonglong
    );
    d
}

unsafe fn ndl_rollback(before: c_int) {
    let mut i = super::g_dimgs_n - 1;
    while i >= before {
        let d = ptr::addr_of_mut!(super::g_dimgs)
            .cast::<DynImage>()
            .add(i as usize);
        super::map::protect_ro_drop(d);
        if (*d).map_size != 0 {
            ffi::ocerz_unmap((*d).map_base, (*d).map_size);
        }
        libc::free((*d).owned_buf.cast());
        ptr::write_bytes(d, 0, 1);
        i -= 1;
    }
    super::g_dimgs_n = before;
}

static mut G_NDL_ADD_FUNCS: *mut u64 = ptr::null_mut();
static mut G_NDL_ADD_N: c_int = 0;
static mut G_NDL_ADD_CAP: c_int = 0;
static mut G_NDL_REMOVE_FUNCS: *mut u64 = ptr::null_mut();
static mut G_NDL_REMOVE_N: c_int = 0;
static mut G_NDL_REMOVE_CAP: c_int = 0;
static mut G_NDL_OBJC_FUNCS: *mut u64 = ptr::null_mut();
static mut G_NDL_OBJC_N: c_int = 0;
static mut G_NDL_OBJC_CAP: c_int = 0;

unsafe fn ndl_append_func(
    array: *mut *mut u64,
    n: *mut c_int,
    cap: *mut c_int,
    func: u64,
) -> c_int {
    if *n == *cap {
        let new_cap = if *cap != 0 {
            (*cap).wrapping_mul(2)
        } else {
            16
        };
        let grown = libc::realloc(
            (*array).cast(),
            (new_cap as usize).wrapping_mul(core::mem::size_of::<u64>()),
        )
        .cast::<u64>();
        if grown.is_null() {
            return 0;
        }
        *array = grown;
        *cap = new_cap;
    }
    (*array).add(*n as usize).write(func);
    *n += 1;
    1
}

unsafe fn ndl_call_add(vm: *mut OcerzVM, func: u64, mh: u64, slide: i64, stack_top: u64) {
    let args = [mh, slide as u64];
    ffi::ocerz_vm_call(vm, func, args.as_ptr(), 2, stack_top);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_native_add_image_func(
    vm: *mut OcerzVM,
    func: u64,
    stack_top: u64,
) -> c_int {
    if func == 0 {
        return 0;
    }
    super::dlopen::load_lock();
    let ok = ndl_append_func(
        ptr::addr_of_mut!(G_NDL_ADD_FUNCS),
        ptr::addr_of_mut!(G_NDL_ADD_N),
        ptr::addr_of_mut!(G_NDL_ADD_CAP),
        func,
    );
    let n = if ok != 0 { ocerz_dyld_image_count() } else { 0 };
    let mut im = NdlImage {
        mh: 0,
        slide: 0,
        name: 0,
        d: ptr::null_mut(),
    };
    for i in 0..n {
        if (*vm).exited != 0 {
            break;
        }
        if ndl_image(i, &mut im) != 0 {
            ndl_call_add(vm, func, im.mh, im.slide, stack_top);
        }
    }
    super::dlopen::load_unlock();
    ok
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_native_objc_load_func(
    vm: *mut OcerzVM,
    func: u64,
    stack_top: u64,
) -> c_int {
    if func == 0 {
        return 0;
    }
    super::dlopen::load_lock();
    let ok = ndl_append_func(
        ptr::addr_of_mut!(G_NDL_OBJC_FUNCS),
        ptr::addr_of_mut!(G_NDL_OBJC_N),
        ptr::addr_of_mut!(G_NDL_OBJC_CAP),
        func,
    );
    let n = if ok != 0 { ocerz_dyld_image_count() } else { 0 };
    let mut im = NdlImage {
        mh: 0,
        slide: 0,
        name: 0,
        d: ptr::null_mut(),
    };
    for i in 0..n {
        if (*vm).exited != 0 {
            break;
        }
        if ndl_image(i, &mut im) != 0 {
            let args = [im.mh];
            ffi::ocerz_vm_call(vm, func, args.as_ptr(), 1, stack_top);
        }
    }
    super::dlopen::load_unlock();
    ok
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_native_remove_image_func(func: u64) -> c_int {
    if func == 0 {
        return 0;
    }
    super::dlopen::load_lock();
    let ok = ndl_append_func(
        ptr::addr_of_mut!(G_NDL_REMOVE_FUNCS),
        ptr::addr_of_mut!(G_NDL_REMOVE_N),
        ptr::addr_of_mut!(G_NDL_REMOVE_CAP),
        func,
    );
    super::dlopen::load_unlock();
    ok
}

unsafe extern "C" fn ndl_seq_cmp(a: *const c_void, b: *const c_void) -> c_int {
    let x = (**a.cast::<*mut DynImage>()).seq;
    let y = (**b.cast::<*mut DynImage>()).seq;
    if x < y {
        -1
    } else if x > y {
        1
    } else {
        0
    }
}

unsafe fn ndl_fail_load(path: *const c_char, mode: c_int, top: *mut DynImage, miss_before: c_int) {
    if top.is_null() {
        ndl_err!(
            "dlopen(%s, 0x%04x): %s",
            path,
            mode,
            if super::load::g_ndl.reason[0] != 0 {
                ptr::addr_of!(super::load::g_ndl.reason).cast::<c_char>()
            } else {
                cstr_ptr(c"it could not be loaded")
            }
        );
    } else if super::load::g_ndl.missing[0] != 0 {
        ndl_err!(
            "dlopen(%s, 0x%04x): Library not loaded: %s\n  Referenced from: %s\n  Reason: %s",
            path,
            mode,
            ptr::addr_of!(super::load::g_ndl.missing).cast::<c_char>(),
            ptr::addr_of!(super::load::g_ndl.missing_from).cast::<c_char>(),
            ptr::addr_of!(super::load::g_ndl.reason).cast::<c_char>()
        );
    } else if super::g_native_miss_n > miss_before {
        let m = ptr::addr_of!(super::g_native_miss)
            .cast::<super::NativeMiss>()
            .add(miss_before as usize);
        ndl_err!(
            "dlopen(%s, 0x%04x): Symbol not found: %s\n  Referenced from: %s\n  Expected in: %s",
            path,
            mode,
            (*m).sym.as_ptr(),
            (*m).from.as_ptr(),
            (*m).lib.as_ptr()
        );
    } else {
        ndl_err!(
            "dlopen(%s, 0x%04x): more symbols were not found than ocerz keeps a record of",
            path,
            mode
        );
    }
}

unsafe fn ndl_dlopen_locked(
    vm: *mut OcerzVM,
    path: *const c_char,
    mode: c_int,
    caller: u64,
    stack_top: u64,
) -> u64 {
    ndl_err_clear();
    if path.is_null() {
        return if mode & NDL_RTLD_FIRST != 0 {
            NDL_MAIN_ONLY
        } else {
            NDL_DEFAULT
        };
    }
    let mut caller_image = NdlImage {
        mh: 0,
        slide: 0,
        name: 0,
        d: ptr::null_mut(),
    };
    let caller_d = if ndl_containing(caller, &mut caller_image, ptr::null_mut()) != 0 {
        caller_image.d
    } else {
        ptr::null_mut()
    };
    let t = libc::calloc(1, core::mem::size_of::<NdlTarget>()).cast::<NdlTarget>();
    if t.is_null() {
        ndl_err!("dlopen(%s, 0x%04x): no memory", path, mode);
        return 0;
    }
    let first = if mode & NDL_RTLD_FIRST != 0 { 1 } else { 0 };
    let mut load = false;
    let mut handle = 0;
    if !ndl_resolve(path, caller_d, t) {
        ndl_err!(
            "dlopen(%s, 0x%04x): tried: %s",
            path,
            mode,
            (*t).tried.as_ptr()
        );
    } else if matches!((*t).kind, NdlKind::Main) {
        handle = ocerz_main_mh | first;
    } else if matches!((*t).kind, NdlKind::Loaded) {
        if mode & NDL_RTLD_LOCAL == 0 {
            (*(*t).img).local = 0;
        }
        handle = (*(*t).img).load_base | first;
    } else if matches!((*t).kind, NdlKind::NativeOnly) {
        ndl_err!(
            "dlopen(%s, 0x%04x): native library without an API database: %s",
            path,
            mode,
            (*t).why.as_ptr()
        );
    } else if matches!((*t).kind, NdlKind::BadFile) {
        ndl_err!("dlopen(%s, 0x%04x): %s", path, mode, (*t).why.as_ptr());
    } else if mode & NDL_RTLD_NOLOAD != 0 {
        ndl_err!(
            "dlopen(%s, 0x%04x): not loaded, and RTLD_NOLOAD forbids loading it",
            path,
            mode
        );
    } else {
        load = true;
    }
    if !load {
        libc::free((*t).buf.cast());
        libc::free(t.cast());
        return handle;
    }

    let before = super::g_dimgs_n;
    let miss_before = super::g_native_miss_n;
    let dropped_before = super::g_native_miss_dropped;
    ptr::write_bytes(ptr::addr_of_mut!(super::load::g_ndl), 0, 1);
    super::load::g_ndl.active = 1;
    let top = if matches!((*t).kind, NdlKind::Virtual) {
        super::load::load_disk_dylib(
            super::g_run_cache,
            (*t).path.as_ptr(),
            ptr::null_mut(),
            ptr::null(),
        )
    } else {
        let chain = ndl_rpaths(caller_d);
        let img = ndl_load_file(t, chain);
        libc::free(chain.cast());
        img
    };
    super::load::g_ndl.active = 0;
    libc::free((*t).buf.cast());
    libc::free(t.cast());
    if top.is_null()
        || super::load::g_ndl.missing[0] != 0
        || super::g_native_miss_n > miss_before
        || super::g_native_miss_dropped > dropped_before
    {
        ndl_fail_load(path, mode, top, miss_before);
        super::g_native_miss_n = miss_before;
        super::g_native_miss_dropped = dropped_before;
        ndl_rollback(before);
        return 0;
    }
    let after = super::g_dimgs_n;
    if mode & NDL_RTLD_LOCAL != 0 {
        (*top).local = 1;
    }
    native_publish();
    super::tlv::native_tlv_register_loaded(ocerz_main_mh);

    for f in 0..G_NDL_ADD_N {
        if (*vm).exited != 0 {
            break;
        }
        for i in before..after {
            if (*vm).exited != 0 {
                break;
            }
            let d = ptr::addr_of_mut!(super::g_dimgs)
                .cast::<DynImage>()
                .add(i as usize);
            ndl_call_add(
                vm,
                *G_NDL_ADD_FUNCS.add(f as usize),
                (*d).load_base,
                (*d).slide as i64,
                stack_top,
            );
        }
    }

    let mut order = [ptr::null_mut::<DynImage>(); DYN_DIMG_MAX];
    let mut n = 0;
    for i in before..after {
        let d = ptr::addr_of_mut!(super::g_dimgs)
            .cast::<DynImage>()
            .add(i as usize);
        if (*d).is_virtual == 0 {
            order[n as usize] = d;
            n += 1;
        }
    }
    if n > 1 {
        libc::qsort(
            order.as_mut_ptr().cast(),
            n as usize,
            core::mem::size_of::<*mut DynImage>(),
            Some(ndl_seq_cmp),
        );
    }
    for i in 0..n {
        let d = order[i as usize];
        let h = crate::ported::dyldapi::hostmem::ocerz_g2h((*d).load_base).cast::<u8>();
        super::ffi::ocerz_objcbridge_fix_selrefs(h, (*d).slide as i64);
        super::ffi::ocerz_objcbridge_define_image(h, (*d).slide as i64);
    }
    if !super::dimg_find_by_install_name(cstr_ptr(c"/usr/lib/libobjc.A.dylib")).is_null() {
        super::ffi::ocerz_objcbridge_install_uncaught();
    }
    for f in 0..G_NDL_OBJC_N {
        if (*vm).exited != 0 {
            break;
        }
        for i in before..after {
            if (*vm).exited != 0 {
                break;
            }
            let d = ptr::addr_of_mut!(super::g_dimgs)
                .cast::<DynImage>()
                .add(i as usize);
            let args = [(*d).load_base];
            ffi::ocerz_vm_call(
                vm,
                *G_NDL_OBJC_FUNCS.add(f as usize),
                args.as_ptr(),
                1,
                stack_top,
            );
        }
    }
    super::map::protect_ro_flush();
    for i in 0..n {
        if (*vm).exited != 0 {
            break;
        }
        let d = order[i as usize];
        let h = crate::ported::dyldapi::hostmem::ocerz_g2h((*d).load_base).cast::<u8>();
        ffi::ocerz_objcbridge_run_image_loads(vm, h, stack_top);
        if (*vm).exited == 0 {
            super::init::run_image_inits(
                vm,
                (*d).load_base,
                ptr::addr_of!(g_native_init_args).cast::<u64>(),
                stack_top,
            );
        }
    }
    (*top).load_base | first
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_native_dlopen(
    vm: *mut OcerzVM,
    path: *const c_char,
    mode: c_int,
    caller: u64,
    stack_top: u64,
) -> u64 {
    let log = !libc::getenv(cstr_ptr(c"OCERZ_DLOPENLOG")).is_null();
    if log {
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"ocerz: DLOPEN \"%s\" mode=%#x\n"),
            if path.is_null() {
                cstr_ptr(c"(null)")
            } else {
                path
            },
            mode,
        );
    }
    super::dlopen::load_lock();
    let handle = ndl_dlopen_locked(vm, path, mode, caller, stack_top);
    super::dlopen::load_unlock();
    if log {
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"ocerz: DLOPEN \"%s\" -> %#llx\n"),
            if path.is_null() {
                cstr_ptr(c"(null)")
            } else {
                path
            },
            handle as c_ulonglong,
        );
    }
    handle
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_native_dlopen_preflight(
    path: *const c_char,
    caller: u64,
) -> c_int {
    super::dlopen::load_lock();
    ndl_err_clear();
    let mut caller_image = NdlImage {
        mh: 0,
        slide: 0,
        name: 0,
        d: ptr::null_mut(),
    };
    let caller_d = if ndl_containing(caller, &mut caller_image, ptr::null_mut()) != 0 {
        caller_image.d
    } else {
        ptr::null_mut()
    };
    let t = if path.is_null() {
        ptr::null_mut()
    } else {
        libc::calloc(1, core::mem::size_of::<NdlTarget>()).cast::<NdlTarget>()
    };
    let mut ok = 0;
    if path.is_null() {
        ok = 1;
    } else if t.is_null() {
        ndl_err!("dlopen_preflight(%s): no memory", path);
    } else if !ndl_resolve(path, caller_d, t) {
        ndl_err!("dlopen_preflight(%s): tried: %s", path, (*t).tried.as_ptr());
    } else if matches!((*t).kind, NdlKind::NativeOnly) {
        ndl_err!(
            "dlopen_preflight(%s): native library without an API database: %s",
            path,
            (*t).why.as_ptr()
        );
    } else if matches!((*t).kind, NdlKind::BadFile) {
        ndl_err!("dlopen_preflight(%s): %s", path, (*t).why.as_ptr());
    } else {
        ok = 1;
    }
    if !t.is_null() {
        libc::free((*t).buf.cast());
    }
    libc::free(t.cast());
    super::dlopen::load_unlock();
    ok
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_native_names_library(path: *const c_char) -> c_int {
    let mut canon = [0 as c_char; libc::PATH_MAX as usize];
    if path.is_null() || path.read() == 0 {
        return 0;
    }
    if ffi::ocerz_vdylib_have(path) != 0 {
        return 1;
    }
    if super::load::ocerz_canon_dylib_path(path, canon.as_mut_ptr(), canon.len()) != 0
        && libc::strcmp(canon.as_ptr(), path) != 0
        && ffi::ocerz_vdylib_have(canon.as_ptr()) != 0
    {
        return 1;
    }
    let real = super::load::host_cache_real_path(path);
    (!real.is_null() && ffi::ocerz_vdylib_have(real) != 0) as c_int
}

#[repr(C)]
#[derive(Clone, Copy)]
struct NdlBuildVersion {
    platform: u32,
    version: u32,
}

type NdlImmutableFn = unsafe extern "C" fn(*const c_void, usize) -> bool;
type NdlVersionFn = unsafe extern "C" fn(*const c_void, NdlBuildVersion) -> bool;

#[repr(C)]
struct NdlHostDyld {
    immutable: Option<NdlImmutableFn>,
    sdk_at_least: Option<NdlVersionFn>,
    minos_at_least: Option<NdlVersionFn>,
}

static mut G_NDL_HOST: NdlHostDyld = NdlHostDyld {
    immutable: None,
    sdk_at_least: None,
    minos_at_least: None,
};
static mut G_NDL_HOST_ONCE: libc::pthread_once_t = libc::PTHREAD_ONCE_INIT;

unsafe extern "C" fn ndl_host_init() {
    let p = libc::dlsym(libc::RTLD_DEFAULT, cstr_ptr(c"_dyld_is_memory_immutable"));
    if !p.is_null() {
        G_NDL_HOST.immutable = Some(core::mem::transmute::<*mut c_void, NdlImmutableFn>(p));
    }
    let p = libc::dlsym(libc::RTLD_DEFAULT, cstr_ptr(c"dyld_sdk_at_least"));
    if !p.is_null() {
        G_NDL_HOST.sdk_at_least = Some(core::mem::transmute::<*mut c_void, NdlVersionFn>(p));
    }
    let p = libc::dlsym(libc::RTLD_DEFAULT, cstr_ptr(c"dyld_minos_at_least"));
    if !p.is_null() {
        G_NDL_HOST.minos_at_least = Some(core::mem::transmute::<*mut c_void, NdlVersionFn>(p));
    }
}

unsafe fn ndl_host() -> *const NdlHostDyld {
    libc::pthread_once(ptr::addr_of_mut!(G_NDL_HOST_ONCE), Some(ndl_host_init));
    ptr::addr_of!(G_NDL_HOST)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_is_memory_immutable(addr: u64, len: u64) -> c_int {
    let n = ocerz_dyld_image_count();
    let mut im = NdlImage {
        mh: 0,
        slide: 0,
        name: 0,
        d: ptr::null_mut(),
    };
    for i in 0..n {
        let mut ro = 0;
        if ndl_image(i, &mut im) != 0 && ndl_covers(im.mh, addr, len, &mut ro) != 0 {
            return ro;
        }
    }
    let h = crate::ported::dyldapi::hostmem::ocerz_g2h(addr);
    let host = ndl_host();
    if host_in_guest_reservation(h) || (*host).immutable.is_none() {
        return 0;
    }
    ((*host).immutable.unwrap()(h, len as usize)) as c_int
}

unsafe fn ndl_is_image(mh: u64) -> bool {
    let mut slide = 0;
    mh != 0 && ocerz_dyld_image_slide(mh, &mut slide) != 0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_build_version(
    mh: u64,
    platform: *mut u32,
    minos: *mut u32,
    sdk: *mut u32,
) -> c_int {
    *platform = 0;
    *minos = 0;
    *sdk = 0;
    if !ndl_is_image(mh) {
        return 0;
    }
    let h = crate::ported::dyldapi::hostmem::ocerz_g2h(mh).cast::<u8>();
    let ncmds = rd32(h.add(16));
    let mut lc = h.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        let cmd = rd32(lc);
        if cmd == LC_BUILD_VERSION {
            *platform = rd32(lc.add(8));
            *minos = rd32(lc.add(12));
            *sdk = rd32(lc.add(16));
            return 1;
        }
        if cmd == LC_VERSION_MIN_MACOSX {
            *platform = PLATFORM_MACOS;
            *minos = rd32(lc.add(8));
            *sdk = rd32(lc.add(12));
            return 1;
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_version_at_least(mh: u64, version: u64, sdk: c_int) -> c_int {
    let v = NdlBuildVersion {
        platform: version as u32,
        version: (version >> 32) as u32,
    };
    let mut platform = 0;
    let mut minos = 0;
    let mut have = 0;
    if ocerz_dyld_build_version(mh, &mut platform, &mut minos, &mut have) == 0 {
        return 0;
    }
    let host = ndl_host();
    let fun = if sdk != 0 {
        (*host).sdk_at_least
    } else {
        (*host).minos_at_least
    };
    if let Some(fun) = fun {
        return fun(crate::ported::dyldapi::hostmem::ocerz_g2h(mh), v) as c_int;
    }
    if v.platform == 0xffff_ffff {
        return 1;
    }
    (v.platform == platform && (if sdk != 0 { have } else { minos }) >= v.version) as c_int
}

pub(super) unsafe fn native_image_minos(mh: *const u8) -> u32 {
    if rd32(mh) != MH_MAGIC_64 {
        return 0;
    }
    let ncmds = rd32(mh.add(16));
    let sizeofcmds = rd32(mh.add(20));
    let mut lc = mh.add(core::mem::size_of::<MachHeader64>());
    let mut walked = 0u32;
    for _ in 0..ncmds {
        if walked.wrapping_add(16) > sizeofcmds {
            break;
        }
        let cmd = rd32(lc);
        let cmdsize = rd32(lc.add(4));
        if cmdsize < 8 || walked.wrapping_add(cmdsize) > sizeofcmds {
            break;
        }
        if cmd == LC_BUILD_VERSION && cmdsize >= 16 && rd32(lc.add(8)) == PLATFORM_MACOS {
            return rd32(lc.add(12));
        }
        if cmd == LC_VERSION_MIN_MACOSX && cmdsize >= 12 {
            return rd32(lc.add(8));
        }
        lc = lc.add(cmdsize as usize);
        walked = walked.wrapping_add(cmdsize);
    }
    0
}

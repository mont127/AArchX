//! Rpath expansion and recursive loading of disk, guest, and virtual dylibs.

use super::*;
use crate::ported::dyldapi::macho::{LoadCommand, Section64, SegmentCommand64};

const LC_LOAD_DYLIB: u32 = 0xc;
const LC_LOAD_WEAK_DYLIB: u32 = 0x8000_0018;
const LC_RPATH: u32 = 0x8000_001c;
const LC_REEXPORT_DYLIB: u32 = 0x8000_001f;
const LC_LOAD_UPWARD_DYLIB: u32 = 0x8000_0023;
pub(super) const RPATH_MAX: usize = 64;

#[repr(C)]
pub(super) struct NativeDlLoad {
    pub(super) active: c_int,
    pub(super) missing: [c_char; 1024],
    pub(super) missing_from: [c_char; 1024],
    pub(super) reason: [c_char; 256],
}

pub(super) static mut g_ndl: NativeDlLoad = NativeDlLoad {
    active: 0,
    missing: [0; 1024],
    missing_from: [0; 1024],
    reason: [0; 256],
};
pub(super) static mut G_DIMG_SEQ: u32 = 0;
static mut G_HOST_CACHE_REAL_PATH: Option<unsafe extern "C" fn(*const c_char) -> *const c_char> =
    None;
static mut G_HOST_CACHE_REAL_PATH_ONCE: libc::pthread_once_t = libc::PTHREAD_ONCE_INIT;

unsafe extern "C" {
    fn _NSGetExecutablePath(buf: *mut c_char, bufsize: *mut u32) -> c_int;
}

unsafe fn path_dirname(input: *const c_char, out: *mut c_char, n: usize) {
    if input.is_null() || input.read() == 0 {
        libc::snprintf(out, n, cstr_ptr(c"."));
        return;
    }
    let slash = libc::strrchr(input, b'/' as c_int);
    if slash.is_null() {
        libc::snprintf(out, n, cstr_ptr(c"."));
        return;
    }
    if slash.cast_const() == input {
        libc::snprintf(out, n, cstr_ptr(c"/"));
        return;
    }
    let mut len = slash.offset_from(input) as usize;
    if len >= n {
        len = n - 1;
    }
    ptr::copy_nonoverlapping(input, out, len);
    out.add(len).write(0);
}

pub(super) unsafe fn expand_at_prefix(
    loader: *mut DynImage,
    name: *const c_char,
    out: *mut c_char,
    n: usize,
) -> c_int {
    let mut base = ptr::null();
    let mut rest = ptr::null();
    if libc::strncmp(name, cstr_ptr(c"@executable_path"), 16) == 0
        && (*name.add(16) == b'/' as c_char || *name.add(16) == 0)
    {
        base = ptr::addr_of!(g_main_hostpath).cast::<c_char>();
        rest = name.add(16);
    } else if libc::strncmp(name, cstr_ptr(c"@loader_path"), 12) == 0
        && (*name.add(12) == b'/' as c_char || *name.add(12) == 0)
    {
        base = if loader.is_null() {
            ptr::addr_of!(g_main_hostpath).cast::<c_char>()
        } else {
            ptr::addr_of!((*loader).path).cast::<c_char>()
        };
        rest = name.add(12);
    } else {
        return 0;
    }
    let mut dir = [0 as c_char; 1024];
    path_dirname(base, dir.as_mut_ptr(), dir.len());
    libc::snprintf(out, n, cstr_ptr(c"%s%s"), dir.as_ptr(), rest);
    1
}

unsafe fn expand_rpath_entry(
    entry: *const c_char,
    loader: *mut DynImage,
    out: *mut c_char,
    n: usize,
) -> c_int {
    if expand_at_prefix(loader, entry, out, n) != 0 {
        return 1;
    }
    libc::snprintf(out, n, cstr_ptr(c"%s"), entry);
    1
}

pub(super) unsafe fn collect_rpaths(
    img: *mut DynImage,
    inherited: *const RpathList,
    merged: *mut RpathList,
) {
    (*merged).n = 0;
    if !inherited.is_null() {
        for i in 0..(*inherited).n {
            if (*merged).n >= RPATH_MAX as c_int {
                break;
            }
            let dst = ptr::addr_of_mut!((*merged).entry)
                .cast::<[c_char; 1024]>()
                .add((*merged).n as usize);
            let src = ptr::addr_of!((*inherited).entry)
                .cast::<[c_char; 1024]>()
                .add(i as usize);
            libc::snprintf(dst.cast(), 1024, cstr_ptr(c"%s"), src.cast::<c_char>());
            (*merged).n += 1;
        }
    }
    let mh = (*img).slice;
    let ncmds = rd32(mh.add(16));
    let mut lc = mh.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        if rd32(lc) == LC_RPATH {
            let off = rd32(lc.add(8));
            if off < rd32(lc.add(4)) && (*merged).n < RPATH_MAX as c_int {
                let mut exp = [0 as c_char; 1024];
                expand_rpath_entry(
                    lc.add(off as usize).cast(),
                    img,
                    exp.as_mut_ptr(),
                    exp.len(),
                );
                let dst = ptr::addr_of_mut!((*merged).entry)
                    .cast::<[c_char; 1024]>()
                    .add((*merged).n as usize);
                libc::snprintf(dst.cast(), 1024, cstr_ptr(c"%s"), exp.as_ptr());
                (*merged).n += 1;
            }
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
}

pub(super) unsafe fn rpath_supplied(cache: *mut OcerzCache, path: *const c_char) -> bool {
    let mut guest = [0 as c_char; 1024];
    if libc::access(path, libc::F_OK) == 0 {
        return true;
    }
    if ffi::ocerz_mode != MODE_NATIVE {
        return !cache.is_null() && super::eager::dep_find(cache, path) != 0;
    }
    native_guest_path(path, guest.as_mut_ptr(), guest.len()) != 0
        || ffi::ocerz_vdylib_have(path) != 0
}

pub(super) unsafe fn expand_install_name(
    cache: *mut OcerzCache,
    loader: *mut DynImage,
    name: *const c_char,
    rpaths: *const RpathList,
    out: *mut c_char,
    n: usize,
) -> c_int {
    if name.is_null() {
        return 0;
    }
    if name.read() != b'@' as c_char {
        libc::snprintf(out, n, cstr_ptr(c"%s"), name);
        return 1;
    }
    if expand_at_prefix(loader, name, out, n) != 0 {
        return 1;
    }
    if libc::strncmp(name, cstr_ptr(c"@rpath/"), 7) == 0 {
        let stem = name.add(7);
        if !rpaths.is_null() {
            for i in 0..(*rpaths).n {
                let entry = ptr::addr_of!((*rpaths).entry)
                    .cast::<[c_char; 1024]>()
                    .add(i as usize)
                    .cast::<c_char>();
                let mut cand = [0 as c_char; 1024];
                libc::snprintf(
                    cand.as_mut_ptr(),
                    cand.len(),
                    cstr_ptr(c"%s/%s"),
                    entry,
                    stem,
                );
                if rpath_supplied(cache, cand.as_ptr()) {
                    libc::snprintf(out, n, cstr_ptr(c"%s"), cand.as_ptr());
                    return 1;
                }
            }
        }
        return 0;
    }
    0
}

pub(super) unsafe fn canonicalize_objc_selrefs(img: *mut DynImage) {
    let slide = (*img).slide;
    let h = crate::ported::dyldapi::hostmem::ocerz_g2h((*img).load_base).cast::<u8>();
    if ffi::ocerz_mode == MODE_NATIVE {
        ffi::ocerz_objcbridge_fix_selrefs(h, slide as i64);
        ffi::ocerz_objcbridge_define_image(h, slide as i64);
        return;
    }
    if rd32(h) != MH_MAGIC_64 {
        return;
    }
    let ncmds = rd32(h.add(16));
    let mut lc = h.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        let l = lc.cast::<LoadCommand>();
        if (*l).cmd == LC_SEGMENT_64 {
            let s = lc.cast::<SegmentCommand64>();
            let sections = s.add(1).cast::<Section64>();
            for j in 0..(*s).nsects {
                let sc = sections.add(j as usize);
                if libc::strncmp(
                    (*sc).sectname.as_ptr().cast(),
                    cstr_ptr(c"__objc_selrefs"),
                    16,
                ) != 0
                {
                    continue;
                }
                let a = (*sc).addr.wrapping_add(slide as u64);
                let e = a.wrapping_add((*sc).size);
                let mut pp = a;
                while pp.wrapping_add(8) <= e {
                    let name = rd64(crate::ported::dyldapi::hostmem::ocerz_g2h(pp).cast());
                    if name != 0 {
                        let canon = ffi::ocerz_dyldapi_canonical_selector(
                            crate::ported::dyldapi::hostmem::ocerz_g2h(name).cast(),
                        );
                        if canon != 0 && canon != name {
                            wr64(crate::ported::dyldapi::hostmem::ocerz_g2h(pp).cast(), canon);
                        }
                    }
                    pp = pp.wrapping_add(8);
                }
            }
        }
        lc = lc.add((*l).cmdsize as usize);
    }
}

unsafe extern "C" fn host_cache_real_path_init() {
    let ptr = libc::dlsym(
        libc::RTLD_DEFAULT,
        cstr_ptr(c"_dyld_shared_cache_real_path"),
    );
    G_HOST_CACHE_REAL_PATH = if ptr.is_null() {
        None
    } else {
        Some(core::mem::transmute::<
            *mut c_void,
            unsafe extern "C" fn(*const c_char) -> *const c_char,
        >(ptr))
    };
}

pub(super) unsafe fn host_cache_real_path(path: *const c_char) -> *const c_char {
    libc::pthread_once(
        ptr::addr_of_mut!(G_HOST_CACHE_REAL_PATH_ONCE),
        Some(host_cache_real_path_init),
    );
    match G_HOST_CACHE_REAL_PATH {
        Some(func) if !path.is_null() => func(path),
        _ => ptr::null(),
    }
}

pub(super) unsafe fn native_dl_reason(what: *const c_char, path: *const c_char) {
    if g_ndl.active == 0 || g_ndl.reason[0] != 0 {
        return;
    }
    libc::snprintf(
        ptr::addr_of_mut!(g_ndl.reason).cast::<c_char>(),
        256,
        what,
        if path.is_null() { cstr_ptr(c"") } else { path },
    );
}

pub(super) unsafe fn native_guest_path(name: *const c_char, out: *mut c_char, n: usize) -> c_int {
    let mut root = libc::getenv(cstr_ptr(c"OCERZ_GUEST_ROOT"));
    if ffi::ocerz_mode != MODE_NATIVE || name.is_null() || name.read() != b'/' as c_char {
        return 0;
    }
    let mut default_root = [0 as c_char; libc::PATH_MAX as usize];
    if root.is_null() {
        let mut exe = [0 as c_char; libc::PATH_MAX as usize];
        let mut size = exe.len() as u32;
        if _NSGetExecutablePath(exe.as_mut_ptr(), &mut size) != 0
            || libc::realpath(exe.as_ptr(), default_root.as_mut_ptr()).is_null()
        {
            return 0;
        }
        let slash = libc::strrchr(default_root.as_mut_ptr(), b'/' as c_int);
        if slash.is_null()
            || slash.offset_from(default_root.as_ptr()) as usize
                + libc::strlen(cstr_ptr(c"/runtime/guest"))
                > default_root.len()
        {
            return 0;
        }
        libc::strcpy(slash, cstr_ptr(c"/runtime/guest"));
        root = default_root.as_mut_ptr();
    }
    if root.read() == 0 {
        return 0;
    }
    let cryptex = cstr_ptr(c"/System/Volumes/Preboot/Cryptexes/OS");
    let cryptex_len = libc::strlen(cryptex);
    let mut actual_name = name;
    if libc::strncmp(name, cryptex, cryptex_len) == 0 && *name.add(cryptex_len) == b'/' as c_char {
        actual_name = name.add(cryptex_len);
    }
    let len = libc::snprintf(out, n, cstr_ptr(c"%s%s"), root, actual_name);
    let mut st: libc::stat = core::mem::zeroed();
    (len > 0 && (len as usize) < n && libc::lstat(out, &mut st) == 0) as c_int
}

unsafe extern "C" fn virt_slot_of(ctx: *mut c_void, export_name: *const c_char) -> u64 {
    let mut found = 0;
    let at = super::exports::ocerz_image_self_resolve_ex(ctx.cast(), export_name, &mut found);
    if found != 0 { at } else { 0 }
}

pub(super) unsafe fn load_disk_dylib(
    cache: *mut OcerzCache,
    install_name: *const c_char,
    loader: *mut DynImage,
    rpaths: *const RpathList,
) -> *mut DynImage {
    let by_name = super::dimg_find_by_install_name(install_name);
    if !by_name.is_null() {
        return by_name;
    }
    let mut guest_path = [0 as c_char; 1024];
    let guest_override =
        native_guest_path(install_name, guest_path.as_mut_ptr(), guest_path.len()) != 0;
    if !guest_override
        && ffi::ocerz_mode == MODE_NATIVE
        && ffi::ocerz_vdylib_have(install_name) != 0
    {
        if super::g_dimgs_n >= DYN_DIMG_MAX as c_int {
            crate::ocerz_fatal!("too many disk dylibs to load (limit %d)\n", DYN_DIMG_MAX);
            native_dl_reason(
                cstr_ptr(c"the loader holds as many images as it can"),
                ptr::null(),
            );
            return ptr::null_mut();
        }
        let mut vlen = 0;
        let vbuf = ffi::ocerz_vdylib_image(install_name, &mut vlen);
        if vbuf.is_null() || vlen == 0 {
            crate::ocerz_fatal!("cannot synthesize %s\n", install_name);
            native_dl_reason(
                cstr_ptr(c"its API database would not build an image"),
                ptr::null(),
            );
            libc::free(vbuf.cast());
            return ptr::null_mut();
        }
        let v = ptr::addr_of_mut!(super::g_dimgs)
            .cast::<DynImage>()
            .add(super::g_dimgs_n as usize);
        super::g_dimgs_n += 1;
        ptr::write_bytes(v, 0, 1);
        (*v).slice = vbuf;
        (*v).owned_buf = vbuf;
        (*v).is_virtual = 1;
        libc::snprintf(
            (*v).path.as_mut_ptr(),
            (*v).path.len(),
            cstr_ptr(c"%s"),
            install_name,
        );
        libc::snprintf(
            (*v).install_name.as_mut_ptr(),
            (*v).install_name.len(),
            cstr_ptr(c"%s"),
            install_name,
        );
        super::dimg_record_id(v);
        if super::map::map_segments(v, 0) != ffi::OCERZ_OK {
            crate::ocerz_fatal!("cannot map segments of virtual %s\n", install_name);
            native_dl_reason(
                cstr_ptr(c"its synthesized image could not be mapped"),
                ptr::null(),
            );
            super::g_dimgs_n -= 1;
            libc::free(vbuf.cast());
            return ptr::null_mut();
        }
        super::map::protect_ro_segments(v);
        ffi::ocerz_vdylib_late_fill(install_name, Some(virt_slot_of), v.cast());
        G_DIMG_SEQ = G_DIMG_SEQ.wrapping_add(1);
        (*v).seq = G_DIMG_SEQ;
        crate::ocerz_log!(
            "dynamic: registered virtual dylib %s at load_base=%#llx slide=%#llx\n",
            install_name,
            (*v).load_base as c_ulonglong,
            (*v).slide as c_ulonglong
        );
        return v;
    }
    let mut resolved = [0 as c_char; 1024];
    if guest_override {
        libc::snprintf(
            resolved.as_mut_ptr(),
            resolved.len(),
            cstr_ptr(c"%s"),
            guest_path.as_ptr(),
        );
        crate::ocerz_log!(
            "dynamic: guest override %s -> %s\n",
            install_name,
            resolved.as_ptr()
        );
    } else if expand_install_name(
        cache,
        loader,
        install_name,
        rpaths,
        resolved.as_mut_ptr(),
        resolved.len(),
    ) == 0
        || resolved[0] == b'@' as c_char
    {
        native_dl_reason(
            cstr_ptr(c"it is in no LC_RPATH directory of the images that load it"),
            ptr::null(),
        );
        return ptr::null_mut();
    }
    if ffi::ocerz_mode == MODE_NATIVE
        && install_name.read() == b'@' as c_char
        && libc::access(resolved.as_ptr(), libc::F_OK) != 0
    {
        let d = load_disk_dylib(cache, resolved.as_ptr(), loader, rpaths);
        if !d.is_null() && (*d).rpath_name[0] == 0 {
            libc::snprintf(
                (*d).rpath_name.as_mut_ptr(),
                (*d).rpath_name.len(),
                cstr_ptr(c"%s"),
                install_name,
            );
        }
        return d;
    }
    if super::eager::dep_find(cache, resolved.as_ptr()) != 0 {
        return ptr::null_mut();
    }
    let mut canon = [0 as c_char; 1024];
    if ocerz_canon_dylib_path(resolved.as_ptr(), canon.as_mut_ptr(), canon.len()) != 0
        && libc::strcmp(canon.as_ptr(), resolved.as_ptr()) != 0
    {
        libc::snprintf(
            resolved.as_mut_ptr(),
            resolved.len(),
            cstr_ptr(c"%s"),
            canon.as_ptr(),
        );
        if super::eager::dep_find(cache, resolved.as_ptr()) != 0 {
            return ptr::null_mut();
        }
    }
    let existing = super::dimg_find_by_path(resolved.as_ptr());
    if !existing.is_null() {
        return existing;
    }
    let mut fdev = 0;
    let mut fino = 0;
    if super::file_identity(resolved.as_ptr(), &mut fdev, &mut fino) != 0 {
        let existing = super::dimg_find_by_identity(fdev, fino);
        if !existing.is_null() {
            return existing;
        }
    }
    if super::g_dimgs_n >= DYN_DIMG_MAX as c_int {
        crate::ocerz_fatal!("too many disk dylibs to load (limit %d)\n", DYN_DIMG_MAX);
        native_dl_reason(
            cstr_ptr(c"the loader holds as many images as it can"),
            ptr::null(),
        );
        return ptr::null_mut();
    }
    let mut flen = 0;
    let buf = super::read_file(resolved.as_ptr(), &mut flen);
    if buf.is_null() {
        if ffi::ocerz_mode == MODE_NATIVE {
            crate::ocerz_log!(
                "dynamic: %s is not on disk, native mode has no image for it yet\n",
                resolved.as_ptr()
            );
        } else {
            crate::ocerz_fatal!("Library not loaded: %s (no such file)\n", resolved.as_ptr());
        }
        if g_ndl.active != 0 {
            let real = host_cache_real_path(resolved.as_ptr());
            if !real.is_null() {
                native_dl_reason(
                    cstr_ptr(c"%s is a native library without an API database"),
                    real,
                );
            } else {
                native_dl_reason(cstr_ptr(c"no such file"), ptr::null());
            }
        }
        return ptr::null_mut();
    }
    let slice = super::select_slice(buf, flen);
    if slice.is_null() {
        if g_ndl.active != 0 {
            crate::ocerz_log!("dynamic: %s has no x86_64 slice\n", resolved.as_ptr());
        } else {
            crate::ocerz_fatal!(
                "incompatible architecture: %s has no x86_64 slice\n",
                resolved.as_ptr()
            );
        }
        native_dl_reason(
            cstr_ptr(c"%s has no x86_64 slice, so it is a native library without an API database"),
            resolved.as_ptr(),
        );
        libc::free(buf.cast());
        return ptr::null_mut();
    }
    let d = ptr::addr_of_mut!(super::g_dimgs)
        .cast::<DynImage>()
        .add(super::g_dimgs_n as usize);
    super::g_dimgs_n += 1;
    ptr::write_bytes(d, 0, 1);
    (*d).slice = slice;
    (*d).owned_buf = buf;
    libc::snprintf(
        (*d).path.as_mut_ptr(),
        (*d).path.len(),
        cstr_ptr(c"%s"),
        resolved.as_ptr(),
    );
    libc::snprintf(
        (*d).install_name.as_mut_ptr(),
        (*d).install_name.len(),
        cstr_ptr(c"%s"),
        install_name,
    );
    (*d).file_dev = fdev;
    (*d).file_ino = fino;
    super::dimg_record_id(d);
    if super::map::map_segments(d, 0) != ffi::OCERZ_OK {
        crate::ocerz_fatal!("cannot map segments of %s\n", resolved.as_ptr());
        native_dl_reason(cstr_ptr(c"its segments could not be mapped"), ptr::null());
        super::g_dimgs_n -= 1;
        libc::free(buf.cast());
        return ptr::null_mut();
    }
    let merged = libc::malloc(core::mem::size_of::<RpathList>()).cast::<RpathList>();
    if merged.is_null() {
        crate::ocerz_fatal!("no memory for the rpaths of %s\n", resolved.as_ptr());
        return ptr::null_mut();
    }
    collect_rpaths(d, rpaths, merged);
    load_disk_deps(cache, d, merged);
    libc::free(merged.cast());
    if super::bind::apply_fixups(d, cache) != ffi::OCERZ_OK {
        crate::ocerz_fatal!("cannot apply fixups of %s\n", resolved.as_ptr());
        return ptr::null_mut();
    }
    if (*d).cf_off == 0 {
        super::bind::apply_classic_fixups(d, cache);
    }
    G_DIMG_SEQ = G_DIMG_SEQ.wrapping_add(1);
    (*d).seq = G_DIMG_SEQ;
    if ffi::ocerz_mode == MODE_CACHE {
        ffi::ocerz_dyldapi_register_image((*d).load_base, (*d).path.as_ptr());
    }
    if g_ndl.active == 0 {
        canonicalize_objc_selrefs(d);
    }
    super::map::protect_ro_segments(d);
    if !libc::getenv(cstr_ptr(c"OCERZ_DLPATH")).is_null() {
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"ocerz: DLPATH disk-dep load_base=%#llx install=%s path=%s\n"),
            (*d).load_base as c_ulonglong,
            (*d).install_name.as_ptr(),
            resolved.as_ptr(),
        );
    }
    crate::ocerz_log!(
        "dynamic: loaded disk dylib %s at load_base=%#llx slide=%#llx\n",
        resolved.as_ptr(),
        (*d).load_base as c_ulonglong,
        (*d).slide as c_ulonglong
    );
    d
}

pub(super) unsafe fn load_disk_deps(
    cache: *mut OcerzCache,
    loader: *mut DynImage,
    rpaths: *const RpathList,
) {
    let mh = (*loader).slice;
    let ncmds = rd32(mh.add(16));
    let mut lc = mh.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        let cmd = rd32(lc);
        if cmd == LC_LOAD_DYLIB
            || cmd == LC_LOAD_WEAK_DYLIB
            || cmd == LC_REEXPORT_DYLIB
            || cmd == LC_LOAD_UPWARD_DYLIB
        {
            let noff = rd32(lc.add(8));
            if noff < rd32(lc.add(4)) {
                let name = lc.add(noff as usize).cast::<c_char>();
                let dep = load_disk_dylib(cache, name, loader, rpaths);
                if dep.is_null()
                    && g_ndl.active != 0
                    && cmd != LC_LOAD_WEAK_DYLIB
                    && g_ndl.missing[0] == 0
                {
                    libc::snprintf(
                        ptr::addr_of_mut!(g_ndl.missing).cast::<c_char>(),
                        1024,
                        cstr_ptr(c"%s"),
                        name,
                    );
                    libc::snprintf(
                        ptr::addr_of_mut!(g_ndl.missing_from).cast::<c_char>(),
                        1024,
                        cstr_ptr(c"%s"),
                        (*loader).path.as_ptr(),
                    );
                    if g_ndl.reason[0] == 0 {
                        libc::snprintf(
                            ptr::addr_of_mut!(g_ndl.reason).cast::<c_char>(),
                            256,
                            cstr_ptr(c"it could not be loaded"),
                        );
                    }
                }
                if g_ndl.active != 0 && g_ndl.missing[0] == 0 {
                    g_ndl.reason[0] = 0;
                }
            }
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
}

pub(super) unsafe fn dlerror_set(fmt: *const c_char, arg: *const c_char) {
    let mut host = [0 as c_char; 1280];
    libc::snprintf(
        host.as_mut_ptr(),
        host.len(),
        fmt,
        if arg.is_null() { cstr_ptr(c"") } else { arg },
    );
    if !libc::getenv(cstr_ptr(c"OCERZ_DLPATH")).is_null() {
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"ocerz: DLERR %s\n"),
            host.as_ptr(),
        );
    }
    let mut need = libc::strlen(host.as_ptr()) as u64 + 1;
    if g_dlerror_g == 0 {
        g_dlerror_g = ffi::ocerz_map_anywhere(2048, libc::PROT_READ | libc::PROT_WRITE);
    }
    if g_dlerror_g != 0 {
        if need > 2048 {
            need = 2048;
        }
        ptr::copy_nonoverlapping(
            host.as_ptr().cast::<u8>(),
            crate::ported::dyldapi::hostmem::ocerz_g2h(g_dlerror_g).cast::<u8>(),
            need as usize,
        );
        crate::ported::dyldapi::hostmem::ocerz_g2h(g_dlerror_g)
            .cast::<u8>()
            .add(need as usize - 1)
            .write(0);
    }
}

pub(super) unsafe fn dlopen_load_image(
    cache: *mut OcerzCache,
    install_path: *const c_char,
) -> *mut DynImage {
    if super::eager::dep_find(cache, install_path) != 0 {
        return ptr::null_mut();
    }
    let existing = super::dimg_find_by_path(install_path);
    if !existing.is_null() {
        return existing;
    }
    if super::g_dimgs_n >= DYN_DIMG_MAX as c_int {
        dlerror_set(cstr_ptr(c"dlopen(%s): image registry full"), install_path);
        return ptr::null_mut();
    }
    let mut flen = 0;
    let buf = super::read_file(install_path, &mut flen);
    if buf.is_null() {
        dlerror_set(cstr_ptr(c"dlopen(%s): image not found"), install_path);
        return ptr::null_mut();
    }
    let slice = super::select_slice(buf, flen);
    if slice.is_null() {
        dlerror_set(
            cstr_ptr(c"dlopen(%s): no compatible x86_64 slice"),
            install_path,
        );
        libc::free(buf.cast());
        return ptr::null_mut();
    }
    let d = ptr::addr_of_mut!(super::g_dimgs)
        .cast::<DynImage>()
        .add(super::g_dimgs_n as usize);
    super::g_dimgs_n += 1;
    ptr::write_bytes(d, 0, 1);
    (*d).slice = slice;
    (*d).owned_buf = buf;
    libc::snprintf(
        (*d).path.as_mut_ptr(),
        (*d).path.len(),
        cstr_ptr(c"%s"),
        install_path,
    );
    libc::snprintf(
        (*d).install_name.as_mut_ptr(),
        (*d).install_name.len(),
        cstr_ptr(c"%s"),
        install_path,
    );
    super::file_identity(install_path, &mut (*d).file_dev, &mut (*d).file_ino);
    super::dimg_record_id(d);
    if super::map::map_segments(d, 0) != ffi::OCERZ_OK {
        dlerror_set(cstr_ptr(c"dlopen(%s): cannot map segments"), install_path);
        super::g_dimgs_n -= 1;
        libc::free(buf.cast());
        return ptr::null_mut();
    }
    let merged = libc::malloc(core::mem::size_of::<RpathList>()).cast::<RpathList>();
    if merged.is_null() {
        dlerror_set(
            cstr_ptr(c"dlopen(%s): no memory for its rpaths"),
            install_path,
        );
        return ptr::null_mut();
    }
    collect_rpaths(d, ptr::null(), merged);
    load_disk_deps(cache, d, merged);
    libc::free(merged.cast());
    if super::bind::apply_fixups(d, cache) != ffi::OCERZ_OK {
        dlerror_set(cstr_ptr(c"dlopen(%s): cannot apply fixups"), install_path);
        return ptr::null_mut();
    }
    if (*d).cf_off == 0 {
        super::bind::apply_classic_fixups(d, cache);
    }
    if ffi::ocerz_mode == MODE_CACHE {
        ffi::ocerz_dyldapi_register_image((*d).load_base, (*d).path.as_ptr());
    }
    canonicalize_objc_selrefs(d);
    super::map::protect_ro_segments(d);
    if !libc::getenv(cstr_ptr(c"OCERZ_DLPATH")).is_null() {
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"ocerz: DLPATH dlopen load_base=%#llx install=%s path=%s\n"),
            (*d).load_base as c_ulonglong,
            (*d).install_name.as_ptr(),
            install_path,
        );
    }
    crate::ocerz_log!(
        "dynamic: dlopen loaded %s at load_base=%#llx slide=%#llx\n",
        install_path,
        (*d).load_base as c_ulonglong,
        (*d).slide as c_ulonglong
    );
    d
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_canon_dylib_path(
    path: *const c_char,
    out: *mut c_char,
    outsz: usize,
) -> c_int {
    let mut cur = [0 as c_char; libc::PATH_MAX as usize];
    if libc::snprintf(cur.as_mut_ptr(), cur.len(), cstr_ptr(c"%s"), path) >= cur.len() as c_int {
        return 0;
    }
    for _ in 0..32 {
        let mut link = [0 as c_char; libc::PATH_MAX as usize];
        let len = libc::readlink(cur.as_ptr(), link.as_mut_ptr(), link.len() - 1);
        if len < 0 {
            break;
        }
        link[len as usize] = 0;
        let mut next = [0 as c_char; libc::PATH_MAX as usize];
        if link[0] == b'/' as c_char {
            if libc::snprintf(
                next.as_mut_ptr(),
                next.len(),
                cstr_ptr(c"%s"),
                link.as_ptr(),
            ) >= next.len() as c_int
            {
                return 0;
            }
        } else {
            let slash = libc::strrchr(cur.as_ptr(), b'/' as c_int);
            let dlen = if slash.is_null() {
                0
            } else {
                slash.offset_from(cur.as_ptr()) as usize + 1
            };
            if libc::snprintf(
                next.as_mut_ptr(),
                next.len(),
                cstr_ptr(c"%.*s%s"),
                dlen as c_int,
                cur.as_ptr(),
                link.as_ptr(),
            ) >= next.len() as c_int
            {
                return 0;
            }
        }
        ptr::copy_nonoverlapping(next.as_ptr(), cur.as_mut_ptr(), next.len());
        cur = next;
    }
    let slash = libc::strrchr(cur.as_ptr(), b'/' as c_int);
    if !slash.is_null() {
        let mut dir = [0 as c_char; libc::PATH_MAX as usize];
        let mut rdir = [0 as c_char; libc::PATH_MAX as usize];
        let mut dlen = slash.offset_from(cur.as_ptr()) as usize;
        if dlen == 0 {
            dlen = 1;
        }
        if dlen < dir.len() {
            ptr::copy_nonoverlapping(cur.as_ptr(), dir.as_mut_ptr(), dlen);
            dir[dlen] = 0;
            if !libc::realpath(dir.as_ptr(), rdir.as_mut_ptr()).is_null()
                && libc::snprintf(out, outsz, cstr_ptr(c"%s/%s"), rdir.as_ptr(), slash.add(1))
                    < outsz as c_int
            {
                return 1;
            }
        }
    }
    (libc::snprintf(out, outsz, cstr_ptr(c"%s"), cur.as_ptr()) < outsz as c_int) as c_int
}

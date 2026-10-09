//! Cache-aware dlopen, dlsym, dlclose, dlerror, and soname lookup.

use super::*;

const NDL_NEXT: u64 = u64::MAX;
const NDL_DEFAULT: u64 = u64::MAX - 1;
const NDL_SELF: u64 = u64::MAX - 2;
const NDL_MAIN_ONLY: u64 = u64::MAX - 4;
const ROSETTA_CRYPTEX: &[u8] = b"/System/Volumes/Preboot/Cryptexes/Rosetta\0";
const _: () = assert!(core::mem::size_of::<libc::pthread_mutex_t>() == 64);
const _: () = assert!(core::mem::size_of::<libc::c_long>() == 8);

const fn recursive_mutex_init() -> libc::pthread_mutex_t {
    let mut bytes = [0u8; 64];
    bytes[0] = 0xa2;
    bytes[1] = 0xab;
    bytes[2] = 0xaa;
    bytes[3] = 0x32;
    unsafe { core::mem::transmute(bytes) }
}

static mut G_LOAD_LOCK: libc::pthread_mutex_t = recursive_mutex_init();

pub(super) unsafe fn load_lock() {
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_LOAD_LOCK));
}

pub(super) unsafe fn load_unlock() {
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_LOAD_LOCK));
}

unsafe fn clear_dlerror() {
    if super::g_dlerror_g != 0 {
        crate::ported::dyldapi::hostmem::ocerz_g2h(super::g_dlerror_g)
            .cast::<u8>()
            .write(0);
    }
}

unsafe fn cache_dlopen_hit(vm: *mut OcerzVM, cmh: u64) -> u64 {
    clear_dlerror();
    let mut sp = 0;
    if super::g_run_init_ready != 0 && !super::g_run_vm.is_null() && (*vm).exited == 0 {
        let istk = ffi::ocerz_map_anywhere(DYN_STACK_SIZE, libc::PROT_READ | libc::PROT_WRITE);
        if istk != 0 {
            sp = istk.wrapping_add(DYN_STACK_SIZE).wrapping_sub(64);
        }
        ffi::ocerz_dyldapi_objc_map_one(super::g_run_vm, cmh);
        if sp != 0 && (*vm).exited == 0 {
            super::init::G_INIT_CUR_GEN = super::init::G_INIT_CUR_GEN.wrapping_add(1);
            let previous_force = super::init::G_INIT_FORCE;
            super::init::G_INIT_FORCE = 1;
            super::init::run_load_phase(super::g_run_vm, super::g_run_cache, cmh, sp, 0);
            super::init::G_INIT_FORCE = previous_force;
        }
        if sp != 0
            && super::init::G_FOUNDATION_INITED == 0
            && !super::init::init_is_done(cmh)
            && (*vm).exited == 0
        {
            let previous_restricted = super::init::g_init_dlopen_restricted;
            super::init::g_init_dlopen_restricted = 1;
            super::init::init_closure(
                super::g_run_vm,
                super::g_run_cache,
                cmh,
                ptr::addr_of!(super::g_run_init_args).cast::<u64>(),
                sp,
            );
            super::init::g_init_dlopen_restricted = previous_restricted;
        }
    }
    ffi::ocerz_dyldapi_register_cache_image(cmh);
    if sp != 0 && (*vm).exited == 0 {
        ffi::ocerz_dyldapi_notify_added(super::g_run_vm, sp);
    }
    cmh
}

unsafe fn try_soname_in_pathlist(
    list: *const c_char,
    name: *const c_char,
    out: *mut c_char,
    n: usize,
) -> c_int {
    if list.is_null() || list.read() == 0 {
        return 0;
    }
    let mut p = list;
    while p.read() != 0 {
        let colon = libc::strchr(p, b':' as c_int);
        let len = if colon.is_null() {
            libc::strlen(p)
        } else {
            colon.offset_from(p) as usize
        };
        if len > 0 && len < 900 {
            let mut cand = [0 as c_char; 1024];
            if libc::snprintf(
                cand.as_mut_ptr(),
                cand.len(),
                cstr_ptr(c"%.*s/%s"),
                len as c_int,
                p,
                name,
            ) < cand.len() as c_int
                && libc::access(cand.as_ptr(), libc::F_OK) == 0
            {
                libc::snprintf(out, n, cstr_ptr(c"%s"), cand.as_ptr());
                return 1;
            }
        }
        p = p.add(len);
        if p.read() == b':' as c_char {
            p = p.add(1);
        }
    }
    0
}

unsafe fn resolve_bare_soname(name: *const c_char, out: *mut c_char, n: usize) -> c_int {
    if name.is_null()
        || !libc::strchr(name, b'/' as c_int).is_null()
        || name.read() == b'@' as c_char
    {
        return 0;
    }
    if try_soname_in_pathlist(libc::getenv(cstr_ptr(c"DYLD_LIBRARY_PATH")), name, out, n) != 0 {
        return 1;
    }
    let fallback = libc::getenv(cstr_ptr(c"DYLD_FALLBACK_LIBRARY_PATH"));
    if !fallback.is_null() && fallback.read() != 0 {
        return try_soname_in_pathlist(fallback, name, out, n);
    }
    let home = libc::getenv(cstr_ptr(c"HOME"));
    let mut default_path = [0 as c_char; 1024];
    if !home.is_null() && home.read() != 0 {
        libc::snprintf(
            default_path.as_mut_ptr(),
            default_path.len(),
            cstr_ptr(c"%s/lib:/usr/local/lib:/usr/lib"),
            home,
        );
    } else {
        libc::snprintf(
            default_path.as_mut_ptr(),
            default_path.len(),
            cstr_ptr(c"/usr/local/lib:/usr/lib"),
        );
    }
    try_soname_in_pathlist(default_path.as_ptr(), name, out, n)
}

unsafe fn dlopen_expand_at(
    path: *const c_char,
    caller: u64,
    out: *mut c_char,
    n: usize,
) -> *const c_char {
    let caller_image = super::dimg_containing(caller);
    if libc::strncmp(path, cstr_ptr(c"@rpath/"), 7) == 0 {
        let rpaths = super::native::ndl_rpaths(caller_image);
        let mut hit: *const c_char = ptr::null();
        if !rpaths.is_null() {
            for i in 0..(*rpaths).n {
                if hit.is_null() {
                    let entry = ptr::addr_of!((*rpaths).entry)
                        .cast::<[c_char; 1024]>()
                        .add(i as usize)
                        .cast::<c_char>();
                    if libc::snprintf(out, n, cstr_ptr(c"%s/%s"), entry, path.add(7)) >= n as c_int
                    {
                        continue;
                    }
                    if super::load::rpath_supplied(super::g_run_cache, out)
                        || !super::dimg_find_by_path(out).is_null()
                        || super::eager::dep_find(super::g_run_cache, out) != 0
                    {
                        hit = out;
                    }
                }
            }
        }
        libc::free(rpaths.cast());
        return if hit.is_null() { path } else { hit };
    }
    if super::load::expand_at_prefix(caller_image, path, out, n) != 0 {
        out
    } else {
        path
    }
}

unsafe fn dlopen_inner(
    vm: *mut OcerzVM,
    mut hostpath: *const c_char,
    mode: c_int,
    caller: u64,
) -> u64 {
    let mut atpath = [0 as c_char; libc::PATH_MAX as usize];
    if !hostpath.is_null()
        && hostpath.read() == b'@' as c_char
        && libc::getenv(cstr_ptr(c"OCERZ_NO_DLOPEN_CALLER_RPATH")).is_null()
    {
        hostpath = dlopen_expand_at(hostpath, caller, atpath.as_mut_ptr(), atpath.len());
    }
    if super::g_run_cache.is_null() {
        super::load::dlerror_set(
            cstr_ptr(c"dlopen: runtime loader not initialized"),
            ptr::null(),
        );
        return 0;
    }
    if hostpath.is_null() {
        clear_dlerror();
        return if ocerz_main_mh != 0 {
            ocerz_main_mh
        } else {
            ffi::ocerz_arena_lo
        };
    }
    if ocerz_main_mh != 0 && super::g_main_hostpath[0] != 0 {
        let mut rp = [0 as c_char; libc::PATH_MAX as usize];
        if libc::strcmp(hostpath, ptr::addr_of!(super::g_main_hostpath).cast()) == 0
            || (!libc::realpath(hostpath, rp.as_mut_ptr()).is_null()
                && libc::strcmp(rp.as_ptr(), ptr::addr_of!(super::g_main_hostpath).cast()) == 0)
        {
            clear_dlerror();
            return ocerz_main_mh;
        }
    }
    let already = super::dimg_find_by_path(hostpath);
    if !already.is_null() {
        clear_dlerror();
        return (*already).load_base;
    }
    let mut cmh = super::eager::dep_find(super::g_run_cache, hostpath);
    if cmh != 0 {
        return cache_dlopen_hit(vm, cmh);
    }
    let mut canon = [0 as c_char; libc::PATH_MAX as usize];
    let mut loadpath = hostpath;
    if super::load::ocerz_canon_dylib_path(hostpath, canon.as_mut_ptr(), canon.len()) != 0
        && libc::strcmp(canon.as_ptr(), hostpath) != 0
    {
        let canonical_existing = super::dimg_find_by_path(canon.as_ptr());
        if !canonical_existing.is_null() {
            clear_dlerror();
            return (*canonical_existing).load_base;
        }
        cmh = super::eager::dep_find(super::g_run_cache, canon.as_ptr());
        if cmh != 0 {
            return cache_dlopen_hit(vm, cmh);
        }
        loadpath = canon.as_ptr();
    }
    let mut sopath = [0 as c_char; libc::PATH_MAX as usize];
    if libc::strchr(loadpath, b'/' as c_int).is_null()
        && loadpath.read() != b'@' as c_char
        && libc::access(loadpath, libc::F_OK) != 0
        && resolve_bare_soname(loadpath, sopath.as_mut_ptr(), sopath.len()) != 0
    {
        let soname_existing = super::dimg_find_by_path(sopath.as_ptr());
        if !soname_existing.is_null() {
            clear_dlerror();
            return (*soname_existing).load_base;
        }
        cmh = super::eager::dep_find(super::g_run_cache, sopath.as_ptr());
        if cmh != 0 {
            return cache_dlopen_hit(vm, cmh);
        }
        loadpath = sopath.as_ptr();
    }
    let mut cxpath = [0 as c_char; libc::PATH_MAX as usize];
    if ffi::ocerz_mode != MODE_NATIVE
        && loadpath.read() == b'/' as c_char
        && libc::access(loadpath, libc::F_OK) != 0
        && libc::snprintf(
            cxpath.as_mut_ptr(),
            cxpath.len(),
            cstr_ptr(c"%s%s"),
            ROSETTA_CRYPTEX.as_ptr().cast::<c_char>(),
            loadpath,
        ) < cxpath.len() as c_int
        && libc::access(cxpath.as_ptr(), libc::F_OK) == 0
    {
        let cryptex_existing = super::dimg_find_by_path(cxpath.as_ptr());
        if !cryptex_existing.is_null() {
            clear_dlerror();
            return (*cryptex_existing).load_base;
        }
        loadpath = cxpath.as_ptr();
    }
    if mode & 0x10 != 0 {
        super::load::dlerror_set(
            cstr_ptr(c"dlopen(%s): not already loaded (RTLD_NOLOAD)"),
            hostpath,
        );
        return 0;
    }
    let mut fdev = 0;
    let mut fino = 0;
    if super::file_identity(loadpath, &mut fdev, &mut fino) != 0 {
        let mut same = 0;
        if ocerz_main_mh != 0 && super::g_main_ino == fino && super::g_main_dev == fdev {
            same = ocerz_main_mh;
        } else {
            let identity_image = super::dimg_find_by_identity(fdev, fino);
            if !identity_image.is_null() {
                same = (*identity_image).load_base;
            }
        }
        if same != 0 {
            clear_dlerror();
            return same;
        }
    }
    let before = super::g_dimgs_n;
    let d = super::load::dlopen_load_image(super::g_run_cache, loadpath);
    if d.is_null() {
        return 0;
    }
    if super::g_run_init_ready == 0
        && !super::g_run_vm.is_null()
        && (*vm).exited == 0
        && super::g_dimgs_n > before
    {
        let mut i = super::g_dimgs_n - 1;
        while i >= before {
            let img = ptr::addr_of_mut!(super::g_dimgs)
                .cast::<DynImage>()
                .add(i as usize);
            if !libc::getenv(cstr_ptr(c"OCERZ_NO_AGXMAP")).is_null()
                && !libc::strstr(
                    (*img).path.as_ptr(),
                    cstr_ptr(c"/System/Library/Extensions/AGXMetal"),
                )
                .is_null()
            {
                i -= 1;
                continue;
            }
            ffi::ocerz_dyldapi_objc_map_one(super::g_run_vm, (*img).load_base);
            if (*vm).exited != 0 {
                break;
            }
            i -= 1;
        }
    }
    if super::g_run_init_ready != 0
        && !super::g_run_vm.is_null()
        && (*vm).exited == 0
        && super::g_dimgs_n > before
    {
        let istk = ffi::ocerz_map_anywhere(DYN_STACK_SIZE, libc::PROT_READ | libc::PROT_WRITE);
        if istk != 0 {
            let itop = istk.wrapping_add(DYN_STACK_SIZE).wrapping_sub(64);
            let mut i = super::g_dimgs_n - 1;
            while i >= before {
                let img = ptr::addr_of_mut!(super::g_dimgs)
                    .cast::<DynImage>()
                    .add(i as usize);
                super::tlv::ocerz_tlv_register_image(
                    super::g_run_vm,
                    super::g_run_cache,
                    (*img).load_base,
                    itop,
                );
                if (*vm).exited != 0 {
                    break;
                }
                i -= 1;
            }
            i = super::g_dimgs_n - 1;
            while i >= before && (*vm).exited == 0 {
                let img = ptr::addr_of_mut!(super::g_dimgs)
                    .cast::<DynImage>()
                    .add(i as usize);
                if libc::getenv(cstr_ptr(c"OCERZ_NO_AGXMAP")).is_null()
                    || libc::strstr(
                        (*img).path.as_ptr(),
                        cstr_ptr(c"/System/Library/Extensions/AGXMetal"),
                    )
                    .is_null()
                {
                    ffi::ocerz_dyldapi_objc_map_one(super::g_run_vm, (*img).load_base);
                }
                i -= 1;
            }
            if ffi::ocerz_mode == MODE_CACHE && (*vm).exited == 0 {
                ffi::ocerz_dyldapi_notify_added(super::g_run_vm, itop);
            }
            super::init::init_closure(
                super::g_run_vm,
                super::g_run_cache,
                (*d).load_base,
                ptr::addr_of!(super::g_run_init_args).cast::<u64>(),
                itop,
            );
        }
    }
    clear_dlerror();
    (*d).load_base
}

unsafe fn dlopen_piggyback(vm: *mut OcerzVM, path: *const c_char) {
    static mut PIG_KEY: [c_char; 256] = [0; 256];
    static mut PIG_LIB: [c_char; 1024] = [0; 1024];
    static mut PIG: c_int = -1;
    static mut PIG_DONE: c_int = 0;
    let pig_key = ptr::addr_of_mut!(PIG_KEY).cast::<c_char>();
    let pig_lib = ptr::addr_of_mut!(PIG_LIB).cast::<c_char>();
    if PIG < 0 {
        let env = libc::getenv(cstr_ptr(c"OCERZ_DLOPEN_PIGGYBACK"));
        PIG = 0;
        if !env.is_null() {
            let colon = libc::strchr(env, b':' as c_int);
            if !colon.is_null() {
                let key_len = colon.offset_from(env) as usize;
                let lib_len = libc::strlen(colon.add(1));
                if key_len < 256 && lib_len < 1024 {
                    ptr::copy_nonoverlapping(env, pig_key, key_len);
                    pig_key.add(key_len).write(0);
                    libc::strcpy(pig_lib, colon.add(1));
                    PIG = 1;
                }
            }
        }
    }
    if PIG != 0
        && PIG_DONE == 0
        && !path.is_null()
        && !libc::strstr(path, ptr::addr_of!(PIG_KEY).cast::<c_char>()).is_null()
    {
        PIG_DONE = 1;
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"ocerz: PIGGYBACK[%d] after \"%s\": dlopen \"%s\"\n"),
            libc::getpid(),
            path,
            ptr::addr_of!(PIG_LIB).cast::<c_char>(),
        );
        let pb = dlopen_inner(vm, ptr::addr_of!(PIG_LIB).cast::<c_char>(), 2, 0);
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"ocerz: PIGGYBACK[%d] -> %#llx\n"),
            libc::getpid(),
            pb as c_ulonglong,
        );
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dlopen(
    vm: *mut OcerzVM,
    hostpath: *const c_char,
    mode: c_int,
) -> u64 {
    ocerz_dlopen_from(vm, hostpath, mode, 0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dlopen_from(
    vm: *mut OcerzVM,
    hostpath: *const c_char,
    mode: c_int,
    caller: u64,
) -> u64 {
    if !libc::getenv(cstr_ptr(c"OCERZ_DLOPENLOG")).is_null() {
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"ocerz: DLOPEN \"%s\" mode=%#x caller=%#llx\n"),
            if hostpath.is_null() {
                cstr_ptr(c"(null)")
            } else {
                hostpath
            },
            mode,
            caller as c_ulonglong,
        );
    }
    load_lock();
    let result = dlopen_inner(vm, hostpath, mode, caller);
    if result != 0 && (*vm).exited == 0 {
        dlopen_piggyback(vm, hostpath);
    }
    super::map::protect_ro_flush();
    load_unlock();
    if !libc::getenv(cstr_ptr(c"OCERZ_DLOPENLOG")).is_null() {
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"ocerz: DLOPEN[%d] \"%s\" -> %#llx%s%s\n"),
            libc::getpid(),
            if hostpath.is_null() {
                cstr_ptr(c"(null)")
            } else {
                hostpath
            },
            result as c_ulonglong,
            if result == 0 && super::g_dlerror_g != 0 {
                cstr_ptr(c" err=")
            } else {
                cstr_ptr(c"")
            },
            if result == 0 && super::g_dlerror_g != 0 {
                crate::ported::dyldapi::hostmem::ocerz_g2h(super::g_dlerror_g).cast()
            } else {
                cstr_ptr(c"")
            },
        );
    }
    result
}

pub(super) unsafe fn image_symtab_resolve(img: *mut DynImage, sym: *const c_char) -> u64 {
    let mh = (*img).slice;
    let ncmds = rd32(mh.add(16));
    let mut lc = mh.add(core::mem::size_of::<MachHeader64>());
    let mut symoff = 0;
    let mut nsyms = 0;
    let mut stroff = 0;
    let mut strsize = 0;
    for _ in 0..ncmds {
        if rd32(lc) == LC_SYMTAB {
            symoff = rd32(lc.add(8));
            nsyms = rd32(lc.add(12));
            stroff = rd32(lc.add(16));
            strsize = rd32(lc.add(20));
            break;
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
    if symoff == 0 || nsyms == 0 || stroff == 0 {
        return 0;
    }
    let nl = mh.add(symoff as usize);
    let strs = mh.add(stroff as usize).cast::<c_char>();
    for i in 0..nsyms {
        let entry = nl.add((i as u64 * 16) as usize);
        let strx = rd32(entry);
        let ntype = *entry.add(4);
        if strx == 0 || strx >= strsize || ntype & 0x0e != 0x0e || ntype & 0x01 == 0 {
            continue;
        }
        if libc::strcmp(strs.add(strx as usize), sym) == 0 {
            return rd64(entry.add(8)).wrapping_add((*img).slide as u64);
        }
    }
    0
}

pub(super) unsafe fn main_image_resolve(sym: *const c_char) -> u64 {
    if super::g_main_dimg_valid == 0 {
        return 0;
    }
    let value =
        super::exports::ocerz_image_self_resolve(ptr::addr_of_mut!(super::g_main_dimg), sym);
    if value != 0 {
        value
    } else {
        image_symtab_resolve(ptr::addr_of_mut!(super::g_main_dimg), sym)
    }
}

pub(super) unsafe fn main_image_resolve_ex(sym: *const c_char, found: *mut c_int) -> u64 {
    *found = 0;
    if super::g_main_dimg_valid == 0 {
        return 0;
    }
    let value = super::exports::ocerz_image_self_resolve_ex(
        ptr::addr_of_mut!(super::g_main_dimg),
        sym,
        found,
    );
    if *found != 0 {
        return value;
    }
    let value = image_symtab_resolve(ptr::addr_of_mut!(super::g_main_dimg), sym);
    *found = (value != 0) as c_int;
    value
}

unsafe fn dlsym_dependents(root: u64, sym: *const c_char) -> u64 {
    let mut queue = [0u64; 1024];
    let mut head = 0;
    let mut tail = 1;
    queue[0] = root;
    while head < tail {
        let mh = queue[head];
        head += 1;
        if mh != root {
            if !super::g_run_cache.is_null()
                && ffi::ocerz_cache_has_image(super::g_run_cache, mh) != 0
            {
                let mut found = 0;
                let value = ffi::ocerz_cache_dlsym_image(super::g_run_cache, mh, sym, &mut found);
                if found != 0 {
                    return value;
                }
                continue;
            }
            for i in 0..super::g_dimgs_n {
                let img = ptr::addr_of_mut!(super::g_dimgs)
                    .cast::<DynImage>()
                    .add(i as usize);
                if (*img).load_base == mh {
                    let mut found = 0;
                    let value = super::exports::ocerz_image_self_resolve_ex(img, sym, &mut found);
                    if found != 0 {
                        return value;
                    }
                    break;
                }
            }
        }
        let h = crate::ported::dyldapi::hostmem::ocerz_g2h(mh).cast::<u8>();
        if rd32(h) != MH_MAGIC_64 {
            continue;
        }
        let ncmds = rd32(h.add(16));
        let mut lc = h.add(core::mem::size_of::<MachHeader64>());
        for _ in 0..ncmds {
            let noff = rd32(lc.add(8));
            if super::init::dylib_lc_is_init_dep(lc) && noff < rd32(lc.add(4)) {
                let dep = super::eager::dep_mh(super::g_run_cache, lc.add(noff as usize).cast());
                let mut k = 0;
                while k < tail && queue[k] != dep {
                    k += 1;
                }
                if dep != 0 && k == tail && tail < queue.len() {
                    queue[tail] = dep;
                    tail += 1;
                }
            }
            lc = lc.add(rd32(lc.add(4)) as usize);
        }
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dlsym(handle: u64, sym: *const c_char) -> u64 {
    if sym.is_null() || sym.read() == 0 {
        return 0;
    }
    let mut buf = [0 as c_char; 1024];
    buf[0] = b'_' as c_char;
    libc::snprintf(buf.as_mut_ptr().add(1), buf.len() - 1, cstr_ptr(c"%s"), sym);
    if handle == NDL_DEFAULT || handle == NDL_SELF {
        let mut value = main_image_resolve(buf.as_ptr());
        if value == 0 {
            value = super::bind::disk_flat_resolve(buf.as_ptr());
        }
        if value != 0 {
            return value;
        }
        if !super::g_run_cache.is_null() {
            value = ffi::ocerz_cache_resolve(super::g_run_cache, buf.as_ptr());
        }
        return value;
    }
    if handle == NDL_MAIN_ONLY {
        return main_image_resolve(buf.as_ptr());
    }
    if handle == NDL_NEXT {
        let mut value = main_image_resolve(buf.as_ptr());
        if value == 0 {
            value = super::bind::disk_flat_resolve(buf.as_ptr());
        }
        if value != 0 {
            return value;
        }
        if !super::g_run_cache.is_null() {
            value = ffi::ocerz_cache_resolve(super::g_run_cache, buf.as_ptr());
        }
        return value;
    }
    for i in 0..super::g_dimgs_n {
        let img = ptr::addr_of_mut!(super::g_dimgs)
            .cast::<DynImage>()
            .add(i as usize);
        if (*img).load_base == handle {
            let mut value = super::exports::ocerz_image_self_resolve(img, buf.as_ptr());
            if value == 0 {
                value = image_symtab_resolve(img, buf.as_ptr());
            }
            if value == 0 && libc::getenv(cstr_ptr(c"OCERZ_NO_DLSYM_DEPS")).is_null() {
                value = dlsym_dependents(handle, buf.as_ptr());
            }
            return value;
        }
    }
    if !super::g_run_cache.is_null() && ffi::ocerz_cache_has_image(super::g_run_cache, handle) != 0
    {
        return ffi::ocerz_cache_dlsym_image(
            super::g_run_cache,
            handle,
            buf.as_ptr(),
            ptr::null_mut(),
        );
    }
    if !super::g_run_cache.is_null() {
        let value = ffi::ocerz_cache_resolve(super::g_run_cache, buf.as_ptr());
        if value != 0 {
            return value;
        }
    }
    super::bind::disk_flat_resolve(buf.as_ptr())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dlclose(_handle: u64) -> c_int {
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dlerror() -> u64 {
    if super::g_dlerror_g == 0 {
        return 0;
    }
    let s = crate::ported::dyldapi::hostmem::ocerz_g2h(super::g_dlerror_g).cast::<c_char>();
    if s.read() == 0 { 0 } else { super::g_dlerror_g }
}

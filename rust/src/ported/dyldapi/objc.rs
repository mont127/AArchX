//! Objective-C callbacks, optimization-table lookup, selector canonicalization, and load methods.

use core::ffi::{c_char, c_int};
use core::mem;
use core::ptr;

use crate::ffi::{
    OCERZ_RAX, OCERZ_RDX, OCERZ_RSI, OCERZ_RSP, OCERZ_STEP_OK as STEP_OK_RAW, OcerzCPU, OcerzVM,
    ocerz_cache_image_addr, ocerz_map_anywhere, ocerz_vm_call,
};

use super::closure::{
    cache_find_path, cache_path_for_mh, find_section_any, find_section_sz, image_closure_walk,
};
use super::hostmem::{ocerz_g2h, ocerz_h2g, ocerz_ld, ocerz_st};
use super::{
    cstr_ptr, g_block_scratch, g_cache, g_cache_start, g_closure_cap, g_closure_hash_mask,
    g_closure_mh, g_closure_n, g_clsopt, g_headeropt_ro, g_headeropt_rw, g_objc_dlopen_mapped,
    g_objc_dlopen_mapped_n, g_objc_init_cb, g_objc_init_info, g_objc_make_mutable,
    g_objc_mapped_cb, g_protoopt, g_sel_pool, g_selopt,
};

const BULK_CB_MAX: usize = 16;
const PRELOAD_MAX: usize = 8192;
const CLSOPT_MAX_HITS: usize = 16;
const PROT_READ: c_int = libc::PROT_READ;
const PROT_WRITE: c_int = libc::PROT_WRITE;
const OCERZ_STEP_OK: c_int = STEP_OK_RAW as c_int;

static mut g_bulk_cb: [u64; BULK_CB_MAX] = [0; BULK_CB_MAX];
static mut g_bulk_n: c_int = 0;
static mut g_bulk_reported: c_int = 0;
static mut g_bulk_lock: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
static mut g_objc_preload_added: c_int = 0;
static mut g_selidx: *mut *const c_char = ptr::null_mut();
static mut g_selidx_cap: u32 = 0;
static mut g_selidx_lock: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

unsafe fn api_return(cpu: *mut OcerzCPU, result: u64) {
    unsafe { super::dispatch::api_return(cpu, result) }
}

unsafe fn bulk_call(vm: *mut OcerzVM, func: u64, from: c_int, to: c_int, stack_top: u64) {
    unsafe {
        let n = to - from;
        if func == 0 || n <= 0 || stack_top == 0 || (*vm).exited != 0 {
            return;
        }
        let bytes = n as u64 * 16;
        let mhs = ocerz_map_anywhere(bytes, PROT_READ | PROT_WRITE);
        if mhs == 0 {
            return;
        }
        let paths = mhs + n as u64 * 8;
        for k in 0..n {
            let mh = *g_closure_mh.add((from + k) as usize);
            let path = cache_path_for_mh(g_cache, mh);
            ocerz_st(mhs + k as u64 * 8, 8, mh);
            ocerz_st(
                paths + k as u64 * 8,
                8,
                if !path.is_null() {
                    ocerz_h2g(path.cast())
                } else {
                    super::closure::disk_gpath_for_mh(mh)
                },
            );
        }
        crate::ocerz_log!(
            "dyldapi: bulk_image_loads driving %d image(s) to cb=%#llx\n",
            n,
            func as libc::c_ulonglong
        );
        let args = [n as u64, mhs, paths];
        ocerz_vm_call(vm, func, args.as_ptr(), 3, stack_top);
        crate::ffi::ocerz_unmap(mhs, bytes);
    }
}

unsafe fn bulk_take_new(cbs: *mut u64, nc: *mut c_int, from: *mut c_int, to: *mut c_int) -> bool {
    unsafe {
        libc::pthread_mutex_lock(ptr::addr_of_mut!(g_bulk_lock));
        *nc = g_bulk_n;
        ptr::copy_nonoverlapping(ptr::addr_of!(g_bulk_cb).cast::<u64>(), cbs, BULK_CB_MAX);
        *from = g_bulk_reported;
        *to = g_closure_n;
        if g_bulk_reported < g_closure_n {
            g_bulk_reported = g_closure_n;
        }
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(g_bulk_lock));
        *nc > 0 && *to > *from
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyldapi_notify_added(vm: *mut OcerzVM, stack_top: u64) {
    unsafe {
        let mut cbs = [0u64; BULK_CB_MAX];
        let mut nc = 0;
        let mut from = 0;
        let mut to = 0;
        static mut off: c_int = -1;
        if off < 0 {
            off = (!libc::getenv(cstr_ptr(c"OCERZ_NO_BULK_NOTIFY")).is_null()) as c_int;
        }
        if off != 0 || vm.is_null() || !bulk_take_new(cbs.as_mut_ptr(), &mut nc, &mut from, &mut to)
        {
            return;
        }
        for i in 0..nc {
            if (*vm).exited != 0 {
                break;
            }
            bulk_call(vm, cbs[i as usize], from, to, stack_top);
        }
    }
}

pub(crate) unsafe fn api_register_for_bulk_image_loads(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
) -> c_int {
    unsafe {
        let func = (*cpu).gpr[OCERZ_RSI as usize];
        let rsp = (*cpu).gpr[OCERZ_RSP as usize];
        let caller_ret = ocerz_ld(rsp, 8);
        let ret_rsp = rsp + 8;
        ocerz_dyldapi_notify_added(vm, ret_rsp);
        let mut n = 0;
        if func != 0 {
            libc::pthread_mutex_lock(ptr::addr_of_mut!(g_bulk_lock));
            if g_bulk_n < BULK_CB_MAX as c_int {
                *ptr::addr_of_mut!(g_bulk_cb)
                    .cast::<u64>()
                    .add(g_bulk_n as usize) = func;
                g_bulk_n += 1;
            }
            n = g_bulk_reported;
            libc::pthread_mutex_unlock(ptr::addr_of_mut!(g_bulk_lock));
        }
        bulk_call(vm, func, 0, n, ret_rsp);
        (*cpu).rip = caller_ret;
        (*cpu).gpr[OCERZ_RSP as usize] = ret_rsp;
        (*cpu).gpr[OCERZ_RAX as usize] = 0;
        OCERZ_STEP_OK
    }
}

unsafe fn objc_preload_append(
    mhs: *mut u64,
    paths: *mut u64,
    iis: *mut u64,
    np: *mut c_int,
    max: c_int,
) {
    unsafe {
        let spec = libc::getenv(cstr_ptr(c"OCERZ_PRELOAD_OBJC"));
        if spec.is_null() || g_cache.is_null() {
            return;
        }
        static mut visited: [u64; 8192] = [0; 8192];
        let mut vn = 0;
        let mut buf = [0 as c_char; 2048];
        libc::snprintf(buf.as_mut_ptr(), buf.len(), cstr_ptr(c"%s"), spec);
        let mut save = ptr::null_mut();
        let mut tok = libc::strtok_r(buf.as_mut_ptr(), cstr_ptr(c","), &mut save);
        while !tok.is_null() {
            let all_cats = libc::strcmp(tok, cstr_ptr(c"@cat")) == 0;
            for i in 0..(*g_cache).images_cnt {
                if *np >= max {
                    break;
                }
                let mut p = ptr::null();
                let mh = ocerz_cache_image_addr(g_cache, i, &mut p);
                if mh == 0 || p.is_null() {
                    continue;
                }
                if all_cats {
                    if find_section_any(mh, cstr_ptr(c"__objc_catlist")) == 0 {
                        continue;
                    }
                } else if libc::strstr(p, tok).is_null() {
                    continue;
                }
                let mut queue = [0u64; 8192];
                let mut qn = 1usize;
                queue[0] = mh;
                while qn > 0 && *np < max {
                    qn -= 1;
                    let cur = queue[qn];
                    let mut seen = false;
                    for k in 0..vn {
                        if *ptr::addr_of!(visited).cast::<u64>().add(k) == cur {
                            seen = true;
                            break;
                        }
                    }
                    if seen {
                        continue;
                    }
                    if vn < 8192 {
                        *ptr::addr_of_mut!(visited).cast::<u64>().add(vn) = cur;
                        vn += 1;
                    }
                    let h = ocerz_g2h(cur).cast::<super::macho::MachHeader64>();
                    if (*h).magic != super::macho::MH_MAGIC_64 {
                        continue;
                    }
                    let mut dup = false;
                    for k in 0..*np {
                        if *mhs.add(k as usize) == cur {
                            dup = true;
                            break;
                        }
                    }
                    let ii = if dup {
                        0
                    } else {
                        find_section_any(cur, cstr_ptr(c"__objc_imageinfo"))
                    };
                    if ii != 0 {
                        let cp = cache_path_for_mh(g_cache, cur);
                        *mhs.add(*np as usize) = cur;
                        *paths.add(*np as usize) = if cp.is_null() {
                            0
                        } else {
                            ocerz_h2g(cp.cast())
                        };
                        *iis.add(*np as usize) = ii;
                        *np += 1;
                    }
                    let mut lc = h.add(1).cast::<u8>();
                    for _ in 0..(*h).ncmds {
                        let l = lc.cast::<super::macho::LoadCommand>();
                        if (*l).cmd == super::macho::LC_LOAD_DYLIB
                            || (*l).cmd == super::macho::LC_LOAD_WEAK_DYLIB
                            || (*l).cmd == super::macho::LC_REEXPORT_DYLIB
                            || (*l).cmd == super::macho::LC_LOAD_UPWARD_DYLIB
                        {
                            let noff = ptr::read_unaligned(lc.add(8).cast::<u32>());
                            if noff < (*l).cmdsize && qn < queue.len() {
                                let dep = cache_find_path(g_cache, lc.add(noff as usize).cast());
                                if dep != 0 {
                                    queue[qn] = dep;
                                    qn += 1;
                                }
                            }
                        }
                        lc = lc.add((*l).cmdsize as usize);
                    }
                }
            }
            tok = libc::strtok_r(ptr::null_mut(), cstr_ptr(c","), &mut save);
        }
    }
}

pub(crate) unsafe fn api_objc_register_callbacks(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let cb = (*cpu).gpr[OCERZ_RSI as usize];
        let mapped = if cb != 0 { ocerz_ld(cb + 0x08, 8) } else { 0 };
        if cb != 0 && !libc::getenv(cstr_ptr(c"OCERZ_OBJCCB")).is_null() {
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(c"ocerz: OBJCCB struct@%#llx version=%llu mapped=%#llx init=%#llx unmapped=%#llx patches=%#llx\n"),
                cb as libc::c_ulonglong,
                ocerz_ld(cb, 8) as libc::c_ulonglong,
                ocerz_ld(cb + 0x08, 8) as libc::c_ulonglong,
                ocerz_ld(cb + 0x10, 8) as libc::c_ulonglong,
                ocerz_ld(cb + 0x18, 8) as libc::c_ulonglong,
                ocerz_ld(cb + 0x20, 8) as libc::c_ulonglong,
            );
        }
        if mapped == 0 || g_cache.is_null() {
            api_return(cpu, 0);
            return OCERZ_STEP_OK;
        }
        g_objc_mapped_cb = mapped;
        g_objc_init_cb = if cb != 0 { ocerz_ld(cb + 0x10, 8) } else { 0 };
        static mut mhs: [u64; PRELOAD_MAX] = [0; PRELOAD_MAX];
        static mut paths: [u64; PRELOAD_MAX] = [0; PRELOAD_MAX];
        static mut iis: [u64; PRELOAD_MAX] = [0; PRELOAD_MAX];
        let mut n = 0;
        for i in 0..g_closure_n {
            if n >= PRELOAD_MAX as c_int {
                break;
            }
            let mh = *g_closure_mh.add(i as usize);
            let ii = find_section_any(mh, cstr_ptr(c"__objc_imageinfo"));
            if ii == 0 {
                continue;
            }
            let path = cache_path_for_mh(g_cache, mh);
            *ptr::addr_of_mut!(mhs).cast::<u64>().add(n as usize) = mh;
            *ptr::addr_of_mut!(paths).cast::<u64>().add(n as usize) = if path.is_null() {
                0
            } else {
                ocerz_h2g(path.cast())
            };
            *ptr::addr_of_mut!(iis).cast::<u64>().add(n as usize) = ii;
            n += 1;
        }
        let before = n;
        objc_preload_append(
            ptr::addr_of_mut!(mhs).cast(),
            ptr::addr_of_mut!(paths).cast(),
            ptr::addr_of_mut!(iis).cast(),
            &mut n,
            PRELOAD_MAX as c_int,
        );
        g_objc_preload_added = n - before;
        if g_objc_preload_added != 0 && !libc::getenv(cstr_ptr(c"OCERZ_OBJCLOG")).is_null() {
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(c"ocerz: PRELOAD_OBJC added %d image(s) to the initial batch (%d total)\n"),
                g_objc_preload_added,
                n,
            );
        }
        if n == 0 {
            api_return(cpu, 0);
            return OCERZ_STEP_OK;
        }
        let infos = ocerz_map_anywhere(n as u64 * 0x20, PROT_READ | PROT_WRITE);
        if infos == 0 {
            api_return(cpu, 0);
            return OCERZ_STEP_OK;
        }
        for k in 0..n {
            let e = infos + k as u64 * 0x20;
            let mh = *ptr::addr_of!(mhs).cast::<u64>().add(k as usize);
            ocerz_st(e, 8, mh);
            ocerz_st(
                e + 8,
                8,
                *ptr::addr_of!(paths).cast::<u64>().add(k as usize),
            );
            ocerz_st(e + 16, 8, mh);
            ocerz_st(e + 24, 8, *ptr::addr_of!(iis).cast::<u64>().add(k as usize));
        }
        let rsp = (*cpu).gpr[OCERZ_RSP as usize];
        let caller_ret = ocerz_ld(rsp, 8);
        let ret_rsp = rsp + 8;
        crate::ocerz_log!(
            "dyldapi: objc map_images driving %d images (infos=%#llx)\n",
            n,
            infos as libc::c_ulonglong
        );
        for k in 0..n.min(3) {
            hinfo_diag(
                cstr_ptr(c"pre-init"),
                *ptr::addr_of!(mhs).cast::<u64>().add(k as usize),
            );
        }
        let args = [n as u64, infos, g_objc_make_mutable];
        ocerz_vm_call(vm, mapped, args.as_ptr(), 3, ret_rsp);
        if !libc::getenv(cstr_ptr(c"OCERZ_HINFO")).is_null() {
            let mut loaded = 0;
            for k in 0..n {
                let idx = hinfo_ro_index(*ptr::addr_of!(mhs).cast::<u64>().add(k as usize));
                if idx >= 0 && objc_index_loaded(idx as u32) != 0 {
                    loaded += 1;
                }
            }
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(c"ocerz: HINFO initial batch n=%d isLoaded-after=%d\n"),
                n,
                loaded,
            );
        }
        methdump_diag(cstr_ptr(c"post-initial"));
        let want = libc::getenv(cstr_ptr(c"OCERZ_HINFO_PRESET"));
        if !want.is_null() && g_headeropt_ro != 0 && g_headeropt_rw != 0 {
            let rocnt = ocerz_ld(g_headeropt_ro, 4) as u32;
            let roent = ocerz_ld(g_headeropt_ro + 4, 4) as u32;
            let rwcnt = ocerz_ld(g_headeropt_rw, 4) as u32;
            let rwent = ocerz_ld(g_headeropt_rw + 4, 4) as u32;
            let mut set = 0;
            for i in 0..rocnt.min(rwcnt) {
                if roent == 0 || rwent == 0 {
                    break;
                }
                let e = g_headeropt_ro + 8 + i as u64 * roent as u64;
                let mh = (e as i64).wrapping_add(ocerz_ld(e, 8) as i64) as u64;
                let p = cache_path_for_mh(g_cache, mh);
                if p.is_null() || libc::strstr(p, want).is_null() {
                    continue;
                }
                let rwe = g_headeropt_rw + 8 + i as u64 * rwent as u64;
                ocerz_st(rwe, 8, ocerz_ld(rwe, 8) | 1);
                libc::fprintf(
                    crate::log::stderr(),
                    cstr_ptr(c"ocerz: HINFO PRESET idx=%u %s\n"),
                    i,
                    p,
                );
                set += 1;
            }
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(c"ocerz: HINFO PRESET set %d image(s)\n"),
                set,
            );
        }
        for k in 0..n {
            if g_objc_dlopen_mapped_n < g_closure_cap {
                *g_objc_dlopen_mapped.add(g_objc_dlopen_mapped_n as usize) =
                    *ptr::addr_of!(mhs).cast::<u64>().add(k as usize);
                g_objc_dlopen_mapped_n += 1;
            }
        }
        (*cpu).rip = caller_ret;
        (*cpu).gpr[OCERZ_RSP as usize] = ret_rsp;
        (*cpu).gpr[OCERZ_RAX as usize] = 0;
        OCERZ_STEP_OK
    }
}

unsafe fn objc_image_already_loaded(mh: u64) -> bool {
    unsafe {
        if g_objc_dlopen_mapped.is_null() {
            return false;
        }
        for i in 0..g_objc_dlopen_mapped_n {
            if *g_objc_dlopen_mapped.add(i as usize) == mh {
                return true;
            }
        }
        false
    }
}

unsafe fn hinfo_ro_index(mh: u64) -> c_int {
    unsafe {
        if g_headeropt_ro == 0 {
            return -1;
        }
        let count = ocerz_ld(g_headeropt_ro, 4) as u32;
        let entsize = ocerz_ld(g_headeropt_ro + 4, 4) as u32;
        if entsize == 0 || entsize > 0x40 || count > 100000 {
            return -1;
        }
        for i in 0..count {
            let e = g_headeropt_ro + 8 + i as u64 * entsize as u64;
            let off = ocerz_ld(e, 8) as i64;
            if (e as i64).wrapping_add(off) as u64 == mh {
                return i as c_int;
            }
        }
        -1
    }
}

unsafe fn objc_index_loaded(idx: u32) -> u64 {
    unsafe {
        if g_headeropt_rw == 0 {
            return 1;
        }
        let count = ocerz_ld(g_headeropt_rw, 4) as u32;
        let entsize = ocerz_ld(g_headeropt_rw + 4, 4) as u32;
        if entsize == 0 || entsize > 0x40 || idx >= count {
            return 0;
        }
        ocerz_ld(g_headeropt_rw + 8 + idx as u64 * entsize as u64, 8) & 1
    }
}

unsafe fn hinfo_diag(tag: *const c_char, mh: u64) {
    unsafe {
        if libc::getenv(cstr_ptr(c"OCERZ_HINFO")).is_null() {
            return;
        }
        let idx = hinfo_ro_index(mh);
        let p = if !g_cache.is_null() {
            cache_path_for_mh(g_cache, mh)
        } else {
            ptr::null()
        };
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"ocerz: HINFO %-10s mh=%#llx roidx=%d isLoaded=%llu %s\n"),
            tag,
            mh as libc::c_ulonglong,
            idx,
            if idx >= 0 {
                objc_index_loaded(idx as u32) as libc::c_ulonglong
            } else {
                9
            },
            if p.is_null() { cstr_ptr(c"?") } else { p },
        );
    }
}

unsafe fn objc_drive_map_images(vm: *mut OcerzVM, mhs: *const u64, n: c_int) {
    unsafe {
        if g_objc_mapped_cb == 0 || n <= 0 {
            return;
        }
        let infos = ocerz_map_anywhere(n as u64 * 0x20, PROT_READ | PROT_WRITE);
        let istk = ocerz_map_anywhere(0x100000, PROT_READ | PROT_WRITE);
        if infos == 0 || istk == 0 {
            return;
        }
        for k in 0..n {
            let e = infos + k as u64 * 0x20;
            let mh = *mhs.add(k as usize);
            let path = cache_path_for_mh(g_cache, mh);
            ocerz_st(e, 8, mh);
            ocerz_st(
                e + 8,
                8,
                if path.is_null() {
                    0
                } else {
                    ocerz_h2g(path.cast())
                },
            );
            ocerz_st(e + 16, 8, mh);
            ocerz_st(
                e + 24,
                8,
                find_section_any(mh, cstr_ptr(c"__objc_imageinfo")),
            );
        }
        if !libc::getenv(cstr_ptr(c"OCERZ_DLOPENLOG")).is_null() {
            for k in 0..n {
                let p = cache_path_for_mh(g_cache, *mhs.add(k as usize));
                libc::fprintf(
                    crate::log::stderr(),
                    cstr_ptr(c"ocerz: MAPIMG %s\n"),
                    if p.is_null() { cstr_ptr(c"?") } else { p },
                );
            }
        }
        let p = cache_path_for_mh(g_cache, *mhs);
        crate::ocerz_log!(
            "dyldapi: objc map_images (dlopen) %d image(s), first %s\n",
            n,
            if p.is_null() { cstr_ptr(c"?") } else { p }
        );
        for k in 0..n {
            hinfo_diag(cstr_ptr(c"pre-dlopen"), *mhs.add(k as usize));
        }
        methdump_diag(cstr_ptr(c"pre-dlopen"));
        let args = [n as u64, infos, g_objc_make_mutable];
        ocerz_vm_call(vm, g_objc_mapped_cb, args.as_ptr(), 3, istk + 0x100000 - 64);
        for k in 0..n {
            hinfo_diag(cstr_ptr(c"post-dlopen"), *mhs.add(k as usize));
        }
        methdump_diag(cstr_ptr(c"post-dlopen"));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyldapi_objc_map_one(vm: *mut OcerzVM, mh: u64) {
    unsafe {
        if !libc::getenv(cstr_ptr(c"OCERZ_OBJCLOG")).is_null() {
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(c"ocerz: OBJCMAP1 mh=%#llx cb=%d cache=%d already=%d\n"),
                mh as libc::c_ulonglong,
                (g_objc_mapped_cb != 0) as c_int,
                (!g_cache.is_null()) as c_int,
                if mh != 0 {
                    objc_image_already_loaded(mh) as c_int
                } else {
                    -1
                },
            );
        }
        if g_objc_mapped_cb == 0 || mh == 0 || g_cache.is_null() || objc_image_already_loaded(mh) {
            return;
        }
        let walk = libc::calloc(g_closure_cap as usize, mem::size_of::<u64>()).cast::<u64>();
        let seen =
            libc::calloc(g_closure_hash_mask as usize + 1, mem::size_of::<u64>()).cast::<u64>();
        let batch = libc::calloc(g_closure_cap as usize, mem::size_of::<u64>()).cast::<u64>();
        if walk.is_null() || seen.is_null() || batch.is_null() {
            libc::free(walk.cast());
            libc::free(seen.cast());
            libc::free(batch.cast());
            return;
        }
        let wn = image_closure_walk(
            g_cache,
            mh,
            walk,
            0,
            g_closure_cap,
            seen,
            g_closure_hash_mask,
        );
        let mut bn = 0;
        for i in 0..wn {
            let cur = *walk.add(i as usize);
            if !objc_image_already_loaded(cur)
                && find_section_any(cur, cstr_ptr(c"__objc_imageinfo")) != 0
            {
                *batch.add(bn as usize) = cur;
                bn += 1;
            }
        }
        libc::free(walk.cast());
        libc::free(seen.cast());
        if !libc::getenv(cstr_ptr(c"OCERZ_OBJCLOG")).is_null() {
            let p = cache_path_for_mh(g_cache, mh);
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(c"ocerz: OBJCMAP mh=%#llx batch=%d path=%s\n"),
                mh as libc::c_ulonglong,
                bn,
                if p.is_null() { cstr_ptr(c"?") } else { p },
            );
            for k in 0..bn {
                let p = cache_path_for_mh(g_cache, *batch.add(k as usize));
                libc::fprintf(
                    crate::log::stderr(),
                    cstr_ptr(c"ocerz:   batch[%d] mh=%#llx %s\n"),
                    k,
                    *batch.add(k as usize) as libc::c_ulonglong,
                    if p.is_null() { cstr_ptr(c"?") } else { p },
                );
            }
        }
        if bn == 0 {
            libc::free(batch.cast());
            return;
        }
        for k in 0..bn {
            if g_objc_dlopen_mapped_n < g_closure_cap {
                *g_objc_dlopen_mapped.add(g_objc_dlopen_mapped_n as usize) = *batch.add(k as usize);
                g_objc_dlopen_mapped_n += 1;
            }
        }
        objc_drive_map_images(vm, batch, bn);
        libc::free(batch.cast());
    }
}

#[inline]
fn clshash_mix(mut a: u64, mut b: u64, mut c: u64) -> (u64, u64, u64) {
    macro_rules! mix {
        ($ar:expr, $bl:expr, $cr:expr) => {
            a = a.wrapping_sub(b).wrapping_sub(c) ^ (c >> $ar);
            b = b.wrapping_sub(c).wrapping_sub(a) ^ (a << $bl);
            c = c.wrapping_sub(a).wrapping_sub(b) ^ (b >> $cr);
        };
    }
    mix!(43, 9, 8);
    mix!(38, 23, 5);
    mix!(35, 49, 11);
    a = a.wrapping_sub(b).wrapping_sub(c) ^ (c >> 12);
    b = b.wrapping_sub(c).wrapping_sub(a) ^ (a << 18);
    c = c.wrapping_sub(a).wrapping_sub(b) ^ (b >> 22);
    (a, b, c)
}

unsafe fn clshash_lookup8(mut k: *const u8, length: usize, level: u64) -> u64 {
    unsafe {
        let mut a = level;
        let mut b = level;
        let mut c = 0x9e3779b97f4a7c13u64;
        let mut len = length;
        while len >= 24 {
            for i in 0..8 {
                a = a.wrapping_add(((*k.add(i)) as u64) << (i * 8));
                b = b.wrapping_add(((*k.add(8 + i)) as u64) << (i * 8));
                c = c.wrapping_add(((*k.add(16 + i)) as u64) << (i * 8));
            }
            (a, b, c) = clshash_mix(a, b, c);
            k = k.add(24);
            len -= 24;
        }
        c = c.wrapping_add(length as u64);
        for i in 0..len {
            let byte = *k.add(i) as u64;
            if i < 8 {
                a = a.wrapping_add(byte << (i * 8));
            } else if i < 16 {
                b = b.wrapping_add(byte << ((i - 8) * 8));
            } else {
                c = c.wrapping_add(byte << ((i - 15) * 8));
            }
        }
        clshash_mix(a, b, c).2
    }
}

unsafe fn stringhash_find_raw(
    table: u64,
    key: *const c_char,
    od_out: *mut u64,
    dups_out: *mut u64,
) -> bool {
    unsafe {
        if table == 0 || key.is_null() || *key == 0 {
            return false;
        }
        let t = table as *const u8;
        let capacity = ptr::read_unaligned(t.add(4).cast::<u32>());
        let shift = ptr::read_unaligned(t.add(12).cast::<u32>());
        let mask = ptr::read_unaligned(t.add(16).cast::<u32>());
        let salt = ptr::read_unaligned(t.add(24).cast::<u64>());
        if capacity == 0 || capacity > 0x1000000 {
            return false;
        }
        let kl = libc::strlen(key);
        let val = clshash_lookup8(key.cast(), kl, salt);
        let off_tab = 0x420usize;
        let off_check = off_tab + mask as usize + 1;
        let off_stroffs = off_check + capacity as usize;
        let off_obj = off_stroffs + capacity as usize * 4;
        let off_dups = off_obj + capacity as usize * 8 + 4;
        let scr_idx = *t.add(off_tab + (val as u32 & mask) as usize) as usize;
        let scr = ptr::read_unaligned(t.add(0x20 + scr_idx * 4).cast::<u32>());
        let h = (if shift >= 64 {
            0
        } else {
            (val >> shift) as u32
        }) ^ scr;
        if h >= capacity {
            return false;
        }
        let cb = ((*key as u8 & 7) << 5) | (kl as u8 & 0x1f);
        if *t.add(off_check + h as usize) != cb {
            return false;
        }
        let so = ptr::read_unaligned(t.add(off_stroffs + h as usize * 4).cast::<i32>());
        if so == 0 || libc::strcmp(t.offset(so as isize).cast(), key) != 0 {
            return false;
        }
        *od_out = ptr::read_unaligned(t.add(off_obj + h as usize * 8).cast());
        *dups_out = table + off_dups as u64;
        true
    }
}

unsafe fn stringhash_lookup(table: u64, key: *const c_char, out: *mut u64, max: c_int) -> c_int {
    unsafe {
        let mut od = 0;
        let mut dups = 0;
        if !stringhash_find_raw(table, key, &mut od, &mut dups) {
            return 0;
        }
        if od & 1 == 0 {
            *out = od;
            return 1;
        }
        let didx = (od >> 1) & ((1u64 << 47) - 1);
        let dcnt = (od >> 48) as u32;
        let mut n = 0;
        for i in 0..dcnt {
            if n >= max {
                break;
            }
            let d = ocerz_ld(dups + (didx + i as u64) * 8, 8);
            if d & 1 == 0 {
                *out.add(n as usize) = d;
                n += 1;
            }
        }
        n
    }
}

unsafe fn selhash(mut s: *const u8, mut n: usize) -> u64 {
    unsafe {
        let mut h = 0x9e3779b97f4a7c15u64 ^ n as u64;
        while n >= 8 {
            let w = ptr::read_unaligned(s.cast::<u64>());
            h = (h ^ w).wrapping_mul(0xff51afd7ed558ccd);
            h ^= h >> 32;
            s = s.add(8);
            n -= 8;
        }
        let mut w = 0u64;
        ptr::copy_nonoverlapping(s, (&mut w as *mut u64).cast(), n);
        h = (h ^ w).wrapping_mul(0xc4ceb9fe1a85ec53);
        h ^ (h >> 29)
    }
}

unsafe fn selpool_build() {
    unsafe {
        if !g_selidx.is_null() || g_sel_pool == 0 {
            return;
        }
        libc::pthread_mutex_lock(ptr::addr_of_mut!(g_selidx_lock));
        if !g_selidx.is_null() || g_sel_pool == 0 {
            libc::pthread_mutex_unlock(ptr::addr_of_mut!(g_selidx_lock));
            return;
        }
        let base = g_sel_pool as *const c_char;
        let cap_end = base.add(0x10000000);
        let mut count = 0u64;
        let mut zeros = 0;
        let mut p = base;
        let mut pool_end = base;
        while p < cap_end {
            if *p == 0 {
                zeros += 1;
                if zeros > 256 {
                    break;
                }
                p = p.add(1);
                continue;
            }
            zeros = 0;
            count += 1;
            p = p.add(libc::strlen(p) + 1);
            pool_end = p;
        }
        let mut cap = 1u32;
        while (cap as u64) < count.wrapping_mul(2).wrapping_add(16) {
            cap = cap.wrapping_shl(1);
        }
        let idx =
            libc::calloc(cap as usize, mem::size_of::<*const c_char>()).cast::<*const c_char>();
        if idx.is_null() {
            libc::pthread_mutex_unlock(ptr::addr_of_mut!(g_selidx_lock));
            return;
        }
        p = base;
        while p < pool_end {
            if *p == 0 {
                p = p.add(1);
                continue;
            }
            let len = libc::strlen(p);
            let mut h = selhash(p.cast(), len) as u32 & (cap - 1);
            let mut probes = 0u32;
            while !(*idx.add(h as usize)).is_null() && probes.wrapping_add(1) < cap {
                if **idx.add(h as usize) == *p && libc::strcmp(*idx.add(h as usize), p) == 0 {
                    break;
                }
                h = (h + 1) & (cap - 1);
                probes += 1;
            }
            if (*idx.add(h as usize)).is_null() {
                *idx.add(h as usize) = p;
            }
            p = p.add(len + 1);
        }
        if !libc::getenv(cstr_ptr(c"OCERZ_SELPOOLLOG")).is_null() {
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(c"ocerz: SELPOOL base=%p bytes=%llu strings=%llu cap=%u\n"),
                base,
                pool_end.offset_from(base) as libc::c_ulonglong,
                count as libc::c_ulonglong,
                cap,
            );
        }
        g_selidx_cap = cap;
        g_selidx = idx;
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(g_selidx_lock));
    }
}

unsafe fn selopt_canonical(want: *const c_char) -> u64 {
    unsafe {
        if g_selopt == 0 || want.is_null() || *want == 0 {
            return 0;
        }
        let t = g_selopt as *const u8;
        let capacity = ptr::read_unaligned(t.add(4).cast::<u32>());
        let shift = ptr::read_unaligned(t.add(12).cast::<u32>());
        let mask = ptr::read_unaligned(t.add(16).cast::<u32>());
        let salt = ptr::read_unaligned(t.add(24).cast::<u64>());
        if capacity == 0 || capacity > 0x1000000 {
            return 0;
        }
        let kl = libc::strlen(want);
        let val = clshash_lookup8(want.cast(), kl, salt);
        let off_tab = 0x420usize;
        let off_check = off_tab + mask as usize + 1;
        let off_stroffs = off_check + capacity as usize;
        let scr_idx = *t.add(off_tab + (val as u32 & mask) as usize) as usize;
        let scr = ptr::read_unaligned(t.add(0x20 + scr_idx * 4).cast::<u32>());
        let h = (if shift >= 64 {
            0
        } else {
            (val >> shift) as u32
        }) ^ scr;
        if h >= capacity {
            return 0;
        }
        let cb = ((*want as u8 & 7) << 5) | (kl as u8 & 0x1f);
        if *t.add(off_check + h as usize) != cb {
            return 0;
        }
        let so = ptr::read_unaligned(t.add(off_stroffs + h as usize * 4).cast::<i32>());
        if so == 0 {
            return 0;
        }
        let canon = g_selopt.wrapping_add(so as i64 as u64);
        if libc::strcmp(canon as *const c_char, want) != 0 {
            0
        } else {
            canon
        }
    }
}

pub(crate) unsafe fn selpool_canonical(want: *const c_char) -> u64 {
    unsafe {
        if want.is_null() || *want == 0 {
            return 0;
        }
        if g_selopt != 0 {
            let r = selopt_canonical(want);
            if !libc::getenv(cstr_ptr(c"OCERZ_SELVERIFY")).is_null() && g_sel_pool != 0 {
                if g_selidx.is_null() {
                    selpool_build();
                }
                let mut b = 0;
                if !g_selidx.is_null() {
                    let wl = libc::strlen(want);
                    let mut hh = selhash(want.cast(), wl) as u32 & (g_selidx_cap - 1);
                    while !(*g_selidx.add(hh as usize)).is_null() {
                        if libc::strcmp(*g_selidx.add(hh as usize), want) == 0 {
                            b = *g_selidx.add(hh as usize) as u64;
                            break;
                        }
                        hh = (hh + 1) & (g_selidx_cap - 1);
                    }
                }
                if b != r {
                    libc::fprintf(
                        crate::log::stderr(),
                        cstr_ptr(c"ocerz: SELVERIFY MISMATCH \"%s\" selopt=%#llx build=%#llx\n"),
                        want,
                        r as libc::c_ulonglong,
                        b as libc::c_ulonglong,
                    );
                }
            }
            return r;
        }
        if g_sel_pool == 0 {
            return 0;
        }
        if g_selidx.is_null() {
            selpool_build();
        }
        if g_selidx.is_null() {
            return 0;
        }
        let wl = libc::strlen(want);
        let mut h = selhash(want.cast(), wl) as u32 & (g_selidx_cap - 1);
        while !(*g_selidx.add(h as usize)).is_null() {
            if libc::strcmp(*g_selidx.add(h as usize), want) == 0 {
                return *g_selidx.add(h as usize) as u64;
            }
            h = (h + 1) & (g_selidx_cap - 1);
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyldapi_canonical_selector(name: *const c_char) -> u64 {
    unsafe { selpool_canonical(name) }
}

pub(crate) unsafe fn api_for_each_objc_class(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let name = (*cpu).gpr[OCERZ_RSI as usize];
        let block = (*cpu).gpr[OCERZ_RDX as usize];
        let rsp = (*cpu).gpr[OCERZ_RSP as usize];
        let caller_ret = ocerz_ld(rsp, 8);
        let ret_rsp = rsp + 8;
        let invoke = if block != 0 {
            ocerz_ld(block + 0x10, 8)
        } else {
            0
        };
        let mut hits = [0u64; CLSOPT_MAX_HITS];
        let n = if name != 0 && invoke != 0 && g_block_scratch != 0 {
            stringhash_lookup(
                g_clsopt,
                ocerz_g2h(name).cast(),
                hits.as_mut_ptr(),
                CLSOPT_MAX_HITS as c_int,
            )
        } else {
            0
        };
        crate::ocerz_log!(
            "dyldapi: for_each_objc_class \"%s\" -> %d hit(s)\n",
            if name != 0 {
                ocerz_g2h(name).cast::<c_char>()
            } else {
                cstr_ptr(c"(null)")
            },
            n
        );
        if n == 0 {
            api_return(cpu, 0);
            return OCERZ_STEP_OK;
        }
        ocerz_st(g_block_scratch, 8, 0);
        for i in 0..n {
            let hit = hits[i as usize];
            let cls = g_cache_start + ((hit >> 1) & ((1u64 << 47) - 1));
            let loaded = objc_index_loaded((hit >> 48) as u32);
            let nm = ocerz_g2h(name).cast::<c_char>();
            if !libc::getenv(cstr_ptr(c"OCERZ_CLSLOG")).is_null()
                && !libc::strstr(nm, cstr_ptr(c"NSString")).is_null()
            {
                libc::fprintf(
                    crate::log::stderr(),
                    cstr_ptr(c"ocerz: CLSLOOKUP[%d] \"%s\" cls=%#llx imgidx=%u loaded=%llu\n"),
                    libc::getpid(),
                    nm,
                    cls as libc::c_ulonglong,
                    (hit >> 48) as u32,
                    loaded as libc::c_ulonglong,
                );
            }
            let args = [block, cls, loaded, g_block_scratch];
            ocerz_vm_call(vm, invoke, args.as_ptr(), 4, ret_rsp);
            if (*vm).exited != 0 {
                return OCERZ_STEP_OK;
            }
            if ocerz_ld(g_block_scratch, 1) & 1 != 0 {
                break;
            }
        }
        (*cpu).rip = caller_ret;
        (*cpu).gpr[OCERZ_RSP as usize] = ret_rsp;
        (*cpu).gpr[OCERZ_RAX as usize] = 0;
        OCERZ_STEP_OK
    }
}

pub(crate) unsafe fn api_for_each_objc_protocol(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let name = (*cpu).gpr[OCERZ_RSI as usize];
        let block = (*cpu).gpr[OCERZ_RDX as usize];
        let rsp = (*cpu).gpr[OCERZ_RSP as usize];
        let caller_ret = ocerz_ld(rsp, 8);
        let ret_rsp = rsp + 8;
        let invoke = if block != 0 {
            ocerz_ld(block + 0x10, 8)
        } else {
            0
        };
        let mut od = 0;
        let mut dups = 0;
        let found = name != 0
            && invoke != 0
            && g_block_scratch != 0
            && stringhash_find_raw(g_protoopt, ocerz_g2h(name).cast(), &mut od, &mut dups);
        crate::ocerz_log!(
            "dyldapi: for_each_objc_protocol \"%s\" -> %s\n",
            if name != 0 {
                ocerz_g2h(name).cast::<c_char>()
            } else {
                cstr_ptr(c"(null)")
            },
            if found {
                cstr_ptr(c"hit")
            } else {
                cstr_ptr(c"miss")
            }
        );
        if !found {
            api_return(cpu, 0);
            return OCERZ_STEP_OK;
        }
        ocerz_st(g_block_scratch, 8, 0);
        let mut run_base = 0u64;
        let mut run_cnt = 1u64;
        if od & 1 != 0 {
            run_base = dups + ((od >> 1) & ((1u64 << 47) - 1)) * 8;
            run_cnt = od >> 48;
        }
        for i in 0..run_cnt {
            let mut d = od;
            if od & 1 != 0 {
                d = ocerz_ld(run_base + i * 8, 8);
                if d & 1 != 0 {
                    continue;
                }
            }
            let proto = g_cache_start + ((d >> 1) & ((1u64 << 47) - 1));
            let loaded = objc_index_loaded((d >> 48) as u32);
            let args = [block, proto, loaded, g_block_scratch];
            ocerz_vm_call(vm, invoke, args.as_ptr(), 4, ret_rsp);
            if (*vm).exited != 0 {
                return OCERZ_STEP_OK;
            }
            if ocerz_ld(g_block_scratch, 1) & 1 != 0 {
                break;
            }
        }
        (*cpu).rip = caller_ret;
        (*cpu).gpr[OCERZ_RSP as usize] = ret_rsp;
        (*cpu).gpr[OCERZ_RAX as usize] = 0;
        OCERZ_STEP_OK
    }
}

unsafe fn in_cache(a: u64) -> bool {
    unsafe { a >= g_cache_start && a < g_cache_start.wrapping_add(super::g_cache_size) }
}

unsafe fn load_in_plain_list(ml: u64) -> u64 {
    unsafe {
        if !in_cache(ml) {
            return 0;
        }
        let ef = ocerz_ld(ml, 4) as u32;
        let count = ocerz_ld(ml + 4, 4) as u32;
        let entsize = ef & 0xfffc;
        let rel = ef & 0x80000000 != 0;
        if count == 0 || count > 65536 || entsize == 0 || entsize > 64 {
            return 0;
        }
        for i in 0..count {
            let ent = ml + 8 + i as u64 * entsize as u64;
            let nameptr;
            let imp;
            if rel {
                let noff = ocerz_ld(ent, 4) as u32 as i32;
                let ioff = ocerz_ld(ent + 8, 4) as u32 as i32;
                nameptr = g_sel_pool.wrapping_add(noff as i64 as u64);
                imp = (ent + 8).wrapping_add(ioff as i64 as u64);
            } else {
                nameptr = ocerz_ld(ent, 8);
                imp = ocerz_ld(ent + 16, 8);
            }
            if nameptr != 0 && libc::strcmp(ocerz_g2h(nameptr).cast(), cstr_ptr(c"load")) == 0 {
                return imp;
            }
        }
        0
    }
}

unsafe fn load_in_method_list(raw: u64) -> u64 {
    unsafe {
        if raw == 0 {
            return 0;
        }
        let p = raw & !7;
        if !in_cache(p) {
            return 0;
        }
        if raw & 1 != 0 {
            let count = ocerz_ld(p + 4, 4) as u32;
            if count > 4096 {
                return 0;
            }
            for i in 0..count {
                let ent = p + 8 + i as u64 * 8;
                let off = (ocerz_ld(ent, 8) as i64) >> 16;
                let imp = load_in_plain_list((ent as i64).wrapping_add(off) as u64);
                if imp != 0 {
                    return imp;
                }
            }
            0
        } else {
            load_in_plain_list(p)
        }
    }
}

unsafe fn metaclass_load_imp(cls: u64) -> u64 {
    unsafe {
        if !in_cache(cls) {
            return 0;
        }
        let meta = ocerz_ld(cls, 8) & 0x00007ffffffffff8;
        if !in_cache(meta) {
            return 0;
        }
        let data = ocerz_ld(meta + 0x20, 8) & !7;
        if !in_cache(data) {
            return 0;
        }
        load_in_method_list(ocerz_ld(data + 0x20, 8))
    }
}

unsafe fn meth_list_has(ml: u64, want: *const c_char) -> bool {
    unsafe {
        if !in_cache(ml) {
            return false;
        }
        let ef = ocerz_ld(ml, 4) as u32;
        let count = ocerz_ld(ml + 4, 4) as u32;
        let entsize = ef & 0xfffc;
        let rel = ef & 0x80000000 != 0;
        if count == 0 || count > 65536 || entsize == 0 || entsize > 64 {
            return false;
        }
        for i in 0..count {
            let ent = ml + 8 + i as u64 * entsize as u64;
            let nameptr = if rel {
                g_sel_pool.wrapping_add(ocerz_ld(ent, 4) as u32 as i32 as i64 as u64)
            } else {
                ocerz_ld(ent, 8)
            };
            if nameptr != 0 && libc::strcmp(ocerz_g2h(nameptr).cast(), want) == 0 {
                return true;
            }
        }
        false
    }
}

unsafe fn meth_list_print_substr(ml: u64, substr: *const c_char) {
    unsafe {
        if !in_cache(ml) {
            return;
        }
        let ef = ocerz_ld(ml, 4) as u32;
        let count = ocerz_ld(ml + 4, 4) as u32;
        let entsize = ef & 0xfffc;
        let rel = ef & 0x80000000 != 0;
        if count == 0 || count > 65536 || entsize == 0 || entsize > 64 {
            return;
        }
        for i in 0..count {
            let ent = ml + 8 + i as u64 * entsize as u64;
            let nameptr = if rel {
                g_sel_pool.wrapping_add(ocerz_ld(ent, 4) as u32 as i32 as i64 as u64)
            } else {
                ocerz_ld(ent, 8)
            };
            if nameptr == 0 {
                continue;
            }
            let nm = ocerz_g2h(nameptr).cast::<c_char>();
            if !libc::strstr(nm, substr).is_null() {
                let imp = if rel {
                    (ent + 8).wrapping_add(ocerz_ld(ent + 8, 4) as u32 as i32 as i64 as u64)
                } else {
                    ocerz_ld(ent + 16, 8)
                };
                libc::fprintf(
                    crate::log::stderr(),
                    cstr_ptr(c"ocerz:       method \"%s\" imp=%#llx\n"),
                    nm,
                    imp as libc::c_ulonglong,
                );
            }
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyldapi_dump_method(cls: u64, sel: *const c_char) {
    unsafe {
        if !in_cache(cls) || g_sel_pool == 0 {
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(c"ocerz: METHDUMP cls=%#llx not-in-cache or no selpool\n"),
                cls as libc::c_ulonglong,
            );
            return;
        }
        let meta = ocerz_ld(cls, 8) & 0x00007ffffffffff8;
        let data = ocerz_ld(meta + 0x20, 8) & !7;
        let mut ro = data;
        let dflags = ocerz_ld(data, 4) as u32;
        let realized = dflags & 0x80000000 != 0;
        if realized {
            let roe = ocerz_ld(data + 8, 8);
            ro = if roe & 1 != 0 {
                ocerz_ld(roe & !1, 8)
            } else {
                roe
            };
        }
        let raw = ocerz_ld(ro + 0x20, 8);
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: METHDUMP cls=%#llx meta=%#llx realized=%d ro=%#llx baseMethods=%#llx sel=%s\n"
                .as_ptr(),
            cls as libc::c_ulonglong,
            meta as libc::c_ulonglong,
            realized as c_int,
            ro as libc::c_ulonglong,
            raw as libc::c_ulonglong,
            sel,
        );
        let p = raw & !7;
        if !in_cache(p) {
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(c"ocerz:   baseMethods not in cache\n"),
            );
            return;
        }
        if raw & 1 != 0 {
            let count = (ocerz_ld(p + 4, 4) as u32).min(4096);
            for i in 0..count {
                let ent = p + 8 + i as u64 * 8;
                let rawent = ocerz_ld(ent, 8);
                let off = (rawent as i64) >> 16;
                let imgidx = rawent as u32 & 0xffff;
                let sublist = (ent as i64).wrapping_add(off) as u64;
                let loaded = objc_index_loaded(imgidx);
                let has = meth_list_has(sublist, sel);
                libc::fprintf(
                    crate::log::stderr(),
                    cstr_ptr(c"ocerz:   sub[%u] list=%#llx imgidx=%u loaded=%llu has(%s)=%d%s\n"),
                    i,
                    sublist as libc::c_ulonglong,
                    imgidx,
                    loaded as libc::c_ulonglong,
                    sel,
                    has as c_int,
                    if has && loaded == 0 {
                        cstr_ptr(c"  <== DROPPED (has sel but image unloaded)")
                    } else {
                        cstr_ptr(c"")
                    },
                );
                meth_list_print_substr(sublist, cstr_ptr(c"allocWithZone:"));
            }
        } else {
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(c"ocerz:   single list has(%s)=%d\n"),
                sel,
                meth_list_has(p, sel) as c_int,
            );
        }
        let g = 0x7ff8436dad48u64;
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"ocerz:   ALLOCGLOBAL[%#llx]=%#llx  cls=%#llx  match=%d\n"),
            g as libc::c_ulonglong,
            ocerz_ld(g, 8) as libc::c_ulonglong,
            cls as libc::c_ulonglong,
            (ocerz_ld(g, 8) == cls) as c_int,
        );
    }
}

unsafe fn methdump_diag(tag: *const c_char) {
    unsafe {
        let spec = libc::getenv(cstr_ptr(c"OCERZ_METHDUMP"));
        if spec.is_null() {
            return;
        }
        let mut buf = [0 as c_char; 256];
        libc::snprintf(buf.as_mut_ptr(), buf.len(), cstr_ptr(c"%s"), spec);
        let colon = libc::strchr(buf.as_mut_ptr(), b':' as c_int);
        if colon.is_null() {
            return;
        }
        *colon = 0;
        let cls = libc::strtoull(buf.as_ptr(), ptr::null_mut(), 16);
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"ocerz: === METHDUMP @%s ===\n"),
            tag,
        );
        ocerz_dyldapi_dump_method(cls, colon.add(1));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyldapi_run_image_loads(vm: *mut OcerzVM, mh: u64, stack_top: u64) {
    unsafe {
        if g_sel_pool == 0 {
            return;
        }
        let imgpath = if !g_cache.is_null() {
            cache_path_for_mh(g_cache, mh)
        } else {
            ptr::null()
        };
        crate::ocerz_log!(
            "loadphase: image %s (mh=%#llx)\n",
            if imgpath.is_null() {
                cstr_ptr(c"?")
            } else {
                imgpath
            },
            mh as libc::c_ulonglong
        );
        if in_cache(mh) && libc::getenv(cstr_ptr(c"OCERZ_NO_LOADMAP")).is_null() {
            ocerz_dyldapi_objc_map_one(vm, mh);
        }
        if g_objc_init_cb != 0 {
            if g_objc_init_info == 0 {
                g_objc_init_info = ocerz_map_anywhere(0x20, PROT_READ | PROT_WRITE);
                if g_objc_init_info != 0 {
                    ocerz_st(g_objc_init_info, 8, mh);
                    ocerz_st(
                        g_objc_init_info + 8,
                        8,
                        if imgpath.is_null() {
                            0
                        } else {
                            ocerz_h2g(imgpath.cast())
                        },
                    );
                    ocerz_st(g_objc_init_info + 16, 8, mh);
                    ocerz_st(
                        g_objc_init_info + 24,
                        8,
                        find_section_any(mh, cstr_ptr(c"__objc_imageinfo")),
                    );
                    let args = [g_objc_init_info];
                    ocerz_vm_call(vm, g_objc_init_cb, args.as_ptr(), 1, stack_top);
                    return;
                }
            } else {
                ocerz_st(g_objc_init_info, 8, mh);
                ocerz_st(
                    g_objc_init_info + 8,
                    8,
                    if imgpath.is_null() {
                        0
                    } else {
                        ocerz_h2g(imgpath.cast())
                    },
                );
                ocerz_st(g_objc_init_info + 16, 8, mh);
                ocerz_st(
                    g_objc_init_info + 24,
                    8,
                    find_section_any(mh, cstr_ptr(c"__objc_imageinfo")),
                );
                let args = [g_objc_init_info];
                ocerz_vm_call(vm, g_objc_init_cb, args.as_ptr(), 1, stack_top);
                return;
            }
        }
        let mut size = 0;
        let nl = find_section_sz(mh, cstr_ptr(c"__objc_nlclslist"), &mut size);
        for i in 0..(size / 8) {
            if nl == 0 {
                break;
            }
            let cls = ocerz_ld(nl + i * 8, 8) & !1;
            let imp = metaclass_load_imp(cls);
            if imp != 0 {
                let args = [cls, 0];
                ocerz_vm_call(vm, imp, args.as_ptr(), 2, stack_top);
                if (*vm).exited != 0 {
                    return;
                }
            }
        }
        let mut csize = 0;
        let ncl = find_section_sz(mh, cstr_ptr(c"__objc_nlcatlist"), &mut csize);
        for i in 0..(csize / 8) {
            if ncl == 0 {
                break;
            }
            let cat = ocerz_ld(ncl + i * 8, 8) & !1;
            if !in_cache(cat) {
                continue;
            }
            let imp = load_in_method_list(ocerz_ld(cat + 0x18, 8));
            if imp != 0 {
                let cls = ocerz_ld(cat + 8, 8);
                let args = [cls, 0];
                crate::ocerz_log!(
                    "loadphase:   +load category imp=%#llx cls=%#llx\n",
                    imp as libc::c_ulonglong,
                    cls as libc::c_ulonglong
                );
                ocerz_vm_call(vm, imp, args.as_ptr(), 2, stack_top);
                if (*vm).exited != 0 {
                    return;
                }
            }
        }
    }
}

//! Image registration, dyld dependency closures, cache path lookup, and PC indexing.

use core::ffi::{c_char, c_int, c_void};
use core::mem;
use core::ptr;
use core::sync::atomic::{AtomicPtr, Ordering};

use crate::ffi::{
    OcerzCache, ocerz_cache_find_alias, ocerz_cache_image_addr, ocerz_canon_dylib_path,
    ocerz_main_mh,
};

use super::hostmem::ocerz_g2h;
use super::macho::*;
use super::{
    cstr_ptr, g_cache, g_closure_cap, g_closure_hash, g_closure_hash_mask, g_closure_mh,
    g_closure_n,
};

const DYLDAPI_DISK_MAX: usize = 256;

static mut g_disk_mh: [u64; DYLDAPI_DISK_MAX] = [0; DYLDAPI_DISK_MAX];
static mut g_disk_path: [u64; DYLDAPI_DISK_MAX] = [0; DYLDAPI_DISK_MAX];
static mut g_disk_n: c_int = 0;

#[repr(C)]
struct CachePathSlot {
    path: *const c_char,
    mh: u64,
}

static g_cache_paths: AtomicPtr<CachePathSlot> = AtomicPtr::new(ptr::null_mut());
static mut g_cache_paths_mask: u32 = 0;
static mut g_cache_paths_of: *mut OcerzCache = ptr::null_mut();
static mut g_cache_paths_lock: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

#[repr(C)]
#[derive(Clone, Copy)]
struct PcRange {
    lo: u64,
    hi: u64,
    mh: u64,
}

struct PcIndex {
    r: *mut PcRange,
    maxhi: *mut u64,
    n: usize,
    closure_n: c_int,
    closure_mh: *mut u64,
}

static g_pcidx: AtomicPtr<PcIndex> = AtomicPtr::new(ptr::null_mut());
static mut g_pcidx_lock: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

#[inline(always)]
unsafe fn cache_image(cache: *mut OcerzCache, i: u32, path: *mut *const c_char) -> u64 {
    unsafe { ocerz_cache_image_addr(cache, i, path) }
}

pub(crate) unsafe fn disk_gpath_for_mh(mh: u64) -> u64 {
    unsafe {
        for i in 0..g_disk_n {
            if *ptr::addr_of!(g_disk_mh).cast::<u64>().add(i as usize) == mh {
                return *ptr::addr_of!(g_disk_path).cast::<u64>().add(i as usize);
            }
        }
        0
    }
}

unsafe fn disk_path_for_mh(mh: u64) -> *const c_char {
    unsafe {
        let path = disk_gpath_for_mh(mh);
        if path == 0 {
            ptr::null()
        } else {
            ocerz_g2h(path).cast()
        }
    }
}

unsafe fn cache_path_hash(s: *const c_char) -> u32 {
    unsafe {
        let mut h = 0xcbf29ce484222325u64;
        let mut p = s;
        while *p != 0 {
            h ^= *p as u8 as u64;
            h = h.wrapping_mul(0x100000001b3);
            p = p.add(1);
        }
        (h ^ (h >> 32)) as u32
    }
}

unsafe fn cache_paths_index(cache: *mut OcerzCache) -> *mut CachePathSlot {
    unsafe {
        let mut have = g_cache_paths.load(Ordering::SeqCst);
        if !have.is_null() && g_cache_paths_of == cache {
            return have;
        }
        libc::pthread_mutex_lock(ptr::addr_of_mut!(g_cache_paths_lock));
        have = g_cache_paths.load(Ordering::SeqCst);
        if have.is_null() || g_cache_paths_of != cache {
            let images_cnt = (*cache).images_cnt;
            let mut cap = 16u32;
            while cap < images_cnt.wrapping_mul(2).wrapping_add(16) {
                cap = cap.wrapping_shl(1);
            }
            let made =
                libc::calloc(cap as usize, mem::size_of::<CachePathSlot>()).cast::<CachePathSlot>();
            if !made.is_null() {
                for i in 0..images_cnt {
                    let mut path = ptr::null();
                    let mh = cache_image(cache, i, &mut path);
                    if mh == 0 || path.is_null() {
                        continue;
                    }
                    let mut at = cache_path_hash(path) & (cap - 1);
                    while !(*made.add(at as usize)).path.is_null()
                        && libc::strcmp((*made.add(at as usize)).path, path) != 0
                    {
                        at = (at + 1) & (cap - 1);
                    }
                    if (*made.add(at as usize)).path.is_null() {
                        (*made.add(at as usize)).path = path;
                        (*made.add(at as usize)).mh = mh;
                    }
                }
                g_cache_paths_mask = cap - 1;
                g_cache_paths_of = cache;
                g_cache_paths.store(made, Ordering::SeqCst);
                have = made;
            }
        }
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(g_cache_paths_lock));
        have
    }
}

pub(crate) unsafe fn cache_find_path_ex(
    cache: *mut OcerzCache,
    path: *const c_char,
    cache_path: *mut *const c_char,
) -> u64 {
    unsafe {
        let index = cache_paths_index(cache);
        if index.is_null() {
            for i in 0..(*cache).images_cnt {
                let mut p = ptr::null();
                let mh = cache_image(cache, i, &mut p);
                if mh != 0 && !p.is_null() && libc::strcmp(p, path) == 0 {
                    if !cache_path.is_null() {
                        *cache_path = p;
                    }
                    return mh;
                }
            }
            return 0;
        }
        let mask = g_cache_paths_mask;
        let mut at = cache_path_hash(path) & mask;
        while !(*index.add(at as usize)).path.is_null() {
            if libc::strcmp((*index.add(at as usize)).path, path) == 0 {
                if !cache_path.is_null() {
                    *cache_path = (*index.add(at as usize)).path;
                }
                return (*index.add(at as usize)).mh;
            }
            at = (at + 1) & mask;
        }
        let alias = ocerz_cache_find_alias(cache, path);
        if alias != 0 && !cache_path.is_null() {
            *cache_path = ptr::null();
            for i in 0..(*cache).images_cnt {
                let mut p = ptr::null();
                if cache_image(cache, i, &mut p) == alias {
                    *cache_path = p;
                    break;
                }
            }
            if (*cache_path).is_null() {
                return 0;
            }
        }
        alias
    }
}

pub(crate) unsafe fn cache_find_path(cache: *mut OcerzCache, path: *const c_char) -> u64 {
    unsafe { cache_find_path_ex(cache, path, ptr::null_mut()) }
}

pub(crate) unsafe fn cache_find_canonical(
    path: *const c_char,
    cache_path: *mut *const c_char,
) -> u64 {
    unsafe {
        let mut canon = [0 as c_char; 1024];
        let mut want = path;
        if g_cache.is_null() || path.is_null() {
            return 0;
        }
        for pass in 0..2 {
            let mh = cache_find_path_ex(g_cache, want, cache_path);
            if mh != 0 {
                return mh;
            }
            if pass != 0
                || ocerz_canon_dylib_path(path, canon.as_mut_ptr(), canon.len()) == 0
                || libc::strcmp(canon.as_ptr(), path) == 0
            {
                break;
            }
            want = canon.as_ptr();
        }
        0
    }
}

pub(crate) unsafe fn find_section_sz(mh: u64, sect: *const c_char, size_out: *mut u64) -> u64 {
    unsafe {
        let h = ocerz_g2h(mh).cast::<MachHeader64>();
        if mh == 0 || (*h).magic != MH_MAGIC_64 {
            return 0;
        }
        let mut lc = h.add(1).cast::<u8>();
        let mut slide = 0u64;
        let mut q = lc;
        for _ in 0..(*h).ncmds {
            let l = q.cast::<LoadCommand>();
            if (*l).cmd == LC_SEGMENT_64 {
                let s = q.cast::<SegmentCommand64>();
                if (*s).fileoff == 0 && (*s).filesize != 0 {
                    slide = mh.wrapping_sub((*s).vmaddr);
                    break;
                }
            }
            q = q.add((*l).cmdsize as usize);
        }
        for _ in 0..(*h).ncmds {
            let l = lc.cast::<LoadCommand>();
            if (*l).cmd == LC_SEGMENT_64 {
                let s = lc.cast::<SegmentCommand64>();
                let sc = s.add(1).cast::<Section64>();
                for j in 0..(*s).nsects {
                    let section = sc.add(j as usize);
                    if libc::strncmp((*section).sectname.as_ptr(), sect, 16) == 0 {
                        if !size_out.is_null() {
                            *size_out = (*section).size;
                        }
                        return (*section).addr.wrapping_add(slide);
                    }
                }
            }
            lc = lc.add((*l).cmdsize as usize);
        }
        0
    }
}

pub(crate) unsafe fn find_section_any(mh: u64, sect: *const c_char) -> u64 {
    unsafe { find_section_sz(mh, sect, ptr::null_mut()) }
}

pub(crate) unsafe fn image_slide(mh: u64) -> u64 {
    unsafe {
        let h = ocerz_g2h(mh).cast::<MachHeader64>();
        if mh == 0 || (*h).magic != MH_MAGIC_64 {
            return 0;
        }
        let mut lc = h.add(1).cast::<u8>();
        for _ in 0..(*h).ncmds {
            let l = lc.cast::<LoadCommand>();
            if (*l).cmd == LC_SEGMENT_64 {
                let s = lc.cast::<SegmentCommand64>();
                if (*s).fileoff == 0 && (*s).filesize != 0 {
                    return mh.wrapping_sub((*s).vmaddr);
                }
            }
            lc = lc.add((*l).cmdsize as usize);
        }
        0
    }
}

pub(crate) unsafe fn parse_build_version(mh: u64, plat: *mut u32, minos: *mut u32, sdk: *mut u32) {
    unsafe {
        *plat = 0;
        *minos = 0;
        *sdk = 0;
        let h = ocerz_g2h(mh).cast::<MachHeader64>();
        if mh == 0 || (*h).magic != MH_MAGIC_64 {
            return;
        }
        let mut lc = h.add(1).cast::<u8>();
        for _ in 0..(*h).ncmds {
            let l = lc.cast::<LoadCommand>();
            if (*l).cmd == LC_BUILD_VERSION {
                ptr::copy_nonoverlapping(lc.add(8), plat.cast::<u8>(), 4);
                ptr::copy_nonoverlapping(lc.add(12), minos.cast::<u8>(), 4);
                ptr::copy_nonoverlapping(lc.add(16), sdk.cast::<u8>(), 4);
                return;
            }
            if (*l).cmd == LC_VERSION_MIN_MACOSX {
                *plat = 1;
                ptr::copy_nonoverlapping(lc.add(8), minos.cast::<u8>(), 4);
                ptr::copy_nonoverlapping(lc.add(12), sdk.cast::<u8>(), 4);
                return;
            }
            lc = lc.add((*l).cmdsize as usize);
        }
    }
}

pub(crate) fn build_version_at_least(plat: u32, have: u32, q: u64) -> u64 {
    let qplat = q as u32;
    let qver = (q >> 32) as u32;
    if qplat == u32::MAX {
        1
    } else if qplat != plat {
        0
    } else {
        (have >= qver) as u64
    }
}

unsafe fn dylib_name_matches(install_name: *const c_char, library_name: *const c_char) -> bool {
    unsafe {
        let slash = libc::strrchr(install_name, b'/' as c_int);
        let leaf = if slash.is_null() {
            install_name
        } else {
            slash.add(1)
        };
        if libc::strcmp(leaf, library_name) == 0 {
            return true;
        }
        let leaf_len = libc::strlen(leaf);
        let name_len = libc::strlen(library_name);
        if leaf_len < name_len + 9
            || libc::strncmp(leaf, cstr_ptr(c"lib"), 3) != 0
            || libc::strcmp(leaf.add(leaf_len - 6), cstr_ptr(c".dylib")) != 0
            || libc::strncmp(leaf.add(3), library_name, name_len) != 0
        {
            return false;
        }
        *leaf.add(name_len + 3) == b'.' as c_char
    }
}

pub(crate) unsafe fn link_time_library_version(library_name: *const c_char) -> i32 {
    unsafe {
        if library_name.is_null() || ocerz_main_mh == 0 {
            return -1;
        }
        let h = ocerz_g2h(ocerz_main_mh).cast::<MachHeader64>();
        if (*h).magic != MH_MAGIC_64 {
            return -1;
        }
        let mut lc = h.add(1).cast::<u8>();
        let end = lc.add((*h).sizeofcmds as usize);
        let mut result = -1;
        for _ in 0..(*h).ncmds {
            if end.offset_from(lc) < mem::size_of::<LoadCommand>() as isize {
                break;
            }
            let l = lc.cast::<LoadCommand>();
            if (*l).cmdsize < mem::size_of::<LoadCommand>() as u32
                || end.offset_from(lc) < (*l).cmdsize as isize
            {
                break;
            }
            if ((*l).cmd == LC_LOAD_DYLIB
                || (*l).cmd == LC_LOAD_WEAK_DYLIB
                || (*l).cmd == LC_REEXPORT_DYLIB
                || (*l).cmd == LC_LOAD_UPWARD_DYLIB)
                && (*l).cmdsize >= mem::size_of::<DylibCommand>() as u32
            {
                let d = lc.cast::<DylibCommand>();
                let name_off = (*d).dylib.name_offset;
                if name_off < (*l).cmdsize {
                    let install_name = lc.add(name_off as usize).cast::<c_char>();
                    let avail = (*l).cmdsize - name_off;
                    if !libc::memchr(install_name.cast(), 0, avail as usize).is_null()
                        && dylib_name_matches(install_name, library_name)
                    {
                        result = (*d).dylib.current_version as i32;
                    }
                }
            }
            lc = lc.add((*l).cmdsize as usize);
        }
        result
    }
}

pub(crate) unsafe fn runtime_library_version(library_name: *const c_char) -> i32 {
    unsafe {
        if library_name.is_null() {
            return -1;
        }
        let mut image = 0;
        while image < g_closure_n {
            let mh = *g_closure_mh.add(image as usize);
            let h = ocerz_g2h(mh).cast::<MachHeader64>();
            if (*h).magic == MH_MAGIC_64 {
                let mut lc = h.add(1).cast::<u8>();
                let end = lc.add((*h).sizeofcmds as usize);
                for _ in 0..(*h).ncmds {
                    if end.offset_from(lc) < mem::size_of::<LoadCommand>() as isize {
                        break;
                    }
                    let l = lc.cast::<LoadCommand>();
                    if (*l).cmdsize < mem::size_of::<LoadCommand>() as u32
                        || end.offset_from(lc) < (*l).cmdsize as isize
                    {
                        break;
                    }
                    if (*l).cmd == LC_ID_DYLIB
                        && (*l).cmdsize >= mem::size_of::<DylibCommand>() as u32
                    {
                        let d = lc.cast::<DylibCommand>();
                        let name_off = (*d).dylib.name_offset;
                        if name_off < (*l).cmdsize {
                            let install_name = lc.add(name_off as usize).cast::<c_char>();
                            let avail = (*l).cmdsize - name_off;
                            if !libc::memchr(install_name.cast(), 0, avail as usize).is_null()
                                && dylib_name_matches(install_name, library_name)
                            {
                                return (*d).dylib.current_version as i32;
                            }
                        }
                    }
                    lc = lc.add((*l).cmdsize as usize);
                }
            }
            image += 1;
        }
        -1
    }
}

pub(crate) unsafe fn cache_path_for_mh(cache: *mut OcerzCache, mh: u64) -> *const c_char {
    unsafe {
        let disk = disk_path_for_mh(mh);
        if !disk.is_null() {
            return disk;
        }
        for i in 0..(*cache).images_cnt {
            let mut path = ptr::null();
            if cache_image(cache, i, &mut path) == mh {
                return path;
            }
        }
        ptr::null()
    }
}

unsafe extern "C" fn pcrange_cmp(a: *const c_void, b: *const c_void) -> c_int {
    unsafe {
        let x = &*a.cast::<PcRange>();
        let y = &*b.cast::<PcRange>();
        if x.lo < y.lo {
            -1
        } else if x.lo > y.lo {
            1
        } else {
            0
        }
    }
}

unsafe fn pcidx_add_image(r: *mut *mut PcRange, n: usize, cap: *mut usize, mh: u64) -> usize {
    unsafe {
        let h = ocerz_g2h(mh).cast::<MachHeader64>();
        if mh == 0 || (*h).magic != MH_MAGIC_64 {
            return n;
        }
        let slide = image_slide(mh);
        let mut lc = h.add(1).cast::<u8>();
        let mut count = n;
        for _ in 0..(*h).ncmds {
            let l = lc.cast::<LoadCommand>();
            if (*l).cmd == LC_SEGMENT_64 {
                let sg = lc.cast::<SegmentCommand64>();
                if (*sg).vmsize != 0 && ((*sg).initprot != 0 || (*sg).maxprot != 0) {
                    if count == *cap {
                        *cap = if *cap != 0 {
                            (*cap).wrapping_mul(2)
                        } else {
                            16384
                        };
                        let nr = libc::realloc(
                            *r.cast::<*mut c_void>(),
                            cap.read().wrapping_mul(mem::size_of::<PcRange>()),
                        )
                        .cast::<PcRange>();
                        if nr.is_null() {
                            return count;
                        }
                        *r = nr;
                    }
                    let item = (*r).add(count);
                    (*item).lo = (*sg).vmaddr.wrapping_add(slide);
                    (*item).hi = (*sg).vmaddr.wrapping_add(slide).wrapping_add((*sg).vmsize);
                    (*item).mh = mh;
                    count += 1;
                }
            }
            lc = lc.add((*l).cmdsize as usize);
        }
        count
    }
}

unsafe fn pcidx_current() -> *const PcIndex {
    unsafe {
        let mut cur = g_pcidx.load(Ordering::Acquire);
        if !cur.is_null() && (*cur).closure_n == g_closure_n && (*cur).closure_mh == g_closure_mh {
            return cur;
        }
        libc::pthread_mutex_lock(ptr::addr_of_mut!(g_pcidx_lock));
        cur = g_pcidx.load(Ordering::SeqCst);
        if cur.is_null() || (*cur).closure_n != g_closure_n || (*cur).closure_mh != g_closure_mh {
            let ni = libc::calloc(1, mem::size_of::<PcIndex>()).cast::<PcIndex>();
            if !ni.is_null() {
                let mut n = 0usize;
                let mut cap = 0usize;
                let mut r = ptr::null_mut();
                (*ni).closure_n = g_closure_n;
                (*ni).closure_mh = g_closure_mh;
                for i in 0..g_closure_n {
                    n = pcidx_add_image(&mut r, n, &mut cap, *g_closure_mh.add(i as usize));
                }
                if !g_cache.is_null() {
                    for i in 0..(*g_cache).images_cnt {
                        n = pcidx_add_image(
                            &mut r,
                            n,
                            &mut cap,
                            cache_image(g_cache, i, ptr::null_mut()),
                        );
                    }
                }
                if n != 0 {
                    libc::qsort(r.cast(), n, mem::size_of::<PcRange>(), Some(pcrange_cmp));
                }
                let maxhi = if n != 0 {
                    libc::malloc(n.wrapping_mul(mem::size_of::<u64>())).cast::<u64>()
                } else {
                    ptr::null_mut()
                };
                if !maxhi.is_null() {
                    for i in 0..n {
                        *maxhi.add(i) = if i != 0 && *maxhi.add(i - 1) > (*r.add(i)).hi {
                            *maxhi.add(i - 1)
                        } else {
                            (*r.add(i)).hi
                        };
                    }
                }
                (*ni).r = r;
                (*ni).n = if !maxhi.is_null() || n == 0 { n } else { 0 };
                (*ni).maxhi = maxhi;
                g_pcidx.store(ni, Ordering::Release);
                cur = ni;
            }
        }
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(g_pcidx_lock));
        cur
    }
}

pub(crate) unsafe fn image_for_pc(pc: u64) -> u64 {
    unsafe {
        let ix = pcidx_current();
        if ix.is_null() || (*ix).n == 0 {
            return 0;
        }
        let mut lo = 0usize;
        let mut hi = (*ix).n;
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if (*(*ix).r.add(mid)).lo <= pc {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        let mut k = lo;
        while k > 0 && *(*ix).maxhi.add(k - 1) > pc {
            k -= 1;
            let r = &*(*ix).r.add(k);
            if pc < r.hi {
                return r.mh;
            }
        }
        0
    }
}

pub(crate) unsafe fn image_nearest_symbol(
    mh: u64,
    addr: u64,
    sname_out: *mut u64,
    saddr_out: *mut u64,
) {
    unsafe {
        *sname_out = 0;
        *saddr_out = 0;
        let h = ocerz_g2h(mh).cast::<MachHeader64>();
        if mh == 0 || (*h).magic != MH_MAGIC_64 {
            return;
        }
        let slide = image_slide(mh);
        let mut symoff = 0u32;
        let mut nsyms = 0u32;
        let mut stroff = 0u32;
        let mut strsize = 0u32;
        let mut le_vmaddr = 0u64;
        let mut le_fileoff = 0u64;
        let mut le_filesize = 0u64;
        let mut have_le = false;
        let mut have_sym = false;
        let mut lc = h.add(1).cast::<u8>();
        for _ in 0..(*h).ncmds {
            let l = lc.cast::<LoadCommand>();
            if (*l).cmd == LC_SYMTAB {
                let s = lc.cast::<SymtabCommand>();
                symoff = (*s).symoff;
                nsyms = (*s).nsyms;
                stroff = (*s).stroff;
                strsize = (*s).strsize;
                have_sym = true;
            } else if (*l).cmd == LC_SEGMENT_64 {
                let s = lc.cast::<SegmentCommand64>();
                if libc::strcmp((*s).segname.as_ptr(), cstr_ptr(c"__LINKEDIT")) == 0 {
                    le_vmaddr = (*s).vmaddr;
                    le_fileoff = (*s).fileoff;
                    le_filesize = (*s).filesize;
                    have_le = true;
                }
            }
            lc = lc.add((*l).cmdsize as usize);
        }
        if !have_sym || !have_le || nsyms == 0 {
            return;
        }
        if (symoff as u64) < le_fileoff
            || (stroff as u64) < le_fileoff
            || symoff as u64 + nsyms as u64 * mem::size_of::<Nlist64>() as u64
                > le_fileoff + le_filesize
            || stroff as u64 + strsize as u64 > le_fileoff + le_filesize
        {
            return;
        }
        let symtab = le_vmaddr
            .wrapping_add(slide)
            .wrapping_add(symoff as u64 - le_fileoff);
        let strtab = le_vmaddr
            .wrapping_add(slide)
            .wrapping_add(stroff as u64 - le_fileoff);
        let mut best_val = 0u64;
        let mut best_strx = 0u32;
        let mut found = false;
        for i in 0..nsyms {
            let n = ocerz_g2h(symtab.wrapping_add(i as u64 * mem::size_of::<Nlist64>() as u64))
                .cast::<Nlist64>();
            if (*n).n_type & N_STAB != 0 || (*n).n_type & N_TYPE != N_SECT {
                continue;
            }
            let strx = (*n).n_un.n_strx;
            if strx == 0 || strx >= strsize {
                continue;
            }
            let val = (*n).n_value.wrapping_add(slide);
            if val > addr {
                continue;
            }
            if !found || val > best_val {
                best_val = val;
                best_strx = strx;
                found = true;
            }
        }
        if !found {
            return;
        }
        let mut namep = strtab.wrapping_add(best_strx as u64);
        if *(ocerz_g2h(namep).cast::<u8>()) == b'_' {
            namep = namep.wrapping_add(1);
        }
        *saddr_out = best_val;
        *sname_out = namep;
    }
}

unsafe fn set_add(
    out: *mut u64,
    n: c_int,
    cap: c_int,
    seen: *mut u64,
    mask: u32,
    mh: u64,
) -> c_int {
    unsafe {
        if mh == 0 || out.is_null() || seen.is_null() {
            return n;
        }
        let h = ocerz_g2h(mh).cast::<MachHeader64>();
        if (*h).magic != MH_MAGIC_64 {
            return n;
        }
        let mut i = ((mh.wrapping_mul(0x9e3779b97f4a7c15) >> 40) as u32) & mask;
        while *seen.add(i as usize) != 0 {
            if *seen.add(i as usize) == mh {
                return n;
            }
            i = (i + 1) & mask;
        }
        if n >= cap {
            return n;
        }
        *seen.add(i as usize) = mh;
        *out.add(n as usize) = mh;
        n + 1
    }
}

pub(crate) unsafe fn set_has(seen: *const u64, mask: u32, mh: u64) -> bool {
    unsafe {
        if mh == 0 || seen.is_null() {
            return false;
        }
        let mut i = ((mh.wrapping_mul(0x9e3779b97f4a7c15) >> 40) as u32) & mask;
        while *seen.add(i as usize) != 0 {
            if *seen.add(i as usize) == mh {
                return true;
            }
            i = (i + 1) & mask;
        }
        false
    }
}

unsafe fn closure_add(mh: u64) {
    unsafe {
        g_closure_n = set_add(
            g_closure_mh,
            g_closure_n,
            g_closure_cap,
            g_closure_hash,
            g_closure_hash_mask,
            mh,
        );
    }
}

pub(crate) unsafe fn image_closure_walk(
    cache: *mut OcerzCache,
    root: u64,
    out: *mut u64,
    mut n: c_int,
    cap: c_int,
    seen: *mut u64,
    mask: u32,
) -> c_int {
    unsafe {
        let start = n;
        n = set_add(out, n, cap, seen, mask, root);
        let mut i = start;
        while i < n {
            let h = ocerz_g2h(*out.add(i as usize)).cast::<MachHeader64>();
            let mut lc = h.add(1).cast::<u8>();
            for _ in 0..(*h).ncmds {
                let l = lc.cast::<LoadCommand>();
                if (*l).cmd == LC_LOAD_DYLIB
                    || (*l).cmd == LC_LOAD_WEAK_DYLIB
                    || (*l).cmd == LC_REEXPORT_DYLIB
                    || (*l).cmd == LC_LOAD_UPWARD_DYLIB
                {
                    let name_off = ptr::read_unaligned(lc.add(8).cast::<u32>());
                    if name_off < (*l).cmdsize {
                        n = set_add(
                            out,
                            n,
                            cap,
                            seen,
                            mask,
                            cache_find_path(cache, lc.add(name_off as usize).cast()),
                        );
                    }
                }
                lc = lc.add((*l).cmdsize as usize);
            }
            i += 1;
        }
        n
    }
}

pub(crate) unsafe fn compute_closure(cache: *mut OcerzCache, main_mh: u64) {
    unsafe {
        g_closure_n = 0;
        if !g_closure_hash.is_null() {
            libc::memset(
                g_closure_hash.cast(),
                0,
                (g_closure_hash_mask as usize + 1) * mem::size_of::<u64>(),
            );
        }
        g_closure_n = image_closure_walk(
            cache,
            main_mh,
            g_closure_mh,
            g_closure_n,
            g_closure_cap,
            g_closure_hash,
            g_closure_hash_mask,
        );
        for i in 0..g_disk_n {
            closure_add(*ptr::addr_of!(g_disk_mh).cast::<u64>().add(i as usize));
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyldapi_register_image(mh: u64, path: *const c_char) {
    unsafe {
        if !libc::getenv(cstr_ptr(c"OCERZ_IMGLOG")).is_null() {
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(c"ocerz: IMGREG mh=%#llx path=%s\n"),
                mh as libc::c_ulonglong,
                if !path.is_null() && *path != 0 {
                    path
                } else {
                    cstr_ptr(c"<NULL>")
                },
            );
        }
        let mut gpath = 0u64;
        if !path.is_null() && *path != 0 {
            let need = libc::strlen(path) as u64 + 1;
            gpath = crate::ffi::ocerz_map_anywhere(need, libc::PROT_READ | libc::PROT_WRITE);
            if gpath != 0 {
                libc::memcpy(ocerz_g2h(gpath), path.cast(), need as usize);
            }
        }
        for i in 0..g_disk_n {
            if *ptr::addr_of!(g_disk_mh).cast::<u64>().add(i as usize) == mh {
                return;
            }
        }
        if (g_disk_n as usize) < DYLDAPI_DISK_MAX {
            *ptr::addr_of_mut!(g_disk_mh)
                .cast::<u64>()
                .add(g_disk_n as usize) = mh;
            *ptr::addr_of_mut!(g_disk_path)
                .cast::<u64>()
                .add(g_disk_n as usize) = gpath;
            g_disk_n += 1;
        }
        closure_add(mh);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyldapi_cache_image_loaded(mh: u64) -> c_int {
    unsafe {
        if g_closure_mh.is_null() {
            1
        } else {
            set_has(g_closure_hash, g_closure_hash_mask, mh) as c_int
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyldapi_register_cache_image(mh: u64) {
    unsafe {
        if mh == 0
            || g_cache.is_null()
            || g_closure_mh.is_null()
            || set_has(g_closure_hash, g_closure_hash_mask, mh)
        {
            return;
        }
        g_closure_n = image_closure_walk(
            g_cache,
            mh,
            g_closure_mh,
            g_closure_n,
            g_closure_cap,
            g_closure_hash,
            g_closure_hash_mask,
        );
        if !libc::getenv(cstr_ptr(c"OCERZ_IMGLOG")).is_null() {
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(c"ocerz: IMGREG cache mh=%#llx closure now %d\n"),
                mh as libc::c_ulonglong,
                g_closure_n,
            );
        }
    }
}

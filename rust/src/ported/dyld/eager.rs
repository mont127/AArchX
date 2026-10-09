//! Cache dependency and segment indexes used to identify eager initialization.

use super::*;
use crate::ported::dyldapi::macho::{LoadCommand, Section64, SegmentCommand64};

#[repr(C)]
#[derive(Clone, Copy)]
struct DepMapEnt {
    path: *const c_char,
    mh: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct SegEnt {
    lo: u64,
    hi: u64,
    mh: u64,
}

static mut G_DEPMAP: [DepMapEnt; 1 << DEPMAP_BITS] = [DepMapEnt {
    path: ptr::null(),
    mh: 0,
}; 1 << DEPMAP_BITS];
static mut G_DEPMAP_BUILT: c_int = 0;
static mut G_SEGS: [SegEnt; SEG_MAX] = [SegEnt {
    lo: 0,
    hi: 0,
    mh: 0,
}; SEG_MAX];
static mut G_SEGS_N: c_int = 0;
pub(super) static mut G_EAGER: [u64; EAGER_MAX] = [0; EAGER_MAX];
pub(super) static mut G_EAGER_N: c_int = 0;
static mut G_EAGER_SET: [u64; EAGER_SET] = [0; EAGER_SET];
static mut G_EAGER_SET_N: c_int = 0;

pub(super) static mut g_tlv_registered: [u64; TLV_REG_MAX] = [0; TLV_REG_MAX];
pub(super) static mut g_tlv_registered_n: c_int = 0;

unsafe fn depmap_hash(mut s: *const c_char) -> u32 {
    let mut h = 2166136261u32;
    while s.read() != 0 {
        h ^= s.read() as u8 as u32;
        h = h.wrapping_mul(16777619);
        s = s.add(1);
    }
    h
}

unsafe fn depmap_build(cache: *mut OcerzCache) {
    let map = ptr::addr_of_mut!(G_DEPMAP).cast::<DepMapEnt>();
    for i in 0..(*cache).images_cnt {
        let mut path = ptr::null();
        let mh = ffi::ocerz_cache_image_addr(cache, i, &mut path);
        if mh == 0 || path.is_null() {
            continue;
        }
        let mut h = (depmap_hash(path) as usize) & ((1 << DEPMAP_BITS) - 1);
        while !(*map.add(h)).path.is_null() {
            if libc::strcmp((*map.add(h)).path, path) == 0 {
                break;
            }
            h = (h + 1) & ((1 << DEPMAP_BITS) - 1);
        }
        if (*map.add(h)).path.is_null() {
            (*map.add(h)).path = path;
            (*map.add(h)).mh = mh;
        }
    }
    G_DEPMAP_BUILT = 1;
}

pub(super) unsafe fn dep_find(cache: *mut OcerzCache, path: *const c_char) -> u64 {
    if cache.is_null() {
        return 0;
    }
    if G_DEPMAP_BUILT == 0 {
        depmap_build(cache);
    }
    let map = ptr::addr_of_mut!(G_DEPMAP).cast::<DepMapEnt>();
    let mut h = (depmap_hash(path) as usize) & ((1 << DEPMAP_BITS) - 1);
    for _ in 0..(1 << DEPMAP_BITS) {
        let ent = map.add(h);
        if (*ent).path.is_null() {
            break;
        }
        if libc::strcmp((*ent).path, path) == 0 {
            return (*ent).mh;
        }
        h = (h + 1) & ((1 << DEPMAP_BITS) - 1);
    }
    ffi::ocerz_cache_find_alias(cache, path)
}

pub(super) unsafe fn dep_mh(cache: *mut OcerzCache, path: *const c_char) -> u64 {
    let mh = dep_find(cache, path);
    if mh != 0 {
        return mh;
    }
    let mut img = dimg_find_by_install_name(path);
    if img.is_null() {
        img = dimg_find_by_path(path);
    }
    if img.is_null() { 0 } else { (*img).load_base }
}

pub(super) unsafe fn image_slide_d(mh: u64) -> i64 {
    let h = crate::ported::dyldapi::hostmem::ocerz_g2h(mh).cast::<u8>();
    let ncmds = rd32(h.add(16));
    let mut lc = h.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        let l = lc.cast::<LoadCommand>();
        if (*l).cmd == LC_SEGMENT_64 {
            let s = lc.cast::<SegmentCommand64>();
            if (*s).fileoff == 0 && (*s).filesize != 0 {
                return mh as i64 - (*s).vmaddr as i64;
            }
        }
        lc = lc.add((*l).cmdsize as usize);
    }
    0
}

unsafe extern "C" fn seg_cmp(a: *const c_void, b: *const c_void) -> c_int {
    let x = a.cast::<SegEnt>();
    let y = b.cast::<SegEnt>();
    if (*x).lo < (*y).lo {
        -1
    } else if (*x).lo > (*y).lo {
        1
    } else {
        0
    }
}

unsafe fn build_segs(cache: *mut OcerzCache) {
    G_SEGS_N = 0;
    let segs = ptr::addr_of_mut!(G_SEGS).cast::<SegEnt>();
    for i in 0..(*cache).images_cnt {
        let mut path = ptr::null();
        let mh = ffi::ocerz_cache_image_addr(cache, i, &mut path);
        if mh == 0 || rd32(mh as *const u8) != MH_MAGIC_64 {
            continue;
        }
        let slide = image_slide_d(mh);
        let ncmds = rd32((mh as *const u8).add(16));
        let mut lc = (mh as *const u8).add(core::mem::size_of::<MachHeader64>());
        for _ in 0..ncmds {
            let l = lc.cast::<LoadCommand>();
            if (*l).cmd == LC_SEGMENT_64 {
                let s = lc.cast::<SegmentCommand64>();
                if (*s).vmsize != 0 && G_SEGS_N < SEG_MAX as c_int {
                    let ent = segs.add(G_SEGS_N as usize);
                    (*ent).lo = (*s).vmaddr.wrapping_add(slide as u64);
                    (*ent).hi = (*s)
                        .vmaddr
                        .wrapping_add(slide as u64)
                        .wrapping_add((*s).vmsize);
                    (*ent).mh = mh;
                    G_SEGS_N += 1;
                }
            }
            lc = lc.add((*l).cmdsize as usize);
        }
    }
    libc::qsort(
        segs.cast(),
        G_SEGS_N as usize,
        core::mem::size_of::<SegEnt>(),
        Some(seg_cmp),
    );
}

unsafe fn seg_index(addr: u64) -> c_int {
    let segs = ptr::addr_of_mut!(G_SEGS).cast::<SegEnt>();
    let mut lo = 0;
    let mut hi = G_SEGS_N - 1;
    let mut best = -1;
    while lo <= hi {
        let mid = (lo + hi) / 2;
        if (*segs.add(mid as usize)).lo <= addr {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if best >= 0 && addr < (*segs.add(best as usize)).hi {
        best
    } else {
        -1
    }
}

unsafe fn eager_slot(mh: u64) -> usize {
    let mut at = (mh.wrapping_mul(0x9e37_79b9_7f4a_7c15) >> 40) as usize & (EAGER_SET - 1);
    let set = ptr::addr_of_mut!(G_EAGER_SET).cast::<u64>();
    while *set.add(at) != 0 && *set.add(at) != mh {
        at = (at + 1) & (EAGER_SET - 1);
    }
    at
}

pub(super) unsafe fn eager_has(mh: u64) -> bool {
    if G_EAGER_SET_N != G_EAGER_N {
        ptr::write_bytes(ptr::addr_of_mut!(G_EAGER_SET).cast::<u64>(), 0, EAGER_SET);
        for i in 0..G_EAGER_N {
            let value = *ptr::addr_of!(G_EAGER).cast::<u64>().add(i as usize);
            let at = eager_slot(value);
            *ptr::addr_of_mut!(G_EAGER_SET).cast::<u64>().add(at) = value;
        }
        G_EAGER_SET_N = G_EAGER_N;
    }
    *ptr::addr_of_mut!(G_EAGER_SET)
        .cast::<u64>()
        .add(eager_slot(mh))
        == mh
}

unsafe fn eager_add(mh: u64) {
    if mh != 0 && !eager_has(mh) && G_EAGER_N < EAGER_MAX as c_int {
        let eager = ptr::addr_of_mut!(G_EAGER).cast::<u64>();
        eager.add(G_EAGER_N as usize).write(mh);
        G_EAGER_N += 1;
        *ptr::addr_of_mut!(G_EAGER_SET)
            .cast::<u64>()
            .add(eager_slot(mh)) = mh;
        G_EAGER_SET_N = G_EAGER_N;
    }
}

unsafe fn scan_uses(mh: u64) {
    let slide = image_slide_d(mh);
    let h = crate::ported::dyldapi::hostmem::ocerz_g2h(mh).cast::<u8>();
    let ncmds = rd32(h.add(16));
    let mut lc = h.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        let l = lc.cast::<LoadCommand>();
        if (*l).cmd == LC_SEGMENT_64 {
            let s = lc.cast::<SegmentCommand64>();
            let sections = s.add(1).cast::<Section64>();
            for j in 0..(*s).nsects {
                let sc = sections.add(j as usize);
                let ty = (*sc).flags & 0xff;
                let sectname = (*sc).sectname.as_ptr().cast::<c_char>();
                let isptr = ty == 6
                    || ty == 7
                    || libc::strncmp(sectname, cstr_ptr(c"__got"), 16) == 0
                    || libc::strncmp(sectname, cstr_ptr(c"__la_symbol_ptr"), 16) == 0
                    || libc::strncmp(sectname, cstr_ptr(c"__auth_got"), 16) == 0
                    || libc::strncmp(sectname, cstr_ptr(c"__objc_classrefs"), 16) == 0
                    || libc::strncmp(sectname, cstr_ptr(c"__objc_superrefs"), 16) == 0
                    || libc::strncmp(sectname, cstr_ptr(c"__objc_protorefs"), 16) == 0
                    || libc::strncmp(sectname, cstr_ptr(c"__objc_nlclslist"), 16) == 0
                    || libc::strncmp(sectname, cstr_ptr(c"__objc_catlist"), 16) == 0
                    || libc::strncmp(sectname, cstr_ptr(c"__cfstring"), 16) == 0
                    || libc::strncmp(sectname, cstr_ptr(c"__objc_classlist"), 16) == 0;
                if isptr {
                    let a = (*sc).addr.wrapping_add(slide as u64);
                    let e = a.wrapping_add((*sc).size);
                    let mut last_lo = 1u64;
                    let mut last_hi = 0u64;
                    let mut pp = a;
                    while pp.wrapping_add(8) <= e {
                        let v = rd64(crate::ported::dyldapi::hostmem::ocerz_g2h(pp).cast());
                        if v >= last_lo && v < last_hi {
                            pp = pp.wrapping_add(8);
                            continue;
                        }
                        let at = seg_index(v);
                        if at >= 0 {
                            let seg = ptr::addr_of_mut!(G_SEGS).cast::<SegEnt>().add(at as usize);
                            last_lo = (*seg).lo;
                            last_hi = (*seg).hi;
                            if (*seg).mh != mh {
                                eager_add((*seg).mh);
                            }
                        }
                        pp = pp.wrapping_add(8);
                    }
                }
            }
        }
        lc = lc.add((*l).cmdsize as usize);
    }
}

unsafe fn eager_add_direct_deps(cache: *mut OcerzCache, mh: u64) {
    let h = crate::ported::dyldapi::hostmem::ocerz_g2h(mh).cast::<u8>();
    if rd32(h) != MH_MAGIC_64 {
        return;
    }
    let ncmds = rd32(h.add(16));
    let mut lc = h.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        let cmd = rd32(lc);
        if cmd == 0xc || cmd == 0x8000_0018 || cmd == 0x8000_001f || cmd == 0x8000_0023 {
            let noff = rd32(lc.add(8));
            if noff < rd32(lc.add(4)) {
                eager_add(dep_mh(cache, lc.add(noff as usize).cast()));
            }
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
}

unsafe fn is_libsystem_path(path: *const c_char) -> bool {
    !libc::strstr(path, cstr_ptr(c"/usr/lib/system/")).is_null()
        || libc::strcmp(path, cstr_ptr(c"/usr/lib/libSystem.B.dylib")) == 0
}

pub(super) unsafe fn closure_links_cf(cache: *mut OcerzCache, main_mh: u64) -> bool {
    static mut SEEN: [u64; EAGER_MAX] = [0; EAGER_MAX];
    let seen = ptr::addr_of_mut!(SEEN).cast::<u64>();
    let mut n = 0;
    if main_mh != 0 {
        seen.add(n).write(main_mh);
        n += 1;
    }
    let mut i = 0;
    while i < n {
        let h = crate::ported::dyldapi::hostmem::ocerz_g2h(seen.add(i).read()).cast::<u8>();
        if rd32(h) != MH_MAGIC_64 {
            i += 1;
            continue;
        }
        let ncmds = rd32(h.add(16));
        let mut lc = h.add(core::mem::size_of::<MachHeader64>());
        for _ in 0..ncmds {
            let cmd = rd32(lc);
            if cmd == 0xc || cmd == 0x8000_0018 || cmd == 0x8000_001f || cmd == 0x8000_0023 {
                let noff = rd32(lc.add(8));
                if noff < rd32(lc.add(4)) {
                    let dp = lc.add(noff as usize).cast::<c_char>();
                    if !libc::strstr(dp, cstr_ptr(c"/CoreFoundation.framework/")).is_null()
                        || !libc::strstr(dp, cstr_ptr(c"/Foundation.framework/")).is_null()
                        || !libc::strstr(dp, cstr_ptr(c"/AppKit.framework/")).is_null()
                    {
                        return true;
                    }
                    if !is_libsystem_path(dp) {
                        let dmh = dep_mh(cache, dp);
                        if dmh != 0 && n < EAGER_MAX {
                            let mut dup = false;
                            for k in 0..n {
                                if seen.add(k).read() == dmh {
                                    dup = true;
                                    break;
                                }
                            }
                            if !dup {
                                seen.add(n).write(dmh);
                                n += 1;
                            }
                        }
                    }
                }
            }
            lc = lc.add(rd32(lc.add(4)) as usize);
        }
        i += 1;
    }
    false
}

pub(super) unsafe fn compute_eager_set(cache: *mut OcerzCache, main_mh: u64) {
    build_segs(cache);
    G_EAGER_N = 0;
    eager_add(main_mh);
    eager_add_direct_deps(cache, main_mh);
    for i in 0..(*cache).images_cnt {
        let mut path = ptr::null();
        let mh = ffi::ocerz_cache_image_addr(cache, i, &mut path);
        if mh != 0
            && !path.is_null()
            && (!libc::strstr(path, cstr_ptr(c"/usr/lib/system/")).is_null()
                || libc::strcmp(path, cstr_ptr(c"/usr/lib/libSystem.B.dylib")) == 0)
        {
            eager_add(mh);
        }
    }
    let root_n = G_EAGER_N;
    let mut i = 0;
    while i < G_EAGER_N {
        let mh = ptr::addr_of!(G_EAGER).cast::<u64>().add(i as usize).read();
        eager_add_direct_deps(cache, mh);
        scan_uses(mh);
        i += 1;
    }
    if !libc::getenv(cstr_ptr(c"OCERZ_INITLOG")).is_null() {
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"dynamic: eager init set: root=%d eager=%d (of closure)\n"),
            root_n,
            G_EAGER_N,
        );
    }
}

pub(super) unsafe fn tlv_is_registered(mh: u64) -> bool {
    for i in 0..g_tlv_registered_n {
        if ptr::addr_of!(g_tlv_registered)
            .cast::<u64>()
            .add(i as usize)
            .read()
            == mh
        {
            return true;
        }
    }
    false
}

pub(super) unsafe fn tlv_register(mh: u64) {
    if !tlv_is_registered(mh) && g_tlv_registered_n < TLV_REG_MAX as c_int {
        ptr::addr_of_mut!(g_tlv_registered)
            .cast::<u64>()
            .add(g_tlv_registered_n as usize)
            .write(mh);
        g_tlv_registered_n += 1;
    }
}

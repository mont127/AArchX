//! Open-addressed cache image indexes and per-image load-command metadata.

use core::ffi::{c_char, c_void};
use core::mem;
use core::ptr;
use core::sync::atomic::{AtomicPtr, Ordering};

use crate::ffi::OcerzCache;

use super::rd32;

pub(super) const CIX_NONE: u32 = 0xffff_ffff;
pub(super) const DEP_LOAD: u8 = 0;
pub(super) const DEP_REEXPORT: u8 = 1;
pub(super) const DEP_UPWARD: u8 = 2;

#[repr(C)]
pub(super) struct CacheImgInfo {
    pub ts: *const u8,
    pub te: *const u8,
    pub ndeps: u32,
    pub dep: *mut u32,
    pub dep_kind: *mut u8,
}

#[repr(C)]
pub(super) struct CacheIndex {
    pub c: *mut OcerzCache,
    pub n: u32,
    pub mask: u32,
    pub by_path: *mut u32,
    pub by_mh: *mut u32,
    pub info: *mut *mut CacheImgInfo,
}

static G_CIX: AtomicPtr<CacheIndex> = AtomicPtr::new(ptr::null_mut());

#[inline]
pub(super) unsafe fn cix_str_hash(s: *const c_char) -> u32 {
    unsafe {
        let mut h = 2_166_136_261u32;
        let mut p = s.cast::<u8>();
        while *p != 0 {
            h = (h ^ (*p as u32)).wrapping_mul(16_777_619);
            p = p.add(1);
        }
        h
    }
}

#[inline]
pub(super) fn cix_mh_hash(mh: u64) -> u32 {
    (mh.wrapping_mul(0x9e37_79b9_7f4a_7c15) >> 32) as u32
}

pub(super) unsafe fn cix_get(c: *mut OcerzCache) -> *mut CacheIndex {
    unsafe {
        let current = G_CIX.load(Ordering::Acquire);
        if !current.is_null() {
            return if (*current).c == c {
                current
            } else {
                ptr::null_mut()
            };
        }
        if (*c).mapped == 0 || (*c).images_cnt == 0 {
            return ptr::null_mut();
        }
        let mut cap = 1024u32;
        while cap < (*c).images_cnt.wrapping_mul(4) {
            cap = cap.wrapping_shl(1);
        }
        let ix = libc::calloc(1, mem::size_of::<CacheIndex>()).cast::<CacheIndex>();
        let bp = if ix.is_null() {
            ptr::null_mut()
        } else {
            libc::calloc(cap as usize, mem::size_of::<u32>()).cast::<u32>()
        };
        let bm = if bp.is_null() {
            ptr::null_mut()
        } else {
            libc::calloc(cap as usize, mem::size_of::<u32>()).cast::<u32>()
        };
        let info = if bm.is_null() {
            ptr::null_mut()
        } else {
            libc::calloc(
                (*c).images_cnt as usize,
                mem::size_of::<*mut CacheImgInfo>(),
            )
            .cast::<*mut CacheImgInfo>()
        };
        if info.is_null() {
            libc::free(bm.cast::<c_void>());
            libc::free(bp.cast::<c_void>());
            libc::free(ix.cast::<c_void>());
            return ptr::null_mut();
        }
        (*ix).c = c;
        (*ix).n = (*c).images_cnt;
        (*ix).mask = cap.wrapping_sub(1);
        (*ix).by_path = bp;
        (*ix).by_mh = bm;
        (*ix).info = info;
        let mut i = 0u32;
        while i < (*ix).n {
            let mut p = ptr::null();
            let mh = super::map::ocerz_cache_image_addr(c, i, &mut p);
            if mh != 0 {
                if !p.is_null() {
                    let mut h = cix_str_hash(p) & (*ix).mask;
                    while *bp.add(h as usize) != 0 {
                        h = h.wrapping_add(1) & (*ix).mask;
                    }
                    bp.add(h as usize).write(i.wrapping_add(1));
                }
                let mut h = cix_mh_hash(mh) & (*ix).mask;
                while *bm.add(h as usize) != 0 {
                    h = h.wrapping_add(1) & (*ix).mask;
                }
                bm.add(h as usize).write(i.wrapping_add(1));
            }
            i = i.wrapping_add(1);
        }
        match G_CIX.compare_exchange(ptr::null_mut(), ix, Ordering::AcqRel, Ordering::Acquire) {
            Ok(_) => ix,
            Err(expected) => {
                libc::free(info.cast::<c_void>());
                libc::free(bm.cast::<c_void>());
                libc::free(bp.cast::<c_void>());
                libc::free(ix.cast::<c_void>());
                if !expected.is_null() && (*expected).c == c {
                    expected
                } else {
                    ptr::null_mut()
                }
            }
        }
    }
}

pub(super) unsafe fn cix_by_path(ix: *mut CacheIndex, path: *const c_char) -> u32 {
    unsafe {
        let mut h = cix_str_hash(path) & (*ix).mask;
        loop {
            let stored = *(*ix).by_path.add(h as usize);
            if stored == 0 {
                return CIX_NONE;
            }
            let i = stored.wrapping_sub(1);
            let mut p = ptr::null();
            super::map::ocerz_cache_image_addr((*ix).c, i, &mut p);
            if !p.is_null() && libc::strcmp(p, path) == 0 {
                return i;
            }
            h = h.wrapping_add(1) & (*ix).mask;
        }
    }
}

pub(super) unsafe fn cix_by_mh(ix: *mut CacheIndex, mh: u64) -> u32 {
    unsafe {
        let mut h = cix_mh_hash(mh) & (*ix).mask;
        loop {
            let stored = *(*ix).by_mh.add(h as usize);
            if stored == 0 {
                return CIX_NONE;
            }
            let i = stored.wrapping_sub(1);
            if super::map::ocerz_cache_image_addr((*ix).c, i, ptr::null_mut()) == mh {
                return i;
            }
            h = h.wrapping_add(1) & (*ix).mask;
        }
    }
}

pub(super) unsafe fn cix_info(ix: *mut CacheIndex, i: u32) -> *mut CacheImgInfo {
    unsafe {
        let slot = (*ix).info.add(i as usize);
        let atomic = AtomicPtr::from_ptr(slot);
        let inf = atomic.load(Ordering::Acquire);
        if !inf.is_null() {
            return inf;
        }
        let mh = super::map::ocerz_cache_image_addr((*ix).c, i, ptr::null_mut());
        if mh == 0 {
            return ptr::null_mut();
        }
        let m = mh as usize as *const u8;
        let ncmds = rd32(m.add(16));
        let mut nd = 0u32;
        let mut lc = m.add(32);
        let mut k = 0u32;
        while k < ncmds {
            let cmd = rd32(lc);
            let size = rd32(lc.add(4));
            if size < 8 {
                break;
            }
            if super::is_dylib_cmd(cmd) {
                nd = nd.wrapping_add(1);
            }
            lc = lc.add(size as usize);
            k = k.wrapping_add(1);
        }
        let total = mem::size_of::<CacheImgInfo>()
            .wrapping_add((nd as usize).wrapping_mul(mem::size_of::<u32>() + 1));
        let inf = libc::calloc(1, total).cast::<CacheImgInfo>();
        if inf.is_null() {
            return ptr::null_mut();
        }
        (*inf).dep = inf.add(1).cast::<u32>();
        (*inf).dep_kind = (*inf).dep.add(nd as usize).cast::<u8>();
        let mut ts = ptr::null();
        let mut te = ptr::null();
        if super::resolve::dylib_export_region(mh, &mut ts, &mut te) != 0 {
            ts = ptr::null();
            te = ptr::null();
        }
        (*inf).ts = ts;
        (*inf).te = te;
        lc = m.add(32);
        k = 0;
        while k < ncmds && (*inf).ndeps < nd {
            let cmd = rd32(lc);
            let size = rd32(lc.add(4));
            if size < 8 {
                break;
            }
            if super::is_dylib_cmd(cmd) {
                let noff = rd32(lc.add(8));
                let mut di = CIX_NONE;
                if noff < size {
                    let path = lc.add(noff as usize).cast::<c_char>();
                    di = cix_by_path(ix, path);
                    if di == CIX_NONE {
                        let amh = super::resolve::ocerz_cache_find_alias((*ix).c, path);
                        if amh != 0 {
                            di = cix_by_mh(ix, amh);
                        }
                    }
                }
                let upward = cmd == super::LC_LOAD_UPWARD_DYLIB
                    || (cmd != super::LC_REEXPORT_DYLIB
                        && size >= super::DYLIB_USE_COMMAND_SIZE
                        && noff == super::DYLIB_USE_COMMAND_SIZE
                        && rd32(lc.add(12)) == super::DYLIB_USE_MARKER
                        && rd32(lc.add(24)) & super::DYLIB_USE_UPWARD != 0);
                let n = (*inf).ndeps as usize;
                (*inf).dep.add(n).write(di);
                (*inf)
                    .dep_kind
                    .add(n)
                    .write(if cmd == super::LC_REEXPORT_DYLIB {
                        DEP_REEXPORT
                    } else if upward {
                        DEP_UPWARD
                    } else {
                        DEP_LOAD
                    });
                (*inf).ndeps = (*inf).ndeps.wrapping_add(1);
            }
            lc = lc.add(size as usize);
            k = k.wrapping_add(1);
        }
        match atomic.compare_exchange(ptr::null_mut(), inf, Ordering::AcqRel, Ordering::Acquire) {
            Ok(_) => inf,
            Err(expected) => {
                libc::free(inf.cast::<c_void>());
                expected
            }
        }
    }
}

pub(super) unsafe fn cache_image_by_path(c: *mut OcerzCache, path: *const c_char) -> u64 {
    unsafe {
        let ix = cix_get(c);
        if !ix.is_null() {
            let i = cix_by_path(ix, path);
            if i != CIX_NONE {
                return super::map::ocerz_cache_image_addr(c, i, ptr::null_mut());
            }
            return super::resolve::ocerz_cache_find_alias(c, path);
        }
        let mut i = 0u32;
        while i < (*c).images_cnt {
            let mut p = ptr::null();
            let mh = super::map::ocerz_cache_image_addr(c, i, &mut p);
            if mh != 0 && !p.is_null() && libc::strcmp(p, path) == 0 {
                return mh;
            }
            i = i.wrapping_add(1);
        }
        super::resolve::ocerz_cache_find_alias(c, path)
    }
}

pub(super) unsafe fn cache_image_by_path_memo(c: *mut OcerzCache, path: *const c_char) -> u64 {
    unsafe { cache_image_by_path(c, path) }
}

//! Export-trie parsing, dependency traversal, and symbol-resolution memos.

use core::ffi::{c_char, c_int, c_void};
use core::ptr;

use crate::ffi::OcerzCache;

use super::index::{
    CIX_NONE, CacheIndex, DEP_REEXPORT, DEP_UPWARD, cache_image_by_path, cache_image_by_path_memo,
    cix_by_mh, cix_get, cix_info,
};
use super::{rd32, rd64, uleb};

const EXPORT_FLAGS_REEXPORT: u64 = 0x08;
const EXPORT_FLAGS_KIND_MASK: u64 = 0x03;
const EXPORT_FLAGS_KIND_ABSOLUTE: u64 = 0x02;

pub(super) unsafe fn dylib_export_region(
    mh_addr: u64,
    trie_start: *mut *const u8,
    trie_end: *mut *const u8,
) -> c_int {
    unsafe {
        let m = mh_addr as usize as *const u8;
        let ncmds = rd32(m.add(16));
        let mut lc = m.add(32);
        let mut le_vmaddr = 0u64;
        let mut le_fileoff = 0u64;
        let mut have_le = 0;
        let mut exp_off = 0u32;
        let mut exp_size = 0u32;
        let mut k = 0u32;
        while k < ncmds {
            let cmd = rd32(lc);
            let csize = rd32(lc.add(4));
            if csize < 8 {
                return -1;
            }
            if cmd == super::LC_SEGMENT_64 {
                if libc::memcmp(lc.add(8).cast(), c"__LINKEDIT".as_ptr().cast(), 10) == 0 {
                    le_vmaddr = rd64(lc.add(24));
                    le_fileoff = rd64(lc.add(40));
                    have_le = 1;
                }
            } else if cmd == super::LC_DYLD_EXPORTS_TRIE {
                exp_off = rd32(lc.add(8));
                exp_size = rd32(lc.add(12));
            } else if (cmd == super::LC_DYLD_INFO || cmd == super::LC_DYLD_INFO_ONLY)
                && exp_off == 0
            {
                exp_off = rd32(lc.add(40));
                exp_size = rd32(lc.add(44));
            }
            lc = lc.add(csize as usize);
            k = k.wrapping_add(1);
        }
        if have_le == 0 || exp_off == 0 || exp_size == 0 {
            return -1;
        }
        let addr = le_vmaddr.wrapping_add((exp_off as u64).wrapping_sub(le_fileoff));
        trie_start.write(addr as usize as *const u8);
        trie_end.write((addr as usize as *const u8).add(exp_size as usize));
        0
    }
}

unsafe fn trie_lookup(
    start: *const u8,
    end: *const u8,
    sym: *const c_char,
    is_reexport: *mut c_int,
    reexport_ord: *mut u64,
    reexport_name: *mut *const c_char,
    found: *mut c_int,
    flags_out: *mut u64,
) -> u64 {
    unsafe {
        is_reexport.write(0);
        found.write(0);
        flags_out.write(0);
        let mut p = start;
        let mut s = sym;
        while p < end {
            let term = uleb(&mut p, end);
            if *s == 0 {
                if term == 0 {
                    return 0;
                }
                let mut tp = p;
                let flags = uleb(&mut tp, end);
                flags_out.write(flags);
                found.write(1);
                if flags & EXPORT_FLAGS_REEXPORT != 0 {
                    is_reexport.write(1);
                    reexport_ord.write(uleb(&mut tp, end));
                    reexport_name.write(tp.cast::<c_char>());
                    return 1;
                }
                return uleb(&mut tp, end);
            }
            p = p.add(term as usize);
            if p >= end {
                return 0;
            }
            let children = *p;
            p = p.add(1);
            let mut next: *const u8 = ptr::null();
            let mut i = 0u32;
            while i < children as u32 {
                let edge = p.cast::<c_char>();
                let elen = libc::strlen(edge);
                p = p.add(elen + 1);
                let child_off = uleb(&mut p, end);
                if next.is_null() && libc::strncmp(s, edge, elen) == 0 {
                    s = s.add(elen);
                    next = start.add(child_off as usize);
                }
                i = i.wrapping_add(1);
            }
            if next.is_null() {
                return 0;
            }
            p = next;
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_find_alias(c: *mut OcerzCache, path: *const c_char) -> u64 {
    unsafe {
        if c.is_null() || (*c).mapped == 0 || path.is_null() {
            return 0;
        }
        let taddr = rd64((*c).hdr.add(0x108));
        let tsize = rd64((*c).hdr.add(0x110));
        if taddr == 0 || tsize == 0 || tsize > (64u64 << 20) {
            return 0;
        }
        let start = taddr as usize as *const u8;
        let end = start.add(tsize as usize);
        let mut p = start;
        let mut s = path;
        let mut depth = 0;
        while depth < 512 && p < end {
            let term = uleb(&mut p, end);
            if *s == 0 {
                if term == 0 {
                    return 0;
                }
                let mut tp = p;
                let idx = uleb(&mut tp, end);
                return if idx < (*c).images_cnt as u64 {
                    super::map::ocerz_cache_image_addr(c, idx as u32, ptr::null_mut())
                } else {
                    0
                };
            }
            if term > end.offset_from(p) as u64 {
                return 0;
            }
            p = p.add(term as usize);
            if p >= end {
                return 0;
            }
            let children = *p;
            p = p.add(1);
            let mut next: *const u8 = ptr::null();
            let mut i = 0u32;
            while i < children as u32 && p < end {
                let edge = p.cast::<c_char>();
                let elen = libc::strnlen(edge, end.offset_from(p) as usize);
                p = p.add(elen + 1);
                let child_off = uleb(&mut p, end);
                if next.is_null() && libc::strncmp(s, edge, elen) == 0 {
                    s = s.add(elen);
                    next = start.add(child_off as usize);
                }
                i = i.wrapping_add(1);
            }
            if next.is_null() {
                return 0;
            }
            p = next;
            depth += 1;
        }
        0
    }
}

unsafe fn dylib_ordinal_name(mh: u64, ord: u64) -> *const c_char {
    unsafe {
        let m = mh as usize as *const u8;
        let ncmds = rd32(m.add(16));
        let mut lc = m.add(32);
        let mut n = 0u64;
        let mut i = 0u32;
        while i < ncmds {
            let cmd = rd32(lc);
            if super::is_dylib_cmd(cmd) {
                n = n.wrapping_add(1);
                if n == ord {
                    return lc.add(rd32(lc.add(8)) as usize).cast::<c_char>();
                }
            }
            lc = lc.add(rd32(lc.add(4)) as usize);
            i = i.wrapping_add(1);
        }
        ptr::null()
    }
}

pub(super) unsafe fn resolve_in_dylib(
    c: *mut OcerzCache,
    mh: u64,
    sym: *const c_char,
    depth: c_int,
    found: *mut c_int,
) -> u64 {
    unsafe {
        if depth > 16 {
            return 0;
        }
        let ix = cix_get(c);
        let ii = if !ix.is_null() {
            cix_by_mh(ix, mh)
        } else {
            CIX_NONE
        };
        let inf = if ii != CIX_NONE {
            cix_info(ix, ii)
        } else {
            ptr::null_mut()
        };
        if !inf.is_null() {
            if (*inf).ts.is_null() {
                return 0;
            }
            let mut reexp = 0;
            let mut lfound = 0;
            let mut ord = 0;
            let mut lflags = 0;
            let mut imp = ptr::null();
            let off = trie_lookup(
                (*inf).ts,
                (*inf).te,
                sym,
                &mut reexp,
                &mut ord,
                &mut imp,
                &mut lfound,
                &mut lflags,
            );
            if lfound == 0 {
                let mut k = 0u32;
                while k < (*inf).ndeps {
                    let di = *(*inf).dep.add(k as usize);
                    if *(*inf).dep_kind.add(k as usize) != DEP_REEXPORT || di == CIX_NONE {
                        k = k.wrapping_add(1);
                        continue;
                    }
                    let tmh = super::map::ocerz_cache_image_addr(c, di, ptr::null_mut());
                    if tmh != 0 && tmh != mh {
                        let mut f = 0;
                        let v = resolve_in_dylib(c, tmh, sym, depth.wrapping_add(1), &mut f);
                        if f != 0 {
                            found.write(1);
                            return v;
                        }
                    }
                    k = k.wrapping_add(1);
                }
                return 0;
            }
            if reexp == 0 {
                found.write(1);
                return if lflags & EXPORT_FLAGS_KIND_MASK == EXPORT_FLAGS_KIND_ABSOLUTE {
                    off
                } else {
                    mh.wrapping_add(off)
                };
            }
            if ord == 0
                || ord > (*inf).ndeps as u64
                || *(*inf).dep.add(ord.wrapping_sub(1) as usize) == CIX_NONE
            {
                return 0;
            }
            let tmh = super::map::ocerz_cache_image_addr(
                c,
                *(*inf).dep.add(ord.wrapping_sub(1) as usize),
                ptr::null_mut(),
            );
            if tmh == 0 {
                return 0;
            }
            let want = if !imp.is_null() && *imp != 0 {
                imp
            } else {
                sym
            };
            return resolve_in_dylib(c, tmh, want, depth.wrapping_add(1), found);
        }
        let mut ts = ptr::null();
        let mut te = ptr::null();
        if dylib_export_region(mh, &mut ts, &mut te) != 0 {
            return 0;
        }
        let mut reexp = 0;
        let mut ord = 0;
        let mut imp = ptr::null();
        let mut lfound = 0;
        let mut lflags = 0;
        let off = trie_lookup(
            ts,
            te,
            sym,
            &mut reexp,
            &mut ord,
            &mut imp,
            &mut lfound,
            &mut lflags,
        );
        if lfound == 0 {
            let h = mh as usize as *const u8;
            let ncmds = rd32(h.add(16));
            let mut lc = h.add(32);
            let mut i = 0u32;
            while i < ncmds {
                let cmd = rd32(lc);
                let size = rd32(lc.add(4));
                if size < 8 {
                    break;
                }
                if cmd == super::LC_REEXPORT_DYLIB {
                    let noff = rd32(lc.add(8));
                    if noff < size {
                        let tmh = cache_image_by_path_memo(c, lc.add(noff as usize).cast());
                        if tmh != 0 && tmh != mh {
                            let mut f = 0;
                            let v = resolve_in_dylib(c, tmh, sym, depth.wrapping_add(1), &mut f);
                            if f != 0 {
                                found.write(1);
                                return v;
                            }
                        }
                    }
                }
                lc = lc.add(size as usize);
                i = i.wrapping_add(1);
            }
            return 0;
        }
        if reexp == 0 {
            found.write(1);
            return if lflags & EXPORT_FLAGS_KIND_MASK == EXPORT_FLAGS_KIND_ABSOLUTE {
                off
            } else {
                mh.wrapping_add(off)
            };
        }
        let want = if !imp.is_null() && *imp != 0 {
            imp
        } else {
            sym
        };
        let tgt = dylib_ordinal_name(mh, ord);
        if tgt.is_null() {
            return 0;
        }
        let tmh = cache_image_by_path(c, tgt);
        if tmh == 0 {
            return 0;
        }
        resolve_in_dylib(c, tmh, want, depth.wrapping_add(1), found)
    }
}

const RMEMO_SLOTS: u32 = 1 << 16;

#[repr(C)]
#[derive(Clone, Copy)]
struct ResolveMemo {
    name: *mut c_char,
    val: u64,
    found: c_int,
}

static mut G_RMEMO: [ResolveMemo; RMEMO_SLOTS as usize] = [ResolveMemo {
    name: ptr::null_mut(),
    val: 0,
    found: 0,
}; RMEMO_SLOTS as usize];
static mut G_RMEMO_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

unsafe fn rmemo_hash(s: *const c_char) -> u32 {
    unsafe { super::index::cix_str_hash(s) & (RMEMO_SLOTS - 1) }
}

unsafe fn rmemo_find(symbol: *const c_char) -> *mut ResolveMemo {
    unsafe {
        let slots = ptr::addr_of_mut!(G_RMEMO).cast::<ResolveMemo>();
        let mut i = rmemo_hash(symbol);
        let mut n = 0u32;
        while n < 32 {
            let m = slots.add(i as usize);
            if (*m).name.is_null() || libc::strcmp((*m).name, symbol) == 0 {
                return m;
            }
            i = i.wrapping_add(1) & (RMEMO_SLOTS - 1);
            n = n.wrapping_add(1);
        }
        ptr::null_mut()
    }
}

unsafe fn cache_resolve_walk(c: *mut OcerzCache, symbol: *const c_char, found: *mut c_int) -> u64 {
    unsafe {
        let mut i = 0u32;
        while i < (*c).images_cnt {
            let mh = super::map::ocerz_cache_image_addr(c, i, ptr::null_mut());
            if mh != 0 && rd32((mh as usize as *const u8)) == super::MH_MAGIC_64 {
                let mut f = 0;
                let r = resolve_in_dylib(c, mh, symbol, 0, &mut f);
                if f != 0 {
                    found.write(1);
                    return r;
                }
            }
            i = i.wrapping_add(1);
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_resolve_ex(
    c: *mut OcerzCache,
    symbol: *const c_char,
    found: *mut c_int,
) -> u64 {
    unsafe {
        let mut dummy = 0;
        let found = if found.is_null() {
            &mut dummy as *mut c_int
        } else {
            found
        };
        found.write(0);
        if (*c).mapped == 0 {
            return 0;
        }
        let lock = ptr::addr_of_mut!(G_RMEMO_LOCK);
        libc::pthread_mutex_lock(lock);
        let m = rmemo_find(symbol);
        if !m.is_null() && !(*m).name.is_null() {
            found.write((*m).found);
            let v = (*m).val;
            libc::pthread_mutex_unlock(lock);
            return v;
        }
        libc::pthread_mutex_unlock(lock);
        let mut f = 0;
        let v = cache_resolve_walk(c, symbol, &mut f);
        libc::pthread_mutex_lock(lock);
        let m = rmemo_find(symbol);
        if !m.is_null() && (*m).name.is_null() {
            (*m).val = v;
            (*m).found = f;
            (*m).name = libc::strdup(symbol);
        }
        libc::pthread_mutex_unlock(lock);
        found.write(f);
        v
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_resolve(c: *mut OcerzCache, symbol: *const c_char) -> u64 {
    unsafe { ocerz_cache_resolve_ex(c, symbol, ptr::null_mut()) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_resolve_in_image(
    c: *mut OcerzCache,
    path: *const c_char,
    symbol: *const c_char,
    found: *mut c_int,
) -> u64 {
    unsafe {
        let mut dummy = 0;
        let found = if found.is_null() {
            &mut dummy as *mut c_int
        } else {
            found
        };
        found.write(0);
        if (*c).mapped == 0 || path.is_null() || symbol.is_null() {
            return 0;
        }
        let mh = cache_image_by_path_memo(c, path);
        if mh == 0 {
            return 0;
        }
        let mut f = 0;
        let v = resolve_in_dylib(c, mh, symbol, 0, &mut f);
        found.write(f);
        v
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_has_image(c: *mut OcerzCache, mh: u64) -> c_int {
    unsafe {
        if (*c).mapped == 0 || mh == 0 {
            return 0;
        }
        let ix = cix_get(c);
        if !ix.is_null() {
            return (cix_by_mh(ix, mh) != CIX_NONE) as c_int;
        }
        let mut i = 0u32;
        while i < (*c).images_cnt {
            if super::map::ocerz_cache_image_addr(c, i, ptr::null_mut()) == mh {
                return 1;
            }
            i = i.wrapping_add(1);
        }
        0
    }
}

const FMEMO_SLOTS: u32 = 1 << 15;

#[repr(C)]
#[derive(Clone, Copy)]
struct FromMemo {
    mh: u64,
    name: *mut c_char,
    val: u64,
    found: c_int,
}

static mut G_FMEMO: [FromMemo; FMEMO_SLOTS as usize] = [FromMemo {
    mh: 0,
    name: ptr::null_mut(),
    val: 0,
    found: 0,
}; FMEMO_SLOTS as usize];
static mut G_FMEMO_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

unsafe fn fmemo_find(mh: u64, symbol: *const c_char) -> *mut FromMemo {
    unsafe {
        let slots = ptr::addr_of_mut!(G_FMEMO).cast::<FromMemo>();
        let mut i = (super::index::cix_str_hash(symbol) ^ super::index::cix_mh_hash(mh))
            & (FMEMO_SLOTS - 1);
        let mut n = 0u32;
        while n < 16 {
            let m = slots.add(i as usize);
            if (*m).name.is_null() || ((*m).mh == mh && libc::strcmp((*m).name, symbol) == 0) {
                return m;
            }
            i = i.wrapping_add(1) & (FMEMO_SLOTS - 1);
            n = n.wrapping_add(1);
        }
        ptr::null_mut()
    }
}

unsafe fn resolve_from_index(
    c: *mut OcerzCache,
    ix: *mut CacheIndex,
    root: u32,
    mh: u64,
    symbol: *const c_char,
    found: *mut c_int,
    skip_upward: c_int,
) -> u64 {
    unsafe {
        let key = mh | (skip_upward != 0) as u64;
        let lock = ptr::addr_of_mut!(G_FMEMO_LOCK);
        libc::pthread_mutex_lock(lock);
        let m = fmemo_find(key, symbol);
        if !m.is_null() && !(*m).name.is_null() {
            found.write((*m).found);
            let v = (*m).val;
            libc::pthread_mutex_unlock(lock);
            return v;
        }
        libc::pthread_mutex_unlock(lock);

        let mut order = [0u32; 1024];
        let mut seen_small = [0u8; 1024];
        let mut head = 0u32;
        let mut tail = 0u32;
        let nbytes = (*ix).n.wrapping_add(7) / 8;
        let seen = if nbytes as usize <= seen_small.len() {
            seen_small.as_mut_ptr()
        } else {
            libc::calloc(nbytes as usize, 1).cast::<u8>()
        };
        let mut v = 0u64;
        let mut f = 0;
        if !seen.is_null() {
            if seen == seen_small.as_mut_ptr() {
                libc::memset(seen.cast(), 0, nbytes as usize);
            }
            let orderp = order.as_mut_ptr();
            orderp.add(tail as usize).write(root);
            tail = tail.wrapping_add(1);
            let slot = seen.add((root >> 3) as usize);
            slot.write(slot.read() | (1u8 << (root & 7)));
            while head < tail {
                let cur = orderp.add(head as usize).read();
                head = head.wrapping_add(1);
                v = resolve_in_dylib(
                    c,
                    super::map::ocerz_cache_image_addr(c, cur, ptr::null_mut()),
                    symbol,
                    0,
                    &mut f,
                );
                if f != 0 {
                    break;
                }
                v = 0;
                let inf = cix_info(ix, cur);
                if inf.is_null() {
                    continue;
                }
                let mut k = 0u32;
                while k < (*inf).ndeps {
                    let d = *(*inf).dep.add(k as usize);
                    if d == CIX_NONE {
                        k = k.wrapping_add(1);
                        continue;
                    }
                    let seen_byte = seen.add((d >> 3) as usize);
                    if seen_byte.read() & (1u8 << (d & 7)) != 0
                        || (skip_upward != 0 && *(*inf).dep_kind.add(k as usize) == DEP_UPWARD)
                    {
                        k = k.wrapping_add(1);
                        continue;
                    }
                    if tail >= order.len() as u32 {
                        break;
                    }
                    seen_byte.write(seen_byte.read() | (1u8 << (d & 7)));
                    orderp.add(tail as usize).write(d);
                    tail = tail.wrapping_add(1);
                    k = k.wrapping_add(1);
                }
            }
            if seen != seen_small.as_mut_ptr() {
                libc::free(seen.cast::<c_void>());
            }
        }
        libc::pthread_mutex_lock(lock);
        let m = fmemo_find(key, symbol);
        if !m.is_null() && (*m).name.is_null() {
            let name = libc::strdup(symbol);
            (*m).name = name;
            if !name.is_null() {
                (*m).mh = key;
                (*m).val = v;
                (*m).found = f;
            }
        }
        libc::pthread_mutex_unlock(lock);
        found.write(f);
        v
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_dlsym_image(
    c: *mut OcerzCache,
    mh: u64,
    symbol: *const c_char,
    found: *mut c_int,
) -> u64 {
    unsafe {
        let mut dummy = 0;
        let found = if found.is_null() {
            &mut dummy as *mut c_int
        } else {
            found
        };
        found.write(0);
        if (*c).mapped == 0 || mh == 0 || symbol.is_null() {
            return 0;
        }
        let ix = cix_get(c);
        let root = if !ix.is_null() {
            cix_by_mh(ix, mh)
        } else {
            CIX_NONE
        };
        if root == CIX_NONE {
            return ocerz_cache_resolve_from_image(c, mh, symbol, found);
        }
        let skip_upward = libc::getenv(c"OCERZ_DLSYM_UPWARD".as_ptr()).is_null() as c_int;
        resolve_from_index(c, ix, root, mh, symbol, found, skip_upward)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_resolve_from_image(
    c: *mut OcerzCache,
    mh: u64,
    symbol: *const c_char,
    found: *mut c_int,
) -> u64 {
    unsafe {
        let mut dummy = 0;
        let found = if found.is_null() {
            &mut dummy as *mut c_int
        } else {
            found
        };
        found.write(0);
        if (*c).mapped == 0 || mh == 0 || symbol.is_null() {
            return 0;
        }
        let ix = cix_get(c);
        let root = if !ix.is_null() {
            cix_by_mh(ix, mh)
        } else {
            CIX_NONE
        };
        if root != CIX_NONE {
            return resolve_from_index(c, ix, root, mh, symbol, found, 0);
        }
        let mut order = [0u64; 1024];
        let orderp = order.as_mut_ptr();
        let mut head = 0u32;
        let mut tail = 1u32;
        orderp.write(mh);
        while head < tail {
            let cur = orderp.add(head as usize).read();
            head = head.wrapping_add(1);
            let mut f = 0;
            let v = resolve_in_dylib(c, cur, symbol, 0, &mut f);
            if f != 0 {
                found.write(1);
                return v;
            }
            let m = cur as usize as *const u8;
            let ncmds = rd32(m.add(16));
            let mut lc = m.add(32);
            let mut i = 0u32;
            while i < ncmds {
                let cmd = rd32(lc);
                if super::is_dylib_cmd(cmd) {
                    let dep = cache_image_by_path_memo(c, lc.add(rd32(lc.add(8)) as usize).cast());
                    let mut k = 0u32;
                    while k < tail && orderp.add(k as usize).read() != dep {
                        k = k.wrapping_add(1);
                    }
                    if dep != 0 && k == tail && tail < order.len() as u32 {
                        orderp.add(tail as usize).write(dep);
                        tail = tail.wrapping_add(1);
                    }
                }
                lc = lc.add(rd32(lc.add(4)) as usize);
                i = i.wrapping_add(1);
            }
        }
        0
    }
}

//! Weak-export enumeration and the lazily-built name bloom filter.

use core::ffi::{c_char, c_int, c_void};
use core::ptr;
use core::sync::atomic::{AtomicPtr, Ordering};

use crate::ffi::OcerzCache;

use super::{rd32, uleb};

const FNV64_BASIS: u64 = 14_695_981_039_346_656_037;
const FNV64_PRIME: u64 = 1_099_511_628_211;
const NAMEBLOOM_BITS: u32 = 1 << 23;
const WEAK_FILTER_AFTER: u32 = 128;

static mut WEAK: *mut u64 = ptr::null_mut();
static mut NWEAK: u32 = 0;
static BLOOM: AtomicPtr<u8> = AtomicPtr::new(ptr::null_mut());
static mut BUILT: c_int = 0;
static mut BLOOM_BUILT: c_int = 0;
static mut LOOKUPS: u32 = 0;
static mut LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

unsafe fn fnv64_extend(mut h: u64, s: *const c_char, n: usize) -> u64 {
    unsafe {
        let mut i = 0usize;
        while i < n {
            h = (h ^ (*s.add(i) as u8 as u64)).wrapping_mul(FNV64_PRIME);
            i += 1;
        }
        h
    }
}

unsafe fn bloom_probe_bits(mut h: u64, bits: *mut u32) {
    h ^= h >> 29;
    h = h.wrapping_mul(0xbf58_476d_1ce4_e5b9);
    h ^= h >> 32;
    let a = h as u32;
    let b = ((h >> 32) as u32) | 1;
    let mut k = 0u32;
    while k < 4 {
        bits.add(k as usize)
            .write(a.wrapping_add(k.wrapping_mul(b)) & (NAMEBLOOM_BITS - 1));
        k = k.wrapping_add(1);
    }
}

unsafe fn bloom_add(bloom: *mut u8, h: u64) {
    unsafe {
        let mut bits = [0u32; 4];
        bloom_probe_bits(h, bits.as_mut_ptr());
        let mut k = 0usize;
        while k < 4 {
            let bit = *bits.as_ptr().add(k);
            let byte = bloom.add((bit >> 3) as usize);
            byte.write(byte.read() | (1u8 << (bit & 7)));
            k += 1;
        }
    }
}

unsafe fn bloom_has(bloom: *const u8, h: u64) -> c_int {
    unsafe {
        let mut bits = [0u32; 4];
        bloom_probe_bits(h, bits.as_mut_ptr());
        let mut k = 0usize;
        while k < 4 {
            let bit = *bits.as_ptr().add(k);
            if *bloom.add((bit >> 3) as usize) & (1u8 << (bit & 7)) == 0 {
                return 0;
            }
            k += 1;
        }
        1
    }
}

unsafe fn trie_collect(
    start: *const u8,
    end: *const u8,
    node: *const u8,
    h: u64,
    depth: c_int,
    bloom: *mut u8,
    count: *mut u64,
) -> c_int {
    unsafe {
        if depth > 256 || node < start || node >= end {
            return -1;
        }
        let mut p = node;
        let term = uleb(&mut p, end);
        if term != 0 {
            bloom_add(bloom, h);
            count.write(count.read().wrapping_add(1));
        }
        if term > end.offset_from(p) as u64 {
            return -1;
        }
        p = p.add(term as usize);
        if p >= end {
            return 0;
        }
        let children = *p;
        p = p.add(1);
        let mut i = 0u32;
        while i < children as u32 {
            let edge = p.cast::<c_char>();
            let elen = libc::strnlen(edge, end.offset_from(p) as usize);
            if p.add(elen) >= end {
                return -1;
            }
            p = p.add(elen + 1);
            let child_off = uleb(&mut p, end);
            if trie_collect(
                start,
                end,
                start.add(child_off as usize),
                fnv64_extend(h, edge, elen),
                depth.wrapping_add(1),
                bloom,
                count,
            ) != 0
            {
                return -1;
            }
            i = i.wrapping_add(1);
        }
        0
    }
}

unsafe fn collect_image_exports(
    c: *mut OcerzCache,
    mh: u64,
    bloom: *mut u8,
    count: *mut u64,
    seen: *mut u64,
    nseen: *mut u32,
    cap: u32,
    depth: c_int,
) -> c_int {
    unsafe {
        if depth > 16 {
            return -1;
        }
        let mut i = 0u32;
        while i < nseen.read() {
            if *seen.add(i as usize) == mh {
                return 0;
            }
            i = i.wrapping_add(1);
        }
        if nseen.read() >= cap {
            return -1;
        }
        seen.add(nseen.read() as usize).write(mh);
        nseen.write(nseen.read().wrapping_add(1));
        let mut ts = ptr::null();
        let mut te = ptr::null();
        if super::resolve::dylib_export_region(mh, &mut ts, &mut te) == 0
            && trie_collect(ts, te, ts, FNV64_BASIS, 0, bloom, count) != 0
        {
            return -1;
        }
        let h = mh as usize as *const u8;
        let ncmds = rd32(h.add(16));
        let mut lc = h.add(32);
        let mut k = 0u32;
        while k < ncmds {
            let cmd = rd32(lc);
            let size = rd32(lc.add(4));
            if size < 8 {
                break;
            }
            if cmd == super::LC_REEXPORT_DYLIB && rd32(lc.add(8)) < size {
                let path = lc.add(rd32(lc.add(8)) as usize).cast::<c_char>();
                let tmh = super::index::cache_image_by_path_memo(c, path);
                if tmh == 0
                    || collect_image_exports(
                        c,
                        tmh,
                        bloom,
                        count,
                        seen,
                        nseen,
                        cap,
                        depth.wrapping_add(1),
                    ) != 0
                {
                    return -1;
                }
            }
            lc = lc.add(size as usize);
            k = k.wrapping_add(1);
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_cache_resolve_weak_ex(
    c: *mut OcerzCache,
    symbol: *const c_char,
    found: *mut c_int,
    loaded: Option<unsafe extern "C" fn(u64) -> c_int>,
) -> u64 {
    unsafe {
        let mut dummy = 0;
        let found = if found.is_null() {
            &mut dummy as *mut c_int
        } else {
            found
        };
        found.write(0);
        if (*c).mapped == 0 || symbol.is_null() {
            return 0;
        }
        let lock = ptr::addr_of_mut!(LOCK);
        libc::pthread_mutex_lock(lock);
        let cap = if (*c).images_cnt != 0 {
            (*c).images_cnt
        } else {
            1
        };
        if ptr::addr_of!(BUILT).read() == 0 {
            let weak = libc::calloc(cap as usize, core::mem::size_of::<u64>()).cast::<u64>();
            ptr::addr_of_mut!(WEAK).write(weak);
            let mut i = 0u32;
            while !weak.is_null() && i < (*c).images_cnt {
                let mh = super::map::ocerz_cache_image_addr(c, i, ptr::null_mut());
                let h = mh as usize as *const u8;
                if mh != 0
                    && rd32(h) == super::MH_MAGIC_64
                    && rd32(h.add(24)) & super::MH_WEAK_DEFINES != 0
                {
                    let nweak = ptr::addr_of!(NWEAK).read();
                    weak.add(nweak as usize).write(mh);
                    ptr::addr_of_mut!(NWEAK).write(nweak.wrapping_add(1));
                }
                i = i.wrapping_add(1);
            }
            ptr::addr_of_mut!(BUILT).write(1);
        }
        if ptr::addr_of!(BLOOM_BUILT).read() == 0 {
            let lookups = ptr::addr_of_mut!(LOOKUPS);
            lookups.write(lookups.read().wrapping_add(1));
            if lookups.read() > WEAK_FILTER_AFTER {
                let seen = libc::calloc(cap as usize, core::mem::size_of::<u64>()).cast::<u64>();
                let mut nseen = 0u32;
                let mut count = 0u64;
                let mut fresh = if !seen.is_null() {
                    libc::calloc((NAMEBLOOM_BITS / 8) as usize, 1).cast::<u8>()
                } else {
                    ptr::null_mut()
                };
                let weak = ptr::addr_of!(WEAK).read();
                let nweak = ptr::addr_of!(NWEAK).read();
                let mut i = 0u32;
                while !fresh.is_null() && i < nweak {
                    if collect_image_exports(
                        c,
                        *weak.add(i as usize),
                        fresh,
                        &mut count,
                        seen,
                        &mut nseen,
                        cap,
                        0,
                    ) != 0
                    {
                        libc::free(fresh.cast::<c_void>());
                        fresh = ptr::null_mut();
                    }
                    i = i.wrapping_add(1);
                }
                libc::free(seen.cast::<c_void>());
                BLOOM.store(fresh, Ordering::Release);
                ptr::addr_of_mut!(BLOOM_BUILT).write(1);
            }
        }
        libc::pthread_mutex_unlock(lock);
        let bloom = BLOOM.load(Ordering::Acquire);
        if !bloom.is_null()
            && bloom_has(
                bloom,
                fnv64_extend(FNV64_BASIS, symbol, libc::strlen(symbol)),
            ) == 0
        {
            return 0;
        }
        let weak = ptr::addr_of!(WEAK).read();
        let nweak = ptr::addr_of!(NWEAK).read();
        let mut i = 0u32;
        while i < nweak {
            let mh = *weak.add(i as usize);
            if let Some(loaded) = loaded {
                if loaded(mh) == 0 {
                    i = i.wrapping_add(1);
                    continue;
                }
            }
            let mut f = 0;
            let v = super::resolve::resolve_in_dylib(c, mh, symbol, 0, &mut f);
            if f != 0 {
                found.write(1);
                return v;
            }
            i = i.wrapping_add(1);
        }
        0
    }
}

//! Function-start indexing and special handling for libsystem_platform routines.

use core::ffi::{CStr, c_int, c_void};
use core::mem;
use core::ptr;
use core::sync::atomic::{AtomicI32, Ordering};

use crate::ffi::{
    ocerz_cache_resolve_from_image, ocerz_leaf_memchr, ocerz_leaf_memcmp, ocerz_leaf_memmove,
    ocerz_leaf_memset, ocerz_leaf_strchr, ocerz_leaf_strcmp, ocerz_leaf_strlen, ocerz_leaf_strncmp,
    ocerz_leaf_strnlen,
};

use super::closure::{cache_find_path, image_slide};
use super::g_cache;
use super::hostmem::{ocerz_g2h, ocerz_ld};
use super::macho::*;

const MEMFN_MAX: usize = 128;
const MEMFN_BYTES_MAX: u64 = 0x4000;
const LEAF_ENTRY_MAX: usize = 16;

#[derive(Clone, Copy)]
struct MemFn {
    lo: u64,
    hi: u64,
}

#[derive(Clone, Copy)]
struct LeafName {
    name: &'static CStr,
    routine: unsafe extern "C" fn(),
    writes: c_int,
}

#[derive(Clone, Copy)]
struct LeafEntry {
    entry: u64,
    routine: *const c_void,
    writes: c_int,
}

static mut g_memfn: [MemFn; MEMFN_MAX] = [MemFn { lo: 0, hi: 0 }; MEMFN_MAX];
static g_memfn_n: AtomicI32 = AtomicI32::new(0);
static mut g_memfn_slot: [u64; MEMFN_MAX] = [0; MEMFN_MAX];
static mut g_memfn_slots: c_int = 0;
static mut g_memfn_starts: *mut u64 = ptr::null_mut();
static mut g_memfn_nstarts: usize = 0;
static mut g_memfn_text_lo: u64 = 0;
static mut g_memfn_text_hi: u64 = 0;
static g_memfn_built: AtomicI32 = AtomicI32::new(0);
static mut g_memfn_lock: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;
static mut g_leaf_entry: [LeafEntry; LEAF_ENTRY_MAX] = [LeafEntry {
    entry: 0,
    routine: ptr::null(),
    writes: 0,
}; LEAF_ENTRY_MAX];
static mut g_leaf_entries: c_int = 0;

const LEAF_NAMES: [LeafName; 10] = [
    LeafName {
        name: c"_strlen",
        routine: ocerz_leaf_strlen,
        writes: 0,
    },
    LeafName {
        name: c"_strnlen",
        routine: ocerz_leaf_strnlen,
        writes: 0,
    },
    LeafName {
        name: c"_strcmp",
        routine: ocerz_leaf_strcmp,
        writes: 0,
    },
    LeafName {
        name: c"_strncmp",
        routine: ocerz_leaf_strncmp,
        writes: 0,
    },
    LeafName {
        name: c"_memcmp",
        routine: ocerz_leaf_memcmp,
        writes: 0,
    },
    LeafName {
        name: c"_strchr",
        routine: ocerz_leaf_strchr,
        writes: 0,
    },
    LeafName {
        name: c"_memchr",
        routine: ocerz_leaf_memchr,
        writes: 0,
    },
    LeafName {
        name: c"_memcpy",
        routine: ocerz_leaf_memmove,
        writes: 1,
    },
    LeafName {
        name: c"_memmove",
        routine: ocerz_leaf_memmove,
        writes: 1,
    },
    LeafName {
        name: c"_memset",
        routine: ocerz_leaf_memset,
        writes: 1,
    },
];

const MEMFN_NAMES: [&CStr; 21] = [
    c"_memmove",
    c"_memcpy",
    c"_memset",
    c"_bzero",
    c"___bzero",
    c"_memset_pattern4",
    c"_memset_pattern8",
    c"_memset_pattern16",
    c"_memccpy",
    c"_memchr",
    c"_memcmp",
    c"_strchr",
    c"_strcmp",
    c"_strncmp",
    c"_strcpy",
    c"_strlcpy",
    c"_strlcat",
    c"_strlen",
    c"_strncpy",
    c"_strnlen",
    c"_strstr",
];

unsafe fn memfn_function(addr: u64, lo_out: *mut u64, hi_out: *mut u64) -> bool {
    unsafe {
        let n = g_memfn_nstarts;
        let starts = g_memfn_starts;
        if n == 0 || addr < *starts || addr >= g_memfn_text_hi {
            return false;
        }
        let mut lo = 0usize;
        let mut hi = n;
        while hi - lo > 1 {
            let mid = lo + (hi - lo) / 2;
            if *starts.add(mid) <= addr {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        let start = *starts.add(lo);
        let end = if lo + 1 < n {
            *starts.add(lo + 1)
        } else {
            g_memfn_text_hi
        };
        if end.wrapping_sub(start) > MEMFN_BYTES_MAX {
            return false;
        }
        *lo_out = start;
        *hi_out = end;
        true
    }
}

unsafe fn memfn_confirm_locked(lo: u64, hi: u64) {
    unsafe {
        let n = g_memfn_n.load(Ordering::SeqCst);
        for k in 0..n {
            if (*ptr::addr_of!(g_memfn).cast::<MemFn>().add(k as usize)).lo == lo {
                return;
            }
        }
        if n < MEMFN_MAX as i32 {
            let e = ptr::addr_of_mut!(g_memfn).cast::<MemFn>().add(n as usize);
            (*e).lo = lo;
            (*e).hi = hi;
            g_memfn_n.store(n + 1, Ordering::SeqCst);
        }
    }
}

unsafe fn memfn_build() {
    unsafe {
        let mh = cache_find_path(
            g_cache,
            c"/usr/lib/system/libsystem_platform.dylib".as_ptr(),
        );
        let h = if mh != 0 {
            ocerz_g2h(mh).cast::<MachHeader64>()
        } else {
            ptr::null_mut()
        };
        if h.is_null() || (*h).magic != MH_MAGIC_64 {
            return;
        }
        let slide = image_slide(mh);
        let mut le_addr = 0u64;
        let mut le_fileoff = 0u64;
        let mut fs_off = 0u64;
        let mut fs_size = 0u64;
        let mut lc = h.add(1).cast::<u8>();
        for _ in 0..(*h).ncmds {
            let l = lc.cast::<LoadCommand>();
            if (*l).cmd == LC_SEGMENT_64 {
                let sg = lc.cast::<SegmentCommand64>();
                if libc::strcmp((*sg).segname.as_ptr(), c"__TEXT".as_ptr()) == 0 {
                    g_memfn_text_lo = (*sg).vmaddr.wrapping_add(slide);
                    g_memfn_text_hi = g_memfn_text_lo.wrapping_add((*sg).vmsize);
                } else if libc::strcmp((*sg).segname.as_ptr(), c"__LINKEDIT".as_ptr()) == 0 {
                    le_addr = (*sg).vmaddr.wrapping_add(slide);
                    le_fileoff = (*sg).fileoff;
                }
            } else if (*l).cmd == LC_FUNCTION_STARTS {
                let d = lc.cast::<LinkeditDataCommand>();
                fs_off = (*d).dataoff as u64;
                fs_size = (*d).datasize as u64;
            }
            lc = lc.add((*l).cmdsize as usize);
        }
        if g_memfn_text_hi == 0 || le_addr == 0 || fs_size == 0 || fs_off < le_fileoff {
            g_memfn_text_hi = 0;
            return;
        }
        let mut p = ocerz_g2h(le_addr.wrapping_add(fs_off - le_fileoff)).cast::<u8>();
        let end = p.add(fs_size as usize);
        let mut cap = 4096usize;
        let mut n = 0usize;
        let mut starts = libc::malloc(cap * mem::size_of::<u64>()).cast::<u64>();
        let mut at = g_memfn_text_lo;
        while !starts.is_null() && p < end && *p != 0 {
            let mut delta = 0u64;
            let mut shift = 0u32;
            loop {
                if p >= end {
                    break;
                }
                let byte = *p;
                p = p.add(1);
                delta |= ((byte & 0x7f) as u64).wrapping_shl(shift);
                shift += 7;
                if byte & 0x80 == 0 || shift > 63 {
                    break;
                }
            }
            at = at.wrapping_add(delta);
            if n == cap {
                let grown = libc::realloc(
                    starts.cast(),
                    cap.wrapping_mul(2).wrapping_mul(mem::size_of::<u64>()),
                )
                .cast::<u64>();
                if grown.is_null() {
                    break;
                }
                starts = grown;
                cap *= 2;
            }
            *starts.add(n) = at;
            n += 1;
        }
        g_memfn_starts = starts;
        g_memfn_nstarts = if starts.is_null() { 0 } else { n };
        let sys = cache_find_path(g_cache, c"/usr/lib/libSystem.B.dylib".as_ptr());
        for k in 0..MEMFN_NAMES.len() {
            if g_memfn_nstarts == 0 {
                break;
            }
            let mut found = 0;
            let a = ocerz_cache_resolve_from_image(
                g_cache,
                if sys != 0 { sys } else { mh },
                MEMFN_NAMES[k].as_ptr(),
                &mut found,
            );
            if found == 0 || a < g_memfn_text_lo || a.wrapping_add(6) > g_memfn_text_hi {
                continue;
            }
            for leaf in LEAF_NAMES.iter() {
                if libc::strcmp(leaf.name.as_ptr(), MEMFN_NAMES[k].as_ptr()) == 0
                    && g_leaf_entries < LEAF_ENTRY_MAX as i32
                {
                    let e = ptr::addr_of_mut!(g_leaf_entry)
                        .cast::<LeafEntry>()
                        .add(g_leaf_entries as usize);
                    (*e).entry = a;
                    (*e).routine = leaf.routine as *const () as *const c_void;
                    (*e).writes = leaf.writes;
                    g_leaf_entries += 1;
                }
            }
            let code = ocerz_g2h(a).cast::<u8>();
            if *code == 0xff && *code.add(1) == 0x25 {
                let rel = ptr::read_unaligned(code.add(2).cast::<i32>());
                if g_memfn_slots < MEMFN_MAX as i32 {
                    *ptr::addr_of_mut!(g_memfn_slot)
                        .cast::<u64>()
                        .add(g_memfn_slots as usize) =
                        a.wrapping_add(6).wrapping_add(rel as i64 as u64);
                    g_memfn_slots += 1;
                }
            } else {
                let mut lo = 0;
                let mut hi = 0;
                if memfn_function(a, &mut lo, &mut hi) {
                    memfn_confirm_locked(lo, hi);
                }
            }
        }
        crate::ocerz_log!(
            "dyldapi: libsystem_platform has %d string and memory routines of its own and %d chosen through a pointer; both kinds are translated with plain accesses\n",
            g_memfn_n.load(Ordering::SeqCst),
            g_memfn_slots
        );
    }
}

unsafe fn memfn_ensure() {
    unsafe {
        if g_memfn_built.load(Ordering::SeqCst) != 0 {
            return;
        }
        libc::pthread_mutex_lock(ptr::addr_of_mut!(g_memfn_lock));
        if g_memfn_built.load(Ordering::SeqCst) == 0 {
            memfn_build();
            g_memfn_built.store(1, Ordering::SeqCst);
        }
        libc::pthread_mutex_unlock(ptr::addr_of_mut!(g_memfn_lock));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyldapi_leaf_entry(rip: u64, writes: *mut c_int) -> *const c_void {
    unsafe {
        if g_cache.is_null() || writes.is_null() {
            return ptr::null();
        }
        memfn_ensure();
        if rip < g_memfn_text_lo || rip >= g_memfn_text_hi {
            return ptr::null();
        }
        for k in 0..g_leaf_entries {
            let entry = *ptr::addr_of!(g_leaf_entry)
                .cast::<LeafEntry>()
                .add(k as usize);
            if entry.entry == rip {
                *writes = entry.writes;
                return entry.routine;
            }
        }
        ptr::null()
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyldapi_memfn(rip: u64) -> c_int {
    unsafe {
        static mut off: c_int = -1;
        if off < 0 {
            off = (!libc::getenv(c"OCERZ_NO_MEMFN_PLAIN".as_ptr()).is_null()) as c_int;
        }
        if off != 0 || g_cache.is_null() {
            return 0;
        }
        memfn_ensure();
        if rip < g_memfn_text_lo || rip >= g_memfn_text_hi {
            return 0;
        }
        let n = g_memfn_n.load(Ordering::SeqCst);
        for k in 0..n {
            let range = *ptr::addr_of!(g_memfn).cast::<MemFn>().add(k as usize);
            if rip >= range.lo && rip < range.hi {
                return 1;
            }
        }
        let mut lo = 0;
        let mut hi = 0;
        if !memfn_function(rip, &mut lo, &mut hi) {
            return 0;
        }
        for k in 0..g_memfn_slots {
            let target = ocerz_ld(
                *ptr::addr_of!(g_memfn_slot).cast::<u64>().add(k as usize),
                8,
            );
            if target >= lo && target < hi {
                libc::pthread_mutex_lock(ptr::addr_of_mut!(g_memfn_lock));
                memfn_confirm_locked(lo, hi);
                libc::pthread_mutex_unlock(ptr::addr_of_mut!(g_memfn_lock));
                return 1;
            }
        }
        0
    }
}

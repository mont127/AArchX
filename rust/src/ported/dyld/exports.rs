//! Export-trie traversal, symbol indexing, and image export resolution.

use super::*;
use crate::ported::dyldapi::macho::{LC_SYMTAB, N_SECT, N_TYPE};

const LC_DYSYMTAB: u32 = 0xb;
const N_EXT: u8 = 0x01;

#[inline(always)]
pub(super) unsafe fn self_uleb(pp: *mut *const u8, end: *const u8) -> u64 {
    let mut result = 0u64;
    let mut shift = 0u32;
    while *pp < end {
        let p = *pp;
        let byte = p.read();
        *pp = p.add(1);
        result |= ((byte & 0x7f) as u64).wrapping_shl(shift);
        if byte & 0x80 == 0 {
            break;
        }
        shift = shift.wrapping_add(7);
    }
    result
}

#[inline(always)]
pub(super) unsafe fn self_sleb(pp: *mut *const u8, end: *const u8) -> i64 {
    let mut result = 0i64;
    let mut shift = 0u32;
    let mut byte = 0u8;
    while *pp < end {
        let p = *pp;
        byte = p.read();
        *pp = p.add(1);
        result |= ((byte & 0x7f) as i64).wrapping_shl(shift);
        shift = shift.wrapping_add(7);
        if byte & 0x80 == 0 {
            break;
        }
    }
    if shift < 64 && byte & 0x40 != 0 {
        result |= (!0i64).wrapping_shl(shift);
    }
    result
}

pub(super) unsafe fn image_export_trie(slice: *const u8, size_out: *mut u32) -> u64 {
    let ncmds = rd32(slice.add(16));
    let mut lc = slice.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        let cmd = rd32(lc);
        if cmd == LC_DYLD_EXPORTS_TRIE {
            *size_out = rd32(lc.add(12));
            return rd32(lc.add(8)) as u64;
        }
        if cmd == LC_DYLD_INFO || cmd == LC_DYLD_INFO_ONLY {
            *size_out = rd32(lc.add(0x2c));
            return rd32(lc.add(0x28)) as u64;
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
    *size_out = 0;
    0
}

unsafe fn symidx_hash(s: *const c_char) -> u32 {
    let mut hash = 2_166_136_261u32;
    let mut p = s;
    while p.read() != 0 {
        hash = (hash ^ p.read() as u8 as u32).wrapping_mul(16_777_619);
        p = p.add(1);
    }
    hash
}

unsafe fn symidx_build(slice: *const u8, text_vmaddr: u64) -> *mut SymIndex {
    let ncmds = rd32(slice.add(16));
    let mut lc = slice.add(core::mem::size_of::<MachHeader64>());
    let mut symoff = 0u32;
    let mut nsyms = 0u32;
    let mut stroff = 0u32;
    let mut strsize = 0u32;
    let mut iextdef = 0u32;
    let mut nextdef = 0u32;
    let mut have_dysym = 0;
    for _ in 0..ncmds {
        let cmd = rd32(lc);
        if cmd == LC_SYMTAB {
            symoff = rd32(lc.add(8));
            nsyms = rd32(lc.add(12));
            stroff = rd32(lc.add(16));
            strsize = rd32(lc.add(20));
        } else if cmd == LC_DYSYMTAB {
            iextdef = rd32(lc.add(8 + 3 * 4));
            nextdef = rd32(lc.add(8 + 4 * 4));
            have_dysym = 1;
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
    if symoff == 0 || nsyms == 0 || stroff == 0 || strsize == 0 {
        return ptr::null_mut();
    }
    let mut first = 0u32;
    let mut count = nsyms;
    if have_dysym != 0 && nextdef != 0 && iextdef.wrapping_add(nextdef) <= nsyms {
        first = iextdef;
        count = nextdef;
    }
    let ix = libc::calloc(1, core::mem::size_of::<SymIndex>()).cast::<SymIndex>();
    if ix.is_null() {
        return ptr::null_mut();
    }
    let mut cap = 64u32;
    while cap < count.wrapping_mul(2) {
        cap = cap.wrapping_shl(1);
    }
    (*ix).ent =
        libc::calloc(cap as usize, core::mem::size_of::<SymIndexEntry>()).cast::<SymIndexEntry>();
    if (*ix).ent.is_null() {
        libc::free(ix.cast());
        return ptr::null_mut();
    }
    (*ix).cap = cap;
    (*ix).strtab = slice.add(stroff as usize).cast::<c_char>();
    for k in 0..count {
        let nl = slice.add(symoff as usize + first.wrapping_add(k) as usize * 16);
        let strx = rd32(nl);
        let ntype = nl.add(4).read();
        if strx == 0 || strx >= strsize {
            continue;
        }
        if ntype & N_EXT == 0 || ntype & N_TYPE != N_SECT {
            continue;
        }
        let value = rd64(nl.add(8));
        if value == 0 {
            continue;
        }
        let name = (*ix).strtab.add(strx as usize);
        let mut h = symidx_hash(name) & (cap - 1);
        let entries = (*ix).ent;
        while (*entries.add(h as usize)).stroff != 0 {
            h = (h + 1) & (cap - 1);
        }
        (*entries.add(h as usize)).stroff = strx;
        (*entries.add(h as usize)).value = value.wrapping_sub(text_vmaddr);
        (*ix).n += 1;
    }
    ix
}

unsafe fn symtab_export_resolve(img: *mut DynImage, sym: *const c_char, found: *mut c_int) -> u64 {
    if (*img).symidx.is_null() {
        let mut tsize = 0u32;
        if image_export_trie((*img).slice, &mut tsize) != 0 && tsize != 0 {
            return 0;
        }
        let text = if (*img).seg_count > 0 {
            (*img).seg_vmaddr[0]
        } else {
            0
        };
        (*img).symidx = symidx_build((*img).slice, text);
        if (*img).symidx.is_null() {
            return 0;
        }
        let name = if (*img).install_name[0] != 0 {
            ptr::addr_of!((*img).install_name).cast::<c_char>()
        } else {
            ptr::addr_of!((*img).path).cast::<c_char>()
        };
        crate::ocerz_log!(
            "dynamic: %s has no export trie; indexed %u exports from its symbol table\n",
            name,
            (*(*img).symidx).n
        );
    }
    let ix = (*img).symidx;
    if (*ix).cap == 0 {
        return 0;
    }
    let mut h = symidx_hash(sym) & ((*ix).cap - 1);
    let entries = (*ix).ent;
    while (*entries.add(h as usize)).stroff != 0 {
        if libc::strcmp(
            (*ix).strtab.add((*entries.add(h as usize)).stroff as usize),
            sym,
        ) == 0
        {
            if !found.is_null() {
                *found = 1;
            }
            return (*img)
                .load_base
                .wrapping_add((*entries.add(h as usize)).value);
        }
        h = (h + 1) & ((*ix).cap - 1);
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_trie_resolve(
    slice: *const u8,
    load_base: u64,
    sym: *const c_char,
    found: *mut c_int,
) -> u64 {
    let mut dummy = 0;
    let found = if found.is_null() {
        &mut dummy
    } else {
        &mut *found
    };
    *found = 0;
    let mut tsize = 0u32;
    let toff = image_export_trie(slice, &mut tsize);
    if toff == 0 || tsize == 0 {
        return 0;
    }
    let start = slice.add(toff as usize);
    let end = start.add(tsize as usize);
    let mut p = start;
    let mut s = sym;
    while p < end {
        let mut pp = p;
        let term = self_uleb(&mut pp, end);
        p = pp;
        if s.read() == 0 && term != 0 {
            let mut tp = p;
            let flags = self_uleb(&mut tp, end);
            if flags & 0x08 != 0 {
                return 0;
            }
            *found = 1;
            if flags & 0x03 == 0x02 {
                return self_uleb(&mut tp, end);
            }
            return load_base.wrapping_add(self_uleb(&mut tp, end));
        }
        p = p.add(term as usize);
        if p >= end {
            return 0;
        }
        let children = p.read();
        p = p.add(1);
        let mut next: *const u8 = ptr::null();
        for _ in 0..children {
            let edge = p.cast::<c_char>();
            let elen = libc::strlen(edge);
            p = p.add(elen + 1);
            let mut pp = p;
            let child_off = self_uleb(&mut pp, end);
            p = pp;
            if next.is_null() && libc::strncmp(s, edge, elen) == 0 {
                s = s.add(elen);
                next = start.add(child_off as usize);
            }
        }
        if next.is_null() {
            return 0;
        }
        p = next;
    }
    0
}

#[repr(C)]
struct TrieWalk {
    start: *const u8,
    end: *const u8,
    load_base: u64,
    visit: OcerzTrieVisit,
    ctx: *mut c_void,
    count: c_int,
    stopped: c_int,
    name: [c_char; 4096],
}

unsafe fn trie_walk(w: *mut TrieWalk, off: u64, len: usize, depth: c_int) -> c_int {
    let span = (*w).end.offset_from((*w).start) as u64;
    if depth > 512 || off >= span {
        return -1;
    }
    let mut p = (*w).start.add(off as usize);
    let term = self_uleb(&mut p, (*w).end);
    if p >= (*w).end || term >= (*w).end.offset_from(p) as u64 {
        return -1;
    }
    let after = p.add(term as usize);
    if term != 0 {
        let mut tp = p;
        let flags = self_uleb(&mut tp, after);
        let mut value = 0u64;
        if flags & 0x08 == 0 {
            let raw = self_uleb(&mut tp, after);
            value = if flags & 0x03 == 0x02 {
                raw
            } else {
                (*w).load_base.wrapping_add(raw)
            };
        }
        *ptr::addr_of_mut!((*w).name).cast::<c_char>().add(len) = 0;
        (*w).count += 1;
        if let Some(visit) = (*w).visit {
            if visit((*w).ctx, ptr::addr_of!((*w).name).cast(), value, flags) != 0 {
                (*w).stopped = 1;
                return 0;
            }
        }
    }
    p = after;
    let children = p.read();
    p = p.add(1);
    for _ in 0..children {
        if p >= (*w).end {
            return -1;
        }
        let remaining = (*w).end.offset_from(p) as usize;
        let elen = libc::strnlen(p.cast(), remaining);
        if elen == remaining || len.wrapping_add(elen) >= 4096 {
            return -1;
        }
        ptr::copy_nonoverlapping(p, ptr::addr_of_mut!((*w).name).cast::<u8>().add(len), elen);
        p = p.add(elen + 1);
        let child = self_uleb(&mut p, (*w).end);
        if trie_walk(w, child, len.wrapping_add(elen), depth + 1) < 0 {
            return -1;
        }
        if (*w).stopped != 0 {
            return 0;
        }
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_trie_each(
    slice: *const u8,
    load_base: u64,
    visit: OcerzTrieVisit,
    ctx: *mut c_void,
) -> c_int {
    let mut tsize = 0u32;
    let toff = if slice.is_null() {
        0
    } else {
        image_export_trie(slice, &mut tsize)
    };
    if toff == 0 || tsize == 0 {
        return 0;
    }
    let w = libc::calloc(1, core::mem::size_of::<TrieWalk>()).cast::<TrieWalk>();
    if w.is_null() {
        return -1;
    }
    (*w).start = slice.add(toff as usize);
    (*w).end = (*w).start.add(tsize as usize);
    (*w).load_base = load_base;
    (*w).visit = visit;
    (*w).ctx = ctx;
    let rc = trie_walk(w, 0, 0, 0);
    let count = (*w).count;
    libc::free(w.cast());
    if rc < 0 { -1 } else { count }
}

pub(super) unsafe fn ocerz_image_self_resolve_ex(
    img: *mut DynImage,
    sym: *const c_char,
    found: *mut c_int,
) -> u64 {
    if ffi::ocerz_mode == MODE_NATIVE
        && (*img).is_virtual != 0
        && libc::strcmp(
            ptr::addr_of!((*img).install_name).cast(),
            cstr_ptr(c"/usr/lib/libSystem.B.dylib"),
        ) == 0
        && (libc::strncmp(sym, cstr_ptr(c"__Unwind_"), 9) == 0
            || libc::strncmp(sym, cstr_ptr(c"_unw_"), 5) == 0
            || libc::strcmp(sym, cstr_ptr(c"___register_frame")) == 0
            || libc::strcmp(sym, cstr_ptr(c"___deregister_frame")) == 0)
    {
        let unwind = dimg_find_by_install_name(cstr_ptr(c"/usr/lib/libunwind.1.dylib"));
        if !unwind.is_null() && (*unwind).is_virtual == 0 {
            let mut present = 0;
            let addr =
                ocerz_dyld_trie_resolve((*unwind).slice, (*unwind).load_base, sym, &mut present);
            if present != 0 {
                if !found.is_null() {
                    *found = 1;
                }
                return addr;
            }
        }
    }
    let mut f = 0;
    let value = ocerz_dyld_trie_resolve((*img).slice, (*img).load_base, sym, &mut f);
    if f != 0 {
        if !found.is_null() {
            *found = 1;
        }
        return value;
    }
    symtab_export_resolve(img, sym, found)
}

pub(super) unsafe fn ocerz_image_self_resolve(img: *mut DynImage, sym: *const c_char) -> u64 {
    ocerz_image_self_resolve_ex(img, sym, ptr::null_mut())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_guest_export(
    install_name: *const c_char,
    sym: *const c_char,
    found: *mut c_int,
) -> u64 {
    *found = 0;
    let d = dimg_find_by_install_name(install_name);
    if !d.is_null() && (*d).is_virtual == 0 {
        ocerz_image_self_resolve_ex(d, sym, found)
    } else {
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_generation() -> u32 {
    g_dimgs_n as u32
}

pub(super) unsafe fn dimg_ordinal_name(img: *mut DynImage, ord: c_int) -> *const c_char {
    if ord <= 0 {
        return ptr::null();
    }
    let mh = (*img).slice;
    let ncmds = rd32(mh.add(16));
    let mut lc = mh.add(core::mem::size_of::<MachHeader64>());
    let mut n = 0;
    for _ in 0..ncmds {
        let cmd = rd32(lc);
        if cmd == 0xc || cmd == 0x8000_0018 || cmd == 0x8000_001f || cmd == 0x8000_0023 {
            n += 1;
            if n == ord {
                return lc.add(rd32(lc.add(8)) as usize).cast();
            }
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
    ptr::null()
}

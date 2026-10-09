//! Import resolution, chained fixups, classic binding, and legacy relocations.

use super::exports::{dimg_ordinal_name, ocerz_image_self_resolve_ex, self_sleb, self_uleb};
use super::*;

unsafe fn disk_flat_resolve_ex(name: *const c_char, found: *mut c_int) -> u64 {
    for i in 0..g_dimgs_n {
        let img = dimg_at(i as usize);
        if (*img).local != 0 {
            continue;
        }
        let mut f = 0;
        let value = ocerz_image_self_resolve_ex(img, name, &mut f);
        if f != 0 {
            *found = 1;
            return value;
        }
    }
    0
}

pub(super) unsafe fn disk_flat_resolve(name: *const c_char) -> u64 {
    let mut found = 0;
    disk_flat_resolve_ex(name, &mut found)
}

unsafe fn virt_flat_resolve_ex(
    name: *const c_char,
    want: *const c_char,
    found: *mut c_int,
    hit: *mut *const c_char,
) -> u64 {
    for i in 0..g_dimgs_n {
        let img = dimg_at(i as usize);
        if (*img).is_virtual == 0 {
            continue;
        }
        if !want.is_null() && libc::strcmp(ptr::addr_of!((*img).install_name).cast(), want) == 0 {
            continue;
        }
        let mut f = 0;
        let value = ocerz_image_self_resolve_ex(img, name, &mut f);
        if f != 0 {
            *found = 1;
            if !hit.is_null() {
                *hit = ptr::addr_of!((*img).install_name).cast();
            }
            return value;
        }
    }
    0
}

unsafe fn virt_ondemand_resolve_ex(
    cache: *mut OcerzCache,
    img: *mut DynImage,
    name: *const c_char,
    found: *mut c_int,
    hit: *mut *const c_char,
) -> u64 {
    let mut n = 0;
    let names = ffi::ocerz_apidb_install_names(&mut n);
    for i in 0..n {
        let install_name = *names.add(i as usize);
        if !dimg_find_by_install_name(install_name).is_null() {
            continue;
        }
        let lib = ffi::ocerz_apidb_library(install_name);
        if lib.is_null() || ffi::ocerz_apidb_find(lib, name).is_null() {
            continue;
        }
        let dep = super::load::load_disk_dylib(cache, install_name, img, ptr::null());
        if dep.is_null() {
            continue;
        }
        let mut f = 0;
        let value = ocerz_image_self_resolve_ex(dep, name, &mut f);
        if f != 0 {
            *found = 1;
            if !hit.is_null() {
                *hit = ptr::addr_of!((*dep).install_name).cast();
            }
            return value;
        }
    }
    0
}

unsafe fn native_miss_add(lib: *const c_char, sym: *const c_char, from: *const c_char) {
    let lib = if lib.is_null() || lib.read() == 0 {
        cstr_ptr(c"(flat)")
    } else {
        lib
    };
    let n = g_native_miss_n;
    let misses = ptr::addr_of_mut!(g_native_miss).cast::<NativeMiss>();
    for i in 0..n {
        let miss = misses.add(i as usize);
        if libc::strcmp(ptr::addr_of!((*miss).sym).cast(), sym) == 0
            && libc::strcmp(ptr::addr_of!((*miss).lib).cast(), lib) == 0
        {
            return;
        }
    }
    if n >= NATIVE_MISS_MAX as c_int {
        g_native_miss_dropped += 1;
        return;
    }
    let miss = misses.add(n as usize);
    libc::snprintf(
        ptr::addr_of_mut!((*miss).lib).cast(),
        core::mem::size_of_val(&(*miss).lib),
        cstr_ptr(c"%s"),
        lib,
    );
    libc::snprintf(
        ptr::addr_of_mut!((*miss).sym).cast(),
        core::mem::size_of_val(&(*miss).sym),
        cstr_ptr(c"%s"),
        sym,
    );
    libc::snprintf(
        ptr::addr_of_mut!((*miss).from).cast(),
        core::mem::size_of_val(&(*miss).from),
        cstr_ptr(c"%s"),
        if from.is_null() { cstr_ptr(c"") } else { from },
    );
    g_native_miss_n = n + 1;
}

unsafe fn native_guest_runtime_for(
    cache: *mut OcerzCache,
    img: *mut DynImage,
    dep: *const DynImage,
    name: *const c_char,
) {
    let objc_eh: [*const c_char; 6] = [
        cstr_ptr(c"_objc_exception_throw"),
        cstr_ptr(c"_objc_exception_rethrow"),
        cstr_ptr(c"_objc_begin_catch"),
        cstr_ptr(c"_objc_end_catch"),
        cstr_ptr(c"_objc_terminate"),
        cstr_ptr(c"___objc_personality_v0"),
    ];
    let mut want = ptr::null();
    if ffi::ocerz_mode != MODE_NATIVE || (*dep).is_virtual == 0 {
        return;
    }
    if libc::strcmp(
        ptr::addr_of!((*dep).install_name).cast(),
        cstr_ptr(c"/usr/lib/libSystem.B.dylib"),
    ) == 0
        && (libc::strncmp(name, cstr_ptr(c"__Unwind_"), 9) == 0
            || libc::strncmp(name, cstr_ptr(c"_unw_"), 5) == 0
            || libc::strcmp(name, cstr_ptr(c"___register_frame")) == 0
            || libc::strcmp(name, cstr_ptr(c"___deregister_frame")) == 0)
    {
        want = cstr_ptr(c"/usr/lib/libunwind.1.dylib");
    } else if libc::strcmp(
        ptr::addr_of!((*dep).install_name).cast(),
        cstr_ptr(c"/usr/lib/libobjc.A.dylib"),
    ) == 0
    {
        for candidate in objc_eh {
            if want.is_null() && libc::strcmp(name, candidate) == 0 {
                want = cstr_ptr(c"/usr/lib/libc++abi.dylib");
            }
        }
    }
    let mut path = [0 as c_char; 1024];
    if want.is_null()
        || !dimg_find_by_install_name(want).is_null()
        || super::load::native_guest_path(want, path.as_mut_ptr(), path.len()) == 0
    {
        return;
    }
    if super::load::load_disk_dylib(cache, want, img, ptr::null()).is_null() {
        crate::ocerz_log!(
            "dynamic: %s wanted %s for %s and it did not load\n",
            ptr::addr_of!((*img).path).cast::<c_char>(),
            want,
            name
        );
    }
}

unsafe fn resolve_import(
    cache: *mut OcerzCache,
    img: *mut DynImage,
    name: *const c_char,
    libord: c_int,
    weak: c_int,
) -> u64 {
    let mut value = 0u64;
    let mut found = 0;
    let mut virtual_dep = 0;
    let mut tgt = ptr::null();
    if libord > 0 {
        tgt = dimg_ordinal_name(img, libord);
        if !tgt.is_null() {
            let mut dep = dimg_find_by_install_name(tgt);
            if dep.is_null() {
                dep = dimg_find_by_path(tgt);
            }
            if dep.is_null() && tgt.read() == b'@' as c_char {
                let mut ex = [0 as c_char; 1024];
                if super::load::expand_at_prefix(img, tgt, ex.as_mut_ptr(), ex.len()) != 0 {
                    dep = dimg_find_by_path(ex.as_ptr());
                    if dep.is_null() {
                        dep = dimg_find_by_install_name(ex.as_ptr());
                    }
                }
            }
            if !dep.is_null() {
                native_guest_runtime_for(cache, img, dep, name);
                value = ocerz_image_self_resolve_ex(dep, name, &mut found);
                virtual_dep =
                    (ffi::ocerz_mode == MODE_NATIVE && ffi::ocerz_vdylib_have(tgt) != 0) as c_int;
            }
            if found == 0 && dep.is_null() && tgt.read() != b'@' as c_char {
                value = ffi::ocerz_cache_resolve_in_image(cache, tgt, name, &mut found);
            }
        }
    }
    static mut WEAK_ALL_CACHE: c_int = -1;
    if WEAK_ALL_CACHE < 0 {
        WEAK_ALL_CACHE = (!libc::getenv(cstr_ptr(c"OCERZ_WEAK_ALL_CACHE")).is_null()) as c_int;
    }
    if found == 0 && libord == -3 {
        let loaded: Option<unsafe extern "C" fn(u64) -> c_int> = if WEAK_ALL_CACHE != 0 {
            None
        } else {
            Some(ffi::ocerz_dyldapi_cache_image_loaded)
        };
        value = ffi::ocerz_cache_resolve_weak_ex(cache, name, &mut found, loaded);
    } else if found == 0 {
        value = ffi::ocerz_cache_resolve_ex(cache, name, &mut found);
    }
    if found == 0 && (libord == -3 || libord == 0 || libord == -2) {
        value = ocerz_image_self_resolve_ex(img, name, &mut found);
    }
    if found == 0 && virtual_dep != 0 {
        let mut hit: *const c_char = ptr::null();
        value = virt_flat_resolve_ex(name, tgt, &mut found, &mut hit);
        if found != 0 {
            crate::ocerz_log!(
                "dynamic: %s in %s bound in %s instead\n",
                name,
                if tgt.is_null() {
                    cstr_ptr(c"(flat)")
                } else {
                    tgt
                },
                if hit.is_null() { cstr_ptr(c"?") } else { hit }
            );
        }
    }
    if found == 0 && virtual_dep == 0 {
        value = disk_flat_resolve_ex(name, &mut found);
    }
    if found == 0 && (libord == -1 || libord == -2 || libord == -3) {
        value = super::dlopen::main_image_resolve_ex(name, &mut found);
    }
    if found == 0 && ffi::ocerz_mode == MODE_NATIVE {
        let mut hit: *const c_char = ptr::null();
        value = virt_ondemand_resolve_ex(cache, img, name, &mut found, &mut hit);
        if found != 0 {
            crate::ocerz_log!(
                "dynamic: %s in %s bound in %s instead\n",
                name,
                if tgt.is_null() {
                    cstr_ptr(c"(flat)")
                } else {
                    tgt
                },
                if hit.is_null() { cstr_ptr(c"?") } else { hit }
            );
        }
    }
    if found == 0 && weak == 0 {
        if ffi::ocerz_mode == MODE_NATIVE {
            native_miss_add(
                if libord == -1 {
                    cstr_ptr(c"(main executable)")
                } else {
                    tgt
                },
                name,
                ptr::addr_of!((*img).path).cast(),
            );
        } else if libord == BIND_ORDINAL_FLAT_LOOKUP {
            g_dynlookup_miss += 1;
            if !libc::getenv(cstr_ptr(c"OCERZ_DYNLOOKUPLOG")).is_null() {
                libc::fprintf(
                    crate::log::stderr(),
                    cstr_ptr(c"ocerz: dynamic-lookup miss: %s, wanted by %s\n"),
                    name,
                    if (*img).install_name[0] != 0 {
                        ptr::addr_of!((*img).install_name).cast::<c_char>()
                    } else {
                        ptr::addr_of!((*img).path).cast::<c_char>()
                    },
                );
            }
        } else {
            static mut SHOWN: c_int = 0;
            static mut ALL: c_int = -1;
            if ALL < 0 {
                ALL = (!libc::getenv(cstr_ptr(c"OCERZ_ALLMISS")).is_null()) as c_int;
            }
            if ALL != 0 || SHOWN < MISS_LIST_MAX {
                crate::ocerz_fatal!(
                    "unresolved import: %s, wanted by %s from %s\n",
                    name,
                    if (*img).install_name[0] != 0 {
                        ptr::addr_of!((*img).install_name).cast::<c_char>()
                    } else {
                        ptr::addr_of!((*img).path).cast::<c_char>()
                    },
                    if !tgt.is_null() && tgt.read() != 0 {
                        tgt
                    } else {
                        cstr_ptr(c"the flat namespace")
                    }
                );
            } else if SHOWN == MISS_LIST_MAX {
                crate::ocerz_fatal!(
                    "unresolved import: further ones are not listed; OCERZ_ALLMISS=1 lists them all\n"
                );
            }
            SHOWN += 1;
        }
    }
    value
}

pub(super) unsafe fn apply_fixups(img: *mut DynImage, cache: *mut OcerzCache) -> c_int {
    if (*img).cf_off == 0 {
        return ffi::OCERZ_OK;
    }
    let cf = (*img).slice.add((*img).cf_off as usize);
    let starts_off = rd32(cf.add(4));
    let imports_off = rd32(cf.add(8));
    let symbols_off = rd32(cf.add(12));
    let imports_cnt = rd32(cf.add(16));
    let mut ivals = if imports_cnt != 0 && imports_cnt < (1 << 24) {
        libc::calloc(imports_cnt as usize, core::mem::size_of::<u64>()).cast::<u64>()
    } else {
        ptr::null_mut()
    };
    let mut idone = if !ivals.is_null() {
        libc::calloc(imports_cnt as usize, 1).cast::<u8>()
    } else {
        ptr::null_mut()
    };
    if !ivals.is_null() && idone.is_null() {
        libc::free(ivals.cast());
        ivals = ptr::null_mut();
    }
    let sii = cf.add(starts_off as usize);
    let seg_count = rd32(sii);
    for s in 0..seg_count {
        let so = rd32(sii.add(4 + s as usize * 4));
        if so == 0 {
            continue;
        }
        let sis = sii.add(so as usize);
        let page_size = rd16(sis.add(4));
        let ptr_format = rd16(sis.add(6));
        let seg_off = rd64(sis.add(8));
        let page_count = rd16(sis.add(0x14));
        let page_start = sis.add(0x16);
        if ptr_format != 2 && ptr_format != 6 {
            crate::ocerz_fatal!(
                "unsupported chained pointer format %u\n",
                ptr_format as c_uint
            );
            libc::free(ivals.cast());
            libc::free(idone.cast());
            return ffi::OCERZ_EUNSUP;
        }
        for pg in 0..page_count {
            let start = rd16(page_start.add(pg as usize * 2));
            if start == 0xffff {
                continue;
            }
            let mut addr = (*img)
                .load_base
                .wrapping_add(seg_off)
                .wrapping_add((pg as u64).wrapping_mul(page_size as u64))
                .wrapping_add(start as u64);
            loop {
                let raw = crate::ported::dyldapi::hostmem::ocerz_ld(addr, 8);
                let bind = ((raw >> 63) & 1) as c_int;
                let next = ((raw >> 51) & 0xfff) as u32;
                if bind != 0 {
                    let ordinal = (raw & 0xff_ffff) as u32;
                    let addend = (raw >> 24) & 0xff;
                    let mut value = 0u64;
                    if ordinal < imports_cnt {
                        let imp = rd32(cf.add(imports_off as usize + ordinal as usize * 4));
                        let noff = imp >> 9;
                        let libord = (imp as u8 as i8) as c_int;
                        let weakimp = ((imp >> 8) & 1) as c_int;
                        let name = cf
                            .add(symbols_off as usize + noff as usize)
                            .cast::<c_char>();
                        if !idone.is_null() && idone.add(ordinal as usize).read() != 0 {
                            value = ivals.add(ordinal as usize).read();
                        } else {
                            value = resolve_import(cache, img, name, libord, weakimp);
                            if !idone.is_null() {
                                ivals.add(ordinal as usize).write(value);
                                idone.add(ordinal as usize).write(1);
                            }
                        }
                    }
                    crate::ported::dyldapi::hostmem::ocerz_st(addr, 8, value.wrapping_add(addend));
                } else {
                    let target = raw & 0x0f_ffff_ffff;
                    let high8 = (raw >> 36) & 0xff;
                    let value = if ptr_format == 6 {
                        (*img).load_base.wrapping_add(target)
                    } else {
                        target.wrapping_add((*img).slide)
                    } | high8.wrapping_shl(56);
                    crate::ported::dyldapi::hostmem::ocerz_st(addr, 8, value);
                }
                if next == 0 {
                    break;
                }
                addr = addr.wrapping_add((next as u64).wrapping_mul(4));
            }
        }
    }
    libc::free(ivals.cast());
    libc::free(idone.cast());
    ffi::OCERZ_OK
}

#[repr(C)]
struct ClassicMemo {
    name: *const c_char,
    libord: c_int,
    weak: c_int,
    valid: c_int,
    value: u64,
}

unsafe fn classic_resolve(
    img: *mut DynImage,
    cache: *mut OcerzCache,
    name: *const c_char,
    libord: c_int,
    weak: c_int,
    memo: *mut ClassicMemo,
) -> u64 {
    if (*memo).valid != 0
        && name == (*memo).name
        && libord == (*memo).libord
        && weak == (*memo).weak
    {
        return (*memo).value;
    }
    let value = resolve_import(cache, img, name, libord, weak);
    (*memo).name = name;
    (*memo).libord = libord;
    (*memo).weak = weak;
    (*memo).value = value;
    (*memo).valid = 1;
    value
}

unsafe fn classic_rebase(img: *mut DynImage) {
    if (*img).rebase_size == 0 {
        return;
    }
    let mut p = (*img).slice.add((*img).rebase_off as usize);
    let end = p.add((*img).rebase_size as usize);
    let mut addr = 0u64;
    let mut done = 0;
    while p < end && done == 0 {
        let byte = p.read();
        p = p.add(1);
        let op = byte & 0xf0;
        let imm = byte & 0x0f;
        match op {
            0x00 => done = 1,
            0x10 => {}
            0x20 => {
                let off = self_uleb(&mut p, end);
                if imm < (*img).seg_count as u8 {
                    addr = (*img).seg_vmaddr[imm as usize]
                        .wrapping_add((*img).slide)
                        .wrapping_add(off);
                }
            }
            0x30 => addr = addr.wrapping_add(self_uleb(&mut p, end)),
            0x40 => addr = addr.wrapping_add((imm as u64).wrapping_mul(8)),
            0x50 => {
                for _ in 0..imm {
                    let value = crate::ported::dyldapi::hostmem::ocerz_ld(addr, 8)
                        .wrapping_add((*img).slide);
                    crate::ported::dyldapi::hostmem::ocerz_st(addr, 8, value);
                    addr = addr.wrapping_add(8);
                }
            }
            0x60 => {
                let count = self_uleb(&mut p, end);
                for _ in 0..count {
                    let value = crate::ported::dyldapi::hostmem::ocerz_ld(addr, 8)
                        .wrapping_add((*img).slide);
                    crate::ported::dyldapi::hostmem::ocerz_st(addr, 8, value);
                    addr = addr.wrapping_add(8);
                }
            }
            0x70 => {
                let value =
                    crate::ported::dyldapi::hostmem::ocerz_ld(addr, 8).wrapping_add((*img).slide);
                crate::ported::dyldapi::hostmem::ocerz_st(addr, 8, value);
                addr = addr.wrapping_add(8).wrapping_add(self_uleb(&mut p, end));
            }
            0x80 => {
                let count = self_uleb(&mut p, end);
                let skip = self_uleb(&mut p, end);
                for _ in 0..count {
                    let value = crate::ported::dyldapi::hostmem::ocerz_ld(addr, 8)
                        .wrapping_add((*img).slide);
                    crate::ported::dyldapi::hostmem::ocerz_st(addr, 8, value);
                    addr = addr.wrapping_add(8).wrapping_add(skip);
                }
            }
            _ => done = 1,
        }
    }
}

unsafe fn classic_bind_stream(
    img: *mut DynImage,
    cache: *mut OcerzCache,
    mut p: *const u8,
    end: *const u8,
    is_lazy: c_int,
) {
    let mut memo = ClassicMemo {
        name: ptr::null(),
        libord: 0,
        weak: 0,
        valid: 0,
        value: 0,
    };
    let mut addr = 0u64;
    let mut name = cstr_ptr(c"");
    let mut addend = 0i64;
    let mut libord = 0;
    let mut weak = 0;
    let mut done = 0;
    while p < end && done == 0 {
        let byte = p.read();
        p = p.add(1);
        let op = byte & 0xf0;
        let imm = byte & 0x0f;
        match op {
            0x00 => {
                if is_lazy != 0 {
                    addr = 0;
                    addend = 0;
                    weak = 0;
                } else {
                    done = 1;
                }
            }
            0x10 => libord = imm as c_int,
            0x20 => libord = self_uleb(&mut p, end) as c_int,
            0x30 => {
                libord = if imm != 0 {
                    ((0xf0 | imm) as u8 as i8) as c_int
                } else {
                    0
                }
            }
            0x40 => {
                weak = (imm & 1 != 0) as c_int;
                name = p.cast();
                p = p.add(libc::strlen(name) + 1);
            }
            0x50 => {}
            0x60 => addend = self_sleb(&mut p, end),
            0x70 => {
                let off = self_uleb(&mut p, end);
                if imm < (*img).seg_count as u8 {
                    addr = (*img).seg_vmaddr[imm as usize]
                        .wrapping_add((*img).slide)
                        .wrapping_add(off);
                }
            }
            0x80 => addr = addr.wrapping_add(self_uleb(&mut p, end)),
            0x90 => {
                let value = classic_resolve(img, cache, name, libord, weak, &mut memo);
                crate::ported::dyldapi::hostmem::ocerz_st(
                    addr,
                    8,
                    if value != 0 {
                        value.wrapping_add(addend as u64)
                    } else {
                        0
                    },
                );
                addr = addr.wrapping_add(8);
            }
            0xa0 => {
                let value = classic_resolve(img, cache, name, libord, weak, &mut memo);
                crate::ported::dyldapi::hostmem::ocerz_st(
                    addr,
                    8,
                    if value != 0 {
                        value.wrapping_add(addend as u64)
                    } else {
                        0
                    },
                );
                addr = addr.wrapping_add(8).wrapping_add(self_uleb(&mut p, end));
            }
            0xb0 => {
                let value = classic_resolve(img, cache, name, libord, weak, &mut memo);
                crate::ported::dyldapi::hostmem::ocerz_st(
                    addr,
                    8,
                    if value != 0 {
                        value.wrapping_add(addend as u64)
                    } else {
                        0
                    },
                );
                addr = addr.wrapping_add(8 + (imm as u64).wrapping_mul(8));
            }
            0xc0 => {
                let count = self_uleb(&mut p, end);
                let skip = self_uleb(&mut p, end);
                for _ in 0..count {
                    let value = classic_resolve(img, cache, name, libord, weak, &mut memo);
                    crate::ported::dyldapi::hostmem::ocerz_st(
                        addr,
                        8,
                        if value != 0 {
                            value.wrapping_add(addend as u64)
                        } else {
                            0
                        },
                    );
                    addr = addr.wrapping_add(8).wrapping_add(skip);
                }
            }
            _ => done = 1,
        }
    }
}

unsafe fn legacy_ordinal(img: *const DynImage, n_desc: u16) -> c_int {
    const MH_TWOLEVEL: u32 = 0x80;
    let flags = rd32((*img).slice.add(24));
    let ord = ((n_desc >> 8) & 0xff) as c_int;
    if flags & MH_TWOLEVEL == 0 || ord == 0xff {
        return BIND_ORDINAL_FLAT_LOOKUP;
    }
    if ord == 0xfe {
        return -1;
    }
    ord
}

unsafe fn legacy_symbol(
    img: *mut DynImage,
    cache: *mut OcerzCache,
    syms: *const u8,
    nsyms: u32,
    strs: *const c_char,
    strsize: u32,
    index: u32,
) -> u64 {
    const N_ABS: u8 = 0x02;
    const N_WEAK_REF: u16 = 0x0040;
    if index >= nsyms {
        return 0;
    }
    let n = syms.add(index as usize * 16);
    let strx = rd32(n);
    let ntype = n.add(4).read();
    let desc = n.add(6).cast::<u16>().read_unaligned();
    let value = rd64(n.add(8));
    if ntype & N_TYPE == N_SECT {
        return value.wrapping_add((*img).slide);
    }
    if ntype & N_TYPE == N_ABS {
        return value;
    }
    if strx >= strsize {
        return 0;
    }
    resolve_import(
        cache,
        img,
        strs.add(strx as usize),
        legacy_ordinal(img, desc),
        (desc & N_WEAK_REF != 0) as c_int,
    )
}

pub(super) unsafe fn apply_legacy_relocations(img: *mut DynImage, cache: *mut OcerzCache) -> c_int {
    const VM_PROT_WRITE: u32 = 0x2;
    const SECTION_TYPE: u32 = 0xff;
    const S_NON_LAZY_SYMBOL_POINTERS: u32 = 0x6;
    const S_LAZY_SYMBOL_POINTERS: u32 = 0x7;
    const INDIRECT_SYMBOL_LOCAL: u32 = 0x8000_0000;
    const INDIRECT_SYMBOL_ABS: u32 = 0x4000_0000;
    let h = (*img).slice;
    let ncmds = rd32(h.add(16));
    let mut lc = h.add(core::mem::size_of::<MachHeader64>());
    let mut symtab = ptr::null();
    let mut dysymtab = ptr::null();
    let mut reloc_base = 0u64;
    let mut have_writable = 0;
    for _ in 0..ncmds {
        let cmd = rd32(lc);
        let size = rd32(lc.add(4));
        if size < 8 {
            return ffi::OCERZ_OK;
        }
        if cmd == LC_SYMTAB {
            symtab = lc;
        } else if cmd == 0xb {
            dysymtab = lc;
        } else if cmd == LC_SEGMENT_64
            && have_writable == 0
            && rd32(lc.add(60)) & VM_PROT_WRITE != 0
        {
            reloc_base = rd64(lc.add(24)).wrapping_add((*img).slide);
            have_writable = 1;
        }
        lc = lc.add(size as usize);
    }
    if symtab.is_null() || dysymtab.is_null() {
        return ffi::OCERZ_OK;
    }
    let syms = h.add(rd32(symtab.add(8)) as usize);
    let nsyms = rd32(symtab.add(12));
    let strs = h.add(rd32(symtab.add(16)) as usize).cast::<c_char>();
    let strsize = rd32(symtab.add(20));
    let indirect = h.add(rd32(dysymtab.add(0x38)) as usize);
    let nindirect = rd32(dysymtab.add(0x3c));
    let extrel = h.add(rd32(dysymtab.add(0x40)) as usize);
    let nextrel = rd32(dysymtab.add(0x44));
    let locrel = h.add(rd32(dysymtab.add(0x48)) as usize);
    let nlocrel = rd32(dysymtab.add(0x4c));
    if have_writable != 0 && (*img).slide != 0 {
        for i in 0..nlocrel {
            let ent = locrel.add(i as usize * 8);
            let addr = rd32(ent) as i32;
            let info = rd32(ent.add(4));
            if (info >> 25) & 3 != 3
                || (info >> 24) & 1 != 0
                || (info >> 27) & 1 != 0
                || info >> 28 != 0
            {
                continue;
            }
            let at = reloc_base.wrapping_add(addr as i64 as u64);
            let value = crate::ported::dyldapi::hostmem::ocerz_ld(at, 8).wrapping_add((*img).slide);
            crate::ported::dyldapi::hostmem::ocerz_st(at, 8, value);
        }
    }
    if have_writable != 0 {
        for i in 0..nextrel {
            let ent = extrel.add(i as usize * 8);
            let addr = rd32(ent) as i32;
            let info = rd32(ent.add(4));
            if (info >> 25) & 3 != 3
                || (info >> 24) & 1 != 0
                || (info >> 27) & 1 == 0
                || info >> 28 != 0
            {
                continue;
            }
            let at = reloc_base.wrapping_add(addr as i64 as u64);
            let target = legacy_symbol(img, cache, syms, nsyms, strs, strsize, info & 0x00ff_ffff);
            let value = crate::ported::dyldapi::hostmem::ocerz_ld(at, 8).wrapping_add(target);
            crate::ported::dyldapi::hostmem::ocerz_st(at, 8, value);
        }
    }
    lc = h.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        let cmd = rd32(lc);
        let size = rd32(lc.add(4));
        if cmd == LC_SEGMENT_64 {
            let nsects = rd32(lc.add(64));
            for k in 0..nsects {
                if 72 + (k + 1) * 80 > size {
                    break;
                }
                let sc = lc.add(72 + k as usize * 80);
                let kind = rd32(sc.add(64)) & SECTION_TYPE;
                if kind != S_NON_LAZY_SYMBOL_POINTERS && kind != S_LAZY_SYMBOL_POINTERS {
                    continue;
                }
                let addr = rd64(sc.add(32)).wrapping_add((*img).slide);
                let count = rd64(sc.add(40)) / 8;
                let first = rd32(sc.add(68));
                for j in 0..count {
                    if first as u64 + j >= nindirect as u64 {
                        break;
                    }
                    let index = rd32(indirect.add((first as u64 + j) as usize * 4));
                    let at = addr.wrapping_add(j.wrapping_mul(8));
                    if index == INDIRECT_SYMBOL_ABS
                        || index == INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS
                    {
                        continue;
                    }
                    if index == INDIRECT_SYMBOL_LOCAL {
                        let value = crate::ported::dyldapi::hostmem::ocerz_ld(at, 8)
                            .wrapping_add((*img).slide);
                        crate::ported::dyldapi::hostmem::ocerz_st(at, 8, value);
                    } else {
                        let value = legacy_symbol(img, cache, syms, nsyms, strs, strsize, index);
                        crate::ported::dyldapi::hostmem::ocerz_st(at, 8, value);
                    }
                }
            }
        }
        lc = lc.add(size as usize);
    }
    ffi::OCERZ_OK
}

pub(super) unsafe fn apply_classic_fixups(img: *mut DynImage, cache: *mut OcerzCache) -> c_int {
    if (*img).has_dyld_info == 0 {
        return apply_legacy_relocations(img, cache);
    }
    classic_rebase(img);
    if (*img).bind_size != 0 {
        classic_bind_stream(
            img,
            cache,
            (*img).slice.add((*img).bind_off as usize),
            (*img)
                .slice
                .add((*img).bind_off.wrapping_add((*img).bind_size) as usize),
            0,
        );
    }
    if (*img).weak_bind_size != 0 {
        classic_bind_stream(
            img,
            cache,
            (*img).slice.add((*img).weak_bind_off as usize),
            (*img)
                .slice
                .add((*img).weak_bind_off.wrapping_add((*img).weak_bind_size) as usize),
            0,
        );
    }
    if (*img).lazy_bind_size != 0 {
        classic_bind_stream(
            img,
            cache,
            (*img).slice.add((*img).lazy_bind_off as usize),
            (*img)
                .slice
                .add((*img).lazy_bind_off.wrapping_add((*img).lazy_bind_size) as usize),
            1,
        );
    }
    ffi::OCERZ_OK
}

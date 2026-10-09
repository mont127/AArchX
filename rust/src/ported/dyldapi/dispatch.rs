//! Dyld API setup, callback helpers, and dispatch for libdyld trampoline slots.

use core::ffi::{c_char, c_int, c_void};
use core::mem;
use core::ptr;

use crate::ffi::{
    OCERZ_BRIDGE_OFF, OCERZ_DYLDAPI_LO, OCERZ_ENOMEM, OCERZ_EUNSUP, OCERZ_OK, OCERZ_RAX, OCERZ_RBP,
    OCERZ_RCX, OCERZ_RDI, OCERZ_RDX, OCERZ_RSI, OCERZ_RSP, OCERZ_STEP_OK as STEP_OK_RAW, OcerzCPU,
    OcerzVM, ocerz_addr_readable, ocerz_cache_resolve, ocerz_cpu_restore_saved, ocerz_dlclose,
    ocerz_dlerror, ocerz_dlopen, ocerz_dlopen_from, ocerz_dlsym, ocerz_map_anywhere,
};

use super::closure::{
    build_version_at_least, cache_find_canonical, cache_path_for_mh, find_section_any,
    find_section_sz, image_for_pc, image_nearest_symbol, image_slide, link_time_library_version,
    parse_build_version, runtime_library_version, set_has,
};
use super::hostmem::{ocerz_g2h, ocerz_h2g, ocerz_ld, ocerz_st};
use super::macho::*;
use super::objc::{
    api_for_each_objc_class, api_for_each_objc_protocol, api_objc_register_callbacks,
    api_register_for_bulk_image_loads, selpool_canonical,
};
use super::{
    cstr_ptr, DYLDAPI_NOOP_OFF, DYLDAPI_VTABLE_SIZE, g_apis_global, g_block_scratch, g_cache,
    g_cache_size, g_cache_start, g_closure_cap, g_closure_hash, g_closure_hash_mask,
    g_closure_mh, g_closure_n, g_clsopt, g_headeropt_ro, g_headeropt_rw, g_main_bv_minos,
    g_main_bv_platform, g_main_bv_sdk, g_main_path, g_objc_dlopen_mapped, g_objc_make_mutable,
    g_protoopt, g_sel_pool, g_selopt,
};

const CACHE_HDR_OBJC_OPTS: usize = 0x1d0;
const OBJC_OPTS_SEL_TABLE: usize = 0x18;
const OBJC_OPTS_SEL_BASE: usize = 0x30;
const PROT_READ: c_int = libc::PROT_READ;
const PROT_WRITE: c_int = libc::PROT_WRITE;
const OCERZ_STEP_OK: c_int = STEP_OK_RAW as c_int;

unsafe extern "C" {
    fn ocerz_vdylib_dispatch(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int;
}

pub(crate) unsafe fn api_return(cpu: *mut OcerzCPU, result: u64) {
    unsafe {
        let rsp = (*cpu).gpr[OCERZ_RSP as usize];
        (*cpu).rip = ocerz_ld(rsp, 8);
        (*cpu).gpr[OCERZ_RSP as usize] = rsp + 8;
        (*cpu).gpr[OCERZ_RAX as usize] = result;
    }
}

unsafe fn lazy_load_path(mh: u64, flag: u64, weak: *mut c_int) -> *const c_char {
    unsafe {
        let h = ocerz_g2h(mh).cast::<MachHeader64>();
        if h.is_null() || (*h).magic != MH_MAGIC_64 || flag == 0 {
            return ptr::null();
        }
        let slide = image_slide(mh);
        let mut le_addr = 0u64;
        let mut le_fileoff = 0u64;
        let mut le_size = 0u64;
        let mut lc = h.add(1).cast::<u8>();
        for _ in 0..(*h).ncmds {
            let l = lc.cast::<LoadCommand>();
            if (*l).cmd == LC_SEGMENT_64 {
                let s = lc.cast::<SegmentCommand64>();
                if libc::strcmp((*s).segname.as_ptr(), cstr_ptr(c"__LINKEDIT")) == 0 {
                    le_addr = (*s).vmaddr.wrapping_add(slide);
                    le_fileoff = (*s).fileoff;
                    le_size = (*s).filesize;
                }
            }
            lc = lc.add((*l).cmdsize as usize);
        }
        if le_addr == 0 {
            return ptr::null();
        }
        lc = h.add(1).cast::<u8>();
        for _ in 0..(*h).ncmds {
            let l = lc.cast::<LoadCommand>();
            if (*l).cmd == LC_LAZY_LOAD_DYLIB_INFO
                && (*l).cmdsize >= mem::size_of::<LinkeditDataCommand>() as u32
            {
                let dataoff = ptr::read_unaligned(lc.add(8).cast::<u32>());
                let datasize = ptr::read_unaligned(lc.add(12).cast::<u32>());
                if dataoff as u64 >= le_fileoff
                    && (dataoff as u64 - le_fileoff) <= le_size
                    && datasize >= 24
                    && datasize as u64 <= le_size - (dataoff as u64 - le_fileoff)
                {
                    let info = ocerz_g2h(
                        le_addr
                            .wrapping_add(dataoff as u64)
                            .wrapping_sub(le_fileoff),
                    )
                    .cast::<u8>();
                    let pathoff = ptr::read_unaligned(info.cast::<u32>());
                    let flagoff = ptr::read_unaligned(info.add(4).cast::<u32>());
                    let flags = ptr::read_unaligned(info.add(8).cast::<u16>());
                    let ptrfmt = ptr::read_unaligned(info.add(10).cast::<u16>());
                    let chainoff = ptr::read_unaligned(info.add(12).cast::<u32>());
                    let symcount = ptr::read_unaligned(info.add(16).cast::<u32>());
                    if mh.wrapping_add(flagoff as u64) == flag && pathoff < datasize {
                        let path = info.add(pathoff as usize).cast::<c_char>();
                        if !libc::memchr(path.cast::<c_void>(), 0, (datasize - pathoff) as usize)
                            .is_null()
                        {
                            if !libc::getenv(cstr_ptr(c"OCERZ_LAZYLOADLOG")).is_null() {
                                libc::fprintf(
                                    crate::log::stderr(),
                                    cstr_ptr(c"ocerz: LAZYLOAD path=%s flag=%#llx fmt=%u chain=%#x symbols=%u\n"),
                                    path,
                                    flag as libc::c_ulonglong,
                                    ptrfmt as u32,
                                    chainoff,
                                    symcount,
                                );
                            }
                            if !weak.is_null() {
                                *weak = (flags & 1) as c_int;
                            }
                            return path;
                        }
                    }
                }
            }
            lc = lc.add((*l).cmdsize as usize);
        }
        ptr::null()
    }
}

unsafe fn api_lazy_load(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let flag = (*cpu).gpr[OCERZ_RSI as usize];
        let mh = (*cpu).gpr[OCERZ_RDX as usize];
        let mut weak = 0;
        let path = lazy_load_path(mh, flag, &mut weak);
        if path.is_null() {
            crate::ocerz_log!(
                "dyldapi: lazy_load flag=%#llx mh=%#llx has no matching metadata\n",
                flag as libc::c_ulonglong,
                mh as libc::c_ulonglong
            );
            api_return(cpu, 0);
            return OCERZ_STEP_OK;
        }
        if ocerz_ld(flag, 4) == 0 {
            let saved = *cpu;
            let handle = ocerz_dlopen(vm, path, 0x100);
            ocerz_cpu_restore_saved(cpu, &saved);
            if (*vm).jit_ordered_required != 0 {
                (*cpu).ras_top = 0;
            }
            if (*vm).exited != 0 {
                return OCERZ_STEP_OK;
            }
            if handle != 0 {
                ocerz_st(flag, 4, 1);
            } else if weak == 0 {
                crate::ocerz_log!("dyldapi: lazy_load failed for %s\n", path);
            }
        }
        api_return(cpu, 0);
        OCERZ_STEP_OK
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyldapi_setup(cache: *mut crate::ffi::OcerzCache) -> c_int {
    unsafe {
        let fnptr = ocerz_cache_resolve(cache, cstr_ptr(c"_dyld_get_active_platform"));
        if fnptr == 0 {
            return OCERZ_EUNSUP as c_int;
        }
        let p = fnptr as *const u8;
        if *p != 0x55 || *p.add(1) != 0x48 || *p.add(2) != 0x89 || *p.add(3) != 0xe5 {
            return OCERZ_EUNSUP as c_int;
        }
        if *p.add(4) != 0x48 || *p.add(5) != 0x8b || *p.add(6) != 0x3d {
            return OCERZ_EUNSUP as c_int;
        }
        let disp = ptr::read_unaligned(p.add(7).cast::<i32>());
        g_apis_global = fnptr.wrapping_add(11).wrapping_add(disp as i64 as u64);
        let vtable = ocerz_map_anywhere(DYLDAPI_VTABLE_SIZE, PROT_READ | PROT_WRITE);
        let obj = ocerz_map_anywhere(0x400, PROT_READ | PROT_WRITE);
        if vtable == 0 || obj == 0 {
            return OCERZ_ENOMEM as c_int;
        }
        let mut off = 0;
        while off < DYLDAPI_VTABLE_SIZE {
            ocerz_st(vtable + off, 8, OCERZ_DYLDAPI_LO as u64 + off);
            off += 8;
        }
        ocerz_st(obj, 8, vtable);
        ocerz_st(g_apis_global, 8, obj);
        g_cache = cache;
        g_cache_start = (*cache).base;
        g_cache_size = 0x40000000000;
        g_closure_cap = (*cache).images_cnt as c_int + 256;
        let mut hsz = 1u32;
        while hsz < g_closure_cap as u32 * 2 {
            hsz <<= 1;
        }
        g_closure_mh = libc::calloc(g_closure_cap as usize, mem::size_of::<u64>()).cast();
        g_closure_hash = libc::calloc(hsz as usize, mem::size_of::<u64>()).cast();
        g_objc_dlopen_mapped = libc::calloc(g_closure_cap as usize, mem::size_of::<u64>()).cast();
        if g_closure_mh.is_null() || g_closure_hash.is_null() || g_objc_dlopen_mapped.is_null() {
            return OCERZ_ENOMEM as c_int;
        }
        g_closure_hash_mask = hsz - 1;
        super::closure::compute_closure(cache, crate::ffi::ocerz_main_mh);
        crate::ocerz_log!(
            "dyldapi: closure %d image(s) of %u in the cache\n",
            g_closure_n,
            (*cache).images_cnt
        );
        let mut objc_mh = 0;
        for i in 0..(*cache).images_cnt {
            let mut path = ptr::null();
            let mh = crate::ffi::ocerz_cache_image_addr(cache, i, &mut path);
            if mh != 0
                && !path.is_null()
                && libc::strcmp(path, cstr_ptr(c"/usr/lib/libobjc.A.dylib")) == 0
            {
                objc_mh = mh;
                break;
            }
        }
        let opt = if objc_mh != 0 {
            find_section_any(objc_mh, cstr_ptr(c"__objc_opt_ro"))
        } else {
            0
        };
        if opt != 0 {
            let selopt_off = ptr::read_unaligned(ocerz_g2h(opt + 0x08).cast::<i32>());
            let ro_off = ptr::read_unaligned(ocerz_g2h(opt + 0x0c).cast::<i32>());
            let rw_off = ptr::read_unaligned(ocerz_g2h(opt + 0x18).cast::<i32>());
            let cls_off = ptr::read_unaligned(ocerz_g2h(opt + 0x20).cast::<i32>());
            let proto_off = ptr::read_unaligned(ocerz_g2h(opt + 0x24).cast::<i32>());
            let sel_off = ptr::read_unaligned(ocerz_g2h(opt + 0x28).cast::<i64>());
            g_selopt = if selopt_off != 0 {
                opt.wrapping_add(selopt_off as i64 as u64)
            } else {
                0
            };
            g_headeropt_ro = if ro_off != 0 {
                opt.wrapping_add(ro_off as i64 as u64)
            } else {
                0
            };
            g_headeropt_rw = if rw_off != 0 {
                opt.wrapping_add(rw_off as i64 as u64)
            } else {
                0
            };
            g_clsopt = if cls_off != 0 {
                opt.wrapping_add(cls_off as i64 as u64)
            } else {
                0
            };
            g_protoopt = if proto_off != 0 {
                opt.wrapping_add(proto_off as i64 as u64)
            } else {
                0
            };
            g_sel_pool = if sel_off != 0 {
                opt.wrapping_add(sel_off as i64 as u64)
            } else {
                0
            };
        }
        if g_selopt == 0 && g_sel_pool != 0 {
            let hd = ocerz_g2h((*cache).base).cast::<u8>();
            let mapping_off = ptr::read_unaligned(hd.add(0x10).cast::<u32>());
            let mut opts_off = 0u64;
            let mut opts_size = 0u64;
            if mapping_off as usize >= CACHE_HDR_OBJC_OPTS + 16 {
                opts_off = ptr::read_unaligned(hd.add(CACHE_HDR_OBJC_OPTS).cast::<u64>());
                opts_size = ptr::read_unaligned(hd.add(CACHE_HDR_OBJC_OPTS + 8).cast::<u64>());
            }
            if opts_off != 0 && opts_size >= (OBJC_OPTS_SEL_BASE + 8) as u64 {
                let ob = hd.add(opts_off as usize);
                let table_off = ptr::read_unaligned(ob.add(OBJC_OPTS_SEL_TABLE).cast::<u64>());
                let sel_base = ptr::read_unaligned(ob.add(OBJC_OPTS_SEL_BASE).cast::<u64>());
                if table_off != 0 && (*cache).base.wrapping_add(sel_base) == g_sel_pool {
                    g_selopt = (*cache).base.wrapping_add(table_off);
                }
            }
            crate::ocerz_log!(
                "dyldapi: selector table %s the cache header's objc optimizations\n",
                if g_selopt != 0 {
                    cstr_ptr(c"taken from")
                } else {
                    cstr_ptr(c"not found in")
                }
            );
        }
        parse_build_version(
            crate::ffi::ocerz_main_mh,
            &raw mut g_main_bv_platform,
            &raw mut g_main_bv_minos,
            &raw mut g_main_bv_sdk,
        );
        g_block_scratch = ocerz_map_anywhere(0x40, PROT_READ | PROT_WRITE);
        g_objc_make_mutable = ocerz_map_anywhere(0x40, PROT_READ | PROT_WRITE);
        if g_objc_make_mutable != 0 {
            for o in (0..0x40).step_by(8) {
                ocerz_st(
                    g_objc_make_mutable + o,
                    8,
                    OCERZ_DYLDAPI_LO as u64 + DYLDAPI_NOOP_OFF,
                );
            }
        }
        crate::ocerz_log!(
            "dyldapi: apis_global=%#llx obj=%#llx vtable=%#llx opt=%#llx rw=%#llx ro=%#llx\n",
            g_apis_global as libc::c_ulonglong,
            obj as libc::c_ulonglong,
            vtable as libc::c_ulonglong,
            opt as libc::c_ulonglong,
            g_headeropt_rw as libc::c_ulonglong,
            g_headeropt_ro as libc::c_ulonglong
        );
        OCERZ_OK as c_int
    }
}

unsafe fn mh_copy_uuid(mh: u64, out: u64) -> c_int {
    unsafe {
        if mh == 0 || out == 0 || ocerz_addr_readable(mh + 32) == 0 {
            return 0;
        }
        let ncmds = ocerz_ld(mh + 16, 4) as u32;
        let sizeofcmds = ocerz_ld(mh + 20, 4) as u32;
        let mut lc = mh + 32;
        let end = lc.wrapping_add(sizeofcmds as u64);
        for _ in 0..ncmds {
            if lc.wrapping_add(8) > end {
                break;
            }
            let cmd = ocerz_ld(lc, 4) as u32;
            let cmdsize = ocerz_ld(lc + 4, 4) as u32;
            if cmdsize < 8 || lc.wrapping_add(cmdsize as u64) > end {
                return 0;
            }
            if cmd == 0x1b && cmdsize >= 24 {
                ocerz_st(out, 8, ocerz_ld(lc + 8, 8));
                ocerz_st(out + 8, 8, ocerz_ld(lc + 16, 8));
                return 1;
            }
            lc += cmdsize as u64;
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyldapi_dispatch(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let off = (*cpu).rip.wrapping_sub(OCERZ_DYLDAPI_LO as u64);
        if off == OCERZ_BRIDGE_OFF as u64 {
            return ocerz_vdylib_dispatch(vm, cpu);
        }
        static mut trace: c_int = -1;
        if trace < 0 {
            trace = (!libc::getenv(cstr_ptr(c"OCERZ_DYLDAPI_TRACE")).is_null()) as c_int;
        }
        if trace != 0 {
            let a1 = (*cpu).gpr[OCERZ_RSI as usize];
            let mut text = [0 as c_char; 96];
            let mut k = 0usize;
            while k + 1 < text.len()
                && a1 != 0
                && ocerz_addr_readable(a1.wrapping_add(k as u64)) != 0
            {
                let ch = ocerz_ld(a1 + k as u64, 1) as u8 as c_char;
                if ch < 0x20 || ch > 0x7e {
                    if ch != 0 {
                        text[0] = 0;
                    }
                    break;
                }
                text[k] = ch;
                text[k + 1] = 0;
                if ch == 0 {
                    break;
                }
                k += 1;
            }
            let sp = (*cpu).gpr[OCERZ_RSP as usize];
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(c"ocerz: DYLDAPI +%#llx a1=%#llx a2=%#llx caller=%#llx%s%s\n"),
                off as libc::c_ulonglong,
                a1 as libc::c_ulonglong,
                (*cpu).gpr[OCERZ_RDX as usize] as libc::c_ulonglong,
                if ocerz_addr_readable(sp) != 0 {
                    ocerz_ld(sp, 8) as libc::c_ulonglong
                } else {
                    0
                },
                if text[0] != 0 {
                    cstr_ptr(c" a1-string=")
                } else {
                    cstr_ptr(c"")
                },
                text.as_ptr(),
            );
        }
        match off {
            DYLDAPI_NOOP_OFF => {
                api_return(cpu, 0);
                OCERZ_STEP_OK
            }
            0x210 => {
                api_return(cpu, 1);
                OCERZ_STEP_OK
            }
            0x238 => {
                api_return(
                    cpu,
                    build_version_at_least(
                        g_main_bv_platform,
                        g_main_bv_sdk,
                        (*cpu).gpr[OCERZ_RSI as usize],
                    ),
                );
                OCERZ_STEP_OK
            }
            0x240 => {
                api_return(
                    cpu,
                    build_version_at_least(
                        g_main_bv_platform,
                        g_main_bv_minos,
                        (*cpu).gpr[OCERZ_RSI as usize],
                    ),
                );
                OCERZ_STEP_OK
            }
            0x188 => {
                api_return(cpu, g_main_bv_sdk as u64);
                OCERZ_STEP_OK
            }
            0x190 => {
                api_return(cpu, g_main_bv_minos as u64);
                OCERZ_STEP_OK
            }
            0x178 | 0x180 => {
                let mut plat = 0;
                let mut minos = 0;
                let mut sdk = 0;
                parse_build_version(
                    (*cpu).gpr[OCERZ_RSI as usize],
                    &mut plat,
                    &mut minos,
                    &mut sdk,
                );
                api_return(
                    cpu,
                    if off == 0x178 {
                        sdk as u64
                    } else {
                        minos as u64
                    },
                );
                OCERZ_STEP_OK
            }
            0x3d8 | 0x3e0 => {
                api_return(
                    cpu,
                    ((if off == 0x3d8 {
                        g_main_bv_sdk
                    } else {
                        g_main_bv_minos
                    } as u64)
                        << 32)
                        | g_main_bv_platform as u64,
                );
                OCERZ_STEP_OK
            }
            0x218 => {
                let p = (*cpu).gpr[OCERZ_RSI as usize] as u32 as usize;
                let base = [0u64, 1, 2, 3, 4, 5, 2, 2, 3, 4, 10, 11, 11];
                api_return(
                    cpu,
                    if p < base.len() {
                        *base.get_unchecked(p)
                    } else {
                        p as u64
                    },
                );
                OCERZ_STEP_OK
            }
            0x1d8 => {
                api_return(
                    cpu,
                    mh_copy_uuid(
                        (*cpu).gpr[OCERZ_RSI as usize],
                        (*cpu).gpr[OCERZ_RDX as usize],
                    ) as u64,
                );
                OCERZ_STEP_OK
            }
            0x1e0 => {
                let out = (*cpu).gpr[OCERZ_RSI as usize];
                let ok = !g_cache.is_null() && out != 0;
                if ok {
                    ocerz_st(out, 8, ocerz_ld((*g_cache).base + 0x58, 8));
                    ocerz_st(out + 8, 8, ocerz_ld((*g_cache).base + 0x60, 8));
                }
                api_return(cpu, ok as u64);
                OCERZ_STEP_OK
            }
            0x2d0 => {
                let pathg = (*cpu).gpr[OCERZ_RSI as usize];
                let mut real = ptr::null();
                api_return(
                    cpu,
                    if pathg != 0
                        && cache_find_canonical(ocerz_g2h(pathg).cast(), &mut real) != 0
                        && !real.is_null()
                    {
                        ocerz_h2g(real.cast())
                    } else {
                        0
                    },
                );
                OCERZ_STEP_OK
            }
            0x278 => {
                api_return(cpu, 0);
                OCERZ_STEP_OK
            }
            0x228 | 0x230 => {
                let mut plat = 0;
                let mut minos = 0;
                let mut sdk = 0;
                parse_build_version(
                    (*cpu).gpr[OCERZ_RSI as usize],
                    &mut plat,
                    &mut minos,
                    &mut sdk,
                );
                api_return(
                    cpu,
                    build_version_at_least(
                        plat,
                        if off == 0x228 { sdk } else { minos },
                        (*cpu).gpr[OCERZ_RDX as usize],
                    ),
                );
                OCERZ_STEP_OK
            }
            0x198 | 0x1a0 | 0x328 | 0x360 | 0x3c8 | 0x3d0 => {
                api_return(cpu, 0);
                OCERZ_STEP_OK
            }
            0x1e8 => {
                let addr = (*cpu).gpr[OCERZ_RSI as usize];
                let len = (*cpu).gpr[OCERZ_RDX as usize];
                api_return(
                    cpu,
                    (in_cache(addr) && in_cache(addr.wrapping_add(len))) as u64,
                );
                OCERZ_STEP_OK
            }
            0x318 | 0x320 | 0x368 | 0x370 => {
                (*cpu).gpr[OCERZ_RDX as usize] = 0;
                api_return(cpu, 2);
                OCERZ_STEP_OK
            }
            0x290 => api_register_for_bulk_image_loads(vm, cpu),
            0x2b0 => api_for_each_objc_protocol(vm, cpu),
            0x2d8 => {
                let path = (*cpu).gpr[OCERZ_RSI as usize];
                api_return(
                    cpu,
                    (path != 0
                        && cache_find_canonical(ocerz_g2h(path).cast(), ptr::null_mut()) != 0)
                        as u64,
                );
                OCERZ_STEP_OK
            }
            0x68 | 0x2e0 => {
                let pathg = (*cpu).gpr[OCERZ_RSI as usize];
                let mode = (*cpu).gpr[OCERZ_RDX as usize];
                let host = if pathg != 0 {
                    ocerz_g2h(pathg).cast::<c_char>()
                } else {
                    ptr::null()
                };
                let dlbt = libc::getenv(cstr_ptr(c"OCERZ_DLBT"));
                if !host.is_null() && !dlbt.is_null() && !libc::strstr(host, dlbt).is_null() {
                    libc::fprintf(
                        crate::log::stderr(),
                        cstr_ptr(c"ocerz: DLBT dlopen(\"%s\") caller-chain:"),
                        host,
                    );
                    let mut fp = (*cpu).gpr[OCERZ_RBP as usize];
                    for _ in 0..14 {
                        if fp < 0x300000000 {
                            break;
                        }
                        libc::fprintf(
                            crate::log::stderr(),
                            cstr_ptr(c" %#llx"),
                            ocerz_ld(fp + 8, 8) as libc::c_ulonglong,
                        );
                        let nf = ocerz_ld(fp, 8);
                        if nf <= fp {
                            break;
                        }
                        fp = nf;
                    }
                    libc::fprintf(crate::log::stderr(), cstr_ptr(c"\n"));
                }
                let caller = if off == 0x2e0 {
                    (*cpu).gpr[OCERZ_RCX as usize]
                } else {
                    let rsp = (*cpu).gpr[OCERZ_RSP as usize];
                    if ocerz_addr_readable(rsp) != 0 {
                        ocerz_ld(rsp, 8)
                    } else {
                        0
                    }
                };
                let saved = *cpu;
                let h = ocerz_dlopen_from(vm, host, mode as c_int, caller);
                ocerz_cpu_restore_saved(cpu, &saved);
                if (*vm).jit_ordered_required != 0 {
                    (*cpu).ras_top = 0;
                }
                if (*vm).exited != 0 {
                    return OCERZ_STEP_OK;
                }
                api_return(cpu, h);
                OCERZ_STEP_OK
            }
            0x88 => {
                let pathg = (*cpu).gpr[OCERZ_RSI as usize];
                let mut ok = 0;
                if pathg != 0 {
                    let host = ocerz_g2h(pathg).cast::<c_char>();
                    ok = (cache_find_canonical(host, ptr::null_mut()) != 0
                        || libc::access(host, libc::R_OK) == 0) as u64;
                    if !libc::getenv(cstr_ptr(c"OCERZ_DLPATH")).is_null() {
                        libc::fprintf(
                            crate::log::stderr(),
                            cstr_ptr(c"ocerz: dlopen_preflight(\"%s\") -> %llu\n"),
                            host,
                            ok as libc::c_ulonglong,
                        );
                    }
                }
                api_return(cpu, ok);
                OCERZ_STEP_OK
            }
            0x80 => {
                let handle = (*cpu).gpr[OCERZ_RSI as usize];
                let symg = (*cpu).gpr[OCERZ_RDX as usize];
                let addr = if symg != 0 {
                    ocerz_dlsym(handle, ocerz_g2h(symg).cast())
                } else {
                    0
                };
                if !libc::getenv(cstr_ptr(c"OCERZ_DLSYMLOG")).is_null() {
                    let rsp = (*cpu).gpr[OCERZ_RSP as usize];
                    libc::fprintf(
                        crate::log::stderr(),
                        cstr_ptr(c"ocerz: DLSYM[%d] handle=%#llx \"%s\" -> %#llx caller=%#llx\n"),
                        libc::getpid(),
                        handle as libc::c_ulonglong,
                        if symg != 0 {
                            ocerz_g2h(symg).cast()
                        } else {
                            cstr_ptr(c"(null)")
                        },
                        addr as libc::c_ulonglong,
                        if ocerz_addr_readable(rsp) != 0 {
                            ocerz_ld(rsp, 8) as libc::c_ulonglong
                        } else {
                            0
                        },
                    );
                }
                api_return(cpu, addr);
                OCERZ_STEP_OK
            }
            0x70 => {
                api_return(cpu, ocerz_dlclose((*cpu).gpr[OCERZ_RSI as usize]) as u64);
                OCERZ_STEP_OK
            }
            0x78 => {
                api_return(cpu, ocerz_dlerror());
                OCERZ_STEP_OK
            }
            0x10 => {
                api_return(cpu, g_closure_n as u64);
                OCERZ_STEP_OK
            }
            0x18 => {
                let idx = (*cpu).gpr[OCERZ_RSI as usize] as u32;
                api_return(
                    cpu,
                    if idx < g_closure_n as u32 {
                        *g_closure_mh.add(idx as usize)
                    } else {
                        0
                    },
                );
                OCERZ_STEP_OK
            }
            0x20 => {
                let idx = (*cpu).gpr[OCERZ_RSI as usize] as u32;
                api_return(
                    cpu,
                    if idx < g_closure_n as u32 {
                        image_slide(*g_closure_mh.add(idx as usize))
                    } else {
                        0
                    },
                );
                OCERZ_STEP_OK
            }
            0x28 => {
                let idx = (*cpu).gpr[OCERZ_RSI as usize] as u32;
                let name = if idx == 0 && g_main_path != 0 {
                    g_main_path
                } else if idx < g_closure_n as u32 {
                    cache_path_for_mh(g_cache, *g_closure_mh.add(idx as usize)) as u64
                } else {
                    0
                };
                api_return(cpu, name);
                OCERZ_STEP_OK
            }
            0x40 => {
                let name = (*cpu).gpr[OCERZ_RSI as usize];
                let version = if name != 0 {
                    link_time_library_version(ocerz_g2h(name).cast()) as u32 as u64
                } else {
                    u32::MAX as u64
                };
                api_return(cpu, version);
                OCERZ_STEP_OK
            }
            0x48 => {
                let name = (*cpu).gpr[OCERZ_RSI as usize];
                let version = if name != 0 {
                    runtime_library_version(ocerz_g2h(name).cast()) as u32 as u64
                } else {
                    u32::MAX as u64
                };
                api_return(cpu, version);
                OCERZ_STEP_OK
            }
            0x50 => {
                let buf = (*cpu).gpr[OCERZ_RSI as usize];
                let szp = (*cpu).gpr[OCERZ_RDX as usize];
                if g_main_path == 0 {
                    api_return(cpu, u64::MAX);
                    return OCERZ_STEP_OK;
                }
                let host = ocerz_g2h(g_main_path).cast::<c_char>();
                let need = libc::strlen(host) as u32 + 1;
                let have = if szp != 0 { ocerz_ld(szp, 4) as u32 } else { 0 };
                if buf != 0 && have >= need {
                    libc::memcpy(ocerz_g2h(buf), host.cast(), need as usize);
                    api_return(cpu, 0);
                } else {
                    if szp != 0 {
                        ocerz_st(szp, 4, need as u64);
                    }
                    api_return(cpu, u64::MAX);
                }
                OCERZ_STEP_OK
            }
            0x160 => {
                api_return(cpu, image_slide((*cpu).gpr[OCERZ_RSI as usize]));
                OCERZ_STEP_OK
            }
            0x168 => {
                let addr = (*cpu).gpr[OCERZ_RSI as usize];
                let mut path = 0u64;
                for i in 0..g_closure_n {
                    let mh = *g_closure_mh.add(i as usize);
                    let h = ocerz_g2h(mh).cast::<MachHeader64>();
                    if (*h).magic != MH_MAGIC_64 {
                        continue;
                    }
                    let slide = image_slide(mh);
                    let mut lc = h.add(1).cast::<u8>();
                    let mut hit = false;
                    for _ in 0..(*h).ncmds {
                        let l = lc.cast::<LoadCommand>();
                        if (*l).cmd == LC_SEGMENT_64 {
                            let s = lc.cast::<SegmentCommand64>();
                            let lo = (*s).vmaddr.wrapping_add(slide);
                            if (*s).vmsize != 0 && addr >= lo && addr < lo.wrapping_add((*s).vmsize)
                            {
                                hit = true;
                            }
                        }
                        lc = lc.add((*l).cmdsize as usize);
                        if hit {
                            break;
                        }
                    }
                    if hit {
                        path = if mh == crate::ffi::ocerz_main_mh {
                            g_main_path
                        } else {
                            cache_path_for_mh(g_cache, mh) as u64
                        };
                        break;
                    }
                }
                api_return(cpu, path);
                OCERZ_STEP_OK
            }
            0x170 => {
                let pc = (*cpu).gpr[OCERZ_RSI as usize];
                let info = (*cpu).gpr[OCERZ_RDX as usize];
                let mh = image_for_pc(pc);
                if mh != 0 && info != 0 {
                    let mut eh_sz = 0;
                    let mut cu_sz = 0;
                    let eh = find_section_sz(mh, cstr_ptr(c"__eh_frame"), &mut eh_sz);
                    let cu = find_section_sz(mh, cstr_ptr(c"__unwind_info"), &mut cu_sz);
                    ocerz_st(info, 8, mh);
                    ocerz_st(info + 8, 8, eh);
                    ocerz_st(info + 16, 8, if eh != 0 { eh_sz } else { 0 });
                    ocerz_st(info + 24, 8, cu);
                    ocerz_st(info + 32, 8, if cu != 0 { cu_sz } else { 0 });
                    if !libc::getenv(cstr_ptr(c"OCERZ_UNWLOG")).is_null() {
                        libc::fprintf(
                            crate::log::stderr(),
                            cstr_ptr(c"ocerz: UNWLOG pc=%#llx mh=%#llx eh=%#llx eh_sz=%#llx cu=%#llx cu_sz=%#llx\n"),
                            pc as libc::c_ulonglong,
                            mh as libc::c_ulonglong,
                            eh as libc::c_ulonglong,
                            eh_sz as libc::c_ulonglong,
                            cu as libc::c_ulonglong,
                            cu_sz as libc::c_ulonglong,
                        );
                    }
                } else if !libc::getenv(cstr_ptr(c"OCERZ_UNWLOG")).is_null() {
                    libc::fprintf(
                        crate::log::stderr(),
                        cstr_ptr(c"ocerz: UNWLOG pc=%#llx mh=0 (no image)\n"),
                        pc as libc::c_ulonglong,
                    );
                }
                api_return(cpu, (mh != 0) as u64);
                OCERZ_STEP_OK
            }
            0x1b0 => {
                api_return(cpu, image_for_pc((*cpu).gpr[OCERZ_RSI as usize]));
                OCERZ_STEP_OK
            }
            0x60 => {
                let addr = (*cpu).gpr[OCERZ_RSI as usize];
                let info = (*cpu).gpr[OCERZ_RDX as usize];
                let mh = image_for_pc(addr);
                if mh == 0 || info == 0 {
                    api_return(cpu, 0);
                    return OCERZ_STEP_OK;
                }
                let fname = if mh == crate::ffi::ocerz_main_mh {
                    g_main_path
                } else {
                    cache_path_for_mh(g_cache, mh) as u64
                };
                let mut sname = 0;
                let mut saddr = 0;
                image_nearest_symbol(mh, addr, &mut sname, &mut saddr);
                ocerz_st(info, 8, fname);
                ocerz_st(info + 8, 8, mh);
                ocerz_st(info + 16, 8, sname);
                ocerz_st(info + 24, 8, saddr);
                api_return(cpu, 1);
                OCERZ_STEP_OK
            }
            0x2f0 => {
                api_return(cpu, crate::ffi::ocerz_main_mh);
                OCERZ_STEP_OK
            }
            0x1f8 => {
                let size_out = (*cpu).gpr[OCERZ_RSI as usize];
                if size_out != 0 {
                    ocerz_st(size_out, 8, g_cache_size);
                }
                api_return(cpu, g_cache_start);
                OCERZ_STEP_OK
            }
            0x2a8 => api_for_each_objc_class(vm, cpu),
            0x358 => api_objc_register_callbacks(vm, cpu),
            0x378 => {
                let kindsect: [*const c_char; 21] = [
                    cstr_ptr(c"__swift5_protos"),
                    cstr_ptr(c"__swift5_proto"),
                    cstr_ptr(c"__swift5_types"),
                    cstr_ptr(c"__swift5_replace"),
                    cstr_ptr(c"__swift5_replac2"),
                    cstr_ptr(c"__swift5_acfuncs"),
                    cstr_ptr(c"__objc_imageinfo"),
                    cstr_ptr(c"__objc_selrefs"),
                    cstr_ptr(c"__objc_msgrefs"),
                    cstr_ptr(c"__objc_classrefs"),
                    cstr_ptr(c"__objc_superrefs"),
                    cstr_ptr(c"__objc_protorefs"),
                    cstr_ptr(c"__objc_classlist"),
                    cstr_ptr(c"__objc_nlclslist"),
                    cstr_ptr(c"__objc_stublist"),
                    cstr_ptr(c"__objc_catlist"),
                    cstr_ptr(c"__objc_catlist2"),
                    cstr_ptr(c"__objc_nlcatlist"),
                    cstr_ptr(c"__objc_protolist"),
                    cstr_ptr(c"__objc_fork_ok"),
                    cstr_ptr(c"__objc_rawisa"),
                ];
                let mh = (*cpu).gpr[OCERZ_RSI as usize];
                let kind = (*cpu).gpr[OCERZ_RCX as usize];
                let mut addr = 0;
                let mut size = 0;
                let swift = kind <= 5;
                let loadmark = kind == 13 || kind == 17;
                let latecat = (kind == 15 || kind == 16)
                    && mh >= g_cache_start
                    && !g_closure_hash.is_null()
                    && !set_has(g_closure_hash, g_closure_hash_mask, mh)
                    && libc::getenv(cstr_ptr(c"OCERZ_NO_LATE_CATLIST")).is_null();
                if mh != 0
                    && (swift || loadmark || latecat || mh < g_cache_start)
                    && (kind as usize) < kindsect.len()
                {
                    addr = find_section_sz(mh, kindsect[kind as usize], &mut size);
                }
                if !libc::getenv(cstr_ptr(c"OCERZ_SECLOG")).is_null() {
                    libc::fprintf(
                        crate::log::stderr(),
                        c"ocerz: SECINFO mh=%#llx kind=%llu (%s) -> addr=%#llx size=%#llx\n"
                            .as_ptr(),
                        mh as libc::c_ulonglong,
                        kind as libc::c_ulonglong,
                        if (kind as usize) < kindsect.len() {
                            kindsect[kind as usize]
                        } else {
                            cstr_ptr(c"?")
                        },
                        addr as libc::c_ulonglong,
                        size as libc::c_ulonglong,
                    );
                }
                (*cpu).gpr[OCERZ_RDX as usize] = size;
                api_return(cpu, addr);
                OCERZ_STEP_OK
            }
            0x308 => {
                let mut occupied = 0u32;
                if g_clsopt != 0 {
                    ptr::copy_nonoverlapping(
                        (g_clsopt + 8) as *const u8,
                        (&mut occupied as *mut u32).cast::<u8>(),
                        4,
                    );
                }
                api_return(cpu, occupied as u64);
                OCERZ_STEP_OK
            }
            0x3a8 => {
                api_return(cpu, g_headeropt_rw);
                OCERZ_STEP_OK
            }
            0x2a0 => {
                let name = (*cpu).gpr[OCERZ_RSI as usize];
                let result = if name != 0 {
                    selpool_canonical(ocerz_g2h(name).cast())
                } else {
                    0
                };
                if result == 0 && name != 0 && !libc::getenv(cstr_ptr(c"OCERZ_SELLOG")).is_null() {
                    libc::fprintf(
                        crate::log::stderr(),
                        cstr_ptr(c"ocerz: SELMISS \"%s\"\n"),
                        ocerz_g2h(name).cast::<c_char>(),
                    );
                }
                api_return(cpu, result);
                OCERZ_STEP_OK
            }
            0x3b0 => {
                api_return(cpu, g_headeropt_ro);
                OCERZ_STEP_OK
            }
            0x438 => api_lazy_load(vm, cpu),
            0x450 => {
                (*cpu).gpr[OCERZ_RDX as usize] = 0;
                api_return(cpu, 0);
                OCERZ_STEP_OK
            }
            _ => {
                crate::ocerz_log!(
                    "dyldapi: unimplemented vtable slot +%#llx (this=%#llx a0=%#llx a1=%#llx a2=%#llx caller=%#llx)\n",
                    off as libc::c_ulonglong,
                    (*cpu).gpr[OCERZ_RDI as usize] as libc::c_ulonglong,
                    (*cpu).gpr[OCERZ_RSI as usize] as libc::c_ulonglong,
                    (*cpu).gpr[OCERZ_RDX as usize] as libc::c_ulonglong,
                    (*cpu).gpr[OCERZ_RCX as usize] as libc::c_ulonglong,
                    ocerz_ld((*cpu).gpr[OCERZ_RSP as usize], 8) as libc::c_ulonglong
                );
                (*cpu).gpr[OCERZ_RDX as usize] = 0;
                api_return(cpu, 0);
                OCERZ_STEP_OK
            }
        }
    }
}

unsafe fn in_cache(addr: u64) -> bool {
    unsafe { addr >= g_cache_start && addr < g_cache_start.wrapping_add(g_cache_size) }
}

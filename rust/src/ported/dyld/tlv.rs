//! Guest and native TLV registration, lookup, initialization, and thread release.

use super::eager::{g_tlv_registered, g_tlv_registered_n, tlv_is_registered, tlv_register};
use super::*;
use core::sync::atomic::Ordering;

#[repr(C)]
struct TlvSections {
    descs: u64,
    descs_size: u64,
    tmpl: u64,
    block_size: u64,
    has_data: c_int,
}

static mut NATIVE_TLV_SIZE: [u32; NATIVE_TLV_KEYS + 1] = [0; NATIVE_TLV_KEYS + 1];

unsafe fn tlv_find_sections(mh: u64, ts: *mut TlvSections) -> c_int {
    let h = crate::ported::dyldapi::hostmem::ocerz_g2h(mh).cast::<u8>();
    if rd32(h) != MH_MAGIC_64 {
        return 0;
    }
    let slide = super::eager::image_slide_d(mh);
    let ncmds = rd32(h.add(16));
    let mut lc = h.add(core::mem::size_of::<MachHeader64>());
    let mut vars_addr = 0u64;
    let mut vars_size = 0u64;
    let mut tmpl_lo = u64::MAX;
    let mut tmpl_hi = 0u64;
    let mut data_lo = u64::MAX;
    for _ in 0..ncmds {
        if rd32(lc) == LC_SEGMENT_64 {
            let ns = rd32(lc.add(64));
            let mut sec = lc.add(72);
            for _ in 0..ns {
                let ty = (rd32(sec.add(64)) & 0xff) as u8;
                let sa = rd64(sec.add(32));
                let size = rd64(sec.add(40));
                if ty == 0x13 {
                    vars_addr = sa;
                    vars_size = size;
                } else if ty == 0x11 || ty == 0x12 {
                    if sa < tmpl_lo {
                        tmpl_lo = sa;
                    }
                    if sa.wrapping_add(size) > tmpl_hi {
                        tmpl_hi = sa.wrapping_add(size);
                    }
                    if ty == 0x11 && sa < data_lo {
                        data_lo = sa;
                    }
                }
                sec = sec.add(80);
            }
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
    (*ts).descs = vars_addr.wrapping_add(slide as u64);
    (*ts).descs_size = if vars_addr != 0 { vars_size } else { 0 };
    (*ts).tmpl = tmpl_lo.wrapping_add(slide as u64);
    (*ts).block_size = if tmpl_hi > tmpl_lo {
        tmpl_hi - tmpl_lo
    } else {
        0
    };
    (*ts).has_data = (data_lo != u64::MAX) as c_int;
    1
}

unsafe fn tlv_pack_descriptors(ts: *const TlvSections, thunk: u64, key: u32) {
    let mut off = 0u64;
    while off.wrapping_add(24) <= (*ts).descs_size {
        let desc = (*ts).descs.wrapping_add(off);
        let packed_off =
            crate::ported::dyldapi::hostmem::ocerz_ld(desc.wrapping_add(0xc), 4) as u32;
        let var_off = if packed_off != 0 {
            packed_off
        } else {
            crate::ported::dyldapi::hostmem::ocerz_ld(desc.wrapping_add(0x10), 8) as u32
        };
        let self_rel = if (*ts).has_data != 0 {
            ((*ts).tmpl as i64).wrapping_sub(desc.wrapping_add(0x10) as i64) as i32
        } else {
            0
        };
        if thunk != 0 {
            crate::ported::dyldapi::hostmem::ocerz_st(desc, 8, thunk);
        }
        crate::ported::dyldapi::hostmem::ocerz_st(desc.wrapping_add(8), 4, key as u64);
        crate::ported::dyldapi::hostmem::ocerz_st(desc.wrapping_add(0xc), 4, var_off as u64);
        crate::ported::dyldapi::hostmem::ocerz_st(
            desc.wrapping_add(0x10),
            4,
            self_rel as u32 as u64,
        );
        crate::ported::dyldapi::hostmem::ocerz_st(
            desc.wrapping_add(0x14),
            4,
            (*ts).block_size as u32 as u64,
        );
        off = off.wrapping_add(24);
    }
}

pub(super) unsafe fn ocerz_tlv_register_image(
    vm: *mut OcerzVM,
    cache: *mut OcerzCache,
    mh: u64,
    stack_top: u64,
) {
    if mh == 0 || tlv_is_registered(mh) {
        return;
    }
    let mut ts = TlvSections {
        descs: 0,
        descs_size: 0,
        tmpl: 0,
        block_size: 0,
        has_data: 0,
    };
    if tlv_find_sections(mh, &mut ts) == 0 {
        return;
    }
    tlv_register(mh);
    if ts.descs_size < 24 {
        return;
    }
    let boot = ffi::ocerz_cache_resolve(cache, cstr_ptr(c"__tlv_bootstrap"));
    if boot == 0 {
        crate::ocerz_log!(
            "dynamic: TLV: __tlv_bootstrap unresolved, skipping mh=%#llx\n",
            mh as c_ulonglong
        );
        return;
    }
    let tlv_get_addr = boot.wrapping_add(8);
    let keycreate = ffi::ocerz_cache_resolve(cache, cstr_ptr(c"_pthread_key_create"));
    if keycreate == 0 {
        crate::ocerz_log!(
            "dynamic: TLV: _pthread_key_create unresolved, skipping mh=%#llx\n",
            mh as c_ulonglong
        );
        return;
    }
    let scratch = ffi::ocerz_map_anywhere(16, libc::PROT_READ | libc::PROT_WRITE);
    if scratch == 0 {
        return;
    }
    crate::ported::dyldapi::hostmem::ocerz_st(scratch, 8, 0);
    let args = [scratch, 0];
    let krc = ffi::ocerz_vm_call(vm, keycreate, args.as_ptr(), 2, stack_top);
    if (*vm).exited != 0 {
        return;
    }
    let key = crate::ported::dyldapi::hostmem::ocerz_ld(scratch, 4) as u32;
    if krc != 0 || key < 0xa || key > 0x2ff {
        crate::ocerz_log!(
            "dynamic: TLV: pthread_key_create failed (rc=%llu key=%u) mh=%#llx\n",
            krc as c_ulonglong,
            key,
            mh as c_ulonglong
        );
        return;
    }
    tlv_pack_descriptors(&ts, tlv_get_addr, key);
    crate::ocerz_log!(
        "dynamic: TLV: registered mh=%#llx key=%u block=%llu descs@%#llx size=%llu\n",
        mh as c_ulonglong,
        key,
        ts.block_size as c_ulonglong,
        ts.descs as c_ulonglong,
        ts.descs_size as c_ulonglong
    );
}

pub(super) unsafe fn native_tlv_register_image(mh: u64) {
    if mh == 0 || tlv_is_registered(mh) {
        return;
    }
    let mut ts = TlvSections {
        descs: 0,
        descs_size: 0,
        tmpl: 0,
        block_size: 0,
        has_data: 0,
    };
    if tlv_find_sections(mh, &mut ts) == 0 {
        return;
    }
    tlv_register(mh);
    if ts.descs_size < 24 {
        return;
    }
    let key = g_native_tlv_keys.load(Ordering::SeqCst).wrapping_add(1);
    if key as usize > NATIVE_TLV_KEYS {
        crate::ocerz_log!(
            "dynamic: TLV: no native key left for mh=%#llx\n",
            mh as c_ulonglong
        );
        return;
    }
    ptr::addr_of_mut!(NATIVE_TLV_SIZE)
        .cast::<u32>()
        .add(key as usize)
        .write(ts.block_size as u32);
    g_native_tlv_keys.store(key, Ordering::SeqCst);
    tlv_pack_descriptors(&ts, 0, key);
    crate::ocerz_log!(
        "dynamic: TLV: native mh=%#llx key=%u block=%llu descs@%#llx size=%llu\n",
        mh as c_ulonglong,
        key,
        ts.block_size as c_ulonglong,
        ts.descs as c_ulonglong,
        ts.descs_size as c_ulonglong
    );
}

pub(super) unsafe fn native_tlv_register_loaded(main_mh: u64) {
    native_tlv_register_image(main_mh);
    for i in 0..g_dimgs_n {
        let img = dimg_at(i as usize);
        native_tlv_register_image((*img).load_base);
    }
}

unsafe fn native_tlv_first_touch(gs_base: u64, desc: u64, key: u32, off: u32, size: u32) -> u64 {
    if key > g_native_tlv_keys.load(Ordering::SeqCst)
        || size
            != ptr::addr_of!(NATIVE_TLV_SIZE)
                .cast::<u32>()
                .add(key as usize)
                .read()
    {
        crate::ocerz_log!(
            "dynamic: TLV: descriptor %#llx names key %u with a %u-byte block, which no registered image has\n",
            desc as c_ulonglong,
            key,
            size
        );
        return 0;
    }
    let slot = gs_base.wrapping_add(ffi::OCERZ_TLV_TABLE_SLOT as u64);
    let mut table = crate::ported::dyldapi::hostmem::ocerz_ld(slot, 8);
    if table == 0 {
        table = ffi::ocerz_map_anywhere(NATIVE_TLV_TABLE_BYTES, libc::PROT_READ | libc::PROT_WRITE);
        if table == 0 {
            crate::ocerz_log!(
                "dynamic: TLV: no guest memory for the thread table of gs=%#llx\n",
                gs_base as c_ulonglong
            );
            return 0;
        }
        crate::ported::dyldapi::hostmem::ocerz_st(slot, 8, table);
    }
    let block = ffi::ocerz_map_anywhere(size as u64, libc::PROT_READ | libc::PROT_WRITE);
    if block == 0 {
        crate::ocerz_log!(
            "dynamic: TLV: no guest memory for a %u-byte block of key %u\n",
            size,
            key
        );
        return 0;
    }
    let delta =
        rd32(crate::ported::dyldapi::hostmem::ocerz_g2h(desc.wrapping_add(0x10)).cast()) as i32;
    if delta != 0 {
        ptr::copy_nonoverlapping(
            crate::ported::dyldapi::hostmem::ocerz_g2h(
                desc.wrapping_add(0x10).wrapping_add(delta as i64 as u64),
            )
            .cast::<u8>(),
            crate::ported::dyldapi::hostmem::ocerz_g2h(block).cast::<u8>(),
            size as usize,
        );
    }
    crate::ported::dyldapi::hostmem::ocerz_st(
        table.wrapping_add((key as u64).wrapping_mul(8)),
        8,
        block,
    );
    block.wrapping_add(off as u64)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_tlv_address(cpu: *mut OcerzCPU, desc: u64) -> u64 {
    let d = crate::ported::dyldapi::hostmem::ocerz_g2h(desc).cast::<u8>();
    let key = rd32(d.add(8));
    let off = rd32(d.add(0xc));
    let size = rd32(d.add(0x14));
    let gs_base = (*cpu).gs_base;
    if key == 0 || key as usize > NATIVE_TLV_KEYS || size == 0 || off > size || gs_base == 0 {
        return 0;
    }
    let table = crate::ported::dyldapi::hostmem::ocerz_ld(
        gs_base.wrapping_add(ffi::OCERZ_TLV_TABLE_SLOT as u64),
        8,
    );
    let block = if table != 0 {
        crate::ported::dyldapi::hostmem::ocerz_ld(
            table.wrapping_add((key as u64).wrapping_mul(8)),
            8,
        )
    } else {
        0
    };
    if block != 0 {
        block.wrapping_add(off as u64)
    } else {
        native_tlv_first_touch(gs_base, desc, key, off, size)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_tlv_release_thread(gs_base: u64) {
    if ffi::ocerz_mode != MODE_NATIVE || gs_base == 0 {
        return;
    }
    let slot = gs_base.wrapping_add(ffi::OCERZ_TLV_TABLE_SLOT as u64);
    let table = crate::ported::dyldapi::hostmem::ocerz_ld(slot, 8);
    if table == 0 {
        return;
    }
    crate::ported::dyldapi::hostmem::ocerz_st(slot, 8, 0);
    let keys = g_native_tlv_keys.load(Ordering::SeqCst);
    let mut key = 1;
    while key <= keys && key as usize <= NATIVE_TLV_KEYS {
        let block = crate::ported::dyldapi::hostmem::ocerz_ld(
            table.wrapping_add((key as u64).wrapping_mul(8)),
            8,
        );
        let size = ptr::addr_of!(NATIVE_TLV_SIZE)
            .cast::<u32>()
            .add(key as usize)
            .read();
        if block != 0 && size != 0 {
            ffi::ocerz_unmap(block, size as u64);
        }
        key += 1;
    }
    ffi::ocerz_unmap(table, NATIVE_TLV_TABLE_BYTES);
}

pub(super) unsafe fn ocerz_tlv_register_closure(
    vm: *mut OcerzVM,
    cache: *mut OcerzCache,
    main_mh: u64,
    stack_top: u64,
) {
    ocerz_tlv_register_image(vm, cache, main_mh, stack_top);
    let mut i = 0;
    while i < super::eager::G_EAGER_N && (*vm).exited == 0 {
        let mh = ptr::addr_of!(super::eager::G_EAGER)
            .cast::<u64>()
            .add(i as usize)
            .read();
        ocerz_tlv_register_image(vm, cache, mh, stack_top);
        i += 1;
    }
    let mut i = 0;
    while i < g_dimgs_n && (*vm).exited == 0 {
        let img = dimg_at(i as usize);
        if (*img).load_base != 0 {
            ocerz_tlv_register_image(vm, cache, (*img).load_base, stack_top);
        }
        i += 1;
    }
}

//! Image segment mapping and post-fixup protections.

use super::*;

static mut g_ro_pending: [*mut DynImage; DYN_DIMG_MAX] = [ptr::null_mut(); DYN_DIMG_MAX];
static mut g_ro_pending_n: c_int = 0;

pub(super) unsafe fn map_segments(img: *mut DynImage, is_main: c_int) -> c_int {
    let mh = (*img).slice;
    let ncmds = rd32(mh.add(16));
    (*img).is_pie = (rd32(mh.add(24)) & MH_PIE != 0) as c_int;
    let mut lc = mh.add(core::mem::size_of::<MachHeader64>());
    let mut text_vmaddr = 0u64;
    let mut have_text = 0;
    for _ in 0..ncmds {
        if rd32(lc) == LC_SEGMENT_64 && rd64(lc.add(40)) == 0 && rd64(lc.add(48)) != 0 {
            text_vmaddr = rd64(lc.add(24));
            have_text = 1;
            break;
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
    if have_text == 0 {
        return ffi::OCERZ_EFORMAT;
    }

    let mut vmlo = u64::MAX;
    let mut vmhi = 0u64;
    lc = mh.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        if rd32(lc) == LC_SEGMENT_64 {
            let vmaddr = rd64(lc.add(24));
            let vmsize = rd64(lc.add(32));
            let initprot = rd32(lc.add(56));
            if !(vmaddr == 0 && initprot == 0) {
                if vmaddr < vmlo {
                    vmlo = vmaddr;
                }
                let end = vmaddr.wrapping_add(vmsize);
                if end > vmhi {
                    vmhi = end;
                }
            }
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
    if vmhi <= vmlo {
        return ffi::OCERZ_EFORMAT;
    }

    if is_main != 0 && (*img).is_pie == 0 {
        (*img).load_base = text_vmaddr;
        (*img).slide = 0;
        lc = mh.add(core::mem::size_of::<MachHeader64>());
        for _ in 0..ncmds {
            if rd32(lc) == LC_SEGMENT_64 {
                let vmaddr = rd64(lc.add(24));
                let vmsize = rd64(lc.add(32));
                let initprot = rd32(lc.add(56));
                let mut unmapped = 0;
                if !(vmaddr == 0 && initprot == 0) && vmsize != 0 {
                    if vmaddr < ffi::OCERZ_LOW_LIMIT {
                        if vmaddr.wrapping_add(vmsize) > ffi::OCERZ_LOW_LIMIT {
                            return ffi::OCERZ_ENOMEM;
                        }
                        if ffi::ocerz_mem_init_low_shadow() != ffi::OCERZ_OK {
                            return ffi::OCERZ_ENOMEM;
                        }
                        if ffi::ocerz_mode == MODE_NATIVE
                            && rd64(lc.add(48)) == 0
                            && ffi::ocerz_host_low_readable(vmaddr, vmaddr.wrapping_add(vmsize))
                                != 0
                        {
                            crate::ocerz_log!(
                                "dynamic: zero-fill segment %.16s [%#llx, %#llx) overlaps host memory and stays unmapped\n",
                                lc.add(8).cast::<c_char>(),
                                vmaddr,
                                vmaddr.wrapping_add(vmsize)
                            );
                            unmapped = 1;
                        }
                    } else if !(vmaddr >= ffi::ocerz_arena_lo
                        && vmaddr.wrapping_add(vmsize) <= ffi::ocerz_arena_hi)
                    {
                        if ffi::ocerz_mem_register_range(vmaddr, vmaddr.wrapping_add(vmsize))
                            != ffi::OCERZ_OK
                        {
                            return ffi::OCERZ_ENOMEM;
                        }
                    }
                    if unmapped == 0
                        && ffi::ocerz_map_fixed(vmaddr, vmsize, libc::PROT_READ | libc::PROT_WRITE)
                            != ffi::OCERZ_OK
                    {
                        return ffi::OCERZ_ENOMEM;
                    }
                }
            }
            lc = lc.add(rd32(lc.add(4)) as usize);
        }
        ffi::ocerz_mem_pin_host_low();
    } else if is_main != 0 {
        (*img).load_base = ffi::ocerz_arena_lo;
        (*img).slide = (*img).load_base.wrapping_sub(text_vmaddr);
        if ffi::ocerz_map_fixed(
            vmlo.wrapping_add((*img).slide),
            vmhi - vmlo,
            libc::PROT_READ | libc::PROT_WRITE,
        ) != ffi::OCERZ_OK
        {
            return ffi::OCERZ_ENOMEM;
        }
    } else {
        let region = ffi::ocerz_map_anywhere(vmhi - vmlo, libc::PROT_READ | libc::PROT_WRITE);
        if region == 0 {
            return ffi::OCERZ_ENOMEM;
        }
        (*img).map_base = region;
        (*img).map_size = vmhi - vmlo;
        (*img).slide = region.wrapping_sub(vmlo);
        (*img).load_base = text_vmaddr.wrapping_add((*img).slide);
    }
    if is_main != 0 {
        ocerz_main_mh = (*img).load_base;
    }

    lc = mh.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        let cmd = rd32(lc);
        let csize = rd32(lc.add(4));
        if cmd == LC_SEGMENT_64 {
            let vmaddr = rd64(lc.add(24));
            let fileoff = rd64(lc.add(40));
            let filesize = rd64(lc.add(48));
            let initprot = rd32(lc.add(56));
            if (*img).seg_count < DYN_SEG_MAX as c_int {
                (*img).seg_vmaddr[(*img).seg_count as usize] = vmaddr;
                (*img).seg_count += 1;
            }
            if !(vmaddr == 0 && initprot == 0) && filesize != 0 {
                ptr::copy_nonoverlapping(
                    (*img).slice.add(fileoff as usize),
                    crate::ported::dyldapi::hostmem::ocerz_g2h(vmaddr.wrapping_add((*img).slide))
                        .cast::<u8>(),
                    filesize as usize,
                );
            }
        } else if cmd == LC_MAIN {
            (*img).main_entry = text_vmaddr
                .wrapping_add(rd64(lc.add(8)))
                .wrapping_add((*img).slide);
        } else if cmd == LC_UNIXTHREAD && csize >= 152 && rd32(lc.add(8)) == 4 {
            (*img).thread_entry = rd64(lc.add(144)).wrapping_add((*img).slide);
        } else if cmd == LC_DYLD_CHAINED_FIXUPS {
            (*img).cf_off = rd32(lc.add(8));
            (*img).cf_size = rd32(lc.add(12));
        } else if cmd == LC_DYLD_INFO || cmd == LC_DYLD_INFO_ONLY {
            (*img).has_dyld_info = 1;
            (*img).rebase_off = rd32(lc.add(8));
            (*img).rebase_size = rd32(lc.add(12));
            (*img).bind_off = rd32(lc.add(0x10));
            (*img).bind_size = rd32(lc.add(0x14));
            (*img).weak_bind_off = rd32(lc.add(0x18));
            (*img).weak_bind_size = rd32(lc.add(0x1c));
            (*img).lazy_bind_off = rd32(lc.add(0x20));
            (*img).lazy_bind_size = rd32(lc.add(0x24));
        } else if cmd == 0xc || cmd == 0x8000_001f || cmd == 0x8000_0018 {
            (*img).links_dylib = 1;
            let noff = rd32(lc.add(8));
            let dp = lc.add(noff as usize).cast::<c_char>();
            if noff < rd32(lc.add(4))
                && (!libc::strstr(dp, cstr_ptr(c"/CoreFoundation.framework/")).is_null()
                    || !libc::strstr(dp, cstr_ptr(c"/Foundation.framework/")).is_null()
                    || !libc::strstr(dp, cstr_ptr(c"/AppKit.framework/")).is_null())
            {
                (*img).links_cf = 1;
            }
        }
        lc = lc.add(csize as usize);
    }
    ffi::OCERZ_OK
}

unsafe fn apply_seg_prots(img: *mut DynImage) {
    let mh = (*img).slice;
    let ncmds = rd32(mh.add(16));
    let mut lc = mh.add(core::mem::size_of::<MachHeader64>());
    for _ in 0..ncmds {
        if rd32(lc) == LC_SEGMENT_64 {
            let vmaddr = rd64(lc.add(24));
            let vmsize = rd64(lc.add(32));
            let initprot = rd32(lc.add(60));
            let flags = rd32(lc.add(68));
            if vmsize == 0 || (vmaddr == 0 && initprot == 0) {
                lc = lc.add(rd32(lc.add(4)) as usize);
                continue;
            }
            let want = if flags & SEG_FLAG_READ_ONLY != 0 {
                libc::PROT_READ
            } else if initprot & 2 != 0 {
                -1
            } else {
                ((if initprot & 1 != 0 {
                    libc::PROT_READ
                } else {
                    0
                }) | (if initprot & VM_PROT_EXECUTE != 0 {
                    libc::PROT_EXEC
                } else {
                    0
                }))
            };
            if want > 0 {
                ffi::ocerz_protect(vmaddr.wrapping_add((*img).slide), vmsize, want);
            }
        }
        lc = lc.add(rd32(lc.add(4)) as usize);
    }
}

pub(super) unsafe fn protect_ro_segments(img: *mut DynImage) {
    static mut DIS: c_int = -1;
    if DIS < 0 {
        DIS = (!libc::getenv(cstr_ptr(c"OCERZ_NO_TEXT_RO")).is_null()) as c_int;
    }
    if DIS != 0 || (*img).slice.is_null() {
        return;
    }
    let n = g_ro_pending_n;
    for i in 0..n {
        if *ptr::addr_of!(g_ro_pending)
            .cast::<*mut DynImage>()
            .add(i as usize)
            == img
        {
            return;
        }
    }
    if n < DYN_DIMG_MAX as c_int {
        *ptr::addr_of_mut!(g_ro_pending)
            .cast::<*mut DynImage>()
            .add(n as usize) = img;
        g_ro_pending_n = n + 1;
    } else {
        apply_seg_prots(img);
    }
}

pub(super) unsafe fn protect_ro_flush() {
    let n = g_ro_pending_n;
    g_ro_pending_n = 0;
    for i in 0..n {
        let img = *ptr::addr_of!(g_ro_pending)
            .cast::<*mut DynImage>()
            .add(i as usize);
        apply_seg_prots(img);
    }
}

pub(super) unsafe fn protect_ro_drop(img: *const DynImage) {
    let n = g_ro_pending_n;
    let mut out = 0;
    for i in 0..n {
        let item = *ptr::addr_of!(g_ro_pending)
            .cast::<*mut DynImage>()
            .add(i as usize);
        if item.cast_const() != img {
            *ptr::addr_of_mut!(g_ro_pending)
                .cast::<*mut DynImage>()
                .add(out as usize) = item;
            out += 1;
        }
    }
    g_ro_pending_n = out;
}

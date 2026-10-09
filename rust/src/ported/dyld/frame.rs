//! Initial stack construction and guest program-variable setup.

use super::*;

unsafe fn put_str(sp: *mut u64, s: *const c_char) -> u64 {
    let len = libc::strlen(s).wrapping_add(1);
    *sp = (*sp).wrapping_sub(len as u64);
    ptr::copy_nonoverlapping(
        s.cast::<u8>(),
        crate::ported::dyldapi::hostmem::ocerz_g2h(*sp).cast::<u8>(),
        len,
    );
    *sp
}

pub(super) unsafe fn build_frame(
    path: *const c_char,
    argc: c_int,
    argv: *mut *mut c_char,
    envp: *mut *mut c_char,
    out: *mut DynFrame,
) -> c_int {
    let mut envc = 0;
    while !envp.is_null() && !(*envp.add(envc as usize)).is_null() {
        envc += 1;
    }
    let mut apple0 = [0 as c_char; 2048];
    libc::snprintf(
        apple0.as_mut_ptr(),
        apple0.len(),
        cstr_ptr(c"executable_path=%s"),
        path,
    );
    let mut need = libc::strlen(apple0.as_ptr())
        .wrapping_add(1)
        .wrapping_add(512);
    for i in 0..argc {
        need = need.wrapping_add(libc::strlen(*argv.add(i as usize)).wrapping_add(1));
    }
    for i in 0..envc {
        need = need.wrapping_add(libc::strlen(*envp.add(i as usize)).wrapping_add(1));
    }
    need = need.wrapping_add(
        ((argc as usize).wrapping_add(envc as usize).wrapping_add(16))
            .wrapping_mul(8)
            .wrapping_add(256),
    );
    let mut aux_size = 1u64 << 20;
    if need.wrapping_add(65536) > aux_size as usize {
        aux_size = need.wrapping_add(65536).wrapping_add(0x3fff) as u64 & !0x3fff;
    }
    let aux = ffi::ocerz_map_anywhere(aux_size, libc::PROT_READ | libc::PROT_WRITE);
    let stack = ffi::ocerz_map_anywhere(DYN_STACK_SIZE, libc::PROT_READ | libc::PROT_WRITE);
    if aux == 0 || stack == 0 {
        return ffi::OCERZ_ENOMEM;
    }
    ffi::ocerz_vm_set_main_stack(stack, stack.wrapping_add(DYN_STACK_SIZE));
    let argv_g =
        libc::calloc((argc as usize).wrapping_add(1), core::mem::size_of::<u64>()).cast::<u64>();
    let envp_g =
        libc::calloc((envc as usize).wrapping_add(1), core::mem::size_of::<u64>()).cast::<u64>();
    if argv_g.is_null() || envp_g.is_null() {
        libc::free(argv_g.cast());
        libc::free(envp_g.cast());
        return ffi::OCERZ_ENOMEM;
    }

    let exit_stub: [u8; 9] = [0x89, 0xc7, 0xb8, 0x01, 0x00, 0x00, 0x02, 0x0f, 0x05];
    (*out).exit_stub = stack.wrapping_add(DYN_STACK_SIZE).wrapping_sub(64);
    ptr::copy_nonoverlapping(
        exit_stub.as_ptr(),
        crate::ported::dyldapi::hostmem::ocerz_g2h((*out).exit_stub).cast(),
        exit_stub.len(),
    );
    (*out).stack_top = ((*out).exit_stub.wrapping_sub(256)) & !0xf;

    let mut sp = aux.wrapping_add(aux_size);
    let mut apple_g = [0u64; 8];
    for i in (0..argc).rev() {
        argv_g
            .add(i as usize)
            .write(put_str(&mut sp, *argv.add(i as usize)));
    }
    for i in (0..envc).rev() {
        envp_g
            .add(i as usize)
            .write(put_str(&mut sp, *envp.add(i as usize)));
    }
    let mut thbuf = [0 as c_char; 64];
    let mut stkbuf = [0 as c_char; 160];
    libc::snprintf(
        thbuf.as_mut_ptr(),
        thbuf.len(),
        cstr_ptr(c"th_port=0x%x"),
        libc::mach_thread_self() as c_uint,
    );
    libc::snprintf(
        stkbuf.as_mut_ptr(),
        stkbuf.len(),
        cstr_ptr(c"main_stack=0x%llx,0x%llx,0x%llx,0x%llx"),
        stack.wrapping_add(DYN_STACK_SIZE) as libc::c_ulonglong,
        DYN_STACK_SIZE as libc::c_ulonglong,
        0x4000u64 as libc::c_ulonglong,
        0x4000u64 as libc::c_ulonglong,
    );
    apple_g[0] = put_str(&mut sp, apple0.as_ptr());
    apple_g[1] = put_str(&mut sp, cstr_ptr(c"stack_guard=0x6f6365727a5f6700"));
    apple_g[2] = put_str(&mut sp, cstr_ptr(c"ptr_munge=0xa3f1c2b4d5e60718"));
    apple_g[3] = put_str(
        &mut sp,
        cstr_ptr(c"malloc_entropy=0x91827364a5b6c7d8,0x1f2e3d4c5b6a7988"),
    );
    apple_g[4] = put_str(&mut sp, stkbuf.as_ptr());
    apple_g[5] = put_str(&mut sp, thbuf.as_ptr());
    (*out).exec_path = put_str(&mut sp, path);
    let applec = 6u64;

    sp &= !0xf;
    let vec_bytes = ((argc as u64)
        .wrapping_add(1)
        .wrapping_add(envc as u64)
        .wrapping_add(1)
        .wrapping_add(applec)
        .wrapping_add(1))
    .wrapping_mul(8);
    let argv_arr = sp.wrapping_sub(vec_bytes) & !0xf;
    for i in 0..argc {
        crate::ported::dyldapi::hostmem::ocerz_st(
            argv_arr.wrapping_add((i as u64).wrapping_mul(8)),
            8,
            argv_g.add(i as usize).read(),
        );
    }
    crate::ported::dyldapi::hostmem::ocerz_st(
        argv_arr.wrapping_add((argc as u64).wrapping_mul(8)),
        8,
        0,
    );
    let envp_arr = argv_arr.wrapping_add((argc as u64).wrapping_add(1).wrapping_mul(8));
    for i in 0..envc {
        crate::ported::dyldapi::hostmem::ocerz_st(
            envp_arr.wrapping_add((i as u64).wrapping_mul(8)),
            8,
            envp_g.add(i as usize).read(),
        );
    }
    crate::ported::dyldapi::hostmem::ocerz_st(
        envp_arr.wrapping_add((envc as u64).wrapping_mul(8)),
        8,
        0,
    );
    let apple_arr = envp_arr.wrapping_add((envc as u64).wrapping_add(1).wrapping_mul(8));
    for i in 0..applec as usize {
        crate::ported::dyldapi::hostmem::ocerz_st(
            apple_arr.wrapping_add((i as u64).wrapping_mul(8)),
            8,
            apple_g[i],
        );
    }
    crate::ported::dyldapi::hostmem::ocerz_st(apple_arr.wrapping_add(applec.wrapping_mul(8)), 8, 0);

    let cells = argv_arr.wrapping_sub(8 * 8) & !0xf;
    let slash = libc::strrchr(*argv, b'/' as c_int);
    let leaf = argv_g.read().wrapping_add(if slash.is_null() {
        0
    } else {
        slash.offset_from(*argv) as u64 + 1
    });
    crate::ported::dyldapi::hostmem::ocerz_st(cells, 4, argc as u64);
    crate::ported::dyldapi::hostmem::ocerz_st(cells.wrapping_add(8), 8, argv_arr);
    crate::ported::dyldapi::hostmem::ocerz_st(cells.wrapping_add(16), 8, envp_arr);
    crate::ported::dyldapi::hostmem::ocerz_st(cells.wrapping_add(24), 8, leaf);
    let pv = cells.wrapping_sub(48);
    crate::ported::dyldapi::hostmem::ocerz_st(
        pv,
        8,
        if ocerz_main_mh != 0 {
            ocerz_main_mh
        } else {
            ffi::ocerz_arena_lo
        },
    );
    crate::ported::dyldapi::hostmem::ocerz_st(pv.wrapping_add(8), 8, cells);
    crate::ported::dyldapi::hostmem::ocerz_st(pv.wrapping_add(16), 8, cells.wrapping_add(8));
    crate::ported::dyldapi::hostmem::ocerz_st(pv.wrapping_add(24), 8, cells.wrapping_add(16));
    crate::ported::dyldapi::hostmem::ocerz_st(pv.wrapping_add(32), 8, cells.wrapping_add(24));

    (*out).argc = argc as u64;
    (*out).argv_arr = argv_arr;
    (*out).envp_arr = envp_arr;
    (*out).apple_arr = apple_arr;
    (*out).progvars = pv;
    libc::free(argv_g.cast());
    libc::free(envp_g.cast());
    ffi::OCERZ_OK
}

pub(super) unsafe fn find_dylib_init(cache: *mut OcerzCache, substr: *const c_char) -> u64 {
    for i in 0..(*cache).images_cnt {
        let mut path = ptr::null();
        let mh = ffi::ocerz_cache_image_addr(cache, i, &mut path);
        if path.is_null()
            || libc::strstr(path, substr).is_null()
            || rd32(mh as *const u8) != MH_MAGIC_64
        {
            continue;
        }
        let h = crate::ported::dyldapi::hostmem::ocerz_g2h(mh).cast::<u8>();
        let ncmds = rd32(h.add(16));
        let mut lc = h.add(core::mem::size_of::<MachHeader64>());
        for _ in 0..ncmds {
            if rd32(lc) == LC_SEGMENT_64 {
                let ns = rd32(lc.add(64));
                let mut sec = lc.add(72);
                for _ in 0..ns {
                    let kind = rd32(sec.add(64)) & 0xff;
                    let sa = rd64(sec.add(32));
                    let size = rd64(sec.add(40));
                    if kind == 0x16 && size >= 4 {
                        return mh.wrapping_add(rd32(sa as *const u8) as u64);
                    }
                    if kind == 0x09 && size >= 8 {
                        return rd64(sa as *const u8);
                    }
                    sec = sec.add(80);
                }
            }
            lc = lc.add(rd32(lc.add(4)) as usize);
        }
        return 0;
    }
    0
}

pub(super) unsafe fn progvars_point_at(
    fr: *const DynFrame,
    argc_at: u64,
    argv_at: u64,
    environ_at: u64,
    progname_at: u64,
) {
    let at = [argc_at, argv_at, environ_at, progname_at];
    for i in 0..4 {
        if at[i] != 0 {
            crate::ported::dyldapi::hostmem::ocerz_st(
                (*fr).progvars.wrapping_add(8).wrapping_add(8 * i as u64),
                8,
                at[i],
            );
        }
    }
}

pub(super) unsafe fn native_exit_through_libsystem(fr: *const DynFrame) {
    let libsys = dimg_find_by_install_name(cstr_ptr(c"/usr/lib/libSystem.B.dylib"));
    let mut found = 0;
    let exit_fn = if libsys.is_null() {
        0
    } else {
        super::exports::ocerz_image_self_resolve_ex(libsys, cstr_ptr(c"_exit"), &mut found)
    };
    if found == 0 || exit_fn == 0 {
        return;
    }
    let mut code = [
        0x89, 0xc3, 0x48, 0x83, 0xec, 0x08, 0x89, 0xc7, 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff,
        0xd0, 0x89, 0xdf, 0xb8, 0x01, 0x00, 0x00, 0x02, 0x0f, 0x05,
    ];
    ptr::copy_nonoverlapping(
        ptr::addr_of!(exit_fn).cast::<u8>(),
        code.as_mut_ptr().add(10),
        core::mem::size_of::<u64>(),
    );
    ptr::copy_nonoverlapping(
        code.as_ptr(),
        crate::ported::dyldapi::hostmem::ocerz_g2h((*fr).exit_stub).cast(),
        code.len(),
    );
}

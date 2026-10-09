//! Main-image setup, inserted libraries, and translated entry execution.

use super::*;

static mut G_RUN_CACHE_STORAGE: OcerzCache = unsafe { core::mem::zeroed() };
static mut G_INIT_ORDER: [u64; INIT_CLOSURE_CAP] = [0; INIT_CLOSURE_CAP];

unsafe extern "C" {
    fn _NSGetArgc() -> *mut c_int;
    fn _NSGetArgv() -> *mut *mut *mut c_char;
    fn _NSGetEnviron() -> *mut *mut *mut c_char;
    fn _NSGetProgname() -> *mut *mut c_char;
    fn ocerz_peek_dump(name: *const c_char);
}

unsafe fn load_inserted_libraries(vm: *mut OcerzVM) {
    let list = libc::getenv(cstr_ptr(c"DYLD_INSERT_LIBRARIES"));
    if list.is_null() || list.read() == 0 {
        return;
    }
    let copy = libc::strdup(list);
    let mut save = ptr::null_mut();
    let mut p = if copy.is_null() {
        ptr::null_mut()
    } else {
        libc::strtok_r(copy, cstr_ptr(c":"), &mut save)
    };
    while !p.is_null() && (*vm).exited == 0 {
        if super::dlopen::ocerz_dlopen(vm, p, 2 | 8) == 0 {
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(c"ocerz: DYLD_INSERT_LIBRARIES: could not load %s\n"),
                p,
            );
        }
        p = libc::strtok_r(ptr::null_mut(), cstr_ptr(c":"), &mut save);
    }
    libc::free(copy.cast());
}

unsafe fn wine_preload_skip_pinned(img: *mut DynImage) {
    let mut found = 0;
    let var = super::exports::ocerz_image_self_resolve_ex(
        img,
        cstr_ptr(c"_wine_main_preload_info"),
        &mut found,
    );
    let list = if found != 0 && var != 0 {
        crate::ported::dyldapi::hostmem::ocerz_ld(var, 8)
    } else {
        0
    };
    if list == 0 {
        return;
    }
    let mut parts = [0u64; 2 * 120];
    let mut n = 0;
    let mut cut = 0;
    for i in 0..16 {
        if n >= 120 {
            break;
        }
        let addr = crate::ported::dyldapi::hostmem::ocerz_ld(list + i * 16, 8);
        let size = crate::ported::dyldapi::hostmem::ocerz_ld(list + i * 16 + 8, 8);
        if size == 0 {
            break;
        }
        if ffi::ocerz_mem_pinned(addr, size) != 0 {
            cut = 1;
            n += ffi::ocerz_mem_unpinned_parts(
                addr,
                addr.wrapping_add(size),
                parts.as_mut_ptr().add((2 * n) as usize),
                120 - n,
            );
        } else {
            parts[(2 * n) as usize] = addr;
            parts[(2 * n + 1) as usize] = size;
            n += 1;
        }
    }
    let copy = if cut != 0 {
        ffi::ocerz_map_anywhere(
            (n as u64).wrapping_add(2).wrapping_mul(16),
            libc::PROT_READ | libc::PROT_WRITE,
        )
    } else {
        0
    };
    if copy == 0 {
        return;
    }
    for i in 0..n {
        crate::ported::dyldapi::hostmem::ocerz_st(
            copy + (i as u64).wrapping_mul(16),
            8,
            parts[(2 * i) as usize],
        );
        crate::ported::dyldapi::hostmem::ocerz_st(
            copy + (i as u64).wrapping_mul(16) + 8,
            8,
            parts[(2 * i + 1) as usize],
        );
    }
    crate::ported::dyldapi::hostmem::ocerz_st(var, 8, copy);
    crate::ocerz_log!(
        "dynamic: wine_main_preload_info now lists %d ranges that leave out host memory pinned below %#llx\n",
        n,
        0x0000_0003_0000_0000u64 as c_ulonglong
    );
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_dyld_run(
    vm: *mut OcerzVM,
    path_arg: *const c_char,
    argc: c_int,
    argv: *mut *mut c_char,
    envp: *mut *mut c_char,
) -> c_int {
    if ffi::ocerz_mem_init_identity(DYN_ARENA_SIZE) != ffi::OCERZ_OK {
        return ffi::OCERZ_ENOMEM;
    }
    let cache = ptr::addr_of_mut!(G_RUN_CACHE_STORAGE);
    if ffi::ocerz_mode == MODE_CACHE {
        if ffi::ocerz_cache_map(cache) != ffi::OCERZ_OK {
            crate::ocerz_fatal!("cannot map shared cache for dynamic loading\n");
            return ffi::OCERZ_EIO;
        }
    } else {
        crate::ocerz_log!("dynamic: native mode, shared cache not mapped\n");
        crate::ocerz_log!(
            "dynamic: native mode is Rosetta-independent: x86 code runs JIT-translated, system calls bridge to arm64\n"
        );
        crate::ocerz_log!(
            "dynamic: native JIT %s, bridge fastcall %s\n",
            if (*vm).jit_enabled != 0 {
                cstr_ptr(c"enabled")
            } else {
                cstr_ptr(c"disabled (-no-jit)")
            },
            if !libc::getenv(cstr_ptr(c"OCERZ_NO_BRIDGE_FASTCALL")).is_null() {
                cstr_ptr(c"disabled (trap path)")
            } else {
                cstr_ptr(c"enabled")
            }
        );
    }
    super::g_run_cache = cache;
    super::g_run_vm = vm;

    let mut flen = 0usize;
    let mut buf = super::read_file(path_arg, &mut flen);
    if buf.is_null() {
        crate::ocerz_fatal!("cannot read %s\n", path_arg);
        return ffi::OCERZ_EIO;
    }
    let slice = super::select_slice(buf, flen);
    if slice.is_null() {
        crate::ocerz_fatal!("%s has no x86_64 slice\n", path_arg);
        libc::free(buf.cast());
        return ffi::OCERZ_EFORMAT;
    }

    let mut abspath = [0 as c_char; libc::PATH_MAX as usize];
    let path = if libc::realpath(path_arg, abspath.as_mut_ptr()).is_null() {
        path_arg
    } else {
        abspath.as_ptr()
    };
    let mut img = DynImage::ZERO;
    img.slice = slice;
    libc::snprintf(img.path.as_mut_ptr(), img.path.len(), cstr_ptr(c"%s"), path);
    libc::snprintf(
        img.install_name.as_mut_ptr(),
        img.install_name.len(),
        cstr_ptr(c"%s"),
        path,
    );
    libc::snprintf(
        ptr::addr_of_mut!(super::g_main_hostpath).cast(),
        core::mem::size_of::<[c_char; 1024]>(),
        cstr_ptr(c"%s"),
        path,
    );
    super::file_identity(
        path,
        ptr::addr_of_mut!(super::g_main_dev),
        ptr::addr_of_mut!(super::g_main_ino),
    );
    let mut r = super::map::map_segments(&mut img, 1);
    if r != ffi::OCERZ_OK {
        crate::ocerz_fatal!("cannot map segments of %s\n", path);
        libc::free(buf.cast());
        return r;
    }
    if img.main_entry == 0 && img.thread_entry == 0 {
        crate::ocerz_fatal!("%s has no LC_MAIN or LC_UNIXTHREAD entry\n", path);
        libc::free(buf.cast());
        return ffi::OCERZ_EFORMAT;
    }

    if ffi::ocerz_mode == MODE_NATIVE {
        let minos = super::native::native_image_minos(slice);
        crate::ocerz_log!(
            "dynamic: %s declares macOS %u.%u.%u\n",
            path,
            minos >> 16,
            (minos >> 8) & 0xff,
            minos & 0xff
        );
        ffi::ocerz_apidb_set_minos(minos);
        ffi::ocerz_bridge_set_process_args(argc, argv);
    }

    let mut main_rpaths: super::RpathList = core::mem::zeroed();
    super::load::collect_rpaths(&mut img, ptr::null(), ptr::addr_of_mut!(main_rpaths));
    super::load::load_disk_deps(cache, &mut img, ptr::addr_of!(main_rpaths));

    r = super::bind::apply_fixups(&mut img, cache);
    if r != ffi::OCERZ_OK {
        libc::free(buf.cast());
        return r;
    }
    if img.cf_off == 0 {
        r = super::bind::apply_classic_fixups(&mut img, cache);
        if r != ffi::OCERZ_OK {
            libc::free(buf.cast());
            return r;
        }
    }
    super::map::protect_ro_segments(&mut img);

    if ffi::ocerz_mode == MODE_NATIVE && super::g_native_miss_n > 0 {
        for i in 0..super::g_native_miss_n {
            let miss = ptr::addr_of!(super::g_native_miss)
                .cast::<NativeMiss>()
                .add(i as usize);
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(c"ocerz: native: no bridge for %s in %s\n"),
                (*miss).sym.as_ptr(),
                (*miss).lib.as_ptr(),
            );
        }
        if super::g_native_miss_dropped != 0 {
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(c"ocerz: native: %d more unresolved imports not listed\n"),
                super::g_native_miss_dropped,
            );
        }
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"ocerz: native: %d unresolved imports, which no virtual library exports\n"),
            super::g_native_miss_n + super::g_native_miss_dropped,
        );
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"ocerz: native: to run it translated instead, supply an x86_64 build at $OCERZ_GUEST_ROOT%s (JIT, no Rosetta needed)\n"),
            cstr_ptr(c" or runtime/guest beside ocerz"),
        );
        libc::free(buf.cast());
        return 71;
    }
    if ffi::ocerz_mode == MODE_NATIVE {
        super::native::native_publish();
        super::tlv::native_tlv_register_loaded(img.load_base);
        let h = crate::ported::dyldapi::hostmem::ocerz_g2h(img.load_base).cast::<u8>();
        ffi::ocerz_objcbridge_fix_selrefs(h, img.slide as i64);
        ffi::ocerz_objcbridge_define_image(h, img.slide as i64);
        if !super::dimg_find_by_install_name(cstr_ptr(c"/usr/lib/libobjc.A.dylib")).is_null() {
            ffi::ocerz_objcbridge_install_uncaught();
        }
        super::map::protect_ro_flush();
    }

    let mut fr: DynFrame = core::mem::zeroed();
    if super::frame::build_frame(path, argc, argv, envp, &mut fr) != ffi::OCERZ_OK {
        crate::ocerz_fatal!("cannot build dynamic entry frame\n");
        libc::free(buf.cast());
        return ffi::OCERZ_ENOMEM;
    }
    if ffi::ocerz_mode == MODE_NATIVE {
        super::frame::native_exit_through_libsystem(&fr);
        ffi::ocerz_bridge_set_process_args(
            fr.argc as c_int,
            crate::ported::dyldapi::hostmem::ocerz_g2h(fr.argv_arr).cast(),
        );
    }

    let tsd = ffi::ocerz_map_anywhere(0x8000, libc::PROT_READ | libc::PROT_WRITE);
    if tsd == 0 {
        libc::free(buf.cast());
        return ffi::OCERZ_ENOMEM;
    }
    let self_addr = tsd + 0x4000;
    let gs = self_addr + 0xe0;
    (*vm).cpu.gs_base = gs;
    crate::ported::dyldapi::hostmem::ocerz_st(gs, 8, self_addr);
    let mut htid = 0u64;
    libc::pthread_threadid_np(0, &mut htid);
    crate::ported::dyldapi::hostmem::ocerz_st(gs - 8, 8, htid);

    if super::g_main_dimg_valid != 0 {
        libc::free(super::g_main_dimg.owned_buf.cast());
    }
    super::g_main_dimg = img;
    super::g_main_dimg.owned_buf = buf;
    super::g_main_dimg_valid = 1;
    buf = ptr::null_mut();
    libc::free(buf.cast());

    ffi::ocerz_vm_install_handlers(vm);
    if ffi::ocerz_mode == MODE_NATIVE {
        ffi::ocerz_fork_register();
    }
    ffi::ocerz_commpage_init();
    ocerz_peek_dump(cstr_ptr(c"cache-mapped"));
    if ffi::ocerz_mode == MODE_CACHE && ffi::ocerz_dyldapi_setup(cache) != ffi::OCERZ_OK {
        crate::ocerz_log!("dynamic: dyld API shim not installed\n");
    }

    let environ_addr = ffi::ocerz_cache_resolve(cache, cstr_ptr(c"_environ"));
    if environ_addr != 0 {
        crate::ported::dyldapi::hostmem::ocerz_st(environ_addr, 8, fr.envp_arr);
        crate::ocerz_log!(
            "dynamic: environ=%#llx set to %#llx\n",
            environ_addr as c_ulonglong,
            fr.envp_arr as c_ulonglong
        );
    }
    let mut progname_addr = ffi::ocerz_cache_resolve(cache, cstr_ptr(c"___progname"));
    if progname_addr == 0 {
        progname_addr = ffi::ocerz_cache_resolve(cache, cstr_ptr(c"__progname"));
    }
    if progname_addr != 0 && fr.argv_arr != 0 {
        let argv0 = crate::ported::dyldapi::hostmem::ocerz_ld(fr.argv_arr, 8);
        let mut leaf = argv0;
        let mut a = argv0;
        for _ in 0..4096 {
            if argv0 == 0 {
                break;
            }
            let c = crate::ported::dyldapi::hostmem::ocerz_ld(a, 1) as u8;
            if c == 0 {
                break;
            }
            if c == b'/' {
                leaf = a + 1;
            }
            a += 1;
        }
        if argv0 != 0 {
            crate::ported::dyldapi::hostmem::ocerz_st(progname_addr, 8, leaf);
        }
        crate::ocerz_log!(
            "dynamic: libdyld __progname=%#llx set to %#llx\n",
            progname_addr as c_ulonglong,
            leaf as c_ulonglong
        );
    }

    if ffi::ocerz_mode == MODE_CACHE {
        let argc_addr = ffi::ocerz_cache_resolve(cache, cstr_ptr(c"_NXArgc"));
        let argv_addr = ffi::ocerz_cache_resolve(cache, cstr_ptr(c"_NXArgv"));
        if argc_addr != 0 {
            crate::ported::dyldapi::hostmem::ocerz_st(argc_addr, 4, fr.argc);
        }
        if argv_addr != 0 {
            crate::ported::dyldapi::hostmem::ocerz_st(argv_addr, 8, fr.argv_arr);
        }
        super::frame::progvars_point_at(&fr, argc_addr, argv_addr, environ_addr, progname_addr);
    } else {
        super::frame::progvars_point_at(
            &fr,
            crate::ported::dyldapi::hostmem::ocerz_h2g(_NSGetArgc().cast()),
            crate::ported::dyldapi::hostmem::ocerz_h2g(_NSGetArgv().cast()),
            crate::ported::dyldapi::hostmem::ocerz_h2g(_NSGetEnviron().cast()),
            crate::ported::dyldapi::hostmem::ocerz_h2g(_NSGetProgname().cast()),
        );
    }

    if fr.exec_path != 0 {
        crate::ported::dyldapi::g_main_path = fr.exec_path;
    } else if fr.argv_arr != 0 {
        crate::ported::dyldapi::g_main_path =
            crate::ported::dyldapi::hostmem::ocerz_ld(fr.argv_arr, 8);
    }

    let mut ran_init = 0;
    let noinit = !libc::getenv(cstr_ptr(c"OCERZ_NOINIT")).is_null();
    let want_init =
        !noinit && (img.links_dylib != 0 || !libc::getenv(cstr_ptr(c"OCERZ_INIT")).is_null());
    if want_init {
        let ia = [fr.argc, fr.argv_arr, fr.envp_arr, fr.apple_arr, fr.progvars];
        ptr::copy_nonoverlapping(
            ia.as_ptr(),
            ptr::addr_of_mut!(super::g_run_init_args).cast::<u64>(),
            ia.len(),
        );
        let init = super::frame::find_dylib_init(cache, cstr_ptr(c"libSystem.B.dylib"));
        if init != 0 {
            crate::ocerz_log!(
                "dynamic: running libSystem_initializer at %#llx\n",
                init as c_ulonglong
            );
            ffi::ocerz_vm_call(vm, init, ia.as_ptr(), 5, fr.stack_top);
            if (*vm).exited != 0 {
                return (*vm).exit_code;
            }
            crate::ocerz_log!("dynamic: libSystem_initializer returned\n");
            ocerz_peek_dump(cstr_ptr(c"post-libSystem"));
            ran_init = 1;
        } else {
            crate::ocerz_log!("dynamic: no libSystem initializer found\n");
        }

        if ran_init != 0 {
            super::init::G_LIBSYS_MH =
                super::eager::dep_find(cache, cstr_ptr(c"/usr/lib/libSystem.B.dylib"));
            super::init::init_mark_done_closure(cache, super::init::G_LIBSYS_MH);
        }
        let want_phase = ran_init != 0
            && (img.links_cf != 0
                || super::eager::closure_links_cf(cache, img.load_base)
                || !libc::getenv(cstr_ptr(c"OCERZ_INITPHASE")).is_null())
            && libc::getenv(cstr_ptr(c"OCERZ_NOINITPHASE")).is_null();
        if want_phase {
            let libsys = super::init::G_LIBSYS_MH;
            super::eager::compute_eager_set(cache, img.load_base);
            crate::ocerz_log!(
                "dynamic: eager init set = %d images (of closure)\n",
                super::eager::G_EAGER_N
            );
            super::tlv::ocerz_tlv_register_closure(vm, cache, img.load_base, fr.stack_top);
            if (*vm).exited != 0 {
                return (*vm).exit_code;
            }
            super::init::G_INIT_CUR_GEN = super::init::G_INIT_CUR_GEN.wrapping_add(1);
            super::init::run_load_phase(vm, cache, libsys, fr.stack_top, 0);
            if (*vm).exited != 0 {
                return (*vm).exit_code;
            }
            super::init::G_INIT_CUR_GEN = super::init::G_INIT_CUR_GEN.wrapping_add(1);
            crate::ocerz_log!("dynamic: running dependency-ordered initializer phase\n");
            super::init::run_init_root(vm, cache, img.load_base, ia.as_ptr(), fr.stack_top, libsys);
            if (*vm).exited != 0 {
                return (*vm).exit_code;
            }
            crate::ocerz_log!("dynamic: initializer phase complete\n");
            ocerz_peek_dump(cstr_ptr(c"post-init-phase"));
        } else if ran_init != 0 && libc::getenv(cstr_ptr(c"OCERZ_NOINITPHASE")).is_null() {
            let order = ptr::addr_of_mut!(G_INIT_ORDER).cast::<u64>();
            let mut n = 0;
            super::init::init_collect(
                cache,
                img.load_base,
                order,
                &mut n,
                INIT_CLOSURE_CAP as c_int,
            );
            for i in 0..n {
                let idx = super::init::init_mark(order.add(i as usize).read());
                if idx >= 0 {
                    ptr::addr_of_mut!(super::init::G_INIT_BEING)
                        .cast::<u8>()
                        .add(idx as usize)
                        .write(0);
                }
            }
            for i in 0..n {
                if (*vm).exited != 0 {
                    break;
                }
                super::tlv::ocerz_tlv_register_image(
                    vm,
                    cache,
                    order.add(i as usize).read(),
                    fr.stack_top,
                );
            }
            if (*vm).exited != 0 {
                return (*vm).exit_code;
            }
            super::init::G_INIT_CUR_GEN = super::init::G_INIT_CUR_GEN.wrapping_add(1);
            crate::ocerz_log!(
                "dynamic: running dependency-ordered initializers for %d images\n",
                n
            );
            super::init::run_init_root(
                vm,
                cache,
                img.load_base,
                ia.as_ptr(),
                fr.stack_top,
                super::init::G_LIBSYS_MH,
            );
            if (*vm).exited != 0 {
                return (*vm).exit_code;
            }
        }
        if ran_init != 0 {
            super::g_run_init_ready = 1;
        }
        super::map::protect_ro_flush();
        if ran_init != 0 {
            load_inserted_libraries(vm);
            if (*vm).exited != 0 {
                return (*vm).exit_code;
            }
        }
    }

    if ffi::ocerz_mode == MODE_NATIVE {
        let nia = [fr.argc, fr.argv_arr, fr.envp_arr, fr.apple_arr, fr.progvars];
        ptr::copy_nonoverlapping(
            nia.as_ptr(),
            ptr::addr_of_mut!(super::native::g_native_init_args).cast::<u64>(),
            nia.len(),
        );
        let loaded = super::g_dimgs_n;
        ffi::ocerz_objcbridge_run_loads(vm, fr.stack_top);
        if (*vm).exited != 0 {
            return (*vm).exit_code;
        }
        let mut i = loaded - 1;
        while i >= 0 && (*vm).exited == 0 {
            let d = ptr::addr_of_mut!(super::g_dimgs)
                .cast::<DynImage>()
                .add(i as usize);
            super::init::run_image_inits(vm, (*d).load_base, nia.as_ptr(), fr.stack_top);
            i -= 1;
        }
        if (*vm).exited == 0 {
            super::init::run_image_inits(vm, img.load_base, nia.as_ptr(), fr.stack_top);
        }
        if (*vm).exited != 0 {
            return (*vm).exit_code;
        }
    }

    crate::ocerz_log!(
        "dynamic: load_base=%#llx slide=%#llx main=%#llx\n",
        img.load_base as c_ulonglong,
        img.slide as c_ulonglong,
        img.main_entry as c_ulonglong
    );
    if ffi::ocerz_mode == MODE_NATIVE && ffi::ocerz_low_base != 0 {
        wine_preload_skip_pinned(&mut img);
    }

    if !libc::getenv(cstr_ptr(c"OCERZ_ZONEPROBE")).is_null() {
        let g_malloc_zones = 0x7ff8_436b_4758u64;
        let g_malloc_num_zones = 0x7ff8_436b_49f0u64;
        let g_default_zone = 0x7ff8_4388_c178u64;
        let g_initial_nano = 0x7ff8_436b_47a0u64;
        let g_initial_scalable = 0x7ff8_436b_47a8u64;
        let nz = crate::ported::dyldapi::hostmem::ocerz_ld(g_malloc_num_zones, 4);
        let mzp = crate::ported::dyldapi::hostmem::ocerz_ld(g_malloc_zones, 8);
        let dz = crate::ported::dyldapi::hostmem::ocerz_ld(g_default_zone, 8);
        let z0 = if mzp != 0 {
            crate::ported::dyldapi::hostmem::ocerz_ld(mzp, 8)
        } else {
            0
        };
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"ZONEPROBE num_zones=%llu malloc_zones=%#llx zones[0]=%#llx default_zone=%#llx initial_nano=%#llx initial_scalable=%#llx\n"),
            nz as c_ulonglong,
            mzp as c_ulonglong,
            z0 as c_ulonglong,
            dz as c_ulonglong,
            g_initial_nano as c_ulonglong,
            g_initial_scalable as c_ulonglong,
        );
        for i in 0..nz.min(6) {
            let z = crate::ported::dyldapi::hostmem::ocerz_ld(mzp + i * 8, 8);
            let zmalloc = if z != 0 {
                crate::ported::dyldapi::hostmem::ocerz_ld(z + 0x18, 8)
            } else {
                0
            };
            let znameptr = if z != 0 {
                crate::ported::dyldapi::hostmem::ocerz_ld(z + 0x20, 8)
            } else {
                0
            };
            let mut nm = [0 as c_char; 64];
            if znameptr != 0 {
                for k in 0..63 {
                    let c = crate::ported::dyldapi::hostmem::ocerz_ld(znameptr + k, 1);
                    if c == 0 {
                        break;
                    }
                    nm[k as usize] = c as c_char;
                }
            }
            libc::fprintf(
                crate::log::stderr(),
                cstr_ptr(
                    c"ZONEPROBE  zones[%llu]=%#llx malloc=%#llx name@+0x20=%#llx name=\"%s\"\n",
                ),
                i as c_ulonglong,
                z as c_ulonglong,
                zmalloc as c_ulonglong,
                znameptr as c_ulonglong,
                nm.as_ptr(),
            );
        }
        let cfz = 0x7ff8_4009_5a00u64;
        let cmpg = 0x7ff8_436c_2b20u64;
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"ZONEPROBE CFzone@%#llx: isa/[0]=%#llx [+0x18 malloc]=%#llx [+0x68 ver]=%#llx [+0xd0]=%#llx [+0xb0]=%#llx\n"),
            cfz as c_ulonglong,
            crate::ported::dyldapi::hostmem::ocerz_ld(cfz, 8) as c_ulonglong,
            crate::ported::dyldapi::hostmem::ocerz_ld(cfz + 0x18, 8) as c_ulonglong,
            crate::ported::dyldapi::hostmem::ocerz_ld(cfz + 0x68, 8) as c_ulonglong,
            crate::ported::dyldapi::hostmem::ocerz_ld(cfz + 0xd0, 8) as c_ulonglong,
            crate::ported::dyldapi::hostmem::ocerz_ld(cfz + 0xb0, 8) as c_ulonglong,
        );
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"ZONEPROBE cmpglobal@%#llx=%#llx  (CFzone[0]==cmpglobal? %d)\n"),
            cmpg as c_ulonglong,
            crate::ported::dyldapi::hostmem::ocerz_ld(cmpg, 8) as c_ulonglong,
            (crate::ported::dyldapi::hostmem::ocerz_ld(cfz, 8)
                == crate::ported::dyldapi::hostmem::ocerz_ld(cmpg, 8)) as c_int,
        );
        let cfa = 0x7ff8_4009_64a8u64;
        libc::fprintf(
            crate::log::stderr(),
            cstr_ptr(c"ZONEPROBE kCFAllocatorSystemDefault@%#llx -> %#llx\n"),
            cfa as c_ulonglong,
            crate::ported::dyldapi::hostmem::ocerz_ld(cfa, 8) as c_ulonglong,
        );
    }

    if img.main_entry == 0 {
        let mut vec_end = fr.apple_arr;
        while crate::ported::dyldapi::hostmem::ocerz_ld(vec_end, 8) != 0 {
            vec_end += 8;
        }
        vec_end += 8;
        let vec_len = vec_end - fr.argv_arr;
        let sp = fr.stack_top.wrapping_sub(vec_len).wrapping_sub(8) & !0xf;
        crate::ported::dyldapi::hostmem::ocerz_st(sp, 8, fr.argc);
        ptr::copy_nonoverlapping(
            crate::ported::dyldapi::hostmem::ocerz_g2h(fr.argv_arr).cast::<u8>(),
            crate::ported::dyldapi::hostmem::ocerz_g2h(sp + 8).cast::<u8>(),
            vec_len as usize,
        );
        (*vm).cpu.gpr[ffi::OCERZ_RSP as usize] = sp;
        (*vm).cpu.rip = img.thread_entry;
        return ffi::ocerz_vm_run(vm);
    }

    if ran_init != 0 {
        let margs = [fr.argc, fr.argv_arr, fr.envp_arr, fr.apple_arr];
        let rv = ffi::ocerz_vm_call(vm, img.main_entry, margs.as_ptr(), 4, fr.stack_top);
        if (*vm).exited != 0 {
            return (*vm).exit_code;
        }
        let exit_fn = ffi::ocerz_cache_resolve(cache, cstr_ptr(c"_exit"));
        if exit_fn != 0 {
            let ea = [rv & 0xff];
            ffi::ocerz_vm_call(vm, exit_fn, ea.as_ptr(), 1, fr.stack_top);
            if (*vm).exited != 0 {
                return (*vm).exit_code;
            }
        }
        return (rv & 0xff) as c_int;
    }

    (*vm).cpu.gpr[ffi::OCERZ_RDI as usize] = fr.argc;
    (*vm).cpu.gpr[ffi::OCERZ_RSI as usize] = fr.argv_arr;
    (*vm).cpu.gpr[ffi::OCERZ_RDX as usize] = fr.envp_arr;
    (*vm).cpu.gpr[ffi::OCERZ_RCX as usize] = fr.apple_arr;
    (*vm).cpu.gpr[ffi::OCERZ_R8 as usize] = fr.progvars;
    let rsp = (fr.stack_top & !0xf).wrapping_sub(8);
    crate::ported::dyldapi::hostmem::ocerz_st(rsp, 8, fr.exit_stub);
    (*vm).cpu.gpr[ffi::OCERZ_RSP as usize] = rsp;
    (*vm).cpu.rip = img.main_entry;
    ffi::ocerz_vm_run(vm)
}

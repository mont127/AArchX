//! Guest memory syscall implementations, including file-backed mappings,
//! shared mappings, translation invalidation, and native memory entry points.

use super::util::*;
use super::*;

use core::ffi::{c_char, c_int};
use core::ptr;

const OCERZ_GUEST_PAGE_SIZE: u64 = 0x1000;
const OCERZ_HOST_PAGE_SIZE: u64 = 0x4000;

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct SharedRo {
    lo: u64,
    hi: u64,
}

static mut G_SHARED_RO: [SharedRo; 64] = [SharedRo { lo: 0, hi: 0 }; 64];
static mut G_N_SHARED_RO: c_int = 0;
pub(super) static mut g_pagetrap_lo: u64 = !0;
pub(super) static mut g_pagetrap_hi: u64 = 0;

unsafe extern "C" {
    fn pthread_main_np() -> c_int;
    fn ocerz_guestprof_final();
    fn ocerz_jit_tcache_final();
}

#[inline(always)]
unsafe fn sys_exit(vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    unsafe {
        let args = &*a;
        if !libc::getenv(c"OCERZ_EXITLOG".as_ptr()).is_null() {
            let cmdline =
                ptr::addr_of!(crate::ported::globals::ocerz_cmdline_summary).cast::<c_char>();
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: EXITLOG[%d \"%s\"] code=%d rip=%#llx ret-chain:".as_ptr(),
                libc::getpid(),
                cmdline,
                args[0] as u32 as c_int,
                (*cpu).rip as libc::c_ulonglong,
            );
            let mut fp = (*cpu).gpr[crate::ffi::OCERZ_RBP as usize];
            let mut d = 0;
            while d < 8 && fp >= crate::ffi::ocerz_arena_lo && fp < crate::ffi::ocerz_arena_hi {
                libc::fprintf(
                    crate::log::stderr(),
                    c" %#llx".as_ptr(),
                    ocerz_ld(fp.wrapping_add(8), 8) as libc::c_ulonglong,
                );
                fp = ocerz_ld(fp, 8);
                d += 1;
            }
            libc::fprintf(crate::log::stderr(), c"\n".as_ptr());

            let mut history = [0u64; 32];
            let n = crate::ffi::ocerz_vm_riphist(history.as_mut_ptr(), 32);
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: EXIT-BLOCKHIST[%d]".as_ptr(),
                libc::getpid(),
            );
            for rip in history.iter().take(n as usize) {
                libc::fprintf(
                    crate::log::stderr(),
                    c" %#llx".as_ptr(),
                    *rip as libc::c_ulonglong,
                );
            }
            libc::fprintf(crate::log::stderr(), c"\n".as_ptr());

            let eh = libc::getenv(c"OCERZ_EXITHIST".as_ptr());
            if !eh.is_null() && (args[0] as u32 != 0 || libc::strcmp(eh, c"all".as_ptr()) == 0) {
                let mut rh = [0u64; 32];
                let rn = crate::ffi::ocerz_vm_riphist(rh.as_mut_ptr(), 32);
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: EXITHIST[%d] riphist:".as_ptr(),
                    libc::getpid(),
                );
                for rip in rh.iter().take(rn as usize) {
                    libc::fprintf(
                        crate::log::stderr(),
                        c" %#llx".as_ptr(),
                        *rip as libc::c_ulonglong,
                    );
                }
                libc::fprintf(
                    crate::log::stderr(),
                    c"\nocerz: EXITHIST[%d] stackscan:".as_ptr(),
                    libc::getpid(),
                );
                let sp = (*cpu).gpr[crate::ffi::OCERZ_RSP as usize];
                let mut printed = 0;
                let mut off = 0u64;
                while off < 0x3000 && printed < 48 {
                    if crate::ffi::ocerz_addr_readable(sp.wrapping_add(off)) == 0 {
                        break;
                    }
                    let v = ocerz_ld(sp.wrapping_add(off), 8);
                    let codey = (0x7ff800000000..0x7ffb00000000).contains(&v)
                        || (0x700000000000..0x710000000000).contains(&v)
                        || (0x6fff00000000..0x700000000000).contains(&v)
                        || (0x140000000..0x180000000).contains(&v)
                        || (0x7ff000000000..0x7ff100000000).contains(&v);
                    if codey {
                        libc::fprintf(
                            crate::log::stderr(),
                            c" +%llx:%#llx".as_ptr(),
                            off as libc::c_ulonglong,
                            v as libc::c_ulonglong,
                        );
                        printed += 1;
                    }
                    off += 8;
                }
                libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
            }
        }
        crate::ffi::ocerz_vm_request_exit(vm, (args[0] as u32 as c_int) & 0xff);
        if pthread_main_np() == 0 {
            ocerz_guestprof_final();
            ocerz_jit_tcache_final();
            libc::fflush(crate::log::stderr());
            libc::_exit((args[0] as u32 as c_int) & 0xff);
        }
        crate::ffi::OCERZ_STEP_EXIT as c_int
    }
}

unsafe fn sys_unsupported(_vm: *mut OcerzVM, _cpu: *mut OcerzCPU, _a: *mut [u64; 8]) -> c_int {
    crate::ffi::OCERZ_STEP_FATAL as c_int
}

unsafe fn sys_abort_payload(vm: *mut OcerzVM, _cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    unsafe {
        let args = &*a;
        crate::ocerz_log!(
            "guest abort_with_payload: reason_namespace=%llu reason_code=%llu\n",
            args[0] as libc::c_ulonglong,
            args[1] as libc::c_ulonglong
        );
        crate::ffi::ocerz_vm_request_exit(vm, 134);
        if pthread_main_np() == 0 {
            libc::fflush(crate::log::stderr());
            libc::_exit(134);
        }
        crate::ffi::OCERZ_STEP_EXIT as c_int
    }
}

unsafe fn memtrace(op: *const c_char, addr: u64, len: u64, prot: c_int, flags: c_int) {
    static ON: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
    static ALL: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
    unsafe {
        let mut on = ON.load(core::sync::atomic::Ordering::Relaxed);
        if on < 0 {
            on = c_int::from(!libc::getenv(c"OCERZ_MEMTRACE".as_ptr()).is_null());
            ON.store(on, core::sync::atomic::Ordering::Relaxed);
        }
        let mut all = ALL.load(core::sync::atomic::Ordering::Relaxed);
        if all < 0 {
            all = c_int::from(!libc::getenv(c"OCERZ_MEMTRACE_ALL".as_ptr()).is_null());
            ALL.store(all, core::sync::atomic::Ordering::Relaxed);
        }
        if on != 0 && (all != 0 || (0x7ff00000..0x80000000).contains(&addr)) {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: MEM %s addr=%#llx len=%#llx prot=%#x flags=%#x comm=%d\n".as_ptr(),
                op,
                addr as libc::c_ulonglong,
                len as libc::c_ulonglong,
                prot,
                flags,
                crate::ffi::ocerz_addr_committed(addr),
            );
        }
    }
}

unsafe fn image_clobber_check(
    op: *const c_char,
    cpu: *mut OcerzCPU,
    addr: u64,
    len: u64,
    exec_only: c_int,
) {
    unsafe {
        if len == 0 || addr.wrapping_add(len) < addr {
            return;
        }
        let mut base = 0u64;
        let img = crate::ffi::ocerz_dyld_image_overlapping(
            addr,
            addr.wrapping_add(len),
            exec_only,
            &mut base,
        );
        if !img.is_null() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: IMAGE-CLOBBER[%d] guest %s addr=%#llx len=%#llx lands on %s (base %#llx) rip=%#llx\n".as_ptr(),
                libc::getpid(),
                op,
                addr as libc::c_ulonglong,
                len as libc::c_ulonglong,
                img,
                base as libc::c_ulonglong,
                (*cpu).rip as libc::c_ulonglong,
            );
        }
    }
}

pub(super) unsafe fn invalidate_guest_mapping(vm: *mut OcerzVM, addr: u64, len: u64) {
    unsafe { crate::ffi::ocerz_jit_invalidate_range(vm, addr, len) }
}

unsafe fn readonly_shared_subpage(addr: u64, len: u64, prot: c_int, flags: c_int) -> c_int {
    c_int::from(
        flags & libc::MAP_SHARED != 0
            && prot & libc::PROT_WRITE == 0
            && (addr | len) & (OCERZ_HOST_PAGE_SIZE - 1) != 0,
    )
}

unsafe fn wine_kuser_shared_page(
    addr: u64,
    len: u64,
    prot: c_int,
    flags: c_int,
    pos: u64,
) -> c_int {
    c_int::from(
        addr == 0x7ffe0000
            && len == OCERZ_GUEST_PAGE_SIZE
            && pos == 0
            && flags & (libc::MAP_FIXED | libc::MAP_SHARED) == (libc::MAP_FIXED | libc::MAP_SHARED)
            && prot & libc::PROT_WRITE == 0,
    )
}

unsafe fn shared_ro_record(gaddr: u64, len: u64) {
    unsafe {
        if G_N_SHARED_RO < 64 {
            let slot = ptr::addr_of_mut!(G_SHARED_RO)
                .cast::<SharedRo>()
                .add(G_N_SHARED_RO as usize);
            (*slot).lo = gaddr;
            (*slot).hi = gaddr.wrapping_add(len);
            G_N_SHARED_RO += 1;
        } else {
            let first = ptr::addr_of_mut!(G_SHARED_RO).cast::<SharedRo>();
            (*first).lo = 0;
            (*first).hi = !0;
        }
    }
}

unsafe fn shared_ro_overlaps(gaddr: u64, len: u64) -> c_int {
    unsafe {
        for i in 0..G_N_SHARED_RO as usize {
            let slot = *ptr::addr_of!(G_SHARED_RO).cast::<SharedRo>().add(i);
            if gaddr < slot.hi && gaddr.wrapping_add(len) > slot.lo {
                return 1;
            }
        }
        0
    }
}

unsafe fn mmap_fail_log(
    cpu: *mut OcerzCPU,
    addr: u64,
    len: u64,
    prot: c_int,
    flags: c_int,
    fd: c_int,
    pos: u64,
) {
    static LOG: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
    unsafe {
        let mut on = LOG.load(core::sync::atomic::Ordering::Relaxed);
        if on < 0 {
            on = c_int::from(!libc::getenv(c"OCERZ_MAPFAILLOG".as_ptr()).is_null());
            LOG.store(on, core::sync::atomic::Ordering::Relaxed);
        }
        if on != 0 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: MMAPFAIL[%d] addr=%#llx len=%#llx prot=%#x flags=%#x fd=%d off=%#llx rip=%#llx\n".as_ptr(),
                libc::getpid(),
                addr as libc::c_ulonglong,
                len as libc::c_ulonglong,
                prot,
                flags,
                fd,
                pos as libc::c_ulonglong,
                (*cpu).rip as libc::c_ulonglong,
            );
        }
    }
}

unsafe fn pread_range(fd: c_int, gaddr: u64, mut lo: u64, hi: u64, pos: u64) -> c_int {
    unsafe {
        while lo < hi {
            let n = libc::pread(
                fd,
                ocerz_g2h(lo),
                hi.wrapping_sub(lo) as usize,
                pos.wrapping_add(lo.wrapping_sub(gaddr)) as libc::off_t,
            );
            if n < 0 {
                if *libc::__error() == libc::EINTR {
                    continue;
                }
                return *libc::__error();
            }
            if n == 0 {
                break;
            }
            lo = lo.wrapping_add(n as u64);
        }
        0
    }
}

unsafe fn private_file_backing(gaddr: u64, len: u64, fd: c_int, pos: u64) -> c_int {
    static OFF: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
    unsafe {
        let mut off = OFF.load(core::sync::atomic::Ordering::Relaxed);
        if off < 0 {
            off = c_int::from(!libc::getenv(c"OCERZ_NO_FILEMAP".as_ptr()).is_null());
            OFF.store(off, core::sync::atomic::Ordering::Relaxed);
        }
        let lo = gaddr;
        let hi = gaddr.wrapping_add(len);
        let ilo = lo.wrapping_add(0x3fff) & !0x3fff;
        let ihi = hi & !0x3fff;
        let mut st = core::mem::MaybeUninit::<libc::stat>::uninit();
        if off == 0
            && pos.wrapping_sub(gaddr) & 0x3fff == 0
            && ihi > ilo
            && libc::fstat(fd, st.as_mut_ptr()) == 0
        {
            let st = st.assume_init();
            if (st.st_mode & libc::S_IFMT) == libc::S_IFREG && (st.st_size as u64) > pos {
                let file_hi = gaddr.wrapping_add((st.st_size as u64).wrapping_sub(pos));
                let mut mhi = file_hi.wrapping_add(0x3fff) & !0x3fff;
                if mhi > ihi {
                    mhi = ihi;
                }
                if mhi > ilo {
                    let hp = ocerz_g2h(ilo);
                    if libc::mmap(
                        hp,
                        mhi.wrapping_sub(ilo) as usize,
                        libc::PROT_READ | libc::PROT_WRITE,
                        libc::MAP_PRIVATE | libc::MAP_FIXED,
                        fd,
                        pos.wrapping_add(ilo.wrapping_sub(lo)) as libc::off_t,
                    ) == hp
                    {
                        if !libc::getenv(c"OCERZ_MEMTRACE".as_ptr()).is_null() {
                            libc::fprintf(
                                crate::log::stderr(),
                                c"ocerz: FILEMAP-DIRECT gaddr=%#llx interior=[%#llx,%#llx) of len=%#llx\n".as_ptr(),
                                gaddr as libc::c_ulonglong,
                                ilo as libc::c_ulonglong,
                                mhi as libc::c_ulonglong,
                                len as libc::c_ulonglong,
                            );
                        }
                        let mut err = pread_range(fd, gaddr, lo, ilo, pos);
                        if err == 0 {
                            err = pread_range(fd, gaddr, mhi, hi, pos);
                        }
                        return err;
                    }
                }
            }
        }
        pread_range(fd, gaddr, lo, hi, pos)
    }
}

unsafe fn guest_mmap_apply(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    addr: u64,
    len: u64,
    prot: c_int,
    flags: c_int,
    fd: c_int,
    pos: u64,
    out: *mut u64,
) -> c_int {
    unsafe {
        let anon = flags & libc::MAP_ANON != 0 || fd < 0;
        let fixed = flags & libc::MAP_FIXED != 0;
        let gaddr;
        if flags & libc::MAP_SHARED != 0 && prot & libc::PROT_WRITE != 0 {
            crate::ffi::ocerz_jit_require_ordered(vm);
        }
        memtrace(
            if anon {
                c"mmap-anon".as_ptr()
            } else {
                c"mmap-file".as_ptr()
            },
            addr,
            len,
            prot,
            flags,
        );
        if fixed {
            image_clobber_check(
                if anon {
                    c"mmap-fixed-anon".as_ptr()
                } else {
                    c"mmap-fixed-file".as_ptr()
                },
                cpu,
                addr,
                len,
                0,
            );
        }
        if anon {
            if fixed {
                invalidate_guest_mapping(vm, addr, len);
                let mut rc = crate::ffi::ocerz_map_fixed(addr, len, prot);
                if rc != crate::ffi::OCERZ_OK
                    && crate::ffi::ocerz_mem_register_range(addr, addr.wrapping_add(len))
                        == crate::ffi::OCERZ_OK
                {
                    rc = crate::ffi::ocerz_map_fixed(addr, len, prot);
                }
                if rc != crate::ffi::OCERZ_OK {
                    mmap_fail_log(cpu, addr, len, prot, flags, fd, pos);
                    return OCERZ_ENOMEM_V;
                }
                if flags & libc::MAP_SHARED != 0
                    && crate::ffi::ocerz_map_shared_anon(addr, len, prot) != crate::ffi::OCERZ_OK
                {
                    crate::ffi::ocerz_unmap(addr, len);
                    mmap_fail_log(cpu, addr, len, prot, flags, fd, pos);
                    return OCERZ_ENOMEM_V;
                }
                if prot == libc::PROT_NONE
                    && addr <= 0x10000
                    && len >= 0x100000000u64.wrapping_sub(addr)
                {
                    crate::ffi::ocerz_init_gate_release();
                }
                *out = addr;
                return 0;
            }
            let mut gaddr = 0;
            if addr != 0
                && libc::getenv(c"OCERZ_NO_MMAP_HINT".as_ptr()).is_null()
                && crate::ffi::ocerz_map_hint(addr, len, prot) == crate::ffi::OCERZ_OK
            {
                gaddr = addr & !0x3fff;
            }
            if gaddr == 0 {
                gaddr = crate::ffi::ocerz_map_anywhere(len, prot);
            }
            if gaddr == 0 {
                mmap_fail_log(cpu, addr, len, prot, flags, fd, pos);
                return OCERZ_ENOMEM_V;
            }
            invalidate_guest_mapping(vm, gaddr, len);
            if flags & libc::MAP_SHARED != 0
                && crate::ffi::ocerz_map_shared_anon(gaddr, len, prot) != crate::ffi::OCERZ_OK
            {
                crate::ffi::ocerz_unmap(gaddr, len);
                mmap_fail_log(cpu, addr, len, prot, flags, fd, pos);
                return OCERZ_ENOMEM_V;
            }
            *out = gaddr;
            return 0;
        }
        if fixed {
            invalidate_guest_mapping(vm, addr, len);
            let mut rc = crate::ffi::ocerz_map_fixed(addr, len, libc::PROT_READ | libc::PROT_WRITE);
            if rc != crate::ffi::OCERZ_OK
                && crate::ffi::ocerz_mem_register_range(addr, addr.wrapping_add(len))
                    == crate::ffi::OCERZ_OK
            {
                rc = crate::ffi::ocerz_map_fixed(addr, len, libc::PROT_READ | libc::PROT_WRITE);
            }
            if rc != crate::ffi::OCERZ_OK {
                mmap_fail_log(cpu, addr, len, prot, flags, fd, pos);
                return OCERZ_ENOMEM_V;
            }
            gaddr = addr;
        } else {
            gaddr = crate::ffi::ocerz_map_anywhere(len, libc::PROT_READ | libc::PROT_WRITE);
            if gaddr == 0 {
                mmap_fail_log(cpu, addr, len, prot, flags, fd, pos);
                return OCERZ_ENOMEM_V;
            }
            invalidate_guest_mapping(vm, gaddr, len);
        }
        if !libc::getenv(c"OCERZ_MEMTRACE".as_ptr()).is_null() {
            let mut path = [0 as c_char; 256];
            libc::fcntl(fd, libc::F_GETPATH, path.as_mut_ptr());
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: FILEMAP gaddr=%#llx len=%#llx prot=%#x SHARED=%d fd=%d off=%#llx path=%s\n".as_ptr(),
                gaddr as libc::c_ulonglong,
                len as libc::c_ulonglong,
                prot,
                c_int::from(flags & libc::MAP_SHARED != 0),
                fd,
                pos as libc::c_ulonglong,
                path.as_ptr(),
            );
        }
        if flags & libc::MAP_SHARED != 0 {
            let padded_kuser = wine_kuser_shared_page(gaddr, len, prot, flags, pos) != 0;
            let src = if padded_kuser {
                crate::ffi::ocerz_map_shared_file_padded(gaddr, len, prot, fd, pos)
            } else {
                crate::ffi::ocerz_map_shared_file(gaddr, len, prot, fd, pos)
            };
            let sharedlog = !libc::getenv(c"OCERZ_MEMTRACE".as_ptr()).is_null()
                || !libc::getenv(c"OCERZ_SHAREDLOG".as_ptr()).is_null();
            if sharedlog {
                let mut path = [0 as c_char; 256];
                libc::fcntl(fd, libc::F_GETPATH, path.as_mut_ptr());
                let result = if src == crate::ffi::OCERZ_OK {
                    if padded_kuser {
                        c"shared-padded".as_ptr()
                    } else {
                        c"shared-ok".as_ptr()
                    }
                } else if prot & libc::PROT_WRITE != 0 {
                    c"FAILED(writable-shared)".as_ptr()
                } else {
                    c"private-copy(readonly)".as_ptr()
                };
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: SHAREDMAP[%d] gaddr=%#llx len=%#llx prot=%#x off=%#llx -> %s (src=%d) %s\n".as_ptr(),
                    libc::getpid(),
                    gaddr as libc::c_ulonglong,
                    len as libc::c_ulonglong,
                    prot,
                    pos as libc::c_ulonglong,
                    result,
                    src,
                    path.as_ptr(),
                );
            }
            if src == crate::ffi::OCERZ_OK {
                if prot & libc::PROT_WRITE == 0 {
                    shared_ro_record(gaddr, len);
                }
                if padded_kuser {
                    crate::ffi::ocerz_jit_require_ordered(vm);
                }
                *out = gaddr;
                return 0;
            }
            if padded_kuser {
                crate::ffi::ocerz_unmap(gaddr, len);
                mmap_fail_log(cpu, addr, len, prot, flags, fd, pos);
                return if src == crate::ffi::OCERZ_EUNSUP {
                    libc::EINVAL
                } else {
                    OCERZ_ENOMEM_V
                };
            }
            if prot & libc::PROT_WRITE != 0 {
                crate::ffi::ocerz_unmap(gaddr, len);
                mmap_fail_log(cpu, addr, len, prot, flags, fd, pos);
                return if src == crate::ffi::OCERZ_EUNSUP {
                    libc::EINVAL
                } else {
                    OCERZ_ENOMEM_V
                };
            }
        }
        let err = private_file_backing(gaddr, len, fd, pos);
        if err != 0 {
            crate::ffi::ocerz_unmap(gaddr, len);
            return err;
        }
        if crate::ffi::ocerz_protect(gaddr, len, prot) != crate::ffi::OCERZ_OK {
            crate::ffi::ocerz_unmap(gaddr, len);
            mmap_fail_log(cpu, addr, len, prot, flags, fd, pos);
            return OCERZ_ENOMEM_V;
        }
        *out = gaddr;
        0
    }
}

unsafe fn sys_mmap(vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    unsafe {
        let args = &*a;
        let mut gaddr = 0;
        let err = guest_mmap_apply(
            vm,
            cpu,
            args[0],
            args[1],
            args[2] as c_int,
            args[3] as c_int,
            args[4] as u32 as c_int,
            args[5],
            &mut gaddr,
        );
        if err != 0 {
            ret_err(cpu, err as u64);
        } else {
            ret_ok(cpu, gaddr);
        }
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

unsafe fn sys_munmap(vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    unsafe {
        let args = &*a;
        image_clobber_check(c"munmap".as_ptr(), cpu, args[0], args[1], 0);
        invalidate_guest_mapping(vm, args[0], args[1]);
        crate::ffi::ocerz_unmap(args[0], args[1]);
        ret_ok(cpu, 0);
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

unsafe fn guest_mprotect_apply(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    addr: u64,
    len: u64,
    prot: c_int,
) -> c_int {
    unsafe {
        memtrace(c"mprotect".as_ptr(), addr, len, prot, 0);
        if prot & (libc::PROT_READ | libc::PROT_EXEC) != libc::PROT_READ | libc::PROT_EXEC {
            image_clobber_check(
                if prot & libc::PROT_READ != 0 {
                    c"mprotect-noexec".as_ptr()
                } else {
                    c"mprotect-noread".as_ptr()
                },
                cpu,
                addr,
                len,
                1,
            );
        }
        if crate::ffi::ocerz_cache_region(addr as usize) != 0 {
            let err = crate::ffi::ocerz_cache_protect(addr as usize, len, prot);
            if err != 0 {
                return err;
            }
            if prot & libc::PROT_WRITE != 0 {
                invalidate_guest_mapping(vm, addr, len);
            }
            return 0;
        }
        if prot & libc::PROT_WRITE != 0 && shared_ro_overlaps(addr, len) != 0 {
            crate::ffi::ocerz_jit_require_ordered(vm);
        }
        invalidate_guest_mapping(vm, addr, len);
        let rc = crate::ffi::ocerz_protect(addr, len, prot);
        if rc != crate::ffi::OCERZ_OK && !libc::getenv(c"OCERZ_MAPFAILLOG".as_ptr()).is_null() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: PROTFAIL[%d] addr=%#llx len=%#llx prot=%#x rc=%d rip=%#llx\n".as_ptr(),
                libc::getpid(),
                addr as libc::c_ulonglong,
                len as libc::c_ulonglong,
                prot as libc::c_uint,
                rc,
                (*cpu).rip as libc::c_ulonglong,
            );
        }
        if rc == crate::ffi::OCERZ_OK {
            0
        } else if rc == crate::ffi::OCERZ_EUNSUP {
            libc::EINVAL
        } else {
            OCERZ_ENOMEM_V
        }
    }
}

unsafe fn sys_mprotect(vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    unsafe {
        let args = &*a;
        let err = guest_mprotect_apply(vm, cpu, args[0], args[1], args[2] as c_int);
        if err != 0 {
            ret_err(cpu, err as u64);
        } else {
            ret_ok(cpu, 0);
        }
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

unsafe fn sys_madvise(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, _a: *mut [u64; 8]) -> c_int {
    ret_ok(cpu, 0);
    crate::ffi::OCERZ_STEP_OK as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_mmap(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    addr: u64,
    len: u64,
    prot: c_int,
    flags: c_int,
    fd: c_int,
    off: u64,
    out: *mut u64,
) -> c_int {
    unsafe { guest_mmap_apply(vm, cpu, addr, len, prot, flags, fd, off, out) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_munmap(
    vm: *mut OcerzVM,
    _cpu: *mut OcerzCPU,
    addr: u64,
    len: u64,
) -> c_int {
    unsafe {
        invalidate_guest_mapping(vm, addr, len);
        if crate::ffi::ocerz_mem_overlaps(addr, len) == 0 {
            return if libc::munmap(ocerz_g2h(addr), len as usize) == 0 {
                0
            } else {
                *libc::__error()
            };
        }
        crate::ffi::ocerz_unmap(addr, len);
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_mprotect(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    addr: u64,
    len: u64,
    prot: c_int,
) -> c_int {
    unsafe {
        if crate::ffi::ocerz_mem_overlaps(addr, len) != 0 {
            return guest_mprotect_apply(vm, cpu, addr, len, prot);
        }
        invalidate_guest_mapping(vm, addr, len);
        if libc::mprotect(ocerz_g2h(addr), len as usize, prot) == 0 {
            0
        } else {
            *libc::__error()
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_guest_madvise(
    _vm: *mut OcerzVM,
    _cpu: *mut OcerzCPU,
    addr: u64,
    len: u64,
    advice: c_int,
) -> c_int {
    unsafe {
        if crate::ffi::ocerz_mem_overlaps(addr, len) != 0 {
            return 0;
        }
        if libc::madvise(ocerz_g2h(addr), len as usize, advice) == 0 {
            0
        } else {
            *libc::__error()
        }
    }
}

unsafe fn sys_shared_region_check_np(
    _vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    _a: *mut [u64; 8],
) -> c_int {
    ret_err(cpu, OCERZ_ENOSYS_V as u64);
    crate::ffi::OCERZ_STEP_OK as c_int
}

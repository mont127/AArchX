//! BSD syscall pointer translation, intercepts, and dispatch. Guest pointers
//! are translated only at the host boundary, with explicit scratch vectors
//! where the guest and host layouts differ.

use super::util::*;
use super::*;

use core::ffi::{c_char, c_int, c_void};
use core::mem;
use core::ptr;

const OCERZ_HOST_PAGE_SIZE: u64 = 0x4000;
const OCERZ_SHM_ATTACH_MAX: usize = 64;
const CLOCK_UPTIME_RAW: libc::clockid_t = 8;
const VM_FLAGS_FIXED: i32 = 0x0000;
const VM_FLAGS_OVERWRITE: i32 = 0x4000;
const VM_INHERIT_SHARE: libc::vm_inherit_t = 1;

unsafe extern "C" {
    fn clock_gettime_nsec_np(clock_id: libc::clockid_t) -> u64;
    fn mach_thread_self() -> u32;
}

#[repr(C)]
struct Kev {
    ident: u64,
    filter: i16,
    flags: u16,
    fflags: u32,
    data: i64,
    udata: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct ShmAttach {
    gaddr: u64,
    size: u64,
    host: *mut c_void,
}

static mut G_SHM_ATTACH: [ShmAttach; OCERZ_SHM_ATTACH_MAX] = [ShmAttach {
    gaddr: 0,
    size: 0,
    host: ptr::null_mut(),
}; OCERZ_SHM_ATTACH_MAX];
static mut G_SHM_LOCK: libc::pthread_mutex_t = libc::PTHREAD_MUTEX_INITIALIZER;

unsafe extern "C" {
    fn mach_vm_remap(
        target_task: u32,
        target_address: *mut u64,
        size: u64,
        mask: u64,
        flags: i32,
        source_task: u32,
        source_address: u64,
        copy: i32,
        current_protection: *mut i32,
        maximum_protection: *mut i32,
        inheritance: libc::vm_inherit_t,
    ) -> i32;
    static mut mach_task_self_: u32;
}

#[inline(always)]
pub(super) unsafe fn disarm_guest_buffer(cpu: *mut OcerzCPU, gaddr: u64, len: u64) {
    if gaddr == 0 || len == 0 || crate::ffi::ocerz_mem_armed_any() == 0 {
        return;
    }
    let mut pages = [0u64; 64];
    loop {
        let n = crate::ffi::ocerz_mem_disarm_range(
            gaddr,
            gaddr.wrapping_add(len),
            pages.as_mut_ptr(),
            pages.len() as c_int,
        );
        if n <= 0 {
            break;
        }
        for page in pages.iter().take(n as usize) {
            crate::ffi::ocerz_jit_invalidate_range((*cpu).vm, *page, OCERZ_HOST_PAGE_SIZE);
        }
        if env_set!("OCERZ_CACHEPATCHLOG") {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: ARMBUF[%d] gaddr=%#llx len=%#llx pages=%d rip=%#llx\n".as_ptr(),
                libc::getpid(),
                gaddr as libc::c_ulonglong,
                len as libc::c_ulonglong,
                n,
                (*cpu).rip as libc::c_ulonglong,
            );
        }
        if n < pages.len() as c_int {
            break;
        }
    }
}

#[inline(always)]
unsafe fn disarm_guest_iov(cpu: *mut OcerzCPU, giov: u64, mut cnt: u64) {
    if giov == 0 || crate::ffi::ocerz_mem_armed_any() == 0 {
        return;
    }
    if cnt > 1024 {
        cnt = 1024;
    }
    for k in 0..cnt {
        let iov = giov.wrapping_add(k.wrapping_mul(16));
        if crate::ffi::ocerz_addr_readable(iov.wrapping_add(15)) == 0 {
            break;
        }
        disarm_guest_buffer(cpu, ocerz_ld(iov, 8), ocerz_ld(iov.wrapping_add(8), 8));
    }
}

unsafe fn efault_disarm_retry(cpu: *mut OcerzCPU, num: c_int) -> bool {
    let mut pages = [0u64; 64];
    let mut any = false;
    loop {
        let n = crate::ffi::ocerz_mem_disarm_all(pages.as_mut_ptr(), pages.len() as c_int);
        if n <= 0 {
            break;
        }
        any = true;
        for page in pages.iter().take(n as usize) {
            crate::ffi::ocerz_jit_invalidate_range((*cpu).vm, *page, OCERZ_HOST_PAGE_SIZE);
        }
    }
    if any && env_set!("OCERZ_CACHEPATCHLOG") {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: ARMRETRY[%d] num=%d rip=%#llx\n".as_ptr(),
            libc::getpid(),
            num,
            (*cpu).rip as libc::c_ulonglong,
        );
    }
    any
}

#[inline(always)]
fn unstick_kickable(num: c_int) -> bool {
    (297..=309).contains(&num)
        || num == 312
        || num == 334
        || num == 423
        || num == 363
        || num == 369
        || num == 374
        || num == 375
        || num == 368
        || num == 515
        || num == 544
}

unsafe fn semwait_kick_retry(
    cpu: *mut OcerzCPU,
    num: c_int,
    a: *mut [u64; 8],
    ret2: *mut u64,
    err: *mut c_int,
    t0: u64,
) -> u64 {
    let mut r = libc::EINTR as u64;
    let args = &mut *a;
    let total = (args[4] as i64)
        .wrapping_mul(1_000_000_000)
        .wrapping_add(args[5] as i32 as i64);
    while *err != 0
        && r == libc::EINTR as u64
        && ((*cpu).sig_pending & !(*cpu).sig_mask) == 0
        && crate::ffi::ocerz_peek_pending_async_sig() == 0
        && core::sync::atomic::AtomicI32::from_ptr(ptr::addr_of_mut!((*cpu).suspend_count))
            .load(core::sync::atomic::Ordering::Acquire)
            == 0
        && (*cpu).interrupt == 0
        && core::sync::atomic::AtomicI32::from_ptr(ptr::addr_of_mut!((*(*cpu).vm).exited))
            .load(core::sync::atomic::Ordering::Acquire)
            == 0
    {
        if args[3] != 0 {
            let elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW).wrapping_sub(t0) as i64;
            let left = total.wrapping_sub(elapsed);
            if left <= 0 {
                return libc::ETIMEDOUT as u64;
            }
            args[4] = (left / 1_000_000_000) as u64;
            args[5] = (left % 1_000_000_000) as u64;
        }
        (*cpu).block_since_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        r = crate::ported::syscall::raw::ocerz_host_syscall(num as i64, a, ret2, err);
    }
    r
}

unsafe fn forward_with_scratch(
    cpu: *mut OcerzCPU,
    num: c_int,
    a: *mut [u64; 8],
    dual_ret: c_int,
) -> c_int {
    let mut err = 0;
    let mut ret2 = 0;
    (*cpu).block_nokick = 1;
    (*cpu).block_since_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    let mut r = crate::ported::syscall::raw::ocerz_host_syscall(num as i64, a, &mut ret2, &mut err);
    if err != 0 && r == libc::EFAULT as u64 && efault_disarm_retry(cpu, num) {
        r = crate::ported::syscall::raw::ocerz_host_syscall(num as i64, a, &mut ret2, &mut err);
    }
    (*cpu).block_since_ns = 0;
    (*cpu).block_nokick = 0;
    if err != 0 {
        ret_err(cpu, r);
    } else if dual_ret != 0 {
        ret_ok2(cpu, r, ret2);
    } else {
        ret_ok(cpu, r);
    }
    if err != 0 { r as c_int } else { 0 }
}

unsafe fn sys_iov(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8], num: c_int) -> c_int {
    let args = &*a;
    let mut cnt = args[2];
    if cnt > OCERZ_IOV_MAX as u64 {
        cnt = OCERZ_IOV_MAX as u64;
    }
    let mut scratch = [ocerz_iovec {
        iov_base: 0,
        iov_len: 0,
    }; OCERZ_IOV_MAX];
    for i in 0..cnt {
        let addr = args[1].wrapping_add(i.wrapping_mul(16));
        let base = ocerz_ld(addr, 8);
        let entry = scratch.as_mut_ptr().add(i as usize);
        (*entry).iov_base = if base != 0 { ocerz_g2h(base) as u64 } else { 0 };
        (*entry).iov_len = ocerz_ld(addr.wrapping_add(8), 8);
    }
    let mut fa = [args[0], scratch.as_mut_ptr() as u64, args[2], 0, 0, 0, 0, 0];
    forward_with_scratch(cpu, num, &mut fa, 0);
    crate::ffi::OCERZ_STEP_OK as c_int
}

unsafe fn sys_readv(vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    sys_iov(vm, cpu, a, 120)
}

unsafe fn sys_writev(vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    sys_iov(vm, cpu, a, 121)
}

unsafe fn ocerz_sysfail_note(cpu: *mut OcerzCPU, num: c_int, eno: c_int, orig: *const [u64; 8]) {
    static mut FLOG: c_int = -1;
    if FLOG < 0 {
        let e = libc::getenv(c"OCERZ_SYSFAIL".as_ptr());
        FLOG = if e.is_null() {
            0
        } else {
            let parsed = libc::atoi(e);
            if parsed == 0 { 1 } else { parsed }
        };
    }
    if FLOG == 0 || eno == 0 || num == 333 {
        return;
    }
    let interesting = eno == libc::ENOMEM
        || eno == libc::EFAULT
        || (FLOG >= 2
            && !matches!(
                eno,
                libc::EINTR
                    | libc::EAGAIN
                    | libc::ENOENT
                    | libc::EEXIST
                    | libc::EINPROGRESS
                    | libc::ETIMEDOUT
                    | libc::ENOTCONN
                    | libc::EISDIR
                    | libc::ESRCH
            ));
    if !interesting {
        return;
    }
    let args = &*orig;
    let name = if num > 0 && num < OCERZ_BSD_MAX {
        let candidate = ptr::addr_of!(BSD_TABLE)
            .cast::<ocerz_bsd_entry>()
            .add(num as usize);
        if !(*candidate).name.is_null() {
            (*candidate).name
        } else {
            c"?".as_ptr()
        }
    } else {
        c"?".as_ptr()
    };
    libc::fprintf(
        crate::log::stderr(),
        c"ocerz: SYSFAIL[%d] num=%d(%s) errno=%d a0=%#llx a1=%#llx a2=%#llx a3=%#llx rip=%#llx\n"
            .as_ptr(),
        libc::getpid(),
        num,
        name,
        eno,
        args[0] as libc::c_ulonglong,
        args[1] as libc::c_ulonglong,
        args[2] as libc::c_ulonglong,
        args[3] as libc::c_ulonglong,
        (*cpu).rip as libc::c_ulonglong,
    );
}

unsafe fn strace_bsd(
    vm: *mut OcerzVM,
    e: *const ocerz_bsd_entry,
    num: c_int,
    orig: *const [u64; 8],
    cpu: *mut OcerzCPU,
) {
    if (*vm).strace == 0 {
        return;
    }
    let name = if e.is_null() || (*e).name.is_null() {
        c"?".as_ptr()
    } else {
        (*e).name
    };
    libc::fprintf(
        crate::log::stderr(),
        c"ocerz: [t%d] syscall %s(".as_ptr(),
        (*cpu).cpu_number,
        name,
    );
    let nargs = if e.is_null() { 6 } else { (*e).nargs as c_int }.min(6);
    let args = &*orig;
    for i in 0..nargs {
        libc::fprintf(
            crate::log::stderr(),
            if i == 0 {
                c"%#llx".as_ptr()
            } else {
                c", %#llx".as_ptr()
            },
            *args.as_ptr().add(i as usize) as libc::c_ulonglong,
        );
    }
    if (*cpu).rflags & crate::inline::OCERZ_CF != 0 {
        libc::fprintf(
            crate::log::stderr(),
            c") = -1 %s\n".as_ptr(),
            errno_name((*cpu).gpr[crate::ffi::OCERZ_RAX as usize] as c_int),
        );
    } else if nargs < 3 && args[2] != 0 {
        libc::fprintf(
            crate::log::stderr(),
            c") = %#llx [rdx_in=%#llx]\n".as_ptr(),
            (*cpu).gpr[crate::ffi::OCERZ_RAX as usize] as libc::c_ulonglong,
            args[2] as libc::c_ulonglong,
        );
    } else {
        libc::fprintf(
            crate::log::stderr(),
            c") = %#llx\n".as_ptr(),
            (*cpu).gpr[crate::ffi::OCERZ_RAX as usize] as libc::c_ulonglong,
        );
    }
    if !e.is_null()
        && !(*e).name.is_null()
        && (libc::strcmp((*e).name, c"open".as_ptr()) == 0
            || libc::strcmp((*e).name, c"access".as_ptr()) == 0)
    {
        let mut path = [0i8; 128];
        for i in 0..127usize {
            let ch = path.as_mut_ptr().add(i);
            *ch = ocerz_ld(args[0].wrapping_add(i as u64), 1) as i8;
            if *ch == 0 {
                break;
            }
            *path.as_mut_ptr().add(i + 1) = 0;
        }
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz:    path[%#llx] = \"%s\"\n".as_ptr(),
            args[0] as libc::c_ulonglong,
            path.as_ptr(),
        );
    }
    let _ = num;
}

unsafe fn set_comm_field(g: u64, cap: usize, name: *const c_char) {
    let dst = ocerz_g2h(g).cast::<u8>();
    let n = libc::strlen(name);
    ptr::write_bytes(dst, 0, cap);
    ptr::copy_nonoverlapping(name.cast::<u8>(), dst, n.min(cap));
}

unsafe fn proc_info_self_fixup(num: c_int, orig: *const [u64; 8]) {
    let args = &*orig;
    let path = crate::ffi::ocerz_dyld_main_path();
    let buf = if num == 545 { args[6] } else { args[4] };
    let size = (if num == 545 { args[7] } else { args[5] }) as u32 as u64;
    if path.is_null() || args[0] as i32 != 2 || args[1] as i32 != libc::getpid() || buf == 0 {
        return;
    }
    let mut base = libc::strrchr(path, b'/' as c_int);
    if base.is_null() {
        base = path.cast_mut();
    } else {
        base = base.add(1);
    }
    match args[2] as u32 {
        11 => {
            let n = libc::strlen(path).wrapping_add(1);
            if n <= size as usize {
                ptr::copy_nonoverlapping(path.cast::<u8>(), ocerz_g2h(buf).cast::<u8>(), n);
            }
        }
        2 | 3 if size >= 136 => {
            set_comm_field(buf.wrapping_add(48), 16, base);
            set_comm_field(buf.wrapping_add(64), 32, base);
        }
        13 if size >= 64 => set_comm_field(buf.wrapping_add(16), 16, base),
        _ => {}
    }
}

pub(super) unsafe fn dispatch_bsd(vm: *mut OcerzVM, cpu: *mut OcerzCPU, num: c_int) -> c_int {
    dispatch_bsd_at(vm, cpu, num, 0)
}

pub(super) unsafe fn dispatch_bsd_at(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    num: c_int,
    stack_skip: u64,
) -> c_int {
    if num == 0 {
        let regs = ptr::addr_of_mut!((*cpu).gpr).cast::<u64>();
        let real = (*regs.add(crate::ffi::OCERZ_RDI as usize) & 0x00ff_ffff) as c_int;
        let r9_save = *regs.add(crate::ffi::OCERZ_R9 as usize);
        *regs.add(crate::ffi::OCERZ_RDI as usize) = *regs.add(crate::ffi::OCERZ_RSI as usize);
        *regs.add(crate::ffi::OCERZ_RSI as usize) = *regs.add(crate::ffi::OCERZ_RDX as usize);
        *regs.add(crate::ffi::OCERZ_RDX as usize) = *regs.add(crate::ffi::OCERZ_R10 as usize);
        *regs.add(crate::ffi::OCERZ_R10 as usize) = *regs.add(crate::ffi::OCERZ_R8 as usize);
        *regs.add(crate::ffi::OCERZ_R8 as usize) = r9_save;
        *regs.add(crate::ffi::OCERZ_R9 as usize) = ocerz_ld(
            (*regs.add(crate::ffi::OCERZ_RSP as usize))
                .wrapping_add(stack_skip)
                .wrapping_add(8),
            8,
        );
        return dispatch_bsd_at(vm, cpu, real, stack_skip.wrapping_add(8));
    }

    let entry = if num >= 0 && num < OCERZ_BSD_MAX {
        let candidate = ptr::addr_of!(BSD_TABLE)
            .cast::<ocerz_bsd_entry>()
            .add(num as usize);
        if (*candidate).name.is_null() {
            ptr::null()
        } else {
            candidate
        }
    } else {
        ptr::null()
    };
    if entry.is_null() {
        if libc::getenv(c"OCERZ_STRICT_SYSCALL".as_ptr()).is_null() {
            static mut PROBED: [u8; OCERZ_BSD_MAX as usize] = [0; OCERZ_BSD_MAX as usize];
            if num >= 0 && num < OCERZ_BSD_MAX {
                let probed = ptr::addr_of_mut!(PROBED).cast::<u8>().add(num as usize);
                if *probed == 0 {
                    *probed = 1;
                    libc::fprintf(
                        crate::log::stderr(),
                        c"ocerz: unimplemented BSD syscall num=%d -> ENOSYS rip=%#llx rdi=%#llx rsi=%#llx rdx=%#llx r10=%#llx\n".as_ptr(),
                        num,
                        (*cpu).rip as libc::c_ulonglong,
                        (*cpu).gpr[crate::ffi::OCERZ_RDI as usize] as libc::c_ulonglong,
                        (*cpu).gpr[crate::ffi::OCERZ_RSI as usize] as libc::c_ulonglong,
                        (*cpu).gpr[crate::ffi::OCERZ_RDX as usize] as libc::c_ulonglong,
                        (*cpu).gpr[crate::ffi::OCERZ_R10 as usize] as libc::c_ulonglong,
                    );
                }
            }
            ret_err(cpu, libc::ENOSYS as u64);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        crate::ocerz_fatal!(
            "unknown BSD syscall: class=2 num=%d (no table entry) rip=%#llx rdi=%#llx rsi=%#llx rdx=%#llx r10=%#llx ret=%#llx\n",
            num,
            (*cpu).rip as libc::c_ulonglong,
            (*cpu).gpr[crate::ffi::OCERZ_RDI as usize] as libc::c_ulonglong,
            (*cpu).gpr[crate::ffi::OCERZ_RSI as usize] as libc::c_ulonglong,
            (*cpu).gpr[crate::ffi::OCERZ_RDX as usize] as libc::c_ulonglong,
            (*cpu).gpr[crate::ffi::OCERZ_R10 as usize] as libc::c_ulonglong,
            ocerz_ld((*cpu).gpr[crate::ffi::OCERZ_RSP as usize], 8) as libc::c_ulonglong
        );
        return crate::ffi::OCERZ_STEP_FATAL as c_int;
    }

    let mut a = [0u64; 8];
    let regs = ptr::addr_of!((*cpu).gpr).cast::<u64>();
    a[0] = *regs.add(crate::ffi::OCERZ_RDI as usize);
    a[1] = *regs.add(crate::ffi::OCERZ_RSI as usize);
    a[2] = *regs.add(crate::ffi::OCERZ_RDX as usize);
    a[3] = *regs.add(crate::ffi::OCERZ_R10 as usize);
    a[4] = *regs.add(crate::ffi::OCERZ_R8 as usize);
    a[5] = *regs.add(crate::ffi::OCERZ_R9 as usize);
    if (*entry).nargs > 6 {
        let rsp = *regs.add(crate::ffi::OCERZ_RSP as usize);
        for i in 6..((*entry).nargs as usize).min(8) {
            *a.as_mut_ptr().add(i) = ocerz_ld(
                rsp.wrapping_add(stack_skip)
                    .wrapping_add(8u64.wrapping_mul((i - 5) as u64)),
                8,
            );
        }
    }
    let orig = a;

    static mut FDOPLOG: c_int = -2;
    if FDOPLOG == -2 {
        FDOPLOG = c_int::from(!libc::getenv(c"OCERZ_FDOPLOG".as_ptr()).is_null());
        let filter = libc::getenv(c"OCERZ_FDOPLOG_EXE".as_ptr());
        if FDOPLOG != 0 && !filter.is_null() && *filter != 0 {
            let summary =
                ptr::addr_of!(crate::ported::globals::ocerz_cmdline_summary).cast::<c_char>();
            FDOPLOG = if *summary == 0 {
                -1
            } else {
                c_int::from(!libc::strstr(summary, filter).is_null())
            };
        }
    }
    if FDOPLOG != 0 {
        let selected = matches!(num, 6 | 442 | 90 | 41 | 135 | 399);
        if selected {
            let name = match num {
                6 => c"close".as_ptr(),
                442 => c"guarded_close".as_ptr(),
                90 => c"dup2".as_ptr(),
                41 => c"dup".as_ptr(),
                135 => c"socketpair".as_ptr(),
                _ => c"close_nocancel".as_ptr(),
            };
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: FD[%d] cpu#%u %s fd=%d a1=%#llx rip=%#llx\n".as_ptr(),
                libc::getpid(),
                (*cpu).cpu_number,
                name,
                orig[0] as c_int,
                orig[1] as libc::c_ulonglong,
                (*cpu).rip as libc::c_ulonglong,
            );
        }
    }

    if crate::ffi::ocerz_mem_armed_any() != 0 {
        match num {
            3 | 396 | 153 | 414 | 29 | 403 => {
                disarm_guest_buffer(cpu, orig[1], orig[2]);
            }
            120 | 411 => disarm_guest_iov(cpu, orig[1], orig[2]),
            27 | 401 if crate::ffi::ocerz_addr_readable(orig[1].wrapping_add(31)) != 0 => {
                disarm_guest_iov(
                    cpu,
                    ocerz_ld(orig[1].wrapping_add(16), 8),
                    ocerz_ld(orig[1].wrapping_add(24), 4) as u32 as u64,
                );
            }
            _ => {}
        }
    }

    static mut SCPU: c_int = -2;
    if SCPU == -2 {
        let e = libc::getenv(c"OCERZ_STRACE_CPU".as_ptr());
        SCPU = if e.is_null() { -1 } else { libc::atoi(e) };
    }
    if (*cpu).cpu_number as c_int == SCPU {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: SC[%d] cpu#%u bsd %s(%d) a0=%#llx a1=%#llx a2=%#llx rip=%#llx gs=%#llx gs18=%#llx ic=%#llx\n".as_ptr(),
            libc::getpid(),
            (*cpu).cpu_number,
            (*entry).name,
            num,
            a[0] as libc::c_ulonglong,
            a[1] as libc::c_ulonglong,
            a[2] as libc::c_ulonglong,
            (*cpu).rip as libc::c_ulonglong,
            (*cpu).gs_base as libc::c_ulonglong,
            if crate::ffi::ocerz_addr_readable((*cpu).gs_base.wrapping_add(0x18)) != 0 {
                ocerz_ld((*cpu).gs_base.wrapping_add(0x18), 8)
            } else {
                0
            } as libc::c_ulonglong,
            (*vm).insn_count as libc::c_ulonglong,
        );
    }

    static mut WRLOG: c_int = -1;
    if WRLOG < 0 {
        WRLOG = c_int::from(!libc::getenv(c"OCERZ_MSGLOG".as_ptr()).is_null());
        let filter = libc::getenv(c"OCERZ_MSGLOG_EXE".as_ptr());
        if WRLOG != 0 && !filter.is_null() && *filter != 0 {
            let summary =
                ptr::addr_of!(crate::ported::globals::ocerz_cmdline_summary).cast::<c_char>();
            WRLOG = c_int::from(!libc::strstr(summary, filter).is_null());
        }
    }
    if WRLOG != 0 && matches!(num, 3 | 4 | 396 | 397) && a[2] == 64 && a[1] != 0 {
        let rq = ocerz_ld(a[1], 4) as u32;
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: %s64[%d] cpu#%u fd=%d req=%#x ic=%#llx".as_ptr(),
            if num == 4 || num == 397 {
                c"WR".as_ptr()
            } else {
                c"RD".as_ptr()
            },
            libc::getpid(),
            (*cpu).cpu_number,
            a[0] as c_int,
            rq,
            (*vm).insn_count as libc::c_ulonglong,
        );
        if (num == 4 || num == 397) && rq == 8 {
            libc::fprintf(
                crate::log::stderr(),
                c" handle=%#x exit_code=%#x".as_ptr(),
                ocerz_ld(a[1].wrapping_add(12), 4) as u32,
                ocerz_ld(a[1].wrapping_add(16), 4) as u32,
            );
        }
        libc::fputc(b'\n' as c_int, crate::log::stderr());
    }

    if let Some(intercept) = (*entry).intercept {
        let result = intercept(vm, cpu, &mut a);
        if result == crate::ffi::OCERZ_STEP_FATAL as c_int {
            crate::ocerz_fatal!(
                "not yet supported: class=2 num=%d name=%s\n",
                num,
                (*entry).name
            );
            return crate::ffi::OCERZ_STEP_FATAL as c_int;
        }
        if result == crate::ffi::OCERZ_STEP_OK as c_int
            && (*cpu).rflags & crate::inline::OCERZ_CF != 0
        {
            ocerz_sysfail_note(
                cpu,
                num,
                (*cpu).gpr[crate::ffi::OCERZ_RAX as usize] as c_int,
                &orig,
            );
        }
        strace_bsd(vm, entry, num, &orig, cpu);
        return result;
    }

    for i in 0..8 {
        let arg = a.as_mut_ptr().add(i);
        if (*entry).ptr_mask & (1 << i) != 0 && *arg != 0 {
            *arg = ocerz_g2h(*arg) as u64;
        }
    }

    let mut err = 0;
    let mut ret2 = 0;
    static mut ULOCKLOG: c_int = -1;
    if ULOCKLOG < 0 {
        ULOCKLOG = if libc::getenv(c"OCERZ_ULOCKLOG".as_ptr()).is_null() {
            0
        } else {
            1
        };
    }
    if matches!(num, 515 | 516 | 544) && ULOCKLOG != 0 {
        let myport = if (*cpu).gs_base != 0 {
            ocerz_ld((*cpu).gs_base.wrapping_add(0x18), 4)
        } else {
            0
        };
        let host_self = mach_thread_self();
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: ULOCK[%d] cpu#%u %s op=%#llx addr=%#llx val=%#llx gs18=%#llx hostself=%#x %s caller=%#llx\n".as_ptr(),
            libc::getpid(),
            (*cpu).cpu_number,
            (*entry).name,
            orig[0] as libc::c_ulonglong,
            orig[1] as libc::c_ulonglong,
            orig[2] as libc::c_ulonglong,
            myport as libc::c_ulonglong,
            host_self,
            if myport == host_self as u64 { c"MATCH".as_ptr() } else { c"MISMATCH".as_ptr() },
            (*cpu).rip as libc::c_ulonglong,
        );
    }

    static mut BLOCKLOG: c_int = -1;
    if BLOCKLOG < 0 {
        let e = libc::getenv(c"OCERZ_BLOCKLOG".as_ptr());
        BLOCKLOG = if e.is_null() { 0 } else { 1 };
        let filter = libc::getenv(c"OCERZ_BLOCKLOG_EXE".as_ptr());
        if !filter.is_null() && *filter != 0 {
            let summary =
                ptr::addr_of!(crate::ported::globals::ocerz_cmdline_summary).cast::<c_char>();
            BLOCKLOG = if libc::strstr(summary, filter).is_null() {
                0
            } else {
                1
            };
        }
    }
    static BLOCKSEQ: core::sync::atomic::AtomicU64 = core::sync::atomic::AtomicU64::new(0);
    let btrack = BLOCKLOG != 0
        && matches!(
            num,
            3 | 27 | 29 | 93 | 133 | 230 | 231 | 301 | 305 | 334 | 368 | 478 | 516
        );
    let mut bseq = 0;
    if btrack {
        bseq = BLOCKSEQ
            .fetch_add(1, core::sync::atomic::Ordering::Relaxed)
            .wrapping_add(1);
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: BLK-IN[%d] seq=%llu cpu#%u num=%d a0=%#llx a1=%#llx a2=%#llx rip=%#llx ret0=%#llx bt:".as_ptr(),
            libc::getpid(),
            bseq as libc::c_ulonglong,
            (*cpu).cpu_number,
            num,
            orig[0] as libc::c_ulonglong,
            orig[1] as libc::c_ulonglong,
            orig[2] as libc::c_ulonglong,
            (*cpu).rip as libc::c_ulonglong,
            ocerz_ld((*cpu).gpr[crate::ffi::OCERZ_RSP as usize], 8) as libc::c_ulonglong,
        );
        let mut fp = (*cpu).gpr[crate::ffi::OCERZ_RBP as usize];
        for _ in 0..8 {
            if fp <= 0x1000 {
                break;
            }
            libc::fprintf(
                crate::log::stderr(),
                c" %#llx".as_ptr(),
                ocerz_ld(fp.wrapping_add(8), 8) as libc::c_ulonglong,
            );
            let next = ocerz_ld(fp, 8);
            if next <= fp {
                break;
            }
            fp = next;
        }
        libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
    }

    static mut SOCKLOG_PRE: c_int = -1;
    if SOCKLOG_PRE < 0 {
        SOCKLOG_PRE = c_int::from(!libc::getenv(c"OCERZ_SOCKLOG".as_ptr()).is_null());
        let filter = libc::getenv(c"OCERZ_SOCKLOG_EXE".as_ptr());
        if SOCKLOG_PRE != 0 && !filter.is_null() && *filter != 0 {
            let summary =
                ptr::addr_of!(crate::ported::globals::ocerz_cmdline_summary).cast::<c_char>();
            SOCKLOG_PRE = c_int::from(!libc::strstr(summary, filter).is_null());
        }
    }
    if SOCKLOG_PRE != 0 && matches!(num, 97 | 98 | 104 | 105 | 30 | 106) {
        let mut address = [0i8; 160];
        if matches!(num, 98 | 104)
            && a[1] != 0
            && crate::ffi::ocerz_addr_readable(a[1].wrapping_add(1)) != 0
        {
            let family = ocerz_ld(a[1].wrapping_add(1), 1) as u8;
            if family == 2 && crate::ffi::ocerz_addr_readable(a[1].wrapping_add(7)) != 0 {
                let port =
                    (ocerz_ld(a[1].wrapping_add(2), 1) << 8) | ocerz_ld(a[1].wrapping_add(3), 1);
                let ip = ocerz_ld(a[1].wrapping_add(4), 4);
                libc::snprintf(
                    address.as_mut_ptr(),
                    address.len(),
                    c" AF_INET %llu.%llu.%llu.%llu:%llu".as_ptr(),
                    (ip & 0xff) as libc::c_ulonglong,
                    ((ip >> 8) & 0xff) as libc::c_ulonglong,
                    ((ip >> 16) & 0xff) as libc::c_ulonglong,
                    ((ip >> 24) & 0xff) as libc::c_ulonglong,
                    port as libc::c_ulonglong,
                );
            } else if family == 30 && crate::ffi::ocerz_addr_readable(a[1].wrapping_add(7)) != 0 {
                let port =
                    (ocerz_ld(a[1].wrapping_add(2), 1) << 8) | ocerz_ld(a[1].wrapping_add(3), 1);
                libc::snprintf(
                    address.as_mut_ptr(),
                    address.len(),
                    c" AF_INET6 [..]:%llu".as_ptr(),
                    port as libc::c_ulonglong,
                );
            } else if family == 1 {
                libc::snprintf(address.as_mut_ptr(), address.len(), c" AF_UNIX".as_ptr());
            } else {
                libc::snprintf(
                    address.as_mut_ptr(),
                    address.len(),
                    c" fam=%u".as_ptr(),
                    family as c_uint,
                );
            }
        }
        let name = match num {
            97 => c"socket".as_ptr(),
            98 => c"connect".as_ptr(),
            104 => c"bind".as_ptr(),
            105 => c"setsockopt".as_ptr(),
            30 => c"accept".as_ptr(),
            _ => c"listen".as_ptr(),
        };
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: SOCK[%d] %s fd=%d a1=%#llx a2=%#llx%s len=%#llx\n".as_ptr(),
            libc::getpid(),
            name,
            a[0] as c_int,
            a[1] as libc::c_ulonglong,
            a[2] as libc::c_ulonglong,
            address.as_ptr(),
            a[2] as libc::c_ulonglong,
        );
    }

    if matches!(num, 363 | 369) && env_set!("OCERZ_KEVLOG") && orig[2] == 0 && orig[4] == 1 {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: KEVWAIT-ENTER[%d] num=%d kq=%lld(=fd %d) nev=%lld timeout=%#llx caller=%#llx\n".as_ptr(),
            libc::getpid(),
            num,
            orig[0] as i64,
            orig[0] as c_int,
            orig[4] as i64,
            orig[5] as libc::c_ulonglong,
            (*cpu).rip as libc::c_ulonglong,
        );
    }

    let blocklog_start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    (*cpu).block_nokick = if unstick_kickable(num) { 0 } else { 1 };
    (*cpu).block_since_ns = blocklog_start;
    (*cpu).block_what = num;
    let mut r =
        crate::ported::syscall::raw::ocerz_host_syscall(num as i64, &mut a, &mut ret2, &mut err);
    if err != 0 && r == libc::EFAULT as u64 && efault_disarm_retry(cpu, num) {
        r = crate::ported::syscall::raw::ocerz_host_syscall(
            num as i64, &mut a, &mut ret2, &mut err,
        );
    }
    if matches!(num, 334 | 423) && err != 0 && r == libc::EINTR as u64 && a[2] != 0 {
        r = semwait_kick_retry(cpu, num, &mut a, &mut ret2, &mut err, blocklog_start);
    }
    (*cpu).block_since_ns = 0;
    (*cpu).block_nokick = 0;
    super::entry::pagetrap_post_mmap(cpu, num, &orig, r, err);

    static mut SOCKLOG: c_int = -1;
    if SOCKLOG < 0 {
        SOCKLOG = c_int::from(!libc::getenv(c"OCERZ_SOCKLOG".as_ptr()).is_null());
        let filter = libc::getenv(c"OCERZ_SOCKLOG_EXE".as_ptr());
        if SOCKLOG != 0 && !filter.is_null() && *filter != 0 {
            let summary =
                ptr::addr_of!(crate::ported::globals::ocerz_cmdline_summary).cast::<c_char>();
            SOCKLOG = c_int::from(!libc::strstr(summary, filter).is_null());
        }
    }
    if SOCKLOG != 0 && matches!(num, 97 | 98 | 104 | 105 | 30 | 106) {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: SOCK[%d]  -> %s ret=%#llx err=%d\n".as_ptr(),
            libc::getpid(),
            match num {
                97 => c"socket".as_ptr(),
                98 => c"connect".as_ptr(),
                104 => c"bind".as_ptr(),
                105 => c"setsockopt".as_ptr(),
                30 => c"accept".as_ptr(),
                _ => c"listen".as_ptr(),
            },
            r as libc::c_ulonglong,
            if err != 0 { r as c_int } else { 0 },
        );
    }
    static mut FDOPLOG2: c_int = -2;
    if FDOPLOG2 == -2 {
        FDOPLOG2 = c_int::from(!libc::getenv(c"OCERZ_FDOPLOG".as_ptr()).is_null());
        let filter = libc::getenv(c"OCERZ_FDOPLOG_EXE".as_ptr());
        if FDOPLOG2 != 0 && !filter.is_null() && *filter != 0 {
            let summary =
                ptr::addr_of!(crate::ported::globals::ocerz_cmdline_summary).cast::<c_char>();
            FDOPLOG2 = if *summary == 0 {
                -1
            } else {
                c_int::from(!libc::strstr(summary, filter).is_null())
            };
        }
    }
    if FDOPLOG2 > 0
        && err == 0
        && num == 135
        && orig[3] != 0
        && crate::ffi::ocerz_addr_readable(orig[3].wrapping_add(7)) != 0
    {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: FD[%d] cpu#%u socketpair -> %d %d rip=%#llx\n".as_ptr(),
            libc::getpid(),
            (*cpu).cpu_number,
            ocerz_ld(orig[3], 4) as c_int,
            ocerz_ld(orig[3].wrapping_add(4), 4) as c_int,
            (*cpu).rip as libc::c_ulonglong,
        );
    }
    if FDOPLOG2 > 0
        && err == 0
        && num == 3
        && orig[2] == 16
        && r == 16
        && crate::ffi::ocerz_addr_readable(orig[1].wrapping_add(15)) != 0
        && ocerz_ld(orig[1], 8) == 0
    {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: FD[%d] cpu#%u WAKEUP0 fd=%d signaled=%#llx rip=%#llx\n".as_ptr(),
            libc::getpid(),
            (*cpu).cpu_number,
            orig[0] as c_int,
            ocerz_ld(orig[1].wrapping_add(8), 8) as libc::c_ulonglong,
            (*cpu).rip as libc::c_ulonglong,
        );
    }
    if FDOPLOG2 > 0 && err == 0 && matches!(num, 41 | 90 | 42) {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: FD[%d] cpu#%u %s -> %d rip=%#llx\n".as_ptr(),
            libc::getpid(),
            (*cpu).cpu_number,
            match num {
                41 => c"dup".as_ptr(),
                90 => c"dup2".as_ptr(),
                _ => c"pipe".as_ptr(),
            },
            r as c_int,
            (*cpu).rip as libc::c_ulonglong,
        );
    }
    static mut ULOCKWAITLOG: c_int = -1;
    if ULOCKWAITLOG < 0 {
        ULOCKWAITLOG = c_int::from(!libc::getenv(c"OCERZ_ULOCKLOG".as_ptr()).is_null());
    }
    if ULOCKWAITLOG != 0 && matches!(num, 515 | 544) {
        let signed_result = r as i64;
        if err != 0 || (signed_result < 0 && signed_result > -4096) {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: ULOCKW[%d] num=%d op=%#llx addr=%#llx owner=%#llx r=%lld err=%d hostself=%#x rip=%#llx\n"
                    .as_ptr(),
                libc::getpid(),
                num,
                orig[0] as libc::c_ulonglong,
                orig[1] as libc::c_ulonglong,
                orig[2] as libc::c_ulonglong,
                signed_result,
                err,
                mach_thread_self(),
                (*cpu).rip as libc::c_ulonglong,
            );
        }
    }
    static mut REQLOG: c_int = -1;
    if REQLOG < 0 {
        REQLOG = c_int::from(!libc::getenv(c"OCERZ_REQLOG".as_ptr()).is_null());
    }
    if REQLOG != 0
        && ((matches!(num, 4 | 121) && matches!(orig[2], 8 | 16 | 64 | 80)) || num == 121)
    {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: REQW[%d] num=%d fd=%lld len=%lld first=%#x -> r=%lld err=%d rip=%#llx\n"
                .as_ptr(),
            libc::getpid(),
            num,
            orig[0] as i64,
            orig[2] as libc::c_ulonglong,
            if num == 4 && orig[1] != 0 {
                ocerz_ld(orig[1], 4) as u32
            } else {
                0
            },
            r as i64,
            err,
            (*cpu).rip as libc::c_ulonglong,
        );
    }
    static mut MSGREADLOG: c_int = -1;
    static mut MSGREADLOG_COUNT: c_int = 0;
    if MSGREADLOG < 0 {
        MSGREADLOG = c_int::from(!libc::getenv(c"OCERZ_MSGLOG".as_ptr()).is_null());
    }
    if MSGREADLOG != 0 && num == 3 && orig[2] == 1 && MSGREADLOG_COUNT < 60 {
        MSGREADLOG_COUNT += 1;
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: RD1[%d] cpu#%u fd=%d -> r=%lld err=%d\n".as_ptr(),
            libc::getpid(),
            (*cpu).cpu_number,
            orig[0] as c_int,
            r as i64,
            if err != 0 { r as c_int } else { 0 },
        );
    }
    if MSGREADLOG != 0 && num == 3 && orig[2] == 64 && orig[1] != 0 && r == 64 {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: RPLY[%d] cpu#%u fd=%d err=%#x sz=%u w2=%#x w3=%#x\n".as_ptr(),
            libc::getpid(),
            (*cpu).cpu_number,
            orig[0] as c_int,
            ocerz_ld(orig[1], 4) as u32,
            ocerz_ld(orig[1].wrapping_add(4), 4) as u32,
            ocerz_ld(orig[1].wrapping_add(8), 4) as u32,
            ocerz_ld(orig[1].wrapping_add(12), 4) as u32,
        );
    }
    static mut NETLOG: c_int = -1;
    if NETLOG < 0 {
        NETLOG = c_int::from(!libc::getenv(c"OCERZ_NETLOG".as_ptr()).is_null());
    }
    if NETLOG != 0 && num == 363 && a[1] != 0 && a[2] as i64 > 0 {
        for index in 0..(a[2] as usize).min(8) {
            let event = (a[1] as *const u8).add(index * 32);
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: KEVCH[%d] ident=%llu filter=%d flags=%#x\n".as_ptr(),
                libc::getpid(),
                ptr::read_unaligned(event.cast::<u64>()) as libc::c_ulonglong,
                ptr::read_unaligned(event.add(8).cast::<i16>()) as c_int,
                ptr::read_unaligned(event.add(10).cast::<u16>()) as c_uint,
            );
        }
    }
    if NETLOG != 0 && num == 363 && r as i64 > 0 && a[3] != 0 {
        let event = a[3] as *const u8;
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: KEV[%d] n=%llu ident=%llu filter=%d flags=%#x fflags=%#x data=%lld\n".as_ptr(),
            libc::getpid(),
            r as libc::c_ulonglong,
            ptr::read_unaligned(event.cast::<u64>()) as libc::c_ulonglong,
            ptr::read_unaligned(event.add(8).cast::<i16>()) as c_int,
            ptr::read_unaligned(event.add(10).cast::<u16>()) as c_uint,
            ptr::read_unaligned(event.add(12).cast::<u32>()),
            ptr::read_unaligned(event.add(16).cast::<i64>()),
        );
    }
    if NETLOG != 0
        && matches!(
            num,
            97 | 98 | 104 | 106 | 30 | 133 | 29 | 361 | 362 | 363 | 403 | 404 | 413
        )
    {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: NET[%d] num=%d(%s) a0=%#llx a1=%#llx a2=%#llx -> r=%llu err=%d\n".as_ptr(),
            libc::getpid(),
            num,
            (*entry).name,
            a[0] as libc::c_ulonglong,
            a[1] as libc::c_ulonglong,
            a[2] as libc::c_ulonglong,
            r as libc::c_ulonglong,
            err,
        );
    }
    if matches!(num, 362 | 363 | 369) && env_set!("OCERZ_KEVLOG") {
        static mut KEV_DUMPED: c_int = 0;
        if KEV_DUMPED == 0 {
            KEV_DUMPED = 1;
            crate::ffi::ocerz_dyld_dump_images();
        }
        let mut history = [0u64; 8];
        let count = crate::ffi::ocerz_vm_riphist(history.as_mut_ptr(), history.len() as u32);
        let mut cbase = 0;
        let mut cmod = ptr::null();
        for rip in history.iter().take(count as usize) {
            cmod = crate::ffi::ocerz_dyld_name_for_addr(*rip, &mut cbase);
            if !cmod.is_null() {
                break;
            }
        }
        let _ = (cmod, cbase);
        let mut ts0 = -1i64;
        let mut ts1 = -1i64;
        if num != 362 && orig[5] != 0 {
            ts0 = ocerz_ld(orig[5], 8) as i64;
            ts1 = ocerz_ld(orig[5].wrapping_add(8), 8) as i64;
        }
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: KEV[%d] %s kq=%lld nchanges=%lld nevents=%lld timeout=%lld.%09lld ret=%lld err=%d caller=%#llx\n"
                .as_ptr(),
            libc::getpid(),
            (*entry).name,
            orig[0] as i64,
            orig[2] as i64,
            orig[4] as i64,
            ts0,
            ts1,
            r as i64,
            err,
            if count == 0 {
                0
            } else {
                history[0] as libc::c_ulonglong
            },
        );
        static mut KEV_POLL_SEEN: c_int = 0;
        if num != 362 && orig[2] == 0 && orig[4] > 0 {
            KEV_POLL_SEEN += 1;
        }
        if KEV_POLL_SEEN == 400 && num != 362 {
            let mut late_history = [0u64; 32];
            let late_count =
                crate::ffi::ocerz_vm_riphist(late_history.as_mut_ptr(), late_history.len() as u32);
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: KEVSTACK[%d] kq=%lld late-poll frames:".as_ptr(),
                libc::getpid(),
                orig[0] as i64,
            );
            for rip in late_history.iter().take(late_count as usize) {
                libc::fprintf(
                    crate::log::stderr(),
                    c" %#llx".as_ptr(),
                    *rip as libc::c_ulonglong,
                );
            }
            libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
        }
        if num != 362 && orig[1] != 0 && orig[2] != 0 {
            for index in 0..(orig[2] as usize).min(4) {
                let event = orig[1].wrapping_add(index as u64 * 32);
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz:   change ident=%#llx filter=%d flags=%#x fflags=%#x\n".as_ptr(),
                    ocerz_ld(event, 8) as libc::c_ulonglong,
                    ocerz_ld(event + 8, 2) as u16 as i16 as c_int,
                    ocerz_ld(event + 10, 2) as u16 as c_uint,
                    ocerz_ld(event + 12, 4) as u32,
                );
            }
        }
        if num != 362 && orig[3] != 0 && err == 0 && r as i64 > 0 {
            for index in 0..(r as usize).min(2) {
                let event = orig[3].wrapping_add(index as u64 * 32);
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz:   EVENT ident=%#llx filter=%d flags=%#x fflags=%#x data=%#llx udata=%#llx\n"
                        .as_ptr(),
                    ocerz_ld(event, 8) as libc::c_ulonglong,
                    ocerz_ld(event + 8, 2) as u16 as i16 as c_int,
                    ocerz_ld(event + 10, 2) as u16 as c_uint,
                    ocerz_ld(event + 12, 4) as u32,
                    ocerz_ld(event + 16, 8) as libc::c_ulonglong,
                    ocerz_ld(event + 24, 8) as libc::c_ulonglong,
                );
            }
            for index in 0..r as usize {
                let event = orig[3].wrapping_add(index as u64 * 32);
                let flags = ocerz_ld(event + 10, 2) as u16;
                let filter = ocerz_ld(event + 8, 2) as u16 as i16;
                if flags & 0x8000 == 0 || filter != -1 {
                    continue;
                }
                let fd = ocerz_ld(event, 8) as c_int;
                let status = libc::fcntl(fd, libc::F_GETFL);
                let mut queued = -1;
                libc::ioctl(fd, libc::FIONREAD, &mut queued);
                let mut stat: libc::stat = mem::zeroed();
                let stat_ok = libc::fstat(fd, &mut stat);
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: KEVGUARD[%d] EOF ident=%d udata=%#llx data=%#llx getfl=%#x qread=%d mode=%#x ino=%llu\n".as_ptr(),
                    libc::getpid(),
                    fd,
                    ocerz_ld(event + 24, 8) as libc::c_ulonglong,
                    ocerz_ld(event + 16, 8) as libc::c_ulonglong,
                    status,
                    queued,
                    if stat_ok == 0 { stat.st_mode as c_uint } else { 0 },
                    if stat_ok == 0 { stat.st_ino as libc::c_ulonglong } else { 0 },
                );
            }
        }
    }
    ocerz_sysfail_note(cpu, num, if err != 0 { r as c_int } else { 0 }, &orig);
    if err == 0 && matches!(num, 336 | 545) {
        proc_info_self_fixup(num, &orig);
    }

    static mut ROBUST: c_int = -1;
    if ROBUST < 0 {
        ROBUST = if libc::getenv(c"OCERZ_NO_ROBUST_ULOCK".as_ptr()).is_null() {
            1
        } else {
            0
        };
    }
    if ROBUST != 0
        && matches!(num, 515 | 544)
        && orig[0] & 0xff == 2
        && ((err != 0 && r as c_int == 105) || (err == 0 && r as i64 == -105))
        && a[1] != 0
    {
        let word = a[1] as *mut u32;
        let _ = core::sync::atomic::AtomicU32::from_ptr(word).compare_exchange(
            orig[2] as u32,
            0,
            core::sync::atomic::Ordering::Release,
            core::sync::atomic::Ordering::Relaxed,
        );
        if env_set!("OCERZ_ULOCKLOG") {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: ROBUST-ULOCK[%d] broke dead-owner lock addr=%#llx owner=%#llx\n".as_ptr(),
                libc::getpid(),
                orig[1] as libc::c_ulonglong,
                orig[2] as libc::c_ulonglong,
            );
        }
        err = 0;
        r = (-4i64) as u64;
    }

    static mut OPENLOG: *const c_char = -1isize as *const c_char;
    if OPENLOG == -1isize as *const c_char {
        OPENLOG = libc::getenv(c"OCERZ_OPENLOG".as_ptr());
    }
    if !OPENLOG.is_null() && matches!(num, 5 | 398 | 463) {
        let pp = if num == 463 { a[1] } else { a[0] };
        let path = pp as *const c_char;
        if !path.is_null() && (*OPENLOG == 0 || !libc::strstr(path, OPENLOG).is_null()) {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: OPEN[%d] %s -> r=%llu err=%d\n".as_ptr(),
                libc::getpid(),
                path,
                r as libc::c_ulonglong,
                err,
            );
        }
    }
    static mut PREADLOG: libc::c_long = -2;
    if PREADLOG == -2 {
        let value = libc::getenv(c"OCERZ_PREADLOG".as_ptr());
        PREADLOG = if value.is_null() {
            -1
        } else {
            libc::strtol(value, ptr::null_mut(), 0)
        };
    }
    if PREADLOG >= 0 && matches!(num, 153 | 3) && orig[2] == PREADLOG as u64 {
        let bytes = if crate::ffi::ocerz_addr_readable(orig[1]) != 0 {
            ocerz_g2h(orig[1]).cast::<u8>()
        } else {
            ptr::null()
        };
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: PREAD[%d] cpu#%u num=%d fd=%d off=%#llx len=%llu -> r=%lld err=%d bytes=%02x%02x%02x%02x%02x%02x%02x%02x %.16s\n"
                .as_ptr(),
            libc::getpid(),
            (*cpu).cpu_number,
            num,
            orig[0] as c_int,
            orig[3] as libc::c_ulonglong,
            orig[2] as libc::c_ulonglong,
            r as i64,
            if err != 0 { r as c_int } else { 0 },
            if bytes.is_null() { 0 } else { *bytes.add(0) as c_int },
            if bytes.is_null() { 0 } else { *bytes.add(1) as c_int },
            if bytes.is_null() { 0 } else { *bytes.add(2) as c_int },
            if bytes.is_null() { 0 } else { *bytes.add(3) as c_int },
            if bytes.is_null() { 0 } else { *bytes.add(4) as c_int },
            if bytes.is_null() { 0 } else { *bytes.add(5) as c_int },
            if bytes.is_null() { 0 } else { *bytes.add(6) as c_int },
            if bytes.is_null() { 0 } else { *bytes.add(7) as c_int },
            if bytes.is_null() {
                c"".as_ptr()
            } else {
                bytes.cast::<c_char>()
            },
        );
    }
    static mut FDLOG: libc::c_long = -2;
    if FDLOG == -2 {
        let value = libc::getenv(c"OCERZ_FDLOG".as_ptr());
        FDLOG = if value.is_null() {
            -1
        } else {
            libc::strtol(value, ptr::null_mut(), 0)
        };
    }
    if FDLOG >= 0
        && matches!(num, 3 | 4 | 153 | 154 | 189 | 339 | 199 | 95 | 6)
        && orig[0] as i64 >= FDLOG as i64
        && (orig[0] as i64) < 4096
    {
        let size = if matches!(num, 189 | 339)
            && err == 0
            && crate::ffi::ocerz_addr_readable(orig[1].wrapping_add(104)) != 0
        {
            ocerz_ld(orig[1].wrapping_add(96), 8) as i64
        } else {
            -1
        };
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: FDLOG[%d] cpu#%u %s fd=%d a1=%#llx a2=%#llx a3=%#llx -> r=%lld err=%d%s%lld\n"
                .as_ptr(),
            libc::getpid(),
            (*cpu).cpu_number,
            match num {
                3 => c"read".as_ptr(),
                4 => c"write".as_ptr(),
                153 => c"pread".as_ptr(),
                154 => c"pwrite".as_ptr(),
                189 => c"fstat".as_ptr(),
                339 => c"fstat64".as_ptr(),
                199 => c"lseek".as_ptr(),
                95 => c"fsync".as_ptr(),
                _ => c"close".as_ptr(),
            },
            orig[0] as c_int,
            orig[1] as libc::c_ulonglong,
            orig[2] as libc::c_ulonglong,
            orig[3] as libc::c_ulonglong,
            r as i64,
            if err != 0 { r as c_int } else { 0 },
            if size >= 0 {
                c" st_size=".as_ptr()
            } else {
                c"".as_ptr()
            },
            if size >= 0 { size } else { 0 },
        );
    }
    static mut KICKLOG: c_int = -1;
    if KICKLOG < 0 {
        KICKLOG = c_int::from(!libc::getenv(c"OCERZ_KICKLOG".as_ptr()).is_null());
    }
    if KICKLOG != 0 && err != 0 && r == 4 {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: KICKRET[%d] cpu#%u bsd %d -> EINTR rip=%#llx\n".as_ptr(),
            libc::getpid(),
            (*cpu).cpu_number,
            num,
            (*cpu).rip as libc::c_ulonglong,
        );
    }
    static mut PIPELOG: c_int = -1;
    if PIPELOG < 0 {
        PIPELOG = c_int::from(!libc::getenv(c"OCERZ_PIPELOG".as_ptr()).is_null());
    }
    if PIPELOG != 0 && matches!(num, 3 | 4 | 396 | 397) {
        let is_write = matches!(num, 4 | 397) && orig[2] <= 8;
        let is_read = matches!(num, 3 | 396) && orig[2] <= 512;
        if is_write || is_read {
            let mut stat: libc::stat = mem::zeroed();
            if libc::fstat(orig[0] as c_int, &mut stat) == 0
                && stat.st_mode & libc::S_IFMT == libc::S_IFIFO
            {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: PIPE%s[%d] cpu#%u fd=%lld len=%lld -> r=%lld err=%d ic=%#llx\n"
                        .as_ptr(),
                    if is_write {
                        c"WR".as_ptr()
                    } else {
                        c"RD".as_ptr()
                    },
                    libc::getpid(),
                    (*cpu).cpu_number,
                    orig[0] as libc::c_longlong,
                    orig[2] as libc::c_longlong,
                    r as libc::c_longlong,
                    err,
                    (*vm).insn_count as libc::c_ulonglong,
                );
            }
        }
    }
    if env_set!("OCERZ_SLOWBSD") {
        let elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW).wrapping_sub(blocklog_start);
        if elapsed > 1_500_000_000 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: SLOWBSD[%d] cpu#%u num=%d(%s) %llums -> r=%llu err=%d\n".as_ptr(),
                libc::getpid(),
                (*cpu).cpu_number,
                num,
                (*entry).name,
                (elapsed / 1_000_000) as libc::c_ulonglong,
                r as libc::c_ulonglong,
                err,
            );
        }
    }
    if PIPELOG != 0 && num == 363 && r as i64 != 0 {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: PIPEKEV[%d] cpu#%u kq=%lld -> r=%lld err=%d ic=%#llx\n".as_ptr(),
            libc::getpid(),
            (*cpu).cpu_number,
            orig[0] as i64,
            r as i64,
            err,
            (*vm).insn_count as libc::c_ulonglong,
        );
    }
    if btrack {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: BLK-OUT[%d] seq=%llu num=%d r=%lld err=%d\n".as_ptr(),
            libc::getpid(),
            bseq as libc::c_ulonglong,
            num,
            r as i64,
            err,
        );
    }
    if err != 0 {
        ret_err(cpu, r);
    } else if (*entry).dual_ret != 0 {
        ret_ok2(cpu, r, ret2);
    } else {
        ret_ok(cpu, r);
    }
    strace_bsd(vm, entry, num, &orig, cpu);
    crate::ffi::OCERZ_STEP_OK as c_int
}

unsafe fn sys_preadv_pwritev(
    _vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    a: *mut [u64; 8],
    num: c_int,
) -> c_int {
    let args = &*a;
    let mut cnt = args[2];
    if cnt > OCERZ_IOV_MAX as u64 {
        cnt = OCERZ_IOV_MAX as u64;
    }
    let mut scratch = [ocerz_iovec {
        iov_base: 0,
        iov_len: 0,
    }; OCERZ_IOV_MAX];
    for i in 0..cnt {
        let addr = args[1].wrapping_add(i.wrapping_mul(16));
        let base = ocerz_ld(addr, 8);
        let entry = scratch.as_mut_ptr().add(i as usize);
        (*entry).iov_base = if base != 0 { ocerz_g2h(base) as u64 } else { 0 };
        (*entry).iov_len = ocerz_ld(addr.wrapping_add(8), 8);
    }
    let mut fa = [
        args[0],
        scratch.as_mut_ptr() as u64,
        args[2],
        args[3],
        0,
        0,
        0,
        0,
    ];
    forward_with_scratch(cpu, num, &mut fa, 0);
    crate::ffi::OCERZ_STEP_OK as c_int
}

unsafe fn sys_preadv(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    sys_preadv_pwritev(_vm, cpu, a, 540)
}
unsafe fn sys_pwritev(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    sys_preadv_pwritev(_vm, cpu, a, 541)
}
unsafe fn sys_preadv_nc(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    sys_preadv_pwritev(_vm, cpu, a, 542)
}
unsafe fn sys_pwritev_nc(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    sys_preadv_pwritev(_vm, cpu, a, 543)
}

const EMPTY_BSD_ENTRY: ocerz_bsd_entry = ocerz_bsd_entry {
    name: ptr::null(),
    nargs: 0,
    ptr_mask: 0,
    dual_ret: 0,
    intercept: None,
};

const fn make_bsd_entry(
    name: &'static [u8],
    nargs: u8,
    ptr_mask: u8,
    dual_ret: u8,
    intercept: Option<ocerz_bsd_fn>,
) -> ocerz_bsd_entry {
    ocerz_bsd_entry {
        name: name.as_ptr().cast::<c_char>(),
        nargs,
        ptr_mask,
        dual_ret,
        intercept,
    }
}

macro_rules! bsd_entry {
    ($name:expr, $nargs:expr, $ptr_mask:expr, $dual_ret:expr, $intercept:expr) => {
        make_bsd_entry($name, $nargs, $ptr_mask, $dual_ret, $intercept)
    };
}

const fn make_bsd_table() -> [ocerz_bsd_entry; OCERZ_BSD_MAX as usize] {
    let mut table = [EMPTY_BSD_ENTRY; OCERZ_BSD_MAX as usize];
    table[1] = bsd_entry!(
        b"exit\0",
        1,
        0x00,
        0,
        Some(super::mem::sys_exit as ocerz_bsd_fn)
    );
    table[2] = bsd_entry!(
        b"fork\0",
        0,
        0x00,
        0,
        Some(super::workers::sys_fork as ocerz_bsd_fn)
    );
    table[3] = bsd_entry!(b"read\0", 3, 0x02, 0, None);
    table[4] = bsd_entry!(b"write\0", 3, 0x02, 0, None);
    table[5] = bsd_entry!(b"open\0", 3, 0x01, 0, None);
    table[6] = bsd_entry!(b"close\0", 1, 0x00, 0, None);
    table[7] = bsd_entry!(b"wait4\0", 4, 0x0a, 0, None);
    table[10] = bsd_entry!(b"unlink\0", 1, 0x01, 0, None);
    table[12] = bsd_entry!(b"chdir\0", 1, 0x01, 0, None);
    table[13] = bsd_entry!(b"fchdir\0", 1, 0x00, 0, None);
    table[15] = bsd_entry!(b"chmod\0", 3, 0x01, 0, None);
    table[16] = bsd_entry!(b"chown\0", 3, 0x01, 0, None);
    table[20] = bsd_entry!(b"getpid\0", 0, 0x00, 0, None);
    table[23] = bsd_entry!(b"setuid\0", 1, 0x00, 0, None);
    table[24] = bsd_entry!(b"getuid\0", 0, 0x00, 0, None);
    table[25] = bsd_entry!(b"geteuid\0", 0, 0x00, 0, None);
    table[26] = bsd_entry!(
        b"ptrace\0",
        4,
        0x00,
        0,
        Some(super::mem::sys_unsupported as ocerz_bsd_fn)
    );
    table[27] = bsd_entry!(b"recvmsg\0", 3, 0x00, 0, Some(sys_recvmsg as ocerz_bsd_fn));
    table[28] = bsd_entry!(b"sendmsg\0", 3, 0x00, 0, Some(sys_sendmsg as ocerz_bsd_fn));
    table[29] = bsd_entry!(b"recvfrom\0", 6, 0x32, 0, None);
    table[30] = bsd_entry!(b"accept\0", 3, 0x06, 0, None);
    table[31] = bsd_entry!(b"getpeername\0", 3, 0x06, 0, None);
    table[32] = bsd_entry!(b"getsockname\0", 3, 0x06, 0, None);
    table[33] = bsd_entry!(b"access\0", 2, 0x01, 0, None);
    table[34] = bsd_entry!(b"chflags\0", 2, 0x01, 0, None);
    table[35] = bsd_entry!(b"fchflags\0", 2, 0x00, 0, None);
    table[36] = bsd_entry!(b"sync\0", 0, 0x00, 0, None);
    table[37] = bsd_entry!(b"kill\0", 2, 0x00, 0, None);
    table[39] = bsd_entry!(b"getppid\0", 0, 0x00, 0, None);
    table[41] = bsd_entry!(b"dup\0", 1, 0x00, 0, None);
    table[42] = bsd_entry!(b"pipe\0", 0, 0x00, 1, None);
    table[43] = bsd_entry!(b"getegid\0", 0, 0x00, 0, None);
    table[46] = bsd_entry!(
        b"sigaction\0",
        3,
        0x06,
        0,
        Some(super::signals::sys_sigaction as ocerz_bsd_fn)
    );
    table[47] = bsd_entry!(b"getgid\0", 0, 0x00, 0, None);
    table[48] = bsd_entry!(
        b"sigprocmask\0",
        3,
        0x06,
        0,
        Some(super::signals::sys_sigprocmask as ocerz_bsd_fn)
    );
    table[49] = bsd_entry!(b"getlogin\0", 2, 0x01, 0, None);
    table[50] = bsd_entry!(b"setlogin\0", 1, 0x01, 0, None);
    table[52] = bsd_entry!(
        b"sigpending\0",
        1,
        0x01,
        0,
        Some(super::signals::sys_sigpending as ocerz_bsd_fn)
    );
    table[53] = bsd_entry!(
        b"sigaltstack\0",
        2,
        0x03,
        0,
        Some(super::signals::sys_sigaltstack as ocerz_bsd_fn)
    );
    table[184] = bsd_entry!(
        b"sigreturn\0",
        2,
        0x00,
        0,
        Some(super::signals::sys_sigreturn as ocerz_bsd_fn)
    );
    table[54] = bsd_entry!(b"ioctl\0", 3, 0x04, 0, Some(sys_ioctl as ocerz_bsd_fn));
    table[57] = bsd_entry!(b"symlink\0", 2, 0x03, 0, None);
    table[58] = bsd_entry!(b"readlink\0", 3, 0x03, 0, None);
    table[59] = bsd_entry!(
        b"execve\0",
        3,
        0x00,
        0,
        Some(super::spawn::sys_execve as ocerz_bsd_fn)
    );
    table[60] = bsd_entry!(b"umask\0", 1, 0x00, 0, None);
    table[65] = bsd_entry!(b"msync\0", 3, 0x01, 0, None);
    table[73] = bsd_entry!(
        b"munmap\0",
        2,
        0x00,
        0,
        Some(super::mem::sys_munmap as ocerz_bsd_fn)
    );
    table[74] = bsd_entry!(
        b"mprotect\0",
        3,
        0x00,
        0,
        Some(super::mem::sys_mprotect as ocerz_bsd_fn)
    );
    table[75] = bsd_entry!(
        b"madvise\0",
        3,
        0x00,
        0,
        Some(super::mem::sys_madvise as ocerz_bsd_fn)
    );
    table[79] = bsd_entry!(b"getgroups\0", 2, 0x02, 0, None);
    table[80] = bsd_entry!(b"setgroups\0", 2, 0x02, 0, None);
    table[81] = bsd_entry!(b"getpgrp\0", 0, 0x00, 0, None);
    table[82] = bsd_entry!(b"setpgid\0", 2, 0x00, 0, None);
    table[89] = bsd_entry!(b"getdtablesize\0", 0, 0x00, 0, None);
    table[90] = bsd_entry!(b"dup2\0", 2, 0x00, 0, None);
    table[92] = bsd_entry!(b"fcntl\0", 3, 0x00, 0, Some(sys_fcntl as ocerz_bsd_fn));
    table[93] = bsd_entry!(b"select\0", 5, 0x1e, 0, None);
    table[95] = bsd_entry!(b"fsync\0", 1, 0x00, 0, None);
    table[96] = bsd_entry!(b"setpriority\0", 3, 0x00, 0, None);
    table[97] = bsd_entry!(b"socket\0", 3, 0x00, 0, None);
    table[98] = bsd_entry!(b"connect\0", 3, 0x02, 0, None);
    table[100] = bsd_entry!(b"getpriority\0", 2, 0x00, 0, None);
    table[104] = bsd_entry!(b"bind\0", 3, 0x02, 0, None);
    table[105] = bsd_entry!(b"setsockopt\0", 5, 0x08, 0, None);
    table[106] = bsd_entry!(b"listen\0", 2, 0x00, 0, None);
    table[116] = bsd_entry!(
        b"gettimeofday\0",
        3,
        0x07,
        0,
        Some(super::machmsg::sys_gettimeofday as ocerz_bsd_fn)
    );
    table[117] = bsd_entry!(b"getrusage\0", 2, 0x02, 0, None);
    table[118] = bsd_entry!(b"getsockopt\0", 5, 0x18, 0, None);
    table[120] = bsd_entry!(b"readv\0", 3, 0x00, 0, Some(sys_readv as ocerz_bsd_fn));
    table[121] = bsd_entry!(b"writev\0", 3, 0x00, 0, Some(sys_writev as ocerz_bsd_fn));
    table[123] = bsd_entry!(b"fchown\0", 3, 0x00, 0, None);
    table[124] = bsd_entry!(b"fchmod\0", 2, 0x00, 0, None);
    table[128] = bsd_entry!(b"rename\0", 2, 0x03, 0, None);
    table[139] = bsd_entry!(b"futimes\0", 2, 0x02, 0, None);
    table[133] = bsd_entry!(b"sendto\0", 6, 0x12, 0, None);
    table[134] = bsd_entry!(b"shutdown\0", 2, 0x00, 0, None);
    table[135] = bsd_entry!(b"socketpair\0", 4, 0x08, 0, None);
    table[136] = bsd_entry!(b"mkdir\0", 2, 0x01, 0, None);
    table[137] = bsd_entry!(b"rmdir\0", 1, 0x01, 0, None);
    table[138] = bsd_entry!(b"utimes\0", 2, 0x03, 0, None);
    table[140] = bsd_entry!(b"adjtime\0", 2, 0x03, 0, None);
    table[142] = bsd_entry!(b"gethostuuid\0", 2, 0x03, 0, None);
    table[147] = bsd_entry!(b"setsid\0", 0, 0x00, 0, None);
    table[151] = bsd_entry!(b"getpgid\0", 1, 0x00, 0, None);
    table[153] = bsd_entry!(b"pread\0", 4, 0x02, 0, None);
    table[154] = bsd_entry!(b"pwrite\0", 4, 0x02, 0, None);
    table[169] = bsd_entry!(b"csops\0", 4, 0x04, 0, None);
    table[170] = bsd_entry!(b"csops_audittoken\0", 5, 0x14, 0, None);
    table[180] = bsd_entry!(b"kdebug_trace\0", 5, 0x00, 0, None);
    table[191] = bsd_entry!(b"pathconf\0", 2, 0x01, 0, None);
    table[192] = bsd_entry!(b"fpathconf\0", 2, 0x00, 0, None);
    table[194] = bsd_entry!(b"getrlimit\0", 2, 0x02, 0, None);
    table[195] = bsd_entry!(b"setrlimit\0", 2, 0x02, 0, None);
    table[197] = bsd_entry!(
        b"mmap\0",
        6,
        0x00,
        0,
        Some(super::mem::sys_mmap as ocerz_bsd_fn)
    );
    table[199] = bsd_entry!(b"lseek\0", 3, 0x00, 0, None);
    table[200] = bsd_entry!(b"truncate\0", 2, 0x01, 0, None);
    table[201] = bsd_entry!(b"ftruncate\0", 2, 0x00, 0, None);
    table[202] = bsd_entry!(
        b"sysctl\0",
        6,
        0x1d,
        0,
        Some(super::sysctl::sys_sysctl as ocerz_bsd_fn)
    );
    table[216] = bsd_entry!(b"open_dprotected_np\0", 5, 0x01, 0, None);
    table[220] = bsd_entry!(b"getattrlist\0", 5, 0x07, 0, None);
    table[228] = bsd_entry!(b"fgetattrlist\0", 5, 0x06, 0, None);
    table[229] = bsd_entry!(b"fsetattrlist\0", 5, 0x06, 0, None);
    table[230] = bsd_entry!(b"poll\0", 3, 0x01, 0, None);
    table[234] = bsd_entry!(b"getxattr\0", 6, 0x07, 0, None);
    table[466] = bsd_entry!(b"faccessat\0", 4, 0x02, 0, None);
    table[476] = bsd_entry!(b"getattrlistat\0", 6, 0x0e, 0, None);
    table[235] = bsd_entry!(b"fgetxattr\0", 6, 0x06, 0, None);
    table[236] = bsd_entry!(b"setxattr\0", 6, 0x07, 0, None);
    table[237] = bsd_entry!(b"fsetxattr\0", 6, 0x06, 0, None);
    table[238] = bsd_entry!(b"removexattr\0", 3, 0x03, 0, None);
    table[239] = bsd_entry!(b"fremovexattr\0", 3, 0x02, 0, None);
    table[240] = bsd_entry!(b"listxattr\0", 4, 0x03, 0, None);
    table[241] = bsd_entry!(b"flistxattr\0", 4, 0x02, 0, None);
    table[244] = bsd_entry!(
        b"posix_spawn\0",
        5,
        0x00,
        0,
        Some(super::spawn::sys_posix_spawn as ocerz_bsd_fn)
    );
    table[266] = bsd_entry!(b"shm_open\0", 3, 0x01, 0, None);
    table[267] = bsd_entry!(b"shm_unlink\0", 1, 0x01, 0, None);
    table[268] = bsd_entry!(b"sem_open\0", 4, 0x01, 0, None);
    table[269] = bsd_entry!(b"sem_close\0", 1, 0x00, 0, None);
    table[270] = bsd_entry!(b"sem_unlink\0", 1, 0x01, 0, None);
    table[271] = bsd_entry!(b"sem_wait\0", 1, 0x00, 0, None);
    table[272] = bsd_entry!(b"sem_trywait\0", 1, 0x00, 0, None);
    table[273] = bsd_entry!(b"sem_post\0", 1, 0x00, 0, None);
    table[274] = bsd_entry!(
        b"sysctlbyname\0",
        6,
        0x1d,
        0,
        Some(super::sysctl::sys_sysctlbyname as ocerz_bsd_fn)
    );
    table[381] = bsd_entry!(
        b"__mac_syscall\0",
        3,
        0x05,
        0,
        Some(super::sysctl::sys_mac_syscall as ocerz_bsd_fn)
    );
    table[483] = bsd_entry!(b"csrctl\0", 3, 0x02, 0, None);
    table[441] = bsd_entry!(b"guarded_open_np\0", 5, 0x03, 0, None);
    table[442] = bsd_entry!(b"guarded_close_np\0", 2, 0x02, 0, None);
    table[443] = bsd_entry!(b"guarded_kqueue_np\0", 2, 0x01, 0, None);
    table[444] = bsd_entry!(b"change_fdguard_np\0", 6, 0x2a, 0, None);
    table[484] = bsd_entry!(b"guarded_open_dprotected_np\0", 7, 0x03, 0, None);
    table[486] = bsd_entry!(b"guarded_pwrite_np\0", 5, 0x06, 0, None);
    table[131] = bsd_entry!(b"flock\0", 2, 0x00, 0, None);
    table[501] = bsd_entry!(b"necp_open\0", 1, 0x00, 0, None);
    table[502] = bsd_entry!(b"necp_client_action\0", 6, 0x14, 0, None);
    table[494] = bsd_entry!(b"persona\0", 5, 0x1c, 0, None);
    table[524] = bsd_entry!(b"setattrlistat\0", 6, 0x0e, 0, None);
    table[521] = bsd_entry!(
        b"abort_with_payload\0",
        6,
        0x04,
        0,
        Some(super::mem::sys_abort_payload as ocerz_bsd_fn)
    );
    table[283] = bsd_entry!(b"fchmod_extended\0", 5, 0x10, 0, None);
    table[286] = bsd_entry!(b"gettid\0", 2, 0x03, 0, None);
    table[294] = bsd_entry!(
        b"shared_region_check_np\0",
        1,
        0x01,
        0,
        Some(super::mem::sys_shared_region_check_np as ocerz_bsd_fn)
    );
    table[322] = bsd_entry!(b"iopolicysys\0", 2, 0x02, 0, None);
    table[327] = bsd_entry!(b"issetugid\0", 0, 0x00, 0, None);
    table[336] = bsd_entry!(b"proc_info\0", 6, 0x10, 0, None);
    table[338] = bsd_entry!(b"stat64\0", 2, 0x03, 0, None);
    table[339] = bsd_entry!(b"fstat64\0", 2, 0x02, 0, None);
    table[340] = bsd_entry!(b"lstat64\0", 2, 0x03, 0, None);
    table[341] = bsd_entry!(b"stat64_extended\0", 4, 0x0f, 0, None);
    table[342] = bsd_entry!(b"lstat64_extended\0", 4, 0x0f, 0, None);
    table[343] = bsd_entry!(b"fstat64_extended\0", 4, 0x0e, 0, None);
    table[344] = bsd_entry!(b"getdirentries64\0", 4, 0x0a, 0, None);
    table[345] = bsd_entry!(b"statfs64\0", 2, 0x03, 0, None);
    table[346] = bsd_entry!(b"fstatfs64\0", 2, 0x02, 0, None);
    table[347] = bsd_entry!(b"getfsstat64\0", 3, 0x01, 0, None);
    table[350] = bsd_entry!(b"audit\0", 2, 0x01, 0, None);
    table[351] = bsd_entry!(b"auditon\0", 3, 0x02, 0, None);
    table[353] = bsd_entry!(b"getauid\0", 1, 0x01, 0, None);
    table[357] = bsd_entry!(b"getaudit_addr\0", 2, 0x01, 0, None);
    table[358] = bsd_entry!(b"setaudit_addr\0", 2, 0x01, 0, None);
    table[359] = bsd_entry!(b"auditctl\0", 1, 0x01, 0, None);
    table[360] = bsd_entry!(
        b"bsdthread_create\0",
        5,
        0x00,
        0,
        Some(super::threads::sys_bsdthread_create as ocerz_bsd_fn)
    );
    table[361] = bsd_entry!(
        b"bsdthread_terminate\0",
        4,
        0x00,
        0,
        Some(super::threads::sys_bsdthread_terminate as ocerz_bsd_fn)
    );
    table[362] = bsd_entry!(b"kqueue\0", 0, 0x00, 0, None);
    table[363] = bsd_entry!(b"kevent\0", 6, 0x2a, 0, None);
    table[366] = bsd_entry!(
        b"bsdthread_register\0",
        7,
        0x00,
        0,
        Some(super::sysctl::sys_bsdthread_register as ocerz_bsd_fn)
    );
    table[367] = bsd_entry!(
        b"workq_open\0",
        0,
        0x00,
        0,
        Some(super::sysctl::sys_workq_stub as ocerz_bsd_fn)
    );
    table[368] = bsd_entry!(
        b"workq_kernreturn\0",
        4,
        0x00,
        0,
        Some(super::hostwq::sys_workq_kernreturn as ocerz_bsd_fn)
    );
    table[372] = bsd_entry!(b"thread_selfid\0", 0, 0x00, 0, None);
    table[328] = bsd_entry!(
        b"__pthread_kill\0",
        2,
        0x00,
        0,
        Some(super::signals::sys_pthread_kill as ocerz_bsd_fn)
    );
    table[329] = bsd_entry!(
        b"__pthread_sigmask\0",
        3,
        0x06,
        0,
        Some(super::signals::sys_sigprocmask as ocerz_bsd_fn)
    );
    table[331] = bsd_entry!(
        b"__disable_threadsignal\0",
        1,
        0x00,
        0,
        Some(super::sysctl::sys_workq_stub as ocerz_bsd_fn)
    );
    table[332] = bsd_entry!(
        b"__pthread_markcancel\0",
        1,
        0x00,
        0,
        Some(super::sysctl::sys_workq_stub as ocerz_bsd_fn)
    );
    table[333] = bsd_entry!(b"__pthread_canceled\0", 1, 0x00, 0, None);
    table[334] = bsd_entry!(b"__semwait_signal\0", 6, 0x00, 0, None);
    table[374] = bsd_entry!(
        b"kevent_qos\0",
        8,
        0x00,
        0,
        Some(super::hostwq::sys_kevent_qos as ocerz_bsd_fn)
    );
    table[375] = bsd_entry!(
        b"kevent_id\0",
        6,
        0x00,
        0,
        Some(super::hostwq::sys_kevent_id as ocerz_bsd_fn)
    );
    table[406] = bsd_entry!(
        b"fcntl_nocancel\0",
        3,
        0x00,
        0,
        Some(sys_fcntl as ocerz_bsd_fn)
    );
    table[409] = bsd_entry!(b"connect_nocancel\0", 3, 0x02, 0, None);
    table[478] = bsd_entry!(
        b"bsdthread_ctl\0",
        4,
        0x00,
        0,
        Some(super::sysctl::sys_workq_stub as ocerz_bsd_fn)
    );
    table[515] = bsd_entry!(b"ulock_wait\0", 4, 0x02, 0, None);
    table[516] = bsd_entry!(b"ulock_wake\0", 3, 0x02, 0, None);
    table[544] = bsd_entry!(b"ulock_wait2\0", 5, 0x02, 0, None);
    table[301] = bsd_entry!(b"psynch_mutexwait\0", 5, 0x01, 0, None);
    table[302] = bsd_entry!(b"psynch_mutexdrop\0", 5, 0x01, 0, None);
    table[303] = bsd_entry!(b"psynch_cvbroad\0", 7, 0x11, 0, None);
    table[304] = bsd_entry!(b"psynch_cvsignal\0", 7, 0x11, 0, None);
    table[305] = bsd_entry!(b"psynch_cvwait\0", 8, 0x09, 0, None);
    table[297] = bsd_entry!(b"psynch_rw_longrdlock\0", 5, 0x01, 0, None);
    table[298] = bsd_entry!(b"psynch_rw_yieldwrlock\0", 5, 0x01, 0, None);
    table[299] = bsd_entry!(b"psynch_rw_downgrade\0", 5, 0x01, 0, None);
    table[300] = bsd_entry!(b"psynch_rw_upgrade\0", 5, 0x01, 0, None);
    table[306] = bsd_entry!(b"psynch_rw_rdlock\0", 5, 0x01, 0, None);
    table[307] = bsd_entry!(b"psynch_rw_wrlock\0", 5, 0x01, 0, None);
    table[308] = bsd_entry!(b"psynch_rw_unlock\0", 5, 0x01, 0, None);
    table[309] = bsd_entry!(b"psynch_rw_unlock2\0", 5, 0x01, 0, None);
    table[310] = bsd_entry!(b"getsid\0", 1, 0x00, 0, None);
    table[312] = bsd_entry!(b"psynch_cvclrprepost\0", 7, 0x01, 0, None);
    table[313] = bsd_entry!(b"aio_fsync\0", 2, 0x02, 0, None);
    table[529] = bsd_entry!(b"os_fault_with_payload\0", 6, 0x14, 0, None);
    table[394] = bsd_entry!(b"pselect\0", 6, 0x3e, 0, None);
    table[530] = bsd_entry!(b"kqueue_workloop_ctl\0", 4, 0x04, 0, None);
    table[395] = bsd_entry!(b"pselect_nocancel\0", 6, 0x3e, 0, None);
    table[396] = bsd_entry!(b"read_nocancel\0", 3, 0x02, 0, None);
    table[397] = bsd_entry!(b"write_nocancel\0", 3, 0x02, 0, None);
    table[398] = bsd_entry!(b"open_nocancel\0", 3, 0x01, 0, None);
    table[399] = bsd_entry!(b"close_nocancel\0", 1, 0x00, 0, None);
    table[401] = bsd_entry!(
        b"recvmsg_nocancel\0",
        3,
        0x00,
        0,
        Some(sys_recvmsg_nocancel as ocerz_bsd_fn)
    );
    table[402] = bsd_entry!(
        b"sendmsg_nocancel\0",
        3,
        0x00,
        0,
        Some(sys_sendmsg_nocancel as ocerz_bsd_fn)
    );
    table[403] = bsd_entry!(b"recvfrom_nocancel\0", 6, 0x32, 0, None);
    table[404] = bsd_entry!(b"accept_nocancel\0", 3, 0x06, 0, None);
    table[407] = bsd_entry!(b"select_nocancel\0", 5, 0x1e, 0, None);
    table[413] = bsd_entry!(b"sendto_nocancel\0", 6, 0x12, 0, None);
    table[414] = bsd_entry!(b"pread_nocancel\0", 4, 0x02, 0, None);
    table[415] = bsd_entry!(b"pwrite_nocancel\0", 4, 0x02, 0, None);
    table[411] = bsd_entry!(
        b"readv_nocancel\0",
        3,
        0x00,
        0,
        Some(sys_readv as ocerz_bsd_fn)
    );
    table[412] = bsd_entry!(
        b"writev_nocancel\0",
        3,
        0x00,
        0,
        Some(sys_writev as ocerz_bsd_fn)
    );
    table[417] = bsd_entry!(b"poll_nocancel\0", 3, 0x01, 0, None);
    table[420] = bsd_entry!(b"sem_wait_nocancel\0", 1, 0x00, 0, None);
    table[423] = bsd_entry!(b"__semwait_signal_nocancel\0", 6, 0x00, 0, None);
    table[427] = bsd_entry!(b"fsgetpath\0", 4, 0x05, 0, None);
    table[428] = bsd_entry!(b"audit_session_self\0", 0, 0x00, 0, None);
    table[430] = bsd_entry!(b"fileport_makeport\0", 2, 0x02, 0, None);
    table[431] = bsd_entry!(b"fileport_makefd\0", 1, 0x00, 0, None);
    table[83] = bsd_entry!(b"setitimer\0", 3, 0x06, 0, None);
    table[86] = bsd_entry!(b"getitimer\0", 2, 0x02, 0, None);
    table[539] = bsd_entry!(b"task_read_for_pid\0", 3, 0x04, 0, None);
    table[538] = bsd_entry!(b"task_inspect_for_pid\0", 3, 0x04, 0, None);
    table[188] = bsd_entry!(b"stat\0", 2, 0x03, 0, None);
    table[189] = bsd_entry!(b"fstat\0", 2, 0x02, 0, None);
    table[190] = bsd_entry!(b"lstat\0", 2, 0x03, 0, None);
    table[157] = bsd_entry!(b"statfs\0", 2, 0x03, 0, None);
    table[158] = bsd_entry!(b"fstatfs\0", 2, 0x02, 0, None);
    table[18] = bsd_entry!(b"getfsstat\0", 3, 0x01, 0, None);
    table[196] = bsd_entry!(b"getdirentries\0", 4, 0x0a, 0, None);
    table[469] = bsd_entry!(b"fstatat\0", 4, 0x06, 0, None);
    table[9] = bsd_entry!(b"link\0", 2, 0x03, 0, None);
    table[14] = bsd_entry!(b"mknod\0", 3, 0x01, 0, None);
    table[61] = bsd_entry!(b"chroot\0", 1, 0x01, 0, None);
    table[364] = bsd_entry!(b"lchown\0", 3, 0x01, 0, None);
    table[471] = bsd_entry!(b"linkat\0", 5, 0x0a, 0, None);
    table[474] = bsd_entry!(b"symlinkat\0", 3, 0x05, 0, None);
    table[553] = bsd_entry!(b"mkfifoat\0", 3, 0x02, 0, None);
    table[554] = bsd_entry!(b"mknodat\0", 4, 0x02, 0, None);
    table[187] = bsd_entry!(b"fdatasync\0", 1, 0x00, 0, None);
    table[400] = bsd_entry!(b"wait4_nocancel\0", 4, 0x0a, 0, None);
    table[405] = bsd_entry!(b"msync_nocancel\0", 3, 0x01, 0, None);
    table[408] = bsd_entry!(b"fsync_nocancel\0", 1, 0x00, 0, None);
    table[173] = bsd_entry!(b"waitid\0", 4, 0x04, 0, None);
    table[416] = bsd_entry!(b"waitid_nocancel\0", 4, 0x04, 0, None);
    table[540] = bsd_entry!(b"preadv\0", 4, 0x00, 0, Some(sys_preadv as ocerz_bsd_fn));
    table[541] = bsd_entry!(b"pwritev\0", 4, 0x00, 0, Some(sys_pwritev as ocerz_bsd_fn));
    table[542] = bsd_entry!(
        b"preadv_nocancel\0",
        4,
        0x00,
        0,
        Some(sys_preadv_nc as ocerz_bsd_fn)
    );
    table[543] = bsd_entry!(
        b"pwritev_nocancel\0",
        4,
        0x00,
        0,
        Some(sys_pwritev_nc as ocerz_bsd_fn)
    );
    table[203] = bsd_entry!(b"mlock\0", 2, 0x01, 0, None);
    table[204] = bsd_entry!(b"munlock\0", 2, 0x01, 0, None);
    table[324] = bsd_entry!(b"mlockall\0", 1, 0x00, 0, None);
    table[325] = bsd_entry!(b"munlockall\0", 1, 0x00, 0, None);
    table[78] = bsd_entry!(b"mincore\0", 3, 0x05, 0, None);
    table[250] = bsd_entry!(b"minherit\0", 3, 0x01, 0, None);
    table[534] = bsd_entry!(b"memorystatus_available_memory\0", 2, 0x00, 0, None);
    table[126] = bsd_entry!(b"setreuid\0", 2, 0x00, 0, None);
    table[127] = bsd_entry!(b"setregid\0", 2, 0x00, 0, None);
    table[181] = bsd_entry!(b"setgid\0", 1, 0x00, 0, None);
    table[182] = bsd_entry!(b"setegid\0", 1, 0x00, 0, None);
    table[183] = bsd_entry!(b"seteuid\0", 1, 0x00, 0, None);
    table[285] = bsd_entry!(b"settid\0", 2, 0x00, 0, None);
    table[66] = bsd_entry!(
        b"vfork\0",
        0,
        0x00,
        0,
        Some(super::workers::sys_fork as ocerz_bsd_fn)
    );
    table[148] = bsd_entry!(b"pipe2\0", 2, 0x01, 0, None);
    table[149] = bsd_entry!(b"dup3\0", 3, 0x00, 0, None);
    table[369] = bsd_entry!(b"kevent64\0", 7, 0x4a, 0, None);
    table[111] = bsd_entry!(
        b"sigsuspend\0",
        1,
        0x00,
        0,
        Some(super::signals::sys_sigsuspend as ocerz_bsd_fn)
    );
    table[410] = bsd_entry!(
        b"sigsuspend_nocancel\0",
        1,
        0x00,
        0,
        Some(super::signals::sys_sigsuspend as ocerz_bsd_fn)
    );
    table[330] = bsd_entry!(
        b"__sigwait\0",
        2,
        0x00,
        0,
        Some(super::signals::sys_sigwait as ocerz_bsd_fn)
    );
    table[422] = bsd_entry!(
        b"__sigwait_nocancel\0",
        2,
        0x00,
        0,
        Some(super::signals::sys_sigwait as ocerz_bsd_fn)
    );
    table[255] = bsd_entry!(b"semget\0", 3, 0x00, 0, None);
    table[256] = bsd_entry!(b"semop\0", 3, 0x02, 0, None);
    table[254] = bsd_entry!(b"semctl\0", 4, 0x00, 0, Some(sys_semctl as ocerz_bsd_fn));
    table[265] = bsd_entry!(b"shmget\0", 3, 0x00, 0, None);
    table[262] = bsd_entry!(b"shmat\0", 3, 0x00, 0, Some(sys_shmat as ocerz_bsd_fn));
    table[264] = bsd_entry!(b"shmdt\0", 1, 0x00, 0, Some(sys_shmdt as ocerz_bsd_fn));
    table[263] = bsd_entry!(b"shmctl\0", 3, 0x04, 0, None);
    table[259] = bsd_entry!(b"msgget\0", 2, 0x00, 0, None);
    table[258] = bsd_entry!(b"msgctl\0", 3, 0x04, 0, None);
    table[260] = bsd_entry!(b"msgsnd\0", 4, 0x02, 0, None);
    table[261] = bsd_entry!(b"msgrcv\0", 5, 0x02, 0, None);
    table[132] = bsd_entry!(b"mkfifo\0", 2, 0x01, 0, None);
    table[221] = bsd_entry!(b"setattrlist\0", 5, 0x07, 0, None);
    table[245] = bsd_entry!(b"ffsctl\0", 4, 0x04, 0, None);
    table[165] = bsd_entry!(b"quotactl\0", 4, 0x09, 0, None);
    table[447] = bsd_entry!(
        b"connectx\0",
        7,
        0x00,
        0,
        Some(sys_connectx as ocerz_bsd_fn)
    );
    table[448] = bsd_entry!(b"disconnectx\0", 3, 0x00, 0, None);
    table[337] = bsd_entry!(
        b"sendfile\0",
        6,
        0x00,
        0,
        Some(sys_sendfile as ocerz_bsd_fn)
    );
    table[480] = bsd_entry!(
        b"recvmsg_x\0",
        4,
        0x00,
        0,
        Some(sys_recvmsg_x as ocerz_bsd_fn)
    );
    table[481] = bsd_entry!(
        b"sendmsg_x\0",
        4,
        0x00,
        0,
        Some(sys_sendmsg_x as ocerz_bsd_fn)
    );
    table[461] = bsd_entry!(b"getattrlistbulk\0", 5, 0x06, 0, None);
    table[463] = bsd_entry!(b"openat\0", 4, 0x02, 0, None);
    table[464] = bsd_entry!(b"openat_nocancel\0", 4, 0x02, 0, None);
    table[470] = bsd_entry!(b"fstatat64\0", 4, 0x06, 0, None);
    table[472] = bsd_entry!(b"unlinkat\0", 3, 0x02, 0, None);
    table[473] = bsd_entry!(b"readlinkat\0", 4, 0x06, 0, None);
    table[475] = bsd_entry!(b"mkdirat\0", 3, 0x02, 0, None);
    table[500] = bsd_entry!(b"getentropy\0", 2, 0x01, 0, None);
    table[56] = bsd_entry!(b"revoke\0", 1, 0x01, 0, None);
    table[178] = bsd_entry!(b"kdebug_trace_string\0", 3, 0x04, 0, None);
    table[179] = bsd_entry!(b"kdebug_trace64\0", 5, 0x00, 0, None);
    table[186] = bsd_entry!(b"thread_selfcounts\0", 3, 0x02, 0, None);
    table[217] = bsd_entry!(b"fsgetpath_ext\0", 5, 0x05, 0, None);
    table[218] = bsd_entry!(b"openat_dprotected_np\0", 7, 0x02, 0, None);
    table[222] = bsd_entry!(b"getdirentriesattr\0", 8, 0x76, 0, None);
    table[223] = bsd_entry!(b"exchangedata\0", 3, 0x03, 0, None);
    table[226] = bsd_entry!(b"delete\0", 1, 0x01, 0, None);
    table[227] = bsd_entry!(b"copyfile\0", 4, 0x03, 0, None);
    table[242] = bsd_entry!(b"fsctl\0", 4, 0x05, 0, None);
    table[277] = bsd_entry!(b"open_extended\0", 6, 0x21, 0, None);
    table[278] = bsd_entry!(b"umask_extended\0", 2, 0x02, 0, None);
    table[279] = bsd_entry!(b"stat_extended\0", 4, 0x0f, 0, None);
    table[280] = bsd_entry!(b"lstat_extended\0", 4, 0x0f, 0, None);
    table[281] = bsd_entry!(b"fstat_extended\0", 4, 0x0e, 0, None);
    table[282] = bsd_entry!(b"chmod_extended\0", 5, 0x11, 0, None);
    table[284] = bsd_entry!(b"access_extended\0", 4, 0x05, 0, None);
    table[288] = bsd_entry!(b"getsgroups\0", 2, 0x03, 0, None);
    table[290] = bsd_entry!(b"getwgroups\0", 2, 0x03, 0, None);
    table[291] = bsd_entry!(b"mkfifo_extended\0", 5, 0x11, 0, None);
    table[292] = bsd_entry!(b"mkdir_extended\0", 5, 0x11, 0, None);
    table[314] = bsd_entry!(b"aio_return\0", 1, 0x01, 0, None);
    table[315] = bsd_entry!(b"aio_suspend\0", 3, 0x05, 0, None);
    table[316] = bsd_entry!(b"aio_cancel\0", 2, 0x02, 0, None);
    table[317] = bsd_entry!(b"aio_error\0", 1, 0x01, 0, None);
    table[318] = bsd_entry!(b"aio_read\0", 1, 0x01, 0, None);
    table[319] = bsd_entry!(b"aio_write\0", 1, 0x01, 0, None);
    table[320] = bsd_entry!(b"lio_listio\0", 4, 0x0a, 0, None);
    table[323] = bsd_entry!(b"process_policy\0", 7, 0x10, 0, None);
    table[348] = bsd_entry!(b"pthread_chdir\0", 1, 0x01, 0, None);
    table[349] = bsd_entry!(b"pthread_fchdir\0", 1, 0x00, 0, None);
    table[382] = bsd_entry!(b"mac_get_file\0", 2, 0x03, 0, None);
    table[384] = bsd_entry!(b"mac_get_link\0", 2, 0x03, 0, None);
    table[386] = bsd_entry!(b"mac_get_proc\0", 1, 0x01, 0, None);
    table[388] = bsd_entry!(b"mac_get_fd\0", 2, 0x02, 0, None);
    table[390] = bsd_entry!(b"mac_get_pid\0", 2, 0x02, 0, None);
    table[418] = bsd_entry!(b"msgsnd_nocancel\0", 4, 0x02, 0, None);
    table[419] = bsd_entry!(b"msgrcv_nocancel\0", 5, 0x02, 0, None);
    table[421] = bsd_entry!(b"aio_suspend_nocancel\0", 3, 0x05, 0, None);
    table[425] = bsd_entry!(b"mac_get_mount\0", 2, 0x03, 0, None);
    table[426] = bsd_entry!(b"mac_getfsstat\0", 5, 0x05, 0, None);
    table[429] = bsd_entry!(b"audit_session_join\0", 1, 0x00, 0, None);
    table[432] = bsd_entry!(b"audit_session_port\0", 2, 0x02, 0, None);
    table[440] = bsd_entry!(b"memorystatus_control\0", 5, 0x08, 0, None);
    table[446] = bsd_entry!(b"proc_rlimit_control\0", 3, 0x04, 0, None);
    table[450] = bsd_entry!(b"socket_delegate\0", 4, 0x00, 0, None);
    table[453] = bsd_entry!(b"memorystatus_get_level\0", 1, 0x01, 0, None);
    table[459] = bsd_entry!(b"coalition_info\0", 4, 0x0e, 0, None);
    table[460] = bsd_entry!(b"necp_match_policy\0", 3, 0x05, 0, None);
    table[462] = bsd_entry!(b"clonefileat\0", 5, 0x0a, 0, None);
    table[465] = bsd_entry!(b"renameat\0", 4, 0x0a, 0, None);
    table[467] = bsd_entry!(b"fchmodat\0", 4, 0x02, 0, None);
    table[468] = bsd_entry!(b"fchownat\0", 5, 0x02, 0, None);
    table[479] = bsd_entry!(b"openbyid_np\0", 3, 0x03, 0, None);
    table[482] = bsd_entry!(b"thread_selfusage\0", 0, 0x00, 0, None);
    table[485] = bsd_entry!(b"guarded_write_np\0", 4, 0x06, 0, None);
    table[487] = bsd_entry!(b"guarded_writev_np\0", 4, 0x06, 0, None);
    table[488] = bsd_entry!(b"renameatx_np\0", 5, 0x0a, 0, None);
    table[490] = bsd_entry!(b"netagent_trigger\0", 2, 0x01, 0, None);
    table[496] = bsd_entry!(b"mach_eventlink_signal\0", 2, 0x00, 0, None);
    table[497] = bsd_entry!(b"mach_eventlink_wait_until\0", 5, 0x00, 0, None);
    table[498] = bsd_entry!(b"mach_eventlink_signal_wait_until\0", 6, 0x00, 0, None);
    table[499] = bsd_entry!(b"work_interval_ctl\0", 4, 0x04, 0, None);
    table[517] = bsd_entry!(b"fclonefileat\0", 4, 0x04, 0, None);
    table[522] = bsd_entry!(b"necp_session_open\0", 1, 0x00, 0, None);
    table[523] = bsd_entry!(b"necp_session_action\0", 6, 0x14, 0, None);
    table[525] = bsd_entry!(b"net_qos_guideline\0", 2, 0x01, 0, None);
    table[527] = bsd_entry!(b"ntp_adjtime\0", 1, 0x01, 0, None);
    table[528] = bsd_entry!(b"ntp_gettime\0", 1, 0x01, 0, None);
    table[531] = bsd_entry!(b"mach_bridge_remote_time\0", 1, 0x00, 0, None);
    table[533] = bsd_entry!(b"log_data\0", 4, 0x04, 0, None);
    table[545] = bsd_entry!(b"proc_info_extended_id\0", 8, 0x40, 0, None);
    table[551] = bsd_entry!(b"freadlink\0", 3, 0x02, 0, None);
    table
}

static BSD_TABLE: [ocerz_bsd_entry; OCERZ_BSD_MAX as usize] = make_bsd_table();

unsafe fn sys_recvmsg(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    sys_msg(cpu, a, 27, false)
}
unsafe fn sys_sendmsg(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    sys_msg(cpu, a, 28, true)
}
unsafe fn sys_recvmsg_nocancel(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    sys_msg(cpu, a, 401, false)
}
unsafe fn sys_sendmsg_nocancel(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    sys_msg(cpu, a, 402, true)
}

unsafe fn sys_msg(cpu: *mut OcerzCPU, a: *mut [u64; 8], num: c_int, is_send: bool) -> c_int {
    let args = &*a;
    let gmsg = args[1];
    if gmsg == 0 {
        ret_err(cpu, libc::EFAULT as u64);
        return crate::ffi::OCERZ_STEP_OK as c_int;
    }
    let mut iovlen = ocerz_ld(gmsg.wrapping_add(24), 4) as i32;
    if iovlen < 0 {
        iovlen = 0;
    }
    if iovlen > OCERZ_IOV_MAX as i32 {
        iovlen = OCERZ_IOV_MAX as i32;
    }
    let giov = ocerz_ld(gmsg.wrapping_add(16), 8);
    let mut iovs = [libc::iovec {
        iov_base: ptr::null_mut(),
        iov_len: 0,
    }; OCERZ_IOV_MAX];
    for i in 0..iovlen as usize {
        let base = ocerz_ld(giov.wrapping_add((i as u64).wrapping_mul(16)), 8);
        let len = ocerz_ld(
            giov.wrapping_add((i as u64).wrapping_mul(16))
                .wrapping_add(8),
            8,
        );
        let iov = iovs.as_mut_ptr().add(i);
        (*iov).iov_base = if base != 0 {
            ocerz_g2h(base)
        } else {
            ptr::null_mut()
        };
        (*iov).iov_len = len as usize;
    }
    let gname = ocerz_ld(gmsg, 8);
    let gctrl = ocerz_ld(gmsg.wrapping_add(32), 8);
    let mut h: libc::msghdr = mem::zeroed();
    h.msg_name = if gname != 0 {
        ocerz_g2h(gname)
    } else {
        ptr::null_mut()
    };
    h.msg_namelen = ocerz_ld(gmsg.wrapping_add(8), 4) as libc::socklen_t;
    h.msg_iov = iovs.as_mut_ptr();
    h.msg_iovlen = iovlen as _;
    h.msg_control = if gctrl != 0 {
        ocerz_g2h(gctrl)
    } else {
        ptr::null_mut()
    };
    h.msg_controllen = ocerz_ld(gmsg.wrapping_add(40), 4) as _;
    h.msg_flags = ocerz_ld(gmsg.wrapping_add(44), 4) as i32;
    let mut fa = [
        args[0],
        &mut h as *mut libc::msghdr as u64,
        args[2],
        0,
        0,
        0,
        0,
        0,
    ];
    (*cpu).block_nokick = 1;
    (*cpu).block_since_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    let mut err = 0;
    let mut ret2 = 0;
    let r =
        crate::ported::syscall::raw::ocerz_host_syscall(num as i64, &mut fa, &mut ret2, &mut err);
    (*cpu).block_since_ns = 0;
    (*cpu).block_nokick = 0;
    if err != 0 {
        ret_err(cpu, r);
        return crate::ffi::OCERZ_STEP_OK as c_int;
    }
    if !is_send {
        ocerz_st(gmsg.wrapping_add(8), 4, h.msg_namelen as u64);
        ocerz_st(gmsg.wrapping_add(40), 4, h.msg_controllen as u64);
        ocerz_st(gmsg.wrapping_add(44), 4, h.msg_flags as u32 as u64);
    }
    if env_set!("OCERZ_MSGLOG") {
        let mut cfd = -1;
        let mut ctype = 0;
        if !h.msg_control.is_null() && h.msg_controllen >= 16 {
            ctype = ocerz_ld(gctrl.wrapping_add(8), 4) as c_int;
            cfd = ocerz_ld(gctrl.wrapping_add(12), 4) as c_int;
        }
        let data0 = if iovlen > 0 && !iovs[0].iov_base.is_null() && r >= 4 {
            ptr::read(iovs[0].iov_base.cast::<u32>())
        } else {
            0
        };
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: %s[%d] cpu#%u fd=%d ret=%lld ctrl=%u cmsgtype=%d cmsgfd=%d data0=%#x\n"
                .as_ptr(),
            if is_send {
                c"SENDMSG".as_ptr()
            } else {
                c"RECVMSG".as_ptr()
            },
            libc::getpid(),
            (*cpu).cpu_number,
            args[0] as c_int,
            r as i64,
            h.msg_controllen as u32,
            ctype,
            cfd,
            data0,
        );
    }
    ret_ok(cpu, r);
    crate::ffi::OCERZ_STEP_OK as c_int
}

unsafe fn sys_ioctl(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    let args = &*a;
    let arg = args[2];
    let mut fa = [
        args[0],
        args[1],
        if arg != 0 { ocerz_g2h(arg) as u64 } else { 0 },
        0,
        0,
        0,
        0,
        0,
    ];
    forward_with_scratch(cpu, 54, &mut fa, 0);
    crate::ffi::OCERZ_STEP_OK as c_int
}

unsafe fn sys_fcntl(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    let args = &*a;
    let mut fa = [args[0], args[1], args[2], 0, 0, 0, 0, 0];
    if ocerz_fcntl_ptr_cmd(args[1] as c_int) != 0 && args[2] != 0 {
        fa[2] = ocerz_g2h(args[2]) as u64;
    }
    forward_with_scratch(cpu, 92, &mut fa, 0);
    crate::ffi::OCERZ_STEP_OK as c_int
}

unsafe fn sys_identity_only(
    _vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    a: *mut [u64; 8],
    num: c_int,
    mask: u8,
) -> c_int {
    if crate::ffi::ocerz_guest_base != 0 {
        ret_err(cpu, libc::ENOSYS as u64);
        return crate::ffi::OCERZ_STEP_OK as c_int;
    }
    let mut fa = *a;
    for i in 0..8 {
        let arg = fa.as_mut_ptr().add(i);
        if mask & (1 << i) != 0 && *arg != 0 {
            *arg = ocerz_g2h(*arg) as u64;
        }
    }
    forward_with_scratch(cpu, num, &mut fa, 0);
    crate::ffi::OCERZ_STEP_OK as c_int
}

unsafe fn sys_connectx(vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    sys_identity_only(vm, cpu, a, 447, 0)
}
unsafe fn sys_sendfile(vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    sys_identity_only(vm, cpu, a, 337, 0x18)
}
unsafe fn sys_recvmsg_x(vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    sys_identity_only(vm, cpu, a, 480, 0x02)
}
unsafe fn sys_sendmsg_x(vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    sys_identity_only(vm, cpu, a, 481, 0x02)
}

unsafe fn sys_semctl(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    let args = &*a;
    let cmd = args[2] as c_int;
    let is_ptr = matches!(cmd, 1 | 2 | 6 | 9);
    let mut fa = [
        args[0],
        args[1],
        args[2],
        if is_ptr && args[3] != 0 {
            ocerz_g2h(args[3]) as u64
        } else {
            args[3]
        },
        0,
        0,
        0,
        0,
    ];
    forward_with_scratch(cpu, 254, &mut fa, 0);
    crate::ffi::OCERZ_STEP_OK as c_int
}

unsafe fn shm_slot_reserve() -> c_int {
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_SHM_LOCK));
    let attaches = ptr::addr_of_mut!(G_SHM_ATTACH).cast::<ShmAttach>();
    for index in 0..OCERZ_SHM_ATTACH_MAX {
        let attach = &mut *attaches.add(index);
        if attach.host.is_null() && attach.size == 0 {
            attach.size = 1;
            libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_SHM_LOCK));
            return index as c_int;
        }
    }
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_SHM_LOCK));
    -1
}

unsafe fn shm_slot_release(slot: c_int) {
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_SHM_LOCK));
    *ptr::addr_of_mut!(G_SHM_ATTACH)
        .cast::<ShmAttach>()
        .add(slot as usize) = ShmAttach {
        gaddr: 0,
        size: 0,
        host: ptr::null_mut(),
    };
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_SHM_LOCK));
}

unsafe fn sys_shmat(vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    let args = &*a;
    let shmid = args[0] as c_int;
    let mut want = args[1];
    let flags = args[2] as c_int;
    let mut ds: libc::shmid_ds = mem::zeroed();
    if libc::shmctl(shmid, libc::IPC_STAT, &mut ds) != 0 {
        ret_err(cpu, *libc::__error() as u64);
        return crate::ffi::OCERZ_STEP_OK as c_int;
    }
    let size =
        (ds.shm_segsz as u64).wrapping_add(OCERZ_HOST_PAGE_SIZE - 1) & !(OCERZ_HOST_PAGE_SIZE - 1);
    if size == 0 {
        ret_err(cpu, libc::EINVAL as u64);
        return crate::ffi::OCERZ_STEP_OK as c_int;
    }
    if want != 0 {
        if flags & libc::SHM_RND != 0 {
            want &= !(OCERZ_HOST_PAGE_SIZE - 1);
        }
        if want & (OCERZ_HOST_PAGE_SIZE - 1) != 0 {
            ret_err(cpu, libc::EINVAL as u64);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
    }
    let slot = shm_slot_reserve();
    if slot < 0 {
        ret_err(cpu, libc::EMFILE as u64);
        return crate::ffi::OCERZ_STEP_OK as c_int;
    }
    let host = libc::shmat(shmid, ptr::null(), flags & libc::SHM_RDONLY);
    if host as isize == -1 {
        let e = *libc::__error();
        shm_slot_release(slot);
        ret_err(cpu, e as u64);
        return crate::ffi::OCERZ_STEP_OK as c_int;
    }
    let prot = if flags & libc::SHM_RDONLY != 0 {
        libc::PROT_READ
    } else {
        libc::PROT_READ | libc::PROT_WRITE
    };
    let gaddr;
    if want != 0 {
        super::mem::invalidate_guest_mapping((*cpu).vm, want, size);
        if crate::ffi::ocerz_map_claim_region(want, size, prot) != crate::ffi::OCERZ_OK as c_int {
            libc::shmdt(host);
            shm_slot_release(slot);
            ret_err(cpu, libc::ENOMEM as u64);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        gaddr = want;
    } else {
        gaddr = crate::ffi::ocerz_map_donate(size);
        if gaddr == 0 {
            libc::shmdt(host);
            shm_slot_release(slot);
            ret_err(cpu, libc::ENOMEM as u64);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        super::mem::invalidate_guest_mapping((*cpu).vm, gaddr, size);
    }
    let host_dst = ocerz_g2h(gaddr) as u64;
    let mut dst = host_dst;
    let mut curp = 0;
    let mut maxp = 0;
    crate::ffi::ocerz_jit_require_ordered((*cpu).vm);
    let kr = mach_vm_remap(
        mach_task_self_,
        &mut dst,
        size,
        0,
        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
        mach_task_self_,
        host as u64,
        0,
        &mut curp,
        &mut maxp,
        VM_INHERIT_SHARE,
    );
    if kr != 0 || dst != host_dst {
        crate::ffi::ocerz_unmap(gaddr, size);
        libc::shmdt(host);
        shm_slot_release(slot);
        ret_err(cpu, libc::ENOMEM as u64);
        return crate::ffi::OCERZ_STEP_OK as c_int;
    }
    if prot == libc::PROT_READ {
        crate::ffi::ocerz_protect(gaddr, size, libc::PROT_READ);
    }
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_SHM_LOCK));
    *ptr::addr_of_mut!(G_SHM_ATTACH)
        .cast::<ShmAttach>()
        .add(slot as usize) = ShmAttach { gaddr, size, host };
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_SHM_LOCK));
    if (*vm).strace != 0 {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: shmat id=%d size=%#llx host=%p -> guest=%#llx\n".as_ptr(),
            shmid,
            size as libc::c_ulonglong,
            host,
            gaddr as libc::c_ulonglong,
        );
    }
    ret_ok(cpu, gaddr);
    crate::ffi::OCERZ_STEP_OK as c_int
}

unsafe fn sys_shmdt(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    let gaddr = (*a)[0];
    let mut host = ptr::null_mut();
    let mut size = 0u64;
    libc::pthread_mutex_lock(ptr::addr_of_mut!(G_SHM_LOCK));
    let attaches = ptr::addr_of_mut!(G_SHM_ATTACH).cast::<ShmAttach>();
    for i in 0..OCERZ_SHM_ATTACH_MAX {
        let attach = &mut *attaches.add(i);
        if !attach.host.is_null() && attach.gaddr == gaddr {
            host = attach.host;
            size = attach.size;
            *attach = ShmAttach {
                gaddr: 0,
                size: 0,
                host: ptr::null_mut(),
            };
            break;
        }
    }
    libc::pthread_mutex_unlock(ptr::addr_of_mut!(G_SHM_LOCK));
    if host.is_null() {
        ret_err(cpu, libc::EINVAL as u64);
        return crate::ffi::OCERZ_STEP_OK as c_int;
    }
    super::mem::invalidate_guest_mapping((*cpu).vm, gaddr, size);
    crate::ffi::ocerz_unmap(gaddr, size);
    libc::shmdt(host);
    ret_ok(cpu, 0);
    crate::ffi::OCERZ_STEP_OK as c_int
}

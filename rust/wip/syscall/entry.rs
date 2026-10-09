//! Syscall entry, per-CPU ring tracing, and diagnostic probes at the edge of
//! dispatch; the raw sysring access remains unchecked on the hot path.

use super::signals::SysRingEntry;
use super::util::*;
use super::*;

use core::ffi::{c_char, c_int, c_void};
use core::ptr;
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

unsafe extern "C" {
    fn clock_gettime_nsec_np(clock_id: libc::clockid_t) -> u64;
    fn memmem(
        haystack: *const c_void,
        haystacklen: usize,
        needle: *const c_void,
        needlelen: usize,
    ) -> *mut c_void;
}

const CLOCK_UPTIME_RAW: libc::clockid_t = 8;
static mut G_BIGRING: *mut SysRingEntry = ptr::null_mut();
static mut G_BIGRING_MASK: u32 = 0;
static G_BIGRING_N: AtomicU32 = AtomicU32::new(0);

unsafe fn pe_scan_print(cpu: *mut OcerzCPU, tag: *const c_char, detail: *const c_char) {
    let gs = (*cpu).gs_base;
    let teb_addr = gs.wrapping_add(0x30);
    let teb = if gs != 0 && crate::ffi::ocerz_addr_readable(teb_addr) != 0 {
        ocerz_ld(teb_addr, 8)
    } else {
        0
    };
    let frame_addr = teb.wrapping_add(0x378);
    let frame = if teb != 0 && crate::ffi::ocerz_addr_readable(frame_addr) != 0 {
        ocerz_ld(frame_addr, 8)
    } else {
        0
    };
    let user_rsp = if frame != 0 && crate::ffi::ocerz_addr_readable(frame + 0x88) != 0 {
        ocerz_ld(frame + 0x88, 8)
    } else {
        0
    };
    let tid = if teb != 0 && crate::ffi::ocerz_addr_readable(teb + 0x48) != 0 {
        ocerz_ld(teb + 0x48, 8)
    } else {
        0
    };
    libc::fprintf(
        crate::log::stderr(),
        c"ocerz: %s[%d] tid=%04llx%s pe-rsp=%#llx scan:".as_ptr(),
        tag,
        libc::getpid(),
        tid as libc::c_ulonglong,
        detail,
        user_rsp as libc::c_ulonglong,
    );
    let mut printed = 0;
    let mut offset = 0u64;
    while user_rsp != 0 && offset < 0x4000 && printed < 48 {
        if crate::ffi::ocerz_addr_readable(user_rsp.wrapping_add(offset)) == 0 {
            break;
        }
        let value = ocerz_ld(user_rsp.wrapping_add(offset), 8);
        let codey = (0x6fff00000000..0x7ffc00000000).contains(&value)
            || (0x100000000..0x200000000).contains(&value);
        if codey {
            libc::fprintf(
                crate::log::stderr(),
                c" +%llx:%#llx".as_ptr(),
                offset as libc::c_ulonglong,
                value as libc::c_ulonglong,
            );
            printed += 1;
        }
        offset += 8;
    }
    libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
}

unsafe fn writetrap_check(cpu: *mut OcerzCPU, num: c_int) {
    static mut TRAP: *const c_char = usize::MAX as *const c_char;
    if TRAP as usize == usize::MAX {
        TRAP = libc::getenv(c"OCERZ_WRITETRAP".as_ptr());
    }
    if TRAP.is_null() && !env_set!("OCERZ_REQTRAP") {
        return;
    }
    let trap_len = if TRAP.is_null() {
        0
    } else {
        libc::strlen(TRAP)
    };
    let mut hit = false;
    if !TRAP.is_null() && num == 4 {
        let buffer = (*cpu).gpr[crate::ffi::OCERZ_RSI as usize];
        let len = (*cpu).gpr[crate::ffi::OCERZ_RDX as usize];
        if len != 0
            && len < 0x100000
            && crate::ffi::ocerz_addr_readable(buffer) != 0
            && crate::ffi::ocerz_addr_readable(buffer.wrapping_add(len - 1)) != 0
        {
            hit = !memmem(
                ocerz_g2h(buffer).cast(),
                len as usize,
                TRAP.cast(),
                trap_len,
            )
            .is_null();
        }
    } else if !TRAP.is_null() && num == 121 {
        let iov = (*cpu).gpr[crate::ffi::OCERZ_RSI as usize];
        let count = (*cpu).gpr[crate::ffi::OCERZ_RDX as usize];
        let mut i = 0;
        while i < count && i < 64 && !hit {
            if crate::ffi::ocerz_addr_readable(iov + i * 16 + 15) == 0 {
                break;
            }
            let buffer = ocerz_ld(iov + i * 16, 8);
            let len = ocerz_ld(iov + i * 16 + 8, 8);
            if len != 0
                && len < 0x100000
                && crate::ffi::ocerz_addr_readable(buffer) != 0
                && crate::ffi::ocerz_addr_readable(buffer.wrapping_add(len - 1)) != 0
            {
                hit = !memmem(
                    ocerz_g2h(buffer).cast(),
                    len as usize,
                    TRAP.cast(),
                    trap_len,
                )
                .is_null();
            }
            i += 1;
        }
    }
    static mut REQTRAP: *const c_char = usize::MAX as *const c_char;
    static mut REQNUM: c_int = -1;
    static mut REQHANDLE: u32 = 0;
    if REQTRAP as usize == usize::MAX {
        REQTRAP = libc::getenv(c"OCERZ_REQTRAP".as_ptr());
        if !REQTRAP.is_null() {
            REQNUM = libc::strtol(REQTRAP, ptr::null_mut(), 10) as c_int;
            let colon = libc::strchr(REQTRAP, b':' as c_int);
            if !colon.is_null() {
                REQHANDLE = libc::strtoul(colon.add(1), ptr::null_mut(), 16) as u32;
            }
        }
    }
    if !REQTRAP.is_null() && !hit {
        let mut buffer = 0;
        let mut len = 0;
        if num == 4 {
            buffer = (*cpu).gpr[crate::ffi::OCERZ_RSI as usize];
            len = (*cpu).gpr[crate::ffi::OCERZ_RDX as usize];
        } else if num == 121
            && (*cpu).gpr[crate::ffi::OCERZ_RDX as usize] >= 1
            && crate::ffi::ocerz_addr_readable((*cpu).gpr[crate::ffi::OCERZ_RSI as usize] + 15) != 0
        {
            buffer = ocerz_ld((*cpu).gpr[crate::ffi::OCERZ_RSI as usize], 8);
            len = ocerz_ld((*cpu).gpr[crate::ffi::OCERZ_RSI as usize] + 8, 8);
        }
        if buffer != 0
            && len >= 16
            && crate::ffi::ocerz_addr_readable(buffer + 15) != 0
            && ocerz_ld(buffer, 4) as c_int == REQNUM
            && ocerz_ld(buffer + 12, 4) as u32 == REQHANDLE
        {
            hit = true;
        }
    }
    if hit {
        pe_scan_print(cpu, c"WRITETRAP".as_ptr(), c"".as_ptr());
    }
}

unsafe fn pagetrap_init() {
    let low = ptr::addr_of_mut!(super::mem::g_pagetrap_lo);
    if *low != !0u64 {
        return;
    }
    let env = libc::getenv(c"OCERZ_PAGETRAP".as_ptr());
    *low = 0;
    *ptr::addr_of_mut!(super::mem::g_pagetrap_hi) = 0;
    if env.is_null() {
        return;
    }
    let mut end = ptr::null_mut();
    let addr = libc::strtoull(env, &mut end, 0) as u64;
    let len = if !end.is_null() && *end == b':' as c_char {
        libc::strtoull(end.add(1), ptr::null_mut(), 0) as u64
    } else {
        1
    };
    *low = if len == 1 { addr & !0x3fff } else { addr };
    *ptr::addr_of_mut!(super::mem::g_pagetrap_hi) = if len == 1 {
        (*low).wrapping_add(0x4000)
    } else {
        addr.wrapping_add(len)
    };
}

unsafe fn pagetrap_check(cpu: *mut OcerzCPU, num: c_int) {
    pagetrap_init();
    let low = *ptr::addr_of!(super::mem::g_pagetrap_lo);
    let high = *ptr::addr_of!(super::mem::g_pagetrap_hi);
    if high == 0 {
        return;
    }
    let addr = (*cpu).gpr[crate::ffi::OCERZ_RDI as usize];
    let len = (*cpu).gpr[crate::ffi::OCERZ_RSI as usize];
    let what = match num {
        73 => c"munmap".as_ptr(),
        74 => c"mprotect".as_ptr(),
        75 => c"madvise".as_ptr(),
        197 if (*cpu).gpr[crate::ffi::OCERZ_R10 as usize] & libc::MAP_FIXED as u64 != 0 => {
            c"mmap-fixed".as_ptr()
        }
        _ => return,
    };
    if !(addr < high && low < addr.wrapping_add(len)) {
        return;
    }
    let mut detail = [0i8; 160];
    libc::snprintf(
        detail.as_mut_ptr(),
        detail.len(),
        c" %s addr=%#llx len=%#llx a3=%#llx a4=%#llx".as_ptr(),
        what,
        addr as libc::c_ulonglong,
        len as libc::c_ulonglong,
        (*cpu).gpr[crate::ffi::OCERZ_RDX as usize] as libc::c_ulonglong,
        (*cpu).gpr[crate::ffi::OCERZ_R10 as usize] as libc::c_ulonglong,
    );
    pe_scan_print(cpu, c"PAGETRAP".as_ptr(), detail.as_ptr());
}

unsafe fn peekguard(cpu: *mut OcerzCPU, class: c_int, num: c_int, when: *const c_char) {
    static mut ADDR: u64 = 0;
    static mut LAST: u64 = 0;
    static mut ON: c_int = -1;
    if ON < 0 {
        let env = libc::getenv(c"OCERZ_PEEKGUARD".as_ptr());
        ON = (!env.is_null()) as c_int;
        if ON != 0 {
            ADDR = libc::strtoull(env, ptr::null_mut(), 0) as u64;
            LAST = ocerz_ld(ADDR, 8);
        }
    }
    if ON == 0 {
        return;
    }
    let value = ocerz_ld(ADDR, 8);
    if value != LAST {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: PEEKGUARD [%#llx] %#llx -> %#llx at %s of sys=%d/%d rip=%#llx a0=%#llx a1=%#llx a2=%#llx\n".as_ptr(),
            ADDR as libc::c_ulonglong,
            LAST as libc::c_ulonglong,
            value as libc::c_ulonglong,
            when,
            class,
            num,
            (*cpu).rip as libc::c_ulonglong,
            (*cpu).gpr[crate::ffi::OCERZ_RDI as usize] as libc::c_ulonglong,
            (*cpu).gpr[crate::ffi::OCERZ_RSI as usize] as libc::c_ulonglong,
            (*cpu).gpr[crate::ffi::OCERZ_RDX as usize] as libc::c_ulonglong,
        );
        libc::fflush(crate::log::stderr());
        LAST = value;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bigring_init() {
    let env = libc::getenv(c"OCERZ_SYSRING".as_ptr());
    let capacity = if env.is_null() {
        0
    } else {
        libc::strtoul(env, ptr::null_mut(), 0) as usize
    };
    if capacity < 64 || !capacity.is_power_of_two() {
        return;
    }
    G_BIGRING = libc::calloc(capacity, core::mem::size_of::<SysRingEntry>()).cast::<SysRingEntry>();
    if !G_BIGRING.is_null() {
        G_BIGRING_MASK = (capacity - 1) as u32;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_bigring_dump() {
    if G_BIGRING.is_null() {
        return;
    }
    let now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    let n = G_BIGRING_N.load(Ordering::Relaxed);
    let cap = G_BIGRING_MASK.wrapping_add(1);
    let start = if n > cap { n - cap } else { 0 };
    let mut k = start;
    while k < n {
        let entry = &*G_BIGRING.add((k & G_BIGRING_MASK) as usize);
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: SYSRING[%d] tid=..%04x %10.1fms %d/%d a0=%#llx a1=%#llx a2=%#llx ret=%#llx peek=%#llx/%#llx/%#llx/%#llx\n".as_ptr(),
            libc::getpid(),
            (entry.a2 >> 48) as u16 as c_int,
            (entry.t.wrapping_sub(now) as i64) as f64 / 1e6,
            entry.num >> 24,
            entry.num & 0xffffff,
            entry.a0 as libc::c_ulonglong,
            entry.a1 as libc::c_ulonglong,
            entry.a2 & 0xffffffff,
            entry.ret as libc::c_ulonglong,
            entry.peek as libc::c_ulonglong,
            entry.peek2 as libc::c_ulonglong,
            entry.peek3 as libc::c_ulonglong,
            entry.peek4 as libc::c_ulonglong,
        );
        k += 1;
    }
}

pub(super) unsafe fn ocerz_bigring_push(cpu: *mut OcerzCPU, entry: *const SysRingEntry) {
    if G_BIGRING.is_null() {
        return;
    }
    let index = G_BIGRING_N.fetch_add(1, Ordering::Relaxed) & G_BIGRING_MASK;
    let dest = G_BIGRING.add(index as usize);
    ptr::copy_nonoverlapping(entry, dest, 1);
    (*dest).a2 = ((*dest).a2 & 0xffffffff) | (((*cpu).host_tid & 0xffff) << 48);
}

#[inline(always)]
unsafe fn sysring_enter(cpu: *mut OcerzCPU, class: c_int, num: c_int) -> u32 {
    let at = (*cpu).sysring_n;
    (*cpu).sysring_n = at.wrapping_add(1);
    let entry = ptr::addr_of_mut!((*cpu).sysring)
        .cast::<SysRingEntry>()
        .add((at % 24) as usize);
    (*entry).t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    (*entry).num = ((class << 24) | num) as i32;
    (*entry).a0 = (*cpu).gpr[crate::ffi::OCERZ_RDI as usize];
    (*entry).a1 = (*cpu).gpr[crate::ffi::OCERZ_RSI as usize];
    (*entry).a2 = (*cpu).gpr[crate::ffi::OCERZ_RDX as usize];
    (*entry).ret = !0u64;
    (*entry).peek = 0;
    (*entry).peek2 = 0;
    (*entry).peek3 = 0;
    (*entry).peek4 = 0;
    at
}

#[inline(always)]
unsafe fn sysring_exit(cpu: *mut OcerzCPU, at: u32, class: c_int, num: c_int) {
    let entry = ptr::addr_of_mut!((*cpu).sysring)
        .cast::<SysRingEntry>()
        .add((at % 24) as usize);
    if (*cpu).sysring_n.wrapping_sub(at) > 24 {
        return;
    }
    (*entry).ret = (*cpu).gpr[crate::ffi::OCERZ_RAX as usize]
        | if (*cpu).rflags & crate::inline::OCERZ_CF != 0 {
            1u64 << 63
        } else {
            0
        };
    let mut buffer = 0;
    let mut len = 0;
    if class == 2
        && (num == 3 || num == 4)
        && (*cpu).rflags & crate::inline::OCERZ_CF == 0
        && (*cpu).gpr[crate::ffi::OCERZ_RAX as usize] >= 8
    {
        buffer = (*entry).a1;
        len = (*cpu).gpr[crate::ffi::OCERZ_RAX as usize];
    } else if class == 2
        && num == 121
        && (*cpu).rflags & crate::inline::OCERZ_CF == 0
        && crate::ffi::ocerz_addr_readable((*entry).a1 + 31) != 0
    {
        buffer = ocerz_ld((*entry).a1, 8);
        len = ocerz_ld((*entry).a1 + 8, 8);
        let b1 = if (*entry).a2 > 1 {
            ocerz_ld((*entry).a1 + 16, 8)
        } else {
            0
        };
        if b1 != 0
            && ocerz_ld((*entry).a1 + 24, 8) >= 8
            && crate::ffi::ocerz_addr_readable(b1) != 0
            && crate::ffi::ocerz_addr_readable(b1 + 7) != 0
        {
            (*entry).peek3 = ocerz_ld(b1, 8);
        }
        let b2 = if (*entry).a2 > 2 && crate::ffi::ocerz_addr_readable((*entry).a1 + 47) != 0 {
            ocerz_ld((*entry).a1 + 32, 8)
        } else {
            0
        };
        if b2 != 0
            && ocerz_ld((*entry).a1 + 40, 8) >= 8
            && crate::ffi::ocerz_addr_readable(b2) != 0
            && crate::ffi::ocerz_addr_readable(b2 + 7) != 0
        {
            (*entry).peek4 = ocerz_ld(b2, 8);
        }
    }
    if buffer != 0
        && crate::ffi::ocerz_addr_readable(buffer) != 0
        && crate::ffi::ocerz_addr_readable(buffer + 7) != 0
    {
        (*entry).peek = ocerz_ld(buffer, 8);
    }
    if class == 2 && (num == 3 || num == 4) && buffer != 0 {
        if len >= 16 && crate::ffi::ocerz_addr_readable(buffer + 15) != 0 {
            (*entry).peek2 = ocerz_ld(buffer + 8, 8);
        }
        if len >= 40 && crate::ffi::ocerz_addr_readable(buffer + 39) != 0 {
            (*entry).peek3 = ocerz_ld(buffer + 32, 8);
        }
        if len >= 48 && crate::ffi::ocerz_addr_readable(buffer + 47) != 0 {
            (*entry).peek4 = ocerz_ld(buffer + 40, 8);
        }
    } else if buffer != 0 && len >= 40 && crate::ffi::ocerz_addr_readable(buffer + 39) != 0 {
        (*entry).peek2 = ocerz_ld(buffer + 32, 8);
    }
    ocerz_bigring_push(cpu, entry);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_handle_syscall(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    let rax = (*cpu).gpr[crate::ffi::OCERZ_RAX as usize];
    let class = ((rax >> 24) & 0xff) as c_int;
    let num = (rax & 0xffffff) as c_int;
    let ring_at = sysring_enter(cpu, class, num);
    peekguard(cpu, class, num, c"entry".as_ptr());

    if class == 2 && matches!(num, 4 | 121 | 154 | 397 | 415) {
        writetrap_check(cpu, if num == 121 { 121 } else { 4 });
    }
    if class == 2 && matches!(num, 73 | 74 | 75 | 197) {
        pagetrap_check(cpu, num);
    }
    static mut HOSTMASKLOG: c_int = -1;
    if HOSTMASKLOG < 0 {
        HOSTMASKLOG = (!libc::getenv(c"OCERZ_HOSTMASKLOG".as_ptr()).is_null()) as c_int;
    }
    if HOSTMASKLOG != 0 {
        let mut mask: libc::sigset_t = core::mem::zeroed();
        if libc::pthread_sigmask(libc::SIG_BLOCK, ptr::null(), &mut mask) == 0 {
            let mut value = 0u32;
            for sig in 1..32 {
                if libc::sigismember(&mask, sig) == 1 {
                    value |= 1u32 << sig;
                }
            }
            if value != (*cpu).host_mask_last {
                (*cpu).host_mask_changes += 1;
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: HOSTMASK[%d] cpu#%u kport=%#x mask %#x -> %#x at sys=%d/%d rip=%#llx depth=%u\n".as_ptr(),
                    libc::getpid(),
                    (*cpu).cpu_number,
                    (*cpu).host_kport,
                    (*cpu).host_mask_last,
                    value,
                    class,
                    num,
                    (*cpu).rip as libc::c_ulonglong,
                    (*cpu).in_sighandler,
                );
                (*cpu).host_mask_last = value;
            }
        }
    }
    if (*cpu).in_sighandler != 0 && env_set!("OCERZ_SIGCTXLOG") {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: SIGCTX[%d] cpu#%u depth=%u sys=%d/%d rip=%#llx a0=%#llx a1=%#llx a2=%#llx\n"
                .as_ptr(),
            libc::getpid(),
            (*cpu).cpu_number,
            (*cpu).in_sighandler,
            class,
            num,
            (*cpu).rip as libc::c_ulonglong,
            (*cpu).gpr[crate::ffi::OCERZ_RDI as usize] as libc::c_ulonglong,
            (*cpu).gpr[crate::ffi::OCERZ_RSI as usize] as libc::c_ulonglong,
            (*cpu).gpr[crate::ffi::OCERZ_RDX as usize] as libc::c_ulonglong,
        );
    }
    static SCCOUNT_ON: AtomicU32 = AtomicU32::new(u32::MAX);
    let mut count_enabled = SCCOUNT_ON.load(Ordering::Relaxed);
    if count_enabled == u32::MAX {
        let value = (!libc::getenv(c"OCERZ_SCCOUNT".as_ptr()).is_null()) as u32;
        let _ = SCCOUNT_ON.compare_exchange(u32::MAX, value, Ordering::Relaxed, Ordering::Relaxed);
        count_enabled = SCCOUNT_ON.load(Ordering::Relaxed);
    }
    if count_enabled != 0 && class == 2 {
        static HIST: [AtomicU64; 640] = [const { AtomicU64::new(0) }; 640];
        static TOTAL: AtomicU64 = AtomicU64::new(0);
        if (0..640).contains(&num) {
            HIST[num as usize].fetch_add(1, Ordering::Relaxed);
        }
        let total = TOTAL.fetch_add(1, Ordering::Relaxed).wrapping_add(1);
        if total & ((1 << 16) - 1) == 0 {
            let mut snapshot = [0u64; 640];
            for i in 0..640 {
                snapshot[i] = HIST[i].load(Ordering::Relaxed);
            }
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: SCCOUNT t=%lluM cur num=%d rip=%#llx ret=%#llx a0=%#llx a1=%#llx a2=%#llx | top:".as_ptr(),
                total >> 20,
                num,
                (*cpu).rip as libc::c_ulonglong,
                ocerz_ld((*cpu).gpr[crate::ffi::OCERZ_RSP as usize], 8) as libc::c_ulonglong,
                (*cpu).gpr[crate::ffi::OCERZ_RDI as usize] as libc::c_ulonglong,
                (*cpu).gpr[crate::ffi::OCERZ_RSI as usize] as libc::c_ulonglong,
                (*cpu).gpr[crate::ffi::OCERZ_RDX as usize] as libc::c_ulonglong,
            );
            for _ in 0..6 {
                let mut best = None;
                for i in 0..640 {
                    if best.map_or(true, |(_, value)| snapshot[i] > value) {
                        best = Some((i, snapshot[i]));
                    }
                }
                let Some((index, value)) = best else { break };
                if value == 0 {
                    break;
                }
                libc::fprintf(
                    crate::log::stderr(),
                    c" [%d]=%lluM".as_ptr(),
                    index as c_int,
                    value >> 20,
                );
                snapshot[index] = 0;
            }
            libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
        }
    }

    let result = match class {
        1 => {
            static mut MACHSLOW: c_int = -1;
            if MACHSLOW < 0 {
                MACHSLOW = (!libc::getenv(c"OCERZ_MACHSLOW".as_ptr()).is_null()) as c_int;
            }
            if MACHSLOW != 0 {
                let a0 = (*cpu).gpr[crate::ffi::OCERZ_RDI as usize];
                let a1 = (*cpu).gpr[crate::ffi::OCERZ_RSI as usize];
                let rip0 = (*cpu).rip;
                let t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                (*cpu).block_started_ns = t0;
                (*cpu).block_what = num;
                (*cpu).cur_sys_class = class;
                (*cpu).cur_sys_num = num;
                let rc = super::mach::dispatch_mach(vm, cpu, num);
                (*cpu).cur_sys_class = -1;
                (*cpu).block_started_ns = 0;
                let elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW).wrapping_sub(t0);
                if elapsed > 3_000_000_000 {
                    libc::fprintf(
                        crate::log::stderr(),
                        c"ocerz: MACHSLOW[%d] cpu#%u trap=%d dt=%llus rip=%#llx a0=%#llx a1=%#llx rcvname=%#llx kr=%#llx\n".as_ptr(),
                        libc::getpid(),
                        (*cpu).cpu_number,
                        num,
                        elapsed / 1_000_000_000,
                        rip0 as libc::c_ulonglong,
                        a0 as libc::c_ulonglong,
                        a1 as libc::c_ulonglong,
                        ((*cpu).gpr[crate::ffi::OCERZ_R9 as usize] >> 32) as libc::c_ulonglong,
                        (*cpu).gpr[crate::ffi::OCERZ_RAX as usize] as libc::c_ulonglong,
                    );
                }
                rc
            } else {
                (*cpu).cur_sys_class = class;
                (*cpu).cur_sys_num = num;
                let rc = super::mach::dispatch_mach(vm, cpu, num);
                (*cpu).cur_sys_class = -1;
                rc
            }
        }
        2 => {
            (*cpu).cur_sys_class = class;
            (*cpu).cur_sys_num = num;
            let rc = super::bsd::dispatch_bsd(vm, cpu, num);
            (*cpu).cur_sys_class = -1;
            rc
        }
        3 => {
            (*cpu).cur_sys_class = class;
            (*cpu).cur_sys_num = num;
            let rc = super::ldt::dispatch_machdep(vm, cpu, num);
            (*cpu).cur_sys_class = -1;
            rc
        }
        _ => {
            if !ocerz_gs_is_teb_band((*cpu).gs_base)
                && (*cpu).wine_teb_base != 0
                && crate::ffi::ocerz_addr_committed((*cpu).wine_teb_base) != 0
            {
                (*cpu).unix_gs_base = (*cpu).gs_base;
                (*cpu).gs_base = (*cpu).wine_teb_base;
            }
            super::signals::g_ocerz_deliver_src = 2;
            if super::signals::ocerz_signal_deliver(cpu, crate::ffi::OCERZ_SIGSYS as c_int, 0, 0, 0)
                != 0
            {
                return crate::ffi::OCERZ_STEP_OK as c_int;
            }
            crate::ocerz_fatal!(
                "unknown syscall class=%d num=%d (rax=%#llx) rip=%#llx\n",
                class,
                num,
                rax as libc::c_ulonglong,
                (*cpu).rip as libc::c_ulonglong
            );
            return crate::ffi::OCERZ_STEP_FATAL as c_int;
        }
    };
    peekguard(cpu, class, num, c"exit".as_ptr());
    sysring_exit(cpu, ring_at, class, num);
    if result == crate::ffi::OCERZ_STEP_OK as c_int {
        super::signals::deliver_async_signals(
            vm,
            cpu,
            crate::ffi::ocerz_take_pending_async_sig_mask(super::hostwq::async_accept(
                (*cpu).sig_mask,
            )),
        );
    }
    crate::ffi::ocerz_vm_suspend_point(cpu);
    result
}

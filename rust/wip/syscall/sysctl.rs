//! BSD thread registration, workqueue stubs, and the x86-compatible sysctl
//! views exposed by the syscall boundary.

use super::util::*;
use super::*;

use core::ffi::{c_char, c_int, c_void};

#[repr(C)]
struct SysctlOverride {
    name: *const c_char,
    is_str: c_int,
    sval: *const c_char,
    ival: u32,
    mib: [c_int; 8],
    miblen: usize,
    resolved: c_int,
    width: u8,
}

#[derive(Clone, Copy)]
struct X86Sysctl {
    name: &'static [u8],
    width: u8,
    val: u64,
    sval: Option<&'static [u8]>,
}

static mut G_SYSCTL_OVR: [SysctlOverride; 8] = [
    SysctlOverride {
        name: c"hw.machine".as_ptr(),
        is_str: 1,
        sval: c"x86_64".as_ptr(),
        ival: 0,
        mib: [0; 8],
        miblen: 0,
        resolved: 0,
        width: 0,
    },
    SysctlOverride {
        name: c"hw.cputype".as_ptr(),
        is_str: 0,
        sval: core::ptr::null(),
        ival: 7,
        mib: [0; 8],
        miblen: 0,
        resolved: 0,
        width: 0,
    },
    SysctlOverride {
        name: c"hw.cpusubtype".as_ptr(),
        is_str: 0,
        sval: core::ptr::null(),
        ival: 4,
        mib: [0; 8],
        miblen: 0,
        resolved: 0,
        width: 0,
    },
    SysctlOverride {
        name: c"hw.cpufamily".as_ptr(),
        is_str: 0,
        sval: core::ptr::null(),
        ival: 0x573b5eec,
        mib: [0; 8],
        miblen: 0,
        resolved: 0,
        width: 0,
    },
    SysctlOverride {
        name: c"sysctl.proc_translated".as_ptr(),
        is_str: 0,
        sval: core::ptr::null(),
        ival: 1,
        mib: [0; 8],
        miblen: 0,
        resolved: 0,
        width: 0,
    },
    SysctlOverride {
        name: c"hw.pagesize".as_ptr(),
        is_str: 0,
        sval: core::ptr::null(),
        ival: 4096,
        mib: [0; 8],
        miblen: 0,
        resolved: 0,
        width: 0,
    },
    SysctlOverride {
        name: c"hw.pagesize32".as_ptr(),
        is_str: 0,
        sval: core::ptr::null(),
        ival: 4096,
        mib: [0; 8],
        miblen: 0,
        resolved: 0,
        width: 0,
    },
    SysctlOverride {
        name: c"vm.pagesize".as_ptr(),
        is_str: 0,
        sval: core::ptr::null(),
        ival: 4096,
        mib: [0; 8],
        miblen: 0,
        resolved: 0,
        width: 0,
    },
];

static X86_SYSCTL: [X86Sysctl; 36] = [
    X86Sysctl { name: b"hw.optional.mmx\0", width: 4, val: 1, sval: None },
    X86Sysctl { name: b"hw.optional.sse\0", width: 4, val: 1, sval: None },
    X86Sysctl { name: b"hw.optional.sse2\0", width: 4, val: 1, sval: None },
    X86Sysctl { name: b"hw.optional.sse3\0", width: 4, val: 1, sval: None },
    X86Sysctl { name: b"hw.optional.supplementalsse3\0", width: 4, val: 1, sval: None },
    X86Sysctl { name: b"hw.optional.sse4_1\0", width: 4, val: 1, sval: None },
    X86Sysctl { name: b"hw.optional.sse4_2\0", width: 4, val: 1, sval: None },
    X86Sysctl { name: b"hw.optional.x86_64\0", width: 4, val: 1, sval: None },
    X86Sysctl { name: b"hw.optional.aes\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.avx1_0\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.rdrand\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.f16c\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.enfstrg\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.fma\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.avx2_0\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.bmi1\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.bmi2\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.rtm\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.hle\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.adx\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.mpx\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.sgx\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.avx512f\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.avx512cd\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.avx512dq\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.avx512bw\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.avx512vl\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.avx512ifma\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"hw.optional.avx512vbmi\0", width: 4, val: 0, sval: None },
    X86Sysctl { name: b"machdep.cpu.features\0", width: 0, val: 0, sval: Some(b"FPU VME DE PSE TSC MSR PAE MCE CX8 APIC SEP MTRR PGE MCA CMOV PAT PSE36 CLFSH MMX FXSR SSE SSE2 SSE3 SSSE3 CX16 SSE4.1 SSE4.2 POPCNT\0") },
    X86Sysctl { name: b"machdep.cpu.feature_bits\0", width: 8, val: 0x00982201078bfbff, sval: None },
    X86Sysctl { name: b"machdep.cpu.extfeatures\0", width: 0, val: 0, sval: Some(b"SYSCALL XD RDTSCP EM64T LAHF\0") },
    X86Sysctl { name: b"machdep.cpu.extfeature_bits\0", width: 8, val: 0x0000000128100800, sval: None },
    X86Sysctl { name: b"machdep.cpu.leaf7_features\0", width: 0, val: 0, sval: Some(b"\0") },
    X86Sysctl { name: b"machdep.cpu.leaf7_feature_bits\0", width: 8, val: 0, sval: None },
    X86Sysctl { name: b"machdep.cpu.family\0", width: 4, val: 6, sval: None },
];

unsafe extern "C" {
    fn sysctlnametomib(name: *const c_char, mib: *mut c_int, size: *mut usize) -> c_int;
}

pub(super) unsafe fn sys_bsdthread_register(
    _vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    a: *mut [u64; 8],
) -> c_int {
    unsafe {
        let args = &*a;
        core::sync::atomic::AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_pthread_start))
            .store(args[0], core::sync::atomic::Ordering::Release);
        core::sync::atomic::AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_wqthread_start))
            .store(args[1], core::sync::atomic::Ordering::Release);
        if !libc::getenv(c"OCERZ_THREADLOG".as_ptr()).is_null() {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: THREADREG pthread_start=%#llx wqthread_start=%#llx pthsize=%#llx data=%#llx targetconc=%#llx dqoff=%#llx flags=%#llx\n".as_ptr(),
                args[0] as libc::c_ulonglong,
                args[1] as libc::c_ulonglong,
                args[2] as libc::c_ulonglong,
                args[3] as libc::c_ulonglong,
                args[4] as libc::c_ulonglong,
                args[5] as libc::c_ulonglong,
                args[6] as libc::c_ulonglong,
            );
        }
        let mut feat = 0x4000005fu64;
        if ocerz_hostwq_on() != 0 {
            feat |= 0x80;
        }
        if !libc::getenv(c"OCERZ_NO_KEVWQ".as_ptr()).is_null() {
            feat &= !0xc0;
        }
        ret_ok(cpu, feat);
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

pub(super) unsafe fn sys_workq_stub(
    _vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    _a: *mut [u64; 8],
) -> c_int {
    ret_ok(cpu, 0);
    crate::ffi::OCERZ_STEP_OK as c_int
}

unsafe fn ocerz_sysctl_ovr_resolve() {
    static mut DONE: c_int = 0;
    unsafe {
        if DONE != 0 {
            return;
        }
        DONE = 1;
        let entries = core::ptr::addr_of_mut!(G_SYSCTL_OVR).cast::<SysctlOverride>();
        for i in 0..8 {
            let o = &mut *entries.add(i);
            o.miblen = 8;
            if sysctlnametomib(o.name, o.mib.as_mut_ptr(), &mut o.miblen) != 0 {
                continue;
            }
            o.resolved = 1;
            if o.is_str != 0 {
                continue;
            }
            let mut probe = 0u64;
            let mut plen = core::mem::size_of::<u64>();
            if libc::sysctl(
                o.mib.as_ptr() as *mut c_int,
                o.miblen as libc::c_uint,
                (&mut probe as *mut u64).cast::<c_void>(),
                &mut plen,
                core::ptr::null_mut(),
                0,
            ) == 0
                && (plen == 4 || plen == 8)
            {
                o.width = plen as u8;
            }
        }
    }
}

unsafe fn ocerz_sysctl_ovr_emit(
    cpu: *mut OcerzCPU,
    o: *const SysctlOverride,
    oldp: u64,
    oldlenp: u64,
) -> c_int {
    unsafe {
        let o = &*o;
        let need = if o.is_str != 0 {
            libc::strlen(o.sval) as u64 + 1
        } else if o.width != 0 {
            o.width as u64
        } else {
            4
        };
        if oldp != 0 {
            let cap = if oldlenp != 0 {
                ocerz_ld(oldlenp, 8)
            } else {
                need
            };
            if cap < need {
                if oldlenp != 0 {
                    ocerz_st(oldlenp, 8, need);
                }
                ret_err(cpu, OCERZ_ENOMEM_V as u64);
                return crate::ffi::OCERZ_STEP_OK as c_int;
            }
            if o.is_str != 0 {
                for k in 0..need {
                    ocerz_st(
                        oldp.wrapping_add(k),
                        1,
                        *o.sval.add(k as usize) as u8 as u64,
                    );
                }
            } else {
                ocerz_st(oldp, need as c_int, o.ival as u64);
            }
        }
        if oldlenp != 0 {
            ocerz_st(oldlenp, 8, need);
        }
        ret_ok(cpu, 0);
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

const X86_SYSCTL_MIB: c_int = 0x4f43;

unsafe fn x86_sysctl_find(name: *const c_char) -> c_int {
    unsafe {
        X86_SYSCTL
            .iter()
            .position(|entry| libc::strcmp(name, entry.name.as_ptr().cast()) == 0)
            .map_or(-1, |i| i as c_int)
    }
}

unsafe fn x86_sysctl_emit(cpu: *mut OcerzCPU, idx: c_int, oldp: u64, oldlenp: u64) -> c_int {
    unsafe {
        let entry = *X86_SYSCTL.get_unchecked(idx as usize);
        let string = entry
            .sval
            .map_or(core::ptr::null(), |s| s.as_ptr().cast::<c_char>());
        let need = if string.is_null() {
            entry.width as u64
        } else {
            libc::strlen(string) as u64 + 1
        };
        if oldp != 0 {
            let cap = if oldlenp != 0 {
                ocerz_ld(oldlenp, 8)
            } else {
                need
            };
            if cap < need {
                if oldlenp != 0 {
                    ocerz_st(oldlenp, 8, need);
                }
                ret_err(cpu, OCERZ_ENOMEM_V as u64);
                return crate::ffi::OCERZ_STEP_OK as c_int;
            }
            if !string.is_null() {
                for k in 0..need {
                    ocerz_st(
                        oldp.wrapping_add(k),
                        1,
                        *string.add(k as usize) as u8 as u64,
                    );
                }
            } else {
                ocerz_st(oldp, need as c_int, entry.val);
            }
        }
        if oldlenp != 0 {
            ocerz_st(oldlenp, 8, need);
        }
        ret_ok(cpu, 0);
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

pub(super) unsafe fn sys_sysctl(_vm: *mut OcerzVM, cpu: *mut OcerzCPU, a: *mut [u64; 8]) -> c_int {
    unsafe {
        let a = &*a;
        static LOG: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
        let mut log = LOG.load(core::sync::atomic::Ordering::Relaxed);
        if log < 0 {
            log = c_int::from(!libc::getenv(c"OCERZ_SYSCTLLOG".as_ptr()).is_null());
            LOG.store(log, core::sync::atomic::Ordering::Relaxed);
        }
        let nlen = a[1];
        if nlen == 2
            && a[4] != 0
            && a[5] != 0
            && a[5] < 160
            && ocerz_ld(a[0], 4) == 0
            && ocerz_ld(a[0].wrapping_add(4), 4) == 3
        {
            let mut name = [0 as c_char; 160];
            for i in 0..a[5] {
                name[i as usize] = ocerz_ld(a[4].wrapping_add(i), 1) as u8 as c_char;
            }
            name[a[5] as usize] = 0;
            let idx = x86_sysctl_find(name.as_ptr());
            if idx >= 0 {
                let cap = if a[3] != 0 { ocerz_ld(a[3], 8) } else { 0 };
                if a[2] == 0 || cap < 8 {
                    ret_err(cpu, OCERZ_ENOMEM_V as u64);
                    return crate::ffi::OCERZ_STEP_OK as c_int;
                }
                ocerz_st(a[2], 4, X86_SYSCTL_MIB as u32 as u64);
                ocerz_st(a[2].wrapping_add(4), 4, idx as u64);
                ocerz_st(a[3], 8, 8);
                ret_ok(cpu, 0);
                return crate::ffi::OCERZ_STEP_OK as c_int;
            }
        }
        if a[4] == 0 && nlen == 2 && ocerz_ld(a[0], 4) == X86_SYSCTL_MIB as u64 {
            let idx = ocerz_ld(a[0].wrapping_add(4), 4) as u32;
            if (idx as usize) < X86_SYSCTL.len() {
                return x86_sysctl_emit(cpu, idx as c_int, a[2], a[3]);
            }
        }
        if a[4] == 0 && (2..=8).contains(&nlen) {
            let mut mib = [0 as c_int; 8];
            for i in 0..nlen as usize {
                mib[i] = ocerz_ld(a[0].wrapping_add(i as u64 * 4), 4) as u32 as i32;
            }
            ocerz_sysctl_ovr_resolve();
            if log != 0 {
                libc::fprintf(
                    crate::log::stderr(),
                    c"ocerz: sysctl mib=%d.%d nlen=%llu oldp=%#llx oldlenp=%#llx\n".as_ptr(),
                    mib[0],
                    if nlen > 1 { mib[1] } else { -1 },
                    nlen as libc::c_ulonglong,
                    a[2] as libc::c_ulonglong,
                    a[3] as libc::c_ulonglong,
                );
            }
            let entries = core::ptr::addr_of!(G_SYSCTL_OVR).cast::<SysctlOverride>();
            for i in 0..8 {
                let o = &*entries.add(i);
                if o.resolved == 0 || o.miblen != nlen as usize {
                    continue;
                }
                if libc::memcmp(
                    mib.as_ptr().cast(),
                    o.mib.as_ptr().cast(),
                    nlen as usize * core::mem::size_of::<c_int>(),
                ) == 0
                {
                    return ocerz_sysctl_ovr_emit(cpu, o, a[2], a[3]);
                }
            }
            if nlen == 2 && mib[0] == 6 && mib[1] == 7 {
                let pgo = SysctlOverride {
                    name: c"hw.pagesize(legacy)".as_ptr(),
                    is_str: 0,
                    sval: core::ptr::null(),
                    ival: 4096,
                    mib: [0; 8],
                    miblen: 2,
                    resolved: 1,
                    width: 4,
                };
                return ocerz_sysctl_ovr_emit(cpu, &pgo, a[2], a[3]);
            }
        }
        let mut fa = *a;
        for i in 0..8 {
            if (0x1d & (1 << i)) != 0 && fa[i] != 0 {
                fa[i] = ocerz_g2h(fa[i]) as usize as u64;
            }
        }
        let mut err = 0;
        let mut ret2 = 0;
        let result = raw::ocerz_host_syscall(202, &fa, &mut ret2, &mut err);
        if err != 0 {
            ret_err(cpu, result);
        } else {
            ret_ok(cpu, result);
        }
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

unsafe fn sys_mac_syscall_log(cpu: *mut OcerzCPU, a: *mut [u64; 8]) {
    unsafe {
        let _ = cpu;
        let a = &*a;
        let mut pol = [0 as c_char; 32];
        let mut i = 0;
        while a[0] != 0 && i < 31 {
            pol[i] = ocerz_ld(a[0].wrapping_add(i as u64), 1) as u8 as c_char;
            if pol[i] == 0 {
                break;
            }
            i += 1;
        }
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: MACSYS[%d] policy=%s call=%#llx arg=%#llx:".as_ptr(),
            libc::getpid(),
            pol.as_ptr(),
            a[1] as libc::c_ulonglong,
            a[2] as libc::c_ulonglong,
        );
        for i in 0..8 {
            let at = a[2].wrapping_add(i * 8);
            if a[2] == 0 || crate::ffi::ocerz_addr_readable(at) == 0 {
                break;
            }
            libc::fprintf(
                crate::log::stderr(),
                c" %#llx".as_ptr(),
                ocerz_ld(at, 8) as libc::c_ulonglong,
            );
        }
        libc::fprintf(crate::log::stderr(), c"\n".as_ptr());
    }
}

pub(super) unsafe fn sys_mac_syscall(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    a: *mut [u64; 8],
) -> c_int {
    unsafe {
        let a = &*a;
        static LOG: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
        let mut log = LOG.load(core::sync::atomic::Ordering::Relaxed);
        if log < 0 {
            log = c_int::from(!libc::getenv(c"OCERZ_MACSYSLOG".as_ptr()).is_null());
            LOG.store(log, core::sync::atomic::Ordering::Relaxed);
        }
        if log != 0 {
            sys_mac_syscall_log(cpu, a as *const [u64; 8] as *mut [u64; 8]);
        }
        let mut fa = *a;
        if fa[0] != 0 {
            fa[0] = ocerz_g2h(fa[0]) as usize as u64;
        }
        let mut slots = [0u64; 12];
        let mut orig = [0u64; 12];
        let mut ns = 0;
        if a[2] != 0 {
            let sp = (*cpu).gpr[crate::ffi::OCERZ_RSP as usize];
            let lo = if sp > 0x10000 { sp - 0x10000 } else { 0 };
            let hi = sp.wrapping_add(0x100000);
            static OFF: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
            let mut off = OFF.load(core::sync::atomic::Ordering::Relaxed);
            if off < 0 {
                off = c_int::from(!libc::getenv(c"OCERZ_NO_MACSYS_XLATE".as_ptr()).is_null());
                OFF.store(off, core::sync::atomic::Ordering::Relaxed);
            }
            let mut i = 0;
            while off == 0
                && i < 12
                && crate::ffi::ocerz_addr_readable(a[2].wrapping_add(i * 8 + 7)) != 0
            {
                let at = a[2].wrapping_add(i * 8);
                let w = ocerz_ld(at, 8);
                let hw = if w >= lo && w < hi {
                    ocerz_g2h(w) as usize as u64
                } else {
                    w
                };
                if hw != w {
                    slots[ns] = at;
                    orig[ns] = w;
                    ns += 1;
                    ocerz_st(at, 8, hw);
                }
                i += 1;
            }
            fa[2] = ocerz_g2h(a[2]) as usize as u64;
        }
        let mut err = 0;
        let mut ret2 = 0;
        let result = raw::ocerz_host_syscall(381, &fa, &mut ret2, &mut err);
        while ns > 0 {
            ns -= 1;
            ocerz_st(slots[ns], 8, orig[ns]);
        }
        if err != 0 {
            ret_err(cpu, result);
        } else {
            ret_ok(cpu, result);
        }
        let _ = vm;
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

pub(super) unsafe fn sys_sysctlbyname(
    _vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    a: *mut [u64; 8],
) -> c_int {
    unsafe {
        let a = &*a;
        let mut name = [0 as c_char; 160];
        let nl = a[1].min(159);
        for i in 0..nl {
            name[i as usize] = ocerz_ld(a[0].wrapping_add(i), 1) as u8 as c_char;
        }
        name[nl as usize] = 0;
        static LOG: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
        let mut log = LOG.load(core::sync::atomic::Ordering::Relaxed);
        if log < 0 {
            log = c_int::from(!libc::getenv(c"OCERZ_SYSCTLLOG".as_ptr()).is_null());
            LOG.store(log, core::sync::atomic::Ordering::Relaxed);
        }
        if log != 0 {
            libc::fprintf(
                crate::log::stderr(),
                c"ocerz: sysctlbyname '%s' oldp=%#llx\n".as_ptr(),
                name.as_ptr(),
                a[2] as libc::c_ulonglong,
            );
        }
        if libc::strcmp(name.as_ptr(), c"sysctl.proc_translated".as_ptr()) == 0 {
            if a[3] != 0 {
                let cap = ocerz_ld(a[3], 8);
                if a[2] != 0 && cap >= 4 {
                    ocerz_st(a[2], 4, 1);
                }
                ocerz_st(a[3], 8, 4);
            }
            ret_ok(cpu, 0);
            return crate::ffi::OCERZ_STEP_OK as c_int;
        }
        let x86 = x86_sysctl_find(name.as_ptr());
        if x86 >= 0 {
            return x86_sysctl_emit(cpu, x86, a[2], a[3]);
        }
        let mut fa = *a;
        for i in 0..8 {
            if (0x1d & (1 << i)) != 0 && fa[i] != 0 {
                fa[i] = ocerz_g2h(fa[i]) as usize as u64;
            }
        }
        let mut err = 0;
        let mut ret2 = 0;
        let result = raw::ocerz_host_syscall(274, &fa, &mut ret2, &mut err);
        if err != 0 {
            ret_err(cpu, result);
        } else {
            ret_ok(cpu, result);
        }
        crate::ffi::OCERZ_STEP_OK as c_int
    }
}

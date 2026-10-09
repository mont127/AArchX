//! Direct arm64 kernel entry points used when the C implementation bypasses
//! libSystem. The register constraints match `include/ocerz/sys_raw.h`.

#[inline(always)]
pub(super) unsafe fn ocerz_host_syscall(
    num: i64,
    a: *const [u64; 8],
    ret2: *mut u64,
    err: *mut core::ffi::c_int,
) -> u64 {
    let args = unsafe { &*a };
    let mut ret0 = args[0];
    let mut ret1 = args[1];
    let carry: u64;
    unsafe {
        core::arch::asm!(
            "svc #0x80",
            "cset {carry}, cs",
            inout("x0") ret0,
            inout("x1") ret1,
            in("x2") args[2],
            in("x3") args[3],
            in("x4") args[4],
            in("x5") args[5],
            in("x6") args[6],
            in("x7") args[7],
            in("x16") num,
            carry = lateout(reg) carry,
        );
        if !ret2.is_null() {
            *ret2 = ret1;
        }
        if !err.is_null() {
            *err = carry as core::ffi::c_int;
        }
    }
    ret0
}

#[inline(always)]
pub(super) unsafe fn ocerz_host_mach_trap(trap: i64, a: *const [u64; 8]) -> u64 {
    unsafe {
        ocerz_host_syscall(
            trap.wrapping_neg(),
            a,
            core::ptr::null_mut(),
            core::ptr::null_mut(),
        )
    }
}

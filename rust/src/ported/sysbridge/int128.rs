//! ---- 128-bit arithmetic ----
//! compiler-rt's __udivti3, __umodti3, __divti3, __modti3, __udivmodti4,
//! __clzti2 and the __fix conversions to 128-bit integers are what a compiler
//! calls for __int128 division and conversion, and no header declares them.
//! System V passes a 128-bit integer in two registers, low half first, and
//! returns one in RAX and RDX, so each is computed here from the guest's
//! registers.  Dividing by zero raises SIGFPE with FPE_INTDIV, as the real
//! routine's divide instruction does under Rosetta, and a guest with no handler
//! for it dies of it.  A conversion out of range saturates, as compiler-rt's
//! does: to the largest or smallest value by the sign, a NaN by its sign bit,
//! and to zero for an unsigned one below zero.

use core::ffi::{c_int, c_ulonglong};

use super::common::*;
use crate::ffi::*;

unsafe fn sb_arg128(cpu: *const OcerzCPU, first: c_int) -> u128 {
    unsafe { sb_arg(cpu, first) as u128 | (sb_arg(cpu, first + 1) as u128) << 64 }
}

unsafe fn sb_ret128(vm: *mut OcerzVM, cpu: *mut OcerzCPU, v: u128) -> c_int {
    unsafe {
        (*cpu).gpr[OCERZ_RDX as usize] = (v >> 64) as u64;
        sb_ret(vm, cpu, v as u64 as i64)
    }
}

unsafe fn sb_div128_zero(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let at = ocerz_ld((*cpu).gpr[OCERZ_RSP as usize], 8);
        (*cpu).gpr[OCERZ_RSP as usize] += 8;
        (*cpu).rip = at;
        if ocerz_signal_deliver(cpu, OCERZ_SIGFPE as c_int, at, OCERZ_FPE_INTDIV as c_int, 0) != 0 {
            return ocerz_bridge_settle(vm, cpu);
        }
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: guest divided a 128-bit integer by zero, near rip=%#llx\n".as_ptr(),
            at as c_ulonglong,
        );
        libc::fflush(crate::log::stderr());
        libc::signal(libc::SIGFPE, libc::SIG_DFL);
        libc::raise(libc::SIGFPE);
        libc::abort();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_udivti3(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let a = sb_arg128(cpu, 0);
        let b = sb_arg128(cpu, 2);
        if b != 0 { sb_ret128(vm, cpu, a / b) } else { sb_div128_zero(vm, cpu) }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_umodti3(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let a = sb_arg128(cpu, 0);
        let b = sb_arg128(cpu, 2);
        if b != 0 { sb_ret128(vm, cpu, a % b) } else { sb_div128_zero(vm, cpu) }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_udivmodti4(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let a = sb_arg128(cpu, 0);
        let b = sb_arg128(cpu, 2);
        let rem = sb_arg(cpu, 4);
        if b == 0 {
            return sb_div128_zero(vm, cpu);
        }
        if rem != 0 {
            let r = a % b;
            ocerz_st(rem, 8, r as u64);
            ocerz_st(rem + 8, 8, (r >> 64) as u64);
        }
        sb_ret128(vm, cpu, a / b)
    }
}

fn sb_i128_min() -> i128 {
    (1u128 << 127) as i128
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_divti3(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let a = sb_arg128(cpu, 0) as i128;
        let b = sb_arg128(cpu, 2) as i128;
        if b == 0 {
            return sb_div128_zero(vm, cpu);
        }
        sb_ret128(
            vm,
            cpu,
            if b == -1 { 0u128.wrapping_sub(a as u128) } else { (a / b) as u128 },
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_modti3(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let a = sb_arg128(cpu, 0) as i128;
        let b = sb_arg128(cpu, 2) as i128;
        if b == 0 {
            return sb_div128_zero(vm, cpu);
        }
        sb_ret128(vm, cpu, if b == -1 { 0 } else { (a % b) as u128 })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_clzti2(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let a = sb_arg128(cpu, 0);
        let hi = (a >> 64) as u64;
        let lo = a as u64;
        let n = if hi != 0 {
            hi.leading_zeros() as i64
        } else if lo != 0 {
            64 + lo.leading_zeros() as i64
        } else {
            128
        };
        sb_ret(vm, cpu, n)
    }
}

unsafe fn sb_xmm0(cpu: *const OcerzCPU, single: c_int) -> f64 {
    unsafe {
        if single != 0 {
            let w = (*cpu).xmm[0].lo as u32;
            f64::from(f32::from_bits(w))
        } else {
            f64::from_bits((*cpu).xmm[0].lo)
        }
    }
}

fn sb_fix_signed(d: f64) -> u128 {
    if d.is_nan() {
        return if d.is_sign_negative() {
            sb_i128_min() as u128
        } else {
            (sb_i128_min() as u128).wrapping_sub(1)
        };
    }
    if d >= f64::from_bits(0x47e0000000000000) {
        return (sb_i128_min() as u128).wrapping_sub(1);
    }
    if d < -f64::from_bits(0x47e0000000000000) {
        return sb_i128_min() as u128;
    }
    (d as i128) as u128
}

fn sb_fix_unsigned(d: f64) -> u128 {
    if d.is_nan() {
        return if d.is_sign_negative() { 0 } else { !0u128 };
    }
    if d < 0.0 {
        return 0;
    }
    if d >= f64::from_bits(0x47f0000000000000) {
        return !0u128;
    }
    d as u128
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_fixdfti(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { sb_ret128(vm, cpu, sb_fix_signed(sb_xmm0(cpu, 0))) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_fixsfti(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { sb_ret128(vm, cpu, sb_fix_signed(sb_xmm0(cpu, 1))) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_fixunsdfti(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { sb_ret128(vm, cpu, sb_fix_unsigned(sb_xmm0(cpu, 0))) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_fixunssfti(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { sb_ret128(vm, cpu, sb_fix_unsigned(sb_xmm0(cpu, 1))) }
}

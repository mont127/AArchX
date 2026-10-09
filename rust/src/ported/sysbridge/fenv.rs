//! ---- the floating-point environment ----
//! fenv.h's functions read and change the guest's x87 control and status words
//! and its MXCSR, which live in OcerzCPU, and x86 numbers its exception flags
//! and rounding modes differently from arm64, so none of them can be the host's
//! call: a bridged fesetround(FE_DOWNWARD) asked the host for an arm64 mode that
//! does not exist and failed, and the crossing put the host's mode back after it
//! anyway.  The handlers do what x86 libm does, as running it under Rosetta
//! shows: fegetround reads the x87 control word, fesetround sets both rounding
//! fields and hands an unknown mode back as its nonzero answer, feraiseexcept
//! sets inexact along with overflow or underflow the way the arithmetic it
//! performs does, fegetenv fills all sixteen bytes of the x86 fenv_t, and
//! feholdexcept masks every exception after saving.  The flags the guest's
//! arithmetic raised are where the host's arithmetic raised them, in FPSR,
//! since translated SSE and x87 instructions are arm64 floating-point ones, so
//! the handlers read and clear those along with the flags in OcerzCPU, which
//! ldmxcsr and the earlier handlers left there.  Arithmetic in a native call
//! raises them too, as x86 libm's own arithmetic would.  An exception the guest
//! unmasked does not trap.  _FE_DFL_ENV and _FE_DFL_DISABLE_SSE_DENORMS_ENV are
//! var records holding the x86 defaults (src/vdylib.c).

use core::ffi::c_int;

use super::common::*;
use crate::ffi::*;

const SB_FE_ALL: u32 = 0x3f;
#[allow(dead_code)]
const SB_FE_INVALID: u32 = 0x01;
const SB_FE_OVERFLOW: u32 = 0x08;
const SB_FE_UNDERFLOW: u32 = 0x10;
const SB_FE_INEXACT: u32 = 0x20;
const SB_FE_ROUND: u32 = 0xc00;
const SB_FE_MASKS: u32 = 0x1f80;

const G_SB_FE_FLAGS: [(u32, u64); 6] = [
    (0x01, 1 << 0),
    (0x04, 1 << 1),
    (0x08, 1 << 2),
    (0x10, 1 << 3),
    (0x20, 1 << 4),
    (0x02, 1 << 7),
];

#[inline(always)]
fn sb_fpsr() -> u64 {
    let v: u64;
    unsafe {
        core::arch::asm!("mrs {}, fpsr", out(reg) v, options(nomem, nostack));
    }
    v
}

#[inline(always)]
fn sb_set_fpsr(v: u64) {
    unsafe {
        core::arch::asm!("msr fpsr, {}", in(reg) v, options(nomem, nostack));
    }
}

unsafe fn sb_fe_raised(cpu: *const OcerzCPU) -> u32 {
    unsafe {
        let fpsr = sb_fpsr();
        let mut f = ((*cpu).mxcsr | (*cpu).fsw as u32) & SB_FE_ALL;
        for k in 0..G_SB_FE_FLAGS.len() {
            if fpsr & G_SB_FE_FLAGS.get_unchecked(k).1 != 0 {
                f |= G_SB_FE_FLAGS.get_unchecked(k).0;
            }
        }
        f
    }
}

unsafe fn sb_fe_clear(cpu: *mut OcerzCPU, e: u32) {
    unsafe {
        let fpsr = sb_fpsr();
        let mut keep = fpsr;
        let e = e & SB_FE_ALL;
        for k in 0..G_SB_FE_FLAGS.len() {
            if e & G_SB_FE_FLAGS.get_unchecked(k).0 != 0 {
                keep &= !G_SB_FE_FLAGS.get_unchecked(k).1;
            }
        }
        if keep != fpsr {
            sb_set_fpsr(keep);
        }
        (*cpu).mxcsr &= !e;
        (*cpu).fsw &= !e as u16;
    }
}

unsafe fn sb_fe_raise(cpu: *mut OcerzCPU, e: u32) {
    unsafe {
        let mut e = e & SB_FE_ALL;
        if e & (SB_FE_OVERFLOW | SB_FE_UNDERFLOW) != 0 {
            e |= SB_FE_INEXACT;
        }
        (*cpu).mxcsr |= e;
    }
}

unsafe fn sb_fe_store(cpu: *mut OcerzCPU, env: u64) {
    unsafe {
        ocerz_st(env, 2, (*cpu).fcw as u64);
        ocerz_st(
            env + 2,
            2,
            (((*cpu).fsw & !(7u16 << 11)) as u32 | ((*cpu).ftop as u32 & 7) << 11) as u64,
        );
        ocerz_st(env + 4, 4, ((*cpu).mxcsr | sb_fe_raised(cpu)) as u64);
        ocerz_st(env + 8, 8, 0);
    }
}

unsafe fn sb_fe_load(cpu: *mut OcerzCPU, env: u64) {
    unsafe {
        sb_fe_clear(cpu, SB_FE_ALL);
        (*cpu).fcw = ocerz_ld(env, 2) as u16;
        (*cpu).fsw = (ocerz_ld(env + 2, 2) & !(7u64 << 11)) as u16;
        (*cpu).mxcsr = ocerz_ld(env + 4, 4) as u32;
        ocerz_apply_mxcsr_round((*cpu).mxcsr);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_feclearexcept(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        sb_fe_clear(cpu, sb_arg(cpu, 0) as u32);
        sb_ret(vm, cpu, 0)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_feraiseexcept(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        sb_fe_raise(cpu, sb_arg(cpu, 0) as u32);
        sb_ret(vm, cpu, 0)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_fetestexcept(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { sb_ret(vm, cpu, (sb_fe_raised(cpu) & sb_arg(cpu, 0) as u32) as i64) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_fegetexceptflag(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        ocerz_st(sb_arg(cpu, 0), 2, (sb_fe_raised(cpu) & sb_arg(cpu, 1) as u32) as u64);
        sb_ret(vm, cpu, 0)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_fesetexceptflag(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let e = sb_arg(cpu, 1) as u32 & SB_FE_ALL;
        let want = ocerz_ld(sb_arg(cpu, 0), 2) as u32 & e;
        sb_fe_clear(cpu, e);
        (*cpu).mxcsr |= want;
        sb_ret(vm, cpu, 0)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_fegetround(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { sb_ret(vm, cpu, ((*cpu).fcw as u32 & SB_FE_ROUND) as i64) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_fesetround(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let mode = sb_arg(cpu, 0) as c_int;
        if (mode as u32) & !SB_FE_ROUND != 0 {
            return sb_ret(vm, cpu, mode as i64);
        }
        (*cpu).fcw = (((*cpu).fcw as u32 & !SB_FE_ROUND) | mode as u32) as u16;
        (*cpu).mxcsr = ((*cpu).mxcsr & !(3 << 13)) | (mode as u32) << 3;
        ocerz_apply_mxcsr_round((*cpu).mxcsr);
        sb_ret(vm, cpu, 0)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_fegetenv(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        sb_fe_store(cpu, sb_arg(cpu, 0));
        sb_ret(vm, cpu, 0)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_fesetenv(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        sb_fe_load(cpu, sb_arg(cpu, 0));
        sb_ret(vm, cpu, 0)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_feholdexcept(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        sb_fe_store(cpu, sb_arg(cpu, 0));
        sb_fe_clear(cpu, SB_FE_ALL);
        (*cpu).fcw |= SB_FE_ALL as u16;
        (*cpu).mxcsr |= SB_FE_MASKS;
        sb_ret(vm, cpu, 0)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_feupdateenv(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let raised = sb_fe_raised(cpu);
        sb_fe_load(cpu, sb_arg(cpu, 0));
        (*cpu).mxcsr |= raised;
        sb_ret(vm, cpu, 0)
    }
}

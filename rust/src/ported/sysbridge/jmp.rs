//! ---- non-local jumps ----
//! setjmp and its relatives save and restore machine state, and the host's would
//! save arm64 registers where the guest meant its x86 ones, so they work on the
//! guest's cpu.  The jmp_buf is Apple's x86_64 layout as its libplatform lays it
//! out, confirmed by running the x86_64 routines under Rosetta against a filled
//! buffer and reading their code: rbx at 0, rbp at 8, the caller's rsp at 16,
//! r12 through r15 at 24 to 48, the return rip at 56, MXCSR at 72, the x87
//! control word at 76, the signal mask at 80, sigsetjmp's savemask at 84 and the
//! alternate-stack flags at 88.  rbp, rsp and rip are XORed with the pointer
//! token at gs:0x38, as Apple's are; the thread blocks native mode builds hold
//! zero there, so they are stored as they are.  setjmp and sigsetjmp with a
//! nonzero savemask also save the guest's signal mask and whether it is on its
//! alternate stack, through the same entry points sigprocmask and sigaltstack
//! use, and longjmp and siglongjmp of such a buffer put both back, the second the
//! way _sigunaltstack tells the kernel.  A jump restores the callee-saved
//! registers, rsp and rip, reinitializes the x87 unit and loads the saved control
//! word and MXCSR, which sets the host's rounding mode too, clears the direction
//! flag, and returns the value, 1 in place of 0.  Any signal the restored mask
//! unblocks is delivered on top of the state the jump arrived at.  A jump out of
//! a signal handler is an ordinary jump: the handler runs on the same cpu at the
//! same level as the code it interrupted, and the frame it leaves behind on the
//! guest stack is simply abandoned, as it is natively.
//!
//! A jump may not leave native frames behind.  Guest code a native function
//! called back, a qsort comparator, a dispatch work function or a thread's start
//! routine, runs on the same host thread above that function's frames, and a
//! longjmp from there to a setjmp taken before the call would continue the guest
//! with the native frames still underneath it, holding whatever locks and state
//! they held, and with the callback's own run loop running code it was never
//! handed.  ocerz could only make that safe by unwinding the host side to the
//! crossing the setjmp was taken outside, which would skip the native function's
//! own cleanup exactly as the guest's jump skips its frames, and a native
//! function is not written to be abandoned.  So such a jump is refused by name,
//! naming the native call whose frames it would skip, and stops with 72.  To
//! know, setjmp writes the bridge level it runs at (bridge.h) into the unused
//! tail of the buffer at 104, behind a marker at 96, and a jump compares it with
//! the level it runs at: equal is performed, a level still open further out is
//! the refusal above, and anything else is a jump to a setjmp whose call has
//! returned or that another thread took, refused as well.  A buffer without the
//! marker was filled by something other than these setjmps and is jumped to at
//! the outermost level, where no native frames can be underneath, and refused
//! inside a callback, where they can.

use core::ffi::{c_char, c_int};

use super::common::*;
use crate::ffi::*;
use crate::inline::OCERZ_DF;

const JB_RBX: u64 = 0;
const JB_RBP: u64 = 8;
const JB_RSP: u64 = 16;
const JB_R12: u64 = 24;
const JB_R13: u64 = 32;
const JB_R14: u64 = 40;
const JB_R15: u64 = 48;
const JB_RIP: u64 = 56;
const JB_MXCSR: u64 = 72;
const JB_FPCW: u64 = 76;
const JB_TSD_PTR_MUNGE: u64 = 0x38;

unsafe fn sb_jmp_token(cpu: *const OcerzCPU) -> u64 {
    unsafe {
        let slot = (*cpu).gs_base + JB_TSD_PTR_MUNGE;
        if (*cpu).gs_base != 0 && ocerz_addr_readable(slot) != 0 {
            ocerz_ld(slot, 8)
        } else {
            0
        }
    }
}

unsafe fn sb_jmp_save(vm: *mut OcerzVM, cpu: *mut OcerzCPU, env: u64, save_mask: c_int) {
    unsafe {
        let rsp = (*cpu).gpr[OCERZ_RSP as usize];
        let tok = sb_jmp_token(cpu);
        ocerz_st(env + JB_RBX, 8, (*cpu).gpr[OCERZ_RBX as usize]);
        ocerz_st(env + JB_RBP, 8, (*cpu).gpr[OCERZ_RBP as usize] ^ tok);
        ocerz_st(env + JB_RSP, 8, (rsp + 8) ^ tok);
        ocerz_st(env + JB_R12, 8, (*cpu).gpr[OCERZ_R12 as usize]);
        ocerz_st(env + JB_R13, 8, (*cpu).gpr[OCERZ_R13 as usize]);
        ocerz_st(env + JB_R14, 8, (*cpu).gpr[OCERZ_R14 as usize]);
        ocerz_st(env + JB_R15, 8, (*cpu).gpr[OCERZ_R15 as usize]);
        ocerz_st(env + JB_RIP, 8, ocerz_ld(rsp, 8) ^ tok);
        ocerz_st(env + JB_MXCSR, 4, (*cpu).mxcsr as u64);
        ocerz_st(env + JB_FPCW, 2, (*cpu).fcw as u64);
        if save_mask != 0 {
            ocerz_guest_sigprocmask(vm, cpu, libc::SIG_BLOCK, 0, env + OCERZ_JB_MASK as u64);
            ocerz_st(
                env + OCERZ_JB_ONSSTACK as u64,
                4,
                ocerz_guest_altstack_flags(cpu) as u64,
            );
        }
        ocerz_st(env + OCERZ_JB_OCERZ_MAGIC as u64, 8, OCERZ_JB_MAGIC as u64);
        ocerz_st(env + OCERZ_JB_OCERZ_LEVEL as u64, 8, ocerz_bridge_level());
    }
}

unsafe fn sb_jmp_check(env: u64, sym: *const c_char) {
    unsafe {
        let cb = ocerz_bridge_callback_frame();
        let call = if !cb.is_null() && !(*cb).sym.is_null() {
            (*cb).sym
        } else {
            c"native code".as_ptr()
        };
        let mut why = [0 as c_char; 512];
        if ocerz_ld(env + OCERZ_JB_OCERZ_MAGIC as u64, 8) != OCERZ_JB_MAGIC as u64 {
            if cb.is_null() {
                return;
            }
            libc::snprintf(
                why.as_mut_ptr(),
                why.len(),
                c"was handed a jmp_buf no setjmp of ocerz's filled, inside a callback %s made, so it cannot tell whether the jump would skip %s's native frames; refused".as_ptr(),
                call,
                call,
            );
            sb_refuse(sym, why.as_ptr());
        }
        let want = ocerz_ld(env + OCERZ_JB_OCERZ_LEVEL as u64, 8);
        if want == ocerz_bridge_level() {
            return;
        }
        let mut f = cb;
        while !f.is_null() {
            if (*f).level == want {
                libc::snprintf(
                    why.as_mut_ptr(),
                    why.len(),
                    c"from inside a callback %s made, to a setjmp taken outside that call, would skip the native frames of %s; refused".as_ptr(),
                    call,
                    call,
                );
                sb_refuse(sym, why.as_ptr());
            }
            f = (*f).around;
        }
        sb_refuse(
            sym,
            c"to a setjmp whose call has returned, or that another thread took; refused".as_ptr(),
        );
    }
}

unsafe fn sb_jmp(
    vm: *mut OcerzVM,
    cpu: *mut OcerzCPU,
    env: u64,
    val: u32,
    restore_mask: c_int,
    sym: *const c_char,
) -> c_int {
    unsafe {
        sb_jmp_check(env, sym);
        if restore_mask != 0 {
            ocerz_guest_sigprocmask(vm, cpu, libc::SIG_SETMASK, env + OCERZ_JB_MASK as u64, 0);
            ocerz_guest_set_onstack(
                cpu,
                ((ocerz_ld(env + OCERZ_JB_ONSSTACK as u64, 4) & libc::SS_ONSTACK as u64) != 0)
                    as c_int,
            );
        }
        let tok = sb_jmp_token(cpu);
        (*cpu).gpr[OCERZ_RBX as usize] = ocerz_ld(env + JB_RBX, 8);
        (*cpu).gpr[OCERZ_RBP as usize] = ocerz_ld(env + JB_RBP, 8) ^ tok;
        (*cpu).gpr[OCERZ_R12 as usize] = ocerz_ld(env + JB_R12, 8);
        (*cpu).gpr[OCERZ_R13 as usize] = ocerz_ld(env + JB_R13, 8);
        (*cpu).gpr[OCERZ_R14 as usize] = ocerz_ld(env + JB_R14, 8);
        (*cpu).gpr[OCERZ_R15 as usize] = ocerz_ld(env + JB_R15, 8);
        (*cpu).rip = ocerz_ld(env + JB_RIP, 8) ^ tok;
        (*cpu).gpr[OCERZ_RSP as usize] = ocerz_ld(env + JB_RSP, 8) ^ tok;
        (*cpu).fsw = 0;
        (*cpu).ftw = 0;
        (*cpu).ftop = 0;
        (*cpu).fcw = ocerz_ld(env + JB_FPCW, 2) as u16;
        (*cpu).mxcsr = ocerz_ld(env + JB_MXCSR, 4) as u32;
        ocerz_apply_mxcsr_round((*cpu).mxcsr);
        (*cpu).rflags &= !OCERZ_DF;
        (*cpu).gpr[OCERZ_RAX as usize] = if val != 0 { val as u64 } else { 1 };
        ocerz_bridge_settle(vm, cpu)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_setjmp(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        sb_jmp_save(vm, cpu, sb_arg(cpu, 0), 1);
        sb_ret(vm, cpu, 0)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys__setjmp(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        sb_jmp_save(vm, cpu, sb_arg(cpu, 0), 0);
        sb_ret(vm, cpu, 0)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_sigsetjmp(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let env = sb_arg(cpu, 0);
        let savemask = sb_arg(cpu, 1) as u32;
        ocerz_st(env + OCERZ_JB_SAVEMASK as u64, 4, savemask as u64);
        sb_jmp_save(vm, cpu, env, (savemask != 0) as c_int);
        sb_ret(vm, cpu, 0)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_longjmp(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { sb_jmp(vm, cpu, sb_arg(cpu, 0), sb_arg(cpu, 1) as u32, 1, c"_longjmp".as_ptr()) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys__longjmp(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe { sb_jmp(vm, cpu, sb_arg(cpu, 0), sb_arg(cpu, 1) as u32, 0, c"__longjmp".as_ptr()) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_sys_siglongjmp(vm: *mut OcerzVM, cpu: *mut OcerzCPU) -> c_int {
    unsafe {
        let env = sb_arg(cpu, 0);
        sb_jmp(
            vm,
            cpu,
            env,
            sb_arg(cpu, 1) as u32,
            (ocerz_ld(env + OCERZ_JB_SAVEMASK as u64, 4) != 0) as c_int,
            c"_siglongjmp".as_ptr(),
        )
    }
}

//! Rust port of `src/jit_fp.c`: NaN fixup machinery, FP batch scanning and
//! replay, the L0 xmm lane cache (incl. ymmh caching), and the x87 stack
//! machine emitter. Split along the C file's own sections into `nan`, `fpb`,
//! `l0` and `x87`; the block-level shared state the whole file used lives
//! here.

use core::ffi::{CStr, c_int, c_uint};

use crate::ffi;

pub(crate) mod fpb;
pub(crate) mod l0;
pub(crate) mod nan;
pub(crate) mod x87;

#[unsafe(no_mangle)]
pub static mut g_cur_insn_idx: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_cur_blk: *mut ffi::JitBlock = core::ptr::null_mut();

#[unsafe(no_mangle)]
pub static mut g_keep: *mut u8 = core::ptr::null_mut();

#[unsafe(no_mangle)]
pub static mut g_keep_n: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_cur_insns: *const ffi::X86Insn = core::ptr::null();

#[unsafe(no_mangle)]
pub static mut g_cur_insns_n: c_int = 0;

pub(crate) const JGB: c_int = ffi::JGB as c_int;
pub(crate) const JT0: c_int = ffi::JT0 as c_int;
pub(crate) const JT1: c_int = ffi::JT1 as c_int;
pub(crate) const JT2: c_int = ffi::JT2 as c_int;
pub(crate) const JTF: c_int = ffi::JTF as c_int;
pub(crate) const JTT: c_int = ffi::JTT as c_int;
pub(crate) const JTU: c_int = ffi::JTU as c_int;
pub(crate) const JTA: c_int = ffi::JTA as c_int;
pub(crate) const VX0: c_int = ffi::VX0 as c_int;
pub(crate) const VX1: c_int = ffi::VX1 as c_int;
pub(crate) const VX2: c_int = ffi::VX2 as c_int;
pub(crate) const VX3: c_int = ffi::VX3 as c_int;
pub(crate) const X87S: c_int = ffi::X87S as c_int;
pub(crate) const X87P: c_int = ffi::X87P as c_int;
pub(crate) const X87Q: c_int = ffi::X87Q as c_int;
pub(crate) const TCR_BLK: c_int = ffi::TCR_BLK as c_int;
pub(crate) const A64_EQ: c_int = ffi::A64_EQ as c_int;
pub(crate) const A64_NE: c_int = ffi::A64_NE as c_int;
pub(crate) const A64_CS: c_int = ffi::A64_CS as c_int;
pub(crate) const A64_CC: c_int = ffi::A64_CC as c_int;
pub(crate) const A64_MI: c_int = ffi::A64_MI as c_int;
pub(crate) const A64_VS: c_int = ffi::A64_VS as c_int;
pub(crate) const A64_VC: c_int = ffi::A64_VC as c_int;
pub(crate) const A64_HI: c_int = ffi::A64_HI as c_int;
pub(crate) const A64_LS: c_int = ffi::A64_LS as c_int;
pub(crate) const A64_GE: c_int = ffi::A64_GE as c_int;
pub(crate) const A64_LT: c_int = ffi::A64_LT as c_int;
pub(crate) const A64_AL: c_int = ffi::A64_AL as c_int;
pub(crate) const A64_NV: c_int = ffi::A64_NV as c_int;
pub(crate) const A64_ZR: c_int = ffi::A64_ZR as c_int;

#[inline(always)]
pub(crate) const fn a64_inv(cc: c_int) -> c_int {
    cc ^ 1
}

pub(crate) const RK_END: c_int = ffi::RK_END as c_int;
pub(crate) const RK_DET: c_int = ffi::RK_DET as c_int;
pub(crate) const RK_STORE: c_int = ffi::RK_STORE as c_int;
pub(crate) const RK_LOAD: c_int = ffi::RK_LOAD as c_int;
pub(crate) const RK_MOVE: c_int = ffi::RK_MOVE as c_int;
pub(crate) const RK_LMOVE: c_int = ffi::RK_LMOVE as c_int;
pub(crate) const RK_UNPCKH: c_int = ffi::RK_UNPCKH as c_int;
pub(crate) const RK_UNPCKL: c_int = ffi::RK_UNPCKL as c_int;

pub(crate) const K2_END: c_int = ffi::K2_END as c_int;
pub(crate) const K2_ARITH: c_int = ffi::K2_ARITH as c_int;
pub(crate) const K2_MOVE: c_int = ffi::K2_MOVE as c_int;
pub(crate) const K2_LMOVE: c_int = ffi::K2_LMOVE as c_int;
pub(crate) const K2_STORE: c_int = ffi::K2_STORE as c_int;
pub(crate) const K2_ZERO: c_int = ffi::K2_ZERO as c_int;
pub(crate) const K2_UNPCKH: c_int = ffi::K2_UNPCKH as c_int;
pub(crate) const K2_UNPCKL: c_int = ffi::K2_UNPCKL as c_int;
pub(crate) const K2_DUP: c_int = ffi::K2_DUP as c_int;
pub(crate) const K2_DET: c_int = ffi::K2_DET as c_int;
pub(crate) const K2_SHUF: c_int = ffi::K2_SHUF as c_int;

pub(crate) const X87R_OK: c_int = ffi::X87R_OK as c_int;
pub(crate) const X87R_FCW: c_int = ffi::X87R_FCW as c_int;
pub(crate) const X87R_MXCSR: c_int = ffi::X87R_MXCSR as c_int;
pub(crate) const X87R_END: c_int = ffi::X87R_END as c_int;
pub(crate) const X87R_RC: c_int = ffi::X87R_RC as c_int;

pub(crate) const XF_PE: c_int = ffi::XF_PE as c_int;
pub(crate) const XF_PC24: c_int = ffi::XF_PC24 as c_int;
pub(crate) const XF_ZERO: c_int = ffi::XF_ZERO as c_int;
pub(crate) const XF_ST32: c_int = ffi::XF_ST32 as c_int;
pub(crate) const XF_SETPE: c_int = ffi::XF_SETPE as c_int;
pub(crate) const XF_TOP0: c_int = ffi::XF_TOP0 as c_int;

pub(crate) const XK_ADD: c_int = ffi::XK_ADD as c_int;
pub(crate) const XK_SUB: c_int = ffi::XK_SUB as c_int;
pub(crate) const XK_MUL: c_int = ffi::XK_MUL as c_int;
pub(crate) const XK_DIV: c_int = ffi::XK_DIV as c_int;
pub(crate) const XK_SQRT: c_int = ffi::XK_SQRT as c_int;

pub(crate) const XS_C1: u64 = ffi::XS_C1 as u64;
pub(crate) const XS_C3: u64 = ffi::XS_C3 as u64;
pub(crate) const XS_PC: u64 = ffi::XS_PC as u64;
pub(crate) const XS_PE: u64 = ffi::XS_PE as u64;
pub(crate) const XS_XOK: u64 = ffi::XS_XOK as u64;

pub(crate) const NANOOL_MAX: c_int = ffi::NANOOL_MAX as c_int;
pub(crate) const FPB_MAX: c_int = ffi::FPB_MAX as c_int;
pub(crate) const FPB_SITES_MAX: c_int = ffi::FPB_SITES_MAX as c_int;
pub(crate) const FPB_UNDO_MAX: c_int = ffi::FPB_UNDO_MAX as c_int;
pub(crate) const L0_NLANES: c_int = ffi::L0_NLANES as c_int;
pub(crate) const JIT_MAX_BLOCK_INSNS: usize = ffi::JIT_MAX_BLOCK_INSNS as usize;

#[inline(always)]
pub(crate) unsafe fn env_on(name: &CStr, slot: *mut c_int) -> c_int {
    unsafe {
        if *slot < 0 {
            *slot = (!libc::getenv(name.as_ptr()).is_null()) as c_int;
        }
        *slot
    }
}

#[inline(always)]
pub(crate) unsafe fn getenv_on(name: &CStr) -> bool {
    unsafe { !libc::getenv(name.as_ptr()).is_null() }
}

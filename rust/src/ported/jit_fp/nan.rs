//! NaN fixups for scalar and packed FP results: the cold paths that quiet a
//! NaN operand or substitute the default NaN, the per-site out-of-line arms the
//! block emits after its body, and the scalar-merge pending slot the cvt
//! absorber uses.

use core::ffi::c_int;

use super::*;
use crate::ffi;
use crate::jit_internal::*;

#[unsafe(no_mangle)]
pub static mut g_nanool: [ffi::NanOolPend; ffi::NANOOL_MAX as usize] =
    unsafe { core::mem::zeroed() };

#[unsafe(no_mangle)]
pub static mut g_n_nanool: c_int = 0;

unsafe fn nanool(i: c_int) -> *mut ffi::NanOolPend {
    (&raw mut g_nanool)
        .cast::<ffi::NanOolPend>()
        .add(i as usize)
}

unsafe fn emit_nan_cold_scalar(b: *mut ffi::A64Buf, dbl: c_int, vr: c_int, va: c_int, vb: c_int) {
    unsafe {
        let quiet: u64 = if dbl != 0 {
            0x0008000000000000
        } else {
            0x00400000
        };
        let dflt: u64 = if dbl != 0 {
            0xfff8000000000000
        } else {
            0xffc00000
        };
        ffi::a64_fcmp(b, dbl, va, va);
        let a_ok = ffi::a64_label(b);
        ffi::a64_bcond(b, A64_VC, 0);
        ffi::a64_fmov_x_from_v(b, dbl, JT0, va);
        let use_ = ffi::a64_label(b);
        ffi::a64_b(b, 0);
        ffi::a64_patch_bcond(a_ok, ffi::a64_label(b));
        ffi::a64_fcmp(b, dbl, vb, vb);
        let b_ok = ffi::a64_label(b);
        ffi::a64_bcond(b, A64_VC, 0);
        ffi::a64_fmov_x_from_v(b, dbl, JT0, vb);
        let use2 = ffi::a64_label(b);
        ffi::a64_b(b, 0);
        ffi::a64_patch_bcond(b_ok, ffi::a64_label(b));
        ffi::a64_mov_imm64(b, JT0, dflt);
        ffi::a64_fmov_v_from_x(b, dbl, vr, JT0);
        let done = ffi::a64_label(b);
        ffi::a64_b(b, 0);
        ffi::a64_patch_b(use_, ffi::a64_label(b));
        ffi::a64_patch_b(use2, ffi::a64_label(b));
        ffi::a64_mov_imm64(b, JT1, quiet);
        ffi::a64_orr_reg(b, dbl, JT0, JT0, JT1, 0);
        ffi::a64_fmov_v_from_x(b, dbl, vr, JT0);
        ffi::a64_patch_b(done, ffi::a64_label(b));
    }
}

#[unsafe(no_mangle)]
pub static mut g_scpend: ffi::JitState_g_scpend = unsafe { core::mem::zeroed() };

#[unsafe(no_mangle)]
pub static mut g_scalar_merge_next: c_int = 0;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_nan_fix_scalar2(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vr: c_int,
    va: c_int,
    vb: c_int,
) {
    unsafe {
        ffi::a64_fcmp(b, dbl, vr, vr);
        g_fcmp_self_vreg = vr;
        g_fcmp_self_idx = g_cur_insn_idx + 1;
        if g_scalar_merge_next != 0 {
            g_scalar_merge_next = 0;
            g_scpend.valid = 1;
            g_scpend.idx = g_cur_insn_idx;
            g_scpend.dbl = dbl;
            g_scpend.vr = vr;
            g_scpend.va = va;
            g_scpend.vb = vb;
            return;
        }
        if g_n_nanool < NANOOL_MAX {
            let o = nanool(g_n_nanool);
            g_n_nanool += 1;
            (*o).site = ffi::a64_label(b);
            ffi::a64_bcond(b, A64_VS, 0);
            (*o).back = ffi::a64_label(b);
            (*o).dbl = dbl as u8;
            (*o).packed = 0;
            (*o).vr = vr as u8;
            (*o).va = va as u8;
            (*o).vb = vb as u8;
            (*o).t1 = 0;
            (*o).cvt = 0;
            (*o).refcmp = 1;
            (*o).pre = 0;
            (*o).idx = g_cur_insn_idx;
            (*o).is_cbz = 0;
            return;
        }
        let ok = ffi::a64_label(b);
        ffi::a64_bcond(b, A64_VC, 0);
        emit_nan_cold_scalar(b, dbl, vr, va, vb);
        ffi::a64_fcmp(b, dbl, vr, vr);
        ffi::a64_patch_bcond(ok, ffi::a64_label(b));
    }
}

#[unsafe(no_mangle)]
pub static mut g_pk_consts_needed: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_fcmp_self_vreg: c_int = -1;

#[unsafe(no_mangle)]
pub static mut g_fcmp_self_idx: c_int = -1;

unsafe fn emit_nan_cold_packed(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vr: c_int,
    va: c_int,
    vb: c_int,
    t1: c_int,
) {
    unsafe {
        ffi::a64_sub_imm(b, 1, 31, 31, 48);
        ffi::a64_str_v(b, 16, va, 31, 0);
        ffi::a64_str_v(b, 16, vb, 31, 16);
        ffi::a64_str_v(b, 16, vr, 31, 32);
        let lanes = if dbl != 0 { 2 } else { 4 };
        let esz = if dbl != 0 { 8 } else { 4 };
        let quiet: u64 = if dbl != 0 {
            0x0008000000000000
        } else {
            0x00400000
        };
        let dflt: u64 = if dbl != 0 {
            0xfff8000000000000
        } else {
            0xffc00000
        };
        for l in 0..lanes {
            let oa = (0 + l * esz) as u32;
            let ob = (16 + l * esz) as u32;
            let orr_ = (32 + l * esz) as u32;
            ffi::a64_ldr_v(b, esz, t1, 31, orr_);
            ffi::a64_fcmp(b, dbl, t1, t1);
            let lane_ok = ffi::a64_label(b);
            ffi::a64_bcond(b, A64_VC, 0);
            ffi::a64_ldr_v(b, esz, t1, 31, oa);
            ffi::a64_fcmp(b, dbl, t1, t1);
            let a_ok = ffi::a64_label(b);
            ffi::a64_bcond(b, A64_VC, 0);
            ffi::a64_ldr(b, esz, JT0, 31, oa);
            ffi::a64_mov_imm64(b, JT1, quiet);
            ffi::a64_orr_reg(b, dbl, JT0, JT0, JT1, 0);
            let w1 = ffi::a64_label(b);
            ffi::a64_b(b, 0);
            ffi::a64_patch_bcond(a_ok, ffi::a64_label(b));
            ffi::a64_ldr_v(b, esz, t1, 31, ob);
            ffi::a64_fcmp(b, dbl, t1, t1);
            let b_ok = ffi::a64_label(b);
            ffi::a64_bcond(b, A64_VC, 0);
            ffi::a64_ldr(b, esz, JT0, 31, ob);
            ffi::a64_mov_imm64(b, JT1, quiet);
            ffi::a64_orr_reg(b, dbl, JT0, JT0, JT1, 0);
            let w2 = ffi::a64_label(b);
            ffi::a64_b(b, 0);
            ffi::a64_patch_bcond(b_ok, ffi::a64_label(b));
            ffi::a64_mov_imm64(b, JT0, dflt);
            ffi::a64_patch_b(w1, ffi::a64_label(b));
            ffi::a64_patch_b(w2, ffi::a64_label(b));
            ffi::a64_str(b, esz, JT0, 31, orr_);
            ffi::a64_patch_bcond(lane_ok, ffi::a64_label(b));
        }
        ffi::a64_ldr_v(b, 16, vr, 31, 32);
        ffi::a64_add_imm(b, 1, 31, 31, 48);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_nan_fix_packed2(
    b: *mut ffi::A64Buf,
    dbl: c_int,
    vr: c_int,
    va: c_int,
    vb: c_int,
    t1: c_int,
    t2: c_int,
) {
    unsafe {
        let _ = t2;
        ffi::a64_v_fcmeq(b, dbl, t1, vr, vr);
        ffi::a64_v_xtn(b, if dbl != 0 { 2 } else { 1 }, t1, t1);
        ffi::a64_fmov_x_from_v(b, 1, JT0, t1);
        ffi::a64_cmn_imm(b, 1, JT0, 1);
        if g_n_nanool < NANOOL_MAX {
            let o = nanool(g_n_nanool);
            g_n_nanool += 1;
            (*o).site = ffi::a64_label(b);
            ffi::a64_bcond(b, A64_NE, 0);
            (*o).back = ffi::a64_label(b);
            (*o).dbl = dbl as u8;
            (*o).packed = 1;
            (*o).vr = vr as u8;
            (*o).va = va as u8;
            (*o).vb = vb as u8;
            (*o).t1 = t1 as u8;
            (*o).cvt = 0;
            (*o).refcmp = 0;
            (*o).pre = 0;
            (*o).idx = g_cur_insn_idx;
            (*o).is_cbz = 0;
            return;
        }
        let ok = ffi::a64_label(b);
        ffi::a64_bcond(b, A64_EQ, 0);
        emit_nan_cold_packed(b, dbl, vr, va, vb, t1);
        ffi::a64_patch_bcond(ok, ffi::a64_label(b));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_nan_ool_arms(
    b: *mut ffi::A64Buf,
    blk: *mut ffi::JitBlock,
    entry: *const u32,
) {
    unsafe {
        let _ = blk;
        let _ = entry;
        for i in 0..g_n_nanool {
            let o = &*nanool(i);
            let lo = ffi::a64_label(b);
            if o.is_cbz != 0 {
                ffi::a64_patch_cbz(o.site, lo);
            } else {
                ffi::a64_patch_bcond(o.site, lo);
            }
            if o.pre != 0 {
                ffi::a64_fcmp(b, o.dbl as c_int, o.pvr as c_int, o.pvr as c_int);
                let sk = ffi::a64_label(b);
                ffi::a64_bcond(b, A64_VC, 0);
                emit_nan_cold_scalar(
                    b,
                    o.dbl as c_int,
                    o.pvr as c_int,
                    o.pva as c_int,
                    o.pvb as c_int,
                );
                ffi::a64_patch_bcond(sk, ffi::a64_label(b));
            }
            if o.cvt == 1 {
                ffi::a64_movz(b, o.vr as c_int, 0x8000, 3);
            } else if o.cvt == 2 {
                ffi::a64_fcvtzs(b, 1, o.dbl as c_int, JT0, o.va as c_int);
                ffi::a64_cmp_ext_sxtw(b, JT0, JT0);
                ffi::a64_movz(b, JTU, 0x8000, 1);
                ffi::a64_csel(b, 0, o.vr as c_int, JTU, JT0, A64_NE);
                ffi::a64_fcmp(b, o.dbl as c_int, o.va as c_int, o.va as c_int);
                ffi::a64_csel(b, 0, o.vr as c_int, JTU, o.vr as c_int, A64_VS);
            } else if o.packed != 0 {
                emit_nan_cold_packed(
                    b,
                    o.dbl as c_int,
                    o.vr as c_int,
                    o.va as c_int,
                    o.vb as c_int,
                    o.t1 as c_int,
                );
            } else {
                emit_nan_cold_scalar(
                    b,
                    o.dbl as c_int,
                    o.vr as c_int,
                    o.va as c_int,
                    o.vb as c_int,
                );
                if o.refcmp != 0 {
                    ffi::a64_fcmp(b, o.dbl as c_int, o.vr as c_int, o.vr as c_int);
                }
            }
            let here = ffi::a64_label(b);
            ffi::a64_b(b, o.back.offset_from(here) as i32);
        }
        g_n_nanool = 0;
    }
}

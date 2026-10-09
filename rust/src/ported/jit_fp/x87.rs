//! The x87 stack machine emitter: run detection, the speculative top pointer
//! (`g_x87_spec`/`g_x87_delta`), tag and extended-opcode knowledge tracking,
//! lane-cached stack registers, and the out-of-line fragment arms (exactness,
//! precision control, zero operands, top mismatch, 32-bit stores) plus the
//! per-run slow paths appended after the block body.

use core::ffi::{c_int, c_uint};

use super::l0::{g_l0, g_l0_dbl, g_l0_dirty, g_yc_dirty};
use super::nan::g_scpend;
use super::*;
use crate::ffi;
use crate::jit_internal::*;

const X87_RUN_MAX: usize = ffi::X87_RUN_MAX as usize;
const X87_SITE_MAX: usize = ffi::X87_SITE_MAX as usize;
const X87_FRAG_MAX: usize = ffi::X87_FRAG_MAX as usize;

static mut g_x87_run: [ffi::X87Run; X87_RUN_MAX] = unsafe { core::mem::zeroed() };

#[unsafe(no_mangle)]
pub static mut g_n_x87_run: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_x87_cur: c_int = -1;

#[derive(Clone, Copy, Default)]
struct X87Site {
    site: *mut u32,
    run: i16,
    idx: i16,
}

static mut g_x87_site: [X87Site; X87_SITE_MAX] = [X87Site {
    site: core::ptr::null_mut(),
    run: 0,
    idx: 0,
}; X87_SITE_MAX];

#[unsafe(no_mangle)]
pub static mut g_n_x87_site: c_int = 0;

#[derive(Clone, Copy, Default)]
struct X87Frag {
    site: *mut u32,
    back: *mut u32,
    kind: u8,
    op: u8,
    r0: u8,
    r1: u8,
    run: i16,
    idx: i16,
}

static mut g_x87_frag: [X87Frag; X87_FRAG_MAX] = [X87Frag {
    site: core::ptr::null_mut(),
    back: core::ptr::null_mut(),
    kind: 0,
    op: 0,
    r0: 0,
    r1: 0,
    run: 0,
    idx: 0,
}; X87_FRAG_MAX];

static mut g_x87_fr0: c_int = 0;

static mut g_x87_fr1: c_int = 1;

#[unsafe(no_mangle)]
pub static mut g_n_x87_frag: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_x87_frag_open: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_x87_nzcv: c_int = -1;

static mut g_x87_nzcv_live: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_x87_delta: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_x87_live: c_int = 0;

static mut g_x87_rc_near: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_x87_btop: c_int = -1;

#[unsafe(no_mangle)]
pub static mut g_x87_spec: c_int = -1;

#[unsafe(no_mangle)]
pub static mut g_x87_lane: [i8; 8] = [0; 8];

#[unsafe(no_mangle)]
pub static mut g_x87_lanes_on: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_x87_lv: u8 = 0;

#[inline(always)]
unsafe fn site(i: usize) -> *mut X87Site {
    (&raw mut g_x87_site).cast::<X87Site>().add(i)
}
#[inline(always)]
unsafe fn frag(i: usize) -> *mut X87Frag {
    (&raw mut g_x87_frag).cast::<X87Frag>().add(i)
}
#[inline(always)]
unsafe fn run(i: usize) -> *mut ffi::X87Run {
    (&raw mut g_x87_run).cast::<ffi::X87Run>().add(i)
}
#[inline(always)]
unsafe fn tagk(i: usize) -> *mut i8 {
    (&raw mut g_x87_tagk).cast::<i8>().add(i)
}
#[inline(always)]
unsafe fn xokk(i: usize) -> *mut i8 {
    (&raw mut g_x87_xokk).cast::<i8>().add(i)
}
#[inline(always)]
unsafe fn lane(i: usize) -> *mut i8 {
    (&raw mut g_x87_lane).cast::<i8>().add(i)
}

unsafe fn x87_mem_ok(
    in_: *const ffi::X86Insn,
    o: *const ffi::X86Operand,
    s1: c_int,
    s2: c_int,
    s3: c_int,
) -> c_int {
    unsafe {
        if (*o).kind as u32 != ffi::OCERZ_OPK_MEM || (*in_).seg as u32 != ffi::OCERZ_SEG_NONE {
            return 0;
        }
        if (*in_).addrsize as c_int != if (*in_).mode32 != 0 { 4 } else { 8 } {
            return 0;
        }
        ((*o).size as c_int == s1 || (*o).size as c_int == s2 || (*o).size as c_int == s3) as c_int
    }
}

static mut S_NO_JIT_X87: c_int = -1;
static mut S_NO_FIST_RC: c_int = -1;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn x87_run_flags(in_: *const ffi::X86Insn) -> c_int {
    unsafe {
        if env_on(c"OCERZ_NO_JIT_X87", &raw mut S_NO_JIT_X87) != 0 || (*in_).vex != 0 {
            return 0;
        }
        let in_ = &*in_;
        let o = &in_.ops[0];
        let st1 = (in_.nops == 1 && o.kind as u32 == ffi::OCERZ_OPK_ST) as c_int;
        let st2 = (in_.nops == 2
            && o.kind as u32 == ffi::OCERZ_OPK_ST
            && in_.ops[1].kind as u32 == ffi::OCERZ_OPK_ST) as c_int;
        let m1 = in_.nops == 1;
        let arith = X87R_OK | X87R_FCW | X87R_MXCSR;
        match in_.op as u32 {
            ffi::OCERZ_OP_FLD => {
                if st1 != 0 || (m1 && x87_mem_ok(in_, o, 4, 8, 0) != 0) {
                    X87R_OK
                } else {
                    0
                }
            }
            ffi::OCERZ_OP_FST | ffi::OCERZ_OP_FSTP => {
                if st1 != 0 || (m1 && x87_mem_ok(in_, o, 8, 0, 0) != 0) {
                    X87R_OK
                } else if m1 && x87_mem_ok(in_, o, 4, 0, 0) != 0 {
                    arith
                } else {
                    0
                }
            }
            ffi::OCERZ_OP_FILD => {
                if m1 && x87_mem_ok(in_, o, 2, 4, 0) != 0 {
                    X87R_OK
                } else if m1 && x87_mem_ok(in_, o, 8, 0, 0) != 0 {
                    X87R_OK | X87R_MXCSR
                } else {
                    0
                }
            }
            ffi::OCERZ_OP_FIST | ffi::OCERZ_OP_FISTP => {
                if m1 && x87_mem_ok(in_, o, 2, 4, 8) != 0 {
                    X87R_OK
                        | if env_on(c"OCERZ_NO_FIST_RC", &raw mut S_NO_FIST_RC) != 0 {
                            X87R_FCW
                        } else {
                            X87R_RC
                        }
                } else {
                    0
                }
            }
            ffi::OCERZ_OP_FISTTP => {
                if m1 && x87_mem_ok(in_, o, 2, 4, 8) != 0 {
                    X87R_OK
                } else {
                    0
                }
            }
            ffi::OCERZ_OP_FLDZ
            | ffi::OCERZ_OP_FLD1
            | ffi::OCERZ_OP_FLDPI
            | ffi::OCERZ_OP_FLDL2E
            | ffi::OCERZ_OP_FLDL2T
            | ffi::OCERZ_OP_FLDLG2
            | ffi::OCERZ_OP_FLDLN2
            | ffi::OCERZ_OP_FCHS
            | ffi::OCERZ_OP_FABS
            | ffi::OCERZ_OP_FNCLEX
            | ffi::OCERZ_OP_FWAIT
            | ffi::OCERZ_OP_FINCSTP
            | ffi::OCERZ_OP_FDECSTP
            | ffi::OCERZ_OP_FCOMPP
            | ffi::OCERZ_OP_FUCOMPP
            | ffi::OCERZ_OP_FTST => {
                if in_.nops == 0 {
                    X87R_OK
                } else {
                    0
                }
            }
            ffi::OCERZ_OP_FADD
            | ffi::OCERZ_OP_FSUB
            | ffi::OCERZ_OP_FSUBR
            | ffi::OCERZ_OP_FMUL
            | ffi::OCERZ_OP_FDIV
            | ffi::OCERZ_OP_FDIVR => {
                if st2 != 0 || (m1 && x87_mem_ok(in_, o, 4, 8, 0) != 0) {
                    arith
                } else {
                    0
                }
            }
            ffi::OCERZ_OP_FADDP
            | ffi::OCERZ_OP_FSUBP
            | ffi::OCERZ_OP_FSUBRP
            | ffi::OCERZ_OP_FMULP
            | ffi::OCERZ_OP_FDIVP
            | ffi::OCERZ_OP_FDIVRP => {
                if st2 != 0 {
                    arith
                } else {
                    0
                }
            }
            ffi::OCERZ_OP_FIADD
            | ffi::OCERZ_OP_FISUB
            | ffi::OCERZ_OP_FISUBR
            | ffi::OCERZ_OP_FIMUL
            | ffi::OCERZ_OP_FIDIV
            | ffi::OCERZ_OP_FIDIVR => {
                if m1 && x87_mem_ok(in_, o, 2, 4, 0) != 0 {
                    arith
                } else {
                    0
                }
            }
            ffi::OCERZ_OP_FSQRT => {
                if in_.nops == 0 {
                    arith
                } else {
                    0
                }
            }
            ffi::OCERZ_OP_FRNDINT => {
                if in_.nops == 0 {
                    X87R_OK | X87R_FCW
                } else {
                    0
                }
            }
            ffi::OCERZ_OP_FXCH
            | ffi::OCERZ_OP_FFREE
            | ffi::OCERZ_OP_FFREEP
            | ffi::OCERZ_OP_FUCOM
            | ffi::OCERZ_OP_FUCOMP => {
                if st1 != 0 {
                    X87R_OK
                } else {
                    0
                }
            }
            ffi::OCERZ_OP_FCOM | ffi::OCERZ_OP_FCOMP => {
                if st1 != 0 || (m1 && x87_mem_ok(in_, o, 4, 8, 0) != 0) {
                    X87R_OK
                } else {
                    0
                }
            }
            ffi::OCERZ_OP_FICOM | ffi::OCERZ_OP_FICOMP => {
                if m1 && x87_mem_ok(in_, o, 2, 4, 0) != 0 {
                    X87R_OK
                } else {
                    0
                }
            }
            ffi::OCERZ_OP_FCOMI
            | ffi::OCERZ_OP_FCOMIP
            | ffi::OCERZ_OP_FUCOMI
            | ffi::OCERZ_OP_FUCOMIP
            | ffi::OCERZ_OP_FCMOVCC => {
                if st2 != 0 && o.reg == 0 {
                    X87R_OK
                } else {
                    0
                }
            }
            ffi::OCERZ_OP_FNSTSW => {
                if m1 && o.kind as u32 == ffi::OCERZ_OPK_REG {
                    if o.reg as u32 == ffi::OCERZ_RAX && o.size == 2 && o.high8 == 0 {
                        X87R_OK
                    } else {
                        0
                    }
                } else if m1 && x87_mem_ok(in_, o, 2, 0, 0) != 0 {
                    X87R_OK
                } else {
                    0
                }
            }
            ffi::OCERZ_OP_FNSTCW => {
                if m1 && x87_mem_ok(in_, o, 2, 0, 0) != 0 {
                    X87R_OK
                } else {
                    0
                }
            }
            ffi::OCERZ_OP_FLDCW => {
                if m1 && x87_mem_ok(in_, o, 2, 0, 0) != 0 {
                    X87R_OK | X87R_END
                } else {
                    0
                }
            }
            ffi::OCERZ_OP_FNINIT => {
                if in_.nops == 0 {
                    X87R_OK | X87R_END
                } else {
                    0
                }
            }
            _ => 0,
        }
    }
}

unsafe fn x87_slow_at(b: *mut ffi::A64Buf, cond: c_int, run_: c_int, idx: c_int) {
    unsafe {
        let s = site(g_n_x87_site as usize);
        (*s).site = ffi::a64_label(b);
        (*s).run = run_ as i16;
        (*s).idx = idx as i16;
        g_n_x87_site += 1;
        ffi::a64_bcond(b, cond, 0);
    }
}

unsafe fn x87_slow_if(b: *mut ffi::A64Buf, cond: c_int) {
    unsafe { x87_slow_at(b, cond, g_x87_cur, g_cur_insn_idx) }
}

unsafe fn x87_frag_if(b: *mut ffi::A64Buf, cond: c_int, kind: c_int, op: c_int) {
    unsafe {
        let f = frag(g_n_x87_frag as usize);
        g_n_x87_frag += 1;
        (*f).site = ffi::a64_label(b);
        (*f).back = core::ptr::null_mut();
        (*f).kind = kind as u8;
        (*f).op = op as u8;
        (*f).r0 = g_x87_fr0 as u8;
        (*f).r1 = g_x87_fr1 as u8;
        (*f).run = g_x87_cur as i16;
        (*f).idx = g_cur_insn_idx as i16;
        ffi::a64_bcond(b, cond, 0);
    }
}

unsafe fn x87_frag_land(b: *mut ffi::A64Buf) {
    unsafe {
        for f in g_x87_frag_open..g_n_x87_frag {
            (*frag(f as usize)).back = ffi::a64_label(b);
        }
        g_x87_frag_open = g_n_x87_frag;
    }
}

static mut S_X87_CHECK: c_int = -1;

unsafe fn x87_ld(b: *mut ffi::A64Buf) {
    unsafe {
        if g_x87_live == 0 {
            ffi::a64_ldr(b, 8, X87S, 20, X87_CTL_OFF);
            return;
        }
        if env_on(c"OCERZ_X87_CHECK", &raw mut S_X87_CHECK) != 0 {
            ffi::a64_ldr(b, 8, X87P, 20, X87_CTL_OFF);
            ffi::a64_subs_reg(b, 1, A64_ZR, X87P, X87S, 0);
            ffi::a64_bcond(b, A64_EQ, 2);
            ffi::a64_emit32(b, 0xd4200000u32 | (0x87 << 5));
            x87_top(b, X87P);
            ffi::a64_lsl_imm(b, 1, X87P, X87P, 40);
            ffi::a64_eor_reg(b, 1, X87P, X87P, X87S, 0);
            ffi::a64_try_ands_imm(b, 1, A64_ZR, X87P, 7u64 << 40);
            ffi::a64_bcond(b, A64_EQ, 2);
            ffi::a64_emit32(b, 0xd4200000u32 | (0x88 << 5));
        }
    }
}

static mut g_x87_st_mark: *const u32 = core::ptr::null();

static mut S_NO_X87_ST_ELIDE: c_int = -1;

unsafe fn x87_st(b: *mut ffi::A64Buf) {
    unsafe {
        if env_on(c"OCERZ_NO_X87_ST_ELIDE", &raw mut S_NO_X87_ST_ELIDE) == 0
            && g_x87_live != 0
            && !g_x87_st_mark.is_null()
            && g_x87_st_mark <= (*b).p
        {
            let mut dirty = 0;
            let mut w = g_x87_st_mark;
            while w < (*b).p && dirty == 0 {
                dirty = a64_word_may_write_reg(*w, X87S as c_uint);
                w = w.add(1);
            }
            if dirty == 0 {
                return;
            }
        }
        ffi::a64_str(b, 8, X87S, 20, X87_CTL_OFF);
        g_x87_st_mark = (*b).p;
    }
}

unsafe fn x87_reload(b: *mut ffi::A64Buf) {
    unsafe {
        if g_x87_live != 0 {
            ffi::a64_ldr(b, 8, X87S, 20, X87_CTL_OFF);
            g_x87_st_mark = (*b).p;
        }
    }
}

unsafe fn x87_top_at(b: *mut ffi::A64Buf, rd: c_int, k: c_int) {
    unsafe {
        let mut k = k;
        if g_x87_live != 0 && g_x87_spec >= 0 {
            ffi::a64_movz(b, rd, ((g_x87_spec + g_x87_delta + k) & 7) as u16, 0);
            return;
        }
        if g_x87_live != 0 {
            ffi::a64_ldr(b, 2, rd, 20, X87_TOP0_OFF);
            k += g_x87_delta;
        } else {
            ffi::a64_ubfx(b, 1, rd, X87S, 40, 3);
        }
        if (k & 7) != 0 {
            ffi::a64_add_imm(b, 0, rd, rd, (k & 7) as u32);
            ffi::a64_try_and_imm(b, 0, rd, rd, 7);
        }
    }
}

unsafe fn x87_top(b: *mut ffi::A64Buf, rd: c_int) {
    unsafe { x87_top_at(b, rd, 0) }
}

unsafe fn x87_phys(b: *mut ffi::A64Buf, rd: c_int, i: c_int) {
    unsafe { x87_top_at(b, rd, i) }
}

unsafe fn x87_newtop(b: *mut ffi::A64Buf, rd: c_int) {
    unsafe { x87_top_at(b, rd, -1) }
}

unsafe fn x87_slot(b: *mut ffi::A64Buf, rd: c_int, rp: c_int) {
    unsafe { ffi::a64_add_reg(b, 1, rd, 20, rp, 3) }
}

unsafe fn x87_lane_of(rel: c_int) -> c_int {
    unsafe {
        if g_x87_lanes_on == 0 || g_x87_spec < 0 || g_x87_live == 0 {
            return -1;
        }
        *lane((rel & 7) as usize) as c_int
    }
}

unsafe fn x87_lane_get(b: *mut ffi::A64Buf, rel: c_int) -> c_int {
    unsafe {
        let l = x87_lane_of(rel);
        if l < 0 {
            return -1;
        }
        let p = rel & 7;
        if (g_x87_lv >> p & 1) == 0 {
            ffi::a64_ldr_v(b, 8, l, 20, X87_FPR_OFF + 8 * p as u32);
            g_x87_lv |= 1u8 << p;
        }
        l
    }
}

unsafe fn x87_lane_put(b: *mut ffi::A64Buf, rel: c_int, v: c_int) {
    unsafe {
        let l = x87_lane_of(rel);
        if l < 0 {
            return;
        }
        if l != v {
            ffi::a64_fmov_d_d(b, l, v);
        }
        g_x87_lv |= 1u8 << (rel & 7);
    }
}

unsafe fn x87_lane_read(b: *mut ffi::A64Buf, vd: c_int, rel: c_int, addr: c_int) {
    unsafe {
        let l = x87_lane_get(b, rel);
        if l >= 0 {
            ffi::a64_fmov_d_d(b, vd, l);
        } else {
            ffi::a64_ldr_v(b, 8, vd, addr, X87_FPR_OFF);
        }
    }
}

unsafe fn x87_bit(b: *mut ffi::A64Buf, rd: c_int, rp: c_int) {
    unsafe {
        ffi::a64_movz(b, rd, 1, 0);
        ffi::a64_lslv(b, 0, rd, rd, rp);
    }
}

static mut g_x87_c1k: i8 = 0;

static mut g_x87_tagk: [i8; 8] = [0; 8];

static mut g_x87_xokk: [i8; 8] = [0; 8];

unsafe fn x87_rel(i: c_int) -> c_int {
    unsafe {
        ((if g_x87_live != 0 && g_x87_spec >= 0 {
            g_x87_spec
        } else {
            0
        }) + g_x87_delta
            + i)
            & 7
    }
}

#[unsafe(no_mangle)]
pub static mut g_x87_kcarry: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_x87_spec_cut: c_int = 0;

static mut g_x87_fcmov_static: c_int = 0;

unsafe fn x87_know_reset() {
    unsafe {
        core::ptr::write_bytes(&raw mut g_x87_tagk, 0, 1);
        core::ptr::write_bytes(&raw mut g_x87_xokk, 0, 1);
        g_x87_c1k = 0;
    }
}

unsafe fn x87_clear_c1(b: *mut ffi::A64Buf) {
    unsafe {
        if g_x87_live != 0 && g_x87_c1k != 0 {
            return;
        }
        ffi::a64_try_and_imm(b, 1, X87S, X87S, !XS_C1);
        g_x87_c1k = g_x87_live as i8;
    }
}

unsafe fn x87_tag(b: *mut ffi::A64Buf, rp: c_int, rt: c_int, rel: c_int, image: c_int) {
    unsafe {
        let want = if image != 0 { 1 } else { 2 };
        let tag = (g_x87_live == 0 || *tagk(rel as usize) != 1) as c_int;
        let xok = (g_x87_live == 0 || *xokk(rel as usize) != want) as c_int;
        if g_x87_live != 0 && g_x87_spec >= 0 {
            if tag != 0 {
                ffi::a64_try_orr_imm(b, 1, X87S, X87S, 1u64 << (32 + rel));
            }
            if xok != 0 && image != 0 {
                ffi::a64_try_orr_imm(b, 1, X87S, X87S, 1u64 << (XS_XOK + rel as u64));
            }
            if xok != 0 && image == 0 {
                ffi::a64_try_and_imm(b, 1, X87S, X87S, !(1u64 << (XS_XOK + rel as u64)));
            }
            *tagk(rel as usize) = 1;
            *xokk(rel as usize) = want as i8;
            return;
        }
        if tag != 0 || xok != 0 {
            x87_bit(b, rt, rp);
        }
        if tag != 0 {
            ffi::a64_orr_reg(b, 1, X87S, X87S, rt, 32);
        }
        if xok != 0 && image != 0 {
            ffi::a64_orr_reg(b, 1, X87S, X87S, rt, XS_XOK as c_int);
        }
        if xok != 0 && image == 0 {
            ffi::a64_bic_reg(b, 1, X87S, X87S, rt, XS_XOK as c_int);
        }
        if g_x87_live != 0 {
            *tagk(rel as usize) = 1;
            *xokk(rel as usize) = want as i8;
        }
    }
}

unsafe fn x87_pop(b: *mut ffi::A64Buf) {
    unsafe {
        let rel = x87_rel(0);
        if g_x87_live != 0 && g_x87_spec >= 0 {
            if *tagk(rel as usize) != 2 {
                ffi::a64_try_and_imm(b, 1, X87S, X87S, !(1u64 << (32 + rel)));
            }
        } else if g_x87_live == 0 || *tagk(rel as usize) != 2 {
            x87_top(b, JT0);
            x87_bit(b, JTT, JT0);
            ffi::a64_bic_reg(b, 1, X87S, X87S, JTT, 32);
        }
        if g_x87_live != 0 {
            *tagk(rel as usize) = 2;
        }
        x87_phys(b, JT0, 1);
        ffi::a64_bfi(b, 1, X87S, JT0, 40, 8);
        g_x87_delta += 1;
    }
}

unsafe fn x87_copy(b: *mut ffi::A64Buf, rd: c_int, rs: c_int, rdrel: c_int, rsrel: c_int) {
    unsafe {
        let sx = if g_x87_live != 0 {
            *xokk(rsrel as usize) as c_int
        } else {
            0
        };
        x87_slot(b, X87Q, rs);
        x87_slot(b, JTT, rd);
        let ls = x87_lane_get(b, rsrel);
        let v = if ls >= 0 { ls } else { VX0 };
        if ls < 0 {
            ffi::a64_ldr_v(b, 8, VX0, X87Q, X87_FPR_OFF);
        }
        ffi::a64_str_v(b, 8, v, JTT, X87_FPR_OFF);
        x87_lane_put(b, rdrel, v);
        if sx == 2 {
            x87_tag(b, rd, JTU, rdrel, 0);
            return;
        }
        if sx == 1 {
            ffi::a64_ldr_v(b, 8, VX1, X87Q, X87_XM_OFF);
            ffi::a64_str_v(b, 8, VX1, JTT, X87_XM_OFF);
            ffi::a64_add_reg(b, 1, X87Q, 20, rs, 1);
            ffi::a64_add_reg(b, 1, JTT, 20, rd, 1);
            ffi::a64_ldr(b, 2, JTU, X87Q, X87_XE_OFF);
            ffi::a64_str(b, 2, JTU, JTT, X87_XE_OFF);
            x87_tag(b, rd, JTU, rdrel, 1);
            return;
        }
        ffi::a64_lsrv(b, 1, JTU, X87S, rs);
        ffi::a64_ubfx(b, 1, JTU, JTU, XS_XOK as c_int, 1);
        let noimg = ffi::a64_label(b);
        ffi::a64_cbz(b, 0, JTU, 0);
        ffi::a64_ldr_v(b, 8, VX1, X87Q, X87_XM_OFF);
        ffi::a64_str_v(b, 8, VX1, JTT, X87_XM_OFF);
        ffi::a64_add_reg(b, 1, X87Q, 20, rs, 1);
        ffi::a64_add_reg(b, 1, JTT, 20, rd, 1);
        ffi::a64_ldr(b, 2, JTU, X87Q, X87_XE_OFF);
        ffi::a64_str(b, 2, JTU, JTT, X87_XE_OFF);
        ffi::a64_movz(b, JTU, 1, 0);
        ffi::a64_patch_cbz(noimg, ffi::a64_label(b));
        ffi::a64_lslv(b, 0, JTU, JTU, rd);
        x87_bit(b, JTT, rd);
        ffi::a64_bic_reg(b, 1, X87S, X87S, JTT, XS_XOK as c_int);
        ffi::a64_orr_reg(b, 1, X87S, X87S, JTU, XS_XOK as c_int);
        if g_x87_live == 0 || *tagk(rdrel as usize) != 1 {
            ffi::a64_orr_reg(b, 1, X87S, X87S, JTT, 32);
        }
        if g_x87_live != 0 {
            *tagk(rdrel as usize) = 1;
            *xokk(rdrel as usize) = 0;
        }
    }
}

unsafe fn x87_push(b: *mut ffi::A64Buf, vv: c_int, image: c_int, rm: c_int, rs: c_int) {
    unsafe {
        x87_newtop(b, X87P);
        if g_x87_live != 0 && g_x87_spec >= 0 && image == 0 {
            ffi::a64_str_v(b, 8, vv, 20, X87_FPR_OFF + 8 * x87_rel(-1) as u32);
        } else {
            x87_slot(b, X87Q, X87P);
            ffi::a64_str_v(b, 8, vv, X87Q, X87_FPR_OFF);
        }
        x87_lane_put(b, x87_rel(-1), vv);
        if image != 0 {
            ffi::a64_str(b, 8, rm, X87Q, X87_XM_OFF);
            ffi::a64_add_reg(b, 1, X87Q, 20, X87P, 1);
            ffi::a64_str(b, 2, rs, X87Q, X87_XE_OFF);
        }
        x87_tag(b, X87P, JT0, x87_rel(-1), image);
        ffi::a64_bfi(b, 1, X87S, X87P, 40, 8);
        g_x87_delta -= 1;
        x87_st(b);
    }
}

unsafe fn x87_int_image(b: *mut ffi::A64Buf, rv: c_int, rm: c_int, rs: c_int, rt: c_int) {
    unsafe {
        ffi::a64_subs_imm(b, 1, A64_ZR, rv, 0);
        ffi::a64_csneg(b, 1, rm, rv, rv, A64_GE);
        ffi::a64_clz(b, 1, rs, rm);
        ffi::a64_lslv(b, 1, rm, rm, rs);
        ffi::a64_movz(b, rt, 16383 + 63, 0);
        ffi::a64_sub_reg(b, 0, rs, rt, rs, 0);
        ffi::a64_try_orr_imm(b, 0, rt, rs, 0x8000);
        ffi::a64_csel(b, 0, rs, rt, rs, A64_LT);
        ffi::a64_csel(b, 0, rs, rs, A64_ZR, A64_NE);
    }
}

unsafe fn x87_ld_int(
    b: *mut ffi::A64Buf,
    insn: *const ffi::X86Insn,
    rd: c_int,
    sext: c_int,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    unsafe {
        let o = &(*insn).ops[0];
        let mut skip: *mut u32 = core::ptr::null_mut();
        if ffi::emit_sse_mem_addr(b, insn, o, o.size as c_int, exit_sites, n_exits, &mut skip) == 0
        {
            return 0;
        }
        emit_sse_mem_ld_gpr(b, o.size as c_int, rd);
        patch_guard_skip(skip, ffi::a64_label(b));
        if sext != 0 && o.size == 2 {
            ffi::a64_sxth(b, 1, rd, rd);
        } else if sext != 0 && o.size == 4 {
            ffi::a64_sxtw(b, rd, rd);
        }
        1
    }
}

unsafe fn x87_ld_real(
    b: *mut ffi::A64Buf,
    insn: *const ffi::X86Insn,
    vd: c_int,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    unsafe {
        let o = &(*insn).ops[0];
        let mut skip: *mut u32 = core::ptr::null_mut();
        if ffi::emit_sse_mem_addr(b, insn, o, o.size as c_int, exit_sites, n_exits, &mut skip) == 0
        {
            return 0;
        }
        emit_sse_mem_ld(b, o.size as c_int, vd);
        patch_guard_skip(skip, ffi::a64_label(b));
        if o.size == 4 {
            ffi::a64_fcvt_s2d(b, vd, vd);
        }
        1
    }
}

unsafe fn x87_st_mem(
    b: *mut ffi::A64Buf,
    insn: *const ffi::X86Insn,
    vec_: c_int,
    reg: c_int,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    unsafe {
        let o = &(*insn).ops[0];
        let mut skip: *mut u32 = core::ptr::null_mut();
        if ffi::emit_sse_mem_addr(b, insn, o, o.size as c_int, exit_sites, n_exits, &mut skip) == 0
        {
            return 0;
        }
        if vec_ != 0 {
            emit_sse_mem_st(b, o.size as c_int, reg);
        } else {
            emit_sse_mem_st_gpr(b, o.size as c_int, reg);
        }
        patch_guard_skip(skip, ffi::a64_label(b));
        1
    }
}

unsafe fn x87_result(b: *mut ffi::A64Buf, kind: c_int) {
    unsafe {
        ffi::a64_fmov_x_from_v(b, 1, JT0, VX2);
        ffi::a64_ubfx(b, 1, JTT, JT0, 52, 11);
        if kind == XK_MUL || kind == XK_DIV {
            ffi::a64_sub_imm(b, 0, JTT, JTT, 54);
            ffi::a64_subs_imm(b, 0, A64_ZR, JTT, 0x7fe - 54);
            x87_frag_if(b, A64_HI, XF_ZERO, kind);
        } else {
            ffi::a64_subs_imm(b, 0, A64_ZR, JTT, 0x7ff);
            x87_slow_if(b, A64_EQ);
        }
        ffi::a64_try_ands_imm(b, 1, A64_ZR, X87S, XS_PC);
        x87_frag_if(b, A64_EQ, XF_PC24, kind);
        ffi::a64_try_ands_imm(b, 1, A64_ZR, X87S, XS_PE);
        x87_frag_if(b, A64_EQ, XF_PE, kind);
        x87_frag_land(b);
    }
}

unsafe fn x87_arith(
    b: *mut ffi::A64Buf,
    insn: *const ffi::X86Insn,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    unsafe {
        let insn_ = &*insn;
        let mut kind = 0;
        let mut rev = 0;
        let mut popit = 0;
        let mut intform = 0;
        match insn_.op as u32 {
            ffi::OCERZ_OP_FADD => kind = XK_ADD,
            ffi::OCERZ_OP_FADDP => {
                kind = XK_ADD;
                popit = 1;
            }
            ffi::OCERZ_OP_FIADD => {
                kind = XK_ADD;
                intform = 1;
            }
            ffi::OCERZ_OP_FSUB => kind = XK_SUB,
            ffi::OCERZ_OP_FSUBP => {
                kind = XK_SUB;
                popit = 1;
            }
            ffi::OCERZ_OP_FISUB => {
                kind = XK_SUB;
                intform = 1;
            }
            ffi::OCERZ_OP_FSUBR => {
                kind = XK_SUB;
                rev = 1;
            }
            ffi::OCERZ_OP_FSUBRP => {
                kind = XK_SUB;
                rev = 1;
                popit = 1;
            }
            ffi::OCERZ_OP_FISUBR => {
                kind = XK_SUB;
                rev = 1;
                intform = 1;
            }
            ffi::OCERZ_OP_FMUL => kind = XK_MUL,
            ffi::OCERZ_OP_FMULP => {
                kind = XK_MUL;
                popit = 1;
            }
            ffi::OCERZ_OP_FIMUL => {
                kind = XK_MUL;
                intform = 1;
            }
            ffi::OCERZ_OP_FDIV => kind = XK_DIV,
            ffi::OCERZ_OP_FDIVP => {
                kind = XK_DIV;
                popit = 1;
            }
            ffi::OCERZ_OP_FIDIV => {
                kind = XK_DIV;
                intform = 1;
            }
            ffi::OCERZ_OP_FDIVR => {
                kind = XK_DIV;
                rev = 1;
            }
            ffi::OCERZ_OP_FDIVRP => {
                kind = XK_DIV;
                rev = 1;
                popit = 1;
            }
            ffi::OCERZ_OP_FIDIVR => {
                kind = XK_DIV;
                rev = 1;
                intform = 1;
            }
            _ => return 0,
        }
        let o = &insn_.ops[0];
        let mem = (o.kind as u32 != ffi::OCERZ_OPK_ST) as c_int;
        let mut va = if rev != 0 { VX1 } else { VX0 };
        let mut vb = if rev != 0 { VX0 } else { VX1 };
        if mem != 0 && intform != 0 {
            if x87_ld_int(b, insn, X87Q, 1, exit_sites, n_exits) == 0 {
                return 0;
            }
            ffi::a64_scvtf(b, 1, 1, vb, X87Q);
        } else if mem != 0 && x87_ld_real(b, insn, vb, exit_sites, n_exits) == 0 {
            return 0;
        }
        x87_ld(b);
        let drel = x87_rel(if mem != 0 { 0 } else { o.reg as c_int });
        let known = g_x87_live != 0 && g_x87_spec >= 0;
        if !known {
            x87_phys(b, X87P, if mem != 0 { 0 } else { o.reg as c_int });
            x87_slot(b, X87Q, X87P);
        }
        let la = x87_lane_get(b, drel);
        if la >= 0 {
            va = la;
        } else if known {
            ffi::a64_ldr_v(b, 8, va, 20, X87_FPR_OFF + 8 * drel as u32);
        } else {
            ffi::a64_ldr_v(b, 8, va, X87Q, X87_FPR_OFF);
        }
        if mem == 0 {
            let brel = x87_rel(insn_.ops[1].reg as c_int);
            let lb = x87_lane_get(b, brel);
            if lb >= 0 {
                vb = lb;
            } else if known {
                ffi::a64_ldr_v(b, 8, vb, 20, X87_FPR_OFF + 8 * brel as u32);
            } else {
                x87_phys(b, JT0, insn_.ops[1].reg as c_int);
                x87_slot(b, JT0, JT0);
                ffi::a64_ldr_v(b, 8, vb, JT0, X87_FPR_OFF);
            }
        }
        let r0 = if rev != 0 { vb } else { va };
        let r1 = if rev != 0 { va } else { vb };
        match kind {
            XK_ADD => ffi::a64_fadd_s(b, 1, VX2, r0, r1),
            XK_SUB => ffi::a64_fsub_s(b, 1, VX2, r0, r1),
            XK_MUL => ffi::a64_fmul_s(b, 1, VX2, r0, r1),
            _ => ffi::a64_fdiv_s(b, 1, VX2, r0, r1),
        }
        g_x87_fr0 = r0;
        g_x87_fr1 = r1;
        x87_result(b, kind);
        g_x87_fr0 = VX0;
        g_x87_fr1 = VX1;
        if known {
            ffi::a64_str_v(b, 8, VX2, 20, X87_FPR_OFF + 8 * drel as u32);
        } else {
            ffi::a64_str_v(b, 8, VX2, X87Q, X87_FPR_OFF);
        }
        x87_lane_put(b, drel, VX2);
        x87_tag(
            b,
            X87P,
            JT0,
            x87_rel(if mem != 0 { 0 } else { o.reg as c_int }),
            0,
        );
        if popit != 0 {
            x87_pop(b);
        }
        x87_st(b);
        1
    }
}

unsafe fn x87_compare(
    b: *mut ffi::A64Buf,
    insn: *const ffi::X86Insn,
    need: u64,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    unsafe {
        let insn_ = &*insn;
        let op = insn_.op as u32;
        let o = &insn_.ops[0];
        let mem = (insn_.nops == 1 && o.kind as u32 == ffi::OCERZ_OPK_MEM) as c_int;
        let fcomi = (op == ffi::OCERZ_OP_FCOMI
            || op == ffi::OCERZ_OP_FCOMIP
            || op == ffi::OCERZ_OP_FUCOMI
            || op == ffi::OCERZ_OP_FUCOMIP) as c_int;
        let pops = if op == ffi::OCERZ_OP_FCOMPP || op == ffi::OCERZ_OP_FUCOMPP {
            2
        } else if op == ffi::OCERZ_OP_FCOMP
            || op == ffi::OCERZ_OP_FUCOMP
            || op == ffi::OCERZ_OP_FICOMP
            || op == ffi::OCERZ_OP_FCOMIP
            || op == ffi::OCERZ_OP_FUCOMIP
        {
            1
        } else {
            0
        };
        if mem != 0 && (op == ffi::OCERZ_OP_FICOM || op == ffi::OCERZ_OP_FICOMP) {
            if x87_ld_int(b, insn, X87Q, 1, exit_sites, n_exits) == 0 {
                return 0;
            }
            ffi::a64_scvtf(b, 1, 1, VX1, X87Q);
        } else if mem != 0 && x87_ld_real(b, insn, VX1, exit_sites, n_exits) == 0 {
            return 0;
        }
        x87_ld(b);
        let mut c0 = x87_lane_get(b, x87_rel(0));
        if c0 < 0 {
            c0 = VX0;
            x87_top(b, X87P);
            x87_slot(b, X87Q, X87P);
            ffi::a64_ldr_v(b, 8, VX0, X87Q, X87_FPR_OFF);
        }
        if op == ffi::OCERZ_OP_FTST {
            ffi::a64_fcmp_zero(b, 1, c0);
        } else {
            let mut c1 = VX1;
            if mem == 0 {
                let i = if insn_.nops == 2 {
                    insn_.ops[1].reg as c_int
                } else if insn_.nops == 1 {
                    o.reg as c_int
                } else {
                    1
                };
                c1 = x87_lane_get(b, x87_rel(i));
                if c1 < 0 {
                    c1 = VX1;
                    x87_phys(b, JT0, i);
                    x87_slot(b, JT0, JT0);
                    ffi::a64_ldr_v(b, 8, VX1, JT0, X87_FPR_OFF);
                }
            }
            ffi::a64_fcmp(b, 1, c0, c1);
        }
        x87_slow_if(b, A64_VS);
        if fcomi != 0 {
            if need != 0 {
                ffi::a64_cset(b, JTT, A64_MI);
                ffi::a64_cset(b, JTU, A64_EQ);
                if ffi::g_defer != 0 {
                    ffi::a64_str(b, 4, A64_ZR, 20, CC_OP_OFF);
                }
                ffi::a64_ldr(b, 8, JT0, 20, RF_OFF);
                ffi::a64_mov_imm64(b, X87Q, !JIT_ARITH_FLAGS);
                ffi::a64_and_reg(b, 1, JT0, JT0, X87Q, 0);
                ffi::a64_orr_reg(b, 1, JT0, JT0, JTT, 0);
                ffi::a64_orr_reg(b, 1, JT0, JT0, JTU, 6);
                ffi::a64_str(b, 8, JT0, 20, RF_OFF);
            }
            x87_clear_c1(b);
            g_x87_nzcv = g_cur_insn_idx;
        } else {
            ffi::a64_cset(b, JTT, A64_MI);
            ffi::a64_cset(b, JTU, A64_EQ);
            ffi::a64_try_and_imm(b, 1, X87S, X87S, !(7u64 << 24));
            ffi::a64_try_and_imm(b, 1, X87S, X87S, !XS_C3);
            g_x87_c1k = g_x87_live as i8;
            ffi::a64_orr_reg(b, 1, X87S, X87S, JTT, 24);
            ffi::a64_orr_reg(b, 1, X87S, X87S, JTU, 30);
        }
        for _ in 0..pops {
            x87_pop(b);
        }
        x87_st(b);
        1
    }
}

unsafe fn x87_fist(
    b: *mut ffi::A64Buf,
    insn: *const ffi::X86Insn,
    courier: c_int,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    unsafe {
        let insn_ = &*insn;
        let o = &insn_.ops[0];
        let popit = (insn_.op as u32 != ffi::OCERZ_OP_FIST) as c_int;
        let mut c1_set = 0;
        x87_ld(b);
        x87_top(b, X87P);
        if courier != 0 || insn_.op as u32 == ffi::OCERZ_OP_FISTTP {
            c1_set = (g_x87_live == 0 || g_x87_c1k == 0) as c_int;
            x87_clear_c1(b);
        }
        if courier != 0 {
            x87_slot(b, X87Q, X87P);
            ffi::a64_ldr(b, 8, JTT, X87Q, X87_XM_OFF);
            ffi::a64_add_reg(b, 1, X87Q, 20, X87P, 1);
            ffi::a64_ldr(b, 2, JTU, X87Q, X87_XE_OFF);
            ffi::a64_try_and_imm(b, 0, JT0, JTU, 0x7fff);
            ffi::a64_movz(b, X87Q, 16383 + 63, 0);
            ffi::a64_sub_reg(b, 0, JT0, X87Q, JT0, 0);
            ffi::a64_lsrv(b, 1, JTT, JTT, JT0);
            ffi::a64_try_ands_imm(b, 0, A64_ZR, JTU, 0x8000);
            ffi::a64_csneg(b, 1, X87Q, JTT, JTT, A64_EQ);
        } else {
            if g_x87_live == 0 || *xokk(x87_rel(0) as usize) != 2 {
                ffi::a64_lsrv(b, 1, JTT, X87S, X87P);
                ffi::a64_try_ands_imm(b, 1, A64_ZR, JTT, 1u64 << XS_XOK);
                x87_slow_if(b, A64_NE);
            }
            x87_slot(b, X87Q, X87P);
            x87_lane_read(b, VX0, x87_rel(0), X87Q);
            ffi::a64_fcmp(b, 1, VX0, VX0);
            x87_slow_if(b, A64_VS);
            if insn_.op as u32 == ffi::OCERZ_OP_FISTTP {
                ffi::a64_fcvtzs(b, 1, 1, X87Q, VX0);
            } else if g_x87_rc_near != 0 {
                ffi::a64_fcvtns(b, 1, 1, X87Q, VX0);
            } else {
                let hi = ffi::a64_label(b);
                ffi::a64_tbnz(b, X87S, 11, 0);
                let dn = ffi::a64_label(b);
                ffi::a64_tbnz(b, X87S, 10, 0);
                ffi::a64_fcvtns(b, 1, 1, X87Q, VX0);
                let j1 = ffi::a64_label(b);
                ffi::a64_b(b, 0);
                ffi::a64_patch_tbz(dn, ffi::a64_label(b));
                ffi::a64_fcvtms(b, 1, 1, X87Q, VX0);
                let j2 = ffi::a64_label(b);
                ffi::a64_b(b, 0);
                ffi::a64_patch_tbz(hi, ffi::a64_label(b));
                let ch = ffi::a64_label(b);
                ffi::a64_tbnz(b, X87S, 10, 0);
                ffi::a64_fcvtps(b, 1, 1, X87Q, VX0);
                let j3 = ffi::a64_label(b);
                ffi::a64_b(b, 0);
                ffi::a64_patch_tbz(ch, ffi::a64_label(b));
                ffi::a64_fcvtzs(b, 1, 1, X87Q, VX0);
                ffi::a64_patch_b(j1, ffi::a64_label(b));
                ffi::a64_patch_b(j2, ffi::a64_label(b));
                ffi::a64_patch_b(j3, ffi::a64_label(b));
            }
            if o.size == 8 {
                ffi::a64_adds_imm(b, 1, A64_ZR, X87Q, 1);
                x87_slow_if(b, A64_VS);
                ffi::a64_subs_imm(b, 1, A64_ZR, X87Q, 1);
                x87_slow_if(b, A64_VS);
            } else {
                if o.size == 2 {
                    ffi::a64_sxth(b, 1, JTT, X87Q);
                } else {
                    ffi::a64_sxtw(b, JTT, X87Q);
                }
                ffi::a64_subs_reg(b, 1, A64_ZR, JTT, X87Q, 0);
                x87_slow_if(b, A64_NE);
            }
            if insn_.op as u32 != ffi::OCERZ_OP_FISTTP {
                ffi::a64_fcvtzs(b, 1, 1, JTT, VX0);
                ffi::a64_subs_reg(b, 1, A64_ZR, X87Q, JTT, 0);
                ffi::a64_cset(b, JTU, A64_NE);
                ffi::a64_bfi(b, 1, X87S, JTU, 25, 1);
                g_x87_c1k = 0;
                c1_set = 1;
            }
            ffi::a64_scvtf(b, 1, 1, VX1, X87Q);
            ffi::a64_fcmp(b, 1, VX1, VX0);
            x87_frag_if(b, A64_NE, XF_SETPE, 0);
            x87_frag_land(b);
        }
        if x87_st_mem(b, insn, 0, X87Q, exit_sites, n_exits) == 0 {
            return 0;
        }
        if popit != 0 {
            x87_pop(b);
            x87_st(b);
        } else if c1_set != 0 {
            x87_st(b);
        }
        1
    }
}

unsafe fn x87_fcmov_cond(cc: c_uint) -> c_int {
    match cc as u32 {
        ffi::OCERZ_CC_B => A64_MI,
        ffi::OCERZ_CC_AE => A64_CS,
        ffi::OCERZ_CC_E => A64_EQ,
        ffi::OCERZ_CC_NE => A64_NE,
        ffi::OCERZ_CC_BE => A64_LS,
        ffi::OCERZ_CC_A => A64_HI,
        ffi::OCERZ_CC_P => A64_NV,
        _ => A64_AL,
    }
}

unsafe fn emit_x87_one(
    b: *mut ffi::A64Buf,
    insn: *const ffi::X86Insn,
    need: u64,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    unsafe {
        let insn_ = &*insn;
        let o = &insn_.ops[0];
        let op = insn_.op as u32;
        let st = (insn_.nops >= 1 && o.kind as u32 == ffi::OCERZ_OPK_ST) as c_int;
        match op {
            ffi::OCERZ_OP_FLD => {
                if st != 0 {
                    x87_ld(b);
                    if o.reg == 7 {
                        x87_newtop(b, X87P);
                        x87_tag(b, X87P, JT0, x87_rel(-1), 0);
                    } else {
                        x87_phys(b, JT0, o.reg as c_int);
                        x87_newtop(b, X87P);
                        x87_copy(b, X87P, JT0, x87_rel(-1), x87_rel(o.reg as c_int));
                    }
                    ffi::a64_bfi(b, 1, X87S, X87P, 40, 8);
                    g_x87_delta -= 1;
                    x87_st(b);
                    return 1;
                }
                if x87_ld_real(b, insn, VX0, exit_sites, n_exits) == 0 {
                    return 0;
                }
                ffi::a64_fcmp(b, 1, VX0, VX0);
                x87_slow_if(b, A64_VS);
                x87_ld(b);
                x87_push(b, VX0, 0, 0, 0);
                1
            }
            ffi::OCERZ_OP_FST | ffi::OCERZ_OP_FSTP => {
                x87_ld(b);
                if st != 0 {
                    if o.reg != 0 {
                        x87_phys(b, X87P, o.reg as c_int);
                        x87_top(b, JT0);
                        x87_copy(b, X87P, JT0, x87_rel(o.reg as c_int), x87_rel(0));
                    }
                    if op == ffi::OCERZ_OP_FSTP {
                        x87_pop(b);
                    }
                    if o.reg != 0 || op == ffi::OCERZ_OP_FSTP {
                        x87_st(b);
                    }
                    return 1;
                }
                x87_top(b, X87P);
                x87_slot(b, X87Q, X87P);
                x87_lane_read(b, VX0, x87_rel(0), X87Q);
                if o.size == 4 {
                    ffi::a64_fcvt_d2s(b, VX1, VX0);
                    ffi::a64_fcvt_s2d(b, VX2, VX1);
                    ffi::a64_fcmp(b, 1, VX2, VX0);
                    x87_frag_if(b, A64_NE, XF_ST32, 0);
                    x87_frag_land(b);
                    if x87_st_mem(b, insn, 1, VX1, exit_sites, n_exits) == 0 {
                        return 0;
                    }
                } else {
                    ffi::a64_fcmp(b, 1, VX0, VX0);
                    x87_slow_if(b, A64_VS);
                    if x87_st_mem(b, insn, 1, VX0, exit_sites, n_exits) == 0 {
                        return 0;
                    }
                }
                if op == ffi::OCERZ_OP_FSTP {
                    x87_pop(b);
                    x87_st(b);
                }
                1
            }
            ffi::OCERZ_OP_FILD => {
                if x87_ld_int(b, insn, X87Q, 1, exit_sites, n_exits) == 0 {
                    return 0;
                }
                ffi::a64_scvtf(b, 1, 1, VX0, X87Q);
                x87_int_image(b, X87Q, JTT, JTU, JT0);
                x87_ld(b);
                x87_push(b, VX0, 1, JTT, JTU);
                1
            }
            ffi::OCERZ_OP_FIST | ffi::OCERZ_OP_FISTP | ffi::OCERZ_OP_FISTTP => {
                let i = g_cur_insn_idx;
                let courier = (op == ffi::OCERZ_OP_FISTP
                    && o.size == 8
                    && i as i16 > (*run(g_x87_cur as usize)).first
                    && (*g_cur_insns.add(i as usize - 1)).op as u32 == ffi::OCERZ_OP_FILD
                    && (*g_cur_insns.add(i as usize - 1)).ops[0].size == 8)
                    as c_int;
                x87_fist(b, insn, courier, exit_sites, n_exits)
            }
            ffi::OCERZ_OP_FLDZ
            | ffi::OCERZ_OP_FLD1
            | ffi::OCERZ_OP_FLDPI
            | ffi::OCERZ_OP_FLDL2E
            | ffi::OCERZ_OP_FLDL2T
            | ffi::OCERZ_OP_FLDLG2
            | ffi::OCERZ_OP_FLDLN2 => {
                let mant: u64;
                let se: u32;
                match op {
                    ffi::OCERZ_OP_FLDZ => {
                        mant = 0;
                        se = 0;
                    }
                    ffi::OCERZ_OP_FLD1 => {
                        mant = 1u64 << 63;
                        se = 0x3fff;
                    }
                    ffi::OCERZ_OP_FLDPI => {
                        mant = 0xc90fdaa22168c235;
                        se = 0x4000;
                    }
                    ffi::OCERZ_OP_FLDL2E => {
                        mant = 0xb8aa3b295c17f0bc;
                        se = 0x3fff;
                    }
                    ffi::OCERZ_OP_FLDL2T => {
                        mant = 0xd49a784bcd1b8afe;
                        se = 0x4000;
                    }
                    ffi::OCERZ_OP_FLDLG2 => {
                        mant = 0x9a209a84fbcff799;
                        se = 0x3ffd;
                    }
                    _ => {
                        mant = 0xb17217f7d1cf79ac;
                        se = 0x3ffe;
                    }
                }
                ffi::a64_mov_imm64(b, JTT, ffi::ocerz_x87_f80_dbits(mant, se));
                ffi::a64_fmov_v_from_x(b, 1, VX0, JTT);
                ffi::a64_mov_imm64(b, JTT, mant);
                ffi::a64_movz(b, JTU, se as u16, 0);
                x87_ld(b);
                x87_push(b, VX0, 1, JTT, JTU);
                1
            }
            ffi::OCERZ_OP_FADD
            | ffi::OCERZ_OP_FADDP
            | ffi::OCERZ_OP_FIADD
            | ffi::OCERZ_OP_FSUB
            | ffi::OCERZ_OP_FSUBP
            | ffi::OCERZ_OP_FISUB
            | ffi::OCERZ_OP_FSUBR
            | ffi::OCERZ_OP_FSUBRP
            | ffi::OCERZ_OP_FISUBR
            | ffi::OCERZ_OP_FMUL
            | ffi::OCERZ_OP_FMULP
            | ffi::OCERZ_OP_FIMUL
            | ffi::OCERZ_OP_FDIV
            | ffi::OCERZ_OP_FDIVP
            | ffi::OCERZ_OP_FIDIV
            | ffi::OCERZ_OP_FDIVR
            | ffi::OCERZ_OP_FDIVRP
            | ffi::OCERZ_OP_FIDIVR => x87_arith(b, insn, exit_sites, n_exits),
            ffi::OCERZ_OP_FSQRT | ffi::OCERZ_OP_FRNDINT => {
                x87_ld(b);
                x87_top(b, X87P);
                x87_slot(b, X87Q, X87P);
                x87_lane_read(b, VX0, x87_rel(0), X87Q);
                if op == ffi::OCERZ_OP_FSQRT {
                    ffi::a64_fsqrt_s(b, 1, VX2, VX0);
                    x87_result(b, XK_SQRT);
                } else {
                    ffi::a64_frint_s(b, 1, 0, VX2, VX0);
                    ffi::a64_fcmp(b, 1, VX2, VX0);
                    x87_slow_if(b, A64_VS);
                    x87_frag_if(b, A64_NE, XF_SETPE, 0);
                    x87_frag_land(b);
                }
                ffi::a64_str_v(b, 8, VX2, X87Q, X87_FPR_OFF);
                x87_lane_put(b, x87_rel(0), VX2);
                x87_tag(b, X87P, JT0, x87_rel(0), 0);
                x87_st(b);
                1
            }
            ffi::OCERZ_OP_FCHS | ffi::OCERZ_OP_FABS => {
                x87_ld(b);
                x87_top(b, X87P);
                x87_slot(b, X87Q, X87P);
                ffi::a64_ldr(b, 8, JTT, X87Q, X87_FPR_OFF);
                if op == ffi::OCERZ_OP_FCHS {
                    ffi::a64_try_eor_imm(b, 1, JTT, JTT, 1u64 << 63);
                } else {
                    ffi::a64_try_and_imm(b, 1, JTT, JTT, !(1u64 << 63));
                }
                ffi::a64_str(b, 8, JTT, X87Q, X87_FPR_OFF);
                if x87_lane_of(x87_rel(0)) >= 0 {
                    ffi::a64_fmov_v_from_x(b, 1, VX0, JTT);
                    x87_lane_put(b, x87_rel(0), VX0);
                }
                x87_tag(b, X87P, JT0, x87_rel(0), 0);
                x87_clear_c1(b);
                x87_st(b);
                1
            }
            ffi::OCERZ_OP_FXCH => {
                x87_ld(b);
                if o.reg != 0 {
                    x87_top(b, X87P);
                    x87_phys(b, JT0, o.reg as c_int);
                    x87_slot(b, X87Q, X87P);
                    x87_slot(b, JTT, JT0);
                    let xl0 = x87_lane_get(b, x87_rel(0));
                    let xl1 = x87_lane_get(b, x87_rel(o.reg as c_int));
                    if xl0 >= 0 && xl1 >= 0 {
                        ffi::a64_str_v(b, 8, xl1, X87Q, X87_FPR_OFF);
                        ffi::a64_str_v(b, 8, xl0, JTT, X87_FPR_OFF);
                        let p0 = x87_rel(0);
                        let p1 = x87_rel(o.reg as c_int);
                        *lane(p0 as usize) = xl1 as i8;
                        *lane(p1 as usize) = xl0 as i8;
                    } else {
                        ffi::a64_ldr_v(b, 8, VX0, X87Q, X87_FPR_OFF);
                        ffi::a64_ldr_v(b, 8, VX1, JTT, X87_FPR_OFF);
                        ffi::a64_str_v(b, 8, VX1, X87Q, X87_FPR_OFF);
                        ffi::a64_str_v(b, 8, VX0, JTT, X87_FPR_OFF);
                    }
                    let ra = x87_rel(0);
                    let rc = x87_rel(o.reg as c_int);
                    let xa = if g_x87_live != 0 {
                        *xokk(ra as usize) as c_int
                    } else {
                        0
                    };
                    let xc = if g_x87_live != 0 {
                        *xokk(rc as usize) as c_int
                    } else {
                        0
                    };
                    let known = xa != 0 && xc != 0;
                    let any_img = xa == 1 || xc == 1;
                    let mut noimg: *mut u32 = core::ptr::null_mut();
                    if !known {
                        ffi::a64_lsrv(b, 1, X87Q, X87S, X87P);
                        ffi::a64_lsrv(b, 1, JTT, X87S, JT0);
                        ffi::a64_orr_reg(b, 1, X87Q, X87Q, JTT, 0);
                        noimg = ffi::a64_label(b);
                        ffi::a64_tbz(b, X87Q, XS_XOK as c_int, 0);
                    }
                    if !known || any_img {
                        x87_slot(b, X87Q, X87P);
                        x87_slot(b, JTT, JT0);
                        ffi::a64_ldr_v(b, 8, VX2, X87Q, X87_XM_OFF);
                        ffi::a64_ldr_v(b, 8, VX3, JTT, X87_XM_OFF);
                        ffi::a64_str_v(b, 8, VX3, X87Q, X87_XM_OFF);
                        ffi::a64_str_v(b, 8, VX2, JTT, X87_XM_OFF);
                        ffi::a64_add_reg(b, 1, X87Q, 20, X87P, 1);
                        ffi::a64_add_reg(b, 1, JTT, 20, JT0, 1);
                        ffi::a64_ldr(b, 2, JTU, X87Q, X87_XE_OFF);
                        ffi::a64_ldr(b, 2, X87P, JTT, X87_XE_OFF);
                        ffi::a64_str(b, 2, X87P, X87Q, X87_XE_OFF);
                        ffi::a64_str(b, 2, JTU, JTT, X87_XE_OFF);
                        x87_top(b, X87P);
                    }
                    if !noimg.is_null() {
                        ffi::a64_patch_tbz(noimg, ffi::a64_label(b));
                    }
                    if !known {
                        ffi::a64_lsrv(b, 1, JTU, X87S, X87P);
                        ffi::a64_lsrv(b, 1, JTT, X87S, JT0);
                        ffi::a64_eor_reg(b, 1, JTU, JTU, JTT, 0);
                        ffi::a64_ubfx(b, 1, JTU, JTU, XS_XOK as c_int, 1);
                        ffi::a64_lslv(b, 0, JTT, JTU, X87P);
                        ffi::a64_lslv(b, 0, JTU, JTU, JT0);
                        ffi::a64_orr_reg(b, 0, JTU, JTU, JTT, 0);
                        ffi::a64_eor_reg(b, 1, X87S, X87S, JTU, XS_XOK as c_int);
                    } else if xa != xc {
                        x87_bit(b, JTT, X87P);
                        x87_bit(b, JTU, JT0);
                        ffi::a64_orr_reg(b, 0, JTU, JTU, JTT, 0);
                        ffi::a64_eor_reg(b, 1, X87S, X87S, JTU, XS_XOK as c_int);
                    }
                    let ta = g_x87_live == 0 || *tagk(ra as usize) != 1;
                    let tc = g_x87_live == 0 || *tagk(rc as usize) != 1;
                    if ta {
                        x87_bit(b, JTT, X87P);
                        ffi::a64_orr_reg(b, 1, X87S, X87S, JTT, 32);
                    }
                    if tc {
                        x87_bit(b, JTU, JT0);
                        ffi::a64_orr_reg(b, 1, X87S, X87S, JTU, 32);
                    }
                    if g_x87_live != 0 {
                        *xokk(ra as usize) = xc as i8;
                        *xokk(rc as usize) = xa as i8;
                        *tagk(ra as usize) = 1;
                        *tagk(rc as usize) = 1;
                    }
                }
                x87_clear_c1(b);
                x87_st(b);
                1
            }
            ffi::OCERZ_OP_FCOM
            | ffi::OCERZ_OP_FCOMP
            | ffi::OCERZ_OP_FCOMPP
            | ffi::OCERZ_OP_FUCOM
            | ffi::OCERZ_OP_FUCOMP
            | ffi::OCERZ_OP_FUCOMPP
            | ffi::OCERZ_OP_FICOM
            | ffi::OCERZ_OP_FICOMP
            | ffi::OCERZ_OP_FTST
            | ffi::OCERZ_OP_FCOMI
            | ffi::OCERZ_OP_FCOMIP
            | ffi::OCERZ_OP_FUCOMI
            | ffi::OCERZ_OP_FUCOMIP => x87_compare(b, insn, need, exit_sites, n_exits),
            ffi::OCERZ_OP_FCMOVCC => {
                let i = insn_.ops[1].reg as c_int;
                if i == 0 {
                    return 1;
                }
                let mut skip: *mut u32 = core::ptr::null_mut();
                x87_lane_get(b, x87_rel(0));
                x87_lane_get(b, x87_rel(i));
                if g_x87_nzcv_live != 0 {
                    let cond = x87_fcmov_cond(insn_.cc as c_uint);
                    if cond == A64_NV {
                        if g_x87_live != 0 {
                            *tagk(x87_rel(0) as usize) = 0;
                            *xokk(x87_rel(0) as usize) = 0;
                        }
                        return 1;
                    }
                    if cond == A64_AL {
                        g_x87_fcmov_static = 1;
                    }
                    if cond != A64_AL {
                        skip = ffi::a64_label(b);
                        ffi::a64_bcond(b, a64_inv(cond), 0);
                    }
                } else {
                    emit_cc_predicate(b, insn_.cc as c_uint);
                    ffi::a64_cset(b, X87Q, A64_NE);
                    x87_reload(b);
                    skip = ffi::a64_label(b);
                    ffi::a64_cbz(b, 0, X87Q, 0);
                }
                x87_ld(b);
                x87_top(b, X87P);
                x87_phys(b, JT0, i);
                let mut tk = [0i8; 8];
                let mut xk = [0i8; 8];
                for k in 0..8 {
                    tk[k] = *tagk(k);
                    xk[k] = *xokk(k);
                }
                x87_copy(b, X87P, JT0, x87_rel(0), x87_rel(i));
                if !skip.is_null() {
                    for k in 0..8 {
                        if tk[k] != *tagk(k) {
                            *tagk(k) = 0;
                        }
                        if xk[k] != *xokk(k) {
                            *xokk(k) = 0;
                        }
                    }
                }
                x87_st(b);
                if !skip.is_null() && g_x87_nzcv_live != 0 {
                    ffi::a64_patch_bcond(skip, ffi::a64_label(b));
                } else if !skip.is_null() {
                    ffi::a64_patch_cbz(skip, ffi::a64_label(b));
                }
                if g_x87_fcmov_static != 0 && g_x87_live != 0 {
                    *tagk(x87_rel(0) as usize) = 0;
                    *xokk(x87_rel(0) as usize) = 0;
                }
                g_x87_fcmov_static = 0;
                1
            }
            ffi::OCERZ_OP_FNSTSW => {
                x87_ld(b);
                ffi::a64_ubfx(b, 1, X87Q, X87S, 16, 16);
                x87_top(b, JTT);
                ffi::a64_bfi(b, 0, X87Q, JTT, 11, 3);
                if o.kind as u32 == ffi::OCERZ_OPK_MEM {
                    return x87_st_mem(b, insn, 0, X87Q, exit_sites, n_exits);
                }
                let s = pin_slot(ffi::OCERZ_RAX as c_uint);
                if s >= 0 {
                    ffi::a64_bfi(b, 1, pin_hreg(s), X87Q, 0, 16);
                } else {
                    emit_gpr_rd(b, 1, JT0, ffi::OCERZ_RAX as c_uint);
                    ffi::a64_bfi(b, 1, JT0, X87Q, 0, 16);
                    emit_gpr_wr(b, JT0, ffi::OCERZ_RAX as c_uint);
                    x87_reload(b);
                }
                1
            }
            ffi::OCERZ_OP_FNSTCW => {
                x87_ld(b);
                x87_st_mem(b, insn, 0, X87S, exit_sites, n_exits)
            }
            ffi::OCERZ_OP_FLDCW => {
                if x87_ld_int(b, insn, X87Q, 0, exit_sites, n_exits) == 0 {
                    return 0;
                }
                ffi::a64_try_orr_imm(b, 0, X87Q, X87Q, 0x40);
                x87_ld(b);
                ffi::a64_bfi(b, 1, X87S, X87Q, 0, 16);
                x87_st(b);
                1
            }
            ffi::OCERZ_OP_FNINIT => {
                x87_ld(b);
                ffi::a64_try_and_imm(b, 1, X87S, X87S, 0xffff000000000000);
                ffi::a64_movz(b, X87Q, 0x037f, 0);
                ffi::a64_orr_reg(b, 1, X87S, X87S, X87Q, 0);
                x87_st(b);
                x87_know_reset();
                if g_x87_live != 0 && g_x87_spec >= 0 {
                    g_x87_delta = -g_x87_spec;
                    core::ptr::write_bytes(&raw mut g_x87_tagk, 2, 1);
                }
                1
            }
            ffi::OCERZ_OP_FNCLEX => {
                x87_ld(b);
                ffi::a64_try_and_imm(b, 1, X87S, X87S, !(0xffu64 << 16));
                ffi::a64_try_and_imm(b, 1, X87S, X87S, !(1u64 << 31));
                x87_st(b);
                1
            }
            ffi::OCERZ_OP_FWAIT => 1,
            ffi::OCERZ_OP_FFREE | ffi::OCERZ_OP_FFREEP => {
                x87_ld(b);
                x87_phys(b, X87P, o.reg as c_int);
                if g_x87_live == 0 || *tagk(x87_rel(o.reg as c_int) as usize) != 2 {
                    x87_bit(b, JT0, X87P);
                    ffi::a64_bic_reg(b, 1, X87S, X87S, JT0, 32);
                }
                if g_x87_live != 0 {
                    *tagk(x87_rel(o.reg as c_int) as usize) = 2;
                }
                if op == ffi::OCERZ_OP_FFREEP {
                    x87_pop(b);
                }
                x87_st(b);
                1
            }
            ffi::OCERZ_OP_FINCSTP | ffi::OCERZ_OP_FDECSTP => {
                x87_ld(b);
                if op == ffi::OCERZ_OP_FINCSTP {
                    x87_phys(b, X87P, 1);
                } else {
                    x87_newtop(b, X87P);
                }
                ffi::a64_bfi(b, 1, X87S, X87P, 40, 8);
                g_x87_delta += if op == ffi::OCERZ_OP_FINCSTP { 1 } else { -1 };
                x87_clear_c1(b);
                x87_st(b);
                1
            }
            _ => 0,
        }
    }
}

static mut S_NO_X87_LIVE: c_int = -1;
static mut S_NO_X87_TOPSPEC: c_int = -1;
static mut S_NO_X87_KCARRY: c_int = -1;

unsafe fn x87_run_open(b: *mut ffi::A64Buf, idx: c_int) -> c_int {
    unsafe {
        if g_n_x87_run as usize >= X87_RUN_MAX
            || g_n_x87_site as usize + 2 > X87_SITE_MAX
            || g_n_x87_frag as usize + 1 > X87_FRAG_MAX
        {
            return 0;
        }
        if g_cur_blk.is_null()
            || (*g_cur_blk).insns != g_cur_insns as *mut ffi::X86Insn
            || g_keep.is_null()
            || idx >= g_keep_n
        {
            return 0;
        }
        let mut last = idx;
        let mut need = 0;
        for j in idx..g_cur_insns_n {
            let f = x87_run_flags(g_cur_insns.add(j as usize));
            if f == 0 {
                break;
            }
            need |= f;
            last = j;
            if (f & X87R_END) != 0 {
                break;
            }
        }
        if g_scpend.valid != 0 {
            scalar_pend_flush(b);
        }
        let r = run(g_n_x87_run as usize);
        (*r).first = idx as i16;
        (*r).last = last as i16;
        (*r).back = core::ptr::null_mut();
        for k in 0..16usize {
            (*r).l0[k] = *(&raw const g_l0).cast::<i8>().add(k);
            (*r).l0_dbl[k] = *(&raw const g_l0_dbl).cast::<u8>().add(k);
        }
        (*r).l0_dirty = g_l0_dirty;
        (*r).yc_dirty = g_yc_dirty;
        g_x87_cur = g_n_x87_run;
        g_n_x87_run += 1;
        g_x87_frag_open = g_n_x87_frag;
        if (need & X87R_MXCSR) != 0 {
            ffi::a64_ldr(b, 4, JT0, 20, X87_MXCSR_OFF);
            ffi::a64_try_ands_imm(b, 0, A64_ZR, JT0, 0x6000);
            x87_slow_if(b, A64_NE);
        }
        let nolive = env_on(c"OCERZ_NO_X87_LIVE", &raw mut S_NO_X87_LIVE);
        g_x87_live = 0;
        g_x87_delta = 0;
        g_x87_spec = -1;
        if nolive != 0 {
            g_x87_lv = 0;
        }
        if nolive == 0 && g_x87_btop >= 0 && !getenv_on(c"OCERZ_NO_X87_TOPSPEC") {
            ffi::a64_ldr(b, 8, X87S, 20, X87_CTL_OFF);
            ffi::a64_ubfx(b, 1, JT0, X87S, 40, 3);
            ffi::a64_subs_imm(b, 0, A64_ZR, JT0, g_x87_btop as u32);
            x87_slow_if(b, A64_NE);
            g_x87_spec = g_x87_btop;
            g_x87_live = 1;
        } else if nolive == 0 {
            g_x87_lv = 0;
            ffi::a64_ldr(b, 8, X87S, 20, X87_CTL_OFF);
            ffi::a64_ubfx(b, 1, JT0, X87S, 40, 3);
            ffi::a64_ldr(b, 2, JTT, 20, X87_TOP0_OFF);
            ffi::a64_subs_reg(b, 0, A64_ZR, JT0, JTT, 0);
            x87_frag_if(b, A64_NE, XF_TOP0, 0);
            x87_frag_land(b);
            g_x87_live = 1;
        }
        if !(g_x87_spec >= 0 && g_x87_kcarry != 0 && !getenv_on(c"OCERZ_NO_X87_KCARRY")) {
            x87_know_reset();
        }
        g_x87_c1k = 0;
        g_x87_rc_near = ((need & X87R_FCW) != 0) as c_int;
        if (need & X87R_FCW) != 0 {
            if g_x87_live == 0 {
                ffi::a64_ldr(b, 2, JT0, 20, X87_CTL_OFF);
            }
            ffi::a64_try_ands_imm(
                b,
                0,
                A64_ZR,
                if g_x87_live != 0 { X87S } else { JT0 },
                0xc00,
            );
            x87_slow_if(b, A64_NE);
        }
        g_x87_st_mark = if g_x87_live != 0 {
            (*b).p as *const u32
        } else {
            core::ptr::null()
        };
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_x87(
    b: *mut ffi::A64Buf,
    insn: *const ffi::X86Insn,
    need: u64,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    unsafe {
        let idx = g_cur_insn_idx;
        if x87_inline_ok(insn) == 0
            || g_cur_insns.is_null()
            || idx < 0
            || idx >= g_cur_insns_n
            || insn != g_cur_insns.add(idx as usize)
        {
            return 0;
        }
        if g_x87_cur < 0
            || idx < (*run(g_x87_cur as usize)).first as c_int
            || idx > (*run(g_x87_cur as usize)).last as c_int
        {
            g_x87_cur = -1;
            g_x87_live = 0;
            if x87_run_open(b, idx) == 0 {
                return 0;
            }
        }
        let r = run(g_x87_cur as usize);
        g_x87_nzcv_live = (g_x87_nzcv == idx - 1 && idx > (*r).first as c_int) as c_int;
        g_x87_nzcv = -1;
        let ok = (g_n_x87_site as usize + 16 <= X87_SITE_MAX
            && g_n_x87_frag as usize + 4 <= X87_FRAG_MAX
            && emit_x87_one(b, insn, need, exit_sites, n_exits) != 0) as c_int;
        x87_frag_land(b);
        if ok == 0 {
            ffi::emit_slowcall(b, insn, exit_sites, n_exits);
            (*r).last = idx as i16;
            g_x87_lv = 0;
            g_x87_spec_cut = 1;
        }
        if idx == (*r).last as c_int {
            (*r).top_end = if g_x87_live != 0 && g_x87_spec >= 0 {
                ((g_x87_spec + g_x87_delta) & 7) as i8
            } else {
                -1
            };
            g_x87_kcarry = (g_x87_live != 0 && g_x87_spec >= 0 && g_x87_spec_cut == 0) as c_int;
            g_x87_spec_cut = 0;
            (*r).lv_end = g_x87_lv;
            for p in 0..8 {
                (*r).lmap[p] = *lane(p);
            }
            if g_x87_live != 0 && g_x87_spec >= 0 {
                g_x87_btop = (g_x87_spec + g_x87_delta) & 7;
                ffi::a64_movz(b, JT0, g_x87_btop as u16, 0);
                ffi::a64_str(b, 2, JT0, 20, X87_TOP0_OFF);
            } else if g_x87_live != 0 && (g_x87_delta & 7) != 0 {
                ffi::a64_ldr(b, 2, JT0, 20, X87_TOP0_OFF);
                ffi::a64_add_imm(b, 0, JT0, JT0, (g_x87_delta & 7) as u32);
                ffi::a64_try_and_imm(b, 0, JT0, JT0, 7);
                ffi::a64_str(b, 2, JT0, 20, X87_TOP0_OFF);
            }
            if g_x87_spec < 0 {
                g_x87_btop = -1;
            }
            g_x87_spec = -1;
            (*r).back = ffi::a64_label(b);
            g_x87_cur = -1;
            g_x87_live = 0;
        }
        1
    }
}

unsafe fn x87_frag_exact(
    b: *mut ffi::A64Buf,
    op: c_int,
    run_: c_int,
    idx: c_int,
    r0: c_int,
    r1: c_int,
    pe: *mut *mut u32,
    np: *mut c_int,
    ok: *mut *mut u32,
    nok: *mut c_int,
) {
    unsafe {
        match op {
            XK_ADD | XK_SUB => {
                if op == XK_ADD {
                    ffi::a64_fsub_s(b, 1, VX3, VX2, r0);
                } else {
                    ffi::a64_fsub_s(b, 1, VX3, r0, VX2);
                }
                ffi::a64_fcmp(b, 1, VX3, r1);
                *pe.add(*np as usize) = ffi::a64_label(b);
                *np += 1;
                ffi::a64_bcond(b, A64_NE, 0);
                if op == XK_ADD {
                    ffi::a64_fsub_s(b, 1, VX3, VX2, r1);
                } else {
                    ffi::a64_fadd_s(b, 1, VX3, VX2, r1);
                }
                ffi::a64_fcmp(b, 1, VX3, r0);
            }
            XK_MUL => {
                ffi::a64_fmadd_s(b, 1, 0, 1, VX3, r0, r1, VX2);
                ffi::a64_fcmp_zero(b, 1, VX3);
            }
            _ => {
                if op == XK_SQRT {
                    ffi::a64_fcmp_zero(b, 1, r0);
                    *ok.add(*nok as usize) = ffi::a64_label(b);
                    *nok += 1;
                    ffi::a64_bcond(b, A64_EQ, 0);
                }
                ffi::a64_fmov_x_from_v(b, 1, JTT, r0);
                ffi::a64_ubfx(b, 1, JTT, JTT, 52, 11);
                ffi::a64_subs_imm(b, 0, A64_ZR, JTT, if op == XK_SQRT { 54 } else { 63 });
                x87_slow_at(b, A64_CC, run_, idx);
                if op == XK_SQRT {
                    ffi::a64_fmadd_s(b, 1, 1, 0, VX3, VX2, VX2, r0);
                } else {
                    ffi::a64_fmadd_s(b, 1, 1, 0, VX3, VX2, r1, r0);
                }
                ffi::a64_fcmp_zero(b, 1, VX3);
            }
        }
        *pe.add(*np as usize) = ffi::a64_label(b);
        *np += 1;
        ffi::a64_bcond(b, A64_NE, 0);
    }
}

unsafe fn x87_emit_frag(b: *mut ffi::A64Buf, f: c_int) {
    unsafe {
        let fr = frag(f as usize);
        let run_ = (*fr).run as c_int;
        let idx = (*fr).idx as c_int;
        let op = (*fr).op as c_int;
        let r0 = (*fr).r0 as c_int;
        let r1 = (*fr).r1 as c_int;
        let back = (*fr).back;
        let mut pe: [*mut u32; 4] = [core::ptr::null_mut(); 4];
        let mut ok: [*mut u32; 4] = [core::ptr::null_mut(); 4];
        let mut np = 0;
        let mut nok = 0;
        ffi::a64_patch_bcond((*fr).site, ffi::a64_label(b));
        match (*fr).kind as c_int {
            XF_PE => {
                x87_frag_exact(
                    b,
                    op,
                    run_,
                    idx,
                    r0,
                    r1,
                    pe.as_mut_ptr(),
                    &mut np,
                    ok.as_mut_ptr(),
                    &mut nok,
                );
                for k in 0..nok {
                    ffi::a64_patch_bcond(ok[k as usize], ffi::a64_label(b));
                }
                ffi::a64_b(b, back.offset_from(ffi::a64_label(b)) as i32);
                for k in 0..np {
                    ffi::a64_patch_bcond(pe[k as usize], ffi::a64_label(b));
                }
                ffi::a64_try_orr_imm(b, 1, X87S, X87S, XS_PE);
                ffi::a64_str(b, 8, X87S, 20, X87_CTL_OFF);
            }
            XF_PC24 => {
                ffi::a64_ubfx(b, 1, JTT, JT0, 52, 11);
                let mut zero: *mut u32 = core::ptr::null_mut();
                if op != XK_MUL && op != XK_DIV {
                    ffi::a64_lsl_imm(b, 1, JTU, JT0, 1);
                    zero = ffi::a64_label(b);
                    ffi::a64_cbz(b, 1, JTU, 0);
                }
                ffi::a64_sub_imm(b, 0, JTU, JTT, 1);
                ffi::a64_subs_imm(b, 0, A64_ZR, JTU, 0x7fd - 1);
                x87_slow_at(b, A64_HI, run_, idx);
                ffi::a64_try_ands_imm(b, 1, A64_ZR, X87S, XS_PE);
                let known = ffi::a64_label(b);
                ffi::a64_bcond(b, A64_NE, 0);
                ffi::a64_try_and_imm(b, 1, JTU, JT0, 0x1fffffff);
                let cut = ffi::a64_label(b);
                ffi::a64_cbnz(b, 1, JTU, 0);
                x87_frag_exact(
                    b,
                    op,
                    run_,
                    idx,
                    r0,
                    r1,
                    pe.as_mut_ptr(),
                    &mut np,
                    ok.as_mut_ptr(),
                    &mut nok,
                );
                let exact = ffi::a64_label(b);
                ffi::a64_b(b, 0);
                ffi::a64_patch_cbz(cut, ffi::a64_label(b));
                for k in 0..np {
                    ffi::a64_patch_bcond(pe[k as usize], ffi::a64_label(b));
                }
                ffi::a64_try_orr_imm(b, 1, X87S, X87S, XS_PE);
                ffi::a64_str(b, 8, X87S, 20, X87_CTL_OFF);
                ffi::a64_patch_bcond(known, ffi::a64_label(b));
                ffi::a64_patch_b(exact, ffi::a64_label(b));
                for k in 0..nok {
                    ffi::a64_patch_bcond(ok[k as usize], ffi::a64_label(b));
                }
                ffi::a64_ubfx(b, 1, JTT, JT0, 29, 1);
                ffi::a64_add_reg(b, 1, JT0, JT0, JTT, 0);
                ffi::a64_try_orr_imm(b, 1, JTT, A64_ZR, 0x0fffffff);
                ffi::a64_add_reg(b, 1, JT0, JT0, JTT, 0);
                ffi::a64_try_and_imm(b, 1, JT0, JT0, !0x1fffffff);
                ffi::a64_fmov_v_from_x(b, 1, VX2, JT0);
                if !zero.is_null() {
                    ffi::a64_patch_cbz(zero, ffi::a64_label(b));
                }
            }
            XF_ZERO => {
                ffi::a64_lsl_imm(b, 1, JTT, JT0, 1);
                ffi::a64_subs_imm(b, 1, A64_ZR, JTT, 0);
                x87_slow_at(b, A64_NE, run_, idx);
                ffi::a64_fcmp_zero(b, 1, r0);
                ffi::a64_bcond(b, A64_EQ, back.offset_from(ffi::a64_label(b)) as i32);
                if op == XK_MUL {
                    ffi::a64_fcmp_zero(b, 1, r1);
                } else {
                    ffi::a64_fmov_x_from_v(b, 1, JTT, r1);
                    ffi::a64_lsl_imm(b, 1, JTT, JTT, 1);
                    ffi::a64_movz(b, JTU, 0xffe0, 3);
                    ffi::a64_subs_reg(b, 1, A64_ZR, JTT, JTU, 0);
                }
                x87_slow_at(b, A64_NE, run_, idx);
            }
            XF_TOP0 => {
                ffi::a64_str(b, 2, JT0, 20, X87_TOP0_OFF);
            }
            XF_ST32 => {
                ffi::a64_fcmp(b, 1, VX0, VX0);
                x87_slow_at(b, A64_VS, run_, idx);
                ffi::a64_fmov_x_from_v(b, 0, JTT, VX1);
                ffi::a64_ubfx(b, 0, JTT, JTT, 23, 8);
                ffi::a64_sub_imm(b, 0, JTT, JTT, 2);
                ffi::a64_subs_imm(b, 0, A64_ZR, JTT, 0xfe - 2);
                x87_slow_at(b, A64_HI, run_, idx);
                ffi::a64_try_orr_imm(b, 1, X87S, X87S, XS_PE);
                x87_st(b);
            }
            _ => {
                ffi::a64_try_orr_imm(b, 1, X87S, X87S, XS_PE);
                x87_st(b);
            }
        }
        ffi::a64_b(b, back.offset_from(ffi::a64_label(b)) as i32);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_x87_arms(
    b: *mut ffi::A64Buf,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
    epi_sites: *mut *mut u32,
    n_epi: *mut c_int,
) {
    unsafe {
        for f in 0..g_n_x87_frag {
            x87_emit_frag(b, f);
        }
        let save_idx = g_cur_insn_idx;
        for r in 0..g_n_x87_run {
            let run_ = run(r as usize);
            let mut to_common: [*mut u32; JIT_MAX_BLOCK_INSNS] =
                [core::ptr::null_mut(); JIT_MAX_BLOCK_INSNS];
            let mut nc = 0;
            for k in (*run_).first as c_int..=(*run_).last as c_int {
                let mut entry: *mut u32 = core::ptr::null_mut();
                for s in 0..g_n_x87_site {
                    let st = site(s as usize);
                    if (*st).run as c_int != r || (*st).idx as c_int != k {
                        continue;
                    }
                    if entry.is_null() {
                        entry = ffi::a64_label(b);
                        ffi::a64_movz(b, JT0, k as u16, 0);
                        ffi::a64_str(b, 8, JT0, 20, JIT_SCRATCH_OFF);
                        to_common[nc as usize] = ffi::a64_label(b);
                        nc += 1;
                        ffi::a64_b(b, 0);
                    }
                    ffi::a64_patch_bcond((*st).site, entry);
                }
            }
            if nc == 0 {
                continue;
            }
            for c in 0..nc {
                ffi::a64_patch_b(to_common[c], ffi::a64_label(b));
            }
            g_cur_insn_idx = (*run_).first as c_int;
            emit_l0_flush_from(
                b,
                (*run_).l0.as_ptr(),
                (*run_).l0_dbl.as_ptr(),
                (*run_).l0_dirty,
            );
            yc_flush_from(b, (*run_).yc_dirty);
            ffi::g_slow_run_last = (*run_).last as c_int;
            ffi::emit_slowcall(
                b,
                g_cur_insns.add((*run_).first as usize),
                exit_sites,
                n_exits,
            );
            ffi::g_slow_run_last = -1;
            if (*run_).top_end >= 0 {
                ffi::a64_ldr(b, 1, JT0, 20, X87_CTL_OFF + 5);
                ffi::a64_subs_imm(b, 0, A64_ZR, JT0, (*run_).top_end as u32);
                let same = ffi::a64_label(b);
                ffi::a64_bcond(b, A64_EQ, 0);
                let li = &*g_cur_insns.add((*run_).last as usize);
                tc_imm64(b, JT0, TCR_BLK, 0, g_cur_blk as usize as u64);
                ffi::a64_str(b, 8, JT0, 20, SIDE_BLK_OFF);
                ffi::a64_movn(b, JT0, 2, 0);
                ffi::a64_str(b, 4, JT0, 20, SIDE_IDX_OFF);
                ffi::a64_mov_imm64(
                    b,
                    JT0,
                    (li.rip.wrapping_add(li.len as u64))
                        & if li.mode32 != 0 { 0xffffffff } else { !0u64 },
                );
                ffi::a64_str(b, 8, JT0, 20, RIP_OFF);
                ffi::a64_mov_imm64(b, 0, ffi::OCERZ_STEP_PROFILE as u64);
                *epi_sites.add(*n_epi as usize) = ffi::a64_label(b);
                *n_epi += 1;
                ffi::a64_b(b, 0);
                ffi::a64_patch_bcond(same, ffi::a64_label(b));
            }
            emit_l0_reload_from(b, (*run_).l0.as_ptr(), (*run_).l0_dbl.as_ptr());
            for p in 0..8usize {
                if !(*run_).back.is_null() && ((*run_).lv_end >> p & 1) != 0 && (*run_).lmap[p] >= 0
                {
                    ffi::a64_ldr_v(
                        b,
                        8,
                        (*run_).lmap[p] as c_int,
                        20,
                        X87_FPR_OFF + 8 * p as u32,
                    );
                }
            }
            if !(*run_).back.is_null() {
                ffi::a64_b(b, (*run_).back.offset_from(ffi::a64_label(b)) as i32);
            } else {
                let li = &*g_cur_insns.add((*run_).last as usize);
                ffi::a64_mov_imm64(
                    b,
                    JT0,
                    (li.rip.wrapping_add(li.len as u64))
                        & if li.mode32 != 0 { 0xffffffff } else { !0u64 },
                );
                ffi::a64_str(b, 8, JT0, 20, RIP_OFF);
                ffi::a64_mov_imm64(b, 0, ffi::OCERZ_STEP_OK as u64);
                *epi_sites.add(*n_epi as usize) = ffi::a64_label(b);
                *n_epi += 1;
                ffi::a64_b(b, 0);
            }
        }
        g_cur_insn_idx = save_idx;
        x87_reset();
    }
}

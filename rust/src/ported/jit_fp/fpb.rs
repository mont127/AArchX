//! FP batching: scanning a block's scalar and packed FP instructions for runs
//! worth deferring NaN checks on, the two scanner versions (v1 with the
//! deferred-region tail walk, v2 with liveness and store/undo sites), and the
//! check/replay emission helpers.  `mov_sink_scan` finds the mov/shift pairs
//! the integer piece sinks.

use core::ffi::{c_int, c_uint, c_ulong, c_ulonglong};

use super::l0::{g_l0, g_l0_dbl, g_n_undo_lanes, g_undo_vreg};
use super::*;
use crate::ffi;
use crate::jit_internal::*;

#[unsafe(no_mangle)]
pub static mut g_fpb: [ffi::FpBatch; ffi::FPB_MAX as usize] = unsafe { core::mem::zeroed() };

#[unsafe(no_mangle)]
pub static mut g_n_fpb: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_fpb_sites: [ffi::FpbSite; ffi::FPB_SITES_MAX as usize] =
    unsafe { core::mem::zeroed() };

#[unsafe(no_mangle)]
pub static mut g_n_fpb_sites: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_fpb_member: [u8; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];

#[unsafe(no_mangle)]
pub static mut g_fpb_det: [u8; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];

#[unsafe(no_mangle)]
pub static mut g_fpb_sidechk: [u16; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];

static mut g_fpb_mrd: [u16; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];

static mut g_fpb_mwr: [u16; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];

#[inline(always)]
unsafe fn mrd(i: usize) -> *mut u16 {
    (&raw mut g_fpb_mrd).cast::<u16>().add(i)
}
#[inline(always)]
unsafe fn mwr(i: usize) -> *mut u16 {
    (&raw mut g_fpb_mwr).cast::<u16>().add(i)
}
#[inline(always)]
unsafe fn marith(i: usize) -> *mut u8 {
    (&raw mut g_fpb_marith).cast::<u8>().add(i)
}
#[inline(always)]
unsafe fn mmem(i: usize) -> *mut u8 {
    (&raw mut g_fpb_mmem).cast::<u8>().add(i)
}
#[inline(always)]
unsafe fn mstore(i: usize) -> *mut u8 {
    (&raw mut g_fpb_mstore).cast::<u8>().add(i)
}
#[inline(always)]
unsafe fn fpb_at(i: c_int) -> *mut ffi::FpBatch {
    (&raw mut g_fpb).cast::<ffi::FpBatch>().add(i as usize)
}
#[inline(always)]
unsafe fn site_at(i: c_int) -> *mut ffi::FpbSite {
    (&raw mut g_fpb_sites)
        .cast::<ffi::FpbSite>()
        .add(i as usize)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mov_sink_gap_ok(
    in_: *const ffi::X86Insn,
    dreg: c_uint,
    sreg: c_uint,
) -> c_int {
    unsafe {
        let in_ = &*in_;
        match in_.op as u32 {
            ffi::OCERZ_OP_MOV
            | ffi::OCERZ_OP_MOVZX
            | ffi::OCERZ_OP_MOVSX
            | ffi::OCERZ_OP_LEA
            | ffi::OCERZ_OP_ADD
            | ffi::OCERZ_OP_SUB
            | ffi::OCERZ_OP_AND
            | ffi::OCERZ_OP_OR
            | ffi::OCERZ_OP_XOR
            | ffi::OCERZ_OP_CMP
            | ffi::OCERZ_OP_TEST
            | ffi::OCERZ_OP_INC
            | ffi::OCERZ_OP_DEC
            | ffi::OCERZ_OP_NEG
            | ffi::OCERZ_OP_NOT
            | ffi::OCERZ_OP_SHL
            | ffi::OCERZ_OP_SHR
            | ffi::OCERZ_OP_SAR
            | ffi::OCERZ_OP_ADDSS
            | ffi::OCERZ_OP_ADDSD
            | ffi::OCERZ_OP_SUBSS
            | ffi::OCERZ_OP_SUBSD
            | ffi::OCERZ_OP_MULSS
            | ffi::OCERZ_OP_MULSD
            | ffi::OCERZ_OP_DIVSS
            | ffi::OCERZ_OP_DIVSD
            | ffi::OCERZ_OP_SQRTSS
            | ffi::OCERZ_OP_SQRTSD
            | ffi::OCERZ_OP_MINSS
            | ffi::OCERZ_OP_MINSD
            | ffi::OCERZ_OP_MAXSS
            | ffi::OCERZ_OP_MAXSD
            | ffi::OCERZ_OP_ADDPS
            | ffi::OCERZ_OP_ADDPD
            | ffi::OCERZ_OP_SUBPS
            | ffi::OCERZ_OP_SUBPD
            | ffi::OCERZ_OP_MULPS
            | ffi::OCERZ_OP_MULPD
            | ffi::OCERZ_OP_DIVPS
            | ffi::OCERZ_OP_DIVPD
            | ffi::OCERZ_OP_CVTTSS2SI
            | ffi::OCERZ_OP_CVTTSD2SI
            | ffi::OCERZ_OP_UCOMISS
            | ffi::OCERZ_OP_UCOMISD
            | ffi::OCERZ_OP_COMISS
            | ffi::OCERZ_OP_COMISD
            | ffi::OCERZ_OP_MOVAPS
            | ffi::OCERZ_OP_MOVUPS
            | ffi::OCERZ_OP_MOVDQA
            | ffi::OCERZ_OP_MOVDQU
            | ffi::OCERZ_OP_MOVSS
            | ffi::OCERZ_OP_MOVSDX
            | ffi::OCERZ_OP_UNPCKHPD
            | ffi::OCERZ_OP_UNPCKLPD => {}
            _ => return 0,
        }
        if in_.seg as u32 != ffi::OCERZ_SEG_NONE {
            return 0;
        }
        for k in 0..in_.nops as usize {
            let o = &in_.ops[k];
            if o.kind as u32 == ffi::OCERZ_OPK_MEM {
                if in_.op as u32 != ffi::OCERZ_OP_LEA {
                    return 0;
                }
                if o.base as u32 == dreg || o.index as u32 == dreg {
                    return 0;
                }
                continue;
            }
            if o.kind as u32 != ffi::OCERZ_OPK_REG {
                continue;
            }
            if (o.reg & 15) as u32 == (dreg & 15) {
                return 0;
            }
            if k == 0 && (o.reg & 15) as u32 == (sreg & 15) {
                return 0;
            }
        }
        if (in_.op as u32 == ffi::OCERZ_OP_SHL
            || in_.op as u32 == ffi::OCERZ_OP_SHR
            || in_.op as u32 == ffi::OCERZ_OP_SAR)
            && in_.ops[1].kind as u32 != ffi::OCERZ_OPK_IMM
        {
            return 0;
        }
        1
    }
}

static mut S_NO_MOVFUSE: c_int = -1;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mov_sink_scan(insns: *const ffi::X86Insn, n: c_int, fl_need: *const u64) {
    unsafe {
        for i in 0..n as usize {
            *(&raw mut ffi::g_mov_sink_at).cast::<i16>().add(i) = -1;
            *(&raw mut ffi::g_mov_skip).cast::<u8>().add(i) = 0;
        }
        if S_NO_MOVFUSE < 0 {
            S_NO_MOVFUSE =
                (getenv_on(c"OCERZ_NO_MOVFUSE") || getenv_on(c"OCERZ_NO_MOVSINK")) as c_int;
        }
        if S_NO_MOVFUSE != 0 || ffi::g_defer == 0 {
            return;
        }
        for i in 0..n - 1 {
            let m = &*insns.add(i as usize);
            if m.op as u32 != ffi::OCERZ_OP_MOV || m.nops != 2 {
                continue;
            }
            let md = &m.ops[0];
            let ms = &m.ops[1];
            if md.kind as u32 != ffi::OCERZ_OPK_REG
                || ms.kind as u32 != ffi::OCERZ_OPK_REG
                || md.high8 != 0
                || ms.high8 != 0
            {
                continue;
            }
            if (md.size != 4 && md.size != 8) || ms.size != md.size || md.reg == ms.reg {
                continue;
            }
            if pin_slot(md.reg as c_uint) < 0 || pin_slot(ms.reg as c_uint) < 0 {
                continue;
            }
            if rsp_is_ptr() != 0
                && (md.reg as u32 == ffi::OCERZ_RSP || ms.reg as u32 == ffi::OCERZ_RSP)
            {
                continue;
            }
            let mut j = i + 2;
            while j < n && j <= i + 5 {
                let t = &*insns.add(j as usize);
                if mov_sink_gap_ok(
                    insns.add(j as usize - 1),
                    md.reg as c_uint,
                    ms.reg as c_uint,
                ) == 0
                {
                    break;
                }
                if (t.op as u32 == ffi::OCERZ_OP_SHL
                    || t.op as u32 == ffi::OCERZ_OP_SHR
                    || t.op as u32 == ffi::OCERZ_OP_SAR)
                    && t.ops[0].kind as u32 == ffi::OCERZ_OPK_REG
                    && t.ops[0].high8 == 0
                    && t.ops[0].reg == md.reg
                    && t.ops[0].size == md.size
                    && t.ops[1].kind as u32 == ffi::OCERZ_OPK_IMM
                    && *fl_need.add(j as usize) == 0
                    && (t.ops[1].imm & if md.size == 8 { 63 } else { 31 }) != 0
                {
                    *(&raw mut ffi::g_mov_sink_at).cast::<i16>().add(j as usize) = i as i16;
                    *(&raw mut ffi::g_mov_skip).cast::<u8>().add(i as usize) = 1;
                    break;
                }
                if mov_sink_gap_ok(t, md.reg as c_uint, ms.reg as c_uint) == 0 {
                    break;
                }
                j += 1;
            }
        }
    }
}

static mut g_fpb_marith: [u8; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];

static mut g_fpb_mmem: [u8; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];

#[unsafe(no_mangle)]
pub static mut g_fpb_open: c_int = -1;

#[unsafe(no_mangle)]
pub static mut g_fpb_of: *const i8 = core::ptr::null();

pub(crate) unsafe fn mem_may_alias(
    ia: *const ffi::X86Insn,
    a: *const ffi::X86Operand,
    ib: *const ffi::X86Insn,
    b: *const ffi::X86Operand,
) -> c_int {
    unsafe {
        let (a, b) = (&*a, &*b);
        if a.riprel != 0 && b.riprel != 0 {
            let xa = (*ia).rip.wrapping_add((*ia).len as u64) as i64 + a.disp;
            let xb = (*ib).rip.wrapping_add((*ib).len as u64) as i64 + b.disp;
            return (xa < xb + 16 && xb < xa + 16) as c_int;
        }
        if a.riprel != 0 || b.riprel != 0 {
            return 1;
        }
        if a.base != b.base || a.index != b.index || a.scale != b.scale {
            return 1;
        }
        (a.disp < b.disp + 16 && b.disp < a.disp + 16) as c_int
    }
}

pub(crate) unsafe fn fpb_region_class(in_: *const ffi::X86Insn) -> c_int {
    unsafe {
        let in_ = &*in_;
        if in_.vex != 0 || in_.seg as u32 != ffi::OCERZ_SEG_NONE || in_.nops < 2 {
            return RK_END;
        }
        let d = &in_.ops[0];
        let sr = &in_.ops[1];
        let dx = d.kind as u32 == ffi::OCERZ_OPK_XMM && xmm_is_pinned(d.reg as c_uint) != 0;
        let sx = sr.kind as u32 == ffi::OCERZ_OPK_XMM && xmm_is_pinned(sr.reg as c_uint) != 0;
        let dm = d.kind as u32 == ffi::OCERZ_OPK_MEM && in_.addrsize == 8;
        let sm = sr.kind as u32 == ffi::OCERZ_OPK_MEM && (sr.riprel != 0 || in_.addrsize == 8);
        match in_.op as u32 {
            ffi::OCERZ_OP_UCOMISD | ffi::OCERZ_OP_COMISD => {
                if dx && sx {
                    RK_DET
                } else {
                    RK_END
                }
            }
            ffi::OCERZ_OP_MOVUPS
            | ffi::OCERZ_OP_MOVAPS
            | ffi::OCERZ_OP_MOVDQA
            | ffi::OCERZ_OP_MOVDQU => {
                if dx && sx {
                    RK_MOVE
                } else if dm && sx {
                    RK_STORE
                } else if dx && sm {
                    RK_LOAD
                } else {
                    RK_END
                }
            }
            ffi::OCERZ_OP_MOVSDX => {
                if dx && sx {
                    RK_LMOVE
                } else if dm && sx {
                    RK_STORE
                } else if dx && sm {
                    RK_LOAD
                } else {
                    RK_END
                }
            }
            ffi::OCERZ_OP_MOVLPS | ffi::OCERZ_OP_MOVHPS => {
                if dm && sx {
                    RK_STORE
                } else {
                    RK_END
                }
            }
            ffi::OCERZ_OP_UNPCKHPD => {
                if dx && sx && d.reg == sr.reg {
                    RK_UNPCKH
                } else {
                    RK_END
                }
            }
            ffi::OCERZ_OP_UNPCKLPD => {
                if dx && sx && d.reg == sr.reg {
                    RK_UNPCKL
                } else {
                    RK_END
                }
            }
            _ => RK_END,
        }
    }
}

static mut g_fpb_disabled: c_int = -1;

#[unsafe(no_mangle)]
pub static mut g_fpb_v1_active: c_int = 0;

pub(crate) unsafe fn fpb_class(
    in_: *const ffi::X86Insn,
    packed: *mut c_int,
    dbl: *mut c_int,
    from_mem: *mut c_int,
    sqrt_like: *mut c_int,
) -> c_int {
    unsafe {
        let in_ = &*in_;
        *packed = 0;
        *dbl = 0;
        *from_mem = 0;
        *sqrt_like = 0;
        if in_.seg as u32 != ffi::OCERZ_SEG_NONE || in_.nops < 2 {
            return 0;
        }
        if in_.vex != 0 && g_fpb_v1_active != 0 {
            return 0;
        }
        if in_.vex != 0
            && ((in_.vex & ffi::OCERZ_VEX_L as u8) != 0
                || in_.mode32 != 0
                || in_.nops != 2
                || ((in_.vex & ffi::OCERZ_VEX_NDS as u8) != 0
                    && xmm_is_pinned((in_.vvvv & 15) as c_uint) == 0))
        {
            return 0;
        }
        let d = &in_.ops[0];
        let sr = &in_.ops[1];
        if d.kind as u32 != ffi::OCERZ_OPK_XMM || xmm_is_pinned(d.reg as c_uint) == 0 {
            return 0;
        }
        if sr.kind as u32 == ffi::OCERZ_OPK_MEM {
            if sr.riprel != 0 {
                *from_mem = 1;
            } else if in_.addrsize != 8 {
                return 0;
            } else {
                *from_mem = 1;
            }
        } else if sr.kind as u32 != ffi::OCERZ_OPK_XMM || xmm_is_pinned(sr.reg as c_uint) == 0 {
            return 0;
        }
        if in_.op as u32 >= ffi::OCERZ_OP_VFMA_FIRST && in_.op as u32 <= ffi::OCERZ_OP_VFMA_LAST {
            let idx = in_.op as c_int - ffi::OCERZ_OP_VFMA_FIRST as c_int;
            let kind = (idx >> 1) % 10;
            if kind < 2 || (in_.vex & ffi::OCERZ_VEX_NDS as u8) == 0 {
                return 0;
            }
            *dbl = idx & 1;
            *packed = (kind & 1 == 0) as c_int;
            return 1;
        }
        match in_.op as u32 {
            ffi::OCERZ_OP_ADDSS
            | ffi::OCERZ_OP_SUBSS
            | ffi::OCERZ_OP_MULSS
            | ffi::OCERZ_OP_DIVSS
            | ffi::OCERZ_OP_MAXSS
            | ffi::OCERZ_OP_MINSS => 1,
            ffi::OCERZ_OP_ADDSD
            | ffi::OCERZ_OP_SUBSD
            | ffi::OCERZ_OP_MULSD
            | ffi::OCERZ_OP_DIVSD
            | ffi::OCERZ_OP_MAXSD
            | ffi::OCERZ_OP_MINSD => {
                *dbl = 1;
                1
            }
            ffi::OCERZ_OP_ADDPS
            | ffi::OCERZ_OP_SUBPS
            | ffi::OCERZ_OP_MULPS
            | ffi::OCERZ_OP_DIVPS
            | ffi::OCERZ_OP_MAXPS
            | ffi::OCERZ_OP_MINPS => {
                *packed = 1;
                1
            }
            ffi::OCERZ_OP_ADDPD
            | ffi::OCERZ_OP_SUBPD
            | ffi::OCERZ_OP_MULPD
            | ffi::OCERZ_OP_DIVPD
            | ffi::OCERZ_OP_MAXPD
            | ffi::OCERZ_OP_MINPD => {
                *packed = 1;
                *dbl = 1;
                1
            }
            ffi::OCERZ_OP_SQRTSS => {
                *sqrt_like = 1;
                1
            }
            ffi::OCERZ_OP_SQRTSD => {
                *sqrt_like = 1;
                *dbl = 1;
                1
            }
            ffi::OCERZ_OP_SQRTPS => {
                *sqrt_like = 1;
                *packed = 1;
                1
            }
            ffi::OCERZ_OP_SQRTPD => {
                *sqrt_like = 1;
                *packed = 1;
                *dbl = 1;
                1
            }
            ffi::OCERZ_OP_MOVUPS
            | ffi::OCERZ_OP_MOVAPS
            | ffi::OCERZ_OP_MOVDQA
            | ffi::OCERZ_OP_MOVDQU => {
                if (in_.vex & ffi::OCERZ_VEX_NDS as u8) != 0 {
                    0
                } else {
                    2
                }
            }
            ffi::OCERZ_OP_MOVSS => {
                if (in_.vex & ffi::OCERZ_VEX_NDS as u8) != 0 {
                    0
                } else {
                    3
                }
            }
            ffi::OCERZ_OP_MOVSDX => {
                *dbl = 1;
                if (in_.vex & ffi::OCERZ_VEX_NDS as u8) != 0 {
                    0
                } else {
                    3
                }
            }
            _ => 0,
        }
    }
}

#[derive(Clone, Copy, Default)]
struct FpbEdge {
    s: u8,
    d: u8,
    cls: u8,
    t: c_int,
}

static mut S_NO_FPB_ABSORB: c_int = -1;
static mut S_NO_FPB_DEFER: c_int = -1;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn fpb_scan_v1(insns: *const ffi::X86Insn, n: c_int, bat: *mut i8) {
    unsafe {
        g_n_fpb = 0;
        g_n_fpb_sites = 0;
        for i in 0..n as usize {
            *bat.add(i) = -1;
            *(&raw mut g_fpb_member).cast::<u8>().add(i) = 0;
            *(&raw mut g_fpb_det).cast::<u8>().add(i) = 0;
            *(&raw mut g_fpb_sidechk).cast::<u16>().add(i) = 0;
        }
        let mut planned_sites = 0;
        if g_fpb_disabled < 0 {
            g_fpb_disabled = getenv_on(c"OCERZ_NO_FPBATCH") as c_int;
        }
        if g_fpb_disabled != 0 || ffi::sse_enabled() == 0 || ffi::xmm_global_enabled() == 0 {
            return;
        }
        let mut i = 0;
        while i < n {
            let mut packed = 0;
            let mut dbl = 0;
            let mut from_mem = 0;
            let mut sq = 0;
            if fpb_class(
                insns.add(i as usize),
                &mut packed,
                &mut dbl,
                &mut from_mem,
                &mut sq,
            ) == 0
            {
                i += 1;
                continue;
            }
            let mut j = i;
            let mut written: u16 = 0;
            let mut ckpt: u16 = 0;
            let mut full: u16 = 0;
            let mut s0: u16 = 0;
            let mut d0: u16 = 0;
            let mut gain = 0;
            let mut n_arith = 0;
            let mut edges = [FpbEdge::default(); 64];
            let mut n_edges = 0;
            let mut lastw = [-1; 16];
            let mut lastbreak = [-1; 16];
            let mut taint_dbl = [0u8; 16];
            while j < n {
                let c = fpb_class(
                    insns.add(j as usize),
                    &mut packed,
                    &mut dbl,
                    &mut from_mem,
                    &mut sq,
                );
                if c == 0 {
                    break;
                }
                let ij = &*insns.add(j as usize);
                let d = &ij.ops[0];
                let sr = &ij.ops[1];
                let dr = d.reg as c_uint;
                let sbit: u32 = if sr.kind as u32 == ffi::OCERZ_OPK_XMM {
                    1u32 << sr.reg
                } else {
                    0
                };
                if c == 1
                    && sr.kind as u32 == ffi::OCERZ_OPK_XMM
                    && sr.reg as u32 != dr
                    && n_edges < 64
                {
                    let is_minmax_c = ij.op as u32 == ffi::OCERZ_OP_MAXSS
                        || ij.op as u32 == ffi::OCERZ_OP_MINSS
                        || ij.op as u32 == ffi::OCERZ_OP_MAXSD
                        || ij.op as u32 == ffi::OCERZ_OP_MINSD
                        || ij.op as u32 == ffi::OCERZ_OP_MAXPS
                        || ij.op as u32 == ffi::OCERZ_OP_MINPS
                        || ij.op as u32 == ffi::OCERZ_OP_MAXPD
                        || ij.op as u32 == ffi::OCERZ_OP_MINPD;
                    let sb = 1u32 << sr.reg;
                    if !is_minmax_c {
                        if packed != 0
                            && (full as u32 & sb) != 0
                            && taint_dbl[sr.reg as usize] == dbl as u8
                        {
                            edges[n_edges] = FpbEdge {
                                s: sr.reg,
                                d: dr as u8,
                                cls: 0,
                                t: j,
                            };
                            n_edges += 1;
                        }
                        if dbl == 0 && (s0 as u32 & sb) != 0 && n_edges < 64 {
                            edges[n_edges] = FpbEdge {
                                s: sr.reg,
                                d: dr as u8,
                                cls: 1,
                                t: j,
                            };
                            n_edges += 1;
                        }
                        if dbl != 0 && (d0 as u32 & sb) != 0 && n_edges < 64 {
                            edges[n_edges] = FpbEdge {
                                s: sr.reg,
                                d: dr as u8,
                                cls: 2,
                                t: j,
                            };
                            n_edges += 1;
                        }
                    }
                }
                if c == 1 {
                    taint_dbl[dr as usize] = dbl as u8;
                }
                lastw[dr as usize] = j;
                {
                    let is_mm = ij.op as u32 == ffi::OCERZ_OP_MAXSS
                        || ij.op as u32 == ffi::OCERZ_OP_MINSS
                        || ij.op as u32 == ffi::OCERZ_OP_MAXSD
                        || ij.op as u32 == ffi::OCERZ_OP_MINSD
                        || ij.op as u32 == ffi::OCERZ_OP_MAXPS
                        || ij.op as u32 == ffi::OCERZ_OP_MINPS
                        || ij.op as u32 == ffi::OCERZ_OP_MAXPD
                        || ij.op as u32 == ffi::OCERZ_OP_MINPD;
                    if c != 1
                        || is_mm
                        || (sq != 0
                            && !(sr.kind as u32 == ffi::OCERZ_OPK_XMM && sr.reg as u32 == dr))
                    {
                        lastbreak[dr as usize] = j;
                    }
                }
                let mut reads = sbit as u16;
                if c == 1 || c == 3 {
                    reads |= (1u32 << dr) as u16;
                }
                if c == 3 && from_mem != 0 {
                    reads &= !(1u16 << dr);
                }
                ckpt |= reads & !written;
                *mrd(j as usize) = reads;
                *mwr(j as usize) = (1u32 << dr) as u16;
                *mmem(j as usize) = from_mem as u8;
                *marith(j as usize) = (c == 1) as u8;
                let st_full = full & (sbit as u16);
                let st_s0 = s0 & (sbit as u16);
                let st_d0 = d0 & (sbit as u16);
                if c == 1 {
                    if packed != 0 {
                        full |= 1u16 << dr;
                        s0 &= !(1u16 << dr);
                        d0 &= !(1u16 << dr);
                    } else if dbl != 0 {
                        d0 |= 1u16 << dr;
                        s0 &= !(1u16 << dr);
                    } else {
                        s0 |= 1u16 << dr;
                        d0 &= !(1u16 << dr);
                    }
                    let is_minmax = ij.op as u32 == ffi::OCERZ_OP_MAXSS
                        || ij.op as u32 == ffi::OCERZ_OP_MINSS
                        || ij.op as u32 == ffi::OCERZ_OP_MAXSD
                        || ij.op as u32 == ffi::OCERZ_OP_MINSD
                        || ij.op as u32 == ffi::OCERZ_OP_MAXPS
                        || ij.op as u32 == ffi::OCERZ_OP_MINPS
                        || ij.op as u32 == ffi::OCERZ_OP_MAXPD
                        || ij.op as u32 == ffi::OCERZ_OP_MINPD;
                    if !is_minmax {
                        gain += if packed != 0 { 6 } else { 2 };
                        n_arith += 1;
                    }
                } else if c == 2 {
                    if from_mem != 0 {
                        full &= !(1u16 << dr);
                        s0 &= !(1u16 << dr);
                        d0 &= !(1u16 << dr);
                    } else {
                        full = (full & !(1u16 << dr)) | if st_full != 0 { 1u16 << dr } else { 0 };
                        s0 = (s0 & !(1u16 << dr)) | if st_s0 != 0 { 1u16 << dr } else { 0 };
                        d0 = (d0 & !(1u16 << dr)) | if st_d0 != 0 { 1u16 << dr } else { 0 };
                    }
                } else {
                    if from_mem != 0 {
                        full &= !(1u16 << dr);
                        s0 &= !(1u16 << dr);
                        d0 &= !(1u16 << dr);
                    } else {
                        if st_full != 0 {
                            full |= 1u16 << dr;
                        }
                        if dbl != 0 {
                            if st_d0 != 0 || st_full != 0 {
                                d0 |= 1u16 << dr;
                            }
                            s0 &= !(1u16 << dr);
                        } else {
                            if st_s0 != 0 || st_full != 0 {
                                s0 |= 1u16 << dr;
                            }
                            d0 &= !(1u16 << dr);
                        }
                    }
                }
                written |= 1u16 << dr;
                j += 1;
            }
            let mut last = j - 1;
            while last >= i {
                let c = fpb_class(
                    insns.add(last as usize),
                    &mut packed,
                    &mut dbl,
                    &mut from_mem,
                    &mut sq,
                );
                if c == 1 {
                    break;
                }
                last -= 1;
            }
            let ckpt_raw = ckpt;
            ckpt &= written;
            {
                if S_NO_FPB_ABSORB < 0 {
                    S_NO_FPB_ABSORB = getenv_on(c"OCERZ_NO_FPB_ABSORB") as c_int;
                }
                if S_NO_FPB_ABSORB == 0 {
                    for e in 0..n_edges {
                        let s = edges[e].s as usize;
                        let dd = edges[e].d as usize;
                        let t = edges[e].t;
                        if t > last {
                            continue;
                        }
                        if lastbreak[dd] > t {
                            continue;
                        }
                        if lastw[s] > t {
                            continue;
                        }
                        if edges[e].cls == 0 {
                            full &= !(1u16 << s);
                        } else if edges[e].cls == 1 {
                            s0 &= !(1u16 << s);
                        } else {
                            d0 &= !(1u16 << s);
                        }
                    }
                }
            }
            let nregs = ((full | s0 | d0) as u32).count_ones() as c_int;
            let cost = (ckpt as u32).count_ones() as c_int + 3 + nregs;
            if n_arith >= 1
                && gain > cost
                && g_n_fpb < FPB_MAX
                && last >= i
                && (full | s0 | d0) != 0
            {
                let fb = fpb_at(g_n_fpb);
                (*fb).first = i;
                (*fb).last = last;
                (*fb).ckpt = ckpt;
                (*fb).ckpt_emit = ckpt;
                (*fb).written = written;
                (*fb).dirty_open = 0;
                (*fb).dblonly = 0;
                (*fb).full = full;
                (*fb).s0 = s0;
                (*fb).d0 = d0;
                (*fb).gain = gain - cost;
                (*fb).site = core::ptr::null_mut();
                (*fb).back = core::ptr::null_mut();
                (*fb).end = last;
                for k in i..=last {
                    *bat.add(k as usize) = g_n_fpb as i8;
                    *(&raw mut g_fpb_member).cast::<u8>().add(k as usize) = 1;
                }
                let mut dbl_only = (s0 == 0) as c_int;
                for r in 0..16 {
                    if dbl_only == 0 {
                        break;
                    }
                    if (full & (1u16 << r)) != 0 && taint_dbl[r] == 0 {
                        dbl_only = 0;
                    }
                }
                if S_NO_FPB_DEFER < 0 {
                    S_NO_FPB_DEFER = getenv_on(c"OCERZ_NO_FPB_DEFER") as c_int;
                }
                if dbl_only != 0 && S_NO_FPB_DEFER == 0 && ffi::g_xlat_mode32 == 0 {
                    let mut vid = [[0u8; 2]; 16];
                    for r in 0..16 {
                        if (full & (1u16 << r)) != 0 {
                            vid[r][0] = (1 + 2 * r) as u8;
                            vid[r][1] = (2 + 2 * r) as u8;
                        } else if (d0 & (1u16 << r)) != 0 {
                            vid[r][0] = (1 + 2 * r) as u8;
                        }
                    }
                    let mut rck = ckpt_raw;
                    let mut rwr = written;
                    let mut jj = last + 1;
                    let mut cnt = 0;
                    let mut left = 1;
                    let mut det_jcc = -1;
                    while jj < n && cnt < 24 && planned_sites < FPB_SITES_MAX - 2 {
                        let in_ = &*insns.add(jj as usize);
                        let rk = fpb_region_class(in_);
                        let mut m: u16 = 0;
                        if rk == RK_END {
                            if !(in_.op as u32 == ffi::OCERZ_OP_JCC && jj == det_jcc) {
                                break;
                            }
                            for r in 0..16 {
                                if vid[r][0] != 0 || vid[r][1] != 0 {
                                    m |= 1u16 << r;
                                }
                            }
                            *(&raw mut g_fpb_sidechk).cast::<u16>().add(jj as usize) = m;
                            if m != 0 {
                                planned_sites += 1;
                            }
                            det_jcc = -1;
                            *mrd(jj as usize) = 0;
                            *mwr(jj as usize) = 0;
                            jj += 1;
                            cnt += 1;
                            continue;
                        }
                        let dr = if in_.ops[0].kind as u32 == ffi::OCERZ_OPK_XMM {
                            in_.ops[0].reg as u32
                        } else {
                            16
                        };
                        let sr2 = if in_.ops[1].kind as u32 == ffi::OCERZ_OPK_XMM {
                            in_.ops[1].reg as u32
                        } else {
                            16
                        };
                        let mut reads: u16 = 0;
                        let mut writes: u16 = 0;
                        if rk == RK_STORE {
                            let mut fa = -1;
                            let mut conflict = 0;
                            let mut shift = 0;
                            for k in (*fb).first..=last {
                                if *marith(k as usize) != 0 {
                                    fa = k;
                                    break;
                                }
                            }
                            for k in (*fb).first..=last {
                                if *mmem(k as usize) != 0
                                    && mem_may_alias(
                                        insns.add(k as usize),
                                        &(*insns.add(k as usize)).ops[1],
                                        in_,
                                        &in_.ops[0],
                                    ) != 0
                                {
                                    if *marith(k as usize) != 0 || k >= fa {
                                        conflict = 1;
                                    } else {
                                        shift = 1;
                                    }
                                }
                            }
                            if conflict != 0 || fa < 0 {
                                break;
                            }
                            if shift != 0 {
                                (*fb).first = fa;
                                rck = 0;
                                rwr = 0;
                                for k in fa..jj {
                                    rck |= *mrd(k as usize) & !rwr;
                                    rwr |= *mwr(k as usize);
                                }
                            }
                        }
                        if rk == RK_DET {
                            let mut k = jj + 1;
                            while k < n {
                                let rk2 = fpb_region_class(insns.add(k as usize));
                                if rk2 == RK_END || rk2 == RK_DET {
                                    break;
                                }
                                k += 1;
                            }
                            let ik = &*insns.add(k as usize);
                            let i0 = &*insns;
                            let fused = k < n - 1
                                && ik.op as u32 == ffi::OCERZ_OP_JCC
                                && ik.ops[0].kind as u32 == ffi::OCERZ_OPK_IMM
                                && ik.ops[0].imm > ik.rip
                                && ik.ops[0].imm != i0.rip
                                && ffi::comis_fuse_producer(insns, k) == jj;
                            let ida = vid[dr as usize][0];
                            let idb = vid[sr2 as usize][0];
                            for r in 0..16 {
                                for l in 0..2 {
                                    if vid[r][l] != 0 && (vid[r][l] == ida || vid[r][l] == idb) {
                                        vid[r][l] = 0;
                                    }
                                }
                            }
                            *(&raw mut g_fpb_det).cast::<u8>().add(jj as usize) =
                                if fused { 1 } else { 2 };
                            planned_sites += 1;
                            det_jcc = if fused { k } else { -1 };
                            reads = ((1u32 << dr) | (1u32 << sr2)) as u16;
                        } else {
                            match rk {
                                RK_STORE => reads = (1u32 << sr2) as u16,
                                RK_LOAD => {
                                    writes = (1u32 << dr) as u16;
                                    vid[dr as usize][0] = 0;
                                    vid[dr as usize][1] = 0;
                                }
                                RK_MOVE => {
                                    reads = (1u32 << sr2) as u16;
                                    writes = (1u32 << dr) as u16;
                                    vid[dr as usize][0] = vid[sr2 as usize][0];
                                    vid[dr as usize][1] = vid[sr2 as usize][1];
                                }
                                RK_LMOVE => {
                                    reads = ((1u32 << sr2) | (1u32 << dr)) as u16;
                                    writes = (1u32 << dr) as u16;
                                    vid[dr as usize][0] = vid[sr2 as usize][0];
                                }
                                RK_UNPCKH => {
                                    reads = 1u16 << dr;
                                    writes = 1u16 << dr;
                                    vid[dr as usize][0] = vid[dr as usize][1];
                                }
                                RK_UNPCKL => {
                                    reads = 1u16 << dr;
                                    writes = 1u16 << dr;
                                    vid[dr as usize][1] = vid[dr as usize][0];
                                }
                                _ => {}
                            }
                        }
                        rck |= reads & !rwr;
                        rwr |= writes;
                        *mrd(jj as usize) = reads;
                        *mwr(jj as usize) = writes;
                        jj += 1;
                        cnt += 1;
                        for r in 0..16 {
                            if vid[r][0] != 0 || vid[r][1] != 0 {
                                m |= 1u16 << r;
                            }
                        }
                        if m == 0 && det_jcc < 0 {
                            left = 0;
                            break;
                        }
                        if m == 0 && rk != RK_DET {
                            left = 0;
                            break;
                        }
                    }
                    if jj - 1 > last {
                        (*fb).end = jj - 1;
                        (*fb).ckpt = rck & rwr;
                        (*fb).written = rwr;
                        let mut m: u16 = 0;
                        for r in 0..16 {
                            if vid[r][0] != 0 || vid[r][1] != 0 {
                                m |= 1u16 << r;
                            }
                        }
                        (*fb).full = if left != 0 { m } else { 0 };
                        (*fb).s0 = 0;
                        (*fb).d0 = 0;
                        for k in last + 1..=(*fb).end {
                            *bat.add(k as usize) = g_n_fpb as i8;
                        }
                        for k in i..(*fb).first {
                            *bat.add(k as usize) = -1;
                            *(&raw mut g_fpb_member).cast::<u8>().add(k as usize) = 0;
                        }
                    }
                }
                g_n_fpb += 1;
                if (*fb).end > last {
                    j = (*fb).end + 1;
                }
            }
            i = j;
        }
    }
}

pub(crate) unsafe fn fpb2_kind(
    in_: *const ffi::X86Insn,
    packed: *mut c_int,
    dbl: *mut c_int,
    from_mem: *mut c_int,
    sq: *mut c_int,
    lane_only: *mut c_int,
) -> c_int {
    unsafe {
        let in_ = &*in_;
        *lane_only = 0;
        let c = fpb_class(in_, packed, dbl, from_mem, sq);
        if c == 1 {
            return K2_ARITH;
        }
        if c == 2 {
            return K2_MOVE;
        }
        if c == 3 {
            return K2_LMOVE;
        }
        if in_.seg as u32 != ffi::OCERZ_SEG_NONE || in_.nops < 2 {
            return K2_END;
        }
        let d = &in_.ops[0];
        let s = &in_.ops[1];
        let dx = d.kind as u32 == ffi::OCERZ_OPK_XMM && xmm_is_pinned(d.reg as c_uint) != 0;
        let sx = s.kind as u32 == ffi::OCERZ_OPK_XMM && xmm_is_pinned(s.reg as c_uint) != 0;
        let dm = d.kind as u32 == ffi::OCERZ_OPK_MEM && in_.addrsize == 8;
        let sm = s.kind as u32 == ffi::OCERZ_OPK_MEM && (s.riprel != 0 || in_.addrsize == 8);
        if in_.op as u32 == ffi::OCERZ_OP_SHUFPD
            && in_.nops == 3
            && in_.ops[2].kind as u32 == ffi::OCERZ_OPK_IMM
            && dx
            && (sx || sm)
            && (in_.vex & ffi::OCERZ_VEX_L as u8) == 0
            && in_.mode32 == 0
            && ((in_.vex & ffi::OCERZ_VEX_NDS as u8) == 0
                || xmm_is_pinned((in_.vvvv & 15) as c_uint) != 0)
        {
            *dbl = 1;
            *from_mem = sm as c_int;
            return K2_SHUF;
        }
        if in_.vex != 0 {
            if (in_.vex & ffi::OCERZ_VEX_L as u8) != 0 || in_.mode32 != 0 || in_.nops != 2 {
                return K2_END;
            }
            let nds = (in_.vex & ffi::OCERZ_VEX_NDS as u8) != 0;
            return match in_.op as u32 {
                ffi::OCERZ_OP_MOVDDUP => {
                    *dbl = 1;
                    *from_mem = sm as c_int;
                    if !nds && dx && (sx || sm) {
                        K2_DUP
                    } else {
                        K2_END
                    }
                }
                ffi::OCERZ_OP_MOVUPS
                | ffi::OCERZ_OP_MOVAPS
                | ffi::OCERZ_OP_MOVDQA
                | ffi::OCERZ_OP_MOVDQU => {
                    if !nds && dm && sx {
                        K2_STORE
                    } else {
                        K2_END
                    }
                }
                ffi::OCERZ_OP_MOVSDX => {
                    *dbl = 1;
                    *lane_only = 1;
                    if !nds && dm && sx { K2_STORE } else { K2_END }
                }
                ffi::OCERZ_OP_MOVSS => {
                    *lane_only = 1;
                    if !nds && dm && sx { K2_STORE } else { K2_END }
                }
                ffi::OCERZ_OP_XORPS | ffi::OCERZ_OP_PXOR => {
                    if nds && dx && sx && d.reg == s.reg && (in_.vvvv & 15) == d.reg {
                        K2_ZERO
                    } else {
                        K2_END
                    }
                }
                ffi::OCERZ_OP_UCOMISD
                | ffi::OCERZ_OP_COMISD
                | ffi::OCERZ_OP_UCOMISS
                | ffi::OCERZ_OP_COMISS => {
                    if !nds && dx && sx {
                        K2_DET
                    } else {
                        K2_END
                    }
                }
                _ => K2_END,
            };
        }
        match in_.op as u32 {
            ffi::OCERZ_OP_MOVUPS
            | ffi::OCERZ_OP_MOVAPS
            | ffi::OCERZ_OP_MOVDQA
            | ffi::OCERZ_OP_MOVDQU => {
                if dm && sx {
                    K2_STORE
                } else {
                    K2_END
                }
            }
            ffi::OCERZ_OP_MOVSDX => {
                *dbl = 1;
                *lane_only = 1;
                if dm && sx { K2_STORE } else { K2_END }
            }
            ffi::OCERZ_OP_MOVSS => {
                *lane_only = 1;
                if dm && sx { K2_STORE } else { K2_END }
            }
            ffi::OCERZ_OP_MOVLPS => {
                *dbl = 1;
                *lane_only = 1;
                if dm && sx { K2_STORE } else { K2_END }
            }
            ffi::OCERZ_OP_MOVHPS => {
                if dm && sx {
                    K2_STORE
                } else {
                    K2_END
                }
            }
            ffi::OCERZ_OP_XORPS | ffi::OCERZ_OP_PXOR => {
                if dx && sx && d.reg == s.reg {
                    K2_ZERO
                } else {
                    K2_END
                }
            }
            ffi::OCERZ_OP_UNPCKHPD => {
                if dx && sx {
                    K2_UNPCKH
                } else {
                    K2_END
                }
            }
            ffi::OCERZ_OP_UNPCKLPD => {
                if dx && sx {
                    K2_UNPCKL
                } else {
                    K2_END
                }
            }
            ffi::OCERZ_OP_MOVDDUP => {
                *dbl = 1;
                *from_mem = sm as c_int;
                if dx && (sx || sm) { K2_DUP } else { K2_END }
            }
            ffi::OCERZ_OP_UCOMISD
            | ffi::OCERZ_OP_COMISD
            | ffi::OCERZ_OP_UCOMISS
            | ffi::OCERZ_OP_COMISS => {
                if dx && sx {
                    K2_DET
                } else {
                    K2_END
                }
            }
            _ => K2_END,
        }
    }
}

pub(crate) unsafe fn fpb2_usedef(in_: *const ffi::X86Insn, use_: *mut u16, kill: *mut u16) {
    unsafe {
        let in_ = &*in_;
        let mut u: u16 = 0;
        let mut k: u16 = 0;
        for q in 0..in_.nops as usize {
            if in_.ops[q].kind as u32 == ffi::OCERZ_OPK_XMM && in_.ops[q].reg < 16 {
                u |= 1u16 << in_.ops[q].reg;
            }
        }
        let d = &in_.ops[0];
        let s = if in_.nops > 1 {
            &in_.ops[1]
        } else {
            &in_.ops[0]
        };
        let has_s = in_.nops > 1;
        let dx = in_.nops > 0 && d.kind as u32 == ffi::OCERZ_OPK_XMM && d.reg < 16;
        if in_.vex != 0 {
            let nds = (in_.vex & ffi::OCERZ_VEX_NDS as u8) != 0;
            if nds {
                u |= 1u16 << (in_.vvvv & 15);
            }
            let mut lk = 0;
            if (in_.vex & ffi::OCERZ_VEX_L as u8) == 0 && in_.mode32 == 0 && in_.nops == 2 && dx {
                match in_.op as u32 {
                    ffi::OCERZ_OP_MOVUPS
                    | ffi::OCERZ_OP_MOVAPS
                    | ffi::OCERZ_OP_MOVDQA
                    | ffi::OCERZ_OP_MOVDQU => lk = (!nds) as c_int,
                    ffi::OCERZ_OP_MOVSS | ffi::OCERZ_OP_MOVSDX => {
                        lk = (!nds && has_s && s.kind as u32 == ffi::OCERZ_OPK_MEM) as c_int
                    }
                    ffi::OCERZ_OP_XORPS | ffi::OCERZ_OP_PXOR => {
                        lk = (nds
                            && has_s
                            && s.kind as u32 == ffi::OCERZ_OPK_XMM
                            && s.reg == d.reg
                            && (in_.vvvv & 15) == d.reg) as c_int
                    }
                    _ => {}
                }
            }
            if lk != 0 {
                k = 1u16 << d.reg;
                u &= !k;
                if in_.op as u32 == ffi::OCERZ_OP_XORPS || in_.op as u32 == ffi::OCERZ_OP_PXOR {
                    u = 0;
                }
            }
            *use_ = u;
            *kill = k;
            return;
        }
        match in_.op as u32 {
            ffi::OCERZ_OP_MOVUPS
            | ffi::OCERZ_OP_MOVAPS
            | ffi::OCERZ_OP_MOVDQA
            | ffi::OCERZ_OP_MOVDQU
            | ffi::OCERZ_OP_MOVDDUP => {
                if dx {
                    k = 1u16 << d.reg;
                    u &= !k;
                }
            }
            ffi::OCERZ_OP_MOVSS | ffi::OCERZ_OP_MOVSDX => {
                if dx && has_s && s.kind as u32 == ffi::OCERZ_OPK_MEM {
                    k = 1u16 << d.reg;
                    u &= !k;
                }
            }
            ffi::OCERZ_OP_XORPS | ffi::OCERZ_OP_PXOR => {
                if dx && has_s && s.kind as u32 == ffi::OCERZ_OPK_XMM && s.reg == d.reg {
                    k = 1u16 << d.reg;
                    u = 0;
                }
            }
            _ => {}
        }
        *use_ = u;
        *kill = k;
    }
}

unsafe fn fpb2_gpr_after_ok(in_: *const ffi::X86Insn, gprs: u16) -> c_int {
    unsafe {
        let in_ = &*in_;
        match in_.op as u32 {
            ffi::OCERZ_OP_JCC
            | ffi::OCERZ_OP_JMP
            | ffi::OCERZ_OP_NOP
            | ffi::OCERZ_OP_CMP
            | ffi::OCERZ_OP_TEST => {
                (in_.nops == 0
                    || in_.ops[0].kind as u32 != ffi::OCERZ_OPK_MEM
                    || in_.op as u32 == ffi::OCERZ_OP_CMP
                    || in_.op as u32 == ffi::OCERZ_OP_TEST) as c_int
            }
            ffi::OCERZ_OP_ADD
            | ffi::OCERZ_OP_SUB
            | ffi::OCERZ_OP_INC
            | ffi::OCERZ_OP_DEC
            | ffi::OCERZ_OP_AND
            | ffi::OCERZ_OP_OR
            | ffi::OCERZ_OP_XOR
            | ffi::OCERZ_OP_LEA
            | ffi::OCERZ_OP_MOV
            | ffi::OCERZ_OP_SHL
            | ffi::OCERZ_OP_SHR
            | ffi::OCERZ_OP_SAR
            | ffi::OCERZ_OP_NEG
            | ffi::OCERZ_OP_NOT
            | ffi::OCERZ_OP_MOVZX
            | ffi::OCERZ_OP_MOVSX
            | ffi::OCERZ_OP_CMOVCC
            | ffi::OCERZ_OP_SETCC => {
                if in_.nops < 1 || in_.ops[0].kind as u32 != ffi::OCERZ_OPK_REG {
                    return 0;
                }
                (gprs & (1u16 << (in_.ops[0].reg & 15)) == 0) as c_int
            }
            _ => 0,
        }
    }
}

static mut g_fpb_live: [u16; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];

#[unsafe(no_mangle)]
pub static mut g_fpb_stchk: [u16; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];

static mut g_fpb_stdbl: [u8; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];

#[unsafe(no_mangle)]
pub static mut g_fpb_stlane: [u8; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];

#[unsafe(no_mangle)]
pub static mut g_fpb_undo: [u8; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];

#[unsafe(no_mangle)]
pub static mut g_fpb_undo_done: [u8; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];

#[unsafe(no_mangle)]
pub static mut g_fpb_undo_size: [u8; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];

static mut g_fpb_mstore: [u8; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];

#[unsafe(no_mangle)]
pub static mut g_fpb_undo_ld: [u8; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];

#[unsafe(no_mangle)]
pub static mut g_fpb_undo_ldsz: [u8; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];

#[unsafe(no_mangle)]
pub static mut g_fpb_undo_from: [i16; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];

#[unsafe(no_mangle)]
pub static mut g_fpb_undo_ldst: [i16; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];

#[inline(always)]
unsafe fn live(i: usize) -> *mut u16 {
    (&raw mut g_fpb_live).cast::<u16>().add(i)
}
#[inline(always)]
unsafe fn stchk(i: usize) -> *mut u16 {
    (&raw mut g_fpb_stchk).cast::<u16>().add(i)
}
#[inline(always)]
unsafe fn stlane(i: usize) -> *mut u8 {
    (&raw mut g_fpb_stlane).cast::<u8>().add(i)
}
#[inline(always)]
unsafe fn stdbl(i: usize) -> *mut u8 {
    (&raw mut g_fpb_stdbl).cast::<u8>().add(i)
}
#[inline(always)]
unsafe fn undo(i: usize) -> *mut u8 {
    (&raw mut g_fpb_undo).cast::<u8>().add(i)
}
#[inline(always)]
unsafe fn undo_size(i: usize) -> *mut u8 {
    (&raw mut g_fpb_undo_size).cast::<u8>().add(i)
}
#[inline(always)]
unsafe fn undo_ld(i: usize) -> *mut u8 {
    (&raw mut g_fpb_undo_ld).cast::<u8>().add(i)
}
#[inline(always)]
unsafe fn undo_ldsz(i: usize) -> *mut u8 {
    (&raw mut g_fpb_undo_ldsz).cast::<u8>().add(i)
}
#[inline(always)]
unsafe fn undo_from(i: usize) -> *mut i16 {
    (&raw mut g_fpb_undo_from).cast::<i16>().add(i)
}
#[inline(always)]
unsafe fn undo_ldst(i: usize) -> *mut i16 {
    (&raw mut g_fpb_undo_ldst).cast::<i16>().add(i)
}
#[inline(always)]
unsafe fn sidechk(i: usize) -> *mut u16 {
    (&raw mut g_fpb_sidechk).cast::<u16>().add(i)
}
#[inline(always)]
unsafe fn det(i: usize) -> *mut u8 {
    (&raw mut g_fpb_det).cast::<u8>().add(i)
}
#[inline(always)]
unsafe fn member(i: usize) -> *mut u8 {
    (&raw mut g_fpb_member).cast::<u8>().add(i)
}

pub(crate) unsafe fn mem_same(a: *const ffi::X86Operand, b: *const ffi::X86Operand) -> c_int {
    unsafe {
        ((*a).riprel == 0
            && (*b).riprel == 0
            && (*a).base == (*b).base
            && (*a).index == (*b).index
            && (*a).scale == (*b).scale
            && (*a).disp == (*b).disp) as c_int
    }
}

#[unsafe(no_mangle)]
pub static mut g_fpb_exit_mask: u16 = 0;

#[unsafe(no_mangle)]
pub static mut g_fpb_exit_batch: c_int = -1;

#[unsafe(no_mangle)]
pub static mut g_fpb_exit_end: c_int = -1;

#[inline(always)]
unsafe fn fpb2_membits(m: *const ffi::X86Operand) -> u16 {
    unsafe {
        let mut g: u16 = 0;
        if (*m).base as u32 != ffi::OCERZ_REG_NONE {
            g |= 1u16 << ((*m).base & 15);
        }
        if (*m).index as u32 != ffi::OCERZ_REG_NONE {
            g |= 1u16 << ((*m).index & 15);
        }
        g
    }
}

unsafe fn fpb_undo_lane(slot: c_int) -> c_int {
    unsafe {
        while slot >= g_n_undo_lanes {
            if g_n_undo_lanes >= FPB_UNDO_MAX {
                return -1;
            }
            let v = lane_reserve();
            if v < 0 {
                return -1;
            }
            *(&raw mut g_undo_vreg)
                .cast::<i8>()
                .add(g_n_undo_lanes as usize) = v as i8;
            g_n_undo_lanes += 1;
        }
        *(&raw mut g_undo_vreg).cast::<i8>().add(slot as usize) as c_int
    }
}

static mut S_FPB_NOUNDO: c_int = -1;

unsafe fn fpb2_undo_ok(in_: *const ffi::X86Insn, m: *const ffi::X86Operand, size: c_int) -> c_int {
    unsafe {
        if S_FPB_NOUNDO < 0 {
            S_FPB_NOUNDO = getenv_on(c"OCERZ_FPB_NOUNDO") as c_int;
        }
        if S_FPB_NOUNDO != 0 || ffi::vec_tso_relaxed() == 0 || mem_fast_forms_ok() == 0 {
            return 0;
        }
        if (*in_).seg as u32 != ffi::OCERZ_SEG_NONE || (*in_).addrsize != 8 || (*m).riprel != 0 {
            return 0;
        }
        if (*m).base as u32 == ffi::OCERZ_REG_NONE || pin_slot((*m).base as c_uint) < 0 {
            return 0;
        }
        if (*m).index as u32 != ffi::OCERZ_REG_NONE && pin_slot((*m).index as c_uint) < 0 {
            return 0;
        }
        if rsp_is_ptr() != 0
            && ((*m).base as u32 == ffi::OCERZ_RSP || (*m).index as u32 == ffi::OCERZ_RSP)
        {
            return 0;
        }
        let disp = (*m).disp;
        ((disp >= 0 && disp % size as i64 == 0 && disp / size as i64 <= 4095)
            || (disp >= -256 && disp <= 255)) as c_int
    }
}

static mut S_DBG_RIP: c_ulong = -1i64 as c_ulong;
static mut S_DBG_MAX: c_ulong = -1i64 as c_ulong;
static mut S_DBG_LO: c_ulong = 0;
static mut S_DBG_HI: c_ulong = 0;
static mut S_DBG_INIT: c_int = 0;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn fpb_scan_v2(insns: *const ffi::X86Insn, n: c_int, bat: *mut i8) {
    unsafe {
        g_n_fpb = 0;
        g_n_fpb_sites = 0;
        g_fpb_exit_mask = 0;
        g_fpb_exit_batch = -1;
        g_fpb_exit_end = -1;
        for i in 0..n as usize {
            *bat.add(i) = -1;
            *member(i) = 0;
            *det(i) = 0;
            *sidechk(i) = 0;
            *stchk(i) = 0;
            *stlane(i) = 0;
            *stdbl(i) = 0;
            *live(i) = 0xffff;
            fpb_undo_clear(i as c_int);
        }
        if g_fpb_disabled < 0 {
            g_fpb_disabled = getenv_on(c"OCERZ_NO_FPBATCH") as c_int;
        }
        if g_fpb_disabled != 0
            || ffi::sse_enabled() == 0
            || ffi::xmm_global_enabled() == 0
            || n <= 0
        {
            return;
        }
        let ilast = &*insns.add(n as usize - 1);
        let i0 = &*insns;
        let selfloop = (ilast.op as u32 == ffi::OCERZ_OP_JCC
            || ilast.op as u32 == ffi::OCERZ_OP_JMP)
            && ilast.nops == 1
            && ilast.ops[0].kind as u32 == ffi::OCERZ_OPK_IMM
            && ilast.ops[0].imm == i0.rip;
        static mut S_USE: [u16; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];
        static mut S_KILL: [u16; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];
        static mut S_WR: [u16; JIT_MAX_BLOCK_INSNS] = [0; JIT_MAX_BLOCK_INSNS];
        let use_ = &raw mut S_USE;
        let kill = &raw mut S_KILL;
        let wr = &raw mut S_WR;
        for i in 0..n as usize {
            fpb2_usedef(
                insns.add(i),
                (*use_).as_mut_ptr().add(i),
                (*kill).as_mut_ptr().add(i),
            );
            *(*wr).as_mut_ptr().add(i) = if (*insns.add(i)).nops > 0
                && (*insns.add(i)).ops[0].kind as u32 == ffi::OCERZ_OPK_XMM
                && (*insns.add(i)).ops[0].reg < 16
            {
                1u16 << (*insns.add(i)).ops[0].reg
            } else {
                0
            };
            if (*insns.add(i)).op as u32 == ffi::OCERZ_OP_JCC && !(selfloop && i == n as usize - 1)
            {
                *(*use_).as_mut_ptr().add(i) = 0xffff;
            }
        }
        let mut live_in0: u16 = 0;
        for _ in 0..4 {
            let mut cur: u16 = if selfloop { live_in0 } else { 0xffff };
            for i in (0..n as usize).rev() {
                *live(i) = cur;
                cur = *(*use_).as_ptr().add(i) | (cur & !*(*kill).as_ptr().add(i));
            }
            if !selfloop || cur == live_in0 {
                break;
            }
            live_in0 = cur;
        }
        let mut planned_sites = 0;
        let mut i = 0;
        while i < n {
            let mut packed = 0;
            let mut dbl = 0;
            let mut from_mem = 0;
            let mut sq = 0;
            let mut lane_only = 0;
            let k0 = fpb2_kind(
                insns.add(i as usize),
                &mut packed,
                &mut dbl,
                &mut from_mem,
                &mut sq,
                &mut lane_only,
            );
            if k0 != K2_ARITH && k0 != K2_MOVE && k0 != K2_LMOVE {
                i += 1;
                continue;
            }
            let mut j = i;
            let mut written: u16 = 0;
            let mut ckpt: u16 = 0;
            let mut full: u16 = 0;
            let mut s0: u16 = 0;
            let mut d0: u16 = 0;
            let mut gprs: u16 = 0;
            let mut everf: u16 = 0;
            let mut gain = 0;
            let mut n_arith = 0;
            let mut cost_extra = 0;
            let mut sites = 0;
            let mut edges = [FpbEdge::default(); 64];
            let mut n_edges = 0;
            let mut lastw = [-1; 16];
            let mut lastbreak = [-1; 16];
            let mut taint_dbl = [0u8; 16];
            let mut loads = [0; 64];
            let mut loadsz = [0; 64];
            let mut nload = 0;
            let mut stores = [0; 64];
            let mut nstore = 0;
            let mut nundo = 0;
            let mut st_at = [0; 16];
            let mut st_cls = [0; 16];
            let mut nst = 0;
            let mut det_j = -1;
            let mut n_fma = 0;
            while j < n {
                let in_ = &*insns.add(j as usize);
                let k = fpb2_kind(
                    in_,
                    &mut packed,
                    &mut dbl,
                    &mut from_mem,
                    &mut sq,
                    &mut lane_only,
                );
                {
                    if S_DBG_INIT == 0 {
                        S_DBG_INIT = 1;
                        let e = libc::getenv(c"OCERZ_FPB_DBGRIP".as_ptr());
                        S_DBG_RIP = if !e.is_null() {
                            libc::strtoull(e, core::ptr::null_mut(), 0)
                        } else {
                            0
                        };
                        let e = libc::getenv(c"OCERZ_FPB_DBGMAX".as_ptr());
                        S_DBG_MAX = if !e.is_null() {
                            libc::strtol(e, core::ptr::null_mut(), 0) as c_ulong
                        } else {
                            100000
                        };
                        let e = libc::getenv(c"OCERZ_FPB_DBGLO".as_ptr());
                        S_DBG_LO = if !e.is_null() {
                            libc::strtoull(e, core::ptr::null_mut(), 0)
                        } else {
                            0
                        };
                        let e = libc::getenv(c"OCERZ_FPB_DBGHI".as_ptr());
                        S_DBG_HI = if !e.is_null() {
                            libc::strtoull(e, core::ptr::null_mut(), 0)
                        } else {
                            0
                        };
                    }
                    if S_DBG_HI != 0 && i0.rip >= S_DBG_LO && i0.rip <= S_DBG_HI {
                        let lim: i64 = if S_DBG_RIP != 0 && S_DBG_RIP == i0.rip {
                            S_DBG_MAX as i64
                        } else {
                            -1
                        };
                        if (j as i64) > lim {
                            break;
                        }
                    }
                }
                if det_j >= 0 {
                    if in_.op as u32 == ffi::OCERZ_OP_JCC
                        && j < n - 1
                        && in_.ops[0].kind as u32 == ffi::OCERZ_OPK_IMM
                        && in_.ops[0].imm != i0.rip
                        && ffi::comis_fuse_producer(insns, j) == det_j
                    {
                        *det(det_j as usize) = 1;
                        *sidechk(j as usize) = full | s0 | d0;
                        *mrd(j as usize) = 0;
                        *mwr(j as usize) = 0;
                        *mmem(j as usize) = 0;
                        *marith(j as usize) = 0;
                        sites += 2;
                        det_j = -1;
                        j += 1;
                        continue;
                    }
                    j = det_j;
                    break;
                }
                if k == K2_END {
                    break;
                }
                if planned_sites + sites + 4 >= FPB_SITES_MAX {
                    break;
                }
                let d = &in_.ops[0];
                let sr = &in_.ops[1];
                let dr: c_uint = if d.kind as u32 == ffi::OCERZ_OPK_XMM {
                    d.reg as c_uint
                } else {
                    16
                };
                let srr: c_uint = if sr.kind as u32 == ffi::OCERZ_OPK_XMM {
                    sr.reg as c_uint
                } else {
                    16
                };
                let sbit: u16 = if srr < 16 { 1u16 << srr } else { 0 };
                let dbit: u16 = if dr < 16 { 1u16 << dr } else { 0 };
                let mut reads: u16 = 0;
                let mut writes: u16 = 0;
                let mut is_mem = 0;
                if k == K2_STORE {
                    let mut conflict = 0;
                    for q in 0..nload {
                        let lq = loads[q];
                        if mem_may_alias(
                            insns.add(lq as usize),
                            &(*insns.add(lq as usize)).ops[1],
                            in_,
                            &in_.ops[0],
                        ) != 0
                        {
                            conflict = 1;
                        }
                    }
                    let usz = if lane_only != 0 {
                        if dbl != 0 { 8 } else { 4 }
                    } else if in_.op as u32 == ffi::OCERZ_OP_MOVHPS {
                        8
                    } else {
                        16
                    };
                    if conflict != 0 {
                        if nundo >= FPB_UNDO_MAX
                            || nstore >= 64
                            || fpb2_undo_ok(in_, d, usz) == 0
                            || fpb_undo_lane(nundo) < 0
                        {
                            break;
                        }
                        nundo += 1;
                        *undo(j as usize) = nundo as u8;
                        *undo_size(j as usize) = usz as u8;
                        let mut reuse = -1;
                        let mut q = nload as i32 - 1;
                        while q >= 0 && reuse < 0 {
                            let l = loads[q as usize];
                            let il = &*insns.add(l as usize);
                            if loadsz[q as usize] < usz
                                || il.seg as u32 != ffi::OCERZ_SEG_NONE
                                || il.addrsize != 8
                                || *undo_ld(l as usize) != 0
                            {
                                q -= 1;
                                continue;
                            }
                            if mem_same(&il.ops[1], d) == 0 {
                                q -= 1;
                                continue;
                            }
                            let mut ok = 1;
                            for t in 0..nstore {
                                let st = stores[t];
                                if ok != 0
                                    && st > l
                                    && mem_may_alias(
                                        insns.add(l as usize),
                                        &il.ops[1],
                                        insns.add(st as usize),
                                        &(*insns.add(st as usize)).ops[0],
                                    ) != 0
                                {
                                    ok = 0;
                                }
                            }
                            if ok != 0 {
                                reuse = q;
                            }
                            q -= 1;
                        }
                        if reuse >= 0 {
                            let l = loads[reuse as usize];
                            *undo_ld(l as usize) = nundo as u8;
                            *undo_ldsz(l as usize) = loadsz[reuse as usize] as u8;
                            *undo_ldst(l as usize) = j as i16;
                            *undo_from(j as usize) = l as i16;
                            cost_extra += 1;
                        } else {
                            cost_extra += 2;
                        }
                    }
                    if nstore < 64 {
                        stores[nstore] = j;
                        nstore += 1;
                    }
                    gprs |= fpb2_membits(d);
                    reads = sbit;
                    let t = (full | s0 | d0) & sbit;
                    if t != 0 {
                        if nst < 16 {
                            st_at[nst] = j;
                            st_cls[nst] = if lane_only != 0 {
                                if dbl != 0 { 3 } else { 2 }
                            } else if (full & sbit) != 0 {
                                1
                            } else if (d0 & sbit) != 0 {
                                3
                            } else {
                                2
                            };
                            nst += 1;
                        }
                        *stchk(j as usize) = sbit;
                        if lane_only != 0 {
                            *stlane(j as usize) = 1;
                            *stdbl(j as usize) = dbl as u8;
                            cost_extra += 2;
                        } else {
                            cost_extra += 3;
                            full &= !sbit;
                        }
                        d0 &= !sbit;
                        s0 &= !sbit;
                        sites += 1;
                    }
                } else if k == K2_DET {
                    if j + 1 >= n {
                        break;
                    }
                    reads = sbit | dbit;
                    det_j = j;
                } else {
                    if sr.kind as u32 == ffi::OCERZ_OPK_MEM {
                        if nload >= 64 {
                            break;
                        }
                        loadsz[nload] = if k == K2_MOVE || k == K2_SHUF {
                            16
                        } else if k == K2_LMOVE {
                            if dbl != 0 { 8 } else { 4 }
                        } else if k == K2_DUP {
                            8
                        } else if k == K2_ARITH {
                            if packed != 0 {
                                16
                            } else if dbl != 0 {
                                8
                            } else {
                                4
                            }
                        } else {
                            0
                        };
                        loads[nload] = j;
                        nload += 1;
                        gprs |= fpb2_membits(sr);
                        is_mem = 1;
                    }
                    match k {
                        K2_ARITH => {
                            let is_mm = in_.op as u32 == ffi::OCERZ_OP_MAXSS
                                || in_.op as u32 == ffi::OCERZ_OP_MINSS
                                || in_.op as u32 == ffi::OCERZ_OP_MAXSD
                                || in_.op as u32 == ffi::OCERZ_OP_MINSD
                                || in_.op as u32 == ffi::OCERZ_OP_MAXPS
                                || in_.op as u32 == ffi::OCERZ_OP_MINPS
                                || in_.op as u32 == ffi::OCERZ_OP_MAXPD
                                || in_.op as u32 == ffi::OCERZ_OP_MINPD;
                            let fma = in_.op as u32 >= ffi::OCERZ_OP_VFMA_FIRST
                                && in_.op as u32 <= ffi::OCERZ_OP_VFMA_LAST;
                            n_fma += fma as c_int;
                            let nds = (in_.vex & ffi::OCERZ_VEX_NDS as u8) != 0 && !fma;
                            let vr = if (in_.vex & ffi::OCERZ_VEX_NDS as u8) != 0 {
                                (in_.vvvv & 15) as c_uint
                            } else {
                                dr
                            };
                            let vbit = 1u16 << vr;
                            let srcs: [c_uint; 2] = [
                                srr,
                                if (in_.vex & ffi::OCERZ_VEX_NDS as u8) != 0 {
                                    vr
                                } else {
                                    16
                                },
                            ];
                            for q in 0..2 {
                                let sx = srcs[q];
                                if sx >= 16 || sx == dr || is_mm || n_edges >= 64 {
                                    continue;
                                }
                                let xb = 1u16 << sx;
                                if packed != 0
                                    && (full & xb) != 0
                                    && taint_dbl[sx as usize] == dbl as u8
                                {
                                    edges[n_edges] = FpbEdge {
                                        s: sx as u8,
                                        d: dr as u8,
                                        cls: 0,
                                        t: j,
                                    };
                                    n_edges += 1;
                                }
                                if dbl == 0 && (s0 & xb) != 0 && n_edges < 64 {
                                    edges[n_edges] = FpbEdge {
                                        s: sx as u8,
                                        d: dr as u8,
                                        cls: 1,
                                        t: j,
                                    };
                                    n_edges += 1;
                                }
                                if dbl != 0 && (d0 & xb) != 0 && n_edges < 64 {
                                    edges[n_edges] = FpbEdge {
                                        s: sx as u8,
                                        d: dr as u8,
                                        cls: 2,
                                        t: j,
                                    };
                                    n_edges += 1;
                                }
                            }
                            taint_dbl[dr as usize] = dbl as u8;
                            if dbl == 0 {
                                everf |= dbit;
                            } else {
                                everf &= !dbit;
                            }
                            if is_mm || (sq != 0 && srr != dr) {
                                lastbreak[dr as usize] = j;
                            }
                            reads = sbit | vbit | if fma { dbit } else { 0 };
                            writes = dbit;
                            if packed != 0 {
                                full |= dbit;
                                s0 &= !dbit;
                                d0 &= !dbit;
                            } else {
                                if nds {
                                    if (full & vbit) != 0 {
                                        full |= dbit;
                                    } else {
                                        full &= !dbit;
                                    }
                                }
                                if dbl != 0 {
                                    d0 |= dbit;
                                    s0 &= !dbit;
                                } else {
                                    s0 |= dbit;
                                    d0 &= !dbit;
                                }
                            }
                            if !is_mm {
                                gain += if packed != 0 { 6 } else { 2 };
                                n_arith += 1;
                            }
                        }
                        K2_MOVE => {
                            reads = sbit;
                            writes = dbit;
                            lastbreak[dr as usize] = j;
                            if from_mem != 0 {
                                full &= !dbit;
                                s0 &= !dbit;
                                d0 &= !dbit;
                                everf &= !dbit;
                            } else {
                                everf =
                                    (everf & !dbit) | if (everf & sbit) != 0 { dbit } else { 0 };
                                full = (full & !dbit) | if (full & sbit) != 0 { dbit } else { 0 };
                                s0 = (s0 & !dbit) | if (s0 & sbit) != 0 { dbit } else { 0 };
                                d0 = (d0 & !dbit) | if (d0 & sbit) != 0 { dbit } else { 0 };
                                taint_dbl[dr as usize] = taint_dbl[srr as usize];
                            }
                        }
                        K2_LMOVE => {
                            reads = sbit | if from_mem != 0 { 0 } else { dbit };
                            writes = dbit;
                            lastbreak[dr as usize] = j;
                            if dbl == 0 || (everf & sbit) != 0 {
                                everf |= dbit;
                            }
                            if from_mem != 0 {
                                full &= !dbit;
                                s0 &= !dbit;
                                d0 &= !dbit;
                                if dbl != 0 {
                                    everf &= !dbit;
                                }
                            } else {
                                if (full & sbit) != 0 {
                                    full |= dbit;
                                }
                                if dbl != 0 {
                                    if ((d0 | full) & sbit) != 0 {
                                        d0 |= dbit;
                                    } else {
                                        d0 &= !dbit;
                                    }
                                    s0 &= !dbit;
                                } else {
                                    if ((s0 | full) & sbit) != 0 {
                                        s0 |= dbit;
                                    } else {
                                        s0 &= !dbit;
                                    }
                                    d0 &= !dbit;
                                }
                                taint_dbl[dr as usize] = dbl as u8;
                            }
                        }
                        K2_ZERO => {
                            writes = dbit;
                            lastbreak[dr as usize] = j;
                            full &= !dbit;
                            s0 &= !dbit;
                            d0 &= !dbit;
                            everf &= !dbit;
                        }
                        K2_UNPCKH => {
                            reads = sbit | dbit;
                            writes = dbit;
                            lastbreak[dr as usize] = j;
                            if (full & sbit) != 0 || (full & dbit) != 0 {
                                full |= dbit;
                                d0 &= !dbit;
                                s0 &= !dbit;
                            } else {
                                full &= !dbit;
                                d0 &= !dbit;
                                s0 &= !dbit;
                            }
                        }
                        K2_UNPCKL => {
                            reads = sbit | dbit;
                            writes = dbit;
                            lastbreak[dr as usize] = j;
                            if ((full | d0 | s0) & sbit) != 0 {
                                full |= dbit;
                                d0 &= !dbit;
                                s0 &= !dbit;
                            }
                        }
                        K2_DUP => {
                            reads = sbit;
                            writes = dbit;
                            lastbreak[dr as usize] = j;
                            everf = (everf & !dbit)
                                | if from_mem == 0 && (everf & sbit) != 0 {
                                    dbit
                                } else {
                                    0
                                };
                            if from_mem == 0 && ((full | d0 | s0) & sbit) != 0 {
                                full |= dbit;
                                d0 &= !dbit;
                                s0 &= !dbit;
                            } else {
                                full &= !dbit;
                                d0 &= !dbit;
                                s0 &= !dbit;
                            }
                        }
                        K2_SHUF => {
                            let vb2: u16 = if (in_.vex & ffi::OCERZ_VEX_NDS as u8) != 0 {
                                1u16 << (in_.vvvv & 15)
                            } else {
                                dbit
                            };
                            reads = sbit | vb2;
                            writes = dbit;
                            lastbreak[dr as usize] = j;
                            let srcs2 = sbit | vb2;
                            everf = (everf & !dbit) | if (everf & srcs2) != 0 { dbit } else { 0 };
                            if ((full | d0 | s0) & srcs2) != 0 {
                                full |= dbit;
                                d0 &= !dbit;
                                s0 &= !dbit;
                            } else {
                                full &= !dbit;
                                d0 &= !dbit;
                                s0 &= !dbit;
                            }
                        }
                        _ => {}
                    }
                    if dr < 16 {
                        lastw[dr as usize] = j;
                    }
                }
                ckpt |= reads & !written;
                written |= writes;
                *mrd(j as usize) = reads;
                *mwr(j as usize) = writes;
                *mmem(j as usize) = is_mem as u8;
                *marith(j as usize) = (k == K2_ARITH) as u8;
                *mstore(j as usize) = (k == K2_STORE) as u8;
                j += 1;
            }
            if det_j >= 0 {
                j = det_j;
            }
            let mut end = j - 1;
            while end > i && *mstore(end as usize) != 0 {
                if *undo(end as usize) != 0 {
                    cost_extra -= if *undo_from(end as usize) >= 0 { 1 } else { 2 };
                    if *undo_from(end as usize) >= 0 {
                        fpb_undo_clear(*undo_from(end as usize) as c_int);
                    }
                    fpb_undo_clear(end);
                    nundo -= 1;
                }
                if *stchk(end as usize) != 0 {
                    let sb = *stchk(end as usize);
                    cost_extra -= if *stlane(end as usize) != 0 { 2 } else { 3 };
                    sites -= 1;
                    *stchk(end as usize) = 0;
                    *stlane(end as usize) = 0;
                    for q in 0..nst {
                        if st_at[q] == end {
                            if st_cls[q] == 1 {
                                full |= sb;
                            } else if st_cls[q] == 3 {
                                d0 |= sb;
                            } else {
                                s0 |= sb;
                            }
                            nst = q;
                            break;
                        }
                    }
                }
                end -= 1;
            }
            if end < i || n_arith < 1 {
                let mut q = i;
                while q <= j - 1 && q < n {
                    *stchk(q as usize) = 0;
                    *stlane(q as usize) = 0;
                    *det(q as usize) = 0;
                    *sidechk(q as usize) = 0;
                    fpb_undo_clear(q);
                    q += 1;
                }
                i = if j > i { j } else { i + 1 };
                continue;
            }
            for q in end + 1..j {
                *stchk(q as usize) = 0;
                *stlane(q as usize) = 0;
                *det(q as usize) = 0;
                *sidechk(q as usize) = 0;
                fpb_undo_clear(q);
            }
            ckpt &= written;
            let tainted = full | s0 | d0;
            let l = *live(end as usize);
            let mut wr_after: u16 = 0;
            let mut exit_ok = selfloop as c_int;
            for m in end + 1..n {
                wr_after |= *(*wr).as_ptr().add(m as usize);
                if exit_ok != 0 && fpb2_gpr_after_ok(insns.add(m as usize), gprs) == 0 {
                    exit_ok = 0;
                }
            }
            let mut t = tainted & l;
            let rest = tainted & !t & !wr_after;
            let mut exitchk: u16 = 0;
            if exit_ok != 0 {
                exitchk = rest;
            } else {
                t |= rest;
            }
            let mut tf = full & t;
            let mut ts = s0 & t;
            let mut td = d0 & t;
            for q in 0..nst {
                if q >= nst || st_at[q] > end {
                    break;
                }
                let sj = st_at[q];
                let sb = *stchk(sj as usize);
                let mut later = 0;
                for m in sj + 1..=end {
                    if (*mwr(m as usize) & sb) != 0 {
                        later = 1;
                        break;
                    }
                }
                if later != 0 {
                    continue;
                }
                if sj == end && (t | exitchk) == 0 {
                    if *undo(sj as usize) != 0 {
                        cost_extra -= if *undo_from(sj as usize) >= 0 { 1 } else { 2 };
                        if *undo_from(sj as usize) >= 0 {
                            fpb_undo_clear(*undo_from(sj as usize) as c_int);
                        }
                        fpb_undo_clear(sj);
                        nundo -= 1;
                    }
                    continue;
                }
                cost_extra -= if *stlane(sj as usize) != 0 { 2 } else { 3 };
                sites -= 1;
                *stchk(sj as usize) = 0;
                *stlane(sj as usize) = 0;
                if st_cls[q] == 1 {
                    tf |= sb;
                } else if st_cls[q] == 3 {
                    td |= sb;
                } else {
                    ts |= sb;
                }
                t |= sb;
            }
            if ffi::ocerz_afp() != 0 && n_fma == 0 {
                for q in i..=end {
                    *stchk(q as usize) = 0;
                    *stlane(q as usize) = 0;
                    *det(q as usize) = 0;
                    *sidechk(q as usize) = 0;
                    fpb_undo_clear(q);
                }
                t = 0;
                tf = 0;
                ts = 0;
                td = 0;
                exitchk = 0;
                ckpt = 0;
                sites = 0;
                cost_extra = 0;
                nundo = 0;
            }
            let mut u = t | exitchk;
            let mut changed = 1;
            while changed != 0 {
                changed = 0;
                for e in 0..n_edges {
                    let s = edges[e].s as usize;
                    let dd = edges[e].d as usize;
                    let tt = edges[e].t;
                    if s == dd || tt > end {
                        continue;
                    }
                    if lastbreak[dd] > tt || lastw[s] > tt {
                        continue;
                    }
                    if (u & (1u16 << dd)) == 0 {
                        continue;
                    }
                    if edges[e].cls == 0 {
                        if (tf & (1u16 << s)) != 0 {
                            tf &= !(1u16 << s);
                            changed = 1;
                        }
                    } else if edges[e].cls == 1 {
                        if (ts & (1u16 << s)) != 0 && (tf & (1u16 << s)) == 0 {
                            ts &= !(1u16 << s);
                            changed = 1;
                        }
                    } else {
                        if (td & (1u16 << s)) != 0 && (tf & (1u16 << s)) == 0 {
                            td &= !(1u16 << s);
                            changed = 1;
                        }
                    }
                    if (exitchk & (1u16 << s)) != 0 {
                        if edges[e].cls == 0 || (full & (1u16 << s)) == 0 {
                            exitchk &= !(1u16 << s);
                            changed = 1;
                        }
                    }
                    u = tf | ts | td | exitchk;
                }
            }
            let nregs = ((tf | ts | td) as u32).count_ones() as c_int;
            let cost = (ckpt as u32).count_ones() as c_int
                + if nregs != 0 { 3 + nregs } else { 0 }
                + cost_extra;
            if gain > cost && g_n_fpb < FPB_MAX && planned_sites + sites + 2 < FPB_SITES_MAX {
                let fb = fpb_at(g_n_fpb);
                (*fb).first = i;
                (*fb).last = end;
                (*fb).end = end;
                (*fb).ckpt = ckpt;
                (*fb).ckpt_emit = ckpt;
                (*fb).written = written;
                (*fb).dirty_open = 0;
                (*fb).full = tf;
                (*fb).s0 = ts;
                (*fb).d0 = td;
                (*fb).gain = gain - cost;
                (*fb).dblonly = written & !everf;
                (*fb).site = core::ptr::null_mut();
                (*fb).back = core::ptr::null_mut();
                for q in i..=end {
                    *bat.add(q as usize) = g_n_fpb as i8;
                    *member(q as usize) = *marith(q as usize);
                }
                if getenv_on(c"OCERZ_FPB_DBGPRINT") {
                    libc::fprintf(
                        crate::log::stderr(),
                        c"FPB rip=%#llx batch %d [%d..%d] ckpt=%04x written=%04x Tf=%04x Ts=%04x Td=%04x exit=%04x tainted=%04x live=%04x undo=%d\n".as_ptr(),
                        i0.rip as c_ulonglong,
                        g_n_fpb,
                        i,
                        end,
                        ckpt as c_uint,
                        written as c_uint,
                        tf as c_uint,
                        ts as c_uint,
                        td as c_uint,
                        exitchk as c_uint,
                        tainted as c_uint,
                        l as c_uint,
                        nundo,
                    );
                }
                if exitchk != 0 {
                    g_fpb_exit_mask = exitchk;
                    g_fpb_exit_batch = g_n_fpb;
                    g_fpb_exit_end = end;
                    sites += 1;
                }
                planned_sites += sites + 1;
                g_n_fpb += 1;
            } else {
                for q in i..=end {
                    *stchk(q as usize) = 0;
                    *stlane(q as usize) = 0;
                    *det(q as usize) = 0;
                    *sidechk(q as usize) = 0;
                    fpb_undo_clear(q);
                }
            }
            i = end + 1;
        }
    }
}

static mut S_FPB_V1: c_int = -1;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn fpb_v1() -> c_int {
    unsafe {
        if S_FPB_V1 < 0 {
            S_FPB_V1 = getenv_on(c"OCERZ_FPB_V1") as c_int;
        }
        S_FPB_V1
    }
}

static mut S_UNSAFE_NOCHECKBR: c_int = -1;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn unsafe_nocheckbr() -> c_int {
    unsafe {
        if S_UNSAFE_NOCHECKBR < 0 {
            S_UNSAFE_NOCHECKBR = getenv_on(c"OCERZ_UNSAFE_NOCHECKBR") as c_int;
        }
        S_UNSAFE_NOCHECKBR
    }
}

static mut S_FPB_FORCEREPLAY: c_int = -1;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn fpb_emit_check(b: *mut ffi::A64Buf, fb: *mut ffi::FpBatch) {
    unsafe {
        let mut have_f = 0;
        let mut have_d = 0;
        (*fb).fcmp_vreg = -1;
        g_fcmp_self_idx_clear();
        if ((*fb).full | (*fb).s0 | (*fb).d0) == 0 {
            (*fb).site = core::ptr::null_mut();
            (*fb).back = core::ptr::null_mut();
            return;
        }
        let fullf = (*fb).full & !(*fb).dblonly;
        let fulld = (*fb).full & (*fb).dblonly;
        let mut dcur = -1;
        if fulld != 0 {
            let mut first = -1;
            let mut acc = -1;
            for r in 0..16u32 {
                if (fulld & (1u16 << r)) != 0 {
                    l0_flush_reg(b, r);
                    let v = xmm_vreg(r);
                    if first < 0 {
                        first = v;
                    } else if acc < 0 {
                        ffi::a64_v_fmax(b, 1, VX3, first, v);
                        acc = VX3;
                    } else {
                        ffi::a64_v_fmax(b, 1, VX3, VX3, v);
                    }
                }
            }
            ffi::a64_fmaxp_d(b, VX3, if acc >= 0 { acc } else { first });
            dcur = VX3;
        }
        if fullf != 0 {
            let mut first = -1;
            let mut acc = -1;
            for r in 0..16u32 {
                if (fullf & (1u16 << r)) != 0 {
                    l0_flush_reg(b, r);
                    let v = xmm_vreg(r);
                    if first < 0 {
                        first = v;
                    } else if acc < 0 {
                        ffi::a64_v_fmax(b, 0, VX0, first, v);
                        acc = VX0;
                    } else {
                        ffi::a64_v_fmax(b, 0, VX0, VX0, v);
                    }
                }
            }
            ffi::a64_fmaxv_4s(b, VX1, if acc >= 0 { acc } else { first });
            have_f = 1;
        }
        if (*fb).s0 != 0 {
            let mut cur = if have_f != 0 { VX1 } else { -1 };
            for r in 0..16u32 {
                if ((*fb).s0 & (1u16 << r)) != 0 {
                    let v = l0_src2(b, r, 0);
                    if cur < 0 {
                        cur = v;
                    } else {
                        ffi::a64_fmax_s(b, 0, VX1, cur, v);
                        cur = VX1;
                    }
                }
            }
            if cur != VX1 {
                ffi::a64_fmax_s(b, 0, VX1, cur, cur);
            }
            have_f = 1;
        }
        if (*fb).d0 != 0 || dcur >= 0 {
            let mut cur = dcur;
            for r in 0..16u32 {
                if ((*fb).d0 & (1u16 << r)) != 0 {
                    let v = l0_src2(b, r, 1);
                    if cur < 0 {
                        cur = v;
                    } else {
                        ffi::a64_fmax_s(b, 1, VX2, cur, v);
                        cur = VX2;
                    }
                }
            }
            ffi::a64_fcmp(b, 1, cur, cur);
            crate::ported::jit_fp::nan::g_fcmp_self_vreg = cur;
            crate::ported::jit_fp::nan::g_fcmp_self_idx = g_cur_insn_idx;
            (*fb).fcmp_vreg = cur as i8;
            have_d = 1;
        }
        if have_d != 0 && have_f != 0 && unsafe_nocheckbr() != 0 {
            (*fb).site = core::ptr::null_mut();
            (*fb).back = ffi::a64_label(b);
            (*fb).gain = 0;
            return;
        }
        if S_FPB_FORCEREPLAY < 0 {
            S_FPB_FORCEREPLAY = getenv_on(c"OCERZ_FPB_FORCEREPLAY") as c_int;
        }
        let vs = if S_FPB_FORCEREPLAY != 0 {
            A64_AL
        } else {
            A64_VS
        };
        if have_d != 0 && have_f != 0 {
            let dnan = ffi::a64_label(b);
            ffi::a64_bcond(b, vs, 0);
            ffi::a64_fcmp(b, 0, VX1, VX1);
            (*fb).site = ffi::a64_label(b);
            ffi::a64_bcond(b, vs, 0);
            let skip = ffi::a64_label(b);
            ffi::a64_b(b, 0);
            ffi::a64_patch_bcond(dnan, ffi::a64_label(b));
            let tramp = ffi::a64_label(b);
            ffi::a64_b(b, 0);
            ffi::a64_patch_b(skip, ffi::a64_label(b));
            (*fb).back = ffi::a64_label(b);
            (*fb).gain = tramp.offset_from((*fb).site) as c_int;
        } else if have_d != 0 {
            if unsafe_nocheckbr() != 0 {
                (*fb).site = core::ptr::null_mut();
            } else {
                (*fb).site = ffi::a64_label(b);
                ffi::a64_bcond(b, vs, 0);
            }
            (*fb).back = ffi::a64_label(b);
            (*fb).gain = 0;
        } else {
            ffi::a64_fcmp(b, 0, VX1, VX1);
            if unsafe_nocheckbr() != 0 {
                (*fb).site = core::ptr::null_mut();
            } else {
                (*fb).site = ffi::a64_label(b);
                ffi::a64_bcond(b, vs, 0);
            }
            (*fb).back = ffi::a64_label(b);
            (*fb).gain = 0;
        }
    }
}

#[inline(always)]
unsafe fn g_fcmp_self_idx_clear() {
    crate::ported::jit_fp::nan::g_fcmp_self_idx = -1;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn fpb_emit_regs_check(
    b: *mut ffi::A64Buf,
    regs: u16,
    batch: c_int,
    end: c_int,
    l0: *const i8,
    l0_dbl: *const u8,
) {
    unsafe {
        if regs == 0 || g_n_fpb_sites >= FPB_SITES_MAX {
            return;
        }
        let dmask = regs & (*fpb_at(batch)).dblonly;
        let fmask = regs & !dmask;
        let mut first = -1;
        let mut acc = -1;
        for r in 0..16u32 {
            if (fmask & (1u16 << r)) != 0 {
                let v = xmm_vreg(r);
                if first < 0 {
                    first = v;
                } else if acc < 0 {
                    ffi::a64_v_fmax(b, 0, VX0, first, v);
                    acc = VX0;
                } else {
                    ffi::a64_v_fmax(b, 0, VX0, VX0, v);
                }
            }
        }
        let mut dfirst = -1;
        let mut dacc = -1;
        for r in 0..16u32 {
            if (dmask & (1u16 << r)) != 0 {
                let v = xmm_vreg(r);
                if dfirst < 0 {
                    dfirst = v;
                } else if dacc < 0 {
                    ffi::a64_v_fmax(b, 1, VX3, dfirst, v);
                    dacc = VX3;
                } else {
                    ffi::a64_v_fmax(b, 1, VX3, VX3, v);
                }
            }
        }
        if dmask != 0 {
            ffi::a64_fmaxp_d(b, VX3, if dacc >= 0 { dacc } else { dfirst });
        }
        if fmask != 0 {
            ffi::a64_fmaxv_4s(b, VX1, if acc >= 0 { acc } else { first });
            if dmask != 0 {
                ffi::a64_fcvt_d2s(b, VX2, VX3);
                ffi::a64_fmax_s(b, 0, VX1, VX1, VX2);
            }
            ffi::a64_fcmp(b, 0, VX1, VX1);
        } else {
            ffi::a64_fcmp(b, 1, VX3, VX3);
        }
        let st = site_at(g_n_fpb_sites);
        g_n_fpb_sites += 1;
        (*st).batch = batch;
        (*st).end = end;
        (*st).site = ffi::a64_label(b);
        ffi::a64_bcond(b, A64_VS, 0);
        (*st).back = ffi::a64_label(b);
        for r in 0..16 {
            (*st).l0[r] = *l0.add(r);
            (*st).l0_dbl[r] = *l0_dbl.add(r);
        }
        (*st).fcmp_a = -1;
        (*st).fcmp_b = -1;
        (*st).fcmp_dbl = 0;
        (*st).keep_jt = 0;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn fpb_emit_store_check(b: *mut ffi::A64Buf, i: c_int, batch: c_int) {
    unsafe {
        let m = *stchk(i as usize);
        if m == 0 {
            return;
        }
        if *stlane(i as usize) != 0 {
            let r = m.trailing_zeros();
            let dbl = *stdbl(i as usize) as c_int;
            let v = l0_src2(b, r, dbl);
            ffi::a64_fcmp(b, dbl, v, v);
            fpb_site_emit(b, i - 1, -1, -1, dbl);
            return;
        }
        for r in 0..16u32 {
            if (m & (1u16 << r)) != 0 {
                l0_flush_reg(b, r);
            }
        }
        fpb_emit_regs_check(
            b,
            m,
            batch,
            i - 1,
            (&raw const g_l0).cast::<i8>(),
            (&raw const g_l0_dbl).cast::<u8>(),
        );
    }
}

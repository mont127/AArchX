//! The L0 xmm lane cache: per-block assignment of xmm registers to spare
//! vector lanes with dirty tracking and invalidation, the fixed-lane
//! precomputation that fills those lanes at block head, the ymmh half cache,
//! and the cmps+blendv pair fusion. The lane-recovery table (`g_lanerec`) is
//! consumed by the fault handler and must keep its exact field order.

use core::ffi::{c_int, c_uint};

use super::fpb::{fpb_class, fpb2_usedef, g_fpb_of, g_fpb_stchk, g_fpb_undo, g_fpb_undo_ld};
use super::*;
use crate::ffi;
use crate::jit_internal::*;

static mut S_EN: c_int = -1;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn l0_enabled() -> c_int {
    unsafe { env_on(c"OCERZ_NO_L0CACHE", &raw mut S_EN) ^ 1 }
}

#[unsafe(no_mangle)]
pub static mut g_l0: [i8; 16] = [0; 16];

#[unsafe(no_mangle)]
pub static mut g_l0_dbl: [u8; 16] = [0; 16];

#[unsafe(no_mangle)]
pub static mut g_l0_owners: [u16; 12] = [0; 12];

#[unsafe(no_mangle)]
pub static mut g_l0_next: c_uint = 0;

#[unsafe(no_mangle)]
pub static mut g_l0_nlanes: c_int = 12;

#[unsafe(no_mangle)]
pub static mut g_lane_used: u16 = 0;

#[unsafe(no_mangle)]
pub static mut g_l0_dirty: u16 = 0;

#[unsafe(no_mangle)]
pub static mut g_yc: [i8; 16] = [0; 16];

#[unsafe(no_mangle)]
pub static mut g_yc_dirty: u16 = 0;

#[unsafe(no_mangle)]
pub static mut g_lanerec: [ffi::JitLaneRec; JIT_MAX_BLOCK_INSNS * 2] =
    unsafe { core::mem::zeroed() };

#[unsafe(no_mangle)]
pub static mut g_n_lanerec: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_undo_vreg: [i8; 8] = [0; 8];

#[unsafe(no_mangle)]
pub static mut g_n_undo_lanes: c_int = 0;

#[inline(always)]
unsafe fn l0v(r: usize) -> *mut i8 {
    (&raw mut g_l0).cast::<i8>().add(r)
}
#[inline(always)]
unsafe fn l0d(r: usize) -> *mut u8 {
    (&raw mut g_l0_dbl).cast::<u8>().add(r)
}
#[inline(always)]
unsafe fn owners(t: usize) -> *mut u16 {
    (&raw mut g_l0_owners).cast::<u16>().add(t)
}
#[inline(always)]
unsafe fn ycv(r: usize) -> *mut i8 {
    (&raw mut g_yc).cast::<i8>().add(r)
}
#[inline(always)]
unsafe fn fl(r: usize) -> *mut i8 {
    (&raw mut g_l0_fixed_lane).cast::<i8>().add(r)
}
#[inline(always)]
unsafe fn fd(r: usize) -> *mut u8 {
    (&raw mut g_l0_fixed_dbl).cast::<u8>().add(r)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lanerec_note(off: u32) {
    unsafe {
        let mut r = ffi::JitLaneRec {
            off,
            dirty: g_l0_dirty,
            l0: [0; 16],
            yc: [0; 16],
        };
        for i in 0..16usize {
            *r.l0.get_unchecked_mut(i) = if *l0v(i) < 0 {
                0xff
            } else {
                ((*l0v(i) - 4) | if *l0d(i) != 0 { 0x10 } else { 0 }) as u8
            };
            *r.yc.get_unchecked_mut(i) = if *ycv(i) < 0 {
                0xff
            } else {
                (*ycv(i) - 4) as u8
            };
        }
        if g_n_lanerec > 0 {
            let l = (&raw const g_lanerec)
                .cast::<ffi::JitLaneRec>()
                .add(g_n_lanerec as usize - 1);
            if (*l).dirty == r.dirty && (*l).l0 == r.l0 && (*l).yc == r.yc {
                return;
            }
        }
        if (g_n_lanerec as usize) < JIT_MAX_BLOCK_INSNS * 2 {
            *(&raw mut g_lanerec)
                .cast::<ffi::JitLaneRec>()
                .add(g_n_lanerec as usize) = r;
            g_n_lanerec += 1;
        }
    }
}

static mut S_NO_L0_DEFER: c_int = -1;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn l0_defer() -> c_int {
    unsafe { env_on(c"OCERZ_NO_L0_DEFER", &raw mut S_NO_L0_DEFER) ^ 1 }
}

#[unsafe(no_mangle)]
pub static mut g_l0_fixed: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_l0_fixed_lane: [i8; 16] = [0; 16];

#[unsafe(no_mangle)]
pub static mut g_l0_fixed_dbl: [u8; 16] = [0; 16];

#[unsafe(no_mangle)]
pub static mut g_l0_fixed_dirty: u16 = 0;

unsafe fn insn_writes_xmm0(in_: *const ffi::X86Insn) -> c_int {
    unsafe {
        let in_ = &*in_;
        if in_.nops < 1 || in_.ops[0].kind as u32 != ffi::OCERZ_OPK_XMM {
            return 0;
        }
        match in_.op as u32 {
            ffi::OCERZ_OP_UCOMISS
            | ffi::OCERZ_OP_UCOMISD
            | ffi::OCERZ_OP_COMISS
            | ffi::OCERZ_OP_COMISD
            | ffi::OCERZ_OP_PTEST
            | ffi::OCERZ_OP_VTESTPS
            | ffi::OCERZ_OP_VTESTPD => 0,
            _ => 1,
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn l0_alloc2(b: *mut ffi::A64Buf, r: c_uint, dbl: c_int) -> c_int {
    unsafe {
        if g_l0_fixed != 0 {
            if *fl(r as usize) < 0 {
                l0_inval(r);
                return -1;
            }
            let t = *fl(r as usize) as c_int;
            if *l0v(r as usize) >= 0 && *l0v(r as usize) as c_int != t {
                *owners((*l0v(r as usize) - 4) as usize) &= !(1u16 << r);
            }
            let own = *owners((t - 4) as usize);
            for i in 0..16usize {
                if (own & (1u16 << i)) != 0 && i as u32 != r {
                    if (g_l0_dirty & (1u16 << i)) != 0 {
                        l0_flush_reg(b, i as c_uint);
                    }
                    *l0v(i) = -1;
                }
            }
            *owners((t - 4) as usize) = 1u16 << r;
            *l0v(r as usize) = t as i8;
            *l0d(r as usize) = dbl as u8;
            return t;
        }
        l0_inval(r);
        let t = 4 + (g_l0_next % g_l0_nlanes as c_uint) as c_int;
        g_l0_next = g_l0_next.wrapping_add(1);
        g_lane_used |= 1u16 << (t - 4);
        let own = *owners((t - 4) as usize);
        for i in 0..16usize {
            if (own & (1u16 << i)) != 0 {
                if (g_l0_dirty & (1u16 << i)) != 0 {
                    l0_flush_reg(b, i as c_uint);
                }
                *l0v(i) = -1;
            }
        }
        *owners((t - 4) as usize) = 1u16 << r;
        *l0v(r as usize) = t as i8;
        *l0d(r as usize) = dbl as u8;
        t
    }
}

static mut S_NO_LDP_PAIR: c_int = -1;

unsafe fn mov128_pair_kind(a: *const ffi::X86Insn, c: *const ffi::X86Insn) -> c_int {
    unsafe {
        if env_on(c"OCERZ_NO_LDP_PAIR", &raw mut S_NO_LDP_PAIR) != 0 {
            return 0;
        }
        let (a, c) = (&*a, &*c);
        if a.op != c.op || a.vex != c.vex || a.nops != 2 || c.nops != 2 {
            return 0;
        }
        if a.op as u32 != ffi::OCERZ_OP_MOVUPS
            && a.op as u32 != ffi::OCERZ_OP_MOVAPS
            && a.op as u32 != ffi::OCERZ_OP_MOVDQA
            && a.op as u32 != ffi::OCERZ_OP_MOVDQU
        {
            return 0;
        }
        if (a.vex & (ffi::OCERZ_VEX_L as u8 | ffi::OCERZ_VEX_NDS as u8)) != 0 {
            return 0;
        }
        if a.mode32 != 0
            || c.mode32 != 0
            || a.seg as u32 != ffi::OCERZ_SEG_NONE
            || c.seg as u32 != ffi::OCERZ_SEG_NONE
            || a.addrsize != 8
            || c.addrsize != 8
        {
            return 0;
        }
        let (ad, as_, cd, cs) = (&a.ops[0], &a.ops[1], &c.ops[0], &c.ops[1]);
        let load = ad.kind as u32 == ffi::OCERZ_OPK_XMM
            && as_.kind as u32 == ffi::OCERZ_OPK_MEM
            && cd.kind as u32 == ffi::OCERZ_OPK_XMM
            && cs.kind as u32 == ffi::OCERZ_OPK_MEM;
        let store = ad.kind as u32 == ffi::OCERZ_OPK_MEM
            && as_.kind as u32 == ffi::OCERZ_OPK_XMM
            && cd.kind as u32 == ffi::OCERZ_OPK_MEM
            && cs.kind as u32 == ffi::OCERZ_OPK_XMM;
        if !load && !store {
            return 0;
        }
        let am = if load { as_ } else { ad };
        let cm = if load { cs } else { cd };
        let ar = if load { ad.reg } else { as_.reg };
        let cr = if load { cd.reg } else { cs.reg };
        if xmm_is_pinned(ar as c_uint) == 0
            || xmm_is_pinned(cr as c_uint) == 0
            || (load && ar == cr)
        {
            return 0;
        }
        if am.riprel != 0
            || cm.riprel != 0
            || am.base != cm.base
            || am.index != cm.index
            || am.scale != cm.scale
        {
            return 0;
        }
        if cm.disp != am.disp + 16 || am.disp % 16 != 0 || am.disp < -1024 || am.disp > 1008 {
            return 0;
        }
        if ffi::vec_tso_relaxed() == 0
            && (mem_plain_access_ok(am) == 0 || mem_plain_access_ok(cm) == 0)
        {
            return 0;
        }
        if load { 1 } else { 2 }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_mov128_pair(
    b: *mut ffi::A64Buf,
    a: *const ffi::X86Insn,
    c: *const ffi::X86Insn,
    i: c_int,
) -> c_int {
    unsafe {
        let kind = mov128_pair_kind(a, c);
        if kind == 0 {
            return 0;
        }
        if !g_fpb_of.is_null() && *g_fpb_of.add(i as usize) != *g_fpb_of.add(i as usize + 1) {
            return 0;
        }
        if *(&raw const g_fpb_undo_ld).cast::<u8>().add(i as usize) != 0
            || *(&raw const g_fpb_undo_ld).cast::<u8>().add(i as usize + 1) != 0
            || *(&raw const g_fpb_undo).cast::<u8>().add(i as usize) != 0
            || *(&raw const g_fpb_undo).cast::<u8>().add(i as usize + 1) != 0
            || *(&raw const g_fpb_stchk).cast::<u16>().add(i as usize + 1) != 0
        {
            return 0;
        }
        let (a_, c_) = (&*a, &*c);
        let am = if kind == 1 { &a_.ops[1] } else { &a_.ops[0] };
        let ar = if kind == 1 {
            a_.ops[0].reg
        } else {
            a_.ops[1].reg
        };
        let cr = if kind == 1 {
            c_.ops[0].reg
        } else {
            c_.ops[1].reg
        };
        let va = xmm_vreg(ar as c_uint);
        let vc = xmm_vreg(cr as c_uint);
        if kind == 2 {
            l0_flush_reg(b, ar as c_uint);
            l0_flush_reg(b, cr as c_uint);
        }
        let mut ra = 0;
        let mut disp = 0u32;
        if ffi::emit_mem_ea_plain_ex(b, a, am, 16, &mut ra, &mut disp, 1) == 0 {
            return 0;
        }
        let d = disp as i32;
        let pair = (d % 16 == 0 && d >= -1024 && d <= 1008) as c_int;
        if kind == 1 {
            l0_inval(ar as c_uint);
            l0_inval(cr as c_uint);
            if pair != 0 {
                ffi::a64_ldp_q_off(b, va, vc, ra, d);
            } else {
                ffi::a64_ldr_v(b, 16, va, ra, disp);
                ffi::a64_ldr_v(b, 16, vc, ra, disp.wrapping_add(16));
            }
            if a_.vex != 0 {
                ffi::emit_ymmh_clear(b, ar as c_uint);
                ffi::emit_ymmh_clear(b, cr as c_uint);
            }
        } else {
            if pair != 0 {
                ffi::a64_stp_q_off(b, va, vc, ra, d);
            } else {
                ffi::a64_str_v(b, 16, va, ra, disp);
                ffi::a64_str_v(b, 16, vc, ra, disp.wrapping_add(16));
            }
        }
        *(&raw mut ffi::g_mov_skip).cast::<u8>().add(i as usize + 1) = 1;
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn vex_cmps_blendv_pair(
    c: *const ffi::X86Insn,
    v: *const ffi::X86Insn,
) -> c_int {
    unsafe {
        let (c, v) = (&*c, &*v);
        if c.vex == 0
            || v.vex == 0
            || ((c.vex | v.vex) & ffi::OCERZ_VEX_L as u8) != 0
            || c.mode32 != 0
            || v.mode32 != 0
        {
            return 0;
        }
        if !((c.op as u32 == ffi::OCERZ_OP_CMPSDX && v.op as u32 == ffi::OCERZ_OP_BLENDVPD)
            || (c.op as u32 == ffi::OCERZ_OP_CMPSS && v.op as u32 == ffi::OCERZ_OP_BLENDVPS))
        {
            return 0;
        }
        if (c.vex & ffi::OCERZ_VEX_NDS as u8) == 0
            || c.nops != 3
            || c.ops[0].kind as u32 != ffi::OCERZ_OPK_XMM
            || c.ops[1].kind as u32 != ffi::OCERZ_OPK_XMM
            || c.ops[2].kind as u32 != ffi::OCERZ_OPK_IMM
            || (c.ops[2].imm & 0x1f) >= 8
        {
            return 0;
        }
        if (v.vex & ffi::OCERZ_VEX_NDS as u8) == 0
            || (v.vex & ffi::OCERZ_VEX_IS4 as u8) == 0
            || v.nops != 3
            || v.ops[0].kind as u32 != ffi::OCERZ_OPK_XMM
            || v.ops[1].kind as u32 != ffi::OCERZ_OPK_XMM
            || v.ops[2].kind as u32 != ffi::OCERZ_OPK_XMM
        {
            return 0;
        }
        if c.seg as u32 != ffi::OCERZ_SEG_NONE || v.seg as u32 != ffi::OCERZ_SEG_NONE {
            return 0;
        }
        let m = c.ops[0].reg;
        let s1 = (v.vvvv & 15) as u32;
        let s2 = v.ops[1].reg;
        if v.ops[2].reg != m || s1 == m as u32 || s2 == m {
            return 0;
        }
        if xmm_is_pinned(m as c_uint) == 0
            || xmm_is_pinned((c.vvvv & 15) as c_uint) == 0
            || xmm_is_pinned(c.ops[1].reg as c_uint) == 0
            || xmm_is_pinned(v.ops[0].reg as c_uint) == 0
            || xmm_is_pinned(s1) == 0
            || xmm_is_pinned(s2 as c_uint) == 0
        {
            return 0;
        }
        l0_enabled()
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn l0_fixed_setup(
    b: *mut ffi::A64Buf,
    insns: *const ffi::X86Insn,
    n: c_int,
) -> c_int {
    unsafe {
        let mut cnt = [0i32; 16];
        let mut firstdbl = [-1i8; 16];
        let mut wfirst = [0u8; 16];
        for i in 0..n as usize {
            let in_ = &*insns.add(i);
            let mut packed = 0;
            let mut dbl = 0;
            let mut from_mem = 0;
            let mut sq = 0;
            let c = fpb_class(in_, &mut packed, &mut dbl, &mut from_mem, &mut sq);
            if in_.nops >= 1 && in_.ops[0].kind as u32 == ffi::OCERZ_OPK_XMM {
                let dr = in_.ops[0].reg as usize;
                if *wfirst.get_unchecked_mut(dr) == 0 {
                    let selfzero = in_.vex == 0
                        && (in_.op as u32 == ffi::OCERZ_OP_XORPS
                            || in_.op as u32 == ffi::OCERZ_OP_PXOR)
                        && in_.nops >= 2
                        && in_.ops[1].kind as u32 == ffi::OCERZ_OPK_XMM
                        && in_.ops[1].reg as usize == dr;
                    *wfirst.get_unchecked_mut(dr) = if c == 2 || selfzero { 2 } else { 1 };
                }
            }
            if in_.nops >= 2
                && in_.ops[1].kind as u32 == ffi::OCERZ_OPK_XMM
                && *wfirst.get_unchecked_mut(in_.ops[1].reg as usize) == 0
            {
                *wfirst.get_unchecked_mut(in_.ops[1].reg as usize) = 1;
            }
            let mut use_ = ((c == 1 || c == 3) && packed == 0) as c_int;
            if use_ == 0
                && in_.vex == 0
                && (in_.op as u32 == ffi::OCERZ_OP_CVTSI2SD
                    || in_.op as u32 == ffi::OCERZ_OP_CVTSI2SS)
            {
                use_ = 1;
                dbl = (in_.op as u32 == ffi::OCERZ_OP_CVTSI2SD) as c_int;
            }
            if use_ == 0
                && i + 1 < n as usize
                && vex_cmps_blendv_pair(insns.add(i), insns.add(i + 1)) != 0
            {
                let pd = (in_.op as u32 == ffi::OCERZ_OP_CMPSDX) as i8;
                let regs = [
                    (in_.vvvv & 15) as usize,
                    in_.ops[1].reg as usize,
                    (*insns.add(i + 1)).ops[0].reg as usize,
                    ((*insns.add(i + 1)).vvvv & 15) as usize,
                    (*insns.add(i + 1)).ops[1].reg as usize,
                ];
                for q in 0..5 {
                    *cnt.get_unchecked_mut(regs[q]) += 1;
                    if *firstdbl.get_unchecked_mut(regs[q]) < 0 {
                        *firstdbl.get_unchecked_mut(regs[q]) = pd;
                    }
                }
                continue;
            }
            if use_ == 0 {
                continue;
            }
            for q in 0..in_.nops.min(2) as usize {
                let o = &in_.ops.get_unchecked(q);
                if o.kind as u32 == ffi::OCERZ_OPK_XMM && xmm_is_pinned(o.reg as c_uint) != 0 {
                    *cnt.get_unchecked_mut(o.reg as usize) += 1;
                    if *firstdbl.get_unchecked_mut(o.reg as usize) < 0 {
                        *firstdbl.get_unchecked_mut(o.reg as usize) = dbl as i8;
                    }
                }
            }
            if (in_.vex & ffi::OCERZ_VEX_NDS as u8) != 0
                && xmm_is_pinned((in_.vvvv & 15) as c_uint) != 0
            {
                let v = (in_.vvvv & 15) as usize;
                *cnt.get_unchecked_mut(v) += 1;
                if *firstdbl.get_unchecked_mut(v) < 0 {
                    *firstdbl.get_unchecked_mut(v) = dbl as i8;
                }
            }
        }
        let mut key = [0i32; 16];
        for r in 0..16 {
            *key.get_unchecked_mut(r) = if *cnt.get_unchecked_mut(r) >= 2 {
                *cnt.get_unchecked_mut(r)
                    + if *wfirst.get_unchecked_mut(r) == 2 {
                        0
                    } else {
                        1000
                    }
            } else {
                0
            };
        }
        let mut lanes = 0;
        for _k in 0..g_l0_nlanes {
            let mut best = -1i32;
            for r in 0..16usize {
                if *fl(r) < 0
                    && *key.get_unchecked_mut(r) > 0
                    && (best < 0
                        || *key.get_unchecked_mut(r) > *key.get_unchecked_mut(best as usize))
                {
                    best = r as i32;
                }
            }
            if best < 0 {
                break;
            }
            *fl(best as usize) = (4 + lanes) as i8;
            *fd(best as usize) = (*firstdbl.get_unchecked_mut(best as usize) > 0) as u8;
            lanes += 1;
        }
        if lanes == 0 {
            return 0;
        }
        g_l0_fixed_dirty = 0;
        for i in 0..n as usize {
            if insn_writes_xmm0(insns.add(i)) != 0 {
                g_l0_fixed_dirty |= 1u16 << (*insns.add(i)).ops[0].reg;
            }
        }
        for r in 0..16usize {
            if *fl(r) >= 0 {
                ffi::a64_v_mov(b, *fl(r) as c_int, xmm_vreg(r as c_uint));
            }
        }
        g_l0_fixed = 1;
        l0_fixed_map();
        1
    }
}

static mut S_NO_YMMH_CACHE: c_int = -1;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yc_setup(
    b: *mut ffi::A64Buf,
    insns: *const ffi::X86Insn,
    n: c_int,
) -> c_int {
    unsafe {
        if env_on(c"OCERZ_NO_YMMH_CACHE", &raw mut S_NO_YMMH_CACHE) != 0 {
            return 0;
        }
        let mut cnt = [0i32; 16];
        for i in 0..n as usize {
            let in_ = &*insns.add(i);
            if (in_.vex & ffi::OCERZ_VEX_L as u8) == 0 || in_.mode32 != 0 {
                continue;
            }
            for k in 0..in_.nops as usize {
                if in_.ops.get_unchecked(k).kind as u32 == ffi::OCERZ_OPK_XMM
                    && in_.ops.get_unchecked(k).reg < 16
                {
                    *cnt.get_unchecked_mut(in_.ops.get_unchecked(k).reg as usize) += 1;
                }
            }
            if (in_.vex & ffi::OCERZ_VEX_NDS as u8) != 0 {
                *cnt.get_unchecked_mut((in_.vvvv & 15) as usize) += 1;
            }
        }
        let mut used = g_lane_used;
        for r in 0..16usize {
            if *fl(r) >= 0 {
                used |= 1u16 << (*fl(r) - 4);
            }
        }
        let mut got = 0;
        loop {
            let mut best = -1i32;
            for r in 0..16usize {
                if *ycv(r) < 0
                    && *cnt.get_unchecked_mut(r) > 0
                    && (best < 0
                        || *cnt.get_unchecked_mut(r) > *cnt.get_unchecked_mut(best as usize))
                {
                    best = r as i32;
                }
            }
            if best < 0 {
                break;
            }
            let mut lane = -1;
            for k in (0..g_l0_nlanes).rev() {
                if (used & (1u16 << k)) == 0 {
                    lane = k;
                    break;
                }
            }
            if lane < 0 {
                break;
            }
            used |= 1u16 << lane;
            g_lane_used |= 1u16 << lane;
            *ycv(best as usize) = (4 + lane) as i8;
            got += 1;
        }
        if got == 0 {
            return 0;
        }
        for r in 0..16usize {
            if *ycv(r) >= 0 {
                ffi::a64_ldr_v(b, 16, *ycv(r) as c_int, 20, YMMH_OFF + r as u32 * 16);
                g_yc_dirty |= 1u16 << r;
            }
        }
        g_l0_fixed = 1;
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn l0_fixed_restore(b: *mut ffi::A64Buf) {
    unsafe {
        let mut pend: u16 = 0;
        for r in 0..16usize {
            let t = *fl(r);
            if if t >= 0 {
                !(*l0v(r) == t && *l0d(r) == *fd(r))
            } else {
                *l0v(r) >= 0
            } {
                pend |= 1u16 << r;
            }
        }
        while pend != 0 {
            let mut r = -1i32;
            for c in 0..16usize {
                if r >= 0 {
                    break;
                }
                if (pend & (1u16 << c)) == 0 {
                    continue;
                }
                let t = *fl(c);
                let mut blocked = 0;
                if t >= 0 {
                    for o in 0..16usize {
                        if o != c && (pend & (1u16 << o)) != 0 && *l0v(o) == t {
                            blocked = 1;
                        }
                    }
                }
                if blocked == 0 {
                    r = c as i32;
                }
            }
            if r < 0 {
                for c in 0..16usize {
                    if (pend & (1u16 << c)) != 0 {
                        l0_flush_reg(b, c as c_uint);
                    }
                }
                for c in 0..16usize {
                    if (pend & (1u16 << c)) != 0 && *fl(c) >= 0 {
                        ffi::a64_v_mov(b, *fl(c) as c_int, xmm_vreg(c as c_uint));
                    }
                }
                break;
            }
            let r = r as usize;
            let t = *fl(r);
            let u = *l0v(r);
            if t < 0 {
                l0_flush_reg(b, r as c_uint);
                l0_inval(r as c_uint);
            } else if u >= 0 && *l0d(r) == *fd(r) {
                if *fd(r) != 0 {
                    ffi::a64_fmov_d_d(b, t as c_int, u as c_int);
                } else {
                    ffi::a64_fmov_s_s(b, t as c_int, u as c_int);
                }
            } else {
                l0_flush_reg(b, r as c_uint);
                ffi::a64_v_mov(b, t as c_int, xmm_vreg(r as c_uint));
            }
            pend &= !(1u16 << r);
        }
        l0_fixed_map();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn cmps_blendv_fusable(cmps_idx: c_int) -> c_int {
    unsafe {
        if g_cur_insns.is_null() || cmps_idx < 0 || cmps_idx + 1 >= g_cur_insns_n {
            return 0;
        }
        let c = &*g_cur_insns.add(cmps_idx as usize);
        let v = &*g_cur_insns.add(cmps_idx as usize + 1);
        if c.vex != 0 || v.vex != 0 {
            return 0;
        }
        if c.ops[0].kind as u32 != ffi::OCERZ_OPK_XMM || c.ops[0].reg != 0 || c.nops < 3 {
            return 0;
        }
        if !((c.op as u32 == ffi::OCERZ_OP_CMPSDX && v.op as u32 == ffi::OCERZ_OP_BLENDVPD)
            || (c.op as u32 == ffi::OCERZ_OP_CMPSS && v.op as u32 == ffi::OCERZ_OP_BLENDVPS))
        {
            return 0;
        }
        if v.nops < 2
            || v.ops[0].kind as u32 != ffi::OCERZ_OPK_XMM
            || v.ops[0].reg == 0
            || xmm_is_pinned(v.ops[0].reg as c_uint) == 0
            || xmm_is_pinned(0) == 0
        {
            return 0;
        }
        if v.ops[1].kind as u32 == ffi::OCERZ_OPK_XMM && v.ops[1].reg == 0 {
            return 0;
        }
        if v.seg as u32 != ffi::OCERZ_SEG_NONE || l0_enabled() == 0 {
            return 0;
        }
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn vex_lane_aware(insn: *const ffi::X86Insn) -> c_int {
    unsafe {
        let in_ = &*insn;
        if in_.vex == 0 || (in_.vex & ffi::OCERZ_VEX_L as u8) != 0 || in_.mode32 != 0 {
            return 0;
        }
        if !g_cur_insns.is_null()
            && insn >= g_cur_insns
            && insn < g_cur_insns.add(g_cur_insns_n as usize)
        {
            if (in_.op as u32 == ffi::OCERZ_OP_CMPSS || in_.op as u32 == ffi::OCERZ_OP_CMPSDX)
                && insn.add(1) < g_cur_insns.add(g_cur_insns_n as usize)
            {
                return vex_cmps_blendv_pair(insn, insn.add(1));
            }
            if (in_.op as u32 == ffi::OCERZ_OP_BLENDVPD || in_.op as u32 == ffi::OCERZ_OP_BLENDVPS)
                && insn > g_cur_insns
            {
                return vex_cmps_blendv_pair(insn.sub(1), insn);
            }
        }
        if in_.op as u32 >= ffi::OCERZ_OP_VFMA_FIRST && in_.op as u32 <= ffi::OCERZ_OP_VFMA_LAST {
            let kind = ((in_.op as c_int - ffi::OCERZ_OP_VFMA_FIRST as c_int) >> 1) % 10;
            return (kind >= 2 && (kind & 1) != 0) as c_int;
        }
        match in_.op as u32 {
            ffi::OCERZ_OP_ADDSS
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
            | ffi::OCERZ_OP_COMISS
            | ffi::OCERZ_OP_COMISD
            | ffi::OCERZ_OP_UCOMISS
            | ffi::OCERZ_OP_UCOMISD
            | ffi::OCERZ_OP_CVTTSS2SI
            | ffi::OCERZ_OP_CVTTSD2SI
            | ffi::OCERZ_OP_MOVSS
            | ffi::OCERZ_OP_MOVSDX
            | ffi::OCERZ_OP_MOVDDUP => 1,
            _ => 0,
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn l0_pre_insn(b: *mut ffi::A64Buf, insn: *const ffi::X86Insn) {
    unsafe {
        let in_ = &*insn;
        if vex_lane_aware(insn) != 0 || !(in_.vex != 0 || l0_aware_op(in_.op as c_uint) == 0) {
            return;
        }
        let mut use_: u16 = 0;
        let mut kill: u16 = 0;
        fpb2_usedef(insn, &mut use_, &mut kill);
        for k in 0..in_.nops as usize {
            if in_.ops.get_unchecked(k).kind as u32 == ffi::OCERZ_OPK_XMM
                && in_.ops.get_unchecked(k).reg < 16
                && (use_ & (1u16 << in_.ops.get_unchecked(k).reg)) != 0
            {
                l0_flush_reg(b, in_.ops.get_unchecked(k).reg as c_uint);
            }
        }
        if (in_.vex & ffi::OCERZ_VEX_NDS as u8) != 0 {
            l0_flush_reg(b, in_.vvvv as c_uint);
        }
        if in_.op as u32 == ffi::OCERZ_OP_BLENDVPD
            || in_.op as u32 == ffi::OCERZ_OP_BLENDVPS
            || in_.op as u32 == ffi::OCERZ_OP_PBLENDVB
        {
            l0_flush_reg(b, 0);
        }
        for k in 0..in_.nops as usize {
            if in_.ops.get_unchecked(k).kind as u32 == ffi::OCERZ_OPK_XMM
                && (k == 0
                    || in_.op as u32 == ffi::OCERZ_OP_BLENDVPD
                    || in_.op as u32 == ffi::OCERZ_OP_BLENDVPS
                    || in_.op as u32 == ffi::OCERZ_OP_PBLENDVB)
            {
                l0_inval(in_.ops.get_unchecked(k).reg as c_uint);
            }
        }
        if in_.op as u32 == ffi::OCERZ_OP_FXRSTOR || in_.op as u32 == ffi::OCERZ_OP_SYSCALL {
            l0_flush_all(b);
            l0_reset();
        }
    }
}

unsafe fn l0_aware_op(op: c_uint) -> c_int {
    match op as u32 {
        ffi::OCERZ_OP_BLENDVPD
        | ffi::OCERZ_OP_BLENDVPS
        | ffi::OCERZ_OP_CMPSS
        | ffi::OCERZ_OP_CMPSDX
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
        | ffi::OCERZ_OP_COMISS
        | ffi::OCERZ_OP_COMISD
        | ffi::OCERZ_OP_UCOMISS
        | ffi::OCERZ_OP_UCOMISD
        | ffi::OCERZ_OP_CVTTSS2SI
        | ffi::OCERZ_OP_CVTTSD2SI
        | ffi::OCERZ_OP_MOVAPS
        | ffi::OCERZ_OP_MOVUPS
        | ffi::OCERZ_OP_MOVDQA
        | ffi::OCERZ_OP_MOVDQU
        | ffi::OCERZ_OP_MOVSS
        | ffi::OCERZ_OP_MOVSDX
        | ffi::OCERZ_OP_MOVDDUP => 1,
        _ => 0,
    }
}

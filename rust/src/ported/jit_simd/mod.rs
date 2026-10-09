//!
//! ---- SSE and FP ----
//! x86's NaN rule (result NaN -> quiet(a) if a is NaN, else quiet(b), else the
//! default NaN) differs from arm64's, so the hot path branches out of line to an
//! exact fixup arm emitted after the body.  FP batches take that further: a run
//! of arithmetic is checked once at its end rather than per instruction, with
//! the registers it overwrites checkpointed and the whole run replayed exactly
//! if the check fires.  A double batch's taint can ride past replayable
//! instructions to a ucomisd/comisd, which raises V for a NaN in either
//! operand's lane 0 and so detects for free; lanes still unverified at a
//! superblock side exit are checked in that exit's stub, off the hot path.
//! Scalar chains additionally keep lane 0 in a scratch V register (fixed-lane
//! mode gives four scratches to a self-looping block for its whole body) so a
//! loop-carried scalar chain never round-trips through the architectural
//! register.  A generated NaN kept arm64's sign until the exact arm was given an
//! off-chain copy of the operand the result overwrites (found by fp_loop_nan).
//!
//! All of that is what a processor without FPCR.AH needs.  With the bit set
//! (ocerz_afp, src/cpu.c) arm64's add, subtract, multiply, divide and square
//! root give x86's NaN results themselves as long as the x86 destination is the
//! first arm64 operand, which is how they were already emitted, so they go out
//! bare, and a batch made only of those, moves, shuffles, compares and stores
//! keeps its members' fast emission and drops its checkpoint, its checks, its
//! undo log and its replay arms.  In a packed loop the end-of-batch check was
//! three vector operations on top of the eight that did the work, and the four
//! vector pipelines were what bounded it: fpvec went from 1.06x of Rosetta's
//! time to 0.82x.  min and max were exact already, being a compare and a
//! select.  Fused multiply-add is not covered: its negated forms negate a NaN
//! operand where x86 returns it as it came, so a batch holding one keeps the
//! whole apparatus, and outside a batch it keeps its own check.
//!
//! An emitter that takes a memory operand reaches it through emit_mem_load_any
//! or emit_sse_mem_addr, never through the plain helpers alone.  The plain forms
//! need guest and host addresses to coincide, which the low shadow window of
//! the Wine mode breaks, so a plain-only emitter hands every memory form to the
//! interpreter there and nowhere else: Steam's webhelper interpreted pinsrw from
//! memory for 15% of its samples, and the same rule held back bsf/bsr, the wide
//! and three-operand multiplies, pmovzx from a word, pextr to memory and the
//! BMI sources.  gs- and fs-relative operands are formed in that mode too, with
//! Wine's gs:[0x58] redirect reading the TEB pointer at gs:[0x30] through the
//! same translation (OCERZ_NO_LOW_SEG restores the interpreter for them).
//!
//! That translation sits in front of every guest memory access in the Wine
//! mode, and in its general form it builds three 64-bit constants and tests
//! three ranges - the low window, the identity middle, the top strip with the
//! commpage in it - about twenty instructions for one load.  When the low
//! window's host base is a single run of bits above every low address, as the
//! usual 0x8000000000 is, an address below 12 GB becomes host with one orr
//! after a shift and a compare, and one between 12 GB and the top strip is
//! recognised as identity with two shifts and an add; only the top strip
//! takes the general form.  In the Wine layout that took memcpy from 6.1x of
//! Rosetta's time to 3.1x, a mixed workload from 3.0x to 1.65x and an
//! interpreter loop from 1.5x to 1.0x (OCERZ_NO_FAST_LOW_GUARD=1 keeps the
//! general form everywhere).  The top strip is not tested at all until an
//! access in the block faults there: arm64 cannot map anything at or above
//! TOP_LO, so its identity address faults, and the handler marks the block the
//! way it marks a commpage reader in identity mode, interprets the one
//! instruction and retires the block.  The retranslation then tests the
//! identity range with two shifts and an add and takes the general form out of
//! line at the end of the block.  A low address costs a shift, a compare, an
//! untaken branch and an orr, and an identity address the first three
//! (OCERZ_LOW_TOP_GUARD=1 tests the top strip in every block).  Stack accesses (push, pop, call, ret and rsp-relative operands) are
//! plain in this mode as in every other, after the translation instead of in
//! place of it; they used to take the ordered load and store
//! (OCERZ_TSO_STRICT=1 orders them everywhere).  Their translation is the stack
//! delta kept in x0 rather than the guard (emit_stack_delta), and a rip-relative
//! address, whose side of 12 GB is known when the block is translated, takes an
//! orr below it and nothing above it.  On xbench in the Wine layout these took
//! leafcall from 1.27 s to 0.65 s (Rosetta 0.62 s) and str from 0.84 s to 0.75 s.
//! A 32-bit address is below 4 GB, so in 32-bit code every translation is the
//! orr alone, and esp-relative operands there are stack accesses as well: a
//! WoW64 loop of virtual calls into small frames spent most of its time in the
//! acquire loads and release stores of its stack slots, and went from 459 to
//! 163 ms (Rosetta 158).
//!
//! The integer SSE forms map almost one to one: widening multiplies and a
//! narrowing unzip for the high halves and pmaddubsw, saturating narrows for the
//! packs, uabd with three pairwise widening adds for psadbw, addp or an unzip
//! pair for the horizontal ops, ext against a zero register for the byte shifts
//! and palignr, and lane inserts for the immediate blends and pshuflw/hw.  A
//! shift by an xmm or m128 count clamps the 64-bit count to 64 and shifts by a
//! duplicated register, which reproduces x86's saturation: zero for the logical
//! shifts, the sign for the arithmetic ones.  cvtps2dq rounds with frinti, which
//! follows the FPCR mode that tracks MXCSR.RC, and then shares cvttps2dq's
//! conversion, whose lanes that are NaN or not below 2^31 take x86's
//! 0x80000000 where arm64 would saturate or give zero.  cvtps2pd and cvtpd2ps
//! are fcvtl and fcvtn, which quiet a signalling NaN and keep its payload and
//! round by the FPCR mode as x86 does; fcvtn also clears the upper half.  AES rounds use aese or
//! aesd against a zero key, then aesmc or aesimc, then the round key, because
//! arm64 adds the key before the substitution and x86 after it;
//! aeskeygenassist picks its words out of a zero-key aese with a table lookup.
//! crc32 is the arm64 crc32c of the same width, pclmulqdq is pmull.
//!
//! MMX instructions are translated too (emit_mmx): the eight registers live in
//! the cpu's mmx array, each instruction loads what it reads into a scratch
//! vector register with a d-sized load, which clears the upper half, runs the
//! 128-bit form of the operation and stores the low 64 bits back, so lanewise
//! operations need nothing more.  The others are arranged to land in the low
//! half: a high unpack is a low zip followed by the upper half, the packs join
//! both operands into one register before narrowing, pmulhw and pmulhuw keep
//! the odd halves of a widening multiply, and psadbw is uabd with three
//! pairwise widening adds as in SSE.  Each one leaves the x87 tag word full and
//! TOP at zero, as the interpreter does.  UnityPlayer's video decoder is written
//! in MMX, and as one slow-path call per instruction it was a few percent of
//! R.E.P.O.'s process; a loop of its moves and multiplies runs 9.8 times faster
//! translated (Rosetta is 2.7 times faster again, since every instruction here
//! goes through memory).  OCERZ_NO_JIT_MMX=1 interprets them again.
//!
//!
//! A memory operand is loaded here whatever the flags' fate, so a fault stays
//! this instruction's, and kept in jit_fcmp_mem: a jcc, setcc or cmovcc that
//! comis_fuse_producer pairs with it compares the pinned register against
//! that copy instead of reading the flags back out of RFLAGS.
//!
//! shufps of a register with itself: a broadcast is one dup, a change to one lane one ins.
//!
//! The two-source selections a transpose is made of, each one instruction into the destination.
//!

#![allow(
    unused_mut,
    unused_variables,
    unused_assignments,
    unused_unsafe,
    unsafe_op_in_unsafe_fn,
    static_mut_refs
)]

use crate::ffi::*;
use crate::inline::{OCERZ_AF, OCERZ_OF, OCERZ_SF, ocerz_cc_pack};
use crate::jit_internal::*;

#[unsafe(no_mangle)]
pub static mut g_raslit: [RasLit; RASLIT_MAX as usize] = [RasLit {
    site: ::core::ptr::null::<u32>() as *mut u32,
    retaddr: 0,
    hi: 0,
    kind: 0,
    rt: 0,
    tcr: 0,
}; RASLIT_MAX as usize];
#[unsafe(no_mangle)]
pub static mut g_n_raslit: ::core::ffi::c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_vec_int_move: ::core::ffi::c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_zero_vreg: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
#[unsafe(no_mangle)]
pub static mut g_blk_ymm_write: ::core::ffi::c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_cur_need: u64 = 0;
#[unsafe(no_mangle)]
pub static mut g_xmm_pinned: u16 = 0;
#[unsafe(no_mangle)]
pub unsafe extern "C" fn xmm_pinning_enabled() -> ::core::ffi::c_int {
    static mut on: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    if on < 0 as ::core::ffi::c_int {
        on = if !libc::getenv(c"OCERZ_NO_XMM_PIN".as_ptr()).is_null() {
            0 as ::core::ffi::c_int
        } else {
            1 as ::core::ffi::c_int
        };
    }
    return on;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn xmm_global_enabled() -> ::core::ffi::c_int {
    static mut on: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    if on < 0 as ::core::ffi::c_int {
        on = if !libc::getenv(c"OCERZ_NO_XMM_GLOBAL".as_ptr()).is_null()
            || !libc::getenv(c"OCERZ_NO_FULLPIN".as_ptr()).is_null()
        {
            0 as ::core::ffi::c_int
        } else {
            1 as ::core::ffi::c_int
        };
    }
    return on;
}
unsafe fn emit_xmm_ld(mut b: *mut A64Buf, mut vd: ::core::ffi::c_int, mut xr: ::core::ffi::c_uint) {
    l0_flush_reg(b, xr);
    if xmm_is_pinned(xr) != 0 {
        if vd != xmm_vreg(xr) {
            a64_v_mov(b, vd, xmm_vreg(xr));
        }
        return;
    }
    a64_ldr_v(
        b,
        16 as ::core::ffi::c_int,
        vd,
        20 as ::core::ffi::c_int,
        XMM_BASE_OFF.wrapping_add((xr as u32).wrapping_mul(16 as u32)),
    );
}
unsafe fn emit_xmm_st(mut b: *mut A64Buf, mut vs: ::core::ffi::c_int, mut xr: ::core::ffi::c_uint) {
    if xmm_is_pinned(xr) != 0 {
        if vs != xmm_vreg(xr) {
            a64_v_mov(b, xmm_vreg(xr), vs);
        }
        return;
    }
    a64_str_v(
        b,
        16 as ::core::ffi::c_int,
        vs,
        20 as ::core::ffi::c_int,
        XMM_BASE_OFF.wrapping_add((xr as u32).wrapping_mul(16 as u32)),
    );
}
unsafe fn emit_xmm_ld_lo(
    mut b: *mut A64Buf,
    mut size: ::core::ffi::c_int,
    mut vd: ::core::ffi::c_int,
    mut xr: ::core::ffi::c_uint,
) {
    l0_flush_reg(b, xr);
    if xmm_is_pinned(xr) != 0 {
        if size == 8 as ::core::ffi::c_int {
            a64_fmov_d_d(b, vd, xmm_vreg(xr));
        } else {
            a64_fmov_s_s(b, vd, xmm_vreg(xr));
        }
        return;
    }
    a64_ldr_v(
        b,
        size,
        vd,
        20 as ::core::ffi::c_int,
        XMM_BASE_OFF.wrapping_add((xr as u32).wrapping_mul(16 as u32)),
    );
}
unsafe fn emit_xmm_st_lo(
    mut b: *mut A64Buf,
    mut size: ::core::ffi::c_int,
    mut vs: ::core::ffi::c_int,
    mut xr: ::core::ffi::c_uint,
) {
    if xmm_is_pinned(xr) != 0 {
        if l0_defer_take(vs, xr, size) != 0 {
            return;
        }
        if size == 8 as ::core::ffi::c_int {
            a64_ins_d_d(
                b,
                xmm_vreg(xr),
                0 as ::core::ffi::c_int,
                vs,
                0 as ::core::ffi::c_int,
            );
        } else {
            a64_ins_s_s(
                b,
                xmm_vreg(xr),
                0 as ::core::ffi::c_int,
                vs,
                0 as ::core::ffi::c_int,
            );
        }
        return;
    }
    a64_str_v(
        b,
        size,
        vs,
        20 as ::core::ffi::c_int,
        XMM_BASE_OFF.wrapping_add((xr as u32).wrapping_mul(16 as u32)),
    );
}
#[unsafe(no_mangle)]
pub static mut g_sse_mem_ra: ::core::ffi::c_int = JTA as ::core::ffi::c_int;
#[unsafe(no_mangle)]
pub static mut g_sse_mem_disp: u32 = 0;
#[unsafe(no_mangle)]
pub static mut g_sse_mem_plain: ::core::ffi::c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_sse_mem_plainacc: ::core::ffi::c_int = 0;
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_sse_mem_addr(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut o: *const X86Operand,
    mut size: ::core::ffi::c_int,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
    mut skip_out: *mut *mut u32,
) -> ::core::ffi::c_int {
    g_sse_mem_plainacc = mem_plain_access_ok(o);
    if emit_mem_ea_plain_ex(
        b,
        insn,
        o,
        size,
        &raw mut g_sse_mem_ra,
        &raw mut g_sse_mem_disp,
        1 as ::core::ffi::c_int,
    ) != 0
    {
        g_sse_mem_plain = 1 as ::core::ffi::c_int;
        *skip_out = ::core::ptr::null_mut::<u32>();
        return 1 as ::core::ffi::c_int;
    }
    g_sse_mem_plain = 0 as ::core::ffi::c_int;
    g_sse_mem_ra = JTA as ::core::ffi::c_int;
    g_sse_mem_disp = 0 as u32;
    if emit_mem_ea(b, insn, o, JTA as ::core::ffi::c_int) == 0 {
        return 0 as ::core::ffi::c_int;
    }
    *skip_out = emit_commpage_guard(b, insn, JTA as ::core::ffi::c_int, exit_sites, n_exits);
    emit_add_const(
        b,
        JTA as ::core::ffi::c_int,
        ocerz_guest_base.wrapping_sub(ea_fold()),
    );
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_sse_src(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut o: *const X86Operand,
    mut size: ::core::ffi::c_int,
    mut vd: ::core::ffi::c_int,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*o).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
        emit_xmm_ld(b, vd, (*o).reg as ::core::ffi::c_uint);
        return 1 as ::core::ffi::c_int;
    }
    if (*o).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int {
        let mut skip: *mut u32 = ::core::ptr::null_mut::<u32>();
        if emit_sse_mem_addr(b, insn, o, size, exit_sites, n_exits, &raw mut skip) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        emit_sse_mem_ld(b, size, vd);
        patch_guard_skip(skip, a64_label(b));
        return 1 as ::core::ffi::c_int;
    }
    return 0 as ::core::ffi::c_int;
}
unsafe fn emit_sse_src_reg(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut o: *const X86Operand,
    mut size: ::core::ffi::c_int,
    mut vtmp: ::core::ffi::c_int,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*o).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
        && xmm_is_pinned((*o).reg as ::core::ffi::c_uint) != 0
    {
        l0_flush_reg(b, (*o).reg as ::core::ffi::c_uint);
        return xmm_vreg((*o).reg as ::core::ffi::c_uint);
    }
    if emit_sse_src(b, insn, o, size, vtmp, exit_sites, n_exits) == 0 {
        return -(1 as ::core::ffi::c_int);
    }
    return vtmp;
}
#[inline]
unsafe fn xmm_dst_reg(
    mut xr: ::core::ffi::c_uint,
    mut vtmp: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    return if xmm_is_pinned(xr) != 0 {
        xmm_vreg(xr)
    } else {
        vtmp
    };
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sse_enabled() -> ::core::ffi::c_int {
    static mut on: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    if on < 0 as ::core::ffi::c_int {
        on = if !libc::getenv(c"OCERZ_NO_INLINE_SSE".as_ptr()).is_null() {
            0 as ::core::ffi::c_int
        } else {
            1 as ::core::ffi::c_int
        };
    }
    return on;
}
unsafe fn emit_sse_mov128(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
    {
        if (*d).reg as ::core::ffi::c_int != (*s).reg as ::core::ffi::c_int {
            if xmm_is_pinned((*d).reg as ::core::ffi::c_uint) != 0
                && xmm_is_pinned((*s).reg as ::core::ffi::c_uint) != 0
            {
                l0_inval((*d).reg as ::core::ffi::c_uint);
                a64_v_mov(
                    b,
                    xmm_vreg((*d).reg as ::core::ffi::c_uint),
                    xmm_vreg((*s).reg as ::core::ffi::c_uint),
                );
            } else {
                l0_flush_reg(b, (*s).reg as ::core::ffi::c_uint);
                emit_xmm_ld(
                    b,
                    VX0 as ::core::ffi::c_int,
                    (*s).reg as ::core::ffi::c_uint,
                );
                emit_xmm_st(
                    b,
                    VX0 as ::core::ffi::c_int,
                    (*d).reg as ::core::ffi::c_uint,
                );
            }
            l0_share(
                (*d).reg as ::core::ffi::c_uint,
                (*s).reg as ::core::ffi::c_uint,
            );
        }
        return 1 as ::core::ffi::c_int;
    }
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int
    {
        let mut skip: *mut u32 = ::core::ptr::null_mut::<u32>();
        l0_inval((*d).reg as ::core::ffi::c_uint);
        let mut vd: ::core::ffi::c_int = if xmm_is_pinned((*d).reg as ::core::ffi::c_uint) != 0 {
            xmm_vreg((*d).reg as ::core::ffi::c_uint)
        } else {
            VX0 as ::core::ffi::c_int
        };
        if vd != VX0 as ::core::ffi::c_int
            && emit_plain_mem_fast(
                b,
                insn,
                s,
                16 as ::core::ffi::c_int,
                vd,
                0 as ::core::ffi::c_int,
                1 as ::core::ffi::c_int,
            ) != 0
        {
            return 1 as ::core::ffi::c_int;
        }
        if emit_sse_mem_addr(
            b,
            insn,
            s,
            16 as ::core::ffi::c_int,
            exit_sites,
            n_exits,
            &raw mut skip,
        ) == 0
        {
            return 0 as ::core::ffi::c_int;
        }
        emit_sse_mem_ld(b, 16 as ::core::ffi::c_int, vd);
        patch_guard_skip(skip, a64_label(b));
        if vd == VX0 as ::core::ffi::c_int {
            emit_xmm_st(
                b,
                VX0 as ::core::ffi::c_int,
                (*d).reg as ::core::ffi::c_uint,
            );
        }
        return 1 as ::core::ffi::c_int;
    }
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
    {
        l0_flush_reg(b, (*s).reg as ::core::ffi::c_uint);
        let mut vs: ::core::ffi::c_int = if xmm_is_pinned((*s).reg as ::core::ffi::c_uint) != 0 {
            xmm_vreg((*s).reg as ::core::ffi::c_uint)
        } else {
            VX0 as ::core::ffi::c_int
        };
        if vs != VX0 as ::core::ffi::c_int
            && emit_plain_mem_fast(
                b,
                insn,
                d,
                16 as ::core::ffi::c_int,
                vs,
                1 as ::core::ffi::c_int,
                1 as ::core::ffi::c_int,
            ) != 0
        {
            return 1 as ::core::ffi::c_int;
        }
        if vs == VX0 as ::core::ffi::c_int {
            emit_xmm_ld(
                b,
                VX0 as ::core::ffi::c_int,
                (*s).reg as ::core::ffi::c_uint,
            );
        }
        let mut skip_0: *mut u32 = ::core::ptr::null_mut::<u32>();
        if emit_sse_mem_addr(
            b,
            insn,
            d,
            16 as ::core::ffi::c_int,
            exit_sites,
            n_exits,
            &raw mut skip_0,
        ) == 0
        {
            return 0 as ::core::ffi::c_int;
        }
        emit_sse_mem_st(b, 16 as ::core::ffi::c_int, vs);
        patch_guard_skip(skip_0, a64_label(b));
        return 1 as ::core::ffi::c_int;
    }
    return 0 as ::core::ffi::c_int;
}
unsafe fn emit_sse_movlh(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut hi: ::core::ffi::c_int = ((*insn).op as ::core::ffi::c_int
        == OCERZ_OP_MOVHPS as ::core::ffi::c_int)
        as ::core::ffi::c_int;
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int
    {
        if xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        let mut skip: *mut u32 = ::core::ptr::null_mut::<u32>();
        l0_inval((*d).reg as ::core::ffi::c_uint);
        if emit_sse_mem_addr(
            b,
            insn,
            s,
            8 as ::core::ffi::c_int,
            exit_sites,
            n_exits,
            &raw mut skip,
        ) == 0
        {
            return 0 as ::core::ffi::c_int;
        }
        emit_sse_mem_ld(b, 8 as ::core::ffi::c_int, VX0 as ::core::ffi::c_int);
        patch_guard_skip(skip, a64_label(b));
        a64_ins_d_d(
            b,
            xmm_vreg((*d).reg as ::core::ffi::c_uint),
            if hi != 0 {
                1 as ::core::ffi::c_int
            } else {
                0 as ::core::ffi::c_int
            },
            VX0 as ::core::ffi::c_int,
            0 as ::core::ffi::c_int,
        );
        return 1 as ::core::ffi::c_int;
    }
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
    {
        let mut vs: ::core::ffi::c_int = 0;
        if hi == 0 {
            vs = if xmm_is_pinned((*s).reg as ::core::ffi::c_uint) != 0 {
                l0_src2(b, (*s).reg as ::core::ffi::c_uint, 1 as ::core::ffi::c_int)
            } else {
                VX0 as ::core::ffi::c_int
            };
        } else {
            vs = VX0 as ::core::ffi::c_int;
        }
        if vs == VX0 as ::core::ffi::c_int {
            if xmm_is_pinned((*s).reg as ::core::ffi::c_uint) != 0 {
                a64_v_dup_d(
                    b,
                    VX0 as ::core::ffi::c_int,
                    xmm_vreg((*s).reg as ::core::ffi::c_uint),
                    if hi != 0 {
                        1 as ::core::ffi::c_int
                    } else {
                        0 as ::core::ffi::c_int
                    },
                );
            } else {
                emit_xmm_ld(
                    b,
                    VX0 as ::core::ffi::c_int,
                    (*s).reg as ::core::ffi::c_uint,
                );
                if hi != 0 {
                    a64_v_dup_d(
                        b,
                        VX0 as ::core::ffi::c_int,
                        VX0 as ::core::ffi::c_int,
                        1 as ::core::ffi::c_int,
                    );
                }
            }
        }
        let mut skip_0: *mut u32 = ::core::ptr::null_mut::<u32>();
        if emit_sse_mem_addr(
            b,
            insn,
            d,
            8 as ::core::ffi::c_int,
            exit_sites,
            n_exits,
            &raw mut skip_0,
        ) == 0
        {
            return 0 as ::core::ffi::c_int;
        }
        emit_sse_mem_st(b, 8 as ::core::ffi::c_int, vs);
        patch_guard_skip(skip_0, a64_label(b));
        return 1 as ::core::ffi::c_int;
    }
    return 0 as ::core::ffi::c_int;
}
unsafe fn emit_sse_movs(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut size: ::core::ffi::c_int,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut dbl: ::core::ffi::c_int = (size == 8 as ::core::ffi::c_int) as ::core::ffi::c_int;
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
    {
        if xmm_is_pinned((*s).reg as ::core::ffi::c_uint) != 0
            && xmm_is_pinned((*d).reg as ::core::ffi::c_uint) != 0
        {
            let mut vs: ::core::ffi::c_int = l0_src2(b, (*s).reg as ::core::ffi::c_uint, dbl);
            if dbl != 0 {
                a64_ins_d_d(
                    b,
                    xmm_vreg((*d).reg as ::core::ffi::c_uint),
                    0 as ::core::ffi::c_int,
                    vs,
                    0 as ::core::ffi::c_int,
                );
            } else {
                a64_ins_s_s(
                    b,
                    xmm_vreg((*d).reg as ::core::ffi::c_uint),
                    0 as ::core::ffi::c_int,
                    vs,
                    0 as ::core::ffi::c_int,
                );
            }
            l0_share(
                (*d).reg as ::core::ffi::c_uint,
                (*s).reg as ::core::ffi::c_uint,
            );
            return 1 as ::core::ffi::c_int;
        }
        emit_xmm_ld_lo(
            b,
            size,
            VX0 as ::core::ffi::c_int,
            (*s).reg as ::core::ffi::c_uint,
        );
        emit_xmm_st_lo(
            b,
            size,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
        l0_inval((*d).reg as ::core::ffi::c_uint);
        return 1 as ::core::ffi::c_int;
    }
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int
    {
        let mut skip: *mut u32 = ::core::ptr::null_mut::<u32>();
        l0_inval((*d).reg as ::core::ffi::c_uint);
        let mut vd: ::core::ffi::c_int = if xmm_is_pinned((*d).reg as ::core::ffi::c_uint) != 0 {
            xmm_vreg((*d).reg as ::core::ffi::c_uint)
        } else {
            VX0 as ::core::ffi::c_int
        };
        if emit_sse_mem_addr(b, insn, s, size, exit_sites, n_exits, &raw mut skip) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        emit_sse_mem_ld(b, size, vd);
        patch_guard_skip(skip, a64_label(b));
        if vd == VX0 as ::core::ffi::c_int {
            emit_xmm_st(
                b,
                VX0 as ::core::ffi::c_int,
                (*d).reg as ::core::ffi::c_uint,
            );
        }
        return 1 as ::core::ffi::c_int;
    }
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
    {
        let mut vs_0: ::core::ffi::c_int = if xmm_is_pinned((*s).reg as ::core::ffi::c_uint) != 0 {
            l0_src2(b, (*s).reg as ::core::ffi::c_uint, dbl)
        } else {
            VX0 as ::core::ffi::c_int
        };
        if vs_0 == VX0 as ::core::ffi::c_int {
            emit_xmm_ld_lo(
                b,
                size,
                VX0 as ::core::ffi::c_int,
                (*s).reg as ::core::ffi::c_uint,
            );
        }
        let mut skip_0: *mut u32 = ::core::ptr::null_mut::<u32>();
        if emit_sse_mem_addr(b, insn, d, size, exit_sites, n_exits, &raw mut skip_0) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        emit_sse_mem_st(b, size, vs_0);
        patch_guard_skip(skip_0, a64_label(b));
        return 1 as ::core::ffi::c_int;
    }
    return 0 as ::core::ffi::c_int;
}
#[unsafe(no_mangle)]
pub static mut g_fpb_fast: ::core::ffi::c_int = 0;
#[unsafe(no_mangle)]
pub static mut g_cmps_mask_idx: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
unsafe fn emit_sse_fparith(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut dbl: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    let mut packed: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    let mut kind: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    match (*insn).op as ::core::ffi::c_int {
        206 => {
            kind = 0 as ::core::ffi::c_int;
        }
        207 => {
            kind = 0 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        204 => {
            kind = 0 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        205 => {
            kind = 0 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        210 => {
            kind = 1 as ::core::ffi::c_int;
        }
        211 => {
            kind = 1 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        208 => {
            kind = 1 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        209 => {
            kind = 1 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        214 => {
            kind = 2 as ::core::ffi::c_int;
        }
        215 => {
            kind = 2 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        212 => {
            kind = 2 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        213 => {
            kind = 2 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        218 => {
            kind = 3 as ::core::ffi::c_int;
        }
        219 => {
            kind = 3 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        216 => {
            kind = 3 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        217 => {
            kind = 3 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        226 => {
            kind = 4 as ::core::ffi::c_int;
        }
        227 => {
            kind = 4 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        224 => {
            kind = 4 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        225 => {
            kind = 4 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        222 => {
            kind = 5 as ::core::ffi::c_int;
        }
        223 => {
            kind = 5 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        220 => {
            kind = 5 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        221 => {
            kind = 5 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        230 => {
            kind = 6 as ::core::ffi::c_int;
        }
        231 => {
            kind = 6 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        228 => {
            kind = 6 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        229 => {
            kind = 6 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        _ => return 0 as ::core::ffi::c_int,
    }
    let mut esz: ::core::ffi::c_int = if dbl != 0 {
        8 as ::core::ffi::c_int
    } else {
        4 as ::core::ffi::c_int
    };
    static mut inexact_env: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    if inexact_env < 0 as ::core::ffi::c_int {
        inexact_env = if !libc::getenv(c"OCERZ_INEXACT_NAN".as_ptr()).is_null() {
            1 as ::core::ffi::c_int
        } else {
            0 as ::core::ffi::c_int
        };
    }
    let mut inexact_nan: ::core::ffi::c_int =
        (inexact_env != 0 || g_fpb_fast != 0 || ocerz_afp() != 0) as ::core::ffi::c_int;
    let mut vb: ::core::ffi::c_int = 0;
    let mut dst_pinned: ::core::ffi::c_int = xmm_is_pinned((*d).reg as ::core::ffi::c_uint);
    if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
        && xmm_is_pinned((*s).reg as ::core::ffi::c_uint) != 0
    {
        vb = if packed != 0 {
            xmm_vreg((*s).reg as ::core::ffi::c_uint)
        } else {
            l0_src2(b, (*s).reg as ::core::ffi::c_uint, dbl)
        };
    } else {
        if emit_sse_src(
            b,
            insn,
            s,
            if packed != 0 {
                16 as ::core::ffi::c_int
            } else {
                esz
            },
            VX1 as ::core::ffi::c_int,
            exit_sites,
            n_exits,
        ) == 0
        {
            return 0 as ::core::ffi::c_int;
        }
        vb = VX1 as ::core::ffi::c_int;
    }
    if packed != 0 {
        l0_flush_reg(b, (*d).reg as ::core::ffi::c_uint);
        if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
            l0_flush_reg(b, (*s).reg as ::core::ffi::c_uint);
        }
        l0_inval((*d).reg as ::core::ffi::c_uint);
    }
    if kind == 6 as ::core::ffi::c_int {
        if inexact_nan != 0 {
            if packed != 0 {
                let mut vd: ::core::ffi::c_int =
                    xmm_dst_reg((*d).reg as ::core::ffi::c_uint, VX2 as ::core::ffi::c_int);
                a64_v_fsqrt(b, dbl, vd, vb);
                if vd == VX2 as ::core::ffi::c_int {
                    emit_xmm_st(
                        b,
                        VX2 as ::core::ffi::c_int,
                        (*d).reg as ::core::ffi::c_uint,
                    );
                }
            } else {
                let mut t: ::core::ffi::c_int = if dst_pinned != 0 && l0_enabled() != 0 {
                    l0_alloc2(b, (*d).reg as ::core::ffi::c_uint, dbl)
                } else {
                    VX2 as ::core::ffi::c_int
                };
                if t < 0 as ::core::ffi::c_int {
                    t = VX2 as ::core::ffi::c_int;
                }
                a64_fsqrt_s(b, dbl, t, vb);
                emit_xmm_st_lo(b, esz, t, (*d).reg as ::core::ffi::c_uint);
            }
            return 1 as ::core::ffi::c_int;
        }
        if packed != 0 {
            a64_v_fsqrt(b, dbl, VX2 as ::core::ffi::c_int, vb);
            emit_nan_fix_packed2(
                b,
                dbl,
                VX2 as ::core::ffi::c_int,
                vb,
                vb,
                VX3 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
            );
            emit_xmm_st(
                b,
                VX2 as ::core::ffi::c_int,
                (*d).reg as ::core::ffi::c_uint,
            );
        } else {
            let mut t_0: ::core::ffi::c_int = if dst_pinned != 0 && l0_enabled() != 0 {
                l0_alloc2(b, (*d).reg as ::core::ffi::c_uint, dbl)
            } else {
                VX2 as ::core::ffi::c_int
            };
            if t_0 < 0 as ::core::ffi::c_int {
                t_0 = VX2 as ::core::ffi::c_int;
            }
            let mut fb: ::core::ffi::c_int = vb;
            if t_0 == vb {
                if dbl != 0 {
                    a64_fmov_d_d(b, VX3 as ::core::ffi::c_int, vb);
                } else {
                    a64_fmov_s_s(b, VX3 as ::core::ffi::c_int, vb);
                }
                fb = VX3 as ::core::ffi::c_int;
            }
            a64_fsqrt_s(b, dbl, t_0, vb);
            g_scalar_merge_next = (t_0 != VX2 as ::core::ffi::c_int
                && scalar_cvt_follows((*d).reg as ::core::ffi::c_uint, dbl) != 0)
                as ::core::ffi::c_int;
            emit_nan_fix_scalar2(b, dbl, t_0, fb, fb);
            emit_xmm_st_lo(b, esz, t_0, (*d).reg as ::core::ffi::c_uint);
        }
        return 1 as ::core::ffi::c_int;
    }
    let mut va: ::core::ffi::c_int = 0;
    if dst_pinned != 0 {
        va = if packed != 0 {
            xmm_vreg((*d).reg as ::core::ffi::c_uint)
        } else {
            l0_src2(b, (*d).reg as ::core::ffi::c_uint, dbl)
        };
    } else {
        emit_xmm_ld(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
        va = VX0 as ::core::ffi::c_int;
    }
    if kind == 4 as ::core::ffi::c_int || kind == 5 as ::core::ffi::c_int {
        if packed != 0 {
            if kind == 4 as ::core::ffi::c_int {
                a64_v_fcmgt(b, dbl, VX2 as ::core::ffi::c_int, va, vb);
            } else {
                a64_v_fcmgt(b, dbl, VX2 as ::core::ffi::c_int, vb, va);
            }
            if va == xmm_vreg((*d).reg as ::core::ffi::c_uint)
                && xmm_is_pinned((*d).reg as ::core::ffi::c_uint) != 0
            {
                a64_v_bif(b, va, vb, VX2 as ::core::ffi::c_int);
            } else {
                a64_v_bsl(b, VX2 as ::core::ffi::c_int, va, vb);
                emit_xmm_st(
                    b,
                    VX2 as ::core::ffi::c_int,
                    (*d).reg as ::core::ffi::c_uint,
                );
            }
        } else {
            let mut t_1: ::core::ffi::c_int = if dst_pinned != 0 && l0_enabled() != 0 {
                l0_alloc2(b, (*d).reg as ::core::ffi::c_uint, dbl)
            } else {
                VX2 as ::core::ffi::c_int
            };
            if t_1 < 0 as ::core::ffi::c_int {
                t_1 = VX2 as ::core::ffi::c_int;
            }
            a64_fcmp(b, dbl, va, vb);
            a64_fcsel(
                b,
                dbl,
                t_1,
                va,
                vb,
                if kind == 4 as ::core::ffi::c_int {
                    A64_GT as ::core::ffi::c_int
                } else {
                    A64_MI as ::core::ffi::c_int
                },
            );
            emit_xmm_st_lo(b, esz, t_1, (*d).reg as ::core::ffi::c_uint);
        }
        return 1 as ::core::ffi::c_int;
    }
    if inexact_nan != 0 && packed != 0 && xmm_is_pinned((*d).reg as ::core::ffi::c_uint) != 0 {
        match kind {
            0 => {
                a64_v_fadd(b, dbl, va, va, vb);
            }
            1 => {
                a64_v_fsub(b, dbl, va, va, vb);
            }
            2 => {
                a64_v_fmul(b, dbl, va, va, vb);
            }
            3 => {
                a64_v_fdiv(b, dbl, va, va, vb);
            }
            _ => {}
        }
        return 1 as ::core::ffi::c_int;
    }
    if packed != 0 {
        match kind {
            0 => {
                a64_v_fadd(b, dbl, VX2 as ::core::ffi::c_int, va, vb);
            }
            1 => {
                a64_v_fsub(b, dbl, VX2 as ::core::ffi::c_int, va, vb);
            }
            2 => {
                a64_v_fmul(b, dbl, VX2 as ::core::ffi::c_int, va, vb);
            }
            3 => {
                a64_v_fdiv(b, dbl, VX2 as ::core::ffi::c_int, va, vb);
            }
            _ => {}
        }
        emit_nan_fix_packed2(
            b,
            dbl,
            VX2 as ::core::ffi::c_int,
            va,
            vb,
            VX3 as ::core::ffi::c_int,
            VX3 as ::core::ffi::c_int,
        );
        emit_xmm_st(
            b,
            VX2 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    } else if inexact_nan != 0 {
        let mut t_2: ::core::ffi::c_int = if dst_pinned != 0 && l0_enabled() != 0 {
            l0_alloc2(b, (*d).reg as ::core::ffi::c_uint, dbl)
        } else {
            VX2 as ::core::ffi::c_int
        };
        if t_2 < 0 as ::core::ffi::c_int {
            t_2 = VX2 as ::core::ffi::c_int;
        }
        match kind {
            0 => {
                a64_fadd_s(b, dbl, t_2, va, vb);
            }
            1 => {
                a64_fsub_s(b, dbl, t_2, va, vb);
            }
            2 => {
                a64_fmul_s(b, dbl, t_2, va, vb);
            }
            3 => {
                a64_fdiv_s(b, dbl, t_2, va, vb);
            }
            _ => {}
        }
        emit_xmm_st_lo(b, esz, t_2, (*d).reg as ::core::ffi::c_uint);
    } else {
        let mut t_3: ::core::ffi::c_int = if dst_pinned != 0 && l0_enabled() != 0 {
            l0_alloc2(b, (*d).reg as ::core::ffi::c_uint, dbl)
        } else {
            VX2 as ::core::ffi::c_int
        };
        if t_3 < 0 as ::core::ffi::c_int {
            t_3 = VX2 as ::core::ffi::c_int;
        }
        let mut fa: ::core::ffi::c_int = va;
        let mut fb_0: ::core::ffi::c_int = vb;
        if t_3 == va || t_3 == vb {
            let mut alias: ::core::ffi::c_int = if t_3 == va { va } else { vb };
            if dbl != 0 {
                a64_fmov_d_d(b, VX3 as ::core::ffi::c_int, alias);
            } else {
                a64_fmov_s_s(b, VX3 as ::core::ffi::c_int, alias);
            }
            if t_3 == va {
                fa = VX3 as ::core::ffi::c_int;
            }
            if t_3 == vb {
                fb_0 = VX3 as ::core::ffi::c_int;
            }
        }
        match kind {
            0 => {
                a64_fadd_s(b, dbl, t_3, va, vb);
            }
            1 => {
                a64_fsub_s(b, dbl, t_3, va, vb);
            }
            2 => {
                a64_fmul_s(b, dbl, t_3, va, vb);
            }
            3 => {
                a64_fdiv_s(b, dbl, t_3, va, vb);
            }
            _ => {}
        }
        g_scalar_merge_next = (t_3 != VX2 as ::core::ffi::c_int
            && scalar_cvt_follows((*d).reg as ::core::ffi::c_uint, dbl) != 0)
            as ::core::ffi::c_int;
        emit_nan_fix_scalar2(b, dbl, t_3, fa, fb_0);
        emit_xmm_st_lo(b, esz, t_3, (*d).reg as ::core::ffi::c_uint);
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn sse_int_kind(
    mut op: ::core::ffi::c_uint,
    mut esz: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    *esz = 0 as ::core::ffi::c_int;
    match op {
        249 | 245 => return SIK_XOR as ::core::ffi::c_int,
        246 | 242 => return SIK_AND as ::core::ffi::c_int,
        248 | 244 => return SIK_OR as ::core::ffi::c_int,
        247 | 243 => return SIK_ANDN as ::core::ffi::c_int,
        282 => return SIK_ADD as ::core::ffi::c_int,
        283 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_ADD as ::core::ffi::c_int;
        }
        284 => {
            *esz = 2 as ::core::ffi::c_int;
            return SIK_ADD as ::core::ffi::c_int;
        }
        285 => {
            *esz = 3 as ::core::ffi::c_int;
            return SIK_ADD as ::core::ffi::c_int;
        }
        286 => return SIK_SUB as ::core::ffi::c_int,
        287 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_SUB as ::core::ffi::c_int;
        }
        288 => {
            *esz = 2 as ::core::ffi::c_int;
            return SIK_SUB as ::core::ffi::c_int;
        }
        289 => {
            *esz = 3 as ::core::ffi::c_int;
            return SIK_SUB as ::core::ffi::c_int;
        }
        258 => return SIK_CMPEQ as ::core::ffi::c_int,
        259 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_CMPEQ as ::core::ffi::c_int;
        }
        260 => {
            *esz = 2 as ::core::ffi::c_int;
            return SIK_CMPEQ as ::core::ffi::c_int;
        }
        261 => {
            *esz = 3 as ::core::ffi::c_int;
            return SIK_CMPEQ as ::core::ffi::c_int;
        }
        262 => return SIK_CMPGT as ::core::ffi::c_int,
        263 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_CMPGT as ::core::ffi::c_int;
        }
        264 => {
            *esz = 2 as ::core::ffi::c_int;
            return SIK_CMPGT as ::core::ffi::c_int;
        }
        265 => {
            *esz = 3 as ::core::ffi::c_int;
            return SIK_CMPGT as ::core::ffi::c_int;
        }
        308 => return SIK_UMIN as ::core::ffi::c_int,
        316 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_UMIN as ::core::ffi::c_int;
        }
        317 => {
            *esz = 2 as ::core::ffi::c_int;
            return SIK_UMIN as ::core::ffi::c_int;
        }
        306 => return SIK_UMAX as ::core::ffi::c_int,
        312 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_UMAX as ::core::ffi::c_int;
        }
        313 => {
            *esz = 2 as ::core::ffi::c_int;
            return SIK_UMAX as ::core::ffi::c_int;
        }
        314 => return SIK_SMIN as ::core::ffi::c_int,
        309 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_SMIN as ::core::ffi::c_int;
        }
        315 => {
            *esz = 2 as ::core::ffi::c_int;
            return SIK_SMIN as ::core::ffi::c_int;
        }
        310 => return SIK_SMAX as ::core::ffi::c_int,
        307 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_SMAX as ::core::ffi::c_int;
        }
        311 => {
            *esz = 2 as ::core::ffi::c_int;
            return SIK_SMAX as ::core::ffi::c_int;
        }
        298 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_MUL as ::core::ffi::c_int;
        }
        299 => {
            *esz = 2 as ::core::ffi::c_int;
            return SIK_MUL as ::core::ffi::c_int;
        }
        292 => return SIK_UQADD as ::core::ffi::c_int,
        293 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_UQADD as ::core::ffi::c_int;
        }
        296 => return SIK_UQSUB as ::core::ffi::c_int,
        297 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_UQSUB as ::core::ffi::c_int;
        }
        290 => return SIK_SQADD as ::core::ffi::c_int,
        291 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_SQADD as ::core::ffi::c_int;
        }
        294 => return SIK_SQSUB as ::core::ffi::c_int,
        295 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_SQSUB as ::core::ffi::c_int;
        }
        304 => return SIK_AVG as ::core::ffi::c_int,
        305 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_AVG as ::core::ffi::c_int;
        }
        303 => return SIK_MADDWD as ::core::ffi::c_int,
        348 => return SIK_MULHRSW as ::core::ffi::c_int,
        323 => return SIK_PACKSSDW as ::core::ffi::c_int,
        324 => return SIK_PACKUSWB as ::core::ffi::c_int,
        302 => return SIK_MULUDQ as ::core::ffi::c_int,
        300 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_MULHW as ::core::ffi::c_int;
        }
        301 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_MULHUW as ::core::ffi::c_int;
        }
        322 => return SIK_PACKSSWB as ::core::ffi::c_int,
        325 => return SIK_PACKUSDW as ::core::ffi::c_int,
        421 => return SIK_MULDQ as ::core::ffi::c_int,
        318 => return SIK_SADBW as ::core::ffi::c_int,
        347 => return SIK_MADDUBSW as ::core::ffi::c_int,
        319 => return SIK_ABS as ::core::ffi::c_int,
        320 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_ABS as ::core::ffi::c_int;
        }
        321 => {
            *esz = 2 as ::core::ffi::c_int;
            return SIK_ABS as ::core::ffi::c_int;
        }
        344 => return SIK_SIGN as ::core::ffi::c_int,
        345 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_SIGN as ::core::ffi::c_int;
        }
        346 => {
            *esz = 2 as ::core::ffi::c_int;
            return SIK_SIGN as ::core::ffi::c_int;
        }
        338 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_HADD as ::core::ffi::c_int;
        }
        339 => {
            *esz = 2 as ::core::ffi::c_int;
            return SIK_HADD as ::core::ffi::c_int;
        }
        341 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_HSUB as ::core::ffi::c_int;
        }
        342 => {
            *esz = 2 as ::core::ffi::c_int;
            return SIK_HSUB as ::core::ffi::c_int;
        }
        340 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_HADDS as ::core::ffi::c_int;
        }
        343 => {
            *esz = 1 as ::core::ffi::c_int;
            return SIK_HSUBS as ::core::ffi::c_int;
        }
        _ => return 0 as ::core::ffi::c_int,
    };
}
unsafe fn sse_int_self_zero(mut kind: ::core::ffi::c_int) -> ::core::ffi::c_int {
    return (kind == SIK_XOR as ::core::ffi::c_int
        || kind == SIK_ANDN as ::core::ffi::c_int
        || kind == SIK_SUB as ::core::ffi::c_int
        || kind == SIK_CMPGT as ::core::ffi::c_int
        || kind == SIK_UQSUB as ::core::ffi::c_int
        || kind == SIK_SQSUB as ::core::ffi::c_int) as ::core::ffi::c_int;
}
unsafe fn emit_sse_int_op(
    mut b: *mut A64Buf,
    mut kind: ::core::ffi::c_int,
    mut esz: ::core::ffi::c_int,
    mut vd: ::core::ffi::c_int,
    mut va: ::core::ffi::c_int,
    mut vb: ::core::ffi::c_int,
) {
    match kind {
        1 => {
            a64_v_eor(b, vd, va, vb);
        }
        2 => {
            a64_v_and(b, vd, va, vb);
        }
        3 => {
            a64_v_orr(b, vd, va, vb);
        }
        4 => {
            a64_v_bic(b, vd, vb, va);
        }
        5 => {
            a64_v_add(b, esz, vd, va, vb);
        }
        6 => {
            a64_v_sub(b, esz, vd, va, vb);
        }
        7 => {
            a64_v_cmeq(b, esz, vd, va, vb);
        }
        8 => {
            a64_v_cmgt(b, esz, vd, va, vb);
        }
        9 => {
            a64_v_umin(b, esz, vd, va, vb);
        }
        10 => {
            a64_v_umax(b, esz, vd, va, vb);
        }
        11 => {
            a64_v_smin(b, esz, vd, va, vb);
        }
        12 => {
            a64_v_smax(b, esz, vd, va, vb);
        }
        13 => {
            a64_v_mul(b, esz, vd, va, vb);
        }
        14 => {
            a64_v_uqadd(b, esz, vd, va, vb);
        }
        15 => {
            a64_v_uqsub(b, esz, vd, va, vb);
        }
        16 => {
            a64_v_sqadd(b, esz, vd, va, vb);
        }
        17 => {
            a64_v_sqsub(b, esz, vd, va, vb);
        }
        19 => {
            a64_v_smull_h(
                b,
                0 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                va,
                vb,
            );
            a64_v_smull_h(
                b,
                1 as ::core::ffi::c_int,
                VX3 as ::core::ffi::c_int,
                va,
                vb,
            );
            a64_v_addp_4s(b, vd, VX2 as ::core::ffi::c_int, VX3 as ::core::ffi::c_int);
        }
        20 => {
            a64_v_smull_h(
                b,
                0 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                va,
                vb,
            );
            a64_v_smull_h(
                b,
                1 as ::core::ffi::c_int,
                VX3 as ::core::ffi::c_int,
                va,
                vb,
            );
            a64_v_rshrn_s15(b, 0 as ::core::ffi::c_int, vd, VX2 as ::core::ffi::c_int);
            a64_v_rshrn_s15(b, 1 as ::core::ffi::c_int, vd, VX3 as ::core::ffi::c_int);
        }
        21 => {
            a64_v_sqxtn_s(b, 0 as ::core::ffi::c_int, VX2 as ::core::ffi::c_int, va);
            a64_v_sqxtn_s(b, 1 as ::core::ffi::c_int, VX2 as ::core::ffi::c_int, vb);
            a64_v_mov(b, vd, VX2 as ::core::ffi::c_int);
        }
        22 => {
            a64_v_sqxtun_h(b, 0 as ::core::ffi::c_int, VX2 as ::core::ffi::c_int, va);
            a64_v_sqxtun_h(b, 1 as ::core::ffi::c_int, VX2 as ::core::ffi::c_int, vb);
            a64_v_mov(b, vd, VX2 as ::core::ffi::c_int);
        }
        23 => {
            a64_v_xtn(b, 2 as ::core::ffi::c_int, VX2 as ::core::ffi::c_int, va);
            a64_v_xtn(b, 2 as ::core::ffi::c_int, VX3 as ::core::ffi::c_int, vb);
            a64_v_umull_s(b, vd, VX2 as ::core::ffi::c_int, VX3 as ::core::ffi::c_int);
        }
        24 | 25 => {
            if kind == SIK_MULHW as ::core::ffi::c_int {
                a64_v_smull_h(
                    b,
                    0 as ::core::ffi::c_int,
                    VX2 as ::core::ffi::c_int,
                    va,
                    vb,
                );
                a64_v_smull_h(
                    b,
                    1 as ::core::ffi::c_int,
                    VX3 as ::core::ffi::c_int,
                    va,
                    vb,
                );
            } else {
                a64_v_umull_h(
                    b,
                    0 as ::core::ffi::c_int,
                    VX2 as ::core::ffi::c_int,
                    va,
                    vb,
                );
                a64_v_umull_h(
                    b,
                    1 as ::core::ffi::c_int,
                    VX3 as ::core::ffi::c_int,
                    va,
                    vb,
                );
            }
            a64_v_uzp(
                b,
                1 as ::core::ffi::c_int,
                1 as ::core::ffi::c_int,
                vd,
                VX2 as ::core::ffi::c_int,
                VX3 as ::core::ffi::c_int,
            );
        }
        26 => {
            a64_v_sqxtn_h(b, 0 as ::core::ffi::c_int, VX2 as ::core::ffi::c_int, va);
            a64_v_sqxtn_h(b, 1 as ::core::ffi::c_int, VX2 as ::core::ffi::c_int, vb);
            a64_v_mov(b, vd, VX2 as ::core::ffi::c_int);
        }
        27 => {
            a64_v_sqxtun_s(b, 0 as ::core::ffi::c_int, VX2 as ::core::ffi::c_int, va);
            a64_v_sqxtun_s(b, 1 as ::core::ffi::c_int, VX2 as ::core::ffi::c_int, vb);
            a64_v_mov(b, vd, VX2 as ::core::ffi::c_int);
        }
        28 => {
            a64_v_xtn(b, 2 as ::core::ffi::c_int, VX2 as ::core::ffi::c_int, va);
            a64_v_xtn(b, 2 as ::core::ffi::c_int, VX3 as ::core::ffi::c_int, vb);
            a64_v_smull_s(b, vd, VX2 as ::core::ffi::c_int, VX3 as ::core::ffi::c_int);
        }
        29 => {
            a64_v_uabd(
                b,
                0 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                va,
                vb,
            );
            a64_v_uaddlp(
                b,
                0 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
            );
            a64_v_uaddlp(
                b,
                1 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
            );
            a64_v_uaddlp(b, 2 as ::core::ffi::c_int, vd, VX2 as ::core::ffi::c_int);
        }
        30 => {
            a64_v_xtl(
                b,
                0 as ::core::ffi::c_int,
                1 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                va,
            );
            a64_v_xtl(
                b,
                1 as ::core::ffi::c_int,
                1 as ::core::ffi::c_int,
                VX3 as ::core::ffi::c_int,
                vb,
            );
            a64_v_mul(
                b,
                1 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                VX3 as ::core::ffi::c_int,
            );
            a64_v_xtl2(
                b,
                1 as ::core::ffi::c_int,
                1 as ::core::ffi::c_int,
                VX3 as ::core::ffi::c_int,
                vb,
            );
            a64_v_xtl2(b, 0 as ::core::ffi::c_int, 1 as ::core::ffi::c_int, vd, va);
            a64_v_mul(
                b,
                1 as ::core::ffi::c_int,
                vd,
                vd,
                VX3 as ::core::ffi::c_int,
            );
            a64_v_uzp(
                b,
                1 as ::core::ffi::c_int,
                1 as ::core::ffi::c_int,
                VX3 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                vd,
            );
            a64_v_uzp(
                b,
                1 as ::core::ffi::c_int,
                0 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                vd,
            );
            a64_v_sqadd(
                b,
                1 as ::core::ffi::c_int,
                vd,
                VX2 as ::core::ffi::c_int,
                VX3 as ::core::ffi::c_int,
            );
        }
        31 => {
            a64_v_abs(b, esz, vd, vb);
        }
        32 => {
            a64_v_sshr_imm(
                b,
                esz,
                VX2 as ::core::ffi::c_int,
                vb,
                ((8 as ::core::ffi::c_int) << esz) - 1 as ::core::ffi::c_int,
            );
            a64_v_neg(b, esz, VX3 as ::core::ffi::c_int, va);
            a64_v_bsl(b, VX2 as ::core::ffi::c_int, VX3 as ::core::ffi::c_int, va);
            a64_v_cmeq0(b, esz, VX3 as ::core::ffi::c_int, vb);
            a64_v_bic(b, vd, VX2 as ::core::ffi::c_int, VX3 as ::core::ffi::c_int);
        }
        33 => {
            a64_v_addp(b, esz, vd, va, vb);
        }
        34 | 35 | 36 => {
            a64_v_uzp(
                b,
                esz,
                0 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                va,
                vb,
            );
            a64_v_uzp(
                b,
                esz,
                1 as ::core::ffi::c_int,
                VX3 as ::core::ffi::c_int,
                va,
                vb,
            );
            if kind == SIK_HSUB as ::core::ffi::c_int {
                a64_v_sub(
                    b,
                    esz,
                    vd,
                    VX2 as ::core::ffi::c_int,
                    VX3 as ::core::ffi::c_int,
                );
            } else if kind == SIK_HADDS as ::core::ffi::c_int {
                a64_v_sqadd(
                    b,
                    esz,
                    vd,
                    VX2 as ::core::ffi::c_int,
                    VX3 as ::core::ffi::c_int,
                );
            } else {
                a64_v_sqsub(
                    b,
                    esz,
                    vd,
                    VX2 as ::core::ffi::c_int,
                    VX3 as ::core::ffi::c_int,
                );
            }
        }
        _ => {
            a64_v_urhadd(b, esz, vd, va, vb);
        }
    };
}
unsafe fn emit_sse_bitwise(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut esz: ::core::ffi::c_int = 0;
    let mut kind: ::core::ffi::c_int =
        sse_int_kind((*insn).op as ::core::ffi::c_uint, &raw mut esz);
    if kind == 0 {
        return 0 as ::core::ffi::c_int;
    }
    if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
        && (*s).reg as ::core::ffi::c_int == (*d).reg as ::core::ffi::c_int
        && sse_int_self_zero(kind) != 0
    {
        let mut vd: ::core::ffi::c_int =
            xmm_dst_reg((*d).reg as ::core::ffi::c_uint, VX0 as ::core::ffi::c_int);
        a64_v_zero(b, vd);
        if vd == VX0 as ::core::ffi::c_int {
            emit_xmm_st(
                b,
                VX0 as ::core::ffi::c_int,
                (*d).reg as ::core::ffi::c_uint,
            );
        }
        return 1 as ::core::ffi::c_int;
    }
    let mut vb: ::core::ffi::c_int = emit_sse_src_reg(
        b,
        insn,
        s,
        16 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    );
    if vb < 0 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut vd_0: ::core::ffi::c_int =
        xmm_dst_reg((*d).reg as ::core::ffi::c_uint, VX0 as ::core::ffi::c_int);
    if vd_0 == VX0 as ::core::ffi::c_int {
        emit_xmm_ld(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    emit_sse_int_op(b, kind, esz, vd_0, vd_0, vb);
    if vd_0 == VX0 as ::core::ffi::c_int {
        emit_xmm_st(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_sse_pblendw_palignr(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int || sse_enabled() == 0 {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || (*insn).vex as ::core::ffi::c_int != 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut imm: ::core::ffi::c_uint = (*insn).ops[2 as ::core::ffi::c_int as usize].imm
        as ::core::ffi::c_uint
        & 0xff as ::core::ffi::c_uint;
    let mut vb: ::core::ffi::c_int = emit_sse_src_reg(
        b,
        insn,
        s,
        16 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    );
    if vb < 0 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut vd: ::core::ffi::c_int =
        xmm_dst_reg((*d).reg as ::core::ffi::c_uint, VX0 as ::core::ffi::c_int);
    if vd == VX0 as ::core::ffi::c_int {
        emit_xmm_ld(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    if (*insn).op as ::core::ffi::c_int == OCERZ_OP_PBLENDW as ::core::ffi::c_int {
        let mut i: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
        while i < 8 as ::core::ffi::c_int {
            if imm & (1 as ::core::ffi::c_uint) << i != 0 {
                a64_ins_h_h(b, vd, i, vb, i);
            }
            i += 1;
        }
    } else if imm < 16 as ::core::ffi::c_uint {
        a64_v_ext(
            b,
            VX2 as ::core::ffi::c_int,
            vb,
            vd,
            imm as ::core::ffi::c_int,
        );
        a64_v_mov(b, vd, VX2 as ::core::ffi::c_int);
    } else if imm < 32 as ::core::ffi::c_uint {
        a64_v_zero(b, VX3 as ::core::ffi::c_int);
        a64_v_ext(
            b,
            VX2 as ::core::ffi::c_int,
            vd,
            VX3 as ::core::ffi::c_int,
            imm.wrapping_sub(16 as ::core::ffi::c_uint) as ::core::ffi::c_int,
        );
        a64_v_mov(b, vd, VX2 as ::core::ffi::c_int);
    } else {
        a64_v_zero(b, vd);
    }
    if vd == VX0 as ::core::ffi::c_int {
        emit_xmm_st(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_sse_pshuflhw(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int
        || (*insn).ops[2 as ::core::ffi::c_int as usize].kind as ::core::ffi::c_int
            != OCERZ_OPK_IMM as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || (*s).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
            && (*s).kind as ::core::ffi::c_int != OCERZ_OPK_MEM as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut imm: ::core::ffi::c_uint = (*insn).ops[2 as ::core::ffi::c_int as usize].imm
        as ::core::ffi::c_uint
        & 0xff as ::core::ffi::c_uint;
    let mut base: ::core::ffi::c_int =
        if (*insn).op as ::core::ffi::c_int == OCERZ_OP_PSHUFHW as ::core::ffi::c_int {
            4 as ::core::ffi::c_int
        } else {
            0 as ::core::ffi::c_int
        };
    let mut vs: ::core::ffi::c_int = emit_sse_src_reg(
        b,
        insn,
        s,
        16 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    );
    if vs < 0 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut vd: ::core::ffi::c_int =
        xmm_dst_reg((*d).reg as ::core::ffi::c_uint, VX0 as ::core::ffi::c_int);
    let mut from: ::core::ffi::c_int = vs;
    if vs == vd {
        a64_v_mov(b, VX2 as ::core::ffi::c_int, vs);
        from = VX2 as ::core::ffi::c_int;
    } else {
        a64_v_mov(b, vd, vs);
    }
    let mut i: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    while i < 4 as ::core::ffi::c_int {
        let mut sel: ::core::ffi::c_int =
            (imm >> 2 as ::core::ffi::c_int * i & 3 as ::core::ffi::c_uint) as ::core::ffi::c_int;
        if !(from == VX2 as ::core::ffi::c_int && sel == i) {
            if !(from != VX2 as ::core::ffi::c_int && sel == i) {
                a64_ins_h_h(b, vd, base + i, from, base + sel);
            }
        }
        i += 1;
    }
    if vd == VX0 as ::core::ffi::c_int {
        emit_xmm_st(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_sse_bytesh(mut b: *mut A64Buf, mut insn: *const X86Insn) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut c: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int
        || (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || (*c).kind as ::core::ffi::c_int != OCERZ_OPK_IMM as ::core::ffi::c_int
        || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut n: ::core::ffi::c_uint = (*c).imm as ::core::ffi::c_uint & 0xff as ::core::ffi::c_uint;
    let mut vd: ::core::ffi::c_int = xmm_vreg((*d).reg as ::core::ffi::c_uint);
    if n == 0 as ::core::ffi::c_uint {
        return 1 as ::core::ffi::c_int;
    }
    if n >= 16 as ::core::ffi::c_uint {
        a64_v_zero(b, vd);
        return 1 as ::core::ffi::c_int;
    }
    a64_v_zero(b, VX2 as ::core::ffi::c_int);
    if (*insn).op as ::core::ffi::c_int == OCERZ_OP_PSRLDQ as ::core::ffi::c_int {
        a64_v_ext(
            b,
            vd,
            vd,
            VX2 as ::core::ffi::c_int,
            n as ::core::ffi::c_int,
        );
    } else {
        a64_v_ext(
            b,
            vd,
            VX2 as ::core::ffi::c_int,
            vd,
            (16 as ::core::ffi::c_uint).wrapping_sub(n) as ::core::ffi::c_int,
        );
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_sse_blendp(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int
        || (*insn).ops[2 as ::core::ffi::c_int as usize].kind as ::core::ffi::c_int
            != OCERZ_OPK_IMM as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut imm: ::core::ffi::c_uint = (*insn).ops[2 as ::core::ffi::c_int as usize].imm
        as ::core::ffi::c_uint
        & 0xff as ::core::ffi::c_uint;
    let mut vb: ::core::ffi::c_int = emit_sse_src_reg(
        b,
        insn,
        s,
        16 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    );
    if vb < 0 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut vd: ::core::ffi::c_int =
        xmm_dst_reg((*d).reg as ::core::ffi::c_uint, VX0 as ::core::ffi::c_int);
    if vd == VX0 as ::core::ffi::c_int {
        emit_xmm_ld(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    if (*insn).op as ::core::ffi::c_int == OCERZ_OP_BLENDPD as ::core::ffi::c_int {
        let mut i: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
        while i < 2 as ::core::ffi::c_int {
            if imm & (1 as ::core::ffi::c_uint) << i != 0 {
                a64_ins_d_d(b, vd, i, vb, i);
            }
            i += 1;
        }
    } else {
        let mut i_0: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
        while i_0 < 4 as ::core::ffi::c_int {
            if imm & (1 as ::core::ffi::c_uint) << i_0 != 0 {
                a64_ins_s_s(b, vd, i_0, vb, i_0);
            }
            i_0 += 1;
        }
    }
    if vd == VX0 as ::core::ffi::c_int {
        emit_xmm_st(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_sse_movsdup(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut vs: ::core::ffi::c_int = emit_sse_src_reg(
        b,
        insn,
        s,
        16 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    );
    if vs < 0 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut vd: ::core::ffi::c_int =
        xmm_dst_reg((*d).reg as ::core::ffi::c_uint, VX0 as ::core::ffi::c_int);
    a64_v_trn(
        b,
        2 as ::core::ffi::c_int,
        ((*insn).op as ::core::ffi::c_int == OCERZ_OP_MOVSHDUP as ::core::ffi::c_int)
            as ::core::ffi::c_int,
        vd,
        vs,
        vs,
    );
    if vd == VX0 as ::core::ffi::c_int {
        emit_xmm_st(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_sse_movmskp(mut b: *mut A64Buf, mut insn: *const X86Insn) -> ::core::ffi::c_int {
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int
        || g_n_raslit >= RASLIT_MAX as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_REG as ::core::ffi::c_int
        || (*d).high8 as ::core::ffi::c_int != 0
        || (*d).size as ::core::ffi::c_int != 4 as ::core::ffi::c_int
            && (*d).size as ::core::ffi::c_int != 8 as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    if (*s).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut ds: ::core::ffi::c_int = pin_slot((*d).reg as ::core::ffi::c_uint);
    if ds < 0 as ::core::ffi::c_int
        || rsp_is_ptr() != 0 && (*d).reg as ::core::ffi::c_int == OCERZ_RSP as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    l0_flush_reg(b, (*s).reg as ::core::ffi::c_uint);
    let mut dbl: ::core::ffi::c_int = ((*insn).op as ::core::ffi::c_int
        == OCERZ_OP_MOVMSKPD as ::core::ffi::c_int)
        as ::core::ffi::c_int;
    g_raslit[g_n_raslit as usize].site = a64_label(b);
    g_raslit[g_n_raslit as usize].retaddr = (if dbl != 0 {
        1 as ::core::ffi::c_ulonglong
    } else {
        0x200000001 as ::core::ffi::c_ulonglong
    }) as u64;
    g_raslit[g_n_raslit as usize].hi = (if dbl != 0 {
        2 as ::core::ffi::c_ulonglong
    } else {
        0x800000004 as ::core::ffi::c_ulonglong
    }) as u64;
    g_raslit[g_n_raslit as usize].kind = 2 as ::core::ffi::c_int;
    g_raslit[g_n_raslit as usize].rt = VX1 as ::core::ffi::c_int;
    g_n_raslit += 1;
    a64_emit32(b, 0x9c000000 as u32 | VX1 as ::core::ffi::c_int as u32);
    if dbl != 0 {
        a64_v_sshr_2d(
            b,
            VX0 as ::core::ffi::c_int,
            xmm_vreg((*s).reg as ::core::ffi::c_uint),
            63 as ::core::ffi::c_int,
        );
    } else {
        a64_v_sshr_4s(
            b,
            VX0 as ::core::ffi::c_int,
            xmm_vreg((*s).reg as ::core::ffi::c_uint),
            31 as ::core::ffi::c_int,
        );
    }
    a64_v_and(
        b,
        VX0 as ::core::ffi::c_int,
        VX0 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
    );
    if dbl != 0 {
        a64_addp_d(b, VX0 as ::core::ffi::c_int, VX0 as ::core::ffi::c_int);
    } else {
        a64_v_addv_4s(b, VX0 as ::core::ffi::c_int, VX0 as ::core::ffi::c_int);
    }
    a64_fmov_x_from_v(
        b,
        0 as ::core::ffi::c_int,
        pin_hreg(ds),
        VX0 as ::core::ffi::c_int,
    );
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_sse_cmpp(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int
        || (*insn).ops[2 as ::core::ffi::c_int as usize].kind as ::core::ffi::c_int
            != OCERZ_OPK_IMM as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut dbl: ::core::ffi::c_int = ((*insn).op as ::core::ffi::c_int
        == OCERZ_OP_CMPPD as ::core::ffi::c_int)
        as ::core::ffi::c_int;
    let mut pred: ::core::ffi::c_uint = (*insn).ops[2 as ::core::ffi::c_int as usize].imm
        as ::core::ffi::c_uint
        & 7 as ::core::ffi::c_uint;
    let mut vb: ::core::ffi::c_int = emit_sse_src_reg(
        b,
        insn,
        s,
        16 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    );
    if vb < 0 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut vd: ::core::ffi::c_int =
        xmm_dst_reg((*d).reg as ::core::ffi::c_uint, VX0 as ::core::ffi::c_int);
    if vd == VX0 as ::core::ffi::c_int {
        emit_xmm_ld(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    match pred {
        0 => {
            a64_v_fcmeq(b, dbl, vd, vd, vb);
        }
        1 => {
            a64_v_fcmgt(b, dbl, vd, vb, vd);
        }
        2 => {
            a64_v_fcmge(b, dbl, vd, vb, vd);
        }
        3 => {
            a64_v_fcmeq(b, dbl, VX2 as ::core::ffi::c_int, vd, vd);
            a64_v_fcmeq(b, dbl, VX3 as ::core::ffi::c_int, vb, vb);
            a64_v_and(
                b,
                VX2 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                VX3 as ::core::ffi::c_int,
            );
            a64_v_not(b, vd, VX2 as ::core::ffi::c_int);
        }
        4 => {
            a64_v_fcmeq(b, dbl, vd, vd, vb);
            a64_v_not(b, vd, vd);
        }
        5 => {
            a64_v_fcmgt(b, dbl, vd, vb, vd);
            a64_v_not(b, vd, vd);
        }
        6 => {
            a64_v_fcmge(b, dbl, vd, vb, vd);
            a64_v_not(b, vd, vd);
        }
        _ => {
            a64_v_fcmeq(b, dbl, VX2 as ::core::ffi::c_int, vd, vd);
            a64_v_fcmeq(b, dbl, VX3 as ::core::ffi::c_int, vb, vb);
            a64_v_and(b, vd, VX2 as ::core::ffi::c_int, VX3 as ::core::ffi::c_int);
        }
    }
    if vd == VX0 as ::core::ffi::c_int {
        emit_xmm_st(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_sse_cvtp(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut half: ::core::ffi::c_int = ((*insn).op as ::core::ffi::c_int
        == OCERZ_OP_CVTDQ2PD as ::core::ffi::c_int
        || (*insn).op as ::core::ffi::c_int == OCERZ_OP_CVTPS2PD as ::core::ffi::c_int)
        as ::core::ffi::c_int;
    let mut vs: ::core::ffi::c_int = emit_sse_src_reg(
        b,
        insn,
        s,
        if half != 0 {
            8 as ::core::ffi::c_int
        } else {
            16 as ::core::ffi::c_int
        },
        VX1 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    );
    if vs < 0 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut vd: ::core::ffi::c_int =
        xmm_dst_reg((*d).reg as ::core::ffi::c_uint, VX0 as ::core::ffi::c_int);
    if (*insn).op as ::core::ffi::c_int == OCERZ_OP_CVTDQ2PD as ::core::ffi::c_int {
        a64_v_xtl(
            b,
            1 as ::core::ffi::c_int,
            4 as ::core::ffi::c_int,
            VX2 as ::core::ffi::c_int,
            vs,
        );
        a64_v_scvtf_2d(b, vd, VX2 as ::core::ffi::c_int);
    } else if (*insn).op as ::core::ffi::c_int == OCERZ_OP_CVTPS2PD as ::core::ffi::c_int {
        a64_v_fcvtl(b, vd, vs);
    } else if (*insn).op as ::core::ffi::c_int == OCERZ_OP_CVTPD2PS as ::core::ffi::c_int {
        a64_v_fcvtn(b, vd, vs);
    } else {
        let mut src: ::core::ffi::c_int = vs;
        if (*insn).op as ::core::ffi::c_int == OCERZ_OP_CVTPS2DQ as ::core::ffi::c_int {
            a64_v_frint(
                b,
                0 as ::core::ffi::c_int,
                4 as ::core::ffi::c_int,
                VX3 as ::core::ffi::c_int,
                vs,
            );
            src = VX3 as ::core::ffi::c_int;
        }
        a64_v_movi_s_lsl24(b, VX2 as ::core::ffi::c_int, 0x4f as ::core::ffi::c_uint);
        a64_v_fcmgt(
            b,
            0 as ::core::ffi::c_int,
            VX2 as ::core::ffi::c_int,
            VX2 as ::core::ffi::c_int,
            src,
        );
        a64_v_fcvtzs_4s(b, VX3 as ::core::ffi::c_int, src);
        a64_v_movi_s_lsl24(b, vd, 0x80 as ::core::ffi::c_uint);
        a64_v_bit(b, vd, VX3 as ::core::ffi::c_int, VX2 as ::core::ffi::c_int);
    }
    if vd == VX0 as ::core::ffi::c_int {
        emit_xmm_st(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_v_literal(mut b: *mut A64Buf, mut vt: ::core::ffi::c_int, mut lo: u64, mut hi: u64) {
    g_raslit[g_n_raslit as usize].site = a64_label(b);
    g_raslit[g_n_raslit as usize].retaddr = lo;
    g_raslit[g_n_raslit as usize].hi = hi;
    g_raslit[g_n_raslit as usize].kind = 2 as ::core::ffi::c_int;
    g_raslit[g_n_raslit as usize].rt = vt;
    g_n_raslit += 1;
    a64_emit32(b, 0x9c000000 as u32 | vt as u32);
}
unsafe fn emit_sse_aes(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut op: ::core::ffi::c_uint = (*insn).op as ::core::ffi::c_uint;
    let mut kga: ::core::ffi::c_int = (op
        == OCERZ_OP_AESKEYGENASSIST as ::core::ffi::c_int as ::core::ffi::c_uint)
        as ::core::ffi::c_int;
    if (*insn).nops as ::core::ffi::c_int
        != (if kga != 0 {
            3 as ::core::ffi::c_int
        } else {
            2 as ::core::ffi::c_int
        })
    {
        return 0 as ::core::ffi::c_int;
    }
    if kga != 0
        && ((*insn).ops[2 as ::core::ffi::c_int as usize].kind as ::core::ffi::c_int
            != OCERZ_OPK_IMM as ::core::ffi::c_int
            || g_n_raslit + 2 as ::core::ffi::c_int > RASLIT_MAX as ::core::ffi::c_int)
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut vb: ::core::ffi::c_int = emit_sse_src_reg(
        b,
        insn,
        s,
        16 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    );
    if vb < 0 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut vd: ::core::ffi::c_int =
        xmm_dst_reg((*d).reg as ::core::ffi::c_uint, VX0 as ::core::ffi::c_int);
    let mut need_d: ::core::ffi::c_int = (op
        != OCERZ_OP_AESIMC as ::core::ffi::c_int as ::core::ffi::c_uint
        && kga == 0) as ::core::ffi::c_int;
    if vd == VX0 as ::core::ffi::c_int && need_d != 0 {
        emit_xmm_ld(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    match op {
        399 | 400 => {
            a64_v_zero(b, VX3 as ::core::ffi::c_int);
            a64_aese(b, VX3 as ::core::ffi::c_int, vd);
            if op == OCERZ_OP_AESENC as ::core::ffi::c_int as ::core::ffi::c_uint {
                a64_aesmc(b, VX3 as ::core::ffi::c_int, VX3 as ::core::ffi::c_int);
            }
            a64_v_eor(b, vd, VX3 as ::core::ffi::c_int, vb);
        }
        401 | 402 => {
            a64_v_zero(b, VX3 as ::core::ffi::c_int);
            a64_aesd(b, VX3 as ::core::ffi::c_int, vd);
            if op == OCERZ_OP_AESDEC as ::core::ffi::c_int as ::core::ffi::c_uint {
                a64_aesimc(b, VX3 as ::core::ffi::c_int, VX3 as ::core::ffi::c_int);
            }
            a64_v_eor(b, vd, VX3 as ::core::ffi::c_int, vb);
        }
        403 => {
            a64_aesimc(b, vd, vb);
        }
        _ => {
            let mut rcon: u64 = ((*insn).ops[2 as ::core::ffi::c_int as usize].imm & 0xff as u64)
                << 32 as ::core::ffi::c_int;
            a64_v_zero(b, VX2 as ::core::ffi::c_int);
            a64_aese(b, VX2 as ::core::ffi::c_int, vb);
            emit_v_literal(
                b,
                VX3 as ::core::ffi::c_int,
                0x40b0e010b0e0104 as u64,
                0xc0306090306090c as u64,
            );
            a64_v_tbl1(
                b,
                VX2 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                VX3 as ::core::ffi::c_int,
            );
            emit_v_literal(b, VX3 as ::core::ffi::c_int, rcon, rcon);
            a64_v_eor(b, vd, VX2 as ::core::ffi::c_int, VX3 as ::core::ffi::c_int);
        }
    }
    if vd == VX0 as ::core::ffi::c_int {
        emit_xmm_st(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_sse_pclmul(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int
        || (*insn).ops[2 as ::core::ffi::c_int as usize].kind as ::core::ffi::c_int
            != OCERZ_OPK_IMM as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut imm: ::core::ffi::c_uint = (*insn).ops[2 as ::core::ffi::c_int as usize].imm
        as ::core::ffi::c_uint
        & 0xff as ::core::ffi::c_uint;
    let mut vb: ::core::ffi::c_int = emit_sse_src_reg(
        b,
        insn,
        s,
        16 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    );
    if vb < 0 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut vd: ::core::ffi::c_int =
        xmm_dst_reg((*d).reg as ::core::ffi::c_uint, VX0 as ::core::ffi::c_int);
    if vd == VX0 as ::core::ffi::c_int {
        emit_xmm_ld(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    let mut xh: ::core::ffi::c_int = (imm & 1 as ::core::ffi::c_uint) as ::core::ffi::c_int;
    let mut yh: ::core::ffi::c_int =
        (imm >> 4 as ::core::ffi::c_int & 1 as ::core::ffi::c_uint) as ::core::ffi::c_int;
    if xh == yh {
        a64_v_pmull_d(b, xh, vd, vd, vb);
    } else {
        let mut vx: ::core::ffi::c_int = vd;
        let mut vy: ::core::ffi::c_int = vb;
        if xh != 0 {
            a64_v_dup_d(b, VX2 as ::core::ffi::c_int, vd, 1 as ::core::ffi::c_int);
            vx = VX2 as ::core::ffi::c_int;
        }
        if yh != 0 {
            a64_v_dup_d(b, VX3 as ::core::ffi::c_int, vb, 1 as ::core::ffi::c_int);
            vy = VX3 as ::core::ffi::c_int;
        }
        a64_v_pmull_d(b, 0 as ::core::ffi::c_int, vd, vx, vy);
    }
    if vd == VX0 as ::core::ffi::c_int {
        emit_xmm_st(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    return 1 as ::core::ffi::c_int;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_crc32(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
) -> ::core::ffi::c_int {
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int
        || (*insn).seg as ::core::ffi::c_int != OCERZ_SEG_NONE as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_REG as ::core::ffi::c_int
        || (*d).high8 as ::core::ffi::c_int != 0
        || (*d).size as ::core::ffi::c_int != 4 as ::core::ffi::c_int
            && (*d).size as ::core::ffi::c_int != 8 as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut ds: ::core::ffi::c_int = pin_slot((*d).reg as ::core::ffi::c_uint);
    if ds < 0 as ::core::ffi::c_int
        || rsp_is_ptr() != 0 && (*d).reg as ::core::ffi::c_int == OCERZ_RSP as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut size: ::core::ffi::c_int = (*s).size as ::core::ffi::c_int;
    let mut rs: ::core::ffi::c_int = 0;
    if size != 1 as ::core::ffi::c_int
        && size != 2 as ::core::ffi::c_int
        && size != 4 as ::core::ffi::c_int
        && size != 8 as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_REG as ::core::ffi::c_int {
        if (*s).high8 != 0 {
            return 0 as ::core::ffi::c_int;
        }
        let mut ss: ::core::ffi::c_int = pin_slot((*s).reg as ::core::ffi::c_uint);
        if ss < 0 as ::core::ffi::c_int
            || rsp_is_ptr() != 0
                && (*s).reg as ::core::ffi::c_int == OCERZ_RSP as ::core::ffi::c_int
        {
            return 0 as ::core::ffi::c_int;
        }
        rs = pin_hreg(ss);
    } else if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int {
        if emit_mem_load_any(b, insn, s, size, JT1 as ::core::ffi::c_int) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        rs = JT1 as ::core::ffi::c_int;
    } else {
        return 0 as ::core::ffi::c_int;
    }
    let mut rd: ::core::ffi::c_int = pin_hreg(ds);
    a64_crc32c(b, size, rd, rd, rs);
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_sse_comis(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut dbl: ::core::ffi::c_int = ((*insn).op as ::core::ffi::c_int
        == OCERZ_OP_UCOMISD as ::core::ffi::c_int
        || (*insn).op as ::core::ffi::c_int == OCERZ_OP_COMISD as ::core::ffi::c_int)
        as ::core::ffi::c_int;
    let mut esz: ::core::ffi::c_int = if dbl != 0 {
        8 as ::core::ffi::c_int
    } else {
        4 as ::core::ffi::c_int
    };
    if g_cur_need == 0 as u64
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
    {
        if fpb_det_here(g_cur_insn_idx) != 0
            && g_fpb_det[g_cur_insn_idx as usize] as ::core::ffi::c_int == 2 as ::core::ffi::c_int
        {
            let mut vb: ::core::ffi::c_int = l0_src2(b, (*s).reg as ::core::ffi::c_uint, dbl);
            let mut va: ::core::ffi::c_int = l0_src2(b, (*d).reg as ::core::ffi::c_uint, dbl);
            a64_fcmp(b, dbl, va, vb);
            fpb_site_emit(b, g_cur_insn_idx, va, vb, dbl);
        }
        return 1 as ::core::ffi::c_int;
    }
    if g_cur_need == 0 as u64
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int
        && ({
            static mut on_: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
            if on_ < 0 as ::core::ffi::c_int {
                on_ = (!libc::getenv(c"OCERZ_NO_COMIS_MEM_FUSE".as_ptr()).is_null())
                    as ::core::ffi::c_int;
            }
            on_
        }) == 0
    {
        let mut vb_0: ::core::ffi::c_int = emit_sse_src_reg(
            b,
            insn,
            s,
            esz,
            VX1 as ::core::ffi::c_int,
            exit_sites,
            n_exits,
        );
        if vb_0 < 0 as ::core::ffi::c_int {
            return 0 as ::core::ffi::c_int;
        }
        a64_str_v(b, esz, vb_0, 20 as ::core::ffi::c_int, FCMP_MEM_OFF);
        return 1 as ::core::ffi::c_int;
    }
    if g_defer != 0 {
        a64_str(
            b,
            4 as ::core::ffi::c_int,
            A64_ZR as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            CC_OP_OFF,
        );
    }
    let mut vb_1: ::core::ffi::c_int = if (*s).kind as ::core::ffi::c_int
        == OCERZ_OPK_XMM as ::core::ffi::c_int
        && xmm_is_pinned((*s).reg as ::core::ffi::c_uint) != 0
    {
        l0_src2(b, (*s).reg as ::core::ffi::c_uint, dbl)
    } else {
        emit_sse_src_reg(
            b,
            insn,
            s,
            esz,
            VX1 as ::core::ffi::c_int,
            exit_sites,
            n_exits,
        )
    };
    if vb_1 < 0 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int {
        a64_str_v(b, esz, vb_1, 20 as ::core::ffi::c_int, FCMP_MEM_OFF);
    }
    let mut va_0: ::core::ffi::c_int = if xmm_is_pinned((*d).reg as ::core::ffi::c_uint) != 0 {
        l0_src2(b, (*d).reg as ::core::ffi::c_uint, dbl)
    } else {
        VX0 as ::core::ffi::c_int
    };
    if va_0 == VX0 as ::core::ffi::c_int {
        emit_xmm_ld_lo(
            b,
            esz,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    a64_fcmp(b, dbl, va_0, vb_1);
    if fpb_det_here(g_cur_insn_idx) != 0 {
        fpb_site_emit(b, g_cur_insn_idx, va_0, vb_1, dbl);
    }
    a64_ldr(
        b,
        8 as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        20 as ::core::ffi::c_int,
        RF_OFF,
    );
    a64_cset(b, JT0 as ::core::ffi::c_int, A64_LT as ::core::ffi::c_int);
    a64_cset(b, JT1 as ::core::ffi::c_int, A64_VS as ::core::ffi::c_int);
    a64_bfi(
        b,
        1 as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
        1 as ::core::ffi::c_int,
    );
    a64_bfi(
        b,
        1 as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        2 as ::core::ffi::c_int,
        1 as ::core::ffi::c_int,
    );
    a64_cset(b, JT0 as ::core::ffi::c_int, A64_EQ as ::core::ffi::c_int);
    a64_orr_reg(
        b,
        1 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    a64_bfi(
        b,
        1 as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        6 as ::core::ffi::c_int,
        1 as ::core::ffi::c_int,
    );
    a64_mov_imm64(
        b,
        JTU as ::core::ffi::c_int,
        !(OCERZ_SF | OCERZ_OF | OCERZ_AF),
    );
    a64_and_reg(
        b,
        1 as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        JTU as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    a64_str(
        b,
        8 as ::core::ffi::c_int,
        JTT as ::core::ffi::c_int,
        20 as ::core::ffi::c_int,
        RF_OFF,
    );
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_sse_cvt(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    match (*insn).op as ::core::ffi::c_int {
        271 | 270 => {
            if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_REG as ::core::ffi::c_int
                || (*d).high8 as ::core::ffi::c_int != 0
                || (*d).size as ::core::ffi::c_int != 4 as ::core::ffi::c_int
                    && (*d).size as ::core::ffi::c_int != 8 as ::core::ffi::c_int
            {
                return 0 as ::core::ffi::c_int;
            }
            if rsp_is_ptr() != 0
                && (*d).reg as ::core::ffi::c_int == OCERZ_RSP as ::core::ffi::c_int
            {
                return 0 as ::core::ffi::c_int;
            }
            let mut dbl: ::core::ffi::c_int = ((*insn).op as ::core::ffi::c_int
                == OCERZ_OP_CVTTSD2SI as ::core::ffi::c_int)
                as ::core::ffi::c_int;
            let mut vs: ::core::ffi::c_int = if (*s).kind as ::core::ffi::c_int
                == OCERZ_OPK_XMM as ::core::ffi::c_int
                && xmm_is_pinned((*s).reg as ::core::ffi::c_uint) != 0
            {
                l0_src2(b, (*s).reg as ::core::ffi::c_uint, dbl)
            } else {
                emit_sse_src_reg(
                    b,
                    insn,
                    s,
                    if dbl != 0 {
                        8 as ::core::ffi::c_int
                    } else {
                        4 as ::core::ffi::c_int
                    },
                    VX0 as ::core::ffi::c_int,
                    exit_sites,
                    n_exits,
                )
            };
            if vs < 0 as ::core::ffi::c_int {
                return 0 as ::core::ffi::c_int;
            }
            let mut ds: ::core::ffi::c_int = pin_slot((*d).reg as ::core::ffi::c_uint);
            let mut rd: ::core::ffi::c_int = if ds >= 0 as ::core::ffi::c_int {
                pin_hreg(ds)
            } else {
                JT0 as ::core::ffi::c_int
            };
            let mut reuse: ::core::ffi::c_int =
                (dbl != 0 && g_fcmp_self_idx == g_cur_insn_idx && g_fcmp_self_vreg == vs)
                    as ::core::ffi::c_int;
            let mut pend: ::core::ffi::c_int = (g_scpend.valid != 0
                && g_scpend.idx == g_cur_insn_idx - 1 as ::core::ffi::c_int)
                as ::core::ffi::c_int;
            if pend != 0
                && (reuse == 0
                    || g_n_nanool + 1 as ::core::ffi::c_int > NANOOL_MAX as ::core::ffi::c_int
                    || unsafe_nocheckbr() != 0)
            {
                scalar_pend_flush(b);
                pend = 0 as ::core::ffi::c_int;
                reuse = (dbl != 0 && g_fcmp_self_idx == g_cur_insn_idx && g_fcmp_self_vreg == vs)
                    as ::core::ffi::c_int;
            }
            if g_n_nanool + 1 as ::core::ffi::c_int <= NANOOL_MAX as ::core::ffi::c_int {
                a64_fcvtzs(
                    b,
                    ((*d).size as ::core::ffi::c_int == 8 as ::core::ffi::c_int)
                        as ::core::ffi::c_int,
                    dbl,
                    rd,
                    vs,
                );
                if reuse == 0 {
                    a64_fcmp(b, dbl, vs, vs);
                }
                g_fcmp_self_idx = -(1 as ::core::ffi::c_int);
                a64_ccmn_imm(
                    b,
                    ((*d).size as ::core::ffi::c_int == 8 as ::core::ffi::c_int)
                        as ::core::ffi::c_int,
                    rd,
                    1 as ::core::ffi::c_int,
                    1 as ::core::ffi::c_int,
                    A64_VC as ::core::ffi::c_int,
                );
                if unsafe_nocheckbr() == 0 {
                    let fresh0 = g_n_nanool;
                    g_n_nanool = g_n_nanool + 1;
                    let mut o: *mut NanOolPend = (&raw mut g_nanool as *mut NanOolPend)
                        .offset(fresh0 as isize)
                        as *mut NanOolPend;
                    (*o).site = a64_label(b);
                    a64_bcond(b, A64_VS as ::core::ffi::c_int, 0 as i32);
                    (*o).back = a64_label(b);
                    (*o).dbl = dbl as u8;
                    (*o).packed = 0 as u8;
                    (*o).vr = rd as u8;
                    (*o).va = vs as u8;
                    (*o).vb = 0 as u8;
                    (*o).t1 = 0 as u8;
                    (*o).cvt = (if (*d).size as ::core::ffi::c_int == 8 as ::core::ffi::c_int {
                        1 as ::core::ffi::c_int
                    } else {
                        2 as ::core::ffi::c_int
                    }) as u8;
                    (*o).refcmp = 0 as u8;
                    (*o).pre = pend as u8;
                    if pend != 0 {
                        (*o).pvr = g_scpend.vr as u8;
                        (*o).pva = g_scpend.va as u8;
                        (*o).pvb = g_scpend.vb as u8;
                        g_scpend.valid = 0 as ::core::ffi::c_int;
                    }
                    (*o).idx = g_cur_insn_idx;
                    (*o).is_cbz = 0 as ::core::ffi::c_int;
                }
            } else if (*d).size as ::core::ffi::c_int == 8 as ::core::ffi::c_int {
                a64_fcvtzs(b, 1 as ::core::ffi::c_int, dbl, rd, vs);
                a64_cmn_imm(b, 1 as ::core::ffi::c_int, rd, 1 as ::core::ffi::c_int);
                a64_movz(
                    b,
                    JTU as ::core::ffi::c_int,
                    0x8000 as u16,
                    3 as ::core::ffi::c_int,
                );
                a64_csel(
                    b,
                    1 as ::core::ffi::c_int,
                    rd,
                    JTU as ::core::ffi::c_int,
                    rd,
                    A64_VS as ::core::ffi::c_int,
                );
                a64_fcmp(b, dbl, vs, vs);
                a64_csel(
                    b,
                    1 as ::core::ffi::c_int,
                    rd,
                    JTU as ::core::ffi::c_int,
                    rd,
                    A64_VS as ::core::ffi::c_int,
                );
            } else {
                a64_fcvtzs(
                    b,
                    1 as ::core::ffi::c_int,
                    dbl,
                    JT0 as ::core::ffi::c_int,
                    vs,
                );
                a64_cmp_ext_sxtw(b, JT0 as ::core::ffi::c_int, JT0 as ::core::ffi::c_int);
                a64_movz(
                    b,
                    JTU as ::core::ffi::c_int,
                    0x8000 as u16,
                    1 as ::core::ffi::c_int,
                );
                a64_csel(
                    b,
                    0 as ::core::ffi::c_int,
                    rd,
                    JTU as ::core::ffi::c_int,
                    JT0 as ::core::ffi::c_int,
                    A64_NE as ::core::ffi::c_int,
                );
                a64_fcmp(b, dbl, vs, vs);
                a64_csel(
                    b,
                    0 as ::core::ffi::c_int,
                    rd,
                    JTU as ::core::ffi::c_int,
                    rd,
                    A64_VS as ::core::ffi::c_int,
                );
            }
            if ds < 0 as ::core::ffi::c_int {
                emit_gpr_wr(
                    b,
                    JT0 as ::core::ffi::c_int,
                    (*d).reg as ::core::ffi::c_uint,
                );
            }
            return 1 as ::core::ffi::c_int;
        }
        267 | 266 => {
            if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int {
                return 0 as ::core::ffi::c_int;
            }
            let mut dbl_0: ::core::ffi::c_int = ((*insn).op as ::core::ffi::c_int
                == OCERZ_OP_CVTSI2SD as ::core::ffi::c_int)
                as ::core::ffi::c_int;
            let mut sf: ::core::ffi::c_int = 0;
            if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_REG as ::core::ffi::c_int {
                if (*s).high8 as ::core::ffi::c_int != 0
                    || (*s).size as ::core::ffi::c_int != 4 as ::core::ffi::c_int
                        && (*s).size as ::core::ffi::c_int != 8 as ::core::ffi::c_int
                {
                    return 0 as ::core::ffi::c_int;
                }
                sf = ((*s).size as ::core::ffi::c_int == 8 as ::core::ffi::c_int)
                    as ::core::ffi::c_int;
                emit_gpr_rd(
                    b,
                    sf,
                    JT0 as ::core::ffi::c_int,
                    (*s).reg as ::core::ffi::c_uint,
                );
            } else if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int {
                if (*s).size as ::core::ffi::c_int != 4 as ::core::ffi::c_int
                    && (*s).size as ::core::ffi::c_int != 8 as ::core::ffi::c_int
                {
                    return 0 as ::core::ffi::c_int;
                }
                sf = ((*s).size as ::core::ffi::c_int == 8 as ::core::ffi::c_int)
                    as ::core::ffi::c_int;
                let mut skip: *mut u32 = ::core::ptr::null_mut::<u32>();
                if emit_sse_mem_addr(
                    b,
                    insn,
                    s,
                    (*s).size as ::core::ffi::c_int,
                    exit_sites,
                    n_exits,
                    &raw mut skip,
                ) == 0
                {
                    return 0 as ::core::ffi::c_int;
                }
                emit_sse_mem_ld_gpr(
                    b,
                    (*s).size as ::core::ffi::c_int,
                    JT0 as ::core::ffi::c_int,
                );
                patch_guard_skip(skip, a64_label(b));
            } else {
                return 0 as ::core::ffi::c_int;
            }
            let mut t: ::core::ffi::c_int =
                if xmm_is_pinned((*d).reg as ::core::ffi::c_uint) != 0 && l0_enabled() != 0 {
                    l0_alloc2(b, (*d).reg as ::core::ffi::c_uint, dbl_0)
                } else {
                    VX0 as ::core::ffi::c_int
                };
            if t < 0 as ::core::ffi::c_int {
                t = VX0 as ::core::ffi::c_int;
            }
            a64_scvtf(b, sf, dbl_0, t, JT0 as ::core::ffi::c_int);
            emit_xmm_st_lo(
                b,
                if dbl_0 != 0 {
                    8 as ::core::ffi::c_int
                } else {
                    4 as ::core::ffi::c_int
                },
                t,
                (*d).reg as ::core::ffi::c_uint,
            );
            return 1 as ::core::ffi::c_int;
        }
        273 => {
            if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int {
                return 0 as ::core::ffi::c_int;
            }
            if emit_sse_src(
                b,
                insn,
                s,
                8 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                exit_sites,
                n_exits,
            ) == 0
            {
                return 0 as ::core::ffi::c_int;
            }
            a64_fcvt_d2s(b, VX1 as ::core::ffi::c_int, VX0 as ::core::ffi::c_int);
            emit_xmm_st_lo(
                b,
                4 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
                (*d).reg as ::core::ffi::c_uint,
            );
            return 1 as ::core::ffi::c_int;
        }
        272 => {
            if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int {
                return 0 as ::core::ffi::c_int;
            }
            if emit_sse_src(
                b,
                insn,
                s,
                4 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                exit_sites,
                n_exits,
            ) == 0
            {
                return 0 as ::core::ffi::c_int;
            }
            a64_fcvt_s2d(b, VX1 as ::core::ffi::c_int, VX0 as ::core::ffi::c_int);
            emit_xmm_st_lo(
                b,
                8 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
                (*d).reg as ::core::ffi::c_uint,
            );
            return 1 as ::core::ffi::c_int;
        }
        276 => {
            if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int {
                return 0 as ::core::ffi::c_int;
            }
            if emit_sse_src(
                b,
                insn,
                s,
                16 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                exit_sites,
                n_exits,
            ) == 0
            {
                return 0 as ::core::ffi::c_int;
            }
            a64_v_scvtf_4s(b, VX1 as ::core::ffi::c_int, VX0 as ::core::ffi::c_int);
            emit_xmm_st(
                b,
                VX1 as ::core::ffi::c_int,
                (*d).reg as ::core::ffi::c_uint,
            );
            return 1 as ::core::ffi::c_int;
        }
        _ => return 0 as ::core::ffi::c_int,
    };
}
unsafe fn emit_sse_movd(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_REG as ::core::ffi::c_int
    {
        if (*s).high8 as ::core::ffi::c_int != 0
            || (*s).size as ::core::ffi::c_int != 4 as ::core::ffi::c_int
                && (*s).size as ::core::ffi::c_int != 8 as ::core::ffi::c_int
        {
            return 0 as ::core::ffi::c_int;
        }
        emit_gpr_rd(
            b,
            ((*s).size as ::core::ffi::c_int == 8 as ::core::ffi::c_int) as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            (*s).reg as ::core::ffi::c_uint,
        );
        a64_fmov_v_from_x(
            b,
            ((*s).size as ::core::ffi::c_int == 8 as ::core::ffi::c_int) as ::core::ffi::c_int,
            VX0 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
        );
        emit_xmm_st(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
        return 1 as ::core::ffi::c_int;
    }
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_REG as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
    {
        if (*d).high8 as ::core::ffi::c_int != 0
            || (*d).size as ::core::ffi::c_int != 4 as ::core::ffi::c_int
                && (*d).size as ::core::ffi::c_int != 8 as ::core::ffi::c_int
        {
            return 0 as ::core::ffi::c_int;
        }
        emit_xmm_ld_lo(
            b,
            (*d).size as ::core::ffi::c_int,
            VX0 as ::core::ffi::c_int,
            (*s).reg as ::core::ffi::c_uint,
        );
        a64_fmov_x_from_v(
            b,
            ((*d).size as ::core::ffi::c_int == 8 as ::core::ffi::c_int) as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            VX0 as ::core::ffi::c_int,
        );
        emit_gpr_wr(
            b,
            JT0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
        return 1 as ::core::ffi::c_int;
    }
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int
    {
        if (*s).size as ::core::ffi::c_int != 4 as ::core::ffi::c_int
            && (*s).size as ::core::ffi::c_int != 8 as ::core::ffi::c_int
        {
            return 0 as ::core::ffi::c_int;
        }
        let mut skip: *mut u32 = ::core::ptr::null_mut::<u32>();
        if emit_sse_mem_addr(
            b,
            insn,
            s,
            (*s).size as ::core::ffi::c_int,
            exit_sites,
            n_exits,
            &raw mut skip,
        ) == 0
        {
            return 0 as ::core::ffi::c_int;
        }
        emit_sse_mem_ld(
            b,
            (*s).size as ::core::ffi::c_int,
            VX0 as ::core::ffi::c_int,
        );
        patch_guard_skip(skip, a64_label(b));
        emit_xmm_st(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
        return 1 as ::core::ffi::c_int;
    }
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
    {
        if (*d).size as ::core::ffi::c_int != 4 as ::core::ffi::c_int
            && (*d).size as ::core::ffi::c_int != 8 as ::core::ffi::c_int
        {
            return 0 as ::core::ffi::c_int;
        }
        emit_xmm_ld_lo(
            b,
            (*d).size as ::core::ffi::c_int,
            VX0 as ::core::ffi::c_int,
            (*s).reg as ::core::ffi::c_uint,
        );
        let mut skip_0: *mut u32 = ::core::ptr::null_mut::<u32>();
        if emit_sse_mem_addr(
            b,
            insn,
            d,
            (*d).size as ::core::ffi::c_int,
            exit_sites,
            n_exits,
            &raw mut skip_0,
        ) == 0
        {
            return 0 as ::core::ffi::c_int;
        }
        emit_sse_mem_st(
            b,
            (*d).size as ::core::ffi::c_int,
            VX0 as ::core::ffi::c_int,
        );
        patch_guard_skip(skip_0, a64_label(b));
        return 1 as ::core::ffi::c_int;
    }
    return 0 as ::core::ffi::c_int;
}
unsafe fn emit_sse_movq(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
    {
        if xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
            || xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0
        {
            return 0 as ::core::ffi::c_int;
        }
        l0_flush_reg(b, (*s).reg as ::core::ffi::c_uint);
        l0_inval((*d).reg as ::core::ffi::c_uint);
        a64_fmov_d_d(
            b,
            xmm_vreg((*d).reg as ::core::ffi::c_uint),
            xmm_vreg((*s).reg as ::core::ffi::c_uint),
        );
        return 1 as ::core::ffi::c_int;
    }
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_REG as ::core::ffi::c_int
    {
        if (*s).high8 as ::core::ffi::c_int != 0
            || (*s).size as ::core::ffi::c_int != 8 as ::core::ffi::c_int
            || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
        {
            return 0 as ::core::ffi::c_int;
        }
        emit_gpr_rd(
            b,
            1 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            (*s).reg as ::core::ffi::c_uint,
        );
        a64_fmov_v_from_x(
            b,
            1 as ::core::ffi::c_int,
            xmm_vreg((*d).reg as ::core::ffi::c_uint),
            JT0 as ::core::ffi::c_int,
        );
        return 1 as ::core::ffi::c_int;
    }
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_REG as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
    {
        if (*d).high8 as ::core::ffi::c_int != 0
            || (*d).size as ::core::ffi::c_int != 8 as ::core::ffi::c_int
            || xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0
        {
            return 0 as ::core::ffi::c_int;
        }
        l0_flush_reg(b, (*s).reg as ::core::ffi::c_uint);
        a64_fmov_x_from_v(
            b,
            1 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            xmm_vreg((*s).reg as ::core::ffi::c_uint),
        );
        emit_gpr_wr(
            b,
            JT0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
        return 1 as ::core::ffi::c_int;
    }
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int
    {
        if xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        l0_inval((*d).reg as ::core::ffi::c_uint);
        let mut vd: ::core::ffi::c_int = xmm_vreg((*d).reg as ::core::ffi::c_uint);
        if emit_plain_mem_fast(
            b,
            insn,
            s,
            8 as ::core::ffi::c_int,
            vd,
            0 as ::core::ffi::c_int,
            1 as ::core::ffi::c_int,
        ) != 0
        {
            return 1 as ::core::ffi::c_int;
        }
        let mut skip: *mut u32 = ::core::ptr::null_mut::<u32>();
        if emit_sse_mem_addr(
            b,
            insn,
            s,
            8 as ::core::ffi::c_int,
            exit_sites,
            n_exits,
            &raw mut skip,
        ) == 0
        {
            return 0 as ::core::ffi::c_int;
        }
        emit_sse_mem_ld(b, 8 as ::core::ffi::c_int, vd);
        patch_guard_skip(skip, a64_label(b));
        return 1 as ::core::ffi::c_int;
    }
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
    {
        if xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        l0_flush_reg(b, (*s).reg as ::core::ffi::c_uint);
        let mut vs: ::core::ffi::c_int = xmm_vreg((*s).reg as ::core::ffi::c_uint);
        if emit_plain_mem_fast(
            b,
            insn,
            d,
            8 as ::core::ffi::c_int,
            vs,
            1 as ::core::ffi::c_int,
            1 as ::core::ffi::c_int,
        ) != 0
        {
            return 1 as ::core::ffi::c_int;
        }
        let mut skip_0: *mut u32 = ::core::ptr::null_mut::<u32>();
        if emit_sse_mem_addr(
            b,
            insn,
            d,
            8 as ::core::ffi::c_int,
            exit_sites,
            n_exits,
            &raw mut skip_0,
        ) == 0
        {
            return 0 as ::core::ffi::c_int;
        }
        emit_sse_mem_st(b, 8 as ::core::ffi::c_int, vs);
        patch_guard_skip(skip_0, a64_label(b));
        return 1 as ::core::ffi::c_int;
    }
    return 0 as ::core::ffi::c_int;
}
unsafe fn emit_sse_pshufd(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int || sse_enabled() == 0 {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut imm: ::core::ffi::c_uint = (*insn).ops[2 as ::core::ffi::c_int as usize].imm
        as ::core::ffi::c_uint
        & 0xff as ::core::ffi::c_uint;
    let mut vs: ::core::ffi::c_int = emit_sse_src_reg(
        b,
        insn,
        s,
        16 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    );
    if vs < 0 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    l0_flush_reg(b, (*d).reg as ::core::ffi::c_uint);
    l0_inval((*d).reg as ::core::ffi::c_uint);
    let mut vd: ::core::ffi::c_int = xmm_vreg((*d).reg as ::core::ffi::c_uint);
    let mut sel: [::core::ffi::c_uint; 4] = [
        imm & 3 as ::core::ffi::c_uint,
        imm >> 2 as ::core::ffi::c_int & 3 as ::core::ffi::c_uint,
        imm >> 4 as ::core::ffi::c_int & 3 as ::core::ffi::c_uint,
        imm >> 6 as ::core::ffi::c_int & 3 as ::core::ffi::c_uint,
    ];
    if sel[0 as ::core::ffi::c_int as usize] == sel[1 as ::core::ffi::c_int as usize]
        && sel[1 as ::core::ffi::c_int as usize] == sel[2 as ::core::ffi::c_int as usize]
        && sel[2 as ::core::ffi::c_int as usize] == sel[3 as ::core::ffi::c_int as usize]
    {
        a64_v_dup_s(
            b,
            vd,
            vs,
            sel[0 as ::core::ffi::c_int as usize] as ::core::ffi::c_int,
        );
        return 1 as ::core::ffi::c_int;
    }
    if sel[0 as ::core::ffi::c_int as usize] == 0 as ::core::ffi::c_uint
        && sel[1 as ::core::ffi::c_int as usize] == 1 as ::core::ffi::c_uint
        && sel[2 as ::core::ffi::c_int as usize] == 0 as ::core::ffi::c_uint
        && sel[3 as ::core::ffi::c_int as usize] == 1 as ::core::ffi::c_uint
    {
        a64_v_dup_d(b, vd, vs, 0 as ::core::ffi::c_int);
        return 1 as ::core::ffi::c_int;
    }
    if sel[0 as ::core::ffi::c_int as usize] == 2 as ::core::ffi::c_uint
        && sel[1 as ::core::ffi::c_int as usize] == 3 as ::core::ffi::c_uint
        && sel[2 as ::core::ffi::c_int as usize] == 2 as ::core::ffi::c_uint
        && sel[3 as ::core::ffi::c_int as usize] == 3 as ::core::ffi::c_uint
    {
        a64_v_dup_d(b, vd, vs, 1 as ::core::ffi::c_int);
        return 1 as ::core::ffi::c_int;
    }
    if sel[0 as ::core::ffi::c_int as usize] == 2 as ::core::ffi::c_uint
        && sel[1 as ::core::ffi::c_int as usize] == 3 as ::core::ffi::c_uint
        && sel[2 as ::core::ffi::c_int as usize] == 0 as ::core::ffi::c_uint
        && sel[3 as ::core::ffi::c_int as usize] == 1 as ::core::ffi::c_uint
    {
        a64_v_ext(b, vd, vs, vs, 8 as ::core::ffi::c_int);
        return 1 as ::core::ffi::c_int;
    }
    if sel[0 as ::core::ffi::c_int as usize] == 0 as ::core::ffi::c_uint
        && sel[1 as ::core::ffi::c_int as usize] == 1 as ::core::ffi::c_uint
        && sel[2 as ::core::ffi::c_int as usize] == 2 as ::core::ffi::c_uint
        && sel[3 as ::core::ffi::c_int as usize] == 3 as ::core::ffi::c_uint
    {
        if vd != vs {
            a64_v_mov(b, vd, vs);
        }
        return 1 as ::core::ffi::c_int;
    }
    if sel[0 as ::core::ffi::c_int as usize] == 1 as ::core::ffi::c_uint
        && sel[1 as ::core::ffi::c_int as usize] == 0 as ::core::ffi::c_uint
        && sel[2 as ::core::ffi::c_int as usize] == 3 as ::core::ffi::c_uint
        && sel[3 as ::core::ffi::c_int as usize] == 2 as ::core::ffi::c_uint
    {
        a64_v_rev64_4s(b, vd, vs);
        return 1 as ::core::ffi::c_int;
    }
    if sel[0 as ::core::ffi::c_int as usize] == 3 as ::core::ffi::c_uint
        && sel[1 as ::core::ffi::c_int as usize] == 2 as ::core::ffi::c_uint
        && sel[2 as ::core::ffi::c_int as usize] == 1 as ::core::ffi::c_uint
        && sel[3 as ::core::ffi::c_int as usize] == 0 as ::core::ffi::c_uint
    {
        a64_v_rev64_4s(b, VX0 as ::core::ffi::c_int, vs);
        a64_v_ext(
            b,
            vd,
            VX0 as ::core::ffi::c_int,
            VX0 as ::core::ffi::c_int,
            8 as ::core::ffi::c_int,
        );
        return 1 as ::core::ffi::c_int;
    }
    if g_n_raslit >= RASLIT_MAX as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut lo: u64 = 0 as u64;
    let mut hi: u64 = 0 as u64;
    let mut i: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    while i < 4 as ::core::ffi::c_int {
        let mut k: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
        while k < 4 as ::core::ffi::c_int {
            let mut byte: u64 = sel[i as usize]
                .wrapping_mul(4 as ::core::ffi::c_uint)
                .wrapping_add(k as ::core::ffi::c_uint) as u64;
            let mut pos: ::core::ffi::c_int = i * 4 as ::core::ffi::c_int + k;
            if pos < 8 as ::core::ffi::c_int {
                lo |= byte << 8 as ::core::ffi::c_int * pos;
            } else {
                hi |= byte << 8 as ::core::ffi::c_int * (pos - 8 as ::core::ffi::c_int);
            }
            k += 1;
        }
        i += 1;
    }
    g_raslit[g_n_raslit as usize].site = a64_label(b);
    g_raslit[g_n_raslit as usize].retaddr = lo;
    g_raslit[g_n_raslit as usize].hi = hi;
    g_raslit[g_n_raslit as usize].kind = 2 as ::core::ffi::c_int;
    g_raslit[g_n_raslit as usize].rt = VX0 as ::core::ffi::c_int;
    g_n_raslit += 1;
    a64_emit32(b, 0x9c000000 as u32 | VX0 as ::core::ffi::c_int as u32);
    a64_v_tbl1(b, vd, vs, VX0 as ::core::ffi::c_int);
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_sse_pshufb(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int || sse_enabled() == 0 {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut vb: ::core::ffi::c_int = emit_sse_src_reg(
        b,
        insn,
        s,
        16 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    );
    if vb < 0 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    l0_flush_reg(b, (*d).reg as ::core::ffi::c_uint);
    l0_inval((*d).reg as ::core::ffi::c_uint);
    let mut vd: ::core::ffi::c_int = xmm_vreg((*d).reg as ::core::ffi::c_uint);
    a64_emit32(b, 0x4f04e5e0 as u32 | VX0 as ::core::ffi::c_int as u32);
    a64_v_and(b, VX0 as ::core::ffi::c_int, vb, VX0 as ::core::ffi::c_int);
    a64_v_tbl1(b, vd, vd, VX0 as ::core::ffi::c_int);
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_sse_punpck(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int || sse_enabled() == 0 {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut vb: ::core::ffi::c_int = emit_sse_src_reg(
        b,
        insn,
        s,
        16 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    );
    if vb < 0 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    l0_flush_reg(b, (*d).reg as ::core::ffi::c_uint);
    l0_inval((*d).reg as ::core::ffi::c_uint);
    let mut vd: ::core::ffi::c_int = xmm_vreg((*d).reg as ::core::ffi::c_uint);
    match (*insn).op as ::core::ffi::c_int {
        326 => {
            a64_v_zip1(b, 0 as ::core::ffi::c_int, vd, vd, vb);
        }
        327 => {
            a64_v_zip1(b, 1 as ::core::ffi::c_int, vd, vd, vb);
        }
        328 => {
            a64_v_zip1(b, 2 as ::core::ffi::c_int, vd, vd, vb);
        }
        329 => {
            a64_v_zip1(b, 3 as ::core::ffi::c_int, vd, vd, vb);
        }
        330 => {
            a64_v_zip2(b, 0 as ::core::ffi::c_int, vd, vd, vb);
        }
        331 => {
            a64_v_zip2(b, 1 as ::core::ffi::c_int, vd, vd, vb);
        }
        332 => {
            a64_v_zip2(b, 2 as ::core::ffi::c_int, vd, vd, vb);
        }
        333 => {
            a64_v_zip2(b, 3 as ::core::ffi::c_int, vd, vd, vb);
        }
        _ => return 0 as ::core::ffi::c_int,
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_sse_unpck(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut vb: ::core::ffi::c_int = emit_sse_src_reg(
        b,
        insn,
        s,
        16 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    );
    if vb < 0 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut va: ::core::ffi::c_int = if xmm_is_pinned((*d).reg as ::core::ffi::c_uint) != 0 {
        xmm_vreg((*d).reg as ::core::ffi::c_uint)
    } else {
        VX0 as ::core::ffi::c_int
    };
    if va == VX0 as ::core::ffi::c_int {
        emit_xmm_ld(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    let mut vd: ::core::ffi::c_int =
        xmm_dst_reg((*d).reg as ::core::ffi::c_uint, VX2 as ::core::ffi::c_int);
    match (*insn).op as ::core::ffi::c_int {
        354 | 195 => {
            a64_v_zip1(b, 3 as ::core::ffi::c_int, vd, va, vb);
        }
        355 => {
            a64_v_zip2(b, 3 as ::core::ffi::c_int, vd, va, vb);
        }
        196 => {
            if vd != va {
                a64_v_mov(b, vd, va);
            }
            a64_ins_d_d(b, vd, 0 as ::core::ffi::c_int, vb, 1 as ::core::ffi::c_int);
        }
        352 => {
            a64_v_zip1(b, 2 as ::core::ffi::c_int, vd, va, vb);
        }
        353 => {
            a64_v_zip2(b, 2 as ::core::ffi::c_int, vd, va, vb);
        }
        _ => return 0 as ::core::ffi::c_int,
    }
    if vd == VX2 as ::core::ffi::c_int {
        emit_xmm_st(
            b,
            VX2 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_cmps_pred(
    mut b: *mut A64Buf,
    mut dbl: ::core::ffi::c_int,
    mut pred: ::core::ffi::c_uint,
    mut vr: ::core::ffi::c_int,
    mut va: ::core::ffi::c_int,
    mut vb: ::core::ffi::c_int,
) {
    match pred {
        0 => {
            a64_fcmeq_s(b, dbl, vr, va, vb);
        }
        1 => {
            a64_fcmgt_s(b, dbl, vr, vb, va);
        }
        2 => {
            a64_fcmge_s(b, dbl, vr, vb, va);
        }
        3 => {
            a64_fcmeq_s(b, dbl, vr, va, va);
            a64_fcmeq_s(b, dbl, VX3 as ::core::ffi::c_int, vb, vb);
            a64_v_and(b, vr, vr, VX3 as ::core::ffi::c_int);
            a64_v_not(b, vr, vr);
        }
        4 => {
            a64_fcmeq_s(b, dbl, vr, va, vb);
            a64_v_not(b, vr, vr);
        }
        5 => {
            a64_fcmgt_s(b, dbl, vr, vb, va);
            a64_v_not(b, vr, vr);
        }
        6 => {
            a64_fcmge_s(b, dbl, vr, vb, va);
            a64_v_not(b, vr, vr);
        }
        _ => {
            a64_fcmeq_s(b, dbl, vr, va, va);
            a64_fcmeq_s(b, dbl, VX3 as ::core::ffi::c_int, vb, vb);
            a64_v_and(b, vr, vr, VX3 as ::core::ffi::c_int);
        }
    };
}
unsafe fn emit_sse_cmps(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || ((*insn).nops as ::core::ffi::c_int) < 3 as ::core::ffi::c_int
        || (*insn).ops[2 as ::core::ffi::c_int as usize].kind as ::core::ffi::c_int
            != OCERZ_OPK_IMM as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut dbl: ::core::ffi::c_int = ((*insn).op as ::core::ffi::c_int
        == OCERZ_OP_CMPSDX as ::core::ffi::c_int)
        as ::core::ffi::c_int;
    let mut esz: ::core::ffi::c_int = if dbl != 0 {
        8 as ::core::ffi::c_int
    } else {
        4 as ::core::ffi::c_int
    };
    let mut pred: ::core::ffi::c_uint = (*insn).ops[2 as ::core::ffi::c_int as usize].imm
        as ::core::ffi::c_uint
        & 7 as ::core::ffi::c_uint;
    let mut vb: ::core::ffi::c_int = if (*s).kind as ::core::ffi::c_int
        == OCERZ_OPK_XMM as ::core::ffi::c_int
        && xmm_is_pinned((*s).reg as ::core::ffi::c_uint) != 0
    {
        l0_src2(b, (*s).reg as ::core::ffi::c_uint, dbl)
    } else {
        emit_sse_src_reg(
            b,
            insn,
            s,
            esz,
            VX1 as ::core::ffi::c_int,
            exit_sites,
            n_exits,
        )
    };
    if vb < 0 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut va: ::core::ffi::c_int = if xmm_is_pinned((*d).reg as ::core::ffi::c_uint) != 0 {
        l0_src2(b, (*d).reg as ::core::ffi::c_uint, dbl)
    } else {
        VX0 as ::core::ffi::c_int
    };
    if va == VX0 as ::core::ffi::c_int {
        emit_xmm_ld_lo(
            b,
            esz,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    emit_cmps_pred(b, dbl, pred, VX2 as ::core::ffi::c_int, va, vb);
    if cmps_blendv_fusable(g_cur_insn_idx) != 0 {
        g_cmps_mask_idx = g_cur_insn_idx;
        l0_inval((*d).reg as ::core::ffi::c_uint);
        return 1 as ::core::ffi::c_int;
    }
    emit_xmm_st_lo(
        b,
        esz,
        VX2 as ::core::ffi::c_int,
        (*d).reg as ::core::ffi::c_uint,
    );
    l0_inval((*d).reg as ::core::ffi::c_uint);
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_sse_blendv(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut fused: ::core::ffi::c_int = (g_cmps_mask_idx
        == g_cur_insn_idx - 1 as ::core::ffi::c_int
        && cmps_blendv_fusable(g_cur_insn_idx - 1 as ::core::ffi::c_int) != 0)
        as ::core::ffi::c_int;
    g_cmps_mask_idx = -(1 as ::core::ffi::c_int);
    if fused != 0 {
        let mut dbl: ::core::ffi::c_int = ((*insn).op as ::core::ffi::c_int
            == OCERZ_OP_BLENDVPD as ::core::ffi::c_int)
            as ::core::ffi::c_int;
        let mut esz: ::core::ffi::c_int = if dbl != 0 {
            8 as ::core::ffi::c_int
        } else {
            4 as ::core::ffi::c_int
        };
        let mut vd0: ::core::ffi::c_int = l0_src2(b, (*d).reg as ::core::ffi::c_uint, dbl);
        let mut t: ::core::ffi::c_int = l0_alloc2(b, (*d).reg as ::core::ffi::c_uint, dbl);
        if t >= 0 as ::core::ffi::c_int {
            let mut vs0: ::core::ffi::c_int = 0;
            let mut vsfull: ::core::ffi::c_int = 0;
            if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
                && xmm_is_pinned((*s).reg as ::core::ffi::c_uint) != 0
            {
                vs0 = l0_src2(b, (*s).reg as ::core::ffi::c_uint, dbl);
                vsfull = xmm_vreg((*s).reg as ::core::ffi::c_uint);
            } else {
                vsfull = emit_sse_src_reg(
                    b,
                    insn,
                    s,
                    16 as ::core::ffi::c_int,
                    VX1 as ::core::ffi::c_int,
                    exit_sites,
                    n_exits,
                );
                if vsfull < 0 as ::core::ffi::c_int {
                    return 0 as ::core::ffi::c_int;
                }
                vs0 = vsfull;
            }
            if vd0 != t {
                if dbl != 0 {
                    a64_fmov_d_d(b, t, vd0);
                } else {
                    a64_fmov_s_s(b, t, vd0);
                }
            }
            a64_v_bit(b, t, vs0, VX2 as ::core::ffi::c_int);
            if dbl != 0 {
                a64_v_sshr_2d(
                    b,
                    VX3 as ::core::ffi::c_int,
                    xmm_vreg(0 as ::core::ffi::c_uint),
                    63 as ::core::ffi::c_int,
                );
            } else {
                a64_v_sshr_4s(
                    b,
                    VX3 as ::core::ffi::c_int,
                    xmm_vreg(0 as ::core::ffi::c_uint),
                    31 as ::core::ffi::c_int,
                );
            }
            a64_v_bit(
                b,
                xmm_vreg((*d).reg as ::core::ffi::c_uint),
                vsfull,
                VX3 as ::core::ffi::c_int,
            );
            g_l0_dirty = (g_l0_dirty as ::core::ffi::c_int
                | ((1 as ::core::ffi::c_uint) << (*d).reg as ::core::ffi::c_int) as u16
                    as ::core::ffi::c_int) as u16;
            if dbl != 0 {
                a64_ins_d_d(
                    b,
                    xmm_vreg(0 as ::core::ffi::c_uint),
                    0 as ::core::ffi::c_int,
                    VX2 as ::core::ffi::c_int,
                    0 as ::core::ffi::c_int,
                );
            } else {
                a64_ins_s_s(
                    b,
                    xmm_vreg(0 as ::core::ffi::c_uint),
                    0 as ::core::ffi::c_int,
                    VX2 as ::core::ffi::c_int,
                    0 as ::core::ffi::c_int,
                );
            }
            return 1 as ::core::ffi::c_int;
        }
        if dbl != 0 {
            a64_ins_d_d(
                b,
                xmm_vreg(0 as ::core::ffi::c_uint),
                0 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                0 as ::core::ffi::c_int,
            );
        } else {
            a64_ins_s_s(
                b,
                xmm_vreg(0 as ::core::ffi::c_uint),
                0 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                0 as ::core::ffi::c_int,
            );
        }
    }
    if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
        l0_flush_reg(b, (*s).reg as ::core::ffi::c_uint);
    }
    l0_flush_reg(b, (*d).reg as ::core::ffi::c_uint);
    l0_flush_reg(b, 0 as ::core::ffi::c_uint);
    l0_inval((*d).reg as ::core::ffi::c_uint);
    let mut vb: ::core::ffi::c_int = emit_sse_src_reg(
        b,
        insn,
        s,
        16 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    );
    if vb < 0 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut va: ::core::ffi::c_int = if xmm_is_pinned((*d).reg as ::core::ffi::c_uint) != 0 {
        xmm_vreg((*d).reg as ::core::ffi::c_uint)
    } else {
        VX0 as ::core::ffi::c_int
    };
    if va == VX0 as ::core::ffi::c_int {
        emit_xmm_ld(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    let mut vm: ::core::ffi::c_int = if xmm_is_pinned(0 as ::core::ffi::c_uint) != 0 {
        xmm_vreg(0 as ::core::ffi::c_uint)
    } else {
        VX2 as ::core::ffi::c_int
    };
    if vm == VX2 as ::core::ffi::c_int {
        emit_xmm_ld(b, VX2 as ::core::ffi::c_int, 0 as ::core::ffi::c_uint);
    }
    match (*insn).op as ::core::ffi::c_int {
        397 => {
            a64_v_sshr_2d(b, VX2 as ::core::ffi::c_int, vm, 63 as ::core::ffi::c_int);
        }
        396 => {
            a64_v_sshr_4s(b, VX2 as ::core::ffi::c_int, vm, 31 as ::core::ffi::c_int);
        }
        398 => {
            a64_v_zero(b, VX3 as ::core::ffi::c_int);
            a64_v_cmgt(
                b,
                0 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                VX3 as ::core::ffi::c_int,
                vm,
            );
        }
        _ => return 0 as ::core::ffi::c_int,
    }
    if va != VX0 as ::core::ffi::c_int {
        a64_v_bit(b, va, vb, VX2 as ::core::ffi::c_int);
    } else {
        a64_v_bsl(b, VX2 as ::core::ffi::c_int, vb, va);
        emit_xmm_st(
            b,
            VX2 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_simd_shift_imm(
    mut b: *mut A64Buf,
    mut kind: ::core::ffi::c_int,
    mut esz: ::core::ffi::c_int,
    mut vd: ::core::ffi::c_int,
    mut vn: ::core::ffi::c_int,
    mut cnt: ::core::ffi::c_uint,
) {
    let mut w: ::core::ffi::c_uint = (8 as ::core::ffi::c_uint) << esz;
    if cnt == 0 as ::core::ffi::c_uint {
        if vd != vn {
            a64_v_mov(b, vd, vn);
        }
    } else if kind == 2 as ::core::ffi::c_int {
        a64_v_sshr_imm(
            b,
            esz,
            vd,
            vn,
            (if cnt < w { cnt } else { w }) as ::core::ffi::c_int,
        );
    } else if cnt >= w {
        a64_v_zero(b, vd);
    } else if kind == 0 as ::core::ffi::c_int {
        a64_v_shl_imm(b, esz, vd, vn, cnt as ::core::ffi::c_int);
    } else {
        a64_v_ushr_imm(b, esz, vd, vn, cnt as ::core::ffi::c_int);
    };
}
unsafe fn emit_sse_shift_imm(mut b: *mut A64Buf, mut insn: *const X86Insn) -> ::core::ffi::c_int {
    let mut kind: ::core::ffi::c_int = 0;
    let mut esz: ::core::ffi::c_int = 0;
    match (*insn).op as ::core::ffi::c_int {
        356 => {
            kind = 0 as ::core::ffi::c_int;
            esz = 1 as ::core::ffi::c_int;
        }
        357 => {
            kind = 0 as ::core::ffi::c_int;
            esz = 2 as ::core::ffi::c_int;
        }
        358 => {
            kind = 0 as ::core::ffi::c_int;
            esz = 3 as ::core::ffi::c_int;
        }
        359 => {
            kind = 1 as ::core::ffi::c_int;
            esz = 1 as ::core::ffi::c_int;
        }
        360 => {
            kind = 1 as ::core::ffi::c_int;
            esz = 2 as ::core::ffi::c_int;
        }
        361 => {
            kind = 1 as ::core::ffi::c_int;
            esz = 3 as ::core::ffi::c_int;
        }
        362 => {
            kind = 2 as ::core::ffi::c_int;
            esz = 1 as ::core::ffi::c_int;
        }
        363 => {
            kind = 2 as ::core::ffi::c_int;
            esz = 2 as ::core::ffi::c_int;
        }
        _ => return 0 as ::core::ffi::c_int,
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut c: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int
        || (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if (*c).kind as ::core::ffi::c_int == OCERZ_OPK_IMM as ::core::ffi::c_int {
        emit_simd_shift_imm(
            b,
            kind,
            esz,
            xmm_vreg((*d).reg as ::core::ffi::c_uint),
            xmm_vreg((*d).reg as ::core::ffi::c_uint),
            ((*c).imm & 0xff as u64) as ::core::ffi::c_uint,
        );
        return 1 as ::core::ffi::c_int;
    }
    if (*c).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
        if xmm_is_pinned((*c).reg as ::core::ffi::c_uint) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        l0_flush_reg(b, (*c).reg as ::core::ffi::c_uint);
        a64_fmov_x_from_v(
            b,
            1 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            xmm_vreg((*c).reg as ::core::ffi::c_uint),
        );
    } else if (*c).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int {
        if emit_mem_load_any(
            b,
            insn,
            c,
            8 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
        ) == 0
        {
            return 0 as ::core::ffi::c_int;
        }
    } else {
        return 0 as ::core::ffi::c_int;
    }
    a64_mov_imm64(b, JT1 as ::core::ffi::c_int, 64 as u64);
    a64_subs_reg(
        b,
        1 as ::core::ffi::c_int,
        A64_ZR as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    a64_csel(
        b,
        1 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        A64_CC as ::core::ffi::c_int,
    );
    if kind != 0 as ::core::ffi::c_int {
        a64_neg_reg(
            b,
            1 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
        );
    }
    a64_v_dup_gpr(
        b,
        (1 as ::core::ffi::c_int) << esz,
        VX2 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
    );
    let mut vd: ::core::ffi::c_int = xmm_vreg((*d).reg as ::core::ffi::c_uint);
    if kind == 2 as ::core::ffi::c_int {
        a64_v_sshl(b, esz, vd, vd, VX2 as ::core::ffi::c_int);
    } else {
        a64_v_ushl(b, esz, vd, vd, VX2 as ::core::ffi::c_int);
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_shufp_lane(
    mut b: *mut A64Buf,
    mut dbl: ::core::ffi::c_int,
    mut imm: ::core::ffi::c_uint,
    mut vd: ::core::ffi::c_int,
    mut va: ::core::ffi::c_int,
    mut vb: ::core::ffi::c_int,
) {
    if dbl != 0 {
        let mut i0: ::core::ffi::c_int = (imm & 1 as ::core::ffi::c_uint) as ::core::ffi::c_int;
        let mut i1: ::core::ffi::c_int =
            (imm >> 1 as ::core::ffi::c_int & 1 as ::core::ffi::c_uint) as ::core::ffi::c_int;
        if va == vb {
            if i0 == i1 {
                a64_v_dup_d(b, vd, va, i0);
            } else if i0 == 1 as ::core::ffi::c_int {
                a64_v_ext(b, vd, va, va, 8 as ::core::ffi::c_int);
            } else if vd != va {
                a64_v_mov(b, vd, va);
            }
            return;
        }
        if i0 == 0 as ::core::ffi::c_int && i1 == 0 as ::core::ffi::c_int {
            a64_v_zip1(b, 3 as ::core::ffi::c_int, vd, va, vb);
            return;
        }
        if i0 == 1 as ::core::ffi::c_int && i1 == 1 as ::core::ffi::c_int {
            a64_v_zip2(b, 3 as ::core::ffi::c_int, vd, va, vb);
            return;
        }
        if i0 == 1 as ::core::ffi::c_int && i1 == 0 as ::core::ffi::c_int {
            a64_v_ext(b, vd, va, vb, 8 as ::core::ffi::c_int);
            return;
        }
        if vd == vb {
            a64_ins_d_d(b, vd, 0 as ::core::ffi::c_int, va, 0 as ::core::ffi::c_int);
            return;
        }
        if vd != va {
            a64_v_mov(b, vd, va);
        }
        a64_ins_d_d(b, vd, 1 as ::core::ffi::c_int, vb, 1 as ::core::ffi::c_int);
        return;
    }
    let mut i0_0: ::core::ffi::c_int = (imm & 3 as ::core::ffi::c_uint) as ::core::ffi::c_int;
    let mut i1_0: ::core::ffi::c_int =
        (imm >> 2 as ::core::ffi::c_int & 3 as ::core::ffi::c_uint) as ::core::ffi::c_int;
    let mut i2: ::core::ffi::c_int =
        (imm >> 4 as ::core::ffi::c_int & 3 as ::core::ffi::c_uint) as ::core::ffi::c_int;
    let mut i3: ::core::ffi::c_int =
        (imm >> 6 as ::core::ffi::c_int & 3 as ::core::ffi::c_uint) as ::core::ffi::c_int;
    if vd != va && vd != vb {
        a64_v_dup_s(b, vd, va, i0_0);
    } else {
        a64_ins_s_s(b, vd, 0 as ::core::ffi::c_int, va, i0_0);
    }
    a64_ins_s_s(b, vd, 1 as ::core::ffi::c_int, va, i1_0);
    a64_ins_s_s(b, vd, 2 as ::core::ffi::c_int, vb, i2);
    a64_ins_s_s(b, vd, 3 as ::core::ffi::c_int, vb, i3);
}
unsafe fn emit_sse_shufp(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int
        || (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || (*insn).ops[2 as ::core::ffi::c_int as usize].kind as ::core::ffi::c_int
            != OCERZ_OPK_IMM as ::core::ffi::c_int
        || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if (*insn).op as ::core::ffi::c_int == OCERZ_OP_SHUFPS as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
        && (*s).reg as ::core::ffi::c_int == (*d).reg as ::core::ffi::c_int
    {
        let mut v: ::core::ffi::c_int = xmm_vreg((*d).reg as ::core::ffi::c_uint);
        let mut imm: ::core::ffi::c_uint =
            (*insn).ops[2 as ::core::ffi::c_int as usize].imm as ::core::ffi::c_uint;
        let mut sel: [::core::ffi::c_int; 4] = [
            (imm & 3 as ::core::ffi::c_uint) as ::core::ffi::c_int,
            (imm >> 2 as ::core::ffi::c_int & 3 as ::core::ffi::c_uint) as ::core::ffi::c_int,
            (imm >> 4 as ::core::ffi::c_int & 3 as ::core::ffi::c_uint) as ::core::ffi::c_int,
            (imm >> 6 as ::core::ffi::c_int & 3 as ::core::ffi::c_uint) as ::core::ffi::c_int,
        ];
        let mut changed: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
        let mut k1: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
        let mut k: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
        while k < 4 as ::core::ffi::c_int {
            if sel[k as usize] != k {
                changed += 1;
                k1 = k;
            }
            k += 1;
        }
        if sel[0 as ::core::ffi::c_int as usize] == sel[1 as ::core::ffi::c_int as usize]
            && sel[1 as ::core::ffi::c_int as usize] == sel[2 as ::core::ffi::c_int as usize]
            && sel[2 as ::core::ffi::c_int as usize] == sel[3 as ::core::ffi::c_int as usize]
        {
            a64_v_dup_s(b, v, v, sel[0 as ::core::ffi::c_int as usize]);
            return 1 as ::core::ffi::c_int;
        }
        if changed == 0 as ::core::ffi::c_int {
            return 1 as ::core::ffi::c_int;
        }
        if changed == 1 as ::core::ffi::c_int {
            a64_ins_s_s(b, v, k1, v, sel[k1 as usize]);
            return 1 as ::core::ffi::c_int;
        }
    }
    let mut vb: ::core::ffi::c_int = emit_sse_src_reg(
        b,
        insn,
        s,
        16 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    );
    if vb < 0 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    if (*insn).op as ::core::ffi::c_int == OCERZ_OP_SHUFPS as ::core::ffi::c_int
        && vb != xmm_vreg((*d).reg as ::core::ffi::c_uint)
    {
        let mut vd: ::core::ffi::c_int = xmm_vreg((*d).reg as ::core::ffi::c_uint);
        match (*insn).ops[2 as ::core::ffi::c_int as usize].imm as ::core::ffi::c_uint
            & 0xff as ::core::ffi::c_uint
        {
            68 => {
                a64_v_zip1(b, 3 as ::core::ffi::c_int, vd, vd, vb);
                return 1 as ::core::ffi::c_int;
            }
            238 => {
                a64_v_zip2(b, 3 as ::core::ffi::c_int, vd, vd, vb);
                return 1 as ::core::ffi::c_int;
            }
            136 => {
                a64_v_uzp1(b, 2 as ::core::ffi::c_int, vd, vd, vb);
                return 1 as ::core::ffi::c_int;
            }
            221 => {
                a64_v_uzp2(b, 2 as ::core::ffi::c_int, vd, vd, vb);
                return 1 as ::core::ffi::c_int;
            }
            228 => {
                a64_ins_d_d(b, vd, 1 as ::core::ffi::c_int, vb, 1 as ::core::ffi::c_int);
                return 1 as ::core::ffi::c_int;
            }
            78 => {
                a64_v_ext(b, vd, vd, vb, 8 as ::core::ffi::c_int);
                return 1 as ::core::ffi::c_int;
            }
            _ => {}
        }
    }
    emit_shufp_lane(
        b,
        ((*insn).op as ::core::ffi::c_int == OCERZ_OP_SHUFPD as ::core::ffi::c_int)
            as ::core::ffi::c_int,
        (*insn).ops[2 as ::core::ffi::c_int as usize].imm as ::core::ffi::c_uint,
        VX2 as ::core::ffi::c_int,
        xmm_vreg((*d).reg as ::core::ffi::c_uint),
        vb,
    );
    a64_v_mov(
        b,
        xmm_vreg((*d).reg as ::core::ffi::c_uint),
        VX2 as ::core::ffi::c_int,
    );
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_sse_movddup(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int
        || (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut vs: ::core::ffi::c_int = if (*s).kind as ::core::ffi::c_int
        == OCERZ_OPK_XMM as ::core::ffi::c_int
        && xmm_is_pinned((*s).reg as ::core::ffi::c_uint) != 0
    {
        l0_src2(b, (*s).reg as ::core::ffi::c_uint, 1 as ::core::ffi::c_int)
    } else {
        emit_sse_src_reg(
            b,
            insn,
            s,
            8 as ::core::ffi::c_int,
            VX0 as ::core::ffi::c_int,
            exit_sites,
            n_exits,
        )
    };
    if vs < 0 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    l0_inval((*d).reg as ::core::ffi::c_uint);
    a64_v_dup_d(
        b,
        xmm_vreg((*d).reg as ::core::ffi::c_uint),
        vs,
        0 as ::core::ffi::c_int,
    );
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_insertps_to(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut va: ::core::ffi::c_int,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int
        || (*insn).ops[2 as ::core::ffi::c_int as usize].kind as ::core::ffi::c_int
            != OCERZ_OPK_IMM as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut imm: ::core::ffi::c_uint = (*insn).ops[2 as ::core::ffi::c_int as usize].imm
        as ::core::ffi::c_uint
        & 0xff as ::core::ffi::c_uint;
    let mut vs: ::core::ffi::c_int = 0;
    let mut si: ::core::ffi::c_int =
        (imm >> 6 as ::core::ffi::c_int & 3 as ::core::ffi::c_uint) as ::core::ffi::c_int;
    if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
        if xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        vs = xmm_vreg((*s).reg as ::core::ffi::c_uint);
    } else if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int {
        if emit_sse_src(
            b,
            insn,
            s,
            4 as ::core::ffi::c_int,
            VX1 as ::core::ffi::c_int,
            exit_sites,
            n_exits,
        ) == 0
        {
            return 0 as ::core::ffi::c_int;
        }
        vs = VX1 as ::core::ffi::c_int;
        si = 0 as ::core::ffi::c_int;
    } else {
        return 0 as ::core::ffi::c_int;
    }
    a64_v_mov(b, VX2 as ::core::ffi::c_int, va);
    a64_ins_s_s(
        b,
        VX2 as ::core::ffi::c_int,
        (imm >> 4 as ::core::ffi::c_int & 3 as ::core::ffi::c_uint) as ::core::ffi::c_int,
        vs,
        si,
    );
    let mut i: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    while i < 4 as ::core::ffi::c_int {
        if imm & (1 as ::core::ffi::c_uint) << i != 0 {
            a64_ins_gpr(
                b,
                4 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                i,
                A64_ZR as ::core::ffi::c_int,
            );
        }
        i += 1;
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_sse_insertps(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if emit_insertps_to(
        b,
        insn,
        xmm_vreg((*d).reg as ::core::ffi::c_uint),
        exit_sites,
        n_exits,
    ) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    a64_v_mov(
        b,
        xmm_vreg((*d).reg as ::core::ffi::c_uint),
        VX2 as ::core::ffi::c_int,
    );
    return 1 as ::core::ffi::c_int;
}
unsafe fn mmx_ld(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut s: *const X86Operand,
    mut size: ::core::ffi::c_int,
    mut vt: ::core::ffi::c_int,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MMX as ::core::ffi::c_int {
        a64_ldr_v(
            b,
            8 as ::core::ffi::c_int,
            vt,
            20 as ::core::ffi::c_int,
            (6032 as ::core::ffi::c_ulong as u32)
                .wrapping_add((8 as u32).wrapping_mul((*s).reg as u32)),
        );
        return 1 as ::core::ffi::c_int;
    }
    if emit_plain_mem_fast(
        b,
        insn,
        s,
        size,
        vt,
        0 as ::core::ffi::c_int,
        1 as ::core::ffi::c_int,
    ) != 0
    {
        return 1 as ::core::ffi::c_int;
    }
    let mut skip: *mut u32 = ::core::ptr::null_mut::<u32>();
    if emit_sse_mem_addr(b, insn, s, size, exit_sites, n_exits, &raw mut skip) == 0 {
        return 0 as ::core::ffi::c_int;
    }
    emit_sse_mem_ld(b, size, vt);
    patch_guard_skip(skip, a64_label(b));
    return 1 as ::core::ffi::c_int;
}
unsafe fn mmx_st(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut d: *const X86Operand,
    mut size: ::core::ffi::c_int,
    mut vs: ::core::ffi::c_int,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if emit_plain_mem_fast(
        b,
        insn,
        d,
        size,
        vs,
        1 as ::core::ffi::c_int,
        1 as ::core::ffi::c_int,
    ) != 0
    {
        return 1 as ::core::ffi::c_int;
    }
    let mut skip: *mut u32 = ::core::ptr::null_mut::<u32>();
    if emit_sse_mem_addr(b, insn, d, size, exit_sites, n_exits, &raw mut skip) == 0 {
        return 0 as ::core::ffi::c_int;
    }
    emit_sse_mem_st(b, size, vs);
    patch_guard_skip(skip, a64_label(b));
    return 1 as ::core::ffi::c_int;
}
unsafe fn mmx_enter(mut b: *mut A64Buf) {
    a64_movz(
        b,
        JTU as ::core::ffi::c_int,
        0xff as u16,
        0 as ::core::ffi::c_int,
    );
    a64_str(
        b,
        2 as ::core::ffi::c_int,
        JTU as ::core::ffi::c_int,
        20 as ::core::ffi::c_int,
        5020 as ::core::ffi::c_ulong as u32,
    );
}
unsafe fn mmx_mov(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut r: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    g_vec_int_move = 1 as ::core::ffi::c_int;
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_MMX as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MMX as ::core::ffi::c_int
    {
        a64_ldr(
            b,
            8 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            (6032 as ::core::ffi::c_ulong as u32)
                .wrapping_add((8 as u32).wrapping_mul((*s).reg as u32)),
        );
        a64_str(
            b,
            8 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            (6032 as ::core::ffi::c_ulong as u32)
                .wrapping_add((8 as u32).wrapping_mul((*d).reg as u32)),
        );
        r = 1 as ::core::ffi::c_int;
    } else if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_MMX as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_REG as ::core::ffi::c_int
        && (*s).high8 == 0
        && ((*s).size as ::core::ffi::c_int == 4 as ::core::ffi::c_int
            || (*s).size as ::core::ffi::c_int == 8 as ::core::ffi::c_int)
    {
        emit_gpr_rd(
            b,
            1 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            (*s).reg as ::core::ffi::c_uint,
        );
        if (*s).size as ::core::ffi::c_int == 4 as ::core::ffi::c_int {
            a64_mov_reg(
                b,
                0 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
            );
        }
        a64_str(
            b,
            8 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            (6032 as ::core::ffi::c_ulong as u32)
                .wrapping_add((8 as u32).wrapping_mul((*d).reg as u32)),
        );
        r = 1 as ::core::ffi::c_int;
    } else if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_REG as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MMX as ::core::ffi::c_int
        && (*d).high8 == 0
        && ((*d).size as ::core::ffi::c_int == 4 as ::core::ffi::c_int
            || (*d).size as ::core::ffi::c_int == 8 as ::core::ffi::c_int)
    {
        a64_ldr(
            b,
            (*d).size as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            (6032 as ::core::ffi::c_ulong as u32)
                .wrapping_add((8 as u32).wrapping_mul((*s).reg as u32)),
        );
        emit_gpr_wr(
            b,
            JT0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
        r = 1 as ::core::ffi::c_int;
    } else if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_MMX as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int
        && ((*s).size as ::core::ffi::c_int == 4 as ::core::ffi::c_int
            || (*s).size as ::core::ffi::c_int == 8 as ::core::ffi::c_int)
    {
        if mmx_ld(
            b,
            insn,
            s,
            (*s).size as ::core::ffi::c_int,
            VX0 as ::core::ffi::c_int,
            exit_sites,
            n_exits,
        ) != 0
        {
            a64_str_v(
                b,
                8 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                20 as ::core::ffi::c_int,
                (6032 as ::core::ffi::c_ulong as u32)
                    .wrapping_add((8 as u32).wrapping_mul((*d).reg as u32)),
            );
            r = 1 as ::core::ffi::c_int;
        }
    } else if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MMX as ::core::ffi::c_int
        && ((*d).size as ::core::ffi::c_int == 4 as ::core::ffi::c_int
            || (*d).size as ::core::ffi::c_int == 8 as ::core::ffi::c_int)
    {
        a64_ldr_v(
            b,
            (*d).size as ::core::ffi::c_int,
            VX0 as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            (6032 as ::core::ffi::c_ulong as u32)
                .wrapping_add((8 as u32).wrapping_mul((*s).reg as u32)),
        );
        r = mmx_st(
            b,
            insn,
            d,
            (*d).size as ::core::ffi::c_int,
            VX0 as ::core::ffi::c_int,
            exit_sites,
            n_exits,
        );
    }
    g_vec_int_move = 0 as ::core::ffi::c_int;
    return r;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_mmx(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    static mut off: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    if off < 0 as ::core::ffi::c_int {
        off = if !libc::getenv(c"OCERZ_NO_JIT_MMX".as_ptr()).is_null() {
            1 as ::core::ffi::c_int
        } else {
            0 as ::core::ffi::c_int
        };
    }
    if off != 0
        || (*insn).seg as ::core::ffi::c_int != OCERZ_SEG_NONE as ::core::ffi::c_int
        || ((*insn).nops as ::core::ffi::c_int) < 2 as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut op: ::core::ffi::c_int = (*insn).op as ::core::ffi::c_int;
    if op == OCERZ_OP_MOVD as ::core::ffi::c_int || op == OCERZ_OP_MOVQX as ::core::ffi::c_int {
        if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int
            || mmx_mov(b, insn, exit_sites, n_exits) == 0
        {
            return 0 as ::core::ffi::c_int;
        }
        mmx_enter(b);
        return 1 as ::core::ffi::c_int;
    }
    if op == OCERZ_OP_PEXTRW as ::core::ffi::c_int {
        if (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int
            || (*d).kind as ::core::ffi::c_int != OCERZ_OPK_REG as ::core::ffi::c_int
            || (*d).high8 as ::core::ffi::c_int != 0
            || (*s).kind as ::core::ffi::c_int != OCERZ_OPK_MMX as ::core::ffi::c_int
        {
            return 0 as ::core::ffi::c_int;
        }
        a64_ldr(
            b,
            2 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            (6032 as ::core::ffi::c_ulong as u32)
                .wrapping_add((8 as u32).wrapping_mul((*s).reg as u32))
                .wrapping_add((2 as u32).wrapping_mul(
                    ((*insn).ops[2 as ::core::ffi::c_int as usize].imm & 3 as u64) as u32,
                )),
        );
        emit_gpr_wr(
            b,
            JT0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
        mmx_enter(b);
        return 1 as ::core::ffi::c_int;
    }
    if op == OCERZ_OP_PINSRW as ::core::ffi::c_int {
        if (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int
            || (*d).kind as ::core::ffi::c_int != OCERZ_OPK_MMX as ::core::ffi::c_int
            || (*s).kind as ::core::ffi::c_int != OCERZ_OPK_REG as ::core::ffi::c_int
            || (*s).high8 as ::core::ffi::c_int != 0
        {
            return 0 as ::core::ffi::c_int;
        }
        emit_gpr_rd(
            b,
            1 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            (*s).reg as ::core::ffi::c_uint,
        );
        a64_str(
            b,
            2 as ::core::ffi::c_int,
            JT0 as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            (6032 as ::core::ffi::c_ulong as u32)
                .wrapping_add((8 as u32).wrapping_mul((*d).reg as u32))
                .wrapping_add((2 as u32).wrapping_mul(
                    ((*insn).ops[2 as ::core::ffi::c_int as usize].imm & 3 as u64) as u32,
                )),
        );
        mmx_enter(b);
        return 1 as ::core::ffi::c_int;
    }
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_MMX as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    if op == OCERZ_OP_PSHUFLW as ::core::ffi::c_int {
        if (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int
            || (*s).kind as ::core::ffi::c_int != OCERZ_OPK_MMX as ::core::ffi::c_int
                && (*s).kind as ::core::ffi::c_int != OCERZ_OPK_MEM as ::core::ffi::c_int
        {
            return 0 as ::core::ffi::c_int;
        }
        let mut imm: ::core::ffi::c_uint = (*insn).ops[2 as ::core::ffi::c_int as usize].imm
            as ::core::ffi::c_uint
            & 0xff as ::core::ffi::c_uint;
        if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MMX as ::core::ffi::c_int {
            a64_ldr(
                b,
                8 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                20 as ::core::ffi::c_int,
                (6032 as ::core::ffi::c_ulong as u32)
                    .wrapping_add((8 as u32).wrapping_mul((*s).reg as u32)),
            );
        } else {
            if mmx_ld(
                b,
                insn,
                s,
                8 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
                exit_sites,
                n_exits,
            ) == 0
            {
                return 0 as ::core::ffi::c_int;
            }
            a64_fmov_x_from_v(
                b,
                1 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        let mut i: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
        while i < 4 as ::core::ffi::c_int {
            a64_ubfx(
                b,
                1 as ::core::ffi::c_int,
                JT2 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                16 as ::core::ffi::c_int
                    * (imm >> 2 as ::core::ffi::c_int * i & 3 as ::core::ffi::c_uint)
                        as ::core::ffi::c_int,
                16 as ::core::ffi::c_int,
            );
            if i == 0 as ::core::ffi::c_int {
                a64_mov_reg(
                    b,
                    1 as ::core::ffi::c_int,
                    JT1 as ::core::ffi::c_int,
                    JT2 as ::core::ffi::c_int,
                );
            } else {
                a64_bfi(
                    b,
                    1 as ::core::ffi::c_int,
                    JT1 as ::core::ffi::c_int,
                    JT2 as ::core::ffi::c_int,
                    16 as ::core::ffi::c_int * i,
                    16 as ::core::ffi::c_int,
                );
            }
            i += 1;
        }
        a64_str(
            b,
            8 as ::core::ffi::c_int,
            JT1 as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            (6032 as ::core::ffi::c_ulong as u32)
                .wrapping_add((8 as u32).wrapping_mul((*d).reg as u32)),
        );
        mmx_enter(b);
        return 1 as ::core::ffi::c_int;
    }
    let mut shift_imm: ::core::ffi::c_int = ((*insn).nops as ::core::ffi::c_int
        == 2 as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_IMM as ::core::ffi::c_int)
        as ::core::ffi::c_int;
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int
        || shift_imm == 0
            && (*s).kind as ::core::ffi::c_int != OCERZ_OPK_MMX as ::core::ffi::c_int
            && (*s).kind as ::core::ffi::c_int != OCERZ_OPK_MEM as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    if shift_imm != 0 {
        let mut esz: ::core::ffi::c_int = 0;
        let mut kind: ::core::ffi::c_int = 0;
        match op {
            356 => {
                esz = 1 as ::core::ffi::c_int;
                kind = 0 as ::core::ffi::c_int;
            }
            357 => {
                esz = 2 as ::core::ffi::c_int;
                kind = 0 as ::core::ffi::c_int;
            }
            358 => {
                esz = 3 as ::core::ffi::c_int;
                kind = 0 as ::core::ffi::c_int;
            }
            359 => {
                esz = 1 as ::core::ffi::c_int;
                kind = 1 as ::core::ffi::c_int;
            }
            360 => {
                esz = 2 as ::core::ffi::c_int;
                kind = 1 as ::core::ffi::c_int;
            }
            361 => {
                esz = 3 as ::core::ffi::c_int;
                kind = 1 as ::core::ffi::c_int;
            }
            362 => {
                esz = 1 as ::core::ffi::c_int;
                kind = 2 as ::core::ffi::c_int;
            }
            363 => {
                esz = 2 as ::core::ffi::c_int;
                kind = 2 as ::core::ffi::c_int;
            }
            _ => return 0 as ::core::ffi::c_int,
        }
        let mut n: ::core::ffi::c_uint =
            (*s).imm as ::core::ffi::c_uint & 0xff as ::core::ffi::c_uint;
        let mut w: ::core::ffi::c_uint = (8 as ::core::ffi::c_uint) << esz;
        a64_ldr_v(
            b,
            8 as ::core::ffi::c_int,
            VX0 as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            (6032 as ::core::ffi::c_ulong as u32)
                .wrapping_add((8 as u32).wrapping_mul((*d).reg as u32)),
        );
        if !(n == 0 as ::core::ffi::c_uint) {
            if kind == 2 as ::core::ffi::c_int {
                a64_v_sshr_imm(
                    b,
                    esz,
                    VX0 as ::core::ffi::c_int,
                    VX0 as ::core::ffi::c_int,
                    (if n >= w { w } else { n }) as ::core::ffi::c_int,
                );
            } else if n >= w {
                a64_v_zero(b, VX0 as ::core::ffi::c_int);
            } else if kind == 0 as ::core::ffi::c_int {
                a64_v_shl_imm(
                    b,
                    esz,
                    VX0 as ::core::ffi::c_int,
                    VX0 as ::core::ffi::c_int,
                    n as ::core::ffi::c_int,
                );
            } else {
                a64_v_ushr_imm(
                    b,
                    esz,
                    VX0 as ::core::ffi::c_int,
                    VX0 as ::core::ffi::c_int,
                    n as ::core::ffi::c_int,
                );
            }
        }
        a64_str_v(
            b,
            8 as ::core::ffi::c_int,
            VX0 as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            (6032 as ::core::ffi::c_ulong as u32)
                .wrapping_add((8 as u32).wrapping_mul((*d).reg as u32)),
        );
        mmx_enter(b);
        return 1 as ::core::ffi::c_int;
    }
    match op {
        282 | 283 | 284 | 285 | 286 | 287 | 288 | 289 | 290 | 291 | 292 | 293 | 294 | 295 | 296
        | 297 | 298 | 300 | 301 | 246 | 248 | 249 | 247 | 258 | 259 | 260 | 262 | 263 | 264
        | 326 | 327 | 328 | 330 | 331 | 332 | 324 | 322 | 323 | 318 | 304 | 305 | 308 | 306
        | 309 | 307 => {}
        _ => return 0 as ::core::ffi::c_int,
    }
    if op == OCERZ_OP_PXOR as ::core::ffi::c_int
        && (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MMX as ::core::ffi::c_int
        && (*s).reg as ::core::ffi::c_int == (*d).reg as ::core::ffi::c_int
    {
        a64_str(
            b,
            8 as ::core::ffi::c_int,
            31 as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            (6032 as ::core::ffi::c_ulong as u32)
                .wrapping_add((8 as u32).wrapping_mul((*d).reg as u32)),
        );
        mmx_enter(b);
        return 1 as ::core::ffi::c_int;
    }
    a64_ldr_v(
        b,
        8 as ::core::ffi::c_int,
        VX0 as ::core::ffi::c_int,
        20 as ::core::ffi::c_int,
        (6032 as ::core::ffi::c_ulong as u32)
            .wrapping_add((8 as u32).wrapping_mul((*d).reg as u32)),
    );
    if mmx_ld(
        b,
        insn,
        s,
        8 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    ) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    match op {
        282 => {
            a64_v_add(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        283 => {
            a64_v_add(
                b,
                1 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        284 => {
            a64_v_add(
                b,
                2 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        285 => {
            a64_v_add(
                b,
                3 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        286 => {
            a64_v_sub(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        287 => {
            a64_v_sub(
                b,
                1 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        288 => {
            a64_v_sub(
                b,
                2 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        289 => {
            a64_v_sub(
                b,
                3 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        290 => {
            a64_v_sqadd(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        291 => {
            a64_v_sqadd(
                b,
                1 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        292 => {
            a64_v_uqadd(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        293 => {
            a64_v_uqadd(
                b,
                1 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        294 => {
            a64_v_sqsub(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        295 => {
            a64_v_sqsub(
                b,
                1 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        296 => {
            a64_v_uqsub(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        297 => {
            a64_v_uqsub(
                b,
                1 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        298 => {
            a64_v_mul(
                b,
                1 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        300 => {
            a64_v_smull_h(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
            a64_v_uzp(
                b,
                1 as ::core::ffi::c_int,
                1 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
            );
        }
        301 => {
            a64_v_umull_h(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
            a64_v_uzp(
                b,
                1 as ::core::ffi::c_int,
                1 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
            );
        }
        246 => {
            a64_v_and(
                b,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        248 => {
            a64_v_orr(
                b,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        249 => {
            a64_v_eor(
                b,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        247 => {
            a64_v_bic(
                b,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
            );
        }
        258 => {
            a64_v_cmeq(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        259 => {
            a64_v_cmeq(
                b,
                1 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        260 => {
            a64_v_cmeq(
                b,
                2 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        262 => {
            a64_v_cmgt(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        263 => {
            a64_v_cmgt(
                b,
                1 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        264 => {
            a64_v_cmgt(
                b,
                2 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        326 => {
            a64_v_zip1(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        327 => {
            a64_v_zip1(
                b,
                1 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        328 => {
            a64_v_zip1(
                b,
                2 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        330 => {
            a64_v_zip1(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
            a64_v_dup_d(
                b,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                1 as ::core::ffi::c_int,
            );
        }
        331 => {
            a64_v_zip1(
                b,
                1 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
            a64_v_dup_d(
                b,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                1 as ::core::ffi::c_int,
            );
        }
        332 => {
            a64_v_zip1(
                b,
                2 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
            a64_v_dup_d(
                b,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                1 as ::core::ffi::c_int,
            );
        }
        324 => {
            a64_v_zip1(
                b,
                3 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
            a64_v_sqxtun_h(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
            );
        }
        322 => {
            a64_v_zip1(
                b,
                3 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
            a64_v_sqxtn_h(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
            );
        }
        323 => {
            a64_v_zip1(
                b,
                3 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
            a64_v_sqxtn_s(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
            );
        }
        318 => {
            a64_v_uabd(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
            a64_v_uaddlp(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
            );
            a64_v_uaddlp(
                b,
                1 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
            );
            a64_v_uaddlp(
                b,
                2 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
            );
        }
        304 => {
            a64_v_urhadd(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        305 => {
            a64_v_urhadd(
                b,
                1 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        308 => {
            a64_v_umin(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        306 => {
            a64_v_umax(
                b,
                0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        309 => {
            a64_v_smin(
                b,
                1 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        307 => {
            a64_v_smax(
                b,
                1 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
            );
        }
        _ => return 0 as ::core::ffi::c_int,
    }
    a64_str_v(
        b,
        8 as ::core::ffi::c_int,
        VX0 as ::core::ffi::c_int,
        20 as ::core::ffi::c_int,
        (6032 as ::core::ffi::c_ulong as u32)
            .wrapping_add((8 as u32).wrapping_mul((*d).reg as u32)),
    );
    mmx_enter(b);
    return 1 as ::core::ffi::c_int;
}
#[inline]
unsafe fn ocerz_insn_has_mmx(insn: *const X86Insn) -> ::core::ffi::c_int {
    let mut i: ::core::ffi::c_int = 0;
    while i < (*insn).nops as ::core::ffi::c_int {
        let op = (&raw const (*insn).ops as *const X86Operand).offset(i as isize);
        if (*op).kind as ::core::ffi::c_int == OCERZ_OPK_MMX as ::core::ffi::c_int {
            return 1;
        }
        i += 1;
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_sse(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if ocerz_insn_has_mmx(insn) != 0 {
        return if (*insn).mode32 as ::core::ffi::c_int != 0 {
            0 as ::core::ffi::c_int
        } else {
            emit_mmx(b, insn, exit_sites, n_exits)
        };
    }
    if sse_enabled() == 0 {
        return 0 as ::core::ffi::c_int;
    }
    if (*insn).seg as ::core::ffi::c_int != OCERZ_SEG_NONE as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    match (*insn).op as ::core::ffi::c_int {
        185 | 186 | 187 | 188 => return emit_sse_mov128(b, insn, exit_sites, n_exits),
        189 => {
            return emit_sse_movs(b, insn, 4 as ::core::ffi::c_int, exit_sites, n_exits);
        }
        190 => {
            return emit_sse_movs(b, insn, 8 as ::core::ffi::c_int, exit_sites, n_exits);
        }
        193 | 194 => return emit_sse_movlh(b, insn, exit_sites, n_exits),
        206 | 207 | 204 | 205 | 210 | 211 | 208 | 209 | 214 | 215 | 212 | 213 | 218 | 219 | 216
        | 217 | 226 | 227 | 222 | 223 | 224 | 225 | 220 | 221 | 230 | 231 | 228 | 229 => {
            return emit_sse_fparith(b, insn, exit_sites, n_exits);
        }
        249 | 245 | 246 | 242 | 248 | 244 | 247 | 243 | 282 | 283 | 284 | 285 | 286 | 287 | 288
        | 289 | 258 | 259 | 260 | 261 | 262 | 263 | 264 | 265 | 308 | 316 | 317 | 306 | 312
        | 313 | 314 | 309 | 315 | 310 | 307 | 311 | 298 | 299 | 304 | 305 | 292 | 293 | 296
        | 297 | 290 | 291 | 294 | 295 | 303 | 348 | 323 | 324 | 302 | 300 | 301 | 322 | 325
        | 421 | 318 | 347 | 319 | 320 | 321 | 344 | 345 | 346 | 338 | 339 | 341 | 342 | 340
        | 343 => {
            return emit_sse_bitwise(b, insn, exit_sites, n_exits);
        }
        393 | 349 => return emit_sse_pblendw_palignr(b, insn, exit_sites, n_exits),
        335 | 336 => return emit_sse_pshuflhw(b, insn, exit_sites, n_exits),
        364 | 365 => return emit_sse_bytesh(b, insn),
        394 | 395 => return emit_sse_blendp(b, insn, exit_sites, n_exits),
        200 | 201 => return emit_sse_movsdup(b, insn, exit_sites, n_exits),
        197 | 198 => return emit_sse_movmskp(b, insn),
        250 | 251 => return emit_sse_cmpp(b, insn, exit_sites, n_exits),
        278 | 277 | 279 | 274 | 275 => return emit_sse_cvtp(b, insn, exit_sites, n_exits),
        399 | 400 | 401 | 402 | 403 | 404 => {
            return emit_sse_aes(b, insn, exit_sites, n_exits);
        }
        405 => return emit_sse_pclmul(b, insn, exit_sites, n_exits),
        256 | 257 | 254 | 255 => return emit_sse_comis(b, insn, exit_sites, n_exits),
        271 | 270 | 267 | 266 | 273 | 272 | 276 => {
            return emit_sse_cvt(b, insn, exit_sites, n_exits);
        }
        192 => {
            g_vec_int_move = 1 as ::core::ffi::c_int;
            let mut r: ::core::ffi::c_int = emit_sse_movq(b, insn, exit_sites, n_exits);
            g_vec_int_move = 0 as ::core::ffi::c_int;
            return r;
        }
        334 => return emit_sse_pshufd(b, insn, exit_sites, n_exits),
        370 | 371 | 372 | 373 | 366 | 367 | 368 | 369 => {
            return emit_sse_pinsr_pextr(b, insn, exit_sites, n_exits);
        }
        383 | 384 | 385 | 386 | 387 | 388 | 377 | 378 | 379 | 380 | 381 | 382 => {
            return emit_sse_pmovx(b, insn, exit_sites, n_exits);
        }
        391 | 392 | 389 | 390 => return emit_sse_round(b, insn, exit_sites, n_exits),
        337 => return emit_sse_pshufb(b, insn, exit_sites, n_exits),
        326 | 327 | 328 | 329 | 330 | 331 | 332 | 333 => {
            return emit_sse_punpck(b, insn, exit_sites, n_exits);
        }
        191 => {
            g_vec_int_move = 1 as ::core::ffi::c_int;
            let mut r_0: ::core::ffi::c_int = emit_sse_movd(b, insn, exit_sites, n_exits);
            g_vec_int_move = 0 as ::core::ffi::c_int;
            return r_0;
        }
        354 | 355 | 195 | 196 | 352 | 353 => {
            return emit_sse_unpck(b, insn, exit_sites, n_exits);
        }
        252 | 253 => return emit_sse_cmps(b, insn, exit_sites, n_exits),
        397 | 396 | 398 => return emit_sse_blendv(b, insn, exit_sites, n_exits),
        202 => return emit_sse_movddup(b, insn, exit_sites, n_exits),
        350 | 351 => return emit_sse_shufp(b, insn, exit_sites, n_exits),
        375 => return emit_sse_insertps(b, insn, exit_sites, n_exits),
        356 | 357 | 358 | 359 | 360 | 361 | 362 | 363 => {
            return emit_sse_shift_imm(b, insn);
        }
        _ => return 0 as ::core::ffi::c_int,
    };
}
unsafe fn emit_sse_pinsr_pextr(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int || sse_enabled() == 0 {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut op: ::core::ffi::c_uint = (*insn).op as ::core::ffi::c_uint;
    let mut esize: ::core::ffi::c_int = if op
        == OCERZ_OP_PINSRB as ::core::ffi::c_int as ::core::ffi::c_uint
        || op == OCERZ_OP_PEXTRB as ::core::ffi::c_int as ::core::ffi::c_uint
    {
        1 as ::core::ffi::c_int
    } else if op == OCERZ_OP_PINSRW as ::core::ffi::c_int as ::core::ffi::c_uint
        || op == OCERZ_OP_PEXTRW as ::core::ffi::c_int as ::core::ffi::c_uint
    {
        2 as ::core::ffi::c_int
    } else if op == OCERZ_OP_PINSRD as ::core::ffi::c_int as ::core::ffi::c_uint
        || op == OCERZ_OP_PEXTRD as ::core::ffi::c_int as ::core::ffi::c_uint
    {
        4 as ::core::ffi::c_int
    } else {
        8 as ::core::ffi::c_int
    };
    let mut lanes: ::core::ffi::c_uint =
        (16 as ::core::ffi::c_uint).wrapping_div(esize as ::core::ffi::c_uint);
    let mut idx: ::core::ffi::c_uint = (*insn).ops[2 as ::core::ffi::c_int as usize].imm
        as ::core::ffi::c_uint
        & lanes.wrapping_sub(1 as ::core::ffi::c_uint);
    if op == OCERZ_OP_PINSRB as ::core::ffi::c_int as ::core::ffi::c_uint
        || op == OCERZ_OP_PINSRW as ::core::ffi::c_int as ::core::ffi::c_uint
        || op == OCERZ_OP_PINSRD as ::core::ffi::c_int as ::core::ffi::c_uint
        || op == OCERZ_OP_PINSRQ as ::core::ffi::c_int as ::core::ffi::c_uint
    {
        if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
            || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
        {
            return 0 as ::core::ffi::c_int;
        }
        l0_flush_reg(b, (*d).reg as ::core::ffi::c_uint);
        l0_inval((*d).reg as ::core::ffi::c_uint);
        let mut vd: ::core::ffi::c_int = xmm_vreg((*d).reg as ::core::ffi::c_uint);
        if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_REG as ::core::ffi::c_int {
            if (*s).high8 != 0 {
                return 0 as ::core::ffi::c_int;
            }
            emit_gpr_rd(
                b,
                1 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
                (*s).reg as ::core::ffi::c_uint,
            );
            a64_ins_gpr(
                b,
                esize,
                vd,
                idx as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
            );
            return 1 as ::core::ffi::c_int;
        }
        if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int {
            if emit_mem_load_any(b, insn, s, esize, JT0 as ::core::ffi::c_int) == 0 {
                return 0 as ::core::ffi::c_int;
            }
            a64_ins_gpr(
                b,
                esize,
                vd,
                idx as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
            );
            return 1 as ::core::ffi::c_int;
        }
        return 0 as ::core::ffi::c_int;
    }
    if (*s).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut vs: ::core::ffi::c_int = xmm_vreg((*s).reg as ::core::ffi::c_uint);
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_REG as ::core::ffi::c_int {
        if (*d).high8 != 0 {
            return 0 as ::core::ffi::c_int;
        }
        a64_umov_gpr(
            b,
            esize,
            JT0 as ::core::ffi::c_int,
            vs,
            idx as ::core::ffi::c_int,
        );
        emit_gpr_wr(
            b,
            JT0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
        return 1 as ::core::ffi::c_int;
    }
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int {
        a64_umov_gpr(
            b,
            esize,
            JT0 as ::core::ffi::c_int,
            vs,
            idx as ::core::ffi::c_int,
        );
        if emit_plain_mem_fast(
            b,
            insn,
            d,
            esize,
            JT0 as ::core::ffi::c_int,
            1 as ::core::ffi::c_int,
            0 as ::core::ffi::c_int,
        ) != 0
        {
            return 1 as ::core::ffi::c_int;
        }
        let mut skip: *mut u32 = ::core::ptr::null_mut::<u32>();
        if emit_sse_mem_addr(b, insn, d, esize, exit_sites, n_exits, &raw mut skip) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        a64_umov_gpr(
            b,
            esize,
            JT1 as ::core::ffi::c_int,
            vs,
            idx as ::core::ffi::c_int,
        );
        emit_sse_mem_st_gpr(b, esize, JT1 as ::core::ffi::c_int);
        patch_guard_skip(skip, a64_label(b));
        return 1 as ::core::ffi::c_int;
    }
    return 0 as ::core::ffi::c_int;
}
unsafe fn emit_sse_pmovx(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int || sse_enabled() == 0 {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut op: ::core::ffi::c_uint = (*insn).op as ::core::ffi::c_uint;
    let mut sx: ::core::ffi::c_int = (op
        == OCERZ_OP_PMOVSXBW as ::core::ffi::c_int as ::core::ffi::c_uint
        || op == OCERZ_OP_PMOVSXBD as ::core::ffi::c_int as ::core::ffi::c_uint
        || op == OCERZ_OP_PMOVSXBQ as ::core::ffi::c_int as ::core::ffi::c_uint
        || op == OCERZ_OP_PMOVSXWD as ::core::ffi::c_int as ::core::ffi::c_uint
        || op == OCERZ_OP_PMOVSXWQ as ::core::ffi::c_int as ::core::ffi::c_uint
        || op == OCERZ_OP_PMOVSXDQ as ::core::ffi::c_int as ::core::ffi::c_uint)
        as ::core::ffi::c_int;
    let mut from: ::core::ffi::c_int = 0;
    let mut steps: ::core::ffi::c_int = 0;
    let mut srcw: ::core::ffi::c_int = 0;
    match op {
        383 | 377 => {
            from = 1 as ::core::ffi::c_int;
            steps = 1 as ::core::ffi::c_int;
            srcw = 8 as ::core::ffi::c_int;
        }
        384 | 378 => {
            from = 1 as ::core::ffi::c_int;
            steps = 2 as ::core::ffi::c_int;
            srcw = 4 as ::core::ffi::c_int;
        }
        385 | 379 => {
            from = 1 as ::core::ffi::c_int;
            steps = 3 as ::core::ffi::c_int;
            srcw = 2 as ::core::ffi::c_int;
        }
        386 | 380 => {
            from = 2 as ::core::ffi::c_int;
            steps = 1 as ::core::ffi::c_int;
            srcw = 8 as ::core::ffi::c_int;
        }
        387 | 381 => {
            from = 2 as ::core::ffi::c_int;
            steps = 2 as ::core::ffi::c_int;
            srcw = 4 as ::core::ffi::c_int;
        }
        388 | 382 => {
            from = 4 as ::core::ffi::c_int;
            steps = 1 as ::core::ffi::c_int;
            srcw = 8 as ::core::ffi::c_int;
        }
        _ => return 0 as ::core::ffi::c_int,
    }
    let mut vsrc: ::core::ffi::c_int = 0;
    if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
        if xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        vsrc = xmm_vreg((*s).reg as ::core::ffi::c_uint);
    } else if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int {
        let mut skip: *mut u32 = ::core::ptr::null_mut::<u32>();
        if srcw == 2 as ::core::ffi::c_int {
            if emit_mem_load_any(
                b,
                insn,
                s,
                2 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
            ) == 0
            {
                return 0 as ::core::ffi::c_int;
            }
            a64_fmov_v_from_x(
                b,
                0 as ::core::ffi::c_int,
                VX1 as ::core::ffi::c_int,
                JT0 as ::core::ffi::c_int,
            );
        } else {
            if emit_sse_mem_addr(b, insn, s, srcw, exit_sites, n_exits, &raw mut skip) == 0 {
                return 0 as ::core::ffi::c_int;
            }
            emit_sse_mem_ld(b, srcw, VX1 as ::core::ffi::c_int);
            patch_guard_skip(skip, a64_label(b));
        }
        vsrc = VX1 as ::core::ffi::c_int;
    } else {
        return 0 as ::core::ffi::c_int;
    }
    let mut vd: ::core::ffi::c_int = xmm_vreg((*d).reg as ::core::ffi::c_uint);
    let mut cur: ::core::ffi::c_int = vsrc;
    let mut e: ::core::ffi::c_int = from;
    let mut k: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    while k < steps {
        a64_v_xtl(b, sx, e, vd, cur);
        cur = vd;
        e *= 2 as ::core::ffi::c_int;
        k += 1;
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_sse_round(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int || sse_enabled() == 0 {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut op: ::core::ffi::c_uint = (*insn).op as ::core::ffi::c_uint;
    let mut imm: ::core::ffi::c_uint =
        (*insn).ops[2 as ::core::ffi::c_int as usize].imm as ::core::ffi::c_uint;
    let mut mode: ::core::ffi::c_int = if imm & 4 as ::core::ffi::c_uint != 0 {
        4 as ::core::ffi::c_int
    } else {
        (imm & 3 as ::core::ffi::c_uint) as ::core::ffi::c_int
    };
    let mut mode_a64: ::core::ffi::c_int = if mode == 0 as ::core::ffi::c_int {
        0 as ::core::ffi::c_int
    } else if mode == 1 as ::core::ffi::c_int {
        1 as ::core::ffi::c_int
    } else if mode == 2 as ::core::ffi::c_int {
        2 as ::core::ffi::c_int
    } else if mode == 3 as ::core::ffi::c_int {
        3 as ::core::ffi::c_int
    } else {
        4 as ::core::ffi::c_int
    };
    let mut dbl: ::core::ffi::c_int = (op
        == OCERZ_OP_ROUNDSD as ::core::ffi::c_int as ::core::ffi::c_uint
        || op == OCERZ_OP_ROUNDPD as ::core::ffi::c_int as ::core::ffi::c_uint)
        as ::core::ffi::c_int;
    let mut packed: ::core::ffi::c_int = (op
        == OCERZ_OP_ROUNDPS as ::core::ffi::c_int as ::core::ffi::c_uint
        || op == OCERZ_OP_ROUNDPD as ::core::ffi::c_int as ::core::ffi::c_uint)
        as ::core::ffi::c_int;
    let mut vd: ::core::ffi::c_int = xmm_vreg((*d).reg as ::core::ffi::c_uint);
    if packed != 0 {
        let mut vs: ::core::ffi::c_int = emit_sse_src_reg(
            b,
            insn,
            s,
            16 as ::core::ffi::c_int,
            VX1 as ::core::ffi::c_int,
            exit_sites,
            n_exits,
        );
        if vs < 0 as ::core::ffi::c_int {
            return 0 as ::core::ffi::c_int;
        }
        a64_v_frint(b, dbl, mode_a64, vd, vs);
        return 1 as ::core::ffi::c_int;
    }
    let mut esz: ::core::ffi::c_int = if dbl != 0 {
        8 as ::core::ffi::c_int
    } else {
        4 as ::core::ffi::c_int
    };
    let mut vs_0: ::core::ffi::c_int = 0;
    if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
        && xmm_is_pinned((*s).reg as ::core::ffi::c_uint) != 0
    {
        vs_0 = xmm_vreg((*s).reg as ::core::ffi::c_uint);
    } else {
        if emit_sse_src(
            b,
            insn,
            s,
            esz,
            VX1 as ::core::ffi::c_int,
            exit_sites,
            n_exits,
        ) == 0
        {
            return 0 as ::core::ffi::c_int;
        }
        vs_0 = VX1 as ::core::ffi::c_int;
    }
    a64_frint_s(b, dbl, mode_a64, VX0 as ::core::ffi::c_int, vs_0);
    if dbl != 0 {
        a64_ins_d_d(
            b,
            vd,
            0 as ::core::ffi::c_int,
            VX0 as ::core::ffi::c_int,
            0 as ::core::ffi::c_int,
        );
    } else {
        a64_ins_s_s(
            b,
            vd,
            0 as ::core::ffi::c_int,
            VX0 as ::core::ffi::c_int,
            0 as ::core::ffi::c_int,
        );
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_mskb_bits(mut b: *mut A64Buf, mut vt: ::core::ffi::c_int) {
    g_raslit[g_n_raslit as usize].site = a64_label(b);
    g_raslit[g_n_raslit as usize].retaddr = 0x8040201008040201 as ::core::ffi::c_ulonglong as u64;
    g_raslit[g_n_raslit as usize].hi = 0x8040201008040201 as ::core::ffi::c_ulonglong as u64;
    g_raslit[g_n_raslit as usize].kind = 2 as ::core::ffi::c_int;
    g_raslit[g_n_raslit as usize].rt = vt;
    g_n_raslit += 1;
    a64_emit32(b, 0x9c000000 as u32 | vt as u32);
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_pmovmskb(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
) -> ::core::ffi::c_int {
    if sse_enabled() == 0 || (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_REG as ::core::ffi::c_int
        || (*d).high8 as ::core::ffi::c_int != 0
        || (*d).size as ::core::ffi::c_int != 4 as ::core::ffi::c_int
            && (*d).size as ::core::ffi::c_int != 8 as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    if (*s).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if g_n_raslit >= RASLIT_MAX as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut ds: ::core::ffi::c_int = pin_slot((*d).reg as ::core::ffi::c_uint);
    if ds < 0 as ::core::ffi::c_int
        || rsp_is_ptr() != 0 && (*d).reg as ::core::ffi::c_int == OCERZ_RSP as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    a64_v_sshr_16b(
        b,
        VX0 as ::core::ffi::c_int,
        xmm_vreg((*s).reg as ::core::ffi::c_uint),
        7 as ::core::ffi::c_int,
    );
    emit_mskb_bits(b, VX1 as ::core::ffi::c_int);
    a64_v_and(
        b,
        VX0 as ::core::ffi::c_int,
        VX0 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
    );
    a64_v_addp_16b(
        b,
        VX0 as ::core::ffi::c_int,
        VX0 as ::core::ffi::c_int,
        VX0 as ::core::ffi::c_int,
    );
    a64_v_addp_16b(
        b,
        VX0 as ::core::ffi::c_int,
        VX0 as ::core::ffi::c_int,
        VX0 as ::core::ffi::c_int,
    );
    a64_v_addp_16b(
        b,
        VX0 as ::core::ffi::c_int,
        VX0 as ::core::ffi::c_int,
        VX0 as ::core::ffi::c_int,
    );
    a64_umov_w_h(
        b,
        pin_hreg(ds),
        VX0 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    return 1 as ::core::ffi::c_int;
}
unsafe fn vex_inline_enabled() -> ::core::ffi::c_int {
    static mut on: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    if on < 0 as ::core::ffi::c_int {
        on = if !libc::getenv(c"OCERZ_NO_INLINE_VEX".as_ptr()).is_null() {
            0 as ::core::ffi::c_int
        } else {
            1 as ::core::ffi::c_int
        };
    }
    return on;
}
#[unsafe(no_mangle)]
pub static mut g_ymmh_zero: u16 = 0;
unsafe fn ymmh_src(
    mut b: *mut A64Buf,
    mut xr: ::core::ffi::c_uint,
    mut vtmp: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if g_yc[xr as usize] as ::core::ffi::c_int >= 0 as ::core::ffi::c_int {
        return g_yc[xr as usize] as ::core::ffi::c_int;
    }
    if g_ymmh_zero as ::core::ffi::c_uint & (1 as ::core::ffi::c_uint) << xr != 0 {
        a64_v_zero(b, vtmp);
        return vtmp;
    }
    a64_ldr_v(
        b,
        16 as ::core::ffi::c_int,
        vtmp,
        20 as ::core::ffi::c_int,
        YMMH_OFF.wrapping_add((xr as u32).wrapping_mul(16 as u32)),
    );
    return vtmp;
}
#[inline]
unsafe fn ymmh_dst(
    mut xr: ::core::ffi::c_uint,
    mut vtmp: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    return if g_yc[xr as usize] as ::core::ffi::c_int >= 0 as ::core::ffi::c_int {
        g_yc[xr as usize] as ::core::ffi::c_int
    } else {
        vtmp
    };
}
unsafe fn emit_ymmh_ld(
    mut b: *mut A64Buf,
    mut vd: ::core::ffi::c_int,
    mut xr: ::core::ffi::c_uint,
) {
    let mut v: ::core::ffi::c_int = ymmh_src(b, xr, vd);
    if v != vd {
        a64_v_mov(b, vd, v);
    }
}
unsafe fn emit_ymmh_st(
    mut b: *mut A64Buf,
    mut vs: ::core::ffi::c_int,
    mut xr: ::core::ffi::c_uint,
) {
    g_ymmh_zero = (g_ymmh_zero as ::core::ffi::c_int
        & !((1 as ::core::ffi::c_uint) << xr) as u16 as ::core::ffi::c_int)
        as u16;
    if g_yc[xr as usize] as ::core::ffi::c_int >= 0 as ::core::ffi::c_int {
        if vs != g_yc[xr as usize] as ::core::ffi::c_int {
            a64_v_mov(b, g_yc[xr as usize] as ::core::ffi::c_int, vs);
        }
        g_yc_dirty = (g_yc_dirty as ::core::ffi::c_int
            | ((1 as ::core::ffi::c_uint) << xr) as u16 as ::core::ffi::c_int)
            as u16;
        return;
    }
    a64_str_v(
        b,
        16 as ::core::ffi::c_int,
        vs,
        20 as ::core::ffi::c_int,
        YMMH_OFF.wrapping_add((xr as u32).wrapping_mul(16 as u32)),
    );
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_ymmh_clear(mut b: *mut A64Buf, mut xr: ::core::ffi::c_uint) {
    if g_ymmh_zero as ::core::ffi::c_uint & (1 as ::core::ffi::c_uint) << xr != 0 {
        return;
    }
    if g_yc[xr as usize] as ::core::ffi::c_int >= 0 as ::core::ffi::c_int {
        a64_v_zero(b, g_yc[xr as usize] as ::core::ffi::c_int);
        g_yc_dirty = (g_yc_dirty as ::core::ffi::c_int
            | ((1 as ::core::ffi::c_uint) << xr) as u16 as ::core::ffi::c_int)
            as u16;
    } else if g_zero_vreg >= 0 as ::core::ffi::c_int {
        a64_str_v(
            b,
            16 as ::core::ffi::c_int,
            g_zero_vreg,
            20 as ::core::ffi::c_int,
            YMMH_OFF.wrapping_add((xr as u32).wrapping_mul(16 as u32)),
        );
    } else {
        a64_str(
            b,
            8 as ::core::ffi::c_int,
            A64_ZR as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            YMMH_OFF.wrapping_add((xr as u32).wrapping_mul(16 as u32)),
        );
        a64_str(
            b,
            8 as ::core::ffi::c_int,
            A64_ZR as ::core::ffi::c_int,
            20 as ::core::ffi::c_int,
            YMMH_OFF
                .wrapping_add((xr as u32).wrapping_mul(16 as u32))
                .wrapping_add(8 as u32),
        );
    }
    g_ymmh_zero = (g_ymmh_zero as ::core::ffi::c_int
        | ((1 as ::core::ffi::c_uint) << xr) as u16 as ::core::ffi::c_int) as u16;
}
static mut g_vex_mem_skip: *mut u32 = ::core::ptr::null::<u32>() as *mut u32;
unsafe fn emit_vex_mem_addr(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut m: *const X86Operand,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if emit_sse_mem_addr(
        b,
        insn,
        m,
        16 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
        &raw mut g_vex_mem_skip,
    ) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut dsp: i32 = g_sse_mem_disp as i32;
    let mut hi: i32 = dsp + 16 as i32;
    let mut plain: ::core::ffi::c_int =
        (g_sse_mem_plainacc != 0 || vec_tso_relaxed() != 0) as ::core::ffi::c_int;
    let mut lo_ok: ::core::ffi::c_int =
        (dsp >= 0 as i32 && dsp & 15 as i32 == 0 as i32 && dsp / 16 as i32 <= 4095 as i32
            || dsp >= -(256 as i32) && dsp <= 255 as i32) as ::core::ffi::c_int;
    let mut hi_ok: ::core::ffi::c_int =
        (hi >= 0 as i32 && hi & 15 as i32 == 0 as i32 && hi / 16 as i32 <= 4095 as i32
            || hi >= -(256 as i32) && hi <= 255 as i32) as ::core::ffi::c_int;
    if lo_ok == 0 || hi_ok == 0 || plain == 0 && dsp != 0 as i32 {
        if dsp > 0 as i32 && dsp <= 4095 as i32 {
            a64_add_imm(
                b,
                1 as ::core::ffi::c_int,
                JTA as ::core::ffi::c_int,
                g_sse_mem_ra,
                dsp as u32,
            );
        } else if dsp < 0 as i32 && -dsp <= 4095 as i32 {
            a64_sub_imm(
                b,
                1 as ::core::ffi::c_int,
                JTA as ::core::ffi::c_int,
                g_sse_mem_ra,
                -dsp as u32,
            );
        } else {
            a64_mov_imm64(b, JTU as ::core::ffi::c_int, dsp as i64 as u64);
            a64_add_reg(
                b,
                1 as ::core::ffi::c_int,
                JTA as ::core::ffi::c_int,
                g_sse_mem_ra,
                JTU as ::core::ffi::c_int,
                0 as ::core::ffi::c_int,
            );
        }
        ea_cache_reset();
        g_sse_mem_ra = JTA as ::core::ffi::c_int;
        g_sse_mem_disp = 0 as u32;
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_vex_mem_acc(
    mut b: *mut A64Buf,
    mut v: ::core::ffi::c_int,
    mut off: u32,
    mut store: ::core::ffi::c_int,
) {
    let mut disp: i32 = g_sse_mem_disp.wrapping_add(off) as i32;
    if store != 0 {
        emit_v_st_at(
            b,
            16 as ::core::ffi::c_int,
            v,
            g_sse_mem_ra,
            disp,
            g_sse_mem_plainacc,
        );
    } else {
        emit_v_ld_at(
            b,
            16 as ::core::ffi::c_int,
            v,
            g_sse_mem_ra,
            disp,
            g_sse_mem_plainacc,
        );
    };
}
unsafe fn emit_vex_mem_done(mut b: *mut A64Buf) {
    patch_guard_skip(g_vex_mem_skip, a64_label(b));
    g_vex_mem_skip = ::core::ptr::null_mut::<u32>();
}
unsafe fn emit_vex_ld128(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut m: *const X86Operand,
    mut size: ::core::ffi::c_int,
    mut vd: ::core::ffi::c_int,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if size == 16 as ::core::ffi::c_int
        && emit_plain_mem_fast(
            b,
            insn,
            m,
            16 as ::core::ffi::c_int,
            vd,
            0 as ::core::ffi::c_int,
            1 as ::core::ffi::c_int,
        ) != 0
    {
        return 1 as ::core::ffi::c_int;
    }
    let mut skip: *mut u32 = ::core::ptr::null_mut::<u32>();
    if emit_sse_mem_addr(b, insn, m, size, exit_sites, n_exits, &raw mut skip) == 0 {
        return 0 as ::core::ffi::c_int;
    }
    emit_sse_mem_ld(b, size, vd);
    patch_guard_skip(skip, a64_label(b));
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_vex_mov(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut L: ::core::ffi::c_int,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int
        || (*insn).vex as ::core::ffi::c_int & OCERZ_VEX_NDS as ::core::ffi::c_int != 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if (*d).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
        if xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        let mut vd: ::core::ffi::c_int = xmm_vreg((*d).reg as ::core::ffi::c_uint);
        if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
            if xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0 {
                return 0 as ::core::ffi::c_int;
            }
            if (*s).reg as ::core::ffi::c_int != (*d).reg as ::core::ffi::c_int {
                a64_v_mov(b, vd, xmm_vreg((*s).reg as ::core::ffi::c_uint));
                if L != 0 {
                    emit_ymmh_st(
                        b,
                        ymmh_src(
                            b,
                            (*s).reg as ::core::ffi::c_uint,
                            VX0 as ::core::ffi::c_int,
                        ),
                        (*d).reg as ::core::ffi::c_uint,
                    );
                }
            }
            if L == 0 {
                emit_ymmh_clear(b, (*d).reg as ::core::ffi::c_uint);
            }
            return 1 as ::core::ffi::c_int;
        }
        if (*s).kind as ::core::ffi::c_int != OCERZ_OPK_MEM as ::core::ffi::c_int {
            return 0 as ::core::ffi::c_int;
        }
        if L == 0 {
            if emit_vex_ld128(
                b,
                insn,
                s,
                16 as ::core::ffi::c_int,
                vd,
                exit_sites,
                n_exits,
            ) == 0
            {
                return 0 as ::core::ffi::c_int;
            }
            emit_ymmh_clear(b, (*d).reg as ::core::ffi::c_uint);
            return 1 as ::core::ffi::c_int;
        }
        if emit_vex_mem_addr(b, insn, s, exit_sites, n_exits) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        let mut vh: ::core::ffi::c_int =
            ymmh_dst((*d).reg as ::core::ffi::c_uint, VX1 as ::core::ffi::c_int);
        let mut dl: i32 = g_sse_mem_disp as i32;
        if (g_sse_mem_plainacc != 0 || vec_tso_relaxed() != 0)
            && dl % 16 as i32 == 0 as i32
            && dl >= -(1024 as i32)
            && dl <= 1008 as i32
        {
            a64_ldp_q_off(
                b,
                VX0 as ::core::ffi::c_int,
                vh,
                g_sse_mem_ra,
                dl as ::core::ffi::c_int,
            );
        } else {
            emit_vex_mem_acc(
                b,
                VX0 as ::core::ffi::c_int,
                0 as u32,
                0 as ::core::ffi::c_int,
            );
            emit_vex_mem_acc(b, vh, 16 as u32, 0 as ::core::ffi::c_int);
        }
        a64_v_mov(b, vd, VX0 as ::core::ffi::c_int);
        emit_vex_mem_done(b);
        emit_ymmh_st(b, vh, (*d).reg as ::core::ffi::c_uint);
        return 1 as ::core::ffi::c_int;
    }
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_MEM as ::core::ffi::c_int
        || (*s).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut vs: ::core::ffi::c_int = xmm_vreg((*s).reg as ::core::ffi::c_uint);
    if L == 0 {
        if emit_plain_mem_fast(
            b,
            insn,
            d,
            16 as ::core::ffi::c_int,
            vs,
            1 as ::core::ffi::c_int,
            1 as ::core::ffi::c_int,
        ) != 0
        {
            return 1 as ::core::ffi::c_int;
        }
        let mut skip: *mut u32 = ::core::ptr::null_mut::<u32>();
        if emit_sse_mem_addr(
            b,
            insn,
            d,
            16 as ::core::ffi::c_int,
            exit_sites,
            n_exits,
            &raw mut skip,
        ) == 0
        {
            return 0 as ::core::ffi::c_int;
        }
        emit_sse_mem_st(b, 16 as ::core::ffi::c_int, vs);
        patch_guard_skip(skip, a64_label(b));
        return 1 as ::core::ffi::c_int;
    }
    if emit_vex_mem_addr(b, insn, d, exit_sites, n_exits) == 0 {
        return 0 as ::core::ffi::c_int;
    }
    let mut vh_0: ::core::ffi::c_int = ymmh_src(
        b,
        (*s).reg as ::core::ffi::c_uint,
        VX1 as ::core::ffi::c_int,
    );
    emit_vex_mem_acc(b, vs, 0 as u32, 1 as ::core::ffi::c_int);
    emit_vex_mem_acc(b, vh_0, 16 as u32, 1 as ::core::ffi::c_int);
    emit_vex_mem_done(b);
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_vex_int(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut kind: ::core::ffi::c_int,
    mut esz: ::core::ffi::c_int,
    mut L: ::core::ffi::c_int,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*insn).vex as ::core::ffi::c_int & OCERZ_VEX_NDS as ::core::ffi::c_int == 0
        || (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int
        || (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    if xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
        || xmm_is_pinned((*insn).vvvv as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
        (xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0) as ::core::ffi::c_int
    } else {
        ((*s).kind as ::core::ffi::c_int != OCERZ_OPK_MEM as ::core::ffi::c_int)
            as ::core::ffi::c_int
    } != 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut vd: ::core::ffi::c_int = xmm_vreg((*d).reg as ::core::ffi::c_uint);
    let mut va: ::core::ffi::c_int = xmm_vreg((*insn).vvvv as ::core::ffi::c_uint);
    let mut vb: ::core::ffi::c_int = VX0 as ::core::ffi::c_int;
    if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int
        && (*s).reg as ::core::ffi::c_int == (*insn).vvvv as ::core::ffi::c_int
        && sse_int_self_zero(kind) != 0
    {
        a64_v_zero(b, vd);
        emit_ymmh_clear(b, (*d).reg as ::core::ffi::c_uint);
        return 1 as ::core::ffi::c_int;
    }
    let mut vbh: ::core::ffi::c_int = VX1 as ::core::ffi::c_int;
    if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
        vb = xmm_vreg((*s).reg as ::core::ffi::c_uint);
        if L != 0 {
            vbh = ymmh_src(
                b,
                (*s).reg as ::core::ffi::c_uint,
                VX1 as ::core::ffi::c_int,
            );
        }
    } else if L != 0 {
        if emit_vex_mem_addr(b, insn, s, exit_sites, n_exits) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        emit_vex_mem_acc(
            b,
            VX0 as ::core::ffi::c_int,
            0 as u32,
            0 as ::core::ffi::c_int,
        );
        emit_vex_mem_acc(
            b,
            VX1 as ::core::ffi::c_int,
            16 as u32,
            0 as ::core::ffi::c_int,
        );
        emit_vex_mem_done(b);
    } else if emit_vex_ld128(
        b,
        insn,
        s,
        16 as ::core::ffi::c_int,
        VX0 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    ) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if L != 0 {
        let mut vah: ::core::ffi::c_int = ymmh_src(
            b,
            (*insn).vvvv as ::core::ffi::c_uint,
            VX2 as ::core::ffi::c_int,
        );
        let mut vdh: ::core::ffi::c_int =
            ymmh_dst((*d).reg as ::core::ffi::c_uint, VX2 as ::core::ffi::c_int);
        emit_sse_int_op(b, kind, esz, vdh, vah, vbh);
        emit_sse_int_op(b, kind, esz, vd, va, vb);
        emit_ymmh_st(b, vdh, (*d).reg as ::core::ffi::c_uint);
        return 1 as ::core::ffi::c_int;
    }
    emit_sse_int_op(b, kind, esz, vd, va, vb);
    emit_ymmh_clear(b, (*d).reg as ::core::ffi::c_uint);
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_vex_pmovmskb(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut L: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if L == 0 {
        return emit_pmovmskb(b, insn);
    }
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_REG as ::core::ffi::c_int
        || (*d).high8 as ::core::ffi::c_int != 0
        || (*d).size as ::core::ffi::c_int != 4 as ::core::ffi::c_int
            && (*d).size as ::core::ffi::c_int != 8 as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    if (*s).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if g_n_raslit >= RASLIT_MAX as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut ds: ::core::ffi::c_int = pin_slot((*d).reg as ::core::ffi::c_uint);
    if ds < 0 as ::core::ffi::c_int
        || rsp_is_ptr() != 0 && (*d).reg as ::core::ffi::c_int == OCERZ_RSP as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    emit_mskb_bits(b, VX1 as ::core::ffi::c_int);
    emit_ymmh_ld(
        b,
        VX2 as ::core::ffi::c_int,
        (*s).reg as ::core::ffi::c_uint,
    );
    a64_v_sshr_16b(
        b,
        VX0 as ::core::ffi::c_int,
        xmm_vreg((*s).reg as ::core::ffi::c_uint),
        7 as ::core::ffi::c_int,
    );
    a64_v_sshr_16b(
        b,
        VX2 as ::core::ffi::c_int,
        VX2 as ::core::ffi::c_int,
        7 as ::core::ffi::c_int,
    );
    a64_v_and(
        b,
        VX0 as ::core::ffi::c_int,
        VX0 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
    );
    a64_v_and(
        b,
        VX2 as ::core::ffi::c_int,
        VX2 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
    );
    let mut i: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    while i < 3 as ::core::ffi::c_int {
        a64_v_addp_16b(
            b,
            VX0 as ::core::ffi::c_int,
            VX0 as ::core::ffi::c_int,
            VX0 as ::core::ffi::c_int,
        );
        a64_v_addp_16b(
            b,
            VX2 as ::core::ffi::c_int,
            VX2 as ::core::ffi::c_int,
            VX2 as ::core::ffi::c_int,
        );
        i += 1;
    }
    a64_umov_w_h(
        b,
        JT0 as ::core::ffi::c_int,
        VX0 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    a64_umov_w_h(
        b,
        JT1 as ::core::ffi::c_int,
        VX2 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    a64_orr_reg(
        b,
        0 as ::core::ffi::c_int,
        pin_hreg(ds),
        JT0 as ::core::ffi::c_int,
        JT1 as ::core::ffi::c_int,
        16 as ::core::ffi::c_int,
    );
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_vex_broadcast(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut esz: ::core::ffi::c_int,
    mut L: ::core::ffi::c_int,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int
        || (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if L == 0
        && (esz == 16 as ::core::ffi::c_int
            || (*insn).op as ::core::ffi::c_int == OCERZ_OP_VBROADCASTSD as ::core::ffi::c_int)
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut vd: ::core::ffi::c_int = xmm_vreg((*d).reg as ::core::ffi::c_uint);
    if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
        if esz == 16 as ::core::ffi::c_int || xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        let mut vs: ::core::ffi::c_int = xmm_vreg((*s).reg as ::core::ffi::c_uint);
        if esz == 1 as ::core::ffi::c_int {
            a64_v_dup_b(b, vd, vs, 0 as ::core::ffi::c_int);
        } else if esz == 2 as ::core::ffi::c_int {
            a64_v_dup_h(b, vd, vs, 0 as ::core::ffi::c_int);
        } else if esz == 4 as ::core::ffi::c_int {
            a64_v_dup_s(b, vd, vs, 0 as ::core::ffi::c_int);
        } else {
            a64_v_dup_d(b, vd, vs, 0 as ::core::ffi::c_int);
        }
    } else if (*s).kind as ::core::ffi::c_int != OCERZ_OPK_MEM as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    } else if esz == 16 as ::core::ffi::c_int {
        if emit_vex_ld128(
            b,
            insn,
            s,
            16 as ::core::ffi::c_int,
            vd,
            exit_sites,
            n_exits,
        ) == 0
        {
            return 0 as ::core::ffi::c_int;
        }
    } else {
        let mut skip: *mut u32 = ::core::ptr::null_mut::<u32>();
        if emit_sse_mem_addr(b, insn, s, esz, exit_sites, n_exits, &raw mut skip) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        emit_sse_mem_ld_gpr(b, esz, JT0 as ::core::ffi::c_int);
        patch_guard_skip(skip, a64_label(b));
        a64_v_dup_gpr(b, esz, vd, JT0 as ::core::ffi::c_int);
    }
    if L != 0 {
        emit_ymmh_st(b, vd, (*d).reg as ::core::ffi::c_uint);
    } else {
        emit_ymmh_clear(b, (*d).reg as ::core::ffi::c_uint);
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_vex_pmovx(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut sgn: ::core::ffi::c_int,
    mut from: ::core::ffi::c_int,
    mut L: ::core::ffi::c_int,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int
        || (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut vd: ::core::ffi::c_int = xmm_vreg((*d).reg as ::core::ffi::c_uint);
    let mut vs: ::core::ffi::c_int = VX1 as ::core::ffi::c_int;
    if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
        if xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        vs = xmm_vreg((*s).reg as ::core::ffi::c_uint);
    } else if (*s).kind as ::core::ffi::c_int != OCERZ_OPK_MEM as ::core::ffi::c_int
        || emit_vex_ld128(
            b,
            insn,
            s,
            if L != 0 {
                16 as ::core::ffi::c_int
            } else {
                8 as ::core::ffi::c_int
            },
            VX1 as ::core::ffi::c_int,
            exit_sites,
            n_exits,
        ) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if L != 0 {
        a64_v_xtl2(b, sgn, from, VX0 as ::core::ffi::c_int, vs);
        a64_v_xtl(b, sgn, from, vd, vs);
        emit_ymmh_st(
            b,
            VX0 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    } else {
        a64_v_xtl(b, sgn, from, vd, vs);
        emit_ymmh_clear(b, (*d).reg as ::core::ffi::c_uint);
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_vex_shift_imm(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut kind: ::core::ffi::c_int,
    mut esz: ::core::ffi::c_int,
    mut L: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*insn).vex as ::core::ffi::c_int & OCERZ_VEX_NDD as ::core::ffi::c_int == 0
        || (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut c: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(2 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || (*c).kind as ::core::ffi::c_int != OCERZ_OPK_IMM as ::core::ffi::c_int
        || (*s).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    if xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
        || xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut cnt: ::core::ffi::c_uint = ((*c).imm & 0xff as u64) as ::core::ffi::c_uint;
    if L != 0 {
        emit_ymmh_ld(
            b,
            VX1 as ::core::ffi::c_int,
            (*s).reg as ::core::ffi::c_uint,
        );
        emit_simd_shift_imm(
            b,
            kind,
            esz,
            VX1 as ::core::ffi::c_int,
            VX1 as ::core::ffi::c_int,
            cnt,
        );
    }
    emit_simd_shift_imm(
        b,
        kind,
        esz,
        xmm_vreg((*d).reg as ::core::ffi::c_uint),
        xmm_vreg((*s).reg as ::core::ffi::c_uint),
        cnt,
    );
    if L != 0 {
        emit_ymmh_st(
            b,
            VX1 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    } else {
        emit_ymmh_clear(b, (*d).reg as ::core::ffi::c_uint);
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn inexact_nan_env() -> ::core::ffi::c_int {
    static mut v: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
    if v < 0 as ::core::ffi::c_int {
        v = if !libc::getenv(c"OCERZ_INEXACT_NAN".as_ptr()).is_null() {
            1 as ::core::ffi::c_int
        } else {
            0 as ::core::ffi::c_int
        };
    }
    return v;
}
unsafe fn emit_vex_fp_op(
    mut b: *mut A64Buf,
    mut kind: ::core::ffi::c_int,
    mut dbl: ::core::ffi::c_int,
    mut vr: ::core::ffi::c_int,
    mut va: ::core::ffi::c_int,
    mut vb: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    match kind {
        0 => {
            a64_v_fadd(b, dbl, vr, va, vb);
            return 1 as ::core::ffi::c_int;
        }
        1 => {
            a64_v_fsub(b, dbl, vr, va, vb);
            return 1 as ::core::ffi::c_int;
        }
        2 => {
            a64_v_fmul(b, dbl, vr, va, vb);
            return 1 as ::core::ffi::c_int;
        }
        3 => {
            a64_v_fdiv(b, dbl, vr, va, vb);
            return 1 as ::core::ffi::c_int;
        }
        4 => {
            a64_v_fcmgt(b, dbl, vr, va, vb);
            a64_v_bsl(b, vr, va, vb);
            return 0 as ::core::ffi::c_int;
        }
        5 => {
            a64_v_fcmgt(b, dbl, vr, vb, va);
            a64_v_bsl(b, vr, va, vb);
            return 0 as ::core::ffi::c_int;
        }
        _ => {
            a64_v_fsqrt(b, dbl, vr, vb);
            return 1 as ::core::ffi::c_int;
        }
    };
}
unsafe fn emit_vex_fp_lane(
    mut b: *mut A64Buf,
    mut kind: ::core::ffi::c_int,
    mut dbl: ::core::ffi::c_int,
    mut vr: ::core::ffi::c_int,
    mut va: ::core::ffi::c_int,
    mut vb: ::core::ffi::c_int,
    mut t1: ::core::ffi::c_int,
) {
    if emit_vex_fp_op(b, kind, dbl, vr, va, vb) == 0 {
        return;
    }
    if kind == 6 as ::core::ffi::c_int {
        va = vb;
    }
    if inexact_nan_env() != 0 || g_fpb_fast != 0 || ocerz_afp() != 0 {
        return;
    }
    emit_nan_fix_packed2(b, dbl, vr, va, vb, t1, t1);
}
unsafe fn emit_vex_cvtdq2ps256(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int
        || (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
        (xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0) as ::core::ffi::c_int
    } else {
        ((*s).kind as ::core::ffi::c_int != OCERZ_OPK_MEM as ::core::ffi::c_int)
            as ::core::ffi::c_int
    } != 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut lo: ::core::ffi::c_int = 0;
    let mut hi: ::core::ffi::c_int = 0;
    if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int {
        if emit_vex_mem_addr(b, insn, s, exit_sites, n_exits) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        emit_vex_mem_acc(
            b,
            VX0 as ::core::ffi::c_int,
            0 as u32,
            0 as ::core::ffi::c_int,
        );
        emit_vex_mem_acc(
            b,
            VX2 as ::core::ffi::c_int,
            16 as u32,
            0 as ::core::ffi::c_int,
        );
        emit_vex_mem_done(b);
        lo = VX0 as ::core::ffi::c_int;
        hi = VX2 as ::core::ffi::c_int;
    } else {
        lo = xmm_vreg((*s).reg as ::core::ffi::c_uint);
        hi = ymmh_src(
            b,
            (*s).reg as ::core::ffi::c_uint,
            VX2 as ::core::ffi::c_int,
        );
    }
    let mut dh: ::core::ffi::c_int =
        ymmh_dst((*d).reg as ::core::ffi::c_uint, VX3 as ::core::ffi::c_int);
    a64_v_scvtf_4s(b, dh, hi);
    a64_v_scvtf_4s(b, xmm_vreg((*d).reg as ::core::ffi::c_uint), lo);
    emit_ymmh_st(b, dh, (*d).reg as ::core::ffi::c_uint);
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_vex_fp256(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut kind: ::core::ffi::c_int = 0;
    let mut dbl: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    match (*insn).op as ::core::ffi::c_int {
        204 => {
            kind = 0 as ::core::ffi::c_int;
        }
        205 => {
            kind = 0 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        208 => {
            kind = 1 as ::core::ffi::c_int;
        }
        209 => {
            kind = 1 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        212 => {
            kind = 2 as ::core::ffi::c_int;
        }
        213 => {
            kind = 2 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        216 => {
            kind = 3 as ::core::ffi::c_int;
        }
        217 => {
            kind = 3 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        224 => {
            kind = 4 as ::core::ffi::c_int;
        }
        225 => {
            kind = 4 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        220 => {
            kind = 5 as ::core::ffi::c_int;
        }
        221 => {
            kind = 5 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        228 => {
            kind = 6 as ::core::ffi::c_int;
        }
        229 => {
            kind = 6 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        _ => return 0 as ::core::ffi::c_int,
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut sq: ::core::ffi::c_int = (kind == 6 as ::core::ffi::c_int) as ::core::ffi::c_int;
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int
        || (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if sq == 0
        && ((*insn).vex as ::core::ffi::c_int & OCERZ_VEX_NDS as ::core::ffi::c_int == 0
            || xmm_is_pinned((*insn).vvvv as ::core::ffi::c_uint) == 0)
    {
        return 0 as ::core::ffi::c_int;
    }
    if if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
        (xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0) as ::core::ffi::c_int
    } else {
        ((*s).kind as ::core::ffi::c_int != OCERZ_OPK_MEM as ::core::ffi::c_int)
            as ::core::ffi::c_int
    } != 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut mem: ::core::ffi::c_int = ((*s).kind as ::core::ffi::c_int
        == OCERZ_OPK_MEM as ::core::ffi::c_int)
        as ::core::ffi::c_int;
    let mut lb: ::core::ffi::c_int = VX0 as ::core::ffi::c_int;
    let mut sh: ::core::ffi::c_int = VX2 as ::core::ffi::c_int;
    let mut ah: ::core::ffi::c_int = VX1 as ::core::ffi::c_int;
    if mem != 0 {
        if emit_vex_mem_addr(b, insn, s, exit_sites, n_exits) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        emit_vex_mem_acc(
            b,
            VX0 as ::core::ffi::c_int,
            0 as u32,
            0 as ::core::ffi::c_int,
        );
        emit_vex_mem_acc(
            b,
            VX2 as ::core::ffi::c_int,
            16 as u32,
            0 as ::core::ffi::c_int,
        );
        emit_vex_mem_done(b);
    } else {
        sh = ymmh_src(
            b,
            (*s).reg as ::core::ffi::c_uint,
            VX2 as ::core::ffi::c_int,
        );
        lb = xmm_vreg((*s).reg as ::core::ffi::c_uint);
    }
    if sq == 0 {
        ah = ymmh_src(
            b,
            (*insn).vvvv as ::core::ffi::c_uint,
            VX1 as ::core::ffi::c_int,
        );
    }
    let mut dh: ::core::ffi::c_int =
        ymmh_dst((*d).reg as ::core::ffi::c_uint, VX3 as ::core::ffi::c_int);
    if dh == ah || dh == sh {
        dh = VX3 as ::core::ffi::c_int;
    }
    let mut chk: ::core::ffi::c_int = (emit_vex_fp_op(b, kind, dbl, dh, ah, sh) != 0
        && inexact_nan_env() == 0
        && ocerz_afp() == 0) as ::core::ffi::c_int;
    emit_vex_fp_op(
        b,
        kind,
        dbl,
        VX1 as ::core::ffi::c_int,
        if sq != 0 {
            lb
        } else {
            xmm_vreg((*insn).vvvv as ::core::ffi::c_uint)
        },
        lb,
    );
    if chk == 0 {
        a64_v_mov(
            b,
            xmm_vreg((*d).reg as ::core::ffi::c_uint),
            VX1 as ::core::ffi::c_int,
        );
        emit_ymmh_st(b, dh, (*d).reg as ::core::ffi::c_uint);
        return 1 as ::core::ffi::c_int;
    }
    a64_v_fcmeq(
        b,
        dbl,
        VX0 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
    );
    a64_v_fcmeq(b, dbl, VX2 as ::core::ffi::c_int, dh, dh);
    a64_v_and(
        b,
        VX0 as ::core::ffi::c_int,
        VX0 as ::core::ffi::c_int,
        VX2 as ::core::ffi::c_int,
    );
    a64_v_uminv_4s(b, VX0 as ::core::ffi::c_int, VX0 as ::core::ffi::c_int);
    a64_fmov_x_from_v(
        b,
        0 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        VX0 as ::core::ffi::c_int,
    );
    let mut site: *mut u32 = a64_label(b);
    a64_cbz(
        b,
        0 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        0 as i32,
    );
    a64_v_mov(
        b,
        xmm_vreg((*d).reg as ::core::ffi::c_uint),
        VX1 as ::core::ffi::c_int,
    );
    emit_ymmh_st(b, dh, (*d).reg as ::core::ffi::c_uint);
    if oolslow_add(insn, &raw mut site, 1 as ::core::ffi::c_int, a64_label(b)) == 0 {
        let mut done: *mut u32 = a64_label(b);
        a64_b(b, 0 as i32);
        patch_any_branch(site, a64_label(b));
        emit_slowcall_keep_lanes(b, insn, exit_sites, n_exits);
        a64_patch_b(done, a64_label(b));
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_vex_fp128_alias(mut b: *mut A64Buf, mut insn: *const X86Insn) -> ::core::ffi::c_int {
    let mut kind: ::core::ffi::c_int = 0;
    let mut dbl: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    let mut packed: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    match (*insn).op as ::core::ffi::c_int {
        206 => {
            kind = 0 as ::core::ffi::c_int;
        }
        207 => {
            kind = 0 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        210 => {
            kind = 1 as ::core::ffi::c_int;
        }
        211 => {
            kind = 1 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        214 => {
            kind = 2 as ::core::ffi::c_int;
        }
        215 => {
            kind = 2 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        218 => {
            kind = 3 as ::core::ffi::c_int;
        }
        219 => {
            kind = 3 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        226 => {
            kind = 4 as ::core::ffi::c_int;
        }
        227 => {
            kind = 4 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        222 => {
            kind = 5 as ::core::ffi::c_int;
        }
        223 => {
            kind = 5 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        230 => {
            kind = 6 as ::core::ffi::c_int;
        }
        231 => {
            kind = 6 as ::core::ffi::c_int;
            dbl = 1 as ::core::ffi::c_int;
        }
        204 => {
            kind = 0 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        205 => {
            kind = 0 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
            dbl = packed;
        }
        208 => {
            kind = 1 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        209 => {
            kind = 1 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
            dbl = packed;
        }
        212 => {
            kind = 2 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        213 => {
            kind = 2 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
            dbl = packed;
        }
        216 => {
            kind = 3 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        217 => {
            kind = 3 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
            dbl = packed;
        }
        224 => {
            kind = 4 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        225 => {
            kind = 4 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
            dbl = packed;
        }
        220 => {
            kind = 5 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
        }
        221 => {
            kind = 5 as ::core::ffi::c_int;
            packed = 1 as ::core::ffi::c_int;
            dbl = packed;
        }
        _ => return 0 as ::core::ffi::c_int,
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int
        || (*insn).vex as ::core::ffi::c_int & OCERZ_VEX_NDS as ::core::ffi::c_int == 0
        || (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || (*s).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    if (*s).reg as ::core::ffi::c_int != (*d).reg as ::core::ffi::c_int
        || (*insn).vvvv as ::core::ffi::c_int == (*d).reg as ::core::ffi::c_int
        || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
        || xmm_is_pinned((*insn).vvvv as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut vd: ::core::ffi::c_int = xmm_vreg((*d).reg as ::core::ffi::c_uint);
    g_scalar_merge_next = 0 as ::core::ffi::c_int;
    if packed != 0 {
        emit_vex_fp_lane(
            b,
            kind,
            dbl,
            VX2 as ::core::ffi::c_int,
            xmm_vreg((*insn).vvvv as ::core::ffi::c_uint),
            xmm_vreg((*s).reg as ::core::ffi::c_uint),
            VX3 as ::core::ffi::c_int,
        );
        a64_v_mov(b, vd, VX2 as ::core::ffi::c_int);
    } else {
        let mut va: ::core::ffi::c_int = l0_src2(b, (*insn).vvvv as ::core::ffi::c_uint, dbl);
        let mut vb: ::core::ffi::c_int = l0_src2(b, (*d).reg as ::core::ffi::c_uint, dbl);
        let mut t: ::core::ffi::c_int = if l0_enabled() != 0 {
            l0_alloc2(b, (*d).reg as ::core::ffi::c_uint, dbl)
        } else {
            VX2 as ::core::ffi::c_int
        };
        if t < 0 as ::core::ffi::c_int {
            t = VX2 as ::core::ffi::c_int;
        }
        let mut fa: ::core::ffi::c_int = va;
        let mut fb: ::core::ffi::c_int = vb;
        if t == va || t == vb {
            let mut alias: ::core::ffi::c_int = if t == va { va } else { vb };
            if dbl != 0 {
                a64_fmov_d_d(b, VX3 as ::core::ffi::c_int, alias);
            } else {
                a64_fmov_s_s(b, VX3 as ::core::ffi::c_int, alias);
            }
            if t == va {
                fa = VX3 as ::core::ffi::c_int;
            }
            if t == vb {
                fb = VX3 as ::core::ffi::c_int;
            }
        }
        match kind {
            0 => {
                a64_fadd_s(b, dbl, t, va, vb);
            }
            1 => {
                a64_fsub_s(b, dbl, t, va, vb);
            }
            2 => {
                a64_fmul_s(b, dbl, t, va, vb);
            }
            3 => {
                a64_fdiv_s(b, dbl, t, va, vb);
            }
            4 => {
                a64_fcmp(b, dbl, va, vb);
                a64_fcsel(b, dbl, t, va, vb, A64_GT as ::core::ffi::c_int);
            }
            5 => {
                a64_fcmp(b, dbl, va, vb);
                a64_fcsel(b, dbl, t, va, vb, A64_MI as ::core::ffi::c_int);
            }
            _ => {
                a64_fsqrt_s(b, dbl, t, vb);
            }
        }
        if !(inexact_nan_env() != 0 || g_fpb_fast != 0 || ocerz_afp() != 0) {
            if kind < 4 as ::core::ffi::c_int {
                emit_nan_fix_scalar2(b, dbl, t, fa, fb);
            } else if kind == 6 as ::core::ffi::c_int {
                emit_nan_fix_scalar2(b, dbl, t, fb, fb);
            }
        }
        g_fcmp_self_idx = -(1 as ::core::ffi::c_int);
        a64_v_mov(b, vd, xmm_vreg((*insn).vvvv as ::core::ffi::c_uint));
        emit_xmm_st_lo(
            b,
            if dbl != 0 {
                8 as ::core::ffi::c_int
            } else {
                4 as ::core::ffi::c_int
            },
            t,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    emit_ymmh_clear(b, (*d).reg as ::core::ffi::c_uint);
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_vex_cmps_alias(mut b: *mut A64Buf, mut insn: *const X86Insn) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut dbl: ::core::ffi::c_int = ((*insn).op as ::core::ffi::c_int
        == OCERZ_OP_CMPSDX as ::core::ffi::c_int)
        as ::core::ffi::c_int;
    let mut esz: ::core::ffi::c_int = if dbl != 0 {
        8 as ::core::ffi::c_int
    } else {
        4 as ::core::ffi::c_int
    };
    let mut pred: ::core::ffi::c_uint = (*insn).ops[2 as ::core::ffi::c_int as usize].imm
        as ::core::ffi::c_uint
        & 7 as ::core::ffi::c_uint;
    let mut va: ::core::ffi::c_int = l0_src2(b, (*insn).vvvv as ::core::ffi::c_uint, dbl);
    let mut vb: ::core::ffi::c_int = l0_src2(b, (*d).reg as ::core::ffi::c_uint, dbl);
    emit_cmps_pred(b, dbl, pred, VX2 as ::core::ffi::c_int, va, vb);
    a64_v_mov(
        b,
        xmm_vreg((*d).reg as ::core::ffi::c_uint),
        xmm_vreg((*insn).vvvv as ::core::ffi::c_uint),
    );
    emit_xmm_st_lo(
        b,
        esz,
        VX2 as ::core::ffi::c_int,
        (*d).reg as ::core::ffi::c_uint,
    );
    l0_inval((*d).reg as ::core::ffi::c_uint);
    emit_ymmh_clear(b, (*d).reg as ::core::ffi::c_uint);
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_blend_mask(
    mut b: *mut A64Buf,
    mut op: ::core::ffi::c_uint,
    mut vd: ::core::ffi::c_int,
    mut vm: ::core::ffi::c_int,
    mut vz: ::core::ffi::c_int,
) {
    if op == OCERZ_OP_BLENDVPD as ::core::ffi::c_int as ::core::ffi::c_uint {
        a64_v_sshr_2d(b, vd, vm, 63 as ::core::ffi::c_int);
    } else if op == OCERZ_OP_BLENDVPS as ::core::ffi::c_int as ::core::ffi::c_uint {
        a64_v_sshr_4s(b, vd, vm, 31 as ::core::ffi::c_int);
    } else {
        a64_v_zero(b, vz);
        a64_v_cmgt(b, 0 as ::core::ffi::c_int, vd, vz, vm);
    };
}
unsafe fn emit_vex_blendv(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut L: ::core::ffi::c_int,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut m: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(2 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int
        || (*insn).vex as ::core::ffi::c_int & OCERZ_VEX_IS4 as ::core::ffi::c_int == 0
        || (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || (*m).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    if xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
        || xmm_is_pinned((*m).reg as ::core::ffi::c_uint) == 0
        || xmm_is_pinned((*insn).vvvv as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
        (xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0) as ::core::ffi::c_int
    } else {
        ((*s).kind as ::core::ffi::c_int != OCERZ_OPK_MEM as ::core::ffi::c_int)
            as ::core::ffi::c_int
    } != 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut vv: ::core::ffi::c_int = xmm_vreg((*insn).vvvv as ::core::ffi::c_uint);
    let mut vm: ::core::ffi::c_int = xmm_vreg((*m).reg as ::core::ffi::c_uint);
    let mut vd: ::core::ffi::c_int = xmm_vreg((*d).reg as ::core::ffi::c_uint);
    let mut vs: ::core::ffi::c_int = VX0 as ::core::ffi::c_int;
    if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
        vs = xmm_vreg((*s).reg as ::core::ffi::c_uint);
        if L != 0 {
            emit_ymmh_ld(
                b,
                VX1 as ::core::ffi::c_int,
                (*s).reg as ::core::ffi::c_uint,
            );
        }
    } else if L != 0 {
        if emit_vex_mem_addr(b, insn, s, exit_sites, n_exits) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        emit_vex_mem_acc(
            b,
            VX0 as ::core::ffi::c_int,
            0 as u32,
            0 as ::core::ffi::c_int,
        );
        emit_vex_mem_acc(
            b,
            VX1 as ::core::ffi::c_int,
            16 as u32,
            0 as ::core::ffi::c_int,
        );
        emit_vex_mem_done(b);
    } else if emit_vex_ld128(
        b,
        insn,
        s,
        16 as ::core::ffi::c_int,
        VX0 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    ) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    emit_blend_mask(
        b,
        (*insn).op as ::core::ffi::c_uint,
        VX2 as ::core::ffi::c_int,
        vm,
        VX3 as ::core::ffi::c_int,
    );
    a64_v_bsl(b, VX2 as ::core::ffi::c_int, vs, vv);
    if L != 0 {
        emit_ymmh_ld(
            b,
            VX3 as ::core::ffi::c_int,
            (*m).reg as ::core::ffi::c_uint,
        );
        emit_blend_mask(
            b,
            (*insn).op as ::core::ffi::c_uint,
            VX3 as ::core::ffi::c_int,
            VX3 as ::core::ffi::c_int,
            VX0 as ::core::ffi::c_int,
        );
        emit_ymmh_ld(
            b,
            VX0 as ::core::ffi::c_int,
            (*insn).vvvv as ::core::ffi::c_uint,
        );
        a64_v_bsl(
            b,
            VX3 as ::core::ffi::c_int,
            VX1 as ::core::ffi::c_int,
            VX0 as ::core::ffi::c_int,
        );
        a64_v_mov(b, vd, VX2 as ::core::ffi::c_int);
        emit_ymmh_st(
            b,
            VX3 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
        return 1 as ::core::ffi::c_int;
    }
    a64_v_mov(b, vd, VX2 as ::core::ffi::c_int);
    emit_ymmh_clear(b, (*d).reg as ::core::ffi::c_uint);
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_vex_cmps_fused(mut b: *mut A64Buf, mut insn: *const X86Insn) -> ::core::ffi::c_int {
    let mut dbl: ::core::ffi::c_int = ((*insn).op as ::core::ffi::c_int
        == OCERZ_OP_CMPSDX as ::core::ffi::c_int)
        as ::core::ffi::c_int;
    let mut va: ::core::ffi::c_int = l0_src2(
        b,
        ((*insn).vvvv as ::core::ffi::c_int & 15 as ::core::ffi::c_int) as ::core::ffi::c_uint,
        dbl,
    );
    let mut vb: ::core::ffi::c_int = l0_src2(
        b,
        (*insn).ops[1 as ::core::ffi::c_int as usize].reg as ::core::ffi::c_uint,
        dbl,
    );
    emit_cmps_pred(
        b,
        dbl,
        (*insn).ops[2 as ::core::ffi::c_int as usize].imm as ::core::ffi::c_uint
            & 7 as ::core::ffi::c_uint,
        VX2 as ::core::ffi::c_int,
        va,
        vb,
    );
    g_cmps_mask_idx = g_cur_insn_idx;
    l0_inval((*insn).ops[0 as ::core::ffi::c_int as usize].reg as ::core::ffi::c_uint);
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_vex_blendv_fused(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut c: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut dbl: ::core::ffi::c_int = ((*insn).op as ::core::ffi::c_int
        == OCERZ_OP_BLENDVPD as ::core::ffi::c_int)
        as ::core::ffi::c_int;
    let mut m: ::core::ffi::c_uint =
        (*c).ops[0 as ::core::ffi::c_int as usize].reg as ::core::ffi::c_uint;
    let mut cv: ::core::ffi::c_uint =
        ((*c).vvvv as ::core::ffi::c_int & 15 as ::core::ffi::c_int) as ::core::ffi::c_uint;
    let mut d: ::core::ffi::c_uint =
        (*insn).ops[0 as ::core::ffi::c_int as usize].reg as ::core::ffi::c_uint;
    let mut s1: ::core::ffi::c_uint =
        ((*insn).vvvv as ::core::ffi::c_int & 15 as ::core::ffi::c_int) as ::core::ffi::c_uint;
    let mut s2: ::core::ffi::c_uint =
        (*insn).ops[1 as ::core::ffi::c_int as usize].reg as ::core::ffi::c_uint;
    let mut have: ::core::ffi::c_int =
        (g_cmps_mask_idx == g_cur_insn_idx - 1 as ::core::ffi::c_int) as ::core::ffi::c_int;
    g_cmps_mask_idx = -(1 as ::core::ffi::c_int);
    if have == 0 {
        l0_flush_reg(b, s1);
        l0_flush_reg(b, s2);
        l0_flush_reg(b, d);
        l0_flush_reg(b, m);
        l0_inval(d);
        return emit_vex_blendv(b, insn, 0 as ::core::ffi::c_int, exit_sites, n_exits);
    }
    if m != d {
        if m != cv {
            a64_v_mov(b, xmm_vreg(m), xmm_vreg(cv));
        }
        if dbl != 0 {
            a64_ins_d_d(
                b,
                xmm_vreg(m),
                0 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                0 as ::core::ffi::c_int,
            );
        } else {
            a64_ins_s_s(
                b,
                xmm_vreg(m),
                0 as ::core::ffi::c_int,
                VX2 as ::core::ffi::c_int,
                0 as ::core::ffi::c_int,
            );
        }
        emit_ymmh_clear(b, m);
    }
    let mut vd_old: ::core::ffi::c_int = l0_src2(b, d, dbl);
    let mut t: ::core::ffi::c_int = l0_alloc2(b, d, dbl);
    if t < 0 as ::core::ffi::c_int {
        l0_flush_reg(b, s1);
        l0_flush_reg(b, s2);
        if m == d {
            if d != cv {
                a64_v_mov(b, xmm_vreg(d), xmm_vreg(cv));
            }
            if dbl != 0 {
                a64_ins_d_d(
                    b,
                    xmm_vreg(d),
                    0 as ::core::ffi::c_int,
                    VX2 as ::core::ffi::c_int,
                    0 as ::core::ffi::c_int,
                );
            } else {
                a64_ins_s_s(
                    b,
                    xmm_vreg(d),
                    0 as ::core::ffi::c_int,
                    VX2 as ::core::ffi::c_int,
                    0 as ::core::ffi::c_int,
                );
            }
        }
        return emit_vex_blendv(b, insn, 0 as ::core::ffi::c_int, exit_sites, n_exits);
    }
    let mut v1: ::core::ffi::c_int = if s1 == d { vd_old } else { l0_src2(b, s1, dbl) };
    let mut v2: ::core::ffi::c_int = if s2 == d { vd_old } else { l0_src2(b, s2, dbl) };
    if v2 == t {
        a64_v_bif(b, t, v1, VX2 as ::core::ffi::c_int);
    } else {
        if v1 != t {
            if dbl != 0 {
                a64_fmov_d_d(b, t, v1);
            } else {
                a64_fmov_s_s(b, t, v1);
            }
        }
        a64_v_bit(b, t, v2, VX2 as ::core::ffi::c_int);
    }
    if dbl != 0 {
        a64_v_sshr_2d(
            b,
            VX3 as ::core::ffi::c_int,
            xmm_vreg(cv),
            63 as ::core::ffi::c_int,
        );
    } else {
        a64_v_sshr_4s(
            b,
            VX3 as ::core::ffi::c_int,
            xmm_vreg(cv),
            31 as ::core::ffi::c_int,
        );
    }
    a64_v_bsl(b, VX3 as ::core::ffi::c_int, xmm_vreg(s2), xmm_vreg(s1));
    a64_v_mov(b, xmm_vreg(d), VX3 as ::core::ffi::c_int);
    g_l0_dirty = (g_l0_dirty as ::core::ffi::c_int
        | ((1 as ::core::ffi::c_uint) << d) as u16 as ::core::ffi::c_int) as u16;
    emit_ymmh_clear(b, d);
    return 1 as ::core::ffi::c_int;
}
unsafe fn bmi_src(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut o: *const X86Operand,
    mut size: ::core::ffi::c_int,
    mut tmp: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if (*o).kind as ::core::ffi::c_int == OCERZ_OPK_REG as ::core::ffi::c_int {
        if (*o).high8 as ::core::ffi::c_int != 0
            || (*o).size as ::core::ffi::c_int != size
            || pin_slot((*o).reg as ::core::ffi::c_uint) < 0 as ::core::ffi::c_int
            || rsp_is_ptr() != 0
                && (*o).reg as ::core::ffi::c_int == OCERZ_RSP as ::core::ffi::c_int
        {
            return -(1 as ::core::ffi::c_int);
        }
        return pin_hreg(pin_slot((*o).reg as ::core::ffi::c_uint));
    }
    if (*o).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int
        && emit_mem_load_any(b, insn, o, size, tmp) != 0
    {
        return tmp;
    }
    return -(1 as ::core::ffi::c_int);
}
unsafe fn emit_bmi(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut need: u64,
) -> ::core::ffi::c_int {
    if (*insn).mode32 as ::core::ffi::c_int != 0
        || (*insn).seg as ::core::ffi::c_int != OCERZ_SEG_NONE as ::core::ffi::c_int
        || ((*insn).nops as ::core::ffi::c_int) < 2 as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut size: ::core::ffi::c_int = (*d).size as ::core::ffi::c_int;
    let mut sf: ::core::ffi::c_int = (size == 8 as ::core::ffi::c_int) as ::core::ffi::c_int;
    if size != 4 as ::core::ffi::c_int && size != 8 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    if (*d).kind as ::core::ffi::c_int != OCERZ_OPK_REG as ::core::ffi::c_int
        || (*d).high8 as ::core::ffi::c_int != 0
        || pin_slot((*d).reg as ::core::ffi::c_uint) < 0 as ::core::ffi::c_int
        || rsp_is_ptr() != 0 && (*d).reg as ::core::ffi::c_int == OCERZ_RSP as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut rd: ::core::ffi::c_int = pin_hreg(pin_slot((*d).reg as ::core::ffi::c_uint));
    let mut bits: ::core::ffi::c_uint =
        (size as ::core::ffi::c_uint).wrapping_mul(8 as ::core::ffi::c_uint);
    match (*insn).op as ::core::ffi::c_int {
        551 => {
            if (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int
                || (*insn).ops[2 as ::core::ffi::c_int as usize].kind as ::core::ffi::c_int
                    != OCERZ_OPK_IMM as ::core::ffi::c_int
            {
                return 0 as ::core::ffi::c_int;
            }
            let mut rs: ::core::ffi::c_int = bmi_src(
                b,
                insn,
                (&raw const (*insn).ops as *const X86Operand)
                    .offset(1 as ::core::ffi::c_int as isize) as *const X86Operand,
                size,
                JT0 as ::core::ffi::c_int,
            );
            if rs < 0 as ::core::ffi::c_int {
                return 0 as ::core::ffi::c_int;
            }
            let mut c: ::core::ffi::c_uint = (*insn).ops[2 as ::core::ffi::c_int as usize].imm
                as ::core::ffi::c_uint
                & bits.wrapping_sub(1 as ::core::ffi::c_uint);
            if c == 0 as ::core::ffi::c_uint {
                a64_mov_reg(b, sf, rd, rs);
            } else {
                a64_extr(b, sf, rd, rs, rs, c as ::core::ffi::c_int);
            }
            return 1 as ::core::ffi::c_int;
        }
        553 | 554 | 552 => {
            if (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int {
                return 0 as ::core::ffi::c_int;
            }
            let mut rs_0: ::core::ffi::c_int = bmi_src(
                b,
                insn,
                (&raw const (*insn).ops as *const X86Operand)
                    .offset(1 as ::core::ffi::c_int as isize) as *const X86Operand,
                size,
                JT0 as ::core::ffi::c_int,
            );
            let mut rc: ::core::ffi::c_int = bmi_src(
                b,
                insn,
                (&raw const (*insn).ops as *const X86Operand)
                    .offset(2 as ::core::ffi::c_int as isize) as *const X86Operand,
                size,
                JT1 as ::core::ffi::c_int,
            );
            if rs_0 < 0 as ::core::ffi::c_int || rc < 0 as ::core::ffi::c_int {
                return 0 as ::core::ffi::c_int;
            }
            if (*insn).op as ::core::ffi::c_int == OCERZ_OP_SHLX as ::core::ffi::c_int {
                a64_lslv(b, sf, rd, rs_0, rc);
            } else if (*insn).op as ::core::ffi::c_int == OCERZ_OP_SHRX as ::core::ffi::c_int {
                a64_lsrv(b, sf, rd, rs_0, rc);
            } else {
                a64_asrv(b, sf, rd, rs_0, rc);
            }
            return 1 as ::core::ffi::c_int;
        }
        542 => {
            if (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int
                || need != 0 && g_defer == 0
            {
                return 0 as ::core::ffi::c_int;
            }
            let mut r1: ::core::ffi::c_int = bmi_src(
                b,
                insn,
                (&raw const (*insn).ops as *const X86Operand)
                    .offset(1 as ::core::ffi::c_int as isize) as *const X86Operand,
                size,
                JT0 as ::core::ffi::c_int,
            );
            let mut r2: ::core::ffi::c_int = bmi_src(
                b,
                insn,
                (&raw const (*insn).ops as *const X86Operand)
                    .offset(2 as ::core::ffi::c_int as isize) as *const X86Operand,
                size,
                JT1 as ::core::ffi::c_int,
            );
            if r1 < 0 as ::core::ffi::c_int || r2 < 0 as ::core::ffi::c_int {
                return 0 as ::core::ffi::c_int;
            }
            a64_bic_reg(b, sf, rd, r2, r1, 0 as ::core::ffi::c_int);
            if need != 0 {
                emit_defer_flags(
                    b,
                    ocerz_cc_pack(
                        OCERZ_CC_LOGIC as ::core::ffi::c_int as ::core::ffi::c_uint,
                        size,
                        0 as ::core::ffi::c_int,
                    ),
                    rd,
                    rd,
                );
            }
            return 1 as ::core::ffi::c_int;
        }
        543 | 544 | 545 => {
            if need != 0 {
                return 0 as ::core::ffi::c_int;
            }
            let mut rs_1: ::core::ffi::c_int = bmi_src(
                b,
                insn,
                (&raw const (*insn).ops as *const X86Operand)
                    .offset(1 as ::core::ffi::c_int as isize) as *const X86Operand,
                size,
                JT0 as ::core::ffi::c_int,
            );
            if rs_1 < 0 as ::core::ffi::c_int {
                return 0 as ::core::ffi::c_int;
            }
            if (*insn).op as ::core::ffi::c_int == OCERZ_OP_BLSI as ::core::ffi::c_int {
                a64_neg_reg(b, sf, JT1 as ::core::ffi::c_int, rs_1);
            } else {
                a64_sub_imm(b, sf, JT1 as ::core::ffi::c_int, rs_1, 1 as u32);
            }
            if (*insn).op as ::core::ffi::c_int == OCERZ_OP_BLSMSK as ::core::ffi::c_int {
                a64_eor_reg(
                    b,
                    sf,
                    rd,
                    rs_1,
                    JT1 as ::core::ffi::c_int,
                    0 as ::core::ffi::c_int,
                );
            } else {
                a64_and_reg(
                    b,
                    sf,
                    rd,
                    rs_1,
                    JT1 as ::core::ffi::c_int,
                    0 as ::core::ffi::c_int,
                );
            }
            return 1 as ::core::ffi::c_int;
        }
        546 => {
            if need != 0 || (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int {
                return 0 as ::core::ffi::c_int;
            }
            let mut rs_2: ::core::ffi::c_int = bmi_src(
                b,
                insn,
                (&raw const (*insn).ops as *const X86Operand)
                    .offset(1 as ::core::ffi::c_int as isize) as *const X86Operand,
                size,
                JT0 as ::core::ffi::c_int,
            );
            let mut rc_0: ::core::ffi::c_int = bmi_src(
                b,
                insn,
                (&raw const (*insn).ops as *const X86Operand)
                    .offset(2 as ::core::ffi::c_int as isize) as *const X86Operand,
                size,
                JT1 as ::core::ffi::c_int,
            );
            if rs_2 < 0 as ::core::ffi::c_int || rc_0 < 0 as ::core::ffi::c_int {
                return 0 as ::core::ffi::c_int;
            }
            a64_try_and_imm(
                b,
                1 as ::core::ffi::c_int,
                JT2 as ::core::ffi::c_int,
                rc_0,
                0xff as u64,
            );
            a64_mov_imm64(b, JT1 as ::core::ffi::c_int, !(0 as u64));
            a64_lslv(
                b,
                sf,
                JT1 as ::core::ffi::c_int,
                JT1 as ::core::ffi::c_int,
                JT2 as ::core::ffi::c_int,
            );
            a64_bic_reg(
                b,
                sf,
                JT1 as ::core::ffi::c_int,
                rs_2,
                JT1 as ::core::ffi::c_int,
                0 as ::core::ffi::c_int,
            );
            a64_subs_imm(
                b,
                1 as ::core::ffi::c_int,
                A64_ZR as ::core::ffi::c_int,
                JT2 as ::core::ffi::c_int,
                bits as u32,
            );
            a64_csel(
                b,
                sf,
                rd,
                rs_2,
                JT1 as ::core::ffi::c_int,
                A64_CS as ::core::ffi::c_int,
            );
            return 1 as ::core::ffi::c_int;
        }
        550 => {
            if (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int
                || pin_slot(OCERZ_RDX as ::core::ffi::c_int as ::core::ffi::c_uint)
                    < 0 as ::core::ffi::c_int
            {
                return 0 as ::core::ffi::c_int;
            }
            let mut lo: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
                .offset(1 as ::core::ffi::c_int as isize)
                as *const X86Operand;
            if (*lo).kind as ::core::ffi::c_int != OCERZ_OPK_REG as ::core::ffi::c_int
                || (*lo).high8 as ::core::ffi::c_int != 0
                || pin_slot((*lo).reg as ::core::ffi::c_uint) < 0 as ::core::ffi::c_int
                || rsp_is_ptr() != 0
                    && (*lo).reg as ::core::ffi::c_int == OCERZ_RSP as ::core::ffi::c_int
            {
                return 0 as ::core::ffi::c_int;
            }
            let mut rs_3: ::core::ffi::c_int = bmi_src(
                b,
                insn,
                (&raw const (*insn).ops as *const X86Operand)
                    .offset(2 as ::core::ffi::c_int as isize) as *const X86Operand,
                size,
                JT0 as ::core::ffi::c_int,
            );
            if rs_3 < 0 as ::core::ffi::c_int {
                return 0 as ::core::ffi::c_int;
            }
            let mut rdx: ::core::ffi::c_int = pin_hreg(pin_slot(
                OCERZ_RDX as ::core::ffi::c_int as ::core::ffi::c_uint,
            ));
            let mut rlo: ::core::ffi::c_int = pin_hreg(pin_slot((*lo).reg as ::core::ffi::c_uint));
            if sf != 0 {
                a64_umulh(b, JT1 as ::core::ffi::c_int, rdx, rs_3);
                a64_mul(
                    b,
                    1 as ::core::ffi::c_int,
                    JT2 as ::core::ffi::c_int,
                    rdx,
                    rs_3,
                );
                a64_mov_reg(b, 1 as ::core::ffi::c_int, rlo, JT2 as ::core::ffi::c_int);
                a64_mov_reg(b, 1 as ::core::ffi::c_int, rd, JT1 as ::core::ffi::c_int);
            } else {
                a64_mov_reg(b, 0 as ::core::ffi::c_int, JT1 as ::core::ffi::c_int, rdx);
                a64_mov_reg(b, 0 as ::core::ffi::c_int, JT2 as ::core::ffi::c_int, rs_3);
                a64_mul(
                    b,
                    1 as ::core::ffi::c_int,
                    JT2 as ::core::ffi::c_int,
                    JT1 as ::core::ffi::c_int,
                    JT2 as ::core::ffi::c_int,
                );
                a64_mov_reg(b, 0 as ::core::ffi::c_int, rlo, JT2 as ::core::ffi::c_int);
                a64_lsr_imm(
                    b,
                    1 as ::core::ffi::c_int,
                    rd,
                    JT2 as ::core::ffi::c_int,
                    32 as ::core::ffi::c_int,
                );
            }
            return 1 as ::core::ffi::c_int;
        }
        _ => return 0 as ::core::ffi::c_int,
    };
}
unsafe fn emit_vex_shufp(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut L: ::core::ffi::c_int,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut dbl: ::core::ffi::c_int = ((*insn).op as ::core::ffi::c_int
        == OCERZ_OP_SHUFPD as ::core::ffi::c_int)
        as ::core::ffi::c_int;
    if (*insn).nops as ::core::ffi::c_int != 3 as ::core::ffi::c_int
        || (*insn).vex as ::core::ffi::c_int & OCERZ_VEX_NDS as ::core::ffi::c_int == 0
        || (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || (*insn).ops[2 as ::core::ffi::c_int as usize].kind as ::core::ffi::c_int
            != OCERZ_OPK_IMM as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    if xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
        || xmm_is_pinned((*insn).vvvv as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
        (xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0) as ::core::ffi::c_int
    } else {
        ((*s).kind as ::core::ffi::c_int != OCERZ_OPK_MEM as ::core::ffi::c_int)
            as ::core::ffi::c_int
    } != 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut imm: ::core::ffi::c_uint =
        (*insn).ops[2 as ::core::ffi::c_int as usize].imm as ::core::ffi::c_uint;
    let mut vb: ::core::ffi::c_int = VX0 as ::core::ffi::c_int;
    if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
        vb = xmm_vreg((*s).reg as ::core::ffi::c_uint);
        if L != 0 {
            emit_ymmh_ld(
                b,
                VX3 as ::core::ffi::c_int,
                (*s).reg as ::core::ffi::c_uint,
            );
        }
    } else if L != 0 {
        if emit_vex_mem_addr(b, insn, s, exit_sites, n_exits) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        emit_vex_mem_acc(
            b,
            VX0 as ::core::ffi::c_int,
            0 as u32,
            0 as ::core::ffi::c_int,
        );
        emit_vex_mem_acc(
            b,
            VX3 as ::core::ffi::c_int,
            16 as u32,
            0 as ::core::ffi::c_int,
        );
        emit_vex_mem_done(b);
    } else if emit_vex_ld128(
        b,
        insn,
        s,
        16 as ::core::ffi::c_int,
        VX0 as ::core::ffi::c_int,
        exit_sites,
        n_exits,
    ) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if L != 0 {
        emit_ymmh_ld(
            b,
            VX1 as ::core::ffi::c_int,
            (*insn).vvvv as ::core::ffi::c_uint,
        );
        emit_shufp_lane(
            b,
            dbl,
            if dbl != 0 {
                imm >> 2 as ::core::ffi::c_int
            } else {
                imm
            },
            VX2 as ::core::ffi::c_int,
            VX1 as ::core::ffi::c_int,
            VX3 as ::core::ffi::c_int,
        );
        emit_ymmh_st(
            b,
            VX2 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
    }
    emit_shufp_lane(
        b,
        dbl,
        imm,
        VX2 as ::core::ffi::c_int,
        xmm_vreg((*insn).vvvv as ::core::ffi::c_uint),
        vb,
    );
    a64_v_mov(
        b,
        xmm_vreg((*d).reg as ::core::ffi::c_uint),
        VX2 as ::core::ffi::c_int,
    );
    if L == 0 {
        emit_ymmh_clear(b, (*d).reg as ::core::ffi::c_uint);
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_vex_insertps(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*insn).vex as ::core::ffi::c_int & OCERZ_VEX_NDS as ::core::ffi::c_int == 0
        || (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
        || xmm_is_pinned((*insn).vvvv as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if emit_insertps_to(
        b,
        insn,
        xmm_vreg((*insn).vvvv as ::core::ffi::c_uint),
        exit_sites,
        n_exits,
    ) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    a64_v_mov(
        b,
        xmm_vreg((*d).reg as ::core::ffi::c_uint),
        VX2 as ::core::ffi::c_int,
    );
    emit_ymmh_clear(b, (*d).reg as ::core::ffi::c_uint);
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_vex_movddup256(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int
        || (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut vs: ::core::ffi::c_int = VX0 as ::core::ffi::c_int;
    if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
        if xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        emit_ymmh_ld(
            b,
            VX1 as ::core::ffi::c_int,
            (*s).reg as ::core::ffi::c_uint,
        );
        vs = xmm_vreg((*s).reg as ::core::ffi::c_uint);
    } else if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_MEM as ::core::ffi::c_int {
        if emit_vex_mem_addr(b, insn, s, exit_sites, n_exits) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        emit_vex_mem_acc(
            b,
            VX0 as ::core::ffi::c_int,
            0 as u32,
            0 as ::core::ffi::c_int,
        );
        emit_vex_mem_acc(
            b,
            VX1 as ::core::ffi::c_int,
            16 as u32,
            0 as ::core::ffi::c_int,
        );
        emit_vex_mem_done(b);
    } else {
        return 0 as ::core::ffi::c_int;
    }
    a64_v_dup_d(
        b,
        VX1 as ::core::ffi::c_int,
        VX1 as ::core::ffi::c_int,
        0 as ::core::ffi::c_int,
    );
    a64_v_dup_d(
        b,
        xmm_vreg((*d).reg as ::core::ffi::c_uint),
        vs,
        0 as ::core::ffi::c_int,
    );
    emit_ymmh_st(
        b,
        VX1 as ::core::ffi::c_int,
        (*d).reg as ::core::ffi::c_uint,
    );
    return 1 as ::core::ffi::c_int;
}
unsafe fn emit_vex_fma(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut idx: ::core::ffi::c_int =
        (*insn).op as ::core::ffi::c_int - OCERZ_OP_VFMA_FIRST as ::core::ffi::c_int;
    let mut pd: ::core::ffi::c_int = idx & 1 as ::core::ffi::c_int;
    let mut kind: ::core::ffi::c_int = (idx >> 1 as ::core::ffi::c_int) % 10 as ::core::ffi::c_int;
    let mut order: ::core::ffi::c_int = (idx >> 1 as ::core::ffi::c_int) / 10 as ::core::ffi::c_int;
    if kind < 2 as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    let mut scalar: ::core::ffi::c_int = kind & 1 as ::core::ffi::c_int;
    let mut negmul: ::core::ffi::c_int = (kind >= 6 as ::core::ffi::c_int) as ::core::ffi::c_int;
    let mut negadd: ::core::ffi::c_int = (kind == 4 as ::core::ffi::c_int
        || kind == 5 as ::core::ffi::c_int
        || kind >= 8 as ::core::ffi::c_int)
        as ::core::ffi::c_int;
    let mut L: ::core::ffi::c_int = (scalar == 0
        && (*insn).vex as ::core::ffi::c_int & OCERZ_VEX_L as ::core::ffi::c_int
            != 0 as ::core::ffi::c_int) as ::core::ffi::c_int;
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut s: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(1 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    if (*insn).nops as ::core::ffi::c_int != 2 as ::core::ffi::c_int
        || (*d).kind as ::core::ffi::c_int != OCERZ_OPK_XMM as ::core::ffi::c_int
        || xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0
        || xmm_is_pinned((*insn).vvvv as ::core::ffi::c_uint) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if if (*s).kind as ::core::ffi::c_int == OCERZ_OPK_XMM as ::core::ffi::c_int {
        (xmm_is_pinned((*s).reg as ::core::ffi::c_uint) == 0) as ::core::ffi::c_int
    } else {
        ((*s).kind as ::core::ffi::c_int != OCERZ_OPK_MEM as ::core::ffi::c_int)
            as ::core::ffi::c_int
    } != 0
    {
        return 0 as ::core::ffi::c_int;
    }
    let mut mem: ::core::ffi::c_int = ((*s).kind as ::core::ffi::c_int
        == OCERZ_OPK_MEM as ::core::ffi::c_int)
        as ::core::ffi::c_int;
    if mem != 0 && L != 0 {
        if emit_vex_mem_addr(b, insn, s, exit_sites, n_exits) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        emit_vex_mem_acc(
            b,
            VX0 as ::core::ffi::c_int,
            0 as u32,
            0 as ::core::ffi::c_int,
        );
        emit_vex_mem_acc(
            b,
            VX3 as ::core::ffi::c_int,
            16 as u32,
            0 as ::core::ffi::c_int,
        );
        emit_vex_mem_done(b);
    } else if mem != 0
        && emit_vex_ld128(
            b,
            insn,
            s,
            if scalar != 0 {
                if pd != 0 {
                    8 as ::core::ffi::c_int
                } else {
                    4 as ::core::ffi::c_int
                }
            } else {
                16 as ::core::ffi::c_int
            },
            VX0 as ::core::ffi::c_int,
            exit_sites,
            n_exits,
        ) == 0
    {
        return 0 as ::core::ffi::c_int;
    }
    if scalar != 0 {
        let mut s1: ::core::ffi::c_int = l0_src2(b, (*d).reg as ::core::ffi::c_uint, pd);
        let mut s2: ::core::ffi::c_int = l0_src2(b, (*insn).vvvv as ::core::ffi::c_uint, pd);
        let mut s3: ::core::ffi::c_int = if mem != 0 {
            VX0 as ::core::ffi::c_int
        } else {
            l0_src2(b, (*s).reg as ::core::ffi::c_uint, pd)
        };
        let mut sa: ::core::ffi::c_int = if order == 0 as ::core::ffi::c_int {
            s1
        } else {
            s2
        };
        let mut sm: ::core::ffi::c_int = if order == 1 as ::core::ffi::c_int {
            s1
        } else {
            s3
        };
        let mut sc: ::core::ffi::c_int = if order == 0 as ::core::ffi::c_int {
            s2
        } else if order == 1 as ::core::ffi::c_int {
            s3
        } else {
            s1
        };
        l0_flush_reg(b, (*d).reg as ::core::ffi::c_uint);
        let mut t: ::core::ffi::c_int = if l0_enabled() != 0 {
            l0_alloc2(b, (*d).reg as ::core::ffi::c_uint, pd)
        } else {
            VX1 as ::core::ffi::c_int
        };
        if t < 0 as ::core::ffi::c_int {
            t = VX1 as ::core::ffi::c_int;
        }
        a64_fmadd_s(b, pd, negmul, negadd, t, sa, sm, sc);
        if inexact_nan_env() != 0 || g_fpb_fast != 0 {
            emit_xmm_st_lo(
                b,
                if pd != 0 {
                    8 as ::core::ffi::c_int
                } else {
                    4 as ::core::ffi::c_int
                },
                t,
                (*d).reg as ::core::ffi::c_uint,
            );
            emit_ymmh_clear(b, (*d).reg as ::core::ffi::c_uint);
            return 1 as ::core::ffi::c_int;
        }
        a64_fcmp(b, pd, t, t);
        let mut ssite: *mut u32 = a64_label(b);
        a64_bcond(b, A64_VS as ::core::ffi::c_int, 0 as i32);
        let mut dirty_before: u16 = g_l0_dirty;
        emit_xmm_st_lo(
            b,
            if pd != 0 {
                8 as ::core::ffi::c_int
            } else {
                4 as ::core::ffi::c_int
            },
            t,
            (*d).reg as ::core::ffi::c_uint,
        );
        emit_ymmh_clear(b, (*d).reg as ::core::ffi::c_uint);
        let mut dirty_after: u16 = g_l0_dirty;
        g_l0_dirty = dirty_before;
        if oolslow_add(insn, &raw mut ssite, 1 as ::core::ffi::c_int, a64_label(b)) == 0 {
            let mut done: *mut u32 = a64_label(b);
            a64_b(b, 0 as i32);
            patch_any_branch(ssite, a64_label(b));
            emit_slowcall_keep_lanes(b, insn, exit_sites, n_exits);
            a64_patch_b(done, a64_label(b));
        }
        g_l0_dirty = dirty_after;
        return 1 as ::core::ffi::c_int;
    }
    let mut o1: ::core::ffi::c_int = xmm_vreg((*d).reg as ::core::ffi::c_uint);
    let mut o2: ::core::ffi::c_int = xmm_vreg((*insn).vvvv as ::core::ffi::c_uint);
    let mut o3: ::core::ffi::c_int = if mem != 0 {
        VX0 as ::core::ffi::c_int
    } else {
        xmm_vreg((*s).reg as ::core::ffi::c_uint)
    };
    let mut a: ::core::ffi::c_int = if order == 0 as ::core::ffi::c_int {
        o1
    } else {
        o2
    };
    let mut m: ::core::ffi::c_int = if order == 1 as ::core::ffi::c_int {
        o1
    } else {
        o3
    };
    let mut c: ::core::ffi::c_int = if order == 0 as ::core::ffi::c_int {
        o2
    } else if order == 1 as ::core::ffi::c_int {
        o3
    } else {
        o1
    };
    let mut ch: ::core::ffi::c_int = VX3 as ::core::ffi::c_int;
    let mut r: ::core::ffi::c_int = VX1 as ::core::ffi::c_int;
    let mut t_0: ::core::ffi::c_int = VX2 as ::core::ffi::c_int;
    if L != 0 {
        if mem == 0 {
            emit_ymmh_ld(
                b,
                VX3 as ::core::ffi::c_int,
                (*s).reg as ::core::ffi::c_uint,
            );
        }
        emit_ymmh_ld(
            b,
            VX1 as ::core::ffi::c_int,
            (*d).reg as ::core::ffi::c_uint,
        );
        emit_ymmh_ld(
            b,
            VX2 as ::core::ffi::c_int,
            (*insn).vvvv as ::core::ffi::c_uint,
        );
        let mut ah: ::core::ffi::c_int = if order == 0 as ::core::ffi::c_int {
            VX1 as ::core::ffi::c_int
        } else {
            VX2 as ::core::ffi::c_int
        };
        let mut mh: ::core::ffi::c_int = if order == 1 as ::core::ffi::c_int {
            VX1 as ::core::ffi::c_int
        } else {
            VX3 as ::core::ffi::c_int
        };
        ch = if order == 0 as ::core::ffi::c_int {
            VX2 as ::core::ffi::c_int
        } else if order == 1 as ::core::ffi::c_int {
            VX3 as ::core::ffi::c_int
        } else {
            VX1 as ::core::ffi::c_int
        };
        if negadd != 0 {
            a64_v_fneg(b, pd, ch, ch);
        }
        if negmul != 0 {
            a64_v_fmls(b, pd, ch, ah, mh);
        } else {
            a64_v_fmla(b, pd, ch, ah, mh);
        }
        r = if ch == VX1 as ::core::ffi::c_int {
            VX2 as ::core::ffi::c_int
        } else {
            VX1 as ::core::ffi::c_int
        };
        t_0 = if ch == VX3 as ::core::ffi::c_int {
            VX2 as ::core::ffi::c_int
        } else {
            VX3 as ::core::ffi::c_int
        };
    }
    if negadd != 0 {
        a64_v_fneg(b, pd, r, c);
    } else {
        a64_v_mov(b, r, c);
    }
    if negmul != 0 {
        a64_v_fmls(b, pd, r, a, m);
    } else {
        a64_v_fmla(b, pd, r, a, m);
    }
    if L == 0 && (inexact_nan_env() != 0 || g_fpb_fast != 0) {
        a64_v_mov(b, o1, r);
        emit_ymmh_clear(b, (*d).reg as ::core::ffi::c_uint);
        return 1 as ::core::ffi::c_int;
    }
    let mut site: *mut u32 = ::core::ptr::null_mut::<u32>();
    a64_v_fcmeq(b, pd, t_0, r, r);
    if L != 0 {
        a64_v_fcmeq(b, pd, VX0 as ::core::ffi::c_int, ch, ch);
        a64_v_and(b, t_0, t_0, VX0 as ::core::ffi::c_int);
    }
    a64_v_uminv_4s(b, t_0, t_0);
    a64_fmov_x_from_v(b, 0 as ::core::ffi::c_int, JT0 as ::core::ffi::c_int, t_0);
    site = a64_label(b);
    a64_cbz(
        b,
        0 as ::core::ffi::c_int,
        JT0 as ::core::ffi::c_int,
        0 as i32,
    );
    a64_v_mov(b, o1, r);
    if L != 0 {
        emit_ymmh_st(b, ch, (*d).reg as ::core::ffi::c_uint);
    } else {
        emit_ymmh_clear(b, (*d).reg as ::core::ffi::c_uint);
    }
    if oolslow_add(insn, &raw mut site, 1 as ::core::ffi::c_int, a64_label(b)) == 0 {
        let mut done_0: *mut u32 = a64_label(b);
        a64_b(b, 0 as i32);
        patch_any_branch(site, a64_label(b));
        emit_slowcall_keep_lanes(b, insn, exit_sites, n_exits);
        a64_patch_b(done_0, a64_label(b));
    }
    return 1 as ::core::ffi::c_int;
}
unsafe fn vex_sse128_ok(mut insn: *const X86Insn, mut L: ::core::ffi::c_int) -> ::core::ffi::c_int {
    match (*insn).op as ::core::ffi::c_int {
        189 | 190 | 206 | 207 | 210 | 211 | 214 | 215 | 218 | 219 | 226 | 227 | 222 | 223 | 230
        | 231 | 391 | 392 | 256 | 257 | 254 | 255 | 271 | 270 | 267 | 266 | 273 | 272 => {
            return 1 as ::core::ffi::c_int;
        }
        252 | 253 => {
            return ((*insn).nops as ::core::ffi::c_int >= 3 as ::core::ffi::c_int
                && (*insn).ops[2 as ::core::ffi::c_int as usize].kind as ::core::ffi::c_int
                    == OCERZ_OPK_IMM as ::core::ffi::c_int
                && ((*insn).ops[2 as ::core::ffi::c_int as usize].imm & 0x1f as u64) < 8 as u64)
                as ::core::ffi::c_int;
        }
        193 | 194 | 204 | 205 | 208 | 209 | 212 | 213 | 216 | 217 | 224 | 225 | 220 | 221 | 228
        | 229 | 276 | 191 | 192 | 334 | 337 | 202 | 370 | 371 | 372 | 373 | 366 | 367 | 368
        | 369 | 384 | 385 | 387 | 378 | 379 | 381 | 389 | 390 | 326 | 327 | 328 | 329 | 330
        | 331 | 332 | 333 | 354 | 355 | 352 | 353 | 195 | 196 => {
            return (L == 0) as ::core::ffi::c_int;
        }
        _ => return 0 as ::core::ffi::c_int,
    };
}
unsafe fn emit_vex_sse128(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut L: ::core::ffi::c_int,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if vex_sse128_ok(insn, L) == 0 {
        return 0 as ::core::ffi::c_int;
    }
    let mut d: *const X86Operand = (&raw const (*insn).ops as *const X86Operand)
        .offset(0 as ::core::ffi::c_int as isize)
        as *const X86Operand;
    let mut wx: ::core::ffi::c_int = ((*d).kind as ::core::ffi::c_int
        == OCERZ_OPK_XMM as ::core::ffi::c_int
        && (*insn).op as ::core::ffi::c_int != OCERZ_OP_UCOMISS as ::core::ffi::c_int
        && (*insn).op as ::core::ffi::c_int != OCERZ_OP_UCOMISD as ::core::ffi::c_int
        && (*insn).op as ::core::ffi::c_int != OCERZ_OP_COMISS as ::core::ffi::c_int
        && (*insn).op as ::core::ffi::c_int != OCERZ_OP_COMISD as ::core::ffi::c_int)
        as ::core::ffi::c_int;
    if wx != 0 && xmm_is_pinned((*d).reg as ::core::ffi::c_uint) == 0 {
        return 0 as ::core::ffi::c_int;
    }
    if (*insn).vex as ::core::ffi::c_int & OCERZ_VEX_NDS as ::core::ffi::c_int != 0 {
        if wx == 0 || xmm_is_pinned((*insn).vvvv as ::core::ffi::c_uint) == 0 {
            return 0 as ::core::ffi::c_int;
        }
        if (*insn).vvvv as ::core::ffi::c_int != (*d).reg as ::core::ffi::c_int {
            let mut k: ::core::ffi::c_int = 1 as ::core::ffi::c_int;
            while k < (*insn).nops as ::core::ffi::c_int {
                if (*insn).ops[k as usize].kind as ::core::ffi::c_int
                    == OCERZ_OPK_XMM as ::core::ffi::c_int
                    && (*insn).ops[k as usize].reg as ::core::ffi::c_int
                        == (*d).reg as ::core::ffi::c_int
                {
                    return if (*insn).op as ::core::ffi::c_int
                        == OCERZ_OP_CMPSS as ::core::ffi::c_int
                        || (*insn).op as ::core::ffi::c_int == OCERZ_OP_CMPSDX as ::core::ffi::c_int
                    {
                        emit_vex_cmps_alias(b, insn)
                    } else {
                        emit_vex_fp128_alias(b, insn)
                    };
                }
                k += 1;
            }
            if vex_lane_aware(insn) != 0 {
                l0_inval((*d).reg as ::core::ffi::c_uint);
            }
            a64_v_mov(
                b,
                xmm_vreg((*d).reg as ::core::ffi::c_uint),
                xmm_vreg((*insn).vvvv as ::core::ffi::c_uint),
            );
            if vex_lane_aware(insn) != 0 {
                l0_share(
                    (*d).reg as ::core::ffi::c_uint,
                    (*insn).vvvv as ::core::ffi::c_uint,
                );
            }
        }
    }
    if emit_sse(b, insn, exit_sites, n_exits) == 0 {
        return 0 as ::core::ffi::c_int;
    }
    if wx != 0 {
        emit_ymmh_clear(b, (*d).reg as ::core::ffi::c_uint);
    }
    return 1 as ::core::ffi::c_int;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_vex(
    mut b: *mut A64Buf,
    mut insn: *const X86Insn,
    mut exit_sites: *mut *mut u32,
    mut n_exits: *mut ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    if vex_inline_enabled() == 0
        || sse_enabled() == 0
        || (*insn).mode32 as ::core::ffi::c_int != 0
        || (*insn).seg as ::core::ffi::c_int != OCERZ_SEG_NONE as ::core::ffi::c_int
    {
        return 0 as ::core::ffi::c_int;
    }
    if !g_cur_insns.is_null()
        && insn >= g_cur_insns
        && insn < g_cur_insns.offset(g_cur_insns_n as isize)
    {
        if ((*insn).op as ::core::ffi::c_int == OCERZ_OP_CMPSS as ::core::ffi::c_int
            || (*insn).op as ::core::ffi::c_int == OCERZ_OP_CMPSDX as ::core::ffi::c_int)
            && insn.offset(1 as ::core::ffi::c_int as isize)
                < g_cur_insns.offset(g_cur_insns_n as isize)
            && vex_cmps_blendv_pair(insn, insn.offset(1 as ::core::ffi::c_int as isize)) != 0
        {
            return emit_vex_cmps_fused(b, insn);
        }
        if ((*insn).op as ::core::ffi::c_int == OCERZ_OP_BLENDVPD as ::core::ffi::c_int
            || (*insn).op as ::core::ffi::c_int == OCERZ_OP_BLENDVPS as ::core::ffi::c_int)
            && insn > g_cur_insns
            && vex_cmps_blendv_pair(insn.offset(-(1 as ::core::ffi::c_int as isize)), insn) != 0
        {
            return emit_vex_blendv_fused(
                b,
                insn,
                insn.offset(-(1 as ::core::ffi::c_int as isize)),
                exit_sites,
                n_exits,
            );
        }
    }
    let mut L: ::core::ffi::c_int = ((*insn).vex as ::core::ffi::c_int
        & OCERZ_VEX_L as ::core::ffi::c_int
        != 0 as ::core::ffi::c_int) as ::core::ffi::c_int;
    let mut esz: ::core::ffi::c_int = 0;
    if (*insn).op as ::core::ffi::c_int >= OCERZ_OP_VFMA_FIRST as ::core::ffi::c_int
        && (*insn).op as ::core::ffi::c_int <= OCERZ_OP_VFMA_LAST as ::core::ffi::c_int
    {
        return emit_vex_fma(b, insn, exit_sites, n_exits);
    }
    let mut kind: ::core::ffi::c_int =
        sse_int_kind((*insn).op as ::core::ffi::c_uint, &raw mut esz);
    if kind >= SIK_MADDWD as ::core::ffi::c_int {
        return 0 as ::core::ffi::c_int;
    }
    if kind != 0 {
        return emit_vex_int(b, insn, kind, esz, L, exit_sites, n_exits);
    }
    match (*insn).op as ::core::ffi::c_int {
        204 | 205 | 208 | 209 | 212 | 213 | 216 | 217 | 224 | 225 | 220 | 221 | 228 | 229 => {
            if L != 0 {
                return emit_vex_fp256(b, insn, exit_sites, n_exits);
            }
            return emit_vex_sse128(b, insn, L, exit_sites, n_exits);
        }
        185 | 186 | 187 | 188 => return emit_vex_mov(b, insn, L, exit_sites, n_exits),
        199 => return emit_vex_pmovmskb(b, insn, L),
        452 => {
            return emit_vex_broadcast(b, insn, 1 as ::core::ffi::c_int, L, exit_sites, n_exits);
        }
        453 => {
            return emit_vex_broadcast(b, insn, 2 as ::core::ffi::c_int, L, exit_sites, n_exits);
        }
        454 | 432 => {
            return emit_vex_broadcast(b, insn, 4 as ::core::ffi::c_int, L, exit_sites, n_exits);
        }
        455 | 433 => {
            return emit_vex_broadcast(b, insn, 8 as ::core::ffi::c_int, L, exit_sites, n_exits);
        }
        456 | 434 => {
            return emit_vex_broadcast(b, insn, 16 as ::core::ffi::c_int, L, exit_sites, n_exits);
        }
        276 => {
            if L != 0 {
                return emit_vex_cvtdq2ps256(b, insn, exit_sites, n_exits);
            }
            return emit_vex_sse128(b, insn, L, exit_sites, n_exits);
        }
        444 => {
            static mut noflag: ::core::ffi::c_int = -(1 as ::core::ffi::c_int);
            if noflag < 0 as ::core::ffi::c_int {
                noflag = if !libc::getenv(c"OCERZ_NO_YMMH_FLAG".as_ptr()).is_null() {
                    1 as ::core::ffi::c_int
                } else {
                    0 as ::core::ffi::c_int
                };
            }
            let mut skip: *mut u32 = ::core::ptr::null_mut::<u32>();
            if noflag == 0 && g_blk_ymm_write == 0 {
                a64_ldr(
                    b,
                    4 as ::core::ffi::c_int,
                    JT0 as ::core::ffi::c_int,
                    20 as ::core::ffi::c_int,
                    YMMH_ALL_ZERO_OFF,
                );
                skip = a64_label(b);
                a64_cbnz(
                    b,
                    0 as ::core::ffi::c_int,
                    JT0 as ::core::ffi::c_int,
                    0 as i32,
                );
            }
            let mut vz: ::core::ffi::c_int = if g_zero_vreg >= 0 as ::core::ffi::c_int {
                g_zero_vreg
            } else {
                VX0 as ::core::ffi::c_int
            };
            if vz == VX0 as ::core::ffi::c_int {
                a64_v_zero(b, VX0 as ::core::ffi::c_int);
            }
            let mut r: ::core::ffi::c_uint = 0 as ::core::ffi::c_uint;
            while r < 16 as ::core::ffi::c_uint {
                if g_yc[r as usize] as ::core::ffi::c_int >= 0 as ::core::ffi::c_int {
                    a64_v_zero(b, g_yc[r as usize] as ::core::ffi::c_int);
                    g_yc_dirty = (g_yc_dirty as ::core::ffi::c_int
                        | ((1 as ::core::ffi::c_uint) << r) as u16 as ::core::ffi::c_int)
                        as u16;
                } else {
                    a64_str_v(
                        b,
                        16 as ::core::ffi::c_int,
                        vz,
                        20 as ::core::ffi::c_int,
                        YMMH_OFF.wrapping_add((r as u32).wrapping_mul(16 as u32)),
                    );
                }
                r = r.wrapping_add(1);
            }
            if !skip.is_null() {
                a64_mov_imm64(b, JT0 as ::core::ffi::c_int, 1 as u64);
                a64_str(
                    b,
                    4 as ::core::ffi::c_int,
                    JT0 as ::core::ffi::c_int,
                    20 as ::core::ffi::c_int,
                    YMMH_ALL_ZERO_OFF,
                );
                a64_patch_cbz(skip, a64_label(b));
            }
            g_ymmh_zero = 0xffff as u16;
            return 1 as ::core::ffi::c_int;
        }
        383 => {
            return emit_vex_pmovx(
                b,
                insn,
                1 as ::core::ffi::c_int,
                1 as ::core::ffi::c_int,
                L,
                exit_sites,
                n_exits,
            );
        }
        386 => {
            return emit_vex_pmovx(
                b,
                insn,
                1 as ::core::ffi::c_int,
                2 as ::core::ffi::c_int,
                L,
                exit_sites,
                n_exits,
            );
        }
        388 => {
            return emit_vex_pmovx(
                b,
                insn,
                1 as ::core::ffi::c_int,
                4 as ::core::ffi::c_int,
                L,
                exit_sites,
                n_exits,
            );
        }
        377 => {
            return emit_vex_pmovx(
                b,
                insn,
                0 as ::core::ffi::c_int,
                1 as ::core::ffi::c_int,
                L,
                exit_sites,
                n_exits,
            );
        }
        380 => {
            return emit_vex_pmovx(
                b,
                insn,
                0 as ::core::ffi::c_int,
                2 as ::core::ffi::c_int,
                L,
                exit_sites,
                n_exits,
            );
        }
        382 => {
            return emit_vex_pmovx(
                b,
                insn,
                0 as ::core::ffi::c_int,
                4 as ::core::ffi::c_int,
                L,
                exit_sites,
                n_exits,
            );
        }
        356 => {
            return emit_vex_shift_imm(
                b,
                insn,
                0 as ::core::ffi::c_int,
                1 as ::core::ffi::c_int,
                L,
            );
        }
        357 => {
            return emit_vex_shift_imm(
                b,
                insn,
                0 as ::core::ffi::c_int,
                2 as ::core::ffi::c_int,
                L,
            );
        }
        358 => {
            return emit_vex_shift_imm(
                b,
                insn,
                0 as ::core::ffi::c_int,
                3 as ::core::ffi::c_int,
                L,
            );
        }
        359 => {
            return emit_vex_shift_imm(
                b,
                insn,
                1 as ::core::ffi::c_int,
                1 as ::core::ffi::c_int,
                L,
            );
        }
        360 => {
            return emit_vex_shift_imm(
                b,
                insn,
                1 as ::core::ffi::c_int,
                2 as ::core::ffi::c_int,
                L,
            );
        }
        361 => {
            return emit_vex_shift_imm(
                b,
                insn,
                1 as ::core::ffi::c_int,
                3 as ::core::ffi::c_int,
                L,
            );
        }
        362 => {
            return emit_vex_shift_imm(
                b,
                insn,
                2 as ::core::ffi::c_int,
                1 as ::core::ffi::c_int,
                L,
            );
        }
        363 => {
            return emit_vex_shift_imm(
                b,
                insn,
                2 as ::core::ffi::c_int,
                2 as ::core::ffi::c_int,
                L,
            );
        }
        350 | 351 => return emit_vex_shufp(b, insn, L, exit_sites, n_exits),
        397 | 396 | 398 => return emit_vex_blendv(b, insn, L, exit_sites, n_exits),
        551 | 553 | 554 | 552 | 542 | 543 | 544 | 545 | 546 | 550 => {
            return emit_bmi(b, insn, g_cur_need);
        }
        375 => {
            return if L != 0 {
                0 as ::core::ffi::c_int
            } else {
                emit_vex_insertps(b, insn, exit_sites, n_exits)
            };
        }
        202 => {
            if L != 0 {
                return emit_vex_movddup256(b, insn, exit_sites, n_exits);
            }
            return emit_vex_sse128(b, insn, L, exit_sites, n_exits);
        }
        _ => return emit_vex_sse128(b, insn, L, exit_sites, n_exits),
    };
}

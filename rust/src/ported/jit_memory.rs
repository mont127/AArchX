//! ---- addressing ----
//! Guest addresses reach the host through one of three maps (plain guest_base,
//! low_base below LOW_LIMIT, top_base above TOP_LO) plus the commpage, which in
//! identity-mapped dynamic mode cannot be mapped at its guest address at all.
//! Blocks that need them carry a per-block mark so unmarked blocks emit no
//! guards.  Bases with two or more accesses are hoisted into JMEMBASE/JMEMBASE2/
//! JMEMBASE3 (x30 is free inside a body: a guest RET pops its continuation from
//! the shadow and the epilogue restores the C return address from the frame),
//! with base + index<<scale kept in JMEMAUX and recomputed at the loop head.
//! The hoist signature is published so a chained predecessor with the same
//! signature enters past the reload.  Identity mapping needs none of this: every
//! pinned base already holds guest_base + base.
//!
//! ---- memory ordering ----
//! Ordered (x86-TSO) mode makes scalar accesses acquire/release - flags, locks
//! and atomics are scalar, a release scalar store orders every earlier vector
//! store, an acquire scalar load orders every later vector load.  Vector
//! accesses stay plain: programs do not synchronize through a 16-byte access
//! (x86 does not even make them atomic), and ordering them cost memcpy 3.3x and
//! fpvec 3.0x of Rosetta.  What that gives up is a vector load followed by a
//! scalar re-read of a seqlock counter observing newer data than the counter
//! covers, and two vector stores becoming visible out of order; OCERZ_TSO_VECTOR
//! =1 orders them too.  Measured 2026-09-05 on M2 Max; FEX ships the same
//! default.  The 8- and 4-byte forms are plain as well, though compilers do move
//! pointers and counters through xmm registers: a plain movq store after an
//! ordered scalar store can become visible first, and a plain movq load can be
//! satisfied after the scalar load that follows it, so a reader sees a published
//! counter newer than the data written before it - hundreds to over a thousand
//! times in three million handoffs in tests/dynamic/tso_narrow.c, where x86
//! never shows it.  OCERZ_TSO_NARROW=1 orders movd and movq, two instructions and no
//! barrier each (fmov and stlur, ldr and a one-byte ldapur), and =2 every vector
//! access of 8 bytes or fewer.  Neither is the default: the first took R.E.P.O.'s
//! main menu from 65 frames a second to 44, the second costs more again, and no
//! program is known to depend on either.  An ordered vector load is a plain load
//! followed by a one-byte ACQUIRE load of the same address: same-address reads
//! are coherent, so the copy sees a value at least as new as the vector and
//! everything after is ordered behind it - TSO's load ordering with no barrier,
//! where a dmb ishld waited for every outstanding miss (40-60 ns per access on a
//! 4 MB working set).
//!
//! Apple silicon faults an acquire/release access only when it crosses a 16-byte
//! granule, so the alignment guard tests exactly that; testing natural alignment
//! instead sent three quarters of memcpy's unaligned tail accesses down the slow
//! arm.  Faulting sites are hot-patched, and the out-of-line store arm picks
//! dmb ish + plain store when the block also does ordered loads (the drain is
//! cheap then: memcpy 0.31s vs 1.1s for release pieces) and release pieces
//! otherwise (store-only loops: memset 0.10s vs 0.5-1.0s).  An ordered access
//! carries no test by default: it is an ldapur or stlur, and the first time one
//! crosses a granule the fault handler patches that site into a branch to a
//! tested arm.  Only an instruction whose fault could not be patched carries
//! the test inline, and only that instruction; a second fault there marks the
//! whole block.  Testing every ordered access cost four instructions on each
//! load and store the Wine layout translates, and marking a whole block for
//! one fault put the test in front of every access of an unrolled Unity loop
//! that had one misaligned slot (OCERZ_ALIGN_TEST_ALL=1 tests every access).
//! A patched arm stands in for one instruction in the middle of an emitter's
//! sequence, so it saves the two registers it borrows on the host stack and
//! tests the granule with a bit test instead of the flags: a cmov keeps its
//! condition in JTF across the load, and an arm that borrowed JTF made a cmov
//! from a straddling address take the wrong side.
//!
//!
//! esp-relative operands of 32-bit code are stack accesses too, plain as their push and pop already are.
//!
//! Below 12 GB in the Wine layout the host address is a constant too: the
//! guard's orr is folded into the mov, and the guard takes it as translated.
//!
//! A rip-relative address in the Wine layout is known when the block is
//! translated, so is its side of 12 GB: below it the translation is one orr,
//! between it and the top strip there is none.  Only an address emit_mem_ea
//! has just formed counts, and never for an instruction that also reaches the
//! stack implicitly, whose slot would take the constant's answer.
//!
//! A 32-bit address is below 4 GB, so in the Wine layout it is in the low
//! window whatever it is, and its translation is the orr alone, as the 32-bit
//! stack's is.  An address emit_mem_ea built is already zero-extended; any
//! other is zero-extended first, which is the 32-bit wrap besides.
//!
//! A block that bailed translates without the hoist: a learned variant, as the alignment marks make.
//!
//! The Wine layout's stack delta in x0 (emit_stack_delta), for the block being translated.
//!
//! The Wine layout's base hoist (select_low_hoist): its guest register, the instruction it holds until, and the displacement span.
//!
//! An operand the block's low hoist covers: its host address is JMEMBASE plus its displacement.
//!
//! The Wine layout's stack delta.  Every guest address below 12 GB is the host's
//! with low_base or'ed in, and every one between 12 GB and the top strip is the
//! host's own, and a thread's stack sits wholly on one side: so while rsp is
//! below 12 GB every stack slot is rsp + low_base, and otherwise rsp itself.  x0,
//! the guest-base register of the other layouts, holds that delta here, and a
//! push, a pop, a call, a ret or an rsp-relative operand adds it where the fast
//! guard spent a shift, a compare, a branch and an orr (the push itself, its
//! guard and its two conversions of rsp were thirteen instructions).  The delta
//! is computed without the flags, which may be live: (rsp >> 32) - 3 is negative
//! below 12 GB, its sign spread over the word masks low_base.  It is computed
//! when a body is entered from the dispatcher, after every call-out (which
//! clobbers x0) and before a chain, and before the next instruction once an
//! instruction other than push, pop, call or ret has written rsp: those move it
//! by eight, which no stack crosses 12 GB by.  OCERZ_NO_LOW_STACK_DELTA=1 keeps
//! the guard on stack slots; OCERZ_LOWSTACK_CHECK=1 checks x0 before every
//! instruction and traps on a stale one.
//!
//! Whether an instruction may move rsp by more than push, pop, call and ret do.
//!
//! Whether an instruction can move rsp to the other side of 12 GB, so that the
//! stack delta must be recomputed after it.  A frame's add or sub rsp, imm (and
//! lea rsp, [rsp + disp]) under 64 KB cannot carry a valid stack across: no
//! guest mapping straddles 12 GB, and in the Wine layout nothing can be mapped
//! from 12 GB up to hundreds of gigabytes, so a stack below 12 GB ends at or
//! under it and one above starts far over it.  Every prologue and epilogue paid
//! four instructions for that before.
//!
//! Whether an instruction reaches the stack without naming it as an operand.
//!
//! Whether every guest access an instruction makes is a stack slot: the implicit
//! ones of push, pop, call, ret and leave, and explicit operands based on rsp
//! with no index and a displacement under a megabyte.  An index can reach memory
//! nowhere near the stack - code on a thread with a stack below 12 GB loaded from
//! hundreds of gigabytes past rsp that way (iosurface_low_stack) - so an indexed
//! operand takes the guard.  The string instructions, which reach memory through
//! other registers, are never stack-only.
//!
//! An [rsp + disp] operand in the Wine layout, whose host address is rsp plus
//! the stack delta in x0: JTA takes rsp + x0 once, and the access carries the
//! displacement, so the stack slots a block touches between two moves of rsp
//! share one add (the address cache's base form, JTA = JGB + base).
//!
//! The Wine layout's base hoist.  Every access a 64-bit block makes through a
//! register pays the low-window test - lsr, cmp, b.hs, orr - before it, because
//! the register may point on either side of 12 GB.  For the base register with
//! the most accesses before the block first writes it, the test runs once, at
//! the loop head, on the whole span the block reaches from it: when the base
//! plus its smallest and largest displacement (and size) are all below 12 GB,
//! JMEMBASE (x17) holds base | low_base and those accesses are JMEMBASE plus
//! their displacement, with no test.  When they are not, the block has met a
//! pointer of the other kind: it leaves before running anything, and C retires
//! it and remembers its key, so its translation takes the test per access from
//! then on.  The test is flag-free, since a block may be entered with the guest's
//! flags live in NZCV.  It pays for itself from three accesses.  winbench64's
//! struct-of-floats loop went from 35.5 to 33.1 ms (Rosetta 12), against 28 with
//! no test at all.  OCERZ_NO_LOW_HOIST=1 turns it off.
//!
//! Blocks marked to translate without an assumption that failed them: open addressing on the block key.
//!
//! At the loop head: the span below 12 GB, then JMEMBASE = base | low_base; the
//! branches go to the bail stub.  Without touching the flags, which a block may
//! be entered with live in NZCV: (base | (base + hi)) >> 32 must be below 3,
//! which also catches base + hi wrapping, and base + lo must not go below zero.
//! A span that crosses 8 GB fails the or for no reason, which costs that block
//! its hoist and nothing else.
//!
//! Out of line: leave before the block's first instruction, with side_idx -2 asking C to retire it.  It
//! returns OCERZ_STEP_PROFILE, as a probe's side exit does: STEP_OK goes to the in-arena dispatcher,
//! which would enter the same block again without C ever seeing side_blk.

use core::ffi::{c_char, c_int, c_uint};
use core::mem::{MaybeUninit, offset_of};
use core::ptr::{null, null_mut};

use crate::ffi::*;
use crate::inline::ocerz_sext;
use crate::jit_internal as ji;

macro_rules! env_on {
    ($name:literal) => {{
        static mut ON_: c_int = -1;
        if ON_ < 0 {
            ON_ =
                (!libc::getenv(concat!($name, "\0").as_ptr() as *const c_char).is_null()) as c_int;
        }
        ON_
    }};
}

#[unsafe(no_mangle)]
pub static mut g_no_ldapr: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_no_oolslow: c_int = 0;

static mut g_const_ea_valid: c_int = 0;

static mut g_const_ea: u64 = 0;

#[unsafe(no_mangle)]
pub static mut g_ea_is_const: c_int = 0;

static mut g_ea_const: u64 = 0;

static mut g_ea_w32: c_int = 0;

static mut g_oslow: [OrderedSlowPend; OSLOW_MAX as usize] = [OrderedSlowPend {
    bne: null_mut(),
    back: null_mut(),
    size: 0,
    rv: 0,
    ra: 0,
    store: 0,
    idx: 0,
    vec: 0,
    disp: 0,
}; OSLOW_MAX as usize];

#[unsafe(no_mangle)]
pub static mut g_n_oslow: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_plain_mem: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_cp_marks: [u64; CP_MARK_SIZE as usize] = [0; CP_MARK_SIZE as usize];

#[unsafe(no_mangle)]
pub static mut g_cp_guard: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_low_top: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_lowstack: c_int = 0;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn stack_plain_ok() -> c_int {
    static mut EN: c_int = -1;
    unsafe {
        if EN < 0 {
            EN = if libc::getenv(c"OCERZ_TSO_STRICT".as_ptr()).is_null() {
                1
            } else {
                0
            };
        }
        EN
    }
}

#[unsafe(no_mangle)]
pub static mut g_al_marks: [u64; AL_MARK_SIZE as usize] = [0; AL_MARK_SIZE as usize];

#[unsafe(no_mangle)]
pub static mut g_align_guard: c_int = 0;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn vec_tso_relaxed() -> c_int {
    static mut V: c_int = -1;
    unsafe {
        if V < 0 {
            V = libc::getenv(c"OCERZ_TSO_VECTOR".as_ptr()).is_null() as c_int;
        }
        V
    }
}

fn vec_plain_size(size: c_int) -> c_int {
    unsafe {
        static mut MODE: c_int = -1;
        if MODE < 0 {
            MODE = if libc::getenv(c"OCERZ_TSO_NARROW".as_ptr()).is_null() {
                0
            } else {
                libc::atoi(libc::getenv(c"OCERZ_TSO_NARROW".as_ptr()))
            };
        }
        if vec_tso_relaxed() == 0 || size > 8 || MODE == 0 {
            return vec_tso_relaxed();
        }
        (MODE == 1 && g_vec_int_move == 0) as c_int
    }
}

#[unsafe(no_mangle)]
pub static mut g_blk_ordered_loads: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_al_all: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_al_n: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_mem_hoist_greg: c_int = -1;

#[unsafe(no_mangle)]
pub static mut g_low_hoist_greg: c_int = -1;

static mut g_low_hoist_until: c_int = 0;

static mut g_low_hoist_hi: i32 = 0;

static mut g_low_hoist_lo: i32 = 0;

unsafe fn low_hoist_covers(insn: *const X86Insn, op: *const X86Operand) -> c_int {
    unsafe {
        (g_low_hoist_greg >= 0
            && (*insn).seg == OCERZ_SEG_NONE as u8
            && (*insn).addrsize == 8
            && (*op).base as c_int == g_low_hoist_greg
            && (*op).index == OCERZ_REG_NONE as u8
            && (*op).riprel == 0
            && g_cur_insn_idx < g_low_hoist_until
            && (*op).disp >= g_low_hoist_lo as i64
            && (*op).disp < g_low_hoist_hi as i64) as c_int
    }
}

static mut g_low_hoist_bail: [*mut u32; 3] = [null_mut(); 3];

#[unsafe(no_mangle)]
pub static mut g_ea_lowhoisted: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_n_low_hoist_bail: c_int = 0;

static mut g_ea_lowhoisted_reg: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_mem_hoist_aux_disp: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_mem_hoist_aux_index: c_int = -1;

#[unsafe(no_mangle)]
pub static mut g_mem_hoist_aux_scale: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_mem_hoist_greg2: c_int = -1;

#[unsafe(no_mangle)]
pub static mut g_mem_hoist_greg3: c_int = -1;

#[inline]
unsafe fn hoist_reg_for(base: c_uint) -> c_int {
    unsafe {
        if g_mem_hoist_greg >= 0 && base == g_mem_hoist_greg as c_uint {
            return JMEMBASE as c_int;
        }
        if g_mem_hoist_greg2 >= 0 && base == g_mem_hoist_greg2 as c_uint {
            return JMEMBASE2 as c_int;
        }
        if g_mem_hoist_greg3 >= 0 && base == g_mem_hoist_greg3 as c_uint {
            return JMEMBASE3 as c_int;
        }
        if ocerz_guest_base == 0
            && ji::pin_slot(base) >= 0
            && !(g_pin_class_fwd() == 2 && base == OCERZ_RSP)
        {
            static mut DIS: c_int = -1;
            if DIS < 0 {
                DIS = if libc::getenv(c"OCERZ_NO_IDBASE".as_ptr()).is_null() {
                    0
                } else {
                    1
                };
            }
            if DIS == 0 {
                return ji::pin_hreg(ji::pin_slot(base));
            }
        }
        -1
    }
}

unsafe fn mem_hoist_view(m: *const X86Operand, tmp: *mut X86Operand) -> *const X86Operand {
    unsafe {
        if (*m).riprel != 0
            || (*m).base == OCERZ_REG_NONE as u8
            || (*m).index == OCERZ_REG_NONE as u8
            || ((*m).scale & 3) != 0
        {
            return m;
        }
        if hoist_reg_for((*m).base as c_uint) >= 0 || hoist_reg_for((*m).index as c_uint) < 0 {
            return m;
        }
        *tmp = *m;
        (*tmp).base = (*m).index;
        (*tmp).index = (*m).base;
        tmp
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn stack_identity() -> c_int {
    static mut DIS: c_int = -1;
    unsafe {
        if DIS < 0 {
            DIS = if libc::getenv(c"OCERZ_NO_STACK_IDX".as_ptr()).is_null() {
                0
            } else {
                1
            };
        }
        (DIS == 0 && ocerz_guest_base == 0 && ocerz_low_base == 0) as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_jgb_trap(rip: u64, x0: u64) {
    unsafe {
        libc::fprintf(
            crate::log::stderr(),
            c"ocerz: JGB TRAP entering body of block %#llx with x0=%#llx (gbase=%#llx)\n".as_ptr(),
            rip,
            x0,
            ocerz_guest_base,
        );
        libc::abort();
    }
}

unsafe fn m32_addr_src(b: *mut A64Buf, greg: c_uint, scratch: c_int) -> c_int {
    unsafe {
        let s = ji::pin_slot(greg);
        if s >= 0 {
            return ji::pin_hreg(s);
        }
        ji::emit_gpr_rd(b, 0, scratch, greg);
        scratch
    }
}

unsafe fn emit_mem_ea32(
    b: *mut A64Buf,
    insn: *const X86Insn,
    op: *const X86Operand,
    addr_reg: c_int,
) -> c_int {
    unsafe {
        let _ = insn;
        let fold = ji::ea_fold();
        let has_b = ((*op).base != OCERZ_REG_NONE as u8) as c_int;
        let mut has_i = ((*op).index != OCERZ_REG_NONE as u8) as c_int;
        let sc = ((*op).scale & 3) as c_int;
        let disp = (*op).disp;
        let d32 = disp as u32;

        if ji::rsp_is_ptr() != 0
            && ((has_b != 0 && (*op).base as c_uint == OCERZ_RSP)
                || (has_i != 0 && (*op).index as c_uint == OCERZ_RSP))
        {
            return 0;
        }

        if has_b == 0 && has_i == 0 {
            a64_mov_imm64(b, addr_reg, (d32 as u64).wrapping_add(fold));
            return 1;
        }

        let mut fold_reg = -1;
        if fold != 0 {
            if ji::jgb_usable() != 0 && fold == ocerz_guest_base {
                fold_reg = JGB as c_int;
            } else {
                a64_mov_imm64(b, JTU as c_int, fold);
                fold_reg = JTU as c_int;
            }
        }

        if fold_reg >= 0 && d32 == 0 && has_b != has_i {
            let g = if has_b != 0 { (*op).base } else { (*op).index };
            let r = m32_addr_src(b, g as c_uint, addr_reg);
            a64_add_ext_uxtw(b, addr_reg, fold_reg, r, if has_b != 0 { 0 } else { sc });
            return 1;
        }

        let rb = if has_b != 0 {
            m32_addr_src(b, (*op).base as c_uint, JT0 as c_int)
        } else {
            -1
        };
        let ri = if has_i != 0 {
            m32_addr_src(
                b,
                (*op).index as c_uint,
                if rb == JT0 as c_int {
                    JTT as c_int
                } else {
                    JT0 as c_int
                },
            )
        } else {
            -1
        };
        let t = addr_reg;
        let mut have = 0;

        if has_b != 0 && has_i != 0 && disp == 0 {
            a64_add_reg(b, 0, t, rb, ri, sc);
            have = 1;
            has_i = 0;
        } else if has_b != 0 && disp >= -4095 && disp <= 4095 {
            if disp > 0 {
                a64_add_imm(b, 0, t, rb, disp as u32);
            } else if disp < 0 {
                a64_sub_imm(b, 0, t, rb, disp.wrapping_neg() as u32);
            } else {
                a64_mov_reg(b, 0, t, rb);
            }
            have = 1;
        } else {
            if d32 != 0 {
                a64_mov_imm64(b, t, d32 as u64);
                have = 1;
            }
            if has_b != 0 {
                if have != 0 {
                    a64_add_reg(b, 0, t, t, rb, 0);
                } else {
                    a64_mov_reg(b, 0, t, rb);
                    have = 1;
                }
            }
        }
        if has_i != 0 {
            if have != 0 {
                a64_add_reg(b, 0, t, t, ri, sc);
            } else {
                a64_lsl_imm(b, 0, t, ri, sc);
                have = 1;
            }
        }
        let _ = have;
        if fold_reg >= 0 {
            a64_add_reg(b, 1, addr_reg, fold_reg, t, 0);
        }
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_mem_ea(
    b: *mut A64Buf,
    insn: *const X86Insn,
    op: *const X86Operand,
    addr_reg: c_int,
) -> c_int {
    unsafe {
        g_const_ea_valid = 0;
        g_ea_is_const = 0;
        g_ea_w32 = 0;
        g_ea_lowhoisted = 0;

        g_ea_plain = (ocerz_low_base != 0
            && (*insn).seg == OCERZ_SEG_NONE as u8
            && ji::mem_plain_access_ok(op) != 0
            && ((*insn).mode32 == 0 || env_on!("OCERZ_NO_M32_STACK_PLAIN") == 0))
            as c_int;
        let fold = ji::ea_fold();
        let seg = (*insn).seg as c_uint;
        if seg != OCERZ_SEG_NONE {
            static mut NO_SEG: c_int = -1;
            if NO_SEG < 0 {
                NO_SEG = if libc::getenv(c"OCERZ_NO_INLINE_SEG".as_ptr()).is_null() {
                    0
                } else {
                    1
                };
            }
            static mut NO_LOW_SEG: c_int = -1;
            if NO_LOW_SEG < 0 {
                NO_LOW_SEG = if libc::getenv(c"OCERZ_NO_LOW_SEG".as_ptr()).is_null() {
                    0
                } else {
                    1
                };
            }
            if NO_SEG != 0
                || (*op).riprel != 0
                || ((*insn).addrsize == 4 && (*insn).mode32 == 0)
                || (ocerz_low_base != 0 && NO_LOW_SEG != 0)
            {
                return 0;
            }
        }
        if (*op).riprel != 0 {
            let ga = ((*op).disp as u64).wrapping_add(fold);
            g_ea_is_const = (seg == OCERZ_SEG_NONE) as c_int;
            g_ea_const = ga;

            if g_ea_is_const != 0
                && ocerz_low_base != 0
                && fold == 0
                && ga < OCERZ_LOW_LIMIT
                && low_guard_fast_ok() != 0
                && insn_stack_implicit(insn) == 0
                && env_on!("OCERZ_NO_RIP_LOWFOLD") == 0
            {
                emit_const_lit(b, addr_reg, ga | ocerz_low_base);
                g_ea_lowhoisted = 1;
                g_ea_lowhoisted_reg = addr_reg;
                return 1;
            }
            a64_mov_imm64(b, addr_reg, ga);
            return 1;
        }
        if (*insn).addrsize == 4 {
            if (*insn).mode32 == 0 || emit_mem_ea32(b, insn, op, addr_reg) == 0 {
                return 0;
            }
            if seg == OCERZ_SEG_FS || seg == OCERZ_SEG_GS {
                a64_ldr(
                    b,
                    8,
                    JT0 as c_int,
                    20,
                    (if seg == OCERZ_SEG_FS {
                        offset_of!(OcerzCPU, fs_base)
                    } else {
                        offset_of!(OcerzCPU, gs_base)
                    }) as u32,
                );
                a64_add_reg(b, 1, addr_reg, addr_reg, JT0 as c_int, 0);
            } else if seg == OCERZ_SEG_NONE && fold == 0 {
                g_ea_w32 = 1;
                if (*op).base == OCERZ_REG_NONE as u8 && (*op).index == OCERZ_REG_NONE as u8 {
                    g_ea_is_const = 1;
                    g_ea_const = (*op).disp as u32 as u64;
                }
            }
            return 1;
        }
        if (*insn).addrsize != 8 {
            return 0;
        }
        if low_hoist_covers(insn, op) != 0 {
            if (*op).disp > 0 {
                a64_add_imm(b, 1, addr_reg, JMEMBASE as c_int, (*op).disp as u32);
            } else if (*op).disp < 0 {
                a64_sub_imm(
                    b,
                    1,
                    addr_reg,
                    JMEMBASE as c_int,
                    (*op).disp.wrapping_neg() as u32,
                );
            } else {
                a64_mov_reg(b, 1, addr_reg, JMEMBASE as c_int);
            }
            g_ea_lowhoisted = 1;
            g_ea_lowhoisted_reg = addr_reg;
            return 1;
        }
        let mut initial = ((*op).disp as u64).wrapping_add(fold);
        if ji::rsp_is_ptr() != 0
            && (*op).base as c_uint == OCERZ_RSP
            && ji::pin_slot(OCERZ_RSP) >= 0
        {
            initial = (*op).disp as u64;
        }
        if ji::jgb_usable() != 0
            && fold == ocerz_guest_base
            && seg == OCERZ_SEG_NONE
            && !(ji::rsp_is_ptr() != 0
                && ((*op).base as c_uint == OCERZ_RSP || (*op).index as c_uint == OCERZ_RSP))
            && (*op).disp >= -4095
            && (*op).disp <= 4095
            && ((*op).base == OCERZ_REG_NONE as u8 || ji::pin_slot((*op).base as c_uint) >= 0)
            && ((*op).index == OCERZ_REG_NONE as u8 || ji::pin_slot((*op).index as c_uint) >= 0)
        {
            let mut have = 0;
            if (*op).base != OCERZ_REG_NONE as u8 {
                a64_add_reg(
                    b,
                    1,
                    addr_reg,
                    JGB as c_int,
                    ji::pin_hreg(ji::pin_slot((*op).base as c_uint)),
                    0,
                );
                have = 1;
            }
            if (*op).index != OCERZ_REG_NONE as u8 {
                a64_add_reg(
                    b,
                    1,
                    addr_reg,
                    if have != 0 { addr_reg } else { JGB as c_int },
                    ji::pin_hreg(ji::pin_slot((*op).index as c_uint)),
                    ((*op).scale & 3) as c_int,
                );
                have = 1;
            }
            if have == 0 {
                a64_mov_reg(b, 1, addr_reg, JGB as c_int);
            }
            if (*op).disp > 0 {
                a64_add_imm(b, 1, addr_reg, addr_reg, (*op).disp as u32);
            } else if (*op).disp < 0 {
                a64_sub_imm(b, 1, addr_reg, addr_reg, (*op).disp.wrapping_neg() as u32);
            }
            return 1;
        }
        let mut index_done = 0;
        if (*op).base != OCERZ_REG_NONE as u8
            && ji::pin_slot((*op).base as c_uint) >= 0
            && (initial as i64) >= -4095
            && (initial as i64) <= 4095
        {
            let hb = ji::pin_hreg(ji::pin_slot((*op).base as c_uint));
            let xs = if (*op).index != OCERZ_REG_NONE as u8 {
                ji::pin_slot((*op).index as c_uint)
            } else {
                -1
            };
            if initial as i64 == 0
                && xs >= 0
                && !(ji::rsp_is_ptr() != 0 && (*op).index as c_uint == OCERZ_RSP)
            {
                a64_add_reg(
                    b,
                    1,
                    addr_reg,
                    hb,
                    ji::pin_hreg(xs),
                    ((*op).scale & 3) as c_int,
                );
                index_done = 1;
            } else if initial as i64 > 0 {
                a64_add_imm(b, 1, addr_reg, hb, initial as u32);
            } else if (initial as i64) < 0 {
                a64_sub_imm(b, 1, addr_reg, hb, (initial as i64).wrapping_neg() as u32);
            } else {
                a64_mov_reg(b, 1, addr_reg, hb);
            }
        } else {
            a64_mov_imm64(b, addr_reg, initial);
            if (*op).base != OCERZ_REG_NONE as u8 {
                let s = ji::pin_slot((*op).base as c_uint);
                if s >= 0 {
                    a64_add_reg(b, 1, addr_reg, addr_reg, ji::pin_hreg(s), 0);
                } else {
                    ji::emit_gpr_rd(b, 1, JT0 as c_int, (*op).base as c_uint);
                    a64_add_reg(b, 1, addr_reg, addr_reg, JT0 as c_int, 0);
                }
            }
        }
        if (*op).index != OCERZ_REG_NONE as u8 && index_done == 0 {
            let s = ji::pin_slot((*op).index as c_uint);
            if s >= 0 && !(ji::rsp_is_ptr() != 0 && (*op).index as c_uint == OCERZ_RSP) {
                a64_add_reg(
                    b,
                    1,
                    addr_reg,
                    addr_reg,
                    ji::pin_hreg(s),
                    ((*op).scale & 3) as c_int,
                );
            } else {
                ji::emit_gpr_rd(b, 1, JT0 as c_int, (*op).index as c_uint);
                a64_add_reg(
                    b,
                    1,
                    addr_reg,
                    addr_reg,
                    JT0 as c_int,
                    ((*op).scale & 3) as c_int,
                );
            }
        }
        if seg == OCERZ_SEG_FS {
            a64_ldr(b, 8, JT0 as c_int, 20, offset_of!(OcerzCPU, fs_base) as u32);
            a64_add_reg(b, 1, addr_reg, addr_reg, JT0 as c_int, 0);
        } else if seg == OCERZ_SEG_GS {
            a64_ldr(b, 8, JT0 as c_int, 20, offset_of!(OcerzCPU, gs_base) as u32);
            a64_add_reg(b, 1, addr_reg, addr_reg, JT0 as c_int, 0);
            if (*op).disp == 0x58
                && (*op).base == OCERZ_REG_NONE as u8
                && (*op).index == OCERZ_REG_NONE as u8
                && (*op).riprel == 0
                && (*insn).addrsize == 8
                && fold == 0
                && ocerz_low_base != 0
            {
                a64_lsr_imm(b, 1, JTU as c_int, JT0 as c_int, 32);
                let low_gs = a64_label(b);
                a64_cbz(b, 1, JTU as c_int, 0);
                a64_add_imm(b, 1, JT0 as c_int, JT0 as c_int, 0x30);
                emit_commpage_guard(b, insn, JT0 as c_int, null_mut(), null_mut());
                ji::emit_add_const(
                    b,
                    JT0 as c_int,
                    ocerz_guest_base.wrapping_sub(ji::ea_fold()),
                );
                a64_ldr(b, 8, JTT as c_int, JT0 as c_int, 0);
                let no_self = a64_label(b);
                a64_cbz(b, 1, JTT as c_int, 0);
                a64_add_imm(b, 1, addr_reg, JTT as c_int, 0x58);
                a64_patch_cbz(low_gs, a64_label(b));
                a64_patch_cbz(no_self, a64_label(b));
            } else if (*op).disp == 0x58
                && (*op).base == OCERZ_REG_NONE as u8
                && (*op).index == OCERZ_REG_NONE as u8
                && (*op).riprel == 0
                && (*insn).addrsize == 8
                && fold == 0
            {
                a64_lsr_imm(b, 1, JTU as c_int, JT0 as c_int, 32);
                a64_cbz(b, 1, JTU as c_int, 5);
                a64_sub_imm(b, 1, JTT as c_int, addr_reg, 0x28);
                a64_ldr(b, 8, JTT as c_int, JTT as c_int, 0);
                a64_cbz(b, 1, JTT as c_int, 2);
                a64_add_imm(b, 1, addr_reg, JTT as c_int, 0x58);
            }
        }
        1
    }
}

unsafe fn insn_const_addr(insn: *const X86Insn, ga: *mut u64) -> c_int {
    unsafe {
        if insn.is_null() || (*insn).seg != OCERZ_SEG_NONE as u8 {
            return 0;
        }
        for i in 0..(*insn).nops as usize {
            let o = &(*insn).ops[i];
            if o.kind as c_uint != OCERZ_OPK_MEM {
                continue;
            }
            if o.riprel != 0 {
                *ga = o.disp as u64;
                return 1;
            }
            if (*insn).addrsize == 8
                && o.base == OCERZ_REG_NONE as u8
                && o.index == OCERZ_REG_NONE as u8
            {
                *ga = o.disp as u64;
                return 1;
            }
            return 0;
        }
        0
    }
}

#[repr(C)]
#[derive(Copy, Clone)]
struct Garm {
    site: *mut u32,
    back: *mut u32,
    reg: c_int,
    idx: c_int,
}

static mut g_garm: [Garm; GUARD_ARMS_MAX as usize] = [Garm {
    site: null_mut(),
    back: null_mut(),
    reg: 0,
    idx: 0,
}; GUARD_ARMS_MAX as usize];

#[unsafe(no_mangle)]
pub static mut g_n_garm: c_int = 0;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn low_guard_fast_ok() -> c_int {
    static mut OK: c_int = -1;
    unsafe {
        if OK < 0 && ocerz_low_base != 0 {
            OK = 0;
            if libc::getenv(c"OCERZ_NO_FAST_LOW_GUARD".as_ptr()).is_null()
                && (ocerz_low_base & ((1u64 << 34) - 1)) == 0
            {
                let mut w = [0u32; 2];
                let mut t = A64Buf {
                    start: w.as_mut_ptr(),
                    p: w.as_mut_ptr(),
                    end: w.as_mut_ptr().add(2),
                    overflow: 0,
                    sink: 0,
                };
                OK = a64_try_orr_imm(&mut t, 1, 1, 1, ocerz_low_base);
            }
        }
        (OK > 0) as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lowstack_delta_ok() -> c_int {
    static mut OFF: c_int = -1;
    unsafe {
        if OFF < 0 {
            OFF = if libc::getenv(c"OCERZ_NO_LOW_STACK_DELTA".as_ptr()).is_null() {
                0
            } else {
                1
            };
        }
        (OFF == 0
            && ocerz_low_base != 0
            && ocerz_guest_base == 0
            && g_pin_class == 3
            && g_xlat_mode32 == 0
            && ji::pin_slot(OCERZ_RSP) >= 0
            && ji::rsp_is_ptr() != 0
            && low_guard_fast_ok() != 0) as c_int
    }
}

unsafe fn rsp_small_adjust(in_: *const X86Insn) -> c_int {
    unsafe {
        let d = &(*in_).ops[0];
        let s = &(*in_).ops[1];
        if (*in_).nops != 2
            || d.kind as c_uint != OCERZ_OPK_REG
            || (d.reg & 15) as c_uint != OCERZ_RSP
            || d.size != 8
        {
            return 0;
        }
        if (*in_).op as c_uint == OCERZ_OP_ADD || (*in_).op as c_uint == OCERZ_OP_SUB {
            return (s.kind as c_uint == OCERZ_OPK_IMM
                && s.imm as i64 > -65536
                && (s.imm as i64) < 65536) as c_int;
        }
        if (*in_).op as c_uint == OCERZ_OP_LEA {
            return (s.kind as c_uint == OCERZ_OPK_MEM
                && s.riprel == 0
                && s.base as c_uint == OCERZ_RSP
                && s.index == OCERZ_REG_NONE as u8
                && s.disp > -65536
                && s.disp < 65536) as c_int;
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lowstack_disturbs(in_: *const X86Insn) -> c_int {
    unsafe {
        match (*in_).op as c_uint {
            OCERZ_OP_PUSH | OCERZ_OP_CALL | OCERZ_OP_RET => 0,
            OCERZ_OP_ADD | OCERZ_OP_SUB | OCERZ_OP_LEA => {
                if rsp_small_adjust(in_) != 0 && env_on!("OCERZ_LOWSTACK_ADJ_RECOMPUTE") == 0 {
                    return 0;
                }
                insn_may_write_gpr(in_, OCERZ_RSP)
            }
            OCERZ_OP_POP => {
                ((*in_).nops > 0
                    && (*in_).ops[0].kind as c_uint == OCERZ_OPK_REG
                    && ((*in_).ops[0].reg & 15) as c_uint == OCERZ_RSP) as c_int
            }
            _ => insn_may_write_gpr(in_, OCERZ_RSP),
        }
    }
}

unsafe fn insn_stack_implicit(in_: *const X86Insn) -> c_int {
    unsafe {
        match (*in_).op as c_uint {
            OCERZ_OP_PUSH | OCERZ_OP_POP | OCERZ_OP_CALL | OCERZ_OP_RET | OCERZ_OP_LEAVE
            | OCERZ_OP_PUSHF | OCERZ_OP_POPF => 1,
            _ => 0,
        }
    }
}

unsafe fn insn_stack_only(in_: *const X86Insn) -> c_int {
    unsafe {
        if (*in_).seg != OCERZ_SEG_NONE as u8 || (*in_).mode32 != 0 || (*in_).addrsize != 8 {
            return 0;
        }
        let mut implicit = 0;
        match (*in_).op as c_uint {
            OCERZ_OP_PUSH | OCERZ_OP_POP | OCERZ_OP_CALL | OCERZ_OP_RET | OCERZ_OP_LEAVE
            | OCERZ_OP_PUSHF | OCERZ_OP_POPF => {
                implicit = 1;
            }
            OCERZ_OP_MOVS | OCERZ_OP_STOS | OCERZ_OP_LODS | OCERZ_OP_SCAS | OCERZ_OP_CMPS => {
                return 0;
            }
            _ => {}
        }
        let mut mem = 0;
        for k in 0..(*in_).nops as usize {
            let o = &(*in_).ops[k];
            if o.kind as c_uint != OCERZ_OPK_MEM {
                continue;
            }
            if o.riprel != 0
                || (o.base & 15) as c_uint != OCERZ_RSP
                || o.base == OCERZ_REG_NONE as u8
                || o.index != OCERZ_REG_NONE as u8
                || o.disp >= 1 << 20
                || o.disp <= -(1 << 20)
            {
                return 0;
            }
            mem = 1;
        }
        (implicit != 0 || mem != 0) as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_commpage_guard(
    b: *mut A64Buf,
    insn: *const X86Insn,
    addr_reg: c_int,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> *mut u32 {
    unsafe {
        let _ = exit_sites;
        let _ = n_exits;
        g_const_ea_valid = 0;
        if g_ea_lowhoisted != 0 && addr_reg == g_ea_lowhoisted_reg {
            g_ea_lowhoisted = 0;
            if g_ea_is_const != 0 {
                g_const_ea = g_ea_const;
                g_const_ea_valid = 1;
            }
            g_ea_is_const = 0;
            return null_mut();
        }
        if g_lowstack != 0 && !insn.is_null() && insn_stack_only(insn) != 0 {
            g_ea_is_const = 0;
            a64_add_reg(b, 1, addr_reg, addr_reg, JGB as c_int, 0);
            return null_mut();
        }

        if g_ea_is_const != 0
            && ocerz_low_base != 0
            && ji::ea_fold() == 0
            && low_guard_fast_ok() != 0
            && !insn.is_null()
            && insn_stack_implicit(insn) == 0
        {
            let ga = g_ea_const;
            g_ea_is_const = 0;
            if ga < OCERZ_LOW_LIMIT {
                a64_try_orr_imm(b, 1, addr_reg, addr_reg, ocerz_low_base);
                g_const_ea = ga;
                g_const_ea_valid = 1;
                return null_mut();
            }
            if ga < OCERZ_TOP_LO
                && !(ocerz_commpage != null_mut()
                    && ga >= OCERZ_COMMPAGE_LO
                    && ga < OCERZ_COMMPAGE_HI)
            {
                g_const_ea = ga;
                g_const_ea_valid = 1;
                return null_mut();
            }
        }
        g_ea_is_const = 0;

        if !insn.is_null()
            && (*insn).mode32 != 0
            && (*insn).addrsize == 4
            && (*insn).seg == OCERZ_SEG_NONE as u8
            && ocerz_low_base != 0
            && ji::ea_fold() == 0
            && low_guard_fast_ok() != 0
            && env_on!("OCERZ_NO_M32_ORR") == 0
        {
            if g_ea_w32 == 0 {
                a64_mov_reg(b, 0, addr_reg, addr_reg);
            }
            g_ea_w32 = 0;
            a64_try_orr_imm(b, 1, addr_reg, addr_reg, ocerz_low_base);
            return null_mut();
        }
        g_ea_w32 = 0;
        if ocerz_commpage == null_mut() && ocerz_low_base == 0 {
            return null_mut();
        }
        let mut ga: u64 = 0;
        if ocerz_low_base == 0 && insn_const_addr(insn, &mut ga) != 0 {
            if ga >= OCERZ_COMMPAGE_LO && ga < OCERZ_COMMPAGE_HI {
                ji::tc_imm64(
                    b,
                    JTU as c_int,
                    TCR_COMMPAGE as c_int,
                    0,
                    (ocerz_commpage as u64)
                        .wrapping_sub(OCERZ_COMMPAGE_LO)
                        .wrapping_sub(ocerz_guest_base),
                );
                a64_add_reg(b, 1, addr_reg, addr_reg, JTU as c_int, 0);
                return null_mut();
            }
            g_const_ea = ga;
            g_const_ea_valid = 1;
            return null_mut();
        }
        if ocerz_low_base != 0 && ji::ea_fold() == 0 && low_guard_fast_ok() != 0 {
            a64_lsr_imm(b, 1, JTT as c_int, addr_reg, 32);
            a64_subs_imm(
                b,
                1,
                A64_ZR as c_int,
                JTT as c_int,
                (OCERZ_LOW_LIMIT >> 32) as u32,
            );
            let high = a64_label(b);
            a64_bcond(b, A64_CS as c_int, 0);
            a64_try_orr_imm(b, 1, addr_reg, addr_reg, ocerz_low_base);
            if g_low_top == 0 {
                a64_patch_bcond(high, a64_label(b));
                return null_mut();
            }
            let done_low = a64_label(b);
            a64_b(b, 0);
            a64_patch_bcond(high, a64_label(b));
            a64_lsr_imm(b, 1, JTT as c_int, addr_reg, 25);
            a64_add_imm(b, 1, JTT as c_int, JTT as c_int, 1);
            a64_lsr_imm(b, 1, JTT as c_int, JTT as c_int, 22);
            if g_n_garm < GUARD_ARMS_MAX as c_int {
                let e = &mut g_garm[g_n_garm as usize];
                e.site = a64_label(b);
                a64_cbnz(b, 1, JTT as c_int, 0);
                e.back = a64_label(b);
                e.reg = addr_reg;
                e.idx = g_cur_insn_idx;
                g_n_garm += 1;
                a64_patch_b(done_low, a64_label(b));
                return null_mut();
            }
            let identity = a64_label(b);
            a64_cbz(b, 1, JTT as c_int, 0);
            emit_guard_full(b, addr_reg);
            let done_full = a64_label(b);
            a64_b(b, 0);
            a64_patch_cbz(identity, a64_label(b));
            a64_patch_b(done_low, a64_label(b));
            a64_patch_b(done_full, a64_label(b));
            return null_mut();
        }
        emit_guard_full(b, addr_reg);
        null_mut()
    }
}

unsafe fn emit_guard_full(b: *mut A64Buf, addr_reg: c_int) {
    unsafe {
        let fold = ji::ea_fold();
        let mut to_native = null_mut();
        if ocerz_low_base != 0 {
            a64_mov_imm64(b, JTU as c_int, OCERZ_LOW_LIMIT.wrapping_add(fold));
            a64_sub_reg(b, 1, JTT as c_int, addr_reg, JTU as c_int, 0);
            a64_mov_imm64(b, JTU as c_int, OCERZ_TOP_LO.wrapping_sub(OCERZ_LOW_LIMIT));
            a64_subs_reg(b, 1, A64_ZR as c_int, JTT as c_int, JTU as c_int, 0);
            to_native = a64_label(b);
            a64_bcond(b, A64_CC as c_int, 0);
        }
        let mut done_cp = null_mut();
        if ocerz_commpage != null_mut() {
            a64_mov_imm64(b, JTU as c_int, OCERZ_COMMPAGE_LO.wrapping_add(fold));
            a64_sub_reg(b, 1, JTT as c_int, addr_reg, JTU as c_int, 0);
            a64_mov_imm64(
                b,
                JTU as c_int,
                OCERZ_COMMPAGE_HI.wrapping_sub(OCERZ_COMMPAGE_LO),
            );
            a64_subs_reg(b, 1, A64_ZR as c_int, JTT as c_int, JTU as c_int, 0);
            let not_cp = a64_label(b);
            a64_bcond(b, A64_CS as c_int, 0);
            ji::tc_imm64(
                b,
                JTU as c_int,
                TCR_COMMPAGE as c_int,
                0,
                (ocerz_commpage as u64)
                    .wrapping_sub(OCERZ_COMMPAGE_LO)
                    .wrapping_sub(ocerz_guest_base),
            );
            a64_add_reg(b, 1, addr_reg, addr_reg, JTU as c_int, 0);
            done_cp = a64_label(b);
            a64_b(b, 0);
            a64_patch_bcond(not_cp, a64_label(b));
        }
        if ocerz_low_base != 0 {
            a64_mov_imm64(b, JTU as c_int, OCERZ_TOP_LO.wrapping_add(fold));
            a64_subs_reg(b, 1, A64_ZR as c_int, addr_reg, JTU as c_int, 0);
            let is_low = a64_label(b);
            a64_bcond(b, A64_CC as c_int, 0);
            a64_mov_imm64(
                b,
                JTU as c_int,
                ocerz_top_base
                    .wrapping_sub(OCERZ_TOP_LO)
                    .wrapping_sub(ocerz_guest_base),
            );
            a64_add_reg(b, 1, addr_reg, addr_reg, JTU as c_int, 0);
            let done_top = a64_label(b);
            a64_b(b, 0);
            a64_patch_bcond(is_low, a64_label(b));
            a64_mov_imm64(
                b,
                JTU as c_int,
                ocerz_low_base.wrapping_sub(ocerz_guest_base),
            );
            a64_add_reg(b, 1, addr_reg, addr_reg, JTU as c_int, 0);
            a64_patch_b(done_top, a64_label(b));
        }
        if !done_cp.is_null() {
            a64_patch_b(done_cp, a64_label(b));
        }
        if !to_native.is_null() {
            a64_patch_bcond(to_native, a64_label(b));
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_reload_mem_base(b: *mut A64Buf) {
    unsafe {
        if g_low_hoist_greg >= 0 {
            a64_try_orr_imm(
                b,
                1,
                JMEMBASE as c_int,
                ji::pin_hreg(ji::pin_slot(g_low_hoist_greg as c_uint)),
                ocerz_low_base,
            );
        }
        if g_mem_hoist_greg < 0 {
            return;
        }
        let bs = ji::pin_slot(g_mem_hoist_greg as c_uint);
        assert!(bs >= 0);
        if ji::jgb_usable() != 0 {
            a64_add_reg(b, 1, JMEMBASE as c_int, JGB as c_int, ji::pin_hreg(bs), 0);
        } else {
            a64_mov_imm64(b, JMEMBASE as c_int, ocerz_guest_base);
            a64_add_reg(
                b,
                1,
                JMEMBASE as c_int,
                JMEMBASE as c_int,
                ji::pin_hreg(bs),
                0,
            );
        }
        if g_mem_hoist_aux_index >= 0 {
            a64_add_reg(
                b,
                1,
                JMEMAUX as c_int,
                JMEMBASE as c_int,
                ji::pin_hreg(ji::pin_slot(g_mem_hoist_aux_index as c_uint)),
                g_mem_hoist_aux_scale,
            );
        } else if g_mem_hoist_aux_disp > 0 {
            a64_add_imm(
                b,
                1,
                JMEMAUX as c_int,
                JMEMBASE as c_int,
                g_mem_hoist_aux_disp as u32,
            );
        } else if g_mem_hoist_aux_disp < 0 {
            a64_sub_imm(
                b,
                1,
                JMEMAUX as c_int,
                JMEMBASE as c_int,
                g_mem_hoist_aux_disp.wrapping_neg() as u32,
            );
        }
        if g_mem_hoist_greg2 >= 0 {
            let bs2 = ji::pin_slot(g_mem_hoist_greg2 as c_uint);
            assert!(bs2 >= 0);
            if ji::jgb_usable() != 0 {
                a64_add_reg(b, 1, JMEMBASE2 as c_int, JGB as c_int, ji::pin_hreg(bs2), 0);
            } else {
                a64_mov_imm64(b, JMEMBASE2 as c_int, ocerz_guest_base);
                a64_add_reg(
                    b,
                    1,
                    JMEMBASE2 as c_int,
                    JMEMBASE2 as c_int,
                    ji::pin_hreg(bs2),
                    0,
                );
            }
        }
        if g_mem_hoist_greg3 >= 0 {
            let bs3 = ji::pin_slot(g_mem_hoist_greg3 as c_uint);
            assert!(bs3 >= 0);
            if ji::jgb_usable() != 0 {
                a64_add_reg(b, 1, JMEMBASE3 as c_int, JGB as c_int, ji::pin_hreg(bs3), 0);
            } else {
                a64_mov_imm64(b, JMEMBASE3 as c_int, ocerz_guest_base);
                a64_add_reg(
                    b,
                    1,
                    JMEMBASE3 as c_int,
                    JMEMBASE3 as c_int,
                    ji::pin_hreg(bs3),
                    0,
                );
            }
        }
    }
}

unsafe fn emit_hoisted_mem_access(
    b: *mut A64Buf,
    insn: *const X86Insn,
    mem: *const X86Operand,
    size: c_int,
    value_reg: c_int,
    store: c_int,
) -> c_int {
    unsafe {
        if g_mem_hoist_greg < 0
            || (*insn).seg != OCERZ_SEG_NONE as u8
            || (*insn).addrsize != 8
            || (*mem).riprel != 0
            || (*mem).base == OCERZ_REG_NONE as u8
        {
            return 0;
        }
        let plain = ji::mem_plain_access_ok(mem);
        let mut mview = MaybeUninit::<X86Operand>::uninit();
        let mem = mem_hoist_view(mem, mview.as_mut_ptr());
        if ji::rsp_is_ptr() != 0
            && ((*mem).base as c_uint == OCERZ_RSP || (*mem).index as c_uint == OCERZ_RSP)
        {
            return 0;
        }
        let hbase = hoist_reg_for((*mem).base as c_uint);
        if hbase < 0 {
            return 0;
        }

        let disp = (*mem).disp;
        if (*mem).index == OCERZ_REG_NONE as u8 {
            if disp < 0 || disp as u64 > 4095u64 * size as u64 || (disp & (size as i64 - 1)) != 0 {
                return 0;
            }
            if store != 0 {
                emit_gpr_st_at(b, size, value_reg, hbase, disp as i32, plain);
            } else {
                emit_gpr_ld_at(b, size, value_reg, hbase, disp as i32, plain);
            }
            return 1;
        }

        let is = ji::pin_slot((*mem).index as c_uint);
        let want_scale = if size == 8 {
            3
        } else if size == 4 {
            2
        } else if size == 2 {
            1
        } else {
            0
        };
        if is < 0 || ((*mem).scale & 3) as c_int != want_scale || disp < -4095 || disp > 4095 {
            return 0;
        }
        if plain == 0 {
            let mut ra: c_int = 0;
            let mut d: u32 = 0;
            if emit_mem_ea_plain_ex(b, insn, mem, size, &mut ra, &mut d, 1) != 0 {
                if store != 0 {
                    emit_gpr_st_at(b, size, value_reg, ra, d as i32, 0);
                } else {
                    emit_gpr_ld_at(b, size, value_reg, ra, d as i32, 0);
                }
                return 1;
            }
        }
        let mut base = hbase;
        if hbase == JMEMBASE as c_int
            && g_mem_hoist_aux_index >= 0
            && (*mem).index as c_int == g_mem_hoist_aux_index
            && ((*mem).scale & 3) as c_int == g_mem_hoist_aux_scale
        {
            if disp < 0 || disp as u64 > 4095u64 * size as u64 || (disp & (size as i64 - 1)) != 0 {
                return 0;
            }
            if store != 0 {
                emit_gpr_st_at(b, size, value_reg, JMEMAUX as c_int, disp as i32, plain);
            } else {
                emit_gpr_ld_at(b, size, value_reg, JMEMAUX as c_int, disp as i32, plain);
            }
            return 1;
        }
        if disp != 0
            && disp == g_mem_hoist_aux_disp as i64
            && g_mem_hoist_aux_index < 0
            && hbase == JMEMBASE as c_int
        {
            base = JMEMAUX as c_int;
        } else if disp != 0 {
            if disp > 0 {
                a64_add_imm(b, 1, JTA as c_int, hbase, disp as u32);
            } else {
                a64_sub_imm(b, 1, JTA as c_int, hbase, disp.wrapping_neg() as u32);
            }
            base = JTA as c_int;
        }
        if store != 0 {
            emit_gpr_st_regoff(b, size, value_reg, base, ji::pin_hreg(is), 1, plain);
        } else {
            emit_gpr_ld_regoff(b, size, value_reg, base, ji::pin_hreg(is), 1, plain);
        }
        1
    }
}

fn align_test_all() -> c_int {
    unsafe {
        static mut ON: c_int = -1;
        if ON < 0 {
            ON = if libc::getenv(c"OCERZ_ALIGN_TEST_ALL".as_ptr()).is_null() {
                0
            } else {
                1
            };
        }
        ON
    }
}

unsafe fn emit_granule_cross_test(b: *mut A64Buf, size: c_int, ra: c_int, scratch: c_int) {
    unsafe {
        a64_add_imm(b, 1, scratch, ra, (size - 1) as u32);
        a64_eor_reg(b, 1, scratch, scratch, ra, 0);
        a64_try_ands_imm(b, 1, A64_ZR as c_int, scratch, 16);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_guest_store_ordered(
    b: *mut A64Buf,
    size: c_int,
    rv: c_int,
    ra: c_int,
    scratch: c_int,
) {
    unsafe {
        if g_plain_mem != 0 || g_ea_plain != 0 {
            a64_str(b, size, rv, ra, 0);
            return;
        }

        if size == 1 {
            a64_stlr(b, 1, rv, ra);
            return;
        }
        if g_const_ea_valid != 0 {
            let aligned = (g_const_ea & (size as u64 - 1)) == 0;
            g_const_ea_valid = 0;
            if aligned {
                a64_stlr(b, size, rv, ra);
                return;
            }
        }
        if g_align_guard == 0 && align_test_all() == 0 {
            a64_stlur(b, size, rv, ra, 0);
            return;
        }
        emit_granule_cross_test(b, size, ra, scratch);
        if g_no_oolslow == 0 && g_n_oslow < OSLOW_MAX as c_int {
            let bne = a64_label(b);
            a64_bcond(b, A64_NE as c_int, 0);
            a64_stlr(b, size, rv, ra);
            g_oslow[g_n_oslow as usize] = OrderedSlowPend {
                bne,
                back: a64_label(b),
                size,
                rv,
                ra,
                store: 1,
                idx: g_cur_insn_idx,
                vec: 0,
                disp: 0,
            };
            g_n_oslow += 1;
            ji::ea_cache_reset();
            return;
        }
        let to_aligned = a64_label(b);
        a64_bcond(b, A64_EQ as c_int, 0);
        a64_dmb_ish(b);
        a64_str(b, size, rv, ra, 0);
        let to_done = a64_label(b);
        a64_b(b, 0);
        a64_patch_bcond(to_aligned, a64_label(b));
        a64_stlr(b, size, rv, ra);
        a64_patch_b(to_done, a64_label(b));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_guest_load_ordered(
    b: *mut A64Buf,
    size: c_int,
    rd: c_int,
    ra: c_int,
    scratch: c_int,
) {
    unsafe {
        if g_plain_mem != 0 || g_ea_plain != 0 {
            a64_ldr(b, size, rd, ra, 0);
            return;
        }
        g_blk_ordered_loads = 1;
        if size == 1 {
            if g_no_ldapr != 0 {
                a64_ldar(b, 1, rd, ra);
            } else {
                a64_ldapr(b, 1, rd, ra);
            }
            return;
        }
        if g_const_ea_valid != 0 {
            let aligned = (g_const_ea & (size as u64 - 1)) == 0;
            g_const_ea_valid = 0;
            if aligned {
                if g_no_ldapr != 0 {
                    a64_ldar(b, size, rd, ra);
                } else {
                    a64_ldapr(b, size, rd, ra);
                }
                return;
            }
        }
        if g_align_guard == 0 && g_no_ldapr == 0 && align_test_all() == 0 {
            a64_ldapur(b, size, rd, ra, 0);
            return;
        }
        emit_granule_cross_test(b, size, ra, scratch);
        if g_no_oolslow == 0 && g_n_oslow < OSLOW_MAX as c_int {
            let bne = a64_label(b);
            a64_bcond(b, A64_NE as c_int, 0);
            if g_no_ldapr != 0 {
                a64_ldar(b, size, rd, ra);
            } else {
                a64_ldapr(b, size, rd, ra);
            }
            g_oslow[g_n_oslow as usize] = OrderedSlowPend {
                bne,
                back: a64_label(b),
                size,
                rv: rd,
                ra,
                store: 0,
                idx: g_cur_insn_idx,
                vec: 0,
                disp: 0,
            };
            ji::ea_cache_reset();
            g_n_oslow += 1;
            return;
        }
        let to_aligned = a64_label(b);
        a64_bcond(b, A64_EQ as c_int, 0);
        a64_ldr(b, size, rd, ra, 0);
        a64_dmb_ish(b);
        let to_done = a64_label(b);
        a64_b(b, 0);
        a64_patch_bcond(to_aligned, a64_label(b));

        if g_no_ldapr != 0 {
            a64_ldar(b, size, rd, ra);
        } else {
            a64_ldapr(b, size, rd, ra);
        }
        a64_patch_b(to_done, a64_label(b));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_gpr_ld_at(
    b: *mut A64Buf,
    size: c_int,
    rd: c_int,
    ra: c_int,
    disp: i32,
    plain: c_int,
) {
    unsafe {
        let scaled = (disp >= 0 && (disp % size) == 0 && disp / size <= 4095) as c_int;
        if plain == 0 {
            g_blk_ordered_loads = 1;
        }
        if plain != 0 {
            if scaled != 0 {
                a64_ldr(b, size, rd, ra, disp as u32);
            } else if (-256..=255).contains(&disp) {
                a64_ldur(b, size, rd, ra, disp);
            } else {
                a64_mov_imm64(b, JTU as c_int, disp as i64 as u64);
                a64_add_reg(b, 1, JTA as c_int, ra, JTU as c_int, 0);
                a64_ldr(b, size, rd, JTA as c_int, 0);
            }
            return;
        }
        if g_align_guard == 0 || size == 1 {
            if (-256..=255).contains(&disp) {
                a64_ldapur(b, size, rd, ra, disp);
                return;
            }
            if scaled != 0 && disp <= 4095 {
                a64_add_imm(b, 1, JTA as c_int, ra, disp as u32);
            } else {
                a64_mov_imm64(b, JTU as c_int, disp as i64 as u64);
                a64_add_reg(b, 1, JTA as c_int, ra, JTU as c_int, 0);
            }
            a64_ldapur(b, size, rd, JTA as c_int, 0);
            return;
        }
        if disp == 0 {
            emit_guest_load_ordered(b, size, rd, ra, JTU as c_int);
            return;
        }
        if disp > 0 && disp <= 4095 {
            a64_add_imm(b, 1, JTA as c_int, ra, disp as u32);
        } else if disp < 0 && disp.wrapping_neg() <= 4095 {
            a64_sub_imm(b, 1, JTA as c_int, ra, disp.wrapping_neg() as u32);
        } else {
            a64_mov_imm64(b, JTU as c_int, disp as i64 as u64);
            a64_add_reg(b, 1, JTA as c_int, ra, JTU as c_int, 0);
        }
        emit_guest_load_ordered(b, size, rd, JTA as c_int, JTU as c_int);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_gpr_lds_at(
    b: *mut A64Buf,
    size: c_int,
    sf: c_int,
    rd: c_int,
    ra: c_int,
    disp: i32,
) {
    unsafe {
        if g_no_ldapr != 0 || (g_align_guard != 0 && size != 1) {
            emit_gpr_ld_at(b, size, JT1 as c_int, ra, disp, 0);
            if size == 1 {
                a64_sxtb(b, sf, rd, JT1 as c_int);
            } else if size == 2 {
                a64_sxth(b, sf, rd, JT1 as c_int);
            } else {
                a64_sxtw(b, rd, JT1 as c_int);
            }
            return;
        }
        g_blk_ordered_loads = 1;
        let mut ra = ra;
        let mut disp = disp;
        if !(-256..=255).contains(&disp) {
            if disp > 0 && disp <= 4095 {
                a64_add_imm(b, 1, JTA as c_int, ra, disp as u32);
            } else {
                a64_mov_imm64(b, JTU as c_int, disp as i64 as u64);
                a64_add_reg(b, 1, JTA as c_int, ra, JTU as c_int, 0);
            }
            ra = JTA as c_int;
            disp = 0;
        }
        a64_ldapurs(b, size, sf, rd, ra, disp);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_gpr_st_at(
    b: *mut A64Buf,
    size: c_int,
    rv: c_int,
    ra: c_int,
    disp: i32,
    plain: c_int,
) {
    unsafe {
        let scaled = (disp >= 0 && (disp % size) == 0 && disp / size <= 4095) as c_int;
        if plain != 0 {
            if scaled != 0 {
                a64_str(b, size, rv, ra, disp as u32);
            } else if (-256..=255).contains(&disp) {
                a64_stur(b, size, rv, ra, disp);
            } else {
                a64_mov_imm64(b, JTU as c_int, disp as i64 as u64);
                a64_add_reg(b, 1, JTA as c_int, ra, JTU as c_int, 0);
                a64_str(b, size, rv, JTA as c_int, 0);
            }
            return;
        }
        if g_align_guard == 0 || size == 1 {
            if (-256..=255).contains(&disp) {
                a64_stlur(b, size, rv, ra, disp);
                return;
            }
            if scaled != 0 && disp <= 4095 {
                a64_add_imm(b, 1, JTA as c_int, ra, disp as u32);
            } else {
                a64_mov_imm64(b, JTU as c_int, disp as i64 as u64);
                a64_add_reg(b, 1, JTA as c_int, ra, JTU as c_int, 0);
            }
            a64_stlur(b, size, rv, JTA as c_int, 0);
            return;
        }
        if disp == 0 {
            emit_guest_store_ordered(b, size, rv, ra, JTU as c_int);
            return;
        }
        if disp > 0 && disp <= 4095 {
            a64_add_imm(b, 1, JTA as c_int, ra, disp as u32);
        } else if disp < 0 && disp.wrapping_neg() <= 4095 {
            a64_sub_imm(b, 1, JTA as c_int, ra, disp.wrapping_neg() as u32);
        } else {
            a64_mov_imm64(b, JTU as c_int, disp as i64 as u64);
            a64_add_reg(b, 1, JTA as c_int, ra, JTU as c_int, 0);
        }
        emit_guest_store_ordered(b, size, rv, JTA as c_int, JTU as c_int);
    }
}

unsafe fn emit_gpr_ld_regoff(
    b: *mut A64Buf,
    size: c_int,
    rd: c_int,
    ra: c_int,
    ri: c_int,
    scaled: c_int,
    plain: c_int,
) {
    unsafe {
        if plain != 0 {
            a64_ldr_regoff(b, size, rd, ra, ri, scaled);
            return;
        }
        let sh = if scaled != 0 {
            if size == 8 {
                3
            } else if size == 4 {
                2
            } else if size == 2 {
                1
            } else {
                0
            }
        } else {
            0
        };
        a64_add_reg(b, 1, JTA as c_int, ra, ri, sh);
        emit_gpr_ld_at(b, size, rd, JTA as c_int, 0, 0);
    }
}

unsafe fn emit_gpr_st_regoff(
    b: *mut A64Buf,
    size: c_int,
    rv: c_int,
    ra: c_int,
    ri: c_int,
    scaled: c_int,
    plain: c_int,
) {
    unsafe {
        if plain != 0 {
            a64_str_regoff(b, size, rv, ra, ri, scaled);
            return;
        }
        let sh = if scaled != 0 {
            if size == 8 {
                3
            } else if size == 4 {
                2
            } else if size == 2 {
                1
            } else {
                0
            }
        } else {
            0
        };
        a64_add_reg(b, 1, JTA as c_int, ra, ri, sh);
        emit_gpr_st_at(b, size, rv, JTA as c_int, 0, 0);
    }
}

unsafe fn emit_v_st_ordered_fast(b: *mut A64Buf, size: c_int, vs: c_int, ra: c_int, disp: i32) {
    unsafe {
        if size == 16 {
            a64_fmov_x_from_v(b, 1, JT0 as c_int, vs);
            a64_umov_gpr(b, 8, JTU as c_int, vs, 1);
            a64_stlur(b, 8, JT0 as c_int, ra, disp);
            a64_stlur(b, 8, JTU as c_int, ra, disp + 8);
        } else if size == 8 {
            a64_fmov_x_from_v(b, 1, JT0 as c_int, vs);
            a64_stlur(b, 8, JT0 as c_int, ra, disp);
        } else {
            a64_fmov_x_from_v(b, 0, JT0 as c_int, vs);
            a64_stlur(b, 4, JT0 as c_int, ra, disp);
        }
    }
}

unsafe fn emit_v_acc_ordered_checked(
    b: *mut A64Buf,
    size: c_int,
    vr: c_int,
    ra: c_int,
    disp: i32,
    store: c_int,
) {
    unsafe {
        let mut ra = ra;
        if disp != 0 {
            if disp > 0 && disp <= 4095 {
                a64_add_imm(b, 1, JTA as c_int, ra, disp as u32);
            } else if disp < 0 && disp.wrapping_neg() <= 4095 {
                a64_sub_imm(b, 1, JTA as c_int, ra, disp.wrapping_neg() as u32);
            } else {
                a64_mov_imm64(b, JTU as c_int, disp as i64 as u64);
                a64_add_reg(b, 1, JTA as c_int, ra, JTU as c_int, 0);
            }
            ra = JTA as c_int;
        }
        if size == 16 {
            a64_try_ands_imm(b, 1, A64_ZR as c_int, ra, 7);
        } else {
            emit_granule_cross_test(b, size, ra, JTU as c_int);
        }
        let _ = store;
        if g_no_oolslow == 0 && g_n_oslow < OSLOW_MAX as c_int {
            let bne = a64_label(b);
            a64_bcond(b, A64_NE as c_int, 0);
            emit_v_st_ordered_fast(b, size, vr, ra, 0);
            g_oslow[g_n_oslow as usize] = OrderedSlowPend {
                bne,
                back: a64_label(b),
                size,
                rv: vr,
                ra,
                store: 1,
                idx: g_cur_insn_idx,
                vec: 1,
                disp: 0,
            };
            g_n_oslow += 1;
            ji::ea_cache_reset();
            return;
        }
        let to_aligned = a64_label(b);
        a64_bcond(b, A64_EQ as c_int, 0);
        a64_dmb_ish(b);
        a64_str_v(b, size, vr, ra, 0);
        let to_done = a64_label(b);
        a64_b(b, 0);
        a64_patch_bcond(to_aligned, a64_label(b));
        emit_v_st_ordered_fast(b, size, vr, ra, 0);
        a64_patch_b(to_done, a64_label(b));
    }
}

#[unsafe(no_mangle)]
pub static mut g_undo_saved: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_undo_want_size: c_int = 0;

#[unsafe(no_mangle)]
pub static mut g_undo_want_slot: c_int = -1;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_v_ld_at_(
    b: *mut A64Buf,
    size: c_int,
    vd: c_int,
    ra: c_int,
    disp: i32,
    plain: c_int,
) {
    unsafe {
        let scaled = (disp >= 0 && (disp % size) == 0 && disp / size <= 4095) as c_int;
        let mut plain = plain;
        if plain == 0 && vec_plain_size(size) != 0 {
            plain = 1;
        }
        if plain == 0 {
            g_blk_ordered_loads = 1;
        }
        if plain != 0 {
            if scaled != 0 {
                a64_ldr_v(b, size, vd, ra, disp as u32);
            } else if (-256..=255).contains(&disp) {
                a64_ldur_v(b, size, vd, ra, disp);
            } else {
                a64_mov_imm64(b, JTU as c_int, disp as i64 as u64);
                a64_add_reg(b, 1, JTA as c_int, ra, JTU as c_int, 0);
                a64_ldr_v(b, size, vd, JTA as c_int, 0);
            }
            return;
        }
        if (-256..=255).contains(&disp) {
            if scaled != 0 {
                a64_ldr_v(b, size, vd, ra, disp as u32);
            } else {
                a64_ldur_v(b, size, vd, ra, disp);
            }
            a64_ldapur(b, 1, JTU as c_int, ra, disp);
        } else {
            if disp > 0 && disp <= 4095 {
                a64_add_imm(b, 1, JTA as c_int, ra, disp as u32);
            } else if disp < 0 && disp.wrapping_neg() <= 4095 {
                a64_sub_imm(b, 1, JTA as c_int, ra, disp.wrapping_neg() as u32);
            } else {
                a64_mov_imm64(b, JTU as c_int, disp as i64 as u64);
                a64_add_reg(b, 1, JTA as c_int, ra, JTU as c_int, 0);
            }
            a64_ldr_v(b, size, vd, JTA as c_int, 0);
            a64_ldapur(b, 1, JTU as c_int, JTA as c_int, 0);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_v_st_at(
    b: *mut A64Buf,
    size: c_int,
    vs: c_int,
    ra: c_int,
    disp: i32,
    plain: c_int,
) {
    unsafe {
        let scaled = (disp >= 0 && (disp % size) == 0 && disp / size <= 4095) as c_int;
        let mut plain = plain;
        if plain == 0 && vec_plain_size(size) != 0 {
            plain = 1;
        }
        if plain != 0 {
            if scaled != 0 {
                a64_str_v(b, size, vs, ra, disp as u32);
            } else if (-256..=255).contains(&disp) {
                a64_stur_v(b, size, vs, ra, disp);
            } else {
                a64_mov_imm64(b, JTU as c_int, disp as i64 as u64);
                a64_add_reg(b, 1, JTA as c_int, ra, JTU as c_int, 0);
                a64_str_v(b, size, vs, JTA as c_int, 0);
            }
            return;
        }
        if g_align_guard != 0 {
            emit_v_acc_ordered_checked(b, size, vs, ra, disp, 1);
            return;
        }
        let mut ra = ra;
        let mut disp = disp;
        if !(disp >= -256 && disp + (if size == 16 { 8 } else { 0 }) <= 255) {
            a64_mov_imm64(b, JTU as c_int, disp as i64 as u64);
            a64_add_reg(b, 1, JTA as c_int, ra, JTU as c_int, 0);
            ra = JTA as c_int;
            disp = 0;
        }
        emit_v_st_ordered_fast(b, size, vs, ra, disp);
    }
}

unsafe fn emit_v_ld_regoff(
    b: *mut A64Buf,
    size: c_int,
    vd: c_int,
    ra: c_int,
    ri: c_int,
    scaled: c_int,
    plain: c_int,
) {
    unsafe {
        if plain != 0 || vec_plain_size(size) != 0 {
            a64_ldr_v_regoff(b, size, vd, ra, ri, scaled);
            ji::undo_save_hook(b, size, vd);
            return;
        }
        let sh = if scaled != 0 {
            if size == 16 {
                4
            } else if size == 8 {
                3
            } else {
                2
            }
        } else {
            0
        };
        a64_add_reg(b, 1, JTA as c_int, ra, ri, sh);
        ji::emit_v_ld_at(b, size, vd, JTA as c_int, 0, 0);
    }
}

unsafe fn emit_v_st_regoff(
    b: *mut A64Buf,
    size: c_int,
    vs: c_int,
    ra: c_int,
    ri: c_int,
    scaled: c_int,
    plain: c_int,
) {
    unsafe {
        if plain != 0 || vec_plain_size(size) != 0 {
            a64_str_v_regoff(b, size, vs, ra, ri, scaled);
            return;
        }
        let sh = if scaled != 0 {
            if size == 16 {
                4
            } else if size == 8 {
                3
            } else {
                2
            }
        } else {
            0
        };
        a64_add_reg(b, 1, JTA as c_int, ra, ri, sh);
        emit_v_st_at(b, size, vs, JTA as c_int, 0, 0);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lowstack_disp_ok(
    insn: *const X86Insn,
    m: *const X86Operand,
    size: c_int,
    unscaled_ok: c_int,
) -> c_int {
    unsafe {
        if g_lowstack == 0
            || (*m).base as c_uint != OCERZ_RSP
            || (*m).index != OCERZ_REG_NONE as u8
            || (*m).riprel != 0
        {
            return 0;
        }
        if insn_stack_only(insn) == 0 || env_on!("OCERZ_NO_LOWSTACK_EA") != 0 {
            return 0;
        }
        let d = (*m).disp;
        ((d >= 0 && (d % size as i64) == 0 && d / size as i64 <= 4095)
            || (unscaled_ok != 0 && (-256..=255).contains(&d))) as c_int
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_plain_mem_fast(
    b: *mut A64Buf,
    insn: *const X86Insn,
    m: *const X86Operand,
    size: c_int,
    reg: c_int,
    store: c_int,
    vec: c_int,
) -> c_int {
    unsafe {
        static mut DIS: c_int = -1;
        if DIS < 0 {
            DIS = if libc::getenv(c"OCERZ_NO_PLAINFAST".as_ptr()).is_null() {
                0
            } else {
                1
            };
        }
        if DIS != 0 {
            return 0;
        }
        if low_hoist_covers(insn, m) != 0 && env_on!("OCERZ_NO_HOIST_DISP") == 0 {
            let plain = ji::mem_plain_access_ok(m);
            if vec != 0 {
                if store != 0 {
                    emit_v_st_at(b, size, reg, JMEMBASE as c_int, (*m).disp as i32, plain);
                } else {
                    ji::emit_v_ld_at(b, size, reg, JMEMBASE as c_int, (*m).disp as i32, plain);
                }
            } else if store != 0 {
                emit_gpr_st_at(b, size, reg, JMEMBASE as c_int, (*m).disp as i32, plain);
            } else {
                emit_gpr_ld_at(b, size, reg, JMEMBASE as c_int, (*m).disp as i32, plain);
            }
            return 1;
        }
        if ji::lowstack_disp_ea(b, insn, m, size, 1) != 0 {
            let plain = ji::mem_plain_access_ok(m);
            if vec != 0 {
                if store != 0 {
                    emit_v_st_at(b, size, reg, JTA as c_int, (*m).disp as i32, plain);
                } else {
                    ji::emit_v_ld_at(b, size, reg, JTA as c_int, (*m).disp as i32, plain);
                }
            } else if store != 0 {
                emit_gpr_st_at(b, size, reg, JTA as c_int, (*m).disp as i32, plain);
            } else {
                emit_gpr_ld_at(b, size, reg, JTA as c_int, (*m).disp as i32, plain);
            }
            return 1;
        }
        if ji::mem_fast_forms_ok() == 0 {
            return 0;
        }
        if (*insn).seg != OCERZ_SEG_NONE as u8 || (*insn).addrsize != 8 || (*m).riprel != 0 {
            return 0;
        }
        if (*m).base == OCERZ_REG_NONE as u8 || ji::pin_slot((*m).base as c_uint) < 0 {
            return 0;
        }
        if ji::rsp_is_ptr() != 0 && (*m).index as c_uint == OCERZ_RSP {
            return 0;
        }
        let plain = ji::mem_plain_access_ok(m);
        let mut mview = MaybeUninit::<X86Operand>::uninit();
        let m = mem_hoist_view(m, mview.as_mut_ptr());
        let hb = ji::pin_hreg(ji::pin_slot((*m).base as c_uint));
        let hreg = hoist_reg_for((*m).base as c_uint);
        let hoisted = hreg >= 0;
        macro_rules! acc_at {
            ($ra:expr, $d:expr) => {{
                if vec != 0 {
                    if store != 0 {
                        emit_v_st_at(b, size, reg, $ra, $d as i32, plain);
                    } else {
                        ji::emit_v_ld_at(b, size, reg, $ra, $d as i32, plain);
                    }
                } else if store != 0 {
                    emit_gpr_st_at(b, size, reg, $ra, $d as i32, plain);
                } else {
                    emit_gpr_ld_at(b, size, reg, $ra, $d as i32, plain);
                }
            }};
        }
        macro_rules! acc_regoff {
            ($ra:expr, $ri:expr, $sc:expr) => {{
                if vec != 0 {
                    if store != 0 {
                        emit_v_st_regoff(b, size, reg, $ra, $ri, $sc, plain);
                    } else {
                        emit_v_ld_regoff(b, size, reg, $ra, $ri, $sc, plain);
                    }
                } else if store != 0 {
                    emit_gpr_st_regoff(b, size, reg, $ra, $ri, $sc, plain);
                } else {
                    emit_gpr_ld_regoff(b, size, reg, $ra, $ri, $sc, plain);
                }
            }};
        }
        if ji::rsp_is_ptr() != 0 && (*m).base as c_uint == OCERZ_RSP {
            if (*m).index != OCERZ_REG_NONE as u8 {
                return 0;
            }
            let scaled = ((*m).disp >= 0
                && ((*m).disp % size as i64) == 0
                && (*m).disp / size as i64 <= 4095) as c_int;
            let unscaled = (scaled == 0 && (-256..=255).contains(&(*m).disp)) as c_int;
            if scaled == 0 && unscaled == 0 {
                return 0;
            }
            acc_at!(ji::pin_hreg(ji::pin_slot(OCERZ_RSP)), (*m).disp);
            return 1;
        }
        if (*m).index != OCERZ_REG_NONE as u8 {
            if ji::pin_slot((*m).index as c_uint) < 0 {
                return 0;
            }
            let hi = ji::pin_hreg(ji::pin_slot((*m).index as c_uint));
            let sc = ((*m).scale & 3) as c_int;
            let want = if size == 16 {
                4
            } else if size == 8 {
                3
            } else if size == 4 {
                2
            } else if size == 2 {
                1
            } else {
                0
            };
            if hoisted
                && hreg == JMEMBASE as c_int
                && g_mem_hoist_aux_index >= 0
                && (*m).index as c_int == g_mem_hoist_aux_index
                && sc == g_mem_hoist_aux_scale
                && (*m).disp >= 0
                && ((*m).disp % size as i64) == 0
                && (*m).disp / size as i64 <= 4095
            {
                acc_at!(JMEMAUX as c_int, (*m).disp);
                return 1;
            }
            if ((*m).disp == 0
                || (hoisted
                    && hreg == JMEMBASE as c_int
                    && g_mem_hoist_aux_index < 0
                    && (*m).disp == g_mem_hoist_aux_disp as i64
                    && (*m).disp != 0))
                && (sc == 0 || sc == want)
            {
                let mut ra = JTA as c_int;
                if hoisted {
                    ra = if (*m).disp != 0 {
                        JMEMAUX as c_int
                    } else {
                        hreg
                    };
                } else if ea_cache_reusable(b, m) != 0 {
                    acc_at!(JTA as c_int, 0);
                    return 1;
                } else {
                    if ji::ea_cache_has_base(b, m) == 0 {
                        a64_add_reg(b, 1, JTA as c_int, JGB as c_int, hb, 0);
                    }
                    ji::ea_cache_set_full(b, (*m).base as c_uint, OCERZ_REG_NONE, 0);
                }
                acc_regoff!(ra, hi, (sc != 0) as c_int);
                return 1;
            }
            if (*m).disp == 0 && sc != 0 {
                if !hoisted {
                    if ea_cache_reusable(b, m) != 0 {
                        acc_at!(JTA as c_int, 0);
                        return 1;
                    }
                    if ji::ea_cache_has_base(b, m) != 0 {
                        a64_add_reg(b, 1, JTA as c_int, JTA as c_int, hi, sc);
                        ea_cache_set(b, m);
                        acc_at!(JTA as c_int, 0);
                        return 1;
                    }
                    if plain != 0 {
                        a64_add_reg(b, 1, JTA as c_int, hb, hi, sc);
                        ji::ea_cache_reset();
                        acc_regoff!(JGB as c_int, JTA as c_int, 0);
                        return 1;
                    }
                    a64_add_reg(b, 1, JTA as c_int, JGB as c_int, hb, 0);
                    a64_add_reg(b, 1, JTA as c_int, JTA as c_int, hi, sc);
                    ea_cache_set(b, m);
                    acc_at!(JTA as c_int, 0);
                    return 1;
                }
            }
            let scaled = ((*m).disp >= 0
                && ((*m).disp % size as i64) == 0
                && (*m).disp / size as i64 <= 4095) as c_int;
            let unscaled = (scaled == 0 && (-256..=255).contains(&(*m).disp)) as c_int;
            if scaled == 0 && unscaled == 0 {
                return 0;
            }
            if ea_cache_reusable(b, m) == 0 {
                if hoisted {
                    a64_add_reg(b, 1, JTA as c_int, hreg, hi, sc);
                } else if ji::ea_cache_has_base(b, m) != 0 {
                    a64_add_reg(b, 1, JTA as c_int, JTA as c_int, hi, sc);
                } else {
                    a64_add_reg(b, 1, JTA as c_int, JGB as c_int, hb, 0);
                    a64_add_reg(b, 1, JTA as c_int, JTA as c_int, hi, sc);
                }
            }
            ea_cache_set(b, m);
            acc_at!(JTA as c_int, (*m).disp);
            return 1;
        }
        {
            let scaled = ((*m).disp >= 0
                && ((*m).disp % size as i64) == 0
                && (*m).disp / size as i64 <= 4095) as c_int;
            let unscaled = (scaled == 0 && (-256..=255).contains(&(*m).disp)) as c_int;
            if scaled == 0 && unscaled == 0 {
                return 0;
            }
            let mut ra = JTA as c_int;
            if hoisted {
                ra = hreg;
            } else {
                if ji::ea_cache_has_base(b, m) == 0 {
                    a64_add_reg(b, 1, JTA as c_int, JGB as c_int, hb, 0);
                }
                ji::ea_cache_set_full(b, (*m).base as c_uint, OCERZ_REG_NONE, 0);
            }
            acc_at!(ra, (*m).disp);
            return 1;
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn g_cur_insns_fwd() -> *const X86Insn {
    unsafe { g_cur_insns }
}

#[unsafe(no_mangle)]
pub static mut g_ea_cache: JitState_g_ea_cache = JitState_g_ea_cache {
    valid: 0,
    base: 0,
    index: 0,
    scale: 0,
    seq: 0,
    after: null(),
};

fn a64_word_may_write_x15(w: u32) -> c_int {
    ji::a64_word_may_write_reg(w, 15)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ea_cache_usable(b: *const A64Buf) -> c_int {
    static mut DIS: c_int = -1;
    unsafe {
        if DIS < 0 {
            DIS = if libc::getenv(c"OCERZ_NO_EACACHE".as_ptr()).is_null() {
                0
            } else {
                1
            };
        }
        if DIS != 0 || g_ea_cache.valid == 0 || g_cur_insns.is_null() {
            return 0;
        }
        if g_ea_cache.seq != g_callout_seq {
            return 0;
        }
        if g_ea_cache.after.is_null() || g_ea_cache.after > (*b).p {
            return 0;
        }
        let mut w = g_ea_cache.after;
        while w < (*b).p {
            if a64_word_may_write_x15(*w) != 0 {
                return 0;
            }
            w = w.add(1);
        }
        1
    }
}

unsafe fn ea_cache_reusable(b: *const A64Buf, op: *const X86Operand) -> c_int {
    unsafe {
        if ea_cache_usable(b) == 0 {
            return 0;
        }
        (g_ea_cache.base == (*op).base as c_uint
            && g_ea_cache.index == (*op).index as c_uint
            && g_ea_cache.scale == ((*op).scale & 3) as c_int) as c_int
    }
}

unsafe fn ea_cache_set(b: *const A64Buf, op: *const X86Operand) {
    unsafe {
        ji::ea_cache_set_full(
            b,
            (*op).base as c_uint,
            (*op).index as c_uint,
            ((*op).scale & 3) as c_int,
        );
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_mem_ea_plain_ex(
    b: *mut A64Buf,
    insn: *const X86Insn,
    op: *const X86Operand,
    size: c_int,
    ra_out: *mut c_int,
    disp_out: *mut u32,
    unscaled_ok: c_int,
) -> c_int {
    unsafe {
        if ji::lowstack_disp_ea(b, insn, op, size, unscaled_ok) != 0 {
            *ra_out = JTA as c_int;
            *disp_out = (*op).disp as u32;
            return 1;
        }
        if low_hoist_covers(insn, op) != 0 && env_on!("OCERZ_NO_HOIST_DISP") == 0 {
            let d = (*op).disp;
            if (d >= 0 && (d % size as i64) == 0 && d / size as i64 <= 4095)
                || (unscaled_ok != 0 && (-256..=255).contains(&d))
            {
                *ra_out = JMEMBASE as c_int;
                *disp_out = d as u32;
                return 1;
            }
        }
        if ji::mem_fast_forms_ok() == 0 {
            return 0;
        }
        if (*insn).seg != OCERZ_SEG_NONE as u8 || (*insn).addrsize != 8 {
            return 0;
        }
        if ji::rsp_is_ptr() != 0 && (*op).index as c_uint == OCERZ_RSP {
            return 0;
        }
        if ji::rsp_is_ptr() != 0 && (*op).base as c_uint == OCERZ_RSP {
            if (*op).index != OCERZ_REG_NONE as u8
                || (*op).riprel != 0
                || ji::pin_slot(OCERZ_RSP) < 0
            {
                return 0;
            }
            let sd = (*op).disp;
            if !((sd >= 0 && (sd % size as i64) == 0 && sd / size as i64 <= 4095)
                || (unscaled_ok != 0 && (-256..=255).contains(&sd)))
            {
                return 0;
            }
            *ra_out = ji::pin_hreg(ji::pin_slot(OCERZ_RSP));
            *disp_out = sd as u32;
            return 1;
        }
        if (*op).riprel != 0 {
            let c = ((*op).disp as u64).wrapping_add(ocerz_guest_base);
            static mut NOLIT: c_int = -1;
            if NOLIT < 0 {
                NOLIT = if libc::getenv(c"OCERZ_NO_RIPLIT".as_ptr()).is_null() {
                    0
                } else {
                    1
                };
            }
            if NOLIT == 0
                && g_n_raslit < RASLIT_MAX as c_int
                && (c >> 32) != 0
                && ((c >> 16) & 0xffff) != 0
            {
                let e = &mut g_raslit[g_n_raslit as usize];
                e.site = a64_label(b);
                e.retaddr = c;
                e.kind = 1;
                e.tcr = 0;
                e.rt = JTA as c_int;
                g_n_raslit += 1;
                a64_emit32(b, 0x58000000u32 | JTA);
            } else {
                a64_mov_imm64(b, JTA as c_int, c);
            }
            ji::ea_cache_reset();
            *ra_out = JTA as c_int;
            *disp_out = 0;
            return 1;
        }
        if (*op).base != OCERZ_REG_NONE as u8 && ji::pin_slot((*op).base as c_uint) < 0 {
            return 0;
        }
        if (*op).index != OCERZ_REG_NONE as u8 && ji::pin_slot((*op).index as c_uint) < 0 {
            return 0;
        }
        let mut mview = MaybeUninit::<X86Operand>::uninit();
        let op = mem_hoist_view(op, mview.as_mut_ptr());
        let disp = (*op).disp;
        let fits = ((disp >= 0 && (disp % size as i64) == 0 && disp / size as i64 <= 4095)
            || (unscaled_ok != 0 && (-256..=255).contains(&disp))) as c_int;
        let mut have = 0;
        let hreg = if (*op).base != OCERZ_REG_NONE as u8 {
            hoist_reg_for((*op).base as c_uint)
        } else {
            -1
        };
        if fits != 0
            && ((*op).base != OCERZ_REG_NONE as u8 || (*op).index != OCERZ_REG_NONE as u8)
            && ea_cache_reusable(b, op) != 0
        {
            *ra_out = JTA as c_int;
            *disp_out = disp as u32;
            return 1;
        }
        if hreg == JMEMBASE as c_int
            && g_mem_hoist_aux_index >= 0
            && (*op).index != OCERZ_REG_NONE as u8
            && (*op).index as c_int == g_mem_hoist_aux_index
            && ((*op).scale & 3) as c_int == g_mem_hoist_aux_scale
        {
            if fits != 0 {
                *ra_out = JMEMAUX as c_int;
                *disp_out = disp as u32;
                return 1;
            }
            if disp > 0 && disp <= 4095 {
                a64_add_imm(b, 1, JTA as c_int, JMEMAUX as c_int, disp as u32);
            } else if disp < 0 && disp.wrapping_neg() <= 4095 {
                a64_sub_imm(
                    b,
                    1,
                    JTA as c_int,
                    JMEMAUX as c_int,
                    disp.wrapping_neg() as u32,
                );
            } else {
                a64_mov_imm64(b, JTU as c_int, disp as u64);
                a64_add_reg(b, 1, JTA as c_int, JMEMAUX as c_int, JTU as c_int, 0);
            }
            ji::ea_cache_reset();
            *ra_out = JTA as c_int;
            *disp_out = 0;
            return 1;
        }
        let mut fold_now = false;
        if hreg >= 0 {
            if (*op).index == OCERZ_REG_NONE as u8 {
                if fits != 0 {
                    *ra_out = hreg;
                    *disp_out = disp as u32;
                    return 1;
                }
                if disp > 0 && disp <= 4095 {
                    a64_add_imm(b, 1, JTA as c_int, hreg, disp as u32);
                } else if disp < 0 && disp.wrapping_neg() <= 4095 {
                    a64_sub_imm(b, 1, JTA as c_int, hreg, disp.wrapping_neg() as u32);
                } else {
                    a64_mov_imm64(b, JTU as c_int, disp as u64);
                    a64_add_reg(b, 1, JTA as c_int, hreg, JTU as c_int, 0);
                }
                ji::ea_cache_reset();
                *ra_out = JTA as c_int;
                *disp_out = 0;
                return 1;
            }
            a64_add_reg(
                b,
                1,
                JTA as c_int,
                hreg,
                ji::pin_hreg(ji::pin_slot((*op).index as c_uint)),
                ((*op).scale & 3) as c_int,
            );
            have = 1;
        } else if (*op).base != OCERZ_REG_NONE as u8 {
            if (*op).index != OCERZ_REG_NONE as u8 && ji::ea_cache_has_base(b, op) != 0 {
                a64_add_reg(
                    b,
                    1,
                    JTA as c_int,
                    JTA as c_int,
                    ji::pin_hreg(ji::pin_slot((*op).index as c_uint)),
                    ((*op).scale & 3) as c_int,
                );
                ea_cache_set(b, op);
                if fits != 0 {
                    *ra_out = JTA as c_int;
                    *disp_out = disp as u32;
                    return 1;
                }
                fold_now = true;
            }
            if !fold_now {
                if ji::ea_cache_has_base(b, op) == 0 {
                    a64_add_reg(
                        b,
                        1,
                        JTA as c_int,
                        JGB as c_int,
                        ji::pin_hreg(ji::pin_slot((*op).base as c_uint)),
                        0,
                    );
                }
                ji::ea_cache_set_full(b, (*op).base as c_uint, OCERZ_REG_NONE, 0);
                have = 1;
            }
        }
        if !fold_now {
            if (*op).index != OCERZ_REG_NONE as u8 && !(have != 0 && hreg >= 0) {
                a64_add_reg(
                    b,
                    1,
                    JTA as c_int,
                    if have != 0 {
                        JTA as c_int
                    } else {
                        JGB as c_int
                    },
                    ji::pin_hreg(ji::pin_slot((*op).index as c_uint)),
                    ((*op).scale & 3) as c_int,
                );
                have = 1;
            }
            if have == 0 {
                if fits != 0 {
                    *ra_out = JGB as c_int;
                    *disp_out = disp as u32;
                    return 1;
                }
                a64_mov_imm64(
                    b,
                    JTA as c_int,
                    (disp as u64).wrapping_add(ocerz_guest_base),
                );
                ji::ea_cache_reset();
                *ra_out = JTA as c_int;
                *disp_out = 0;
                return 1;
            }
            ea_cache_set(b, op);
            if fits != 0 {
                *ra_out = JTA as c_int;
                *disp_out = disp as u32;
                return 1;
            }
        }
        if disp > 0 && disp <= 4095 {
            a64_add_imm(b, 1, JTA as c_int, JTA as c_int, disp as u32);
        } else if disp < 0 && disp.wrapping_neg() <= 4095 {
            a64_sub_imm(b, 1, JTA as c_int, JTA as c_int, disp.wrapping_neg() as u32);
        } else {
            a64_mov_imm64(b, JTU as c_int, disp as u64);
            a64_add_reg(b, 1, JTA as c_int, JTA as c_int, JTU as c_int, 0);
        }
        ji::ea_cache_reset();
        *ra_out = JTA as c_int;
        *disp_out = 0;
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_mem_load_plain(
    b: *mut A64Buf,
    insn: *const X86Insn,
    op: *const X86Operand,
    size: c_int,
    rd: c_int,
) -> c_int {
    unsafe {
        let mut ra: c_int = 0;
        let mut disp: u32 = 0;
        let plain = ji::mem_plain_access_ok(op);
        let mut mview = MaybeUninit::<X86Operand>::uninit();
        let op = mem_hoist_view(op, mview.as_mut_ptr());
        let aux_disp_ok = ((*op).disp != 0
            && g_pin_class != 2
            && g_mem_hoist_aux_index < 0
            && (*op).disp == g_mem_hoist_aux_disp as i64
            && (*op).base != OCERZ_REG_NONE as u8
            && hoist_reg_for((*op).base as c_uint) == JMEMBASE as c_int)
            as c_int;
        if ji::mem_fast_forms_ok() != 0
            && (*insn).seg == OCERZ_SEG_NONE as u8
            && (*insn).addrsize == 8
            && (*op).riprel == 0
            && (*op).base != OCERZ_REG_NONE as u8
            && (*op).index != OCERZ_REG_NONE as u8
            && ((*op).disp == 0 || aux_disp_ok != 0)
            && ji::pin_slot((*op).base as c_uint) >= 0
            && ji::pin_slot((*op).index as c_uint) >= 0
            && !(ji::rsp_is_ptr() != 0
                && ((*op).base as c_uint == OCERZ_RSP || (*op).index as c_uint == OCERZ_RSP))
        {
            let sc = ((*op).scale & 3) as c_int;
            let want = if size == 8 {
                3
            } else if size == 4 {
                2
            } else if size == 2 {
                1
            } else {
                0
            };
            if aux_disp_ok != 0 && (sc == 0 || sc == want) {
                emit_gpr_ld_regoff(
                    b,
                    size,
                    rd,
                    JMEMAUX as c_int,
                    ji::pin_hreg(ji::pin_slot((*op).index as c_uint)),
                    (sc != 0) as c_int,
                    plain,
                );
                return 1;
            }
            if aux_disp_ok == 0 {
                if sc == 0 || sc == want {
                    let mut hb = hoist_reg_for((*op).base as c_uint);
                    if hb < 0 {
                        hb = JTA as c_int;
                        if ji::ea_cache_has_base(b, op) == 0 {
                            a64_add_reg(
                                b,
                                1,
                                JTA as c_int,
                                JGB as c_int,
                                ji::pin_hreg(ji::pin_slot((*op).base as c_uint)),
                                0,
                            );
                        }
                        ji::ea_cache_set_full(b, (*op).base as c_uint, OCERZ_REG_NONE, 0);
                    }
                    emit_gpr_ld_regoff(
                        b,
                        size,
                        rd,
                        hb,
                        ji::pin_hreg(ji::pin_slot((*op).index as c_uint)),
                        (sc != 0) as c_int,
                        plain,
                    );
                    return 1;
                }
                static mut NOGEA: c_int = -1;
                if NOGEA < 0 {
                    NOGEA = if libc::getenv(c"OCERZ_NO_GEAFORM".as_ptr()).is_null() {
                        0
                    } else {
                        1
                    };
                }
                if NOGEA == 0
                    && plain != 0
                    && hoist_reg_for((*op).base as c_uint) < 0
                    && ji::ea_cache_has_base(b, op) == 0
                    && ea_cache_reusable(b, op) == 0
                {
                    a64_add_reg(
                        b,
                        1,
                        JTA as c_int,
                        ji::pin_hreg(ji::pin_slot((*op).base as c_uint)),
                        ji::pin_hreg(ji::pin_slot((*op).index as c_uint)),
                        sc,
                    );
                    ji::ea_cache_reset();
                    a64_ldr_regoff(b, size, rd, JGB as c_int, JTA as c_int, 0);
                    return 1;
                }
            }
        }
        if emit_mem_ea_plain_ex(b, insn, op, size, &mut ra, &mut disp, 1) == 0 {
            return 0;
        }
        emit_gpr_ld_at(b, size, rd, ra, disp as i32, plain);
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_mov_mem(
    b: *mut A64Buf,
    insn: *const X86Insn,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    unsafe {
        let d = &(*insn).ops[0];
        let s = &(*insn).ops[1];
        let gbase = ocerz_guest_base;

        if d.kind as c_uint == OCERZ_OPK_MEM && s.kind as c_uint == OCERZ_OPK_IMM {
            let size = d.size as c_int;
            if size != 1 && size != 2 && size != 4 && size != 8 {
                return 0;
            }
            if ji::mem_native_store_ok() == 0 {
                return 0;
            }
            let mut v = ocerz_sext(s.imm, s.size as c_int) as u64;
            if size < 8 {
                v &= (1u64 << (size * 8)) - 1;
            }
            let mut rv = A64_ZR as c_int;
            if v != 0 {
                a64_mov_imm64(b, JT1 as c_int, v);
                rv = JT1 as c_int;
            }
            if emit_hoisted_mem_access(b, insn, d, size, rv, 1) != 0 {
                return 1;
            }
            if emit_plain_mem_fast(b, insn, d, size, rv, 1, 0) != 0 {
                return 1;
            }
            if emit_mem_ea(b, insn, d, JTA as c_int) == 0 {
                return 0;
            }
            let skip = emit_commpage_guard(b, insn, JTA as c_int, exit_sites, n_exits);
            ji::emit_add_const(b, JTA as c_int, gbase.wrapping_sub(ji::ea_fold()));
            emit_guest_store_ordered(b, size, rv, JTA as c_int, JTU as c_int);
            ji::patch_guard_skip(skip, a64_label(b));
            return 1;
        }
        if d.kind as c_uint == OCERZ_OPK_MEM && s.kind as c_uint == OCERZ_OPK_REG {
            if s.high8 != 0 || (s.size != 1 && s.size != 2 && s.size != 4 && s.size != 8) {
                return 0;
            }
            if ji::mem_native_store_ok() == 0 {
                return 0;
            }
            let ss = ji::pin_slot(s.reg as c_uint);
            let rv = if ss >= 0 {
                ji::pin_hreg(ss)
            } else {
                JT1 as c_int
            };
            if ss >= 0 && emit_hoisted_mem_access(b, insn, d, s.size as c_int, rv, 1) != 0 {
                return 1;
            }
            if ss >= 0 && emit_plain_mem_fast(b, insn, d, s.size as c_int, rv, 1, 0) != 0 {
                return 1;
            }
            if emit_mem_ea(b, insn, d, JTA as c_int) == 0 {
                return 0;
            }
            let skip = emit_commpage_guard(b, insn, JTA as c_int, exit_sites, n_exits);
            if ss < 0 {
                ji::emit_gpr_rd(
                    b,
                    if s.size == 8 { 1 } else { 0 },
                    JT1 as c_int,
                    s.reg as c_uint,
                );
            }
            ji::emit_add_const(b, JTA as c_int, gbase.wrapping_sub(ji::ea_fold()));

            emit_guest_store_ordered(b, s.size as c_int, rv, JTA as c_int, JTU as c_int);
            ji::patch_guard_skip(skip, a64_label(b));
            return 1;
        }
        if d.kind as c_uint == OCERZ_OPK_REG
            && s.kind as c_uint == OCERZ_OPK_MEM
            && (d.size == 1 || d.size == 2)
            && d.high8 == 0
        {
            let ds = ji::pin_slot(d.reg as c_uint);
            if ds < 0 || (ji::rsp_is_ptr() != 0 && d.reg as c_uint == OCERZ_RSP) {
                return 0;
            }
            if emit_mem_load_plain(b, insn, s, d.size as c_int, JT1 as c_int) == 0 {
                if emit_mem_ea(b, insn, s, JTA as c_int) == 0 {
                    return 0;
                }
                let skip = emit_commpage_guard(b, insn, JTA as c_int, exit_sites, n_exits);
                ji::emit_add_const(b, JTA as c_int, gbase.wrapping_sub(ji::ea_fold()));
                emit_guest_load_ordered(
                    b,
                    d.size as c_int,
                    JT1 as c_int,
                    JTA as c_int,
                    JTU as c_int,
                );
                ji::patch_guard_skip(skip, a64_label(b));
            }
            a64_bfi(b, 1, ji::pin_hreg(ds), JT1 as c_int, 0, d.size as c_int * 8);
            return 1;
        }
        if d.kind as c_uint == OCERZ_OPK_REG && s.kind as c_uint == OCERZ_OPK_MEM {
            if d.high8 != 0 || (d.size != 4 && d.size != 8) {
                return 0;
            }
            let ds = ji::pin_slot(d.reg as c_uint);
            let rd = if ds >= 0 {
                ji::pin_hreg(ds)
            } else {
                JT1 as c_int
            };
            if emit_hoisted_mem_access(b, insn, s, d.size as c_int, rd, 0) != 0 {
                return 1;
            }
            if ds >= 0 && emit_plain_mem_fast(b, insn, s, d.size as c_int, rd, 0, 0) != 0 {
                return 1;
            }
            if emit_mem_ea(b, insn, s, JTA as c_int) == 0 {
                return 0;
            }
            let skip = emit_commpage_guard(b, insn, JTA as c_int, exit_sites, n_exits);
            ji::emit_add_const(b, JTA as c_int, gbase.wrapping_sub(ji::ea_fold()));

            emit_guest_load_ordered(b, d.size as c_int, rd, JTA as c_int, JTU as c_int);
            if ds < 0 {
                ji::emit_gpr_wr(b, JT1 as c_int, d.reg as c_uint);
            }
            ji::patch_guard_skip(skip, a64_label(b));
            return 1;
        }
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_movx(
    b: *mut A64Buf,
    insn: *const X86Insn,
    is_signed: c_int,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    unsafe {
        static mut NO_SUB: c_int = -1;
        if NO_SUB < 0 {
            NO_SUB = if libc::getenv(c"OCERZ_NO_INLINE_SUBWORD".as_ptr()).is_null() {
                0
            } else {
                1
            };
        }
        if NO_SUB != 0 {
            return 0;
        }
        let d = &(*insn).ops[0];
        let s = &(*insn).ops[1];
        let gbase = ocerz_guest_base;
        if d.kind as c_uint != OCERZ_OPK_REG || d.high8 != 0 || (d.size != 4 && d.size != 8) {
            return 0;
        }
        if s.size != 1 && s.size != 2 {
            return 0;
        }
        let sf = (d.size == 8) as c_int;
        if s.kind as c_uint == OCERZ_OPK_REG {
            if s.high8 != 0 {
                return 0;
            }
            if ji::pin_slot(d.reg as c_uint) >= 0
                && ji::pin_slot(s.reg as c_uint) >= 0
                && !(ji::rsp_is_ptr() != 0
                    && (d.reg as c_uint == OCERZ_RSP || s.reg as c_uint == OCERZ_RSP))
            {
                let rd = ji::pin_hreg(ji::pin_slot(d.reg as c_uint));
                let rs = ji::pin_hreg(ji::pin_slot(s.reg as c_uint));
                if is_signed != 0 {
                    if s.size == 1 {
                        a64_sxtb(b, sf, rd, rs);
                    } else {
                        a64_sxth(b, sf, rd, rs);
                    }
                } else if s.size == 1 {
                    a64_uxtb(b, rd, rs);
                } else {
                    a64_uxth(b, rd, rs);
                }
                return 1;
            }
            ji::emit_gpr_rd(b, 1, JT1 as c_int, s.reg as c_uint);
            if is_signed != 0 {
                if s.size == 1 {
                    a64_sxtb(b, sf, JT1 as c_int, JT1 as c_int);
                } else {
                    a64_sxth(b, sf, JT1 as c_int, JT1 as c_int);
                }
            } else if s.size == 1 {
                a64_uxtb(b, JT1 as c_int, JT1 as c_int);
            } else {
                a64_uxth(b, JT1 as c_int, JT1 as c_int);
            }
            ji::emit_gpr_wr(b, JT1 as c_int, d.reg as c_uint);
            return 1;
        }
        if s.kind as c_uint == OCERZ_OPK_MEM {
            let mut ds = ji::pin_slot(d.reg as c_uint);
            if ji::rsp_is_ptr() != 0 && d.reg as c_uint == OCERZ_RSP {
                ds = -1;
            }
            if ds >= 0 {
                let mut ra: c_int = 0;
                let mut disp: u32 = 0;
                if is_signed == 0
                    && emit_mem_load_plain(b, insn, s, s.size as c_int, ji::pin_hreg(ds)) != 0
                {
                    return 1;
                }
                if is_signed != 0
                    && ji::emit_mem_ea_plain(b, insn, s, s.size as c_int, &mut ra, &mut disp) != 0
                {
                    if ji::mem_plain_access_ok(s) != 0 {
                        if s.size == 1 {
                            a64_ldrsb(b, sf, ji::pin_hreg(ds), ra, disp);
                        } else {
                            a64_ldrsh(b, sf, ji::pin_hreg(ds), ra, disp);
                        }
                    } else {
                        emit_gpr_lds_at(b, s.size as c_int, sf, ji::pin_hreg(ds), ra, disp as i32);
                    }
                    return 1;
                }
            }
            if emit_mem_ea(b, insn, s, JTA as c_int) == 0 {
                return 0;
            }
            let skip = emit_commpage_guard(b, insn, JTA as c_int, exit_sites, n_exits);
            ji::emit_add_const(b, JTA as c_int, gbase.wrapping_sub(ji::ea_fold()));
            emit_guest_load_ordered(b, s.size as c_int, JT1 as c_int, JTA as c_int, JTU as c_int);
            if is_signed != 0 {
                if s.size == 1 {
                    a64_sxtb(b, sf, JT1 as c_int, JT1 as c_int);
                } else {
                    a64_sxth(b, sf, JT1 as c_int, JT1 as c_int);
                }
            }
            ji::emit_gpr_wr(b, JT1 as c_int, d.reg as c_uint);
            ji::patch_guard_skip(skip, a64_label(b));
            return 1;
        }
        0
    }
}

#[unsafe(no_mangle)]
pub static mut g_fpbmap: [JitBlock_JitOslowMap; JIT_MAX_BLOCK_INSNS as usize] =
    [JitBlock_JitOslowMap {
        lo: 0,
        hi: 0,
        idx: 0,
    }; JIT_MAX_BLOCK_INSNS as usize];

#[unsafe(no_mangle)]
pub static mut g_n_fpbmap: c_int = 0;

unsafe fn rmw_src_to(
    b: *mut A64Buf,
    s: *const X86Operand,
    size: c_int,
    into: c_int,
    out: *mut c_int,
) -> c_int {
    unsafe {
        if (*s).kind as c_uint == OCERZ_OPK_IMM {
            let mut v = ocerz_sext((*s).imm, (*s).size as c_int) as u64;
            if size == 4 {
                v &= 0xffffffff;
            } else if size == 2 {
                v &= 0xffff;
            } else if size == 1 {
                v &= 0xff;
            }
            a64_mov_imm64(b, into, v);
            *out = into;
            return 1;
        }
        if (*s).kind as c_uint != OCERZ_OPK_REG || (*s).high8 != 0 {
            return 0;
        }
        let ss = ji::pin_slot((*s).reg as c_uint);
        if ss < 0 || (ji::rsp_is_ptr() != 0 && (*s).reg as c_uint == OCERZ_RSP) {
            return 0;
        }
        let r = ji::pin_hreg(ss);
        if size == 1 {
            a64_uxtb(b, into, r);
            *out = into;
        } else if size == 2 {
            a64_uxth(b, into, r);
            *out = into;
        } else {
            *out = r;
        }
        1
    }
}

unsafe fn rmw_write_reg(b: *mut A64Buf, d: *const X86Operand, size: c_int, val: c_int) {
    unsafe {
        let rd = ji::pin_hreg(ji::pin_slot((*d).reg as c_uint));
        if size == 8 {
            a64_mov_reg(b, 1, rd, val);
        } else if size == 4 {
            a64_mov_reg(b, 0, rd, val);
        } else {
            a64_bfi(b, 1, rd, val, 0, size * 8);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_rmw_mem(
    b: *mut A64Buf,
    insn: *const X86Insn,
    need: u64,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    unsafe {
        static mut DIS: c_int = -1;
        if DIS < 0 {
            DIS = if libc::getenv(c"OCERZ_NO_INLINE_RMW".as_ptr()).is_null() {
                0
            } else {
                1
            };
        }
        if DIS != 0
            || g_defer == 0
            || ((*insn).addrsize != 8 && !((*insn).addrsize == 4 && (*insn).mode32 != 0))
        {
            return 0;
        }
        if (*insn).seg != OCERZ_SEG_NONE as u8
            && (*insn).seg != OCERZ_SEG_GS as u8
            && (*insn).seg != OCERZ_SEG_FS as u8
        {
            return 0;
        }
        let op = (*insn).op as c_uint;
        let m: *const X86Operand;
        let mut s: *const X86Operand = null();
        let mut r: *const X86Operand = null();
        if op == OCERZ_OP_XCHG {
            if (*insn).nops != 2 {
                return 0;
            }
            if (*insn).ops[0].kind as c_uint == OCERZ_OPK_MEM {
                m = &(*insn).ops[0];
                r = &(*insn).ops[1];
            } else if (*insn).ops[1].kind as c_uint == OCERZ_OPK_MEM {
                m = &(*insn).ops[1];
                r = &(*insn).ops[0];
            } else {
                return 0;
            }
            if (*r).kind as c_uint != OCERZ_OPK_REG
                || (*r).high8 != 0
                || ji::pin_slot((*r).reg as c_uint) < 0
                || (*r).size != (*m).size
            {
                return 0;
            }
            s = r;
        } else {
            if (*insn).nops < 1 || (*insn).ops[0].kind as c_uint != OCERZ_OPK_MEM {
                return 0;
            }
            m = &(*insn).ops[0];
            if (*insn).nops == 2 {
                s = &(*insn).ops[1];
                if (*s).size != (*m).size {
                    return 0;
                }
                if op == OCERZ_OP_XADD {
                    r = s;
                    if (*r).kind as c_uint != OCERZ_OPK_REG
                        || (*r).high8 != 0
                        || ji::pin_slot((*r).reg as c_uint) < 0
                    {
                        return 0;
                    }
                }
            } else if op != OCERZ_OP_INC
                && op != OCERZ_OP_DEC
                && op != OCERZ_OP_NEG
                && op != OCERZ_OP_NOT
            {
                return 0;
            }
        }
        let size = (*m).size as c_int;
        if size != 1 && size != 2 && size != 4 && size != 8 {
            return 0;
        }
        if ji::rsp_is_ptr() != 0
            && ((*m).index as c_uint == OCERZ_RSP
                || (!r.is_null() && (*r).reg as c_uint == OCERZ_RSP)
                || (!s.is_null()
                    && (*s).kind as c_uint == OCERZ_OPK_REG
                    && (*s).reg as c_uint == OCERZ_RSP))
        {
            return 0;
        }
        if op == OCERZ_OP_CMPXCHG
            && (ji::pin_slot(OCERZ_RAX) < 0
                || s.is_null()
                || (*s).kind as c_uint != OCERZ_OPK_REG
                || (*s).high8 != 0
                || ji::pin_slot((*s).reg as c_uint) < 0)
        {
            return 0;
        }
        if ji::mem_native_store_ok() == 0 {
            return 0;
        }
        let atomic = (op == OCERZ_OP_XCHG
            || op == OCERZ_OP_XADD
            || op == OCERZ_OP_CMPXCHG
            || (*insn).lock != 0) as c_int;
        let is_cmp = (op == OCERZ_OP_CMP || op == OCERZ_OP_TEST) as c_int;
        if g_plain_mem == 0 && atomic != 0 && op == OCERZ_OP_NEG {
            return 0;
        }
        let sf = (size == 8) as c_int;
        let ordered = (g_plain_mem == 0) as c_int;

        let incdec_cf = (op == OCERZ_OP_INC || op == OCERZ_OP_DEC) && need != 0;
        if incdec_cf {
            ji::emit_cc_predicate(b, OCERZ_CC_B);
            a64_cset(b, JT1 as c_int, A64_NE as c_int);
            a64_str(
                b,
                8,
                JT1 as c_int,
                20,
                offset_of!(OcerzCPU, jit_scratch) as u32,
            );
        }
        let mut ra: c_int = 0;
        let mut disp: u32 = 0;
        let plainacc = ji::mem_plain_access_ok(m);
        if (*insn).seg != OCERZ_SEG_NONE as u8
            || ji::emit_mem_ea_plain(b, insn, m, size, &mut ra, &mut disp) == 0
        {
            if emit_mem_ea(b, insn, m, JTA as c_int) == 0 {
                return 0;
            }
            emit_commpage_guard(b, insn, JTA as c_int, exit_sites, n_exits);
            ji::emit_add_const(
                b,
                JTA as c_int,
                ocerz_guest_base.wrapping_sub(ji::ea_fold()),
            );
            ra = JTA as c_int;
            disp = 0;
        } else if (plainacc == 0 || (ordered != 0 && atomic != 0)) && disp != 0 {
            if disp <= 4095 {
                a64_add_imm(b, 1, JTA as c_int, ra, disp);
            } else {
                a64_mov_imm64(b, JTU as c_int, disp as u64);
                a64_add_reg(b, 1, JTA as c_int, ra, JTU as c_int, 0);
            }
            ra = JTA as c_int;
            disp = 0;
        }
        let mut rs = -1;
        if !s.is_null() && op != OCERZ_OP_CMPXCHG {
            if rmw_src_to(b, s, size, JT1 as c_int, &mut rs) == 0 {
                return 0;
            }
        }
        if op == OCERZ_OP_CMPXCHG {
            rs = ji::pin_hreg(ji::pin_slot((*s).reg as c_uint));
        }
        if op == OCERZ_OP_CMPXCHG && size < 4 {
            if size == 1 {
                a64_uxtb(b, JT1 as c_int, rs);
            } else {
                a64_uxth(b, JT1 as c_int, rs);
            }
            rs = JT1 as c_int;
        }
        let hax = if ji::pin_slot(OCERZ_RAX) >= 0 {
            ji::pin_hreg(ji::pin_slot(OCERZ_RAX))
        } else {
            -1
        };

        let mut align_bne: *mut u32 = null_mut();
        if ordered != 0 && atomic != 0 {
            if size > 1 {
                a64_try_ands_imm(b, 1, A64_ZR as c_int, ra, (size - 1) as u64);
                align_bne = a64_label(b);
                a64_bcond(b, A64_NE as c_int, 0);
            }
            match op {
                OCERZ_OP_ADD | OCERZ_OP_XADD => {
                    a64_ldop_al(b, size, 0, rs, JT0 as c_int, ra);
                }
                OCERZ_OP_SUB => {
                    a64_neg_reg(b, sf, JT2 as c_int, rs);
                    if size == 1 {
                        a64_uxtb(b, JT2 as c_int, JT2 as c_int);
                    } else if size == 2 {
                        a64_uxth(b, JT2 as c_int, JT2 as c_int);
                    }
                    a64_ldop_al(b, size, 0, JT2 as c_int, JT0 as c_int, ra);
                }
                OCERZ_OP_OR => {
                    a64_ldop_al(b, size, 3, rs, JT0 as c_int, ra);
                }
                OCERZ_OP_XOR => {
                    a64_ldop_al(b, size, 2, rs, JT0 as c_int, ra);
                }
                OCERZ_OP_AND => {
                    a64_mvn_reg(b, sf, JT2 as c_int, rs);
                    a64_ldop_al(b, size, 1, JT2 as c_int, JT0 as c_int, ra);
                }
                OCERZ_OP_INC => {
                    a64_mov_imm64(b, JT2 as c_int, 1);
                    a64_ldop_al(b, size, 0, JT2 as c_int, JT0 as c_int, ra);
                }
                OCERZ_OP_DEC => {
                    a64_mov_imm64(
                        b,
                        JT2 as c_int,
                        if size == 8 {
                            !0u64
                        } else if size == 4 {
                            0xffffffff
                        } else if size == 2 {
                            0xffff
                        } else {
                            0xff
                        },
                    );
                    a64_ldop_al(b, size, 0, JT2 as c_int, JT0 as c_int, ra);
                }
                OCERZ_OP_NOT => {
                    a64_mov_imm64(b, JT2 as c_int, !0u64);
                    a64_ldop_al(b, size, 2, JT2 as c_int, JT0 as c_int, ra);
                }
                OCERZ_OP_XCHG => {
                    a64_swpal(b, size, rs, JT0 as c_int, ra);
                }
                OCERZ_OP_CMPXCHG => {
                    if size == 8 {
                        a64_mov_reg(b, 1, JT0 as c_int, hax);
                    } else if size == 4 {
                        a64_mov_reg(b, 0, JT0 as c_int, hax);
                    } else if size == 2 {
                        a64_uxth(b, JT0 as c_int, hax);
                    } else {
                        a64_uxtb(b, JT0 as c_int, hax);
                    }
                    a64_casal(b, size, JT0 as c_int, rs, ra);
                }
                _ => return 0,
            }
        } else {
            emit_gpr_ld_at(b, size, JT0 as c_int, ra, disp as i32, plainacc);
        }

        let have_new;
        match op {
            OCERZ_OP_ADD | OCERZ_OP_XADD => {
                a64_add_reg(b, sf, JT2 as c_int, JT0 as c_int, rs, 0);
                have_new = 1;
            }
            OCERZ_OP_SUB => {
                a64_sub_reg(b, sf, JT2 as c_int, JT0 as c_int, rs, 0);
                have_new = 1;
            }
            OCERZ_OP_AND => {
                a64_and_reg(b, sf, JT2 as c_int, JT0 as c_int, rs, 0);
                have_new = 1;
            }
            OCERZ_OP_TEST => {
                if need != 0 {
                    a64_and_reg(b, sf, JT2 as c_int, JT0 as c_int, rs, 0);
                }
                have_new = 0;
            }
            OCERZ_OP_OR => {
                a64_orr_reg(b, sf, JT2 as c_int, JT0 as c_int, rs, 0);
                have_new = 1;
            }
            OCERZ_OP_XOR => {
                a64_eor_reg(b, sf, JT2 as c_int, JT0 as c_int, rs, 0);
                have_new = 1;
            }
            OCERZ_OP_INC => {
                a64_add_imm(b, sf, JT2 as c_int, JT0 as c_int, 1);
                have_new = 1;
            }
            OCERZ_OP_DEC => {
                a64_sub_imm(b, sf, JT2 as c_int, JT0 as c_int, 1);
                have_new = 1;
            }
            OCERZ_OP_NEG => {
                a64_neg_reg(b, sf, JT2 as c_int, JT0 as c_int);
                have_new = 1;
            }
            OCERZ_OP_NOT => {
                a64_mvn_reg(b, sf, JT2 as c_int, JT0 as c_int);
                have_new = 1;
            }
            OCERZ_OP_XCHG => {
                a64_mov_reg(b, 1, JT2 as c_int, rs);
                have_new = 1;
            }
            OCERZ_OP_CMPXCHG => {
                let mut acc = JTU as c_int;
                if size == 8 {
                    acc = hax;
                } else if size == 4 {
                    a64_mov_reg(b, 0, JTU as c_int, hax);
                } else if size == 2 {
                    a64_uxth(b, JTU as c_int, hax);
                } else {
                    a64_uxtb(b, JTU as c_int, hax);
                }
                if size == 8 {
                    a64_subs_reg(b, 1, A64_ZR as c_int, JT0 as c_int, hax, 0);
                } else {
                    a64_subs_reg(b, 0, A64_ZR as c_int, JT0 as c_int, acc, 0);
                }
                a64_csel(b, 1, JT2 as c_int, rs, JT0 as c_int, A64_EQ as c_int);
                if need != 0 {
                    ji::emit_defer_flags(
                        b,
                        crate::inline::ocerz_cc_pack(OCERZ_CC_SUB, size, 0),
                        JT0 as c_int,
                        acc,
                    );
                }
                if size == 8 {
                    a64_csel(b, 1, hax, hax, JT0 as c_int, A64_EQ as c_int);
                } else if size == 4 {
                    a64_csel(b, 1, hax, acc, JT0 as c_int, A64_EQ as c_int);
                } else {
                    a64_csel(b, 1, JT1 as c_int, acc, JT0 as c_int, A64_EQ as c_int);
                    a64_bfi(b, 1, hax, JT1 as c_int, 0, size * 8);
                }
                have_new = 1;
            }
            OCERZ_OP_CMP => {
                have_new = 0;
            }
            _ => return 0,
        }
        if op != OCERZ_OP_CMP && (op != OCERZ_OP_TEST || need != 0) {
            if size == 1 {
                a64_uxtb(b, JT2 as c_int, JT2 as c_int);
            } else if size == 2 {
                a64_uxth(b, JT2 as c_int, JT2 as c_int);
            } else if size == 4
                && (op == OCERZ_OP_NEG
                    || op == OCERZ_OP_NOT
                    || op == OCERZ_OP_SUB
                    || op == OCERZ_OP_ADD
                    || op == OCERZ_OP_XADD
                    || op == OCERZ_OP_INC
                    || op == OCERZ_OP_DEC)
            {
                a64_mov_reg(b, 0, JT2 as c_int, JT2 as c_int);
            }
        }

        if is_cmp == 0 && !(ordered != 0 && atomic != 0) {
            emit_gpr_st_at(b, size, JT2 as c_int, ra, disp as i32, plainacc);
        }
        if need != 0 && op != OCERZ_OP_CMPXCHG {
            match op {
                OCERZ_OP_ADD | OCERZ_OP_XADD => {
                    if rs == JT1 as c_int {
                        ji::emit_defer_flags(
                            b,
                            crate::inline::ocerz_cc_pack(OCERZ_CC_ADD, size, 0),
                            JT0 as c_int,
                            JT1 as c_int,
                        );
                    } else {
                        if size == 8 {
                            ji::emit_defer_flags(
                                b,
                                crate::inline::ocerz_cc_pack(OCERZ_CC_ADD, size, 0),
                                JT0 as c_int,
                                rs,
                            );
                        } else {
                            a64_mov_reg(b, 0, JT1 as c_int, rs);
                            ji::emit_defer_flags(
                                b,
                                crate::inline::ocerz_cc_pack(OCERZ_CC_ADD, size, 0),
                                JT0 as c_int,
                                JT1 as c_int,
                            );
                        }
                    }
                }
                OCERZ_OP_SUB | OCERZ_OP_CMP => {
                    if rs == JT1 as c_int {
                        ji::emit_defer_flags(
                            b,
                            crate::inline::ocerz_cc_pack(OCERZ_CC_SUB, size, 0),
                            JT0 as c_int,
                            JT1 as c_int,
                        );
                    } else {
                        if size == 8 {
                            ji::emit_defer_flags(
                                b,
                                crate::inline::ocerz_cc_pack(OCERZ_CC_SUB, size, 0),
                                JT0 as c_int,
                                rs,
                            );
                        } else {
                            a64_mov_reg(b, 0, JT1 as c_int, rs);
                            ji::emit_defer_flags(
                                b,
                                crate::inline::ocerz_cc_pack(OCERZ_CC_SUB, size, 0),
                                JT0 as c_int,
                                JT1 as c_int,
                            );
                        }
                    }
                }
                OCERZ_OP_AND | OCERZ_OP_OR | OCERZ_OP_XOR | OCERZ_OP_TEST => {
                    ji::emit_defer_flags(
                        b,
                        crate::inline::ocerz_cc_pack(OCERZ_CC_LOGIC, size, 0),
                        JT2 as c_int,
                        JT2 as c_int,
                    );
                }
                OCERZ_OP_INC | OCERZ_OP_DEC => {
                    a64_ldr(
                        b,
                        8,
                        JT1 as c_int,
                        20,
                        offset_of!(OcerzCPU, jit_scratch) as u32,
                    );
                    ji::emit_defer_flags(
                        b,
                        crate::inline::ocerz_cc_pack(
                            if op == OCERZ_OP_INC {
                                OCERZ_CC_INC
                            } else {
                                OCERZ_CC_DEC
                            },
                            size,
                            0,
                        ),
                        JT1 as c_int,
                        JT2 as c_int,
                    );
                }
                OCERZ_OP_NEG => {
                    a64_mov_imm64(b, JT1 as c_int, 0);
                    ji::emit_defer_flags(
                        b,
                        crate::inline::ocerz_cc_pack(OCERZ_CC_SUB, size, 0),
                        JT1 as c_int,
                        JT0 as c_int,
                    );
                }
                _ => {}
            }
        }
        if op == OCERZ_OP_XCHG || op == OCERZ_OP_XADD {
            rmw_write_reg(b, r, size, JT0 as c_int);
        }
        if g_nzcv_want != 0 && is_cmp != 0 && size >= 4 {
            if op == OCERZ_OP_CMP {
                a64_subs_reg(b, sf, A64_ZR as c_int, JT0 as c_int, rs, 0);
            } else {
                a64_ands_reg(b, sf, A64_ZR as c_int, JT0 as c_int, rs, 0);
            }
            g_nzcv_kind = if op == OCERZ_OP_CMP {
                OCERZ_CC_SUB
            } else {
                OCERZ_CC_LOGIC
            };
            g_nzcv_from = g_cur_insn_idx;
        }
        let _ = have_new;
        if !align_bne.is_null() {
            let mut sites = [align_bne];
            if oolslow_add(insn, sites.as_mut_ptr(), 1, a64_label(b)) == 0 {
                let done = a64_label(b);
                a64_b(b, 0);
                ji::patch_any_branch(align_bne, a64_label(b));
                emit_slowcall(b, insn, exit_sites, n_exits);
                a64_patch_b(done, a64_label(b));
            }
        }
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_xchg_reg32(b: *mut A64Buf, insn: *const X86Insn) -> c_int {
    unsafe {
        let x = &(*insn).ops[0];
        let y = &(*insn).ops[1];
        let size = x.size as c_int;
        if (*insn).nops != 2
            || y.kind as c_uint != OCERZ_OPK_REG
            || y.size as c_int != size
            || ji::pin_slot(x.reg as c_uint) < 0
            || ji::pin_slot(y.reg as c_uint) < 0
        {
            return 0;
        }
        let rx = ji::pin_hreg(ji::pin_slot(x.reg as c_uint));
        let ry = ji::pin_hreg(ji::pin_slot(y.reg as c_uint));
        if size == 4 {
            if rx == ry {
                a64_mov_reg(b, 0, rx, rx);
                return 1;
            }
            a64_mov_reg(b, 0, JT0 as c_int, rx);
            a64_mov_reg(b, 0, rx, ry);
            a64_mov_reg(b, 0, ry, JT0 as c_int);
            return 1;
        }
        if size != 1 && size != 2 {
            return 0;
        }
        let lx = if x.high8 != 0 { 8 } else { 0 };
        let ly = if y.high8 != 0 { 8 } else { 0 };
        let w = size * 8;
        a64_ubfx(b, 1, JT0 as c_int, rx, lx, w);
        a64_ubfx(b, 1, JT1 as c_int, ry, ly, w);
        a64_bfi(b, 1, rx, JT1 as c_int, lx, w);
        a64_bfi(b, 1, ry, JT0 as c_int, ly, w);
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_cmpxchg8b(
    b: *mut A64Buf,
    insn: *const X86Insn,
    exit_sites: *mut *mut u32,
    n_exits: *mut c_int,
) -> c_int {
    unsafe {
        let m = &(*insn).ops[0];
        if (*insn).mode32 == 0
            || (*insn).opsize != 8
            || (*insn).nops != 1
            || m.kind as c_uint != OCERZ_OPK_MEM
            || (*insn).addrsize != 4
        {
            return 0;
        }
        if g_defer == 0 || ji::mem_native_store_ok() == 0 {
            return 0;
        }
        if ji::pin_slot(OCERZ_RAX) < 0
            || ji::pin_slot(OCERZ_RDX) < 0
            || ji::pin_slot(OCERZ_RBX) < 0
            || ji::pin_slot(OCERZ_RCX) < 0
        {
            return 0;
        }
        let hax = ji::pin_hreg(ji::pin_slot(OCERZ_RAX));
        let hdx = ji::pin_hreg(ji::pin_slot(OCERZ_RDX));
        let hbx = ji::pin_hreg(ji::pin_slot(OCERZ_RBX));
        let hcx = ji::pin_hreg(ji::pin_slot(OCERZ_RCX));
        emit_materialize(b);
        if emit_mem_ea(b, insn, m, JTA as c_int) == 0 {
            return 0;
        }
        emit_commpage_guard(b, insn, JTA as c_int, exit_sites, n_exits);
        ji::emit_add_const(
            b,
            JTA as c_int,
            ocerz_guest_base.wrapping_sub(ji::ea_fold()),
        );
        a64_mov_reg(b, 0, JT2 as c_int, hax);
        a64_bfi(b, 1, JT2 as c_int, hdx, 32, 32);
        a64_mov_reg(b, 0, JT1 as c_int, hbx);
        a64_bfi(b, 1, JT1 as c_int, hcx, 32, 32);
        let mut align_bne: *mut u32 = null_mut();
        if g_plain_mem == 0 {
            a64_try_ands_imm(b, 1, A64_ZR as c_int, JTA as c_int, 7);
            align_bne = a64_label(b);
            a64_bcond(b, A64_NE as c_int, 0);
            a64_mov_reg(b, 1, JT0 as c_int, JT2 as c_int);
            a64_casal(b, 8, JT0 as c_int, JT1 as c_int, JTA as c_int);
        } else {
            let plainacc = ji::mem_plain_access_ok(m);
            emit_gpr_ld_at(b, 8, JT0 as c_int, JTA as c_int, 0, plainacc);
            a64_subs_reg(b, 1, A64_ZR as c_int, JT0 as c_int, JT2 as c_int, 0);
            a64_csel(
                b,
                1,
                JT1 as c_int,
                JT1 as c_int,
                JT0 as c_int,
                A64_EQ as c_int,
            );
            emit_gpr_st_at(b, 8, JT1 as c_int, JTA as c_int, 0, plainacc);
        }
        a64_subs_reg(b, 1, A64_ZR as c_int, JT0 as c_int, JT2 as c_int, 0);
        a64_mov_reg(b, 0, JTT as c_int, JT0 as c_int);
        a64_csel(b, 1, hax, hax, JTT as c_int, A64_EQ as c_int);
        a64_lsr_imm(b, 1, JTT as c_int, JT0 as c_int, 32);
        a64_csel(b, 1, hdx, hdx, JTT as c_int, A64_EQ as c_int);
        a64_cset(b, JTT as c_int, A64_EQ as c_int);
        a64_ldr(b, 8, JTU as c_int, 20, offset_of!(OcerzCPU, rflags) as u32);
        a64_bfi(b, 1, JTU as c_int, JTT as c_int, 6, 1);
        a64_str(b, 8, JTU as c_int, 20, offset_of!(OcerzCPU, rflags) as u32);
        if !align_bne.is_null() {
            let mut sites = [align_bne];
            if oolslow_add(insn, sites.as_mut_ptr(), 1, a64_label(b)) == 0 {
                let done = a64_label(b);
                a64_b(b, 0);
                ji::patch_any_branch(align_bne, a64_label(b));
                emit_slowcall(b, insn, exit_sites, n_exits);
                a64_patch_b(done, a64_label(b));
            }
        }
        1
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn insn_may_write_gpr(in_: *const X86Insn, reg: c_uint) -> c_int {
    unsafe {
        let reg = reg & 15;
        match (*in_).op as c_uint {
            OCERZ_OP_CMP | OCERZ_OP_TEST | OCERZ_OP_BT | OCERZ_OP_JMP | OCERZ_OP_JCC
            | OCERZ_OP_JRCXZ | OCERZ_OP_NOP | OCERZ_OP_PREFETCH | OCERZ_OP_CLFLUSH
            | OCERZ_OP_UCOMISS | OCERZ_OP_UCOMISD | OCERZ_OP_COMISS | OCERZ_OP_COMISD
            | OCERZ_OP_PTEST => {
                return 0;
            }
            OCERZ_OP_PUSH | OCERZ_OP_POP | OCERZ_OP_CALL | OCERZ_OP_RET | OCERZ_OP_PUSHF
            | OCERZ_OP_POPF | OCERZ_OP_LEAVE => {
                if reg == OCERZ_RSP || reg == OCERZ_RBP {
                    return 1;
                }
            }
            OCERZ_OP_DIV | OCERZ_OP_IDIV | OCERZ_OP_MUL | OCERZ_OP_CBW | OCERZ_OP_CWD => {
                if reg == OCERZ_RAX || reg == OCERZ_RDX {
                    return 1;
                }
            }
            OCERZ_OP_IMUL => {
                if (*in_).nops == 1 && (reg == OCERZ_RAX || reg == OCERZ_RDX) {
                    return 1;
                }
            }
            OCERZ_OP_CPUID => {
                if reg == OCERZ_RAX || reg == OCERZ_RBX || reg == OCERZ_RCX || reg == OCERZ_RDX {
                    return 1;
                }
            }
            OCERZ_OP_RDTSC | OCERZ_OP_RDTSCP | OCERZ_OP_XGETBV => {
                if reg == OCERZ_RAX || reg == OCERZ_RDX || reg == OCERZ_RCX {
                    return 1;
                }
            }
            OCERZ_OP_MOVS | OCERZ_OP_STOS | OCERZ_OP_LODS | OCERZ_OP_SCAS | OCERZ_OP_CMPS => {
                if reg == OCERZ_RSI || reg == OCERZ_RDI || reg == OCERZ_RCX || reg == OCERZ_RAX {
                    return 1;
                }
            }
            OCERZ_OP_SYSCALL | OCERZ_OP_INT | OCERZ_OP_INT3 => {
                return 1;
            }
            OCERZ_OP_XCHG | OCERZ_OP_XADD => {
                for k in 0..(*in_).nops as usize {
                    if (*in_).ops[k].kind as c_uint == OCERZ_OPK_REG
                        && ((*in_).ops[k].reg & 15) as c_uint == reg
                    {
                        return 1;
                    }
                }
                return 0;
            }
            OCERZ_OP_CMPXCHG | OCERZ_OP_CMPXCHGXB => {
                if reg == OCERZ_RAX || reg == OCERZ_RDX || reg == OCERZ_RBX || reg == OCERZ_RCX {
                    return 1;
                }
            }
            _ => {}
        }
        if (*in_).nops > 0
            && (*in_).ops[0].kind as c_uint == OCERZ_OPK_REG
            && ((*in_).ops[0].reg & 15) as c_uint == reg
        {
            return 1;
        }
        0
    }
}

#[unsafe(no_mangle)]
pub static mut g_lowhoist_marks: MarkSet = MarkSet {
    off: [0; LOWHOIST_N as usize],
    full: 0,
};

fn lowhoist_marked(key: u64) -> c_int {
    unsafe { ji::mark_has(&raw mut g_lowhoist_marks, key) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn select_low_hoist(insns: *const X86Insn, n: c_int, rip: u64) -> c_int {
    unsafe {
        if ocerz_low_base == 0
            || ocerz_guest_base != 0
            || g_xlat_mode32 != 0
            || g_pin_class != 3
            || g_no_chain != 0
            || low_guard_fast_ok() == 0
            || g_mem_hoist_greg >= 0
            || n < 2
            || env_on!("OCERZ_NO_LOW_HOIST") != 0
        {
            return -1;
        }

        if lowhoist_marked(ji::jit_key(rip, 0)) != 0 {
            g_tc_learned = 1;
            return -1;
        }
        let mut cnt = [0i32; 16];
        let mut until = [0i32; 16];
        let shut = [0i32; 16];
        let mut lo = [0i32; 16];
        let mut hi = [0i32; 16];
        for r in 0..16 {
            until[r] = n;
        }
        for i in 0..n {
            let in_ = insns.add(i as usize);
            for k in 0..(*in_).nops as usize {
                let m = &(*in_).ops[k];
                if m.kind as c_uint != OCERZ_OPK_MEM
                    || m.riprel != 0
                    || m.base == OCERZ_REG_NONE as u8
                    || m.index != OCERZ_REG_NONE as u8
                {
                    continue;
                }
                let r = (m.base & 15) as usize;
                if shut[r] != 0 || until[r] < n {
                    continue;
                }
                let sz = if m.size != 0 { m.size as i64 } else { 64 };
                if (*in_).seg != OCERZ_SEG_NONE as u8
                    || (*in_).addrsize != 8
                    || m.disp < -4095
                    || m.disp + sz > 4095
                    || sz > 64
                {
                    cnt[r] = -1000;
                    continue;
                }
                cnt[r] += 1;
                if m.disp < lo[r] as i64 {
                    lo[r] = m.disp as i32;
                }
                if m.disp + sz > hi[r] as i64 {
                    hi[r] = (m.disp + sz) as i32;
                }
            }
            for r in 0..16usize {
                if shut[r] == 0 && until[r] == n && insn_may_write_gpr(in_, r as c_uint) != 0 {
                    until[r] = i + 1;
                }
            }
        }
        let mut best = -1;
        for r in 0..16usize {
            if r as c_uint == OCERZ_RSP || ji::pin_slot(r as c_uint) < 0 || cnt[r] < 3 {
                continue;
            }
            if best < 0 || cnt[r] > cnt[best as usize] {
                best = r as i32;
            }
        }
        if best < 0 {
            return -1;
        }
        g_low_hoist_until = until[best as usize];
        g_low_hoist_lo = lo[best as usize];
        g_low_hoist_hi = if hi[best as usize] > 0 {
            hi[best as usize]
        } else {
            1
        };
        best
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_low_hoist_check(b: *mut A64Buf) {
    unsafe {
        let hb = ji::pin_hreg(ji::pin_slot(g_low_hoist_greg as c_uint));
        g_n_low_hoist_bail = 0;
        a64_add_imm(b, 1, JTT as c_int, hb, g_low_hoist_hi as u32);
        a64_orr_reg(b, 1, JTT as c_int, JTT as c_int, hb, 0);
        a64_lsr_imm(b, 1, JTT as c_int, JTT as c_int, 32);
        a64_sub_imm(
            b,
            1,
            JTT as c_int,
            JTT as c_int,
            (OCERZ_LOW_LIMIT >> 32) as u32,
        );
        g_low_hoist_bail[g_n_low_hoist_bail as usize] = a64_label(b);
        g_n_low_hoist_bail += 1;
        a64_tbz(b, JTT as c_int, 63, 0);
        if g_low_hoist_lo < 0 {
            a64_sub_imm(b, 1, JTT as c_int, hb, g_low_hoist_lo.wrapping_neg() as u32);
            g_low_hoist_bail[g_n_low_hoist_bail as usize] = a64_label(b);
            g_n_low_hoist_bail += 1;
            a64_tbnz(b, JTT as c_int, 63, 0);
        }
        a64_try_orr_imm(b, 1, JMEMBASE as c_int, hb, ocerz_low_base);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_low_hoist_bail(
    b: *mut A64Buf,
    rip: u64,
    epi_sites: *mut *mut u32,
    n_epi: *mut c_int,
) {
    unsafe {
        if g_n_low_hoist_bail == 0 {
            return;
        }
        for k in 0..g_n_low_hoist_bail as usize {
            a64_patch_tbz(g_low_hoist_bail[k], a64_label(b));
        }
        g_n_low_hoist_bail = 0;
        ji::tc_imm64(b, JT0 as c_int, TCR_BLK as c_int, 0, g_cur_blk as u64);
        a64_str(b, 8, JT0 as c_int, 20, ji::SIDE_BLK_OFF);
        a64_movn(b, JT0 as c_int, 1, 0);
        a64_str(b, 4, JT0 as c_int, 20, ji::SIDE_IDX_OFF);
        a64_mov_imm64(b, JT0 as c_int, rip);
        a64_str(b, 8, JT0 as c_int, 20, ji::RIP_OFF);
        a64_mov_imm64(b, 0, OCERZ_STEP_PROFILE as u64);
        *epi_sites.add(*n_epi as usize) = a64_label(b);
        *n_epi += 1;
        a64_b(b, 0);
    }
}

unsafe fn emit_misaligned_arm(b: *mut A64Buf, o: *const OrderedSlowPend) {
    unsafe {
        let cand = [JTF as c_int, JTT as c_int, JTU as c_int];
        let mut sc = [0i32; 2];
        let mut n = 0;
        for i in 0..3 {
            if n >= 2 {
                break;
            }
            if cand[i] != (*o).rv && cand[i] != (*o).ra {
                sc[n] = cand[i];
                n += 1;
            }
        }
        let s1 = sc[0];
        let s2 = sc[1];
        let size = (*o).size;
        if (*o).store == 0 {
            if (*o).vec != 0 {
                a64_ldr_v(b, size, (*o).rv, (*o).ra, (*o).disp as u32);
            } else {
                a64_ldr(b, size, (*o).rv, (*o).ra, 0);
            }
            a64_dmb_ishld(b);
            return;
        }
        if (*o).vec != 0 && g_blk_ordered_loads != 0 {
            a64_dmb_ish(b);
            a64_str_v(b, size, (*o).rv, (*o).ra, (*o).disp as u32);
            return;
        }
        if (*o).vec == 0 {
            if size == 8 {
                a64_try_ands_imm(b, 1, A64_ZR as c_int, (*o).ra, 3);
                let to_bytes = a64_label(b);
                a64_bcond(b, A64_NE as c_int, 0);
                ji::emit_misaligned_pieces_st(b, 4, 2, (*o).rv, (*o).ra, (*o).disp, s1);
                let to_done = a64_label(b);
                a64_b(b, 0);
                a64_patch_bcond(to_bytes, a64_label(b));
                ji::emit_misaligned_pieces_st(b, 1, 8, (*o).rv, (*o).ra, (*o).disp, s1);
                a64_patch_b(to_done, a64_label(b));
            } else {
                ji::emit_misaligned_pieces_st(b, 1, size, (*o).rv, (*o).ra, (*o).disp, s1);
            }
            return;
        }
        let nh = if size == 16 { 2 } else { 1 };
        let hs = if size == 4 { 4 } else { 8 };
        if size == 4 {
            a64_fmov_x_from_v(b, 0, s1, (*o).rv);
        } else {
            a64_fmov_x_from_v(b, 1, s1, (*o).rv);
        }
        if nh == 2 {
            a64_umov_gpr(b, 8, s2, (*o).rv, 1);
        }
        let mut to_done: [*mut u32; 2] = [null_mut(), null_mut()];
        for level in 0..3 {
            let psize = if level == 0 {
                4
            } else if level == 1 {
                2
            } else {
                1
            };
            let mut to_next: *mut u32 = null_mut();
            if level < 2 {
                a64_try_ands_imm(b, 1, A64_ZR as c_int, (*o).ra, (psize - 1) as u64);
                to_next = a64_label(b);
                a64_bcond(b, A64_NE as c_int, 0);
            }
            for h in 0..nh {
                let r = if h != 0 { s2 } else { s1 };
                for i in 0..hs / psize {
                    if i != 0 {
                        a64_lsr_imm(b, 1, r, r, psize * 8);
                    }
                    a64_stlur(b, psize, r, (*o).ra, (*o).disp + h * 8 + i * psize);
                }
            }
            if level < 2 {
                to_done[level as usize] = a64_label(b);
                a64_b(b, 0);
                a64_patch_bcond(to_next, a64_label(b));
            }
        }
        a64_patch_b(to_done[0], a64_label(b));
        a64_patch_b(to_done[1], a64_label(b));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_guard_arms(b: *mut A64Buf, entry: *const u32) {
    unsafe {
        for k in 0..g_n_garm as usize {
            let lo = a64_label(b);
            a64_patch_cbz(g_garm[k].site, lo);
            emit_guard_full(b, g_garm[k].reg);
            let here = a64_label(b);
            a64_b(b, g_garm[k].back.offset_from(here) as i32);
            if g_n_fpbmap < JIT_MAX_BLOCK_INSNS as c_int {
                let e = &mut g_fpbmap[g_n_fpbmap as usize];
                e.lo = lo.offset_from(entry) as u32;
                e.hi = a64_label(b).offset_from(entry) as u32;
                e.idx = g_garm[k].idx;
                g_n_fpbmap += 1;
            }
        }
        g_n_garm = 0;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn emit_ordered_slow_arms(
    b: *mut A64Buf,
    blk: *mut JitBlock,
    entry: *const u32,
) {
    unsafe {
        if g_n_oslow <= 0 && g_n_fpbmap <= 0 {
            return;
        }
        (*blk).oslow = libc::malloc(
            ((g_n_oslow + g_n_fpbmap) as usize)
                .wrapping_mul(core::mem::size_of::<JitBlock_JitOslowMap>()),
        ) as *mut JitBlock_JitOslowMap;
        (*blk).n_oslow = 0;
        if !(*blk).oslow.is_null() {
            for i in 0..g_n_fpbmap as usize {
                *(*blk).oslow.add((*blk).n_oslow as usize) = g_fpbmap[i];
                (*blk).n_oslow += 1;
            }
        }
        g_n_fpbmap = 0;
        for i in 0..g_n_oslow as usize {
            let o = &g_oslow[i];
            let lo = a64_label(b);
            a64_patch_bcond(o.bne, lo);
            let cand = [JTF as c_int, JTT as c_int, JTU as c_int];
            let mut sc = [0i32; 2];
            let mut nsc = 0;
            for k in 0..3 {
                if nsc >= 2 {
                    break;
                }
                if cand[k] != o.rv && cand[k] != o.ra {
                    sc[nsc] = cand[k];
                    nsc += 1;
                }
            }
            a64_stp_pre(b, sc[0], sc[1], 31, -16);
            emit_misaligned_arm(b, o);
            a64_ldp_post(b, sc[0], sc[1], 31, 16);
            let here = a64_label(b);
            a64_b(b, o.back.offset_from(here) as i32);
            if !(*blk).oslow.is_null() {
                let e = &mut *(*blk).oslow.add((*blk).n_oslow as usize);
                e.lo = lo.offset_from(entry) as u32;
                e.hi = a64_label(b).offset_from(entry) as u32;
                e.idx = o.idx;
                (*blk).n_oslow += 1;
            }
        }
        g_n_oslow = 0;
    }
}

//! The JIT: guest basic blocks translated to native arm64, with the
//! interpreter as the fallback for anything it will not encode.
//!
//! ---- i386 ----
//! 32-bit blocks are compiled from a whitelist of instructions (m32_inline_ok),
//! with no superblocks and 0x67/16-bit addressing left to the interpreter.
//! Effective addresses wrap at 2^32 before the host mapping is applied, and pin
//! class 2 (the 64-bit CALL/RET protocol) is never selected for them.
//!
//! A cmp or test, an inc or dec, or an add/sub then inc/dec, that ends a block
//! in a jcc fuses with it as in 64-bit code; the only difference is the
//! fall-through address, which wraps at 2^32 like EIP.  Unfused, every loop
//! branch wrote a flag record for the jcc to read back: `add eax, ebx ; dec
//! edi ; jnz` ran at 7.1 ns an iteration and runs at 0.4.
//!
//! The condition forwarding is on as well - NZCV from an adjacent producer, E,
//! NE, S and NS from a result register, a comis redone by fcmp - and so are the
//! mov+logic and add+inc pairs and mov sinking into a shift.  Forwarding is
//! only sound when the consumer is translated, since the producer then leaves
//! its flags in NZCV alone, so cc_consumer_inline_ok asks m32_inline_ok about a
//! 32-bit consumer: a cmov through 16-bit addressing is interpreted, and read a
//! stale record.  (So did a 64-bit cmov whose memory operand has a 0x67
//! prefix.)  After a fused pair the translate loop takes the pair's second
//! instruction as the latest flag producer; it went on naming the first, so
//! after `add ; inc` a jb or setb read the inc's record as an add's, in both
//! modes.  The FP batch and lane-0 machinery stays off in 32-bit blocks: the
//! differential has no SSE arithmetic corpus to hold it to.
//!
//! An fs- or gs-relative operand adds the segment base to the wrapped address
//! without wrapping the sum, as ocerz_ea does, and then takes the same guard and
//! translation as any other operand.  32-bit Windows code reads fs:[0] for every
//! SEH frame it pushes and pops and fs:[0x18] and fs:[0x2c] for the TEB and its
//! TLS slots, and each of those was a slow call.  push and pop with a memory
//! operand are translated too, since `push dword fs:[0]` opens every frame.
//!
//! In the Wine layout a 32-bit stack slot always lies in the low window, so
//! push, pop, call, ret and leave reach it with low_base or'ed into the
//! zero-extended esp, with no range test (low_guard_fast_ok has checked that
//! orr can encode low_base).  They used to want guest_base in JGB, which that
//! layout does not keep, so in the one layout 32-bit code runs in every one of
//! them was a slow call: an SEH frame of two pushes cost 37 ns, now 9.
//!
//! Arithmetic on memory, xchg, xadd and cmpxchg go through emit_rmw_mem as in
//! 64-bit blocks: in ordered mode the locked and exchanging forms are LSE
//! atomics, and an access that is not naturally aligned leaves for the
//! interpreter out of line.  cmpxchg8b is a casal of EDX:EAX against ECX:EBX
//! that writes ZF into the materialized flags and EDX:EAX only on a mismatch,
//! and xchg between two registers goes through a scratch register.  A block
//! has room for 32 out-of-line arms; past that the slow call goes inline behind
//! a branch.  emit_rmw_mem used to give up there after emitting its atomic, so
//! the slow call emitted in its place performed an aligned access a second
//! time, and a misaligned one spun on the alignment branch, never patched.
//!
//! bt, bts, btr and btc on a register and bt on memory are emit_bt's, whose
//! 32-bit operand forms are those of 64-bit code; a register offset into memory
//! is sign-extended from its own size and added after the address wraps, as in
//! the interpreter.  bts, btr and btc on memory stay interpreted, in both modes.
//!
//! ---- bisection ----
//! OCERZ_INTERP_LO/HI and OCERZ_INTERP_RIP keep chosen ranges or addresses in
//! the interpreter, which is how a JIT miscompile is narrowed down;
//! OCERZ_CHAINCHECK validates every published jump target against the arena,
//! OCERZ_INVMAP_CHECK asserts the region map's one invariant, OCERZ_BTRACE
//! records block entries, and the OCERZ_UNSAFE_* knobs are measurement aids that
//! deliberately produce wrong state and must never be enabled outside a
//! benchmark.
//!
//!
//! Set by emit_mem_ea when the address it just formed is a constant, for the guard that follows.
//!
//! A 64-bit constant that takes three or four instructions to build is one load from the block's literal pool.
//!
//! The exactness test for VX2 = VX0 op VX1: falls through when exact, or branches to pe[] or ok[].
//!
//! Two pushes, or two pops, of 64-bit registers in a row, where the stack
//! delta is in use: one address, one stp or ldp, and rsp moved once afterwards.
//! rsp moves only after the access, so a fault restarts the pair at its first
//! instruction with nothing yet done; a pop pair never loads one register twice
//! (an ldp with equal destinations is unpredictable).  Prologues and epilogues
//! are these runs: fib's four pushes went from 12 instructions to 6.  The
//! call-frame forms and the push/pop renames own their instructions and are
//! left alone.  OCERZ_NO_STACK_PAIR=1 turns it off.
//!
//! ---- Rust port ----
//! This module is the Rust translation of src/jit.c.  The decode loop that
//! runs under ocerz_jit_decode_recover's sigsetjmp lives in src/jit_core_shim.c
//! so that no Rust frame sits between the sigsetjmp and a siglongjmp out of a
//! faulting guest-code read.
#![allow(unsafe_op_in_unsafe_fn)]
#![allow(clippy::missing_safety_doc)]
#![allow(static_mut_refs)]
#![allow(unexpected_cfgs)]

use core::ffi::{c_char, c_int, c_uint, c_void};
use core::mem::{size_of, size_of_val, MaybeUninit};
use core::ptr::{null, null_mut};
use core::sync::atomic::{AtomicPtr, AtomicU64, Ordering};

use crate::ffi::*;
use crate::jit_internal::*;
use core::mem::offset_of;

const JT0: c_int = crate::ffi::JT0 as c_int;
const JT1: c_int = crate::ffi::JT1 as c_int;
const JT2: c_int = crate::ffi::JT2 as c_int;
const JTT: c_int = crate::ffi::JTT as c_int;
const JTU: c_int = crate::ffi::JTU as c_int;
const JTA: c_int = crate::ffi::JTA as c_int;
const JGB: c_int = crate::ffi::JGB as c_int;
const JMEMAUX: c_int = crate::ffi::JMEMAUX as c_int;
const JMEMBASE: c_int = crate::ffi::JMEMBASE as c_int;
const JMEMBASE2: c_int = crate::ffi::JMEMBASE2 as c_int;
const JMEMBASE3: c_int = crate::ffi::JMEMBASE3 as c_int;
const A64_EQ: c_int = crate::ffi::A64_EQ as c_int;
const A64_NE: c_int = crate::ffi::A64_NE as c_int;
const OCERZ_FL_ALL: u64 = crate::inline::OCERZ_CF | crate::inline::OCERZ_PF | crate::inline::OCERZ_AF
    | crate::inline::OCERZ_ZF | crate::inline::OCERZ_SF | crate::inline::OCERZ_OF;
const KREG: u8 = OCERZ_OPK_REG as u8;
const KIMM: u8 = OCERZ_OPK_IMM as u8;
const KMEM: u8 = OCERZ_OPK_MEM as u8;
const KXMM: u8 = OCERZ_OPK_XMM as u8;
const KMMX: u8 = OCERZ_OPK_MMX as u8;
const SEG_NONE: u8 = OCERZ_SEG_NONE as u8;

unsafe extern "C" {
    static ocerz_cxa_throw_rip: u64;
    fn pthread_jit_write_protect_np(enabled: c_int);
    fn sys_icache_invalidate(start: *mut c_void, len: usize);
    fn clock_gettime_nsec_np(clock: libc::clockid_t) -> u64;
}

#[inline(always)]
unsafe fn ocerz_insn_has_mmx(insn: *const X86Insn) -> c_int {
    for i in 0..(*insn).nops as usize {
        if (*insn).ops[i].kind == KMMX {
            return 1;
        }
    }
    0
}

#[inline(always)]
fn arr_elem<T, const N: usize>(a: *mut [T; N]) -> *mut T {
    a.cast()
}

#[inline(always)]
fn pointee_size<T>(_: *mut T) -> usize {
    size_of::<T>()
}

macro_rules! env_on {
    ($name:literal) => {{
        static mut ON_: c_int = -1;
        if ON_ < 0 {
            ON_ = (!libc::getenv(concat!($name, "\0").as_ptr().cast()).is_null()) as c_int;
        }
        ON_ != 0
    }};
}

macro_rules! ga {
    ($a:ident, $i:expr) => {
        *arr_elem(&raw mut $a).add($i as usize)
    };
}


#[unsafe(no_mangle)]
pub static mut g_flaglive_log: c_int = 0;
static mut g_rsp_lag: u32 = 0;
static mut g_flag_producer_operands_intact: c_int = 0;
static mut g_xlat_n: c_int = 0;
static mut g_lowstack_check: c_int = 0;
static mut g_lowstack_from: c_int = 0;
static mut g_align_any: c_int = 0;
static mut g_align_blk: c_int = 0;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rsp_ptr3() -> c_int {
    static mut OFF: c_int = -1;
    if OFF < 0 {
        OFF = (!libc::getenv(c"OCERZ_RSP_VALUE".as_ptr()).is_null()) as c_int;
    }
    (OFF == 0) as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn g_pin_class_fwd() -> c_int {
    g_pin_class
}

#[unsafe(no_mangle)]
pub static mut ocerz_jit_time_xlat: c_int = 0;
#[unsafe(no_mangle)]
pub static mut ocerz_jit_xlat_ns: u64 = 0;
#[unsafe(no_mangle)]
pub static ps_hits: AtomicU64 = AtomicU64::new(0);
#[unsafe(no_mangle)]
pub static ps_misses: AtomicU64 = AtomicU64::new(0);
#[unsafe(no_mangle)]
pub static ps_steps: AtomicU64 = AtomicU64::new(0);


#[unsafe(no_mangle)]
pub static mut ps_retsite: [JitState_ps_retsite; PS_RETSITE_N as usize] = unsafe { core::mem::zeroed() };
#[unsafe(no_mangle)]
pub static mut ps_t0: u64 = 0;
#[unsafe(no_mangle)]
#[thread_local]
pub static mut t_xlat_overflow: c_int = 0;

static mut g_keep_cap: c_int = 0;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn is_terminator(op: c_uint) -> c_int {
    match op {
        OCERZ_OP_JMP | OCERZ_OP_JCC | OCERZ_OP_JRCXZ | OCERZ_OP_LOOP | OCERZ_OP_LOOPE
        | OCERZ_OP_LOOPNE | OCERZ_OP_CALL | OCERZ_OP_RET | OCERZ_OP_IRET | OCERZ_OP_JMPF
        | OCERZ_OP_CALLF | OCERZ_OP_RETF | OCERZ_OP_SYSCALL | OCERZ_OP_INT3 | OCERZ_OP_INT
        | OCERZ_OP_UD2 | OCERZ_OP_HLT => 1,
        _ => 0,
    }
}

fn term_may_switch_mode(op: c_uint) -> c_int {
    (op == OCERZ_OP_IRET || op == OCERZ_OP_JMPF || op == OCERZ_OP_CALLF || op == OCERZ_OP_RETF) as c_int
}

static mut g_cur_fpb: c_int = -1;
#[unsafe(no_mangle)]
pub static mut g_xlat_ftop: c_int = -1;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn m32_inline_ok(insn: *const X86Insn) -> c_int {
    let i = &*insn;
    if i.addrsize != 4 {
        return 0;
    }
    if x87_inline_ok(insn) != 0 {
        return 1;
    }
    match i.op as c_uint {
        OCERZ_OP_NOP | OCERZ_OP_PAUSE | OCERZ_OP_PREFETCH | OCERZ_OP_CLFLUSH | OCERZ_OP_MOV |
        OCERZ_OP_MOVZX | OCERZ_OP_MOVSX | OCERZ_OP_ADD | OCERZ_OP_SUB | OCERZ_OP_CMP |
        OCERZ_OP_AND | OCERZ_OP_OR | OCERZ_OP_XOR | OCERZ_OP_TEST | OCERZ_OP_ADC |
        OCERZ_OP_SBB | OCERZ_OP_INC | OCERZ_OP_DEC | OCERZ_OP_NOT | OCERZ_OP_NEG |
        OCERZ_OP_SHL | OCERZ_OP_SHR | OCERZ_OP_SAR | OCERZ_OP_ROL | OCERZ_OP_ROR |
        OCERZ_OP_SHLD | OCERZ_OP_SHRD | OCERZ_OP_MUL | OCERZ_OP_IMUL | OCERZ_OP_DIV |
        OCERZ_OP_IDIV | OCERZ_OP_CBW | OCERZ_OP_CWD | OCERZ_OP_CMOVCC | OCERZ_OP_SETCC |
        OCERZ_OP_BSWAP | OCERZ_OP_BSF | OCERZ_OP_BSR | OCERZ_OP_TZCNT | OCERZ_OP_LZCNT |
        OCERZ_OP_POPCNT | OCERZ_OP_LEA | OCERZ_OP_PUSH | OCERZ_OP_POP | OCERZ_OP_LEAVE |
        OCERZ_OP_PMOVMSKB | OCERZ_OP_XCHG | OCERZ_OP_XADD | OCERZ_OP_CMPXCHG |
        OCERZ_OP_CMPXCHGXB | OCERZ_OP_BT | OCERZ_OP_BTS | OCERZ_OP_BTR | OCERZ_OP_BTC => 1,
        op => (op >= OCERZ_OP_MOVUPS && op <= OCERZ_OP_PBLENDVB) as c_int,
    }
}

unsafe fn try_inline(b: *mut A64Buf, insn: *const X86Insn, need: u64, exit_sites: *mut *mut u32, n_exits: *mut c_int) -> c_int {
    let i = &*insn;
    if ocerz_insn_has_mmx(insn) != 0 {
        return if i.mode32 != 0 { 0 } else { emit_mmx(b, insn, exit_sites, n_exits) };
    }
    if i.vex != 0 {
        return emit_vex(b, insn, exit_sites, n_exits);
    }
    if i.mode32 != 0 && m32_inline_ok(insn) == 0 {
        return 0;
    }
    let op = i.op as c_uint;
    if op > OCERZ_OP_X87_FIRST && op < OCERZ_OP_SSE_FIRST {
        return emit_x87(b, insn, need, exit_sites, n_exits);
    }
    if op == OCERZ_OP_NOP || op == OCERZ_OP_PAUSE || op == OCERZ_OP_PREFETCH || op == OCERZ_OP_CLFLUSH {
        return 1;
    }
    if (op == OCERZ_OP_XOR || op == OCERZ_OP_CWD)
        && !g_cur_insns.is_null()
        && insn == g_cur_insns.add(g_cur_insn_idx as usize) as *const X86Insn
        && rdx_prep_skippable(g_cur_insns, g_cur_insn_idx, g_cur_insns_n, need) != 0
    {
        g_div_prev_skipped = 1;
        return 1;
    }
    g_div_prev_skipped = 0;

    if op == OCERZ_OP_MOV {
        let d = &i.ops[0];
        let s = &i.ops[1];
        if d.kind == KREG && d.high8 != 0 {
            return 0;
        }
        if d.kind == KREG && (d.size == 4 || d.size == 8) {
            if s.kind == KREG && s.high8 == 0 && s.size == d.size {
                let sz4 = (d.size == 4) as c_int;
                let ds = pin_slot(d.reg as c_uint);
                let ss = pin_slot(s.reg as c_uint);
                let host_rsp = rsp_is_ptr() != 0 && (d.reg as c_uint == OCERZ_RSP || s.reg as c_uint == OCERZ_RSP);
                if host_rsp && sz4 == 0 && d.reg == s.reg {
                    return 1;
                }
                if rsp_is_ptr() != 0 && sz4 == 0 && s.reg as c_uint == OCERZ_RSP && d.reg as c_uint != OCERZ_RSP && ds >= 0 && ss >= 0 {
                    if jgb_usable() != 0 {
                        a64_sub_reg(b, 1, pin_hreg(ds), pin_hreg(ss), JGB, 0);
                    } else {
                        a64_mov_imm64(b, pin_hreg(ds), ocerz_guest_base);
                        a64_sub_reg(b, 1, pin_hreg(ds), pin_hreg(ss), pin_hreg(ds), 0);
                    }
                    return 1;
                }
                if ds >= 0 && ss >= 0 && !host_rsp {
                    if ds != ss || sz4 != 0 {
                        a64_mov_reg(b, if sz4 != 0 { 0 } else { 1 }, pin_hreg(ds), pin_hreg(ss));
                    }
                    return 1;
                }
                emit_gpr_rd(b, if sz4 != 0 { 0 } else { 1 }, JT0, s.reg as c_uint);
                emit_gpr_wr(b, JT0, d.reg as c_uint);
                return 1;
            }
            if s.kind == KIMM {
                let mut v = s.imm;
                if d.size == 4 {
                    v &= 0xffffffff;
                }
                let ds = pin_slot(d.reg as c_uint);
                if ds >= 0 && !(rsp_is_ptr() != 0 && d.reg as c_uint == OCERZ_RSP) {
                    a64_mov_imm64(b, pin_hreg(ds), v);
                } else {
                    a64_mov_imm64(b, JT0, v);
                    emit_gpr_wr(b, JT0, d.reg as c_uint);
                }
                return 1;
            }
        }
        if d.kind == KREG && (d.size == 1 || d.size == 2) && pin_slot(d.reg as c_uint) >= 0
            && !(rsp_is_ptr() != 0 && d.reg as c_uint == OCERZ_RSP)
        {
            let rd = pin_hreg(pin_slot(d.reg as c_uint));
            if s.kind == KREG && s.high8 == 0 && s.size == d.size && pin_slot(s.reg as c_uint) >= 0
                && !(rsp_is_ptr() != 0 && s.reg as c_uint == OCERZ_RSP)
            {
                if s.reg != d.reg {
                    a64_bfi(b, 1, rd, pin_hreg(pin_slot(s.reg as c_uint)), 0, d.size as c_int * 8);
                }
                return 1;
            }
            if s.kind == KIMM {
                let v = s.imm & ((1u64 << (d.size as u32 * 8)) - 1);
                a64_mov_imm64(b, JT0, v);
                a64_bfi(b, 1, rd, JT0, 0, d.size as c_int * 8);
                return 1;
            }
        }
        if s.kind == KMEM || d.kind == KMEM {
            return emit_mov_mem(b, insn, exit_sites, n_exits);
        }
        return 0;
    }

    match op {
        OCERZ_OP_ADD | OCERZ_OP_SUB | OCERZ_OP_CMP | OCERZ_OP_AND | OCERZ_OP_OR | OCERZ_OP_XOR | OCERZ_OP_TEST => {
            if emit_cmp_test_narrow(b, insn, need, exit_sites, n_exits) != 0 {
                return 1;
            }
            if emit_arith_narrow(b, insn, need) != 0 {
                return 1;
            }
            if i.ops[0].kind == KMEM {
                return emit_rmw_mem(b, insn, need, exit_sites, n_exits);
            }
            if i.ops[1].kind == KMEM {
                return emit_arith_mem(b, insn, need, exit_sites, n_exits);
            }
            emit_arith(b, insn, need)
        }
        OCERZ_OP_INC | OCERZ_OP_DEC => {
            if i.ops[0].kind == KMEM {
                return emit_rmw_mem(b, insn, need, exit_sites, n_exits);
            }
            emit_incdec(b, insn, need)
        }
        OCERZ_OP_XCHG => {
            if i.mode32 != 0 && i.ops[0].kind == KREG && i.ops[1].kind == KREG {
                return emit_xchg_reg32(b, insn);
            }
            emit_rmw_mem(b, insn, need, exit_sites, n_exits)
        }
        OCERZ_OP_XADD | OCERZ_OP_CMPXCHG => emit_rmw_mem(b, insn, need, exit_sites, n_exits),
        OCERZ_OP_CMPXCHGXB => emit_cmpxchg8b(b, insn, exit_sites, n_exits),
        OCERZ_OP_SHL | OCERZ_OP_SHR | OCERZ_OP_SAR => {
            if i.ops[1].kind == KREG {
                return emit_shift_cl(b, insn, need);
            }
            emit_shift(b, insn, need)
        }
        OCERZ_OP_ROL | OCERZ_OP_ROR => emit_rot(b, insn, need),
        OCERZ_OP_NOT | OCERZ_OP_NEG => {
            if i.ops[0].kind == KMEM {
                return emit_rmw_mem(b, insn, need, exit_sites, n_exits);
            }
            emit_not_neg(b, insn, need)
        }
        OCERZ_OP_ADC | OCERZ_OP_SBB => emit_adc_sbb(b, insn, need),
        OCERZ_OP_CBW | OCERZ_OP_CWD => emit_cbw_cwd(b, insn),
        OCERZ_OP_DIV | OCERZ_OP_IDIV => emit_div(b, insn, exit_sites, n_exits),
        OCERZ_OP_CMOVCC => {
            if env_on!("OCERZ_NO_INLINE_CMOV") {
                return 0;
            }
            emit_cmov(b, insn, exit_sites, n_exits)
        }
        OCERZ_OP_SETCC => {
            if env_on!("OCERZ_NO_INLINE_SETCC") {
                return 0;
            }
            emit_setcc(b, insn)
        }
        OCERZ_OP_BSWAP => emit_bswap(b, insn),
        OCERZ_OP_MOVUPS | OCERZ_OP_MOVAPS | OCERZ_OP_MOVDQA | OCERZ_OP_MOVDQU |
        OCERZ_OP_MOVSS | OCERZ_OP_MOVSDX | OCERZ_OP_MOVLPS | OCERZ_OP_MOVHPS | OCERZ_OP_ADDSS |
        OCERZ_OP_ADDSD | OCERZ_OP_ADDPS | OCERZ_OP_ADDPD | OCERZ_OP_SUBSS | OCERZ_OP_SUBSD |
        OCERZ_OP_SUBPS | OCERZ_OP_SUBPD | OCERZ_OP_MULSS | OCERZ_OP_MULSD | OCERZ_OP_MULPS |
        OCERZ_OP_MULPD | OCERZ_OP_DIVSS | OCERZ_OP_DIVSD | OCERZ_OP_DIVPS | OCERZ_OP_DIVPD |
        OCERZ_OP_MAXSS | OCERZ_OP_MAXSD | OCERZ_OP_MINSS | OCERZ_OP_MINSD | OCERZ_OP_MAXPS |
        OCERZ_OP_MAXPD | OCERZ_OP_MINPS | OCERZ_OP_MINPD | OCERZ_OP_SQRTSS | OCERZ_OP_SQRTSD |
        OCERZ_OP_SQRTPS | OCERZ_OP_SQRTPD | OCERZ_OP_PXOR | OCERZ_OP_XORPS | OCERZ_OP_PAND |
        OCERZ_OP_ANDPS | OCERZ_OP_POR | OCERZ_OP_ORPS | OCERZ_OP_PANDN | OCERZ_OP_ANDNPS |
        OCERZ_OP_PADDB | OCERZ_OP_PADDW | OCERZ_OP_PADDD | OCERZ_OP_PADDQ | OCERZ_OP_PSUBB |
        OCERZ_OP_PSUBW | OCERZ_OP_PSUBD | OCERZ_OP_PSUBQ | OCERZ_OP_PCMPEQB |
        OCERZ_OP_PCMPEQW | OCERZ_OP_PCMPEQD | OCERZ_OP_PCMPEQQ | OCERZ_OP_PCMPGTB |
        OCERZ_OP_PCMPGTW | OCERZ_OP_PCMPGTD | OCERZ_OP_PCMPGTQ | OCERZ_OP_PMINUB |
        OCERZ_OP_PMINUW | OCERZ_OP_PMINUD | OCERZ_OP_PMAXUB | OCERZ_OP_PMAXUW |
        OCERZ_OP_PMAXUD | OCERZ_OP_PMINSB | OCERZ_OP_PMINSW | OCERZ_OP_PMINSD |
        OCERZ_OP_PMAXSB | OCERZ_OP_PMAXSW | OCERZ_OP_PMAXSD | OCERZ_OP_PMULLW |
        OCERZ_OP_PMULLD | OCERZ_OP_PAVGB | OCERZ_OP_PAVGW | OCERZ_OP_PADDUSB |
        OCERZ_OP_PADDUSW | OCERZ_OP_PSUBUSB | OCERZ_OP_PSUBUSW | OCERZ_OP_PADDSB |
        OCERZ_OP_PADDSW | OCERZ_OP_PSUBSB | OCERZ_OP_PSUBSW | OCERZ_OP_PMADDWD |
        OCERZ_OP_PMULHRSW | OCERZ_OP_PACKSSDW | OCERZ_OP_PACKUSWB | OCERZ_OP_PMULUDQ |
        OCERZ_OP_PBLENDW | OCERZ_OP_PALIGNR | OCERZ_OP_PMULHW | OCERZ_OP_PMULHUW |
        OCERZ_OP_PACKSSWB | OCERZ_OP_PACKUSDW | OCERZ_OP_PMULDQ | OCERZ_OP_PSADBW |
        OCERZ_OP_PMADDUBSW | OCERZ_OP_PABSB | OCERZ_OP_PABSW | OCERZ_OP_PABSD |
        OCERZ_OP_PSIGNB | OCERZ_OP_PSIGNW | OCERZ_OP_PSIGND | OCERZ_OP_PHADDW |
        OCERZ_OP_PHADDD | OCERZ_OP_PHSUBW | OCERZ_OP_PHSUBD | OCERZ_OP_PHADDSW |
        OCERZ_OP_PHSUBSW | OCERZ_OP_PSHUFLW | OCERZ_OP_PSHUFHW | OCERZ_OP_PSLLDQ |
        OCERZ_OP_PSRLDQ | OCERZ_OP_BLENDPS | OCERZ_OP_BLENDPD | OCERZ_OP_MOVSHDUP |
        OCERZ_OP_MOVSLDUP | OCERZ_OP_MOVMSKPS | OCERZ_OP_MOVMSKPD | OCERZ_OP_CMPPS |
        OCERZ_OP_CMPPD | OCERZ_OP_CVTTPS2DQ | OCERZ_OP_CVTPS2DQ | OCERZ_OP_CVTDQ2PD |
        OCERZ_OP_CVTPS2PD | OCERZ_OP_CVTPD2PS | OCERZ_OP_AESENC | OCERZ_OP_AESENCLAST |
        OCERZ_OP_AESDEC | OCERZ_OP_AESDECLAST | OCERZ_OP_AESIMC | OCERZ_OP_AESKEYGENASSIST |
        OCERZ_OP_PCLMULQDQ | OCERZ_OP_UCOMISS | OCERZ_OP_UCOMISD | OCERZ_OP_COMISS |
        OCERZ_OP_COMISD | OCERZ_OP_CVTTSD2SI | OCERZ_OP_CVTTSS2SI | OCERZ_OP_CVTSI2SD |
        OCERZ_OP_CVTSI2SS | OCERZ_OP_CVTSD2SS | OCERZ_OP_CVTSS2SD | OCERZ_OP_CVTDQ2PS |
        OCERZ_OP_MOVD | OCERZ_OP_MOVQX | OCERZ_OP_PSHUFD | OCERZ_OP_PINSRB | OCERZ_OP_PINSRW |
        OCERZ_OP_PINSRD | OCERZ_OP_PINSRQ | OCERZ_OP_PEXTRB | OCERZ_OP_PEXTRW |
        OCERZ_OP_PEXTRD | OCERZ_OP_PEXTRQ | OCERZ_OP_PMOVSXBW | OCERZ_OP_PMOVSXBD |
        OCERZ_OP_PMOVSXBQ | OCERZ_OP_PMOVSXWD | OCERZ_OP_PMOVSXWQ | OCERZ_OP_PMOVSXDQ |
        OCERZ_OP_PMOVZXBW | OCERZ_OP_PMOVZXBD | OCERZ_OP_PMOVZXBQ | OCERZ_OP_PMOVZXWD |
        OCERZ_OP_PMOVZXWQ | OCERZ_OP_PMOVZXDQ | OCERZ_OP_ROUNDSS | OCERZ_OP_ROUNDSD |
        OCERZ_OP_ROUNDPS | OCERZ_OP_ROUNDPD | OCERZ_OP_PSHUFB | OCERZ_OP_PUNPCKLBW |
        OCERZ_OP_PUNPCKLWD | OCERZ_OP_PUNPCKLDQ | OCERZ_OP_PUNPCKLQDQ | OCERZ_OP_PUNPCKHBW |
        OCERZ_OP_PUNPCKHWD | OCERZ_OP_PUNPCKHDQ | OCERZ_OP_PUNPCKHQDQ | OCERZ_OP_UNPCKLPD |
        OCERZ_OP_UNPCKHPD | OCERZ_OP_MOVLHPS | OCERZ_OP_MOVHLPS | OCERZ_OP_UNPCKLPS |
        OCERZ_OP_UNPCKHPS | OCERZ_OP_CMPSS | OCERZ_OP_CMPSDX | OCERZ_OP_BLENDVPD |
        OCERZ_OP_BLENDVPS | OCERZ_OP_PBLENDVB | OCERZ_OP_MOVDDUP | OCERZ_OP_SHUFPS |
        OCERZ_OP_SHUFPD | OCERZ_OP_INSERTPS | OCERZ_OP_PSLLW | OCERZ_OP_PSLLD |
        OCERZ_OP_PSLLQ | OCERZ_OP_PSRLW | OCERZ_OP_PSRLD | OCERZ_OP_PSRLQ | OCERZ_OP_PSRAW |
        OCERZ_OP_PSRAD => emit_sse(b, insn, exit_sites, n_exits),
        OCERZ_OP_MOVNTI => emit_mov_mem(b, insn, exit_sites, n_exits),
        OCERZ_OP_CRC32 => emit_crc32(b, insn),
        OCERZ_OP_MUL => emit_mul_wide(b, insn, need, 0),
        OCERZ_OP_IMUL => {
            if i.nops == 1 {
                return emit_mul_wide(b, insn, need, 1);
            }
            if i.ops[0].kind == KMEM {
                return 0;
            }
            if ((i.nops > 1 && i.ops[1].kind == KMEM) || (i.nops > 2 && i.ops[2].kind == KMEM))
                && env_on!("OCERZ_NO_IMUL_MEM")
            {
                return 0;
            }
            emit_imul(b, insn, need)
        }
        OCERZ_OP_LEA => emit_lea(b, insn),
        OCERZ_OP_BSF | OCERZ_OP_BSR | OCERZ_OP_TZCNT | OCERZ_OP_LZCNT | OCERZ_OP_POPCNT => emit_bitscan(b, insn, need),
        OCERZ_OP_PMOVMSKB => emit_pmovmskb(b, insn),
        OCERZ_OP_BT | OCERZ_OP_BTS | OCERZ_OP_BTR | OCERZ_OP_BTC => emit_bt(b, insn, need, exit_sites, n_exits),
        OCERZ_OP_LEAVE => emit_leave(b, insn, exit_sites, n_exits),
        OCERZ_OP_SHLD | OCERZ_OP_SHRD => emit_shiftd(b, insn, need),
        OCERZ_OP_PUSH | OCERZ_OP_POP => {
            if i.nops == 1 && i.ops[0].kind == KMEM {
                return emit_push_pop_mem(b, insn, exit_sites, n_exits);
            }
            emit_push_pop(b, insn, exit_sites, n_exits)
        }
        OCERZ_OP_MOVZX | OCERZ_OP_MOVSX => emit_movx(b, insn, (op == OCERZ_OP_MOVSX) as c_int, exit_sites, n_exits),
        OCERZ_OP_MOVSXD => emit_movsxd(b, insn, exit_sites, n_exits),
        _ => 0,
    }
}

#[unsafe(no_mangle)]
pub static mut g_x87spec_marks: MarkSet = unsafe { core::mem::zeroed() };

unsafe fn select_mem_base_hoist(insns: *const X86Insn, n: c_int, rip: u64) -> c_int {
    g_mem_hoist_aux_disp = 0;
    g_mem_hoist_aux_index = -1;
    {
        static mut DIS: c_int = -1;
        if DIS < 0 {
            DIS = (!libc::getenv(c"OCERZ_NO_HOIST".as_ptr()).is_null()) as c_int;
        }
        if DIS != 0 {
            return -1;
        }
    }
    if g_no_chain != 0 || mem_guard_needed() != 0 || mem_native_store_ok() == 0 || n < 2 {
        return -1;
    }
    let term = &*insns.add(n as usize - 1);
    let self_loop = ((term.op as c_uint == OCERZ_OP_JCC && term.ops[0].kind == KIMM && term.ops[0].imm == rip)
        || (term.op as c_uint == OCERZ_OP_JMP && term.ops[0].kind == KIMM && term.ops[0].imm == rip)) as c_int;
    static mut HOIST_ALL: c_int = -1;
    if HOIST_ALL < 0 {
        HOIST_ALL = if libc::getenv(c"OCERZ_NO_HOIST_ALL".as_ptr()).is_null() { 1 } else { 0 };
    }
    if self_loop == 0 && (HOIST_ALL == 0 || ocerz_guest_base == 0) {
        return -1;
    }
    static mut MC: c_int = -1;
    if MC < 0 {
        let e = libc::getenv(c"OCERZ_HOIST_MIN".as_ptr());
        MC = if e.is_null() { 2 } else { libc::atoi(e) };
    }
    let min_count = if self_loop != 0 { 1 } else { MC };

    let mut count = [0 as c_int; 16];
    let mut aux = [0 as c_int; 16];
    for i in 0..n as usize {
        let inn = &*insns.add(i);
        if inn.seg != SEG_NONE || inn.addrsize != 8 {
            continue;
        }
        for k in 0..inn.nops as usize {
            let mem = &inn.ops[k];
            if mem.kind != KMEM || mem.riprel != 0 || mem.base as c_uint == OCERZ_REG_NONE {
                continue;
            }
            if pin_slot(mem.base as c_uint) < 0 {
                continue;
            }
            let bb = (mem.base & 15) as usize;
            count[bb] += 1;
            if mem.index as c_uint != OCERZ_REG_NONE && (mem.scale & 3) == 0 && pin_slot(mem.index as c_uint) >= 0
                && !(rsp_is_ptr() != 0 && (mem.index as c_uint == OCERZ_RSP || mem.base as c_uint == OCERZ_RSP))
            {
                count[(mem.index & 15) as usize] += 1;
            }
            if g_pin_class != 2 && mem.index as c_uint != OCERZ_REG_NONE && mem.disp != 0 && mem.disp >= -4095
                && mem.disp <= 4095 && aux[bb] == 0
            {
                aux[bb] = mem.disp as c_int;
            }
        }
    }
    let (mut best, mut bestn, mut second, mut secondn, mut third, mut thirdn) = (-1 as c_int, 0, -1 as c_int, 0, -1 as c_int, 0);
    for r in 0..16 as c_int {
        let cr = count[r as usize];
        if cr <= 0 || cr <= thirdn {
            continue;
        }
        if rsp_is_ptr() != 0 && r as c_uint == OCERZ_RSP {
            continue;
        }
        let mut written = 0;
        let mut i = 0;
        while i < n && written == 0 {
            written = insn_may_write_gpr(insns.add(i as usize), r as c_uint);
            i += 1;
        }
        if written != 0 {
            continue;
        }
        if cr > bestn {
            third = second; thirdn = secondn; second = best; secondn = bestn; best = r; bestn = cr;
        } else if cr > secondn {
            third = second; thirdn = secondn; second = r; secondn = cr;
        } else {
            third = r; thirdn = cr;
        }
    }
    if env_on!("OCERZ_HOISTLOG") && best < 0 {
        libc::fprintf(crate::log::stderr(), c"HOIST rip=%#llx no candidate (self_loop=%d n=%d)\n".as_ptr(),
            rip as libc::c_ulonglong, self_loop, n);
    }
    if best < 0 || bestn < min_count {
        return -1;
    }
    if secondn < min_count {
        second = -1;
    }
    if thirdn < min_count {
        third = -1;
    }
    {
        static mut NO2: c_int = -1;
        if NO2 < 0 {
            NO2 = (!libc::getenv(c"OCERZ_NO_HOIST2".as_ptr()).is_null()) as c_int;
        }
        if NO2 != 0 {
            second = -1;
            third = -1;
        }
    }
    {
        static mut NO3: c_int = -1;
        if NO3 < 0 {
            NO3 = (!libc::getenv(c"OCERZ_NO_HOIST3".as_ptr()).is_null()) as c_int;
        }
        if NO3 != 0 || g_pin_class != 3 {
            third = -1;
        }
    }
    g_mem_hoist_aux_disp = aux[best as usize];
    g_mem_hoist_greg2 = second;
    g_mem_hoist_greg3 = third;
    g_mem_hoist_aux_index = -1;
    {
        let mut icnt = [[0 as c_int; 4]; 16];
        let mut ilast = [[0 as c_int; 4]; 16];
        for i in 0..n as usize {
            let inn = &*insns.add(i);
            if inn.seg != SEG_NONE || inn.addrsize != 8 {
                continue;
            }
            for k in 0..inn.nops as usize {
                let mem = &inn.ops[k];
                if mem.kind != KMEM || mem.riprel != 0 || mem.base as c_uint != best as c_uint {
                    continue;
                }
                if mem.index as c_uint == OCERZ_REG_NONE || pin_slot(mem.index as c_uint) < 0 {
                    continue;
                }
                if rsp_is_ptr() != 0 && mem.index as c_uint == OCERZ_RSP {
                    continue;
                }
                icnt[(mem.index & 15) as usize][(mem.scale & 3) as usize] += 1;
                ilast[(mem.index & 15) as usize][(mem.scale & 3) as usize] = i as c_int;
            }
        }
        let (mut bi, mut bs, mut bc) = (-1 as c_int, 0 as c_int, 0 as c_int);
        for r in 0..16 {
            for sc in 0..4 {
                if icnt[r][sc] > bc {
                    bc = icnt[r][sc];
                    bi = r as c_int;
                    bs = sc as c_int;
                }
            }
        }
        if bi >= 0 && bc >= 2 && bi != best {
            let mut written = 0;
            let mut i = 0;
            while i < ilast[bi as usize][bs as usize] && written == 0 {
                written = insn_may_write_gpr(insns.add(i as usize), bi as c_uint);
                i += 1;
            }
            if written == 0 {
                g_mem_hoist_aux_index = bi;
                g_mem_hoist_aux_scale = bs;
                g_mem_hoist_aux_disp = 0;
            }
        }
    }
    if env_on!("OCERZ_HOISTLOG") {
        libc::fprintf(crate::log::stderr(),
            c"HOIST rip=%#llx base=%d(n=%d) aux=%d second=%d(n=%d) third=%d(n=%d)\n".as_ptr(),
            rip as libc::c_ulonglong, best, bestn, aux[best as usize], second, secondn, third, thirdn);
    }
    best
}

static mut g_ic_kind: [u8; JIT_MAX_BLOCK_INSNS as usize] = [0; JIT_MAX_BLOCK_INSNS as usize];
static mut g_ic_expect: [u64; JIT_MAX_BLOCK_INSNS as usize] = [0; JIT_MAX_BLOCK_INSNS as usize];
static mut g_ic_pushelide: [u8; JIT_MAX_BLOCK_INSNS as usize] = [0; JIT_MAX_BLOCK_INSNS as usize];
static mut g_ic_pair_rj: [i32; JIT_MAX_BLOCK_INSNS as usize] = [0; JIT_MAX_BLOCK_INSNS as usize];
static mut g_promo_reg: [u8; JIT_MAX_BLOCK_INSNS as usize] = [0; JIT_MAX_BLOCK_INSNS as usize];
static mut g_promo_mate: [i32; JIT_MAX_BLOCK_INSNS as usize] = [0; JIT_MAX_BLOCK_INSNS as usize];
static mut g_promo_push_of: [i32; JIT_MAX_BLOCK_INSNS as usize] = [0; JIT_MAX_BLOCK_INSNS as usize];
static mut g_promo_seq: [u64; JIT_MAX_BLOCK_INSNS as usize] = [0; JIT_MAX_BLOCK_INSNS as usize];



unsafe fn ic_kind(i: c_int) -> u8 {
    *(&raw const g_ic_kind).cast::<u8>().add(i as usize)
}
unsafe fn promo_reg(i: c_int) -> u8 {
    *(&raw const g_promo_reg).cast::<u8>().add(i as usize)
}

unsafe fn stack_pair_reg(inn: &X86Insn, op: c_uint) -> c_int {
    let o = &inn.ops[0];
    if inn.op as c_uint != op || inn.mode32 != 0 || inn.opsize != 8 || inn.seg != SEG_NONE || inn.nops != 1 {
        return -1;
    }
    if o.kind != KREG || o.high8 != 0 || o.size != 8 || (o.reg & 15) as c_uint == OCERZ_RSP {
        return -1;
    }
    let s = pin_slot(o.reg as c_uint);
    if s < 0 { -1 } else { pin_hreg(s) }
}

unsafe fn emit_stack_pair(b: *mut A64Buf, a: &X86Insn, c: &X86Insn, i: c_int) -> c_int {
    if g_lowstack == 0 || stack_plain_access_ok() == 0 || env_on!("OCERZ_NO_STACK_PAIR") {
        return 0;
    }
    if ic_kind(i) != 0 || ic_kind(i + 1) != 0 || promo_reg(i) != 0 || promo_reg(i + 1) != 0 {
        return 0;
    }
    let hs = pin_hreg(pin_slot(OCERZ_RSP));
    let ra;
    let rc;
    let pa = stack_pair_reg(a, OCERZ_OP_PUSH);
    let pc = if pa >= 0 { stack_pair_reg(c, OCERZ_OP_PUSH) } else { -1 };
    if pa >= 0 && pc >= 0 {
        ra = pa;
        rc = pc;
        if mem_native_store_ok() == 0 {
            return 0;
        }
        a64_add_reg(b, 1, JTA, hs, JGB, 0);
        a64_stp_off(b, rc, ra, JTA, -16);
        a64_sub_imm(b, 1, hs, hs, 16);
    } else {
        let qa = stack_pair_reg(a, OCERZ_OP_POP);
        let qc = if qa >= 0 { stack_pair_reg(c, OCERZ_OP_POP) } else { -1 };
        if qa >= 0 && qc >= 0 {
            ra = qa;
            rc = qc;
            if ra == rc {
                return 0;
            }
            a64_add_reg(b, 1, JTA, hs, JGB, 0);
            a64_ldp_off(b, ra, rc, JTA, 0);
            a64_add_imm(b, 1, hs, hs, 16);
        } else {
            return 0;
        }
    }
    *(&raw mut g_mov_skip).cast::<u8>().add(i as usize + 1) = 1;
    1
}

unsafe fn low_splice_ok(insn: &X86Insn) -> c_int {
    static mut OFF: c_int = -1;
    if OFF < 0 {
        OFF = (!libc::getenv(c"OCERZ_NO_LOW_SPLICE".as_ptr()).is_null()) as c_int;
    }
    (OFF == 0 && insn.mode32 == 0 && g_pin_class == 3 && pin_slot(OCERZ_RSP) >= 0 && rsp_is_ptr() != 0
        && mem_native_store_ok() != 0) as c_int
}

unsafe fn rsp_run_member(insns: *const X86Insn, j: c_int, n: c_int, fast3: c_int) -> c_int {
    if j >= n {
        return 0;
    }
    if promo_reg(j) != 0 && (*insns.add(j as usize)).op as c_uint == OCERZ_OP_POP {
        return 1;
    }
    if ic_kind(j) == 3 && fast3 != 0 {
        return 1;
    }
    0
}

unsafe fn inline_calls_off() -> c_int {
    static mut OFF: c_int = -1;
    if OFF < 0 {
        OFF = (!libc::getenv(c"OCERZ_NO_INLINE_CALL".as_ptr()).is_null()) as c_int;
    }
    OFF
}

unsafe fn splice_callee(target: u64, ret_rip: u64, self_rip: u64, scratch: *mut X86Insn, vn: *mut c_int, depth: c_int) -> c_int {
    if inline_calls_off() != 0 || target == self_rip || depth > 2 {
        return 0;
    }
    let start = *vn;
    let mut pc = target;
    'ok: {
        for _steps in 0..24 {
            if *vn + 2 >= JIT_MAX_BLOCK_INSNS as c_int {
                break 'ok;
            }
            let inn = scratch.add(*vn as usize);
            if jit_decode(pc, inn, 0) != OCERZ_OK as c_int {
                break 'ok;
            }
            let op = (*inn).op as c_uint;
            if op == OCERZ_OP_RET {
                if (*inn).nops != 0 {
                    break 'ok;
                }
                ga!(g_ic_kind, *vn) = 2u8;
                ga!(g_ic_expect, *vn) = ret_rip;
                *vn += 1;
                return 1;
            }
            if op == OCERZ_OP_CALL && (*inn).ops[0].kind == KIMM && (*inn).mode32 == 0 {
                let at = *vn;
                ga!(g_ic_kind, at) = 1u8;
                *vn += 1;
                if splice_callee((*inn).ops[0].imm, pc + (*inn).len as u64, self_rip, scratch, vn, depth + 1) == 0 {
                    ga!(g_ic_kind, at) = 0u8;
                    break 'ok;
                }
                pc += (*inn).len as u64;
                continue;
            }
            if is_terminator(op) != 0 || op == OCERZ_OP_JCC || op == OCERZ_OP_SYSCALL || op == OCERZ_OP_FXSAVE
                || op == OCERZ_OP_FXRSTOR || op == OCERZ_OP_XSAVE || op == OCERZ_OP_XRSTOR
            {
                break 'ok;
            }
            *vn += 1;
            pc += (*inn).len as u64;
        }
    }
    for k in start..*vn {
        ga!(g_ic_kind, k) = 0u8;
    }
    *vn = start;
    0
}

unsafe fn ret_flags_live() -> c_int {
    static mut V: c_int = -1;
    if V < 0 {
        V = (!libc::getenv(c"OCERZ_RET_FLAGS_LIVE".as_ptr()).is_null()) as c_int;
    }
    V
}

unsafe fn ret_flags_live_at(rip: u64) -> c_int {
    static mut HAVE: c_int = -1;
    static mut LO: u64 = 0;
    static mut HI: u64 = 0;
    if HAVE < 0 {
        let a = libc::getenv(c"OCERZ_RETFL_LO".as_ptr());
        let b = libc::getenv(c"OCERZ_RETFL_HI".as_ptr());
        HAVE = (!a.is_null() && !b.is_null()) as c_int;
        if HAVE != 0 {
            LO = libc::strtoull(a, null_mut(), 0) as u64;
            HI = libc::strtoull(b, null_mut(), 0) as u64;
        }
    }
    if HAVE != 0 {
        return (rip >= LO && rip < HI) as c_int;
    }
    ret_flags_live()
}

unsafe fn ret_seam_live(insns: *const X86Insn, n: c_int) -> u64 {
    let mut i = n - 2;
    while i >= 0 {
        let mut def: u64 = 0;
        let mut usev: u64 = 0;
        ocerz_flags_defuse(insns.add(i as usize), &mut def, &mut usev);
        if def & JIT_ARITH_FLAGS == 0 {
            i -= 1;
            continue;
        }
        return match (*insns.add(i as usize)).op as c_uint {
            OCERZ_OP_CMP | OCERZ_OP_TEST | OCERZ_OP_BT | OCERZ_OP_BTS | OCERZ_OP_BTR | OCERZ_OP_BTC
            | OCERZ_OP_CMPXCHG => OCERZ_FL_ALL as u64,
            _ => 0,
        };
    }
    OCERZ_FL_ALL as u64
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jit_interp_block(vm: *mut OcerzVM, cpu: *mut OcerzCPU, b: *mut JitBlock) -> c_int {
    static mut LG: c_int = -1;
    if LG < 0 {
        LG = (!libc::getenv(c"OCERZ_IBLOG".as_ptr()).is_null()) as c_int;
    }
    if LG != 0 {
        libc::fprintf(crate::log::stderr(), c"ocerz: INTERP-BLOCK rip=%#llx n=%d\n".as_ptr(),
            blk_rip(b) as libc::c_ulonglong, (*b).n_insns);
    }
    if (*b).insns.is_null() {
        return OCERZ_EUNSUP as c_int;
    }
    ocerz_jit_exec_state = 2;
    ocerz_flags_materialize(cpu);
    if ocerz_perfstat > 0 {
        (*b).exec_count += 1;
    }
    for i in 0..(*b).n_insns as usize {
        let inn = (*b).insns.add(i);
        let r = ocerz_jit_exec_one(vm, cpu, inn);
        if r != OCERZ_STEP_OK as c_int {
            ocerz_jit_exec_state = 0;
            return r;
        }
        if (*cpu).rip != (*inn).rip + (*inn).len as u64 || (*cpu).interp_once != 0 {
            ocerz_jit_exec_state = 0;
            return OCERZ_STEP_OK as c_int;
        }
    }
    ocerz_jit_exec_state = 0;
    OCERZ_STEP_OK as c_int
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jit_core_decode_reset() {
    core::ptr::write_bytes(&raw mut g_ic_kind, 0, 1);
    core::ptr::write_bytes(&raw mut g_ic_pushelide, 0, 1);
    core::ptr::write_bytes(&raw mut g_promo_reg, 0, 1);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jit_core_ic_kind_set(at: c_int, kind: c_int) {
    ga!(g_ic_kind, at) = kind as u8;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jit_core_splice_callee(target: u64, ret_rip: u64, self_rip: u64, scratch: *mut X86Insn, vn: *mut c_int, depth: c_int) -> c_int {
    splice_callee(target, ret_rip, self_rip, scratch, vn, depth)
}

unsafe extern "C" {
    fn jit_core_decode(rip: u64, mode32: c_int, scratch: *mut X86Insn, pc_out: *mut u64) -> c_int;
}

#[inline(always)]
unsafe fn now_ns() -> u64 {
    clock_gettime_nsec_np(libc::CLOCK_UPTIME_RAW)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn translate(jit: *mut OcerzJit, rip: u64, mode32: c_int) -> *mut JitBlock {
    g_xlat_mode32 = mode32;

    if ocerz_exc_trap_rip != 0 && rip == ocerz_exc_trap_rip {
        return null_mut();
    }
    if ocerz_cxa_throw_rip != 0 && rip == ocerz_cxa_throw_rip {
        return null_mut();
    }

    if churn_blacklisted(rip) != 0 {
        churn_note_refusal(rip);
        static REFN: AtomicU64 = AtomicU64::new(0);
        static mut CLOG: c_int = -1;
        if CLOG < 0 {
            CLOG = if libc::getenv(c"OCERZ_CHURNLOG".as_ptr()).is_null() { 0 } else { 1 };
        }
        if CLOG != 0 {
            let r = REFN.fetch_add(1, Ordering::SeqCst) + 1;
            if (r & 0xfff) == 0 {
                libc::fprintf(crate::log::stderr(), c"ocerz: CHURNREF[%d] n=%llu rip=%#llx\n".as_ptr(),
                    libc::getpid() as c_int, r as libc::c_ulonglong, rip as libc::c_ulonglong);
            }
        }
        return null_mut();
    }

    {
        static mut ILO: u64 = 0;
        static mut IHI: u64 = 0;
        static mut ILO2: u64 = 0;
        static mut IHI2: u64 = 0;
        static mut IRNG: c_int = -1;
        if IRNG < 0 {
            let l = libc::getenv(c"OCERZ_INTERP_LO".as_ptr());
            let h = libc::getenv(c"OCERZ_INTERP_HI".as_ptr());
            if !l.is_null() && !h.is_null() {
                ILO = libc::strtoull(l, null_mut(), 0) as u64;
                IHI = libc::strtoull(h, null_mut(), 0) as u64;
            }
            let l2 = libc::getenv(c"OCERZ_INTERP_LO2".as_ptr());
            let h2 = libc::getenv(c"OCERZ_INTERP_HI2".as_ptr());
            if !l2.is_null() && !h2.is_null() {
                ILO2 = libc::strtoull(l2, null_mut(), 0) as u64;
                IHI2 = libc::strtoull(h2, null_mut(), 0) as u64;
            }
            IRNG = (ILO < IHI || ILO2 < IHI2) as c_int;
        }
        if IRNG != 0 && ((rip >= ILO && rip < IHI) || (rip >= ILO2 && rip < IHI2)) {
            return null_mut();
        }
    }
    {
        static mut IRIPS: [u64; 8] = [0; 8];
        static mut N_IRIPS: c_int = -1;
        if N_IRIPS < 0 {
            let mut e = libc::getenv(c"OCERZ_INTERP_RIP".as_ptr()) as *const c_char;
            N_IRIPS = 0;
            while !e.is_null() && *e != 0 && N_IRIPS < 8 {
                ga!(IRIPS, N_IRIPS) = libc::strtoull(e, null_mut(), 0) as u64;
                N_IRIPS += 1;
                e = libc::strchr(e, b',' as c_int);
                if !e.is_null() {
                    e = e.add(1);
                }
            }
        }
        for i in 0..N_IRIPS {
            if ga!(IRIPS, i) == rip {
                return null_mut();
            }
        }
    }

    let mut tc_hit: *const OcerzTcRecHead = null();
    let tcm = ocerz_tcache_mode();
    g_tc_rec = 0;
    g_tc_bad = 0;
    g_tc_learned = 0;
    g_tc_nrel = 0;
    g_tc_ndlog = 0;
    g_tc_nbytes = 0;
    g_tc_dbar = 0;
    if tcm != OCERZ_TC_OFF as c_int {
        tc_log_init();
    }
    if (tcm == OCERZ_TC_ON as c_int || tcm == OCERZ_TC_VERIFY as c_int) && tc_usable(jit) != 0 && tc_keepable(rip) != 0 {
        g_tc_rec = 1;
        g_tc_key = tc_key(rip, mode32);
        let jk = jit_key(rip, mode32);
        if al_marked(jk) == 0 && al_marked(jk | AL_BLK_TAG) == 0 && cp_marked(jk) == 0 && tc_noload_has(jk) == 0 {
            tc_hit = ocerz_tcache_find(g_tc_key);
        }
        {
            static mut TR: c_int = -1;
            if TR < 0 {
                TR = if libc::getenv(c"OCERZ_TCACHE_TRACE".as_ptr()).is_null() { 0 } else { 1 };
            }
            if TR != 0 && g_tc_log > 0 {
                libc::fprintf(g_tc_lf.cast(), c"ocerz: TCACHE[%d] %s key=%#llx\n".as_ptr(), libc::getpid() as c_int,
                    if tc_hit.is_null() { c"MISS".as_ptr() } else { c"HIT".as_ptr() }, g_tc_key as libc::c_ulonglong);
            }
        }
        if !tc_hit.is_null() && tcm == OCERZ_TC_ON as c_int {
            let t0 = if ocerz_jit_time_xlat != 0 { now_ns() } else { 0 };
            let lb = tc_load(jit, tc_hit, rip, mode32);
            if ocerz_jit_time_xlat != 0 {
                AtomicU64::from_ptr(&raw mut ocerz_jit_xlat_ns).fetch_add(now_ns() - t0, Ordering::Relaxed);
            }
            if !lb.is_null() {
                g_tc_rec = 0;
                return lb;
            }
        }
    }

    static mut G_JITMEASURE: c_int = -1;
    if G_JITMEASURE < 0 {
        G_JITMEASURE = if libc::getenv(c"OCERZ_JITMEASURE".as_ptr()).is_null() { 0 } else { 1 };
    }
    let xlat_t0 = if G_JITMEASURE != 0 || ocerz_jit_time_xlat != 0 { now_ns() } else { 0 };
    let mut scratch_mem = MaybeUninit::<[X86Insn; JIT_MAX_BLOCK_INSNS as usize]>::uninit();
    let scratch = scratch_mem.as_mut_ptr().cast::<X86Insn>();
    let mut pc: u64 = rip;
    let n: c_int = jit_core_decode(rip, mode32, scratch, &mut pc);
    g_xlat_n = n;
    if ocerz_jitstat > 0 {
        js_decoded_insns = js_decoded_insns.wrapping_add(n as u64);
    }
    if n == 0 {
        if ocerz_jitstat > 0 {
            js_xlat_fail += 1;
            js_fail_decode0 += 1;
            js_note_fail(rip, JSR_DECODE0 as c_uint, 0);
        }
        return null_mut();
    }

    let blk = libc::calloc(1, size_of::<JitBlock>()) as *mut JitBlock;
    if blk.is_null() {
        if ocerz_jitstat > 0 {
            js_xlat_fail += 1;
            js_fail_alloc += 1;
            js_note_fail(rip, JSR_ALLOC as c_uint, n);
        }
        return null_mut();
    }
    (*blk).insns = libc::malloc(n as usize * size_of::<X86Insn>()) as *mut X86Insn;
    if (*blk).insns.is_null() {
        libc::free(blk.cast());
        if ocerz_jitstat > 0 {
            js_xlat_fail += 1;
            js_fail_alloc += 1;
            js_note_fail(rip, JSR_ALLOC as c_uint, n);
        }
        return null_mut();
    }
    core::ptr::copy_nonoverlapping(scratch, (*blk).insns, n as usize);
    (*blk).n_insns = n as _;
    (*blk).key = jit_key(rip, mode32);
    (*blk).edges = libc::calloc(JIT_MAX_EDGES as usize, pointee_size((*blk).edges)).cast();
    if (*blk).edges.is_null() {
        libc::free((*blk).insns.cast());
        libc::free(blk.cast());
        if ocerz_jitstat > 0 {
            js_xlat_fail += 1;
            js_fail_alloc += 1;
            js_note_fail(rip, JSR_ALLOC as c_uint, n);
        }
        return null_mut();
    }
    if g_keep_cap < n {
        libc::free(g_keep.cast());
        g_keep = libc::malloc(n as usize) as *mut u8;
        g_keep_cap = if g_keep.is_null() { 0 } else { n };
    }
    if !g_keep.is_null() {
        core::ptr::write_bytes(g_keep, 0, n as usize);
    }
    g_keep_n = if g_keep.is_null() { 0 } else { n };
    g_cur_blk = blk;
    g_no_compact = 0;

    for g in 0..16 {
        (*blk).guest_in_host[g] = -1;
    }
    (*blk).n_pinned = 0;
    g_pin = null_mut();
    g_pin_hold = null_mut();
    g_n_pinned = 0;
    g_pin_class = 0;
    g_lowstack = 0;
    g_m32low = 0;

    g_defer = (g_no_regflags == 0) as c_int;
    (*blk).n_edges = 0;
    g_chain_target = 0;
    g_chain_keeps_jgb = 0;
    g_n_raslit = 0;
    g_tc_on = 0;
    g_tc_entry = null_mut();
    g_tc_pool_off = u32::MAX;
    g_chain_epi = null_mut();
    g_n_jcc_edges = 0;
    g_nzcv_want = 0;
    g_nzcv_from = -1;
    ga!(g_jcc_edge, 0).cond_site = null_mut();
    ga!(g_jcc_edge, 1).cond_site = null_mut();
    g_n_oslow = 0;
    g_n_garm = 0;
    g_n_nanool = 0;
    g_n_pe_real = 0;
    g_n_promo_real = 0;
    g_rsp_lag = 0;
    g_pe_insns = (*blk).insns;
    g_n_call_edges = 0;
    g_n_oolslow = 0;
    x87_reset();
    g_x87_btop = g_xlat_ftop;
    if g_x87_btop >= 0 && mark_has(&raw mut g_x87spec_marks, jit_key(rip, mode32)) != 0 {
        g_x87_btop = -1;
        g_tc_learned = 1;
    }
    g_x87_spec = -1;
    g_x87_kcarry = 0;
    g_x87_spec_cut = 0;
    g_oolslow_pre = 0;
    g_n_stop_extra = 0;
    g_xlat_jit = jit;
    g_self_rip = rip;
    g_body_entry = null_mut();
    g_loop_entry = null_mut();
    g_l0_fixed = 0;
    g_lane_used = 0;
    g_l0_next = 0;
    g_n_undo_lanes = 0;
    g_l0_nlanes = L0_NLANES as _;
    g_zero_vreg = -1;
    core::ptr::write_bytes(&raw mut g_yc, 0xff, 1);
    g_yc_dirty = 0;
    core::ptr::write_bytes(&raw mut g_l0_fixed_lane, 0xff, 1);
    {
        static mut NOZERO: c_int = -1;
        if NOZERO < 0 {
            NOZERO = if libc::getenv(c"OCERZ_NO_ZEROREG".as_ptr()).is_null() { 0 } else { 1 };
        }
        let mut nv = 0;
        g_blk_ymm_write = 0;
        for i in 0..n as usize {
            let inn = &*(*blk).insns.add(i);
            if inn.vex != 0 && (inn.vex as c_uint & OCERZ_VEX_L as c_uint) == 0 && inn.mode32 == 0 && inn.nops > 0
                && inn.ops[0].kind == KXMM
            {
                nv += 1;
            }
            if inn.vex != 0 && (inn.vex as c_uint & OCERZ_VEX_L as c_uint) != 0 && inn.op as c_uint != OCERZ_OP_VZEROUPPER {
                g_blk_ymm_write = 1;
            }
        }
        if NOZERO == 0 && nv >= 2 && g_xlat_mode32 == 0 {
            g_zero_vreg = lane_reserve();
        }
    }
    g_x87_lanes_on = 0;
    g_x87_lv = 0;
    for p in 0..8 {
        (*(&raw mut g_x87_lane))[p] = -1;
    }
    if !env_on!("OCERZ_NO_X87_LANES") {
        let mut nx = 0;
        let mut sse = 0;
        for i in 0..n as usize {
            let inn = (*blk).insns.add(i);
            if x87_inline_ok(inn) != 0 {
                nx += 1;
            }
            for k in 0..(*inn).nops as usize {
                if (*inn).ops[k].kind == KXMM || (*inn).ops[k].kind == KMMX {
                    sse = 1;
                }
            }
        }
        if nx >= 2 && sse == 0 {
            let mut got = 0;
            for p in 0..8 {
                let v = lane_reserve();
                if v < 0 {
                    break;
                }
                (*(&raw mut g_x87_lane))[p] = v as i8;
                got += 1;
            }
            g_x87_lanes_on = (got == 8) as c_int;
        }
    }
    g_stop_patch = null_mut();
    g_n_stop_extra = 0;
    g_n_push_fix = 0;
    g_n_oolslow = 0;
    g_cp_guard = (ocerz_commpage as u64 != 0
        && (env_on!("OCERZ_CP_GUARD_ALL") || cp_marked(jit_key(rip, mode32)) != 0)) as c_int;
    g_low_top = (ocerz_low_base as u64 != 0
        && (env_on!("OCERZ_LOW_TOP_GUARD") || cp_marked(jit_key(rip, mode32)) != 0)) as c_int;
    {
        static mut ALL: c_int = -1;
        if ALL < 0 {
            ALL = if libc::getenv(c"OCERZ_AL_GUARD_ALL".as_ptr()).is_null() { 0 } else { 1 };
        }
        if ALL != 0 {
            g_al_all = 1;
        }
    }
    g_align_blk = (g_plain_mem == 0 && al_marked(jit_key(rip, mode32) | AL_BLK_TAG) != 0) as c_int;
    g_align_any = g_align_blk;
    let mut i = 0;
    while i < n && g_align_any == 0 && g_plain_mem == 0 && g_al_n != 0 {
        g_align_any = al_marked(jit_key((*(*blk).insns.add(i as usize)).rip, mode32));
        i += 1;
    }
    g_align_guard = g_align_blk;
    if g_cp_guard != 0 || g_align_any != 0 {
        g_tc_learned = 1;
    }
    g_blk_ordered_loads = 0;
    g_push_entry = null_mut();
    g_n_side = 0;
    g_stop_target = null_mut();
    g_mem_hoist_greg = -1;
    g_low_hoist_greg = -1;
    g_n_low_hoist_bail = 0;
    g_mem_hoist_greg2 = -1;
    g_mem_hoist_greg3 = -1;
    g_mem_hoist_aux_index = -1;
    g_mem_hoist_aux_disp = 0;
    let ins = (*blk).insns;
    let fuse_cmp = (n >= 2 && can_fuse_cmp_test_jcc(ins.add(n as usize - 2), ins.add(n as usize - 1), rip) != 0) as c_int;
    let fuse_incdec = (n >= 2 && can_fuse_incdec_jcc(ins.add(n as usize - 2), ins.add(n as usize - 1)) != 0) as c_int;
    let fuse_pair = (fuse_cmp != 0 || fuse_incdec != 0) as c_int;
    let fuse_self = (fuse_cmp != 0 && (*ins.add(n as usize - 1)).ops[0].imm == rip) as c_int;

    static mut G_PIN_MIN: c_int = -1;
    if G_PIN_MIN < 0 {
        let e = libc::getenv(c"OCERZ_PIN_MIN_INSNS".as_ptr());
        G_PIN_MIN = if e.is_null() { 24 } else { libc::strtol(e, null_mut(), 0) as c_int };
        if G_PIN_MIN < 1 {
            G_PIN_MIN = 1;
        }
    }
    let term = &*ins.add(n as usize - 1);
    let top = term.op as c_uint;
    let mut call_region = (g_no_regflags == 0 && ocerz_low_base as u64 == 0 && mode32 == 0
        && (top == OCERZ_OP_CALL || top == OCERZ_OP_RET)) as c_int;
    if call_region != 0 && top == OCERZ_OP_CALL {
        call_region = (term.ops[0].kind == KIMM && decoded_call_region_entry(term.ops[0].imm) != 0) as c_int;
    }
    if call_region != 0 {
        let mut i = 0;
        while i < n - 1 && call_region != 0 {
            let inn = &*ins.add(i as usize);
            for k in 0..inn.nops as usize {
                let o = &inn.ops[k];
                if (o.kind == KREG && (o.reg & 15) as c_uint == OCERZ_RSP)
                    || (o.kind == KMEM
                        && ((o.base as c_uint != OCERZ_REG_NONE && (o.base & 15) as c_uint == OCERZ_RSP)
                            || (o.index as c_uint != OCERZ_REG_NONE && (o.index & 15) as c_uint == OCERZ_RSP)))
                {
                    call_region = 0;
                    break;
                }
            }
            i += 1;
        }
    }
    if call_region == 0 && g_no_regflags == 0 && top == OCERZ_OP_JCC && term.ops[0].kind == KIMM {
        let taken = term.ops[0].imm;
        let fall = term.rip + term.len as u64;
        call_region = (call_body_successor(taken) != 0 && call_body_successor(fall) != 0) as c_int;
        let mut i = 0;
        while i < n - 1 && call_region != 0 {
            let inn = &*ins.add(i as usize);
            for k in 0..inn.nops as usize {
                let o = &inn.ops[k];
                let rsp = (o.kind == KREG && (o.reg & 15) as c_uint == OCERZ_RSP)
                    || (o.kind == KMEM
                        && ((o.base as c_uint != OCERZ_REG_NONE && (o.base & 15) as c_uint == OCERZ_RSP)
                            || (o.index as c_uint != OCERZ_REG_NONE && (o.index & 15) as c_uint == OCERZ_RSP)));
                if rsp && !(inn.op as c_uint == OCERZ_OP_MOV && k == 1 && o.kind == KREG) {
                    call_region = 0;
                    break;
                }
            }
            i += 1;
        }
    }

    let indirect_jmp_term = (top == OCERZ_OP_JMP && term.ops[0].kind != KIMM && term.seg == SEG_NONE) as c_int;
    let mut fixed_region = (call_region == 0 && g_no_regflags == 0
        && (top == OCERZ_OP_JCC || (top == OCERZ_OP_JMP && term.ops[0].kind == KIMM) || indirect_jmp_term != 0)) as c_int;
    let full_pin = (fullpin_enabled() != 0 && g_no_regflags == 0) as c_int;
    if full_pin != 0 {
        call_region = 0;
        fixed_region = 0;
        for i in 0..16 {
            (*blk).host_holds[i] = i as u8;
            (*blk).guest_in_host[i] = i as i8;
        }
        (*blk).n_pinned = 16;
        (*blk).pin_class = 3;
        g_pin = (*blk).guest_in_host.as_mut_ptr();
        g_pin_hold = (*blk).host_holds.as_mut_ptr();
        g_n_pinned = 16;
        g_pin_class = 3;
    } else if call_region != 0 {
        const CALL_GPR: [u8; 6] = [OCERZ_RAX as u8, OCERZ_RBX as u8, OCERZ_RSP as u8, OCERZ_RBP as u8, OCERZ_R14 as u8, OCERZ_RDI as u8];
        for i in 0..6 {
            (*blk).host_holds[i] = CALL_GPR[i];
            (*blk).guest_in_host[CALL_GPR[i] as usize] = i as i8;
        }
        (*blk).n_pinned = 6;
        (*blk).pin_class = 2;
        g_pin = (*blk).guest_in_host.as_mut_ptr();
        g_pin_hold = (*blk).host_holds.as_mut_ptr();
        g_n_pinned = 6;
        g_pin_class = 2;
    } else if fixed_region != 0 && indirect_jmp_term == 0 {
        let target = term.ops[0].imm;
        fixed_region = canonical_body_successor(target);
        if top == OCERZ_OP_JCC {
            fixed_region |= canonical_body_successor(term.rip + term.len as u64);
        }
    }
    if fixed_region != 0 {
        const FIXED_GPR: [u8; 8] = [OCERZ_RAX as u8, OCERZ_RCX as u8, OCERZ_RDX as u8, OCERZ_RBX as u8,
            OCERZ_RSI as u8, OCERZ_RDI as u8, OCERZ_R8 as u8, OCERZ_R9 as u8];
        for i in 0..8 {
            (*blk).host_holds[i] = FIXED_GPR[i];
            (*blk).guest_in_host[FIXED_GPR[i] as usize] = i as i8;
        }
        (*blk).n_pinned = 8;
        (*blk).pin_class = 1;
        g_pin = (*blk).guest_in_host.as_mut_ptr();
        g_pin_hold = (*blk).host_holds.as_mut_ptr();
        g_n_pinned = 8;
        g_pin_class = 1;
    } else if full_pin == 0 && call_region == 0 && g_no_regflags == 0 && (n >= G_PIN_MIN || fuse_self != 0) {
        let mut cnt = [0 as c_int; 16];
        for i in 0..n as usize {
            let inn = &*ins.add(i);
            for k in 0..inn.nops as usize {
                let o = &inn.ops[k];
                if o.kind == KREG {
                    cnt[(o.reg & 15) as usize] += 1;
                } else if o.kind == KMEM {
                    if o.base as c_uint != OCERZ_REG_NONE {
                        cnt[(o.base & 15) as usize] += 1;
                    }
                    if o.index as c_uint != OCERZ_REG_NONE {
                        cnt[(o.index & 15) as usize] += 1;
                    }
                }
            }
        }
        cnt[OCERZ_RSP as usize] = 0;
        let mut np: c_int = 0;
        while np < 8 {
            let mut best: c_int = -1;
            for g in 0..16 as c_int {
                if cnt[g as usize] > 0 && (best < 0 || cnt[g as usize] > cnt[best as usize]) {
                    best = g;
                }
            }
            if best < 0 {
                break;
            }
            (*blk).host_holds[np as usize] = best as u8;
            (*blk).guest_in_host[best as usize] = np as i8;
            cnt[best as usize] = 0;
            np += 1;
        }
        (*blk).n_pinned = np as u8;
        if np > 0 {
            g_pin = (*blk).guest_in_host.as_mut_ptr();
            g_pin_hold = (*blk).host_holds.as_mut_ptr();
            g_n_pinned = np;
        }
    }

    g_mem_hoist_greg = select_mem_base_hoist(ins, n, rip);
    g_low_hoist_greg = select_low_hoist(ins, n, rip);
    g_n_low_hoist_bail = 0;

    g_xmm_pinned = 0;
    if xmm_pinning_enabled() != 0 && sse_enabled() != 0 && xmm_global_enabled() != 0 && g_no_regflags == 0 {
        g_xmm_pinned = 0xffff;
    } else if xmm_pinning_enabled() != 0 && sse_enabled() != 0 {
        for i in 0..n as usize {
            let inn = &*ins.add(i);
            for k in 0..inn.nops as usize {
                if inn.ops[k].kind == KXMM && inn.ops[k].reg < 16 {
                    g_xmm_pinned |= (1u32 << inn.ops[k].reg) as u16;
                }
            }
            let op = inn.op as c_uint;
            if op == OCERZ_OP_BLENDVPD || op == OCERZ_OP_BLENDVPS || op == OCERZ_OP_PBLENDVB {
                g_xmm_pinned |= 1;
            }
            if (inn.vex as c_uint & OCERZ_VEX_NDS as c_uint) != 0 {
                g_xmm_pinned |= (1u32 << (inn.vvvv & 15)) as u16;
            }
        }
    }
    (*blk).xmm_pinned = g_xmm_pinned;
    g_pk_consts_needed = 0;
    let mut i = 0;
    while i < n && sse_enabled() != 0 {
        match (*ins.add(i as usize)).op as c_uint {
            OCERZ_OP_ADDPS | OCERZ_OP_ADDPD | OCERZ_OP_SUBPS | OCERZ_OP_SUBPD | OCERZ_OP_MULPS | OCERZ_OP_MULPD
            | OCERZ_OP_DIVPS | OCERZ_OP_DIVPD | OCERZ_OP_SQRTPS | OCERZ_OP_SQRTPD => g_pk_consts_needed = 1,
            _ => {}
        }
        i += 1;
    }
    pthread_jit_write_protect_np(0);
    if !env_on!("OCERZ_NO_DISPATCH_STUB") {
        if mode32 != 0 {
            if (*jit).dispatch_stub32.is_null() {
                emit_dispatch_stub(jit, 1);
            }
        } else if (*jit).dispatch_stub.is_null() {
            emit_dispatch_stub(jit, 0);
        }
    }
    veneer_pool_check(jit);
    if !tc_hit.is_null() && tcm == OCERZ_TC_VERIFY as c_int && (*tc_hit).size as usize >= size_of::<TcRec>() {
        let pp = (*jit).code_cur as *mut u8;
        let pad = ((*(tc_hit as *const TcRec)).entry_mod as usize).wrapping_sub(pp as usize) & 63;
        if (pp.wrapping_add(pad) as usize) < (*jit).code_end as usize {
            (*jit).code_cur = pp.add(pad) as *mut u32;
        }
    }
    #[cfg(ocerz_jit_emit_audit)]
    let audit_x64 = ocerz_jit_emit_audit_begin(jit);
    let mut b: A64Buf = core::mem::zeroed();
    b.start = (*jit).code_cur;
    b.p = (*jit).code_cur;
    b.end = (*jit).code_end;
    let bp: *mut A64Buf = &mut b;
    let entry = (*bp).p;
    g_push_entry = entry;
    g_tc_entry = entry;
    g_tc_on = if g_tc_rec != 0 || ocerz_tcache_mode() == OCERZ_TC_ROUNDTRIP as c_int { tc_usable(jit) } else { 0 };
    #[cfg(ocerz_jit_emit_audit)]
    if audit_x64 != 0 { g_tc_on = 1; }

    a64_stp_pre(bp, 29, 30, 31, -16);
    a64_stp_pre(bp, 19, 20, 31, -16);
    a64_mov_reg(bp, 1, 19, 0);
    a64_mov_reg(bp, 1, 20, 1);
    emit_reload_jgb(bp);

    emit_pin_prologue(bp);

    if rsp_is_ptr() != 0 && ocerz_guest_base != 0 {
        let rs = pin_slot(OCERZ_RSP);
        assert!(rs >= 0);
        a64_mov_imm64(bp, JT0, ocerz_guest_base);
        a64_add_reg(bp, 1, pin_hreg(rs), pin_hreg(rs), JT0, 0);
    }
    g_lowstack = lowstack_delta_ok();
    g_lowstack_from = 0;
    g_lowstack_check = (g_lowstack != 0 && env_on!("OCERZ_LOWSTACK_CHECK")) as c_int;
    if g_lowstack != 0 {
        emit_stack_delta(bp);
    }
    g_m32low = (g_lowstack == 0 && m32_lowreg_ok() != 0) as c_int;
    if g_m32low != 0 {
        a64_mov_imm64(bp, JGB, ocerz_low_base as u64);
    }

    if g_pin_class == 2 {
        a64_add_imm(bp, 1, 29, 31, 0);
    }
    if g_pin_class == 3 && host_ras_enabled() != 0 {
        a64_add_imm(bp, 1, JT0, 31, 0);
        a64_str(bp, 8, JT0, 20, JIT_FP_OFF as u32);
        a64_stp_pre(bp, 31, 31, 31, -16);
    }

    let mut loop_poll_exit: *mut u32 = null_mut();
    if xmm_global_enabled() != 0 {
        emit_xmm_pin_load_all(bp);
    }
    let mut body_noreload: *mut u32 = null_mut();
    if g_no_chain == 0 && (*jit).stop_requested == 0 {
        g_body_entry = a64_label(bp);
        emit_reload_mem_base(bp);
        body_noreload = a64_label(bp);
        {
            static mut BT: c_int = -1;
            if BT < 0 {
                BT = if libc::getenv(c"OCERZ_BTRACE".as_ptr()).is_null() { 0 } else { 1 };
            }
            if BT != 0 {
                a64_ldr(bp, 8, JTA, 20, offset_of!(OcerzCPU, btrace) as u32);
                a64_ldr(bp, 4, JT2, 20, offset_of!(OcerzCPU, btrace_n) as u32);
                a64_ldr(bp, 4, JTT, 20, offset_of!(OcerzCPU, btrace_mask) as u32);
                a64_and_reg(bp, 0, JTT, JT2, JTT, 0);
                a64_add_reg(bp, 1, JTA, JTA, JTT, 3);
                a64_mov_imm64(bp, JT0, rip);
                a64_str(bp, 8, JT0, JTA, 0);
                a64_add_imm(bp, 0, JT2, JT2, 1);
                a64_str(bp, 4, JT2, 20, offset_of!(OcerzCPU, btrace_n) as u32);
            }
        }
        if env_on!("OCERZ_JGB_CHECK") && jgb_usable() != 0 {
            a64_mov_imm64(bp, JTU, ocerz_guest_base);
            a64_subs_reg(bp, 1, A64_ZR as c_int, 0, JTU, 0);
            let okl = a64_label(bp);
            a64_bcond(bp, A64_EQ as c_int, 0);
            a64_mov_reg(bp, 1, 1, 0);
            a64_mov_imm64(bp, 0, rip);
            tc_imm64(bp, 16, TCR_SYM as c_int, TCS_JGB_TRAP as u64, (ocerz_jgb_trap as *const ()) as usize as u64);
            a64_blr(bp, 16);
            a64_patch_bcond(okl, a64_label(bp));
        }
        if xmm_global_enabled() == 0 {
            emit_xmm_pin_load_all(bp);
        }
        {
            static mut LA: c_int = -1;
            if LA < 0 {
                let e = libc::getenv(c"OCERZ_LOOP_ALIGN".as_ptr());
                LA = if e.is_null() { 32 } else { libc::strtol(e, null_mut(), 0) as c_int };
            }
            let mut self_loop = 0;
            {
                let t = &*ins.add(n as usize - 1);
                if (t.op as c_uint == OCERZ_OP_JCC || t.op as c_uint == OCERZ_OP_JMP) && t.nops == 1 && t.ops[0].kind == KIMM
                    && t.ops[0].imm == rip
                {
                    self_loop = 1;
                }
            }
            if LA > 4 && !g_body_entry.is_null() && self_loop != 0 {
                let k = (*bp).p.offset_from(g_body_entry) as usize;
                let la = LA as usize;
                let mut pad = (la.wrapping_sub((*bp).p as usize & (la - 1))) & (la - 1);
                pad /= 4;
                if pad != 0 && ((*bp).p.wrapping_add(pad) as usize) < (*bp).end as usize {
                    core::ptr::copy(g_body_entry, g_body_entry.add(pad), k);
                    for q in 0..pad {
                        *g_body_entry.add(q) = 0xd503201f;
                    }
                    g_body_entry = g_body_entry.add(pad);
                    if !body_noreload.is_null() {
                        body_noreload = body_noreload.add(pad);
                    }
                    (*bp).p = (*bp).p.add(pad);
                }
            }
        }
        {
            let t = &*ins.add(n as usize - 1);
            let selfl = (t.op as c_uint == OCERZ_OP_JCC || t.op as c_uint == OCERZ_OP_JMP) && t.nops == 1
                && t.ops[0].kind == KIMM && t.ops[0].imm == rip;
            static mut NOFIX: c_int = -1;
            if NOFIX < 0 {
                NOFIX = if libc::getenv(c"OCERZ_NO_L0FIXED".as_ptr()).is_null() { 0 } else { 1 };
            }
            if g_zero_vreg >= 0 {
                a64_v_zero(bp, g_zero_vreg);
            }
            if g_blk_ymm_write != 0 {
                a64_str(bp, 4, A64_ZR as c_int, 20, YMMH_ALL_ZERO_OFF as u32);
            }
            if NOFIX == 0 && selfl && g_no_chain == 0 && g_xlat_mode32 == 0 && l0_enabled() != 0 && sse_enabled() != 0
                && xmm_global_enabled() != 0 && g_no_regflags == 0
            {
                l0_fixed_setup(bp, ins, n);
            }
            if NOFIX == 0 && selfl && g_no_chain == 0 && g_xlat_mode32 == 0 && l0_enabled() != 0 && sse_enabled() != 0
                && xmm_global_enabled() != 0 && g_no_regflags == 0
            {
                yc_setup(bp, ins, n);
            }
        }
        g_loop_entry = a64_label(bp);
        if g_low_hoist_greg >= 0 {
            emit_low_hoist_check(bp);
        }
        if g_mem_hoist_greg >= 0 && g_mem_hoist_aux_index >= 0 {
            a64_add_reg(bp, 1, JMEMAUX, JMEMBASE, pin_hreg(pin_slot(g_mem_hoist_aux_index as c_uint)), g_mem_hoist_aux_scale);
        }
        static mut LOOP_POLL: c_int = -1;
        if LOOP_POLL < 0 {
            LOOP_POLL = if libc::getenv(c"OCERZ_LOOP_POLL".as_ptr()).is_null() { 0 } else { 1 };
        }
        if LOOP_POLL != 0 {
            a64_ldr(bp, 4, JT0, 20, INT_OFF as u32);
            loop_poll_exit = a64_label(bp);
            a64_cbnz(bp, 0, JT0, 0);
        }
    } else {
        if xmm_global_enabled() == 0 {
            emit_xmm_pin_load_all(bp);
        }
        g_low_hoist_greg = -1;
    }
    if ocerz_perfstat > 0 {
        g_tc_bad = 1;
        a64_mov_imm64(bp, JT0, (&raw mut (*blk).exec_count) as usize as u64);
        a64_ldr(bp, 8, JT1, JT0, 0);
        a64_add_imm(bp, 1, JT1, JT1, 1);
        a64_str(bp, 8, JT1, JT0, 0);
    }

    let mut fault_recipes_mem = MaybeUninit::<[JitFaultFlagRecipe; JIT_MAX_BLOCK_INSNS as usize]>::uninit();
    let fault_recipes = fault_recipes_mem.as_mut_ptr().cast::<JitFaultFlagRecipe>();
    let n_fault_recipes = build_fault_flag_recipes(ins, n, fault_recipes);
    (*blk).insn_off = libc::malloc(n as usize * size_of::<u32>()) as *mut u32;
    if !(*blk).insn_off.is_null() && n_fault_recipes != 0 {
        (*blk).fault_flags = libc::malloc(n as usize * size_of::<JitFaultFlagRecipe>()) as *mut JitFaultFlagRecipe;
        if !(*blk).fault_flags.is_null() {
            core::ptr::copy_nonoverlapping(fault_recipes, (*blk).fault_flags, n as usize);
        }
    }

    let mut seam_seed: u64 = OCERZ_FL_ALL as u64;
    if g_no_xlive == 0 && is_terminator((*ins.add(n as usize - 1)).op as c_uint) != 0 {
        let term = &*ins.add(n as usize - 1);
        match term.op as c_uint {
            OCERZ_OP_JMP => {
                if term.ops[0].kind == KIMM {
                    seam_seed = xlive_succ_live(jit, term.ops[0].imm);
                }
            }
            OCERZ_OP_JCC => {
                let taken = xlive_succ_live(jit, term.ops[0].imm);
                let fall = xlive_succ_live(jit, term.rip + term.len as u64);
                seam_seed = taken | fall;
            }
            OCERZ_OP_CALL => {
                if term.ops[0].kind == KIMM {
                    seam_seed = xlive_succ_live(jit, term.ops[0].imm);
                } else if ret_flags_live_at(term.rip) == 0 && mode32 == 0 {
                    seam_seed = 0;
                }
            }
            OCERZ_OP_RET => {
                if ret_flags_live_at(term.rip) == 0 && mode32 == 0 {
                    static mut DEAD: c_int = -1;
                    if DEAD < 0 {
                        DEAD = (!libc::getenv(c"OCERZ_RET_FLAGS_DEAD".as_ptr()).is_null()) as c_int;
                    }
                    seam_seed = if DEAD != 0 { 0 } else { ret_seam_live(ins, n) };
                }
            }
            _ => {}
        }
    }

    let mut fl_need_mem = MaybeUninit::<[u64; JIT_MAX_BLOCK_INSNS as usize]>::uninit();
    let fl_need = fl_need_mem.as_mut_ptr().cast::<u64>();
    let mut jcc_fall_live_mem = MaybeUninit::<[u64; JIT_MAX_BLOCK_INSNS as usize]>::uninit();
    let jcc_fall_live = jcc_fall_live_mem.as_mut_ptr().cast::<u64>();
    let entry_all: u64;
    {
        let mut live_seam = seam_seed;
        let mut live_all: u64 = OCERZ_FL_ALL as u64;
        let mut i = n - 1;
        while i >= 0 {
            let iu = i as usize;
            let inn = &*ins.add(iu);
            let iop = inn.op as c_uint;
            let mut def: u64 = 0;
            let mut usev: u64 = 0;
            *jcc_fall_live.add(iu) = 0;
            if i < n - 1 && iop == OCERZ_OP_JCC {
                let tl = if g_no_xlive != 0 || inn.ops[0].kind != KIMM {
                    OCERZ_FL_ALL as u64
                } else {
                    xlive_succ_live(jit, inn.ops[0].imm)
                };
                *jcc_fall_live.add(iu) = live_seam;
                live_seam |= tl;
                live_all |= tl;
            }
            let side_fused = i < n - 1 && i >= 1 && iop == OCERZ_OP_JCC
                && (side_fuse_ok(ins, i - 1, n) != 0 || (i >= 2 && side_gap_fuse_ok(ins, i - 2, n) != 0));
            if !(*blk).fault_flags.is_null() && (*(*blk).fault_flags.add(iu)).kind as c_uint != JFF_NONE as c_uint {
                ocerz_flags_defuse_nofault(inn, &mut def, &mut usev);
            } else {
                ocerz_flags_defuse(inn, &mut def, &mut usev);
            }
            if (iop == OCERZ_OP_JCC || iop == OCERZ_OP_SETCC || iop == OCERZ_OP_CMOVCC)
                && ((sse_enabled() != 0 && comis_fuse_producer(ins, i) >= 0)
                    || (g_defer != 0 && g_no_regflags == 0 && value_cond_fuse_producer(ins, i) >= 0))
            {
                usev = 0;
            }
            if (iop == OCERZ_OP_SETCC || iop == OCERZ_OP_CMOVCC || iop == OCERZ_OP_ADC || iop == OCERZ_OP_SBB
                || iop == OCERZ_OP_JCC)
                && nzcv_fuse_producer(ins, i) >= 0
            {
                usev &= !(JIT_ARITH_FLAGS as u64);
            }
            if side_fused {
                usev = 0;
            }
            *fl_need.add(iu) = def & live_seam;
            live_seam = (live_seam & !def) | usev;
            live_all = (live_all & !def) | usev;

            if g_no_lazyflags != 0 {
                *fl_need.add(iu) = def;
            }
            i -= 1;
        }
        static mut PUB_ALL: c_int = -1;
        if PUB_ALL < 0 {
            PUB_ALL = if libc::getenv(c"OCERZ_XLIVE_ALL".as_ptr()).is_null() { 0 } else { 1 };
        }
        entry_all = if PUB_ALL != 0 { live_all } else { live_seam };
    }

    (*blk).entry_live = entry_all as u16;

    for ci in 0..n {
        if ic_kind(ci) != 1 {
            continue;
        }
        let mut delta: i64 = 0;
        let mut safe = 1;
        let mut rj: c_int = -1;
        let mut nest = 0;
        for k in ci + 1..n {
            let m = &*ins.add(k as usize);
            let kk = ic_kind(k);
            if kk == 2 || kk == 3 {
                if nest == 0 {
                    rj = k;
                    break;
                }
                nest -= 1;
                delta += 8;
                continue;
            }
            if kk == 1 {
                nest += 1;
                delta -= 8;
                continue;
            }
            match m.op as c_uint {
                OCERZ_OP_PUSH => {
                    if m.nops > 0 && (m.ops[0].kind == KMEM || m.ops[0].size != 8) {
                        safe = 0;
                    } else {
                        delta -= 8;
                    }
                }
                OCERZ_OP_POP => {
                    if m.nops > 0 && (m.ops[0].kind == KMEM || m.ops[0].size != 8) {
                        safe = 0;
                    } else {
                        delta += 8;
                    }
                }
                OCERZ_OP_MOV | OCERZ_OP_LEA | OCERZ_OP_ADD | OCERZ_OP_SUB | OCERZ_OP_XOR | OCERZ_OP_OR | OCERZ_OP_AND
                | OCERZ_OP_SHR | OCERZ_OP_SHL | OCERZ_OP_SAR | OCERZ_OP_IMUL | OCERZ_OP_INC | OCERZ_OP_DEC
                | OCERZ_OP_NEG | OCERZ_OP_NOT | OCERZ_OP_MOVZX | OCERZ_OP_MOVSX | OCERZ_OP_MOVSXD | OCERZ_OP_NOP
                | OCERZ_OP_TEST | OCERZ_OP_CMP => {
                    if m.nops > 0 && m.ops[0].kind == KMEM && m.op as c_uint != OCERZ_OP_TEST
                        && m.op as c_uint != OCERZ_OP_CMP
                    {
                        safe = 0;
                    } else if m.nops > 0 && m.ops[0].kind == KREG && m.ops[0].reg as c_uint == OCERZ_RSP {
                        safe = 0;
                    }
                }
                _ => safe = 0,
            }
            if safe == 0 {
                break;
            }
        }
        if safe != 0 && rj >= 0 && delta == 0 {
            ga!(g_ic_kind, rj) = 3u8;
        }
    }

    for _sweep in 0..3 {
        for ci in 0..n {
            if ic_kind(ci) != 1 || ga!(g_ic_pushelide, ci) != 0u8 || (*ins.add(ci as usize)).mode32 != 0 {
                continue;
            }
            let mut delta: i64 = 0;
            let mut ok = 1;
            let mut rj: c_int = -1;
            let mut nest = 0;
            for k in ci + 1..n {
                let m = &*ins.add(k as usize);
                let kk = ic_kind(k);
                if kk == 3 {
                    if nest == 0 {
                        rj = k;
                        break;
                    }
                    nest -= 1;
                    delta += 8;
                    continue;
                }
                if kk == 2 {
                    ok = 0;
                    break;
                }
                if kk == 1 {
                    if ga!(g_ic_pushelide, k) == 0u8 {
                        ok = 0;
                        break;
                    }
                    nest += 1;
                    delta -= 8;
                    continue;
                }
                let mut memop = 0;
                for q in 0..m.nops as usize {
                    if m.ops[q].kind == KMEM {
                        memop = 1;
                    }
                }
                match m.op as c_uint {
                    OCERZ_OP_PUSH => {
                        if memop != 0 || m.ops[0].size != 8 {
                            ok = 0;
                        } else {
                            delta -= 8;
                        }
                    }
                    OCERZ_OP_POP => {
                        if memop != 0 || delta == 0 || m.ops[0].size != 8 {
                            ok = 0;
                        } else {
                            delta += 8;
                        }
                    }
                    OCERZ_OP_MOV | OCERZ_OP_LEA | OCERZ_OP_ADD | OCERZ_OP_SUB | OCERZ_OP_XOR | OCERZ_OP_OR | OCERZ_OP_AND
                    | OCERZ_OP_SHR | OCERZ_OP_SHL | OCERZ_OP_SAR | OCERZ_OP_IMUL | OCERZ_OP_INC | OCERZ_OP_DEC
                    | OCERZ_OP_NEG | OCERZ_OP_NOT | OCERZ_OP_MOVZX | OCERZ_OP_MOVSX | OCERZ_OP_MOVSXD | OCERZ_OP_NOP
                    | OCERZ_OP_TEST | OCERZ_OP_CMP => {
                        if memop != 0 && m.op as c_uint != OCERZ_OP_LEA {
                            ok = 0;
                        } else if m.nops > 0 && m.ops[0].kind == KREG && m.ops[0].reg as c_uint == OCERZ_RSP {
                            ok = 0;
                        }
                    }
                    _ => ok = 0,
                }
                if ok == 0 {
                    break;
                }
            }
            if ok != 0 && rj >= 0 && delta == 0 {
                ga!(g_ic_pushelide, ci) = 1u8;
                ga!(g_ic_pair_rj, ci) = rj;
            }
        }
    }

    static mut NO_PROMO: c_int = -1;
    if NO_PROMO < 0 {
        NO_PROMO = if libc::getenv(c"OCERZ_NO_PROMO".as_ptr()).is_null() { 0 } else { 1 };
    }
    if NO_PROMO == 0 && g_pin_class == 3 && pin_slot(OCERZ_RSP) >= 0 && (stack_identity() != 0 || rsp_is_ptr() != 0) {
        let mut freer = [0 as c_int; 3];
        let mut nfree = 0usize;
        if g_mem_hoist_greg2 < 0 {
            freer[nfree] = JMEMBASE2;
            nfree += 1;
        }
        if g_mem_hoist_greg < 0 && g_low_hoist_greg < 0 {
            freer[nfree] = JMEMBASE;
            nfree += 1;
        }
        if g_mem_hoist_greg3 < 0 {
            freer[nfree] = JMEMBASE3;
            nfree += 1;
        }
        let mut npairs = 0;
        let mut sp = 0usize;
        let mut dead = 0;
        let mut pstk = [0 as c_int; 64];
        let mut rres = [0 as i8; 64];
        let mut nend = 0usize;
        let mut endstk = [0 as i32; 8];
        for i in 0..n {
            while nend > 0 && i >= endstk[nend - 1] {
                nend -= 1;
            }
            if ic_kind(i) == 1 && ga!(g_ic_pushelide, i) != 0u8 {
                if nend < 8 {
                    endstk[nend] = ga!(g_ic_pair_rj, i);
                    nend += 1;
                }
                continue;
            }
            if nend == 0 {
                sp = 0;
                dead = 0;
                continue;
            }
            if dead != 0 {
                continue;
            }
            let m = &*ins.add(i as usize);
            let plain = m.nops > 0 && m.ops[0].kind == KREG && m.ops[0].high8 == 0 && m.ops[0].size == 8
                && m.ops[0].reg as c_uint != OCERZ_RSP && pin_slot(m.ops[0].reg as c_uint) >= 0 && m.mode32 == 0;
            if m.op as c_uint == OCERZ_OP_PUSH {
                if !plain || sp >= 64 {
                    dead = 1;
                    continue;
                }
                if nfree > 0 && npairs < PE_MAX as c_int {
                    nfree -= 1;
                    rres[sp] = freer[nfree] as i8;
                } else {
                    rres[sp] = -1;
                }
                pstk[sp] = i;
                sp += 1;
            } else if m.op as c_uint == OCERZ_OP_POP {
                if !plain || sp == 0 {
                    dead = 1;
                    continue;
                }
                sp -= 1;
                if rres[sp] >= 0 {
                    ga!(g_promo_reg, pstk[sp]) = rres[sp] as u8;
                    ga!(g_promo_reg, i) = rres[sp] as u8;
                    ga!(g_promo_mate, pstk[sp]) = i;
                    ga!(g_promo_push_of, i) = pstk[sp];
                    freer[nfree] = rres[sp] as c_int;
                    nfree += 1;
                    npairs += 1;
                }
            }
        }
    }

    if g_flaglive_log != 0 {
        let mut wrote = 0 as c_int;
        let mut killed = 0 as c_int;
        const FBITS: [u32; 6] = [0, 2, 4, 6, 7, 11];
        for i in 0..n as usize {
            let mut def: u64 = 0;
            let mut usev: u64 = 0;
            ocerz_flags_defuse(ins.add(i), &mut def, &mut usev);
            for f in 0..6 {
                let bit = 1u64 << FBITS[f];
                if def & bit != 0 {
                    wrote += 1;
                    if *fl_need.add(i) & bit == 0 {
                        killed += 1;
                    }
                }
            }
        }
        if wrote != 0 {
            libc::fprintf(crate::log::stderr(),
                c"ocerz: FLAGLIVE rip=%#llx insns=%d flagwrites=%d dead=%d (%.1f%%)\n".as_ptr(),
                rip as libc::c_ulonglong, n, wrote, killed, 100.0 * killed as f64 / wrote as f64);
        }
    }

    let mut exit_sites_mem = MaybeUninit::<[*mut u32; 2 * JIT_MAX_BLOCK_INSNS as usize + 64]>::uninit();
    let exit_sites = exit_sites_mem.as_mut_ptr().cast::<*mut u32>();
    let mut epi_sites_mem = MaybeUninit::<[*mut u32; 2 * JIT_MAX_BLOCK_INSNS as usize + 64]>::uninit();
    let epi_sites = epi_sites_mem.as_mut_ptr().cast::<*mut u32>();
    let mut n_exits: c_int = 0;
    let mut n_epi: c_int = 0;
    let mut fpb_of_mem = MaybeUninit::<[i8; JIT_MAX_BLOCK_INSNS as usize]>::uninit();
    let fpb_of = fpb_of_mem.as_mut_ptr().cast::<i8>();
    fpb_scan(ins, n, fpb_of);
    mov_sink_scan(ins, n, fl_need);
    (*(&raw mut g_scpend)).valid = 0;
    g_scalar_merge_next = 0;
    g_fpb_of = fpb_of;
    g_fpb_open = -1;
    g_fpb_fast = 0;
    g_fcmp_self_idx = -1;
    g_cmps_mask_idx = -1;
    l0_reset();
    if g_l0_fixed != 0 {
        l0_fixed_map();
    }
    g_ymmh_zero = 0;
    let mut l0_last_seq = g_callout_seq;
    g_n_fpbmap = 0;
    g_n_lanerec = 0;
    let mut fpb_open: c_int = -1;
    let gfpb = (&raw mut g_fpb).cast::<FpBatch>();
    let gl0 = (&raw mut g_l0).cast::<i8>();
    let gl0_dbl = (&raw mut g_l0_dbl).cast::<u8>();

    let mut last_flag_def: c_int = -1;
    ea_cache_reset();
    let mut leaf_entry_writes: c_int = 0;
    let mut leaf_entry: *const c_void = null();
    if ocerz_mode != OCERZ_MODE_NATIVE as _ && g_l0_fixed == 0 && leaf_layout_ok() != 0 {
        leaf_entry = ocerz_dyldapi_leaf_entry(rip, &mut leaf_entry_writes);
        let mut k = 0;
        while !leaf_entry.is_null() && k < n {
            let t = &*ins.add(k as usize);
            if t.op as c_uint != OCERZ_OP_CALL && t.nops == 1 && t.ops[0].kind == KIMM && t.ops[0].imm == rip {
                leaf_entry = null();
            }
            k += 1;
        }
    }
    let mut i: c_int = 0;
    while i < n {
        'body: {
        let iu = i as usize;
        let insn = ins.add(iu);
        let ir = &*insn;
        let iop = ir.op as c_uint;
        g_cur_insn_idx = i;
        g_cur_insn_start = (*bp).p;
        g_align_guard = (g_align_blk != 0 || (g_align_any != 0 && al_marked(jit_key(ir.rip, mode32)) != 0)) as c_int;
        g_ea_plain = 0;
        g_ea_lowhoisted = 0;
        lanerec_note((*bp).p.offset_from(entry) as u32);
        if i == 0 && fps_watch(rip) != 0 {
            g_tc_bad = 1;
            a64_mov_imm64(bp, JT0, (&raw mut g_fps_frames) as usize as u64);
            a64_ldr(bp, 8, JT1, JT0, 0);
            a64_add_imm(bp, 1, JT1, JT1, 1);
            a64_str(bp, 8, JT1, JT0, 0);
        }
        if i == 0 && !leaf_entry.is_null() {
            crate::ocerz_log!("jit: the routine at %#llx is answered in place\n", rip as libc::c_ulonglong);
            let declined = emit_leaf_call_ret(bp, leaf_entry, leaf_entry_writes, epi_sites, &mut n_epi);
            a64_patch_cbz(declined, a64_label(bp));
            ea_cache_reset();
        }
        g_ea_is_const = 0;
        if g_lowstack != 0 {
            let mut moved = 0;
            for k in g_lowstack_from..i {
                moved |= lowstack_disturbs(ins.add(k as usize));
            }
            g_lowstack_from = i;
            if moved != 0 {
                emit_stack_delta(bp);
            }
            if g_lowstack_check != 0 {
                emit_stack_delta_check(bp);
            }
        }
        g_cur_need = *fl_need.add(iu);
        g_cur_insns = ins;
        g_cur_insns_n = n;
        {
            let mut mmx = false;
            for k in 0..ir.nops as usize {
                mmx |= ir.ops[k].kind == KMMX;
            }
            if (iop > OCERZ_OP_X87_FIRST && iop < OCERZ_OP_SSE_FIRST && x87_inline_ok(insn) == 0)
                || iop == OCERZ_OP_FXRSTOR || iop == OCERZ_OP_XRSTOR || iop == OCERZ_OP_EMMS || mmx
            {
                g_x87_btop = -1;
                g_x87_lv = 0;
                g_x87_kcarry = 0;
            }
        }
        g_cur_fpb = *fpb_of.add(iu) as c_int;
        if (*(&raw const g_scpend)).valid != 0 && (*(&raw const g_scpend)).idx < i - 1 {
            scalar_pend_flush(bp);
        }
        g_fpb_open = fpb_open;
        g_fpb_fast = (fpb_open >= 0 && ga!(g_fpb_member, iu) != 0) as c_int;
        ea_cache_step(insn, if i > 0 { ins.add(iu - 1) } else { null() });
        g_nzcv_want = 0;
        let mut j = i + 1;
        while j < n && j <= i + 1 + NZCV_GAP_MAX as c_int {
            if nzcv_fuse_producer(ins, j) == i {
                g_nzcv_want = 1;
                break;
            }
            j += 1;
        }
        if g_callout_seq != l0_last_seq {
            l0_flush_all(bp);
            l0_reset();
            l0_last_seq = g_callout_seq;
        }
        l0_pre_insn(bp, insn);
        if !(iop == OCERZ_OP_JCC && i < n - 1)
            && !(g_l0_fixed != 0 && i >= n - 2)
            && (is_terminator(iop) != 0
                || (i + 1 < n
                    && ((*ins.add(iu + 1)).op as c_uint == OCERZ_OP_JMP
                        || ((*ins.add(iu + 1)).op as c_uint == OCERZ_OP_JCC && i + 1 == n - 1))))
        {
            l0_flush_all(bp);
        }
        if fpb_open >= 0 && *fpb_of.add(iu) as c_int != fpb_open {
            let fb = gfpb.add(fpb_open as usize);
            fpb_emit_check(bp, fb);
            for r in 0..16 {
                (*fb).l0[r] = *gl0.add(r) as _;
                (*fb).l0_dbl[r] = *gl0_dbl.add(r) as _;
            }
            fpb_open = -1;
            g_fpb_open = -1;
            g_fpb_fast = 0;
        }
        if *fpb_of.add(iu) >= 0 && fpb_open < 0 && (*gfpb.add(*fpb_of.add(iu) as usize)).first as c_int == i {
            fpb_open = *fpb_of.add(iu) as c_int;
            g_fpb_open = fpb_open;
            let fb = gfpb.add(fpb_open as usize);
            let mut lanes: u16 = 0;
            for r in 0..16u32 {
                if *gl0.add(r as usize) >= 0 && (g_l0_dirty as u32 & (*fb).ckpt as u32 & (1u32 << r)) != 0 {
                    lanes |= (1u32 << r) as u16;
                }
            }
            (*fb).dirty_open = g_l0_dirty as _;
            (*fb).ckpt_emit = (*fb).ckpt;
            for r in 0..16u32 {
                if ((*fb).ckpt_emit as u32 & (1u32 << r)) != 0 {
                    a64_str_v(bp, 16, xmm_vreg(r), 20, FPCKPT_OFF as u32 + r * 16);
                    if (lanes as u32 & (1u32 << r)) != 0 {
                        a64_str_v(bp, if *gl0_dbl.add(r as usize) != 0 { 8 } else { 4 }, *gl0.add(r as usize) as c_int, 20,
                            FPCKPT_OFF as u32 + r * 16);
                    }
                }
            }
            g_fpb_fast = 1;
        }
        g_flag_producer = if last_flag_def >= 0 { ins.add(last_flag_def as usize) } else { null_mut() };
        {
            g_flag_producer_operands_intact = 1;
            if !g_flag_producer.is_null() {
                let fp = &*g_flag_producer;
                for k in last_flag_def + 1..i {
                    let m = &*ins.add(k as usize);
                    if m.nops > 0 && m.ops[0].kind == KXMM {
                        let w = m.ops[0].reg;
                        if (fp.ops[0].kind == KXMM && fp.ops[0].reg == w) || (fp.ops[1].kind == KXMM && fp.ops[1].reg == w) {
                            g_flag_producer_operands_intact = 0;
                        }
                    }
                }
            }
            let mut pdef: u64 = 0;
            let mut puse: u64 = 0;
            ocerz_flags_defuse(insn, &mut pdef, &mut puse);
            if pdef & JIT_ARITH_FLAGS as u64 != 0 {
                last_flag_def = i;
            }
        }
        if g_n_side < SIDE_MAX as c_int && side_gap_fuse_ok(ins, i, n) != 0 {
            let mut jcc_label: *mut u32 = null_mut();
            let mut gap_label: *mut u32 = null_mut();
            if !(*blk).insn_off.is_null() {
                *(*blk).insn_off.add(iu) = (*bp).p.offset_from(entry) as u32;
            }
            g_jcc_side_mode = 1;
            g_jcc_side_need = *fl_need.add(iu);
            {
                let mut pdef: u64 = 0;
                let mut puse: u64 = 0;
                ocerz_flags_defuse(insn, &mut pdef, &mut puse);
                g_jcc_side_fall_need = pdef & *jcc_fall_live.add(iu + 2);
            }
            let fused = emit_cmp_test_jcc(bp, insn, ins.add(iu + 2), epi_sites, &mut n_epi, &mut jcc_label, exit_sites,
                &mut n_exits, ins.add(iu + 1), &mut gap_label);
            g_jcc_side_mode = 0;
            if fused != 0 && !jcc_label.is_null() && !gap_label.is_null() {
                if !(*blk).insn_off.is_null() {
                    *(*blk).insn_off.add(iu + 1) = gap_label.offset_from(entry) as u32;
                    *(*blk).insn_off.add(iu + 2) = jcc_label.offset_from(entry) as u32;
                }
                (*blk).n_inlined += 3;
                i += 2;
                break 'body;
            }
        }
        if g_n_side < SIDE_MAX as c_int && side_fuse_ok(ins, i, n) != 0 {
            let mut jcc_label: *mut u32 = null_mut();
            if !(*blk).insn_off.is_null() {
                *(*blk).insn_off.add(iu) = (*bp).p.offset_from(entry) as u32;
            }
            g_jcc_side_mode = 1;
            g_jcc_side_need = *fl_need.add(iu);
            {
                let mut pdef: u64 = 0;
                let mut puse: u64 = 0;
                ocerz_flags_defuse(insn, &mut pdef, &mut puse);
                g_jcc_side_fall_need = pdef & *jcc_fall_live.add(iu + 1);
            }
            let fused = emit_cmp_test_jcc(bp, insn, ins.add(iu + 1), epi_sites, &mut n_epi, &mut jcc_label, exit_sites,
                &mut n_exits, null(), null_mut());
            g_jcc_side_mode = 0;
            if fused != 0 {
                if !(*blk).insn_off.is_null() {
                    *(*blk).insn_off.add(iu + 1) = jcc_label.offset_from(entry) as u32;
                }
                (*blk).n_inlined += 2;
                i += 1;
                break 'body;
            }
        }
        if i < n - 1 && iop == OCERZ_OP_JCC {
            if fpb_open >= 0 && *fpb_of.add(iu) as c_int != fpb_open {
                l0_flush_all(bp);
                let fb = gfpb.add(fpb_open as usize);
                fpb_emit_check(bp, fb);
                for r in 0..16 {
                    (*fb).l0[r] = *gl0.add(r) as _;
                    (*fb).l0_dbl[r] = *gl0_dbl.add(r) as _;
                }
                fpb_open = -1;
                g_fpb_open = -1;
                g_fpb_fast = 0;
                l0_reset();
            }
            if !(*blk).insn_off.is_null() {
                *(*blk).insn_off.add(iu) = (*bp).p.offset_from(entry) as u32;
            }
            g_cc_want_cbz = 1;
            emit_cc_predicate_ex(bp, ir.cc as _, 1);
            g_cc_want_cbz = 0;
            let cond = if g_cc_direct >= 0 { g_cc_direct } else { A64_NE as c_int };
            if g_n_side < SIDE_MAX as c_int {
                let sd = (&raw mut g_side).cast::<JitState_g_side>().add(g_n_side as usize);
                let sidechk = ga!(g_fpb_sidechk, iu);
                (*sd).site = a64_label(bp);
                (*sd).taken = ir.ops[0].imm;
                (*sd).idx = i as _;
                (*sd).stub = null_mut();
                (*sd).patch_b = null_mut();
                (*sd).rec = 0;
                (*sd).fpb = (if fpb_open >= 0 && sidechk != 0 { fpb_open } else { -1 }) as _;
                (*sd).fpb_chk = sidechk as _;
                (*sd).fpb_end = (i - 1) as _;
                (*sd).l0_dirty = g_l0_dirty as _;
                (*sd).yc_dirty = g_yc_dirty as _;
                for r in 0..16 {
                    (*sd).l0[r] = *gl0.add(r) as _;
                    (*sd).l0_dbl[r] = *gl0_dbl.add(r) as _;
                }
                (*sd).jcc_rip = ir.rip;
                (*sd).ft_rip = ir.rip + ir.len as u64;
                (*sd).ft_site = null_mut();
                (*sd).probe = probe_wanted(ir.rip, ir.rip + ir.len as u64) as _;
                if g_cc_cbz_reg >= 0 {
                    if g_cc_cbz_nz != 0 {
                        a64_cbnz(bp, g_cc_cbz_sf, g_cc_cbz_reg, 0);
                    } else {
                        a64_cbz(bp, g_cc_cbz_sf, g_cc_cbz_reg, 0);
                    }
                } else {
                    a64_bcond(bp, cond, 0);
                }
                if (*sd).probe != 0 {
                    (*sd).ft_site = a64_label(bp);
                    a64_b(bp, 0);
                }
                g_n_side += 1;
            } else {
                panic!("side exit table full");
            }
            (*blk).n_inlined += 1;
            let mut pdef: u64 = 0;
            let mut puse: u64 = 0;
            ocerz_flags_defuse(insn, &mut pdef, &mut puse);
            break 'body;
        }
        if !(*blk).insn_off.is_null() {
            *(*blk).insn_off.add(iu) = (*bp).p.offset_from(entry) as u32;
        }
        if i == n - 2 {
            let mut jmp_label: *mut u32 = null_mut();
            if emit_logic_jmp_incdec_jcc(bp, insn, ins.add(iu + 1), *fl_need.add(iu), epi_sites, &mut n_epi, &mut jmp_label) != 0 {
                if !(*blk).insn_off.is_null() {
                    *(*blk).insn_off.add(iu + 1) = jmp_label.offset_from(entry) as u32;
                }
                (*blk).n_inlined += 2;
                i += 1;
                break 'body;
            }
        }
        if i == n - 3 {
            let mut incdec_label: *mut u32 = null_mut();
            let mut jcc_label: *mut u32 = null_mut();
            if emit_arith_incdec_jcc(bp, insn, ins.add(iu + 1), ins.add(iu + 2), *fl_need.add(iu), epi_sites, &mut n_epi,
                &mut incdec_label, &mut jcc_label) != 0
            {
                if !(*blk).insn_off.is_null() {
                    *(*blk).insn_off.add(iu + 1) = incdec_label.offset_from(entry) as u32;
                    *(*blk).insn_off.add(iu + 2) = jcc_label.offset_from(entry) as u32;
                }
                (*blk).n_inlined += 3;
                i += 2;
                break 'body;
            }
        }
        let mut pair_consumed = 0;
        let mut j = i + 2;
        while j < n && j <= i + 2 + NZCV_GAP_MAX as c_int {
            if nzcv_fuse_producer(ins, j) == i + 1 {
                pair_consumed = 1;
                break;
            }
            j += 1;
        }
        if i + 1 < n && pair_consumed == 0 {
            let mut logic_label: *mut u32 = null_mut();
            if emit_mov_logic_pair(bp, insn, ins.add(iu + 1), *fl_need.add(iu + 1), &mut logic_label) != 0 {
                if !(*blk).insn_off.is_null() {
                    *(*blk).insn_off.add(iu + 1) = logic_label.offset_from(entry) as u32;
                }
                (*blk).n_inlined += 2;
                last_flag_def = i + 1;
                i += 1;
                break 'body;
            }
        }
        if i + 1 < n {
            let mut inc_label: *mut u32 = null_mut();
            if emit_add_inc_pair(bp, insn, ins.add(iu + 1), *fl_need.add(iu), *fl_need.add(iu + 1), &mut inc_label) != 0 {
                if !(*blk).insn_off.is_null() {
                    *(*blk).insn_off.add(iu + 1) = inc_label.offset_from(entry) as u32;
                }
                (*blk).n_inlined += 2;
                last_flag_def = i + 1;
                i += 1;
                break 'body;
            }
        }
        if fuse_cmp != 0 && i == n - 2 {
            let mut jcc_label: *mut u32 = null_mut();
            if emit_ifconv_diamond(bp, insn, ins.add(iu + 1), epi_sites, &mut n_epi, &mut jcc_label) != 0 {
                if !(*blk).insn_off.is_null() {
                    *(*blk).insn_off.add(iu + 1) = jcc_label.offset_from(entry) as u32;
                }
                (*blk).n_inlined += 2;
                i += 1;
                break 'body;
            }
        }
        if n >= 3 && i == n - 3 && g_no_jccfuse == 0 && g_defer != 0 && (iop == OCERZ_OP_CMP || iop == OCERZ_OP_TEST)
            && can_fuse_cmp_test_jcc(insn, ins.add(n as usize - 1), rip) != 0
        {
            let mut jcc_label: *mut u32 = null_mut();
            let mut gap_label: *mut u32 = null_mut();
            let fused = emit_cmp_test_jcc(bp, insn, ins.add(n as usize - 1), epi_sites, &mut n_epi, &mut jcc_label,
                exit_sites, &mut n_exits, ins.add(n as usize - 2), &mut gap_label);
            if fused != 0 && !jcc_label.is_null() && !gap_label.is_null() {
                if !(*blk).insn_off.is_null() {
                    *(*blk).insn_off.add(iu + 1) = gap_label.offset_from(entry) as u32;
                    *(*blk).insn_off.add(iu + 2) = jcc_label.offset_from(entry) as u32;
                }
                (*blk).n_inlined += 3;
                i += 2;
                break 'body;
            }
        }
        if fuse_pair != 0 && i == n - 2 {
            let mut jcc_label: *mut u32 = null_mut();
            let fused = if fuse_cmp != 0 {
                emit_cmp_test_jcc(bp, insn, ins.add(iu + 1), epi_sites, &mut n_epi, &mut jcc_label, exit_sites, &mut n_exits,
                    null(), null_mut())
            } else {
                emit_incdec_jcc(bp, insn, ins.add(iu + 1), epi_sites, &mut n_epi, &mut jcc_label)
            };
            if fused != 0 {
                if !(*blk).insn_off.is_null() {
                    *(*blk).insn_off.add(iu + 1) = jcc_label.offset_from(entry) as u32;
                }
                (*blk).n_inlined += 2;
                i += 1;
                break 'body;
            }
        }
        if i == n - 1 && iop == OCERZ_OP_JCC {
            if emit_jcc(bp, insn, epi_sites, &mut n_epi) != 0 {
                (*blk).n_inlined += 1;
                break 'body;
            }
        }

        if i == n - 1 && iop == OCERZ_OP_JMP {
            emit_bridge_fastcall(bp, ins, i, epi_sites, &mut n_epi);
            if emit_jmp(bp, insn, epi_sites, &mut n_epi) != 0
                || emit_indirect_jmp(bp, insn, exit_sites, &mut n_exits, epi_sites, &mut n_epi) != 0
            {
                (*blk).n_inlined += 1;
                break 'body;
            }
        }

        'promo: {
            if promo_reg(i) != 0 {
                let hsp = pin_hreg(pin_slot(OCERZ_RSP));
                let pr = promo_reg(i) as c_int;
                let gr = pin_hreg(pin_slot(ir.ops[0].reg as c_uint));
                if iop == OCERZ_OP_POP && ga!(g_promo_seq, ga!(g_promo_push_of, i)) != g_callout_seq as u64 {
                    if g_rsp_lag != 0 {
                        a64_add_imm(bp, 1, hsp, hsp, g_rsp_lag);
                        g_rsp_lag = 0;
                    }
                    break 'promo;
                }
                if iop == OCERZ_OP_PUSH {
                    ga!(g_promo_seq, i) = g_callout_seq as u64;
                    a64_mov_reg(bp, 1, pr, gr);
                    if g_n_promo_real < PE_MAX as c_int {
                        *(&raw mut g_promo_real).cast::<JitPromo>().add(g_n_promo_real as usize) =
                            JitPromo { pi: i, qi: ga!(g_promo_mate, i), hreg: pr as u8 };
                        g_n_promo_real += 1;
                    }
                    break 'promo;
                } else {
                    let f3 = (g_pin_class == 3 && pin_slot(OCERZ_RSP) >= 0 && stack_plain_access_ok() != 0
                        && jgb_usable() != 0 && stack_guard_needed() == 0) as c_int;
                    a64_mov_reg(bp, 1, gr, pr);
                    if rsp_run_member(ins, i + 1, n, f3) != 0 {
                        g_rsp_lag += 8;
                    } else {
                        a64_add_imm(bp, 1, hsp, hsp, 8 + g_rsp_lag);
                        g_rsp_lag = 0;
                    }
                }
                (*blk).n_inlined += 1;
                break 'body;
            }
        }
        if ic_kind(i) != 0 {
            let fast3 = (g_pin_class == 3 && pin_slot(OCERZ_RSP) >= 0 && stack_plain_access_ok() != 0 && jgb_usable() != 0
                && stack_guard_needed() == 0) as c_int;
            if fast3 == 0 && low_splice_ok(ir) != 0 {
                let hs = pin_hreg(pin_slot(OCERZ_RSP));
                if ic_kind(i) == 1 && ga!(g_ic_pushelide, i) != 0u8 && g_n_pe_real < PE_MAX as c_int
                    && (stack_identity() != 0 || rsp_is_ptr() != 0)
                {
                    a64_sub_imm(bp, 1, hs, hs, 8);
                    *(&raw mut g_pe_real).cast::<JitBlock_JitPushElide>().add(g_n_pe_real as usize) =
                        JitBlock_JitPushElide { ci: i, rj: ga!(g_ic_pair_rj, i), ra: ir.rip + ir.len as u64 };
                    g_n_pe_real += 1;
                } else if ic_kind(i) == 3 {
                    if rsp_run_member(ins, i + 1, n, 1) != 0 {
                        g_rsp_lag += 8;
                    } else {
                        a64_add_imm(bp, 1, hs, hs, 8 + g_rsp_lag);
                        g_rsp_lag = 0;
                    }
                } else if ic_kind(i) == 1 {
                    emit_gpr_rd(bp, 1, JT0, OCERZ_RSP);
                    a64_sub_imm(bp, 1, JTA, JT0, 8);
                    emit_add_const(bp, JTA, ea_fold());
                    let skip = emit_commpage_guard(bp, insn, JTA, exit_sites, &mut n_exits);
                    emit_add_const(bp, JTA, ocerz_guest_base.wrapping_sub(ea_fold()));
                    a64_mov_imm64(bp, JT1, ir.rip + ir.len as u64);
                    g_ea_plain = stack_plain_now();
                    emit_guest_store_ordered(bp, 8, JT1, JTA, JTU);
                    patch_guard_skip(skip, a64_label(bp));
                    a64_sub_imm(bp, 1, hs, hs, 8);
                } else {
                    emit_gpr_rd(bp, 1, JT0, OCERZ_RSP);
                    a64_mov_reg(bp, 1, JTA, JT0);
                    emit_add_const(bp, JTA, ea_fold());
                    let skip = emit_commpage_guard(bp, insn, JTA, exit_sites, &mut n_exits);
                    emit_add_const(bp, JTA, ocerz_guest_base.wrapping_sub(ea_fold()));
                    emit_guest_load_ordered(bp, 8, JT0, JTA, JTU);
                    patch_guard_skip(skip, a64_label(bp));
                    a64_mov_imm64(bp, JT1, ga!(g_ic_expect, i));
                    a64_subs_reg(bp, 1, 31, JT0, JT1, 0);
                    let ok = a64_label(bp);
                    a64_bcond(bp, A64_EQ as c_int, 0);
                    a64_mov_imm64(bp, JT0, ir.rip);
                    a64_str(bp, 8, JT0, 20, RIP_OFF as u32);
                    a64_mov_imm64(bp, 0, OCERZ_STEP_OK as u64);
                    *epi_sites.add(n_epi as usize) = a64_label(bp);
                    a64_b(bp, 0);
                    n_epi += 1;
                    a64_patch_bcond(ok, a64_label(bp));
                    a64_add_imm(bp, 1, hs, hs, 8);
                }
                (*blk).n_inlined += 1;
                break 'body;
            }
            if fast3 == 0 {
                emit_slowcall(bp, insn, exit_sites, &mut n_exits);
                (*blk).n_slow += 1;
                break 'body;
            }
            let hs = pin_hreg(pin_slot(OCERZ_RSP));
            if ic_kind(i) == 1 {
                if ga!(g_ic_pushelide, i) != 0u8 && g_n_pe_real < PE_MAX as c_int && (stack_identity() != 0 || rsp_is_ptr() != 0) {
                    a64_sub_imm(bp, 1, hs, hs, 8);
                    *(&raw mut g_pe_real).cast::<JitBlock_JitPushElide>().add(g_n_pe_real as usize) =
                        JitBlock_JitPushElide { ci: i, rj: ga!(g_ic_pair_rj, i), ra: ir.rip + ir.len as u64 };
                    g_n_pe_real += 1;
                } else {
                    a64_mov_imm64(bp, JT1, ir.rip + ir.len as u64);
                    emit_push_pinned(bp, hs, JT1);
                }
            } else if ic_kind(i) == 3 {
                if rsp_run_member(ins, i + 1, n, fast3) != 0 {
                    g_rsp_lag += 8;
                } else {
                    a64_add_imm(bp, 1, hs, hs, 8 + g_rsp_lag);
                    g_rsp_lag = 0;
                }
            } else {
                if stack_identity() != 0 || rsp_is_ptr() != 0 {
                    a64_ldr_post64(bp, JT0, hs, 8);
                } else {
                    a64_ldr_regoff(bp, 8, JT0, JGB, hs, 0);
                    a64_add_imm(bp, 1, hs, hs, 8);
                }
                a64_mov_imm64(bp, JT1, ga!(g_ic_expect, i));
                a64_subs_reg(bp, 1, 31, JT0, JT1, 0);
                let ok = a64_label(bp);
                a64_bcond(bp, A64_EQ as c_int, 0);
                a64_sub_imm(bp, 1, hs, hs, 8);
                a64_mov_imm64(bp, JT0, ir.rip);
                a64_str(bp, 8, JT0, 20, RIP_OFF as u32);
                a64_mov_imm64(bp, 0, OCERZ_STEP_OK as u64);
                *epi_sites.add(n_epi as usize) = a64_label(bp);
                a64_b(bp, 0);
                n_epi += 1;
                a64_patch_bcond(ok, a64_label(bp));
            }
            (*blk).n_inlined += 1;
            break 'body;
        }
        if i == n - 1 && (iop == OCERZ_OP_CALL || iop == OCERZ_OP_RET) {
            if emit_call_ret(bp, insn, exit_sites, &mut n_exits, epi_sites, &mut n_epi) != 0
                || emit_indirect_call(bp, insn, exit_sites, &mut n_exits, epi_sites, &mut n_epi) != 0
            {
                (*blk).n_inlined += 1;
                break 'body;
            }
        }
        if ga!(g_mov_skip, iu) != 0 {
            if !(*blk).insn_off.is_null() {
                *(*blk).insn_off.add(iu) = (*bp).p.offset_from(entry) as u32;
            }
            (*blk).n_inlined += 1;
            break 'body;
        }
        let in_fpb = fpb_open >= 0 && *fpb_of.add(iu) as c_int == fpb_open;
        if in_fpb && ga!(g_fpb_stchk, iu) != 0 {
            fpb_emit_store_check(bp, i, fpb_open);
        }
        if in_fpb && ga!(g_fpb_undo, iu) != 0 && ga!(g_fpb_undo_done, iu) == 0 {
            fpb_emit_undo_save(bp, insn, i, exit_sites, &mut n_exits);
        }
        g_undo_want_slot = -1;
        g_undo_saved = 0;
        if i + 1 < n && ga!(g_mov_skip, iu + 1) == 0 && emit_mov128_pair(bp, insn, ins.add(iu + 1), i) != 0 {
            (*blk).n_inlined += 1;
            break 'body;
        }
        if i + 1 < n && ga!(g_mov_skip, iu + 1) == 0 && emit_stack_pair(bp, ir, &*ins.add(iu + 1), i) != 0 {
            (*blk).n_inlined += 1;
            break 'body;
        }
        if in_fpb && ga!(g_fpb_undo_ld, iu) != 0 {
            g_undo_want_slot = ga!(g_fpb_undo_ld, iu) as c_int - 1;
            g_undo_want_size = ga!(g_fpb_undo_ldsz, iu) as _;
        }
        if try_inline(bp, insn, *fl_need.add(iu), exit_sites, &mut n_exits) == 0 {
            emit_slowcall(bp, insn, exit_sites, &mut n_exits);
            (*blk).n_slow += 1;
        } else {
            (*blk).n_inlined += 1;
        }
        if g_undo_saved != 0 && ga!(g_fpb_undo_ldst, iu) >= 0 {
            ga!(g_fpb_undo_done, ga!(g_fpb_undo_ldst, iu) as usize) = 1;
        }
        g_undo_want_slot = -1;
        g_undo_saved = 0;
        }
        i += 1;
    }

    if fpb_open >= 0 {
        let fb = gfpb.add(fpb_open as usize);
        fpb_emit_check(bp, fb);
        for r in 0..16 {
            (*fb).l0[r] = *gl0.add(r) as _;
            (*fb).l0_dbl[r] = *gl0_dbl.add(r) as _;
        }
        fpb_open = -1;
        g_fpb_open = -1;
        g_fpb_fast = 0;
        l0_flush_all(bp);
        l0_reset();
    }
    let _ = fpb_open;
    g_pe_insns = null_mut();
    if g_n_pe_real > 0 {
        (*blk).pushelide = libc::malloc(g_n_pe_real as usize * pointee_size((*blk).pushelide)).cast();
        if !(*blk).pushelide.is_null() {
            core::ptr::copy_nonoverlapping((&raw const g_pe_real).cast::<JitBlock_JitPushElide>(), (*blk).pushelide.cast(), g_n_pe_real as usize);
            (*blk).n_pushelide = g_n_pe_real as u16;
        }
    }

    l0_flush_all(bp);
    if is_terminator((*ins.add(n as usize - 1)).op as c_uint) == 0 {
        emit_materialize(bp);
        a64_mov_imm64(bp, JT0, if mode32 != 0 { pc as u32 as u64 } else { pc });
        a64_str(bp, 8, JT0, 20, RIP_OFF as u32);
        a64_mov_imm64(bp, 0, OCERZ_STEP_OK as u64);
    }

    let exit_label = a64_label(bp);
    emit_xmm_pin_spill_all(bp);
    emit_spill_pinned(bp);
    emit_frame_sp_reset(bp);
    emit_pin_epilogue_restore(bp);
    let mut dstub: *mut u32 = if mode32 != 0 { (*jit).dispatch_stub32 } else { (*jit).dispatch_stub };
    if term_may_switch_mode((*ins.add(n as usize - 1)).op as c_uint) != 0 {
        dstub = null_mut();
    }
    if !dstub.is_null() {
        a64_mov_reg(bp, 1, 1, 20);
        a64_mov_reg(bp, 1, JTT, 0);
        a64_mov_reg(bp, 1, 0, 19);
        a64_ldp_post(bp, 19, 20, 31, 16);
        a64_ldp_post(bp, 29, 30, 31, 16);
        let nonzero = a64_label(bp);
        a64_cbnz(bp, 1, JTT, 0);
        let here = a64_label(bp);
        let soff: isize = ((dstub as isize).wrapping_sub(here as isize)) / 4;
        if g_tc_on != 0 {
            tc_imm64(bp, 16, TCR_DSTUB as c_int, mode32 as u64, dstub as usize as u64);
            a64_br(bp, 16);
        } else if soff >= -(1isize << 25) && soff <= (1isize << 25) - 1 {
            a64_b(bp, soff as i32);
        } else {
            a64_mov_imm64(bp, 16, dstub as usize as u64);
            a64_br(bp, 16);
        }
        a64_patch_cbz(nonzero, a64_label(bp));
        a64_mov_reg(bp, 1, 0, JTT);
        a64_ret(bp);
    } else {
        a64_ldp_post(bp, 19, 20, 31, 16);
        a64_ldp_post(bp, 29, 30, 31, 16);
        a64_ret(bp);
    }
    let mut side_patch_oor = 0;
    let gside = (&raw mut g_side).cast::<JitState_g_side>();
    for k in 0..g_n_side as usize {
        if (*gside.add(k)).probe != 0 && (*blk).prof.is_null() {
            (*blk).prof = libc::calloc(SIDE_MAX as usize, size_of::<JitProf>()) as *mut JitProf;
        }
    }
    for k in 0..g_n_side as usize {
        let sd = gside.add(k);
        let stub = a64_label(bp);
        let w = *(*sd).site;
        if (w & 0x7e000000) == 0x36000000 {
            if a64_try_patch_tbz((*sd).site, stub) == 0 {
                side_patch_oor = 1;
            }
        } else if (w & 0x7e000000) == 0x34000000 {
            a64_patch_cbz((*sd).site, stub);
        } else {
            a64_patch_bcond((*sd).site, stub);
        }
        let edge_class = body_edge_pin_class();
        let body_edge = (edge_class >= 0) as c_int;
        (*sd).stub = stub;
        for r in 0..16u32 {
            if ((*sd).l0_dirty as u32 & (1u32 << r)) != 0 && (*sd).l0[r as usize] >= 0 {
                if (*sd).l0_dbl[r as usize] != 0 {
                    a64_ins_d_d(bp, xmm_vreg(r), 0, (*sd).l0[r as usize] as c_int, 0);
                } else {
                    a64_ins_s_s(bp, xmm_vreg(r), 0, (*sd).l0[r as usize] as c_int, 0);
                }
            }
        }
        yc_flush_from(bp, (*sd).yc_dirty as _);
        if (*sd).fpb >= 0 && (*sd).fpb_chk != 0 {
            fpb_emit_regs_check(bp, (*sd).fpb_chk as _, (*sd).fpb as _, (*sd).fpb_end as _, (*sd).l0.as_mut_ptr(),
                (*sd).l0_dbl.as_mut_ptr());
        }
        if (*sd).rec != 0 {
            if (*sd).rec_imm_pending != 0 {
                a64_mov_imm64(bp, JT1, (*sd).rec_imm as u64);
            }
            emit_defer_flags(bp, (*sd).rec_ccop as _, (*sd).rec_src as _, (*sd).rec_dst as _);
        }
        if (*sd).probe != 0 && !(*blk).prof.is_null() {
            let pf = (*blk).prof.add(k);
            emit_prof_count(bp, pf, 0);
            (*pf).tk_trip = a64_label(bp);
            a64_tbnz(bp, JT2, PROBE_BIT as _, 0);
            (*sd).patch_b = emit_static_chain_tail(bp, (*sd).taken, 0, body_edge, epi_sites, &mut n_epi);
            a64_patch_tbz((*pf).tk_trip, a64_label(bp));
            g_tag_blk = blk;
            g_tag_idx = k as c_int;
            emit_static_chain_tail(bp, (*sd).taken, 0, body_edge, epi_sites, &mut n_epi);
            a64_patch_b((*sd).ft_site, a64_label(bp));
            emit_prof_count(bp, pf, 4);
            a64_tbnz(bp, JT2, PROBE_BIT as _, 2);
            let back = a64_label(bp);
            a64_b(bp, 0);
            a64_patch_b(back, (*sd).ft_site.add(1));
            for r in 0..16u32 {
                if ((*sd).l0_dirty as u32 & (1u32 << r)) != 0 && (*sd).l0[r as usize] >= 0 {
                    if (*sd).l0_dbl[r as usize] != 0 {
                        a64_ins_d_d(bp, xmm_vreg(r), 0, (*sd).l0[r as usize] as c_int, 0);
                    } else {
                        a64_ins_s_s(bp, xmm_vreg(r), 0, (*sd).l0[r as usize] as c_int, 0);
                    }
                }
            }
            yc_flush_from(bp, (*sd).yc_dirty as _);
            emit_static_chain_tail(bp, (*sd).ft_rip, 0, body_edge, epi_sites, &mut n_epi);
            g_tag_blk = null_mut();
            (*pf).ft_site = (*sd).ft_site;
        } else {
            if !(*sd).ft_site.is_null() {
                a64_patch_b((*sd).ft_site, (*sd).ft_site.add(1));
            }
            (*sd).patch_b = emit_static_chain_tail(bp, (*sd).taken, 0, body_edge, epi_sites, &mut n_epi);
        }
    }
    let gfpbmap = (&raw mut g_fpbmap).cast::<JitBlock_JitOslowMap>();
    for k in 0..g_n_fpb as usize {
        let fb = gfpb.add(k);
        if (*fb).site.is_null() {
            continue;
        }
        let lbl = a64_label(bp);
        a64_patch_bcond((*fb).site, lbl);
        if (*fb).gain != 0 {
            a64_patch_b((*fb).site.add((*fb).gain as usize), lbl);
        }
        fpb_replay_prelude(bp, fb, (*fb).l0.as_mut_ptr(), (*fb).l0_dbl.as_mut_ptr());
        yc_flush_from(bp, 0xffff);
        g_yc_dirty = 0;
        g_fpb_fast = 0;
        g_fpb_open = -1;
        l0_reset();
        ea_cache_reset();
        fpb_emit_undo_restore(bp, ins, (*fb).first as c_int, (*fb).end as c_int, exit_sites, &mut n_exits);
        for m in (*fb).first as c_int..=(*fb).end as c_int {
            let mu = m as usize;
            if (*ins.add(mu)).op as c_uint == OCERZ_OP_JCC {
                continue;
            }
            g_cur_insn_idx = m;
            g_cur_need = *fl_need.add(mu);
            g_cur_fpb = -1;
            let lo = a64_label(bp);
            lanerec_note(lo.offset_from(entry) as u32);
            l0_pre_insn(bp, ins.add(mu));
            if try_inline(bp, ins.add(mu), *fl_need.add(mu), exit_sites, &mut n_exits) == 0 {
                emit_slowcall(bp, ins.add(mu), exit_sites, &mut n_exits);
            }
            if g_n_fpbmap < JIT_MAX_BLOCK_INSNS as c_int {
                let e = gfpbmap.add(g_n_fpbmap as usize);
                (*e).lo = lo.offset_from(entry) as u32;
                (*e).hi = a64_label(bp).offset_from(entry) as u32;
                (*e).idx = m as _;
                g_n_fpbmap += 1;
            }
        }
        l0_flush_all(bp);
        for t in 4..4 + L0_NLANES as c_int {
            for r in 0..16u32 {
                if (*fb).l0[r as usize] as i8 != t as i8 {
                    continue;
                }
                if (*fb).l0_dbl[r as usize] != 0 {
                    a64_ins_d_d(bp, t, 0, xmm_vreg(r), 0);
                } else {
                    a64_ins_s_s(bp, t, 0, xmm_vreg(r), 0);
                }
                break;
            }
        }
        if (*fb).fcmp_vreg >= 0 {
            a64_fcmp(bp, 1, (*fb).fcmp_vreg as c_int, (*fb).fcmp_vreg as c_int);
        }
        let here = a64_label(bp);
        a64_b(bp, (((*fb).back as isize).wrapping_sub(here as isize) / 4) as i32);
    }
    for k in 0..g_n_fpb_sites as usize {
        let st = (&raw mut g_fpb_sites).cast::<FpbSite>().add(k);
        let fb = gfpb.add((*st).batch as usize);
        a64_patch_bcond((*st).site, a64_label(bp));
        if (*st).keep_jt != 0 {
            a64_stp_pre(bp, 9, 10, 31, -32);
            a64_stp_off(bp, 11, 12, 31, 16);
        }
        fpb_replay_prelude(bp, fb, (*st).l0.as_mut_ptr(), (*st).l0_dbl.as_mut_ptr());
        yc_flush_from(bp, 0xffff);
        g_yc_dirty = 0;
        g_fpb_fast = 0;
        g_fpb_open = -1;
        l0_reset();
        ea_cache_reset();
        fpb_emit_undo_restore(bp, ins, (*fb).first as c_int, (*st).end as c_int, exit_sites, &mut n_exits);
        for m in (*fb).first as c_int..=(*st).end as c_int {
            let mu = m as usize;
            if (*ins.add(mu)).op as c_uint == OCERZ_OP_JCC {
                continue;
            }
            g_cur_insn_idx = m;
            g_cur_need = *fl_need.add(mu);
            g_cur_fpb = -1;
            let lo = a64_label(bp);
            lanerec_note(lo.offset_from(entry) as u32);
            l0_pre_insn(bp, ins.add(mu));
            if try_inline(bp, ins.add(mu), *fl_need.add(mu), exit_sites, &mut n_exits) == 0 {
                emit_slowcall(bp, ins.add(mu), exit_sites, &mut n_exits);
            }
            if g_n_fpbmap < JIT_MAX_BLOCK_INSNS as c_int {
                let e = gfpbmap.add(g_n_fpbmap as usize);
                (*e).lo = lo.offset_from(entry) as u32;
                (*e).hi = a64_label(bp).offset_from(entry) as u32;
                (*e).idx = m as _;
                g_n_fpbmap += 1;
            }
        }
        l0_flush_all(bp);
        for t in 4..4 + L0_NLANES as c_int {
            for r in 0..16u32 {
                if (*st).l0[r as usize] as i8 != t as i8 {
                    continue;
                }
                if (*st).l0_dbl[r as usize] != 0 {
                    a64_ins_d_d(bp, t, 0, xmm_vreg(r), 0);
                } else {
                    a64_ins_s_s(bp, t, 0, xmm_vreg(r), 0);
                }
                break;
            }
        }
        if (*st).fcmp_a >= 0 {
            a64_fcmp(bp, (*st).fcmp_dbl as c_int, (*st).fcmp_a as c_int, (*st).fcmp_b as c_int);
        }
        if (*st).keep_jt != 0 {
            a64_ldp_off(bp, 11, 12, 31, 16);
            a64_ldp_post(bp, 9, 10, 31, 32);
        }
        let here = a64_label(bp);
        a64_b(bp, (((*st).back as isize).wrapping_sub(here as isize) / 4) as i32);
    }
    emit_oolslow_arms(bp, exit_sites, &mut n_exits);
    emit_x87_arms(bp, exit_sites, &mut n_exits, epi_sites, &mut n_epi);
    emit_low_hoist_bail(bp, rip, epi_sites, &mut n_epi);
    emit_guard_arms(bp, entry);
    emit_ordered_slow_arms(bp, blk, entry);
    emit_nan_ool_arms(bp, blk, entry);
    if !loop_poll_exit.is_null() {
        let poll_stub = a64_label(bp);
        a64_mov_imm64(bp, JT0, rip);
        a64_str(bp, 8, JT0, 20, RIP_OFF as u32);
        a64_mov_imm64(bp, 0, OCERZ_STEP_OK as u64);
        let here = a64_label(bp);
        a64_b(bp, exit_label.offset_from(here) as i32);
        a64_patch_cbz(loop_poll_exit, poll_stub);
    }

    let mut chain_tail_lbl: *mut u32 = null_mut();
    let mut chain_patch_b: *mut u32 = null_mut();
    let mut chain_is_body = 0;
    if g_no_chain == 0 && g_chain_target != 0 {
        chain_tail_lbl = a64_label(bp);
        if g_pin_class == 3 {
            if g_chain_keeps_jgb == 0 {
                emit_reload_jgb(bp);
            }
            chain_patch_b = emit_body_chain_tail(bp, g_chain_target, 0, epi_sites, &mut n_epi);
            chain_is_body = 1;
        } else {
            chain_patch_b = emit_chain_tail(bp, 0);
        }
    }

    if g_flaglive_log != 0 {
        let words = (*bp).p.offset_from(entry);
        libc::fprintf(crate::log::stderr(),
            c"ocerz: FLAGLIVE rip=%#llx EMITTED words=%d guest=%d per_guest=%.2f\n".as_ptr(),
            rip as libc::c_ulonglong, words as c_int, n, words as f64 / n as f64);
    }

    if g_n_raslit != 0 && (*bp).overflow == 0 {
        if ((*bp).p as usize & 7) != 0 {
            a64_emit32(bp, 0xd503201f);
        }
        g_tc_pool_off = (*bp).p.offset_from(entry) as u32;
        let ras = (&raw mut g_raslit).cast::<RasLit>();
        for i in 0..g_n_raslit as usize {
            let rl = ras.add(i);
            let cell = (*bp).p as *mut *mut c_void;
            a64_emit32(bp, 0);
            a64_emit32(bp, 0);
            if (*bp).overflow != 0 {
                break;
            }
            let off = ((cell as isize).wrapping_sub((*rl).site as isize) / 4) as i32;
            if (*rl).kind == 2 {
                a64_emit32(bp, 0);
                a64_emit32(bp, 0);
                if (*bp).overflow != 0 {
                    break;
                }
                *(*rl).site = 0x9c000000u32 | (((off as u32) & 0x7ffff) << 5) | ((*rl).rt as u32 & 31);
                *(cell as *mut u64) = (*rl).retaddr;
                *(cell as *mut u64).add(1) = (*rl).hi;
                continue;
            }
            *(*rl).site = 0x58000000u32 | (((off as u32) & 0x7ffff) << 5) | ((*rl).rt as u32 & 31);
            if (*rl).kind == 1 {
                *cell = (*rl).retaddr as usize as *mut c_void;
                if (*rl).tcr as c_uint == TCR_PSC as c_uint {
                    tc_note(cell as *mut u32, TCR_PSC as _, 1, 0);
                }
                continue;
            }
            if g_tc_on != 0 {
                *cell = null_mut();
                tc_note(cell as *mut u32, TCR_RASCELL as _, 1, (*rl).retaddr);
                continue;
            }
            ras_cell_register(cell);
            let rb = cache_lookup(g_xlat_jit, (*rl).retaddr, g_xlat_mode32);
            if !rb.is_null() && (*rb).code.is_some() {
                *cell = ras_entry_for(rb);
            } else {
                pending_add_ras(jit_key((*rl).retaddr, g_xlat_mode32), cell);
            }
        }
        g_n_raslit = 0;
    }

    if (*bp).overflow == 0 {
        for i in 0..n_exits as usize {
            a64_patch_cbz(*exit_sites.add(i), exit_label);
        }
        for i in 0..n_epi as usize {
            a64_patch_b(*epi_sites.add(i), exit_label);
        }

        if !chain_tail_lbl.is_null() && !g_chain_epi.is_null() {
            a64_patch_b(g_chain_epi, chain_tail_lbl);
        }
        if !g_stop_patch.is_null() {
            assert!(!g_stop_target.is_null());
            let running_insn = *g_stop_patch;
            (*blk).stop_patch = g_stop_patch;
            (*blk).stop_insn = stop_retarget(running_insn, g_stop_patch, g_stop_target);
            if (*jit).stop_requested != 0 {
                *g_stop_patch = (*blk).stop_insn;
            } else {
                *g_stop_patch = running_insn;
            }
        }
        (*blk).push_fix = null_mut();
        (*blk).n_push_fix = 0;
        if g_n_push_fix != 0 {
            (*blk).push_fix = libc::malloc(g_n_push_fix as usize * size_of::<u32>()) as *mut u32;
            if !(*blk).push_fix.is_null() {
                core::ptr::copy_nonoverlapping((&raw const g_push_fix).cast::<u32>(), (*blk).push_fix, g_n_push_fix as usize);
                (*blk).n_push_fix = g_n_push_fix as u16;
            }
        }
        (*blk).n_stop_extra = 0;
        let gse = (&raw mut g_stop_extra).cast::<JitState_g_stop_extra>();
        for i in 0..g_n_stop_extra as usize {
            let site = (*gse.add(i)).site;
            let ne = (*blk).n_stop_extra as usize;
            (*blk).stop_extra[ne].site = site;
            (*blk).stop_extra[ne].insn = stop_retarget(*site, site, (*gse.add(i)).target);
            (*blk).n_stop_extra += 1;
            if (*jit).stop_requested != 0 {
                *site = (*blk).stop_extra[ne].insn;
            }
        }
    }

    pthread_jit_write_protect_np(1);

    if side_patch_oor != 0 {
        static mut WARNED_OOR: c_int = 0;
        if WARNED_OOR == 0 {
            WARNED_OOR = 1;
            libc::fprintf(crate::log::stderr(),
                c"ocerz: note: superblock side exit out of TBZ range at rip=%#llx (%u words); block runs interpreted\n".as_ptr(),
                rip as libc::c_ulonglong, (*bp).p.offset_from(entry) as c_uint);
        }
        (*blk).n_slow = n as _;
        (*blk).n_inlined = 0;
        (*blk).n_pinned = 0;
        (*blk).pin_class = 0;
        (*blk).code = None;
        (*blk).body_code = null_mut();
        g_pin = null_mut();
        g_pin_hold = null_mut();
        g_n_pinned = 0;
        g_pin_class = 0;
        g_lowstack = 0;
        g_m32low = 0;
        cache_insert(jit, blk);
        return blk;
    }

    if (*bp).overflow != 0 {
        t_xlat_overflow = 1;
        static mut WARNED: c_int = 0;
        if (*jit).code_full == 0 {
            let w = WARNED;
            WARNED += 1;
            if w == 0 {
                libc::fprintf(crate::log::stderr(),
                    c"ocerz: warning: JIT code arena full (%zu MB, %llu blocks); it is flushed once every thread can leave it\n".as_ptr(),
                    ((*jit).code_bytes >> 20) as usize, (*jit).blocks_translated as libc::c_ulonglong);
            }
        }
        (*jit).code_full = 1;
        if ocerz_jitstat > 0 {
            js_fail_overflow += 1;
            js_note_fail(rip, JSR_OVERFLOW as c_uint, n);
        }

        (*blk).n_slow = n as _;
        (*blk).n_inlined = 0;
        (*blk).n_pinned = 0;
        (*blk).pin_class = 0;
        (*blk).code = None;
        g_pin = null_mut();
        g_pin_hold = null_mut();
        g_n_pinned = 0;
        g_pin_class = 0;
        g_lowstack = 0;
        g_m32low = 0;
        cache_insert(jit, blk);
        return blk;
    }

    sys_icache_invalidate(entry.cast(), (*bp).p.offset_from(entry) as usize * 4);
    (*jit).code_cur = (*bp).p;
    (*blk).code = core::mem::transmute::<*mut u32, JitBlockFn>(entry);
    (*blk).body_code = g_body_entry;
    (*blk).body_noreload = body_noreload;
    (*blk).hoist_sig = hoist_signature();
    (*blk).ordered_loads = g_blk_ordered_loads as u8;
    (*blk).code_words = (*bp).p.offset_from(entry) as u32;
    if !(*blk).stop_patch.is_null() || (*blk).n_stop_extra != 0 {
        (*blk).stop_next = (*jit).stop_blocks;
        (*jit).stop_blocks = blk;
    }

    assert!(!(!chain_patch_b.is_null() && (g_n_jcc_edges != 0 || g_n_call_edges != 0)),
        "block cannot mix legacy CALL, canonical CALL, and Jcc edges");
    assert!(!(g_n_jcc_edges != 0 && g_n_call_edges != 0), "block cannot have both canonical CALL and Jcc edges");
    let edges = (*blk).edges;
    if g_n_call_edges != 0 {
        for i in 0..g_n_call_edges as usize {
            let ce = &ga!(g_call_edge, i);
            (*edges.add(i)).target_rip = ce.target_rip;
            (*edges.add(i)).patch_b = ce.patch_b;
            (*edges.add(i)).cond_site = null_mut();
            (*edges.add(i)).kind = ce.kind as _;
            (*edges.add(i)).pin_class = ce.pin_class as _;
        }
        (*blk).n_edges = g_n_call_edges as u8;
    } else if !chain_patch_b.is_null() {
        (*edges).target_rip = g_chain_target;
        (*edges).patch_b = chain_patch_b;
        (*edges).cond_site = null_mut();
        (*edges).kind = (if chain_is_body != 0 { EDGE_BODY } else { EDGE_XBLOCK }) as _;
        (*edges).pin_class = if chain_is_body != 0 { 3 } else { 0 };
        (*blk).n_edges = 1;
    } else if g_n_jcc_edges != 0 {
        for i in 0..g_n_jcc_edges as usize {
            let je = &ga!(g_jcc_edge, i);
            (*edges.add(i)).target_rip = je.target_rip;
            (*edges.add(i)).patch_b = je.patch_b;
            (*edges.add(i)).cond_site = je.cond_site;
            (*edges.add(i)).kind = je.kind as _;
            (*edges.add(i)).pin_class = je.pin_class as _;
        }
        (*blk).n_edges = g_n_jcc_edges as u8;
    }
    let mut k = 0usize;
    while k < g_n_side as usize && (*blk).n_edges < 8 {
        let sd = gside.add(k);
        if (*sd).patch_b.is_null() {
            k += 1;
            continue;
        }
        let e = (*blk).n_edges as usize;
        (*blk).n_edges += 1;
        let ep = edges.add(e);
        (*ep).target_rip = (*sd).taken;
        (*ep).patch_b = (*sd).patch_b;
        (*ep).cond_site = if side_stub_has_work(k as c_int) != 0 { null_mut() } else { (*sd).site };
        (*ep).kind = (if body_edge_pin_class() >= 0 { EDGE_BODY } else { EDGE_XBLOCK }) as _;
        (*ep).pin_class = if body_edge_pin_class() >= 0 { body_edge_pin_class() as u8 } else { 0 };
        (*ep).side = (k + 1) as u8;
        (*ep).jcc_rip = (*sd).jcc_rip;
        (*ep).probing = ((*sd).probe != 0 && !(*blk).prof.is_null()) as u8;
        k += 1;
    }
    for i in 0..(*blk).n_edges as usize {
        let ep = edges.add(i);
        (*ep).fallback_insn = *(*ep).patch_b;
        (*ep).cond_orig = if (*ep).cond_site.is_null() { 0 } else { *(*ep).cond_site };
    }

    {
        static mut G_JITDIS: c_int = -1;
        static mut G_JF: *mut libc::FILE = null_mut();
        static mut G_JD_LO: u64 = 0;
        static mut G_JD_HI: u64 = 0;
        static mut G_JD_BRIEF: c_int = 0;
        if G_JITDIS < 0 {
            G_JD_BRIEF = if libc::getenv(c"OCERZ_JITDIS_BRIEF".as_ptr()).is_null() { 0 } else { 1 };
            let p = libc::getenv(c"OCERZ_JITDIS".as_ptr());
            G_JITDIS = if p.is_null() { 0 } else { 1 };
            let lo = libc::getenv(c"OCERZ_JITDIS_LO".as_ptr());
            let hi = libc::getenv(c"OCERZ_JITDIS_HI".as_ptr());
            G_JD_LO = if lo.is_null() { 0 } else { libc::strtoull(lo, null_mut(), 0) as u64 };
            G_JD_HI = if hi.is_null() { !0u64 } else { libc::strtoull(hi, null_mut(), 0) as u64 };
            if !p.is_null() {
                let mut pb = [0 as c_char; 1024];
                libc::snprintf(pb.as_mut_ptr(), pb.len(), c"%s.%d".as_ptr(), p, libc::getpid() as c_int);
                G_JF = libc::fopen(pb.as_ptr(), c"w".as_ptr());
                if !G_JF.is_null() {
                    libc::setvbuf(G_JF, null_mut(), libc::_IOFBF, 1usize << 20);
                }
            }
        }
        if G_JITDIS > 0 && !G_JF.is_null() && !(*blk).insn_off.is_null() && rip >= G_JD_LO && rip < G_JD_HI {
            let jf = G_JF;
            let mut tb = [0 as c_char; 128];
            let epi = exit_label.offset_from(entry) as u32;
            let io = (*blk).insn_off;
            libc::fprintf(jf,
                c"BLOCK rip=%#llx host=%p words=%u n_insns=%d inlined=%d slow=%d prologue_words=%u epilogue_words=%u pin_class=%d n_pinned=%d body=%d\n".as_ptr(),
                rip as libc::c_ulonglong, entry as *mut c_void, (*blk).code_words, n, (*blk).n_inlined as c_int,
                (*blk).n_slow as c_int, *io, (*blk).code_words - epi, (*blk).pin_class as c_int, (*blk).n_pinned as c_int,
                (!(*blk).body_code.is_null()) as c_int);
            let rel = |p: *mut u32| -> libc::c_long { if p.is_null() { -1 } else { p.offset_from(entry) as libc::c_long } };
            libc::fprintf(jf, c"  LABELS body_entry=%ld loop_entry=%ld stop_patch=%ld\n".as_ptr(), rel(g_body_entry),
                rel(g_loop_entry), rel(g_stop_patch));
            for e in 0..(*blk).n_edges as usize {
                let ep = edges.add(e);
                libc::fprintf(jf, c"  EDGE -> %#llx kind=%d pin_class=%d side=%d pb=+%ld cs=+%ld\n".as_ptr(),
                    (*ep).target_rip as libc::c_ulonglong, (*ep).kind as c_int, (*ep).pin_class as c_int, (*ep).side as c_int,
                    rel((*ep).patch_b), rel((*ep).cond_site));
            }
            if n > 0 && *io > 0 {
                libc::fprintf(jf, c"  PRO off=0 words=%u\n".as_ptr(), *io);
                let mut w = 0u32;
                while G_JD_BRIEF == 0 && w < *io {
                    libc::fprintf(jf, c"    %08x\n".as_ptr(), *entry.add(w as usize));
                    w += 1;
                }
            }
            for i in 0..n as usize {
                let s = *io.add(i);
                let e = if (i as c_int) + 1 < n { *io.add(i + 1) } else { epi };
                ocerz_format_insn(ins.add(i), tb.as_mut_ptr(), tb.len());
                libc::fprintf(jf, c"  INSN %d off=%u words=%u  %s\n".as_ptr(), i as c_int, s, if e > s { e - s } else { 0 },
                    tb.as_ptr());
                let mut w = s;
                while G_JD_BRIEF == 0 && w < e {
                    libc::fprintf(jf, c"    %08x\n".as_ptr(), *entry.add(w as usize));
                    w += 1;
                }
            }
            libc::fprintf(jf, c"  EPI off=%u words=%u\n".as_ptr(), epi, (*blk).code_words - epi);
            let mut w = epi;
            while G_JD_BRIEF == 0 && w < (*blk).code_words {
                libc::fprintf(jf, c"    %08x\n".as_ptr(), *entry.add(w as usize));
                w += 1;
            }
            libc::fflush(jf);
        }
    }

    #[cfg(ocerz_jit_emit_audit)]
    ocerz_jit_emit_audit(rip, entry, (*blk).code_words, (*blk).insns, n as u32, (&raw mut g_tc_rel).cast(), g_tc_nrel as u32);
    let mut tc_save = 0;
    if g_tc_on != 0 {
        if ocerz_tcache_mode() != OCERZ_TC_ROUNDTRIP as c_int || tc_roundtrip(jit, blk, entry).is_null() {
            pthread_jit_write_protect_np(0);
            tc_bind(jit, blk, entry, (&raw mut g_tc_rel).cast(), g_tc_nrel, 0);
            pthread_jit_write_protect_np(1);
        }
        tc_save = (g_tc_rec != 0 && g_tc_bad == 0) as c_int;
        g_tc_on = 0;
    }

    if code_index_append_locked(jit, blk) == 0 {
        static mut WARNED_CI: c_int = 0;
        if WARNED_CI == 0 {
            WARNED_CI = 1;
            libc::fprintf(crate::log::stderr(),
                c"ocerz: warning: JIT code index allocation failed; block %#llx runs interpreted\n".as_ptr(),
                rip as libc::c_ulonglong);
        }
        (*blk).n_slow = n as _;
        (*blk).n_inlined = 0;
        (*blk).n_pinned = 0;
        (*blk).pin_class = 0;
        (*blk).code = None;
        (*blk).body_code = null_mut();
        g_pin = null_mut();
        g_pin_hold = null_mut();
        g_n_pinned = 0;
        g_pin_class = 0;
        cache_insert(jit, blk);
        return blk;
    }

    {
        static mut EC: c_int = -1;
        if EC < 0 {
            EC = if libc::getenv(c"OCERZ_EMITCHECK".as_ptr()).is_null() { 0 } else { 1 };
        }
        if EC != 0 && (*blk).code.is_some() {
            let cw = core::mem::transmute::<JitBlockFn, *const u32>((*blk).code);
            for w in 0..(*blk).code_words {
                let v = *cw.add(w as usize);
                if (v & 0x7c000000) != 0x14000000 {
                    continue;
                }
                let off = (((v << 6) as i32) >> 6) as i64 * 4;
                let tgt = (cw.add(w as usize) as *const u8).wrapping_offset(off as isize) as *const u32;
                if (tgt as usize) < (*jit).code_base as usize || (tgt as usize) > (*jit).code_cur.wrapping_add(4096) as usize {
                    let mut ii: c_int = -1;
                    if !(*blk).insn_off.is_null() {
                        for k in 0..(*blk).n_insns as c_int {
                            if *(*blk).insn_off.add(k as usize) <= w {
                                ii = k;
                            }
                        }
                    }
                    libc::fprintf(crate::log::stderr(),
                        c"ocerz: EMITCHECK[%d] rip=%#llx word=%u insn=%d v=%08x tgt=%p arena=[%p,%p)\n".as_ptr(),
                        libc::getpid() as c_int, blk_rip(blk) as libc::c_ulonglong, w, ii, v, tgt as *const c_void,
                        (*jit).code_base as *mut c_void, (*jit).code_cur as *mut c_void);
                    libc::fprintf(crate::log::stderr(), c"ocerz: EMITCHECK[%d]   ctx:".as_ptr(), libc::getpid() as c_int);
                    let mut q = w as c_int - 6;
                    while q <= w as c_int + 6 && q < (*blk).code_words as c_int {
                        if q >= 0 {
                            libc::fprintf(crate::log::stderr(), c"%s%08x".as_ptr(),
                                if q == w as c_int { c" |".as_ptr() } else { c" ".as_ptr() }, *cw.add(q as usize));
                        }
                        q += 1;
                    }
                    libc::fprintf(crate::log::stderr(), c"\nocerz: EMITCHECK[%d]   edges:".as_ptr(), libc::getpid() as c_int);
                    let code = cw as *mut u32;
                    for q in 0..(*blk).n_edges as usize {
                        let ep = (*blk).edges.add(q);
                        let pb = if (*ep).patch_b.is_null() { -1 } else { (*ep).patch_b.offset_from(code) as libc::c_long };
                        let cs = if (*ep).cond_site.is_null() { -1 } else { (*ep).cond_site.offset_from(code) as libc::c_long };
                        libc::fprintf(crate::log::stderr(), c" [%d]tgt=%#llx pb=+%#lx cs=+%#lx".as_ptr(), q as c_int,
                            (*ep).target_rip as libc::c_ulonglong, pb, cs);
                    }
                    libc::fprintf(crate::log::stderr(), c" nool=%d nosl=%d nstop=%d\n".as_ptr(), g_n_oolslow, g_n_oslow,
                        (*blk).n_stop_extra as c_int);
                }
            }
        }
    }
    (*blk).lanerec = null_mut();
    (*blk).n_lanerec = 0;
    if g_n_lanerec > 0 {
        (*blk).lanerec = libc::malloc(g_n_lanerec as usize * pointee_size((*blk).lanerec)).cast();
        if !(*blk).lanerec.is_null() {
            core::ptr::copy_nonoverlapping((&raw const g_lanerec).cast(), (*blk).lanerec, g_n_lanerec as usize);
            (*blk).n_lanerec = g_n_lanerec as _;
        }
    }
    g_n_lanerec = 0;
    compact_block(blk);
    cache_insert(jit, blk);

    if tc_save != 0 {
        if !tc_hit.is_null() && ocerz_tcache_mode() == OCERZ_TC_VERIFY as c_int {
            tc_verify(jit, blk, tc_hit);
        } else {
            tc_put(jit, blk);
        }
    }
    blk_chain_install(jit, blk);

    (*jit).blocks_translated += 1;
    if ocerz_jitstat > 0 {
        js_xlat_ok += 1;
    }
    if ocerz_jit_time_xlat != 0 {
        core::sync::atomic::AtomicU64::from_ptr(&raw mut ocerz_jit_xlat_ns).fetch_add(
            clock_gettime_nsec_np(libc::CLOCK_UPTIME_RAW).wrapping_sub(xlat_t0), core::sync::atomic::Ordering::Relaxed);
    }
    if G_JITMEASURE != 0 {
        static mut M_XLAT_NS: u64 = 0;
        static mut M_XLAT_SC_NS: u64 = 0;
        static mut M_XLAT_N: c_uint = 0;
        static mut M_XLAT_SC_N: c_uint = 0;
        let ns = clock_gettime_nsec_np(libc::CLOCK_UPTIME_RAW).wrapping_sub(xlat_t0);
        M_XLAT_N = M_XLAT_N.wrapping_add(1);
        M_XLAT_NS = M_XLAT_NS.wrapping_add(ns);
        if rip >= 0x7ff800000000 {
            M_XLAT_SC_N = M_XLAT_SC_N.wrapping_add(1);
            M_XLAT_SC_NS = M_XLAT_SC_NS.wrapping_add(ns);
        }
        if (M_XLAT_N & 0x3fff) == 0 {
            libc::fprintf(crate::log::stderr(),
                c"ocerz: XLAT[%d] blocks=%u xlat_total=%llums | shared-cache: blocks=%u xlat=%llums\n".as_ptr(),
                libc::getpid() as c_int, M_XLAT_N, (M_XLAT_NS / 1000000) as libc::c_ulonglong, M_XLAT_SC_N,
                (M_XLAT_SC_NS / 1000000) as libc::c_ulonglong);
        }
    }
    blk
}

#[inline(always)]
unsafe fn ald(p: *mut u64) -> u64 {
    AtomicU64::from_ptr(p).load(Ordering::SeqCst)
}

unsafe extern "C" fn ps_cmp(a: *const c_void, bb: *const c_void) -> c_int {
    let x = (*(a as *const PsOpRow)).n;
    let y = (*(bb as *const PsOpRow)).n;
    if x < y {
        1
    } else if x > y {
        -1
    } else {
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ps_report(jit: *mut OcerzJit) {
    let err = crate::log::stderr();
    let pid = libc::getpid() as c_int;
    let mut sec = clock_gettime_nsec_np(libc::CLOCK_UPTIME_RAW).wrapping_sub(ps_t0) as f64 / 1e9;
    if sec <= 0.0 {
        sec = 1e-9;
    }
    let mut blk_exec: u64 = 0;
    let mut ins_inl: u64 = 0;
    let mut ins_slow_static: u64 = 0;
    let mut nblocks: u64 = 0;
    let mut ncompiled: u64 = 0;
    let mut static_insns: u64 = 0;
    let mut nonempty: u64 = 0;
    let mut maxchain: u64 = 0;
    let mut probe_w: u64 = 0;
    let buckets = (*jit).buckets.as_mut_ptr();
    for i in 0..JIT_HASH_SIZE as usize {
        let mut pos: u64 = 0;
        let mut b = AtomicPtr::from_ptr(buckets.add(i)).load(Ordering::Acquire);
        while !b.is_null() {
            nblocks += 1;
            pos += 1;
            if (*b).code.is_some() {
                ncompiled += 1;
            }
            static_insns += (*b).n_insns as c_uint as u64;
            let e = (*b).exec_count as u64;
            blk_exec = blk_exec.wrapping_add(e);
            probe_w = probe_w.wrapping_add(pos.wrapping_mul(e));
            ins_inl = ins_inl.wrapping_add(e.wrapping_mul((*b).n_inlined as c_uint as u64));
            ins_slow_static = ins_slow_static.wrapping_add(e.wrapping_mul((*b).n_slow as c_uint as u64));
            b = (*b).hnext;
        }
        if pos != 0 {
            nonempty += 1;
            if pos > maxchain {
                maxchain = pos;
            }
        }
    }
    let slow = ald((&raw mut ps_slow_insns).cast());
    let total = ins_inl.wrapping_add(slow);
    let st = ps_steps.load(Ordering::SeqCst);
    let hi = ps_hits.load(Ordering::SeqCst);
    let mi = ps_misses.load(Ordering::SeqCst);
    let pct = |a: u64, b: u64| if b != 0 { 100.0 * a as f64 / b as f64 } else { 0.0 };

    libc::fprintf(err,
        c"ocerz: PERFSTAT[%d] t=%.1fs blocks=%llu (compiled=%llu) static_insns/blk=%.2f\nocerz: PERFSTAT[%d]   EXECUTED insns: total=%llu  slow(exec_one)=%llu (%.2f%%)  inlined=%llu (%.2f%%)\nocerz: PERFSTAT[%d]   slow_static_est=%llu (guard-slowcalls = %lld)\nocerz: PERFSTAT[%d]   block_execs=%llu (%.0f/s)  jit_step/cache_lookup=%llu (%.0f/s) hits=%llu misses=%llu\nocerz: PERFSTAT[%d]   avg EXECUTED insns per block = %.2f   insns/s = %.0f\nocerz: PERFSTAT[%d]   HASH bits=%d buckets=%u nonempty=%llu load=%.3f maxchain=%llu mean_probes/lookup(exec-weighted)=%.3f\n".as_ptr(),
        pid, sec, nblocks, ncompiled, if nblocks != 0 { static_insns as f64 / nblocks as f64 } else { 0.0 },
        pid, total, slow, pct(slow, total), ins_inl, pct(ins_inl, total),
        pid, ins_slow_static, (slow as i64).wrapping_sub(ins_slow_static as i64),
        pid, blk_exec, blk_exec as f64 / sec, st, st as f64 / sec, hi, mi,
        pid, if blk_exec != 0 { total as f64 / blk_exec as f64 } else { 0.0 }, total as f64 / sec,
        pid, JIT_HASH_BITS as c_int, JIT_HASH_SIZE as c_uint, nonempty, nblocks as f64 / JIT_HASH_SIZE as f64, maxchain,
        if blk_exec != 0 { probe_w as f64 / blk_exec as f64 } else { 0.0 });

    {
        const HB: usize = 12;
        let mut top: [*mut JitBlock; HB] = [null_mut(); HB];
        let mut topw: [f64; HB] = [0.0; HB];
        for i in 0..JIT_HASH_SIZE as usize {
            let mut bb = AtomicPtr::from_ptr(buckets.add(i)).load(Ordering::Acquire);
            while !bb.is_null() {
                let w = (*bb).exec_count as f64 * (*bb).code_words as f64;
                for k in 0..HB {
                    if w > topw[k] {
                        let mut m = HB - 1;
                        while m > k {
                            top[m] = top[m - 1];
                            topw[m] = topw[m - 1];
                            m -= 1;
                        }
                        top[k] = bb;
                        topw[k] = w;
                        break;
                    }
                }
                bb = (*bb).hnext;
            }
        }
        let mut k = 0;
        while k < HB && !top[k].is_null() {
            let bb = top[k];
            libc::fprintf(err,
                c"ocerz: PERFSTAT[%d]   HOTBLOCK #%2d rip=%#llx execs=%llu insns=%d words=%u w/insn=%.1f pin=%d slow=%d\n".as_ptr(),
                pid, k as c_int + 1, blk_rip(bb) as libc::c_ulonglong, (*bb).exec_count as libc::c_ulonglong,
                (*bb).n_insns as c_int, (*bb).code_words,
                if (*bb).n_insns != 0 { (*bb).code_words as f64 / (*bb).n_insns as f64 } else { 0.0 },
                (*bb).pin_class as c_int, (*bb).n_slow as c_int);
            k += 1;
        }
    }
    let mut rows_mem = MaybeUninit::<[PsOpRow; OCERZ_OP_COUNT as usize]>::uninit();
    let rows = rows_mem.as_mut_ptr().cast::<PsOpRow>();
    let ops = (&raw mut ps_ops).cast::<u64>();
    for i in 0..OCERZ_OP_COUNT as usize {
        (*rows.add(i)).op = i as _;
        (*rows.add(i)).n = ald(ops.add(i)) as _;
    }
    libc::qsort(rows.cast(), OCERZ_OP_COUNT as usize, size_of::<PsOpRow>(), Some(ps_cmp));
    let mut cum: u64 = 0;
    let shapes = (&raw const ps_shapes).cast::<[[c_char; 96]; 3]>();
    let mut i = 0usize;
    while i < 24 && (*rows.add(i)).n != 0 {
        let r = &*rows.add(i);
        let rn = r.n as u64;
        cum += rn;
        let sh = &*shapes.add(r.op as usize);
        libc::fprintf(err,
            c"ocerz: PERFSTAT[%d]   SLOWOP #%2d %-12s %14llu  %5.2f%% of slow  cum %5.2f%%  (%.2f%% of ALL)  e.g. %s | %s | %s\n".as_ptr(),
            pid, i as c_int + 1, ocerz_op_name(r.op as _), rn, pct(rn, slow), pct(cum, slow), pct(rn, total),
            sh[0].as_ptr(), sh[1].as_ptr(), sh[2].as_ptr());
        i += 1;
    }
    {
        libc::fprintf(err,
            c"ocerz: PERFSTAT[%d]   RAS misses=%llu (stale=%llu, null entry=%llu, empty=%llu)  align-hotpatches=%llu  ras_slots=%u/%u call-sites-without-slot=%llu\n".as_ptr(),
            pid, ald((&raw mut ps_ras_miss).cast()), ald((&raw mut ps_ras_stale).cast()), ald((&raw mut ps_ras_null).cast()),
            ald((&raw mut ps_ras_sentinel).cast()), ald((&raw mut ps_align_patches).cast()), g_ras_slot_n as c_uint,
            RAS_SLOT_CAP as c_uint, ald((&raw mut ps_ras_noslot).cast()));
        {
            let mut top_n = [0u64; 12];
            let mut top_r = [0u64; 12];
            let rs = (&raw mut ps_retsite).cast::<JitState_ps_retsite>();
            for i in 0..PS_RETSITE_N as usize {
                let n = (*rs.add(i)).n as u64;
                for k in 0..12 {
                    if n > top_n[k] {
                        let mut q = 11;
                        while q > k {
                            top_n[q] = top_n[q - 1];
                            top_r[q] = top_r[q - 1];
                            q -= 1;
                        }
                        top_n[k] = n;
                        top_r[k] = (*rs.add(i)).rip as u64;
                        break;
                    }
                }
            }
            let mut k = 0;
            while k < 12 && top_n[k] != 0 {
                libc::fprintf(err, c"ocerz: PERFSTAT[%d]   RETMISS #%d rip=%#llx stale=%llu\n".as_ptr(), pid, k as c_int + 1,
                    top_r[k] as libc::c_ulonglong, top_n[k] as libc::c_ulonglong);
                k += 1;
            }
        }
        let cok = ald((&raw mut ps_chain_ok).cast());
        let cfar = ald((&raw mut ps_chain_far).cast());
        let cven = ald((&raw mut ps_chain_veneer).cast());
        let ctot = cok.wrapping_add(cfar).wrapping_add(cven);
        if ctot != 0 {
            libc::fprintf(err,
                c"ocerz: PERFSTAT[%d]   CHAIN activated=%llu veneered=%llu out_of_range=%llu (%.2f%% dropped)\n".as_ptr(),
                pid, cok, cven, cfar, 100.0 * cfar as f64 / ctot as f64);
        }
    }
    let shp = (&raw mut ps_shape).cast::<[u64; 2]>();
    let nshape = size_of_val(&*(&raw const ps_shape)) / size_of::<[u64; 2]>();
    for i in 0..nshape {
        let easy = AtomicU64::from_ptr((*shp.add(i)).as_mut_ptr()).load(Ordering::SeqCst);
        let hard = AtomicU64::from_ptr((*shp.add(i)).as_mut_ptr().add(1)).load(Ordering::SeqCst);
        let s = easy.wrapping_add(hard);
        if s != 0 {
            libc::fprintf(err, c"ocerz: PERFSTAT[%d]   SHAPE %-7s easy=%llu (%.1f%%) other=%llu (%.1f%%)\n".as_ptr(), pid,
                PS_SHAPE_NAME[i].as_ptr(), easy, 100.0 * easy as f64 / s as f64, hard, 100.0 * hard as f64 / s as f64);
        }
    }
}

static PS_SHAPE_NAME: [&core::ffi::CStr; 9] = [c"push", c"pop", c"test", c"movsxd", c"call", c"ret", c"jmp", c"jmpind", c"jmpmem"];

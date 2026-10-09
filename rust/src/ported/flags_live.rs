//! The per-instruction flag def/use table behind flags_live.h.
//!
//! The table drives the JIT's decision to skip computing flags nothing reads, so
//! every entry is a claim about what an instruction may do, and the safe
//! direction is always "defines less, uses more".  DIV/IDIV leave the flags
//! architecturally UNDEFINED, but undefined is not killed: ocerz's interpreter
//! produces specific values there and the differential gate compares them, so
//! they are treated as defined.  Variable shifts by %cl, the rotates and
//! SHLD/SHRD are MAY-define, because a count that masks to zero preserves the
//! flags while any other count writes them - a distinction that cannot be made
//! statically.  SSE compares define everything (they write ZF/PF/CF and clear
//! OF/SF/AF), bsf/bsr write only ZF and leave the rest, tzcnt/lzcnt write CF and
//! ZF, popcnt writes all.  Outside the compare-into-RFLAGS forms, FCMOVcc and
//! PTEST, the x87 and SSE regions are flag-neutral.
//!
//! Memory-touching instructions are fault barriers: every flag is forced live
//! across them so a guest fault handler observes the flags the hardware would
//! have left.

use core::sync::atomic::{AtomicI32, Ordering};

use crate::ffi::*;
use crate::inline::{OCERZ_AF, OCERZ_CF, OCERZ_OF, OCERZ_PF, OCERZ_SF, OCERZ_ZF};

pub const OCERZ_FL_ALL: u64 = OCERZ_CF | OCERZ_PF | OCERZ_AF | OCERZ_ZF | OCERZ_SF | OCERZ_OF;

fn insn_touches_memory(insn: &X86Insn) -> bool {
    match insn.op as u32 {
        OCERZ_OP_LEA | OCERZ_OP_PREFETCH | OCERZ_OP_CLFLUSH | OCERZ_OP_NOP => return false,
        OCERZ_OP_PUSH | OCERZ_OP_POP | OCERZ_OP_PUSHF | OCERZ_OP_POPF | OCERZ_OP_CALL
        | OCERZ_OP_RET | OCERZ_OP_LEAVE | OCERZ_OP_MOVS | OCERZ_OP_STOS | OCERZ_OP_LODS
        | OCERZ_OP_SCAS | OCERZ_OP_CMPS => return true,
        _ => {}
    }
    for i in 0..insn.nops as usize {
        if insn.ops[i].kind as u32 == OCERZ_OPK_MEM {
            return true;
        }
    }
    false
}

static FAULT_BARRIER: AtomicI32 = AtomicI32::new(-1);

fn fault_barrier_enabled() -> bool {
    let mut en = FAULT_BARRIER.load(Ordering::Relaxed);
    if en < 0 {
        en = unsafe { !libc::getenv(c"OCERZ_FAULT_FLAG_BARRIER".as_ptr()).is_null() } as i32;
        FAULT_BARRIER.store(en, Ordering::Relaxed);
    }
    en != 0
}

#[inline(always)]
fn cc_use(cc: u8) -> u64 {
    match cc >> 1 {
        0 => OCERZ_OF,
        1 => OCERZ_CF,
        2 => OCERZ_ZF,
        3 => OCERZ_CF | OCERZ_ZF,
        4 => OCERZ_SF,
        5 => OCERZ_PF,
        6 => OCERZ_SF | OCERZ_OF,
        _ => OCERZ_ZF | OCERZ_SF | OCERZ_OF,
    }
}

#[inline(always)]
fn flags_defuse(insn: &X86Insn, fault_barrier: bool) -> (u64, u64) {
    let mut d: u64 = 0;
    let mut u: u64 = OCERZ_FL_ALL;

    match insn.op as u32 {
        OCERZ_OP_MOV | OCERZ_OP_MOVZX | OCERZ_OP_MOVSX | OCERZ_OP_MOVSXD | OCERZ_OP_LEA
        | OCERZ_OP_NOP | OCERZ_OP_PAUSE | OCERZ_OP_PREFETCH | OCERZ_OP_CLFLUSH
        | OCERZ_OP_MFENCE | OCERZ_OP_LFENCE | OCERZ_OP_SFENCE | OCERZ_OP_BSWAP | OCERZ_OP_NOT
        | OCERZ_OP_PUSH | OCERZ_OP_POP | OCERZ_OP_CALL | OCERZ_OP_RET | OCERZ_OP_LEAVE
        | OCERZ_OP_XCHG | OCERZ_OP_CBW | OCERZ_OP_CWD | OCERZ_OP_MOVS | OCERZ_OP_STOS
        | OCERZ_OP_LODS | OCERZ_OP_CLD | OCERZ_OP_STD | OCERZ_OP_CPUID | OCERZ_OP_RDTSC
        | OCERZ_OP_RDTSCP | OCERZ_OP_MOVSEG | OCERZ_OP_FXSAVE | OCERZ_OP_FXRSTOR
        | OCERZ_OP_LDMXCSR | OCERZ_OP_STMXCSR | OCERZ_OP_XGETBV | OCERZ_OP_XSAVE
        | OCERZ_OP_XRSTOR | OCERZ_OP_MOVBE | OCERZ_OP_PDEP | OCERZ_OP_PEXT | OCERZ_OP_MULX
        | OCERZ_OP_RORX | OCERZ_OP_SARX | OCERZ_OP_SHLX | OCERZ_OP_SHRX => {
            d = 0;
            u = 0;
        }
        OCERZ_OP_POPF => {
            d = OCERZ_FL_ALL;
            u = 0;
        }
        OCERZ_OP_LAHF => {
            d = 0;
            u = OCERZ_SF | OCERZ_ZF | OCERZ_AF | OCERZ_PF | OCERZ_CF;
        }
        OCERZ_OP_ADD | OCERZ_OP_SUB | OCERZ_OP_CMP | OCERZ_OP_AND | OCERZ_OP_OR | OCERZ_OP_XOR
        | OCERZ_OP_TEST | OCERZ_OP_NEG => {
            d = OCERZ_FL_ALL;
            u = 0;
        }
        OCERZ_OP_MUL | OCERZ_OP_IMUL => {
            d = OCERZ_FL_ALL;
            u = 0;
        }
        OCERZ_OP_DIV | OCERZ_OP_IDIV => {
            d = 0;
            u = 0;
        }
        OCERZ_OP_ADC | OCERZ_OP_SBB => {
            d = OCERZ_FL_ALL;
            u = OCERZ_CF;
        }
        OCERZ_OP_UCOMISS | OCERZ_OP_UCOMISD | OCERZ_OP_COMISS | OCERZ_OP_COMISD => {
            d = OCERZ_FL_ALL;
            u = 0;
        }
        OCERZ_OP_SETCC | OCERZ_OP_CMOVCC => {
            d = 0;
            u = cc_use(insn.cc);
        }
        OCERZ_OP_INC | OCERZ_OP_DEC => {
            d = OCERZ_FL_ALL & !OCERZ_CF;
            u = 0;
        }
        OCERZ_OP_SHL | OCERZ_OP_SHR | OCERZ_OP_SAR => {
            if insn.nops >= 2 && insn.ops[1].kind as u32 == OCERZ_OPK_IMM {
                let cnt = (insn.ops[1].imm & if insn.ops[0].size == 8 { 63 } else { 31 }) as u32;
                if cnt == 0 {
                    d = 0;
                    u = 0;
                } else {
                    d = OCERZ_FL_ALL;
                    u = 0;
                }
            } else {
                d = OCERZ_FL_ALL;
                u = OCERZ_FL_ALL;
            }
        }
        OCERZ_OP_ROL | OCERZ_OP_ROR | OCERZ_OP_RCL | OCERZ_OP_RCR => {
            d = OCERZ_FL_ALL;
            u = OCERZ_FL_ALL;
        }
        OCERZ_OP_SCAS | OCERZ_OP_CMPS => {
            d = OCERZ_FL_ALL;
            u = OCERZ_FL_ALL;
        }
        OCERZ_OP_JCC => {
            d = 0;
            u = cc_use(insn.cc);
        }
        OCERZ_OP_JMP | OCERZ_OP_JRCXZ | OCERZ_OP_LOOP => {
            d = 0;
            u = 0;
        }
        OCERZ_OP_LOOPE | OCERZ_OP_LOOPNE => {
            d = 0;
            u = OCERZ_ZF;
        }
        OCERZ_OP_FCOMI | OCERZ_OP_FCOMIP | OCERZ_OP_FUCOMI | OCERZ_OP_FUCOMIP | OCERZ_OP_PTEST
        | OCERZ_OP_VTESTPS | OCERZ_OP_VTESTPD | OCERZ_OP_PCMPESTRM | OCERZ_OP_PCMPESTRI
        | OCERZ_OP_PCMPISTRM | OCERZ_OP_PCMPISTRI => {
            d = OCERZ_FL_ALL;
            u = 0;
        }
        OCERZ_OP_FCMOVCC => {
            d = 0;
            u = OCERZ_CF | OCERZ_ZF | OCERZ_PF;
        }
        OCERZ_OP_BSF | OCERZ_OP_BSR => {
            d = OCERZ_ZF;
            u = 0;
        }
        OCERZ_OP_TZCNT | OCERZ_OP_LZCNT => {
            d = OCERZ_CF | OCERZ_ZF;
            u = 0;
        }
        OCERZ_OP_POPCNT | OCERZ_OP_RDRAND => {
            d = OCERZ_FL_ALL;
            u = 0;
        }
        OCERZ_OP_ANDN | OCERZ_OP_BLSR | OCERZ_OP_BLSMSK | OCERZ_OP_BLSI | OCERZ_OP_BZHI => {
            d = OCERZ_CF | OCERZ_ZF | OCERZ_SF | OCERZ_OF;
            u = 0;
        }
        OCERZ_OP_BEXTR => {
            d = OCERZ_CF | OCERZ_ZF | OCERZ_OF;
            u = 0;
        }
        OCERZ_OP_BT | OCERZ_OP_BTS | OCERZ_OP_BTR | OCERZ_OP_BTC => {
            d = OCERZ_CF;
            u = 0;
        }
        OCERZ_OP_SHLD | OCERZ_OP_SHRD => {
            if insn.nops >= 3 && insn.ops[2].kind as u32 == OCERZ_OPK_IMM {
                let scnt = (insn.ops[2].imm & if insn.ops[0].size == 8 { 63 } else { 31 }) as u32;
                if scnt == 0 {
                    d = 0;
                    u = 0;
                } else {
                    d = OCERZ_FL_ALL;
                    u = 0;
                }
            } else {
                d = OCERZ_FL_ALL;
                u = OCERZ_FL_ALL;
            }
        }
        OCERZ_OP_XADD | OCERZ_OP_CMPXCHG => {
            d = OCERZ_FL_ALL;
            u = 0;
        }
        op => {
            if (op >= OCERZ_OP_X87_FIRST && op < OCERZ_OP_PUSHA) || insn.vex != 0 {
                d = 0;
                u = 0;
            }
        }
    }

    if fault_barrier && fault_barrier_enabled() && insn_touches_memory(insn) {
        u = OCERZ_FL_ALL;
    }
    (d, u)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_flags_defuse(insn: *const X86Insn, def: *mut u64, use_: *mut u64) {
    unsafe {
        let (d, u) = flags_defuse(&*insn, true);
        *def = d;
        *use_ = u;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_flags_defuse_nofault(
    insn: *const X86Insn,
    def: *mut u64,
    use_: *mut u64,
) {
    unsafe {
        let (d, u) = flags_defuse(&*insn, false);
        *def = d;
        *use_ = u;
    }
}

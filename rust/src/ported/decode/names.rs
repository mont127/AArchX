//! Opcode names and human-readable instruction formatting.

use super::*;
use std::sync::Once;

const OP_COUNT: usize = OCERZ_OP_COUNT as usize;

static OP_NAMES_ONCE: Once = Once::new();
static mut OP_NAMES: [*const c_char; OP_COUNT] = [ptr::null(); OP_COUNT];
static mut FMA_NAMES: [[c_char; 16]; 60] = [[0; 16]; 60];

const GPR64: [*const c_char; 16] = [
    c"rax".as_ptr(),
    c"rcx".as_ptr(),
    c"rdx".as_ptr(),
    c"rbx".as_ptr(),
    c"rsp".as_ptr(),
    c"rbp".as_ptr(),
    c"rsi".as_ptr(),
    c"rdi".as_ptr(),
    c"r8".as_ptr(),
    c"r9".as_ptr(),
    c"r10".as_ptr(),
    c"r11".as_ptr(),
    c"r12".as_ptr(),
    c"r13".as_ptr(),
    c"r14".as_ptr(),
    c"r15".as_ptr(),
];
const GPR32: [*const c_char; 16] = [
    c"eax".as_ptr(),
    c"ecx".as_ptr(),
    c"edx".as_ptr(),
    c"ebx".as_ptr(),
    c"esp".as_ptr(),
    c"ebp".as_ptr(),
    c"esi".as_ptr(),
    c"edi".as_ptr(),
    c"r8d".as_ptr(),
    c"r9d".as_ptr(),
    c"r10d".as_ptr(),
    c"r11d".as_ptr(),
    c"r12d".as_ptr(),
    c"r13d".as_ptr(),
    c"r14d".as_ptr(),
    c"r15d".as_ptr(),
];
const GPR16: [*const c_char; 16] = [
    c"ax".as_ptr(),
    c"cx".as_ptr(),
    c"dx".as_ptr(),
    c"bx".as_ptr(),
    c"sp".as_ptr(),
    c"bp".as_ptr(),
    c"si".as_ptr(),
    c"di".as_ptr(),
    c"r8w".as_ptr(),
    c"r9w".as_ptr(),
    c"r10w".as_ptr(),
    c"r11w".as_ptr(),
    c"r12w".as_ptr(),
    c"r13w".as_ptr(),
    c"r14w".as_ptr(),
    c"r15w".as_ptr(),
];
const GPR8: [*const c_char; 16] = [
    c"al".as_ptr(),
    c"cl".as_ptr(),
    c"dl".as_ptr(),
    c"bl".as_ptr(),
    c"spl".as_ptr(),
    c"bpl".as_ptr(),
    c"sil".as_ptr(),
    c"dil".as_ptr(),
    c"r8b".as_ptr(),
    c"r9b".as_ptr(),
    c"r10b".as_ptr(),
    c"r11b".as_ptr(),
    c"r12b".as_ptr(),
    c"r13b".as_ptr(),
    c"r14b".as_ptr(),
    c"r15b".as_ptr(),
];
const GPR_HIGH8: [*const c_char; 4] = [
    c"ah".as_ptr(),
    c"ch".as_ptr(),
    c"dh".as_ptr(),
    c"bh".as_ptr(),
];

unsafe fn set_name(op: usize, name: *const c_char) {
    ptr::addr_of_mut!(OP_NAMES)
        .cast::<*const c_char>()
        .add(op)
        .write(name);
}

unsafe fn set_all_op_names() {
    set_name((OCERZ_OP_INVALID) as usize, c"(invalid)".as_ptr());
    set_name((OCERZ_OP_MOV) as usize, c"mov".as_ptr());
    set_name((OCERZ_OP_MOVZX) as usize, c"movzx".as_ptr());
    set_name((OCERZ_OP_MOVSX) as usize, c"movsx".as_ptr());
    set_name((OCERZ_OP_MOVSXD) as usize, c"movsxd".as_ptr());
    set_name((OCERZ_OP_LEA) as usize, c"lea".as_ptr());
    set_name((OCERZ_OP_XCHG) as usize, c"xchg".as_ptr());
    set_name((OCERZ_OP_BSWAP) as usize, c"bswap".as_ptr());
    set_name((OCERZ_OP_PUSH) as usize, c"push".as_ptr());
    set_name((OCERZ_OP_POP) as usize, c"pop".as_ptr());
    set_name((OCERZ_OP_PUSHF) as usize, c"pushf".as_ptr());
    set_name((OCERZ_OP_POPF) as usize, c"popf".as_ptr());
    set_name((OCERZ_OP_LAHF) as usize, c"lahf".as_ptr());
    set_name((OCERZ_OP_SAHF) as usize, c"sahf".as_ptr());
    set_name((OCERZ_OP_CBW) as usize, c"cbw".as_ptr());
    set_name((OCERZ_OP_CWD) as usize, c"cwd".as_ptr());
    set_name((OCERZ_OP_CMOVCC) as usize, c"cmovcc".as_ptr());
    set_name((OCERZ_OP_SETCC) as usize, c"setcc".as_ptr());
    set_name((OCERZ_OP_ADD) as usize, c"add".as_ptr());
    set_name((OCERZ_OP_ADC) as usize, c"adc".as_ptr());
    set_name((OCERZ_OP_SUB) as usize, c"sub".as_ptr());
    set_name((OCERZ_OP_SBB) as usize, c"sbb".as_ptr());
    set_name((OCERZ_OP_AND) as usize, c"and".as_ptr());
    set_name((OCERZ_OP_OR) as usize, c"or".as_ptr());
    set_name((OCERZ_OP_XOR) as usize, c"xor".as_ptr());
    set_name((OCERZ_OP_CMP) as usize, c"cmp".as_ptr());
    set_name((OCERZ_OP_TEST) as usize, c"test".as_ptr());
    set_name((OCERZ_OP_INC) as usize, c"inc".as_ptr());
    set_name((OCERZ_OP_DEC) as usize, c"dec".as_ptr());
    set_name((OCERZ_OP_NEG) as usize, c"neg".as_ptr());
    set_name((OCERZ_OP_NOT) as usize, c"not".as_ptr());
    set_name((OCERZ_OP_MUL) as usize, c"mul".as_ptr());
    set_name((OCERZ_OP_IMUL) as usize, c"imul".as_ptr());
    set_name((OCERZ_OP_DIV) as usize, c"div".as_ptr());
    set_name((OCERZ_OP_IDIV) as usize, c"idiv".as_ptr());
    set_name((OCERZ_OP_SHL) as usize, c"shl".as_ptr());
    set_name((OCERZ_OP_SHR) as usize, c"shr".as_ptr());
    set_name((OCERZ_OP_SAR) as usize, c"sar".as_ptr());
    set_name((OCERZ_OP_ROL) as usize, c"rol".as_ptr());
    set_name((OCERZ_OP_ROR) as usize, c"ror".as_ptr());
    set_name((OCERZ_OP_RCL) as usize, c"rcl".as_ptr());
    set_name((OCERZ_OP_RCR) as usize, c"rcr".as_ptr());
    set_name((OCERZ_OP_SHLD) as usize, c"shld".as_ptr());
    set_name((OCERZ_OP_SHRD) as usize, c"shrd".as_ptr());
    set_name((OCERZ_OP_BT) as usize, c"bt".as_ptr());
    set_name((OCERZ_OP_BTS) as usize, c"bts".as_ptr());
    set_name((OCERZ_OP_BTR) as usize, c"btr".as_ptr());
    set_name((OCERZ_OP_BTC) as usize, c"btc".as_ptr());
    set_name((OCERZ_OP_BSF) as usize, c"bsf".as_ptr());
    set_name((OCERZ_OP_BSR) as usize, c"bsr".as_ptr());
    set_name((OCERZ_OP_POPCNT) as usize, c"popcnt".as_ptr());
    set_name((OCERZ_OP_TZCNT) as usize, c"tzcnt".as_ptr());
    set_name((OCERZ_OP_LZCNT) as usize, c"lzcnt".as_ptr());
    set_name((OCERZ_OP_JMP) as usize, c"jmp".as_ptr());
    set_name((OCERZ_OP_JCC) as usize, c"jcc".as_ptr());
    set_name((OCERZ_OP_JRCXZ) as usize, c"jrcxz".as_ptr());
    set_name((OCERZ_OP_LOOP) as usize, c"loop".as_ptr());
    set_name((OCERZ_OP_LOOPE) as usize, c"loope".as_ptr());
    set_name((OCERZ_OP_LOOPNE) as usize, c"loopne".as_ptr());
    set_name((OCERZ_OP_CALL) as usize, c"call".as_ptr());
    set_name((OCERZ_OP_RET) as usize, c"ret".as_ptr());
    set_name((OCERZ_OP_IRET) as usize, c"iret".as_ptr());
    set_name((OCERZ_OP_JMPF) as usize, c"jmpf".as_ptr());
    set_name((OCERZ_OP_CALLF) as usize, c"callf".as_ptr());
    set_name((OCERZ_OP_RETF) as usize, c"retf".as_ptr());
    set_name((OCERZ_OP_MOVSEG) as usize, c"mov_sreg".as_ptr());
    set_name((OCERZ_OP_MOVFROMSEG) as usize, c"mov_from_sreg".as_ptr());
    set_name((OCERZ_OP_LEAVE) as usize, c"leave".as_ptr());
    set_name((OCERZ_OP_INT3) as usize, c"int3".as_ptr());
    set_name((OCERZ_OP_INT) as usize, c"int".as_ptr());
    set_name((OCERZ_OP_UD2) as usize, c"ud2".as_ptr());
    set_name((OCERZ_OP_HLT) as usize, c"hlt".as_ptr());
    set_name((OCERZ_OP_MOVS) as usize, c"movs".as_ptr());
    set_name((OCERZ_OP_STOS) as usize, c"stos".as_ptr());
    set_name((OCERZ_OP_LODS) as usize, c"lods".as_ptr());
    set_name((OCERZ_OP_SCAS) as usize, c"scas".as_ptr());
    set_name((OCERZ_OP_CMPS) as usize, c"cmps".as_ptr());
    set_name((OCERZ_OP_XADD) as usize, c"xadd".as_ptr());
    set_name((OCERZ_OP_CMPXCHG) as usize, c"cmpxchg".as_ptr());
    set_name((OCERZ_OP_CMPXCHGXB) as usize, c"cmpxchgxb".as_ptr());
    set_name((OCERZ_OP_CLC) as usize, c"clc".as_ptr());
    set_name((OCERZ_OP_STC) as usize, c"stc".as_ptr());
    set_name((OCERZ_OP_CMC) as usize, c"cmc".as_ptr());
    set_name((OCERZ_OP_CLD) as usize, c"cld".as_ptr());
    set_name((OCERZ_OP_STD) as usize, c"std".as_ptr());
    set_name((OCERZ_OP_SYSCALL) as usize, c"syscall".as_ptr());
    set_name((OCERZ_OP_CPUID) as usize, c"cpuid".as_ptr());
    set_name((OCERZ_OP_RDTSC) as usize, c"rdtsc".as_ptr());
    set_name((OCERZ_OP_RDTSCP) as usize, c"rdtscp".as_ptr());
    set_name((OCERZ_OP_PAUSE) as usize, c"pause".as_ptr());
    set_name((OCERZ_OP_NOP) as usize, c"nop".as_ptr());
    set_name((OCERZ_OP_MFENCE) as usize, c"mfence".as_ptr());
    set_name((OCERZ_OP_LFENCE) as usize, c"lfence".as_ptr());
    set_name((OCERZ_OP_SFENCE) as usize, c"sfence".as_ptr());
    set_name((OCERZ_OP_XGETBV) as usize, c"xgetbv".as_ptr());
    set_name((OCERZ_OP_SGDT) as usize, c"sgdt".as_ptr());
    set_name((OCERZ_OP_SIDT) as usize, c"sidt".as_ptr());
    set_name((OCERZ_OP_FXSAVE) as usize, c"fxsave".as_ptr());
    set_name((OCERZ_OP_FXRSTOR) as usize, c"fxrstor".as_ptr());
    set_name((OCERZ_OP_LDMXCSR) as usize, c"ldmxcsr".as_ptr());
    set_name((OCERZ_OP_STMXCSR) as usize, c"stmxcsr".as_ptr());
    set_name((OCERZ_OP_PREFETCH) as usize, c"prefetch".as_ptr());
    set_name((OCERZ_OP_CLFLUSH) as usize, c"clflush".as_ptr());
    set_name((OCERZ_OP_EMMS) as usize, c"emms".as_ptr());
    set_name((OCERZ_OP_FLD) as usize, c"fld".as_ptr());
    set_name((OCERZ_OP_FST) as usize, c"fst".as_ptr());
    set_name((OCERZ_OP_FSTP) as usize, c"fstp".as_ptr());
    set_name((OCERZ_OP_FILD) as usize, c"fild".as_ptr());
    set_name((OCERZ_OP_FIST) as usize, c"fist".as_ptr());
    set_name((OCERZ_OP_FISTP) as usize, c"fistp".as_ptr());
    set_name((OCERZ_OP_FLDCW) as usize, c"fldcw".as_ptr());
    set_name((OCERZ_OP_FNSTCW) as usize, c"fnstcw".as_ptr());
    set_name((OCERZ_OP_FNSTSW) as usize, c"fnstsw".as_ptr());
    set_name((OCERZ_OP_FNSTENV) as usize, c"fnstenv".as_ptr());
    set_name((OCERZ_OP_FLDENV) as usize, c"fldenv".as_ptr());
    set_name((OCERZ_OP_FXCH) as usize, c"fxch".as_ptr());
    set_name((OCERZ_OP_FCHS) as usize, c"fchs".as_ptr());
    set_name((OCERZ_OP_FABS) as usize, c"fabs".as_ptr());
    set_name((OCERZ_OP_FADD) as usize, c"fadd".as_ptr());
    set_name((OCERZ_OP_FADDP) as usize, c"faddp".as_ptr());
    set_name((OCERZ_OP_FIADD) as usize, c"fiadd".as_ptr());
    set_name((OCERZ_OP_FSUB) as usize, c"fsub".as_ptr());
    set_name((OCERZ_OP_FSUBR) as usize, c"fsubr".as_ptr());
    set_name((OCERZ_OP_FSUBP) as usize, c"fsubp".as_ptr());
    set_name((OCERZ_OP_FSUBRP) as usize, c"fsubrp".as_ptr());
    set_name((OCERZ_OP_FISUB) as usize, c"fisub".as_ptr());
    set_name((OCERZ_OP_FISUBR) as usize, c"fisubr".as_ptr());
    set_name((OCERZ_OP_FMUL) as usize, c"fmul".as_ptr());
    set_name((OCERZ_OP_FMULP) as usize, c"fmulp".as_ptr());
    set_name((OCERZ_OP_FIMUL) as usize, c"fimul".as_ptr());
    set_name((OCERZ_OP_FDIV) as usize, c"fdiv".as_ptr());
    set_name((OCERZ_OP_FDIVR) as usize, c"fdivr".as_ptr());
    set_name((OCERZ_OP_FDIVP) as usize, c"fdivp".as_ptr());
    set_name((OCERZ_OP_FDIVRP) as usize, c"fdivrp".as_ptr());
    set_name((OCERZ_OP_FIDIV) as usize, c"fidiv".as_ptr());
    set_name((OCERZ_OP_FIDIVR) as usize, c"fidivr".as_ptr());
    set_name((OCERZ_OP_FCOM) as usize, c"fcom".as_ptr());
    set_name((OCERZ_OP_FCOMP) as usize, c"fcomp".as_ptr());
    set_name((OCERZ_OP_FCOMPP) as usize, c"fcompp".as_ptr());
    set_name((OCERZ_OP_FCOMI) as usize, c"fcomi".as_ptr());
    set_name((OCERZ_OP_FCOMIP) as usize, c"fcomip".as_ptr());
    set_name((OCERZ_OP_FUCOM) as usize, c"fucom".as_ptr());
    set_name((OCERZ_OP_FUCOMP) as usize, c"fucomp".as_ptr());
    set_name((OCERZ_OP_FUCOMPP) as usize, c"fucompp".as_ptr());
    set_name((OCERZ_OP_FUCOMI) as usize, c"fucomi".as_ptr());
    set_name((OCERZ_OP_FUCOMIP) as usize, c"fucomip".as_ptr());
    set_name((OCERZ_OP_FCMOVCC) as usize, c"fcmovcc".as_ptr());
    set_name((OCERZ_OP_FTST) as usize, c"ftst".as_ptr());
    set_name((OCERZ_OP_FLDZ) as usize, c"fldz".as_ptr());
    set_name((OCERZ_OP_FLD1) as usize, c"fld1".as_ptr());
    set_name((OCERZ_OP_FLDPI) as usize, c"fldpi".as_ptr());
    set_name((OCERZ_OP_FLDL2E) as usize, c"fldl2e".as_ptr());
    set_name((OCERZ_OP_FLDL2T) as usize, c"fldl2t".as_ptr());
    set_name((OCERZ_OP_FLDLG2) as usize, c"fldlg2".as_ptr());
    set_name((OCERZ_OP_FLDLN2) as usize, c"fldln2".as_ptr());
    set_name((OCERZ_OP_FSQRT) as usize, c"fsqrt".as_ptr());
    set_name((OCERZ_OP_FRNDINT) as usize, c"frndint".as_ptr());
    set_name((OCERZ_OP_F2XM1) as usize, c"f2xm1".as_ptr());
    set_name((OCERZ_OP_FYL2X) as usize, c"fyl2x".as_ptr());
    set_name((OCERZ_OP_FPTAN) as usize, c"fptan".as_ptr());
    set_name((OCERZ_OP_FPATAN) as usize, c"fpatan".as_ptr());
    set_name((OCERZ_OP_FPREM) as usize, c"fprem".as_ptr());
    set_name((OCERZ_OP_FPREM1) as usize, c"fprem1".as_ptr());
    set_name((OCERZ_OP_FSCALE) as usize, c"fscale".as_ptr());
    set_name((OCERZ_OP_FSIN) as usize, c"fsin".as_ptr());
    set_name((OCERZ_OP_FCOS) as usize, c"fcos".as_ptr());
    set_name((OCERZ_OP_FSINCOS) as usize, c"fsincos".as_ptr());
    set_name((OCERZ_OP_FNINIT) as usize, c"fninit".as_ptr());
    set_name((OCERZ_OP_FNCLEX) as usize, c"fnclex".as_ptr());
    set_name((OCERZ_OP_FFREE) as usize, c"ffree".as_ptr());
    set_name((OCERZ_OP_FINCSTP) as usize, c"fincstp".as_ptr());
    set_name((OCERZ_OP_FDECSTP) as usize, c"fdecstp".as_ptr());
    set_name((OCERZ_OP_FWAIT) as usize, c"fwait".as_ptr());
    set_name((OCERZ_OP_FICOM) as usize, c"ficom".as_ptr());
    set_name((OCERZ_OP_FICOMP) as usize, c"ficomp".as_ptr());
    set_name((OCERZ_OP_FISTTP) as usize, c"fisttp".as_ptr());
    set_name((OCERZ_OP_FBLD) as usize, c"fbld".as_ptr());
    set_name((OCERZ_OP_FBSTP) as usize, c"fbstp".as_ptr());
    set_name((OCERZ_OP_FXAM) as usize, c"fxam".as_ptr());
    set_name((OCERZ_OP_FXTRACT) as usize, c"fxtract".as_ptr());
    set_name((OCERZ_OP_FYL2XP1) as usize, c"fyl2xp1".as_ptr());
    set_name((OCERZ_OP_FNSAVE) as usize, c"fnsave".as_ptr());
    set_name((OCERZ_OP_FRSTOR) as usize, c"frstor".as_ptr());
    set_name((OCERZ_OP_FFREEP) as usize, c"ffreep".as_ptr());
    set_name((OCERZ_OP_MOVUPS) as usize, c"movups".as_ptr());
    set_name((OCERZ_OP_MOVAPS) as usize, c"movaps".as_ptr());
    set_name((OCERZ_OP_MOVDQA) as usize, c"movdqa".as_ptr());
    set_name((OCERZ_OP_MOVDQU) as usize, c"movdqu".as_ptr());
    set_name((OCERZ_OP_MOVSS) as usize, c"movss".as_ptr());
    set_name((OCERZ_OP_MOVSDX) as usize, c"movsd".as_ptr());
    set_name((OCERZ_OP_MOVD) as usize, c"movd".as_ptr());
    set_name((OCERZ_OP_MOVQX) as usize, c"movq".as_ptr());
    set_name((OCERZ_OP_MOVLPS) as usize, c"movlps".as_ptr());
    set_name((OCERZ_OP_MOVHPS) as usize, c"movhps".as_ptr());
    set_name((OCERZ_OP_MOVLHPS) as usize, c"movlhps".as_ptr());
    set_name((OCERZ_OP_MOVHLPS) as usize, c"movhlps".as_ptr());
    set_name((OCERZ_OP_MOVMSKPS) as usize, c"movmskps".as_ptr());
    set_name((OCERZ_OP_MOVMSKPD) as usize, c"movmskpd".as_ptr());
    set_name((OCERZ_OP_PMOVMSKB) as usize, c"pmovmskb".as_ptr());
    set_name((OCERZ_OP_MOVSHDUP) as usize, c"movshdup".as_ptr());
    set_name((OCERZ_OP_MOVSLDUP) as usize, c"movsldup".as_ptr());
    set_name((OCERZ_OP_MOVDDUP) as usize, c"movddup".as_ptr());
    set_name((OCERZ_OP_MOVNTI) as usize, c"movnti".as_ptr());
    set_name((OCERZ_OP_ADDPS) as usize, c"addps".as_ptr());
    set_name((OCERZ_OP_ADDPD) as usize, c"addpd".as_ptr());
    set_name((OCERZ_OP_ADDSS) as usize, c"addss".as_ptr());
    set_name((OCERZ_OP_ADDSD) as usize, c"addsd".as_ptr());
    set_name((OCERZ_OP_HADDPS) as usize, c"haddps".as_ptr());
    set_name((OCERZ_OP_HADDPD) as usize, c"haddpd".as_ptr());
    set_name((OCERZ_OP_HSUBPS) as usize, c"hsubps".as_ptr());
    set_name((OCERZ_OP_HSUBPD) as usize, c"hsubpd".as_ptr());
    set_name((OCERZ_OP_SUBPS) as usize, c"subps".as_ptr());
    set_name((OCERZ_OP_SUBPD) as usize, c"subpd".as_ptr());
    set_name((OCERZ_OP_SUBSS) as usize, c"subss".as_ptr());
    set_name((OCERZ_OP_SUBSD) as usize, c"subsd".as_ptr());
    set_name((OCERZ_OP_MULPS) as usize, c"mulps".as_ptr());
    set_name((OCERZ_OP_MULPD) as usize, c"mulpd".as_ptr());
    set_name((OCERZ_OP_MULSS) as usize, c"mulss".as_ptr());
    set_name((OCERZ_OP_MULSD) as usize, c"mulsd".as_ptr());
    set_name((OCERZ_OP_DIVPS) as usize, c"divps".as_ptr());
    set_name((OCERZ_OP_DIVPD) as usize, c"divpd".as_ptr());
    set_name((OCERZ_OP_DIVSS) as usize, c"divss".as_ptr());
    set_name((OCERZ_OP_DIVSD) as usize, c"divsd".as_ptr());
    set_name((OCERZ_OP_MINPS) as usize, c"minps".as_ptr());
    set_name((OCERZ_OP_MINPD) as usize, c"minpd".as_ptr());
    set_name((OCERZ_OP_MINSS) as usize, c"minss".as_ptr());
    set_name((OCERZ_OP_MINSD) as usize, c"minsd".as_ptr());
    set_name((OCERZ_OP_MAXPS) as usize, c"maxps".as_ptr());
    set_name((OCERZ_OP_MAXPD) as usize, c"maxpd".as_ptr());
    set_name((OCERZ_OP_MAXSS) as usize, c"maxss".as_ptr());
    set_name((OCERZ_OP_MAXSD) as usize, c"maxsd".as_ptr());
    set_name((OCERZ_OP_SQRTPS) as usize, c"sqrtps".as_ptr());
    set_name((OCERZ_OP_SQRTPD) as usize, c"sqrtpd".as_ptr());
    set_name((OCERZ_OP_SQRTSS) as usize, c"sqrtss".as_ptr());
    set_name((OCERZ_OP_SQRTSD) as usize, c"sqrtsd".as_ptr());
    set_name((OCERZ_OP_RSQRTPS) as usize, c"rsqrtps".as_ptr());
    set_name((OCERZ_OP_RSQRTSS) as usize, c"rsqrtss".as_ptr());
    set_name((OCERZ_OP_RCPPS) as usize, c"rcpps".as_ptr());
    set_name((OCERZ_OP_RCPSS) as usize, c"rcpss".as_ptr());
    set_name((OCERZ_OP_ANDPS) as usize, c"andps".as_ptr());
    set_name((OCERZ_OP_ANDNPS) as usize, c"andnps".as_ptr());
    set_name((OCERZ_OP_ORPS) as usize, c"orps".as_ptr());
    set_name((OCERZ_OP_XORPS) as usize, c"xorps".as_ptr());
    set_name((OCERZ_OP_PAND) as usize, c"pand".as_ptr());
    set_name((OCERZ_OP_PANDN) as usize, c"pandn".as_ptr());
    set_name((OCERZ_OP_POR) as usize, c"por".as_ptr());
    set_name((OCERZ_OP_PXOR) as usize, c"pxor".as_ptr());
    set_name((OCERZ_OP_CMPPS) as usize, c"cmpps".as_ptr());
    set_name((OCERZ_OP_CMPPD) as usize, c"cmppd".as_ptr());
    set_name((OCERZ_OP_CMPSS) as usize, c"cmpss".as_ptr());
    set_name((OCERZ_OP_CMPSDX) as usize, c"cmpsd".as_ptr());
    set_name((OCERZ_OP_COMISS) as usize, c"comiss".as_ptr());
    set_name((OCERZ_OP_COMISD) as usize, c"comisd".as_ptr());
    set_name((OCERZ_OP_UCOMISS) as usize, c"ucomiss".as_ptr());
    set_name((OCERZ_OP_UCOMISD) as usize, c"ucomisd".as_ptr());
    set_name((OCERZ_OP_PCMPEQB) as usize, c"pcmpeqb".as_ptr());
    set_name((OCERZ_OP_PCMPEQW) as usize, c"pcmpeqw".as_ptr());
    set_name((OCERZ_OP_PCMPEQD) as usize, c"pcmpeqd".as_ptr());
    set_name((OCERZ_OP_PCMPEQQ) as usize, c"pcmpeqq".as_ptr());
    set_name((OCERZ_OP_PCMPGTB) as usize, c"pcmpgtb".as_ptr());
    set_name((OCERZ_OP_PCMPGTW) as usize, c"pcmpgtw".as_ptr());
    set_name((OCERZ_OP_PCMPGTD) as usize, c"pcmpgtd".as_ptr());
    set_name((OCERZ_OP_PCMPGTQ) as usize, c"pcmpgtq".as_ptr());
    set_name((OCERZ_OP_PMULDQ) as usize, c"pmuldq".as_ptr());
    set_name((OCERZ_OP_MPSADBW) as usize, c"mpsadbw".as_ptr());
    set_name((OCERZ_OP_PHMINPOSUW) as usize, c"phminposuw".as_ptr());
    set_name((OCERZ_OP_DPPS) as usize, c"dpps".as_ptr());
    set_name((OCERZ_OP_DPPD) as usize, c"dppd".as_ptr());
    set_name((OCERZ_OP_CRC32) as usize, c"crc32".as_ptr());
    set_name((OCERZ_OP_PCMPESTRM) as usize, c"pcmpestrm".as_ptr());
    set_name((OCERZ_OP_PCMPESTRI) as usize, c"pcmpestri".as_ptr());
    set_name((OCERZ_OP_PCMPISTRM) as usize, c"pcmpistrm".as_ptr());
    set_name((OCERZ_OP_PCMPISTRI) as usize, c"pcmpistri".as_ptr());
    set_name((OCERZ_OP_CVTSI2SS) as usize, c"cvtsi2ss".as_ptr());
    set_name((OCERZ_OP_CVTSI2SD) as usize, c"cvtsi2sd".as_ptr());
    set_name((OCERZ_OP_CVTSS2SI) as usize, c"cvtss2si".as_ptr());
    set_name((OCERZ_OP_CVTSD2SI) as usize, c"cvtsd2si".as_ptr());
    set_name((OCERZ_OP_CVTTSS2SI) as usize, c"cvttss2si".as_ptr());
    set_name((OCERZ_OP_CVTTSD2SI) as usize, c"cvttsd2si".as_ptr());
    set_name((OCERZ_OP_CVTSS2SD) as usize, c"cvtss2sd".as_ptr());
    set_name((OCERZ_OP_CVTSD2SS) as usize, c"cvtsd2ss".as_ptr());
    set_name((OCERZ_OP_CVTPS2PD) as usize, c"cvtps2pd".as_ptr());
    set_name((OCERZ_OP_CVTPD2PS) as usize, c"cvtpd2ps".as_ptr());
    set_name((OCERZ_OP_CVTDQ2PS) as usize, c"cvtdq2ps".as_ptr());
    set_name((OCERZ_OP_CVTPS2DQ) as usize, c"cvtps2dq".as_ptr());
    set_name((OCERZ_OP_CVTTPS2DQ) as usize, c"cvttps2dq".as_ptr());
    set_name((OCERZ_OP_CVTDQ2PD) as usize, c"cvtdq2pd".as_ptr());
    set_name((OCERZ_OP_CVTPD2DQ) as usize, c"cvtpd2dq".as_ptr());
    set_name((OCERZ_OP_CVTTPD2DQ) as usize, c"cvttpd2dq".as_ptr());
    set_name((OCERZ_OP_PADDB) as usize, c"paddb".as_ptr());
    set_name((OCERZ_OP_PADDW) as usize, c"paddw".as_ptr());
    set_name((OCERZ_OP_PADDD) as usize, c"paddd".as_ptr());
    set_name((OCERZ_OP_PADDQ) as usize, c"paddq".as_ptr());
    set_name((OCERZ_OP_PSUBB) as usize, c"psubb".as_ptr());
    set_name((OCERZ_OP_PSUBW) as usize, c"psubw".as_ptr());
    set_name((OCERZ_OP_PSUBD) as usize, c"psubd".as_ptr());
    set_name((OCERZ_OP_PSUBQ) as usize, c"psubq".as_ptr());
    set_name((OCERZ_OP_PADDSB) as usize, c"paddsb".as_ptr());
    set_name((OCERZ_OP_PADDSW) as usize, c"paddsw".as_ptr());
    set_name((OCERZ_OP_PADDUSB) as usize, c"paddusb".as_ptr());
    set_name((OCERZ_OP_PADDUSW) as usize, c"paddusw".as_ptr());
    set_name((OCERZ_OP_PSUBSB) as usize, c"psubsb".as_ptr());
    set_name((OCERZ_OP_PSUBSW) as usize, c"psubsw".as_ptr());
    set_name((OCERZ_OP_PSUBUSB) as usize, c"psubusb".as_ptr());
    set_name((OCERZ_OP_PSUBUSW) as usize, c"psubusw".as_ptr());
    set_name((OCERZ_OP_PMULLW) as usize, c"pmullw".as_ptr());
    set_name((OCERZ_OP_PMULLD) as usize, c"pmulld".as_ptr());
    set_name((OCERZ_OP_PMULHW) as usize, c"pmulhw".as_ptr());
    set_name((OCERZ_OP_PMULHUW) as usize, c"pmulhuw".as_ptr());
    set_name((OCERZ_OP_PMULUDQ) as usize, c"pmuludq".as_ptr());
    set_name((OCERZ_OP_PMADDWD) as usize, c"pmaddwd".as_ptr());
    set_name((OCERZ_OP_PAVGB) as usize, c"pavgb".as_ptr());
    set_name((OCERZ_OP_PAVGW) as usize, c"pavgw".as_ptr());
    set_name((OCERZ_OP_PMAXUB) as usize, c"pmaxub".as_ptr());
    set_name((OCERZ_OP_PMAXSW) as usize, c"pmaxsw".as_ptr());
    set_name((OCERZ_OP_PMINUB) as usize, c"pminub".as_ptr());
    set_name((OCERZ_OP_PMINSW) as usize, c"pminsw".as_ptr());
    set_name((OCERZ_OP_PMAXSB) as usize, c"pmaxsb".as_ptr());
    set_name((OCERZ_OP_PMAXSD) as usize, c"pmaxsd".as_ptr());
    set_name((OCERZ_OP_PMAXUW) as usize, c"pmaxuw".as_ptr());
    set_name((OCERZ_OP_PMAXUD) as usize, c"pmaxud".as_ptr());
    set_name((OCERZ_OP_PMINSB) as usize, c"pminsb".as_ptr());
    set_name((OCERZ_OP_PMINSD) as usize, c"pminsd".as_ptr());
    set_name((OCERZ_OP_PMINUW) as usize, c"pminuw".as_ptr());
    set_name((OCERZ_OP_PMINUD) as usize, c"pminud".as_ptr());
    set_name((OCERZ_OP_VBROADCASTSS) as usize, c"vbroadcastss".as_ptr());
    set_name((OCERZ_OP_VBROADCASTSD) as usize, c"vbroadcastsd".as_ptr());
    set_name(
        (OCERZ_OP_VBROADCASTF128) as usize,
        c"vbroadcastf128".as_ptr(),
    );
    set_name((OCERZ_OP_VPERMILPS) as usize, c"vpermilps".as_ptr());
    set_name((OCERZ_OP_VPERMILPD) as usize, c"vpermilpd".as_ptr());
    set_name((OCERZ_OP_VTESTPS) as usize, c"vtestps".as_ptr());
    set_name((OCERZ_OP_VTESTPD) as usize, c"vtestpd".as_ptr());
    set_name((OCERZ_OP_VCVTPH2PS) as usize, c"vcvtph2ps".as_ptr());
    set_name((OCERZ_OP_VCVTPS2PH) as usize, c"vcvtps2ph".as_ptr());
    set_name((OCERZ_OP_VINSERTF128) as usize, c"vinsertf128".as_ptr());
    set_name((OCERZ_OP_VEXTRACTF128) as usize, c"vextractf128".as_ptr());
    set_name((OCERZ_OP_VPERM2F128) as usize, c"vperm2f128".as_ptr());
    set_name((OCERZ_OP_VZEROUPPER) as usize, c"vzeroupper".as_ptr());
    set_name((OCERZ_OP_VZEROALL) as usize, c"vzeroall".as_ptr());
    set_name((OCERZ_OP_PSADBW) as usize, c"psadbw".as_ptr());
    set_name((OCERZ_OP_PABSB) as usize, c"pabsb".as_ptr());
    set_name((OCERZ_OP_PABSW) as usize, c"pabsw".as_ptr());
    set_name((OCERZ_OP_PABSD) as usize, c"pabsd".as_ptr());
    set_name((OCERZ_OP_PACKSSWB) as usize, c"packsswb".as_ptr());
    set_name((OCERZ_OP_PACKSSDW) as usize, c"packssdw".as_ptr());
    set_name((OCERZ_OP_PACKUSWB) as usize, c"packuswb".as_ptr());
    set_name((OCERZ_OP_PACKUSDW) as usize, c"packusdw".as_ptr());
    set_name((OCERZ_OP_PUNPCKLBW) as usize, c"punpcklbw".as_ptr());
    set_name((OCERZ_OP_PUNPCKLWD) as usize, c"punpcklwd".as_ptr());
    set_name((OCERZ_OP_PUNPCKLDQ) as usize, c"punpckldq".as_ptr());
    set_name((OCERZ_OP_PUNPCKLQDQ) as usize, c"punpcklqdq".as_ptr());
    set_name((OCERZ_OP_PUNPCKHBW) as usize, c"punpckhbw".as_ptr());
    set_name((OCERZ_OP_PUNPCKHWD) as usize, c"punpckhwd".as_ptr());
    set_name((OCERZ_OP_PUNPCKHDQ) as usize, c"punpckhdq".as_ptr());
    set_name((OCERZ_OP_PUNPCKHQDQ) as usize, c"punpckhqdq".as_ptr());
    set_name((OCERZ_OP_PSHUFD) as usize, c"pshufd".as_ptr());
    set_name((OCERZ_OP_PSHUFLW) as usize, c"pshuflw".as_ptr());
    set_name((OCERZ_OP_PSHUFHW) as usize, c"pshufhw".as_ptr());
    set_name((OCERZ_OP_PSHUFB) as usize, c"pshufb".as_ptr());
    set_name((OCERZ_OP_PHADDW) as usize, c"phaddw".as_ptr());
    set_name((OCERZ_OP_PHADDD) as usize, c"phaddd".as_ptr());
    set_name((OCERZ_OP_PHADDSW) as usize, c"phaddsw".as_ptr());
    set_name((OCERZ_OP_PHSUBW) as usize, c"phsubw".as_ptr());
    set_name((OCERZ_OP_PHSUBD) as usize, c"phsubd".as_ptr());
    set_name((OCERZ_OP_PHSUBSW) as usize, c"phsubsw".as_ptr());
    set_name((OCERZ_OP_PSIGNB) as usize, c"psignb".as_ptr());
    set_name((OCERZ_OP_PSIGNW) as usize, c"psignw".as_ptr());
    set_name((OCERZ_OP_PSIGND) as usize, c"psignd".as_ptr());
    set_name((OCERZ_OP_PMADDUBSW) as usize, c"pmaddubsw".as_ptr());
    set_name((OCERZ_OP_PMULHRSW) as usize, c"pmulhrsw".as_ptr());
    set_name((OCERZ_OP_PALIGNR) as usize, c"palignr".as_ptr());
    set_name((OCERZ_OP_SHUFPS) as usize, c"shufps".as_ptr());
    set_name((OCERZ_OP_SHUFPD) as usize, c"shufpd".as_ptr());
    set_name((OCERZ_OP_UNPCKLPS) as usize, c"unpcklps".as_ptr());
    set_name((OCERZ_OP_UNPCKHPS) as usize, c"unpckhps".as_ptr());
    set_name((OCERZ_OP_UNPCKLPD) as usize, c"unpcklpd".as_ptr());
    set_name((OCERZ_OP_UNPCKHPD) as usize, c"unpckhpd".as_ptr());
    set_name((OCERZ_OP_PSLLW) as usize, c"psllw".as_ptr());
    set_name((OCERZ_OP_PSLLD) as usize, c"pslld".as_ptr());
    set_name((OCERZ_OP_PSLLQ) as usize, c"psllq".as_ptr());
    set_name((OCERZ_OP_PSRLW) as usize, c"psrlw".as_ptr());
    set_name((OCERZ_OP_PSRLD) as usize, c"psrld".as_ptr());
    set_name((OCERZ_OP_PSRLQ) as usize, c"psrlq".as_ptr());
    set_name((OCERZ_OP_PSRAW) as usize, c"psraw".as_ptr());
    set_name((OCERZ_OP_PSRAD) as usize, c"psrad".as_ptr());
    set_name((OCERZ_OP_PSLLDQ) as usize, c"pslldq".as_ptr());
    set_name((OCERZ_OP_PSRLDQ) as usize, c"psrldq".as_ptr());
    set_name((OCERZ_OP_PEXTRB) as usize, c"pextrb".as_ptr());
    set_name((OCERZ_OP_PEXTRW) as usize, c"pextrw".as_ptr());
    set_name((OCERZ_OP_PEXTRD) as usize, c"pextrd".as_ptr());
    set_name((OCERZ_OP_PEXTRQ) as usize, c"pextrq".as_ptr());
    set_name((OCERZ_OP_PINSRB) as usize, c"pinsrb".as_ptr());
    set_name((OCERZ_OP_PINSRW) as usize, c"pinsrw".as_ptr());
    set_name((OCERZ_OP_PINSRD) as usize, c"pinsrd".as_ptr());
    set_name((OCERZ_OP_PINSRQ) as usize, c"pinsrq".as_ptr());
    set_name((OCERZ_OP_EXTRACTPS) as usize, c"extractps".as_ptr());
    set_name((OCERZ_OP_INSERTPS) as usize, c"insertps".as_ptr());
    set_name((OCERZ_OP_PTEST) as usize, c"ptest".as_ptr());
    set_name((OCERZ_OP_PMOVZXBW) as usize, c"pmovzxbw".as_ptr());
    set_name((OCERZ_OP_PMOVZXBD) as usize, c"pmovzxbd".as_ptr());
    set_name((OCERZ_OP_PMOVZXBQ) as usize, c"pmovzxbq".as_ptr());
    set_name((OCERZ_OP_PMOVZXWD) as usize, c"pmovzxwd".as_ptr());
    set_name((OCERZ_OP_PMOVZXWQ) as usize, c"pmovzxwq".as_ptr());
    set_name((OCERZ_OP_PMOVZXDQ) as usize, c"pmovzxdq".as_ptr());
    set_name((OCERZ_OP_PMOVSXBW) as usize, c"pmovsxbw".as_ptr());
    set_name((OCERZ_OP_PMOVSXBD) as usize, c"pmovsxbd".as_ptr());
    set_name((OCERZ_OP_PMOVSXBQ) as usize, c"pmovsxbq".as_ptr());
    set_name((OCERZ_OP_PMOVSXWD) as usize, c"pmovsxwd".as_ptr());
    set_name((OCERZ_OP_PMOVSXWQ) as usize, c"pmovsxwq".as_ptr());
    set_name((OCERZ_OP_PMOVSXDQ) as usize, c"pmovsxdq".as_ptr());
    set_name((OCERZ_OP_ROUNDPS) as usize, c"roundps".as_ptr());
    set_name((OCERZ_OP_ROUNDPD) as usize, c"roundpd".as_ptr());
    set_name((OCERZ_OP_ROUNDSS) as usize, c"roundss".as_ptr());
    set_name((OCERZ_OP_ROUNDSD) as usize, c"roundsd".as_ptr());
    set_name((OCERZ_OP_PBLENDW) as usize, c"pblendw".as_ptr());
    set_name((OCERZ_OP_BLENDPS) as usize, c"blendps".as_ptr());
    set_name((OCERZ_OP_BLENDPD) as usize, c"blendpd".as_ptr());
    set_name((OCERZ_OP_BLENDVPS) as usize, c"blendvps".as_ptr());
    set_name((OCERZ_OP_BLENDVPD) as usize, c"blendvpd".as_ptr());
    set_name((OCERZ_OP_PBLENDVB) as usize, c"pblendvb".as_ptr());
    set_name((OCERZ_OP_AESENC) as usize, c"aesenc".as_ptr());
    set_name((OCERZ_OP_AESENCLAST) as usize, c"aesenclast".as_ptr());
    set_name((OCERZ_OP_AESDEC) as usize, c"aesdec".as_ptr());
    set_name((OCERZ_OP_AESDECLAST) as usize, c"aesdeclast".as_ptr());
    set_name((OCERZ_OP_AESIMC) as usize, c"aesimc".as_ptr());
    set_name(
        (OCERZ_OP_AESKEYGENASSIST) as usize,
        c"aeskeygenassist".as_ptr(),
    );
    set_name((OCERZ_OP_PCLMULQDQ) as usize, c"pclmulqdq".as_ptr());
    set_name((OCERZ_OP_PUSHA) as usize, c"pusha".as_ptr());
    set_name((OCERZ_OP_POPA) as usize, c"popa".as_ptr());
    set_name((OCERZ_OP_PUSHSEG) as usize, c"push_sreg".as_ptr());
    set_name((OCERZ_OP_POPSEG) as usize, c"pop_sreg".as_ptr());
    set_name((OCERZ_OP_DAA) as usize, c"daa".as_ptr());
    set_name((OCERZ_OP_DAS) as usize, c"das".as_ptr());
    set_name((OCERZ_OP_AAA) as usize, c"aaa".as_ptr());
    set_name((OCERZ_OP_AAS) as usize, c"aas".as_ptr());
    set_name((OCERZ_OP_AAM) as usize, c"aam".as_ptr());
    set_name((OCERZ_OP_AAD) as usize, c"aad".as_ptr());
    set_name((OCERZ_OP_BOUND) as usize, c"bound".as_ptr());
    set_name((OCERZ_OP_LES) as usize, c"les".as_ptr());
    set_name((OCERZ_OP_LDS) as usize, c"lds".as_ptr());
    set_name((OCERZ_OP_INTO) as usize, c"into".as_ptr());
    set_name((OCERZ_OP_SALC) as usize, c"salc".as_ptr());
    set_name((OCERZ_OP_CVTPI2PS) as usize, c"cvtpi2ps".as_ptr());
    set_name((OCERZ_OP_CVTPI2PD) as usize, c"cvtpi2pd".as_ptr());
    set_name((OCERZ_OP_CVTPS2PI) as usize, c"cvtps2pi".as_ptr());
    set_name((OCERZ_OP_CVTTPS2PI) as usize, c"cvttps2pi".as_ptr());
    set_name((OCERZ_OP_CVTPD2PI) as usize, c"cvtpd2pi".as_ptr());
    set_name((OCERZ_OP_CVTTPD2PI) as usize, c"cvttpd2pi".as_ptr());
    set_name((OCERZ_OP_VPBROADCASTB) as usize, c"vpbroadcastb".as_ptr());
    set_name((OCERZ_OP_VPBROADCASTW) as usize, c"vpbroadcastw".as_ptr());
    set_name((OCERZ_OP_VPBROADCASTD) as usize, c"vpbroadcastd".as_ptr());
    set_name((OCERZ_OP_VPBROADCASTQ) as usize, c"vpbroadcastq".as_ptr());
    set_name(
        (OCERZ_OP_VBROADCASTI128) as usize,
        c"vbroadcasti128".as_ptr(),
    );
    set_name((OCERZ_OP_VINSERTI128) as usize, c"vinserti128".as_ptr());
    set_name((OCERZ_OP_VEXTRACTI128) as usize, c"vextracti128".as_ptr());
    set_name((OCERZ_OP_VPERM2I128) as usize, c"vperm2i128".as_ptr());
    set_name((OCERZ_OP_VPBLENDD) as usize, c"vpblendd".as_ptr());
    set_name((OCERZ_OP_VPERMD) as usize, c"vpermd".as_ptr());
    set_name((OCERZ_OP_VPERMPS) as usize, c"vpermps".as_ptr());
    set_name((OCERZ_OP_VPERMQ) as usize, c"vpermq".as_ptr());
    set_name((OCERZ_OP_VPERMPD) as usize, c"vpermpd".as_ptr());
    set_name((OCERZ_OP_VPSLLVD) as usize, c"vpsllvd".as_ptr());
    set_name((OCERZ_OP_VPSLLVQ) as usize, c"vpsllvq".as_ptr());
    set_name((OCERZ_OP_VPSRLVD) as usize, c"vpsrlvd".as_ptr());
    set_name((OCERZ_OP_VPSRLVQ) as usize, c"vpsrlvq".as_ptr());
    set_name((OCERZ_OP_VPSRAVD) as usize, c"vpsravd".as_ptr());
    set_name((OCERZ_OP_VPMASKMOVD) as usize, c"vpmaskmovd".as_ptr());
    set_name((OCERZ_OP_VPMASKMOVQ) as usize, c"vpmaskmovq".as_ptr());
    set_name((OCERZ_OP_VMASKMOVPS) as usize, c"vmaskmovps".as_ptr());
    set_name((OCERZ_OP_VMASKMOVPD) as usize, c"vmaskmovpd".as_ptr());
    set_name((OCERZ_OP_VPGATHERDD) as usize, c"vpgatherdd".as_ptr());
    set_name((OCERZ_OP_VPGATHERDQ) as usize, c"vpgatherdq".as_ptr());
    set_name((OCERZ_OP_VPGATHERQD) as usize, c"vpgatherqd".as_ptr());
    set_name((OCERZ_OP_VPGATHERQQ) as usize, c"vpgatherqq".as_ptr());
    set_name((OCERZ_OP_VGATHERDPS) as usize, c"vgatherdps".as_ptr());
    set_name((OCERZ_OP_VGATHERDPD) as usize, c"vgatherdpd".as_ptr());
    set_name((OCERZ_OP_VGATHERQPS) as usize, c"vgatherqps".as_ptr());
    set_name((OCERZ_OP_VGATHERQPD) as usize, c"vgatherqpd".as_ptr());
    set_name((OCERZ_OP_ANDN) as usize, c"andn".as_ptr());
    set_name((OCERZ_OP_BLSR) as usize, c"blsr".as_ptr());
    set_name((OCERZ_OP_BLSMSK) as usize, c"blsmsk".as_ptr());
    set_name((OCERZ_OP_BLSI) as usize, c"blsi".as_ptr());
    set_name((OCERZ_OP_BZHI) as usize, c"bzhi".as_ptr());
    set_name((OCERZ_OP_BEXTR) as usize, c"bextr".as_ptr());
    set_name((OCERZ_OP_PDEP) as usize, c"pdep".as_ptr());
    set_name((OCERZ_OP_PEXT) as usize, c"pext".as_ptr());
    set_name((OCERZ_OP_MULX) as usize, c"mulx".as_ptr());
    set_name((OCERZ_OP_RORX) as usize, c"rorx".as_ptr());
    set_name((OCERZ_OP_SARX) as usize, c"sarx".as_ptr());
    set_name((OCERZ_OP_SHLX) as usize, c"shlx".as_ptr());
    set_name((OCERZ_OP_SHRX) as usize, c"shrx".as_ptr());
    set_name((OCERZ_OP_MOVBE) as usize, c"movbe".as_ptr());
    set_name((OCERZ_OP_XSAVE) as usize, c"xsave".as_ptr());
    set_name((OCERZ_OP_XRSTOR) as usize, c"xrstor".as_ptr());
    set_name((OCERZ_OP_RDRAND) as usize, c"rdrand".as_ptr());
}

unsafe fn init_op_names() {
    OP_NAMES_ONCE.call_once(|| unsafe {
        for i in 0..OP_COUNT {
            set_name(i, c"?".as_ptr());
        }
        set_all_op_names();
        let fma_kind: [*const c_char; 10] = [
            c"fmaddsub".as_ptr(),
            c"fmsubadd".as_ptr(),
            c"fmadd".as_ptr(),
            c"fmadd".as_ptr(),
            c"fmsub".as_ptr(),
            c"fmsub".as_ptr(),
            c"fnmadd".as_ptr(),
            c"fnmadd".as_ptr(),
            c"fnmsub".as_ptr(),
            c"fnmsub".as_ptr(),
        ];
        let fma_order: [c_int; 3] = [132, 213, 231];
        let fma_names = ptr::addr_of_mut!(FMA_NAMES).cast::<c_char>();
        for i in 0..60 {
            let k = (i >> 1) % 10;
            let suffix = if k >= 3 && (k & 1) != 0 {
                b's' as c_int
            } else {
                b'p' as c_int
            };
            let precision = if (i & 1) != 0 {
                b'd' as c_int
            } else {
                b's' as c_int
            };
            let name = fma_names.add(i * 16);
            libc::snprintf(
                name,
                16,
                c"v%s%d%c%c".as_ptr(),
                *fma_kind.get_unchecked(k),
                *fma_order.get_unchecked((i >> 1) / 10),
                suffix,
                precision,
            );
            set_name(OCERZ_OP_VFMA_FIRST as usize + i, name);
        }
    });
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_op_name(op: c_uint) -> *const c_char {
    init_op_names();
    if op >= OP_COUNT as c_uint {
        return c"?".as_ptr();
    }
    let name = *ptr::addr_of!(OP_NAMES)
        .cast::<*const c_char>()
        .add(op as usize);
    if name.is_null() { c"?".as_ptr() } else { name }
}

unsafe fn gpr_name(reg: c_int, size: c_int, high8: c_int) -> *const c_char {
    if reg < 0 || reg > 15 {
        return c"?".as_ptr();
    }
    if high8 != 0 && reg < 4 {
        return *GPR_HIGH8.get_unchecked(reg as usize);
    }
    match size {
        1 => *GPR8.get_unchecked(reg as usize),
        2 => *GPR16.get_unchecked(reg as usize),
        4 => *GPR32.get_unchecked(reg as usize),
        _ => *GPR64.get_unchecked(reg as usize),
    }
}

unsafe fn fmt_reg(b: *mut c_char, cap: usize, n: *mut usize, op: *const X86Operand) {
    let written = if (*op).kind == OCERZ_OPK_XMM as u8 {
        libc::snprintf(
            b.add(*n),
            if cap > *n { cap - *n } else { 0 },
            c"xmm%u".as_ptr(),
            (*op).reg as c_uint,
        )
    } else if (*op).kind == OCERZ_OPK_MMX as u8 {
        libc::snprintf(
            b.add(*n),
            if cap > *n { cap - *n } else { 0 },
            c"mm%u".as_ptr(),
            (*op).reg as c_uint,
        )
    } else if (*op).kind == OCERZ_OPK_ST as u8 {
        libc::snprintf(
            b.add(*n),
            if cap > *n { cap - *n } else { 0 },
            c"st%u".as_ptr(),
            (*op).reg as c_uint,
        )
    } else {
        libc::snprintf(
            b.add(*n),
            if cap > *n { cap - *n } else { 0 },
            c"%s".as_ptr(),
            gpr_name(
                (*op).reg as c_int,
                (*op).size as c_int,
                (*op).high8 as c_int,
            ),
        )
    };
    if written > 0 {
        *n += written as usize;
    }
}

unsafe fn size_kw(size: c_int) -> *const c_char {
    match size {
        1 => c"byte".as_ptr(),
        2 => c"word".as_ptr(),
        4 => c"dword".as_ptr(),
        8 => c"qword".as_ptr(),
        10 => c"tword".as_ptr(),
        16 => c"xmmword".as_ptr(),
        _ => c"".as_ptr(),
    }
}

unsafe fn fmt_mem(
    b: *mut c_char,
    cap: usize,
    n: *mut usize,
    insn: *const X86Insn,
    op: *const X86Operand,
) {
    let seg = if (*insn).seg == OCERZ_SEG_FS as u8 {
        c"fs:".as_ptr()
    } else if (*insn).seg == OCERZ_SEG_GS as u8 {
        c"gs:".as_ptr()
    } else {
        c"".as_ptr()
    };
    if (*op).riprel != 0 {
        let written = libc::snprintf(
            b.add(*n),
            if cap > *n { cap - *n } else { 0 },
            c"%s [0x%llx]".as_ptr(),
            size_kw((*op).size as c_int),
            (*op).disp as u64 as libc::c_ulonglong,
        );
        if written > 0 {
            *n += written as usize;
        }
        return;
    }
    let mut written = libc::snprintf(
        b.add(*n),
        if cap > *n { cap - *n } else { 0 },
        c"%s %s[".as_ptr(),
        size_kw((*op).size as c_int),
        seg,
    );
    if written > 0 {
        *n += written as usize;
    }
    let aw = if (*insn).mode32 != 0 {
        (*insn).addrsize as c_int
    } else {
        8
    };
    let mut any = 0;
    if (*op).base != OCERZ_REG_NONE as u8 {
        written = libc::snprintf(
            b.add(*n),
            if cap > *n { cap - *n } else { 0 },
            c"%s".as_ptr(),
            gpr_name((*op).base as c_int, aw, 0),
        );
        if written > 0 {
            *n += written as usize;
        }
        any = 1;
    }
    if (*op).index != OCERZ_REG_NONE as u8 {
        written = libc::snprintf(
            b.add(*n),
            if cap > *n { cap - *n } else { 0 },
            c"%s%s*%d".as_ptr(),
            if any != 0 {
                c"+".as_ptr()
            } else {
                c"".as_ptr()
            },
            gpr_name((*op).index as c_int, aw, 0),
            (1 << (*op).scale) as c_int,
        );
        if written > 0 {
            *n += written as usize;
        }
        any = 1;
    }
    if (*op).disp != 0 || any == 0 {
        let d = (*op).disp as i64;
        if d < 0 {
            written = libc::snprintf(
                b.add(*n),
                if cap > *n { cap - *n } else { 0 },
                c"-0x%llx".as_ptr(),
                d.wrapping_neg() as u64 as libc::c_ulonglong,
            );
        } else {
            written = libc::snprintf(
                b.add(*n),
                if cap > *n { cap - *n } else { 0 },
                c"%s0x%llx".as_ptr(),
                if any != 0 {
                    c"+".as_ptr()
                } else {
                    c"".as_ptr()
                },
                d as u64 as libc::c_ulonglong,
            );
        }
        if written > 0 {
            *n += written as usize;
        }
    }
    written = libc::snprintf(
        b.add(*n),
        if cap > *n { cap - *n } else { 0 },
        c"]".as_ptr(),
    );
    if written > 0 {
        *n += written as usize;
    }
}

unsafe fn fmt_operand(
    b: *mut c_char,
    cap: usize,
    n: *mut usize,
    insn: *const X86Insn,
    op: *const X86Operand,
) {
    match (*op).kind as c_int {
        x if x == OCERZ_OPK_REG as c_int
            || x == OCERZ_OPK_XMM as c_int
            || x == OCERZ_OPK_ST as c_int
            || x == OCERZ_OPK_MMX as c_int =>
        {
            fmt_reg(b, cap, n, op)
        }
        x if x == OCERZ_OPK_MEM as c_int => fmt_mem(b, cap, n, insn, op),
        x if x == OCERZ_OPK_IMM as c_int => {
            let written = libc::snprintf(
                b.add(*n),
                if cap > *n { cap - *n } else { 0 },
                c"0x%llx".as_ptr(),
                (*op).imm as libc::c_ulonglong,
            );
            if written > 0 {
                *n += written as usize;
            }
        }
        _ => {}
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_format_insn(insn: *const X86Insn, buf: *mut c_char, cap: usize) {
    if cap == 0 {
        return;
    }
    let mut n = 0usize;
    let mut written = libc::snprintf(
        buf,
        cap,
        c"%s".as_ptr(),
        ocerz_op_name((*insn).op as c_uint),
    );
    if written > 0 {
        n += written as usize;
    }
    if (*insn).rep == OCERZ_REP_REP as u8 {
        let mut tmp = [0 as c_char; 64];
        libc::snprintf(
            tmp.as_mut_ptr(),
            tmp.len(),
            c"rep %s".as_ptr(),
            ocerz_op_name((*insn).op as c_uint),
        );
        n = libc::snprintf(buf, cap, c"%s".as_ptr(), tmp.as_ptr()) as usize;
    } else if (*insn).rep == OCERZ_REP_REPNE as u8 {
        let mut tmp = [0 as c_char; 64];
        libc::snprintf(
            tmp.as_mut_ptr(),
            tmp.len(),
            c"repne %s".as_ptr(),
            ocerz_op_name((*insn).op as c_uint),
        );
        n = libc::snprintf(buf, cap, c"%s".as_ptr(), tmp.as_ptr()) as usize;
    }
    for i in 0..(*insn).nops as usize {
        written = libc::snprintf(
            buf.add(n),
            if cap > n { cap - n } else { 0 },
            c"%s".as_ptr(),
            if i == 0 {
                c" ".as_ptr()
            } else {
                c", ".as_ptr()
            },
        );
        if written > 0 {
            n += written as usize;
        }
        let op = ptr::addr_of!((*insn).ops).cast::<X86Operand>().add(i);
        fmt_operand(buf, cap, &mut n, insn, op);
    }
    if n >= cap {
        *buf.add(cap - 1) = 0;
    }
}

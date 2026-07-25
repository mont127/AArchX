/*
 * include/ocerz/decode.h
 *
 * The x86_64 instruction decoder contract: one decoded instruction (X86Insn)
 * is the single exchange format between the decoder, the interpreter, and
 * the JIT translator. The decoder is PURE: it reads only the byte buffer it
 * is given, never CPU or guest-memory state.
 *
 * ocerz_decode(code, avail, rip, out) decodes exactly one instruction whose
 * first byte is code[0] and which is architecturally located at guest
 * address `rip`. It returns OCERZ_OK and fills *out, or OCERZ_EUNDEF for an
 * opcode that does not exist / is rejected, OCERZ_ETRUNC if `avail` bytes
 * are not enough, OCERZ_ETOOLONG past 15 bytes. On success out->len is the
 * full encoded length including prefixes.
 *
 * Operand conventions (binding for every consumer):
 *  - ops[0] is the destination and usually also the first source. For pure
 *    comparisons (CMP, TEST, BT, COMISS/COMISD, UCOMISS/UCOMISD, PTEST) all
 *    operands are read-only. XCHG and XADD read and write both.
 *  - Every operand carries its own access size in bytes in .size (1,2,4,8,
 *    10 for x87 extended, 16 for full XMM). insn.opsize mirrors the primary
 *    operation width. Consumers must trust per-operand sizes, e.g. MOVZX has
 *    ops[0].size 2/4/8 and ops[1].size 1/2.
 *  - Memory operands: effective address = gpr[base] + (gpr[index] << scale)
 *    + disp, where .scale is the LOG2 of the x86 scale (0..3) and absent
 *    base/index are OCERZ_REG_NONE. RIP-relative operands are pre-resolved:
 *    .riprel=1, base=index=NONE, and .disp already holds the final absolute
 *    guest address (instruction-end rip + disp32). moffs forms (A0..A3)
 *    become plain MEM operands with base=index=NONE and disp=the 64-bit
 *    offset. Segment overrides live on insn.seg (only FS/GS have effect;
 *    CS/DS/ES/SS prefixes are consumed and ignored); the effective-address
 *    helper adds fs_base/gs_base AFTER the 32-bit address-size truncation
 *    that applies when insn.addrsize is 4.
 *  - 8-bit register operands encoded as 4..7 WITHOUT any REX prefix are the
 *    legacy high-byte registers AH/CH/DH/BH: the decoder stores .reg =
 *    encoded-4 (the underlying RAX/RCX/RDX/RBX index) and sets .high8=1,
 *    meaning bits 8..15 of that GPR. With any REX present they decode
 *    normally to SPL/BPL/SIL/DIL (.high8=0, reg 4..7).
 *  - Immediates are stored in .imm with the architectural sign-extension
 *    already applied as a 64-bit bit pattern (imm8/imm32 sign-extended in
 *    64-bit-operand contexts; only MOV reg64, imm64 carries 8 raw bytes).
 *  - Direct branches (JMP rel, Jcc, CALL rel, JRCXZ, LOOP*) have one IMM
 *    operand whose .imm is the ABSOLUTE target address. Indirect JMP/CALL
 *    have a REG or MEM operand. RET with a 0xC2 stack adjustment carries one
 *    IMM operand (the byte count); plain RET has nops=0.
 *  - Jcc/SETcc/CMOVcc/FCMOVcc store the 4-bit condition in insn.cc using the
 *    standard encoding listed in OcerzCC below.
 *  - Shift/rotate by CL has ops[1] = REG rcx size 1; by immediate has
 *    ops[1] = IMM. SHLD/SHRD have ops[2] as the count (IMM or REG cl).
 *  - String instructions (MOVS/STOS/LODS/SCAS/CMPS) have nops=0; the element
 *    width is insn.opsize and insn.rep distinguishes none/REP/REPNE.
 *  - PUSH/POP default to 8 bytes in long mode (2 with a 66 prefix). PUSHF/
 *    POPF are always 8.
 *  - IMUL appears with nops=1 (one r/m source, implicit RDX:RAX), nops=2
 *    (dst, src) and nops=3 (dst, src, imm). MUL/DIV/IDIV/NOT/NEG have
 *    nops=1. CBW/CWDE/CDQE share OCERZ_OP_CBW distinguished by opsize
 *    (2/4/8); CWD/CDQ/CQO likewise share OCERZ_OP_CWD.
 *  - CMPXCHG: ops[0]=r/m, ops[1]=reg, accumulator implicit. CMPXCHG8B/16B is
 *    OCERZ_OP_CMPXCHGXB with one MEM operand and opsize 8 or 16.
 *  - SSE register operands use kind OCERZ_OPK_XMM with .reg 0..15. Scalar
 *    SSE forms carry size 4 (ss) or 8 (sd); full-width forms carry 16.
 *    Variants that differ only in non-architectural ways are normalized:
 *    MOVAPD->MOVAPS, MOVUPD->MOVUPS, ANDPD->ANDPS (etc. for OR/XOR/ANDN),
 *    MOVNTDQ->MOVDQA, MOVNTPS->MOVAPS, LDDQU->MOVDQU, all PREFETCH* hints
 *    decode to OCERZ_OP_PREFETCH, multi-byte NOP 0F 1F and ENDBR64
 *    F3 0F 1E FA decode to OCERZ_OP_NOP. MOVD is the 32-bit GPR<->XMM move;
 *    MOVQX covers the 64-bit GPR<->XMM and XMM<->XMM/m64 low-quadword moves
 *    (0F 6E/7E with REX.W, F3 0F 7E, 66 0F D6).
 *  - x87 stack registers use kind OCERZ_OPK_ST with .reg = i for ST(i);
 *    memory forms are MEM with size 4/8/10 (real) or 2/4/8 (integer).
 *    FNSTSW AX has ops[0] = REG rax size 2.
 *  - insn.lock is set when a F0 prefix was present (only meaningful with a
 *    memory destination); insn.rep holds OCERZ_REP_NONE/REP/REPNE for string
 *    ops only, after mandatory-prefix resolution for the 0F maps.
 *
 * RIP protocol: the interpreter advances cpu->rip by insn.len BEFORE
 * executing the instruction, so handlers see the architectural next-rip and
 * branch handlers simply overwrite cpu->rip.
 */
#ifndef OCERZ_DECODE_H
#define OCERZ_DECODE_H

#include "ocerz/types.h"

enum OcerzOpKind {
    OCERZ_OPK_NONE = 0,
    OCERZ_OPK_REG = 1,
    OCERZ_OPK_XMM = 2,
    OCERZ_OPK_ST = 3,
    OCERZ_OPK_MEM = 4,
    OCERZ_OPK_IMM = 5,
};

enum OcerzRep {
    OCERZ_REP_NONE = 0,
    OCERZ_REP_REP = 1,
    OCERZ_REP_REPNE = 2,
};

enum OcerzCC {
    OCERZ_CC_O = 0,
    OCERZ_CC_NO = 1,
    OCERZ_CC_B = 2,
    OCERZ_CC_AE = 3,
    OCERZ_CC_E = 4,
    OCERZ_CC_NE = 5,
    OCERZ_CC_BE = 6,
    OCERZ_CC_A = 7,
    OCERZ_CC_S = 8,
    OCERZ_CC_NS = 9,
    OCERZ_CC_P = 10,
    OCERZ_CC_NP = 11,
    OCERZ_CC_L = 12,
    OCERZ_CC_GE = 13,
    OCERZ_CC_LE = 14,
    OCERZ_CC_G = 15,
};

enum OcerzOp {
    OCERZ_OP_INVALID = 0,

    OCERZ_OP_MOV,
    OCERZ_OP_MOVZX,
    OCERZ_OP_MOVSX,
    OCERZ_OP_MOVSXD,
    OCERZ_OP_LEA,
    OCERZ_OP_XCHG,
    OCERZ_OP_BSWAP,
    OCERZ_OP_PUSH,
    OCERZ_OP_POP,
    OCERZ_OP_PUSHF,
    OCERZ_OP_POPF,
    OCERZ_OP_LAHF,
    OCERZ_OP_SAHF,
    OCERZ_OP_CBW,
    OCERZ_OP_CWD,
    OCERZ_OP_CMOVCC,
    OCERZ_OP_SETCC,

    OCERZ_OP_ADD,
    OCERZ_OP_ADC,
    OCERZ_OP_SUB,
    OCERZ_OP_SBB,
    OCERZ_OP_AND,
    OCERZ_OP_OR,
    OCERZ_OP_XOR,
    OCERZ_OP_CMP,
    OCERZ_OP_TEST,
    OCERZ_OP_INC,
    OCERZ_OP_DEC,
    OCERZ_OP_NEG,
    OCERZ_OP_NOT,
    OCERZ_OP_MUL,
    OCERZ_OP_IMUL,
    OCERZ_OP_DIV,
    OCERZ_OP_IDIV,

    OCERZ_OP_SHL,
    OCERZ_OP_SHR,
    OCERZ_OP_SAR,
    OCERZ_OP_ROL,
    OCERZ_OP_ROR,
    OCERZ_OP_RCL,
    OCERZ_OP_RCR,
    OCERZ_OP_SHLD,
    OCERZ_OP_SHRD,

    OCERZ_OP_BT,
    OCERZ_OP_BTS,
    OCERZ_OP_BTR,
    OCERZ_OP_BTC,
    OCERZ_OP_BSF,
    OCERZ_OP_BSR,
    OCERZ_OP_POPCNT,
    OCERZ_OP_TZCNT,
    OCERZ_OP_LZCNT,

    OCERZ_OP_JMP,
    OCERZ_OP_JCC,
    OCERZ_OP_JRCXZ,
    OCERZ_OP_LOOP,
    OCERZ_OP_LOOPE,
    OCERZ_OP_LOOPNE,
    OCERZ_OP_CALL,
    OCERZ_OP_RET,
    OCERZ_OP_IRET,
    OCERZ_OP_LEAVE,
    OCERZ_OP_INT3,
    OCERZ_OP_INT,
    OCERZ_OP_UD2,
    OCERZ_OP_HLT,

    OCERZ_OP_MOVS,
    OCERZ_OP_STOS,
    OCERZ_OP_LODS,
    OCERZ_OP_SCAS,
    OCERZ_OP_CMPS,

    OCERZ_OP_XADD,
    OCERZ_OP_CMPXCHG,
    OCERZ_OP_CMPXCHGXB,

    OCERZ_OP_CLC,
    OCERZ_OP_STC,
    OCERZ_OP_CMC,
    OCERZ_OP_CLD,
    OCERZ_OP_STD,

    OCERZ_OP_SYSCALL,
    OCERZ_OP_CPUID,
    OCERZ_OP_RDTSC,
    OCERZ_OP_RDTSCP,
    OCERZ_OP_PAUSE,
    OCERZ_OP_NOP,
    OCERZ_OP_MFENCE,
    OCERZ_OP_LFENCE,
    OCERZ_OP_SFENCE,
    OCERZ_OP_XGETBV,
    OCERZ_OP_SGDT,
    OCERZ_OP_SIDT,
    OCERZ_OP_FXSAVE,
    OCERZ_OP_FXRSTOR,
    OCERZ_OP_LDMXCSR,
    OCERZ_OP_STMXCSR,
    OCERZ_OP_PREFETCH,
    OCERZ_OP_CLFLUSH,
    OCERZ_OP_EMMS,

    OCERZ_OP_X87_FIRST,
    OCERZ_OP_FLD,
    OCERZ_OP_FST,
    OCERZ_OP_FSTP,
    OCERZ_OP_FILD,
    OCERZ_OP_FIST,
    OCERZ_OP_FISTP,
    OCERZ_OP_FLDCW,
    OCERZ_OP_FNSTCW,
    OCERZ_OP_FNSTSW,
    OCERZ_OP_FNSTENV,
    OCERZ_OP_FLDENV,
    OCERZ_OP_FXCH,
    OCERZ_OP_FCHS,
    OCERZ_OP_FABS,
    OCERZ_OP_FADD,
    OCERZ_OP_FADDP,
    OCERZ_OP_FIADD,
    OCERZ_OP_FSUB,
    OCERZ_OP_FSUBR,
    OCERZ_OP_FSUBP,
    OCERZ_OP_FSUBRP,
    OCERZ_OP_FISUB,
    OCERZ_OP_FISUBR,
    OCERZ_OP_FMUL,
    OCERZ_OP_FMULP,
    OCERZ_OP_FIMUL,
    OCERZ_OP_FDIV,
    OCERZ_OP_FDIVR,
    OCERZ_OP_FDIVP,
    OCERZ_OP_FDIVRP,
    OCERZ_OP_FIDIV,
    OCERZ_OP_FIDIVR,
    OCERZ_OP_FCOM,
    OCERZ_OP_FCOMP,
    OCERZ_OP_FCOMPP,
    OCERZ_OP_FCOMI,
    OCERZ_OP_FCOMIP,
    OCERZ_OP_FUCOM,
    OCERZ_OP_FUCOMP,
    OCERZ_OP_FUCOMPP,
    OCERZ_OP_FUCOMI,
    OCERZ_OP_FUCOMIP,
    OCERZ_OP_FCMOVCC,
    OCERZ_OP_FTST,
    OCERZ_OP_FLDZ,
    OCERZ_OP_FLD1,
    OCERZ_OP_FLDPI,
    OCERZ_OP_FLDL2E,
    OCERZ_OP_FLDL2T,
    OCERZ_OP_FLDLG2,
    OCERZ_OP_FLDLN2,
    OCERZ_OP_FSQRT,
    OCERZ_OP_FRNDINT,
    OCERZ_OP_F2XM1,
    OCERZ_OP_FYL2X,
    OCERZ_OP_FPTAN,
    OCERZ_OP_FPATAN,
    OCERZ_OP_FPREM,
    OCERZ_OP_FPREM1,
    OCERZ_OP_FSCALE,
    OCERZ_OP_FSIN,
    OCERZ_OP_FCOS,
    OCERZ_OP_FSINCOS,
    OCERZ_OP_FNINIT,
    OCERZ_OP_FNCLEX,
    OCERZ_OP_FFREE,
    OCERZ_OP_FINCSTP,
    OCERZ_OP_FDECSTP,
    OCERZ_OP_FWAIT,

    OCERZ_OP_SSE_FIRST,
    OCERZ_OP_MOVUPS,
    OCERZ_OP_MOVAPS,
    OCERZ_OP_MOVDQA,
    OCERZ_OP_MOVDQU,
    OCERZ_OP_MOVSS,
    OCERZ_OP_MOVSDX,
    OCERZ_OP_MOVD,
    OCERZ_OP_MOVQX,
    OCERZ_OP_MOVLPS,
    OCERZ_OP_MOVHPS,
    OCERZ_OP_MOVLHPS,
    OCERZ_OP_MOVHLPS,
    OCERZ_OP_MOVMSKPS,
    OCERZ_OP_MOVMSKPD,
    OCERZ_OP_PMOVMSKB,
    OCERZ_OP_MOVSHDUP,
    OCERZ_OP_MOVSLDUP,
    OCERZ_OP_MOVDDUP,
    OCERZ_OP_MOVNTI,

    OCERZ_OP_ADDPS,
    OCERZ_OP_ADDPD,
    OCERZ_OP_ADDSS,
    OCERZ_OP_ADDSD,
    OCERZ_OP_SUBPS,
    OCERZ_OP_SUBPD,
    OCERZ_OP_SUBSS,
    OCERZ_OP_SUBSD,
    OCERZ_OP_MULPS,
    OCERZ_OP_MULPD,
    OCERZ_OP_MULSS,
    OCERZ_OP_MULSD,
    OCERZ_OP_DIVPS,
    OCERZ_OP_DIVPD,
    OCERZ_OP_DIVSS,
    OCERZ_OP_DIVSD,
    OCERZ_OP_MINPS,
    OCERZ_OP_MINPD,
    OCERZ_OP_MINSS,
    OCERZ_OP_MINSD,
    OCERZ_OP_MAXPS,
    OCERZ_OP_MAXPD,
    OCERZ_OP_MAXSS,
    OCERZ_OP_MAXSD,
    OCERZ_OP_SQRTPS,
    OCERZ_OP_SQRTPD,
    OCERZ_OP_SQRTSS,
    OCERZ_OP_SQRTSD,
    OCERZ_OP_HADDPS,
    OCERZ_OP_HADDPD,
    OCERZ_OP_HSUBPS,
    OCERZ_OP_HSUBPD,
    OCERZ_OP_ADDSUBPS,
    OCERZ_OP_ADDSUBPD,
    OCERZ_OP_RSQRTPS,
    OCERZ_OP_RSQRTSS,
    OCERZ_OP_RCPPS,
    OCERZ_OP_RCPSS,

    OCERZ_OP_ANDPS,
    OCERZ_OP_ANDNPS,
    OCERZ_OP_ORPS,
    OCERZ_OP_XORPS,
    OCERZ_OP_PAND,
    OCERZ_OP_PANDN,
    OCERZ_OP_POR,
    OCERZ_OP_PXOR,

    OCERZ_OP_CMPPS,
    OCERZ_OP_CMPPD,
    OCERZ_OP_CMPSS,
    OCERZ_OP_CMPSDX,
    OCERZ_OP_COMISS,
    OCERZ_OP_COMISD,
    OCERZ_OP_UCOMISS,
    OCERZ_OP_UCOMISD,
    OCERZ_OP_PCMPEQB,
    OCERZ_OP_PCMPEQW,
    OCERZ_OP_PCMPEQD,
    OCERZ_OP_PCMPEQQ,
    OCERZ_OP_PCMPGTB,
    OCERZ_OP_PCMPGTW,
    OCERZ_OP_PCMPGTD,
    OCERZ_OP_PCMPGTQ,

    OCERZ_OP_CVTSI2SS,
    OCERZ_OP_CVTSI2SD,
    OCERZ_OP_CVTSS2SI,
    OCERZ_OP_CVTSD2SI,
    OCERZ_OP_CVTTSS2SI,
    OCERZ_OP_CVTTSD2SI,
    OCERZ_OP_CVTSS2SD,
    OCERZ_OP_CVTSD2SS,
    OCERZ_OP_CVTPS2PD,
    OCERZ_OP_CVTPD2PS,
    OCERZ_OP_CVTDQ2PS,
    OCERZ_OP_CVTPS2DQ,
    OCERZ_OP_CVTTPS2DQ,
    OCERZ_OP_CVTDQ2PD,
    OCERZ_OP_CVTPD2DQ,
    OCERZ_OP_CVTTPD2DQ,

    OCERZ_OP_PADDB,
    OCERZ_OP_PADDW,
    OCERZ_OP_PADDD,
    OCERZ_OP_PADDQ,
    OCERZ_OP_PSUBB,
    OCERZ_OP_PSUBW,
    OCERZ_OP_PSUBD,
    OCERZ_OP_PSUBQ,
    OCERZ_OP_PADDSB,
    OCERZ_OP_PADDSW,
    OCERZ_OP_PADDUSB,
    OCERZ_OP_PADDUSW,
    OCERZ_OP_PSUBSB,
    OCERZ_OP_PSUBSW,
    OCERZ_OP_PSUBUSB,
    OCERZ_OP_PSUBUSW,
    OCERZ_OP_PMULLW,
    OCERZ_OP_PMULLD,
    OCERZ_OP_PMULHW,
    OCERZ_OP_PMULHUW,
    OCERZ_OP_PMULUDQ,
    OCERZ_OP_PMADDWD,
    OCERZ_OP_PAVGB,
    OCERZ_OP_PAVGW,
    OCERZ_OP_PMAXUB,
    OCERZ_OP_PMAXSW,
    OCERZ_OP_PMINUB,
    OCERZ_OP_PMINSW,
    OCERZ_OP_PMAXSB,
    OCERZ_OP_PMAXSD,
    OCERZ_OP_PMAXUW,
    OCERZ_OP_PMAXUD,
    OCERZ_OP_PMINSB,
    OCERZ_OP_PMINSD,
    OCERZ_OP_PMINUW,
    OCERZ_OP_PMINUD,
    OCERZ_OP_PSADBW,
    OCERZ_OP_PABSB,
    OCERZ_OP_PABSW,
    OCERZ_OP_PABSD,

    OCERZ_OP_PACKSSWB,
    OCERZ_OP_PACKSSDW,
    OCERZ_OP_PACKUSWB,
    OCERZ_OP_PACKUSDW,
    OCERZ_OP_PUNPCKLBW,
    OCERZ_OP_PUNPCKLWD,
    OCERZ_OP_PUNPCKLDQ,
    OCERZ_OP_PUNPCKLQDQ,
    OCERZ_OP_PUNPCKHBW,
    OCERZ_OP_PUNPCKHWD,
    OCERZ_OP_PUNPCKHDQ,
    OCERZ_OP_PUNPCKHQDQ,
    OCERZ_OP_PSHUFD,
    OCERZ_OP_PSHUFLW,
    OCERZ_OP_PSHUFHW,
    OCERZ_OP_PSHUFB,
    OCERZ_OP_PHADDW,
    OCERZ_OP_PHADDD,
    OCERZ_OP_PHADDSW,
    OCERZ_OP_PHSUBW,
    OCERZ_OP_PHSUBD,
    OCERZ_OP_PHSUBSW,
    OCERZ_OP_PSIGNB,
    OCERZ_OP_PSIGNW,
    OCERZ_OP_PSIGND,
    OCERZ_OP_PMADDUBSW,
    OCERZ_OP_PMULHRSW,
    OCERZ_OP_PALIGNR,
    OCERZ_OP_SHUFPS,
    OCERZ_OP_SHUFPD,
    OCERZ_OP_UNPCKLPS,
    OCERZ_OP_UNPCKHPS,
    OCERZ_OP_UNPCKLPD,
    OCERZ_OP_UNPCKHPD,

    OCERZ_OP_PSLLW,
    OCERZ_OP_PSLLD,
    OCERZ_OP_PSLLQ,
    OCERZ_OP_PSRLW,
    OCERZ_OP_PSRLD,
    OCERZ_OP_PSRLQ,
    OCERZ_OP_PSRAW,
    OCERZ_OP_PSRAD,
    OCERZ_OP_PSLLDQ,
    OCERZ_OP_PSRLDQ,

    OCERZ_OP_PEXTRB,
    OCERZ_OP_PEXTRW,
    OCERZ_OP_PEXTRD,
    OCERZ_OP_PEXTRQ,
    OCERZ_OP_PINSRB,
    OCERZ_OP_PINSRW,
    OCERZ_OP_PINSRD,
    OCERZ_OP_PINSRQ,
    OCERZ_OP_EXTRACTPS,
    OCERZ_OP_INSERTPS,

    OCERZ_OP_PTEST,
    OCERZ_OP_PMOVZXBW,
    OCERZ_OP_PMOVZXBD,
    OCERZ_OP_PMOVZXBQ,
    OCERZ_OP_PMOVZXWD,
    OCERZ_OP_PMOVZXWQ,
    OCERZ_OP_PMOVZXDQ,
    OCERZ_OP_PMOVSXBW,
    OCERZ_OP_PMOVSXBD,
    OCERZ_OP_PMOVSXBQ,
    OCERZ_OP_PMOVSXWD,
    OCERZ_OP_PMOVSXWQ,
    OCERZ_OP_PMOVSXDQ,
    OCERZ_OP_ROUNDPS,
    OCERZ_OP_ROUNDPD,
    OCERZ_OP_ROUNDSS,
    OCERZ_OP_ROUNDSD,
    OCERZ_OP_PBLENDW,
    OCERZ_OP_BLENDPS,
    OCERZ_OP_BLENDPD,
    OCERZ_OP_BLENDVPS,
    OCERZ_OP_BLENDVPD,
    OCERZ_OP_PBLENDVB,

    OCERZ_OP_AESENC,
    OCERZ_OP_AESENCLAST,
    OCERZ_OP_AESDEC,
    OCERZ_OP_AESDECLAST,
    OCERZ_OP_AESIMC,
    OCERZ_OP_AESKEYGENASSIST,
    OCERZ_OP_PCLMULQDQ,
    /* Far control transfers. Not reachable from ordinary 64-bit code, but they are the
     * entire WoW64 64<->32 mode switch: wow64cpu.dll uses FF /5 (far JMP m16:32) and IRETQ
     * to enter 32-bit code, and a far JMP back. Previously OCERZ_EUNDEF. */
    OCERZ_OP_JMPF,
    OCERZ_OP_CALLF,
    OCERZ_OP_RETF,

    OCERZ_OP_COUNT,
};

typedef struct X86Operand {
    uint8_t kind;
    uint8_t reg;
    uint8_t size;
    uint8_t high8;
    uint8_t base;
    uint8_t index;
    uint8_t scale;
    uint8_t riprel;
    int64_t disp;
    uint64_t imm;
} X86Operand;

typedef struct X86Insn {
    uint64_t rip;
    uint8_t len;
    uint16_t op;
    uint8_t opsize;
    uint8_t addrsize;
    uint8_t rep;
    uint8_t lock;
    uint8_t seg;
    uint8_t cc;
    uint8_t nops;
    X86Operand ops[3];
} X86Insn;

int ocerz_decode(const uint8_t *code, size_t avail, uint64_t rip, X86Insn *out);
const char *ocerz_op_name(unsigned op);
void ocerz_format_insn(const X86Insn *insn, char *buf, size_t cap);

#endif

/*
 * tests/unit/test_decode.c
 *
 * Self-contained unit test for the x86_64 decoder in src/decode.c. It drives
 * ocerz_decode() over a large static table of hand-encoded instructions whose
 * exact byte sequences were cross-checked against the LLVM assembler/objdump,
 * and asserts the decoded length, op, operand count, and a handful of
 * load-bearing operand fields per case.
 *
 * The table deliberately exercises the corners that are easy to get wrong:
 * the REX-dropped-by-a-later-legacy-prefix rule, AH versus SPL for 8-bit
 * encodings 4..7, RIP-relative resolution into an absolute disp, the SIB
 * base=101/mod=00 bare-disp32 form and the no-index SIB, the F2/F3-beats-66
 * mandatory-prefix precedence, MOVD versus MOVQX by REX.W, the multi-byte NOP
 * and ENDBR64 collapse to NOP, movzx into a 64-bit register, and a spread of
 * one-byte, 0F, 0F38, 0F3A and x87 opcodes so every coverage family has at
 * least one row. A few rows assert error returns (EUNDEF, ETRUNC, ETOOLONG).
 *
 * Each case carries an optional set of expectation fields; only the fields a
 * row sets are compared, using a small bitmask of CHK_* flags, so a row can
 * assert exactly what matters for the family it covers without over-fitting.
 * main() runs every row, prints any mismatch with the raw bytes and the
 * decoded-versus-expected values, and returns non-zero if any row failed.
 *
 * It links only against src/decode.c (pulled in by the unit harness) and the
 * standard C library; it needs no CPU, memory, or VM state because the decoder
 * is pure.
 *
 * One contract note: X86Insn.op is declared uint8_t in decode.h, but the
 * OcerzOp enum has 361 entries, so every op id at or above 256 (most of the
 * SSE/SSE2/SSE4 space) is stored truncated mod 256 in the struct. The decoder
 * writes the architectural enum value and the header field truncates it; the
 * op comparison here therefore checks insn.op against (uint8_t)expected so it
 * matches what actually lands in the struct. This is flagged as a gap for the
 * header owner to widen the field; until then high-numbered ops alias their
 * low-8-bit twins and cannot be told apart through this field.
 */
#include "ocerz/decode.h"
#include "ocerz/cpu.h"

#include <stdio.h>
#include <string.h>

enum {
    CHK_RET = 1 << 0,
    CHK_LEN = 1 << 1,
    CHK_OP = 1 << 2,
    CHK_NOPS = 1 << 3,
    CHK_OPSIZE = 1 << 4,
    CHK_CC = 1 << 5,
    CHK_REP = 1 << 6,
    CHK_SEG = 1 << 7,
    CHK_LOCK = 1 << 8,
    CHK_ADDRSIZE = 1 << 9,

    CHK_O0_KIND = 1 << 10,
    CHK_O0_REG = 1 << 11,
    CHK_O0_SIZE = 1 << 12,
    CHK_O0_HIGH8 = 1 << 13,
    CHK_O0_BASE = 1 << 14,
    CHK_O0_INDEX = 1 << 15,
    CHK_O0_SCALE = 1 << 16,
    CHK_O0_RIPREL = 1 << 17,
    CHK_O0_DISP = 1 << 18,
    CHK_O0_IMM = 1 << 19,

    CHK_O1_KIND = 1 << 20,
    CHK_O1_REG = 1 << 21,
    CHK_O1_SIZE = 1 << 22,
    CHK_O1_HIGH8 = 1 << 23,
    CHK_O1_IMM = 1 << 24,
    CHK_O1_DISP = 1 << 25,

    CHK_O2_KIND = 1 << 26,
    CHK_O2_IMM = 1 << 27,
    CHK_O2_REG = 1 << 28,
};

typedef struct Case {
    const char *name;
    uint8_t bytes[16];
    int nbytes;
    uint64_t rip;
    int avail;
    uint32_t chk;
    int ret;
    int len;
    int op;
    int nops;
    int opsize;
    int cc;
    int rep;
    int seg;
    int lock;
    int addrsize;
    int o0_kind, o0_reg, o0_size, o0_high8, o0_base, o0_index, o0_scale, o0_riprel;
    int64_t o0_disp;
    uint64_t o0_imm;
    int o1_kind, o1_reg, o1_size, o1_high8;
    uint64_t o1_imm;
    int64_t o1_disp;
    int o2_kind, o2_reg;
    uint64_t o2_imm;
} Case;

#define B(...) { __VA_ARGS__ }

static const Case cases[] = {
    { .name = "add al,bl", .bytes = B(0x00, 0xd8), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_LEN | CHK_OP | CHK_NOPS | CHK_OPSIZE | CHK_O0_KIND | CHK_O0_REG | CHK_O0_SIZE | CHK_O1_REG,
      .ret = OCERZ_OK, .len = 2, .op = OCERZ_OP_ADD, .nops = 2, .opsize = 1,
      .o0_kind = OCERZ_OPK_REG, .o0_reg = OCERZ_RAX, .o0_size = 1, .o1_reg = OCERZ_RBX },
    { .name = "add eax,ebx", .bytes = B(0x01, 0xd8), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_LEN | CHK_OP | CHK_OPSIZE | CHK_O0_REG | CHK_O0_SIZE,
      .ret = OCERZ_OK, .len = 2, .op = OCERZ_OP_ADD, .opsize = 4, .o0_reg = OCERZ_RAX, .o0_size = 4 },
    { .name = "add rax,rbx (REX.W)", .bytes = B(0x48, 0x01, 0xd8), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_LEN | CHK_OP | CHK_OPSIZE | CHK_O0_SIZE,
      .ret = OCERZ_OK, .len = 3, .op = OCERZ_OP_ADD, .opsize = 8, .o0_size = 8 },
    { .name = "or r8,r9 (REX.RB)", .bytes = B(0x4d, 0x09, 0xc8), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_REG | CHK_O1_REG,
      .ret = OCERZ_OK, .op = OCERZ_OP_OR, .o0_reg = OCERZ_R8, .o1_reg = OCERZ_R9 },
    { .name = "cmp r,rm form (3b)", .bytes = B(0x3b, 0xc1), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_REG | CHK_O1_REG,
      .ret = OCERZ_OK, .op = OCERZ_OP_CMP, .o0_reg = OCERZ_RAX, .o1_reg = OCERZ_RCX },
    { .name = "add al,imm8", .bytes = B(0x04, 0x7f), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_REG | CHK_O0_SIZE | CHK_O1_KIND | CHK_O1_IMM,
      .ret = OCERZ_OK, .op = OCERZ_OP_ADD, .o0_reg = OCERZ_RAX, .o0_size = 1,
      .o1_kind = OCERZ_OPK_IMM, .o1_imm = 0x7f },
    { .name = "add eax,imm32", .bytes = B(0x05, 0x78, 0x56, 0x34, 0x12), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_LEN | CHK_OP | CHK_O1_IMM,
      .ret = OCERZ_OK, .len = 5, .op = OCERZ_OP_ADD, .o1_imm = 0x12345678 },
    { .name = "sub rax,imm32 sx", .bytes = B(0x48, 0x2d, 0xff, 0xff, 0xff, 0xff), .nbytes = 6, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O1_IMM,
      .ret = OCERZ_OK, .op = OCERZ_OP_SUB, .o1_imm = 0xffffffffffffffffULL },

    { .name = "add ah,al (no rex)", .bytes = B(0x00, 0xc4), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_REG | CHK_O0_HIGH8 | CHK_O1_REG | CHK_O1_HIGH8,
      .ret = OCERZ_OK, .op = OCERZ_OP_ADD, .o0_reg = OCERZ_RAX, .o0_high8 = 1,
      .o1_reg = OCERZ_RAX, .o1_high8 = 0 },
    { .name = "mov spl,al (rex)", .bytes = B(0x40, 0x88, 0xc4), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_REG | CHK_O0_HIGH8 | CHK_O0_SIZE,
      .ret = OCERZ_OK, .op = OCERZ_OP_MOV, .o0_reg = OCERZ_RSP, .o0_high8 = 0, .o0_size = 1 },

    { .name = "rex then 66 drops rex", .bytes = B(0x41, 0x66, 0x01, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_OPSIZE | CHK_O0_REG | CHK_O1_REG,
      .ret = OCERZ_OK, .op = OCERZ_OP_ADD, .opsize = 2, .o0_reg = OCERZ_RCX, .o1_reg = OCERZ_RAX },
    { .name = "66 then rex.w keeps 8", .bytes = B(0x66, 0x48, 0x01, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OPSIZE, .ret = OCERZ_OK, .opsize = 8 },
    { .name = "45 0F prefix combo (cmov)", .bytes = B(0x45, 0x0f, 0x44, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_CC | CHK_O0_REG | CHK_O1_REG,
      .ret = OCERZ_OK, .op = OCERZ_OP_CMOVCC, .cc = OCERZ_CC_E, .o0_reg = OCERZ_R8, .o1_reg = OCERZ_R9 },

    { .name = "mov rax,imm64", .bytes = B(0x48, 0xb8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11), .nbytes = 10, .avail = 12,
      .chk = CHK_RET | CHK_LEN | CHK_OP | CHK_O0_REG | CHK_O0_SIZE | CHK_O1_KIND | CHK_O1_IMM,
      .ret = OCERZ_OK, .len = 10, .op = OCERZ_OP_MOV, .o0_reg = OCERZ_RAX, .o0_size = 8,
      .o1_kind = OCERZ_OPK_IMM, .o1_imm = 0x1122334455667788ULL },
    { .name = "mov eax,imm32", .bytes = B(0xb8, 0x78, 0x56, 0x34, 0x12), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_LEN | CHK_OP | CHK_O0_SIZE | CHK_O1_IMM,
      .ret = OCERZ_OK, .len = 5, .op = OCERZ_OP_MOV, .o0_size = 4, .o1_imm = 0x12345678 },
    { .name = "mov r8b,imm8 (rex.b)", .bytes = B(0x41, 0xb0, 0x55), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_REG | CHK_O0_SIZE | CHK_O0_HIGH8,
      .ret = OCERZ_OK, .op = OCERZ_OP_MOV, .o0_reg = OCERZ_R8, .o0_size = 1, .o0_high8 = 0 },
    { .name = "mov ch,imm8 (no rex high8)", .bytes = B(0xb5, 0x12), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_REG | CHK_O0_HIGH8,
      .ret = OCERZ_OK, .op = OCERZ_OP_MOV, .o0_reg = OCERZ_RCX, .o0_high8 = 1 },

    { .name = "movsxd rax,ebx", .bytes = B(0x48, 0x63, 0xc3), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_OPSIZE | CHK_O0_SIZE | CHK_O1_SIZE,
      .ret = OCERZ_OK, .op = OCERZ_OP_MOVSXD, .opsize = 8, .o0_size = 8, .o1_size = 4 },
    { .name = "movsxd no rex.w", .bytes = B(0x63, 0xc3), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_SIZE | CHK_O1_SIZE,
      .ret = OCERZ_OK, .op = OCERZ_OP_MOVSXD, .o0_size = 4, .o1_size = 4 },

    { .name = "lea rax,[rip+0x10]", .bytes = B(0x48, 0x8d, 0x05, 0x10, 0x00, 0x00, 0x00), .nbytes = 7,
      .rip = 0x1000, .avail = 8,
      .chk = CHK_RET | CHK_LEN | CHK_OP | CHK_O1_KIND | CHK_O0_REG,
      .ret = OCERZ_OK, .len = 7, .op = OCERZ_OP_LEA, .o0_reg = OCERZ_RAX, .o1_kind = OCERZ_OPK_MEM,
      .o1_disp = 0x1000 + 7 + 0x10 },
    { .name = "mov eax,[rip+0] riprel abs", .bytes = B(0x8b, 0x05, 0x00, 0x00, 0x00, 0x00), .nbytes = 6,
      .rip = 0x4000, .avail = 8,
      .chk = CHK_RET | CHK_LEN | CHK_O1_DISP | CHK_O1_KIND,
      .ret = OCERZ_OK, .len = 6, .o1_kind = OCERZ_OPK_MEM, .o1_disp = 0x4000 + 6 },

    { .name = "mov eax,[rbp-4]", .bytes = B(0x8b, 0x45, 0xfc), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_LEN | CHK_O1_KIND | CHK_O1_DISP,
      .ret = OCERZ_OK, .len = 3, .o1_kind = OCERZ_OPK_MEM, .o1_disp = -4 },
    { .name = "mov rax,[rax+rbx*4+0x10]", .bytes = B(0x48, 0x8b, 0x44, 0x98, 0x10), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_LEN | CHK_O1_KIND | CHK_O1_DISP,
      .ret = OCERZ_OK, .len = 5, .o1_kind = OCERZ_OPK_MEM, .o1_disp = 0x10 },
    { .name = "mov eax,[disp32] sib base=101", .bytes = B(0x8b, 0x04, 0x25, 0x78, 0x56, 0x34, 0x12), .nbytes = 7, .avail = 8,
      .chk = CHK_RET | CHK_LEN | CHK_O1_KIND | CHK_O1_DISP,
      .ret = OCERZ_OK, .len = 7, .o1_kind = OCERZ_OPK_MEM, .o1_disp = 0x12345678 },
    { .name = "mov eax,[rsp] sib no-index", .bytes = B(0x8b, 0x04, 0x24), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_LEN | CHK_O1_KIND,
      .ret = OCERZ_OK, .len = 3, .o1_kind = OCERZ_OPK_MEM },

    { .name = "push rax", .bytes = B(0x50), .nbytes = 1, .avail = 8,
      .chk = CHK_RET | CHK_LEN | CHK_OP | CHK_OPSIZE | CHK_O0_REG | CHK_O0_SIZE,
      .ret = OCERZ_OK, .len = 1, .op = OCERZ_OP_PUSH, .opsize = 8, .o0_reg = OCERZ_RAX, .o0_size = 8 },
    { .name = "push r13", .bytes = B(0x41, 0x55), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_REG, .ret = OCERZ_OK, .op = OCERZ_OP_PUSH, .o0_reg = OCERZ_R13 },
    { .name = "push ax (66)", .bytes = B(0x66, 0x50), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OPSIZE, .ret = OCERZ_OK, .opsize = 2 },
    { .name = "pop r8", .bytes = B(0x41, 0x58), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_REG, .ret = OCERZ_OK, .op = OCERZ_OP_POP, .o0_reg = OCERZ_R8 },
    { .name = "push imm32", .bytes = B(0x68, 0x78, 0x56, 0x34, 0x12), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_KIND | CHK_O0_IMM,
      .ret = OCERZ_OK, .op = OCERZ_OP_PUSH, .o0_kind = OCERZ_OPK_IMM, .o0_imm = 0x12345678 },
    { .name = "push imm8 sx", .bytes = B(0x6a, 0xff), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_IMM, .ret = OCERZ_OK, .op = OCERZ_OP_PUSH,
      .o0_imm = 0xffffffffffffffffULL },
    { .name = "jmp rel32 target", .bytes = B(0xe9, 0x00, 0x00, 0x00, 0x00), .nbytes = 5, .rip = 0x2000, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_KIND | CHK_O0_IMM,
      .ret = OCERZ_OK, .op = OCERZ_OP_JMP, .o0_kind = OCERZ_OPK_IMM, .o0_imm = 0x2000 + 5 },
    { .name = "jmp rel8 target", .bytes = B(0xeb, 0x10), .nbytes = 2, .rip = 0x100, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_IMM, .ret = OCERZ_OK, .op = OCERZ_OP_JMP, .o0_imm = 0x100 + 2 + 0x10 },
    { .name = "jne rel8", .bytes = B(0x75, 0x05), .nbytes = 2, .rip = 0x200, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_CC | CHK_O0_IMM,
      .ret = OCERZ_OK, .op = OCERZ_OP_JCC, .cc = OCERZ_CC_NE, .o0_imm = 0x200 + 2 + 5 },
    { .name = "jg rel32", .bytes = B(0x0f, 0x8f, 0x00, 0x00, 0x00, 0x00), .nbytes = 6, .rip = 0x300, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_CC, .ret = OCERZ_OK, .op = OCERZ_OP_JCC, .cc = OCERZ_CC_G },
    { .name = "call rel32", .bytes = B(0xe8, 0x00, 0x00, 0x00, 0x00), .nbytes = 5, .rip = 0x500, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_IMM, .ret = OCERZ_OK, .op = OCERZ_OP_CALL, .o0_imm = 0x500 + 5 },
    { .name = "loop rel8", .bytes = B(0xe2, 0x02), .nbytes = 2, .rip = 0x10, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_LOOP },
    { .name = "jrcxz rel8", .bytes = B(0xe3, 0x02), .nbytes = 2, .rip = 0x10, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_JRCXZ },
    { .name = "ret", .bytes = B(0xc3), .nbytes = 1, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_NOPS, .ret = OCERZ_OK, .op = OCERZ_OP_RET, .nops = 0 },
    { .name = "ret imm16", .bytes = B(0xc2, 0x08, 0x00), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_NOPS | CHK_O0_IMM, .ret = OCERZ_OK, .op = OCERZ_OP_RET, .nops = 1, .o0_imm = 8 },
    /* WoW64 mode-switch encodings. These are the ENTIRE 64->32 transition in
     * wow64cpu.dll, and every one of them decoded as OCERZ_EUNDEF before. Lengths are
     * cross-checked against capstone; a wrong length desyncs the whole instruction stream,
     * so CHK_LEN is the load-bearing assertion here. */
    { .name = "far jmp [r14] (41 ff 2e, the wow64cpu 64->32 edge)",
      .bytes = B(0x41, 0xff, 0x2e), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_LEN | CHK_NOPS | CHK_O0_KIND,
      .ret = OCERZ_OK, .len = 3, .op = OCERZ_OP_JMPF, .nops = 1, .o0_kind = OCERZ_OPK_MEM },
    { .name = "far jmp [rip+disp32] (ff 2d, the 32->64 thunk)",
      .bytes = B(0xff, 0x2d, 0x00, 0x10, 0x00, 0x00), .nbytes = 6, .rip = 0x10, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_LEN | CHK_NOPS | CHK_O0_KIND,
      .ret = OCERZ_OK, .len = 6, .op = OCERZ_OP_JMPF, .nops = 1, .o0_kind = OCERZ_OPK_MEM },
    { .name = "far call [rax] (ff /3)", .bytes = B(0xff, 0x18), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_LEN | CHK_NOPS | CHK_O0_KIND,
      .ret = OCERZ_OK, .len = 2, .op = OCERZ_OP_CALLF, .nops = 1, .o0_kind = OCERZ_OPK_MEM },
    { .name = "far jmp register form is invalid (ff /5 mod=11)",
      .bytes = B(0xff, 0xee), .nbytes = 2, .avail = 8,
      .chk = CHK_RET, .ret = OCERZ_EUNDEF },
    /* MOV Sreg, r/m16 -- inert in 64-bit but no longer discarded; ops[1] carries the
     * destination segment (0=ES 1=CS 2=SS 3=DS 4=FS 5=GS). The third case is the exact
     * instruction WoW64 uses to install the 32-bit TEB. */
    { .name = "mov ds,eax (8e d8)", .bytes = B(0x8e, 0xd8), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_LEN | CHK_NOPS | CHK_O1_IMM,
      .ret = OCERZ_OK, .len = 2, .op = OCERZ_OP_MOVSEG, .nops = 2, .o1_imm = 3 },
    { .name = "mov es,eax (8e c0)", .bytes = B(0x8e, 0xc0), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_LEN | CHK_NOPS | CHK_O1_IMM,
      .ret = OCERZ_OK, .len = 2, .op = OCERZ_OP_MOVSEG, .nops = 2, .o1_imm = 0 },
    { .name = "mov fs,[r13+0x90] (the WoW64 32-bit TEB install)",
      .bytes = B(0x41, 0x8e, 0xa5, 0x90, 0x00, 0x00, 0x00), .nbytes = 7, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_LEN | CHK_NOPS | CHK_O0_KIND | CHK_O1_IMM,
      .ret = OCERZ_OK, .len = 7, .op = OCERZ_OP_MOVSEG, .nops = 2,
      .o0_kind = OCERZ_OPK_MEM, .o1_imm = 4 },
    { .name = "retf", .bytes = B(0xcb), .nbytes = 1, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_LEN | CHK_NOPS, .ret = OCERZ_OK, .len = 1,
      .op = OCERZ_OP_RETF, .nops = 0 },
    { .name = "retf imm16", .bytes = B(0xca, 0x08, 0x00), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_LEN | CHK_NOPS | CHK_O0_IMM, .ret = OCERZ_OK, .len = 3,
      .op = OCERZ_OP_RETF, .nops = 1, .o0_imm = 8 },
    { .name = "leave", .bytes = B(0xc9), .nbytes = 1, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_LEAVE },

    { .name = "add rm,imm8 (83 /0)", .bytes = B(0x83, 0xc0, 0x05), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_REG | CHK_O1_IMM, .ret = OCERZ_OK, .op = OCERZ_OP_ADD,
      .o0_reg = OCERZ_RAX, .o1_imm = 5 },
    { .name = "cmp rm,imm32 (81 /7)", .bytes = B(0x81, 0xf9, 0x00, 0x01, 0x00, 0x00), .nbytes = 6, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O1_IMM, .ret = OCERZ_OK, .op = OCERZ_OP_CMP, .o1_imm = 0x100 },
    { .name = "and byte rm,imm8 (80 /4)", .bytes = B(0x80, 0xe0, 0x0f), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_OPSIZE, .ret = OCERZ_OK, .op = OCERZ_OP_AND, .opsize = 1 },
    { .name = "shl eax,imm8 (c1 /4)", .bytes = B(0xc1, 0xe0, 0x03), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O1_IMM, .ret = OCERZ_OK, .op = OCERZ_OP_SHL, .o1_imm = 3 },
    { .name = "sal alias (c1 /6) -> shl", .bytes = B(0xc1, 0xf0, 0x02), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_SHL },
    { .name = "sar eax,1 (d1 /7)", .bytes = B(0xd1, 0xf8), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O1_KIND | CHK_O1_IMM, .ret = OCERZ_OK, .op = OCERZ_OP_SAR,
      .o1_kind = OCERZ_OPK_IMM, .o1_imm = 1 },
    { .name = "shl eax,cl (d3 /4)", .bytes = B(0xd3, 0xe0), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O1_KIND | CHK_O1_REG, .ret = OCERZ_OK, .op = OCERZ_OP_SHL,
      .o1_kind = OCERZ_OPK_REG, .o1_reg = OCERZ_RCX },
    { .name = "neg eax (f7 /3)", .bytes = B(0xf7, 0xd8), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_NOPS, .ret = OCERZ_OK, .op = OCERZ_OP_NEG, .nops = 1 },
    { .name = "test eax,imm32 (f7 /0)", .bytes = B(0xf7, 0xc0, 0x01, 0x00, 0x00, 0x00), .nbytes = 6, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_NOPS | CHK_O1_IMM, .ret = OCERZ_OK, .op = OCERZ_OP_TEST, .nops = 2, .o1_imm = 1 },
    { .name = "mul rm (f6 /4)", .bytes = B(0xf6, 0xe3), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_NOPS, .ret = OCERZ_OK, .op = OCERZ_OP_MUL, .nops = 1 },
    { .name = "inc rm8 (fe /0)", .bytes = B(0xfe, 0xc0), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_OPSIZE, .ret = OCERZ_OK, .op = OCERZ_OP_INC, .opsize = 1 },
    { .name = "call rm (ff /2)", .bytes = B(0xff, 0xd0), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_OPSIZE | CHK_O0_REG, .ret = OCERZ_OK, .op = OCERZ_OP_CALL, .opsize = 8, .o0_reg = OCERZ_RAX },
    { .name = "push rm (ff /6)", .bytes = B(0xff, 0x30), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_KIND, .ret = OCERZ_OK, .op = OCERZ_OP_PUSH, .o0_kind = OCERZ_OPK_MEM },
    { .name = "imul r,rm,imm8 (6b)", .bytes = B(0x6b, 0xc3, 0x10), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_NOPS | CHK_O2_KIND | CHK_O2_IMM,
      .ret = OCERZ_OK, .op = OCERZ_OP_IMUL, .nops = 3, .o2_kind = OCERZ_OPK_IMM, .o2_imm = 0x10 },

    { .name = "test rm,r (85)", .bytes = B(0x85, 0xc1), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_NOPS, .ret = OCERZ_OK, .op = OCERZ_OP_TEST, .nops = 2 },
    { .name = "xchg rm,r (87)", .bytes = B(0x87, 0xc1), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_XCHG },
    { .name = "xchg rax,r8 (90+rex.b)", .bytes = B(0x49, 0x90), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_REG | CHK_O1_REG, .ret = OCERZ_OK, .op = OCERZ_OP_XCHG,
      .o0_reg = OCERZ_RAX, .o1_reg = OCERZ_R8 },
    { .name = "nop (90)", .bytes = B(0x90), .nbytes = 1, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_NOPS, .ret = OCERZ_OK, .op = OCERZ_OP_NOP, .nops = 0 },
    { .name = "pause (f3 90)", .bytes = B(0xf3, 0x90), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_PAUSE },
    { .name = "xchg eax,ecx (91)", .bytes = B(0x91), .nbytes = 1, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O1_REG, .ret = OCERZ_OK, .op = OCERZ_OP_XCHG, .o1_reg = OCERZ_RCX },

    { .name = "cdqe (98 rex.w)", .bytes = B(0x48, 0x98), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_OPSIZE, .ret = OCERZ_OK, .op = OCERZ_OP_CBW, .opsize = 8 },
    { .name = "cqo (99 rex.w)", .bytes = B(0x48, 0x99), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_OPSIZE, .ret = OCERZ_OK, .op = OCERZ_OP_CWD, .opsize = 8 },
    { .name = "pushf", .bytes = B(0x9c), .nbytes = 1, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_OPSIZE, .ret = OCERZ_OK, .op = OCERZ_OP_PUSHF, .opsize = 8 },
    { .name = "sahf", .bytes = B(0x9e), .nbytes = 1, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_SAHF },
    { .name = "int3", .bytes = B(0xcc), .nbytes = 1, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_INT3 },
    { .name = "int imm8", .bytes = B(0xcd, 0x80), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_IMM, .ret = OCERZ_OK, .op = OCERZ_OP_INT, .o0_imm = 0x80 },
    { .name = "hlt", .bytes = B(0xf4), .nbytes = 1, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_HLT },
    { .name = "cmc", .bytes = B(0xf5), .nbytes = 1, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_CMC },
    { .name = "std", .bytes = B(0xfd), .nbytes = 1, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_STD },

    { .name = "mov al,moffs8 (a0)", .bytes = B(0xa0, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00), .nbytes = 9, .avail = 12,
      .chk = CHK_RET | CHK_LEN | CHK_OP | CHK_O1_KIND | CHK_O1_DISP,
      .ret = OCERZ_OK, .len = 9, .op = OCERZ_OP_MOV, .o1_kind = OCERZ_OPK_MEM, .o1_disp = 0x1000 },
    { .name = "rep movs (f3 a5)", .bytes = B(0xf3, 0xa5), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_REP | CHK_NOPS, .ret = OCERZ_OK, .op = OCERZ_OP_MOVS, .rep = OCERZ_REP_REP, .nops = 0 },
    { .name = "repne scas (f2 ae)", .bytes = B(0xf2, 0xae), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_REP, .ret = OCERZ_OK, .op = OCERZ_OP_SCAS, .rep = OCERZ_REP_REPNE },
    { .name = "stos (aa)", .bytes = B(0xaa), .nbytes = 1, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_OPSIZE, .ret = OCERZ_OK, .op = OCERZ_OP_STOS, .opsize = 1 },
    { .name = "test al,imm8 (a8)", .bytes = B(0xa8, 0x01), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O1_IMM, .ret = OCERZ_OK, .op = OCERZ_OP_TEST, .o1_imm = 1 },

    { .name = "fs mov (64)", .bytes = B(0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00), .nbytes = 9, .avail = 12,
      .chk = CHK_RET | CHK_SEG, .ret = OCERZ_OK, .seg = OCERZ_SEG_FS },
    { .name = "67 addr size 4", .bytes = B(0x67, 0x8b, 0x00), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_ADDRSIZE, .ret = OCERZ_OK, .addrsize = 4 },
    { .name = "lock add (f0)", .bytes = B(0xf0, 0x01, 0x00), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_LOCK, .ret = OCERZ_OK, .lock = 1 },

    { .name = "syscall", .bytes = B(0x0f, 0x05), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_SYSCALL },
    { .name = "ud2", .bytes = B(0x0f, 0x0b), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_UD2 },
    { .name = "cpuid", .bytes = B(0x0f, 0xa2), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_CPUID },
    { .name = "rdtsc", .bytes = B(0x0f, 0x31), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_RDTSC },
    { .name = "endbr64 -> nop", .bytes = B(0xf3, 0x0f, 0x1e, 0xfa), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_LEN | CHK_OP, .ret = OCERZ_OK, .len = 4, .op = OCERZ_OP_NOP },
    { .name = "multibyte nop 0f 1f /0", .bytes = B(0x0f, 0x1f, 0x00), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_LEN | CHK_OP, .ret = OCERZ_OK, .len = 3, .op = OCERZ_OP_NOP },
    { .name = "nop [rax+rax+disp32] 0f1f /4", .bytes = B(0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00), .nbytes = 8, .avail = 9,
      .chk = CHK_RET | CHK_LEN | CHK_OP, .ret = OCERZ_OK, .len = 8, .op = OCERZ_OP_NOP },
    { .name = "prefetch 0f 0d", .bytes = B(0x0f, 0x0d, 0x00), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_PREFETCH },
    { .name = "lfence (0f ae e8)", .bytes = B(0x0f, 0xae, 0xe8), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_LFENCE },
    { .name = "ldmxcsr (0f ae /2)", .bytes = B(0x0f, 0xae, 0x10), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_LDMXCSR },

    { .name = "setne rm8", .bytes = B(0x0f, 0x95, 0xc0), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_CC | CHK_OPSIZE, .ret = OCERZ_OK, .op = OCERZ_OP_SETCC, .cc = OCERZ_CC_NE, .opsize = 1 },
    { .name = "movzx rax,byte [rbx]", .bytes = B(0x48, 0x0f, 0xb6, 0x03), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_SIZE | CHK_O1_SIZE | CHK_O1_KIND,
      .ret = OCERZ_OK, .op = OCERZ_OP_MOVZX, .o0_size = 8, .o1_size = 1, .o1_kind = OCERZ_OPK_MEM },
    { .name = "movzx eax,word [rbx]", .bytes = B(0x0f, 0xb7, 0x03), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_SIZE | CHK_O1_SIZE, .ret = OCERZ_OK, .op = OCERZ_OP_MOVZX, .o0_size = 4, .o1_size = 2 },
    { .name = "movsx rax,word [rbx]", .bytes = B(0x48, 0x0f, 0xbf, 0x03), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_SIZE | CHK_O1_SIZE, .ret = OCERZ_OK, .op = OCERZ_OP_MOVSX, .o0_size = 8, .o1_size = 2 },

    { .name = "bswap rax", .bytes = B(0x48, 0x0f, 0xc8), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_OPSIZE | CHK_O0_REG, .ret = OCERZ_OK, .op = OCERZ_OP_BSWAP, .opsize = 8, .o0_reg = OCERZ_RAX },
    { .name = "bt rax,rbx (0f a3)", .bytes = B(0x48, 0x0f, 0xa3, 0xd8), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_REG | CHK_O1_REG, .ret = OCERZ_OK, .op = OCERZ_OP_BT, .o0_reg = OCERZ_RAX, .o1_reg = OCERZ_RBX },
    { .name = "bt rm,imm8 (0f ba /4)", .bytes = B(0x48, 0x0f, 0xba, 0xe0, 0x05), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O1_IMM, .ret = OCERZ_OK, .op = OCERZ_OP_BT, .o1_imm = 5 },
    { .name = "shld rm,r,imm8", .bytes = B(0x48, 0x0f, 0xa4, 0xd8, 0x05), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_NOPS | CHK_O2_KIND | CHK_O2_IMM, .ret = OCERZ_OK, .op = OCERZ_OP_SHLD, .nops = 3,
      .o2_kind = OCERZ_OPK_IMM, .o2_imm = 5 },
    { .name = "shrd rm,r,cl", .bytes = B(0x0f, 0xad, 0xd8), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O2_KIND | CHK_O2_REG, .ret = OCERZ_OK, .op = OCERZ_OP_SHRD,
      .o2_kind = OCERZ_OPK_REG, .o2_reg = OCERZ_RCX },
    { .name = "imul r,rm (0f af)", .bytes = B(0x0f, 0xaf, 0xc1), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_NOPS, .ret = OCERZ_OK, .op = OCERZ_OP_IMUL, .nops = 2 },
    { .name = "cmpxchg rm,r (0f b1)", .bytes = B(0x48, 0x0f, 0xb1, 0x18), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_KIND, .ret = OCERZ_OK, .op = OCERZ_OP_CMPXCHG, .o0_kind = OCERZ_OPK_MEM },
    { .name = "cmpxchg16b (0f c7 /1 rex.w)", .bytes = B(0x48, 0x0f, 0xc7, 0x08), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_OPSIZE | CHK_NOPS, .ret = OCERZ_OK, .op = OCERZ_OP_CMPXCHGXB, .opsize = 16, .nops = 1 },
    { .name = "cmpxchg8b (0f c7 /1)", .bytes = B(0x0f, 0xc7, 0x08), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_OPSIZE, .ret = OCERZ_OK, .op = OCERZ_OP_CMPXCHGXB, .opsize = 8 },
    { .name = "xadd rm,r (0f c1)", .bytes = B(0x0f, 0xc1, 0xc8), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_XADD },
    { .name = "popcnt (f3 0f b8)", .bytes = B(0xf3, 0x0f, 0xb8, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_POPCNT },
    { .name = "tzcnt (f3 0f bc)", .bytes = B(0xf3, 0x0f, 0xbc, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_TZCNT },
    { .name = "bsf (0f bc)", .bytes = B(0x0f, 0xbc, 0xc1), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_BSF },

    { .name = "movups xmm0,xmm1", .bytes = B(0x0f, 0x10, 0xc1), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_KIND | CHK_O0_REG, .ret = OCERZ_OK, .op = OCERZ_OP_MOVUPS,
      .o0_kind = OCERZ_OPK_XMM, .o0_reg = 0 },
    { .name = "movss (f3 0f 10)", .bytes = B(0xf3, 0x0f, 0x10, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_SIZE, .ret = OCERZ_OK, .op = OCERZ_OP_MOVSS, .o0_size = 4 },
    { .name = "movsd (f2 0f 10)", .bytes = B(0xf2, 0x0f, 0x10, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_SIZE, .ret = OCERZ_OK, .op = OCERZ_OP_MOVSDX, .o0_size = 8 },
    { .name = "movapd->movaps (66 0f 28)", .bytes = B(0x66, 0x0f, 0x28, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_MOVAPS },
    { .name = "movdqa (66 0f 6f)", .bytes = B(0x66, 0x0f, 0x6f, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_MOVDQA },
    { .name = "movdqu (f3 0f 6f)", .bytes = B(0xf3, 0x0f, 0x6f, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_MOVDQU },
    { .name = "movhlps (0f 12 reg)", .bytes = B(0x0f, 0x12, 0xc1), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_MOVHLPS },
    { .name = "movlps mem (0f 12 mem)", .bytes = B(0x0f, 0x12, 0x00), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_MOVLPS },

    { .name = "addps (0f 58)", .bytes = B(0x0f, 0x58, 0xc1), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_SIZE, .ret = OCERZ_OK, .op = OCERZ_OP_ADDPS, .o0_size = 16 },
    { .name = "addsd (f2 0f 58)", .bytes = B(0xf2, 0x0f, 0x58, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_SIZE, .ret = OCERZ_OK, .op = OCERZ_OP_ADDSD, .o0_size = 8 },
    { .name = "mulss (f3 0f 59)", .bytes = B(0xf3, 0x0f, 0x59, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_MULSS },
    { .name = "divpd (66 0f 5e)", .bytes = B(0x66, 0x0f, 0x5e, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_DIVPD },
    { .name = "xorps (0f 57)", .bytes = B(0x0f, 0x57, 0xc1), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_XORPS },
    { .name = "andpd->andps (66 0f 54)", .bytes = B(0x66, 0x0f, 0x54, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_ANDPS },
    { .name = "ucomiss (0f 2e)", .bytes = B(0x0f, 0x2e, 0xc1), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_SIZE, .ret = OCERZ_OK, .op = OCERZ_OP_UCOMISS, .o0_size = 4 },
    { .name = "comisd (66 0f 2f)", .bytes = B(0x66, 0x0f, 0x2f, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_COMISD },

    { .name = "cvtsi2sd rax (f2 rex.w)", .bytes = B(0xf2, 0x48, 0x0f, 0x2a, 0xc0), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O1_SIZE, .ret = OCERZ_OK, .op = OCERZ_OP_CVTSI2SD, .o1_size = 8 },
    { .name = "cvtsi2ss eax (f3)", .bytes = B(0xf3, 0x0f, 0x2a, 0xc0), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O1_SIZE, .ret = OCERZ_OK, .op = OCERZ_OP_CVTSI2SS, .o1_size = 4 },
    { .name = "cvttsd2si rax (f2 rex.w)", .bytes = B(0xf2, 0x48, 0x0f, 0x2c, 0xc0), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_SIZE | CHK_O0_KIND, .ret = OCERZ_OK, .op = OCERZ_OP_CVTTSD2SI,
      .o0_size = 8, .o0_kind = OCERZ_OPK_REG },
    { .name = "cvtss2sd (f3 0f 5a)", .bytes = B(0xf3, 0x0f, 0x5a, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_CVTSS2SD },
    { .name = "cvtdq2ps (0f 5b)", .bytes = B(0x0f, 0x5b, 0xc1), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_CVTDQ2PS },

    { .name = "pxor (66 0f ef)", .bytes = B(0x66, 0x0f, 0xef, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_KIND, .ret = OCERZ_OK, .op = OCERZ_OP_PXOR, .o0_kind = OCERZ_OPK_XMM },
    { .name = "paddq (66 0f d4)", .bytes = B(0x66, 0x0f, 0xd4, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_PADDQ },
    { .name = "paddb (66 0f fc)", .bytes = B(0x66, 0x0f, 0xfc, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_PADDB },
    { .name = "pcmpeqd (66 0f 76)", .bytes = B(0x66, 0x0f, 0x76, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_PCMPEQD },
    { .name = "punpcklqdq (66 0f 6c)", .bytes = B(0x66, 0x0f, 0x6c, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_PUNPCKLQDQ },
    { .name = "psrlw by xmm (66 0f d1)", .bytes = B(0x66, 0x0f, 0xd1, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_PSRLW },
    { .name = "pslldq imm (66 0f 73 /7)", .bytes = B(0x66, 0x0f, 0x73, 0xf8, 0x04), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O1_IMM, .ret = OCERZ_OK, .op = OCERZ_OP_PSLLDQ, .o1_imm = 4 },
    { .name = "psrld imm (66 0f 72 /2)", .bytes = B(0x66, 0x0f, 0x72, 0xd0, 0x03), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O1_IMM, .ret = OCERZ_OK, .op = OCERZ_OP_PSRLD, .o1_imm = 3 },
    { .name = "pshufd (66 0f 70)", .bytes = B(0x66, 0x0f, 0x70, 0xc1, 0x1b), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_NOPS | CHK_O2_IMM, .ret = OCERZ_OK, .op = OCERZ_OP_PSHUFD, .nops = 3, .o2_imm = 0x1b },
    { .name = "pshufhw (f3 0f 70)", .bytes = B(0xf3, 0x0f, 0x70, 0xc1, 0x1b), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_PSHUFHW },
    { .name = "shufps imm (0f c6)", .bytes = B(0x0f, 0xc6, 0xc1, 0x00), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_NOPS, .ret = OCERZ_OK, .op = OCERZ_OP_SHUFPS, .nops = 3 },
    { .name = "cmpsd imm (f2 0f c2)", .bytes = B(0xf2, 0x0f, 0xc2, 0xc1, 0x00), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_CMPSDX },
    { .name = "pmovmskb (66 0f d7)", .bytes = B(0x66, 0x0f, 0xd7, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_KIND, .ret = OCERZ_OK, .op = OCERZ_OP_PMOVMSKB, .o0_kind = OCERZ_OPK_REG },
    { .name = "movmskps (0f 50)", .bytes = B(0x0f, 0x50, 0xc1), .nbytes = 3, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_KIND, .ret = OCERZ_OK, .op = OCERZ_OP_MOVMSKPS, .o0_kind = OCERZ_OPK_REG },
    { .name = "emms (0f 77)", .bytes = B(0x0f, 0x77), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_EMMS },

    { .name = "movd xmm0,eax (66 0f 6e)", .bytes = B(0x66, 0x0f, 0x6e, 0xc0), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_KIND | CHK_O1_SIZE, .ret = OCERZ_OK, .op = OCERZ_OP_MOVD,
      .o0_kind = OCERZ_OPK_XMM, .o1_size = 4 },
    { .name = "movq xmm0,rax (66 48 0f 6e)", .bytes = B(0x66, 0x48, 0x0f, 0x6e, 0xc0), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O1_SIZE, .ret = OCERZ_OK, .op = OCERZ_OP_MOVQX, .o1_size = 8 },
    { .name = "movq store (66 0f d6)", .bytes = B(0x66, 0x0f, 0xd6, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_MOVQX },
    { .name = "movq load (f3 0f 7e)", .bytes = B(0xf3, 0x0f, 0x7e, 0xc1), .nbytes = 4, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_MOVQX },

    { .name = "pshufb (66 0f 38 00)", .bytes = B(0x66, 0x0f, 0x38, 0x00, 0xc1), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_LEN | CHK_OP, .ret = OCERZ_OK, .len = 5, .op = OCERZ_OP_PSHUFB },
    { .name = "ptest (66 0f 38 17)", .bytes = B(0x66, 0x0f, 0x38, 0x17, 0xc1), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_PTEST },
    { .name = "pcmpeqq (66 0f 38 29)", .bytes = B(0x66, 0x0f, 0x38, 0x29, 0xc1), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_PCMPEQQ },
    { .name = "pmovsxbw (66 0f 38 20)", .bytes = B(0x66, 0x0f, 0x38, 0x20, 0xc1), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_PMOVSXBW },
    { .name = "pmovzxbq (66 0f 38 32)", .bytes = B(0x66, 0x0f, 0x38, 0x32, 0xc1), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_PMOVZXBQ },
    { .name = "pmulld (66 0f 38 40)", .bytes = B(0x66, 0x0f, 0x38, 0x40, 0xc1), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_PMULLD },
    { .name = "pminsd (66 0f 38 39)", .bytes = B(0x66, 0x0f, 0x38, 0x39, 0xc1), .nbytes = 5, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_PMINSD },

    { .name = "roundsd (66 0f 3a 0b)", .bytes = B(0x66, 0x0f, 0x3a, 0x0b, 0xc1, 0x01), .nbytes = 6, .avail = 8,
      .chk = CHK_RET | CHK_LEN | CHK_OP | CHK_NOPS, .ret = OCERZ_OK, .len = 6, .op = OCERZ_OP_ROUNDSD, .nops = 3 },
    { .name = "pextrd eax,xmm0 (66 0f 3a 16)", .bytes = B(0x66, 0x0f, 0x3a, 0x16, 0xc0, 0x02), .nbytes = 6, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_KIND, .ret = OCERZ_OK, .op = OCERZ_OP_PEXTRD, .o0_kind = OCERZ_OPK_REG },
    { .name = "pextrq rax,xmm0 (66 48 0f 3a 16)", .bytes = B(0x66, 0x48, 0x0f, 0x3a, 0x16, 0xc0, 0x02), .nbytes = 7, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_SIZE, .ret = OCERZ_OK, .op = OCERZ_OP_PEXTRQ, .o0_size = 8 },
    { .name = "palignr (66 0f 3a 0f)", .bytes = B(0x66, 0x0f, 0x3a, 0x0f, 0xc1, 0x04), .nbytes = 6, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_PALIGNR },
    { .name = "insertps (66 0f 3a 21)", .bytes = B(0x66, 0x0f, 0x3a, 0x21, 0xc1, 0x00), .nbytes = 6, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_INSERTPS },
    { .name = "pinsrb (66 0f 3a 20)", .bytes = B(0x66, 0x0f, 0x3a, 0x20, 0xc1, 0x00), .nbytes = 6, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_KIND, .ret = OCERZ_OK, .op = OCERZ_OP_PINSRB, .o0_kind = OCERZ_OPK_XMM },
    { .name = "pextrb mem (66 0f 3a 14)", .bytes = B(0x66, 0x0f, 0x3a, 0x14, 0x00, 0x01), .nbytes = 6, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_PEXTRB },

    { .name = "fld dword [rax] (d9 /0)", .bytes = B(0xd9, 0x00), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_KIND | CHK_O0_SIZE, .ret = OCERZ_OK, .op = OCERZ_OP_FLD,
      .o0_kind = OCERZ_OPK_MEM, .o0_size = 4 },
    { .name = "fld qword [rax] (dd /0)", .bytes = B(0xdd, 0x00), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_SIZE, .ret = OCERZ_OK, .op = OCERZ_OP_FLD, .o0_size = 8 },
    { .name = "fld m80 (db /5)", .bytes = B(0xdb, 0x28), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_SIZE, .ret = OCERZ_OK, .op = OCERZ_OP_FLD, .o0_size = 10 },
    { .name = "fldcw (d9 /5)", .bytes = B(0xd9, 0x28), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_SIZE, .ret = OCERZ_OK, .op = OCERZ_OP_FLDCW, .o0_size = 2 },
    { .name = "fnstcw (d9 /7)", .bytes = B(0xd9, 0x38), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_FNSTCW },
    { .name = "fnstsw ax (df e0)", .bytes = B(0xdf, 0xe0), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_KIND | CHK_O0_REG | CHK_O0_SIZE, .ret = OCERZ_OK, .op = OCERZ_OP_FNSTSW,
      .o0_kind = OCERZ_OPK_REG, .o0_reg = OCERZ_RAX, .o0_size = 2 },
    { .name = "fild m16 (df /0)", .bytes = B(0xdf, 0x00), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_SIZE, .ret = OCERZ_OK, .op = OCERZ_OP_FILD, .o0_size = 2 },
    { .name = "fild m64 (df /5)", .bytes = B(0xdf, 0x28), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_SIZE, .ret = OCERZ_OK, .op = OCERZ_OP_FILD, .o0_size = 8 },
    { .name = "fadd st0,st1 (d8 c1)", .bytes = B(0xd8, 0xc1), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_KIND | CHK_O0_REG, .ret = OCERZ_OK, .op = OCERZ_OP_FADD,
      .o0_kind = OCERZ_OPK_ST, .o0_reg = 0 },
    { .name = "fld st1 (d9 c1)", .bytes = B(0xd9, 0xc1), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_KIND | CHK_O0_REG, .ret = OCERZ_OK, .op = OCERZ_OP_FLD,
      .o0_kind = OCERZ_OPK_ST, .o0_reg = 1 },
    { .name = "fchs (d9 e0)", .bytes = B(0xd9, 0xe0), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_FCHS },
    { .name = "fldz (d9 ee)", .bytes = B(0xd9, 0xee), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_FLDZ },
    { .name = "fsqrt (d9 fa)", .bytes = B(0xd9, 0xfa), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_FSQRT },
    { .name = "fsin (d9 fe)", .bytes = B(0xd9, 0xfe), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_FSIN },
    { .name = "fcmovb st0,st1 (da c1)", .bytes = B(0xda, 0xc1), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_CC, .ret = OCERZ_OK, .op = OCERZ_OP_FCMOVCC, .cc = OCERZ_CC_B },
    { .name = "fcmovnb st0,st1 (db c1)", .bytes = B(0xdb, 0xc1), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_CC, .ret = OCERZ_OK, .op = OCERZ_OP_FCMOVCC, .cc = OCERZ_CC_AE },
    { .name = "fninit (db e3)", .bytes = B(0xdb, 0xe3), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_FNINIT },
    { .name = "fucomi st0,st1 (db e9)", .bytes = B(0xdb, 0xe9), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_FUCOMI },
    { .name = "fadd st1,st0 (dc c1)", .bytes = B(0xdc, 0xc1), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_KIND | CHK_O0_REG, .ret = OCERZ_OK, .op = OCERZ_OP_FADD,
      .o0_kind = OCERZ_OPK_ST, .o0_reg = 1 },
    { .name = "ffree st1 (dd c1)", .bytes = B(0xdd, 0xc1), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_FFREE },
    { .name = "faddp st1,st0 (de c1)", .bytes = B(0xde, 0xc1), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP | CHK_O0_REG, .ret = OCERZ_OK, .op = OCERZ_OP_FADDP, .o0_reg = 1 },
    { .name = "fcompp (de d9)", .bytes = B(0xde, 0xd9), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_FCOMPP },
    { .name = "fcomip st0,st1 (df f1)", .bytes = B(0xdf, 0xf1), .nbytes = 2, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_FCOMIP },
    { .name = "fwait (9b)", .bytes = B(0x9b), .nbytes = 1, .avail = 8,
      .chk = CHK_RET | CHK_OP, .ret = OCERZ_OK, .op = OCERZ_OP_FWAIT },

    { .name = "EUNDEF cli (fa)", .bytes = B(0xfa), .nbytes = 1, .avail = 8,
      .chk = CHK_RET, .ret = OCERZ_EUNDEF },
    { .name = "EUNDEF in al (e4)", .bytes = B(0xe4, 0x00), .nbytes = 2, .avail = 8,
      .chk = CHK_RET, .ret = OCERZ_EUNDEF },
    { .name = "EUNDEF mmx paddd no66 (0f fe)", .bytes = B(0x0f, 0xfe, 0xc1), .nbytes = 3, .avail = 8,
      .chk = CHK_RET, .ret = OCERZ_EUNDEF },
    { .name = "EUNDEF c7 /1", .bytes = B(0xc7, 0xc8, 0x00, 0x00, 0x00, 0x00), .nbytes = 6, .avail = 8,
      .chk = CHK_RET, .ret = OCERZ_EUNDEF },
    { .name = "ETRUNC mov imm64 short", .bytes = B(0x48, 0xb8, 0x00, 0x00), .nbytes = 4, .avail = 4,
      .chk = CHK_RET, .ret = OCERZ_ETRUNC },
    { .name = "ETRUNC empty", .bytes = B(0x00), .nbytes = 0, .avail = 0,
      .chk = CHK_RET, .ret = OCERZ_ETRUNC },
    { .name = "ETOOLONG 16 prefixes", .bytes = B(0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66), .nbytes = 16, .avail = 16,
      .chk = CHK_RET, .ret = OCERZ_ETOOLONG },
};

static int field_fail(const char *name, const char *field, long long got, long long want)
{
    printf("FAIL %s: %s got %lld (0x%llx) want %lld (0x%llx)\n",
           name, field, got, (unsigned long long)got, want, (unsigned long long)want);
    return 1;
}

static int run_case(const Case *c)
{
    X86Insn insn;
    int r = ocerz_decode(c->bytes, (size_t)c->avail, c->rip, &insn);
    int fails = 0;

    if (c->chk & CHK_RET) {
        if (r != c->ret)
            fails += field_fail(c->name, "ret", r, c->ret);
    }
    if (r != OCERZ_OK)
        return fails;

    if ((c->chk & CHK_LEN) && insn.len != c->len)
        fails += field_fail(c->name, "len", insn.len, c->len);
    if ((c->chk & CHK_OP) && insn.op != c->op)
        fails += field_fail(c->name, "op", insn.op, c->op);
    if ((c->chk & CHK_NOPS) && insn.nops != c->nops)
        fails += field_fail(c->name, "nops", insn.nops, c->nops);
    if ((c->chk & CHK_OPSIZE) && insn.opsize != c->opsize)
        fails += field_fail(c->name, "opsize", insn.opsize, c->opsize);
    if ((c->chk & CHK_CC) && insn.cc != c->cc)
        fails += field_fail(c->name, "cc", insn.cc, c->cc);
    if ((c->chk & CHK_REP) && insn.rep != c->rep)
        fails += field_fail(c->name, "rep", insn.rep, c->rep);
    if ((c->chk & CHK_SEG) && insn.seg != c->seg)
        fails += field_fail(c->name, "seg", insn.seg, c->seg);
    if ((c->chk & CHK_LOCK) && insn.lock != c->lock)
        fails += field_fail(c->name, "lock", insn.lock, c->lock);
    if ((c->chk & CHK_ADDRSIZE) && insn.addrsize != c->addrsize)
        fails += field_fail(c->name, "addrsize", insn.addrsize, c->addrsize);

    const X86Operand *o0 = &insn.ops[0];
    if ((c->chk & CHK_O0_KIND) && o0->kind != c->o0_kind)
        fails += field_fail(c->name, "o0.kind", o0->kind, c->o0_kind);
    if ((c->chk & CHK_O0_REG) && o0->reg != c->o0_reg)
        fails += field_fail(c->name, "o0.reg", o0->reg, c->o0_reg);
    if ((c->chk & CHK_O0_SIZE) && o0->size != c->o0_size)
        fails += field_fail(c->name, "o0.size", o0->size, c->o0_size);
    if ((c->chk & CHK_O0_HIGH8) && o0->high8 != c->o0_high8)
        fails += field_fail(c->name, "o0.high8", o0->high8, c->o0_high8);
    if ((c->chk & CHK_O0_BASE) && o0->base != c->o0_base)
        fails += field_fail(c->name, "o0.base", o0->base, c->o0_base);
    if ((c->chk & CHK_O0_INDEX) && o0->index != c->o0_index)
        fails += field_fail(c->name, "o0.index", o0->index, c->o0_index);
    if ((c->chk & CHK_O0_SCALE) && o0->scale != c->o0_scale)
        fails += field_fail(c->name, "o0.scale", o0->scale, c->o0_scale);
    if ((c->chk & CHK_O0_RIPREL) && o0->riprel != c->o0_riprel)
        fails += field_fail(c->name, "o0.riprel", o0->riprel, c->o0_riprel);
    if ((c->chk & CHK_O0_DISP) && o0->disp != c->o0_disp)
        fails += field_fail(c->name, "o0.disp", o0->disp, c->o0_disp);
    if ((c->chk & CHK_O0_IMM) && o0->imm != c->o0_imm)
        fails += field_fail(c->name, "o0.imm", (long long)o0->imm, (long long)c->o0_imm);

    const X86Operand *o1 = &insn.ops[1];
    if ((c->chk & CHK_O1_KIND) && o1->kind != c->o1_kind)
        fails += field_fail(c->name, "o1.kind", o1->kind, c->o1_kind);
    if ((c->chk & CHK_O1_REG) && o1->reg != c->o1_reg)
        fails += field_fail(c->name, "o1.reg", o1->reg, c->o1_reg);
    if ((c->chk & CHK_O1_SIZE) && o1->size != c->o1_size)
        fails += field_fail(c->name, "o1.size", o1->size, c->o1_size);
    if ((c->chk & CHK_O1_HIGH8) && o1->high8 != c->o1_high8)
        fails += field_fail(c->name, "o1.high8", o1->high8, c->o1_high8);
    if ((c->chk & CHK_O1_IMM) && o1->imm != c->o1_imm)
        fails += field_fail(c->name, "o1.imm", (long long)o1->imm, (long long)c->o1_imm);
    if ((c->chk & CHK_O1_DISP) && o1->disp != c->o1_disp)
        fails += field_fail(c->name, "o1.disp", o1->disp, c->o1_disp);

    const X86Operand *o2 = &insn.ops[2];
    if ((c->chk & CHK_O2_KIND) && o2->kind != c->o2_kind)
        fails += field_fail(c->name, "o2.kind", o2->kind, c->o2_kind);
    if ((c->chk & CHK_O2_REG) && o2->reg != c->o2_reg)
        fails += field_fail(c->name, "o2.reg", o2->reg, c->o2_reg);
    if ((c->chk & CHK_O2_IMM) && o2->imm != c->o2_imm)
        fails += field_fail(c->name, "o2.imm", (long long)o2->imm, (long long)c->o2_imm);

    return fails;
}

int main(void)
{
    int n = (int)(sizeof(cases) / sizeof(cases[0]));
    int total_fails = 0;
    for (int i = 0; i < n; i++)
        total_fails += run_case(&cases[i]);

    if (total_fails == 0) {
        printf("test_decode: all %d cases passed\n", n);
        return 0;
    }
    printf("test_decode: %d failures across %d cases\n", total_fails, n);
    return 1;
}

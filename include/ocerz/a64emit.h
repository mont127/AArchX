/*
 * include/ocerz/a64emit.h
 *
 * A minimal arm64 (AArch64) instruction emitter: each function appends one
 * machine instruction to an A64Buf cursor. This is the JIT backend's only
 * way of producing code, so every encoding lives in exactly one place and
 * is exercised by tests/unit/test_a64emit.c, which executes emitted
 * sequences and checks their results (run-don't-read validation: an
 * encoding bug cannot hide from an executed branch, load, or flag check).
 *
 * Conventions: register numbers are 0..30 plus 31 meaning ZR or SP
 * depending on the instruction class (these helpers always mean XZR/WZR).
 * The sf parameter selects 64-bit (1) or 32-bit (0) operation width.
 * Sized loads and stores take the access size in bytes (1/2/4/8) and an
 * unsigned byte offset that must be a multiple of the size (the scaled-
 * immediate form); 32-bit and smaller loads zero-extend into the X
 * register, matching how the JIT exploits them for x86 sub-register reads.
 * Shifted-register ALU forms only expose LSL shifts, which is all the
 * translator needs. Bitfield helpers (ubfx/sbfx/bfi and the extend
 * aliases) wrap UBFM/SBFM/BFM with the architectural alias arithmetic so
 * call sites read like the assembler syntax. Branch offsets are given in
 * INSTRUCTION WORDS relative to the branch itself (the a64_label/patch pair
 * supports forward branches: remember the cursor, emit a placeholder, then
 * patch once the target is known).
 *
 * a64_mov_imm64 emits the shortest MOVZ/MOVN+MOVK sequence for the value.
 * Condition codes use the ARM numbering (A64_EQ..A64_LE); A64_INV(cc)
 * flips a condition's polarity (the encoding's low-bit toggle).
 */
#ifndef OCERZ_A64EMIT_H
#define OCERZ_A64EMIT_H

#include "ocerz/types.h"

enum {
    A64_EQ = 0, A64_NE = 1, A64_CS = 2, A64_CC = 3,
    A64_MI = 4, A64_PL = 5, A64_VS = 6, A64_VC = 7,
    A64_HI = 8, A64_LS = 9, A64_GE = 10, A64_LT = 11,
    A64_GT = 12, A64_LE = 13, A64_AL = 14,
};

#define A64_INV(cc) ((cc) ^ 1)
#define A64_ZR 31

typedef struct A64Buf {
    uint32_t *start;
    uint32_t *p;
    uint32_t *end;
    int overflow;
    /* Patch sink for labels taken from a full buffer. a64_emit32 clamps at
     * p == end without advancing, so once the buffer is full every subsequent
     * a64_label() would hand out `end` itself — an address one word PAST the
     * mapping — and the a64_patch_* helpers write through the site they are
     * given. a64_label() returns &sink instead, so patches from a dead block
     * land here rather than off the end of the MAP_JIT region. The block is
     * discarded (or demoted to the interpreter), so the value is never read. */
    uint32_t sink;
} A64Buf;

void a64_emit32(A64Buf *b, uint32_t w);
uint32_t *a64_label(A64Buf *b);

void a64_movz(A64Buf *b, int rd, uint16_t imm, int hw);
void a64_movk(A64Buf *b, int rd, uint16_t imm, int hw);
void a64_movn(A64Buf *b, int rd, uint16_t imm, int hw);
void a64_mov_imm64(A64Buf *b, int rd, uint64_t v);
void a64_mov_reg(A64Buf *b, int sf, int rd, int rm);

void a64_ldr(A64Buf *b, int size, int rt, int rn, uint32_t off);
void a64_str(A64Buf *b, int size, int rt, int rn, uint32_t off);
void a64_ldar(A64Buf *b, int size, int rt, int rn);
void a64_stlr(A64Buf *b, int size, int rt, int rn);
void a64_dmb_ish(A64Buf *b);
void a64_ldrsb(A64Buf *b, int sf, int rt, int rn, uint32_t off);
void a64_ldrsh(A64Buf *b, int sf, int rt, int rn, uint32_t off);
void a64_ldrsw(A64Buf *b, int rt, int rn, uint32_t off);

void a64_add_imm(A64Buf *b, int sf, int rd, int rn, uint32_t imm12);
void a64_sub_imm(A64Buf *b, int sf, int rd, int rn, uint32_t imm12);
void a64_adds_imm(A64Buf *b, int sf, int rd, int rn, uint32_t imm12);
void a64_subs_imm(A64Buf *b, int sf, int rd, int rn, uint32_t imm12);

void a64_add_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl);
void a64_adds_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl);
void a64_sub_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl);
void a64_subs_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl);
void a64_adcs_reg(A64Buf *b, int sf, int rd, int rn, int rm);
void a64_sbcs_reg(A64Buf *b, int sf, int rd, int rn, int rm);

void a64_and_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl);
void a64_ands_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl);
void a64_orr_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl);
void a64_orn_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl);
void a64_eor_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl);
void a64_bic_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl);

void a64_lslv(A64Buf *b, int sf, int rd, int rn, int rm);
void a64_lsrv(A64Buf *b, int sf, int rd, int rn, int rm);
void a64_asrv(A64Buf *b, int sf, int rd, int rn, int rm);

void a64_ubfm(A64Buf *b, int sf, int rd, int rn, int immr, int imms);
void a64_sbfm(A64Buf *b, int sf, int rd, int rn, int immr, int imms);
void a64_bfm(A64Buf *b, int sf, int rd, int rn, int immr, int imms);
void a64_ubfx(A64Buf *b, int sf, int rd, int rn, int lsb, int width);
void a64_sbfx(A64Buf *b, int sf, int rd, int rn, int lsb, int width);
void a64_bfi(A64Buf *b, int sf, int rd, int rn, int lsb, int width);
void a64_lsl_imm(A64Buf *b, int sf, int rd, int rn, int sh);
void a64_lsr_imm(A64Buf *b, int sf, int rd, int rn, int sh);
void a64_asr_imm(A64Buf *b, int sf, int rd, int rn, int sh);
void a64_uxtb(A64Buf *b, int rd, int rn);
void a64_uxth(A64Buf *b, int rd, int rn);
void a64_sxtb(A64Buf *b, int sf, int rd, int rn);
void a64_sxth(A64Buf *b, int sf, int rd, int rn);
void a64_sxtw(A64Buf *b, int rd, int rn);

void a64_cset(A64Buf *b, int rd, int cond);
void a64_csel(A64Buf *b, int sf, int rd, int rn, int rm, int cond);

void a64_mul(A64Buf *b, int sf, int rd, int rn, int rm);
void a64_umulh(A64Buf *b, int rd, int rn, int rm);
void a64_smulh(A64Buf *b, int rd, int rn, int rm);
void a64_rev(A64Buf *b, int sf, int rd, int rn);

void a64_b(A64Buf *b, int32_t off_words);
void a64_bcond(A64Buf *b, int cond, int32_t off_words);
void a64_cbz(A64Buf *b, int sf, int rt, int32_t off_words);
void a64_cbnz(A64Buf *b, int sf, int rt, int32_t off_words);
void a64_patch_b(uint32_t *at, uint32_t *target);
/* Range-checked B patch: returns 1 and patches if target is within the +-128MB
 * (+-(2^25-1) words) reach of an unconditional B at `at`; returns 0 and patches
 * NOTHING if out of range. Unlike a64_patch_b, which masks off & 0x03ffffff with
 * no range check and would silently encode a branch to an arbitrary arena
 * address, this is safe to use for cross-block chaining where the target may be
 * anywhere in the (1GB, 8x the B reach) arena. */
int a64_try_patch_b(uint32_t *at, uint32_t *target);
void a64_patch_bcond(uint32_t *at, uint32_t *target);
void a64_patch_cbz(uint32_t *at, uint32_t *target);
void a64_ret(A64Buf *b);
void a64_br(A64Buf *b, int rn);
void a64_blr(A64Buf *b, int rn);

void a64_stp_pre(A64Buf *b, int rt, int rt2, int rn, int imm);
void a64_ldp_post(A64Buf *b, int rt, int rt2, int rn, int imm);
void a64_stp_off(A64Buf *b, int rt, int rt2, int rn, int imm);
void a64_ldp_off(A64Buf *b, int rt, int rt2, int rn, int imm);

#endif

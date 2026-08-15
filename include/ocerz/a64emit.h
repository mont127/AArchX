/* arm64 instruction emitter: each call appends one instruction to an A64Buf. */
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

    uint32_t sink;
} A64Buf;

void a64_emit32(A64Buf *b, uint32_t w);
uint32_t *a64_label(A64Buf *b);

void a64_movz(A64Buf *b, int rd, uint16_t imm, int hw);
void a64_movk(A64Buf *b, int rd, uint16_t imm, int hw);
void a64_movn(A64Buf *b, int rd, uint16_t imm, int hw);
void a64_mov_imm64(A64Buf *b, int rd, uint64_t v);
void a64_mov_reg(A64Buf *b, int sf, int rd, int rm);

int a64_try_and_imm(A64Buf *b, int sf, int rd, int rn, uint64_t imm);
int a64_try_ands_imm(A64Buf *b, int sf, int rd, int rn, uint64_t imm);
int a64_try_orr_imm(A64Buf *b, int sf, int rd, int rn, uint64_t imm);
int a64_try_eor_imm(A64Buf *b, int sf, int rd, int rn, uint64_t imm);

void a64_ldr(A64Buf *b, int size, int rt, int rn, uint32_t off);
void a64_str(A64Buf *b, int size, int rt, int rn, uint32_t off);
void a64_ldr_post64(A64Buf *b, int rt, int rn, int imm);
void a64_str_pre64(A64Buf *b, int rt, int rn, int imm);
void a64_ldr_regoff(A64Buf *b, int size, int rt, int rn, int rm, int scaled);
void a64_str_regoff(A64Buf *b, int size, int rt, int rn, int rm, int scaled);
void a64_ldar(A64Buf *b, int size, int rt, int rn);
void a64_stlr(A64Buf *b, int size, int rt, int rn);
void a64_ldapr(A64Buf *b, int size, int rt, int rn);
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
void a64_rorv(A64Buf *b, int sf, int rd, int rn, int rm);
void a64_extr(A64Buf *b, int sf, int rd, int rn, int rm, int lsb);
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
void a64_tbz(A64Buf *b, int rt, int bit, int32_t off_words);
void a64_tbnz(A64Buf *b, int rt, int bit, int32_t off_words);
void a64_patch_b(uint32_t *at, uint32_t *target);

int a64_try_patch_b(uint32_t *at, uint32_t *target);
void a64_patch_bcond(uint32_t *at, uint32_t *target);
void a64_patch_cbz(uint32_t *at, uint32_t *target);
void a64_patch_tbz(uint32_t *at, uint32_t *target);
void a64_ret(A64Buf *b);
void a64_br(A64Buf *b, int rn);
void a64_blr(A64Buf *b, int rn);

void a64_stp_pre(A64Buf *b, int rt, int rt2, int rn, int imm);
void a64_ldp_post(A64Buf *b, int rt, int rt2, int rn, int imm);
void a64_stp_off(A64Buf *b, int rt, int rt2, int rn, int imm);
void a64_ldp_off(A64Buf *b, int rt, int rt2, int rn, int imm);

#endif

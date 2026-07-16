/*
 * src/a64emit.c
 *
 * The arm64 instruction emitter implementing a64emit.h: one function per
 * machine instruction, each appending a single 32-bit word to the buffer
 * cursor. Encodings follow the ARM Architecture Reference Manual field
 * layouts directly; the comments-free body keeps each encoder to its raw
 * bit assembly so that test_a64emit.c (which EXECUTES emitted code) is the
 * specification of correctness rather than prose.
 *
 * Field conventions used throughout: sf is bit 31 (1=64-bit). Data-
 * processing-immediate ADD/SUB carry a 12-bit unsigned immediate with an
 * optional 12-bit left shift (only the unshifted form is exposed). Load/
 * store use the unsigned-offset (scaled) encoding, with the size field in
 * bits 30-31 and the byte offset divided by the access size. Logical-
 * shifted-register forms always use shift type LSL (00) with the amount in
 * bits 10-15. The bitfield helpers compute UBFM/SBFM/BFM immr/imms from the
 * higher-level lsb/width arguments using the standard alias formulas. Branch
 * encoders take a signed word displacement and pack the two's-complement
 * imm field; the patch helpers recompute that field once a forward target
 * is known by reading the displacement from the already-emitted word.
 *
 * a64_mov_imm64 chooses the shortest constant materialization: a single
 * MOVZ or MOVN when three 16-bit lanes are 0x0000 or 0xffff respectively,
 * otherwise a MOVZ seed followed by MOVK for each remaining nonzero lane.
 */
#include "ocerz/a64emit.h"

void a64_emit32(A64Buf *b, uint32_t w)
{
    if (b->p >= b->end) {
        b->overflow = 1;
        return;
    }
    *b->p++ = w;
}

uint32_t *a64_label(A64Buf *b)
{
    return b->p;
}

void a64_movz(A64Buf *b, int rd, uint16_t imm, int hw)
{
    a64_emit32(b, 0xd2800000u | ((uint32_t)(hw & 3) << 21) | ((uint32_t)imm << 5) | (uint32_t)(rd & 31));
}

void a64_movk(A64Buf *b, int rd, uint16_t imm, int hw)
{
    a64_emit32(b, 0xf2800000u | ((uint32_t)(hw & 3) << 21) | ((uint32_t)imm << 5) | (uint32_t)(rd & 31));
}

void a64_movn(A64Buf *b, int rd, uint16_t imm, int hw)
{
    a64_emit32(b, 0x92800000u | ((uint32_t)(hw & 3) << 21) | ((uint32_t)imm << 5) | (uint32_t)(rd & 31));
}

void a64_mov_imm64(A64Buf *b, int rd, uint64_t v)
{
    uint16_t lane[4] = {
        (uint16_t)(v), (uint16_t)(v >> 16), (uint16_t)(v >> 32), (uint16_t)(v >> 48),
    };
    int zero = 0, ones = 0;
    for (int i = 0; i < 4; i++) {
        if (lane[i] == 0x0000)
            zero++;
        if (lane[i] == 0xffff)
            ones++;
    }
    if (ones > zero) {
        int seeded = 0;
        for (int i = 0; i < 4; i++) {
            if (lane[i] != 0xffff) {
                if (!seeded) {
                    a64_movn(b, rd, (uint16_t)~lane[i], i);
                    seeded = 1;
                } else {
                    a64_movk(b, rd, lane[i], i);
                }
            }
        }
        if (!seeded)
            a64_movn(b, rd, 0, 0);
        return;
    }
    int seeded = 0;
    for (int i = 0; i < 4; i++) {
        if (lane[i] != 0x0000) {
            if (!seeded) {
                a64_movz(b, rd, lane[i], i);
                seeded = 1;
            } else {
                a64_movk(b, rd, lane[i], i);
            }
        }
    }
    if (!seeded)
        a64_movz(b, rd, 0, 0);
}

void a64_mov_reg(A64Buf *b, int sf, int rd, int rm)
{
    a64_orr_reg(b, sf, rd, A64_ZR, rm, 0);
}

static uint32_t ldst_size_bits(int size)
{
    switch (size) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    default: return 3;
    }
}

void a64_ldr(A64Buf *b, int size, int rt, int rn, uint32_t off)
{
    uint32_t sz = ldst_size_bits(size);
    uint32_t imm12 = off / (uint32_t)size;
    a64_emit32(b, 0x39400000u | (sz << 30) | (imm12 << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
}

void a64_str(A64Buf *b, int size, int rt, int rn, uint32_t off)
{
    uint32_t sz = ldst_size_bits(size);
    uint32_t imm12 = off / (uint32_t)size;
    a64_emit32(b, 0x39000000u | (sz << 30) | (imm12 << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
}

/* Load-Acquire / Store-Release (LDAR/STLR): the ordered forms of LDR/STR the
 * JIT uses for GUEST memory so plain x86 accesses keep Total Store Order. These
 * are the base-register-only [Xn] encodings (no immediate offset): the Rs and
 * Rt2 fields are fixed to 0b11111 and size selects the width in bits 31-30, so
 * the body mirrors a64_ldr/a64_str minus the scaled imm12. */
void a64_ldar(A64Buf *b, int size, int rt, int rn)
{
    uint32_t sz = ldst_size_bits(size);
    a64_emit32(b, 0x08dffc00u | (sz << 30) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
}

void a64_stlr(A64Buf *b, int size, int rt, int rn)
{
    uint32_t sz = ldst_size_bits(size);
    a64_emit32(b, 0x089ffc00u | (sz << 30) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
}

/* DMB ISH: inner-shareable full data barrier. Provided as the offset-handling
 * fallback for any guest access the ldar/stlr [Xn] form cannot express; the
 * current JIT materializes every guest address into a base register with no
 * displacement, so it uses ldar/stlr directly and never needs this. */
void a64_dmb_ish(A64Buf *b)
{
    a64_emit32(b, 0xd5033bbfu);
}

void a64_ldrsb(A64Buf *b, int sf, int rt, int rn, uint32_t off)
{
    uint32_t opc = sf ? 2u : 3u;
    a64_emit32(b, 0x38000000u | (opc << 22) | (off << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
}

void a64_ldrsh(A64Buf *b, int sf, int rt, int rn, uint32_t off)
{
    uint32_t opc = sf ? 2u : 3u;
    a64_emit32(b, 0x78000000u | (opc << 22) | ((off / 2) << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
}

void a64_ldrsw(A64Buf *b, int rt, int rn, uint32_t off)
{
    a64_emit32(b, 0xb8800000u | ((off / 4) << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
}

static void addsub_imm(A64Buf *b, uint32_t base, int sf, int rd, int rn, uint32_t imm12)
{
    a64_emit32(b, base | ((uint32_t)sf << 31) | ((imm12 & 0xfff) << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}

void a64_add_imm(A64Buf *b, int sf, int rd, int rn, uint32_t imm12) { addsub_imm(b, 0x11000000u, sf, rd, rn, imm12); }
void a64_sub_imm(A64Buf *b, int sf, int rd, int rn, uint32_t imm12) { addsub_imm(b, 0x51000000u, sf, rd, rn, imm12); }
void a64_adds_imm(A64Buf *b, int sf, int rd, int rn, uint32_t imm12) { addsub_imm(b, 0x31000000u, sf, rd, rn, imm12); }
void a64_subs_imm(A64Buf *b, int sf, int rd, int rn, uint32_t imm12) { addsub_imm(b, 0x71000000u, sf, rd, rn, imm12); }

static void addsub_reg(A64Buf *b, uint32_t base, int sf, int rd, int rn, int rm, int lsl)
{
    a64_emit32(b, base | ((uint32_t)sf << 31) | ((uint32_t)(rm & 31) << 16) | ((uint32_t)(lsl & 63) << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}

void a64_add_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl) { addsub_reg(b, 0x0b000000u, sf, rd, rn, rm, lsl); }
void a64_adds_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl) { addsub_reg(b, 0x2b000000u, sf, rd, rn, rm, lsl); }
void a64_sub_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl) { addsub_reg(b, 0x4b000000u, sf, rd, rn, rm, lsl); }
void a64_subs_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl) { addsub_reg(b, 0x6b000000u, sf, rd, rn, rm, lsl); }

void a64_adcs_reg(A64Buf *b, int sf, int rd, int rn, int rm)
{
    a64_emit32(b, 0x3a000000u | ((uint32_t)sf << 31) | ((uint32_t)(rm & 31) << 16) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}

void a64_sbcs_reg(A64Buf *b, int sf, int rd, int rn, int rm)
{
    a64_emit32(b, 0x7a000000u | ((uint32_t)sf << 31) | ((uint32_t)(rm & 31) << 16) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}

static void logic_reg(A64Buf *b, uint32_t base, int sf, int rd, int rn, int rm, int lsl)
{
    a64_emit32(b, base | ((uint32_t)sf << 31) | ((uint32_t)(rm & 31) << 16) | ((uint32_t)(lsl & 63) << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}

void a64_and_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl) { logic_reg(b, 0x0a000000u, sf, rd, rn, rm, lsl); }
void a64_ands_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl) { logic_reg(b, 0x6a000000u, sf, rd, rn, rm, lsl); }
void a64_orr_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl) { logic_reg(b, 0x2a000000u, sf, rd, rn, rm, lsl); }
void a64_orn_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl) { logic_reg(b, 0x2a200000u, sf, rd, rn, rm, lsl); }
void a64_eor_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl) { logic_reg(b, 0x4a000000u, sf, rd, rn, rm, lsl); }
void a64_bic_reg(A64Buf *b, int sf, int rd, int rn, int rm, int lsl) { logic_reg(b, 0x0a200000u, sf, rd, rn, rm, lsl); }

static void datap2(A64Buf *b, uint32_t op, int sf, int rd, int rn, int rm)
{
    a64_emit32(b, 0x1ac00000u | ((uint32_t)sf << 31) | ((uint32_t)(rm & 31) << 16) | (op << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}

void a64_lslv(A64Buf *b, int sf, int rd, int rn, int rm) { datap2(b, 0x8, sf, rd, rn, rm); }
void a64_lsrv(A64Buf *b, int sf, int rd, int rn, int rm) { datap2(b, 0x9, sf, rd, rn, rm); }
void a64_asrv(A64Buf *b, int sf, int rd, int rn, int rm) { datap2(b, 0xa, sf, rd, rn, rm); }

static void bfm(A64Buf *b, uint32_t opc, int sf, int rd, int rn, int immr, int imms)
{
    uint32_t n = sf ? 1u : 0u;
    a64_emit32(b, 0x13000000u | (opc << 29) | ((uint32_t)sf << 31) | (n << 22) | ((uint32_t)(immr & 63) << 16) | ((uint32_t)(imms & 63) << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}

void a64_ubfm(A64Buf *b, int sf, int rd, int rn, int immr, int imms) { bfm(b, 2, sf, rd, rn, immr, imms); }
void a64_sbfm(A64Buf *b, int sf, int rd, int rn, int immr, int imms) { bfm(b, 0, sf, rd, rn, immr, imms); }
void a64_bfm(A64Buf *b, int sf, int rd, int rn, int immr, int imms) { bfm(b, 1, sf, rd, rn, immr, imms); }

void a64_ubfx(A64Buf *b, int sf, int rd, int rn, int lsb, int width) { a64_ubfm(b, sf, rd, rn, lsb, lsb + width - 1); }
void a64_sbfx(A64Buf *b, int sf, int rd, int rn, int lsb, int width) { a64_sbfm(b, sf, rd, rn, lsb, lsb + width - 1); }

void a64_bfi(A64Buf *b, int sf, int rd, int rn, int lsb, int width)
{
    int bits = sf ? 64 : 32;
    a64_bfm(b, sf, rd, rn, (bits - lsb) & (bits - 1), width - 1);
}

void a64_lsl_imm(A64Buf *b, int sf, int rd, int rn, int sh)
{
    int bits = sf ? 64 : 32;
    a64_ubfm(b, sf, rd, rn, (bits - sh) & (bits - 1), bits - 1 - sh);
}

void a64_lsr_imm(A64Buf *b, int sf, int rd, int rn, int sh)
{
    int bits = sf ? 64 : 32;
    a64_ubfm(b, sf, rd, rn, sh, bits - 1);
}

void a64_asr_imm(A64Buf *b, int sf, int rd, int rn, int sh)
{
    int bits = sf ? 64 : 32;
    a64_sbfm(b, sf, rd, rn, sh, bits - 1);
}

void a64_uxtb(A64Buf *b, int rd, int rn) { a64_ubfm(b, 0, rd, rn, 0, 7); }
void a64_uxth(A64Buf *b, int rd, int rn) { a64_ubfm(b, 0, rd, rn, 0, 15); }
void a64_sxtb(A64Buf *b, int sf, int rd, int rn) { a64_sbfm(b, sf, rd, rn, 0, 7); }
void a64_sxth(A64Buf *b, int sf, int rd, int rn) { a64_sbfm(b, sf, rd, rn, 0, 15); }
void a64_sxtw(A64Buf *b, int rd, int rn) { a64_sbfm(b, 1, rd, rn, 0, 31); }

void a64_cset(A64Buf *b, int rd, int cond)
{
    int inv = A64_INV(cond);
    a64_emit32(b, 0x1a9f07e0u | ((uint32_t)(inv & 15) << 12) | (uint32_t)(rd & 31));
}

void a64_csel(A64Buf *b, int sf, int rd, int rn, int rm, int cond)
{
    a64_emit32(b, 0x1a800000u | ((uint32_t)sf << 31) | ((uint32_t)(rm & 31) << 16) | ((uint32_t)(cond & 15) << 12) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}

void a64_mul(A64Buf *b, int sf, int rd, int rn, int rm)
{
    a64_emit32(b, 0x1b007c00u | ((uint32_t)sf << 31) | ((uint32_t)(rm & 31) << 16) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}

void a64_umulh(A64Buf *b, int rd, int rn, int rm)
{
    a64_emit32(b, 0x9bc07c00u | ((uint32_t)(rm & 31) << 16) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}

void a64_smulh(A64Buf *b, int rd, int rn, int rm)
{
    a64_emit32(b, 0x9b407c00u | ((uint32_t)(rm & 31) << 16) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}

void a64_rev(A64Buf *b, int sf, int rd, int rn)
{
    uint32_t base = sf ? 0xdac00c00u : 0x5ac00800u;
    a64_emit32(b, base | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}

void a64_b(A64Buf *b, int32_t off_words)
{
    a64_emit32(b, 0x14000000u | ((uint32_t)off_words & 0x03ffffffu));
}

void a64_bcond(A64Buf *b, int cond, int32_t off_words)
{
    a64_emit32(b, 0x54000000u | (((uint32_t)off_words & 0x7ffff) << 5) | (uint32_t)(cond & 15));
}

void a64_cbz(A64Buf *b, int sf, int rt, int32_t off_words)
{
    a64_emit32(b, 0x34000000u | ((uint32_t)sf << 31) | (((uint32_t)off_words & 0x7ffff) << 5) | (uint32_t)(rt & 31));
}

void a64_cbnz(A64Buf *b, int sf, int rt, int32_t off_words)
{
    a64_emit32(b, 0x35000000u | ((uint32_t)sf << 31) | (((uint32_t)off_words & 0x7ffff) << 5) | (uint32_t)(rt & 31));
}

void a64_patch_b(uint32_t *at, uint32_t *target)
{
    int32_t off = (int32_t)(target - at);
    *at = (*at & 0xfc000000u) | ((uint32_t)off & 0x03ffffffu);
}

void a64_patch_bcond(uint32_t *at, uint32_t *target)
{
    int32_t off = (int32_t)(target - at);
    *at = (*at & 0xff00001fu) | (((uint32_t)off & 0x7ffff) << 5);
}

void a64_patch_cbz(uint32_t *at, uint32_t *target)
{
    int32_t off = (int32_t)(target - at);
    *at = (*at & 0xff00001fu) | (((uint32_t)off & 0x7ffff) << 5);
}

void a64_ret(A64Buf *b)
{
    a64_emit32(b, 0xd65f03c0u);
}

void a64_br(A64Buf *b, int rn)
{
    a64_emit32(b, 0xd61f0000u | ((uint32_t)(rn & 31) << 5));
}

void a64_blr(A64Buf *b, int rn)
{
    a64_emit32(b, 0xd63f0000u | ((uint32_t)(rn & 31) << 5));
}

static uint32_t ldstp_imm7(int imm)
{
    return (uint32_t)((imm / 8) & 0x7f);
}

void a64_stp_pre(A64Buf *b, int rt, int rt2, int rn, int imm)
{
    a64_emit32(b, 0xa9800000u | (ldstp_imm7(imm) << 15) | ((uint32_t)(rt2 & 31) << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
}

void a64_ldp_post(A64Buf *b, int rt, int rt2, int rn, int imm)
{
    a64_emit32(b, 0xa8c00000u | (ldstp_imm7(imm) << 15) | ((uint32_t)(rt2 & 31) << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
}

void a64_stp_off(A64Buf *b, int rt, int rt2, int rn, int imm)
{
    a64_emit32(b, 0xa9000000u | (ldstp_imm7(imm) << 15) | ((uint32_t)(rt2 & 31) << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
}

void a64_ldp_off(A64Buf *b, int rt, int rt2, int rn, int imm)
{
    a64_emit32(b, 0xa9400000u | (ldstp_imm7(imm) << 15) | ((uint32_t)(rt2 & 31) << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
}

/* arm64 instruction emitter: one function per instruction, each appending a word to the buffer. */
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

    if (b->p >= b->end)
        return &b->sink;
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

static uint64_t low_mask(unsigned bits)
{
    return bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
}

static uint64_t ror_element(uint64_t v, unsigned rot, unsigned bits)
{
    uint64_t mask = low_mask(bits);
    rot &= bits - 1;
    if (rot == 0)
        return v & mask;
    return ((v >> rot) | (v << (bits - rot))) & mask;
}

static int logical_imm_fields(int sf, uint64_t imm, uint32_t *fields)
{
    unsigned width = sf ? 64u : 32u;
    uint64_t width_mask = low_mask(width);
    imm &= width_mask;
    if (imm == 0 || imm == width_mask)
        return 0;

    for (unsigned esize = 2; esize <= width; esize <<= 1) {
        uint64_t emask = low_mask(esize);
        uint64_t element = imm & emask;
        uint64_t replicated = element;
        for (unsigned shift = esize; shift < width; shift <<= 1)
            replicated |= replicated << shift;
        if ((replicated & width_mask) != imm)
            continue;

        unsigned ones = (unsigned)__builtin_popcountll(element);
        if (ones == 0 || ones == esize)
            continue;
        uint64_t run = low_mask(ones);
        for (unsigned rot = 0; rot < esize; rot++) {
            if (ror_element(run, rot, esize) != element)
                continue;
            uint32_t n = esize == 64 ? 1u : 0u;
            uint32_t imms = ((~(esize * 2u - 1u)) & 0x3fu) | (ones - 1u);
            *fields = (n << 22) | ((rot & 0x3fu) << 16) | (imms << 10);
            return 1;
        }
    }
    return 0;
}

static int try_logic_imm(A64Buf *b, uint32_t base, int sf, int rd, int rn,
                         uint64_t imm)
{
    uint32_t fields;
    if (!logical_imm_fields(sf, imm, &fields))
        return 0;
    a64_emit32(b, base | ((uint32_t)(sf != 0) << 31) | fields |
                  ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
    return 1;
}

int a64_try_and_imm(A64Buf *b, int sf, int rd, int rn, uint64_t imm)
{
    return try_logic_imm(b, 0x12000000u, sf, rd, rn, imm);
}

int a64_try_ands_imm(A64Buf *b, int sf, int rd, int rn, uint64_t imm)
{
    return try_logic_imm(b, 0x72000000u, sf, rd, rn, imm);
}

int a64_try_orr_imm(A64Buf *b, int sf, int rd, int rn, uint64_t imm)
{
    return try_logic_imm(b, 0x32000000u, sf, rd, rn, imm);
}

int a64_try_eor_imm(A64Buf *b, int sf, int rd, int rn, uint64_t imm)
{
    return try_logic_imm(b, 0x52000000u, sf, rd, rn, imm);
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
    int wide_count = 4 - (zero > ones ? zero : ones);
    if (wide_count == 0)
        wide_count = 1;
    if (wide_count > 1 && a64_try_orr_imm(b, 1, rd, A64_ZR, v))
        return;
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

void a64_ldr_post64(A64Buf *b, int rt, int rn, int imm)
{
    a64_emit32(b, 0xf8400400u | (((uint32_t)imm & 0x1ffu) << 12) |
                  ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
}

void a64_str_pre64(A64Buf *b, int rt, int rn, int imm)
{
    a64_emit32(b, 0xf8000c00u | (((uint32_t)imm & 0x1ffu) << 12) |
                  ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
}

void a64_ldr_regoff(A64Buf *b, int size, int rt, int rn, int rm, int scaled)
{
    uint32_t sz = size == 8 ? 3u : size == 4 ? 2u : size == 2 ? 1u : 0u;
    a64_emit32(b, 0x38606800u | (sz << 30) |
                  ((uint32_t)(rm & 31) << 16) |
                  ((uint32_t)(scaled != 0) << 12) |
                  ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
}

void a64_str_regoff(A64Buf *b, int size, int rt, int rn, int rm, int scaled)
{
    uint32_t sz = size == 8 ? 3u : size == 4 ? 2u : size == 2 ? 1u : 0u;
    a64_emit32(b, 0x38206800u | (sz << 30) |
                  ((uint32_t)(rm & 31) << 16) |
                  ((uint32_t)(scaled != 0) << 12) |
                  ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
}

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

void a64_ldapr(A64Buf *b, int size, int rt, int rn)
{
    uint32_t sz = ldst_size_bits(size);
    a64_emit32(b, 0x38bfc000u | (sz << 30) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
}

void a64_dmb_ish(A64Buf *b)
{
    a64_emit32(b, 0xd5033bbfu);
}

void a64_ldrsb(A64Buf *b, int sf, int rt, int rn, uint32_t off)
{
    uint32_t opc = sf ? 2u : 3u;
    a64_emit32(b, 0x39000000u | (opc << 22) | (off << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
}

void a64_ldrsh(A64Buf *b, int sf, int rt, int rn, uint32_t off)
{
    uint32_t opc = sf ? 2u : 3u;
    a64_emit32(b, 0x79000000u | (opc << 22) | ((off / 2) << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
}

void a64_ldrsw(A64Buf *b, int rt, int rn, uint32_t off)
{
    a64_emit32(b, 0xb9800000u | ((off / 4) << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
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
void a64_rorv(A64Buf *b, int sf, int rd, int rn, int rm) { datap2(b, 0xb, sf, rd, rn, rm); }
void a64_udiv(A64Buf *b, int sf, int rd, int rn, int rm) { datap2(b, 0x2, sf, rd, rn, rm); }
void a64_sdiv(A64Buf *b, int sf, int rd, int rn, int rm) { datap2(b, 0x3, sf, rd, rn, rm); }
/* MSUB rd = ra - rn*rm */
void a64_msub(A64Buf *b, int sf, int rd, int rn, int rm, int ra)
{
    a64_emit32(b, 0x1b008000u | ((uint32_t)sf << 31) | ((uint32_t)(rm & 31) << 16) | ((uint32_t)(ra & 31) << 10) |
               ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}

/* EXTR rd, rn, rm, #lsb  (ROR rd, rn, #imm when rn == rm) */
void a64_extr(A64Buf *b, int sf, int rd, int rn, int rm, int lsb)
{
    uint32_t n = sf ? 1u : 0u;
    a64_emit32(b, 0x13800000u | ((uint32_t)sf << 31) | (n << 22) | ((uint32_t)(rm & 31) << 16) |
               ((uint32_t)(lsb & 63) << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}

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

/* CSETM Xd, cond  == CSINV Xd, XZR, XZR, invert(cond) */
void a64_csetm(A64Buf *b, int rd, int cond)
{
    int inv = A64_INV(cond);
    a64_emit32(b, 0xda9f03e0u | ((uint32_t)(inv & 15) << 12) | (uint32_t)(rd & 31));
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

void a64_tbz(A64Buf *b, int rt, int bit, int32_t off_words)
{
    a64_emit32(b, 0x36000000u | ((uint32_t)(bit & 0x20) << 26) |
                  ((uint32_t)(bit & 31) << 19) |
                  (((uint32_t)off_words & 0x3fff) << 5) |
                  (uint32_t)(rt & 31));
}

void a64_tbnz(A64Buf *b, int rt, int bit, int32_t off_words)
{
    a64_emit32(b, 0x37000000u | ((uint32_t)(bit & 0x20) << 26) |
                  ((uint32_t)(bit & 31) << 19) |
                  (((uint32_t)off_words & 0x3fff) << 5) |
                  (uint32_t)(rt & 31));
}

void a64_patch_b(uint32_t *at, uint32_t *target)
{
    int32_t off = (int32_t)(target - at);
    *at = (*at & 0xfc000000u) | ((uint32_t)off & 0x03ffffffu);
}

int a64_try_patch_b(uint32_t *at, uint32_t *target)
{

    ptrdiff_t off = target - at;
    if (off < -(ptrdiff_t)(1 << 25) || off > (ptrdiff_t)((1 << 25) - 1))
        return 0;
    *at = (*at & 0xfc000000u) | ((uint32_t)(int32_t)off & 0x03ffffffu);
    return 1;
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

void a64_patch_tbz(uint32_t *at, uint32_t *target)
{
    int32_t off = (int32_t)(target - at);
    *at = (*at & 0xfff8001fu) | (((uint32_t)off & 0x3fff) << 5);
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

/* ---------------- FP / SIMD (V registers) ---------------- */
/* LDR/STR Qt, [Xn, #imm] (imm scaled by 16); size 16 -> Q, 8 -> D, 4 -> S */
void a64_ldr_v(A64Buf *b, int size, int vt, int rn, uint32_t off)
{
    uint32_t base, scale;
    if (size == 16)      { base = 0x3dc00000u; scale = 16; }
    else if (size == 8)  { base = 0xfd400000u; scale = 8; }
    else                 { base = 0xbd400000u; scale = 4; }
    a64_emit32(b, base | ((off / scale) << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(vt & 31));
}
void a64_str_v(A64Buf *b, int size, int vt, int rn, uint32_t off)
{
    uint32_t base, scale;
    if (size == 16)      { base = 0x3d800000u; scale = 16; }
    else if (size == 8)  { base = 0xfd000000u; scale = 8; }
    else                 { base = 0xbd000000u; scale = 4; }
    a64_emit32(b, base | ((off / scale) << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(vt & 31));
}
/* LDUR/STUR Qt/Dt/St, [Xn, #simm9] -- unscaled, for arbitrary offsets */
void a64_ldur_v(A64Buf *b, int size, int vt, int rn, int32_t simm9)
{
    uint32_t base = size == 16 ? 0x3cc00000u : size == 8 ? 0xfc400000u : 0xbc400000u;
    a64_emit32(b, base | (((uint32_t)simm9 & 0x1ffu) << 12) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(vt & 31));
}
void a64_stur_v(A64Buf *b, int size, int vt, int rn, int32_t simm9)
{
    uint32_t base = size == 16 ? 0x3c800000u : size == 8 ? 0xfc000000u : 0xbc000000u;
    a64_emit32(b, base | (((uint32_t)simm9 & 0x1ffu) << 12) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(vt & 31));
}
/* scalar FP arithmetic: type 0 = single, 1 = double */
static void fp2(A64Buf *b, uint32_t opc, int dbl, int vd, int vn, int vm)
{
    a64_emit32(b, 0x1e200800u | ((uint32_t)dbl << 22) | ((uint32_t)(vm & 31) << 16) | (opc << 12) |
               ((uint32_t)(vn & 31) << 5) | (uint32_t)(vd & 31));
}
void a64_fadd_s(A64Buf *b, int dbl, int vd, int vn, int vm) { fp2(b, 0x2, dbl, vd, vn, vm); }
void a64_fsub_s(A64Buf *b, int dbl, int vd, int vn, int vm) { fp2(b, 0x3, dbl, vd, vn, vm); }
void a64_fmul_s(A64Buf *b, int dbl, int vd, int vn, int vm) { fp2(b, 0x0, dbl, vd, vn, vm); }
void a64_fdiv_s(A64Buf *b, int dbl, int vd, int vn, int vm) { fp2(b, 0x1, dbl, vd, vn, vm); }
void a64_fmax_s(A64Buf *b, int dbl, int vd, int vn, int vm) { fp2(b, 0x4, dbl, vd, vn, vm); }
void a64_fmin_s(A64Buf *b, int dbl, int vd, int vn, int vm) { fp2(b, 0x5, dbl, vd, vn, vm); }
void a64_fsqrt_s(A64Buf *b, int dbl, int vd, int vn)
{
    a64_emit32(b, 0x1e21c000u | ((uint32_t)dbl << 22) | ((uint32_t)(vn & 31) << 5) | (uint32_t)(vd & 31));
}
/* FMOV Dd, Dn / FMOV Sd, Sn (scalar move; zeroes the upper part of Vd) */
void a64_fmov_d_d(A64Buf *b, int vd, int vn) { a64_emit32(b, 0x1e604000u | ((uint32_t)(vn & 31) << 5) | (uint32_t)(vd & 31)); }
void a64_fmov_s_s(A64Buf *b, int vd, int vn) { a64_emit32(b, 0x1e204000u | ((uint32_t)(vn & 31) << 5) | (uint32_t)(vd & 31)); }
/* FCMP Sn/Dn, Sm/Dm  (sets NZCV) */
void a64_fcmp(A64Buf *b, int dbl, int vn, int vm)
{
    a64_emit32(b, 0x1e202000u | ((uint32_t)dbl << 22) | ((uint32_t)(vm & 31) << 16) | ((uint32_t)(vn & 31) << 5));
}
/* FCVT: double<->single  (fcvt Sd, Dn : opc=00 type=01 ; fcvt Dd, Sn : opc=01 type=00) */
void a64_fcvt_d2s(A64Buf *b, int vd, int vn) { a64_emit32(b, 0x1e624000u | ((uint32_t)(vn & 31) << 5) | (uint32_t)(vd & 31)); }
void a64_fcvt_s2d(A64Buf *b, int vd, int vn) { a64_emit32(b, 0x1e22c000u | ((uint32_t)(vn & 31) << 5) | (uint32_t)(vd & 31)); }
/* FCVTZS Xd/Wd, Sn/Dn : truncating float->int */
void a64_fcvtzs(A64Buf *b, int sf, int dbl, int rd, int vn)
{
    a64_emit32(b, 0x1e380000u | ((uint32_t)sf << 31) | ((uint32_t)dbl << 22) | ((uint32_t)(vn & 31) << 5) | (uint32_t)(rd & 31));
}
/* SCVTF Sd/Dd, Xn/Wn : int->float */
void a64_scvtf(A64Buf *b, int sf, int dbl, int vd, int rn)
{
    a64_emit32(b, 0x1e220000u | ((uint32_t)sf << 31) | ((uint32_t)dbl << 22) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(vd & 31));
}
/* FMOV Xd, Dn / FMOV Dd, Xn / FMOV Wd, Sn / FMOV Sd, Wn */
void a64_fmov_x_from_v(A64Buf *b, int sf, int rd, int vn)   /* sf=1: X<-D ; sf=0: W<-S */
{
    a64_emit32(b, (sf ? 0x9e660000u : 0x1e260000u) | ((uint32_t)(vn & 31) << 5) | (uint32_t)(rd & 31));
}
void a64_fmov_v_from_x(A64Buf *b, int sf, int vd, int rn)   /* sf=1: D<-X ; sf=0: S<-W */
{
    a64_emit32(b, (sf ? 0x9e670000u : 0x1e270000u) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(vd & 31));
}
/* Vector (128-bit) ops on Vd.16B/4S/2D */
static void v3(A64Buf *b, uint32_t base, int vd, int vn, int vm)
{
    a64_emit32(b, base | ((uint32_t)(vm & 31) << 16) | ((uint32_t)(vn & 31) << 5) | (uint32_t)(vd & 31));
}
void a64_v_fadd(A64Buf *b, int dbl, int vd, int vn, int vm) { v3(b, dbl ? 0x4e60d400u : 0x4e20d400u, vd, vn, vm); }
void a64_v_fsub(A64Buf *b, int dbl, int vd, int vn, int vm) { v3(b, dbl ? 0x4ee0d400u : 0x4ea0d400u, vd, vn, vm); }
void a64_v_fmul(A64Buf *b, int dbl, int vd, int vn, int vm) { v3(b, dbl ? 0x6e60dc00u : 0x6e20dc00u, vd, vn, vm); }
void a64_v_fdiv(A64Buf *b, int dbl, int vd, int vn, int vm) { v3(b, dbl ? 0x6e60fc00u : 0x6e20fc00u, vd, vn, vm); }
void a64_v_fmax(A64Buf *b, int dbl, int vd, int vn, int vm) { v3(b, dbl ? 0x4e60f400u : 0x4e20f400u, vd, vn, vm); }
void a64_v_fmin(A64Buf *b, int dbl, int vd, int vn, int vm) { v3(b, dbl ? 0x4ee0f400u : 0x4ea0f400u, vd, vn, vm); }
void a64_v_fsqrt(A64Buf *b, int dbl, int vd, int vn) { v3(b, dbl ? 0x6ee1f800u : 0x6ea1f800u, vd, vn, 0); }
void a64_v_and(A64Buf *b, int vd, int vn, int vm) { v3(b, 0x4e201c00u, vd, vn, vm); }
void a64_v_orr(A64Buf *b, int vd, int vn, int vm) { v3(b, 0x4ea01c00u, vd, vn, vm); }
void a64_v_eor(A64Buf *b, int vd, int vn, int vm) { v3(b, 0x6e201c00u, vd, vn, vm); }
void a64_v_bic(A64Buf *b, int vd, int vn, int vm) { v3(b, 0x4e601c00u, vd, vn, vm); }   /* vd = vn & ~vm */
void a64_v_mov(A64Buf *b, int vd, int vn) { v3(b, 0x4ea01c00u, vd, vn, vn); }            /* orr vd, vn, vn */
/* integer vector add/sub: esz 0=B 1=H 2=S 3=D */
void a64_v_add(A64Buf *b, int esz, int vd, int vn, int vm) { v3(b, 0x4e208400u | ((uint32_t)esz << 22), vd, vn, vm); }
void a64_v_sub(A64Buf *b, int esz, int vd, int vn, int vm) { v3(b, 0x6e208400u | ((uint32_t)esz << 22), vd, vn, vm); }
/* MOVI Vd.2D, #0  (zero a vector register) */
void a64_v_zero(A64Buf *b, int vd) { a64_emit32(b, 0x6f00e400u | (uint32_t)(vd & 31)); }
/* INS Vd.D[idx], Xn ; UMOV Xd, Vn.D[idx] ; INS Vd.D[i1], Vn.D[i2] */
void a64_ins_d_x(A64Buf *b, int vd, int idx, int rn) { a64_emit32(b, 0x4e081c00u | ((uint32_t)(idx & 1) << 20) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(vd & 31)); }
void a64_umov_x_d(A64Buf *b, int rd, int vn, int idx) { a64_emit32(b, 0x4e083c00u | ((uint32_t)(idx & 1) << 20) | ((uint32_t)(vn & 31) << 5) | (uint32_t)(rd & 31)); }
void a64_ins_d_d(A64Buf *b, int vd, int i1, int vn, int i2) { a64_emit32(b, 0x6e080400u | ((uint32_t)(i1 & 1) << 20) | ((uint32_t)(i2 & 1) << 14) | ((uint32_t)(vn & 31) << 5) | (uint32_t)(vd & 31)); }
void a64_ins_s_s(A64Buf *b, int vd, int i1, int vn, int i2) { a64_emit32(b, 0x6e040400u | ((uint32_t)(i1 & 3) << 19) | ((uint32_t)(i2 & 3) << 13) | ((uint32_t)(vn & 31) << 5) | (uint32_t)(vd & 31)); }
/* FCVTL Vd.2D, Vn.2S (low half) ; FCVTN Vd.2S, Vn.2D */
void a64_v_fcvtl(A64Buf *b, int vd, int vn) { v3(b, 0x0e617800u, vd, vn, 0); }
void a64_v_fcvtn(A64Buf *b, int vd, int vn) { v3(b, 0x0e616800u, vd, vn, 0); }
/* SCVTF Vd.4S, Vn.4S (int->float, vector) ; FCVTZS Vd.4S, Vn.4S */
void a64_v_scvtf_4s(A64Buf *b, int vd, int vn) { v3(b, 0x4e21d800u, vd, vn, 0); }
void a64_v_fcvtzs_4s(A64Buf *b, int vd, int vn) { v3(b, 0x4ea1b800u, vd, vn, 0); }
/* CMEQ/CMGT integer vector (esz as above) */
void a64_v_cmeq(A64Buf *b, int esz, int vd, int vn, int vm) { v3(b, 0x6e208c00u | ((uint32_t)esz << 22), vd, vn, vm); }
void a64_v_cmgt(A64Buf *b, int esz, int vd, int vn, int vm) { v3(b, 0x4e203400u | ((uint32_t)esz << 22), vd, vn, vm); }
/* DUP Vd.2D, Vn.D[idx] ; DUP Vd.4S, Vn.S[idx] */
void a64_v_dup_d(A64Buf *b, int vd, int vn, int idx) { a64_emit32(b, 0x4e080400u | ((uint32_t)(idx & 1) << 20) | ((uint32_t)(vn & 31) << 5) | (uint32_t)(vd & 31)); }
void a64_v_dup_s(A64Buf *b, int vd, int vn, int idx) { a64_emit32(b, 0x4e040400u | ((uint32_t)(idx & 3) << 19) | ((uint32_t)(vn & 31) << 5) | (uint32_t)(vd & 31)); }
/* ZIP1/ZIP2/UZP1/UZP2/TRN Vd.2D  (esz=3), Vd.4S (esz=2) */
void a64_v_zip1(A64Buf *b, int esz, int vd, int vn, int vm) { v3(b, 0x4e003800u | ((uint32_t)esz << 22), vd, vn, vm); }
void a64_v_zip2(A64Buf *b, int esz, int vd, int vn, int vm) { v3(b, 0x4e007800u | ((uint32_t)esz << 22), vd, vn, vm); }
/* BSL Vd.16B, Vn, Vm  (bitwise select: vd = (vd & vn) | (~vd & vm)) */
void a64_v_bsl(A64Buf *b, int vd, int vn, int vm) { v3(b, 0x6e601c00u, vd, vn, vm); }
/* SSHR Vd.2D/#imm ; SSHR Vd.4S ; used for sign masks (arith shift right) */
void a64_v_sshr_2d(A64Buf *b, int vd, int vn, int sh) { a64_emit32(b, 0x4f400400u | ((uint32_t)(128 - sh) << 16) | ((uint32_t)(vn & 31) << 5) | (uint32_t)(vd & 31)); }
void a64_v_sshr_4s(A64Buf *b, int vd, int vn, int sh) { a64_emit32(b, 0x4f200400u | ((uint32_t)(64 - sh) << 16) | ((uint32_t)(vn & 31) << 5) | (uint32_t)(vd & 31)); }
/* FCMEQ Vd.4S/2D, Vn, Vm  (all-ones where equal; NaN lanes -> 0) */
void a64_v_fcmeq(A64Buf *b, int dbl, int vd, int vn, int vm) { v3(b, dbl ? 0x4e60e400u : 0x4e20e400u, vd, vn, vm); }
/* UMINV Sd, Vn.4S (min across lanes) */
void a64_v_uminv_4s(A64Buf *b, int vd, int vn) { a64_emit32(b, 0x6eb1a800u | ((uint32_t)(vn & 31) << 5) | (uint32_t)(vd & 31)); }
/* BIT Vd, Vn, Vm : vd = (vd & ~vm) | (vn & vm)  -- insert vn where vm set */
void a64_v_bit(A64Buf *b, int vd, int vn, int vm) { v3(b, 0x6ea01c00u, vd, vn, vm); }
/* ORR Vd.4S, #imm8 shifted -- used for quieting: not general; use and/orr with const regs instead */

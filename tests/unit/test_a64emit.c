/* Run-don't-read validation of the arm64 emitter. */
#include "ocerz/a64emit.h"

#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static uint32_t *g_code;
static size_t g_cap = 1u << 16;

static void jit_alloc(void)
{
    g_code = mmap(NULL, g_cap, PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (g_code == MAP_FAILED) {
        perror("mmap MAP_JIT");
        exit(2);
    }
}

static A64Buf emit_begin(void)
{
    pthread_jit_write_protect_np(0);
    A64Buf b = { g_code, g_code, g_code + g_cap / 4, 0 };
    return b;
}

static void *emit_end(A64Buf *b)
{
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(g_code, (size_t)((b->p - g_code) * 4));
    if (b->overflow) {
        printf("FAIL emitter overflow\n");
        failures++;
    }
    return (void *)g_code;
}

typedef uint64_t (*fn2)(uint64_t, uint64_t);

static uint64_t run2(void (*build)(A64Buf *), uint64_t a, uint64_t bb)
{
    A64Buf buf = emit_begin();
    build(&buf);
    fn2 f = (fn2)emit_end(&buf);
    return f(a, bb);
}

static void b_movimm(A64Buf *b) { a64_mov_imm64(b, 0, 0x123456789abcdef0ull); a64_ret(b); }
static void b_movimm2(A64Buf *b) { a64_mov_imm64(b, 0, 0xffffffffffff0000ull); a64_ret(b); }
static void b_movimm3(A64Buf *b) { a64_mov_imm64(b, 0, 0x000000000000ffffull); a64_ret(b); }
static void b_movimm4(A64Buf *b) { a64_mov_imm64(b, 0, 0xffffffffffffffffull); a64_ret(b); }
static void b_movimm_logical(A64Buf *b) { a64_mov_imm64(b, 0, 0x1fffe); a64_ret(b); }

static void b_ldapr64(A64Buf *b) { a64_ldapr(b, 8, 0, 1); a64_ret(b); }
static void b_ldapr32(A64Buf *b) { a64_ldapr(b, 4, 0, 1); a64_ret(b); }
static void b_ldapr16(A64Buf *b) { a64_ldapr(b, 2, 0, 1); a64_ret(b); }
static void b_ldapr8(A64Buf *b)  { a64_ldapr(b, 1, 0, 1); a64_ret(b); }
static void b_ldar64(A64Buf *b)  { a64_ldar(b, 8, 0, 1); a64_ret(b); }
static void b_add(A64Buf *b) { a64_add_reg(b, 1, 0, 0, 1, 0); a64_ret(b); }
static void b_add32(A64Buf *b) { a64_add_reg(b, 0, 0, 0, 1, 0); a64_ret(b); }
static void b_sub(A64Buf *b) { a64_sub_reg(b, 1, 0, 0, 1, 0); a64_ret(b); }
static void b_addimm(A64Buf *b) { a64_add_imm(b, 1, 0, 0, 0x111); a64_ret(b); }
static void b_subimm(A64Buf *b) { a64_sub_imm(b, 1, 0, 0, 1); a64_ret(b); }
static void b_orr(A64Buf *b) { a64_orr_reg(b, 1, 0, 0, 1, 0); a64_ret(b); }
static void b_and(A64Buf *b) { a64_and_reg(b, 1, 0, 0, 1, 0); a64_ret(b); }
static void b_eor(A64Buf *b) { a64_eor_reg(b, 1, 0, 0, 1, 0); a64_ret(b); }
static void b_bic(A64Buf *b) { a64_bic_reg(b, 1, 0, 0, 1, 0); a64_ret(b); }
static void b_orn(A64Buf *b) { a64_orn_reg(b, 1, 0, 0, 1, 0); a64_ret(b); }
static void b_andimm(A64Buf *b) { a64_try_and_imm(b, 1, 0, 0, 0x1ffff); a64_ret(b); }
static void b_andimm32(A64Buf *b) { a64_try_and_imm(b, 0, 0, 0, 0xff00ff00); a64_ret(b); }
static void b_orrimm(A64Buf *b) { a64_try_orr_imm(b, 1, 0, 0, 0x8000000000000001ull); a64_ret(b); }
static void b_eorimm(A64Buf *b) { a64_try_eor_imm(b, 1, 0, 0, 0xaaaaaaaaaaaaaaaaull); a64_ret(b); }
static void b_andsimm_z(A64Buf *b) { a64_try_ands_imm(b, 1, 0, 0, 0xff); a64_cset(b, 0, A64_EQ); a64_ret(b); }
static void b_lslv(A64Buf *b) { a64_lslv(b, 1, 0, 0, 1); a64_ret(b); }
static void b_lsrv(A64Buf *b) { a64_lsrv(b, 1, 0, 0, 1); a64_ret(b); }
static void b_asrv(A64Buf *b) { a64_asrv(b, 1, 0, 0, 1); a64_ret(b); }
static void b_lslimm(A64Buf *b) { a64_lsl_imm(b, 1, 0, 0, 4); a64_ret(b); }
static void b_lsrimm(A64Buf *b) { a64_lsr_imm(b, 1, 0, 0, 4); a64_ret(b); }
static void b_asrimm(A64Buf *b) { a64_asr_imm(b, 1, 0, 0, 4); a64_ret(b); }
static void b_uxtb(A64Buf *b) { a64_uxtb(b, 0, 0); a64_ret(b); }
static void b_uxth(A64Buf *b) { a64_uxth(b, 0, 0); a64_ret(b); }
static void b_sxtb(A64Buf *b) { a64_sxtb(b, 1, 0, 0); a64_ret(b); }
static void b_sxth(A64Buf *b) { a64_sxth(b, 1, 0, 0); a64_ret(b); }
static void b_sxtw(A64Buf *b) { a64_sxtw(b, 0, 0); a64_ret(b); }
static void b_ubfx(A64Buf *b) { a64_ubfx(b, 1, 0, 0, 8, 8); a64_ret(b); }
static void b_sbfx(A64Buf *b) { a64_sbfx(b, 1, 0, 0, 8, 8); a64_ret(b); }
static void b_bfi(A64Buf *b) { a64_bfi(b, 1, 0, 1, 8, 8); a64_ret(b); }
static void b_mul(A64Buf *b) { a64_mul(b, 1, 0, 0, 1); a64_ret(b); }
static void b_umulh(A64Buf *b) { a64_umulh(b, 0, 0, 1); a64_ret(b); }
static void b_smulh(A64Buf *b) { a64_smulh(b, 0, 0, 1); a64_ret(b); }
static void b_csel(A64Buf *b) { a64_subs_reg(b, 1, A64_ZR, 0, 1, 0); a64_csel(b, 1, 0, 0, 1, A64_LT); a64_ret(b); }
static void b_rev64(A64Buf *b) { a64_rev(b, 1, 0, 0); a64_ret(b); }
static void b_rev32(A64Buf *b) { a64_rev(b, 0, 0, 0); a64_ret(b); }
static void b_cset(A64Buf *b) { a64_subs_reg(b, 1, A64_ZR, 0, 1, 0); a64_cset(b, 0, A64_EQ); a64_ret(b); }
static void b_adcs(A64Buf *b) { a64_adds_reg(b, 1, A64_ZR, A64_ZR, A64_ZR, 0); a64_subs_imm(b, 1, A64_ZR, A64_ZR, 0); a64_adcs_reg(b, 1, 0, 0, 1); a64_ret(b); }
static uint64_t g_scratch[16];
static void b_ldrsw(A64Buf *b) { a64_mov_imm64(b, 0, (uint64_t)(uintptr_t)g_scratch); a64_str(b, 4, 1, 0, 0); a64_ldrsw(b, 0, 0, 0); a64_ret(b); }
static void b_ldsb(A64Buf *b) { a64_mov_imm64(b, 0, (uint64_t)(uintptr_t)g_scratch); a64_str(b, 1, 1, 0, 0); a64_ldrsb(b, 1, 0, 0, 0); a64_ret(b); }
static void b_ldrh(A64Buf *b) { a64_mov_imm64(b, 0, (uint64_t)(uintptr_t)g_scratch); a64_str(b, 2, 1, 0, 2); a64_ldr(b, 2, 0, 0, 2); a64_ret(b); }
static void b_ldr_regoff(A64Buf *b) { a64_mov_imm64(b, 2, (uint64_t)(uintptr_t)g_scratch); a64_str(b, 8, 1, 2, 8 * 5); a64_mov_imm64(b, 3, 5); a64_ldr_regoff(b, 8, 0, 2, 3, 1); a64_ret(b); }
static void b_str_regoff(A64Buf *b) { a64_mov_imm64(b, 2, (uint64_t)(uintptr_t)g_scratch); a64_mov_imm64(b, 3, 5); a64_str_regoff(b, 8, 0, 2, 3, 1); a64_ldr(b, 8, 0, 2, 8 * 5); a64_ret(b); }

static void b_ldrsw_off(A64Buf *b) { a64_mov_imm64(b, 2, (uint64_t)(uintptr_t)g_scratch); a64_str(b, 4, 1, 2, 8 * 5); a64_ldrsw(b, 0, 2, 8 * 5); a64_ret(b); }
static void b_ldrsb_off(A64Buf *b) { a64_mov_imm64(b, 2, (uint64_t)(uintptr_t)g_scratch); a64_str(b, 1, 1, 2, 8 * 5); a64_ldrsb(b, 1, 0, 2, 8 * 5); a64_ret(b); }
static void b_ldrsh_off(A64Buf *b) { a64_mov_imm64(b, 2, (uint64_t)(uintptr_t)g_scratch); a64_str(b, 2, 1, 2, 8 * 5); a64_ldrsh(b, 1, 0, 2, 8 * 5); a64_ret(b); }

static void b_cset_cc_subborrow(A64Buf *b) { a64_subs_reg(b, 1, A64_ZR, 0, 1, 0); a64_cset(b, 0, A64_CC); a64_ret(b); }
static void b_cset_cs_addcarry(A64Buf *b) { a64_adds_reg(b, 1, A64_ZR, 0, 1, 0); a64_cset(b, 0, A64_CS); a64_ret(b); }
static void b_cset_vs_sub(A64Buf *b) { a64_subs_reg(b, 1, A64_ZR, 0, 1, 0); a64_cset(b, 0, A64_VS); a64_ret(b); }
static void b_cset_vs_add(A64Buf *b) { a64_adds_reg(b, 1, A64_ZR, 0, 1, 0); a64_cset(b, 0, A64_VS); a64_ret(b); }
static void b_cset_mi(A64Buf *b) { a64_subs_reg(b, 1, A64_ZR, 0, 1, 0); a64_cset(b, 0, A64_MI); a64_ret(b); }

static void b_parity_fold(A64Buf *b)
{
    a64_uxtb(b, 13, 0);
    a64_lsr_imm(b, 0, 14, 13, 4); a64_eor_reg(b, 0, 13, 13, 14, 0);
    a64_lsr_imm(b, 0, 14, 13, 2); a64_eor_reg(b, 0, 13, 13, 14, 0);
    a64_lsr_imm(b, 0, 14, 13, 1); a64_eor_reg(b, 0, 13, 13, 14, 0);
    a64_mov_imm64(b, 14, 1);
    a64_bic_reg(b, 0, 13, 14, 13, 0);
    a64_lsl_imm(b, 0, 13, 13, 2);
    a64_mov_reg(b, 1, 0, 13);
    a64_ret(b);
}

static void b_str8_off(A64Buf *b) { a64_mov_imm64(b, 2, (uint64_t)(uintptr_t)g_scratch); a64_str(b, 8, 1, 2, 8 * 5); a64_ldr(b, 8, 0, 2, 8 * 5); a64_ret(b); }
static void b_str4_zx(A64Buf *b) { a64_mov_imm64(b, 2, (uint64_t)(uintptr_t)g_scratch); a64_mov_imm64(b, 1, 0); a64_str(b, 8, 1, 2, 0); a64_add_reg(b, 0, 1, 0, A64_ZR, 0); a64_str(b, 8, 1, 2, 0); a64_ldr(b, 8, 0, 2, 0); a64_ret(b); }

static int g_last_ok;

static void b_bpatch(A64Buf *b)
{
    uint32_t *j = a64_label(b);
    a64_b(b, 0);
    a64_mov_imm64(b, 0, 111);
    a64_ret(b);
    uint32_t *tgt = a64_label(b);
    g_last_ok = a64_try_patch_b(j, tgt);
    a64_mov_imm64(b, 0, 222);
    a64_ret(b);
}

static uint64_t run_br(int reg)
{
    A64Buf b = emit_begin();
    uint32_t *entry = a64_label(&b);
    a64_mov_reg(&b, 1, reg, 0);
    a64_br(&b, reg);
    uint32_t *stub = a64_label(&b);
    a64_mov_imm64(&b, 0, 222);
    a64_ret(&b);
    emit_end(&b);
    fn2 f = (fn2)(void *)entry;
    return f((uint64_t)(uintptr_t)stub, 0);
}

static void b_stp_ldp_off_x20(A64Buf *b)
{
    a64_stp_pre(b, 20, 21, 31, -16);
    a64_mov_imm64(b, 20, (uint64_t)(uintptr_t)g_scratch);
    a64_stp_off(b, 0, 1, 20, 16);
    a64_ldp_off(b, 2, 3, 20, 16);
    a64_ldp_post(b, 20, 21, 31, 16);
    a64_add_reg(b, 1, 0, 2, 3, 0);
    a64_ret(b);
}

static void b_wform_x21(A64Buf *b)
{
    a64_stp_pre(b, 21, 22, 31, -16);
    a64_add_reg(b, 0, 21, 0, 1, 0);
    a64_mov_reg(b, 1, 0, 21);
    a64_ldp_post(b, 21, 22, 31, 16);
    a64_ret(b);
}

static void test_try_patch_b_range(void)
{
    uint32_t w;
    uint32_t *at = &w;
    const ptrdiff_t MAXP = (1 << 25) - 1;

    w = 0x14000000u;
    CHECK(a64_try_patch_b(at, at + MAXP) == 1, "try_patch_b +(2^25-1) accepted");
    CHECK((w & 0x03ffffffu) == (uint32_t)MAXP, "try_patch_b +(2^25-1) field");

    w = 0x14000000u;
    CHECK(a64_try_patch_b(at, at + (MAXP + 1)) == 0, "try_patch_b +2^25 rejected");
    CHECK(w == 0x14000000u, "try_patch_b +2^25 leaves word untouched");

    w = 0x14000000u;
    CHECK(a64_try_patch_b(at, at - (1 << 25)) == 1, "try_patch_b -2^25 accepted");
    CHECK((w & 0x03ffffffu) == ((uint32_t)(-(1 << 25)) & 0x03ffffffu), "try_patch_b -2^25 field");

    w = 0x14000000u;
    CHECK(a64_try_patch_b(at, at - (1 << 25) - 1) == 0, "try_patch_b -(2^25+1) rejected");
    CHECK(w == 0x14000000u, "try_patch_b -(2^25+1) leaves word untouched");
}

static uint64_t test_low_mask(unsigned bits)
{
    return bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
}

static uint64_t test_ror_element(uint64_t v, unsigned rot, unsigned bits)
{
    uint64_t mask = test_low_mask(bits);
    rot &= bits - 1;
    if (rot == 0)
        return v & mask;
    return ((v >> rot) | (v << (bits - rot))) & mask;
}

static uint64_t decode_logical_imm_word(int sf, uint32_t word)
{
    uint32_t n = (word >> 22) & 1;
    uint32_t immr = (word >> 16) & 0x3f;
    uint32_t imms = (word >> 10) & 0x3f;
    uint32_t marker = (n << 6) | ((~imms) & 0x3f);
    unsigned len = 0;
    for (unsigned bit = 1; bit <= 6; bit++) {
        if (marker & (1u << bit))
            len = bit;
    }
    unsigned esize = 1u << len;
    unsigned levels = esize - 1;
    uint64_t element = test_ror_element(test_low_mask((imms & levels) + 1),
                                        immr & levels, esize);
    uint64_t value = element;
    unsigned width = sf ? 64u : 32u;
    for (unsigned shift = esize; shift < width; shift <<= 1)
        value |= value << shift;
    return value & test_low_mask(width);
}

static void test_all_logical_immediates(void)
{
    uint32_t word;
    int accepted = 1;
    int decoded = 1;
    unsigned cases = 0;

    for (int sf = 0; sf <= 1; sf++) {
        unsigned width = sf ? 64u : 32u;
        for (unsigned esize = 2; esize <= width; esize <<= 1) {
            for (unsigned ones = 1; ones < esize; ones++) {
                for (unsigned rot = 0; rot < esize; rot++) {
                    uint64_t element = test_ror_element(test_low_mask(ones),
                                                        rot, esize);
                    uint64_t value = element;
                    for (unsigned shift = esize; shift < width; shift <<= 1)
                        value |= value << shift;
                    A64Buf b = { .start = &word, .p = &word, .end = &word + 1 };
                    if (!a64_try_orr_imm(&b, sf, 0, A64_ZR, value)) {
                        accepted = 0;
                    } else if (decode_logical_imm_word(sf, word) != value) {
                        decoded = 0;
                    }
                    cases++;
                }
            }
        }
    }
    CHECK(cases == 6636, "logical immediate property case count");
    CHECK(accepted, "all architectural logical immediates accepted");
    CHECK(decoded, "all emitted logical immediate fields decode correctly");
}

static void test_logical_imm_encodings(void)
{
    uint32_t words[8] = {0};
    A64Buf b = {
        .start = words,
        .p = words,
        .end = words + sizeof(words) / sizeof(words[0]),
    };

    CHECK(a64_try_and_imm(&b, 1, 3, 4, 0x1ffff) == 1,
          "and immediate accepted");
    CHECK(words[0] == 0x92404083u, "and x3,x4,#0x1ffff encoding");
    CHECK(a64_try_and_imm(&b, 0, 5, 6, 0xff00ff00) == 1,
          "and w immediate accepted");
    CHECK(words[1] == 0x12089cc5u, "and w5,w6,#0xff00ff00 encoding");
    CHECK(a64_try_ands_imm(&b, 1, 7, 8, 0x8000000000000001ull) == 1,
          "ands rotated immediate accepted");
    CHECK(words[2] == 0xf2410507u,
          "ands x7,x8,#0x8000000000000001 encoding");
    CHECK(a64_try_orr_imm(&b, 1, 9, A64_ZR, 0x1fffe) == 1,
          "orr immediate accepted");
    CHECK(words[3] == 0xb27f3fe9u, "orr x9,xzr,#0x1fffe encoding");
    CHECK(a64_try_eor_imm(&b, 1, 10, 11, 0xaaaaaaaaaaaaaaaaull) == 1,
          "eor repeated immediate accepted");
    CHECK(words[4] == 0xd201f16au,
          "eor x10,x11,#0xaaaaaaaaaaaaaaaa encoding");

    uint32_t *before = b.p;
    CHECK(a64_try_orr_imm(&b, 1, 0, 0, 0) == 0,
          "logical immediate rejects zero");
    CHECK(a64_try_orr_imm(&b, 1, 0, 0, UINT64_MAX) == 0,
          "logical immediate rejects all ones");
    CHECK(a64_try_orr_imm(&b, 1, 0, 0, 0x0123456789abcdefull) == 0,
          "logical immediate rejects irregular pattern");
    CHECK(b.p == before, "rejected logical immediates emit nothing");

    b.p = words;
    a64_mov_imm64(&b, 0, 0x1fffe);
    CHECK(b.p == words + 1, "mov logical immediate uses one word");
    CHECK(words[0] == 0xb27f3fe0u, "mov logical immediate encoding");

    b.p = words;
    a64_mov_imm64(&b, 0, 0xffff);
    CHECK(b.p == words + 1, "one-word movz stays one word");
    CHECK(words[0] == 0xd29fffe0u, "one-word movz remains preferred");
}

static void b_branch(A64Buf *b)
{
    a64_subs_reg(b, 1, A64_ZR, 0, 1, 0);
    uint32_t *j = a64_label(b);
    a64_bcond(b, A64_NE, 0);
    a64_mov_imm64(b, 0, 111);
    a64_ret(b);
    uint32_t *tgt = a64_label(b);
    a64_patch_bcond(j, tgt);
    a64_mov_imm64(b, 0, 222);
    a64_ret(b);
}

static void b_cbz(A64Buf *b)
{
    uint32_t *j = a64_label(b);
    a64_cbz(b, 1, 0, 0);
    a64_mov_imm64(b, 0, 1);
    a64_ret(b);
    uint32_t *tgt = a64_label(b);
    a64_patch_cbz(j, tgt);
    a64_mov_imm64(b, 0, 2);
    a64_ret(b);
}

static void b_tbz(A64Buf *b)
{
    uint32_t *j = a64_label(b);
    a64_tbz(b, 0, 5, 0);
    a64_mov_imm64(b, 0, 1);
    a64_ret(b);
    uint32_t *tgt = a64_label(b);
    a64_patch_tbz(j, tgt);
    a64_mov_imm64(b, 0, 2);
    a64_ret(b);
}

static void b_tbnz(A64Buf *b)
{
    uint32_t *j = a64_label(b);
    a64_tbnz(b, 0, 37, 0);
    a64_mov_imm64(b, 0, 1);
    a64_ret(b);
    uint32_t *tgt = a64_label(b);
    a64_patch_tbz(j, tgt);
    a64_mov_imm64(b, 0, 2);
    a64_ret(b);
}

static void test_ldapr(void)
{
    static uint64_t cell __attribute__((aligned(8)));
    cell = 0x1122334455667788ull;
    uint64_t a = (uint64_t)(uintptr_t)&cell;
    CHECK(run2(b_ldapr64, 0, a) == 0x1122334455667788ull, "ldapr 64 got %#llx",
          (unsigned long long)run2(b_ldapr64, 0, a));
    CHECK(run2(b_ldapr32, 0, a) == 0x55667788ull, "ldapr 32 got %#llx",
          (unsigned long long)run2(b_ldapr32, 0, a));
    CHECK(run2(b_ldapr16, 0, a) == 0x7788ull, "ldapr 16 got %#llx",
          (unsigned long long)run2(b_ldapr16, 0, a));
    CHECK(run2(b_ldapr8, 0, a) == 0x88ull, "ldapr 8 got %#llx",
          (unsigned long long)run2(b_ldapr8, 0, a));

    CHECK(run2(b_ldapr64, 0, a) == run2(b_ldar64, 0, a), "ldapr != ldar on an aligned cell");
}

int main(void)
{
    jit_alloc();

    CHECK(run2(b_movimm, 0, 0) == 0x123456789abcdef0ull, "mov_imm64 general");
    CHECK(run2(b_movimm2, 0, 0) == 0xffffffffffff0000ull, "mov_imm64 movn-friendly");
    CHECK(run2(b_movimm3, 0, 0) == 0x000000000000ffffull, "mov_imm64 single lane");
    CHECK(run2(b_movimm4, 0, 0) == 0xffffffffffffffffull, "mov_imm64 all ones");
    CHECK(run2(b_movimm_logical, 0, 0) == 0x1fffe, "mov_imm64 logical immediate");
    CHECK(run2(b_add, 5, 7) == 12, "add64");
    CHECK(run2(b_add32, 0xffffffffull, 2) == 1, "add32 zero-extends");
    CHECK(run2(b_sub, 20, 8) == 12, "sub64");
    CHECK(run2(b_addimm, 1, 0) == 1 + 0x111, "add_imm");
    CHECK(run2(b_subimm, 5, 0) == 4, "sub_imm");
    CHECK(run2(b_orr, 0xf0, 0x0f) == 0xff, "orr");
    CHECK(run2(b_and, 0xff, 0x0f) == 0x0f, "and");
    CHECK(run2(b_eor, 0xff, 0x0f) == 0xf0, "eor");
    CHECK(run2(b_bic, 0xff, 0x0f) == 0xf0, "bic");
    CHECK(run2(b_orn, 0x0f, 0x00) == 0xffffffffffffffffull, "orn");
    CHECK(run2(b_andimm, 0x123456789abcdef0ull, 0) == 0xdef0,
          "and logical immediate");
    CHECK(run2(b_andimm32, 0xffffffffffffffffull, 0) == 0xff00ff00,
          "and logical immediate w-form zero-extends");
    CHECK(run2(b_orrimm, 0x20, 0) == 0x8000000000000021ull,
          "orr rotated logical immediate");
    CHECK(run2(b_eorimm, UINT64_MAX, 0) == 0x5555555555555555ull,
          "eor repeated logical immediate");
    CHECK(run2(b_andsimm_z, 0x100, 0) == 1, "ands immediate sets Z");
    CHECK(run2(b_andsimm_z, 0x101, 0) == 0, "ands immediate clears Z");
    CHECK(run2(b_lslv, 1, 4) == 16, "lslv");
    CHECK(run2(b_lsrv, 0x100, 4) == 0x10, "lsrv");
    CHECK(run2(b_asrv, (uint64_t)-16, 2) == (uint64_t)-4, "asrv");
    CHECK(run2(b_lslimm, 1, 0) == 16, "lsl_imm");
    CHECK(run2(b_lsrimm, 0x100, 0) == 0x10, "lsr_imm");
    CHECK(run2(b_asrimm, (uint64_t)-256, 0) == (uint64_t)-16, "asr_imm");
    CHECK(run2(b_uxtb, 0x1234, 0) == 0x34, "uxtb");
    CHECK(run2(b_uxth, 0x123456, 0) == 0x3456, "uxth");
    CHECK(run2(b_sxtb, 0x80, 0) == (uint64_t)-128, "sxtb");
    CHECK(run2(b_sxth, 0x8000, 0) == (uint64_t)-32768, "sxth");
    CHECK(run2(b_sxtw, 0x80000000ull, 0) == (uint64_t)(int64_t)(int32_t)0x80000000, "sxtw");
    CHECK(run2(b_ubfx, 0xab34, 0) == 0xab, "ubfx");
    CHECK(run2(b_sbfx, 0x8034, 0) == (uint64_t)-128, "sbfx");
    CHECK(run2(b_bfi, 0x00, 0xaa) == 0xaa00, "bfi inserts byte");
    CHECK(run2(b_mul, 6, 7) == 42, "mul");
    CHECK(run2(b_umulh, 0xffffffffffffffffull, 2) == 1, "umulh");
    CHECK(run2(b_smulh, (uint64_t)-1, 2) == (uint64_t)-1, "smulh");
    CHECK(run2(b_csel, 3, 9) == 3, "csel LT picks rn");
    CHECK(run2(b_rev64, 0x0102030405060708ull, 0) == 0x0807060504030201ull, "rev64");
    CHECK(run2(b_rev32, 0x01020304ull, 0) == 0x04030201ull, "rev32 zero-extends");
    CHECK(run2(b_cset, 5, 5) == 1, "cset EQ");
    CHECK(run2(b_adcs, 7, 0) == 8, "adcs with carry");
    CHECK(run2(b_ldrsw, 0, 0xfffffff0ull) == (uint64_t)(int64_t)(int32_t)0xfffffff0, "ldrsw");
    CHECK(run2(b_ldsb, 0, 0x80) == (uint64_t)-128, "ldrsb");

    for (int i = 0; i < 16; i++)
        g_scratch[i] = 0x5a5a5a5a5a5a5a5aull;
    CHECK(run2(b_ldrsw_off, 0, 0xfffffff0ull) == (uint64_t)(int64_t)(int32_t)0xfffffff0, "ldrsw nonzero offset");
    CHECK(run2(b_ldrsb_off, 0, 0x80) == (uint64_t)-128, "ldrsb nonzero offset");
    CHECK(run2(b_ldrsh_off, 0, 0x8000) == (uint64_t)-32768, "ldrsh nonzero offset");
    CHECK(run2(b_ldrh, 0, 0xbeef) == 0xbeef, "ldrh scaled offset");
    CHECK(run2(b_ldr_regoff, 0, 0x123456789abcdef0ull) == 0x123456789abcdef0ull,
          "ldr register offset scaled");
    CHECK(run2(b_str_regoff, 0xfeedfacecafebeefull, 0) == 0xfeedfacecafebeefull,
          "str register offset scaled");
    CHECK(run2(b_branch, 1, 2) == 222, "bcond taken (NE)");
    CHECK(run2(b_branch, 1, 1) == 111, "bcond not taken (EQ)");
    CHECK(run2(b_cbz, 0, 0) == 2, "cbz taken");
    CHECK(run2(b_cbz, 5, 0) == 1, "cbz not taken");
    CHECK(run2(b_tbz, 0, 0) == 2, "tbz bit clear taken");
    CHECK(run2(b_tbz, 1ull << 5, 0) == 1, "tbz bit set not taken");
    CHECK(run2(b_tbnz, 1ull << 37, 0) == 2, "tbnz high bit set taken");
    CHECK(run2(b_tbnz, 0, 0) == 1, "tbnz high bit clear not taken");

    CHECK(run2(b_cset_cc_subborrow, 0, 1) == 1, "sub borrow -> A64_CC (x86 CF)");
    CHECK(run2(b_cset_cc_subborrow, 5, 3) == 0, "sub no-borrow -> A64_CC clear");
    CHECK(run2(b_cset_cs_addcarry, 0xffffffffffffffffull, 1) == 1, "add carry -> A64_CS (x86 CF)");
    CHECK(run2(b_cset_cs_addcarry, 1, 1) == 0, "add no-carry -> A64_CS clear");
    CHECK(run2(b_cset_vs_sub, 0x8000000000000000ull, 1) == 1, "sub signed overflow -> A64_VS");
    CHECK(run2(b_cset_vs_sub, 5, 3) == 0, "sub no overflow -> A64_VS clear");
    CHECK(run2(b_cset_vs_add, 0x7fffffffffffffffull, 1) == 1, "add signed overflow -> A64_VS");
    CHECK(run2(b_cset_mi, 1, 5) == 1, "sub negative result -> A64_MI (x86 SF)");
    CHECK(run2(b_cset_mi, 5, 1) == 0, "sub positive result -> A64_MI clear");
    CHECK(run2(b_parity_fold, 0x00, 0) == 4, "parity of 0x00 (even) -> PF set");
    CHECK(run2(b_parity_fold, 0x01, 0) == 0, "parity of 0x01 (odd) -> PF clear");
    CHECK(run2(b_parity_fold, 0xff, 0) == 4, "parity of 0xff (even) -> PF set");
    CHECK(run2(b_parity_fold, 0x07, 0) == 0, "parity of 0x07 (odd) -> PF clear");
    CHECK(run2(b_parity_fold, 0x1234, 0) == 0, "parity reads low byte only (0x34 odd)");
    CHECK(run2(b_str8_off, 0, 0xdeadbeefcafef00dull) == 0xdeadbeefcafef00dull, "str/ldr 8 scaled offset");
    CHECK(run2(b_str4_zx, 0xffffffffull, 0) == 0xffffffffull, "w-op zero-extends, full slot stored");

    CHECK(run2(b_bpatch, 0, 0) == 222, "a64_try_patch_b in-range patch executes");
    CHECK(g_last_ok == 1, "a64_try_patch_b in-range reported success");
    test_try_patch_b_range();
    test_logical_imm_encodings();
    test_ldapr();
    test_all_logical_immediates();
    CHECK(run_br(16) == 222, "a64_br x16");
    CHECK(run_br(9) == 222, "a64_br x9");
    CHECK(run2(b_stp_ldp_off_x20, 5, 7) == 12, "stp/ldp off x20");
    CHECK(run2(b_wform_x21, 0xffffffffull, 2) == 1, "w-form add writes x21, zero-extends");

    if (failures == 0)
        printf("test_a64emit: all encodings validated\n");
    else
        printf("test_a64emit: %d FAILURES\n", failures);
    return failures != 0;
}

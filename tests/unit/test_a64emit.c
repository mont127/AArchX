/*
 * tests/unit/test_a64emit.c
 *
 * Run-don't-read validation of the arm64 emitter. Each test emits a short
 * sequence into a MAP_JIT buffer, publishes it (W^X toggle plus instruction
 * cache invalidation), then CALLS it as a native function and checks the
 * result. An encoding bug therefore cannot pass: a wrong opcode either
 * computes the wrong value, branches wrong, or faults. The harness builds
 * each function with a uniform prologue/epilogue (the emitter's stp/ldp
 * pair) only where a call is needed; most tests are leaf sequences ending
 * in RET that take up to two uint64 arguments in x0/x1 and return x0.
 *
 * jit_alloc grabs one MAP_JIT page; emit_begin/emit_end wrap the W^X
 * toggle and sys_icache_invalidate around emission. CHECK records failures
 * without aborting so one run reports every broken encoding at once.
 */
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

/* The flag-emission building blocks the JIT relies on. cset after subs/adds
 * must map arm64 NZCV exactly the way the x86-flag translation depends on:
 * SUB borrow is carry-CLEAR (A64_CC), ADD carry is carry-SET (A64_CS), signed
 * overflow is A64_VS, and the sign/zero come from MI/EQ. */
static void b_cset_cc_subborrow(A64Buf *b) { a64_subs_reg(b, 1, A64_ZR, 0, 1, 0); a64_cset(b, 0, A64_CC); a64_ret(b); }
static void b_cset_cs_addcarry(A64Buf *b) { a64_adds_reg(b, 1, A64_ZR, 0, 1, 0); a64_cset(b, 0, A64_CS); a64_ret(b); }
static void b_cset_vs_sub(A64Buf *b) { a64_subs_reg(b, 1, A64_ZR, 0, 1, 0); a64_cset(b, 0, A64_VS); a64_ret(b); }
static void b_cset_vs_add(A64Buf *b) { a64_adds_reg(b, 1, A64_ZR, 0, 1, 0); a64_cset(b, 0, A64_VS); a64_ret(b); }
static void b_cset_mi(A64Buf *b) { a64_subs_reg(b, 1, A64_ZR, 0, 1, 0); a64_cset(b, 0, A64_MI); a64_ret(b); }

/* The x86 parity fold: reduce the low byte of x0 to its even-parity bit at
 * position 2, exactly as emit_pf does (XOR-fold, then (~b)&1 via bic against a
 * materialized 1). Returns the PF bit value (0 or 4). */
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

/* Sized stores at scaled offsets, the load/store shape the inlined ALU path
 * uses against the register file (str of a full 8-byte slot after a 32-bit
 * zero-extending op). */
static void b_str8_off(A64Buf *b) { a64_mov_imm64(b, 2, (uint64_t)(uintptr_t)g_scratch); a64_str(b, 8, 1, 2, 8 * 5); a64_ldr(b, 8, 0, 2, 8 * 5); a64_ret(b); }
static void b_str4_zx(A64Buf *b) { a64_mov_imm64(b, 2, (uint64_t)(uintptr_t)g_scratch); a64_mov_imm64(b, 1, 0); a64_str(b, 8, 1, 2, 0); a64_add_reg(b, 0, 1, 0, A64_ZR, 0); a64_str(b, 8, 1, 2, 0); a64_ldr(b, 8, 0, 2, 0); a64_ret(b); }

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

int main(void)
{
    jit_alloc();

    CHECK(run2(b_movimm, 0, 0) == 0x123456789abcdef0ull, "mov_imm64 general");
    CHECK(run2(b_movimm2, 0, 0) == 0xffffffffffff0000ull, "mov_imm64 movn-friendly");
    CHECK(run2(b_movimm3, 0, 0) == 0x000000000000ffffull, "mov_imm64 single lane");
    CHECK(run2(b_movimm4, 0, 0) == 0xffffffffffffffffull, "mov_imm64 all ones");
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
    CHECK(run2(b_ldrh, 0, 0xbeef) == 0xbeef, "ldrh scaled offset");
    CHECK(run2(b_branch, 1, 2) == 222, "bcond taken (NE)");
    CHECK(run2(b_branch, 1, 1) == 111, "bcond not taken (EQ)");
    CHECK(run2(b_cbz, 0, 0) == 2, "cbz taken");
    CHECK(run2(b_cbz, 5, 0) == 1, "cbz not taken");

    /* x86-flag translation building blocks used by the JIT inlined ALU. */
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

    if (failures == 0)
        printf("test_a64emit: all encodings validated\n");
    else
        printf("test_a64emit: %d FAILURES\n", failures);
    return failures != 0;
}

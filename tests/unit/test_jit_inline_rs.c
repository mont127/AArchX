/*
 * The jit_internal.h static inline helpers, checked against the Rust copies
 * in rust/src/jit_internal.rs.
 *
 * Each check calls the C inline compiled into this file and the
 * ocerz_rs_t_* wrapper that calls the Rust port on the same inputs, over
 * edge values (0, 1, sign bits, all-ones, alignment boundaries) and a few
 * thousand LCG-random inputs, and asserts equality. Helpers that consult
 * shared translator state (g_pin, g_xmm_pinned, g_pin_class/g_n_pinned,
 * g_plain_mem) are run with that state set from here, so both sides read the
 * same words. mark_has/mark_add run on two parallel MarkSets and the whole
 * set is compared at the end. patch_any_branch/patch_guard_skip get paired
 * site/target arrays and the patched words are compared.
 *
 * A FAIL here means the Rust helper and the C inline disagree: one of them
 * is wrong, and since no translator code calls the Rust copies yet the test
 * is the only thing standing between this file and eight JIT porters
 * inheriting a bad translation.
 */
#include "ocerz/jit_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t ocerz_rs_t_jit_key(uint64_t, int);
uint64_t ocerz_rs_t_jit_key_rip(uint64_t);
int ocerz_rs_t_jit_key_mode32(uint64_t);
unsigned ocerz_rs_t_hash_key(uint64_t);
unsigned ocerz_rs_t_psc_col(uint64_t);
uint64_t ocerz_rs_t_tc_key(uint64_t, int);
int ocerz_rs_t_a64_word_may_write_reg(uint32_t, unsigned);
int ocerz_rs_t_pin_hreg(int);
int ocerz_rs_t_pin_slot(unsigned);
int ocerz_rs_t_body_edge_pin_class(void);
int ocerz_rs_t_xmm_vreg(unsigned);
int ocerz_rs_t_xmm_is_pinned(unsigned);
int ocerz_rs_t_mark_has(MarkSet *, uint64_t);
void ocerz_rs_t_mark_add(MarkSet *, uint64_t);
void ocerz_rs_t_patch_guard_skip(uint32_t *, uint32_t *);
void ocerz_rs_t_patch_any_branch(uint32_t *, uint32_t *);

static int nchecks, nfails;
#define CHECK(c, ...) do { \
    nchecks++; \
    if (!(c)) { nfails++; \
        fprintf(stderr, "FAIL %s:%d: " #c " -- ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } \
} while (0)

static uint64_t lcg(uint64_t *s)
{
    *s = *s * 6364136223846793005ull + 1442695040888963407ull;
    return *s >> 11;
}

static const uint64_t edges[] = {
    0, 1, 2, 3, 4, 0x1000, 0xffff, 0x10000, 0x7fffffff,
    0x80000000ull, 0xffffffffull, 0x100000000ull,
    0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffffffffffffull,
    0x00007fffffe00000ull, 0x00007fffffff0000ull, 0xdeadbeefcafebabeull,
};
static const uint32_t words[] = {
    0, 1, 0x34000000u, 0x35ffffffu, 0x36000000u, 0x37ffffffu,
    0x54000000u, 0x54000010u, 0x54000011u, 0x14000000u, 0x17ffffffu,
    0x94000000u, 0x28000000u, 0x28400000u, 0x08000000u, 0xbc410000u,
    0xffffffffu, 0x7e000000u, 0xff000010u,
};

int main(void)
{
    uint64_t s = 0x9e3779b97f4a7c15ull;
    int8_t pins[16];
    MarkSet mc, mr;

    for (int i = 0; i < 16; i++) pins[i] = (int8_t)(i % 7);
    g_pin = pins;
    g_pin_class = 0;
    g_n_pinned = 3;
    g_xmm_pinned = 0x5a5a;
    g_plain_mem = 0;

    /* pure-argument helpers over edges + random inputs */
    for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]) + 2000; i++) {
        uint64_t v = i < sizeof(edges) / sizeof(edges[0]) ? edges[i] : lcg(&s);
        int m32 = (int)(lcg(&s) & 1);
        unsigned r = (unsigned)(lcg(&s) % 32);
        unsigned x = r % 20;
        uint32_t w = (uint32_t)lcg(&s);

        CHECK(jit_key(v, m32) == ocerz_rs_t_jit_key(v, m32),
              "jit_key(%#llx,%d)", (unsigned long long)v, m32);
        CHECK(jit_key_rip(v) == ocerz_rs_t_jit_key_rip(v),
              "jit_key_rip(%#llx)", (unsigned long long)v);
        CHECK(jit_key_mode32(v) == ocerz_rs_t_jit_key_mode32(v),
              "jit_key_mode32(%#llx)", (unsigned long long)v);
        CHECK(hash_key(v) == ocerz_rs_t_hash_key(v),
              "hash_key(%#llx)", (unsigned long long)v);
        CHECK(psc_col(v) == ocerz_rs_t_psc_col(v),
              "psc_col(%#llx)", (unsigned long long)v);
        CHECK(tc_key(v, m32) == ocerz_rs_t_tc_key(v, m32),
              "tc_key(%#llx,%d)", (unsigned long long)v, m32);
        CHECK(a64_word_may_write_reg(w, r) == ocerz_rs_t_a64_word_may_write_reg(w, r),
              "a64_word_may_write_reg(%#x,%u)", w, r);
        CHECK(pin_hreg((int)r - 8) == ocerz_rs_t_pin_hreg((int)r - 8),
              "pin_hreg(%d)", (int)r - 8);
        CHECK(pin_slot(x) == ocerz_rs_t_pin_slot(x),
              "pin_slot(%u)", x);
        CHECK(xmm_vreg(x) == ocerz_rs_t_xmm_vreg(x),
              "xmm_vreg(%u)", x);
        CHECK(xmm_is_pinned(x) == ocerz_rs_t_xmm_is_pinned(x),
              "xmm_is_pinned(%u)", x);
    }

    /* tc_key also depends on g_plain_mem */
    g_plain_mem = 1;
    for (int i = 0; i < 64; i++) {
        uint64_t v = lcg(&s);
        CHECK(tc_key(v, 1) == ocerz_rs_t_tc_key(v, 1),
              "tc_key plain (%#llx)", (unsigned long long)v);
    }
    g_plain_mem = 0;

    /* body_edge_pin_class over its state space */
    for (int cls = -1; cls <= 2; cls++)
        for (int np = 0; np <= 2; np++) {
            g_pin_class = cls; g_n_pinned = np;
            CHECK(body_edge_pin_class() == ocerz_rs_t_body_edge_pin_class(),
                  "body_edge_pin_class cls=%d np=%d", cls, np);
        }
    g_pin_class = 0; g_n_pinned = 3;

    /* mark_has/mark_add on parallel sets */
    memset(&mc, 0, sizeof(mc));
    memset(&mr, 0, sizeof(mr));
    for (int i = 0; i < 4 * LOWHOIST_N + 100; i++) {
        uint64_t v = i % 3 ? lcg(&s) & ~1ull : (uint64_t)(i * 8) + 0x1000;
        mark_add(&mc, v);
        ocerz_rs_t_mark_add(&mr, v);
        CHECK(mark_has(&mc, v) == ocerz_rs_t_mark_has(&mr, v),
              "mark_has i=%d %#llx", i, (unsigned long long)v);
    }
    for (int i = 0; i < 512; i++) {
        uint64_t q = lcg(&s);
        CHECK(mark_has(&mc, q) == ocerz_rs_t_mark_has(&mr, q),
              "mark_has query %#llx", (unsigned long long)q);
    }
    CHECK(mc.full == mr.full, "MarkSet.full %d vs %d", mc.full, mr.full);
    CHECK(!memcmp(mc.off, mr.off, sizeof(mc.off)), "MarkSet.off differs");

    /* patch_any_branch + patch_guard_skip encode the site->target distance,
       so both sides must patch the same address: C patches, the word is
       recorded, the site is restored, and Rust patches it again. */
    {
        uint32_t sc[64], t[64];
        for (int i = 0; i < 64; i++) {
            sc[i] = i < 19 ? words[i] : (uint32_t)lcg(&s);
            t[i] = 0x14000000u + (uint32_t)i;
        }
        for (int i = 0; i < 64; i++) {
            uint32_t orig = sc[i], wc, wr;
            patch_any_branch(&sc[i], &t[i]);
            wc = sc[i];
            sc[i] = orig;
            ocerz_rs_t_patch_any_branch(&sc[i], &t[i]);
            wr = sc[i];
            CHECK(wc == wr, "patch_any_branch word %#x -> %#x vs %#x",
                  orig, wc, wr);
            patch_guard_skip(&sc[i], &t[i]);
            wc = sc[i];
            sc[i] = orig;
            ocerz_rs_t_patch_guard_skip(&sc[i], &t[i]);
            wr = sc[i];
            CHECK(wc == wr, "patch_guard_skip word %#x -> %#x vs %#x",
                  orig, wc, wr);
        }
        patch_guard_skip(NULL, &t[0]);
        ocerz_rs_t_patch_guard_skip(NULL, &t[0]);
    }

    fprintf(stderr, "test_jit_inline_rs: %d checks, %d failed\n", nchecks, nfails);
    printf("test_jit_inline_rs: %d checks, %d failed\n", nchecks, nfails);
    return nfails ? 1 : 0;
}

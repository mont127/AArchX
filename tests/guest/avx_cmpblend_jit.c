#include "gsys.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

static u8 IN[96] __attribute__((aligned(16)));
static u8 R[96] __attribute__((aligned(16)));

static void wr64(u8 *p, u64 v) { for (int i = 0; i < 8; i++) p[i] = (u8)(v >> (8 * i)); }

static void init(void)
{
    static const u64 v[12] = {
        0x3ff8000000000000ull, 0x8000000000000000ull,
        0x3fe0000000000000ull, 0x4000000000000000ull,
        0x7ff8000000000123ull, 0xbff0000000000000ull,
        0xc008000000000000ull, 0x7ff0000000000000ull,
        0x0000000000000000ull, 0x8010000000000000ull,
        0x3ff8000000000000ull, 0x4010000000000000ull,
    };
    for (int i = 0; i < 12; i++) wr64(IN + 8 * i, v[i]);
}

static void hexline(const char *name, int k, const u8 *p)
{
    static const char hx[] = "0123456789abcdef";
    char buf[48];
    int n = 0;
    g_puts(name);
    buf[n++] = ' '; buf[n++] = (char)('1' + k); buf[n++] = ':'; buf[n++] = ' ';
    for (int i = 0; i < 16; i++) { buf[n++] = hx[p[i] >> 4]; buf[n++] = hx[p[i] & 15]; }
    buf[n++] = '\n';
    sys_write(1, buf, (g_u64)n);
}

#define LOAD "vmovupd (%0), %%xmm1\n\tvmovupd 16(%0), %%xmm2\n\tvmovupd 32(%0), %%xmm3\n\t" \
             "vmovupd 48(%0), %%xmm4\n\tvmovupd 64(%0), %%xmm5\n\tvmovupd 80(%0), %%xmm6\n\t"
#define STORE "vmovupd %%xmm1, (%1)\n\tvmovupd %%xmm2, 16(%1)\n\tvmovupd %%xmm3, 32(%1)\n\t" \
              "vmovupd %%xmm4, 48(%1)\n\tvmovupd %%xmm5, 64(%1)\n\tvmovupd %%xmm6, 80(%1)\n\t"
#define PAIR(T, B, P, M, CV, CS, D, S1, S2) \
    "vcmp" P T " %%xmm" #CS ", %%xmm" #CV ", %%xmm" #M "\n\t" \
    "vblendv" B " %%xmm" #M ", %%xmm" #S2 ", %%xmm" #S1 ", %%xmm" #D "\n\t"
#define CLOB "memory", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6"

#define CASE(name, T, B, P, M, CV, CS, D, S1, S2) \
__attribute__((noinline)) static void name##_line(const u8 *in, u8 *out) \
{ \
    __asm__ volatile(LOAD PAIR(T, B, P, M, CV, CS, D, S1, S2) STORE :: "r"(in), "r"(out) : CLOB); \
} \
__attribute__((noinline)) static void name##_loop(const u8 *in, u8 *out, int n) \
{ \
    for (int i = 0; i < n; i++) \
        __asm__ volatile(LOAD PAIR(T, B, P, M, CV, CS, D, S1, S2) PAIR(T, B, P, M, CV, CS, D, S1, S2) STORE \
                         :: "r"(in), "r"(out) : CLOB); \
}

#define SHAPES(T, B, P, tag) \
    CASE(tag##_a, T, B, P, 1, 2, 3, 4, 5, 6) \
    CASE(tag##_b, T, B, P, 1, 2, 3, 1, 5, 6) \
    CASE(tag##_c, T, B, P, 1, 2, 3, 5, 5, 6) \
    CASE(tag##_d, T, B, P, 1, 2, 3, 6, 5, 6) \
    CASE(tag##_e, T, B, P, 1, 4, 3, 4, 5, 6) \
    CASE(tag##_f, T, B, P, 2, 2, 3, 4, 5, 6) \
    CASE(tag##_g, T, B, P, 2, 2, 3, 2, 5, 6) \
    CASE(tag##_h, T, B, P, 1, 2, 4, 4, 5, 6) \
    CASE(tag##_i, T, B, P, 4, 2, 4, 4, 5, 6) \
    CASE(tag##_j, T, B, P, 1, 2, 3, 4, 6, 6) \
    CASE(tag##_k, T, B, P, 1, 2, 3, 4, 3, 2) \
    CASE(tag##_l, T, B, P, 1, 2, 3, 4, 5, 1) \
    CASE(tag##_m, T, B, P, 1, 2, 3, 4, 1, 6)

#define PREDS(T, B, tag) \
    CASE(tag##_eq, T, B, "eq", 1, 2, 3, 4, 5, 6) \
    CASE(tag##_le, T, B, "le", 1, 2, 3, 4, 5, 6) \
    CASE(tag##_unord, T, B, "unord", 1, 2, 3, 4, 5, 6) \
    CASE(tag##_neq, T, B, "neq", 1, 2, 3, 4, 5, 6) \
    CASE(tag##_nlt, T, B, "nlt", 1, 2, 3, 4, 5, 6) \
    CASE(tag##_ord, T, B, "ord", 1, 2, 3, 4, 5, 6) \
    CASE(tag##_equq, T, B, "eq_uq", 1, 2, 3, 4, 5, 6) \
    CASE(tag##_gt, T, B, "gt", 1, 2, 3, 4, 5, 6)

SHAPES("sd", "pd", "lt", dlt)
SHAPES("sd", "pd", "nle", dnle)
SHAPES("ss", "ps", "lt", slt)
SHAPES("ss", "ps", "nle", snle)
PREDS("sd", "pd", dp)
PREDS("ss", "ps", sp)

#define RUN(name) do { \
    name##_line(IN, R); for (int k = 0; k < 6; k++) hexline(#name "_line", k, R + 16 * k); \
    name##_loop(IN, R, 8); for (int k = 0; k < 6; k++) hexline(#name "_loop", k, R + 16 * k); \
} while (0)
#define RUN_SHAPES(tag) RUN(tag##_a); RUN(tag##_b); RUN(tag##_c); RUN(tag##_d); RUN(tag##_e); RUN(tag##_f); RUN(tag##_g); \
    RUN(tag##_h); RUN(tag##_i); RUN(tag##_j); RUN(tag##_k); RUN(tag##_l); RUN(tag##_m)
#define RUN_PREDS(tag) RUN(tag##_eq); RUN(tag##_le); RUN(tag##_unord); RUN(tag##_neq); RUN(tag##_nlt); RUN(tag##_ord); \
    RUN(tag##_equq); RUN(tag##_gt)

int main(void)
{
    init();
    RUN_SHAPES(dlt); RUN_SHAPES(dnle); RUN_SHAPES(slt); RUN_SHAPES(snle);
    RUN_PREDS(dp); RUN_PREDS(sp);
    g_puts("DONE\n");
    return 0;
}

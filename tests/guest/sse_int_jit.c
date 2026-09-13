#include "gsys.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;

#define MAXCHK 128
static const char *cname[MAXCHK];
static int cfail[MAXCHK], coff[MAXCHK], ncheck, cur, fails;
static u8 cgot[MAXCHK], cwant[MAXCHK];

static void chk(const char *name, const u8 *got, const u8 *want, int n)
{
    int i = cur++;
    if (i >= MAXCHK) return;
    cname[i] = name;
    if (cur > ncheck) ncheck = cur;
    for (int k = 0; k < n; k++)
        if (got[k] != want[k]) {
            if (!cfail[i]) { coff[i] = k; cgot[i] = got[k]; cwant[i] = want[k]; }
            cfail[i]++;
            return;
        }
}

static u8 VA[64] __attribute__((aligned(64)));
static u8 VB[64] __attribute__((aligned(64)));
static u8 R[128] __attribute__((aligned(64)));
static u8 W[128] __attribute__((aligned(64)));
static u64 rs = 0x13198a2e03707344ull;
static u8 rnd(void) { rs = rs * 6364136223846793005ull + 1442695040888963407ull; return (u8)((rs >> 57) ^ (rs >> 29)); }

static void init_data(void)
{
    for (int i = 0; i < 64; i++) { VA[i] = rnd(); VB[i] = rnd(); }
    for (int i = 0; i < 64; i += 3) VB[i] = VA[i];
    VA[0] = 0x7f; VB[0] = 0x01; VA[1] = 0x80; VB[1] = 0x80; VA[4] = 0xff; VB[4] = 0xff; VA[5] = 0x00; VB[5] = 0x01;
    VA[18] = 0x80; VB[18] = 0x7f; VA[19] = 0x7f; VB[19] = 0x80;
    for (int i = 8; i < 16; i++) VB[i] = VA[i];
}
static void fill(u8 *p, int n, u8 v) { for (int i = 0; i < n; i++) p[i] = v; }

static u64 rd(const u8 *p, int n) { u64 v = 0; for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static void wr(u8 *p, int n, u64 v) { for (int i = 0; i < n; i++) { p[i] = (u8)v; v >>= 8; } }
static i64 sx(u64 v, int n) { return n == 8 ? (i64)v : (i64)(v << (64 - 8 * n)) >> (64 - 8 * n); }

enum { K_XOR = 1, K_AND, K_OR, K_ANDN, K_ADD, K_SUB, K_EQ, K_GT, K_UMIN, K_UMAX, K_SMIN, K_SMAX,
       K_MUL, K_UQADD, K_UQSUB, K_SQADD, K_SQSUB, K_AVG };
static u64 ref_el(int k, int n, u64 a, u64 b)
{
    u64 m = n == 8 ? ~0ull : (1ull << (8 * n)) - 1;
    i64 sa = sx(a, n), sb = sx(b, n), hi = (i64)(m >> 1), lo = -hi - 1, t;
    switch (k) {
    case K_XOR: return a ^ b;
    case K_AND: return a & b;
    case K_OR: return a | b;
    case K_ANDN: return ~a & b & m;
    case K_ADD: return (a + b) & m;
    case K_SUB: return (a - b) & m;
    case K_EQ: return a == b ? m : 0;
    case K_GT: return sa > sb ? m : 0;
    case K_UMIN: return a < b ? a : b;
    case K_UMAX: return a > b ? a : b;
    case K_SMIN: return sa < sb ? a : b;
    case K_SMAX: return sa > sb ? a : b;
    case K_MUL: return (a * b) & m;
    case K_UQADD: return a + b > m ? m : a + b;
    case K_UQSUB: return a > b ? a - b : 0;
    case K_SQADD: t = sa + sb; return (u64)(t > hi ? hi : t < lo ? lo : t) & m;
    case K_SQSUB: t = sa - sb; return (u64)(t > hi ? hi : t < lo ? lo : t) & m;
    default: return (a + b + 1) >> 1;
    }
}
static void ref_bin(int k, int n, const u8 *a, const u8 *b, u8 *o)
{
    for (int i = 0; i < 16; i += n) wr(o + i, n, ref_el(k, n, rd(a + i, n), rd(b + i, n)));
}

#define BIN(fn, mn) \
__attribute__((noinline)) static void fn(u8 *r) \
{ \
    __asm__ volatile("movdqu (%0), %%xmm1\n\tmovdqu (%1), %%xmm2\n\t" mn " %%xmm2, %%xmm1\n\tmovdqu %%xmm1, (%2)\n\t" \
                     "movdqu (%0), %%xmm3\n\t" mn " (%1), %%xmm3\n\tmovdqu %%xmm3, 16(%2)\n\t" \
                     "movdqu (%0), %%xmm4\n\t" mn " %%xmm4, %%xmm4\n\tmovdqu %%xmm4, 32(%2)\n\t" \
                     "movdqu 16(%0), %%xmm9\n\tmovdqu 16(%1), %%xmm12\n\t" mn " %%xmm12, %%xmm9\n\tmovdqu %%xmm9, 48(%2)\n\t" \
                     "movdqu 32(%1), %%xmm13\n\tmovdqu 32(%0), %%xmm5\n\t" mn " %%xmm5, %%xmm13\n\tmovdqu %%xmm13, 64(%2)" \
                     :: "r"(VA), "r"(VB), "r"(r) : "memory", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm9", "xmm12", "xmm13"); \
}
BIN(b_pxor, "pxor") BIN(b_pand, "pand") BIN(b_por, "por") BIN(b_pandn, "pandn")
BIN(b_paddb, "paddb") BIN(b_paddw, "paddw") BIN(b_paddd, "paddd") BIN(b_paddq, "paddq")
BIN(b_psubb, "psubb") BIN(b_psubw, "psubw") BIN(b_psubd, "psubd") BIN(b_psubq, "psubq")
BIN(b_pcmpeqb, "pcmpeqb") BIN(b_pcmpeqw, "pcmpeqw") BIN(b_pcmpeqd, "pcmpeqd") BIN(b_pcmpeqq, "pcmpeqq")
BIN(b_pcmpgtb, "pcmpgtb") BIN(b_pcmpgtw, "pcmpgtw") BIN(b_pcmpgtd, "pcmpgtd") BIN(b_pcmpgtq, "pcmpgtq")
BIN(b_pminub, "pminub") BIN(b_pminuw, "pminuw") BIN(b_pminud, "pminud")
BIN(b_pmaxub, "pmaxub") BIN(b_pmaxuw, "pmaxuw") BIN(b_pmaxud, "pmaxud")
BIN(b_pminsb, "pminsb") BIN(b_pminsw, "pminsw") BIN(b_pminsd, "pminsd")
BIN(b_pmaxsb, "pmaxsb") BIN(b_pmaxsw, "pmaxsw") BIN(b_pmaxsd, "pmaxsd")
BIN(b_pmullw, "pmullw") BIN(b_pmulld, "pmulld")
BIN(b_paddusb, "paddusb") BIN(b_paddusw, "paddusw") BIN(b_psubusb, "psubusb") BIN(b_psubusw, "psubusw")
BIN(b_paddsb, "paddsb") BIN(b_paddsw, "paddsw") BIN(b_psubsb, "psubsb") BIN(b_psubsw, "psubsw")
BIN(b_pavgb, "pavgb") BIN(b_pavgw, "pavgw")

typedef void (*tfn)(u8 *);
static const struct { const char *name; tfn fn; u8 kind, n; } bins[] = {
    { "pxor", b_pxor, K_XOR, 1 }, { "pand", b_pand, K_AND, 1 }, { "por", b_por, K_OR, 1 }, { "pandn", b_pandn, K_ANDN, 1 },
    { "paddb", b_paddb, K_ADD, 1 }, { "paddw", b_paddw, K_ADD, 2 }, { "paddd", b_paddd, K_ADD, 4 }, { "paddq", b_paddq, K_ADD, 8 },
    { "psubb", b_psubb, K_SUB, 1 }, { "psubw", b_psubw, K_SUB, 2 }, { "psubd", b_psubd, K_SUB, 4 }, { "psubq", b_psubq, K_SUB, 8 },
    { "pcmpeqb", b_pcmpeqb, K_EQ, 1 }, { "pcmpeqw", b_pcmpeqw, K_EQ, 2 }, { "pcmpeqd", b_pcmpeqd, K_EQ, 4 }, { "pcmpeqq", b_pcmpeqq, K_EQ, 8 },
    { "pcmpgtb", b_pcmpgtb, K_GT, 1 }, { "pcmpgtw", b_pcmpgtw, K_GT, 2 }, { "pcmpgtd", b_pcmpgtd, K_GT, 4 }, { "pcmpgtq", b_pcmpgtq, K_GT, 8 },
    { "pminub", b_pminub, K_UMIN, 1 }, { "pminuw", b_pminuw, K_UMIN, 2 }, { "pminud", b_pminud, K_UMIN, 4 },
    { "pmaxub", b_pmaxub, K_UMAX, 1 }, { "pmaxuw", b_pmaxuw, K_UMAX, 2 }, { "pmaxud", b_pmaxud, K_UMAX, 4 },
    { "pminsb", b_pminsb, K_SMIN, 1 }, { "pminsw", b_pminsw, K_SMIN, 2 }, { "pminsd", b_pminsd, K_SMIN, 4 },
    { "pmaxsb", b_pmaxsb, K_SMAX, 1 }, { "pmaxsw", b_pmaxsw, K_SMAX, 2 }, { "pmaxsd", b_pmaxsd, K_SMAX, 4 },
    { "pmullw", b_pmullw, K_MUL, 2 }, { "pmulld", b_pmulld, K_MUL, 4 },
    { "paddusb", b_paddusb, K_UQADD, 1 }, { "paddusw", b_paddusw, K_UQADD, 2 },
    { "psubusb", b_psubusb, K_UQSUB, 1 }, { "psubusw", b_psubusw, K_UQSUB, 2 },
    { "paddsb", b_paddsb, K_SQADD, 1 }, { "paddsw", b_paddsw, K_SQADD, 2 },
    { "psubsb", b_psubsb, K_SQSUB, 1 }, { "psubsw", b_psubsw, K_SQSUB, 2 },
    { "pavgb", b_pavgb, K_AVG, 1 }, { "pavgw", b_pavgw, K_AVG, 2 },
};
static void run_bins(void)
{
    for (unsigned t = 0; t < sizeof bins / sizeof bins[0]; t++) {
        int k = bins[t].kind, n = bins[t].n;
        fill(R, 80, 0xee);
        bins[t].fn(R);
        ref_bin(k, n, VA, VB, W);
        ref_bin(k, n, VA, VB, W + 16);
        ref_bin(k, n, VA, VA, W + 32);
        ref_bin(k, n, VA + 16, VB + 16, W + 48);
        ref_bin(k, n, VB + 32, VA + 32, W + 64);
        chk(bins[t].name, R, W, 80);
    }
}

static int IA[1003], IB[1003], IC1[1003], IC2[1003];
static short SA[1003], SB[1003], SO1[1003], SO2[1003];
static u8 BA[1003], BB[1003], BO1[1003], BO2[1003];

__attribute__((noinline)) static i64 sum_vec(const int *a, const int *b, int *c, u64 n)
{
    i64 s = 0;
    for (u64 i = 0; i < n; i++) { c[i] = a[i] + b[i] * 3; s += c[i]; }
    return s;
}
__attribute__((noinline, optnone)) static i64 sum_ref(const int *a, const int *b, int *c, u64 n)
{
    i64 s = 0;
    for (u64 i = 0; i < n; i++) { c[i] = a[i] + b[i] * 3; s += c[i]; }
    return s;
}
__attribute__((noinline)) static void sat_vec(const short *a, const short *b, short *o, u64 n)
{
    for (u64 i = 0; i < n; i++) { int v = a[i] - b[i]; o[i] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
}
__attribute__((noinline, optnone)) static void sat_ref(const short *a, const short *b, short *o, u64 n)
{
    for (u64 i = 0; i < n; i++) { int v = a[i] - b[i]; o[i] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
}
__attribute__((noinline)) static void avgmax_vec(const u8 *a, const u8 *b, u8 *o, u64 n)
{
    for (u64 i = 0; i < n; i++) { u8 m = a[i] > b[i] ? a[i] : b[i]; o[i] = (u8)((m + a[i] + 1) >> 1); }
}
__attribute__((noinline, optnone)) static void avgmax_ref(const u8 *a, const u8 *b, u8 *o, u64 n)
{
    for (u64 i = 0; i < n; i++) { u8 m = a[i] > b[i] ? a[i] : b[i]; o[i] = (u8)((m + a[i] + 1) >> 1); }
}
static void run_loops(void)
{
    for (int i = 0; i < 1003; i++) {
        IA[i] = (int)((u32)i * 2654435761u); IB[i] = (int)((u32)i * 40503u) - 20000;
        SA[i] = (short)(i * 97 - 30000); SB[i] = (short)(20000 - i * 61);
        BA[i] = (u8)(i * 37); BB[i] = (u8)(i * 91 + 7);
    }
    i64 s1 = sum_vec(IA, IB, IC1, 1003), s2 = sum_ref(IA, IB, IC2, 1003);
    chk("loop.sum_i32.total", (const u8 *)&s1, (const u8 *)&s2, 8);
    chk("loop.sum_i32.out", (const u8 *)IC1, (const u8 *)IC2, sizeof IC1);
    sat_vec(SA, SB, SO1, 1003); sat_ref(SA, SB, SO2, 1003);
    chk("loop.subs16", (const u8 *)SO1, (const u8 *)SO2, sizeof SO1);
    avgmax_vec(BA, BB, BO1, 1003); avgmax_ref(BA, BB, BO2, 1003);
    chk("loop.avg_max_u8", BO1, BO2, sizeof BO1);
}

int main(void)
{
    init_data();
    for (int pass = 0; pass < 3; pass++) {
        cur = 0;
        run_bins();
        run_loops();
    }
    for (int i = 0; i < ncheck; i++) {
        g_puts(cname[i]);
        if (!cfail[i]) { g_puts(" OK\n"); continue; }
        fails++;
        g_puts(" FAIL off="); g_putu64((u64)coff[i]);
        g_puts(" got="); g_puthex64(cgot[i]);
        g_puts(" want="); g_puthex64(cwant[i]); g_puts("\n");
    }
    if (fails) { g_puts("FAILURES: "); g_putu64((u64)fails); g_puts("\n"); return 1; }
    g_puts("ALL OK\n");
    return 0;
}

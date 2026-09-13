#include "gsys.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;

#define MAXCHK 256
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
static void chk64(const char *name, u64 got, u64 want)
{
    u8 g[8], w[8];
    for (int i = 0; i < 8; i++) { g[i] = (u8)(got >> (8 * i)); w[i] = (u8)(want >> (8 * i)); }
    chk(name, g, w, 8);
}

static u8 VA[64] __attribute__((aligned(64)));
static u8 VB[64] __attribute__((aligned(64)));
static u8 VG[64] __attribute__((aligned(64)));
static u8 R[512] __attribute__((aligned(64)));
static u8 W[512] __attribute__((aligned(64)));
static u64 rs = 0x243f6a8885a308d3ull;
static u8 rnd(void) { rs = rs * 6364136223846793005ull + 1442695040888963407ull; return (u8)((rs >> 57) ^ (rs >> 33)); }

static void init_data(void)
{
    for (int i = 0; i < 64; i++) { VA[i] = rnd(); VB[i] = rnd(); VG[i] = (u8)(0x5a ^ (i * 29)); }
    for (int i = 0; i < 64; i += 5) VB[i] = VA[i];
    for (int i = 1; i < 64; i += 8) { VB[i] = VA[i]; VB[i - 1] = VA[i - 1]; }
    VA[2] = 0x7f; VB[2] = 0x7f; VA[3] = 0x80; VB[3] = 0x80; VA[6] = 0xff; VB[6] = 0x01; VA[11] = 0x00; VB[11] = 0xff;
    VA[13] = 0x7f; VB[13] = 0x7f; VA[15] = 0x80; VB[15] = 0x80; VA[21] = 0x7f; VB[21] = 0x80;
    for (int i = 24; i < 32; i++) VB[i] = VA[i];
    for (int i = 36; i < 40; i++) VB[i] = VA[i];
}
static void fill(u8 *p, int n, u8 v) { for (int i = 0; i < n; i++) p[i] = v; }
static void copy(u8 *d, const u8 *s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }
static void pattern(u8 *d, const u8 *s, int unit, int n) { for (int i = 0; i < n; i++) d[i] = s[i % unit]; }

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
static void ref_bin(int k, int n, int nbytes, const u8 *a, const u8 *b, u8 *o)
{
    fill(o, 32, 0);
    for (int i = 0; i < nbytes; i += n) wr(o + i, n, ref_el(k, n, rd(a + i, n), rd(b + i, n)));
}

#define BIN(fn, mn) \
__attribute__((noinline)) static void fn(u8 *r) \
{ \
    __asm__ volatile("vmovdqu (%0), %%ymm1\n\tvmovdqu (%1), %%ymm2\n\tvmovdqu (%3), %%ymm3\n\t" \
                     mn " %%ymm2, %%ymm1, %%ymm3\n\tvmovdqu %%ymm3, (%2)\n\t" \
                     "vmovdqu (%3), %%ymm4\n\t" mn " (%1), %%ymm1, %%ymm4\n\tvmovdqu %%ymm4, 32(%2)\n\t" \
                     "vmovdqu %%ymm1, %%ymm5\n\t" mn " %%ymm2, %%ymm5, %%ymm5\n\tvmovdqu %%ymm5, 64(%2)\n\t" \
                     "vmovdqu %%ymm2, %%ymm6\n\t" mn " %%ymm6, %%ymm1, %%ymm6\n\tvmovdqu %%ymm6, 96(%2)\n\t" \
                     "vmovdqu (%3), %%ymm7\n\t" mn " %%xmm2, %%xmm1, %%xmm7\n\tvmovdqu %%ymm7, 128(%2)\n\t" \
                     "vmovdqu (%3), %%ymm8\n\t" mn " (%1), %%xmm1, %%xmm8\n\tvmovdqu %%ymm8, 160(%2)\n\t" \
                     "vmovdqu (%3), %%ymm9\n\t" mn " %%xmm1, %%xmm1, %%xmm9\n\tvmovdqu %%ymm9, 192(%2)\n\t" \
                     "vmovdqu %%ymm1, %%ymm10\n\t" mn " %%ymm10, %%ymm10, %%ymm10\n\tvmovdqu %%ymm10, 224(%2)" \
                     :: "r"(VA), "r"(VB), "r"(r), "r"(VG) \
                     : "memory", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10"); \
}
BIN(b_pxor, "vpxor") BIN(b_pand, "vpand") BIN(b_por, "vpor") BIN(b_pandn, "vpandn")
BIN(b_xorps, "vxorps") BIN(b_andps, "vandps") BIN(b_orps, "vorps") BIN(b_andnps, "vandnps")
BIN(b_xorpd, "vxorpd") BIN(b_andnpd, "vandnpd")
BIN(b_paddb, "vpaddb") BIN(b_paddw, "vpaddw") BIN(b_paddd, "vpaddd") BIN(b_paddq, "vpaddq")
BIN(b_psubb, "vpsubb") BIN(b_psubw, "vpsubw") BIN(b_psubd, "vpsubd") BIN(b_psubq, "vpsubq")
BIN(b_pcmpeqb, "vpcmpeqb") BIN(b_pcmpeqw, "vpcmpeqw") BIN(b_pcmpeqd, "vpcmpeqd") BIN(b_pcmpeqq, "vpcmpeqq")
BIN(b_pcmpgtb, "vpcmpgtb") BIN(b_pcmpgtw, "vpcmpgtw") BIN(b_pcmpgtd, "vpcmpgtd") BIN(b_pcmpgtq, "vpcmpgtq")
BIN(b_pminub, "vpminub") BIN(b_pminuw, "vpminuw") BIN(b_pminud, "vpminud")
BIN(b_pmaxub, "vpmaxub") BIN(b_pmaxuw, "vpmaxuw") BIN(b_pmaxud, "vpmaxud")
BIN(b_pminsb, "vpminsb") BIN(b_pminsw, "vpminsw") BIN(b_pminsd, "vpminsd")
BIN(b_pmaxsb, "vpmaxsb") BIN(b_pmaxsw, "vpmaxsw") BIN(b_pmaxsd, "vpmaxsd")
BIN(b_pmullw, "vpmullw") BIN(b_pmulld, "vpmulld")
BIN(b_paddusb, "vpaddusb") BIN(b_paddusw, "vpaddusw") BIN(b_psubusb, "vpsubusb") BIN(b_psubusw, "vpsubusw")
BIN(b_paddsb, "vpaddsb") BIN(b_paddsw, "vpaddsw") BIN(b_psubsb, "vpsubsb") BIN(b_psubsw, "vpsubsw")
BIN(b_pavgb, "vpavgb") BIN(b_pavgw, "vpavgw")

typedef void (*tfn)(u8 *);
static const struct { const char *name; tfn fn; u8 kind, n; } bins[] = {
    { "vpxor", b_pxor, K_XOR, 1 }, { "vpand", b_pand, K_AND, 1 }, { "vpor", b_por, K_OR, 1 }, { "vpandn", b_pandn, K_ANDN, 1 },
    { "vxorps", b_xorps, K_XOR, 1 }, { "vandps", b_andps, K_AND, 1 }, { "vorps", b_orps, K_OR, 1 }, { "vandnps", b_andnps, K_ANDN, 1 },
    { "vxorpd", b_xorpd, K_XOR, 1 }, { "vandnpd", b_andnpd, K_ANDN, 1 },
    { "vpaddb", b_paddb, K_ADD, 1 }, { "vpaddw", b_paddw, K_ADD, 2 }, { "vpaddd", b_paddd, K_ADD, 4 }, { "vpaddq", b_paddq, K_ADD, 8 },
    { "vpsubb", b_psubb, K_SUB, 1 }, { "vpsubw", b_psubw, K_SUB, 2 }, { "vpsubd", b_psubd, K_SUB, 4 }, { "vpsubq", b_psubq, K_SUB, 8 },
    { "vpcmpeqb", b_pcmpeqb, K_EQ, 1 }, { "vpcmpeqw", b_pcmpeqw, K_EQ, 2 }, { "vpcmpeqd", b_pcmpeqd, K_EQ, 4 }, { "vpcmpeqq", b_pcmpeqq, K_EQ, 8 },
    { "vpcmpgtb", b_pcmpgtb, K_GT, 1 }, { "vpcmpgtw", b_pcmpgtw, K_GT, 2 }, { "vpcmpgtd", b_pcmpgtd, K_GT, 4 }, { "vpcmpgtq", b_pcmpgtq, K_GT, 8 },
    { "vpminub", b_pminub, K_UMIN, 1 }, { "vpminuw", b_pminuw, K_UMIN, 2 }, { "vpminud", b_pminud, K_UMIN, 4 },
    { "vpmaxub", b_pmaxub, K_UMAX, 1 }, { "vpmaxuw", b_pmaxuw, K_UMAX, 2 }, { "vpmaxud", b_pmaxud, K_UMAX, 4 },
    { "vpminsb", b_pminsb, K_SMIN, 1 }, { "vpminsw", b_pminsw, K_SMIN, 2 }, { "vpminsd", b_pminsd, K_SMIN, 4 },
    { "vpmaxsb", b_pmaxsb, K_SMAX, 1 }, { "vpmaxsw", b_pmaxsw, K_SMAX, 2 }, { "vpmaxsd", b_pmaxsd, K_SMAX, 4 },
    { "vpmullw", b_pmullw, K_MUL, 2 }, { "vpmulld", b_pmulld, K_MUL, 4 },
    { "vpaddusb", b_paddusb, K_UQADD, 1 }, { "vpaddusw", b_paddusw, K_UQADD, 2 },
    { "vpsubusb", b_psubusb, K_UQSUB, 1 }, { "vpsubusw", b_psubusw, K_UQSUB, 2 },
    { "vpaddsb", b_paddsb, K_SQADD, 1 }, { "vpaddsw", b_paddsw, K_SQADD, 2 },
    { "vpsubsb", b_psubsb, K_SQSUB, 1 }, { "vpsubsw", b_psubsw, K_SQSUB, 2 },
    { "vpavgb", b_pavgb, K_AVG, 1 }, { "vpavgw", b_pavgw, K_AVG, 2 },
};
static void run_bins(void)
{
    for (unsigned t = 0; t < sizeof bins / sizeof bins[0]; t++) {
        int k = bins[t].kind, n = bins[t].n;
        fill(R, 256, 0xee);
        bins[t].fn(R);
        ref_bin(k, n, 32, VA, VB, W);
        copy(W + 32, W, 32); copy(W + 64, W, 32); copy(W + 96, W, 32);
        ref_bin(k, n, 16, VA, VB, W + 128);
        copy(W + 160, W + 128, 32);
        ref_bin(k, n, 16, VA, VA, W + 192);
        ref_bin(k, n, 32, VA, VA, W + 224);
        chk(bins[t].name, R, W, 256);
    }
}

#define SHF(fn, mn, c) \
__attribute__((noinline)) static void fn(u8 *r) \
{ \
    __asm__ volatile("vmovdqu (%0), %%ymm1\n\t" \
                     mn " $" #c ", %%ymm1, %%ymm2\n\tvmovdqu %%ymm2, (%1)\n\t" \
                     "vmovdqu (%2), %%ymm3\n\t" mn " $" #c ", %%xmm1, %%xmm3\n\tvmovdqu %%ymm3, 32(%1)\n\t" \
                     "vmovdqu %%ymm1, %%ymm4\n\t" mn " $" #c ", %%ymm4, %%ymm4\n\tvmovdqu %%ymm4, 64(%1)" \
                     :: "r"(VA), "r"(r), "r"(VG) : "memory", "xmm1", "xmm2", "xmm3", "xmm4"); \
}
#define SHSET(p, mn) SHF(p##_0, mn, 0) SHF(p##_3, mn, 3) SHF(p##_15, mn, 15) SHF(p##_16, mn, 16) \
                     SHF(p##_31, mn, 31) SHF(p##_33, mn, 33) SHF(p##_64, mn, 64) SHF(p##_255, mn, 255)
SHSET(sllw, "vpsllw") SHSET(slld, "vpslld") SHSET(sllq, "vpsllq")
SHSET(srlw, "vpsrlw") SHSET(srld, "vpsrld") SHSET(srlq, "vpsrlq")
SHSET(sraw, "vpsraw") SHSET(srad, "vpsrad")
#define SHENT(p, mn, k, n) { mn " $0", p##_0, k, n, 0 }, { mn " $3", p##_3, k, n, 3 }, { mn " $15", p##_15, k, n, 15 }, \
                           { mn " $16", p##_16, k, n, 16 }, { mn " $31", p##_31, k, n, 31 }, { mn " $33", p##_33, k, n, 33 }, \
                           { mn " $64", p##_64, k, n, 64 }, { mn " $255", p##_255, k, n, 255 },
static const struct { const char *name; tfn fn; u8 kind, n; u8 cnt; } shifts[] = {
    SHENT(sllw, "vpsllw", 0, 2) SHENT(slld, "vpslld", 0, 4) SHENT(sllq, "vpsllq", 0, 8)
    SHENT(srlw, "vpsrlw", 1, 2) SHENT(srld, "vpsrld", 1, 4) SHENT(srlq, "vpsrlq", 1, 8)
    SHENT(sraw, "vpsraw", 2, 2) SHENT(srad, "vpsrad", 2, 4)
};
static void ref_shift(int kind, int n, unsigned cnt, int nbytes, const u8 *a, u8 *o)
{
    unsigned w = 8u * (unsigned)n;
    u64 m = n == 8 ? ~0ull : (1ull << w) - 1;
    fill(o, 32, 0);
    for (int i = 0; i < nbytes; i += n) {
        u64 v = rd(a + i, n), r;
        if (kind == 0) r = cnt >= w ? 0 : (v << cnt) & m;
        else if (kind == 1) r = cnt >= w ? 0 : v >> cnt;
        else r = (u64)(sx(v, n) >> (cnt >= w ? w - 1 : cnt)) & m;
        wr(o + i, n, r);
    }
}
static void run_shifts(void)
{
    for (unsigned t = 0; t < sizeof shifts / sizeof shifts[0]; t++) {
        fill(R, 96, 0xee);
        shifts[t].fn(R);
        ref_shift(shifts[t].kind, shifts[t].n, shifts[t].cnt, 32, VA, W);
        ref_shift(shifts[t].kind, shifts[t].n, shifts[t].cnt, 16, VA, W + 32);
        copy(W + 64, W, 32);
        chk(shifts[t].name, R, W, 96);
    }
}

__asm__(".section __TEXT,__const\n.p2align 5\n.globl _avxjit_k\n_avxjit_k:\n"
        ".quad 0x0123456789abcdef, 0xfedcba9876543210, 0x8899aabbccddeeff, 0x7766554433221100\n.text\n");
extern const u8 avxjit_k[32];

__attribute__((noinline)) static void t_mov1(u8 *r)
{
    __asm__ volatile("vmovdqu (%0), %%ymm1\n\tvmovdqu %%ymm1, (%1)\n\t"
                     "vmovdqa %%ymm1, %%ymm2\n\tvmovdqu %%ymm2, 32(%1)\n\t"
                     "vmovdqu (%2), %%ymm3\n\tvmovdqu 32(%0), %%xmm3\n\tvmovdqu %%ymm3, 64(%1)\n\t"
                     "vmovdqu (%2), %%ymm4\n\tvmovdqa %%xmm1, %%xmm4\n\tvmovdqu %%ymm4, 96(%1)\n\t"
                     "vmovdqu (%2), %%ymm5\n\tvmovaps %%xmm5, %%xmm5\n\tvmovdqu %%ymm5, 128(%1)\n\t"
                     "vmovdqu %%xmm1, 160(%1)\n\t"
                     "vmovups 32(%0), %%ymm6\n\tvmovaps %%ymm6, 192(%1)\n\t"
                     "vmovdqu (%2), %%ymm7\n\tvmovups %%ymm7, %%ymm7\n\tvmovdqu %%ymm7, 224(%1)"
                     :: "r"(VA), "r"(r), "r"(VG) : "memory", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
}
__attribute__((noinline)) static void t_mov2(u8 *r, u64 big1, u64 big2, u64 big3, u64 rbig)
{
    u64 ix = 16;
    __asm__ volatile("vmovdqu (%0,%1,1), %%ymm1\n\tvmovdqu %%ymm1, (%2,%1,2)\n\t"
                     "vmovdqu 0x10000(%3), %%ymm2\n\tvmovdqu %%ymm2, 64(%2)\n\t"
                     "vmovdqu 0xfff0(%4), %%ymm3\n\tvmovdqu %%ymm3, 96(%2)\n\t"
                     "vmovdqu 0xffe0(%5), %%ymm4\n\tvmovdqu %%ymm4, 128(%2)\n\t"
                     "vmovdqu -32(%0,%1,4), %%ymm5\n\tvmovdqu %%ymm5, 0x10000(%6)\n\t"
                     "vmovdqu _avxjit_k(%%rip), %%ymm6\n\tvmovdqu %%ymm6, 192(%2)\n\t"
                     "vpaddb 0x10000(%3), %%ymm6, %%ymm7\n\tvmovdqu %%ymm7, 224(%2)"
                     :: "r"(VA), "r"(ix), "r"(r), "r"(big1), "r"(big2), "r"(big3), "r"(rbig)
                     : "memory", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
}
static void run_mov(void)
{
    fill(R, 256, 0xee);
    t_mov1(R);
    copy(W, VA, 32); copy(W + 32, VA, 32);
    copy(W + 64, VA + 32, 16); fill(W + 80, 16, 0);
    copy(W + 96, VA, 16); fill(W + 112, 16, 0);
    copy(W + 128, VG, 16); fill(W + 144, 16, 0);
    copy(W + 160, VA, 16); fill(W + 176, 16, 0xee);
    copy(W + 192, VA + 32, 32);
    copy(W + 224, VG, 32);
    chk("vmovdqu.forms", R, W, 256);

    fill(R, 256, 0xee);
    u64 a = (u64)(unsigned long)VA, r = (u64)(unsigned long)R;
    t_mov2(R, a - 0x10000, a - 0xfff0, a - 0xffe0, r + 160 - 0x10000);
    fill(W, 32, 0xee);
    copy(W + 32, VA + 16, 32);
    copy(W + 64, VA, 32); copy(W + 96, VA, 32); copy(W + 128, VA, 32);
    copy(W + 160, VA + 32, 32);
    copy(W + 192, avxjit_k, 32);
    for (int i = 0; i < 32; i++) W[224 + i] = (u8)(avxjit_k[i] + VA[i]);
    chk("vmovdqu.addressing", R, W, 256);
}

__attribute__((noinline)) static void t_mskb(u64 *o)
{
    __asm__ volatile("vmovdqu (%0), %%ymm1\n\tvpmovmskb %%ymm1, %%eax\n\tmovq %%rax, (%1)\n\t"
                     "movq $-1, %%r9\n\tvpmovmskb %%xmm1, %%r9d\n\tmovq %%r9, 8(%1)\n\t"
                     "movq $-1, %%r10\n\tvpmovmskb %%ymm1, %%r10\n\tmovq %%r10, 16(%1)\n\t"
                     "vmovdqu (%2), %%ymm2\n\tvpcmpeqb %%ymm1, %%ymm2, %%ymm3\n\tvpmovmskb %%ymm3, %%ecx\n\tmovq %%rcx, 24(%1)"
                     :: "r"(VA), "r"(o), "r"(VB) : "memory", "rax", "rcx", "r9", "r10", "xmm1", "xmm2", "xmm3");
}
static void run_mskb(void)
{
    u64 o[4];
    t_mskb(o);
    u64 m = 0, e = 0;
    for (int i = 0; i < 32; i++) { m |= (u64)(VA[i] >> 7) << i; e |= (u64)(VA[i] == VB[i]) << i; }
    chk64("vpmovmskb.y", o[0], m);
    chk64("vpmovmskb.x", o[1], m & 0xffff);
    chk64("vpmovmskb.y.r64", o[2], m);
    chk64("vpcmpeqb.vpmovmskb", o[3], e);
}

__attribute__((noinline)) static void t_bcast(u8 *r)
{
    __asm__ volatile("vmovdqu (%0), %%ymm1\n\t"
                     "vmovdqu (%2), %%ymm2\n\tvpbroadcastb %%xmm1, %%ymm2\n\tvmovdqu %%ymm2, (%1)\n\t"
                     "vmovdqu (%2), %%ymm3\n\tvpbroadcastb %%xmm1, %%xmm3\n\tvmovdqu %%ymm3, 32(%1)\n\t"
                     "vpbroadcastb 5(%0), %%ymm4\n\tvmovdqu %%ymm4, 64(%1)\n\t"
                     "vmovdqu (%2), %%ymm5\n\tvpbroadcastw 6(%0), %%xmm5\n\tvmovdqu %%ymm5, 96(%1)\n\t"
                     "vpbroadcastd %%xmm1, %%ymm6\n\tvmovdqu %%ymm6, 128(%1)\n\t"
                     "vpbroadcastq 8(%0), %%ymm7\n\tvmovdqu %%ymm7, 160(%1)\n\t"
                     "vbroadcastss 4(%0), %%ymm8\n\tvmovdqu %%ymm8, 192(%1)\n\t"
                     "vmovdqu (%2), %%ymm9\n\tvbroadcastss %%xmm1, %%xmm9\n\tvmovdqu %%ymm9, 224(%1)\n\t"
                     "vbroadcastsd %%xmm1, %%ymm10\n\tvmovdqu %%ymm10, 256(%1)\n\t"
                     "vbroadcasti128 16(%0), %%ymm11\n\tvmovdqu %%ymm11, 288(%1)\n\t"
                     "vbroadcastf128 32(%0), %%ymm12\n\tvmovdqu %%ymm12, 320(%1)\n\t"
                     "vmovdqu %%ymm1, %%ymm13\n\tvpbroadcastd %%xmm13, %%ymm13\n\tvmovdqu %%ymm13, 352(%1)\n\t"
                     "vpbroadcastw %%xmm1, %%ymm14\n\tvmovdqu %%ymm14, 384(%1)\n\t"
                     "vmovdqu (%2), %%ymm15\n\tvpbroadcastq %%xmm1, %%xmm15\n\tvmovdqu %%ymm15, 416(%1)"
                     :: "r"(VA), "r"(r), "r"(VG) : "memory", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
                     "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}
static void run_bcast(void)
{
    fill(R, 448, 0xee);
    t_bcast(R);
    fill(W, 448, 0);
    pattern(W, VA, 1, 32);
    pattern(W + 32, VA, 1, 16);
    pattern(W + 64, VA + 5, 1, 32);
    pattern(W + 96, VA + 6, 2, 16);
    pattern(W + 128, VA, 4, 32);
    pattern(W + 160, VA + 8, 8, 32);
    pattern(W + 192, VA + 4, 4, 32);
    pattern(W + 224, VA, 4, 16);
    pattern(W + 256, VA, 8, 32);
    pattern(W + 288, VA + 16, 16, 32);
    pattern(W + 320, VA + 32, 16, 32);
    pattern(W + 352, VA, 4, 32);
    pattern(W + 384, VA, 2, 32);
    pattern(W + 416, VA, 8, 16);
    chk("vpbroadcast.forms", R, W, 448);
}

__attribute__((noinline)) static void t_zu(u8 *r)
{
    __asm__ volatile("vmovdqu (%0), %%ymm0\n\tvmovdqu %%ymm0, %%ymm1\n\tvmovdqu %%ymm0, %%ymm2\n\tvmovdqu %%ymm0, %%ymm3\n\t"
                     "vmovdqu %%ymm0, %%ymm4\n\tvmovdqu %%ymm0, %%ymm5\n\tvmovdqu %%ymm0, %%ymm6\n\tvmovdqu %%ymm0, %%ymm7\n\t"
                     "vmovdqu %%ymm0, %%ymm8\n\tvmovdqu %%ymm0, %%ymm9\n\tvmovdqu %%ymm0, %%ymm10\n\tvmovdqu %%ymm0, %%ymm11\n\t"
                     "vmovdqu %%ymm0, %%ymm12\n\tvmovdqu %%ymm0, %%ymm13\n\tvmovdqu %%ymm0, %%ymm14\n\tvmovdqu %%ymm0, %%ymm15\n\t"
                     "vzeroupper\n\t"
                     "vmovdqu %%ymm0, (%1)\n\tvmovdqu %%ymm1, 32(%1)\n\tvmovdqu %%ymm2, 64(%1)\n\tvmovdqu %%ymm3, 96(%1)\n\t"
                     "vmovdqu %%ymm4, 128(%1)\n\tvmovdqu %%ymm5, 160(%1)\n\tvmovdqu %%ymm6, 192(%1)\n\tvmovdqu %%ymm7, 224(%1)\n\t"
                     "vmovdqu %%ymm8, 256(%1)\n\tvmovdqu %%ymm9, 288(%1)\n\tvmovdqu %%ymm10, 320(%1)\n\tvmovdqu %%ymm11, 352(%1)\n\t"
                     "vmovdqu %%ymm12, 384(%1)\n\tvmovdqu %%ymm13, 416(%1)\n\tvmovdqu %%ymm14, 448(%1)\n\tvmovdqu %%ymm15, 480(%1)"
                     :: "r"(VA), "r"(r) : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
                     "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}
static void run_zu(void)
{
    fill(R, 512, 0xee);
    t_zu(R);
    for (int i = 0; i < 16; i++) { copy(W + 32 * i, VA, 16); fill(W + 32 * i + 16, 16, 0); }
    chk("vzeroupper.all", R, W, 512);
}

__attribute__((noinline)) static void t_pmovx(u8 *r)
{
    __asm__ volatile("vmovdqu (%0), %%ymm1\n\t"
                     "vpmovsxdq %%xmm1, %%ymm2\n\tvmovdqu %%ymm2, (%1)\n\t"
                     "vpmovsxdq 8(%0), %%ymm3\n\tvmovdqu %%ymm3, 32(%1)\n\t"
                     "vmovdqu (%2), %%ymm4\n\tvpmovsxdq (%0), %%xmm4\n\tvmovdqu %%ymm4, 64(%1)\n\t"
                     "vpmovzxbw %%xmm1, %%ymm5\n\tvmovdqu %%ymm5, 96(%1)\n\t"
                     "vpmovsxwd 4(%0), %%ymm6\n\tvmovdqu %%ymm6, 128(%1)\n\t"
                     "vmovdqu %%ymm1, %%ymm7\n\tvpmovzxdq %%xmm7, %%ymm7\n\tvmovdqu %%ymm7, 160(%1)\n\t"
                     "vmovdqu (%2), %%ymm8\n\tvpmovsxbw %%xmm8, %%xmm8\n\tvmovdqu %%ymm8, 192(%1)\n\t"
                     "vmovdqu (%2), %%ymm9\n\tvpmovzxwd 12(%0), %%xmm9\n\tvmovdqu %%ymm9, 224(%1)\n\t"
                     "vpmovsxbw 16(%0), %%ymm10\n\tvmovdqu %%ymm10, 256(%1)\n\t"
                     "vmovdqu (%2), %%ymm11\n\tvpmovzxdq 20(%0), %%xmm11\n\tvmovdqu %%ymm11, 288(%1)"
                     :: "r"(VA), "r"(r), "r"(VG) : "memory", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6",
                     "xmm7", "xmm8", "xmm9", "xmm10", "xmm11");
}
static void ref_x(int sgn, int in, int out, const u8 *src, int cnt, u8 *o)
{
    fill(o, 32, 0);
    for (int i = 0; i < cnt; i++) {
        u64 v = rd(src + i * in, in);
        if (sgn) v = (u64)sx(v, in);
        wr(o + i * out, out, v);
    }
}
static void run_pmovx(void)
{
    fill(R, 320, 0xee);
    t_pmovx(R);
    ref_x(1, 4, 8, VA, 4, W);
    ref_x(1, 4, 8, VA + 8, 4, W + 32);
    ref_x(1, 4, 8, VA, 2, W + 64);
    ref_x(0, 1, 2, VA, 16, W + 96);
    ref_x(1, 2, 4, VA + 4, 8, W + 128);
    ref_x(0, 4, 8, VA, 4, W + 160);
    ref_x(1, 1, 2, VG, 8, W + 192);
    ref_x(0, 2, 4, VA + 12, 4, W + 224);
    ref_x(1, 1, 2, VA + 16, 16, W + 256);
    ref_x(0, 4, 8, VA + 20, 2, W + 288);
    chk("vpmovsx.forms", R, W, 320);
}

__asm__(".text\n.p2align 4\n"
        ".globl _kj_memclr\n_kj_memclr:\n vpxor %xmm0, %xmm0, %xmm0\n"
        "1: vmovdqu %ymm0, 0(%rdi)\n vmovdqu %ymm0, 32(%rdi)\n addq $64, %rdi\n subq $64, %rsi\n ja 1b\n vzeroupper\n ret\n"
        ".globl _kj_index\n_kj_index:\n movd %edx, %xmm0\n vpbroadcastb %xmm0, %ymm1\n movq %rdi, %rax\n"
        "1: vmovdqu (%rax), %ymm2\n vpcmpeqb %ymm1, %ymm2, %ymm3\n vpmovmskb %ymm3, %ecx\n testl %ecx, %ecx\n jnz 2f\n"
        " addq $32, %rax\n subq $32, %rsi\n ja 1b\n movq $-1, %rax\n vzeroupper\n ret\n"
        "2: bsfl %ecx, %ecx\n addq %rcx, %rax\n subq %rdi, %rax\n vzeroupper\n ret\n"
        ".globl _kj_memeq\n_kj_memeq:\n xorl %eax, %eax\n"
        "1: vmovdqu (%rdi,%rax), %ymm0\n vpcmpeqb (%rsi,%rax), %ymm0, %ymm1\n vpmovmskb %ymm1, %ecx\n"
        " cmpl $0xffffffff, %ecx\n jne 2f\n addq $32, %rax\n cmpq %rdx, %rax\n jb 1b\n movl $1, %eax\n vzeroupper\n ret\n"
        "2: xorl %eax, %eax\n vzeroupper\n ret\n"
        ".globl _kj_f1\n_kj_f1:\n vmovdqu (%rdx), %ymm0\n cmpq %rsi, %rdi\n vpaddd %ymm0, %ymm0, %ymm1\n je 1f\n"
        " xorl %eax, %eax\n vzeroupper\n ret\n1: movl $1, %eax\n vzeroupper\n ret\n"
        ".globl _kj_f2\n_kj_f2:\n vmovdqu (%rdx), %ymm0\n cmpq %rsi, %rdi\n vtestps %ymm0, %ymm0\n je 1f\n"
        " xorl %eax, %eax\n vzeroupper\n ret\n1: movl $1, %eax\n vzeroupper\n ret\n"
        ".globl _kj_f3\n_kj_f3:\n vmovdqu (%rdx), %xmm0\n vmovdqu 16(%rdx), %xmm1\n cmpq %rsi, %rdi\n"
        " vpcmpistri $0x0c, %xmm1, %xmm0\n movl %ecx, %eax\n jc 1f\n orl $0x100, %eax\n1: vzeroupper\n ret\n");
extern u64 kj_memclr(u8 *p, u64 n);
extern u64 kj_index(const u8 *p, u64 n, u64 c);
extern u64 kj_memeq(const u8 *a, const u8 *b, u64 n);
extern u64 kj_f1(u64 a, u64 b, const u8 *p);
extern u64 kj_f2(u64 a, u64 b, const u8 *p);
extern u64 kj_f3(u64 a, u64 b, const u8 *p);

static u8 BUF1[1024] __attribute__((aligned(64)));
static u8 BUF2[1024] __attribute__((aligned(64)));
static const u8 NEEDLE[32] = "world\0\0\0\0\0\0\0\0\0\0\0hello world\0\0\0\0";
static const u8 NONEEDLE[32] = "xyz\0\0\0\0\0\0\0\0\0\0\0\0\0hello world\0\0\0\0";

static void run_kernels(void)
{
    u8 zero[32] = { 0 };
    fill(BUF1, 1024, 0x33);
    kj_memclr(BUF1 + 64, 640);
    int ok = 1;
    for (int i = 0; i < 1024; i++) if (BUF1[i] != (i >= 64 && i < 704 ? 0 : 0x33)) ok = 0;
    chk64("kernel.memclr", (u64)ok, 1);

    for (int i = 0; i < 1024; i++) BUF1[i] = (u8)(i * 7 + 1) | 0x80;
    BUF1[517] = 42;
    chk64("kernel.index.hit", kj_index(BUF1, 1024, 42), 517);
    chk64("kernel.index.miss", kj_index(BUF1, 512, 42), ~0ull);

    copy(BUF2, BUF1, 1024);
    chk64("kernel.memeq.same", kj_memeq(BUF1, BUF2, 1024), 1);
    BUF2[1000] ^= 1;
    chk64("kernel.memeq.diff", kj_memeq(BUF1, BUF2, 1024), 0);

    chk64("flags.cmp.vex.je.taken", kj_f1(5, 5, VA), 1);
    chk64("flags.cmp.vex.je.not", kj_f1(5, 6, VA), 0);
    chk64("flags.vtestps.zf1", kj_f2(1, 2, zero), 1);
    chk64("flags.vtestps.zf0", kj_f2(7, 7, VA), 0);
    chk64("flags.vpcmpistri.cf1", kj_f3(1, 2, NEEDLE), 6);
    chk64("flags.vpcmpistri.cf0", kj_f3(1, 2, NONEEDLE), 0x110);
}

static int IA[1000], IB[1000], IC1[1000], IC2[1000];
static short SA[1000], SB[1000], SO1[1000], SO2[1000];
static u32 UA[1000], UB[1000], UO1[1000], UO2[1000];

__attribute__((target("avx2"), noinline)) static i64 sum_avx2(const int *a, const int *b, int *c, u64 n)
{
    i64 s = 0;
    for (u64 i = 0; i < n; i++) { c[i] = a[i] + b[i] * 3; s += c[i]; }
    return s;
}
__attribute__((noinline)) static i64 sum_ref(const int *a, const int *b, int *c, u64 n)
{
    i64 s = 0;
    for (u64 i = 0; i < n; i++) { c[i] = a[i] + b[i] * 3; s += c[i]; }
    return s;
}
__attribute__((target("avx2"), noinline)) static void sat_avx2(const short *a, const short *b, short *o, u64 n)
{
    for (u64 i = 0; i < n; i++) { int v = a[i] + b[i]; o[i] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
}
__attribute__((noinline)) static void sat_ref(const short *a, const short *b, short *o, u64 n)
{
    for (u64 i = 0; i < n; i++) { int v = a[i] + b[i]; o[i] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
}
__attribute__((target("avx2"), noinline)) static void minmax_avx2(const u32 *a, const u32 *b, u32 *o, u64 n)
{
    for (u64 i = 0; i < n; i++) o[i] = (a[i] < b[i] ? a[i] : b[i]) ^ ((a[i] > b[i] ? a[i] : b[i]) >> 3);
}
__attribute__((noinline)) static void minmax_ref(const u32 *a, const u32 *b, u32 *o, u64 n)
{
    for (u64 i = 0; i < n; i++) o[i] = (a[i] < b[i] ? a[i] : b[i]) ^ ((a[i] > b[i] ? a[i] : b[i]) >> 3);
}
static void run_loops(void)
{
    for (int i = 0; i < 1000; i++) {
        IA[i] = (int)((u32)i * 2654435761u); IB[i] = (int)((u32)i * 40503u) - 20000;
        SA[i] = (short)(i * 97 - 30000); SB[i] = (short)(i * 61 - 1000);
        UA[i] = (u32)i * 2246822519u; UB[i] = (u32)i * 3266489917u;
    }
    i64 s1 = sum_avx2(IA, IB, IC1, 1000), s2 = sum_ref(IA, IB, IC2, 1000);
    chk64("loop.sum_i32.total", (u64)s1, (u64)s2);
    chk("loop.sum_i32.out", (const u8 *)IC1, (const u8 *)IC2, sizeof IC1);
    sat_avx2(SA, SB, SO1, 1000); sat_ref(SA, SB, SO2, 1000);
    chk("loop.sat16", (const u8 *)SO1, (const u8 *)SO2, sizeof SO1);
    minmax_avx2(UA, UB, UO1, 1000); minmax_ref(UA, UB, UO2, 1000);
    chk("loop.minmax_u32", (const u8 *)UO1, (const u8 *)UO2, sizeof UO1);
}

int main(void)
{
    init_data();
    for (int pass = 0; pass < 3; pass++) {
        cur = 0;
        run_bins();
        run_shifts();
        run_mov();
        run_mskb();
        run_bcast();
        run_zu();
        run_pmovx();
        run_kernels();
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

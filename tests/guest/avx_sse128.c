#include "gsys.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

static u8 FA[32] __attribute__((aligned(32)));
static u8 FB[32] __attribute__((aligned(32)));
static u8 FC[32] __attribute__((aligned(32)));
static u8 FD[32] __attribute__((aligned(32)));
static u8 DA[32] __attribute__((aligned(32)));
static u8 DB[32] __attribute__((aligned(32)));
static u8 DC[32] __attribute__((aligned(32)));
static u8 DD[32] __attribute__((aligned(32)));
static u8 IA[32] __attribute__((aligned(32)));
static u8 IB[32] __attribute__((aligned(32)));
static u8 IG[32] __attribute__((aligned(32)));
static u8 R[256] __attribute__((aligned(64)));

static void wr32(u8 *p, u32 v) { for (int i = 0; i < 4; i++) p[i] = (u8)(v >> (8 * i)); }
static void wr64(u8 *p, u64 v) { for (int i = 0; i < 8; i++) p[i] = (u8)(v >> (8 * i)); }
static void fill(u8 *p, int n, u8 v) { for (int i = 0; i < n; i++) p[i] = v; }

static void init(void)
{
    static const u32 fa[8] = { 0x3fc00000u, 0x80000000u, 0x7fc00123u, 0x40400000u, 0x7e967699u, 0xc0200000u, 0x7f800000u, 0x3e800000u };
    static const u32 fb[8] = { 0x3f000000u, 0x7f800011u, 0xffc00456u, 0xc0400000u, 0x00000001u, 0x40200000u, 0xff800000u, 0x00000000u };
    static const u64 da[4] = { 0x3ff8000000000000ull, 0x7ff8000000000123ull, 0x8000000000000000ull, 0x7ff0000000000000ull };
    static const u64 db[4] = { 0x3fe0000000000000ull, 0x7ff0000000000011ull, 0x0000000000000000ull, 0xfff0000000000000ull };
    for (int i = 0; i < 8; i++) {
        wr32(FA + 4 * i, fa[i]); wr32(FB + 4 * i, fb[i]);
        wr32(FC + 4 * i, fa[(i + 2) % 8]); wr32(FD + 4 * i, fb[(i + 1) % 8]);
    }
    for (int i = 0; i < 4; i++) {
        wr64(DA + 8 * i, da[i]); wr64(DB + 8 * i, db[i]);
        wr64(DC + 8 * i, da[(i + 1) % 4]); wr64(DD + 8 * i, db[(i + 1) % 4]);
    }
    for (int i = 0; i < 32; i++) { IA[i] = (u8)(i * 37 + 11); IB[i] = (u8)(0x80 ^ (i * 53)); IG[i] = (u8)(0xa5 ^ (i * 29)); }
    IB[0] = 3; IB[1] = 0x8f; IB[2] = 0x1f; IB[3] = 15; IB[4] = 0x7f;
}

static void hexline(const char *name, int form, const u8 *p, int n)
{
    static const char hx[] = "0123456789abcdef";
    char buf[96];
    int k = 0;
    g_puts(name);
    buf[k++] = ' '; buf[k++] = (char)('0' + form); buf[k++] = ':'; buf[k++] = ' ';
    for (int i = 0; i < n && i < 32; i++) { buf[k++] = hx[p[i] >> 4]; buf[k++] = hx[p[i] & 15]; }
    buf[k++] = '\n';
    sys_write(1, buf, (g_u64)k);
}
static void dump(const char *name, int base, int forms)
{
    for (int f = 0; f < forms; f++) hexline(name, base + f, R + 32 * f, 32);
}

typedef void (*bfn)(const u8 *, const u8 *, u8 *);

#define FP2(fn, mn) \
__attribute__((noinline)) static void fn(const u8 *a, const u8 *b, u8 *r) \
{ \
    __asm__ volatile("vmovdqu (%2), %%ymm0\n\tvmovups (%0), %%xmm1\n\tvmovups (%1), %%xmm2\n\t" \
                     mn " %%xmm2, %%xmm1, %%xmm0\n\tvmovdqu %%ymm0, (%3)\n\t" \
                     "vmovdqu (%2), %%ymm0\n\tvmovups (%0), %%xmm3\n\tvmovaps %%xmm3, %%xmm0\n\t" mn " %%xmm2, %%xmm0, %%xmm0\n\tvmovdqu %%ymm0, 32(%3)\n\t" \
                     "vmovdqu (%2), %%ymm0\n\t" mn " (%1), %%xmm1, %%xmm0\n\tvmovdqu %%ymm0, 64(%3)\n\t" \
                     "vmovdqu (%2), %%ymm0\n\tvmovups (%1), %%xmm4\n\tvmovaps %%xmm4, %%xmm0\n\t" mn " %%xmm0, %%xmm1, %%xmm0\n\tvmovdqu %%ymm0, 96(%3)\n\t" \
                     "vmovdqu (%2), %%ymm5\n\tvmovups (%0), %%xmm6\n\t" mn " %%xmm6, %%xmm6, %%xmm5\n\tvmovdqu %%ymm5, 128(%3)" \
                     :: "r"(a), "r"(b), "r"(IG), "r"(r) : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6"); \
}
#define FP2R(fn, mn) \
__attribute__((noinline)) static void fn(const u8 *a, const u8 *b, u8 *r) \
{ \
    __asm__ volatile("vmovdqu (%2), %%ymm0\n\tvmovups (%0), %%xmm1\n\tvmovups (%1), %%xmm2\n\t" \
                     mn " %%xmm2, %%xmm1, %%xmm0\n\tvmovdqu %%ymm0, (%3)\n\t" \
                     "vmovdqu (%2), %%ymm0\n\tvmovups (%0), %%xmm3\n\tvmovaps %%xmm3, %%xmm0\n\t" mn " %%xmm2, %%xmm0, %%xmm0\n\tvmovdqu %%ymm0, 32(%3)\n\t" \
                     "vmovdqu (%2), %%ymm0\n\tvmovups (%1), %%xmm4\n\tvmovaps %%xmm4, %%xmm0\n\t" mn " %%xmm0, %%xmm1, %%xmm0\n\tvmovdqu %%ymm0, 64(%3)\n\t" \
                     "vmovdqu (%2), %%ymm5\n\tvmovups (%0), %%xmm6\n\t" mn " %%xmm6, %%xmm6, %%xmm5\n\tvmovdqu %%ymm5, 96(%3)" \
                     :: "r"(a), "r"(b), "r"(IG), "r"(r) : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6"); \
}
#define FP2I(fn, mn, imm) \
__attribute__((noinline)) static void fn(const u8 *a, const u8 *b, u8 *r) \
{ \
    __asm__ volatile("vmovdqu (%2), %%ymm0\n\tvmovups (%0), %%xmm1\n\tvmovups (%1), %%xmm2\n\t" \
                     mn " $" #imm ", %%xmm2, %%xmm1, %%xmm0\n\tvmovdqu %%ymm0, (%3)\n\t" \
                     "vmovdqu (%2), %%ymm0\n\tvmovups (%0), %%xmm3\n\tvmovaps %%xmm3, %%xmm0\n\t" mn " $" #imm ", %%xmm2, %%xmm0, %%xmm0\n\tvmovdqu %%ymm0, 32(%3)\n\t" \
                     "vmovdqu (%2), %%ymm0\n\t" mn " $" #imm ", (%1), %%xmm1, %%xmm0\n\tvmovdqu %%ymm0, 64(%3)\n\t" \
                     "vmovdqu (%2), %%ymm5\n\tvmovups (%0), %%xmm6\n\t" mn " $" #imm ", %%xmm6, %%xmm6, %%xmm5\n\tvmovdqu %%ymm5, 96(%3)" \
                     :: "r"(a), "r"(b), "r"(IG), "r"(r) : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm5", "xmm6"); \
}

FP2(f_addss, "vaddss") FP2(f_subss, "vsubss") FP2(f_mulss, "vmulss") FP2(f_divss, "vdivss")
FP2(f_minss, "vminss") FP2(f_maxss, "vmaxss") FP2(f_sqrtss, "vsqrtss") FP2(f_cvtss2sd, "vcvtss2sd")
FP2(f_addsd, "vaddsd") FP2(f_subsd, "vsubsd") FP2(f_mulsd, "vmulsd") FP2(f_divsd, "vdivsd")
FP2(f_minsd, "vminsd") FP2(f_maxsd, "vmaxsd") FP2(f_sqrtsd, "vsqrtsd") FP2(f_cvtsd2ss, "vcvtsd2ss")
FP2(f_addps, "vaddps") FP2(f_subps, "vsubps") FP2(f_mulps, "vmulps") FP2(f_divps, "vdivps")
FP2(f_minps, "vminps") FP2(f_maxps, "vmaxps") FP2(f_unpcklps, "vunpcklps") FP2(f_unpckhps, "vunpckhps")
FP2(f_addpd, "vaddpd") FP2(f_subpd, "vsubpd") FP2(f_mulpd, "vmulpd") FP2(f_divpd, "vdivpd")
FP2(f_minpd, "vminpd") FP2(f_maxpd, "vmaxpd") FP2(f_unpcklpd, "vunpcklpd") FP2(f_unpckhpd, "vunpckhpd")
FP2(f_punpcklbw, "vpunpcklbw") FP2(f_punpcklwd, "vpunpcklwd") FP2(f_punpckldq, "vpunpckldq") FP2(f_punpcklqdq, "vpunpcklqdq")
FP2(f_punpckhbw, "vpunpckhbw") FP2(f_punpckhwd, "vpunpckhwd") FP2(f_punpckhdq, "vpunpckhdq") FP2(f_punpckhqdq, "vpunpckhqdq")
FP2(f_pshufb, "vpshufb")
FP2R(f_movlhps, "vmovlhps") FP2R(f_movhlps, "vmovhlps")
FP2I(f_roundss9, "vroundss", 9) FP2I(f_roundsd10, "vroundsd", 10)
FP2I(f_cmpss1, "vcmpss", 1) FP2I(f_cmpsd2, "vcmpsd", 2) FP2I(f_cmpss13, "vcmpss", 13) FP2I(f_cmpsd17, "vcmpsd", 17)

static const struct { const char *name; bfn fn; u8 kind, forms; } fp2[] = {
    { "vaddss", f_addss, 0, 5 }, { "vsubss", f_subss, 0, 5 }, { "vmulss", f_mulss, 0, 5 }, { "vdivss", f_divss, 0, 5 },
    { "vminss", f_minss, 0, 5 }, { "vmaxss", f_maxss, 0, 5 }, { "vsqrtss", f_sqrtss, 0, 5 }, { "vcvtss2sd", f_cvtss2sd, 0, 5 },
    { "vaddsd", f_addsd, 1, 5 }, { "vsubsd", f_subsd, 1, 5 }, { "vmulsd", f_mulsd, 1, 5 }, { "vdivsd", f_divsd, 1, 5 },
    { "vminsd", f_minsd, 1, 5 }, { "vmaxsd", f_maxsd, 1, 5 }, { "vsqrtsd", f_sqrtsd, 1, 5 }, { "vcvtsd2ss", f_cvtsd2ss, 1, 5 },
    { "vaddps", f_addps, 0, 5 }, { "vsubps", f_subps, 0, 5 }, { "vmulps", f_mulps, 0, 5 }, { "vdivps", f_divps, 0, 5 },
    { "vminps", f_minps, 0, 5 }, { "vmaxps", f_maxps, 0, 5 }, { "vunpcklps", f_unpcklps, 0, 5 }, { "vunpckhps", f_unpckhps, 0, 5 },
    { "vaddpd", f_addpd, 1, 5 }, { "vsubpd", f_subpd, 1, 5 }, { "vmulpd", f_mulpd, 1, 5 }, { "vdivpd", f_divpd, 1, 5 },
    { "vminpd", f_minpd, 1, 5 }, { "vmaxpd", f_maxpd, 1, 5 }, { "vunpcklpd", f_unpcklpd, 1, 5 }, { "vunpckhpd", f_unpckhpd, 1, 5 },
    { "vpunpcklbw", f_punpcklbw, 2, 5 }, { "vpunpcklwd", f_punpcklwd, 2, 5 }, { "vpunpckldq", f_punpckldq, 2, 5 }, { "vpunpcklqdq", f_punpcklqdq, 2, 5 },
    { "vpunpckhbw", f_punpckhbw, 2, 5 }, { "vpunpckhwd", f_punpckhwd, 2, 5 }, { "vpunpckhdq", f_punpckhdq, 2, 5 }, { "vpunpckhqdq", f_punpckhqdq, 2, 5 },
    { "vpshufb", f_pshufb, 2, 5 },
    { "vmovlhps", f_movlhps, 0, 4 }, { "vmovhlps", f_movhlps, 0, 4 },
    { "vroundss$9", f_roundss9, 0, 4 }, { "vroundsd$10", f_roundsd10, 1, 4 },
    { "vcmpss$1", f_cmpss1, 0, 4 }, { "vcmpsd$2", f_cmpsd2, 1, 4 }, { "vcmpss$13", f_cmpss13, 0, 4 }, { "vcmpsd$17", f_cmpsd17, 1, 4 },
};

__attribute__((noinline)) static void t_unary(u8 *r)
{
    __asm__ volatile("vmovdqu (%3), %%ymm0\n\tvsqrtps (%0), %%xmm0\n\tvmovdqu %%ymm0, (%4)\n\t"
                     "vmovdqu (%3), %%ymm1\n\tvmovupd (%1), %%xmm2\n\tvsqrtpd %%xmm2, %%xmm1\n\tvmovdqu %%ymm1, 32(%4)\n\t"
                     "vmovdqu (%3), %%ymm3\n\tvmovdqu (%2), %%xmm4\n\tvcvtdq2ps %%xmm4, %%xmm3\n\tvmovdqu %%ymm3, 64(%4)\n\t"
                     "vmovdqu (%3), %%ymm5\n\tvpshufd $0x1b, (%2), %%xmm5\n\tvmovdqu %%ymm5, 96(%4)\n\t"
                     "vmovdqu (%3), %%ymm6\n\tvroundps $1, (%0), %%xmm6\n\tvmovdqu %%ymm6, 128(%4)\n\t"
                     "vmovdqu (%3), %%ymm7\n\tvmovupd (%1), %%xmm8\n\tvroundpd $2, %%xmm8, %%xmm7\n\tvmovdqu %%ymm7, 160(%4)\n\t"
                     "vmovdqu (%3), %%ymm9\n\tvpmovsxbd %%xmm4, %%xmm9\n\tvmovdqu %%ymm9, 192(%4)\n\t"
                     "vmovdqu (%3), %%ymm10\n\tvpmovzxwq 2(%2), %%xmm10\n\tvmovdqu %%ymm10, 224(%4)"
                     :: "r"(FA), "r"(DA), "r"(IA), "r"(IG), "r"(r) : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4",
                     "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10");
}
__attribute__((noinline)) static void t_unary2(u8 *r)
{
    __asm__ volatile("vmovdqu (%3), %%ymm0\n\tvmovdqu (%2), %%xmm1\n\tvpmovsxbq %%xmm1, %%xmm0\n\tvmovdqu %%ymm0, (%4)\n\t"
                     "vmovdqu (%3), %%ymm2\n\tvpmovsxwq (%2), %%xmm2\n\tvmovdqu %%ymm2, 32(%4)\n\t"
                     "vmovdqu (%3), %%ymm3\n\tvpmovzxbd 5(%2), %%xmm3\n\tvmovdqu %%ymm3, 64(%4)\n\t"
                     "vmovdqu (%3), %%ymm4\n\tvpmovzxbq %%xmm1, %%xmm4\n\tvmovdqu %%ymm4, 96(%4)\n\t"
                     "vmovdqu (%3), %%ymm5\n\tvmovdqu (%2), %%xmm5\n\tvpshufd $0xa7, %%xmm5, %%xmm5\n\tvmovdqu %%ymm5, 128(%4)\n\t"
                     "vmovdqu (%3), %%ymm6\n\tvsqrtpd (%1), %%xmm6\n\tvmovdqu %%ymm6, 160(%4)"
                     :: "r"(FA), "r"(DA), "r"(IA), "r"(IG), "r"(r) : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6");
}

__attribute__((noinline)) static void t_mov(u8 *r)
{
    __asm__ volatile("vmovdqu (%2), %%ymm0\n\tvmovups (%0), %%xmm1\n\tvmovups (%1), %%xmm2\n\tvmovss %%xmm2, %%xmm1, %%xmm0\n\tvmovdqu %%ymm0, (%3)\n\t"
                     "vmovdqu (%2), %%ymm0\n\tvmovss 4(%1), %%xmm0\n\tvmovdqu %%ymm0, 32(%3)\n\t"
                     "vmovdqu (%2), %%ymm0\n\tvmovsd %%xmm2, %%xmm1, %%xmm0\n\tvmovdqu %%ymm0, 64(%3)\n\t"
                     "vmovdqu (%2), %%ymm0\n\tvmovsd 8(%0), %%xmm0\n\tvmovdqu %%ymm0, 96(%3)\n\t"
                     "vmovdqu (%2), %%ymm3\n\tvmovlps 8(%1), %%xmm1, %%xmm3\n\tvmovdqu %%ymm3, 128(%3)\n\t"
                     "vmovdqu (%2), %%ymm4\n\tvmovhps 16(%1), %%xmm1, %%xmm4\n\tvmovdqu %%ymm4, 160(%3)\n\t"
                     "vmovdqu (%2), %%ymm5\n\tvmovaps %%xmm1, %%xmm5\n\tvmovss %%xmm2, %%xmm5, %%xmm5\n\tvmovdqu %%ymm5, 192(%3)\n\t"
                     "vmovss %%xmm2, 224(%3)\n\tvmovsd %%xmm1, 228(%3)\n\tvmovlps %%xmm2, 236(%3)\n\tvmovhps %%xmm1, 244(%3)"
                     :: "r"(FA), "r"(FB), "r"(IG), "r"(r) : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5");
}

__attribute__((noinline)) static void t_gpr(u8 *r)
{
    u64 q = 0x8123456789abcdefull, d = 0xfedcba98u;
    __asm__ volatile("vmovdqu (%2), %%ymm0\n\tvmovd %k0, %%xmm0\n\tvmovdqu %%ymm0, (%3)\n\t"
                     "vmovdqu (%2), %%ymm1\n\tvmovq %1, %%xmm1\n\tvmovdqu %%ymm1, 32(%3)\n\t"
                     "vmovdqu (%2), %%ymm2\n\tvmovd 4(%2), %%xmm2\n\tvmovdqu %%ymm2, 64(%3)\n\t"
                     "vmovdqu (%2), %%ymm3\n\tvmovq 8(%2), %%xmm3\n\tvmovdqu %%ymm3, 96(%3)\n\t"
                     "vmovdqu (%2), %%ymm4\n\tvmovq %%xmm1, %%xmm4\n\tvmovdqu %%ymm4, 128(%3)\n\t"
                     "vmovq %%xmm1, 160(%3)\n\tvmovd %%xmm0, 168(%3)\n\tvmovq %%xmm3, %%rax\n\tmovq %%rax, 172(%3)\n\tvmovd %%xmm2, %%eax\n\tmovq %%rax, 180(%3)\n\t"
                     "vmovdqu (%2), %%ymm5\n\tvcvtsi2ss %k0, %%xmm2, %%xmm5\n\tvmovdqu %%ymm5, 192(%3)\n\t"
                     "vmovdqu (%2), %%ymm6\n\tvcvtsi2sd %1, %%xmm2, %%xmm6\n\tvmovdqu %%ymm6, 224(%3)"
                     :: "r"(d), "r"(q), "r"(IG), "r"(r) : "memory", "rax", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6");
}
__attribute__((noinline)) static void t_gpr2(u8 *r)
{
    __asm__ volatile("vmovdqu (%2), %%ymm5\n\tvcvtsi2sdl 4(%3), %%xmm5, %%xmm5\n\tvmovdqu %%ymm5, (%4)\n\t"
                     "vmovdqu (%2), %%ymm6\n\tvcvtsi2ssq 8(%3), %%xmm2, %%xmm6\n\tvmovdqu %%ymm6, 32(%4)\n\t"
                     "vmovups (%0), %%xmm1\n\tvcvttss2si %%xmm1, %%eax\n\tmovq %%rax, 64(%4)\n\tvcvttss2si 12(%0), %%rax\n\tmovq %%rax, 72(%4)\n\t"
                     "vmovupd (%1), %%xmm2\n\tvcvttsd2si %%xmm2, %%eax\n\tmovq %%rax, 80(%4)\n\tvcvttsd2si 24(%1), %%rax\n\tmovq %%rax, 88(%4)\n\t"
                     "vcvttsd2si 16(%1), %%rax\n\tmovq %%rax, 96(%4)\n\tvcvttss2si 16(%0), %%eax\n\tmovq %%rax, 104(%4)\n\t"
                     "vmovdqu (%2), %%ymm7\n\tvmovdqu (%3), %%xmm8\n\tvpinsrb $3, 1(%3), %%xmm8, %%xmm7\n\tvmovdqu %%ymm7, 128(%4)\n\t"
                     "vmovdqu (%2), %%ymm9\n\tmovl $0x1234abcd, %%eax\n\tvpinsrw $5, %%eax, %%xmm8, %%xmm9\n\tvmovdqu %%ymm9, 160(%4)\n\t"
                     "vmovdqu (%2), %%ymm10\n\tvpinsrd $1, %%eax, %%xmm8, %%xmm10\n\tvmovdqu %%ymm10, 192(%4)\n\t"
                     "vmovdqu (%2), %%ymm11\n\tvpinsrq $1, 8(%3), %%xmm8, %%xmm11\n\tvmovdqu %%ymm11, 224(%4)"
                     :: "r"(FA), "r"(DA), "r"(IG), "r"(IA), "r"(r) : "memory", "rax", "xmm1", "xmm2", "xmm5", "xmm6", "xmm7", "xmm8",
                     "xmm9", "xmm10", "xmm11");
}
__attribute__((noinline)) static void t_extr(u64 *o)
{
    __asm__ volatile("vmovdqu (%0), %%xmm1\n\tmovq $-1, %%rax\n\tvpextrb $7, %%xmm1, %%eax\n\tmovq %%rax, (%1)\n\t"
                     "movq $-1, %%rax\n\tvpextrw $9, %%xmm1, %%eax\n\tmovq %%rax, 8(%1)\n\t"
                     "movq $-1, %%rax\n\tvpextrd $3, %%xmm1, %%eax\n\tmovq %%rax, 16(%1)\n\t"
                     "vpextrq $1, %%xmm1, %%rax\n\tmovq %%rax, 24(%1)\n\t"
                     "movq $-1, 32(%1)\n\tvpextrd $2, %%xmm1, 32(%1)\n\t"
                     "movq $-1, 40(%1)\n\tvpextrb $15, %%xmm1, 40(%1)"
                     :: "r"(IA), "r"(o) : "memory", "rax", "xmm1");
}
__attribute__((noinline)) static void t_comi(u64 *o)
{
    __asm__ volatile("vmovups (%0), %%xmm1\n\tvmovups (%1), %%xmm2\n\tvucomiss %%xmm2, %%xmm1\n\tpushfq\n\tpopq %%rax\n\tmovq %%rax, (%2)\n\t"
                     "vcomiss 8(%1), %%xmm1\n\tpushfq\n\tpopq %%rax\n\tmovq %%rax, 8(%2)\n\t"
                     "vmovups 8(%0), %%xmm3\n\tvucomiss %%xmm3, %%xmm3\n\tpushfq\n\tpopq %%rax\n\tmovq %%rax, 16(%2)\n\t"
                     "vmovupd (%3), %%xmm4\n\tvucomisd 16(%3), %%xmm4\n\tpushfq\n\tpopq %%rax\n\tmovq %%rax, 24(%2)\n\t"
                     "vcomisd %%xmm4, %%xmm4\n\tsetae %%al\n\tsetp %%ah\n\tmovzwl %%ax, %%eax\n\tmovq %%rax, 32(%2)"
                     :: "r"(FA), "r"(FB), "r"(o), "r"(DA) : "memory", "rax", "xmm1", "xmm2", "xmm3", "xmm4", "cc");
}

static void hex64s(const char *name, const u64 *v, int n)
{
    u8 buf[32];
    for (int i = 0; i < n; i += 4) {
        for (int j = 0; j < 4; j++) wr64(buf + 8 * j, i + j < n ? v[i + j] : 0);
        hexline(name, i / 4, buf, 32);
    }
}

int main(void)
{
    init();
    for (int pass = 0; pass < 2; pass++) {
        int last = pass == 1;
        for (unsigned t = 0; t < sizeof fp2 / sizeof fp2[0]; t++) {
            const u8 *a1 = fp2[t].kind == 1 ? DA : fp2[t].kind == 2 ? IA : FA;
            const u8 *b1 = fp2[t].kind == 1 ? DB : fp2[t].kind == 2 ? IB : FB;
            const u8 *a2 = fp2[t].kind == 1 ? DC : fp2[t].kind == 2 ? IB : FC;
            const u8 *b2 = fp2[t].kind == 1 ? DD : fp2[t].kind == 2 ? IA : FD;
            fill(R, 256, 0xee);
            fp2[t].fn(a1, b1, R);
            if (last) dump(fp2[t].name, 0, fp2[t].forms);
            fill(R, 256, 0xee);
            fp2[t].fn(a2, b2, R);
            if (last) dump(fp2[t].name, 5, fp2[t].forms);
        }
        fill(R, 256, 0xee); t_unary(R); if (last) dump("unary", 0, 8);
        fill(R, 256, 0xee); t_unary2(R); if (last) dump("unary2", 0, 6);
        fill(R, 256, 0xee); t_mov(R); if (last) dump("mov", 0, 8);
        fill(R, 256, 0xee); t_gpr(R); if (last) dump("gpr", 0, 8);
        fill(R, 256, 0xee); t_gpr2(R); if (last) dump("gpr2", 0, 8);
        u64 o[8];
        for (int i = 0; i < 8; i++) o[i] = 0;
        t_extr(o); if (last) hex64s("pextr", o, 6);
        for (int i = 0; i < 8; i++) o[i] = 0;
        t_comi(o);
        for (int i = 0; i < 4; i++) o[i] &= 0x8d5;
        if (last) hex64s("comi", o, 5);
    }
    g_puts("DONE\n");
    return 0;
}

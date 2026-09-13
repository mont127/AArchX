#include "gsys.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

static u8 F1[32] __attribute__((aligned(32)));
static u8 F2[32] __attribute__((aligned(32)));
static u8 F3[32] __attribute__((aligned(32)));
static u8 G1[32] __attribute__((aligned(32)));
static u8 G2[32] __attribute__((aligned(32)));
static u8 G3[32] __attribute__((aligned(32)));
static u8 D1[32] __attribute__((aligned(32)));
static u8 D2[32] __attribute__((aligned(32)));
static u8 D3[32] __attribute__((aligned(32)));
static u8 E1[32] __attribute__((aligned(32)));
static u8 E2[32] __attribute__((aligned(32)));
static u8 E3[32] __attribute__((aligned(32)));
static u8 R[128] __attribute__((aligned(64)));

static void wr32(u8 *p, u32 v) { for (int i = 0; i < 4; i++) p[i] = (u8)(v >> (8 * i)); }
static void wr64(u8 *p, u64 v) { for (int i = 0; i < 8; i++) p[i] = (u8)(v >> (8 * i)); }
static void fill(u8 *p, int n, u8 v) { for (int i = 0; i < n; i++) p[i] = v; }

static void init(void)
{
    static const u32 f1[8] = { 0x3fc00000u, 0xc0100000u, 0x40400000u, 0x3e000000u, 0x50000000u, 0xc0f00000u, 0x40000000u, 0x3a83126fu };
    static const u32 f2[8] = { 0x40200000u, 0x3f000000u, 0xbf400000u, 0x41200000u, 0x2f000000u, 0x3fc00000u, 0xc0000000u, 0x42c80000u };
    static const u32 f3[8] = { 0x3f800000u, 0xc0200000u, 0x40800000u, 0xbe800000u, 0x3fe00000u, 0x40f00000u, 0xc0800000u, 0x3f000000u };
    static const u32 g1[8] = { 0x7fc00123u, 0x7f800000u, 0x00000000u, 0x7f800011u, 0x80000000u, 0x3f800000u, 0x00000001u, 0xff800000u };
    static const u32 g2[8] = { 0x3f800000u, 0x00000000u, 0x7f800000u, 0x40000000u, 0xffc00456u, 0x7f800022u, 0x80000000u, 0x3f800000u };
    static const u32 g3[8] = { 0xffc00789u, 0xff800000u, 0x3f800000u, 0x7fc00abcu, 0x3f800000u, 0x80000000u, 0x00000001u, 0x7f800000u };
    static const u64 d1[4] = { 0x3ff8000000000000ull, 0xc002000000000000ull, 0x4008000000000000ull, 0x3fc0000000000000ull };
    static const u64 d2[4] = { 0x4004000000000000ull, 0x3fe0000000000000ull, 0xbfe8000000000000ull, 0x4024000000000000ull };
    static const u64 d3[4] = { 0x3ff0000000000000ull, 0xc004000000000000ull, 0x4010000000000000ull, 0xbfd0000000000000ull };
    static const u64 e1[4] = { 0x7ff8000000000123ull, 0x7ff0000000000000ull, 0x0000000000000000ull, 0x7ff0000000000011ull };
    static const u64 e2[4] = { 0x3ff0000000000000ull, 0x0000000000000000ull, 0xfff0000000000000ull, 0xfff8000000000456ull };
    static const u64 e3[4] = { 0xfff8000000000789ull, 0xfff0000000000000ull, 0x7ff0000000000000ull, 0x3ff0000000000000ull };
    for (int i = 0; i < 8; i++) {
        wr32(F1 + 4 * i, f1[i]); wr32(F2 + 4 * i, f2[i]); wr32(F3 + 4 * i, f3[i]);
        wr32(G1 + 4 * i, g1[i]); wr32(G2 + 4 * i, g2[i]); wr32(G3 + 4 * i, g3[i]);
    }
    for (int i = 0; i < 4; i++) {
        wr64(D1 + 8 * i, d1[i]); wr64(D2 + 8 * i, d2[i]); wr64(D3 + 8 * i, d3[i]);
        wr64(E1 + 8 * i, e1[i]); wr64(E2 + 8 * i, e2[i]); wr64(E3 + 8 * i, e3[i]);
    }
}

static void hexline(const char *name, int form, const u8 *p)
{
    static const char hx[] = "0123456789abcdef";
    char buf[80];
    int k = 0;
    g_puts(name);
    buf[k++] = ' '; buf[k++] = (char)('0' + form); buf[k++] = ':'; buf[k++] = ' ';
    for (int i = 0; i < 32; i++) { buf[k++] = hx[p[i] >> 4]; buf[k++] = hx[p[i] & 15]; }
    buf[k++] = '\n';
    sys_write(1, buf, (g_u64)k);
}

typedef void (*f3fn)(const u8 *, const u8 *, const u8 *, u8 *);

#define FMAY(fn, mn) \
__attribute__((noinline)) static void fn(const u8 *x, const u8 *y, const u8 *z, u8 *r) \
{ \
    __asm__ volatile("vmovups (%0), %%ymm0\n\tvmovups (%1), %%ymm1\n\tvmovups (%2), %%ymm2\n\t" \
                     mn " %%ymm2, %%ymm1, %%ymm0\n\tvmovups %%ymm0, (%3)\n\t" \
                     "vmovups (%0), %%ymm3\n\t" mn " (%2), %%ymm1, %%ymm3\n\tvmovups %%ymm3, 32(%3)\n\t" \
                     "vmovups (%1), %%ymm4\n\t" mn " %%ymm4, %%ymm4, %%ymm4\n\tvmovups %%ymm4, 64(%3)" \
                     :: "r"(x), "r"(y), "r"(z), "r"(r) : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4"); \
}
#define FMAX(fn, mn) \
__attribute__((noinline)) static void fn(const u8 *x, const u8 *y, const u8 *z, u8 *r) \
{ \
    __asm__ volatile("vmovups (%0), %%ymm0\n\tvmovups (%1), %%ymm1\n\tvmovups (%2), %%ymm2\n\t" \
                     mn " %%xmm2, %%xmm1, %%xmm0\n\tvmovups %%ymm0, (%3)\n\t" \
                     "vmovups (%0), %%ymm3\n\t" mn " (%2), %%xmm1, %%xmm3\n\tvmovups %%ymm3, 32(%3)\n\t" \
                     "vmovups (%1), %%ymm4\n\t" mn " %%xmm4, %%xmm4, %%xmm4\n\tvmovups %%ymm4, 64(%3)" \
                     :: "r"(x), "r"(y), "r"(z), "r"(r) : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4"); \
}
#define FMSET(p, suf, M) \
    M(p##_fmadd132, "vfmadd132" suf) M(p##_fmadd213, "vfmadd213" suf) M(p##_fmadd231, "vfmadd231" suf) \
    M(p##_fmsub132, "vfmsub132" suf) M(p##_fmsub213, "vfmsub213" suf) M(p##_fmsub231, "vfmsub231" suf) \
    M(p##_fnmadd132, "vfnmadd132" suf) M(p##_fnmadd213, "vfnmadd213" suf) M(p##_fnmadd231, "vfnmadd231" suf) \
    M(p##_fnmsub132, "vfnmsub132" suf) M(p##_fnmsub213, "vfnmsub213" suf) M(p##_fnmsub231, "vfnmsub231" suf)
FMSET(yps, "ps", FMAY) FMSET(ypd, "pd", FMAY)
FMSET(xps, "ps", FMAX) FMSET(xpd, "pd", FMAX)
FMSET(xss, "ss", FMAX) FMSET(xsd, "sd", FMAX)

#define FMENT(p, tag, suf, dbl) \
    { tag "vfmadd132" suf, p##_fmadd132, dbl }, { tag "vfmadd213" suf, p##_fmadd213, dbl }, { tag "vfmadd231" suf, p##_fmadd231, dbl }, \
    { tag "vfmsub132" suf, p##_fmsub132, dbl }, { tag "vfmsub213" suf, p##_fmsub213, dbl }, { tag "vfmsub231" suf, p##_fmsub231, dbl }, \
    { tag "vfnmadd132" suf, p##_fnmadd132, dbl }, { tag "vfnmadd213" suf, p##_fnmadd213, dbl }, { tag "vfnmadd231" suf, p##_fnmadd231, dbl }, \
    { tag "vfnmsub132" suf, p##_fnmsub132, dbl }, { tag "vfnmsub213" suf, p##_fnmsub213, dbl }, { tag "vfnmsub231" suf, p##_fnmsub231, dbl },
static const struct { const char *name; f3fn fn; int dbl; } fmas[] = {
    FMENT(yps, "y.", "ps", 0) FMENT(ypd, "y.", "pd", 1)
    FMENT(xps, "x.", "ps", 0) FMENT(xpd, "x.", "pd", 1)
    FMENT(xss, "x.", "ss", 0) FMENT(xsd, "x.", "sd", 1)
};

typedef void (*f2fn)(const u8 *, const u8 *, u8 *);
#define FPY(fn, mn) \
__attribute__((noinline)) static void fn(const u8 *x, const u8 *y, u8 *r) \
{ \
    __asm__ volatile("vmovups (%0), %%ymm1\n\tvmovups (%1), %%ymm2\n\t" \
                     mn " %%ymm2, %%ymm1, %%ymm0\n\tvmovups %%ymm0, (%2)\n\t" \
                     "vmovups %%ymm1, %%ymm3\n\t" mn " %%ymm2, %%ymm3, %%ymm3\n\tvmovups %%ymm3, 32(%2)\n\t" \
                     mn " (%1), %%ymm1, %%ymm4\n\tvmovups %%ymm4, 64(%2)\n\t" \
                     "vmovups %%ymm2, %%ymm5\n\t" mn " %%ymm5, %%ymm1, %%ymm5\n\tvmovups %%ymm5, 96(%2)" \
                     :: "r"(x), "r"(y), "r"(r) : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5"); \
}
#define SQY(fn, mn) \
__attribute__((noinline)) static void fn(const u8 *x, const u8 *y, u8 *r) \
{ \
    __asm__ volatile("vmovups (%0), %%ymm1\n\t" mn " %%ymm1, %%ymm0\n\tvmovups %%ymm0, (%2)\n\t" \
                     mn " (%1), %%ymm2\n\tvmovups %%ymm2, 32(%2)\n\t" \
                     "vmovups (%1), %%ymm3\n\t" mn " %%ymm3, %%ymm3\n\tvmovups %%ymm3, 64(%2)\n\t" \
                     "vmovups (%0), %%ymm4\n\t" mn " %%ymm1, %%ymm4\n\tvmovups %%ymm4, 96(%2)" \
                     :: "r"(x), "r"(y), "r"(r) : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4"); \
}
FPY(y_addps, "vaddps") FPY(y_subps, "vsubps") FPY(y_mulps, "vmulps") FPY(y_divps, "vdivps")
FPY(y_minps, "vminps") FPY(y_maxps, "vmaxps") SQY(y_sqrtps, "vsqrtps")
FPY(y_addpd, "vaddpd") FPY(y_subpd, "vsubpd") FPY(y_mulpd, "vmulpd") FPY(y_divpd, "vdivpd")
FPY(y_minpd, "vminpd") FPY(y_maxpd, "vmaxpd") SQY(y_sqrtpd, "vsqrtpd")
static const struct { const char *name; f2fn fn; int dbl; } fps[] = {
    { "y.vaddps", y_addps, 0 }, { "y.vsubps", y_subps, 0 }, { "y.vmulps", y_mulps, 0 }, { "y.vdivps", y_divps, 0 },
    { "y.vminps", y_minps, 0 }, { "y.vmaxps", y_maxps, 0 }, { "y.vsqrtps", y_sqrtps, 0 },
    { "y.vaddpd", y_addpd, 1 }, { "y.vsubpd", y_subpd, 1 }, { "y.vmulpd", y_mulpd, 1 }, { "y.vdivpd", y_divpd, 1 },
    { "y.vminpd", y_minpd, 1 }, { "y.vmaxpd", y_maxpd, 1 }, { "y.vsqrtpd", y_sqrtpd, 1 },
};

__attribute__((target("avx2,fma"), noinline)) static void saxpy_fma(float a, const float *x, float *y, int n)
{
    for (int i = 0; i < n; i++) y[i] = a * x[i] + y[i];
}
__attribute__((target("avx2,fma"), noinline)) static double dot_fma(const double *x, const double *y, int n)
{
    double s = 0;
    for (int i = 0; i < n; i++) s = s * 0.5 + x[i] * y[i];
    return s;
}
static float SX[1000], SY[1000];
static double DX[1000], DY[1000];

int main(void)
{
    init();
    for (int pass = 0; pass < 2; pass++) {
        int last = pass == 1;
        for (unsigned t = 0; t < sizeof fmas / sizeof fmas[0]; t++) {
            const u8 *n1 = fmas[t].dbl ? D1 : F1, *n2 = fmas[t].dbl ? D2 : F2, *n3 = fmas[t].dbl ? D3 : F3;
            const u8 *s1 = fmas[t].dbl ? E1 : G1, *s2 = fmas[t].dbl ? E2 : G2, *s3 = fmas[t].dbl ? E3 : G3;
            fill(R, 128, 0xee);
            fmas[t].fn(n1, n2, n3, R);
            if (last) for (int f = 0; f < 3; f++) hexline(fmas[t].name, f, R + 32 * f);
            fill(R, 128, 0xee);
            fmas[t].fn(s1, s2, s3, R);
            if (last) for (int f = 0; f < 3; f++) hexline(fmas[t].name, 3 + f, R + 32 * f);
            fill(R, 128, 0xee);
            fmas[t].fn(s3, n1, s2, R);
            if (last) for (int f = 0; f < 3; f++) hexline(fmas[t].name, 6 + f, R + 32 * f);
        }
        for (unsigned t = 0; t < sizeof fps / sizeof fps[0]; t++) {
            const u8 *n1 = fps[t].dbl ? D1 : F1, *n2 = fps[t].dbl ? D2 : F2;
            const u8 *s1 = fps[t].dbl ? E1 : G1, *s2 = fps[t].dbl ? E2 : G2;
            fill(R, 128, 0xee);
            fps[t].fn(n1, n2, R);
            if (last) for (int f = 0; f < 4; f++) hexline(fps[t].name, f, R + 32 * f);
            fill(R, 128, 0xee);
            fps[t].fn(s1, s2, R);
            if (last) for (int f = 0; f < 4; f++) hexline(fps[t].name, 4 + f, R + 32 * f);
        }
        for (int i = 0; i < 1000; i++) { SX[i] = (float)i * 0.5f - 100.0f; SY[i] = (float)(i % 7) * 1.25f; DX[i] = (double)i / 3.0; DY[i] = 1.0 - (double)i / 7.0; }
        SY[500] = (float)(0.0 / 0.0);
        saxpy_fma(1.5f, SX, SY, 1000);
        double dsum = dot_fma(DX, DY, 1000);
        if (last) {
            u8 buf[32];
            for (int k = 0; k < 8; k++) {
                u32 v; u8 *p = (u8 *)&SY[k * 125 + 3];
                v = (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
                wr32(buf + 4 * k, v);
            }
            hexline("loop.saxpy_fma", 0, buf);
            fill(buf, 32, 0);
            u8 *q = (u8 *)&dsum;
            for (int k = 0; k < 8; k++) buf[k] = q[k];
            q = (u8 *)&SY[500];
            for (int k = 0; k < 4; k++) buf[8 + k] = q[k];
            hexline("loop.dot_fma", 0, buf);
        }
    }
    g_puts("DONE\n");
    return 0;
}

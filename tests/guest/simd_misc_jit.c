#include "gsys.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

static u8 FA[32] __attribute__((aligned(32)));
static u8 FB[32] __attribute__((aligned(32)));
static u8 DA[32] __attribute__((aligned(32)));
static u8 DB[32] __attribute__((aligned(32)));
static u8 IA[32] __attribute__((aligned(32)));
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
    for (int i = 0; i < 8; i++) { wr32(FA + 4 * i, fa[i]); wr32(FB + 4 * i, fb[i]); }
    for (int i = 0; i < 4; i++) { wr64(DA + 8 * i, da[i]); wr64(DB + 8 * i, db[i]); }
    for (int i = 0; i < 32; i++) { IA[i] = (u8)(0x80 ^ (i * 37 + 11)); IG[i] = (u8)(0xa5 ^ (i * 29)); }
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
static void dump(const char *name, int lines)
{
    for (int f = 0; f < lines; f++) hexline(name, f, R + 32 * f);
}

__attribute__((noinline)) static void t_movddup(u8 *r)
{
    __asm__ volatile("movupd (%0), %%xmm1\n\tmovddup %%xmm1, %%xmm0\n\tmovupd %%xmm0, (%2)\n\t"
                     "movddup 8(%0), %%xmm2\n\tmovupd %%xmm2, 16(%2)\n\t"
                     "movupd 16(%0), %%xmm3\n\tmovddup %%xmm3, %%xmm3\n\tmovupd %%xmm3, 32(%2)\n\t"
                     "movupd (%0), %%xmm12\n\tmovddup %%xmm12, %%xmm13\n\tmovupd %%xmm13, 48(%2)\n\t"
                     "vmovdqu (%1), %%ymm4\n\tvmovddup %%xmm1, %%xmm4\n\tvmovdqu %%ymm4, 64(%2)\n\t"
                     "vmovdqu (%1), %%ymm5\n\tvmovddup 16(%0), %%xmm5\n\tvmovdqu %%ymm5, 96(%2)\n\t"
                     "vmovupd (%0), %%ymm6\n\tvmovddup %%ymm6, %%ymm7\n\tvmovdqu %%ymm7, 128(%2)\n\t"
                     "vmovddup (%0), %%ymm8\n\tvmovdqu %%ymm8, 160(%2)\n\t"
                     "vmovupd (%1), %%ymm9\n\tvmovddup %%ymm9, %%ymm9\n\tvmovdqu %%ymm9, 192(%2)"
                     :: "r"(DA), "r"(IG), "r"(r) : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5",
                     "xmm6", "xmm7", "xmm8", "xmm9", "xmm12", "xmm13");
}

typedef void (*f2fn)(const u8 *, const u8 *, u8 *);
#define SHUF(fn, sfx, imm) \
__attribute__((noinline)) static void fn(const u8 *a, const u8 *b, u8 *r) \
{ \
    __asm__ volatile("movups (%0), %%xmm1\n\tmovups (%1), %%xmm2\n\tshuf" sfx " $" #imm ", %%xmm2, %%xmm1\n\tmovups %%xmm1, (%3)\n\t" \
                     "movups (%0), %%xmm3\n\tshuf" sfx " $" #imm ", (%1), %%xmm3\n\tmovups %%xmm3, 16(%3)\n\t" \
                     "movups (%1), %%xmm4\n\tshuf" sfx " $" #imm ", %%xmm4, %%xmm4\n\tmovups %%xmm4, 32(%3)\n\t" \
                     "vmovdqu (%2), %%ymm5\n\tvmovups (%0), %%xmm6\n\tvshuf" sfx " $" #imm ", %%xmm2, %%xmm6, %%xmm5\n\tvmovdqu %%ymm5, 64(%3)\n\t" \
                     "vmovdqu (%2), %%ymm7\n\tvshuf" sfx " $" #imm ", 8(%1), %%xmm6, %%xmm7\n\tvmovdqu %%ymm7, 96(%3)\n\t" \
                     "vmovdqu (%2), %%ymm8\n\tvmovups (%1), %%xmm8\n\tvshuf" sfx " $" #imm ", %%xmm8, %%xmm6, %%xmm8\n\tvmovdqu %%ymm8, 128(%3)\n\t" \
                     "vmovups (%0), %%ymm9\n\tvmovups (%1), %%ymm10\n\tvshuf" sfx " $" #imm ", %%ymm10, %%ymm9, %%ymm11\n\tvmovdqu %%ymm11, 160(%3)\n\t" \
                     "vshuf" sfx " $" #imm ", (%1), %%ymm9, %%ymm12\n\tvmovdqu %%ymm12, 192(%3)\n\t" \
                     "vmovdqu %%ymm10, %%ymm13\n\tvshuf" sfx " $" #imm ", %%ymm13, %%ymm9, %%ymm13\n\tvmovdqu %%ymm13, 224(%3)" \
                     :: "r"(a), "r"(b), "r"(IG), "r"(r) : "memory", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", \
                     "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13"); \
}
SHUF(s_ps1b, "ps", 0x1b) SHUF(s_psc6, "ps", 0xc6) SHUF(s_ps72, "ps", 0x72)
SHUF(s_pd1, "pd", 0x1) SHUF(s_pd2, "pd", 0x2) SHUF(s_pdd, "pd", 0xd)

#define INSP(fn, imm) \
__attribute__((noinline)) static void fn(const u8 *a, const u8 *b, u8 *r) \
{ \
    __asm__ volatile("movups (%0), %%xmm1\n\tmovups (%1), %%xmm2\n\tinsertps $" #imm ", %%xmm2, %%xmm1\n\tmovups %%xmm1, (%3)\n\t" \
                     "movups (%0), %%xmm3\n\tinsertps $" #imm ", 4(%1), %%xmm3\n\tmovups %%xmm3, 16(%3)\n\t" \
                     "movups (%1), %%xmm4\n\tinsertps $" #imm ", %%xmm4, %%xmm4\n\tmovups %%xmm4, 32(%3)\n\t" \
                     "vmovdqu (%2), %%ymm5\n\tvmovups (%0), %%xmm6\n\tvinsertps $" #imm ", %%xmm2, %%xmm6, %%xmm5\n\tvmovdqu %%ymm5, 64(%3)\n\t" \
                     "vmovdqu (%2), %%ymm7\n\tvinsertps $" #imm ", 8(%1), %%xmm6, %%xmm7\n\tvmovdqu %%ymm7, 96(%3)\n\t" \
                     "vmovdqu (%2), %%ymm8\n\tvmovups (%1), %%xmm8\n\tvinsertps $" #imm ", %%xmm8, %%xmm6, %%xmm8\n\tvmovdqu %%ymm8, 128(%3)" \
                     :: "r"(a), "r"(b), "r"(IG), "r"(r) : "memory", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8"); \
}
INSP(i_30, 0x30) INSP(i_4a, 0x4a) INSP(i_d5, 0xd5) INSP(i_0f, 0x0f) INSP(i_e0, 0xe0)

static const struct { const char *name; f2fn fn; int lines; } shufs[] = {
    { "shufps$0x1b", s_ps1b, 8 }, { "shufps$0xc6", s_psc6, 8 }, { "shufps$0x72", s_ps72, 8 },
    { "shufpd$0x1", s_pd1, 8 }, { "shufpd$0x2", s_pd2, 8 }, { "shufpd$0xd", s_pdd, 8 },
    { "insertps$0x30", i_30, 5 }, { "insertps$0x4a", i_4a, 5 }, { "insertps$0xd5", i_d5, 5 },
    { "insertps$0x0f", i_0f, 5 }, { "insertps$0xe0", i_e0, 5 },
};

typedef void (*f1fn)(u8 *);
#define LSH(fn, mn, c) \
__attribute__((noinline)) static void fn(u8 *r) \
{ \
    __asm__ volatile("movdqu (%0), %%xmm1\n\t" mn " $" #c ", %%xmm1\n\tmovdqu %%xmm1, (%1)\n\t" \
                     "movdqu 16(%0), %%xmm14\n\t" mn " $" #c ", %%xmm14\n\tmovdqu %%xmm14, 16(%1)" \
                     :: "r"(IA), "r"(r) : "memory", "xmm1", "xmm14"); \
}
#define LSHSET(p, mn) LSH(p##_0, mn, 0) LSH(p##_1, mn, 1) LSH(p##_7, mn, 7) LSH(p##_15, mn, 15) LSH(p##_16, mn, 16) \
                      LSH(p##_31, mn, 31) LSH(p##_32, mn, 32) LSH(p##_63, mn, 63) LSH(p##_64, mn, 64) LSH(p##_255, mn, 255)
LSHSET(sllw, "psllw") LSHSET(slld, "pslld") LSHSET(sllq, "psllq")
LSHSET(srlw, "psrlw") LSHSET(srld, "psrld") LSHSET(srlq, "psrlq")
LSHSET(sraw, "psraw") LSHSET(srad, "psrad")
#define LSHENT(p, mn) { mn " $0", p##_0 }, { mn " $1", p##_1 }, { mn " $7", p##_7 }, { mn " $15", p##_15 }, { mn " $16", p##_16 }, \
                      { mn " $31", p##_31 }, { mn " $32", p##_32 }, { mn " $63", p##_63 }, { mn " $64", p##_64 }, { mn " $255", p##_255 },
static const struct { const char *name; f1fn fn; } lshifts[] = {
    LSHENT(sllw, "psllw") LSHENT(slld, "pslld") LSHENT(sllq, "psllq")
    LSHENT(srlw, "psrlw") LSHENT(srld, "psrld") LSHENT(srlq, "psrlq")
    LSHENT(sraw, "psraw") LSHENT(srad, "psrad")
};

__attribute__((noinline)) static void t_movnti(u8 *r)
{
    u64 v = 0x8877665544332211ull, ix = 3;
    __asm__ volatile("movnti %k0, (%1)\n\tmovnti %0, 4(%1)\n\tmovnti %0, 16(%1,%2,4)"
                     :: "r"(v), "r"(r), "r"(ix) : "memory");
}

typedef struct { double x, y, z, vx, vy, vz, m; } Body;
#define NBODY(name, attr) \
attr static double name(int steps) \
{ \
    Body b[3] = { { 0, 0, 0, 0, 0, 0, 39.47841760435743 }, \
                  { 4.84143144246472090, -1.16032004402742839, -0.103622044471123109, 0.606326392995832, 2.81198684491626, -0.02521836165988763, 0.037693674870389 }, \
                  { 8.34336671824457987, 4.12479856412430479, -0.403523417114321381, -1.01077434617730, 1.82566237123041, 0.008415761376584, 0.011286326131968 } }; \
    double dt = 0.01; \
    for (int s = 0; s < steps; s++) { \
        for (int i = 0; i < 3; i++) \
            for (int j = i + 1; j < 3; j++) { \
                double dx = b[i].x - b[j].x, dy = b[i].y - b[j].y, dz = b[i].z - b[j].z; \
                double d2 = dx * dx + dy * dy + dz * dz; \
                double mag = dt / (d2 * __builtin_sqrt(d2)); \
                b[i].vx -= dx * b[j].m * mag; b[i].vy -= dy * b[j].m * mag; b[i].vz -= dz * b[j].m * mag; \
                b[j].vx += dx * b[i].m * mag; b[j].vy += dy * b[i].m * mag; b[j].vz += dz * b[i].m * mag; \
            } \
        for (int i = 0; i < 3; i++) { b[i].x += dt * b[i].vx; b[i].y += dt * b[i].vy; b[i].z += dt * b[i].vz; } \
    } \
    double e = 0; \
    for (int i = 0; i < 3; i++) e += 0.5 * b[i].m * (b[i].vx * b[i].vx + b[i].vy * b[i].vy + b[i].vz * b[i].vz); \
    return e; \
}
NBODY(nb_sse2, __attribute__((noinline)))
NBODY(nb_avx2, __attribute__((noinline, target("avx2"))))
NBODY(nb_fma, __attribute__((noinline, target("avx2,fma"))))

#define MANDEL(name, attr) \
attr static long name(int n) \
{ \
    long total = 0; \
    for (int py = 0; py < n; py++) \
        for (int px = 0; px < n; px++) { \
            double cr = -2.0 + 2.5 * px / n, ci = -1.25 + 2.5 * py / n, zr = 0, zi = 0; \
            int k = 0; \
            while (k < 100 && zr * zr + zi * zi < 4.0) { double t = zr * zr - zi * zi + cr; zi = 2 * zr * zi + ci; zr = t; k++; } \
            total += k * (px + 1); \
        } \
    return total; \
}
MANDEL(mb_sse2, __attribute__((noinline)))
MANDEL(mb_avx2, __attribute__((noinline, target("avx2"))))

int main(void)
{
    init();
    for (int pass = 0; pass < 2; pass++) {
        int last = pass == 1;
        fill(R, 256, 0xee); t_movddup(R); if (last) dump("movddup", 7);
        for (unsigned t = 0; t < sizeof shufs / sizeof shufs[0]; t++) {
            int dbl = shufs[t].name[4] == 'p' && shufs[t].name[5] == 'd';
            fill(R, 256, 0xee);
            shufs[t].fn(dbl ? DA : FA, dbl ? DB : FB, R);
            if (last) dump(shufs[t].name, shufs[t].lines);
        }
        for (unsigned t = 0; t < sizeof lshifts / sizeof lshifts[0]; t++) {
            fill(R, 64, 0xee);
            lshifts[t].fn(R);
            if (last) hexline(lshifts[t].name, 0, R);
        }
        fill(R, 64, 0xee); t_movnti(R); if (last) dump("movnti", 2);
        u8 buf[32];
        fill(buf, 32, 0);
        {
            double e1 = nb_sse2(3000), e2 = nb_avx2(3000), e3 = nb_fma(3000);
            __builtin_memcpy(buf, &e1, 8); __builtin_memcpy(buf + 8, &e2, 8); __builtin_memcpy(buf + 16, &e3, 8);
            long m1 = mb_sse2(48), m2 = mb_avx2(48);
            u32 x = (u32)m1, y = (u32)m2;
            __builtin_memcpy(buf + 24, &x, 4); __builtin_memcpy(buf + 28, &y, 4);
        }
        if (last) hexline("loops.nbody.mandel", 0, buf);
    }
    g_puts("DONE\n");
    return 0;
}

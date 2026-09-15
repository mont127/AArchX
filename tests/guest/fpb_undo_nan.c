#include "gsys.h"

typedef unsigned long long u64;
typedef unsigned int u32;

#define N 40
static double A[N] __attribute__((aligned(16)));
static double B[N] __attribute__((aligned(16)));
static float F[N * 2] __attribute__((aligned(16)));

static u64 bits(double d) { union { double d; u64 u; } u; u.d = d; return u.u; }
static u32 bitsf(float f) { union { float f; u32 u; } u; u.f = f; return u.u; }
static double mk(u64 v) { union { double d; u64 u; } u; u.u = v; return u.d; }
static float mkf(u32 v) { union { float f; u32 u; } u; u.u = v; return u.f; }

static const u64 sp[8] = { 0x7ff8000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull, 0x0000000000000000ull,
                           0x7ff8000000000123ull, 0xfff4000000000001ull, 0x7ff4000000000abcull, 0xfff8000000000777ull };
static const u32 spf[6] = { 0x7fc00000u, 0x7f800000u, 0xff800000u, 0x7fc00123u, 0xffa00001u, 0x7fa00abcu };

static void init(int variant)
{
    volatile double one = 1.0;
    for (int i = 0; i < N; i++) {
        A[i] = (double)(i + 1) * 0.5 - 3.0;
        B[i] = (double)(N - i) * 0.25 + one;
        F[2 * i] = (float)i * 0.75f - 2.0f;
        F[2 * i + 1] = 1.5f - (float)i * 0.125f;
    }
    if (variant == 1) { A[5] = mk(sp[4]); }
    if (variant == 2) { A[9] = mk(sp[1]); A[10] = mk(sp[2]); B[9] = mk(sp[5]); }
    if (variant == 3) { A[3] = mk(sp[6]); A[4] = mk(sp[7]); F[7] = mkf(spf[3]); F[20] = mkf(spf[5]); }
    if (variant == 4) { A[12] = mk(sp[3]); B[12] = mk(sp[3]); A[13] = mk(sp[1]); B[13] = mk(sp[1]); F[11] = mkf(spf[1]); F[12] = mkf(spf[2]); }
    if (variant == 5) { for (int i = 0; i < 8; i++) A[i * 4 + 1] = mk(sp[i]); F[3] = mkf(spf[4]); }
}

__attribute__((noinline)) static void alias_loop(double *p, double *q, long n, double k)
{
    asm volatile(
        "xor %%ecx, %%ecx\n"
        "1:\n"
        "movsd (%[p],%%rcx,8), %%xmm0\n"
        "mulsd %[k], %%xmm0\n"
        "addsd 8(%[p],%%rcx,8), %%xmm0\n"
        "movsd %%xmm0, (%[q],%%rcx,8)\n"
        "movsd 8(%[q],%%rcx,8), %%xmm2\n"
        "subsd %%xmm0, %%xmm2\n"
        "mulsd %[k], %%xmm2\n"
        "movsd %%xmm2, 8(%[q],%%rcx,8)\n"
        "inc %%rcx\n"
        "cmp %[n], %%rcx\n"
        "jb 1b\n"
        : : [p] "r"(p), [q] "r"(q), [n] "r"(n), [k] "x"(k) : "rcx", "xmm0", "xmm2", "memory", "cc");
}

__attribute__((noinline)) static void rmw_loop(double *p, long n, double k, double k2)
{
    asm volatile(
        "xor %%ecx, %%ecx\n"
        "movapd %[k2], %%xmm5\n"
        "unpcklpd %%xmm5, %%xmm5\n"
        "1:\n"
        "movupd (%[p],%%rcx,8), %%xmm0\n"
        "mulpd %%xmm5, %%xmm0\n"
        "subpd 16(%[p],%%rcx,8), %%xmm0\n"
        "movupd %%xmm0, (%[p],%%rcx,8)\n"
        "movsd 16(%[p],%%rcx,8), %%xmm1\n"
        "mulsd %[k], %%xmm1\n"
        "addsd 24(%[p],%%rcx,8), %%xmm1\n"
        "movsd %%xmm1, 16(%[p],%%rcx,8)\n"
        "movapd %%xmm1, %%xmm3\n"
        "mulsd %[k], %%xmm3\n"
        "addsd 24(%[p],%%rcx,8), %%xmm3\n"
        "movsd %%xmm3, 24(%[p],%%rcx,8)\n"
        "add $2, %%rcx\n"
        "cmp %[n], %%rcx\n"
        "jb 1b\n"
        : : [p] "r"(p), [n] "r"(n), [k] "x"(k), [k2] "x"(k2) : "rcx", "xmm0", "xmm1", "xmm3", "xmm5", "memory", "cc");
}

__attribute__((noinline)) static void partial_loop(double *p, long n, double k, double k2)
{
    asm volatile(
        "xor %%ecx, %%ecx\n"
        "movapd %[k2], %%xmm5\n"
        "unpcklpd %%xmm5, %%xmm5\n"
        "1:\n"
        "movupd (%[p],%%rcx,8), %%xmm0\n"
        "mulpd %%xmm5, %%xmm0\n"
        "movsd %%xmm0, 8(%[p],%%rcx,8)\n"
        "addpd %%xmm5, %%xmm0\n"
        "movupd %%xmm0, (%[p],%%rcx,8)\n"
        "movsd (%[p],%%rcx,8), %%xmm1\n"
        "mulsd %[k], %%xmm1\n"
        "movsd %%xmm1, (%[p],%%rcx,8)\n"
        "add $2, %%rcx\n"
        "cmp %[n], %%rcx\n"
        "jb 1b\n"
        : : [p] "r"(p), [n] "r"(n), [k] "x"(k), [k2] "x"(k2) : "rcx", "xmm0", "xmm1", "xmm5", "memory", "cc");
}

__attribute__((noinline)) static void kept_loop(double *p, double *q, long n, double k)
{
    asm volatile(
        "xor %%ecx, %%ecx\n"
        "1:\n"
        "movsd (%[p],%%rcx,8), %%xmm0\n"
        "mulsd %[k], %%xmm0\n"
        "movsd %%xmm0, (%[p],%%rcx,8)\n"
        "mulsd %[k], %%xmm0\n"
        "addsd (%[q],%%rcx,8), %%xmm0\n"
        "movsd %%xmm0, (%[q],%%rcx,8)\n"
        "inc %%rcx\n"
        "cmp %[n], %%rcx\n"
        "jb 1b\n"
        : : [p] "r"(p), [q] "r"(q), [n] "r"(n), [k] "x"(k) : "rcx", "xmm0", "memory", "cc");
}

__attribute__((noinline)) static void float_loop(float *p, long n, float k)
{
    asm volatile(
        "xor %%ecx, %%ecx\n"
        "movaps %[k], %%xmm5\n"
        "shufps $0, %%xmm5, %%xmm5\n"
        "1:\n"
        "movss (%[p],%%rcx,4), %%xmm0\n"
        "mulss %[k], %%xmm0\n"
        "addss 4(%[p],%%rcx,4), %%xmm0\n"
        "movss %%xmm0, (%[p],%%rcx,4)\n"
        "movups 4(%[p],%%rcx,4), %%xmm1\n"
        "mulps %%xmm5, %%xmm1\n"
        "movups %%xmm1, 4(%[p],%%rcx,4)\n"
        "add $2, %%rcx\n"
        "cmp %[n], %%rcx\n"
        "jb 1b\n"
        : : [p] "r"(p), [n] "r"(n), [k] "x"(k) : "rcx", "xmm0", "xmm1", "xmm5", "memory", "cc");
}

__attribute__((noinline, target("avx2,fma"))) static void vex_loop(double *p, long n, double k, double k2)
{
    asm volatile(
        "xor %%ecx, %%ecx\n"
        "vmovddup %[k2], %%xmm5\n"
        "vmovddup %[k], %%xmm3\n"
        "1:\n"
        "vmovupd (%[p],%%rcx,8), %%xmm0\n"
        "vmulpd %%xmm5, %%xmm0, %%xmm1\n"
        "vfnmadd213pd 16(%[p],%%rcx,8), %%xmm3, %%xmm1\n"
        "vmovupd %%xmm1, 16(%[p],%%rcx,8)\n"
        "vmovsd 32(%[p],%%rcx,8), %%xmm2\n"
        "vfmadd231sd %%xmm0, %%xmm3, %%xmm2\n"
        "vmovsd %%xmm2, 32(%[p],%%rcx,8)\n"
        "vmulpd %%xmm3, %%xmm0, %%xmm0\n"
        "vmovupd %%xmm0, (%[p],%%rcx,8)\n"
        "add $4, %%rcx\n"
        "cmp %[n], %%rcx\n"
        "jb 1b\n"
        : : [p] "r"(p), [n] "r"(n), [k] "x"(k), [k2] "x"(k2) : "rcx", "xmm0", "xmm1", "xmm2", "xmm3", "xmm5", "memory", "cc");
}

static void dumpa(const char *name, const double *p, int n)
{
    for (int i = 0; i < n; i++) { g_puts(name); g_puts(" "); g_putu64_nonl((u64)i); g_puts(" "); g_puthex64(bits(p[i])); }
}
static void dumpf(const char *name, const float *p, int n)
{
    for (int i = 0; i < n; i++) { g_puts(name); g_puts(" "); g_putu64_nonl((u64)i); g_puts(" "); g_puthex64((u64)bitsf(p[i])); }
}

int main(void)
{
    volatile double k = 0.75, k2 = -1.25;
    volatile float kf = 1.375f;
    for (int v = 0; v < 6; v++) {
        init(v); alias_loop(A, A, 30, k); dumpa("alias", A, 32);
        init(v); rmw_loop(A, 32, k, k2); dumpa("rmw", A, 36);
        init(v); partial_loop(A, 32, k, k2); dumpa("partial", A, 34);
        init(v); kept_loop(A, B, 30, k); dumpa("keptA", A, 31); dumpa("keptB", B, 31);
        init(v); float_loop(F, 60, kf); dumpf("float", F, 66);
        init(v); vex_loop(A, 32, k, k2); dumpa("vex", A, 36);
    }
    return 0;
}

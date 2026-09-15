#include "gsys.h"

typedef unsigned char u8;
typedef unsigned long long u64;

#define N 24
static double A[N] __attribute__((aligned(16)));
static double B[N] __attribute__((aligned(16)));
static double C[N] __attribute__((aligned(16)));
static double P[N * 2] __attribute__((aligned(16)));
static double Q[N * 2] __attribute__((aligned(16)));
static double S[8] __attribute__((aligned(16)));

static u64 bits(double d) { union { double d; u64 u; } u; u.d = d; return u.u; }
static double mk(u64 v) { union { double d; u64 u; } u; u.u = v; return u.d; }

static void init(int variant)
{
    static const u64 sp[8] = { 0x3ff8000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull, 0x0000000000000000ull,
                               0x7ff8000000000123ull, 0xfff4000000000001ull, 0x4010000000000000ull, 0xbfe0000000000000ull };
    volatile double one = 1.0, q0 = 0.75, q1 = -0.5;
    for (int i = 0; i < N; i++) {
        A[i] = (double)(i + 1) * 0.5;
        B[i] = (double)(N - i) * 0.25;
        C[i] = one;
        P[2 * i] = (double)i; P[2 * i + 1] = 2.0 - (double)i;
        Q[2 * i] = q0; Q[2 * i + 1] = q1;
    }
    if (variant == 1) { A[7] = mk(sp[1]); B[7] = mk(sp[2]); }
    if (variant == 2) { A[11] = mk(sp[4]); }
    if (variant == 3) { A[5] = -4.0; B[13] = 0.0; A[13] = 0.0; }
    if (variant == 4) { P[9] = mk(sp[5]); Q[16] = mk(sp[1]); Q[17] = mk(sp[3]); }
    if (variant == 5) { A[3] = mk(sp[1]); B[3] = mk(sp[3]); C[3] = mk(sp[4]); }
    for (int i = 0; i < 8; i++) S[i] = mk(sp[i]);
}

__attribute__((noinline)) static double rmw_loop(int n)
{
    double e = 0.0;
    for (int i = 0; i < n; i++) {
        double d = A[i] - B[i];
        double m = d * d + C[i];
        C[i] = m * 0.5 - A[i];
        B[i] += d * 0.25;
        e += __builtin_sqrt(m) / (1.0 + (double)(i & 3));
    }
    return e;
}

__attribute__((noinline)) static double exit_loop(int n, double lim)
{
    double x = 0.0, y = 0.5;
    int i;
    for (i = 0; i < n; i++) {
        double xx = x * x, yy = y * y;
        if (xx + yy > lim) break;
        y = 2.0 * x * y + B[i];
        x = xx - yy + A[i];
    }
    return x + y * 3.0 + (double)i;
}

__attribute__((noinline)) static double dead_loop(int n, double *out)
{
    double last = 0.0, s = 0.0, t = 1.0;
    for (int i = 0; i < n; i++) {
        last = A[i] * B[i] - C[i];
        t = t * 0.999 + A[i];
        s += A[i] * 0.5;
    }
    *out = last;
    return s + t;
}

__attribute__((noinline)) static void packed_loop(int n)
{
    for (int i = 0; i < n; i++) {
        double p0 = P[2 * i], p1 = P[2 * i + 1];
        double q0 = Q[2 * i], q1 = Q[2 * i + 1];
        double m0 = p0 * q0, m1 = p1 * q1;
        double sum = m0 + m1;
        P[2 * i] = sum * p0;
        P[2 * i + 1] = sum * p1;
        Q[2 * i] = q0 - m1;
        Q[2 * i + 1] = q1 - m0;
    }
}

__attribute__((noinline)) static double divsqrt_loop(int n)
{
    double acc = 0.0;
    for (int i = 0; i < n; i++) {
        double r = __builtin_sqrt(A[i]) / B[i];
        double q = (A[i] - A[i]) / (B[i] - B[i]);
        C[i] = r + q * 0.0 + C[i];
        acc += r;
    }
    return acc;
}

__attribute__((noinline)) static double mixed_loop(int n, double *o)
{
    double x = S[0], y = S[6], z = S[7];
    for (int i = 0; i < n; i++) {
        x = x * A[i] + y;
        y = y - z * B[i];
        z = z * 0.5 + x;
        if (z > 1e300) z = S[7];
        if (y < -1e300) { y = S[6]; break; }
    }
    o[0] = x; o[1] = y; o[2] = z;
    return x + y + z;
}

static void dumpd(const char *name, double d)
{
    g_puts(name); g_puts(" "); g_puthex64(bits(d));
}
static void dumpa(const char *name, const double *p, int n)
{
    for (int i = 0; i < n; i++) { g_puts(name); g_puts(" "); g_putu64_nonl((u64)i); g_puts(" "); g_puthex64(bits(p[i])); }
}

int main(void)
{
    for (int v = 0; v < 6; v++) {
        double o[4];
        init(v);
        dumpd("rmw", rmw_loop(N));
        dumpa("C", C, N); dumpa("B", B, N);
        init(v);
        dumpd("exit", exit_loop(N, 4.0));
        dumpd("exit2", exit_loop(N, 1e308));
        init(v);
        dumpd("dead", dead_loop(N, &o[0])); dumpd("dead.last", o[0]);
        init(v);
        packed_loop(N); dumpa("P", P, 2 * N); dumpa("Q", Q, 2 * N);
        init(v);
        dumpd("divsqrt", divsqrt_loop(N)); dumpa("C2", C, N);
        init(v);
        dumpd("mixed", mixed_loop(N, o)); dumpa("mixed.o", o, 3);
    }
    g_puts("DONE\n");
    return 0;
}

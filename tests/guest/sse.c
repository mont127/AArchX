/* SSE and SSE2 scalar floating-point torture. */
#include "gsys.h"

static g_u64 dbits(double d)
{
    g_u64 u;
    __builtin_memcpy(&u, &d, 8);
    return u;
}

static g_u64 fbits(float f)
{
    g_u32 u;
    __builtin_memcpy(&u, &f, 4);
    return (g_u64)u;
}

int main(int argc, char **argv, char **envp)
{
    volatile double seq[8] = { 1.5, -2.25, 3.125, -0.5, 1000000.0625, -0.0009765625, 42.0, -7.0 };

    double sum = 0.0;
    double prod = 1.0;
    for (int i = 0; i < 8; i++) {
        sum += seq[i];
        prod *= seq[i];
    }
    g_puthex64(dbits(sum));
    g_puthex64(dbits(prod));

    double q = 1.0;
    for (int i = 0; i < 8; i++)
        q /= (seq[i] == 0.0 ? 1.0 : seq[i]);
    g_puthex64(dbits(q));

    volatile float fseq[8] = { 1.5f, -2.25f, 3.125f, -0.5f, 65536.5f, -0.001953125f, 13.0f, -9.0f };
    float fsum = 0.0f;
    float fprod = 1.0f;
    for (int i = 0; i < 8; i++) {
        fsum += fseq[i];
        fprod *= fseq[i];
    }
    g_puthex64(fbits(fsum));
    g_puthex64(fbits(fprod));

    volatile double sq[6] = { 2.0, 3.0, 1024.0, 0.25, 1e300, 7.0 };
    for (int i = 0; i < 6; i++)
        g_puthex64(dbits(__builtin_sqrt(sq[i])));
    volatile float fsq[4] = { 2.0f, 9.0f, 0.0625f, 12345.0f };
    for (int i = 0; i < 4; i++)
        g_puthex64(fbits(__builtin_sqrtf(fsq[i])));

    volatile double tc[6] = { 3.99, -3.99, 2.5, -2.5, 1e9 + 0.75, -(1e9 + 0.75) };
    for (int i = 0; i < 6; i++) {
        long long t = (long long)tc[i];
        g_puthex64((g_u64)t);
    }
    volatile float ftc[4] = { 7.9f, -7.9f, 100000.5f, -100000.5f };
    for (int i = 0; i < 4; i++) {
        int t = (int)ftc[i];
        g_puthex64((g_u64)(g_u32)t);
    }

    volatile long long iv[5] = { 0, 1, -1, 1234567890123LL, -987654321LL };
    for (int i = 0; i < 5; i++) {
        double d = (double)iv[i];
        g_puthex64(dbits(d));
    }
    for (int i = 0; i < 5; i++) {
        float f = (float)iv[i];
        g_puthex64(fbits(f));
    }

    volatile double nanv = 0.0;
    nanv = nanv / nanv;
    g_puthex64(dbits(nanv));
    g_puthex64((g_u64)(nanv != nanv ? 1 : 0));

    volatile double zero = 0.0;
    volatile double one = 1.0;
    double inf = one / zero;
    g_puthex64(dbits(inf));
    g_puthex64(dbits(-zero));

    volatile double ma[6] = { 1.0, 5.0, -3.0, 2.0, -8.0, 4.0 };
    volatile double mb[6] = { 2.0, -1.0, -3.0, 9.0, 8.0, 4.0 };
    for (int i = 0; i < 6; i++) {
        double lo = ma[i] < mb[i] ? ma[i] : mb[i];
        double hi = ma[i] > mb[i] ? ma[i] : mb[i];
        g_puthex64(dbits(lo));
        g_puthex64(dbits(hi));
    }

    double cmpacc = 0.0;
    for (int i = 0; i < 8; i++) {
        if (seq[i] < 0.0) cmpacc += 1.0;
        if (seq[i] > 0.0) cmpacc += 2.0;
        if (seq[i] == 42.0) cmpacc += 4.0;
        if (seq[i] <= -7.0) cmpacc += 8.0;
        if (seq[i] >= 1.5) cmpacc += 16.0;
    }
    g_puthex64(dbits(cmpacc));

    return 0;
}

/* cvttsd2si right after a batched scalar op: the conversion reuses the
 * batch check's fcmp flags, so the NaN case must still land on the x86
 * "integer indefinite" both on the fast path and after the batch replay
 * has fixed the NaN's sign.  Goldens come from the native binary. */
#include "gsys.h"

static volatile double v_inf = __builtin_inf();
static volatile double v_one = 1.0;
static volatile double v_big = 9223372036854775808.0;   /* 2^63 */
static volatile double v_nan = __builtin_nan("");
static volatile double v_neg = -9223372036854775808.0; /* -2^63 */
static volatile double v_x = 12345.75;

static g_u64 mix;
static void out(const char *tag, long v)
{
    g_puts(tag); g_puts(" = "); g_putu64((g_u64)v); g_puts("\n");
    mix = (mix ^ (g_u64)v) * 0x9E3779B97F4A7C15ULL;
}

int main(int argc, char **argv, char **envp)
{
    for (int i = 0; i < 4; i++) {          /* run the block several times: JIT'd paths */
        double a = v_inf, b = v_inf, c = v_one, x = v_x, big = v_big;
        double r1 = a - b;                 /* NaN generated in a batch */
        long i1 = (long)r1;
        double r2 = x - c;                 /* ordinary */
        long i2 = (long)r2;
        double r3 = v_nan - c;             /* NaN propagated */
        long i3 = (long)r3;
        double r4 = big * c;               /* 2^63: positive overflow */
        long i4 = (long)r4;
        double r5 = v_neg - c;             /* below -2^63: saturates to the indefinite too */
        long i5 = (long)r5;
        double r6 = x * c;
        int i6 = (int)r6;                  /* 32-bit form */
        double r7 = big * c;
        int i7 = (int)r7;                  /* 32-bit overflow */
        out("inf-inf", i1); out("x-1", i2); out("nan-1", i3);
        out("2^63", i4); out("-2^63-1", i5); out("x32", i6); out("ovf32", i7);
        /* the NaN's bit pattern after the batch fix: sign must be x86's */
        union { double d; g_u64 u; } u; u.d = r1;
        g_puts("inf-inf bits = "); g_putu64(u.u); g_puts("\n");
    }
    g_puts("mix = "); g_putu64(mix); g_puts("\n");
    return 0;
}

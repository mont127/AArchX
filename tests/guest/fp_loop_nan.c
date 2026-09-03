/* A scalar-double loop shaped like the fpsse kernel (mul/add recurrences,
 * sqrt, div, compare-and-select) in which the values overflow and then
 * produce a NaN from inf - inf mid-loop.  The generated NaN must carry
 * x86's sign bit and propagate unchanged through the rest of the loop and
 * across its back edge, whatever lane caches and fusions the JIT applies.
 * Goldens come from the native binary. */
#include "gsys.h"

static volatile double v_c1 = 1.5, v_c2 = 0.3, v_c3 = -1.5, v_c4 = 0.25, v_lim = 1e300;
static volatile float f_c1 = 1.5f, f_c2 = 0.3f, f_c3 = -1.5f, f_c4 = 0.25f, f_lim = 1e37f;
static volatile int v_n = 5000;

static void bits64(const char *tag, double d)
{
    union { double d; g_u64 u; } u; u.d = d;
    g_puts(tag); g_puts(" "); g_putu64(u.u);
}
static void bits32(const char *tag, float f)
{
    union { float f; g_u32 u; } u; u.f = f;
    g_puts(tag); g_puts(" "); g_putu64(u.u);
}

int main(int argc, char **argv, char **envp)
{
    double x = 1.0, y = 0.5, acc = 0.0;
    double c1 = v_c1, c2 = v_c2, c3 = v_c3, c4 = v_c4, lim = v_lim;
    int n = v_n, first_nan = -1;
    for (int i = 0; i < n; i++) {
        x = x * c1 + c2;
        y = y * c3 - x * c4;
        double z = x * x + y * y;
        acc += __builtin_sqrt(z) / (1.0 + (double)(i & 7));
        if (x > lim) x = -x;                    /* flips sign at overflow: inf - inf next */
        if (acc != acc && first_nan < 0) first_nan = i;
    }
    g_puts("first_nan "); g_putu64((g_u64)(long)first_nan);
    bits64("x  ", x); bits64("y  ", y); bits64("acc", acc);

    float fx = 1.0f, fy = 0.5f, facc = 0.0f;
    float g1 = f_c1, g2 = f_c2, g3 = f_c3, g4 = f_c4, flim = f_lim;
    int fn = -1;
    for (int i = 0; i < n; i++) {
        fx = fx * g1 + g2;
        fy = fy * g3 - fx * g4;
        float fz = fx * fx + fy * fy;
        facc += __builtin_sqrtf(fz) / (1.0f + (float)(i & 7));
        if (fx > flim) fx = -fx;
        if (facc != facc && fn < 0) fn = i;
    }
    g_puts("first_nan32 "); g_putu64((g_u64)(long)fn);
    bits32("fx  ", fx); bits32("fy  ", fy); bits32("facc", facc);
    return 0;
}

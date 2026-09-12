/*
 * SSE directed rounding via MXCSR (which fesetround sets): a divide of an
 * inexact ratio must round differently in each mode.  ocerz stored MXCSR but
 * never pushed the rounding-control bits to the host FPCR, so divsd/sqrtsd
 * always rounded to nearest and every mode returned the same value.
 *
 * The check is on DIRECTION rather than exact bits, so it is robust across
 * libm versions: 1/3 is positive and inexact, so down < up strictly, nearest
 * lies between, and toward-zero equals down; for -1/3, toward -inf is more
 * negative than toward zero.
 */
#include <fenv.h>
#include <stdio.h>

int main(void)
{
    volatile double a = 1.0, b = 3.0;

    fesetround(FE_DOWNWARD);
    volatile double d = a / b;
    fesetround(FE_UPWARD);
    volatile double u = a / b;
    fesetround(FE_TONEAREST);
    volatile double n = a / b;
    fesetround(FE_TOWARDZERO);
    volatile double z = a / b;

    if (!(d < u && d <= n && n <= u && z == d)) {
        printf("BAD pos d=%.17g u=%.17g n=%.17g z=%.17g\n", d, u, n, z);
        return 1;
    }

    volatile double na = -1.0;
    fesetround(FE_DOWNWARD);
    volatile double nd = na / b;
    fesetround(FE_TOWARDZERO);
    volatile double nz = na / b;
    if (!(nd < nz)) {
        printf("BAD neg nd=%.17g nz=%.17g\n", nd, nz);
        return 2;
    }

    fesetround(FE_TONEAREST);
    printf("OK\n");
    return 0;
}

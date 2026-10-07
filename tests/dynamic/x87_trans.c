/* x87 transcendental and remainder instructions.  Transcendental results are
 * printed with %.9g, so they do not depend on 64- vs 80-bit internal
 * precision; no argument is near a multiple of pi/2 (the result there depends
 * on the precision of the internal pi).  Everything else is exact. */
#include "x87_common.h"

#define BITS(d) ((unsigned long long)d2b(d))
#define CC(sw) (((sw) >> 8) & 1), (((sw) >> 9) & 1), (((sw) >> 10) & 1), (((sw) >> 14) & 1)

static void prem_one(const char *nm, double x, double y)
{
    double r0, r1; uint16_t s0, s1;
    __asm__ volatile("fldl %4\n\tfldl %5\n\tfprem\n\tfnstsw %0\n\tfstpl %2\n\tfstp %%st(0)\n\t"
                     "fldl %4\n\tfldl %5\n\tfprem1\n\tfnstsw %1\n\tfstpl %3\n\tfstp %%st(0)"
                     : "=m"(s0), "=m"(s1), "=m"(r0), "=m"(r1) : "m"(y), "m"(x) : X87CLOB);
    /* quotient low bits: Q2 = C0, Q1 = C3, Q0 = C1 */
    printf("%s x=%g y=%g\n  fprem  r=%016llx (%g) C0=%u C1=%u C2=%u C3=%u q=%u\n  fprem1 r=%016llx (%g) C0=%u C1=%u C2=%u C3=%u q=%u\n",
           nm, x, y, BITS(r0), r0, CC(s0), ((s0 >> 8) & 1) * 4 + ((s0 >> 14) & 1) * 2 + ((s0 >> 9) & 1),
           BITS(r1), r1, CC(s1), ((s1 >> 8) & 1) * 4 + ((s1 >> 14) & 1) * 2 + ((s1 >> 9) & 1));
}

static void prem(void)
{
    static const double c[][2] = {
        { 7, 3 }, { -7, 3 }, { 7, -3 }, { -7, -3 }, { 5.5, 2 }, { 10, 4 }, { 7, 4 }, { 5, 2 }, { 3, 2 }, { 1, 3 },
        { 6, 3 }, { -6, 3 }, { 0.75, 0.5 }, { 1e10, 7 }, { -1e10, 7 }, { 1.5, INFINITY }, { -1.5, INFINITY },
        { 0.0, 5 }, { -0.0, 5 }, { 100, 7 }, { 255, 16 }, { 1023, 2 },
    };
    for (unsigned k = 0; k < sizeof c / sizeof *c; k++) prem_one("prem", c[k][0], c[k][1]);
    /* invalid cases: result is the default NaN */
    static const double inv[][2] = { { INFINITY, 2 }, { 5, 0.0 }, { INFINITY, INFINITY } };
    for (unsigned k = 0; k < sizeof inv / sizeof *inv; k++) prem_one("prem-invalid", inv[k][0], inv[k][1]);

    /* large exponent difference: the first pass is incomplete (C2 = 1) on hardware.  Only C2 of the first pass
     * and the final remainder (after looping until C2 = 0) are printed; the partial remainders and the quotient
     * bits of the last pass depend on how far each pass reduces. */
    double big = ldexp(1.0, 100), x2 = ldexp(1.0, 70) + ldexp(1.0, 20), x3 = ldexp(1.0, 100) + ldexp(1.0, 50);
    double three = 3.0, seven = 7.0, r; uint16_t s;
    __asm__ volatile("fldl %2\n\tfldl %3\n\tfprem\n\tfnstsw %0\n\tfninit" : "=m"(s), "=m"(r) : "m"(three), "m"(big) : X87CLOB);
    printf("fprem 2^100 / 3 first pass: C2=%u\n", (s >> 10) & 1);
    const double cases[][2] = { { 18446744073709551616.0, 3 }, { x2, 3 }, { x3, 7 }, { -x3, 7 }, { big, 1.5 } };
    for (unsigned k = 0; k < sizeof cases / sizeof *cases; k++) {
        double r1, r2; uint16_t s1, s2; double y = cases[k][1], x = cases[k][0];
        __asm__ volatile("fldl %4\n\tfldl %5\n\t1: fprem\n\tfnstsw %%ax\n\ttest $0x4, %%ah\n\tjnz 1b\n\tfnstsw %0\n\tfstpl %2\n\tfstp %%st(0)\n\t"
                         "fldl %4\n\tfldl %5\n\t2: fprem1\n\tfnstsw %%ax\n\ttest $0x4, %%ah\n\tjnz 2b\n\tfnstsw %1\n\tfstpl %3\n\tfstp %%st(0)"
                         : "=m"(s1), "=m"(s2), "=m"(r1), "=m"(r2) : "m"(y), "m"(x) : "rax", X87CLOB);
        printf("fprem loop x=%a y=%g: fprem r=%g C2=%u, fprem1 r=%g C2=%u\n", x, y, r1, (s1 >> 10) & 1, r2, (s2 >> 10) & 1);
    }
    (void)seven;
}

static void xtract(void)
{
    static const double v[] = { 12.0, 0.15625, -8.0, 1.0, -1.0, 1e300, 5e-324 /* m64 denormal: normal in 80 bits */,
                                0.0, -0.0, INFINITY, -INFINITY, 3.0, -0.1 };
    for (unsigned k = 0; k < sizeof v / sizeof *v; k++) {
        double sig, ex; uint16_t sw;
        __asm__ volatile("fnclex\n\tfldl %3\n\tfxtract\n\tfnstsw %2\n\tfstpl %0\n\tfstpl %1" : "=m"(sig), "=m"(ex), "=m"(sw) : "m"(v[k]) : X87CLOB);
        printf("fxtract in=%016llx: sig=%016llx exp=%016llx (%g) ze=%u\n", BITS(v[k]), BITS(sig), BITS(ex), ex, (sw >> 2) & 1);
    }
    /* fxtract pushes: the stack depth and TOP afterwards */
    uint16_t sw; double d = 6.0;
    __asm__ volatile("fldl %1\n\tfxtract\n\tfnstsw %0\n\tfninit" : "=m"(sw) : "m"(d) : X87CLOB);
    printf("fxtract top after = %u\n", (sw >> 11) & 7);
}

static void scale(void)
{
    static const double c[][2] = {
        { 3, 4 }, { 3, -2 }, { 1, 2.7 }, { 1, -2.7 }, { 0.0, 5 }, { -5, 1 }, { INFINITY, -5 }, { 1, INFINITY }, { 1, -INFINITY },
        { INFINITY, -INFINITY }, { 0.0, INFINITY }, { 1.5, 0.5 }, { 1, -0.5 }, { 1, -1074 }, { -0.0, 3 }, { 7, 0.0 }, { 1, 62 },
        { 0.1, 1 }, { 3, 1023 },
    };
    for (unsigned k = 0; k < sizeof c / sizeof *c; k++) {
        double r; uint16_t sw;
        __asm__ volatile("fldl %3\n\tfldl %2\n\tfscale\n\tfnstsw %1\n\tfstpl %0\n\tfstp %%st(0)" : "=m"(r), "=m"(sw) : "m"(c[k][0]), "m"(c[k][1]) : X87CLOB);
        printf("fscale st0=%g st1=%g -> %016llx (%.9g) top=%u\n", c[k][0], c[k][1], BITS(r), r, (sw >> 11) & 7);
    }
}

static void logs(void)
{
    static const double l2x[][2] = { { 8, 3 }, { 10, 1 }, { 0.5, 2 }, { 1, 5 }, { 2, -3 }, { 0.0, 3 }, { -1, 3 }, { INFINITY, 1 },
                                     { 3, 0.5 }, { 0.1, 7 }, { 1024, 0.1 }, { 1e300, 1 } };
    for (unsigned k = 0; k < sizeof l2x / sizeof *l2x; k++) {
        double r; uint16_t sw;
        __asm__ volatile("fldl %3\n\tfldl %2\n\tfyl2x\n\tfnstsw %1\n\tfstpl %0" : "=m"(r), "=m"(sw) : "m"(l2x[k][0]), "m"(l2x[k][1]) : X87CLOB);
        printf("fyl2x x=%g y=%g -> %.9g top=%u\n", l2x[k][0], l2x[k][1], r, (sw >> 11) & 7);
    }
    static const double l2p[][2] = { { 0.25, 2 }, { -0.25, 3 }, { 0.0, 5 }, { -0.0, 5 }, { 1e-5, 1 }, { 0.125, -4 }, { -0.125, 1 }, { 0.2, 10 } };
    for (unsigned k = 0; k < sizeof l2p / sizeof *l2p; k++) {
        double r; uint16_t sw;
        __asm__ volatile("fldl %3\n\tfldl %2\n\tfyl2xp1\n\tfnstsw %1\n\tfstpl %0" : "=m"(r), "=m"(sw) : "m"(l2p[k][0]), "m"(l2p[k][1]) : X87CLOB);
        printf("fyl2xp1 x=%g y=%g -> %.9g top=%u\n", l2p[k][0], l2p[k][1], r, (sw >> 11) & 7);
    }
}

static void exps(void)
{
    static const double v[] = { 0.5, -0.5, 1.0, -1.0, 0.0, -0.0, 0.25, 1e-10, -0.75, 0.9 };
    for (unsigned k = 0; k < sizeof v / sizeof *v; k++) {
        double r;
        __asm__ volatile("fldl %1\n\tf2xm1\n\tfstpl %0" : "=m"(r) : "m"(v[k]) : X87CLOB);
        printf("f2xm1 %g -> %.9g\n", v[k], r);
    }
}

static void trig(void)
{
    static const double v[] = { 0.0, -0.0, 0.1, 0.5, 1.0, -1.0, 2.0, -2.0, 3.0, 5.0, 7.0, 100.0, 1e5, 0.7853981633974483 };
    for (unsigned k = 0; k < sizeof v / sizeof *v; k++) {
        double s, c, t1, t0, sc_s, sc_c; uint16_t sw[4];
        __asm__ volatile("fldl %[v]\n\tfsin\n\tfnstsw %[w0]\n\tfstpl %[s]\n\t"
                         "fldl %[v]\n\tfcos\n\tfnstsw %[w1]\n\tfstpl %[c]\n\t"
                         "fldl %[v]\n\tfptan\n\tfnstsw %[w2]\n\tfstpl %[t0]\n\tfstpl %[t1]\n\t"
                         "fldl %[v]\n\tfsincos\n\tfnstsw %[w3]\n\tfstpl %[sc]\n\tfstpl %[ss]"
                         : [s] "=m"(s), [c] "=m"(c), [t0] "=m"(t0), [t1] "=m"(t1), [sc] "=m"(sc_c), [ss] "=m"(sc_s),
                           [w0] "=m"(sw[0]), [w1] "=m"(sw[1]), [w2] "=m"(sw[2]), [w3] "=m"(sw[3])
                         : [v] "m"(v[k]) : X87CLOB);
        printf("x=%g: fsin %.9g (c2=%u) fcos %.9g (c2=%u) fptan tan=%.9g st0=%g (c2=%u top=%u) fsincos cos=%.9g sin=%.9g (c2=%u top=%u)\n",
               v[k], s, (sw[0] >> 10) & 1, c, (sw[1] >> 10) & 1, t1, t0, (sw[2] >> 10) & 1, (sw[2] >> 11) & 7,
               sc_c, sc_s, (sw[3] >> 10) & 1, (sw[3] >> 11) & 7);
    }
    static const double pa[][2] = { { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 }, { 0.0, -1 }, { -0.0, -1 }, { 0.0, 1 }, { -0.0, 1 },
                                    { 1, 0.0 }, { -1, 0.0 }, { 0.0, 0.0 }, { 0.0, -0.0 }, { -0.0, -0.0 }, { INFINITY, 1 }, { 1, INFINITY },
                                    { 1, -INFINITY }, { INFINITY, INFINITY }, { -INFINITY, INFINITY }, { INFINITY, -INFINITY },
                                    { 3, 4 }, { 0.5, 2 }, { 2, 0.5 }, { -3, 4 }, { 1e-300, 1e300 } };
    for (unsigned k = 0; k < sizeof pa / sizeof *pa; k++) {
        double r; uint16_t sw;
        __asm__ volatile("fldl %3\n\tfldl %2\n\tfpatan\n\tfnstsw %1\n\tfstpl %0" : "=m"(r), "=m"(sw) : "m"(pa[k][1]), "m"(pa[k][0]) : X87CLOB);
        printf("fpatan y=%g x=%g -> %.9g top=%u\n", pa[k][0], pa[k][1], r, (sw >> 11) & 7);
    }
}

/* |x| >= 2^63: C2 = 1 and the operand is left unchanged (FPTAN and FSINCOS do not push). */
static void big_args(void)
{
    const double v[] = { 9223372036854775808.0, -9223372036854775808.0, 18446744073709551616.0, 1e300,
                         9223372036854774784.0 /* 2^63 - 1024: in range */ };
    for (unsigned k = 0; k < sizeof v / sizeof *v; k++) {
        double r[4]; uint16_t sw[4];
        __asm__ volatile("fldl %[v]\n\tfsin\n\tfnstsw %[w0]\n\tfstpl %[r0]\n\t"
                         "fldl %[v]\n\tfcos\n\tfnstsw %[w1]\n\tfstpl %[r1]\n\t"
                         "fldl %[v]\n\tfptan\n\tfnstsw %[w2]\n\tfstpl %[r2]\n\tfninit\n\t"
                         "fldl %[v]\n\tfsincos\n\tfnstsw %[w3]\n\tfstpl %[r3]\n\tfninit"
                         : [r0] "=m"(r[0]), [r1] "=m"(r[1]), [r2] "=m"(r[2]), [r3] "=m"(r[3]),
                           [w0] "=m"(sw[0]), [w1] "=m"(sw[1]), [w2] "=m"(sw[2]), [w3] "=m"(sw[3])
                         : [v] "m"(v[k]) : X87CLOB);
        int inrange = k == 4;
        printf("big x=%016llx:", BITS(v[k]));
        static const char *nm[] = { "fsin", "fcos", "fptan", "fsincos" };
        for (int j = 0; j < 4; j++) {
            printf(" %s C2=%u top=%u", nm[j], (sw[j] >> 10) & 1, (sw[j] >> 11) & 7);
            if (!inrange) printf(" st0=%016llx", BITS(r[j]));
        }
        printf("\n");
    }
    /* infinity and NaN operands: invalid, C2 stays 0 */
    const double w[] = { INFINITY, -INFINITY, __builtin_nan("0x1") };
    for (unsigned k = 0; k < sizeof w / sizeof *w; k++) {
        double r[2]; uint16_t sw[2];
        __asm__ volatile("fldl %[v]\n\tfsin\n\tfnstsw %[w0]\n\tfstpl %[r0]\n\tfldl %[v]\n\tfcos\n\tfnstsw %[w1]\n\tfstpl %[r1]"
                         : [r0] "=m"(r[0]), [r1] "=m"(r[1]), [w0] "=m"(sw[0]), [w1] "=m"(sw[1]) : [v] "m"(w[k]) : X87CLOB);
        printf("special x=%016llx: fsin %016llx C2=%u fcos %016llx C2=%u\n", BITS(w[k]), BITS(r[0]), (sw[0] >> 10) & 1, BITS(r[1]), (sw[1] >> 10) & 1);
    }
}

int main(void)
{
    x87_init();
    prem();
    x87_group_end("prem");
    xtract();
    x87_group_end("fxtract");
    scale();
    x87_group_end("fscale");
    logs();
    x87_group_end("logs");
    exps();
    x87_group_end("f2xm1");
    trig();
    x87_group_end("trig");
    big_args();
    x87_group_end("big-args");
    return 0;
}

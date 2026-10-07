/* x87 arithmetic: every register form of D8/DC/DE, memory forms, integer
 * forms, unary ops, constants, FXCH/FLD/FST, FRNDINT and NaN propagation.
 * Register forms are emitted as .byte sequences, with the Intel mnemonic in a
 * comment, because AT&T syntax swaps fsub/fsubr and fdiv/fdivr for some of
 * them. */
#include "x87_common.h"

enum { ADD, MUL, SUB, SUBR, DIV, DIVR };
static const char *opname[] = { "add", "mul", "sub", "subr", "div", "divr" };

/* A set of eight register values, st(0) first. */
static const double SETP[8] = { 4, 2, 8, 16, 32, 0.5, 64, 0.25 }; /* powers of two: all six ops exact */
static const double SETN[8] = { -3, 5, -7, 9, 11, -13, 15, 17 };  /* distinct odd integers: add/mul/sub/subr exact */

#define LOAD8 \
    "fldl 56(%[a])\n\tfldl 48(%[a])\n\tfldl 40(%[a])\n\tfldl 32(%[a])\n\t" \
    "fldl 24(%[a])\n\tfldl 16(%[a])\n\tfldl 8(%[a])\n\tfldl 0(%[a])\n\t"
#define STORE8 \
    "fstpl 0(%[o])\n\tfstpl 8(%[o])\n\tfstpl 16(%[o])\n\tfstpl 24(%[o])\n\t" \
    "fstpl 32(%[o])\n\tfstpl 40(%[o])\n\tfstpl 48(%[o])\n\tfstpl 56(%[o])\n\t"
#define STORE7 \
    "fstpl 0(%[o])\n\tfstpl 8(%[o])\n\tfstpl 16(%[o])\n\tfstpl 24(%[o])\n\t" \
    "fstpl 32(%[o])\n\tfstpl 40(%[o])\n\tfstpl 48(%[o])\n\t"

static double apply(int op, double x, double y)
{
    switch (op) {
    case ADD: return x + y;
    case MUL: return x * y;
    case SUB: return x - y;
    case SUBR: return y - x;
    case DIV: return x / y;
    default: return y / x;
    }
}

/* dest = st(0) (dst_i = 0) or st(i) (dst_i = 1); src is the other one. */
static void check(const char *form, int op, int dst_i, int pop, int i, const double *a, const double *out)
{
    double e[8];
    int n = 8, k, ok = 1;
    memcpy(e, a, sizeof e);
    int d = dst_i ? i : 0, s = dst_i ? 0 : i;
    e[d] = apply(op, e[d], e[s]);
    if (pop) { for (k = 0; k < 7; k++) e[k] = e[k + 1]; n = 7; }
    printf("%s f%s i=%d:", form, opname[op], i);
    for (k = 0; k < n; k++) {
        printf(" %.17g", out[k]);
        if (d2b(out[k]) != d2b(e[k])) ok = 0;
    }
    printf(" %s\n", ok ? "ok" : "MISMATCH");
}

/* X-macro over the seven register indices. */
#define FORI(M, ...) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) \
                     M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__)

#define CASE(i, form, op, dst_i, pop, STORE, B0, B1) \
    { double out[8] = { 0 }; \
      __asm__ volatile(LOAD8 ".byte " #B0 ", " #B1 "+" #i "\n\t" STORE :: [a] "r"(a), [o] "r"(out) : X87CLOB); \
      check(form, op, dst_i, pop, i, a, out); }

/* D8 forms: st(0) = st(0) op st(i) */
#define D8_ADD(i, ...)  CASE(i, "d8 ", ADD,  0, 0, STORE8, 0xd8, 0xc0) /* fadd st(0), st(i) */
#define D8_MUL(i, ...)  CASE(i, "d8 ", MUL,  0, 0, STORE8, 0xd8, 0xc8) /* fmul st(0), st(i) */
#define D8_SUB(i, ...)  CASE(i, "d8 ", SUB,  0, 0, STORE8, 0xd8, 0xe0) /* fsub st(0), st(i) */
#define D8_SUBR(i, ...) CASE(i, "d8 ", SUBR, 0, 0, STORE8, 0xd8, 0xe8) /* fsubr st(0), st(i) */
#define D8_DIV(i, ...)  CASE(i, "d8 ", DIV,  0, 0, STORE8, 0xd8, 0xf0) /* fdiv st(0), st(i) */
#define D8_DIVR(i, ...) CASE(i, "d8 ", DIVR, 0, 0, STORE8, 0xd8, 0xf8) /* fdivr st(0), st(i) */
/* DC forms: st(i) = st(i) op st(0); note DC swaps the sub/subr and div/divr opcodes */
#define DC_ADD(i, ...)  CASE(i, "dc ", ADD,  1, 0, STORE8, 0xdc, 0xc0) /* fadd st(i), st(0) */
#define DC_MUL(i, ...)  CASE(i, "dc ", MUL,  1, 0, STORE8, 0xdc, 0xc8) /* fmul st(i), st(0) */
#define DC_SUB(i, ...)  CASE(i, "dc ", SUB,  1, 0, STORE8, 0xdc, 0xe8) /* fsub st(i), st(0) */
#define DC_SUBR(i, ...) CASE(i, "dc ", SUBR, 1, 0, STORE8, 0xdc, 0xe0) /* fsubr st(i), st(0) */
#define DC_DIV(i, ...)  CASE(i, "dc ", DIV,  1, 0, STORE8, 0xdc, 0xf8) /* fdiv st(i), st(0) */
#define DC_DIVR(i, ...) CASE(i, "dc ", DIVR, 1, 0, STORE8, 0xdc, 0xf0) /* fdivr st(i), st(0) */
/* DE forms: as DC, then pop */
#define DE_ADD(i, ...)  CASE(i, "de ", ADD,  1, 1, STORE7, 0xde, 0xc0) /* faddp st(i), st(0) */
#define DE_MUL(i, ...)  CASE(i, "de ", MUL,  1, 1, STORE7, 0xde, 0xc8) /* fmulp st(i), st(0) */
#define DE_SUB(i, ...)  CASE(i, "de ", SUB,  1, 1, STORE7, 0xde, 0xe8) /* fsubp st(i), st(0) */
#define DE_SUBR(i, ...) CASE(i, "de ", SUBR, 1, 1, STORE7, 0xde, 0xe0) /* fsubrp st(i), st(0) */
#define DE_DIV(i, ...)  CASE(i, "de ", DIV,  1, 1, STORE7, 0xde, 0xf8) /* fdivp st(i), st(0) */
#define DE_DIVR(i, ...) CASE(i, "de ", DIVR, 1, 1, STORE7, 0xde, 0xf0) /* fdivrp st(i), st(0) */

static void reg_forms(const double *a, int full)
{
    FORI(D8_ADD, 0) FORI(D8_MUL, 0) FORI(D8_SUB, 0) FORI(D8_SUBR, 0)
    FORI(DC_ADD, 0) FORI(DC_MUL, 0) FORI(DC_SUB, 0) FORI(DC_SUBR, 0)
    FORI(DE_ADD, 0) FORI(DE_MUL, 0) FORI(DE_SUB, 0) FORI(DE_SUBR, 0)
    if (full) {
        FORI(D8_DIV, 0) FORI(D8_DIVR, 0) FORI(DC_DIV, 0) FORI(DC_DIVR, 0)
        FORI(DE_DIV, 0) FORI(DE_DIVR, 0)
    }
}

/* Memory forms: st(0) = st(0) op mem, or mem op st(0) for the reversed ones. */
#define MEMF(name, insn, ty, op, x, y) { \
    ty m = (y); double r, xv = (x); \
    __asm__ volatile("fldl %1\n\t" insn " %2\n\tfstpl %0" : "=m"(r) : "m"(xv), "m"(m) : X87CLOB); \
    double e = apply(op, xv, (double)m); \
    printf("%s st0=%.17g mem=%.17g -> %.17g %s\n", name, xv, (double)m, r, d2b(r) == d2b(e) ? "ok" : "MISMATCH"); }

static void mem_forms(void)
{
    MEMF("fadds", "fadds", float, ADD, -16.0, 4.0f)
    MEMF("fmuls", "fmuls", float, MUL, -16.0, 4.0f)
    MEMF("fsubs", "fsubs", float, SUB, -16.0, 4.0f)
    MEMF("fsubrs", "fsubrs", float, SUBR, -16.0, 4.0f)
    MEMF("fdivs", "fdivs", float, DIV, -16.0, 4.0f)
    MEMF("fdivrs", "fdivrs", float, DIVR, -16.0, 4.0f)
    MEMF("faddl", "faddl", double, ADD, -16.0, 0.5)
    MEMF("fmull", "fmull", double, MUL, -16.0, 0.5)
    MEMF("fsubl", "fsubl", double, SUB, -16.0, 0.5)
    MEMF("fsubrl", "fsubrl", double, SUBR, -16.0, 0.5)
    MEMF("fdivl", "fdivl", double, DIV, -16.0, 0.5)
    MEMF("fdivrl", "fdivrl", double, DIVR, -16.0, 0.5)
    MEMF("fiadds", "fiadds", int16_t, ADD, -16.0, -4)
    MEMF("fimuls", "fimuls", int16_t, MUL, -16.0, -4)
    MEMF("fisubs", "fisubs", int16_t, SUB, -16.0, -4)
    MEMF("fisubrs", "fisubrs", int16_t, SUBR, -16.0, -4)
    MEMF("fidivs", "fidivs", int16_t, DIV, -16.0, -4)
    MEMF("fidivrs", "fidivrs", int16_t, DIVR, -16.0, -4)
    MEMF("fiaddl", "fiaddl", int32_t, ADD, -16.0, 100000)
    MEMF("fimull", "fimull", int32_t, MUL, -16.0, 100000)
    MEMF("fisubl", "fisubl", int32_t, SUB, -16.0, 100000)
    MEMF("fisubrl", "fisubrl", int32_t, SUBR, -16.0, 100000)
    MEMF("fidivl", "fidivl", int32_t, DIV, -16.0, 8)
    MEMF("fidivrl", "fidivrl", int32_t, DIVR, -16.0, 8)
    /* second operand set so the quotients are not all powers of two */
    MEMF("fidivs2", "fidivs", int16_t, DIV, 7.5, -3)
    MEMF("fidivrs2", "fidivrs", int16_t, DIVR, 0.25, 12)
    MEMF("fiaddl2", "fiaddl", int32_t, ADD, 0.5, -2147483647 - 1)
    MEMF("fimuls2", "fimuls", int16_t, MUL, 1.5, 32767)
}

static void unary(void)
{
    static const double v[] = { 0.0, -0.0, 2.5, -2.5, 1e300, -1e300, 5e-324, INFINITY, -INFINITY };
    for (unsigned k = 0; k < sizeof v / sizeof *v; k++) {
        double c, a;
        __asm__ volatile("fldl %2\n\tfchs\n\tfstpl %0\n\tfldl %2\n\tfabs\n\tfstpl %1" : "=m"(c), "=m"(a) : "m"(v[k]) : X87CLOB);
        printf("fchs/fabs %016llx -> chs %016llx abs %016llx\n", (unsigned long long)d2b(v[k]),
               (unsigned long long)d2b(c), (unsigned long long)d2b(a));
    }
    static const double sq[] = { 16.0, 2.0, 0.25, 0.0, -0.0, INFINITY, -1.0, -INFINITY, 1e300 };
    for (unsigned k = 0; k < sizeof sq / sizeof *sq; k++) {
        double r;
        __asm__ volatile("fldl %1\n\tfsqrt\n\tfstpl %0" : "=m"(r) : "m"(sq[k]) : X87CLOB);
        printf("fsqrt %g -> %016llx %.9g\n", sq[k], (unsigned long long)d2b(r), r);
    }
    double c[7];
    __asm__ volatile("fld1\n\tfstpl %0\n\tfldl2t\n\tfstpl %1\n\tfldl2e\n\tfstpl %2\n\tfldpi\n\tfstpl %3\n\t"
                     "fldlg2\n\tfstpl %4\n\tfldln2\n\tfstpl %5\n\tfldz\n\tfstpl %6"
                     : "=m"(c[0]), "=m"(c[1]), "=m"(c[2]), "=m"(c[3]), "=m"(c[4]), "=m"(c[5]), "=m"(c[6]) :: X87CLOB);
    const char *cn[] = { "fld1", "fldl2t", "fldl2e", "fldpi", "fldlg2", "fldln2", "fldz" };
    for (int k = 0; k < 7; k++) printf("%s = %.17g\n", cn[k], c[k]);
}

#define XCH(i, ...) { double out[8] = { 0 }; \
    __asm__ volatile(LOAD8 ".byte 0xd9, 0xc8+" #i "\n\t" STORE8 :: [a] "r"(SETP), [o] "r"(out) : X87CLOB); /* fxch st(i) */ \
    double e[8]; memcpy(e, SETP, sizeof e); e[0] = SETP[i]; e[i] = SETP[0]; \
    printf("fxch st(%d):", i); for (int k = 0; k < 8; k++) printf(" %g", out[k]); \
    printf(" %s\n", memcmp(e, out, sizeof e) ? "MISMATCH" : "ok"); }
/* fld st(i): needs a free slot, so only seven registers are loaded; i = 0..6 */
#define FLDI(i) { double out[8] = { 0 }; \
    __asm__ volatile("fldl 48(%[a])\n\tfldl 40(%[a])\n\tfldl 32(%[a])\n\tfldl 24(%[a])\n\tfldl 16(%[a])\n\tfldl 8(%[a])\n\tfldl 0(%[a])\n\t" \
        ".byte 0xd9, 0xc0+" #i "\n\t" STORE8 :: [a] "r"(SETP), [o] "r"(out) : X87CLOB); /* fld st(i) */ \
    printf("fld st(%d):", i); for (int k = 0; k < 8; k++) printf(" %g", out[k]); \
    printf(" %s\n", (out[0] == SETP[i] && out[1] == SETP[0] && out[7] == SETP[6]) ? "ok" : "MISMATCH"); }
#define FSTI(i, ...) { double out[8] = { 0 }; \
    __asm__ volatile(LOAD8 ".byte 0xdd, 0xd0+" #i "\n\t" STORE8 :: [a] "r"(SETP), [o] "r"(out) : X87CLOB); /* fst st(i) */ \
    printf("fst st(%d):", i); for (int k = 0; k < 8; k++) printf(" %g", out[k]); \
    printf(" %s\n", (out[i] == SETP[0] && out[0] == SETP[0]) ? "ok" : "MISMATCH"); }
#define FSTPI(i, ...) { double out[8] = { 0 }; \
    __asm__ volatile(LOAD8 ".byte 0xdd, 0xd8+" #i "\n\t" STORE7 :: [a] "r"(SETP), [o] "r"(out) : X87CLOB); /* fstp st(i) */ \
    printf("fstp st(%d):", i); for (int k = 0; k < 7; k++) printf(" %g", out[k]); \
    printf(" %s\n", (out[i - 1] == SETP[0] && out[0] == (i == 1 ? SETP[0] : SETP[1])) ? "ok" : "MISMATCH"); }

static void moves(void)
{
    FORI(XCH, 0)
    FLDI(0) FLDI(1) FLDI(2) FLDI(3) FLDI(4) FLDI(5) FLDI(6)
    FORI(FSTI, 0)
    FORI(FSTPI, 0)
    /* fst/fstp to m32/m64, and rounding of a double through m32 */
    float f; double d;
    __asm__ volatile("fldl %2\n\tfsts %0\n\tfstpl %1" : "=m"(f), "=m"(d) : "m"(SETN[1]) : X87CLOB);
    printf("fsts/fstpl %g %g\n", f, d);
    double big = 1e40; uint32_t fb;
    __asm__ volatile("fldl %1\n\tfstps %0" : "=m"(f) : "m"(big) : X87CLOB);
    memcpy(&fb, &f, 4);
    printf("fstps 1e40 -> %08x\n", fb);
}

static void rndint(void)
{
    static const double v[] = { 0.5, -0.5, 1.5, -1.5, 2.5, -2.5, -0.0, 0.3, -0.3, 3.5, -3.5, 7.0, 1e300 };
    enum { NV = sizeof v / sizeof *v };
    static const char *rcn[] = { "nearest", "down", "up", "trunc" };
    double res[4][NV];
    for (int rc = 0; rc < 4; rc++) {
        x87_setcw(0x037f | rc << 10);
        for (int k = 0; k < NV; k++)
            __asm__ volatile("fldl %1\n\tfrndint\n\tfstpl %0" : "=m"(res[rc][k]) : "m"(v[k]) : X87CLOB);
    }
    /* restore the default control word before printing: libc's printf can be affected by the rounding mode */
    x87_setcw(0x037f);
    for (int rc = 0; rc < 4; rc++)
        for (int k = 0; k < NV; k++)
            printf("frndint rc=%s in=%016llx -> %016llx\n", rcn[rc], (unsigned long long)d2b(v[k]),
                   (unsigned long long)d2b(res[rc][k]));
}

typedef struct { uint64_t m; uint16_t se; } f80; /* 80-bit memory image: 8-byte mantissa, sign/exponent */
static const f80 SNAN1 = { 0x8000000000000800ull, 0x7fff }; /* signalling: bit 62 clear, payload nonzero */
static const f80 SNAN2 = { 0x8000000000001000ull, 0x7fff };
static const f80 SNANN = { 0x8000000000000800ull, 0xffff }; /* negative SNaN */

#define P(name, v) printf("%-30s %016llx\n", name, (unsigned long long)(v))

/* st(0) = a, then fadd m64 b */
static uint64_t memadd(double a, double b)
{
    double r;
    __asm__ volatile("fldl %1\n\tfaddl %2\n\tfstpl %0" : "=m"(r) : "m"(a), "m"(b) : X87CLOB);
    return d2b(r);
}
/* st(1) = a, st(0) = b; fadd st(0), st(1) */
static uint64_t regadd(double a, double b)
{
    double r;
    __asm__ volatile("fldl %1\n\tfldl %2\n\t.byte 0xd8, 0xc1\n\tfstpl %0\n\tfstp %%st(0)" /* fadd st(0), st(1) */
                     : "=m"(r) : "m"(a), "m"(b) : X87CLOB);
    return d2b(r);
}
/* st(1) = a, st(0) = b; fadd st(1), st(0) then pop st(0) */
static uint64_t regadd_dc(double a, double b)
{
    double r;
    __asm__ volatile("fldl %1\n\tfldl %2\n\t.byte 0xdc, 0xc1\n\tfstp %%st(0)\n\tfstpl %0" /* fadd st(1), st(0) */
                     : "=m"(r) : "m"(a), "m"(b) : X87CLOB);
    return d2b(r);
}
/* st(1) = A (m80), st(0) = B (m80); fadd st(0), st(1) */
static uint64_t regadd80(const f80 *a, const f80 *b)
{
    double r;
    __asm__ volatile("fldt %1\n\tfldt %2\n\t.byte 0xd8, 0xc1\n\tfstpl %0\n\tfstp %%st(0)" /* fadd st(0), st(1) */
                     : "=m"(r) : "m"(*a), "m"(*b) : X87CLOB);
    return d2b(r);
}
static uint64_t regadd80d(const f80 *a, double b)
{
    double r;
    __asm__ volatile("fldt %1\n\tfldl %2\n\t.byte 0xd8, 0xc1\n\tfstpl %0\n\tfstp %%st(0)" /* fadd st(0), st(1) */
                     : "=m"(r) : "m"(*a), "m"(b) : X87CLOB);
    return d2b(r);
}
static uint64_t regadd80dr(double a, const f80 *b)
{
    double r;
    __asm__ volatile("fldl %1\n\tfldt %2\n\t.byte 0xd8, 0xc1\n\tfstpl %0\n\tfstp %%st(0)" /* fadd st(0), st(1) */
                     : "=m"(r) : "m"(a), "m"(*b) : X87CLOB);
    return d2b(r);
}

static void nans(void)
{
    double zero = 0.0, inf = INFINITY, m1 = -1.0, one = 1.0, r;
    uint64_t qa = 0x7ff8000000000111ull, qb = 0x7ff8000000000222ull, qc = 0xfff8000000000001ull, qd = 0xfff8000000000333ull;
    double Qa = b2d(qa), Qb = b2d(qb), Qc = b2d(qc), Qd = b2d(qd);

    __asm__ volatile("fldl %1\n\tfdivl %2\n\tfstpl %0" : "=m"(r) : "m"(zero), "m"(zero) : X87CLOB); P("0/0", d2b(r));
    __asm__ volatile("fldl %1\n\tfsubl %2\n\tfstpl %0" : "=m"(r) : "m"(inf), "m"(inf) : X87CLOB); P("inf-inf", d2b(r));
    __asm__ volatile("fldl %1\n\tfmull %2\n\tfstpl %0" : "=m"(r) : "m"(inf), "m"(zero) : X87CLOB); P("inf*0", d2b(r));
    __asm__ volatile("fldl %1\n\tfdivl %2\n\tfstpl %0" : "=m"(r) : "m"(inf), "m"(inf) : X87CLOB); P("inf/inf", d2b(r));
    __asm__ volatile("fldl %1\n\tfsqrt\n\tfstpl %0" : "=m"(r) : "m"(m1) : X87CLOB); P("sqrt(-1)", d2b(r));
    P("qnan+1 (mem)", memadd(Qa, one));
    P("1+qnan (mem)", memadd(one, Qa));
    P("-qnan+1 (mem)", memadd(Qc, one));
    P("1+(-qnan) (mem)", memadd(one, Qc));
    P("qa+qb (mem)", memadd(Qa, Qb));
    P("qb+qa (mem)", memadd(Qb, Qa));
    P("qa+qc(neg) (mem)", memadd(Qa, Qc));
    P("qc(neg)+qa (mem)", memadd(Qc, Qa));
    P("qc+qd both neg (mem)", memadd(Qc, Qd));
    P("qd+qc both neg (mem)", memadd(Qd, Qc));
    P("qa+qb d8", regadd(Qa, Qb));
    P("qb+qa d8", regadd(Qb, Qa));
    P("qa+qc d8", regadd(Qa, Qc));
    P("qc+qa d8", regadd(Qc, Qa));
    P("qa+qb dc", regadd_dc(Qa, Qb));
    P("qb+qa dc", regadd_dc(Qb, Qa));
    P("qc+qd dc", regadd_dc(Qc, Qd));
    P("qd+qc dc", regadd_dc(Qd, Qc));
    P("qnan*0 d8", regadd(Qa, 0.0));
    /* signalling NaNs, loaded with FLD m80 (which does not quiet them) */
    { double q;
      __asm__ volatile("fldt %1\n\tfstpl %0" : "=m"(q) : "m"(SNAN1) : X87CLOB); P("snan1 fld80 -> fstp64", d2b(q));
      __asm__ volatile("fldt %1\n\tfstpl %0" : "=m"(q) : "m"(SNANN) : X87CLOB); P("-snan fld80 -> fstp64", d2b(q)); }
    P("snan1+1 d8", regadd80d(&SNAN1, one));
    P("1+snan1 d8", regadd80dr(one, &SNAN1));
    P("snan1+qa d8", regadd80d(&SNAN1, Qa));
    P("qa+snan1 d8", regadd80dr(Qa, &SNAN1));
    P("snan1+qb d8", regadd80d(&SNAN1, Qb));
    P("qb+snan1 d8", regadd80dr(Qb, &SNAN1));
    P("snan1+qc d8", regadd80d(&SNAN1, Qc));
    P("qc+snan1 d8", regadd80dr(Qc, &SNAN1));
    P("snan1+snan2 d8", regadd80(&SNAN1, &SNAN2));
    P("snan2+snan1 d8", regadd80(&SNAN2, &SNAN1));
    P("snan1+(-snan) d8", regadd80(&SNAN1, &SNANN));
    P("(-snan)+snan1 d8", regadd80(&SNANN, &SNAN1));
}

int main(void)
{
    x87_init();
    reg_forms(SETP, 1);
    x87_group_end("reg-forms-pow2");
    reg_forms(SETN, 0);
    x87_group_end("reg-forms-odd");
    mem_forms();
    x87_group_end("mem-forms");
    unary();
    x87_group_end("unary");
    moves();
    x87_group_end("moves");
    rndint();
    x87_group_end("frndint");
    nans();
    x87_group_end("nans");
    return 0;
}

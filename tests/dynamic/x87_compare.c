/* x87 comparisons: FCOM/FUCOM families (C3 C2 C1 C0 via FNSTSW), FICOM, FTST,
 * FCOMI/FUCOMI (EFLAGS), FCMOVcc and FXAM.  Register forms are .byte
 * sequences with the Intel mnemonic in a comment. */
#include "x87_common.h"

#define LOAD8 \
    "fldl 56(%[a])\n\tfldl 48(%[a])\n\tfldl 40(%[a])\n\tfldl 32(%[a])\n\t" \
    "fldl 24(%[a])\n\tfldl 16(%[a])\n\tfldl 8(%[a])\n\tfldl 0(%[a])\n\t"
#define FORI(M, ...) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) \
                     M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__)

#define QNAN __builtin_nan("0x1")
#define QNANN (-__builtin_nan("0x42"))

typedef struct { const char *nm; double a, b; } cs_t;
static const cs_t CASES[] = {
    { "less", 1, 2 }, { "equal", 2, 2 }, { "greater", 3, 2 },
    { "unord-st0", QNAN, 2 }, { "unord-sti", 2, QNANN },
    { "zeros", 0.0, -0.0 }, { "-inf<inf", -INFINITY, INFINITY }, { "neg", -1, -2 },
};
#define NCASES (int)(sizeof CASES / sizeof *CASES)

static void fill(double *v, double a, double b, int i)
{
    for (int k = 0; k < 8; k++) v[k] = 1000 + k;
    v[0] = a;
    v[i] = b;
}
static void showsw(const char *what, int i, const cs_t *c, unsigned sw)
{
    printf("%s i=%d %s: C3=%u C2=%u C1=%u C0=%u ie=%u top=%u\n", what, i, c->nm, (sw >> 14) & 1, (sw >> 10) & 1,
           (sw >> 9) & 1, (sw >> 8) & 1, sw & 1, (sw >> 11) & 7);
}

#define CMPREG(i, nm, B0, B1) { double v[8]; uint16_t sw; fill(v, c->a, c->b, i); \
    __asm__ volatile(LOAD8 ".byte " #B0 ", " #B1 "+" #i "\n\tfnstsw %[s]\n\tfninit" : [s] "=m"(sw) : [a] "r"(v) : X87CLOB); \
    showsw(nm, i, c, sw); }
#define FCOM_R(i, ...)   CMPREG(i, "fcom",   0xd8, 0xd0) /* fcom st(i) */
#define FCOMP_R(i, ...)  CMPREG(i, "fcomp",  0xd8, 0xd8) /* fcomp st(i) */
#define FUCOM_R(i, ...)  CMPREG(i, "fucom",  0xdd, 0xe0) /* fucom st(i) */
#define FUCOMP_R(i, ...) CMPREG(i, "fucomp", 0xdd, 0xe8) /* fucomp st(i) */

static void reg_compares(void)
{
    for (int k = 0; k < NCASES; k++) {
        const cs_t *c = &CASES[k];
        FORI(FCOM_R, 0) FORI(FCOMP_R, 0) FORI(FUCOM_R, 0) FORI(FUCOMP_R, 0)
        double v[8]; uint16_t sw;
        fill(v, c->a, c->b, 1);
        __asm__ volatile(LOAD8 ".byte 0xde, 0xd9\n\tfnstsw %[s]\n\tfninit" : [s] "=m"(sw) : [a] "r"(v) : X87CLOB); /* fcompp (st(1)) */
        showsw("fcompp", 1, c, sw);
        __asm__ volatile(LOAD8 ".byte 0xda, 0xe9\n\tfnstsw %[s]\n\tfninit" : [s] "=m"(sw) : [a] "r"(v) : X87CLOB); /* fucompp (st(1)) */
        showsw("fucompp", 1, c, sw);
        /* plain forms are the same instructions with i = 1; also exercise FCOM with no operand (st(1)) */
        __asm__ volatile(LOAD8 "fcom\n\tfnstsw %[s]\n\tfninit" : [s] "=m"(sw) : [a] "r"(v) : X87CLOB);
        showsw("fcom-noarg", 1, c, sw);
    }
}

static void mem_compares(void)
{
    for (int k = 0; k < NCASES; k++) {
        const cs_t *c = &CASES[k];
        float f = (float)c->b; double d = c->b; uint16_t sw[4];
        __asm__ volatile("fldl %4\n\tfcoms %5\n\tfnstsw %0\n\tfninit\n\t"
                         "fldl %4\n\tfcomps %5\n\tfnstsw %1\n\tfninit\n\t"
                         "fldl %4\n\tfcoml %6\n\tfnstsw %2\n\tfninit\n\t"
                         "fldl %4\n\tfcompl %6\n\tfnstsw %3\n\tfninit"
                         : "=m"(sw[0]), "=m"(sw[1]), "=m"(sw[2]), "=m"(sw[3]) : "m"(c->a), "m"(f), "m"(d) : X87CLOB);
        showsw("fcoms", 0, c, sw[0]);
        showsw("fcomps", 0, c, sw[1]);
        showsw("fcoml", 0, c, sw[2]);
        showsw("fcompl", 0, c, sw[3]);
    }
    static const double av[] = { 1.5, 2.0, 3.0, 2.5, -3.0 };
    static const int bv[] = { 2, -2, 0 };
    for (unsigned x = 0; x < sizeof av / sizeof *av; x++)
        for (unsigned y = 0; y < sizeof bv / sizeof *bv; y++) {
            int16_t s = (int16_t)bv[y]; int32_t l = bv[y]; uint16_t sw[4];
            cs_t c = { "int", av[x], bv[y] };
            __asm__ volatile("fldl %4\n\tficoms %5\n\tfnstsw %0\n\tfninit\n\t"
                             "fldl %4\n\tficomps %5\n\tfnstsw %1\n\tfninit\n\t"
                             "fldl %4\n\tficoml %6\n\tfnstsw %2\n\tfninit\n\t"
                             "fldl %4\n\tficompl %6\n\tfnstsw %3\n\tfninit"
                             : "=m"(sw[0]), "=m"(sw[1]), "=m"(sw[2]), "=m"(sw[3]) : "m"(av[x]), "m"(s), "m"(l) : X87CLOB);
            printf("a=%g b=%d\n", av[x], bv[y]);
            showsw("ficoms", 0, &c, sw[0]);
            showsw("ficomps", 0, &c, sw[1]);
            showsw("ficoml", 0, &c, sw[2]);
            showsw("ficompl", 0, &c, sw[3]);
        }
    /* QNaN against an integer operand */
    { int16_t s = 5; int32_t l = 5; uint16_t sw[2]; double q = QNAN; cs_t c = { "nan", 0, 5 };
      __asm__ volatile("fldl %2\n\tficoms %3\n\tfnstsw %0\n\tfninit\n\tfldl %2\n\tficompl %4\n\tfnstsw %1\n\tfninit"
                       : "=m"(sw[0]), "=m"(sw[1]) : "m"(q), "m"(s), "m"(l) : X87CLOB);
      showsw("ficoms", 0, &c, sw[0]); showsw("ficompl", 0, &c, sw[1]); }
}

static void ftst(void)
{
    const double v[] = { -1.0, 0.0, -0.0, 1.0, QNAN, INFINITY, -INFINITY, 5e-324, -5e-324 };
    for (unsigned k = 0; k < sizeof v / sizeof *v; k++) {
        uint16_t sw; cs_t c = { "ftst", v[k], 0 };
        __asm__ volatile("fldl %1\n\tftst\n\tfnstsw %0\n\tfninit" : "=m"(sw) : "m"(v[k]) : X87CLOB);
        printf("in=%016llx ", (unsigned long long)d2b(v[k]));
        showsw("ftst", 0, &c, sw);
    }
}

/* FCOMI family: result in EFLAGS (ZF PF CF), OF/SF/AF cleared.  Each case is
 * run once with all of CF PF AF ZF SF OF set beforehand and once with all
 * clear, so both "set" and "cleared" are observed. */
#define FLAGS_SET   "mov $0x7f, %%al\n\tadd $1, %%al\n\tmov $0xd5, %%ah\n\tsahf\n\t"  /* OF SF AF PF ZF CF = 1 */
#define FLAGS_CLEAR "xor %%eax, %%eax\n\tsahf\n\t"                                    /* all = 0 */
#define CMPI(i, nm, B0, B1) { double v[8]; uint32_t r1, r0; uint16_t sw; fill(v, c->a, c->b, i); \
    __asm__ volatile(LOAD8 FLAGS_SET ".byte " #B0 ", " #B1 "+" #i "\n\tlahf\n\tseto %%al\n\tfnstsw %[s]\n\tfninit" \
                     : "=&a"(r1), [s] "=m"(sw) : [a] "r"(v) : X87CLOB); \
    uint16_t sw1 = sw; \
    __asm__ volatile(LOAD8 FLAGS_CLEAR ".byte " #B0 ", " #B1 "+" #i "\n\tlahf\n\tseto %%al\n\tfnstsw %[s]\n\tfninit" \
                     : "=&a"(r0), [s] "=m"(sw) : [a] "r"(v) : X87CLOB); \
    showfl(nm, i, c, r1, r0, sw1, sw); }
static void showfl(const char *what, int i, const cs_t *c, uint32_t r1, uint32_t r0, unsigned sw1, unsigned sw0)
{
    printf("%s i=%d %s: from-set ZF=%u PF=%u CF=%u OF=%u SF=%u AF=%u | from-clear ZF=%u PF=%u CF=%u OF=%u SF=%u AF=%u | top=%u,%u ie=%u,%u\n",
           what, i, c->nm,
           (r1 >> 14) & 1, (r1 >> 10) & 1, (r1 >> 8) & 1, r1 & 1, (r1 >> 15) & 1, (r1 >> 12) & 1,
           (r0 >> 14) & 1, (r0 >> 10) & 1, (r0 >> 8) & 1, r0 & 1, (r0 >> 15) & 1, (r0 >> 12) & 1,
           (sw1 >> 11) & 7, (sw0 >> 11) & 7, sw1 & 1, sw0 & 1);
}
#define FCOMI_R(i, ...)   CMPI(i, "fcomi",   0xdb, 0xf0) /* fcomi st(0), st(i) */
#define FCOMIP_R(i, ...)  CMPI(i, "fcomip",  0xdf, 0xf0) /* fcomip st(0), st(i) */
#define FUCOMI_R(i, ...)  CMPI(i, "fucomi",  0xdb, 0xe8) /* fucomi st(0), st(i) */
#define FUCOMIP_R(i, ...) CMPI(i, "fucomip", 0xdf, 0xe8) /* fucomip st(0), st(i) */

static void comi(void)
{
    for (int k = 0; k < NCASES; k++) {
        const cs_t *c = &CASES[k];
        FORI(FCOMI_R, 0) FORI(FCOMIP_R, 0) FORI(FUCOMI_R, 0) FORI(FUCOMIP_R, 0)
    }
}

/* FCMOVcc: st(0) = -5 and st(i) = 100 + i.  The five EFLAGS states are
 * none, CF, PF, ZF and CF|PF|ZF; a character per state, 'T' if st(0) took st(i). */
static const uint32_t FLST[5] = { 0x0000, 0x0100, 0x0400, 0x4000, 0x4500 }; /* AH value << 8 for sahf */
#define CMOV(i, nm, B0, B1) { char res[6]; \
    for (int s = 0; s < 5; s++) { double v[8], r; fill(v, -5.0, 100.0 + i, i); \
        __asm__ volatile(LOAD8 "mov %[f], %%eax\n\tsahf\n\t.byte " #B0 ", " #B1 "+" #i "\n\tfstpl %[r]\n\tfninit" \
                         : [r] "=m"(r) : [a] "r"(v), [f] "r"(FLST[s]) : "rax", X87CLOB); \
        res[s] = r == 100.0 + i ? 'T' : (r == -5.0 ? '.' : '?'); } \
    res[5] = 0; printf("%s st(%d) [none CF PF ZF all]: %s\n", nm, i, res); }
#define CMOVB(i, ...)   CMOV(i, "fcmovb",   0xda, 0xc0) /* fcmovb st(0), st(i) */
#define CMOVE(i, ...)   CMOV(i, "fcmove",   0xda, 0xc8) /* fcmove st(0), st(i) */
#define CMOVBE(i, ...)  CMOV(i, "fcmovbe",  0xda, 0xd0) /* fcmovbe st(0), st(i) */
#define CMOVU(i, ...)   CMOV(i, "fcmovu",   0xda, 0xd8) /* fcmovu st(0), st(i) */
#define CMOVNB(i, ...)  CMOV(i, "fcmovnb",  0xdb, 0xc0) /* fcmovnb st(0), st(i) */
#define CMOVNE(i, ...)  CMOV(i, "fcmovne",  0xdb, 0xc8) /* fcmovne st(0), st(i) */
#define CMOVNBE(i, ...) CMOV(i, "fcmovnbe", 0xdb, 0xd0) /* fcmovnbe st(0), st(i) */
#define CMOVNU(i, ...)  CMOV(i, "fcmovnu",  0xdb, 0xd8) /* fcmovnu st(0), st(i) */

static void cmov(void)
{
    FORI(CMOVB, 0) FORI(CMOVE, 0) FORI(CMOVBE, 0) FORI(CMOVU, 0)
    FORI(CMOVNB, 0) FORI(CMOVNE, 0) FORI(CMOVNBE, 0) FORI(CMOVNU, 0)
}

typedef struct { uint64_t m; uint16_t se; } f80;
static void fxam_one(const char *nm, unsigned sw)
{
    printf("fxam %-34s C3=%u C2=%u C1=%u C0=%u top=%u\n", nm, (sw >> 14) & 1, (sw >> 10) & 1, (sw >> 9) & 1, (sw >> 8) & 1, (sw >> 11) & 7);
}
static void fxam(void)
{
    uint16_t sw;
    /* classes loaded from m64 */
    const struct { const char *nm; double v; } d[] = {
        { "+normal 1.5", 1.5 }, { "-normal -1.5", -1.5 }, { "+zero", 0.0 }, { "-zero", -0.0 },
        { "+inf", INFINITY }, { "-inf", -INFINITY },
        { "+qnan", QNAN }, { "-qnan", QNANN },
        /* a double denormal loaded with FLD m64 is converted to a NORMAL 80-bit number (its exponent is
         * far above the 80-bit minimum), so FXAM reports "normal", not "denormal" */
        { "+double-denormal (loads as normal)", 5e-324 }, { "-double-denormal (loads as normal)", -5e-324 },
        { "+largest double", 1.7976931348623157e308 },
    };
    for (unsigned k = 0; k < sizeof d / sizeof *d; k++) {
        __asm__ volatile("fldl %1\n\tfxam\n\tfnstsw %0\n\tfninit" : "=m"(sw) : "m"(d[k].v) : X87CLOB);
        fxam_one(d[k].nm, sw);
    }
    /* classes loaded from m80 (the mantissa carries an explicit integer bit) */
    const struct { const char *nm; f80 v; } t[] = {
        { "+snan (m80)", { 0x8000000000000800ull, 0x7fff } },
        { "-snan (m80)", { 0x8000000000000800ull, 0xffff } },
        { "+80-bit denormal exp=0 m=1", { 0x0000000000000001ull, 0x0000 } },
        { "-80-bit denormal exp=0 m=1", { 0x0000000000000001ull, 0x8000 } },
        { "+80-bit denormal exp=0 m=2^62", { 0x4000000000000000ull, 0x0000 } },
        { "+80-bit normal exp=1 (smallest)", { 0x8000000000000000ull, 0x0001 } },
        { "+80-bit normal exp=7ffe (largest)", { 0xffffffffffffffffull, 0x7ffe } },
        { "+80-bit infinity", { 0x8000000000000000ull, 0x7fff } },
        { "+80-bit zero", { 0x0000000000000000ull, 0x0000 } },
        { "-80-bit zero", { 0x0000000000000000ull, 0x8000 } },
    };
    for (unsigned k = 0; k < sizeof t / sizeof *t; k++) {
        __asm__ volatile("fldt %1\n\tfxam\n\tfnstsw %0\n\tfninit" : "=m"(sw) : "m"(t[k].v) : X87CLOB);
        fxam_one(t[k].nm, sw);
    }
    /* empty registers */
    __asm__ volatile("fninit\n\tfxam\n\tfnstsw %0" : "=m"(sw) :: X87CLOB);
    fxam_one("empty after fninit", sw);
    double p = 1.5, n = -1.5;
    __asm__ volatile("fldl %1\n\tffree %%st(0)\n\tfxam\n\tfnstsw %0\n\tfninit" : "=m"(sw) : "m"(p) : X87CLOB);
    fxam_one("empty after ffree (was +1.5)", sw);
    __asm__ volatile("fldl %1\n\tffree %%st(0)\n\tfxam\n\tfnstsw %0\n\tfninit" : "=m"(sw) : "m"(n) : X87CLOB);
    fxam_one("empty after ffree (was -1.5)", sw);
    __asm__ volatile("fldl %1\n\tfstp %%st(0)\n\tfxam\n\tfnstsw %0\n\tfninit" : "=m"(sw) : "m"(p) : X87CLOB);
    fxam_one("empty after fld+fstp", sw);
    /* fxam must not change the stack */
    __asm__ volatile("fldl %1\n\tfxam\n\tfxam\n\tfstpl %2\n\tfnstsw %0" : "=m"(sw), "+m"(p) , "=m"(n) :: X87CLOB);
    printf("fxam keeps stack: st0=%g top=%u\n", n, (sw >> 11) & 7);
}

int main(void)
{
    x87_init();
    reg_compares();
    x87_group_end("reg-compares");
    mem_compares();
    x87_group_end("mem-compares");
    ftst();
    x87_group_end("ftst");
    comi();
    x87_group_end("comi");
    cmov();
    x87_group_end("cmov");
    fxam();
    x87_group_end("fxam");
    return 0;
}

/* x87 integer and BCD conversions: FILD, FIST/FISTP, FISTTP, the exact m64
 * courier (FILD m64 / FISTP m64), m80 images of integers, FBLD/FBSTP. */
#include "x87_common.h"

static void hexn(const uint8_t *p, int n)
{
    for (int k = 0; k < n; k++) printf("%02x", p[k]);
}

static const uint64_t PATS[] = {
    0x0000000000000001ull, 0x00000000FFFFFFFFull, 0x001FFFFFFFFFFFFFull, 0x0020000000000001ull,
    0x0123456789ABCDEFull, 0x00FF00FF00FF00FFull, 0x7FFFFFFFFFFFFFFFull, 0x4142434445464748ull,
    0x0000800000001234ull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 0x8000000000000001ull,
    0x7FFFFFFFFFFFFFFEull, 0x0000000100000000ull, 0xFFFFFFFF00000000ull, 0xFFE0000000000001ull,
};
#define NPATS (int)(sizeof PATS / sizeof *PATS)

static uint64_t courier(uint64_t v)
{
    uint64_t out;
    __asm__ volatile("fildll %1\n\tfistpll %0" : "=m"(out) : "m"(v) : X87CLOB);
    return out;
}

static void fild_forms(void)
{
    static const int16_t s16[] = { 0, 1, -1, 32767, -32768, 12345 };
    static const int32_t s32[] = { 0, 1, -1, 2147483647, -2147483647 - 1, 123456789, -123456789 };
    uint8_t b[16];
    for (unsigned k = 0; k < sizeof s16 / sizeof *s16; k++) {
        memset(b, 0, sizeof b);
        __asm__ volatile("filds %1\n\tfstpt %0" : "=m"(b) : "m"(s16[k]) : X87CLOB);
        printf("filds %d -> m80 ", s16[k]); hexn(b, 10); printf("\n");
    }
    for (unsigned k = 0; k < sizeof s32 / sizeof *s32; k++) {
        memset(b, 0, sizeof b);
        __asm__ volatile("fildl %1\n\tfstpt %0" : "=m"(b) : "m"(s32[k]) : X87CLOB);
        printf("fildl %d -> m80 ", s32[k]); hexn(b, 10); printf("\n");
    }
    for (int k = 0; k < NPATS; k++) {
        memset(b, 0, sizeof b);
        __asm__ volatile("fildll %1\n\tfstpt %0" : "=m"(b) : "m"(PATS[k]) : X87CLOB);
        printf("fildll %016llx -> m80 ", (unsigned long long)PATS[k]); hexn(b, 10); printf("\n");
    }
}

typedef struct { int16_t f16, p16; int32_t f32, p32; int64_t p64; uint16_t sw[5]; } ires;

/* FIST m16/m32 (non-popping), FISTP m16/m32/m64, in all four rounding modes.  There is no FIST m64. */
static void fist_modes(void)
{
    const double v[] = { 0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 0.3, -0.7, 3.5, -3.5, 32767.5, -32768.5, 32768.0, -32769.0,
                         2147483647.0, 2147483647.5, -2147483648.0, -2147483648.5, 2147483648.0, -2147483649.0,
                         3000000000.0, -3000000000.0, 1e12, 5e9, 9223372036854775808.0,
                         -9223372036854775808.0, 4e18, -4e18, 1e19, 0.0, -0.0,
                         __builtin_nan("0x1"), INFINITY, -INFINITY };
    enum { NV = sizeof v / sizeof *v };
    static const char *rcn[] = { "nearest", "down", "up", "trunc" };
    static ires res[4][NV];
    for (int rc = 0; rc < 4; rc++) {
        x87_setcw(0x037f | rc << 10);
        for (int k = 0; k < NV; k++) {
            ires *r = &res[rc][k];
            __asm__ volatile("fnclex\n\tfldl %[v]\n\tfists %[a]\n\tfnstsw %[s0]\n\tfnclex\n\tfistl %[b]\n\tfnstsw %[s1]\n\t"
                             "fnclex\n\tfistpl %[d]\n\tfnstsw %[s2]\n\t"
                             "fnclex\n\tfldl %[v]\n\tfistps %[c]\n\tfnstsw %[s3]\n\t"
                             "fnclex\n\tfldl %[v]\n\tfistpll %[e]\n\tfnstsw %[s4]"
                             : [a] "=m"(r->f16), [b] "=m"(r->f32), [c] "=m"(r->p16), [d] "=m"(r->p32), [e] "=m"(r->p64),
                               [s0] "=m"(r->sw[0]), [s1] "=m"(r->sw[1]), [s2] "=m"(r->sw[2]), [s3] "=m"(r->sw[3]), [s4] "=m"(r->sw[4])
                             : [v] "m"(v[k]) : X87CLOB);
        }
    }
    x87_setcw(0x037f);
    for (int rc = 0; rc < 4; rc++)
        for (int k = 0; k < NV; k++) {
            const ires *r = &res[rc][k];
            printf("rc=%s in=%016llx fist16=%04x fist32=%08x fistp32=%08x fistp16=%04x fistp64=%016llx ie/pe=",
                   rcn[rc], (unsigned long long)d2b(v[k]), (uint16_t)r->f16, (uint32_t)r->f32, (uint32_t)r->p32,
                   (uint16_t)r->p16, (unsigned long long)r->p64);
            for (int s = 0; s < 5; s++) printf("%u%u%s", r->sw[s] & 1, (r->sw[s] >> 5) & 1, s < 4 ? "," : "");
            printf("\n");
        }
}

static void fisttp(void)
{
    const double v[] = { 0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 0.99999, -0.99999, 32767.9, -32768.9, 32768.0, -32769.0,
                         2147483647.9, -2147483648.9, 2147483648.0, 9223372036854775807.0, -9223372036854775808.0,
                         4e18, 1e19, 0.0, -0.0, __builtin_nan("0x1"), INFINITY, -INFINITY };
    enum { NV = sizeof v / sizeof *v };
    static const char *rcn[] = { "nearest", "up" };
    static struct { int16_t a; int32_t b; int64_t c; uint16_t sw[3]; } r[2][NV];
    for (int m = 0; m < 2; m++) {
        x87_setcw(m ? 0x0b7f : 0x037f); /* FISTTP truncates regardless of RC */
        for (int k = 0; k < NV; k++)
            __asm__ volatile("fnclex\n\tfldl %[v]\n\tfisttps %[a]\n\tfnstsw %[s0]\n\tfnclex\n\tfldl %[v]\n\tfisttpl %[b]\n\tfnstsw %[s1]\n\t"
                             "fnclex\n\tfldl %[v]\n\tfisttpll %[c]\n\tfnstsw %[s2]"
                             : [a] "=m"(r[m][k].a), [b] "=m"(r[m][k].b), [c] "=m"(r[m][k].c),
                               [s0] "=m"(r[m][k].sw[0]), [s1] "=m"(r[m][k].sw[1]), [s2] "=m"(r[m][k].sw[2]) : [v] "m"(v[k]) : X87CLOB);
    }
    x87_setcw(0x037f);
    for (int m = 0; m < 2; m++)
        for (int k = 0; k < NV; k++) {
            printf("fisttp rc=%s in=%016llx m16=%04x m32=%08x m64=%016llx ie/pe=", rcn[m], (unsigned long long)d2b(v[k]),
                   (uint16_t)r[m][k].a, (uint32_t)r[m][k].b, (unsigned long long)r[m][k].c);
            for (int s = 0; s < 3; s++) printf("%u%u%s", r[m][k].sw[s] & 1, (r[m][k].sw[s] >> 5) & 1, s < 2 ? "," : "");
            printf("\n");
        }
}

static void courier_tests(void)
{
    for (int k = 0; k < NPATS; k++) {
        uint64_t o = courier(PATS[k]);
        printf("courier in=%016llx out=%016llx %s\n", (unsigned long long)PATS[k], (unsigned long long)o, o == PATS[k] ? "ok" : "CORRUPTED");
    }
    uint64_t first = 0; int bad = 0, n = 0;
    for (int rc = 0; rc < 4; rc++) {
        x87_setcw(0x037f | rc << 10); /* the courier must not depend on RC */
        uint64_t s = 0x243f6a8885a308d3ull;
        for (int k = 0; k < 64; k++) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            uint64_t v = s ^ (s >> 29);
            n++;
            if (courier(v) != v && !bad++) first = v;
        }
    }
    x87_setcw(0x037f);
    printf("courier lcg (4 rounding modes): count=%d bad=%d first=", n, bad);
    if (bad) printf("%016llx\n", (unsigned long long)first); else printf("none\n");
    /* the same, but with the value kept on the stack across other x87 work */
    int bad2 = 0;
    uint64_t s = 12345;
    for (int k = 0; k < 64; k++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t v = s ^ (s >> 31), a, b;
        __asm__ volatile("fildll %2\n\tfld %%st(0)\n\tfxch %%st(1)\n\tfistpll %0\n\tfistpll %1" : "=m"(a), "=m"(b) : "m"(v) : X87CLOB);
        if (a != v || b != v) bad2++;
    }
    printf("fild/fld st0/fxch/fistp chain lcg: count=64 bad=%d\n", bad2);
    for (int k = 0; k < NPATS; k++) {
        uint64_t a, b;
        __asm__ volatile("fildll %2\n\tfld %%st(0)\n\tfxch %%st(1)\n\tfistpll %0\n\tfistpll %1" : "=m"(a), "=m"(b) : "m"(PATS[k]) : X87CLOB);
        printf("chain %016llx -> %016llx %016llx %s\n", (unsigned long long)PATS[k], (unsigned long long)a, (unsigned long long)b,
               (a == PATS[k] && b == PATS[k]) ? "ok" : "CORRUPTED");
    }
    /* two different integers on the stack: fxch must keep each exact */
    uint64_t p = 0x0123456789ABCDEFull, q = 0x0020000000000001ull, a, b;
    __asm__ volatile("fildll %2\n\tfildll %3\n\tfxch %%st(1)\n\tfistpll %0\n\tfistpll %1" : "=m"(a), "=m"(b) : "m"(p), "m"(q) : X87CLOB);
    printf("fxch exact: %016llx %016llx\n", (unsigned long long)a, (unsigned long long)b);
    /* st(i) register copies through fstp st(i) and fld st(i) */
    __asm__ volatile("fildll %2\n\tfildll %3\n\tfld %%st(1)\n\tfistpll %0\n\tfistpll %1\n\tfistpll %1" : "=m"(a), "=m"(b) : "m"(p), "m"(q) : X87CLOB);
    printf("fld st(1) exact: %016llx %016llx\n", (unsigned long long)a, (unsigned long long)b);
}

typedef struct { uint64_t m; uint16_t se; } f80;
static void m80_to_int(void)
{
    const struct { f80 v; const char *nm; } t[] = {
        { { 0xfedcba9876543210ull, 0x403d }, "63-bit positive" },
        { { 0xfedcba9876543210ull, 0xc03d }, "63-bit negative" },
        { { 0x8000000000000002ull, 0x403d }, "2^62+1" },
        { { 0xfffffffffffffffeull, 0x403d }, "2^63-1" },
        { { 0x8000000000000000ull, 0xc03e }, "-2^63 (in range)" },
        { { 0x8000000000000000ull, 0x403e }, "+2^63 (out of range)" },
        { { 0x8000000000000001ull, 0x403e }, "2^63+1 (out of range)" },
        { { 0xffffffffffffffffull, 0x403e }, "2^64-1 (out of range)" },
        { { 0x8000000000000000ull, 0xc03f }, "-2^64 (out of range)" },
        { { 0xfffffffffffffffeull, 0xc03d }, "-(2^63-1)" },
        { { 0x8000000000000000ull, 0x0000 + 0x3fff }, "1.0" },
        { { 0xe000000000000000ull, 0x4000 }, "3.5 (inexact)" },
    };
    for (unsigned k = 0; k < sizeof t / sizeof *t; k++) {
        int64_t r; uint16_t sw;
        __asm__ volatile("fnclex\n\tfldt %2\n\tfistpll %0\n\tfnstsw %1" : "=m"(r), "=m"(sw) : "m"(t[k].v) : X87CLOB);
        printf("fld m80 %016llx:%04x (%s) -> fistp m64 %016llx ie=%u pe=%u\n", (unsigned long long)t[k].v.m, t[k].v.se, t[k].nm,
               (unsigned long long)r, sw & 1, (sw >> 5) & 1);
    }
}

/* packed BCD: 9 bytes of 18 digits (little-endian pairs), byte 9 = sign (0x80 negative) */
static void to_bcd(uint8_t *b, uint64_t mag, int neg)
{
    memset(b, 0, 10);
    for (int k = 0; k < 9; k++) { b[k] = mag % 10; mag /= 10; b[k] |= (mag % 10) << 4; mag /= 10; }
    b[9] = neg ? 0x80 : 0;
}

static void bcd(void)
{
    const struct { uint64_t mag; int neg; } t[] = {
        { 0, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 10, 0 }, { 99, 1 }, { 123456789012345678ull, 0 },
        { 999999999999999999ull, 0 }, { 999999999999999999ull, 1 }, { 100000000000000000ull, 0 },
    };
    for (unsigned k = 0; k < sizeof t / sizeof *t; k++) {
        uint8_t in[10], out[10], img[16] = { 0 }; int64_t iv;
        to_bcd(in, t[k].mag, t[k].neg);
        __asm__ volatile("fbld %3\n\tfld %%st(0)\n\tfbstp %0\n\tfld %%st(0)\n\tfistpll %2\n\tfstpt %1"
                         : "=m"(out), "=m"(img), "=m"(iv) : "m"(in) : X87CLOB);
        printf("fbld "); hexn(in, 10); printf(" -> fbstp "); hexn(out, 10);
        printf(" m80 "); hexn(img, 10);
        printf(" fistp %016llx %s\n", (unsigned long long)iv, memcmp(in, out, 10) ? "DIFF" : "same");
    }
    /* the FBSTP result rounds according to RC */
    const double v[] = { 2.5, 3.5, -2.5, -3.5, 0.5, -0.5, 1.5, 0.3, -0.7, 1e17, 123456789.0 };
    enum { NV = sizeof v / sizeof *v };
    static const char *rcn[] = { "nearest", "down", "up", "trunc" };
    static uint8_t r[4][NV][10];
    for (int rc = 0; rc < 4; rc++) {
        x87_setcw(0x037f | rc << 10);
        for (int k = 0; k < NV; k++) __asm__ volatile("fldl %1\n\tfbstp %0" : "=m"(r[rc][k]) : "m"(v[k]) : X87CLOB);
    }
    x87_setcw(0x037f);
    for (int rc = 0; rc < 4; rc++)
        for (int k = 0; k < NV; k++) { printf("fbstp rc=%s in=%016llx -> ", rcn[rc], (unsigned long long)d2b(v[k])); hexn(r[rc][k], 10); printf("\n"); }
    /* out of range: BCD indefinite */
    const double o[] = { 1e18, -1e18, 1e19, 1e300, __builtin_nan("0x1"), INFINITY, -INFINITY, 999999999999999999.0 };
    for (unsigned k = 0; k < sizeof o / sizeof *o; k++) {
        uint8_t out[10]; uint16_t sw;
        __asm__ volatile("fnclex\n\tfldl %2\n\tfbstp %0\n\tfnstsw %1" : "=m"(out), "=m"(sw) : "m"(o[k]) : X87CLOB);
        printf("fbstp in=%016llx -> ", (unsigned long long)d2b(o[k])); hexn(out, 10); printf(" ie=%u\n", sw & 1);
    }
    const int64_t big[] = { 999999999999999999ll, 1000000000000000000ll, -999999999999999999ll, -1000000000000000000ll, 4611686018427387904ll };
    for (unsigned k = 0; k < sizeof big / sizeof *big; k++) {
        uint8_t out[10]; uint16_t sw;
        __asm__ volatile("fnclex\n\tfildll %2\n\tfbstp %0\n\tfnstsw %1" : "=m"(out), "=m"(sw) : "m"(big[k]) : X87CLOB);
        printf("fildll %lld -> fbstp ", (long long)big[k]); hexn(out, 10); printf(" ie=%u\n", sw & 1);
    }
}

int main(void)
{
    x87_init();
    fild_forms();
    x87_group_end("fild");
    fist_modes();
    x87_group_end("fist");
    fisttp();
    x87_group_end("fisttp");
    courier_tests();
    x87_group_end("courier");
    m80_to_int();
    x87_group_end("m80-to-int");
    bcd();
    x87_group_end("bcd");
    return 0;
}

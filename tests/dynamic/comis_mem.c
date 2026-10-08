/*
 * comiss/ucomiss/comisd/ucomisd with a memory operand and the jcc, setcc or
 * cmovcc that reads their flags a few instructions later, as src/jit.c fuses
 * them (comis_fuse_producer with a memory source): every condition, ordered,
 * equal and unordered operands, and a store to the compared memory between
 * the compare and the branch, which must still see the value that was read.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static volatile float fmem[4] = { 1.5f, 1.5f, -2.0f, 0.0f };
static volatile double dmem[4] = { 1.5, 1.5, -2.0, 0.0 };

#define CONDS(X) X(a) X(ae) X(b) X(be) X(e) X(ne) X(p) X(np)

/* ucomiss xmm, m32 then an unrelated scalar op, then jcc: one bit per condition. */
#define FJCC(cc)                                                                          \
    static int f_##cc(float x, volatile float *m)                                         \
    {                                                                                     \
        int r;                                                                            \
        float t = x;                                                                      \
        __asm__ volatile("ucomiss (%2), %1\n\t"                                           \
                         "addss %1, %1\n\t"                                               \
                         "mov $1, %0\n\t"                                                 \
                         "j" #cc " 1f\n\t"                                                \
                         "mov $0, %0\n"                                                   \
                         "1:"                                                             \
                         : "=&r"(r), "+x"(t) : "r"(m) : "cc");                            \
        return r;                                                                         \
    }
CONDS(FJCC)

#define DSET(cc)                                                                          \
    static int d_##cc(double x, volatile double *m)                                       \
    {                                                                                     \
        unsigned char r;                                                                  \
        double t = x;                                                                     \
        __asm__ volatile("comisd (%2), %1\n\t"                                            \
                         "movapd %1, %%xmm7\n\t"                                          \
                         "set" #cc " %0"                                                  \
                         : "=r"(r), "+x"(t) : "r"(m) : "xmm7", "cc");                     \
        return r;                                                                         \
    }
CONDS(DSET)

/* comiss then a store to the same memory, then cmov: the cmov sees the old comparison. */
static int store_between(float x, volatile float *m)
{
    int r = 7, alt = 9;
    float t = x;
    __asm__ volatile("comiss (%3), %2\n\t"
                     "movl $0x7fc00000, (%3)\n\t"
                     "cmova %1, %0"
                     : "+r"(r) : "r"(alt), "x"(t), "r"(m) : "memory", "cc");
    return r;
}

int main(void)
{
    static const float fx[] = { 1.0f, 1.5f, 2.0f, -3.0f, 0.0f, -0.0f, NAN, INFINITY };
    unsigned long long acc = 0;
    for (unsigned i = 0; i < sizeof fx / sizeof fx[0]; i++)
        for (unsigned k = 0; k < 4; k++) {
            float m = k == 3 ? NAN : fmem[k];
            volatile float cell = m;
            int bits = f_a(fx[i], &cell) | f_ae(fx[i], &cell) << 1 | f_b(fx[i], &cell) << 2 |
                       f_be(fx[i], &cell) << 3 | f_e(fx[i], &cell) << 4 | f_ne(fx[i], &cell) << 5 |
                       f_p(fx[i], &cell) << 6 | f_np(fx[i], &cell) << 7;
            volatile double dcell = k == 3 ? (double)NAN : dmem[k];
            int dbits = d_a(fx[i], &dcell) | d_ae(fx[i], &dcell) << 1 | d_b(fx[i], &dcell) << 2 |
                        d_be(fx[i], &dcell) << 3 | d_e(fx[i], &dcell) << 4 | d_ne(fx[i], &dcell) << 5 |
                        d_p(fx[i], &dcell) << 6 | d_np(fx[i], &dcell) << 7;
            printf("%u.%u f=%02x d=%02x\n", i, k, bits, dbits);
            acc = acc * 131 + (unsigned)bits * 256 + (unsigned)dbits;
        }
    volatile float s1 = 1.0f, s2 = 1.0f;
    printf("store_between %d %d\n", store_between(2.0f, &s1), store_between(0.5f, &s2));

    /* A hot loop of the shape winbench's structs has: compare against a constant, branch. */
    float v = 0.0f, step = 0.37f;
    int wraps = 0;
    for (int i = 0; i < 200000; i++) {
        v += step;
        if (v > 100.0f) { v = 0.0f; wraps++; }
    }
    printf("loop %d %.3f acc %llu\n", wraps, (double)v, acc);
    return 0;
}

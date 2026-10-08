/*
 * fist/fistp under each x87 rounding control, as src/jit.c emits them in a
 * run that has no round-to-nearest guard (x87_fist reads RC from the control
 * word: fcvtns, fcvtms, fcvtps or fcvtzs): the compilers' truncating cast
 * (fldcw ; fistp ; fldcw), a run with no fldcw at all under a mode set
 * earlier, and fistp after an add, whose run keeps the guard and leaves
 * other modes to the interpreter.  Every RC, 16-, 32- and 64-bit stores,
 * ties, out-of-range values, NaN and infinities; prints the stored integer
 * and the exception flags, against Rosetta.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static const double vals[] = { 0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 1.25, -1.25, 0.75, -0.75, 3.0, -3.0,
                               1e10, -1e10, 32767.5, -32768.5, 32768.0, -32769.0, 2147483647.5,
                               -2147483648.5, 2147483648.0, 9.2e18, -9.2e18, 1e19, 0.0, -0.0, 1e-300,
                               NAN, INFINITY, -INFINITY };
#define NV (sizeof vals / sizeof vals[0])

/* the truncating cast's shape, with the mode word given */
static void sandwich(double v, uint16_t cw, int64_t out[3], uint16_t sw[3])
{
    uint16_t old, mode = cw;
    int16_t s; int32_t l; int64_t q;
    __asm__ volatile("fnstcw %[old]\n\t" "fnclex\n\t"
                     "fldl %[v]\n\t" "fldcw %[mode]\n\t" "fistps %[s]\n\t" "fldcw %[old]\n\t" "fnstsw %[w0]\n\t" "fnclex\n\t"
                     "fldl %[v]\n\t" "fldcw %[mode]\n\t" "fistpl %[l]\n\t" "fldcw %[old]\n\t" "fnstsw %[w1]\n\t" "fnclex\n\t"
                     "fldl %[v]\n\t" "fldcw %[mode]\n\t" "fistpll %[q]\n\t" "fldcw %[old]\n\t" "fnstsw %[w2]\n\t" "fnclex"
                     : [old] "=m"(old), [s] "=m"(s), [l] "=m"(l), [q] "=m"(q), [w0] "=m"(sw[0]), [w1] "=m"(sw[1]), [w2] "=m"(sw[2])
                     : [v] "m"(v), [mode] "m"(mode) : "memory");
    out[0] = s; out[1] = l; out[2] = q;
}

/* fist (no pop) then fistp in one run, the mode set by the caller beforehand */
__attribute__((noinline)) static void run_only(double v, int64_t out[3], uint16_t *sw)
{
    int16_t s; int32_t l; int64_t q;
    __asm__ volatile("fnclex\n\t" "fldl %[v]\n\t" "fists %[s]\n\t" "fistl %[l]\n\t" "fistpll %[q]\n\t" "fnstsw %[w]"
                     : [s] "=m"(s), [l] "=m"(l), [q] "=m"(q), [w] "=m"(*sw) : [v] "m"(v) : "memory");
    out[0] = s; out[1] = l; out[2] = q;
}

/* an add first: this run keeps the round-to-nearest guard */
__attribute__((noinline)) static void after_add(double v, int64_t *out, uint16_t *sw)
{
    static const double quarter = 0.25;
    int32_t l;
    __asm__ volatile("fnclex\n\t" "fldl %[v]\n\t" "faddl %[q]\n\t" "fistpl %[l]\n\t" "fnstsw %[w]"
                     : [l] "=m"(l), [w] "=m"(*sw) : [v] "m"(v), [q] "m"(quarter) : "memory");
    *out = l;
}

int main(void)
{
    static const char *const rc_name[4] = { "near", "down", "up", "chop" };
    uint16_t base;
    __asm__ volatile("fnstcw %0" : "=m"(base));
    unsigned long long acc = 0;
    for (int rc = 0; rc < 4; rc++) {
        uint16_t cw = (uint16_t)((base & ~0x0c00) | (rc << 10));
        for (unsigned i = 0; i < NV; i++) {
            int64_t o[3]; uint16_t w[3];
            sandwich(vals[i], cw, o, w);
            printf("%s sandwich %-14g %d %d %lld | %04x %04x %04x\n", rc_name[rc], vals[i], (int)o[0], (int)o[1],
                   (long long)o[2], w[0] & 0x23f, w[1] & 0x23f, w[2] & 0x23f);
        }
        __asm__ volatile("fldcw %0" : : "m"(cw));
        for (int pass = 0; pass < 50; pass++)
            for (unsigned i = 0; i < NV; i++) {
                int64_t o[3], a; uint16_t w, wa;
                run_only(vals[i], o, &w);
                after_add(vals[i], &a, &wa);
                if (pass == 0)
                    printf("%s run %-14g %d %d %lld | %04x | add %d %04x\n", rc_name[rc], vals[i], (int)o[0], (int)o[1],
                           (long long)o[2], w & 0x23f, (int)a, wa & 0x23f);
                acc = acc * 31 + (unsigned long long)(o[0] ^ o[1] ^ o[2] ^ a) + w + wa;
            }
        __asm__ volatile("fldcw %0" : : "m"(base));
    }
    printf("acc %016llx\n", acc);
    return 0;
}

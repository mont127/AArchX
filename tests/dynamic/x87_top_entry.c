/*
 * x87 runs that src/jit.c translates for the TOP their block was entered with
 * and keeps in lanes (g_x87_spec, g_x87_lane), checked against themselves and
 * C, so it prints OK or the first difference:
 *   - a function with an x87 loop called with the stack empty and with one,
 *     two and three values pushed: every depth gives the depth-0 result and
 *     the values beneath come back (the deeper calls take the run's slow path,
 *     then the block is translated again without the known TOP);
 *   - fucomip then fcmovu / fcmovnu with NaN among the operands: the fast path
 *     takes the compare as ordered, the slow path does not, and what the next
 *     run knows of ST(0) must hold either way;
 *   - fninit inside a block: TOP 0 and every register empty after it.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

__attribute__((noinline)) static double poly(double x, int n)
{
    double r;
    __asm__ volatile("fldl %1\n\t"
                     "fld1\n\t"
                     "mov %2, %%ecx\n"
                     "1:\n\t"
                     "fmul %%st(1), %%st\n\t"
                     "fld1\n\t"
                     "faddp\n\t"
                     "fld %%st(0)\n\t"
                     "fmul %%st(2), %%st\n\t"
                     "fsubrp\n\t"
                     "fchs\n\t"
                     "dec %%ecx\n\t"
                     "jnz 1b\n\t"
                     "fstpl %0\n\t"
                     "fstp %%st(0)"
                     : "=m"(r) : "m"(x), "r"(n) : "rcx", "cc", "memory");
    return r;
}

static double with_depth(int depth, double x, int n, double below[3])
{
    for (int i = 0; i < depth; i++) {
        double v = 10.0 + i;
        __asm__ volatile("fldl %0" : : "m"(v));
    }
    double r = poly(x, n);
    for (int i = depth - 1; i >= 0; i--) __asm__ volatile("fstpl %0" : "=m"(below[i]));
    return r;
}

/* st0 = b; fcmovu (or fcmovnu) takes 1.0 when a and b compare unordered (ordered) */
__attribute__((noinline)) static double cmov_after_ucomi(double a, double b, int ordered_form)
{
    double r;
    static const double one = 1.0;
    if (ordered_form)
        __asm__ volatile("fldl %3\n\t" "fldl %2\n\t" "fldl %1\n\t"
                         "fucomip %%st(1), %%st\n\t"
                         "fcmovnu %%st(1), %%st\n\t"
                         "fstpl %0\n\t" "fstp %%st(0)"
                         : "=m"(r) : "m"(a), "m"(b), "m"(one) : "cc", "memory");
    else
        __asm__ volatile("fldl %3\n\t" "fldl %2\n\t" "fldl %1\n\t"
                         "fucomip %%st(1), %%st\n\t"
                         "fcmovu %%st(1), %%st\n\t"
                         "fstpl %0\n\t" "fstp %%st(0)"
                         : "=m"(r) : "m"(a), "m"(b), "m"(one) : "cc", "memory");
    return r;
}

/* a value pushed, fninit, then y pushed and stored: TOP 0, ST(0) = y, nothing else tagged */
__attribute__((noinline)) static double after_fninit(double x, double y, uint16_t *sw, uint16_t *tw)
{
    double r;
    uint8_t env[28];
    __asm__ volatile("fldl %3\n\t" "fninit\n\t" "fldl %4\n\t" "fnstenv %2\n\t" "fstpl %0\n\t" "fnstsw %1"
                     : "=m"(r), "=m"(*sw), "=m"(env) : "m"(x), "m"(y) : "memory");
    memcpy(tw, env + 8, 2);
    return r;
}

static int same(double a, double b) { return memcmp(&a, &b, sizeof a) == 0; }

int main(void)
{
    double ref[8];
    for (int k = 0; k < 8; k++) ref[k] = poly(0.5 + k * 0.125, 50);
    for (int d = 0; d < 4; d++)
        for (int rep = 0; rep < 3000; rep++) {
            double below[3] = { 0, 0, 0 };
            double r = with_depth(d, 0.5 + (rep & 7) * 0.125, 50, below);
            if (!same(r, ref[rep & 7])) { printf("depth %d rep %d: %.17g, want %.17g\n", d, rep, r, ref[rep & 7]); return 1; }
            for (int i = 0; i < d; i++)
                if (below[i] != 10.0 + i) { printf("depth %d rep %d: below[%d]=%.17g\n", d, rep, i, below[i]); return 1; }
        }

    static const double vals[] = { 1.5, -2.0, 0.0, 3.25, NAN, -0.5 };
    for (int rep = 0; rep < 4000; rep++)
        for (int i = 0; i < 6; i++)
            for (int form = 0; form < 2; form++) {
                double a = vals[i], b = vals[(i + rep) % 6];
                int unord = isnan(a) || isnan(b);
                double want = (form ? !unord : unord) ? 1.0 : b;
                double r = cmov_after_ucomi(a, b, form);
                if (!same(r, want) && !(isnan(r) && isnan(want))) {
                    printf("fcmov%s a=%g b=%g: %g, want %g\n", form ? "nu" : "u", a, b, r, want);
                    return 1;
                }
            }

    for (int rep = 0; rep < 2000; rep++) {
        uint16_t sw, tw;
        double y = rep * 0.25;
        double r = after_fninit(7.0, y, &sw, &tw);
        /* ST(0), physical 7 after the push, is tagged valid, or zero for 0.0 */
        uint16_t want_tw = (uint16_t)(0x3fff | (y == 0.0 ? 1u << 14 : 0));
        if (!same(r, y) || ((sw >> 11) & 7) != 0 || tw != want_tw) {
            printf("fninit rep %d: r=%g sw=%04x tw=%04x\n", rep, r, sw, tw);
            return 1;
        }
    }
    printf("OK\n");
    return 0;
}

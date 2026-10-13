/*
 * Loops that end in inc or dec and jz or jnz to their own head
 * (emit_incdec_jcc and emit_arith_incdec_jcc in src/jit.c).  One whose head
 * reads none of the flags goes straight back to its loop entry; any other
 * leaves through a chained edge.  Either way a double the loop carries in a
 * fixed lane must come out right, which it did not before.  It checks itself
 * against plain C and prints OK or the first difference:
 *   - dec/jnz and inc/jnz over memory, with 64-bit and 32-bit counters;
 *   - a multi-word add, where adc at the head reads the carry the inc left
 *     alone, so the flags are live round the loop;
 *   - a double carried round the loop by addsd and mulsd, with dec/jnz
 *     alone, with add, lea or xor before the dec, with a 32-bit counter,
 *     with adc at the head so that the edge is a chained one, with add
 *     reg, reg; dec; jnz taken as one, with movq, cvttsd2si and sqrtsd of
 *     the carried double in the loop, and with an instruction the
 *     translator calls out for (stmxcsr) between the mulsd and the dec;
 *   - setp at the head, which reads the flags the dec left;
 *   - packed single precision in the loop;
 *   - a loop left by a branch in its middle;
 *   - the exit taken on the first trip.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fail(const char *what, uint64_t got, uint64_t want)
{
    printf("%s: %#llx, want %#llx\n", what, (unsigned long long)got, (unsigned long long)want);
    return 1;
}

__attribute__((noinline)) static uint64_t sum_dec(const uint64_t *a, uint64_t n)
{
    uint64_t acc = 0;
    __asm__ volatile(
        "1:\n\t"
        "add -8(%[a],%[n],8), %[acc]\n\t"
        "dec %[n]\n\t"
        "jnz 1b\n"
        : [acc] "+r"(acc), [n] "+r"(n) : [a] "r"(a) : "cc", "memory");
    return acc;
}

__attribute__((noinline)) static uint64_t sum_inc_neg(const uint64_t *end, uint64_t n)
{
    uint64_t acc = 0;
    int64_t i = -(int64_t)n;
    __asm__ volatile(
        "1:\n\t"
        "xor (%[e],%[i],8), %[acc]\n\t"
        "rol $1, %[acc]\n\t"
        "inc %[i]\n\t"
        "jnz 1b\n"
        : [acc] "+r"(acc), [i] "+r"(i) : [e] "r"(end) : "cc", "memory");
    return acc;
}

__attribute__((noinline)) static uint64_t sum_dec32(const uint32_t *a, uint32_t n)
{
    uint64_t acc = 0, t;
    uint64_t c = n;
    __asm__ volatile(
        "1:\n\t"
        "mov -4(%[a],%[c],4), %k[t]\n\t"
        "add %[t], %[acc]\n\t"
        "dec %k[c]\n\t"
        "jnz 1b\n"
        : [acc] "+r"(acc), [c] "+r"(c), [t] "=&r"(t) : [a] "r"(a) : "cc", "memory");
    return acc;
}

/* r = a + b over n words, the carry out returned: adc reads the carry that inc and dec leave alone */
__attribute__((noinline)) static uint64_t add_words(uint64_t *r, const uint64_t *a, const uint64_t *b, uint64_t n)
{
    uint64_t i = 0, t, carry;
    __asm__ volatile(
        "clc\n"
        "1:\n\t"
        "mov (%[a],%[i],8), %[t]\n\t"
        "adc (%[b],%[i],8), %[t]\n\t"
        "mov %[t], (%[r],%[i],8)\n\t"
        "inc %[i]\n\t"
        "dec %[n]\n\t"
        "jnz 1b\n\t"
        "setc %b[t]\n\t"
        "movzbl %b[t], %k[t]\n"
        : [i] "+r"(i), [n] "+r"(n), [t] "=&q"(t)
        : [a] "r"(a), [b] "r"(b), [r] "r"(r)
        : "cc", "memory");
    carry = t;
    return carry;
}

/* acc = (acc + a[i]) * k from the top of a down, t taking whatever the integer part of the body does */
#define FLOOP(name, body) \
__attribute__((noinline)) static double name(const double *a, uint64_t n, uint64_t *tp) \
{ \
    double acc = 0.0; \
    const double *p = a + n; \
    uint64_t t = 0, u = 0x9e3779b97f4a7c15ull, v = 0xc2b2ae3d27d4eb4full; \
    uint32_t mx = 0; \
    __asm__ volatile("clc\n1:\n\t" body "\n" \
        : [acc] "+x"(acc), [n] "+r"(n), [p] "+r"(p), [t] "+r"(t), [u] "+r"(u), [mx] "+m"(mx) \
        : [a] "r"(a), [k] "x"(1.0000001), [v] "r"(v) : "cc", "memory"); \
    *tp = t ^ u ^ (uint64_t)(uintptr_t)p ^ (uint64_t)(uintptr_t)a; \
    return acc; \
}
#define FP "addsd -8(%[a],%[n],8), %[acc]\n\tmulsd %[k], %[acc]\n\t"
FLOOP(f_dec,      FP "dec %[n]\n\tjnz 1b")
FLOOP(f_add_dec,  FP "add $-8, %[p]\n\tdec %[n]\n\tjnz 1b")
FLOOP(f_lea_dec,  FP "lea -8(%[p]), %[p]\n\tdec %[n]\n\tjnz 1b")
FLOOP(f_xor_dec,  FP "xor %[n], %[t]\n\tdec %[n]\n\tjnz 1b")
FLOOP(f_dec32,    FP "dec %k[n]\n\tjnz 1b")
FLOOP(f_adc_dec,  "adc $0, %[t]\n\t" FP "dec %[n]\n\tjnz 1b")
FLOOP(f_adc_add,  "adc $0, %[t]\n\t" FP "add %[v], %[u]\n\tdec %[n]\n\tjnz 1b")
FLOOP(f_movq_dec, FP "movq %[acc], %[p]\n\txor %[p], %[t]\n\tdec %[n]\n\tjnz 1b")
FLOOP(f_cvt_dec,  FP "cvttsd2si %[acc], %[p]\n\tadd %[p], %[t]\n\tsqrtsd %[acc], %[acc]\n\tdec %[n]\n\tjnz 1b")
FLOOP(f_out_dec,  FP "stmxcsr %[mx]\n\tdec %[n]\n\tjnz 1b")

/* How many of n, n - 1, ... 1 have an even number of bits set in their low byte: setp at the head reads what the dec left. */
__attribute__((noinline)) static uint64_t parity_dec(uint64_t n)
{
    uint64_t acc = 0, t = 0;
    __asm__ volatile(
        "test %[n], %[n]\n"
        "1:\n\t"
        "setp %b[t]\n\t"
        "add %[t], %[acc]\n\t"
        "dec %[n]\n\t"
        "jnz 1b\n"
        : [acc] "+r"(acc), [n] "+r"(n), [t] "+q"(t) : : "cc");
    return acc;
}

__attribute__((noinline)) static void vadd_inc(float *r, const float *a, const float *b, int64_t n4)
{
    int64_t i = -n4;
    __asm__ volatile(
        "1:\n\t"
        "movups (%[a],%[i],8), %%xmm0\n\t"
        "movups (%[b],%[i],8), %%xmm1\n\t"
        "addps %%xmm1, %%xmm0\n\t"
        "movups %%xmm0, (%[r],%[i],8)\n\t"
        "add $1, %[i]\n\t"
        "inc %[i]\n\t"
        "jnz 1b\n"
        : [i] "+r"(i) : [a] "r"(a), [b] "r"(b), [r] "r"(r) : "cc", "memory", "xmm0", "xmm1");
}

/* the index of the first zero word, searching down from n: left through the middle or by the count */
__attribute__((noinline)) static uint64_t find_zero(const uint64_t *a, uint64_t n)
{
    __asm__ volatile(
        "1:\n\t"
        "cmpq $0, -8(%[a],%[n],8)\n\t"
        "je 2f\n\t"
        "dec %[n]\n\t"
        "jnz 1b\n"
        "2:\n"
        : [n] "+r"(n) : [a] "r"(a) : "cc", "memory");
    return n;
}

__attribute__((noinline)) static uint64_t spin(uint64_t n)
{
    uint64_t acc = 0;
    __asm__ volatile(
        "1:\n\t"
        "lea 3(%[acc],%[acc],2), %[acc]\n\t"
        "dec %[n]\n\t"
        "jnz 1b\n"
        : [acc] "+r"(acc), [n] "+r"(n) : : "cc");
    return acc;
}

int main(void)
{
    enum { N = 2048 };
    static uint64_t a[N], b[N], r[N];
    static uint32_t w[N];
    static double d[N];
    static float fa[N], fb[N], fr[N];
    uint64_t s = 99;
    for (int rep = 0; rep < 6; rep++) {
        for (int i = 0; i < N; i++) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            a[i] = s; b[i] = ~s + (uint64_t)(i & 3); w[i] = (uint32_t)(s >> 17);
            d[i] = (double)(int64_t)(s >> 40) / 3.0; fa[i] = (float)(i + rep); fb[i] = (float)(s >> 50);
        }
        uint64_t want = 0, got;
        for (int i = N; i > 0; i--) want += a[i - 1];
        if ((got = sum_dec(a, N)) != want) return fail("dec", got, want);
        want = 0;
        for (int i = 0; i < N; i++) { want ^= a[i]; want = want << 1 | want >> 63; }
        if ((got = sum_inc_neg(a + N, N)) != want) return fail("inc", got, want);
        want = 0;
        for (int i = N; i > 0; i--) want += w[i - 1];
        if ((got = sum_dec32(w, N)) != want) return fail("dec32", got, want);
        if ((got = sum_dec(a, 1)) != a[0]) return fail("dec once", got, a[0]);

        uint64_t carry = 0, cr[N];
        for (int i = 0; i < N; i++) {
            unsigned __int128 t = (unsigned __int128)a[i] + b[i] + carry;
            cr[i] = (uint64_t)t; carry = (uint64_t)(t >> 64);
        }
        if ((got = add_words(r, a, b, N)) != carry) return fail("adc carry", got, carry);
        if (memcmp(r, cr, sizeof r)) return fail("adc words", r[0], cr[0]);

        {
            double (*const f[8])(const double *, uint64_t, uint64_t *) =
                { f_dec, f_add_dec, f_lea_dec, f_xor_dec, f_dec32, f_adc_dec, f_adc_add, f_out_dec };
            static const char *const nm[8] = { "addsd dec", "addsd add dec", "addsd lea dec", "addsd xor dec", "addsd dec32",
                                               "addsd adc dec", "addsd adc add dec", "addsd stmxcsr dec" };
            double fw = 0.0;
            for (int i = N; i > 0; i--) { fw += d[i - 1]; fw *= 1.0000001; }
            for (int k = 0; k < 8; k++) {
                uint64_t t = 0, u = 0x9e3779b97f4a7c15ull, tg, x, y;
                uint64_t pd = k == 1 || k == 2 ? 0 : (uint64_t)(uintptr_t)(d + N) ^ (uint64_t)(uintptr_t)d;
                if (k == 3) for (uint64_t i = N; i > 0; i--) t ^= i;
                if (k == 6) for (int i = 0; i < N; i++) { uint64_t c = u + 0xc2b2ae3d27d4eb4full < u; u += 0xc2b2ae3d27d4eb4full; if (i < N - 1) t += c; }
                double fg = f[k](d, N, &tg);
                memcpy(&x, &fg, 8); memcpy(&y, &fw, 8);
                if (x != y) return fail(nm[k], x, y);
                if (tg != (t ^ u ^ pd)) return fail(nm[k], tg, t ^ u ^ pd);
            }
            /* the carried double read as bits each time round, and converted and rooted */
            uint64_t tm = 0, tc = 0, tg, x, y, last = 0;
            double fm = 0.0, fc = 0.0, fg;
            for (int i = N; i > 0; i--) {
                fm += d[i - 1]; fm *= 1.0000001;
                memcpy(&last, &fm, 8); tm ^= last;
            }
            fg = f_movq_dec(d, N, &tg);
            memcpy(&x, &fg, 8); memcpy(&y, &fm, 8);
            if (x != y) return fail("addsd movq dec", x, y);
            if (tg != (tm ^ 0x9e3779b97f4a7c15ull ^ last ^ (uint64_t)(uintptr_t)d)) return fail("addsd movq dec bits", tg, tm);
            for (int i = N; i > 0; i--) {
                fc += d[i - 1]; fc *= 1.0000001;
                last = (uint64_t)(int64_t)fc; tc += last;
                fc = __builtin_sqrt(fc);
            }
            fg = f_cvt_dec(d, N, &tg);
            memcpy(&x, &fg, 8); memcpy(&y, &fc, 8);
            if (x != y) return fail("addsd cvt sqrt dec", x, y);
            if (tg != (tc ^ 0x9e3779b97f4a7c15ull ^ last ^ (uint64_t)(uintptr_t)d)) return fail("addsd cvt sqrt dec sum", tg, tc);
        }
        {
            uint64_t want = 0, n0 = 1000 + (uint64_t)rep * 37;
            for (uint64_t v = n0; v > 0; v--) want += !(__builtin_popcountll(v & 0xff) & 1);
            if (parity_dec(n0) != want) return fail("setp at the head", parity_dec(n0), want);
        }
        vadd_inc(fr + N, fa + N, fb + N, N / 2);
        for (int i = 0; i < N; i++) {
            float t = fa[i] + fb[i];
            if (memcmp(&t, &fr[i], sizeof t)) return fail("addps", (uint64_t)i, 0);
        }

        a[rep * 100 + 7] = 0;
        if ((got = find_zero(a, N)) != (uint64_t)(rep * 100 + 8)) return fail("find", got, (uint64_t)(rep * 100 + 8));
        if ((got = find_zero(a + rep * 100 + 8, 40)) != 0) return fail("find none", got, 0);
    }
    {
        uint64_t x = 0;
        for (int i = 0; i < 1000; i++) x = 3 + 3 * x;
        if (spin(1000) != x) return fail("spin", spin(1000), x);
    }

    printf("OK\n");
    return 0;
}

/*
 * ptest, which src/jit.c translates: ZF is set when dst AND src is zero, CF
 * when src AND NOT dst is, and OF, SF, AF and PF are cleared.  Each pair of
 * vectors is tested with the source in a register and in memory, and the
 * flags read with pushf, with a jcc on ZF and on CF, with jbe and with setbe,
 * the ways translated code consumes them, after presetting every flag to one
 * so a flag that should be cleared is seen.  The pairs are zero, all ones,
 * single bits in either half, disjoint, overlapping and 200,000 random ones.
 * Then a loop of the shape SSE4.1 UTF-8 validation has: ptest against a mask
 * of high bits and a jnz out.
 *
 * It checks against the Intel manual rather than Rosetta, which leaves OF, SF,
 * AF and PF as they were; its ZF, CF and branches agree with this test.
 */
#include <stdint.h>
#include <stdio.h>

#define FLAGS_MASK 0x8d5ull /* OF SF ZF AF PF CF */

typedef struct { uint64_t lo, hi; } v128;

static void test_reg(const v128 *a, const v128 *b, uint64_t preset, uint64_t out[4])
{
    uint64_t fl, jz, jc, setb_nbe;
    __asm__ volatile(
        "movdqu (%4), %%xmm1\n\t" "movdqu (%5), %%xmm2\n\t"
        "sub $128, %%rsp\n\t"
        "push %6\n\t" "popfq\n\t"
        "ptest %%xmm2, %%xmm1\n\t"
        "pushfq\n\t" "pop %0\n\t"
        "push %6\n\t" "popfq\n\t"
        "ptest %%xmm2, %%xmm1\n\t"
        "mov $0, %1\n\t" "jnz 1f\n\t" "mov $1, %1\n\t" "1:\n\t"
        "ptest %%xmm2, %%xmm1\n\t"
        "mov $0, %2\n\t" "jnc 2f\n\t" "mov $1, %2\n\t" "2:\n\t"
        "ptest %%xmm2, %%xmm1\n\t"
        "setbe %b3\n\t" "movzbl %b3, %k3\n\t"
        "add $128, %%rsp\n\t"
        : "=&r"(fl), "=&r"(jz), "=&r"(jc), "=&q"(setb_nbe)
        : "r"(a), "r"(b), "r"(preset) : "xmm1", "xmm2", "cc", "memory");
    out[0] = fl & FLAGS_MASK; out[1] = jz; out[2] = jc; out[3] = setb_nbe;
}

static void test_mem(const v128 *a, const v128 *b, uint64_t preset, uint64_t out[2])
{
    uint64_t fl, ja;
    __asm__ volatile(
        "movdqu (%2), %%xmm3\n\t"
        "sub $128, %%rsp\n\t"
        "push %4\n\t" "popfq\n\t"
        "ptest (%3), %%xmm3\n\t"
        "pushfq\n\t" "pop %0\n\t"
        "ptest (%3), %%xmm3\n\t"
        "mov $0, %1\n\t" "jbe 1f\n\t" "mov $1, %1\n\t" "1:\n\t"
        "add $128, %%rsp\n\t"
        : "=&r"(fl), "=&r"(ja)
        : "r"(a), "r"(b), "r"(preset) : "xmm3", "cc", "memory");
    out[0] = fl & FLAGS_MASK; out[1] = ja;
}

/* index of the first 16-byte block with a byte >= 0x80, the validation loop's shape */
static long first_high(const uint8_t *p, long n)
{
    long i;
    __asm__ volatile(
        "xor %0, %0\n\t"
        "mov $0x8080808080808080, %%rax\n\t"
        "movq %%rax, %%xmm5\n\t" "punpcklqdq %%xmm5, %%xmm5\n\t"
        "1:\n\t"
        "cmp %2, %0\n\t" "jae 3f\n\t"
        "movdqu (%1,%0), %%xmm4\n\t"
        "ptest %%xmm5, %%xmm4\n\t"
        "jnz 2f\n\t"
        "add $16, %0\n\t" "jmp 1b\n\t"
        "3:\n\t" "mov $-1, %0\n\t"
        "2:\n\t"
        : "=&r"(i) : "r"(p), "r"(n) : "rax", "xmm4", "xmm5", "cc", "memory");
    return i;
}

static int bad;

static void check(const v128 *a, const v128 *b)
{
    int zf = ((a->lo & b->lo) | (a->hi & b->hi)) == 0;
    int cf = ((b->lo & ~a->lo) | (b->hi & ~a->hi)) == 0;
    uint64_t want = (uint64_t)zf << 6 | (uint64_t)cf;
    uint64_t r[4], m[2];
    test_reg(a, b, 0x8d7, r);
    test_mem(a, b, 0x8d7, m);
    if (r[0] != want || r[1] != (uint64_t)zf || r[2] != (uint64_t)cf || r[3] != (uint64_t)(zf | cf) ||
        m[0] != want || m[1] != (uint64_t)!(zf | cf)) {
        if (bad++ < 5)
            printf("ptest %016llx%016llx, %016llx%016llx: flags %llx/%llx jz %llu jc %llu setbe %llu ja %llu, "
                   "want flags %llx zf %d cf %d\n",
                   (unsigned long long)a->hi, (unsigned long long)a->lo, (unsigned long long)b->hi,
                   (unsigned long long)b->lo, (unsigned long long)r[0], (unsigned long long)m[0],
                   (unsigned long long)r[1], (unsigned long long)r[2], (unsigned long long)r[3],
                   (unsigned long long)m[1], (unsigned long long)want, zf, cf);
    }
}

int main(void)
{
    static const v128 v[] = {
        {0, 0}, {~0ull, ~0ull}, {1, 0}, {0, 1ull << 63}, {0x00ff00ff00ff00ffull, 0},
        {0xff00ff00ff00ff00ull, 0}, {0x00ff00ff00ff00ffull, 0x00ff00ff00ff00ffull},
        {0x0f0f0f0f0f0f0f0full, 0xf0f0f0f0f0f0f0f0ull}, {0x8000000000000000ull, 1},
        {0x123456789abcdef0ull, 0x0fedcba987654321ull},
    };
    const unsigned n = sizeof v / sizeof v[0];
    for (unsigned i = 0; i < n; i++)
        for (unsigned j = 0; j < n; j++)
            check(&v[i], &v[j]);
    uint64_t x = 88172645463325252ull;
    for (int k = 0; k < 200000; k++) {
        v128 a, b;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17; a.lo = x;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17; a.hi = x & (x >> 3);
        x ^= x << 13; x ^= x >> 7; x ^= x << 17; b.lo = x & (x >> 5) & (x >> 9);
        x ^= x << 13; x ^= x >> 7; x ^= x << 17; b.hi = x & (x >> 1) & (x >> 11);
        check(&a, &b);
        check(&b, &a);
    }
    static uint8_t buf[1 << 16];
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i * 7 % 128);
    long clean = first_high(buf, sizeof buf);
    buf[40007] = 0xc3;
    long hit = first_high(buf, sizeof buf);
    if (clean != -1 || hit != 40000) {
        printf("first_high %ld and %ld, want -1 and 40000\n", clean, hit);
        bad++;
    }
    if (!bad)
        printf("OK\n");
    return bad != 0;
}

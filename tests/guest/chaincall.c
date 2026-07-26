/* Register-map correctness across a chained call(imm) edge. */
#include "gsys.h"

__attribute__((noinline))
static g_u64 mix(g_u64 a, g_u64 b, g_u64 c, g_u64 d, g_u64 e, g_u64 f)
{
    a += b * 0x9e3779b97f4a7c15ULL;
    b ^= a >> 17;
    c += a + b;
    d ^= c << 3;
    e += d ^ (b + 0x1234567ULL);
    f ^= e + (a << 5);
    a -= f + (c >> 7);
    b += a ^ (d << 2);
    c ^= b + (e >> 11);
    d += c ^ (f << 1);
    e -= d + (a >> 13);
    f += e ^ (b << 4);
    a ^= f + (c << 6);
    b -= a + (d >> 9);
    c += b ^ (e << 7);
    d ^= c + (f >> 3);
    e += d ^ (a << 8);
    f -= e + (b >> 5);
    return a ^ b ^ c ^ d ^ e ^ f;
}

int main(void)
{

    g_u64 acc0 = 0x0123456789abcdefULL;
    g_u64 acc1 = 0xfedcba9876543210ULL;
    g_u64 acc2 = 0xa5a5a5a55a5a5a5aULL;
    g_u64 acc3 = 0x0f1e2d3c4b5a6978ULL;
    g_u64 out = 0;

    for (g_u64 i = 0; i < 4000; i++) {
        acc0 += i * 3 + 1;
        acc1 ^= acc0 + (i << 2);
        acc2 -= acc1 ^ (acc0 >> 5);
        acc3 += acc2 + (acc1 << 1);
        acc0 ^= acc3 + (i >> 1);
        acc1 += acc2 ^ (acc3 << 3);
        acc2 ^= acc0 + (acc3 >> 7);
        acc3 -= acc1 + (acc2 << 2);
        acc0 += acc2 ^ (acc1 >> 9);
        acc1 ^= acc3 + (acc0 << 4);
        acc2 += acc0 ^ (i << 6);
        acc3 ^= acc1 + (acc2 >> 11);

        out += mix(acc0, acc1, acc2, acc3, i, out);
        acc0 ^= out;
        acc1 += out >> 3;
        acc2 -= out << 1;
        acc3 ^= out + i;
    }

    g_putu64(out ^ acc0 ^ acc1 ^ acc2 ^ acc3);
    return 0;
}

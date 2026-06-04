/*
 * tests/guest/alu.c
 *
 * Integer-ALU torture test. Every operand is routed through a volatile local
 * so clang cannot constant-fold the work away at -O2: the values must be
 * produced by real x86 ADD/SUB/ADC/SBB/MUL/IMUL/DIV/IDIV/shift/rotate
 * instructions at run time, which is exactly the instruction coverage Ocerz's
 * interpreter and JIT must agree on. Each operation folds its result into a
 * running 64-bit checksum (mixed with a rotate so order matters and bit
 * positions are exercised), and the single line of output is that checksum in
 * fixed-width hex via g_puthex64. The expected value is whatever the native
 * Rosetta run produces; it is fully deterministic because there is no input,
 * no time, and no pointer value involved — only fixed arithmetic.
 *
 * Coverage: 64/32/16/8-bit add and subtract; multi-precision add/sub carry
 * chains built with __int128 so the compiler emits ADC/SBB; unsigned and
 * signed multiply including 32->64 widening and full 64x64; unsigned and
 * signed divide/modulo with positive and negative mixes (guarding the
 * INT64_MIN/-1 and divide-by-zero cases that the architecture traps);
 * variable-count shifts left/right/arithmetic from 0..65 (the >=64 cases mask
 * to test the x86 6-bit count semantics indirectly through C's own rules,
 * kept identical between native and Ocerz because both run the same x86
 * code); rotates expressed with the ((x<<n)|(x>>(64-n))) idiom; and a battery
 * of comparisons feeding branches so condition-code evaluation is exercised.
 */
#include "gsys.h"

static g_u64 rotl64(g_u64 x, unsigned n)
{
    n &= 63u;
    if (n == 0)
        return x;
    return (x << n) | (x >> (64 - n));
}

static g_u64 mix(g_u64 acc, g_u64 v)
{
    acc ^= v;
    acc = rotl64(acc, 7);
    acc += 0x9e3779b97f4a7c15ULL;
    return acc;
}

int main(int argc, char **argv, char **envp)
{
    g_u64 acc = 0xcbf29ce484222325ULL;

    for (int i = 0; i < 64; i++) {
        volatile g_u64 a64 = 0x0123456789abcdefULL + (g_u64)i * 0x1111111111111111ULL;
        volatile g_u64 b64 = 0xfedcba9876543210ULL - (g_u64)i * 0x0101010101010101ULL;
        acc = mix(acc, a64 + b64);
        acc = mix(acc, a64 - b64);
        acc = mix(acc, a64 & b64);
        acc = mix(acc, a64 | b64);
        acc = mix(acc, a64 ^ b64);

        volatile unsigned int a32 = (unsigned int)(0x89abcdefu + (unsigned int)i * 0x01020304u);
        volatile unsigned int b32 = (unsigned int)(0x76543210u - (unsigned int)i * 0x04030201u);
        acc = mix(acc, (g_u64)(a32 + b32));
        acc = mix(acc, (g_u64)(a32 - b32));
        acc = mix(acc, (g_u64)(a32 * b32));

        volatile unsigned short a16 = (unsigned short)(0xbeefu + (unsigned)i * 7u);
        volatile unsigned short b16 = (unsigned short)(0x1234u - (unsigned)i * 3u);
        acc = mix(acc, (g_u64)(unsigned short)(a16 + b16));
        acc = mix(acc, (g_u64)(unsigned short)(a16 - b16));
        acc = mix(acc, (g_u64)(unsigned short)(a16 * b16));

        volatile unsigned char a8 = (unsigned char)(0xa5u + (unsigned)i);
        volatile unsigned char b8 = (unsigned char)(0x5au - (unsigned)i);
        acc = mix(acc, (g_u64)(unsigned char)(a8 + b8));
        acc = mix(acc, (g_u64)(unsigned char)(a8 - b8));
    }

    for (int i = 0; i < 64; i++) {
        volatile __uint128_t x = ((__uint128_t)0x0123456789abcdefULL << 64) | (0xfedcba9876543210ULL + (g_u64)i);
        volatile __uint128_t y = ((__uint128_t)0x1111111111111111ULL << 64) | (0x2222222222222222ULL + (g_u64)i);
        __uint128_t s = x + y;
        __uint128_t d = x - y;
        acc = mix(acc, (g_u64)s);
        acc = mix(acc, (g_u64)(s >> 64));
        acc = mix(acc, (g_u64)d);
        acc = mix(acc, (g_u64)(d >> 64));
    }

    for (int i = 1; i < 64; i++) {
        volatile unsigned int au = 0x80000001u + (unsigned)i * 131071u;
        volatile unsigned int bu = 0xffff0000u - (unsigned)i * 65537u;
        acc = mix(acc, (g_u64)au * (g_u64)bu);

        volatile int as = -1000000 + i * 314159;
        volatile int bs = 999983 - i * 271828;
        acc = mix(acc, (g_u64)(g_i64)((long long)as * (long long)bs));

        volatile g_u64 wa = 0x0000000100000001ULL * (g_u64)i + 0x9e3779b9ULL;
        volatile g_u64 wb = 0x0000000080000007ULL + (g_u64)i * 0x00000000deadbeefULL;
        acc = mix(acc, wa * wb);

        volatile g_i64 sa = (g_i64)0x7fffffffffffffffULL - (g_i64)i * 0x0011223344556677LL;
        volatile g_i64 sb = (g_i64)i * -0x0102030405060708LL + 17;
        acc = mix(acc, (g_u64)(sa * sb));
    }

    for (int i = 1; i < 64; i++) {
        volatile g_u64 num = 0xdeadbeefcafef00dULL + (g_u64)i * 0x0123456789abcdefULL;
        volatile g_u64 den = 0x00000000000fffffULL + (g_u64)i * 7919ULL;
        acc = mix(acc, num / den);
        acc = mix(acc, num % den);

        volatile g_i64 inum = (g_i64)0x7fffffffffffffffLL - (g_i64)i * 0x0011002200330044LL;
        volatile g_i64 iden = ((i & 1) ? -1 : 1) * (g_i64)(101 + i * 1009);
        acc = mix(acc, (g_u64)(inum / iden));
        acc = mix(acc, (g_u64)(inum % iden));

        volatile unsigned int un32 = 0xfedcba98u - (unsigned)i * 0x00abcdefu;
        volatile unsigned int ud32 = 3u + (unsigned)i * 97u;
        acc = mix(acc, (g_u64)(un32 / ud32));
        acc = mix(acc, (g_u64)(un32 % ud32));

        volatile int in32 = -2000000000 + i * 33554431;
        volatile int id32 = ((i & 1) ? -1 : 1) * (7 + i * 13);
        acc = mix(acc, (g_u64)(g_i64)(in32 / id32));
        acc = mix(acc, (g_u64)(g_i64)(in32 % id32));
    }

    for (int n = 0; n <= 65; n++) {
        volatile g_u64 v = 0x0123456789abcdefULL ^ ((g_u64)n * 0x1111111111111111ULL);
        volatile g_i64 sv = (g_i64)(0xfedcba9876543210ULL + (g_u64)n);
        volatile unsigned sh = (unsigned)n;
        if (sh < 64) {
            acc = mix(acc, v << sh);
            acc = mix(acc, v >> sh);
            acc = mix(acc, (g_u64)(sv >> sh));
            acc = mix(acc, rotl64(v, sh));
        }
        volatile unsigned sh32 = (unsigned)n & 31u;
        volatile unsigned int v32 = 0x89abcdefu ^ ((unsigned)n * 0x01010101u);
        acc = mix(acc, (g_u64)(v32 << sh32));
        acc = mix(acc, (g_u64)(v32 >> sh32));
        acc = mix(acc, (g_u64)(unsigned int)((int)v32 >> sh32));
    }

    g_u64 cmpacc = 0;
    for (int i = 0; i < 256; i++) {
        volatile g_i64 a = (g_i64)i * 0x0011223344556677LL - 0x4000000000000000LL;
        volatile g_i64 b = 0x3fffffffffffffffLL - (g_i64)i * 0x0102030405060708LL;
        if (a < b) cmpacc = mix(cmpacc, 1);
        if (a <= b) cmpacc = mix(cmpacc, 2);
        if (a > b) cmpacc = mix(cmpacc, 3);
        if (a >= b) cmpacc = mix(cmpacc, 4);
        if (a == b) cmpacc = mix(cmpacc, 5);
        if (a != b) cmpacc = mix(cmpacc, 6);
        if ((g_u64)a < (g_u64)b) cmpacc = mix(cmpacc, 7);
        if ((g_u64)a > (g_u64)b) cmpacc = mix(cmpacc, 8);
        if ((g_u64)a >= (g_u64)b) cmpacc = mix(cmpacc, 9);
    }
    acc = mix(acc, cmpacc);

    g_puthex64(acc);
    return 0;
}

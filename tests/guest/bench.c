/* Throughput benchmark, not a correctness test. */
#include "gsys.h"

static g_u64 fib(unsigned n)
{
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}

int main(int argc, char **argv)
{
    const char *w = argc > 1 ? argv[1] : "call";
    g_u64 r = 0;

    if (w[0] == 'c') {
        r = fib(34);
    } else if (w[0] == 'a') {
        g_u64 a = 1, b = 2, c = 3;
        for (g_u64 i = 0; i < 300000000ULL; i++) {
            a += i; b ^= a; c -= b; a |= (c >> 3); b &= 0xffffffffULL;
        }
        r = a ^ b ^ c;
    } else if (w[0] == 'm') {
        static g_u64 buf[131072];
        for (g_u64 i = 0; i < 40000000ULL; i++) {
            g_u64 k = i & 131071;
            buf[k] = buf[(k * 7 + 1) & 131071] + i;
        }
        for (g_u64 i = 0; i < 131072; i++) r += buf[i];
    } else {
        g_u64 x = 12345;
        for (g_u64 i = 0; i < 200000000ULL; i++) {
            x = x * 1103515245ULL + 12345ULL;
            if (x & 0x10000) r += 3; else if (x & 0x20000) r -= 1; else r ^= x >> 7;
        }
    }
    g_putu64(r);
    return 0;
}

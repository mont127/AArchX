/* Adversarial gate for the return-address-stack predictor. */
#include "gsys.h"

static g_u64 helper(g_u64 x) { return (x * 3u) + 0x9e3779b9u; }
static g_u64 (*volatile g_h)(g_u64) = helper;

static g_u64 rec(unsigned n)
{
    if (n == 0)
        return 0x9e3779b9u;
    g_u64 a = rec(n - 1);
    g_u64 b = g_h(a);
    g_u64 c = 0;
    if ((n & 15u) == 0)
        c = rec((n >> 4) + 1);
    return (a * 1000003u) ^ b ^ c ^ ((g_u64)n << 5);
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    g_u64 acc = 0x1234567u;
    for (int i = 0; i < 200; i++) {
        acc += rec(200);
        acc ^= (acc << 1) + 0x9e3779b97f4a7c15ull;
        acc = (acc >> 7) | (acc << 57);
    }
    g_putu64(acc);
    return 0;
}

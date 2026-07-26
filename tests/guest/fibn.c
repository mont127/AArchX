/* Two-point call/return benchmark. */
#include "gsys.h"

static g_u64 fib(unsigned n)
{
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}

static g_u64 nodes_for(unsigned n)
{
    g_u64 a = 0, b = 1;
    for (unsigned i = 0; i < n + 1u; i++) {
        g_u64 t = a + b;
        a = b; b = t;
    }
    return 2u * a - 1u;
}

int main(int argc, char **argv)
{
    unsigned n = 34;

    if (argc > 1) {
        unsigned v = 0;
        const char *p = argv[1];

        for (; *p >= '0' && *p <= '9'; p++)
            v = v * 10u + (unsigned)(*p - '0');
        if (v)
            n = v;
    }

    g_u64 r = fib(n);
    g_putu64(r);
    g_putu64(nodes_for(n));
    return 0;
}

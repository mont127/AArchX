/* Control-flow torture test. */
#include "gsys.h"

static g_u64 op_add(g_u64 a, g_u64 b) { return a + b; }
static g_u64 op_sub(g_u64 a, g_u64 b) { return a - b; }
static g_u64 op_mul(g_u64 a, g_u64 b) { return a * b; }
static g_u64 op_xor(g_u64 a, g_u64 b) { return a ^ b; }
static g_u64 op_or(g_u64 a, g_u64 b) { return a | b; }
static g_u64 op_and(g_u64 a, g_u64 b) { return a & b; }

typedef g_u64 (*binop)(g_u64, g_u64);

static g_u64 switch_kernel(int sel, g_u64 x)
{
    switch (sel) {
    case 0: return x + 1;
    case 1: return x * 3;
    case 2: return x ^ 0x55;
    case 3: return x - 7;
    case 4: return x << 2;
    case 5: return x >> 1;
    case 6: return x | 0x100;
    case 7: return x & 0xff;
    case 8: return x + 0x1000;
    case 9: return x * 7;
    case 10: return x - 0x33;
    case 11: return x ^ 0xaa55;
    default: return x;
    }
}

static unsigned ackermann(unsigned m, unsigned n)
{
    if (m == 0)
        return n + 1;
    if (n == 0)
        return ackermann(m - 1, 1);
    return ackermann(m - 1, ackermann(m, n - 1));
}

static g_u64 fib(unsigned n)
{
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}

int main(int argc, char **argv, char **envp)
{
    g_u64 sacc = 0;
    for (int k = 0; k < 240; k++) {
        volatile int sel = k % 12;
        sacc += switch_kernel(sel, (g_u64)k * 1000003ULL + 1);
    }
    g_putu64(sacc);

    volatile binop tab[6] = { op_add, op_sub, op_mul, op_xor, op_or, op_and };
    g_u64 iacc = 0x1234;
    for (int k = 0; k < 600; k++) {
        binop f = tab[k % 6];
        iacc = f(iacc, (g_u64)k * 2654435761ULL + 7);
    }
    g_putu64(iacc);

    g_u64 loopacc = 0;
    for (int i = 1; i <= 50; i++) {
        for (int j = 1; j <= 50; j++) {
            if (((i * j) & 3) == 0)
                loopacc += (g_u64)(i ^ j);
            else
                loopacc += (g_u64)(i + j);
        }
    }
    g_putu64(loopacc);

    g_putu64((g_u64)ackermann(2, 3));
    g_putu64(fib(25));
    return 0;
}

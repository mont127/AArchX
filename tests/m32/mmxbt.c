/* MMX moves (movq/movd through registers and memory, emms) and bt/bts/btr/btc on registers and memory in i386
 * code, compiled by the JIT (src/jit.c emit_mmx, emit_bt) */
#include <stdio.h>

static unsigned long long g_q[4] = { 0x0123456789abcdefull, 0, 0, 0 };
static unsigned g_bits[4] = { 0x80000001u, 0x00ff00ffu, 0, 0 };

int main(void)
{
    unsigned sum = 0;
    for (unsigned i = 0; i < 1000; i++) {
        unsigned lo, x = i * 2654435761u;
        __asm__ volatile("movq (%1), %%mm0\n\tmovq %%mm0, 8(%1)\n\tmovd %2, %%mm1\n\tmovq %%mm1, 16(%1)\n\t"
                         "movd %%mm0, %0\n\temms" : "=r"(lo) : "r"(g_q), "r"(x) : "memory", "mm0", "mm1");
        unsigned char c1, c2;
        unsigned r = x;
        __asm__ volatile("btl %3, %2\n\tsetc %0\n\tbtsl $5, %2\n\tbtl %4, (%5)\n\tsetc %1\n\tbtcl $9, 4(%5)"
                         : "=q"(c1), "=q"(c2), "+r"(r) : "r"(i & 31), "r"(i & 63), "r"(g_bits) : "cc", "memory");
        sum = sum * 33 + lo + (unsigned)g_q[1] + (unsigned)g_q[2] + c1 * 3 + c2 * 5 + r + g_bits[1];
    }
    printf("mmxbt %08x\n", sum);
    return 0;
}

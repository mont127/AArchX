/* LDDQU (F2 0F F0): an unaligned 16-byte load.  Chromium's renderer uses it
 * in its SIMD UTF-8 scanner; ocerz once refused to decode it and every
 * renderer Steam launched died.  Loads from odd offsets, a 16-byte granule
 * crossing and a page crossing; golden from the native run. */
#include "gsys.h"

static unsigned char buf[8192] __attribute__((aligned(4096)));

static void load_show(const unsigned char *p)
{
    g_u64 lo, hi;
    __asm__ __volatile__("lddqu (%[p]), %%xmm1\n\tmovq %%xmm1, %[lo]\n\tpsrldq $8, %%xmm1\n\tmovq %%xmm1, %[hi]"
                         : [lo] "=r"(lo), [hi] "=r"(hi) : [p] "r"(p) : "xmm1", "memory");
    g_putu64_nonl(lo); g_puts(" "); g_putu64(hi);
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    for (int i = 0; i < (int)sizeof buf; i++) buf[i] = (unsigned char)(i * 7 + 3);
    load_show(buf);
    load_show(buf + 1);
    load_show(buf + 7);
    load_show(buf + 15);
    load_show(buf + 4096 - 5);
    load_show(buf + 4096 - 1);
    /* register form and a reg-indexed address, the shapes the scanner emits */
    g_u64 v;
    __asm__ __volatile__("mov $0x123, %%rax\n\tlddqu 5(%[p],%%rax,2), %%xmm2\n\tmovq %%xmm2, %[v]"
                         : [v] "=r"(v) : [p] "r"(buf) : "rax", "xmm2", "memory");
    g_putu64(v);
    return 0;
}

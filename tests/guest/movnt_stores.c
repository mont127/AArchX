/* Non-temporal stores: movntps/movntpd (0F 2B), movntdq (66 0F E7), movnti
 * (0F C3) and their VEX forms.  Steam's SDL3.dll memset streams with movntps
 * and the decoder used to reject 0F 2B.  The cache hint is not observable;
 * the stores must simply land.  Goldens come from the native binary. */
#include "gsys.h"

static float src[4] __attribute__((aligned(16))) = { 1.0f, 2.0f, 3.0f, 4.0f };
static double dsrc[2] __attribute__((aligned(16))) = { 5.0, 6.0 };
static unsigned int isrc[4] __attribute__((aligned(16))) = { 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u };

int main(void)
{
    unsigned char buf[128] __attribute__((aligned(32)));
    for (int i = 0; i < 128; i++) buf[i] = 0xee;
    __asm__ volatile("movaps (%0), %%xmm0\n\tmovntps %%xmm0, (%1)\n\tmovntps %%xmm0, 16(%1)\n\t"
                     "movapd (%2), %%xmm1\n\tmovntpd %%xmm1, 32(%1)\n\t"
                     "movdqa (%3), %%xmm2\n\tmovntdq %%xmm2, 48(%1)\n\t"
                     "movl $0x55667788, %%eax\n\tmovnti %%eax, 64(%1)\n\t"
                     "movq $0x1122334455667788, %%rax\n\tmovnti %%rax, 72(%1)\n\t"
                     "vmovaps (%0), %%xmm3\n\tvmovntps %%xmm3, 80(%1)\n\tvmovntdq %%xmm2, 96(%1)\n\tvzeroupper"
                     :: "r"(src), "r"(buf), "r"(dsrc), "r"(isrc) : "memory", "rax", "xmm0", "xmm1", "xmm2", "xmm3");
    for (int off = 0; off < 112; off += 8) {
        g_u64 v; __builtin_memcpy(&v, buf + off, 8);
        g_puthex64(v);
    }
    g_puts("done\n");
    return 0;
}

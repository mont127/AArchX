/* memset/memcpy-style self-loop blocks (store/load + add + sub + jae) over
 * misaligned pointers: the ordered-tier acquire/release access at the loop
 * head faults on a 16-byte crossing in the middle of a self-looping block.
 * Golden from Rosetta. */
#include "gsys.h"
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
static char src[512] __attribute__((aligned(64)));
static char dst[512] __attribute__((aligned(64)));
static g_u64 sum(const char *p, int n) { g_u64 s = 0; for (int i = 0; i < n; i++) s = s * 131 + (unsigned char)p[i]; return s; }
int main(void){
    for (int i = 0; i < 512; i++) src[i] = (char)(i * 13 + 5);
    for (int off = 0; off < 24; off++) {
        for (int i = 0; i < 512; i++) dst[i] = 0;
        char *d = dst + off; g_u64 n = 200; g_u64 v = 0x1122334455667788ULL;
        /* memset 8-byte loop */
        __asm__ volatile("1:\n\tmovq %2, (%0)\n\taddq $8, %0\n\tsubq $8, %1\n\tjae 1b" : "+r"(d), "+r"(n) : "r"(v) : "memory", "cc");
        put("memset ", sum(dst, 512)); put("d ", (g_u64)(d - dst)); put("n ", n);
        /* memcpy 8-byte loop */
        d = dst + off; const char *s = src + (off * 3 % 16); n = 200;
        __asm__ volatile("1:\n\tmovq (%2), %%r8\n\taddq $8, %2\n\tmovq %%r8, (%0)\n\taddq $8, %0\n\tsubq $8, %1\n\tjae 1b" : "+r"(d), "+r"(n), "+r"(s) : : "memory", "cc", "r8");
        put("memcpy ", sum(dst, 512)); put("d ", (g_u64)(d - dst)); put("s ", (g_u64)(s - src));
        /* 4-byte and 2-byte variants */
        d = dst + off; n = 100; 
        __asm__ volatile("1:\n\tmovl %k2, (%0)\n\taddq $4, %0\n\tsubq $4, %1\n\tjae 1b" : "+r"(d), "+r"(n) : "r"(v) : "memory", "cc");
        put("memset4 ", sum(dst, 512));
        d = dst + off; n = 60;
        __asm__ volatile("1:\n\tmovw %w2, (%0)\n\taddq $2, %0\n\tsubq $2, %1\n\tjae 1b" : "+r"(d), "+r"(n) : "r"(v) : "memory", "cc");
        put("memset2 ", sum(dst, 512));
        /* SSE 16-byte copy loop */
        d = dst + off; s = src + (off * 5 % 16); n = 160;
        __asm__ volatile("1:\n\tmovdqu (%2), %%xmm1\n\taddq $16, %2\n\tmovdqu %%xmm1, (%0)\n\taddq $16, %0\n\tsubq $16, %1\n\tjae 1b" : "+r"(d), "+r"(n), "+r"(s) : : "memory", "cc", "xmm1");
        put("sse16 ", sum(dst, 512));
        /* movq xmm loop */
        d = dst + off; s = src + (off * 7 % 16); n = 160;
        __asm__ volatile("1:\n\tmovq (%2), %%xmm2\n\taddq $8, %2\n\tmovq %%xmm2, (%0)\n\taddq $8, %0\n\tsubq $8, %1\n\tjae 1b" : "+r"(d), "+r"(n), "+r"(s) : : "memory", "cc", "xmm2");
        put("sse8 ", sum(dst, 512));
        /* load-accumulate loop (loads only) */
        s = src + off; n = 320; g_u64 acc = 0;
        __asm__ volatile("1:\n\taddq (%1), %0\n\taddq $8, %1\n\tsubq $8, %2\n\tjae 1b" : "+r"(acc), "+r"(s), "+r"(n) : : "memory", "cc");
        put("acc ", acc);
    }
    return 0;
}

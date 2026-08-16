/* bsf/bsr/tzcnt/lzcnt/popcnt (64/32, reg and mem source, zero and non-zero
 * inputs, dead and live flags) and pmovmskb; golden from Rosetta. */
#include "gsys.h"
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
int main(void){
    volatile g_u64 vals[8] = { 0, 1, 0x80, 0x8000000000000000ULL, 0x00f0f0f0f0f0f0f0ULL, 0xffffffffffffffffULL, 0x100000000ULL, 0x0000000080000001ULL };
    for (int i = 0; i < 8; i++) {
        g_u64 v = vals[i], r, z;
        /* 64-bit reg source, dead flags: dst preloaded with a marker */
        r = 0x1122334455667788ULL; __asm__ volatile("bsfq %1, %0" : "+r"(r) : "r"(v) : "cc"); put("bsfq r ", r);
        r = 0x1122334455667788ULL; __asm__ volatile("bsrq %1, %0" : "+r"(r) : "r"(v) : "cc"); put("bsrq r ", r);
        r = 0x1122334455667788ULL; __asm__ volatile("tzcntq %1, %0" : "+r"(r) : "r"(v) : "cc"); put("tzcntq r ", r);
        r = 0x1122334455667788ULL; __asm__ volatile("lzcntq %1, %0" : "+r"(r) : "r"(v) : "cc"); put("lzcntq r ", r);
        r = 0x1122334455667788ULL; __asm__ volatile("popcntq %1, %0" : "+r"(r) : "r"(v) : "cc"); put("popcntq r ", r);
        /* 32-bit forms (upper half of the marker must survive when the source is 0 for bsf/bsr) */
        r = 0x1122334455667788ULL; __asm__ volatile("bsfl %k1, %k0" : "+r"(r) : "r"(v) : "cc"); put("bsfl r ", r);
        r = 0x1122334455667788ULL; __asm__ volatile("bsrl %k1, %k0" : "+r"(r) : "r"(v) : "cc"); put("bsrl r ", r);
        r = 0x1122334455667788ULL; __asm__ volatile("tzcntl %k1, %k0" : "+r"(r) : "r"(v) : "cc"); put("tzcntl r ", r);
        r = 0x1122334455667788ULL; __asm__ volatile("lzcntl %k1, %k0" : "+r"(r) : "r"(v) : "cc"); put("lzcntl r ", r);
        r = 0x1122334455667788ULL; __asm__ volatile("popcntl %k1, %k0" : "+r"(r) : "r"(v) : "cc"); put("popcntl r ", r);
        /* memory source */
        r = 0x1122334455667788ULL; __asm__ volatile("bsfq %1, %0" : "+r"(r) : "m"(vals[i]) : "cc"); put("bsfq m ", r);
        r = 0x1122334455667788ULL; __asm__ volatile("bsrl %1, %k0" : "+r"(r) : "m"(*(volatile g_u32 *)&vals[i]) : "cc"); put("bsrl m ", r);
        /* live flags: ZF via setz (goes through the exact path) */
        r = 5; __asm__ volatile("bsfq %2, %0\n\tsetz %b1" : "+r"(r), "=r"(z) : "r"(v) : "cc"); put("bsfq zf ", z & 0xff); put("bsfq zf dst ", r);
        r = 5; __asm__ volatile("tzcntq %2, %0\n\tsetc %b1" : "+r"(r), "=r"(z) : "r"(v) : "cc"); put("tzcnt cf ", z & 0xff);
        r = 5; __asm__ volatile("popcntq %2, %0\n\tsetz %b1" : "+r"(r), "=r"(z) : "r"(v) : "cc"); put("popcnt zf ", z & 0xff);
    }
    /* pmovmskb over assorted byte patterns */
    static const g_u64 pats[6][2] = { {0,0}, {0x8080808080808080ULL,0x8080808080808080ULL}, {0x0080000000800000ULL,0x8000000000000080ULL},
                                      {0x7f7f7f7f7f7f7f7fULL,0xffffffffffffffffULL}, {0xff00ff00ff00ff00ULL,0x00ff00ff00ff00ffULL}, {0x0123456789abcdefULL,0xfedcba9876543210ULL} };
    for (int i = 0; i < 6; i++) {
        g_u64 m64 = 0x1122334455667788ULL; g_u32 m32 = 0xdeadbeef;
        __asm__ volatile("movdqu %1, %%xmm3\n\tpmovmskb %%xmm3, %k0" : "+r"(m64) : "m"(pats[i]) : "xmm3");
        put("pmovmskb 32 ", m64);
        __asm__ volatile("movdqu %1, %%xmm5\n\tpmovmskb %%xmm5, %q0" : "+r"(m32) : "m"(pats[i]) : "xmm5");
        put("pmovmskb 64 ", m32);
    }
    return 0;
}

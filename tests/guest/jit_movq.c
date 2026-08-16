/* movq in all forms (xmm<-r64, r64<-xmm, xmm<-xmm, xmm<-m64, m64<-xmm) with
 * the upper half of xmm destinations checked; pshufd/pinsrd/pmovsxbd shapes
 * for future inlining.  Golden from Rosetta. */
#include "gsys.h"
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
int main(void){
    static const g_u64 vals[4] = { 0x0123456789abcdefULL, 0xffffffffffffffffULL, 0x8000000000000000ULL, 0x00000000000000ffULL };
    volatile g_u64 mem[2];
    g_u64 lo, hi;
    for (int i = 0; i < 4; i++) {
        g_u64 v = vals[i];
        /* xmm <- r64: upper zeroed even if xmm had garbage */
        __asm__ volatile("pcmpeqb %%xmm1, %%xmm1\n\tmovq %2, %%xmm1\n\tmovq %%xmm1, %0\n\tpshufd $0xee, %%xmm1, %%xmm2\n\tmovq %%xmm2, %1"
                         : "=r"(lo), "=r"(hi) : "r"(v) : "xmm1", "xmm2"); put("movq x<-r lo ", lo); put("movq x<-r hi ", hi);
        /* xmm <- xmm */
        __asm__ volatile("pcmpeqb %%xmm3, %%xmm3\n\tmovq %2, %%xmm1\n\tpcmpeqb %%xmm1, %%xmm1\n\tpslldq $8, %%xmm1\n\tmovq %2, %%xmm4\n\tmovq %%xmm4, %%xmm3\n\tmovq %%xmm3, %0\n\tpshufd $0xee, %%xmm3, %%xmm2\n\tmovq %%xmm2, %1"
                         : "=r"(lo), "=r"(hi) : "r"(v) : "xmm1", "xmm2", "xmm3", "xmm4"); put("movq x<-x lo ", lo); put("movq x<-x hi ", hi);
        /* xmm <- m64 */
        mem[0] = v; mem[1] = 0x1111111111111111ULL;
        __asm__ volatile("pcmpeqb %%xmm5, %%xmm5\n\tmovq %2, %%xmm5\n\tmovq %%xmm5, %0\n\tpshufd $0xee, %%xmm5, %%xmm2\n\tmovq %%xmm2, %1"
                         : "=r"(lo), "=r"(hi) : "m"(mem[0]) : "xmm5", "xmm2"); put("movq x<-m lo ", lo); put("movq x<-m hi ", hi);
        /* m64 <- xmm and r64 <- xmm */
        mem[0] = 0; mem[1] = 0x2222222222222222ULL;
        __asm__ volatile("movq %1, %%xmm6\n\tpcmpeqb %%xmm7, %%xmm7\n\tpunpcklqdq %%xmm7, %%xmm6\n\tmovq %%xmm6, %0" : "=m"(mem[0]) : "r"(v) : "xmm6", "xmm7");
        put("movq m<-x m0 ", mem[0]); put("movq m<-x m1 ", mem[1]);
        __asm__ volatile("movq %1, %%xmm6\n\tmovq %%xmm6, %0" : "=r"(lo) : "r"(v ^ 0x55) : "xmm6"); put("movq r<-x ", lo);
        /* pshufd / pinsrd / pmovsxbd shapes */
        __asm__ volatile("movq %2, %%xmm1\n\tpshufd $0x1b, %%xmm1, %%xmm2\n\tmovq %%xmm2, %0\n\tpshufd $0xee, %%xmm2, %%xmm2\n\tmovq %%xmm2, %1"
                         : "=r"(lo), "=r"(hi) : "r"(v) : "xmm1", "xmm2"); put("pshufd lo ", lo); put("pshufd hi ", hi);
        __asm__ volatile("movq %2, %%xmm1\n\tpunpcklqdq %%xmm1, %%xmm1\n\tpaddq %%xmm1, %%xmm1\n\tpshufd $0x39, %%xmm1, %%xmm2\n\tmovq %%xmm2, %0\n\tpshufd $0xee, %%xmm2, %%xmm2\n\tmovq %%xmm2, %1"
                         : "=r"(lo), "=r"(hi) : "r"(v) : "xmm1", "xmm2"); put("pshufd39 lo ", lo); put("pshufd39 hi ", hi);
        __asm__ volatile("movq %2, %%xmm1\n\tpshufd $0xc6, %%xmm1, %%xmm2\n\tmovq %%xmm2, %0\n\tpshufd $0xee, %%xmm2, %%xmm2\n\tmovq %%xmm2, %1"
                         : "=r"(lo), "=r"(hi) : "r"(v) : "xmm1", "xmm2"); put("pshufdc6 lo ", lo); put("pshufdc6 hi ", hi);
        __asm__ volatile("movq %2, %%xmm1\n\tpshufd $0x00, %%xmm1, %%xmm2\n\tmovq %%xmm2, %0\n\tpshufd $0x55, %%xmm1, %%xmm2\n\tmovq %%xmm2, %1"
                         : "=r"(lo), "=r"(hi) : "r"(v) : "xmm1", "xmm2"); put("pshufd00 lo ", lo); put("pshufd55 lo ", hi);
        __asm__ volatile("movq %2, %%xmm1\n\tpshufd $0x4e, %%xmm1, %%xmm2\n\tmovq %%xmm2, %0\n\tpshufd $0xb1, %%xmm1, %%xmm2\n\tmovq %%xmm2, %1"
                         : "=r"(lo), "=r"(hi) : "r"(v) : "xmm1", "xmm2"); put("pshufd4e lo ", lo); put("pshufdb1 lo ", hi);
        __asm__ volatile("movq %2, %%xmm1\n\tpinsrd $2, %k2, %%xmm1\n\tmovq %%xmm1, %0\n\tpshufd $0xee, %%xmm1, %%xmm2\n\tmovq %%xmm2, %1"
                         : "=r"(lo), "=r"(hi) : "r"(v) : "xmm1", "xmm2"); put("pinsrd lo ", lo); put("pinsrd hi ", hi);
        __asm__ volatile("movq %2, %%xmm1\n\tpmovsxbd %%xmm1, %%xmm2\n\tmovq %%xmm2, %0\n\tpshufd $0xee, %%xmm2, %%xmm2\n\tmovq %%xmm2, %1"
                         : "=r"(lo), "=r"(hi) : "r"(v) : "xmm1", "xmm2"); put("pmovsxbd lo ", lo); put("pmovsxbd hi ", hi);
    }
    return 0;
}

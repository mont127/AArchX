/* Accesses crossing 16-byte boundaries at every offset, in every width the
 * JIT has a fast form for (GPR 2/4/8, movq/movsd/movss/movdqu/movups), with
 * loads feeding pinned-xmm arithmetic and stores read back through GPRs.
 * Under the ordered (multi-observer) memory tier Apple silicon faults on
 * acquire/release accesses that cross a 16-byte boundary; the JIT marks the
 * block and retranslates it with alignment checks -- this covers that path
 * (run under OCERZ_NO_PLAIN_MEM=1 as well as plain).  Golden from Rosetta. */
#include "gsys.h"
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
static char buf[256] __attribute__((aligned(64)));
int main(void){
    for (int i = 0; i < 256; i++) buf[i] = (char)(i * 7 + 3);
    g_u64 lo, hi, a, b, c;
    for (int off = 0; off < 40; off++) {
        char *p = buf + off;
        /* GPR loads of every width, base+disp and base+index forms */
        __asm__ volatile("movq (%1), %0" : "=r"(a) : "r"(p) : "memory");
        __asm__ volatile("movl 8(%1), %k0" : "=r"(b) : "r"(p) : "memory");
        __asm__ volatile("movzwl 12(%1,%2,1), %k0" : "=r"(c) : "r"(buf), "r"((g_u64)off) : "memory");
        put("gpr8 ", a); put("gpr4 ", b); put("gpr2 ", c);
        /* GPR stores then reload */
        __asm__ volatile("movq %1, 64(%0)\n\tmovl %k2, 72(%0)\n\tmovw %w3, 76(%0)" : : "r"(p), "r"(a ^ 0x5555555555555555ULL), "r"(b + 1), "r"(c + 2) : "memory");
        __asm__ volatile("movq 64(%1), %0" : "=r"(a) : "r"(p) : "memory"); put("st8 ", a);
        __asm__ volatile("movq 72(%1), %0" : "=r"(a) : "r"(p) : "memory"); put("st4+2 ", a);
        /* movq xmm <- m64 feeding paddq with a pinned xmm, then m64 <- xmm */
        __asm__ volatile("movq (%2), %%xmm1\n\tmovq %2, %%xmm2\n\tpaddq %%xmm2, %%xmm1\n\tmovq %%xmm1, %0\n\tpshufd $0xee, %%xmm1, %%xmm3\n\tmovq %%xmm3, %1\n\tmovq %%xmm1, 96(%2)"
                         : "=r"(lo), "=r"(hi) : "r"(p) : "xmm1", "xmm2", "xmm3", "memory");
        put("movq lo ", lo); put("movq hi ", hi);
        __asm__ volatile("movq 96(%1), %0" : "=r"(a) : "r"(p) : "memory"); put("movq st ", a);
        /* movdqu / movups 16-byte load and store */
        __asm__ volatile("movdqu 1(%2), %%xmm4\n\tpxor %%xmm5, %%xmm5\n\tpsubb %%xmm4, %%xmm5\n\tmovq %%xmm5, %0\n\tpshufd $0xee, %%xmm5, %%xmm5\n\tmovq %%xmm5, %1\n\tmovups %%xmm4, 112(%2)"
                         : "=r"(lo), "=r"(hi) : "r"(p) : "xmm4", "xmm5", "memory");
        put("dqu lo ", lo); put("dqu hi ", hi);
        __asm__ volatile("movq 112(%1), %0\n\tmovq 120(%1), %1" : "=r"(a), "+r"(p) : : "memory"); put("ups st0 ", a); put("ups st1 ", (g_u64)p); p = buf + off;
        /* movsd / movss scalar loads into a pinned register (upper zeroed) and stores */
        __asm__ volatile("pcmpeqb %%xmm6, %%xmm6\n\tmovsd 3(%2), %%xmm6\n\tmovq %%xmm6, %0\n\tpshufd $0xee, %%xmm6, %%xmm7\n\tmovq %%xmm7, %1\n\tmovsd %%xmm6, 128(%2)"
                         : "=r"(lo), "=r"(hi) : "r"(p) : "xmm6", "xmm7", "memory");
        put("sd lo ", lo); put("sd hi ", hi);
        __asm__ volatile("pcmpeqb %%xmm6, %%xmm6\n\tmovss 5(%2), %%xmm6\n\tmovq %%xmm6, %0\n\tpshufd $0xee, %%xmm6, %%xmm7\n\tmovq %%xmm7, %1\n\tmovss %%xmm6, 136(%2)"
                         : "=r"(lo), "=r"(hi) : "r"(p) : "xmm6", "xmm7", "memory");
        put("ss lo ", lo); put("ss hi ", hi);
        __asm__ volatile("movq 128(%1), %0" : "=r"(a) : "r"(p) : "memory"); put("sd st ", a);
        __asm__ volatile("movq 136(%1), %0" : "=r"(a) : "r"(p) : "memory"); put("ss st ", a);
        /* movlps/movhps and pinsrw/pextrw shapes across a boundary */
        __asm__ volatile("movq %2, %%xmm1\n\tmovhps 7(%2), %%xmm1\n\tmovlps 9(%2), %%xmm1\n\tmovq %%xmm1, %0\n\tpshufd $0xee, %%xmm1, %%xmm2\n\tmovq %%xmm2, %1"
                         : "=r"(lo), "=r"(hi) : "r"(p) : "xmm1", "xmm2", "memory");
        put("lh lo ", lo); put("lh hi ", hi);
        /* SSE arithmetic straight from memory */
        __asm__ volatile("movq %2, %%xmm1\n\tpunpcklqdq %%xmm1, %%xmm1\n\tpaddd 15(%2), %%xmm1\n\tpxor 31(%2), %%xmm1\n\tmovq %%xmm1, %0\n\tpshufd $0xee, %%xmm1, %%xmm2\n\tmovq %%xmm2, %1"
                         : "=r"(lo), "=r"(hi) : "r"(p) : "xmm1", "xmm2", "memory");
        put("arith lo ", lo); put("arith hi ", hi);
        /* rmw / cmp / test with memory operands crossing */
        __asm__ volatile("addq $3, 144(%0)\n\torl $0x10001, 148(%0)\n\txorw $0x1234, 158(%0)" : : "r"(p) : "memory", "cc");
        __asm__ volatile("movq 144(%1), %0" : "=r"(a) : "r"(p) : "memory"); put("rmw a ", a);
        __asm__ volatile("movq 152(%1), %0" : "=r"(a) : "r"(p) : "memory"); put("rmw b ", a);
        __asm__ volatile("xorl %k0, %k0\n\tcmpq %2, 160(%1)\n\tsetb %b0" : "=&r"(a) : "r"(p), "r"(lo) : "memory", "cc"); put("cmpm ", a);
    }
    return 0;
}

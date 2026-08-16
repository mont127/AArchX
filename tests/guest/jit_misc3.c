/* push/pop with memory operands, setcc to memory, 8/16-bit inc/dec (CF kept),
 * bsf/bsr feeding je/jne, pshufb, integer punpck. */
#include "gsys.h"
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
int main(void){
    static const g_u64 vals[5] = { 0x0123456789abcdefULL, 0xffffffffffffffffULL, 0x8000000000000001ULL, 0x00000000deadbeefULL, 0 };
    volatile g_u64 mem[4]; volatile unsigned char m8[4];
    g_u64 r, f, lo, hi;
    for (int i = 0; i < 5; i++) {
        g_u64 v = vals[i], w = vals[(i + 3) % 5];
        mem[0] = v; mem[1] = w; mem[2] = 0; mem[3] = 0;
        __asm__ volatile("pushq %2\n\tpushq %3\n\tpopq %1\n\tpopq %0" : "=m"(mem[2]), "=m"(mem[3]) : "m"(mem[0]), "m"(mem[1]) : "memory"); put("pushpop m2 ", mem[2]); put("pushpop m3 ", mem[3]);
        m8[0] = m8[1] = m8[2] = 0x77;
        __asm__ volatile("cmpq %3, %2\n\tsetb %0\n\tsete %1" : "=m"(m8[0]), "=m"(m8[1]) : "r"(v), "r"(w) : "cc"); put("setb m ", m8[0]); put("sete m ", m8[1]);
        r = v; __asm__ volatile("stc\n\tincb %b0\n\tpushfq\n\tpopq %1" : "+q"(r), "=r"(f) :: "cc"); put("inc8 r ", r); put("inc8 f ", f & 0x8d5);
        r = v; __asm__ volatile("clc\n\tdecw %w0\n\tpushfq\n\tpopq %1" : "+r"(r), "=r"(f) :: "cc"); put("dec16 r ", r); put("dec16 f ", f & 0x8d5);
        r = v; __asm__ volatile("stc\n\tdecb %b0\n\tpushfq\n\tpopq %1" : "+q"(r), "=r"(f) :: "cc"); put("dec8 r ", r); put("dec8 f ", f & 0x8d5);
        r = 0x1122334455667788ULL; __asm__ volatile("bsfq %2, %0\n\tjz 1f\n\tmovq $0x99, %1\n\tjmp 2f\n\t1: movq $0x11, %1\n\t2:" : "+r"(r), "=r"(f) : "r"(v) : "cc"); put("bsf jz r ", r); put("bsf jz path ", f);
        r = 0x1122334455667788ULL; __asm__ volatile("bsrl %k2, %k0\n\tjnz 1f\n\tmovq $0x99, %1\n\tjmp 2f\n\t1: movq $0x11, %1\n\t2:" : "+r"(r), "=r"(f) : "r"(v) : "cc"); put("bsr jnz r ", r); put("bsr jnz path ", f);
        /* pshufb: reverse bytes; select with high bit clears */
        static const g_u64 masks[3][2] = { {0x0001020304050607ULL, 0x08090a0b0c0d0e0fULL}, {0x0f0e0d0c0b0a0908ULL, 0x0706050403020100ULL}, {0x8080808000000000ULL, 0x0f8f0f8f7f7f7f7fULL} };
        for (int k = 0; k < 3; k++) {
            __asm__ volatile("movq %2, %%xmm1\n\tpunpcklqdq %%xmm1, %%xmm1\n\tpaddq %%xmm1, %%xmm1\n\tmovdqu %3, %%xmm2\n\tpshufb %%xmm2, %%xmm1\n\tmovq %%xmm1, %0\n\tpextrq $1, %%xmm1, %1"
                             : "=r"(lo), "=r"(hi) : "r"(v), "m"(masks[k]) : "xmm1", "xmm2"); put("pshufb lo ", lo); put("pshufb hi ", hi);
        }
#define UNP(insn, tag) __asm__ volatile("movq %2, %%xmm1\n\tpunpcklqdq %%xmm1, %%xmm1\n\tmovq %3, %%xmm2\n\tpunpcklqdq %%xmm2, %%xmm2\n\tpaddq %%xmm2, %%xmm2\n\t" insn " %%xmm2, %%xmm1\n\tmovq %%xmm1, %0\n\tpextrq $1, %%xmm1, %1" : "=r"(lo), "=r"(hi) : "r"(v), "r"(w) : "xmm1", "xmm2"); put(tag " lo ", lo); put(tag " hi ", hi);
        UNP("punpcklbw", "lbw") UNP("punpcklwd", "lwd") UNP("punpckldq", "ldq") UNP("punpcklqdq", "lqdq") UNP("punpckhbw", "hbw") UNP("punpckhwd", "hwd") UNP("punpckhdq", "hdq") UNP("punpckhqdq", "hqdq")
    }
    return 0;
}

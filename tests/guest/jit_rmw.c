/* Read-modify-write memory forms: add/sub/and/or/xor/inc/dec/neg/not [mem]
 * (plain and lock), xchg/xadd/cmpxchg with memory, cmp/test [mem],x; all
 * sizes; results and flags via setcc.  Golden from Rosetta. */
#include "gsys.h"
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
#define FL(out) __asm__ volatile("pushfq\n\tpopq %0" : "=r"(out))
int main(void){
    volatile g_u64 mem64; volatile g_u32 mem32; volatile unsigned short mem16; volatile unsigned char mem8;
    g_u64 f, r;
    static const g_u64 vals[5] = { 0, 1, 0x7fffffffffffffffULL, 0xffffffffffffffffULL, 0x80000000ULL };
    static const g_u64 srcs[4] = { 1, 0xffffffffffffffffULL, 0x80000000ULL, 0x1234 };
    for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) {
        g_u64 v = vals[i], s = srcs[j];
#define T64(insn, tag) mem64 = v; __asm__ volatile(insn " %2, %0\n\tpushfq\n\tpopq %1" : "+m"(mem64), "=r"(f) : "r"(s) : "cc"); put(tag " m ", mem64); put(tag " f ", f & 0x8d5);
#define T32(insn, tag) mem32 = (g_u32)v; __asm__ volatile(insn " %2, %0\n\tpushfq\n\tpopq %1" : "+m"(mem32), "=r"(f) : "r"((g_u32)s) : "cc"); put(tag " m ", mem32); put(tag " f ", f & 0x8d5);
#define T16(insn, tag) mem16 = (unsigned short)v; __asm__ volatile(insn " %2, %0\n\tpushfq\n\tpopq %1" : "+m"(mem16), "=r"(f) : "r"((unsigned short)s) : "cc"); put(tag " m ", mem16); put(tag " f ", f & 0x8d5);
#define T8(insn, tag) mem8 = (unsigned char)v; __asm__ volatile(insn " %2, %0\n\tpushfq\n\tpopq %1" : "+m"(mem8), "=q"(f) : "q"((unsigned char)s) : "cc"); put(tag " m ", mem8); put(tag " f ", f & 0x8d5);
        T64("addq", "add64") T64("subq", "sub64") T64("andq", "and64") T64("orq", "or64") T64("xorq", "xor64")
        T64("lock addq", "ladd64") T64("lock subq", "lsub64") T64("lock orq", "lor64") T64("lock andq", "land64") T64("lock xorq", "lxor64")
        T32("addl", "add32") T32("subl", "sub32") T32("lock xorl", "lxor32") T32("cmpl", "cmp32") T64("cmpq", "cmp64") T64("testq", "test64")
        T16("addw", "add16") T16("lock orw", "lor16") T16("cmpw", "cmp16") T8("addb", "add8") T8("lock subb", "lsub8") T8("cmpb", "cmp8") T8("testb", "test8")
        /* immediates */
        mem64 = v; __asm__ volatile("addq $0x1234, %0\n\tpushfq\n\tpopq %1" : "+m"(mem64), "=r"(f) :: "cc"); put("addi64 m ", mem64); put("addi64 f ", f & 0x8d5);
        mem32 = (g_u32)v; __asm__ volatile("lock orl $0x80000000, %0\n\tpushfq\n\tpopq %1" : "+m"(mem32), "=r"(f) :: "cc"); put("lori32 m ", mem32); put("lori32 f ", f & 0x8d5);
        mem8 = (unsigned char)v; __asm__ volatile("cmpb $0x80, %0\n\tpushfq\n\tpopq %1" : "+m"(mem8), "=r"(f) :: "cc"); put("cmpi8 f ", f & 0x8d5);
        /* inc/dec/neg/not with CF preserved through inc/dec */
        mem64 = v; __asm__ volatile("stc\n\tincq %0\n\tpushfq\n\tpopq %1" : "+m"(mem64), "=r"(f) :: "cc"); put("inc64 m ", mem64); put("inc64 f ", f & 0x8d5);
        mem64 = v; __asm__ volatile("clc\n\tlock decq %0\n\tpushfq\n\tpopq %1" : "+m"(mem64), "=r"(f) :: "cc"); put("ldec64 m ", mem64); put("ldec64 f ", f & 0x8d5);
        mem32 = (g_u32)v; __asm__ volatile("stc\n\tlock incl %0\n\tpushfq\n\tpopq %1" : "+m"(mem32), "=r"(f) :: "cc"); put("linc32 m ", mem32); put("linc32 f ", f & 0x8d5);
        mem8 = (unsigned char)v; __asm__ volatile("decb %0\n\tpushfq\n\tpopq %1" : "+m"(mem8), "=r"(f) :: "cc"); put("dec8 m ", mem8); put("dec8 f ", f & 0x8d5);
        mem64 = v; __asm__ volatile("negq %0\n\tpushfq\n\tpopq %1" : "+m"(mem64), "=r"(f) :: "cc"); put("neg64 m ", mem64); put("neg64 f ", f & 0x8d5);
        mem32 = (g_u32)v; __asm__ volatile("negl %0\n\tpushfq\n\tpopq %1" : "+m"(mem32), "=r"(f) :: "cc"); put("neg32 m ", mem32); put("neg32 f ", f & 0x8d5);
        mem64 = v; __asm__ volatile("notq %0" : "+m"(mem64) :: "cc"); put("not64 m ", mem64);
        mem16 = (unsigned short)v; __asm__ volatile("lock notw %0" : "+m"(mem16) :: "cc"); put("lnot16 m ", mem16);
        /* xchg / xadd / cmpxchg */
        mem64 = v; r = s; __asm__ volatile("xchgq %1, %0" : "+m"(mem64), "+r"(r)); put("xchg64 m ", mem64); put("xchg64 r ", r);
        mem32 = (g_u32)v; r = s; __asm__ volatile("xchgl %k1, %0" : "+m"(mem32), "+r"(r)); put("xchg32 m ", mem32); put("xchg32 r ", r);
        mem8 = (unsigned char)v; r = s | 0xaa00; __asm__ volatile("xchgb %b1, %0" : "+m"(mem8), "+q"(r)); put("xchg8 m ", mem8); put("xchg8 r ", r);
        mem64 = v; r = s; __asm__ volatile("lock xaddq %1, %0\n\tpushfq\n\tpopq %2" : "+m"(mem64), "+r"(r), "=r"(f) :: "cc"); put("xadd64 m ", mem64); put("xadd64 r ", r); put("xadd64 f ", f & 0x8d5);
        mem32 = (g_u32)v; r = s; __asm__ volatile("xaddl %k1, %0\n\tpushfq\n\tpopq %2" : "+m"(mem32), "+r"(r), "=r"(f) :: "cc"); put("xadd32 m ", mem32); put("xadd32 r ", r); put("xadd32 f ", f & 0x8d5);
        mem16 = (unsigned short)v; r = s | 0x5555000000000000ULL; __asm__ volatile("lock xaddw %w1, %0\n\tpushfq\n\tpopq %2" : "+m"(mem16), "+r"(r), "=r"(f) :: "cc"); put("xadd16 m ", mem16); put("xadd16 r ", r); put("xadd16 f ", f & 0x8d5);
        for (int k = 0; k < 2; k++) {
            g_u64 acc = k ? v : s ^ 0x55;   /* match / mismatch */
            mem64 = v; r = acc; __asm__ volatile("lock cmpxchgq %3, %0\n\tpushfq\n\tpopq %2" : "+m"(mem64), "+a"(r), "=r"(f) : "r"(s) : "cc"); put("cmpxchg64 m ", mem64); put("cmpxchg64 a ", r); put("cmpxchg64 f ", f & 0x8d5);
            mem32 = (g_u32)v; r = acc | 0x1111000000000000ULL; __asm__ volatile("cmpxchgl %k3, %0\n\tpushfq\n\tpopq %2" : "+m"(mem32), "+a"(r), "=r"(f) : "r"((g_u32)s) : "cc"); put("cmpxchg32 m ", mem32); put("cmpxchg32 a ", r); put("cmpxchg32 f ", f & 0x8d5);
            mem16 = (unsigned short)v; r = (acc & 0xffff) | 0x2222000000000000ULL; __asm__ volatile("lock cmpxchgw %w3, %0\n\tpushfq\n\tpopq %2" : "+m"(mem16), "+a"(r), "=r"(f) : "r"((unsigned short)s) : "cc"); put("cmpxchg16 m ", mem16); put("cmpxchg16 a ", r); put("cmpxchg16 f ", f & 0x8d5);
            mem8 = (unsigned char)v; r = (acc & 0xff) | 0x3333000000000000ULL; __asm__ volatile("cmpxchgb %b3, %0\n\tpushfq\n\tpopq %2" : "+m"(mem8), "+a"(r), "=r"(f) : "q"((unsigned char)s) : "cc"); put("cmpxchg8 m ", mem8); put("cmpxchg8 a ", r); put("cmpxchg8 f ", f & 0x8d5);
        }
    }
    return 0;
}

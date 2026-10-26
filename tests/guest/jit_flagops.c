/* clc, stc and cmc ahead of adc, sbb and setc (CF deferred) and ahead of readers of the other flags (pushfq,
 * sete, sets: the flags materialized), lahf and sahf after compares and with crafted AH, and movbe loads and
 * stores of 16, 32 and 64 bits (16 keeping the register's upper bits).  The JIT has translated them since
 * 2026-10-26; they were interpreted before. */
#include "gsys.h"
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
static volatile g_u64 mem64 = 0x0102030405060708ULL;
static volatile unsigned mem32 = 0xa1b2c3d4u;
static volatile unsigned short mem16 = 0xbeefu;
int main(void)
{
    static const g_u64 vals[] = { 0, 1, 0x7fffffffffffffffULL, 0x8000000000000000ULL, ~0ULL, 42, 0x123456789ULL };
    for (unsigned i = 0; i < 7; i++) {
        for (unsigned j = 0; j < 7; j++) {
            g_u64 a = vals[i], b = vals[j], r, f;
            r = a; __asm__ volatile("stc\n\tadcq %1, %0" : "+r"(r) : "r"(b) : "cc"); put("stc adc ", r);
            r = a; __asm__ volatile("clc\n\tsbbq %1, %0" : "+r"(r) : "r"(b) : "cc"); put(" clc sbb ", r);
            __asm__ volatile("cmpq %2, %1\n\tcmc\n\tsetc %b0" : "=r"(r) : "r"(a), "r"(b) : "cc"); put(" cmp cmc setc ", r & 0xff);
            __asm__ volatile("cmpq %2, %1\n\tcmc\n\tcmc\n\tadcq $0, %0" : "=r"(r) : "r"(a), "r"(b), "0"((g_u64)100) : "cc"); put(" cmc cmc adc ", r);
            __asm__ volatile("cmpq %2, %1\n\tstc\n\tpushfq\n\tpopq %0" : "=r"(f) : "r"(a), "r"(b) : "cc"); put(" cmp stc flags ", f & 0x8d5);
            __asm__ volatile("cmpq %2, %1\n\tclc\n\tsete %b0" : "=r"(r) : "r"(a), "r"(b) : "cc"); put(" cmp clc sete ", r & 0xff);
            __asm__ volatile("cmpq %2, %1\n\tcmc\n\tpushfq\n\tpopq %0" : "=r"(f) : "r"(a), "r"(b) : "cc"); put(" cmp cmc flags ", f & 0x8d5);
            __asm__ volatile("cmpq %2, %1\n\tlahf\n\tmovzbl %%ah, %k0" : "=r"(r) : "r"(a), "r"(b) : "cc", "rax"); put(" cmp lahf ", r);
            __asm__ volatile("addq %2, %1\n\tlahf\n\tmovzbl %%ah, %k0" : "=r"(r), "+r"(a) : "r"(b) : "cc", "rax"); put(" add lahf ", r);
        }
    }
    for (unsigned ah = 0; ah < 256; ah += 7) {
        g_u64 f, r;
        __asm__ volatile("movq %2, %%rax\n\tshlq $8, %%rax\n\tcmpq $0, %%rax\n\tsahf\n\tpushfq\n\tpopq %0\n\tsetc %b1"
                         : "=r"(f), "=r"(r) : "r"((g_u64)ah) : "cc", "rax");
        put("sahf ", ah); put(" flags ", f & 0x8d5); put(" c ", r & 0xff);
        __asm__ volatile("movq %1, %%rax\n\tshlq $8, %%rax\n\tsahf\n\tmovq $5, %0\n\tadcq $0, %0"
                         : "=r"(r) : "r"((g_u64)ah) : "cc", "rax");
        put(" adc ", r);
    }
    g_u64 r64, x = 0xdeadbeefcafef00dULL;
    unsigned r32; g_u64 keep = 0x1111111111111111ULL;
    __asm__ volatile("movbeq %1, %0" : "=r"(r64) : "m"(mem64)); put("movbe ld64 ", r64);
    __asm__ volatile("movbel %1, %0" : "=r"(r32) : "m"(mem32)); put(" ld32 ", r32);
    __asm__ volatile("movbew %1, %w0" : "+r"(keep) : "m"(mem16)); put(" ld16 keep ", keep);
    __asm__ volatile("movbeq %1, %0" : "=m"(mem64) : "r"(x)); put(" st64 ", mem64);
    __asm__ volatile("movbel %1, %0" : "=m"(mem32) : "r"((unsigned)x)); put(" st32 ", mem32);
    __asm__ volatile("movbew %w1, %0" : "=m"(mem16) : "r"(x)); put(" st16 ", mem16);
    g_u64 acc = 0;
    for (g_u64 i = 0; i < 1000; i++) {
        g_u64 v = i * 0x9e3779b97f4a7c15ULL, w;
        __asm__ volatile("movbeq %1, %0" : "=r"(w) : "m"(v));
        acc = acc * 31 + w;
        __asm__ volatile("stc\n\tadcq $0, %0" : "+r"(acc) :: "cc");
    }
    put("loop ", acc);
    return 0;
}

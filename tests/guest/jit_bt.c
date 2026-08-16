/* bt reg/mem, imm/reg: CF into jc/jnc, setc, adc and RFLAGS (pushfq); bit
 * offsets beyond the operand size and negative offsets for the memory form. */
#include "gsys.h"
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
int main(void){
    static g_u64 bits[8] = { 0x8000000000000001ULL, 0x00000000ffffffffULL, 0x5555555555555555ULL, 0, 0xaaaaaaaaaaaaaaaaULL, 1ULL << 63, 0x0123456789abcdefULL, ~0ULL };
    volatile g_u64 *arr = bits;
    g_u64 f, r, c;
    for (int i = 0; i < 8; i++) {
        g_u64 v = bits[i];
        for (int n = 0; n < 4; n++) {
            unsigned idx = (unsigned)(n * 21 + i);           /* 0..84: wraps for 32/64 */
            r = 0; __asm__ volatile("btq %2, %1\n\tjc 1f\n\tmovq $1, %0\n\t1:" : "+r"(r) : "r"(v), "r"((g_u64)idx) : "cc"); put("bt64 rr jc ", r);
            r = 0; __asm__ volatile("btl %k2, %k1\n\tjnc 1f\n\tmovq $1, %0\n\t1:" : "+r"(r) : "r"(v), "r"((g_u64)idx) : "cc"); put("bt32 rr jnc ", r);
            __asm__ volatile("btq %2, %1\n\tsetc %b0" : "=r"(c) : "r"(v), "r"((g_u64)idx) : "cc"); put("bt64 rr setc ", c & 0xff);
            r = 10; __asm__ volatile("btq %2, %1\n\tadcq $0, %0" : "+r"(r) : "r"(v), "r"((g_u64)idx) : "cc"); put("bt64 rr adc ", r);
            __asm__ volatile("xorl %%eax, %%eax\n\tbtq %2, %1\n\tpushfq\n\tpopq %0" : "=r"(f) : "r"(v), "r"((g_u64)idx) : "cc", "rax"); put("bt64 rr flags ", f & 0x8d5);
            __asm__ volatile("btw %w2, %w1\n\tsetc %b0" : "=r"(c) : "r"(v), "r"((g_u64)idx) : "cc"); put("bt16 rr setc ", c & 0xff);
            /* memory forms: offset relative to arr[0], may reach other elements */
            long off = (long)(i * 64 + n * 13) - 70;
            r = 0; __asm__ volatile("btq %2, %1\n\tjc 1f\n\tmovq $1, %0\n\t1:" : "+r"(r) : "m"(*arr), "r"((g_u64)off) : "cc"); put("bt64 mr jc ", r);
            __asm__ volatile("btl %k2, %1\n\tsetc %b0" : "=r"(c) : "m"(*(volatile g_u32 *)arr), "r"((g_u64)off) : "cc"); put("bt32 mr setc ", c & 0xff);
            __asm__ volatile("xorl %%eax, %%eax\n\tbtq %2, %1\n\tpushfq\n\tpopq %0" : "=r"(f) : "m"(*arr), "r"((g_u64)off) : "cc", "rax"); put("bt64 mr flags ", f & 0x8d5);
        }
        /* immediates */
        __asm__ volatile("btq $63, %1\n\tsetc %b0" : "=r"(c) : "r"(v) : "cc"); put("bt64 ri63 ", c & 0xff);
        __asm__ volatile("btq $0, %1\n\tsetc %b0" : "=r"(c) : "r"(v) : "cc"); put("bt64 ri0 ", c & 0xff);
        __asm__ volatile("btl $31, %k1\n\tsetc %b0" : "=r"(c) : "r"(v) : "cc"); put("bt32 ri31 ", c & 0xff);
        __asm__ volatile("btl $32, %k1\n\tsetc %b0" : "=r"(c) : "r"(v) : "cc"); put("bt32 ri32wrap ", c & 0xff);
        r = 0; __asm__ volatile("btq $17, %1\n\tjc 1f\n\tmovq $1, %0\n\t1:" : "+r"(r) : "m"(arr[i]) : "cc"); put("bt64 mi17 jc ", r);
        __asm__ volatile("btq $63, %1\n\tsetc %b0" : "=r"(c) : "m"(arr[i]) : "cc"); put("bt64 mi63 setc ", c & 0xff);
        __asm__ volatile("xorl %%eax, %%eax\n\tbtl $19, %1\n\tpushfq\n\tpopq %0" : "=r"(f) : "m"(*(volatile g_u32 *)&arr[i]) : "cc", "rax"); put("bt32 mi19 flags ", f & 0x8d5);
        /* CF surviving a gap before the consumer */
        r = 0; __asm__ volatile("btq $2, %1\n\tmovq $7, %%rcx\n\tjc 1f\n\tmovq $1, %0\n\t1:" : "+r"(r) : "r"(v) : "cc", "rcx"); put("bt gap jc ", r);
    }
    return 0;
}

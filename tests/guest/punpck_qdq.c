/* punpckhqdq / punpcklqdq in every form (reg<-reg, reg<-same reg, reg<-mem),
 * with the low and the REX-extended registers.  0F 6D used to decode as
 * punpcklqdq, so the high qword never moved down, and every Cocoa window was
 * constrained to a 74-pixel-high visible frame.  Golden from Rosetta.
 */
#include "gsys.h"
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
int main(void){
    static const g_u64 A[2] = { 0x1111111122222222ULL, 0x3333333344444444ULL };
    static const g_u64 B[2] = { 0x5555555566666666ULL, 0x7777777788888888ULL };
    g_u64 lo, hi;
    __asm__ volatile("movdqu %2, %%xmm1\n\tmovdqu %3, %%xmm2\n\tpunpckhqdq %%xmm2, %%xmm1\n\tmovq %%xmm1, %0\n\tpshufd $0xee, %%xmm1, %%xmm1\n\tmovq %%xmm1, %1"
                     : "=r"(lo), "=r"(hi) : "m"(A[0]), "m"(B[0]) : "xmm1", "xmm2"); put("punpckhqdq rr lo ", lo); put("punpckhqdq rr hi ", hi);
    __asm__ volatile("movdqu %2, %%xmm1\n\tpunpckhqdq %%xmm1, %%xmm1\n\tmovq %%xmm1, %0\n\tpshufd $0xee, %%xmm1, %%xmm1\n\tmovq %%xmm1, %1"
                     : "=r"(lo), "=r"(hi) : "m"(A[0]) : "xmm1"); put("punpckhqdq same lo ", lo); put("punpckhqdq same hi ", hi);
    __asm__ volatile("movdqu %2, %%xmm1\n\tpunpckhqdq %3, %%xmm1\n\tmovq %%xmm1, %0\n\tpshufd $0xee, %%xmm1, %%xmm1\n\tmovq %%xmm1, %1"
                     : "=r"(lo), "=r"(hi) : "m"(A[0]), "m"(B[0]) : "xmm1"); put("punpckhqdq rm lo ", lo); put("punpckhqdq rm hi ", hi);
    __asm__ volatile("movdqu %2, %%xmm1\n\tmovdqu %3, %%xmm2\n\tpunpcklqdq %%xmm2, %%xmm1\n\tmovq %%xmm1, %0\n\tpshufd $0xee, %%xmm1, %%xmm1\n\tmovq %%xmm1, %1"
                     : "=r"(lo), "=r"(hi) : "m"(A[0]), "m"(B[0]) : "xmm1", "xmm2"); put("punpcklqdq rr lo ", lo); put("punpcklqdq rr hi ", hi);
    __asm__ volatile("movdqu %2, %%xmm1\n\tpunpcklqdq %%xmm1, %%xmm1\n\tmovq %%xmm1, %0\n\tpshufd $0xee, %%xmm1, %%xmm1\n\tmovq %%xmm1, %1"
                     : "=r"(lo), "=r"(hi) : "m"(A[0]) : "xmm1"); put("punpcklqdq same lo ", lo); put("punpcklqdq same hi ", hi);
    __asm__ volatile("movdqu %2, %%xmm1\n\tpunpcklqdq %3, %%xmm1\n\tmovq %%xmm1, %0\n\tpshufd $0xee, %%xmm1, %%xmm1\n\tmovq %%xmm1, %1"
                     : "=r"(lo), "=r"(hi) : "m"(A[0]), "m"(B[0]) : "xmm1"); put("punpcklqdq rm lo ", lo); put("punpcklqdq rm hi ", hi);
    __asm__ volatile("movdqu %2, %%xmm9\n\tmovdqu %3, %%xmm12\n\tpunpckhqdq %%xmm12, %%xmm9\n\tmovq %%xmm9, %0\n\tpshufd $0xee, %%xmm9, %%xmm9\n\tmovq %%xmm9, %1"
                     : "=r"(lo), "=r"(hi) : "m"(B[0]), "m"(A[0]) : "xmm9", "xmm12"); put("punpckhqdq rex rr lo ", lo); put("punpckhqdq rex rr hi ", hi);
    __asm__ volatile("movdqu %2, %%xmm10\n\tpunpckhqdq %%xmm10, %%xmm10\n\tmovq %%xmm10, %0\n\tpshufd $0xee, %%xmm10, %%xmm10\n\tmovq %%xmm10, %1"
                     : "=r"(lo), "=r"(hi) : "m"(B[0]) : "xmm10"); put("punpckhqdq rex same lo ", lo); put("punpckhqdq rex same hi ", hi);
    __asm__ volatile("movdqu %2, %%xmm11\n\tpunpcklqdq %3, %%xmm11\n\tmovq %%xmm11, %0\n\tpshufd $0xee, %%xmm11, %%xmm11\n\tmovq %%xmm11, %1"
                     : "=r"(lo), "=r"(hi) : "m"(B[0]), "m"(A[0]) : "xmm11"); put("punpcklqdq rex rm lo ", lo); put("punpcklqdq rex rm hi ", hi);
    g_puts("done\n");
    return 0;
}

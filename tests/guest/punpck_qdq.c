/* punpckhqdq / punpcklqdq in every form (reg<-reg, reg<-same reg, reg<-mem),
 * plus the SkyLight shape that broke NSScreen.visibleFrame under ocerz:
 * four floats -> CGRect via cvtps2pd / punpckhqdq xmm0,xmm0 / cvtps2pd.
 * 0F 6D used to decode as punpcklqdq, so the high qword never moved down and
 * every Cocoa window was constrained to a 74-pixel-high "visible frame".
 * Golden from Rosetta. */
#include "gsys.h"
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
int main(void){
    static const g_u64 A[2] = { 0x1111111122222222ULL, 0x3333333344444444ULL };
    static const g_u64 B[2] = { 0x5555555566666666ULL, 0x7777777788888888ULL };
    g_u64 lo, hi;
    /* punpckhqdq xmm1, xmm2 : lo = A.hi, hi = B.hi */
    __asm__ volatile("movdqu %2, %%xmm1\n\tmovdqu %3, %%xmm2\n\tpunpckhqdq %%xmm2, %%xmm1\n\tmovq %%xmm1, %0\n\tpshufd $0xee, %%xmm1, %%xmm1\n\tmovq %%xmm1, %1"
                     : "=r"(lo), "=r"(hi) : "m"(A[0]), "m"(B[0]) : "xmm1", "xmm2"); put("punpckhqdq rr lo ", lo); put("punpckhqdq rr hi ", hi);
    /* punpckhqdq xmm1, xmm1 : lo = hi = A.hi */
    __asm__ volatile("movdqu %2, %%xmm1\n\tpunpckhqdq %%xmm1, %%xmm1\n\tmovq %%xmm1, %0\n\tpshufd $0xee, %%xmm1, %%xmm1\n\tmovq %%xmm1, %1"
                     : "=r"(lo), "=r"(hi) : "m"(A[0]) : "xmm1"); put("punpckhqdq same lo ", lo); put("punpckhqdq same hi ", hi);
    /* punpckhqdq xmm1, m128 */
    __asm__ volatile("movdqu %2, %%xmm1\n\tpunpckhqdq %3, %%xmm1\n\tmovq %%xmm1, %0\n\tpshufd $0xee, %%xmm1, %%xmm1\n\tmovq %%xmm1, %1"
                     : "=r"(lo), "=r"(hi) : "m"(A[0]), "m"(B[0]) : "xmm1"); put("punpckhqdq rm lo ", lo); put("punpckhqdq rm hi ", hi);
    /* punpcklqdq xmm1, xmm2 : lo = A.lo, hi = B.lo */
    __asm__ volatile("movdqu %2, %%xmm1\n\tmovdqu %3, %%xmm2\n\tpunpcklqdq %%xmm2, %%xmm1\n\tmovq %%xmm1, %0\n\tpshufd $0xee, %%xmm1, %%xmm1\n\tmovq %%xmm1, %1"
                     : "=r"(lo), "=r"(hi) : "m"(A[0]), "m"(B[0]) : "xmm1", "xmm2"); put("punpcklqdq rr lo ", lo); put("punpcklqdq rr hi ", hi);
    __asm__ volatile("movdqu %2, %%xmm1\n\tpunpcklqdq %%xmm1, %%xmm1\n\tmovq %%xmm1, %0\n\tpshufd $0xee, %%xmm1, %%xmm1\n\tmovq %%xmm1, %1"
                     : "=r"(lo), "=r"(hi) : "m"(A[0]) : "xmm1"); put("punpcklqdq same lo ", lo); put("punpcklqdq same hi ", hi);
    __asm__ volatile("movdqu %2, %%xmm1\n\tpunpcklqdq %3, %%xmm1\n\tmovq %%xmm1, %0\n\tpshufd $0xee, %%xmm1, %%xmm1\n\tmovq %%xmm1, %1"
                     : "=r"(lo), "=r"(hi) : "m"(A[0]), "m"(B[0]) : "xmm1"); put("punpcklqdq rm lo ", lo); put("punpcklqdq rm hi ", hi);
    /* the SkyLight dock-rect shape: floats {171, 2056, 3497, 104} -> doubles */
    static const float F[4] = { 171.0f, 2056.0f, 3497.0f, 104.0f };
    volatile double D[4] = { 0, 0, 0, 0 };
    __asm__ volatile("movdqu %2, %%xmm0\n\tcvtps2pd %%xmm0, %%xmm1\n\tmovups %%xmm1, %0\n\tpunpckhqdq %%xmm0, %%xmm0\n\tcvtps2pd %%xmm0, %%xmm0\n\tmovups %%xmm0, %1"
                     : "=m"(D[0]), "=m"(D[2]) : "m"(F[0]) : "xmm0", "xmm1");
    for (int i = 0; i < 4; i++) { g_u64 bits; __builtin_memcpy(&bits, (const void *)&D[i], 8); put("rect word ", bits); }
    g_puts("done\n");
    return 0;
}

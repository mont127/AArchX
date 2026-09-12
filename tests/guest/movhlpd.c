/* movhpd/movlpd/movhps/movlps load and store forms, unpckhpd/unpcklpd, shufpd, movddup,
 * movhlps/movlhps: the half-register moves AppKit's view-transform code leans on.
 * Golden from the architectural semantics (Rosetta was unavailable). */
#include "gsys.h"
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
static void show(const char *tag, const g_u64 *r) { put(tag, r[0]); put("   hi ", r[1]); }
int main(void){
    static const g_u64 A[2] = { 0x1111111122222222ULL, 0x3333333344444444ULL };
    static const g_u64 B[2] = { 0x5555555566666666ULL, 0x7777777788888888ULL };
    static const g_u64 M = 0x9999999aaaaaaaaaULL;
    g_u64 r[2] __attribute__((aligned(16)));
    __asm__ volatile("movdqu %1, %%xmm0\n\tmovhpd %2, %%xmm0\n\tmovdqu %%xmm0, %0" : "=m"(r[0]) : "m"(A[0]), "m"(M) : "xmm0"); show("movhpd load ", r);
    __asm__ volatile("movdqu %1, %%xmm0\n\tmovlpd %2, %%xmm0\n\tmovdqu %%xmm0, %0" : "=m"(r[0]) : "m"(A[0]), "m"(M) : "xmm0"); show("movlpd load ", r);
    __asm__ volatile("movdqu %1, %%xmm0\n\tmovhps %2, %%xmm0\n\tmovdqu %%xmm0, %0" : "=m"(r[0]) : "m"(A[0]), "m"(M) : "xmm0"); show("movhps load ", r);
    __asm__ volatile("movdqu %1, %%xmm0\n\tmovlps %2, %%xmm0\n\tmovdqu %%xmm0, %0" : "=m"(r[0]) : "m"(A[0]), "m"(M) : "xmm0"); show("movlps load ", r);
    r[0] = r[1] = 0;
    __asm__ volatile("movdqu %1, %%xmm0\n\tmovhpd %%xmm0, %0" : "=m"(r[0]) : "m"(A[0]) : "xmm0"); show("movhpd store ", r);
    r[0] = r[1] = 0;
    __asm__ volatile("movdqu %1, %%xmm0\n\tmovlpd %%xmm0, %0" : "=m"(r[0]) : "m"(A[0]) : "xmm0"); show("movlpd store ", r);
    r[0] = r[1] = 0;
    __asm__ volatile("movdqu %1, %%xmm0\n\tmovhps %%xmm0, %0" : "=m"(r[0]) : "m"(A[0]) : "xmm0"); show("movhps store ", r);
    __asm__ volatile("xorpd %%xmm3, %%xmm3\n\tmovhpd %1, %%xmm3\n\tmovdqu %%xmm3, %0" : "=m"(r[0]) : "m"(M) : "xmm3"); show("xorpd+movhpd ", r);
    __asm__ volatile("movdqu %1, %%xmm0\n\tmovdqu %2, %%xmm1\n\tunpckhpd %%xmm1, %%xmm0\n\tmovdqu %%xmm0, %0" : "=m"(r[0]) : "m"(A[0]), "m"(B[0]) : "xmm0", "xmm1"); show("unpckhpd ", r);
    __asm__ volatile("movdqu %1, %%xmm0\n\tmovdqu %2, %%xmm1\n\tunpcklpd %%xmm1, %%xmm0\n\tmovdqu %%xmm0, %0" : "=m"(r[0]) : "m"(A[0]), "m"(B[0]) : "xmm0", "xmm1"); show("unpcklpd ", r);
    __asm__ volatile("movdqu %1, %%xmm0\n\tmovapd %%xmm0, %%xmm1\n\tunpckhpd %%xmm0, %%xmm1\n\tmovdqu %%xmm1, %0" : "=m"(r[0]) : "m"(A[0]) : "xmm0", "xmm1"); show("unpckhpd same ", r);
    __asm__ volatile("movdqu %1, %%xmm0\n\tmovdqu %2, %%xmm1\n\tshufpd $1, %%xmm1, %%xmm0\n\tmovdqu %%xmm0, %0" : "=m"(r[0]) : "m"(A[0]), "m"(B[0]) : "xmm0", "xmm1"); show("shufpd 1 ", r);
    __asm__ volatile("movdqu %1, %%xmm0\n\tmovdqu %2, %%xmm1\n\tshufpd $2, %%xmm1, %%xmm0\n\tmovdqu %%xmm0, %0" : "=m"(r[0]) : "m"(A[0]), "m"(B[0]) : "xmm0", "xmm1"); show("shufpd 2 ", r);
    __asm__ volatile("movdqu %1, %%xmm0\n\tmovddup %%xmm0, %%xmm1\n\tmovdqu %%xmm1, %0" : "=m"(r[0]) : "m"(A[0]) : "xmm0", "xmm1"); show("movddup ", r);
    __asm__ volatile("movdqu %1, %%xmm0\n\tmovdqu %2, %%xmm1\n\tmovhlps %%xmm1, %%xmm0\n\tmovdqu %%xmm0, %0" : "=m"(r[0]) : "m"(A[0]), "m"(B[0]) : "xmm0", "xmm1"); show("movhlps ", r);
    __asm__ volatile("movdqu %1, %%xmm0\n\tmovdqu %2, %%xmm1\n\tmovlhps %%xmm1, %%xmm0\n\tmovdqu %%xmm0, %0" : "=m"(r[0]) : "m"(A[0]), "m"(B[0]) : "xmm0", "xmm1"); show("movlhps ", r);
    __asm__ volatile("movdqu %1, %%xmm0\n\tmovdqu %2, %%xmm1\n\tmovsd %%xmm1, %%xmm0\n\tmovdqu %%xmm0, %0" : "=m"(r[0]) : "m"(A[0]), "m"(B[0]) : "xmm0", "xmm1"); show("movsd rr ", r);
    __asm__ volatile("movdqu %1, %%xmm0\n\tmovsd %2, %%xmm0\n\tmovdqu %%xmm0, %0" : "=m"(r[0]) : "m"(A[0]), "m"(M) : "xmm0"); show("movsd rm ", r);
    g_puts("done\n");
    return 0;
}

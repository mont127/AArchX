/* AppKit's identity-transform idiom (NSViewGetTransformToDescendant helper):
 *   xorpd xmm2,xmm2 ; xorpd xmm3,xmm3 ; movhpd xmm3,[one] ; movsd xmm1,[one]
 *   subpd xmm1,xmm2 ; subpd xmm3,xmm2 ; stores
 * A zero-idiom tracker that forgets movhpd's partial write turns d=1 into 0 and
 * makes every view transform singular.  Golden from the architectural semantics. */
#include "gsys.h"
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
int main(void){
    static const double one = 1.0, two = 2.0;
    g_u64 r[6] __attribute__((aligned(16)));
    __asm__ volatile(
        "xorpd %%xmm2, %%xmm2\n\t"
        "xorpd %%xmm3, %%xmm3\n\t"
        "movhpd %1, %%xmm3\n\t"
        "movsd %1, %%xmm1\n\t"
        "subpd %%xmm2, %%xmm1\n\t"
        "movupd %%xmm1, 0(%0)\n\t"
        "subpd %%xmm2, %%xmm3\n\t"
        "movupd %%xmm3, 16(%0)\n\t"
        "movupd %%xmm2, 32(%0)\n\t"
        : : "r"(r), "m"(one) : "xmm1", "xmm2", "xmm3", "memory");
    for (int i = 0; i < 6; i++) put("identity ", r[i]);
    __asm__ volatile(
        "movsd %2, %%xmm2\n\t"
        "unpcklpd %%xmm2, %%xmm2\n\t"
        "xorpd %%xmm3, %%xmm3\n\t"
        "movlpd %1, %%xmm3\n\t"
        "xorpd %%xmm1, %%xmm1\n\t"
        "movhpd %1, %%xmm1\n\t"
        "subpd %%xmm2, %%xmm1\n\t"
        "movupd %%xmm1, 0(%0)\n\t"
        "addpd %%xmm2, %%xmm3\n\t"
        "movupd %%xmm3, 16(%0)\n\t"
        "xorps %%xmm4, %%xmm4\n\t"
        "movhps %1, %%xmm4\n\t"
        "addps %%xmm4, %%xmm4\n\t"
        "movups %%xmm4, 32(%0)\n\t"
        : : "r"(r), "m"(one), "m"(two) : "xmm1", "xmm2", "xmm3", "xmm4", "memory");
    for (int i = 0; i < 6; i++) put("mixed ", r[i]);
    g_puts("done\n");
    return 0;
}

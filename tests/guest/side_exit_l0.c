/* A scalar SSE result still parked in the JIT's lane-0 scratch when a
 * mid-block jcc is taken: the side-exit stub replays the lane flush, so once
 * the edge turns hot the conditional branch must NOT be retargeted straight at
 * the successor, which reads the register as a packed value.  lane_sel takes
 * its branch on a selector word (always or never), lane_alt on the loop
 * counter (three times in four).  Golden from Rosetta. */
#include "gsys.h"
__asm__(".text\n.globl _lane_sel\n_lane_sel:\n"
        "  movupd (%rdi), %xmm6\n"
        "  movapd %xmm6, %xmm3\n"
        "1:\n"
        "  mulsd (%rsi), %xmm3\n"
        "  addsd 8(%rsi), %xmm3\n"
        "  movq (%rdx), %rax\n"
        "  cmpq $0, %rax\n"
        "  jne 2f\n"
        "  subsd %xmm3, %xmm6\n"
        "2:\n"
        "  addpd %xmm3, %xmm6\n"
        "  decq %rcx\n"
        "  jnz 1b\n"
        "  movupd %xmm6, (%rdi)\n"
        "  ret\n"
        ".globl _lane_alt\n_lane_alt:\n"
        "  movupd (%rdi), %xmm6\n"
        "  movapd %xmm6, %xmm4\n"
        "1:\n"
        "  sqrtsd (%rsi), %xmm5\n"
        "  movsd 8(%rsi), %xmm4\n"
        "  divsd %xmm5, %xmm4\n"
        "  testq $3, %rdx\n"
        "  jne 2f\n"
        "  addsd %xmm4, %xmm6\n"
        "2:\n"
        "  movlhps %xmm4, %xmm4\n"
        "  addpd %xmm4, %xmm6\n"
        "  decq %rdx\n"
        "  jnz 1b\n"
        "  movupd %xmm6, (%rdi)\n"
        "  ret\n");
void lane_sel(double *acc, const double *src, const volatile g_u64 *sel, unsigned long n);
void lane_alt(double *acc, const double *src, unsigned long n);
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
static void show(const char *tag, const double *acc)
{
    g_u64 b0, b1; __builtin_memcpy(&b0, &acc[0], 8); __builtin_memcpy(&b1, &acc[1], 8);
    put(tag, b0); put("   hi ", b1);
}
int main(void){
    static const double src[2] = { 0.5, 1.0 };
    static const double root[2] = { 2.25, 3.0 };
    static volatile g_u64 sel_on = 1, sel_off = 0;
    double acc[2] __attribute__((aligned(16)));
    for (int rep = 0; rep < 3; rep++) {
        volatile double *va = acc;
        va[0] = 0.25; va[1] = -8.0; lane_sel(acc, src, &sel_on, 5000); show("sel taken ", acc);
        va[0] = 0.25; va[1] = -8.0; lane_sel(acc, src, &sel_off, 5000); show("sel fall  ", acc);
        va[0] = 1.0; va[1] = 2.0; lane_alt(acc, root, 4000); show("alt       ", acc);
    }
    g_puts("done\n");
    return 0;
}

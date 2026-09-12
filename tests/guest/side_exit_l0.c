/* A scalar SSE result (subsd) still parked in the JIT's lane-0 scratch when a
 * mid-block jcc is taken: the side-exit stub replays the lane flush, so once
 * the edge turns hot the conditional branch must NOT be retargeted straight at
 * the successor (which reads the register as a packed value).  Shape lifted
 * from AppKit's view-transform helper.  Golden from the interpreter. */
#include "gsys.h"
__asm__(".text\n.globl _xk\n_xk:\n"
        "  pushq %rbp\n  movq %rsp, %rbp\n  pushq %rbx\n"
        "  xorpd %xmm7, %xmm7\n"
        "1:\n"
        "  movsd (%rsi), %xmm1\n"
        "  movsd 8(%rsi), %xmm2\n"
        "  subsd %xmm1, %xmm2\n"
        "  movapd %xmm2, %xmm1\n"
        "  movq 16(%rsi), %rax\n"
        "  testq %rax, %rax\n"
        "  je 2f\n"
        "  addsd %xmm7, %xmm7\n"
        "  movq %rax, (%rdi)\n"
        "2:\n"
        "  unpcklpd %xmm2, %xmm1\n"
        "  addpd %xmm1, %xmm7\n"
        "  decq %rdx\n"
        "  jnz 1b\n"
        "  movupd %xmm7, (%rdi)\n"
        "  popq %rbx\n  popq %rbp\n  ret\n");
void xk(double *out, const double *in, unsigned long n);
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
int main(void){
    static const double in[3] = { 1.5, 4.0, 0.0 };
    double out[2] __attribute__((aligned(16)));
    for (int rep = 0; rep < 3; rep++) {
        volatile double *vo = out; vo[0] = 0; vo[1] = 0;
        xk(out, in, 5000);
        g_u64 b0, b1; __builtin_memcpy(&b0, &out[0], 8); __builtin_memcpy(&b1, &out[1], 8);
        put("sum lo ", b0); put("sum hi ", b1);
    }
    g_puts("done\n");
    return 0;
}

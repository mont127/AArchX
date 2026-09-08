/* Exact shape of AppKit's view-transform helper exit path (NSViewGetTransformToDescendant's
 * callee): identity constants built with xorpd/movhpd/movsd, three unaligned 16-byte stores
 * through a base reloaded from a stack slot, then the pop-heavy epilogue.  Under the JIT this
 * produced a singular transform (AppKit assertion) while the interpreter was fine. */
#include "gsys.h"
__asm__(".text\n"
        ".globl _xf\n"
        "_xf:\n"
        "  pushq %rbp\n  movq %rsp, %rbp\n  pushq %r15\n  pushq %r14\n  pushq %r13\n  pushq %r12\n  pushq %rbx\n"
        "  subq $0x78, %rsp\n"
        "  movq %rdi, -0x78(%rbp)\n"
        "  movq %rsi, %r15\n"
        "  testq %rsi, %rsi\n"
        "  je 1f\n"
        /* loop-ish path: xmm2 = {tx,ty} from [rsi], xmm1 = {a,b}, xmm3 = {c,d} */
        "  movupd (%rsi), %xmm2\n"
        "  movupd 16(%rsi), %xmm1\n"
        "  movupd 32(%rsi), %xmm3\n"
        "  jmp 2f\n"
        "1:\n"
        "  xorpd %xmm2, %xmm2\n"
        "  xorpd %xmm3, %xmm3\n"
        "  movhpd _one(%rip), %xmm3\n"
        "  movsd _one(%rip), %xmm1\n"
        "2:\n"
        "  subpd %xmm2, %xmm1\n"
        "  movq -0x78(%rbp), %rax\n"
        "  movupd %xmm1, (%rax)\n"
        "  subpd %xmm2, %xmm3\n"
        "  movupd %xmm3, 0x10(%rax)\n"
        "  movupd %xmm2, 0x20(%rax)\n"
        "  addq $0x78, %rsp\n"
        "  popq %rbx\n  popq %r12\n  popq %r13\n  popq %r14\n  popq %r15\n  popq %rbp\n  ret\n"
        ".data\n.p2align 4\n_one:\n  .quad 0x3ff0000000000000\n  .quad 0x7ff8000000000000\n");
void xf(double *out, const double *in);
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
int main(void){
    double out[6] __attribute__((aligned(16)));
    for (int rep = 0; rep < 3; rep++) {
        volatile double *vo = out; for (int i = 0; i < 6; i++) vo[i] = -7.0 - i;
        xf(out, 0);
        for (int i = 0; i < 6; i++) { g_u64 b; __builtin_memcpy(&b, &out[i], 8); put("id ", b); }
        volatile double in[6]; in[0] = 10.0; in[1] = 20.0; in[2] = 1.5; in[3] = 2.5; in[4] = 3.5; in[5] = 4.5;
        xf(out, (const double *)in);
        for (int i = 0; i < 6; i++) { g_u64 b; __builtin_memcpy(&b, &out[i], 8); put("in ", b); }
    }
    g_puts("done\n");
    return 0;
}

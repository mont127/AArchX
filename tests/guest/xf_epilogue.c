/* Two paths into one join, then packed work and a callee-saved epilogue.  With
 * no input the vectors are assembled from partial writes (xorpd, movlpd,
 * movsd from memory, movhpd); with input they are loaded whole.  After the
 * join a scalar result is stored as a packed value, and every store goes
 * through a pointer reloaded from a stack slot at an unaligned offset.  The
 * join is reached often enough from both sides to be chained, so a lane-0
 * flush the JIT owes on either edge must still happen.  Golden from Rosetta. */
#include "gsys.h"
__asm__(".text\n"
        ".globl _pk\n"
        "_pk:\n"
        "  pushq %rbp\n  movq %rsp, %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n"
        "  subq $0x28, %rsp\n"
        "  movq %rdi, -0x20(%rbp)\n"
        "  movddup %xmm0, %xmm5\n"
        "  testq %rsi, %rsi\n"
        "  jne 1f\n"
        "  xorpd %xmm1, %xmm1\n"
        "  movlpd _k2(%rip), %xmm1\n"
        "  movsd _k3(%rip), %xmm2\n"
        "  movhpd _k4(%rip), %xmm2\n"
        "  xorpd %xmm4, %xmm4\n"
        "  movhpd _k5(%rip), %xmm4\n"
        "  jmp 2f\n"
        "1:\n"
        "  movupd (%rsi), %xmm1\n"
        "  movupd 16(%rsi), %xmm2\n"
        "  movupd 32(%rsi), %xmm4\n"
        "2:\n"
        "  mulpd %xmm5, %xmm1\n"
        "  addpd %xmm4, %xmm1\n"
        "  movq -0x20(%rbp), %rbx\n"
        "  movupd %xmm1, 8(%rbx)\n"
        "  subpd %xmm4, %xmm2\n"
        "  movupd %xmm2, 24(%rbx)\n"
        "  addsd %xmm1, %xmm4\n"
        "  movupd %xmm4, 40(%rbx)\n"
        "  addq $0x28, %rsp\n"
        "  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n  ret\n"
        ".data\n.p2align 3\n"
        "_k2:\n  .quad 0x4000000000000000\n"
        "_k3:\n  .quad 0x4008000000000000\n"
        "_k4:\n  .quad 0x4010000000000000\n"
        "_k5:\n  .quad 0x4014000000000000\n");
void pk(double *out, const double *in, double k);
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
static void dump(const char *tag, const double *out)
{
    for (int i = 0; i < 7; i++) { g_u64 b; __builtin_memcpy(&b, &out[i], 8); put(tag, b); }
}
int main(void){
    double out[7] __attribute__((aligned(16)));
    volatile double in[6]; in[0] = 10.0; in[1] = -20.0; in[2] = 1.5; in[3] = 2.5; in[4] = 3.25; in[5] = -4.75;
    for (int rep = 0; rep < 3; rep++) {
        for (int i = 0; i < 400; i++) {
            volatile double *vo = out; for (int j = 0; j < 7; j++) vo[j] = -1.0 - j;
            pk(out, (i & 1) ? (const double *)in : 0, 0.5 + i);
        }
        dump("last ", out);
        pk(out, 0, 3.0);
        dump("null ", out);
        pk(out, (const double *)in, -2.0);
        dump("in   ", out);
    }
    g_puts("done\n");
    return 0;
}

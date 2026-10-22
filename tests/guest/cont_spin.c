/*
 * An asynchronous stop while the thread loops inside a placed call
 * continuation (src/jit.c, "Continuation placement").  The loop is the block
 * at the return address of a call, so it runs in its caller's reservation,
 * entered at bl+4 by the callee's ret; the stop has to reach its stop site
 * there.  Run with OCERZ_TEST_ASYNC_STOP_MS: the golden is "start" and exit 0,
 * written by hand since natively the loop never ends.
 */
#include "gsys.h"

__asm__(
    ".text\n"
    ".p2align 4\n"
    "_work:\n"
    "    movl  $4, %ecx\n"
    "    movq  %rdi, %rax\n"
    "1:  addq  %rcx, %rax\n"
    "    decl  %ecx\n"
    "    jnz   1b\n"
    "    ret\n");

volatile g_u64 stop_flag;
__asm__(
    ".text\n"
    ".p2align 4\n"
    "_forever:\n"
    "    pushq %rbx\n"
    "    movq  %rdi, %rbx\n"
    "    callq _work\n"
    "1:  addq  %rbx, %rax\n"
    "    rorq  $3, %rax\n"
    "    cmpq  $0, _stop_flag(%rip)\n"
    "    je    1b\n"
    "    popq  %rbx\n"
    "    ret\n");
extern g_u64 forever(g_u64 x);

int main(void)
{
    g_puts("start\n");
    g_u64 s = 0;
    for (g_u64 i = 0; i < 6000; i++) {
        stop_flag = 1;
        s += forever(i);
    }
    stop_flag = 0;
    s += forever(s);
    g_putu64(s);
    return 0;
}

/*
 * fork after call continuations have been placed (src/jit.c, "Continuation
 * placement").  The parent runs calls whose continuations are laid out at
 * bl+4 and flip a branch inside one of them, then forks; the child abandons
 * the inherited arena, so it must not reach anything the parent laid out, and
 * runs the same code again from a fresh one, flips included, before the
 * parent does the same.  Golden from the native run.
 */
#include "gsys.h"

#define SYS_fork 2
#define SYS_wait4 7

static g_i64 guest_fork(g_u64 *is_child)
{
    register g_i64 rax __asm__("rax") = SYS(SYS_fork);
    register g_i64 rdx __asm__("rdx");
    unsigned char err;
    __asm__ volatile("syscall; setc %2"
                     : "+a"(rax), "=d"(rdx), "=qm"(err)
                     :
                     : "rcx", "r11", "memory", "cc");
    if (err)
        return -rax;
    *is_child = (g_u64)rdx;
    return rax;
}

static g_i64 guest_wait4(g_i64 pid, int *status)
{
    register g_i64 rax __asm__("rax") = SYS(SYS_wait4);
    register g_i64 rdi __asm__("rdi") = pid;
    register g_i64 rsi __asm__("rsi") = (g_i64)status;
    register g_i64 rdx __asm__("rdx") = 0;
    register g_i64 r10 __asm__("r10") = 0;
    __asm__ volatile("syscall"
                     : "+a"(rax)
                     : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10)
                     : "rcx", "r11", "memory", "cc");
    return rax;
}

__asm__(
    ".text\n"
    ".p2align 4\n"
    "_work:\n"
    "    movq  %rdi, %rcx\n"
    "    andq  $7, %rcx\n"
    "    leaq  1(%rdi), %rax\n"
    "1:  addq  %rcx, %rax\n"
    "    rorq  $5, %rax\n"
    "    decq  %rcx\n"
    "    jns   1b\n"
    "    ret\n");

volatile g_u64 sink[4];
/* spin(iters, k): after each call a branch on bit k of the count, flipped from phase to phase. */
__asm__(
    ".text\n"
    ".p2align 4\n"
    "_spin:\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    movq  %rdi, %r12\n"
    "    movq  %rsi, %r14\n"
    "    xorl  %r13d, %r13d\n"
    "    xorl  %ebx, %ebx\n"
    "1:  movq  %rbx, %rdi\n"
    "    callq _work\n"
    "    btq   %r14, %rbx\n"
    "    jc    2f\n"
    "    addq  %rax, %r13\n"
    "    movq  %r13, _sink(%rip)\n"
    "2:  xorq  %rbx, %r13\n"
    "    leaq  7(%rbx), %rdi\n"
    "    callq _work\n"
    "    subq  %rax, %r13\n"
    "    incq  %rbx\n"
    "    cmpq  %r12, %rbx\n"
    "    jb    1b\n"
    "    movq  %r13, %rax\n"
    "    popq  %r14\n"
    "    popq  %r13\n"
    "    popq  %r12\n"
    "    popq  %rbx\n"
    "    ret\n");
extern g_u64 spin(g_u64 iters, g_u64 k);

int main(void)
{
    g_puts("before ");
    g_putu64(spin(30000, 11));
    g_u64 is_child = 0;
    g_i64 pid = guest_fork(&is_child);
    if (pid < 0) {
        g_puts("fork failed\n");
        return 1;
    }
    if (is_child) {
        g_puts("child ");
        g_putu64(spin(30000, 11));
        g_puts("child short phases ");
        g_putu64(spin(20000, 5));
        sys_exit(9);
    }
    int status = 0;
    guest_wait4(pid, &status);
    g_puts("child status ");
    g_putu64((g_u64)((status >> 8) & 0xff));
    g_puts("parent ");
    g_putu64(spin(30000, 11));
    g_puts("parent short phases ");
    g_putu64(spin(20000, 5));
    return 0;
}

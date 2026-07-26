/* Precision of the guest rip reported in a signal frame. */
#include "gsys.h"

#define SYS_sigaction 46
#define SIGSEGV 11
#define SA_SIGINFO 0x0040

struct k_sigaction {
    g_u64 handler;
    g_u64 tramp;
    g_u32 mask;
    g_u32 flags;
};

__asm__(
    ".text\n"
    ".globl _sig_tramp\n"
    "_sig_tramp:\n"
    "    pushq %rbp\n"
    "    movq  %rsp, %rbp\n"
    "    movq  %r8, %rbx\n"
    "    movq  %r9, %r12\n"
    "    movq  %rdi, %rax\n"
    "    movl  %edx, %edi\n"
    "    movq  %rcx, %rsi\n"
    "    movq  %r8, %rdx\n"
    "    callq *%rax\n"
    "    movq  %rbx, %rdi\n"
    "    movl  $0x1e, %esi\n"
    "    movq  %r12, %rdx\n"
    "    movl  $0x20000b8, %eax\n"
    "    syscall\n"
    "    ud2\n");

extern void sig_tramp(void);

extern char fault_insn[];

static void recover(void)
{
    sys_exit(0);
}

static void handler(int signo, void *siginfo, void *ucontext)
{
    g_u64 uc = (g_u64)ucontext;
    g_u64 mc = *(g_u64 *)(uc + 48);
    g_u64 rip = *(g_u64 *)(mc + 144);

    if (rip == (g_u64)(g_u64 *)fault_insn)
        g_puts("rip exact\n");
    else
        g_puts("rip WRONG\n");

    *(g_u64 *)(mc + 144) = (g_u64)(g_u64 *)&recover;
}

int main(int argc, char **argv, char **envp)
{
    struct k_sigaction sa;
    sa.handler = (g_u64)&handler;
    sa.tramp = (g_u64)&sig_tramp;
    sa.mask = 0;
    sa.flags = SA_SIGINFO;
    g_syscall3(SYS(SYS_sigaction), SIGSEGV, (g_i64)(g_u64)&sa, 0);

    g_puts("before fault\n");

    volatile g_u64 *p = (volatile g_u64 *)0;
    g_u64 v = 0x1234;
    __asm__ __volatile__(
        ".globl _fault_insn\n"
        "_fault_insn:\n"
        "    movq %[val], (%[ptr])\n"
        :
        : [val] "r"(v), [ptr] "r"(p)
        : "memory");

    g_puts("unreachable\n");
    return 2;
}

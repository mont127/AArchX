/* Is the guest signal mask enforced?  Block SIGUSR1, raise it, and check the
 * handler does NOT run until the mask is lowered.  Deterministic, no race. */
#include "gsys.h"

#define SYS_sigaction 46
#define SYS_sigprocmask 48
#define SYS_sigpending 52
#define SYS_kill 37

#define SIGUSR1 30
#define SA_SIGINFO 0x0040
#define SIG_BLOCK 1
#define SIG_SETMASK 3

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

static void handler(int signo, void *si, void *uc)
{
    (void)signo; (void)si; (void)uc;
    g_puts("HANDLER\n");
}

int main(void)
{
    struct k_sigaction sa;
    sa.handler = (g_u64)&handler;
    sa.tramp = (g_u64)&sig_tramp;
    sa.mask = 0;
    sa.flags = SA_SIGINFO;
    g_syscall3(SYS(SYS_sigaction), SIGUSR1, (g_i64)(g_u64)&sa, 0);

    g_u32 set = 1u << (SIGUSR1 - 1);
    g_syscall3(SYS(SYS_sigprocmask), SIG_BLOCK, (g_i64)(g_u64)&set, 0);

    g_i64 pid = sys_getpid();
    g_syscall3(SYS(SYS_kill), pid, SIGUSR1, 1);
    g_puts("after kill\n");

    g_u32 pend = 0;
    g_syscall1(SYS(SYS_sigpending), (g_i64)(g_u64)&pend);
    g_puts(pend & (1u << (SIGUSR1 - 1)) ? "pending yes\n" : "pending no\n");

    g_u32 none = 0;
    g_syscall3(SYS(SYS_sigprocmask), SIG_SETMASK, (g_i64)(g_u64)&none, 0);
    g_puts("after unblock\n");
    return 0;
}

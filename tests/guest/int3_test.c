/* A guest int3 must raise SIGTRAP to the guest's own handler and then resume
 * at the instruction after it, the way a Windows app's breakpoint handler
 * expects.  Steam's assert path does exactly this. */
#include "gsys.h"

#define SYS_sigaction 46
#define SIGTRAP 5
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

static volatile int hits;

static void handler(int signo, void *si, void *uc)
{
    (void)si; (void)uc;
    hits++;
    g_puts(signo == SIGTRAP ? "TRAP\n" : "WRONGSIG\n");
}

int main(void)
{
    struct k_sigaction sa;
    sa.handler = (g_u64)&handler;
    sa.tramp = (g_u64)&sig_tramp;
    sa.mask = 0;
    sa.flags = SA_SIGINFO;
    g_syscall3(SYS(SYS_sigaction), SIGTRAP, (g_i64)(g_u64)&sa, 0);

    g_puts("before\n");
    __asm__ __volatile__("int3");
    g_puts("after\n");

    g_puts(hits == 1 ? "hits 1\n" : "hits wrong\n");
    return 0;
}

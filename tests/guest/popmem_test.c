/* Fault atomicity of pop qword [mem]. */
#include "gsys.h"

#define SYS_sigaction 46
#define SIGSEGV 11
#define SIGBUS 10
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
extern char pm_fault[];

g_u64 g_expect_rsp;

static void recover(void)
{
    sys_exit(0);
}

static void handler(int signo, void *siginfo, void *ucontext)
{
    (void)signo; (void)siginfo;
    g_u64 uc = (g_u64)ucontext;
    g_u64 mc = *(g_u64 *)(uc + 48);
    g_u64 rip = *(g_u64 *)(mc + 144);
    g_u64 rsp = *(g_u64 *)(mc + 72);

    const char *msg =
        (rip == (g_u64)(g_u64 *)pm_fault)
            ? (rsp == g_expect_rsp ? "rip exact rsp exact\n" : "rip exact rsp WRONG\n")
            : (rsp == g_expect_rsp ? "rip WRONG rsp exact\n" : "rip WRONG rsp WRONG\n");
    g_puts(msg);

    *(g_u64 *)(mc + 144) = (g_u64)(g_u64 *)&recover;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    struct k_sigaction sa;
    sa.handler = (g_u64)&handler;
    sa.tramp = (g_u64)&sig_tramp;
    sa.mask = 0;
    sa.flags = SA_SIGINFO;
    g_syscall3(SYS(SYS_sigaction), SIGSEGV, (g_i64)(g_u64)&sa, 0);
    g_syscall3(SYS(SYS_sigaction), SIGBUS, (g_i64)(g_u64)&sa, 0);

    g_puts("before fault\n");

    g_u64 val = 0xfeedfacecafebeefull;
    g_u64 *ptr = (g_u64 *)0;

    __asm__ __volatile__(
        "movq %%rsp, %%r13\n"
        "subq $8, %%r13\n"
        "movq %%r13, %[exp]\n"
        "pushq %[val]\n"
        ".globl _pm_fault\n"
        "_pm_fault:\n"
        "popq (%[ptr])\n"
        : [exp] "=m"(g_expect_rsp)
        : [val] "r"(val), [ptr] "r"(ptr)
        : "memory", "r13");

    g_puts("unreachable\n");
    return 2;
}

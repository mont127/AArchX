/*
 * tests/guest/signal_jump0.c
 *
 * Regression test for the JIT translate-under-lock fault path (the wineboot
 * rip=0 wall). Unlike signal_test, which faults on a null DATA store, this
 * program transfers control to address 0 — an instruction FETCH fault at rip=0.
 * Under the JIT that fault occurs inside the block translator while it holds the
 * translation lock; ocerz must unwind translation cleanly (releasing the lock)
 * and let the interpreter re-fault and deliver the guest SIGSEGV, rather than
 * orphaning the lock and deadlocking. The handler rewrites the saved instruction
 * pointer to a recovery routine and returns through __sigreturn, exactly like
 * signal_test; a clean exit 0 (no hang) proves the path works under both tiers.
 *
 * The trampoline mirrors the macOS x86_64 _sigtramp register contract (handler
 * in rdi, edx=signo, rcx=siginfo*, r8=ucontext*, r9=token); ucontext->uc_mcontext
 * sits at offset 48 and the saved rip within it at offset 144.
 */
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
    ".globl _sig_tramp0\n"
    "_sig_tramp0:\n"
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

extern void sig_tramp0(void);

static void recover(void)
{
    g_puts("recovered\n");
    sys_exit(0);
}

static void handler(int signo, void *siginfo, void *ucontext)
{
    g_puts("handler\n");
    g_u64 uc = (g_u64)ucontext;
    g_u64 mc = *(g_u64 *)(uc + 48);
    *(g_u64 *)(mc + 144) = (g_u64)(g_u64 *)&recover;
}

int main(int argc, char **argv, char **envp)
{
    struct k_sigaction sa;
    sa.handler = (g_u64)&handler;
    sa.tramp = (g_u64)&sig_tramp0;
    sa.mask = 0;
    sa.flags = SA_SIGINFO;
    g_syscall3(SYS(SYS_sigaction), SIGSEGV, (g_i64)(g_u64)&sa, 0);

    g_puts("before jump\n");
    volatile g_u64 target = 0;
    void (*fn)(void) = (void (*)(void))(g_u64)target;
    fn();
    g_puts("unreachable\n");
    return 2;
}

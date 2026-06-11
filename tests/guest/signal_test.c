/*
 * tests/guest/signal_test.c
 *
 * Exercises Ocerz guest signal delivery end to end without libSystem. The
 * program installs a SIGSEGV handler through the raw __sigaction syscall
 * (number 46), supplying a private trampoline that reproduces the macOS x86_64
 * _sigtramp register contract, then deliberately stores through a null pointer
 * to raise SIGSEGV. The emulator's fault handler must convert that host fault
 * into a Darwin guest-signal delivery: build a signal frame, redirect the guest
 * to the trampoline, which calls the handler and then issues __sigreturn
 * (syscall 184). The handler rewrites the saved instruction pointer inside the
 * delivered ucontext so the sigreturn resumes in a recovery routine that prints
 * a deterministic line and exits 0. A clean exit proves ocerz_signal_deliver and
 * sys_sigreturn round-trip correctly; with no delivery the null store would
 * instead abort the whole emulator with status 139.
 *
 * Trampoline ABI (verified against this host's libsystem_platform _sigtramp):
 * entered with the handler in rdi, signal number in edx, siginfo pointer in rcx,
 * ucontext pointer in r8 and the sigreturn token in r9; it calls
 * handler(signo=edi, siginfo=rsi, ucontext=rdx) then __sigreturn(ucontext, 0x1e,
 * token). The push at entry restores the 16-byte call alignment the ABI expects.
 * The ucontext's uc_mcontext pointer sits at offset 48 and the saved rip within
 * that mcontext at offset 144, matching the frame ocerz_signal_deliver writes.
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
    sa.tramp = (g_u64)&sig_tramp;
    sa.mask = 0;
    sa.flags = SA_SIGINFO;
    g_syscall3(SYS(SYS_sigaction), SIGSEGV, (g_i64)(g_u64)&sa, 0);

    g_puts("before fault\n");
    volatile int *p = (volatile int *)0;
    *p = 1;
    g_puts("unreachable\n");
    return 2;
}

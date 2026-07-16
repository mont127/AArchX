/*
 * tests/guest/rip_test.c
 *
 * Guards the PRECISION of the guest rip delivered in a signal frame when the
 * faulting instruction is one the JIT INLINES as native arm64.
 *
 * Why this exists as a separate test from signal_test.c: signal_test faults on
 * `*p = 1`, a MOV mem<-IMMEDIATE, which emit_mov_mem does NOT inline (it only
 * covers mem<-REG). That store therefore takes the interpreter slow path, and
 * ocerz_jit_exec_one sets cpu->rip for every slow instruction, so the delivered
 * rip is exact by accident. It cannot see the bug this test targets.
 *
 * The bug: inlined instructions never write cpu->rip (jit.c justifies that on
 * "inlined ops never read it" -- true for a register move, FALSE for a faulting
 * memory access). So a fault inside inlined code used to report the rip of the
 * last SLOW instruction, silently mislocating every guest fault taken in
 * inlined code. That matters for real guests: Wine grows thread stacks from the
 * guard-page fault's rip, and a debugger/unwinder cannot work from a wrong one.
 *
 * The test forces the inlined shape with inline asm (`movq %reg, (%reg)` at
 * size 8, no segment override -- exactly emit_mov_mem's mem<-REG arm), labels
 * the faulting instruction, and has the handler compare the rip delivered in
 * the ucontext against that label. It prints "rip exact" only on an exact
 * match, so the golden output pins the precise value, not an approximation.
 *
 * Trampoline ABI and ucontext offsets are identical to signal_test.c.
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
/* The address of the faulting store, defined by the asm label below. */
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

    /* Resume in recover() regardless, so the test's exit code reports the
     * harness working and the STDOUT line reports the rip verdict. */
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

    /* MOV mem<-REG at operand size 8 with no segment override: precisely the
     * form emit_mov_mem inlines to a native str. Written in asm so no compiler
     * choice (folding the value to an immediate, widening, reordering) can move
     * it off the inlined path and quietly stop testing anything. */
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

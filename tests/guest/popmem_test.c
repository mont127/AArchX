/*
 * tests/guest/popmem_test.c
 *
 * Guards FAULT ATOMICITY of `pop qword [mem]`.
 *
 * A faulting x86 instruction is ABORTED: it has no architectural effect, so the
 * RSP a real x86 kernel reports in the signal frame for a faulting
 * `popq (%reg)` is the value BEFORE the pop. This test asserts exactly that,
 * against the same frame layout rip_test.c/signal_test.c use.
 *
 * Why it matters now: crash_handler (src/vm.c) rewinds cpu->rip to the FAULTING
 * instruction before delivering a guest SIGSEGV, so a handler that fixes the
 * page and returns genuinely RE-EXECUTES the instruction. That makes fault
 * atomicity a REQUIREMENT of every interpreter handler. ocerz_push was audited
 * and fixed for exactly this (it now stores before committing rsp, see
 * interp_common.h). op_stack's POP was not:
 *
 *      uint64_t v = ocerz_pop(cpu, size);            // rsp += 8 ALREADY
 *      ocerz_write_op(cpu, insn, &insn->ops[0], v);  // may FAULT
 *
 * so when the destination is MEMORY and the store faults, rsp has already been
 * incremented. The frame therefore reports rsp+8, and on a fix-and-retry the
 * pop reads the NEXT slot and increments rsp a SECOND time.
 *
 * The golden (verified on REAL HARDWARE natively under Rosetta) is "rsp exact".
 *
 * Trampoline ABI and ucontext offsets are identical to rip_test.c; __rsp lives
 * at mcontext+72 (16-byte __es, then __ss: rax,rbx,rcx,rdx,rdi,rsi,rbp,rsp).
 */
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

/* RSP as it stands immediately after the push, i.e. what a real x86 kernel must
 * report for the aborted pop. Written from inside the asm block so no compiler
 * frame choice can make it disagree with the actual stack pointer. */
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

    /* ONE write: emitting several from inside the handler proved unreliable
     * under Rosetta (the second syscall did not reload edi/edx), which would
     * silently swallow the verdict this test exists to report. */
    const char *msg =
        (rip == (g_u64)(g_u64 *)pm_fault)
            ? (rsp == g_expect_rsp ? "rip exact rsp exact\n" : "rip exact rsp WRONG\n")
            : (rsp == g_expect_rsp ? "rip WRONG rsp exact\n" : "rip WRONG rsp WRONG\n");
    g_puts(msg);

    /* Resume in recover() so the verdict lines are the whole observable output. */
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
    g_u64 *ptr = (g_u64 *)0;   /* a NULL store faults under ocerz and real hw */

    /* push a known value, then pop it into a faulting address. The pop is
     * aborted, so the frame must still show the post-push rsp. Written in asm
     * so the compiler cannot pick a different pop form (8F /0 is what we want). */
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

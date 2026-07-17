/*
 * tests/guest/fault_regs.c
 *
 * Takes a SIGSEGV in the MIDDLE of a reuse-heavy block with every data GPR
 * live, and prints all of them (plus rflags, trapno and the fault address)
 * from the delivered ucontext. This is a GATE test, not a feature test.
 *
 * WHY IT EXISTS. The differential suite's proof that JIT == interp is a
 * comparison of program stdout at NORMAL EXIT (run_diff_test.sh), and the
 * flags tests capture rflags with PUSHF on the normal path. Neither can see
 * the register file that a FAULT observes. That is a real blind spot the
 * moment guest registers stop living in cpu->gpr: a translator that keeps
 * guest state in host registers must reconstruct cpu->gpr from the host
 * mcontext when a guest instruction faults, and if it reconstructs the wrong
 * values -- or silently skips the reconstruction -- every existing test still
 * passes. The corruption is only visible to code that actually READS the
 * registers at fault time, i.e. a signal handler. This test is that reader.
 *
 * So this exists BEFORE any register allocation does, and it must pass on the
 * unmodified build first: a gate that only starts passing after the change it
 * guards is not a gate, it is a rationalization.
 *
 * SHAPE. The faulting body is hand-written asm, not C, for two reasons. The
 * register assignment must be guaranteed rather than left to the compiler's
 * scheduler, and the block must be REUSE-HEAVY -- every register written and
 * re-read several times -- because that is precisely the shape a hot-subset
 * allocator will choose to pin. A fault in a block with no pinned registers
 * proves nothing about the reconstruction path.
 *
 * DETERMINISM. Only the 14 data registers, rflags, trapno and the fault
 * address are printed. rbp/rsp/rip are deliberately NOT printed: they are
 * addresses, they vary between the native run that generates the golden and
 * the emulated run that is checked against it, and a test whose golden is
 * machine-dependent is worse than no test. Every value printed here is a pure
 * function of the instruction stream.
 *
 * rflags is masked to the six architecturally-defined status flags
 * (CF PF AF ZF SF OF = 0x8d5). The final flag producer before the fault is the
 * `addq %rbx, %rcx` immediately preceding it -- the null-pointer materialization
 * is placed BEFORE that add on purpose, since `xorq %r11,%r11` would otherwise
 * be the last flag writer and the interesting flag state would be lost. `movq`
 * does not write flags, so the flags observed in the handler are exactly the
 * ones the add produced.
 *
 * Trampoline ABI and the ucontext/mcontext offsets are the same ones
 * signal_test.c documents and verifies against this host's _sigtramp.
 *
 * mcontext (__darwin_mcontext64) layout used below, offsets from mc:
 *    0 trapno(u16) 2 cpu(u16) 4 err(u32) 8 faultvaddr(u64)     <- exception state
 *   16 rax 24 rbx 32 rcx 40 rdx 48 rdi 56 rsi 64 rbp 72 rsp    <- thread state
 *   80 r8 88 r9 96 r10 104 r11 112 r12 120 r13 128 r14 136 r15
 *  144 rip 152 rflags
 * Note the x86_64 thread-state order is rax,rbx,rcx,rdx,RDI,RSI -- rdi comes
 * before rsi, which is not the encoding order and is easy to get backwards.
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

/*
 * The faulting body. Thirteen registers get distinct constants, then two rounds
 * of a dependent add/xor/sub chain so each is written and re-read repeatedly --
 * a_r well above any sane pin threshold. rsp/rbp are never touched, so the
 * stack stays valid for the handler and for recover().
 *
 * Callee-saved registers are destroyed on purpose. Control never returns here:
 * the handler rewrites the saved rip so sigreturn lands in recover(), which
 * only writes a string and exits.
 */
__asm__(
    ".text\n"
    ".globl _fault_body\n"
    "_fault_body:\n"
    "    movabsq $0x0102030405060708, %rax\n"
    "    movabsq $0x1122334455667788, %rbx\n"
    "    movabsq $0x00000000deadbeef, %rcx\n"
    "    movabsq $0xfedcba9876543210, %rdx\n"
    "    movabsq $0x000000000000f00d, %rsi\n"
    "    movabsq $0x0f0f0f0f0f0f0f0f, %rdi\n"
    "    movabsq $0x5555555555555555, %r8\n"
    "    movabsq $0xaaaaaaaaaaaaaaaa, %r9\n"
    "    movabsq $0x123456789abcdef0, %r10\n"
    "    movabsq $0x1111111111111111, %r12\n"
    "    movabsq $0x2222222222222222, %r13\n"
    "    movabsq $0x3333333333333333, %r14\n"
    "    movabsq $0x4444444444444444, %r15\n"
    /* round 1 */
    "    addq %rbx, %rax\n"
    "    xorq %rax, %rcx\n"
    "    subq %rcx, %rdx\n"
    "    addq %rdx, %rsi\n"
    "    xorq %rsi, %rdi\n"
    "    addq %rdi, %r8\n"
    "    subq %r8, %r9\n"
    "    xorq %r9, %r10\n"
    "    addq %r10, %r12\n"
    "    xorq %r12, %r13\n"
    "    subq %r13, %r14\n"
    "    addq %r14, %r15\n"
    /* round 2: re-read everything written in round 1 */
    "    addq %r15, %rax\n"
    "    xorq %rax, %rbx\n"
    "    subq %rbx, %rdx\n"
    "    addq %rdx, %rdi\n"
    "    xorq %rdi, %r9\n"
    "    subq %r9, %r13\n"
    "    addq %r13, %rsi\n"
    "    xorq %rsi, %r10\n"
    "    addq %r10, %r8\n"
    "    subq %r8, %r12\n"
    "    xorq %r12, %r14\n"
    /* null pointer FIRST, so it is not the last flag writer */
    "    xorq %r11, %r11\n"
    /* the flag state observed at the fault is exactly this add's */
    "    addq %rbx, %rcx\n"
    /* FAULT: movq writes no flags, so nothing perturbs the state above */
    "    movq %rax, (%r11)\n"
    "    ud2\n");

extern void sig_tramp(void);
extern void fault_body(void);

static void recover(void)
{
    g_puts("recovered\n");
    sys_exit(0);
}

/* g_puthex64 terminates its own line; do not add another. */
static void p(const char *name, g_u64 v)
{
    g_puts(name);
    g_puthex64(v);
}

static void handler(int signo, void *siginfo, void *ucontext)
{
    g_u64 uc = (g_u64)ucontext;
    g_u64 mc = *(g_u64 *)(uc + 48);

    p("trapno     ", *(g_u64 *)(mc + 0) & 0xffff);
    p("faultvaddr ", *(g_u64 *)(mc + 8));
    p("rax ", *(g_u64 *)(mc + 16));
    p("rbx ", *(g_u64 *)(mc + 24));
    p("rcx ", *(g_u64 *)(mc + 32));
    p("rdx ", *(g_u64 *)(mc + 40));
    p("rdi ", *(g_u64 *)(mc + 48));
    p("rsi ", *(g_u64 *)(mc + 56));
    p("r8  ", *(g_u64 *)(mc + 80));
    p("r9  ", *(g_u64 *)(mc + 88));
    p("r10 ", *(g_u64 *)(mc + 96));
    p("r11 ", *(g_u64 *)(mc + 104));
    p("r12 ", *(g_u64 *)(mc + 112));
    p("r13 ", *(g_u64 *)(mc + 120));
    p("r14 ", *(g_u64 *)(mc + 128));
    p("r15 ", *(g_u64 *)(mc + 136));
    /* CF PF AF ZF SF OF only: the rest of rflags carries bits whose value at a
     * fault is not something this test should pretend to pin down. */
    p("rflags&8d5 ", *(g_u64 *)(mc + 152) & 0x8d5);

    /* Resume in recover() rather than at the faulting store. */
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
    fault_body();
    g_puts("unreachable\n");
    return 2;
}

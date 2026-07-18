/*
 * tests/guest/fault_chain.c
 *
 * FAULT-INSIDE-A-CHAINED-SELF-LOOP gate for JIT block chaining (Increment 2).
 *
 * WHY IT EXISTS. fault_regs.c proves the register-pinning fault seam for a
 * STRAIGHT-LINE block that executes once: a SIGSEGV mid-block recovers the
 * pinned guest GPRs from the host mcontext's x21..x28 and delivers the exact
 * faulting rip. It does NOT reach a CHAINED self-loop. Self-loop chaining keeps
 * the pinned guest GPRs resident in x21..x28 ACROSS loop iterations by branching
 * the taken back-edge straight to body-entry (past the prologue), never
 * returning to the dispatcher. A fault taken on the Nth (N>1) iteration must
 * therefore still (a) map host_pc -> the block -> the exact faulting guest rip,
 * and (b) recover the CURRENT-iteration pinned register values from x21..x28 --
 * NOT the stale block-entry values sitting in cpu->gpr[]. If the chained loop
 * corrupted either, every straight-line test still passes; only a reader that
 * faults deep inside a chained loop and prints the register file can see it.
 * This program is that reader, and its golden is the NATIVE x86_64 run, so it
 * holds ocerz to real hardware, not to itself.
 *
 * SHAPE. Hand-written asm so the block is (1) a real self-loop -- the setup ends
 * in `jmp Lchain_loop`, a terminator, so Lchain_loop begins a fresh JIT block
 * whose entry rip EQUALS the jnz taken target (the self-loop the chainer
 * recognizes and back-edges past the prologue); and (2) reuse-heavy and >= 24
 * instructions, so the pin/defer size gate fires and the accumulators live in
 * x21..x28. Ten accumulators fold the loop counter rcx through two dependent
 * add/xor rounds every iteration, so their values are a function of the
 * iteration number. Disassembly of the compiled block (OCERZ_JITDIS) confirms
 * both properties: n_pinned>0 (prologue saves x21..x28) and the terminating Jcc
 * emits the self-loop back-edge -- `subs` against the entry rip, the
 * cpu->interrupt poll (`ldr w,[x20,#interrupt]; cbnz`), and a `b` back to
 * body-entry.
 *
 * THE FAULT, DETERMINISTIC AND BRANCH-FREE. A conditional *branch* in the body
 * would end the block early and destroy the single-block self-loop, so the fault
 * is selected with a branch-free CMOV instead. rcx counts 50 -> 49 -> ... ; every
 * iteration the store base is buf, EXCEPT when rcx == 10, where `cmpq;cmovne`
 * leaves it NULL. The store scribbles a scratch buffer for 40 iterations and then,
 * on the 41st (rcx == 10), stores through NULL -> SIGSEGV. NULL is used (rather
 * than a high uncommitted address, which ocerz commits on demand) because it
 * faults identically on native x86, the interpreter, and the JIT. The iteration
 * at which it faults depends only on the constants, so all three fault at the same
 * logical point with identical register values.
 *
 * DETERMINISM. Only iteration-pure values are printed: the ten accumulators,
 * rcx (== 10 at the fault), the fault trapno and address (0), the masked status
 * flags, and the fault rip AS AN OFFSET from the fault_body symbol (rip and the
 * symbol both carry the load base, so their difference is load-base-independent
 * and machine-portable -- and it pins the exact faulting instruction, i.e. the
 * host_pc -> block -> guest-rip mapping evaluated at a host pc reached only via
 * the chained back-edge). The scratch-buffer base (r15) and rbp/rsp are never
 * printed.
 *
 * MUTATION TEST (how this earns its place). Break the JIT fault seam and this
 * three-way equivalence fails while the interpreter (which uses cur_rip and never
 * pins) still passes:
 *   - fault-rip mapping: perturb ocerz_jit_fault_rip's returned rip -- the
 *     printed riptoff, resolved from a host pc deep in the chained loop, goes
 *     wrong (verified during review: riptoff shifted by the perturbation, JIT !=
 *     golden, interp still == golden). This is the seam that must survive the
 *     prologue-skipping back-edge.
 * The pinned-register recovery (x21..x28 -> cpu->gpr) is proven CORRECT here by
 * the three-way match on the accumulators; its dedicated mutation-teeth live in
 * fault_regs.c, whose null store takes the no-spill inline path where a broken
 * ocerz_jit_fault_recover_regs prints stale registers. (On this binary's memory
 * layout the null store instead routes through the spill-first path, which makes
 * cpu->gpr current independently, so the recovery no-op is masked here -- a
 * layout artifact, not a coverage gap: the chained-loop-specific seam this test
 * uniquely guards is the host_pc->block->rip mapping across the back-edge.)
 *
 * Trampoline/handler ABI and mcontext offsets are exactly fault_regs.c's.
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

/* Scratch store target for the non-faulting iterations. Its address is machine
 * dependent and is never printed; only stored into. Not static and marked used:
 * it is referenced only from inline asm, which the C compiler does not scan. */
g_u64 g_buf[16] __attribute__((used));

/* fault_body's runtime address, captured in main, so the handler can report the
 * faulting rip as a load-base-independent offset. */
static g_u64 g_body_addr;

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
 * The chained self-loop. SETUP loads distinct constants and the counter, then
 * `jmp Lloop` terminates the setup block so Lloop starts a NEW block whose entry
 * rip == the jnz taken target below == a self-loop. rsp/rbp are never touched.
 * r15 holds the scratch buffer base for the whole loop. Control never returns:
 * the handler rewrites the saved rip so sigreturn lands in recover().
 */
__asm__(
    ".text\n"
    ".globl _fault_body\n"
    "_fault_body:\n"
    "    movabsq $0x0102030405060708, %rax\n"
    "    movabsq $0x1122334455667788, %rbx\n"
    "    movabsq $0xfedcba9876543210, %rdx\n"
    "    movabsq $0x000000000000f00d, %rsi\n"
    "    movabsq $0x0f0f0f0f0f0f0f0f, %rdi\n"
    "    movabsq $0x5555555555555555, %r8\n"
    "    movabsq $0xaaaaaaaaaaaaaaaa, %r9\n"
    "    movabsq $0x123456789abcdef0, %r10\n"
    "    movabsq $0x1111111111111111, %r12\n"
    "    movabsq $0x2222222222222222, %r13\n"
    "    movq    $50, %rcx\n"                 /* loop counter: 50..down */
    "    leaq    _g_buf(%rip), %r15\n"        /* scratch store base (never printed) */
    "    jmp     Lchain_loop\n"               /* terminator: Lchain_loop is a fresh block */
    "Lchain_loop:\n"
    /* round 1: fold the counter through the ten accumulators (reuse-heavy) */
    "    addq %rcx, %rax\n"
    "    xorq %rax, %rbx\n"
    "    addq %rbx, %rdx\n"
    "    xorq %rdx, %rsi\n"
    "    addq %rsi, %rdi\n"
    "    xorq %rdi, %r8\n"
    "    addq %r8, %r9\n"
    "    xorq %r9, %r10\n"
    "    addq %r10, %r12\n"
    "    xorq %r12, %r13\n"
    /* round 2: re-read everything written in round 1 */
    "    addq %r13, %rax\n"
    "    xorq %rax, %rbx\n"
    "    addq %rbx, %rdx\n"
    "    xorq %rdx, %rsi\n"
    "    addq %rsi, %rdi\n"
    "    xorq %rdi, %r8\n"
    "    addq %r8, %r9\n"
    "    xorq %r9, %r10\n"
    "    addq %r10, %r12\n"
    "    xorq %r12, %r13\n"
    /* BRANCH-FREE fault selector (CMOV, so the block is NOT split by a branch and
     * stays a single self-loop the chainer recognizes). The store base is buf on
     * every iteration EXCEPT rcx==10, when it is NULL -- a guest address that
     * faults deterministically on native x86, on the interpreter, and on the JIT
     * alike (unlike a high uncommitted arena address, which ocerz commits on
     * demand). The store thus faults deep inside the chained, pinned self-loop
     * (41st iteration), exercising crash_handler's host_pc -> block -> exact rip
     * mapping and the x21..x28 pin recovery for a chained back-edge that skipped
     * the prologue. r11 is the store base only, kept below the pin threshold. */
    "    xorq %r11, %r11\n"                   /* r11 = 0 (NULL) */
    "    cmpq $10, %rcx\n"                    /* ZF=1 iff this is the fault iteration */
    "    cmovneq %r15, %r11\n"                /* other iters: r11 = buf (valid store) */
    "    movq %rax, (%r11)\n"                 /* FAULT here on the 41st iter (rcx==10) */
    "    decq %rcx\n"
    "    jnz  Lchain_loop\n"                  /* taken target == block entry -> self-loop */
    "    ud2\n");

extern void fault_body(void);
extern void sig_tramp(void);

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
    /* rip as a load-base-independent offset: pins the exact faulting instruction
     * inside the chained loop without printing a machine-dependent address. */
    p("riptoff    ", *(g_u64 *)(mc + 144) - g_body_addr);
    p("rax ", *(g_u64 *)(mc + 16));
    p("rbx ", *(g_u64 *)(mc + 24));
    p("rcx ", *(g_u64 *)(mc + 32));
    p("rdx ", *(g_u64 *)(mc + 40));
    p("rdi ", *(g_u64 *)(mc + 48));
    p("rsi ", *(g_u64 *)(mc + 56));
    p("r8  ", *(g_u64 *)(mc + 80));
    p("r9  ", *(g_u64 *)(mc + 88));
    p("r10 ", *(g_u64 *)(mc + 96));
    p("r12 ", *(g_u64 *)(mc + 112));
    p("r13 ", *(g_u64 *)(mc + 120));
    p("rflags&8d5 ", *(g_u64 *)(mc + 152) & 0x8d5);

    /* Resume in recover() rather than at the faulting store. */
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

    g_body_addr = (g_u64)&fault_body;

    g_puts("before fault\n");
    fault_body();
    g_puts("unreachable\n");
    return 2;
}

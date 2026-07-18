/*
 * tests/guest/fault_link.c
 *
 * FAULT-INSIDE-A-CROSS-BLOCK-LINKED-LOOP gate for GENERAL two-way Jcc block
 * linking.
 *
 * WHY IT EXISTS. fault_chain.c proves the fault seam for a chained SELF-LOOP (one
 * block, one register map, back-edge to its own body-entry). General two-way
 * linking chains DISTINCT blocks with DIFFERENT hot-register subsets edge-to-edge
 * via the gpr[] seam: the source block's chain tail spills its pinned regs to
 * cpu->gpr[] and tears its frame down, and the target block's prologue reloads
 * ITS (different) pinned subset from cpu->gpr[]. If a cross-block edge instead
 * took the self-loop body-entry shortcut (branching past the target's prologue),
 * the target would run with the SOURCE's registers still in x21..x28 -- corrupt.
 * If a chain tail dropped its spill, the target would reload STALE gpr[]. Either
 * bug misassigns the accumulators. A straight-line or self-loop test cannot see
 * it: only a loop that repeatedly crosses between blocks with different maps and
 * then FAULTS, printing the register file, exposes a mis-reconciled edge. This is
 * that reader; its golden is the NATIVE x86_64 run, so it holds ocerz to real
 * hardware, not to itself, and the interp==jit differential adds a second axis.
 *
 * SHAPE. Hand-written asm forming a genuine multi-block loop:
 *   setup            -> jmp La               (terminator: La is a fresh block)
 *   La   (subset A = rax,rbx,rdx,rsi,rdi,r8; 24 reuse-heavy insns) then a
 *        data-dependent `testq $1,%rax; jz Lbe` -- a REAL two-way Jcc whose taken
 *        (Lbe) and fall (Lbo) targets are BOTH other blocks (the forward diamond
 *        general linking chains).
 *   Lbe  / Lbo  (subset B = r9,r10,r12,r13,r14; 24 reuse-heavy insns each, folded
 *        differently so the two arms diverge) each end `jmp Lmerge`.
 *   Lmerge  the branch-free NULL-store fault selector and the loop back-edge
 *        `decq %rcx; jnz La` (taken target La == a back-edge general linking
 *        chains with the interrupt poll; here the loop is bounded by the fault so
 *        the poll is irrelevant to termination).
 * The >=24-insn reuse-heavy bodies make the pin/defer size gate fire, so subset A
 * lives in x21..x28 across La and subset B across the arms -- DIFFERENT maps that
 * the cross-block edges must reconcile through gpr[]. rsp/rbp are never touched.
 *
 * THE FAULT, DETERMINISTIC AND BRANCH-FREE. rcx counts 50 -> 49 -> ... The store
 * base is r15 (a scratch buffer) on every iteration EXCEPT rcx==10, where
 * `cmpq;cmovneq` leaves it NULL, so the 41st iteration stores through NULL ->
 * SIGSEGV. NULL faults identically on native x86, the interpreter, and the JIT
 * (unlike a high uncommitted address, which ocerz commits on demand). Every value
 * printed is iteration-pure (a function of the constants and the iteration count),
 * so all three tiers fault at the same logical point with identical registers.
 *
 * DETERMINISM. Only iteration-pure values are printed: the accumulators, rcx
 * (== 10 at the fault), the fault trapno and address (0), the masked flags, and
 * the fault rip AS AN OFFSET from the fault_body symbol (both carry the load base,
 * so their difference is load-base-independent and pins the exact faulting
 * instruction -- the host_pc -> block -> guest-rip mapping evaluated at a host pc
 * reached only through the cross-block links). The scratch base r15 and rbp/rsp
 * are never printed.
 *
 * MUTATION TEST. Break the cross-block edge reconciliation and this three-way
 * equivalence fails while the interpreter (which never pins) still passes:
 *   - a body-entry shortcut on a cross-block edge: subset B runs with subset A's
 *     values -> the printed r9/r10/r12/r13/r14 go wrong, JIT != golden.
 *   - a dropped chain-tail spill: the target reloads stale gpr[] -> accumulators
 *     wrong, JIT != golden. interp stays == golden either way.
 * Bounded/self-terminating (the fault ends it), so it needs no async-stop arming
 * and cannot hang the differential.
 *
 * Trampoline/handler ABI and mcontext offsets are exactly fault_chain.c's.
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

/* Scratch store target for the non-faulting iterations. Machine-dependent
 * address, never printed. Referenced only from inline asm. */
g_u64 g_buf[16] __attribute__((used));

/* fault_body's runtime address, captured in main, for the load-base-independent
 * fault-rip offset. */
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
 * The cross-block linked loop. SETUP loads distinct constants and the counter,
 * then `jmp La` terminates the setup block so La starts a fresh block. La folds
 * subset A and ends in a data-dependent two-way Jcc to Lbe/Lbo (both other
 * blocks). Each arm folds subset B and jmps to Lmerge. Lmerge does the branch-free
 * NULL-store fault selector and the `jnz La` loop back-edge. Control never
 * returns: the handler rewrites the saved rip so sigreturn lands in recover().
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
    "    movabsq $0x3333333333333333, %r14\n"
    "    movq    $50, %rcx\n"                 /* loop counter: 50..down */
    "    leaq    _g_buf(%rip), %r15\n"        /* scratch store base (never printed) */
    "    jmp     La\n"                        /* terminator: La is a fresh block */
    /* ---- block A: subset {rax,rbx,rdx,rsi,rdi,r8}, 24 reuse-heavy insns ---- */
    "La:\n"
    "    addq %rcx,%rax\n"
    "    xorq %rax,%rbx\n"
    "    addq %rbx,%rdx\n"
    "    xorq %rdx,%rsi\n"
    "    addq %rsi,%rdi\n"
    "    xorq %rdi,%r8\n"
    "    addq %r8,%rax\n"
    "    xorq %rax,%rbx\n"
    "    addq %rbx,%rdx\n"
    "    xorq %rdx,%rsi\n"
    "    addq %rsi,%rdi\n"
    "    xorq %rdi,%r8\n"
    "    addq %r8,%rax\n"
    "    xorq %rax,%rbx\n"
    "    addq %rbx,%rdx\n"
    "    xorq %rdx,%rsi\n"
    "    addq %rsi,%rdi\n"
    "    xorq %rdi,%r8\n"
    "    addq %r8,%rax\n"
    "    xorq %rax,%rbx\n"
    "    addq %rbx,%rdx\n"
    "    xorq %rdx,%rsi\n"
    "    addq %rsi,%rdi\n"
    "    xorq %rdi,%r8\n"
    "    testq $1,%rax\n"
    "    jz   Lbe\n"                          /* two-way: taken Lbe, fall Lbo (both blocks) */
    /* ---- block Lbo (odd arm): subset {r9,r10,r12,r13,r14}, folded via sub ---- */
    "Lbo:\n"
    "    subq %rcx,%r9\n"
    "    xorq %r9,%r10\n"
    "    subq %r10,%r12\n"
    "    xorq %r12,%r13\n"
    "    subq %r13,%r14\n"
    "    xorq %r14,%r9\n"
    "    subq %r9,%r10\n"
    "    xorq %r10,%r12\n"
    "    subq %r12,%r13\n"
    "    xorq %r13,%r14\n"
    "    subq %r14,%r9\n"
    "    xorq %r9,%r10\n"
    "    subq %r10,%r12\n"
    "    xorq %r12,%r13\n"
    "    subq %r13,%r14\n"
    "    xorq %r14,%r9\n"
    "    subq %r9,%r10\n"
    "    xorq %r10,%r12\n"
    "    subq %r12,%r13\n"
    "    xorq %r13,%r14\n"
    "    subq %r14,%r9\n"
    "    xorq %r9,%r10\n"
    "    subq %r10,%r12\n"
    "    xorq %r12,%r13\n"
    "    jmp  Lmerge\n"
    /* ---- block Lbe (even arm): subset {r9,r10,r12,r13,r14}, folded via add ---- */
    "Lbe:\n"
    "    addq %rcx,%r9\n"
    "    xorq %r9,%r10\n"
    "    addq %r10,%r12\n"
    "    xorq %r12,%r13\n"
    "    addq %r13,%r14\n"
    "    xorq %r14,%r9\n"
    "    addq %r9,%r10\n"
    "    xorq %r10,%r12\n"
    "    addq %r12,%r13\n"
    "    xorq %r13,%r14\n"
    "    addq %r14,%r9\n"
    "    xorq %r9,%r10\n"
    "    addq %r10,%r12\n"
    "    xorq %r12,%r13\n"
    "    addq %r13,%r14\n"
    "    xorq %r14,%r9\n"
    "    addq %r9,%r10\n"
    "    xorq %r10,%r12\n"
    "    addq %r12,%r13\n"
    "    xorq %r13,%r14\n"
    "    addq %r14,%r9\n"
    "    xorq %r9,%r10\n"
    "    addq %r10,%r12\n"
    "    xorq %r12,%r13\n"
    "    jmp  Lmerge\n"
    /* ---- block Lmerge: branch-free fault selector + loop back-edge ---- */
    "Lmerge:\n"
    "    xorq %r11,%r11\n"                     /* r11 = 0 (NULL) */
    "    cmpq $10,%rcx\n"                      /* ZF=1 iff fault iteration */
    "    cmovneq %r15,%r11\n"                  /* other iters: r11 = buf (valid store) */
    "    movq %rax,(%r11)\n"                   /* FAULT here on the 41st iter (rcx==10) */
    "    decq %rcx\n"
    "    jnz  La\n"                            /* back-edge to A (loop header) */
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
    p("r14 ", *(g_u64 *)(mc + 128));
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

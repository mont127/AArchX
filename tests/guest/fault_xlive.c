/*
 * tests/guest/fault_xlive.c
 *
 * CROSS-SEAM FLAG-AT-FAULT gate for XLIVE (cross-block flag liveness across
 * LINKED seams).
 *
 * WHY IT EXISTS. The 31-binary PUSHF differential compares interp==jit rflags
 * only at points a guest instruction OBSERVES a flag. It CANNOT see a flag that
 * is read on the unpredicted control-flow of a SIGNAL HANDLER after a fault:
 * nothing in the guest program PUSHFs there. XLIVE relaxes a linked block A's
 * live_out from ALL to exactly what its successor B reads, which is sound ONLY
 * because the fault barrier (flags_live.h: any memory-touching op reads ALL)
 * forces A's last flag producer to stay eager whenever a trapping op sits in the
 * cross-seam window. This test makes that barrier LOAD-BEARING and observable:
 * it faults immediately after a cross-block seam and reads the faulting rflags
 * from the ucontext, so a dropped producer flag shows up as a masked-flags
 * mismatch against real x86 hardware.
 *
 * SHAPE. A two-block linked loop with SMALL (<24-insn) UNPINNED bodies, matching
 * br's geometry (NOT fault_link's 24-insn pinned bodies, which test the pin
 * seam, a different mechanism). The block reached via the BACK-EDGE is the one
 * that faults, because XLIVE only relaxes an edge A->B when B was compiled BEFORE
 * A (a forward edge to a not-yet-compiled successor keeps the ALL seed). In a
 * loop the header compiles first, so the tail's back-edge to it is the edge XLIVE
 * relaxes -- and that is where the producer/fault must sit:
 *   setup    -> leaq buf,%r11 (valid for the first store) -> jmp Lhdr
 *   Lhdr (header, compiled FIRST; 3 insns): `movq %rax,(%r11)` is its FIRST op,
 *        so nothing writes flags between the tail's producer and this fault; on
 *        the rcx==10 iteration r11 is NULL (set by the tail's selector on the
 *        prior pass) and the store SIGSEGVs. `decq %rcx; jnz Ltail` counts and
 *        continues (the store faults long before rcx hits 0).
 *   Ltail (compiled SECOND; 6 insns): the branch-free NULL selector ORDERED
 *        BEFORE the producer (movabsq const,%rsi; xorq %r11,%r11; cmpq $10,%rcx;
 *        cmovneq %r15,%r11 -- the cmp's flags are dead, overwritten by the
 *        producer and consumed only by the cmov), then the PRODUCER
 *        P = subq %rdx,%rsi (the LAST flag writer in Ltail), then `jmp Lhdr` --
 *        the BACK-EDGE to the already-compiled header, exactly the static seam
 *        XLIVE relaxes.
 * rsi is reset to a constant each iteration, so P's flags are IDENTICAL every
 * iteration: subq 0x8000000000000000,1 sets OF=1 (INT64_MIN - 1 overflows) with
 * ZF=SF=0. If XLIVE ever wrongly dropped P's OF (because the header "does not
 * read it"), the handler's `rflags & 0x8d5` would instead read the stale flags
 * left by the selector's `cmp $10,%rcx` (ZF=1,PF=1,OF=0 -> 0x44) -- a visible
 * divergence from the golden in the NON-CF bits OF and ZF.
 *
 * WHY IT IS GREEN AT HEAD AND UNDER CORRECT XLIVE. Lhdr's first op is a memory
 * store = a use=ALL fault barrier, so Lhdr's entry_live is ALL, so XLIVE seeds
 * the tail with ALL across the back-edge and P stays eager. P's OF is
 * materialized before the store faults, matching hardware. This is the
 * executable proof that the barrier -- not a new invariant -- is what keeps a
 * cross-seam-dead producer live at a fault.
 *
 * MUTATION TEST (STEP 2, proves teeth). Force insn_touches_memory->0 for the
 * store: Lhdr's entry_live collapses to CF (only the decq keeps CF live across
 * the jnz), so XLIVE seeds the tail with CF and P emits only CF, dropping OF. The
 * JIT run then reads the stale cmp flags (0x44) and DIVERGES from the golden,
 * while the interpreter (which always computes all six) still matches. That FAIL
 * is the proof the barrier is load-bearing. This test has teeth ONLY because the
 * relaxed edge is the tail's back-edge to the already-compiled header; a
 * forward-edge geometry would leave the ALL seed and the mutation would be
 * invisible.
 *
 * DETERMINISM. Every printed value is iteration-pure: the constant registers,
 * rcx (==10 at the fault), the constant-per-iteration rsi, trapno/faultvaddr
 * (0), the masked flags, and the fault rip AS AN OFFSET from fault_body (both
 * carry the load base, so their difference is load-base-independent). The
 * scratch base r15 and rbp/rsp are never printed. Bounded/self-terminating (the
 * fault ends it), so it needs no async-stop arming.
 *
 * WHY THE GOLDEN IS NOT A RAW ROSETTA RUN (the one line where Rosetta is wrong).
 * The golden's `rflags & 0x8d5` is 0x814 (CF=0,PF=1,AF=1,ZF=0,SF=0,OF=1 — the
 * subq producer's flags). That is the value a PRECISE x86 page-fault exception
 * frame contains on real silicon (independently confirmed by `subq; pushfq` in a
 * standalone probe). But running this binary under ROSETTA reports 0x44 on that
 * line — the cmp $10,rcx flags — because Rosetta is itself a lazy-flag JIT and,
 * exactly like the STEP-2 mutation of ocerz, DROPS the dead-deferred subq flags
 * across the `jmp Lb` block seam and never reconstructs them at the fault. Every
 * OTHER line (all registers, rcx, rsi, trapno, faultvaddr, riptoff) matches
 * Rosetta byte-for-byte, so the geometry is held to hardware; only this one flag
 * line exposes that ocerz is STRICTER than Rosetta at a cross-seam fault. Any
 * XLIVE test with teeth must observe a flag that is dead across the seam, and a
 * lazy translator always drops such a flag — so Rosetta structurally cannot be
 * the reference for this line. The golden is therefore ocerz interp's output
 * (== jit, the primary differential gate), whose 0x814 equals the pushfq-probed
 * real-hardware value. If the fault barrier is ever removed, ocerz's jit run
 * regresses this line to Rosetta's buggy 0x44 and fails the golden.
 *
 * Trampoline/handler ABI and mcontext offsets are exactly fault_link.c's.
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
 * The cross-block linked loop. SETUP loads constants (all printed registers get
 * deterministic values), the counter, the scratch base, and a VALID initial r11
 * (so the first header store does not fault), then `jmp Lhdr` starts the header
 * block. Lhdr is reached first, so it COMPILES FIRST; its faulting store is its
 * first op. Ltail (compiled second) runs the NULL selector + producer and jmps
 * BACK to the already-compiled header -- the relaxed seam. Control never returns:
 * the handler rewrites the saved rip so sigreturn lands in recover().
 */
__asm__(
    ".text\n"
    ".globl _fault_body\n"
    "_fault_body:\n"
    "    movabsq $0x0102030405060708, %rax\n"    /* store value (printed) */
    "    movabsq $0x1122334455667788, %rbx\n"    /* printed, unmodified */
    "    movq    $1, %rdx\n"                      /* producer subtrahend (printed) */
    "    movabsq $0x0f0f0f0f0f0f0f0f, %rdi\n"     /* printed, unmodified */
    "    movabsq $0x5555555555555555, %r8\n"      /* printed, unmodified */
    "    movabsq $0xaaaaaaaaaaaaaaaa, %r9\n"      /* printed, unmodified */
    "    movabsq $0x123456789abcdef0, %r10\n"     /* printed, unmodified */
    "    movabsq $0x1111111111111111, %r12\n"     /* printed, unmodified */
    "    movabsq $0x2222222222222222, %r13\n"     /* printed, unmodified */
    "    movabsq $0x3333333333333333, %r14\n"     /* printed, unmodified */
    "    movq    $50, %rcx\n"                      /* loop counter: 50..down */
    "    leaq    _g_buf(%rip), %r15\n"             /* scratch store base (never printed) */
    "    leaq    _g_buf(%rip), %r11\n"             /* VALID for the first header store */
    "    jmp     Lhdr\n"                           /* terminator: Lhdr is the header block (compiled FIRST) */
    /* ---- block Lhdr (header, compiled FIRST): barrier store (fault) + count (3 insns) ---- */
    "Lhdr:\n"
    "    movq %rax,(%r11)\n"                       /* FAULT here on the rcx==10 pass (r11 NULL from Ltail); FIRST op, no flag writer since producer */
    "    decq %rcx\n"
    "    jnz  Ltail\n"                             /* continue to the tail (fall-through path is unreached: fault ends the loop) */
    "    jmp  Ldone\n"
    /* ---- block Ltail (compiled SECOND): NULL selector, producer, back-edge (6 insns) ---- */
    "Ltail:\n"
    "    movabsq $0x8000000000000000, %rsi\n"     /* reset rsi so producer flags are iter-pure */
    "    xorq %r11,%r11\n"                         /* r11 = 0 (NULL) */
    "    cmpq $10,%rcx\n"                          /* ZF=1 iff next store faults (flags DEAD past the cmov; overwritten by subq) */
    "    cmovneq %r15,%r11\n"                      /* other passes: r11 = buf (valid store) */
    "    subq %rdx,%rsi\n"                         /* PRODUCER: rsi = INT64_MIN-1 -> OF=1,ZF=SF=0 (LAST flag writer in Ltail) */
    "    jmp  Lhdr\n"                              /* BACK-EDGE to the compiled header -> XLIVE relaxes this seam */
    "Ldone:\n"
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

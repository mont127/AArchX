/*
 * tests/guest/flags_live.c
 *
 * The gate for LAZY FLAG EMISSION (src/flags_live.c + the backward liveness
 * pass in src/jit.c:translate). tests/guest/flags.c already covers per-op flag
 * exactness with PUSHF, but it cannot see either of the two ways the liveness
 * pass can be wrong, because both are about flags being read somewhere the pass
 * did not look:
 *
 *   1. CROSS-BLOCK. The pass kills a flag write when a later write in the SAME
 *      block overwrites it first. A consumer in a SUCCESSOR block is invisible
 *      to it. The pass is sound only because it forces live-out of a block's
 *      last instruction to all six flags; if someone ever "optimizes" that rule
 *      away, a producer at the end of one block feeding a SETcc/ADC in the next
 *      silently reads a flag that was never computed. test_crossblock() is that
 *      shape: a JMP splits the block between producer and consumer.
 *
 *   2. THE FAULT BARRIER. A flag the pass proves dead is dead only for the
 *      instructions it can see. A FAULT delivers a guest signal, and the handler
 *      is unpredicted control flow that can observe rflags in the ucontext it
 *      was handed. src/flags_live.c therefore treats every memory-touching
 *      instruction as reading all six flags. test_faultbarrier() puts a live
 *      producer, a faulting store, and a would-be killer in ONE block and has
 *      the SIGSEGV handler read the saved rflags straight out of the mcontext.
 *      Without the barrier the pass kills the producer (the third instruction
 *      overwrites it) and the handler observes a stale value.
 *
 * Both tests fold the observed flags into the printed checksum, so the JIT-vs-
 * interpreter differential (tests/run_diff_test.sh) is what actually enforces
 * them: the expected VALUES are not spelled out here, only that both tiers must
 * agree, which is the same contract the rest of the corpus runs under.
 *
 * Verified to be a real gate rather than a test that cannot fail: with the
 * barrier removed from ocerz_flags_defuse, test_faultbarrier diverges.
 *
 * The mcontext layout (ucontext+48 -> mcontext, rip@144, rflags@152) matches
 * the frame src/syscall.c builds; signal_test.c documents the trampoline ABI.
 */
#include "gsys.h"

#define SYS_sigaction 46
#define SIGSEGV 11
#define SA_SIGINFO 0x0040

/* CF PF AF ZF SF OF — the six architectural arithmetic flags, masking out the
 * always-set fixed bit and IF/DF so the comparison is state-independent. */
#define ARITH_MASK 0x0cd5ULL

#define MC_OFF     48
#define MC_RIP     144
#define MC_RFLAGS  152

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

static volatile g_u64 g_capflags;   /* rflags the handler saw at the fault */
static volatile g_u64 g_resume;     /* where the handler sends the fault back */

static void segv_handler(int signo, void *info, void *uc)
{
    (void)signo;
    (void)info;
    g_u64 mc = *(volatile g_u64 *)((char *)uc + MC_OFF);
    /* THE OBSERVATION UNDER TEST: the flags a guest handler sees at a fault. */
    g_capflags = *(volatile g_u64 *)(mc + MC_RFLAGS) & ARITH_MASK;
    /* Resume past the faulting store instead of retrying it forever. */
    *(volatile g_u64 *)(mc + MC_RIP) = g_resume;
}

/* ---- 1. Cross-block producer -> consumer ------------------------------------
 * The CMP writes all six flags and is the last instruction of its block (the
 * JMP terminates it). The SETcc/ADC in the successor block read them. Only the
 * live-out=ALL rule makes those flags exist. */
static g_u64 test_crossblock(g_u64 a, g_u64 b)
{
    g_u64 z = 0, c = 0;
    __asm__ volatile(
        "cmpq %[b], %[a]\n\t"
        "jmp  1f\n"
        "1:\n\t"
        "setz %%cl\n\t"
        "movzbq %%cl, %[z]\n\t"
        "adcq $0, %[c]\n\t"
        : [z] "=&r"(z), [c] "+r"(c)
        : [a] "r"(a), [b] "r"(b)
        : "cc", "rcx");
    return z * 3 + c * 5;
}

/* ---- 2. Mid-block fault with a would-be-dead producer ------------------------
 * add  -> writes all six flags
 * mov  -> FAULTS (null store). The barrier must have materialized the add's
 *         flags by here, because the handler below reads them.
 * add  -> overwrites all six, which is exactly what would license killing the
 *         first add's flags if the faulting store were not a barrier.
 */
static g_u64 test_faultbarrier(g_u64 a, g_u64 b, g_u64 c, g_u64 d)
{
    g_u64 x = a, y = c;
    g_capflags = 0;
    g_resume = (g_u64)(void *)&&resume;
    __asm__ volatile(
        "addq %[b], %[x]\n\t"
        "xorq %%r11, %%r11\n\t"
        "movq %[x], (%%r11)\n\t"
        "addq %[d], %[y]\n\t"
        : [x] "+r"(x), [y] "+r"(y)
        : [b] "r"(b), [d] "r"(d)
        : "cc", "r11", "memory");
resume:
    return g_capflags;
}

int main(void)
{
    struct k_sigaction sa;
    sa.handler = (g_u64)(void *)segv_handler;
    sa.tramp = (g_u64)(void *)sig_tramp;
    sa.mask = 0;
    sa.flags = SA_SIGINFO;
    __asm__ volatile("syscall" : : "a"(0x2000000 | SYS_sigaction),
                     "D"(SIGSEGV), "S"(&sa), "d"(0) : "rcx", "r11", "memory");

    g_u64 sum = 0;

    /* Cross-block: sweep equal / greater / less / boundary operand pairs so the
     * consumed ZF and CF each take both values. */
    static const g_u64 pairs[][2] = {
        { 0, 0 }, { 1, 0 }, { 0, 1 }, { 5, 5 },
        { 0xffffffffffffffffULL, 1 }, { 1, 0xffffffffffffffffULL },
        { 0x8000000000000000ULL, 0x7fffffffffffffffULL },
        { 0x100000000ULL, 0xffffffffULL },
    };
    for (g_u64 i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
        sum = sum * 31 + test_crossblock(pairs[i][0], pairs[i][1]);

    /* Fault barrier: operand sets chosen so the first add's flags differ from
     * the second's (ZF/CF/SF/OF/AF/PF all take both values across the sweep),
     * which is what makes a stale capture observable. */
    static const g_u64 quads[][4] = {
        { 0, 0, 1, 1 },
        { 0xffffffffffffffffULL, 1, 5, 5 },
        { 0x7fffffffffffffffULL, 1, 0, 0 },
        { 0xfULL, 1, 0xffffffffffffffffULL, 1 },
        { 0x80ULL, 0x80ULL, 3, 4 },
    };
    for (g_u64 i = 0; i < sizeof(quads) / sizeof(quads[0]); i++)
        sum = sum * 31 + test_faultbarrier(quads[i][0], quads[i][1],
                                           quads[i][2], quads[i][3]);

    g_putu64(sum);
    return 0;
}

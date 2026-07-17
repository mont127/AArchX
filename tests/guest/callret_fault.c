/*
 * tests/guest/callret_fault.c
 *
 * Gate for the JIT's INLINED `call rel32` / `ret` (emit_call_ret, src/jit.c).
 *
 * Those two are terminators that now perform their stack access NATIVELY
 * instead of calling the interpreter. emit_call_ret's comments assert two
 * properties that nothing in the suite actually exercised:
 *
 *   (1) FAULT ATOMICITY. "a fault here must leave rsp untouched so the retry
 *       pushes to the same address" (CALL) and the mirror for RET's pop. A
 *       faulting x86 instruction is ABORTED -- no architectural effect -- and
 *       crash_handler (src/vm.c) rewinds rip so a handler that fixes the page
 *       and returns genuinely RE-EXECUTES it. This is popmem_test's class, but
 *       popmem_test faults on a MEMORY DESTINATION, so its rsp is untouched and
 *       it needs no alternate stack. call/ret fault ON THE STACK ITSELF, so
 *       they were never covered. If rsp committed before the faulting access,
 *       the retry pushes/pops one slot over and DOUBLE-adjusts rsp -- silent
 *       stack corruption, and exactly what the ordering comments guard.
 *
 *   (2) MISALIGNED rsp. emit_guest_{store,load}_ordered emit a runtime split:
 *       naturally-aligned takes STLR/LDAR, misaligned falls back to DMB+STR/LDR
 *       (LDAR/STLR alignment-fault, x86 does not). Every other guest test keeps
 *       rsp 16-byte aligned, so the misaligned arm of the call/ret stack access
 *       is otherwise NEVER executed.
 *
 * Both properties are checked against REAL HARDWARE: the golden is generated
 * from the native x86 run under Rosetta, so this holds ocerz to the machine,
 * not merely to itself. Trampoline ABI / k_sigaction layout are identical to
 * rip_test.c and popmem_test.c.
 *
 * GUARD GRANULARITY: the guarded region is 16K-aligned and 16K-sized, not 4K.
 * ocerz_protect (src/mem.c) rounds RESTRICTIVE protection changes INWARD to the
 * host's 16K page, so a 4K-granular PROT_NONE could legitimately protect
 * nothing under ocerz while faulting natively -- that would make this test
 * report a difference that is not the one it is hunting.
 *
 * Everything that matters is hand-written asm so no compiler choice can move
 * the call/ret off the inlined path and quietly stop testing anything.
 */
#include "gsys.h"

#define SYS_sigaction   46
#define SYS_sigaltstack 53
#define SYS_mprotect    74
#define SIGSEGV 11
#define SIGBUS  10
#define SA_SIGINFO 0x0040
#define SA_ONSTACK 0x0001

#define PROT_NONE  0x0
#define MAP_ANON    0x1000
#define MAP_PRIVATE 0x0002

/* Host page granularity, not the guest's 4K. See GUARD GRANULARITY above. */
#define PG 16384

struct k_sigaction {
    g_u64 handler;
    g_u64 tramp;
    g_u32 mask;
    g_u32 flags;
};

struct g_sigaltstack {
    g_u64 ss_sp;
    g_u64 ss_size;
    g_i64 ss_flags;   /* int + tail padding */
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

/* Page the handler unguards, and the fault tally. Both volatile: the handler
 * runs asynchronously with respect to the asm blocks below. */
static volatile g_u64 g_fix_lo;
static volatile g_u64 g_nfault;

static void handler(int signo, void *siginfo, void *ucontext)
{
    (void)signo; (void)siginfo; (void)ucontext;
    g_nfault++;
    /* Heal the page and return. The kernel (and ocerz's crash_handler, which
     * rewinds rip to the faulting instruction) then RE-EXECUTES the call/ret.
     * rip is deliberately NOT touched -- re-execution is the whole point. */
    g_syscall3(SYS(SYS_mprotect), (g_i64)g_fix_lo, PG, PROT_READ | PROT_WRITE);
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    static char altbuf[64 * 1024] __attribute__((aligned(16)));
    struct g_sigaltstack ss;
    ss.ss_sp = (g_u64)(void *)altbuf;
    ss.ss_size = sizeof altbuf;
    ss.ss_flags = 0;
    g_syscall3(SYS(SYS_sigaltstack), (g_i64)(g_u64)&ss, 0, 0);

    struct k_sigaction sa;
    sa.handler = (g_u64)&handler;
    sa.tramp = (g_u64)&sig_tramp;
    sa.mask = 0;
    sa.flags = SA_SIGINFO | SA_ONSTACK;
    g_syscall3(SYS(SYS_sigaction), SIGSEGV, (g_i64)(g_u64)&sa, 0);
    g_syscall3(SYS(SYS_sigaction), SIGBUS, (g_i64)(g_u64)&sa, 0);

    /* Four 16K pages, then align up so [lo, lo+PG) is a whole host page.
     * [lo] gets guarded; [hi = lo+PG] stays mapped. A stack pointer at hi makes
     * `call`'s push (rsp-8) land in lo and fault, while `ret`'s pop reads lo's
     * top cell. */
    char *base = (char *)sys_mmap(0, 4 * PG, PROT_READ | PROT_WRITE,
                                  MAP_ANON | MAP_PRIVATE, -1, 0);
    if ((g_i64)(g_u64)base <= 0) {
        g_puts("mmap FAILED\n");
        return 1;
    }
    g_u64 lo = (((g_u64)base) + PG - 1) & ~(g_u64)(PG - 1);
    g_u64 hi = lo + PG;
    g_fix_lo = lo;

    /* ================= 1. CALL whose push FAULTS ================= */
    /* rsp = hi, so the push targets hi-8, inside the guarded page. Fault ->
     * handler unguards -> retry. rsp must end at hi-8 and the pushed value must
     * be the return address. An rsp committed before the faulting store would
     * retry from hi-8 and land at hi-16. */
    g_nfault = 0;
    g_syscall3(SYS(SYS_mprotect), (g_i64)lo, PG, PROT_NONE);
    {
        g_u64 got_rsp = 0, got_val = 0, want_val = 0;
        __asm__ __volatile__(
            "movq %%rsp, %%r15\n"          /* save the real stack */
            "movq %[hi], %%rsp\n"
            "callq 1f\n"                   /* rel32 -> the inlined form */
            "1:\n"
            "movq %%rsp, %[grsp]\n"
            "movq (%%rsp), %[gval]\n"
            "leaq 1b(%%rip), %[wval]\n"
            "movq %%r15, %%rsp\n"
            : [grsp] "=&r"(got_rsp), [gval] "=&r"(got_val), [wval] "=&r"(want_val)
            : [hi] "r"(hi)
            : "r15", "memory");

        if (g_nfault != 1)
            g_puts("call push: fault count WRONG\n");
        else if (got_rsp != hi - 8)
            g_puts("call push: rsp WRONG\n");
        else if (got_val != want_val)
            g_puts("call push: retaddr WRONG\n");
        else
            g_puts("call push: rsp exact\n");
    }

    /* ================= 2. RET whose pop FAULTS ================= */
    /* Plant the return address at hi-8 FIRST, then guard the page. rsp = hi-8,
     * so `ret`'s load faults. After the retry rsp must be hi (one 8-byte pop),
     * not hi+8, which is what a pre-committed rsp would give. */
    g_nfault = 0;
    {
        g_u64 got_rsp = 0, landed = 0;
        __asm__ __volatile__(
            "leaq 2f(%%rip), %%rax\n"
            "movq %%rax, -8(%[hi])\n"      /* plant retaddr while still mapped */
            "movq %[lo], %%rdi\n"
            "movl %[pg], %%esi\n"
            "xorl %%edx, %%edx\n"          /* PROT_NONE */
            "movl $0x200004a, %%eax\n"     /* mprotect */
            "syscall\n"
            "movq %%rsp, %%r15\n"
            "leaq -8(%[hi]), %%rsp\n"
            "ret\n"                        /* nops==0 -> the inlined form */
            "2:\n"
            "movq %%rsp, %[grsp]\n"
            "movq $1, %[land]\n"
            "movq %%r15, %%rsp\n"
            : [grsp] "=&r"(got_rsp), [land] "=&r"(landed)
            : [hi] "r"(hi), [lo] "r"(lo), [pg] "i"(PG)
            : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "r15", "memory");

        if (!landed || g_nfault != 1)
            g_puts("ret pop: fault count WRONG\n");
        else if (got_rsp != hi)
            g_puts("ret pop: rsp WRONG\n");
        else
            g_puts("ret pop: rsp exact\n");
    }

    /* ================= 3. MISALIGNED rsp call/ret round trip ============= */
    /* Drives the DMB+STR / DMB+LDR fallback arm of emit_guest_{store,load}_
     * ordered, which the aligned-stack rest of the suite never reaches. k=0 is
     * the aligned control. A real call/ret PAIR: `callq 3f` jumps forward over
     * the return site to a callee that returns. */
    g_syscall3(SYS(SYS_mprotect), (g_i64)lo, PG, PROT_READ | PROT_WRITE);
    {
        int bad = 0;
        for (int k = 0; k < 8; k++) {
            g_u64 top = hi - 64 - (g_u64)k;   /* rsp misaligned by k */
            g_u64 in_rsp = 0, in_val = 0, want_val = 0, out_rsp = 0;
            __asm__ __volatile__(
                "movq %%rsp, %%r15\n"
                "movq %[top], %%rsp\n"
                "callq 3f\n"
                "5:\n"                        /* the return site */
                "movq %%rsp, %[orsp]\n"
                "jmp 4f\n"
                "3:\n"                        /* callee */
                "movq %%rsp, %[irsp]\n"
                "movq (%%rsp), %[ival]\n"
                "leaq 5b(%%rip), %[wval]\n"
                "ret\n"
                "4:\n"
                "movq %%r15, %%rsp\n"
                : [irsp] "=&r"(in_rsp), [ival] "=&r"(in_val),
                  [wval] "=&r"(want_val), [orsp] "=&r"(out_rsp)
                : [top] "r"(top)
                : "r15", "memory");
            /* inside the callee: rsp = top-8, [rsp] = return site.
             * after the ret:     rsp = top. */
            if (in_rsp != top - 8 || in_val != want_val || out_rsp != top)
                bad = 1;
        }
        if (bad)
            g_puts("misaligned call/ret: WRONG\n");
        else
            g_puts("misaligned call/ret: ok\n");
    }

    g_puts("done\n");
    return 0;
}

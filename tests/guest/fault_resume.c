/*
 * tests/guest/fault_resume.c
 *
 * The RESUME-AND-RE-EXECUTE gate. Every other fault test in this suite
 * (fault_regs, popmem_test, flags_live, callret_fault) observes state at the
 * fault and then ESCAPES: its handler rewrites the saved rip so sigreturn lands
 * in a recover() stub and the faulting instruction is never run again. That
 * leaves the single most load-bearing claim in crash_handler UNTESTED end to
 * end:
 *
 *    "crash_handler rewinds cpu->rip to the FAULTING instruction before
 *     delivering a guest SIGSEGV, so a handler that fixes the page and returns
 *     genuinely RE-EXECUTES the instruction." (src/vm.c, popmem_test.c header)
 *
 * popmem_test only checks that rsp is intact AT the fault; it never lets the pop
 * run. This test closes the loop: the handler FIXES the fault by pointing the
 * base register at a real buffer (a store straight into the delivered mcontext),
 * leaves rip ALONE, and returns. sigreturn (src/syscall.c sys_sigreturn) must
 * then (a) reload the handler-modified GPR into cpu->gpr, and (b) resume at the
 * rewound rip, so the faulting instruction executes a SECOND time and now
 * SUCCEEDS. The architectural result of that second execution -- the destination
 * register / the stored memory, AND the six arithmetic flags it produces -- is
 * compared against a golden generated on REAL x86_64 hardware (native, under
 * Rosetta), so ocerz is held to hardware, not merely to itself.
 *
 * Three distinct pieces of machinery are on the hook, and a break in any one is
 * visible only here:
 *
 *   1. rip REWIND EXACTNESS. The delivered frame's rip must be the faulting
 *      instruction itself. Under the JIT that address does not exist in cpu->rip
 *      (an inlined instruction never writes it) and must be recovered from the
 *      block side table (ocerz_jit_fault_rip). The handler asserts rip ==
 *      &<fault label>; a side table that names the wrong guest instruction, or a
 *      rewind that is off by one, redirects to abort_path and prints "RIP WRONG".
 *
 *   2. sigreturn GPR WRITE-BACK. The handler's only fix is a write to the base
 *      register slot in the mcontext. If sys_sigreturn ignored handler edits to
 *      the GPRs (restoring the pre-fault values), the re-executed instruction
 *      would fault at the SAME bad address again; the second-fault guard fires
 *      and prints "REFAULT".
 *
 *   3. FAULT ATOMICITY + LAZY-FLAG CORRECTNESS ACROSS RE-EXECUTION. The faulting
 *      op is an arithmetic op with a memory operand (a memory-source ADD, then a
 *      memory-destination ADD). On the aborted first run it must have committed
 *      NOTHING; on the successful second run it must produce exactly the flags
 *      real hardware does. The flags are captured with PUSHF immediately after
 *      and folded into the printed output, so the JIT's dead-flag elimination
 *      (src/flags_live.c) is forced to have materialized them correctly at an
 *      instruction that both faults AND is a live flag producer.
 *
 * Trampoline ABI and ucontext/mcontext offsets are the ones signal_test.c
 * documents against this host's _sigtramp: ucontext+48 -> mcontext, and within
 * mcontext rip@144, rflags@152, and the x86_64 thread-state GPRs from +16 in the
 * order rax,rbx,rcx,rdx,rdi,rsi,rbp,rsp,r8..r15 (so r11 is at +104).
 *
 * VERIFIED TO HAVE TEETH: with sys_sigreturn's GPR reload disabled the second
 * execution re-faults and this test prints "REFAULT"; with crash_handler's rip
 * rewind disabled (OCERZ_NO_RIP_REWIND=1) the frame names the instruction after
 * the fault and the handler prints "RIP WRONG". Both diverge from the golden.
 */
#include "gsys.h"

#define SYS_sigaction 46
#define SIGSEGV 11
#define SIGBUS 10
#define SA_SIGINFO 0x0040

/* CF PF AF ZF SF OF -- the six architecturally-defined ADD flags. Every one is
 * defined by x86 for ADD, so this line is native-comparable (unlike IMUL). */
#define ARITH_MASK 0x08d5ULL

#define MC_OFF     48
#define MC_TRAPNO  0
#define MC_VADDR   8
#define MC_R11     104
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

/* State shared with the two asm bodies and the handler. Non-static so the
 * module-level asm can reach them by their underscored labels. */
g_u64 g_bufA;          /* memory source for subtest A (preloaded) */
g_u64 g_bufB;          /* memory destination for subtest B (preloaded) */
g_u64 g_resA_rax;      /* rax after A's memory-source add re-executes */
g_u64 g_resA_flags;    /* flags A's add produced */
g_u64 g_resB_flags;    /* flags B's add produced */

/* Handler <-> main contract, all set by main before arming each faulting body. */
static volatile g_u64 g_fixptr;      /* value the handler writes into r11 */
static volatile g_u64 g_expect_rip;  /* the exact faulting-instruction address */
static volatile int   g_block_faults;/* per-body fault count, reset before each */

/* Subtest A: memory-SOURCE add. rax = 0xffff...ff, r11 = 0 (bad). The add faults
 * on the LOAD; the handler points r11 at g_bufA (= 2). On re-execute rax becomes
 * 1 with CF and AF set. PUSHF captures the flags the add actually produced. */
__asm__(
    ".text\n"
    ".globl _fault_read\n"
    "_fault_read:\n"
    "    movabsq $0xffffffffffffffff, %rax\n"
    "    xorq %r11, %r11\n"
    ".globl _fr_fault\n"
    "_fr_fault:\n"
    "    addq (%r11), %rax\n"          /* FAULT (load from 0); re-exec adds g_bufA */
    "    pushfq\n"
    "    popq %rdx\n"
    "    movq %rax, _g_resA_rax(%rip)\n"
    "    movq %rdx, _g_resA_flags(%rip)\n"
    "    ret\n");

/* Subtest B: memory-DESTINATION add. rdx = 1, r11 = 0 (bad). The add faults on
 * the STORE; the handler points r11 at g_bufB (= 0xffffffff). On re-execute the
 * store commits g_bufB = 0x1_00000000 with AF set, PF set, CF clear. Aborted
 * first run must have left g_bufB untouched. */
__asm__(
    ".text\n"
    ".globl _fault_write\n"
    "_fault_write:\n"
    "    movq $0x0000000000000001, %rdx\n"
    "    xorq %r11, %r11\n"
    ".globl _fw_fault\n"
    "_fw_fault:\n"
    "    addq %rdx, (%r11)\n"          /* FAULT (store to 0); re-exec adds into g_bufB */
    "    pushfq\n"
    "    popq %rax\n"
    "    movq %rax, _g_resB_flags(%rip)\n"
    "    ret\n");

extern void fault_read(void);
extern void fault_write(void);
extern char fr_fault[];
extern char fw_fault[];

/* Reached only on a broken build: the fault could not be fixed by a single GPR
 * edit + resume. Printing a distinct line makes the divergence from the golden
 * unambiguous instead of hanging in the handler's re-delivery loop. */
static void abort_path(void)
{
    g_puts("ABORTED\n");
    sys_exit(0);
}

static void handler(int signo, void *siginfo, void *ucontext)
{
    (void)signo; (void)siginfo;
    g_u64 uc = (g_u64)ucontext;
    g_u64 mc = *(volatile g_u64 *)(uc + MC_OFF);
    g_u64 vaddr = *(volatile g_u64 *)(mc + MC_VADDR);
    g_u64 rip = *(volatile g_u64 *)(mc + MC_RIP);

    /* Teeth #1: the frame must name the exact faulting instruction. */
    if (rip != g_expect_rip) {
        g_puts("RIP WRONG\n");
        *(volatile g_u64 *)(mc + MC_RIP) = (g_u64)(void *)&abort_path;
        return;
    }
    /* Teeth #2: a second fault for the same body means the GPR fix did not take
     * (sigreturn dropped the handler's edit) -- the re-executed op hit the bad
     * address again. Do not spin; redirect to abort_path. */
    if (vaddr != 0 || g_block_faults != 0) {
        g_puts("REFAULT\n");
        *(volatile g_u64 *)(mc + MC_RIP) = (g_u64)(void *)&abort_path;
        return;
    }
    g_block_faults = 1;
    /* THE FIX: repoint the base register at a real buffer, and leave rip ALONE so
     * the faulting instruction re-executes. */
    *(volatile g_u64 *)(mc + MC_R11) = g_fixptr;
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

    /* ---- Subtest A: memory-source add re-executes after the load is fixed. */
    g_bufA = 0x0000000000000002ULL;
    g_fixptr = (g_u64)(void *)&g_bufA;
    g_expect_rip = (g_u64)(void *)fr_fault;
    g_block_faults = 0;
    fault_read();
    g_puts("A rax   "); g_puthex64(g_resA_rax);
    g_puts("A flags "); g_puthex64(g_resA_flags & ARITH_MASK);

    /* ---- Subtest B: memory-destination add re-executes after the store is
     * fixed; the stored value proves the aborted first run committed nothing. */
    g_bufB = 0x00000000ffffffffULL;
    g_fixptr = (g_u64)(void *)&g_bufB;
    g_expect_rip = (g_u64)(void *)fw_fault;
    g_block_faults = 0;
    fault_write();
    g_puts("B mem   "); g_puthex64(g_bufB);
    g_puts("B flags "); g_puthex64(g_resB_flags & ARITH_MASK);

    g_puts("done\n");
    return 0;
}

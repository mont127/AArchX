/*
 * Call continuations laid out at bl+4 (src/jit.c, "Continuation placement").
 * Every call here is a direct call to a callee with a loop, which is never
 * spliced, so each block that ends in one gets a reservation after its bl and
 * the block at the return address is translated into it.  Five shapes:
 *
 *   storm   - the continuation holds a forward branch whose direction changes
 *             every 2048 iterations, from four call sites, so the copies are
 *             probed, flipped, retired and translated into their reservations
 *             again while the callers keep returning into them;
 *   big     - a continuation far longer than any reservation is sized for,
 *             which has to fall back to an ordinary block;
 *   fault   - a load inside a placed continuation faults, and the handler must
 *             see the faulting instruction's rip and the registers it had,
 *             then resume at it;
 *   smc     - generated code whose continuation, then whose call, is
 *             rewritten in place after it has been placed;
 *   loop    - a continuation that is itself a loop, entered once per call.
 *
 * The golden is the native run's.  Under OCERZ_PERFSTAT=1 the CONT line shows
 * the placements, the overflow and the re-placements this exercises.
 */
#include "gsys.h"

#define SYS_sigaction 46
#define SIGSEGV 11
#define SIGBUS 10
#define SA_SIGINFO 0x0040
#define MC_OFF 48
#define MC_R11 104
#define MC_RIP 144

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

/* work(n): a loop, so never spliced; returns sum of (i ^ n) * 3 + 1 for i < n&15. */
__asm__(
    ".text\n"
    ".p2align 4\n"
    "_work:\n"
    "    movq  %rdi, %rcx\n"
    "    andq  $15, %rcx\n"
    "    xorl  %eax, %eax\n"
    "    testq %rcx, %rcx\n"
    "    jz    2f\n"
    "1:  leaq  -1(%rcx), %rdx\n"
    "    xorq  %rdi, %rdx\n"
    "    leaq  1(%rdx,%rdx,2), %rdx\n"
    "    addq  %rdx, %rax\n"
    "    decq  %rcx\n"
    "    jnz   1b\n"
    "2:  ret\n");

/*
 * storm(iters, k): four call sites; after each, a forward branch on bit k of
 * the iteration count (k = 11: phases of 2048), taken in one phase and not in
 * the next.  The not-taken side stores, so it stays a branch.
 */
volatile g_u64 sink[16];
__asm__(
    ".text\n"
    ".p2align 4\n"
    "_storm:\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    movq  %rdi, %r12\n"
    "    movq  %rsi, %r14\n"
    "    xorl  %r13d, %r13d\n"
    "    xorl  %ebx, %ebx\n"
    "1:  movq  %rbx, %rdi\n"
    "    callq _work\n"
    "    btq   %r14, %rbx\n"
    "    jc    2f\n"
    "    leaq  (%rax,%rax,2), %rax\n"
    "    addq  %rax, %r13\n"
    "    movq  %r13, _sink(%rip)\n"
    "2:  xorq  %rbx, %r13\n"
    "    leaq  3(%rbx), %rdi\n"
    "    callq _work\n"
    "    btq   %r14, %rbx\n"
    "    jnc   3f\n"
    "    addq  %rax, %r13\n"
    "    movq  %r13, _sink+8(%rip)\n"
    "3:  rolq  $7, %r13\n"
    "    leaq  5(%rbx), %rdi\n"
    "    callq _work\n"
    "    testq $0x1800, %rbx\n"
    "    jz    4f\n"
    "    subq  %rax, %r13\n"
    "    movq  %r13, _sink+16(%rip)\n"
    "4:  addq  %rbx, %r13\n"
    "    leaq  9(%rbx), %rdi\n"
    "    callq _work\n"
    "    btq   %r14, %rbx\n"
    "    jc    5f\n"
    "    imulq $5, %rax, %rax\n"
    "    xorq  %rax, %r13\n"
    "    movq  %r13, _sink+24(%rip)\n"
    "5:  incq  %rbx\n"
    "    cmpq  %r12, %rbx\n"
    "    jb    1b\n"
    "    movq  %r13, %rax\n"
    "    popq  %r14\n"
    "    popq  %r13\n"
    "    popq  %r12\n"
    "    popq  %rbx\n"
    "    ret\n");
extern g_u64 storm(g_u64 iters, g_u64 k);

/* big(x): a call followed by a straight run of 240 instructions. */
volatile g_u64 bigmem[4] = { 1, 3, 0, 0 };
__asm__(
    ".text\n"
    ".p2align 4\n"
    "_big:\n"
    "    pushq %rbx\n"
    "    movq  %rdi, %rbx\n"
    "    callq _work\n"
    "    leaq  _bigmem(%rip), %rsi\n"
    "    .rept 60\n"
    "    addq  %rax, (%rsi)\n"
    "    movq  8(%rsi), %rcx\n"
    "    rolq  %cl, %rax\n"
    "    xaddq %rcx, 8(%rsi)\n"
    "    .endr\n"
    "    movq  (%rsi), %rax\n"
    "    addq  8(%rsi), %rax\n"
    "    popq  %rbx\n"
    "    ret\n");
extern g_u64 big(g_u64 x);

/* fault_cont(p): the load through p after the call faults when p is NULL. */
g_u64 fc_fix;
static volatile g_u64 fc_expect_rip;
static volatile int fc_faults;
__asm__(
    ".text\n"
    ".p2align 4\n"
    "_fault_cont:\n"
    "    pushq %rbx\n"
    "    movq  %rdi, %rbx\n"
    "    movl  $6, %edi\n"
    "    callq _work\n"
    "    movq  %rbx, %r11\n"
    "    addq  $0x40, %rax\n"
    ".globl _fc_fault\n"
    "_fc_fault:\n"
    "    addq  (%r11), %rax\n"
    "    rolq  $5, %rax\n"
    "    popq  %rbx\n"
    "    ret\n");
extern g_u64 fault_cont(g_u64 *p);
extern char fc_fault[];

static void handler(int signo, void *siginfo, void *ucontext)
{
    (void)signo; (void)siginfo;
    g_u64 mc = *(volatile g_u64 *)((g_u64)ucontext + MC_OFF);
    g_u64 rip = *(volatile g_u64 *)(mc + MC_RIP);
    fc_faults++;
    if (rip != fc_expect_rip || *(volatile g_u64 *)(mc + MC_R11) != 0) {
        g_puts("fault: wrong rip or r11\n");
        sys_exit(3);
    }
    *(volatile g_u64 *)(mc + MC_R11) = (g_u64)&fc_fix;
}

/* loop_cont(n): the continuation is a loop of its own. */
__asm__(
    ".text\n"
    ".p2align 4\n"
    "_loop_cont:\n"
    "    pushq %rbx\n"
    "    movq  %rdi, %rbx\n"
    "    callq _work\n"
    "1:  addq  %rbx, %rax\n"
    "    rorq  $3, %rax\n"
    "    decq  %rbx\n"
    "    jnz   1b\n"
    "    popq  %rbx\n"
    "    ret\n");
extern g_u64 loop_cont(g_u64 n);

static void emit32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/*
 * caller at code:      call callee ; add eax, imm32 ; ret
 * callee at code+64:   mov ecx, 3 ; 1: dec ecx ; jnz 1b ; mov eax, imm32 ; ret
 */
static void gen(unsigned char *code, unsigned cont_imm, unsigned callee_imm)
{
    code[0] = 0xe8; emit32(code + 1, 64 - 5);
    code[5] = 0x05; emit32(code + 6, cont_imm);
    code[10] = 0xc3;
    unsigned char *c = code + 64;
    c[0] = 0xb9; emit32(c + 1, 3);
    c[5] = 0xff; c[6] = 0xc9;
    c[7] = 0x75; c[8] = 0xfc;
    c[9] = 0xb8; emit32(c + 10, callee_imm);
    c[14] = 0xc3;
}
typedef unsigned (*fn_t)(void);

static g_u64 smc_run(fn_t f, int n)
{
    g_u64 s = 0;
    for (int i = 0; i < n; i++) s += f();
    return s;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    g_puts("storm ");
    g_putu64(storm(40000, 11));
    g_puts("storm short phases ");
    g_putu64(storm(20000, 6));
    g_puts("storm again ");
    g_putu64(storm(40000, 11));

    g_u64 bs = 0;
    for (g_u64 i = 0; i < 6000; i++) bs += big(i);
    g_puts("big ");
    g_putu64(bs);

    struct k_sigaction sa;
    sa.handler = (g_u64)&handler;
    sa.tramp = (g_u64)&sig_tramp;
    sa.mask = 0;
    sa.flags = SA_SIGINFO;
    g_syscall3(SYS(SYS_sigaction), SIGSEGV, (g_i64)(g_u64)&sa, 0);
    g_syscall3(SYS(SYS_sigaction), SIGBUS, (g_i64)(g_u64)&sa, 0);
    fc_expect_rip = (g_u64)(void *)fc_fault;
    g_u64 cell = 1000, fs = 0;
    fc_fix = 77;
    for (int i = 0; i < 6000; i++) fs += fault_cont(&cell);
    for (int i = 0; i < 3; i++) fs += fault_cont((g_u64 *)0);
    for (int i = 0; i < 6000; i++) fs += fault_cont(&cell);
    g_puts("fault ");
    g_putu64(fs);
    g_puts("faults ");
    g_putu64((g_u64)fc_faults);

    unsigned char *code = (unsigned char *)sys_mmap(0, 4096, 7, 0x1002, -1, 0);
    if ((long)code < 0 && (long)code > -4096) { g_puts("mmap failed\n"); return 1; }
    fn_t f = (fn_t)(void *)code;
    gen(code, 10, 1000);
    g_puts("smc ");
    g_putu64(smc_run(f, 6000));
    emit32(code + 6, 20);
    g_puts("smc continuation rewritten ");
    g_putu64(smc_run(f, 6000));
    emit32(code + 64 + 10, 2000);
    g_puts("smc callee rewritten ");
    g_putu64(smc_run(f, 6000));
    code[32] = 0xe8; emit32(code + 33, 64 - 37);
    code[37] = 0x2d; emit32(code + 38, 5);
    code[42] = 0xc3;
    code[0] = 0xe9; emit32(code + 1, 32 - 5);
    g_puts("smc call rewritten ");
    g_putu64(smc_run(f, 6000));

    g_u64 ls = 0;
    for (g_u64 i = 1; i < 6000; i++) ls += loop_cont(i);
    g_puts("loop ");
    g_putu64(ls);
    return 0;
}

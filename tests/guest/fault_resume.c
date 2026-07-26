/* Resume and re-execute after a fault. */
#include "gsys.h"

#define SYS_sigaction 46
#define SIGSEGV 11
#define SIGBUS 10
#define SA_SIGINFO 0x0040

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

g_u64 g_bufA;
g_u64 g_bufB;
g_u64 g_resA_rax;
g_u64 g_resA_flags;
g_u64 g_resB_flags;

static volatile g_u64 g_fixptr;
static volatile g_u64 g_expect_rip;
static volatile int   g_block_faults;

__asm__(
    ".text\n"
    ".globl _fault_read\n"
    "_fault_read:\n"
    "    movabsq $0xffffffffffffffff, %rax\n"
    "    xorq %r11, %r11\n"
    ".globl _fr_fault\n"
    "_fr_fault:\n"
    "    addq (%r11), %rax\n"
    "    pushfq\n"
    "    popq %rdx\n"
    "    movq %rax, _g_resA_rax(%rip)\n"
    "    movq %rdx, _g_resA_flags(%rip)\n"
    "    ret\n");

__asm__(
    ".text\n"
    ".globl _fault_write\n"
    "_fault_write:\n"
    "    movq $0x0000000000000001, %rdx\n"
    "    xorq %r11, %r11\n"
    ".globl _fw_fault\n"
    "_fw_fault:\n"
    "    addq %rdx, (%r11)\n"
    "    pushfq\n"
    "    popq %rax\n"
    "    movq %rax, _g_resB_flags(%rip)\n"
    "    ret\n");

extern void fault_read(void);
extern void fault_write(void);
extern char fr_fault[];
extern char fw_fault[];

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

    if (rip != g_expect_rip) {
        g_puts("RIP WRONG\n");
        *(volatile g_u64 *)(mc + MC_RIP) = (g_u64)(void *)&abort_path;
        return;
    }

    if (vaddr != 0 || g_block_faults != 0) {
        g_puts("REFAULT\n");
        *(volatile g_u64 *)(mc + MC_RIP) = (g_u64)(void *)&abort_path;
        return;
    }
    g_block_faults = 1;

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

    g_bufA = 0x0000000000000002ULL;
    g_fixptr = (g_u64)(void *)&g_bufA;
    g_expect_rip = (g_u64)(void *)fr_fault;
    g_block_faults = 0;
    fault_read();
    g_puts("A rax   "); g_puthex64(g_resA_rax);
    g_puts("A flags "); g_puthex64(g_resA_flags & ARITH_MASK);

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

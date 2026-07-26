/* Gate for the JIT's inlined call rel32 and ret. */
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
    g_i64 ss_flags;
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

static volatile g_u64 g_fix_lo;
static volatile g_u64 g_nfault;

static void handler(int signo, void *siginfo, void *ucontext)
{
    (void)signo; (void)siginfo; (void)ucontext;
    g_nfault++;

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

    char *base = (char *)sys_mmap(0, 4 * PG, PROT_READ | PROT_WRITE,
                                  MAP_ANON | MAP_PRIVATE, -1, 0);
    if ((g_i64)(g_u64)base <= 0) {
        g_puts("mmap FAILED\n");
        return 1;
    }
    g_u64 lo = (((g_u64)base) + PG - 1) & ~(g_u64)(PG - 1);
    g_u64 hi = lo + PG;
    g_fix_lo = lo;

    g_nfault = 0;
    g_syscall3(SYS(SYS_mprotect), (g_i64)lo, PG, PROT_NONE);
    {
        g_u64 got_rsp = 0, got_val = 0, want_val = 0;
        __asm__ __volatile__(
            "movq %%rsp, %%r15\n"
            "movq %[hi], %%rsp\n"
            "callq 1f\n"
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

    g_nfault = 0;
    {
        g_u64 got_rsp = 0, landed = 0;
        __asm__ __volatile__(
            "leaq 2f(%%rip), %%rax\n"
            "movq %%rax, -8(%[hi])\n"
            "movq %[lo], %%rdi\n"
            "movl %[pg], %%esi\n"
            "xorl %%edx, %%edx\n"
            "movl $0x200004a, %%eax\n"
            "syscall\n"
            "movq %%rsp, %%r15\n"
            "leaq -8(%[hi]), %%rsp\n"
            "ret\n"
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

    g_syscall3(SYS(SYS_mprotect), (g_i64)lo, PG, PROT_READ | PROT_WRITE);
    {
        int bad = 0;
        for (int k = 0; k < 8; k++) {
            g_u64 top = hi - 64 - (g_u64)k;
            g_u64 in_rsp = 0, in_val = 0, want_val = 0, out_rsp = 0;
            __asm__ __volatile__(
                "movq %%rsp, %%r15\n"
                "movq %[top], %%rsp\n"
                "callq 3f\n"
                "5:\n"
                "movq %%rsp, %[orsp]\n"
                "jmp 4f\n"
                "3:\n"
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

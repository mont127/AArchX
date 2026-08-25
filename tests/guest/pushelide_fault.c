/* Gate for the elided return-address push: a fault INSIDE a spliced leaf
 * frame must find the slot repaired (recovery writes the address back) and
 * unwind exactly. */
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
    "    ud2\n"
    /* the leaf the JIT splices: pure register frame, elidable */
    ".globl _pe_leaf\n"
    "_pe_leaf:\n"
    "    pushq %rbp\n"
    "    movq  %rsp, %rbp\n"
    "    nop\n"
    "    popq  %rbp\n"
    "    ret\n");

extern void sig_tramp(void);
extern void pe_leaf(void);

static volatile g_u64 g_fix_lo;
static volatile g_u64 g_nfault;
static volatile g_u64 g_slot_addr;
static volatile g_u64 g_slot_seen;

static void handler(int signo, void *siginfo, void *ucontext)
{
    (void)signo; (void)siginfo; (void)ucontext;
    g_nfault++;
    g_slot_seen = *(volatile g_u64 *)g_slot_addr;   /* repaired retaddr? */
    g_syscall3(SYS(SYS_mprotect), (g_i64)g_fix_lo, PG, PROT_READ | PROT_WRITE);
}

int main(void)
{
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
    g_slot_addr = hi;

    /* pass 0 warms the block with a harmless stack; pass 1 puts the call
     * push on the last mapped slot so the leaf's own push faults */
    for (int pass = 0; pass < 2; pass++) {
        g_u64 top = pass == 0 ? hi + 2048 : hi + 8;
        if (pass == 1) {
            g_nfault = 0;
            g_slot_seen = 0;
            g_syscall3(SYS(SYS_mprotect), (g_i64)lo, PG, PROT_NONE);
        }
        g_u64 out_rsp = 0, want = 0;
        __asm__ __volatile__(
            "movq %%rsp, %%r15\n"
            "movq %[top], %%rsp\n"
            "callq _pe_leaf\n"
            "1:\n"
            "movq %%rsp, %[orsp]\n"
            "leaq 1b(%%rip), %[want]\n"
            "movq %%r15, %%rsp\n"
            : [orsp] "=&r"(out_rsp), [want] "=&r"(want)
            : [top] "r"(top)
            : "r15", "memory");
        if (pass == 1) {
            if (g_nfault != 1)
                g_puts("frame fault: count WRONG\n");
            else
                g_puts("frame fault: one fault\n");
            if (out_rsp != top)
                g_puts("frame fault: rsp WRONG\n");
            else
                g_puts("frame fault: rsp exact\n");
            /* the handler observed the slot content mid-frame: it must be
             * the return address (the instruction after the call) */
            if (g_slot_seen != want)
                g_puts("frame fault: slot WRONG\n");
            else
                g_puts("frame fault: slot repaired\n");
        }
    }
    g_puts("done\n");
    return 0;
}

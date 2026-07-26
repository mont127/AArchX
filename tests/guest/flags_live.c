/* Gate for lazy flag emission and the backward liveness pass. */
#include "gsys.h"

#define SYS_sigaction 46
#define SIGSEGV 11
#define SA_SIGINFO 0x0040

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

static volatile g_u64 g_capflags;
static volatile g_u64 g_resume;

static void segv_handler(int signo, void *info, void *uc)
{
    (void)signo;
    (void)info;
    g_u64 mc = *(volatile g_u64 *)((char *)uc + MC_OFF);

    g_capflags = *(volatile g_u64 *)(mc + MC_RFLAGS) & ARITH_MASK;

    *(volatile g_u64 *)(mc + MC_RIP) = g_resume;
}

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

    static const g_u64 pairs[][2] = {
        { 0, 0 }, { 1, 0 }, { 0, 1 }, { 5, 5 },
        { 0xffffffffffffffffULL, 1 }, { 1, 0xffffffffffffffffULL },
        { 0x8000000000000000ULL, 0x7fffffffffffffffULL },
        { 0x100000000ULL, 0xffffffffULL },
    };
    for (g_u64 i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
        sum = sum * 31 + test_crossblock(pairs[i][0], pairs[i][1]);

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

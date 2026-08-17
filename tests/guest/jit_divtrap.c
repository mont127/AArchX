/* div/idiv trap paths after an elided rdx preparation (xor edx,edx / cqo):
 * the SIGFPE handler must see the real rdx (0 / sign extension) and the
 * fault rip at the div; execution resumes past it. */
#include "gsys.h"

#define SYS_sigaction 46
#define SIGFPE 8
#define SA_SIGINFO 0x0040

struct k_sigaction { g_u64 handler; g_u64 tramp; g_u32 mask; g_u32 flags; };

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

static volatile g_u64 seen_rdx, seen_rax, seen_rip, nfault;
static void g_puthex64_nonl(g_u64 n)
{
    static const char hexd[] = "0123456789abcdef";
    char buf[18]; buf[0] = '0'; buf[1] = 'x';
    for (int k = 0; k < 16; k++) buf[2 + k] = hexd[(n >> ((15 - k) * 4)) & 0xf];
    sys_write(1, buf, sizeof(buf));
}
static volatile g_u64 resume_at;

static void handler(int signo, void *siginfo, void *ucontext)
{
    (void)signo; (void)siginfo;
    g_u64 uc = (g_u64)ucontext;
    g_u64 mc = *(g_u64 *)(uc + 48);
    /* x86_64 mcontext: __ss starts at +16; rax=+16, rdx=+40 (rax,rbx,rcx,rdx), rip=+144 */
    seen_rax = *(g_u64 *)(mc + 16 + 0);
    seen_rdx = *(g_u64 *)(mc + 16 + 24);
    seen_rip = *(g_u64 *)(mc + 144);
    nfault++;
    *(g_u64 *)(mc + 144) = resume_at;
}

static void report(const char *tag, g_u64 rip_expect)
{
    g_puts(tag); g_puts(": faults="); g_putu64_nonl(nfault);
    g_puts(" rax="); g_puthex64_nonl(seen_rax); g_puts(" rdx="); g_puthex64_nonl(seen_rdx);
    g_puts(seen_rip == rip_expect ? " rip=ok\n" : " rip=BAD\n");
}
static void g_puti64_nonl(g_i64 v) { if (v < 0) { g_puts("-"); g_putu64_nonl((g_u64)(-v)); } else g_putu64_nonl((g_u64)v); }

int main(void)
{
    struct k_sigaction sa;
    sa.handler = (g_u64)&handler; sa.tramp = (g_u64)&sig_tramp; sa.mask = 0; sa.flags = SA_SIGINFO;
    g_syscall3(SYS(SYS_sigaction), SIGFPE, (g_i64)(g_u64)&sa, 0);

    volatile g_u64 zero = 0, m1 = (g_u64)-1, big = 0x8000000000000000ULL, v = 12345;
    g_u64 q = 0, r = 0, ripv = 0, resume = 0;

    /* 1. xor edx,edx ; div (zero) -> #DE, rdx must read 0 in the handler */
    nfault = 0; seen_rdx = 0x1111;
    __asm__ volatile(
        "movq $0xdeadbeefcafef00d, %%rdx\n\t"     /* garbage that the elided xor must hide */
        "leaq 1f(%%rip), %[res]\n\t"
        "movq %[res], %[ra]\n\t"
        "movq %[vv], %%rax\n\t"
        "movq %[z], %%rcx\n\t"
        "xorl %%edx, %%edx\n\t"
        "leaq 0f(%%rip), %[rip]\n\t"
        "0: divq %%rcx\n\t"
        "1:\n\t"
        "movq %%rax, %[q]\n\t"
        "movq %%rdx, %[r]\n\t"
        : [q] "=&r"(q), [r] "=&r"(r), [rip] "=&r"(ripv), [res] "=&r"(resume), [ra] "=m"(resume_at)
        : [vv] "r"(v), [z] "r"(zero)
        : "rax", "rcx", "rdx", "cc", "memory");
    report("div0", ripv);

    /* 2. cqo ; idiv -1 with rax = INT_MIN -> #DE, rdx must read sext(rax) = -1 */
    nfault = 0; seen_rdx = 0x2222;
    __asm__ volatile(
        "movq $0x0123456789abcdef, %%rdx\n\t"
        "leaq 1f(%%rip), %[res]\n\t"
        "movq %[res], %[ra]\n\t"
        "movq %[bg], %%rax\n\t"
        "movq %[mm], %%rcx\n\t"
        "cqto\n\t"
        "leaq 0f(%%rip), %[rip]\n\t"
        "0: idivq %%rcx\n\t"
        "1:\n\t"
        "movq %%rax, %[q]\n\t"
        "movq %%rdx, %[r]\n\t"
        : [q] "=&r"(q), [r] "=&r"(r), [rip] "=&r"(ripv), [res] "=&r"(resume), [ra] "=m"(resume_at)
        : [bg] "r"(big), [mm] "r"(m1)
        : "rax", "rcx", "rdx", "cc", "memory");
    report("idivm1", ripv);

    /* 3. cdq ; idivl -1 with eax = INT_MIN (32-bit) */
    nfault = 0; seen_rdx = 0x3333;
    __asm__ volatile(
        "movq $0x0123456789abcdef, %%rdx\n\t"
        "leaq 1f(%%rip), %[res]\n\t"
        "movq %[res], %[ra]\n\t"
        "movl $0x80000000, %%eax\n\t"
        "movl $-1, %%ecx\n\t"
        "cltd\n\t"
        "leaq 0f(%%rip), %[rip]\n\t"
        "0: idivl %%ecx\n\t"
        "1:\n\t"
        "movq %%rax, %[q]\n\t"
        "movq %%rdx, %[r]\n\t"
        : [q] "=&r"(q), [r] "=&r"(r), [rip] "=&r"(ripv), [res] "=&r"(resume), [ra] "=m"(resume_at)
        :
        : "rax", "rcx", "rdx", "cc", "memory");
    report("idivl_m1", ripv);

    /* 4. the non-trapping neighbours still compute exactly */
    nfault = 0;
    __asm__ volatile(
        "movq $0xdeadbeefcafef00d, %%rdx\n\t"
        "movq %[vv], %%rax\n\t"
        "movq $7, %%rcx\n\t"
        "xorl %%edx, %%edx\n\t"
        "divq %%rcx\n\t"
        "movq %%rax, %[q]\n\t"
        "movq %%rdx, %[r]\n\t"
        : [q] "=&r"(q), [r] "=&r"(r) : [vv] "r"(v) : "rax", "rcx", "rdx", "cc", "memory");
    g_puts("div7: q="); g_putu64_nonl(q); g_puts(" r="); g_putu64_nonl(r); g_puts(" faults="); g_putu64(nfault);
    __asm__ volatile(
        "movq $0xdeadbeefcafef00d, %%rdx\n\t"
        "movq $-1000000007, %%rax\n\t"
        "movq $12345, %%rcx\n\t"
        "cqto\n\t"
        "idivq %%rcx\n\t"
        "movq %%rax, %[q]\n\t"
        "movq %%rdx, %[r]\n\t"
        : [q] "=&r"(q), [r] "=&r"(r) : : "rax", "rcx", "rdx", "cc", "memory");
    g_puts("idiv12345: q="); g_puti64_nonl((g_i64)q); g_puts(" r="); g_puti64_nonl((g_i64)r); g_puts(" faults="); g_putu64(nfault);
    return 0;
}

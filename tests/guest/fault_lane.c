/* A fault inside scalar and 256-bit loops whose values live in host lane registers. */
#include "gsys.h"
#define SYS_sigaction 46
#define SYS_mprotect 74
#define SIGSEGV 11
#define SIGBUS 10
#define SA_SIGINFO 0x0040
#define PAGE 0x4000
typedef unsigned long long u64;
typedef double v4d __attribute__((vector_size(32), aligned(8)));
struct k_sigaction {
    u64 handler;
    u64 tramp;
    g_u32 mask;
    g_u32 flags;
};
static u64 g_map; static volatile u64 g_faults;
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
static u64 bits(double d) { union { double d; u64 u; } u; u.d = d; return u.u; }
static void p(const char *name, u64 v) { g_puts(name); g_puthex64(v); }

__attribute__((noinline)) static void run_sse2(const double *a, long n, double *out)
{
    double x = 1.5, y = -0.25, s = 0.0, t = 2.0;
    for (long i = 0; i < n; i++) {
        x = x * a[i] + y;
        y = y - x * 0.5;
        t = t * 0.75 + a[i];
        s += a[i] * x - t;
    }
    out[0] = x; out[1] = y; out[2] = s; out[3] = t;
}
__attribute__((noinline, target("avx2,fma"))) static void run_avx(const double *a, long n, double *out)
{
    double x = 1.5, y = -0.25, s = 0.0, t = 2.0;
    for (long i = 0; i < n; i++) {
        x = x * a[i] + y;
        y = y - x * 0.5;
        t = t * 0.75 + a[i];
        s += a[i] * x - t;
    }
    out[0] = x; out[1] = y; out[2] = s; out[3] = t;
}
__attribute__((noinline, target("avx2"))) static void run_ymm(const double *a, long n, double *out)
{
    v4d acc = { 0.5, 0.25, 0.125, 1.0 }, m = { 1.0001, 0.9999, 1.0, 1.00005 }, acc2 = { 1, 2, 3, 4 };
    for (long i = 0; i + 4 <= n; i += 4) {
        v4d v = *(const v4d *)(a + i);
        acc = acc * m + v;
        acc2 = acc2 - v * m;
    }
    *(v4d *)out = acc;
    *(v4d *)(out + 4) = acc2;
}
static void handler(int signo, void *siginfo, void *ucontext)
{
    (void)signo; (void)siginfo;
    u64 uc = (u64)ucontext;
    u64 mc = *(u64 *)(uc + 48);
    p("faultoff ", *(u64 *)(mc + 8) - g_map);
    for (int r = 0; r < 8; r++) {
        g_puts("xmm"); g_putu64_nonl((u64)r); g_puts(" ");
        g_puthex64(*(u64 *)(mc + 352 + 16 * r)); 
        g_puts("     "); g_puthex64(*(u64 *)(mc + 352 + 16 * r + 8));
    }
    g_syscall3(SYS(SYS_mprotect), (g_i64)(g_map + PAGE), PAGE, PROT_READ | PROT_WRITE);
    g_faults++;
}
static void reset(double *a)
{
    for (int i = 0; i < PAGE / 8; i++) a[i] = 0.999 + (double)(i % 16) * 0.0001;
    g_syscall3(SYS(SYS_mprotect), (g_i64)(g_map + PAGE), PAGE, 0);
    g_faults = 0;
}
int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    struct k_sigaction sa;
    sa.handler = (u64)&handler;
    sa.tramp = (u64)&sig_tramp;
    sa.mask = 0;
    sa.flags = SA_SIGINFO;
    g_syscall3(SYS(SYS_sigaction), SIGSEGV, (g_i64)(u64)&sa, 0);
    g_syscall3(SYS(SYS_sigaction), SIGBUS, (g_i64)(u64)&sa, 0);
    unsigned char *map = (unsigned char *)sys_mmap(0, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if ((g_i64)map < 0 && (g_i64)map > -4096) { g_puts("mmap failed\n"); return 1; }
    g_map = (u64)map;
    double *a = (double *)map;
    long n = PAGE / 8 + 12;
    double out[8] __attribute__((aligned(32)));
    reset(a); run_sse2(a, n, out);
    p("sse2 faults ", g_faults); for (int i = 0; i < 4; i++) { g_puts("sse2 "); g_puthex64(bits(out[i])); }
    reset(a); run_avx(a, n, out);
    p("avx faults ", g_faults); for (int i = 0; i < 4; i++) { g_puts("avx "); g_puthex64(bits(out[i])); }
    reset(a); run_ymm(a, n, out);
    p("ymm faults ", g_faults); for (int i = 0; i < 8; i++) { g_puts("ymm "); g_puthex64(bits(out[i])); }
    return 0;
}

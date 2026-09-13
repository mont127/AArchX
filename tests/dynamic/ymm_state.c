#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

#define ASM(name, body) extern uint64_t name(uint64_t, uint64_t, uint64_t); \
    __asm__(".text\n.p2align 4\n.globl _" #name "\n_" #name ":\n" body "\n")

ASM(t_cpuid, " pushq %rbx\n movq %rdx, %r8\n movl %edi, %eax\n movl %esi, %ecx\n cpuid\n"
             " movl %eax, 0(%r8)\n movl %ebx, 4(%r8)\n movl %ecx, 8(%r8)\n movl %edx, 12(%r8)\n popq %rbx\n ret");
ASM(t_xgetbv, " xorl %ecx, %ecx\n xgetbv\n shlq $32, %rdx\n orq %rdx, %rax\n ret");
ASM(t_xsave, " vmovdqu (%rdx), %ymm15\n movl %esi, %eax\n xorl %edx, %edx\n xsave (%rdi)\n vzeroupper\n ret");
ASM(t_xrstor, " movq %rdx, %r8\n vpcmpeqd %ymm15, %ymm15, %ymm15\n movl %esi, %eax\n xorl %edx, %edx\n"
              " xrstor (%rdi)\n vmovdqu %ymm15, (%r8)\n vzeroupper\n ret");
ASM(t_ymmsig, " vmovdqu (%rdi), %ymm15\n movq %rsi, %r8\n movl %edx, %edi\n movl $30, %esi\n"
              " movl $0x2000025, %eax\n syscall\n movl $0x2000014, %eax\n syscall\n movl $0x2000014, %eax\n syscall\n"
              " movl $100000, %ecx\n1: decl %ecx\n jnz 1b\n vmovdqu %ymm15, (%r8)\n vzeroupper\n ret");
ASM(t_ymm_clobber, " vpcmpeqd %ymm15, %ymm15, %ymm15\n ret");

static int fails;
static int g_modify, g_seen;
static uint32_t g_mcsize;
static uint8_t g_frame_lo[16], g_frame_hi[16];

static void expect(const char *what, int ok)
{
    if (!ok) {
        printf("BAD %s\n", what);
        fails++;
    }
}

static int all_bytes(const uint8_t *p, size_t n, uint8_t v)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] != v)
            return 0;
    return 1;
}

static void on_usr1(int sig, siginfo_t *si, void *ctx)
{
    (void)sig;
    (void)si;
    ucontext_t *uc = ctx;
    g_seen++;
    g_mcsize = (uint32_t)uc->uc_mcsize;
    if (uc->uc_mcsize >= sizeof(struct __darwin_mcontext_avx64)) {
        struct __darwin_mcontext_avx64 *mc = (void *)uc->uc_mcontext;
        memcpy(g_frame_lo, &mc->__fs.__fpu_xmm15, 16);
        memcpy(g_frame_hi, &mc->__fs.__fpu_ymmh15, 16);
        if (g_modify) {
            memset(&mc->__fs.__fpu_xmm15, 0xa5, 16);
            memset(&mc->__fs.__fpu_ymmh15, 0x5a, 16);
        }
    }
    t_ymm_clobber(0, 0, 0);
}

int main(void)
{
    uint32_t r[4];
    t_cpuid(0, 0, (uint64_t)(uintptr_t)r);
    expect("cpuid max leaf reaches 0xd", r[0] >= 0xd);
    t_cpuid(0xd, 0, (uint64_t)(uintptr_t)r);
    expect("cpuid 0xd.0", r[0] == 7 && r[1] == 0x340 && r[2] == 0x340);
    t_cpuid(0xd, 2, (uint64_t)(uintptr_t)r);
    expect("cpuid 0xd.2", r[0] == 0x100 && r[1] == 0x240);
    expect("xgetbv", t_xgetbv(0, 0, 0) == 7);

    uint8_t pat[32], out[32], want[32];
    for (int i = 0; i < 32; i++)
        pat[i] = (uint8_t)(0x11 * (i % 15) + 1);
    static uint8_t area[1024] __attribute__((aligned(64)));

    memset(area, 0xee, sizeof area);
    t_xsave((uint64_t)(uintptr_t)area, 7, (uint64_t)(uintptr_t)pat);
    uint64_t bv;
    memcpy(&bv, area + 512, 8);
    expect("xsave xstate_bv", bv == 7);
    expect("xsave leaves xcomp_bv", all_bytes(area + 520, 8, 0xee));
    expect("xsave xmm15", memcmp(area + 160 + 15 * 16, pat, 16) == 0);
    expect("xsave ymmh15", memcmp(area + 576 + 15 * 16, pat + 16, 16) == 0);
    expect("xsave ends at 832", all_bytes(area + 832, 64, 0xee));

    memset(area + 576 + 15 * 16, 0x77, 16);
    t_xrstor((uint64_t)(uintptr_t)area, 7, (uint64_t)(uintptr_t)out);
    memcpy(want, pat, 16);
    memset(want + 16, 0x77, 16);
    expect("xrstor ymm15", memcmp(out, want, 32) == 0);

    t_xrstor((uint64_t)(uintptr_t)area, 3, (uint64_t)(uintptr_t)out);
    memset(want + 16, 0xff, 16);
    expect("xrstor without the AVX component keeps the upper half", memcmp(out, want, 32) == 0);

    bv = 3;
    memcpy(area + 512, &bv, 8);
    t_xrstor((uint64_t)(uintptr_t)area, 7, (uint64_t)(uintptr_t)out);
    memset(want + 16, 0, 16);
    expect("xrstor with AVX in init state zeroes the upper half", memcmp(out, want, 32) == 0);

    memset(area, 0xee, sizeof area);
    t_xsave((uint64_t)(uintptr_t)area, 2, (uint64_t)(uintptr_t)pat);
    memcpy(&bv, area + 512, 8);
    expect("xsave sse-only keeps xstate_bv bits outside the request", bv == 6);
    expect("xsave sse-only skips ymmh", all_bytes(area + 576, 256, 0xee));
    expect("xsave sse-only xmm15", memcmp(area + 160 + 15 * 16, pat, 16) == 0);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_usr1;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, NULL);
    for (g_modify = 0; g_modify < 2; g_modify++) {
        g_seen = 0;
        memset(out, 0, sizeof out);
        t_ymmsig((uint64_t)(uintptr_t)pat, (uint64_t)(uintptr_t)out, (uint64_t)getpid());
        expect("signal delivered once", g_seen == 1);
        expect("signal frame is the AVX mcontext", g_mcsize == sizeof(struct __darwin_mcontext_avx64));
        expect("signal frame xmm15", memcmp(g_frame_lo, pat, 16) == 0);
        expect("signal frame ymmh15", memcmp(g_frame_hi, pat + 16, 16) == 0);
        if (g_modify) {
            memset(want, 0xa5, 16);
            memset(want + 16, 0x5a, 16);
            expect("sigreturn loads ymm15 from the frame", memcmp(out, want, 32) == 0);
        } else {
            expect("ymm15 survives a handler that clobbers it", memcmp(out, pat, 32) == 0);
        }
    }

    if (!fails)
        printf("OK\n");
    return fails != 0;
}

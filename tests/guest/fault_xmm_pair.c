/* A fault on the second of two adjacent 16-byte loads or stores. */
#include "gsys.h"
#define SYS_sigaction 46
#define SYS_mprotect 74
#define SIGSEGV 11
#define SIGBUS 10
#define SA_SIGINFO 0x0040
#ifndef PROT_READ
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANON 0x1000
#endif
#define PAGE 0x4000
struct k_sigaction {
    g_u64 handler;
    g_u64 tramp;
    g_u32 mask;
    g_u32 flags;
};
g_u64 g_pat[16] __attribute__((used, aligned(16)));
g_u64 g_out[8] __attribute__((used, aligned(16)));
static g_u64 g_body_addr, g_map, g_stage;
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
__asm__(
    ".text\n"
    ".globl _load_body\n"
    "_load_body:\n"
    "    movq %rdi, %r11\n"
    "    leaq _g_pat(%rip), %rsi\n"
    "    movups 32(%rsi), %xmm0\n"
    "    movups 48(%rsi), %xmm1\n"
    "    movups (%r11), %xmm0\n"
    "    movups 16(%r11), %xmm1\n"
    "    ud2\n"
    ".globl _load_recover\n"
    "_load_recover:\n"
    "    leaq _g_out(%rip), %rax\n"
    "    movups %xmm0, (%rax)\n"
    "    movups %xmm1, 16(%rax)\n"
    "    jmp _after_load\n"
    ".globl _store_body\n"
    "_store_body:\n"
    "    movq %rdi, %r11\n"
    "    leaq _g_pat(%rip), %rsi\n"
    "    movups 64(%rsi), %xmm2\n"
    "    movups 80(%rsi), %xmm3\n"
    "    movups %xmm2, (%r11)\n"
    "    movups %xmm3, 16(%r11)\n"
    "    ud2\n"
    ".globl _store_recover\n"
    "_store_recover:\n"
    "    jmp _after_store\n");
extern void load_body(g_u64 p);
extern void store_body(g_u64 p);
extern void load_recover(void);
extern void store_recover(void);
extern void sig_tramp(void);
static void p(const char *name, g_u64 v)
{
    g_puts(name);
    g_puthex64(v);
}
void after_load(void)
{
    p("xmm0.lo ", g_out[0]); p("xmm0.hi ", g_out[1]);
    p("xmm1.lo ", g_out[2]); p("xmm1.hi ", g_out[3]);
    g_stage = 2;
    g_body_addr = (g_u64)&store_body;
    store_body(g_map + PAGE - 16);
    g_puts("unreachable\n");
    sys_exit(3);
}
void after_store(void)
{
    const g_u64 *m = (const g_u64 *)(g_map + PAGE - 16);
    p("mem.lo ", m[0]); p("mem.hi ", m[1]);
    g_puts("done\n");
    sys_exit(0);
}
static void handler(int signo, void *siginfo, void *ucontext)
{
    (void)signo; (void)siginfo;
    g_u64 uc = (g_u64)ucontext;
    g_u64 mc = *(g_u64 *)(uc + 48);
    p("stage ", g_stage);
    p("riptoff ", *(g_u64 *)(mc + 144) - g_body_addr);
    p("faultoff ", *(g_u64 *)(mc + 8) - g_map);
    *(g_u64 *)(mc + 144) = g_stage == 1 ? (g_u64)&load_recover : (g_u64)&store_recover;
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
    unsigned char *map = (unsigned char *)sys_mmap(0, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if ((g_i64)map < 0 && (g_i64)map > -4096) { g_puts("mmap failed\n"); return 1; }
    g_syscall3(SYS(SYS_mprotect), (g_i64)(g_u64)(map + PAGE), PAGE, 0);
    g_map = (g_u64)map;
    g_u64 *q = (g_u64 *)(map + PAGE - 16);
    q[0] = 0x1111111111111111ull; q[1] = 0x2222222222222222ull;
    for (int i = 0; i < 16; i++) g_pat[i] = 0xa0a0a0a0a0a0a0a0ull + (g_u64)i * 0x0101010101010101ull;
    g_stage = 1;
    g_body_addr = (g_u64)&load_body;
    load_body(g_map + PAGE - 16);
    g_puts("unreachable\n");
    return 2;
}

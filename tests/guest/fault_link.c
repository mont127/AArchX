/* Fault inside a cross-block linked loop. */
#include "gsys.h"

#define SYS_sigaction 46
#define SIGSEGV 11
#define SA_SIGINFO 0x0040

struct k_sigaction {
    g_u64 handler;
    g_u64 tramp;
    g_u32 mask;
    g_u32 flags;
};

g_u64 g_buf[16] __attribute__((used));

static g_u64 g_body_addr;

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
    ".globl _fault_body\n"
    "_fault_body:\n"
    "    movabsq $0x0102030405060708, %rax\n"
    "    movabsq $0x1122334455667788, %rbx\n"
    "    movabsq $0xfedcba9876543210, %rdx\n"
    "    movabsq $0x000000000000f00d, %rsi\n"
    "    movabsq $0x0f0f0f0f0f0f0f0f, %rdi\n"
    "    movabsq $0x5555555555555555, %r8\n"
    "    movabsq $0xaaaaaaaaaaaaaaaa, %r9\n"
    "    movabsq $0x123456789abcdef0, %r10\n"
    "    movabsq $0x1111111111111111, %r12\n"
    "    movabsq $0x2222222222222222, %r13\n"
    "    movabsq $0x3333333333333333, %r14\n"
    "    movq    $50, %rcx\n"
    "    leaq    _g_buf(%rip), %r15\n"
    "    jmp     La\n"

    "La:\n"
    "    addq %rcx,%rax\n"
    "    xorq %rax,%rbx\n"
    "    addq %rbx,%rdx\n"
    "    xorq %rdx,%rsi\n"
    "    addq %rsi,%rdi\n"
    "    xorq %rdi,%r8\n"
    "    addq %r8,%rax\n"
    "    xorq %rax,%rbx\n"
    "    addq %rbx,%rdx\n"
    "    xorq %rdx,%rsi\n"
    "    addq %rsi,%rdi\n"
    "    xorq %rdi,%r8\n"
    "    addq %r8,%rax\n"
    "    xorq %rax,%rbx\n"
    "    addq %rbx,%rdx\n"
    "    xorq %rdx,%rsi\n"
    "    addq %rsi,%rdi\n"
    "    xorq %rdi,%r8\n"
    "    addq %r8,%rax\n"
    "    xorq %rax,%rbx\n"
    "    addq %rbx,%rdx\n"
    "    xorq %rdx,%rsi\n"
    "    addq %rsi,%rdi\n"
    "    xorq %rdi,%r8\n"
    "    testq $1,%rax\n"
    "    jz   Lbe\n"

    "Lbo:\n"
    "    subq %rcx,%r9\n"
    "    xorq %r9,%r10\n"
    "    subq %r10,%r12\n"
    "    xorq %r12,%r13\n"
    "    subq %r13,%r14\n"
    "    xorq %r14,%r9\n"
    "    subq %r9,%r10\n"
    "    xorq %r10,%r12\n"
    "    subq %r12,%r13\n"
    "    xorq %r13,%r14\n"
    "    subq %r14,%r9\n"
    "    xorq %r9,%r10\n"
    "    subq %r10,%r12\n"
    "    xorq %r12,%r13\n"
    "    subq %r13,%r14\n"
    "    xorq %r14,%r9\n"
    "    subq %r9,%r10\n"
    "    xorq %r10,%r12\n"
    "    subq %r12,%r13\n"
    "    xorq %r13,%r14\n"
    "    subq %r14,%r9\n"
    "    xorq %r9,%r10\n"
    "    subq %r10,%r12\n"
    "    xorq %r12,%r13\n"
    "    jmp  Lmerge\n"

    "Lbe:\n"
    "    addq %rcx,%r9\n"
    "    xorq %r9,%r10\n"
    "    addq %r10,%r12\n"
    "    xorq %r12,%r13\n"
    "    addq %r13,%r14\n"
    "    xorq %r14,%r9\n"
    "    addq %r9,%r10\n"
    "    xorq %r10,%r12\n"
    "    addq %r12,%r13\n"
    "    xorq %r13,%r14\n"
    "    addq %r14,%r9\n"
    "    xorq %r9,%r10\n"
    "    addq %r10,%r12\n"
    "    xorq %r12,%r13\n"
    "    addq %r13,%r14\n"
    "    xorq %r14,%r9\n"
    "    addq %r9,%r10\n"
    "    xorq %r10,%r12\n"
    "    addq %r12,%r13\n"
    "    xorq %r13,%r14\n"
    "    addq %r14,%r9\n"
    "    xorq %r9,%r10\n"
    "    addq %r10,%r12\n"
    "    xorq %r12,%r13\n"
    "    jmp  Lmerge\n"

    "Lmerge:\n"
    "    xorq %r11,%r11\n"
    "    cmpq $10,%rcx\n"
    "    cmovneq %r15,%r11\n"
    "    movq %rax,(%r11)\n"
    "    decq %rcx\n"
    "    jnz  La\n"
    "    ud2\n");

extern void fault_body(void);
extern void sig_tramp(void);

static void recover(void)
{
    g_puts("recovered\n");
    sys_exit(0);
}

static void p(const char *name, g_u64 v)
{
    g_puts(name);
    g_puthex64(v);
}

static void handler(int signo, void *siginfo, void *ucontext)
{
    g_u64 uc = (g_u64)ucontext;
    g_u64 mc = *(g_u64 *)(uc + 48);

    p("trapno     ", *(g_u64 *)(mc + 0) & 0xffff);
    p("faultvaddr ", *(g_u64 *)(mc + 8));
    p("riptoff    ", *(g_u64 *)(mc + 144) - g_body_addr);
    p("rax ", *(g_u64 *)(mc + 16));
    p("rbx ", *(g_u64 *)(mc + 24));
    p("rcx ", *(g_u64 *)(mc + 32));
    p("rdx ", *(g_u64 *)(mc + 40));
    p("rdi ", *(g_u64 *)(mc + 48));
    p("rsi ", *(g_u64 *)(mc + 56));
    p("r8  ", *(g_u64 *)(mc + 80));
    p("r9  ", *(g_u64 *)(mc + 88));
    p("r10 ", *(g_u64 *)(mc + 96));
    p("r12 ", *(g_u64 *)(mc + 112));
    p("r13 ", *(g_u64 *)(mc + 120));
    p("r14 ", *(g_u64 *)(mc + 128));
    p("rflags&8d5 ", *(g_u64 *)(mc + 152) & 0x8d5);

    *(g_u64 *)(mc + 144) = (g_u64)(g_u64 *)&recover;
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

    g_body_addr = (g_u64)&fault_body;

    g_puts("before fault\n");
    fault_body();
    g_puts("unreachable\n");
    return 2;
}

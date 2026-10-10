/*
 * rsp written every way an instruction can write it, on a stack below 12 GB
 * and on one above it, and switched between the two: the shapes src/jit.c
 * must convert while the Wine layout keeps rsp's register as a host pointer
 * (rsp plus the stack delta, g_lowptr).  mov rsp from a register and from
 * memory, lea rsp off rsp and off rbp, add and sub with an immediate (one
 * with its flags read) and with a register, and with -16, xchg, pop rsp,
 * cmov, leave, push rsp, and rsp read back with mov, lea and a store; push,
 * pop, call and rsp-relative loads and stores after each.  Then the pairs and
 * fused forms that read or write rsp's register themselves: mov then and, from
 * rsp and into it, cmp and test of rsp fused with a jump, esp's low half, steps
 * too big for one instruction, rsp stored at rsp, an indexed lea, the other
 * stack reached as rsp plus an index, and add and sub of a register that cross
 * between the stacks.  Last, a load that faults on each stack, whose handler
 * reads rsp from the context: the translator rebuilds it from the host
 * register.  Then fork, from each stack: the translator hands it back to its
 * outer loop from inside a block, which must hear the request.  The checksum
 * takes values and rsp's offsets from the stack tops, never addresses, so it is
 * the same anywhere.  Against Rosetta.
 */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <unistd.h>

/* rspforms(top, other_top, out): runs on top, visits other_top, comes back; returns a checksum. */
__asm__(
    ".text\n"
    ".p2align 4\n"
    "_lsp_leaf:\n"
    "    mov 8(%rsp), %rax\n"            /* an argument above the return address */
    "    lea 3(%rax,%rax,2), %rax\n"
    "    ret\n"
    ".p2align 4\n"
    "_rspforms:\n"
    "    push %rbp\n    push %rbx\n    push %r12\n    push %r13\n    push %r14\n    push %r15\n"
    "    mov %rsp, %r15\n"
    "    mov %rdx, %r14\n"
    "    xor %r13d, %r13d\n"
    "    mov %rdi, %r12\n"
    /* mov rsp, reg */
    "    mov %rdi, %rsp\n"
    "    push $0x11\n    push $0x22\n    pop %rax\n    pop %rcx\n"
    "    imul $3, %r13, %r13\n    add %rax, %r13\n    imul $3, %r13, %r13\n    add %rcx, %r13\n"
    /* sub rsp, imm with its flags read */
    "    sub $0x40, %rsp\n"
    "    ja 1f\n"
    "    add $1000, %r13\n"
    "1:  movq $0x33, 8(%rsp)\n    mov 8(%rsp), %rax\n    add %rax, %r13\n"
    /* lea rsp off rsp, and rsp read back three ways */
    "    lea 0x20(%rsp), %rsp\n"
    "    mov %rsp, %rax\n    sub %r12, %rax\n    imul $5, %r13, %r13\n    add %rax, %r13\n"
    "    lea 0x18(%rsp), %rbx\n    sub %r12, %rbx\n    add %rbx, %r13\n"
    "    mov %rsp, 0(%r14)\n    mov 0(%r14), %rcx\n    sub %r12, %rcx\n    imul $7, %r13, %r13\n    add %rcx, %r13\n"
    "    push %rsp\n    pop %rbx\n    sub %rsp, %rbx\n    add %rbx, %r13\n"
    /* and rsp, -16 after a step that -32 would round further */
    "    sub $0x8, %rsp\n    and $-16, %rsp\n"
    "    mov %rsp, %rax\n    sub %r12, %rax\n    imul $11, %r13, %r13\n    add %rax, %r13\n"
    "    movq $0x55, (%rsp)\n    add (%rsp), %r13\n"
    /* a frame: push rbp; mov rbp, rsp; sub; slots; lea rsp off rbp; leave */
    "    push %rbp\n    mov %rsp, %rbp\n    sub $0x30, %rsp\n"
    "    mov %r13, -8(%rbp)\n    movq $0x66, -0x10(%rbp)\n    mov -0x10(%rbp), %rax\n    add %rax, %r13\n"
    "    push $0x77\n    lea -0x30(%rbp), %rsp\n    add -8(%rbp), %r13\n"
    "    leave\n"
    "    mov %rsp, %rax\n    sub %r12, %rax\n    imul $13, %r13, %r13\n    add %rax, %r13\n"
    /* a call with an argument on the stack */
    "    push $9\n    call _lsp_leaf\n    add $8, %rsp\n    add %rax, %r13\n"
    /* xchg into the other stack, work there, and back through memory */
    "    mov %rsi, %rax\n    xchg %rax, %rsp\n"
    "    push $0x88\n    pop %rcx\n    add %rcx, %r13\n"
    "    mov %rsp, %rbx\n    sub %rsi, %rbx\n    imul $17, %r13, %r13\n    add %rbx, %r13\n"
    "    push $5\n    call _lsp_leaf\n    add $8, %rsp\n    add %rax, %r13\n"
    "    mov %rax, 8(%r14)\n"            /* the value only, for the store below */
    "    mov %r12, 16(%r14)\n    mov 16(%r14), %rsp\n"
    "    push $0x99\n    pop %rcx\n    add %rcx, %r13\n"
    /* pop rsp into the other stack, cmov back */
    "    push %rsi\n    pop %rsp\n"
    "    mov %rsp, %rbx\n    sub %rsi, %rbx\n    imul $19, %r13, %r13\n    add %rbx, %r13\n"
    "    movq $0xaa, -8(%rsp)\n    add -8(%rsp), %r13\n"
    "    xor %eax, %eax\n    cmovz %r12, %rsp\n"
    "    push $0xbb\n    pop %rcx\n    add %rcx, %r13\n"
    "    mov %rsp, %rbx\n    sub %r12, %rbx\n    imul $23, %r13, %r13\n    add %rbx, %r13\n"
    /* add rsp, reg: down by 0x48 and back */
    "    mov $-0x48, %rax\n    add %rax, %rsp\n"
    "    movq $0xcc, (%rsp)\n    add (%rsp), %r13\n"
    "    neg %rax\n    add %rax, %rsp\n"
    "    mov %rsp, %rbx\n    sub %r12, %rbx\n    imul $29, %r13, %r13\n    add %rbx, %r13\n"
    /* lea rsp off another register */
    "    lea -0x100(%r12), %rsp\n    push $0xdd\n    pop %rcx\n    add %rcx, %r13\n"
    "    mov %rsp, %rbx\n    sub %r12, %rbx\n    imul $31, %r13, %r13\n    add %rbx, %r13\n"
    "    mov %r15, %rsp\n"
    "    mov %r13, %rax\n"
    "    pop %r15\n    pop %r14\n    pop %r13\n    pop %r12\n    pop %rbx\n    pop %rbp\n"
    "    ret\n"
    /* rspmore(top, other_top): pairs and fused compares that read or write rsp */
    ".p2align 4\n"
    "_rspmore:\n"
    "    push %rbp\n    push %rbx\n    push %r12\n    push %r13\n    push %r14\n    push %r15\n"
    "    mov %rsp, %r15\n"
    "    xor %r13d, %r13d\n"
    "    mov %rdi, %r12\n"
    "    mov %rdi, %rsp\n"
    "    sub $0x28, %rsp\n"
    /* mov then and, from rsp and into it */
    "    mov %rsp, %rax\n    and $-16, %rax\n    sub %r12, %rax\n    add %rax, %r13\n"
    "    lea -0x17(%r12), %rcx\n    mov %rcx, %rsp\n    and $-32, %rsp\n"
    "    mov %rsp, %rax\n    sub %r12, %rax\n    imul $3, %r13, %r13\n    add %rax, %r13\n"
    "    push $0x12\n    pop %rax\n    add %rax, %r13\n"
    /* compares of rsp with a register, each way, and a test, fused with a jump */
    "    lea -0x10(%rsp), %rbx\n"
    "    cmp %rbx, %rsp\n    jbe 1f\n    add $0x100, %r13\n"
    "1:  cmp %rsp, %rbx\n    jae 1f\n    add $0x200, %r13\n"
    "1:  mov %rsp, %rbx\n    cmp %rbx, %rsp\n    jne 1f\n    add $0x400, %r13\n"
    "1:  mov $15, %ecx\n    test %rcx, %rsp\n    jne 1f\n    add $0x800, %r13\n"
    "1:  test $0x10, %rsp\n    jne 1f\n    add $0x1000, %r13\n"
    "1:  cmp $0, %rsp\n    je 1f\n    add $0x2000, %r13\n"
    /* esp's low half read, and a large immediate step each way */
    "1:  mov %esp, %eax\n    sub %r12d, %eax\n    movslq %eax, %rax\n    imul $5, %r13, %r13\n    add %rax, %r13\n"
    "    sub $0x2000, %rsp\n    movq $0x34, 0x10(%rsp)\n    add 0x10(%rsp), %r13\n    add $0x2000, %rsp\n"
    "    add $-128, %rsp\n    movq $0x56, (%rsp)\n    add (%rsp), %r13\n    sub $-128, %rsp\n"
    "    mov %rsp, %rax\n    sub %r12, %rax\n    imul $7, %r13, %r13\n    add %rax, %r13\n"
    /* rsp stored where it points, and an indexed lea off it */
    "    mov %rsp, 8(%rsp)\n    mov 8(%rsp), %rax\n    sub %rsp, %rax\n    add %rax, %r13\n"
    "    mov $3, %ecx\n    lea 8(%rsp,%rcx,8), %rax\n    sub %rsp, %rax\n    imul $11, %r13, %r13\n    add %rax, %r13\n"
    /* the other stack reached as rsp + (p - rsp), a load and a store */
    "    movq $0x5a, -0x20(%rsi)\n    mov %rsi, %rdi\n    sub %rsp, %rdi\n"
    "    mov -0x20(%rsp,%rdi), %rax\n    add %rax, %r13\n"
    "    mov %r13, -0x28(%rsp,%rdi,1)\n    mov -0x28(%rsi), %rax\n    sub %r13, %rax\n    add $0x3c, %rax\n    add %rax, %r13\n"
    /* add rsp, reg to the other stack, a call there, and sub rsp, reg back */
    "    mov %rsi, %rax\n    sub %rsp, %rax\n    sub $0x40, %rax\n    add %rax, %rsp\n"
    "    mov %rsp, %rbx\n    sub %rsi, %rbx\n    imul $13, %r13, %r13\n    add %rbx, %r13\n"
    "    push $7\n    call _lsp_leaf\n    add $8, %rsp\n    add %rax, %r13\n"
    "    movq $0x78, 0x18(%rsp)\n    add 0x18(%rsp), %r13\n"
    "    mov %rsp, %rax\n    sub %r12, %rax\n    sub %rax, %rsp\n"
    "    push $0x9a\n    pop %rcx\n    add %rcx, %r13\n"
    "    mov %rsp, %rbx\n    sub %r12, %rbx\n    imul $17, %r13, %r13\n    add %rbx, %r13\n"
    "    mov %r15, %rsp\n"
    "    mov %r13, %rax\n"
    "    pop %r15\n    pop %r14\n    pop %r13\n    pop %r12\n    pop %rbx\n    pop %rbp\n"
    "    ret\n"
    /* rspfault(top, bad): a load from bad faults on the stack at top; the handler reports rsp and steps over it */
    ".p2align 4\n"
    "_call_on:\n"                   /* call_on(top, fn): fn() with rsp at top, then back */
    "    push %rbp\n    mov %rsp, %rbp\n    mov %rdi, %rsp\n    call *%rsi\n    mov %rbp, %rsp\n    pop %rbp\n    ret\n"
    ".p2align 4\n"
    "_rspfault:\n"
    "    push %rbp\n    push %rbx\n    push %r12\n    push %r13\n    push %r14\n    push %r15\n"
    "    mov %rsp, %r15\n"
    "    xor %r13d, %r13d\n"
    "    mov %rdi, %r12\n"
    "    mov %rdi, %rsp\n"
    "    push $0x21\n    push $0x43\n    sub $0x18, %rsp\n"
    "    mov %rsp, %rbx\n"
    "    mov (%rsi), %rcx\n"            /* 48 8b 0e: three bytes, which the handler skips */
    "    mov _g_fault_rsp(%rip), %rax\n    sub %rbx, %rax\n    add %rax, %r13\n"
    "    mov %rsp, %rax\n    sub %r12, %rax\n    imul $3, %r13, %r13\n    add %rax, %r13\n"
    "    add $0x18, %rsp\n    pop %rax\n    pop %rcx\n    imul $5, %r13, %r13\n    add %rax, %r13\n    add %rcx, %r13\n"
    "    mov %r15, %rsp\n"
    "    mov %r13, %rax\n"
    "    pop %r15\n    pop %r14\n    pop %r13\n    pop %r12\n    pop %rbx\n    pop %rbp\n"
    "    ret\n");

uint64_t rspforms(uint8_t *top, uint8_t *other, uint64_t *out);
uint64_t rspmore(uint8_t *top, uint8_t *other);
uint64_t rspfault(uint8_t *top, const uint64_t *bad);

int call_on(uint8_t *top, int (*fn)(void));

/* fork, which the translator hands back to its outer loop from the middle of a block; the child works and exits */
static uint8_t *g_low, *g_high;
static int fork_once(void)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) _exit((int)(rspmore(g_low + 24 * 1024, g_high + 24 * 1024) & 0x7f));
    int st = 0;
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st)) return -2;
    return WEXITSTATUS(st);
}

uint64_t g_fault_rsp;
static void on_fault(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si;
    ucontext_t *uc = ctx;
    g_fault_rsp = uc->uc_mcontext->__ss.__rsp;
    uc->uc_mcontext->__ss.__rip += 3;
}

static uint8_t *place(uint64_t want, size_t len)
{
    mach_vm_address_t a = want;
    if (mach_vm_allocate(mach_task_self(), &a, len, VM_FLAGS_FIXED) == KERN_SUCCESS)
        return (uint8_t *)(uintptr_t)a;
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

int main(void)
{
    size_t len = 64 * 1024;
    uint8_t *low = place(0x2e0000000ull, len), *high = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (!low || high == MAP_FAILED) { printf("map\n"); return 1; }
    fprintf(stderr, "lowstack_ptr: low %p high %p\n", (void *)low, (void *)high);
    uint64_t out[4];
    uint64_t a = 0, b = 0, c = 0, d = 0;
    for (int r = 0; r < 2000; r++) {
        a += rspforms(low + 48 * 1024, high + 48 * 1024, out);
        b += rspforms(high + 48 * 1024, low + 48 * 1024, out);
        c += rspmore(low + 48 * 1024, high + 48 * 1024);
        d += rspmore(high + 48 * 1024, low + 48 * 1024);
    }
    printf("low first %llx high first %llx\n", (unsigned long long)a, (unsigned long long)b);
    printf("more: low first %llx high first %llx\n", (unsigned long long)c, (unsigned long long)d);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    uint64_t *bad = mmap(NULL, 16384, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (bad == MAP_FAILED) { printf("map\n"); return 1; }
    uint64_t e = 0, f = 0;
    for (int r = 0; r < 100; r++) {
        e += rspfault(low + 40 * 1024, bad + 2 * r);   /* a new address each time: */
        f += rspfault(high + 40 * 1024, bad + 2 * r + 1);  /* ocerz ends a thread faulting at one address 17 times running */
    }
    printf("fault: low %llx high %llx\n", (unsigned long long)e, (unsigned long long)f);

    g_low = low; g_high = high;
    int fh = fork_once(), fl = call_on(low + 32 * 1024, fork_once);
    printf("fork: high %d low %d\n", fh, fl);
    return 0;
}

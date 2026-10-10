/*
 * Memory reached off one base register many times in a block, the shape
 * src/jit.c's Wine-layout base guard takes (wine_base_ea): the first operand
 * on a base settles the base's host address in a scratch register, and the
 * operands after it on the same base, while the block writes neither, are
 * that register plus their displacement.  So each function here works off a
 * base with displacements on both sides of it, through loads and stores of
 * every size, sign- and zero-extending loads, read-modify-write and compare
 * forms (with the setcc after them), a push and a pop of memory, and SSE
 * loads and stores, pairs of them adjacent; then writes the base the ways that
 * are translated together with the next instruction (a mov and an and, a lea
 * inside a compare and its jump, as Lua's code has them) and goes on off the
 * new value; works off an argument register it never writes, whose side is
 * guessed from the value it had at translation; walks backwards off a pointer
 * one past an array that ends exactly at 12 GB, which is above 12 GB itself;
 * and calls out (getpid) between
 * two operands on one base.  The bases are a mapping at 5 GB and malloc's
 * memory, each first and then alternating, so the side guessed at
 * translation is wrong half the time, and the whole is run before and after a
 * second thread exists, which makes every access ordered.  Against Rosetta.
 */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

__asm__(
    ".text\n"
    ".p2align 4\n"
    /* wb_forms(p): p points into the middle of 1 KB; returns a checksum */
    "_wb_forms:\n"
    "    push %rbx\n    push %r12\n"
    "    mov %rdi, %rbx\n"
    "    mov 8(%rbx), %rax\n"
    "    add -0x10(%rbx), %rax\n"
    "    movzbl 0x11(%rbx), %ecx\n    add %rcx, %rax\n"
    "    movswq -0x22(%rbx), %rcx\n    add %rcx, %rax\n"
    "    movslq 0x24(%rbx), %rcx\n    add %rcx, %rax\n"
    "    mov %rax, 0x30(%rbx)\n"
    "    movl %eax, -0x38(%rbx)\n"
    "    movw %ax, 0x3e(%rbx)\n"
    "    movb %al, -0x41(%rbx)\n"
    "    addq %rax, -0x48(%rbx)\n"
    "    xorl %eax, 0x50(%rbx)\n"
    "    incq -0x58(%rbx)\n"
    "    cmpl $0, -0x48(%rbx)\n    setl %cl\n    movzbl %cl, %ecx\n    add %rcx, %rax\n"
    "    cmpb $0x31, 0x11(%rbx)\n    sete %cl\n    movzbl %cl, %ecx\n    add %rcx, %rax\n"
    "    cmp -0x10(%rbx), %rax\n    setb %cl\n    movzbl %cl, %ecx\n    add %rcx, %rax\n"
    "    push 0x60(%rbx)\n    pop -0x68(%rbx)\n"
    "    movdqu -0x18(%rbx), %xmm0\n    movdqu -0x8(%rbx), %xmm1\n"
    "    movdqu %xmm1, 0x70(%rbx)\n    movdqu %xmm0, 0x80(%rbx)\n"
    "    movdqu %xmm0, 0x94(%rbx)\n    movdqu %xmm1, 0xa4(%rbx)\n"
    "    movdqu %xmm1, -0xb8(%rbx)\n    movdqu %xmm0, -0xa8(%rbx)\n"
    "    movq %xmm0, %rcx\n    add %rcx, %rax\n"
    "    mov $0x77, %r12d\n    mov %r12, 0xf8(%rbx)\n    add 0xf8(%rbx), %rax\n"
    "    pop %r12\n    pop %rbx\n"
    "    ret\n"
    ".p2align 4\n"
    /* wb_rebase(p, q): off p, then the base rewritten to q by fused pairs */
    "_wb_rebase:\n"
    "    push %rbx\n    push %r15\n"
    "    mov %rdi, %rbx\n"
    "    mov 8(%rbx), %rax\n"
    "    add -8(%rbx), %rax\n"
    "    mov %rsi, %rbx\n    and $-16, %rbx\n"
    "    add 8(%rbx), %rax\n"
    "    add -8(%rbx), %rax\n"
    "    mov %rdi, %r15\n"
    "    mov 0x10(%r15), %rcx\n"
    "    cmp %rax, %rcx\n    lea 0x40(%rsi), %r15\n    jne 1f\n"
    "    add $1, %rax\n"
    "1:  add 0x10(%r15), %rax\n"
    "    add -0x20(%r15), %rax\n"
    /* a compare, a mov rewriting the base, a jump: translated as one */
    "    mov %rdi, %rbx\n"
    "    add 8(%rbx), %rax\n"
    "    mov 0x10(%rbx), %r15\n"
    "    cmp %r15, %rax\n    mov %rsi, %rbx\n    je 2f\n"
    "    movzbl 0x64(%rbx), %edx\n    add %rdx, %rax\n"
    /* a compare off the base, a lea moving it, a jump */
    "2:  mov %rdi, %r15\n"
    "    cmpq $0, 0x10(%r15)\n    lea 0x10(%r15), %r15\n    je 3f\n"
    "    mov 8(%r15), %rcx\n    add %rcx, %rax\n"
    "3:  pop %r15\n    pop %rbx\n"
    "    ret\n"
    ".p2align 4\n"
    /* wb_arg(p): off a base the function never writes */
    "_wb_arg:\n"
    "    mov 8(%rdi), %rax\n"
    "    add -0x10(%rdi), %rax\n"
    "    mov %rax, 0x18(%rdi)\n"
    "    addq $3, -0x20(%rdi)\n"
    "    add 0x28(%rdi), %rax\n"
    "    ret\n"
    ".p2align 4\n"
    /* wb_arg2(p): the same, first called with the other side */
    "_wb_arg2:\n"
    "    mov 8(%rdi), %rax\n"
    "    add -0x10(%rdi), %rax\n"
    "    mov %rax, 0x18(%rdi)\n"
    "    addq $3, -0x20(%rdi)\n"
    "    add 0x28(%rdi), %rax\n"
    "    ret\n"
    ".p2align 4\n"
    /* wb_back(end): backwards off a one-past-the-end pointer, the base it holds and a copy */
    "_wb_back:\n"
    "    push %rbx\n"
    "    mov -8(%rdi), %rax\n"
    "    add -0x40(%rdi), %rax\n"
    "    mov %rdi, %rbx\n"
    "    mov -0x18(%rbx), %rcx\n    add %rcx, %rax\n"
    "    mov %rax, -0x20(%rbx)\n"
    "    add -0x28(%rbx), %rax\n"
    "    pop %rbx\n"
    "    ret\n"
    ".p2align 4\n"
    /* wb_callout(p): two operands on one base with a system call, a call-out, between */
    "_wb_callout:\n"
    "    push %rbx\n    push %r12\n"
    "    mov %rdi, %r12\n"
    "    mov 8(%r12), %r8\n"
    "    mov $0x2000014, %eax\n    syscall\n"     /* getpid */
    "    add 0x10(%r12), %r8\n"
    "    add -0x18(%r12), %r8\n"
    "    mov %r8, %rax\n"
    "    pop %r12\n    pop %rbx\n"
    "    ret\n");

uint64_t wb_forms(uint8_t *p);
uint64_t wb_rebase(uint8_t *p, uint8_t *q);
uint64_t wb_callout(uint8_t *p);
uint64_t wb_arg(uint8_t *p);
uint64_t wb_arg2(uint8_t *p);
uint64_t wb_back(uint8_t *end);

static uint64_t seed(uint8_t *buf, size_t n, uint64_t s)
{
    for (size_t k = 0; k < n; k++) { s = s * 6364136223846793005ull + 1442695040888963407ull; buf[k] = (uint8_t)(s >> 33); }
    return s;
}

static uint64_t sum(const uint8_t *buf, size_t n)
{
    uint64_t s = 0;
    for (size_t k = 0; k < n; k++) s = s * 31 + buf[k];
    return s;
}

/* Called through pointers, so that they stay blocks of their own instead of being spliced into the caller. */
static uint64_t (*volatile g_arg)(uint8_t *) = wb_arg, (*volatile g_arg2)(uint8_t *) = wb_arg2;
static uint8_t *g_end;   /* one past an array that ends at 12 GB in the Wine layout's build */

/* Each side first, then alternating; the buffers' contents are summed at the end. */
static uint64_t rounds(uint8_t *lo, uint8_t *hi)
{
    uint64_t acc = 0;
    for (int r = 0; r < 600; r++) {
        uint8_t *p = (r < 200) ? lo : (r < 400) ? hi : ((r & 1) ? lo : hi);
        uint8_t *q = p == lo ? hi : lo;
        acc = acc * 3 + wb_forms(p + 512);
        acc = acc * 5 + wb_rebase(p + 512, q + 600);
        acc = acc * 7 + wb_callout(q + 256);
        acc = acc * 11 + g_arg(q + 768);
        acc = acc * 13 + g_arg2(p + 832);
        acc = acc * 17 + wb_back(g_end);
    }
    return acc + sum(lo, 1024) * 3 + sum(hi, 1024) + sum(g_end - 0x100, 0x100) * 5;
}

static void *idle(void *arg) { return arg; }

static uint8_t *place(uint64_t want, size_t len, int low_build)
{
    mach_vm_address_t a = want;
    if (low_build && mach_vm_allocate(mach_task_self(), &a, len, VM_FLAGS_FIXED) == KERN_SUCCESS)
        return (uint8_t *)(uintptr_t)a;
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static uint8_t g_image_mark;

int main(void)
{
    int low_build = (uintptr_t)&g_image_mark - 0x200000000ull < 0x100000000ull;
    uint8_t *lo = place(0x140000000ull, 16384, low_build), *hi = malloc(1024);
    uint8_t *edge = place(0x300000000ull - 0x4000, 0x4000, low_build);
    if (!lo || !hi || !edge) { printf("map\n"); return 1; }
    g_end = edge + 0x4000;
    seed(edge + 0x3f00, 0x100, 5);
    fprintf(stderr, "wine_base: low %p high %p\n", (void *)lo, (void *)hi);
    seed(lo, 1024, 1);
    seed(hi, 1024, 2);
    uint64_t plain = rounds(lo, hi);
    pthread_t t;
    pthread_create(&t, NULL, idle, NULL);
    pthread_join(t, NULL);
    seed(lo, 1024, 3);
    seed(hi, 1024, 4);
    uint64_t ordered = rounds(lo, hi);
    printf("plain %llx ordered %llx\n", (unsigned long long)plain, (unsigned long long)ordered);
    return 0;
}

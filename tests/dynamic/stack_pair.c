/*
 * Runs of 64-bit pushes and pops, which src/jit.c emits two at a time (stp and
 * ldp, emit_stack_pair) where the stack delta is in use: odd and even runs, a
 * register pushed twice, two pops into one register, pops in another order
 * than the pushes, and a frame of the shape compilers emit around a call.
 * Prints the registers after each, against Rosetta.
 */
#include <stdint.h>
#include <stdio.h>

static uint64_t r[8];

static void pushes_pops(void)
{
    __asm__ volatile(
        "mov $0x1111, %%rax\n\t" "mov $0x2222, %%rbx\n\t" "mov $0x3333, %%rcx\n\t" "mov $0x4444, %%rdx\n\t"
        "mov $0x5555, %%rsi\n\t" "mov $0x6666, %%rdi\n\t" "mov $0x7777, %%r8\n\t"
        "push %%rax\n\t" "push %%rbx\n\t" "push %%rcx\n\t" "push %%rdx\n\t" "push %%rsi\n\t"
        "push %%rax\n\t" "push %%rax\n\t"
        "pop %%r8\n\t" "pop %%r8\n\t"
        "pop %%rdi\n\t" "pop %%rsi\n\t" "pop %%rdx\n\t" "pop %%rcx\n\t" "pop %%rax\n\t"
        "mov %%rax, 0(%0)\n\t" "mov %%rcx, 8(%0)\n\t" "mov %%rdx, 16(%0)\n\t" "mov %%rsi, 24(%0)\n\t"
        "mov %%rdi, 32(%0)\n\t" "mov %%r8, 40(%0)\n\t" "mov %%rbx, 48(%0)\n\t"
        : : "r"(r) : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "memory");
}

__attribute__((noinline)) static uint64_t leaf(uint64_t a, uint64_t b) { return a * 3 + b; }

/* push rbp; push rbx; push r12; push r13 ... call ... pop r13; pop r12; pop rbx; pop rbp */
__attribute__((noinline)) static uint64_t frame(uint64_t n)
{
    uint64_t out;
    __asm__ volatile(
        "push %%rbp\n\t" "push %%rbx\n\t" "push %%r12\n\t" "push %%r13\n\t"
        "mov %1, %%rbx\n\t" "mov $5, %%r12\n\t" "mov $7, %%r13\n\t"
        "sub $8, %%rsp\n\t"
        "mov %%rbx, %%rdi\n\t" "mov %%r12, %%rsi\n\t"
        "call *%2\n\t"
        "add %%r13, %%rax\n\t" "add %%rbx, %%rax\n\t"
        "add $8, %%rsp\n\t"
        "pop %%r13\n\t" "pop %%r12\n\t" "pop %%rbx\n\t" "pop %%rbp\n\t"
        "mov %%rax, %0"
        : "=&r"(out) : "r"(n), "r"(leaf)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r8", "r9", "r10", "r11", "memory", "cc");
    return out;
}

int main(void)
{
    pushes_pops();
    printf("regs %llx %llx %llx %llx %llx %llx %llx\n", (unsigned long long)r[0], (unsigned long long)r[1],
           (unsigned long long)r[2], (unsigned long long)r[3], (unsigned long long)r[4], (unsigned long long)r[5],
           (unsigned long long)r[6]);
    uint64_t acc = 0;
    for (uint64_t i = 0; i < 100000; i++) acc += frame(i);
    printf("frame %llu\n", (unsigned long long)acc);
    return 0;
}

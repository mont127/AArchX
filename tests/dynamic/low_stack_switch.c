/*
 * Stack slots on either side of 12 GB.  In the Wine layout a stack slot below
 * 12 GB is reached through the low window and one above it at its own address,
 * and src/jit.c keeps that choice for the stack in a register it recomputes
 * whenever rsp is written other than by push, pop, call and ret.  This moves rsp
 * between a stack at 1.25 GB and one at 20 GB with mov, xchg, pop rsp and lea,
 * pushes, pops, calls and returns on each, reads its own frame through rsp-based
 * operands, and loads through an indexed rsp operand that reaches far past the
 * stack, which must not be taken for a stack slot.
 */
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

#define LOW_STACK ((void *)0x50000000)
#define STACK_SIZE 0x10000

/* A stack at its own address above 12 GB: the first hint the system honours. */
static void *high_stack(void)
{
    static const uint64_t hints[] = { 0x500000000ull, 0x1000000000ull, 0x4000000000ull, 0x20000000000ull };
    for (unsigned k = 0; k < sizeof hints / sizeof *hints; k++) {
        void *p = mmap((void *)hints[k], STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p != MAP_FAILED && (uint64_t)p >= 0x300000000ull)
            return p;
        if (p != MAP_FAILED)
            munmap(p, STACK_SIZE);
    }
    return MAP_FAILED;
}

__attribute__((noinline)) static uint64_t frame_sum(uint64_t a, uint64_t b)
{
    volatile uint64_t local[4] = { a, b, a ^ b, a + b };
    return local[0] * 3 + local[1] * 5 + local[2] * 7 + local[3];
}

/* Runs on whatever stack rsp holds: pushes, pops, a call, rsp-relative reads. */
static uint64_t work_on(void *top, uint64_t seed)
{
    uint64_t out;
    __asm__ volatile(
        "mov %%rsp, %%r12\n\t"
        "mov %[top], %%rsp\n\t"
        "push %[seed]\n\t"
        "push $0x1234\n\t"
        "mov 8(%%rsp), %%rdi\n\t"
        "mov (%%rsp), %%rsi\n\t"
        "call *%[fn]\n\t"
        "pop %%rcx\n\t"
        "pop %%rdx\n\t"
        "add %%rcx, %%rax\n\t"
        "add %%rdx, %%rax\n\t"
        "lea 16(%%rsp), %%rcx\n\t"
        "sub %[top], %%rcx\n\t"
        "add %%rcx, %%rax\n\t"
        "mov %%r12, %%rsp\n\t"
        : "=&a"(out)
        : [top] "r"(top), [seed] "r"(seed), [fn] "r"(frame_sum)
        : "rcx", "rdx", "rdi", "rsi", "r8", "r9", "r10", "r11", "r12", "memory", "cc");
    return out;
}

/* Switches stacks with xchg and pop rsp instead of mov. */
static uint64_t xchg_pop(void *low_top, void *high_top)
{
    uint64_t out;
    __asm__ volatile(
        "mov %%rsp, %%r12\n\t"
        "mov %[lo], %%rbx\n\t"
        "xchg %%rbx, %%rsp\n\t"
        "push $7\n\t"
        "push %[hi]\n\t"
        "mov 8(%%rsp), %%rax\n\t"
        "pop %%rsp\n\t"
        "push $11\n\t"
        "pop %%rcx\n\t"
        "add %%rcx, %%rax\n\t"
        "mov %%r12, %%rsp\n\t"
        : "=&a"(out)
        : [lo] "r"(low_top), [hi] "r"(high_top)
        : "rbx", "rcx", "r12", "memory", "cc");
    return out;
}

/* An indexed rsp operand that lands on a global, nowhere near the stack. */
static volatile uint16_t far_word = 0xbeef;

static uint64_t indexed_far(void *top)
{
    uint64_t out;
    __asm__ volatile(
        "mov %%rsp, %%r12\n\t"
        "mov %[top], %%rsp\n\t"
        "mov %[target], %%rcx\n\t"
        "sub %%rsp, %%rcx\n\t"
        "movzwl (%%rsp,%%rcx,1), %%eax\n\t"
        "mov %%r12, %%rsp\n\t"
        : "=&a"(out)
        : [top] "r"(top), [target] "r"(&far_word)
        : "rcx", "r12", "memory");
    return out;
}

int main(void)
{
    void *lo = mmap(LOW_STACK, STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    void *hi = high_stack();
    if (lo == MAP_FAILED || hi == MAP_FAILED) {
        printf("mmap failed\n");
        return 1;
    }
    char *lo_top = (char *)lo + STACK_SIZE - 64, *hi_top = (char *)hi + STACK_SIZE - 64;
    uint64_t acc = 0;
    for (uint64_t i = 0; i < 1000; i++) {
        acc += work_on(i & 1 ? lo_top : hi_top, i);
        acc ^= work_on(i & 1 ? hi_top : lo_top, acc);
    }
    printf("work %016llx\n", (unsigned long long)acc);
    printf("xchg/pop rsp %llu\n", (unsigned long long)xchg_pop(lo_top, hi_top - 32));
    printf("indexed far %04llx %04llx\n", (unsigned long long)indexed_far(lo_top),
           (unsigned long long)indexed_far(hi_top));
    return 0;
}

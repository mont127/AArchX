/* Adversarial gate for deferred flag materialization. */
#include "gsys.h"

#define AM 0x0cd5ULL

static g_u64 cap_block_a(g_u64 a, g_u64 b)
{
    g_u64 out;
    __asm__ volatile(
        "xor %%r15, %%r15\n\t"
        "mov %1, %%rcx\n\t"
        "mov %2, %%rdx\n\t"

        "add %%rdx, %%rcx\n\t"
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"

        "sub %%rdx, %%rcx\n\t"
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"

        "and %%rdx, %%rcx\n\t"
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"

        "or %%rdx, %%rcx\n\t"
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"

        "xor %%rdx, %%rcx\n\t"
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"

        "add %%rbx, %%rcx\n\t"
        "sub %%rbx, %%rcx\n\t"
        "cmp %%rdx, %%rcx\n\t"
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"

        "mov %%r15, %0\n\t"
        : "=r"(out)
        : "r"(a), "r"(b)
        : "rax", "rbx", "rcx", "rdx", "r15", "cc", "memory");
    return out;
}

static g_u64 cap_block_b(g_u64 a, g_u64 b, int seed_cf)
{
    g_u64 out;
    __asm__ volatile(
        "xor %%r15, %%r15\n\t"
        "mov %1, %%rcx\n\t"
        "mov %2, %%rdx\n\t"

        "add %%rdx, %%rdx\n\t"
        "test %3, %3\n\t"
        "jz 1f\n\t stc\n\t jmp 2f\n\t 1: clc\n\t 2:\n\t"
        "inc %%rcx\n\t"
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"
        "dec %%rcx\n\t"
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"
        "inc %%rcx\n\t"
        "dec %%rcx\n\t"
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"

        "mov %%rdx, %%rcx\n\t"
        "shl $1, %%rcx\n\t"
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"
        "shl $7, %%rcx\n\t"
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"
        "shr $1, %%rcx\n\t"
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"
        "shr $9, %%rcx\n\t"
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"
        "sar $1, %%rcx\n\t"
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"
        "sar $13, %%rcx\n\t"
        "pushfq\n\t pop %%rax\n\t and $0x0cd5, %%rax\n\t xor %%rax, %%r15\n\t"

        "mov %%r15, %0\n\t"
        : "=r"(out)
        : "r"(a), "r"(b), "r"((g_u64)seed_cf)
        : "rax", "rcx", "rdx", "r15", "cc", "memory");
    return out;
}

static g_u64 cap_block_c(g_u32 a, g_u32 b)
{
    g_u64 out;
    __asm__ volatile(
        "xor %%r15, %%r15\n\t"
        "mov %k1, %%ecx\n\t"
        "mov %k2, %%edx\n\t"

        "add %%edx, %%ecx\n\t"
        "seto %%al\n\t movzbq %%al, %%rax\n\t xor %%rax, %%r15\n\t"
        "sub %%edx, %%ecx\n\t"
        "setb %%al\n\t movzbq %%al, %%rax\n\t add %%rax, %%r15\n\t"
        "and %%edx, %%ecx\n\t"
        "sets %%al\n\t movzbq %%al, %%rax\n\t xor %%rax, %%r15\n\t"
        "shl $1, %%ecx\n\t"
        "setc %%al\n\t movzbq %%al, %%rax\n\t add %%rax, %%r15\n\t"
        "sar $5, %%ecx\n\t"
        "sets %%al\n\t movzbq %%al, %%rax\n\t xor %%rax, %%r15\n\t"
        "inc %%ecx\n\t"
        "sete %%al\n\t movzbq %%al, %%rax\n\t add %%rax, %%r15\n\t"
        "cmp %%edx, %%ecx\n\t"
        "setle %%al\n\t movzbq %%al, %%rax\n\t xor %%rax, %%r15\n\t"
        "setg %%al\n\t movzbq %%al, %%rax\n\t add %%rax, %%r15\n\t"

        "mov %%r15, %0\n\t"
        : "=r"(out)
        : "r"((g_u64)a), "r"((g_u64)b)
        : "rax", "rcx", "rdx", "r15", "cc", "memory");
    return out;
}

static g_u64 mix(g_u64 acc, g_u64 v)
{
    acc ^= v;
    acc = (acc << 13) | (acc >> 51);
    acc += 0x9e3779b97f4a7c15ULL;
    acc ^= acc >> 29;
    return acc;
}

static const g_u64 vals[] = {
    0, 1, 2, 0xffffffffffffffffULL, 0x8000000000000000ULL, 0x7fffffffffffffffULL,
    0x0fULL, 0x10ULL, 0xffULL, 0x100ULL, 0x0123456789abcdefULL, 0xfedcba9876543210ULL,
    0x00000000ffffffffULL, 0x0000000100000000ULL, 0x5555555555555555ULL, 0xaaaaaaaaaaaaaaaaULL,
    0x55ULL, 0xaaULL, 0x7fULL, 0x80ULL, 0xdeadbeefcafef00dULL, 0x0000000080000000ULL,
};
#define NV (sizeof(vals) / sizeof(vals[0]))

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    g_u64 acc = 0xcbf29ce484222325ULL;
    for (unsigned i = 0; i < NV; i++) {
        for (unsigned j = 0; j < NV; j++) {
            g_u64 a = vals[i], b = vals[j];
            acc = mix(acc, cap_block_a(a, b));
            acc = mix(acc, cap_block_b(a, b, (int)(j & 1)));
            acc = mix(acc, cap_block_c((g_u32)a, (g_u32)b));
        }
    }
    g_puthex64(acc);
    return 0;
}

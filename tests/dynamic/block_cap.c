/*
 * Straight-line code longer than a block (JIT_MAX_BLOCK_INSNS, 256 in
 * src/jit.c), where the block that stops at the cap chains to the next one
 * and leaves its flags lazy for it.  Each run is a pattern of flag producers
 * each followed by a consumer of exactly those flags (add/adc, cmp/sbb,
 * test/setcc, inc/cmov, shl/adc, sub/cmov), repeated past the cap, with no
 * branch, which would end the block before it.  The pattern is 16
 * instructions and the sixteen variants start with 0 to 15 nops, so the cap
 * falls at every point in it, and every pair is split across it in one of
 * them.  Against Rosetta.
 */
#include <stdint.h>
#include <stdio.h>

#define PAIR6 \
    "add %%rcx, %%rax\n\t" "adc $0, %%rdx\n\t" \
    "cmp %%rsi, %%rax\n\t" "sbb %%rbx, %%rbx\n\t" "add %%rbx, %%r8\n\t" \
    "test $0x10, %%al\n\t" "setnz %%r9b\n\t" "add %%r9, %%r10\n\t" \
    "inc %%r11\n\t" "cmovs %%rcx, %%r12\n\t" \
    "shl $1, %%r13\n\t" "adc $0, %%r14\n\t" \
    "sub $3, %%r15\n\t" "cmovb %%rdx, %%r15\n\t" \
    "imul $0x5bd1e995, %%rcx, %%rcx\n\t" "xor %%rax, %%rcx\n\t"
#define PAIR4 PAIR6 PAIR6 PAIR6 PAIR6
#define PAIR24 PAIR4 PAIR4 PAIR4 PAIR4 PAIR4 PAIR4

#define CAP_FN(name, nops)                                                                          \
    static void name(uint64_t *s)                                                                   \
    {                                                                                               \
        __asm__ volatile(                                                                           \
            "push %%rbx\n\t" "push %%r12\n\t" "push %%r13\n\t" "push %%r14\n\t" "push %%r15\n\t"    \
            "mov 0(%0), %%rax\n\t" "mov 8(%0), %%rcx\n\t" "mov 16(%0), %%rdx\n\t"                   \
            "mov 24(%0), %%rsi\n\t" "mov 32(%0), %%r8\n\t" "mov 40(%0), %%r10\n\t"                  \
            "mov 48(%0), %%r11\n\t" "mov 56(%0), %%r12\n\t" "mov 64(%0), %%r13\n\t"                 \
            "mov 72(%0), %%r14\n\t" "mov 80(%0), %%r15\n\t" "xor %%r9d, %%r9d\n\t"                  \
            ".rept " #nops "\n\tnop\n\t.endr\n\t"                                                    \
            PAIR24                                                                                  \
            "mov %%rax, 0(%0)\n\t" "mov %%rcx, 8(%0)\n\t" "mov %%rdx, 16(%0)\n\t"                   \
            "mov %%r8, 32(%0)\n\t" "mov %%r10, 40(%0)\n\t" "mov %%r11, 48(%0)\n\t"                  \
            "mov %%r12, 56(%0)\n\t" "mov %%r13, 64(%0)\n\t" "mov %%r14, 72(%0)\n\t"                 \
            "mov %%r15, 80(%0)\n\t"                                                                 \
            "pop %%r15\n\t" "pop %%r14\n\t" "pop %%r13\n\t" "pop %%r12\n\t" "pop %%rbx\n\t"         \
            : : "D"(s) : "rax", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11", "cc", "memory");     \
    }

CAP_FN(cap0, 0) CAP_FN(cap1, 1) CAP_FN(cap2, 2) CAP_FN(cap3, 3)
CAP_FN(cap4, 4) CAP_FN(cap5, 5) CAP_FN(cap6, 6) CAP_FN(cap7, 7)
CAP_FN(cap8, 8) CAP_FN(cap9, 9) CAP_FN(cap10, 10) CAP_FN(cap11, 11)
CAP_FN(cap12, 12) CAP_FN(cap13, 13) CAP_FN(cap14, 14) CAP_FN(cap15, 15)

int main(void)
{
    void (*fns[16])(uint64_t *) = { cap0, cap1, cap2,  cap3,  cap4,  cap5,  cap6,  cap7,
                                    cap8, cap9, cap10, cap11, cap12, cap13, cap14, cap15 };
    for (int f = 0; f < 16; f++) {
        uint64_t s[11] = { 1, 0x123456789abcdefull, 0, 0x8000000000000000ull, 0, 0, 0x7ffffffffffffff0ull,
                           0, 0xf0f0f0f0f0f0f0f1ull, 0, 100 };
        for (int r = 0; r < 20000; r++)
            fns[f](s);
        printf("cap%d", f);
        for (int k = 0; k < 11; k++)
            if (k != 3) printf(" %llx", (unsigned long long)s[k]);
        printf("\n");
    }
    return 0;
}

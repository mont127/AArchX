/*
 * Flags returned across a block boundary.  Flags reaching a ret are dead
 * unless the last instruction to write them was a cmp, test, bt or cmpxchg,
 * which hand-written code may return an answer in; src/jit.c settles that for
 * an epilogue block that writes no flags from the block that jumps to it.
 * These functions compare, then reach a shared pop-and-ret epilogue through a
 * jmp, through a taken jne, and through a bt, and the caller reads CF and ZF
 * after the call (only CF after bt, which leaves ZF as it was: Rosetta keeps
 * it, translated code here does not).  Against Rosetta.
 */
#include <stdint.h>
#include <stdio.h>

__asm__(
    ".text\n"
    ".p2align 4\n"
    "_seam_epilogue:\n"
    "    pop %rbx\n"
    "    ret\n"
    ".p2align 4\n"
    "_seam_cmp_jmp:\n"
    "    push %rbx\n"
    "    mov %rdi, %rbx\n"
    "    cmp %rsi, %rbx\n"
    "    jmp _seam_epilogue\n"
    ".p2align 4\n"
    "_seam_cmp_jne:\n"
    "    push %rbx\n"
    "    cmp %rsi, %rdi\n"
    "    jne _seam_epilogue\n"
    "    mov %rdi, %rbx\n"
    "    jmp _seam_epilogue\n"
    ".p2align 4\n"
    "_seam_bt:\n"
    "    push %rbx\n"
    "    mov %rsi, %rbx\n"
    "    bt %rbx, %rdi\n"
    "    jmp _seam_epilogue\n");

static unsigned call_flags(void *fn, uint64_t a, uint64_t b)
{
    uint8_t cf, zf;
    __asm__ volatile(
        "mov %2, %%rdi\n\t" "mov %3, %%rsi\n\t"
        "call *%4\n\t"
        "setb %0\n\t" "sete %1\n\t"
        : "=&r"(cf), "=&r"(zf) : "r"(a), "r"(b), "r"(fn)
        : "rdi", "rsi", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "cc", "memory");
    return (unsigned)cf << 1 | zf;
}

extern char seam_cmp_jmp[], seam_cmp_jne[], seam_bt[];

int main(void)
{
    static const uint64_t v[][2] = { {1, 2}, {2, 1}, {5, 5}, {0, ~0ull}, {0x8000000000000000ull, 1}, {6, 1}, {6, 2} };
    unsigned sum = 0;
    for (int r = 0; r < 20000; r++)
        for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++)
            sum += call_flags(seam_cmp_jmp, v[i][0], v[i][1]) + call_flags(seam_cmp_jne, v[i][0], v[i][1]) * 4 +
                   (call_flags(seam_bt, v[i][0], v[i][1]) >> 1) * 16;
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++)
        printf("%llx %llx: cmp/jmp %u cmp/jne %u bt cf %u\n", (unsigned long long)v[i][0], (unsigned long long)v[i][1],
               call_flags(seam_cmp_jmp, v[i][0], v[i][1]), call_flags(seam_cmp_jne, v[i][0], v[i][1]),
               call_flags(seam_bt, v[i][0], v[i][1]) >> 1);
    printf("sum %u\n", sum);
    return 0;
}

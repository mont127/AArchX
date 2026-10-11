/*
 * Flags read by several consumers well after the instruction that set them
 * (src/jit.c, nzcv_fuse_producer): the window a compare's NZCV is handed on
 * through, and which consumers may have siblings in it that write the
 * producer's operands.
 * - lz_bit: 7-Zip's LZMA range-decoder step as its compiler wrote it, a sub
 *   then two cmovae, two cmovb and an sbb up to 8 instructions on, with movs
 *   and the cmovs (which rewrite the sub's operands) between;
 * - adc_late: the same with an adc reading the carry instead of the sbb;
 * - edge: consumers at 7 and 8 instructions from a cmp, and one at 9, which
 *   is past the window and reads the flags another way;
 * - jcc_after: a jcc after cmov siblings that rewrote the compare's operands,
 *   which may not take the handed-on flags;
 * each over inputs on both sides of every condition.  Against Rosetta.
 */
#include <stdint.h>
#include <stdio.h>

__asm__(
    ".text\n"
    ".p2align 4\n"
    /* lz_bit(range, code, prob, sym) -> mixes every register the step writes */
    "_lz_bit:\n"
    "    push %rbx\n    push %r12\n    push %r13\n    push %r14\n    push %rbp\n"
    "    mov %edi, %eax\n    mov %esi, %ebp\n    mov %edx, %ecx\n    mov %ecx, %ebx\n"
    "    mov $0x111, %r12d\n    mov $0x222, %r14d\n    mov $0x333, %r9d\n    mov $0x7ff, %r10d\n"
    "    mov %eax, %edi\n"
    "    shr $0xb, %eax\n"
    "    imul %ecx, %eax\n"
    "    sub %eax, %edi\n"
    "    mov %ebp, %esi\n"
    "    sub %eax, %ebp\n"
    "    cmovae %r14d, %r12d\n"
    "    mov %r9d, %r14d\n"
    "    cmovae %edi, %eax\n"
    "    mov $0x1f, %edi\n"
    "    cmovb %esi, %ebp\n"
    "    mov %ebx, %esi\n"
    "    cmovb %r10d, %edi\n"
    "    sbb $-1, %ebx\n"
    "    shl $7, %r12\n    xor %r12, %rax\n    shl $5, %r14\n    xor %r14, %rax\n"
    "    shl $9, %rbp\n    xor %rbp, %rax\n    shl $3, %rdi\n    xor %rdi, %rax\n"
    "    shl $11, %rsi\n    xor %rsi, %rax\n    shl $13, %rbx\n    xor %rbx, %rax\n"
    "    pop %rbp\n    pop %r14\n    pop %r13\n    pop %r12\n    pop %rbx\n"
    "    ret\n"
    ".p2align 4\n"
    /* adc_late(a, b, c): sub, cmov siblings rewriting the sub's operands, an adc */
    "_adc_late:\n"
    "    push %rbx\n"
    "    mov %edi, %eax\n    mov %esi, %ecx\n    mov %edx, %ebx\n    mov $5, %r8d\n    mov $9, %r9d\n"
    "    sub %ecx, %eax\n"
    "    cmovb %r8d, %eax\n"
    "    mov %ebx, %r10d\n"
    "    cmovae %r9d, %ecx\n"
    "    mov $3, %r11d\n"
    "    cmovb %r11d, %r10d\n"
    "    adc $0x40, %ebx\n"
    "    shl $16, %rcx\n    xor %rcx, %rax\n    shl $20, %rbx\n    xor %rbx, %rax\n    shl $40, %r10\n    xor %r10, %rax\n"
    "    pop %rbx\n"
    "    ret\n"
    ".p2align 4\n"
    /* edge(a, b): consumers 7, 8 and 9 instructions after a cmp */
    "_edge:\n"
    "    xor %eax, %eax\n    xor %ecx, %ecx\n    xor %edx, %edx\n    mov $1, %r8d\n"
    "    cmp %esi, %edi\n"
    "    mov %edi, %r9d\n    mov %esi, %r10d\n    mov $7, %r11d\n    lea 1(%r8), %r8d\n    mov %r9d, %r11d\n    lea 2(%r10), %r10d\n"
    "    cmovl %r8d, %eax\n"
    "    setg %cl\n"
    "    setle %dl\n"
    "    shl $8, %rcx\n    or %rcx, %rax\n    shl $16, %rdx\n    or %rdx, %rax\n"
    "    shl $24, %r10\n    xor %r10, %rax\n    shl $40, %r11\n    xor %r11, %rax\n"
    "    ret\n"
    ".p2align 4\n"
    /* jcc_after(a, b): a jcc after cmovs that rewrote the compare's operands */
    "_jcc_after:\n"
    "    mov %edi, %eax\n    mov %esi, %ecx\n    mov $0x55, %r8d\n    mov $0x66, %r9d\n"
    "    cmp %ecx, %eax\n"
    "    cmovb %r8d, %eax\n"
    "    cmovae %r9d, %ecx\n"
    "    jb 1f\n"
    "    lea 0x1000(%rax,%rcx), %eax\n"
    "1:  shl $20, %rcx\n    xor %rcx, %rax\n"
    "    ret\n");

uint64_t lz_bit(uint32_t range, uint32_t code, uint32_t prob, uint32_t sym);
uint64_t adc_late(uint32_t a, uint32_t b, uint32_t c);
uint64_t edge(uint32_t a, uint32_t b);
uint64_t jcc_after(uint32_t a, uint32_t b);

int main(void)
{
    uint64_t s = 0x9e3779b97f4a7c15ull, h[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 400000; i++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        uint32_t x = (uint32_t)(s >> 32), y = (uint32_t)s;
        uint32_t range = x | 0x1000000u, code = y % range, prob = (x >> 20) & 0x7ff;
        h[0] = h[0] * 31 + lz_bit(range, code, prob, y & 0xff);
        h[1] = h[1] * 31 + adc_late(x, (i & 1) ? x ^ (y & 15) : y, y >> 7);
        h[2] = h[2] * 31 + edge((int32_t)x >> (i & 7), (int32_t)y >> (i & 5));
        h[3] = h[3] * 31 + jcc_after(x & 0xffff, y & 0xffff);
    }
    printf("lz_bit %llx adc_late %llx edge %llx jcc_after %llx\n", (unsigned long long)h[0],
           (unsigned long long)h[1], (unsigned long long)h[2], (unsigned long long)h[3]);
    return 0;
}

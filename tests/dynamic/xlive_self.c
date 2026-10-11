/*
 * Flags live at the start of a block that loops to itself (src/jit.c,
 * xlive_decode_entry_d): the least fixed point over the loop's own edge, so
 * an inc or dec closing a loop keeps CF in its flag record only where some
 * path reads it.
 * - copy_cf: 7-Zip's LZ copy loop (mov byte; movzx; inc rdx; jne) with a
 *   negative index, after a cmp whose CF a setc reads past the loop;
 * - copy_jmp: the cmp in a block that jumps to the loop, which keeps the
 *   cmp's CF only if the loop's start says the exit reads it;
 * - copy_dead: the same loop with CF dead after it, as 7-Zip has it;
 * - adc_head: an adc at the loop's head reading the carry the last trip's
 *   adc left, across lea and dec, and nothing reading CF after the loop;
 * - add_inc: an add then an inc closing the loop, its CF read by a setc
 *   after the loop, so only the exit keeps it;
 * each over lengths and inputs on both sides of every condition.  Against
 * Rosetta.
 */
#include <stdint.h>
#include <stdio.h>

__asm__(
    ".text\n"
    ".p2align 4\n"
    /* copy_cf(dst_end, src_end, n, a, b) -> copied bytes' sum mixed with CF of a < b */
    "_copy_cf:\n"
    "    push %rbx\n"
    "    mov %rdx, %r9\n    neg %rdx\n    xor %eax, %eax\n    xor %ebx, %ebx\n"
    "    cmp %r8d, %ecx\n"
    "1:  mov %bl, (%rdi,%rdx)\n"
    "    movzbl (%rsi,%rdx), %ebx\n"
    "    inc %rdx\n"
    "    jne 1b\n"
    "    setc %al\n"
    "    shl $8, %rax\n    or %rbx, %rax\n    shl $16, %r9\n    xor %r9, %rax\n"
    "    pop %rbx\n"
    "    ret\n"
    ".p2align 4\n"
    /* copy_jmp(dst_end, src_end, n, a, b): as copy_cf, the loop reached by a jmp */
    "_copy_jmp:\n"
    "    push %rbx\n"
    "    mov %rdx, %r9\n    neg %rdx\n    xor %eax, %eax\n    xor %ebx, %ebx\n"
    "    cmp %r8d, %ecx\n"
    "    jmp 1f\n"
    "    .p2align 4\n"
    "1:  mov %bl, (%rdi,%rdx)\n"
    "    movzbl (%rsi,%rdx), %ebx\n"
    "    inc %rdx\n"
    "    jne 1b\n"
    "    setc %al\n"
    "    shl $8, %rax\n    or %rbx, %rax\n    shl $16, %r9\n    xor %r9, %rax\n"
    "    pop %rbx\n"
    "    ret\n"
    ".p2align 4\n"
    /* copy_dead(dst_end, src_end, n) */
    "_copy_dead:\n"
    "    push %rbx\n"
    "    neg %rdx\n    xor %ebx, %ebx\n"
    "1:  mov %bl, (%rdi,%rdx)\n"
    "    movzbl (%rsi,%rdx), %ebx\n"
    "    inc %rdx\n"
    "    jne 1b\n"
    "    mov %rbx, %rax\n"
    "    pop %rbx\n"
    "    ret\n"
    ".p2align 4\n"
    /* adc_head(p, n, seed) -> the sum with carries of seed twice and n words */
    "_adc_head:\n"
    "    mov %rdx, %rax\n    mov %esi, %ecx\n"
    "    add %rdx, %rax\n"
    "1:  adc (%rdi), %rax\n"
    "    lea 8(%rdi), %rdi\n"
    "    dec %ecx\n"
    "    jne 1b\n"
    "    ret\n"
    ".p2align 4\n"
    /* add_inc(p, n_neg, a) -> sum of p[n..0) and the last add's carry */
    "_add_inc:\n"
    "    mov %rdx, %rax\n    xor %ecx, %ecx\n"
    "1:  add (%rdi,%rsi,8), %rax\n"
    "    inc %rsi\n"
    "    jne 1b\n"
    "    setc %cl\n"
    "    shl $1, %rax\n    or %rcx, %rax\n"
    "    ret\n");

uint64_t copy_cf(uint8_t *dst_end, const uint8_t *src_end, uint64_t n, uint32_t a, uint32_t b);
uint64_t copy_jmp(uint8_t *dst_end, const uint8_t *src_end, uint64_t n, uint32_t a, uint32_t b);
uint64_t copy_dead(uint8_t *dst_end, const uint8_t *src_end, uint64_t n);
uint64_t adc_head(const uint64_t *p, uint32_t n, uint64_t seed);
uint64_t add_inc(const uint64_t *p_end, int64_t n_neg, uint64_t a);

int main(void)
{
    static uint8_t src[300], dst[300];
    static uint64_t words[64];
    uint64_t s = 0x9e3779b97f4a7c15ull, h[5] = { 0, 0, 0, 0, 0 };
    static const uint32_t edge[] = { 0, 1, 0x7fffffff, 0x80000000u, 0xffffffffu };
    for (int i = 0; i < 200000; i++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        uint32_t x = (uint32_t)(s >> 32), y = (uint32_t)s;
        for (int k = 0; k < 8; k++) src[(i * 8 + k) % 300] = (uint8_t)(s >> (8 * k));
        for (int k = 0; k < 4; k++) words[(i * 4 + k) & 63] = s * (uint64_t)(k + 3) | ((i & 3) == 0 ? 0xffffffff00000000ull : 0);
        uint64_t n = 1 + (x % 64);
        if (i & 1) x = edge[(y >> 3) % 5];
        if (i & 2) y = (i & 4) ? x : edge[(y >> 7) % 5];
        h[0] = h[0] * 31 + copy_cf(dst + 128 + n, src + 128 + n, n, x, y);
        h[4] = h[4] * 31 + copy_jmp(dst + 192 + n, src + 192 + n, n, y, x);
        h[1] = h[1] * 31 + copy_dead(dst + 64 + n, src + 64 + n, n) + dst[64 + n - 1];
        h[2] = h[2] * 31 + adc_head(words, 1 + (y % 64), (i & 8) ? ~0ull - x : s);
        h[3] = h[3] * 31 + add_inc(words + 64, -(int64_t)(1 + (x % 64)), (i & 16) ? ~0ull - y : y);
    }
    printf("copy_cf %llx copy_jmp %llx copy_dead %llx adc_head %llx add_inc %llx\n", (unsigned long long)h[0],
           (unsigned long long)h[4], (unsigned long long)h[1], (unsigned long long)h[2], (unsigned long long)h[3]);
    return 0;
}

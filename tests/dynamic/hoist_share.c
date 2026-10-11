/*
 * Loops whose two memory operands share an index register, which the Wine
 * layout's hoist (src/jit.c, select_low_hoist) checks once a time round
 * rather than at each operand:
 * - copy_up: 7-Zip's LZ copy loop, mov [r14+rdx], bl; movzx ebx,
 *   [rdi+rdx]; inc rdx; jne, rdx running up from below zero;
 * - cmp_bytes: its match finder's byte compare, movzx ecx, [rax+r9]; cmp
 *   [rax+r8], cl; jne; inc eax; cmp eax, r10d; jne;
 * - sum2: two loads off one index, first entered with a small index and
 *   then with one below zero and one past 2 GB from the bases, so the
 *   check made for the first fails later and the loop goes on another way;
 * - sum2b: the same, its bases in the image and the index reaching a malloc'd
 *   buffer, which that layout places above the low window: only the index's
 *   check keeps the hoisted base from being used for it.
 * Built at an image base below 12 GB so the data is in the low window.  Each
 * prints a checksum, against Rosetta.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t src[4096], dst[4096], cmpa[4096], cmpb[4096];

__asm__(
    ".text\n"
    ".p2align 4\n"
    /* copy_up(dst_end, src_end, n): dst_end[-n..0) = previous byte copied, the byte loaded each step */
    "_copy_up:\n"
    "    push %rbx\n    push %r14\n"
    "    mov %rdi, %r14\n    mov %rsi, %rdi\n    neg %rdx\n    xor %ebx, %ebx\n"
    "    .p2align 4\n"
    "1:  mov %bl, (%r14,%rdx)\n"
    "    movzbl (%rdi,%rdx), %ebx\n"
    "    inc %rdx\n"
    "    jne 1b\n"
    "    mov %rbx, %rax\n"
    "    pop %r14\n    pop %rbx\n"
    "    ret\n"
    ".p2align 4\n"
    /* cmp_bytes(a, b, start, limit) -> first index in [start, limit) where a and b differ, or limit */
    "_cmp_bytes:\n"
    "    mov %rdi, %r9\n    mov %rsi, %r8\n    mov %edx, %eax\n    mov %ecx, %r10d\n"
    "    .p2align 4\n"
    "1:  movzbl (%rax,%r9), %ecx\n"
    "    cmpb %cl, (%rax,%r8)\n"
    "    jne 2f\n"
    "    inc %eax\n"
    "    cmp %r10d, %eax\n"
    "    jne 1b\n"
    "2:  ret\n"
    ".p2align 4\n"
    /* sum2(a, b, i, n) -> sum over n steps of a[i] * 3 + b[i], i counting up */
    "_sum2:\n"
    "    xor %eax, %eax\n"
    "    .p2align 4\n"
    "1:  movzbl (%rdi,%rdx), %r8d\n"
    "    movzbl (%rsi,%rdx), %r9d\n"
    "    lea (%r8,%r8,2), %r8\n"
    "    add %r8, %rax\n"
    "    add %r9, %rax\n"
    "    inc %rdx\n"
    "    dec %rcx\n"
    "    jne 1b\n"
    "    ret\n"
    ".p2align 4\n"
    /* sum2b: sum2 again in other registers, so the linker keeps it a block of its own */
    "_sum2b:\n"
    "    xor %eax, %eax\n"
    "    .p2align 4\n"
    "1:  movzbl (%rdi,%rdx), %r10d\n"
    "    movzbl (%rsi,%rdx), %r11d\n"
    "    lea (%r10,%r10,2), %r10\n"
    "    add %r10, %rax\n"
    "    add %r11, %rax\n"
    "    inc %rdx\n"
    "    dec %rcx\n"
    "    jne 1b\n"
    "    ret\n");

uint64_t copy_up(uint8_t *dst_end, const uint8_t *src_end, uint64_t n);
uint32_t cmp_bytes(const uint8_t *a, const uint8_t *b, uint32_t start, uint32_t limit);
uint64_t sum2(const uint8_t *a, const uint8_t *b, int64_t i, uint64_t n);
uint64_t sum2b(const uint8_t *a, const uint8_t *b, int64_t i, uint64_t n);

int main(void)
{
    uint64_t s = 0x9e3779b97f4a7c15ull, h[3] = { 0, 0, 0 };
    for (int k = 0; k < 4096; k++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        src[k] = (uint8_t)(s >> 33);
        cmpa[k] = cmpb[k] = (uint8_t)(s >> 41);
    }
    for (int i = 0; i < 20000; i++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t n = 1 + (s >> 40) % 2000, at = 2048 + (s >> 20) % 2000;
        h[0] = h[0] * 31 + copy_up(dst + at, src + at, n);
        for (uint64_t k = at - n; k < at; k++) h[0] = h[0] * 7 + dst[k];
        uint32_t start = (uint32_t)((s >> 8) % 1000), limit = start + 1 + (uint32_t)((s >> 24) % 3000);
        if (limit > 4096) limit = 4096;
        uint32_t flip = start + (uint32_t)((s >> 50) % (limit - start + 200));
        if (flip < 4096) cmpb[flip] ^= 0x5a;
        h[1] = h[1] * 31 + cmp_bytes(cmpa, cmpb, start, limit);
        if (flip < 4096) cmpb[flip] ^= 0x5a;
    }
    /* The same loop entered with indices of every kind: small, below zero, and 2 GB past its bases. */
    for (int i = 0; i < 3000; i++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t n = 1 + (s >> 40) % 1000, at = (s >> 20) % 3000;
        int64_t off;
        switch (i < 1000 ? 0 : i % 3) {
        case 0: off = 0; break;
        case 1: off = -(int64_t)0x80001000; break;
        default: off = 0x80001000; break;
        }
        h[2] = h[2] * 31 + sum2(src - off, cmpa - off, (int64_t)at + off, n);
    }
    printf("copy_up %llx cmp_bytes %llx sum2 %llx\n", (unsigned long long)h[0], (unsigned long long)h[1],
           (unsigned long long)h[2]);

    uint8_t *hi = malloc(0x10000);
    if (!hi) return 1;
    for (int k = 0; k < 0x10000; k++) hi[k] = (uint8_t)(k * 37 + (k >> 8));
    uint64_t hb = 0;
    for (int i = 0; i < 2000; i++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t n = 1 + (s >> 40) % 1000, at = (s >> 20) % 3000;
        int64_t idx = i < 1000 ? (int64_t)at : (int64_t)((uintptr_t)hi - (uintptr_t)src) + (int64_t)at;
        hb = hb * 31 + sum2b(src, cmpa, idx, n);
    }
    printf("sum2b %llx\n", (unsigned long long)hb);
    return 0;
}

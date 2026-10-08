/*
 * Every access shape through a base the Wine layout hoists (select_low_hoist),
 * as src/jit.c folds the displacement into the access (low_hoist_covers):
 * loads and stores of each size, arithmetic, compares and read-modify-writes
 * with a memory operand, movzx/movsx/movsxd, xmm loads and stores and a
 * movups pair, at displacements the scaled encodings take, odd ones only the
 * unscaled take, and negative ones.  Built at an image base below 12 GB, so
 * the data is in the low window; prints each checksum, against Rosetta.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct rec { uint64_t a; uint32_t b, c; uint16_t d, e; uint8_t f, g, h, pad; float v[4]; double w[2]; int32_t s; uint8_t tail[13]; };
static struct rec rs[512];

/* One loop in asm, the pointer stepped at its end: the shape the hoist takes. */
__attribute__((noinline)) static uint64_t walk(int rounds)
{
    uint64_t acc = 0;
    for (int r = 0; r < rounds; r++) {
        struct rec *p = rs + 1, *end = rs + 511;
        uint64_t t = 0;
        __asm__ volatile(
            "1:\n\t"
            "mov 0(%[p]), %[t]\n\t"
            "add 8(%[p]), %k[t]\n\t"
            "xor 12(%[p]), %k[t]\n\t"
            "addq %[t], 0(%[p])\n\t"
            "subl $3, 8(%[p])\n\t"
            "movzwl 16(%[p]), %%eax\n\t"  "add %%rax, %[t]\n\t"
            "movswq 18(%[p]), %%rax\n\t"  "add %%rax, %[t]\n\t"
            "movzbl 20(%[p]), %%eax\n\t"  "add %%rax, %[t]\n\t"
            "movsbl 21(%[p]), %%eax\n\t"  "add %%rax, %[t]\n\t"
            "incb 22(%[p])\n\t"
            "movslq 64(%[p]), %%rax\n\t"  "add %%rax, %[t]\n\t"
            "mov 69(%[p]), %%eax\n\t"     "add %%rax, %[t]\n\t"
            "movl %k[t], 73(%[p])\n\t"
            "cmpl $7, 12(%[p])\n\t"       "adc $0, %[t]\n\t"
            "cmp %[t], -16(%[p])\n\t"     "sbb $0, %[t]\n\t"
            "movups 24(%[p]), %%xmm0\n\t"
            "movups 40(%[p]), %%xmm1\n\t"
            "addps %%xmm0, %%xmm0\n\t"
            "movups %%xmm0, 24(%[p])\n\t"
            "movups %%xmm1, 40(%[p])\n\t"
            "movsd 48(%[p]), %%xmm2\n\t"
            "addsd -40(%[p]), %%xmm2\n\t"
            "movsd %%xmm2, 48(%[p])\n\t"
            "movss -52(%[p]), %%xmm3\n\t"
            "movss %%xmm3, 28(%[p])\n\t"
            "movw %%ax, 18(%[p])\n\t"
            "movb %%al, 23(%[p])\n\t"
            "imul $31, %[acc], %[acc]\n\t"
            "add %[t], %[acc]\n\t"
            "add $80, %[p]\n\t"
            "cmp %[end], %[p]\n\t"
            "jne 1b"
            : [t] "+&r"(t), [p] "+&r"(p), [acc] "+&r"(acc)
            : [end] "r"(end)
            : "rax", "xmm0", "xmm1", "xmm2", "xmm3", "memory", "cc");
    }
    return acc;
}

int main(void)
{
    for (int i = 0; i < 512; i++) {
        struct rec *p = &rs[i];
        p->a = 0x0123456789abcdefull * (uint64_t)(i + 1);
        p->b = (uint32_t)i * 2654435761u; p->c = (uint32_t)i ^ 0x5a5a5a5a;
        p->d = (uint16_t)(i * 7); p->e = (uint16_t)(0x8000 | i); p->f = (uint8_t)i; p->g = (uint8_t)(0x80 | i);
        p->h = 0; p->pad = 0;
        for (int k = 0; k < 4; k++) p->v[k] = (float)(i + k) * 0.25f;
        p->w[0] = i * 1.5; p->w[1] = -i * 0.5;
        p->s = -i * 1000;
        for (int k = 0; k < 13; k++) p->tail[k] = (uint8_t)(i + k);
    }
    printf("sizeof %zu\n", sizeof(struct rec));
    printf("walk %016llx\n", (unsigned long long)walk(40));
    uint64_t h = 0;
    const uint8_t *bytes = (const uint8_t *)rs;
    for (size_t k = 0; k < sizeof rs; k++) h = h * 1000003 + bytes[k];
    printf("mem %016llx\n", (unsigned long long)h);
    return 0;
}

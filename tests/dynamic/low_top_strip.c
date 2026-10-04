#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

static uint64_t walk(uint64_t base, long n)
{
    uint64_t acc = 0;
    __asm__ volatile("1:\n\t"
                     "movq -0x20(%1), %%rax\n\t"
                     "addq %%rax, %0\n\t"
                     "movq 0xff0(%1), %%rax\n\t"
                     "xorq %%rax, %0\n\t"
                     "addl $1, 0x10(%1)\n\t"
                     "movl 0x7f8(%1), %%eax\n\t"
                     "addq %%rax, %0\n\t"
                     "rolq $7, %0\n\t"
                     "decq %2\n\t"
                     "jnz 1b"
                     : "+r"(acc), "+r"(base), "+r"(n) : : "rax", "cc", "memory");
    return acc;
}

static uint64_t walk_ref(uint64_t base, long n)
{
    volatile uint64_t *q = (volatile uint64_t *)base;
    volatile uint32_t *w = (volatile uint32_t *)base;
    uint64_t acc = 0;
    while (n--) {
        acc += q[-4];
        acc ^= q[0xff0 / 8];
        w[4] += 1;
        acc += w[0x7f8 / 4];
        acc = acc << 7 | acc >> 57;
    }
    return acc;
}

int main(void)
{
    uint64_t top = 0x7ffffe100000ull;
    void *p = mmap((void *)top, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p != MAP_FAILED) {
        uint64_t *t = p;
        for (size_t i = 0; i < 0x10000 / 8; i++)
            t[i] = 3 * 0x9e3779b97f4a7c15ull + i * 0x100000001b3ull;
        for (uint64_t off = 0x20; off <= 0x8000; off += 0x3fe0) {
            uint32_t before = *(volatile uint32_t *)(top + off + 0x10);
            uint64_t a = walk(top + off, 5000);
            *(volatile uint32_t *)(top + off + 0x10) = before;
            if (a != walk_ref(top + off, 5000))
                printf("top+%#llx differs\n", (unsigned long long)off);
        }
    }
    printf("done\n");
    return 0;
}

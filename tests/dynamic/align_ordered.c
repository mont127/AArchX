#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned char buf[512] __attribute__((aligned(64)));

int main(void)
{
    uint64_t h = 0;
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < (int)sizeof buf; i++)
            buf[i] = (unsigned char)(i * 7 + 3 + round);
        for (int off = 0; off < 48; off++) {
            unsigned char *p = buf + off;
            uint64_t a, b, c;
            __asm__ volatile("movq (%1), %0" : "=r"(a) : "r"(p) : "memory");
            __asm__ volatile("movl 4(%1), %k0" : "=r"(b) : "r"(p) : "memory");
            __asm__ volatile("movzwl 6(%1), %k0" : "=r"(c) : "r"(p) : "memory");
            __asm__ volatile("movq %1, 128(%0)\n\tmovl %k2, 140(%0)\n\tmovw %w3, 150(%0)"
                             : : "r"(p), "r"(a ^ 0x5555555555555555ull), "r"(b + 1), "r"(c + 2) : "memory");
            __asm__ volatile("addq %1, 256(%0)\n\taddl %k1, 268(%0)\n\taddw %w1, 278(%0)"
                             : : "r"(p), "r"(a) : "memory");
            uint64_t m = 0x1111111111111111ull * (uint64_t)off, k = (uint64_t)off;
            __asm__ volatile("cmpq $24, %2\n\tcmovbq 1(%1), %0" : "+r"(m) : "r"(p), "r"(k) : "cc", "memory");
            uint32_t m32 = (uint32_t)off;
            __asm__ volatile("cmpq $24, %2\n\tcmoval 3(%1), %0" : "+r"(m32) : "r"(p), "r"(k) : "cc", "memory");
            uint64_t sum = 0;
            __asm__ volatile("cmpq $24, %2\n\tadcq 5(%1), %0\n\tadcq $0, %0" : "+r"(sum) : "r"(p), "r"(k) : "cc", "memory");
            h = h * 0x100000001b3ull ^ a ^ (b << 1) ^ (c << 3) ^ m ^ ((uint64_t)m32 << 7) ^ (sum << 11);
        }
        for (size_t i = 0; i < sizeof buf; i++)
            h = h * 0x100000001b3ull ^ buf[i];
    }
    printf("%016llx\n", (unsigned long long)h);
    return 0;
}

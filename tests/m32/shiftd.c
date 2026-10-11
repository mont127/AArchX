/* Shld/shrd on 32 bits with a cl count and a memory destination, every count */
#include <stdio.h>

int main(void)
{
    unsigned sum = 0, mem[2] = { 0x12345678u, 0x9abcdef0u };
    for (unsigned rep = 0; rep < 2000; rep++)
        for (unsigned n = 0; n < 64; n++) {   /* counts above 31 are masked */
            unsigned a = 0x80000001u ^ rep, b2 = 0x0f0f0f0fu + n, c = a, d = b2;
            __asm__ volatile("shldl %%cl, %2, %0" : "+r"(a) : "c"(n), "r"(b2) : "cc");
            __asm__ volatile("shrdl %%cl, %2, %0" : "+r"(c) : "c"(n), "r"(d) : "cc");
            __asm__ volatile("shldl $13, %1, (%0)\n\tshrdl %%cl, %1, 4(%0)" :: "r"(mem), "r"(a), "c"(n) : "cc", "memory");
            sum = sum * 31 + a + c * 7 + mem[0] + mem[1];
        }
    printf("shiftd %08x %08x %08x\n", sum, mem[0], mem[1]);
    return 0;
}

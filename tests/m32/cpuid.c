/* The EFLAGS.ID toggle i386 code uses to see whether CPUID exists (Bink's check leaves the stack unbalanced
 * when the bit will not change: popf must keep it), and CPUID reporting MMX/SSE/SSE2. */
#include <stdio.h>

int main(void)
{
    unsigned before, after, d;
    __asm__ volatile("pushfl; popl %0; movl %0, %1; xorl $0x200000, %1; pushl %1; popfl; pushfl; popl %1; pushl %0; popfl"
                     : "=&r"(before), "=&r"(after));
    __asm__ volatile("cpuid" : "=d"(d) : "a"(1) : "ebx", "ecx");
    printf("id toggles %d mmx %d sse %d sse2 %d\n", ((before ^ after) >> 21) & 1, (d >> 23) & 1, (d >> 25) & 1,
           (d >> 26) & 1);
    return 0;
}

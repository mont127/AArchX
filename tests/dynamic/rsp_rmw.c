#include <stdint.h>
#include <stdio.h>

int main(void)
{
    uint64_t acc = 0;
    for (uint32_t i = 0; i < 200000; i++) {
        uint32_t a = i * 2654435761u, r;
        uint64_t q;
        __asm__ volatile("subq $64, %%rsp\n\t"
                         "movl %2, 8(%%rsp)\n\t"
                         "movq $0x1234, 16(%%rsp)\n\t"
                         "movq $0, 24(%%rsp)\n\t"
                         "movw $7, 24(%%rsp)\n\t"
                         "movb $9, 26(%%rsp)\n\t"
                         "addl %2, 8(%%rsp)\n\t"
                         "lock orl $0, (%%rsp)\n\t"
                         "subq $1, 16(%%rsp)\n\t"
                         "xorw %w2, 24(%%rsp)\n\t"
                         "andb $0x3c, 26(%%rsp)\n\t"
                         "incl 8(%%rsp)\n\t"
                         "notq 16(%%rsp)\n\t"
                         "cmpl %2, 8(%%rsp)\n\t"
                         "setb %b0\n\t"
                         "movzbl %b0, %0\n\t"
                         "addl %0, 8(%%rsp)\n\t"
                         "lock xaddl %2, 8(%%rsp)\n\t"
                         "lock addq $0x20, 16(%%rsp)\n\t"
                         "xchgl %2, 8(%%rsp)\n\t"
                         "movl %2, %%eax\n\t"
                         "lock cmpxchgl %2, 8(%%rsp)\n\t"
                         "movl 8(%%rsp), %0\n\t"
                         "movq 16(%%rsp), %1\n\t"
                         "addq 24(%%rsp), %1\n\t"
                         "addq $64, %%rsp"
                         : "=&r"(r), "=&r"(q), "+r"(a) : : "rax", "cc", "memory");
        acc = acc * 31 + r + q + a;
    }
    printf("%016llx\n", (unsigned long long)acc);
    return 0;
}

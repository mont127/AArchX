/* i386 call/ret through the JIT's host return-address stack: PIC's
 * `call 1f; 1: pop`, calls that never return (their host entries must not grow the host stack without bound),
 * deep recursion past the 256 KB guard, and returns that do not match their call (push/ret, a skipped frame). */
#include <stdio.h>

static unsigned depth(unsigned n) { return n ? depth(n - 1) + (n & 7) : 0; }   /* not a tail call: + after it */

int main(int argc, char **argv)
{
    unsigned pic = 0, lost = 0, tramp = 0, only = argc > 1 ? (unsigned)(argv[1][0] - '0') : 0;
    for (unsigned i = 0; i < 1000000 && (!only || only == 1); i++) {
        unsigned a;
        __asm__ volatile("call 1f\n1:\tpopl %0" : "=r"(a));                              /* PIC */
        pic += a & 1;
    }
    for (unsigned i = 0; i < 1000000 && (!only || only == 2); i++)   /* alone: a mismatched ret below would drop each entry again */
        __asm__ volatile("call 2f\n\tud2\n2:\taddl $4, %%esp\n\tincl %0" : "+r"(lost) :: "cc");   /* never returns */
    for (unsigned i = 0; i < 1000000 && (!only || only == 3); i++)
        __asm__ volatile("pushl $3f\n\tret\n\tud2\n3:\tincl %0" : "+r"(tramp) :: "cc");         /* ret without call */
    unsigned d = 0;
    for (int r = 0; r < 20 && (!only || only == 4); r++)
        d += depth(40000);
    printf("ras pic %u lost %u tramp %u depth %u\n", pic, lost, tramp, d);
    return 0;
}

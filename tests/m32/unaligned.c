/* Memory arithmetic and compares on addresses that cross a 16-byte granule, which the JIT's ordered loads
 * and stores fault on (src/jit.c recovers by rebuilding the block with alignment guards), and plain loads and stores across one. */
#include <stdio.h>
#include <string.h>

static unsigned char g_buf[64] __attribute__((aligned(16)));

int main(void)
{
    unsigned *p = (unsigned *)(g_buf + 14);   /* bytes 14..17: across the boundary at 16 */
    unsigned short *h = (unsigned short *)(g_buf + 31);
    unsigned *q = (unsigned *)(g_buf + 45);   /* bytes 45..48, and 47..48 for the word store */
    unsigned n = 0, s = 0;
    memset(g_buf, 0, sizeof g_buf);
    for (int i = 0; i < 100000; i++) {
        __asm__ volatile("addl $3, (%0)\n\tsubl $1, (%0)\n\tincl (%0)\n\tdecl (%0)" :: "r"(p) : "cc", "memory");
        __asm__ volatile("addw $1, (%0)" :: "r"(h) : "cc", "memory");
        __asm__ volatile("cmpl $0, (%1)\n\tje 1f\n\tincl %0\n1:" : "+r"(n) : "r"(p) : "cc", "memory");
        unsigned x;   /* plain loads and stores through a register, across the boundary at 48 */
        __asm__ volatile("movl (%1), %0\n\taddl $5, %0\n\tmovl %0, (%1)\n\tmovw %w0, 2(%1)" : "=&r"(x) : "r"(q) : "cc", "memory");
        s += x;
    }
    unsigned v;
    memcpy(&v, p, 4);
    printf("value %u short %u nonzero %u plain %u\n", v, (unsigned)*h, n, s);
    return 0;
}

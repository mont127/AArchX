/*
 * Misaligned GUEST accesses under the ordered (x86-TSO) memory tier.
 *
 * LDAPR/STLR alignment-fault, so an ordered access compiles to an alignment test
 * plus a rare unaligned arm that falls back to a fenced plain LDR/STR. That arm is
 * emitted AFTER the block epilogue so the aligned case falls through instead of
 * reaching its code by a taken branch -- worth 37% on the ordered path. This test
 * is what proves the relocated arm is still entered and still correct: it stores
 * and reads back 8- and 4-byte values at every misaligned offset, so a mispatched
 * branch, a clobbered operand register, or a lost value fails it immediately.
 */
#include "gsys.h"
static char buf[128];
int main(void)
{
    for (int off = 1; off < 8; off++) {
        volatile g_u64 *p8 = (volatile g_u64 *)(buf + off);
        volatile g_u32 *p4 = (volatile g_u32 *)(buf + off + 16);
        *p8 = 0x1122334455667788ull + (g_u64)off;
        *p4 = 0xdeadbe00u + (g_u32)off;
        if (*p8 != 0x1122334455667788ull + (g_u64)off) sys_exit(10 + off);
        if (*p4 != (0xdeadbe00u + (g_u32)off)) sys_exit(20 + off);
    }
    sys_write(1, "unaligned ok\n", 13);
    sys_exit(0);
    return 0;
}

/* Misaligned guest accesses under the ordered memory tier. */
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

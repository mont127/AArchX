/* Large working set: mmap 64MB and touch it in 4KB strides. */
#include "gsys.h"

#define TOTAL (64UL * 1024UL * 1024UL)
#define STRIDE 4096UL
#define NPAGES (TOTAL / STRIDE)

static g_u64 pageword(g_u64 idx)
{
    g_u64 v = idx * 0x9e3779b97f4a7c15ULL;
    v ^= v >> 29;
    v += 0x1234567 + idx;
    return v;
}

int main(int argc, char **argv, char **envp)
{
    unsigned char *base = (unsigned char *)sys_mmap(0, TOTAL,
                                                    PROT_READ | PROT_WRITE,
                                                    MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) {
        g_puts("memstress fail\n");
        return 1;
    }

    for (g_u64 i = 0; i < NPAGES; i++) {
        volatile g_u64 *slot = (volatile g_u64 *)(base + i * STRIDE);
        *slot = pageword(i);
    }

    g_u64 checksum = 1469598103934665603ULL;
    for (g_u64 i = 0; i < NPAGES; i++) {
        volatile g_u64 *slot = (volatile g_u64 *)(base + i * STRIDE);
        g_u64 got = *slot;
        if (got != pageword(i)) {
            g_puts("memstress fail\n");
            sys_munmap(base, TOTAL);
            return 1;
        }
        checksum ^= got;
        checksum *= 1099511628211ULL;
    }

    for (g_u64 i = 0; i < NPAGES; i += 977UL) {
        volatile g_u64 *slot = (volatile g_u64 *)(base + i * STRIDE);
        if (*slot != pageword(i)) {
            g_puts("memstress fail\n");
            sys_munmap(base, TOTAL);
            return 1;
        }
    }

    if (sys_munmap(base, TOTAL) != 0) {
        g_puts("memstress fail\n");
        return 1;
    }

    g_putu64(checksum);
    return 0;
}

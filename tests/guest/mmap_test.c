/* Guest mmap and munmap through the arena. */
#include "gsys.h"

#define ONE_MB (1024UL * 1024UL)
#define FIXED_ADDR 0x400000000UL
#define FIXED_LEN (256UL * 1024UL)

static unsigned char pat(g_u64 i)
{
    return (unsigned char)((i * 73UL + (i >> 5) * 19UL + 11UL) & 0xff);
}

int main(int argc, char **argv, char **envp)
{
    unsigned char *p = (unsigned char *)sys_mmap(0, ONE_MB,
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        g_puts("anon fail\n");
        return 1;
    }
    for (g_u64 i = 0; i < ONE_MB; i++)
        p[i] = pat(i);
    for (g_u64 i = 0; i < ONE_MB; i++) {
        if (p[i] != pat(i)) {
            g_puts("anon fail\n");
            return 1;
        }
    }
    if (sys_munmap(p, ONE_MB) != 0) {
        g_puts("anon fail\n");
        return 1;
    }
    g_puts("anon ok\n");

    unsigned char *f = (unsigned char *)sys_mmap((void *)FIXED_ADDR, FIXED_LEN,
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (f == MAP_FAILED || (g_u64)f != FIXED_ADDR) {
        g_puts("fixed fail\n");
        return 1;
    }
    for (g_u64 i = 0; i < FIXED_LEN; i++)
        f[i] = pat(i + 12345UL);
    for (g_u64 i = 0; i < FIXED_LEN; i++) {
        if (f[i] != pat(i + 12345UL)) {
            g_puts("fixed fail\n");
            return 1;
        }
    }
    if (sys_munmap(f, FIXED_LEN) != 0) {
        g_puts("fixed fail\n");
        return 1;
    }
    g_puts("fixed ok\n");

    g_puts("mmap ok\n");
    return 0;
}

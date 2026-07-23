/* End-to-end MAP_SHARED memory-mode transition after plain blocks are warm. */
#include "gsys.h"

#define SYS_fork 2
#define SYS_wait4 7

static volatile g_u64 warm[64];

static g_i64 guest_fork(g_u64 *is_child)
{
    register g_i64 rax __asm__("rax") = SYS(SYS_fork);
    register g_i64 rdx __asm__("rdx");
    unsigned char err;
    __asm__ volatile("syscall; setc %2"
                     : "+a"(rax), "=d"(rdx), "=qm"(err)
                     :
                     : "rcx", "r11", "memory", "cc");
    if (err)
        return -rax;
    *is_child = (g_u64)rdx;
    return rax;
}

static g_i64 guest_wait4(g_i64 pid, int *status)
{
    register g_i64 rax __asm__("rax") = SYS(SYS_wait4);
    register g_i64 rdi __asm__("rdi") = pid;
    register g_i64 rsi __asm__("rsi") = (g_i64)status;
    register g_i64 rdx __asm__("rdx") = 0;
    register g_i64 r10 __asm__("r10") = 0;
    __asm__ volatile("syscall"
                     : "+a"(rax)
                     : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10)
                     : "rcx", "r11", "memory", "cc");
    return rax;
}

int main(void)
{
    g_u64 seed = 1;
    for (g_u64 i = 0; i < 20000; i++) {
        g_u64 k = (i * 9) & 63;
        warm[k] += i + seed;
        seed ^= warm[k];
    }

    volatile g_u64 *p = (volatile g_u64 *)sys_mmap(
        0, 0x4000, PROT_READ | PROT_WRITE,
        MAP_ANON | MAP_SHARED, -1, 0);
    if ((void *)p == MAP_FAILED) {
        g_puts("shared ordered bad\n");
        return 0;
    }

    g_u64 sum = seed;
    for (g_u64 i = 0; i < 30000; i++) {
        g_u64 k = (i * 5) & 255;
        p[k] = p[(k + 17) & 255] + i + seed;
        sum += p[k];
    }
    p[300] = 0;
    g_u64 is_child = 0;
    g_i64 pid = guest_fork(&is_child);
    if (pid < 0) {
        g_puts("shared ordered bad\n");
        return 0;
    }
    if (is_child) {
        p[300] = 0x51a7ed5aULL;
        sys_exit(0);
    }
    int status = 0;
    g_i64 waited = guest_wait4(pid, &status);
    if (sum != 0 && p[0] != p[1] && waited == pid && status == 0 &&
        p[300] == 0x51a7ed5aULL)
        g_puts("shared ordered ok\n");
    else
        g_puts("shared ordered bad\n");
    sys_munmap((void *)p, 0x4000);
    return 0;
}

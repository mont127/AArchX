/* Plain-to-ordered memory transition across fork. */
#include "gsys.h"

#define SYS_fork 2
#define SYS_wait4 7

static volatile g_u64 cells[256];

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

static g_u64 churn(g_u64 seed, g_u64 n)
{
    g_u64 sum = seed;
    for (g_u64 i = 0; i < n; i++) {
        g_u64 k = (i * 13 + seed) & 255;
        cells[k] += i + seed;
        sum += cells[k];
    }
    return sum;
}

int main(void)
{
    g_u64 before = churn(3, 20000);
    g_u64 is_child = 0;
    g_i64 pid = guest_fork(&is_child);
    if (pid < 0) {
        g_puts("fork failed\n");
        return 1;
    }
    if (is_child) {
        g_u64 child = churn(7, 30000);
        sys_exit(child != before ? 7 : 8);
    }

    int status = 0;
    g_i64 waited = guest_wait4(pid, &status);
    g_u64 after = churn(0, 1);
    if (waited == pid && status == (7 << 8) && after != 0 && cells[0] != 0)
        g_puts("fork ordered ok\n");
    else {
        g_puts("fork ordered bad\n");
        g_puts("  pid="); g_putu64((g_u64)pid);
        g_puts("  waited="); g_putu64((g_u64)waited);
        g_puts("  status="); g_putu64((g_u64)status);
        g_puts("  after="); g_putu64(after);
        g_puts("  cells0="); g_putu64(cells[0]);
    }
    return 0;
}

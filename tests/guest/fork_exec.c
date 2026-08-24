/* Fork twice, then exec a fresh Ocerz process from the grandchild. */
#include "gsys.h"

#define SYS_fork 2
#define SYS_wait4 7
#define SYS_execve 59

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
    unsigned char err;
    __asm__ volatile("syscall; setc %1"
                     : "+a"(rax), "=qm"(err)
                     : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10)
                     : "rcx", "r11", "memory", "cc");
    return err ? -rax : rax;
}

static g_i64 guest_execve(const char *path, char *const argv[], char *const envp[])
{
    return g_syscall3(SYS(SYS_execve), (g_i64)path, (g_i64)argv,
                      (g_i64)envp);
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int main(int argc, char **argv, char **envp)
{
    if (argc == 2 && streq(argv[1], "--exec-child")) {
        g_puts("double fork exec ok\n");
        return 23;
    }

    g_u64 is_child = 0;
    g_i64 child = guest_fork(&is_child);
    if (child < 0)
        return 1;

    if (is_child) {
        g_u64 is_grandchild = 0;
        g_i64 grandchild = guest_fork(&is_grandchild);
        if (grandchild < 0)
            sys_exit(2);

        if (is_grandchild) {
            char *child_argv[] = { argv[0], "--exec-child", 0 };
            guest_execve(argv[0], child_argv, envp);
            sys_exit(3);
        }

        int status = 0;
        g_i64 waited = guest_wait4(grandchild, &status);
        sys_exit(waited == grandchild && status == (23 << 8) ? 0 : 4);
    }

    int status = 0;
    g_i64 waited = guest_wait4(child, &status);
    return waited == child && status == 0 ? 0 : 5;
}

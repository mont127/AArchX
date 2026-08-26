/* POSIX byte-range locks through fcntl: the struct flock pointer must be
 * translated at the syscall boundary.  Parent takes a write lock, the fork
 * child must see it via F_GETLK and fail to take a conflicting F_SETLK.
 * Goldens come from the native binary. */
#include "gsys.h"

#define SYS_fork 2
#define SYS_wait4 7
#define SYS_fcntl 92

#define F_GETLK 7
#define F_SETLK 8
#define F_RDLCK 1
#define F_UNLCK 2
#define F_WRLCK 3
#define SEEK_SET 0

#define PATH "/tmp/ocerz_guest_locks.dat"

struct flock_g {
    g_i64 l_start;
    g_i64 l_len;
    int l_pid;
    short l_type;
    short l_whence;
};

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

static g_i64 guest_fcntl(int fd, int cmd, void *arg)
{
    return g_syscall3(SYS(SYS_fcntl), fd, cmd, (g_i64)arg);
}

int main(int argc, char **argv, char **envp)
{
    g_i64 fd = sys_open(PATH, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        g_puts("open fail\n");
        return 1;
    }
    struct flock_g fl;
    fl.l_start = 16;
    fl.l_len = 32;
    fl.l_pid = 0;
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    g_i64 r = guest_fcntl((int)fd, F_SETLK, &fl);
    g_puts(r == 0 ? "parent setlk ok\n" : "parent setlk FAIL\n");

    g_u64 is_child = 0;
    g_i64 pid = guest_fork(&is_child);
    if (pid < 0) {
        g_puts("fork fail\n");
        return 1;
    }
    if (is_child) {
        /* re-open: POSIX locks are per-process, the inherited fd shares them */
        g_i64 cfd = sys_open(PATH, O_WRONLY, 0);
        if (cfd < 0) {
            g_puts("child open fail\n");
            return 1;
        }
        struct flock_g q;
        q.l_start = 0;
        q.l_len = 0;
        q.l_pid = 0;
        q.l_type = F_WRLCK;
        q.l_whence = SEEK_SET;
        r = guest_fcntl((int)cfd, F_GETLK, &q);
        if (r != 0)
            g_puts("child getlk FAIL\n");
        else {
            g_puts(q.l_type == F_WRLCK ? "child sees wrlck\n" : "child sees no lock\n");
            g_puts(q.l_start == 16 && q.l_len == 32 ? "range ok\n" : "range BAD\n");
        }
        struct flock_g c;
        c.l_start = 20;
        c.l_len = 4;
        c.l_pid = 0;
        c.l_type = F_WRLCK;
        c.l_whence = SEEK_SET;
        r = guest_fcntl((int)cfd, F_SETLK, &c);
        g_puts(r < 0 ? "child conflict blocked\n" : "child conflict ALLOWED\n");
        return 0;
    }
    int status = 0;
    guest_wait4(pid, &status);
    g_puts((status & 0xff) == 0 && ((status >> 8) & 0xff) == 0 ? "child exit 0\n"
                                                               : "child exit BAD\n");
    sys_unlink(PATH);
    g_puts("done\n");
    return 0;
}

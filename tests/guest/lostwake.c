/* A signal that lands while the guest is in pure user-mode code must still be
 * delivered before the next blocking syscall parks the thread.
 *
 * Parent installs a SIGUSR1 handler that writes a byte to a pipe, tells a
 * forked child to kill it, then spins with NO syscalls at all, then blocks in
 * read() on that pipe.  Natively the handler has already run and the read
 * returns at once.  If the emulator only drains pending signals on the RETURN
 * edge of a syscall, the byte is never written and the read parks forever.
 *
 * Manual reproducer, not part of `make check`: it needs the child to deliver
 * the signal while the parent is mid-spin, and the spin takes minutes under
 * the interpreter.  Build and run it as described in tests/guest/Makefile. */
#include "gsys.h"

#define SYS_sigaction 46
#define SYS_fork 2
#define SYS_pipe 42
#define SYS_kill 37

#define SIGUSR1 30
#define SA_SIGINFO 0x0040

struct k_sigaction {
    g_u64 handler;
    g_u64 tramp;
    g_u32 mask;
    g_u32 flags;
};

__asm__(
    ".text\n"
    ".globl _sig_tramp\n"
    "_sig_tramp:\n"
    "    pushq %rbp\n"
    "    movq  %rsp, %rbp\n"
    "    movq  %r8, %rbx\n"
    "    movq  %r9, %r12\n"
    "    movq  %rdi, %rax\n"
    "    movl  %edx, %edi\n"
    "    movq  %rcx, %rsi\n"
    "    movq  %r8, %rdx\n"
    "    callq *%rax\n"
    "    movq  %rbx, %rdi\n"
    "    movl  $0x1e, %esi\n"
    "    movq  %r12, %rdx\n"
    "    movl  $0x20000b8, %eax\n"
    "    syscall\n"
    "    ud2\n");

extern void sig_tramp(void);

static int data_w;

static void handler(int signo, void *si, void *uc)
{
    (void)signo; (void)si; (void)uc;
    sys_write(data_w, "x", 1);
}

static g_i64 guest_fork(g_u64 *is_child)
{
    register g_i64 rax __asm__("rax") = SYS(SYS_fork);
    register g_i64 rdx __asm__("rdx");
    unsigned char err;
    __asm__ volatile("syscall; setc %2"
                     : "+a"(rax), "=d"(rdx), "=qm"(err)
                     : : "rcx", "r11", "memory", "cc");
    if (err)
        return -rax;
    *is_child = (g_u64)rdx;
    return rax;
}

static g_i64 guest_pipe(int *rd, int *wr)
{
    register g_i64 rax __asm__("rax") = SYS(SYS_pipe);
    register g_i64 rdx __asm__("rdx");
    unsigned char err;
    __asm__ volatile("syscall; setc %2"
                     : "+a"(rax), "=d"(rdx), "=qm"(err)
                     : : "rcx", "r11", "memory", "cc");
    if (err)
        return -1;
    *rd = (int)rax;
    *wr = (int)rdx;
    return 0;
}

static volatile g_u64 acc;

int main(void)
{
    int dr, dw, sr, sw;
    if (guest_pipe(&dr, &dw) < 0 || guest_pipe(&sr, &sw) < 0) {
        g_puts("pipe failed\n");
        return 1;
    }
    data_w = dw;

    struct k_sigaction sa;
    sa.handler = (g_u64)&handler;
    sa.tramp = (g_u64)&sig_tramp;
    sa.mask = 0;
    sa.flags = SA_SIGINFO;
    g_syscall3(SYS(SYS_sigaction), SIGUSR1, (g_i64)(g_u64)&sa, 0);

    g_i64 me = sys_getpid();
    g_u64 is_child = 0;
    g_i64 pid = guest_fork(&is_child);
    if (pid < 0) {
        g_puts("fork failed\n");
        return 1;
    }
    if (is_child) {
        char c;
        sys_read(sr, &c, 1);
        g_syscall3(SYS(SYS_kill), me, SIGUSR1, 1);
        sys_exit(0);
    }

    g_puts("start\n");
    sys_write(sw, "g", 1);

    /* No syscalls from here to the read: the signal must land in this loop. */
    for (g_u64 i = 0; i < 300000000ull; i++)
        acc += i;

    char b = 0;
    g_i64 r = sys_read(dr, &b, 1);
    g_puts("woke rc=");
    g_putu64((g_u64)r);
    return 0;
}

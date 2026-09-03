/* Freestanding x86_64 syscall wrappers and output helpers for the guest tests. */
#ifndef OCERZ_GUEST_GSYS_H
#define OCERZ_GUEST_GSYS_H

typedef unsigned long g_u64;
typedef long g_i64;
typedef unsigned int g_u32;

#define SYS(n) (0x2000000 | (n))

#define SYS_exit 1
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_unlink 10
#define SYS_getpid 20
#define SYS_gettimeofday 116
#define SYS_munmap 73
#define SYS_mmap 197

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define MAP_SHARED 0x0001
#define MAP_PRIVATE 0x0002
#define MAP_ANON 0x1000
#define MAP_FIXED 0x0010
#define MAP_FAILED ((void *)-1L)

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_CREAT 0x0200
#define O_TRUNC 0x0400

static inline g_i64 g_syscall0(g_i64 num)
{
    register g_i64 rax __asm__("rax") = num;
    __asm__ volatile("syscall" : "+r"(rax) : : "rcx", "rdx", "r11", "memory", "cc");
    return rax;
}

static inline g_i64 g_syscall1(g_i64 num, g_i64 a1)
{
    register g_i64 rax __asm__("rax") = num;
    register g_i64 rdi __asm__("rdi") = a1;
    __asm__ volatile("syscall" : "+r"(rax) : "r"(rdi) : "rcx", "rdx", "r11", "memory", "cc");
    return rax;
}

static inline g_i64 g_syscall3(g_i64 num, g_i64 a1, g_i64 a2, g_i64 a3)
{
    register g_i64 rax __asm__("rax") = num;
    register g_i64 rdi __asm__("rdi") = a1;
    register g_i64 rsi __asm__("rsi") = a2;
    register g_i64 rdx __asm__("rdx") = a3;
    __asm__ volatile("syscall"
                     : "+r"(rax), "+r"(rdx)
                     : "r"(rdi), "r"(rsi)
                     : "rcx", "r11", "memory", "cc");
    return rax;
}

static inline g_i64 g_syscall6(g_i64 num, g_i64 a1, g_i64 a2, g_i64 a3, g_i64 a4, g_i64 a5, g_i64 a6)
{
    register g_i64 rax __asm__("rax") = num;
    register g_i64 rdi __asm__("rdi") = a1;
    register g_i64 rsi __asm__("rsi") = a2;
    register g_i64 rdx __asm__("rdx") = a3;
    register g_i64 r10 __asm__("r10") = a4;
    register g_i64 r8 __asm__("r8") = a5;
    register g_i64 r9 __asm__("r9") = a6;
    __asm__ volatile("syscall"
                     : "+r"(rax), "+r"(rdx)
                     : "r"(rdi), "r"(rsi), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory", "cc");
    return rax;
}

static inline void sys_exit(int code)
{
    g_syscall1(SYS(SYS_exit), (g_i64)code);
    for (;;) {
    }
}

static inline g_i64 sys_write(int fd, const void *buf, g_u64 len)
{
    return g_syscall3(SYS(SYS_write), (g_i64)fd, (g_i64)buf, (g_i64)len);
}

static inline g_i64 sys_read(int fd, void *buf, g_u64 len)
{
    return g_syscall3(SYS(SYS_read), (g_i64)fd, (g_i64)buf, (g_i64)len);
}

static inline g_i64 sys_open(const char *path, int flags, int mode)
{
    return g_syscall3(SYS(SYS_open), (g_i64)path, (g_i64)flags, (g_i64)mode);
}

static inline g_i64 sys_close(int fd)
{
    return g_syscall1(SYS(SYS_close), (g_i64)fd);
}

static inline g_i64 sys_unlink(const char *path)
{
    return g_syscall1(SYS(SYS_unlink), (g_i64)path);
}

static inline g_i64 sys_getpid(void)
{
    return g_syscall0(SYS(SYS_getpid));
}

static inline g_i64 sys_gettimeofday(void *tv, void *tz)
{
    return g_syscall3(SYS(SYS_gettimeofday), (g_i64)tv, (g_i64)tz, 0);
}

static inline void *sys_mmap(void *addr, g_u64 len, int prot, int flags, int fd, g_i64 off)
{
    return (void *)g_syscall6(SYS(SYS_mmap), (g_i64)addr, (g_i64)len,
                              (g_i64)prot, (g_i64)flags, (g_i64)fd, off);
}

static inline g_i64 sys_munmap(void *addr, g_u64 len)
{
    return g_syscall3(SYS(SYS_munmap), (g_i64)addr, (g_i64)len, 0);
}

static inline g_u64 g_strlen_local(const char *s)
{
    g_u64 n = 0;
    while (s[n])
        n++;
    return n;
}

static inline void g_puts(const char *s)
{
    sys_write(1, s, g_strlen_local(s));
}

static inline void g_putu64(g_u64 n)
{
    char buf[24];
    int i = (int)sizeof(buf);
    buf[--i] = '\n';
    if (n == 0) {
        buf[--i] = '0';
    } else {
        while (n != 0 && i > 0) {
            buf[--i] = (char)('0' + (int)(n % 10));
            n /= 10;
        }
    }
    sys_write(1, buf + i, sizeof(buf) - (g_u64)i);
}

static inline void g_putu64_nonl(g_u64 n)
{
    char buf[24];
    int i = (int)sizeof(buf);
    if (n == 0) {
        buf[--i] = '0';
    } else {
        while (n != 0 && i > 0) {
            buf[--i] = (char)('0' + (int)(n % 10));
            n /= 10;
        }
    }
    sys_write(1, buf + i, sizeof(buf) - (g_u64)i);
}

static inline void g_puthex64(g_u64 n)
{
    static const char hexd[] = "0123456789abcdef";
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int k = 0; k < 16; k++)
        buf[2 + k] = hexd[(n >> ((15 - k) * 4)) & 0xf];
    buf[18] = '\n';
    sys_write(1, buf, sizeof(buf));
}

#endif

/*
 * Cross-process coherence of a writable MAP_SHARED file view, sized like
 * Steam's SteamChrome_MasterStream ring buffer (0x2010 bytes, so 3 guest pages
 * after rounding, base 16 KB-aligned, mapped read/write in TWO processes that
 * each mmap the same fd independently).
 *
 * wine backs an anonymous named section with a file in the server dir, and each
 * process calls NtMapViewOfSection which mmaps that fd MAP_SHARED.  If the
 * emulator hands one side a private copy - or two overlays that do not share
 * physical pages - Steam's writes never reach the CEF browser and Steam
 * re-creates the stream forever with no window (2026-09-06).  The child
 * re-mmaps the same fd at its own address and the two sides hand a token back
 * and forth through the shared bytes: instant natively, a poll timeout when
 * broken.
 */
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VIEW 0x3000
#define FILELEN 0x4000

static volatile unsigned *at(void *base, unsigned off) { return (volatile unsigned *)((char *)base + off); }

static int spin_until(volatile unsigned *p, unsigned want, double secs)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    double t0 = ts.tv_sec + ts.tv_nsec / 1e9;
    for (;;) {
        if (__atomic_load_n(p, __ATOMIC_ACQUIRE) == want) return 1;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if (ts.tv_sec + ts.tv_nsec / 1e9 - t0 > secs) return 0;
        usleep(200);
    }
}

int main(void)
{
    char path[] = "/tmp/ocerz_shmem.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 2; }
    if (ftruncate(fd, FILELEN) != 0) { perror("ftruncate"); return 2; }

    const unsigned P2C_FLAG = 0x000, P2C_DATA = 0x004;
    const unsigned C2P_FLAG = 0x1000, C2P_DATA = 0x1004;
    const unsigned TAIL      = 0x2000;

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 2; }

    if (pid == 0) {
        void *m = mmap(NULL, VIEW, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) { _exit(3); }
        if (!spin_until(at(m, P2C_FLAG), 1, 8.0)) _exit(4);
        unsigned got = __atomic_load_n(at(m, P2C_DATA), __ATOMIC_ACQUIRE);
        unsigned tail = __atomic_load_n(at(m, TAIL), __ATOMIC_ACQUIRE);
        __atomic_store_n(at(m, C2P_DATA), got + 1, __ATOMIC_RELEASE);
        __atomic_store_n(at(m, TAIL + 4), tail + 1, __ATOMIC_RELEASE);
        __atomic_store_n(at(m, C2P_FLAG), 1, __ATOMIC_RELEASE);
        _exit((got == 0xC0FFEE && tail == 0x7A1100) ? 0 : 5);
    }

    void *m = mmap(NULL, VIEW, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { perror("parent mmap"); return 2; }
    __atomic_store_n(at(m, P2C_DATA), 0xC0FFEE, __ATOMIC_RELEASE);
    __atomic_store_n(at(m, TAIL), 0x7A1100, __ATOMIC_RELEASE);
    __atomic_store_n(at(m, P2C_FLAG), 1, __ATOMIC_RELEASE);

    int ok_reply = spin_until(at(m, C2P_FLAG), 1, 8.0);
    unsigned reply = __atomic_load_n(at(m, C2P_DATA), __ATOMIC_ACQUIRE);
    unsigned tailback = __atomic_load_n(at(m, TAIL + 4), __ATOMIC_ACQUIRE);

    int st = 0; waitpid(pid, &st, 0);

    fprintf(stderr, "child_exit=%d reply=%#x (want 0xc0ffef) tailback=%#x (want 0x7a1101) reply_seen=%d\n",
           WIFEXITED(st) ? WEXITSTATUS(st) : -1, reply, tailback, ok_reply);
    int pass = ok_reply && reply == 0xC0FFEF && tailback == 0x7A1101;
    printf(pass ? "OK\n" : "FAIL\n");
    unlink(path);
    return pass ? 0 : 1;
}

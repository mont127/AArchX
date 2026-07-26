/* File I/O round trip: write a pattern, read it back, verify, unlink. */
#include "gsys.h"

#define PATH "/tmp/ocerz_guest_test.dat"
#define N 100

static unsigned char pat(int i)
{
    return (unsigned char)((i * 37 + 13) & 0xff);
}

int main(int argc, char **argv, char **envp)
{
    unsigned char out[N];
    unsigned char in[N];
    for (int i = 0; i < N; i++)
        out[i] = pat(i);

    g_i64 fd = sys_open(PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        g_puts("open-w fail\n");
        return 1;
    }
    g_i64 w = sys_write((int)fd, out, N);
    if (w != N) {
        g_puts("write fail\n");
        sys_close((int)fd);
        sys_unlink(PATH);
        return 1;
    }
    sys_close((int)fd);

    fd = sys_open(PATH, O_RDONLY, 0);
    if (fd < 0) {
        g_puts("open-r fail\n");
        sys_unlink(PATH);
        return 1;
    }

    int got = 0;
    while (got < N) {
        g_i64 r = sys_read((int)fd, in + got, (g_u64)(N - got));
        if (r <= 0)
            break;
        got += (int)r;
    }
    sys_close((int)fd);

    if (got != N) {
        g_puts("read fail\n");
        sys_unlink(PATH);
        return 1;
    }
    for (int i = 0; i < N; i++) {
        if (in[i] != out[i]) {
            g_puts("compare fail\n");
            sys_unlink(PATH);
            return 1;
        }
    }

    if (sys_unlink(PATH) != 0) {
        g_puts("unlink fail\n");
        return 1;
    }

    g_puts("fileio ok\n");
    return 0;
}

/*
 * preadv/pwritev: scattered positioned I/O.  Both were absent from the syscall
 * table and returned ENOSYS.  The offset must not move the file position.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

int main(void)
{
    char path[] = "/tmp/ocerz_preadv.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return 1;
    unlink(path);

    struct iovec wv[2] = { { "hello", 5 }, { "world", 5 } };
    if (pwritev(fd, wv, 2, 3) != 10) {
        printf("BAD pwritev\n");
        return 2;
    }
    char a[5] = { 0 }, b[6] = { 0 };
    struct iovec rv[2] = { { a, 5 }, { b, 5 } };
    if (preadv(fd, rv, 2, 3) != 10 || memcmp(a, "hello", 5) != 0 || memcmp(b, "world", 5) != 0) {
        printf("BAD preadv a='%.5s' b='%.5s'\n", a, b);
        return 3;
    }
    char z;
    if (pread(fd, &z, 1, 0) != 1 || z != 0) {
        printf("BAD hole byte %d\n", z);
        return 4;
    }
    close(fd);
    printf("OK\n");
    return 0;
}

/* POSIX aio from an i386 guest: the read lands in aio_buf and nowhere else. */
#include <aio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    char path[] = "/tmp/m32_aio_XXXXXX";
    int fd = mkstemp(path);
    const char text[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    write(fd, text, sizeof text - 1);
    struct { char before[16]; char buf[16]; char after[16]; } m;
    memset(&m, 'x', sizeof m);
    struct aiocb cb;
    memset(&cb, 0, sizeof cb);
    cb.aio_fildes = fd;
    cb.aio_offset = 10;
    cb.aio_buf = m.buf;
    cb.aio_nbytes = 16;
    int r = aio_read(&cb);
    const struct aiocb *list[1] = { &cb };
    while (r == 0 && aio_error(&cb) == EINPROGRESS)
        aio_suspend(list, 1, NULL);
    ssize_t n = aio_return(&cb);
    int clean = 1;
    for (int i = 0; i < 16; i++)
        clean &= m.before[i] == 'x' && m.after[i] == 'x';
    printf("read %d %.16s clean %d\n", (int)n, m.buf, clean);
    close(fd);
    unlink(path);
    return 0;
}

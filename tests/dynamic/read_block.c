/*
 * A read() that blocks well past the unstick monitor's 800 ms threshold must
 * come back with the data, not EINTR: natively a read only returns EINTR when
 * a signal handler ran, and this process installs none.  Chess's engine lexer
 * read its pipe exactly like this and died on the -1.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int p[2];

static void *writer(void *arg)
{
    (void)arg;
    usleep(2500 * 1000);
    if (write(p[1], "x", 1) != 1)
        return (void *)1;
    return NULL;
}

int main(void)
{
    if (pipe(p) != 0)
        return 1;
    pthread_t t;
    if (pthread_create(&t, NULL, writer, NULL) != 0)
        return 2;
    char c = 0;
    ssize_t r = read(p[0], &c, 1);
    int e = errno;
    pthread_join(t, NULL);
    if (r != 1 || c != 'x') {
        printf("BAD r=%zd errno=%d (%s)\n", r, e, strerror(e));
        return 3;
    }
    printf("OK\n");
    return 0;
}

/*
 * A loopback TCP client/server in one process: bind/listen/accept/connect/
 * send/recv/poll/getsockname/setsockopt across two guest threads.  The server
 * upper-cases what it receives and the client checks the echo, exercising the
 * socket syscalls and cross-thread blocking I/O.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int port;

static void *server(void *a)
{
    (void)a;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(ls, (void *)&sa, sizeof sa) != 0)
        return NULL;
    socklen_t sl = sizeof sa;
    getsockname(ls, (void *)&sa, &sl);
    port = ntohs(sa.sin_port);
    listen(ls, 4);
    int cs = accept(ls, 0, 0);
    char buf[128];
    ssize_t n;
    while ((n = recv(cs, buf, sizeof buf, 0)) > 0) {
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] >= 'a' && buf[i] <= 'z')
                buf[i] -= 32;
        send(cs, buf, n, 0);
    }
    close(cs);
    close(ls);
    return NULL;
}

int main(void)
{
    pthread_t t;
    if (pthread_create(&t, 0, server, 0))
        return 2;
    for (int i = 0; !port && i < 5000; i++)
        usleep(1000);
    if (!port) {
        printf("BAD no port\n");
        return 3;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(port);
    if (connect(fd, (void *)&sa, sizeof sa) != 0) {
        printf("BAD connect\n");
        return 1;
    }
    const char *msgs[] = { "hello", "world", "socket ok", 0 };
    char out[256];
    int oi = 0;
    for (int i = 0; msgs[i]; i++) {
        send(fd, msgs[i], strlen(msgs[i]), 0);
        struct pollfd p = { fd, POLLIN, 0 };
        poll(&p, 1, 1000);
        char b[128];
        ssize_t n = recv(fd, b, sizeof b, 0);
        if (n > 0) {
            memcpy(out + oi, b, n);
            oi += n;
        }
    }
    out[oi] = 0;
    close(fd);
    pthread_join(t, 0);
    if (strcmp(out, "HELLOWORLDSOCKET OK") != 0) {
        printf("BAD echo '%s'\n", out);
        return 1;
    }
    printf("OK\n");
    return 0;
}

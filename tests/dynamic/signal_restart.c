/*
 * A signal sent to a thread blocked in read() reaches its handler while the read waits, and the read is made again
 * or fails with EINTR as the handler asked.
 *
 * SIGALRM's host handler is installed with SA_RESTART, so the kernel restarted ocerz's host read() under it and the
 * guest's handler did not run until data came; a SIGUSR1 that landed just before the read reached the kernel was held
 * the same way (thread_signal.c lost one about once in thirty).  The unstick monitor now interrupts a wait with a guest
 * signal pending: with SA_RESTART the handler runs and the read goes on to return the byte written after it, without
 * SA_RESTART the read returns EINTR.
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t hit;
static int fds[2];
static pthread_t reader;
static volatile int handled_before_write;

static void on_alarm(int sig) { (void)sig; hit = 1; }

static void *poker(void *arg)
{
    (void)arg;
    usleep(300000);
    pthread_kill(reader, SIGALRM);
    for (int i = 0; i < 500 && !hit; i++)   /* 5 s for the handler to run while the read waits */
        usleep(10000);
    handled_before_write = hit;
    usleep(100000);
    write(fds[1], "x", 1);
    return NULL;
}

static int round_trip(int flags, ssize_t *n, int *err)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sa.sa_flags = flags;
    sigaction(SIGALRM, &sa, NULL);
    hit = 0;
    handled_before_write = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, poker, NULL) != 0)
        return -1;
    char c;
    errno = 0;
    *n = read(fds[0], &c, 1);
    *err = errno;
    pthread_join(t, NULL);
    if (*n < 0)   /* the byte the poker wrote after all */
        read(fds[0], &c, 1);
    return 0;
}

int main(void)
{
    if (pipe(fds) != 0) { perror("pipe"); return 2; }
    reader = pthread_self();
    ssize_t n;
    int err;
    if (round_trip(SA_RESTART, &n, &err) != 0) return 2;
    if (!handled_before_write || n != 1) {
        printf("FAIL restart: handled while waiting %d, read %zd errno %d\n", handled_before_write, n, err);
        return 1;
    }
    if (round_trip(0, &n, &err) != 0) return 2;
    if (!handled_before_write || n != -1 || err != EINTR) {
        printf("FAIL no restart: handled while waiting %d, read %zd errno %d\n", handled_before_write, n, err);
        return 1;
    }
    printf("OK\n");
    return 0;
}

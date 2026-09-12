/*
 * sigsuspend and sigwait: both blocked on a signal the host kernel could never
 * route to the guest and returned ENOSYS.  A worker thread sends SIGUSR1 so
 * sigsuspend must wake in its handler; then a blocked SIGUSR2 is raised and
 * sigwait must hand its number back without any handler running.
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t got_usr1;
static pthread_t main_tid;

static void on_usr1(int s)
{
    (void)s;
    got_usr1++;
}

static void *waker(void *a)
{
    (void)a;
    usleep(50000);
    pthread_kill(main_tid, SIGUSR1);
    return NULL;
}

int main(void)
{
    main_tid = pthread_self();

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);

    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &block, &old);

    pthread_t w;
    pthread_create(&w, NULL, waker, NULL);

    sigset_t none = old;
    sigdelset(&none, SIGUSR1);
    if (sigsuspend(&none) != -1 || errno != EINTR) {
        printf("BAD sigsuspend errno=%d\n", errno);
        return 1;
    }
    pthread_join(w, NULL);
    if (got_usr1 != 1) {
        printf("BAD sigsuspend handler ran %d times\n", (int)got_usr1);
        return 2;
    }

    sigset_t s2;
    sigemptyset(&s2);
    sigaddset(&s2, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &s2, NULL);
    raise(SIGUSR2);
    int sig = 0;
    if (sigwait(&s2, &sig) != 0 || sig != SIGUSR2) {
        printf("BAD sigwait sig=%d\n", sig);
        return 3;
    }
    printf("OK\n");
    return 0;
}

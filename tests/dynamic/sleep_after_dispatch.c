#include <dispatch/dispatch.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t alarmed;

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void on_alarm(int sig)
{
    (void)sig;
    alarmed = 1;
}

int main(void)
{
    int fails = 0;
    dispatch_group_t g = dispatch_group_create();
    for (int i = 0; i < 8; i++)
        dispatch_group_async(g, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            usleep(10000);
        });
    dispatch_group_wait(g, DISPATCH_TIME_FOREVER);

    double t0 = now();
    unsigned left = sleep(2);
    double dt = now() - t0;
    if (left != 0 || dt < 1.95) {
        printf("BAD sleep(2) returned %u after %.2fs\n", left, dt);
        fails++;
    }

    struct timespec req = { 1, 500000000 }, rem = { 0, 0 };
    t0 = now();
    int rc = nanosleep(&req, &rem);
    dt = now() - t0;
    if (rc != 0 || dt < 1.45) {
        printf("BAD nanosleep of 1.5s returned %d after %.2fs\n", rc, dt);
        fails++;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    alarm(1);
    t0 = now();
    left = sleep(4);
    dt = now() - t0;
    if (!alarmed || left == 0 || dt > 3.0) {
        printf("BAD sleep(4) with SIGALRM after 1s returned %u after %.2fs (handler ran: %d)\n", left, dt,
               (int)alarmed);
        fails++;
    }

    if (!fails)
        printf("OK\n");
    return fails != 0;
}

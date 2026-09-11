/* setitimer/getitimer: an ITIMER_REAL timer must fire into the guest's
 * SIGALRM handler, and getitimer must report the armed interval.  Both
 * calls came back ENOSYS. */
#include <signal.h>
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>

static volatile sig_atomic_t fired;

static void on_alarm(int s)
{
    (void)s;
    fired++;
}

int main(void)
{
    signal(SIGALRM, on_alarm);
    struct itimerval it = { { 0, 20000 }, { 0, 20000 } }, cur;
    if (setitimer(ITIMER_REAL, &it, NULL) != 0) {
        printf("BAD setitimer\n");
        return 1;
    }
    if (getitimer(ITIMER_REAL, &cur) != 0 || cur.it_interval.tv_sec != 0 ||
        cur.it_interval.tv_usec != 20000) {
        printf("BAD getitimer interval %ld.%06ld\n", (long)cur.it_interval.tv_sec,
               (long)cur.it_interval.tv_usec);
        return 2;
    }
    for (int i = 0; i < 500 && fired < 3; i++)
        usleep(10000);
    struct itimerval off = { { 0, 0 }, { 0, 0 } };
    setitimer(ITIMER_REAL, &off, NULL);
    if (fired < 3) {
        printf("BAD SIGALRM fired %d times\n", (int)fired);
        return 3;
    }
    printf("OK\n");
    return 0;
}

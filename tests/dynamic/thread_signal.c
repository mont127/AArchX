/* Thread-directed signals into a thread parked in read().
 *
 * wineserver suspends a thread (NtSuspendThread, the StackSamplingProfiler
 * in Steam's CEF does it 20 times a second) by sending it SIGUSR1 with
 * __pthread_kill(thread port); the handler reports the context back and the
 * server only then completes any wait for that thread.  One lost delivery
 * leaves the thread "suspended" forever server-side: every one of its waits
 * stays pending and the browser IO thread deadlocks (2026-09-06).
 *
 * The target loops in a blocking read() on a pipe that never gets data.  The
 * sender fires SIGUSR1 at it and waits for the handler to run on the target
 * thread before firing the next one.  Natively every signal is delivered
 * within microseconds; a miss is a signal the emulator swallowed. */
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static atomic_int hits;
static atomic_int wrong_thread;
static atomic_int stop;
static pthread_t target;
static int pipe_rd;

static int nested;          /* handler blocks in read() until resumed, like wine's wait_suspend */
static int resume_rd, resume_wr;
static atomic_int in_handler;
static atomic_int entered;

static void handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si; (void)uc;
    if (!pthread_equal(pthread_self(), target))
        atomic_fetch_add(&wrong_thread, 1);
    atomic_fetch_add(&entered, 1);
    if (nested) {
        char c;
        atomic_store(&in_handler, 1);
        while (read(resume_rd, &c, 1) < 0 && errno == EINTR)
            ;
    }
    atomic_fetch_add(&hits, 1);
}

static void *reader(void *arg)
{
    (void)arg;
    char c;
    while (!atomic_load(&stop)) {
        ssize_t n = read(pipe_rd, &c, 1);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
    }
    return NULL;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 120;
    nested = argc > 2 && strcmp(argv[2], "nested") == 0;
    int fds[2], rfds[2];
    if (pipe(fds) != 0 || pipe(rfds) != 0) { perror("pipe"); return 2; }
    pipe_rd = fds[0];
    resume_rd = rfds[0]; resume_wr = rfds[1];

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);

    if (pthread_create(&target, NULL, reader, NULL) != 0) { perror("pthread_create"); return 2; }
    usleep(20000);

    int misses = 0, slow = 0;
    double worst = 0;
    for (int i = 0; i < rounds; i++) {
        int before = atomic_load(&hits);
        if (nested) {
            /* suspend: signal, wait until the handler is parked in its read,
             * resume it and fire the NEXT suspend at once, so the second
             * signal lands while the first handler is still unwinding with
             * SIGUSR1 masked.  Both must enter the handler. */
            int e0 = atomic_load(&entered), h0 = atomic_load(&hits);
            if (pthread_kill(target, SIGUSR1) != 0) { perror("pthread_kill"); return 2; }
            double tw = now();
            while (atomic_load(&entered) < e0 + 1 && now() - tw < 2.0) usleep(50);
            if (atomic_load(&entered) < e0 + 1) { misses++; fprintf(stderr, "round %d: first SIGUSR1 never entered the handler\n", i); continue; }
            write(resume_wr, "r", 1);
            if (pthread_kill(target, SIGUSR1) != 0) { perror("pthread_kill"); return 2; }
            double t1 = now();
            while (atomic_load(&entered) < e0 + 2 && now() - t1 < 2.0) usleep(50);
            if (atomic_load(&entered) < e0 + 2) {
                misses++;
                fprintf(stderr, "round %d: second SIGUSR1 lost (entered %d, hits %d)\n", i,
                       atomic_load(&entered) - e0, atomic_load(&hits) - h0);
                continue;
            }
            write(resume_wr, "r", 1);
            double t2 = now();
            while (atomic_load(&hits) < h0 + 2 && now() - t2 < 2.0) usleep(50);
            if (atomic_load(&hits) < h0 + 2) { misses++; fprintf(stderr, "round %d: handlers did not finish\n", i); }
            else { double dt = now() - t1; if (dt > worst) worst = dt; if (dt > 0.1) slow++; }
            continue;
        }
        if (pthread_kill(target, SIGUSR1) != 0) { perror("pthread_kill"); return 2; }
        double t0 = now();
        while (atomic_load(&hits) == before) {
            if (now() - t0 > 2.0) break;
            if (now() - t0 > 0.001) usleep(200);
        }
        double dt = now() - t0;
        if (atomic_load(&hits) == before) {
            misses++;
            fprintf(stderr, "round %d: SIGUSR1 not delivered within 2s\n", i);
            /* give the pending signal every chance: it must arrive eventually */
        } else {
            if (dt > worst) worst = dt;
            if (dt > 0.1) slow++;
        }
    }
    atomic_store(&stop, 1);
    nested = 0;                      /* the wake-up below must not park in the handler */
    pthread_kill(target, SIGUSR1);   /* wake the reader so it sees stop */
    close(fds[1]);
    pthread_join(target, NULL);

    fprintf(stderr, "rounds=%d hits=%d misses=%d slow(>100ms)=%d worst=%.1fms wrong_thread=%d\n",
           rounds, atomic_load(&hits), misses, slow, worst * 1000.0, atomic_load(&wrong_thread));
    printf(misses == 0 && atomic_load(&wrong_thread) == 0 ? "OK\n" : "FAIL\n");
    return misses == 0 && atomic_load(&wrong_thread) == 0 ? 0 : 1;
}

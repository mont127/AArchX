#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define MAXT 64

static pthread_key_t key;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static mach_port_t ports[MAXT];
static int alive[MAXT];
static int nthreads;

static void destructor(void *v)
{
    int idx = (int)(intptr_t)v - 1;
    pthread_mutex_lock(&lock);
    if (idx >= 0 && idx < nthreads)
        alive[idx] = 0;
    pthread_mutex_unlock(&lock);
}

static void note_thread(void)
{
    if (pthread_getspecific(key))
        return;
    pthread_mutex_lock(&lock);
    int idx = nthreads < MAXT ? nthreads++ : -1;
    if (idx >= 0) {
        ports[idx] = pthread_mach_thread_np(pthread_self());
        alive[idx] = 1;
    }
    pthread_mutex_unlock(&lock);
    if (idx >= 0)
        pthread_setspecific(key, (void *)(intptr_t)(idx + 1));
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void idle_for(double seconds)
{
    double end = now() + seconds;
    while (now() < end)
        usleep(50000);
}

static int check_live_threads(const char *when)
{
    int bad = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nthreads; i++) {
        if (!alive[i])
            continue;
        kern_return_t kr = thread_suspend(ports[i]);
        if (kr == KERN_SUCCESS) {
            thread_resume(ports[i]);
        } else {
            if (bad < 3)
                printf("BAD %s: a workqueue thread that has not run its destructors cannot be suspended (%#x)\n",
                       when, kr);
            bad++;
        }
    }
    pthread_mutex_unlock(&lock);
    return bad;
}

int main(int argc, char **argv)
{
    double long_idle = argc > 1 ? atof(argv[1]) : 6.0;
    pthread_key_create(&key, destructor);
    dispatch_group_t g = dispatch_group_create();
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
    for (int i = 0; i < 32; i++)
        dispatch_group_async(g, q, ^{
            note_thread();
            usleep(20000);
        });
    dispatch_group_wait(g, DISPATCH_TIME_FOREVER);

    idle_for(0.5);
    int bad = check_live_threads("after the work finished");
    idle_for(long_idle);
    bad += check_live_threads("after the pool had time to shrink");
    if (!bad)
        printf("OK\n");
    return bad != 0;
}

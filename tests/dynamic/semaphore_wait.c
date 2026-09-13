#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static semaphore_t g_sem;

static void *signaller(void *arg)
{
    (void)arg;
    sleep(2);
    semaphore_signal(g_sem);
    return NULL;
}

int main(void)
{
    if (semaphore_create(mach_task_self(), &g_sem, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) {
        printf("BAD create\n");
        return 1;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, signaller, NULL) != 0) {
        printf("BAD thread\n");
        return 1;
    }
    kern_return_t kr = semaphore_wait(g_sem);
    pthread_join(t, NULL);
    semaphore_destroy(mach_task_self(), g_sem);
    if (kr != KERN_SUCCESS) {
        printf("BAD kr=%d\n", kr);
        return 1;
    }
    printf("OK\n");
    return 0;
}

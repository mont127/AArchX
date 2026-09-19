#include <dispatch/dispatch.h>
#include <stdio.h>

int main(void)
{
    enum { workers = 48 };
    dispatch_semaphore_t ready = dispatch_semaphore_create(0);
    dispatch_semaphore_t release = dispatch_semaphore_create(0);
    dispatch_group_t group = dispatch_group_create();
    dispatch_queue_t queues[workers];
    for (int i = 0; i < workers; i++) {
        queues[i] = dispatch_queue_create("ocerz.overcommit", DISPATCH_QUEUE_SERIAL);
        dispatch_group_async(group, queues[i], ^{
            dispatch_semaphore_signal(ready);
            dispatch_semaphore_wait(release, DISPATCH_TIME_FOREVER);
        });
    }
    dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC);
    int started = 0;
    while (started < workers && dispatch_semaphore_wait(ready, deadline) == 0)
        started++;
    for (int i = 0; i < workers; i++)
        dispatch_semaphore_signal(release);
    int drained = dispatch_group_wait(group,
        dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0;
    if (started != workers || !drained) {
        printf("FAIL started=%d drained=%d\n", started, drained);
        return 1;
    }
    for (int i = 0; i < workers; i++)
        dispatch_release(queues[i]);
    dispatch_release(group);
    dispatch_release(release);
    dispatch_release(ready);
    puts("OK");
    return 0;
}

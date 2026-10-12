/* dispatch_once and dispatch_once_f from i386 code, where the predicate is 4 bytes (8 on the host), so m32 runs them
 * itself. Eight threads race for each; each block and function runs once, and the others wait for it to finish.
 * dispatchonce.expected is the x86_64 build's output under Rosetta. */
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static dispatch_once_t once_block, once_fn;
static volatile int runs_block, runs_fn, ready_block, ready_fn, early;

static void run_fn(void *ctx)
{
    __sync_fetch_and_add((volatile int *)ctx, 1);
    usleep(20000);
    ready_fn = 1;
}

static void *worker(void *arg)
{
    (void)arg;
    dispatch_once(&once_block, ^{
        __sync_fetch_and_add(&runs_block, 1);
        usleep(20000);
        ready_block = 1;
    });
    early += !ready_block;
    dispatch_once_f(&once_fn, (void *)&runs_fn, run_fn);
    early += !ready_fn;
    return NULL;
}

int main(void)
{
    pthread_t t[8];
    for (int i = 0; i < 8; i++)
        pthread_create(&t[i], NULL, worker, NULL);
    for (int i = 0; i < 8; i++)
        pthread_join(t[i], NULL);
    printf("block ran %d, function ran %d, returned early %d, predicates set %d %d\n", runs_block, runs_fn, early,
           once_block != 0, once_fn != 0);
    return 0;
}

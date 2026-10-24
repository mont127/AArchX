/*
 * The .ord note beside a program's profile (src/tcache.c, profile_ord_decide):
 * a launch that needs ordered memory early, here for a second thread made at
 * once, leaves it, the next launch starts ordered, and one that never needs
 * it ("single") takes it away.  Each launch must count right either way.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static volatile long g_sum;

static void *work(void *arg)
{
    for (long i = 0; i < 100000; i++)
        __atomic_add_fetch(&g_sum, 1, __ATOMIC_RELAXED);
    return arg;
}

int main(int argc, char **argv)
{
    int threads = !(argc > 1 && !strcmp(argv[1], "single"));
    pthread_t t;
    if (threads && pthread_create(&t, NULL, work, NULL) != 0)
        return 1;
    work(NULL);
    if (threads)
        pthread_join(t, NULL);
    if (g_sum != (threads ? 200000 : 100000)) {
        printf("BAD %ld\n", g_sum);
        return 1;
    }
    printf("OK\n");
    return 0;
}

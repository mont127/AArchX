/*
 * Atomicity and lock correctness under contention: eight guest threads each do
 * PER atomic fetch-add on one counter, then PER mutex-protected increments on
 * another.  A lost update - a non-atomic RMW, or a lock that does not actually
 * serialize - shows up as a final total below the expected one.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#define NT 8
#define PER 400000

static _Atomic long acnt;
static long lcnt;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;

static void *aw(void *a)
{
    (void)a;
    for (int i = 0; i < PER; i++)
        atomic_fetch_add_explicit(&acnt, 1, memory_order_relaxed);
    return NULL;
}

static void *lw(void *a)
{
    (void)a;
    for (int i = 0; i < PER; i++) {
        pthread_mutex_lock(&lk);
        lcnt++;
        pthread_mutex_unlock(&lk);
    }
    return NULL;
}

int main(void)
{
    pthread_t t[NT];
    for (int i = 0; i < NT; i++)
        if (pthread_create(&t[i], NULL, aw, NULL))
            return 2;
    for (int i = 0; i < NT; i++)
        pthread_join(t[i], NULL);
    for (int i = 0; i < NT; i++)
        if (pthread_create(&t[i], NULL, lw, NULL))
            return 2;
    for (int i = 0; i < NT; i++)
        pthread_join(t[i], NULL);

    if (acnt != (long)NT * PER || lcnt != (long)NT * PER) {
        printf("BAD atomic=%ld lock=%ld want=%d\n", acnt, lcnt, NT * PER);
        return 1;
    }
    printf("OK\n");
    return 0;
}

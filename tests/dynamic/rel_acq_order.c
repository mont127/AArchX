/* x86-TSO release/acquire ordering across two guest threads, deterministically.
 * A handshake ping-pong: the reader touches shared[] ONLY after the writer
 * hands the turn over with a release store, and the writer only overwrites it
 * after the reader hands back.  shared[] is plain (non-atomic) on purpose, so
 * nothing but the turn store's ordering protects it.  If ocerz let a shared[]
 * write float past the release (weaker than x86-TSO), or a read float above
 * the acquire, the reader would observe a stale value and errs>0.  This pins
 * the memory-ordering correctness that a translator is tempted to trade away
 * for speed -- 0 errors is the only pass. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#define ROUNDS 300000
#define N 8

static _Atomic int turn;      /* 0 = writer's turn, 1 = reader's turn */
static long shared[N];
static long errs;

static void *writer(void *a)
{
    (void)a;
    for (long r = 1; r <= ROUNDS; r++) {
        while (atomic_load_explicit(&turn, memory_order_acquire) != 0) { }
        for (int k = 0; k < N; k++)
            shared[k] = r * N + k;
        atomic_store_explicit(&turn, 1, memory_order_release);
    }
    return NULL;
}

static void *reader(void *a)
{
    (void)a;
    long e = 0;
    for (long r = 1; r <= ROUNDS; r++) {
        while (atomic_load_explicit(&turn, memory_order_acquire) != 1) { }
        for (int k = 0; k < N; k++)
            if (shared[k] != r * N + k)
                e++;
        atomic_store_explicit(&turn, 0, memory_order_release);
    }
    errs = e;
    return NULL;
}

int main(void)
{
    pthread_t w, rd;
    if (pthread_create(&w, NULL, writer, NULL) || pthread_create(&rd, NULL, reader, NULL))
        return 2;
    pthread_join(w, NULL);
    pthread_join(rd, NULL);
    if (errs) {
        printf("BAD %ld ordering violations\n", errs);
        return 1;
    }
    printf("OK\n");
    return 0;
}

/*
 * Hardware TSO (src/cpu.c, src/jit.c "memory ordering"): whatever ordered model a
 * multithreaded process ends up in, its locked instructions stay atomic and x86's
 * store order holds.  Four threads add to one counter with lock add and lock xadd
 * and move a second one with a lock cmpxchg loop, and a lock cmpxchg spinlock
 * released by a plain store guards a third, plain one; then a message-passing
 * litmus test: a writer stores x then y, a reader loads y then x, and y set with x
 * clear never happens under x86's order.  "atomic" runs the counters but not the
 * spinlock and the litmus, which only the order makes right: the unsafe
 * OCERZ_PRETEND_HW_TSO runs translated code without that order and still has to
 * keep every atomic one.  "count" prints the litmus's forbidden outcomes.  A first
 * thread, joined before anything else starts, is the one
 * OCERZ_PRETEND_HW_TSO_REFUSE=2 has refused: the process gives the order up while
 * main waits for it, so no code runs without one order or the other.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { THREADS = 4, ADDS = 200000, LOCKED = 50000, MP_N = 1 << 17, MP_ROUNDS = 6, LINE = 128 };

static uint64_t add_counter __attribute__((aligned(64)));
static uint64_t cas_counter __attribute__((aligned(64)));
static uint32_t spin __attribute__((aligned(64)));
static uint64_t guarded __attribute__((aligned(64)));
static int with_lock;

static void *adder(void *arg)
{
    (void)arg;
    for (int i = 0; i < ADDS; i++) {
        __atomic_fetch_add(&add_counter, 1, __ATOMIC_SEQ_CST);
        if (__atomic_fetch_add(&add_counter, 1, __ATOMIC_SEQ_CST) == UINT64_MAX)
            abort();
        uint64_t v = __atomic_load_n(&cas_counter, __ATOMIC_RELAXED);
        while (!__atomic_compare_exchange_n(&cas_counter, &v, v + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
        }
    }
    for (int i = 0; with_lock && i < LOCKED; i++) {
        uint32_t z = 0;
        while (!__atomic_compare_exchange_n(&spin, &z, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            z = 0;
        guarded = guarded + 1;
        __asm__ volatile("movl $0, %0" : "=m"(spin) : : "memory");
    }
    return NULL;
}

static char *xs, *ys;
static volatile int go;
#define X(i) (*(volatile int *)(xs + (size_t)(i) * LINE))
#define Y(i) (*(volatile int *)(ys + (size_t)(i) * LINE))

static void *writer(void *arg)
{
    (void)arg;
    while (!go) {
    }
    for (int i = 0; i < MP_N; i++) {
        X(i) = 1;
        __asm__ volatile("" ::: "memory");
        Y(i) = 1;
    }
    return NULL;
}

static long litmus(void)
{
    xs = calloc(MP_N, LINE);
    ys = calloc(MP_N, LINE);
    long bad = 0;
    for (int r = 0; r < MP_ROUNDS; r++) {
        memset(xs, 0, (size_t)MP_N * LINE);
        memset(ys, 0, (size_t)MP_N * LINE);
        go = 0;
        pthread_t t;
        pthread_create(&t, NULL, writer, NULL);
        go = 1;
        for (int i = 0; i < MP_N; i++) {
            while (Y(i) == 0 && X(i) == 0) {
            }
            int r1 = Y(i);
            __asm__ volatile("" ::: "memory");
            int r2 = X(i);
            if (r1 == 1 && r2 == 0)
                bad++;
        }
        pthread_join(t, NULL);
    }
    return bad;
}

static void *first(void *arg)
{
    return arg;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "full";
    pthread_t t0;
    pthread_create(&t0, NULL, first, NULL);
    pthread_join(t0, NULL);
    if (!strcmp(mode, "count")) {
        printf("%ld\n", litmus());
        return 0;
    }
    with_lock = strcmp(mode, "atomic") != 0;
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, adder, NULL);
    for (int i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);
    int ok = add_counter == 2ull * THREADS * ADDS && cas_counter == (uint64_t)THREADS * ADDS &&
             guarded == (with_lock ? (uint64_t)THREADS * LOCKED : 0);
    long bad = with_lock ? litmus() : 0;
    if (!ok || bad) {
        printf("BAD add=%llu cas=%llu guarded=%llu forbidden=%ld\n", (unsigned long long)add_counter,
               (unsigned long long)cas_counter, (unsigned long long)guarded, bad);
        return 1;
    }
    printf("OK\n");
    return 0;
}

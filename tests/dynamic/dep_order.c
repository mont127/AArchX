/*
 * x86-TSO load-load order through a chain of dependent loads (dep_plain_mark
 * in src/jit.c): a load whose next load is addressed through it may be a plain
 * ldr, because arm64 orders the two by the address dependency, but the chain's
 * last load must still keep every later load behind it.
 *
 * The writer fills a node that is never reused, stores data, then publishes the
 * node through head, in that order.  The reader loads head, the node's round
 * through it, then data, which depends on neither.  x86 keeps loads in order,
 * so a reader that sees round r in the node must see data >= r.  If the load of
 * the round were relaxed too, the load of data could be satisfied before it.
 * The loads are written in assembly so the compiler keeps their order and form.
 * The litmus part rarely trips even on a broken rule, so the program also
 * prints the three loads' addresses, and run_dynamic_tests.sh checks against
 * OCERZ_DEP_PLAIN_LOG that only the first, which the second depends on, was
 * made plain.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ROUNDS 2000000

struct node { int64_t round; };

struct node *g_head;
int64_t g_data;
static struct node *nodes;
static volatile int done;

static void *writer(void *a)
{
    (void)a;
    for (int64_t r = 1; r <= ROUNDS; r++) {
        struct node *nd = &nodes[r];
        __asm__ volatile(
            "movq %0, (%1)\n\t"
            "movq %0, _g_data(%%rip)\n\t"
            "movq %1, _g_head(%%rip)\n\t"
            : : "r"(r), "r"(nd) : "memory");
    }
    done = 1;
    return NULL;
}

extern char dep_l1[], dep_l2[], dep_l3[];

__attribute__((noinline)) static void *reader(void *a)
{
    (void)a;
    long bad = 0, seen = 0;
#pragma clang loop unroll(disable)
    while (!done) {
        int64_t round, data;
        __asm__ volatile(
            "_dep_l1: movq _g_head(%%rip), %%rax\n\t"
            "_dep_l2: movq (%%rax), %0\n\t"
            "_dep_l3: movq _g_data(%%rip), %1\n\t"
            : "=&r"(round), "=&r"(data) : : "rax", "memory");
        if (round) {
            seen++;
            if (data < round) bad++;
        }
    }
    return (void *)(intptr_t)(bad + (seen ? 0 : 1));
}

int main(void)
{
    nodes = calloc(ROUNDS + 1, sizeof *nodes);
    static struct node zero;
    g_head = &zero;
    pthread_t w, r;
    void *bad;
    pthread_create(&r, NULL, reader, NULL);
    pthread_create(&w, NULL, writer, NULL);
    pthread_join(w, NULL);
    pthread_join(r, &bad);
    if (bad) {
        printf("dep_order bad=%ld\n", (long)(intptr_t)bad);
        return 1;
    }
    printf("OK\n");
    fprintf(stderr, "loads %#llx %#llx %#llx\n", (unsigned long long)(uintptr_t)dep_l1,
            (unsigned long long)(uintptr_t)dep_l2, (unsigned long long)(uintptr_t)dep_l3);
    return 0;
}

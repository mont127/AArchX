#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

static uint64_t data __attribute__((aligned(128)));
static uint64_t flag __attribute__((aligned(128)));
enum { N = 3000000 };

static void *writer(void *a)
{
    (void)a;
    for (uint64_t i = 1; i <= N; i++) {
        __asm__ volatile("movq %1, %0" : "=m"(data) : "r"(i) : "memory");
        __asm__ volatile("movq %1, %%xmm0\n\tmovq %%xmm0, %0" : "=m"(flag) : "r"(i) : "xmm0", "memory");
    }
    return NULL;
}

int main(void)
{
    pthread_t t;
    pthread_create(&t, NULL, writer, NULL);
    uint64_t f = 0, d = 0;
    long bad = 0;
    while (f < N) {
        __asm__ volatile("movq %1, %%xmm1\n\tmovq %%xmm1, %0" : "=r"(f) : "m"(flag) : "xmm1", "memory");
        __asm__ volatile("movq %1, %0" : "=r"(d) : "m"(data) : "memory");
        if (d < f)
            bad++;
    }
    pthread_join(t, NULL);
    if (bad)
        printf("BAD %ld\n", bad);
    else
        printf("OK\n");
    return bad != 0;
}

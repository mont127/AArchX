/*
 * Instructions src/jit.c used to leave to the interpreter, which real libraries
 * run in their hot paths: rdtsc and rdtscp (the x86 mach_absolute_time reads the
 * time stamp counter), lfence, mfence and sfence, sidt (x86 malloc reads the cpu
 * number from it to pick a magazine), and adc and sbb with a memory source
 * (multi-word arithmetic).  Prints facts that hold under Rosetta and ocerz alike:
 * the counter never runs backwards and ticks at 1 GHz against the wall clock,
 * sidt writes ten bytes, and exact multi-word sums and differences.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t rdtscp(void)
{
    uint32_t lo, hi, aux;
    __asm__ volatile("rdtscp\n\tmfence" : "=a"(lo), "=d"(hi), "=c"(aux) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* a[0..3] += b[0..3], carried through adc with each b word in memory */
static void add256(uint64_t a[4], const uint64_t b[4])
{
    __asm__ volatile(
        "mov 0(%0), %%rax\n\t" "add 0(%1), %%rax\n\t" "mov %%rax, 0(%0)\n\t"
        "mov 8(%0), %%rax\n\t" "adc 8(%1), %%rax\n\t" "mov %%rax, 8(%0)\n\t"
        "mov 16(%0), %%rax\n\t" "adc 16(%1), %%rax\n\t" "mov %%rax, 16(%0)\n\t"
        "mov 24(%0), %%rax\n\t" "adc 24(%1), %%rax\n\t" "mov %%rax, 24(%0)\n\t"
        : : "r"(a), "r"(b) : "rax", "cc", "memory");
}

/* a[0..3] -= b[0..3], borrowed through sbb with each b word in memory; 32-bit halves too */
static void sub256(uint64_t a[4], const uint64_t b[4], uint32_t *lo32)
{
    __asm__ volatile(
        "mov 0(%0), %%rax\n\t" "sub 0(%1), %%rax\n\t" "mov %%rax, 0(%0)\n\t"
        "mov 8(%0), %%rax\n\t" "sbb 8(%1), %%rax\n\t" "mov %%rax, 8(%0)\n\t"
        "mov 16(%0), %%rax\n\t" "sbb 16(%1), %%rax\n\t" "mov %%rax, 16(%0)\n\t"
        "mov 24(%0), %%rax\n\t" "sbb 24(%1), %%rax\n\t" "mov %%rax, 24(%0)\n\t"
        "mov 0(%1), %%eax\n\t" "sub 8(%1), %%eax\n\t" "sbb 12(%1), %%eax\n\t" "mov %%eax, (%2)\n\t"
        : : "r"(a), "r"(b), "r"(lo32) : "rax", "cc", "memory");
}

int main(void)
{
    struct timespec w0, w1;
    clock_gettime(CLOCK_MONOTONIC_RAW, &w0);
    uint64_t t0 = rdtsc(), prev = t0;
    int backwards = 0;
    for (int i = 0; i < 2000000; i++) {
        uint64_t t = (i & 1) ? rdtscp() : rdtsc();
        backwards += t < prev;
        prev = t;
        __asm__ volatile("sfence" ::: "memory");
    }
    usleep(100000);
    uint64_t t1 = rdtsc();
    clock_gettime(CLOCK_MONOTONIC_RAW, &w1);
    double ns = (double)(w1.tv_sec - w0.tv_sec) * 1e9 + (double)(w1.tv_nsec - w0.tv_nsec);
    double ghz = (double)(t1 - t0) / ns;
    printf("tsc backwards=%d rate=%s\n", backwards, ghz > 0.95 && ghz < 1.05 ? "1GHz" : "off");

    unsigned char buf[32];
    memset(buf, 0xa5, sizeof buf);
    __asm__ volatile("sidt 8(%0)" : : "r"(buf) : "memory");
    int untouched = 0;
    for (int i = 0; i < 8; i++) untouched += buf[i] == 0xa5;
    for (int i = 18; i < 32; i++) untouched += buf[i] == 0xa5;
    printf("sidt guard bytes intact=%d of 22\n", untouched);

    uint64_t a[4] = { 0xffffffffffffffffull, 0xffffffffffffffffull, 0x7fffffffffffffffull, 1 };
    uint64_t b[4] = { 1, 0, 0x8000000000000000ull, 2 };
    for (int r = 0; r < 1000; r++) {
        add256(a, b);
        b[0] = b[0] * 6364136223846793005ull + 1442695040888963407ull;
        b[1] ^= a[0];
    }
    printf("add256 %016llx %016llx %016llx %016llx\n", (unsigned long long)a[3], (unsigned long long)a[2],
           (unsigned long long)a[1], (unsigned long long)a[0]);
    uint32_t lo = 0;
    for (int r = 0; r < 1000; r++) {
        sub256(a, b, &lo);
        b[2] = b[2] * 2862933555777941757ull + 3037000493ull;
    }
    printf("sub256 %016llx %016llx %016llx %016llx lo %08x\n", (unsigned long long)a[3], (unsigned long long)a[2],
           (unsigned long long)a[1], (unsigned long long)a[0], lo);
    return 0;
}

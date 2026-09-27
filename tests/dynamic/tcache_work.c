/*
 * A program run several times against one translation cache directory, so the
 * later runs execute blocks that an earlier process translated and stored.  It
 * reaches the shapes whose code carries process-specific values: libc calls
 * through stubs and the shared cache (leaf routines, the dispatch stub),
 * indirect calls through a table, deep call/return chains (return-address
 * slots and cells), floating point, and time lookups that read the commpage.
 * Every result is checked against a second computation done a different way,
 * and the program prints OK only when all of them agree.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

typedef unsigned long ul;
static ul op_add(ul a, ul b) { return a + b; }
static ul op_sub(ul a, ul b) { return a - b; }
static ul op_mul(ul a, ul b) { return a * b; }
static ul op_xor(ul a, ul b) { return a ^ b; }
static ul (*const ops[4])(ul, ul) = { op_add, op_sub, op_mul, op_xor };

static long fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

int main(void)
{
    enum { N = 30000 };
    int *v = malloc(N * sizeof *v), *w = malloc(N * sizeof *w);
    if (!v || !w)
        return 1;
    unsigned s = 12345;
    for (int i = 0; i < N; i++) {
        s = s * 1103515245u + 12345u;
        v[i] = w[i] = (int)(s >> 8) % 100000;
    }
    qsort(v, N, sizeof *v, cmp_int);
    for (int i = 1; i < N; i++)
        if (v[i - 1] > v[i])
            return 2;
    long hv = 0, hw = 0;
    for (int i = 0; i < N; i++) {
        hv += v[i];
        hw += w[i];
    }
    if (hv != hw)
        return 3;

    ul acc = 1, ref = 1;
    for (int i = 0; i < 100000; i++) {
        ul x = (ul)((unsigned)i * 2654435761u >> 7);
        acc = ops[i & 3](acc, x);
        switch (i & 3) {
        case 0: ref = ref + x; break;
        case 1: ref = ref - x; break;
        case 2: ref = ref * x; break;
        default: ref = ref ^ x; break;
        }
    }
    if (acc != ref)
        return 4;

    if (fib(24) != 46368)
        return 5;

    double d = 0.0;
    for (int i = 1; i <= 20000; i++)
        d += 1.0 / ((double)i * (double)i);
    if (d < 1.6448 || d > 1.6450)
        return 6;

    char buf[64];
    size_t total = 0;
    for (int i = 0; i < 2000; i++) {
        snprintf(buf, sizeof buf, "%d:%x:%.2f", v[i * 13], v[i * 13], v[i * 13] / 3.0);
        total += strlen(buf);
    }
    if (total < 2000 * 7)
        return 7;

    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    clock_gettime(CLOCK_MONOTONIC, &b);
    if (b.tv_sec < a.tv_sec || (b.tv_sec == a.tv_sec && b.tv_nsec < a.tv_nsec))
        return 8;

    free(v);
    free(w);
    printf("OK\n");
    return 0;
}

/*
 * tests/guest/fibn.c
 *
 * A TWO-POINT variant of bench.c's `call` workload, and it exists for exactly
 * one reason: bench's own `call` number is startup-contaminated and therefore
 * structurally unreliable as a ratio.
 *
 * fib(34) under Rosetta runs in ~0.039s, of which ~0.015s is process startup
 * (dyld, translation of the startup path, page-ins). A ratio computed from a
 * 0.039s wall clock is measuring startup as much as translation, which is how
 * the same workload came to be reported as 10.54x, 12.54x and 15.86x by three
 * different measurements of the same binary.
 *
 * The fix is a two-point regression. Startup cost S is a CONSTANT independent
 * of n, so timing two different n and subtracting cancels it exactly:
 *
 *     t(n)   = S + k * nodes(n)
 *     t(n+2) = S + k * nodes(n+2)
 *     k = (t(n+2) - t(n)) / (nodes(n+2) - nodes(n))      <- S is gone
 *
 * k is the true per-node cost, in seconds, with no startup term at all. That
 * is the number to compare against Rosetta, and it is the metric this stage
 * uses for `call` in place of the wall-clock ratio.
 *
 * nodes(n) = the number of fib() invocations = 2*fib(n+1) - 1. For n=34 that
 * is 18,454,929. This was VERIFIED by direct instrumented count (a nodes++ in
 * fib) before being replaced by the closed form here, because the figure had
 * been disputed: 9,227,465 is fib(35), i.e. the VALUE, and is not the node
 * count. Confirmed at two points: nodes(34)=18,454,929, nodes(36)=48,315,633.
 *
 * fib() below is deliberately IDENTICAL to bench.c's -- pure, no counter. An
 * instrumented fib would add a global increment to every node and measure a
 * different call shape than the workload we are trying to characterize; the
 * node count is therefore computed analytically by an iterative fib in main,
 * which costs nothing and does not perturb the recursion under test.
 *
 * argv[1] is the fib argument as a plain decimal (no libc; parsed by hand).
 * Output is the fib value and the node count, both deterministic, so this
 * doubles as a correctness cross-check between Rosetta, -no-jit and the JIT.
 *
 * Built into benchbin/, never bin/: run_diff_test.sh and run_guest_tests.sh
 * glob the bin directory, and a benchmark there becomes a differential case
 * (see the comment in tests/guest/Makefile).
 */
#include "gsys.h"

/* IDENTICAL to bench.c's fib, character for character. That is the point: this
 * binary must exercise the same call/ret shape the `call` workload does, or the
 * per-node cost it regresses out describes a different program. */
static g_u64 fib(unsigned n)
{
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}

/* nodes(n) = 2*fib(n+1) - 1, computed iteratively so it adds no measurable work
 * and, crucially, does not touch the recursion under test. */
static g_u64 nodes_for(unsigned n)
{
    g_u64 a = 0, b = 1;                   /* a=fib(0), b=fib(1) */
    for (unsigned i = 0; i < n + 1u; i++) {
        g_u64 t = a + b;
        a = b; b = t;
    }
    return 2u * a - 1u;                   /* a == fib(n+1) */
}

int main(int argc, char **argv)
{
    unsigned n = 34;

    if (argc > 1) {
        unsigned v = 0;
        const char *p = argv[1];
        /* Hand-rolled: -nostdlib, so there is no strtoul. */
        for (; *p >= '0' && *p <= '9'; p++)
            v = v * 10u + (unsigned)(*p - '0');
        if (v)
            n = v;
    }

    g_u64 r = fib(n);
    g_putu64(r);
    g_putu64(nodes_for(n));
    return 0;
}

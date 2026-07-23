/*
 * tests/guest/memn.c
 *
 * TWO-POINT variant of bench.c's `mem` workload, for exactly the same reason
 * fibn.c exists for `call`: bench's mem wall clock is dominated by process
 * startup (Rosetta mem ~0.03-0.06s of which most is startup + the fixed
 * 131072-element reduction), so it is unusable as a throughput ratio.
 *
 * The mem loop body is character-for-character identical to bench.c's, so this
 * binary exercises the SAME guest load/store shape the mem workload does. Only
 * the iteration count N is lifted to argv[1]. Startup cost S (dyld, image load,
 * JIT warmup) and the fixed final reduction are both CONSTANT in N, so:
 *
 *     t(N)  = S + k * N
 *     k = (t(N2) - t(N1)) / (N2 - N1)        <- S cancels exactly
 *
 * k is the true per-iteration cost (2 guest loads + 2 guest stores of the
 * unrolled body, plus the index ALU). Compare k against Rosetta's k, not the
 * raw wall clock.
 *
 * CROSS-CHECK: run with N=40000000 and the checksum MUST equal bench mem's
 * golden 1598560506013336 (buf identical, same loop) -- this validates the
 * replica before any timing is trusted.
 *
 * argv[1] parsed by hand (-nostdlib, no strtoull). Built into benchbin/, never
 * bin/ (same rule as bench/fibn: the runners glob every entry in bin/).
 */
#include "gsys.h"

int main(int argc, char **argv)
{
    g_u64 N = 40000000ULL;

    if (argc > 1) {
        g_u64 v = 0;
        const char *p = argv[1];
        for (; *p >= '0' && *p <= '9'; p++)
            v = v * 10u + (g_u64)(*p - '0');
        if (v)
            N = v;
    }

    static g_u64 buf[131072];
    g_u64 r = 0;
    for (g_u64 i = 0; i < N; i++) {
        g_u64 k = i & 131071;
        buf[k] = buf[(k * 7 + 1) & 131071] + i;
    }
    for (g_u64 i = 0; i < 131072; i++) r += buf[i];

    g_putu64(r);
    return 0;
}

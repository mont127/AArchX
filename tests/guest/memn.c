/* Memory-bound throughput loop, iteration count from argv[1]. */
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

/* Cross-thread-exit safety net for two-way Jcc linking. */
#include "gsys.h"

int main(void)
{
    g_puts("start\n");

    volatile g_u64 stop = 0;
    volatile g_u64 seed = 0x9e3779b9u;
    g_u64 x = 1, y = 2;

    while (!stop) {
        g_u64 s = seed;
        if (s & 1u)
            x = x * 3u + y;
        else
            y = y * 5u + x;

    }

    g_puts("spun ");
    g_putu64(x + y);
    return 0;
}

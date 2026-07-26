/* Cross-thread-exit safety net for block chaining. */
#include "gsys.h"

int main(void)
{
    g_puts("start\n");

    volatile g_u64 stop = 0;
    g_u64 x = 0;
    while (!stop)
        x = x + 1;

    g_puts("spun ");
    g_putu64(x);
    return 0;
}

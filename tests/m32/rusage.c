/* getrusage from i386 code: struct rusage is two 8-byte timevals and fourteen 4-byte longs (72 bytes). Left 4 Dead 2's
 * engine reads it when its local server starts. Expected output written by hand. */
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

int main(void)
{
    struct rusage ru;
    memset(&ru, 0xee, sizeof ru);
    volatile double x = 0;
    for (int i = 0; i < 20000000; i++)
        x += i * 0.5;
    int r = getrusage(RUSAGE_SELF, &ru);
    long us = ru.ru_utime.tv_sec * 1000000L + ru.ru_utime.tv_usec;
    printf("size %u result %d\n", (unsigned)sizeof ru, r);
    printf("user time %d usec in range %d %d\n", us > 0, ru.ru_utime.tv_usec >= 0 && ru.ru_utime.tv_usec < 1000000,
           ru.ru_stime.tv_usec >= 0 && ru.ru_stime.tv_usec < 1000000);
    printf("maxrss %d nivcsw %d\n", ru.ru_maxrss > 0, ru.ru_nivcsw != (long)0xeeeeeeee);
    printf("bad who %d\n", getrusage(42, &ru));
    return 0;
}

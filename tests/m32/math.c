#include <math.h>
#include <stdio.h>
#include <stdint.h>
static void put_num(long long v) { char b[32]; int i = 31; b[i] = 0; int neg = v < 0; unsigned long long u = neg ? -v : v;
    do { b[--i] = '0' + u % 10; u /= 10; } while (u); if (neg) b[--i] = '-'; puts(b + i); }
volatile double d2 = 2.0; volatile float f3 = 3.0f; volatile uint64_t big = 10000000000ull; volatile int64_t neg = -10000000007ll;
int main(void) {
    put_num((long long)(pow(d2, 10.0)));             /* double args, ST0 result */
    put_num((long long)(sqrt(d2) * 1000000));       /* 1414213 */
    put_num((long long)(sinf(f3) * 1000000));       /* float arg, ST0 result: 141120 */
    put_num((long long)(fmodf(7.5f, f3) * 10));     /* 15 */
    put_num(lround(2.5));                           /* long result: 3 */
    put_num((long long)(big / 7));                  /* __udivdi3: 1428571428 */
    put_num((long long)(big % 7));                  /* __umoddi3: 4 */
    put_num(neg / 3);                               /* __divdi3: -3333333335 */
    put_num(neg % 3);                               /* __moddi3: -2 */
    put_num((long long)(double)big);                /* __floatundidf or inline: 10000000000 */
    return 0;
}

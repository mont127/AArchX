/* Regression test for the block-length-cap rip hazard. */
#include "gsys.h"

#define STEP(k)                          \
    a += b;                              \
    a ^= (g_u32)(0x9e3779b9u + (k));     \
    b -= a;                              \
    b &= 0x7fffffffu;                    \
    a |= (b >> 3);                       \
    a -= (g_u32)(k);                     \
    b ^= (a << 5);                       \
    a += (b | (g_u32)(k));               \
    b += (a & 0x00ffff00u);              \
    a ^= (b - (g_u32)((k) * 7u));

#define STEP4(k)  STEP(k) STEP((k) + 1) STEP((k) + 2) STEP((k) + 3)
#define STEP16(k) STEP4(k) STEP4((k) + 4) STEP4((k) + 8) STEP4((k) + 12)
#define STEP64(k) STEP16(k) STEP16((k) + 16) STEP16((k) + 32) STEP16((k) + 48)

int main(void)
{
    volatile g_u32 vs0 = 0x01234567u;
    volatile g_u32 vs1 = 0x89abcdefu;
    g_u32 a = vs0;
    g_u32 b = vs1 ^ 0xa5a5a5a5u;

    STEP64(1)
    STEP64(65)
    STEP64(129)

    g_puthex64(((g_u64)a << 32) | (g_u64)b);
    return 0;
}

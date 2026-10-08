/*
 * shufps of a register with itself and of two registers, for all 256
 * immediates each: src/jit.c emits a broadcast as one dup, a change to a single
 * lane as one ins, the transpose selections (0x44, 0xee, 0x88, 0xdd, 0xe4,
 * 0x4e) as one zip, uzp, ins or ext, the rest through the general shuffle.
 * Each result is printed, against Rosetta.  Also a rip-relative constant load
 * and add, which the Wine layout folds into one host constant.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CASE(n)                                                                     \
    {                                                                               \
        uint32_t out[4];                                                            \
        __asm__ volatile("movups (%1), %%xmm0\n\t"                                  \
                         "shufps $" #n ", %%xmm0, %%xmm0\n\t"                       \
                         "movups %%xmm0, (%0)"                                      \
                         : : "r"(out), "r"(in) : "xmm0", "memory");                 \
        printf("%3d %08x %08x %08x %08x\n", n, out[0], out[1], out[2], out[3]);     \
    }
#define CASE2(n)                                                                    \
    {                                                                               \
        uint32_t out[4];                                                            \
        __asm__ volatile("movups (%1), %%xmm0\n\t"                                  \
                         "movups 16(%1), %%xmm1\n\t"                                \
                         "shufps $" #n ", %%xmm1, %%xmm0\n\t"                       \
                         "movups %%xmm0, (%0)"                                      \
                         : : "r"(out), "r"(in) : "xmm0", "xmm1", "memory");         \
        printf("2 %3d %08x %08x %08x %08x\n", n, out[0], out[1], out[2], out[3]);   \
    }
#define C4(n) CASE(n) CASE(n + 1) CASE(n + 2) CASE(n + 3) CASE2(n) CASE2(n + 1) CASE2(n + 2) CASE2(n + 3)
#define C16(n) C4(n) C4(n + 4) C4(n + 8) C4(n + 12)
#define C64(n) C16(n) C16(n + 16) C16(n + 32) C16(n + 48)

static const float kscale = 1.5f;
__attribute__((noinline)) static float scaled(float x) { return x * kscale + kscale; }

int main(void)
{
    static const uint32_t in[8] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444,
                                    0x55555555, 0x66666666, 0x77777777, 0x88888888 };
    C64(0) C64(64) C64(128) C64(192)
    float acc = 0;
    for (int i = 0; i < 1000; i++) acc += scaled((float)i);
    printf("rip constant %.1f\n", (double)acc);
    return 0;
}

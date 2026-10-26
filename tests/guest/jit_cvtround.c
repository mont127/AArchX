/* cvtsd2si and cvtss2si round by MXCSR.RC: every rounding mode, halfway cases, values at and past the 32- and
 * 64-bit limits, NaN, infinities, -0 and a denormal, into 32- and 64-bit registers, from registers and memory.
 * Their VEX forms too.  The JIT has translated them since 2026-10-26 (frinti, then cvttsd2si's conversion); the
 * interpreter did before. */
#include "gsys.h"
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
static const g_u64 dbits[] = {
    0x3fe0000000000000ULL, 0x3ff8000000000000ULL, 0x4004000000000000ULL, 0xbfe0000000000000ULL,
    0xbff8000000000000ULL, 0xc004000000000000ULL, 0x4003fffffffffffeULL, 0x4202a05f20020000ULL,
    0x41dfffffffd9999aULL, 0x41dfffffffe00000ULL, 0x41e0000000000000ULL, 0xc1e000000008cccdULL,
    0xc1e0000000133333ULL, 0x43dfffffffffffffULL, 0x43e0000000000000ULL, 0xc3e0000000000000ULL,
    0x7ff8000000000000ULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0x8000000000000000ULL,
    0x0000000000000001ULL, 0x7e37e43c8800759cULL,
};
static const unsigned fbits[] = {
    0x3f000000u, 0x3fc00000u, 0x40200000u, 0xbf000000u, 0xbfc00000u, 0xc0200000u, 0x4f000000u, 0xcf000000u,
    0xcf000001u, 0x5f000000u, 0xdf000000u, 0x7fc00000u, 0x7f800000u, 0xff800000u, 0x80000000u, 0x00000001u,
    0x4b7fffffu, 0x3effffffu,
};
static volatile g_u64 dmem;
static volatile unsigned fmem;
int main(void)
{
    unsigned saved;
    __asm__ volatile("stmxcsr %0" : "=m"(saved));
    for (unsigned rc = 0; rc < 4; rc++) {
        unsigned m = (saved & ~0x6000u) | (rc << 13);
        __asm__ volatile("ldmxcsr %0" : : "m"(m));
        put("rc ", rc);
        for (unsigned i = 0; i < sizeof dbits / sizeof dbits[0]; i++) {
            double d; g_u64 bits = dbits[i], r64; unsigned r32;
            __builtin_memcpy(&d, &bits, 8);
            dmem = bits;
            __asm__ volatile("cvtsd2si %1, %0" : "=r"(r64) : "x"(d));
            __asm__ volatile("cvtsd2si %1, %0" : "=r"(r32) : "x"(d));
            put(" d64 ", r64);
            put(" d32 ", r32);
            __asm__ volatile("cvtsd2siq %1, %0" : "=r"(r64) : "m"(dmem));
            __asm__ volatile("cvtsd2sil %1, %0" : "=r"(r32) : "m"(dmem));
            put(" d64m ", r64);
            put(" d32m ", r32);
            __asm__ volatile("vcvtsd2si %1, %0" : "=r"(r64) : "x"(d));
            __asm__ volatile("vcvtsd2sil %1, %0" : "=r"(r32) : "m"(dmem));
            put(" vd64 ", r64);
            put(" vd32m ", r32);
        }
        for (unsigned i = 0; i < sizeof fbits / sizeof fbits[0]; i++) {
            float f; unsigned bits = fbits[i], r32; g_u64 r64;
            __builtin_memcpy(&f, &bits, 4);
            fmem = bits;
            __asm__ volatile("cvtss2si %1, %0" : "=r"(r32) : "x"(f));
            __asm__ volatile("cvtss2si %1, %0" : "=r"(r64) : "x"(f));
            put(" f32 ", r32);
            put(" f64 ", r64);
            __asm__ volatile("cvtss2sil %1, %0" : "=r"(r32) : "m"(fmem));
            __asm__ volatile("cvtss2siq %1, %0" : "=r"(r64) : "m"(fmem));
            put(" f32m ", r32);
            put(" f64m ", r64);
            __asm__ volatile("vcvtss2si %1, %0" : "=r"(r32) : "x"(f));
            __asm__ volatile("vcvtss2siq %1, %0" : "=r"(r64) : "m"(fmem));
            put(" vf32 ", r32);
            put(" vf64m ", r64);
        }
    }
    __asm__ volatile("ldmxcsr %0" : : "m"(saved));
    g_u64 acc = 0;
    for (int i = -2000; i < 2000; i++) {
        double d = (double)i * 0.25; g_u64 r;
        __asm__ volatile("cvtsd2si %1, %0" : "=r"(r) : "x"(d));
        acc = acc * 31 + r;
    }
    put("loop ", acc);
    return 0;
}

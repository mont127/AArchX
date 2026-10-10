/* shl, shr and sar of memory by an immediate, with the flags dead after them (a test redefines them at once):
 * bytes, words and dwords (and qwords in the x86_64 build), counts 0 to 31 and the edges, on values with the sign
 * bit set and clear. Builds for i386 and x86_64; shiftmem.expected is the x86_64 build's output under Rosetta, and
 * the i386 lines are the same ones. */
#include <stdio.h>
#include <stdint.h>

#define SH(op, sfx, T, n)                                                                                    \
    static T op##sfx##_##n(T v)                                                                              \
    {                                                                                                        \
        volatile T m = v;                                                                                    \
        __asm__ volatile(#op #sfx " $" #n ", %0\n\ttest %%esp, %%esp" : "+m"(m) : : "cc");                    \
        return m;                                                                                            \
    }
#define COUNTS(op, sfx, T) SH(op, sfx, T, 0) SH(op, sfx, T, 1) SH(op, sfx, T, 2) SH(op, sfx, T, 7) SH(op, sfx, T, 8) \
    SH(op, sfx, T, 9) SH(op, sfx, T, 15) SH(op, sfx, T, 16) SH(op, sfx, T, 31)
#define ALL(sfx, T) COUNTS(shl, sfx, T) COUNTS(shr, sfx, T) COUNTS(sar, sfx, T)
ALL(b, uint8_t) ALL(w, uint16_t) ALL(l, uint32_t)

#define ROW(op, sfx, T, v)                                                                                   \
    printf("%s%s %08llx:", #op, #sfx, (unsigned long long)(T)(v));                                          \
    printf(" %llx %llx %llx %llx %llx %llx %llx %llx %llx\n", (unsigned long long)op##sfx##_0(v),            \
           (unsigned long long)op##sfx##_1(v), (unsigned long long)op##sfx##_2(v),                           \
           (unsigned long long)op##sfx##_7(v), (unsigned long long)op##sfx##_8(v),                           \
           (unsigned long long)op##sfx##_9(v), (unsigned long long)op##sfx##_15(v),                          \
           (unsigned long long)op##sfx##_16(v), (unsigned long long)op##sfx##_31(v))
#define ROWS(sfx, T, v) ROW(shl, sfx, T, v); ROW(shr, sfx, T, v); ROW(sar, sfx, T, v)

int main(void)
{
    static const uint32_t vals[] = { 0x00000000u, 0xffffffffu, 0x80000001u, 0x7fffffffu, 0x12345678u, 0xedcba987u };
    for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        ROWS(b, uint8_t, (uint8_t)vals[i]);
        ROWS(w, uint16_t, (uint16_t)vals[i]);
        ROWS(l, uint32_t, vals[i]);
    }
    return 0;
}

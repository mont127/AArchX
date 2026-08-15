#include "gsys.h"
static g_u64 acc;
static void mix(g_u64 v) { acc = (acc ^ v) * 0x9E3779B97F4A7C15ULL; acc ^= acc >> 29; }
int main(void) {
    volatile g_u64 u[8] = { 0, 1, 7, 0xffffffffULL, 0x100000000ULL, 0x8000000000000000ULL, 0x7fffffffffffffffULL, 123456789012345ULL };
    volatile g_i64 si[8] = { 1, -1, 7, -7, 0x7fffffffffffffffLL, (g_i64)0x8000000000000001ULL, 100000, -100000 };
    for (int i = 0; i < 8; i++) for (int j = 0; j < 8; j++) {
        g_u64 a = u[i], d = u[j] | 1; mix(a / d); mix(a % d);
        g_u32 a32 = (g_u32)u[i], d32 = (g_u32)u[j] | 1; mix(a32 / d32); mix(a32 % d32);
        g_i64 x = si[i], y = si[j]; if (y != 0 && !(x == (g_i64)0x8000000000000000ULL && y == -1)) { mix((g_u64)(x / y)); mix((g_u64)(x % y)); }
        int x32 = (int)si[i], y32 = (int)si[j]; if (y32 != 0 && !(x32 == (int)0x80000000 && y32 == -1)) { mix((g_u64)(g_u32)(x32 / y32)); mix((g_u64)(g_u32)(x32 % y32)); }
    }
    /* 128/64 with nonzero high half: force via inline asm */
    g_u64 q, r; g_u64 hi = 5, lo = 0x1234, dv = 0x10000;
    __asm__ volatile("divq %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "r"(dv));
    mix(q); mix(r);
    /* signed 128/64 with sext high half (cqo path) */
    g_i64 sq, sr; g_i64 slo = -1000000007, sdv = 12345;
    __asm__ volatile("cqto\n\tidivq %4" : "=a"(sq), "=d"(sr) : "a"(slo), "d"(0), "r"(sdv) : "cc");
    mix((g_u64)sq); mix((g_u64)sr);
    /* cbw/cwde/cdqe/cwd/cdq */
    volatile g_u64 v = 0x8f;
    __asm__ volatile("cbw" : "+a"(v)); mix(v);
    v = 0x8fff; __asm__ volatile("cwde" : "+a"(v)); mix(v);
    v = 0x8fffffff; __asm__ volatile("cdqe" : "+a"(v)); mix(v);
    { g_u64 dx; v = 0x8000; __asm__ volatile("cwd" : "=d"(dx) : "a"(v)); mix(dx); }
    { g_u64 dx; v = 0x80000000; __asm__ volatile("cdq" : "=d"(dx) : "a"(v)); mix(dx); }
    /* narrow arith */
    volatile unsigned char c = 200; c += 100; mix(c); c -= 250; mix(c); c ^= 0x55; mix(c);
    volatile unsigned short w = 65000; w += 1000; mix(w); w &= 0xff0f; mix(w); w |= 0x1234; mix(w);
    g_putu64(acc); return 0;
}

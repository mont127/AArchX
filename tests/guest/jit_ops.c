#include "gsys.h"
static g_u64 acc;
static void mix(g_u64 v) { acc = (acc ^ v) * 0x9E3779B97F4A7C15ULL; acc ^= acc >> 29; }
int main(int argc, char **argv) {
    const char *w = argc > 1 ? argv[1] : "";  /* no arg = every section */
    volatile g_u64 vals[6] = { 0x0123456789abcdefULL, 0xfedcba9876543210ULL, 1, 0, 0x8000000000000000ULL, 0x7fffffffffffffffULL };
    volatile unsigned cnts[6] = { 0, 1, 7, 31, 32, 63 };
    for (int i = 0; i < 6; i++) for (int j = 0; j < 6; j++) {
        g_u64 v = vals[i]; unsigned c = cnts[j]; g_u32 v32 = (g_u32)v; unsigned c32 = c & 31;
        if (w[0]=='s'||!w[0]) { mix(v << c); mix(v >> c); mix((g_u64)((g_i64)v >> c)); mix((g_u64)(v32 << c32)); mix((g_u64)(v32 >> c32)); mix((g_u64)(g_u32)((int)v32 >> c32)); }
        if (w[0]=='r'||!w[0]) { unsigned r = c & 63; mix((v << r) | (v >> ((64 - r) & 63))); mix((v >> r) | (v << ((64 - r) & 63))); unsigned r2 = c & 31; mix((g_u64)((v32 << r2) | (v32 >> ((32 - r2) & 31)))); mix((g_u64)((v32 >> r2) | (v32 << ((32 - r2) & 31)))); }
        if (w[0]=='n'||!w[0]) { mix(~v); mix((g_u64)-(g_i64)v); mix((g_u64)(g_u32)~v32); mix((g_u64)(g_u32)-(int)v32); mix(__builtin_bswap64(v)); mix((g_u64)__builtin_bswap32(v32)); }
        if (w[0]=='c'||!w[0]) { mix(v > vals[j] ? v : (g_u64)c); mix((g_i64)v < (g_i64)vals[j] ? 1u : 0u); mix(v == vals[j]); mix(v <= vals[j] ? v32 : c); mix((g_i64)v >= 0 ? 5 : 9); }
    }
    g_putu64(acc); return 0;
}

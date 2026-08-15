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
    /* all 16 conditions x {cmp,test} x sizes {1,2,4,8} via setcc, plus cmov */
    #define SET(cc, insn, xa, xb) do { g_u64 r_ = 0; __asm__ volatile(insn " %2, %1\n\tset" #cc " %b0" : "+r"(r_) : "r"(xb), "r"(xa) : "cc"); mix(r_ & 0xff); } while (0)
    #define SETALL(insn, xa, xb) do { SET(o,insn,xa,xb); SET(no,insn,xa,xb); SET(b,insn,xa,xb); SET(ae,insn,xa,xb); SET(e,insn,xa,xb); SET(ne,insn,xa,xb); SET(be,insn,xa,xb); SET(a,insn,xa,xb); SET(s,insn,xa,xb); SET(ns,insn,xa,xb); SET(p,insn,xa,xb); SET(np,insn,xa,xb); SET(l,insn,xa,xb); SET(ge,insn,xa,xb); SET(le,insn,xa,xb); SET(g,insn,xa,xb); } while (0)
    for (int i = 0; i < 6; i++) for (int j = 0; j < 6; j++) {
        g_u64 a = vals[i], b = vals[j] ^ (g_u64)cnts[j];
        SETALL("cmpq", a, b); SETALL("testq", a, b);
        g_u32 a32 = (g_u32)a, b32 = (g_u32)b;
        SETALL("cmpl", a32, b32); SETALL("testl", a32, b32);
        unsigned short a16 = (unsigned short)a, b16 = (unsigned short)b;
        SETALL("cmpw", a16, b16); SETALL("testw", a16, b16);
        unsigned char a8 = (unsigned char)a, b8 = (unsigned char)b;
        SETALL("cmpb", a8, b8); SETALL("testb", a8, b8);
        /* cmov after cmp of each size */
        { g_u64 r = 111; __asm__ volatile("cmpq %2, %1\n\tcmovg %2, %0" : "+r"(r) : "r"(a), "r"(b) : "cc"); mix(r); }
        { g_u32 r = 222; __asm__ volatile("cmpl %2, %1\n\tcmovbe %2, %0" : "+r"(r) : "r"(a32), "r"(b32) : "cc"); mix(r); }
        { g_u32 r = 333; __asm__ volatile("cmpw %w2, %w1\n\tcmovl %2, %0" : "+r"(r) : "r"((g_u32)a16), "r"((g_u32)b16) : "cc"); mix(r); }
        { g_u32 r = 444; __asm__ volatile("cmpb %b2, %b1\n\tcmovne %2, %0" : "+r"(r) : "r"((g_u32)a8), "r"((g_u32)b8) : "cc"); mix(r); }
        { g_u64 r = 555; __asm__ volatile("testq %1, %1\n\tcmovs %2, %0" : "+r"(r) : "r"(a), "r"(b) : "cc"); mix(r); }
    }
    g_putu64(acc); return 0;
}

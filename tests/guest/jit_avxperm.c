/* vpermq, vpermpd, vpermd, vpermps, vpermilps and vpermilpd with an immediate, vpblendd and vcvtps2pd: register
 * and memory sources, the destination aliasing the index or the table, index dwords with junk above bit 2, NaNs,
 * infinities and denormals through the conversion; vptest and vmovmskps/pd of both widths (PF left out: Rosetta
 * keeps it through vptest, which the SDM says clears it).  The JIT has translated them since 2026-10-26. */
#include "gsys.h"
typedef g_u64 v4q __attribute__((vector_size(32)));
typedef unsigned v8u __attribute__((vector_size(32)));
typedef unsigned v4u __attribute__((vector_size(16)));
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
__attribute__((target("avx2"))) static void puty(const char *tag, v4q v) { g_puts(tag); for (int i = 0; i < 4; i++) g_puthex64(v[i]); }
static volatile v4q ymem;
static volatile v4u xmem;
#define IMMS(X) X(0x1b) X(0x00) X(0xe4) X(0x4e) X(0xb1) X(0x93) X(0x0f) X(0xc6)
__attribute__((target("avx2"))) int main(void)
{
    v4q A = { 0x1111111111111111ULL, 0x2222222222222222ULL, 0x3333333333333333ULL, 0x4444444444444444ULL };
    v4q B = { 0xa0a1a2a3a4a5a6a7ULL, 0xb0b1b2b3b4b5b6b7ULL, 0xc0c1c2c3c4c5c6c7ULL, 0xd0d1d2d3d4d5d6d7ULL };
    v8u I = { 7, 0x10000002, 0xfffffff9, 3, 4, 0x80000005, 6, 0x123 };
    v4q r;
    ymem = B;
#define PQ(m) __asm__ volatile("vpermq $" #m ", %1, %0" : "=x"(r) : "x"(A)); puty("vpermq " #m " ", r); \
              __asm__ volatile("vpermpd $" #m ", %1, %0" : "=x"(r) : "m"(ymem)); puty(" vpermpd m ", r); \
              r = A; __asm__ volatile("vpermq $" #m ", %0, %0" : "+x"(r)); puty(" alias ", r); \
              __asm__ volatile("vpermilps $" #m ", %1, %0" : "=x"(r) : "x"(B)); puty(" vpermilps ", r); \
              __asm__ volatile("vpermilps $" #m ", %x1, %%xmm7\n\tvmovdqu %%ymm7, %0" : "=m"(ymem) : "x"(A) : "xmm7"); puty(" vpermilps x ", ymem); ymem = B; \
              __asm__ volatile("vpermilpd $" #m ", %1, %0" : "=x"(r) : "m"(ymem)); puty(" vpermilpd m ", r); \
              __asm__ volatile("vpblendd $" #m ", %2, %1, %0" : "=x"(r) : "x"(A), "x"(B)); puty(" vpblendd ", r); \
              __asm__ volatile("vpblendd $" #m ", %2, %1, %0" : "=x"(r) : "x"(B), "m"(ymem)); puty(" vpblendd m ", r); \
              r = A; __asm__ volatile("vpblendd $" #m ", %1, %0, %0" : "+x"(r) : "x"(B)); puty(" vpblendd alias ", r); \
              g_puts("\n");
    IMMS(PQ)
    for (int k = 0; k < 4; k++) {
        v8u ix = I;
        for (int j = 0; j < 8; j++) ix[j] = ix[j] + (unsigned)(k * 3 + j * 5);
        __asm__ volatile("vpermd %2, %1, %0" : "=x"(r) : "x"(ix), "x"(A)); puty("vpermd ", r);
        __asm__ volatile("vpermps %2, %1, %0" : "=x"(r) : "x"(ix), "m"(ymem)); puty(" vpermps m ", r);
        v4q t = (v4q)ix; __asm__ volatile("vpermd %1, %0, %0" : "+x"(t) : "x"(B)); puty(" d=idx ", t);
        t = B; __asm__ volatile("vpermd %0, %1, %0" : "+x"(t) : "x"(ix)); puty(" d=table ", t);
        g_puts("\n");
    }
    static const unsigned fb[] = { 0x3fc00000u, 0xc0200000u, 0x7fc00001u, 0xff800000u, 0x00000001u, 0x80000000u,
                                   0x7f7fffffu, 0xffa00000u };
    for (int k = 0; k < 2; k++) {
        v4u f = { fb[4 * k], fb[4 * k + 1], fb[4 * k + 2], fb[4 * k + 3] };
        xmem = f;
        __asm__ volatile("vcvtps2pd %1, %0" : "=x"(r) : "x"(f)); puty("vcvtps2pd ", r);
        __asm__ volatile("vcvtps2pd %1, %0" : "=x"(r) : "m"(xmem)); puty(" m ", r);
        __asm__ volatile("vcvtps2pd %1, %%xmm6\n\tvmovdqu %%ymm6, %0" : "=m"(ymem) : "x"(f) : "xmm6"); puty(" x ", ymem);
        g_puts("\n");
    }
    static const g_u64 pv[] = { 0, ~0ULL, 0x8000000000000001ULL, 0x00000000ffffffffULL, 0x0123456789abcdefULL };
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 5; j++) {
            v4q a = { pv[i], pv[(i + 1) % 5], pv[(i + 2) % 5], pv[(i + 3) % 5] };
            v4q bb = { pv[j], pv[(j + 3) % 5], pv[(j + 1) % 5], pv[(j + 4) % 5] };
            g_u64 f;
            __asm__ volatile("vptest %2, %1\n\tpushfq\n\tpopq %0" : "=r"(f) : "x"(a), "x"(bb) : "cc"); put("vptest ", f & 0x8d1);
            ymem = bb;
            __asm__ volatile("vptest %2, %1\n\tpushfq\n\tpopq %0" : "=r"(f) : "x"(a), "m"(ymem) : "cc"); put(" m ", f & 0x8d1);
            __asm__ volatile("vptest %x2, %x1\n\tpushfq\n\tpopq %0" : "=r"(f) : "x"(a), "x"(bb) : "cc"); put(" x ", f & 0x8d1);
            unsigned z, c;
            __asm__ volatile("vptest %3, %2\n\tsetz %b0\n\tsetc %b1" : "=r"(z), "=r"(c) : "x"(a), "x"(bb) : "cc"); put(" zc ", (z & 1) | (c & 1) << 1);
            g_u64 m;
            __asm__ volatile("vmovmskps %1, %k0" : "=r"(m) : "x"(a)); put(" mskps ", m);
            __asm__ volatile("vmovmskpd %1, %k0" : "=r"(m) : "x"(bb)); put(" mskpd ", m);
            __asm__ volatile("vmovmskps %x1, %k0" : "=r"(m) : "x"(bb)); put(" mskps x ", m);
            g_puts("\n");
        }
    }
    return 0;
}

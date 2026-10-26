/* vinsertf128/i128, vextractf128/i128 and vperm2f128/i128: both lanes, register and memory operands, the
 * destination aliasing either source, every selector of vperm2 with and without its zero bits.  The JIT has
 * translated them since 2026-10-26; they were interpreted before. */
#include "gsys.h"
typedef g_u64 v4q __attribute__((vector_size(32)));
typedef g_u64 v2q __attribute__((vector_size(16)));
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
__attribute__((target("avx2"))) static void puty(const char *tag, v4q v) { g_puts(tag); for (int i = 0; i < 4; i++) g_puthex64(v[i]); }
static void putx(const char *tag, v2q v) { g_puts(tag); g_puthex64(v[0]); g_puthex64(v[1]); }
static volatile v4q ymem;
static volatile v2q xmem, xout;
__attribute__((target("avx2"))) int main(void)
{
    v4q A = { 0x1111111111111111ULL, 0x2222222222222222ULL, 0x3333333333333333ULL, 0x4444444444444444ULL };
    v4q B = { 0xaaaaaaaaaaaaaaaaULL, 0xbbbbbbbbbbbbbbbbULL, 0xccccccccccccccccULL, 0xddddddddddddddddULL };
    v2q X = { 0x5555555555555555ULL, 0x6666666666666666ULL };
    v4q r; v2q x;
    xmem = X;
    __asm__ volatile("vinsertf128 $0, %2, %1, %0" : "=x"(r) : "x"(A), "x"(X)); puty("ins0 ", r);
    __asm__ volatile("vinsertf128 $1, %2, %1, %0" : "=x"(r) : "x"(A), "x"(X)); puty(" ins1 ", r);
    __asm__ volatile("vinserti128 $1, %2, %1, %0" : "=x"(r) : "x"(B), "m"(xmem)); puty(" insi1m ", r);
    __asm__ volatile("vinsertf128 $0, %2, %1, %0" : "=x"(r) : "x"(B), "m"(xmem)); puty(" ins0m ", r);
    r = A; __asm__ volatile("vinsertf128 $1, %1, %0, %0" : "+x"(r) : "x"(X)); puty(" ins1 alias ", r);
    r = A; __asm__ volatile("vinsertf128 $0, %1, %0, %0" : "+x"(r) : "x"(X)); puty(" ins0 alias ", r);
    __asm__ volatile("vextractf128 $1, %1, %0" : "=x"(x) : "x"(A)); putx("\next1 ", x);
    __asm__ volatile("vextractf128 $0, %1, %0" : "=x"(x) : "x"(B)); putx(" ext0 ", x);
    __asm__ volatile("vextracti128 $1, %1, %0" : "=m"(xout) : "x"(B)); putx(" exti1m ", xout);
    __asm__ volatile("vextractf128 $0, %1, %0" : "=m"(xout) : "x"(A)); putx(" ext0m ", xout);
    {
        v4q y = A; v2q lo;
        __asm__ volatile("vextractf128 $1, %1, %%xmm15\n\tvmovdqu %%ymm15, %0" : "=m"(ymem) : "x"(y) : "xmm15");
        puty(" ext1 zeroes upper ", ymem);
        (void)lo;
    }
    static const unsigned imms[] = { 0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13, 0x20, 0x21, 0x31, 0x30,
                                     0x08, 0x80, 0x88, 0x28, 0x83, 0x0b, 0xb3, 0x22 };
    ymem = B;
    for (unsigned i = 0; i < sizeof imms / sizeof imms[0]; i++) {
        put("\nimm ", imms[i]);
        switch (imms[i]) {
#define P(I) case I: \
            __asm__ volatile("vperm2f128 $" #I ", %2, %1, %0" : "=x"(r) : "x"(A), "x"(B)); puty(" rr ", r); \
            __asm__ volatile("vperm2i128 $" #I ", %2, %1, %0" : "=x"(r) : "x"(A), "m"(ymem)); puty(" rm ", r); \
            r = A; __asm__ volatile("vperm2f128 $" #I ", %1, %0, %0" : "+x"(r) : "x"(B)); puty(" d=s1 ", r); \
            r = B; __asm__ volatile("vperm2f128 $" #I ", %0, %1, %0" : "+x"(r) : "x"(A)); puty(" d=s2 ", r); \
            break;
        P(0x00) P(0x01) P(0x02) P(0x03) P(0x10) P(0x11) P(0x12) P(0x13) P(0x20) P(0x21) P(0x31) P(0x30)
        P(0x08) P(0x80) P(0x88) P(0x28) P(0x83) P(0x0b) P(0xb3) P(0x22)
        }
    }
    g_puts("\n");
    return 0;
}

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
typedef float f4 __attribute__((vector_size(16)));
typedef double d2 __attribute__((vector_size(16)));
static uint64_t h;
static void mix(const void *p, size_t n) { const unsigned char *c = p; for (size_t i = 0; i < n; i++) h = (h ^ c[i]) * 0x100000001b3ull; }
int main(void)
{
    float fs[] = { 0.0f, -0.0f, 1.5f, -2.25f, 3.4028235e38f, -3.4028235e38f, 1e-40f, -1e-45f, INFINITY, -INFINITY, NAN, -NAN };
    uint32_t snan = 0x7f800001u, qpay = 0x7fc12345u; memcpy(&fs[10], &snan, 4); memcpy(&fs[11], &qpay, 4);
    double ds[] = { 0.0, -0.0, 1.5, -2.25, 1e300, -1e300, 3.4028236e38, 1e-310, 1.0000001, 0.1, INFINITY, NAN };
    uint64_t dsn = 0x7ff0000000000001ull; memcpy(&ds[11], &dsn, 8);
    int n = 0;
    for (int i = 0; i < 12; i++) for (int j = 0; j < 12; j++) {
        f4 a = { fs[i], fs[j], 7.0f, 8.0f };
        d2 r; f4 q;
        __asm__ volatile("cvtps2pd %1, %0" : "=x"(r) : "x"(a));
        mix(&r, 16);
        float m[2] = { fs[j], fs[i] };
        __asm__ volatile("cvtps2pd %1, %0" : "=x"(r) : "m"(m));
        mix(&r, 16);
        d2 c = { ds[i], ds[j] };
        q = (f4){ 9.0f, 9.0f, 9.0f, 9.0f };
        __asm__ volatile("cvtpd2ps %1, %0" : "+x"(q) : "x"(c));
        mix(&q, 16);
        __asm__ volatile("cvtpd2ps %1, %0" : "+x"(q) : "m"(c));
        mix(&q, 16);
        n++;
    }
    unsigned modes[] = { 0x1f80, 0x3f80, 0x5f80, 0x7f80, 0x9f80 };
    for (int k = 0; k < 5; k++) {
        unsigned old; __asm__ volatile("stmxcsr %0" : "=m"(old));
        __asm__ volatile("ldmxcsr %0" :: "m"(modes[k]));
        for (int i = 0; i < 12; i++) { d2 c = { ds[i], -ds[i] }; f4 q; __asm__ volatile("cvtpd2ps %1, %0" : "=x"(q) : "x"(c)); mix(&q, 16); }
        __asm__ volatile("ldmxcsr %0" :: "m"(old));
    }
    printf("%d %016llx\n", n, (unsigned long long)h);
    return 0;
}

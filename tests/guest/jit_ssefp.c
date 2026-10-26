/* haddps, haddpd, hsubps, hsubpd, addsubps and addsubpd on ordinary values, infinities, signed zeros and NaNs
 * in either operand (x86's rule: the first operand's NaN, else the second's, quieted; an invalid operation gives
 * the negative default NaN), register and memory sources; extractps of every lane to a register and to memory;
 * rcpps, rcpss, rsqrtps and rsqrtss within x86's documented error (1.5 * 2^-12 relative), since x86 gives an
 * estimate and ocerz the exact value; dpps and dppd with assorted masks and signed zeros (no NaNs: dpps and
 * dppd use the host's NaN rules in the interpreter too, which give x86's only where FPCR.AH is on).  The JIT has
 * translated them since 2026-10-26. */
#include "gsys.h"
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
typedef float v4f __attribute__((vector_size(16)));
typedef double v2d __attribute__((vector_size(16)));
typedef unsigned v4u __attribute__((vector_size(16)));
typedef g_u64 v2u __attribute__((vector_size(16)));
static const unsigned fb[] = { 0x3fc00000u, 0x40200000u, 0xc0600000u, 0x3e800000u, 0x7f800000u, 0xff800000u,
                               0x80000000u, 0x00000000u, 0x7fc00001u, 0xffa00002u, 0x7f800001u, 0x4b000001u };
static const g_u64 db[] = { 0x3ff8000000000000ULL, 0xc004000000000000ULL, 0x7ff0000000000000ULL,
                            0xfff0000000000000ULL, 0x8000000000000000ULL, 0x7ff8000000000003ULL,
                            0xfff4000000000005ULL, 0x4330000000000001ULL };
static volatile v4f fmem;
static volatile v2d dmem;
static void putv4(const char *tag, v4f v) { v4u u = (v4u)v; g_puts(tag); g_puthex64(((g_u64)u[1] << 32) | u[0]); g_puthex64(((g_u64)u[3] << 32) | u[2]); }
static void putv2(const char *tag, v2d v) { v2u u = (v2u)v; g_puts(tag); g_puthex64(u[0]); g_puthex64(u[1]); }
static int close_rel(float got, float want)
{
    if (want != want) return got != got;
    if (want == 0 || want > 3.0e38f || want < -3.0e38f) return got == want;
    float e = (got - want) / want;
    if (e < 0) e = -e;
    return e <= 1.5f / 4096.0f;
}
int main(void)
{
    unsigned nf = sizeof fb / sizeof fb[0], nd = sizeof db / sizeof db[0];
    for (unsigned i = 0; i < nf; i++) {
        v4u au = { fb[i], fb[(i + 1) % nf], fb[(i + 5) % nf], fb[(i + 7) % nf] };
        v4u bu = { fb[(i + 3) % nf], fb[(i + 2) % nf], fb[(i + 9) % nf], fb[(i + 4) % nf] };
        v4f a = (v4f)au, b = (v4f)bu, r;
        r = a; __asm__ volatile("haddps %1, %0" : "+x"(r) : "x"(b)); putv4("haddps ", r);
        r = a; __asm__ volatile("hsubps %1, %0" : "+x"(r) : "x"(b)); putv4(" hsubps ", r);
        r = a; __asm__ volatile("addsubps %1, %0" : "+x"(r) : "x"(b)); putv4(" addsubps ", r);
        fmem = b;
        r = a; __asm__ volatile("haddps %1, %0" : "+x"(r) : "m"(fmem)); putv4(" haddps m ", r);
        r = a; __asm__ volatile("addsubps %1, %0" : "+x"(r) : "m"(fmem)); putv4(" addsubps m ", r);
        g_u64 e0, e3; unsigned em;
        __asm__ volatile("extractps $0, %1, %k0" : "=r"(e0) : "x"(a));
        __asm__ volatile("extractps $3, %1, %k0" : "=r"(e3) : "x"(a));
        __asm__ volatile("extractps $2, %1, %0" : "=m"(em) : "x"(b));
        put(" ext0 ", e0); put(" ext3 ", e3); put(" ext2m ", em);
        __asm__ volatile("vhaddps %2, %1, %0" : "=x"(r) : "x"(b), "x"(a)); putv4(" vhaddps ", r);
        __asm__ volatile("vaddsubps %2, %1, %0" : "=x"(r) : "x"(a), "m"(fmem)); putv4(" vaddsubps m ", r);
        __asm__ volatile("vextractps $1, %1, %k0" : "=r"(e0) : "x"(b)); put(" vext1 ", e0);
    }
    for (unsigned i = 0; i < nd; i++) {
        v2u au = { db[i], db[(i + 1) % nd] }, bu = { db[(i + 3) % nd], db[(i + 6) % nd] };
        v2d a = (v2d)au, b = (v2d)bu, r;
        r = a; __asm__ volatile("haddpd %1, %0" : "+x"(r) : "x"(b)); putv2("haddpd ", r);
        r = a; __asm__ volatile("hsubpd %1, %0" : "+x"(r) : "x"(b)); putv2(" hsubpd ", r);
        r = a; __asm__ volatile("addsubpd %1, %0" : "+x"(r) : "x"(b)); putv2(" addsubpd ", r);
        dmem = b;
        r = a; __asm__ volatile("addsubpd %1, %0" : "+x"(r) : "m"(dmem)); putv2(" addsubpd m ", r);
        __asm__ volatile("vhsubpd %2, %1, %0" : "=x"(r) : "x"(b), "x"(a)); putv2(" vhsubpd ", r);
    }
    {
        static const float dv[] = { 1.5f, -2.25f, 3.0f, 0.125f, -0.0f, 1e30f, -7.5f, 4.0f, 1e-30f, 2.5f, 0.0f, -1.0f };
        static const double dd[] = { 1.5, -2.25, 1e300, -0.0, 3.75, 0.5 };
        for (unsigned i = 0; i < 8; i++) {
            v4f a = { dv[i], dv[i + 1], dv[i + 2], dv[i + 3] }, b = { dv[i + 4], dv[i + 3], dv[i + 2], dv[i + 1] }, r;
            r = a; __asm__ volatile("dpps $0xff, %1, %0" : "+x"(r) : "x"(b)); putv4("dpps ff ", r);
            r = a; __asm__ volatile("dpps $0x71, %1, %0" : "+x"(r) : "x"(b)); putv4(" dpps 71 ", r);
            r = a; __asm__ volatile("dpps $0xa6, %1, %0" : "+x"(r) : "x"(b)); putv4(" dpps a6 ", r);
            fmem = b;
            r = a; __asm__ volatile("dpps $0x3c, %1, %0" : "+x"(r) : "m"(fmem)); putv4(" dpps 3c m ", r);
            __asm__ volatile("vdpps $0xd3, %2, %1, %0" : "=x"(r) : "x"(a), "x"(b)); putv4(" vdpps d3 ", r);
        }
        for (unsigned i = 0; i < 4; i++) {
            v2d a = { dd[i], dd[i + 1] }, b = { dd[i + 2], dd[(i + 5) % 6] }, r;
            r = a; __asm__ volatile("dppd $0x31, %1, %0" : "+x"(r) : "x"(b)); putv2("dppd 31 ", r);
            r = a; __asm__ volatile("dppd $0x12, %1, %0" : "+x"(r) : "x"(b)); putv2(" dppd 12 ", r);
            __asm__ volatile("vdppd $0x33, %2, %1, %0" : "=x"(r) : "x"(a), "x"(b)); putv2(" vdppd 33 ", r);
        }
    }
    static const float xs[] = { 3.0f, 0.1f, 7.0f, 1e-3f, 1234.5f, 0.5f, 2.0f, 9.75f };
    int ok = 1;
    for (unsigned i = 0; i < 8; i += 4) {
        v4f x = { xs[i], xs[i + 1], xs[i + 2], xs[i + 3] }, r, q, s = { 5.0f, 5.0f, 5.0f, 5.0f }, t = s;
        __asm__ volatile("rcpps %1, %0" : "=x"(r) : "x"(x));
        __asm__ volatile("rsqrtps %1, %0" : "=x"(q) : "x"(x));
        __asm__ volatile("rcpss %1, %0" : "+x"(s) : "x"(x));
        __asm__ volatile("rsqrtss %1, %0" : "+x"(t) : "x"(x));
        v4f vr, vt = { 6.0f, 6.0f, 6.0f, 6.0f };
        __asm__ volatile("vrcpps %1, %0" : "=x"(vr) : "x"(x));
        __asm__ volatile("vrsqrtss %1, %2, %0" : "=x"(vt) : "x"(x), "x"(vt));
        ok &= close_rel(vr[1], 1.0f / xs[i + 1]) && close_rel(vt[0], 1.0f / __builtin_sqrtf(xs[i])) && vt[3] == 6.0f;
        for (int k = 0; k < 4; k++) {
            float rs = __builtin_sqrtf(xs[i + k]);
            ok &= close_rel(r[k], 1.0f / xs[i + k]) && close_rel(q[k], 1.0f / rs);
        }
        ok &= close_rel(s[0], 1.0f / xs[i]) && s[1] == 5.0f && s[3] == 5.0f;
        ok &= close_rel(t[0], 1.0f / __builtin_sqrtf(xs[i])) && t[2] == 5.0f;
    }
    put("rcp rsqrt within x86 error ", ok);
    return 0;
}

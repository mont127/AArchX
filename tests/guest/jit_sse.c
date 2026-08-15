/* SSE/SSE2 JIT coverage: many small blocks mixing scalar/packed ops, memory
 * operands, conversions, compares, blends and moves; checksummed. */
#include "gsys.h"
static g_u64 acc;
static void mixu(g_u64 v) { acc = (acc ^ v) * 0x9E3779B97F4A7C15ULL; acc ^= acc >> 29; }
static void mixd(double d) { union { double d; g_u64 u; } u; u.d = d; mixu(u.u); }
static void mixf(float f) { union { float f; g_u32 u; } u; u.f = f; mixu(u.u); }
typedef float v4f __attribute__((vector_size(16)));
typedef double v2d __attribute__((vector_size(16)));
typedef int v4i __attribute__((vector_size(16)));
typedef long long v2i __attribute__((vector_size(16)));
static void mixv4(v4f v) { for (int i = 0; i < 4; i++) mixf(v[i]); }
static void mixv2(v2d v) { for (int i = 0; i < 2; i++) mixd(v[i]); }
__attribute__((noinline)) static double sc(double a, double b, int k) {
    switch (k & 7) { case 0: return a + b; case 1: return a - b; case 2: return a * b; case 3: return b != 0 ? a / b : a;
    case 4: return __builtin_sqrt(a < 0 ? -a : a); case 5: return a > b ? a : b; case 6: return a < b ? a : b; default: return (double)(float)a; }
}
__attribute__((noinline)) static float scf(float a, float b, int k) {
    switch (k & 7) { case 0: return a + b; case 1: return a - b; case 2: return a * b; case 3: return b != 0 ? a / b : a;
    case 4: return __builtin_sqrtf(a < 0 ? -a : a); case 5: return a > b ? a : b; case 6: return a < b ? a : b; default: return (float)(int)a; }
}
__attribute__((noinline)) static v4f pk(v4f a, v4f b, int k) {
    switch (k & 3) { case 0: return a + b; case 1: return a - b; case 2: return a * b; default: return a / b; }
}
__attribute__((noinline)) static v2d pkd(v2d a, v2d b, int k) {
    switch (k & 3) { case 0: return a + b; case 1: return a - b; case 2: return a * b; default: return a / b; }
}
int main(void) {
    volatile double dv[8] = { 0.0, 1.0, -2.5, 3.25e10, -1e-300, 1e300, 0.1, -0.0 };
    volatile float fv[8] = { 0.0f, 1.0f, -2.5f, 3.25e10f, -1e-30f, 1e30f, 0.1f, -0.0f };
    double inf = 1e308 * 10.0, nan = inf - inf;
    for (int i = 0; i < 8; i++) for (int j = 0; j < 8; j++) for (int k = 0; k < 8; k++) {
        mixd(sc(dv[i], dv[j], k)); mixf(scf(fv[i], fv[j], k));
        mixd(sc(dv[i], k & 1 ? nan : inf, k)); mixd(sc(nan, dv[j], k));
        v4f a = { fv[i], fv[j], fv[(i+j)&7], fv[(i*j)&7] }, b = { fv[j], fv[i], 1.0f, -3.0f };
        mixv4(pk(a, b, k)); mixv4(pk(a, a, k));
        v2d c = { dv[i], dv[j] }, d = { dv[(i+3)&7], k & 1 ? nan : 2.0 };
        mixv2(pkd(c, d, k)); mixv2(pkd(d, c, k));
        /* compares & conversions */
        mixu(dv[i] < dv[j]); mixu(dv[i] <= dv[j]); mixu(dv[i] == dv[j]); mixu(dv[i] != nan); mixu(nan == nan);
        mixu(fv[i] > fv[j]); mixu(fv[i] >= fv[j]);
        mixu((g_u64)(long)dv[i]); mixu((g_u64)(int)fv[j]); mixd((double)(long)(i * 1000003 - j)); mixf((float)(i - j));
        mixd((double)fv[i]); mixf((float)dv[j]);
        /* blend-style select (clang emits blendvpd/cmpsd) */
        double s = dv[i] > 1e6 ? 1.0 : dv[i]; mixd(s);
        double t = dv[j] < -1e6 ? 0.5 : dv[j]; mixd(t);
        /* integer vectors */
        v4i ia = { i, j, i - j, i * j }, ib = { 3, -1, 7, j }; v4i ic = ia + ib, id = ia - ib, ie = ia & ib, ig = ia | ib, ih = ia ^ ib;
        for (int q = 0; q < 4; q++) { mixu((g_u32)ic[q]); mixu((g_u32)id[q]); mixu((g_u32)ie[q]); mixu((g_u32)ig[q]); mixu((g_u32)ih[q]); }
        v2i la = { (long long)i << 33, -(long long)j }, lb = { 1, 2 }; v2i lc = la + lb, ld = la - lb;
        mixu((g_u64)lc[0]); mixu((g_u64)lc[1]); mixu((g_u64)ld[0]); mixu((g_u64)ld[1]);
        v4i eq = (v4i)(ia == ib), gt = (v4i)(ia > ib); mixu((g_u32)eq[0] ^ (g_u32)eq[3]); mixu((g_u32)gt[1] ^ (g_u32)gt[2]);
        v4f cvt = __builtin_convertvector(ia, v4f); mixv4(cvt);
    }
    /* memcpy/memset lowering (movups/movaps/movdqu paths) */
    static unsigned char buf[4096], buf2[4096];
    for (int i = 0; i < 4096; i++) buf[i] = (unsigned char)(i * 7 + 3);
    for (int len = 1; len < 200; len += 7) { __builtin_memcpy(buf2 + (len & 15), buf + (len & 31), len); mixu(buf2[(len & 15) + len - 1]); }
    __builtin_memset(buf2, 0x5a, 777); mixu(buf2[776]); mixu(buf2[777]);
    g_putu64(acc); return 0;
}

/* Exact SSE NaN semantics, printed per case so a golden diff pinpoints the
 * rule: SNaN/QNaN priority, operand order, default-NaN sign, payload
 * preservation, for scalar and packed float/double arithmetic, min/max,
 * sqrt and conversions.  Golden comes from real x86 (Rosetta). */
#include "gsys.h"
typedef struct { g_u64 lo, hi; } __attribute__((aligned(16))) x128;
#define X(name, ...) __asm__ volatile(name : __VA_ARGS__)
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
static const g_u32 fv[] = {
    0x3fc00000u, /* 1.5 */ 0x7f800000u, /* +inf */ 0xff800000u, /* -inf */ 0x00000000u, /* +0 */
    0x7f800001u, /* SNaN pl1 */ 0xff800002u, /* -SNaN pl2 */ 0x7fc00001u, /* QNaN pl1 */
    0xffc00003u, /* -QNaN pl3 */ 0x7fc00000u, /* +default QNaN */ 0xffc00000u, /* x86 default */ 0x80000000u /* -0 */ };
static const g_u64 dv[] = {
    0x3ff8000000000000ULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0ULL,
    0x7ff0000000000001ULL, 0xfff0000000000002ULL, 0x7ff8000000000001ULL,
    0xfff8000000000003ULL, 0x7ff8000000000000ULL, 0xfff8000000000000ULL, 0x8000000000000000ULL };
#define NF (int)(sizeof fv / sizeof fv[0])
#define ND (int)(sizeof dv / sizeof dv[0])
int main(void){
    x128 a, b, r;
    /* scalar single: op a, b  (a = dst/first source, b = second source) */
    for (int i = 0; i < NF; i++) for (int j = 0; j < NF; j++) {
        a.lo = fv[i]; a.hi = 0; b.lo = fv[j]; b.hi = 0;
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\taddss %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("addss ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tsubss %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("subss ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tmulss %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("mulss ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tdivss %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("divss ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tminss %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("minss ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tmaxss %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("maxss ", r.lo);
        /* packed: lanes {a_i, b_j, a_i, b_j} op {b_j, a_i, 1.5, 1.5} */
        a.lo = (g_u64)fv[i] | ((g_u64)fv[j] << 32); a.hi = a.lo;
        b.lo = (g_u64)fv[j] | ((g_u64)fv[i] << 32); b.hi = 0x3fc000003fc00000ULL;
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\taddps %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("addps ", r.lo); put("addps.hi ", r.hi);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tmulps %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("mulps ", r.lo); put("mulps.hi ", r.hi);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tminps %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("minps ", r.lo); put("minps.hi ", r.hi);
    }
    /* scalar double + packed double */
    for (int i = 0; i < ND; i++) for (int j = 0; j < ND; j++) {
        a.lo = dv[i]; a.hi = dv[j]; b.lo = dv[j]; b.hi = dv[i];
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\taddsd %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("addsd ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tmulsd %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("mulsd ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tdivsd %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("divsd ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tsubpd %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("subpd ", r.lo); put("subpd.hi ", r.hi);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tmaxpd %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("maxpd ", r.lo); put("maxpd.hi ", r.hi);
    }
    /* unary: sqrt, conversions */
    for (int i = 0; i < NF; i++) {
        a.lo = fv[i]; a.hi = 0;
        X("movaps %1, %%xmm0\n\tsqrtss %%xmm0, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a) : "xmm0"); put("sqrtss ", r.lo);
        X("movaps %1, %%xmm0\n\tcvtss2sd %%xmm0, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a) : "xmm0"); put("cvtss2sd ", r.lo);
        a.lo = (g_u64)fv[i] | ((g_u64)fv[i] << 32); a.hi = a.lo;
        X("movaps %1, %%xmm0\n\tsqrtps %%xmm0, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a) : "xmm0"); put("sqrtps ", r.lo);
        X("movaps %1, %%xmm0\n\tcvtps2pd %%xmm0, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a) : "xmm0"); put("cvtps2pd ", r.lo); put("cvtps2pd.hi ", r.hi);
    }
    for (int i = 0; i < ND; i++) {
        a.lo = dv[i]; a.hi = dv[i];
        X("movaps %1, %%xmm0\n\tsqrtsd %%xmm0, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a) : "xmm0"); put("sqrtsd ", r.lo);
        X("movaps %1, %%xmm0\n\tcvtsd2ss %%xmm0, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a) : "xmm0"); put("cvtsd2ss ", r.lo);
        X("movaps %1, %%xmm0\n\tcvtpd2ps %%xmm0, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a) : "xmm0"); put("cvtpd2ps ", r.lo);
        g_u64 g; X("movaps %1, %%xmm0\n\tcvttsd2si %%xmm0, %0", "=r"(g) : "m"(a) : "xmm0"); put("cvttsd2si ", g);
    }
    /* cmpss/cmpsd predicates 0..7 with NaN / ordered operands (mask results) */
    for (int i = 0; i < NF; i += 2) for (int j = 1; j < NF; j += 3) {
        a.lo = fv[i]; a.hi = 0x11111111u; b.lo = fv[j]; b.hi = 0;
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tcmpss $0, %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("cmpss0 ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tcmpss $1, %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("cmpss1 ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tcmpss $2, %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("cmpss2 ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tcmpss $3, %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("cmpss3 ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tcmpss $4, %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("cmpss4 ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tcmpss $5, %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("cmpss5 ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tcmpss $6, %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("cmpss6 ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tcmpss $7, %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("cmpss7 ", r.lo);
    }
    for (int i = 0; i < ND; i += 2) for (int j = 1; j < ND; j += 3) {
        a.lo = dv[i]; a.hi = 0x2222222222222222ULL; b.lo = dv[j]; b.hi = 0;
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tcmpsd $0, %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("cmpsd0 ", r.lo); put("cmpsd0.hi ", r.hi);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tcmpsd $1, %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("cmpsd1 ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tcmpsd $2, %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("cmpsd2 ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tcmpsd $3, %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("cmpsd3 ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tcmpsd $4, %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("cmpsd4 ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tcmpsd $5, %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("cmpsd5 ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tcmpsd $6, %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("cmpsd6 ", r.lo);
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tcmpsd $7, %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("cmpsd7 ", r.lo);
        /* blendvpd with the compare mask in xmm0 */
        X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tcmpsd $1, %%xmm1, %%xmm0\n\tmovaps %1, %%xmm2\n\tblendvpd %%xmm0, %%xmm1, %%xmm2\n\tmovaps %%xmm2, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1","xmm2"); put("blendvpd ", r.lo); put("blendvpd.hi ", r.hi);
    }
    /* invalid ops without NaN inputs: inf-inf, 0*inf, 0/0, sqrt(-1) -> default NaN sign */
    a.lo = 0x7f800000u; b.lo = 0x7f800000u; a.hi = b.hi = 0;
    X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tsubss %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("inf-inf ", r.lo);
    a.lo = 0; X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tmulss %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("0*inf ", r.lo);
    b.lo = 0; X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tdivss %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("0/0 ", r.lo);
    a.lo = 0xbf800000u; X("movaps %1, %%xmm0\n\tsqrtss %%xmm0, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a) : "xmm0"); put("sqrt(-1) ", r.lo);
    a.lo = 0xfff0000000000000ULL; b.lo = 0x7ff0000000000000ULL;
    X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\taddsd %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); put("-inf+inf(d) ", r.lo);
    return 0;
}

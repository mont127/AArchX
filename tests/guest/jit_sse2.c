/* Direct-asm SSE coverage: each inlined instruction form with checks of the
 * lanes it must preserve/zero, register and memory variants. */
#include "gsys.h"
static g_u64 acc; static void mix(g_u64 v){ acc=(acc^v)*0x9E3779B97F4A7C15ULL; acc^=acc>>29; }
typedef struct { g_u64 lo, hi; } __attribute__((aligned(16))) x128;
static void mixx(x128 v){ mix(v.lo); mix(v.hi); }
#define X(name, ...) __asm__ volatile(name : __VA_ARGS__)
int main(void){
    x128 a = { 0x3ff0000000000000ULL, 0x4000000000000000ULL };   /* {1.0, 2.0} */
    x128 b = { 0x4008000000000000ULL, 0xc010000000000000ULL };   /* {3.0, -4.0} */
    x128 f = { 0x3f80000040000000ULL, 0x4040000040800000ULL };   /* {2.0f,1.0f,4.0f,3.0f} lanes lo->hi */
    x128 r, m; g_u64 g;
    volatile g_u64 mem64 = 0x400c000000000000ULL; volatile g_u32 mem32 = 0x40a00000u;
    /* movsd xmm,xmm: low lane copied, high lane of dst preserved */
    X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tmovsd %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); mixx(r);
    /* movsd xmm, mem: high lane ZEROED */
    X("movaps %1, %%xmm0\n\tmovsd %2, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(mem64) : "xmm0"); mixx(r);
    /* movss variants */
    X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tmovss %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(f), "m"(b) : "xmm0","xmm1"); mixx(r);
    X("movaps %1, %%xmm0\n\tmovss %2, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(f), "m"(mem32) : "xmm0"); mixx(r);
    /* movd/movq: zero-extend into xmm; extract low */
    g = 0x1122334455667788ULL;
    X("movaps %1, %%xmm0\n\tmovq %2, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "r"(g) : "xmm0"); mixx(r);
    X("movaps %1, %%xmm0\n\tmovd %2, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "r"((g_u32)g) : "xmm0"); mixx(r);
    X("movaps %1, %%xmm0\n\tmovq %%xmm0, %0", "=r"(g) : "m"(b) : "xmm0"); mix(g);
    X("movaps %1, %%xmm0\n\tmovd %%xmm0, %0", "=r"(g) : "m"(b) : "xmm0"); mix(g & 0xffffffffULL);
    /* addsd/mulsd/divsd/sqrtsd/subsd: high lane preserved */
    X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\taddsd %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); mixx(r);
    X("movaps %1, %%xmm0\n\tmulsd %2, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(mem64) : "xmm0"); mixx(r);
    X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tdivsd %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); mixx(r);
    X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tsqrtsd %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); mixx(r);
    X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tsubss %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(f), "m"(b) : "xmm0","xmm1"); mixx(r);
    /* packed */
    X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\taddpd %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); mixx(r);
    X("movaps %1, %%xmm0\n\tmulps %2, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(f), "m"(f) : "xmm0"); mixx(r);
    X("movaps %1, %%xmm0\n\tsqrtpd %%xmm0, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a) : "xmm0"); mixx(r);
    /* unpck / movlhps / movhlps */
    X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tunpcklpd %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); mixx(r);
    X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tunpckhpd %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); mixx(r);
    X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tmovlhps %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); mixx(r);
    X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tmovhlps %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); mixx(r);
    X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tunpcklps %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(f), "m"(b) : "xmm0","xmm1"); mixx(r);
    X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tunpckhps %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(f), "m"(b) : "xmm0","xmm1"); mixx(r);
    /* logic + integer add/sub/cmp */
    X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tpxor %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); mixx(r);
    X("movaps %1, %%xmm0\n\tandnps %2, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0"); mixx(r);
    X("movaps %1, %%xmm0\n\tpaddq %2, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0"); mixx(r);
    X("movaps %1, %%xmm0\n\tpsubd %2, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0"); mixx(r);
    X("movaps %1, %%xmm0\n\tpcmpeqd %2, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(a) : "xmm0"); mixx(r);
    X("movaps %1, %%xmm0\n\tpcmpgtd %2, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0"); mixx(r);
    /* comis flags via setcc */
    { g_u64 fl; X("xor %%eax,%%eax\n\txor %%ebx,%%ebx\n\txor %%ecx,%%ecx\n\txor %%edx,%%edx\n\tmovaps %1, %%xmm0\n\tucomisd %2, %%xmm0\n\tsetae %%al\n\tsetb %%bl\n\tsete %%cl\n\tsetp %%dl\n\tshl $8,%%rbx\n\tor %%rbx,%%rax\n\tshl $16,%%rcx\n\tor %%rcx,%%rax\n\tshl $24,%%rdx\n\tor %%rdx,%%rax", "=a"(fl) : "m"(a), "m"(b) : "rbx","rcx","rdx","xmm0","cc"); mix(fl & 0xffffffffULL); }
    { g_u64 fl; volatile double big=1e308; double nan=(big*10)-(big*10); X("xor %%eax,%%eax\n\txor %%ebx,%%ebx\n\txor %%ecx,%%ecx\n\txor %%edx,%%edx\n\tmovsd %1, %%xmm0\n\tucomisd %2, %%xmm0\n\tsetae %%al\n\tsetb %%bl\n\tsete %%cl\n\tsetp %%dl\n\tshl $8,%%rbx\n\tor %%rbx,%%rax\n\tshl $16,%%rcx\n\tor %%rcx,%%rax\n\tshl $24,%%rdx\n\tor %%rdx,%%rax", "=a"(fl) : "m"(nan), "m"(mem64) : "rbx","rcx","rdx","xmm0","cc"); mix(fl & 0xffffffffULL); }
    /* cvt */
    X("movaps %1, %%xmm0\n\tcvtsi2sd %2, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "r"((long)-77) : "xmm0"); mixx(r);
    X("movaps %1, %%xmm0\n\tcvtsi2ss %2, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(f), "r"((int)12345) : "xmm0"); mixx(r);
    X("movaps %1, %%xmm0\n\tcvtsd2ss %2, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(f), "m"(mem64) : "xmm0"); mixx(r);
    X("movaps %1, %%xmm0\n\tcvtss2sd %2, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(mem32) : "xmm0"); mixx(r);
    X("movaps %1, %%xmm0\n\tcvtdq2ps %%xmm0, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a) : "xmm0"); mixx(r);
    /* cmpsd predicates 0..7 and blendvpd */
    for (int p = 0; p < 8; p++) {
        volatile int pp = p; (void)pp;
        switch (p) {
        #define CMP(P) X("movaps %1, %%xmm0\n\tmovaps %2, %%xmm1\n\tcmpsd $" #P ", %%xmm1, %%xmm0\n\tmovaps %%xmm0, %0", "=m"(r) : "m"(a), "m"(b) : "xmm0","xmm1"); mixx(r); break;
        case 0: CMP(0) case 1: CMP(1) case 2: CMP(2) case 3: CMP(3) case 4: CMP(4) case 5: CMP(5) case 6: CMP(6) default: CMP(7)
        }
    }
    m.lo = 0x8000000000000000ULL; m.hi = 0;   /* mask lane0 set */
    X("movaps %3, %%xmm0\n\tmovaps %1, %%xmm2\n\tmovaps %2, %%xmm3\n\tblendvpd %%xmm0, %%xmm3, %%xmm2\n\tmovaps %%xmm2, %0", "=m"(r) : "m"(a), "m"(b), "m"(m) : "xmm0","xmm2","xmm3"); mixx(r);
    g_putu64(acc); return 0;
}

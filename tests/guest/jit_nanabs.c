/* NaN payload exactness through FP batches with absorption: a divergent
 * (x86 default) NaN is produced in a register that only flows into another
 * register through propagating ops, then both are observed; also the cases
 * where the chain is broken (reload, min/max, mixed precision) and the
 * source must be checked itself.  Golden from Rosetta. */
#include "gsys.h"
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
static float fa[4] __attribute__((aligned(16))) = { 0.f, 1.f, 2.f, 3.f };
static float finf[4] __attribute__((aligned(16))) = { 1.f/0.f, 1.f, 1.f, 1.f };   /* +inf lane 0 */
static float fone[4] __attribute__((aligned(16))) = { 1.f, 1.f, 1.f, 1.f };
static double da[2] __attribute__((aligned(16))) = { 0.0, 1.0 };
static double dinf[2] __attribute__((aligned(16))) = { 1.0/0.0, 1.0 };
static float out[8] __attribute__((aligned(16)));
static double dout[4] __attribute__((aligned(16)));
static g_u64 h(const void *p, int n) { const unsigned char *c = p; g_u64 s = 0; for (int i = 0; i < n; i++) s = s * 131 + c[i]; return s; }
int main(void){
    /* (a) S = 0*inf -> NaN; D = D + S; store both */
    __asm__ volatile("movaps %0, %%xmm3\n\tmovaps %2, %%xmm7\n\tmulps %%xmm7, %%xmm7\n\taddps %%xmm7, %%xmm7\n\tmulps %1, %%xmm3\n\tmovaps %2, %%xmm2\n\taddps %%xmm3, %%xmm2\n\tmulps %%xmm7, %%xmm7\n\tmovaps %%xmm3, %3\n\tmovaps %%xmm2, %4"
                     : : "m"(fa[0]), "m"(finf[0]), "m"(fone[0]), "m"(out[0]), "m"(out[4]) : "xmm2", "xmm3", "xmm7", "memory");
    put("a S ", h(out, 16)); put("a D ", h(out + 4, 16));
    /* (b) same but D reloaded from memory before the batch ends -> S must be checked itself */
    __asm__ volatile("movaps %0, %%xmm3\n\tmovaps %2, %%xmm7\n\tmulps %%xmm7, %%xmm7\n\tmulps %1, %%xmm3\n\tmovaps %2, %%xmm2\n\taddps %%xmm3, %%xmm2\n\tmovaps %2, %%xmm2\n\tmulps %%xmm2, %%xmm2\n\taddps %%xmm7, %%xmm7\n\tmovaps %%xmm3, %3\n\tmovaps %%xmm2, %4"
                     : : "m"(fa[0]), "m"(finf[0]), "m"(fone[0]), "m"(out[0]), "m"(out[4]) : "xmm2", "xmm3", "xmm7", "memory");
    put("b S ", h(out, 16)); put("b D ", h(out + 4, 16));
    /* (c) consumer is maxps (does not propagate NaN on x86: max(NaN, x) = x) */
    __asm__ volatile("movaps %0, %%xmm3\n\tmovaps %2, %%xmm7\n\tmulps %%xmm7, %%xmm7\n\tmulps %1, %%xmm3\n\tmovaps %2, %%xmm2\n\tmaxps %%xmm3, %%xmm2\n\taddps %%xmm7, %%xmm7\n\tmulps %%xmm7, %%xmm7\n\tmovaps %%xmm3, %3\n\tmovaps %%xmm2, %4"
                     : : "m"(fa[0]), "m"(finf[0]), "m"(fone[0]), "m"(out[0]), "m"(out[4]) : "xmm2", "xmm3", "xmm7", "memory");
    put("c S ", h(out, 16)); put("c D ", h(out + 4, 16));
    /* (d) S packed-single NaN consumed by a scalar op: only lane 0 flows */
    __asm__ volatile("movaps %0, %%xmm3\n\tmulps %1, %%xmm3\n\tshufps $0x1b, %%xmm3, %%xmm3\n\tmovaps %2, %%xmm7\n\tmulps %%xmm7, %%xmm7\n\tmovaps %2, %%xmm2\n\taddss %%xmm3, %%xmm2\n\taddps %%xmm7, %%xmm7\n\tmulps %%xmm7, %%xmm7\n\tmovaps %%xmm3, %3\n\tmovaps %%xmm2, %4"
                     : : "m"(fa[0]), "m"(finf[0]), "m"(fone[0]), "m"(out[0]), "m"(out[4]) : "xmm2", "xmm3", "xmm7", "memory");
    put("d S ", h(out, 16)); put("d D ", h(out + 4, 16));
    /* (e) double: S = 0*inf; D = D * S; D then reused; both stored */
    __asm__ volatile("movapd %0, %%xmm5\n\tmulpd %1, %%xmm5\n\tmovapd %0, %%xmm4\n\tmulpd %%xmm5, %%xmm4\n\taddpd %%xmm4, %%xmm4\n\tmovapd %%xmm5, %2\n\tmovapd %%xmm4, %3"
                     : : "m"(da[0]), "m"(dinf[0]), "m"(dout[0]), "m"(dout[2]) : "xmm4", "xmm5", "memory");
    put("e S ", h(dout, 16)); put("e D ", h(dout + 2, 16));
    /* (f) chain: S -> T -> D, only D live at the end but S,T stored after */
    __asm__ volatile("movaps %0, %%xmm3\n\tmulps %1, %%xmm3\n\tmovaps %2, %%xmm6\n\tsubps %%xmm3, %%xmm6\n\tmovaps %2, %%xmm2\n\tdivps %%xmm6, %%xmm2\n\tmovaps %%xmm3, %3\n\tmovaps %%xmm6, %4\n\tmovaps %%xmm2, %5"
                     : : "m"(fa[0]), "m"(finf[0]), "m"(fone[0]), "m"(out[0]), "m"(out[4]), "m"(dout[0]) : "xmm2", "xmm3", "xmm6", "memory");
    put("f S ", h(out, 16)); put("f T ", h(out + 4, 16)); put("f D ", h(dout, 16));
    /* (g) S rewritten after absorption with a fresh NaN */
    __asm__ volatile("movaps %0, %%xmm3\n\tmovaps %2, %%xmm7\n\tmulps %%xmm7, %%xmm7\n\tmulps %1, %%xmm3\n\tmovaps %2, %%xmm2\n\taddps %%xmm3, %%xmm2\n\tmovaps %0, %%xmm3\n\tmulps %1, %%xmm3\n\tsubps %2, %%xmm3\n\taddps %%xmm7, %%xmm7\n\tmovaps %%xmm3, %3\n\tmovaps %%xmm2, %4"
                     : : "m"(fa[0]), "m"(finf[0]), "m"(fone[0]), "m"(out[0]), "m"(out[4]) : "xmm2", "xmm3", "xmm7", "memory");
    put("g S ", h(out, 16)); put("g D ", h(out + 4, 16));
    return 0;
}

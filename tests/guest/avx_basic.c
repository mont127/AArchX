/* AVX (VEX-encoded) instructions: the set Apple's x86_64 Metal.framework is
 * built with (it assumes AVX and never checks), executed here in both the
 * 128-bit and 256-bit forms and compared with scalar C references.  Covers
 * the three-operand (non-destructive) forms, the NDD shift-by-immediate
 * forms, is4 blends, merge moves, upper-half zeroing of VEX.128 writes,
 * two-lane 256-bit ops, the lane-crossing converts/broadcasts/inserts and
 * the AVX-only ops (vbroadcast, vpermilps, vtestps, vcvtph2ps, vzeroupper).
 * Self-checking: prints one "<name> OK" line per check.
 *
 * Specific shapes worth naming: a VEX.128 write must zero the upper half (load
 * ymm0 with a value, vandps the low lane, extract the high one); the scalar
 * merges take their untouched part from vvvv, not from the destination; and
 * vtestps must set ZF when no sign bit is in common and CF when src2's sign bits
 * all lie inside src1.
 */
#include "gsys.h"

typedef float f32;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned short u16;
typedef unsigned char u8;

static int fails;
static int mism_i; static u64 mism_got, mism_want;
static void ok(const char *name) { g_puts(name); g_puts(" OK\n"); }
static void bad(const char *name, u64 got, u64 want)
{
    fails++; g_puts(name); g_puts(" FAIL got="); g_puthex64(got); g_puts("     want="); g_puthex64(want);
}
static void chk(const char *name, int cond, u64 got, u64 want)
{
    if (cond) { ok(name); return; }
    if (mism_got || mism_want) { g_puts("  lane "); g_putu64((u64)mism_i); got = mism_got; want = mism_want; mism_got = mism_want = 0; }
    bad(name, got, want);
}
static u32 fb(f32 f) { u32 u; __builtin_memcpy(&u, &f, 4); return u; }
static int eqf(const f32 *a, const f32 *b, int n) { for (int i = 0; i < n; i++) if (fb(a[i]) != fb(b[i])) { mism_i = i; mism_got = fb(a[i]); mism_want = fb(b[i]); return 0; } return 1; }
static int equ(const u32 *a, const u32 *b, int n) { for (int i = 0; i < n; i++) if (a[i] != b[i]) { mism_i = i; mism_got = a[i]; mism_want = b[i]; return 0; } return 1; }
static u64 db(double d) { u64 u; __builtin_memcpy(&u, &d, 8); return u; }

static f32 A[8] __attribute__((aligned(32))) = { 1.5f, -2.25f, 3.0f, 4.75f, 5.5f, -6.0f, 7.25f, 8.125f };
static f32 B[8] __attribute__((aligned(32))) = { 0.5f, 2.0f, -3.5f, 4.0f, -1.0f, 6.5f, 0.25f, -8.0f };
static u32 UA[8] __attribute__((aligned(32))) = { 1, 0xffffffffu, 7, 0x80000000u, 100, 200, 300, 400 };
static u32 UB[8] __attribute__((aligned(32))) = { 3, 2, 7, 1, 50, 250, 300, 0x7fffffffu };
static double DA[4] __attribute__((aligned(32))) = { 1.25, -2.5, 3.75, 1e10 };
static double DB[4] __attribute__((aligned(32))) = { 0.5, -3.0, 3.5, 2.0 };

int main(void)
{
    f32 R[16] __attribute__((aligned(32)));
    u32 UR[16] __attribute__((aligned(32)));
    double DR[8] __attribute__((aligned(32)));
    f32 E[8]; u32 UE[8];
    u64 g;

    __asm__ volatile("vmovups (%0), %%xmm1\n\tvmovups (%1), %%xmm2\n\tvaddps %%xmm2, %%xmm1, %%xmm2\n\tvmovups %%xmm2, (%2)"
                     :: "r"(A), "r"(B), "r"(R) : "memory", "xmm1", "xmm2");
    for (int i = 0; i < 4; i++) E[i] = A[i] + B[i];
    chk("vaddps.x", eqf(R, E, 4), fb(R[0]), fb(E[0]));

    __asm__ volatile("vmovaps (%0), %%ymm1\n\tvmovaps (%1), %%ymm2\n\tvmulps %%ymm2, %%ymm1, %%ymm0\n\tvmovaps %%ymm0, (%2)"
                     :: "r"(A), "r"(B), "r"(R) : "memory", "xmm0", "xmm1", "xmm2");
    for (int i = 0; i < 8; i++) E[i] = A[i] * B[i];
    chk("vmulps.y", eqf(R, E, 8), fb(R[7]), fb(E[7]));

    __asm__ volatile("vmovaps (%0), %%ymm1\n\tvsubps (%1), %%ymm1, %%ymm0\n\tvmovups %%ymm0, (%2)"
                     :: "r"(A), "r"(B), "r"(R) : "memory", "xmm0", "xmm1");
    for (int i = 0; i < 8; i++) E[i] = A[i] - B[i];
    chk("vsubps.y.mem", eqf(R, E, 8), fb(R[5]), fb(E[5]));

    __asm__ volatile("vmovaps (%0), %%ymm0\n\tvmovaps (%1), %%xmm1\n\tvandps %%xmm1, %%xmm0, %%xmm0\n\tvextractf128 $1, %%ymm0, (%2)\n\tvmovaps %%xmm0, 16(%2)"
                     :: "r"(A), "r"(B), "r"(R) : "memory", "xmm0", "xmm1");
    for (int i = 0; i < 4; i++) { E[i] = 0; u32 v = fb(A[i]) & fb(B[i]); __builtin_memcpy(&E[4 + i], &v, 4); }
    chk("vex128.zero-upper", eqf(R, E, 4), fb(R[0]) | fb(R[1]), 0);
    chk("vandps.x", eqf(R + 4, E + 4, 4), fb(R[4]), fb(E[4]));

    __asm__ volatile("vmovaps (%0), %%ymm1\n\tvorps (%1), %%ymm1, %%ymm0\n\tvmovaps %%ymm0, (%2)" :: "r"(UA), "r"(UB), "r"(UR) : "memory", "xmm0", "xmm1");
    for (int i = 0; i < 8; i++) UE[i] = UA[i] | UB[i];
    chk("vorps.y", equ(UR, UE, 8), UR[7], UE[7]);
    __asm__ volatile("vmovdqu (%0), %%xmm1\n\tvpxor (%1), %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, (%2)" :: "r"(UA), "r"(UB), "r"(UR) : "memory", "xmm0", "xmm1");
    for (int i = 0; i < 4; i++) UE[i] = UA[i] ^ UB[i];
    chk("vpxor.x", equ(UR, UE, 4), UR[1], UE[1]);

    __asm__ volatile("vmovdqu (%0), %%xmm1\n\tvmovdqu (%1), %%xmm2\n\tvpminud %%xmm2, %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, (%2)\n\t"
                     "vpmaxsd %%xmm2, %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, 16(%2)"
                     :: "r"(UA), "r"(UB), "r"(UR) : "memory", "xmm0", "xmm1", "xmm2");
    for (int i = 0; i < 4; i++) { UE[i] = UA[i] < UB[i] ? UA[i] : UB[i]; UE[4 + i] = (int)UA[i] > (int)UB[i] ? UA[i] : UB[i]; }
    chk("vpminud.x", equ(UR, UE, 4), UR[3], UE[3]);
    chk("vpmaxsd.x", equ(UR + 4, UE + 4, 4), UR[7], UE[7]);
    __asm__ volatile("vmovdqu (%0), %%xmm1\n\tvpcmpeqd (%1), %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, (%2)\n\tvpaddd (%1), %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, 16(%2)"
                     :: "r"(UA), "r"(UB), "r"(UR) : "memory", "xmm0", "xmm1");
    for (int i = 0; i < 4; i++) { UE[i] = UA[i] == UB[i] ? 0xffffffffu : 0; UE[4 + i] = UA[i] + UB[i]; }
    chk("vpcmpeqd.x", equ(UR, UE, 4), UR[2], UE[2]);
    chk("vpaddd.x", equ(UR + 4, UE + 4, 4), UR[5], UE[5]);
    __asm__ volatile("vmovdqa (%0), %%ymm1\n\tvpsubd (%1), %%ymm1, %%ymm0\n\tvmovdqa %%ymm0, (%2)" :: "r"(UA), "r"(UB), "r"(UR) : "memory", "xmm0", "xmm1");
    for (int i = 0; i < 8; i++) UE[i] = UA[i] - UB[i];
    chk("vpsubd.y", equ(UR, UE, 8), UR[7], UE[7]);

    __asm__ volatile("vmovdqu (%0), %%xmm1\n\tvpsrld $3, %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, (%1)\n\tvpslld $5, %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, 16(%1)"
                     :: "r"(UA), "r"(UR) : "memory", "xmm0", "xmm1");
    for (int i = 0; i < 4; i++) { UE[i] = UA[i] >> 3; UE[4 + i] = UA[i] << 5; }
    chk("vpsrld.imm", equ(UR, UE, 4), UR[1], UE[1]);
    chk("vpslld.imm", equ(UR + 4, UE + 4, 4), UR[7], UE[7]);
    {
        u64 cnt = 7;
        __asm__ volatile("vmovdqu (%0), %%xmm1\n\tvmovq %1, %%xmm3\n\tvpsrld %%xmm3, %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, (%2)\n\tvpsrldq $4, %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, 16(%2)"
                         :: "r"(UA), "r"(cnt), "r"(UR) : "memory", "xmm0", "xmm1", "xmm3");
        for (int i = 0; i < 4; i++) { UE[i] = UA[i] >> 7; UE[4 + i] = i < 3 ? UA[i + 1] : 0; }
        chk("vpsrld.reg", equ(UR, UE, 4), UR[0], UE[0]);
        chk("vpsrldq.imm", equ(UR + 4, UE + 4, 4), UR[4], UE[4]);
    }

    __asm__ volatile("vbroadcastss 4(%0), %%ymm0\n\tvmovaps %%ymm0, (%1)\n\tvmovss 8(%0), %%xmm2\n\tvbroadcastss %%xmm2, %%xmm1\n\tvmovaps %%xmm1, 32(%1)"
                     :: "r"(A), "r"(R) : "memory", "xmm0", "xmm1", "xmm2");
    { int g1 = 1; for (int i = 0; i < 8; i++) if (fb(R[i]) != fb(A[1])) g1 = 0; chk("vbroadcastss.y.mem", g1, fb(R[7]), fb(A[1])); }
    {
        f32 T[4] __attribute__((aligned(16)));
        __asm__ volatile("vmovss 8(%0), %%xmm2\n\tvbroadcastss %%xmm2, %%xmm1\n\tvmovaps %%xmm1, (%1)" :: "r"(A), "r"(T) : "memory", "xmm1", "xmm2");
        int g2 = 1; for (int i = 0; i < 4; i++) if (fb(T[i]) != fb(A[2])) g2 = 0;
        chk("vbroadcastss.x.reg", g2, fb(T[3]), fb(A[2]));
    }
    __asm__ volatile("vbroadcastsd 8(%0), %%ymm0\n\tvmovapd %%ymm0, (%1)" :: "r"(DA), "r"(DR) : "memory", "xmm0");
    chk("vbroadcastsd.y", db(DR[0]) == db(DA[1]) && db(DR[3]) == db(DA[1]), db(DR[3]), db(DA[1]));

    __asm__ volatile("vmovapd (%0), %%ymm1\n\tvsubpd (%1), %%ymm1, %%ymm0\n\tvmovupd %%ymm0, (%2)" :: "r"(DA), "r"(DB), "r"(DR) : "memory", "xmm0", "xmm1");
    chk("vsubpd.y", db(DR[0]) == db(DA[0] - DB[0]) && db(DR[3]) == db(DA[3] - DB[3]), db(DR[3]), db(DA[3] - DB[3]));
    __asm__ volatile("vmovapd (%0), %%ymm1\n\tvmaxpd (%1), %%ymm1, %%ymm0\n\tvmovapd %%ymm0, (%2)\n\tvminpd (%1), %%ymm1, %%ymm0\n\tvmovapd %%ymm0, 32(%2)"
                     :: "r"(DA), "r"(DB), "r"(DR) : "memory", "xmm0", "xmm1");
    { double mx[4], mn[4]; for (int i = 0; i < 4; i++) { mx[i] = DA[i] > DB[i] ? DA[i] : DB[i]; mn[i] = DA[i] < DB[i] ? DA[i] : DB[i]; }
      chk("vmaxpd.y", db(DR[0]) == db(mx[0]) && db(DR[3]) == db(mx[3]), db(DR[3]), db(mx[3]));
      double *DR2 = DR + 4; chk("vminpd.y", db(DR2[0]) == db(mn[0]) && db(DR2[3]) == db(mn[3]), db(DR2[3]), db(mn[3])); }
    __asm__ volatile("vmovapd (%0), %%ymm1\n\tvcvtpd2ps %%ymm1, %%xmm0\n\tvmovups %%xmm0, (%1)\n\tvcvttpd2dq %%ymm1, %%xmm0\n\tvmovdqu %%xmm0, 16(%1)"
                     :: "r"(DA), "r"(R) : "memory", "xmm0", "xmm1");
    for (int i = 0; i < 4; i++) { E[i] = (f32)DA[i]; }
    chk("vcvtpd2ps.y", eqf(R, E, 4), fb(R[3]), fb(E[3]));
    { u32 *UR2 = (u32 *)(R + 4); u32 want[4] = { 1, (u32)-2, 3, 0x80000000u }; chk("vcvttpd2dq.y", equ(UR2, want, 4), UR2[3], want[3]); }
    __asm__ volatile("vmovups (%0), %%xmm1\n\tvcvtps2pd %%xmm1, %%ymm0\n\tvmovapd %%ymm0, (%1)" :: "r"(A), "r"(DR) : "memory", "xmm0", "xmm1");
    chk("vcvtps2pd.y", db(DR[0]) == db((double)A[0]) && db(DR[3]) == db((double)A[3]), db(DR[3]), db((double)A[3]));
    __asm__ volatile("vmovdqu (%0), %%xmm1\n\tvcvtdq2pd %%xmm1, %%ymm0\n\tvmovapd %%ymm0, (%1)" :: "r"(UB), "r"(DR) : "memory", "xmm0", "xmm1");
    chk("vcvtdq2pd.y", db(DR[0]) == db((double)(int)UB[0]) && db(DR[3]) == db((double)(int)UB[3]), db(DR[3]), db((double)(int)UB[3]));

    __asm__ volatile("vmovups (%0), %%xmm1\n\tvcvttps2dq %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, (%1)\n\tvroundps $9, %%xmm1, %%xmm0\n\tvmovups %%xmm0, 16(%1)\n\tvroundps $10, %%xmm1, %%xmm0\n\tvmovups %%xmm0, 32(%1)"
                     :: "r"(A), "r"(R) : "memory", "xmm0", "xmm1");
    { u32 want[4] = { 1, (u32)-2, 3, 4 }; chk("vcvttps2dq.x", equ((u32 *)R, want, 4), ((u32 *)R)[1], want[1]);
      f32 fl[4] = { 1.0f, -3.0f, 3.0f, 4.0f }, ce[4] = { 2.0f, -2.0f, 3.0f, 5.0f };
      chk("vroundps.floor", eqf(R + 4, fl, 4), fb(R[5]), fb(fl[1])); chk("vroundps.ceil", eqf(R + 8, ce, 4), fb(R[8]), fb(ce[0])); }
    __asm__ volatile("vmovdqu (%0), %%xmm1\n\tvcvtdq2ps %%xmm1, %%xmm0\n\tvmovups %%xmm0, (%1)" :: "r"(UB), "r"(R) : "memory", "xmm0", "xmm1");
    for (int i = 0; i < 4; i++) E[i] = (f32)(int)UB[i];
    chk("vcvtdq2ps.x", eqf(R, E, 4), fb(R[3]), fb(E[3]));

    __asm__ volatile("vmovups (%0), %%xmm1\n\tvcmpltps (%1), %%xmm1, %%xmm0\n\tvmovups %%xmm0, (%2)\n\tvcmpnltps (%1), %%xmm1, %%xmm0\n\tvmovups %%xmm0, 16(%2)"
                     :: "r"(A), "r"(B), "r"(R) : "memory", "xmm0", "xmm1");
    for (int i = 0; i < 4; i++) { UE[i] = A[i] < B[i] ? 0xffffffffu : 0; UE[4 + i] = ~UE[i]; }
    chk("vcmpltps.x", equ((u32 *)R, UE, 4), ((u32 *)R)[0], UE[0]);
    chk("vcmpnltps.x", equ((u32 *)R + 4, UE + 4, 4), ((u32 *)R)[4], UE[4]);
    __asm__ volatile("vmovups (%0), %%xmm1\n\tvmovups (%1), %%xmm2\n\tvmovdqu (%2), %%xmm3\n\tvblendvps %%xmm3, %%xmm2, %%xmm1, %%xmm0\n\tvmovups %%xmm0, (%3)\n\tvblendps $5, %%xmm2, %%xmm1, %%xmm0\n\tvmovups %%xmm0, 16(%3)"
                     :: "r"(A), "r"(B), "r"(UA), "r"(R) : "memory", "xmm0", "xmm1", "xmm2", "xmm3");
    for (int i = 0; i < 4; i++) { E[i] = (UA[i] & 0x80000000u) ? B[i] : A[i]; E[4 + i] = ((5 >> i) & 1) ? B[i] : A[i]; }
    chk("vblendvps.is4", eqf(R, E, 4), fb(R[1]), fb(E[1]));
    chk("vblendps.imm", eqf(R + 4, E + 4, 4), fb(R[4]), fb(E[4]));
    __asm__ volatile("vmovaps (%0), %%ymm1\n\tvblendps $0xa5, (%1), %%ymm1, %%ymm0\n\tvmovaps %%ymm0, (%2)" :: "r"(A), "r"(B), "r"(R) : "memory", "xmm0", "xmm1");
    for (int i = 0; i < 8; i++) E[i] = ((0xa5 >> i) & 1) ? B[i] : A[i];
    chk("vblendps.y", eqf(R, E, 8), fb(R[7]), fb(E[7]));

    __asm__ volatile("vmovdqu (%0), %%xmm1\n\tvpshufd $0x1b, %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, (%1)\n\tvmovups (%2), %%xmm2\n\tvshufps $0x44, %%xmm2, %%xmm1, %%xmm0\n\tvmovups %%xmm0, 16(%1)"
                     :: "r"(UA), "r"(UR), "r"(B) : "memory", "xmm0", "xmm1", "xmm2");
    { u32 want[4] = { UA[3], UA[2], UA[1], UA[0] }; chk("vpshufd.x", equ(UR, want, 4), UR[0], want[0]);
      u32 w2[4] = { UA[0], UA[1], fb(B[0]), fb(B[1]) }; chk("vshufps.x", equ(UR + 4, w2, 4), UR[6], w2[2]); }
    { u64 v = 0x12345678u;
      __asm__ volatile("vmovdqu (%0), %%xmm1\n\tvpinsrd $2, %k1, %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, (%2)" :: "r"(UA), "r"(v), "r"(UR) : "memory", "xmm0", "xmm1");
      u32 want[4] = { UA[0], UA[1], 0x12345678u, UA[3] }; chk("vpinsrd.x", equ(UR, want, 4), UR[2], want[2]); }
    __asm__ volatile("vmovdqu (%1), %%xmm1\n\tvpextrd $3, %%xmm1, %k0" : "=r"(g) : "r"(UA) : "memory", "xmm1");
    chk("vpextrd.x", (u32)g == UA[3], g, UA[3]);
    __asm__ volatile("vmovups (%0), %%xmm1\n\tvmovups (%1), %%xmm2\n\tvinsertps $0x30, %%xmm2, %%xmm1, %%xmm0\n\tvmovups %%xmm0, (%2)\n\tvunpcklps %%xmm2, %%xmm1, %%xmm0\n\tvmovups %%xmm0, 16(%2)\n\tvmovlhps %%xmm2, %%xmm1, %%xmm0\n\tvmovups %%xmm0, 32(%2)"
                     :: "r"(A), "r"(B), "r"(R) : "memory", "xmm0", "xmm1", "xmm2");
    { f32 w[4] = { A[0], A[1], A[2], B[0] }; chk("vinsertps.x", eqf(R, w, 4), fb(R[3]), fb(w[3]));
      f32 w2[4] = { A[0], B[0], A[1], B[1] }; chk("vunpcklps.x", eqf(R + 4, w2, 4), fb(R[5]), fb(w2[1]));
      f32 w3[4] = { A[0], A[1], B[0], B[1] }; chk("vmovlhps.x", eqf(R + 8, w3, 4), fb(R[10]), fb(w3[2])); }
    __asm__ volatile("vmovaps (%0), %%ymm1\n\tvextractf128 $1, %%ymm1, %%xmm0\n\tvmovups %%xmm0, (%1)\n\tvinsertf128 $1, (%2), %%ymm1, %%ymm0\n\tvmovaps %%ymm0, 16(%1)\n\tvperm2f128 $1, %%ymm1, %%ymm1, %%ymm0\n\tvmovaps %%ymm0, 48(%1)"
                     :: "r"(A), "r"(R), "r"(B) : "memory", "xmm0", "xmm1");
    chk("vextractf128", eqf(R, A + 4, 4), fb(R[0]), fb(A[4]));
    { f32 w[8] = { A[0], A[1], A[2], A[3], B[0], B[1], B[2], B[3] }; chk("vinsertf128", eqf(R + 4, w, 8), fb(R[11]), fb(w[7])); }
    { f32 w[8] = { A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3] }; chk("vperm2f128", eqf(R + 12, w, 8), fb(R[12]), fb(w[0])); }
    __asm__ volatile("vmovups (%0), %%xmm1\n\tvpermilps $0x1b, %%xmm1, %%xmm0\n\tvmovups %%xmm0, (%1)\n\tvmovdqu (%2), %%xmm2\n\tvpermilps %%xmm2, %%xmm1, %%xmm0\n\tvmovups %%xmm0, 16(%1)"
                     :: "r"(A), "r"(R), "r"(UB) : "memory", "xmm0", "xmm1", "xmm2");
    { f32 w[4] = { A[3], A[2], A[1], A[0] }; chk("vpermilps.imm", eqf(R, w, 4), fb(R[0]), fb(w[0]));
      f32 w2[4] = { A[UB[0] & 3], A[UB[1] & 3], A[UB[2] & 3], A[UB[3] & 3] }; chk("vpermilps.reg", eqf(R + 4, w2, 4), fb(R[4]), fb(w2[0])); }

    __asm__ volatile("vmovdqu (%0), %%xmm1\n\tvmovdqu (%1), %%xmm2\n\tvpackssdw %%xmm2, %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, (%2)\n\tvpunpckldq %%xmm2, %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, 16(%2)"
                     :: "r"(UA), "r"(UB), "r"(UR) : "memory", "xmm0", "xmm1", "xmm2");
    { u16 w[8]; for (int i = 0; i < 4; i++) { int v = (int)UA[i]; w[i] = (u16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); v = (int)UB[i]; w[4 + i] = (u16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
      chk("vpackssdw.x", __builtin_memcmp(UR, w, 16) == 0, UR[0], ((u32 *)w)[0]);
      u32 w2[4] = { UA[0], UB[0], UA[1], UB[1] }; chk("vpunpckldq.x", equ(UR + 4, w2, 4), UR[5], w2[1]); }
    {
        u8 idx[16] = { 3, 2, 1, 0, 0x80, 5, 6, 7, 15, 14, 13, 12, 8, 9, 10, 11 };
        __asm__ volatile("vmovdqu (%0), %%xmm1\n\tvmovdqu (%1), %%xmm2\n\tvpshufb %%xmm2, %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, (%2)" :: "r"(UA), "r"(idx), "r"(UR) : "memory", "xmm0", "xmm1", "xmm2");
        u8 w[16]; const u8 *src = (const u8 *)UA; for (int i = 0; i < 16; i++) w[i] = (idx[i] & 0x80) ? 0 : src[idx[i] & 15];
        chk("vpshufb.x", __builtin_memcmp(UR, w, 16) == 0, UR[1], ((u32 *)w)[1]);
        __asm__ volatile("vpmovzxbd (%0), %%xmm0\n\tvmovdqu %%xmm0, (%1)\n\tvpmovsxbd (%0), %%xmm0\n\tvmovdqu %%xmm0, 16(%1)\n\tvpmovzxbw (%0), %%ymm0\n\tvmovdqu %%ymm0, 32(%1)"
                         :: "r"(idx), "r"(UR) : "memory", "xmm0");
        u32 wz[4] = { 3, 2, 1, 0 }, ws[4] = { 3, 2, 1, 0 }; chk("vpmovzxbd.x", equ(UR, wz, 4), UR[0], wz[0]); chk("vpmovsxbd.x", equ(UR + 4, ws, 4), UR[4], ws[0]);
        u16 ww[16]; for (int i = 0; i < 16; i++) ww[i] = idx[i]; chk("vpmovzxbw.y", __builtin_memcmp(UR + 8, ww, 32) == 0, UR[10], ((u32 *)ww)[2]);
    }

    __asm__ volatile("vmovups (%0), %%xmm1\n\tvmovups (%1), %%xmm2\n\tvmovss %%xmm2, %%xmm1, %%xmm0\n\tvmovups %%xmm0, (%2)\n\tvmulss %%xmm2, %%xmm1, %%xmm0\n\tvmovups %%xmm0, 16(%2)\n\tvminss %%xmm2, %%xmm1, %%xmm0\n\tvmovups %%xmm0, 32(%2)"
                     :: "r"(A), "r"(B), "r"(R) : "memory", "xmm0", "xmm1", "xmm2");
    { f32 w[4] = { B[0], A[1], A[2], A[3] }; chk("vmovss.merge", eqf(R, w, 4), fb(R[1]), fb(w[1]));
      f32 w2[4] = { A[0] * B[0], A[1], A[2], A[3] }; chk("vmulss.x", eqf(R + 4, w2, 4), fb(R[4]), fb(w2[0]));
      f32 w3[4] = { A[0] < B[0] ? A[0] : B[0], A[1], A[2], A[3] }; chk("vminss.x", eqf(R + 8, w3, 4), fb(R[8]), fb(w3[0])); }
    { u64 iv = (u64)(unsigned)-7;
      __asm__ volatile("vmovups (%0), %%xmm1\n\tvcvtsi2ss %k1, %%xmm1, %%xmm0\n\tvmovups %%xmm0, (%2)" :: "r"(A), "r"(iv), "r"(R) : "memory", "xmm0", "xmm1");
      f32 w[4] = { -7.0f, A[1], A[2], A[3] }; chk("vcvtsi2ss.merge", eqf(R, w, 4), fb(R[0]), fb(w[0])); }
    { u64 q = 0x1122334455667788ull;
      __asm__ volatile("vmovq %1, %%xmm0\n\tvpaddd %%xmm0, %%xmm0, %%xmm0\n\tvmovq %%xmm0, %0" : "=r"(g) : "r"(q) : "xmm0");
      chk("vmovq.gpr", g == 0x22446688aaccef10ull, g, 0x22446688aaccef10ull); }

    { u64 fl;
      __asm__ volatile("vmovdqu (%1), %%xmm1\n\tvmovdqu (%2), %%xmm2\n\tvtestps %%xmm2, %%xmm1\n\tpushfq\n\tpopq %0" : "=r"(fl) : "r"(UA), "r"(UB) : "memory", "xmm1", "xmm2", "cc");
      chk("vtestps.flags", (fl & 0x40) && (fl & 1), fl & 0x41, 0x41); }

    __asm__ volatile("vmovups (%0), %%xmm1\n\tvcvtps2ph $0, %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, (%1)\n\tvcvtph2ps %%xmm0, %%xmm2\n\tvmovups %%xmm2, 16(%1)" :: "r"(A), "r"(R) : "memory", "xmm0", "xmm1", "xmm2");
    { u16 w[4] = { 0x3e00, 0xc080, 0x4200, 0x44c0 }; chk("vcvtps2ph.x", __builtin_memcmp(R, w, 8) == 0, ((u32 *)R)[0], ((u32 *)w)[0]);
      chk("vcvtph2ps.x", eqf(R + 4, A, 4), fb(R[7]), fb(A[3])); }

    __asm__ volatile("vmovaps (%0), %%ymm3\n\tvzeroupper\n\tvextractf128 $1, %%ymm3, (%1)\n\tvmovaps %%xmm3, 16(%1)" :: "r"(A), "r"(R) : "memory", "xmm3");
    chk("vzeroupper", fb(R[0]) == 0 && fb(R[3]) == 0 && eqf(R + 4, A, 4), fb(R[0]) | fb(R[3]), 0);

    if (fails) { g_puts("FAILURES: "); g_putu64((u64)fails); return 1; }
    g_puts("ALL OK\n");
    return 0;
}

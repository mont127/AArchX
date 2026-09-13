#include <immintrin.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef union v256 {
    uint8_t u8[32];
    uint16_t u16[16];
    uint32_t u32[8];
    int32_t i32[8];
    uint64_t u64[4];
    int64_t i64[4];
    float f[8];
    double d[4];
} v256;

#define LPS(p) _mm256_loadu_ps((const float *)(void *)(p))
#define LPD(p) _mm256_loadu_pd((const double *)(void *)(p))
#define LSI(p) _mm256_loadu_si256((const __m256i *)(void *)(p))
#define LPS128(p) _mm_loadu_ps((const float *)(void *)(p))
#define LPD128(p) _mm_loadu_pd((const double *)(void *)(p))
#define LSI128(p) _mm_loadu_si128((const __m128i *)(void *)(p))
#define SPS(p, v) _mm256_storeu_ps((float *)(void *)(p), (v))
#define SPD(p, v) _mm256_storeu_pd((double *)(void *)(p), (v))
#define SSI(p, v) _mm256_storeu_si256((__m256i *)(void *)(p), (v))
#define SPS128(p, v) _mm_storeu_ps((float *)(void *)(p), (v))
#define SPD128(p, v) _mm_storeu_pd((double *)(void *)(p), (v))
#define SSI128(p, v) _mm_storeu_si128((__m128i *)(void *)(p), (v))

static uint64_t rng = 0x94d049bb133111ebull;
static int fails;

static uint64_t next64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void report(const char *name, int iter, const uint8_t *got, const uint8_t *want, int n)
{
    if (memcmp(got, want, (size_t)n) == 0)
        return;
    if (fails < 12) {
        printf("BAD %s iteration %d\n got ", name, iter);
        for (int i = 0; i < n; i++)
            printf("%02x", got[i]);
        printf("\nwant ");
        for (int i = 0; i < n; i++)
            printf("%02x", want[i]);
        printf("\n");
    }
    fails++;
}

#define PRED32(F)                                                                                                    \
    switch (p) {                                                                                                     \
    case 0: r = F(x, y, 0); break; case 1: r = F(x, y, 1); break; case 2: r = F(x, y, 2); break;                     \
    case 3: r = F(x, y, 3); break; case 4: r = F(x, y, 4); break; case 5: r = F(x, y, 5); break;                     \
    case 6: r = F(x, y, 6); break; case 7: r = F(x, y, 7); break; case 8: r = F(x, y, 8); break;                     \
    case 9: r = F(x, y, 9); break; case 10: r = F(x, y, 10); break; case 11: r = F(x, y, 11); break;                 \
    case 12: r = F(x, y, 12); break; case 13: r = F(x, y, 13); break; case 14: r = F(x, y, 14); break;               \
    case 15: r = F(x, y, 15); break; case 16: r = F(x, y, 16); break; case 17: r = F(x, y, 17); break;               \
    case 18: r = F(x, y, 18); break; case 19: r = F(x, y, 19); break; case 20: r = F(x, y, 20); break;               \
    case 21: r = F(x, y, 21); break; case 22: r = F(x, y, 22); break; case 23: r = F(x, y, 23); break;               \
    case 24: r = F(x, y, 24); break; case 25: r = F(x, y, 25); break; case 26: r = F(x, y, 26); break;               \
    case 27: r = F(x, y, 27); break; case 28: r = F(x, y, 28); break; case 29: r = F(x, y, 29); break;               \
    case 30: r = F(x, y, 30); break; default: r = F(x, y, 31); break;                                                \
    }

#define IMM16(F)                                                                                                     \
    switch (imm) {                                                                                                   \
    case 0: r = F(0); break; case 1: r = F(1); break; case 2: r = F(2); break; case 3: r = F(3); break;             \
    case 4: r = F(4); break; case 5: r = F(5); break; case 6: r = F(6); break; case 7: r = F(7); break;             \
    case 8: r = F(8); break; case 9: r = F(9); break; case 10: r = F(10); break; case 11: r = F(11); break;         \
    case 12: r = F(12); break; case 13: r = F(13); break; case 14: r = F(14); break; default: r = F(15); break;     \
    }

__attribute__((target("avx"), noinline)) static void cmp_ps256(const uint8_t *a, const uint8_t *b, int p, uint8_t *out)
{
    __m256 x = LPS(a), y = LPS(b), r;
    PRED32(_mm256_cmp_ps)
    SPS(out, r);
}

__attribute__((target("avx"), noinline)) static void cmp_pd256(const uint8_t *a, const uint8_t *b, int p, uint8_t *out)
{
    __m256d x = LPD(a), y = LPD(b), r;
    PRED32(_mm256_cmp_pd)
    SPD(out, r);
}

__attribute__((target("avx"), noinline)) static void cmp_ps128(const uint8_t *a, const uint8_t *b, int p, uint8_t *out)
{
    __m128 x = LPS128(a), y = LPS128(b), r;
    PRED32(_mm_cmp_ps)
    SPS128(out, r);
}

__attribute__((target("avx"), noinline)) static void cmp_sd(const uint8_t *a, const uint8_t *b, int p, uint8_t *out)
{
    __m128d x = LPD128(a), y = LPD128(b), r;
    PRED32(_mm_cmp_sd)
    SPD128(out, r);
}

__attribute__((target("avx"), noinline)) static void cmp_ss(const uint8_t *a, const uint8_t *b, int p, uint8_t *out)
{
    __m128 x = LPS128(a), y = LPS128(b), r;
    PRED32(_mm_cmp_ss)
    SPS128(out, r);
}

static int pred(double a, double b, int p)
{
    int un = isnan(a) || isnan(b);
    switch (p & 15) {
    case 0: return !un && a == b;
    case 1: return !un && a < b;
    case 2: return !un && a <= b;
    case 3: return un;
    case 4: return un || a != b;
    case 5: return un || !(a < b);
    case 6: return un || !(a <= b);
    case 7: return !un;
    case 8: return un || a == b;
    case 9: return un || a < b;
    case 10: return un || a <= b;
    case 11: return 0;
    case 12: return !un && a != b;
    case 13: return !un && a >= b;
    case 14: return !un && a > b;
    default: return 1;
    }
}

static double pick_cmp(void)
{
    static const double vals[] = { 0.0, -0.0, 1.0, -1.0, 2.5, NAN, INFINITY, -INFINITY };
    return vals[next64() % 8];
}

static void test_cmp(int iter)
{
    v256 a, b, got, want;
    for (int i = 0; i < 8; i++) {
        a.f[i] = (float)pick_cmp();
        b.f[i] = (float)pick_cmp();
    }
    for (int p = 0; p < 32; p++) {
        char name[48];
        memset(&got, 0, sizeof got);
        memset(&want, 0, sizeof want);
        cmp_ps256(a.u8, b.u8, p, got.u8);
        for (int i = 0; i < 8; i++)
            want.u32[i] = pred(a.f[i], b.f[i], p) ? 0xffffffffu : 0;
        snprintf(name, sizeof name, "vcmpps ymm pred %d", p);
        report(name, iter, got.u8, want.u8, 32);

        memset(&got, 0, sizeof got);
        cmp_ps128(a.u8, b.u8, p, got.u8);
        memset(want.u8 + 16, 0, 16);
        snprintf(name, sizeof name, "vcmpps xmm pred %d", p);
        report(name, iter, got.u8, want.u8, 32);

        memset(&got, 0, sizeof got);
        cmp_ss(a.u8, b.u8, p, got.u8);
        memcpy(want.u8, a.u8, 16);
        want.u32[0] = pred(a.f[0], b.f[0], p) ? 0xffffffffu : 0;
        snprintf(name, sizeof name, "vcmpss pred %d", p);
        report(name, iter, got.u8, want.u8, 32);
    }
    for (int i = 0; i < 4; i++) {
        a.d[i] = pick_cmp();
        b.d[i] = pick_cmp();
    }
    for (int p = 0; p < 32; p++) {
        char name[48];
        memset(&got, 0, sizeof got);
        cmp_pd256(a.u8, b.u8, p, got.u8);
        for (int i = 0; i < 4; i++)
            want.u64[i] = pred(a.d[i], b.d[i], p) ? ~0ull : 0;
        snprintf(name, sizeof name, "vcmppd ymm pred %d", p);
        report(name, iter, got.u8, want.u8, 32);

        memset(&got, 0, sizeof got);
        cmp_sd(a.u8, b.u8, p, got.u8);
        memset(&want, 0, sizeof want);
        memcpy(want.u8, a.u8, 16);
        want.u64[0] = pred(a.d[0], b.d[0], p) ? ~0ull : 0;
        snprintf(name, sizeof name, "vcmpsd pred %d", p);
        report(name, iter, got.u8, want.u8, 32);
    }
}

#define SHUFPD_IMM(i) _mm256_shuffle_pd(x, y, i)
#define PERMILPD_IMM(i) _mm256_permute_pd(x, i)

__attribute__((target("avx"), noinline)) static void shufpd256(const uint8_t *a, const uint8_t *b, int imm, uint8_t *out)
{
    __m256d x = LPD(a), y = LPD(b), r;
    IMM16(SHUFPD_IMM)
    SPD(out, r);
}

__attribute__((target("avx"), noinline)) static void permilpd256(const uint8_t *a, int imm, uint8_t *out)
{
    __m256d x = LPD(a), r;
    IMM16(PERMILPD_IMM)
    SPD(out, r);
}

static const int mpsad_imms[6] = { 0x00, 0x05, 0x12, 0x2d, 0x3f, 0x09 };

__attribute__((target("avx2"), noinline)) static void mpsadbw256(const uint8_t *a, const uint8_t *b, int k, uint8_t *out)
{
    __m256i x = LSI(a), y = LSI(b), r;
    switch (k) {
    case 0: r = _mm256_mpsadbw_epu8(x, y, 0x00); break;
    case 1: r = _mm256_mpsadbw_epu8(x, y, 0x05); break;
    case 2: r = _mm256_mpsadbw_epu8(x, y, 0x12); break;
    case 3: r = _mm256_mpsadbw_epu8(x, y, 0x2d); break;
    case 4: r = _mm256_mpsadbw_epu8(x, y, 0x3f); break;
    default: r = _mm256_mpsadbw_epu8(x, y, 0x09); break;
    }
    SSI(out, r);
}

static void test_imm_lanes(int iter)
{
    v256 a, b, got, want;
    for (int i = 0; i < 32; i++) {
        a.u8[i] = (uint8_t)next64();
        b.u8[i] = (uint8_t)next64();
    }
    for (int imm = 0; imm < 16; imm++) {
        char name[48];
        shufpd256(a.u8, b.u8, imm, got.u8);
        for (int l = 0; l < 2; l++) {
            want.u64[2 * l] = a.u64[2 * l + ((imm >> (2 * l)) & 1)];
            want.u64[2 * l + 1] = b.u64[2 * l + ((imm >> (2 * l + 1)) & 1)];
        }
        snprintf(name, sizeof name, "vshufpd ymm imm %d", imm);
        report(name, iter, got.u8, want.u8, 32);

        permilpd256(a.u8, imm, got.u8);
        for (int l = 0; l < 2; l++)
            for (int i = 0; i < 2; i++)
                want.u64[2 * l + i] = a.u64[2 * l + ((imm >> (2 * l + i)) & 1)];
        snprintf(name, sizeof name, "vpermilpd ymm imm %d", imm);
        report(name, iter, got.u8, want.u8, 32);
    }
    for (int k = 0; k < 6; k++) {
        char name[48];
        mpsadbw256(a.u8, b.u8, k, got.u8);
        for (int l = 0; l < 2; l++) {
            int imm = (mpsad_imms[k] >> (3 * l)) & 7;
            int aoff = ((imm >> 2) & 1) * 4, boff = (imm & 3) * 4;
            for (int i = 0; i < 8; i++) {
                int sum = 0;
                for (int j = 0; j < 4; j++) {
                    int dlt = (int)a.u8[16 * l + aoff + i + j] - (int)b.u8[16 * l + boff + j];
                    sum += dlt < 0 ? -dlt : dlt;
                }
                want.u16[8 * l + i] = (uint16_t)sum;
            }
        }
        snprintf(name, sizeof name, "vmpsadbw ymm imm %#x", mpsad_imms[k]);
        report(name, iter, got.u8, want.u8, 32);
    }
}

__attribute__((target("avx"), noinline)) static void maskmov_fp(const uint8_t *mem, const uint8_t *mask, const uint8_t *val,
                                                              uint8_t *out, uint8_t *sps, uint8_t *spd, uint8_t *sps128)
{
    __m256i m = LSI(mask);
    SPS(out, _mm256_maskload_ps((const float *)(void *)mem, m));
    SPD(out + 32, _mm256_maskload_pd((const double *)(void *)mem, m));
    SPS128(out + 64, _mm_maskload_ps((const float *)(void *)mem, LSI128(mask)));
    SPD128(out + 80, _mm_maskload_pd((const double *)(void *)mem, LSI128(mask)));
    _mm256_maskstore_ps((float *)(void *)sps, m, LPS(val));
    _mm256_maskstore_pd((double *)(void *)spd, m, LPD(val));
    _mm_maskstore_ps((float *)(void *)sps128, LSI128(mask), LPS128(val));
}

static void test_maskmov(int iter)
{
    v256 mem, mask, val, s1, s2, s3, w1, w2, w3;
    uint8_t got[96], want[96];
    for (int i = 0; i < 32; i++) {
        mem.u8[i] = (uint8_t)next64();
        mask.u8[i] = (uint8_t)next64();
        val.u8[i] = (uint8_t)next64();
        s1.u8[i] = s2.u8[i] = s3.u8[i] = (uint8_t)next64();
    }
    w1 = s1;
    w2 = s2;
    w3 = s3;
    maskmov_fp(mem.u8, mask.u8, val.u8, got, s1.u8, s2.u8, s3.u8);
    memset(want, 0, sizeof want);
    for (int i = 0; i < 8; i++) {
        int on = mask.u32[i] >> 31;
        if (on) {
            memcpy(want + 4 * i, mem.u8 + 4 * i, 4);
            w1.u32[i] = val.u32[i];
        }
        if (on && i < 4) {
            memcpy(want + 64 + 4 * i, mem.u8 + 4 * i, 4);
            w3.u32[i] = val.u32[i];
        }
    }
    for (int i = 0; i < 4; i++) {
        int on = (int)(mask.u64[i] >> 63);
        if (on) {
            memcpy(want + 32 + 8 * i, mem.u8 + 8 * i, 8);
            w2.u64[i] = val.u64[i];
        }
        if (on && i < 2)
            memcpy(want + 80 + 8 * i, mem.u8 + 8 * i, 8);
    }
    report("vmaskmovps/pd load", iter, got, want, 96);
    report("vmaskmovps ymm store", iter, s1.u8, w1.u8, 32);
    report("vmaskmovpd ymm store", iter, s2.u8, w2.u8, 32);
    report("vmaskmovps xmm store", iter, s3.u8, w3.u8, 32);
}

#define GATHER_FN(fname, tgt, rtype, stype, itype, mtype, loadS, loadI, loadM, store, call, btype)                     \
    __attribute__((target(tgt), noinline)) static void fname(const uint8_t *base, const uint8_t *src, const uint8_t *idx, \
                                                           const uint8_t *mask, int scale, uint8_t *out)             \
    {                                                                                                                \
        stype s = loadS(src);                                                                                        \
        itype i = loadI(idx);                                                                                        \
        mtype m = loadM(mask);                                                                                       \
        rtype r;                                                                                                     \
        switch (scale) {                                                                                             \
        case 1: r = call(s, (btype)(void *)base, i, m, 1); break;                                                    \
        case 2: r = call(s, (btype)(void *)base, i, m, 2); break;                                                    \
        case 4: r = call(s, (btype)(void *)base, i, m, 4); break;                                                    \
        default: r = call(s, (btype)(void *)base, i, m, 8); break;                                                   \
        }                                                                                                            \
        store(out, r);                                                                                               \
    }

GATHER_FN(g_dd128, "avx2", __m128i, __m128i, __m128i, __m128i, LSI128, LSI128, LSI128, SSI128, _mm_mask_i32gather_epi32, const int *)
GATHER_FN(g_dd256, "avx2", __m256i, __m256i, __m256i, __m256i, LSI, LSI, LSI, SSI, _mm256_mask_i32gather_epi32, const int *)
GATHER_FN(g_dq128, "avx2", __m128i, __m128i, __m128i, __m128i, LSI128, LSI128, LSI128, SSI128, _mm_mask_i32gather_epi64, const long long *)
GATHER_FN(g_dq256, "avx2", __m256i, __m256i, __m128i, __m256i, LSI, LSI128, LSI, SSI, _mm256_mask_i32gather_epi64, const long long *)
GATHER_FN(g_qd128, "avx2", __m128i, __m128i, __m128i, __m128i, LSI128, LSI128, LSI128, SSI128, _mm_mask_i64gather_epi32, const int *)
GATHER_FN(g_qd256, "avx2", __m128i, __m128i, __m256i, __m128i, LSI128, LSI, LSI128, SSI128, _mm256_mask_i64gather_epi32, const int *)
GATHER_FN(g_qq128, "avx2", __m128i, __m128i, __m128i, __m128i, LSI128, LSI128, LSI128, SSI128, _mm_mask_i64gather_epi64, const long long *)
GATHER_FN(g_qq256, "avx2", __m256i, __m256i, __m256i, __m256i, LSI, LSI, LSI, SSI, _mm256_mask_i64gather_epi64, const long long *)
GATHER_FN(g_dps128, "avx2", __m128, __m128, __m128i, __m128, LPS128, LSI128, LPS128, SPS128, _mm_mask_i32gather_ps, const float *)
GATHER_FN(g_dps256, "avx2", __m256, __m256, __m256i, __m256, LPS, LSI, LPS, SPS, _mm256_mask_i32gather_ps, const float *)
GATHER_FN(g_dpd128, "avx2", __m128d, __m128d, __m128i, __m128d, LPD128, LSI128, LPD128, SPD128, _mm_mask_i32gather_pd, const double *)
GATHER_FN(g_dpd256, "avx2", __m256d, __m256d, __m128i, __m256d, LPD, LSI128, LPD, SPD, _mm256_mask_i32gather_pd, const double *)
GATHER_FN(g_qps128, "avx2", __m128, __m128, __m128i, __m128, LPS128, LSI128, LPS128, SPS128, _mm_mask_i64gather_ps, const float *)
GATHER_FN(g_qps256, "avx2", __m128, __m128, __m256i, __m128, LPS128, LSI, LPS128, SPS128, _mm256_mask_i64gather_ps, const float *)
GATHER_FN(g_qpd128, "avx2", __m128d, __m128d, __m128i, __m128d, LPD128, LSI128, LPD128, SPD128, _mm_mask_i64gather_pd, const double *)
GATHER_FN(g_qpd256, "avx2", __m256d, __m256d, __m256i, __m256d, LPD, LSI, LPD, SPD, _mm256_mask_i64gather_pd, const double *)

typedef void (*gather_fn)(const uint8_t *, const uint8_t *, const uint8_t *, const uint8_t *, int, uint8_t *);

static const struct {
    gather_fn f;
    const char *name;
    int esz, qidx, n, outbytes;
} gcases[] = {
    { g_dd128, "vpgatherdd xmm", 4, 0, 4, 16 },  { g_dd256, "vpgatherdd ymm", 4, 0, 8, 32 },
    { g_dq128, "vpgatherdq xmm", 8, 0, 2, 16 },  { g_dq256, "vpgatherdq ymm", 8, 0, 4, 32 },
    { g_qd128, "vpgatherqd xmm", 4, 1, 2, 16 },  { g_qd256, "vpgatherqd ymm", 4, 1, 4, 16 },
    { g_qq128, "vpgatherqq xmm", 8, 1, 2, 16 },  { g_qq256, "vpgatherqq ymm", 8, 1, 4, 32 },
    { g_dps128, "vgatherdps xmm", 4, 0, 4, 16 }, { g_dps256, "vgatherdps ymm", 4, 0, 8, 32 },
    { g_dpd128, "vgatherdpd xmm", 8, 0, 2, 16 }, { g_dpd256, "vgatherdpd ymm", 8, 0, 4, 32 },
    { g_qps128, "vgatherqps xmm", 4, 1, 2, 16 }, { g_qps256, "vgatherqps ymm", 4, 1, 4, 16 },
    { g_qpd128, "vgatherqpd xmm", 8, 1, 2, 16 }, { g_qpd256, "vgatherqpd ymm", 8, 1, 4, 32 },
};

static uint8_t gtable[8192];

static void test_gather(int iter)
{
    static const int scales[4] = { 1, 2, 4, 8 };
    const uint8_t *base = gtable + 4096;
    for (size_t c = 0; c < sizeof gcases / sizeof gcases[0]; c++) {
        v256 src, idx, mask, got, want;
        char name[48];
        int scale = scales[next64() % 4];
        for (int i = 0; i < 32; i++) {
            src.u8[i] = (uint8_t)next64();
            mask.u8[i] = (uint8_t)next64();
        }
        for (int i = 0; i < 8; i++)
            idx.i32[i] = (int32_t)(next64() % 201) - 100;
        if (gcases[c].qidx)
            for (int i = 0; i < 4; i++)
                idx.i64[i] = (int64_t)(next64() % 201) - 100;
        memset(&got, 0, sizeof got);
        gcases[c].f(base, src.u8, idx.u8, mask.u8, scale, got.u8);
        int esz = gcases[c].esz, n = gcases[c].n;
        memset(&want, 0, sizeof want);
        memcpy(want.u8, src.u8, (size_t)gcases[c].outbytes);
        for (int i = 0; i < n; i++) {
            if (!(mask.u8[esz * i + esz - 1] & 0x80))
                continue;
            int64_t ix = gcases[c].qidx ? idx.i64[i] : (int64_t)idx.i32[i];
            memcpy(want.u8 + esz * i, base + ix * scale, (size_t)esz);
        }
        memset(want.u8 + n * esz, 0, (size_t)(gcases[c].outbytes - n * esz));
        snprintf(name, sizeof name, "%s scale %d", gcases[c].name, scale);
        report(name, iter, got.u8, want.u8, 32);
    }
}

#define RAW3(name, bytes) extern void name(const uint8_t *, const uint8_t *, const uint8_t *, uint8_t *); \
    __asm__(".text\n.p2align 4\n.globl _" #name "\n_" #name ":\n vmovdqu (%rdi), %ymm0\n vmovdqu (%rsi), %ymm1\n"  \
            " vmovdqu (%rdx), %ymm2\n movq (%rdx), %rax\n .byte " bytes "\n vmovdqu %ymm0, (%rcx)\n vzeroupper\n ret\n")

RAW3(lig_addsd, "0xc5, 0xf7, 0x58, 0xc2");
RAW3(lig_sqrtss, "0xc5, 0xf6, 0x51, 0xc2");
RAW3(lig_cvtsi2sd, "0xc4, 0xe1, 0xf7, 0x2a, 0xc0");

#define LANE(name, body) extern void name(const uint8_t *, const uint8_t *, const uint8_t *, uint8_t *); \
    __asm__(".text\n.p2align 4\n.globl _" #name "\n_" #name ":\n vmovdqu (%rdi), %ymm1\n vmovdqu (%rsi), %ymm2\n"  \
            " vmovdqu (%rdx), %ymm3\n vpcmpeqd %ymm0, %ymm0, %ymm0\n " body "\n vmovdqu %ymm0, (%rcx)\n vzeroupper\n ret\n")

LANE(l_addps, "vaddps (%rsi), %ymm1, %ymm0");
LANE(l_mulpd, "vmulpd %ymm2, %ymm1, %ymm0");
LANE(l_unpckhpd, "vunpckhpd (%rsi), %ymm1, %ymm0");
LANE(l_shufps, "vshufps $0x9c, (%rsi), %ymm1, %ymm0");
LANE(l_blendvps, "vblendvps %ymm3, (%rsi), %ymm1, %ymm0");
LANE(l_dpps, "vdpps $0xf1, %ymm2, %ymm1, %ymm0");
LANE(l_roundpd, "vroundpd $1, (%rsi), %ymm0");
LANE(l_movddup, "vmovddup (%rsi), %ymm0");
LANE(l_haddpd, "vhaddpd %ymm2, %ymm1, %ymm0");
LANE(l_cvtpd2ps, "vcvtpd2psy (%rsi), %xmm0");
LANE(l_cvtdq2pd, "vcvtdq2pd (%rsi), %ymm0");
LANE(l_andnpd, "vandnpd (%rsi), %ymm1, %ymm0");
LANE(l_minps, "vminps %ymm2, %ymm1, %ymm0");
LANE(l_sqrtpd, "vsqrtpd (%rsi), %ymm0");
LANE(l_permilps, "vpermilps (%rsi), %ymm1, %ymm0");
LANE(l_movmskpd, "vmovmskpd %ymm2, %eax\n vpxor %xmm0, %xmm0, %xmm0\n vmovq %rax, %xmm0");
LANE(l_testps, "vtestps %ymm2, %ymm1\n setz %al\n setc %ah\n vpxor %xmm0, %xmm0, %xmm0\n movzwl %ax, %eax\n vmovq %rax, %xmm0");

static double small_d(void)
{
    return (double)((int64_t)(next64() % 2001) - 1000) / 8.0;
}

static void test_lig_and_lanes(int iter)
{
    v256 a, b, c, got, want;
    for (int i = 0; i < 4; i++) {
        a.d[i] = ldexp((double)(int64_t)(next64() >> 11), -40);
        b.d[i] = ldexp((double)(int64_t)(next64() >> 11), -40);
        c.d[i] = ldexp((double)(int64_t)(next64() >> 11), -40);
    }

    lig_addsd(a.u8, b.u8, c.u8, got.u8);
    memset(&want, 0, sizeof want);
    want.d[0] = b.d[0] + c.d[0];
    want.u64[1] = b.u64[1];
    report("vaddsd with VEX.L=1", iter, got.u8, want.u8, 32);

    c.f[0] = fabsf(c.f[0]);
    lig_sqrtss(a.u8, b.u8, c.u8, got.u8);
    memset(&want, 0, sizeof want);
    memcpy(want.u8, b.u8, 16);
    want.f[0] = sqrtf(c.f[0]);
    report("vsqrtss with VEX.L=1", iter, got.u8, want.u8, 32);

    c.i64[0] = (int64_t)next64() >> (next64() % 60);
    lig_cvtsi2sd(a.u8, b.u8, c.u8, got.u8);
    memset(&want, 0, sizeof want);
    want.d[0] = (double)c.i64[0];
    want.u64[1] = b.u64[1];
    report("vcvtsi2sd with VEX.L=1", iter, got.u8, want.u8, 32);

    for (int i = 0; i < 8; i++) {
        a.f[i] = (float)small_d();
        b.f[i] = (float)small_d();
        c.u32[i] = (uint32_t)next64();
    }
    l_addps(a.u8, b.u8, c.u8, got.u8);
    for (int i = 0; i < 8; i++)
        want.f[i] = a.f[i] + b.f[i];
    report("vaddps ymm,mem", iter, got.u8, want.u8, 32);

    l_shufps(a.u8, b.u8, c.u8, got.u8);
    for (int l = 0; l < 2; l++) {
        want.u32[4 * l + 0] = a.u32[4 * l + (0x9c & 3)];
        want.u32[4 * l + 1] = a.u32[4 * l + ((0x9c >> 2) & 3)];
        want.u32[4 * l + 2] = b.u32[4 * l + ((0x9c >> 4) & 3)];
        want.u32[4 * l + 3] = b.u32[4 * l + ((0x9c >> 6) & 3)];
    }
    report("vshufps ymm,mem", iter, got.u8, want.u8, 32);

    l_blendvps(a.u8, b.u8, c.u8, got.u8);
    for (int i = 0; i < 8; i++)
        want.u32[i] = (c.u32[i] >> 31) ? b.u32[i] : a.u32[i];
    report("vblendvps ymm,mem", iter, got.u8, want.u8, 32);

    l_dpps(a.u8, b.u8, c.u8, got.u8);
    memset(&want, 0, sizeof want);
    for (int l = 0; l < 2; l++)
        want.f[4 * l] = (a.f[4 * l] * b.f[4 * l] + a.f[4 * l + 1] * b.f[4 * l + 1]) +
                        (a.f[4 * l + 2] * b.f[4 * l + 2] + a.f[4 * l + 3] * b.f[4 * l + 3]);
    report("vdpps ymm imm 0xf1", iter, got.u8, want.u8, 32);

    l_minps(a.u8, b.u8, c.u8, got.u8);
    for (int i = 0; i < 8; i++)
        want.f[i] = a.f[i] < b.f[i] ? a.f[i] : b.f[i];
    report("vminps ymm", iter, got.u8, want.u8, 32);

    for (int i = 0; i < 8; i++)
        c.u32[i] = (uint32_t)next64();
    l_permilps(a.u8, c.u8, c.u8, got.u8);
    for (int l = 0; l < 2; l++)
        for (int i = 0; i < 4; i++)
            want.u32[4 * l + i] = a.u32[4 * l + (c.u32[4 * l + i] & 3)];
    report("vpermilps ymm,mem", iter, got.u8, want.u8, 32);

    l_testps(a.u8, c.u8, c.u8, got.u8);
    {
        int z = 1, cy = 1;
        for (int i = 0; i < 8; i++) {
            if ((a.u32[i] & c.u32[i]) >> 31)
                z = 0;
            if ((~a.u32[i] & c.u32[i]) >> 31)
                cy = 0;
        }
        memset(&want, 0, sizeof want);
        want.u8[0] = (uint8_t)z;
        want.u8[1] = (uint8_t)cy;
    }
    report("vtestps ymm", iter, got.u8, want.u8, 32);

    for (int i = 0; i < 4; i++) {
        a.d[i] = small_d();
        b.d[i] = small_d();
        c.u64[i] = next64();
    }
    l_mulpd(a.u8, b.u8, c.u8, got.u8);
    for (int i = 0; i < 4; i++)
        want.d[i] = a.d[i] * b.d[i];
    report("vmulpd ymm", iter, got.u8, want.u8, 32);

    l_unpckhpd(a.u8, b.u8, c.u8, got.u8);
    for (int l = 0; l < 2; l++) {
        want.u64[2 * l] = a.u64[2 * l + 1];
        want.u64[2 * l + 1] = b.u64[2 * l + 1];
    }
    report("vunpckhpd ymm,mem", iter, got.u8, want.u8, 32);

    l_roundpd(a.u8, b.u8, c.u8, got.u8);
    for (int i = 0; i < 4; i++)
        want.d[i] = floor(b.d[i]);
    report("vroundpd ymm floor", iter, got.u8, want.u8, 32);

    l_movddup(a.u8, b.u8, c.u8, got.u8);
    for (int l = 0; l < 2; l++)
        want.u64[2 * l] = want.u64[2 * l + 1] = b.u64[2 * l];
    report("vmovddup ymm,mem", iter, got.u8, want.u8, 32);

    l_haddpd(a.u8, b.u8, c.u8, got.u8);
    for (int l = 0; l < 2; l++) {
        want.d[2 * l] = a.d[2 * l] + a.d[2 * l + 1];
        want.d[2 * l + 1] = b.d[2 * l] + b.d[2 * l + 1];
    }
    report("vhaddpd ymm", iter, got.u8, want.u8, 32);

    l_cvtpd2ps(a.u8, b.u8, c.u8, got.u8);
    memset(&want, 0, sizeof want);
    for (int i = 0; i < 4; i++)
        want.f[i] = (float)b.d[i];
    report("vcvtpd2ps ymm mem", iter, got.u8, want.u8, 32);

    l_andnpd(a.u8, b.u8, c.u8, got.u8);
    for (int i = 0; i < 4; i++)
        want.u64[i] = ~a.u64[i] & b.u64[i];
    report("vandnpd ymm,mem", iter, got.u8, want.u8, 32);

    for (int i = 0; i < 4; i++)
        b.d[i] = fabs(b.d[i]);
    l_sqrtpd(a.u8, b.u8, c.u8, got.u8);
    for (int i = 0; i < 4; i++)
        want.d[i] = sqrt(b.d[i]);
    report("vsqrtpd ymm,mem", iter, got.u8, want.u8, 32);

    l_movmskpd(a.u8, c.u8, c.u8, got.u8);
    memset(&want, 0, sizeof want);
    for (int i = 0; i < 4; i++)
        want.u8[0] |= (uint8_t)((c.u64[i] >> 63) << i);
    report("vmovmskpd ymm", iter, got.u8, want.u8, 32);

    for (int i = 0; i < 8; i++)
        b.i32[i] = (int32_t)next64();
    l_cvtdq2pd(a.u8, b.u8, c.u8, got.u8);
    for (int i = 0; i < 4; i++)
        want.d[i] = (double)b.i32[i];
    report("vcvtdq2pd ymm,mem", iter, got.u8, want.u8, 32);
}

int main(void)
{
    for (size_t i = 0; i < sizeof gtable; i++)
        gtable[i] = (uint8_t)next64();
    for (int iter = 0; iter < 40; iter++) {
        test_cmp(iter);
        test_imm_lanes(iter);
        test_maskmov(iter);
        test_gather(iter);
        test_lig_and_lanes(iter);
    }
    if (!fails)
        printf("OK\n");
    return fails != 0;
}

#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LOADU(p) _mm256_loadu_si256((const __m256i *)(p))
#define STOREU(p, v) _mm256_storeu_si256((__m256i *)(p), (v))
#define LOADU128(p) _mm_loadu_si128((const __m128i *)(p))
#define STOREU128(p, v) _mm_storeu_si128((__m128i *)(p), (v))

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static int fails;

static uint64_t next64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void fill(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)next64();
}

static uint32_t get32(const uint8_t *p, int i)
{
    uint32_t v;
    memcpy(&v, p + 4 * i, 4);
    return v;
}

static void put32(uint8_t *p, int i, uint32_t v)
{
    memcpy(p + 4 * i, &v, 4);
}

static uint64_t get64(const uint8_t *p, int i)
{
    uint64_t v;
    memcpy(&v, p + 8 * i, 8);
    return v;
}

static void put64(uint8_t *p, int i, uint64_t v)
{
    memcpy(p + 8 * i, &v, 8);
}

static void check(const char *name, int iter, const uint8_t *got, const uint8_t *want, size_t n)
{
    if (memcmp(got, want, n) != 0) {
        if (fails < 12)
            printf("BAD %s iteration %d\n", name, iter);
        fails++;
    }
}

__attribute__((target("avx2"), noinline)) static void t_broadcast(const uint8_t *src, uint8_t *out)
{
    __m128i x = LOADU128(src);
    STOREU(out, _mm256_broadcastb_epi8(x));
    STOREU(out + 32, _mm256_broadcastw_epi16(x));
    STOREU(out + 64, _mm256_broadcastd_epi32(x));
    STOREU(out + 96, _mm256_broadcastq_epi64(x));
    STOREU128(out + 128, _mm_broadcastb_epi8(x));
    STOREU128(out + 144, _mm_broadcastw_epi16(x));
    STOREU128(out + 160, _mm_broadcastd_epi32(x));
    STOREU128(out + 176, _mm_broadcastq_epi64(x));
    STOREU(out + 192, _mm256_broadcastsi128_si256(x));
    __m128i y = _mm_add_epi8(x, _mm_set1_epi8(1));
    STOREU(out + 224, _mm256_broadcastb_epi8(y));
    STOREU(out + 256, _mm256_broadcastd_epi32(y));
}

static void r_broadcast(const uint8_t *src, uint8_t *out)
{
    uint8_t inc[16];
    for (int i = 0; i < 16; i++)
        inc[i] = (uint8_t)(src[i] + 1);
    for (int i = 0; i < 32; i++)
        out[i] = src[0];
    for (int i = 0; i < 32; i += 2)
        memcpy(out + 32 + i, src, 2);
    for (int i = 0; i < 32; i += 4)
        memcpy(out + 64 + i, src, 4);
    for (int i = 0; i < 32; i += 8)
        memcpy(out + 96 + i, src, 8);
    for (int i = 0; i < 16; i++)
        out[128 + i] = src[0];
    for (int i = 0; i < 16; i += 2)
        memcpy(out + 144 + i, src, 2);
    for (int i = 0; i < 16; i += 4)
        memcpy(out + 160 + i, src, 4);
    for (int i = 0; i < 16; i += 8)
        memcpy(out + 176 + i, src, 8);
    memcpy(out + 192, src, 16);
    memcpy(out + 208, src, 16);
    for (int i = 0; i < 32; i++)
        out[224 + i] = inc[0];
    for (int i = 0; i < 32; i += 4)
        memcpy(out + 256 + i, inc, 4);
}

__attribute__((target("avx2"), noinline)) static void t_lanes(const uint8_t *a, const uint8_t *b, uint8_t *out)
{
    __m256i ya = LOADU(a), yb = LOADU(b);
    __m128i xb = LOADU128(b);
    STOREU(out, _mm256_inserti128_si256(ya, xb, 0));
    STOREU(out + 32, _mm256_inserti128_si256(ya, xb, 1));
    STOREU128(out + 64, _mm256_extracti128_si256(ya, 1));
    STOREU128(out + 80, _mm256_extracti128_si256(ya, 0));
    STOREU(out + 96, _mm256_permute2x128_si256(ya, yb, 0x00));
    STOREU(out + 128, _mm256_permute2x128_si256(ya, yb, 0x01));
    STOREU(out + 160, _mm256_permute2x128_si256(ya, yb, 0x20));
    STOREU(out + 192, _mm256_permute2x128_si256(ya, yb, 0x31));
    STOREU(out + 224, _mm256_permute2x128_si256(ya, yb, 0x08));
    STOREU(out + 256, _mm256_permute2x128_si256(ya, yb, 0x80));
    STOREU(out + 288, _mm256_permute2x128_si256(ya, yb, 0x13));
    STOREU(out + 320, _mm256_permute2x128_si256(ya, yb, 0x02));
}

static void r_perm2(const uint8_t *a, const uint8_t *b, int imm, uint8_t *out)
{
    const uint8_t *pick[4] = { a, a + 16, b, b + 16 };
    memcpy(out, pick[imm & 3], 16);
    memcpy(out + 16, pick[(imm >> 4) & 3], 16);
    if (imm & 0x08)
        memset(out, 0, 16);
    if (imm & 0x80)
        memset(out + 16, 0, 16);
}

static void r_lanes(const uint8_t *a, const uint8_t *b, uint8_t *out)
{
    memcpy(out, b, 16);
    memcpy(out + 16, a + 16, 16);
    memcpy(out + 32, a, 16);
    memcpy(out + 48, b, 16);
    memcpy(out + 64, a + 16, 16);
    memcpy(out + 80, a, 16);
    static const int imms[8] = { 0x00, 0x01, 0x20, 0x31, 0x08, 0x80, 0x13, 0x02 };
    for (int k = 0; k < 8; k++)
        r_perm2(a, b, imms[k], out + 96 + 32 * k);
}

__attribute__((target("avx2"), noinline)) static void t_blendd(const uint8_t *a, const uint8_t *b, uint8_t *out)
{
    __m256i ya = LOADU(a), yb = LOADU(b);
    __m128i xa = LOADU128(a), xb = LOADU128(b);
    STOREU(out, _mm256_blend_epi32(ya, yb, 0xa5));
    STOREU(out + 32, _mm256_blend_epi32(ya, yb, 0x3c));
    STOREU128(out + 64, _mm_blend_epi32(xa, xb, 0x9));
    STOREU128(out + 80, _mm_blend_epi32(xa, xb, 0x6));
}

static void r_blendd(const uint8_t *a, const uint8_t *b, uint8_t *out)
{
    for (int i = 0; i < 8; i++) {
        put32(out, i, ((0xa5 >> i) & 1) ? get32(b, i) : get32(a, i));
        put32(out + 32, i, ((0x3c >> i) & 1) ? get32(b, i) : get32(a, i));
    }
    for (int i = 0; i < 4; i++) {
        put32(out + 64, i, ((0x9 >> i) & 1) ? get32(b, i) : get32(a, i));
        put32(out + 80, i, ((0x6 >> i) & 1) ? get32(b, i) : get32(a, i));
    }
}

__attribute__((target("avx2"), noinline)) static void t_perm(const uint8_t *a, const uint8_t *idx, uint8_t *out)
{
    __m256i ya = LOADU(a), yi = LOADU(idx);
    STOREU(out, _mm256_permutevar8x32_epi32(ya, yi));
    _mm256_storeu_ps((float *)(void *)(out + 32), _mm256_permutevar8x32_ps(_mm256_loadu_ps((const float *)(void *)a), yi));
    STOREU(out + 64, _mm256_permute4x64_epi64(ya, 0x1b));
    STOREU(out + 96, _mm256_permute4x64_epi64(ya, 0xd8));
    _mm256_storeu_pd((double *)(void *)(out + 128), _mm256_permute4x64_pd(_mm256_loadu_pd((const double *)(void *)a), 0x72));
}

static void r_perm(const uint8_t *a, const uint8_t *idx, uint8_t *out)
{
    for (int i = 0; i < 8; i++) {
        put32(out, i, get32(a, (int)(get32(idx, i) & 7)));
        put32(out + 32, i, get32(a, (int)(get32(idx, i) & 7)));
    }
    for (int i = 0; i < 4; i++) {
        put64(out + 64, i, get64(a, (0x1b >> (2 * i)) & 3));
        put64(out + 96, i, get64(a, (0xd8 >> (2 * i)) & 3));
        put64(out + 128, i, get64(a, (0x72 >> (2 * i)) & 3));
    }
}

__attribute__((target("avx2"), noinline)) static void t_shiftv(const uint8_t *a, const uint8_t *c32, const uint8_t *c64,
                                                             uint8_t *out)
{
    __m256i ya = LOADU(a), y32 = LOADU(c32), y64 = LOADU(c64);
    __m128i xa = LOADU128(a), x32 = LOADU128(c32), x64 = LOADU128(c64);
    STOREU(out, _mm256_sllv_epi32(ya, y32));
    STOREU(out + 32, _mm256_srlv_epi32(ya, y32));
    STOREU(out + 64, _mm256_srav_epi32(ya, y32));
    STOREU(out + 96, _mm256_sllv_epi64(ya, y64));
    STOREU(out + 128, _mm256_srlv_epi64(ya, y64));
    STOREU128(out + 160, _mm_sllv_epi32(xa, x32));
    STOREU128(out + 176, _mm_srlv_epi64(xa, x64));
    STOREU128(out + 192, _mm_srav_epi32(xa, x32));
}

static void r_shiftv(const uint8_t *a, const uint8_t *c32, const uint8_t *c64, uint8_t *out)
{
    memset(out, 0, 208);
    for (int i = 0; i < 8; i++) {
        uint32_t v = get32(a, i), c = get32(c32, i);
        put32(out, i, c >= 32 ? 0 : v << c);
        put32(out + 32, i, c >= 32 ? 0 : v >> c);
        put32(out + 64, i, (uint32_t)(c >= 32 ? (int32_t)v >> 31 : (int32_t)v >> c));
        if (i < 4) {
            put32(out + 160, i, c >= 32 ? 0 : v << c);
            put32(out + 192, i, (uint32_t)(c >= 32 ? (int32_t)v >> 31 : (int32_t)v >> c));
        }
    }
    for (int i = 0; i < 4; i++) {
        uint64_t v = get64(a, i), c = get64(c64, i);
        put64(out + 96, i, c >= 64 ? 0 : v << c);
        put64(out + 128, i, c >= 64 ? 0 : v >> c);
        if (i < 2)
            put64(out + 176, i, c >= 64 ? 0 : v >> c);
    }
}

__attribute__((target("avx2"), noinline)) static void t_mask(const uint8_t *mem, const uint8_t *mask, const uint8_t *val,
                                                           uint8_t *out, uint8_t *store32, uint8_t *store64)
{
    __m256i ym = LOADU(mask), yv = LOADU(val);
    STOREU(out, _mm256_maskload_epi32((const int *)(void *)mem, ym));
    STOREU(out + 32, _mm256_maskload_epi64((const long long *)(void *)mem, ym));
    STOREU128(out + 64, _mm_maskload_epi32((const int *)(void *)mem, LOADU128(mask)));
    _mm256_maskstore_epi32((int *)(void *)store32, ym, yv);
    _mm256_maskstore_epi64((long long *)(void *)store64, ym, yv);
}

static void r_mask(const uint8_t *mem, const uint8_t *mask, const uint8_t *val, uint8_t *out, uint8_t *store32,
                   uint8_t *store64)
{
    memset(out, 0, 80);
    for (int i = 0; i < 8; i++) {
        int on = (get32(mask, i) >> 31) != 0;
        put32(out, i, on ? get32(mem, i) : 0);
        if (i < 4)
            put32(out + 64, i, on ? get32(mem, i) : 0);
        if (on)
            put32(store32, i, get32(val, i));
    }
    for (int i = 0; i < 4; i++) {
        int on = (get64(mask, i) >> 63) != 0;
        put64(out + 32, i, on ? get64(mem, i) : 0);
        if (on)
            put64(store64, i, get64(val, i));
    }
}

__attribute__((target("avx2"), noinline)) static void t_lanewise(const uint8_t *a, const uint8_t *b, uint8_t *out,
                                                               uint32_t *masks)
{
    __m256i ya = LOADU(a), yb = LOADU(b);
    STOREU(out, _mm256_shuffle_epi8(ya, yb));
    STOREU(out + 32, _mm256_alignr_epi8(ya, yb, 5));
    masks[0] = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(ya, yb));
    masks[1] = (uint32_t)_mm256_testz_si256(ya, yb);
    STOREU(out + 64, _mm256_add_epi32(ya, yb));
    STOREU(out + 96, _mm256_xor_si256(_mm256_or_si256(ya, yb), _mm256_and_si256(ya, yb)));
    STOREU(out + 128, _mm256_srli_epi32(ya, 7));
    STOREU(out + 160, _mm256_slli_epi64(ya, 13));
    STOREU(out + 192, _mm256_srli_si256(ya, 3));
    STOREU(out + 224, _mm256_shuffle_epi32(ya, 0x4e));
}

static void r_lanewise(const uint8_t *a, const uint8_t *b, uint8_t *out, uint32_t *masks)
{
    for (int lane = 0; lane < 2; lane++) {
        const uint8_t *la = a + 16 * lane, *lb = b + 16 * lane;
        uint8_t tmp[32];
        for (int i = 0; i < 16; i++) {
            uint8_t idx = lb[i];
            out[16 * lane + i] = (idx & 0x80) ? 0 : la[idx & 15];
        }
        memcpy(tmp, lb, 16);
        memcpy(tmp + 16, la, 16);
        for (int i = 0; i < 16; i++)
            out[32 + 16 * lane + i] = i + 5 < 32 ? tmp[i + 5] : 0;
        for (int i = 0; i < 16; i++)
            out[192 + 16 * lane + i] = i + 3 < 16 ? la[i + 3] : 0;
        for (int j = 0; j < 4; j++)
            memcpy(out + 224 + 16 * lane + 4 * j, la + 4 * ((0x4e >> (2 * j)) & 3), 4);
    }
    uint32_t m = 0;
    int any = 0;
    for (int i = 0; i < 32; i++) {
        if (a[i] == b[i])
            m |= 1u << i;
        if (a[i] & b[i])
            any = 1;
        out[96 + i] = (uint8_t)((a[i] | b[i]) ^ (a[i] & b[i]));
    }
    masks[0] = m;
    masks[1] = any ? 0 : 1;
    for (int i = 0; i < 8; i++) {
        put32(out + 64, i, get32(a, i) + get32(b, i));
        put32(out + 128, i, get32(a, i) >> 7);
    }
    for (int i = 0; i < 4; i++)
        put64(out + 160, i, get64(a, i) << 13);
}

int main(void)
{
    for (int iter = 0; iter < 200; iter++) {
        uint8_t a[64], b[64], got[400], want[400];
        uint32_t gm[2], wm[2];
        fill(a, sizeof a);
        fill(b, sizeof b);
        if (iter % 7 == 0)
            memcpy(b + 3, a + 3, 20);

        memset(got, 0, sizeof got);
        memset(want, 0, sizeof want);
        t_broadcast(a, got);
        r_broadcast(a, want);
        check("vpbroadcast", iter, got, want, 288);

        memset(got, 0, sizeof got);
        memset(want, 0, sizeof want);
        t_lanes(a, b, got);
        r_lanes(a, b, want);
        check("vinserti128/vextracti128/vperm2i128", iter, got, want, 352);

        memset(got, 0, sizeof got);
        memset(want, 0, sizeof want);
        t_blendd(a, b, got);
        r_blendd(a, b, want);
        check("vpblendd", iter, got, want, 96);

        memset(got, 0, sizeof got);
        memset(want, 0, sizeof want);
        t_perm(a, b, got);
        r_perm(a, b, want);
        check("vpermd/vpermps/vpermq/vpermpd", iter, got, want, 160);

        uint8_t c32[32], c64[32];
        for (int i = 0; i < 8; i++)
            put32(c32, i, (uint32_t)(next64() % 40));
        for (int i = 0; i < 4; i++)
            put64(c64, i, next64() % 70);
        memset(got, 0, sizeof got);
        t_shiftv(a, c32, c64, got);
        r_shiftv(a, c32, c64, want);
        check("vpsllv/vpsrlv/vpsrav", iter, got, want, 208);

        uint8_t s32g[32], s32w[32], s64g[32], s64w[32];
        fill(s32g, sizeof s32g);
        memcpy(s32w, s32g, sizeof s32w);
        fill(s64g, sizeof s64g);
        memcpy(s64w, s64g, sizeof s64w);
        memset(got, 0, sizeof got);
        t_mask(a, b, a + 32, got, s32g, s64g);
        r_mask(a, b, a + 32, want, s32w, s64w);
        check("vpmaskmov load", iter, got, want, 80);
        check("vpmaskmovd store", iter, s32g, s32w, 32);
        check("vpmaskmovq store", iter, s64g, s64w, 32);

        memset(got, 0, sizeof got);
        memset(want, 0, sizeof want);
        t_lanewise(a, b, got, gm);
        r_lanewise(a, b, want, wm);
        check("lane-wise AVX2 set", iter, got, want, 256);
        check("vpmovmskb/vptest", iter, (const uint8_t *)gm, (const uint8_t *)wm, sizeof gm);
    }
    if (!fails)
        printf("OK\n");
    return fails != 0;
}

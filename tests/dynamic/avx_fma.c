#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FMA_ASM(name, body) extern void name(const uint8_t *, const uint8_t *, const uint8_t *, uint8_t *); \
    __asm__(".text\n.p2align 4\n.globl _" #name "\n_" #name ":\n vmovdqu (%rdi), %ymm0\n vmovdqu (%rsi), %ymm1\n"      \
            " vmovdqu (%rdx), %ymm2\n " body "\n vmovdqu %ymm0, (%rcx)\n vzeroupper\n ret\n")

enum { ADDSUB, SUBADD, MADD, MSUB, NMADD, NMSUB };

#define FMA_LIST(P, S)                                                                     \
    P(vfmadd132, MADD, 0) P(vfmadd213, MADD, 1) P(vfmadd231, MADD, 2)                      \
    P(vfmsub132, MSUB, 0) P(vfmsub213, MSUB, 1) P(vfmsub231, MSUB, 2)                      \
    P(vfnmadd132, NMADD, 0) P(vfnmadd213, NMADD, 1) P(vfnmadd231, NMADD, 2)                \
    P(vfnmsub132, NMSUB, 0) P(vfnmsub213, NMSUB, 1) P(vfnmsub231, NMSUB, 2)                \
    P(vfmaddsub132, ADDSUB, 0) P(vfmaddsub213, ADDSUB, 1) P(vfmaddsub231, ADDSUB, 2)       \
    P(vfmsubadd132, SUBADD, 0) P(vfmsubadd213, SUBADD, 1) P(vfmsubadd231, SUBADD, 2)       \
    S(vfmadd132, MADD, 0) S(vfmadd213, MADD, 1) S(vfmadd231, MADD, 2)                      \
    S(vfmsub132, MSUB, 0) S(vfmsub213, MSUB, 1) S(vfmsub231, MSUB, 2)                      \
    S(vfnmadd132, NMADD, 0) S(vfnmadd213, NMADD, 1) S(vfnmadd231, NMADD, 2)                \
    S(vfnmsub132, NMSUB, 0) S(vfnmsub213, NMSUB, 1) S(vfnmsub231, NMSUB, 2)

#define GEN_P(mn, k, o)                                        \
    FMA_ASM(mn##ps_y, #mn "ps %ymm2, %ymm1, %ymm0");            \
    FMA_ASM(mn##ps_x, #mn "ps %xmm2, %xmm1, %xmm0");            \
    FMA_ASM(mn##ps_ym, #mn "ps (%rdx), %ymm1, %ymm0");          \
    FMA_ASM(mn##ps_xm, #mn "ps (%rdx), %xmm1, %xmm0");          \
    FMA_ASM(mn##pd_y, #mn "pd %ymm2, %ymm1, %ymm0");            \
    FMA_ASM(mn##pd_x, #mn "pd %xmm2, %xmm1, %xmm0");            \
    FMA_ASM(mn##pd_ym, #mn "pd (%rdx), %ymm1, %ymm0");          \
    FMA_ASM(mn##pd_xm, #mn "pd (%rdx), %xmm1, %xmm0");
#define GEN_S(mn, k, o)                                        \
    FMA_ASM(mn##ss_x, #mn "ss %xmm2, %xmm1, %xmm0");            \
    FMA_ASM(mn##ss_xm, #mn "ss (%rdx), %xmm1, %xmm0");          \
    FMA_ASM(mn##sd_x, #mn "sd %xmm2, %xmm1, %xmm0");            \
    FMA_ASM(mn##sd_xm, #mn "sd (%rdx), %xmm1, %xmm0");

FMA_LIST(GEN_P, GEN_S)

typedef void (*fma_fn)(const uint8_t *, const uint8_t *, const uint8_t *, uint8_t *);

struct fma_case {
    fma_fn f;
    const char *name;
    int kind, order, pd, width, scalar;
};

#define ROW_P(mn, k, o)                                                                                    \
    { mn##ps_y, #mn "ps ymm", k, o, 0, 32, 0 }, { mn##ps_x, #mn "ps xmm", k, o, 0, 16, 0 },                \
    { mn##ps_ym, #mn "ps ymm,mem", k, o, 0, 32, 0 }, { mn##ps_xm, #mn "ps xmm,mem", k, o, 0, 16, 0 },      \
    { mn##pd_y, #mn "pd ymm", k, o, 1, 32, 0 }, { mn##pd_x, #mn "pd xmm", k, o, 1, 16, 0 },                \
    { mn##pd_ym, #mn "pd ymm,mem", k, o, 1, 32, 0 }, { mn##pd_xm, #mn "pd xmm,mem", k, o, 1, 16, 0 },
#define ROW_S(mn, k, o)                                                                                    \
    { mn##ss_x, #mn "ss", k, o, 0, 16, 1 }, { mn##ss_xm, #mn "ss mem", k, o, 0, 16, 1 },                   \
    { mn##sd_x, #mn "sd", k, o, 1, 16, 1 }, { mn##sd_xm, #mn "sd mem", k, o, 1, 16, 1 },

static const struct fma_case cases[] = { FMA_LIST(ROW_P, ROW_S) };

static uint64_t rng = 0x853c49e6748fea9bull;
static int fails;

static uint64_t next64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void fill_fp(uint8_t *p, int pd)
{
    for (int i = 0; i < (pd ? 4 : 8); i++) {
        uint64_t r = next64();
        if (pd) {
            double v;
            uint64_t u;
            switch (r % 40) {
            case 0: v = 0.0; break;
            case 1: v = -0.0; break;
            case 2: v = INFINITY; break;
            case 3: v = -INFINITY; break;
            case 4: u = 0x7ff8000000000000ull | (next64() & 0x0007ffffffffffffull); memcpy(&v, &u, 8); break;
            case 5: u = 0xfff0000000000001ull | (next64() & 0x0007ffffffffffffull); memcpy(&v, &u, 8); break;
            case 6: v = ldexp((double)(int64_t)next64(), -1100); break;
            case 7: v = 1.0; break;
            default: v = ldexp((double)(int64_t)(next64() >> 11), (int)(next64() % 120) - 110); break;
            }
            memcpy(p + 8 * i, &v, 8);
        } else {
            float v;
            uint32_t u;
            switch (r % 40) {
            case 0: v = 0.0f; break;
            case 1: v = -0.0f; break;
            case 2: v = INFINITY; break;
            case 3: v = -INFINITY; break;
            case 4: u = 0x7fc00000u | ((uint32_t)next64() & 0x003fffffu); memcpy(&v, &u, 4); break;
            case 5: u = 0xff800001u | ((uint32_t)next64() & 0x003fffffu); memcpy(&v, &u, 4); break;
            case 6: v = ldexpf((float)(int32_t)next64(), -160); break;
            case 7: v = 1.0f; break;
            default: v = ldexpf((float)(int32_t)(next64() >> 40), (int)(next64() % 60) - 50); break;
            }
            memcpy(p + 4 * i, &v, 4);
        }
    }
}

static double ref_d(double a, double b, double c, int negmul, int negadd)
{
    if (isnan(a) || isnan(b) || isnan(c)) {
        double n = isnan(a) ? a : isnan(b) ? b : c;
        uint64_t u;
        memcpy(&u, &n, 8);
        u |= 1ull << 51;
        memcpy(&n, &u, 8);
        return n;
    }
    double r = fma(negmul ? -a : a, b, negadd ? -c : c);
    if (isnan(r)) {
        uint64_t u = 0xfff8000000000000ull;
        memcpy(&r, &u, 8);
    }
    return r;
}

static float ref_f(float a, float b, float c, int negmul, int negadd)
{
    if (isnan(a) || isnan(b) || isnan(c)) {
        float n = isnan(a) ? a : isnan(b) ? b : c;
        uint32_t u;
        memcpy(&u, &n, 4);
        u |= 1u << 22;
        memcpy(&n, &u, 4);
        return n;
    }
    float r = fmaf(negmul ? -a : a, b, negadd ? -c : c);
    if (isnan(r)) {
        uint32_t u = 0xffc00000u;
        memcpy(&r, &u, 4);
    }
    return r;
}

static void expect_fma(const struct fma_case *c, const uint8_t *o1, const uint8_t *o2, const uint8_t *o3, uint8_t *out)
{
    memset(out, 0, 32);
    memcpy(out, o1, 16);
    if (c->width == 32)
        memcpy(out + 16, o1 + 16, 16);
    int esz = c->pd ? 8 : 4;
    int n = c->scalar ? 1 : c->width / esz;
    const uint8_t *A = c->order == 0 ? o1 : o2;
    const uint8_t *B = c->order == 1 ? o1 : o3;
    const uint8_t *C = c->order == 0 ? o2 : c->order == 1 ? o3 : o1;
    int negmul = c->kind == NMADD || c->kind == NMSUB;
    for (int i = 0; i < n; i++) {
        int negadd = c->kind == ADDSUB ? !(i & 1) : c->kind == SUBADD ? (i & 1) : (c->kind == MSUB || c->kind == NMSUB);
        if (c->pd) {
            double x, y, z;
            memcpy(&x, A + 8 * i, 8);
            memcpy(&y, B + 8 * i, 8);
            memcpy(&z, C + 8 * i, 8);
            double v = ref_d(x, y, z, negmul, negadd);
            memcpy(out + 8 * i, &v, 8);
        } else {
            float x, y, z;
            memcpy(&x, A + 4 * i, 4);
            memcpy(&y, B + 4 * i, 4);
            memcpy(&z, C + 4 * i, 4);
            float v = ref_f(x, y, z, negmul, negadd);
            memcpy(out + 4 * i, &v, 4);
        }
    }
}

int main(void)
{
    int ncases = (int)(sizeof cases / sizeof cases[0]);
    for (int iter = 0; iter < 60; iter++) {
        for (int k = 0; k < ncases; k++) {
            const struct fma_case *c = &cases[k];
            uint8_t o1[32], o2[32], o3[32], got[32], want[32];
            fill_fp(o1, c->pd);
            fill_fp(o2, c->pd);
            fill_fp(o3, c->pd);
            c->f(o1, o2, o3, got);
            expect_fma(c, o1, o2, o3, want);
            if (memcmp(got, want, 32) != 0) {
                if (fails < 12) {
                    printf("BAD %s iteration %d\n got ", c->name, iter);
                    for (int i = 0; i < 32; i++)
                        printf("%02x", got[i]);
                    printf("\nwant ");
                    for (int i = 0; i < 32; i++)
                        printf("%02x", want[i]);
                    printf("\n");
                }
                fails++;
            }
        }
    }
    if (!fails)
        printf("OK\n");
    return fails != 0;
}

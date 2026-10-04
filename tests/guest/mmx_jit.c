#include "gsys.h"

typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long size_t;

static void put(const char *s)
{
    sys_write(1, s, g_strlen_local(s));
}
static void hex(uint64_t v, int digits)
{
    char b[16];
    for (int i = digits - 1; i >= 0; i--, v >>= 4)
        b[i] = "0123456789abcdef"[v & 15];
    sys_write(1, b, (g_u64)digits);
}
static void name(const char *s)
{
    put(s);
    for (g_u64 n = g_strlen_local(s); n < 10; n++)
        put(" ");
}

static const uint64_t vals[] = {
    0x0000000000000000ull, 0xffffffffffffffffull, 0x7fff80007fff8000ull, 0x0123456789abcdefull,
    0x8000000180007fffull, 0x00ff00ff7f7f8080ull, 0xfedcba9876543210ull, 0x0001000200030004ull,
};
#define NV (sizeof vals / sizeof vals[0])

#define BIN(name, insn)                                                            \
    static uint64_t name##_rr(uint64_t a, uint64_t b)                              \
    {                                                                              \
        uint64_t r;                                                                \
        __asm__ volatile("movq %1, %%mm0\n\tmovq %2, %%mm1\n\t" insn " %%mm1, %%mm0\n\t" \
                         "movq %%mm0, %0\n\temms"                                  \
                         : "=r"(r) : "r"(a), "r"(b) : "mm0", "mm1");              \
        return r;                                                                  \
    }                                                                              \
    static uint64_t name##_rm(uint64_t a, uint64_t b)                              \
    {                                                                              \
        uint64_t r;                                                                \
        volatile uint64_t m = b;                                                   \
        __asm__ volatile("movq %1, %%mm2\n\t" insn " %2, %%mm2\n\tmovq %%mm2, %0\n\temms" \
                         : "=m"(r) : "m"(a), "m"(m) : "mm2");                      \
        return r;                                                                  \
    }

BIN(paddb, "paddb") BIN(paddw, "paddw") BIN(paddd, "paddd") BIN(paddq, "paddq")
BIN(psubb, "psubb") BIN(psubw, "psubw") BIN(psubd, "psubd") BIN(psubq, "psubq")
BIN(paddsb, "paddsb") BIN(paddsw, "paddsw") BIN(paddusb, "paddusb") BIN(paddusw, "paddusw")
BIN(psubsb, "psubsb") BIN(psubsw, "psubsw") BIN(psubusb, "psubusb") BIN(psubusw, "psubusw")
BIN(pmullw, "pmullw") BIN(pmulhw, "pmulhw") BIN(pmulhuw, "pmulhuw")
BIN(pand, "pand") BIN(por, "por") BIN(pxor, "pxor") BIN(pandn, "pandn")
BIN(pcmpeqb, "pcmpeqb") BIN(pcmpeqw, "pcmpeqw") BIN(pcmpeqd, "pcmpeqd")
BIN(pcmpgtb, "pcmpgtb") BIN(pcmpgtw, "pcmpgtw") BIN(pcmpgtd, "pcmpgtd")
BIN(punpcklbw, "punpcklbw") BIN(punpcklwd, "punpcklwd") BIN(punpckldq, "punpckldq")
BIN(punpckhbw, "punpckhbw") BIN(punpckhwd, "punpckhwd") BIN(punpckhdq, "punpckhdq")
BIN(packuswb, "packuswb") BIN(packsswb, "packsswb") BIN(packssdw, "packssdw")
BIN(psadbw, "psadbw") BIN(pavgb, "pavgb") BIN(pavgw, "pavgw")
BIN(pminub, "pminub") BIN(pmaxub, "pmaxub") BIN(pminsw, "pminsw") BIN(pmaxsw, "pmaxsw")

typedef uint64_t (*binfn)(uint64_t, uint64_t);
static const struct { const char *name; binfn rr, rm; } bins[] = {
#define E(n) { #n, n##_rr, n##_rm },
    E(paddb) E(paddw) E(paddd) E(paddq) E(psubb) E(psubw) E(psubd) E(psubq)
    E(paddsb) E(paddsw) E(paddusb) E(paddusw) E(psubsb) E(psubsw) E(psubusb) E(psubusw)
    E(pmullw) E(pmulhw) E(pmulhuw) E(pand) E(por) E(pxor) E(pandn)
    E(pcmpeqb) E(pcmpeqw) E(pcmpeqd) E(pcmpgtb) E(pcmpgtw) E(pcmpgtd)
    E(punpcklbw) E(punpcklwd) E(punpckldq) E(punpckhbw) E(punpckhwd) E(punpckhdq)
    E(packuswb) E(packsswb) E(packssdw) E(psadbw) E(pavgb) E(pavgw)
    E(pminub) E(pmaxub) E(pminsw) E(pmaxsw)
#undef E
};

#define SHIFT(name, insn, n)                                                       \
    static uint64_t name##_##n(uint64_t a)                                         \
    {                                                                              \
        uint64_t r;                                                                \
        __asm__ volatile("movq %1, %%mm3\n\t" insn " $" #n ", %%mm3\n\tmovq %%mm3, %0\n\temms" \
                         : "=r"(r) : "r"(a) : "mm3");                              \
        return r;                                                                  \
    }
#define SHIFTS(name, insn) SHIFT(name, insn, 0) SHIFT(name, insn, 1) SHIFT(name, insn, 7) \
    SHIFT(name, insn, 15) SHIFT(name, insn, 16) SHIFT(name, insn, 31) SHIFT(name, insn, 32) SHIFT(name, insn, 64)
SHIFTS(psllw, "psllw") SHIFTS(pslld, "pslld") SHIFTS(psllq, "psllq")
SHIFTS(psrlw, "psrlw") SHIFTS(psrld, "psrld") SHIFTS(psrlq, "psrlq")
SHIFTS(psraw, "psraw") SHIFTS(psrad, "psrad")
typedef uint64_t (*unfn)(uint64_t);
static const struct { const char *name; unfn f[8]; } shifts[] = {
#define S(n) { #n, { n##_0, n##_1, n##_7, n##_15, n##_16, n##_31, n##_32, n##_64 } },
    S(psllw) S(pslld) S(psllq) S(psrlw) S(psrld) S(psrlq) S(psraw) S(psrad)
#undef S
};

static uint64_t pshufw_1b(uint64_t a)
{
    uint64_t r;
    __asm__ volatile("movq %1, %%mm4\n\tpshufw $0x1b, %%mm4, %%mm5\n\tmovq %%mm5, %0\n\temms"
                     : "=r"(r) : "r"(a) : "mm4", "mm5");
    return r;
}
static uint64_t pshufw_00m(uint64_t a)
{
    uint64_t r;
    volatile uint64_t m = a;
    __asm__ volatile("pshufw $0x00, %1, %%mm5\n\tmovq %%mm5, %0\n\temms" : "=r"(r) : "m"(m) : "mm5");
    return r;
}
static uint32_t pextrw_2(uint64_t a)
{
    uint32_t r;
    __asm__ volatile("movq %1, %%mm6\n\tpextrw $2, %%mm6, %0\n\temms" : "=r"(r) : "r"(a) : "mm6");
    return r;
}
static uint64_t pinsrw_1(uint64_t a, uint32_t w)
{
    uint64_t r;
    __asm__ volatile("movq %1, %%mm7\n\tpinsrw $1, %2, %%mm7\n\tmovq %%mm7, %0\n\temms"
                     : "=r"(r) : "r"(a), "r"(w) : "mm7");
    return r;
}
static uint64_t movd_roundtrip(uint64_t a)
{
    uint32_t lo = (uint32_t)a, back;
    uint64_t q;
    volatile uint32_t m = (uint32_t)(a >> 32);
    __asm__ volatile("movd %2, %%mm0\n\tmovq %%mm0, %0\n\tmovd %3, %%mm1\n\tmovd %%mm1, %1\n\temms"
                     : "=r"(q), "=r"(back) : "r"(lo), "m"(m) : "mm0", "mm1");
    return q ^ ((uint64_t)back << 32);
}
static uint64_t movq_mem(uint64_t a)
{
    volatile uint64_t src = a, dst = 0;
    __asm__ volatile("movq %1, %%mm2\n\tmovq %%mm2, %%mm3\n\tmovq %%mm3, %0\n\temms" : "=m"(dst) : "m"(src) : "mm2", "mm3");
    return dst;
}

int main(void)
{
    uint64_t h = 0;
    for (size_t k = 0; k < sizeof bins / sizeof bins[0]; k++) {
        uint64_t hk = 0;
        for (size_t i = 0; i < NV; i++)
            for (size_t j = 0; j < NV; j++) {
                uint64_t a = bins[k].rr(vals[i], vals[j]), b = bins[k].rm(vals[i], vals[j]);
                if (a != b) {
                    put(bins[k].name);
                    put(" reg/mem differ\n");
                }
                hk = hk * 0x100000001b3ull ^ a;
            }
        name(bins[k].name);
        put(" ");
        hex(hk, 16);
        put("\n");
        h ^= hk;
    }
    for (size_t k = 0; k < sizeof shifts / sizeof shifts[0]; k++) {
        name(shifts[k].name);
        for (int c = 0; c < 8; c++) {
            put(" ");
            hex(shifts[k].f[c](vals[c == 0 ? 3 : (size_t)c]), 16);
        }
        put("\n");
    }
    for (size_t i = 0; i < NV; i++) {
        put("misc ");
        hex(pshufw_1b(vals[i]), 16);
        put(" ");
        hex(pshufw_00m(vals[i]), 16);
        put(" ");
        hex(pextrw_2(vals[i]), 8);
        put(" ");
        hex(pinsrw_1(vals[i], 0xabcd1234u), 16);
        put(" ");
        hex(movd_roundtrip(vals[i]), 16);
        put(" ");
        hex(movq_mem(vals[i]), 16);
        put("\n");
    }
    put("all ");
    hex(h, 16);
    put("\n");
    return 0;
}

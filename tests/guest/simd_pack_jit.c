/* Every SSE form the JIT gained in one pass: the pack/multiply/horizontal integer
 * ops, pshuflw/hw, byte shifts, blends, movs[lh]dup, movmskp[sd], cmpp[sd],
 * cvt(t)ps2dq under all four MXCSR rounding modes, cvtdq2pd, shifts by an xmm or
 * m128 count, pinsrw/pextrw through memory, crc32, AES-NI, pclmulqdq and the
 * two- and three-operand imul with a memory source, with and without a flag reader.  Each form runs with an
 * xmm source, a memory source and source == destination over random rows plus
 * rows of integer and floating-point edge values; the per-form hashes are
 * verified under Rosetta.  The same source also builds as a low-based dynamic
 * main in the dynamic suite, which puts every memory operand in the low shadow
 * window and so exercises the guarded address path the Wine mode uses. */
#include "gsys.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define NV 48
#define NC 18
static u8 V[NV][16] __attribute__((aligned(16)));
static u8 C[NC][16] __attribute__((aligned(16)));
static u8 OUT[16] __attribute__((aligned(16)));
static u64 G[4];

static u64 seed = 0x9e3779b97f4a7c15ull;
static u64 rnd(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}

static void init(void)
{
    static const u16 w[8] = { 0x8000, 0x7fff, 0xffff, 0x0001, 0x8001, 0x4000, 0xc000, 0x0000 };
    static const u32 d[4] = { 0x7fffffff, 0x80000000, 0xffff8000, 0x00008000 };
    static const u32 f[24] = { 0x7fc00000, 0xffc00001, 0x7f800001, 0x7f800000, 0xff800000, 0x00000000,
                               0x80000000, 0x00000001, 0x4f000000, 0xcf000000, 0x4effffff, 0xcf000001,
                               0x3f000000, 0x3fc00000, 0x40200000, 0xbf000000, 0xbfc00000, 0xc0200000,
                               0x7f7fffff, 0x501502f9, 0xd01502f9, 0x47f12064, 0x3f800000, 0x3f800000 };
    static const u64 q[12] = { 0x7ff8000000000000ull, 0xfff8000000000001ull, 0x7ff0000000000001ull,
                               0x7ff0000000000000ull, 0xfff0000000000000ull, 0x0000000000000000ull,
                               0x8000000000000000ull, 0x0000000000000001ull, 0x3ff8000000000000ull,
                               0xc004000000000000ull, 0x41e0000000000000ull, 0x3ff0000000000000ull };
    static const u64 cnt[NC] = { 0, 1, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 255, 256,
                                 0x100000001ull, ~0ull, 0x8000000000000003ull };
    for (int v = 0; v < NV; v++)
        for (int k = 0; k < 16; k++)
            ((volatile u8 *)V[v])[k] = (u8)(rnd() >> 24);
    for (int k = 0; k < 8; k++) {
        ((volatile u16 *)V[0])[k] = 0x8000;
        ((volatile u16 *)V[1])[k] = w[k];
        ((volatile u16 *)V[2])[k] = w[7 - k];
        ((volatile u16 *)V[3])[k] = 0x7fff;
        ((volatile u16 *)V[4])[k] = (u16)(k & 1 ? 0x00ff : 0xff01);
    }
    for (int k = 0; k < 4; k++) {
        ((volatile u32 *)V[5])[k] = d[k];
        ((volatile u32 *)V[6])[k] = d[3 - k];
    }
    for (int r = 0; r < 6; r++)
        for (int k = 0; k < 4; k++)
            ((volatile u32 *)V[7 + r])[k] = f[(r * 4 + k * 5) % 24];
    for (int r = 0; r < 6; r++)
        for (int k = 0; k < 2; k++)
            ((volatile u64 *)V[13 + r])[k] = q[(r * 2 + k * 7) % 12];
    for (int c = 0; c < NC; c++) {
        ((volatile u64 *)C[c])[0] = cnt[c];
        ((volatile u64 *)C[c])[1] = rnd();
    }
}

static u64 fold(u64 h)
{
    for (int k = 0; k < 16; k++)
        h = (h ^ OUT[k]) * 0x100000001b3ull;
    return h;
}
static u64 foldg(u64 h, u64 v)
{
    for (int k = 0; k < 8; k++)
        h = (h ^ ((v >> (8 * k)) & 0xff)) * 0x100000001b3ull;
    return h;
}

#define TEST2(fn, ins) \
    static u64 fn(void) \
    { \
        u64 h = 0xcbf29ce484222325ull; \
        for (int rep = 0; rep < 8; rep++) \
            for (int i = 0; i < NV; i++) { \
                const u8 *a = V[i], *b = V[(i * 7 + 3 + rep) % NV]; \
                __asm__ volatile("movdqa (%0), %%xmm0\n\tmovdqa (%1), %%xmm1\n\t" ins " %%xmm1, %%xmm0\n\tmovdqa %%xmm0, (%2)" \
                                 :: "r"(a), "r"(b), "r"(OUT) : "xmm0", "xmm1", "memory"); \
                h = fold(h); \
                __asm__ volatile("movdqa (%0), %%xmm2\n\t" ins " (%1), %%xmm2\n\tmovdqa %%xmm2, (%2)" \
                                 :: "r"(a), "r"(b), "r"(OUT) : "xmm2", "memory"); \
                h = fold(h); \
                __asm__ volatile("movdqa (%0), %%xmm5\n\t" ins " %%xmm5, %%xmm5\n\tmovdqa %%xmm5, (%1)" \
                                 :: "r"(a), "r"(OUT) : "xmm5", "memory"); \
                h = fold(h); \
            } \
        return h; \
    }

#define TESTSH(fn, ins) \
    static u64 fn(void) \
    { \
        u64 h = 0xcbf29ce484222325ull; \
        for (int i = 0; i < NV; i++) \
            for (int c = 0; c < NC; c++) { \
                __asm__ volatile("movdqa (%0), %%xmm0\n\tmovdqa (%1), %%xmm1\n\t" ins " %%xmm1, %%xmm0\n\tmovdqa %%xmm0, (%2)" \
                                 :: "r"(V[i]), "r"(C[c]), "r"(OUT) : "xmm0", "xmm1", "memory"); \
                h = fold(h); \
                __asm__ volatile("movdqa (%0), %%xmm3\n\t" ins " (%1), %%xmm3\n\tmovdqa %%xmm3, (%2)" \
                                 :: "r"(V[i]), "r"(C[c]), "r"(OUT) : "xmm3", "memory"); \
                h = fold(h); \
            } \
        return h; \
    }

#define TESTIMM(fn, ins) \
    static u64 fn(void) \
    { \
        u64 h = 0xcbf29ce484222325ull; \
        for (int i = 0; i < NV; i++) { \
            __asm__ volatile("movdqa (%0), %%xmm4\n\t" ins ", %%xmm4\n\tmovdqa %%xmm4, (%1)" \
                             :: "r"(V[i]), "r"(OUT) : "xmm4", "memory"); \
            h = fold(h); \
        } \
        return h; \
    }

TEST2(t_pmaddwd, "pmaddwd")
TEST2(t_pmulhrsw, "pmulhrsw")
TEST2(t_packssdw, "packssdw")
TEST2(t_packuswb, "packuswb")
TEST2(t_pmuludq, "pmuludq")
TEST2(t_pmulhw, "pmulhw")
TEST2(t_pmulhuw, "pmulhuw")
TEST2(t_packsswb, "packsswb")
TEST2(t_packusdw, "packusdw")
TEST2(t_pmuldq, "pmuldq")
TEST2(t_psadbw, "psadbw")
TEST2(t_pmaddubsw, "pmaddubsw")
TEST2(t_pabsb, "pabsb")
TEST2(t_pabsw, "pabsw")
TEST2(t_pabsd, "pabsd")
TEST2(t_psignb, "psignb")
TEST2(t_psignw, "psignw")
TEST2(t_psignd, "psignd")
TEST2(t_phaddw, "phaddw")
TEST2(t_phaddd, "phaddd")
TEST2(t_phsubw, "phsubw")
TEST2(t_phsubd, "phsubd")
TEST2(t_phaddsw, "phaddsw")
TEST2(t_phsubsw, "phsubsw")
TEST2(t_pblendw00, "pblendw $0x00,")
TEST2(t_pblendw5a, "pblendw $0x5a,")
TEST2(t_pblendw81, "pblendw $0x81,")
TEST2(t_pblendwff, "pblendw $0xff,")
TEST2(t_palignr0, "palignr $0,")
TEST2(t_palignr1, "palignr $1,")
TEST2(t_palignr4, "palignr $4,")
TEST2(t_palignr15, "palignr $15,")
TEST2(t_palignr16, "palignr $16,")
TEST2(t_palignr21, "palignr $21,")
TEST2(t_palignr31, "palignr $31,")
TEST2(t_palignr32, "palignr $32,")
TEST2(t_pshuflw1b, "pshuflw $0x1b,")
TEST2(t_pshuflwe4, "pshuflw $0xe4,")
TEST2(t_pshuflw00, "pshuflw $0x00,")
TEST2(t_pshufhw1b, "pshufhw $0x1b,")
TEST2(t_pshufhw4e, "pshufhw $0x4e,")
TEST2(t_pshufhwff, "pshufhw $0xff,")
TEST2(t_blendps5, "blendps $0x5,")
TEST2(t_blendpse, "blendps $0xe,")
TEST2(t_blendpd1, "blendpd $0x1,")
TEST2(t_blendpd2, "blendpd $0x2,")
TEST2(t_movshdup, "movshdup")
TEST2(t_movsldup, "movsldup")
TEST2(t_cmpps0, "cmpps $0,")
TEST2(t_cmpps1, "cmpps $1,")
TEST2(t_cmpps2, "cmpps $2,")
TEST2(t_cmpps3, "cmpps $3,")
TEST2(t_cmpps4, "cmpps $4,")
TEST2(t_cmpps5, "cmpps $5,")
TEST2(t_cmpps6, "cmpps $6,")
TEST2(t_cmpps7, "cmpps $7,")
TEST2(t_cmppd0, "cmppd $0,")
TEST2(t_cmppd1, "cmppd $1,")
TEST2(t_cmppd2, "cmppd $2,")
TEST2(t_cmppd3, "cmppd $3,")
TEST2(t_cmppd4, "cmppd $4,")
TEST2(t_cmppd5, "cmppd $5,")
TEST2(t_cmppd6, "cmppd $6,")
TEST2(t_cmppd7, "cmppd $7,")
TEST2(t_cvttps2dq, "cvttps2dq")
TEST2(t_cvtps2dq, "cvtps2dq")
TEST2(t_cvtdq2pd, "cvtdq2pd")
TEST2(t_aesenc, "aesenc")
TEST2(t_aesenclast, "aesenclast")
TEST2(t_aesdec, "aesdec")
TEST2(t_aesdeclast, "aesdeclast")
TEST2(t_aesimc, "aesimc")
TEST2(t_aeskga01, "aeskeygenassist $0x01,")
TEST2(t_aeskga1b, "aeskeygenassist $0x1b,")
TEST2(t_aeskga80, "aeskeygenassist $0x80,")
TEST2(t_pclmul00, "pclmulqdq $0x00,")
TEST2(t_pclmul01, "pclmulqdq $0x01,")
TEST2(t_pclmul10, "pclmulqdq $0x10,")
TEST2(t_pclmul11, "pclmulqdq $0x11,")
TESTSH(t_psllw, "psllw")
TESTSH(t_pslld, "pslld")
TESTSH(t_psllq, "psllq")
TESTSH(t_psrlw, "psrlw")
TESTSH(t_psrld, "psrld")
TESTSH(t_psrlq, "psrlq")
TESTSH(t_psraw, "psraw")
TESTSH(t_psrad, "psrad")
TESTIMM(t_pslldq0, "pslldq $0")
TESTIMM(t_pslldq1, "pslldq $1")
TESTIMM(t_pslldq7, "pslldq $7")
TESTIMM(t_pslldq15, "pslldq $15")
TESTIMM(t_pslldq16, "pslldq $16")
TESTIMM(t_psrldq1, "psrldq $1")
TESTIMM(t_psrldq8, "psrldq $8")
TESTIMM(t_psrldq15, "psrldq $15")
TESTIMM(t_psrldq200, "psrldq $200")

static u64 t_cvtps2dq_rc(void)
{
    u64 h = 0xcbf29ce484222325ull;
    u32 saved, rc;
    __asm__ volatile("stmxcsr %0" : "=m"(saved));
    for (u32 mode = 0; mode < 4; mode++) {
        rc = (saved & ~0x6000u) | (mode << 13);
        __asm__ volatile("ldmxcsr %0" :: "m"(rc));
        for (int i = 0; i < NV; i++) {
            __asm__ volatile("movdqa (%0), %%xmm6\n\tcvtps2dq %%xmm6, %%xmm7\n\tmovdqa %%xmm7, (%1)"
                             :: "r"(V[i]), "r"(OUT) : "xmm6", "xmm7", "memory");
            h = fold(h);
            __asm__ volatile("cvtps2dq (%0), %%xmm7\n\tmovdqa %%xmm7, (%1)"
                             :: "r"(V[i]), "r"(OUT) : "xmm7", "memory");
            h = fold(h);
        }
    }
    __asm__ volatile("ldmxcsr %0" :: "m"(saved));
    return h;
}

static u64 t_movmsk(void)
{
    u64 h = 0xcbf29ce484222325ull;
    for (int i = 0; i < NV; i++) {
        u64 a, b;
        __asm__ volatile("movdqa (%2), %%xmm3\n\tmovmskps %%xmm3, %%eax\n\tmovmskpd %%xmm3, %%ecx"
                         : "=&a"(a), "=&c"(b) : "r"(V[i]) : "xmm3");
        h = foldg(foldg(h, a), b);
    }
    return h;
}

static u64 t_crc32(void)
{
    u64 h = 0xcbf29ce484222325ull;
    for (int i = 0; i < NV; i++) {
        const u64 *p = (const u64 *)V[i];
        u64 acc = p[1], x = p[0];
        __asm__ volatile("crc32b %%cl, %%eax\n\t" : "+a"(acc) : "c"(x));
        h = foldg(h, acc);
        __asm__ volatile("crc32w %%cx, %%eax\n\t" : "+a"(acc) : "c"(x));
        h = foldg(h, acc);
        __asm__ volatile("crc32l %%ecx, %%eax\n\t" : "+a"(acc) : "c"(x));
        h = foldg(h, acc);
        __asm__ volatile("crc32q %%rcx, %%rax\n\t" : "+a"(acc) : "c"(x));
        h = foldg(h, acc);
        acc = ~acc;
        __asm__ volatile("crc32b (%1), %%eax\n\tcrc32w 2(%1), %%eax\n\tcrc32l 4(%1), %%eax\n\tcrc32q 8(%1), %%rax"
                         : "+a"(acc) : "r"(p) : "memory");
        h = foldg(h, acc);
        __asm__ volatile("crc32b (%1), %%rax" : "+a"(acc) : "r"(p) : "memory");
        h = foldg(h, acc);
    }
    return h;
}

static u64 t_pinsr_pextr(void)
{
    u64 h = 0xcbf29ce484222325ull;
    for (int i = 0; i < NV; i++) {
        const u8 *a = V[i], *b = V[(i * 5 + 1) % NV];
        __asm__ volatile("movdqa (%0), %%xmm8\n\tpinsrw $3, 6(%1), %%xmm8\n\tpinsrw $7, (%1), %%xmm8\n\t"
                         "pinsrb $9, 13(%1), %%xmm8\n\tpinsrd $2, 4(%1), %%xmm8\n\tmovdqa %%xmm8, (%2)"
                         :: "r"(a), "r"(b), "r"(OUT) : "xmm8", "memory");
        h = fold(h);
        __asm__ volatile("movdqa (%0), %%xmm9\n\tpextrw $6, %%xmm9, (%1)\n\tpextrb $11, %%xmm9, 5(%1)\n\t"
                         "pextrd $1, %%xmm9, 8(%1)\n\tpextrq $1, %%xmm9, 16(%1)"
                         :: "r"(a), "r"(G) : "xmm9", "memory");
        h = foldg(foldg(foldg(h, G[0]), G[1]), G[2]);
    }
    return h;
}

static u64 t_imul_mem(void)
{
    u64 h = 0xcbf29ce484222325ull;
    for (int i = 0; i < NV; i++) {
        const u64 *p = (const u64 *)V[i];
        u64 a = p[1] | 1, r, f;
        __asm__ volatile("imulq (%1), %0" : "+r"(a) : "r"(p) : "cc", "memory");
        h = foldg(h, a);
        r = p[1];
        __asm__ volatile("imull 4(%1), %k0" : "+r"(r) : "r"(p) : "cc", "memory");
        h = foldg(h, r);
        __asm__ volatile("imulq $-7777, 8(%1), %0" : "=r"(r) : "r"(p) : "cc", "memory");
        h = foldg(h, r);
        __asm__ volatile("imull $37, (%1), %k0" : "=r"(r) : "r"(p) : "cc", "memory");
        h = foldg(h, r);
        r = p[1];
        __asm__ volatile("imulq (%2), %0\n\tsetc %b1\n\tseto %h1" : "+r"(r), "=Q"(f) : "r"(p) : "cc", "memory");
        h = foldg(foldg(h, r), f & 0xffff);
        r = p[0];
        __asm__ volatile("imull 12(%2), %k0\n\tsetc %b1\n\tseto %h1" : "+r"(r), "=Q"(f) : "r"(p) : "cc", "memory");
        h = foldg(foldg(h, r), f & 0xffff);
        __asm__ volatile("imulq $0x7fffffff, (%2), %0\n\tsetc %b1\n\tseto %h1" : "=r"(r), "=Q"(f) : "r"(p) : "cc", "memory");
        h = foldg(foldg(h, r), f & 0xffff);
    }
    return h;
}

static void line(const char *name, u64 h)
{
    g_puts(name);
    g_puts(" ");
    g_puthex64(h);
}

int main(void)
{
    init();
    line("pmaddwd", t_pmaddwd());
    line("pmulhrsw", t_pmulhrsw());
    line("packssdw", t_packssdw());
    line("packuswb", t_packuswb());
    line("pmuludq", t_pmuludq());
    line("pmulhw", t_pmulhw());
    line("pmulhuw", t_pmulhuw());
    line("packsswb", t_packsswb());
    line("packusdw", t_packusdw());
    line("pmuldq", t_pmuldq());
    line("psadbw", t_psadbw());
    line("pmaddubsw", t_pmaddubsw());
    line("pabsb", t_pabsb());
    line("pabsw", t_pabsw());
    line("pabsd", t_pabsd());
    line("psignb", t_psignb());
    line("psignw", t_psignw());
    line("psignd", t_psignd());
    line("phaddw", t_phaddw());
    line("phaddd", t_phaddd());
    line("phsubw", t_phsubw());
    line("phsubd", t_phsubd());
    line("phaddsw", t_phaddsw());
    line("phsubsw", t_phsubsw());
    line("pblendw00", t_pblendw00());
    line("pblendw5a", t_pblendw5a());
    line("pblendw81", t_pblendw81());
    line("pblendwff", t_pblendwff());
    line("palignr0", t_palignr0());
    line("palignr1", t_palignr1());
    line("palignr4", t_palignr4());
    line("palignr15", t_palignr15());
    line("palignr16", t_palignr16());
    line("palignr21", t_palignr21());
    line("palignr31", t_palignr31());
    line("palignr32", t_palignr32());
    line("pshuflw1b", t_pshuflw1b());
    line("pshuflwe4", t_pshuflwe4());
    line("pshuflw00", t_pshuflw00());
    line("pshufhw1b", t_pshufhw1b());
    line("pshufhw4e", t_pshufhw4e());
    line("pshufhwff", t_pshufhwff());
    line("blendps5", t_blendps5());
    line("blendpse", t_blendpse());
    line("blendpd1", t_blendpd1());
    line("blendpd2", t_blendpd2());
    line("movshdup", t_movshdup());
    line("movsldup", t_movsldup());
    line("cmpps0", t_cmpps0());
    line("cmpps1", t_cmpps1());
    line("cmpps2", t_cmpps2());
    line("cmpps3", t_cmpps3());
    line("cmpps4", t_cmpps4());
    line("cmpps5", t_cmpps5());
    line("cmpps6", t_cmpps6());
    line("cmpps7", t_cmpps7());
    line("cmppd0", t_cmppd0());
    line("cmppd1", t_cmppd1());
    line("cmppd2", t_cmppd2());
    line("cmppd3", t_cmppd3());
    line("cmppd4", t_cmppd4());
    line("cmppd5", t_cmppd5());
    line("cmppd6", t_cmppd6());
    line("cmppd7", t_cmppd7());
    line("cvttps2dq", t_cvttps2dq());
    line("cvtps2dq", t_cvtps2dq());
    line("cvtps2dq_rc", t_cvtps2dq_rc());
    line("cvtdq2pd", t_cvtdq2pd());
    line("aesenc", t_aesenc());
    line("aesenclast", t_aesenclast());
    line("aesdec", t_aesdec());
    line("aesdeclast", t_aesdeclast());
    line("aesimc", t_aesimc());
    line("aeskga01", t_aeskga01());
    line("aeskga1b", t_aeskga1b());
    line("aeskga80", t_aeskga80());
    line("pclmul00", t_pclmul00());
    line("pclmul01", t_pclmul01());
    line("pclmul10", t_pclmul10());
    line("pclmul11", t_pclmul11());
    line("psllw", t_psllw());
    line("pslld", t_pslld());
    line("psllq", t_psllq());
    line("psrlw", t_psrlw());
    line("psrld", t_psrld());
    line("psrlq", t_psrlq());
    line("psraw", t_psraw());
    line("psrad", t_psrad());
    line("pslldq0", t_pslldq0());
    line("pslldq1", t_pslldq1());
    line("pslldq7", t_pslldq7());
    line("pslldq15", t_pslldq15());
    line("pslldq16", t_pslldq16());
    line("psrldq1", t_psrldq1());
    line("psrldq8", t_psrldq8());
    line("psrldq15", t_psrldq15());
    line("psrldq200", t_psrldq200());
    line("movmsk", t_movmsk());
    line("crc32", t_crc32());
    line("pinsr_pextr", t_pinsr_pextr());
    line("imul_mem", t_imul_mem());
    return 0;
}

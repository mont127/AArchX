/* Audit gate for rsp as the destination (and odd source) of ordinary ALU/misc
 * ops.  Under the pointer-rsp pin classes every one of these must convert at
 * the boundary; an unguarded fast path writing the pin raw shows up here as a
 * value off by the guest base.  Goldens come from the native binary. */
#include "gsys.h"

static g_u64 acc;
static void mix(g_u64 v) { acc = (acc ^ v) * 0x9E3779B97F4A7C15ULL; acc ^= acc >> 29; }

#define RSP1(seq) do {                                                       \
    g_u64 sv_, out_;                                                         \
    __asm__ volatile(                                                        \
        "movq %%rsp, %[sv]\n\t"                                              \
        "movq %[in], %%rsp\n\t"                                              \
        seq "\n\t"                                                           \
        "movq %%rsp, %[out]\n\t"                                             \
        "movq %[sv], %%rsp"                                                  \
        : [sv]"=&r"(sv_), [out]"=&r"(out_)                                   \
        : [in]"r"(in), [a]"r"(a), [b]"r"(bb)                                 \
        : "cc");                                                             \
    g_puts(seq); g_puts(" = "); mix(out_); g_putu64(acc);                    \
} while (0)

/* same, but also captures CF/ZF/SF right after the op */
#define RSPF(seq) do {                                                       \
    g_u64 sv_, out_; g_u64 f1_ = 0, f2_ = 0, f3_ = 0;                        \
    __asm__ volatile(                                                        \
        "movq %%rsp, %[sv]\n\t"                                              \
        "movq %[in], %%rsp\n\t"                                              \
        seq "\n\t"                                                           \
        "setc %b[f1]\n\t"                                                    \
        "setz %b[f2]\n\t"                                                    \
        "sets %b[f3]\n\t"                                                    \
        "movq %%rsp, %[out]\n\t"                                             \
        "movq %[sv], %%rsp"                                                  \
        : [sv]"=&r"(sv_), [out]"=&r"(out_),                                  \
          [f1]"+r"(f1_), [f2]"+r"(f2_), [f3]"+r"(f3_)                        \
        : [in]"r"(in), [a]"r"(a), [b]"r"(bb)                                 \
        : "cc");                                                             \
    g_puts(seq); g_puts(" = "); mix(out_); mix(f1_ | (f2_ << 1) | (f3_ << 2)); g_putu64(acc); \
} while (0)

/* CF/OF only: imul leaves ZF/SF/PF undefined */
#define RSPC(seq) do {                                                       \
    g_u64 sv_, out_; g_u64 f1_ = 0, f2_ = 0;                                 \
    __asm__ volatile(                                                        \
        "movq %%rsp, %[sv]\n\t"                                              \
        "movq %[in], %%rsp\n\t"                                              \
        seq "\n\t"                                                           \
        "setc %b[f1]\n\t"                                                    \
        "seto %b[f2]\n\t"                                                    \
        "movq %%rsp, %[out]\n\t"                                             \
        "movq %[sv], %%rsp"                                                  \
        : [sv]"=&r"(sv_), [out]"=&r"(out_), [f1]"+r"(f1_), [f2]"+r"(f2_)     \
        : [in]"r"(in), [a]"r"(a), [b]"r"(bb)                                 \
        : "cc");                                                             \
    g_puts(seq); g_puts(" = "); mix(out_); mix(f1_ | (f2_ << 1)); g_putu64(acc); \
} while (0)

/* ZF only: bsf/bsr leave the rest undefined */
#define RSPZ(seq) do {                                                       \
    g_u64 sv_, out_; g_u64 f1_ = 0;                                          \
    __asm__ volatile(                                                        \
        "movq %%rsp, %[sv]\n\t"                                              \
        "movq %[in], %%rsp\n\t"                                              \
        seq "\n\t"                                                           \
        "setz %b[f1]\n\t"                                                    \
        "movq %%rsp, %[out]\n\t"                                             \
        "movq %[sv], %%rsp"                                                  \
        : [sv]"=&r"(sv_), [out]"=&r"(out_), [f1]"+r"(f1_)                    \
        : [in]"r"(in), [a]"r"(a), [b]"r"(bb)                                 \
        : "cc");                                                             \
    g_puts(seq); g_puts(" = "); mix(out_); mix(f1_); g_putu64(acc);           \
} while (0)

int main(void)
{
    static const g_u64 ins[] = { 0x00007ffe12345678ULL, 0xfff0000000000320ULL,
                                 0x0000000000000000ULL, 0x8000000000000001ULL };
    static const g_u64 as[]  = { 0x0000000123400abcULL, 0xffffffffffffff01ULL,
                                 0x0000000000000040ULL, 0x7fffffffffffffffULL };
    for (unsigned i = 0; i < 4; i++) for (unsigned j = 0; j < 4; j++) {
        g_u64 in = ins[i], a = as[j], bb = as[3 - j] ^ (g_u64)i;

        RSP1("movq %[a], %%rsp");
        RSP1("movl %k[a], %%esp");
        RSP1("movzwl %w[a], %%esp");
        RSPF("addq %[a], %%rsp");
        RSPF("subq %[a], %%rsp");
        RSPF("addq $0x28, %%rsp");
        RSPF("subq $8, %%rsp");
        RSPF("addl $0x30, %%esp");
        RSPF("andq $-64, %%rsp");
        RSPF("orq $0xf01, %%rsp");
        RSPF("xorq %[a], %%rsp");
        RSPF("andq %[a], %%rsp");
        RSPF("orq %[a], %%rsp");
        RSPC("imulq %[a], %%rsp");
        RSP1("imulq $3, %[a], %%rsp");
        RSPF("shlq $4, %%rsp");
        RSPF("shrq $5, %%rsp");
        RSPF("sarq $3, %%rsp");
        RSP1("rolq $9, %%rsp");
        RSP1("rorq $7, %%rsp");
        RSPF("negq %%rsp");
        RSP1("notq %%rsp");
        RSPF("incq %%rsp");
        RSPF("decq %%rsp");
        RSP1("cmpq %[a], %[b]\n\tcmovaq %[a], %%rsp");
        RSP1("cmpq %[a], %[b]\n\tcmovbq %[a], %%rsp");
        RSPZ("bsfq %[a], %%rsp");
        RSPZ("bsrq %[a], %%rsp");
        RSPF("popcntq %[a], %%rsp");
        RSPF("stc\n\tadcq %[a], %%rsp");
        RSPF("stc\n\tsbbq %[a], %%rsp");
        RSPF("testq %[a], %%rsp");
        RSPF("cmpq %[a], %%rsp");
        RSP1("leaq -8(%[a]), %%rsp");
        RSP1("leaq 16(%[a],%[b],4), %%rsp");
        RSP1("leaq (%%rsp,%[b],2), %%rsp");
        RSP1("leaq 0x30(%%rsp), %%rsp");

        /* xchg both directions observable */
        {
            g_u64 sv_, out_, aa = a;
            __asm__ volatile(
                "movq %%rsp, %[sv]\n\t"
                "movq %[in], %%rsp\n\t"
                "xchgq %[aa], %%rsp\n\t"
                "movq %%rsp, %[out]\n\t"
                "movq %[sv], %%rsp"
                : [sv]"=&r"(sv_), [out]"=&r"(out_), [aa]"+r"(aa)
                : [in]"r"(in) : "cc");
            mix(out_); g_putu64(acc); mix(aa); g_putu64(acc);
        }
        /* cvttsd2si with rsp dest */
        {
            g_u64 sv_, out_;
            double d = (double)(g_i64)a * 1.5;
            __asm__ volatile(
                "movq %%rsp, %[sv]\n\t"
                "movq %[in], %%rsp\n\t"
                "cvttsd2si %[d], %%rsp\n\t"
                "movq %%rsp, %[out]\n\t"
                "movq %[sv], %%rsp"
                : [sv]"=&r"(sv_), [out]"=&r"(out_)
                : [in]"r"(in), [d]"x"(d) : "cc");
            g_puts("cvt:"); g_putu64(out_); mix(out_); g_putu64(acc);
        }
        /* rsp as a plain source */
        {
            g_u64 sv_, o1 = 0, o2 = 0;
            __asm__ volatile(
                "movq %%rsp, %[sv]\n\t"
                "movq %[in], %%rsp\n\t"
                "imulq %%rsp, %[o1]\n\t"
                "leaq 5(%%rsp), %[o2]\n\t"
                "addq %%rsp, %[o2]\n\t"
                "movq %[sv], %%rsp"
                : [sv]"=&r"(sv_), [o1]"+r"(o1), [o2]"+r"(o2)
                : [in]"r"(in) : "cc");
            mix(o1); g_putu64(acc); mix(o2); g_putu64(acc);
        }
    }
    g_putu64(acc);
    g_puts("done\n");
    return 0;
}

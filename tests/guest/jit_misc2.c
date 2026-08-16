/* leave, shld/shrd, bts/btr/btc (reg forms), pinsr/pextr, pmovsx/zx, roundss/sd/ps/pd. */
#include "gsys.h"
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
__attribute__((noinline)) static g_u64 uses_leave(g_u64 a, g_u64 b) {
    volatile g_u64 buf[4]; buf[0] = a; buf[1] = b; buf[2] = a ^ b; buf[3] = a + b;
    g_u64 r; __asm__ volatile("pushq %%rbp\n\tmovq %%rsp, %%rbp\n\tsubq $32, %%rsp\n\tmovq %1, (%%rsp)\n\tmovq (%%rsp), %0\n\tleave" : "=r"(r) : "r"(buf[2] + buf[3]) : "memory"); return r;
}
int main(void){
    static const g_u64 vals[5] = { 0x0123456789abcdefULL, 0xffffffffffffffffULL, 0x8000000000000001ULL, 0x00000000deadbeefULL, 0 };
    g_u64 r, f, c;
    for (int i = 0; i < 5; i++) {
        g_u64 v = vals[i], w = vals[(i + 2) % 5];
        put("leave ", uses_leave(v, w));
        r = v; __asm__ volatile("shldq $13, %1, %0" : "+r"(r) : "r"(w) : "cc"); put("shld64 ", r);
        r = v; __asm__ volatile("shrdq $13, %1, %0" : "+r"(r) : "r"(w) : "cc"); put("shrd64 ", r);
        r = v; __asm__ volatile("shldl $5, %k1, %k0" : "+r"(r) : "r"(w) : "cc"); put("shld32 ", r);
        r = v; __asm__ volatile("shrdl $31, %k1, %k0" : "+r"(r) : "r"(w) : "cc"); put("shrd32 ", r);
        r = v; __asm__ volatile("btsq $63, %0\n\tsetc %b1" : "+r"(r), "=r"(c) :: "cc"); put("bts63 r ", r); put("bts63 c ", c & 0xff);
        r = v; __asm__ volatile("btrq $0, %0\n\tsetc %b1" : "+r"(r), "=r"(c) :: "cc"); put("btr0 r ", r); put("btr0 c ", c & 0xff);
        r = v; __asm__ volatile("btcl $17, %k0\n\tsetc %b1" : "+r"(r), "=r"(c) :: "cc"); put("btc17 r ", r); put("btc17 c ", c & 0xff);
        r = v; __asm__ volatile("btsl %k2, %k0\n\tsetc %b1" : "+r"(r), "=r"(c) : "r"((g_u64)(i * 9)) : "cc"); put("bts rr r ", r); put("bts rr c ", c & 0xff);
        r = v; __asm__ volatile("btrw %w2, %w0\n\tsetc %b1" : "+r"(r), "=r"(c) : "r"((g_u64)(i * 5 + 3)) : "cc"); put("btr16 rr r ", r); put("btr16 rr c ", c & 0xff);
        r = v; __asm__ volatile("btcq $40, %0\n\tjc 1f\n\tmovq $0x77, %0\n\t1:" : "+r"(r) :: "cc"); put("btc jc ", r);
        r = v; __asm__ volatile("xorl %%eax, %%eax\n\tbtsq $5, %0\n\tpushfq\n\tpopq %1" : "+r"(r), "=r"(f) :: "cc", "rax"); put("bts flags ", f & 0x8d5);
        /* pinsr / pextr */
        g_u64 lo, hi;
        __asm__ volatile("pxor %%xmm1, %%xmm1\n\tpinsrq $1, %2, %%xmm1\n\tpinsrd $1, %k2, %%xmm1\n\tpinsrw $1, %k2, %%xmm1\n\tpinsrb $3, %k2, %%xmm1\n\tmovq %%xmm1, %0\n\tpextrq $1, %%xmm1, %1"
                         : "=r"(lo), "=r"(hi) : "r"(v) : "xmm1"); put("pinsr lo ", lo); put("pinsr hi ", hi);
        __asm__ volatile("movq %2, %%xmm2\n\tpunpcklqdq %%xmm2, %%xmm2\n\tpextrb $9, %%xmm2, %k0\n\tpextrw $2, %%xmm2, %k1" : "=r"(lo), "=r"(hi) : "r"(v) : "xmm2"); put("pextrb ", lo); put("pextrw ", hi);
        __asm__ volatile("movq %1, %%xmm2\n\tpextrd $1, %%xmm2, %k0" : "=r"(lo) : "r"(v) : "xmm2"); put("pextrd ", lo);
        volatile g_u32 m32 = 0; volatile unsigned short m16 = 0;
        __asm__ volatile("movq %2, %%xmm2\n\tpextrd $1, %%xmm2, %0\n\tpextrw $0, %%xmm2, %1" : "=m"(m32), "=m"(m16) : "r"(v) : "xmm2"); put("pextrd m ", m32); put("pextrw m ", m16);
        /* pmovsx / pmovzx from register and memory */
        volatile g_u64 mm = v;
#define PMOV(insn, tag) __asm__ volatile("movq %2, %%xmm3\n\t" insn " %%xmm3, %%xmm4\n\tmovq %%xmm4, %0\n\tpextrq $1, %%xmm4, %1" : "=r"(lo), "=r"(hi) : "r"(v) : "xmm3", "xmm4"); put(tag " lo ", lo); put(tag " hi ", hi);
#define PMOVM(insn, tag) __asm__ volatile(insn " %2, %%xmm4\n\tmovq %%xmm4, %0\n\tpextrq $1, %%xmm4, %1" : "=r"(lo), "=r"(hi) : "m"(mm) : "xmm4"); put(tag " lo ", lo); put(tag " hi ", hi);
        PMOV("pmovsxbw", "sxbw") PMOV("pmovsxbd", "sxbd") PMOV("pmovsxbq", "sxbq") PMOV("pmovsxwd", "sxwd") PMOV("pmovsxwq", "sxwq") PMOV("pmovsxdq", "sxdq")
        PMOV("pmovzxbw", "zxbw") PMOV("pmovzxbd", "zxbd") PMOV("pmovzxbq", "zxbq") PMOV("pmovzxwd", "zxwd") PMOV("pmovzxwq", "zxwq") PMOV("pmovzxdq", "zxdq")
        PMOVM("pmovsxbd", "msxbd") PMOVM("pmovzxwq", "mzxwq") PMOVM("pmovsxbq", "msxbq") PMOVM("pmovzxbw", "mzxbw")
        /* rounding */
        static const double ds[6] = { 2.5, -2.5, 3.7, -0.2, 1e18, -7.5 };
        for (int k = 0; k < 6; k++) {
            g_u64 out;
#define RND(mode, tag) __asm__ volatile("movsd %1, %%xmm5\n\tpcmpeqb %%xmm6, %%xmm6\n\troundsd $" #mode ", %%xmm5, %%xmm6\n\tmovq %%xmm6, %0" : "=r"(out) : "m"(ds[k]) : "xmm5", "xmm6"); put(tag " ", out);
            RND(0, "rsd0") RND(1, "rsd1") RND(2, "rsd2") RND(3, "rsd3") RND(4, "rsd4")
            __asm__ volatile("movsd %1, %%xmm5\n\tpcmpeqb %%xmm6, %%xmm6\n\troundsd $9, %%xmm5, %%xmm6\n\tpextrq $1, %%xmm6, %0" : "=r"(out) : "m"(ds[k]) : "xmm5", "xmm6"); put("rsd hi ", out);
            float fs = (float)ds[k]; g_u32 o32;
            __asm__ volatile("movss %1, %%xmm5\n\troundss $1, %%xmm5, %%xmm6\n\tmovd %%xmm6, %0" : "=r"(o32) : "m"(fs) : "xmm5", "xmm6"); put("rss1 ", o32);
            __asm__ volatile("movsd %1, %%xmm5\n\tmovlhps %%xmm5, %%xmm5\n\troundpd $2, %%xmm5, %%xmm6\n\tmovq %%xmm6, %0" : "=r"(out) : "m"(ds[k]) : "xmm5", "xmm6"); put("rpd2 ", out);
            __asm__ volatile("movss %1, %%xmm5\n\tshufps $0, %%xmm5, %%xmm5\n\troundps $3, %%xmm5, %%xmm6\n\tpextrd $3, %%xmm6, %0" : "=r"(o32) : "m"(fs) : "xmm5", "xmm6"); put("rps3 ", o32);
        }
    }
    return 0;
}

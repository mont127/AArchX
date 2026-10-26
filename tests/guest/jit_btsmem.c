/* bts, btr and btc on memory, locked and not, with immediate and register bit
 * offsets (past the operand and negative ones too), and cmpxchg16b matching,
 * half matching and not, locked and not: CF and ZF, the words they leave and
 * RDX:RAX.  The JIT has translated both since 2026-10-26 (byte LSE atomics and
 * caspal ordered, loads and stores plain); jit_btsmem_ord runs this file with
 * OCERZ_NO_PLAIN_MEM=1 for the ordered forms. */
#include "gsys.h"
static void put(const char *tag, g_u64 v) { g_puts(tag); g_puthex64(v); }
static g_u64 words[8] __attribute__((aligned(16)));
static g_u64 pair[2] __attribute__((aligned(16)));
static g_u64 hash(void)
{
    g_u64 h = 0;
    for (int i = 0; i < 8; i++)
        h = (h << 7 | h >> 57) ^ words[i];
    return h;
}
static void cas16(const char *tag, g_u64 a, g_u64 d, g_u64 b, g_u64 c, int lock)
{
    g_u64 z;
    if (lock)
        __asm__ volatile("lock cmpxchg16b %1\n\tsetz %b0" : "=r"(z), "+m"(pair[0]), "+a"(a), "+d"(d) : "b"(b), "c"(c) : "cc", "memory");
    else
        __asm__ volatile("cmpxchg16b %1\n\tsetz %b0" : "=r"(z), "+m"(pair[0]), "+a"(a), "+d"(d) : "b"(b), "c"(c) : "cc", "memory");
    g_puts(tag);
    put(" zf ", z & 0xff);
    put(" rax ", a);
    put(" rdx ", d);
    put(" lo ", pair[0]);
    put(" hi ", pair[1]);
}
int main(void)
{
    static const g_u64 init[8] = { 0x8000000000000001ULL, 0x00000000ffffffffULL, 0x5555555555555555ULL, 0,
                                   0xaaaaaaaaaaaaaaaaULL, 1ULL << 63, 0x0123456789abcdefULL, ~0ULL };
    for (int i = 0; i < 8; i++)
        words[i] = init[i];
    g_u64 *base = &words[4];
    g_u64 c, f;
    for (int n = 0; n < 12; n++) {
        g_u64 off = (g_u64)((long)(n * 37) - 200);
        __asm__ volatile("btsq %2, %1\n\tsetc %b0" : "=r"(c), "+m"(*base) : "r"(off) : "cc", "memory"); put("bts64 mr ", c & 0xff);
        __asm__ volatile("lock btsq %2, %1\n\tsetc %b0" : "=r"(c), "+m"(*base) : "r"(off + 3) : "cc", "memory"); put("lock bts64 mr ", c & 0xff);
        __asm__ volatile("btrq %2, %1\n\tsetc %b0" : "=r"(c), "+m"(*base) : "r"(off + 5) : "cc", "memory"); put("btr64 mr ", c & 0xff);
        __asm__ volatile("lock btcq %2, %1\n\tsetc %b0" : "=r"(c), "+m"(*base) : "r"(off - 9) : "cc", "memory"); put("lock btc64 mr ", c & 0xff);
        __asm__ volatile("lock btrl %k2, %1\n\tsetc %b0" : "=r"(c), "+m"(*(g_u32 *)base) : "r"((g_u64)((long)off / 2)) : "cc", "memory"); put("lock btr32 mr ", c & 0xff);
        __asm__ volatile("btcl %k2, %1\n\tsetc %b0" : "=r"(c), "+m"(*(g_u32 *)base) : "r"(off + 1) : "cc", "memory"); put("btc32 mr ", c & 0xff);
        __asm__ volatile("lock btsw %w2, %1\n\tsetc %b0" : "=r"(c), "+m"(*(unsigned short *)base) : "r"((g_u64)(n * 23 - 130)) : "cc", "memory"); put("lock bts16 mr ", c & 0xff);
        __asm__ volatile("xorl %%eax, %%eax\n\tlock btsq %2, %1\n\tpushfq\n\tpopq %0" : "=r"(f), "+m"(*base) : "r"(off + 11) : "cc", "rax", "memory"); put("lock bts64 flags ", f & 0x41);
        __asm__ volatile("xorl %%eax, %%eax\n\tincl %%eax\n\tbtrq %2, %1\n\tpushfq\n\tpopq %0" : "=r"(f), "+m"(*base) : "r"(off - 2) : "cc", "rax", "memory"); put("btr64 flags ", f & 0x41);
        put("words ", hash());
    }
    for (int i = 0; i < 8; i++) {
        g_u64 *w = &words[i];
        __asm__ volatile("btsq $70, %1\n\tsetc %b0" : "=r"(c), "+m"(*w) : : "cc", "memory"); put("bts64 mi70 ", c & 0xff);
        __asm__ volatile("lock btrq $63, %1\n\tsetc %b0" : "=r"(c), "+m"(*w) : : "cc", "memory"); put("lock btr64 mi63 ", c & 0xff);
        __asm__ volatile("lock btcl $45, %1\n\tsetc %b0" : "=r"(c), "+m"(*(g_u32 *)w) : : "cc", "memory"); put("lock btc32 mi45 ", c & 0xff);
        __asm__ volatile("btsw $9, %1\n\tsetc %b0" : "=r"(c), "+m"(*(unsigned short *)w) : : "cc", "memory"); put("bts16 mi9 ", c & 0xff);
        __asm__ volatile("lock btsl $0, %1\n\tsetc %b0" : "=r"(c), "+m"(*(g_u32 *)w) : : "cc", "memory"); put("lock bts32 mi0 ", c & 0xff);
        g_u64 r = 0;
        __asm__ volatile("lock btsq $33, %1\n\tjc 1f\n\tmovq $1, %0\n\t1:" : "+r"(r), "+m"(*w) : : "cc", "memory"); put("lock bts64 mi33 jc ", r);
        put("word ", *w);
    }
    pair[0] = 0x1111111111111111ULL;
    pair[1] = 0x2222222222222222ULL;
    cas16("cas16 match", 0x1111111111111111ULL, 0x2222222222222222ULL, 0x3333333333333333ULL, 0x4444444444444444ULL, 1);
    cas16("cas16 stale", 0x1111111111111111ULL, 0x2222222222222222ULL, 0x5555555555555555ULL, 0x6666666666666666ULL, 1);
    cas16("cas16 half", 0x3333333333333333ULL, 0x2222222222222222ULL, 0x7777777777777777ULL, 0x8888888888888888ULL, 1);
    cas16("cas16 plain", 0x3333333333333333ULL, 0x4444444444444444ULL, 0x9999999999999999ULL, 0xaaaaaaaaaaaaaaaaULL, 0);
    cas16("cas16 plain stale", 0, 0, 1, 2, 0);
    g_u64 lo = pair[0], hi = pair[1], tries = 0;
    for (int i = 0; i < 1000; i++) {
        g_u64 a = lo, d = hi, z;
        __asm__ volatile("lock cmpxchg16b %1\n\tsetz %b0" : "=r"(z), "+m"(pair[0]), "+a"(a), "+d"(d) : "b"(lo + 1), "c"(hi ^ (g_u64)i) : "cc", "memory");
        tries += z & 0xff;
        lo = a == lo ? lo + 1 : a;
        hi = d == hi ? hi ^ (g_u64)i : d;
    }
    put("cas16 loop tries ", tries);
    put("cas16 loop lo ", pair[0]);
    put("cas16 loop hi ", pair[1]);
    return 0;
}

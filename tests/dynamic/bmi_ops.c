#include <stdint.h>
#include <stdio.h>

#define FN(name, body) extern uint64_t name(uint64_t, uint64_t, uint64_t, uint64_t *); \
    __asm__(".text\n.p2align 4\n.globl _" #name "\n_" #name ":\n pushq %rdx\n popfq\n " body "\n pushfq\n popq (%rcx)\n ret\n")

FN(andn64, "andnq %rsi, %rdi, %rax");
FN(andn32, "movq $-1, %rax\n andnl %esi, %edi, %eax");
FN(andn64m, "movq %rsi, -16(%rsp)\n andnq -16(%rsp), %rdi, %rax");
FN(blsr64, "blsrq %rdi, %rax");
FN(blsr32, "movq $-1, %rax\n blsrl %edi, %eax");
FN(blsr64m, "movq %rdi, -16(%rsp)\n blsrq -16(%rsp), %rax");
FN(blsmsk64, "blsmskq %rdi, %rax");
FN(blsmsk32, "movq $-1, %rax\n blsmskl %edi, %eax");
FN(blsi64, "blsiq %rdi, %rax");
FN(blsi32, "movq $-1, %rax\n blsil %edi, %eax");
FN(bzhi64, "bzhiq %rsi, %rdi, %rax");
FN(bzhi32, "movq $-1, %rax\n bzhil %esi, %edi, %eax");
FN(bzhi64m, "movq %rdi, -16(%rsp)\n bzhiq %rsi, -16(%rsp), %rax");
FN(bextr64, "bextrq %rsi, %rdi, %rax");
FN(bextr32, "movq $-1, %rax\n bextrl %esi, %edi, %eax");
FN(bextr32m, "movl %edi, -16(%rsp)\n movq $-1, %rax\n bextrl %esi, -16(%rsp), %eax");
FN(pdep64, "pdepq %rsi, %rdi, %rax");
FN(pdep32, "movq $-1, %rax\n pdepl %esi, %edi, %eax");
FN(pdep64m, "movq %rsi, -16(%rsp)\n pdepq -16(%rsp), %rdi, %rax");
FN(pext64, "pextq %rsi, %rdi, %rax");
FN(pext32, "movq $-1, %rax\n pextl %esi, %edi, %eax");
FN(pext32m, "movl %esi, -16(%rsp)\n movq $-1, %rax\n pextl -16(%rsp), %edi, %eax");
FN(mulx64, "movq %rdi, %rdx\n mulxq %rsi, %r8, %rax\n movq %r8, 8(%rcx)");
FN(mulx32, "movq %rdi, %rdx\n movq $-1, %rax\n movq $-1, %r8\n mulxl %esi, %r8d, %eax\n movq %r8, 8(%rcx)");
FN(mulx64m, "movq %rdi, %rdx\n movq %rsi, -16(%rsp)\n mulxq -16(%rsp), %r8, %rax\n movq %r8, 8(%rcx)");
FN(mulx64same, "movq %rdi, %rdx\n mulxq %rsi, %rax, %rax");
FN(rorx64, "rorxq $13, %rdi, %rax");
FN(rorx32, "movq $-1, %rax\n rorxl $7, %edi, %eax");
FN(rorx64m, "movq %rdi, -16(%rsp)\n rorxq $63, -16(%rsp), %rax");
FN(shlx64, "shlxq %rsi, %rdi, %rax");
FN(shlx32, "movq $-1, %rax\n shlxl %esi, %edi, %eax");
FN(shrx64, "shrxq %rsi, %rdi, %rax");
FN(shrx32, "movq $-1, %rax\n shrxl %esi, %edi, %eax");
FN(sarx64, "sarxq %rsi, %rdi, %rax");
FN(sarx32, "movq $-1, %rax\n sarxl %esi, %edi, %eax");
FN(sarx64m, "movq %rdi, -16(%rsp)\n sarxq %rsi, -16(%rsp), %rax");
FN(tzcnt64, "tzcntq %rdi, %rax");
FN(tzcnt32, "movq $-1, %rax\n tzcntl %edi, %eax");
FN(tzcnt16, "movq $-1, %rax\n tzcntw %di, %ax");
FN(lzcnt64, "lzcntq %rdi, %rax");
FN(lzcnt32, "movq $-1, %rax\n lzcntl %edi, %eax");
FN(lzcnt16, "movq $-1, %rax\n lzcntw %di, %ax");
FN(popcnt64, "popcntq %rdi, %rax");
FN(movbe64, "movq %rdi, -16(%rsp)\n movbeq -16(%rsp), %rax");
FN(movbe32, "movq %rdi, -16(%rsp)\n movq $-1, %rax\n movbel -16(%rsp), %eax");
FN(movbe16, "movq %rdi, -16(%rsp)\n movq $-1, %rax\n movbew -16(%rsp), %ax");
FN(movbes64, "movq $0, -16(%rsp)\n movbeq %rdi, -16(%rsp)\n movq -16(%rsp), %rax");
FN(movbes32, "movq $-1, -16(%rsp)\n movbel %edi, -16(%rsp)\n movq -16(%rsp), %rax");
FN(movbes16, "movq $-1, -16(%rsp)\n movbew %di, -16(%rsp)\n movq -16(%rsp), %rax");
FN(rdrand64, "rdrandq %rax");

#define CF 0x1ull
#define PF 0x4ull
#define AF 0x10ull
#define ZF 0x40ull
#define SF 0x80ull
#define OF 0x800ull
#define ARITH (CF | PF | AF | ZF | SF | OF)

static uint64_t rng = 0x2545f4914f6cdd1dull;
static int fails;

static uint64_t next64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static uint64_t pick(void)
{
    uint64_t r = next64();
    switch (r & 15) {
    case 0: return 0;
    case 1: return 1;
    case 2: return ~0ull;
    case 3: return 1ull << 63;
    case 4: return 0x80000000ull;
    case 5: return 0xffffffffull;
    case 6: return next64() & next64();
    case 7: return next64() | next64();
    case 8:
    case 9: return next64() % 300;
    case 10: return next64() % 0x10000;
    default: return next64();
    }
}

static void check(const char *name, int iter, uint64_t got, uint64_t want)
{
    if (got != want) {
        if (fails < 20)
            printf("BAD %s iteration %d got %016llx want %016llx\n", name, iter, (unsigned long long)got,
                   (unsigned long long)want);
        fails++;
    }
}

static uint64_t szf(uint64_t r, int bits)
{
    return (r == 0 ? ZF : 0) | (((r >> (bits - 1)) & 1) ? SF : 0);
}

static uint64_t ref_pdep(uint64_t v, uint64_t mask)
{
    uint64_t r = 0;
    int k = 0;
    for (int i = 0; i < 64; i++)
        if ((mask >> i) & 1)
            r |= ((v >> k++) & 1) << i;
    return r;
}

static uint64_t ref_pext(uint64_t v, uint64_t mask)
{
    uint64_t r = 0;
    int k = 0;
    for (int i = 0; i < 64; i++)
        if ((mask >> i) & 1)
            r |= ((v >> i) & 1) << k++;
    return r;
}

static uint64_t rotr64(uint64_t v, unsigned c)
{
    c &= 63;
    return c ? (v >> c) | (v << (64 - c)) : v;
}

static uint32_t rotr32(uint32_t v, unsigned c)
{
    c &= 31;
    return c ? (v >> c) | (v << (32 - c)) : v;
}

int main(void)
{
    uint64_t fo[2];
    for (int iter = 0; iter < 3000; iter++) {
        uint64_t a = pick(), b = pick();
        uint32_t a32 = (uint32_t)a, b32 = (uint32_t)b;
        uint64_t fin = (iter & 1) ? (ARITH | 2) : 2;
        uint64_t r, w;

        r = andn64(a, b, fin, fo);
        w = ~a & b;
        check("andn64", iter, r, w);
        check("andn64 flags", iter, fo[0] & (CF | ZF | SF | OF), szf(w, 64));
        r = andn32(a, b, fin, fo);
        w = (uint32_t)(~a32 & b32);
        check("andn32", iter, r, w);
        check("andn32 flags", iter, fo[0] & (CF | ZF | SF | OF), szf(w, 32));
        r = andn64m(a, b, fin, fo);
        check("andn64m", iter, r, ~a & b);

        r = blsr64(a, b, fin, fo);
        w = a & (a - 1);
        check("blsr64", iter, r, w);
        check("blsr64 flags", iter, fo[0] & (CF | ZF | SF | OF), szf(w, 64) | (a == 0 ? CF : 0));
        r = blsr32(a, b, fin, fo);
        w = (uint32_t)(a32 & (a32 - 1));
        check("blsr32", iter, r, w);
        check("blsr32 flags", iter, fo[0] & (CF | ZF | SF | OF), szf(w, 32) | (a32 == 0 ? CF : 0));
        r = blsr64m(a, b, fin, fo);
        check("blsr64m", iter, r, a & (a - 1));

        r = blsmsk64(a, b, fin, fo);
        w = a ^ (a - 1);
        check("blsmsk64", iter, r, w);
        check("blsmsk64 flags", iter, fo[0] & (CF | ZF | SF | OF), szf(w, 64) | (a == 0 ? CF : 0));
        r = blsmsk32(a, b, fin, fo);
        w = (uint32_t)(a32 ^ (a32 - 1));
        check("blsmsk32", iter, r, w);
        check("blsmsk32 flags", iter, fo[0] & (CF | ZF | SF | OF), szf(w, 32) | (a32 == 0 ? CF : 0));

        r = blsi64(a, b, fin, fo);
        w = a & (0 - a);
        check("blsi64", iter, r, w);
        check("blsi64 flags", iter, fo[0] & (CF | ZF | SF | OF), szf(w, 64) | (a != 0 ? CF : 0));
        r = blsi32(a, b, fin, fo);
        w = (uint32_t)(a32 & (0 - a32));
        check("blsi32", iter, r, w);
        check("blsi32 flags", iter, fo[0] & (CF | ZF | SF | OF), szf(w, 32) | (a32 != 0 ? CF : 0));

        unsigned n = (unsigned)(b & 0xff);
        r = bzhi64(a, b, fin, fo);
        w = n < 64 ? a & ((1ull << n) - 1) : a;
        check("bzhi64", iter, r, w);
        check("bzhi64 flags", iter, fo[0] & (CF | ZF | SF | OF), szf(w, 64) | (n > 63 ? CF : 0));
        r = bzhi64m(a, b, fin, fo);
        check("bzhi64m", iter, r, w);
        r = bzhi32(a, b, fin, fo);
        w = n < 32 ? a32 & ((1u << n) - 1) : a32;
        check("bzhi32", iter, r, w);
        check("bzhi32 flags", iter, fo[0] & (CF | ZF | SF | OF), szf(w, 32) | (n > 31 ? CF : 0));

        unsigned start = (unsigned)(b & 0xff), len = (unsigned)((b >> 8) & 0xff);
        r = bextr64(a, b, fin, fo);
        w = start < 64 ? a >> start : 0;
        if (len < 64)
            w &= (1ull << len) - 1;
        check("bextr64", iter, r, w);
        check("bextr64 flags", iter, fo[0] & (CF | ZF | OF), w == 0 ? ZF : 0);
        r = bextr32(a, b, fin, fo);
        w = start < 32 ? a32 >> start : 0;
        if (len < 32)
            w &= (1ull << len) - 1;
        check("bextr32", iter, r, w);
        check("bextr32 flags", iter, fo[0] & (CF | ZF | OF), w == 0 ? ZF : 0);
        r = bextr32m(a, b, fin, fo);
        check("bextr32m", iter, r, w);

        r = pdep64(a, b, fin, fo);
        check("pdep64", iter, r, ref_pdep(a, b));
        check("pdep64 flags", iter, fo[0] & ARITH, fin & ARITH);
        r = pdep32(a, b, fin, fo);
        check("pdep32", iter, r, ref_pdep(a32, b32));
        r = pdep64m(a, b, fin, fo);
        check("pdep64m", iter, r, ref_pdep(a, b));
        r = pext64(a, b, fin, fo);
        check("pext64", iter, r, ref_pext(a, b));
        check("pext64 flags", iter, fo[0] & ARITH, fin & ARITH);
        r = pext32(a, b, fin, fo);
        check("pext32", iter, r, ref_pext(a32, b32));
        r = pext32m(a, b, fin, fo);
        check("pext32m", iter, r, ref_pext(a32, b32));

        unsigned __int128 p = (unsigned __int128)a * b;
        r = mulx64(a, b, fin, fo);
        check("mulx64 hi", iter, r, (uint64_t)(p >> 64));
        check("mulx64 lo", iter, fo[1], (uint64_t)p);
        check("mulx64 flags", iter, fo[0] & ARITH, fin & ARITH);
        r = mulx64m(a, b, fin, fo);
        check("mulx64m hi", iter, r, (uint64_t)(p >> 64));
        check("mulx64m lo", iter, fo[1], (uint64_t)p);
        r = mulx64same(a, b, fin, fo);
        check("mulx64same", iter, r, (uint64_t)(p >> 64));
        uint64_t p32 = (uint64_t)a32 * b32;
        r = mulx32(a, b, fin, fo);
        check("mulx32 hi", iter, r, p32 >> 32);
        check("mulx32 lo", iter, fo[1], (uint32_t)p32);

        r = rorx64(a, b, fin, fo);
        check("rorx64", iter, r, rotr64(a, 13));
        check("rorx64 flags", iter, fo[0] & ARITH, fin & ARITH);
        r = rorx32(a, b, fin, fo);
        check("rorx32", iter, r, rotr32(a32, 7));
        r = rorx64m(a, b, fin, fo);
        check("rorx64m", iter, r, rotr64(a, 63));

        r = shlx64(a, b, fin, fo);
        check("shlx64", iter, r, a << (b & 63));
        check("shlx64 flags", iter, fo[0] & ARITH, fin & ARITH);
        r = shlx32(a, b, fin, fo);
        check("shlx32", iter, r, (uint32_t)(a32 << (b32 & 31)));
        r = shrx64(a, b, fin, fo);
        check("shrx64", iter, r, a >> (b & 63));
        r = shrx32(a, b, fin, fo);
        check("shrx32", iter, r, a32 >> (b32 & 31));
        r = sarx64(a, b, fin, fo);
        check("sarx64", iter, r, (uint64_t)((int64_t)a >> (b & 63)));
        check("sarx64 flags", iter, fo[0] & ARITH, fin & ARITH);
        r = sarx32(a, b, fin, fo);
        check("sarx32", iter, r, (uint32_t)((int32_t)a32 >> (b32 & 31)));
        r = sarx64m(a, b, fin, fo);
        check("sarx64m", iter, r, (uint64_t)((int64_t)a >> (b & 63)));

        r = tzcnt64(a, b, fin, fo);
        w = a ? (uint64_t)__builtin_ctzll(a) : 64;
        check("tzcnt64", iter, r, w);
        check("tzcnt64 flags", iter, fo[0] & (CF | ZF), (a == 0 ? CF : 0) | (w == 0 ? ZF : 0));
        r = tzcnt32(a, b, fin, fo);
        w = a32 ? (uint64_t)__builtin_ctz(a32) : 32;
        check("tzcnt32", iter, r, w);
        uint16_t a16 = (uint16_t)a;
        r = tzcnt16(a, b, fin, fo);
        w = a16 ? (uint64_t)__builtin_ctz(a16) : 16;
        check("tzcnt16", iter, r, 0xffffffffffff0000ull | w);
        r = lzcnt64(a, b, fin, fo);
        w = a ? (uint64_t)__builtin_clzll(a) : 64;
        check("lzcnt64", iter, r, w);
        check("lzcnt64 flags", iter, fo[0] & (CF | ZF), (a == 0 ? CF : 0) | (w == 0 ? ZF : 0));
        r = lzcnt32(a, b, fin, fo);
        w = a32 ? (uint64_t)__builtin_clz(a32) : 32;
        check("lzcnt32", iter, r, w);
        r = lzcnt16(a, b, fin, fo);
        w = a16 ? (uint64_t)(__builtin_clz(a16) - 16) : 16;
        check("lzcnt16", iter, r, 0xffffffffffff0000ull | w);
        r = popcnt64(a, b, fin, fo);
        check("popcnt64", iter, r, (uint64_t)__builtin_popcountll(a));
        check("popcnt64 flags", iter, fo[0] & ARITH, a == 0 ? ZF : 0);

        r = movbe64(a, b, fin, fo);
        check("movbe64", iter, r, __builtin_bswap64(a));
        check("movbe64 flags", iter, fo[0] & ARITH, fin & ARITH);
        r = movbe32(a, b, fin, fo);
        check("movbe32", iter, r, __builtin_bswap32(a32));
        r = movbe16(a, b, fin, fo);
        check("movbe16", iter, r, 0xffffffffffff0000ull | __builtin_bswap16(a16));
        r = movbes64(a, b, fin, fo);
        check("movbes64", iter, r, __builtin_bswap64(a));
        r = movbes32(a, b, fin, fo);
        check("movbes32", iter, r, 0xffffffff00000000ull | __builtin_bswap32(a32));
        r = movbes16(a, b, fin, fo);
        check("movbes16", iter, r, 0xffffffffffff0000ull | __builtin_bswap16(a16));
    }
    for (int i = 0; i < 4; i++) {
        rdrand64(0, 0, i ? ARITH | 2 : 2, fo);
        check("rdrand64 flags", i, fo[0] & ARITH, CF);
    }
    if (!fails)
        printf("OK\n");
    return fails != 0;
}

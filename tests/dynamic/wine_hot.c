/*
 * Instructions Wine's own code runs constantly, which src/jit.c translates
 * and once handed to the interpreter or to ocerz_flags_materialize: adc and
 * sbb on 8- and 16-bit registers (ucrtbase's wcscmp ends in sbb al, 0),
 * tzcnt and lzcnt with a flag read after (ntdll's heap), one movs, stos or
 * lods at a time in both directions (memcpy's tail), a flag producer ahead
 * of a shift or rotate by cl, every condition after an and, or or xor into
 * a byte or word register, and the carry of a shift by a constant read by a
 * branch, setc, adc, sbb and cmov.  Each result is mixed into a hash that is
 * printed; the expected output is Rosetta's.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NOINLINE __attribute__((noinline))

/* cmp a, b; then sbb/adc on a narrow register; returns result and flags */
static NOINLINE uint64_t sbb8(uint8_t x, uint8_t y, uint64_t a, uint64_t b)
{
    uint64_t r = 0xaabbccdd11223300ull | x, f;
    __asm__ volatile("cmpq %[b], %[a]\n\tsbbb %[y], %b[r]\n\tpushfq\n\tpopq %[f]"
                     : [r] "+r"(r), [f] "=&r"(f) : [a] "r"(a), [b] "r"(b), [y] "q"(y) : "cc");
    return r ^ ((f & 0x8d5) << 40);
}
static NOINLINE uint64_t adc16(uint16_t x, uint16_t y, uint64_t a, uint64_t b)
{
    uint64_t r = 0x1122334455660000ull | x, f;
    __asm__ volatile("cmpq %[b], %[a]\n\tadcw %[y], %w[r]\n\tpushfq\n\tpopq %[f]"
                     : [r] "+r"(r), [f] "=&r"(f) : [a] "r"(a), [b] "r"(b), [y] "r"(y) : "cc");
    return r ^ ((f & 0x8d5) << 40);
}
static NOINLINE uint64_t sbb8imm(uint8_t x, uint64_t a, uint64_t b)
{
    uint64_t r = 0x55aa55aa55aa5500ull | x;
    uint8_t c, o, z, s;
    __asm__ volatile("cmpq %[b], %[a]\n\tsbbb $0x7f, %b[r]\n\tsetc %[c]\n\tseto %[o]\n\tsetz %[z]\n\tsets %[s]"
                     : [r] "+r"(r), [c] "=&q"(c), [o] "=&q"(o), [z] "=&q"(z), [s] "=&q"(s)
                     : [a] "r"(a), [b] "r"(b) : "cc");
    return r ^ ((uint64_t)(c | o << 1 | z << 2 | s << 3) << 56);
}
static NOINLINE uint64_t adc_ah(uint64_t v, uint8_t y, uint64_t a, uint64_t b)
{
    uint64_t f;
    __asm__ volatile("cmpq %[b], %[a]\n\tadcb %[y], %%ah\n\tsbbb %%ah, %%al\n\tpushfq\n\tpopq %[f]"
                     : "+a"(v), [f] "=&r"(f) : [a] "r"(a), [b] "r"(b), [y] "Q"(y) : "cc");
    return v ^ ((f & 0x8d5) << 40);
}
/* wcscmp's tail: cmp; seta; sbb al, 0; movsx */
static NOINLINE int wtail(uint16_t a, uint16_t b)
{
    int r;
    __asm__ volatile("xorl %%eax, %%eax\n\tcmpw %[b], %[a]\n\tseta %%al\n\tsbbb $0, %%al\n\tmovsbl %%al, %%eax"
                     : "=&a"(r) : [a] "r"(a), [b] "r"(b) : "cc");
    return r;
}

static NOINLINE uint64_t tz32(uint32_t v, int which)
{
    uint32_t r = 0xffffffff; uint8_t c = 9, z = 9;
    if (which == 0) __asm__ volatile("tzcntl %[v], %[r]\n\tsetc %[c]\n\tsetz %[z]" : [r] "+r"(r), [c] "=&q"(c), [z] "=&q"(z) : [v] "r"(v) : "cc");
    else            __asm__ volatile("lzcntl %[v], %[r]\n\tsetc %[c]\n\tsetz %[z]" : [r] "+r"(r), [c] "=&q"(c), [z] "=&q"(z) : [v] "r"(v) : "cc");
    return (uint64_t)r | (uint64_t)c << 32 | (uint64_t)z << 40;
}
static NOINLINE uint64_t tz64(uint64_t v, int which)
{
    uint64_t r = ~0ull; int taken = 0;
    if (which == 0) __asm__ volatile("tzcntq %[v], %[r]\n\tjnc 1f\n\taddl $1, %[t]\n1:\tjnz 2f\n\taddl $2, %[t]\n2:" : [r] "+r"(r), [t] "+r"(taken) : [v] "r"(v) : "cc");
    else            __asm__ volatile("lzcntq %[v], %[r]\n\tjnc 1f\n\taddl $1, %[t]\n1:\tjnz 2f\n\taddl $2, %[t]\n2:" : [r] "+r"(r), [t] "+r"(taken) : [v] "r"(v) : "cc");
    return r | (uint64_t)taken << 32;
}
/* tzcnt from memory, into the register it reads, then a rotate by cl and a lock and: RtlAllocateHeap's shape */
static NOINLINE uint64_t heapbits(uint32_t *word)
{
    uint32_t idx, old = *word;
    __asm__ volatile("movl (%[w]), %%eax\n\ttzcntl %%eax, %[i]\n\tmovl $0xfffffffe, %%eax\n\tmovl %[i], %%ecx\n\t"
                     "roll %%cl, %%eax\n\tlock andl %%eax, (%[w])"
                     : [i] "=&r"(idx) : [w] "r"(word) : "eax", "ecx", "cc", "memory");
    return (uint64_t)idx << 32 | (old ^ *word);
}
/* a flag producer, a shift by cl, then readers of the producer's flags when the count is 0 */
static NOINLINE uint64_t shift_keeps(uint64_t a, uint64_t b, uint64_t v, uint8_t count)
{
    uint8_t c, z;
    __asm__ volatile("cmpq %[b], %[a]\n\tshlq %%cl, %[v]\n\tsetc %[c]\n\tsetz %[z]"
                     : [v] "+r"(v), [c] "=&q"(c), [z] "=&q"(z) : [a] "r"(a), [b] "r"(b), "c"(count) : "cc");
    return v ^ ((uint64_t)c << 62) ^ ((uint64_t)z << 61);
}
static NOINLINE uint64_t shift_dead(uint64_t a, uint64_t b, uint64_t v, uint8_t count)
{
    uint64_t t;
    __asm__ volatile("tzcntq %[a], %[t]\n\trorq %%cl, %[v]\n\tsarq %%cl, %[v]\n\taddq %[b], %[v]\n\taddq %[t], %[v]"
                     : [v] "+r"(v), [t] "=&r"(t) : [a] "r"(a), [b] "r"(b), "c"(count) : "cc");
    return v;
}

/* and, or and xor into a byte or word register, then each condition by setcc, by a branch and by cmov */
#define NL(name, insn, reg, cc) \
    static NOINLINE uint64_t name(uint64_t a, uint64_t b) \
    { \
        uint8_t r; uint64_t j = 0, m = 5; \
        __asm__ volatile(insn "\n\tset" #cc " %[r]\n\t" insn "\n\tj" #cc " 1f\n\taddq $2, %[j]\n1:\t" insn "\n\tcmov" #cc "q %[b], %[m]" \
                         : [r] "=&q"(r), [j] "+r"(j), [m] "+r"(m), "+a"(a) : [b] "c"(b) : "cc"); \
        return (a << 16) ^ ((uint64_t)r << 8) ^ j ^ (m << 40); \
    }
#define NLSET(op, sfx, insn) \
    NL(op##sfx##_o, insn, a, o) NL(op##sfx##_no, insn, a, no) NL(op##sfx##_b, insn, a, b) NL(op##sfx##_ae, insn, a, ae) \
    NL(op##sfx##_e, insn, a, e) NL(op##sfx##_ne, insn, a, ne) NL(op##sfx##_be, insn, a, be) NL(op##sfx##_a, insn, a, a) \
    NL(op##sfx##_s, insn, a, s) NL(op##sfx##_ns, insn, a, ns) NL(op##sfx##_p, insn, a, p) NL(op##sfx##_np, insn, a, np) \
    NL(op##sfx##_l, insn, a, l) NL(op##sfx##_ge, insn, a, ge) NL(op##sfx##_le, insn, a, le) NL(op##sfx##_g, insn, a, g) \
    static uint64_t (*const op##sfx##_all[16])(uint64_t, uint64_t) = { op##sfx##_o, op##sfx##_no, op##sfx##_b, op##sfx##_ae, \
        op##sfx##_e, op##sfx##_ne, op##sfx##_be, op##sfx##_a, op##sfx##_s, op##sfx##_ns, op##sfx##_p, op##sfx##_np, \
        op##sfx##_l, op##sfx##_ge, op##sfx##_le, op##sfx##_g };
NLSET(and, b, "andb %%cl, %%al")
NLSET(or, b, "orb %%cl, %%al")
NLSET(xor, b, "xorb %%cl, %%al")
NLSET(and, w, "andw %%cx, %%ax")
NLSET(or, w, "orw %%cx, %%ax")
NLSET(xor, w, "xorw %%cx, %%ax")
NLSET(andi, b, "andb $0x30, %%al")
NLSET(ori, w, "orw $0x8001, %%ax")

/* a shift by a constant and readers of the bit it moved out: a branch, setc, adc, sbb and cmov, with a mov or a
   load between, and memcpy's tail: shr; jae for each power of two */
static NOINLINE uint64_t shcf64(uint64_t v, uint64_t *mem)
{
    uint64_t a = v, r = 0, t = 0; uint8_t c1, c2;
    __asm__ volatile("shrq $1, %[a]\n\tjnc 1f\n\taddq $1, %[r]\n1:\t"
                     "shrq $3, %[a]\n\tsetc %[c1]\n\t"
                     "shlq $5, %[a]\n\tmovq %[a], %[t]\n\tadcq $0, %[r]\n\t"
                     "sarq $63, %[t]\n\tsetnc %[c2]\n\t"
                     "shlq $63, %[a]\n\tsbbq $0, %[r]\n\t"
                     "movq (%[m]), %[a]\n\tshrq $7, %[a]\n\tmovq 8(%[m]), %[t]\n\tjc 2f\n\taddq $64, %[r]\n2:\t"
                     "sarq $2, %[t]\n\tcmovcq %[a], %[r]"
                     : [a] "+r"(a), [r] "+r"(r), [t] "+r"(t), [c1] "=&q"(c1), [c2] "=&q"(c2) : [m] "r"(mem) : "cc");
    return r ^ (a << 8) ^ ((uint64_t)c1 << 60) ^ ((uint64_t)c2 << 61) ^ t;
}
static NOINLINE uint64_t shcf32(uint32_t v)
{
    uint32_t a = v, r = 0; uint8_t c1, c2, c3;
    __asm__ volatile("shrl $1, %[a]\n\tsetc %[c1]\n\tshll $31, %[a]\n\tsetc %[c2]\n\torl %[v], %[a]\n\t"
                     "sarl $31, %[a]\n\tjnc 1f\n\taddl $7, %[r]\n1:\tshll $1, %[a]\n\tsetae %[c3]\n\tadcl %[a], %[r]"
                     : [a] "+r"(a), [r] "+r"(r), [c1] "=&q"(c1), [c2] "=&q"(c2), [c3] "=&q"(c3) : [v] "r"(v) : "cc");
    return r ^ ((uint64_t)a << 32) ^ ((uint64_t)c1 << 20) ^ ((uint64_t)c2 << 21) ^ ((uint64_t)c3 << 22);
}
static NOINLINE uint64_t tail(uint64_t n)
{
    uint64_t r = 0, a = n, b = n;
    __asm__ volatile("andq $3, %[b]\n\tshrq $3, %[a]\n\tjae 1f\n\taddq $4, %[r]\n1:\tshrq $1, %[a]\n\tjae 2f\n\taddq $8, %[r]\n2:\t"
                     "shrq $1, %[b]\n\tjae 3f\n\taddq $1, %[r]\n3:\tshrq $1, %[b]\n\tjae 4f\n\taddq $2, %[r]\n4:"
                     : [a] "+r"(a), [b] "+r"(b), [r] "+r"(r) : : "cc");
    return r ^ (a << 8) ^ (b << 40);
}

static uint8_t sbuf[64], dbuf[64];
static NOINLINE void strings(int backwards)
{
    for (int i = 0; i < 64; i++) { sbuf[i] = (uint8_t)(i * 7 + 3); dbuf[i] = 0xee; }
    uint8_t *s = sbuf + (backwards ? 40 : 1), *d = dbuf + (backwards ? 40 : 1);
    uint64_t acc = 0x1111111111111111ull;
    if (backwards) __asm__ volatile("std");
    __asm__ volatile("movsq\n\tmovsl\n\tmovsw\n\tmovsb\n\tlodsb\n\tstosb\n\tlodsw\n\tstosw\n\tlodsl\n\tstosl\n\tlodsq\n\tstosq"
                     : "+S"(s), "+D"(d), "+a"(acc) : : "memory");
    __asm__ volatile("cld");
    printf("strings %s: s%+ld d%+ld acc %016llx dst", backwards ? "down" : "up", (long)(s - sbuf), (long)(d - dbuf),
           (unsigned long long)acc);
    for (int i = 0; i < 64; i++) printf("%s%02x", i % 16 ? "" : " ", dbuf[i]);
    printf("\n");
}

int main(void)
{
    static const uint64_t pairs[][2] = { { 1, 2 }, { 2, 1 }, { 5, 5 }, { 0, ~0ull }, { ~0ull, 0 } };
    static const uint8_t bytes[] = { 0, 1, 0x7f, 0x80, 0xff, 0x55 };
    uint64_t h = 1469598103934665603ull;
#define MIX(v) (h = (h ^ (uint64_t)(v)) * 1099511628211ull)
    for (unsigned p = 0; p < 5; p++)
        for (unsigned i = 0; i < 6; i++) {
            for (unsigned j = 0; j < 6; j++) {
                MIX(sbb8(bytes[i], bytes[j], pairs[p][0], pairs[p][1]));
                MIX(adc16((uint16_t)(bytes[i] * 257u), (uint16_t)(bytes[j] * 259u + 1), pairs[p][0], pairs[p][1]));
                MIX(adc_ah(0x123456789abc0000ull | (uint64_t)bytes[i] << 8 | bytes[j], bytes[(i + j) % 6], pairs[p][0], pairs[p][1]));
            }
            MIX(sbb8imm(bytes[i], pairs[p][0], pairs[p][1]));
        }
    printf("carry %016llx\n", (unsigned long long)h);
    printf("wtail %d %d %d %d\n", wtail(1, 2), wtail(2, 1), wtail(7, 7), wtail(0xffff, 0));
    static const uint64_t vals[] = { 0, 1, 2, 0x80000000ull, 0xffffffffull, 0x100000000ull, 0x8000000000000000ull, ~0ull, 0x10, 0xff00 };
    for (unsigned i = 0; i < 10; i++)
        printf("bits %llx: %llx %llx %llx %llx\n", (unsigned long long)vals[i], (unsigned long long)tz32((uint32_t)vals[i], 0),
               (unsigned long long)tz32((uint32_t)vals[i], 1), (unsigned long long)tz64(vals[i], 0), (unsigned long long)tz64(vals[i], 1));
    uint32_t word = 0xf0f0;
    printf("heap %llx", (unsigned long long)heapbits(&word));
    printf(" %llx", (unsigned long long)heapbits(&word));
    word = 1;
    printf(" %llx %x\n", (unsigned long long)heapbits(&word), word);
    h = 1469598103934665603ull;
    for (unsigned p = 0; p < 5; p++)
        for (unsigned c = 0; c < 70; c += 3) {
            MIX(shift_keeps(pairs[p][0], pairs[p][1], 0x0123456789abcdefull, (uint8_t)c));
            MIX(shift_dead(pairs[p][0] | 4, pairs[p][1], 0xfedcba9876543210ull, (uint8_t)c));
        }
    printf("shifts %016llx\n", (unsigned long long)h);
    h = 1469598103934665603ull;
    static uint64_t (*const *const sets[])(uint64_t, uint64_t) = { andb_all, orb_all, xorb_all, andw_all, orw_all, xorw_all, andib_all, oriw_all };
    static const uint64_t nv[] = { 0, 1, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x8001, 0x30, 0xcf, 0x123456789abcdef0ull };
    for (unsigned k = 0; k < 8; k++)
        for (unsigned c = 0; c < 16; c++)
            for (unsigned i = 0; i < 13; i++)
                for (unsigned j = 0; j < 13; j++)
                    MIX(sets[k][c](nv[i], nv[j]));
    printf("narrow %016llx\n", (unsigned long long)h);
    h = 1469598103934665603ull;
    uint64_t mem[2];
    for (unsigned i = 0; i < 10; i++)
        for (unsigned j = 0; j < 10; j++) {
            mem[0] = vals[j] * 0x9e3779b97f4a7c15ull + i; mem[1] = vals[i] ^ (vals[j] << 3);
            MIX(shcf64(vals[i] + j * 0x41, mem));
            MIX(shcf32((uint32_t)(vals[i] >> (j & 7)) + j));
        }
    for (uint64_t n = 0; n < 64; n++) MIX(tail(n));
    printf("shiftcf %016llx\n", (unsigned long long)h);
    strings(0);
    strings(1);
    return 0;
}

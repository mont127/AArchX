/* Byte arithmetic, logic, cmp and test with ah or ch on either side (register, immediate and memory operands), the
 * flags read back by pushf (the defined ones: CF, PF, ZF, SF, OF, and AF after add, sub and cmp) and by setcc right
 * after the instruction, on edge values. Builds for i386 and x86_64; high8.expected is the x86_64 build's output under
 * Rosetta, and the i386 lines are the same ones. The x86_64 build steps past the red zone around pushf, which would
 * overwrite the memory operand there. */
#include <stdio.h>
#include <stdint.h>

#ifdef __x86_64__
#define PUSHF_POP "lea -128(%%rsp), %%rsp\n\tpushf\n\tpop %2\n\tlea 128(%%rsp), %%rsp"
#else
#define PUSHF_POP "pushf\n\tpop %2"
#endif

#define ARITHF 0x8d5ul
#define LOGICF 0x8c5ul

#define OPF(name, insn, fmask)                                                                               \
    static uint64_t name(uint32_t a, uint32_t c)                                                             \
    {                                                                                                        \
        unsigned long f;                                                                                     \
        volatile uint8_t m = (uint8_t)(c >> 16);                                                             \
        __asm__ volatile(insn "\n\t" PUSHF_POP : "+a"(a), "+c"(c), "=r"(f) : "m"(m) : "cc");             \
        return (uint64_t)a << 32 ^ (uint64_t)(uint16_t)c << 16 ^ (uint64_t)(f & (fmask)) ^ (uint64_t)m << 12;  \
    }
#define OPS(name, cc, insn)                                                                                  \
    static uint64_t name(uint32_t a, uint32_t c)                                                             \
    {                                                                                                        \
        uint8_t r;                                                                                           \
        volatile uint8_t m = (uint8_t)(c >> 16);                                                             \
        __asm__ volatile(insn "\n\tset" cc " %2" : "+a"(a), "+c"(c), "=d"(r) : "m"(m) : "cc");                \
        return (uint64_t)a << 32 ^ (uint64_t)(uint16_t)c << 16 ^ r;                                          \
    }

#define ALU(op, f)                                                                                           \
    OPF(op##_hh, #op "b %%ch, %%ah", f) OPF(op##_hl, #op "b %%cl, %%ah", f) OPF(op##_lh, #op "b %%ch, %%al", f) \
    OPF(op##_hi, #op "b $0x7f, %%ah", f) OPF(op##_hm, #op "b %3, %%ah", f) OPF(op##_mh, #op "b %%ah, %3", f)
ALU(add, ARITHF) ALU(sub, ARITHF) ALU(and, LOGICF) ALU(or, LOGICF) ALU(xor, LOGICF)
OPF(cmp_hh, "cmpb %%ch, %%ah", ARITHF) OPF(cmp_hi, "cmpb $0x80, %%ah", ARITHF) OPF(cmp_hm, "cmpb %3, %%ah", ARITHF)
OPF(cmp_mh, "cmpb %%ah, %3", ARITHF) OPF(cmp_lh, "cmpb %%ch, %%al", ARITHF)
OPF(test_hh, "testb %%ch, %%ah", LOGICF) OPF(test_hi, "testb $0x40, %%ah", LOGICF) OPF(test_mh, "testb %%ah, %3", LOGICF)
OPS(cmp_b, "b", "cmpb %%ch, %%ah") OPS(cmp_l, "l", "cmpb %%ch, %%ah") OPS(cmp_e, "e", "cmpb $0x80, %%ah")
OPS(test_z, "z", "testb $0x40, %%ah") OPS(test_s, "s", "testb %%ch, %%ah") OPS(and_z, "z", "andb %%cl, %%ah")
OPS(add_c, "c", "addb %%ch, %%ah") OPS(sub_o, "o", "subb %%ch, %%ah") OPS(xor_p, "p", "xorb %%ch, %%ah")

typedef uint64_t (*fn)(uint32_t, uint32_t);
static const struct { const char *name; fn f; } ops[] = {
#define E(n) { #n, n },
    E(add_hh) E(add_hl) E(add_lh) E(add_hi) E(add_hm) E(add_mh) E(sub_hh) E(sub_hl) E(sub_lh) E(sub_hi) E(sub_hm)
    E(sub_mh) E(and_hh) E(and_hl) E(and_lh) E(and_hi) E(and_hm) E(and_mh) E(or_hh) E(or_hl) E(or_lh) E(or_hi)
    E(or_hm) E(or_mh) E(xor_hh) E(xor_hl) E(xor_lh) E(xor_hi) E(xor_hm) E(xor_mh) E(cmp_hh) E(cmp_hi) E(cmp_hm)
    E(cmp_mh) E(cmp_lh) E(test_hh) E(test_hi) E(test_mh) E(cmp_b) E(cmp_l) E(cmp_e) E(test_z) E(test_s) E(and_z)
    E(add_c) E(sub_o) E(xor_p)
#undef E
};

int main(void)
{
    static const uint32_t vals[] = { 0x00000000u, 0xffffffffu, 0x12348000u, 0x00ff7f80u, 0x80017f01u,
                                     0x00004040u, 0x7f80ff7fu, 0xdeadbeefu };
    const unsigned nv = sizeof vals / sizeof vals[0];
    for (unsigned k = 0; k < sizeof ops / sizeof ops[0]; k++) {
        uint64_t h = 0;
        for (unsigned i = 0; i < nv; i++)
            for (unsigned j = 0; j < nv; j++)
                h = h * 1000003u ^ ops[k].f(vals[i], vals[j]);
        printf("%-8s %016llx %016llx\n", ops[k].name, (unsigned long long)ops[k].f(vals[3], vals[4]),
               (unsigned long long)h);
    }
    return 0;
}

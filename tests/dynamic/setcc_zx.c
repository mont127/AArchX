/*
 * setcc r8 widened by a later movzx of the same register, which src/jit.c
 * emits as one cset into the whole register (setcc_zx_partner): adjacent,
 * across sibling setcc and register moves, to 32 and 64 bits, every
 * condition; and the shapes it must leave alone, where something between
 * reads the register's other bits or the movzx widens another register.
 * Prints each, against Rosetta.
 */
#include <stdint.h>
#include <stdio.h>

#define CCS(X) X(o) X(no) X(b) X(ae) X(e) X(ne) X(be) X(a) X(s) X(ns) X(p) X(np) X(l) X(ge) X(le) X(g)

#define ZX(cc)                                                                                 \
    static void zx_##cc(uint64_t x, uint64_t y, uint64_t out[6])                               \
    {                                                                                          \
        uint64_t a = 0xdeadbeefcafef00dull, c = 0x1111222233334444ull, d = 0x5555666677778888ull; \
        __asm__ volatile(                                                                      \
            "cmp %[y], %[x]\n\t"                                                               \
            "set" #cc " %b[a]\n\t" "movzbl %b[a], %k[a]\n\t"                                   \
            "mov %[a], 0(%[o])\n\t"                                                            \
            "mov $0xdeadbeefcafef00d, %[a]\n\t"                                                \
            "cmp %[y], %[x]\n\t"                                                               \
            "set" #cc " %b[a]\n\t" "setl %b[d]\n\t" "mov %[x], %[c]\n\t"                       \
            "movzbl %b[d], %k[d]\n\t" "movzbq %b[a], %[a]\n\t"                                 \
            "mov %[a], 8(%[o])\n\t" "mov %[d], 16(%[o])\n\t"                                   \
            "mov $0xdeadbeefcafef00d, %[a]\n\t"                                                \
            "cmp %[y], %[x]\n\t"                                                               \
            "set" #cc " %b[a]\n\t" "mov %[a], %[c]\n\t" "movzbl %b[a], %k[a]\n\t"              \
            "mov %[c], 24(%[o])\n\t"                                                           \
            "mov $0xdeadbeefcafef00d, %[a]\n\t"                                                \
            "cmp %[y], %[x]\n\t"                                                               \
            "set" #cc " %b[a]\n\t" "movzbl %b[a], %k[c]\n\t"                                   \
            "mov %[a], 32(%[o])\n\t" "mov %[c], 40(%[o])"                                      \
            : [a] "+&r"(a), [c] "+&r"(c), [d] "+&r"(d)                                         \
            : [x] "r"(x), [y] "r"(y), [o] "r"(out)                                             \
            : "cc", "memory");                                                                 \
    }
CCS(ZX)

typedef void (*zfn)(uint64_t, uint64_t, uint64_t *);
#define ADDR(cc) zx_##cc,
static const zfn fns[] = { CCS(ADDR) };
#define NAME(cc) #cc,
static const char *const names[] = { CCS(NAME) };

int main(void)
{
    static const uint64_t v[] = { 0, 1, 7, 0x80, 0x7fffffffffffffffull, 0x8000000000000000ull,
                                  0xffffffffffffffffull, 0x123456789ull };
    for (unsigned f = 0; f < 16; f++) {
        unsigned long long acc = 0;
        for (int pass = 0; pass < 30; pass++)
            for (unsigned i = 0; i < 8; i++)
                for (unsigned j = 0; j < 8; j++) {
                    uint64_t out[6];
                    fns[f](v[i], v[j], out);
                    if (pass == 0)
                        for (int k = 0; k < 6; k++) acc = acc * 1000003 + out[k];
                    if (pass == 0 && i == 2 && j == 5)
                        printf("%s sample %llx %llx %llx %llx %llx %llx\n", names[f], (unsigned long long)out[0],
                               (unsigned long long)out[1], (unsigned long long)out[2], (unsigned long long)out[3],
                               (unsigned long long)out[4], (unsigned long long)out[5]);
                }
        printf("%s acc %016llx\n", names[f], acc);
    }
    return 0;
}

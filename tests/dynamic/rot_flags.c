/*
 * The flags around ROL and ROR by an immediate count, which src/flags_live.c
 * models as writing CF and OF (OF with the count-1 formula for every count, as
 * x86 hardware does) and passing the other flags through, so the instruction
 * before a rotate keeps its ZF, SF, PF and AF.  Each case is an add, sub or xor
 * that sets known flags, rotates by 0, 1 and larger counts in both directions
 * and in 8, 16, 32 and 64 bits (rol ax, 8 is a byte swap), then reads the flags
 * with pushf, a jcc and a setcc, the three ways translated code consumes them.
 * Also SHA-256's shape, a chain of rotates and xors after an add, whose flags
 * nothing reads.  Against Rosetta.
 */
#include <stdint.h>
#include <stdio.h>

#define FLAGS_MASK 0x8d5ull /* OF SF ZF AF PF CF */

#define ROT_CASE(name, setup, rot)                                                   \
    static void name(uint64_t a, uint64_t b, uint64_t *out)                          \
    {                                                                                \
        uint64_t fl, v, jz, seto;                                                    \
        __asm__ volatile(                                                            \
            "mov %4, %%rax\n\t"                                                      \
            setup "\n\t"                                                             \
            rot "\n\t"                                                               \
            "pushfq\n\t" "pop %0\n\t"                                                \
            "mov %%rax, %1\n\t"                                                      \
            setup "\n\t"                                                             \
            rot "\n\t"                                                               \
            "mov $0, %2\n\t"                                                         \
            "jnz 1f\n\t" "mov $1, %2\n\t" "1:\n\t"                                   \
            setup "\n\t"                                                             \
            rot "\n\t"                                                               \
            "seto %b3\n\t" "movzbl %b3, %k3\n\t"                                     \
            : "=&r"(fl), "=&r"(v), "=&r"(jz), "=&q"(seto)                            \
            : "r"(a), "r"(b) : "rax", "rdx", "cc");                                  \
        out[0] = fl & FLAGS_MASK; out[1] = v; out[2] = jz; out[3] = seto;            \
    }

ROT_CASE(c_ror1_add32, "add %k5, %%eax", "ror $1, %%eax")
ROT_CASE(c_ror9_add32, "add %k5, %%eax", "ror $9, %%eax")
ROT_CASE(c_rol1_sub64, "sub %5, %%rax", "rol $1, %%rax")
ROT_CASE(c_rol17_sub64, "sub %5, %%rax", "rol $17, %%rax")
ROT_CASE(c_ror0_add32, "add %k5, %%eax", "ror $0, %%eax")
ROT_CASE(c_ror32_add32, "add %k5, %%eax", "ror $32, %%eax")
ROT_CASE(c_ror64_sub64, "sub %5, %%rax", "ror $64, %%rax")
ROT_CASE(c_rol8_add8, "add %b5, %%al", "rol $8, %%al")
ROT_CASE(c_ror3_add16, "add %w5, %%ax", "ror $3, %%ax")
ROT_CASE(c_two_rots, "add %k5, %%eax", "ror $14, %%eax\n\tror $1, %%eax")
ROT_CASE(c_rol8_add16, "add %w5, %%ax", "rol $8, %%ax")
ROT_CASE(c_ror1_sub16, "sub %w5, %%ax", "ror $1, %%ax")
ROT_CASE(c_rol16_add16, "add %w5, %%ax", "rol $16, %%ax")
ROT_CASE(c_ror17_add16, "add %w5, %%ax", "ror $17, %%ax")
ROT_CASE(c_rol5_xor16, "xor %w5, %%ax", "rol $5, %%ax")

typedef void (*case_fn)(uint64_t, uint64_t, uint64_t *);

static uint32_t sha_chain(uint32_t x, uint32_t y, int n)
{
    for (int i = 0; i < n; i++) {
        __asm__ volatile(
            "add %1, %0\n\t"
            "mov %0, %%edx\n\t" "ror $6, %%edx\n\t" "mov %0, %%ecx\n\t" "ror $11, %%ecx\n\t"
            "xor %%ecx, %%edx\n\t" "ror $25, %0\n\t" "xor %%edx, %0\n\t"
            : "+r"(x) : "r"(y) : "rcx", "rdx", "cc");
        y = y * 2654435761u + (uint32_t)i;
    }
    return x;
}

int main(void)
{
    static const struct { const char *name; case_fn f; } cases[] = {
        {"ror1_add32", c_ror1_add32}, {"ror9_add32", c_ror9_add32}, {"rol1_sub64", c_rol1_sub64},
        {"rol17_sub64", c_rol17_sub64}, {"ror0_add32", c_ror0_add32}, {"ror32_add32", c_ror32_add32},
        {"ror64_sub64", c_ror64_sub64}, {"rol8_add8", c_rol8_add8}, {"ror3_add16", c_ror3_add16},
        {"two_rots", c_two_rots},
        {"rol8_add16", c_rol8_add16}, {"ror1_sub16", c_ror1_sub16}, {"rol16_add16", c_rol16_add16},
        {"ror17_add16", c_ror17_add16}, {"rol5_xor16", c_rol5_xor16},
    };
    static const uint64_t in[][2] = {
        {0, 0}, {1, 0xffffffff}, {0x7fffffff, 1}, {0x80000000, 0x80000000}, {0x0f, 0x01},
        {0x8000000000000000ull, 1}, {5, 7}, {0x40000000, 0x40000000}, {0xfe, 0x02}, {0x7fff, 1},
    };
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        printf("%s", cases[c].name);
        for (unsigned k = 0; k < sizeof in / sizeof in[0]; k++) {
            uint64_t o[4];
            cases[c].f(in[k][0], in[k][1], o);
            printf(" %llx/%llx/%llu%llu", (unsigned long long)o[0], (unsigned long long)o[1],
                   (unsigned long long)o[2], (unsigned long long)o[3]);
        }
        printf("\n");
    }
    printf("sha %08x\n", sha_chain(0x6a09e667u, 0xbb67ae85u, 100000));
    return 0;
}

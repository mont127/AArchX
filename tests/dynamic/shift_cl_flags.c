/*
 * Shifts by cl whose flags are read, and stores of ah, ch, dh and bh, which
 * src/jit.c translates.  A shift by cl leaves every flag as it was when the
 * masked count is 0 and sets them as a shift by an immediate does otherwise,
 * so each case sets flags first with a sub (lazily, as translated code keeps
 * them) and then shifts: by 0, 1, 2, the operand width and past it, in 32 and
 * 64 bits, shl, shr and sar.  The flags are read with pushf, a jcc, a setcc
 * and an adc.  OF is printed only for a count of 1 and AF never, since x86
 * leaves them undefined otherwise.  A 32-bit shift or rotate by a count that
 * masks to 0 still clears the register's upper half, by cl or by an
 * immediate.  Then the four high-byte registers are stored over a buffer,
 * through a base and through a scaled index, and the bytes around each must
 * stay as they were.  Against Rosetta.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SHIFT_CASE(name, op, reg)                                                    \
    static void name(uint64_t v, uint64_t a, uint64_t b, uint8_t cnt, uint64_t out[5]) \
    {                                                                                \
        uint64_t fl, r, jz, sc, ad;                                                  \
        __asm__ volatile(                                                            \
            "sub $128, %%rsp\n\t"                                                    \
            "mov %5, %%rax\n\t" "mov %8, %%cl\n\t"                                   \
            "mov %6, %%rdx\n\t" "sub %7, %%rdx\n\t"                                  \
            op " %%cl, %" reg "\n\t"                                                 \
            "pushfq\n\t" "pop %0\n\t"                                                \
            "mov %%rax, %1\n\t"                                                      \
            "mov %5, %%rax\n\t"                                                      \
            "mov %6, %%rdx\n\t" "sub %7, %%rdx\n\t"                                  \
            op " %%cl, %" reg "\n\t"                                                 \
            "mov $0, %2\n\t" "jnz 1f\n\t" "mov $1, %2\n\t" "1:\n\t"                  \
            "mov %5, %%rax\n\t"                                                      \
            "mov %6, %%rdx\n\t" "sub %7, %%rdx\n\t"                                  \
            op " %%cl, %" reg "\n\t"                                                 \
            "setl %b3\n\t" "movzbl %b3, %k3\n\t"                                     \
            "mov %5, %%rax\n\t"                                                      \
            "mov %6, %%rdx\n\t" "sub %7, %%rdx\n\t"                                  \
            op " %%cl, %" reg "\n\t"                                                 \
            "mov $0, %4\n\t" "adc $0, %4\n\t"                                        \
            "add $128, %%rsp\n\t"                                                    \
            : "=&r"(fl), "=&r"(r), "=&r"(jz), "=&q"(sc), "=&r"(ad)                   \
            : "r"(v), "r"(a), "r"(b), "r"(cnt) : "rax", "rcx", "rdx", "cc");         \
        out[0] = fl; out[1] = r; out[2] = jz; out[3] = sc; out[4] = ad;              \
    }

SHIFT_CASE(c_shl64, "shl", "%rax")
SHIFT_CASE(c_shr64, "shr", "%rax")
SHIFT_CASE(c_sar64, "sar", "%rax")
SHIFT_CASE(c_shl32, "shl", "%eax")
SHIFT_CASE(c_shr32, "shr", "%eax")
SHIFT_CASE(c_sar32, "sar", "%eax")

typedef void (*case_fn)(uint64_t, uint64_t, uint64_t, uint8_t, uint64_t *);

#define ZERO_IMM(name, ins) \
    static uint64_t name(uint64_t v) { __asm__ volatile(ins : "+r"(v) : : "cc"); return v; }
#define ZERO_CL(name, ins) \
    static uint64_t name(uint64_t v, uint8_t c) \
    { __asm__ volatile("mov %1, %%cl\n\t" ins " %%cl, %k0" : "+r"(v) : "r"(c) : "rcx", "cc"); return v; }
ZERO_IMM(z_shl, "shl $0, %k0") ZERO_IMM(z_shr, "shr $32, %k0") ZERO_IMM(z_sar, "sar $0, %k0")
ZERO_IMM(z_rol, "rol $0, %k0") ZERO_IMM(z_ror, "ror $32, %k0") ZERO_IMM(z_rcl, "rcl $0, %k0")
ZERO_CL(zc_rol, "rol") ZERO_CL(zc_ror, "ror") ZERO_CL(zc_rcr, "rcr")

static void store_high(uint8_t *buf, uint64_t ax, uint64_t bx, uint64_t cx, uint64_t dx, uint64_t idx)
{
    __asm__ volatile(
        "mov %1, %%rax\n\t" "mov %2, %%rbx\n\t" "mov %3, %%rcx\n\t" "mov %4, %%rdx\n\t"
        "mov %%ah, 1(%0)\n\t" "mov %%bh, 5(%0)\n\t" "mov %%ch, 9(%0)\n\t" "mov %%dh, 13(%0)\n\t"
        "mov %%dh, 16(%0,%5,1)\n\t" "mov %%ah, 30(%0,%5,2)\n\t"
        : : "S"(buf), "r"(ax), "r"(bx), "r"(cx), "r"(dx), "D"(idx) : "rax", "rbx", "rcx", "rdx", "memory");
}

int main(void)
{
    static const struct { const char *name; case_fn f; int bits; } cases[] = {
        {"shl64", c_shl64, 64}, {"shr64", c_shr64, 64}, {"sar64", c_sar64, 64},
        {"shl32", c_shl32, 32}, {"shr32", c_shr32, 32}, {"sar32", c_sar32, 32},
    };
    static const uint64_t vals[] = { 0, 1, 0x80000000ull, 0x8000000000000001ull, 0xfedcba9876543210ull, 0x40 };
    static const uint64_t subs[][2] = { {5, 5}, {3, 7}, {0x8000000000000000ull, 1}, {9, 2} };
    static const uint8_t cnts[] = { 0, 1, 2, 7, 31, 32, 33, 63, 64, 65, 255 };
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        for (unsigned k = 0; k < sizeof cnts / sizeof cnts[0]; k++) {
            unsigned masked = cnts[k] & (cases[c].bits == 64 ? 63u : 31u);
            uint64_t mask = 0x8c5ull & ~(masked == 1 ? 0ull : 0x800ull);
            printf("%s cl=%u", cases[c].name, cnts[k]);
            for (unsigned v = 0; v < sizeof vals / sizeof vals[0]; v++)
                for (unsigned s = 0; s < sizeof subs / sizeof subs[0]; s++) {
                    uint64_t o[5];
                    cases[c].f(vals[v], subs[s][0], subs[s][1], cnts[k], o);
                    printf(" %llx/%llx/%llu%llu%llu", (unsigned long long)(o[0] & mask), (unsigned long long)o[1],
                           (unsigned long long)o[2], (unsigned long long)o[3], (unsigned long long)o[4]);
                }
            printf("\n");
        }
    }
    uint64_t v = 0x8000000000000001ull;
    printf("zero counts %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx\n", (unsigned long long)z_shl(v),
           (unsigned long long)z_shr(v), (unsigned long long)z_sar(v), (unsigned long long)z_rol(v),
           (unsigned long long)z_ror(v), (unsigned long long)z_rcl(v), (unsigned long long)zc_rol(v, 0),
           (unsigned long long)zc_ror(v, 32), (unsigned long long)zc_rcr(v, 0), (unsigned long long)zc_rol(v, 1));
    uint8_t buf[64];
    memset(buf, 0x5a, sizeof buf);
    store_high(buf, 0x1122334455667788ull, 0xa1b2c3d4e5f60718ull, 0x0000000000000a0bull, 0xffffffffffff99eeull, 3);
    printf("high8");
    for (unsigned i = 0; i < 48; i++)
        printf(" %02x", buf[i]);
    printf("\n");
    return 0;
}

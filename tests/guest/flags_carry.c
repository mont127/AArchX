/* Carry-chain gate for lazy flag emission. */
#include "gsys.h"

static g_u64 mix(g_u64 acc, g_u64 v)
{
    return acc * 1000003u + v;
}

static g_u64 t_cf_through_incdec(g_u64 a, g_u64 b, g_u64 c, g_u64 d)
{
    g_u64 x = a, y = c, z = d, acc = 0;
    __asm__ volatile(
        "addq %[b], %[x]\n\t"
        "incq %[y]\n\t"
        "decq %[z]\n\t"
        "adcq $0, %[acc]\n\t"
        : [x] "+r"(x), [y] "+r"(y), [z] "+r"(z), [acc] "+r"(acc)
        : [b] "r"(b)
        : "cc");
    return x * 7 + y * 11 + z * 13 + acc * 17;
}

static g_u64 t_cf_through_incdec_sbb(g_u64 a, g_u64 b, g_u64 c)
{
    g_u64 x = a, y = c, acc = 0;
    __asm__ volatile(
        "subq %[b], %[x]\n\t"
        "incq %[y]\n\t"
        "sbbq $0, %[acc]\n\t"
        : [x] "+r"(x), [y] "+r"(y), [acc] "+r"(acc)
        : [b] "r"(b)
        : "cc");
    return x * 3 + y * 5 + acc * 19;
}

static g_u64 t_logic_kills_cf(g_u64 a, g_u64 b)
{
    g_u64 x = a, t = 0, acc = 0;
    __asm__ volatile(
        "addq %[b], %[x]\n\t"
        "xorq %[t], %[t]\n\t"
        "adcq $0, %[acc]\n\t"
        : [x] "+r"(x), [t] "+r"(t), [acc] "+r"(acc)
        : [b] "r"(b)
        : "cc");
    return x * 3 + t * 5 + acc * 23;
}

static g_u64 t_flagfree_passthrough(g_u64 a, g_u64 b, g_u64 c)
{
    g_u64 x = a, y = c, acc = 0, w = 0;
    __asm__ volatile(
        "addq %[b], %[x]\n\t"
        "notq %[y]\n\t"
        "bswapq %[y]\n\t"
        "leaq 1(%[y]), %[y]\n\t"
        "movzbl %b[y], %k[w]\n\t"
        "adcq $0, %[acc]\n\t"
        : [x] "+r"(x), [y] "+r"(y), [acc] "+r"(acc), [w] "=&r"(w)
        : [b] "r"(b)
        : "cc");
    return x * 3 + y * 5 + acc * 29 + w * 31;
}

static g_u64 t_neg_defines_cf(g_u64 a, g_u64 b, g_u64 c)
{
    g_u64 x = a, y = c, acc = 0;
    __asm__ volatile(
        "addq %[b], %[x]\n\t"
        "negq %[y]\n\t"
        "adcq $0, %[acc]\n\t"
        : [x] "+r"(x), [y] "+r"(y), [acc] "+r"(acc)
        : [b] "r"(b)
        : "cc");
    return x * 3 + y * 5 + acc * 37;
}

static g_u64 t_shift_zero(g_u64 a, g_u64 b, g_u64 c)
{
    g_u64 x = c, z = 0;
    __asm__ volatile(
        "cmpq %[b], %[a]\n\t"
        "shlq $0, %[x]\n\t"
        "shrq $0, %[x]\n\t"
        "sarq $0, %[x]\n\t"
        "setz %%cl\n\t"
        "movzbq %%cl, %[z]\n\t"
        : [x] "+r"(x), [z] "=&r"(z)
        : [a] "r"(a), [b] "r"(b)
        : "cc", "rcx");
    return x * 3 + z * 41;
}

static g_u64 t_shift_defines(g_u64 a, g_u64 b)
{
    g_u64 x = a, y = b, acc = 0;
    __asm__ volatile(
        "addq %[y], %[x]\n\t"
        "shlq $3, %[y]\n\t"
        "adcq $0, %[acc]\n\t"
        : [x] "+r"(x), [y] "+r"(y), [acc] "+r"(acc)
        :
        : "cc");
    return x * 3 + y * 5 + acc * 43;
}

static g_u64 t_adc_chain(g_u64 a0, g_u64 a1, g_u64 a2, g_u64 b0, g_u64 b1, g_u64 b2)
{
    g_u64 r0 = a0, r1 = a1, r2 = a2;
    __asm__ volatile(
        "addq %[b0], %[r0]\n\t"
        "adcq %[b1], %[r1]\n\t"
        "adcq %[b2], %[r2]\n\t"
        : [r0] "+r"(r0), [r1] "+r"(r1), [r2] "+r"(r2)
        : [b0] "r"(b0), [b1] "r"(b1), [b2] "r"(b2)
        : "cc");
    return r0 * 3 + r1 * 5 + r2 * 7;
}

static g_u64 t_sbb_chain(g_u64 a0, g_u64 a1, g_u64 a2, g_u64 b0, g_u64 b1, g_u64 b2)
{
    g_u64 r0 = a0, r1 = a1, r2 = a2;
    __asm__ volatile(
        "subq %[b0], %[r0]\n\t"
        "sbbq %[b1], %[r1]\n\t"
        "sbbq %[b2], %[r2]\n\t"
        : [r0] "+r"(r0), [r1] "+r"(r1), [r2] "+r"(r2)
        : [b0] "r"(b0), [b1] "r"(b1), [b2] "r"(b2)
        : "cc");
    return r0 * 3 + r1 * 5 + r2 * 7;
}

static g_u64 t_inc_of_add_cf(g_u64 a, g_u64 b, g_u64 c)
{
    g_u64 x = a, y = c, acc = 0, o = 0;
    __asm__ volatile(
        "addq %[b], %[x]\n\t"
        "incq %[y]\n\t"
        "adcq $0, %[acc]\n\t"
        "seto %%cl\n\t"
        "movzbq %%cl, %[o]\n\t"
        : [x] "+r"(x), [y] "+r"(y), [acc] "+r"(acc), [o] "=&r"(o)
        : [b] "r"(b)
        : "cc", "rcx");
    return x * 3 + y * 5 + acc * 47 + o * 53;
}

static g_u64 t_cf_through_incdec32(g_u32 a, g_u32 b, g_u32 c)
{
    g_u32 x = a, y = c, acc = 0;
    __asm__ volatile(
        "addl %[b], %[x]\n\t"
        "incl %[y]\n\t"
        "decl %[y]\n\t"
        "adcl $0, %[acc]\n\t"
        : [x] "+r"(x), [y] "+r"(y), [acc] "+r"(acc)
        : [b] "r"(b)
        : "cc");
    return (g_u64)x * 3 + (g_u64)y * 5 + (g_u64)acc * 59;
}

static g_u64 t_lengthcap_liveout(g_u64 a, g_u64 b)
{
    g_u64 z = 0, c = 0;
    __asm__ volatile(

        "jmp 1f\n"
        "1:\n\t"
        ".rept 255\n\t"
        "nop\n\t"
        ".endr\n\t"

        "cmpq %[b], %[a]\n\t"

        "setz %%cl\n\t"
        "movzbq %%cl, %[z]\n\t"
        "adcq $0, %[c]\n\t"
        : [z] "=&r"(z), [c] "+r"(c)
        : [a] "r"(a), [b] "r"(b)
        : "cc", "rcx");
    return z * 61 + c * 67;
}

int main(void)
{

    static const g_u64 vals[] = {
        0, 1, 2, 0xf, 0x10, 0x7f, 0x80,
        0x7fffffffULL, 0x80000000ULL, 0xffffffffULL,
        0x100000000ULL,
        0x7fffffffffffffffULL, 0x8000000000000000ULL,
        0xfffffffffffffffeULL, 0xffffffffffffffffULL,
    };
    const g_u64 nv = sizeof(vals) / sizeof(vals[0]);

    g_u64 acc = 0;

    for (g_u64 i = 0; i < nv; i++) {
        for (g_u64 j = 0; j < nv; j++) {
            g_u64 a = vals[i], b = vals[j];
            g_u64 c = vals[(i + j) % nv];
            g_u64 d = vals[(i + 2 * j) % nv];

            acc = mix(acc, t_cf_through_incdec(a, b, c, d));
            acc = mix(acc, t_cf_through_incdec_sbb(a, b, c));
            acc = mix(acc, t_logic_kills_cf(a, b));
            acc = mix(acc, t_flagfree_passthrough(a, b, c));
            acc = mix(acc, t_neg_defines_cf(a, b, c));
            acc = mix(acc, t_shift_zero(a, b, c));
            acc = mix(acc, t_shift_defines(a, b));
            acc = mix(acc, t_inc_of_add_cf(a, b, c));
            acc = mix(acc, t_adc_chain(a, b, c, d, a, b));
            acc = mix(acc, t_sbb_chain(a, b, c, d, a, b));
            acc = mix(acc, t_cf_through_incdec32((g_u32)a, (g_u32)b, (g_u32)c));
            acc = mix(acc, t_lengthcap_liveout(a, b));
        }
    }

    g_putu64(acc);
    return 0;
}

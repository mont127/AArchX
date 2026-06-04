/*
 * src/flags.c
 *
 * Eager RFLAGS computation — the reference implementation of x86 flag
 * semantics for all of Ocerz (the JIT must match it bit for bit).
 *
 * Every helper rewrites only the six arithmetic flags (CF PF AF ZF SF OF)
 * and leaves control bits (DF IF TF, fixed bit 1) untouched. All inputs are
 * masked to the operation width internally. The carry-in variants fold ADC
 * and SBB into the add/sub helpers using the relations:
 *   add: carry-out  = res < a, or res == a when a carry-in was added
 *   sub: borrow-out = b > a,  or b == a when a borrow-in was subtracted
 * which hold in modular arithmetic at any width. Signed overflow uses the
 * classic XOR identities: for addition the operands agreed in sign and the
 * result disagrees (~(a^b) & (a^res)); for subtraction the operands
 * disagreed and the result took b's side ((a^b) & (a^res)). The adjust
 * flag is the half-carry out of bit 3: bit 4 of a^b^res. Parity covers the
 * low byte only and is set for an even bit count.
 *
 * INC/DEC preserve CF and compute OF positionally: INC overflows exactly
 * into the minimum signed value, DEC overflows exactly from it leaving the
 * maximum. Shift helpers implement the architectural carve-outs: callers
 * never invoke them with a zero count (count-0 shifts leave all flags
 * unchanged), CF is the last bit shifted out, OF is defined for count 1
 * only (SHL: CF^SF, SHR: original MSB, SAR: 0) but is computed for every
 * count to stay deterministic. MUL sets CF=OF when the high half is
 * nonzero; IMUL when the high half is not the sign-extension of the low
 * half. Architecturally-undefined flags after these (SF/ZF/PF, and AF
 * everywhere it is undefined) get fixed deterministic values so the
 * interpreter and JIT can be diffed bit-exactly.
 *
 * ocerz_cc_eval decodes the standard 4-bit condition table by pairing each
 * predicate with its negation through bit 0.
 */
#include "ocerz/flags.h"

#define OCERZ_ARITH_FLAGS (OCERZ_CF | OCERZ_PF | OCERZ_AF | OCERZ_ZF | OCERZ_SF | OCERZ_OF)

static uint64_t szp_bits(int size, uint64_t res)
{
    uint64_t f = 0;
    res &= ocerz_mask(size);
    if (res == 0)
        f |= OCERZ_ZF;
    if (ocerz_msb(res, size))
        f |= OCERZ_SF;
    if (!__builtin_parity((unsigned)(res & 0xff)))
        f |= OCERZ_PF;
    return f;
}

static void put_arith(OcerzCPU *cpu, uint64_t f)
{
    cpu->rflags = (cpu->rflags & ~(uint64_t)OCERZ_ARITH_FLAGS) | f;
}

void ocerz_flags_szp(OcerzCPU *cpu, int size, uint64_t res)
{
    cpu->rflags = (cpu->rflags & ~(uint64_t)(OCERZ_PF | OCERZ_ZF | OCERZ_SF)) | szp_bits(size, res);
}

void ocerz_flags_add(OcerzCPU *cpu, int size, uint64_t a, uint64_t b, int cin, uint64_t res)
{
    uint64_t m = ocerz_mask(size);
    a &= m;
    b &= m;
    res &= m;
    uint64_t f = szp_bits(size, res);
    if (res < a || (cin && res == a))
        f |= OCERZ_CF;
    if (ocerz_msb(~(a ^ b) & (a ^ res), size))
        f |= OCERZ_OF;
    if ((a ^ b ^ res) & 0x10)
        f |= OCERZ_AF;
    put_arith(cpu, f);
}

void ocerz_flags_sub(OcerzCPU *cpu, int size, uint64_t a, uint64_t b, int cin, uint64_t res)
{
    uint64_t m = ocerz_mask(size);
    a &= m;
    b &= m;
    res &= m;
    uint64_t f = szp_bits(size, res);
    if (b > a || (cin && b == a))
        f |= OCERZ_CF;
    if (ocerz_msb((a ^ b) & (a ^ res), size))
        f |= OCERZ_OF;
    if ((a ^ b ^ res) & 0x10)
        f |= OCERZ_AF;
    put_arith(cpu, f);
}

void ocerz_flags_logic(OcerzCPU *cpu, int size, uint64_t res)
{
    put_arith(cpu, szp_bits(size, res));
}

void ocerz_flags_inc(OcerzCPU *cpu, int size, uint64_t res)
{
    uint64_t m = ocerz_mask(size);
    res &= m;
    uint64_t f = szp_bits(size, res) | (cpu->rflags & OCERZ_CF);
    if (res == ((uint64_t)1 << (size * 8 - 1)))
        f |= OCERZ_OF;
    if ((res & 0xf) == 0)
        f |= OCERZ_AF;
    put_arith(cpu, f);
}

void ocerz_flags_dec(OcerzCPU *cpu, int size, uint64_t res)
{
    uint64_t m = ocerz_mask(size);
    res &= m;
    uint64_t f = szp_bits(size, res) | (cpu->rflags & OCERZ_CF);
    if (res == (m >> 1))
        f |= OCERZ_OF;
    if ((res & 0xf) == 0xf)
        f |= OCERZ_AF;
    put_arith(cpu, f);
}

void ocerz_flags_shl(OcerzCPU *cpu, int size, uint64_t val, unsigned cnt, uint64_t res)
{
    int bits = size * 8;
    val &= ocerz_mask(size);
    res &= ocerz_mask(size);
    uint64_t f = szp_bits(size, res);
    int cf = 0;
    if (cnt <= (unsigned)bits)
        cf = (int)((val >> (bits - cnt)) & 1);
    if (cf)
        f |= OCERZ_CF;
    if ((cf ^ ocerz_msb(res, size)) & 1)
        f |= OCERZ_OF;
    put_arith(cpu, f);
}

void ocerz_flags_shr(OcerzCPU *cpu, int size, uint64_t val, unsigned cnt, uint64_t res)
{
    int bits = size * 8;
    val &= ocerz_mask(size);
    res &= ocerz_mask(size);
    uint64_t f = szp_bits(size, res);
    if (cnt <= (unsigned)bits && ((val >> (cnt - 1)) & 1))
        f |= OCERZ_CF;
    if (ocerz_msb(val, size))
        f |= OCERZ_OF;
    put_arith(cpu, f);
}

void ocerz_flags_sar(OcerzCPU *cpu, int size, uint64_t val, unsigned cnt, uint64_t res)
{
    res &= ocerz_mask(size);
    int64_t sval = ocerz_sext(val, size);
    uint64_t f = szp_bits(size, res);
    unsigned shift = cnt - 1;
    if (shift > 63)
        shift = 63;
    if ((uint64_t)(sval >> shift) & 1)
        f |= OCERZ_CF;
    put_arith(cpu, f);
}

void ocerz_flags_mul(OcerzCPU *cpu, int size, uint64_t lo, uint64_t hi)
{
    uint64_t f = szp_bits(size, lo);
    if (hi & ocerz_mask(size))
        f |= OCERZ_CF | OCERZ_OF;
    put_arith(cpu, f);
}

void ocerz_flags_imul(OcerzCPU *cpu, int size, uint64_t lo, uint64_t hi)
{
    uint64_t m = ocerz_mask(size);
    uint64_t sign = ocerz_msb(lo, size) ? m : 0;
    uint64_t f = szp_bits(size, lo);
    if ((hi & m) != sign)
        f |= OCERZ_CF | OCERZ_OF;
    put_arith(cpu, f);
}

int ocerz_cc_eval(const OcerzCPU *cpu, unsigned cc)
{
    uint64_t f = cpu->rflags;
    int cf = (f & OCERZ_CF) != 0;
    int zf = (f & OCERZ_ZF) != 0;
    int sf = (f & OCERZ_SF) != 0;
    int of = (f & OCERZ_OF) != 0;
    int pf = (f & OCERZ_PF) != 0;
    int r;
    switch (cc >> 1) {
    case 0: r = of; break;
    case 1: r = cf; break;
    case 2: r = zf; break;
    case 3: r = cf || zf; break;
    case 4: r = sf; break;
    case 5: r = pf; break;
    case 6: r = sf != of; break;
    default: r = zf || sf != of; break;
    }
    if (cc & 1)
        r = !r;
    return r;
}

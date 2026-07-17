/*
 * include/ocerz/flags.h
 *
 * RFLAGS computation helpers, and THE semantic reference for x86 flags in this
 * emulator. The INTERPRETER keeps cpu->rflags architecturally current at every
 * instruction boundary: each flag-writing instruction calls exactly one of these
 * helpers, which rewrite the six arithmetic flag bits (CF PF AF ZF SF OF) while
 * preserving control bits (DF TF IF and the fixed bit 1). This trades a little
 * interpreter speed for zero lazy-flag bugs, and makes the interpreter the
 * reference the JIT is differentially held to.
 *
 * The JIT tier maps these onto arm64 NZCV natively AND omits the writes a
 * liveness pass proves dead (src/flags_live.c, include/ocerz/flags_live.h) --
 * but it must still match what these helpers would have produced at every
 * observable point. That equivalence is what tests/guest/flags.c (PUSHF per op)
 * and tests/guest/flags_live.c (cross-block consumers, and a handler reading
 * rflags out of a fault's mcontext) enforce through the differential.
 *
 * All helpers take the operation width in BYTES (1,2,4,8) and operate on
 * values already masked or maskable to that width; they mask internally, so
 * passing full 64-bit registers is safe.
 *
 * Formulas implemented (size-masked throughout):
 *   add  (cin=0/1 folds ADC):  CF = res<a || (cin && res==a)
 *                              OF = (~(a^b) & (a^res)) >> msb
 *                              AF = (a^b^res) bit 4
 *   sub  (cin=0/1 folds SBB):  CF = b>a || (cin && b==a)   (borrow)
 *                              OF = ((a^b) & (a^res)) >> msb
 *   logic:                     CF=OF=AF=0
 *   inc/dec:                   like add/sub with b=1 but CF preserved
 *   shl:  CF = bit (width-cnt) of original (cnt<=width), OF(cnt==1)=CF^SF
 *   shr:  CF = bit (cnt-1) of original, OF(cnt==1) = original MSB
 *   sar:  CF = bit (cnt-1) of sign-extended original, OF(cnt==1) = 0
 *         (shift helpers must only be called with cnt != 0; count-0 shifts
 *         change no flags and the interpreter skips the call)
 *   mul:  CF=OF = high half != 0;             SF/ZF/PF from low half
 *   imul: CF=OF = wide result not equal to sign-extended low half
 * ZF/SF/PF always follow the result (PF covers the LOW BYTE only, set on
 * even parity). Flags the architecture leaves undefined are set to
 * deterministic values (the formulas above), which keeps differential
 * testing stable.
 *
 * ocerz_cc_eval evaluates an OcerzCC condition code against rflags.
 * ocerz_mask/ocerz_sext are the central width helpers used everywhere.
 */
#ifndef OCERZ_FLAGS_H
#define OCERZ_FLAGS_H

#include "ocerz/cpu.h"

static inline uint64_t ocerz_mask(int size)
{
    return size >= 8 ? ~(uint64_t)0 : (((uint64_t)1 << (size * 8)) - 1);
}

static inline uint64_t ocerz_trunc(uint64_t v, int size)
{
    return v & ocerz_mask(size);
}

static inline int64_t ocerz_sext(uint64_t v, int size)
{
    int shift = 64 - size * 8;
    return (int64_t)(v << shift) >> shift;
}

static inline int ocerz_msb(uint64_t v, int size)
{
    return (int)((v >> (size * 8 - 1)) & 1);
}

static inline void ocerz_flag_assign(OcerzCPU *cpu, uint64_t mask, int set)
{
    if (set)
        cpu->rflags |= mask;
    else
        cpu->rflags &= ~mask;
}

void ocerz_flags_szp(OcerzCPU *cpu, int size, uint64_t res);
void ocerz_flags_add(OcerzCPU *cpu, int size, uint64_t a, uint64_t b, int cin, uint64_t res);
void ocerz_flags_sub(OcerzCPU *cpu, int size, uint64_t a, uint64_t b, int cin, uint64_t res);
void ocerz_flags_logic(OcerzCPU *cpu, int size, uint64_t res);
void ocerz_flags_inc(OcerzCPU *cpu, int size, uint64_t res);
void ocerz_flags_dec(OcerzCPU *cpu, int size, uint64_t res);
void ocerz_flags_shl(OcerzCPU *cpu, int size, uint64_t val, unsigned cnt, uint64_t res);
void ocerz_flags_shr(OcerzCPU *cpu, int size, uint64_t val, unsigned cnt, uint64_t res);
void ocerz_flags_sar(OcerzCPU *cpu, int size, uint64_t val, unsigned cnt, uint64_t res);
void ocerz_flags_mul(OcerzCPU *cpu, int size, uint64_t lo, uint64_t hi);
void ocerz_flags_imul(OcerzCPU *cpu, int size, uint64_t lo, uint64_t hi);
int ocerz_cc_eval(const OcerzCPU *cpu, unsigned cc);

#endif

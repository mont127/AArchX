/* RFLAGS helpers, and the semantic reference for x86 flags. */
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

enum {
    OCERZ_CC_NONE = 0,
    OCERZ_CC_ADD,
    OCERZ_CC_SUB,
    OCERZ_CC_LOGIC,
    OCERZ_CC_INC,
    OCERZ_CC_DEC,
    OCERZ_CC_SHL,
    OCERZ_CC_SHR,
    OCERZ_CC_SAR,
    OCERZ_CC_MUL,
    OCERZ_CC_IMUL,
};

static inline uint32_t ocerz_cc_pack(unsigned kind, int size, int cin)
{
    return (uint32_t)kind | ((uint32_t)size << 8) | ((uint32_t)(cin & 1) << 16);
}

void ocerz_flags_materialize(OcerzCPU *cpu);

#endif

/* Shared inline operand machinery for the interpreter translation units. */
#ifndef OCERZ_INTERP_COMMON_H
#define OCERZ_INTERP_COMMON_H

#include "ocerz/cpu.h"
#include "ocerz/decode.h"
#include "ocerz/flags.h"
#include "ocerz/mem.h"
#include "ocerz/interp.h"

static inline uint64_t ocerz_ea(const OcerzCPU *cpu, const X86Insn *insn, const X86Operand *op)
{
    uint64_t a;
    if (op->riprel) {
        a = (uint64_t)op->disp;
    } else {
        a = (uint64_t)op->disp;
        if (op->base != OCERZ_REG_NONE)
            a += cpu->gpr[op->base];
        if (op->index != OCERZ_REG_NONE)
            a += cpu->gpr[op->index] << op->scale;
        if (insn->addrsize == 4)
            a = (uint32_t)a;
    }
    if (insn->seg == OCERZ_SEG_FS)
        a += cpu->fs_base;
    else if (insn->seg == OCERZ_SEG_GS)
        a += cpu->gs_base;
    return a;
}

static inline uint64_t ocerz_read_gpr(const OcerzCPU *cpu, unsigned reg, int size, int high8)
{
    uint64_t v = cpu->gpr[reg];
    if (high8)
        return (v >> 8) & 0xff;
    return ocerz_trunc(v, size);
}

static inline void ocerz_write_gpr(OcerzCPU *cpu, unsigned reg, int size, int high8, uint64_t v)
{
    if (high8) {
        cpu->gpr[reg] = (cpu->gpr[reg] & ~(uint64_t)0xff00) | ((v & 0xff) << 8);
        return;
    }
    switch (size) {
    case 1:
        cpu->gpr[reg] = (cpu->gpr[reg] & ~(uint64_t)0xff) | (v & 0xff);
        break;
    case 2:
        cpu->gpr[reg] = (cpu->gpr[reg] & ~(uint64_t)0xffff) | (v & 0xffff);
        break;
    case 4:
        cpu->gpr[reg] = (uint32_t)v;
        break;
    default:
        cpu->gpr[reg] = v;
        break;
    }
}

static inline uint64_t ocerz_read_op(OcerzCPU *cpu, const X86Insn *insn, const X86Operand *op)
{
    switch (op->kind) {
    case OCERZ_OPK_REG:
        return ocerz_read_gpr(cpu, op->reg, op->size, op->high8);
    case OCERZ_OPK_IMM:
        return ocerz_trunc(op->imm, op->size);
    case OCERZ_OPK_MEM:
        return ocerz_ld(ocerz_ea(cpu, insn, op), op->size);
    default:
        return 0;
    }
}

static inline void ocerz_write_op(OcerzCPU *cpu, const X86Insn *insn, const X86Operand *op, uint64_t v)
{
    if (op->kind == OCERZ_OPK_REG)
        ocerz_write_gpr(cpu, op->reg, op->size, op->high8, v);
    else
        ocerz_st(ocerz_ea(cpu, insn, op), op->size, v);
}

static inline Ocerz128 ocerz_read_op128(OcerzCPU *cpu, const X86Insn *insn, const X86Operand *op)
{
    if (op->kind == OCERZ_OPK_XMM)
        return cpu->xmm[op->reg];
    if (op->size == 16)
        return ocerz_ld128(ocerz_ea(cpu, insn, op));
    Ocerz128 v = { 0, 0 };
    v.lo = ocerz_ld(ocerz_ea(cpu, insn, op), op->size);
    return v;
}

static inline void ocerz_write_op128(OcerzCPU *cpu, const X86Insn *insn, const X86Operand *op, Ocerz128 v)
{
    if (op->kind == OCERZ_OPK_XMM) {
        cpu->xmm[op->reg] = v;
        return;
    }
    if (op->size == 16)
        ocerz_st128(ocerz_ea(cpu, insn, op), v);
    else
        ocerz_st(ocerz_ea(cpu, insn, op), op->size, v.lo);
}

static inline void ocerz_push(OcerzCPU *cpu, int size, uint64_t v)
{
    uint64_t sp = cpu->gpr[OCERZ_RSP] - (uint64_t)size;
    ocerz_st(sp, size, v);
    cpu->gpr[OCERZ_RSP] = sp;
}

static inline uint64_t ocerz_pop(OcerzCPU *cpu, int size)
{
    uint64_t v = ocerz_ld(cpu->gpr[OCERZ_RSP], size);
    cpu->gpr[OCERZ_RSP] += (uint64_t)size;
    return v;
}

int ocerz_unimpl(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn, const char *why);

#endif

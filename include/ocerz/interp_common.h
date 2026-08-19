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
        else if (insn->addrsize == 2)
            a = (uint16_t)a;   /* 32-bit mode + 0x67; addrsize is never 2 in long mode */
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

/* The stack pointer is RSP in long mode and ESP in i386 mode, so in 32-bit
 * mode every update wraps at 32 bits instead of 64.  The mode comes from the
 * decoded instruction, never from a test inside the emitted code.
 *
 * ocerz_push()/ocerz_pop() keep their exact 64-bit behaviour by delegating
 * with mode32 = 0; nothing that calls them today changes. */
static inline uint64_t ocerz_stack_wrap(uint64_t sp, int mode32)
{
    return mode32 ? (uint32_t)sp : sp;
}

static inline void ocerz_push_mode(OcerzCPU *cpu, int size, uint64_t v, int mode32)
{
    uint64_t sp = ocerz_stack_wrap(cpu->gpr[OCERZ_RSP] - (uint64_t)size, mode32);
    ocerz_st(sp, size, v);
    cpu->gpr[OCERZ_RSP] = sp;
}

static inline uint64_t ocerz_pop_mode(OcerzCPU *cpu, int size, int mode32)
{
    uint64_t v = ocerz_ld(cpu->gpr[OCERZ_RSP], size);
    cpu->gpr[OCERZ_RSP] = ocerz_stack_wrap(cpu->gpr[OCERZ_RSP] + (uint64_t)size, mode32);
    return v;
}

static inline void ocerz_push(OcerzCPU *cpu, int size, uint64_t v)
{
    ocerz_push_mode(cpu, size, v, 0);
}

static inline uint64_t ocerz_pop(OcerzCPU *cpu, int size)
{
    return ocerz_pop_mode(cpu, size, 0);
}

int ocerz_unimpl(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn, const char *why);

#endif

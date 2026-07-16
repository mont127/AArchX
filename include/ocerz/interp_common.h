/*
 * include/ocerz/interp_common.h
 *
 * Shared inline operand machinery for all interpreter translation units.
 * These helpers are the ONLY sanctioned way handlers read and write decoded
 * operands; they encode the architectural rules that are easy to get wrong:
 *
 *  - 32-bit register writes ZERO the upper 32 bits of the GPR (including
 *    CMOVcc with a false condition, which still writes its destination).
 *  - 8/16-bit register writes merge into the low bits, leaving the rest of
 *    the register untouched; high8 operands address bits 8..15.
 *  - Effective addresses wrap at 64 bits, truncate to 32 bits first when the
 *    address-size prefix was present, and add fs_base/gs_base afterwards.
 *    RIP-relative operands arrive pre-resolved with the absolute address in
 *    disp, so no special case is needed here.
 *  - Immediates were sign-extended by the decoder; ocerz_read_op only masks
 *    them to the operand size.
 *  - Stack pushes/pops move RSP by the operand size and use 64-bit slots in
 *    long mode (16-bit with a 66 prefix, which real code never uses but the
 *    helpers honor).
 *
 * ocerz_read_op returns the operand zero-extended to 64 bits; callers who
 * need a signed view apply ocerz_sext. ocerz_read_op128/write_op128 handle
 * XMM registers and 16-byte memory; scalar SSE forms read/write only the
 * low lanes via the sized variants.
 *
 * ocerz_unimpl prints a uniform unimplemented-instruction diagnostic (op
 * name, rip, raw bytes, CPU dump) and returns OCERZ_STEP_FATAL so call
 * sites can `return ocerz_unimpl(...)`.
 */
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

/* NOTE THE ORDER: the store happens BEFORE rsp is committed.
 *
 * On the non-faulting path this is indistinguishable from decrementing rsp
 * first -- the address stored to is identical. It matters only when the store
 * FAULTS, and then it is the difference between correct and silently wrong.
 *
 * A faulting x86 instruction is ABORTED: it has no architectural effect, so rsp
 * must be unchanged when the handler sees the fault, and re-executing the push
 * on return must push to the SAME address. Committing rsp first left it already
 * decremented, so the canonical fix-and-retry idiom -- which is exactly how a
 * guest grows a thread stack when a push hits the guard page -- would decrement
 * rsp a SECOND time on the retry, permanently shifting the stack and silently
 * leaving an 8-byte hole. (Reachable only now that a delivered fault rewinds rip
 * to the faulting instruction so the push is genuinely retried; see
 * OcerzCPU::cur_rip and crash_handler in src/vm.c.)
 *
 * ocerz_pop below loads before it increments, which makes the LOAD fault-safe --
 * but that is NOT sufficient on its own: a caller whose destination can also fault
 * (`pop qword [mem]`) must not let rsp commit before that store lands. op_stack's
 * POP/LEAVE therefore do the load themselves and commit rsp last; do not "simplify"
 * them back into ocerz_pop. The JIT's inlined push/pop reproduce this order exactly
 * (reg destinations only, so they cannot fault after the load) -- see emit_push_pop
 * in src/jit.c. */
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

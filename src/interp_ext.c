/*
 * Interpreter ops that are neither core integer nor SSE: string ops, CPUID,
 * CRC-32C and the rest of the long tail.
 *
 * String ops count in rCX and step rSI/rDI at the ADDRESS size - RCX/RSI/RDI at
 * 8, ECX/ESI/EDI at 4, CX/SI/DI at 2.  The 2 case is 32-bit mode with a 0x67
 * prefix and cannot arise in long mode, where addrsize is only ever 8 or 4.  A
 * 16-bit step writes back only the low half of the register, which is why it
 * goes through the register accessor rather than a direct store.
 *
 * CRC-32C is done bitwise: correctness over speed, since the JIT does not
 * translate it and hashing loops run interpreted anyway.  CPUID reports SSE3,
 * SSSE3, CX16, SSE4.1, SSE4.2 and POPCNT, all of which are implemented in full
 * - and Steam's bootstrapper refuses to start the client on a CPU without
 * SSE4.2.
 */
#include "ocerz/interp.h"
#include "ocerz/interp_common.h"
#include "ocerz/vm.h"
#include "ocerz/x87.h"

#include <fenv.h>
#include <math.h>
#include <stdlib.h>
#include <mach/mach_time.h>

static uint64_t ext_rcx_read(const OcerzCPU *cpu, const X86Insn *insn)
{
    if (insn->addrsize == 4)
        return (uint32_t)cpu->gpr[OCERZ_RCX];
    if (insn->addrsize == 2)
        return (uint16_t)cpu->gpr[OCERZ_RCX];
    return cpu->gpr[OCERZ_RCX];
}

static void ext_rcx_write(OcerzCPU *cpu, const X86Insn *insn, uint64_t v)
{
    if (insn->addrsize == 4)
        cpu->gpr[OCERZ_RCX] = (uint32_t)v;
    else if (insn->addrsize == 2)
        ocerz_write_gpr(cpu, OCERZ_RCX, 2, 0, v);
    else
        cpu->gpr[OCERZ_RCX] = v;
}

static uint64_t ext_ptr_read(const OcerzCPU *cpu, const X86Insn *insn, unsigned reg)
{
    if (insn->addrsize == 4)
        return (uint32_t)cpu->gpr[reg];
    if (insn->addrsize == 2)
        return (uint16_t)cpu->gpr[reg];
    return cpu->gpr[reg];
}

static void ext_ptr_write(OcerzCPU *cpu, const X86Insn *insn, unsigned reg, uint64_t v)
{
    if (insn->addrsize == 4)
        cpu->gpr[reg] = (uint32_t)v;
    else if (insn->addrsize == 2)
        ocerz_write_gpr(cpu, reg, 2, 0, v);
    else
        cpu->gpr[reg] = v;
}

static int ext_string(OcerzCPU *cpu, const X86Insn *insn)
{
    int size = insn->opsize;
    int64_t step = (cpu->rflags & OCERZ_DF) ? -(int64_t)size : (int64_t)size;
    int rep = insn->rep;
    int op = insn->op;

    if (rep != OCERZ_REP_NONE && ext_rcx_read(cpu, insn) == 0)
        return OCERZ_STEP_OK;

    for (;;) {
        uint64_t a = 0, b = 0;
        int did_cmp = 0;

        switch (op) {
        case OCERZ_OP_MOVS: {
            uint64_t s = ext_ptr_read(cpu, insn, OCERZ_RSI);
            uint64_t d = ext_ptr_read(cpu, insn, OCERZ_RDI);
            ocerz_st(d, size, ocerz_ld(s, size));
            ext_ptr_write(cpu, insn, OCERZ_RSI, s + (uint64_t)step);
            ext_ptr_write(cpu, insn, OCERZ_RDI, d + (uint64_t)step);
            break;
        }
        case OCERZ_OP_STOS: {
            uint64_t d = ext_ptr_read(cpu, insn, OCERZ_RDI);
            ocerz_st(d, size, ocerz_trunc(cpu->gpr[OCERZ_RAX], size));
            ext_ptr_write(cpu, insn, OCERZ_RDI, d + (uint64_t)step);
            break;
        }
        case OCERZ_OP_LODS: {
            uint64_t s = ext_ptr_read(cpu, insn, OCERZ_RSI);
            ocerz_write_gpr(cpu, OCERZ_RAX, size, 0, ocerz_ld(s, size));
            ext_ptr_write(cpu, insn, OCERZ_RSI, s + (uint64_t)step);
            break;
        }
        case OCERZ_OP_SCAS: {
            uint64_t d = ext_ptr_read(cpu, insn, OCERZ_RDI);
            a = ocerz_trunc(cpu->gpr[OCERZ_RAX], size);
            b = ocerz_ld(d, size);
            ext_ptr_write(cpu, insn, OCERZ_RDI, d + (uint64_t)step);
            did_cmp = 1;
            break;
        }
        case OCERZ_OP_CMPS: {
            uint64_t s = ext_ptr_read(cpu, insn, OCERZ_RSI);
            uint64_t d = ext_ptr_read(cpu, insn, OCERZ_RDI);
            a = ocerz_ld(s, size);
            b = ocerz_ld(d, size);
            ext_ptr_write(cpu, insn, OCERZ_RSI, s + (uint64_t)step);
            ext_ptr_write(cpu, insn, OCERZ_RDI, d + (uint64_t)step);
            did_cmp = 1;
            break;
        }
        default:
            return OCERZ_EUNSUP;
        }

        if (rep == OCERZ_REP_NONE) {
            if (did_cmp)
                ocerz_flags_sub(cpu, size, a, b, 0, a - b);
            return OCERZ_STEP_OK;
        }

        uint64_t cnt = ext_rcx_read(cpu, insn) - 1;
        ext_rcx_write(cpu, insn, cnt);

        if (did_cmp) {
            ocerz_flags_sub(cpu, size, a, b, 0, a - b);
            int zf = (cpu->rflags & OCERZ_ZF) != 0;
            if (rep == OCERZ_REP_REP && !zf)
                return OCERZ_STEP_OK;
            if (rep == OCERZ_REP_REPNE && zf)
                return OCERZ_STEP_OK;
        }

        if (cnt == 0)
            return OCERZ_STEP_OK;
    }
}

static int ext_bit(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *dst = &insn->ops[0];
    const X86Operand *off = &insn->ops[1];
    int size = dst->size;
    int op = insn->op;
    int testbit;

    if (dst->kind == OCERZ_OPK_REG) {
        unsigned bit = (unsigned)(ocerz_read_op(cpu, insn, off) & (size * 8 - 1));
        uint64_t val = ocerz_read_gpr(cpu, dst->reg, size, dst->high8);
        testbit = (int)((val >> bit) & 1);
        ocerz_flag_assign(cpu, OCERZ_CF, testbit);
        if (op != OCERZ_OP_BT) {
            uint64_t nv = val;
            if (op == OCERZ_OP_BTS)
                nv |= (uint64_t)1 << bit;
            else if (op == OCERZ_OP_BTR)
                nv &= ~((uint64_t)1 << bit);
            else
                nv ^= (uint64_t)1 << bit;
            ocerz_write_gpr(cpu, dst->reg, size, dst->high8, nv);
        }
        return OCERZ_STEP_OK;
    }

    uint64_t ea = ocerz_ea(cpu, insn, dst);
    uint64_t bit;
    if (off->kind == OCERZ_OPK_IMM) {
        bit = ocerz_read_op(cpu, insn, off) & (size * 8 - 1);
        ea = ea + (bit >> 3);
        bit = bit & 7;
    } else {
        int64_t sbit = ocerz_sext(ocerz_read_op(cpu, insn, off), off->size);
        ea = ea + (uint64_t)(sbit >> 3);
        bit = (uint64_t)sbit & 7;
    }

    if (op != OCERZ_OP_BT && insn->lock) {
        uint8_t *hp = (uint8_t *)ocerz_g2h(ea);
        uint8_t cur = __atomic_load_n(hp, __ATOMIC_SEQ_CST);
        for (;;) {
            uint8_t nv = cur;
            if (op == OCERZ_OP_BTS)
                nv |= (uint8_t)(1u << bit);
            else if (op == OCERZ_OP_BTR)
                nv &= (uint8_t)~(1u << bit);
            else
                nv ^= (uint8_t)(1u << bit);
            if (__atomic_compare_exchange_n(hp, &cur, nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                testbit = (int)((cur >> bit) & 1);
                break;
            }
        }
        ocerz_flag_assign(cpu, OCERZ_CF, testbit);
        if (ocerz_watch_addr && ocerz_watch_addr - ea < 1)
            ocerz_watch_hit(ea, 1, __atomic_load_n(hp, __ATOMIC_SEQ_CST), 0);
        return OCERZ_STEP_OK;
    }

    uint8_t byte = (uint8_t)ocerz_ld(ea, 1);
    testbit = (int)((byte >> bit) & 1);
    ocerz_flag_assign(cpu, OCERZ_CF, testbit);
    if (op != OCERZ_OP_BT) {
        if (op == OCERZ_OP_BTS)
            byte |= (uint8_t)(1u << bit);
        else if (op == OCERZ_OP_BTR)
            byte &= (uint8_t)~(1u << bit);
        else
            byte ^= (uint8_t)(1u << bit);
        ocerz_st(ea, 1, byte);
    }
    return OCERZ_STEP_OK;
}

static int ext_crc32(OcerzCPU *cpu, const X86Insn *insn)
{
    uint32_t crc = (uint32_t)cpu->gpr[insn->ops[0].reg];
    uint64_t v = ocerz_read_op(cpu, insn, &insn->ops[1]);
    for (int i = 0; i < insn->ops[1].size; i++) {
        crc ^= (uint8_t)(v >> (8 * i));
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0x82f63b78u & (uint32_t)-(int32_t)(crc & 1));
    }
    ocerz_write_op(cpu, insn, &insn->ops[0], crc);
    return OCERZ_STEP_OK;
}

static int ext_scan(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *dst = &insn->ops[0];
    const X86Operand *src = &insn->ops[1];
    int size = dst->size;
    int bits = size * 8;
    uint64_t s = ocerz_trunc(ocerz_read_op(cpu, insn, src), size);

    switch (insn->op) {
    case OCERZ_OP_BSF:
        if (s == 0) {
            ocerz_flag_assign(cpu, OCERZ_ZF, 1);
        } else {
            ocerz_flag_assign(cpu, OCERZ_ZF, 0);
            ocerz_write_op(cpu, insn, dst, (uint64_t)__builtin_ctzll(s));
        }
        return OCERZ_STEP_OK;
    case OCERZ_OP_BSR:
        if (s == 0) {
            ocerz_flag_assign(cpu, OCERZ_ZF, 1);
        } else {
            ocerz_flag_assign(cpu, OCERZ_ZF, 0);
            ocerz_write_op(cpu, insn, dst, (uint64_t)(63 - __builtin_clzll(s)));
        }
        return OCERZ_STEP_OK;
    case OCERZ_OP_POPCNT: {
        uint64_t r = (uint64_t)__builtin_popcountll(s);
        ocerz_write_op(cpu, insn, dst, r);
        cpu->rflags &= ~(uint64_t)(OCERZ_CF | OCERZ_OF | OCERZ_AF | OCERZ_SF | OCERZ_PF);
        ocerz_flag_assign(cpu, OCERZ_ZF, r == 0);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_TZCNT: {
        uint64_t r = (s == 0) ? (uint64_t)bits : (uint64_t)__builtin_ctzll(s);
        ocerz_write_op(cpu, insn, dst, r);
        ocerz_flag_assign(cpu, OCERZ_CF, s == 0);
        ocerz_flag_assign(cpu, OCERZ_ZF, r == 0);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_LZCNT: {
        uint64_t r = (s == 0) ? (uint64_t)bits : (uint64_t)(bits - 1 - (63 - __builtin_clzll(s)));
        ocerz_write_op(cpu, insn, dst, r);
        ocerz_flag_assign(cpu, OCERZ_CF, s == 0);
        ocerz_flag_assign(cpu, OCERZ_ZF, r == 0);
        return OCERZ_STEP_OK;
    }
    default:
        return OCERZ_EUNSUP;
    }
}

static uint64_t bmi_pdep(uint64_t v, uint64_t mask)
{
    uint64_t r = 0;
    for (uint64_t bit = 1; mask; mask &= mask - 1, bit <<= 1)
        if (v & bit)
            r |= mask & (0 - mask);
    return r;
}

static uint64_t bmi_pext(uint64_t v, uint64_t mask)
{
    uint64_t r = 0;
    for (uint64_t bit = 1; mask; mask &= mask - 1, bit <<= 1)
        if (v & mask & (0 - mask))
            r |= bit;
    return r;
}

static int ext_bmi(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *o0 = &insn->ops[0];
    const X86Operand *o1 = &insn->ops[1];
    const X86Operand *o2 = &insn->ops[2];
    int size = o0->size;
    unsigned bits = (unsigned)size * 8;
    uint64_t m = ocerz_mask(size);
    uint64_t a = ocerz_read_op(cpu, insn, o1) & m;
    uint64_t r;
    int cf = 0;

    switch (insn->op) {
    case OCERZ_OP_ANDN:
        r = ~a & ocerz_read_op(cpu, insn, o2) & m;
        break;
    case OCERZ_OP_BLSR:
        r = a & (a - 1);
        cf = a == 0;
        break;
    case OCERZ_OP_BLSMSK:
        r = (a ^ (a - 1)) & m;
        cf = a == 0;
        break;
    case OCERZ_OP_BLSI:
        r = a & (0 - a);
        cf = a != 0;
        break;
    case OCERZ_OP_BZHI: {
        unsigned n = (unsigned)(ocerz_read_op(cpu, insn, o2) & 0xff);
        r = n < bits ? a & ((1ull << n) - 1) : a;
        cf = n > bits - 1;
        break;
    }
    case OCERZ_OP_BEXTR: {
        uint64_t ctl = ocerz_read_op(cpu, insn, o2);
        unsigned start = (unsigned)(ctl & 0xff), len = (unsigned)((ctl >> 8) & 0xff);
        r = start < bits ? a >> start : 0;
        if (len < bits)
            r &= (1ull << len) - 1;
        ocerz_write_op(cpu, insn, o0, r);
        ocerz_flag_assign(cpu, OCERZ_ZF, r == 0);
        ocerz_flag_assign(cpu, OCERZ_CF, 0);
        ocerz_flag_assign(cpu, OCERZ_OF, 0);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_PDEP:
        ocerz_write_op(cpu, insn, o0, bmi_pdep(a, ocerz_read_op(cpu, insn, o2) & m));
        return OCERZ_STEP_OK;
    case OCERZ_OP_PEXT:
        ocerz_write_op(cpu, insn, o0, bmi_pext(a, ocerz_read_op(cpu, insn, o2) & m));
        return OCERZ_STEP_OK;
    case OCERZ_OP_MULX: {
        unsigned __int128 p = (unsigned __int128)(cpu->gpr[OCERZ_RDX] & m) * (ocerz_read_op(cpu, insn, o2) & m);
        ocerz_write_op(cpu, insn, o1, (uint64_t)p & m);
        ocerz_write_op(cpu, insn, o0, (uint64_t)(p >> bits) & m);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_RORX: {
        unsigned c = (unsigned)(o2->imm & (bits - 1));
        ocerz_write_op(cpu, insn, o0, c ? ((a >> c) | (a << (bits - c))) & m : a);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_SHLX:
    case OCERZ_OP_SHRX:
    case OCERZ_OP_SARX: {
        unsigned c = (unsigned)(ocerz_read_op(cpu, insn, o2) & (bits - 1));
        if (insn->op == OCERZ_OP_SHLX)
            r = (a << c) & m;
        else if (insn->op == OCERZ_OP_SHRX)
            r = a >> c;
        else
            r = (uint64_t)(ocerz_sext(a, size) >> c) & m;
        ocerz_write_op(cpu, insn, o0, r);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_MOVBE:
        r = size == 2 ? __builtin_bswap16((uint16_t)a) : size == 4 ? __builtin_bswap32((uint32_t)a) : __builtin_bswap64(a);
        ocerz_write_op(cpu, insn, o0, r);
        return OCERZ_STEP_OK;
    case OCERZ_OP_RDRAND:
        arc4random_buf(&r, sizeof r);
        ocerz_write_op(cpu, insn, o0, r & m);
        cpu->rflags &= ~(uint64_t)(OCERZ_OF | OCERZ_SF | OCERZ_ZF | OCERZ_AF | OCERZ_PF);
        cpu->rflags |= OCERZ_CF;
        return OCERZ_STEP_OK;
    default:
        return OCERZ_EUNSUP;
    }
    ocerz_write_op(cpu, insn, o0, r);
    ocerz_flag_assign(cpu, OCERZ_CF, cf);
    ocerz_flag_assign(cpu, OCERZ_ZF, r == 0);
    ocerz_flag_assign(cpu, OCERZ_SF, ocerz_msb(r, size));
    ocerz_flag_assign(cpu, OCERZ_OF, 0);
    return OCERZ_STEP_OK;
}

static void cpuid_brand(uint32_t leaf, uint32_t *regs)
{
    static const char brand[48] = "Ocerz x86_64 Emulated CPU";
    uint32_t idx = leaf - 0x80000002u;
    uint32_t words[12];
    memcpy(words, brand, 48);
    regs[0] = words[idx * 4 + 0];
    regs[1] = words[idx * 4 + 1];
    regs[2] = words[idx * 4 + 2];
    regs[3] = words[idx * 4 + 3];
}

static int ext_cpuid(OcerzCPU *cpu)
{
    uint32_t leaf = (uint32_t)cpu->gpr[OCERZ_RAX];
    uint32_t r[4] = { 0, 0, 0, 0 };

    if (leaf == 0) {
        r[0] = 0xd;
        r[1] = 0x756e6547;
        r[2] = 0x6c65746e;
        r[3] = 0x49656e69;
    } else if (leaf == 1) {
        r[0] = 0x000306a9;
        r[1] = 0x00100800;
        r[2] = 0x00982201;
        r[3] = 0x078bfbff;
    } else if (leaf == 0xd) {
        uint32_t sub = (uint32_t)cpu->gpr[OCERZ_RCX];
        if (sub == 0) {
            r[0] = 7;
            r[1] = 0x340;
            r[2] = 0x340;
        } else if (sub == 2) {
            r[0] = 0x100;
            r[1] = 0x240;
        }
    } else if (leaf == 0x80000000u) {
        r[0] = 0x80000004;
    } else if (leaf == 0x80000001u) {
        r[2] = 0x00000001;
        r[3] = 0x28100800;
    } else if (leaf >= 0x80000002u && leaf <= 0x80000004u) {
        cpuid_brand(leaf, r);
    }

    cpu->gpr[OCERZ_RAX] = r[0];
    cpu->gpr[OCERZ_RBX] = r[1];
    cpu->gpr[OCERZ_RCX] = r[2];
    cpu->gpr[OCERZ_RDX] = r[3];
    return OCERZ_STEP_OK;
}

static uint64_t ext_rdtsc_ns(void)
{
    static mach_timebase_info_data_t tb;
    static int have_tb;
    if (!have_tb) {
        mach_timebase_info(&tb);
        have_tb = 1;
    }
    uint64_t t = mach_absolute_time();
    return t * tb.numer / tb.denom;
}

static int ext_rdtsc(OcerzCPU *cpu, int rdtscp)
{
    uint64_t ns = ext_rdtsc_ns();
    cpu->gpr[OCERZ_RAX] = (uint32_t)ns;
    cpu->gpr[OCERZ_RDX] = (uint32_t)(ns >> 32);
    if (rdtscp)
        cpu->gpr[OCERZ_RCX] = 0;
    return OCERZ_STEP_OK;
}

static int ext_xgetbv(OcerzCPU *cpu)
{
    if ((uint32_t)cpu->gpr[OCERZ_RCX] != 0) {
        OCERZ_FATAL("xgetbv with ecx=%u is unsupported\n", (unsigned)cpu->gpr[OCERZ_RCX]);
        return OCERZ_STEP_FATAL;
    }
    cpu->gpr[OCERZ_RAX] = 7;
    cpu->gpr[OCERZ_RDX] = 0;
    return OCERZ_STEP_OK;
}

static int ext_fxsave(OcerzCPU *cpu, const X86Insn *insn)
{
    uint64_t ea = ocerz_ea(cpu, insn, &insn->ops[0]);
    ocerz_x87_fxsave(cpu, ea);
    ocerz_st(ea + 24, 4, cpu->mxcsr);
    ocerz_st(ea + 28, 4, 0x0000ffffu);

    for (int i = 0; i < 16; i++)
        ocerz_st128(ea + 160 + (uint64_t)i * 16, cpu->xmm[i]);
    return OCERZ_STEP_OK;
}

static int ext_fxrstor(OcerzCPU *cpu, const X86Insn *insn)
{
    uint64_t ea = ocerz_ea(cpu, insn, &insn->ops[0]);
    ocerz_x87_fxrstor(cpu, ea);
    cpu->mxcsr = (uint32_t)ocerz_ld(ea + 24, 4);
    ocerz_apply_mxcsr_round(cpu->mxcsr);
    for (int i = 0; i < 16; i++)
        cpu->xmm[i] = ocerz_ld128(ea + 160 + (uint64_t)i * 16);
    return OCERZ_STEP_OK;
}

static int ext_xsave(OcerzCPU *cpu, const X86Insn *insn)
{
    uint64_t ea = ocerz_ea(cpu, insn, &insn->ops[0]);
    uint64_t rfbm = (uint32_t)cpu->gpr[OCERZ_RAX] & 7u;
    if (rfbm & 1)
        ocerz_x87_fxsave(cpu, ea);
    if (rfbm & 6) {
        ocerz_st(ea + 24, 4, cpu->mxcsr);
        ocerz_st(ea + 28, 4, 0x0000ffffu);
    }
    if (rfbm & 2)
        for (int i = 0; i < 16; i++)
            ocerz_st128(ea + 160 + (uint64_t)i * 16, cpu->xmm[i]);
    if (rfbm & 4)
        for (int i = 0; i < 16; i++)
            ocerz_st128(ea + 576 + (uint64_t)i * 16, cpu->ymmh[i]);
    ocerz_st(ea + 512, 8, ((ocerz_ld(ea + 512, 8) & ~rfbm) | rfbm) & 7u);
    return OCERZ_STEP_OK;
}

static int ext_xrstor(OcerzCPU *cpu, const X86Insn *insn)
{
    uint64_t ea = ocerz_ea(cpu, insn, &insn->ops[0]);
    uint64_t rfbm = (uint32_t)cpu->gpr[OCERZ_RAX] & 7u;
    uint64_t bv = ocerz_ld(ea + 512, 8);
    if (rfbm & 1) {
        if (bv & 1) {
            ocerz_x87_fxrstor(cpu, ea);
        } else {
            ocerz_x87_reset(cpu);
            memset(cpu->fpr, 0, sizeof cpu->fpr);
            cpu->fpr_x_ok = 0;
        }
    }
    if (rfbm & 6) {
        cpu->mxcsr = (uint32_t)ocerz_ld(ea + 24, 4);
        ocerz_apply_mxcsr_round(cpu->mxcsr);
    }
    for (int i = 0; i < 16; i++) {
        if (rfbm & 2) {
            if (bv & 2)
                cpu->xmm[i] = ocerz_ld128(ea + 160 + (uint64_t)i * 16);
            else
                memset(&cpu->xmm[i], 0, sizeof cpu->xmm[i]);
        }
        if (rfbm & 4) {
            if (bv & 4) {
                cpu->ymmh[i] = ocerz_ld128(ea + 576 + (uint64_t)i * 16);
                cpu->ymmh_all_zero = 0;
            } else
                memset(&cpu->ymmh[i], 0, sizeof cpu->ymmh[i]);
        }
    }
    return OCERZ_STEP_OK;
}

static int ext_misc(OcerzCPU *cpu, const X86Insn *insn)
{
    switch (insn->op) {
    case OCERZ_OP_CPUID:
        return ext_cpuid(cpu);
    case OCERZ_OP_RDTSC:
        return ext_rdtsc(cpu, 0);
    case OCERZ_OP_RDTSCP:
        return ext_rdtsc(cpu, 1);
    case OCERZ_OP_XGETBV:
        return ext_xgetbv(cpu);
    case OCERZ_OP_SGDT:
    case OCERZ_OP_SIDT: {
        uint64_t ea = ocerz_ea(cpu, insn, &insn->ops[0]);
        ocerz_st(ea + 0, 2, (uint64_t)(uint16_t)(cpu->cpu_number & 0xfff));
        ocerz_st(ea + 2, 8, 0);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_LDMXCSR:
        cpu->mxcsr = (uint32_t)ocerz_ld(ocerz_ea(cpu, insn, &insn->ops[0]), 4);
        ocerz_apply_mxcsr_round(cpu->mxcsr);
        return OCERZ_STEP_OK;
    case OCERZ_OP_STMXCSR:
        ocerz_st(ocerz_ea(cpu, insn, &insn->ops[0]), 4, cpu->mxcsr);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FXSAVE:
        return ext_fxsave(cpu, insn);
    case OCERZ_OP_FXRSTOR:
        return ext_fxrstor(cpu, insn);
    case OCERZ_OP_XSAVE:
        return ext_xsave(cpu, insn);
    case OCERZ_OP_XRSTOR:
        return ext_xrstor(cpu, insn);
    case OCERZ_OP_EMMS:
        cpu->ftop = 0;
        cpu->ftw = 0;
        return OCERZ_STEP_OK;
    default:
        return OCERZ_EUNSUP;
    }
}

int ocerz_interp_ext(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    (void)vm;
    int op = insn->op;

    if (op > OCERZ_OP_X87_FIRST && op < OCERZ_OP_SSE_FIRST)
        return ocerz_x87_exec(cpu, insn);

    switch (op) {
    case OCERZ_OP_MOVS:
    case OCERZ_OP_STOS:
    case OCERZ_OP_LODS:
    case OCERZ_OP_SCAS:
    case OCERZ_OP_CMPS:
        return ext_string(cpu, insn);

    case OCERZ_OP_BT:
    case OCERZ_OP_BTS:
    case OCERZ_OP_BTR:
    case OCERZ_OP_BTC:
        return ext_bit(cpu, insn);

    case OCERZ_OP_CRC32:
        return ext_crc32(cpu, insn);

    case OCERZ_OP_BSF:
    case OCERZ_OP_BSR:
    case OCERZ_OP_POPCNT:
    case OCERZ_OP_TZCNT:
    case OCERZ_OP_LZCNT:
        return ext_scan(cpu, insn);

    case OCERZ_OP_CPUID:
    case OCERZ_OP_RDTSC:
    case OCERZ_OP_RDTSCP:
    case OCERZ_OP_XGETBV:
    case OCERZ_OP_SGDT:
    case OCERZ_OP_SIDT:
    case OCERZ_OP_LDMXCSR:
    case OCERZ_OP_STMXCSR:
    case OCERZ_OP_FXSAVE:
    case OCERZ_OP_FXRSTOR:
    case OCERZ_OP_XSAVE:
    case OCERZ_OP_XRSTOR:
    case OCERZ_OP_EMMS:
        return ext_misc(cpu, insn);

    case OCERZ_OP_ANDN:
    case OCERZ_OP_BLSR:
    case OCERZ_OP_BLSMSK:
    case OCERZ_OP_BLSI:
    case OCERZ_OP_BZHI:
    case OCERZ_OP_BEXTR:
    case OCERZ_OP_PDEP:
    case OCERZ_OP_PEXT:
    case OCERZ_OP_MULX:
    case OCERZ_OP_RORX:
    case OCERZ_OP_SARX:
    case OCERZ_OP_SHLX:
    case OCERZ_OP_SHRX:
    case OCERZ_OP_MOVBE:
    case OCERZ_OP_RDRAND:
        return ext_bmi(cpu, insn);

    default:
        return OCERZ_EUNSUP;
    }
}

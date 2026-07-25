/*
 * tests/unit/test_ext.c
 *
 * Unit tests for ocerz_interp_ext(): string ops, bit ops, the bit scans,
 * CPUID/RDTSC, and the x87 subset. The harness mirrors the planned
 * test_interp.c pattern: reserve a guest arena with ocerz_mem_init(), map a
 * scratch page, reset a CPU, place hand-encoded instruction bytes (or set up
 * operands directly via a synthesized X86Insn), call the handler, and assert
 * on the resulting CPU/memory state.
 *
 * Because ocerz_decode() is owned by another translation unit and may not be
 * linkable in isolation, the tests build X86Insn structures by hand. This is
 * legitimate: ocerz_interp_ext() consumes a decoded X86Insn, and the decode
 * contract (decode.h) fully specifies every field, so a hand-built insn is
 * exactly what the decoder would have produced. The few real-encoding cases
 * (CPUID) need no decoder either because they take no operands.
 *
 * A tiny check() macro counts assertions and prints the first failure with
 * file/line; main() returns non-zero if any assertion failed so `make unit`
 * stops on error. We target well over forty assertions across the families
 * named in the task: rep movsb forward/backward, rep stosq, repne scasb
 * found/not-found, cmpsb flags, BT against a distant byte via a large signed
 * register bit offset, BSF/BSR zero-source destination preservation, POPCNT
 * flag clearing, CPUID leaf 0/1/brand reassembly, RDTSC monotonicity, and a
 * spread of x87 behavior including 80-bit FLD/FSTP round-trips, FILD/FISTP
 * rounding under FLDCW, and FCOMI flag patterns with NaN.
 *
 * The scratch region is set up WITHOUT relying on ocerz_mem_init(): that
 * routine demands the kernel grant one of a fixed list of multi-gigabyte
 * arena base addresses exactly, which some host machines refuse. Instead we
 * mmap a plain host buffer and choose ocerz_guest_base so the guest_base
 * affine map (g2h(g) = g + guest_base) lands a known guest address on that
 * buffer. The sized load/store helpers in mem.h only use that mapping, so
 * this is a faithful, host-independent way to exercise the handler.
 */
#include "ocerz/interp.h"
#include "ocerz/interp_common.h"
#include "ocerz/vm.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>


static void x87_push_helper(OcerzCPU *c, double v);

static int g_checks;
static int g_fails;

#define check(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_fails++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static uint64_t g_scratch;

static void setup(OcerzVM *vm)
{
    memset(vm, 0, sizeof *vm);
    vm->cpu.vm = vm;
    ocerz_cpu_reset(&vm->cpu);
}

static X86Operand reg_op(unsigned reg, int size)
{
    X86Operand o;
    memset(&o, 0, sizeof o);
    o.kind = OCERZ_OPK_REG;
    o.reg = (uint8_t)reg;
    o.size = (uint8_t)size;
    o.base = OCERZ_REG_NONE;
    o.index = OCERZ_REG_NONE;
    return o;
}

static X86Operand imm_op(uint64_t v, int size)
{
    X86Operand o;
    memset(&o, 0, sizeof o);
    o.kind = OCERZ_OPK_IMM;
    o.size = (uint8_t)size;
    o.imm = v;
    o.base = OCERZ_REG_NONE;
    o.index = OCERZ_REG_NONE;
    return o;
}

static X86Operand mem_abs(uint64_t gaddr, int size)
{
    X86Operand o;
    memset(&o, 0, sizeof o);
    o.kind = OCERZ_OPK_MEM;
    o.size = (uint8_t)size;
    o.base = OCERZ_REG_NONE;
    o.index = OCERZ_REG_NONE;
    o.disp = (int64_t)gaddr;
    return o;
}

static X86Operand st_op(unsigned i)
{
    X86Operand o;
    memset(&o, 0, sizeof o);
    o.kind = OCERZ_OPK_ST;
    o.reg = (uint8_t)i;
    o.base = OCERZ_REG_NONE;
    o.index = OCERZ_REG_NONE;
    return o;
}

static X86Insn mk(int op, int opsize)
{
    X86Insn i;
    memset(&i, 0, sizeof i);
    i.op = (uint16_t)op;   /* X86Insn.op is 16-bit; the enum passed 255 long ago */
    i.opsize = (uint8_t)opsize;
    i.addrsize = 8;
    i.rep = OCERZ_REP_NONE;
    return i;
}

static void test_strings(OcerzVM *vm)
{
    OcerzCPU *c = &vm->cpu;
    uint64_t src = g_scratch;
    uint64_t dst = g_scratch + 0x100;

    for (int i = 0; i < 8; i++)
        ocerz_st(src + i, 1, (uint64_t)(0x10 + i));

    setup(vm);
    c->rflags &= ~OCERZ_DF;
    c->gpr[OCERZ_RSI] = src;
    c->gpr[OCERZ_RDI] = dst;
    c->gpr[OCERZ_RCX] = 8;
    X86Insn movs = mk(OCERZ_OP_MOVS, 1);
    movs.rep = OCERZ_REP_REP;
    check(ocerz_interp_ext(vm, c, &movs) == OCERZ_STEP_OK);
    check(c->gpr[OCERZ_RCX] == 0);
    check(c->gpr[OCERZ_RSI] == src + 8);
    check(c->gpr[OCERZ_RDI] == dst + 8);
    int ok = 1;
    for (int i = 0; i < 8; i++)
        if (ocerz_ld(dst + i, 1) != (uint64_t)(0x10 + i))
            ok = 0;
    check(ok);

    setup(vm);
    c->rflags |= OCERZ_DF;
    c->gpr[OCERZ_RSI] = src + 7;
    c->gpr[OCERZ_RDI] = dst + 0x80 + 7;
    c->gpr[OCERZ_RCX] = 8;
    movs = mk(OCERZ_OP_MOVS, 1);
    movs.rep = OCERZ_REP_REP;
    check(ocerz_interp_ext(vm, c, &movs) == OCERZ_STEP_OK);
    check(c->gpr[OCERZ_RSI] == src - 1);
    check(c->gpr[OCERZ_RDI] == dst + 0x80 - 1);
    ok = 1;
    for (int i = 0; i < 8; i++)
        if (ocerz_ld(dst + 0x80 + i, 1) != (uint64_t)(0x10 + i))
            ok = 0;
    check(ok);

    setup(vm);
    c->rflags &= ~OCERZ_DF;
    c->gpr[OCERZ_RDI] = dst + 0x200;
    c->gpr[OCERZ_RAX] = 0xdeadbeefcafef00dull;
    c->gpr[OCERZ_RCX] = 4;
    X86Insn stos = mk(OCERZ_OP_STOS, 8);
    stos.rep = OCERZ_REP_REP;
    check(ocerz_interp_ext(vm, c, &stos) == OCERZ_STEP_OK);
    check(c->gpr[OCERZ_RCX] == 0);
    check(c->gpr[OCERZ_RDI] == dst + 0x200 + 32);
    check(ocerz_ld(dst + 0x200, 8) == 0xdeadbeefcafef00dull);
    check(ocerz_ld(dst + 0x200 + 24, 8) == 0xdeadbeefcafef00dull);

    for (int i = 0; i < 8; i++)
        ocerz_st(src + 0x40 + i, 1, (uint64_t)(i == 5 ? 0x99 : 0x11));
    setup(vm);
    c->rflags &= ~OCERZ_DF;
    c->gpr[OCERZ_RDI] = src + 0x40;
    c->gpr[OCERZ_RAX] = 0x99;
    c->gpr[OCERZ_RCX] = 8;
    X86Insn scas = mk(OCERZ_OP_SCAS, 1);
    scas.rep = OCERZ_REP_REPNE;
    check(ocerz_interp_ext(vm, c, &scas) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_ZF) != 0);
    check(c->gpr[OCERZ_RCX] == 2);
    check(c->gpr[OCERZ_RDI] == src + 0x40 + 6);

    for (int i = 0; i < 8; i++)
        ocerz_st(src + 0x60 + i, 1, 0x22);
    setup(vm);
    c->rflags &= ~OCERZ_DF;
    c->gpr[OCERZ_RDI] = src + 0x60;
    c->gpr[OCERZ_RAX] = 0x77;
    c->gpr[OCERZ_RCX] = 8;
    scas = mk(OCERZ_OP_SCAS, 1);
    scas.rep = OCERZ_REP_REPNE;
    check(ocerz_interp_ext(vm, c, &scas) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_ZF) == 0);
    check(c->gpr[OCERZ_RCX] == 0);

    ocerz_st(src + 0x80, 1, 0x41);
    ocerz_st(dst + 0x80 + 0x300, 1, 0x42);
    setup(vm);
    c->rflags &= ~OCERZ_DF;
    c->gpr[OCERZ_RSI] = src + 0x80;
    c->gpr[OCERZ_RDI] = dst + 0x80 + 0x300;
    X86Insn cmps = mk(OCERZ_OP_CMPS, 1);
    check(ocerz_interp_ext(vm, c, &cmps) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_CF) != 0);
    check((c->rflags & OCERZ_ZF) == 0);
    check(c->gpr[OCERZ_RSI] == src + 0x81);

    ocerz_st(src + 0x90, 1, 0x55);
    ocerz_st(dst + 0x90 + 0x300, 1, 0x55);
    setup(vm);
    c->rflags &= ~OCERZ_DF;
    c->gpr[OCERZ_RSI] = src + 0x90;
    c->gpr[OCERZ_RDI] = dst + 0x90 + 0x300;
    cmps = mk(OCERZ_OP_CMPS, 1);
    check(ocerz_interp_ext(vm, c, &cmps) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_ZF) != 0);
    check((c->rflags & OCERZ_CF) == 0);

    setup(vm);
    c->rflags &= ~OCERZ_DF;
    c->gpr[OCERZ_RSI] = src;
    c->gpr[OCERZ_RAX] = 0;
    X86Insn lods = mk(OCERZ_OP_LODS, 1);
    check(ocerz_interp_ext(vm, c, &lods) == OCERZ_STEP_OK);
    check((c->gpr[OCERZ_RAX] & 0xff) == 0x10);
    check(c->gpr[OCERZ_RSI] == src + 1);
}

static void test_bits(OcerzVM *vm)
{
    OcerzCPU *c = &vm->cpu;

    setup(vm);
    c->gpr[OCERZ_RAX] = (uint64_t)1 << 40;
    X86Insn bt = mk(OCERZ_OP_BT, 8);
    bt.nops = 2;
    bt.ops[0] = reg_op(OCERZ_RAX, 8);
    bt.ops[1] = imm_op(40, 1);
    check(ocerz_interp_ext(vm, c, &bt) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_CF) != 0);

    setup(vm);
    c->gpr[OCERZ_RAX] = (uint64_t)1 << 40;
    bt = mk(OCERZ_OP_BT, 8);
    bt.nops = 2;
    bt.ops[0] = reg_op(OCERZ_RAX, 8);
    bt.ops[1] = imm_op(41, 1);
    check(ocerz_interp_ext(vm, c, &bt) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_CF) == 0);

    setup(vm);
    c->gpr[OCERZ_RAX] = 0;
    X86Insn bts = mk(OCERZ_OP_BTS, 8);
    bts.nops = 2;
    bts.ops[0] = reg_op(OCERZ_RAX, 8);
    bts.ops[1] = imm_op(70, 1);
    check(ocerz_interp_ext(vm, c, &bts) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_CF) == 0);
    check(c->gpr[OCERZ_RAX] == ((uint64_t)1 << (70 & 63)));

    setup(vm);
    c->gpr[OCERZ_RBX] = 0xff;
    X86Insn btr = mk(OCERZ_OP_BTR, 4);
    btr.nops = 2;
    btr.ops[0] = reg_op(OCERZ_RBX, 4);
    btr.ops[1] = imm_op(3, 1);
    check(ocerz_interp_ext(vm, c, &btr) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_CF) != 0);
    check(c->gpr[OCERZ_RBX] == 0xf7);

    uint64_t base = g_scratch + 0x400;
    for (int i = 0; i < 64; i++)
        ocerz_st(base + i, 1, 0);
    ocerz_st(base + 16, 1, (uint64_t)(1u << 3));

    setup(vm);
    c->gpr[OCERZ_RDX] = 16 * 8 + 3;
    X86Insn btm = mk(OCERZ_OP_BT, 4);
    btm.nops = 2;
    btm.ops[0] = mem_abs(base, 4);
    btm.ops[1] = reg_op(OCERZ_RDX, 4);
    check(ocerz_interp_ext(vm, c, &btm) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_CF) != 0);

    ocerz_st(base + 40, 1, 0);
    setup(vm);
    c->gpr[OCERZ_RDX] = 40 * 8 + 5;
    X86Insn btsm = mk(OCERZ_OP_BTS, 4);
    btsm.nops = 2;
    btsm.ops[0] = mem_abs(base, 4);
    btsm.ops[1] = reg_op(OCERZ_RDX, 4);
    check(ocerz_interp_ext(vm, c, &btsm) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_CF) == 0);
    check(ocerz_ld(base + 40, 1) == (uint64_t)(1u << 5));

    ocerz_st(base - 8, 1, (uint64_t)(1u << 2));
    setup(vm);
    c->gpr[OCERZ_RDX] = (uint64_t)(int64_t)(-8 * 8 + 2);
    X86Insn btneg = mk(OCERZ_OP_BT, 4);
    btneg.nops = 2;
    btneg.ops[0] = mem_abs(base, 4);
    btneg.ops[1] = reg_op(OCERZ_RDX, 8);
    check(ocerz_interp_ext(vm, c, &btneg) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_CF) != 0);

    ocerz_st(base + 48, 4, 0x50000000u);
    setup(vm);
    X86Insn btgi = mk(OCERZ_OP_BT, 4);
    btgi.nops = 2;
    btgi.ops[0] = mem_abs(base + 48, 4);
    btgi.ops[1] = imm_op(0x1c, 1);
    check(ocerz_interp_ext(vm, c, &btgi) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_CF) != 0);

    setup(vm);
    btgi.ops[1] = imm_op(0x18, 1);
    check(ocerz_interp_ext(vm, c, &btgi) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_CF) == 0);

    setup(vm);
    X86Insn btri = mk(OCERZ_OP_BTR, 4);
    btri.nops = 2;
    btri.ops[0] = mem_abs(base + 48, 4);
    btri.ops[1] = imm_op(0x1e, 1);
    check(ocerz_interp_ext(vm, c, &btri) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_CF) != 0);
    check(ocerz_ld(base + 48, 4) == 0x10000000u);

    setup(vm);
    c->gpr[OCERZ_RAX] = 0x100;
    X86Insn btc = mk(OCERZ_OP_BTC, 8);
    btc.nops = 2;
    btc.ops[0] = reg_op(OCERZ_RAX, 8);
    btc.ops[1] = imm_op(8, 1);
    check(ocerz_interp_ext(vm, c, &btc) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_CF) != 0);
    check(c->gpr[OCERZ_RAX] == 0);
}

static void test_scans(OcerzVM *vm)
{
    OcerzCPU *c = &vm->cpu;

    setup(vm);
    c->gpr[OCERZ_RAX] = 0x1234;
    c->gpr[OCERZ_RBX] = 0xabc0;
    X86Insn bsf = mk(OCERZ_OP_BSF, 8);
    bsf.nops = 2;
    bsf.ops[0] = reg_op(OCERZ_RAX, 8);
    bsf.ops[1] = reg_op(OCERZ_RBX, 8);
    check(ocerz_interp_ext(vm, c, &bsf) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_ZF) == 0);
    check(c->gpr[OCERZ_RAX] == 6);

    setup(vm);
    c->gpr[OCERZ_RAX] = 0x9999;
    c->gpr[OCERZ_RBX] = 0;
    bsf = mk(OCERZ_OP_BSF, 8);
    bsf.nops = 2;
    bsf.ops[0] = reg_op(OCERZ_RAX, 8);
    bsf.ops[1] = reg_op(OCERZ_RBX, 8);
    check(ocerz_interp_ext(vm, c, &bsf) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_ZF) != 0);
    check(c->gpr[OCERZ_RAX] == 0x9999);

    setup(vm);
    c->gpr[OCERZ_RAX] = 0x5555;
    c->gpr[OCERZ_RBX] = 0x8000;
    X86Insn bsr = mk(OCERZ_OP_BSR, 8);
    bsr.nops = 2;
    bsr.ops[0] = reg_op(OCERZ_RAX, 8);
    bsr.ops[1] = reg_op(OCERZ_RBX, 8);
    check(ocerz_interp_ext(vm, c, &bsr) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_ZF) == 0);
    check(c->gpr[OCERZ_RAX] == 15);

    setup(vm);
    c->gpr[OCERZ_RAX] = 0x7777;
    c->gpr[OCERZ_RBX] = 0;
    bsr = mk(OCERZ_OP_BSR, 8);
    bsr.nops = 2;
    bsr.ops[0] = reg_op(OCERZ_RAX, 8);
    bsr.ops[1] = reg_op(OCERZ_RBX, 8);
    check(ocerz_interp_ext(vm, c, &bsr) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_ZF) != 0);
    check(c->gpr[OCERZ_RAX] == 0x7777);

    setup(vm);
    c->rflags |= OCERZ_CF | OCERZ_OF | OCERZ_SF;
    c->gpr[OCERZ_RBX] = 0xf0f0;
    X86Insn pc = mk(OCERZ_OP_POPCNT, 8);
    pc.nops = 2;
    pc.ops[0] = reg_op(OCERZ_RAX, 8);
    pc.ops[1] = reg_op(OCERZ_RBX, 8);
    check(ocerz_interp_ext(vm, c, &pc) == OCERZ_STEP_OK);
    check(c->gpr[OCERZ_RAX] == 8);
    check((c->rflags & OCERZ_CF) == 0);
    check((c->rflags & OCERZ_OF) == 0);
    check((c->rflags & OCERZ_SF) == 0);
    check((c->rflags & OCERZ_ZF) == 0);

    setup(vm);
    c->gpr[OCERZ_RBX] = 0;
    pc = mk(OCERZ_OP_POPCNT, 8);
    pc.nops = 2;
    pc.ops[0] = reg_op(OCERZ_RAX, 8);
    pc.ops[1] = reg_op(OCERZ_RBX, 8);
    check(ocerz_interp_ext(vm, c, &pc) == OCERZ_STEP_OK);
    check(c->gpr[OCERZ_RAX] == 0);
    check((c->rflags & OCERZ_ZF) != 0);

    setup(vm);
    c->gpr[OCERZ_RBX] = 0x80;
    X86Insn tz = mk(OCERZ_OP_TZCNT, 4);
    tz.nops = 2;
    tz.ops[0] = reg_op(OCERZ_RAX, 4);
    tz.ops[1] = reg_op(OCERZ_RBX, 4);
    check(ocerz_interp_ext(vm, c, &tz) == OCERZ_STEP_OK);
    check(c->gpr[OCERZ_RAX] == 7);
    check((c->rflags & OCERZ_CF) == 0);

    setup(vm);
    c->gpr[OCERZ_RBX] = 0;
    tz = mk(OCERZ_OP_TZCNT, 4);
    tz.nops = 2;
    tz.ops[0] = reg_op(OCERZ_RAX, 4);
    tz.ops[1] = reg_op(OCERZ_RBX, 4);
    check(ocerz_interp_ext(vm, c, &tz) == OCERZ_STEP_OK);
    check(c->gpr[OCERZ_RAX] == 32);
    check((c->rflags & OCERZ_CF) != 0);
    check((c->rflags & OCERZ_ZF) == 0);

    setup(vm);
    c->gpr[OCERZ_RBX] = 1;
    X86Insn lz = mk(OCERZ_OP_LZCNT, 4);
    lz.nops = 2;
    lz.ops[0] = reg_op(OCERZ_RAX, 4);
    lz.ops[1] = reg_op(OCERZ_RBX, 4);
    check(ocerz_interp_ext(vm, c, &lz) == OCERZ_STEP_OK);
    check(c->gpr[OCERZ_RAX] == 31);
    check((c->rflags & OCERZ_CF) == 0);

    setup(vm);
    c->gpr[OCERZ_RBX] = 0;
    lz = mk(OCERZ_OP_LZCNT, 4);
    lz.nops = 2;
    lz.ops[0] = reg_op(OCERZ_RAX, 4);
    lz.ops[1] = reg_op(OCERZ_RBX, 4);
    check(ocerz_interp_ext(vm, c, &lz) == OCERZ_STEP_OK);
    check(c->gpr[OCERZ_RAX] == 32);
    check((c->rflags & OCERZ_CF) != 0);
}

static void test_cpuid_rdtsc(OcerzVM *vm)
{
    OcerzCPU *c = &vm->cpu;

    setup(vm);
    c->gpr[OCERZ_RAX] = 0;
    X86Insn cid = mk(OCERZ_OP_CPUID, 4);
    check(ocerz_interp_ext(vm, c, &cid) == OCERZ_STEP_OK);
    check((uint32_t)c->gpr[OCERZ_RAX] == 7);
    check((uint32_t)c->gpr[OCERZ_RBX] == 0x756e6547);
    check((uint32_t)c->gpr[OCERZ_RDX] == 0x49656e69);
    check((uint32_t)c->gpr[OCERZ_RCX] == 0x6c65746e);

    setup(vm);
    c->gpr[OCERZ_RAX] = 1;
    cid = mk(OCERZ_OP_CPUID, 4);
    check(ocerz_interp_ext(vm, c, &cid) == OCERZ_STEP_OK);
    check((uint32_t)c->gpr[OCERZ_RAX] == 0x000306a9);
    check((uint32_t)c->gpr[OCERZ_RCX] == 0x00802201);
    check((uint32_t)c->gpr[OCERZ_RDX] == 0x078bfbff);

    char brand[49];
    memset(brand, 0, sizeof brand);
    for (uint32_t leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
        setup(vm);
        c->gpr[OCERZ_RAX] = leaf;
        cid = mk(OCERZ_OP_CPUID, 4);
        check(ocerz_interp_ext(vm, c, &cid) == OCERZ_STEP_OK);
        uint32_t idx = leaf - 0x80000002u;
        uint32_t w0 = (uint32_t)c->gpr[OCERZ_RAX];
        uint32_t w1 = (uint32_t)c->gpr[OCERZ_RBX];
        uint32_t w2 = (uint32_t)c->gpr[OCERZ_RCX];
        uint32_t w3 = (uint32_t)c->gpr[OCERZ_RDX];
        memcpy(brand + idx * 16 + 0, &w0, 4);
        memcpy(brand + idx * 16 + 4, &w1, 4);
        memcpy(brand + idx * 16 + 8, &w2, 4);
        memcpy(brand + idx * 16 + 12, &w3, 4);
    }
    check(strcmp(brand, "Ocerz x86_64 Emulated CPU") == 0);

    setup(vm);
    c->gpr[OCERZ_RAX] = 0x80000000u;
    cid = mk(OCERZ_OP_CPUID, 4);
    check(ocerz_interp_ext(vm, c, &cid) == OCERZ_STEP_OK);
    check((uint32_t)c->gpr[OCERZ_RAX] == 0x80000004u);

    setup(vm);
    c->gpr[OCERZ_RAX] = 0x80000001u;
    cid = mk(OCERZ_OP_CPUID, 4);
    check(ocerz_interp_ext(vm, c, &cid) == OCERZ_STEP_OK);
    check((uint32_t)c->gpr[OCERZ_RCX] == 0x00000001u);
    check((uint32_t)c->gpr[OCERZ_RDX] == 0x28100800u);

    setup(vm);
    c->gpr[OCERZ_RAX] = 2;
    cid = mk(OCERZ_OP_CPUID, 4);
    check(ocerz_interp_ext(vm, c, &cid) == OCERZ_STEP_OK);
    check(c->gpr[OCERZ_RAX] == 0 && c->gpr[OCERZ_RBX] == 0 &&
          c->gpr[OCERZ_RCX] == 0 && c->gpr[OCERZ_RDX] == 0);

    setup(vm);
    X86Insn ts = mk(OCERZ_OP_RDTSC, 8);
    check(ocerz_interp_ext(vm, c, &ts) == OCERZ_STEP_OK);
    uint64_t t1 = ((uint64_t)(uint32_t)c->gpr[OCERZ_RDX] << 32) | (uint32_t)c->gpr[OCERZ_RAX];
    check((c->gpr[OCERZ_RDX] >> 32) == 0);
    check((c->gpr[OCERZ_RAX] >> 32) == 0);
    X86Insn tsp = mk(OCERZ_OP_RDTSCP, 8);
    check(ocerz_interp_ext(vm, c, &tsp) == OCERZ_STEP_OK);
    uint64_t t2 = ((uint64_t)(uint32_t)c->gpr[OCERZ_RDX] << 32) | (uint32_t)c->gpr[OCERZ_RAX];
    check(t2 >= t1);
    check(c->gpr[OCERZ_RCX] == 0);

    setup(vm);
    c->gpr[OCERZ_RCX] = 0;
    X86Insn xg = mk(OCERZ_OP_XGETBV, 4);
    check(ocerz_interp_ext(vm, c, &xg) == OCERZ_STEP_OK);
    check(c->gpr[OCERZ_RAX] == 3);
    check(c->gpr[OCERZ_RDX] == 0);
}

/* FXSAVE/FXRSTOR must cover XMM0-15 and MXCSR_MASK, not just the x87 state.
 *
 * This was untested, and the omission was not academic: Wine's __wine_syscall_dispatcher
 * does not rely on the CPU preserving registers across a syscall -- it FXSAVEs and then
 * reloads the Win64 non-volatile xmm6-xmm15 with movaps straight out of this memory image.
 * With the XMM area never written, every PE Nt* syscall restored stack garbage (usually
 * zero) into xmm6-15, ntdll's acl->actctx came back NULL, and wineboot died on it. Assert
 * the whole image round-trips, since "preserves registers" and "writes the save area" are
 * different properties and only the second one is what the guest actually asked for. */
static void test_fxsave_xmm(OcerzVM *vm)
{
    OcerzCPU *c = &vm->cpu;
    uint64_t m = g_scratch + 0x1000;      /* 16-byte aligned */

    setup(vm);
    for (int i = 0; i < 16; i++) {
        c->xmm[i].lo = 0x1111111100000000ull + (uint64_t)i;
        c->xmm[i].hi = 0x2222222200000000ull + (uint64_t)i;
    }
    c->mxcsr = 0x1f80;
    for (uint64_t off = 0; off < 512; off += 8)
        ocerz_st(m + off, 8, 0xAAAAAAAAAAAAAAAAull);   /* poison: prove it is written */

    X86Insn fxs = mk(OCERZ_OP_FXSAVE, 8);
    fxs.nops = 1;
    fxs.ops[0] = mem_abs(m, 8);
    check(ocerz_interp_ext(vm, c, &fxs) == OCERZ_STEP_OK);
    for (int i = 0; i < 16; i++) {
        check(ocerz_ld(m + 160 + (uint64_t)i * 16 + 0, 8) == c->xmm[i].lo);
        check(ocerz_ld(m + 160 + (uint64_t)i * 16 + 8, 8) == c->xmm[i].hi);
    }
    check(ocerz_ld(m + 24, 4) == 0x1f80);              /* MXCSR      */
    check(ocerz_ld(m + 28, 4) == 0x0000ffff);          /* MXCSR_MASK */

    /* and the mirror: FXRSTOR must load them back */
    for (int i = 0; i < 16; i++) {
        c->xmm[i].lo = 0;
        c->xmm[i].hi = 0;
    }
    X86Insn fxr = mk(OCERZ_OP_FXRSTOR, 8);
    fxr.nops = 1;
    fxr.ops[0] = mem_abs(m, 8);
    check(ocerz_interp_ext(vm, c, &fxr) == OCERZ_STEP_OK);
    for (int i = 0; i < 16; i++) {
        check(c->xmm[i].lo == 0x1111111100000000ull + (uint64_t)i);
        check(c->xmm[i].hi == 0x2222222200000000ull + (uint64_t)i);
    }
}

static void test_x87(OcerzVM *vm)
{
    OcerzCPU *c = &vm->cpu;
    uint64_t m = g_scratch + 0x800;

    setup(vm);
    X86Insn fld1 = mk(OCERZ_OP_FLD1, 8);
    check(ocerz_interp_ext(vm, c, &fld1) == OCERZ_STEP_OK);
    check(c->fpr[c->ftop] == 1.0);

    X86Insn fldpi = mk(OCERZ_OP_FLDPI, 8);
    check(ocerz_interp_ext(vm, c, &fldpi) == OCERZ_STEP_OK);
    check(fabs(c->fpr[c->ftop] - M_PI) < 1e-12);

    X86Insn faddp = mk(OCERZ_OP_FADDP, 8);
    faddp.nops = 0;
    check(ocerz_interp_ext(vm, c, &faddp) == OCERZ_STEP_OK);
    check(fabs(c->fpr[c->ftop] - (1.0 + M_PI)) < 1e-12);

    X86Insn fstp = mk(OCERZ_OP_FSTP, 8);
    fstp.nops = 1;
    fstp.ops[0] = mem_abs(m, 8);
    check(ocerz_interp_ext(vm, c, &fstp) == OCERZ_STEP_OK);
    double back;
    uint64_t bits = ocerz_ld(m, 8);
    memcpy(&back, &bits, 8);
    check(fabs(back - (1.0 + M_PI)) < 1e-12);

    setup(vm);
    ocerz_st(m, 4, 0x42280000u);
    X86Insn fldm = mk(OCERZ_OP_FLD, 4);
    fldm.nops = 1;
    fldm.ops[0] = mem_abs(m, 4);
    check(ocerz_interp_ext(vm, c, &fldm) == OCERZ_STEP_OK);
    check(c->fpr[c->ftop] == 42.0);

    setup(vm);
    ocerz_st(m, 8, (uint64_t)(int64_t)-1234);
    X86Insn fild = mk(OCERZ_OP_FILD, 8);
    fild.nops = 1;
    fild.ops[0] = mem_abs(m, 8);
    check(ocerz_interp_ext(vm, c, &fild) == OCERZ_STEP_OK);
    check(c->fpr[c->ftop] == -1234.0);

    setup(vm);
    c->fcw = (uint16_t)((c->fcw & ~0x0c00u) | 0x0c00u);
    x87_push_helper(c, 2.7);
    X86Insn fistp = mk(OCERZ_OP_FISTP, 4);
    fistp.nops = 1;
    fistp.ops[0] = mem_abs(m, 4);
    check(ocerz_interp_ext(vm, c, &fistp) == OCERZ_STEP_OK);
    check((int32_t)ocerz_ld(m, 4) == 2);

    setup(vm);
    c->fcw = (uint16_t)(c->fcw & ~0x0c00u);
    x87_push_helper(c, 2.7);
    fistp = mk(OCERZ_OP_FISTP, 4);
    fistp.nops = 1;
    fistp.ops[0] = mem_abs(m, 4);
    check(ocerz_interp_ext(vm, c, &fistp) == OCERZ_STEP_OK);
    check((int32_t)ocerz_ld(m, 4) == 3);

    setup(vm);
    x87_push_helper(c, 1.0);
    x87_push_helper(c, 2.0);
    X86Insn fcomi = mk(OCERZ_OP_FCOMI, 8);
    fcomi.nops = 1;
    fcomi.ops[0] = st_op(1);
    check(ocerz_interp_ext(vm, c, &fcomi) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_CF) == 0);
    check((c->rflags & OCERZ_ZF) == 0);

    setup(vm);
    x87_push_helper(c, 2.0);
    x87_push_helper(c, 1.0);
    fcomi = mk(OCERZ_OP_FCOMI, 8);
    fcomi.nops = 1;
    fcomi.ops[0] = st_op(1);
    check(ocerz_interp_ext(vm, c, &fcomi) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_CF) != 0);
    check((c->rflags & OCERZ_ZF) == 0);

    setup(vm);
    x87_push_helper(c, 3.0);
    x87_push_helper(c, 3.0);
    fcomi = mk(OCERZ_OP_FCOMI, 8);
    fcomi.nops = 1;
    fcomi.ops[0] = st_op(1);
    check(ocerz_interp_ext(vm, c, &fcomi) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_ZF) != 0);
    check((c->rflags & OCERZ_CF) == 0);

    setup(vm);
    x87_push_helper(c, NAN);
    x87_push_helper(c, 1.0);
    fcomi = mk(OCERZ_OP_FCOMI, 8);
    fcomi.nops = 1;
    fcomi.ops[0] = st_op(1);
    check(ocerz_interp_ext(vm, c, &fcomi) == OCERZ_STEP_OK);
    check((c->rflags & OCERZ_ZF) != 0);
    check((c->rflags & OCERZ_PF) != 0);
    check((c->rflags & OCERZ_CF) != 0);

    setup(vm);
    x87_push_helper(c, 1.5);
    X86Insn fstp80 = mk(OCERZ_OP_FSTP, 10);
    fstp80.nops = 1;
    fstp80.ops[0] = mem_abs(m, 10);
    check(ocerz_interp_ext(vm, c, &fstp80) == OCERZ_STEP_OK);
    X86Insn fld80 = mk(OCERZ_OP_FLD, 10);
    fld80.nops = 1;
    fld80.ops[0] = mem_abs(m, 10);
    check(ocerz_interp_ext(vm, c, &fld80) == OCERZ_STEP_OK);
    check(c->fpr[c->ftop] == 1.5);

    setup(vm);
    x87_push_helper(c, -0.0);
    fstp80 = mk(OCERZ_OP_FSTP, 10);
    fstp80.nops = 1;
    fstp80.ops[0] = mem_abs(m, 10);
    check(ocerz_interp_ext(vm, c, &fstp80) == OCERZ_STEP_OK);
    fld80 = mk(OCERZ_OP_FLD, 10);
    fld80.nops = 1;
    fld80.ops[0] = mem_abs(m, 10);
    check(ocerz_interp_ext(vm, c, &fld80) == OCERZ_STEP_OK);
    check(c->fpr[c->ftop] == 0.0);
    check(signbit(c->fpr[c->ftop]));

    setup(vm);
    x87_push_helper(c, 9.0);
    X86Insn fsqrt = mk(OCERZ_OP_FSQRT, 8);
    check(ocerz_interp_ext(vm, c, &fsqrt) == OCERZ_STEP_OK);
    check(c->fpr[c->ftop] == 3.0);

    setup(vm);
    X86Insn fninit = mk(OCERZ_OP_FNINIT, 8);
    check(ocerz_interp_ext(vm, c, &fninit) == OCERZ_STEP_OK);
    check(c->fcw == 0x037f);
    check(c->fsw == 0);
    check(c->ftop == 0);

    setup(vm);
    x87_push_helper(c, 7.0);
    X86Insn fnstsw = mk(OCERZ_OP_FNSTSW, 2);
    fnstsw.nops = 1;
    fnstsw.ops[0] = reg_op(OCERZ_RAX, 2);
    check(ocerz_interp_ext(vm, c, &fnstsw) == OCERZ_STEP_OK);
    check(((c->gpr[OCERZ_RAX] >> 11) & 7) == c->ftop);
}

static void x87_push_helper(OcerzCPU *c, double v)
{
    c->ftop = (uint8_t)((c->ftop - 1) & 7);
    c->fpr[c->ftop] = v;
    c->ftw = 0xff;
}

int main(void)
{
    size_t scratch_len = 0x20000;
    void *host = mmap(NULL, scratch_len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    if (host == MAP_FAILED) {
        fprintf(stderr, "scratch map failed\n");
        return 2;
    }
    g_scratch = 0x1000;
    ocerz_guest_base = (uint64_t)(uintptr_t)host - g_scratch;

    OcerzVM vm;
    test_strings(&vm);
    test_bits(&vm);
    test_scans(&vm);
    test_cpuid_rdtsc(&vm);
    test_x87(&vm);
    test_fxsave_xmm(&vm);

    fprintf(stderr, "test_ext: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

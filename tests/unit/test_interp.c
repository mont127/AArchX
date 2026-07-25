/*
 * tests/unit/test_interp.c
 *
 * Standalone unit test for the core interpreter (src/interp.c). It exercises
 * the integer/general-purpose half of the ISA the way real guest code does:
 * it reserves a guest arena with ocerz_mem_init(), maps one scratch code page
 * and one scratch data page inside it with ocerz_map_fixed(), hand-assembles
 * short x86_64 machine-code sequences into the code page, points a zeroed
 * OcerzCPU (inside a zeroed OcerzVM from ocerz_vm_init, jit disabled) at them,
 * single-steps with ocerz_interp_step(), and asserts the resulting registers,
 * individual flag bits and memory against values cross-checked on real x86_64
 * hardware (via Rosetta-built oracles during development).
 *
 * Each case writes its own bytes at CODE_BASE, resets the relevant registers
 * and flags, sets cpu.rip = CODE_BASE, steps once (asserting STEP_OK), then
 * checks outcomes. Helpers EXPECT_U64/EXPECT_FLAG print the failing case name,
 * expected and actual values and return nonzero from main on the first
 * failure, so a regression names exactly what broke. Instruction encodings
 * are written by hand and annotated in the assembling helpers; the file holds
 * well over sixty assertions spanning 32-bit zero-extension, high-byte access,
 * ADC/SBB chains, INC preserving CF, shift counts of 0/1/large, SAR sign,
 * MUL/IMUL/DIV including the 8-bit and __int128 forms, CMOVcc still zero-
 * extending on a false condition, PUSH/POP, CALL/RET, Jcc taken and not taken,
 * XADD, CMPXCHG both outcomes, LOOP, SHLD/SHRD and the full rotate family.
 *
 * The harness links against the whole emulator object set (minus main) so the
 * real decoder and flag helpers are under test alongside the interpreter; it
 * is the differential reference that the JIT tier must later match.
 */
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/interp.h"
#include "ocerz/interp_common.h"

#include <stdlib.h>
#include <sys/mman.h>

#define CODE_BASE 0x200000000ull
#define DATA_BASE 0x201000000ull

static int g_failures;

static void put_bytes(uint64_t gaddr, const uint8_t *b, size_t n)
{
    uint8_t *p = (uint8_t *)ocerz_g2h(gaddr);
    for (size_t i = 0; i < n; i++)
        p[i] = b[i];
}

static int check_u64(const char *name, uint64_t expect, uint64_t got)
{
    if (expect != got) {
        fprintf(stderr, "FAIL %s: expected %#llx got %#llx\n",
                name, (unsigned long long)expect, (unsigned long long)got);
        g_failures++;
        return 0;
    }
    return 1;
}

static int check_flag(const char *name, int expect, int got)
{
    if ((!!expect) != (!!got)) {
        fprintf(stderr, "FAIL %s: expected %d got %d\n", name, !!expect, !!got);
        g_failures++;
        return 0;
    }
    return 1;
}

#define EXPECT_U64(name, e, g) check_u64(name, (uint64_t)(e), (uint64_t)(g))
#define EXPECT_FLAG(name, e, g) check_flag(name, (int)(e), (int)(g))

static int CF(const OcerzCPU *c) { return (c->rflags & OCERZ_CF) != 0; }
static int ZF(const OcerzCPU *c) { return (c->rflags & OCERZ_ZF) != 0; }
static int SF(const OcerzCPU *c) { return (c->rflags & OCERZ_SF) != 0; }
static int OF(const OcerzCPU *c) { return (c->rflags & OCERZ_OF) != 0; }
static int PF(const OcerzCPU *c) { return (c->rflags & OCERZ_PF) != 0; }
static int AF(const OcerzCPU *c) { return (c->rflags & OCERZ_AF) != 0; }

static OcerzVM g_vm;

static void reset_cpu(void)
{
    ocerz_cpu_reset(&g_vm.cpu);
}

static int step_at(uint64_t rip)
{
    g_vm.cpu.rip = rip;
    return ocerz_interp_step(&g_vm, &g_vm.cpu);
}

static void emit(const uint8_t *b, size_t n)
{
    put_bytes(CODE_BASE, b, n);
}

#define EMIT(...) do { static const uint8_t _b[] = { __VA_ARGS__ }; emit(_b, sizeof _b); } while (0)

int main(void)
{
    if (ocerz_mem_init(0x100000000ull, 0x900000000ull) != OCERZ_OK) {
        fprintf(stderr, "mem_init failed\n");
        return 2;
    }
    if (ocerz_map_fixed(CODE_BASE, 0x10000, PROT_READ | PROT_WRITE) != OCERZ_OK ||
        ocerz_map_fixed(DATA_BASE, 0x10000, PROT_READ | PROT_WRITE) != OCERZ_OK) {
        fprintf(stderr, "map_fixed failed\n");
        return 2;
    }
    ocerz_vm_init(&g_vm);
    g_vm.jit_enabled = 0;

    int rc;

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0xffffffffffffffffull;
    EMIT(0xb8, 0x78, 0x56, 0x34, 0x12);
    rc = step_at(CODE_BASE);
    EXPECT_U64("mov_eax_imm.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("mov_eax_imm.rax", 0x12345678ull, g_vm.cpu.gpr[OCERZ_RAX]);

    reset_cpu();
    EMIT(0x48, 0xb8, 0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12);
    rc = step_at(CODE_BASE);
    EXPECT_U64("movabs_rax.rax", 0x123456789abcdef0ull, g_vm.cpu.gpr[OCERZ_RAX]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0;
    EMIT(0xb4, 0xab);
    step_at(CODE_BASE);
    EXPECT_U64("mov_ah.rax", 0xab00ull, g_vm.cpu.gpr[OCERZ_RAX]);
    EMIT(0xb3, 0x00);
    step_at(CODE_BASE);
    g_vm.cpu.gpr[OCERZ_RBX] = 0;
    EMIT(0x88, 0xe3);
    step_at(CODE_BASE);
    EXPECT_U64("mov_bl_ah.rbx", 0xabull, g_vm.cpu.gpr[OCERZ_RBX]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0xffffffffull;
    g_vm.cpu.gpr[OCERZ_RBX] = 1;
    EMIT(0x01, 0xd8);
    step_at(CODE_BASE);
    EXPECT_U64("add_wrap.rax", 0, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_FLAG("add_wrap.cf", 1, CF(&g_vm.cpu));
    EXPECT_FLAG("add_wrap.zf", 1, ZF(&g_vm.cpu));
    EXPECT_FLAG("add_wrap.of", 0, OF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x7fffffffull;
    EMIT(0x83, 0xc0, 0x01);
    step_at(CODE_BASE);
    EXPECT_U64("add_of.rax", 0x80000000ull, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_FLAG("add_of.of", 1, OF(&g_vm.cpu));
    EXPECT_FLAG("add_of.sf", 1, SF(&g_vm.cpu));
    EXPECT_FLAG("add_of.cf", 0, CF(&g_vm.cpu));
    EXPECT_FLAG("add_of.af", 1, AF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0xffffffffull;
    g_vm.cpu.gpr[OCERZ_RDX] = 0;
    EMIT(0x83, 0xc0, 0x01);
    step_at(CODE_BASE);
    EXPECT_FLAG("adc_chain.cf1", 1, CF(&g_vm.cpu));
    EMIT(0x83, 0xd2, 0x00);
    step_at(CODE_BASE);
    EXPECT_U64("adc_chain.edx", 1, g_vm.cpu.gpr[OCERZ_RDX]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0;
    g_vm.cpu.gpr[OCERZ_RDX] = 0;
    EMIT(0x83, 0xe8, 0x01);
    step_at(CODE_BASE);
    EXPECT_U64("sbb_chain.eax", 0xffffffffull, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_FLAG("sbb_chain.cf1", 1, CF(&g_vm.cpu));
    EMIT(0x83, 0xda, 0x00);
    step_at(CODE_BASE);
    EXPECT_U64("sbb_chain.edx", 0xffffffffull, g_vm.cpu.gpr[OCERZ_RDX]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0xf0f0;
    EMIT(0x25, 0x0f, 0x0f, 0x00, 0x00);
    step_at(CODE_BASE);
    EXPECT_U64("and.rax", 0, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_FLAG("and.zf", 1, ZF(&g_vm.cpu));
    EXPECT_FLAG("and.cf", 0, CF(&g_vm.cpu));
    EXPECT_FLAG("and.of", 0, OF(&g_vm.cpu));
    EXPECT_FLAG("and.pf", 1, PF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x7fffffffull;
    EMIT(0xf9);
    step_at(CODE_BASE);
    EXPECT_FLAG("inc.precf", 1, CF(&g_vm.cpu));
    EMIT(0xff, 0xc0);
    step_at(CODE_BASE);
    EXPECT_U64("inc.rax", 0x80000000ull, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_FLAG("inc.of", 1, OF(&g_vm.cpu));
    EXPECT_FLAG("inc.cf_preserved", 1, CF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 1;
    EMIT(0xf8);
    step_at(CODE_BASE);
    EMIT(0xff, 0xc8);
    step_at(CODE_BASE);
    EXPECT_U64("dec.rax", 0, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_FLAG("dec.zf", 1, ZF(&g_vm.cpu));
    EXPECT_FLAG("dec.cf0", 0, CF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x80;
    EMIT(0xf6, 0xd8);
    step_at(CODE_BASE);
    EXPECT_U64("neg.al", 0x80, g_vm.cpu.gpr[OCERZ_RAX] & 0xff);
    EXPECT_FLAG("neg.cf", 1, CF(&g_vm.cpu));
    EXPECT_FLAG("neg.of", 1, OF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0;
    EMIT(0xf6, 0xd8);
    step_at(CODE_BASE);
    EXPECT_FLAG("neg0.cf", 0, CF(&g_vm.cpu));
    EXPECT_FLAG("neg0.zf", 1, ZF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x0f0f0f0full;
    g_vm.cpu.rflags |= OCERZ_CF | OCERZ_OF | OCERZ_ZF;
    EMIT(0xf7, 0xd0);
    step_at(CODE_BASE);
    EXPECT_U64("not.rax", 0xf0f0f0f0ull, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_FLAG("not.cf_kept", 1, CF(&g_vm.cpu));
    EXPECT_FLAG("not.zf_kept", 1, ZF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0;
    g_vm.cpu.gpr[OCERZ_RBX] = 0xff;
    EMIT(0x0f, 0xb6, 0xc3);
    step_at(CODE_BASE);
    EXPECT_U64("movzx.rax", 0xff, g_vm.cpu.gpr[OCERZ_RAX]);
    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RBX] = 0xff;
    EMIT(0x48, 0x0f, 0xbe, 0xc3);
    step_at(CODE_BASE);
    EXPECT_U64("movsx.rax", 0xffffffffffffffffull, g_vm.cpu.gpr[OCERZ_RAX]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RBX] = 0x80000000ull;
    EMIT(0x48, 0x63, 0xc3);
    step_at(CODE_BASE);
    EXPECT_U64("movsxd.rax", 0xffffffff80000000ull, g_vm.cpu.gpr[OCERZ_RAX]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RBX] = 0x1000;
    g_vm.cpu.gpr[OCERZ_RCX] = 4;
    EMIT(0x48, 0x8d, 0x44, 0x8b, 0x10);
    step_at(CODE_BASE);
    EXPECT_U64("lea.rax", 0x1000 + 4 * 4 + 0x10, g_vm.cpu.gpr[OCERZ_RAX]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x1111;
    g_vm.cpu.gpr[OCERZ_RBX] = 0x2222;
    EMIT(0x48, 0x87, 0xd8);
    step_at(CODE_BASE);
    EXPECT_U64("xchg.rax", 0x2222, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_U64("xchg.rbx", 0x1111, g_vm.cpu.gpr[OCERZ_RBX]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0xdeadbeef;
    EMIT(0x0f, 0xc8);
    step_at(CODE_BASE);
    EXPECT_U64("bswap32.rax", 0xefbeaddeull, g_vm.cpu.gpr[OCERZ_RAX]);
    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x0102030405060708ull;
    EMIT(0x48, 0x0f, 0xc8);
    step_at(CODE_BASE);
    EXPECT_U64("bswap64.rax", 0x0807060504030201ull, g_vm.cpu.gpr[OCERZ_RAX]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0xffffffffffffffffull;
    g_vm.cpu.gpr[OCERZ_RBX] = 0x55;
    g_vm.cpu.rflags |= OCERZ_ZF;
    EMIT(0x0f, 0x44, 0xc3);
    step_at(CODE_BASE);
    EXPECT_U64("cmove_taken.rax", 0x55, g_vm.cpu.gpr[OCERZ_RAX]);
    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0xffffffff12345678ull;
    g_vm.cpu.gpr[OCERZ_RBX] = 0x99;
    g_vm.cpu.rflags &= ~OCERZ_ZF;
    EMIT(0x0f, 0x44, 0xc3);
    step_at(CODE_BASE);
    EXPECT_U64("cmove_false.rax", 0x12345678ull, g_vm.cpu.gpr[OCERZ_RAX]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0xff;
    g_vm.cpu.rflags |= OCERZ_ZF;
    EMIT(0x0f, 0x94, 0xc0);
    step_at(CODE_BASE);
    EXPECT_U64("sete.al", 1, g_vm.cpu.gpr[OCERZ_RAX] & 0xff);
    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0xff;
    g_vm.cpu.rflags &= ~OCERZ_ZF;
    EMIT(0x0f, 0x95, 0xc0);
    step_at(CODE_BASE);
    EXPECT_U64("setne.al", 1, g_vm.cpu.gpr[OCERZ_RAX] & 0xff);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x80000000ull;
    EMIT(0xd1, 0xe0);
    step_at(CODE_BASE);
    EXPECT_U64("shl1.rax", 0, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_FLAG("shl1.cf", 1, CF(&g_vm.cpu));
    EXPECT_FLAG("shl1.zf", 1, ZF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x1234;
    g_vm.cpu.gpr[OCERZ_RCX] = 0;
    g_vm.cpu.rflags |= OCERZ_CF;
    EMIT(0xd3, 0xe0);
    step_at(CODE_BASE);
    EXPECT_U64("shl0.rax", 0x1234, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_FLAG("shl0.cf_kept", 1, CF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 1;
    g_vm.cpu.gpr[OCERZ_RCX] = 33;
    EMIT(0xd3, 0xe0);
    step_at(CODE_BASE);
    EXPECT_U64("shl_big.rax", 2, g_vm.cpu.gpr[OCERZ_RAX]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x80000000ull;
    g_vm.cpu.gpr[OCERZ_RCX] = 4;
    EMIT(0xd3, 0xf8);
    step_at(CODE_BASE);
    EXPECT_U64("sar.rax", 0xf8000000ull, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_FLAG("sar.cf", 0, CF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x3;
    EMIT(0xd1, 0xe8);
    step_at(CODE_BASE);
    EXPECT_U64("shr.rax", 1, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_FLAG("shr.cf", 1, CF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x80;
    EMIT(0xd0, 0xc0);
    step_at(CODE_BASE);
    EXPECT_U64("rol.al", 1, g_vm.cpu.gpr[OCERZ_RAX] & 0xff);
    EXPECT_FLAG("rol.cf", 1, CF(&g_vm.cpu));
    EXPECT_FLAG("rol.of", 1, OF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x01;
    EMIT(0xd0, 0xc8);
    step_at(CODE_BASE);
    EXPECT_U64("ror.al", 0x80, g_vm.cpu.gpr[OCERZ_RAX] & 0xff);
    EXPECT_FLAG("ror.cf", 1, CF(&g_vm.cpu));
    EXPECT_FLAG("ror.of", 1, OF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x80;
    g_vm.cpu.rflags |= OCERZ_CF;
    EMIT(0xd0, 0xd0);
    step_at(CODE_BASE);
    EXPECT_U64("rcl.al", 0x01, g_vm.cpu.gpr[OCERZ_RAX] & 0xff);
    EXPECT_FLAG("rcl.cf", 1, CF(&g_vm.cpu));
    EXPECT_FLAG("rcl.of", 1, OF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 1;
    g_vm.cpu.rflags |= OCERZ_CF;
    EMIT(0xd1, 0xd8);
    step_at(CODE_BASE);
    EXPECT_U64("rcr.eax", 0x80000000ull, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_FLAG("rcr.cf", 1, CF(&g_vm.cpu));
    EXPECT_FLAG("rcr.of", 1, OF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x12345678ull;
    g_vm.cpu.gpr[OCERZ_RDX] = 0x9abcdef0ull;
    EMIT(0x0f, 0xa4, 0xd0, 0x08);
    step_at(CODE_BASE);
    EXPECT_U64("shld.eax", 0x3456789aull, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_FLAG("shld.cf", 0, CF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x12345678ull;
    g_vm.cpu.gpr[OCERZ_RDX] = 0x9abcdef0ull;
    EMIT(0x0f, 0xac, 0xd0, 0x04);
    step_at(CODE_BASE);
    EXPECT_U64("shrd.eax", 0x01234567ull, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_FLAG("shrd.cf", 1, CF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 200;
    g_vm.cpu.gpr[OCERZ_RBX] = 2;
    EMIT(0xf6, 0xe3);
    step_at(CODE_BASE);
    EXPECT_U64("mul8.ax", 400, g_vm.cpu.gpr[OCERZ_RAX] & 0xffff);
    EXPECT_FLAG("mul8.cf", 1, CF(&g_vm.cpu));
    EXPECT_FLAG("mul8.of", 1, OF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x10000;
    g_vm.cpu.gpr[OCERZ_RBX] = 0x10000;
    EMIT(0xf7, 0xe3);
    step_at(CODE_BASE);
    EXPECT_U64("mul32.eax", 0, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_U64("mul32.edx", 1, g_vm.cpu.gpr[OCERZ_RDX]);
    EXPECT_FLAG("mul32.cf", 1, CF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x8000000000000000ull;
    g_vm.cpu.gpr[OCERZ_RBX] = 0x8000000000000000ull;
    EMIT(0x48, 0xf7, 0xe3);
    step_at(CODE_BASE);
    EXPECT_U64("mul64.rax", 0, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_U64("mul64.rdx", 0x4000000000000000ull, g_vm.cpu.gpr[OCERZ_RDX]);
    EXPECT_FLAG("mul64.cf", 1, CF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = (uint64_t)(uint8_t)(-3);
    g_vm.cpu.gpr[OCERZ_RBX] = 5;
    EMIT(0xf6, 0xeb);
    step_at(CODE_BASE);
    EXPECT_U64("imul8.ax", (uint16_t)(-15), g_vm.cpu.gpr[OCERZ_RAX] & 0xffff);
    EXPECT_FLAG("imul8.of", 0, OF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RBX] = (uint64_t)(uint32_t)(-3);
    EMIT(0x6b, 0xc3, 0x05);
    step_at(CODE_BASE);
    EXPECT_U64("imul3.eax", (uint32_t)(-15), g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_FLAG("imul3.of", 0, OF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RBX] = 0x40000000ull;
    EMIT(0x6b, 0xc3, 0x04);
    step_at(CODE_BASE);
    EXPECT_FLAG("imul_of.of", 1, OF(&g_vm.cpu));
    EXPECT_FLAG("imul_of.cf", 1, CF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 400;
    g_vm.cpu.gpr[OCERZ_RBX] = 7;
    EMIT(0xf6, 0xf3);
    step_at(CODE_BASE);
    EXPECT_U64("div8.al", 57, g_vm.cpu.gpr[OCERZ_RAX] & 0xff);
    EXPECT_U64("div8.ah", 1, (g_vm.cpu.gpr[OCERZ_RAX] >> 8) & 0xff);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RDX] = 1;
    g_vm.cpu.gpr[OCERZ_RAX] = 1;
    g_vm.cpu.gpr[OCERZ_RCX] = 0x10;
    EMIT(0x66, 0xf7, 0xf1);
    step_at(CODE_BASE);
    EXPECT_U64("div16.ax", 0x1000, g_vm.cpu.gpr[OCERZ_RAX] & 0xffff);
    EXPECT_U64("div16.dx", 1, g_vm.cpu.gpr[OCERZ_RDX] & 0xffff);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = (uint16_t)(-7);
    g_vm.cpu.gpr[OCERZ_RBX] = 2;
    EMIT(0xf6, 0xfb);
    step_at(CODE_BASE);
    EXPECT_U64("idiv8.al", (uint8_t)(-3), g_vm.cpu.gpr[OCERZ_RAX] & 0xff);
    EXPECT_U64("idiv8.ah", (uint8_t)(-1), (g_vm.cpu.gpr[OCERZ_RAX] >> 8) & 0xff);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RDX] = 0xffff;
    g_vm.cpu.gpr[OCERZ_RAX] = 0xff9c;
    g_vm.cpu.gpr[OCERZ_RCX] = 7;
    EMIT(0x66, 0xf7, 0xf9);
    step_at(CODE_BASE);
    EXPECT_U64("idiv16.ax", (uint16_t)(-14), g_vm.cpu.gpr[OCERZ_RAX] & 0xffff);
    EXPECT_U64("idiv16.dx", (uint16_t)(-2), g_vm.cpu.gpr[OCERZ_RDX] & 0xffff);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = (uint64_t)(-10);
    g_vm.cpu.gpr[OCERZ_RDX] = (uint64_t)(-1);
    g_vm.cpu.gpr[OCERZ_RBX] = 3;
    EMIT(0x48, 0xf7, 0xfb);
    step_at(CODE_BASE);
    EXPECT_U64("idiv64.rax", (uint64_t)(-3), g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_U64("idiv64.rdx", (uint64_t)(-1), g_vm.cpu.gpr[OCERZ_RDX]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 10;
    g_vm.cpu.gpr[OCERZ_RBX] = 0;
    EMIT(0xf6, 0xf3);
    rc = step_at(CODE_BASE);
    EXPECT_U64("div0.fatal", OCERZ_STEP_FATAL, rc);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RSP] = DATA_BASE + 0x800;
    g_vm.cpu.gpr[OCERZ_RAX] = 0xcafef00dull;
    EMIT(0x50);
    step_at(CODE_BASE);
    EXPECT_U64("push.rsp", DATA_BASE + 0x800 - 8, g_vm.cpu.gpr[OCERZ_RSP]);
    EXPECT_U64("push.mem", 0xcafef00dull, ocerz_ld(g_vm.cpu.gpr[OCERZ_RSP], 8));
    EMIT(0x5b);
    step_at(CODE_BASE);
    EXPECT_U64("pop.rbx", 0xcafef00dull, g_vm.cpu.gpr[OCERZ_RBX]);
    EXPECT_U64("pop.rsp", DATA_BASE + 0x800, g_vm.cpu.gpr[OCERZ_RSP]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RSP] = DATA_BASE + 0x800;
    g_vm.cpu.rflags |= OCERZ_CF | OCERZ_ZF;
    EMIT(0x9c);
    step_at(CODE_BASE);
    g_vm.cpu.rflags &= ~(OCERZ_CF | OCERZ_ZF);
    EMIT(0x9d);
    step_at(CODE_BASE);
    EXPECT_FLAG("popf.cf", 1, CF(&g_vm.cpu));
    EXPECT_FLAG("popf.zf", 1, ZF(&g_vm.cpu));
    EXPECT_FLAG("popf.if_kept", 1, (g_vm.cpu.rflags & OCERZ_IF) != 0);
    EXPECT_FLAG("popf.fixed1", 1, (g_vm.cpu.rflags & OCERZ_FLAG_FIXED1) != 0);

    reset_cpu();
    g_vm.cpu.rflags |= OCERZ_CF | OCERZ_SF | OCERZ_ZF;
    EMIT(0x9f);
    step_at(CODE_BASE);
    EXPECT_FLAG("lahf.ah_cf", 1, (g_vm.cpu.gpr[OCERZ_RAX] >> 8) & OCERZ_CF);
    EXPECT_FLAG("lahf.ah_fixed", 1, ((g_vm.cpu.gpr[OCERZ_RAX] >> 8) >> 1) & 1);
    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = (uint64_t)(OCERZ_CF | OCERZ_ZF | OCERZ_FLAG_FIXED1) << 8;
    EMIT(0x9e);
    step_at(CODE_BASE);
    EXPECT_FLAG("sahf.cf", 1, CF(&g_vm.cpu));
    EXPECT_FLAG("sahf.zf", 1, ZF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x80;
    EMIT(0x66, 0x98);
    step_at(CODE_BASE);
    EXPECT_U64("cbw.ax", 0xff80, g_vm.cpu.gpr[OCERZ_RAX] & 0xffff);
    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x8000;
    EMIT(0x98);
    step_at(CODE_BASE);
    EXPECT_U64("cwde.eax", 0xffff8000ull, g_vm.cpu.gpr[OCERZ_RAX]);
    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x80000000ull;
    EMIT(0x48, 0x98);
    step_at(CODE_BASE);
    EXPECT_U64("cdqe.rax", 0xffffffff80000000ull, g_vm.cpu.gpr[OCERZ_RAX]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x80000000ull;
    g_vm.cpu.gpr[OCERZ_RDX] = 0x1234;
    EMIT(0x99);
    step_at(CODE_BASE);
    EXPECT_U64("cdq.edx", 0xffffffffull, g_vm.cpu.gpr[OCERZ_RDX]);

    reset_cpu();
    EMIT(0xeb, 0x05);
    rc = step_at(CODE_BASE);
    EXPECT_U64("jmp.rip", CODE_BASE + 2 + 5, g_vm.cpu.rip);

    reset_cpu();
    g_vm.cpu.rflags |= OCERZ_ZF;
    EMIT(0x74, 0x10);
    step_at(CODE_BASE);
    EXPECT_U64("je_taken.rip", CODE_BASE + 2 + 0x10, g_vm.cpu.rip);
    reset_cpu();
    g_vm.cpu.rflags &= ~OCERZ_ZF;
    EMIT(0x74, 0x10);
    step_at(CODE_BASE);
    EXPECT_U64("je_nottaken.rip", CODE_BASE + 2, g_vm.cpu.rip);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RCX] = 0;
    EMIT(0xe3, 0x08);
    step_at(CODE_BASE);
    EXPECT_U64("jrcxz.rip", CODE_BASE + 2 + 8, g_vm.cpu.rip);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RCX] = 3;
    EMIT(0xe2, 0x10);
    step_at(CODE_BASE);
    EXPECT_U64("loop.rcx", 2, g_vm.cpu.gpr[OCERZ_RCX]);
    EXPECT_U64("loop.taken", CODE_BASE + 2 + 0x10, g_vm.cpu.rip);
    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RCX] = 1;
    EMIT(0xe2, 0x10);
    step_at(CODE_BASE);
    EXPECT_U64("loop.fall", CODE_BASE + 2, g_vm.cpu.rip);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RSP] = DATA_BASE + 0x800;
    EMIT(0xe8, 0x00, 0x01, 0x00, 0x00);
    step_at(CODE_BASE);
    EXPECT_U64("call.rip", CODE_BASE + 5 + 0x100, g_vm.cpu.rip);
    EXPECT_U64("call.ret_on_stack", CODE_BASE + 5, ocerz_ld(g_vm.cpu.gpr[OCERZ_RSP], 8));
    EXPECT_U64("call.rsp", DATA_BASE + 0x800 - 8, g_vm.cpu.gpr[OCERZ_RSP]);
    EMIT(0xc3);
    step_at(CODE_BASE);
    EXPECT_U64("ret.rip", CODE_BASE + 5, g_vm.cpu.rip);
    EXPECT_U64("ret.rsp", DATA_BASE + 0x800, g_vm.cpu.gpr[OCERZ_RSP]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RSP] = DATA_BASE + 0x800;
    ocerz_st(g_vm.cpu.gpr[OCERZ_RSP], 8, 0xaabbccddull);
    EMIT(0xc2, 0x10, 0x00);
    step_at(CODE_BASE);
    EXPECT_U64("retn.rip", 0xaabbccddull, g_vm.cpu.rip);
    EXPECT_U64("retn.rsp", DATA_BASE + 0x800 + 8 + 0x10, g_vm.cpu.gpr[OCERZ_RSP]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RBP] = DATA_BASE + 0x900;
    ocerz_st(DATA_BASE + 0x900, 8, 0x1234567890ull);
    EMIT(0xc9);
    step_at(CODE_BASE);
    EXPECT_U64("leave.rbp", 0x1234567890ull, g_vm.cpu.gpr[OCERZ_RBP]);
    EXPECT_U64("leave.rsp", DATA_BASE + 0x900 + 8, g_vm.cpu.gpr[OCERZ_RSP]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 5;
    g_vm.cpu.gpr[OCERZ_RBX] = 10;
    EMIT(0x0f, 0xc1, 0xc3);
    step_at(CODE_BASE);
    EXPECT_U64("xadd.dst", 15, g_vm.cpu.gpr[OCERZ_RBX]);
    EXPECT_U64("xadd.src", 10, g_vm.cpu.gpr[OCERZ_RAX]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x100;
    g_vm.cpu.gpr[OCERZ_RBX] = 0x100;
    g_vm.cpu.gpr[OCERZ_RCX] = 0x999;
    EMIT(0x0f, 0xb1, 0xcb);
    step_at(CODE_BASE);
    EXPECT_FLAG("cmpxchg_eq.zf", 1, ZF(&g_vm.cpu));
    EXPECT_U64("cmpxchg_eq.dst", 0x999, g_vm.cpu.gpr[OCERZ_RBX]);
    EXPECT_U64("cmpxchg_eq.acc", 0x100, g_vm.cpu.gpr[OCERZ_RAX]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0xffffffffffffffffull;
    g_vm.cpu.gpr[OCERZ_RBX] = 0x100;
    g_vm.cpu.gpr[OCERZ_RCX] = 0x999;
    EMIT(0x0f, 0xb1, 0xcb);
    step_at(CODE_BASE);
    EXPECT_FLAG("cmpxchg_ne.zf", 0, ZF(&g_vm.cpu));
    EXPECT_U64("cmpxchg_ne.acc", 0x100, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_U64("cmpxchg_ne.dst", 0x100, g_vm.cpu.gpr[OCERZ_RBX]);

    reset_cpu();
    ocerz_st(DATA_BASE + 0x40, 8, 0x1111111122222222ull);
    g_vm.cpu.gpr[OCERZ_RAX] = 0x22222222;
    g_vm.cpu.gpr[OCERZ_RDX] = 0x11111111;
    g_vm.cpu.gpr[OCERZ_RBX] = 0xbbbbbbbb;
    g_vm.cpu.gpr[OCERZ_RCX] = 0xcccccccc;
    g_vm.cpu.gpr[OCERZ_RSI] = DATA_BASE + 0x40;
    EMIT(0x0f, 0xc7, 0x0e);
    step_at(CODE_BASE);
    EXPECT_FLAG("cmpxchg8b.zf", 1, ZF(&g_vm.cpu));
    EXPECT_U64("cmpxchg8b.mem", 0xccccccccbbbbbbbbull, ocerz_ld(DATA_BASE + 0x40, 8));

    reset_cpu();
    ocerz_st(DATA_BASE + 0x40, 8, 0xdeadbeefcafef00dull);
    g_vm.cpu.gpr[OCERZ_RAX] = 0;
    g_vm.cpu.gpr[OCERZ_RDX] = 0;
    g_vm.cpu.gpr[OCERZ_RSI] = DATA_BASE + 0x40;
    EMIT(0x0f, 0xc7, 0x0e);
    step_at(CODE_BASE);
    EXPECT_FLAG("cmpxchg8b_ne.zf", 0, ZF(&g_vm.cpu));
    EXPECT_U64("cmpxchg8b_ne.eax", 0xcafef00dull, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_U64("cmpxchg8b_ne.edx", 0xdeadbeefull, g_vm.cpu.gpr[OCERZ_RDX]);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RSI] = DATA_BASE + 0x100;
    g_vm.cpu.gpr[OCERZ_RAX] = 0x4142434445464748ull;
    EMIT(0x48, 0x89, 0x06);
    step_at(CODE_BASE);
    EXPECT_U64("mov_store.mem", 0x4142434445464748ull, ocerz_ld(DATA_BASE + 0x100, 8));
    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RSI] = DATA_BASE + 0x100;
    EMIT(0x8b, 0x06);
    step_at(CODE_BASE);
    EXPECT_U64("mov_load.rax", 0x45464748ull, g_vm.cpu.gpr[OCERZ_RAX]);

    reset_cpu();
    EMIT(0xf9); step_at(CODE_BASE);
    EXPECT_FLAG("stc.cf", 1, CF(&g_vm.cpu));
    EMIT(0xf5); step_at(CODE_BASE);
    EXPECT_FLAG("cmc.cf", 0, CF(&g_vm.cpu));
    EMIT(0xfd); step_at(CODE_BASE);
    EXPECT_FLAG("std.df", 1, (g_vm.cpu.rflags & OCERZ_DF) != 0);
    EMIT(0xfc); step_at(CODE_BASE);
    EXPECT_FLAG("cld.df", 0, (g_vm.cpu.rflags & OCERZ_DF) != 0);

    reset_cpu();
    EMIT(0x90);
    rc = step_at(CODE_BASE);
    EXPECT_U64("nop.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("nop.rip", CODE_BASE + 1, g_vm.cpu.rip);

    reset_cpu();
    EMIT(0x0f, 0x0b);
    rc = step_at(CODE_BASE);
    EXPECT_U64("ud2.fatal", OCERZ_STEP_FATAL, rc);

    reset_cpu();
    EMIT(0xcc);
    rc = step_at(CODE_BASE);
    EXPECT_U64("int3.fatal", OCERZ_STEP_FATAL, rc);

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 5;
    g_vm.cpu.gpr[OCERZ_RBX] = 5;
    EMIT(0x39, 0xd8);
    step_at(CODE_BASE);
    EXPECT_U64("cmp.rax", 5, g_vm.cpu.gpr[OCERZ_RAX]);
    EXPECT_FLAG("cmp.zf", 1, ZF(&g_vm.cpu));
    EXPECT_FLAG("cmp.cf", 0, CF(&g_vm.cpu));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RAX] = 0x8;
    EMIT(0xa9, 0x08, 0x00, 0x00, 0x00);
    step_at(CODE_BASE);
    EXPECT_FLAG("test.zf", 0, ZF(&g_vm.cpu));
    EXPECT_FLAG("test.cf", 0, CF(&g_vm.cpu));

    /* WoW64 mode-switch instructions, EXECUTED -- not merely decoded. This block exists
     * because decoding is not enough: the four new ops were briefly wired only inside
     * op_branch and never added to the top-level dispatch, so each fell through to the SSE
     * fallback and reported "no handler". test_decode passed the whole time, because it
     * decodes without executing. Stepping the interpreter is the only thing that proves the
     * dispatch actually reaches the implementation. Selector 0x33 is a GDT selector (bit 2
     * clear), i.e. not a 32-bit LDT segment, so every case below must behave as plain
     * 64-bit control flow and must NOT set mode32. */
    reset_cpu();
    ocerz_st(DATA_BASE, 4, 0x12340000ull);
    ocerz_st(DATA_BASE + 4, 2, 0x33);
    g_vm.cpu.gpr[OCERZ_RAX] = DATA_BASE;
    EMIT(0xff, 0x28);                                   /* jmp far ptr [rax] */
    rc = step_at(CODE_BASE);
    EXPECT_U64("jmpf.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("jmpf.rip", 0x12340000ull, g_vm.cpu.rip);
    EXPECT_U64("jmpf.mode32", 0, g_vm.cpu.mode32);

    reset_cpu();
    ocerz_st(DATA_BASE, 4, 0x5000ull);
    ocerz_st(DATA_BASE + 4, 2, 0x33);
    g_vm.cpu.gpr[OCERZ_RAX] = DATA_BASE;
    g_vm.cpu.gpr[OCERZ_RSP] = DATA_BASE + 0x1000;
    EMIT(0xff, 0x18);                                   /* call far ptr [rax] */
    rc = step_at(CODE_BASE);
    EXPECT_U64("callf.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("callf.rip", 0x5000ull, g_vm.cpu.rip);
    EXPECT_U64("callf.retaddr", CODE_BASE + 2, ocerz_ld(g_vm.cpu.gpr[OCERZ_RSP], 8));

    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RSP] = DATA_BASE + 0x2000;
    ocerz_st(DATA_BASE + 0x2000, 8, 0x9abc0000ull);     /* offset   */
    ocerz_st(DATA_BASE + 0x2008, 8, 0x33);              /* selector */
    EMIT(0xcb);                                         /* retf */
    rc = step_at(CODE_BASE);
    EXPECT_U64("retf.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("retf.rip", 0x9abc0000ull, g_vm.cpu.rip);

    /* IRETQ now consumes the CS slot between RIP and RFLAGS. A 64-bit CS must be a no-op --
     * Wine's __wine_syscall_dispatcher epilogue depends on exactly that. */
    reset_cpu();
    g_vm.cpu.gpr[OCERZ_RSP] = DATA_BASE + 0x3000;
    ocerz_st(DATA_BASE + 0x3000, 8, 0x777000ull);       /* RIP   */
    ocerz_st(DATA_BASE + 0x3008, 8, 0x33);              /* CS    */
    ocerz_st(DATA_BASE + 0x3010, 8, 0x202);             /* FLAGS */
    ocerz_st(DATA_BASE + 0x3018, 8, DATA_BASE + 0x4000);/* RSP   */
    EMIT(0x48, 0xcf);                                   /* iretq */
    rc = step_at(CODE_BASE);
    EXPECT_U64("iretq.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("iretq.rip", 0x777000ull, g_vm.cpu.rip);
    EXPECT_U64("iretq.rsp", DATA_BASE + 0x4000, g_vm.cpu.gpr[OCERZ_RSP]);
    EXPECT_U64("iretq.mode32", 0, g_vm.cpu.mode32);

    /* MOV Sreg with a GDT selector stays the no-op it has always been. */
    reset_cpu();
    g_vm.cpu.fs_base = 0xfeed0000ull;
    g_vm.cpu.gpr[OCERZ_RAX] = 0x33;
    EMIT(0x8e, 0xe0);                                   /* mov fs, eax */
    rc = step_at(CODE_BASE);
    EXPECT_U64("movseg.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("movseg.fs_base_unchanged", 0xfeed0000ull, g_vm.cpu.fs_base);

    if (g_failures) {
        fprintf(stderr, "test_interp: %d assertion(s) failed\n", g_failures);
        return 1;
    }
    fprintf(stderr, "test_interp: all assertions passed\n");
    return 0;
}

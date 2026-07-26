/* Unit tests for the SSE interpreter tier. */
#include "ocerz/interp.h"
#include "ocerz/interp_common.h"
#include "ocerz/mem.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <sys/mman.h>

static int g_checks;
static int g_fails;

#define CHECK(cond) do {                                                  \
    g_checks++;                                                           \
    if (!(cond)) {                                                        \
        g_fails++;                                                        \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
    }                                                                     \
} while (0)

static OcerzCPU g_cpu;
static uint64_t g_scratch;

static void reset_cpu(void)
{
    memset(&g_cpu, 0, sizeof g_cpu);
    g_cpu.rflags = OCERZ_FLAG_FIXED1 | OCERZ_IF;
    g_cpu.mxcsr = 0x1f80;
}

static X86Operand xreg(int r, int size)
{
    X86Operand o;
    memset(&o, 0, sizeof o);
    o.kind = OCERZ_OPK_XMM;
    o.reg = (uint8_t)r;
    o.size = (uint8_t)size;
    return o;
}

static X86Operand greg(int r, int size)
{
    X86Operand o;
    memset(&o, 0, sizeof o);
    o.kind = OCERZ_OPK_REG;
    o.reg = (uint8_t)r;
    o.size = (uint8_t)size;
    o.base = OCERZ_REG_NONE;
    o.index = OCERZ_REG_NONE;
    return o;
}

static X86Operand imm(uint64_t v)
{
    X86Operand o;
    memset(&o, 0, sizeof o);
    o.kind = OCERZ_OPK_IMM;
    o.size = 1;
    o.imm = v;
    return o;
}

static X86Operand mem(uint64_t gaddr, int size)
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

static X86Insn xi(int op, int nops, X86Operand a, X86Operand b, X86Operand c)
{
    X86Insn in;
    memset(&in, 0, sizeof in);
    in.op = (uint16_t)op;
    in.nops = (uint8_t)nops;
    in.opsize = 16;
    in.ops[0] = a;
    in.ops[1] = b;
    in.ops[2] = c;
    return in;
}

static Ocerz128 q4f(float a, float b, float c, float d)
{
    union { Ocerz128 q; float f[4]; } u;
    u.f[0] = a; u.f[1] = b; u.f[2] = c; u.f[3] = d;
    return u.q;
}

static Ocerz128 q2d(double a, double b)
{
    union { Ocerz128 q; double d[2]; } u;
    u.d[0] = a; u.d[1] = b;
    return u.q;
}

static float lane_f(Ocerz128 q, int i)
{
    union { Ocerz128 q; float f[4]; } u;
    u.q = q;
    return u.f[i];
}

static double lane_d(Ocerz128 q, int i)
{
    union { Ocerz128 q; double d[2]; } u;
    u.q = q;
    return u.d[i];
}

static uint32_t lane_u32(Ocerz128 q, int i)
{
    union { Ocerz128 q; uint32_t u[4]; } u;
    u.q = q;
    return u.u[i];
}

static uint16_t lane_u16(Ocerz128 q, int i)
{
    union { Ocerz128 q; uint16_t u[8]; } u;
    u.q = q;
    return u.u[i];
}

static uint8_t lane_u8(Ocerz128 q, int i)
{
    union { Ocerz128 q; uint8_t u[16]; } u;
    u.q = q;
    return u.u[i];
}

static Ocerz128 q16b(const uint8_t *b)
{
    Ocerz128 q;
    memcpy(&q, b, 16);
    return q;
}

static void run(const X86Insn *in)
{
    int r = ocerz_interp_sse(NULL, &g_cpu, in);
    CHECK(r == OCERZ_STEP_OK);
}

static void test_below_sse_first(void)
{
    X86Insn in = xi(OCERZ_OP_FWAIT, 0, greg(0, 8), greg(1, 8), imm(0));
    int r = ocerz_interp_sse(NULL, &g_cpu, &in);
    CHECK(r == OCERZ_EUNSUP);
}

static void test_moves(void)
{
    reset_cpu();
    g_cpu.xmm[1] = q4f(1.0f, 2.0f, 3.0f, 4.0f);
    X86Insn in = xi(OCERZ_OP_MOVAPS, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(g_cpu.xmm[0].lo == g_cpu.xmm[1].lo && g_cpu.xmm[0].hi == g_cpu.xmm[1].hi);

    reset_cpu();
    g_cpu.xmm[0].lo = 0xaaaaaaaabbbbbbbbull;
    g_cpu.xmm[0].hi = 0xccccccccddddddddull;
    g_cpu.xmm[1].lo = 0x1111111122222222ull;
    g_cpu.xmm[1].hi = 0x3333333344444444ull;
    in = xi(OCERZ_OP_MOVSS, 2, xreg(0, 4), xreg(1, 4), imm(0));
    run(&in);
    CHECK(lane_u32(g_cpu.xmm[0], 0) == 0x22222222u);
    CHECK(lane_u32(g_cpu.xmm[0], 1) == 0xaaaaaaaau);
    CHECK(g_cpu.xmm[0].hi == 0xccccccccddddddddull);

    reset_cpu();
    g_cpu.xmm[0].lo = ~0ull;
    g_cpu.xmm[0].hi = ~0ull;
    ocerz_st(g_scratch, 4, 0x40490fdbu);
    in = xi(OCERZ_OP_MOVSS, 2, xreg(0, 4), mem(g_scratch, 4), imm(0));
    run(&in);
    CHECK(lane_u32(g_cpu.xmm[0], 0) == 0x40490fdbu);
    CHECK(lane_u32(g_cpu.xmm[0], 1) == 0);
    CHECK(g_cpu.xmm[0].hi == 0);

    reset_cpu();
    g_cpu.xmm[0].lo = 1; g_cpu.xmm[0].hi = 0x99;
    g_cpu.xmm[1].lo = 0xdeadbeefcafebabeull; g_cpu.xmm[1].hi = 0x7;
    in = xi(OCERZ_OP_MOVSDX, 2, xreg(0, 8), xreg(1, 8), imm(0));
    run(&in);
    CHECK(g_cpu.xmm[0].lo == 0xdeadbeefcafebabeull);
    CHECK(g_cpu.xmm[0].hi == 0x99);

    reset_cpu();
    g_cpu.xmm[0].hi = ~0ull;
    g_cpu.xmm[1].lo = 0x123456789aull; g_cpu.xmm[1].hi = ~0ull;
    in = xi(OCERZ_OP_MOVQX, 2, xreg(0, 8), xreg(1, 8), imm(0));
    run(&in);
    CHECK(g_cpu.xmm[0].lo == 0x123456789aull);
    CHECK(g_cpu.xmm[0].hi == 0);

    reset_cpu();
    g_cpu.xmm[0].lo = ~0ull; g_cpu.xmm[0].hi = ~0ull;
    g_cpu.gpr[OCERZ_RAX] = 0xfeedface12345678ull;
    in = xi(OCERZ_OP_MOVD, 2, xreg(0, 4), greg(OCERZ_RAX, 4), imm(0));
    run(&in);
    CHECK(g_cpu.xmm[0].lo == 0x12345678ull);
    CHECK(g_cpu.xmm[0].hi == 0);

    reset_cpu();
    g_cpu.xmm[2] = q4f(-1.0f, 2.0f, -3.0f, -4.0f);
    g_cpu.gpr[OCERZ_RAX] = 0xffffffffffffffffull;
    in = xi(OCERZ_OP_MOVMSKPS, 2, greg(OCERZ_RAX, 4), xreg(2, 16), imm(0));
    run(&in);
    CHECK(g_cpu.gpr[OCERZ_RAX] == 0xd);

    reset_cpu();
    g_cpu.xmm[0].lo = 0xA; g_cpu.xmm[0].hi = 0xB;
    g_cpu.xmm[1].lo = 0xC; g_cpu.xmm[1].hi = 0xD;
    in = xi(OCERZ_OP_MOVLHPS, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(g_cpu.xmm[0].lo == 0xA && g_cpu.xmm[0].hi == 0xC);
    in = xi(OCERZ_OP_MOVHLPS, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(g_cpu.xmm[0].lo == 0xD);

    reset_cpu();
    ocerz_st(g_scratch, 8, 0x0102030405060708ull);
    in = xi(OCERZ_OP_MOVDDUP, 2, xreg(0, 16), mem(g_scratch, 8), imm(0));
    run(&in);
    CHECK(g_cpu.xmm[0].lo == 0x0102030405060708ull);
    CHECK(g_cpu.xmm[0].hi == 0x0102030405060708ull);

    reset_cpu();
    g_cpu.xmm[1] = q4f(1.0f, 2.0f, 3.0f, 4.0f);
    in = xi(OCERZ_OP_MOVSHDUP, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(lane_f(g_cpu.xmm[0], 0) == 2.0f && lane_f(g_cpu.xmm[0], 1) == 2.0f);
    CHECK(lane_f(g_cpu.xmm[0], 2) == 4.0f && lane_f(g_cpu.xmm[0], 3) == 4.0f);
    in = xi(OCERZ_OP_MOVSLDUP, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(lane_f(g_cpu.xmm[0], 0) == 1.0f && lane_f(g_cpu.xmm[0], 1) == 1.0f);
    CHECK(lane_f(g_cpu.xmm[0], 2) == 3.0f && lane_f(g_cpu.xmm[0], 3) == 3.0f);
}

static void test_fp_arith(void)
{
    reset_cpu();
    g_cpu.xmm[0] = q4f(1.0f, 2.0f, 3.0f, 4.0f);
    g_cpu.xmm[1] = q4f(10.0f, 20.0f, 30.0f, 40.0f);
    X86Insn in = xi(OCERZ_OP_ADDPS, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(lane_f(g_cpu.xmm[0], 0) == 11.0f && lane_f(g_cpu.xmm[0], 3) == 44.0f);

    reset_cpu();
    g_cpu.xmm[0] = q4f(1.0f, 2.0f, 3.0f, 4.0f);
    g_cpu.xmm[1] = q4f(100.0f, 200.0f, 300.0f, 400.0f);
    in = xi(OCERZ_OP_ADDSS, 2, xreg(0, 4), xreg(1, 4), imm(0));
    run(&in);
    CHECK(lane_f(g_cpu.xmm[0], 0) == 101.0f);
    CHECK(lane_f(g_cpu.xmm[0], 1) == 2.0f && lane_f(g_cpu.xmm[0], 3) == 4.0f);

    reset_cpu();
    g_cpu.xmm[0] = q2d(7.0, 9.0);
    g_cpu.xmm[1] = q2d(16.0, 25.0);
    in = xi(OCERZ_OP_SQRTSD, 2, xreg(0, 8), xreg(1, 8), imm(0));
    run(&in);
    CHECK(lane_d(g_cpu.xmm[0], 0) == 4.0);
    CHECK(lane_d(g_cpu.xmm[0], 1) == 9.0);

    reset_cpu();
    g_cpu.xmm[1] = q4f(2.0f, 4.0f, 8.0f, 0.5f);
    in = xi(OCERZ_OP_RCPPS, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(lane_f(g_cpu.xmm[0], 0) == 0.5f && lane_f(g_cpu.xmm[0], 3) == 2.0f);
}

static void test_minmax_nan(void)
{

    float nan = NAN;
    reset_cpu();
    g_cpu.xmm[0] = q4f(nan, 1.0f, 5.0f, 2.0f);
    g_cpu.xmm[1] = q4f(3.0f, nan, 2.0f, 5.0f);
    X86Insn in = xi(OCERZ_OP_MINPS, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(lane_f(g_cpu.xmm[0], 0) == 3.0f);
    CHECK(lane_f(g_cpu.xmm[0], 1) != lane_f(g_cpu.xmm[0], 1));
    CHECK(lane_f(g_cpu.xmm[0], 2) == 2.0f);
    CHECK(lane_f(g_cpu.xmm[0], 3) == 2.0f);

    reset_cpu();
    g_cpu.xmm[0] = q4f(nan, 5.0f, 2.0f, 0.0f);
    g_cpu.xmm[1] = q4f(3.0f, 2.0f, 5.0f, -0.0f);
    in = xi(OCERZ_OP_MAXPS, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(lane_f(g_cpu.xmm[0], 0) == 3.0f);
    CHECK(lane_f(g_cpu.xmm[0], 1) == 5.0f);
    CHECK(lane_f(g_cpu.xmm[0], 2) == 5.0f);
    CHECK(lane_u32(g_cpu.xmm[0], 3) == 0x80000000u);
}

static void test_cmpps(void)
{
    float nan = NAN;

    reset_cpu();
    g_cpu.xmm[0] = q4f(1.0f, 2.0f, nan, 4.0f);
    g_cpu.xmm[1] = q4f(1.0f, 9.0f, nan, 4.0f);
    X86Insn in = xi(OCERZ_OP_CMPPS, 3, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(lane_u32(g_cpu.xmm[0], 0) == 0xffffffffu);
    CHECK(lane_u32(g_cpu.xmm[0], 1) == 0);
    CHECK(lane_u32(g_cpu.xmm[0], 2) == 0);
    CHECK(lane_u32(g_cpu.xmm[0], 3) == 0xffffffffu);

    reset_cpu();
    g_cpu.xmm[0] = q4f(1.0f, nan, 3.0f, nan);
    g_cpu.xmm[1] = q4f(2.0f, 2.0f, nan, nan);
    in = xi(OCERZ_OP_CMPPS, 3, xreg(0, 16), xreg(1, 16), imm(3));
    run(&in);
    CHECK(lane_u32(g_cpu.xmm[0], 0) == 0);
    CHECK(lane_u32(g_cpu.xmm[0], 1) == 0xffffffffu);
    CHECK(lane_u32(g_cpu.xmm[0], 2) == 0xffffffffu);
    CHECK(lane_u32(g_cpu.xmm[0], 3) == 0xffffffffu);

    reset_cpu();
    g_cpu.xmm[0] = q4f(1.0f, 2.0f, nan, 4.0f);
    g_cpu.xmm[1] = q4f(1.0f, 9.0f, 5.0f, 4.0f);
    in = xi(OCERZ_OP_CMPPS, 3, xreg(0, 16), xreg(1, 16), imm(4));
    run(&in);
    CHECK(lane_u32(g_cpu.xmm[0], 0) == 0);
    CHECK(lane_u32(g_cpu.xmm[0], 1) == 0xffffffffu);
    CHECK(lane_u32(g_cpu.xmm[0], 2) == 0xffffffffu);

    reset_cpu();
    g_cpu.xmm[0] = q4f(1.0f, nan, 5.0f, 2.0f);
    g_cpu.xmm[1] = q4f(2.0f, 3.0f, 1.0f, 2.0f);
    in = xi(OCERZ_OP_CMPPS, 3, xreg(0, 16), xreg(1, 16), imm(1));
    run(&in);
    CHECK(lane_u32(g_cpu.xmm[0], 0) == 0xffffffffu);
    CHECK(lane_u32(g_cpu.xmm[0], 1) == 0);
    CHECK(lane_u32(g_cpu.xmm[0], 2) == 0);
    reset_cpu();
    g_cpu.xmm[0] = q4f(1.0f, nan, 5.0f, 2.0f);
    g_cpu.xmm[1] = q4f(2.0f, 3.0f, 1.0f, 2.0f);
    in = xi(OCERZ_OP_CMPPS, 3, xreg(0, 16), xreg(1, 16), imm(5));
    run(&in);
    CHECK(lane_u32(g_cpu.xmm[0], 0) == 0);
    CHECK(lane_u32(g_cpu.xmm[0], 1) == 0xffffffffu);
    CHECK(lane_u32(g_cpu.xmm[0], 2) == 0xffffffffu);
}

static void test_comiss(void)
{
    float nan = NAN;

    reset_cpu();
    g_cpu.xmm[0] = q4f(3.0f, 0, 0, 0);
    g_cpu.xmm[1] = q4f(3.0f, 0, 0, 0);
    X86Insn in = xi(OCERZ_OP_COMISS, 2, xreg(0, 4), xreg(1, 4), imm(0));
    run(&in);
    CHECK((g_cpu.rflags & OCERZ_ZF) != 0);
    CHECK((g_cpu.rflags & OCERZ_PF) == 0);
    CHECK((g_cpu.rflags & OCERZ_CF) == 0);
    CHECK((g_cpu.rflags & (OCERZ_OF | OCERZ_SF | OCERZ_AF)) == 0);

    reset_cpu();
    g_cpu.xmm[0] = q4f(1.0f, 0, 0, 0);
    g_cpu.xmm[1] = q4f(2.0f, 0, 0, 0);
    in = xi(OCERZ_OP_COMISS, 2, xreg(0, 4), xreg(1, 4), imm(0));
    run(&in);
    CHECK((g_cpu.rflags & OCERZ_CF) != 0);
    CHECK((g_cpu.rflags & OCERZ_ZF) == 0);
    CHECK((g_cpu.rflags & OCERZ_PF) == 0);

    reset_cpu();
    g_cpu.xmm[0] = q4f(5.0f, 0, 0, 0);
    g_cpu.xmm[1] = q4f(2.0f, 0, 0, 0);
    in = xi(OCERZ_OP_COMISS, 2, xreg(0, 4), xreg(1, 4), imm(0));
    run(&in);
    CHECK((g_cpu.rflags & (OCERZ_ZF | OCERZ_PF | OCERZ_CF)) == 0);

    reset_cpu();
    g_cpu.xmm[0] = q4f(nan, 0, 0, 0);
    g_cpu.xmm[1] = q4f(2.0f, 0, 0, 0);
    in = xi(OCERZ_OP_UCOMISS, 2, xreg(0, 4), xreg(1, 4), imm(0));
    run(&in);
    CHECK((g_cpu.rflags & OCERZ_ZF) != 0);
    CHECK((g_cpu.rflags & OCERZ_PF) != 0);
    CHECK((g_cpu.rflags & OCERZ_CF) != 0);

    reset_cpu();
    g_cpu.xmm[0] = q2d(2.5, 0);
    g_cpu.xmm[1] = q2d(2.5, 0);
    in = xi(OCERZ_OP_UCOMISD, 2, xreg(0, 8), xreg(1, 8), imm(0));
    run(&in);
    CHECK((g_cpu.rflags & OCERZ_ZF) != 0);
    CHECK((g_cpu.rflags & OCERZ_CF) == 0);
}

static void test_converts(void)
{

    reset_cpu();
    g_cpu.xmm[1] = q4f(3.9f, 0, 0, 0);
    X86Insn in = xi(OCERZ_OP_CVTTSS2SI, 2, greg(OCERZ_RAX, 4), xreg(1, 4), imm(0));
    run(&in);
    CHECK((int32_t)g_cpu.gpr[OCERZ_RAX] == 3);
    reset_cpu();
    g_cpu.xmm[1] = q4f(-3.9f, 0, 0, 0);
    in = xi(OCERZ_OP_CVTTSS2SI, 2, greg(OCERZ_RAX, 4), xreg(1, 4), imm(0));
    run(&in);
    CHECK((int32_t)g_cpu.gpr[OCERZ_RAX] == -3);

    reset_cpu();
    g_cpu.xmm[1] = q4f(2.5f, 0, 0, 0);
    in = xi(OCERZ_OP_CVTSS2SI, 2, greg(OCERZ_RAX, 4), xreg(1, 4), imm(0));
    run(&in);
    CHECK((int32_t)g_cpu.gpr[OCERZ_RAX] == 2);

    reset_cpu();
    g_cpu.xmm[1] = q2d(1e300, 0);
    in = xi(OCERZ_OP_CVTTSD2SI, 2, greg(OCERZ_RAX, 4), xreg(1, 8), imm(0));
    run(&in);
    CHECK((uint32_t)g_cpu.gpr[OCERZ_RAX] == 0x80000000u);

    reset_cpu();
    g_cpu.xmm[1] = q2d(NAN, 0);
    in = xi(OCERZ_OP_CVTTSD2SI, 2, greg(OCERZ_RAX, 8), xreg(1, 8), imm(0));
    run(&in);
    CHECK(g_cpu.gpr[OCERZ_RAX] == 0x8000000000000000ull);

    reset_cpu();
    g_cpu.gpr[OCERZ_RCX] = (uint64_t)(int64_t)-5;
    in = xi(OCERZ_OP_CVTSI2SD, 2, xreg(0, 8), greg(OCERZ_RCX, 8), imm(0));
    run(&in);
    CHECK(lane_d(g_cpu.xmm[0], 0) == -5.0);

    reset_cpu();
    {
        union { Ocerz128 q; int32_t i[4]; } u;
        u.i[0] = 1; u.i[1] = -2; u.i[2] = 100; u.i[3] = -7;
        g_cpu.xmm[1] = u.q;
    }
    in = xi(OCERZ_OP_CVTDQ2PS, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(lane_f(g_cpu.xmm[0], 1) == -2.0f && lane_f(g_cpu.xmm[0], 2) == 100.0f);

    reset_cpu();
    g_cpu.xmm[1] = q4f(1.9f, -1.9f, 1e30f, 0.0f);
    in = xi(OCERZ_OP_CVTTPS2DQ, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK((int32_t)lane_u32(g_cpu.xmm[0], 0) == 1);
    CHECK((int32_t)lane_u32(g_cpu.xmm[0], 1) == -1);
    CHECK(lane_u32(g_cpu.xmm[0], 2) == 0x80000000u);
}

static void test_int_arith(void)
{

    reset_cpu();
    {
        union { Ocerz128 q; uint32_t u[4]; } a, b;
        a.u[0] = 0xffffffffu; a.u[1] = 1; a.u[2] = 0; a.u[3] = 7;
        b.u[0] = 1; b.u[1] = 2; b.u[2] = 3; b.u[3] = 4;
        g_cpu.xmm[0] = a.q; g_cpu.xmm[1] = b.q;
    }
    X86Insn in = xi(OCERZ_OP_PADDD, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(lane_u32(g_cpu.xmm[0], 0) == 0);
    CHECK(lane_u32(g_cpu.xmm[0], 3) == 11);

    reset_cpu();
    {
        union { Ocerz128 q; int16_t i[8]; } a, b;
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        a.i[0] = 0x7fff; b.i[0] = 1;
        a.i[1] = -0x8000; b.i[1] = -1;
        g_cpu.xmm[0] = a.q; g_cpu.xmm[1] = b.q;
    }
    in = xi(OCERZ_OP_PADDSW, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK((int16_t)lane_u16(g_cpu.xmm[0], 0) == 0x7fff);
    CHECK((int16_t)lane_u16(g_cpu.xmm[0], 1) == -0x8000);

    reset_cpu();
    {
        union { Ocerz128 q; uint8_t u[16]; } a, b;
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        a.u[0] = 0x10; b.u[0] = 0x80;
        a.u[1] = 0xff; b.u[1] = 0x01;
        g_cpu.xmm[0] = a.q; g_cpu.xmm[1] = b.q;
    }
    in = xi(OCERZ_OP_PSUBUSB, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(lane_u8(g_cpu.xmm[0], 0) == 0);
    CHECK(lane_u8(g_cpu.xmm[0], 1) == 0xfe);

    reset_cpu();
    {
        union { Ocerz128 q; int16_t i[8]; } a, b;
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        a.i[0] = 0x4000; b.i[0] = 0x4000;
        g_cpu.xmm[0] = a.q; g_cpu.xmm[1] = b.q;
    }
    in = xi(OCERZ_OP_PMULHW, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(lane_u16(g_cpu.xmm[0], 0) == 0x1000);

    reset_cpu();
    {
        union { Ocerz128 q; uint8_t u[16]; } a, b;
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        a.u[0] = 3; b.u[0] = 4;
        a.u[1] = 255; b.u[1] = 255;
        g_cpu.xmm[0] = a.q; g_cpu.xmm[1] = b.q;
    }
    in = xi(OCERZ_OP_PAVGB, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(lane_u8(g_cpu.xmm[0], 0) == 4);
    CHECK(lane_u8(g_cpu.xmm[0], 1) == 255);

    reset_cpu();
    {
        union { Ocerz128 q; uint8_t u[16]; } a, b;
        for (int i = 0; i < 16; i++) { a.u[i] = (uint8_t)(i + 10); b.u[i] = (uint8_t)i; }
        g_cpu.xmm[0] = a.q; g_cpu.xmm[1] = b.q;
    }
    in = xi(OCERZ_OP_PSADBW, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(lane_u16(g_cpu.xmm[0], 0) == 80);
    CHECK(lane_u16(g_cpu.xmm[0], 4) == 80);
    CHECK(lane_u16(g_cpu.xmm[0], 1) == 0);

    reset_cpu();
    {
        union { Ocerz128 q; int32_t i[4]; } b;
        b.i[0] = -5; b.i[1] = 6; b.i[2] = -2147483647; b.i[3] = 0;
        g_cpu.xmm[1] = b.q;
    }
    in = xi(OCERZ_OP_PABSD, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(lane_u32(g_cpu.xmm[0], 0) == 5);
    CHECK(lane_u32(g_cpu.xmm[0], 2) == 2147483647u);
}

static void test_pack_shuffle(void)
{

    reset_cpu();
    {
        union { Ocerz128 q; int16_t i[8]; } a, b;
        for (int i = 0; i < 8; i++) { a.i[i] = 0; b.i[i] = 0; }
        a.i[0] = 200; a.i[1] = -200; a.i[2] = 50;
        b.i[0] = 1000; b.i[1] = -1000;
        g_cpu.xmm[0] = a.q; g_cpu.xmm[1] = b.q;
    }
    X86Insn in = xi(OCERZ_OP_PACKSSWB, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK((int8_t)lane_u8(g_cpu.xmm[0], 0) == 127);
    CHECK((int8_t)lane_u8(g_cpu.xmm[0], 1) == -128);
    CHECK((int8_t)lane_u8(g_cpu.xmm[0], 2) == 50);
    CHECK((int8_t)lane_u8(g_cpu.xmm[0], 8) == 127);
    CHECK((int8_t)lane_u8(g_cpu.xmm[0], 9) == -128);

    reset_cpu();
    {
        union { Ocerz128 q; uint8_t u[16]; } a, b;
        for (int i = 0; i < 16; i++) { a.u[i] = (uint8_t)(0x10 + i); b.u[i] = (uint8_t)(0xa0 + i); }
        g_cpu.xmm[0] = a.q; g_cpu.xmm[1] = b.q;
    }
    in = xi(OCERZ_OP_PUNPCKLBW, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(lane_u8(g_cpu.xmm[0], 0) == 0x10);
    CHECK(lane_u8(g_cpu.xmm[0], 1) == 0xa0);
    CHECK(lane_u8(g_cpu.xmm[0], 2) == 0x11);
    CHECK(lane_u8(g_cpu.xmm[0], 3) == 0xa1);

    reset_cpu();
    {
        union { Ocerz128 q; uint32_t u[4]; } b;
        b.u[0] = 0xA; b.u[1] = 0xB; b.u[2] = 0xC; b.u[3] = 0xD;
        g_cpu.xmm[1] = b.q;
    }
    in = xi(OCERZ_OP_PSHUFD, 3, xreg(0, 16), xreg(1, 16), imm(0x1B));
    run(&in);
    CHECK(lane_u32(g_cpu.xmm[0], 0) == 0xD);
    CHECK(lane_u32(g_cpu.xmm[0], 1) == 0xC);
    CHECK(lane_u32(g_cpu.xmm[0], 2) == 0xB);
    CHECK(lane_u32(g_cpu.xmm[0], 3) == 0xA);

    reset_cpu();
    {
        union { Ocerz128 q; uint8_t u[16]; } a, ctl;
        for (int i = 0; i < 16; i++) { a.u[i] = (uint8_t)(0xf0 + i); ctl.u[i] = 0; }
        ctl.u[0] = 0x03;
        ctl.u[1] = 0x80;
        ctl.u[2] = 0x1f;
        g_cpu.xmm[0] = a.q; g_cpu.xmm[1] = ctl.q;
    }
    in = xi(OCERZ_OP_PSHUFB, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(lane_u8(g_cpu.xmm[0], 0) == 0xf3);
    CHECK(lane_u8(g_cpu.xmm[0], 1) == 0x00);
    CHECK(lane_u8(g_cpu.xmm[0], 2) == 0xff);

    reset_cpu();
    {
        union { Ocerz128 q; uint8_t u[16]; } a, b;
        for (int i = 0; i < 16; i++) { a.u[i] = (uint8_t)(0x80 + i); b.u[i] = (uint8_t)i; }
        g_cpu.xmm[0] = a.q; g_cpu.xmm[1] = b.q;
    }
    in = xi(OCERZ_OP_PALIGNR, 3, xreg(0, 16), xreg(1, 16), imm(3));
    run(&in);

    CHECK(lane_u8(g_cpu.xmm[0], 0) == 3);

    CHECK(lane_u8(g_cpu.xmm[0], 13) == 0x80);

    CHECK(lane_u8(g_cpu.xmm[0], 15) == 0x82);

    reset_cpu();
    g_cpu.xmm[0] = q4f(1.0f, 2.0f, 3.0f, 4.0f);
    g_cpu.xmm[1] = q4f(5.0f, 6.0f, 7.0f, 8.0f);
    in = xi(OCERZ_OP_SHUFPS, 3, xreg(0, 16), xreg(1, 16), imm(0x88));
    run(&in);

    CHECK(lane_f(g_cpu.xmm[0], 0) == 1.0f);
    CHECK(lane_f(g_cpu.xmm[0], 1) == 3.0f);
    CHECK(lane_f(g_cpu.xmm[0], 2) == 5.0f);
    CHECK(lane_f(g_cpu.xmm[0], 3) == 7.0f);
}

static void test_shifts(void)
{

    reset_cpu();
    {
        union { Ocerz128 q; uint32_t u[4]; } a;
        a.u[0] = 1; a.u[1] = 0x80000000u; a.u[2] = 0xff; a.u[3] = 2;
        g_cpu.xmm[0] = a.q;
    }
    X86Insn in = xi(OCERZ_OP_PSLLD, 2, xreg(0, 16), imm(4), imm(0));
    run(&in);
    CHECK(lane_u32(g_cpu.xmm[0], 0) == 0x10);
    CHECK(lane_u32(g_cpu.xmm[0], 1) == 0);

    reset_cpu();
    g_cpu.xmm[0].lo = 0x1111111122222222ull;
    g_cpu.xmm[0].hi = 0x3333333344444444ull;
    in = xi(OCERZ_OP_PSLLD, 2, xreg(0, 16), imm(40), imm(0));
    run(&in);
    CHECK(g_cpu.xmm[0].lo == 0 && g_cpu.xmm[0].hi == 0);

    reset_cpu();
    {
        union { Ocerz128 q; int32_t i[4]; } a;
        a.i[0] = -16; a.i[1] = 16; a.i[2] = -1; a.i[3] = 0;
        g_cpu.xmm[0] = a.q;
    }
    in = xi(OCERZ_OP_PSRAD, 2, xreg(0, 16), imm(2), imm(0));
    run(&in);
    CHECK((int32_t)lane_u32(g_cpu.xmm[0], 0) == -4);
    CHECK((int32_t)lane_u32(g_cpu.xmm[0], 1) == 4);

    reset_cpu();
    {
        union { Ocerz128 q; int32_t i[4]; } a;
        a.i[0] = -1; a.i[1] = 5; a.i[2] = 0; a.i[3] = 0;
        g_cpu.xmm[0] = a.q;
    }
    in = xi(OCERZ_OP_PSRAD, 2, xreg(0, 16), imm(50), imm(0));
    run(&in);
    CHECK(lane_u32(g_cpu.xmm[0], 0) == 0xffffffffu);
    CHECK(lane_u32(g_cpu.xmm[0], 1) == 0);

    reset_cpu();
    {
        uint8_t bb[16];
        for (int i = 0; i < 16; i++) bb[i] = (uint8_t)(i + 1);
        g_cpu.xmm[0] = q16b(bb);
    }
    in = xi(OCERZ_OP_PSLLDQ, 2, xreg(0, 16), imm(3), imm(0));
    run(&in);
    CHECK(lane_u8(g_cpu.xmm[0], 0) == 0);
    CHECK(lane_u8(g_cpu.xmm[0], 2) == 0);
    CHECK(lane_u8(g_cpu.xmm[0], 3) == 1);
    CHECK(lane_u8(g_cpu.xmm[0], 15) == 13);

    reset_cpu();
    {
        uint8_t bb[16];
        for (int i = 0; i < 16; i++) bb[i] = (uint8_t)(i + 1);
        g_cpu.xmm[0] = q16b(bb);
    }
    in = xi(OCERZ_OP_PSRLDQ, 2, xreg(0, 16), imm(5), imm(0));
    run(&in);
    CHECK(lane_u8(g_cpu.xmm[0], 0) == 6);
    CHECK(lane_u8(g_cpu.xmm[0], 10) == 16);
    CHECK(lane_u8(g_cpu.xmm[0], 11) == 0);
    CHECK(lane_u8(g_cpu.xmm[0], 15) == 0);

    reset_cpu();
    g_cpu.xmm[0].lo = ~0ull; g_cpu.xmm[0].hi = ~0ull;
    in = xi(OCERZ_OP_PSLLDQ, 2, xreg(0, 16), imm(20), imm(0));
    run(&in);
    CHECK(g_cpu.xmm[0].lo == 0 && g_cpu.xmm[0].hi == 0);
}

static void test_logic_pcmp(void)
{

    reset_cpu();
    g_cpu.xmm[0].lo = 0xff00ff00ff00ff00ull; g_cpu.xmm[0].hi = 0;
    g_cpu.xmm[1].lo = 0x0f0f0f0f0f0f0f0full; g_cpu.xmm[1].hi = ~0ull;
    X86Insn in = xi(OCERZ_OP_ANDNPS, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(g_cpu.xmm[0].lo == (~0xff00ff00ff00ff00ull & 0x0f0f0f0f0f0f0f0full));
    CHECK(g_cpu.xmm[0].hi == ~0ull);

    reset_cpu();
    g_cpu.xmm[3].lo = 0x123; g_cpu.xmm[3].hi = 0x456;
    in = xi(OCERZ_OP_PXOR, 2, xreg(3, 16), xreg(3, 16), imm(0));
    run(&in);
    CHECK(g_cpu.xmm[3].lo == 0 && g_cpu.xmm[3].hi == 0);

    reset_cpu();
    {
        union { Ocerz128 q; int8_t i[16]; } a, b;
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        a.i[0] = -1; b.i[0] = 0;
        a.i[1] = 5; b.i[1] = -3;
        g_cpu.xmm[0] = a.q; g_cpu.xmm[1] = b.q;
    }
    in = xi(OCERZ_OP_PCMPGTB, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(lane_u8(g_cpu.xmm[0], 0) == 0);
    CHECK(lane_u8(g_cpu.xmm[0], 1) == 0xff);

    reset_cpu();
    {
        union { Ocerz128 q; uint32_t u[4]; } a, b;
        a.u[0] = 5; a.u[1] = 6; a.u[2] = 7; a.u[3] = 8;
        b.u[0] = 5; b.u[1] = 0; b.u[2] = 7; b.u[3] = 0;
        g_cpu.xmm[0] = a.q; g_cpu.xmm[1] = b.q;
    }
    in = xi(OCERZ_OP_PCMPEQD, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(lane_u32(g_cpu.xmm[0], 0) == 0xffffffffu);
    CHECK(lane_u32(g_cpu.xmm[0], 1) == 0);
    CHECK(lane_u32(g_cpu.xmm[0], 2) == 0xffffffffu);
}

static void test_pmovmskb(void)
{
    reset_cpu();
    {
        union { Ocerz128 q; uint8_t u[16]; } a;
        for (int i = 0; i < 16; i++) a.u[i] = (i % 2) ? 0x80 : 0x00;
        g_cpu.xmm[2] = a.q;
    }
    g_cpu.gpr[OCERZ_RDX] = ~0ull;
    X86Insn in = xi(OCERZ_OP_PMOVMSKB, 2, greg(OCERZ_RDX, 4), xreg(2, 16), imm(0));
    run(&in);
    CHECK(g_cpu.gpr[OCERZ_RDX] == 0xAAAA);
}

static void test_insert_extract(void)
{

    reset_cpu();
    {
        union { Ocerz128 q; uint8_t u[16]; } a;
        for (int i = 0; i < 16; i++) a.u[i] = (uint8_t)(0x10 + i);
        g_cpu.xmm[1] = a.q;
    }
    g_cpu.gpr[OCERZ_RAX] = ~0ull;
    X86Insn in = xi(OCERZ_OP_PEXTRB, 3, greg(OCERZ_RAX, 4), xreg(1, 16), imm(5));
    run(&in);
    CHECK(g_cpu.gpr[OCERZ_RAX] == 0x15);

    reset_cpu();
    g_cpu.gpr[OCERZ_RCX] = 0xdeadbeef;
    g_cpu.xmm[0].lo = 0; g_cpu.xmm[0].hi = 0;
    in = xi(OCERZ_OP_PINSRW, 3, xreg(0, 16), greg(OCERZ_RCX, 4), imm(2));
    run(&in);
    CHECK(lane_u16(g_cpu.xmm[0], 2) == 0xbeef);
    CHECK(lane_u16(g_cpu.xmm[0], 0) == 0);

    reset_cpu();
    {
        union { Ocerz128 q; uint32_t u[4]; } a;
        a.u[0] = 1; a.u[1] = 2; a.u[2] = 0xcafef00d; a.u[3] = 4;
        g_cpu.xmm[1] = a.q;
    }
    in = xi(OCERZ_OP_PEXTRD, 3, mem(g_scratch, 4), xreg(1, 16), imm(2));
    run(&in);
    CHECK((uint32_t)ocerz_ld(g_scratch, 4) == 0xcafef00d);

    reset_cpu();
    g_cpu.xmm[0] = q4f(1.0f, 2.0f, 3.0f, 4.0f);
    g_cpu.xmm[1] = q4f(10.0f, 20.0f, 30.0f, 40.0f);

    in = xi(OCERZ_OP_INSERTPS, 3, xreg(0, 16), xreg(1, 16), imm((2 << 6) | (1 << 4) | 0x1));
    run(&in);
    CHECK(lane_u32(g_cpu.xmm[0], 0) == 0);
    CHECK(lane_f(g_cpu.xmm[0], 1) == 30.0f);
    CHECK(lane_f(g_cpu.xmm[0], 2) == 3.0f);
    CHECK(lane_f(g_cpu.xmm[0], 3) == 4.0f);

    reset_cpu();
    g_cpu.xmm[1] = q4f(1.0f, 2.0f, 3.0f, 4.0f);
    g_cpu.gpr[OCERZ_RAX] = ~0ull;
    in = xi(OCERZ_OP_EXTRACTPS, 3, greg(OCERZ_RAX, 4), xreg(1, 16), imm(3));
    run(&in);
    CHECK((uint32_t)g_cpu.gpr[OCERZ_RAX] == 0x40800000u);
    CHECK(g_cpu.gpr[OCERZ_RAX] == 0x40800000u);
}

static void test_ptest_blend(void)
{

    reset_cpu();
    g_cpu.xmm[0].lo = 0x0f; g_cpu.xmm[0].hi = 0;
    g_cpu.xmm[1].lo = 0xf0; g_cpu.xmm[1].hi = 0;
    X86Insn in = xi(OCERZ_OP_PTEST, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK((g_cpu.rflags & OCERZ_ZF) != 0);
    CHECK((g_cpu.rflags & OCERZ_CF) == 0);

    reset_cpu();
    g_cpu.xmm[0].lo = ~0ull; g_cpu.xmm[0].hi = ~0ull;
    g_cpu.xmm[1].lo = 0x55; g_cpu.xmm[1].hi = 0;
    in = xi(OCERZ_OP_PTEST, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK((g_cpu.rflags & OCERZ_ZF) == 0);
    CHECK((g_cpu.rflags & OCERZ_CF) != 0);

    reset_cpu();
    g_cpu.xmm[0] = q4f(1.0f, 2.0f, 3.0f, 4.0f);
    g_cpu.xmm[1] = q4f(5.0f, 6.0f, 7.0f, 8.0f);
    in = xi(OCERZ_OP_BLENDPS, 3, xreg(0, 16), xreg(1, 16), imm(0x5));
    run(&in);
    CHECK(lane_f(g_cpu.xmm[0], 0) == 5.0f);
    CHECK(lane_f(g_cpu.xmm[0], 1) == 2.0f);
    CHECK(lane_f(g_cpu.xmm[0], 2) == 7.0f);
    CHECK(lane_f(g_cpu.xmm[0], 3) == 4.0f);

    reset_cpu();
    {
        union { Ocerz128 q; uint16_t u[8]; } a, b;
        for (int i = 0; i < 8; i++) { a.u[i] = (uint16_t)(0x100 + i); b.u[i] = (uint16_t)(0x200 + i); }
        g_cpu.xmm[0] = a.q; g_cpu.xmm[1] = b.q;
    }
    in = xi(OCERZ_OP_PBLENDW, 3, xreg(0, 16), xreg(1, 16), imm(0x81));
    run(&in);
    CHECK(lane_u16(g_cpu.xmm[0], 0) == 0x200);
    CHECK(lane_u16(g_cpu.xmm[0], 1) == 0x101);
    CHECK(lane_u16(g_cpu.xmm[0], 7) == 0x207);
}

static void test_pmovzx_round(void)
{

    reset_cpu();
    {
        union { Ocerz128 q; uint8_t u[16]; } a;
        for (int i = 0; i < 16; i++) a.u[i] = (uint8_t)(0x80 + i);
        g_cpu.xmm[1] = a.q;
    }
    X86Insn in = xi(OCERZ_OP_PMOVZXBW, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK(lane_u16(g_cpu.xmm[0], 0) == 0x80);
    CHECK(lane_u16(g_cpu.xmm[0], 7) == 0x87);

    reset_cpu();
    {
        union { Ocerz128 q; int8_t i[16]; } a;
        memset(&a, 0, sizeof a);
        a.i[0] = -1; a.i[1] = 127; a.i[2] = -128;
        g_cpu.xmm[1] = a.q;
    }
    in = xi(OCERZ_OP_PMOVSXBW, 2, xreg(0, 16), xreg(1, 16), imm(0));
    run(&in);
    CHECK((int16_t)lane_u16(g_cpu.xmm[0], 0) == -1);
    CHECK((int16_t)lane_u16(g_cpu.xmm[0], 1) == 127);
    CHECK((int16_t)lane_u16(g_cpu.xmm[0], 2) == -128);

    reset_cpu();
    g_cpu.xmm[0] = q2d(7.0, 8.0);
    g_cpu.xmm[1] = q2d(-2.5, 99.0);
    in = xi(OCERZ_OP_ROUNDSD, 3, xreg(0, 16), xreg(1, 16), imm(1));
    run(&in);
    CHECK(lane_d(g_cpu.xmm[0], 0) == -3.0);
    CHECK(lane_d(g_cpu.xmm[0], 1) == 8.0);

    reset_cpu();
    g_cpu.xmm[0] = q2d(0.0, 0.0);
    g_cpu.xmm[1] = q2d(-2.9, 0.0);
    in = xi(OCERZ_OP_ROUNDSD, 3, xreg(0, 16), xreg(1, 16), imm(3));
    run(&in);
    CHECK(lane_d(g_cpu.xmm[0], 0) == -2.0);

    reset_cpu();
    g_cpu.xmm[1] = q4f(1.1f, -1.1f, 2.5f, -2.5f);
    in = xi(OCERZ_OP_ROUNDPS, 3, xreg(0, 16), xreg(1, 16), imm(2));
    run(&in);
    CHECK(lane_f(g_cpu.xmm[0], 0) == 2.0f);
    CHECK(lane_f(g_cpu.xmm[0], 1) == -1.0f);
    CHECK(lane_f(g_cpu.xmm[0], 3) == -2.0f);
}

static int setup_memory(void)
{
    if (ocerz_mem_init(0x100000000ull, 0x900000000ull) == OCERZ_OK) {
        g_scratch = ocerz_map_anywhere(0x4000, PROT_READ | PROT_WRITE);
        if (g_scratch)
            return 0;
    }
    void *p = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    g_scratch = 0x100000000ull;
    ocerz_guest_base = (uint64_t)(uintptr_t)p - g_scratch;
    ocerz_arena_lo = g_scratch;
    ocerz_arena_hi = g_scratch + 0x4000;
    return 0;
}

int main(void)
{
    if (setup_memory() != 0) {
        fprintf(stderr, "mem init failed\n");
        return 2;
    }

    test_below_sse_first();
    test_moves();
    test_fp_arith();
    test_minmax_nan();
    test_cmpps();
    test_comiss();
    test_converts();
    test_int_arith();
    test_pack_shuffle();
    test_shifts();
    test_logic_pcmp();
    test_pmovmskb();
    test_insert_extract();
    test_ptest_blend();
    test_pmovzx_round();

    fprintf(stderr, "test_sse: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

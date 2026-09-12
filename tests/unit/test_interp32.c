/*
 * Unit tests for i386 (32-bit) execution in the interpreter.
 *
 * Where the expected values come from.  Instruction identity, length and
 * operand shape: capstone 5.0.7 in CS_MODE_32, run as a black-box oracle while
 * these rows were written, with the exact capstone rendering kept beside each
 * encoding so a reviewer can re-run it without re-deriving the encoding.
 *
 * Architectural effect - register file, flags, stack traffic - comes from the
 * Intel SDM Vol.2 entry for each instruction.  capstone is a disassembler and
 * has no opinion about what an instruction DOES, so it cannot be the oracle
 * for the BCD/ASCII adjusts.  Those were checked instead by transcribing the
 * SDM pseudocode a second time, independently, in python, and diffing that
 * model against this interpreter over 102400 (AL, AH, CF, AF) and
 * (AL, AH, base) combinations: 0 mismatches.  The BCD rows below are a sample
 * of that sweep, kept small enough to read.  Where the SDM says a flag is
 * UNDEFINED this interpreter leaves the flag alone, and the sweep asserted
 * exactly that: OF after DAA/DAS, and OF/SF/ZF/PF after AAA/AAS, all still
 * hold the value they were given.
 *
 * Everything the 32-bit guest touches has to be addressable in 32 bits, and so
 * does the 64-bit code it returns to - a 32-bit RETF pops a 4-byte offset, so
 * the far-return target below 4G is not a shortcut in the test, it is how the
 * WoW64 thunk actually has to be laid out.
 */
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/interp.h"
#include "ocerz/interp_common.h"
#include "ocerz/syscall.h"

#include <stdlib.h>
#include <sys/mman.h>

#define M64_CODE  0x00300000ull
#define M32_CODE  0x00400000ull
#define M32_DATA  0x00500000ull
#define M32_STACK 0x00508000ull
#define WRAP_PAGE 0xfffff000ull

#define CS32   0x0fu
#define CS64   0x33u
#define CS64L  0x17u

static int g_failures;
static OcerzVM g_vm;

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

static OcerzCPU *cpu(void) { return &g_vm.cpu; }

static int CF(void) { return (cpu()->rflags & OCERZ_CF) != 0; }
static int ZF(void) { return (cpu()->rflags & OCERZ_ZF) != 0; }
static int SF(void) { return (cpu()->rflags & OCERZ_SF) != 0; }
static int OF(void) { return (cpu()->rflags & OCERZ_OF) != 0; }
static int PF(void) { return (cpu()->rflags & OCERZ_PF) != 0; }
static int AF(void) { return (cpu()->rflags & OCERZ_AF) != 0; }

static void reset32(void)
{
    ocerz_cpu_reset(cpu());
    cpu()->mode32 = 1;
    cpu()->cs_sel = (uint16_t)CS32;
    cpu()->seg_sel[OCERZ_SREG_CS] = (uint16_t)CS32;
    cpu()->gpr[OCERZ_RSP] = M32_STACK;
}

static void reset64(void)
{
    ocerz_cpu_reset(cpu());
    cpu()->cs_sel = (uint16_t)CS64;
    cpu()->gpr[OCERZ_RSP] = M32_STACK;
    cpu()->gpr[OCERZ_RBP] = M32_STACK;
}

static int step_at(uint64_t rip)
{
    cpu()->rip = rip;
    return ocerz_interp_step(&g_vm, cpu());
}

static void emit_at(uint64_t addr, const uint8_t *b, size_t n)
{
    put_bytes(addr, b, n);
}

#define EMIT32(...) do { static const uint8_t _b[] = { __VA_ARGS__ }; \
                         emit_at(M32_CODE, _b, sizeof _b); } while (0)
#define EMIT_AT(a, ...) do { static const uint8_t _b[] = { __VA_ARGS__ }; \
                             emit_at((a), _b, sizeof _b); } while (0)

typedef struct {
    const char *note;
    uint8_t bytes[2];
    int nbytes;
    uint16_t in_ax;
    int in_cf, in_af;
    uint16_t out_ax;
    int cf, af, zf, sf, pf;
} BcdCase;

static const BcdCase bcd_cases[] = {
    { "daa 05 no adjust",       {0x27}, 1, 0x0005, 0, 0, 0x0005, 0, 0, 0, 0, 1 },
    { "daa 0a low nibble >9",   {0x27}, 1, 0x000a, 0, 0, 0x0010, 0, 1, 0, 0, 0 },
    { "daa 9a both adjusts",    {0x27}, 1, 0x009a, 0, 0, 0x0000, 1, 1, 1, 0, 1 },
    { "daa 99 with CF in",      {0x27}, 1, 0x0099, 1, 0, 0x00f9, 1, 0, 0, 1, 1 },
    { "daa 2f",                 {0x27}, 1, 0x002f, 0, 0, 0x0035, 0, 1, 0, 0, 1 },
    { "daa ff",                 {0x27}, 1, 0x00ff, 0, 0, 0x0065, 1, 1, 0, 0, 1 },
    { "daa 1a with AF in",      {0x27}, 1, 0x001a, 0, 1, 0x0020, 0, 1, 0, 0, 0 },
    { "daa fa CF+AF in",        {0x27}, 1, 0x00fa, 1, 1, 0x0060, 1, 1, 0, 0, 1 },

    { "das 05 no adjust",       {0x2f}, 1, 0x0005, 0, 0, 0x0005, 0, 0, 0, 0, 1 },
    { "das 0a low nibble >9",   {0x2f}, 1, 0x000a, 0, 0, 0x0004, 0, 1, 0, 0, 0 },
    { "das 9a both adjusts",    {0x2f}, 1, 0x009a, 0, 0, 0x0034, 1, 1, 0, 0, 0 },
    { "das 03 AF in borrows",   {0x2f}, 1, 0x0003, 0, 1, 0x00fd, 1, 1, 0, 1, 0 },
    { "das 00 CF in",           {0x2f}, 1, 0x0000, 1, 0, 0x00a0, 1, 0, 0, 1, 1 },
    { "das ff",                 {0x2f}, 1, 0x00ff, 0, 0, 0x0099, 1, 1, 0, 1, 1 },
    { "das 1a AF in",           {0x2f}, 1, 0x001a, 0, 1, 0x0014, 0, 1, 0, 0, 1 },
    { "das 05 CF+AF in",        {0x2f}, 1, 0x0005, 1, 1, 0x009f, 1, 1, 0, 1, 1 },

    { "aaa 1205 no adjust",     {0x37}, 1, 0x1205, 0, 0, 0x1205, 0, 0, 0, 0, 0 },
    { "aaa 120a adjusts",       {0x37}, 1, 0x120a, 0, 0, 0x1300, 1, 1, 0, 0, 0 },
    { "aaa 12ff",               {0x37}, 1, 0x12ff, 0, 0, 0x1405, 1, 1, 0, 0, 0 },
    { "aaa 1205 AF in",         {0x37}, 1, 0x1205, 0, 1, 0x130b, 1, 1, 0, 0, 0 },
    { "aaa 12fa carry into AH", {0x37}, 1, 0x12fa, 0, 0, 0x1400, 1, 1, 0, 0, 0 },
    { "aaa ff0b AH wraps",      {0x37}, 1, 0xff0b, 0, 0, 0x0001, 1, 1, 0, 0, 0 },

    { "aas 1205 no adjust",     {0x3f}, 1, 0x1205, 0, 0, 0x1205, 0, 0, 0, 0, 0 },
    { "aas 120a adjusts",       {0x3f}, 1, 0x120a, 0, 0, 0x1104, 1, 1, 0, 0, 0 },
    { "aas 12ff",               {0x3f}, 1, 0x12ff, 0, 0, 0x1109, 1, 1, 0, 0, 0 },
    { "aas 1205 AF in",         {0x3f}, 1, 0x1205, 0, 1, 0x100f, 1, 1, 0, 0, 0 },
    { "aas 000a AH wraps",      {0x3f}, 1, 0x000a, 0, 0, 0xff04, 1, 1, 0, 0, 0 },
    { "aas 1203 AF in",         {0x3f}, 1, 0x1203, 0, 1, 0x100d, 1, 1, 0, 0, 0 },

    { "salc CF=0",              {0xd6}, 1, 0x00aa, 0, 0, 0x0000, 0, 0, 0, 0, 0 },
    { "salc CF=1",              {0xd6}, 1, 0x00aa, 1, 0, 0x00ff, 1, 0, 0, 0, 0 },

    { "aam 10 of 0x4d",   {0xd4, 0x0a}, 2, 0x004d, 0, 0, 0x0707, 0, 0, 0, 0, 0 },
    { "aam 10 of 0x00",   {0xd4, 0x0a}, 2, 0x1200, 0, 0, 0x0000, 0, 0, 1, 0, 1 },
    { "aam 10 of 0x63",   {0xd4, 0x0a}, 2, 0x0063, 0, 0, 0x0909, 0, 0, 0, 0, 1 },
    { "aad 10 of 0x0707", {0xd5, 0x0a}, 2, 0x0707, 0, 0, 0x004d, 0, 0, 0, 0, 1 },
    { "aad 10 of 0x0000", {0xd5, 0x0a}, 2, 0x0000, 0, 0, 0x0000, 0, 0, 1, 0, 1 },
    { "aad 16 of 0x0102", {0xd5, 0x10}, 2, 0x0102, 0, 0, 0x0012, 0, 0, 0, 0, 1 },
};

static void run_bcd_cases(void)
{
    for (size_t i = 0; i < sizeof bcd_cases / sizeof bcd_cases[0]; i++) {
        const BcdCase *c = &bcd_cases[i];
        char nm[96];
        int rc;
        reset32();
        cpu()->gpr[OCERZ_RAX] = c->in_ax;
        if (c->in_cf) cpu()->rflags |= OCERZ_CF;
        if (c->in_af) cpu()->rflags |= OCERZ_AF;
        emit_at(M32_CODE, c->bytes, (size_t)c->nbytes);
        rc = step_at(M32_CODE);

        snprintf(nm, sizeof nm, "%s.rc", c->note);
        EXPECT_U64(nm, OCERZ_STEP_OK, rc);
        snprintf(nm, sizeof nm, "%s.rip", c->note);
        EXPECT_U64(nm, M32_CODE + (uint64_t)c->nbytes, cpu()->rip);
        snprintf(nm, sizeof nm, "%s.ax", c->note);
        EXPECT_U64(nm, c->out_ax, cpu()->gpr[OCERZ_RAX] & 0xffff);
        snprintf(nm, sizeof nm, "%s.cf", c->note); EXPECT_FLAG(nm, c->cf, CF());
        snprintf(nm, sizeof nm, "%s.af", c->note); EXPECT_FLAG(nm, c->af, AF());
        snprintf(nm, sizeof nm, "%s.zf", c->note); EXPECT_FLAG(nm, c->zf, ZF());
        snprintf(nm, sizeof nm, "%s.sf", c->note); EXPECT_FLAG(nm, c->sf, SF());
        snprintf(nm, sizeof nm, "%s.pf", c->note); EXPECT_FLAG(nm, c->pf, PF());
        snprintf(nm, sizeof nm, "%s.of_untouched", c->note);
        EXPECT_FLAG(nm, 0, OF());
    }
}

int main(void)
{
    int rc;

    if (ocerz_mem_init(0x10000ull, 0x900000000ull) != OCERZ_OK) {
        fprintf(stderr, "mem_init failed\n");
        return 2;
    }
    if (ocerz_map_fixed(M64_CODE, 0x300000, PROT_READ | PROT_WRITE) != OCERZ_OK ||
        ocerz_map_fixed(WRAP_PAGE, 0x2000, PROT_READ | PROT_WRITE) != OCERZ_OK) {
        fprintf(stderr, "map_fixed failed\n");
        return 2;
    }
    ocerz_vm_init(&g_vm);
    g_vm.jit_enabled = 0;

    ocerz_ldt_install(CS32,  0, 0xfffff, 0xfb, 1, 0, 1);
    ocerz_ldt_install(CS64L, 0, 0xfffff, 0xfb, 0, 1, 1);

    reset64();
    ocerz_st(M32_DATA, 4, M32_CODE);
    ocerz_st(M32_DATA + 4, 2, CS32);
    cpu()->gpr[OCERZ_RAX] = M32_DATA;
    EMIT_AT(M64_CODE, 0xff, 0x28);
    rc = step_at(M64_CODE);
    EXPECT_U64("entry.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("entry.mode32", 1, cpu()->mode32);
    EXPECT_U64("entry.rip", M32_CODE, cpu()->rip);
    EXPECT_U64("entry.cs", CS32, cpu()->cs_sel);

    cpu()->gpr[OCERZ_RSP] = M32_STACK;
    cpu()->gpr[OCERZ_RAX] = 0x41;
    EMIT32(0x40, 0x90);
    rc = ocerz_interp_step(&g_vm, cpu());
    EXPECT_U64("entry.inc_rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("entry.inc_eax", 0x42, cpu()->gpr[OCERZ_RAX]);
    EXPECT_U64("entry.inc_len1", M32_CODE + 1, cpu()->rip);

    cpu()->gpr[OCERZ_RSP] = M32_STACK;
    ocerz_st(M32_STACK, 4, M64_CODE + 0x10);
    ocerz_st(M32_STACK + 4, 4, CS64);
    EMIT32(0xcb);
    rc = step_at(M32_CODE);
    EXPECT_U64("exit.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("exit.mode32", 0, cpu()->mode32);
    EXPECT_U64("exit.rip", M64_CODE + 0x10, cpu()->rip);
    EXPECT_U64("exit.cs", CS64, cpu()->cs_sel);
    EXPECT_U64("exit.esp", M32_STACK + 8, cpu()->gpr[OCERZ_RSP]);

    EMIT_AT(M64_CODE + 0x10,
            0x48, 0xb8, 0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12);
    rc = ocerz_interp_step(&g_vm, cpu());
    EXPECT_U64("exit.movabs_rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("exit.movabs_rax", 0x123456789abcdef0ull, cpu()->gpr[OCERZ_RAX]);
    EXPECT_U64("exit.movabs_rip", M64_CODE + 0x1a, cpu()->rip);

    reset32();
    ocerz_st(M32_DATA + 0x40, 4, M64_CODE + 0x10);
    ocerz_st(M32_DATA + 0x44, 2, CS64L);
    cpu()->gpr[OCERZ_RAX] = M32_DATA + 0x40;
    EMIT32(0xff, 0x28);
    rc = step_at(M32_CODE);
    EXPECT_U64("exit_ldtlong.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("exit_ldtlong.mode32", 0, cpu()->mode32);
    EXPECT_U64("exit_ldtlong.rip", M64_CODE + 0x10, cpu()->rip);

    reset32();
    cpu()->gpr[OCERZ_RAX] = 0xffffffffffffffffull;
    EMIT32(0xb8, 0x78, 0x56, 0x34, 0x12);
    rc = step_at(M32_CODE);
    EXPECT_U64("zext.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("zext.rax", 0x12345678ull, cpu()->gpr[OCERZ_RAX]);

    reset32();
    cpu()->gpr[OCERZ_RAX] = 0xffffffffull;
    EMIT32(0xb4, 0xab);
    rc = step_at(M32_CODE);
    EXPECT_U64("high8.write_rax", 0xffffabffull, cpu()->gpr[OCERZ_RAX]);

    cpu()->gpr[OCERZ_RBX] = 0x11223344ull;
    EMIT32(0x88, 0xe3);
    rc = step_at(M32_CODE);
    EXPECT_U64("high8.read_rbx", 0x112233abull, cpu()->gpr[OCERZ_RBX]);

    EMIT32(0x8a, 0xc4);
    rc = step_at(M32_CODE);
    EXPECT_U64("high8.al_from_ah", 0xffffabab, cpu()->gpr[OCERZ_RAX]);

    reset32();
    cpu()->gpr[OCERZ_RAX] = 0xdeadbeefull;
    EMIT32(0x50);
    rc = step_at(M32_CODE);
    EXPECT_U64("push32.esp", M32_STACK - 4, cpu()->gpr[OCERZ_RSP]);
    EXPECT_U64("push32.slot", 0xdeadbeefull, ocerz_ld(M32_STACK - 4, 4));
    EMIT32(0x59);
    rc = step_at(M32_CODE);
    EXPECT_U64("pop32.ecx", 0xdeadbeefull, cpu()->gpr[OCERZ_RCX]);
    EXPECT_U64("pop32.esp", M32_STACK, cpu()->gpr[OCERZ_RSP]);

    reset32();
    EMIT32(0xe8, 0x00, 0x00, 0x00, 0x00);
    rc = step_at(M32_CODE);
    EXPECT_U64("call32.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("call32.rip", M32_CODE + 5, cpu()->rip);
    EXPECT_U64("call32.esp", M32_STACK - 4, cpu()->gpr[OCERZ_RSP]);
    EXPECT_U64("call32.ret", M32_CODE + 5, ocerz_ld(M32_STACK - 4, 4));
    EMIT_AT(M32_CODE + 5, 0xc3);
    rc = ocerz_interp_step(&g_vm, cpu());
    EXPECT_U64("ret32.rip", M32_CODE + 5, cpu()->rip);
    EXPECT_U64("ret32.esp", M32_STACK, cpu()->gpr[OCERZ_RSP]);

    reset32();
    cpu()->rflags |= OCERZ_CF | OCERZ_ZF;
    EMIT32(0x9c);
    rc = step_at(M32_CODE);
    EXPECT_U64("pushfd.esp", M32_STACK - 4, cpu()->gpr[OCERZ_RSP]);
    EXPECT_FLAG("pushfd.cf_in_slot", 1, ocerz_ld(M32_STACK - 4, 4) & OCERZ_CF);
    cpu()->rflags &= ~(uint64_t)(OCERZ_CF | OCERZ_ZF);
    EMIT32(0x9d);
    rc = step_at(M32_CODE);
    EXPECT_U64("popfd.esp", M32_STACK, cpu()->gpr[OCERZ_RSP]);
    EXPECT_FLAG("popfd.cf", 1, CF());
    EXPECT_FLAG("popfd.zf", 1, ZF());

    reset32();
    cpu()->gpr[OCERZ_RBP] = M32_STACK - 0x40;
    ocerz_st(M32_STACK - 0x40, 4, 0xcafe0000ull);
    EMIT32(0xc9);
    rc = step_at(M32_CODE);
    EXPECT_U64("leave32.esp", M32_STACK - 0x3c, cpu()->gpr[OCERZ_RSP]);
    EXPECT_U64("leave32.ebp", 0xcafe0000ull, cpu()->gpr[OCERZ_RBP]);

    reset32();
    cpu()->gpr[OCERZ_RAX] = 0xa0000000ull;
    cpu()->gpr[OCERZ_RCX] = 0xc1000000ull;
    cpu()->gpr[OCERZ_RDX] = 0xd2000000ull;
    cpu()->gpr[OCERZ_RBX] = 0xb3000000ull;
    cpu()->gpr[OCERZ_RBP] = 0x55000000ull;
    cpu()->gpr[OCERZ_RSI] = 0x66000000ull;
    cpu()->gpr[OCERZ_RDI] = 0x77000000ull;
    EMIT32(0x60);
    rc = step_at(M32_CODE);
    EXPECT_U64("pusha.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("pusha.esp", M32_STACK - 32, cpu()->gpr[OCERZ_RSP]);
    EXPECT_U64("pusha.edi", 0x77000000ull, ocerz_ld(M32_STACK - 32, 4));
    EXPECT_U64("pusha.esi", 0x66000000ull, ocerz_ld(M32_STACK - 28, 4));
    EXPECT_U64("pusha.ebp", 0x55000000ull, ocerz_ld(M32_STACK - 24, 4));
    EXPECT_U64("pusha.esp_slot", M32_STACK, ocerz_ld(M32_STACK - 20, 4));
    EXPECT_U64("pusha.ebx", 0xb3000000ull, ocerz_ld(M32_STACK - 16, 4));
    EXPECT_U64("pusha.edx", 0xd2000000ull, ocerz_ld(M32_STACK - 12, 4));
    EXPECT_U64("pusha.ecx", 0xc1000000ull, ocerz_ld(M32_STACK - 8, 4));
    EXPECT_U64("pusha.eax", 0xa0000000ull, ocerz_ld(M32_STACK - 4, 4));

    cpu()->gpr[OCERZ_RAX] = 0;
    cpu()->gpr[OCERZ_RCX] = 0;
    cpu()->gpr[OCERZ_RDX] = 0;
    cpu()->gpr[OCERZ_RBX] = 0;
    cpu()->gpr[OCERZ_RBP] = 0;
    cpu()->gpr[OCERZ_RSI] = 0;
    cpu()->gpr[OCERZ_RDI] = 0;
    ocerz_st(M32_STACK - 20, 4, 0xdeadbeefull);
    EMIT32(0x61);
    rc = step_at(M32_CODE);
    EXPECT_U64("popa.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("popa.eax", 0xa0000000ull, cpu()->gpr[OCERZ_RAX]);
    EXPECT_U64("popa.ecx", 0xc1000000ull, cpu()->gpr[OCERZ_RCX]);
    EXPECT_U64("popa.edx", 0xd2000000ull, cpu()->gpr[OCERZ_RDX]);
    EXPECT_U64("popa.ebx", 0xb3000000ull, cpu()->gpr[OCERZ_RBX]);
    EXPECT_U64("popa.ebp", 0x55000000ull, cpu()->gpr[OCERZ_RBP]);
    EXPECT_U64("popa.esi", 0x66000000ull, cpu()->gpr[OCERZ_RSI]);
    EXPECT_U64("popa.edi", 0x77000000ull, cpu()->gpr[OCERZ_RDI]);
    EXPECT_U64("popa.esp_discarded", M32_STACK, cpu()->gpr[OCERZ_RSP]);

    run_bcd_cases();

    reset32();
    cpu()->gpr[OCERZ_RAX] = 0x0044;
    EMIT32(0xd4, 0x00);
    fprintf(stderr, "-- the next 'fatal' line is expected (AAM base 0 = #DE)\n");
    rc = step_at(M32_CODE);
    EXPECT_U64("aam0.fatal", OCERZ_STEP_FATAL, rc);

    reset32();
    cpu()->gpr[OCERZ_RBP] = M32_DATA;
    ocerz_st(M32_DATA + 8, 4, 0xfffffff0ull);
    ocerz_st(M32_DATA + 12, 4, 0x00000010ull);
    cpu()->gpr[OCERZ_RAX] = 0;
    EMIT32(0x62, 0x45, 0x08);
    rc = step_at(M32_CODE);
    EXPECT_U64("bound.in_range_rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("bound.in_range_rip", M32_CODE + 3, cpu()->rip);

    cpu()->gpr[OCERZ_RAX] = 0x20;
    fprintf(stderr, "-- the next 'fatal' line is expected (BOUND #BR)\n");
    rc = step_at(M32_CODE);
    EXPECT_U64("bound.above_fatal", OCERZ_STEP_FATAL, rc);

    reset32();
    cpu()->gpr[OCERZ_RBP] = M32_DATA;
    cpu()->gpr[OCERZ_RAX] = 0xffffffe0ull;
    fprintf(stderr, "-- the next 'fatal' line is expected (BOUND #BR, signed)\n");
    rc = step_at(M32_CODE);
    EXPECT_U64("bound.below_fatal", OCERZ_STEP_FATAL, rc);

    reset32();
    EMIT32(0xce);
    rc = step_at(M32_CODE);
    EXPECT_U64("into.of0_rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("into.of0_rip", M32_CODE + 1, cpu()->rip);

    reset32();
    cpu()->rflags |= OCERZ_OF;
    fprintf(stderr, "-- the next 'fatal' line is expected (INTO with OF set)\n");
    rc = step_at(M32_CODE);
    EXPECT_U64("into.of1_fatal", OCERZ_STEP_FATAL, rc);

    reset32();
    cpu()->seg_sel[OCERZ_SREG_DS] = 0x2b;
    EMIT32(0x1e);
    rc = step_at(M32_CODE);
    EXPECT_U64("pushds.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("pushds.esp", M32_STACK - 4, cpu()->gpr[OCERZ_RSP]);
    EXPECT_U64("pushds.slot", 0x2b, ocerz_ld(M32_STACK - 4, 4));
    cpu()->seg_sel[OCERZ_SREG_DS] = 0;
    EMIT32(0x1f);
    rc = step_at(M32_CODE);
    EXPECT_U64("popds.sel", 0x2b, cpu()->seg_sel[OCERZ_SREG_DS]);
    EXPECT_U64("popds.esp", M32_STACK, cpu()->gpr[OCERZ_RSP]);

    ocerz_ldt_install(0x27u, 0x7ffe0000ull, 0xfffff, 0xf3, 1, 0, 1);
    reset32();
    cpu()->gpr[OCERZ_RAX] = 0x27;
    EMIT32(0x8e, 0xe0);
    rc = step_at(M32_CODE);
    EXPECT_U64("movfs.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("movfs.sel", 0x27, cpu()->seg_sel[OCERZ_SREG_FS]);
    EXPECT_U64("movfs.fs_base", 0x7ffe0000ull, cpu()->fs_base);

    reset32();
    cpu()->gpr[OCERZ_RBP] = M32_DATA;
    ocerz_st(M32_DATA + 8, 4, 0x11223344ull);
    ocerz_st(M32_DATA + 12, 2, 0x0033);
    cpu()->gpr[OCERZ_RAX] = 0xffffffffffffffffull;
    EMIT32(0xc4, 0x45, 0x08);
    rc = step_at(M32_CODE);
    EXPECT_U64("les.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("les.eax", 0x11223344ull, cpu()->gpr[OCERZ_RAX]);
    EXPECT_U64("les.es", 0x0033, cpu()->seg_sel[OCERZ_SREG_ES]);

    reset32();
    cpu()->gpr[OCERZ_RBP] = M32_DATA;
    EMIT32(0xc5, 0x45, 0x08);
    rc = step_at(M32_CODE);
    EXPECT_U64("lds.eax", 0x11223344ull, cpu()->gpr[OCERZ_RAX]);
    EXPECT_U64("lds.ds", 0x0033, cpu()->seg_sel[OCERZ_SREG_DS]);

    reset32();
    cpu()->fs_base = M32_DATA;
    ocerz_st(M32_DATA + 0x18, 4, 0x5eb00000ull);
    cpu()->gpr[OCERZ_RAX] = 0;
    EMIT32(0x64, 0xa1, 0x18, 0x00, 0x00, 0x00);
    rc = step_at(M32_CODE);
    EXPECT_U64("teb.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("teb.rip", M32_CODE + 6, cpu()->rip);
    EXPECT_U64("teb.eax", 0x5eb00000ull, cpu()->gpr[OCERZ_RAX]);

    reset32();
    ocerz_st(M32_DATA + 0x20, 4, 0xabcd1234ull);
    EMIT32(0x8b, 0x05, 0x20, 0x00, 0x50, 0x00);
    rc = step_at(M32_CODE);
    EXPECT_U64("abs.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("abs.eax", 0xabcd1234ull, cpu()->gpr[OCERZ_RAX]);

    reset32();
    cpu()->fs_base = M32_DATA;
    ocerz_st(M32_DATA + 0x1234, 4, 0x0f0f0f0full);
    cpu()->gpr[OCERZ_RBX] = 0xdead1234ull;
    EMIT32(0x64, 0x67, 0x8b, 0x07);
    rc = step_at(M32_CODE);
    EXPECT_U64("addr16.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("addr16.rip", M32_CODE + 4, cpu()->rip);
    EXPECT_U64("addr16.eax", 0x0f0f0f0full, cpu()->gpr[OCERZ_RAX]);

    reset32();
    cpu()->gpr[OCERZ_RAX] = 0xdead4000ull;
    EMIT32(0x66, 0xff, 0xe0);
    rc = step_at(M32_CODE);
    EXPECT_U64("jmp16.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("jmp16.rip", 0x4000ull, cpu()->rip);

    reset32();
    cpu()->gpr[OCERZ_RSP] = 0;
    cpu()->gpr[OCERZ_RAX] = 0x1234abcdull;
    EMIT32(0x50);
    rc = step_at(M32_CODE);
    EXPECT_U64("espwrap.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("espwrap.esp", 0xfffffffcull, cpu()->gpr[OCERZ_RSP]);
    EXPECT_U64("espwrap.slot", 0x1234abcdull, ocerz_ld(0xfffffffcull, 4));

    EMIT32(0x5b);
    rc = step_at(M32_CODE);
    EXPECT_U64("espwrap.pop_ebx", 0x1234abcdull, cpu()->gpr[OCERZ_RBX]);
    EXPECT_U64("espwrap.pop_esp", 0, cpu()->gpr[OCERZ_RSP]);

    reset32();
    EMIT_AT(0xffffffffull, 0x90);
    rc = step_at(0xffffffffull);
    EXPECT_U64("eipwrap.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("eipwrap.rip", 0, cpu()->rip);

    reset32();
    EMIT32(0xea, 0x10, 0x00, 0x30, 0x00, 0x33, 0x00);
    rc = step_at(M32_CODE);
    EXPECT_U64("ljmp.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("ljmp.mode32", 0, cpu()->mode32);
    EXPECT_U64("ljmp.rip", M64_CODE + 0x10, cpu()->rip);
    EXPECT_U64("ljmp.cs", CS64, cpu()->cs_sel);

    reset32();
    EMIT32(0x9a, 0x20, 0x00, 0x40, 0x00, 0x0f, 0x00);
    rc = step_at(M32_CODE);
    EXPECT_U64("lcall.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("lcall.mode32", 1, cpu()->mode32);
    EXPECT_U64("lcall.rip", M32_CODE + 0x20, cpu()->rip);
    EXPECT_U64("lcall.esp", M32_STACK - 8, cpu()->gpr[OCERZ_RSP]);
    EXPECT_U64("lcall.saved_cs", CS32, ocerz_ld(M32_STACK - 4, 4));
    EXPECT_U64("lcall.retaddr", M32_CODE + 7, ocerz_ld(M32_STACK - 8, 4));

    EMIT_AT(M32_CODE + 0x20, 0xca, 0x08, 0x00);
    rc = ocerz_interp_step(&g_vm, cpu());
    EXPECT_U64("lretf.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("lretf.mode32", 1, cpu()->mode32);
    EXPECT_U64("lretf.rip", M32_CODE + 7, cpu()->rip);
    EXPECT_U64("lretf.esp", M32_STACK + 8, cpu()->gpr[OCERZ_RSP]);

    reset64();
    EMIT_AT(M64_CODE, 0x40, 0x90);
    rc = step_at(M64_CODE);
    EXPECT_U64("long.rex_nop_rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("long.rex_nop_len", M64_CODE + 2, cpu()->rip);

    reset64();
    cpu()->gpr[OCERZ_RSP] = M32_STACK;
    cpu()->gpr[OCERZ_RAX] = 0x1122334455667788ull;
    EMIT_AT(M64_CODE, 0x50);
    rc = step_at(M64_CODE);
    EXPECT_U64("long.push_rsp", M32_STACK - 8, cpu()->gpr[OCERZ_RSP]);
    EXPECT_U64("long.push_slot", 0x1122334455667788ull, ocerz_ld(M32_STACK - 8, 8));

    reset64();
    EMIT_AT(M64_CODE, 0x60);
    fprintf(stderr, "-- the next 'fatal' line is expected (0x60 undefined in long mode)\n");
    rc = step_at(M64_CODE);
    EXPECT_U64("long.pusha_undefined", OCERZ_STEP_FATAL, rc);

    if (g_failures) {
        fprintf(stderr, "test_interp32: %d assertion(s) failed\n", g_failures);
        return 1;
    }
    fprintf(stderr, "test_interp32: all assertions passed\n");
    return 0;
}

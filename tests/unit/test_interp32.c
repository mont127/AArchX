/* Unit tests for i386 (32-bit) execution in the interpreter: stage 5.
 *
 * Where the expected values come from
 * -----------------------------------
 * Instruction identity, length and operand shape: capstone 5.0.7 in
 * CS_MODE_32, run as a black-box oracle while these rows were written.  Each
 * encoding below carries the exact capstone rendering in a comment, so a
 * reviewer can re-run it (`Cs(CS_ARCH_X86, CS_MODE_32).disasm(bytes, addr)`)
 * without re-deriving the encoding.
 *
 * Architectural effect -- register file, flags, stack traffic: the Intel SDM
 * Vol.2 entry for each instruction.  capstone is a disassembler and has no
 * opinion about what an instruction DOES, so it cannot be the oracle for the
 * BCD/ASCII adjusts.  Those were instead checked by transcribing the SDM
 * pseudocode a second time, independently, in python and diffing that model
 * against this interpreter over 102400 (AL, AH, CF, AF) and (AL, AH, base)
 * combinations: 0 mismatches.  The rows in bcd_cases[] below are a sample of
 * that sweep, kept small enough to read.
 *
 * Where the SDM says a flag is UNDEFINED, this interpreter leaves the flag
 * alone, and the sweep asserted exactly that: OF after DAA/DAS, and
 * OF/SF/ZF/PF after AAA/AAS, all still hold the value they were given.
 */
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/interp.h"
#include "ocerz/interp_common.h"
#include "ocerz/syscall.h"

#include <stdlib.h>
#include <sys/mman.h>

/* Everything the 32-bit guest touches has to be addressable in 32 bits, and so
 * does the 64-bit code it returns to -- a 32-bit RETF pops a 4-byte offset, so
 * the far-return target below 4G is not a shortcut in the test, it is how the
 * WoW64 thunk actually has to be laid out. */
#define M64_CODE  0x00300000ull
#define M32_CODE  0x00400000ull
#define M32_DATA  0x00500000ull
#define M32_STACK 0x00508000ull        /* grows down inside the M32_DATA map */
#define WRAP_PAGE 0xfffff000ull        /* the last page below 4G, plus one past */

/* CS32: LDT index 1, RPL 3, present, code, D=1 -> a 32-bit code segment.
 * CS64: a GDT selector, which is what the flat 64-bit CS is; ocerz_ldt_is_big()
 *       answers 0 for it, so it reads as 64-bit and is the mode EXIT.
 * CS64L: LDT index 2 with L=1 -- a 64-bit code segment named through the LDT,
 *       which must beat the D bit. */
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

/* Reset into 32-bit mode directly, for the many cases that are about one
 * instruction's semantics rather than about how the mode was entered.  The
 * entry and exit paths get their own tests further down. */
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
    /* The interpreter's decode-failure diagnostic dereferences [rbp+8], so a
     * test that deliberately fails a decode has to leave RBP somewhere mapped
     * or the diagnostic itself faults. */
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

/* ---------------------------------------------------------------------------
 * The BCD / ASCII adjust table.
 *
 * in_ax/in_cf/in_af go in, out_ax and the five flags come out.  Every row is a
 * verbatim line from the python-SDM cross-check sweep described at the top of
 * the file.  ZF/SF/PF start at 0 in every row (ocerz_cpu_reset gives them
 * that), so a 0 in those columns for AAA/AAS is the positive assertion that
 * the instruction left the UNDEFINED flag alone rather than an accident.
 * ------------------------------------------------------------------------- */
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
    /* DAA (0x27) -- capstone: "daa" (1) */
    { "daa 05 no adjust",       {0x27}, 1, 0x0005, 0, 0, 0x0005, 0, 0, 0, 0, 1 },
    { "daa 0a low nibble >9",   {0x27}, 1, 0x000a, 0, 0, 0x0010, 0, 1, 0, 0, 0 },
    { "daa 9a both adjusts",    {0x27}, 1, 0x009a, 0, 0, 0x0000, 1, 1, 1, 0, 1 },
    { "daa 99 with CF in",      {0x27}, 1, 0x0099, 1, 0, 0x00f9, 1, 0, 0, 1, 1 },
    { "daa 2f",                 {0x27}, 1, 0x002f, 0, 0, 0x0035, 0, 1, 0, 0, 1 },
    { "daa ff",                 {0x27}, 1, 0x00ff, 0, 0, 0x0065, 1, 1, 0, 0, 1 },
    { "daa 1a with AF in",      {0x27}, 1, 0x001a, 0, 1, 0x0020, 0, 1, 0, 0, 0 },
    { "daa fa CF+AF in",        {0x27}, 1, 0x00fa, 1, 1, 0x0060, 1, 1, 0, 0, 1 },

    /* DAS (0x2f) -- capstone: "das" (1).  Row "das 1a AF in" is the one that
     * pins the SDM asymmetry: DAS's second test has no ELSE, so the CF the
     * first adjustment did NOT raise stays 0 here, where DAA would clear it
     * explicitly. */
    { "das 05 no adjust",       {0x2f}, 1, 0x0005, 0, 0, 0x0005, 0, 0, 0, 0, 1 },
    { "das 0a low nibble >9",   {0x2f}, 1, 0x000a, 0, 0, 0x0004, 0, 1, 0, 0, 0 },
    { "das 9a both adjusts",    {0x2f}, 1, 0x009a, 0, 0, 0x0034, 1, 1, 0, 0, 0 },
    { "das 03 AF in borrows",   {0x2f}, 1, 0x0003, 0, 1, 0x00fd, 1, 1, 0, 1, 0 },
    { "das 00 CF in",           {0x2f}, 1, 0x0000, 1, 0, 0x00a0, 1, 0, 0, 1, 1 },
    { "das ff",                 {0x2f}, 1, 0x00ff, 0, 0, 0x0099, 1, 1, 0, 1, 1 },
    { "das 1a AF in",           {0x2f}, 1, 0x001a, 0, 1, 0x0014, 0, 1, 0, 0, 1 },
    { "das 05 CF+AF in",        {0x2f}, 1, 0x0005, 1, 1, 0x009f, 1, 1, 0, 1, 1 },

    /* AAA (0x37) -- capstone: "aaa" (1).  ZF/SF/PF are UNDEFINED for AAA, so
     * the 0s below assert "untouched", not "computed".  The "fa" row is the
     * one that distinguishes the SDM's literal AX := AX + 0x106 from the
     * folklore "AL += 6 (wrapping), AH += 1": AH goes 0x12 -> 0x14 because the
     * carry out of AL reaches AH a second time. */
    { "aaa 1205 no adjust",     {0x37}, 1, 0x1205, 0, 0, 0x1205, 0, 0, 0, 0, 0 },
    { "aaa 120a adjusts",       {0x37}, 1, 0x120a, 0, 0, 0x1300, 1, 1, 0, 0, 0 },
    { "aaa 12ff",               {0x37}, 1, 0x12ff, 0, 0, 0x1405, 1, 1, 0, 0, 0 },
    { "aaa 1205 AF in",         {0x37}, 1, 0x1205, 0, 1, 0x130b, 1, 1, 0, 0, 0 },
    { "aaa 12fa carry into AH", {0x37}, 1, 0x12fa, 0, 0, 0x1400, 1, 1, 0, 0, 0 },
    { "aaa ff0b AH wraps",      {0x37}, 1, 0xff0b, 0, 0, 0x0001, 1, 1, 0, 0, 0 },

    /* AAS (0x3f) -- capstone: "aas" (1).  Same UNDEFINED-flag convention. */
    { "aas 1205 no adjust",     {0x3f}, 1, 0x1205, 0, 0, 0x1205, 0, 0, 0, 0, 0 },
    { "aas 120a adjusts",       {0x3f}, 1, 0x120a, 0, 0, 0x1104, 1, 1, 0, 0, 0 },
    { "aas 12ff",               {0x3f}, 1, 0x12ff, 0, 0, 0x1109, 1, 1, 0, 0, 0 },
    { "aas 1205 AF in",         {0x3f}, 1, 0x1205, 0, 1, 0x100f, 1, 1, 0, 0, 0 },
    { "aas 000a AH wraps",      {0x3f}, 1, 0x000a, 0, 0, 0xff04, 1, 1, 0, 0, 0 },
    { "aas 1203 AF in",         {0x3f}, 1, 0x1203, 0, 1, 0x100d, 1, 1, 0, 0, 0 },

    /* SALC (0xd6) -- capstone: "salc" (1).  Undocumented; no flags change. */
    { "salc CF=0",              {0xd6}, 1, 0x00aa, 0, 0, 0x0000, 0, 0, 0, 0, 0 },
    { "salc CF=1",              {0xd6}, 1, 0x00aa, 1, 0, 0x00ff, 1, 0, 0, 0, 0 },

    /* AAM/AAD imm8 -- capstone: "aam 0xa" (2) / "aad 0xa" (2).  SF/ZF/PF do
     * follow AL for these two; CF/AF/OF are UNDEFINED and stay put. */
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
        /* OF is UNDEFINED for every instruction in this table and starts at 0. */
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

    /* The LDT the mode decision reads.  base 0 keeps the 32-bit segments flat,
     * which is what wine's WoW64 installs and what the rest of this emulator
     * assumes for CS/DS/SS. */
    ocerz_ldt_install(CS32,  0, 0xfffff, 0xfb, /*big*/1, /*is_long*/0, /*gran*/1);
    ocerz_ldt_install(CS64L, 0, 0xfffff, 0xfb, /*big*/0, /*is_long*/1, /*gran*/1);

    /* =====================================================================
     * 1. Mode ENTRY: a 64-bit far jump through a 32-bit code selector.
     * ===================================================================== */
    reset64();
    ocerz_st(M32_DATA, 4, M32_CODE);
    ocerz_st(M32_DATA + 4, 2, CS32);
    cpu()->gpr[OCERZ_RAX] = M32_DATA;
    EMIT_AT(M64_CODE, 0xff, 0x28);              /* jmp far [rax] */
    rc = step_at(M64_CODE);
    EXPECT_U64("entry.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("entry.mode32", 1, cpu()->mode32);
    EXPECT_U64("entry.rip", M32_CODE, cpu()->rip);
    EXPECT_U64("entry.cs", CS32, cpu()->cs_sel);

    /* The very next fetch must decode as i386.  0x40 is INC EAX there and a
     * REX prefix in long mode; capstone CS_MODE_32: "inc eax" (1). */
    cpu()->gpr[OCERZ_RSP] = M32_STACK;
    cpu()->gpr[OCERZ_RAX] = 0x41;
    EMIT32(0x40, 0x90);
    rc = ocerz_interp_step(&g_vm, cpu());
    EXPECT_U64("entry.inc_rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("entry.inc_eax", 0x42, cpu()->gpr[OCERZ_RAX]);
    EXPECT_U64("entry.inc_len1", M32_CODE + 1, cpu()->rip);

    /* =====================================================================
     * 2. Mode EXIT, in the same thread that just entered: a 32-bit far return
     *    to the flat 64-bit CS.  If exit were missing this is where the thread
     *    would be stranded, and the movabs below would decode as DEC EAX.
     * ===================================================================== */
    cpu()->gpr[OCERZ_RSP] = M32_STACK;
    ocerz_st(M32_STACK, 4, M64_CODE + 0x10);    /* offset: 4 bytes in i386 */
    ocerz_st(M32_STACK + 4, 4, CS64);           /* selector: one 4-byte slot */
    EMIT32(0xcb);                               /* capstone 32: "retf" (1) */
    rc = step_at(M32_CODE);
    EXPECT_U64("exit.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("exit.mode32", 0, cpu()->mode32);
    EXPECT_U64("exit.rip", M64_CODE + 0x10, cpu()->rip);
    EXPECT_U64("exit.cs", CS64, cpu()->cs_sel);
    EXPECT_U64("exit.esp", M32_STACK + 8, cpu()->gpr[OCERZ_RSP]);

    /* 48 b8 imm64 is MOVABS RAX in long mode and "dec eax; mov eax, imm32" in
     * i386 mode, so this single step proves the decoder went back to 64-bit. */
    EMIT_AT(M64_CODE + 0x10,
            0x48, 0xb8, 0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12);
    rc = ocerz_interp_step(&g_vm, cpu());
    EXPECT_U64("exit.movabs_rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("exit.movabs_rax", 0x123456789abcdef0ull, cpu()->gpr[OCERZ_RAX]);
    EXPECT_U64("exit.movabs_rip", M64_CODE + 0x1a, cpu()->rip);

    /* An LDT code segment with L=1 is 64-bit even though it sits in the LDT:
     * L beats D.  Entering 32-bit mode and then far-jumping to it must land in
     * long mode. */
    reset32();
    ocerz_st(M32_DATA + 0x40, 4, M64_CODE + 0x10);
    ocerz_st(M32_DATA + 0x44, 2, CS64L);
    cpu()->gpr[OCERZ_RAX] = M32_DATA + 0x40;
    EMIT32(0xff, 0x28);                         /* jmp far [eax] */
    rc = step_at(M32_CODE);
    EXPECT_U64("exit_ldtlong.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("exit_ldtlong.mode32", 0, cpu()->mode32);
    EXPECT_U64("exit_ldtlong.rip", M64_CODE + 0x10, cpu()->rip);

    /* =====================================================================
     * 3. 32-bit register semantics.
     * ===================================================================== */

    /* A 32-bit write zero-extends into the 64-bit slot -- the same rule as in
     * long mode, and NOT conditional on the mode.  capstone 32: "mov eax,
     * 0x12345678" (5). */
    reset32();
    cpu()->gpr[OCERZ_RAX] = 0xffffffffffffffffull;
    EMIT32(0xb8, 0x78, 0x56, 0x34, 0x12);
    rc = step_at(M32_CODE);
    EXPECT_U64("zext.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("zext.rax", 0x12345678ull, cpu()->gpr[OCERZ_RAX]);

    /* AH..BH are the high bytes of the first four registers and exist only
     * outside long mode's REX encodings.  capstone 32: "mov ah, 0xab" (2). */
    reset32();
    cpu()->gpr[OCERZ_RAX] = 0xffffffffull;
    EMIT32(0xb4, 0xab);
    rc = step_at(M32_CODE);
    EXPECT_U64("high8.write_rax", 0xffffabffull, cpu()->gpr[OCERZ_RAX]);

    /* capstone 32: "mov bl, ah" (2) -- reads AH, writes BL. */
    cpu()->gpr[OCERZ_RBX] = 0x11223344ull;
    EMIT32(0x88, 0xe3);
    rc = step_at(M32_CODE);
    EXPECT_U64("high8.read_rbx", 0x112233abull, cpu()->gpr[OCERZ_RBX]);

    /* capstone 32: "mov al, ah" (2), the other direction through 0x8a. */
    EMIT32(0x8a, 0xc4);
    rc = step_at(M32_CODE);
    EXPECT_U64("high8.al_from_ah", 0xffffabab, cpu()->gpr[OCERZ_RAX]);

    /* =====================================================================
     * 4. Stack traffic is 4 bytes wide.
     * ===================================================================== */

    /* capstone 32: "push eax" (1) then "pop ecx" (1). */
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

    /* CALL rel32 pushes a 4-byte return address; RET pops 4.  capstone 32:
     * "call 0x1005" for e8 00000000 at 0x1000, i.e. rel is from the end. */
    reset32();
    EMIT32(0xe8, 0x00, 0x00, 0x00, 0x00);
    rc = step_at(M32_CODE);
    EXPECT_U64("call32.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("call32.rip", M32_CODE + 5, cpu()->rip);
    EXPECT_U64("call32.esp", M32_STACK - 4, cpu()->gpr[OCERZ_RSP]);
    EXPECT_U64("call32.ret", M32_CODE + 5, ocerz_ld(M32_STACK - 4, 4));
    EMIT_AT(M32_CODE + 5, 0xc3);                /* capstone 32: "ret" (1) */
    rc = ocerz_interp_step(&g_vm, cpu());
    EXPECT_U64("ret32.rip", M32_CODE + 5, cpu()->rip);
    EXPECT_U64("ret32.esp", M32_STACK, cpu()->gpr[OCERZ_RSP]);

    /* PUSHFD/POPFD move 4 bytes, not 8.  capstone 32: "pushfd" / "popfd". */
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

    /* LEAVE: ESP := EBP, then EBP := pop32.  capstone 32: "leave" (1). */
    reset32();
    cpu()->gpr[OCERZ_RBP] = M32_STACK - 0x40;
    ocerz_st(M32_STACK - 0x40, 4, 0xcafe0000ull);
    EMIT32(0xc9);
    rc = step_at(M32_CODE);
    EXPECT_U64("leave32.esp", M32_STACK - 0x3c, cpu()->gpr[OCERZ_RSP]);
    EXPECT_U64("leave32.ebp", 0xcafe0000ull, cpu()->gpr[OCERZ_RBP]);

    /* =====================================================================
     * 5. PUSHA / POPA.
     * ===================================================================== */
    reset32();
    cpu()->gpr[OCERZ_RAX] = 0xa0000000ull;
    cpu()->gpr[OCERZ_RCX] = 0xc1000000ull;
    cpu()->gpr[OCERZ_RDX] = 0xd2000000ull;
    cpu()->gpr[OCERZ_RBX] = 0xb3000000ull;
    cpu()->gpr[OCERZ_RBP] = 0x55000000ull;
    cpu()->gpr[OCERZ_RSI] = 0x66000000ull;
    cpu()->gpr[OCERZ_RDI] = 0x77000000ull;
    EMIT32(0x60);                               /* capstone 32: "pushal" (1) */
    rc = step_at(M32_CODE);
    EXPECT_U64("pusha.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("pusha.esp", M32_STACK - 32, cpu()->gpr[OCERZ_RSP]);
    /* Pushed in the SDM's order EAX ECX EDX EBX ESP EBP ESI EDI, so EDI ends
     * up lowest.  The ESP slot holds the value ESP had before the first push. */
    EXPECT_U64("pusha.edi", 0x77000000ull, ocerz_ld(M32_STACK - 32, 4));
    EXPECT_U64("pusha.esi", 0x66000000ull, ocerz_ld(M32_STACK - 28, 4));
    EXPECT_U64("pusha.ebp", 0x55000000ull, ocerz_ld(M32_STACK - 24, 4));
    EXPECT_U64("pusha.esp_slot", M32_STACK, ocerz_ld(M32_STACK - 20, 4));
    EXPECT_U64("pusha.ebx", 0xb3000000ull, ocerz_ld(M32_STACK - 16, 4));
    EXPECT_U64("pusha.edx", 0xd2000000ull, ocerz_ld(M32_STACK - 12, 4));
    EXPECT_U64("pusha.ecx", 0xc1000000ull, ocerz_ld(M32_STACK - 8, 4));
    EXPECT_U64("pusha.eax", 0xa0000000ull, ocerz_ld(M32_STACK - 4, 4));

    /* POPA must restore all seven and DISCARD the saved ESP image. */
    cpu()->gpr[OCERZ_RAX] = 0;
    cpu()->gpr[OCERZ_RCX] = 0;
    cpu()->gpr[OCERZ_RDX] = 0;
    cpu()->gpr[OCERZ_RBX] = 0;
    cpu()->gpr[OCERZ_RBP] = 0;
    cpu()->gpr[OCERZ_RSI] = 0;
    cpu()->gpr[OCERZ_RDI] = 0;
    ocerz_st(M32_STACK - 20, 4, 0xdeadbeefull);   /* poison the ESP slot */
    EMIT32(0x61);                               /* capstone 32: "popal" (1) */
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

    /* =====================================================================
     * 6. The BCD / ASCII adjust table.
     * ===================================================================== */
    run_bcd_cases();

    /* AAM with base 0 is a divide error, the same #DE DIV raises. */
    reset32();
    cpu()->gpr[OCERZ_RAX] = 0x0044;
    EMIT32(0xd4, 0x00);
    fprintf(stderr, "-- the next 'fatal' line is expected (AAM base 0 = #DE)\n");
    rc = step_at(M32_CODE);
    EXPECT_U64("aam0.fatal", OCERZ_STEP_FATAL, rc);

    /* =====================================================================
     * 7. BOUND and INTO.
     * ===================================================================== */

    /* capstone 32: "bound eax, qword ptr [ebp + 8]" (3).  The memory operand
     * is the PAIR of bounds, so it is twice the operand size. */
    reset32();
    cpu()->gpr[OCERZ_RBP] = M32_DATA;
    ocerz_st(M32_DATA + 8, 4, 0xfffffff0ull);   /* lower bound: -16, signed */
    ocerz_st(M32_DATA + 12, 4, 0x00000010ull);  /* upper bound: +16 */
    cpu()->gpr[OCERZ_RAX] = 0;
    EMIT32(0x62, 0x45, 0x08);
    rc = step_at(M32_CODE);
    EXPECT_U64("bound.in_range_rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("bound.in_range_rip", M32_CODE + 3, cpu()->rip);

    /* Signed, so a large unsigned index is BELOW the lower bound only if it is
     * also negative; 0x20 is simply above the upper bound. */
    cpu()->gpr[OCERZ_RAX] = 0x20;
    fprintf(stderr, "-- the next 'fatal' line is expected (BOUND #BR)\n");
    rc = step_at(M32_CODE);
    EXPECT_U64("bound.above_fatal", OCERZ_STEP_FATAL, rc);

    /* -32 as a signed 32-bit index is below the -16 lower bound. */
    reset32();
    cpu()->gpr[OCERZ_RBP] = M32_DATA;
    cpu()->gpr[OCERZ_RAX] = 0xffffffe0ull;
    fprintf(stderr, "-- the next 'fatal' line is expected (BOUND #BR, signed)\n");
    rc = step_at(M32_CODE);
    EXPECT_U64("bound.below_fatal", OCERZ_STEP_FATAL, rc);

    /* INTO is a no-op with OF clear.  capstone 32: "into" (1). */
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

    /* =====================================================================
     * 8. Segment registers as values: PUSH/POP sreg, LES/LDS.
     * ===================================================================== */

    /* capstone 32: "push ds" (1) then "pop ds" (1). */
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

    /* Loading FS has to move fs_base, or a 32-bit guest that reloads FS loses
     * its TEB.  capstone 32: "mov fs, eax" (2).  CS32 was installed with base
     * 0, so this uses a second LDT entry that carries a real base.
     *
     * The stack forms of the same thing, PUSH/POP FS and GS, are 0f a0/a1/a8/
     * a9, which this decoder does not decode in EITHER mode; see the stage-5
     * notes.  POPSEG's fs_base/gs_base arm is therefore only reachable through
     * the one-byte ES/SS/DS opcodes today, which carry no base -- it is
     * written for the 0f forms landing later. */
    ocerz_ldt_install(0x27u, 0x7ffe0000ull, 0xfffff, 0xf3, 1, 0, 1);
    reset32();
    cpu()->gpr[OCERZ_RAX] = 0x27;
    EMIT32(0x8e, 0xe0);
    rc = step_at(M32_CODE);
    EXPECT_U64("movfs.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("movfs.sel", 0x27, cpu()->seg_sel[OCERZ_SREG_FS]);
    EXPECT_U64("movfs.fs_base", 0x7ffe0000ull, cpu()->fs_base);

    /* capstone 32: "les eax, ptr [ebp + 8]" (3).  Offset into EAX, selector
     * into ES. */
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

    /* capstone 32: "lds eax, ptr [ebp + 8]" (3). */
    reset32();
    cpu()->gpr[OCERZ_RBP] = M32_DATA;
    EMIT32(0xc5, 0x45, 0x08);
    rc = step_at(M32_CODE);
    EXPECT_U64("lds.eax", 0x11223344ull, cpu()->gpr[OCERZ_RAX]);
    EXPECT_U64("lds.ds", 0x0033, cpu()->seg_sel[OCERZ_SREG_DS]);

    /* =====================================================================
     * 9. Addressing: the TEB read, absolute moffs, 16-bit r/m, 0x66 branch.
     * ===================================================================== */

    /* The canonical 32-bit TEB read.  capstone 32: "mov eax, dword ptr
     * fs:[0x18]" (6) -- six bytes, so the moffs follows the ADDRESS size. */
    reset32();
    cpu()->fs_base = M32_DATA;
    ocerz_st(M32_DATA + 0x18, 4, 0x5eb00000ull);
    cpu()->gpr[OCERZ_RAX] = 0;
    EMIT32(0x64, 0xa1, 0x18, 0x00, 0x00, 0x00);
    rc = step_at(M32_CODE);
    EXPECT_U64("teb.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("teb.rip", M32_CODE + 6, cpu()->rip);
    EXPECT_U64("teb.eax", 0x5eb00000ull, cpu()->gpr[OCERZ_RAX]);

    /* mod=00 rm=101 is absolute in i386 mode, not RIP-relative.  capstone 32:
     * "mov eax, dword ptr [0x500020]" (6). */
    reset32();
    ocerz_st(M32_DATA + 0x20, 4, 0xabcd1234ull);
    EMIT32(0x8b, 0x05, 0x20, 0x00, 0x50, 0x00);
    rc = step_at(M32_CODE);
    EXPECT_U64("abs.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("abs.eax", 0xabcd1234ull, cpu()->gpr[OCERZ_RAX]);

    /* 0x67 in 32-bit mode selects the 16-bit r/m table, where rm=7 is [BX] and
     * there is no SIB byte.  capstone 32: "mov eax, dword ptr fs:[bx]" (4).
     *
     * The segment prefix is here for two reasons: it keeps the 16-bit address
     * inside the guest arena without pinning a page under 64K, and it pins the
     * ORDER of the two rules -- the effective address is masked to 16 bits
     * FIRST and the segment base is added after, so a poisoned high half of
     * EBX cannot reach the load and the base is not truncated with it. */
    reset32();
    cpu()->fs_base = M32_DATA;
    ocerz_st(M32_DATA + 0x1234, 4, 0x0f0f0f0full);
    cpu()->gpr[OCERZ_RBX] = 0xdead1234ull;
    EMIT32(0x64, 0x67, 0x8b, 0x07);
    rc = step_at(M32_CODE);
    EXPECT_U64("addr16.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("addr16.rip", M32_CODE + 4, cpu()->rip);
    EXPECT_U64("addr16.eax", 0x0f0f0f0full, cpu()->gpr[OCERZ_RAX]);

    /* 0x66 DOES apply to a near branch in 32-bit mode (it is ignored in long
     * mode).  capstone 32: "jmp ax" (3); the target is AX, not EAX. */
    reset32();
    cpu()->gpr[OCERZ_RAX] = 0xdead4000ull;
    EMIT32(0x66, 0xff, 0xe0);
    rc = step_at(M32_CODE);
    EXPECT_U64("jmp16.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("jmp16.rip", 0x4000ull, cpu()->rip);

    /* =====================================================================
     * 10. ESP and EIP wrap at 32 bits.
     * ===================================================================== */

    /* A push with ESP = 0 lands at 0xfffffffc.  Without the 32-bit wrap the
     * store would go to 0xfffffffffffffffc and this test would fault. */
    reset32();
    cpu()->gpr[OCERZ_RSP] = 0;
    cpu()->gpr[OCERZ_RAX] = 0x1234abcdull;
    EMIT32(0x50);
    rc = step_at(M32_CODE);
    EXPECT_U64("espwrap.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("espwrap.esp", 0xfffffffcull, cpu()->gpr[OCERZ_RSP]);
    EXPECT_U64("espwrap.slot", 0x1234abcdull, ocerz_ld(0xfffffffcull, 4));

    /* And the pop wraps back to 0 rather than reaching 0x100000000. */
    EMIT32(0x5b);                               /* capstone 32: "pop ebx" (1) */
    rc = step_at(M32_CODE);
    EXPECT_U64("espwrap.pop_ebx", 0x1234abcdull, cpu()->gpr[OCERZ_RBX]);
    EXPECT_U64("espwrap.pop_esp", 0, cpu()->gpr[OCERZ_RSP]);

    /* EIP wraps too: a one-byte instruction at 0xffffffff leaves EIP at 0. */
    reset32();
    EMIT_AT(0xffffffffull, 0x90);               /* capstone 32: "nop" (1) */
    rc = step_at(0xffffffffull);
    EXPECT_U64("eipwrap.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("eipwrap.rip", 0, cpu()->rip);

    /* =====================================================================
     * 11. The i386-only direct far transfers, 0xea and 0x9a.
     * ===================================================================== */

    /* capstone 32: "ljmp 0x33:0x300010" (7) -- encoded offset-then-selector. */
    reset32();
    EMIT32(0xea, 0x10, 0x00, 0x30, 0x00, 0x33, 0x00);
    rc = step_at(M32_CODE);
    EXPECT_U64("ljmp.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("ljmp.mode32", 0, cpu()->mode32);
    EXPECT_U64("ljmp.rip", M64_CODE + 0x10, cpu()->rip);
    EXPECT_U64("ljmp.cs", CS64, cpu()->cs_sel);

    /* capstone 32: "lcall 0xf, 0x400020" (7).  Stays in 32-bit mode (CS32) and
     * pushes CS then the return offset, one 4-byte slot each. */
    reset32();
    EMIT32(0x9a, 0x20, 0x00, 0x40, 0x00, 0x0f, 0x00);
    rc = step_at(M32_CODE);
    EXPECT_U64("lcall.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("lcall.mode32", 1, cpu()->mode32);
    EXPECT_U64("lcall.rip", M32_CODE + 0x20, cpu()->rip);
    EXPECT_U64("lcall.esp", M32_STACK - 8, cpu()->gpr[OCERZ_RSP]);
    EXPECT_U64("lcall.saved_cs", CS32, ocerz_ld(M32_STACK - 4, 4));
    EXPECT_U64("lcall.retaddr", M32_CODE + 7, ocerz_ld(M32_STACK - 8, 4));

    /* RETF imm16 unwinds both slots plus the argument bytes.  capstone 32:
     * "retf 8" (3). */
    EMIT_AT(M32_CODE + 0x20, 0xca, 0x08, 0x00);
    rc = ocerz_interp_step(&g_vm, cpu());
    EXPECT_U64("lretf.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("lretf.mode32", 1, cpu()->mode32);
    EXPECT_U64("lretf.rip", M32_CODE + 7, cpu()->rip);
    EXPECT_U64("lretf.esp", M32_STACK + 8, cpu()->gpr[OCERZ_RSP]);

    /* =====================================================================
     * 12. 64-bit tripwires: none of the above may leak into long mode.
     * ===================================================================== */

    /* 0x40 is still a REX prefix, so 40 90 is a two-byte NOP. */
    reset64();
    EMIT_AT(M64_CODE, 0x40, 0x90);
    rc = step_at(M64_CODE);
    EXPECT_U64("long.rex_nop_rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("long.rex_nop_len", M64_CODE + 2, cpu()->rip);

    /* PUSH is still 8 bytes wide in long mode. */
    reset64();
    cpu()->gpr[OCERZ_RSP] = M32_STACK;
    cpu()->gpr[OCERZ_RAX] = 0x1122334455667788ull;
    EMIT_AT(M64_CODE, 0x50);
    rc = step_at(M64_CODE);
    EXPECT_U64("long.push_rsp", M32_STACK - 8, cpu()->gpr[OCERZ_RSP]);
    EXPECT_U64("long.push_slot", 0x1122334455667788ull, ocerz_ld(M32_STACK - 8, 8));

    /* 0x60 stays undefined in long mode: PUSHA does not exist there. */
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

/* Milestone A: the wine-free proof that the WoW64 mode switch works.
 *
 * What this file is for
 * ---------------------
 * wine-11.8 picks its WoW64 CPU backend from the NATIVE machine
 * (dlls/wow64/syscall.c, get_cpu_dll_name): ARM64 selects xtajit.dll, anything
 * else selects wow64cpu.dll.  Under ocerz, wine is an x86_64 Mach-O, so wine
 * believes the native machine is AMD64 and loads its own wow64cpu -- the
 * backend written for a CPU that has hardware compatibility mode.  There is no
 * BTCpuSimulate contract for an emulator to implement; wow64cpu's BTCpuSimulate
 * is a dozen x86_64 instructions, and ocerz's entire obligation is to execute
 * them correctly.  So the thing to test is not an interface, it is four exact
 * instruction encodings and the register invariants around them.
 *
 * The four edges, and where the bytes come from
 * --------------------------------------------
 * Every encoding below was produced by clang's own assembler on this machine
 * and cross-checked against capstone 5.0.7 as a black-box disassembler oracle:
 *
 *   `clang -arch x86_64 -c` on "iretq"          -> 48 cf
 *   `clang -arch x86_64 -c` on "ljmpl *(%r14)"  -> 41 ff 2e
 *   `clang -arch i386   -c` on "ljmpl *0x1000"  -> ff 2d 00 10 00 00
 *
 *   capstone CS_MODE_64: 48 cf           -> "iretq"              (2)
 *   capstone CS_MODE_64: 41 ff 2e        -> "jmp ptr [r14]"      (3)
 *   capstone CS_MODE_64: ff 2d 00 10 00 00 -> "jmp ptr [rip + 0x1000]" (6)
 *   capstone CS_MODE_32: ff 2d 00 10 00 00 -> "jmp ptr [0x1000]"      (6)
 *
 * That last pair is the whole point of section 4.  `ff 2d disp32` is a ModRM
 * with mod=00 rm=101, which means RIP-relative in long mode and ABSOLUTE
 * disp32 in 32-bit mode -- the one encoding whose meaning inverts between the
 * two modes -- and it is exactly what wine's 32->64 thunk uses for every WoW64
 * syscall, because wine fills the thunk with an absolute address
 * (thunk->op = PtrToUlong(&thunk->addr)).  A decoder that got this backwards
 * would send every 32-bit syscall to a wild address.
 *
 * The register invariants
 * -----------------------
 * wow64cpu keeps r12 (TEB), r13 (the WOW64 context) and r14 (the 64-bit stack)
 * live across the whole 32-bit run.  32-bit code cannot name r8..r15 at all --
 * there is no REX prefix outside long mode, 0x40..0x4f are INC/DEC there -- so
 * the invariant to assert is the strong one: the ENTIRE high half of the
 * register file is untouched from mode entry to mode exit.  Section 5 runs a
 * stretch of 32-bit code that would write r8 if a single byte were decoded
 * under 64-bit rules (41 50 is "push r8" in long mode and "inc ecx; push eax"
 * in i386 mode) and then checks all eight.
 *
 * The LDT descriptor
 * ------------------
 * 0x00cffb000000ffff is the canonical flat 32-bit user code descriptor: base
 * 0, limit 0xfffff with G=1 (so 4 GB), type 0xfb = present | DPL 3 | code |
 * readable | accessed, D/B=1 (bit 54) and L=0 (bit 53).  wine installs its
 * WoW64 CS through i386_set_ldt; its LDT_SIZE is 8192 and the entry typically
 * lands at index 32, which is selector (32 << 3) | TI | RPL = 0x107.
 */
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/interp.h"
#include "ocerz/interp_common.h"
#include "ocerz/syscall.h"

#include <stdlib.h>
#include <sys/mman.h>

/* Everything is under 4 GB: the 32-bit side has to be able to name all of it,
 * and so does the far pointer the 32-bit side jumps back through. */
#define THUNK32   0x00001000ull   /* the literal address in `ff 2d 00 10 00 00` */
#define M64_CODE  0x00100000ull
#define M32_CODE  0x00200000ull
#define FARPTR64  0x00300000ull   /* r14 points here for `ljmp *(%r14)` */
#define STACK32   0x00380000ull
#define STACK64   0x00400000ull
#define ARENA_END 0x00600000ull

/* Selectors.  0x2b and 0x23 are Darwin's flat 64-bit user CS and DS. */
#define CS32_SEL  0x107u          /* LDT index 32, TI=1, RPL=3 */
#define CS64_SEL  0x2bu
#define DS64_SEL  0x23u

/* The descriptor wine installs, spelled as the SDM lays the bits out. */
#define FLAT32_CODE_DESC 0x00cffb000000ffffull

/* i386_set_ldt / i386_get_ldt: machdep syscall class 3, numbers 5 and 6.
 * rax = (class << 24) | num. */
#define SC_MACHDEP_SET_LDT 0x03000005ull
#define SC_BSD_SIGACTION   0x0200002eull   /* class 2, num 46 */
#define SC_BSD_SIGRETURN   0x020000b8ull   /* class 2, num 184 */

static int g_failures;
static OcerzVM g_vm;

static OcerzCPU *cpu(void) { return &g_vm.cpu; }

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

#define EXPECT_U64(name, e, g) check_u64(name, (uint64_t)(e), (uint64_t)(g))

static void emit_at(uint64_t addr, const uint8_t *b, size_t n)
{
    uint8_t *p = (uint8_t *)ocerz_g2h(addr);
    for (size_t i = 0; i < n; i++)
        p[i] = b[i];
}

#define EMIT_AT(a, ...) do { static const uint8_t _b[] = { __VA_ARGS__ }; \
                             emit_at((a), _b, sizeof _b); } while (0)

static int step_at(uint64_t rip)
{
    cpu()->rip = rip;
    return ocerz_interp_step(&g_vm, cpu());
}

/* The eight registers 32-bit code cannot name.  Seeded with recognisable
 * values so a stray write shows up as a value, not as a zero. */
static const uint64_t high_seed[8] = {
    0x8888888888888888ull,          /* r8  */
    0x9999999999999999ull,          /* r9  */
    0xaaaaaaaaaaaaaaaaull,          /* r10 */
    0xbbbbbbbbbbbbbbbbull,          /* r11 */
    0x00007ffe00001000ull,          /* r12: the TEB, as wow64cpu holds it */
    0x00007ffe00002000ull,          /* r13: the WOW64 context */
    STACK64,                        /* r14: the 64-bit stack */
    0xffffffffffffffffull,          /* r15 */
};

static void seed_high(void)
{
    for (int i = 0; i < 8; i++)
        cpu()->gpr[OCERZ_R8 + i] = high_seed[i];
}

static void check_high(const char *tag)
{
    static const char *const nm[8] = { "r8","r9","r10","r11","r12","r13","r14","r15" };
    for (int i = 0; i < 8; i++) {
        char b[64];
        snprintf(b, sizeof b, "%s.%s", tag, nm[i]);
        check_u64(b, high_seed[i], cpu()->gpr[OCERZ_R8 + i]);
    }
}

static void reset64(void)
{
    ocerz_cpu_reset(cpu());
    cpu()->gpr[OCERZ_RSP] = STACK64;
    cpu()->gpr[OCERZ_RBP] = STACK64;
}

/* Drive i386_set_ldt exactly as a guest would: one descriptor at a guest
 * address, an explicit index, through the syscall dispatcher. */
static uint64_t guest_set_ldt(int32_t index, uint64_t desc)
{
    ocerz_st(FARPTR64 + 0x100, 8, desc);
    reset64();
    cpu()->gpr[OCERZ_RAX] = SC_MACHDEP_SET_LDT;
    cpu()->gpr[OCERZ_RDI] = (uint64_t)(int64_t)index;
    cpu()->gpr[OCERZ_RSI] = FARPTR64 + 0x100;
    cpu()->gpr[OCERZ_RDX] = 1;
    ocerz_handle_syscall(&g_vm, cpu());
    return cpu()->gpr[OCERZ_RAX];
}

static int ldt_call_failed(void) { return (cpu()->rflags & OCERZ_CF) != 0; }

/* ---------------------------------------------------------------------------
 * Section 0.  The signal frame a process that never builds a 32-bit world
 * gets.  This runs FIRST and before any LDT descriptor exists, because
 * "installed" is a one-way latch: it is the gate that promises every process
 * working today keeps the exact mcontext it has always been handed.
 * ------------------------------------------------------------------------- */
static void install_sigsegv_handler(uint64_t handler, uint64_t tramp)
{
    uint64_t act = FARPTR64 + 0x200;
    ocerz_st(act + 0, 8, handler);
    ocerz_st(act + 8, 8, tramp);
    ocerz_st(act + 16, 4, 0);
    ocerz_st(act + 20, 4, 0);      /* no SA_ONSTACK: frame lands on the current sp */
    reset64();
    cpu()->gpr[OCERZ_RAX] = SC_BSD_SIGACTION;
    cpu()->gpr[OCERZ_RDI] = SIGSEGV;
    cpu()->gpr[OCERZ_RSI] = act;
    cpu()->gpr[OCERZ_RDX] = 0;
    ocerz_handle_syscall(&g_vm, cpu());
}

int main(void)
{
    int rc;

    /* The arena starts at 0 so that guest address 0x1000 -- the literal the
     * 32-bit thunk names -- is a real, mapped address and section 4 can
     * EXECUTE the encoding rather than only decode it. */
    if (ocerz_mem_init(0x0ull, 0x900000000ull) != OCERZ_OK) {
        fprintf(stderr, "mem_init failed\n");
        return 2;
    }
    if (ocerz_map_fixed(0x0ull, ARENA_END, PROT_READ | PROT_WRITE) != OCERZ_OK) {
        fprintf(stderr, "map_fixed failed\n");
        return 2;
    }
    ocerz_vm_init(&g_vm);
    g_vm.jit_enabled = 0;

    /* =====================================================================
     * 0. No LDT yet: the classic _STRUCT_MCONTEXT_AVX64 frame, unchanged.
     * ===================================================================== */
    EXPECT_U64("pre.ldt_installed", 0, ocerz_ldt_installed());
    install_sigsegv_handler(M64_CODE + 0x800, M64_CODE + 0x900);

    reset64();
    cpu()->gpr[OCERZ_RSP] = STACK32;
    cpu()->mxcsr = 0x1fa0;
    cpu()->xmm[0].lo = 0x0011223344556677ull;
    cpu()->xmm[0].hi = 0x8899aabbccddeeffull;
    cpu()->rip = M64_CODE + 0x30;
    EXPECT_U64("plain.delivered", 1,
               ocerz_signal_deliver(cpu(), SIGSEGV, 0xdead0000ull, 1, 4));
    {
        uint64_t uc = cpu()->gpr[OCERZ_R8];
        uint64_t mc = ocerz_ld(uc + 48, 8);
        EXPECT_U64("plain.uc_mcsize", 1032, ocerz_ld(uc + 40, 8));
        EXPECT_U64("plain.cs", 0x2b, ocerz_ld(mc + 160, 8));
        EXPECT_U64("plain.rip", M64_CODE + 0x30, ocerz_ld(mc + 144, 8));
        EXPECT_U64("plain.mxcsr_at_216", 0x1fa0, ocerz_ld(mc + 216, 4));
        EXPECT_U64("plain.xmm0_lo_at_352", 0x0011223344556677ull, ocerz_ld(mc + 352, 8));
        EXPECT_U64("plain.mode32", 0, cpu()->mode32);
        EXPECT_U64("plain.tramp", M64_CODE + 0x900, cpu()->rip);

        /* And it round-trips through the same code path sigreturn uses. */
        cpu()->mxcsr = 0;
        cpu()->xmm[0].lo = 0;
        cpu()->gpr[OCERZ_RAX] = SC_BSD_SIGRETURN;
        cpu()->gpr[OCERZ_RDI] = uc;
        ocerz_handle_syscall(&g_vm, cpu());
        EXPECT_U64("plain.ret_rip", M64_CODE + 0x30, cpu()->rip);
        EXPECT_U64("plain.ret_mxcsr", 0x1fa0, cpu()->mxcsr);
        EXPECT_U64("plain.ret_xmm0", 0x0011223344556677ull, cpu()->xmm[0].lo);
        EXPECT_U64("plain.ret_cs", 0x2b, cpu()->cs_sel);
        EXPECT_U64("plain.ret_mode32", 0, cpu()->mode32);
    }

    /* =====================================================================
     * 1. Install LDT index 32 through i386_set_ldt, the way wine does, and
     *    check the selector arithmetic and the table's real size.
     * ===================================================================== */
    EXPECT_U64("ldt.set32.ret", 32, guest_set_ldt(32, FLAT32_CODE_DESC));
    EXPECT_U64("ldt.set32.ok", 0, ldt_call_failed());
    EXPECT_U64("ldt.installed", 1, ocerz_ldt_installed());
    EXPECT_U64("ldt.sel_arith", CS32_SEL, (32u << 3) | 4u | 3u);
    EXPECT_U64("ldt.is_big", 1, ocerz_ldt_is_big(CS32_SEL));
    EXPECT_U64("ldt.is_long", 0, ocerz_ldt_is_long(CS32_SEL));
    EXPECT_U64("ldt.base", 0, ocerz_ldt_base(CS32_SEL));

    /* Index 0 is an ordinary entry.  The null-selector rule is about selector
     * VALUE 0, which has TI=0 and is rejected by the TI test; rejecting index
     * 0 as well would quietly make selector 0x07 unusable. */
    EXPECT_U64("ldt.set0.ret", 0, guest_set_ldt(0, FLAT32_CODE_DESC));
    EXPECT_U64("ldt.set0.ok", 0, ldt_call_failed());
    EXPECT_U64("ldt.idx0_is_big", 1, ocerz_ldt_is_big(0x07u));
    EXPECT_U64("ldt.sel0_rejected", 0, ocerz_ldt_is_big(0x00u));

    /* wine's LDT_SIZE is 8192, so the last legal index is 8191 -> selector
     * 0xffff.  Under the old 512-entry table this read back as "absent",
     * which would have answered "not 32-bit" and stranded the mode switch. */
    EXPECT_U64("ldt.set8191.ret", 8191, guest_set_ldt(8191, FLAT32_CODE_DESC));
    EXPECT_U64("ldt.set8191.ok", 0, ldt_call_failed());
    EXPECT_U64("ldt.idx8191_is_big", 1, ocerz_ldt_is_big(0xffffu));
    guest_set_ldt(8192, FLAT32_CODE_DESC);
    EXPECT_U64("ldt.set8192_rejected", 1, ldt_call_failed());

    /* =====================================================================
     * 2. 64 -> 32 by IRETQ, the wow64cpu entry.
     *
     *    wow64cpu pushes SS, ESP, EFlags, CS, EIP and executes `iretq`, so in
     *    memory order from RSP the frame reads EIP, CS, EFLAGS, ESP, SS.
     * ===================================================================== */
    reset64();
    seed_high();
    cpu()->gpr[OCERZ_RSP] = STACK64;
    ocerz_st(STACK64 + 0,  8, M32_CODE);        /* EIP */
    ocerz_st(STACK64 + 8,  8, CS32_SEL);        /* CS  */
    ocerz_st(STACK64 + 16, 8, 0x202);           /* EFlags */
    ocerz_st(STACK64 + 24, 8, STACK32);         /* ESP */
    ocerz_st(STACK64 + 32, 8, DS64_SEL);        /* SS  */
    EMIT_AT(M64_CODE, 0x48, 0xcf);              /* capstone 64: "iretq" (2) */
    rc = step_at(M64_CODE);
    EXPECT_U64("iretq.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("iretq.mode32", 1, cpu()->mode32);
    EXPECT_U64("iretq.cs_sel", CS32_SEL, cpu()->cs_sel);
    EXPECT_U64("iretq.seg_cs", CS32_SEL, cpu()->seg_sel[OCERZ_SREG_CS]);
    EXPECT_U64("iretq.rip", M32_CODE, cpu()->rip);
    EXPECT_U64("iretq.rsp", STACK32, cpu()->gpr[OCERZ_RSP]);
    EXPECT_U64("iretq.rflags", 0x202, cpu()->rflags);
    /* The fifth slot.  Reading only four would leave SS naming whatever the
     * 64-bit side had, which is the specific bug this pins. */
    EXPECT_U64("iretq.seg_ss", DS64_SEL, cpu()->seg_sel[OCERZ_SREG_SS]);
    check_high("iretq");

    /* The very next fetch must decode as i386: 0x40 is INC EAX there and a
     * REX prefix in long mode.  capstone CS_MODE_32: "inc eax" (1). */
    cpu()->gpr[OCERZ_RAX] = 0x41;
    EMIT_AT(M32_CODE, 0x40, 0x90);
    rc = ocerz_interp_step(&g_vm, cpu());
    EXPECT_U64("iretq.next_rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("iretq.next_eax", 0x42, cpu()->gpr[OCERZ_RAX]);
    EXPECT_U64("iretq.next_rip", M32_CODE + 1, cpu()->rip);

    /* =====================================================================
     * 3. 64 -> 32 by `ljmp *(%r14)`, wow64cpu's fast return path.
     *    41 ff 2e: REX.B with no REX.W, so m16:32 -- a 4-byte offset then a
     *    2-byte selector -- read from [r14].
     * ===================================================================== */
    reset64();
    seed_high();
    ocerz_st(FARPTR64 + 0, 4, M32_CODE + 0x40);
    ocerz_st(FARPTR64 + 4, 2, CS32_SEL);
    cpu()->gpr[OCERZ_R14] = FARPTR64;
    EMIT_AT(M64_CODE + 0x10, 0x41, 0xff, 0x2e);  /* capstone 64: "jmp ptr [r14]" (3) */
    rc = step_at(M64_CODE + 0x10);
    EXPECT_U64("ljmp.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("ljmp.mode32", 1, cpu()->mode32);
    EXPECT_U64("ljmp.cs_sel", CS32_SEL, cpu()->cs_sel);
    EXPECT_U64("ljmp.rip", M32_CODE + 0x40, cpu()->rip);
    EXPECT_U64("ljmp.r14", FARPTR64, cpu()->gpr[OCERZ_R14]);
    EXPECT_U64("ljmp.r12", high_seed[4], cpu()->gpr[OCERZ_R12]);
    EXPECT_U64("ljmp.r13", high_seed[5], cpu()->gpr[OCERZ_R13]);

    /* =====================================================================
     * 4. The encoding whose meaning inverts: ff 2d 00 10 00 00.
     *
     *    ModRM mod=00 rm=101 is RIP-relative in long mode and an ABSOLUTE
     *    disp32 in 32-bit mode.  wine's 32->64 thunk stores an absolute
     *    address there, so getting this backwards sends every WoW64 syscall
     *    to rip + <address> instead of to the thunk.
     * ===================================================================== */
    {
        static const uint8_t bytes[] = { 0xff, 0x2d, 0x00, 0x10, 0x00, 0x00 };
        X86Insn i32, i64;

        EXPECT_U64("inv.dec32.rc", OCERZ_OK,
                   (uint64_t)(int64_t)ocerz_decode_mode(bytes, sizeof bytes,
                                                        M32_CODE + 0x80, &i32, 1));
        EXPECT_U64("inv.dec32.op", OCERZ_OP_JMPF, i32.op);
        EXPECT_U64("inv.dec32.len", 6, i32.len);
        EXPECT_U64("inv.dec32.opsize", 4, i32.opsize);        /* m16:32 */
        EXPECT_U64("inv.dec32.kind", OCERZ_OPK_MEM, i32.ops[0].kind);
        EXPECT_U64("inv.dec32.riprel", 0, i32.ops[0].riprel);
        EXPECT_U64("inv.dec32.base", OCERZ_REG_NONE, i32.ops[0].base);
        EXPECT_U64("inv.dec32.index", OCERZ_REG_NONE, i32.ops[0].index);
        EXPECT_U64("inv.dec32.disp", 0x1000, (uint64_t)i32.ops[0].disp);
        /* The assertion that matters: the effective address is the literal. */
        EXPECT_U64("inv.dec32.ea", THUNK32, ocerz_ea(cpu(), &i32, &i32.ops[0]));

        /* Same six bytes, long mode: RIP-relative, and the decoder has already
         * folded rip+len into disp.  This is the other half of the inversion
         * and it also pins that 64-bit decode did not move. */
        EXPECT_U64("inv.dec64.rc", OCERZ_OK,
                   (uint64_t)(int64_t)ocerz_decode_mode(bytes, sizeof bytes,
                                                        M32_CODE + 0x80, &i64, 0));
        EXPECT_U64("inv.dec64.op", OCERZ_OP_JMPF, i64.op);
        EXPECT_U64("inv.dec64.len", 6, i64.len);
        EXPECT_U64("inv.dec64.riprel", 1, i64.ops[0].riprel);
        EXPECT_U64("inv.dec64.ea", M32_CODE + 0x80 + 6 + 0x1000,
                   ocerz_ea(cpu(), &i64, &i64.ops[0]));
        /* ocerz_decode() is the long-mode entry and must agree with it. */
        {
            X86Insn ip;
            ocerz_decode(bytes, sizeof bytes, M32_CODE + 0x80, &ip);
            EXPECT_U64("inv.plain64.riprel", 1, ip.ops[0].riprel);
            EXPECT_U64("inv.plain64.disp", (uint64_t)i64.ops[0].disp,
                       (uint64_t)ip.ops[0].disp);
        }
    }

    /* Execute it.  The CPU is still in 32-bit mode from section 3. */
    EXPECT_U64("inv.pre_mode32", 1, cpu()->mode32);
    seed_high();
    cpu()->gpr[OCERZ_RSP] = STACK32;
    ocerz_st(THUNK32 + 0, 4, M64_CODE + 0x40);   /* thunk->addr */
    ocerz_st(THUNK32 + 4, 2, CS64_SEL);          /* the flat 64-bit CS */
    EMIT_AT(M32_CODE + 0x80, 0xff, 0x2d, 0x00, 0x10, 0x00, 0x00);
    rc = step_at(M32_CODE + 0x80);
    EXPECT_U64("inv.exec.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("inv.exec.mode32", 0, cpu()->mode32);
    EXPECT_U64("inv.exec.cs_sel", CS64_SEL, cpu()->cs_sel);
    EXPECT_U64("inv.exec.rip", M64_CODE + 0x40, cpu()->rip);
    check_high("inv.exec");

    /* 48 b8 imm64 is MOVABS RAX in long mode and "dec eax; mov eax, imm32" in
     * i386 mode, so one step proves the decoder really went back to 64-bit. */
    EMIT_AT(M64_CODE + 0x40,
            0x48, 0xb8, 0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12);
    rc = ocerz_interp_step(&g_vm, cpu());
    EXPECT_U64("inv.movabs.rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("inv.movabs.rax", 0x123456789abcdef0ull, cpu()->gpr[OCERZ_RAX]);
    EXPECT_U64("inv.movabs.rip", M64_CODE + 0x4a, cpu()->rip);

    /* =====================================================================
     * 5. The whole round trip in one thread, with 32-bit code in the middle
     *    that would corrupt r8 if a byte were read under 64-bit rules.
     * ===================================================================== */
    reset64();
    seed_high();
    cpu()->gpr[OCERZ_RSP] = STACK64;
    ocerz_st(STACK64 + 0,  8, M32_CODE + 0x100);
    ocerz_st(STACK64 + 8,  8, CS32_SEL);
    ocerz_st(STACK64 + 16, 8, 0x202);
    ocerz_st(STACK64 + 24, 8, STACK32);
    ocerz_st(STACK64 + 32, 8, DS64_SEL);
    EMIT_AT(M64_CODE + 0x20, 0x48, 0xcf);
    rc = step_at(M64_CODE + 0x20);
    EXPECT_U64("trip.enter_rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("trip.enter_mode32", 1, cpu()->mode32);

    /* 41 50 -- capstone CS_MODE_64: "push r8" (2);
     *          capstone CS_MODE_32: "inc ecx" (1) then "push eax" (1).
     * Then the thunk back to 64-bit. */
    cpu()->gpr[OCERZ_RCX] = 0x10;
    cpu()->gpr[OCERZ_RAX] = 0xcafef00dull;
    EMIT_AT(M32_CODE + 0x100,
            0x41,                                  /* inc ecx */
            0x50,                                  /* push eax */
            0xff, 0x2d, 0x00, 0x10, 0x00, 0x00);   /* ljmp *0x1000 */
    rc = ocerz_interp_step(&g_vm, cpu());
    EXPECT_U64("trip.inc_rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("trip.inc_ecx", 0x11, cpu()->gpr[OCERZ_RCX]);
    EXPECT_U64("trip.inc_rip", M32_CODE + 0x101, cpu()->rip);
    rc = ocerz_interp_step(&g_vm, cpu());
    EXPECT_U64("trip.push_rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("trip.push_esp", STACK32 - 4, cpu()->gpr[OCERZ_RSP]);
    EXPECT_U64("trip.push_mem", 0xcafef00dull, ocerz_ld(STACK32 - 4, 4));
    rc = ocerz_interp_step(&g_vm, cpu());
    EXPECT_U64("trip.exit_rc", OCERZ_STEP_OK, rc);
    EXPECT_U64("trip.exit_mode32", 0, cpu()->mode32);
    EXPECT_U64("trip.exit_cs", CS64_SEL, cpu()->cs_sel);
    EXPECT_U64("trip.exit_rip", M64_CODE + 0x40, cpu()->rip);
    check_high("trip");

    /* =====================================================================
     * 6. The signal frame once a 32-bit world exists: the FULL flavour.
     *
     *    _STRUCT_MCONTEXT_AVX64_FULL carries __ds/__es/__ss/__gsbase after the
     *    thread state, so it is 1064 bytes and the whole float block slides 32
     *    bytes later: mxcsr at +248 instead of +216, xmm0 at +384 instead of
     *    +352.  Darwin uses this flavour exactly when a thread can be on a
     *    non-flat segment, which is what a WoW64 thread is.
     * ===================================================================== */
    reset64();
    seed_high();
    cpu()->gs_base = 0x00007ffe00003000ull;
    cpu()->gpr[OCERZ_RSP] = STACK64;
    ocerz_st(STACK64 + 0,  8, M32_CODE + 0x200);
    ocerz_st(STACK64 + 8,  8, CS32_SEL);
    ocerz_st(STACK64 + 16, 8, 0x202);
    ocerz_st(STACK64 + 24, 8, STACK32);
    ocerz_st(STACK64 + 32, 8, DS64_SEL);
    EMIT_AT(M64_CODE + 0x28, 0x48, 0xcf);
    rc = step_at(M64_CODE + 0x28);
    EXPECT_U64("sig.enter_mode32", 1, cpu()->mode32);

    cpu()->mxcsr = 0x1fc0;
    cpu()->xmm[3].lo = 0x1122334455667788ull;
    EXPECT_U64("sig.delivered", 1,
               ocerz_signal_deliver(cpu(), SIGSEGV, 0xbeef0000ull, 2, 6));
    {
        uint64_t uc = cpu()->gpr[OCERZ_R8];
        uint64_t mc = ocerz_ld(uc + 48, 8);
        EXPECT_U64("sig.uc_mcsize", 1064, ocerz_ld(uc + 40, 8));
        EXPECT_U64("sig.saved_cs", CS32_SEL, ocerz_ld(mc + 160, 8));
        EXPECT_U64("sig.saved_rip", M32_CODE + 0x200, ocerz_ld(mc + 144, 8));
        EXPECT_U64("sig.__ds", DS64_SEL, ocerz_ld(mc + 184, 8));
        EXPECT_U64("sig.__es", DS64_SEL, ocerz_ld(mc + 192, 8));
        EXPECT_U64("sig.__ss", DS64_SEL, ocerz_ld(mc + 200, 8));
        EXPECT_U64("sig.__gsbase", 0x00007ffe00003000ull, ocerz_ld(mc + 208, 8));
        EXPECT_U64("sig.mxcsr_at_248", 0x1fc0, ocerz_ld(mc + 248, 4));
        EXPECT_U64("sig.xmm3_at_384", 0x1122334455667788ull, ocerz_ld(mc + 384 + 48, 8));
        /* The handler is 64-bit code: delivery must leave 32-bit mode. */
        EXPECT_U64("sig.handler_mode32", 0, cpu()->mode32);
        EXPECT_U64("sig.handler_cs", CS64_SEL, cpu()->cs_sel);
        EXPECT_U64("sig.handler_rip", M64_CODE + 0x900, cpu()->rip);

        /* sigreturn reads the flavour back out of uc_mcsize and puts the
         * 32-bit world back, mode included. */
        cpu()->mxcsr = 0;
        cpu()->xmm[3].lo = 0;
        cpu()->gpr[OCERZ_RAX] = SC_BSD_SIGRETURN;
        cpu()->gpr[OCERZ_RDI] = uc;
        ocerz_handle_syscall(&g_vm, cpu());
        EXPECT_U64("sig.ret_mode32", 1, cpu()->mode32);
        EXPECT_U64("sig.ret_cs", CS32_SEL, cpu()->cs_sel);
        EXPECT_U64("sig.ret_rip", M32_CODE + 0x200, cpu()->rip);
        EXPECT_U64("sig.ret_rsp", STACK32, cpu()->gpr[OCERZ_RSP]);
        EXPECT_U64("sig.ret_mxcsr", 0x1fc0, cpu()->mxcsr);
        EXPECT_U64("sig.ret_xmm3", 0x1122334455667788ull, cpu()->xmm[3].lo);
        check_high("sig.ret");
    }

    if (g_failures) {
        fprintf(stderr, "test_wow64: %d assertion(s) failed\n", g_failures);
        return 1;
    }
    fprintf(stderr, "test_wow64: all assertions passed\n");
    return 0;
}

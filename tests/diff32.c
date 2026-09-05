/* diff32 -- the 32-bit JIT-vs-interpreter differential.
 *
 * tests/run_diff_test.sh is the 64-bit differential: it runs whole guest
 * binaries under -no-jit and under the JIT and compares the output.  There is
 * no 32-bit equivalent, because there is no i386 Mach-O to load: stage 9 (the
 * 32-bit JIT) would otherwise land with nothing able to catch a 32-bit codegen
 * bug.  This harness is that gate.
 *
 * What it does
 * ------------
 * It builds 32-bit x86 instruction sequences in guest memory, runs each one
 * TWICE from the same initial architectural state -- once with vm.jit_enabled
 * off (pure interpreter) and once with it on (the same dispatcher vm.c uses:
 * ocerz_jit_step, falling back to ocerz_interp_step on OCERZ_EUNSUP) -- and
 * then compares the FULL architectural result: all 16 GPR slots at 64 bits
 * wide, EFLAGS after ocerz_flags_materialize(), EIP, the mode and the segment
 * selectors, the FP/SSE file, and every byte of the three memory regions a
 * sequence is allowed to touch.
 *
 * Termination
 * -----------
 * Every sequence ends with the 32 -> 64 edge from the WoW64 handshake:
 *
 *     ff 2d <disp32>      jmp far [disp32]      (32-bit: ModRM 2d is ABSOLUTE)
 *
 * through an m16:32 holding {offset32 = HALT64, selector = a flat 64-bit CS}.
 * The driver loop stops the moment cpu->mode32 goes to 0, so the terminator is
 * both the stop condition and a test of the edge itself.  A mode change is a
 * dispatcher exit for any JIT -- a translator only ever covers one mode -- so
 * this stays a valid stop condition once stage 9 can chain 32-bit blocks.
 * Unreached code is filled with 0xf4 (HLT), which is fatal in the interpreter,
 * so a sequence that runs off its end is loud rather than silent.
 *
 * Why the generated sequences cannot fault
 * ----------------------------------------
 * A differential harness that crashes is worthless, so nothing here relies on
 * luck.  Rules enforced by the generator, not by hope:
 *   - EBP is pinned to the middle of a mapped scratch region and is never a
 *     destination.  In 32-bit mode the 8-bit register numbers are AL CL DL BL
 *     AH CH DH BH, so no byte write can reach EBP either -- which is precisely
 *     why EBP rather than EBX is the pin, and why AH/CH/DH/BH stay free.
 *   - ESP is never written except by a bounded add/sub, and stack traffic is
 *     depth-tracked so ESP stays inside a 128K mapped window.
 *   - Every memory operand is one of a fixed set of forms that provably lands
 *     inside the scratch region, the stack window, or the low-64K window: an
 *     absolute disp32, [EBP+disp], [ESP+disp], a base/index form preceded by
 *     an AND that masks the index, or a 16-bit-addressing form whose registers
 *     were just loaded with constants.
 *   - DIV/IDIV carry their own prologue (zero or sign-extend the high half,
 *     load a small nonzero divisor) so #DE is impossible; AAM's imm8 is never
 *     0.
 *   - Branches are self-contained: a template emits the branch AND the blob it
 *     jumps over, so no displacement can leave the sequence.  Loops are
 *     emitted with their own counter and a body that cannot write it.
 *   - PUSHA/POPA and ENTER/LEAVE only ever appear as matched pairs, because a
 *     lone POPA or LEAVE would load EBP or ESP from stack garbage.
 *
 * Stage 9 has landed: this is a real differential
 * -----------------------------------------------
 * ocerz_jit_step() no longer declines cpu->mode32, so the JIT side really does
 * compile 32-bit blocks (~51k of them on the default corpus) and the two sides
 * are two different engines.  tests/run_diff32.sh therefore passes
 * --jit-required, which turns "the JIT translated 0 blocks" back into a
 * FAILURE: a future change that silently goes back to declining 32-bit blocks
 * would otherwise turn this gate into a second interpreter run that passes
 * 100% and proves nothing.
 *
 * --selftest still runs first and still matters: it injects a deliberate
 * one-field corruption into the JIT-side result for each class of compared
 * state and requires the comparator to catch every one, so a green run cannot
 * be a comparator that compares nothing.  --bug N answers the other half --
 * it makes the JIT side deliberately wrong in one realistic way and reports
 * how much of the corpus notices.
 *
 * What this gate found when it was first pointed at a real 32-bit JIT (all
 * fixed in the same series that landed stage 9; see the commit for detail):
 *   - emit_lea() truncated an effective address at 4 GB but had no case for
 *     16-bit addressing (0x67), which is unreachable in long mode;
 *   - flags_live.c declared DIV/IDIV as defining every flag while both
 *     op_div() and emit_div() actually leave the record alone;
 *   - the i386-only opcodes are appended at the END of OcerzOp, which put them
 *     inside flags_live.c's ">= OCERZ_OP_X87_FIRST is flag-neutral" catch-all,
 *     so DAA/DAS/AAA/AAS were declared to read no flags;
 *   - emit_rot()/emit_shift_cl() write the destination unconditionally on the
 *     CL-count path, but the interpreter writes nothing when the count masks
 *     to 0;
 *   - SHLD/SHRD were declared a definite flag define rather than a may-define;
 *   - nzcv_fuse_producer() predicted a fusion during liveness that stage 9's
 *     32-bit path does not perform;
 *   - fuse_prev_mov() miscompiled `mov A,B` + `op A,A` -- in 64-bit blocks too.
 *
 * If a new 32-bit instruction becomes JIT-able, add it to the template table
 * (breadth) and, if its encoding means something different in 32-bit mode than
 * in long mode, to the hand-written cases (depth).
 */
#include "ocerz/cpu.h"
#include "ocerz/decode.h"
#include "ocerz/flags.h"
#include "ocerz/interp.h"
#include "ocerz/jit.h"
#include "ocerz/mem.h"
#include "ocerz/syscall.h"
#include "ocerz/types.h"
#include "ocerz/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* ---------------------------------------------------------------------------
 * Guest layout.  Everything is below 4G because a 32-bit guest cannot name
 * anything else, and the 64-bit code it returns to has to be nameable by the
 * 4-byte offset in the far pointer.
 * ------------------------------------------------------------------------- */
#define ARENA_LO     0x00000000ull
#define ARENA_HI     0x00000000c0000000ull

#define LOW_CODE     0x00004000ull        /* where a 16-bit EIP truncation lands */
#define LOW_CODE_LEN 0x00001000ull
#define LOW16        0x00008000ull        /* 16-bit addressing scratch */
#define LOW16_LEN    0x00001000ull

#define CODE64       0x00200000ull
#define CODE64_LEN   0x00001000ull
#define HALT64       CODE64

#define CODE32       0x00204000ull        /* low 16 bits are LOW_CODE, on purpose */
#define CODE32_LEN   0x00008000ull

#define FARPTR       0x0020c000ull        /* the m16:32 the terminator reads */
#define FARPTR_LEN   0x00004000ull

#define SCRATCH      0x00210000ull
#define SCRATCH_LEN  0x00002000ull
#define SCRATCH_MID  (SCRATCH + 0x1000)   /* EBP; low 16 bits are 0x1000 */

#define STACK_LO     0x00220000ull
#define STACK_LEN    0x00020000ull
#define ESP0         (STACK_LO + 0x10000)
#define STACK_CMP_LO (ESP0 - 0x1000)
#define STACK_CMP_LEN 0x2000ull

/* CS32: LDT index 1, RPL 3, present code segment with D=1.  CS64: a GDT
 * selector, which is what a flat 64-bit CS is -- ocerz_ldt_is_big() answers 0
 * for it, so returning through it is the mode EXIT. */
#define CS32 0x0fu
#define CS64 0x33u

typedef struct { uint64_t addr; uint64_t len; const char *name; } Region;
static const Region REGIONS[] = {
    { SCRATCH,      SCRATCH_LEN,   "scratch" },
    { STACK_CMP_LO, STACK_CMP_LEN, "stack"   },
    { LOW16,        LOW16_LEN,     "low16"   },
};
#define NREGIONS ((int)(sizeof REGIONS / sizeof REGIONS[0]))
#define SNAPLEN  ((size_t)(SCRATCH_LEN + STACK_CMP_LEN + LOW16_LEN))

/* ---------------------------------------------------------------------------
 * Deterministic RNG.  splitmix64: a seed plus a case index reproduces a
 * failing sequence exactly, which is the whole point of seeding it at all.
 * ------------------------------------------------------------------------- */
static uint64_t sm64(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/* ---------------------------------------------------------------------------
 * A case: the bytes to run, the state to run them from, and any code that has
 * to be planted away from CODE32 (the landing sites of a 16-bit EIP).
 * ------------------------------------------------------------------------- */
#define CODE_MAX 2048
#define NPLANT   8

typedef struct {
    char     name[96];
    uint8_t  code[CODE_MAX];
    size_t   len;
    uint64_t gpr[16];
    uint64_t rflags;
    uint64_t memseed;
    struct { uint64_t addr; uint8_t bytes[40]; size_t len; } plant[NPLANT];
    int      nplant;
} Case;

/* Generator state.  depth is bytes pushed below ESP0; the mapped stack gives
 * 64K of slack either way and the generator never lets it past 2K. */
typedef struct {
    Case    *c;
    uint64_t rng;
    int      depth;
} Gen;

static uint32_t rnd(Gen *g) { return (uint32_t)sm64(&g->rng); }
static uint32_t rndn(Gen *g, uint32_t n) { return rnd(g) % n; }
static int rndi(Gen *g, int lo, int hi) { return lo + (int)rndn(g, (uint32_t)(hi - lo + 1)); }

static void eb(Gen *g, unsigned b)
{
    if (g->c->len < CODE_MAX)
        g->c->code[g->c->len++] = (uint8_t)b;
}
static void ew(Gen *g, unsigned v) { eb(g, v); eb(g, v >> 8); }
static void ed(Gen *g, uint32_t v) { ew(g, v); ew(g, v >> 16); }
static size_t room(const Gen *g) { return CODE_MAX - 16 - g->c->len; }

/* Registers a generated instruction may write.  ESP (4) and EBP (5) are
 * excluded: EBP is the memory sandbox base and ESP is the stack. */
static const uint8_t DST[6] = { 0, 1, 2, 3, 6, 7 };
static uint8_t rdst(Gen *g) { return DST[rndn(g, 6)]; }
static uint8_t rany(Gen *g) { return (uint8_t)rndn(g, 8); }

/* ---------------------------------------------------------------------------
 * Memory operands.
 *
 * pick_mem() may emit guard instructions of its own -- an AND that masks an
 * index register, a MOV that loads a base -- so it has to run before any
 * prefix or opcode byte of the instruction that will use the operand.  The
 * guards are ordinary i386 instructions and are themselves under test.
 * ------------------------------------------------------------------------- */
typedef struct {
    int      a16;      /* needs a 0x67 address-size prefix */
    uint8_t  mod, rm, sib;
    int      has_sib;
    uint32_t disp;
    int      dispn;
} MF;

static void mf_abs32(MF *m, uint32_t addr)
{
    memset(m, 0, sizeof *m);
    m->mod = 0; m->rm = 5; m->disp = addr; m->dispn = 4;
}

/* [EBP + disp8] -- mod=01 rm=101.  [EBP] with mod=00 does not exist: that
 * encoding is the absolute disp32 above, which is exactly the form whose
 * meaning inverts between 32-bit and 64-bit mode. */
static void mf_ebp8(MF *m, int8_t d)
{
    memset(m, 0, sizeof *m);
    m->mod = 1; m->rm = 5; m->disp = (uint32_t)(int32_t)d; m->dispn = 1;
}

static void pick_mem(Gen *g, MF *m, int allow16)
{
    uint32_t k = rndn(g, allow16 ? 10u : 8u);
    uint8_t idx, base;
    switch (k) {
    case 0: case 1:                         /* absolute disp32 */
        mf_abs32(m, (uint32_t)(SCRATCH_MID + rndn(g, 0x400)));
        return;
    case 2:                                 /* [EBP + disp8] */
        mf_ebp8(m, (int8_t)rndi(g, -120, 120));
        return;
    case 3:                                 /* [EBP + disp32] */
        memset(m, 0, sizeof *m);
        m->mod = 2; m->rm = 5; m->dispn = 4;
        m->disp = (uint32_t)(int32_t)rndi(g, -0x400, 0x400);
        return;
    case 4:                                 /* [EBP + idx*s + disp8] */
        idx = DST[rndn(g, 6)];
        eb(g, 0x83); eb(g, 0xe0 | idx); eb(g, 0x1f);   /* and idx, 0x1f */
        memset(m, 0, sizeof *m);
        m->mod = 1; m->rm = 4; m->has_sib = 1;
        m->sib = (uint8_t)((rndn(g, 4) << 6) | (idx << 3) | 5);
        m->disp = (uint32_t)(int32_t)rndi(g, -64, 64); m->dispn = 1;
        return;
    case 5:                                 /* [ESP + disp8], the SIB base=100 form */
        memset(m, 0, sizeof *m);
        m->mod = 1; m->rm = 4; m->has_sib = 1;
        m->sib = (uint8_t)((4 << 3) | 4);   /* index=none, base=ESP */
        m->disp = (uint32_t)(int32_t)rndi(g, 0, 120); m->dispn = 1;
        return;
    case 6:                                 /* [reg] / [reg+disp8], base loaded here */
        base = DST[rndn(g, 6)];
        eb(g, 0xb8 | base); ed(g, (uint32_t)(SCRATCH_MID + rndn(g, 0x400)));
        memset(m, 0, sizeof *m);
        if (rndn(g, 2)) { m->mod = 0; m->rm = base; m->dispn = 0; }
        else { m->mod = 1; m->rm = base; m->disp = (uint32_t)(int32_t)rndi(g, -64, 64); m->dispn = 1; }
        return;
    case 7:                                 /* [disp32 + idx*s], SIB with no base */
        idx = DST[rndn(g, 6)];
        eb(g, 0x83); eb(g, 0xe0 | idx); eb(g, 0x1f);
        memset(m, 0, sizeof *m);
        m->mod = 0; m->rm = 4; m->has_sib = 1;
        m->sib = (uint8_t)((rndn(g, 4) << 6) | (idx << 3) | 5);
        m->disp = (uint32_t)SCRATCH_MID; m->dispn = 4;
        return;
    case 8:                                 /* 16-bit: [disp16] */
        memset(m, 0, sizeof *m);
        m->a16 = 1; m->mod = 0; m->rm = 6;
        m->disp = (uint32_t)(LOW16 + rndn(g, 0x100)); m->dispn = 2;
        return;
    default: {                              /* 16-bit: [BX+SI+disp8] and friends */
        static const uint8_t rm16[4] = { 0, 1, 4, 5 };  /* BX+SI, BX+DI, SI, DI */
        uint8_t r = rm16[rndn(g, 4)];
        /* Load the 16-bit halves with constants so the EA is known.  BX and SI
         * and DI are all free registers; BP is not used by these four forms. */
        eb(g, 0x66); eb(g, 0xbb); ew(g, (unsigned)(LOW16 + rndn(g, 0x80)));  /* mov bx, imm16 */
        eb(g, 0x66); eb(g, 0xbe); ew(g, rndn(g, 0x40));                      /* mov si, imm16 */
        eb(g, 0x66); eb(g, 0xbf); ew(g, rndn(g, 0x40));                      /* mov di, imm16 */
        memset(m, 0, sizeof *m);
        m->a16 = 1; m->mod = 1; m->rm = r;
        m->disp = (uint32_t)(int32_t)rndi(g, 0, 60); m->dispn = 1;
        if (r == 4 || r == 5) {  /* [SI]/[DI] alone: fold the base into the disp */
            m->mod = 2; m->dispn = 2;
            m->disp = (uint32_t)(LOW16 + rndn(g, 0x40));
        }
        return;
    }
    }
}

/* An operand an ATOMIC access may use.
 *
 * x86 lets a LOCKed access straddle any boundary; ocerz implements the atomic
 * ops with __atomic_* on the host pointer, and an unaligned ldxr/stxr on arm64
 * raises SIGBUS with si_code=ADRALN.  That is an emulator property, not
 * something this harness gets to have an opinion about, so the generator keeps
 * every atomic operand naturally aligned: SCRATCH_MID is 0x1000-aligned, so an
 * offset that is a multiple of the operand size is aligned by construction.
 * ESP-based forms are excluded because a 2-byte push can leave ESP odd.
 *
 * XADD and CMPXCHG take this path whether or not a LOCK prefix is present:
 * src/interp.c uses the atomic helpers for their memory forms unconditionally.
 */
static void pick_mem_aligned(Gen *g, MF *m, int align)
{
    uint32_t off = rndn(g, 0x80) * (uint32_t)align;
    switch (rndn(g, 3)) {
    case 0:
        mf_abs32(m, (uint32_t)(SCRATCH_MID + off));
        return;
    case 1: {
        int32_t d = (int32_t)(rndi(g, -30, 30) * align);
        memset(m, 0, sizeof *m);
        m->rm = 5;
        if (d >= -128 && d <= 127) { m->mod = 1; m->dispn = 1; }
        else { m->mod = 2; m->dispn = 4; }
        m->disp = (uint32_t)d;
        return;
    }
    default: {
        uint8_t base = DST[rndn(g, 6)];
        eb(g, 0xb8 | base); ed(g, (uint32_t)(SCRATCH_MID + off));
        memset(m, 0, sizeof *m);
        m->mod = 0; m->rm = base;
        return;
    }
    }
}

static void emit_modrm(Gen *g, unsigned reg, const MF *m)
{
    eb(g, (m->mod << 6) | ((reg & 7) << 3) | m->rm);
    if (m->has_sib)
        eb(g, m->sib);
    for (int i = 0; i < m->dispn; i++)
        eb(g, (m->disp >> (8 * i)) & 0xff);
}

/* Prefix bundle for one generated instruction.  The 0x67 comes from the
 * operand form, the 0x66 from the chosen operand size, and the segment
 * override is decoration -- ES/CS/SS/DS are all flat here, so it changes the
 * decode without changing the address, which is the point. */
static void emit_prefixes(Gen *g, int opsize16, const MF *m, int allow_seg)
{
    if (allow_seg && rndn(g, 16) == 0) {
        static const uint8_t seg[4] = { 0x2e, 0x36, 0x3e, 0x26 };
        eb(g, seg[rndn(g, 4)]);
    }
    if (opsize16)
        eb(g, 0x66);
    if (m && m->a16)
        eb(g, 0x67);
}

/* ---------------------------------------------------------------------------
 * Instruction templates.
 *
 * Each one appends one logical step -- sometimes several instructions, when
 * the step needs a guard (a masked index, a nonzero divisor, a loop counter).
 * A template never emits anything whose effect the generator cannot bound.
 * ------------------------------------------------------------------------- */

/* Register a blob body must not write.  A loop body that writes the loop
 * counter is how a bounded loop becomes a four-billion-iteration one: LOOP
 * decrements ECX and branches while it is nonzero, so a body that leaves ECX
 * at 0 turns the next decrement into 0xffffffff.  Both helpers below therefore
 * REMAP a bad draw deterministically rather than resampling -- a rejection
 * loop with a bounded retry count still falls through eventually, and "one in
 * 65536 sequences hangs" is exactly the kind of flake a gate must not have.
 * (This was not theoretical: it showed up once in a 2.4M-sequence soak as
 * `mov ecx,6 / L: ... setcc cl ... loop L`.) */
static int g_avoid = 0xff;

static uint8_t rdst_avoid(Gen *g)
{
    uint8_t r = rdst(g);
    if (r != g_avoid)
        return r;
    for (int i = 0; i < 6; i++)
        if (DST[i] != g_avoid)
            return DST[i];
    return 0;
}

/* Byte registers in i386 are AL CL DL BL AH CH DH BH, so byte register n
 * aliases the 32-bit register (n & 3). */
static uint8_t rbyte_avoid(Gen *g)
{
    uint8_t br = (uint8_t)rndn(g, 8);
    if (g_avoid != 0xff && (br & 3) == (uint8_t)g_avoid)
        br = (uint8_t)((br & 4) | (((unsigned)g_avoid + 1) & 3));
    return br;
}

/* A body that touches registers only: no memory, no stack, no branches.  Used
 * inside PUSHA/POPA, ENTER/LEAVE, loop bodies and branch-over blobs, where the
 * surrounding structure is what is under test and the body just has to be
 * something nontrivial that cannot escape. */
static void blob(Gen *g, int n)
{
    for (int i = 0; i < n && room(g) > 12; i++) {
        uint8_t d = rdst_avoid(g), s = (uint8_t)rndn(g, 8);
        switch (rndn(g, 10)) {
        case 0: eb(g, 0xb8 | d); ed(g, rnd(g)); break;                    /* mov r32, imm32 */
        case 1: eb(g, 0x01); eb(g, 0xc0 | (s << 3) | d); break;           /* add r32, r32 */
        case 2: eb(g, 0x31); eb(g, 0xc0 | (s << 3) | d); break;           /* xor r32, r32 */
        case 3: eb(g, 0x40 | d); break;                                   /* inc r32 */
        case 4: eb(g, 0x48 | d); break;                                   /* dec r32 */
        case 5: eb(g, 0xc1); eb(g, 0xe0 | d); eb(g, rndn(g, 40)); break;  /* shl r32, imm8 */
        case 6: eb(g, 0xf7); eb(g, 0xd0 | d); break;                      /* not r32 */
        case 7: eb(g, 0x0f); eb(g, 0xb6); eb(g, 0xc0 | (d << 3) | rndn(g, 8)); break; /* movzx r32, r8 */
        case 8:                                                           /* setcc r8 */
            eb(g, 0x0f); eb(g, 0x90 | rndn(g, 16)); eb(g, 0xc0 | rbyte_avoid(g));
            break;
        default: eb(g, 0x90); break;                                      /* nop */
        }
    }
}

static void t_alu(Gen *g)
{
    static const uint8_t alu[8] = { 0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38 };
    unsigned a = alu[rndn(g, 8)];
    unsigned form = rndn(g, 6);
    int sz8 = (form == 0 || form == 2 || form == 4);
    int op16 = !sz8 && rndn(g, 4) == 0;

    if (form >= 4) {                       /* AL, imm8 / eAX, imm16|32 */
        emit_prefixes(g, op16, NULL, 0);
        eb(g, a + form);
        if (sz8) eb(g, rnd(g));
        else if (op16) ew(g, rnd(g));
        else ed(g, rnd(g));
        return;
    }
    int dir = (form == 2 || form == 3);    /* reg <- r/m */
    int use_mem = rndn(g, 2);
    /* LOCK is only legal with a memory destination, and forces alignment. */
    int lock = use_mem && !dir && a != 0x38 && rndn(g, 8) == 0;
    int asz = sz8 ? 1 : (op16 ? 2 : 4);
    MF m;
    if (use_mem) { if (lock) pick_mem_aligned(g, &m, asz); else pick_mem(g, &m, 1); }
    unsigned reg = sz8 ? rany(g) : (dir ? rdst(g) : rany(g));
    if (lock)
        eb(g, 0xf0);
    emit_prefixes(g, op16, use_mem ? &m : NULL, 1);
    eb(g, a + form);
    if (use_mem) emit_modrm(g, reg, &m);
    else eb(g, 0xc0 | (reg << 3) | (sz8 ? rany(g) : (dir ? rany(g) : rdst(g))));
}

static void t_grp1(Gen *g)
{
    unsigned n = rndn(g, 8);
    unsigned kind = rndn(g, 3);            /* 0: 80 ib  1: 81 iz  2: 83 ib */
    int sz8 = (kind == 0);
    int op16 = !sz8 && rndn(g, 4) == 0;
    int use_mem = rndn(g, 2);
    int lock = use_mem && n != 7 && rndn(g, 8) == 0;         /* lock, never on CMP */
    int asz = sz8 ? 1 : (op16 ? 2 : 4);
    MF m;
    if (use_mem) { if (lock) pick_mem_aligned(g, &m, asz); else pick_mem(g, &m, 1); }
    if (lock) eb(g, 0xf0);
    emit_prefixes(g, op16, use_mem ? &m : NULL, 1);
    eb(g, kind == 0 ? 0x80 : kind == 1 ? 0x81 : 0x83);
    if (use_mem) emit_modrm(g, n, &m);
    else eb(g, 0xc0 | (n << 3) | (sz8 ? rany(g) : rdst(g)));
    if (kind == 1) { if (op16) ew(g, rnd(g)); else ed(g, rnd(g)); }
    else eb(g, rnd(g));
}

static void t_incdec(Gen *g)
{
    switch (rndn(g, 4)) {
    case 0:                                 /* 40+r / 48+r: the mode-defining byte */
        if (rndn(g, 4) == 0) eb(g, 0x66);
        eb(g, (rndn(g, 2) ? 0x40 : 0x48) | rdst(g));
        return;
    case 1: {                               /* fe /0 /1, 8-bit */
        int use_mem = rndn(g, 2);
        int lock = use_mem && rndn(g, 8) == 0;
        MF m;
        if (use_mem) { if (lock) pick_mem_aligned(g, &m, 1); else pick_mem(g, &m, 1); }
        if (lock) eb(g, 0xf0);
        emit_prefixes(g, 0, use_mem ? &m : NULL, 1);
        eb(g, 0xfe);
        if (use_mem) emit_modrm(g, rndn(g, 2), &m);
        else eb(g, 0xc0 | (rndn(g, 2) << 3) | rany(g));
        return;
    }
    default: {                              /* ff /0 /1 */
        int op16 = rndn(g, 4) == 0;
        int use_mem = rndn(g, 2);
        int lock = use_mem && rndn(g, 8) == 0;
        MF m;
        if (use_mem) { if (lock) pick_mem_aligned(g, &m, op16 ? 2 : 4); else pick_mem(g, &m, 1); }
        if (lock) eb(g, 0xf0);
        emit_prefixes(g, op16, use_mem ? &m : NULL, 1);
        eb(g, 0xff);
        if (use_mem) emit_modrm(g, rndn(g, 2), &m);
        else eb(g, 0xc0 | (rndn(g, 2) << 3) | rdst(g));
        return;
    }
    }
}

static void t_mov(Gen *g)
{
    switch (rndn(g, 8)) {
    case 0: case 1: {                       /* 88/89/8a/8b */
        unsigned form = rndn(g, 4);
        int sz8 = !(form & 1);
        int op16 = !sz8 && rndn(g, 4) == 0;
        int dir = (form >= 2);
        int use_mem = rndn(g, 2);
        MF m;
        if (use_mem) pick_mem(g, &m, 1);
        unsigned reg = sz8 ? rany(g) : (dir ? rdst(g) : rany(g));
        emit_prefixes(g, op16, use_mem ? &m : NULL, 1);
        eb(g, 0x88 + form);
        if (use_mem) emit_modrm(g, reg, &m);
        else eb(g, 0xc0 | (reg << 3) | (sz8 ? rany(g) : (dir ? rany(g) : rdst(g))));
        return;
    }
    case 2:                                 /* b0+r ib */
        eb(g, 0xb0 | rany(g)); eb(g, rnd(g));
        return;
    case 3: {                               /* b8+r iz */
        int op16 = rndn(g, 4) == 0;
        if (op16) eb(g, 0x66);
        eb(g, 0xb8 | rdst(g));
        if (op16) ew(g, rnd(g)); else ed(g, rnd(g));
        return;
    }
    case 4: case 5: {                       /* c6 /0 ib, c7 /0 iz */
        int sz8 = rndn(g, 2);
        int op16 = !sz8 && rndn(g, 4) == 0;
        int use_mem = rndn(g, 2);
        MF m;
        if (use_mem) pick_mem(g, &m, 1);
        emit_prefixes(g, op16, use_mem ? &m : NULL, 1);
        eb(g, sz8 ? 0xc6 : 0xc7);
        if (use_mem) emit_modrm(g, 0, &m);
        else eb(g, 0xc0 | (sz8 ? rany(g) : rdst(g)));
        if (sz8) eb(g, rnd(g));
        else if (op16) ew(g, rnd(g));
        else ed(g, rnd(g));
        return;
    }
    default: {                              /* a0..a3: moffs, 4 bytes wide in i386 */
        unsigned k = rndn(g, 4);
        int op16 = (k & 1) && rndn(g, 4) == 0;
        if (op16) eb(g, 0x66);
        eb(g, 0xa0 + k);
        ed(g, (uint32_t)(SCRATCH_MID + rndn(g, 0x400)));
        return;
    }
    }
}

static void t_lea(Gen *g)
{
    MF m;
    pick_mem(g, &m, 1);
    /* LEA never touches memory, so the address form can be anything the
     * decoder produces -- including the 16-bit forms, whose result is
     * truncated to 16 bits by the address size and not by the operand size. */
    emit_prefixes(g, rndn(g, 4) == 0, &m, 0);
    eb(g, 0x8d);
    emit_modrm(g, rdst(g), &m);
}

static void t_test(Gen *g)
{
    switch (rndn(g, 4)) {
    case 0: {                               /* 84/85 */
        int sz8 = rndn(g, 2);
        int op16 = !sz8 && rndn(g, 4) == 0;
        int use_mem = rndn(g, 2);
        MF m;
        if (use_mem) pick_mem(g, &m, 1);
        emit_prefixes(g, op16, use_mem ? &m : NULL, 1);
        eb(g, sz8 ? 0x84 : 0x85);
        if (use_mem) emit_modrm(g, rany(g), &m);
        else eb(g, 0xc0 | (rany(g) << 3) | rany(g));
        return;
    }
    case 1:                                 /* a8 ib / a9 iz */
        if (rndn(g, 2)) { eb(g, 0xa8); eb(g, rnd(g)); }
        else { eb(g, 0xa9); ed(g, rnd(g)); }
        return;
    default: {                              /* f6 /0 ib, f7 /0 iz */
        int sz8 = rndn(g, 2);
        int op16 = !sz8 && rndn(g, 4) == 0;
        int use_mem = rndn(g, 2);
        MF m;
        if (use_mem) pick_mem(g, &m, 1);
        emit_prefixes(g, op16, use_mem ? &m : NULL, 1);
        eb(g, sz8 ? 0xf6 : 0xf7);
        if (use_mem) emit_modrm(g, 0, &m);
        else eb(g, 0xc0 | rany(g));
        if (sz8) eb(g, rnd(g));
        else if (op16) ew(g, rnd(g));
        else ed(g, rnd(g));
        return;
    }
    }
}

static void t_xchg(Gen *g)
{
    if (rndn(g, 3) == 0) {                  /* 90+r; 0x90 itself is NOP */
        if (rndn(g, 4) == 0) eb(g, 0x66);
        eb(g, 0x90 | rdst(g));
        return;
    }
    int sz8 = rndn(g, 2);
    int op16 = !sz8 && rndn(g, 4) == 0;
    int use_mem = rndn(g, 2);
    MF m;
    /* XCHG with memory is atomic with or without a LOCK prefix, so its operand
     * must be aligned even though no 0xf0 is emitted. */
    if (use_mem) pick_mem_aligned(g, &m, sz8 ? 1 : (op16 ? 2 : 4));
    unsigned reg = sz8 ? rany(g) : rdst(g);
    emit_prefixes(g, op16, use_mem ? &m : NULL, 1);
    eb(g, sz8 ? 0x86 : 0x87);               /* memory form is implicitly LOCKed */
    if (use_mem) emit_modrm(g, reg, &m);
    else eb(g, 0xc0 | (reg << 3) | (sz8 ? rany(g) : rdst(g)));
}

static void t_shift(Gen *g)
{
    unsigned n = rndn(g, 8);                /* rol ror rcl rcr shl shr sal sar */
    unsigned kind = rndn(g, 3);             /* 0: imm8  1: by 1  2: by CL */
    int sz8 = rndn(g, 2);
    int op16 = !sz8 && rndn(g, 4) == 0;
    int use_mem = rndn(g, 2);
    MF m;
    if (use_mem) pick_mem(g, &m, 1);
    emit_prefixes(g, op16, use_mem ? &m : NULL, 1);
    eb(g, (kind == 0 ? 0xc0 : kind == 1 ? 0xd0 : 0xd2) + (sz8 ? 0 : 1));
    if (use_mem) emit_modrm(g, n, &m);
    else eb(g, 0xc0 | (n << 3) | (sz8 ? rany(g) : rdst(g)));
    if (kind == 0)
        eb(g, rndn(g, 40));                 /* counts past the width, and 0 */
}

/* MUL/IMUL/DIV/IDIV/NEG/NOT.  The divides carry their own prologue: the high
 * half is zeroed or sign-extended and the divisor is a small nonzero
 * immediate, so #DE cannot happen and the harness never has to model it. */
static void t_grp3(Gen *g)
{
    unsigned n = rndn(g, 6) + 2;            /* 2 not, 3 neg, 4 mul, 5 imul, 6 div, 7 idiv */
    if (n >= 6) {
        unsigned sz = rndn(g, 3);           /* 0: 8  1: 16  2: 32 */
        unsigned d = (unsigned)rndi(g, 1, 0x7f);
        if (sz == 0) {
            if (n == 6) { eb(g, 0xb4); eb(g, 0x00); }        /* mov ah, 0 */
            else        { eb(g, 0x66); eb(g, 0x98); }        /* cbw */
            eb(g, 0xb1); eb(g, d);                           /* mov cl, d */
            eb(g, 0xf6); eb(g, 0xc0 | (n << 3) | 1);
        } else if (sz == 1) {
            eb(g, 0x66); eb(g, n == 6 ? 0x31 : 0x99);
            if (n == 6) eb(g, 0xd2);                         /* xor dx, dx / cwd */
            eb(g, 0x66); eb(g, 0xb9); ew(g, d);              /* mov cx, d */
            eb(g, 0x66); eb(g, 0xf7); eb(g, 0xc0 | (n << 3) | 1);
        } else {
            if (n == 6) { eb(g, 0x31); eb(g, 0xd2); }        /* xor edx, edx */
            else        { eb(g, 0x99); }                     /* cdq */
            eb(g, 0xb9); ed(g, d);                           /* mov ecx, d */
            eb(g, 0xf7); eb(g, 0xc0 | (n << 3) | 1);
        }
        return;
    }
    int sz8 = rndn(g, 2);
    int op16 = !sz8 && rndn(g, 4) == 0;
    int use_mem = rndn(g, 2);
    int lock = use_mem && n <= 3 && rndn(g, 8) == 0;         /* lock not/neg */
    int asz = sz8 ? 1 : (op16 ? 2 : 4);
    MF m;
    if (use_mem) { if (lock) pick_mem_aligned(g, &m, asz); else pick_mem(g, &m, 1); }
    if (lock) eb(g, 0xf0);
    emit_prefixes(g, op16, use_mem ? &m : NULL, 1);
    eb(g, sz8 ? 0xf6 : 0xf7);
    if (use_mem) emit_modrm(g, n, &m);
    else eb(g, 0xc0 | (n << 3) | (sz8 ? rany(g) : rdst(g)));
}

static void t_imul(Gen *g)
{
    int op16 = rndn(g, 4) == 0;
    int use_mem = rndn(g, 2);
    MF m;
    if (use_mem) pick_mem(g, &m, 1);
    unsigned k = rndn(g, 3);
    emit_prefixes(g, op16, use_mem ? &m : NULL, 1);
    if (k == 0) { eb(g, 0x0f); eb(g, 0xaf); }
    else eb(g, k == 1 ? 0x69 : 0x6b);
    if (use_mem) emit_modrm(g, rdst(g), &m);
    else eb(g, 0xc0 | (rdst(g) << 3) | rany(g));
    if (k == 1) { if (op16) ew(g, rnd(g)); else ed(g, rnd(g)); }
    else if (k == 2) eb(g, rnd(g));
}

static void t_misc1(Gen *g)
{
    static const uint8_t one[13] = {
        0x98, 0x99,                          /* cwde cdq */
        0x27, 0x2f, 0x37, 0x3f, 0xd6,        /* daa das aaa aas salc */
        0xf5, 0xf8, 0xf9, 0xfc, 0xfd, 0x9e,  /* cmc clc stc cld std sahf */
    };
    switch (rndn(g, 6)) {
    case 0: eb(g, 0x66); eb(g, rndn(g, 2) ? 0x98 : 0x99); return;   /* cbw / cwd */
    case 1: eb(g, 0xd4); eb(g, (unsigned)rndi(g, 1, 255)); return;  /* aam, never imm8=0 */
    case 2: eb(g, 0xd5); eb(g, rnd(g)); return;                     /* aad */
    case 3: eb(g, 0x9f); return;                                    /* lahf */
    default: eb(g, one[rndn(g, 13)]); return;
    }
}

/* ---------------------------------------------------------------------------
 * Stack traffic.  depth is tracked so ESP never leaves the compared window,
 * which is also what keeps the comparison meaningful: a push below the window
 * would be invisible.
 * ------------------------------------------------------------------------- */
#define DEPTH_MAX 512

static void t_stack(Gen *g)
{
    int op16 = rndn(g, 4) == 0;
    int slot = op16 ? 2 : 4;
    switch (rndn(g, 8)) {
    case 0:                                  /* push r */
        if (g->depth + slot > DEPTH_MAX) return;
        if (op16) eb(g, 0x66);
        eb(g, 0x50 | rany(g));
        g->depth += slot;
        return;
    case 1:                                  /* pop r */
        if (g->depth - slot < -DEPTH_MAX) return;
        if (op16) eb(g, 0x66);
        eb(g, 0x58 | rdst(g));
        g->depth -= slot;
        return;
    case 2:                                  /* push imm8 (sign-extended to the slot) */
        if (g->depth + slot > DEPTH_MAX) return;
        if (op16) eb(g, 0x66);
        eb(g, 0x6a); eb(g, rnd(g));
        g->depth += slot;
        return;
    case 3:                                  /* push imm16/imm32 */
        if (g->depth + slot > DEPTH_MAX) return;
        if (op16) eb(g, 0x66);
        eb(g, 0x68);
        if (op16) ew(g, rnd(g)); else ed(g, rnd(g));
        g->depth += slot;
        return;
    case 4: {                                /* push m / pop m */
        int pop = rndn(g, 2);
        if (pop ? (g->depth - slot < -DEPTH_MAX) : (g->depth + slot > DEPTH_MAX))
            return;
        MF m;
        pick_mem(g, &m, 0);
        emit_prefixes(g, op16, &m, 0);
        eb(g, pop ? 0x8f : 0xff);
        emit_modrm(g, pop ? 0 : 6, &m);
        g->depth += pop ? -slot : slot;
        return;
    }
    case 5:                                  /* pushf / popf round trip */
        if (g->depth + slot > DEPTH_MAX) return;
        if (op16) eb(g, 0x66);
        eb(g, 0x9c);
        if (op16) eb(g, 0x66);
        eb(g, 0x9d);
        return;
    case 6:                                  /* popf from a controlled image */
        if (g->depth + 4 > DEPTH_MAX) return;
        eb(g, 0x68); ed(g, (rnd(g) & 0x00000cd5u) | 2u);   /* status flags + DF only */
        eb(g, 0x9d);
        return;
    default: {                               /* bounded add/sub esp */
        int n = rndi(g, 4, 64) & ~3;
        int sub = rndn(g, 2);
        if (sub ? (g->depth + n > DEPTH_MAX) : (g->depth - n < -DEPTH_MAX))
            return;
        eb(g, 0x83); eb(g, sub ? 0xec : 0xc4); eb(g, (unsigned)n);
        g->depth += sub ? n : -n;
        return;
    }
    }
}

static void t_pusha(Gen *g)
{
    int op16 = rndn(g, 4) == 0;
    int slot = op16 ? 16 : 32;
    if (g->depth + slot > DEPTH_MAX || room(g) < 80)
        return;
    if (op16) eb(g, 0x66);
    eb(g, 0x60);
    g->depth += slot;
    blob(g, rndi(g, 2, 5));
    if (op16) eb(g, 0x66);
    eb(g, 0x61);
    g->depth -= slot;
}

/* A stack frame and its LEAVE.  Note the prologue is spelled out rather than
 * written as ENTER: opcode 0xc8 is not in this decoder's map at all (grep 0xc8
 * in src/decode.c -- the only hit is the 0F C8+r BSWAP row), so an ENTER here
 * would only ever measure a decode failure.  LEAVE (0xc9) does decode, and is
 * what actually matters: it loads ESP from EBP, so it is the one instruction
 * that can move the sandbox base, and it only appears here paired with the
 * push that gave EBP its stack value. */
static void t_enter(Gen *g)
{
    unsigned frame = (unsigned)(rndi(g, 0, 64) & ~3);
    if (g->depth + 4 + (int)frame > DEPTH_MAX || room(g) < 80)
        return;
    eb(g, 0x55);                                         /* push ebp */
    eb(g, 0x89); eb(g, 0xe5);                            /* mov ebp, esp */
    if (frame) { eb(g, 0x83); eb(g, 0xec); eb(g, frame); }
    /* EBP is off the scratch sandbox for the length of the frame, so the body
     * is register-only: no memory operand can be built from EBP right now. */
    blob(g, rndi(g, 2, 5));
    eb(g, 0xc9);                                         /* leave */
}

/* ---------------------------------------------------------------------------
 * Control flow.  Every branch template emits the branch AND its target, so a
 * displacement is always computed from bytes this function just wrote.  No
 * fixup table, no way for a target to end up outside the sequence.
 * ------------------------------------------------------------------------- */
static void t_jcc(Gen *g)
{
    if (room(g) < 48) return;
    eb(g, rndn(g, 2) ? 0x39 : 0x85);                    /* cmp/test r32, r32 */
    eb(g, 0xc0 | (rany(g) << 3) | rany(g));
    unsigned cc = rndn(g, 16);
    int rel32 = rndn(g, 2);
    size_t at;
    if (rel32) { eb(g, 0x0f); eb(g, 0x80 | cc); at = g->c->len; ed(g, 0); }
    else       { eb(g, 0x70 | cc);              at = g->c->len; eb(g, 0); }
    size_t after = g->c->len;
    blob(g, rndi(g, 1, 4));
    g->c->code[at] = (uint8_t)(g->c->len - after);
}

static void t_jmp(Gen *g)
{
    if (room(g) < 48) return;
    int rel32 = rndn(g, 2);
    size_t at;
    if (rel32) { eb(g, 0xe9); at = g->c->len; ed(g, 0); }
    else       { eb(g, 0xeb); at = g->c->len; eb(g, 0); }
    size_t after = g->c->len;
    blob(g, rndi(g, 1, 4));
    g->c->code[at] = (uint8_t)(g->c->len - after);
}

static void t_loop(Gen *g)
{
    if (room(g) < 56) return;
    unsigned n = (unsigned)rndi(g, 1, 6);
    eb(g, 0xb9); ed(g, n);                              /* mov ecx, n */
    size_t top = g->c->len;
    int save = g_avoid;
    g_avoid = 1;                                        /* ECX is the counter */
    blob(g, rndi(g, 1, 3));
    g_avoid = save;
    /* LOOP picks CX or ECX by the ADDRESS size, so the 0x67 here is not
     * decoration: it changes which counter the instruction decrements. */
    if (rndn(g, 4) == 0) eb(g, 0x67);
    eb(g, 0xe0 + rndn(g, 3));                           /* loopne / loope / loop */
    int rel = (int)((int64_t)top - (int64_t)(g->c->len + 1));
    eb(g, (unsigned)(uint8_t)(int8_t)rel);
}

static void t_jecxz(Gen *g)
{
    if (room(g) < 40) return;
    if (rndn(g, 2)) { eb(g, 0x31); eb(g, 0xc9); }       /* xor ecx, ecx: taken */
    else { eb(g, 0xb9); ed(g, rnd(g) | 1u); }           /* nonzero: not taken */
    if (rndn(g, 4) == 0) eb(g, 0x67);
    eb(g, 0xe3);
    size_t at = g->c->len;
    eb(g, 0);
    size_t after = g->c->len;
    blob(g, rndi(g, 1, 3));
    g->c->code[at] = (uint8_t)(g->c->len - after);
}

static void t_call(Gen *g)
{
    if (room(g) < 80) return;
    int retn = (rndn(g, 4) == 0) ? rndi(g, 1, 4) * 4 : 0;
    if (g->depth + retn + 4 > DEPTH_MAX)
        retn = 0;
    if (g->depth + 4 > DEPTH_MAX)
        return;
    size_t jat;
    eb(g, 0xeb); jat = g->c->len; eb(g, 0);             /* jmp over the callee */
    size_t sub = g->c->len;
    blob(g, rndi(g, 1, 3));
    if (retn) { eb(g, 0xc2); ew(g, (unsigned)retn); }
    else eb(g, 0xc3);
    g->c->code[jat] = (uint8_t)(g->c->len - (jat + 1));
    for (int i = 0; i < retn / 4; i++) { eb(g, 0x6a); eb(g, rnd(g)); }
    eb(g, 0xe8);
    size_t at = g->c->len;
    ed(g, 0);
    int32_t rel = (int32_t)((int64_t)sub - (int64_t)g->c->len);
    memcpy(&g->c->code[at], &rel, 4);
}

/* String operations.  ECX, ESI and EDI are loaded here, DF is set explicitly,
 * and the count is small enough that even a backward REP stays inside the
 * scratch region -- so no string instruction can walk off a mapping. */
static void t_string(Gen *g)
{
    if (room(g) < 48) return;
    static const uint8_t ops[10] = { 0xa4, 0xa5, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xa6, 0xa7 };
    unsigned k = rndn(g, 10);
    unsigned opc = ops[k];
    int cmp_or_scas = (opc >= 0xa6 && opc <= 0xa7) || (opc >= 0xae && opc <= 0xaf);
    /* With 0x67 the pointers are SI/DI and the addresses truncate to 16 bits,
     * so the buffers have to live in the low window rather than the scratch --
     * a 32-bit scratch pointer would become a 16-bit address in unmapped
     * memory.  This is the whole reason the harness keeps a low window. */
    int a16 = rndn(g, 6) == 0;
    uint64_t src = a16 ? (LOW16 + 0x100) : (SCRATCH_MID + 0x80);
    uint64_t dst = a16 ? (LOW16 + 0x200) : (SCRATCH_MID + 0x200);
    eb(g, rndn(g, 4) == 0 ? 0xfd : 0xfc);               /* std / cld */
    eb(g, 0xb9); ed(g, (uint32_t)rndi(g, 1, 8));        /* mov ecx, n */
    eb(g, 0xbe); ed(g, (uint32_t)(src + rndn(g, 0x40)));
    eb(g, 0xbf); ed(g, (uint32_t)(dst + rndn(g, 0x40)));
    unsigned rep = rndn(g, 3);
    if (rep == 1) eb(g, 0xf3);
    else if (rep == 2) eb(g, cmp_or_scas ? 0xf2 : 0xf3);
    if ((opc & 1) && rndn(g, 3) == 0) eb(g, 0x66);      /* word rather than dword */
    if (a16) eb(g, 0x67);                               /* 16-bit counter and pointers */
    eb(g, opc);
}

/* The 0F space that i386 code actually uses. */
static void t_0f(Gen *g)
{
    int op16 = rndn(g, 5) == 0;
    switch (rndn(g, 9)) {
    case 0: {                                           /* setcc r/m8 */
        int use_mem = rndn(g, 2);
        MF m;
        if (use_mem) pick_mem(g, &m, 1);
        emit_prefixes(g, 0, use_mem ? &m : NULL, 1);
        eb(g, 0x0f); eb(g, 0x90 | rndn(g, 16));
        if (use_mem) emit_modrm(g, 0, &m);
        else eb(g, 0xc0 | rany(g));
        return;
    }
    case 1: {                                           /* cmovcc r32, r/m32 */
        int use_mem = rndn(g, 2);
        MF m;
        if (use_mem) pick_mem(g, &m, 1);
        emit_prefixes(g, op16, use_mem ? &m : NULL, 1);
        eb(g, 0x0f); eb(g, 0x40 | rndn(g, 16));
        if (use_mem) emit_modrm(g, rdst(g), &m);
        else eb(g, 0xc0 | (rdst(g) << 3) | rany(g));
        return;
    }
    case 2: {                                           /* bt/bts/btr/btc, register offsets only */
        static const uint8_t bt[4] = { 0xa3, 0xab, 0xb3, 0xbb };
        /* A memory r/m with a REGISTER bit offset addresses a bit STRING: the
         * offset is a full signed 32-bit bit index, so the address is
         * unbounded.  Register r/m masks the offset to the operand size, so
         * only that form is generated here; the memory form appears below with
         * an immediate offset, where the displacement is bounded. */
        emit_prefixes(g, op16, NULL, 0);
        eb(g, 0x0f); eb(g, bt[rndn(g, 4)]);
        eb(g, 0xc0 | (rany(g) << 3) | rdst(g));
        return;
    }
    case 3: {                                           /* 0f ba /4../7 imm8 */
        int use_mem = rndn(g, 2);
        MF m;
        if (use_mem) pick_mem(g, &m, 0);
        emit_prefixes(g, op16, use_mem ? &m : NULL, 1);
        eb(g, 0x0f); eb(g, 0xba);
        if (use_mem) emit_modrm(g, 4 + rndn(g, 4), &m);
        else eb(g, 0xc0 | ((4 + rndn(g, 4)) << 3) | rdst(g));
        eb(g, rnd(g));
        return;
    }
    case 4: {                                           /* bsf / bsr */
        int use_mem = rndn(g, 2);
        MF m;
        if (use_mem) pick_mem(g, &m, 1);
        emit_prefixes(g, op16, use_mem ? &m : NULL, 1);
        eb(g, 0x0f); eb(g, rndn(g, 2) ? 0xbc : 0xbd);
        if (use_mem) emit_modrm(g, rdst(g), &m);
        else eb(g, 0xc0 | (rdst(g) << 3) | rany(g));
        return;
    }
    case 5: {                                           /* movzx / movsx */
        static const uint8_t mx[4] = { 0xb6, 0xb7, 0xbe, 0xbf };
        unsigned k = rndn(g, 4);
        int use_mem = rndn(g, 2);
        MF m;
        if (use_mem) pick_mem(g, &m, 1);
        emit_prefixes(g, op16, use_mem ? &m : NULL, 1);
        eb(g, 0x0f); eb(g, mx[k]);
        if (use_mem) emit_modrm(g, rdst(g), &m);
        else eb(g, 0xc0 | (rdst(g) << 3) | rany(g));
        return;
    }
    case 6:                                             /* bswap */
        eb(g, 0x0f); eb(g, 0xc8 | rdst(g));
        return;
    case 7: {                                           /* shld / shrd */
        int cl = rndn(g, 2);
        int use_mem = rndn(g, 2);
        MF m;
        if (use_mem) pick_mem(g, &m, 1);
        emit_prefixes(g, op16, use_mem ? &m : NULL, 1);
        eb(g, 0x0f);
        eb(g, (rndn(g, 2) ? 0xa4 : 0xac) + (cl ? 1 : 0));
        if (use_mem) emit_modrm(g, rany(g), &m);
        else eb(g, 0xc0 | (rany(g) << 3) | rdst(g));
        if (!cl) eb(g, rndn(g, 40));
        return;
    }
    default: {                                          /* xadd / cmpxchg */
        int sz8 = rndn(g, 2);
        int xadd = rndn(g, 2);
        int use_mem = rndn(g, 2);
        MF m;
        /* Atomic in ocerz whether or not LOCK is present: aligned only. */
        if (use_mem) pick_mem_aligned(g, &m, sz8 ? 1 : (op16 ? 2 : 4));
        unsigned reg = sz8 ? rany(g) : rdst(g);
        if (use_mem && rndn(g, 4) == 0) eb(g, 0xf0);    /* lock */
        emit_prefixes(g, sz8 ? 0 : op16, use_mem ? &m : NULL, 1);
        eb(g, 0x0f); eb(g, (xadd ? 0xc0 : 0xb0) + (sz8 ? 0 : 1));
        if (use_mem) emit_modrm(g, reg, &m);
        else eb(g, 0xc0 | (reg << 3) | (sz8 ? rany(g) : rdst(g)));
        return;
    }
    }
}

/* ---------------------------------------------------------------------------
 * The weighted template table and the random-case builder.
 * ------------------------------------------------------------------------- */
typedef void (*Tmpl)(Gen *);
static const struct { Tmpl fn; int weight; } TEMPLATES[] = {
    { t_alu,    22 }, { t_grp1,  12 }, { t_incdec,  8 }, { t_mov,   18 },
    { t_lea,     6 }, { t_test,   7 }, { t_xchg,    5 }, { t_shift, 12 },
    { t_grp3,    8 }, { t_imul,   5 }, { t_misc1,   7 }, { t_stack, 14 },
    { t_pusha,   3 }, { t_enter,  2 }, { t_jcc,     8 }, { t_jmp,    3 },
    { t_loop,    4 }, { t_jecxz,  2 }, { t_call,    4 }, { t_string, 5 },
    { t_0f,     14 },
};
#define NTEMPLATES ((int)(sizeof TEMPLATES / sizeof TEMPLATES[0]))

static void gen_random(Case *c, uint64_t seed, int index)
{
    Gen g;
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "soup#%d", index);
    g.c = c;
    g.rng = seed ^ ((uint64_t)index * 0x100000001b3ull);
    g.depth = 0;
    g_avoid = 0xff;

    int total = 0;
    for (int i = 0; i < NTEMPLATES; i++)
        total += TEMPLATES[i].weight;

    int n = rndi(&g, 3, 14);
    for (int i = 0; i < n && room(&g) > 96; i++) {
        int pick = (int)rndn(&g, (uint32_t)total), k = 0;
        while (pick >= TEMPLATES[k].weight) { pick -= TEMPLATES[k].weight; k++; }
        TEMPLATES[k].fn(&g);
    }
    /* Put ESP back where it started before the terminator, so the far jump
     * always reads the same stack and a depth difference cannot hide. */
    if (g.depth > 0)      { eb(&g, 0x81); eb(&g, 0xc4); ed(&g, (uint32_t)g.depth); }
    else if (g.depth < 0) { eb(&g, 0x81); eb(&g, 0xec); ed(&g, (uint32_t)-g.depth); }

    /* Initial state.  The upper halves of the six free registers start as
     * garbage on purpose: "32-bit writes zero-extend into the 64-bit slot" is
     * a claim about ocerz, and a JIT that forgets it is caught right here.
     * ESP and EBP keep clean upper halves because wow64cpu writes them with
     * 32-bit stores, and R8..R15 hold garbage that 32-bit code must not move. */
    for (int i = 0; i < 16; i++)
        c->gpr[i] = sm64(&g.rng);
    c->gpr[OCERZ_RSP] = ESP0;
    c->gpr[OCERZ_RBP] = SCRATCH_MID;
    c->rflags = OCERZ_FLAG_FIXED1 | OCERZ_IF | (sm64(&g.rng) & 0x8d5ull);
    c->memseed = sm64(&g.rng);
}

/* ---------------------------------------------------------------------------
 * Hand-written cases.
 *
 * The generator covers breadth; these cover the specific edges where 32-bit
 * mode differs from long mode or where the encoding's MEANING changes, and
 * where a random walk would only rarely land.
 * ------------------------------------------------------------------------- */
static void plant_bytes(Case *c, uint64_t addr, const uint8_t *b, size_t n)
{
    if (c->nplant >= NPLANT || n > sizeof c->plant[0].bytes)
        return;
    int i = c->nplant++;
    c->plant[i].addr = addr;
    c->plant[i].len = n;
    memcpy(c->plant[i].bytes, b, n);
}

/* mov eax, mark ; jmp far [FARPTR] -- a landing site that records where the
 * flow went and then leaves 32-bit mode. */
static void plant_mark_term(Case *c, uint64_t addr, uint32_t mark)
{
    uint8_t b[16];
    size_t n = 0;
    uint32_t fp = (uint32_t)FARPTR;
    b[n++] = 0xb8; memcpy(b + n, &mark, 4); n += 4;
    b[n++] = 0xff; b[n++] = 0x2d; memcpy(b + n, &fp, 4); n += 4;
    plant_bytes(c, addr, b, n);
}

/* mov eax, mark ; retw -- for the 16-bit call/ret round trip. */
static void plant_mark_retw(Case *c, uint64_t addr, uint32_t mark)
{
    uint8_t b[16];
    size_t n = 0;
    b[n++] = 0xb8; memcpy(b + n, &mark, 4); n += 4;
    b[n++] = 0x66; b[n++] = 0xc3;
    plant_bytes(c, addr, b, n);
}

static void set_flags(Gen *g, uint32_t f) { eb(g, 0x68); ed(g, (f & 0xcd5u) | 2u); eb(g, 0x9d); }
static void mov32(Gen *g, unsigned r, uint32_t v) { eb(g, 0xb8 | r); ed(g, v); }

/* AH/CH/DH/BH: the four byte registers that exist only in 32-bit mode -- in
 * long mode those encodings are SPL/BPL/SIL/DIL the moment a REX is present,
 * and reg 4..7 without REX is the high-byte form the 64-bit decoder must never
 * produce for a REX-carrying instruction. */
static void h_highbyte(Gen *g)
{
    eb(g, 0xb4); eb(g, 0x5a);                    /* mov ah, 0x5a */
    eb(g, 0xb5); eb(g, 0xa5);                    /* mov ch, 0xa5 */
    eb(g, 0xb6); eb(g, 0x7f);                    /* mov dh, 0x7f */
    eb(g, 0xb7); eb(g, 0x80);                    /* mov bh, 0x80 */
    eb(g, 0x00); eb(g, 0xec);                    /* add ah, ch */
    eb(g, 0x28); eb(g, 0xf7);                    /* sub bh, dh */
    eb(g, 0x86); eb(g, 0xe3);                    /* xchg ah, bl */
    eb(g, 0xfe); eb(g, 0xc6);                    /* inc dh */
    eb(g, 0xfe); eb(g, 0xcf);                    /* dec bh */
    eb(g, 0x8a); eb(g, 0xc4);                    /* mov al, ah */
    eb(g, 0x0f); eb(g, 0xb6); eb(g, 0xc7);       /* movzx eax, bh */
    eb(g, 0x0f); eb(g, 0xbe); eb(g, 0xce);       /* movsx ecx, dh */
    eb(g, 0x84); eb(g, 0xfc);                    /* test ah, bh */
    eb(g, 0x38); eb(g, 0xf5);                    /* cmp ch, dh */
    eb(g, 0x80); eb(g, 0xc4); eb(g, 0x01);       /* add ah, 1 */
    eb(g, 0xc0); eb(g, 0xe5); eb(g, 0x03);       /* shl ch, 3 */
    eb(g, 0xf6); eb(g, 0xde);                    /* neg dh */
    eb(g, 0xf6); eb(g, 0xd7);                    /* not bh */
    eb(g, 0x0f); eb(g, 0x94); eb(g, 0xc4);       /* sete ah */
    eb(g, 0x0f); eb(g, 0x9f); eb(g, 0xc7);       /* setg bh */
}

static void h_mov_sreg(Gen *g)
{
    /* mov r/m, Sreg in 32-bit mode: the live selectors, not long-mode
     * constants, in every width and to memory */
    MF m;
    eb(g, 0x8c); eb(g, 0xc8);                    /* mov eax, cs */
    eb(g, 0x8c); eb(g, 0xd3);                    /* mov ebx, ss */
    eb(g, 0x8c); eb(g, 0xd9);                    /* mov ecx, ds */
    eb(g, 0x8c); eb(g, 0xc2);                    /* mov edx, es */
    eb(g, 0x8c); eb(g, 0xe6);                    /* mov esi, fs */
    eb(g, 0x8c); eb(g, 0xef);                    /* mov edi, gs */
    eb(g, 0x66); eb(g, 0x8c); eb(g, 0xcf);       /* mov di, cs */
    mf_abs32(&m, (uint32_t)SCRATCH_MID);
    eb(g, 0x8c); emit_modrm(g, 1, &m);           /* mov word [SCRATCH_MID], cs */
    eb(g, 0x8b); emit_modrm(g, 5, &m);           /* mov ebp, [SCRATCH_MID] */
}

static void h_highbyte_mem(Gen *g)
{
    MF m;
    mf_abs32(&m, (uint32_t)SCRATCH_MID);
    eb(g, 0x8a); emit_modrm(g, 4, &m);           /* mov ah, [SCRATCH_MID] */
    mf_abs32(&m, (uint32_t)(SCRATCH_MID + 1));
    eb(g, 0x88); emit_modrm(g, 7, &m);           /* mov [SCRATCH_MID+1], bh */
    mf_ebp8(&m, 4);
    eb(g, 0x02); emit_modrm(g, 6, &m);           /* add dh, [ebp+4] */
    mf_ebp8(&m, -8);
    eb(g, 0x30); emit_modrm(g, 5, &m);           /* xor [ebp-8], ch */
    mf_ebp8(&m, 12);
    eb(g, 0x86); emit_modrm(g, 4, &m);           /* xchg [ebp+12], ah -- locked */
    mf_ebp8(&m, 16);
    eb(g, 0x0f); eb(g, 0xb6); emit_modrm(g, 3, &m);  /* movzx ebx, byte [ebp+16] */
}

/* mod=00 rm=101.  Absolute disp32 in i386, RIP-relative in long mode: the one
 * ModRM whose MEANING inverts between the two modes. */
static void h_disp32_abs(Gen *g)
{
    MF m;
    mf_abs32(&m, (uint32_t)SCRATCH_MID);
    eb(g, 0x8b); emit_modrm(g, 0, &m);           /* mov eax, [SCRATCH_MID] */
    mf_abs32(&m, (uint32_t)(SCRATCH_MID + 4));
    eb(g, 0x01); emit_modrm(g, 1, &m);           /* add [SCRATCH_MID+4], ecx */
    mf_abs32(&m, (uint32_t)(SCRATCH_MID + 8));
    eb(g, 0xc6); emit_modrm(g, 0, &m); eb(g, 0x5a);   /* mov byte [..+8], 0x5a */
    mf_abs32(&m, (uint32_t)(SCRATCH_MID + 12));
    eb(g, 0xff); emit_modrm(g, 0, &m);           /* inc dword [..+12] */
    mf_abs32(&m, (uint32_t)(SCRATCH_MID + 16));
    eb(g, 0x66); eb(g, 0x89); emit_modrm(g, 3, &m);   /* mov [..+16], bx */
    mf_abs32(&m, (uint32_t)(SCRATCH_MID + 20));
    eb(g, 0xf0); eb(g, 0x01); emit_modrm(g, 6, &m);   /* lock add [..+20], esi */
    /* SIB with no base: mod=00 rm=100, base field 101 -> disp32 + index*scale.
     * Also absolute, also only in this shape. */
    eb(g, 0x83); eb(g, 0xe7); eb(g, 0x1f);       /* and edi, 0x1f */
    eb(g, 0x8b); eb(g, 0x04); eb(g, 0xbd); ed(g, (uint32_t)SCRATCH_MID);  /* mov eax, [SCRATCH_MID+edi*4] */
    eb(g, 0x8d); eb(g, 0x14); eb(g, 0x7d); ed(g, (uint32_t)SCRATCH_MID);  /* lea edx, [SCRATCH_MID+edi*2] */
}

/* moffs: A0..A3 carry an offset whose width follows the ADDRESS size, so four
 * bytes here and eight in long mode. */
static void h_moffs(Gen *g)
{
    eb(g, 0xa1); ed(g, (uint32_t)SCRATCH_MID);            /* mov eax, [moffs32] */
    eb(g, 0xa3); ed(g, (uint32_t)(SCRATCH_MID + 4));      /* mov [moffs32], eax */
    eb(g, 0xa0); ed(g, (uint32_t)(SCRATCH_MID + 8));      /* mov al, [moffs32] */
    eb(g, 0xa2); ed(g, (uint32_t)(SCRATCH_MID + 9));      /* mov [moffs32], al */
    eb(g, 0x66); eb(g, 0xa1); ed(g, (uint32_t)(SCRATCH_MID + 12));
    eb(g, 0x66); eb(g, 0xa3); ed(g, (uint32_t)(SCRATCH_MID + 14));
}

/* 4-byte stack slots.  In long mode PUSH takes 8; here every slot is 4, and
 * the 0x66 forms make it 2 -- which leaves ESP odd-aligned on purpose. */
static void h_stack4(Gen *g)
{
    for (int i = 0; i < 6; i++) eb(g, 0x50 | DST[i]);     /* push */
    eb(g, 0x54);                                          /* push esp */
    eb(g, 0x55);                                          /* push ebp */
    eb(g, 0x58 | 0); eb(g, 0x58 | 1);                     /* pop eax, pop ecx */
    for (int i = 5; i >= 0; i--) eb(g, 0x58 | DST[i]);    /* pop the rest */
    eb(g, 0x6a); eb(g, 0xff);                             /* push -1 (sign-extended to 4) */
    eb(g, 0x68); ed(g, 0xdeadbeefu);                      /* push imm32 */
    eb(g, 0x8f); eb(g, 0xc0 | 0);                         /* pop eax */
    eb(g, 0x58 | 3);                                      /* pop ebx */
    {
        MF m;
        mf_ebp8(&m, 32);
        eb(g, 0xff); emit_modrm(g, 6, &m);                /* push dword [ebp+32] */
        mf_ebp8(&m, 36);
        eb(g, 0x8f); emit_modrm(g, 0, &m);                /* pop dword [ebp+36] */
    }
}

static void h_stack2(Gen *g)
{
    eb(g, 0x66); eb(g, 0x50);                             /* push ax  (2 bytes) */
    eb(g, 0x66); eb(g, 0x51);
    eb(g, 0x53);                                          /* push ebx (4 bytes) */
    eb(g, 0x66); eb(g, 0x68); ew(g, 0x1234);              /* push imm16 */
    eb(g, 0x66); eb(g, 0x6a); eb(g, 0xf0);                /* push imm8 -> 2-byte slot */
    eb(g, 0x66); eb(g, 0x5e);                             /* pop si */
    eb(g, 0x66); eb(g, 0x5f);                             /* pop di */
    eb(g, 0x5b);                                          /* pop ebx */
    eb(g, 0x66); eb(g, 0x59);
    eb(g, 0x66); eb(g, 0x58);
}

static void h_pushf_popf(Gen *g)
{
    set_flags(g, 0x8d5);
    eb(g, 0x9c);                                          /* pushfd */
    eb(g, 0x9d);                                          /* popfd */
    eb(g, 0x66); eb(g, 0x9c);                             /* pushfw */
    eb(g, 0x66); eb(g, 0x9d);                             /* popfw */
    set_flags(g, 0x001);
    eb(g, 0x9f);                                          /* lahf */
    eb(g, 0x9e);                                          /* sahf */
    set_flags(g, 0x400);                                  /* DF set through POPF */
    eb(g, 0xfc);
    set_flags(g, 0x0c5);
}

static void h_pusha_popa(Gen *g)
{
    eb(g, 0x60);                                          /* pushad */
    for (int i = 0; i < 6; i++) mov32(g, DST[i], 0xa5a5a500u | (unsigned)i);
    eb(g, 0x83); eb(g, 0xec); eb(g, 0x10);                /* sub esp, 16 */
    eb(g, 0x83); eb(g, 0xc4); eb(g, 0x10);                /* add esp, 16 */
    eb(g, 0x61);                                          /* popad: EBP comes back, ESP image discarded */
    eb(g, 0x66); eb(g, 0x60);                             /* pushaw: 16 bytes */
    for (int i = 0; i < 6; i++) mov32(g, DST[i], 0x5a5a0000u | (unsigned)i);
    eb(g, 0x66); eb(g, 0x61);                             /* popaw */
}

/* The BCD/ASCII adjusts and their flag effects.  Each row sets AX and the
 * incoming CF/AF explicitly, because those are the inputs the adjust reads. */
static void h_bcd(Gen *g)
{
    static const struct { uint16_t ax; uint32_t fl; uint8_t op; } rows[] = {
        { 0x0005, 0x000, 0x27 }, { 0x000a, 0x000, 0x27 }, { 0x009a, 0x000, 0x27 },
        { 0x0099, 0x001, 0x27 }, { 0x00ff, 0x000, 0x27 }, { 0x00fa, 0x011, 0x27 },
        { 0x0005, 0x000, 0x2f }, { 0x000a, 0x000, 0x2f }, { 0x009a, 0x000, 0x2f },
        { 0x0003, 0x010, 0x2f }, { 0x0000, 0x001, 0x2f }, { 0x0005, 0x011, 0x2f },
        { 0x1205, 0x000, 0x37 }, { 0x120a, 0x000, 0x37 }, { 0x12ff, 0x000, 0x37 },
        { 0x12fa, 0x000, 0x37 }, { 0xff0b, 0x000, 0x37 }, { 0x1205, 0x010, 0x37 },
        { 0x1205, 0x000, 0x3f }, { 0x120a, 0x000, 0x3f }, { 0x12ff, 0x000, 0x3f },
        { 0x000a, 0x000, 0x3f }, { 0x1203, 0x010, 0x3f }, { 0x1205, 0x010, 0x3f },
        { 0x00aa, 0x000, 0xd6 }, { 0x00aa, 0x001, 0xd6 },
    };
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        eb(g, 0x66); eb(g, 0xb8); ew(g, rows[i].ax);      /* mov ax, imm16 */
        set_flags(g, rows[i].fl);
        eb(g, rows[i].op);
        /* park the result so a later row cannot hide an earlier one */
        eb(g, 0x66); eb(g, 0xa3); ed(g, (uint32_t)(SCRATCH_MID + 0x100 + 4 * i));
        eb(g, 0x9c); eb(g, 0x59);                         /* pushfd; pop ecx */
        eb(g, 0x89); eb(g, 0x0d); ed(g, (uint32_t)(SCRATCH_MID + 0x200 + 4 * i));
    }
}

static void h_aam_aad(Gen *g)
{
    static const uint8_t bases[6] = { 0x0a, 0x01, 0x02, 0x10, 0x7f, 0xff };
    for (int i = 0; i < 6; i++) {
        eb(g, 0x66); eb(g, 0xb8); ew(g, (unsigned)(0x1234 + 0x1111 * i));
        eb(g, 0xd4); eb(g, bases[i]);                     /* aam base */
        eb(g, 0x66); eb(g, 0xa3); ed(g, (uint32_t)(SCRATCH_MID + 0x300 + 4 * i));
        eb(g, 0x66); eb(g, 0xb8); ew(g, (unsigned)(0x0509 + 0x0101 * i));
        eb(g, 0xd5); eb(g, bases[i]);                     /* aad base */
        eb(g, 0x66); eb(g, 0xa3); ed(g, (uint32_t)(SCRATCH_MID + 0x340 + 4 * i));
    }
}

/* Near branches WITH the 0x66 prefix.  A 16-bit operand size makes a near jump
 * write only the low 16 bits of EIP, so the flow either truncates or it does
 * not -- and both landing sites are planted with a distinct marker and a
 * terminator, so the case completes either way and EAX records which happened.
 * That is the only way to diff a behaviour whose two outcomes are different
 * addresses. */
static void h_jmp16(Gen *g)
{
    eb(g, 0x66); eb(g, 0xe9); ew(g, 0x0400);             /* jmp rel16; 4 bytes, next = +4 */
    plant_mark_term(g->c, 0x4404, 0x11111111);           /* EIP truncated to 16 bits */
    plant_mark_term(g->c, CODE32 + 0x404, 0x22222222);   /* EIP kept at 32 bits */
}

static void h_jcc16(Gen *g)
{
    eb(g, 0x31); eb(g, 0xc0);                            /* xor eax, eax -> ZF=1 */
    eb(g, 0x66); eb(g, 0x0f); eb(g, 0x84); ew(g, 0x0400);/* je rel16, taken */
    plant_mark_term(g->c, 0x4407, 0x11111111);
    plant_mark_term(g->c, CODE32 + 0x407, 0x22222222);
}

static void h_call16(Gen *g)
{
    eb(g, 0x66); eb(g, 0xe8); ew(g, 0x0100);             /* call rel16: pushes 2 bytes */
    eb(g, 0xb8); ed(g, 0x44444444u);                     /* the 4-byte-return-address path */
    plant_mark_retw(g->c, 0x4104, 0x11111111);
    plant_mark_retw(g->c, CODE32 + 0x104, 0x22222222);
    plant_mark_term(g->c, 0x4004, 0x33333333);           /* where a 2-byte RET lands */
}

static void h_near32(Gen *g)
{
    /* The same shapes without 0x66, where nothing truncates: rel8 and rel32,
     * forward and backward, taken and not. */
    eb(g, 0x31); eb(g, 0xc0);                            /* xor eax, eax */
    eb(g, 0xeb); eb(g, 0x02); eb(g, 0x90); eb(g, 0x90);  /* jmp +2 over two nops */
    eb(g, 0x83); eb(g, 0xf8); eb(g, 0x00);               /* cmp eax, 0 */
    eb(g, 0x74); eb(g, 0x05);                            /* je +5 */
    eb(g, 0xb8); ed(g, 0xbadbad00u);
    eb(g, 0x0f); eb(g, 0x85); ed(g, 0x00000005u);        /* jne rel32 +5 */
    eb(g, 0xb9); ed(g, 0xbadbad01u);
    eb(g, 0xe9); ed(g, 0x00000005u);                     /* jmp rel32 +5 */
    eb(g, 0xba); ed(g, 0xbadbad02u);
    eb(g, 0xe8); ed(g, 0x00000007u);                     /* call the ret 7 bytes ahead */
    eb(g, 0xbb); ed(g, 0xbadbad03u);
    eb(g, 0xeb); eb(g, 0x01);
    eb(g, 0xc3);                                         /* the callee: just return */
}

static void h_loops(Gen *g)
{
    eb(g, 0x31); eb(g, 0xd2);                            /* xor edx, edx */
    eb(g, 0xb9); ed(g, 5);                               /* mov ecx, 5 */
    eb(g, 0x42);                                         /* L: inc edx */
    eb(g, 0xe2); eb(g, 0xfd);                            /* loop L */
    eb(g, 0xb9); ed(g, 4);
    eb(g, 0x42); eb(g, 0x85); eb(g, 0xd2);               /* inc edx; test edx, edx */
    eb(g, 0xe1); eb(g, 0xfb);                            /* loope L */
    eb(g, 0xb9); ed(g, 4);
    eb(g, 0x42); eb(g, 0x85); eb(g, 0xd2);
    eb(g, 0xe0); eb(g, 0xfb);                            /* loopne L */
    eb(g, 0xb9); ed(g, 0x00010003u);                     /* ECX high half set, CX = 3 */
    eb(g, 0x42);
    eb(g, 0x67); eb(g, 0xe2); eb(g, 0xfc);               /* loop with the 16-bit counter (CX) */
    eb(g, 0x31); eb(g, 0xc9);                            /* xor ecx, ecx */
    eb(g, 0xe3); eb(g, 0x05);                            /* jecxz +5, taken */
    eb(g, 0xb8); ed(g, 0xbadbad04u);
    eb(g, 0xb9); ed(g, 0x00010000u);                     /* CX = 0, ECX != 0 */
    eb(g, 0x67); eb(g, 0xe3); eb(g, 0x05);               /* jcxz +5 */
    eb(g, 0xb8); ed(g, 0xbadbad05u);
}

static void h_string(Gen *g)
{
    mov32(g, OCERZ_RSI, (uint32_t)(SCRATCH_MID + 0x40));
    mov32(g, OCERZ_RDI, (uint32_t)(SCRATCH_MID + 0x80));
    eb(g, 0xfc);                                         /* cld */
    eb(g, 0xb9); ed(g, 8); eb(g, 0xf3); eb(g, 0xa4);     /* rep movsb */
    eb(g, 0xb9); ed(g, 4); eb(g, 0xf3); eb(g, 0xa5);     /* rep movsd */
    eb(g, 0xb9); ed(g, 4); eb(g, 0xf3); eb(g, 0x66); eb(g, 0xa5);  /* rep movsw */
    eb(g, 0xb9); ed(g, 4); eb(g, 0xf3); eb(g, 0xab);     /* rep stosd */
    eb(g, 0xac); eb(g, 0xad);                            /* lodsb; lodsd */
    eb(g, 0xb9); ed(g, 8); eb(g, 0xf2); eb(g, 0xae);     /* repne scasb */
    eb(g, 0xb9); ed(g, 4); eb(g, 0xf3); eb(g, 0xa7);     /* repe cmpsd */
    /* backwards */
    mov32(g, OCERZ_RSI, (uint32_t)(SCRATCH_MID + 0x140));
    mov32(g, OCERZ_RDI, (uint32_t)(SCRATCH_MID + 0x180));
    eb(g, 0xfd);                                         /* std */
    eb(g, 0xb9); ed(g, 8); eb(g, 0xf3); eb(g, 0xa4);
    eb(g, 0xb9); ed(g, 4); eb(g, 0xf3); eb(g, 0xa5);
    eb(g, 0xb9); ed(g, 4); eb(g, 0xf3); eb(g, 0xaa);
    eb(g, 0xa6); eb(g, 0xa7);
    eb(g, 0xfc);
    /* 16-bit counter and pointers: 0x67 makes REP read CX and step SI/DI, and
     * the addresses truncate to 16 bits, so the buffers move to the low
     * window. */
    mov32(g, OCERZ_RSI, (uint32_t)(LOW16 + 0x100));
    mov32(g, OCERZ_RDI, (uint32_t)(LOW16 + 0x200));
    eb(g, 0xb9); ed(g, 4);
    eb(g, 0x67); eb(g, 0xf3); eb(g, 0xa5);
    eb(g, 0xb9); ed(g, 6);
    eb(g, 0x67); eb(g, 0xf3); eb(g, 0xa4);
    eb(g, 0x67); eb(g, 0xac); eb(g, 0x67); eb(g, 0xaa);
}

static void h_shift32(Gen *g)
{
    static const uint8_t cnt[9] = { 0, 1, 7, 8, 15, 16, 31, 32, 33 };
    for (unsigned n = 0; n < 8; n++)
        for (int i = 0; i < 9; i++) {
            mov32(g, OCERZ_RBX, 0x87654321u);
            eb(g, 0xc1); eb(g, 0xc0 | (n << 3) | 3); eb(g, cnt[i]);
            eb(g, 0x89); eb(g, 0x1d); ed(g, (uint32_t)(SCRATCH_MID + 0x400 + 4 * (n * 9 + i)));
        }
}

static void h_shift8_16(Gen *g)
{
    static const uint8_t cnt[6] = { 0, 1, 7, 8, 16, 33 };
    for (unsigned n = 0; n < 8; n++)
        for (int i = 0; i < 6; i++) {
            eb(g, 0xb3); eb(g, 0x9c);                    /* mov bl, 0x9c */
            eb(g, 0xc0); eb(g, 0xc0 | (n << 3) | 3); eb(g, cnt[i]);
            eb(g, 0x66); eb(g, 0xbb); ew(g, 0x8421);     /* mov bx, imm16 */
            eb(g, 0x66); eb(g, 0xc1); eb(g, 0xc0 | (n << 3) | 3); eb(g, cnt[i]);
        }
    eb(g, 0xb1); eb(g, 0x21);                            /* mov cl, 33 */
    eb(g, 0xd3); eb(g, 0xe0);                            /* shl eax, cl */
    eb(g, 0xd3); eb(g, 0xf8);                            /* sar eax, cl */
    eb(g, 0xd2); eb(g, 0xd8);                            /* rcr al, cl */
    eb(g, 0xd1); eb(g, 0xe3);                            /* shl ebx, 1 */
    eb(g, 0xd0); eb(g, 0xd7);                            /* rcl bh, 1 */
}

static void h_shld_shrd(Gen *g)
{
    mov32(g, OCERZ_RAX, 0x12345678u);
    mov32(g, OCERZ_RBX, 0xfedcba98u);
    eb(g, 0xb1); eb(g, 0x0c);                            /* mov cl, 12 */
    eb(g, 0x0f); eb(g, 0xa4); eb(g, 0xd8); eb(g, 0x00);  /* shld eax, ebx, 0 */
    eb(g, 0x0f); eb(g, 0xa4); eb(g, 0xd8); eb(g, 0x10);  /* shld eax, ebx, 16 */
    eb(g, 0x0f); eb(g, 0xa5); eb(g, 0xd8);               /* shld eax, ebx, cl */
    eb(g, 0x0f); eb(g, 0xac); eb(g, 0xd8); eb(g, 0x07);  /* shrd eax, ebx, 7 */
    eb(g, 0x0f); eb(g, 0xad); eb(g, 0xd8);               /* shrd eax, ebx, cl */
    eb(g, 0x66); eb(g, 0x0f); eb(g, 0xa4); eb(g, 0xd8); eb(g, 0x05);
    eb(g, 0x66); eb(g, 0x0f); eb(g, 0xad); eb(g, 0xd8);
    eb(g, 0x0f); eb(g, 0xa4); eb(g, 0x1d); ed(g, (uint32_t)(SCRATCH_MID + 0x40)); eb(g, 0x09);
}

static void h_muldiv(Gen *g)
{
    mov32(g, OCERZ_RAX, 0x0000fedcu);
    eb(g, 0xb1); eb(g, 0x07);                            /* mov cl, 7 */
    eb(g, 0xb4); eb(g, 0x00);                            /* mov ah, 0 */
    eb(g, 0xf6); eb(g, 0xf1);                            /* div cl */
    eb(g, 0x66); eb(g, 0x98);                            /* cbw */
    eb(g, 0xf6); eb(g, 0xf9);                            /* idiv cl */
    mov32(g, OCERZ_RAX, 0x89abcdefu);
    eb(g, 0x31); eb(g, 0xd2);                            /* xor edx, edx */
    eb(g, 0xb9); ed(g, 0x1234);
    eb(g, 0xf7); eb(g, 0xf1);                            /* div ecx */
    mov32(g, OCERZ_RAX, 0x89abcdefu);
    eb(g, 0x99);                                         /* cdq */
    eb(g, 0xb9); ed(g, 0x0123);
    eb(g, 0xf7); eb(g, 0xf9);                            /* idiv ecx */
    eb(g, 0x66); eb(g, 0x31); eb(g, 0xd2);
    eb(g, 0x66); eb(g, 0xb9); ew(g, 0x0037);
    eb(g, 0x66); eb(g, 0xf7); eb(g, 0xf1);               /* div cx */
    eb(g, 0x66); eb(g, 0x99);
    eb(g, 0x66); eb(g, 0xf7); eb(g, 0xf9);               /* idiv cx */
    mov32(g, OCERZ_RAX, 0xdeadbeefu);
    eb(g, 0xf7); eb(g, 0xe3);                            /* mul ebx */
    mov32(g, OCERZ_RAX, 0xdeadbeefu);
    eb(g, 0xf7); eb(g, 0xeb);                            /* imul ebx */
    eb(g, 0xf6); eb(g, 0xe3);                            /* mul bl */
    eb(g, 0xf6); eb(g, 0xeb);                            /* imul bl */
    eb(g, 0x66); eb(g, 0xf7); eb(g, 0xe3);
    eb(g, 0x0f); eb(g, 0xaf); eb(g, 0xc3);               /* imul eax, ebx */
    eb(g, 0x69); eb(g, 0xc3); ed(g, 0x00001234u);        /* imul eax, ebx, imm32 */
    eb(g, 0x6b); eb(g, 0xc3); eb(g, 0xf0);               /* imul eax, ebx, -16 */
    eb(g, 0xf7); eb(g, 0xd8);                            /* neg eax */
    eb(g, 0xf7); eb(g, 0xd0);                            /* not eax */
}

static void h_bt(Gen *g)
{
    static const uint8_t bt[4] = { 0xa3, 0xab, 0xb3, 0xbb };
    for (int i = 0; i < 4; i++) {
        mov32(g, OCERZ_RCX, 0x0000001fu + (unsigned)i * 8);   /* offsets past 32 too */
        eb(g, 0x0f); eb(g, bt[i]); eb(g, 0xc8 | 3);           /* bt* ebx, ecx */
        eb(g, 0x66); eb(g, 0x0f); eb(g, bt[i]); eb(g, 0xc8 | 3);
    }
    for (int i = 0; i < 4; i++) {
        eb(g, 0x0f); eb(g, 0xba); eb(g, 0xc0 | ((4 + i) << 3) | 3); eb(g, 0x25);
        eb(g, 0x0f); eb(g, 0xba); eb(g, ((4 + i) << 3) | 5); ed(g, (uint32_t)(SCRATCH_MID + 0x60)); eb(g, 0x11);
    }
    eb(g, 0x0f); eb(g, 0xbc); eb(g, 0xc3);               /* bsf eax, ebx */
    eb(g, 0x0f); eb(g, 0xbd); eb(g, 0xc3);               /* bsr eax, ebx */
    eb(g, 0x31); eb(g, 0xdb);
    eb(g, 0x0f); eb(g, 0xbc); eb(g, 0xc3);               /* bsf of zero: ZF and an undefined dest */
    eb(g, 0x0f); eb(g, 0xc8);                            /* bswap eax */
    eb(g, 0x0f); eb(g, 0xcb);                            /* bswap ebx */
}

static void h_setcc_cmov(Gen *g)
{
    eb(g, 0x39); eb(g, 0xd8);                            /* cmp eax, ebx */
    for (unsigned cc = 0; cc < 16; cc++) {
        eb(g, 0x0f); eb(g, 0x90 | cc); eb(g, 0xc0 | (cc & 7));            /* setcc r8 */
        eb(g, 0x0f); eb(g, 0x40 | cc); eb(g, 0xc0 | (2 << 3) | 6);        /* cmovcc edx, esi */
        eb(g, 0x66); eb(g, 0x0f); eb(g, 0x40 | cc); eb(g, 0xc0 | (2 << 3) | 7);
    }
    for (unsigned cc = 0; cc < 16; cc++) {
        eb(g, 0x0f); eb(g, 0x90 | cc); eb(g, 0x05); ed(g, (uint32_t)(SCRATCH_MID + 0x80 + cc));
        eb(g, 0x0f); eb(g, 0x40 | cc); eb(g, 0x1d); ed(g, (uint32_t)(SCRATCH_MID + 0xa0));
    }
}

static void h_movzx_movsx(Gen *g)
{
    for (unsigned r = 0; r < 8; r++) {
        eb(g, 0x0f); eb(g, 0xb6); eb(g, 0xc0 | (0 << 3) | r);   /* movzx eax, r8 */
        eb(g, 0x0f); eb(g, 0xbe); eb(g, 0xc0 | (1 << 3) | r);   /* movsx ecx, r8 */
    }
    eb(g, 0x0f); eb(g, 0xb7); eb(g, 0xc3);                      /* movzx eax, bx */
    eb(g, 0x0f); eb(g, 0xbf); eb(g, 0xcb);                      /* movsx ecx, bx */
    eb(g, 0x0f); eb(g, 0xb6); eb(g, 0x05); ed(g, (uint32_t)(SCRATCH_MID + 0x10));
    eb(g, 0x0f); eb(g, 0xbf); eb(g, 0x0d); ed(g, (uint32_t)(SCRATCH_MID + 0x14));
    eb(g, 0x66); eb(g, 0x0f); eb(g, 0xb6); eb(g, 0xc3);         /* movzx bx, bl */
}

/* 32-bit writes zero-extend into the 64-bit slot; 16-bit and 8-bit writes do
 * not.  The case starts with garbage in the upper halves so both halves of
 * that claim are observable. */
static void h_zeroext(Gen *g)
{
    g->c->gpr[OCERZ_RAX] = 0x1111111100000000ull;
    g->c->gpr[OCERZ_RCX] = 0x2222222200000000ull;
    g->c->gpr[OCERZ_RDX] = 0x3333333300000000ull;
    g->c->gpr[OCERZ_RBX] = 0x4444444400000000ull;
    g->c->gpr[OCERZ_RSI] = 0x5555555500000000ull;
    g->c->gpr[OCERZ_RDI] = 0x6666666600000000ull;
    mov32(g, OCERZ_RAX, 0x000000ffu);                    /* 32-bit write */
    eb(g, 0x66); eb(g, 0xb9); ew(g, 0xbeef);             /* 16-bit write: upper bits survive */
    eb(g, 0xb2); eb(g, 0x7f);                            /* 8-bit write */
    eb(g, 0xb7); eb(g, 0x33);                            /* mov bh, 0x33 */
    eb(g, 0x01); eb(g, 0xc6);                            /* add esi, eax */
    eb(g, 0x31); eb(g, 0xff);                            /* xor edi, edi */
    eb(g, 0x0f); eb(g, 0xb6); eb(g, 0xc3);               /* movzx eax, bl */
    eb(g, 0x8d); eb(g, 0x0c); eb(g, 0x24);               /* lea ecx, [esp] */
    eb(g, 0x89); eb(g, 0xe0);                            /* mov eax, esp */
}

static void h_addr16(Gen *g)
{
    /* 0x67 in 32-bit mode selects the 16-bit addressing forms, whose effective
     * address is computed modulo 65536.  Every register that takes part is
     * loaded here, so the address is known at generation time. */
    eb(g, 0x66); eb(g, 0xbb); ew(g, (unsigned)(LOW16 + 0x20));   /* mov bx, .. */
    eb(g, 0x66); eb(g, 0xbe); ew(g, 0x0010);                     /* mov si, 16 */
    eb(g, 0x66); eb(g, 0xbf); ew(g, 0x0020);                     /* mov di, 32 */
    eb(g, 0x67); eb(g, 0x8b); eb(g, 0x40); eb(g, 0x04);          /* mov eax, [bx+si+4] */
    eb(g, 0x67); eb(g, 0x89); eb(g, 0x49); eb(g, 0x08);          /* mov [bx+di+8], ecx */
    eb(g, 0x67); eb(g, 0x8a); eb(g, 0x94); ew(g, (unsigned)LOW16);           /* mov dl, [si+disp16] */
    eb(g, 0x67); eb(g, 0x8b); eb(g, 0x1e); ew(g, (unsigned)(LOW16 + 0x40));  /* mov ebx, [disp16] */
    eb(g, 0x67); eb(g, 0x66); eb(g, 0x89); eb(g, 0x85); ew(g, (unsigned)LOW16);  /* mov [di+disp16], ax */
    eb(g, 0x67); eb(g, 0xc6); eb(g, 0x85); ew(g, (unsigned)(LOW16 + 0x10)); eb(g, 0x5a);
    eb(g, 0x67); eb(g, 0x8d); eb(g, 0x41); eb(g, 0x7f);          /* lea eax, [bx+di+127] */
    eb(g, 0x67); eb(g, 0x8d); eb(g, 0x1e); ew(g, 0xffff);        /* lea ebx, [disp16]: no access */
    /* [BP+..] in 16-bit addressing: BP is EBP truncated, so the displacement
     * carries the flow back into the low window rather than the scratch. */
    eb(g, 0x67); eb(g, 0x8b); eb(g, 0x86);
    ew(g, (unsigned)((LOW16 - (SCRATCH_MID & 0xffff)) & 0xffff));            /* mov eax, [bp+disp16] */
}

static void h_lea(Gen *g)
{
    eb(g, 0x83); eb(g, 0xe1); eb(g, 0x0f);                       /* and ecx, 15 */
    eb(g, 0x8d); eb(g, 0x05); ed(g, 0x12345678u);                /* lea eax, [disp32]  (absolute!) */
    eb(g, 0x8d); eb(g, 0x04); eb(g, 0x8d); ed(g, 0x00001000u);   /* lea eax, [ecx*4+0x1000] */
    eb(g, 0x8d); eb(g, 0x44); eb(g, 0x8d); eb(g, 0x10);          /* lea eax, [ebp+ecx*4+16] */
    eb(g, 0x8d); eb(g, 0x04); eb(g, 0x24);                       /* lea eax, [esp] */
    eb(g, 0x8d); eb(g, 0x44); eb(g, 0x24); eb(g, 0x08);          /* lea eax, [esp+8] */
    eb(g, 0x8d); eb(g, 0x45); eb(g, 0x00);                       /* lea eax, [ebp+0] */
    eb(g, 0x66); eb(g, 0x8d); eb(g, 0x45); eb(g, 0x04);          /* lea ax, [ebp+4] -- 16-bit result */
    eb(g, 0x8d); eb(g, 0x0c); eb(g, 0x49);                       /* lea ecx, [ecx+ecx*2] */
}

static void h_seg_prefix(Gen *g)
{
    static const uint8_t seg[4] = { 0x2e, 0x36, 0x3e, 0x26 };
    for (int i = 0; i < 4; i++) {
        eb(g, seg[i]); eb(g, 0x8b); eb(g, 0x45); eb(g, 0x00);    /* mov eax, seg:[ebp] */
        eb(g, seg[i]); eb(g, 0x89); eb(g, 0x4d); eb(g, 0x04);    /* mov seg:[ebp+4], ecx */
    }
    eb(g, 0x64); eb(g, 0x8b); eb(g, 0x05); ed(g, (uint32_t)SCRATCH_MID);  /* fs: with a zero base */
    eb(g, 0x65); eb(g, 0x8b); eb(g, 0x0d); ed(g, (uint32_t)SCRATCH_MID);  /* gs: with a zero base */
}

/* Frame setup and LEAVE.  ENTER is deliberately absent: opcode 0xc8 has no row
 * in src/decode.c's map, so writing one here would test a decode failure and
 * nothing else.  A stage that adds ENTER should add it to this case too. */
static void h_enter_leave(Gen *g)
{
    eb(g, 0x55);                                         /* push ebp */
    eb(g, 0x89); eb(g, 0xe5);                            /* mov ebp, esp */
    eb(g, 0x83); eb(g, 0xec); eb(g, 0x20);               /* sub esp, 32 */
    eb(g, 0x89); eb(g, 0x45); eb(g, 0xfc);               /* mov [ebp-4], eax */
    eb(g, 0x8b); eb(g, 0x4d); eb(g, 0xfc);               /* mov ecx, [ebp-4] */
    eb(g, 0xc9);                                         /* leave */
    eb(g, 0x55);
    eb(g, 0x89); eb(g, 0xe5);
    eb(g, 0x83); eb(g, 0xec); eb(g, 0x10);
    eb(g, 0x89); eb(g, 0x5d); eb(g, 0xf8);
    eb(g, 0x66); eb(g, 0x8b); eb(g, 0x55); eb(g, 0xf8);  /* mov dx, [ebp-8] */
    eb(g, 0xc9);                                         /* leave */
    eb(g, 0x55);
    eb(g, 0x89); eb(g, 0xe5);
    eb(g, 0xc9);                                         /* the zero-size frame */
}

static void h_xchg_lock(Gen *g)
{
    eb(g, 0x87); eb(g, 0x1d); ed(g, (uint32_t)(SCRATCH_MID + 0x10));   /* xchg [abs], ebx: implicitly locked */
    eb(g, 0x86); eb(g, 0x0d); ed(g, (uint32_t)(SCRATCH_MID + 0x14));   /* xchg [abs], cl */
    eb(g, 0xf0); eb(g, 0x01); eb(g, 0x1d); ed(g, (uint32_t)(SCRATCH_MID + 0x18));
    eb(g, 0xf0); eb(g, 0x0f); eb(g, 0xc1); eb(g, 0x1d); ed(g, (uint32_t)(SCRATCH_MID + 0x1c));  /* lock xadd */
    eb(g, 0x8b); eb(g, 0x05); ed(g, (uint32_t)(SCRATCH_MID + 0x20));   /* mov eax, [abs] */
    eb(g, 0xf0); eb(g, 0x0f); eb(g, 0xb1); eb(g, 0x1d); ed(g, (uint32_t)(SCRATCH_MID + 0x20));  /* lock cmpxchg: match */
    eb(g, 0xb8); ed(g, 0xdeadbeefu);
    eb(g, 0xf0); eb(g, 0x0f); eb(g, 0xb1); eb(g, 0x1d); ed(g, (uint32_t)(SCRATCH_MID + 0x20));  /* mismatch */
    eb(g, 0x91); eb(g, 0x93); eb(g, 0x97);               /* xchg eax, ecx/ebx/edi */
    eb(g, 0x66); eb(g, 0x91);
    eb(g, 0x90);                                         /* the 0x90 that is NOP, not xchg eax, eax */
}

static void h_conv_flags(Gen *g)
{
    mov32(g, OCERZ_RAX, 0xffff8000u);
    eb(g, 0x98);                                         /* cwde */
    eb(g, 0x99);                                         /* cdq */
    eb(g, 0x66); eb(g, 0x98);                            /* cbw */
    eb(g, 0x66); eb(g, 0x99);                            /* cwd */
    eb(g, 0xf8); eb(g, 0xf5); eb(g, 0xf9); eb(g, 0xf5);  /* clc cmc stc cmc */
    eb(g, 0xfd); eb(g, 0xfc);                            /* std cld */
    eb(g, 0x9f); eb(g, 0x9e);                            /* lahf sahf */
    eb(g, 0xd6);                                         /* salc */
    eb(g, 0xf9); eb(g, 0xd6);                            /* stc; salc */
}

static void h_esp_sib(Gen *g)
{
    /* SIB with base=ESP: the encoding that has no shorter form, and the one a
     * 32-bit stack frame is built from. */
    eb(g, 0x50); eb(g, 0x51); eb(g, 0x52); eb(g, 0x53);
    eb(g, 0x8b); eb(g, 0x04); eb(g, 0x24);               /* mov eax, [esp] */
    eb(g, 0x8b); eb(g, 0x4c); eb(g, 0x24); eb(g, 0x04);  /* mov ecx, [esp+4] */
    eb(g, 0x89); eb(g, 0x54); eb(g, 0x24); eb(g, 0x08);  /* mov [esp+8], edx */
    eb(g, 0x83); eb(g, 0x44); eb(g, 0x24); eb(g, 0x0c); eb(g, 0x01);  /* add dword [esp+12], 1 */
    eb(g, 0x8b); eb(g, 0x84); eb(g, 0x24); ed(g, 0x00000010u);        /* mov eax, [esp+16] */
    eb(g, 0x83); eb(g, 0xe1); eb(g, 0x03);
    eb(g, 0x8b); eb(g, 0x04); eb(g, 0x8c);               /* mov eax, [esp+ecx*4] */
    eb(g, 0x83); eb(g, 0xc4); eb(g, 0x10);               /* add esp, 16 */
}

/* CALL/RET with 4-byte return addresses, in every shape a 32-bit compiler
 * emits: rel32, register-indirect, memory-indirect, and RET imm16.  Offsets
 * are computed from what has already been written rather than counted by hand,
 * so editing one line cannot silently mis-aim a displacement. */
static void h_callret(Gen *g)
{
    size_t j, at, c1, c2, c3;
    int32_t rel;

    eb(g, 0xeb); j = g->c->len; eb(g, 0);                /* jmp over callee 1 */
    c1 = g->c->len;
    eb(g, 0x40); eb(g, 0xc3);                            /* inc eax; ret */
    g->c->code[j] = (uint8_t)(g->c->len - (j + 1));
    eb(g, 0xe8); at = g->c->len; ed(g, 0);
    rel = (int32_t)((int64_t)c1 - (int64_t)g->c->len);
    memcpy(&g->c->code[at], &rel, 4);

    eb(g, 0xeb); j = g->c->len; eb(g, 0);                /* jmp over callee 2 */
    c2 = g->c->len;
    eb(g, 0x41); eb(g, 0xc2); ew(g, 0x0008);             /* inc ecx; ret 8 */
    g->c->code[j] = (uint8_t)(g->c->len - (j + 1));
    eb(g, 0x6a); eb(g, 0x11); eb(g, 0x6a); eb(g, 0x22);  /* two 4-byte arguments */
    eb(g, 0xe8); at = g->c->len; ed(g, 0);
    rel = (int32_t)((int64_t)c2 - (int64_t)g->c->len);
    memcpy(&g->c->code[at], &rel, 4);

    eb(g, 0xeb); j = g->c->len; eb(g, 0);                /* jmp over callee 3 */
    c3 = g->c->len;
    eb(g, 0x42); eb(g, 0xc3);                            /* inc edx; ret */
    g->c->code[j] = (uint8_t)(g->c->len - (j + 1));
    eb(g, 0xbb); ed(g, (uint32_t)(CODE32 + c3));         /* mov ebx, callee3 */
    eb(g, 0x89); eb(g, 0x1d); ed(g, (uint32_t)(SCRATCH_MID + 0x30));
    eb(g, 0xff); eb(g, 0xd3);                            /* call ebx */
    eb(g, 0xff); eb(g, 0x15); ed(g, (uint32_t)(SCRATCH_MID + 0x30));  /* call dword [abs] */
    /* An indirect near JMP, made safe by pushing the address the callee's RET
     * will return to -- which is exactly where the terminator lands. */
    eb(g, 0x68); at = g->c->len; ed(g, 0);
    eb(g, 0xff); eb(g, 0xe0 | 3);                        /* jmp ebx */
    { uint32_t term = (uint32_t)(CODE32 + g->c->len); memcpy(&g->c->code[at], &term, 4); }
}

static const struct { const char *name; void (*fn)(Gen *); } HANDS[] = {
    { "highbyte",      h_highbyte },
    { "highbyte-mem",  h_highbyte_mem },
    { "mov-sreg",      h_mov_sreg },
    { "disp32-abs",    h_disp32_abs },
    { "moffs",         h_moffs },
    { "stack4",        h_stack4 },
    { "stack2",        h_stack2 },
    { "pushf-popf",    h_pushf_popf },
    { "pusha-popa",    h_pusha_popa },
    { "bcd",           h_bcd },
    { "aam-aad",       h_aam_aad },
    { "jmp16-trunc",   h_jmp16 },
    { "jcc16-trunc",   h_jcc16 },
    { "call16-trunc",  h_call16 },
    { "near32",        h_near32 },
    { "loops",         h_loops },
    { "string",        h_string },
    { "shift32",       h_shift32 },
    { "shift8-16",     h_shift8_16 },
    { "shld-shrd",     h_shld_shrd },
    { "muldiv",        h_muldiv },
    { "bt-bsf-bswap",  h_bt },
    { "setcc-cmov",    h_setcc_cmov },
    { "movzx-movsx",   h_movzx_movsx },
    { "zero-extend",   h_zeroext },
    { "addr16",        h_addr16 },
    { "lea",           h_lea },
    { "seg-prefix",    h_seg_prefix },
    { "enter-leave",   h_enter_leave },
    { "xchg-lock",     h_xchg_lock },
    { "conv-flags",    h_conv_flags },
    { "esp-sib",       h_esp_sib },
    { "call-ret",      h_callret },
};
#define NHANDS ((int)(sizeof HANDS / sizeof HANDS[0]))

static void gen_hand(Case *c, int i, uint64_t seed)
{
    Gen g;
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "hand/%s", HANDS[i].name);
    g.c = c;
    g.rng = seed ^ (0x5bf03635ull * (uint64_t)(i + 1));
    g.depth = 0;
    g_avoid = 0xff;
    for (int k = 0; k < 16; k++)
        c->gpr[k] = sm64(&g.rng);
    c->gpr[OCERZ_RSP] = ESP0;
    c->gpr[OCERZ_RBP] = SCRATCH_MID;
    c->rflags = OCERZ_FLAG_FIXED1 | OCERZ_IF;
    c->memseed = 0x9e3779b97f4a7c15ull ^ (uint64_t)i;
    HANDS[i].fn(&g);
}

/* ---------------------------------------------------------------------------
 * The runner: load, run twice, compare.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t gpr[16];
    uint64_t rip;
    uint64_t rflags;
    uint16_t seg_sel[6];
    uint16_t cs_sel;
    uint8_t  mode32;
    int      rc;
    Ocerz128 xmm[16];
    double   fpr[8];
    uint32_t mxcsr;
    uint16_t fcw, fsw;
    uint8_t  ftw, ftop;
    unsigned long long steps;
    uint8_t  mem[SNAPLEN];
} Snap;

static OcerzVM g_vm;
static uint8_t g_golden[SNAPLEN];
static unsigned long long g_budget = 200000;

/* Corpus statistics.  A differential that quietly generates the same six
 * instructions forever still prints 100%, so the summary says how much was
 * actually executed and how many distinct opcodes the corpus reached. */
static unsigned long long g_insns_emitted, g_insns_executed;
static uint8_t g_opseen[OCERZ_OP_COUNT];

static void tally(const Case *c)
{
    uint64_t rip = CODE32;
    const uint8_t *code = (const uint8_t *)ocerz_g2h(CODE32);
    while (rip < CODE32 + c->len) {
        X86Insn insn;
        if (ocerz_decode_mode(code + (rip - CODE32), 15, rip, &insn, 1) != OCERZ_OK)
            break;
        if (insn.op < OCERZ_OP_COUNT)
            g_opseen[insn.op] = 1;
        g_insns_emitted++;
        rip += insn.len;
    }
}

static void build_golden(const Case *c)
{
    uint64_t s = c->memseed ? c->memseed : 0x0123456789abcdefull;
    for (size_t i = 0; i < SNAPLEN; i += 8) {
        uint64_t v = sm64(&s);
        memcpy(g_golden + i, &v, 8);
    }
}

static void restore_memory(void)
{
    size_t off = 0;
    for (int i = 0; i < NREGIONS; i++) {
        memcpy(ocerz_g2h(REGIONS[i].addr), g_golden + off, (size_t)REGIONS[i].len);
        off += (size_t)REGIONS[i].len;
    }
}

static void snap_memory(uint8_t *out)
{
    size_t off = 0;
    for (int i = 0; i < NREGIONS; i++) {
        memcpy(out + off, ocerz_g2h(REGIONS[i].addr), (size_t)REGIONS[i].len);
        off += (size_t)REGIONS[i].len;
    }
}

/* Which guest address a byte of the flattened snapshot came from. */
static uint64_t snap_addr(size_t off, const char **region)
{
    for (int i = 0; i < NREGIONS; i++) {
        if (off < (size_t)REGIONS[i].len) {
            *region = REGIONS[i].name;
            return REGIONS[i].addr + off;
        }
        off -= (size_t)REGIONS[i].len;
    }
    *region = "?";
    return 0;
}

static void load_case(const Case *c)
{
    uint8_t *code = (uint8_t *)ocerz_g2h(CODE32);
    uint32_t fp = (uint32_t)FARPTR;
    uint8_t term[6] = { 0xff, 0x2d, 0, 0, 0, 0 };
    memcpy(term + 2, &fp, 4);

    memset(code, 0xf4, (size_t)CODE32_LEN);
    memset(ocerz_g2h(LOW_CODE), 0xf4, (size_t)LOW_CODE_LEN);
    memcpy(code, c->code, c->len);
    memcpy(code + c->len, term, sizeof term);
    for (int i = 0; i < c->nplant; i++)
        memcpy(ocerz_g2h(c->plant[i].addr), c->plant[i].bytes, c->plant[i].len);

    /* Every case reuses the same guest addresses, so a block translated for
     * the previous case would be stale code with a valid key.  This is the one
     * place the harness must not economise. */
    ocerz_jit_invalidate_all(&g_vm);
    build_golden(c);
}

/* --bug: a sensitivity probe.
 *
 * "100% pass" against a JIT that never runs measures nothing, and neither does
 * a corpus of instructions whose results all happen to be zero.  With --bug N
 * the JIT SIDE is deliberately made wrong in one specific, realistic way, and
 * the reported failure rate becomes the fraction of the corpus that is
 * sensitive to that class of bug.  Those numbers are the honest answer to
 * "would this gate have caught it".
 *
 *   1 no-zeroext  a 32-bit write leaves the upper half of the 64-bit slot
 *                 alone instead of zeroing it -- the single most likely stage
 *                 9 mistake, and the one the WoW64 contract turns on
 *   2 stale-zf    ZF keeps its previous value: a lazy-flags materialisation
 *                 that forgets one flag
 *   3 cf-flip     one CF bit flipped once, mid-sequence: the smallest possible
 *                 divergence, so its rate is a floor on the gate's resolution
 */
static int g_bug;
static const char *BUGNAME[] = { "none", "no-zeroext", "stale-zf", "cf-flip" };

static void run_side(const Case *c, int use_jit, Snap *s)
{
    OcerzCPU *cpu = &g_vm.cpu;

    restore_memory();
    ocerz_cpu_reset(cpu);
    for (int i = 0; i < 16; i++)
        cpu->gpr[i] = c->gpr[i];
    cpu->rflags = c->rflags;
    cpu->mode32 = 1;
    cpu->cs_sel = (uint16_t)CS32;
    cpu->seg_sel[OCERZ_SREG_CS] = (uint16_t)CS32;
    cpu->rip = CODE32;
    g_vm.jit_enabled = use_jit;
    g_vm.exited = 0;

    unsigned long long steps = 0;
    int rc = OCERZ_STEP_OK;
    /* The same dispatch vm.c uses, minus the tracing hooks: a mode change is
     * what ends the run, so the loop tests it before every step. */
    while (cpu->mode32 && steps < g_budget) {
        uint64_t prev_rax = cpu->gpr[OCERZ_RAX];
        int prev_zf = 0;
        if (use_jit && g_bug == 2) {
            ocerz_flags_materialize(cpu);
            prev_zf = (cpu->rflags & OCERZ_ZF) != 0;
        }
        steps++;
        if (use_jit && g_vm.jit) {
            rc = ocerz_jit_step(&g_vm, cpu);
            if (rc == OCERZ_EUNSUP)
                rc = ocerz_interp_step(&g_vm, cpu);
        } else {
            rc = ocerz_interp_step(&g_vm, cpu);
        }
        if (use_jit && g_bug) {
            switch (g_bug) {
            case 1:
                if ((uint32_t)cpu->gpr[OCERZ_RAX] != (uint32_t)prev_rax)
                    cpu->gpr[OCERZ_RAX] = (prev_rax & 0xffffffff00000000ull) |
                                          (uint32_t)cpu->gpr[OCERZ_RAX];
                break;
            case 2:
                ocerz_flags_materialize(cpu);
                ocerz_flag_assign(cpu, OCERZ_ZF, prev_zf);
                break;
            default:
                if (steps == 2) {
                    ocerz_flags_materialize(cpu);
                    cpu->rflags ^= OCERZ_CF;
                }
                break;
            }
        }
        if (rc == OCERZ_STEP_FATAL || rc == OCERZ_STEP_EXIT)
            break;
        if (cpu->terminated || cpu->interrupt)
            break;
    }
    ocerz_flags_materialize(cpu);

    memset(s, 0, sizeof *s);
    memcpy(s->gpr, cpu->gpr, sizeof s->gpr);
    s->rip = cpu->rip;
    s->rflags = cpu->rflags;
    memcpy(s->seg_sel, cpu->seg_sel, sizeof s->seg_sel);
    s->cs_sel = cpu->cs_sel;
    s->mode32 = cpu->mode32;
    s->rc = rc;
    memcpy(s->xmm, cpu->xmm, sizeof s->xmm);
    memcpy(s->fpr, cpu->fpr, sizeof s->fpr);
    s->mxcsr = cpu->mxcsr;
    s->fcw = cpu->fcw;
    s->fsw = cpu->fsw;
    s->ftw = cpu->ftw;
    s->ftop = cpu->ftop;
    s->steps = steps;
    snap_memory(s->mem);
}

/* The architectural EFLAGS bits.  Everything else in the register is either
 * fixed or system state a 32-bit user sequence cannot reach, and comparing it
 * would turn a harmless representation choice into a false failure. */
#define EFLAGS_MASK 0x0000000000000cd5ull

static const char *REGNAME[16] = {
    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
};

/* A case that did not reach its terminator proves nothing: two engines that
 * both trap on instruction 1 agree trivially.  Every sequence this harness
 * builds is supposed to run to the far jump, so anything else is a generator
 * bug and is reported as a failure rather than counted as a pass. */
static const char *incomplete(const Snap *s)
{
    static char buf[128];
    if (s->mode32) {
        snprintf(buf, sizeof buf,
                 "sequence never left 32-bit mode (rc=%d, %llu steps, eip=%#llx) -- "
                 "budget exhausted or the terminator was not reached",
                 s->rc, s->steps, (unsigned long long)s->rip);
        return buf;
    }
    if (s->rc != OCERZ_STEP_OK) {
        snprintf(buf, sizeof buf, "sequence trapped: step result %d at eip=%#llx",
                 s->rc, (unsigned long long)s->rip);
        return buf;
    }
    if (s->rip != HALT64) {
        snprintf(buf, sizeof buf, "left 32-bit mode at %#llx, expected the terminator target %#llx",
                 (unsigned long long)s->rip, (unsigned long long)HALT64);
        return buf;
    }
    return NULL;
}

/* Returns NULL when the two results are identical, else a description of the
 * FIRST difference -- one line, because the first divergence is the one worth
 * looking at and a hundred downstream ones are noise. */
static const char *diff_snaps(const Snap *a, const Snap *b)
{
    static char buf[256];
    if (a->rc != b->rc) {
        snprintf(buf, sizeof buf, "step result: interp=%d jit=%d", a->rc, b->rc);
        return buf;
    }
    if (a->mode32 != b->mode32) {
        snprintf(buf, sizeof buf, "mode32: interp=%u jit=%u", a->mode32, b->mode32);
        return buf;
    }
    if (a->rip != b->rip) {
        snprintf(buf, sizeof buf, "rip: interp=%#llx jit=%#llx",
                 (unsigned long long)a->rip, (unsigned long long)b->rip);
        return buf;
    }
    for (int i = 0; i < 16; i++)
        if (a->gpr[i] != b->gpr[i]) {
            snprintf(buf, sizeof buf, "%s: interp=%016llx jit=%016llx", REGNAME[i],
                     (unsigned long long)a->gpr[i], (unsigned long long)b->gpr[i]);
            return buf;
        }
    if ((a->rflags & EFLAGS_MASK) != (b->rflags & EFLAGS_MASK)) {
        snprintf(buf, sizeof buf, "eflags: interp=%#llx jit=%#llx (differ in %#llx)",
                 (unsigned long long)(a->rflags & EFLAGS_MASK),
                 (unsigned long long)(b->rflags & EFLAGS_MASK),
                 (unsigned long long)((a->rflags ^ b->rflags) & EFLAGS_MASK));
        return buf;
    }
    if (a->cs_sel != b->cs_sel) {
        snprintf(buf, sizeof buf, "cs: interp=%#x jit=%#x", a->cs_sel, b->cs_sel);
        return buf;
    }
    for (int i = 0; i < 6; i++)
        if (a->seg_sel[i] != b->seg_sel[i]) {
            snprintf(buf, sizeof buf, "seg_sel[%d]: interp=%#x jit=%#x",
                     i, a->seg_sel[i], b->seg_sel[i]);
            return buf;
        }
    for (int i = 0; i < 16; i++)
        if (a->xmm[i].lo != b->xmm[i].lo || a->xmm[i].hi != b->xmm[i].hi) {
            snprintf(buf, sizeof buf, "xmm%d differs", i);
            return buf;
        }
    if (memcmp(a->fpr, b->fpr, sizeof a->fpr) != 0 ||
        a->mxcsr != b->mxcsr || a->fcw != b->fcw || a->fsw != b->fsw ||
        a->ftw != b->ftw || a->ftop != b->ftop) {
        snprintf(buf, sizeof buf, "x87/SSE control or stack state differs");
        return buf;
    }
    for (size_t i = 0; i < SNAPLEN; i++)
        if (a->mem[i] != b->mem[i]) {
            const char *rg;
            uint64_t ga = snap_addr(i, &rg);
            snprintf(buf, sizeof buf, "memory %s %#llx: interp=%02x jit=%02x",
                     rg, (unsigned long long)ga, a->mem[i], b->mem[i]);
            return buf;
        }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Failure reporting.  A differential is only useful if a red line tells you
 * what to look at, so a failure prints the seed that reproduces it, the raw
 * bytes, and the sequence disassembled in i386 mode.
 * ------------------------------------------------------------------------- */
static void dump_case(const Case *c, const char *why, uint64_t seed)
{
    fprintf(stderr, "FAIL %s: %s\n", c->name, why);
    fprintf(stderr, "  reproduce: diff32 --seed %#llx --only %s\n",
            (unsigned long long)seed, c->name);
    fprintf(stderr, "  initial:");
    for (int i = 0; i < 8; i++)
        fprintf(stderr, " %s=%016llx", REGNAME[i], (unsigned long long)c->gpr[i]);
    fprintf(stderr, "\n  eflags=%#llx  memseed=%#llx\n",
            (unsigned long long)c->rflags, (unsigned long long)c->memseed);
    fprintf(stderr, "  bytes:");
    for (size_t i = 0; i < c->len && i < 96; i++)
        fprintf(stderr, " %02x", c->code[i]);
    if (c->len > 96)
        fprintf(stderr, " ...(%zu total)", c->len);
    fprintf(stderr, "\n");

    uint64_t rip = CODE32;
    const uint8_t *code = (const uint8_t *)ocerz_g2h(CODE32);
    for (int n = 0; n < 64 && rip < CODE32 + c->len + 6; n++) {
        X86Insn insn;
        char text[128];
        if (ocerz_decode_mode(code + (rip - CODE32), 15, rip, &insn, 1) != OCERZ_OK) {
            fprintf(stderr, "    %#llx: <undecodable>\n", (unsigned long long)rip);
            break;
        }
        ocerz_format_insn(&insn, text, sizeof text);
        fprintf(stderr, "    %#llx: %s\n", (unsigned long long)rip, text);
        rip += insn.len;
    }
}

/* ---------------------------------------------------------------------------
 * Self-test: prove the comparator is not vacuous.
 *
 * While the JIT refuses 32-bit blocks both sides interpret, so a 100% pass
 * rate would also be the result of a comparator that compares nothing.  Each
 * injection below corrupts exactly one class of compared state on the JIT-side
 * result and requires diff_snaps() to notice.
 * ------------------------------------------------------------------------- */
static const char *INJECT[] = {
    "gpr-low32", "gpr-high32", "eflags", "eip", "mode32", "cs",
    "xmm", "scratch-byte", "stack-byte", "low16-byte", "step-result",
};
#define NINJECT ((int)(sizeof INJECT / sizeof INJECT[0]))

static void inject(Snap *s, int which)
{
    size_t off_scratch = 0;
    size_t off_stack = (size_t)SCRATCH_LEN;
    size_t off_low16 = (size_t)(SCRATCH_LEN + STACK_CMP_LEN);
    switch (which) {
    case 0:  s->gpr[OCERZ_RAX] ^= 1; break;
    case 1:  s->gpr[OCERZ_RSI] ^= 0x100000000ull; break;   /* only the upper half */
    case 2:  s->rflags ^= OCERZ_CF; break;
    case 3:  s->rip ^= 4; break;
    case 4:  s->mode32 ^= 1; break;
    case 5:  s->cs_sel ^= 8; break;
    case 6:  s->xmm[3].hi ^= 1; break;
    case 7:  s->mem[off_scratch + 0x123] ^= 0xff; break;
    case 8:  s->mem[off_stack + 0x45] ^= 0xff; break;
    case 9:  s->mem[off_low16 + 0x7] ^= 0xff; break;
    default: s->rc = OCERZ_STEP_FATAL; break;
    }
}

/* Two properties this harness depends on and cannot observe from a 32-bit
 * sequence while the JIT refuses one:
 *
 *   1. the JIT-side dispatch really reaches the JIT.  If ocerz_jit_step() were
 *      never called, or the VM had no jit, the "JIT side" would be a second
 *      interpreter run and the gate would be green forever.
 *   2. ocerz_jit_invalidate_all() really evicts.  Every case rewrites the same
 *      guest addresses, so a surviving block would be the previous case's code
 *      under this case's key -- which is exactly the stale-object failure mode
 *      that makes a gate lie.
 *
 * Both are checked with a 64-bit block, because 64 bits is the mode the JIT
 * does compile today.  When stage 9 lands these keep testing the same two
 * properties for the 32-bit path, since it is the same dispatcher and the same
 * cache.
 */
static int jit_plumbing(void)
{
    OcerzCPU *cpu = &g_vm.cpu;
    uint8_t *p = (uint8_t *)ocerz_g2h(CODE64);
    int bad = 0;

    for (int round = 0; round < 2; round++) {
        /* add rax, 1|2 ; jmp CODE64+0x80  -- the jump ends the block, so one
         * ocerz_jit_step() translates it, runs it and returns. */
        uint8_t code[9] = { 0x48, 0x83, 0xc0, (uint8_t)(round + 1), 0xe9, 0, 0, 0, 0 };
        int32_t rel = (int32_t)(0x80 - 9);
        memcpy(code + 5, &rel, 4);
        memset(p, 0xf4, (size_t)CODE64_LEN);
        memcpy(p, code, sizeof code);
        ocerz_jit_invalidate_all(&g_vm);

        uint64_t before = ocerz_jit_blocks(g_vm.jit);
        ocerz_cpu_reset(cpu);
        cpu->rip = CODE64;
        cpu->gpr[OCERZ_RAX] = 41;
        cpu->gpr[OCERZ_RSP] = ESP0;
        g_vm.jit_enabled = 1;
        int rc = ocerz_jit_step(&g_vm, cpu);
        uint64_t after = ocerz_jit_blocks(g_vm.jit);

        if (rc != OCERZ_STEP_OK || after == before ||
            cpu->gpr[OCERZ_RAX] != (uint64_t)(41 + round + 1) ||
            cpu->rip != CODE64 + 0x80) {
            fprintf(stderr, "FAIL selftest/jit-%s: rc=%d blocks %llu->%llu rax=%llu rip=%#llx\n",
                    round ? "invalidate" : "reached", rc,
                    (unsigned long long)before, (unsigned long long)after,
                    (unsigned long long)cpu->gpr[OCERZ_RAX],
                    (unsigned long long)cpu->rip);
            bad++;
        } else {
            printf("PASS selftest/jit-%-8s (%s)\n", round ? "invalidate" : "reached",
                   round ? "a rewritten block at the same address was retranslated"
                         : "ocerz_jit_step translated and ran a block through this dispatcher");
        }
    }
    memset(p, 0xf4, (size_t)CODE64_LEN);
    ocerz_jit_invalidate_all(&g_vm);
    return bad;
}

static int selftest(uint64_t seed)
{
    Case c;
    Snap a, b;
    int bad = 0;

    gen_hand(&c, 0, seed);
    load_case(&c);
    run_side(&c, 0, &a);
    run_side(&c, 0, &b);
    if (diff_snaps(&a, &b) != NULL) {
        fprintf(stderr, "FAIL selftest/determinism: two identical runs differ (%s)\n",
                diff_snaps(&a, &b));
        bad++;
    } else {
        printf("PASS selftest/determinism (two identical runs agree)\n");
    }

    bad += jit_plumbing();

    for (int i = 0; i < NINJECT; i++) {
        Snap m = b;
        inject(&m, i);
        const char *d = diff_snaps(&a, &m);
        if (!d) {
            fprintf(stderr, "FAIL selftest/%s: the comparator did NOT see the injected difference\n",
                    INJECT[i]);
            bad++;
        } else {
            printf("PASS selftest/%-12s caught: %s\n", INJECT[i], d);
        }
    }
    return bad;
}

/* ------------------------------------------------------------------------- */
static int setup_memory(void)
{
    if (ocerz_mem_init(ARENA_LO, ARENA_HI) != OCERZ_OK) {
        fprintf(stderr, "diff32: mem_init failed\n");
        return 0;
    }
    static const struct { uint64_t a, l; } maps[] = {
        { LOW_CODE, LOW_CODE_LEN }, { LOW16, LOW16_LEN },
        { CODE64,   CODE64_LEN },   { CODE32, CODE32_LEN },
        { FARPTR,   FARPTR_LEN },   { SCRATCH, SCRATCH_LEN },
        { STACK_LO, STACK_LEN },
    };
    for (unsigned i = 0; i < sizeof maps / sizeof maps[0]; i++)
        if (ocerz_map_fixed(maps[i].a, maps[i].l, PROT_READ | PROT_WRITE) != OCERZ_OK) {
            fprintf(stderr, "diff32: map_fixed(%#llx, %#llx) failed\n",
                    (unsigned long long)maps[i].a, (unsigned long long)maps[i].l);
            return 0;
        }

    /* The far pointer the terminator jumps through: a 4-byte offset and a
     * 2-byte selector, exactly the m16:32 wow64cpu's fast return reads. */
    ocerz_st(FARPTR, 4, HALT64);
    ocerz_st(FARPTR + 4, 2, CS64);

    /* Anything that is executed by accident must be loud, not silent. */
    memset(ocerz_g2h(CODE64), 0xf4, (size_t)CODE64_LEN);

    /* The 32-bit code segment the guest would install through i386_set_ldt.
     * base 0 keeps it flat, which is what wine's WoW64 sets up. */
    ocerz_ldt_install(CS32, 0, 0xfffff, 0xfb, /*big*/1, /*is_long*/0, /*gran*/1);
    return 1;
}

static void usage(void)
{
    printf("usage: diff32 [options]\n"
           "  --seed N          RNG seed (default 1); a failure prints the seed that reproduces it\n"
           "  --cases N         random sequences to generate (default 20000)\n"
           "  --budget N        instruction/block budget per run (default 200000)\n"
           "  --only SUBSTR     run only cases whose name contains SUBSTR\n"
           "  --list            list the hand-written cases and exit\n"
           "  --selftest        prove the comparator catches an injected difference, then exit\n"
           "  --jit-required    fail if the JIT translated no 32-bit blocks (the gate passes this)\n"
           "  --bug N           sensitivity probe: make the JIT side deliberately wrong\n"
           "                    (1 no-zeroext, 2 stale-zf, 3 cf-flip) and report the catch rate\n"
           "  --verbose         print a line per random case as well\n");
}

int main(int argc, char **argv)
{
    uint64_t seed = 1;
    long ncases = 20000;
    int do_selftest = 0, jit_required = 0, verbose = 0;
    const char *only = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--cases") && i + 1 < argc) ncases = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--budget") && i + 1 < argc) g_budget = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--selftest")) do_selftest = 1;
        else if (!strcmp(argv[i], "--jit-required")) jit_required = 1;
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--bug") && i + 1 < argc) g_bug = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--list")) {
            for (int k = 0; k < NHANDS; k++) printf("hand/%s\n", HANDS[k].name);
            return 0;
        } else { usage(); return !strcmp(argv[i], "--help") ? 0 : 2; }
    }

    /* The unstick monitor exists for a live guest with blocking syscalls; here
     * it would only be a thread watching a loop that never blocks. */
    setenv("OCERZ_NO_UNSTICK", "1", 1);

    if (!setup_memory())
        return 2;
    ocerz_vm_init(&g_vm);
    ocerz_vm_install_handlers(&g_vm);          /* also creates the JIT */
    if (!g_vm.jit) {
        fprintf(stderr, "diff32: the JIT could not be created; there is nothing to differ against\n");
        return 2;
    }
    uint64_t blocks0 = ocerz_jit_blocks(g_vm.jit);

    if (do_selftest) {
        int bad = selftest(seed);
        printf("----------------------------------------\n");
        printf("differential32 selftest: %d failed\n", bad);
        return bad ? 1 : 0;
    }

    long pass = 0, fail = 0, shown = 0;
    Case c;
    Snap si, sj, sw;

    for (int i = 0; i < NHANDS; i++) {
        gen_hand(&c, i, seed);
        if (only && !strstr(c.name, only)) continue;
        load_case(&c);
        tally(&c);
        run_side(&c, 0, &si);
        run_side(&c, 1, &sj);
        run_side(&c, 1, &sw);                  /* again, now against warm blocks */
        g_insns_executed += si.steps;
        const char *d = incomplete(&si);
        if (!d) d = diff_snaps(&si, &sj);
        if (!d) d = diff_snaps(&si, &sw);
        if (d) {
            if (!g_bug)
                dump_case(&c, d, seed);
            else
                printf("CAUGHT %-22s %s\n", c.name, d);
            fail++;
        } else {
            printf("PASS %-24s (%zu bytes, interp==jit, rc=%d)\n", c.name, c.len, si.rc);
            pass++;
        }
    }

    for (long i = 0; i < ncases; i++) {
        gen_random(&c, seed, (int)i);
        if (only && !strstr(c.name, only)) continue;
        load_case(&c);
        tally(&c);
        run_side(&c, 0, &si);
        run_side(&c, 1, &sj);
        run_side(&c, 1, &sw);
        g_insns_executed += si.steps;
        const char *d = incomplete(&si);
        if (!d) d = diff_snaps(&si, &sj);
        if (!d) d = diff_snaps(&si, &sw);
        if (d) {
            if (!g_bug && shown++ < 10)
                dump_case(&c, d, seed);
            fail++;
        } else {
            if (verbose)
                printf("PASS %-24s (%zu bytes, rc=%d)\n", c.name, c.len, si.rc);
            pass++;
        }
    }
    if (shown > 10)
        fprintf(stderr, "  ... %ld more random failures not shown\n", shown - 10);

    int nops = 0;
    for (int i = 0; i < OCERZ_OP_COUNT; i++)
        nops += g_opseen[i];
    uint64_t translated = ocerz_jit_blocks(g_vm.jit) - blocks0;
    printf("----------------------------------------\n");
    printf("differential32: %ld passed, %ld failed (%d hand-written + %ld random, seed %#llx)\n",
           pass, fail, NHANDS, ncases, (unsigned long long)seed);
    printf("differential32: corpus %llu instructions in %ld sequences, %d distinct opcodes;\n"
           "                %llu guest steps executed per side\n",
           g_insns_emitted, pass + fail, nops, g_insns_executed);
    if (g_bug) {
        printf("differential32: SENSITIVITY PROBE --bug %d (%s): the JIT side was made wrong\n"
               "                on purpose and %ld of %ld sequences (%.1f%%) caught it.\n",
               g_bug, BUGNAME[g_bug < 4 ? g_bug : 0], fail, pass + fail,
               100.0 * (double)fail / (double)(pass + fail ? pass + fail : 1));
        return 0;    /* the probe measures the corpus; failures are the point */
    }
    if (translated == 0)
        printf("differential32: CALIBRATION -- the JIT translated 0 blocks, so both sides\n"
               "                interpreted.  ocerz_jit_step() still returns OCERZ_EUNSUP for\n"
               "                cpu->mode32 (src/jit.c); this run proves the harness, not the\n"
               "                JIT.  Run --selftest for the comparator's own evidence.\n");
    else
        printf("differential32: the JIT translated %llu blocks -- this is a real differential\n",
               (unsigned long long)translated);
    if (jit_required && translated == 0) {
        printf("differential32: --jit-required and 0 blocks translated: FAIL\n");
        return 1;
    }
    return fail ? 1 : 0;
}


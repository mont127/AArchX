/*
 * diff32 -- the 32-bit JIT-vs-interpreter differential.
 *
 * tests/run_diff_test.sh is the 64-bit differential: it runs whole guest
 * binaries under -no-jit and under the JIT and compares the output.  There is
 * no 32-bit equivalent, because there is no i386 Mach-O to load, so the 32-bit
 * JIT would otherwise have landed with nothing able to catch a codegen bug.
 *
 * This builds 32-bit x86 instruction sequences in guest memory instead, runs
 * each one TWICE from the same initial architectural state - once with the JIT
 * off (pure interpreter) and once with it on, through the same dispatcher vm.c
 * uses - and compares the FULL architectural result: all 16 GPR slots at 64
 * bits wide, EFLAGS after materialization, EIP, the mode, the segment
 * selectors, the FP/SSE file, and every byte of the memory regions a sequence
 * is allowed to touch.
 *
 * Every sequence ends with the 32->64 edge from the WoW64 handshake -
 * `ff 2d <disp32>`, which in 32-bit mode is an ABSOLUTE far jump through an
 * m16:32 holding a flat 64-bit CS.  The driver stops the moment the mode goes
 * back to 64-bit, so the terminator is both the stop condition and a test of
 * the edge itself; a mode change is a dispatcher exit for any JIT, since a
 * translator only ever covers one mode.  Unreached code is filled with 0xf4
 * (HLT), which is fatal in the interpreter, so a sequence that runs off its end
 * is loud rather than silent.
 *
 * ---- why generated code cannot crash ----
 * A differential harness that crashes is worthless, so nothing here relies on
 * luck.  EBP is pinned to the middle of a mapped scratch region and is never a
 * destination - and in 32-bit mode the 8-bit register numbers are AL CL DL BL
 * AH CH DH BH, so no byte write can reach EBP either, which is precisely why
 * EBP rather than EBX is the pin and why AH/CH/DH/BH stay free.  ESP is never
 * written except by a bounded add/sub, with stack traffic depth-tracked so it
 * stays inside its mapped window.  Every memory operand is one of a fixed set
 * of forms that provably lands inside the scratch region, the stack window or
 * the low-64K window.  DIV/IDIV carry their own prologue so #DE is impossible.
 * Branches are self-contained - a template emits the branch AND the blob it
 * jumps over - and loops bring their own counter with a body that cannot write
 * it.  PUSHA/POPA and ENTER/LEAVE appear only as matched pairs, because a lone
 * POPA or LEAVE would load EBP or ESP from stack garbage.
 *
 * The memory-operand guards (an AND that masks an index, a MOV that loads a
 * base) are emitted before any prefix or opcode byte of the instruction that
 * will use the operand, and they are ordinary i386 instructions, so they are
 * themselves under test.
 *
 * ---- keeping the gate honest ----
 * The JIT really does compile 32-bit blocks now, so the two sides are two
 * different engines, and run_diff32.sh passes --jit-required: "the JIT
 * translated 0 blocks" is a FAILURE, because a future change that silently
 * went back to declining 32-bit blocks would otherwise turn this gate into a
 * second interpreter run that passes 100%.  It also fails when fewer blocks
 * were translated than sequences were run.  A translated block arms the page
 * it came from (src/mem.c), so rewriting the code region for the next case
 * faulted into the self-modifying-code path, which retires the blocks and
 * counts the region toward the churn limit, and three cases later the region
 * ran interpreted: from 2026-09-07 the 20000-case gate translated 516 blocks
 * where it had translated 50970, and still passed.  load_case therefore
 * retires every block and disarms before it writes.  The code arena is 2 MB,
 * because retiring everything synchronises the instruction cache over all the
 * code emitted since the last flush, which made a run quadratic in its length
 * (117 s for the default gate in a 1 GB arena, 15 s in this one).
 * --selftest injects a deliberate
 * one-field corruption into the JIT-side result for each class of compared
 * state and requires the comparator to catch every one, so a green run cannot
 * be a comparator that compares nothing.  --bug N answers the other half by
 * making the JIT side deliberately wrong in one realistic way.  The RNG is
 * splitmix64 so that a seed plus a case index reproduces a failing sequence
 * exactly, which is the whole point of seeding it.
 *
 * ---- memory layouts ----
 * By default guest memory is an offset arena: a guest address plus
 * ocerz_guest_base is the host address.  A WoW64 process is not laid out that
 * way.  Wine runs with an identity arena and the low shadow window, so every
 * 32-bit address lies below 12 GB and reaches the host through the window's
 * translation, the guarded path in src/jit.c rather than the base add.  --low
 * lays memory out the way Wine does, and run_diff32.sh runs the corpus in both
 * layouts.
 *
 * --bench is not part of the gate.  It times a few loops of the shapes 32-bit
 * Windows code is made of - an SEH frame pushed and popped through fs:[0],
 * TEB and TLS reads, the interlocked operations, bit tests, arithmetic on
 * memory, x87 - under the JIT in either layout, which is how a slow call is
 * measured against the inlined form that replaces it.
 *
 * ---- what it found ----
 * Pointed at a real 32-bit JIT for the first time, this gate caught: emit_lea()
 * truncating an effective address at 4 GB with no case for 16-bit addressing;
 * flags_live.c declaring DIV/IDIV to define every flag while both the
 * interpreter and the emitter leave the record alone; the i386-only opcodes,
 * appended at the end of OcerzOp, falling inside the ">= X87_FIRST is
 * flag-neutral" catch-all so DAA/DAS/AAA/AAS were declared to read no flags;
 * the CL-count shift and rotate emitters writing the destination
 * unconditionally where the interpreter writes nothing when the count masks to
 * zero; SHLD/SHRD declared a definite flag define rather than a may-define; a
 * liveness pass predicting a fusion the emitter would not make; and
 * fuse_prev_mov() miscompiling `mov A,B` + `op A,A` - in 64-bit blocks too.
 *
 * ---- x87 ----
 * The x87 families start from a register file the case chooses rather than
 * from reset: a random TOP, one to eight values, exact, stale and
 * contradicting 80-bit images, a status word with and without PE, control
 * words with precision 24 and directed rounding, and MXCSR values that move
 * the host's rounding.  Their operands come from a table of values chosen to
 * leave the translated fast path (NaNs, infinities, denormals, range edges,
 * integers above 2^53, float ties), and the comparison covers every fpr bit
 * for bit, the images and their validity bits, the tag word, TOP, fsw, fcw and
 * MXCSR.  Sequences mix every translated form with untranslated ones, integer
 * filler, fnstsw/sahf and fcomi/fcmov/setcc/jcc consumers, so translated runs
 * start, end and fall back to the interpreter at every point.
 *
 * If a new 32-bit instruction becomes JIT-able, add it to the template table,
 * and give it its own template if its encoding means something different in
 * 32-bit mode than in 64-bit.
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
#include "ocerz/x87.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define ARENA_LO     0x00000000ull
#define ARENA_HI     0x00000000c0000000ull

#define LOW_CODE     0x00004000ull
#define LOW_CODE_LEN 0x00001000ull
#define LOW16        0x00008000ull
#define LOW16_LEN    0x00001000ull

#define CODE64       0x00200000ull
#define CODE64_LEN   0x00001000ull
#define HALT64       CODE64

#define CODE32       0x00204000ull
#define CODE32_LEN   0x00008000ull

#define FARPTR       0x0020c000ull
#define FARPTR_LEN   0x00004000ull

#define SCRATCH      0x00210000ull
#define SCRATCH_LEN  0x00002000ull
#define SCRATCH_MID  (SCRATCH + 0x1000)
#define TEB          (SCRATCH_MID + 0x600)

#define STACK_LO     0x00220000ull
#define STACK_LEN    0x00020000ull
#define ESP0         (STACK_LO + 0x10000)
#define STACK_CMP_LO (ESP0 - 0x1000)
#define STACK_CMP_LEN 0x2000ull

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

static uint64_t sm64(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

#define CODE_MAX 2048
#define NPLANT   8

typedef struct {
    char     name[96];
    uint8_t  code[CODE_MAX];
    size_t   len;
    uint64_t gpr[16];
    uint64_t rflags;
    uint64_t memseed;
    uint64_t fs_base, gs_base;
    struct { uint64_t addr; uint8_t bytes[40]; size_t len; } plant[NPLANT];
    int      nplant;
    int      x87;
    uint16_t fcw, fsw;
    uint8_t  ftw, ftop, x_ok;
    uint64_t fpr[8], xm[8];
    uint16_t xe[8];
    uint32_t mxcsr;
} Case;

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

static const uint8_t DST[6] = { 0, 1, 2, 3, 6, 7 };
static uint8_t rdst(Gen *g) { return DST[rndn(g, 6)]; }
static uint8_t rany(Gen *g) { return (uint8_t)rndn(g, 8); }

typedef struct {
    int      a16;
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
    case 0: case 1:
        mf_abs32(m, (uint32_t)(SCRATCH_MID + rndn(g, 0x400)));
        return;
    case 2:
        mf_ebp8(m, (int8_t)rndi(g, -120, 120));
        return;
    case 3:
        memset(m, 0, sizeof *m);
        m->mod = 2; m->rm = 5; m->dispn = 4;
        m->disp = (uint32_t)(int32_t)rndi(g, -0x400, 0x400);
        return;
    case 4:
        idx = DST[rndn(g, 6)];
        eb(g, 0x83); eb(g, 0xe0 | idx); eb(g, 0x1f);
        memset(m, 0, sizeof *m);
        m->mod = 1; m->rm = 4; m->has_sib = 1;
        m->sib = (uint8_t)((rndn(g, 4) << 6) | (idx << 3) | 5);
        m->disp = (uint32_t)(int32_t)rndi(g, -64, 64); m->dispn = 1;
        return;
    case 5:
        memset(m, 0, sizeof *m);
        m->mod = 1; m->rm = 4; m->has_sib = 1;
        m->sib = (uint8_t)((4 << 3) | 4);
        m->disp = (uint32_t)(int32_t)rndi(g, 0, 120); m->dispn = 1;
        return;
    case 6:
        base = DST[rndn(g, 6)];
        eb(g, 0xb8 | base); ed(g, (uint32_t)(SCRATCH_MID + rndn(g, 0x400)));
        memset(m, 0, sizeof *m);
        if (rndn(g, 2)) { m->mod = 0; m->rm = base; m->dispn = 0; }
        else { m->mod = 1; m->rm = base; m->disp = (uint32_t)(int32_t)rndi(g, -64, 64); m->dispn = 1; }
        return;
    case 7:
        idx = DST[rndn(g, 6)];
        eb(g, 0x83); eb(g, 0xe0 | idx); eb(g, 0x1f);
        memset(m, 0, sizeof *m);
        m->mod = 0; m->rm = 4; m->has_sib = 1;
        m->sib = (uint8_t)((rndn(g, 4) << 6) | (idx << 3) | 5);
        m->disp = (uint32_t)SCRATCH_MID; m->dispn = 4;
        return;
    case 8:
        memset(m, 0, sizeof *m);
        m->a16 = 1; m->mod = 0; m->rm = 6;
        m->disp = (uint32_t)(LOW16 + rndn(g, 0x100)); m->dispn = 2;
        return;
    default: {
        static const uint8_t rm16[4] = { 0, 1, 4, 5 };
        uint8_t r = rm16[rndn(g, 4)];
        eb(g, 0x66); eb(g, 0xbb); ew(g, (unsigned)(LOW16 + rndn(g, 0x80)));
        eb(g, 0x66); eb(g, 0xbe); ew(g, rndn(g, 0x40));
        eb(g, 0x66); eb(g, 0xbf); ew(g, rndn(g, 0x40));
        memset(m, 0, sizeof *m);
        m->a16 = 1; m->mod = 1; m->rm = r;
        m->disp = (uint32_t)(int32_t)rndi(g, 0, 60); m->dispn = 1;
        if (r == 4 || r == 5) {
            m->mod = 2; m->dispn = 2;
            m->disp = (uint32_t)(LOW16 + rndn(g, 0x40));
        }
        return;
    }
    }
}

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

static void emit_prefixes(Gen *g, int opsize16, const MF *m, int allow_seg)
{
    if (allow_seg && rndn(g, 16) == 0) {
        static const uint8_t seg[6] = { 0x2e, 0x36, 0x3e, 0x26, 0x64, 0x65 };
        eb(g, seg[rndn(g, 6)]);
    }
    if (opsize16)
        eb(g, 0x66);
    if (m && m->a16)
        eb(g, 0x67);
}

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

static uint8_t rbyte_avoid(Gen *g)
{
    uint8_t br = (uint8_t)rndn(g, 8);
    if (g_avoid != 0xff && (br & 3) == (uint8_t)g_avoid)
        br = (uint8_t)((br & 4) | (((unsigned)g_avoid + 1) & 3));
    return br;
}

static void blob(Gen *g, int n)
{
    for (int i = 0; i < n && room(g) > 12; i++) {
        uint8_t d = rdst_avoid(g), s = (uint8_t)rndn(g, 8);
        switch (rndn(g, 10)) {
        case 0: eb(g, 0xb8 | d); ed(g, rnd(g)); break;
        case 1: eb(g, 0x01); eb(g, 0xc0 | (s << 3) | d); break;
        case 2: eb(g, 0x31); eb(g, 0xc0 | (s << 3) | d); break;
        case 3: eb(g, 0x40 | d); break;
        case 4: eb(g, 0x48 | d); break;
        case 5: eb(g, 0xc1); eb(g, 0xe0 | d); eb(g, rndn(g, 40)); break;
        case 6: eb(g, 0xf7); eb(g, 0xd0 | d); break;
        case 7: eb(g, 0x0f); eb(g, 0xb6); eb(g, 0xc0 | (d << 3) | rndn(g, 8)); break;
        case 8:
            eb(g, 0x0f); eb(g, 0x90 | rndn(g, 16)); eb(g, 0xc0 | rbyte_avoid(g));
            break;
        default: eb(g, 0x90); break;
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

    if (form >= 4) {
        emit_prefixes(g, op16, NULL, 0);
        eb(g, a + form);
        if (sz8) eb(g, rnd(g));
        else if (op16) ew(g, rnd(g));
        else ed(g, rnd(g));
        return;
    }
    int dir = (form == 2 || form == 3);
    int use_mem = rndn(g, 2);
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
    unsigned kind = rndn(g, 3);
    int sz8 = (kind == 0);
    int op16 = !sz8 && rndn(g, 4) == 0;
    int use_mem = rndn(g, 2);
    int lock = use_mem && n != 7 && rndn(g, 8) == 0;
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
    case 0:
        if (rndn(g, 4) == 0) eb(g, 0x66);
        eb(g, (rndn(g, 2) ? 0x40 : 0x48) | rdst(g));
        return;
    case 1: {
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
    default: {
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
    case 0: case 1: {
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
    case 2:
        eb(g, 0xb0 | rany(g)); eb(g, rnd(g));
        return;
    case 3: {
        int op16 = rndn(g, 4) == 0;
        if (op16) eb(g, 0x66);
        eb(g, 0xb8 | rdst(g));
        if (op16) ew(g, rnd(g)); else ed(g, rnd(g));
        return;
    }
    case 4: case 5: {
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
    default: {
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
    emit_prefixes(g, rndn(g, 4) == 0, &m, 0);
    eb(g, 0x8d);
    emit_modrm(g, rdst(g), &m);
}

static void t_test(Gen *g)
{
    switch (rndn(g, 4)) {
    case 0: {
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
    case 1:
        if (rndn(g, 2)) { eb(g, 0xa8); eb(g, rnd(g)); }
        else { eb(g, 0xa9); ed(g, rnd(g)); }
        return;
    default: {
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
    if (rndn(g, 3) == 0) {
        if (rndn(g, 4) == 0) eb(g, 0x66);
        eb(g, 0x90 | rdst(g));
        return;
    }
    int sz8 = rndn(g, 2);
    int op16 = !sz8 && rndn(g, 4) == 0;
    int use_mem = rndn(g, 2);
    MF m;
    if (use_mem) pick_mem_aligned(g, &m, sz8 ? 1 : (op16 ? 2 : 4));
    unsigned reg = sz8 ? rany(g) : rdst(g);
    emit_prefixes(g, op16, use_mem ? &m : NULL, 1);
    eb(g, sz8 ? 0x86 : 0x87);
    if (use_mem) emit_modrm(g, reg, &m);
    else eb(g, 0xc0 | (reg << 3) | (sz8 ? rany(g) : rdst(g)));
}

static void t_shift(Gen *g)
{
    unsigned n = rndn(g, 8);
    unsigned kind = rndn(g, 3);
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
        eb(g, rndn(g, 40));
}

static void t_grp3(Gen *g)
{
    unsigned n = rndn(g, 6) + 2;
    if (n >= 6) {
        unsigned sz = rndn(g, 3);
        unsigned d = (unsigned)rndi(g, 1, 0x7f);
        if (sz == 0) {
            if (n == 6) { eb(g, 0xb4); eb(g, 0x00); }
            else        { eb(g, 0x66); eb(g, 0x98); }
            eb(g, 0xb1); eb(g, d);
            eb(g, 0xf6); eb(g, 0xc0 | (n << 3) | 1);
        } else if (sz == 1) {
            eb(g, 0x66); eb(g, n == 6 ? 0x31 : 0x99);
            if (n == 6) eb(g, 0xd2);
            eb(g, 0x66); eb(g, 0xb9); ew(g, d);
            eb(g, 0x66); eb(g, 0xf7); eb(g, 0xc0 | (n << 3) | 1);
        } else {
            if (n == 6) { eb(g, 0x31); eb(g, 0xd2); }
            else        { eb(g, 0x99); }
            eb(g, 0xb9); ed(g, d);
            eb(g, 0xf7); eb(g, 0xc0 | (n << 3) | 1);
        }
        return;
    }
    int sz8 = rndn(g, 2);
    int op16 = !sz8 && rndn(g, 4) == 0;
    int use_mem = rndn(g, 2);
    int lock = use_mem && n <= 3 && rndn(g, 8) == 0;
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
        0x98, 0x99,
        0x27, 0x2f, 0x37, 0x3f, 0xd6,
        0xf5, 0xf8, 0xf9, 0xfc, 0xfd, 0x9e,
    };
    switch (rndn(g, 6)) {
    case 0: eb(g, 0x66); eb(g, rndn(g, 2) ? 0x98 : 0x99); return;
    case 1: eb(g, 0xd4); eb(g, (unsigned)rndi(g, 1, 255)); return;
    case 2: eb(g, 0xd5); eb(g, rnd(g)); return;
    case 3: eb(g, 0x9f); return;
    default: eb(g, one[rndn(g, 13)]); return;
    }
}

#define DEPTH_MAX 512

static void t_stack(Gen *g)
{
    int op16 = rndn(g, 4) == 0;
    int slot = op16 ? 2 : 4;
    switch (rndn(g, 8)) {
    case 0:
        if (g->depth + slot > DEPTH_MAX) return;
        if (op16) eb(g, 0x66);
        eb(g, 0x50 | rany(g));
        g->depth += slot;
        return;
    case 1:
        if (g->depth - slot < -DEPTH_MAX) return;
        if (op16) eb(g, 0x66);
        eb(g, 0x58 | rdst(g));
        g->depth -= slot;
        return;
    case 2:
        if (g->depth + slot > DEPTH_MAX) return;
        if (op16) eb(g, 0x66);
        eb(g, 0x6a); eb(g, rnd(g));
        g->depth += slot;
        return;
    case 3:
        if (g->depth + slot > DEPTH_MAX) return;
        if (op16) eb(g, 0x66);
        eb(g, 0x68);
        if (op16) ew(g, rnd(g)); else ed(g, rnd(g));
        g->depth += slot;
        return;
    case 4: {
        int pop = rndn(g, 2);
        if (pop ? (g->depth - slot < -DEPTH_MAX) : (g->depth + slot > DEPTH_MAX))
            return;
        MF m;
        pick_mem(g, &m, 0);
        emit_prefixes(g, op16, &m, 1);
        eb(g, pop ? 0x8f : 0xff);
        emit_modrm(g, pop ? 0 : 6, &m);
        g->depth += pop ? -slot : slot;
        return;
    }
    case 5:
        if (g->depth + slot > DEPTH_MAX) return;
        if (op16) eb(g, 0x66);
        eb(g, 0x9c);
        if (op16) eb(g, 0x66);
        eb(g, 0x9d);
        return;
    case 6:
        if (g->depth + 4 > DEPTH_MAX) return;
        eb(g, 0x68); ed(g, (rnd(g) & 0x00000cd5u) | 2u);
        eb(g, 0x9d);
        return;
    default: {
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

static void t_enter(Gen *g)
{
    unsigned frame = (unsigned)(rndi(g, 0, 64) & ~3);
    if (g->depth + 4 + (int)frame > DEPTH_MAX || room(g) < 80)
        return;
    eb(g, 0x55);
    eb(g, 0x89); eb(g, 0xe5);
    if (frame) { eb(g, 0x83); eb(g, 0xec); eb(g, frame); }
    blob(g, rndi(g, 2, 5));
    eb(g, 0xc9);
}

static void t_jcc(Gen *g)
{
    if (room(g) < 48) return;
    eb(g, rndn(g, 2) ? 0x39 : 0x85);
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
    eb(g, 0xb9); ed(g, n);
    size_t top = g->c->len;
    int save = g_avoid;
    g_avoid = 1;
    blob(g, rndi(g, 1, 3));
    g_avoid = save;
    if (rndn(g, 4) == 0) eb(g, 0x67);
    eb(g, 0xe0 + rndn(g, 3));
    int rel = (int)((int64_t)top - (int64_t)(g->c->len + 1));
    eb(g, (unsigned)(uint8_t)(int8_t)rel);
}

static void t_jecxz(Gen *g)
{
    if (room(g) < 40) return;
    if (rndn(g, 2)) { eb(g, 0x31); eb(g, 0xc9); }
    else { eb(g, 0xb9); ed(g, rnd(g) | 1u); }
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
    eb(g, 0xeb); jat = g->c->len; eb(g, 0);
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

static void t_string(Gen *g)
{
    if (room(g) < 48) return;
    static const uint8_t ops[10] = { 0xa4, 0xa5, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xa6, 0xa7 };
    unsigned k = rndn(g, 10);
    unsigned opc = ops[k];
    int cmp_or_scas = (opc >= 0xa6 && opc <= 0xa7) || (opc >= 0xae && opc <= 0xaf);
    int a16 = rndn(g, 6) == 0;
    uint64_t src = a16 ? (LOW16 + 0x100) : (SCRATCH_MID + 0x80);
    uint64_t dst = a16 ? (LOW16 + 0x200) : (SCRATCH_MID + 0x200);
    eb(g, rndn(g, 4) == 0 ? 0xfd : 0xfc);
    eb(g, 0xb9); ed(g, (uint32_t)rndi(g, 1, 8));
    eb(g, 0xbe); ed(g, (uint32_t)(src + rndn(g, 0x40)));
    eb(g, 0xbf); ed(g, (uint32_t)(dst + rndn(g, 0x40)));
    unsigned rep = rndn(g, 3);
    if (rep == 1) eb(g, 0xf3);
    else if (rep == 2) eb(g, cmp_or_scas ? 0xf2 : 0xf3);
    if ((opc & 1) && rndn(g, 3) == 0) eb(g, 0x66);
    if (a16) eb(g, 0x67);
    eb(g, opc);
}

static void t_0f(Gen *g)
{
    int op16 = rndn(g, 5) == 0;
    switch (rndn(g, 9)) {
    case 0: {
        int use_mem = rndn(g, 2);
        MF m;
        if (use_mem) pick_mem(g, &m, 1);
        emit_prefixes(g, 0, use_mem ? &m : NULL, 1);
        eb(g, 0x0f); eb(g, 0x90 | rndn(g, 16));
        if (use_mem) emit_modrm(g, 0, &m);
        else eb(g, 0xc0 | rany(g));
        return;
    }
    case 1: {
        int use_mem = rndn(g, 2);
        MF m;
        if (use_mem) pick_mem(g, &m, 1);
        emit_prefixes(g, op16, use_mem ? &m : NULL, 1);
        eb(g, 0x0f); eb(g, 0x40 | rndn(g, 16));
        if (use_mem) emit_modrm(g, rdst(g), &m);
        else eb(g, 0xc0 | (rdst(g) << 3) | rany(g));
        return;
    }
    case 2: {
        static const uint8_t bt[4] = { 0xa3, 0xab, 0xb3, 0xbb };
        emit_prefixes(g, op16, NULL, 0);
        eb(g, 0x0f); eb(g, bt[rndn(g, 4)]);
        eb(g, 0xc0 | (rany(g) << 3) | rdst(g));
        return;
    }
    case 3: {
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
    case 4: {
        int use_mem = rndn(g, 2);
        MF m;
        if (use_mem) pick_mem(g, &m, 1);
        emit_prefixes(g, op16, use_mem ? &m : NULL, 1);
        eb(g, 0x0f); eb(g, rndn(g, 2) ? 0xbc : 0xbd);
        if (use_mem) emit_modrm(g, rdst(g), &m);
        else eb(g, 0xc0 | (rdst(g) << 3) | rany(g));
        return;
    }
    case 5: {
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
    case 6:
        eb(g, 0x0f); eb(g, 0xc8 | rdst(g));
        return;
    case 7: {
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
    default: {
        int sz8 = rndn(g, 2);
        int xadd = rndn(g, 2);
        int use_mem = rndn(g, 2);
        MF m;
        if (use_mem) pick_mem_aligned(g, &m, sz8 ? 1 : (op16 ? 2 : 4));
        unsigned reg = sz8 ? rany(g) : rdst(g);
        if (use_mem && rndn(g, 4) == 0) eb(g, 0xf0);
        emit_prefixes(g, sz8 ? 0 : op16, use_mem ? &m : NULL, 1);
        eb(g, 0x0f); eb(g, (xadd ? 0xc0 : 0xb0) + (sz8 ? 0 : 1));
        if (use_mem) emit_modrm(g, reg, &m);
        else eb(g, 0xc0 | (reg << 3) | (sz8 ? rany(g) : rdst(g)));
        return;
    }
    }
}


static void jcc_over_blob(Gen *g, unsigned cc)
{
    int rel32 = rndn(g, 2);
    size_t at;
    if (rel32) { eb(g, 0x0f); eb(g, 0x80 | cc); at = g->c->len; ed(g, 0); }
    else       { eb(g, 0x70 | cc);              at = g->c->len; eb(g, 0); }
    size_t after = g->c->len;
    blob(g, rndi(g, 1, 4));
    g->c->code[at] = (uint8_t)(g->c->len - after);
}

static void imm_for(Gen *g, int sz8, int op16, int wide, int is_test)
{
    uint32_t v = rnd(g);
    switch (rndn(g, 4)) {
    case 0: v = 0; break;
    case 1: if (is_test) v = 1u << rndn(g, sz8 ? 8 : op16 ? 16 : 32); else v &= 0xff; break;
    default: break;
    }
    if (!wide) eb(g, v);
    else if (op16) ew(g, v);
    else ed(g, v);
}

static void t_cmpjcc(Gen *g)
{
    if (room(g) < 72) return;
    MF m;
    unsigned sz = rndn(g, 4);
    int sz8 = sz == 0, op16 = sz == 1;
    int cmp = rndn(g, 2);
    switch (rndn(g, 5)) {
    case 0:
        if (op16) eb(g, 0x66);
        eb(g, (cmp ? 0x38 : 0x84) + (sz8 ? 0 : 1));
        eb(g, 0xc0 | (rany(g) << 3) | rany(g));
        break;
    case 1:
        if (op16) eb(g, 0x66);
        if (cmp) {
            unsigned k = sz8 ? 0x80 : (rndn(g, 2) ? 0x81 : 0x83);
            eb(g, k); eb(g, 0xc0 | (7 << 3) | rany(g));
            imm_for(g, sz8, op16, k == 0x81, 0);
        } else {
            eb(g, sz8 ? 0xf6 : 0xf7); eb(g, 0xc0 | rany(g));
            imm_for(g, sz8, op16, !sz8, 1);
        }
        break;
    case 2:
        pick_mem(g, &m, 0);
        emit_prefixes(g, op16, &m, 1);
        eb(g, cmp ? (sz8 ? 0x3a : 0x3b) : (sz8 ? 0x84 : 0x85));
        emit_modrm(g, rany(g), &m);
        break;
    case 3:
        pick_mem(g, &m, 0);
        emit_prefixes(g, op16, &m, 1);
        eb(g, (cmp ? 0x38 : 0x84) + (sz8 ? 0 : 1));
        emit_modrm(g, rany(g), &m);
        break;
    default:
        pick_mem(g, &m, 0);
        emit_prefixes(g, op16, &m, 1);
        if (cmp) {
            unsigned k = sz8 ? 0x80 : (rndn(g, 2) ? 0x81 : 0x83);
            eb(g, k); emit_modrm(g, 7, &m);
            imm_for(g, sz8, op16, k == 0x81, 0);
        } else {
            eb(g, sz8 ? 0xf6 : 0xf7); emit_modrm(g, 0, &m);
            imm_for(g, sz8, op16, !sz8, 1);
        }
        break;
    }
    if (rndn(g, 4) == 0) {
        uint8_t d = rdst(g);
        if (rndn(g, 2)) { eb(g, 0xb8 | d); ed(g, rnd(g)); }
        else { eb(g, 0x89); eb(g, 0xc0 | (rany(g) << 3) | d); }
    }
    jcc_over_blob(g, rndn(g, 16));
}

static void t_ccuse(Gen *g)
{
    if (room(g) < 80) return;
    static const uint8_t alu[7] = { 0x38, 0x84, 0x00, 0x28, 0x20, 0x08, 0x30 };
    uint8_t d = rdst(g);
    unsigned sz = rndn(g, 5);
    int sz8 = sz == 0, op16 = sz == 1;
    switch (rndn(g, 5)) {
    case 0: case 1: {
        unsigned a = alu[rndn(g, 7)];
        if (op16) eb(g, 0x66);
        eb(g, a + (sz8 ? 0 : 1));
        eb(g, 0xc0 | (rany(g) << 3) | (sz8 ? rany(g) : (a == 0x38 || a == 0x84 ? rany(g) : d)));
        break;
    }
    case 2:
        if (op16) eb(g, 0x66);
        if (sz8) { eb(g, 0x80); eb(g, 0xc0 | (rndn(g, 8) << 3) | rany(g)); eb(g, rnd(g)); }
        else { eb(g, 0x83); eb(g, 0xc0 | (rndn(g, 8) << 3) | d); eb(g, rndn(g, 3) ? rnd(g) : 0); }
        break;
    case 3:
        if (op16) eb(g, 0x66);
        switch (rndn(g, 4)) {
        case 0: eb(g, 0x40 | d); break;
        case 1: eb(g, 0x48 | d); break;
        case 2: eb(g, 0xf7); eb(g, 0xd8 | d); break;
        default: eb(g, 0xc1); eb(g, 0xc0 | ((4 + rndn(g, 2) * (rndn(g, 2) ? 1 : 3)) << 3) | d); eb(g, rndn(g, 40)); break;
        }
        break;
    default: {
        static const uint8_t pick[4] = { 0x2e, 0x2f, 0x2e, 0x2f };
        unsigned x = rndn(g, 8), y = rndn(g, 8);
        uint32_t a = (uint32_t)(SCRATCH_MID + rndn(g, 0x100) * 8);
        if (rndn(g, 2)) eb(g, 0x66);
        eb(g, 0x0f); eb(g, 0x10); eb(g, (x << 3) | 5); ed(g, a);
        if (rndn(g, 2)) { eb(g, 0x0f); eb(g, 0x10); eb(g, (y << 3) | 5); ed(g, a + 8); }
        if (rndn(g, 2)) eb(g, 0x66);
        eb(g, 0x0f); eb(g, pick[rndn(g, 4)]); eb(g, 0xc0 | (x << 3) | y);
        break;
    }
    }
    int gaps = (int)rndn(g, 3);
    for (int i = 0; i < gaps; i++) {
        uint8_t r = rdst(g);
        switch (rndn(g, 4)) {
        case 0: eb(g, 0xb8 | r); ed(g, rnd(g)); break;
        case 1: eb(g, 0x89); eb(g, 0xc0 | (rany(g) << 3) | r); break;
        case 2: eb(g, 0x0f); eb(g, 0x90 | rndn(g, 16)); eb(g, 0xc0 | rbyte_avoid(g)); break;
        default: eb(g, 0x0f); eb(g, 0x40 | rndn(g, 16)); eb(g, 0xc0 | (r << 3) | rany(g)); break;
        }
    }
    uint8_t r = rdst(g);
    switch (rndn(g, 5)) {
    case 0: eb(g, 0x0f); eb(g, 0x90 | rndn(g, 16)); eb(g, 0xc0 | rbyte_avoid(g)); break;
    case 1: eb(g, 0x0f); eb(g, 0x40 | rndn(g, 16)); eb(g, 0xc0 | (r << 3) | rany(g)); break;
    case 2: {
        MF m;
        pick_mem(g, &m, 0);
        emit_prefixes(g, 0, &m, 1);
        eb(g, 0x0f); eb(g, 0x40 | rndn(g, 16)); emit_modrm(g, rdst(g), &m);
        break;
    }
    case 3:
        if (rndn(g, 2)) { eb(g, rndn(g, 2) ? 0x11 : 0x19); eb(g, 0xc0 | (rany(g) << 3) | r); }
        else { eb(g, 0x83); eb(g, 0xc0 | ((rndn(g, 2) ? 2 : 3) << 3) | r); eb(g, rnd(g)); }
        break;
    default:
        jcc_over_blob(g, rndn(g, 16));
        break;
    }
}

static void t_pairs(Gen *g)
{
    if (room(g) < 40) return;
    uint8_t d = rdst(g);
    if (rndn(g, 2)) {
        static const uint8_t lg[3] = { 0x21, 0x09, 0x31 };
        eb(g, 0x89); eb(g, 0xc0 | (rany(g) << 3) | d);
        if (rndn(g, 2)) { eb(g, lg[rndn(g, 3)]); eb(g, 0xc0 | (rany(g) << 3) | d); }
        else { eb(g, 0x81); eb(g, 0xc0 | ((rndn(g, 3) == 0 ? 4 : rndn(g, 2) ? 1 : 6) << 3) | d); ed(g, rnd(g)); }
        eb(g, rndn(g, 2) ? 0x39 : 0x85); eb(g, 0xc0 | (rany(g) << 3) | rany(g));
    } else {
        if (rndn(g, 2)) { eb(g, 0x01); eb(g, 0xc0 | (rany(g) << 3) | d); }
        else { eb(g, 0x83); eb(g, 0xc0 | d); eb(g, rnd(g)); }
        eb(g, 0x40 | d);
        uint8_t r = rdst(g);
        switch (rndn(g, 3)) {
        case 0: eb(g, 0x11); eb(g, 0xc0 | (rany(g) << 3) | r); break;
        case 1: eb(g, 0x0f); eb(g, 0x92); eb(g, 0xc0 | rbyte_avoid(g)); break;
        default: jcc_over_blob(g, 2 + rndn(g, 2)); break;
        }
    }
}

static void t_movshift(Gen *g)
{
    if (room(g) < 40) return;
    uint8_t d = rdst(g), sr = rdst(g), r = rdst(g);
    while (sr == d) sr = rdst(g);
    while (r == d || r == sr) r = rdst(g);
    eb(g, 0x89); eb(g, 0xc0 | (sr << 3) | d);
    int gaps = rndi(g, 1, 2);
    for (int i = 0; i < gaps; i++) {
        if (rndn(g, 2)) { eb(g, 0xb8 | r); ed(g, rnd(g)); }
        else { eb(g, rndn(g, 2) ? 0x01 : 0x31); eb(g, 0xc0 | ((rndn(g, 2) ? sr : r) << 3) | r); }
    }
    static const uint8_t sh[3] = { 4, 5, 7 };
    eb(g, 0xc1); eb(g, 0xc0 | (sh[rndn(g, 3)] << 3) | d); eb(g, (uint8_t)rndi(g, 1, 31));
    if (rndn(g, 4)) { eb(g, rndn(g, 2) ? 0x39 : 0x85); eb(g, 0xc0 | (rany(g) << 3) | rany(g)); }
}

static void t_cntloop(Gen *g)
{
    if (room(g) < 72) return;
    uint8_t r = rdst(g);
    unsigned n = (unsigned)rndi(g, 1, 5);
    int save = g_avoid;
    unsigned kind = rndn(g, 4);
    int r16 = kind < 2 && rndn(g, 6) == 0;
    eb(g, 0xb8 | r); ed(g, kind == 1 ? (uint32_t)-(int32_t)n : (kind == 2 ? 0 : n));
    size_t top = g->c->len;
    g_avoid = r;
    if (kind == 3) {
        eb(g, 0x48 | r);
        eb(g, 0x74); size_t at = g->c->len; eb(g, 0);
        blob(g, rndi(g, 1, 3));
        eb(g, 0xeb); eb(g, (uint8_t)(int8_t)((int64_t)top - (int64_t)(g->c->len + 1)));
        g->c->code[at] = (uint8_t)(g->c->len - (at + 1));
        g_avoid = save;
        return;
    }
    blob(g, rndi(g, 1, 3));
    g_avoid = save;
    if (r16) eb(g, 0x66);
    if (kind == 0) eb(g, 0x48 | r);
    else eb(g, 0x40 | r);
    if (kind == 2) { eb(g, 0x83); eb(g, 0xf8 | r); eb(g, n); }
    unsigned cc = kind == 2 ? (rndn(g, 2) ? 0x5 : 0x2) : 0x5;
    if (rndn(g, 2)) {
        eb(g, 0x70 | cc);
        eb(g, (uint8_t)(int8_t)((int64_t)top - (int64_t)(g->c->len + 1)));
    } else {
        eb(g, 0x0f); eb(g, 0x80 | cc);
        ed(g, (uint32_t)((int64_t)top - (int64_t)(g->c->len + 4)));
    }
}

static void t_btmem(Gen *g)
{
    static const uint8_t bt[4] = { 0xa3, 0xab, 0xb3, 0xbb };
    MF m;
    uint8_t r = rdst(g);
    int op16 = rndn(g, 4) == 0;
    unsigned k = rndn(g, 4);
    eb(g, 0xb8 | r); ed(g, rndn(g, 0x400));
    eb(g, 0x81); eb(g, 0xe8 | r); ed(g, 0x200);
    if (rndn(g, 2)) mf_abs32(&m, (uint32_t)(SCRATCH_MID + rndn(g, 0x400)));
    else mf_ebp8(&m, (int8_t)rndi(g, -120, 120));
    if (k && rndn(g, 4) == 0) eb(g, 0xf0);
    emit_prefixes(g, op16, &m, 1);
    eb(g, 0x0f); eb(g, bt[k]);
    emit_modrm(g, r, &m);
}

static void t_cx8(Gen *g)
{
    MF m;
    if (rndn(g, 3) == 0) {
        uint32_t a = (uint32_t)(SCRATCH_MID + rndn(g, 0x80) * 8 + (rndn(g, 4) == 0 ? rndn(g, 8) : 0));
        eb(g, 0xa1); ed(g, a);
        eb(g, 0x8b); eb(g, 0x15); ed(g, a + 4);
        if (rndn(g, 3) == 0) eb(g, 0x40 | (rndn(g, 2) ? 0 : 2));
        mf_abs32(&m, a);
    } else if (rndn(g, 2)) {
        pick_mem_aligned(g, &m, 8);
    } else {
        pick_mem(g, &m, 0);
    }
    if (rndn(g, 4)) eb(g, 0xf0);
    emit_prefixes(g, 0, &m, 1);
    eb(g, 0x0f); eb(g, 0xc7);
    emit_modrm(g, 1, &m);
}

static void t_seg(Gen *g)
{
    static const uint8_t alu[8] = { 0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38 };
    unsigned seg = rndn(g, 3) ? 0x64 : 0x65;
    int sz8 = rndn(g, 4) == 0;
    int op16 = !sz8 && rndn(g, 4) == 0;
    int asz = sz8 ? 1 : (op16 ? 2 : 4);
    MF m;
    switch (rndn(g, 9)) {
    case 0: case 1: {
        int dir = rndn(g, 2);
        pick_mem(g, &m, 0);
        eb(g, seg); emit_prefixes(g, op16, &m, 0);
        eb(g, 0x88 + (dir ? 2 : 0) + (sz8 ? 0 : 1));
        emit_modrm(g, (sz8 || !dir) ? rany(g) : rdst(g), &m);
        return;
    }
    case 2:
        eb(g, seg); if (op16) eb(g, 0x66);
        eb(g, 0xa0 + rndn(g, 4)); ed(g, (uint32_t)(SCRATCH_MID + rndn(g, 0x400)));
        return;
    case 3: {
        unsigned a = alu[rndn(g, 8)];
        int dir = rndn(g, 2);
        int lock = !dir && a != 0x38 && rndn(g, 4) == 0;
        if (lock) pick_mem_aligned(g, &m, asz); else pick_mem(g, &m, 0);
        if (lock) eb(g, 0xf0);
        eb(g, seg); emit_prefixes(g, op16, &m, 0);
        eb(g, a + (dir ? 2 : 0) + (sz8 ? 0 : 1));
        emit_modrm(g, (sz8 || !dir) ? rany(g) : rdst(g), &m);
        return;
    }
    case 4: {
        int pop = rndn(g, 2);
        if (pop ? (g->depth - 4 < -DEPTH_MAX) : (g->depth + 4 > DEPTH_MAX))
            return;
        pick_mem(g, &m, 0);
        eb(g, seg);
        eb(g, pop ? 0x8f : 0xff);
        emit_modrm(g, pop ? 0 : 6, &m);
        g->depth += pop ? -4 : 4;
        return;
    }
    case 5: {
        static const uint8_t mx[4] = { 0xb6, 0xb7, 0xbe, 0xbf };
        pick_mem(g, &m, 0);
        eb(g, seg); emit_prefixes(g, op16, &m, 0);
        eb(g, 0x0f); eb(g, mx[rndn(g, 4)]);
        emit_modrm(g, rdst(g), &m);
        return;
    }
    case 6: {
        unsigned n = rndn(g, 2) ? 7 : 0;
        pick_mem(g, &m, 0);
        eb(g, seg); emit_prefixes(g, op16, &m, 0);
        if (n == 7) {
            unsigned k = sz8 ? 0x80 : (rndn(g, 2) ? 0x81 : 0x83);
            eb(g, k); emit_modrm(g, 7, &m);
            if (k == 0x81) { if (op16) ew(g, rnd(g)); else ed(g, rnd(g)); } else eb(g, rnd(g));
        } else {
            eb(g, sz8 ? 0xf6 : 0xf7); emit_modrm(g, 0, &m);
            if (sz8) eb(g, rnd(g)); else if (op16) ew(g, rnd(g)); else ed(g, rnd(g));
        }
        return;
    }
    case 7:
        pick_mem(g, &m, 0);
        eb(g, seg); emit_prefixes(g, op16, &m, 0);
        eb(g, sz8 ? 0xc6 : 0xc7); emit_modrm(g, 0, &m);
        if (sz8) eb(g, rnd(g)); else if (op16) ew(g, rnd(g)); else ed(g, rnd(g));
        return;
    default:
        pick_mem(g, &m, 0);
        eb(g, seg); emit_prefixes(g, rndn(g, 4) == 0, &m, 0);
        eb(g, 0x0f); eb(g, 0xaf);
        emit_modrm(g, rdst(g), &m);
        return;
    }
}

/*
 * ---- x87 ----
 * A table of operands sits at X87TAB in every case's scratch image: doubles
 * that hit each fast-path exit (NaNs quiet and signalling, infinities, zeros
 * of both signs, denormals, values at the integer and float range edges),
 * floats in the low dword of the next twelve, integers up to 64 bits (eight
 * of them above 2^53, which only an exact FILD keeps), control words and
 * MXCSR values, then four doubles that fall halfway between two floats, which
 * precision control 24 must round to even.  Stores go to X87OUT, the save
 * images to X87ENV.  Every address is reached through a form the rest of the
 * harness uses: absolute, EBP plus a displacement, a base register loaded just
 * before, or EBP plus a loaded index.
 */
#define X87TAB      (SCRATCH_MID + 0x400)
#define X87TAB_N    64
#define X87MXCSR    (X87TAB + 8 * X87TAB_N)
#define X87OUT      (SCRATCH_MID + 0x640)
#define X87ENV      (X87OUT + 0x100)
enum { X87_R64, X87_R32, X87_I16, X87_I32, X87_I64 };

static uint64_t g_x87tab[X87TAB_N + 6];

static void x87tab_init(void)
{
    static const uint64_t t[X87TAB_N + 6] = {
        0x0000000000000000ull, 0x8000000000000000ull, 0x3ff0000000000000ull, 0xbff0000000000000ull,
        0x4000000000000000ull, 0x3fe0000000000000ull, 0x3fb999999999999aull, 0x400921fb54442d18ull,
        0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull, 0xfff8000000000000ull,
        0x7ff4000000000001ull, 0x7ff800000000beefull, 0x0000000000000001ull, 0x800fffffffffffffull,
        0x0010000000000000ull, 0x7fefffffffffffffull, 0x4340000000000001ull, 0x4330000000000000ull,
        0x43e0000000000000ull, 0xc3e0000000000000ull, 0x41dfffffffc00000ull, 0xc1e0000000000000ull,
        0x40dfffc000000000ull, 0xc0e0000000000000ull, 0x4004000000000000ull, 0xc00c000000000000ull,
        0x7e37e43c8800759cull, 0x01a56e1fc2f8f359ull, 0x3ff0000000000001ull, 0x3810000000000000ull,
        0x9e3779b97fc00000ull, 0x5bd1e9957fa00000ull, 0x000000007f800000ull, 0xffffffffff800000ull,
        0x1234567800000000ull, 0x8765432180000000ull, 0x0000000000000001ull, 0x00000000007fffffull,
        0x0000000000800000ull, 0x000000007f7fffffull, 0x000000003fc00000ull, 0x00000000bdcccccdull,
        0x0000000000000000ull, 0x0000000000000001ull, 0xffffffffffffffffull, 0x0020000000000001ull,
        0x0123456789abcdefull, 0x7fffffffffffffffull, 0x8000000000000000ull, 0x4142434445464748ull,
        0x00ff00ff00ff00ffull, 0xffdfffffffffffffull, 0xffffffff80000000ull, 0x0000000000007fffull,
        0x0000000000007fffull, 0x0000000000008000ull, 0x000000007fffffffull, 0x0000000080000000ull,
        0x00000000ffffffffull, 0x000000003b9aca00ull,
        0x0f7f007f027f037full, 0x0c7f13320b7f077full,
        0x00003f8000001f80ull, 0x00007f8000005f80ull,
        0x3ff0000010000000ull, 0xbff0000030000000ull, 0x4000000010000000ull, 0x3810000008000000ull,
    };
    memcpy(g_x87tab, t, sizeof t);
}

/* int_to_f80, for the images the initial state hands the engines. */
static void x87_int_image(int64_t x, uint64_t *mant, uint16_t *se)
{
    if (x == 0) { *mant = 0; *se = 0; return; }
    uint64_t m = x < 0 ? (uint64_t)0 - (uint64_t)x : (uint64_t)x;
    int lz = __builtin_clzll(m);
    *mant = m << lz;
    *se = (uint16_t)((x < 0 ? 0x8000u : 0) | (unsigned)(16383 + 63 - lz));
}

static uint32_t x87_src(Gen *g, int kind)
{
    unsigned k;
    if (rndn(g, 4) == 0) {
        k = rndn(g, X87TAB_N - 2);
    } else {
        switch (kind) {
        case X87_R64: k = rndn(g, 36); if (k >= 32) k += X87TAB_N + 2 - 32; break;
        case X87_R32: k = 32 + rndn(g, 12); break;
        case X87_I64: k = 44 + rndn(g, 12); break;
        default:      k = rndn(g, 2) ? 56 + rndn(g, 6) : 44 + rndn(g, 12); break;
        }
    }
    return (uint32_t)(X87TAB + 8 * k + (rndn(g, 16) == 0 ? 4 : 0));
}
static uint32_t x87_dst(Gen *g) { return (uint32_t)(X87OUT + 8 * rndn(g, 30)); }

static void x87_mf(Gen *g, MF *m, uint32_t addr)
{
    memset(m, 0, sizeof *m);
    switch (rndn(g, 4)) {
    case 0:
        mf_abs32(m, addr);
        return;
    case 1:
        m->mod = 2; m->rm = 5; m->dispn = 4;
        m->disp = addr - (uint32_t)SCRATCH_MID;
        return;
    case 2: {
        uint8_t base = rdst_avoid(g);
        int8_t d = (int8_t)rndi(g, -64, 64);
        eb(g, 0xb8 | base); ed(g, addr - (uint32_t)(int32_t)d);
        m->mod = 1; m->rm = base; m->disp = (uint32_t)(int32_t)d; m->dispn = 1;
        return;
    }
    default: {
        uint8_t idx = rdst_avoid(g);
        unsigned sc = rndn(g, 4), k = rndn(g, 8);
        eb(g, 0xb8 | idx); ed(g, k);
        m->mod = 2; m->rm = 4; m->has_sib = 1;
        m->sib = (uint8_t)((sc << 6) | (idx << 3) | 5);
        m->disp = addr - (uint32_t)SCRATCH_MID - (k << sc); m->dispn = 4;
        return;
    }
    }
}

static void x87_op(Gen *g, int depth);

/* A short forward branch on cc over one or two x87 instructions. */
static void x87_skip(Gen *g, unsigned cc, int depth)
{
    eb(g, 0x70 | cc);
    size_t at = g->c->len;
    eb(g, 0);
    size_t after = g->c->len;
    int n = rndi(g, 1, 2);
    for (int k = 0; k < n && room(g) > 48; k++)
        x87_op(g, depth + 1);
    g->c->code[at] = (uint8_t)(g->c->len - after);
}

static void x87_op(Gen *g, int depth)
{
    static const uint8_t arith_sub[6] = { 0, 1, 4, 5, 6, 7 };
    static const uint8_t fcc[8] = { 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0xa, 0xb };
    MF m;
    unsigned i = rndn(g, 8), r = rndn(g, 2);
    if (room(g) < 64) return;
    switch (rndn(g, 32)) {
    case 0: case 1:
        x87_mf(g, &m, x87_src(g, r ? X87_R64 : X87_R32));
        eb(g, r ? 0xdd : 0xd9); emit_modrm(g, 0, &m);
        return;
    case 2:
        eb(g, 0xd9); eb(g, 0xc0 | i);
        return;
    case 3: case 4:
        x87_mf(g, &m, x87_dst(g));
        eb(g, r ? 0xdd : 0xd9); emit_modrm(g, 2 + rndn(g, 2), &m);
        return;
    case 5:
        eb(g, 0xdd); eb(g, (rndn(g, 2) ? 0xd0 : 0xd8) | i);
        return;
    case 6: {
        unsigned w = rndn(g, 3);
        x87_mf(g, &m, x87_src(g, w == 0 ? X87_I16 : w == 1 ? X87_I32 : X87_I64));
        eb(g, w == 1 ? 0xdb : 0xdf); emit_modrm(g, w == 2 ? 5 : 0, &m);
        return;
    }
    case 7: {
        unsigned w = rndn(g, 3);
        x87_mf(g, &m, x87_dst(g));
        if (w == 2) {
            if (rndn(g, 2)) { eb(g, 0xdf); emit_modrm(g, 7, &m); }
            else            { eb(g, 0xdd); emit_modrm(g, 1, &m); }
        } else {
            eb(g, w == 0 ? 0xdf : 0xdb); emit_modrm(g, 1 + rndn(g, 3), &m);
        }
        return;
    }
    case 8:
        eb(g, 0xd9); eb(g, 0xe8 + rndn(g, 7));
        return;
    case 9: case 10: case 11: case 12: {
        unsigned o = rndn(g, 3);
        eb(g, o == 0 ? 0xd8 : o == 1 ? 0xdc : 0xde);
        eb(g, 0xc0 | (arith_sub[rndn(g, 6)] << 3) | i);
        return;
    }
    case 13: case 14: case 15:
        x87_mf(g, &m, x87_src(g, r ? X87_R64 : X87_R32));
        eb(g, r ? 0xdc : 0xd8); emit_modrm(g, arith_sub[rndn(g, 6)], &m);
        return;
    case 16:
        x87_mf(g, &m, x87_src(g, r ? X87_I16 : X87_I32));
        eb(g, r ? 0xde : 0xda); emit_modrm(g, arith_sub[rndn(g, 6)], &m);
        return;
    case 17: {
        static const uint8_t u[4] = { 0xe0, 0xe1, 0xfa, 0xfc };
        eb(g, 0xd9); eb(g, u[rndn(g, 4)]);
        return;
    }
    case 18:
        eb(g, 0xd9); eb(g, 0xc8 | i);
        return;
    case 19: case 20:
        switch (rndn(g, 6)) {
        case 0: eb(g, 0xd8); eb(g, (r ? 0xd0 : 0xd8) | i); return;
        case 1: eb(g, 0xdd); eb(g, (r ? 0xe0 : 0xe8) | i); return;
        case 2: if (r) { eb(g, 0xde); eb(g, 0xd9); } else { eb(g, 0xda); eb(g, 0xe9); } return;
        case 3: eb(g, 0xd9); eb(g, 0xe4); return;
        case 4:
            x87_mf(g, &m, x87_src(g, r ? X87_R64 : X87_R32));
            eb(g, r ? 0xdc : 0xd8); emit_modrm(g, 2 + rndn(g, 2), &m);
            return;
        default:
            x87_mf(g, &m, x87_src(g, r ? X87_I16 : X87_I32));
            eb(g, r ? 0xde : 0xda); emit_modrm(g, 2 + rndn(g, 2), &m);
            return;
        }
    case 21: case 22: {
        eb(g, rndn(g, 2) ? 0xdb : 0xdf); eb(g, (rndn(g, 2) ? 0xe8 : 0xf0) | i);
        unsigned cc = fcc[rndn(g, 8)];
        switch (depth > 1 ? 3 : rndn(g, 4)) {
        case 0: eb(g, rndn(g, 2) ? 0xda : 0xdb); eb(g, 0xc0 | (rndn(g, 4) << 3) | rndn(g, 8)); return;
        case 1: eb(g, 0x0f); eb(g, 0x90 | cc); eb(g, 0xc0 | rbyte_avoid(g)); return;
        case 2: x87_skip(g, cc, depth); return;
        default: return;
        }
    }
    case 23: {
        static const uint8_t mask[6] = { 0x01, 0x40, 0x41, 0x45, 0x05, 0x44 };
        eb(g, 0xdf); eb(g, 0xe0);
        switch (depth > 1 ? 2 : rndn(g, 3)) {
        case 0: eb(g, 0x9e); x87_skip(g, fcc[rndn(g, 8)], depth); return;
        case 1: eb(g, 0xf6); eb(g, 0xc4); eb(g, mask[rndn(g, 6)]); x87_skip(g, 4 + rndn(g, 2), depth); return;
        default: return;
        }
    }
    case 24:
        eb(g, rndn(g, 2) ? 0xda : 0xdb); eb(g, 0xc0 | (rndn(g, 4) << 3) | i);
        return;
    case 25:
        x87_mf(g, &m, x87_dst(g));
        eb(g, r ? 0xdd : 0xd9); emit_modrm(g, 7, &m);
        return;
    case 26:
        x87_mf(g, &m, (uint32_t)(X87TAB + 8 * 62 + 2 * rndn(g, 8)));
        eb(g, 0xd9); emit_modrm(g, 5, &m);
        return;
    case 27:
        switch (rndn(g, 7)) {
        case 0: eb(g, 0xdb); eb(g, rndn(g, 4) ? 0xe2 : 0xe3); return;
        case 1: eb(g, 0x9b); return;
        case 2: eb(g, 0xdd); eb(g, 0xc0 | i); return;
        case 3: eb(g, 0xdf); eb(g, 0xc0 | i); return;
        case 4: eb(g, 0xd9); eb(g, 0xf6); return;
        default: eb(g, 0xd9); eb(g, 0xf7); return;
        }
    case 28:
        x87_mf(g, &m, x87_src(g, X87_I64));
        eb(g, 0xdf); emit_modrm(g, 5, &m);
        x87_mf(g, &m, x87_dst(g));
        eb(g, 0xdf); emit_modrm(g, 7, &m);
        return;
    case 29: {
        static const uint8_t u[6] = { 0xe5, 0xf8, 0xfd, 0xf0, 0xf5, 0xf4 };
        switch (rndn(g, 4)) {
        case 0: eb(g, 0xd9); eb(g, u[rndn(g, 6)]); return;
        case 1:
            x87_mf(g, &m, r ? x87_src(g, X87_R64) : x87_dst(g));
            eb(g, 0xdb); emit_modrm(g, r ? 5 : 7, &m);
            return;
        case 2:
            x87_mf(g, &m, (uint32_t)X87ENV);
            eb(g, 0xd9); emit_modrm(g, 6, &m);
            if (r) { x87_mf(g, &m, (uint32_t)X87ENV); eb(g, 0xd9); emit_modrm(g, 4, &m); }
            return;
        default:
            x87_mf(g, &m, (uint32_t)X87ENV);
            eb(g, 0xdd); emit_modrm(g, 6, &m);
            if (r) { x87_mf(g, &m, (uint32_t)X87ENV); eb(g, 0xdd); emit_modrm(g, 4, &m); }
            return;
        }
    }
    case 30:
        x87_mf(g, &m, (uint32_t)(X87MXCSR + 4 * rndn(g, 4)));
        eb(g, 0x0f); eb(g, 0xae); emit_modrm(g, 2, &m);
        return;
    default:
        blob(g, 1);
        return;
    }
}

/* x87 in the integer soup, from the reset state and over random scratch bytes. */
static void t_x87(Gen *g)
{
    if (room(g) < 96) return;
    MF m;
    int n = rndi(g, 1, 4);
    for (int k = 0; k < n; k++) {
        if (rndn(g, 3) == 0) {
            pick_mem(g, &m, 0);
            emit_prefixes(g, 0, &m, 0);
            eb(g, rndn(g, 2) ? 0xdd : 0xd9); emit_modrm(g, 0, &m);
        } else {
            x87_op(g, 1);
        }
    }
}

typedef void (*Tmpl)(Gen *);
static const struct { Tmpl fn; int weight; } TEMPLATES[] = {
    { t_alu,    22 }, { t_grp1,  12 }, { t_incdec,  8 }, { t_mov,   18 },
    { t_lea,     6 }, { t_test,   7 }, { t_xchg,    5 }, { t_shift, 12 },
    { t_grp3,    8 }, { t_imul,   5 }, { t_misc1,   7 }, { t_stack, 14 },
    { t_pusha,   3 }, { t_enter,  2 }, { t_jcc,     8 }, { t_jmp,    3 },
    { t_loop,    4 }, { t_jecxz,  2 }, { t_call,    4 }, { t_string, 5 },
    { t_0f,     14 }, { t_seg,    10 }, { t_cx8,    4 },
    { t_btmem,   4 }, { t_cmpjcc, 10 }, { t_cntloop, 5 },
    { t_ccuse,  10 }, { t_pairs,   5 }, { t_movshift, 3 },
    { t_x87,     6 },
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
    if (g.depth > 0)      { eb(&g, 0x81); eb(&g, 0xc4); ed(&g, (uint32_t)g.depth); }
    else if (g.depth < 0) { eb(&g, 0x81); eb(&g, 0xec); ed(&g, (uint32_t)-g.depth); }

    for (int i = 0; i < 16; i++)
        c->gpr[i] = sm64(&g.rng);
    c->gpr[OCERZ_RSP] = ESP0;
    c->gpr[OCERZ_RBP] = SCRATCH_MID;
    c->rflags = OCERZ_FLAG_FIXED1 | OCERZ_IF | (sm64(&g.rng) & 0x8d5ull);
    c->memseed = sm64(&g.rng);
    c->fs_base = (uint64_t)rndn(&g, 0x40) * 16;
    c->gs_base = (uint64_t)rndn(&g, 0x40) * 16;
}

/* The register file an x87 case starts from (the header's x87 section says what varies). */
static void x87_state(Gen *g, Case *c)
{
    static const uint16_t cws[12] = { 0x037f, 0x037f, 0x037f, 0x027f, 0x007f, 0x007f,
                                      0x1332, 0x0f7f, 0x077f, 0x0b7f, 0x0c7f, 0x027f };
    c->x87 = 1;
    c->fcw = cws[rndn(g, 12)];
    c->mxcsr = rndn(g, 10) ? 0x1f80u : 0x1f80u | ((uint32_t)rndi(g, 1, 3) << 13);
    c->fsw = rndn(g, 2) ? 0 : (uint16_t)(rnd(g) & (rndn(g, 4) ? 0x473fu : 0x7f3fu));
    c->ftop = (uint8_t)rndn(g, 8);
    int depth = rndi(g, 1, 9);
    if (depth > 8) depth = rndn(g, 2) ? 8 : 0;
    for (int k = 0; k < 8; k++) {
        int p = (c->ftop + k) & 7;
        unsigned t = rndn(g, 36);
        uint64_t v = rndn(g, 4) ? g_x87tab[t < 32 ? t : t + X87TAB_N + 2 - 32] : sm64(&g->rng);
        c->fpr[p] = (k < depth || rndn(g, 3) == 0) ? v : 0;
        if (k < depth) c->ftw |= (uint8_t)(1u << p);
        c->xm[p] = sm64(&g->rng);
        c->xe[p] = (uint16_t)rnd(g);
        if (rndn(g, 4) == 0) {
            c->x_ok |= (uint8_t)(1u << p);
            if (rndn(g, 3)) {
                x87_int_image((int64_t)g_x87tab[44 + rndn(g, 18)], &c->xm[p], &c->xe[p]);
                c->fpr[p] = ocerz_x87_f80_dbits(c->xm[p], c->xe[p]);
            }
        }
    }
}

static void gen_x87(Case *c, uint64_t seed, int index)
{
    Gen g;
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "x87#%d", index);
    g.c = c;
    g.rng = seed ^ ((uint64_t)index * 0x2545f4914f6cdd1dull) ^ 0x87;
    g.depth = 0;
    g_avoid = 0xff;
    x87_state(&g, c);
    int n = rndi(&g, 4, 28);
    for (int i = 0; i < n && room(&g) > 96; i++)
        x87_op(&g, 0);
    for (int i = rndi(&g, 0, 3); i > 0 && room(&g) > 32; i--) {
        MF m;
        x87_mf(&g, &m, x87_dst(&g));
        eb(&g, 0xdd); emit_modrm(&g, 3, &m);
    }
    for (int i = 0; i < 16; i++)
        c->gpr[i] = sm64(&g.rng);
    c->gpr[OCERZ_RSP] = ESP0;
    c->gpr[OCERZ_RBP] = SCRATCH_MID;
    c->rflags = OCERZ_FLAG_FIXED1 | OCERZ_IF | (sm64(&g.rng) & 0x8d5ull);
    c->memseed = sm64(&g.rng);
}

static void plant_bytes(Case *c, uint64_t addr, const uint8_t *b, size_t n)
{
    if (c->nplant >= NPLANT || n > sizeof c->plant[0].bytes)
        return;
    int i = c->nplant++;
    c->plant[i].addr = addr;
    c->plant[i].len = n;
    memcpy(c->plant[i].bytes, b, n);
}

static void plant_mark_term(Case *c, uint64_t addr, uint32_t mark)
{
    uint8_t b[16];
    size_t n = 0;
    uint32_t fp = (uint32_t)FARPTR;
    b[n++] = 0xb8; memcpy(b + n, &mark, 4); n += 4;
    b[n++] = 0xff; b[n++] = 0x2d; memcpy(b + n, &fp, 4); n += 4;
    plant_bytes(c, addr, b, n);
}

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

static void h_highbyte(Gen *g)
{
    eb(g, 0xb4); eb(g, 0x5a);
    eb(g, 0xb5); eb(g, 0xa5);
    eb(g, 0xb6); eb(g, 0x7f);
    eb(g, 0xb7); eb(g, 0x80);
    eb(g, 0x00); eb(g, 0xec);
    eb(g, 0x28); eb(g, 0xf7);
    eb(g, 0x86); eb(g, 0xe3);
    eb(g, 0xfe); eb(g, 0xc6);
    eb(g, 0xfe); eb(g, 0xcf);
    eb(g, 0x8a); eb(g, 0xc4);
    eb(g, 0x0f); eb(g, 0xb6); eb(g, 0xc7);
    eb(g, 0x0f); eb(g, 0xbe); eb(g, 0xce);
    eb(g, 0x84); eb(g, 0xfc);
    eb(g, 0x38); eb(g, 0xf5);
    eb(g, 0x80); eb(g, 0xc4); eb(g, 0x01);
    eb(g, 0xc0); eb(g, 0xe5); eb(g, 0x03);
    eb(g, 0xf6); eb(g, 0xde);
    eb(g, 0xf6); eb(g, 0xd7);
    eb(g, 0x0f); eb(g, 0x94); eb(g, 0xc4);
    eb(g, 0x0f); eb(g, 0x9f); eb(g, 0xc7);
}

static void h_mov_sreg(Gen *g)
{
    MF m;
    eb(g, 0x8c); eb(g, 0xc8);
    eb(g, 0x8c); eb(g, 0xd3);
    eb(g, 0x8c); eb(g, 0xd9);
    eb(g, 0x8c); eb(g, 0xc2);
    eb(g, 0x8c); eb(g, 0xe6);
    eb(g, 0x8c); eb(g, 0xef);
    eb(g, 0x66); eb(g, 0x8c); eb(g, 0xcf);
    mf_abs32(&m, (uint32_t)SCRATCH_MID);
    eb(g, 0x8c); emit_modrm(g, 1, &m);
    eb(g, 0x8b); emit_modrm(g, 5, &m);
}

static void h_highbyte_mem(Gen *g)
{
    MF m;
    mf_abs32(&m, (uint32_t)SCRATCH_MID);
    eb(g, 0x8a); emit_modrm(g, 4, &m);
    mf_abs32(&m, (uint32_t)(SCRATCH_MID + 1));
    eb(g, 0x88); emit_modrm(g, 7, &m);
    mf_ebp8(&m, 4);
    eb(g, 0x02); emit_modrm(g, 6, &m);
    mf_ebp8(&m, -8);
    eb(g, 0x30); emit_modrm(g, 5, &m);
    mf_ebp8(&m, 12);
    eb(g, 0x86); emit_modrm(g, 4, &m);
    mf_ebp8(&m, 16);
    eb(g, 0x0f); eb(g, 0xb6); emit_modrm(g, 3, &m);
}

static void h_disp32_abs(Gen *g)
{
    MF m;
    mf_abs32(&m, (uint32_t)SCRATCH_MID);
    eb(g, 0x8b); emit_modrm(g, 0, &m);
    mf_abs32(&m, (uint32_t)(SCRATCH_MID + 4));
    eb(g, 0x01); emit_modrm(g, 1, &m);
    mf_abs32(&m, (uint32_t)(SCRATCH_MID + 8));
    eb(g, 0xc6); emit_modrm(g, 0, &m); eb(g, 0x5a);
    mf_abs32(&m, (uint32_t)(SCRATCH_MID + 12));
    eb(g, 0xff); emit_modrm(g, 0, &m);
    mf_abs32(&m, (uint32_t)(SCRATCH_MID + 16));
    eb(g, 0x66); eb(g, 0x89); emit_modrm(g, 3, &m);
    mf_abs32(&m, (uint32_t)(SCRATCH_MID + 20));
    eb(g, 0xf0); eb(g, 0x01); emit_modrm(g, 6, &m);
    eb(g, 0x83); eb(g, 0xe7); eb(g, 0x1f);
    eb(g, 0x8b); eb(g, 0x04); eb(g, 0xbd); ed(g, (uint32_t)SCRATCH_MID);
    eb(g, 0x8d); eb(g, 0x14); eb(g, 0x7d); ed(g, (uint32_t)SCRATCH_MID);
}

static void h_moffs(Gen *g)
{
    eb(g, 0xa1); ed(g, (uint32_t)SCRATCH_MID);
    eb(g, 0xa3); ed(g, (uint32_t)(SCRATCH_MID + 4));
    eb(g, 0xa0); ed(g, (uint32_t)(SCRATCH_MID + 8));
    eb(g, 0xa2); ed(g, (uint32_t)(SCRATCH_MID + 9));
    eb(g, 0x66); eb(g, 0xa1); ed(g, (uint32_t)(SCRATCH_MID + 12));
    eb(g, 0x66); eb(g, 0xa3); ed(g, (uint32_t)(SCRATCH_MID + 14));
}

static void h_stack4(Gen *g)
{
    for (int i = 0; i < 6; i++) eb(g, 0x50 | DST[i]);
    eb(g, 0x54);
    eb(g, 0x55);
    eb(g, 0x58 | 0); eb(g, 0x58 | 1);
    for (int i = 5; i >= 0; i--) eb(g, 0x58 | DST[i]);
    eb(g, 0x6a); eb(g, 0xff);
    eb(g, 0x68); ed(g, 0xdeadbeefu);
    eb(g, 0x8f); eb(g, 0xc0 | 0);
    eb(g, 0x58 | 3);
    {
        MF m;
        mf_ebp8(&m, 32);
        eb(g, 0xff); emit_modrm(g, 6, &m);
        mf_ebp8(&m, 36);
        eb(g, 0x8f); emit_modrm(g, 0, &m);
    }
}

static void h_stack2(Gen *g)
{
    eb(g, 0x66); eb(g, 0x50);
    eb(g, 0x66); eb(g, 0x51);
    eb(g, 0x53);
    eb(g, 0x66); eb(g, 0x68); ew(g, 0x1234);
    eb(g, 0x66); eb(g, 0x6a); eb(g, 0xf0);
    eb(g, 0x66); eb(g, 0x5e);
    eb(g, 0x66); eb(g, 0x5f);
    eb(g, 0x5b);
    eb(g, 0x66); eb(g, 0x59);
    eb(g, 0x66); eb(g, 0x58);
}

static void h_pushf_popf(Gen *g)
{
    set_flags(g, 0x8d5);
    eb(g, 0x9c);
    eb(g, 0x9d);
    eb(g, 0x66); eb(g, 0x9c);
    eb(g, 0x66); eb(g, 0x9d);
    set_flags(g, 0x001);
    eb(g, 0x9f);
    eb(g, 0x9e);
    set_flags(g, 0x400);
    eb(g, 0xfc);
    set_flags(g, 0x0c5);
}

static void h_pusha_popa(Gen *g)
{
    eb(g, 0x60);
    for (int i = 0; i < 6; i++) mov32(g, DST[i], 0xa5a5a500u | (unsigned)i);
    eb(g, 0x83); eb(g, 0xec); eb(g, 0x10);
    eb(g, 0x83); eb(g, 0xc4); eb(g, 0x10);
    eb(g, 0x61);
    eb(g, 0x66); eb(g, 0x60);
    for (int i = 0; i < 6; i++) mov32(g, DST[i], 0x5a5a0000u | (unsigned)i);
    eb(g, 0x66); eb(g, 0x61);
}

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
        eb(g, 0x66); eb(g, 0xb8); ew(g, rows[i].ax);
        set_flags(g, rows[i].fl);
        eb(g, rows[i].op);
        eb(g, 0x66); eb(g, 0xa3); ed(g, (uint32_t)(SCRATCH_MID + 0x100 + 4 * i));
        eb(g, 0x9c); eb(g, 0x59);
        eb(g, 0x89); eb(g, 0x0d); ed(g, (uint32_t)(SCRATCH_MID + 0x200 + 4 * i));
    }
}

static void h_aam_aad(Gen *g)
{
    static const uint8_t bases[6] = { 0x0a, 0x01, 0x02, 0x10, 0x7f, 0xff };
    for (int i = 0; i < 6; i++) {
        eb(g, 0x66); eb(g, 0xb8); ew(g, (unsigned)(0x1234 + 0x1111 * i));
        eb(g, 0xd4); eb(g, bases[i]);
        eb(g, 0x66); eb(g, 0xa3); ed(g, (uint32_t)(SCRATCH_MID + 0x300 + 4 * i));
        eb(g, 0x66); eb(g, 0xb8); ew(g, (unsigned)(0x0509 + 0x0101 * i));
        eb(g, 0xd5); eb(g, bases[i]);
        eb(g, 0x66); eb(g, 0xa3); ed(g, (uint32_t)(SCRATCH_MID + 0x340 + 4 * i));
    }
}

static void h_jmp16(Gen *g)
{
    eb(g, 0x66); eb(g, 0xe9); ew(g, 0x0400);
    plant_mark_term(g->c, 0x4404, 0x11111111);
    plant_mark_term(g->c, CODE32 + 0x404, 0x22222222);
}

static void h_jcc16(Gen *g)
{
    eb(g, 0x31); eb(g, 0xc0);
    eb(g, 0x66); eb(g, 0x0f); eb(g, 0x84); ew(g, 0x0400);
    plant_mark_term(g->c, 0x4407, 0x11111111);
    plant_mark_term(g->c, CODE32 + 0x407, 0x22222222);
}

static void h_call16(Gen *g)
{
    eb(g, 0x66); eb(g, 0xe8); ew(g, 0x0100);
    eb(g, 0xb8); ed(g, 0x44444444u);
    plant_mark_retw(g->c, 0x4104, 0x11111111);
    plant_mark_retw(g->c, CODE32 + 0x104, 0x22222222);
    plant_mark_term(g->c, 0x4004, 0x33333333);
}

static void h_near32(Gen *g)
{
    eb(g, 0x31); eb(g, 0xc0);
    eb(g, 0xeb); eb(g, 0x02); eb(g, 0x90); eb(g, 0x90);
    eb(g, 0x83); eb(g, 0xf8); eb(g, 0x00);
    eb(g, 0x74); eb(g, 0x05);
    eb(g, 0xb8); ed(g, 0xbadbad00u);
    eb(g, 0x0f); eb(g, 0x85); ed(g, 0x00000005u);
    eb(g, 0xb9); ed(g, 0xbadbad01u);
    eb(g, 0xe9); ed(g, 0x00000005u);
    eb(g, 0xba); ed(g, 0xbadbad02u);
    eb(g, 0xe8); ed(g, 0x00000007u);
    eb(g, 0xbb); ed(g, 0xbadbad03u);
    eb(g, 0xeb); eb(g, 0x01);
    eb(g, 0xc3);
}

static void h_loops(Gen *g)
{
    eb(g, 0x31); eb(g, 0xd2);
    eb(g, 0xb9); ed(g, 5);
    eb(g, 0x42);
    eb(g, 0xe2); eb(g, 0xfd);
    eb(g, 0xb9); ed(g, 4);
    eb(g, 0x42); eb(g, 0x85); eb(g, 0xd2);
    eb(g, 0xe1); eb(g, 0xfb);
    eb(g, 0xb9); ed(g, 4);
    eb(g, 0x42); eb(g, 0x85); eb(g, 0xd2);
    eb(g, 0xe0); eb(g, 0xfb);
    eb(g, 0xb9); ed(g, 0x00010003u);
    eb(g, 0x42);
    eb(g, 0x67); eb(g, 0xe2); eb(g, 0xfc);
    eb(g, 0x31); eb(g, 0xc9);
    eb(g, 0xe3); eb(g, 0x05);
    eb(g, 0xb8); ed(g, 0xbadbad04u);
    eb(g, 0xb9); ed(g, 0x00010000u);
    eb(g, 0x67); eb(g, 0xe3); eb(g, 0x05);
    eb(g, 0xb8); ed(g, 0xbadbad05u);
}

static void h_string(Gen *g)
{
    mov32(g, OCERZ_RSI, (uint32_t)(SCRATCH_MID + 0x40));
    mov32(g, OCERZ_RDI, (uint32_t)(SCRATCH_MID + 0x80));
    eb(g, 0xfc);
    eb(g, 0xb9); ed(g, 8); eb(g, 0xf3); eb(g, 0xa4);
    eb(g, 0xb9); ed(g, 4); eb(g, 0xf3); eb(g, 0xa5);
    eb(g, 0xb9); ed(g, 4); eb(g, 0xf3); eb(g, 0x66); eb(g, 0xa5);
    eb(g, 0xb9); ed(g, 4); eb(g, 0xf3); eb(g, 0xab);
    eb(g, 0xac); eb(g, 0xad);
    eb(g, 0xb9); ed(g, 8); eb(g, 0xf2); eb(g, 0xae);
    eb(g, 0xb9); ed(g, 4); eb(g, 0xf3); eb(g, 0xa7);
    mov32(g, OCERZ_RSI, (uint32_t)(SCRATCH_MID + 0x140));
    mov32(g, OCERZ_RDI, (uint32_t)(SCRATCH_MID + 0x180));
    eb(g, 0xfd);
    eb(g, 0xb9); ed(g, 8); eb(g, 0xf3); eb(g, 0xa4);
    eb(g, 0xb9); ed(g, 4); eb(g, 0xf3); eb(g, 0xa5);
    eb(g, 0xb9); ed(g, 4); eb(g, 0xf3); eb(g, 0xaa);
    eb(g, 0xa6); eb(g, 0xa7);
    eb(g, 0xfc);
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
            eb(g, 0xb3); eb(g, 0x9c);
            eb(g, 0xc0); eb(g, 0xc0 | (n << 3) | 3); eb(g, cnt[i]);
            eb(g, 0x66); eb(g, 0xbb); ew(g, 0x8421);
            eb(g, 0x66); eb(g, 0xc1); eb(g, 0xc0 | (n << 3) | 3); eb(g, cnt[i]);
        }
    eb(g, 0xb1); eb(g, 0x21);
    eb(g, 0xd3); eb(g, 0xe0);
    eb(g, 0xd3); eb(g, 0xf8);
    eb(g, 0xd2); eb(g, 0xd8);
    eb(g, 0xd1); eb(g, 0xe3);
    eb(g, 0xd0); eb(g, 0xd7);
}

static void h_shld_shrd(Gen *g)
{
    mov32(g, OCERZ_RAX, 0x12345678u);
    mov32(g, OCERZ_RBX, 0xfedcba98u);
    eb(g, 0xb1); eb(g, 0x0c);
    eb(g, 0x0f); eb(g, 0xa4); eb(g, 0xd8); eb(g, 0x00);
    eb(g, 0x0f); eb(g, 0xa4); eb(g, 0xd8); eb(g, 0x10);
    eb(g, 0x0f); eb(g, 0xa5); eb(g, 0xd8);
    eb(g, 0x0f); eb(g, 0xac); eb(g, 0xd8); eb(g, 0x07);
    eb(g, 0x0f); eb(g, 0xad); eb(g, 0xd8);
    eb(g, 0x66); eb(g, 0x0f); eb(g, 0xa4); eb(g, 0xd8); eb(g, 0x05);
    eb(g, 0x66); eb(g, 0x0f); eb(g, 0xad); eb(g, 0xd8);
    eb(g, 0x0f); eb(g, 0xa4); eb(g, 0x1d); ed(g, (uint32_t)(SCRATCH_MID + 0x40)); eb(g, 0x09);
}

static void h_muldiv(Gen *g)
{
    mov32(g, OCERZ_RAX, 0x0000fedcu);
    eb(g, 0xb1); eb(g, 0x07);
    eb(g, 0xb4); eb(g, 0x00);
    eb(g, 0xf6); eb(g, 0xf1);
    eb(g, 0x66); eb(g, 0x98);
    eb(g, 0xf6); eb(g, 0xf9);
    mov32(g, OCERZ_RAX, 0x89abcdefu);
    eb(g, 0x31); eb(g, 0xd2);
    eb(g, 0xb9); ed(g, 0x1234);
    eb(g, 0xf7); eb(g, 0xf1);
    mov32(g, OCERZ_RAX, 0x89abcdefu);
    eb(g, 0x99);
    eb(g, 0xb9); ed(g, 0x0123);
    eb(g, 0xf7); eb(g, 0xf9);
    eb(g, 0x66); eb(g, 0x31); eb(g, 0xd2);
    eb(g, 0x66); eb(g, 0xb9); ew(g, 0x0037);
    eb(g, 0x66); eb(g, 0xf7); eb(g, 0xf1);
    eb(g, 0x66); eb(g, 0x99);
    eb(g, 0x66); eb(g, 0xf7); eb(g, 0xf9);
    mov32(g, OCERZ_RAX, 0xdeadbeefu);
    eb(g, 0xf7); eb(g, 0xe3);
    mov32(g, OCERZ_RAX, 0xdeadbeefu);
    eb(g, 0xf7); eb(g, 0xeb);
    eb(g, 0xf6); eb(g, 0xe3);
    eb(g, 0xf6); eb(g, 0xeb);
    eb(g, 0x66); eb(g, 0xf7); eb(g, 0xe3);
    eb(g, 0x0f); eb(g, 0xaf); eb(g, 0xc3);
    eb(g, 0x69); eb(g, 0xc3); ed(g, 0x00001234u);
    eb(g, 0x6b); eb(g, 0xc3); eb(g, 0xf0);
    eb(g, 0xf7); eb(g, 0xd8);
    eb(g, 0xf7); eb(g, 0xd0);
}

static void h_bt(Gen *g)
{
    static const uint8_t bt[4] = { 0xa3, 0xab, 0xb3, 0xbb };
    for (int i = 0; i < 4; i++) {
        mov32(g, OCERZ_RCX, 0x0000001fu + (unsigned)i * 8);
        eb(g, 0x0f); eb(g, bt[i]); eb(g, 0xc8 | 3);
        eb(g, 0x66); eb(g, 0x0f); eb(g, bt[i]); eb(g, 0xc8 | 3);
    }
    for (int i = 0; i < 4; i++) {
        eb(g, 0x0f); eb(g, 0xba); eb(g, 0xc0 | ((4 + i) << 3) | 3); eb(g, 0x25);
        eb(g, 0x0f); eb(g, 0xba); eb(g, ((4 + i) << 3) | 5); ed(g, (uint32_t)(SCRATCH_MID + 0x60)); eb(g, 0x11);
    }
    eb(g, 0x0f); eb(g, 0xbc); eb(g, 0xc3);
    eb(g, 0x0f); eb(g, 0xbd); eb(g, 0xc3);
    eb(g, 0x31); eb(g, 0xdb);
    eb(g, 0x0f); eb(g, 0xbc); eb(g, 0xc3);
    eb(g, 0x0f); eb(g, 0xc8);
    eb(g, 0x0f); eb(g, 0xcb);
}

static void h_setcc_cmov(Gen *g)
{
    eb(g, 0x39); eb(g, 0xd8);
    for (unsigned cc = 0; cc < 16; cc++) {
        eb(g, 0x0f); eb(g, 0x90 | cc); eb(g, 0xc0 | (cc & 7));
        eb(g, 0x0f); eb(g, 0x40 | cc); eb(g, 0xc0 | (2 << 3) | 6);
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
        eb(g, 0x0f); eb(g, 0xb6); eb(g, 0xc0 | (0 << 3) | r);
        eb(g, 0x0f); eb(g, 0xbe); eb(g, 0xc0 | (1 << 3) | r);
    }
    eb(g, 0x0f); eb(g, 0xb7); eb(g, 0xc3);
    eb(g, 0x0f); eb(g, 0xbf); eb(g, 0xcb);
    eb(g, 0x0f); eb(g, 0xb6); eb(g, 0x05); ed(g, (uint32_t)(SCRATCH_MID + 0x10));
    eb(g, 0x0f); eb(g, 0xbf); eb(g, 0x0d); ed(g, (uint32_t)(SCRATCH_MID + 0x14));
    eb(g, 0x66); eb(g, 0x0f); eb(g, 0xb6); eb(g, 0xc3);
}

static void h_zeroext(Gen *g)
{
    g->c->gpr[OCERZ_RAX] = 0x1111111100000000ull;
    g->c->gpr[OCERZ_RCX] = 0x2222222200000000ull;
    g->c->gpr[OCERZ_RDX] = 0x3333333300000000ull;
    g->c->gpr[OCERZ_RBX] = 0x4444444400000000ull;
    g->c->gpr[OCERZ_RSI] = 0x5555555500000000ull;
    g->c->gpr[OCERZ_RDI] = 0x6666666600000000ull;
    mov32(g, OCERZ_RAX, 0x000000ffu);
    eb(g, 0x66); eb(g, 0xb9); ew(g, 0xbeef);
    eb(g, 0xb2); eb(g, 0x7f);
    eb(g, 0xb7); eb(g, 0x33);
    eb(g, 0x01); eb(g, 0xc6);
    eb(g, 0x31); eb(g, 0xff);
    eb(g, 0x0f); eb(g, 0xb6); eb(g, 0xc3);
    eb(g, 0x8d); eb(g, 0x0c); eb(g, 0x24);
    eb(g, 0x89); eb(g, 0xe0);
}

static void h_addr16(Gen *g)
{
    eb(g, 0x66); eb(g, 0xbb); ew(g, (unsigned)(LOW16 + 0x20));
    eb(g, 0x66); eb(g, 0xbe); ew(g, 0x0010);
    eb(g, 0x66); eb(g, 0xbf); ew(g, 0x0020);
    eb(g, 0x67); eb(g, 0x8b); eb(g, 0x40); eb(g, 0x04);
    eb(g, 0x67); eb(g, 0x89); eb(g, 0x49); eb(g, 0x08);
    eb(g, 0x67); eb(g, 0x8a); eb(g, 0x94); ew(g, (unsigned)LOW16);
    eb(g, 0x67); eb(g, 0x8b); eb(g, 0x1e); ew(g, (unsigned)(LOW16 + 0x40));
    eb(g, 0x67); eb(g, 0x66); eb(g, 0x89); eb(g, 0x85); ew(g, (unsigned)LOW16);
    eb(g, 0x67); eb(g, 0xc6); eb(g, 0x85); ew(g, (unsigned)(LOW16 + 0x10)); eb(g, 0x5a);
    eb(g, 0x67); eb(g, 0x8d); eb(g, 0x41); eb(g, 0x7f);
    eb(g, 0x67); eb(g, 0x8d); eb(g, 0x1e); ew(g, 0xffff);
    eb(g, 0x67); eb(g, 0x8b); eb(g, 0x86);
    ew(g, (unsigned)((LOW16 - (SCRATCH_MID & 0xffff)) & 0xffff));
}

static void h_lea(Gen *g)
{
    eb(g, 0x83); eb(g, 0xe1); eb(g, 0x0f);
    eb(g, 0x8d); eb(g, 0x05); ed(g, 0x12345678u);
    eb(g, 0x8d); eb(g, 0x04); eb(g, 0x8d); ed(g, 0x00001000u);
    eb(g, 0x8d); eb(g, 0x44); eb(g, 0x8d); eb(g, 0x10);
    eb(g, 0x8d); eb(g, 0x04); eb(g, 0x24);
    eb(g, 0x8d); eb(g, 0x44); eb(g, 0x24); eb(g, 0x08);
    eb(g, 0x8d); eb(g, 0x45); eb(g, 0x00);
    eb(g, 0x66); eb(g, 0x8d); eb(g, 0x45); eb(g, 0x04);
    eb(g, 0x8d); eb(g, 0x0c); eb(g, 0x49);
}

static void h_seg_prefix(Gen *g)
{
    static const uint8_t seg[4] = { 0x2e, 0x36, 0x3e, 0x26 };
    for (int i = 0; i < 4; i++) {
        eb(g, seg[i]); eb(g, 0x8b); eb(g, 0x45); eb(g, 0x00);
        eb(g, seg[i]); eb(g, 0x89); eb(g, 0x4d); eb(g, 0x04);
    }
    eb(g, 0x64); eb(g, 0x8b); eb(g, 0x05); ed(g, (uint32_t)SCRATCH_MID);
    eb(g, 0x65); eb(g, 0x8b); eb(g, 0x0d); ed(g, (uint32_t)SCRATCH_MID);
}

static void h_enter_leave(Gen *g)
{
    eb(g, 0x55);
    eb(g, 0x89); eb(g, 0xe5);
    eb(g, 0x83); eb(g, 0xec); eb(g, 0x20);
    eb(g, 0x89); eb(g, 0x45); eb(g, 0xfc);
    eb(g, 0x8b); eb(g, 0x4d); eb(g, 0xfc);
    eb(g, 0xc9);
    eb(g, 0x55);
    eb(g, 0x89); eb(g, 0xe5);
    eb(g, 0x83); eb(g, 0xec); eb(g, 0x10);
    eb(g, 0x89); eb(g, 0x5d); eb(g, 0xf8);
    eb(g, 0x66); eb(g, 0x8b); eb(g, 0x55); eb(g, 0xf8);
    eb(g, 0xc9);
    eb(g, 0x55);
    eb(g, 0x89); eb(g, 0xe5);
    eb(g, 0xc9);
}

static void h_xchg_lock(Gen *g)
{
    eb(g, 0x87); eb(g, 0x1d); ed(g, (uint32_t)(SCRATCH_MID + 0x10));
    eb(g, 0x86); eb(g, 0x0d); ed(g, (uint32_t)(SCRATCH_MID + 0x14));
    eb(g, 0xf0); eb(g, 0x01); eb(g, 0x1d); ed(g, (uint32_t)(SCRATCH_MID + 0x18));
    eb(g, 0xf0); eb(g, 0x0f); eb(g, 0xc1); eb(g, 0x1d); ed(g, (uint32_t)(SCRATCH_MID + 0x1c));
    eb(g, 0x8b); eb(g, 0x05); ed(g, (uint32_t)(SCRATCH_MID + 0x20));
    eb(g, 0xf0); eb(g, 0x0f); eb(g, 0xb1); eb(g, 0x1d); ed(g, (uint32_t)(SCRATCH_MID + 0x20));
    eb(g, 0xb8); ed(g, 0xdeadbeefu);
    eb(g, 0xf0); eb(g, 0x0f); eb(g, 0xb1); eb(g, 0x1d); ed(g, (uint32_t)(SCRATCH_MID + 0x20));
    eb(g, 0x91); eb(g, 0x93); eb(g, 0x97);
    eb(g, 0x66); eb(g, 0x91);
    eb(g, 0x90);
}

static void h_conv_flags(Gen *g)
{
    mov32(g, OCERZ_RAX, 0xffff8000u);
    eb(g, 0x98);
    eb(g, 0x99);
    eb(g, 0x66); eb(g, 0x98);
    eb(g, 0x66); eb(g, 0x99);
    eb(g, 0xf8); eb(g, 0xf5); eb(g, 0xf9); eb(g, 0xf5);
    eb(g, 0xfd); eb(g, 0xfc);
    eb(g, 0x9f); eb(g, 0x9e);
    eb(g, 0xd6);
    eb(g, 0xf9); eb(g, 0xd6);
}

static void h_esp_sib(Gen *g)
{
    eb(g, 0x50); eb(g, 0x51); eb(g, 0x52); eb(g, 0x53);
    eb(g, 0x8b); eb(g, 0x04); eb(g, 0x24);
    eb(g, 0x8b); eb(g, 0x4c); eb(g, 0x24); eb(g, 0x04);
    eb(g, 0x89); eb(g, 0x54); eb(g, 0x24); eb(g, 0x08);
    eb(g, 0x83); eb(g, 0x44); eb(g, 0x24); eb(g, 0x0c); eb(g, 0x01);
    eb(g, 0x8b); eb(g, 0x84); eb(g, 0x24); ed(g, 0x00000010u);
    eb(g, 0x83); eb(g, 0xe1); eb(g, 0x03);
    eb(g, 0x8b); eb(g, 0x04); eb(g, 0x8c);
    eb(g, 0x83); eb(g, 0xc4); eb(g, 0x10);
}

static void h_callret(Gen *g)
{
    size_t j, at, c1, c2, c3;
    int32_t rel;

    eb(g, 0xeb); j = g->c->len; eb(g, 0);
    c1 = g->c->len;
    eb(g, 0x40); eb(g, 0xc3);
    g->c->code[j] = (uint8_t)(g->c->len - (j + 1));
    eb(g, 0xe8); at = g->c->len; ed(g, 0);
    rel = (int32_t)((int64_t)c1 - (int64_t)g->c->len);
    memcpy(&g->c->code[at], &rel, 4);

    eb(g, 0xeb); j = g->c->len; eb(g, 0);
    c2 = g->c->len;
    eb(g, 0x41); eb(g, 0xc2); ew(g, 0x0008);
    g->c->code[j] = (uint8_t)(g->c->len - (j + 1));
    eb(g, 0x6a); eb(g, 0x11); eb(g, 0x6a); eb(g, 0x22);
    eb(g, 0xe8); at = g->c->len; ed(g, 0);
    rel = (int32_t)((int64_t)c2 - (int64_t)g->c->len);
    memcpy(&g->c->code[at], &rel, 4);

    eb(g, 0xeb); j = g->c->len; eb(g, 0);
    c3 = g->c->len;
    eb(g, 0x42); eb(g, 0xc3);
    g->c->code[j] = (uint8_t)(g->c->len - (j + 1));
    eb(g, 0xbb); ed(g, (uint32_t)(CODE32 + c3));
    eb(g, 0x89); eb(g, 0x1d); ed(g, (uint32_t)(SCRATCH_MID + 0x30));
    eb(g, 0xff); eb(g, 0xd3);
    eb(g, 0xff); eb(g, 0x15); ed(g, (uint32_t)(SCRATCH_MID + 0x30));
    eb(g, 0x68); at = g->c->len; ed(g, 0);
    eb(g, 0xff); eb(g, 0xe0 | 3);
    { uint32_t term = (uint32_t)(CODE32 + g->c->len); memcpy(&g->c->code[at], &term, 4); }
}


static void fs_op(Gen *g, unsigned seg, unsigned op, unsigned reg, uint32_t off)
{
    eb(g, seg); eb(g, op); eb(g, (reg << 3) | 5); ed(g, off);
}

static void h_seh_teb(Gen *g)
{
    g->c->fs_base = TEB;
    g->c->gs_base = TEB + 0x200;
    eb(g, 0x64); eb(g, 0xc7); eb(g, 0x05); ed(g, 0x18); ed(g, (uint32_t)TEB);
    eb(g, 0x64); eb(g, 0xc7); eb(g, 0x05); ed(g, 0x2c); ed(g, (uint32_t)(TEB + 0x100));
    eb(g, 0x64); eb(g, 0xc7); eb(g, 0x05); ed(g, 0x00); ed(g, 0xffffffffu);
    for (int f = 0; f < 2; f++) {
        eb(g, 0x68); ed(g, 0x00401000u + 0x1000u * (unsigned)f);
        fs_op(g, 0x64, 0xff, 6, 0);
        fs_op(g, 0x64, 0x89, 4, 0);
    }
    eb(g, 0x64); eb(g, 0xa1); ed(g, 0x18);
    eb(g, 0x8b); eb(g, 0x50); eb(g, 0x2c);
    fs_op(g, 0x64, 0x8b, 1, 0x2c);
    eb(g, 0x8b); eb(g, 0x59); eb(g, 0x04);
    mov32(g, OCERZ_RSI, 0x10);
    eb(g, 0x64); eb(g, 0x8b); eb(g, 0x7e); eb(g, 0x20);
    eb(g, 0x64); eb(g, 0x01); eb(g, 0x3c); eb(g, 0x75); ed(g, 0x08);
    fs_op(g, 0x64, 0x39, 0, 0x18);
    fs_op(g, 0x64, 0xff, 0, 0x40);
    eb(g, 0x64); eb(g, 0xf0); eb(g, 0x0f); eb(g, 0xc1); eb(g, 0x05); ed(g, 0x48);
    eb(g, 0xf0); eb(g, 0x64); eb(g, 0x0f); eb(g, 0xb1); eb(g, 0x0d); ed(g, 0x4c);
    fs_op(g, 0x64, 0x87, 2, 0x50);
    eb(g, 0x65); eb(g, 0xa1); ed(g, 0x10);
    eb(g, 0x65); eb(g, 0xa3); ed(g, 0x14);
    fs_op(g, 0x65, 0x89, 3, 0x18);
    eb(g, 0x65); eb(g, 0x66); eb(g, 0x89); eb(g, 0x0d); ed(g, 0x1c);
    eb(g, 0x65); eb(g, 0x88); eb(g, 0x2d); ed(g, 0x1f);
    eb(g, 0x8b); eb(g, 0x04); eb(g, 0x24);
    eb(g, 0x64); eb(g, 0xa3); ed(g, 0);
    eb(g, 0x83); eb(g, 0xc4); eb(g, 0x08);
    fs_op(g, 0x64, 0x8f, 0, 0);
    eb(g, 0x83); eb(g, 0xc4); eb(g, 0x04);
    eb(g, 0x64); eb(g, 0x0f); eb(g, 0xb6); eb(g, 0x05); ed(g, 0x2c);
    eb(g, 0x64); eb(g, 0x0f); eb(g, 0xbf); eb(g, 0x0d); ed(g, 0x18);
    eb(g, 0x64); eb(g, 0x0f); eb(g, 0xaf); eb(g, 0x15); ed(g, 0x18);
    fs_op(g, 0x64, 0xff, 6, 0x18);
    eb(g, 0x58);
}

static void bp8(Gen *g, const uint8_t *op, int n, unsigned reg, int8_t d)
{
    for (int i = 0; i < n; i++) eb(g, op[i]);
    eb(g, 0x45 | (reg << 3)); eb(g, (uint8_t)d);
}

static void h_atomics32(Gen *g)
{
    static const uint8_t XADD[] = { 0xf0, 0x0f, 0xc1 }, XADDW[] = { 0xf0, 0x66, 0x0f, 0xc1 },
                         XADDB[] = { 0xf0, 0x0f, 0xc0 }, XADDN[] = { 0x0f, 0xc1 },
                         CX[] = { 0xf0, 0x0f, 0xb1 }, CXB[] = { 0xf0, 0x0f, 0xb0 }, CXW[] = { 0xf0, 0x66, 0x0f, 0xb1 },
                         XCHG[] = { 0x87 }, XCHGB[] = { 0x86 }, MOVL[] = { 0x8b }, MOVW[] = { 0x66, 0x8b },
                         LSUB[] = { 0xf0, 0x29 }, LAND[] = { 0xf0, 0x21 }, LINC[] = { 0xf0, 0xff },
                         LDECW[] = { 0xf0, 0x66, 0xff }, LNOT[] = { 0xf0, 0xf7 }, ADD[] = { 0x01 },
                         TEST[] = { 0x85 }, INCB[] = { 0xfe }, NOTW[] = { 0x66, 0xf7 }, CX8[] = { 0xf0, 0x0f, 0xc7 },
                         CX8N[] = { 0x0f, 0xc7 };
    mov32(g, OCERZ_RAX, 0x11223344u);
    mov32(g, OCERZ_RCX, 0x55667788u);
    eb(g, 0x87); eb(g, 0xc1);
    eb(g, 0x87); eb(g, 0xc9);
    eb(g, 0x66); eb(g, 0x87); eb(g, 0xd3);
    eb(g, 0x86); eb(g, 0xe0);
    eb(g, 0x86); eb(g, 0xf7);
    eb(g, 0x86); eb(g, 0xc3);
    eb(g, 0x86); eb(g, 0xe4);
    eb(g, 0x93);
    eb(g, 0x66); eb(g, 0x96);
    mov32(g, OCERZ_RAX, 5);
    bp8(g, XADD, 3, 0, 0x10);
    bp8(g, XADDW, 4, 0, 0x14);
    bp8(g, XADDB, 3, 5, 0x17);
    bp8(g, XADDN, 2, 2, 0x18);
    bp8(g, MOVL, 1, 0, 0x20);
    bp8(g, CX, 3, 3, 0x20);
    bp8(g, CX, 3, 1, 0x20);
    bp8(g, CXB, 3, 2, 0x24);
    bp8(g, MOVW, 2, 0, 0x26);
    bp8(g, CXW, 4, 6, 0x26);
    bp8(g, XCHG, 1, 6, 0x28);
    bp8(g, XCHGB, 1, 1, 0x2c);
    eb(g, 0xf0); eb(g, 0x83); eb(g, 0x45); eb(g, 0x30); eb(g, 0x07);
    bp8(g, LSUB, 2, 0, 0x30);
    bp8(g, LAND, 2, 1, 0x34);
    eb(g, 0xf0); eb(g, 0x66); eb(g, 0x81); eb(g, 0x4d); eb(g, 0x38); ew(g, 0x0101);
    eb(g, 0xf0); eb(g, 0x80); eb(g, 0x75); eb(g, 0x3a); eb(g, 0x5a);
    bp8(g, LINC, 2, 0, 0x3c);
    bp8(g, LDECW, 3, 1, 0x40);
    bp8(g, LNOT, 2, 2, 0x44);
    bp8(g, LNOT, 2, 3, 0x48);
    bp8(g, ADD, 1, 7, 0x4c);
    eb(g, 0x83); eb(g, 0x7d); eb(g, 0x4c); eb(g, 0x03);
    bp8(g, TEST, 1, 0, 0x4c);
    bp8(g, INCB, 1, 0, 0x50);
    bp8(g, NOTW, 2, 2, 0x52);
    bp8(g, XADD, 3, 0, 0x59);
    bp8(g, CX, 3, 3, 0x5e);
    bp8(g, XCHG, 1, 2, 0x63);
    bp8(g, LINC, 2, 0, 0x67);
    bp8(g, MOVL, 1, 0, -0x10);
    bp8(g, MOVL, 1, 2, -0x0c);
    mov32(g, OCERZ_RBX, 0x12345678u);
    mov32(g, OCERZ_RCX, 0x9abcdef0u);
    bp8(g, CX8, 3, 1, -0x10);
    eb(g, 0x9c); eb(g, 0x5f);
    bp8(g, CX8, 3, 1, -0x10);
    eb(g, 0x9c); eb(g, 0x5e);
    bp8(g, CX8N, 2, 1, -0x10);
    bp8(g, CX8, 3, 1, -0x1d);
    eb(g, 0x64); eb(g, 0xf0); eb(g, 0x0f); eb(g, 0xc7); eb(g, 0x0d); ed(g, (uint32_t)(SCRATCH_MID + 0x58));
}

static void h_lock_many(Gen *g)
{
    static const uint8_t XADD[] = { 0xf0, 0x0f, 0xc1 }, CX8[] = { 0xf0, 0x0f, 0xc7 };
    mov32(g, OCERZ_RAX, 3);
    for (int k = 0; k < 40; k++) {
        eb(g, 0xf0); eb(g, 0x83); eb(g, 0x45); eb(g, (uint8_t)(-0x80 + 4 * k)); eb(g, 0x01);
    }
    bp8(g, XADD, 3, 0, 0x31);
    bp8(g, XADD, 3, 0, 0x40);
    bp8(g, CX8, 3, 1, 0x48);
    bp8(g, CX8, 3, 1, 0x53);
}

static void h_bt32(Gen *g)
{
    static const uint8_t bt[4] = { 0xa3, 0xab, 0xb3, 0xbb };
    g->c->gpr[OCERZ_RDX] = 0xa5a5a5a5f0f0f0f0ull;
    g->c->gpr[OCERZ_RBX] = 0x5a5a5a5a0f0f0f0full;
    for (int i = 0; i < 4; i++) {
        mov32(g, OCERZ_RCX, 0x40u + 9u * (unsigned)i);
        eb(g, 0x0f); eb(g, bt[i]); eb(g, 0xca);
        eb(g, 0x9c); eb(g, 0x5e);
        eb(g, 0x66); eb(g, 0x0f); eb(g, bt[i]); eb(g, 0xcb);
        eb(g, 0x0f); eb(g, 0xba); eb(g, 0xc0 | ((4 + i) << 3) | 2); eb(g, 0x3f - 5 * i);
        eb(g, 0x66); eb(g, 0x0f); eb(g, 0xba); eb(g, 0xc0 | ((4 + i) << 3) | 3); eb(g, 0x1d);
    }
    static const int32_t offs[6] = { 0, 7, 8, 0x1ff, -1, -0x200 };
    for (int i = 0; i < 6; i++) {
        mov32(g, OCERZ_RCX, (uint32_t)offs[i]);
        eb(g, 0x0f); eb(g, 0xa3); eb(g, 0x4d); eb(g, 0x40);
        eb(g, 0x9c); eb(g, 0x5f);
        eb(g, 0x66); eb(g, 0x0f); eb(g, 0xa3); eb(g, 0x0d); ed(g, (uint32_t)(SCRATCH_MID + 0x100));
        eb(g, 0x0f); eb(g, 0xba); eb(g, 0x65); eb(g, 0x44); eb(g, (uint8_t)(3 + 11 * i));
    }
    eb(g, 0x0f); eb(g, 0xa3); eb(g, 0xc8);
    eb(g, 0x0f); eb(g, 0x92); eb(g, 0xc0);
    eb(g, 0x0f); eb(g, 0xab); eb(g, 0xcb);
}

static void h_comis_cc(Gen *g)
{
    static const uint32_t pairs[5][2] = {
        { 0x3f800000u, 0x3f800000u }, { 0x3f800000u, 0x40000000u }, { 0x40000000u, 0x3f800000u },
        { 0x7fc00000u, 0x3f800000u }, { 0x80000000u, 0x00000000u },
    };
    static const uint8_t ccs[8] = { 0x7, 0x3, 0x2, 0x6, 0x4, 0x5, 0xa, 0xb };
    for (int p = 0; p < 10; p++) {
        int dbl = p >= 5;
        const uint32_t *v = pairs[p % 5];
        if (!dbl) {
            mov32(g, OCERZ_RAX, v[0]); eb(g, 0x66); eb(g, 0x0f); eb(g, 0x6e); eb(g, 0xc0);
            mov32(g, OCERZ_RAX, v[1]); eb(g, 0x66); eb(g, 0x0f); eb(g, 0x6e); eb(g, 0xc8);
        } else if (p % 5 == 3) {
            eb(g, 0x66); eb(g, 0x0f); eb(g, 0x76); eb(g, 0xc0);
            mov32(g, OCERZ_RAX, 1); eb(g, 0xf2); eb(g, 0x0f); eb(g, 0x2a); eb(g, 0xc8);
        } else {
            static const uint32_t iv[5][2] = { { 1, 1 }, { 1, 2 }, { 2, 1 }, { 0, 0 }, { 0, 0 } };
            mov32(g, OCERZ_RAX, iv[p % 5][0]); eb(g, 0xf2); eb(g, 0x0f); eb(g, 0x2a); eb(g, 0xc0);
            mov32(g, OCERZ_RAX, iv[p % 5][1]); eb(g, 0xf2); eb(g, 0x0f); eb(g, 0x2a); eb(g, 0xc8);
        }
        if (dbl) eb(g, 0x66);
        eb(g, 0x0f); eb(g, (p & 1) ? 0x2f : 0x2e); eb(g, 0xc1);
        for (int half = 0; half < 2; half++) {
            for (int k = 0; k < 4; k++) {
                eb(g, 0x0f); eb(g, 0x90 | ccs[4 * half + k]); eb(g, 0xc0 | (unsigned)k);
            }
            for (unsigned r = 0; r < 4; r++) {
                eb(g, 0x89); eb(g, 0x05 | (r << 3));
                ed(g, (uint32_t)(SCRATCH_MID + 0x300 + 32 * (unsigned)p + 16 * (unsigned)half + 4 * r));
            }
        }
        eb(g, 0x0f); eb(g, 0x47); eb(g, 0xf3);
        eb(g, 0x0f); eb(g, 0x42); eb(g, 0xfa);
    }
}

#define TAB(k) ((uint32_t)(X87TAB + 8 * (k)))
#define OUT(k) ((uint32_t)(X87OUT + 8 * (k)))
static void x87m(Gen *g, unsigned opc, unsigned digit, uint32_t addr)
{
    eb(g, opc); eb(g, (digit << 3) | 5); ed(g, addr);
}
static void x87_hand_state(Gen *g, uint16_t fcw, uint16_t fsw)
{
    Case *c = g->c;
    c->x87 = 1;
    c->fcw = fcw;
    c->fsw = fsw;
    c->mxcsr = 0x1f80;
}

/* Delphi's Move(): fild qword / fistp qword must copy all eight bytes. */
static void h_x87_courier(Gen *g)
{
    x87_hand_state(g, 0x037f, 0);
    for (int k = 0; k < 12; k++) {
        x87m(g, 0xdf, 5, TAB(44 + k));
        x87m(g, 0xdf, 7, OUT(k));
    }
    eb(g, 0xbe); ed(g, TAB(48));
    eb(g, 0xbf); ed(g, OUT(20));
    eb(g, 0xdf); eb(g, 0x2e);
    eb(g, 0xdf); eb(g, 0x3f);
    eb(g, 0xdf); eb(g, 0x6e); eb(g, 8);
    eb(g, 0xdf); eb(g, 0x7f); eb(g, 8);
    x87m(g, 0xdf, 5, TAB(49));
    x87m(g, 0xdf, 5, TAB(53));
    x87m(g, 0xdf, 7, OUT(24));
    x87m(g, 0xdf, 7, OUT(25));
}

/* Delphi's Trunc(): chop through a control word saved and restored around fistp. */
static void h_x87_trunc(Gen *g)
{
    static const int vals[5] = { 26, 27, 6, 22, 21 };
    x87_hand_state(g, 0x1332, 0);
    for (int k = 0; k < 5; k++) {
        x87m(g, 0xdd, 0, TAB(vals[k]));
        eb(g, 0x83); eb(g, 0xec); eb(g, 12);
        eb(g, 0xd9); eb(g, 0x3c); eb(g, 0x24);
        eb(g, 0xd9); eb(g, 0x7c); eb(g, 0x24); eb(g, 2);
        eb(g, 0x66); eb(g, 0x81); eb(g, 0x4c); eb(g, 0x24); eb(g, 2); ew(g, 0x0f00);
        eb(g, 0xd9); eb(g, 0x6c); eb(g, 0x24); eb(g, 2);
        eb(g, 0xdf); eb(g, 0x7c); eb(g, 0x24); eb(g, 4);
        eb(g, 0xd9); eb(g, 0x2c); eb(g, 0x24);
        eb(g, 0x59); eb(g, 0x58); eb(g, 0x5a);
    }
}

/* Direct3D 9's precision control 24 and back, through results that leave the fast path. */
static void h_x87_pc24(Gen *g)
{
    x87_hand_state(g, 0x037f, 0);
    for (int pass = 0; pass < 2; pass++) {
        x87m(g, 0xd9, 5, (uint32_t)(TAB(62) + (pass ? 0 : 4)));
        x87m(g, 0xdd, 0, TAB(6));
        x87m(g, 0xdc, 1, TAB(7));
        x87m(g, 0xdc, 0, TAB(2));
        x87m(g, 0xdc, 6, TAB(4));
        eb(g, 0xd9); eb(g, 0xfa);
        x87m(g, 0xdd, 3, OUT(pass * 8 + 0));
        x87m(g, 0xdd, 0, TAB(28));
        eb(g, 0xd8); eb(g, 0xc8);
        x87m(g, 0xdd, 3, OUT(pass * 8 + 1));
        x87m(g, 0xdd, 0, TAB(29));
        eb(g, 0xd9); eb(g, 0xc0);
        eb(g, 0xde); eb(g, 0xc9);
        x87m(g, 0xdd, 3, OUT(pass * 8 + 2));
        eb(g, 0xd9); eb(g, 0xe8);
        x87m(g, 0xdd, 0, TAB(30));
        eb(g, 0xd8); eb(g, 0xe1);
        x87m(g, 0xdd, 3, OUT(pass * 8 + 3));
        x87m(g, 0xdd, 0, TAB(17));
        x87m(g, 0xdc, 0, TAB(17));
        x87m(g, 0xdd, 3, OUT(pass * 8 + 4));
        x87m(g, 0xdd, 0, TAB(16));
        x87m(g, 0xdc, 4, TAB(14));
        x87m(g, 0xdd, 3, OUT(pass * 8 + 5));
        eb(g, 0xdd); eb(g, 0xd8);
    }
}

/* fcom + fnstsw + sahf, fcomi + fcmovcc, over ordered, equal and unordered pairs. */
static void h_x87_compare(Gen *g)
{
    static const int pairs[7][2] = { { 2, 3 }, { 3, 2 }, { 2, 2 }, { 1, 0 }, { 10, 2 }, { 8, 17 }, { 14, 0 } };
    x87_hand_state(g, 0x037f, 0);
    for (int k = 0; k < 7; k++) {
        x87m(g, 0xdd, 0, TAB(pairs[k][1]));
        x87m(g, 0xdd, 0, TAB(pairs[k][0]));
        eb(g, 0xd8); eb(g, 0xd1);
        eb(g, 0xdf); eb(g, 0xe0);
        eb(g, 0x9e);
        eb(g, 0x0f); eb(g, 0x92); eb(g, 0xc1);
        eb(g, 0x0f); eb(g, 0x9a); eb(g, 0xc2);
        eb(g, 0xdb); eb(g, 0xf1);
        eb(g, 0x0f); eb(g, 0x97); eb(g, 0xc3);
        eb(g, 0xda); eb(g, 0xc1 | ((unsigned)(k & 3) << 3));
        eb(g, 0xdb); eb(g, 0xe9);
        eb(g, 0xdb); eb(g, 0xc1 | ((unsigned)((k + 1) & 3) << 3));
        eb(g, 0xdf); eb(g, 0xe9);
        eb(g, 0xdd); eb(g, 0xd8);
    }
}

/* More than eight pushes, exchanges and stores across the wrap, and the status word. */
static void h_x87_wrap(Gen *g)
{
    x87_hand_state(g, 0x027f, 0x0020);
    for (int k = 0; k < 10; k++)
        x87m(g, 0xdd, 0, TAB(k * 3));
    eb(g, 0xd9); eb(g, 0xcf);
    eb(g, 0xdd); eb(g, 0xd5);
    eb(g, 0xd9); eb(g, 0xc7);
    eb(g, 0xd9); eb(g, 0xf6);
    eb(g, 0xd9); eb(g, 0xf7);
    eb(g, 0xdd); eb(g, 0xc3);
    eb(g, 0xdf); eb(g, 0xe0);
    for (int k = 0; k < 9; k++) { eb(g, 0xdd); eb(g, 0xd8); }
    x87m(g, 0xdd, 7, OUT(1));
}

/* PE from clear: exact and inexact arithmetic, conversions and fnclex between them. */
static void h_x87_inexact(Gen *g)
{
    x87_hand_state(g, 0x037f, 0);
    eb(g, 0xd9); eb(g, 0xe8);
    x87m(g, 0xdc, 0, TAB(2));
    x87m(g, 0xdd, 7, OUT(0));
    x87m(g, 0xdc, 0, TAB(6));
    x87m(g, 0xdd, 7, OUT(1));
    eb(g, 0xdb); eb(g, 0xe2);
    x87m(g, 0xdc, 1, TAB(4));
    x87m(g, 0xdd, 7, OUT(2));
    x87m(g, 0xdc, 6, TAB(26));
    x87m(g, 0xdd, 7, OUT(3));
    eb(g, 0xdb); eb(g, 0xe2);
    x87m(g, 0xdd, 0, TAB(26));
    x87m(g, 0xdb, 2, OUT(4));
    x87m(g, 0xdd, 7, OUT(5));
    eb(g, 0xdb); eb(g, 0xe2);
    x87m(g, 0xd9, 2, OUT(6));
    x87m(g, 0xdd, 7, OUT(7));
    x87m(g, 0xdd, 0, TAB(6));
    x87m(g, 0xd9, 3, OUT(8));
    x87m(g, 0xdd, 7, OUT(9));
    eb(g, 0xdb); eb(g, 0xe2);
    x87m(g, 0xdd, 0, TAB(4));
    eb(g, 0xd9); eb(g, 0xfa);
    x87m(g, 0xdd, 7, OUT(10));
    eb(g, 0xd9); eb(g, 0xfa);
    x87m(g, 0xdd, 7, OUT(11));
    eb(g, 0xdb); eb(g, 0xe2);
    eb(g, 0xd9); eb(g, 0xfc);
    x87m(g, 0xdd, 7, OUT(12));
    x87m(g, 0xdd, 0, TAB(6));
    eb(g, 0xd9); eb(g, 0xfc);
    x87m(g, 0xdd, 7, OUT(13));
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
    { "seh-teb",       h_seh_teb },
    { "atomics32",     h_atomics32 },
    { "lock-many",     h_lock_many },
    { "bt32",          h_bt32 },
    { "comis-cc",      h_comis_cc },
    { "x87-courier",   h_x87_courier },
    { "x87-trunc",     h_x87_trunc },
    { "x87-pc24",      h_x87_pc24 },
    { "x87-compare",   h_x87_compare },
    { "x87-wrap",      h_x87_wrap },
    { "x87-inexact",   h_x87_inexact },
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
    uint64_t fpr_xm[8];
    uint16_t fpr_xe[8];
    uint8_t  fpr_x_ok;
    uint32_t mxcsr;
    uint16_t fcw, fsw;
    uint8_t  ftw, ftop;
    unsigned long long steps;
    uint8_t  mem[SNAPLEN];
} Snap;

static OcerzVM g_vm;
static uint8_t g_golden[SNAPLEN];
static unsigned long long g_budget = 200000;

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
    memcpy(g_golden + (X87TAB - SCRATCH), g_x87tab, sizeof g_x87tab);
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

    ocerz_jit_invalidate_all(&g_vm);
    uint64_t pages[64];
    while (ocerz_mem_disarm_all(pages, 64) == 64)
        ;
    memset(code, 0xf4, (size_t)CODE32_LEN);
    memset(ocerz_g2h(LOW_CODE), 0xf4, (size_t)LOW_CODE_LEN);
    memcpy(code, c->code, c->len);
    memcpy(code + c->len, term, sizeof term);
    for (int i = 0; i < c->nplant; i++)
        memcpy(ocerz_g2h(c->plant[i].addr), c->plant[i].bytes, c->plant[i].len);

    ocerz_jit_invalidate_all(&g_vm);
    build_golden(c);
}

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
    cpu->fs_base = c->fs_base;
    cpu->gs_base = c->gs_base;
    cpu->mode32 = 1;
    cpu->cs_sel = (uint16_t)CS32;
    cpu->seg_sel[OCERZ_SREG_CS] = (uint16_t)CS32;
    cpu->rip = CODE32;
    if (c->x87) {
        cpu->fcw = c->fcw;
        cpu->fsw = c->fsw;
        cpu->ftw = c->ftw;
        cpu->ftop = c->ftop;
        cpu->fpr_x_ok = c->x_ok;
        memcpy(cpu->fpr, c->fpr, sizeof cpu->fpr);
        memcpy(cpu->fpr_xm, c->xm, sizeof cpu->fpr_xm);
        memcpy(cpu->fpr_xe, c->xe, sizeof cpu->fpr_xe);
        cpu->mxcsr = c->mxcsr;
    }
    ocerz_apply_mxcsr_round(cpu->mxcsr);
    g_vm.jit_enabled = use_jit;
    g_vm.exited = 0;

    unsigned long long steps = 0;
    int rc = OCERZ_STEP_OK;
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
    memcpy(s->fpr_xm, cpu->fpr_xm, sizeof s->fpr_xm);
    memcpy(s->fpr_xe, cpu->fpr_xe, sizeof s->fpr_xe);
    s->fpr_x_ok = cpu->fpr_x_ok;
    s->mxcsr = cpu->mxcsr;
    s->fcw = cpu->fcw;
    s->fsw = cpu->fsw;
    s->ftw = cpu->ftw;
    s->ftop = cpu->ftop;
    s->steps = steps;
    snap_memory(s->mem);
}

#define EFLAGS_MASK 0x0000000000000cd5ull

static const char *REGNAME[16] = {
    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
};

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
    for (int p = 0; p < 8; p++) {
        uint64_t x, y;
        memcpy(&x, &a->fpr[p], 8);
        memcpy(&y, &b->fpr[p], 8);
        if (x != y) {
            snprintf(buf, sizeof buf, "fpr[%d]: interp=%016llx jit=%016llx", p,
                     (unsigned long long)x, (unsigned long long)y);
            return buf;
        }
        if (a->fpr_xm[p] != b->fpr_xm[p] || a->fpr_xe[p] != b->fpr_xe[p]) {
            snprintf(buf, sizeof buf, "x87 image %d: interp=%04x:%016llx jit=%04x:%016llx", p,
                     a->fpr_xe[p], (unsigned long long)a->fpr_xm[p], b->fpr_xe[p], (unsigned long long)b->fpr_xm[p]);
            return buf;
        }
    }
    if (a->fpr_x_ok != b->fpr_x_ok || a->ftw != b->ftw || a->ftop != b->ftop) {
        snprintf(buf, sizeof buf, "x87 image bits/tags/top: interp=%02x/%02x/%u jit=%02x/%02x/%u",
                 a->fpr_x_ok, a->ftw, a->ftop, b->fpr_x_ok, b->ftw, b->ftop);
        return buf;
    }
    if (a->fcw != b->fcw || a->fsw != b->fsw) {
        snprintf(buf, sizeof buf, "x87 fcw/fsw: interp=%04x/%04x jit=%04x/%04x", a->fcw, a->fsw, b->fcw, b->fsw);
        return buf;
    }
    if (a->mxcsr != b->mxcsr) {
        snprintf(buf, sizeof buf, "mxcsr: interp=%08x jit=%08x", a->mxcsr, b->mxcsr);
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
    if (c->x87) {
        fprintf(stderr, "  x87: fcw=%04x fsw=%04x top=%u ftw=%02x x_ok=%02x mxcsr=%08x\n",
                c->fcw, c->fsw, c->ftop, c->ftw, c->x_ok, c->mxcsr);
        for (int p = 0; p < 8; p++)
            fprintf(stderr, "    fpr[%d]=%016llx image=%04x:%016llx\n", p, (unsigned long long)c->fpr[p],
                    c->xe[p], (unsigned long long)c->xm[p]);
    }
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

static const char *INJECT[] = {
    "gpr-low32", "gpr-high32", "eflags", "eip", "mode32", "cs",
    "xmm", "scratch-byte", "stack-byte", "low16-byte",
    "x87-fpr-lsb", "x87-fpr-sign", "x87-ftop", "x87-ftw", "x87-image-bit", "x87-image",
    "x87-fsw-c1", "x87-fsw-pe", "x87-fcw", "mxcsr", "step-result",
};
#define NINJECT ((int)(sizeof INJECT / sizeof INJECT[0]))

static void inject(Snap *s, int which)
{
    size_t off_scratch = 0;
    size_t off_stack = (size_t)SCRATCH_LEN;
    size_t off_low16 = (size_t)(SCRATCH_LEN + STACK_CMP_LEN);
    uint64_t u;
    switch (which) {
    case 10: memcpy(&u, &s->fpr[5], 8); u ^= 1; memcpy(&s->fpr[5], &u, 8); return;
    case 11: memcpy(&u, &s->fpr[0], 8); u ^= 1ull << 63; memcpy(&s->fpr[0], &u, 8); return;
    case 12: s->ftop = (uint8_t)((s->ftop + 1) & 7); return;
    case 13: s->ftw ^= 0x10; return;
    case 14: s->fpr_x_ok ^= 0x04; return;
    case 15: s->fpr_xm[6] ^= 1ull << 20; return;
    case 16: s->fsw ^= 0x0200; return;
    case 17: s->fsw ^= 0x0020; return;
    case 18: s->fcw ^= 0x0c00; return;
    case 19: s->mxcsr ^= 0x6000; return;
    default: break;
    }
    switch (which) {
    case 0:  s->gpr[OCERZ_RAX] ^= 1; break;
    case 1:  s->gpr[OCERZ_RSI] ^= 0x100000000ull; break;
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

static int jit_plumbing(void)
{
    OcerzCPU *cpu = &g_vm.cpu;
    uint8_t *p = (uint8_t *)ocerz_g2h(CODE64);
    int bad = 0;

    for (int round = 0; round < 2; round++) {
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

static int g_low;

static int setup_memory(void)
{
    if (g_low ? (ocerz_mem_init_identity(1ull << 30) != OCERZ_OK || ocerz_mem_init_low_shadow() != OCERZ_OK)
              : ocerz_mem_init(ARENA_LO, ARENA_HI) != OCERZ_OK) {
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

    ocerz_st(FARPTR, 4, HALT64);
    ocerz_st(FARPTR + 4, 2, CS64);

    memset(ocerz_g2h(CODE64), 0xf4, (size_t)CODE64_LEN);

    ocerz_ldt_install(CS32, 0, 0xfffff, 0xfb, 1, 0, 1);
    return 1;
}



static void kb_base(Gen *g)
{
    eb(g, 0x01); eb(g, 0xd8);
}

static void kb_seh(Gen *g)
{
    eb(g, 0x68); ed(g, 0x00401000u);
    eb(g, 0x64); eb(g, 0xff); eb(g, 0x35); ed(g, 0);
    eb(g, 0x64); eb(g, 0x89); eb(g, 0x25); ed(g, 0);
    eb(g, 0x8b); eb(g, 0x04); eb(g, 0x24);
    eb(g, 0x64); eb(g, 0xa3); ed(g, 0);
    eb(g, 0x83); eb(g, 0xc4); eb(g, 0x08);
}

static void kb_teb(Gen *g)
{
    eb(g, 0x64); eb(g, 0xa1); ed(g, 0x18);
    eb(g, 0x8b); eb(g, 0x50); eb(g, 0x2c);
    eb(g, 0x64); eb(g, 0x8b); eb(g, 0x1d); ed(g, 0x2c);
    eb(g, 0x8b); eb(g, 0x1b);
}

static void kb_xadd(Gen *g)
{
    eb(g, 0xb8); ed(g, 1);
    eb(g, 0xf0); eb(g, 0x0f); eb(g, 0xc1); eb(g, 0x45); eb(g, 0x10);
}

static void kb_cmpxchg(Gen *g)
{
    eb(g, 0x31); eb(g, 0xc0);
    eb(g, 0xba); ed(g, 1);
    eb(g, 0xf0); eb(g, 0x0f); eb(g, 0xb1); eb(g, 0x55); eb(g, 0x20);
    eb(g, 0xc7); eb(g, 0x45); eb(g, 0x20); ed(g, 0);
}

static void kb_xchg(Gen *g)
{
    eb(g, 0x89); eb(g, 0xf8);
    eb(g, 0x87); eb(g, 0x45); eb(g, 0x30);
}

static void kb_cx8(Gen *g)
{
    eb(g, 0x8b); eb(g, 0x45); eb(g, 0x40);
    eb(g, 0x8b); eb(g, 0x55); eb(g, 0x44);
    eb(g, 0x8d); eb(g, 0x58); eb(g, 0x01);
    eb(g, 0x89); eb(g, 0xd1);
    eb(g, 0xf0); eb(g, 0x0f); eb(g, 0xc7); eb(g, 0x4d); eb(g, 0x40);
}

static void kb_bt(Gen *g)
{
    eb(g, 0x0f); eb(g, 0xba); eb(g, 0xe0); eb(g, 0x03);
    eb(g, 0x0f); eb(g, 0xab); eb(g, 0xfa);
    eb(g, 0x0f); eb(g, 0xba); eb(g, 0xf2); eb(g, 0x05);
}

static void kb_cc(Gen *g)
{
    eb(g, 0x39); eb(g, 0xf8);
    eb(g, 0x0f); eb(g, 0x92); eb(g, 0xc1);
    eb(g, 0x0f); eb(g, 0x4c); eb(g, 0xd3);
    eb(g, 0x01); eb(g, 0xce);
    eb(g, 0x40);
    eb(g, 0x0f); eb(g, 0x94); eb(g, 0xc3);
}

static void kb_rmw(Gen *g)
{
    eb(g, 0x01); eb(g, 0x45); eb(g, 0x50);
    eb(g, 0xff); eb(g, 0x45); eb(g, 0x54);
    eb(g, 0x83); eb(g, 0x4d); eb(g, 0x58); eb(g, 0x01);
}

static void kb_x87(Gen *g)
{
    x87m(g, 0xdd, 0, TAB(5));
    x87m(g, 0xdc, 1, TAB(4));
    x87m(g, 0xdc, 0, OUT(0));
    x87m(g, 0xdb, 0, TAB(45));
    x87m(g, 0xdc, 1, TAB(5));
    eb(g, 0xde); eb(g, 0xc1);
    eb(g, 0xd9); eb(g, 0xc0);
    x87m(g, 0xdc, 4, TAB(24));
    eb(g, 0xd9); eb(g, 0xee);
    eb(g, 0xdf); eb(g, 0xf1);
    eb(g, 0xdb); eb(g, 0xc1);
    eb(g, 0xdd); eb(g, 0xd9);
    eb(g, 0xd9); eb(g, 0xc0);
    x87m(g, 0xdb, 3, OUT(1));
    x87m(g, 0xdd, 3, OUT(0));
}

static const struct { const char *name; void (*fn)(Gen *); const char *what; } KERNELS[] = {
    { "base",      kb_base,    "add eax, ebx" },
    { "seh",       kb_seh,     "push handler; push fs:[0]; mov fs:[0], esp; ...; mov fs:[0], eax; add esp, 8" },
    { "teb",       kb_teb,     "mov eax, fs:[0x18]; mov edx, [eax+0x2c]; mov ebx, fs:[0x2c]; mov ebx, [ebx]" },
    { "xadd",      kb_xadd,    "mov eax, 1; lock xadd [ebp+0x10], eax" },
    { "cmpxchg",   kb_cmpxchg, "xor eax, eax; mov edx, 1; lock cmpxchg [ebp+0x20], edx; mov dword [ebp+0x20], 0" },
    { "xchg",      kb_xchg,    "mov eax, edi; xchg [ebp+0x30], eax" },
    { "cmpxchg8b", kb_cx8,     "mov eax/edx, [ebp+0x40/0x44]; lea ebx, [eax+1]; mov ecx, edx; lock cmpxchg8b [ebp+0x40]" },
    { "bt",        kb_bt,      "bt eax, 3; bts edx, edi; btr edx, 5" },
    { "rmw",       kb_rmw,     "add [ebp+0x50], eax; inc dword [ebp+0x54]; or dword [ebp+0x58], 1" },
    { "cc",        kb_cc,      "cmp eax, edi; setb cl; cmovl edx, ebx; add esi, ecx; inc eax; sete bl" },
    { "x87",       kb_x87,     "fld/fmul/fadd/fild/faddp, a clamp of fld/fsub/fldz/fcomip/fcmovnb/fstp, fistp, fstp" },
};
#define NKERNELS ((int)(sizeof KERNELS / sizeof KERNELS[0]))

static void gen_kernel(Case *c, int k, uint32_t iters)
{
    Gen g;
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "bench/%s", KERNELS[k].name);
    g.c = c;
    g.rng = 1;
    g.depth = 0;
    mov32(&g, OCERZ_RDI, iters);
    size_t top = c->len;
    KERNELS[k].fn(&g);
    eb(&g, 0x4f);
    eb(&g, 0x0f); eb(&g, 0x85);
    ed(&g, (uint32_t)((int64_t)top - (int64_t)(c->len + 4)));
    c->gpr[OCERZ_RSP] = ESP0;
    c->gpr[OCERZ_RBP] = SCRATCH_MID;
    c->rflags = OCERZ_FLAG_FIXED1 | OCERZ_IF;
    c->fs_base = TEB;
    c->memseed = 1;
}

static int bench(uint32_t iters)
{
    Case c;
    OcerzCPU *cpu = &g_vm.cpu;
    printf("diff32 --bench: %u iterations per kernel, best of 3 timed runs under the JIT (%s layout)\n",
           iters, g_low ? "Wine low-shadow" : "offset arena");
    for (int k = 0; k < NKERNELS; k++) {
        gen_kernel(&c, k, iters);
        load_case(&c);
        uint64_t best = ~0ull;
        for (int r = 0; r < 4; r++) {
            restore_memory();
            ocerz_st(TEB, 4, 0);
            ocerz_st(TEB + 0x18, 4, TEB);
            ocerz_st(TEB + 0x2c, 4, TEB + 0x100);
            ocerz_st(SCRATCH_MID + 0x20, 4, 0);
            ocerz_st(X87OUT, 8, 0);
            ocerz_cpu_reset(cpu);
            memcpy(cpu->gpr, c.gpr, sizeof c.gpr);
            cpu->rflags = c.rflags;
            cpu->fs_base = c.fs_base;
            cpu->mode32 = 1;
            cpu->cs_sel = (uint16_t)CS32;
            cpu->seg_sel[OCERZ_SREG_CS] = (uint16_t)CS32;
            cpu->rip = CODE32;
            g_vm.jit_enabled = 1;
            g_vm.exited = 0;
            int rc = OCERZ_STEP_OK;
            uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            while (cpu->mode32 && rc != OCERZ_STEP_FATAL && rc != OCERZ_STEP_EXIT) {
                rc = ocerz_jit_step(&g_vm, cpu);
                if (rc == OCERZ_EUNSUP)
                    rc = ocerz_interp_step(&g_vm, cpu);
            }
            uint64_t t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
            if (rc != OCERZ_STEP_OK || cpu->rip != HALT64) {
                fprintf(stderr, "bench/%s: stopped with rc=%d at %#llx\n", KERNELS[k].name, rc,
                        (unsigned long long)cpu->rip);
                return 1;
            }
            if (r > 0 && t < best)
                best = t;
        }
        printf("  %-10s %8.2f ns/iter   %s\n", KERNELS[k].name, (double)best / iters, KERNELS[k].what);
    }
    return 0;
}

static void usage(void)
{
    printf("usage: diff32 [options]\n"
           "  --seed N          RNG seed (default 1); a failure prints the seed that reproduces it\n"
           "  --cases N         random sequences to generate (default 20000)\n"
           "  --x87-cases N     x87 sequences, from generated register files (default: as --cases)\n"
           "  --budget N        instruction/block budget per run (default 200000)\n"
           "  --only SUBSTR     run only cases whose name contains SUBSTR\n"
           "  --list            list the hand-written cases and exit\n"
           "  --selftest        prove the comparator catches an injected difference, then exit\n"
           "  --jit-required    fail if the JIT translated no 32-bit blocks (the gate passes this)\n"
           "  --bug N           sensitivity probe: make the JIT side deliberately wrong\n"
           "                    (1 no-zeroext, 2 stale-zf, 3 cf-flip) and report the catch rate\n"
           "  --bench [N]       time each 32-bit kernel in KERNELS under the JIT, N iterations\n"
           "  --low             lay guest memory out as a Wine process does: an identity\n"
           "                    arena and the low shadow window the 32-bit code lives in\n"
           "  --verbose         print a line per random case as well\n");
}

int main(int argc, char **argv)
{
    uint64_t seed = 1;
    long ncases = 20000, nx87 = -1;
    int do_selftest = 0, jit_required = 0, verbose = 0;
    uint32_t do_bench = 0;
    const char *only = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--cases") && i + 1 < argc) ncases = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--x87-cases") && i + 1 < argc) nx87 = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--budget") && i + 1 < argc) g_budget = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--selftest")) do_selftest = 1;
        else if (!strcmp(argv[i], "--jit-required")) jit_required = 1;
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--low")) g_low = 1;
        else if (!strcmp(argv[i], "--bench")) {
            do_bench = 1000000;
            if (i + 1 < argc && argv[i + 1][0] != '-') do_bench = (uint32_t)strtoul(argv[++i], NULL, 0);
        }
        else if (!strcmp(argv[i], "--bug") && i + 1 < argc) g_bug = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--list")) {
            for (int k = 0; k < NHANDS; k++) printf("hand/%s\n", HANDS[k].name);
            return 0;
        } else { usage(); return !strcmp(argv[i], "--help") ? 0 : 2; }
    }
    if (nx87 < 0)
        nx87 = ncases;
    x87tab_init();

    setenv("OCERZ_NO_UNSTICK", "1", 1);
    setenv("OCERZ_JIT_CODE_MB", "2", 0);

    if (!setup_memory())
        return 2;
    ocerz_vm_init(&g_vm);
    ocerz_vm_install_handlers(&g_vm);
    if (!g_vm.jit) {
        fprintf(stderr, "diff32: the JIT could not be created; there is nothing to differ against\n");
        return 2;
    }
    uint64_t blocks0 = ocerz_jit_blocks(g_vm.jit);

    if (do_bench)
        return bench(do_bench);

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
        run_side(&c, 1, &sw);
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

    for (long i = 0; i < ncases + nx87; i++) {
        if (i < ncases) gen_random(&c, seed, (int)i);
        else            gen_x87(&c, seed, (int)(i - ncases));
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
    printf("differential32: %ld passed, %ld failed (%d hand-written + %ld random + %ld x87, seed %#llx)\n",
           pass, fail, NHANDS, ncases, nx87, (unsigned long long)seed);
    printf("differential32: corpus %llu instructions in %ld sequences, %d distinct opcodes;\n"
           "                %llu guest steps executed per side\n",
           g_insns_emitted, pass + fail, nops, g_insns_executed);
    if (g_bug) {
        printf("differential32: SENSITIVITY PROBE --bug %d (%s): the JIT side was made wrong\n"
               "                on purpose and %ld of %ld sequences (%.1f%%) caught it.\n",
               g_bug, BUGNAME[g_bug < 4 ? g_bug : 0], fail, pass + fail,
               100.0 * (double)fail / (double)(pass + fail ? pass + fail : 1));
        return 0;
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
    if (jit_required && translated < (uint64_t)(pass + fail)) {
        printf("differential32: --jit-required and fewer blocks translated than sequences run:\n"
               "                FAIL, the JIT side ran most sequences in the interpreter\n");
        return 1;
    }
    return fail ? 1 : 0;
}


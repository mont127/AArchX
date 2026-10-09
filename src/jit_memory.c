/*
 * ---- addressing ----
 * Guest addresses reach the host through one of three maps (plain guest_base,
 * low_base below LOW_LIMIT, top_base above TOP_LO) plus the commpage, which in
 * identity-mapped dynamic mode cannot be mapped at its guest address at all.
 * Blocks that need them carry a per-block mark so unmarked blocks emit no
 * guards.  Bases with two or more accesses are hoisted into JMEMBASE/JMEMBASE2/
 * JMEMBASE3 (x30 is free inside a body: a guest RET pops its continuation from
 * the shadow and the epilogue restores the C return address from the frame),
 * with base + index<<scale kept in JMEMAUX and recomputed at the loop head.
 * The hoist signature is published so a chained predecessor with the same
 * signature enters past the reload.  Identity mapping needs none of this: every
 * pinned base already holds guest_base + base.
 *
 * ---- memory ordering ----
 * Ordered (x86-TSO) mode makes scalar accesses acquire/release - flags, locks
 * and atomics are scalar, a release scalar store orders every earlier vector
 * store, an acquire scalar load orders every later vector load.  Vector
 * accesses stay plain: programs do not synchronize through a 16-byte access
 * (x86 does not even make them atomic), and ordering them cost memcpy 3.3x and
 * fpvec 3.0x of Rosetta.  What that gives up is a vector load followed by a
 * scalar re-read of a seqlock counter observing newer data than the counter
 * covers, and two vector stores becoming visible out of order; OCERZ_TSO_VECTOR
 * =1 orders them too.  Measured 2026-09-05 on M2 Max; FEX ships the same
 * default.  The 8- and 4-byte forms are plain as well, though compilers do move
 * pointers and counters through xmm registers: a plain movq store after an
 * ordered scalar store can become visible first, and a plain movq load can be
 * satisfied after the scalar load that follows it, so a reader sees a published
 * counter newer than the data written before it - hundreds to over a thousand
 * times in three million handoffs in tests/dynamic/tso_narrow.c, where x86
 * never shows it.  OCERZ_TSO_NARROW=1 orders movd and movq, two instructions and no
 * barrier each (fmov and stlur, ldr and a one-byte ldapur), and =2 every vector
 * access of 8 bytes or fewer.  Neither is the default: the first took R.E.P.O.'s
 * main menu from 65 frames a second to 44, the second costs more again, and no
 * program is known to depend on either.  An ordered vector load is a plain load
 * followed by a one-byte ACQUIRE load of the same address: same-address reads
 * are coherent, so the copy sees a value at least as new as the vector and
 * everything after is ordered behind it - TSO's load ordering with no barrier,
 * where a dmb ishld waited for every outstanding miss (40-60 ns per access on a
 * 4 MB working set).
 *
 * Apple silicon faults an acquire/release access only when it crosses a 16-byte
 * granule, so the alignment guard tests exactly that; testing natural alignment
 * instead sent three quarters of memcpy's unaligned tail accesses down the slow
 * arm.  Faulting sites are hot-patched, and the out-of-line store arm picks
 * dmb ish + plain store when the block also does ordered loads (the drain is
 * cheap then: memcpy 0.31s vs 1.1s for release pieces) and release pieces
 * otherwise (store-only loops: memset 0.10s vs 0.5-1.0s).  An ordered access
 * carries no test by default: it is an ldapur or stlur, and the first time one
 * crosses a granule the fault handler patches that site into a branch to a
 * tested arm.  Only an instruction whose fault could not be patched carries
 * the test inline, and only that instruction; a second fault there marks the
 * whole block.  Testing every ordered access cost four instructions on each
 * load and store the Wine layout translates, and marking a whole block for
 * one fault put the test in front of every access of an unrolled Unity loop
 * that had one misaligned slot (OCERZ_ALIGN_TEST_ALL=1 tests every access).
 * A patched arm stands in for one instruction in the middle of an emitter's
 * sequence, so it saves the two registers it borrows on the host stack and
 * tests the granule with a bit test instead of the flags: a cmov keeps its
 * condition in JTF across the load, and an arm that borrowed JTF made a cmov
 * from a straddling address take the wrong side.
 *
 *
 * esp-relative operands of 32-bit code are stack accesses too, plain as their push and pop already are.
 *
 * Below 12 GB in the Wine layout the host address is a constant too: the
 * guard's orr is folded into the mov, and the guard takes it as translated.
 *
 * A rip-relative address in the Wine layout is known when the block is
 * translated, so is its side of 12 GB: below it the translation is one orr,
 * between it and the top strip there is none.  Only an address emit_mem_ea
 * has just formed counts, and never for an instruction that also reaches the
 * stack implicitly, whose slot would take the constant's answer.
 *
 * A 32-bit address is below 4 GB, so in the Wine layout it is in the low
 * window whatever it is, and its translation is the orr alone, as the 32-bit
 * stack's is.  An address emit_mem_ea built is already zero-extended; any
 * other is zero-extended first, which is the 32-bit wrap besides.
 *
 * A block that bailed translates without the hoist: a learned variant, as the alignment marks make.
 *
 * The Wine layout's stack delta in x0 (emit_stack_delta), for the block being translated.
 *
 * The Wine layout's base hoist (select_low_hoist): its guest register, the instruction it holds until, and the displacement span.
 *
 * An operand the block's low hoist covers: its host address is JMEMBASE plus its displacement.
 *
 * The Wine layout's stack delta.  Every guest address below 12 GB is the host's
 * with low_base or'ed in, and every one between 12 GB and the top strip is the
 * host's own, and a thread's stack sits wholly on one side: so while rsp is
 * below 12 GB every stack slot is rsp + low_base, and otherwise rsp itself.  x0,
 * the guest-base register of the other layouts, holds that delta here, and a
 * push, a pop, a call, a ret or an rsp-relative operand adds it where the fast
 * guard spent a shift, a compare, a branch and an orr (the push itself, its
 * guard and its two conversions of rsp were thirteen instructions).  The delta
 * is computed without the flags, which may be live: (rsp >> 32) - 3 is negative
 * below 12 GB, its sign spread over the word masks low_base.  It is computed
 * when a body is entered from the dispatcher, after every call-out (which
 * clobbers x0) and before a chain, and before the next instruction once an
 * instruction other than push, pop, call or ret has written rsp: those move it
 * by eight, which no stack crosses 12 GB by.  OCERZ_NO_LOW_STACK_DELTA=1 keeps
 * the guard on stack slots; OCERZ_LOWSTACK_CHECK=1 checks x0 before every
 * instruction and traps on a stale one.
 *
 * Whether an instruction may move rsp by more than push, pop, call and ret do.
 *
 * Whether an instruction can move rsp to the other side of 12 GB, so that the
 * stack delta must be recomputed after it.  A frame's add or sub rsp, imm (and
 * lea rsp, [rsp + disp]) under 64 KB cannot carry a valid stack across: no
 * guest mapping straddles 12 GB, and in the Wine layout nothing can be mapped
 * from 12 GB up to hundreds of gigabytes, so a stack below 12 GB ends at or
 * under it and one above starts far over it.  Every prologue and epilogue paid
 * four instructions for that before.
 *
 * Whether an instruction reaches the stack without naming it as an operand.
 *
 * Whether every guest access an instruction makes is a stack slot: the implicit
 * ones of push, pop, call, ret and leave, and explicit operands based on rsp
 * with no index and a displacement under a megabyte.  An index can reach memory
 * nowhere near the stack - code on a thread with a stack below 12 GB loaded from
 * hundreds of gigabytes past rsp that way (iosurface_low_stack) - so an indexed
 * operand takes the guard.  The string instructions, which reach memory through
 * other registers, are never stack-only.
 *
 * An [rsp + disp] operand in the Wine layout, whose host address is rsp plus
 * the stack delta in x0: JTA takes rsp + x0 once, and the access carries the
 * displacement, so the stack slots a block touches between two moves of rsp
 * share one add (the address cache's base form, JTA = JGB + base).
 *
 * The Wine layout's base hoist.  Every access a 64-bit block makes through a
 * register pays the low-window test - lsr, cmp, b.hs, orr - before it, because
 * the register may point on either side of 12 GB.  For the base register with
 * the most accesses before the block first writes it, the test runs once, at
 * the loop head, on the whole span the block reaches from it: when the base
 * plus its smallest and largest displacement (and size) are all below 12 GB,
 * JMEMBASE (x17) holds base | low_base and those accesses are JMEMBASE plus
 * their displacement, with no test.  When they are not, the block has met a
 * pointer of the other kind: it leaves before running anything, and C retires
 * it and remembers its key, so its translation takes the test per access from
 * then on.  The test is flag-free, since a block may be entered with the guest's
 * flags live in NZCV.  It pays for itself from three accesses.  winbench64's
 * struct-of-floats loop went from 35.5 to 33.1 ms (Rosetta 12), against 28 with
 * no test at all.  OCERZ_NO_LOW_HOIST=1 turns it off.
 *
 * Blocks marked to translate without an assumption that failed them: open addressing on the block key.
 *
 * At the loop head: the span below 12 GB, then JMEMBASE = base | low_base; the
 * branches go to the bail stub.  Without touching the flags, which a block may
 * be entered with live in NZCV: (base | (base + hi)) >> 32 must be below 3,
 * which also catches base + hi wrapping, and base + lo must not go below zero.
 * A span that crosses 8 GB fails the or for no reason, which costs that block
 * its hoist and nothing else.
 *
 * Out of line: leave before the block's first instruction, with side_idx -2 asking C to retire it.  It
 * returns OCERZ_STEP_PROFILE, as a probe's side exit does: STEP_OK goes to the in-arena dispatcher,
 * which would enter the same block again without C ever seeing side_blk.
 */
#include "ocerz/jit_internal.h"

static int vec_plain_size(int size);
static int low_hoist_covers(const X86Insn *insn, const X86Operand *op);
static inline int hoist_reg_for(unsigned base);
static const X86Operand *mem_hoist_view(const X86Operand *m, X86Operand *tmp);
static int m32_addr_src(A64Buf *b, unsigned greg, int scratch);
static int emit_mem_ea32(A64Buf *b, const X86Insn *insn, const X86Operand *op, int addr_reg);
static int insn_const_addr(const X86Insn *insn, uint64_t *ga);
static int rsp_small_adjust(const X86Insn *in);
static int insn_stack_implicit(const X86Insn *in);
static int insn_stack_only(const X86Insn *in);
static void emit_guard_full(A64Buf *b, int addr_reg);
static int emit_hoisted_mem_access(A64Buf *b, const X86Insn *insn,
                                   const X86Operand *mem, int size,
                                   int value_reg, int store);
static int align_test_all(void);
static void emit_granule_cross_test(A64Buf *b, int size, int ra, int scratch);
static void emit_gpr_ld_regoff(A64Buf *b, int size, int rd, int ra, int ri, int scaled, int plain);
static void emit_gpr_st_regoff(A64Buf *b, int size, int rv, int ra, int ri, int scaled, int plain);
static void emit_v_st_ordered_fast(A64Buf *b, int size, int vs, int ra, int32_t disp);
static void emit_v_acc_ordered_checked(A64Buf *b, int size, int vr, int ra, int32_t disp, int store);
static void emit_v_ld_regoff(A64Buf *b, int size, int vd, int ra, int ri, int scaled, int plain);
static void emit_v_st_regoff(A64Buf *b, int size, int vs, int ra, int ri, int scaled, int plain);
static int a64_word_may_write_x15(uint32_t w);
static int ea_cache_reusable(const A64Buf *b, const X86Operand *op);
static void ea_cache_set(const A64Buf *b, const X86Operand *op);
static int rmw_src_to(A64Buf *b, const X86Operand *s, int size, int into, int *out);
static void rmw_write_reg(A64Buf *b, const X86Operand *d, int size, int val);
static int lowhoist_marked(uint64_t key);
static void emit_misaligned_arm(A64Buf *b, const OrderedSlowPend *o);

int g_no_ldapr;

int g_no_oolslow;

static int g_const_ea_valid;

static uint64_t g_const_ea;

int g_ea_is_const;

static uint64_t g_ea_const;

static int g_ea_w32;

static OrderedSlowPend g_oslow[OSLOW_MAX];

int g_n_oslow;

int g_plain_mem;

uint64_t g_cp_marks[CP_MARK_SIZE];

int g_cp_guard;

int g_low_top;

int g_lowstack;

int stack_plain_ok(void)
{
    static int en = -1;
    if (en < 0) en = getenv("OCERZ_TSO_STRICT") ? 0 : 1;
    return en;
}

uint64_t g_al_marks[AL_MARK_SIZE];

int g_align_guard;

int vec_tso_relaxed(void)
{
    static int v = -1;
    if (v < 0) v = getenv("OCERZ_TSO_VECTOR") == NULL;
    return v;
}

static int vec_plain_size(int size)
{
    static int mode = -1;
    if (mode < 0) mode = getenv("OCERZ_TSO_NARROW") ? atoi(getenv("OCERZ_TSO_NARROW")) : 0;
    if (!vec_tso_relaxed() || size > 8 || mode == 0) return vec_tso_relaxed();
    return mode == 1 && !g_vec_int_move;
}

int g_blk_ordered_loads;

int g_al_all;

int g_al_n;

int g_mem_hoist_greg = -1;

int g_low_hoist_greg = -1;

static int g_low_hoist_until;

static int32_t g_low_hoist_hi;

static int32_t g_low_hoist_lo;

static int low_hoist_covers(const X86Insn *insn, const X86Operand *op)
{
    return g_low_hoist_greg >= 0 && insn->seg == OCERZ_SEG_NONE && insn->addrsize == 8 &&
           op->base == (unsigned)g_low_hoist_greg && op->index == OCERZ_REG_NONE && !op->riprel &&
           g_cur_insn_idx < g_low_hoist_until && op->disp >= g_low_hoist_lo && op->disp < g_low_hoist_hi;
}

static uint32_t *g_low_hoist_bail[3];

int g_ea_lowhoisted;

int g_n_low_hoist_bail;

static int g_ea_lowhoisted_reg;

int g_mem_hoist_aux_disp;

int g_mem_hoist_aux_index = -1;

int g_mem_hoist_aux_scale;

int g_mem_hoist_greg2 = -1;

int g_mem_hoist_greg3 = -1;

static inline int hoist_reg_for(unsigned base)
{
    if (g_mem_hoist_greg >= 0 && base == (unsigned)g_mem_hoist_greg) return JMEMBASE;
    if (g_mem_hoist_greg2 >= 0 && base == (unsigned)g_mem_hoist_greg2) return JMEMBASE2;
    if (g_mem_hoist_greg3 >= 0 && base == (unsigned)g_mem_hoist_greg3) return JMEMBASE3;
    if (ocerz_guest_base == 0 && pin_slot(base) >= 0 && !(g_pin_class_fwd() == 2 && base == OCERZ_RSP)) {
        static int dis = -1; if (dis < 0) dis = getenv("OCERZ_NO_IDBASE") ? 1 : 0;
        if (!dis) return pin_hreg(pin_slot(base));
    }
    return -1;
}

static const X86Operand *mem_hoist_view(const X86Operand *m, X86Operand *tmp)
{
    if (m->riprel || m->base == OCERZ_REG_NONE || m->index == OCERZ_REG_NONE || (m->scale & 3) != 0) return m;
    if (hoist_reg_for(m->base) >= 0 || hoist_reg_for(m->index) < 0) return m;
    *tmp = *m; tmp->base = m->index; tmp->index = m->base;
    return tmp;
}

int stack_identity(void)
{
    static int dis = -1;
    if (dis < 0) {
        dis = getenv("OCERZ_NO_STACK_IDX") ? 1 : 0;
    }
    return !dis && ocerz_guest_base == 0 && ocerz_low_base == 0;
}

void ocerz_jgb_trap(uint64_t rip, uint64_t x0)
{
    fprintf(stderr, "ocerz: JGB TRAP entering body of block %#llx with x0=%#llx (gbase=%#llx)\n",
            (unsigned long long)rip, (unsigned long long)x0, (unsigned long long)ocerz_guest_base);
    abort();
}

static int m32_addr_src(A64Buf *b, unsigned greg, int scratch)
{
    int s = pin_slot(greg);
    if (s >= 0)
        return pin_hreg(s);
    emit_gpr_rd(b, 0, scratch, greg);
    return scratch;
}

static int emit_mem_ea32(A64Buf *b, const X86Insn *insn, const X86Operand *op, int addr_reg)
{
    uint64_t fold = ea_fold();
    int has_b = op->base != OCERZ_REG_NONE;
    int has_i = op->index != OCERZ_REG_NONE;
    int sc = op->scale & 3;
    int64_t disp = op->disp;
    uint32_t d32 = (uint32_t)(int64_t)disp;

    if (rsp_is_ptr() && ((has_b && op->base == OCERZ_RSP) ||
                             (has_i && op->index == OCERZ_RSP)))
        return 0;

    if (!has_b && !has_i) {
        a64_mov_imm64(b, addr_reg, (uint64_t)d32 + fold);
        return 1;
    }

    int fold_reg = -1;
    if (fold) {
        if (jgb_usable() && fold == ocerz_guest_base)
            fold_reg = JGB;
        else {
            a64_mov_imm64(b, JTU, fold);
            fold_reg = JTU;
        }
    }

    if (fold_reg >= 0 && d32 == 0 && has_b != has_i) {
        unsigned g = has_b ? op->base : op->index;
        int r = m32_addr_src(b, g, addr_reg);
        a64_add_ext_uxtw(b, addr_reg, fold_reg, r, has_b ? 0 : sc);
        return 1;
    }

    int rb = has_b ? m32_addr_src(b, op->base, JT0) : -1;
    int ri = has_i ? m32_addr_src(b, op->index, rb == JT0 ? JTT : JT0) : -1;
    int t = addr_reg;
    int have = 0;

    if (has_b && has_i && disp == 0) {
        a64_add_reg(b, 0, t, rb, ri, sc);
        have = 1;
        has_i = 0;
    } else if (has_b && disp >= -4095 && disp <= 4095) {
        if (disp > 0)      a64_add_imm(b, 0, t, rb, (uint32_t)disp);
        else if (disp < 0) a64_sub_imm(b, 0, t, rb, (uint32_t)-disp);
        else               a64_mov_reg(b, 0, t, rb);
        have = 1;
    } else {
        if (d32) { a64_mov_imm64(b, t, d32); have = 1; }
        if (has_b) {
            if (have) a64_add_reg(b, 0, t, t, rb, 0);
            else      { a64_mov_reg(b, 0, t, rb); have = 1; }
        }
    }
    if (has_i) {
        if (have) a64_add_reg(b, 0, t, t, ri, sc);
        else      { a64_lsl_imm(b, 0, t, ri, sc); have = 1; }
    }
    if (fold_reg >= 0)
        a64_add_reg(b, 1, addr_reg, fold_reg, t, 0);
    return 1;
}

int emit_mem_ea(A64Buf *b, const X86Insn *insn, const X86Operand *op, int addr_reg)
{
    g_const_ea_valid = 0;
    g_ea_is_const = 0;
    g_ea_w32 = 0;
    g_ea_lowhoisted = 0;

    g_ea_plain = ocerz_low_base != 0 && insn->seg == OCERZ_SEG_NONE && mem_plain_access_ok(op) &&
                 (!insn->mode32 || !ENV_ON("OCERZ_NO_M32_STACK_PLAIN"));
    uint64_t fold = ea_fold();
    int seg = insn->seg;
    if (seg != OCERZ_SEG_NONE) {

        static int no_seg = -1;
        if (no_seg < 0)
            no_seg = getenv("OCERZ_NO_INLINE_SEG") ? 1 : 0;
        static int no_low_seg = -1;
        if (no_low_seg < 0)
            no_low_seg = getenv("OCERZ_NO_LOW_SEG") ? 1 : 0;
        if (no_seg || op->riprel || (insn->addrsize == 4 && !insn->mode32) || (ocerz_low_base != 0 && no_low_seg))
            return 0;
    }
    if (op->riprel) {
        uint64_t ga = (uint64_t)op->disp + fold;
        g_ea_is_const = seg == OCERZ_SEG_NONE;
        g_ea_const = ga;




        if (g_ea_is_const && ocerz_low_base && fold == 0 && ga < OCERZ_LOW_LIMIT && low_guard_fast_ok() &&
            !insn_stack_implicit(insn) && !ENV_ON("OCERZ_NO_RIP_LOWFOLD")) {
            emit_const_lit(b, addr_reg, ga | ocerz_low_base);
            g_ea_lowhoisted = 1;
            g_ea_lowhoisted_reg = addr_reg;
            return 1;
        }
        a64_mov_imm64(b, addr_reg, ga);
        return 1;
    }
    if (insn->addrsize == 4) {
        if (!insn->mode32 || !emit_mem_ea32(b, insn, op, addr_reg))
            return 0;
        if (seg == OCERZ_SEG_FS || seg == OCERZ_SEG_GS) {
            a64_ldr(b, 8, JT0, 20, (uint32_t)(seg == OCERZ_SEG_FS ? offsetof(OcerzCPU, fs_base)
                                                                  : offsetof(OcerzCPU, gs_base)));
            a64_add_reg(b, 1, addr_reg, addr_reg, JT0, 0);
        } else if (seg == OCERZ_SEG_NONE && fold == 0) {
            g_ea_w32 = 1;
            if (op->base == OCERZ_REG_NONE && op->index == OCERZ_REG_NONE) {
                g_ea_is_const = 1;
                g_ea_const = (uint32_t)op->disp;
            }
        }
        return 1;
    }
    if (insn->addrsize != 8)
        return 0;
    if (low_hoist_covers(insn, op)) {
        if (op->disp > 0)      a64_add_imm(b, 1, addr_reg, JMEMBASE, (uint32_t)op->disp);
        else if (op->disp < 0) a64_sub_imm(b, 1, addr_reg, JMEMBASE, (uint32_t)-op->disp);
        else                   a64_mov_reg(b, 1, addr_reg, JMEMBASE);
        g_ea_lowhoisted = 1;
        g_ea_lowhoisted_reg = addr_reg;
        return 1;
    }
    uint64_t initial = (uint64_t)op->disp + fold;
    if (rsp_is_ptr() && op->base == OCERZ_RSP &&
        pin_slot(OCERZ_RSP) >= 0)
        initial = (uint64_t)op->disp;
    if (jgb_usable() && fold == ocerz_guest_base && seg == OCERZ_SEG_NONE &&
        !(rsp_is_ptr() && (op->base == OCERZ_RSP || op->index == OCERZ_RSP)) &&
        op->disp >= -4095 && op->disp <= 4095 &&
        (op->base == OCERZ_REG_NONE || pin_slot(op->base) >= 0) &&
        (op->index == OCERZ_REG_NONE || pin_slot(op->index) >= 0)) {
        int have = 0;
        if (op->base != OCERZ_REG_NONE) {
            a64_add_reg(b, 1, addr_reg, JGB, pin_hreg(pin_slot(op->base)), 0);
            have = 1;
        }
        if (op->index != OCERZ_REG_NONE) {
            a64_add_reg(b, 1, addr_reg, have ? addr_reg : JGB,
                        pin_hreg(pin_slot(op->index)), op->scale & 3);
            have = 1;
        }
        if (!have)
            a64_mov_reg(b, 1, addr_reg, JGB);
        if (op->disp > 0)      a64_add_imm(b, 1, addr_reg, addr_reg, (uint32_t)op->disp);
        else if (op->disp < 0) a64_sub_imm(b, 1, addr_reg, addr_reg, (uint32_t)-op->disp);
        return 1;
    }
    int index_done = 0;
    if (op->base != OCERZ_REG_NONE && pin_slot(op->base) >= 0 &&
        (int64_t)initial >= -4095 && (int64_t)initial <= 4095) {
        int hb = pin_hreg(pin_slot(op->base));
        int xs = op->index != OCERZ_REG_NONE ? pin_slot(op->index) : -1;
        if ((int64_t)initial == 0 && xs >= 0 && !(rsp_is_ptr() && op->index == OCERZ_RSP)) {
            a64_add_reg(b, 1, addr_reg, hb, pin_hreg(xs), op->scale & 3);
            index_done = 1;
        } else if ((int64_t)initial > 0) a64_add_imm(b, 1, addr_reg, hb, (uint32_t)initial);
        else if ((int64_t)initial < 0)   a64_sub_imm(b, 1, addr_reg, hb, (uint32_t)-(int64_t)initial);
        else                             a64_mov_reg(b, 1, addr_reg, hb);
    } else {
        a64_mov_imm64(b, addr_reg, initial);
        if (op->base != OCERZ_REG_NONE) {
            int s = pin_slot(op->base);
            if (s >= 0)
                a64_add_reg(b, 1, addr_reg, addr_reg, pin_hreg(s), 0);
            else {
                emit_gpr_rd(b, 1, JT0, op->base);
                a64_add_reg(b, 1, addr_reg, addr_reg, JT0, 0);
            }
        }
    }
    if (op->index != OCERZ_REG_NONE && !index_done) {
        int s = pin_slot(op->index);
        if (s >= 0 && !(rsp_is_ptr() && op->index == OCERZ_RSP))
            a64_add_reg(b, 1, addr_reg, addr_reg, pin_hreg(s), op->scale & 3);
        else {
            emit_gpr_rd(b, 1, JT0, op->index);
            a64_add_reg(b, 1, addr_reg, addr_reg, JT0, op->scale & 3);
        }
    }
    if (seg == OCERZ_SEG_FS) {
        a64_ldr(b, 8, JT0, 20, (uint32_t)offsetof(OcerzCPU, fs_base));
        a64_add_reg(b, 1, addr_reg, addr_reg, JT0, 0);
    } else if (seg == OCERZ_SEG_GS) {
        a64_ldr(b, 8, JT0, 20, (uint32_t)offsetof(OcerzCPU, gs_base));
        a64_add_reg(b, 1, addr_reg, addr_reg, JT0, 0);
        if (op->disp == 0x58 && op->base == OCERZ_REG_NONE &&
            op->index == OCERZ_REG_NONE && !op->riprel &&
            insn->addrsize == 8 && fold == 0 && ocerz_low_base) {
            a64_lsr_imm(b, 1, JTU, JT0, 32);
            uint32_t *low_gs = a64_label(b);
            a64_cbz(b, 1, JTU, 0);
            a64_add_imm(b, 1, JT0, JT0, 0x30);
            emit_commpage_guard(b, insn, JT0, NULL, NULL);
            emit_add_const(b, JT0, ocerz_guest_base - ea_fold());
            a64_ldr(b, 8, JTT, JT0, 0);
            uint32_t *no_self = a64_label(b);
            a64_cbz(b, 1, JTT, 0);
            a64_add_imm(b, 1, addr_reg, JTT, 0x58);
            a64_patch_cbz(low_gs, a64_label(b));
            a64_patch_cbz(no_self, a64_label(b));
        } else if (op->disp == 0x58 && op->base == OCERZ_REG_NONE &&
            op->index == OCERZ_REG_NONE && !op->riprel &&
            insn->addrsize == 8 && fold == 0) {
            a64_lsr_imm(b, 1, JTU, JT0, 32);
            a64_cbz(b, 1, JTU, 5);
            a64_sub_imm(b, 1, JTT, addr_reg, 0x28);
            a64_ldr(b, 8, JTT, JTT, 0);
            a64_cbz(b, 1, JTT, 2);
            a64_add_imm(b, 1, addr_reg, JTT, 0x58);
        }
    }
    return 1;
}

static int insn_const_addr(const X86Insn *insn, uint64_t *ga)
{
    if (!insn || insn->seg != OCERZ_SEG_NONE)
        return 0;
    for (int i = 0; i < insn->nops; i++) {
        const X86Operand *o = &insn->ops[i];
        if (o->kind != OCERZ_OPK_MEM)
            continue;
        if (o->riprel) { *ga = (uint64_t)o->disp; return 1; }
        if (insn->addrsize == 8 && o->base == OCERZ_REG_NONE && o->index == OCERZ_REG_NONE) {
            *ga = (uint64_t)o->disp;
            return 1;
        }
        return 0;
    }
    return 0;
}

static struct { uint32_t *site, *back; int reg, idx; } g_garm[GUARD_ARMS_MAX];

int g_n_garm;

int low_guard_fast_ok(void)
{
    static int ok = -1;
    if (ok < 0 && ocerz_low_base) {
        ok = 0;
        if (!getenv("OCERZ_NO_FAST_LOW_GUARD") && (ocerz_low_base & ((1ull << 34) - 1)) == 0) {
            uint32_t w[2];
            A64Buf t = { w, w, w + 2, 0, 0 };
            ok = a64_try_orr_imm(&t, 1, 1, 1, ocerz_low_base);
        }
    }
    return ok > 0;
}

int lowstack_delta_ok(void)
{
    static int off = -1;
    if (off < 0) off = getenv("OCERZ_NO_LOW_STACK_DELTA") ? 1 : 0;
    return !off && ocerz_low_base != 0 && ocerz_guest_base == 0 && g_pin_class == 3 &&
           !g_xlat_mode32 && pin_slot(OCERZ_RSP) >= 0 && rsp_is_ptr() && low_guard_fast_ok();
}

static int rsp_small_adjust(const X86Insn *in)
{
    const X86Operand *d = &in->ops[0], *s = &in->ops[1];
    if (in->nops != 2 || d->kind != OCERZ_OPK_REG || (d->reg & 15) != OCERZ_RSP || d->size != 8)
        return 0;
    if (in->op == OCERZ_OP_ADD || in->op == OCERZ_OP_SUB)
        return s->kind == OCERZ_OPK_IMM && (int64_t)s->imm > -65536 && (int64_t)s->imm < 65536;
    if (in->op == OCERZ_OP_LEA)
        return s->kind == OCERZ_OPK_MEM && !s->riprel && s->base == OCERZ_RSP && s->index == OCERZ_REG_NONE &&
               s->disp > -65536 && s->disp < 65536;
    return 0;
}

int lowstack_disturbs(const X86Insn *in)
{
    switch (in->op) {
    case OCERZ_OP_PUSH: case OCERZ_OP_CALL: case OCERZ_OP_RET:
        return 0;
    case OCERZ_OP_ADD: case OCERZ_OP_SUB: case OCERZ_OP_LEA:
        if (rsp_small_adjust(in) && !ENV_ON("OCERZ_LOWSTACK_ADJ_RECOMPUTE")) return 0;
        return insn_may_write_gpr(in, OCERZ_RSP);
    case OCERZ_OP_POP:
        return in->nops > 0 && in->ops[0].kind == OCERZ_OPK_REG && (in->ops[0].reg & 15) == OCERZ_RSP;
    default:
        return insn_may_write_gpr(in, OCERZ_RSP);
    }
}

static int insn_stack_implicit(const X86Insn *in)
{
    switch (in->op) {
    case OCERZ_OP_PUSH: case OCERZ_OP_POP: case OCERZ_OP_CALL: case OCERZ_OP_RET:
    case OCERZ_OP_LEAVE: case OCERZ_OP_PUSHF: case OCERZ_OP_POPF:
        return 1;
    default:
        return 0;
    }
}

static int insn_stack_only(const X86Insn *in)
{
    if (in->seg != OCERZ_SEG_NONE || in->mode32 || in->addrsize != 8)
        return 0;
    int implicit = 0;
    switch (in->op) {
    case OCERZ_OP_PUSH: case OCERZ_OP_POP: case OCERZ_OP_CALL: case OCERZ_OP_RET:
    case OCERZ_OP_LEAVE: case OCERZ_OP_PUSHF: case OCERZ_OP_POPF:
        implicit = 1;
        break;
    case OCERZ_OP_MOVS: case OCERZ_OP_STOS: case OCERZ_OP_LODS: case OCERZ_OP_SCAS: case OCERZ_OP_CMPS:
        return 0;
    default:
        break;
    }
    int mem = 0;
    for (int k = 0; k < in->nops; k++) {
        const X86Operand *o = &in->ops[k];
        if (o->kind != OCERZ_OPK_MEM)
            continue;
        if (o->riprel || (o->base & 15) != OCERZ_RSP || o->base == OCERZ_REG_NONE ||
            o->index != OCERZ_REG_NONE || o->disp >= (1 << 20) || o->disp <= -(1 << 20))
            return 0;
        mem = 1;
    }
    return implicit || mem;
}

uint32_t *emit_commpage_guard(A64Buf *b, const X86Insn *insn,
                                     int addr_reg, uint32_t **exit_sites, int *n_exits)
{
    (void)exit_sites; (void)n_exits;
    g_const_ea_valid = 0;
    if (g_ea_lowhoisted && addr_reg == g_ea_lowhoisted_reg) {
        g_ea_lowhoisted = 0;
        if (g_ea_is_const) {
            g_const_ea = g_ea_const;
            g_const_ea_valid = 1;
        }
        g_ea_is_const = 0;
        return NULL;
    }
    if (g_lowstack && insn && insn_stack_only(insn)) {
        g_ea_is_const = 0;
        a64_add_reg(b, 1, addr_reg, addr_reg, JGB, 0);
        return NULL;
    }







    if (g_ea_is_const && ocerz_low_base && ea_fold() == 0 && low_guard_fast_ok() && insn &&
        !insn_stack_implicit(insn)) {
        uint64_t ga = g_ea_const;
        g_ea_is_const = 0;
        if (ga < OCERZ_LOW_LIMIT) {
            (void)a64_try_orr_imm(b, 1, addr_reg, addr_reg, ocerz_low_base);
            g_const_ea = ga;
            g_const_ea_valid = 1;
            return NULL;
        }
        if (ga < OCERZ_TOP_LO && !(ocerz_commpage && ga >= OCERZ_COMMPAGE_LO && ga < OCERZ_COMMPAGE_HI)) {
            g_const_ea = ga;
            g_const_ea_valid = 1;
            return NULL;
        }
    }
    g_ea_is_const = 0;






    if (insn && insn->mode32 && insn->addrsize == 4 && insn->seg == OCERZ_SEG_NONE && ocerz_low_base &&
        ea_fold() == 0 && low_guard_fast_ok() && !ENV_ON("OCERZ_NO_M32_ORR")) {
        if (!g_ea_w32) a64_mov_reg(b, 0, addr_reg, addr_reg);
        g_ea_w32 = 0;
        (void)a64_try_orr_imm(b, 1, addr_reg, addr_reg, ocerz_low_base);
        return NULL;
    }
    g_ea_w32 = 0;
    if (!ocerz_commpage && !ocerz_low_base)
        return NULL;
    uint64_t ga;
    if (!ocerz_low_base && insn_const_addr(insn, &ga)) {
        if (ga >= OCERZ_COMMPAGE_LO && ga < OCERZ_COMMPAGE_HI) {
            tc_imm64(b, JTU, TCR_COMMPAGE, 0, (uint64_t)(uintptr_t)ocerz_commpage - OCERZ_COMMPAGE_LO - ocerz_guest_base);
            a64_add_reg(b, 1, addr_reg, addr_reg, JTU, 0);
            return NULL;
        }
        g_const_ea = ga;
        g_const_ea_valid = 1;
        return NULL;
    }
    if (ocerz_low_base && ea_fold() == 0 && low_guard_fast_ok()) {
        a64_lsr_imm(b, 1, JTT, addr_reg, 32);
        a64_subs_imm(b, 1, A64_ZR, JTT, (uint32_t)(OCERZ_LOW_LIMIT >> 32));
        uint32_t *high = a64_label(b);
        a64_bcond(b, A64_CS, 0);
        (void)a64_try_orr_imm(b, 1, addr_reg, addr_reg, ocerz_low_base);
        if (!g_low_top) {
            a64_patch_bcond(high, a64_label(b));
            return NULL;
        }
        uint32_t *done_low = a64_label(b);
        a64_b(b, 0);
        a64_patch_bcond(high, a64_label(b));
        a64_lsr_imm(b, 1, JTT, addr_reg, 25);
        a64_add_imm(b, 1, JTT, JTT, 1);
        a64_lsr_imm(b, 1, JTT, JTT, 22);
        if (g_n_garm < GUARD_ARMS_MAX) {
            g_garm[g_n_garm].site = a64_label(b);
            a64_cbnz(b, 1, JTT, 0);
            g_garm[g_n_garm].back = a64_label(b);
            g_garm[g_n_garm].reg = addr_reg;
            g_garm[g_n_garm].idx = g_cur_insn_idx;
            g_n_garm++;
            a64_patch_b(done_low, a64_label(b));
            return NULL;
        }
        uint32_t *identity = a64_label(b);
        a64_cbz(b, 1, JTT, 0);
        emit_guard_full(b, addr_reg);
        uint32_t *done_full = a64_label(b);
        a64_b(b, 0);
        a64_patch_cbz(identity, a64_label(b));
        a64_patch_b(done_low, a64_label(b));
        a64_patch_b(done_full, a64_label(b));
        return NULL;
    }
    emit_guard_full(b, addr_reg);
    return NULL;
}

static void emit_guard_full(A64Buf *b, int addr_reg)
{
    uint64_t fold = ea_fold();
    uint32_t *to_native = NULL;
    if (ocerz_low_base) {
        a64_mov_imm64(b, JTU, OCERZ_LOW_LIMIT + fold);
        a64_sub_reg(b, 1, JTT, addr_reg, JTU, 0);
        a64_mov_imm64(b, JTU, OCERZ_TOP_LO - OCERZ_LOW_LIMIT);
        a64_subs_reg(b, 1, A64_ZR, JTT, JTU, 0);
        to_native = a64_label(b);
        a64_bcond(b, A64_CC, 0);
    }
    uint32_t *done_cp = NULL;
    if (ocerz_commpage) {
        a64_mov_imm64(b, JTU, OCERZ_COMMPAGE_LO + fold);
        a64_sub_reg(b, 1, JTT, addr_reg, JTU, 0);
        a64_mov_imm64(b, JTU, OCERZ_COMMPAGE_HI - OCERZ_COMMPAGE_LO);
        a64_subs_reg(b, 1, A64_ZR, JTT, JTU, 0);
        uint32_t *not_cp = a64_label(b);
        a64_bcond(b, A64_CS, 0);
        tc_imm64(b, JTU, TCR_COMMPAGE, 0, (uint64_t)(uintptr_t)ocerz_commpage - OCERZ_COMMPAGE_LO - ocerz_guest_base);
        a64_add_reg(b, 1, addr_reg, addr_reg, JTU, 0);
        done_cp = a64_label(b);
        a64_b(b, 0);
        a64_patch_bcond(not_cp, a64_label(b));
    }
    if (ocerz_low_base) {
        a64_mov_imm64(b, JTU, OCERZ_TOP_LO + fold);
        a64_subs_reg(b, 1, A64_ZR, addr_reg, JTU, 0);
        uint32_t *is_low = a64_label(b);
        a64_bcond(b, A64_CC, 0);
        a64_mov_imm64(b, JTU, ocerz_top_base - OCERZ_TOP_LO - ocerz_guest_base);
        a64_add_reg(b, 1, addr_reg, addr_reg, JTU, 0);
        uint32_t *done_top = a64_label(b);
        a64_b(b, 0);
        a64_patch_bcond(is_low, a64_label(b));
        a64_mov_imm64(b, JTU, ocerz_low_base - ocerz_guest_base);
        a64_add_reg(b, 1, addr_reg, addr_reg, JTU, 0);
        a64_patch_b(done_top, a64_label(b));
    }
    if (done_cp) a64_patch_b(done_cp, a64_label(b));
    if (to_native) a64_patch_bcond(to_native, a64_label(b));
}

void emit_reload_mem_base(A64Buf *b)
{
    if (g_low_hoist_greg >= 0)
        (void)a64_try_orr_imm(b, 1, JMEMBASE, pin_hreg(pin_slot(g_low_hoist_greg)), ocerz_low_base);
    if (g_mem_hoist_greg < 0)
        return;
    int bs = pin_slot(g_mem_hoist_greg);
    assert(bs >= 0);
    if (jgb_usable()) {
        a64_add_reg(b, 1, JMEMBASE, JGB, pin_hreg(bs), 0);
    } else {
        a64_mov_imm64(b, JMEMBASE, ocerz_guest_base);
        a64_add_reg(b, 1, JMEMBASE, JMEMBASE, pin_hreg(bs), 0);
    }
    if (g_mem_hoist_aux_index >= 0)
        a64_add_reg(b, 1, JMEMAUX, JMEMBASE, pin_hreg(pin_slot(g_mem_hoist_aux_index)), g_mem_hoist_aux_scale);
    else if (g_mem_hoist_aux_disp > 0)
        a64_add_imm(b, 1, JMEMAUX, JMEMBASE,
                    (uint32_t)g_mem_hoist_aux_disp);
    else if (g_mem_hoist_aux_disp < 0)
        a64_sub_imm(b, 1, JMEMAUX, JMEMBASE,
                    (uint32_t)-g_mem_hoist_aux_disp);
    if (g_mem_hoist_greg2 >= 0) {
        int bs2 = pin_slot(g_mem_hoist_greg2);
        assert(bs2 >= 0);
        if (jgb_usable()) a64_add_reg(b, 1, JMEMBASE2, JGB, pin_hreg(bs2), 0);
        else { a64_mov_imm64(b, JMEMBASE2, ocerz_guest_base); a64_add_reg(b, 1, JMEMBASE2, JMEMBASE2, pin_hreg(bs2), 0); }
    }
    if (g_mem_hoist_greg3 >= 0) {
        int bs3 = pin_slot(g_mem_hoist_greg3);
        assert(bs3 >= 0);
        if (jgb_usable()) a64_add_reg(b, 1, JMEMBASE3, JGB, pin_hreg(bs3), 0);
        else { a64_mov_imm64(b, JMEMBASE3, ocerz_guest_base); a64_add_reg(b, 1, JMEMBASE3, JMEMBASE3, pin_hreg(bs3), 0); }
    }
}

static int emit_hoisted_mem_access(A64Buf *b, const X86Insn *insn,
                                   const X86Operand *mem, int size,
                                   int value_reg, int store)
{
    if (g_mem_hoist_greg < 0 ||
        insn->seg != OCERZ_SEG_NONE || insn->addrsize != 8 || mem->riprel ||
        mem->base == OCERZ_REG_NONE)
        return 0;
    int plain = mem_plain_access_ok(mem);
    X86Operand mview; mem = mem_hoist_view(mem, &mview);
    if (rsp_is_ptr() && (mem->base == OCERZ_RSP || mem->index == OCERZ_RSP))
        return 0;
    int hbase = hoist_reg_for(mem->base);
    if (hbase < 0)
        return 0;

    int64_t disp = mem->disp;
    if (mem->index == OCERZ_REG_NONE) {
        if (disp < 0 || (uint64_t)disp > (uint64_t)4095 * (uint64_t)size ||
            (disp & (size - 1)) != 0)
            return 0;
        if (store) emit_gpr_st_at(b, size, value_reg, hbase, (int32_t)disp, plain);
        else       emit_gpr_ld_at(b, size, value_reg, hbase, (int32_t)disp, plain);
        return 1;
    }

    int is = pin_slot(mem->index);
    int want_scale = size == 8 ? 3 : size == 4 ? 2 :
                     size == 2 ? 1 : 0;
    if (is < 0 || (mem->scale & 3) != want_scale ||
        disp < -4095 || disp > 4095)
        return 0;
    if (!plain) {
        int ra; uint32_t d;
        if (emit_mem_ea_plain_ex(b, insn, mem, size, &ra, &d, 1)) {
            if (store) emit_gpr_st_at(b, size, value_reg, ra, (int32_t)d, 0);
            else       emit_gpr_ld_at(b, size, value_reg, ra, (int32_t)d, 0);
            return 1;
        }
    }
    int base = hbase;
    if (hbase == JMEMBASE && g_mem_hoist_aux_index >= 0 && mem->index == (unsigned)g_mem_hoist_aux_index &&
        (mem->scale & 3) == g_mem_hoist_aux_scale) {
        if (disp < 0 || (uint64_t)disp > (uint64_t)4095 * (uint64_t)size || (disp & (size - 1)) != 0)
            return 0;
        if (store) emit_gpr_st_at(b, size, value_reg, JMEMAUX, (int32_t)disp, plain);
        else       emit_gpr_ld_at(b, size, value_reg, JMEMAUX, (int32_t)disp, plain);
        return 1;
    }
    if (disp != 0 && disp == g_mem_hoist_aux_disp && g_mem_hoist_aux_index < 0 && hbase == JMEMBASE) {
        base = JMEMAUX;
    } else if (disp != 0) {
        if (disp > 0)
            a64_add_imm(b, 1, JTA, hbase, (uint32_t)disp);
        else
            a64_sub_imm(b, 1, JTA, hbase, (uint32_t)-disp);
        base = JTA;
    }
    if (store) emit_gpr_st_regoff(b, size, value_reg, base, pin_hreg(is), 1, plain);
    else       emit_gpr_ld_regoff(b, size, value_reg, base, pin_hreg(is), 1, plain);
    return 1;
}

static int align_test_all(void)
{
    static int on = -1;
    if (on < 0) on = getenv("OCERZ_ALIGN_TEST_ALL") ? 1 : 0;
    return on;
}

static void emit_granule_cross_test(A64Buf *b, int size, int ra, int scratch)
{
    a64_add_imm(b, 1, scratch, ra, (uint32_t)(size - 1));
    a64_eor_reg(b, 1, scratch, scratch, ra, 0);
    a64_try_ands_imm(b, 1, A64_ZR, scratch, 16);
}

void emit_guest_store_ordered(A64Buf *b, int size, int rv, int ra, int scratch)
{
    if (g_plain_mem || g_ea_plain) {
        a64_str(b, size, rv, ra, 0);
        return;
    }

    if (size == 1) {
        a64_stlr(b, 1, rv, ra);
        return;
    }
    if (g_const_ea_valid) {
        int aligned = (g_const_ea & (uint64_t)(size - 1)) == 0;
        g_const_ea_valid = 0;
        if (aligned) {
            a64_stlr(b, size, rv, ra);
            return;
        }
    }
    if (!g_align_guard && !align_test_all()) {
        a64_stlur(b, size, rv, ra, 0);
        return;
    }
    emit_granule_cross_test(b, size, ra, scratch);
    if (!g_no_oolslow && g_n_oslow < OSLOW_MAX) {
        uint32_t *bne = a64_label(b);
        a64_bcond(b, A64_NE, 0);
        a64_stlr(b, size, rv, ra);
        g_oslow[g_n_oslow] = (OrderedSlowPend){ bne, a64_label(b), size, rv, ra, 1,
                                                g_cur_insn_idx, 0, 0 };
        g_n_oslow++;
        ea_cache_reset();
        return;
    }
    uint32_t *to_aligned = a64_label(b);
    a64_bcond(b, A64_EQ, 0);
    a64_dmb_ish(b);
    a64_str(b, size, rv, ra, 0);
    uint32_t *to_done = a64_label(b);
    a64_b(b, 0);
    a64_patch_bcond(to_aligned, a64_label(b));
    a64_stlr(b, size, rv, ra);
    a64_patch_b(to_done, a64_label(b));
}

void emit_guest_load_ordered(A64Buf *b, int size, int rd, int ra, int scratch)
{
    if (g_plain_mem || g_ea_plain) {
        a64_ldr(b, size, rd, ra, 0);
        return;
    }
    g_blk_ordered_loads = 1;
    if (size == 1) {
        if (g_no_ldapr) a64_ldar(b, 1, rd, ra);
        else            a64_ldapr(b, 1, rd, ra);
        return;
    }
    if (g_const_ea_valid) {
        int aligned = (g_const_ea & (uint64_t)(size - 1)) == 0;
        g_const_ea_valid = 0;
        if (aligned) {
            if (g_no_ldapr) a64_ldar(b, size, rd, ra);
            else            a64_ldapr(b, size, rd, ra);
            return;
        }
    }
    if (!g_align_guard && !g_no_ldapr && !align_test_all()) {
        a64_ldapur(b, size, rd, ra, 0);
        return;
    }
    emit_granule_cross_test(b, size, ra, scratch);
    if (!g_no_oolslow && g_n_oslow < OSLOW_MAX) {
        uint32_t *bne = a64_label(b);
        a64_bcond(b, A64_NE, 0);
        if (g_no_ldapr) a64_ldar(b, size, rd, ra);
        else            a64_ldapr(b, size, rd, ra);
        g_oslow[g_n_oslow] = (OrderedSlowPend){ bne, a64_label(b), size, rd, ra, 0,
                                                g_cur_insn_idx, 0, 0 };
        ea_cache_reset();
        g_n_oslow++;
        return;
    }
    uint32_t *to_aligned = a64_label(b);
    a64_bcond(b, A64_EQ, 0);
    a64_ldr(b, size, rd, ra, 0);
    a64_dmb_ish(b);
    uint32_t *to_done = a64_label(b);
    a64_b(b, 0);
    a64_patch_bcond(to_aligned, a64_label(b));

    if (g_no_ldapr) a64_ldar(b, size, rd, ra);
    else            a64_ldapr(b, size, rd, ra);
    a64_patch_b(to_done, a64_label(b));
}

void emit_gpr_ld_at(A64Buf *b, int size, int rd, int ra, int32_t disp, int plain)
{
    int scaled = disp >= 0 && (disp % size) == 0 && disp / size <= 4095;
    if (!plain) g_blk_ordered_loads = 1;
    if (plain) {
        if (scaled) a64_ldr(b, size, rd, ra, (uint32_t)disp);
        else if (disp >= -256 && disp <= 255) a64_ldur(b, size, rd, ra, disp);
        else { a64_mov_imm64(b, JTU, (uint64_t)(int64_t)disp); a64_add_reg(b, 1, JTA, ra, JTU, 0); a64_ldr(b, size, rd, JTA, 0); }
        return;
    }
    if (!g_align_guard || size == 1) {
        if (disp >= -256 && disp <= 255) { a64_ldapur(b, size, rd, ra, disp); return; }
        if (scaled && disp <= 4095) a64_add_imm(b, 1, JTA, ra, (uint32_t)disp);
        else { a64_mov_imm64(b, JTU, (uint64_t)(int64_t)disp); a64_add_reg(b, 1, JTA, ra, JTU, 0); }
        a64_ldapur(b, size, rd, JTA, 0);
        return;
    }
    if (disp == 0) { emit_guest_load_ordered(b, size, rd, ra, JTU); return; }
    if (disp > 0 && disp <= 4095) a64_add_imm(b, 1, JTA, ra, (uint32_t)disp);
    else if (disp < 0 && -disp <= 4095) a64_sub_imm(b, 1, JTA, ra, (uint32_t)-disp);
    else { a64_mov_imm64(b, JTU, (uint64_t)(int64_t)disp); a64_add_reg(b, 1, JTA, ra, JTU, 0); }
    emit_guest_load_ordered(b, size, rd, JTA, JTU);
}

void emit_gpr_lds_at(A64Buf *b, int size, int sf, int rd, int ra, int32_t disp)
{
    if (g_no_ldapr || (g_align_guard && size != 1)) {
        emit_gpr_ld_at(b, size, JT1, ra, disp, 0);
        if (size == 1)      a64_sxtb(b, sf, rd, JT1);
        else if (size == 2) a64_sxth(b, sf, rd, JT1);
        else                a64_sxtw(b, rd, JT1);
        return;
    }
    g_blk_ordered_loads = 1;
    if (!(disp >= -256 && disp <= 255)) {
        if (disp > 0 && disp <= 4095) a64_add_imm(b, 1, JTA, ra, (uint32_t)disp);
        else { a64_mov_imm64(b, JTU, (uint64_t)(int64_t)disp); a64_add_reg(b, 1, JTA, ra, JTU, 0); }
        ra = JTA;
        disp = 0;
    }
    a64_ldapurs(b, size, sf, rd, ra, disp);
}

void emit_gpr_st_at(A64Buf *b, int size, int rv, int ra, int32_t disp, int plain)
{
    int scaled = disp >= 0 && (disp % size) == 0 && disp / size <= 4095;
    if (plain) {
        if (scaled) a64_str(b, size, rv, ra, (uint32_t)disp);
        else if (disp >= -256 && disp <= 255) a64_stur(b, size, rv, ra, disp);
        else { a64_mov_imm64(b, JTU, (uint64_t)(int64_t)disp); a64_add_reg(b, 1, JTA, ra, JTU, 0); a64_str(b, size, rv, JTA, 0); }
        return;
    }
    if (!g_align_guard || size == 1) {
        if (disp >= -256 && disp <= 255) { a64_stlur(b, size, rv, ra, disp); return; }
        if (scaled && disp <= 4095) a64_add_imm(b, 1, JTA, ra, (uint32_t)disp);
        else { a64_mov_imm64(b, JTU, (uint64_t)(int64_t)disp); a64_add_reg(b, 1, JTA, ra, JTU, 0); }
        a64_stlur(b, size, rv, JTA, 0);
        return;
    }
    if (disp == 0) { emit_guest_store_ordered(b, size, rv, ra, JTU); return; }
    if (disp > 0 && disp <= 4095) a64_add_imm(b, 1, JTA, ra, (uint32_t)disp);
    else if (disp < 0 && -disp <= 4095) a64_sub_imm(b, 1, JTA, ra, (uint32_t)-disp);
    else { a64_mov_imm64(b, JTU, (uint64_t)(int64_t)disp); a64_add_reg(b, 1, JTA, ra, JTU, 0); }
    emit_guest_store_ordered(b, size, rv, JTA, JTU);
}

static void emit_gpr_ld_regoff(A64Buf *b, int size, int rd, int ra, int ri, int scaled, int plain)
{
    if (plain) { a64_ldr_regoff(b, size, rd, ra, ri, scaled); return; }
    int sh = scaled ? (size == 8 ? 3 : size == 4 ? 2 : size == 2 ? 1 : 0) : 0;
    a64_add_reg(b, 1, JTA, ra, ri, sh);
    emit_gpr_ld_at(b, size, rd, JTA, 0, 0);
}

static void emit_gpr_st_regoff(A64Buf *b, int size, int rv, int ra, int ri, int scaled, int plain)
{
    if (plain) { a64_str_regoff(b, size, rv, ra, ri, scaled); return; }
    int sh = scaled ? (size == 8 ? 3 : size == 4 ? 2 : size == 2 ? 1 : 0) : 0;
    a64_add_reg(b, 1, JTA, ra, ri, sh);
    emit_gpr_st_at(b, size, rv, JTA, 0, 0);
}

static void emit_v_st_ordered_fast(A64Buf *b, int size, int vs, int ra, int32_t disp)
{
    if (size == 16) {
        a64_fmov_x_from_v(b, 1, JT0, vs);
        a64_umov_gpr(b, 8, JTU, vs, 1);
        a64_stlur(b, 8, JT0, ra, disp);
        a64_stlur(b, 8, JTU, ra, disp + 8);
    } else if (size == 8) {
        a64_fmov_x_from_v(b, 1, JT0, vs);
        a64_stlur(b, 8, JT0, ra, disp);
    } else {
        a64_fmov_x_from_v(b, 0, JT0, vs);
        a64_stlur(b, 4, JT0, ra, disp);
    }
}

static void emit_v_acc_ordered_checked(A64Buf *b, int size, int vr, int ra, int32_t disp, int store)
{
    if (disp != 0) {
        if (disp > 0 && disp <= 4095) a64_add_imm(b, 1, JTA, ra, (uint32_t)disp);
        else if (disp < 0 && -disp <= 4095) a64_sub_imm(b, 1, JTA, ra, (uint32_t)-disp);
        else { a64_mov_imm64(b, JTU, (uint64_t)(int64_t)disp); a64_add_reg(b, 1, JTA, ra, JTU, 0); }
        ra = JTA;
    }
    if (size == 16) a64_try_ands_imm(b, 1, A64_ZR, ra, 7);
    else emit_granule_cross_test(b, size, ra, JTU);
    (void)store;
    if (!g_no_oolslow && g_n_oslow < OSLOW_MAX) {
        uint32_t *bne = a64_label(b);
        a64_bcond(b, A64_NE, 0);
        emit_v_st_ordered_fast(b, size, vr, ra, 0);
        g_oslow[g_n_oslow] = (OrderedSlowPend){ bne, a64_label(b), size, vr, ra, 1,
                                                g_cur_insn_idx, 1, 0 };
        g_n_oslow++;
        ea_cache_reset();
        return;
    }
    uint32_t *to_aligned = a64_label(b);
    a64_bcond(b, A64_EQ, 0);
    a64_dmb_ish(b); a64_str_v(b, size, vr, ra, 0);
    uint32_t *to_done = a64_label(b);
    a64_b(b, 0);
    a64_patch_bcond(to_aligned, a64_label(b));
    emit_v_st_ordered_fast(b, size, vr, ra, 0);
    a64_patch_b(to_done, a64_label(b));
}

int g_undo_saved;

int g_undo_want_size;

int g_undo_want_slot = -1;

void emit_v_ld_at_(A64Buf *b, int size, int vd, int ra, int32_t disp, int plain)
{
    int scaled = disp >= 0 && (disp % size) == 0 && disp / size <= 4095;
    if (!plain && vec_plain_size(size)) plain = 1;
    if (!plain) g_blk_ordered_loads = 1;
    if (plain) {
        if (scaled) a64_ldr_v(b, size, vd, ra, (uint32_t)disp);
        else if (disp >= -256 && disp <= 255) a64_ldur_v(b, size, vd, ra, disp);
        else { a64_mov_imm64(b, JTU, (uint64_t)(int64_t)disp); a64_add_reg(b, 1, JTA, ra, JTU, 0); a64_ldr_v(b, size, vd, JTA, 0); }
        return;
    }
    if (disp >= -256 && disp <= 255) {
        if (scaled) a64_ldr_v(b, size, vd, ra, (uint32_t)disp); else a64_ldur_v(b, size, vd, ra, disp);
        a64_ldapur(b, 1, JTU, ra, disp);
    } else {
        if (disp > 0 && disp <= 4095) a64_add_imm(b, 1, JTA, ra, (uint32_t)disp);
        else if (disp < 0 && -disp <= 4095) a64_sub_imm(b, 1, JTA, ra, (uint32_t)-disp);
        else { a64_mov_imm64(b, JTU, (uint64_t)(int64_t)disp); a64_add_reg(b, 1, JTA, ra, JTU, 0); }
        a64_ldr_v(b, size, vd, JTA, 0);
        a64_ldapur(b, 1, JTU, JTA, 0);
    }
}

void emit_v_st_at(A64Buf *b, int size, int vs, int ra, int32_t disp, int plain)
{
    int scaled = disp >= 0 && (disp % size) == 0 && disp / size <= 4095;
    if (!plain && vec_plain_size(size)) plain = 1;
    if (plain) {
        if (scaled) a64_str_v(b, size, vs, ra, (uint32_t)disp);
        else if (disp >= -256 && disp <= 255) a64_stur_v(b, size, vs, ra, disp);
        else { a64_mov_imm64(b, JTU, (uint64_t)(int64_t)disp); a64_add_reg(b, 1, JTA, ra, JTU, 0); a64_str_v(b, size, vs, JTA, 0); }
        return;
    }
    if (g_align_guard) { emit_v_acc_ordered_checked(b, size, vs, ra, disp, 1); return; }
    if (!(disp >= -256 && disp + (size == 16 ? 8 : 0) <= 255)) {
        a64_mov_imm64(b, JTU, (uint64_t)(int64_t)disp); a64_add_reg(b, 1, JTA, ra, JTU, 0); ra = JTA; disp = 0;
    }
    emit_v_st_ordered_fast(b, size, vs, ra, disp);
}

static void emit_v_ld_regoff(A64Buf *b, int size, int vd, int ra, int ri, int scaled, int plain)
{
    if (plain || vec_plain_size(size)) { a64_ldr_v_regoff(b, size, vd, ra, ri, scaled); undo_save_hook(b, size, vd); return; }
    int sh = scaled ? (size == 16 ? 4 : size == 8 ? 3 : 2) : 0;
    a64_add_reg(b, 1, JTA, ra, ri, sh);
    emit_v_ld_at(b, size, vd, JTA, 0, 0);
}

static void emit_v_st_regoff(A64Buf *b, int size, int vs, int ra, int ri, int scaled, int plain)
{
    if (plain || vec_plain_size(size)) { a64_str_v_regoff(b, size, vs, ra, ri, scaled); return; }
    int sh = scaled ? (size == 16 ? 4 : size == 8 ? 3 : 2) : 0;
    a64_add_reg(b, 1, JTA, ra, ri, sh);
    emit_v_st_at(b, size, vs, JTA, 0, 0);
}

int lowstack_disp_ok(const X86Insn *insn, const X86Operand *m, int size, int unscaled_ok)
{
    if (!g_lowstack || m->base != OCERZ_RSP || m->index != OCERZ_REG_NONE || m->riprel) return 0;
    if (!insn_stack_only(insn) || ENV_ON("OCERZ_NO_LOWSTACK_EA")) return 0;
    int64_t d = m->disp;
    return (d >= 0 && (d % size) == 0 && d / size <= 4095) || (unscaled_ok && d >= -256 && d <= 255);
}

int emit_plain_mem_fast(A64Buf *b, const X86Insn *insn, const X86Operand *m,
                               int size, int reg, int store, int vec)
{
    static int dis = -1; if (dis < 0) dis = getenv("OCERZ_NO_PLAINFAST") ? 1 : 0;
    if (dis) return 0;
    if (low_hoist_covers(insn, m) && !ENV_ON("OCERZ_NO_HOIST_DISP")) {
        int plain = mem_plain_access_ok(m);
        if (vec) { if (store) emit_v_st_at(b, size, reg, JMEMBASE, (int32_t)m->disp, plain); else emit_v_ld_at(b, size, reg, JMEMBASE, (int32_t)m->disp, plain); }
        else     { if (store) emit_gpr_st_at(b, size, reg, JMEMBASE, (int32_t)m->disp, plain); else emit_gpr_ld_at(b, size, reg, JMEMBASE, (int32_t)m->disp, plain); }
        return 1;
    }
    if (lowstack_disp_ea(b, insn, m, size, 1)) {
        int plain = mem_plain_access_ok(m);
        if (vec) { if (store) emit_v_st_at(b, size, reg, JTA, (int32_t)m->disp, plain); else emit_v_ld_at(b, size, reg, JTA, (int32_t)m->disp, plain); }
        else     { if (store) emit_gpr_st_at(b, size, reg, JTA, (int32_t)m->disp, plain); else emit_gpr_ld_at(b, size, reg, JTA, (int32_t)m->disp, plain); }
        return 1;
    }
    if (!mem_fast_forms_ok()) return 0;
    if (insn->seg != OCERZ_SEG_NONE || insn->addrsize != 8 || m->riprel) return 0;
    if (m->base == OCERZ_REG_NONE || pin_slot(m->base) < 0) return 0;
    if (rsp_is_ptr() && m->index == OCERZ_RSP) return 0;
    int plain = mem_plain_access_ok(m);
    X86Operand mview; m = mem_hoist_view(m, &mview);
    int hb = pin_hreg(pin_slot(m->base));
    int hreg = hoist_reg_for(m->base);
    int hoisted = hreg >= 0;
#define ACC_AT(ra, d)  do { if (vec) { if (store) emit_v_st_at(b, size, reg, (ra), (int32_t)(d), plain); else emit_v_ld_at(b, size, reg, (ra), (int32_t)(d), plain); } \
                            else     { if (store) emit_gpr_st_at(b, size, reg, (ra), (int32_t)(d), plain); else emit_gpr_ld_at(b, size, reg, (ra), (int32_t)(d), plain); } } while (0)
#define ACC_REGOFF(ra, ri, sc) do { if (vec) { if (store) emit_v_st_regoff(b, size, reg, (ra), (ri), (sc), plain); else emit_v_ld_regoff(b, size, reg, (ra), (ri), (sc), plain); } \
                                    else     { if (store) emit_gpr_st_regoff(b, size, reg, (ra), (ri), (sc), plain); else emit_gpr_ld_regoff(b, size, reg, (ra), (ri), (sc), plain); } } while (0)
    if (rsp_is_ptr() && m->base == OCERZ_RSP) {
        if (m->index != OCERZ_REG_NONE) return 0;
        int scaled = m->disp >= 0 && (m->disp % size) == 0 && m->disp / size <= 4095;
        int unscaled = !scaled && m->disp >= -256 && m->disp <= 255;
        if (!scaled && !unscaled) return 0;
        ACC_AT(pin_hreg(pin_slot(OCERZ_RSP)), m->disp);
        return 1;
    }
    if (m->index != OCERZ_REG_NONE) {
        if (pin_slot(m->index) < 0) return 0;
        int hi = pin_hreg(pin_slot(m->index));
        int sc = m->scale & 3;
        int want = size == 16 ? 4 : size == 8 ? 3 : size == 4 ? 2 : size == 2 ? 1 : 0;
        if (hoisted && hreg == JMEMBASE && g_mem_hoist_aux_index >= 0 && m->index == (unsigned)g_mem_hoist_aux_index &&
            sc == g_mem_hoist_aux_scale && m->disp >= 0 && (m->disp % size) == 0 && m->disp / size <= 4095) {
            ACC_AT(JMEMAUX, m->disp);
            return 1;
        }
        if ((m->disp == 0 || (hoisted && hreg == JMEMBASE && g_mem_hoist_aux_index < 0 && m->disp == g_mem_hoist_aux_disp && m->disp != 0)) &&
            (sc == 0 || sc == want)) {
            int ra = JTA;
            if (hoisted) ra = m->disp ? JMEMAUX : hreg;
            else if (ea_cache_reusable(b, m)) {
                ACC_AT(JTA, 0);
                return 1;
            }
            else { if (!ea_cache_has_base(b, m)) a64_add_reg(b, 1, JTA, JGB, hb, 0); ea_cache_set_full(b, m->base, OCERZ_REG_NONE, 0); }
            ACC_REGOFF(ra, hi, sc != 0);
            return 1;
        }
        if (m->disp == 0 && sc != 0) {
            if (!hoisted) {
                if (ea_cache_reusable(b, m)) { ACC_AT(JTA, 0); return 1; }
                if (ea_cache_has_base(b, m)) {
                    a64_add_reg(b, 1, JTA, JTA, hi, sc);
                    ea_cache_set(b, m);
                    ACC_AT(JTA, 0);
                    return 1;
                }
                if (plain) {
                    a64_add_reg(b, 1, JTA, hb, hi, sc);
                    ea_cache_reset();
                    ACC_REGOFF(JGB, JTA, 0);
                    return 1;
                }
                a64_add_reg(b, 1, JTA, JGB, hb, 0); a64_add_reg(b, 1, JTA, JTA, hi, sc);
                ea_cache_set(b, m);
                ACC_AT(JTA, 0);
                return 1;
            }
        }
        int scaled = m->disp >= 0 && (m->disp % size) == 0 && m->disp / size <= 4095;
        int unscaled = !scaled && m->disp >= -256 && m->disp <= 255;
        if (!scaled && !unscaled) return 0;
        if (!ea_cache_reusable(b, m)) {
            if (hoisted) a64_add_reg(b, 1, JTA, hreg, hi, sc);
            else if (ea_cache_has_base(b, m)) a64_add_reg(b, 1, JTA, JTA, hi, sc);
            else { a64_add_reg(b, 1, JTA, JGB, hb, 0); a64_add_reg(b, 1, JTA, JTA, hi, sc); }
        }
        ea_cache_set(b, m);
        ACC_AT(JTA, m->disp);
        return 1;
    }
    {
        int scaled = m->disp >= 0 && (m->disp % size) == 0 && m->disp / size <= 4095;
        int unscaled = !scaled && m->disp >= -256 && m->disp <= 255;
        if (!scaled && !unscaled) return 0;
        int ra = JTA;
        if (hoisted) ra = hreg;
        else { if (!ea_cache_has_base(b, m)) a64_add_reg(b, 1, JTA, JGB, hb, 0); ea_cache_set_full(b, m->base, OCERZ_REG_NONE, 0); }
        ACC_AT(ra, m->disp);
        return 1;
    }
#undef ACC_AT
#undef ACC_REGOFF
}

const struct X86Insn *g_cur_insns_fwd(void) { return g_cur_insns; }

struct JitState_g_ea_cache g_ea_cache;

static int a64_word_may_write_x15(uint32_t w) { return a64_word_may_write_reg(w, 15); }

int ea_cache_usable(const A64Buf *b)
{
    static int dis = -1;
    if (dis < 0) dis = getenv("OCERZ_NO_EACACHE") ? 1 : 0;
    if (dis || !g_ea_cache.valid || !g_cur_insns) return 0;
    if (g_ea_cache.seq != g_callout_seq) return 0;
    if (!g_ea_cache.after || g_ea_cache.after > b->p) return 0;
    for (const uint32_t *w = g_ea_cache.after; w < b->p; w++)
        if (a64_word_may_write_x15(*w)) return 0;
    return 1;
}

static int ea_cache_reusable(const A64Buf *b, const X86Operand *op)
{
    if (!ea_cache_usable(b)) return 0;
    return g_ea_cache.base == op->base && g_ea_cache.index == op->index &&
           g_ea_cache.scale == (op->scale & 3);
}

static void ea_cache_set(const A64Buf *b, const X86Operand *op) { ea_cache_set_full(b, op->base, op->index, op->scale & 3); }

int emit_mem_ea_plain_ex(A64Buf *b, const X86Insn *insn, const X86Operand *op,
                                int size, int *ra_out, uint32_t *disp_out, int unscaled_ok)
{
    if (lowstack_disp_ea(b, insn, op, size, unscaled_ok)) { *ra_out = JTA; *disp_out = (uint32_t)op->disp; return 1; }
    if (low_hoist_covers(insn, op) && !ENV_ON("OCERZ_NO_HOIST_DISP")) {
        int64_t d = op->disp;
        if ((d >= 0 && (d % size) == 0 && d / size <= 4095) || (unscaled_ok && d >= -256 && d <= 255)) {
            *ra_out = JMEMBASE; *disp_out = (uint32_t)d;
            return 1;
        }
    }
    if (!mem_fast_forms_ok()) return 0;
    if (insn->seg != OCERZ_SEG_NONE || insn->addrsize != 8) return 0;
    if (rsp_is_ptr() && op->index == OCERZ_RSP) return 0;
    if (rsp_is_ptr() && op->base == OCERZ_RSP) {
        if (op->index != OCERZ_REG_NONE || op->riprel || pin_slot(OCERZ_RSP) < 0) return 0;
        int64_t sd = op->disp;
        if (!((sd >= 0 && (sd % size) == 0 && sd / size <= 4095) || (unscaled_ok && sd >= -256 && sd <= 255))) return 0;
        *ra_out = pin_hreg(pin_slot(OCERZ_RSP)); *disp_out = (uint32_t)sd;
        return 1;
    }
    if (op->riprel) {
        uint64_t c = (uint64_t)op->disp + ocerz_guest_base;
        static int nolit = -1; if (nolit < 0) nolit = getenv("OCERZ_NO_RIPLIT") ? 1 : 0;
        if (!nolit && g_n_raslit < RASLIT_MAX && (c >> 32) != 0 && ((c >> 16) & 0xffff) != 0) {
            g_raslit[g_n_raslit].site = a64_label(b);
            g_raslit[g_n_raslit].retaddr = c;
            g_raslit[g_n_raslit].kind = 1;
            g_raslit[g_n_raslit].tcr = 0;
            g_raslit[g_n_raslit].rt = JTA;
            g_n_raslit++;
            a64_emit32(b, 0x58000000u | (uint32_t)JTA);
        } else {
            a64_mov_imm64(b, JTA, c);
        }
        ea_cache_reset();
        *ra_out = JTA; *disp_out = 0;
        return 1;
    }
    if (op->base != OCERZ_REG_NONE && pin_slot(op->base) < 0) return 0;
    if (op->index != OCERZ_REG_NONE && pin_slot(op->index) < 0) return 0;
    X86Operand mview; op = mem_hoist_view(op, &mview);
    int64_t disp = op->disp;
    int fits = (disp >= 0 && (disp % size) == 0 && disp / size <= 4095) ||
               (unscaled_ok && disp >= -256 && disp <= 255);
    int have = 0;
    int hreg = op->base != OCERZ_REG_NONE ? hoist_reg_for(op->base) : -1;
    if (fits && (op->base != OCERZ_REG_NONE || op->index != OCERZ_REG_NONE) && ea_cache_reusable(b, op)) {
        *ra_out = JTA; *disp_out = (uint32_t)disp;
        return 1;
    }
    if (hreg == JMEMBASE && g_mem_hoist_aux_index >= 0 && op->index != OCERZ_REG_NONE &&
        op->index == (unsigned)g_mem_hoist_aux_index && (op->scale & 3) == g_mem_hoist_aux_scale) {
        if (fits) { *ra_out = JMEMAUX; *disp_out = (uint32_t)disp; return 1; }
        if (disp > 0 && disp <= 4095)       a64_add_imm(b, 1, JTA, JMEMAUX, (uint32_t)disp);
        else if (disp < 0 && -disp <= 4095) a64_sub_imm(b, 1, JTA, JMEMAUX, (uint32_t)-disp);
        else { a64_mov_imm64(b, JTU, (uint64_t)disp); a64_add_reg(b, 1, JTA, JMEMAUX, JTU, 0); }
        ea_cache_reset();
        *ra_out = JTA; *disp_out = 0;
        return 1;
    }
    if (hreg >= 0) {
        if (op->index == OCERZ_REG_NONE) {
            if (fits) { *ra_out = hreg; *disp_out = (uint32_t)disp; return 1; }
            if (disp > 0 && disp <= 4095)       a64_add_imm(b, 1, JTA, hreg, (uint32_t)disp);
            else if (disp < 0 && -disp <= 4095) a64_sub_imm(b, 1, JTA, hreg, (uint32_t)-disp);
            else { a64_mov_imm64(b, JTU, (uint64_t)disp); a64_add_reg(b, 1, JTA, hreg, JTU, 0); }
            ea_cache_reset();
            *ra_out = JTA; *disp_out = 0;
            return 1;
        }
        a64_add_reg(b, 1, JTA, hreg, pin_hreg(pin_slot(op->index)), op->scale & 3);
        have = 1;
    } else if (op->base != OCERZ_REG_NONE) {
        if (op->index != OCERZ_REG_NONE && ea_cache_has_base(b, op)) {
            a64_add_reg(b, 1, JTA, JTA, pin_hreg(pin_slot(op->index)), op->scale & 3);
            ea_cache_set(b, op);
            if (fits) { *ra_out = JTA; *disp_out = (uint32_t)disp; return 1; }
            goto fold_disp;
        }
        if (!ea_cache_has_base(b, op)) a64_add_reg(b, 1, JTA, JGB, pin_hreg(pin_slot(op->base)), 0);
        ea_cache_set_full(b, op->base, OCERZ_REG_NONE, 0);
        have = 1;
    }
    if (op->index != OCERZ_REG_NONE && !(have && hreg >= 0)) {
        a64_add_reg(b, 1, JTA, have ? JTA : JGB, pin_hreg(pin_slot(op->index)), op->scale & 3);
        have = 1;
    }
    if (!have) {
        if (fits) { *ra_out = JGB; *disp_out = (uint32_t)disp; return 1; }
        a64_mov_imm64(b, JTA, (uint64_t)disp + ocerz_guest_base);
        ea_cache_reset();
        *ra_out = JTA; *disp_out = 0;
        return 1;
    }
    ea_cache_set(b, op);
    if (fits) { *ra_out = JTA; *disp_out = (uint32_t)disp; return 1; }
fold_disp:
    if (disp > 0 && disp <= 4095)       a64_add_imm(b, 1, JTA, JTA, (uint32_t)disp);
    else if (disp < 0 && -disp <= 4095) a64_sub_imm(b, 1, JTA, JTA, (uint32_t)-disp);
    else { a64_mov_imm64(b, JTU, (uint64_t)disp); a64_add_reg(b, 1, JTA, JTA, JTU, 0); }
    ea_cache_reset();
    *ra_out = JTA; *disp_out = 0;
    return 1;
}

int emit_mem_load_plain(A64Buf *b, const X86Insn *insn, const X86Operand *op, int size, int rd)
{
    int ra; uint32_t disp;
    int plain = mem_plain_access_ok(op);
    X86Operand mview; op = mem_hoist_view(op, &mview);
    int aux_disp_ok = op->disp != 0 && g_pin_class != 2 && g_mem_hoist_aux_index < 0 &&
                      op->disp == g_mem_hoist_aux_disp && op->base != OCERZ_REG_NONE &&
                      hoist_reg_for(op->base) == JMEMBASE;
    if (mem_fast_forms_ok() &&
        insn->seg == OCERZ_SEG_NONE && insn->addrsize == 8 && !op->riprel &&
        op->base != OCERZ_REG_NONE && op->index != OCERZ_REG_NONE && (op->disp == 0 || aux_disp_ok) &&
        pin_slot(op->base) >= 0 && pin_slot(op->index) >= 0 &&
        !(rsp_is_ptr() && (op->base == OCERZ_RSP || op->index == OCERZ_RSP))) {
        int sc = op->scale & 3;
        int want = size == 8 ? 3 : size == 4 ? 2 : size == 2 ? 1 : 0;
        if (aux_disp_ok && (sc == 0 || sc == want)) {
            emit_gpr_ld_regoff(b, size, rd, JMEMAUX, pin_hreg(pin_slot(op->index)), sc != 0, plain);
            return 1;
        }
        if (aux_disp_ok) goto generic;
        if (sc == 0 || sc == want) {
            int hb = hoist_reg_for(op->base);
            if (hb < 0) {
                hb = JTA;
                if (!ea_cache_has_base(b, op)) a64_add_reg(b, 1, JTA, JGB, pin_hreg(pin_slot(op->base)), 0);
                ea_cache_set_full(b, op->base, OCERZ_REG_NONE, 0);
            }
            emit_gpr_ld_regoff(b, size, rd, hb, pin_hreg(pin_slot(op->index)), sc != 0, plain);
            return 1;
        }
        static int nogea = -1; if (nogea < 0) nogea = getenv("OCERZ_NO_GEAFORM") ? 1 : 0;
        if (!nogea && plain && hoist_reg_for(op->base) < 0 && !ea_cache_has_base(b, op) && !ea_cache_reusable(b, op)) {
            a64_add_reg(b, 1, JTA, pin_hreg(pin_slot(op->base)), pin_hreg(pin_slot(op->index)), sc);
            ea_cache_reset();
            a64_ldr_regoff(b, size, rd, JGB, JTA, 0);
            return 1;
        }
    }
generic:
    if (!emit_mem_ea_plain_ex(b, insn, op, size, &ra, &disp, 1)) return 0;
    emit_gpr_ld_at(b, size, rd, ra, (int32_t)disp, plain);
    return 1;
}

int emit_mov_mem(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    uint64_t gbase = ocerz_guest_base;

    if (d->kind == OCERZ_OPK_MEM && s->kind == OCERZ_OPK_IMM) {
        int size = d->size;
        if (size != 1 && size != 2 && size != 4 && size != 8) return 0;
        if (!mem_native_store_ok()) return 0;
        uint64_t v = (uint64_t)ocerz_sext(s->imm, s->size);
        if (size < 8) v &= (1ull << (size * 8)) - 1;
        int rv = A64_ZR;
        if (v != 0) { a64_mov_imm64(b, JT1, v); rv = JT1; }
        if (emit_hoisted_mem_access(b, insn, d, size, rv, 1))
            return 1;
        if (emit_plain_mem_fast(b, insn, d, size, rv, 1, 0))
            return 1;
        if (!emit_mem_ea(b, insn, d, JTA))
            return 0;
        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, gbase - ea_fold());
        emit_guest_store_ordered(b, size, rv, JTA, JTU);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    if (d->kind == OCERZ_OPK_MEM && s->kind == OCERZ_OPK_REG) {
        if (s->high8 || (s->size != 1 && s->size != 2 && s->size != 4 && s->size != 8))
            return 0;
        if (!mem_native_store_ok())
            return 0;
        int ss = pin_slot(s->reg);
        int rv = ss >= 0 ? pin_hreg(ss) : JT1;
        if (ss >= 0 && emit_hoisted_mem_access(b, insn, d, s->size, rv, 1))
            return 1;
        if (ss >= 0 && emit_plain_mem_fast(b, insn, d, s->size, rv, 1, 0))
            return 1;
        if (!emit_mem_ea(b, insn, d, JTA))
            return 0;
        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        if (ss < 0)
            emit_gpr_rd(b, s->size == 8 ? 1 : 0, JT1, s->reg);
        emit_add_const(b, JTA, gbase - ea_fold());

        emit_guest_store_ordered(b, s->size, rv, JTA, JTU);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    if (d->kind == OCERZ_OPK_REG && s->kind == OCERZ_OPK_MEM &&
        (d->size == 1 || d->size == 2) && !d->high8) {
        int ds = pin_slot(d->reg);
        if (ds < 0 || (rsp_is_ptr() && d->reg == OCERZ_RSP)) return 0;
        if (!emit_mem_load_plain(b, insn, s, d->size, JT1)) {
            if (!emit_mem_ea(b, insn, s, JTA))
                return 0;
            uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
            emit_add_const(b, JTA, gbase - ea_fold());
            emit_guest_load_ordered(b, d->size, JT1, JTA, JTU);
            patch_guard_skip(skip, a64_label(b));
        }
        a64_bfi(b, 1, pin_hreg(ds), JT1, 0, d->size * 8);
        return 1;
    }
    if (d->kind == OCERZ_OPK_REG && s->kind == OCERZ_OPK_MEM) {
        if (d->high8 || (d->size != 4 && d->size != 8))
            return 0;
        int ds = pin_slot(d->reg);
        int rd = ds >= 0 ? pin_hreg(ds) : JT1;
        if (emit_hoisted_mem_access(b, insn, s, d->size, rd, 0))
            return 1;
        if (ds >= 0 && emit_plain_mem_fast(b, insn, s, d->size, rd, 0, 0))
            return 1;
        if (!emit_mem_ea(b, insn, s, JTA))
            return 0;
        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, gbase - ea_fold());

        emit_guest_load_ordered(b, d->size, rd, JTA, JTU);
        if (ds < 0)
            emit_gpr_wr(b, JT1, d->reg);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    return 0;
}

int emit_movx(A64Buf *b, const X86Insn *insn, int is_signed,
                     uint32_t **exit_sites, int *n_exits)
{
    static int no_sub = -1;
    if (no_sub < 0)
        no_sub = getenv("OCERZ_NO_INLINE_SUBWORD") ? 1 : 0;
    if (no_sub)
        return 0;
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    uint64_t gbase = ocerz_guest_base;
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    if (s->size != 1 && s->size != 2)
        return 0;
    int sf = (d->size == 8);
    if (s->kind == OCERZ_OPK_REG) {
        if (s->high8)
            return 0;
        if (pin_slot(d->reg) >= 0 && pin_slot(s->reg) >= 0 &&
            !(rsp_is_ptr() && (d->reg == OCERZ_RSP || s->reg == OCERZ_RSP))) {
            int rd = pin_hreg(pin_slot(d->reg)), rs = pin_hreg(pin_slot(s->reg));
            if (is_signed) { if (s->size == 1) a64_sxtb(b, sf, rd, rs); else a64_sxth(b, sf, rd, rs); }
            else           { if (s->size == 1) a64_uxtb(b, rd, rs); else a64_uxth(b, rd, rs); }
            return 1;
        }
        emit_gpr_rd(b, 1, JT1, s->reg);
        if (is_signed) {
            if (s->size == 1) a64_sxtb(b, sf, JT1, JT1);
            else              a64_sxth(b, sf, JT1, JT1);
        } else {
            if (s->size == 1) a64_uxtb(b, JT1, JT1);
            else              a64_uxth(b, JT1, JT1);
        }
        emit_gpr_wr(b, JT1, d->reg);
        return 1;
    }
    if (s->kind == OCERZ_OPK_MEM) {
        int ds = pin_slot(d->reg);
        if (rsp_is_ptr() && d->reg == OCERZ_RSP) ds = -1;
        if (ds >= 0) {
            int ra; uint32_t disp;
            if (!is_signed && emit_mem_load_plain(b, insn, s, s->size, pin_hreg(ds)))
                return 1;
            if (is_signed && emit_mem_ea_plain(b, insn, s, s->size, &ra, &disp)) {
                if (mem_plain_access_ok(s)) {
                    if (s->size == 1) a64_ldrsb(b, sf, pin_hreg(ds), ra, disp);
                    else              a64_ldrsh(b, sf, pin_hreg(ds), ra, disp);
                } else {
                    emit_gpr_lds_at(b, s->size, sf, pin_hreg(ds), ra, (int32_t)disp);
                }
                return 1;
            }
        }
        if (!emit_mem_ea(b, insn, s, JTA))
            return 0;
        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, gbase - ea_fold());
        emit_guest_load_ordered(b, s->size, JT1, JTA, JTU);
        if (is_signed) {
            if (s->size == 1) a64_sxtb(b, sf, JT1, JT1);
            else              a64_sxth(b, sf, JT1, JT1);
        }
        emit_gpr_wr(b, JT1, d->reg);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    return 0;
}

struct JitOslowMap g_fpbmap[JIT_MAX_BLOCK_INSNS];

int g_n_fpbmap;

static int rmw_src_to(A64Buf *b, const X86Operand *s, int size, int into, int *out)
{
    if (s->kind == OCERZ_OPK_IMM) {
        uint64_t v = (uint64_t)ocerz_sext(s->imm, s->size);
        if (size == 4) v &= 0xffffffffull; else if (size == 2) v &= 0xffff; else if (size == 1) v &= 0xff;
        a64_mov_imm64(b, into, v); *out = into; return 1;
    }
    if (s->kind != OCERZ_OPK_REG || s->high8) return 0;
    int ss = pin_slot(s->reg);
    if (ss < 0 || (rsp_is_ptr() && s->reg == OCERZ_RSP)) return 0;
    int r = pin_hreg(ss);
    if (size == 1) { a64_uxtb(b, into, r); *out = into; }
    else if (size == 2) { a64_uxth(b, into, r); *out = into; }
    else *out = r;
    return 1;
}

static void rmw_write_reg(A64Buf *b, const X86Operand *d, int size, int val)
{
    int rd = pin_hreg(pin_slot(d->reg));
    if (size == 8) a64_mov_reg(b, 1, rd, val);
    else if (size == 4) a64_mov_reg(b, 0, rd, val);
    else a64_bfi(b, 1, rd, val, 0, size * 8);
}

int emit_rmw_mem(A64Buf *b, const X86Insn *insn, uint64_t need,
                        uint32_t **exit_sites, int *n_exits)
{
    static int dis = -1; if (dis < 0) dis = getenv("OCERZ_NO_INLINE_RMW") ? 1 : 0;
    if (dis || !g_defer || (insn->addrsize != 8 && !(insn->addrsize == 4 && insn->mode32))) return 0;
    if (insn->seg != OCERZ_SEG_NONE && insn->seg != OCERZ_SEG_GS && insn->seg != OCERZ_SEG_FS) return 0;
    unsigned op = insn->op;
    const X86Operand *m, *s = NULL, *r = NULL;
    if (op == OCERZ_OP_XCHG) {
        if (insn->nops != 2) return 0;
        if (insn->ops[0].kind == OCERZ_OPK_MEM) { m = &insn->ops[0]; r = &insn->ops[1]; }
        else if (insn->ops[1].kind == OCERZ_OPK_MEM) { m = &insn->ops[1]; r = &insn->ops[0]; }
        else return 0;
        if (r->kind != OCERZ_OPK_REG || r->high8 || pin_slot(r->reg) < 0 || r->size != m->size) return 0;
        s = r;
    } else {
        if (insn->nops < 1 || insn->ops[0].kind != OCERZ_OPK_MEM) return 0;
        m = &insn->ops[0];
        if (insn->nops == 2) {
            s = &insn->ops[1];
            if (s->size != m->size) return 0;
            if (op == OCERZ_OP_XADD) { r = s; if (r->kind != OCERZ_OPK_REG || r->high8 || pin_slot(r->reg) < 0) return 0; }
        } else if (op != OCERZ_OP_INC && op != OCERZ_OP_DEC && op != OCERZ_OP_NEG && op != OCERZ_OP_NOT) return 0;
    }
    int size = m->size;
    if (size != 1 && size != 2 && size != 4 && size != 8) return 0;
    if (rsp_is_ptr() && (m->index == OCERZ_RSP || (r && r->reg == OCERZ_RSP) ||
                         (s && s->kind == OCERZ_OPK_REG && s->reg == OCERZ_RSP)))
        return 0;
    if (op == OCERZ_OP_CMPXCHG && (pin_slot(OCERZ_RAX) < 0 || !s || s->kind != OCERZ_OPK_REG || s->high8 || pin_slot(s->reg) < 0)) return 0;
    if (!mem_native_store_ok()) return 0;
    int atomic = op == OCERZ_OP_XCHG || op == OCERZ_OP_XADD || op == OCERZ_OP_CMPXCHG || insn->lock;
    int is_cmp = op == OCERZ_OP_CMP || op == OCERZ_OP_TEST;
    if (!g_plain_mem && atomic && op == OCERZ_OP_NEG) return 0;
    int sf = size == 8;
    int ordered = !g_plain_mem;

    int incdec_cf = (op == OCERZ_OP_INC || op == OCERZ_OP_DEC) && need;
    if (incdec_cf) {
        emit_cc_predicate(b, OCERZ_CC_B);
        a64_cset(b, JT1, A64_NE);
        a64_str(b, 8, JT1, 20, (uint32_t)offsetof(OcerzCPU, jit_scratch));
    }
    int ra; uint32_t disp;
    int plainacc = mem_plain_access_ok(m);
    if (insn->seg != OCERZ_SEG_NONE || !emit_mem_ea_plain(b, insn, m, size, &ra, &disp)) {
        if (!emit_mem_ea(b, insn, m, JTA)) return 0;
        (void)emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
        ra = JTA; disp = 0;
    } else if ((!plainacc || (ordered && atomic)) && disp != 0) {
        if (disp <= 4095) a64_add_imm(b, 1, JTA, ra, disp);
        else { a64_mov_imm64(b, JTU, disp); a64_add_reg(b, 1, JTA, ra, JTU, 0); }
        ra = JTA; disp = 0;
    }
    int rs = -1;
    if (s && op != OCERZ_OP_CMPXCHG) { if (!rmw_src_to(b, s, size, JT1, &rs)) return 0; }
    if (op == OCERZ_OP_CMPXCHG) rs = pin_hreg(pin_slot(s->reg));
    if (op == OCERZ_OP_CMPXCHG && size < 4) { if (size == 1) a64_uxtb(b, JT1, rs); else a64_uxth(b, JT1, rs); rs = JT1; }
    int hax = pin_slot(OCERZ_RAX) >= 0 ? pin_hreg(pin_slot(OCERZ_RAX)) : -1;

    uint32_t *align_bne = NULL;
    if (ordered && atomic) {
        if (size > 1) {
            a64_try_ands_imm(b, 1, A64_ZR, ra, (uint64_t)(size - 1));
            align_bne = a64_label(b); a64_bcond(b, A64_NE, 0);
        }
        switch (op) {
        case OCERZ_OP_ADD: case OCERZ_OP_XADD: a64_ldop_al(b, size, 0, rs, JT0, ra); break;
        case OCERZ_OP_SUB: a64_neg_reg(b, sf, JT2, rs); if (size == 1) a64_uxtb(b, JT2, JT2); else if (size == 2) a64_uxth(b, JT2, JT2);
                           a64_ldop_al(b, size, 0, JT2, JT0, ra); break;
        case OCERZ_OP_OR:  a64_ldop_al(b, size, 3, rs, JT0, ra); break;
        case OCERZ_OP_XOR: a64_ldop_al(b, size, 2, rs, JT0, ra); break;
        case OCERZ_OP_AND: a64_mvn_reg(b, sf, JT2, rs); a64_ldop_al(b, size, 1, JT2, JT0, ra); break;
        case OCERZ_OP_INC: a64_mov_imm64(b, JT2, 1); a64_ldop_al(b, size, 0, JT2, JT0, ra); break;
        case OCERZ_OP_DEC: a64_mov_imm64(b, JT2, size == 8 ? ~0ull : size == 4 ? 0xffffffffull : size == 2 ? 0xffffull : 0xffull);
                           a64_ldop_al(b, size, 0, JT2, JT0, ra); break;
        case OCERZ_OP_NOT: a64_mov_imm64(b, JT2, ~0ull); a64_ldop_al(b, size, 2, JT2, JT0, ra); break;
        case OCERZ_OP_XCHG: a64_swpal(b, size, rs, JT0, ra); break;
        case OCERZ_OP_CMPXCHG:
            if (size == 8) a64_mov_reg(b, 1, JT0, hax); else if (size == 4) a64_mov_reg(b, 0, JT0, hax);
            else if (size == 2) a64_uxth(b, JT0, hax); else a64_uxtb(b, JT0, hax);
            a64_casal(b, size, JT0, rs, ra);
            break;
        default: return 0;
        }
    } else {
        emit_gpr_ld_at(b, size, JT0, ra, (int32_t)disp, plainacc);
    }

    int have_new = 1;
    switch (op) {
    case OCERZ_OP_ADD: case OCERZ_OP_XADD: a64_add_reg(b, sf, JT2, JT0, rs, 0); break;
    case OCERZ_OP_SUB: a64_sub_reg(b, sf, JT2, JT0, rs, 0); break;
    case OCERZ_OP_AND: a64_and_reg(b, sf, JT2, JT0, rs, 0); break;
    case OCERZ_OP_TEST: if (need) a64_and_reg(b, sf, JT2, JT0, rs, 0); have_new = 0; break;
    case OCERZ_OP_OR:  a64_orr_reg(b, sf, JT2, JT0, rs, 0); break;
    case OCERZ_OP_XOR: a64_eor_reg(b, sf, JT2, JT0, rs, 0); break;
    case OCERZ_OP_INC: a64_add_imm(b, sf, JT2, JT0, 1); break;
    case OCERZ_OP_DEC: a64_sub_imm(b, sf, JT2, JT0, 1); break;
    case OCERZ_OP_NEG: a64_neg_reg(b, sf, JT2, JT0); break;
    case OCERZ_OP_NOT: a64_mvn_reg(b, sf, JT2, JT0); break;
    case OCERZ_OP_XCHG: a64_mov_reg(b, 1, JT2, rs); break;
    case OCERZ_OP_CMPXCHG: {
        int acc = JTU;
        if (size == 8) acc = hax; else if (size == 4) a64_mov_reg(b, 0, JTU, hax); else if (size == 2) a64_uxth(b, JTU, hax); else a64_uxtb(b, JTU, hax);
        if (size == 8) a64_subs_reg(b, 1, A64_ZR, JT0, hax, 0); else a64_subs_reg(b, 0, A64_ZR, JT0, acc, 0);
        a64_csel(b, 1, JT2, rs, JT0, A64_EQ);
        if (need) emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_SUB, size, 0), JT0, acc);
        if (size == 8) a64_csel(b, 1, hax, hax, JT0, A64_EQ);
        else if (size == 4) a64_csel(b, 1, hax, acc, JT0, A64_EQ);
        else { a64_csel(b, 1, JT1, acc, JT0, A64_EQ); a64_bfi(b, 1, hax, JT1, 0, size * 8); }
        break;
    }
    case OCERZ_OP_CMP: have_new = 0; break;
    default: return 0;
    }
    if (op != OCERZ_OP_CMP && (op != OCERZ_OP_TEST || need)) {
        if (size == 1) a64_uxtb(b, JT2, JT2); else if (size == 2) a64_uxth(b, JT2, JT2);
        else if (size == 4 && (op == OCERZ_OP_NEG || op == OCERZ_OP_NOT || op == OCERZ_OP_SUB || op == OCERZ_OP_ADD || op == OCERZ_OP_XADD || op == OCERZ_OP_INC || op == OCERZ_OP_DEC))
            a64_mov_reg(b, 0, JT2, JT2);
    }

    if (!is_cmp && !(ordered && atomic))
        emit_gpr_st_at(b, size, JT2, ra, (int32_t)disp, plainacc);
    if (need && op != OCERZ_OP_CMPXCHG) {
        switch (op) {
        case OCERZ_OP_ADD: case OCERZ_OP_XADD:
            if (rs == JT1) emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_ADD, size, 0), JT0, JT1);
            else { if (size == 8) emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_ADD, size, 0), JT0, rs);
                   else { a64_mov_reg(b, 0, JT1, rs); emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_ADD, size, 0), JT0, JT1); } }
            break;
        case OCERZ_OP_SUB: case OCERZ_OP_CMP:
            if (rs == JT1) emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_SUB, size, 0), JT0, JT1);
            else { if (size == 8) emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_SUB, size, 0), JT0, rs);
                   else { a64_mov_reg(b, 0, JT1, rs); emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_SUB, size, 0), JT0, JT1); } }
            break;
        case OCERZ_OP_AND: case OCERZ_OP_OR: case OCERZ_OP_XOR: case OCERZ_OP_TEST:
            emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_LOGIC, size, 0), JT2, JT2);
            break;
        case OCERZ_OP_INC: case OCERZ_OP_DEC:
            a64_ldr(b, 8, JT1, 20, (uint32_t)offsetof(OcerzCPU, jit_scratch));
            emit_defer_flags(b, ocerz_cc_pack(op == OCERZ_OP_INC ? OCERZ_CC_INC : OCERZ_CC_DEC, size, 0), JT1, JT2);
            break;
        case OCERZ_OP_NEG:
            a64_mov_imm64(b, JT1, 0);
            emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_SUB, size, 0), JT1, JT0);
            break;
        default: break;
        }
    }
    if (op == OCERZ_OP_XCHG || op == OCERZ_OP_XADD) rmw_write_reg(b, r, size, JT0);
    if (g_nzcv_want && is_cmp && size >= 4) {
        if (op == OCERZ_OP_CMP) a64_subs_reg(b, sf, A64_ZR, JT0, rs, 0);
        else                    a64_ands_reg(b, sf, A64_ZR, JT0, rs, 0);
        g_nzcv_kind = op == OCERZ_OP_CMP ? OCERZ_CC_SUB : OCERZ_CC_LOGIC;
        g_nzcv_from = g_cur_insn_idx;
    }
    (void)have_new;
    if (align_bne) {
        uint32_t *sites[1] = { align_bne };
        if (!oolslow_add(insn, sites, 1, a64_label(b))) {
            uint32_t *done = a64_label(b);
            a64_b(b, 0);
            patch_any_branch(align_bne, a64_label(b));
            emit_slowcall(b, insn, exit_sites, n_exits);
            a64_patch_b(done, a64_label(b));
        }
    }
    return 1;
}

int emit_xchg_reg32(A64Buf *b, const X86Insn *insn)
{
    const X86Operand *x = &insn->ops[0], *y = &insn->ops[1];
    int size = x->size;
    if (insn->nops != 2 || y->kind != OCERZ_OPK_REG || y->size != size || pin_slot(x->reg) < 0 || pin_slot(y->reg) < 0)
        return 0;
    int rx = pin_hreg(pin_slot(x->reg)), ry = pin_hreg(pin_slot(y->reg));
    if (size == 4) {
        if (rx == ry) {
            a64_mov_reg(b, 0, rx, rx);
            return 1;
        }
        a64_mov_reg(b, 0, JT0, rx);
        a64_mov_reg(b, 0, rx, ry);
        a64_mov_reg(b, 0, ry, JT0);
        return 1;
    }
    if (size != 1 && size != 2)
        return 0;
    int lx = x->high8 ? 8 : 0, ly = y->high8 ? 8 : 0, w = size * 8;
    a64_ubfx(b, 1, JT0, rx, lx, w);
    a64_ubfx(b, 1, JT1, ry, ly, w);
    a64_bfi(b, 1, rx, JT1, lx, w);
    a64_bfi(b, 1, ry, JT0, ly, w);
    return 1;
}

int emit_cmpxchg8b(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *m = &insn->ops[0];
    if (!insn->mode32 || insn->opsize != 8 || insn->nops != 1 || m->kind != OCERZ_OPK_MEM || insn->addrsize != 4)
        return 0;
    if (!g_defer || !mem_native_store_ok())
        return 0;
    if (pin_slot(OCERZ_RAX) < 0 || pin_slot(OCERZ_RDX) < 0 || pin_slot(OCERZ_RBX) < 0 || pin_slot(OCERZ_RCX) < 0)
        return 0;
    int hax = pin_hreg(pin_slot(OCERZ_RAX)), hdx = pin_hreg(pin_slot(OCERZ_RDX));
    int hbx = pin_hreg(pin_slot(OCERZ_RBX)), hcx = pin_hreg(pin_slot(OCERZ_RCX));
    emit_materialize(b);
    if (!emit_mem_ea(b, insn, m, JTA))
        return 0;
    (void)emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
    emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
    a64_mov_reg(b, 0, JT2, hax);
    a64_bfi(b, 1, JT2, hdx, 32, 32);
    a64_mov_reg(b, 0, JT1, hbx);
    a64_bfi(b, 1, JT1, hcx, 32, 32);
    uint32_t *align_bne = NULL;
    if (!g_plain_mem) {
        a64_try_ands_imm(b, 1, A64_ZR, JTA, 7);
        align_bne = a64_label(b);
        a64_bcond(b, A64_NE, 0);
        a64_mov_reg(b, 1, JT0, JT2);
        a64_casal(b, 8, JT0, JT1, JTA);
    } else {
        int plainacc = mem_plain_access_ok(m);
        emit_gpr_ld_at(b, 8, JT0, JTA, 0, plainacc);
        a64_subs_reg(b, 1, A64_ZR, JT0, JT2, 0);
        a64_csel(b, 1, JT1, JT1, JT0, A64_EQ);
        emit_gpr_st_at(b, 8, JT1, JTA, 0, plainacc);
    }
    a64_subs_reg(b, 1, A64_ZR, JT0, JT2, 0);
    a64_mov_reg(b, 0, JTT, JT0);
    a64_csel(b, 1, hax, hax, JTT, A64_EQ);
    a64_lsr_imm(b, 1, JTT, JT0, 32);
    a64_csel(b, 1, hdx, hdx, JTT, A64_EQ);
    a64_cset(b, JTT, A64_EQ);
    a64_ldr(b, 8, JTU, 20, (uint32_t)offsetof(OcerzCPU, rflags));
    a64_bfi(b, 1, JTU, JTT, 6, 1);
    a64_str(b, 8, JTU, 20, (uint32_t)offsetof(OcerzCPU, rflags));
    if (align_bne) {
        uint32_t *sites[1] = { align_bne };
        if (!oolslow_add(insn, sites, 1, a64_label(b))) {
            uint32_t *done = a64_label(b);
            a64_b(b, 0);
            patch_any_branch(align_bne, a64_label(b));
            emit_slowcall(b, insn, exit_sites, n_exits);
            a64_patch_b(done, a64_label(b));
        }
    }
    return 1;
}

int insn_may_write_gpr(const X86Insn *in, unsigned reg)
{
    reg &= 15;
    switch (in->op) {
    case OCERZ_OP_CMP: case OCERZ_OP_TEST: case OCERZ_OP_BT:
    case OCERZ_OP_JMP: case OCERZ_OP_JCC: case OCERZ_OP_JRCXZ:
    case OCERZ_OP_NOP: case OCERZ_OP_PREFETCH: case OCERZ_OP_CLFLUSH:
    case OCERZ_OP_UCOMISS: case OCERZ_OP_UCOMISD: case OCERZ_OP_COMISS: case OCERZ_OP_COMISD:
    case OCERZ_OP_PTEST:
        return 0;
    case OCERZ_OP_PUSH: case OCERZ_OP_POP: case OCERZ_OP_CALL: case OCERZ_OP_RET:
    case OCERZ_OP_PUSHF: case OCERZ_OP_POPF: case OCERZ_OP_LEAVE:
        if (reg == OCERZ_RSP || reg == OCERZ_RBP) return 1;
        break;
    case OCERZ_OP_DIV: case OCERZ_OP_IDIV: case OCERZ_OP_MUL:
    case OCERZ_OP_CBW: case OCERZ_OP_CWD:
        if (reg == OCERZ_RAX || reg == OCERZ_RDX) return 1;
        break;
    case OCERZ_OP_IMUL:
        if (in->nops == 1 && (reg == OCERZ_RAX || reg == OCERZ_RDX)) return 1;
        break;
    case OCERZ_OP_CPUID:
        if (reg == OCERZ_RAX || reg == OCERZ_RBX || reg == OCERZ_RCX || reg == OCERZ_RDX) return 1;
        break;
    case OCERZ_OP_RDTSC: case OCERZ_OP_RDTSCP: case OCERZ_OP_XGETBV:
        if (reg == OCERZ_RAX || reg == OCERZ_RDX || reg == OCERZ_RCX) return 1;
        break;
    case OCERZ_OP_MOVS: case OCERZ_OP_STOS: case OCERZ_OP_LODS: case OCERZ_OP_SCAS: case OCERZ_OP_CMPS:
        if (reg == OCERZ_RSI || reg == OCERZ_RDI || reg == OCERZ_RCX || reg == OCERZ_RAX) return 1;
        break;
    case OCERZ_OP_SYSCALL: case OCERZ_OP_INT: case OCERZ_OP_INT3:
        return 1;
    case OCERZ_OP_XCHG: case OCERZ_OP_XADD:
        for (int k = 0; k < in->nops; k++)
            if (in->ops[k].kind == OCERZ_OPK_REG && (in->ops[k].reg & 15) == reg) return 1;
        return 0;
    case OCERZ_OP_CMPXCHG: case OCERZ_OP_CMPXCHGXB:
        if (reg == OCERZ_RAX || reg == OCERZ_RDX || reg == OCERZ_RBX || reg == OCERZ_RCX) return 1;
        break;
    default:
        break;
    }
    if (in->nops > 0 && in->ops[0].kind == OCERZ_OPK_REG && (in->ops[0].reg & 15) == reg)
        return 1;
    return 0;
}

MarkSet g_lowhoist_marks;

static int lowhoist_marked(uint64_t key) { return mark_has(&g_lowhoist_marks, key); }

int select_low_hoist(const X86Insn *insns, int n, uint64_t rip)
{
    if (!ocerz_low_base || ocerz_guest_base != 0 || g_xlat_mode32 || g_pin_class != 3 || g_no_chain ||
        !low_guard_fast_ok() || g_mem_hoist_greg >= 0 || n < 2 || ENV_ON("OCERZ_NO_LOW_HOIST"))
        return -1;

    if (lowhoist_marked(jit_key(rip, 0))) {
        g_tc_learned = 1;
        return -1;
    }
    int cnt[16] = {0}, until[16], shut[16] = {0};
    int32_t lo[16] = {0}, hi[16] = {0};
    for (int r = 0; r < 16; r++) until[r] = n;
    for (int i = 0; i < n; i++) {
        const X86Insn *in = &insns[i];
        for (int k = 0; k < in->nops; k++) {
            const X86Operand *m = &in->ops[k];
            if (m->kind != OCERZ_OPK_MEM || m->riprel || m->base == OCERZ_REG_NONE || m->index != OCERZ_REG_NONE)
                continue;
            unsigned r = m->base & 15;
            if (shut[r] || until[r] < n) continue;
            int sz = m->size ? m->size : 64;
            if (in->seg != OCERZ_SEG_NONE || in->addrsize != 8 || m->disp < -4095 || m->disp + sz > 4095 || sz > 64) {
                cnt[r] = -1000;
                continue;
            }
            cnt[r]++;
            if (m->disp < lo[r]) lo[r] = (int32_t)m->disp;
            if (m->disp + sz > hi[r]) hi[r] = (int32_t)(m->disp + sz);
        }
        for (unsigned r = 0; r < 16; r++)
            if (!shut[r] && until[r] == n && insn_may_write_gpr(in, r)) until[r] = i + 1;
    }
    int best = -1;
    for (int r = 0; r < 16; r++) {
        if (r == OCERZ_RSP || pin_slot((unsigned)r) < 0 || cnt[r] < 3) continue;
        if (best < 0 || cnt[r] > cnt[best]) best = r;
    }
    if (best < 0) return -1;
    g_low_hoist_until = until[best];
    g_low_hoist_lo = lo[best];
    g_low_hoist_hi = hi[best] > 0 ? hi[best] : 1;
    return best;
}

void emit_low_hoist_check(A64Buf *b)
{
    int hb = pin_hreg(pin_slot(g_low_hoist_greg));
    g_n_low_hoist_bail = 0;
    a64_add_imm(b, 1, JTT, hb, (uint32_t)g_low_hoist_hi);
    a64_orr_reg(b, 1, JTT, JTT, hb, 0);
    a64_lsr_imm(b, 1, JTT, JTT, 32);
    a64_sub_imm(b, 1, JTT, JTT, (uint32_t)(OCERZ_LOW_LIMIT >> 32));
    g_low_hoist_bail[g_n_low_hoist_bail++] = a64_label(b);
    a64_tbz(b, JTT, 63, 0);
    if (g_low_hoist_lo < 0) {
        a64_sub_imm(b, 1, JTT, hb, (uint32_t)-g_low_hoist_lo);
        g_low_hoist_bail[g_n_low_hoist_bail++] = a64_label(b);
        a64_tbnz(b, JTT, 63, 0);
    }
    (void)a64_try_orr_imm(b, 1, JMEMBASE, hb, ocerz_low_base);
}

void emit_low_hoist_bail(A64Buf *b, uint64_t rip, uint32_t **epi_sites, int *n_epi)
{
    if (!g_n_low_hoist_bail) return;
    for (int k = 0; k < g_n_low_hoist_bail; k++) a64_patch_tbz(g_low_hoist_bail[k], a64_label(b));
    g_n_low_hoist_bail = 0;
    tc_imm64(b, JT0, TCR_BLK, 0, (uint64_t)(uintptr_t)g_cur_blk);
    a64_str(b, 8, JT0, 20, SIDE_BLK_OFF);
    a64_movn(b, JT0, 1, 0);
    a64_str(b, 4, JT0, 20, SIDE_IDX_OFF);
    a64_mov_imm64(b, JT0, rip);
    a64_str(b, 8, JT0, 20, RIP_OFF);
    a64_mov_imm64(b, 0, OCERZ_STEP_PROFILE);
    epi_sites[(*n_epi)++] = a64_label(b);
    a64_b(b, 0);
}

static void emit_misaligned_arm(A64Buf *b, const OrderedSlowPend *o)
{
    int cand[3] = { JTF, JTT, JTU }, sc[2], n = 0;
    for (int i = 0; i < 3 && n < 2; i++)
        if (cand[i] != o->rv && cand[i] != o->ra) sc[n++] = cand[i];
    int s1 = sc[0], s2 = sc[1];
    int size = o->size;
    if (!o->store) {
        if (o->vec) a64_ldr_v(b, size, o->rv, o->ra, (uint32_t)o->disp);
        else        a64_ldr(b, size, o->rv, o->ra, 0);
        a64_dmb_ishld(b);
        return;
    }
    if (o->vec && g_blk_ordered_loads) {
        a64_dmb_ish(b);
        a64_str_v(b, size, o->rv, o->ra, (uint32_t)o->disp);
        return;
    }
    if (!o->vec) {
        if (size == 8) {
            a64_try_ands_imm(b, 1, A64_ZR, o->ra, 3);
            uint32_t *to_bytes = a64_label(b); a64_bcond(b, A64_NE, 0);
            emit_misaligned_pieces_st(b, 4, 2, o->rv, o->ra, o->disp, s1);
            uint32_t *to_done = a64_label(b); a64_b(b, 0);
            a64_patch_bcond(to_bytes, a64_label(b));
            emit_misaligned_pieces_st(b, 1, 8, o->rv, o->ra, o->disp, s1);
            a64_patch_b(to_done, a64_label(b));
        } else {
            emit_misaligned_pieces_st(b, 1, size, o->rv, o->ra, o->disp, s1);
        }
        return;
    }
    int nh = size == 16 ? 2 : 1, hs = size == 4 ? 4 : 8;
    if (size == 4)  a64_fmov_x_from_v(b, 0, s1, o->rv); else a64_fmov_x_from_v(b, 1, s1, o->rv);
    if (nh == 2)    a64_umov_gpr(b, 8, s2, o->rv, 1);
    uint32_t *to_done[2] = { NULL, NULL };
    for (int level = 0; level < 3; level++) {
        int psize = level == 0 ? 4 : level == 1 ? 2 : 1;
        uint32_t *to_next = NULL;
        if (level < 2) {
            a64_try_ands_imm(b, 1, A64_ZR, o->ra, (uint64_t)(psize - 1));
            to_next = a64_label(b); a64_bcond(b, A64_NE, 0);
        }
        for (int h = 0; h < nh; h++) {
            int r = h ? s2 : s1;
            for (int i = 0; i < hs / psize; i++) {
                if (i) a64_lsr_imm(b, 1, r, r, psize * 8);
                a64_stlur(b, psize, r, o->ra, o->disp + h * 8 + i * psize);
            }
        }
        if (level < 2) { to_done[level] = a64_label(b); a64_b(b, 0); a64_patch_bcond(to_next, a64_label(b)); }
    }
    a64_patch_b(to_done[0], a64_label(b));
    a64_patch_b(to_done[1], a64_label(b));
}

void emit_guard_arms(A64Buf *b, const uint32_t *entry)
{
    for (int k = 0; k < g_n_garm; k++) {
        uint32_t *lo = a64_label(b);
        a64_patch_cbz(g_garm[k].site, lo);
        emit_guard_full(b, g_garm[k].reg);
        uint32_t *here = a64_label(b);
        a64_b(b, (int32_t)(g_garm[k].back - here));
        if (g_n_fpbmap < JIT_MAX_BLOCK_INSNS) {
            g_fpbmap[g_n_fpbmap].lo = (uint32_t)(lo - entry);
            g_fpbmap[g_n_fpbmap].hi = (uint32_t)(a64_label(b) - entry);
            g_fpbmap[g_n_fpbmap].idx = g_garm[k].idx;
            g_n_fpbmap++;
        }
    }
    g_n_garm = 0;
}

void emit_ordered_slow_arms(A64Buf *b, JitBlock *blk, const uint32_t *entry)
{
    if (g_n_oslow <= 0 && g_n_fpbmap <= 0)
        return;
    blk->oslow = (struct JitOslowMap *)malloc((size_t)(g_n_oslow + g_n_fpbmap) * sizeof *blk->oslow);
    blk->n_oslow = 0;
    if (blk->oslow) {
        for (int i = 0; i < g_n_fpbmap; i++)
            blk->oslow[blk->n_oslow++] = g_fpbmap[i];
    }
    g_n_fpbmap = 0;
    for (int i = 0; i < g_n_oslow; i++) {
        const OrderedSlowPend *o = &g_oslow[i];
        uint32_t *lo = a64_label(b);
        a64_patch_bcond(o->bne, lo);
        int cand[3] = { JTF, JTT, JTU }, sc[2], nsc = 0;
        for (int k = 0; k < 3 && nsc < 2; k++)
            if (cand[k] != o->rv && cand[k] != o->ra) sc[nsc++] = cand[k];
        a64_stp_pre(b, sc[0], sc[1], 31, -16);
        emit_misaligned_arm(b, o);
        a64_ldp_post(b, sc[0], sc[1], 31, 16);
        uint32_t *here = a64_label(b);
        a64_b(b, (int32_t)(o->back - here));
        if (blk->oslow) {
            blk->oslow[blk->n_oslow].lo = (uint32_t)(lo - entry);
            blk->oslow[blk->n_oslow].hi = (uint32_t)(a64_label(b) - entry);
            blk->oslow[blk->n_oslow].idx = o->idx;
            blk->n_oslow++;
        }
    }
    g_n_oslow = 0;
}

/*
 * ---- x87 ----
 * The x87 registers stay where src/x87.c keeps them, in the cpu struct: ST(i)
 * is fpr[(ftop + i) & 7] at x20 + 8p.  fcw, fsw, the abridged tag word, ftop
 * and the image bits fpr_x_ok are one aligned 8-byte word, which a run of
 * translated instructions loads once into X87S and every instruction that
 * changes it stores back.  TOP inside a run is the run's first TOP plus the
 * pushes and pops the translator counted, read from a halfword the previous run
 * left (jit_x87_top0) and checked against X87S by a predicted branch, so the
 * addresses never wait on X87S: every tag and image bit lands there, and as one
 * serial chain carried from run to run it set the pace of an x87 loop.  For the
 * same reason the run tracks what it knows of each register's tag and image
 * bit, and of C1, and emits no update that would change nothing; a register
 * whose image is known invalid is copied and exchanged without its image.
 * winbench's i686 double kernel went from 2160 to 1066 ms under Wine this way
 * (Rosetta 13050, Rosetta with rosettax87_jit 483).  It leaves exactly
 * what the interpreter leaves - values, TOP, tags, the valid 80-bit images and
 * their validity bits (cleared by every write, carried by FLD ST(i), FST ST(i),
 * FXCH and FCMOVcc), the condition codes and the exception flags - and before it
 * writes anything it hands the interpreter what it cannot do exactly: a NaN
 * anywhere, an infinite result, a product or quotient within 2^53 of the
 * subnormal range (an exact zero stays), an integer store out of range or from
 * a register whose exact image is valid, and every form it does not translate.
 *
 * fsw stays exact without reading FPSR, whose writes cost 15 ns each.  The
 * invalid, divide-by-zero, overflow and underflow flags can only come from
 * results the interpreter takes, and inexact is decided out of line, only
 * while PE is still clear, with error-free transformations: r - a and r - b
 * against the operands of a sum, a fused multiply-subtract for a product, a
 * quotient or a square root.  Precision control 24, which Direct3D 9 leaves on
 * its thread, is a test per operation and an out-of-line tail that rounds the
 * double's significand to 24 bits in integer arithmetic, as pc_round does.
 * Consecutive translated x87 instructions form a run with one guard, rounding
 * to nearest in the control word and in MXCSR (host FPCR follows MXCSR),
 * emitted only when something in the run rounds; FLDCW and FNINIT end a run.
 * Every exit from a run, the guard's or an instruction's, enters one call that
 * interprets from that instruction to the end of the run
 * (ocerz_jit_exec_run_at).  An fcmovcc right after fcomi(p) branches on that
 * compare's own flags.  FILD qword followed by FISTP qword, Delphi's
 * eight-byte Move(), stores the integer rebuilt from the image FILD just made,
 * so the copy is exact.  32-bit blocks take the same forms through
 * x87_inline_ok.  On xbench's x87 kernel, twenty-two x87 instructions an
 * element, 10,000 rounds took 1.15 s through the interpreter and take 0.16 s,
 * where Rosetta takes 0.73 s.  Keeping ST(i) in V registers across a run
 * instead, written through so that every exit stays exact, measured 6% slower,
 * so the values stay in memory.  OCERZ_NO_JIT_X87=1 interprets x87 again.
 *
 *
 * Moving c into its fixed lane writes that lane, so c waits while another
 * pending register still lives there: movaps xmm3, xmm2 leaves xmm3 in
 * xmm2's lane, and when xmm2 is then zeroed and a back edge restores it,
 * writing xmm2's lane first fed xmm3 the zero.  A float loop of compares
 * and branches lost its running sum that way.
 *
 * fist(p) reads RC as it converts, so a run of them needs no round-to-nearest guard
 *
 * a known physical register: the bits are constants
 *
 * with a known TOP the registers' slots are constant offsets from x20
 *
 * RC, bits 10-11 of the control word in X87S: nearest, down, up, chop
 *
 * C1: the result differs from the truncated value, so it rounded away from zero
 *
 * the two lanes trade roles; memory takes both values
 *
 * The images' bytes swap only when one of the two is valid, and the
 * bits flip only when they differ: decided here when the run knows
 * both, at run time otherwise.
 *
 * Never or always moving is what the fast compare's ordered result
 * makes of the condition; the run's slow path, which the unordered
 * case takes, may decide otherwise, so ST(0) is not known after it.
 *
 * TOP is 0 and every register empty; the image bits stay, whatever the run knew of them
 *
 * stored here: the instruction's own store may find nothing inline changed X87S
 *
 * fall through
 *
 * The block was entered with a TOP other than the one it was
 * translated for: leave after the run with side_idx -3, and C
 * retranslates the block without the known TOP.
 *
 * The x87 control word as X87S holds it: fcw, fsw << 16, ftw << 32, ftop << 40, fpr_x_ok << 48.
 *
 * the registers a fragment finds the operands in (VX0, VX1 unless lanes)
 *
 * the open run's guard has checked RC is round to nearest
 *
 * TOP as the block expects it here: the TOP the translator saw at the block's
 * entry plus each run's pushes and pops, or -1.  A run that knows it checks
 * TOP once against the constant at its open (a mismatch interprets the run)
 * and then names every register by a constant physical number.
 *
 * x87 values in registers.  A block with x87 runs and no SSE borrows eight
 * lanes (v8-v15, whose low halves C calls keep) for the eight physical
 * registers.  g_x87_lv says which lanes hold fpr[p]: a run with a known TOP
 * reads a register from its lane, loading it the first time, and every value
 * it stores to fpr[] goes to the lane as well, so memory stays exact for every
 * exit and the next instruction reads a register instead of waiting on the
 * store.  FXCH swaps two lanes' roles; a run's slow path reloads the lanes on
 * its way back; anything else that may write the x87 registers drops them.
 *
 * What a translated instruction needs from its run: 0 when it is not translated.
 *
 * A branch to the interpreter for the rest of the run, from instruction idx on.
 *
 * A branch to an out-of-line tail that comes back at the next x87_frag_land.
 *
 * Inside a run X87S already holds the control word: x87_run_open loaded it and
 * every instruction that changes it stores it back, so there is nothing to load,
 * except after x87_reload's callers.  OCERZ_X87_CHECK=1 traps where the register
 * and memory disagree.
 *
 * X87S back to memory, unless no word since memory last matched it (the run's
 * load, a reload, the last store) may have written it: a run of arithmetic on
 * registers whose tags it already knows changes nothing there.
 *
 * After a shared emitter that may use X87S's register as scratch (flag predicates, GPR writes).
 *
 * The physical number of ST(k).  Inside a run TOP is the run's first TOP plus
 * pushes and pops counted here (g_x87_delta), so it comes from a halfword the
 * run stored once and not from X87S: every tag and image bit an instruction
 * sets lands in X87S, and the next instruction's addresses would wait for them.
 *
 * ST(rel) as a register: its lane, loaded from fpr[] the first time; -1 without lanes.
 *
 * The value in v has just been stored to ST(rel)'s slot: its lane takes it too.
 *
 * ST(rel) into vd: a move from its lane, or a load from the slot at addr.
 *
 * What a run knows of each register's tag and image bit, by its offset from
 * the run's first TOP: 0 unknown, 1 set, 2 clear; and whether C1 is known
 * clear.  An update that would change nothing is not emitted.  X87S is one
 * serial chain, and its length per iteration is what an x87 loop runs at, so a
 * register that an earlier instruction of the run already tagged and cleared
 * costs nothing when it is written again.
 *
 * ST(i)'s index into what the run knows: its physical number in a run with a known TOP, else its offset from the run's first TOP.
 *
 * what the last run knew holds at this run's open: both had a known TOP
 *
 * Physical register rp, ST(rel) of the run, written: tagged, its image bit set or cleared; rt a scratch.
 *
 * copy_st: value, image and image bit of physical rs into physical rd, rd tagged.
 * The image's bytes move only while its bit says it is valid; a register whose
 * bit is clear never has its image read, and arithmetic clears it.
 *
 * A push whose value is in vv; with an image, its significand and sign/exponent are in rm and rs.
 *
 * int_to_f80 of the integer in rv: significand to rm, sign and exponent to rs.
 *
 * VX2 holds the result of VX0 op VX1 (or the square root of VX0), its bits in
 * JT0 afterwards.  Results the fast path cannot flag exactly go to the
 * interpreter; precision control 24 and a clear PE leave through tails.
 *
 * FIST, FISTP and FISTTP; a courier FISTP stores the integer the preceding FILD qword imaged.
 *
 * An fcmovcc condition after an ordered fcmp: A64_AL moves always, A64_NV never.
 *
 * Opens the run that starts at the current instruction: its extent, its guard.
 *
 * Each run that can leave its fast path gets one call into the interpreter for
 * the rest of the run, entered with the index of the first instruction to run.
 */
#include "ocerz/jit_internal.h"

static void emit_nan_cold_scalar(A64Buf *b, int dbl, int vr, int va, int vb);
static void emit_nan_cold_packed(A64Buf *b, int dbl, int vr, int va, int vb, int t1);
static int mem_may_alias(const X86Insn *ia, const X86Operand *a, const X86Insn *ib, const X86Operand *b);
static int fpb_region_class(const X86Insn *in);
static int fpb_class(const X86Insn *in, int *packed, int *dbl, int *from_mem, int *sqrt_like);
static int fpb2_kind(const X86Insn *in, int *packed, int *dbl, int *from_mem, int *sq, int *lane_only);
static void fpb2_usedef(const X86Insn *in, uint16_t *use, uint16_t *kill);
static int fpb2_gpr_after_ok(const X86Insn *in, uint16_t gprs);
static int mem_same(const X86Operand *a, const X86Operand *b);
static inline uint16_t fpb2_membits(const X86Operand *m);
static int fpb_undo_lane(int slot);
static int fpb2_undo_ok(const X86Insn *in, const X86Operand *m, int size);
static int insn_writes_xmm0(const X86Insn *in);
static int mov128_pair_kind(const X86Insn *a, const X86Insn *c);
static int l0_aware_op(unsigned op);
static int x87_mem_ok(const X86Insn *in, const X86Operand *o, int s1, int s2, int s3);
static void x87_slow_at(A64Buf *b, int cond, int run, int idx);
static void x87_slow_if(A64Buf *b, int cond);
static void x87_frag_if(A64Buf *b, int cond, int kind, int op);
static void x87_frag_land(A64Buf *b);
static void x87_ld(A64Buf *b);
static void x87_st(A64Buf *b);
static void x87_reload(A64Buf *b);
static void x87_top_at(A64Buf *b, int rd, int k);
static void x87_top(A64Buf *b, int rd);
static void x87_phys(A64Buf *b, int rd, int i);
static void x87_newtop(A64Buf *b, int rd);
static void x87_slot(A64Buf *b, int rd, int rp);
static int x87_lane_of(int rel);
static int x87_lane_get(A64Buf *b, int rel);
static void x87_lane_put(A64Buf *b, int rel, int v);
static void x87_lane_read(A64Buf *b, int vd, int rel, int addr);
static void x87_bit(A64Buf *b, int rd, int rp);
static int x87_rel(int i);
static void x87_know_reset(void);
static void x87_clear_c1(A64Buf *b);
static void x87_tag(A64Buf *b, int rp, int rt, int rel, int image);
static void x87_pop(A64Buf *b);
static void x87_copy(A64Buf *b, int rd, int rs, int rdrel, int rsrel);
static void x87_push(A64Buf *b, int vv, int image, int rm, int rs);
static void x87_int_image(A64Buf *b, int rv, int rm, int rs, int rt);
static int x87_ld_int(A64Buf *b, const X86Insn *insn, int rd, int sext, uint32_t **exit_sites, int *n_exits);
static int x87_ld_real(A64Buf *b, const X86Insn *insn, int vd, uint32_t **exit_sites, int *n_exits);
static int x87_st_mem(A64Buf *b, const X86Insn *insn, int vec, int reg, uint32_t **exit_sites, int *n_exits);
static void x87_result(A64Buf *b, int kind);
static int x87_arith(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int x87_compare(A64Buf *b, const X86Insn *insn, uint64_t need, uint32_t **exit_sites, int *n_exits);
static int x87_fist(A64Buf *b, const X86Insn *insn, int courier, uint32_t **exit_sites, int *n_exits);
static int x87_fcmov_cond(unsigned cc);
static int emit_x87_one(A64Buf *b, const X86Insn *insn, uint64_t need, uint32_t **exit_sites, int *n_exits);
static int x87_run_open(A64Buf *b, int idx);
static void x87_frag_exact(A64Buf *b, int op, int run, int idx, int r0, int r1, uint32_t **pe, int *np, uint32_t **ok, int *nok);
static void x87_emit_frag(A64Buf *b, int f);

NanOolPend g_nanool[NANOOL_MAX];

int g_n_nanool;

int g_cur_insn_idx;

JitBlock *g_cur_blk;

uint8_t *g_keep;

int g_keep_n;

uint16_t g_lane_used;

int g_l0_nlanes = L0_NLANES;

int8_t g_undo_vreg[FPB_UNDO_MAX] = {0};

int g_n_undo_lanes;

const X86Insn *g_cur_insns;

int g_cur_insns_n;

int g_fcmp_self_vreg = -1;

int g_fcmp_self_idx = -1;

static void emit_nan_cold_scalar(A64Buf *b, int dbl, int vr, int va, int vb)
{
    uint64_t quiet = dbl ? 0x0008000000000000ull : 0x00400000ull;
    uint64_t dflt  = dbl ? 0xfff8000000000000ull : 0xffc00000ull;
    a64_fcmp(b, dbl, va, va);
    uint32_t *a_ok = a64_label(b); a64_bcond(b, A64_VC, 0);
    a64_fmov_x_from_v(b, dbl, JT0, va);
    uint32_t *use = a64_label(b); a64_b(b, 0);
    a64_patch_bcond(a_ok, a64_label(b));
    a64_fcmp(b, dbl, vb, vb);
    uint32_t *b_ok = a64_label(b); a64_bcond(b, A64_VC, 0);
    a64_fmov_x_from_v(b, dbl, JT0, vb);
    uint32_t *use2 = a64_label(b); a64_b(b, 0);
    a64_patch_bcond(b_ok, a64_label(b));
    a64_mov_imm64(b, JT0, dflt);
    a64_fmov_v_from_x(b, dbl, vr, JT0);
    uint32_t *done = a64_label(b); a64_b(b, 0);
    a64_patch_b(use, a64_label(b));
    a64_patch_b(use2, a64_label(b));
    a64_mov_imm64(b, JT1, quiet);
    a64_orr_reg(b, dbl, JT0, JT0, JT1, 0);
    a64_fmov_v_from_x(b, dbl, vr, JT0);
    a64_patch_b(done, a64_label(b));
}

struct JitState_g_scpend g_scpend;

int g_scalar_merge_next;

void emit_nan_fix_scalar2(A64Buf *b, int dbl, int vr, int va, int vb)
{
    a64_fcmp(b, dbl, vr, vr);
    g_fcmp_self_vreg = vr;
    g_fcmp_self_idx = g_cur_insn_idx + 1;
    if (g_scalar_merge_next) {
        g_scalar_merge_next = 0;
        g_scpend.valid = 1; g_scpend.idx = g_cur_insn_idx; g_scpend.dbl = dbl;
        g_scpend.vr = vr; g_scpend.va = va; g_scpend.vb = vb;
        return;
    }
    if (g_n_nanool < NANOOL_MAX) {
        NanOolPend *o = &g_nanool[g_n_nanool++];
        o->site = a64_label(b); a64_bcond(b, A64_VS, 0);
        o->back = a64_label(b);
        o->dbl = (uint8_t)dbl; o->packed = 0; o->vr = (uint8_t)vr; o->va = (uint8_t)va; o->vb = (uint8_t)vb; o->t1 = 0;
        o->cvt = 0; o->refcmp = 1; o->pre = 0; o->idx = g_cur_insn_idx; o->is_cbz = 0;
        return;
    }
    uint32_t *ok = a64_label(b); a64_bcond(b, A64_VC, 0);
    emit_nan_cold_scalar(b, dbl, vr, va, vb);
    a64_fcmp(b, dbl, vr, vr);
    a64_patch_bcond(ok, a64_label(b));
}

int g_pk_consts_needed;

static void emit_nan_cold_packed(A64Buf *b, int dbl, int vr, int va, int vb, int t1)
{
    a64_sub_imm(b, 1, 31, 31, 48);
    a64_str_v(b, 16, va, 31, 0);
    a64_str_v(b, 16, vb, 31, 16);
    a64_str_v(b, 16, vr, 31, 32);
    int lanes = dbl ? 2 : 4, esz = dbl ? 8 : 4;
    uint64_t quiet = dbl ? 0x0008000000000000ull : 0x00400000ull;
    uint64_t dflt  = dbl ? 0xfff8000000000000ull : 0xffc00000ull;
    for (int l = 0; l < lanes; l++) {
        uint32_t oa = (uint32_t)(0 + l * esz), ob = (uint32_t)(16 + l * esz), orr_ = (uint32_t)(32 + l * esz);
        a64_ldr_v(b, esz, t1, 31, orr_);
        a64_fcmp(b, dbl, t1, t1);
        uint32_t *lane_ok = a64_label(b); a64_bcond(b, A64_VC, 0);
        a64_ldr_v(b, esz, t1, 31, oa);
        a64_fcmp(b, dbl, t1, t1);
        uint32_t *a_ok = a64_label(b); a64_bcond(b, A64_VC, 0);
        a64_ldr(b, esz, JT0, 31, oa);
        a64_mov_imm64(b, JT1, quiet); a64_orr_reg(b, dbl, JT0, JT0, JT1, 0);
        uint32_t *w1 = a64_label(b); a64_b(b, 0);
        a64_patch_bcond(a_ok, a64_label(b));
        a64_ldr_v(b, esz, t1, 31, ob);
        a64_fcmp(b, dbl, t1, t1);
        uint32_t *b_ok = a64_label(b); a64_bcond(b, A64_VC, 0);
        a64_ldr(b, esz, JT0, 31, ob);
        a64_mov_imm64(b, JT1, quiet); a64_orr_reg(b, dbl, JT0, JT0, JT1, 0);
        uint32_t *w2 = a64_label(b); a64_b(b, 0);
        a64_patch_bcond(b_ok, a64_label(b));
        a64_mov_imm64(b, JT0, dflt);
        a64_patch_b(w1, a64_label(b));
        a64_patch_b(w2, a64_label(b));
        a64_str(b, esz, JT0, 31, orr_);
        a64_patch_bcond(lane_ok, a64_label(b));
    }
    a64_ldr_v(b, 16, vr, 31, 32);
    a64_add_imm(b, 1, 31, 31, 48);
}

void emit_nan_fix_packed2(A64Buf *b, int dbl, int vr, int va, int vb, int t1, int t2)
{
    (void)t2;
    a64_v_fcmeq(b, dbl, t1, vr, vr);
    a64_v_xtn(b, dbl ? 2 : 1, t1, t1);
    a64_fmov_x_from_v(b, 1, JT0, t1);
    a64_cmn_imm(b, 1, JT0, 1);
    if (g_n_nanool < NANOOL_MAX) {
        NanOolPend *o = &g_nanool[g_n_nanool++];
        o->site = a64_label(b); a64_bcond(b, A64_NE, 0);
        o->back = a64_label(b);
        o->dbl = (uint8_t)dbl; o->packed = 1; o->vr = (uint8_t)vr; o->va = (uint8_t)va; o->vb = (uint8_t)vb; o->t1 = (uint8_t)t1;
        o->cvt = 0; o->refcmp = 0; o->pre = 0; o->idx = g_cur_insn_idx; o->is_cbz = 0;
        return;
    }
    uint32_t *ok = a64_label(b); a64_bcond(b, A64_EQ, 0);
    emit_nan_cold_packed(b, dbl, vr, va, vb, t1);
    a64_patch_bcond(ok, a64_label(b));
}

void emit_nan_ool_arms(A64Buf *b, JitBlock *blk, const uint32_t *entry)
{
    (void)blk; (void)entry;
    for (int i = 0; i < g_n_nanool; i++) {
        const NanOolPend *o = &g_nanool[i];
        uint32_t *lo = a64_label(b);
        if (o->is_cbz) a64_patch_cbz(o->site, lo); else a64_patch_bcond(o->site, lo);
        if (o->pre) {
            a64_fcmp(b, o->dbl, o->pvr, o->pvr);
            uint32_t *sk = a64_label(b); a64_bcond(b, A64_VC, 0);
            emit_nan_cold_scalar(b, o->dbl, o->pvr, o->pva, o->pvb);
            a64_patch_bcond(sk, a64_label(b));
        }
        if (o->cvt == 1) {
            a64_movz(b, o->vr, 0x8000, 3);
        } else if (o->cvt == 2) {
            a64_fcvtzs(b, 1, o->dbl, JT0, o->va);
            a64_cmp_ext_sxtw(b, JT0, JT0);
            a64_movz(b, JTU, 0x8000, 1);
            a64_csel(b, 0, o->vr, JTU, JT0, A64_NE);
            a64_fcmp(b, o->dbl, o->va, o->va);
            a64_csel(b, 0, o->vr, JTU, o->vr, A64_VS);
        } else if (o->packed) emit_nan_cold_packed(b, o->dbl, o->vr, o->va, o->vb, o->t1);
        else {
            emit_nan_cold_scalar(b, o->dbl, o->vr, o->va, o->vb);
            if (o->refcmp) a64_fcmp(b, o->dbl, o->vr, o->vr);
        }
        uint32_t *here = a64_label(b);
        a64_b(b, (int32_t)(o->back - here));
    }
    g_n_nanool = 0;
}

FpBatch g_fpb[FPB_MAX];

int g_n_fpb;

FpbSite g_fpb_sites[FPB_SITES_MAX];

int g_n_fpb_sites;

uint8_t g_fpb_member[JIT_MAX_BLOCK_INSNS] = {0};

uint8_t g_fpb_det[JIT_MAX_BLOCK_INSNS] = {0};

uint16_t g_fpb_sidechk[JIT_MAX_BLOCK_INSNS];

static uint16_t g_fpb_mrd[JIT_MAX_BLOCK_INSNS];

static uint16_t g_fpb_mwr[JIT_MAX_BLOCK_INSNS];

int mov_sink_gap_ok(const X86Insn *in, unsigned dreg, unsigned sreg)
{
    switch (in->op) {
    case OCERZ_OP_MOV: case OCERZ_OP_MOVZX: case OCERZ_OP_MOVSX: case OCERZ_OP_LEA:
    case OCERZ_OP_ADD: case OCERZ_OP_SUB: case OCERZ_OP_AND: case OCERZ_OP_OR: case OCERZ_OP_XOR:
    case OCERZ_OP_CMP: case OCERZ_OP_TEST: case OCERZ_OP_INC: case OCERZ_OP_DEC:
    case OCERZ_OP_NEG: case OCERZ_OP_NOT: case OCERZ_OP_SHL: case OCERZ_OP_SHR: case OCERZ_OP_SAR:
    case OCERZ_OP_ADDSS: case OCERZ_OP_ADDSD: case OCERZ_OP_SUBSS: case OCERZ_OP_SUBSD:
    case OCERZ_OP_MULSS: case OCERZ_OP_MULSD: case OCERZ_OP_DIVSS: case OCERZ_OP_DIVSD:
    case OCERZ_OP_SQRTSS: case OCERZ_OP_SQRTSD: case OCERZ_OP_MINSS: case OCERZ_OP_MINSD:
    case OCERZ_OP_MAXSS: case OCERZ_OP_MAXSD:
    case OCERZ_OP_ADDPS: case OCERZ_OP_ADDPD: case OCERZ_OP_SUBPS: case OCERZ_OP_SUBPD:
    case OCERZ_OP_MULPS: case OCERZ_OP_MULPD: case OCERZ_OP_DIVPS: case OCERZ_OP_DIVPD:
    case OCERZ_OP_CVTTSS2SI: case OCERZ_OP_CVTTSD2SI:
    case OCERZ_OP_UCOMISS: case OCERZ_OP_UCOMISD: case OCERZ_OP_COMISS: case OCERZ_OP_COMISD:
    case OCERZ_OP_MOVAPS: case OCERZ_OP_MOVUPS: case OCERZ_OP_MOVDQA: case OCERZ_OP_MOVDQU:
    case OCERZ_OP_MOVSS: case OCERZ_OP_MOVSDX: case OCERZ_OP_UNPCKHPD: case OCERZ_OP_UNPCKLPD:
        break;
    default:
        return 0;
    }
    if (in->seg != OCERZ_SEG_NONE) return 0;
    for (int k = 0; k < in->nops; k++) {
        const X86Operand *o = &in->ops[k];
        if (o->kind == OCERZ_OPK_MEM) {
            if (in->op != OCERZ_OP_LEA) return 0;
            if (o->base == dreg || o->index == dreg) return 0;
            continue;
        }
        if (o->kind != OCERZ_OPK_REG) continue;
        if ((o->reg & 15) == (dreg & 15)) return 0;
        if (k == 0 && (o->reg & 15) == (sreg & 15)) return 0;
    }
    if ((in->op == OCERZ_OP_SHL || in->op == OCERZ_OP_SHR || in->op == OCERZ_OP_SAR) &&
        in->ops[1].kind != OCERZ_OPK_IMM) return 0;
    return 1;
}

void mov_sink_scan(const X86Insn *insns, int n, const uint64_t *fl_need)
{
    for (int i = 0; i < n; i++) { g_mov_sink_at[i] = -1; g_mov_skip[i] = 0; }
    static int dis = -1; if (dis < 0) dis = (getenv("OCERZ_NO_MOVFUSE") || getenv("OCERZ_NO_MOVSINK")) ? 1 : 0;
    if (dis || !g_defer) return;
    for (int i = 0; i + 1 < n; i++) {
        const X86Insn *m = &insns[i];
        if (m->op != OCERZ_OP_MOV || m->nops != 2) continue;
        const X86Operand *md = &m->ops[0], *ms = &m->ops[1];
        if (md->kind != OCERZ_OPK_REG || ms->kind != OCERZ_OPK_REG || md->high8 || ms->high8) continue;
        if ((md->size != 4 && md->size != 8) || ms->size != md->size || md->reg == ms->reg) continue;
        if (pin_slot(md->reg) < 0 || pin_slot(ms->reg) < 0) continue;
        if (rsp_is_ptr() && (md->reg == OCERZ_RSP || ms->reg == OCERZ_RSP)) continue;
        for (int j = i + 2; j < n && j <= i + 5; j++) {
            const X86Insn *t = &insns[j];
            if (!mov_sink_gap_ok(&insns[j - 1], md->reg, ms->reg)) break;
            if ((t->op == OCERZ_OP_SHL || t->op == OCERZ_OP_SHR || t->op == OCERZ_OP_SAR) &&
                t->ops[0].kind == OCERZ_OPK_REG && !t->ops[0].high8 && t->ops[0].reg == md->reg &&
                t->ops[0].size == md->size && t->ops[1].kind == OCERZ_OPK_IMM && fl_need[j] == 0 &&
                (t->ops[1].imm & (md->size == 8 ? 63u : 31u)) != 0) {
                g_mov_sink_at[j] = (int16_t)i;
                g_mov_skip[i] = 1;
                break;
            }
            if (!mov_sink_gap_ok(t, md->reg, ms->reg)) break;
        }
    }
}

static uint8_t g_fpb_marith[JIT_MAX_BLOCK_INSNS];

static uint8_t g_fpb_mmem[JIT_MAX_BLOCK_INSNS];

int g_fpb_open = -1;

const int8_t *g_fpb_of;

static int mem_may_alias(const X86Insn *ia, const X86Operand *a, const X86Insn *ib, const X86Operand *b)
{
    if (a->riprel && b->riprel) {
        int64_t xa = (int64_t)(ia->rip + ia->len) + a->disp, xb = (int64_t)(ib->rip + ib->len) + b->disp;
        return xa < xb + 16 && xb < xa + 16;
    }
    if (a->riprel || b->riprel) return 1;
    if (a->base != b->base || a->index != b->index || a->scale != b->scale) return 1;
    return a->disp < b->disp + 16 && b->disp < a->disp + 16;
}

static int fpb_region_class(const X86Insn *in)
{
    if (in->vex || in->seg != OCERZ_SEG_NONE || in->nops < 2) return RK_END;
    const X86Operand *d = &in->ops[0], *sr = &in->ops[1];
    int dx = d->kind == OCERZ_OPK_XMM && xmm_is_pinned(d->reg);
    int sx = sr->kind == OCERZ_OPK_XMM && xmm_is_pinned(sr->reg);
    int dm = d->kind == OCERZ_OPK_MEM && in->addrsize == 8;
    int sm = sr->kind == OCERZ_OPK_MEM && (sr->riprel || in->addrsize == 8);
    switch (in->op) {
    case OCERZ_OP_UCOMISD: case OCERZ_OP_COMISD:
        return dx && sx ? RK_DET : RK_END;
    case OCERZ_OP_MOVUPS: case OCERZ_OP_MOVAPS: case OCERZ_OP_MOVDQA: case OCERZ_OP_MOVDQU:
        if (dx && sx) return RK_MOVE;
        if (dm && sx) return RK_STORE;
        if (dx && sm) return RK_LOAD;
        return RK_END;
    case OCERZ_OP_MOVSDX:
        if (dx && sx) return RK_LMOVE;
        if (dm && sx) return RK_STORE;
        if (dx && sm) return RK_LOAD;
        return RK_END;
    case OCERZ_OP_MOVLPS: case OCERZ_OP_MOVHPS:
        return dm && sx ? RK_STORE : RK_END;
    case OCERZ_OP_UNPCKHPD: return dx && sx && d->reg == sr->reg ? RK_UNPCKH : RK_END;
    case OCERZ_OP_UNPCKLPD: return dx && sx && d->reg == sr->reg ? RK_UNPCKL : RK_END;
    default: return RK_END;
    }
}

static int g_fpb_disabled = -1;

int g_fpb_v1_active;

static int fpb_class(const X86Insn *in, int *packed, int *dbl, int *from_mem, int *sqrt_like)
{
    *packed = *dbl = *from_mem = *sqrt_like = 0;
    if (in->seg != OCERZ_SEG_NONE || in->nops < 2) return 0;
    if (in->vex && g_fpb_v1_active) return 0;
    if (in->vex && ((in->vex & OCERZ_VEX_L) || in->mode32 || in->nops != 2 ||
                    ((in->vex & OCERZ_VEX_NDS) && !xmm_is_pinned(in->vvvv & 15)))) return 0;
    const X86Operand *d = &in->ops[0], *sr = &in->ops[1];
    if (d->kind != OCERZ_OPK_XMM || !xmm_is_pinned(d->reg)) return 0;
    if (sr->kind == OCERZ_OPK_MEM) {
        if (sr->riprel) { *from_mem = 1; }
        else if (in->addrsize != 8) return 0;
        else *from_mem = 1;
    } else if (sr->kind != OCERZ_OPK_XMM || !xmm_is_pinned(sr->reg)) return 0;
    if (in->op >= OCERZ_OP_VFMA_FIRST && in->op <= OCERZ_OP_VFMA_LAST) {
        int idx = (int)(in->op - OCERZ_OP_VFMA_FIRST), kind = (idx >> 1) % 10;
        if (kind < 2 || !(in->vex & OCERZ_VEX_NDS)) return 0;
        *dbl = idx & 1;
        *packed = !(kind & 1);
        return 1;
    }
    switch (in->op) {
    case OCERZ_OP_ADDSS: case OCERZ_OP_SUBSS: case OCERZ_OP_MULSS: case OCERZ_OP_DIVSS:
    case OCERZ_OP_MAXSS: case OCERZ_OP_MINSS: return 1;
    case OCERZ_OP_ADDSD: case OCERZ_OP_SUBSD: case OCERZ_OP_MULSD: case OCERZ_OP_DIVSD:
    case OCERZ_OP_MAXSD: case OCERZ_OP_MINSD: *dbl = 1; return 1;
    case OCERZ_OP_ADDPS: case OCERZ_OP_SUBPS: case OCERZ_OP_MULPS: case OCERZ_OP_DIVPS:
    case OCERZ_OP_MAXPS: case OCERZ_OP_MINPS: *packed = 1; return 1;
    case OCERZ_OP_ADDPD: case OCERZ_OP_SUBPD: case OCERZ_OP_MULPD: case OCERZ_OP_DIVPD:
    case OCERZ_OP_MAXPD: case OCERZ_OP_MINPD: *packed = 1; *dbl = 1; return 1;
    case OCERZ_OP_SQRTSS: *sqrt_like = 1; return 1;
    case OCERZ_OP_SQRTSD: *sqrt_like = 1; *dbl = 1; return 1;
    case OCERZ_OP_SQRTPS: *sqrt_like = 1; *packed = 1; return 1;
    case OCERZ_OP_SQRTPD: *sqrt_like = 1; *packed = 1; *dbl = 1; return 1;
    case OCERZ_OP_MOVUPS: case OCERZ_OP_MOVAPS: case OCERZ_OP_MOVDQA: case OCERZ_OP_MOVDQU:
        return (in->vex & OCERZ_VEX_NDS) ? 0 : 2;
    case OCERZ_OP_MOVSS: return (in->vex & OCERZ_VEX_NDS) ? 0 : 3;
    case OCERZ_OP_MOVSDX: *dbl = 1; return (in->vex & OCERZ_VEX_NDS) ? 0 : 3;
    default: return 0;
    }
}

void fpb_scan_v1(const X86Insn *insns, int n, int8_t *bat)
{
    g_n_fpb = 0;
    g_n_fpb_sites = 0;
    for (int i = 0; i < n; i++) { bat[i] = -1; g_fpb_member[i] = 0; g_fpb_det[i] = 0; g_fpb_sidechk[i] = 0; }
    int planned_sites = 0;
    if (g_fpb_disabled < 0) g_fpb_disabled = getenv("OCERZ_NO_FPBATCH") ? 1 : 0;
    if (g_fpb_disabled || !sse_enabled() || !xmm_global_enabled()) return;
    int i = 0;
    while (i < n) {
        int packed, dbl, from_mem, sq;
        if (!fpb_class(&insns[i], &packed, &dbl, &from_mem, &sq)) { i++; continue; }
        int j = i;
        uint16_t written = 0, ckpt = 0, full = 0, s0 = 0, d0 = 0;
        int gain = 0, n_arith = 0;
        struct { uint8_t s, d, cls; int t; } edges[64]; int n_edges = 0;
        int lastw[16], lastbreak[16]; uint8_t taint_dbl[16];
        for (int r = 0; r < 16; r++) { lastw[r] = -1; lastbreak[r] = -1; taint_dbl[r] = 0; }
        while (j < n) {
            int c = fpb_class(&insns[j], &packed, &dbl, &from_mem, &sq);
            if (!c) break;
            const X86Operand *d = &insns[j].ops[0], *sr = &insns[j].ops[1];
            unsigned dr = d->reg;
            unsigned sbit = sr->kind == OCERZ_OPK_XMM ? (1u << sr->reg) : 0;
            if (c == 1 && sr->kind == OCERZ_OPK_XMM && sr->reg != dr && n_edges < 64) {
                int is_minmax_c = insns[j].op == OCERZ_OP_MAXSS || insns[j].op == OCERZ_OP_MINSS ||
                                  insns[j].op == OCERZ_OP_MAXSD || insns[j].op == OCERZ_OP_MINSD ||
                                  insns[j].op == OCERZ_OP_MAXPS || insns[j].op == OCERZ_OP_MINPS ||
                                  insns[j].op == OCERZ_OP_MAXPD || insns[j].op == OCERZ_OP_MINPD;
                unsigned sb = 1u << sr->reg;
                if (!is_minmax_c) {
                    if (packed && (full & sb) && taint_dbl[sr->reg] == (uint8_t)dbl) { edges[n_edges].s = (uint8_t)sr->reg; edges[n_edges].d = (uint8_t)dr; edges[n_edges].t = j; edges[n_edges].cls = 0; n_edges++; }
                    if (!dbl && (s0 & sb) && n_edges < 64) { edges[n_edges].s = (uint8_t)sr->reg; edges[n_edges].d = (uint8_t)dr; edges[n_edges].t = j; edges[n_edges].cls = 1; n_edges++; }
                    if (dbl && (d0 & sb) && n_edges < 64)  { edges[n_edges].s = (uint8_t)sr->reg; edges[n_edges].d = (uint8_t)dr; edges[n_edges].t = j; edges[n_edges].cls = 2; n_edges++; }
                }
            }
            if (c == 1) taint_dbl[dr] = (uint8_t)dbl;
            lastw[dr] = j;
            {
                int is_mm = insns[j].op == OCERZ_OP_MAXSS || insns[j].op == OCERZ_OP_MINSS ||
                            insns[j].op == OCERZ_OP_MAXSD || insns[j].op == OCERZ_OP_MINSD ||
                            insns[j].op == OCERZ_OP_MAXPS || insns[j].op == OCERZ_OP_MINPS ||
                            insns[j].op == OCERZ_OP_MAXPD || insns[j].op == OCERZ_OP_MINPD;
                if (c != 1 || is_mm || (sq && !(sr->kind == OCERZ_OPK_XMM && sr->reg == dr))) lastbreak[dr] = j;
            }
            uint16_t reads = (uint16_t)sbit;
            if (c == 1 || c == 3) reads |= (uint16_t)(1u << dr);
            if (c == 3 && from_mem) reads &= (uint16_t)~(1u << dr);
            ckpt |= (uint16_t)(reads & ~written);
            g_fpb_mrd[j] = reads; g_fpb_mwr[j] = (uint16_t)(1u << dr);
            g_fpb_mmem[j] = (uint8_t)from_mem; g_fpb_marith[j] = (uint8_t)(c == 1);
            uint16_t st_full = (uint16_t)(full & sbit), st_s0 = (uint16_t)(s0 & sbit), st_d0 = (uint16_t)(d0 & sbit);
            if (c == 1) {
                if (packed) { full |= (uint16_t)(1u << dr); s0 &= (uint16_t)~(1u << dr); d0 &= (uint16_t)~(1u << dr); }
                else if (dbl) { d0 |= (uint16_t)(1u << dr); s0 &= (uint16_t)~(1u << dr); }
                else          { s0 |= (uint16_t)(1u << dr); d0 &= (uint16_t)~(1u << dr); }
                int is_minmax = insns[j].op == OCERZ_OP_MAXSS || insns[j].op == OCERZ_OP_MINSS ||
                                insns[j].op == OCERZ_OP_MAXSD || insns[j].op == OCERZ_OP_MINSD ||
                                insns[j].op == OCERZ_OP_MAXPS || insns[j].op == OCERZ_OP_MINPS ||
                                insns[j].op == OCERZ_OP_MAXPD || insns[j].op == OCERZ_OP_MINPD;
                if (!is_minmax) { gain += packed ? 6 : 2; n_arith++; }
            } else if (c == 2) {
                if (from_mem) { full &= (uint16_t)~(1u << dr); s0 &= (uint16_t)~(1u << dr); d0 &= (uint16_t)~(1u << dr); }
                else {
                    full = (uint16_t)((full & ~(1u << dr)) | (st_full ? (1u << dr) : 0));
                    s0   = (uint16_t)((s0   & ~(1u << dr)) | (st_s0   ? (1u << dr) : 0));
                    d0   = (uint16_t)((d0   & ~(1u << dr)) | (st_d0   ? (1u << dr) : 0));
                }
            } else {
                if (from_mem) { full &= (uint16_t)~(1u << dr); s0 &= (uint16_t)~(1u << dr); d0 &= (uint16_t)~(1u << dr); }
                else {
                    if (st_full) full |= (uint16_t)(1u << dr);
                    if (dbl) { if (st_d0 || st_full) d0 |= (uint16_t)(1u << dr); s0 &= (uint16_t)~(1u << dr); }
                    else     { if (st_s0 || st_full) s0 |= (uint16_t)(1u << dr); d0 &= (uint16_t)~(1u << dr); }
                }
            }
            written |= (uint16_t)(1u << dr);
            j++;
        }
        int last = j - 1;
        while (last >= i) {
            int c = fpb_class(&insns[last], &packed, &dbl, &from_mem, &sq);
            if (c == 1) break;
            last--;
        }
        uint16_t ckpt_raw = ckpt;
        ckpt &= written;
        { static int noabs = -1; if (noabs < 0) noabs = getenv("OCERZ_NO_FPB_ABSORB") ? 1 : 0;
          if (!noabs) for (int e = 0; e < n_edges; e++) {
            int S = edges[e].s, D = edges[e].d, t = edges[e].t;
            if (t > last) continue;
            if (lastbreak[D] > t) continue;
            if (lastw[S] > t) continue;
            if (edges[e].cls == 0) full &= (uint16_t)~(1u << S);
            else if (edges[e].cls == 1) s0 &= (uint16_t)~(1u << S);
            else d0 &= (uint16_t)~(1u << S);
          } }
        int nregs = __builtin_popcount((unsigned)(full | s0 | d0));
        int cost = __builtin_popcount((unsigned)ckpt) + 3 + nregs;
        if (n_arith >= 1 && gain > cost && g_n_fpb < FPB_MAX && last >= i && (full | s0 | d0)) {
            FpBatch *fb = &g_fpb[g_n_fpb];
            fb->first = i; fb->last = last; fb->ckpt = ckpt; fb->ckpt_emit = ckpt; fb->written = written; fb->dirty_open = 0;
            fb->dblonly = 0;
            fb->full = full; fb->s0 = s0; fb->d0 = d0; fb->gain = gain - cost;
            fb->site = NULL; fb->back = NULL;
            fb->end = last;
            for (int k = i; k <= last; k++) { bat[k] = (int8_t)g_n_fpb; g_fpb_member[k] = 1; }
            int dbl_only = s0 == 0;
            for (int r = 0; r < 16 && dbl_only; r++)
                if ((full & (1u << r)) && !taint_dbl[r]) dbl_only = 0;
            static int nodefer = -1;
            if (nodefer < 0) nodefer = getenv("OCERZ_NO_FPB_DEFER") ? 1 : 0;
            if (dbl_only && !nodefer && !g_xlat_mode32) {
                uint8_t vid[16][2];
                memset(vid, 0, sizeof vid);
                for (int r = 0; r < 16; r++) {
                    if (full & (1u << r)) { vid[r][0] = (uint8_t)(1 + 2 * r); vid[r][1] = (uint8_t)(2 + 2 * r); }
                    else if (d0 & (1u << r)) vid[r][0] = (uint8_t)(1 + 2 * r);
                }
                uint16_t rck = ckpt_raw, rwr = written;
                int jj = last + 1, cnt = 0, left = 1, det_jcc = -1;
                while (jj < n && cnt < 24 && planned_sites < FPB_SITES_MAX - 2) {
                    const X86Insn *in = &insns[jj];
                    int rk = fpb_region_class(in);
                    uint16_t m = 0;
                    if (rk == RK_END) {
                        if (in->op != OCERZ_OP_JCC || jj != det_jcc) break;
                        for (int r = 0; r < 16; r++) if (vid[r][0] || vid[r][1]) m |= (uint16_t)(1u << r);
                        g_fpb_sidechk[jj] = m;
                        if (m) planned_sites++;
                        det_jcc = -1;
                        g_fpb_mrd[jj] = 0; g_fpb_mwr[jj] = 0;
                        jj++; cnt++;
                        continue;
                    }
                    unsigned dr = in->ops[0].kind == OCERZ_OPK_XMM ? in->ops[0].reg : 16;
                    unsigned sr = in->ops[1].kind == OCERZ_OPK_XMM ? in->ops[1].reg : 16;
                    uint16_t reads = 0, writes = 0;
                    if (rk == RK_STORE) {
                        int fa = -1, conflict = 0, shift = 0;
                        for (int k = fb->first; k <= last; k++) if (g_fpb_marith[k]) { fa = k; break; }
                        for (int k = fb->first; k <= last; k++)
                            if (g_fpb_mmem[k] && mem_may_alias(&insns[k], &insns[k].ops[1], in, &in->ops[0])) {
                                if (g_fpb_marith[k] || k >= fa) conflict = 1; else shift = 1;
                            }
                        if (conflict || fa < 0) break;
                        if (shift) {
                            fb->first = fa;
                            rck = 0; rwr = 0;
                            for (int k = fa; k < jj; k++) { rck |= (uint16_t)(g_fpb_mrd[k] & ~rwr); rwr |= g_fpb_mwr[k]; }
                        }
                    }
                    if (rk == RK_DET) {
                        int k = jj + 1;
                        while (k < n) {
                            int rk2 = fpb_region_class(&insns[k]);
                            if (rk2 == RK_END || rk2 == RK_DET) break;
                            k++;
                        }
                        int fused = k < n - 1 && insns[k].op == OCERZ_OP_JCC &&
                                    insns[k].ops[0].kind == OCERZ_OPK_IMM &&
                                    insns[k].ops[0].imm > insns[k].rip && insns[k].ops[0].imm != insns[0].rip &&
                                    comis_fuse_producer(insns, k) == jj;
                        uint8_t ida = vid[dr][0], idb = vid[sr][0];
                        for (int r = 0; r < 16; r++) for (int l = 0; l < 2; l++)
                            if (vid[r][l] && (vid[r][l] == ida || vid[r][l] == idb)) vid[r][l] = 0;
                        g_fpb_det[jj] = (uint8_t)(fused ? 1 : 2); planned_sites++;
                        det_jcc = fused ? k : -1;
                        reads = (uint16_t)((1u << dr) | (1u << sr));
                    } else switch (rk) {
                    case RK_STORE: reads = (uint16_t)(1u << sr); break;
                    case RK_LOAD:  writes = (uint16_t)(1u << dr); vid[dr][0] = vid[dr][1] = 0; break;
                    case RK_MOVE:  reads = (uint16_t)(1u << sr); writes = (uint16_t)(1u << dr);
                                   vid[dr][0] = vid[sr][0]; vid[dr][1] = vid[sr][1]; break;
                    case RK_LMOVE: reads = (uint16_t)((1u << sr) | (1u << dr)); writes = (uint16_t)(1u << dr);
                                   vid[dr][0] = vid[sr][0]; break;
                    case RK_UNPCKH: reads = writes = (uint16_t)(1u << dr); vid[dr][0] = vid[dr][1]; break;
                    case RK_UNPCKL: reads = writes = (uint16_t)(1u << dr); vid[dr][1] = vid[dr][0]; break;
                    default: break;
                    }
                    rck |= (uint16_t)(reads & ~rwr);
                    rwr |= writes;
                    g_fpb_mrd[jj] = reads; g_fpb_mwr[jj] = writes;
                    jj++; cnt++;
                    for (int r = 0; r < 16; r++) if (vid[r][0] || vid[r][1]) m |= (uint16_t)(1u << r);
                    if (!m && det_jcc < 0) { left = 0; break; }
                    if (!m && rk != RK_DET) { left = 0; break; }
                }
                if (jj - 1 > last) {
                    fb->end = jj - 1;
                    fb->ckpt = (uint16_t)(rck & rwr);
                    fb->written = rwr;
                    uint16_t m = 0;
                    for (int r = 0; r < 16; r++) if (vid[r][0] || vid[r][1]) m |= (uint16_t)(1u << r);
                    fb->full = left ? m : 0; fb->s0 = 0; fb->d0 = 0;
                    for (int k = last + 1; k <= fb->end; k++) bat[k] = (int8_t)g_n_fpb;
                    for (int k = i; k < fb->first; k++) { bat[k] = -1; g_fpb_member[k] = 0; }
                }
            }
            g_n_fpb++;
            if (fb->end > last) j = fb->end + 1;
        }
        i = j;
    }
}

static int fpb2_kind(const X86Insn *in, int *packed, int *dbl, int *from_mem, int *sq, int *lane_only)
{
    *lane_only = 0;
    int c = fpb_class(in, packed, dbl, from_mem, sq);
    if (c == 1) return K2_ARITH;
    if (c == 2) return K2_MOVE;
    if (c == 3) return K2_LMOVE;
    if (in->seg != OCERZ_SEG_NONE || in->nops < 2) return K2_END;
    const X86Operand *d = &in->ops[0], *s = &in->ops[1];
    int dx = d->kind == OCERZ_OPK_XMM && xmm_is_pinned(d->reg);
    int sx = s->kind == OCERZ_OPK_XMM && xmm_is_pinned(s->reg);
    int dm = d->kind == OCERZ_OPK_MEM && in->addrsize == 8;
    int sm = s->kind == OCERZ_OPK_MEM && (s->riprel || in->addrsize == 8);
    if (in->op == OCERZ_OP_SHUFPD && in->nops == 3 && in->ops[2].kind == OCERZ_OPK_IMM && dx && (sx || sm) &&
        !(in->vex & OCERZ_VEX_L) && !in->mode32 && (!(in->vex & OCERZ_VEX_NDS) || xmm_is_pinned(in->vvvv & 15))) {
        *dbl = 1; *from_mem = sm; return K2_SHUF;
    }
    if (in->vex) {
        if ((in->vex & OCERZ_VEX_L) || in->mode32 || in->nops != 2) return K2_END;
        int nds = (in->vex & OCERZ_VEX_NDS) != 0;
        switch (in->op) {
        case OCERZ_OP_MOVDDUP: *dbl = 1; *from_mem = sm; return !nds && dx && (sx || sm) ? K2_DUP : K2_END;
        case OCERZ_OP_MOVUPS: case OCERZ_OP_MOVAPS: case OCERZ_OP_MOVDQA: case OCERZ_OP_MOVDQU:
            return !nds && dm && sx ? K2_STORE : K2_END;
        case OCERZ_OP_MOVSDX: *dbl = 1; *lane_only = 1; return !nds && dm && sx ? K2_STORE : K2_END;
        case OCERZ_OP_MOVSS: *lane_only = 1; return !nds && dm && sx ? K2_STORE : K2_END;
        case OCERZ_OP_XORPS: case OCERZ_OP_PXOR:
            return nds && dx && sx && d->reg == s->reg && (in->vvvv & 15) == d->reg ? K2_ZERO : K2_END;
        case OCERZ_OP_UCOMISD: case OCERZ_OP_COMISD: case OCERZ_OP_UCOMISS: case OCERZ_OP_COMISS:
            return !nds && dx && sx ? K2_DET : K2_END;
        default: return K2_END;
        }
    }
    switch (in->op) {
    case OCERZ_OP_MOVUPS: case OCERZ_OP_MOVAPS: case OCERZ_OP_MOVDQA: case OCERZ_OP_MOVDQU:
        return dm && sx ? K2_STORE : K2_END;
    case OCERZ_OP_MOVSDX: *dbl = 1; *lane_only = 1; return dm && sx ? K2_STORE : K2_END;
    case OCERZ_OP_MOVSS: *lane_only = 1; return dm && sx ? K2_STORE : K2_END;
    case OCERZ_OP_MOVLPS: *dbl = 1; *lane_only = 1; return dm && sx ? K2_STORE : K2_END;
    case OCERZ_OP_MOVHPS: return dm && sx ? K2_STORE : K2_END;
    case OCERZ_OP_XORPS: case OCERZ_OP_PXOR:
        return dx && sx && d->reg == s->reg ? K2_ZERO : K2_END;
    case OCERZ_OP_UNPCKHPD: return dx && sx ? K2_UNPCKH : K2_END;
    case OCERZ_OP_UNPCKLPD: return dx && sx ? K2_UNPCKL : K2_END;
    case OCERZ_OP_MOVDDUP: *dbl = 1; *from_mem = sm; return dx && (sx || sm) ? K2_DUP : K2_END;
    case OCERZ_OP_UCOMISD: case OCERZ_OP_COMISD: case OCERZ_OP_UCOMISS: case OCERZ_OP_COMISS:
        return dx && sx ? K2_DET : K2_END;
    default: return K2_END;
    }
}

static void fpb2_usedef(const X86Insn *in, uint16_t *use, uint16_t *kill)
{
    uint16_t u = 0, k = 0;
    for (int q = 0; q < in->nops; q++)
        if (in->ops[q].kind == OCERZ_OPK_XMM && in->ops[q].reg < 16) u |= (uint16_t)(1u << in->ops[q].reg);
    const X86Operand *d = &in->ops[0], *s = in->nops > 1 ? &in->ops[1] : NULL;
    int dx = in->nops > 0 && d->kind == OCERZ_OPK_XMM && d->reg < 16;
    if (in->vex) {
        int nds = (in->vex & OCERZ_VEX_NDS) != 0;
        if (nds) u |= (uint16_t)(1u << (in->vvvv & 15));
        int lk = 0;
        if (!(in->vex & OCERZ_VEX_L) && !in->mode32 && in->nops == 2 && dx) {
            switch (in->op) {
            case OCERZ_OP_MOVUPS: case OCERZ_OP_MOVAPS: case OCERZ_OP_MOVDQA: case OCERZ_OP_MOVDQU: lk = !nds; break;
            case OCERZ_OP_MOVSS: case OCERZ_OP_MOVSDX: lk = !nds && s && s->kind == OCERZ_OPK_MEM; break;
            case OCERZ_OP_XORPS: case OCERZ_OP_PXOR: lk = nds && s && s->kind == OCERZ_OPK_XMM && s->reg == d->reg && (in->vvvv & 15) == d->reg; break;
            default: break;
            }
        }
        if (lk) { k = (uint16_t)(1u << d->reg); u &= (uint16_t)~k; if (in->op == OCERZ_OP_XORPS || in->op == OCERZ_OP_PXOR) u = 0; }
        *use = u; *kill = k;
        return;
    }
    switch (in->op) {
    case OCERZ_OP_MOVUPS: case OCERZ_OP_MOVAPS: case OCERZ_OP_MOVDQA: case OCERZ_OP_MOVDQU: case OCERZ_OP_MOVDDUP:
        if (dx) { k = (uint16_t)(1u << d->reg); u &= (uint16_t)~k; }
        break;
    case OCERZ_OP_MOVSS: case OCERZ_OP_MOVSDX:
        if (dx && s && s->kind == OCERZ_OPK_MEM) { k = (uint16_t)(1u << d->reg); u &= (uint16_t)~k; }
        break;
    case OCERZ_OP_XORPS: case OCERZ_OP_PXOR:
        if (dx && s && s->kind == OCERZ_OPK_XMM && s->reg == d->reg) { k = (uint16_t)(1u << d->reg); u = 0; }
        break;
    default: break;
    }
    *use = u; *kill = k;
}

static int fpb2_gpr_after_ok(const X86Insn *in, uint16_t gprs)
{
    switch (in->op) {
    case OCERZ_OP_JCC: case OCERZ_OP_JMP: case OCERZ_OP_NOP: case OCERZ_OP_CMP: case OCERZ_OP_TEST:
        return in->nops == 0 || in->ops[0].kind != OCERZ_OPK_MEM || in->op == OCERZ_OP_CMP || in->op == OCERZ_OP_TEST;
    case OCERZ_OP_ADD: case OCERZ_OP_SUB: case OCERZ_OP_INC: case OCERZ_OP_DEC: case OCERZ_OP_AND: case OCERZ_OP_OR:
    case OCERZ_OP_XOR: case OCERZ_OP_LEA: case OCERZ_OP_MOV: case OCERZ_OP_SHL: case OCERZ_OP_SHR: case OCERZ_OP_SAR:
    case OCERZ_OP_NEG: case OCERZ_OP_NOT: case OCERZ_OP_MOVZX: case OCERZ_OP_MOVSX: case OCERZ_OP_CMOVCC: case OCERZ_OP_SETCC:
        if (in->nops < 1 || in->ops[0].kind != OCERZ_OPK_REG) return 0;
        return !(gprs & (1u << (in->ops[0].reg & 15)));
    default: return 0;
    }
}

static uint16_t g_fpb_live[JIT_MAX_BLOCK_INSNS];

uint16_t g_fpb_stchk[JIT_MAX_BLOCK_INSNS];

static uint8_t g_fpb_stdbl[JIT_MAX_BLOCK_INSNS];

uint8_t g_fpb_stlane[JIT_MAX_BLOCK_INSNS] = {0};

uint8_t g_fpb_undo[JIT_MAX_BLOCK_INSNS] = {0};

uint8_t g_fpb_undo_done[JIT_MAX_BLOCK_INSNS] = {0};

uint8_t g_fpb_undo_size[JIT_MAX_BLOCK_INSNS] = {0};

static uint8_t g_fpb_mstore[JIT_MAX_BLOCK_INSNS];

uint8_t g_fpb_undo_ld[JIT_MAX_BLOCK_INSNS] = {0};

uint8_t g_fpb_undo_ldsz[JIT_MAX_BLOCK_INSNS] = {0};

int16_t g_fpb_undo_from[JIT_MAX_BLOCK_INSNS];

int16_t g_fpb_undo_ldst[JIT_MAX_BLOCK_INSNS];

static int mem_same(const X86Operand *a, const X86Operand *b)
{
    return !a->riprel && !b->riprel && a->base == b->base && a->index == b->index &&
           a->scale == b->scale && a->disp == b->disp;
}

uint16_t g_fpb_exit_mask;

int g_fpb_exit_batch = -1;

int g_fpb_exit_end = -1;

static inline uint16_t fpb2_membits(const X86Operand *m)
{
    uint16_t g = 0;
    if (m->base != OCERZ_REG_NONE) g |= (uint16_t)(1u << (m->base & 15));
    if (m->index != OCERZ_REG_NONE) g |= (uint16_t)(1u << (m->index & 15));
    return g;
}

int g_l0_fixed;

static int fpb_undo_lane(int slot)
{
    while (slot >= g_n_undo_lanes) {
        if (g_n_undo_lanes >= FPB_UNDO_MAX) return -1;
        int v = lane_reserve();
        if (v < 0) return -1;
        g_undo_vreg[g_n_undo_lanes++] = (int8_t)v;
    }
    return g_undo_vreg[slot];
}

static int fpb2_undo_ok(const X86Insn *in, const X86Operand *m, int size)
{
    static int dis = -1;
    if (dis < 0) dis = getenv("OCERZ_FPB_NOUNDO") ? 1 : 0;
    if (dis || !vec_tso_relaxed() || !mem_fast_forms_ok()) return 0;
    if (in->seg != OCERZ_SEG_NONE || in->addrsize != 8 || m->riprel) return 0;
    if (m->base == OCERZ_REG_NONE || pin_slot(m->base) < 0) return 0;
    if (m->index != OCERZ_REG_NONE && pin_slot(m->index) < 0) return 0;
    if (rsp_is_ptr() && (m->base == OCERZ_RSP || m->index == OCERZ_RSP)) return 0;
    int64_t disp = m->disp;
    return (disp >= 0 && (disp % size) == 0 && disp / size <= 4095) || (disp >= -256 && disp <= 255);
}

void fpb_scan_v2(const X86Insn *insns, int n, int8_t *bat)
{
    g_n_fpb = 0;
    g_n_fpb_sites = 0;
    g_fpb_exit_mask = 0; g_fpb_exit_batch = -1; g_fpb_exit_end = -1;
    for (int i = 0; i < n; i++) {
        bat[i] = -1; g_fpb_member[i] = 0; g_fpb_det[i] = 0; g_fpb_sidechk[i] = 0;
        g_fpb_stchk[i] = 0; g_fpb_stlane[i] = 0; g_fpb_stdbl[i] = 0; g_fpb_live[i] = 0xffff;
        fpb_undo_clear(i);
    }
    if (g_fpb_disabled < 0) g_fpb_disabled = getenv("OCERZ_NO_FPBATCH") ? 1 : 0;
    if (g_fpb_disabled || !sse_enabled() || !xmm_global_enabled() || n <= 0) return;
    int selfloop = (insns[n - 1].op == OCERZ_OP_JCC || insns[n - 1].op == OCERZ_OP_JMP) && insns[n - 1].nops == 1 &&
                   insns[n - 1].ops[0].kind == OCERZ_OPK_IMM && insns[n - 1].ops[0].imm == insns[0].rip;
    static uint16_t use[JIT_MAX_BLOCK_INSNS], kill[JIT_MAX_BLOCK_INSNS], wr[JIT_MAX_BLOCK_INSNS];
    for (int i = 0; i < n; i++) {
        fpb2_usedef(&insns[i], &use[i], &kill[i]);
        wr[i] = insns[i].nops > 0 && insns[i].ops[0].kind == OCERZ_OPK_XMM && insns[i].ops[0].reg < 16
                ? (uint16_t)(1u << insns[i].ops[0].reg) : 0;
        if (insns[i].op == OCERZ_OP_JCC && !(selfloop && i == n - 1)) use[i] = 0xffff;
    }
    uint16_t live_in0 = 0;
    for (int pass = 0; pass < 4; pass++) {
        uint16_t cur = selfloop ? live_in0 : 0xffff;
        for (int i = n - 1; i >= 0; i--) {
            g_fpb_live[i] = cur;
            cur = (uint16_t)(use[i] | (cur & ~kill[i]));
        }
        if (!selfloop || cur == live_in0) break;
        live_in0 = cur;
    }
    int planned_sites = 0;
    int i = 0;
    while (i < n) {
        int packed, dbl, from_mem, sq, lane_only;
        int k0 = fpb2_kind(&insns[i], &packed, &dbl, &from_mem, &sq, &lane_only);
        if (k0 != K2_ARITH && k0 != K2_MOVE && k0 != K2_LMOVE) { i++; continue; }
        int j = i;
        uint16_t written = 0, ckpt = 0, full = 0, s0 = 0, d0 = 0, gprs = 0, everf = 0;
        int gain = 0, n_arith = 0, cost_extra = 0, sites = 0;
        struct { uint8_t s, d, cls; int t; } edges[64]; int n_edges = 0;
        int lastw[16], lastbreak[16]; uint8_t taint_dbl[16];
        for (int r = 0; r < 16; r++) { lastw[r] = -1; lastbreak[r] = -1; taint_dbl[r] = 0; }
        int loads[64], loadsz[64]; int nload = 0;
        int stores[64]; int nstore = 0;
        int nundo = 0;
        int st_at[16], st_cls[16], nst = 0;
        int det_j = -1;
        int n_fma = 0;
        while (j < n) {
            const X86Insn *in = &insns[j];
            int k = fpb2_kind(in, &packed, &dbl, &from_mem, &sq, &lane_only);
            { static long dbg_rip = -1, dbg_max = -1, dbg_lo = 0, dbg_hi = 0;
              if (dbg_rip < 0) { const char *e = getenv("OCERZ_FPB_DBGRIP"); dbg_rip = e ? (long)strtoull(e, NULL, 0) : 0;
                                 e = getenv("OCERZ_FPB_DBGMAX"); dbg_max = e ? strtol(e, NULL, 0) : 100000;
                                 e = getenv("OCERZ_FPB_DBGLO"); dbg_lo = e ? (long)strtoull(e, NULL, 0) : 0;
                                 e = getenv("OCERZ_FPB_DBGHI"); dbg_hi = e ? (long)strtoull(e, NULL, 0) : 0; }
              if (dbg_hi && insns[0].rip >= (uint64_t)dbg_lo && insns[0].rip <= (uint64_t)dbg_hi) {
                  long lim = (dbg_rip && (uint64_t)dbg_rip == insns[0].rip) ? dbg_max : -1;
                  if (j > lim) break;
              } }
            if (det_j >= 0) {
                if (in->op == OCERZ_OP_JCC && j < n - 1 && in->ops[0].kind == OCERZ_OPK_IMM &&
                    in->ops[0].imm != insns[0].rip && comis_fuse_producer(insns, j) == det_j) {
                    g_fpb_det[det_j] = 1;
                    g_fpb_sidechk[j] = (uint16_t)(full | s0 | d0);
                    g_fpb_mrd[j] = 0; g_fpb_mwr[j] = 0; g_fpb_mmem[j] = 0; g_fpb_marith[j] = 0;
                    sites += 2;
                    det_j = -1; j++;
                    continue;
                }
                j = det_j;
                break;
            }
            if (k == K2_END) break;
            if (planned_sites + sites + 4 >= FPB_SITES_MAX) break;
            const X86Operand *d = &in->ops[0], *sr = &in->ops[1];
            unsigned dr = d->kind == OCERZ_OPK_XMM ? d->reg : 16, srr = sr->kind == OCERZ_OPK_XMM ? sr->reg : 16;
            uint16_t sbit = srr < 16 ? (uint16_t)(1u << srr) : 0, dbit = dr < 16 ? (uint16_t)(1u << dr) : 0;
            uint16_t reads = 0, writes = 0;
            int is_mem = 0;
            if (k == K2_STORE) {
                int conflict = 0;
                for (int q = 0; q < nload; q++)
                    if (mem_may_alias(&insns[loads[q]], &insns[loads[q]].ops[1], in, &in->ops[0])) conflict = 1;
                int usz = lane_only ? (dbl ? 8 : 4) : (in->op == OCERZ_OP_MOVHPS ? 8 : 16);
                if (conflict) {
                    if (nundo >= FPB_UNDO_MAX || nstore >= 64 || !fpb2_undo_ok(in, d, usz) || fpb_undo_lane(nundo) < 0) break;
                    nundo++;
                    g_fpb_undo[j] = (uint8_t)nundo; g_fpb_undo_size[j] = (uint8_t)usz;
                    int reuse = -1;
                    for (int q = nload - 1; q >= 0 && reuse < 0; q--) {
                        int l = loads[q];
                        if (loadsz[q] < usz || insns[l].seg != OCERZ_SEG_NONE || insns[l].addrsize != 8 || g_fpb_undo_ld[l]) continue;
                        if (!mem_same(&insns[l].ops[1], d)) continue;
                        int ok = 1;
                        for (int t = 0; t < nstore && ok; t++)
                            if (stores[t] > l && mem_may_alias(&insns[l], &insns[l].ops[1], &insns[stores[t]], &insns[stores[t]].ops[0])) ok = 0;
                        if (ok) reuse = q;
                    }
                    if (reuse >= 0) {
                        int l = loads[reuse];
                        g_fpb_undo_ld[l] = (uint8_t)nundo; g_fpb_undo_ldsz[l] = (uint8_t)loadsz[reuse];
                        g_fpb_undo_ldst[l] = (int16_t)j; g_fpb_undo_from[j] = (int16_t)l;
                        cost_extra += 1;
                    } else {
                        cost_extra += 2;
                    }
                }
                if (nstore < 64) stores[nstore++] = j;
                gprs |= fpb2_membits(d);
                reads = sbit;
                uint16_t t = (uint16_t)((full | s0 | d0) & sbit);
                if (t) {
                    if (nst < 16) {
                        st_at[nst] = j;
                        st_cls[nst] = lane_only ? (dbl ? 3 : 2) : (full & sbit) ? 1 : (d0 & sbit) ? 3 : 2;
                        nst++;
                    }
                    g_fpb_stchk[j] = sbit;
                    if (lane_only) {
                        g_fpb_stlane[j] = 1; g_fpb_stdbl[j] = (uint8_t)dbl;
                        cost_extra += 2;
                    } else {
                        cost_extra += 3;
                        full &= (uint16_t)~sbit;
                    }
                    d0 &= (uint16_t)~sbit; s0 &= (uint16_t)~sbit;
                    sites++;
                }
            } else if (k == K2_DET) {
                if (j + 1 >= n) break;
                reads = (uint16_t)(sbit | dbit);
                det_j = j;
            } else {
                if (sr->kind == OCERZ_OPK_MEM) {
                    if (nload >= 64) break;
                    loadsz[nload] = k == K2_MOVE || k == K2_SHUF ? 16 : k == K2_LMOVE ? (dbl ? 8 : 4) : k == K2_DUP ? 8 :
                                    k == K2_ARITH ? (packed ? 16 : dbl ? 8 : 4) : 0;
                    loads[nload++] = j;
                    gprs |= fpb2_membits(sr);
                    is_mem = 1;
                }
                switch (k) {
                case K2_ARITH: {
                    int is_mm = in->op == OCERZ_OP_MAXSS || in->op == OCERZ_OP_MINSS || in->op == OCERZ_OP_MAXSD || in->op == OCERZ_OP_MINSD ||
                                in->op == OCERZ_OP_MAXPS || in->op == OCERZ_OP_MINPS || in->op == OCERZ_OP_MAXPD || in->op == OCERZ_OP_MINPD;
                    int fma = in->op >= OCERZ_OP_VFMA_FIRST && in->op <= OCERZ_OP_VFMA_LAST;
                    n_fma += fma;
                    int nds = (in->vex & OCERZ_VEX_NDS) != 0 && !fma;
                    unsigned vr = (in->vex & OCERZ_VEX_NDS) ? (in->vvvv & 15) : dr;
                    uint16_t vbit = (uint16_t)(1u << vr);
                    unsigned srcs[2] = { srr, (in->vex & OCERZ_VEX_NDS) ? vr : 16 };
                    for (int q = 0; q < 2; q++) {
                        unsigned sx = srcs[q];
                        if (sx >= 16 || sx == dr || is_mm || n_edges >= 64) continue;
                        uint16_t xb = (uint16_t)(1u << sx);
                        if (packed && (full & xb) && taint_dbl[sx] == (uint8_t)dbl) { edges[n_edges].s = (uint8_t)sx; edges[n_edges].d = (uint8_t)dr; edges[n_edges].t = j; edges[n_edges].cls = 0; n_edges++; }
                        if (!dbl && (s0 & xb) && n_edges < 64) { edges[n_edges].s = (uint8_t)sx; edges[n_edges].d = (uint8_t)dr; edges[n_edges].t = j; edges[n_edges].cls = 1; n_edges++; }
                        if (dbl && (d0 & xb) && n_edges < 64)  { edges[n_edges].s = (uint8_t)sx; edges[n_edges].d = (uint8_t)dr; edges[n_edges].t = j; edges[n_edges].cls = 2; n_edges++; }
                    }
                    taint_dbl[dr] = (uint8_t)dbl;
                    if (!dbl) everf |= dbit; else everf &= (uint16_t)~dbit;
                    if (is_mm || (sq && srr != dr)) lastbreak[dr] = j;
                    reads = (uint16_t)(sbit | vbit | (fma ? dbit : 0)); writes = dbit;
                    if (packed) { full |= dbit; s0 &= (uint16_t)~dbit; d0 &= (uint16_t)~dbit; }
                    else {
                        if (nds) { if (full & vbit) full |= dbit; else full &= (uint16_t)~dbit; }
                        if (dbl) { d0 |= dbit; s0 &= (uint16_t)~dbit; }
                        else     { s0 |= dbit; d0 &= (uint16_t)~dbit; }
                    }
                    if (!is_mm) { gain += packed ? 6 : 2; n_arith++; }
                    break;
                }
                case K2_MOVE:
                    reads = sbit; writes = dbit; lastbreak[dr] = j;
                    if (from_mem) { full &= (uint16_t)~dbit; s0 &= (uint16_t)~dbit; d0 &= (uint16_t)~dbit; everf &= (uint16_t)~dbit; }
                    else {
                        everf = (uint16_t)((everf & ~dbit) | ((everf & sbit) ? dbit : 0));
                        full = (uint16_t)((full & ~dbit) | ((full & sbit) ? dbit : 0));
                        s0   = (uint16_t)((s0   & ~dbit) | ((s0   & sbit) ? dbit : 0));
                        d0   = (uint16_t)((d0   & ~dbit) | ((d0   & sbit) ? dbit : 0));
                        taint_dbl[dr] = taint_dbl[srr];
                    }
                    break;
                case K2_LMOVE:
                    reads = (uint16_t)(sbit | (from_mem ? 0 : dbit)); writes = dbit; lastbreak[dr] = j;
                    if (!dbl || (everf & sbit)) everf |= dbit;
                    if (from_mem) { full &= (uint16_t)~dbit; s0 &= (uint16_t)~dbit; d0 &= (uint16_t)~dbit; if (dbl) everf &= (uint16_t)~dbit; }
                    else {
                        if (full & sbit) full |= dbit;
                        if (dbl) { if ((d0 | full) & sbit) d0 |= dbit; else d0 &= (uint16_t)~dbit; s0 &= (uint16_t)~dbit; }
                        else     { if ((s0 | full) & sbit) s0 |= dbit; else s0 &= (uint16_t)~dbit; d0 &= (uint16_t)~dbit; }
                        taint_dbl[dr] = (uint8_t)dbl;
                    }
                    break;
                case K2_ZERO:
                    writes = dbit; lastbreak[dr] = j;
                    full &= (uint16_t)~dbit; s0 &= (uint16_t)~dbit; d0 &= (uint16_t)~dbit; everf &= (uint16_t)~dbit;
                    break;
                case K2_UNPCKH:
                    reads = (uint16_t)(sbit | dbit); writes = dbit; lastbreak[dr] = j;
                    if ((full & sbit) || (full & dbit)) { full |= dbit; d0 &= (uint16_t)~dbit; s0 &= (uint16_t)~dbit; }
                    else { full &= (uint16_t)~dbit; d0 &= (uint16_t)~dbit; s0 &= (uint16_t)~dbit; }
                    break;
                case K2_UNPCKL:
                    reads = (uint16_t)(sbit | dbit); writes = dbit; lastbreak[dr] = j;
                    if ((full | d0 | s0) & sbit) { full |= dbit; d0 &= (uint16_t)~dbit; s0 &= (uint16_t)~dbit; }
                    break;
                case K2_DUP:
                    reads = sbit; writes = dbit; lastbreak[dr] = j;
                    everf = (uint16_t)((everf & ~dbit) | ((!from_mem && (everf & sbit)) ? dbit : 0));
                    if (!from_mem && ((full | d0 | s0) & sbit)) { full |= dbit; d0 &= (uint16_t)~dbit; s0 &= (uint16_t)~dbit; }
                    else { full &= (uint16_t)~dbit; d0 &= (uint16_t)~dbit; s0 &= (uint16_t)~dbit; }
                    break;
                case K2_SHUF: {
                    uint16_t vb2 = (in->vex & OCERZ_VEX_NDS) ? (uint16_t)(1u << (in->vvvv & 15)) : dbit;
                    reads = (uint16_t)(sbit | vb2); writes = dbit; lastbreak[dr] = j;
                    uint16_t srcs2 = (uint16_t)(sbit | vb2);
                    everf = (uint16_t)((everf & ~dbit) | ((everf & srcs2) ? dbit : 0));
                    if ((full | d0 | s0) & srcs2) { full |= dbit; d0 &= (uint16_t)~dbit; s0 &= (uint16_t)~dbit; }
                    else { full &= (uint16_t)~dbit; d0 &= (uint16_t)~dbit; s0 &= (uint16_t)~dbit; }
                    break;
                }
                default: break;
                }
                if (dr < 16) lastw[dr] = j;
            }
            ckpt |= (uint16_t)(reads & ~written);
            written |= writes;
            g_fpb_mrd[j] = reads; g_fpb_mwr[j] = writes;
            g_fpb_mmem[j] = (uint8_t)is_mem; g_fpb_marith[j] = (uint8_t)(k == K2_ARITH); g_fpb_mstore[j] = (uint8_t)(k == K2_STORE);
            j++;
        }
        if (det_j >= 0) j = det_j;
        int end = j - 1;
        while (end > i && g_fpb_mstore[end]) {
            if (g_fpb_undo[end]) {
                cost_extra -= g_fpb_undo_from[end] >= 0 ? 1 : 2;
                if (g_fpb_undo_from[end] >= 0) fpb_undo_clear(g_fpb_undo_from[end]);
                fpb_undo_clear(end);
                nundo--;
            }
            if (g_fpb_stchk[end]) {
                uint16_t sb = g_fpb_stchk[end];
                cost_extra -= g_fpb_stlane[end] ? 2 : 3;
                sites--;
                g_fpb_stchk[end] = 0; g_fpb_stlane[end] = 0;
                for (int q = 0; q < nst; q++) if (st_at[q] == end) {
                    if (st_cls[q] == 1) full |= sb; else if (st_cls[q] == 3) d0 |= sb; else s0 |= sb;
                    nst = q;
                    break;
                }
            }
            end--;
        }
        if (end < i || n_arith < 1) {
            for (int q = i; q <= j - 1 && q < n; q++) { g_fpb_stchk[q] = 0; g_fpb_stlane[q] = 0; g_fpb_det[q] = 0; g_fpb_sidechk[q] = 0; fpb_undo_clear(q); }
            i = j > i ? j : i + 1;
            continue;
        }
        for (int q = end + 1; q < j; q++) { g_fpb_stchk[q] = 0; g_fpb_stlane[q] = 0; g_fpb_det[q] = 0; g_fpb_sidechk[q] = 0; fpb_undo_clear(q); }
        ckpt &= written;
        uint16_t tainted = (uint16_t)(full | s0 | d0);
        uint16_t live = g_fpb_live[end];
        uint16_t wr_after = 0;
        int exit_ok = selfloop;
        for (int m = end + 1; m < n; m++) {
            wr_after |= wr[m];
            if (exit_ok && !fpb2_gpr_after_ok(&insns[m], gprs)) exit_ok = 0;
        }
        uint16_t T = (uint16_t)(tainted & live);
        uint16_t rest = (uint16_t)(tainted & ~T & ~wr_after);
        uint16_t exitchk = 0;
        if (exit_ok) exitchk = rest; else T |= rest;
        uint16_t Tf = (uint16_t)(full & T), Ts = (uint16_t)(s0 & T), Td = (uint16_t)(d0 & T);
        for (int q = 0; q < nst && st_at[q] <= end; q++) {
            int sj = st_at[q];
            uint16_t sb = g_fpb_stchk[sj];
            int later = 0;
            for (int m = sj + 1; m <= end; m++) if (g_fpb_mwr[m] & sb) { later = 1; break; }
            if (later) continue;
            if (sj == end && !(T | exitchk)) {
                if (g_fpb_undo[sj]) {
                    cost_extra -= g_fpb_undo_from[sj] >= 0 ? 1 : 2;
                    if (g_fpb_undo_from[sj] >= 0) fpb_undo_clear(g_fpb_undo_from[sj]);
                    fpb_undo_clear(sj);
                    nundo--;
                }
                continue;
            }
            cost_extra -= g_fpb_stlane[sj] ? 2 : 3;
            sites--;
            g_fpb_stchk[sj] = 0; g_fpb_stlane[sj] = 0;
            if (st_cls[q] == 1) Tf |= sb; else if (st_cls[q] == 3) Td |= sb; else Ts |= sb;
            T |= sb;
        }
        if (ocerz_afp() && n_fma == 0) {
            for (int q = i; q <= end; q++) { g_fpb_stchk[q] = 0; g_fpb_stlane[q] = 0; g_fpb_det[q] = 0; g_fpb_sidechk[q] = 0; fpb_undo_clear(q); }
            T = 0; Tf = 0; Ts = 0; Td = 0; exitchk = 0; ckpt = 0; sites = 0; cost_extra = 0; nundo = 0;
        }
        uint16_t U = (uint16_t)(T | exitchk);
        for (int changed = 1; changed;) {
            changed = 0;
            for (int e = 0; e < n_edges; e++) {
                int S = edges[e].s, D = edges[e].d, t = edges[e].t;
                if (S == D || t > end) continue;
                if (lastbreak[D] > t || lastw[S] > t) continue;
                if (!(U & (1u << D))) continue;
                if (edges[e].cls == 0) { if (Tf & (1u << S)) { Tf &= (uint16_t)~(1u << S); changed = 1; } }
                else if (edges[e].cls == 1) { if ((Ts & (1u << S)) && !(Tf & (1u << S))) { Ts &= (uint16_t)~(1u << S); changed = 1; } }
                else { if ((Td & (1u << S)) && !(Tf & (1u << S))) { Td &= (uint16_t)~(1u << S); changed = 1; } }
                if (exitchk & (1u << S)) {
                    if (edges[e].cls == 0 || !(full & (1u << S))) { exitchk &= (uint16_t)~(1u << S); changed = 1; }
                }
                U = (uint16_t)(Tf | Ts | Td | exitchk);
            }
        }
        int nregs = __builtin_popcount((unsigned)(Tf | Ts | Td));
        int cost = __builtin_popcount((unsigned)ckpt) + (nregs ? 3 + nregs : 0) + cost_extra;
        if (gain > cost && g_n_fpb < FPB_MAX && planned_sites + sites + 2 < FPB_SITES_MAX) {
            FpBatch *fb = &g_fpb[g_n_fpb];
            fb->first = i; fb->last = end; fb->end = end;
            fb->ckpt = ckpt; fb->ckpt_emit = ckpt; fb->written = written; fb->dirty_open = 0;
            fb->full = Tf; fb->s0 = Ts; fb->d0 = Td; fb->gain = gain - cost;
            fb->dblonly = (uint16_t)(written & ~everf);
            fb->site = NULL; fb->back = NULL;
            for (int q = i; q <= end; q++) { bat[q] = (int8_t)g_n_fpb; g_fpb_member[q] = g_fpb_marith[q]; }
            if (getenv("OCERZ_FPB_DBGPRINT"))
                fprintf(stderr, "FPB rip=%#llx batch %d [%d..%d] ckpt=%04x written=%04x Tf=%04x Ts=%04x Td=%04x exit=%04x tainted=%04x live=%04x undo=%d\n",
                        (unsigned long long)insns[0].rip, g_n_fpb, i, end, ckpt, written, Tf, Ts, Td, exitchk, tainted, live, nundo);
            if (exitchk) { g_fpb_exit_mask = exitchk; g_fpb_exit_batch = g_n_fpb; g_fpb_exit_end = end; sites++; }
            planned_sites += sites + 1;
            g_n_fpb++;
        } else {
            for (int q = i; q <= end; q++) { g_fpb_stchk[q] = 0; g_fpb_stlane[q] = 0; g_fpb_det[q] = 0; g_fpb_sidechk[q] = 0; fpb_undo_clear(q); }
        }
        i = end + 1;
    }
}

int fpb_v1(void)
{
    static int v = -1;
    if (v < 0) v = getenv("OCERZ_FPB_V1") ? 1 : 0;
    return v;
}

int unsafe_nocheckbr(void)
{
    static int en = -1;
    if (en < 0) en = getenv("OCERZ_UNSAFE_NOCHECKBR") ? 1 : 0;
    return en;
}

void fpb_emit_check(A64Buf *b, FpBatch *fb)
{
    int have_f = 0, have_d = 0;
    fb->fcmp_vreg = -1;
    g_fcmp_self_idx = -1;
    if (!(fb->full | fb->s0 | fb->d0)) { fb->site = NULL; fb->back = NULL; return; }
    uint16_t fullf = (uint16_t)(fb->full & ~fb->dblonly), fulld = (uint16_t)(fb->full & fb->dblonly);
    int dcur = -1;
    if (fulld) {
        int first = -1, acc = -1;
        for (int r = 0; r < 16; r++) if (fulld & (1u << r)) {
            l0_flush_reg(b, (unsigned)r);
            int v = xmm_vreg((unsigned)r);
            if (first < 0) first = v;
            else if (acc < 0) { a64_v_fmax(b, 1, VX3, first, v); acc = VX3; }
            else a64_v_fmax(b, 1, VX3, VX3, v);
        }
        a64_fmaxp_d(b, VX3, acc >= 0 ? acc : first);
        dcur = VX3;
    }
    if (fullf) {
        int first = -1, acc = -1;
        for (int r = 0; r < 16; r++) if (fullf & (1u << r)) {
            l0_flush_reg(b, (unsigned)r);
            int v = xmm_vreg((unsigned)r);
            if (first < 0) first = v;
            else if (acc < 0) { a64_v_fmax(b, 0, VX0, first, v); acc = VX0; }
            else a64_v_fmax(b, 0, VX0, VX0, v);
        }
        a64_fmaxv_4s(b, VX1, acc >= 0 ? acc : first);
        have_f = 1;
    }
    if (fb->s0) {
        int cur = have_f ? VX1 : -1;
        for (int r = 0; r < 16; r++) if (fb->s0 & (1u << r)) {
            int v = l0_src2(b, (unsigned)r, 0);
            if (cur < 0) cur = v;
            else { a64_fmax_s(b, 0, VX1, cur, v); cur = VX1; }
        }
        if (cur != VX1) { a64_fmax_s(b, 0, VX1, cur, cur); }
        have_f = 1;
    }
    if (fb->d0 || dcur >= 0) {
        int cur = dcur;
        for (int r = 0; r < 16; r++) if (fb->d0 & (1u << r)) {
            int v = l0_src2(b, (unsigned)r, 1);
            if (cur < 0) cur = v;
            else { a64_fmax_s(b, 1, VX2, cur, v); cur = VX2; }
        }
        a64_fcmp(b, 1, cur, cur);
        g_fcmp_self_vreg = cur;
        g_fcmp_self_idx = g_cur_insn_idx;
        fb->fcmp_vreg = (int8_t)cur;
        have_d = 1;
    }
    if (have_d && have_f && unsafe_nocheckbr()) { fb->site = NULL; fb->back = a64_label(b); fb->gain = 0; return; }
    static int force = -1; if (force < 0) force = getenv("OCERZ_FPB_FORCEREPLAY") ? 1 : 0;
    int vs = force ? A64_AL : A64_VS;
    if (have_d && have_f) {
        uint32_t *dnan = a64_label(b); a64_bcond(b, vs, 0);
        a64_fcmp(b, 0, VX1, VX1);
        fb->site = a64_label(b); a64_bcond(b, vs, 0);
        uint32_t *skip = a64_label(b); a64_b(b, 0);
        a64_patch_bcond(dnan, a64_label(b));
        uint32_t *tramp = a64_label(b); a64_b(b, 0);
        a64_patch_b(skip, a64_label(b));
        fb->back = a64_label(b);
        fb->gain = (int)(tramp - fb->site);
    } else if (have_d) {
        if (unsafe_nocheckbr()) fb->site = NULL;
        else { fb->site = a64_label(b); a64_bcond(b, vs, 0); }
        fb->back = a64_label(b);
        fb->gain = 0;
    } else {
        a64_fcmp(b, 0, VX1, VX1);
        if (unsafe_nocheckbr()) fb->site = NULL;
        else { fb->site = a64_label(b); a64_bcond(b, vs, 0); }
        fb->back = a64_label(b);
        fb->gain = 0;
    }
}

int8_t g_l0[16] = {0};

uint8_t g_l0_dbl[16] = {0};

uint16_t g_l0_owners[L0_NLANES];

unsigned g_l0_next;

void fpb_emit_regs_check(A64Buf *b, uint16_t regs, int batch, int end, const int8_t *l0, const uint8_t *l0_dbl)
{
    if (!regs || g_n_fpb_sites >= FPB_SITES_MAX) return;
    uint16_t dmask = (uint16_t)(regs & g_fpb[batch].dblonly), fmask = (uint16_t)(regs & ~dmask);
    int first = -1, acc = -1;
    for (int r = 0; r < 16; r++) if (fmask & (1u << r)) {
        int v = xmm_vreg((unsigned)r);
        if (first < 0) first = v;
        else if (acc < 0) { a64_v_fmax(b, 0, VX0, first, v); acc = VX0; }
        else a64_v_fmax(b, 0, VX0, VX0, v);
    }
    int dfirst = -1, dacc = -1;
    for (int r = 0; r < 16; r++) if (dmask & (1u << r)) {
        int v = xmm_vreg((unsigned)r);
        if (dfirst < 0) dfirst = v;
        else if (dacc < 0) { a64_v_fmax(b, 1, VX3, dfirst, v); dacc = VX3; }
        else a64_v_fmax(b, 1, VX3, VX3, v);
    }
    if (dmask) a64_fmaxp_d(b, VX3, dacc >= 0 ? dacc : dfirst);
    if (fmask) {
        a64_fmaxv_4s(b, VX1, acc >= 0 ? acc : first);
        if (dmask) { a64_fcvt_d2s(b, VX2, VX3); a64_fmax_s(b, 0, VX1, VX1, VX2); }
        a64_fcmp(b, 0, VX1, VX1);
    } else {
        a64_fcmp(b, 1, VX3, VX3);
    }
    FpbSite *st = &g_fpb_sites[g_n_fpb_sites++];
    st->batch = batch; st->end = end;
    st->site = a64_label(b); a64_bcond(b, A64_VS, 0);
    st->back = a64_label(b);
    for (int r = 0; r < 16; r++) { st->l0[r] = l0[r]; st->l0_dbl[r] = l0_dbl[r]; }
    st->fcmp_a = -1; st->fcmp_b = -1; st->fcmp_dbl = 0; st->keep_jt = 0;
}

int l0_enabled(void)
{
    static int en = -1;
    if (en < 0) en = getenv("OCERZ_NO_L0CACHE") ? 0 : 1;
    return en;
}

uint16_t g_l0_dirty;

int8_t g_yc[16] = {0};

uint16_t g_yc_dirty;

struct JitLaneRec g_lanerec[JIT_MAX_BLOCK_INSNS * 2];

int g_n_lanerec;

void lanerec_note(uint32_t off)
{
    struct JitLaneRec r;
    r.off = off;
    r.dirty = g_l0_dirty;
    for (int i = 0; i < 16; i++) {
        r.l0[i] = g_l0[i] < 0 ? 0xff : (uint8_t)((g_l0[i] - 4) | (g_l0_dbl[i] ? 0x10 : 0));
        r.yc[i] = g_yc[i] < 0 ? 0xff : (uint8_t)(g_yc[i] - 4);
    }
    if (g_n_lanerec > 0) {
        const struct JitLaneRec *l = &g_lanerec[g_n_lanerec - 1];
        if (l->dirty == r.dirty && memcmp(l->l0, r.l0, 16) == 0 && memcmp(l->yc, r.yc, 16) == 0) return;
    }
    if (g_n_lanerec < (int)(sizeof g_lanerec / sizeof g_lanerec[0])) g_lanerec[g_n_lanerec++] = r;
}

int l0_defer(void)
{
    static int v = -1;
    if (v < 0) v = getenv("OCERZ_NO_L0_DEFER") == NULL;
    return v;
}

int g_l0_fixed;

int8_t g_l0_fixed_lane[16] = {0};

uint8_t g_l0_fixed_dbl[16] = {0};

uint16_t g_l0_fixed_dirty;

static int insn_writes_xmm0(const X86Insn *in)
{
    if (in->nops < 1 || in->ops[0].kind != OCERZ_OPK_XMM) return 0;
    switch (in->op) {
    case OCERZ_OP_UCOMISS: case OCERZ_OP_UCOMISD: case OCERZ_OP_COMISS: case OCERZ_OP_COMISD:
    case OCERZ_OP_PTEST: case OCERZ_OP_VTESTPS: case OCERZ_OP_VTESTPD:
        return 0;
    default:
        return 1;
    }
}

int l0_alloc2(A64Buf *b, unsigned r, int dbl)
{
    if (g_l0_fixed) {
        if (g_l0_fixed_lane[r] < 0) {
            l0_inval(r);
            return -1;
        }
        int t = g_l0_fixed_lane[r];
        if (g_l0[r] >= 0 && g_l0[r] != t)
            g_l0_owners[g_l0[r] - 4] &= (uint16_t)~(1u << r);
        uint16_t own = g_l0_owners[t - 4];
        for (int i = 0; i < 16; i++) if ((own & (1u << i)) && i != (int)r) {
            if (g_l0_dirty & (1u << i))
                l0_flush_reg(b, (unsigned)i);
            g_l0[i] = -1;
        }
        g_l0_owners[t - 4] = (uint16_t)(1u << r);
        g_l0[r] = (int8_t)t; g_l0_dbl[r] = (uint8_t)dbl;
        return t;
    }
    l0_inval(r);
    int t = 4 + (int)(g_l0_next++ % g_l0_nlanes);
    g_lane_used |= (uint16_t)(1u << (t - 4));
    uint16_t own = g_l0_owners[t - 4];
    for (int i = 0; i < 16; i++) if (own & (1u << i)) {
        if (g_l0_dirty & (1u << i))
            l0_flush_reg(b, (unsigned)i);
        g_l0[i] = -1;
    }
    g_l0_owners[t - 4] = (uint16_t)(1u << r);
    g_l0[r] = (int8_t)t; g_l0_dbl[r] = (uint8_t)dbl;
    return t;
}

void fpb_emit_store_check(A64Buf *b, int i, int batch)
{
    uint16_t m = g_fpb_stchk[i];
    if (!m) return;
    if (g_fpb_stlane[i]) {
        unsigned r = (unsigned)__builtin_ctz(m);
        int dbl = g_fpb_stdbl[i];
        int v = l0_src2(b, r, dbl);
        a64_fcmp(b, dbl, v, v);
        fpb_site_emit(b, i - 1, -1, -1, dbl);
        return;
    }
    for (unsigned r = 0; r < 16; r++) if (m & (1u << r)) l0_flush_reg(b, r);
    fpb_emit_regs_check(b, m, batch, i - 1, g_l0, g_l0_dbl);
}

static int mov128_pair_kind(const X86Insn *a, const X86Insn *c)
{
    static int dis = -1;
    if (dis < 0) dis = getenv("OCERZ_NO_LDP_PAIR") ? 1 : 0;
    if (dis || a->op != c->op || a->vex != c->vex || a->nops != 2 || c->nops != 2) return 0;
    if (a->op != OCERZ_OP_MOVUPS && a->op != OCERZ_OP_MOVAPS && a->op != OCERZ_OP_MOVDQA && a->op != OCERZ_OP_MOVDQU) return 0;
    if (a->vex & (OCERZ_VEX_L | OCERZ_VEX_NDS)) return 0;
    if (a->mode32 || c->mode32 || a->seg != OCERZ_SEG_NONE || c->seg != OCERZ_SEG_NONE || a->addrsize != 8 || c->addrsize != 8) return 0;
    const X86Operand *ad = &a->ops[0], *as = &a->ops[1], *cd = &c->ops[0], *cs = &c->ops[1];
    int load = ad->kind == OCERZ_OPK_XMM && as->kind == OCERZ_OPK_MEM && cd->kind == OCERZ_OPK_XMM && cs->kind == OCERZ_OPK_MEM;
    int store = ad->kind == OCERZ_OPK_MEM && as->kind == OCERZ_OPK_XMM && cd->kind == OCERZ_OPK_MEM && cs->kind == OCERZ_OPK_XMM;
    if (!load && !store) return 0;
    const X86Operand *am = load ? as : ad, *cm = load ? cs : cd;
    unsigned ar = load ? ad->reg : as->reg, cr = load ? cd->reg : cs->reg;
    if (!xmm_is_pinned(ar) || !xmm_is_pinned(cr) || (load && ar == cr)) return 0;
    if (am->riprel || cm->riprel || am->base != cm->base || am->index != cm->index || am->scale != cm->scale) return 0;
    if (cm->disp != am->disp + 16 || am->disp % 16 != 0 || am->disp < -1024 || am->disp > 1008) return 0;
    if (!vec_tso_relaxed() && (!mem_plain_access_ok(am) || !mem_plain_access_ok(cm))) return 0;
    return load ? 1 : 2;
}

int emit_mov128_pair(A64Buf *b, const X86Insn *a, const X86Insn *c, int i)
{
    int kind = mov128_pair_kind(a, c);
    if (!kind) return 0;
    if (g_fpb_of && g_fpb_of[i] != g_fpb_of[i + 1]) return 0;
    if (g_fpb_undo_ld[i] || g_fpb_undo_ld[i + 1] || g_fpb_undo[i] || g_fpb_undo[i + 1] || g_fpb_stchk[i + 1]) return 0;
    const X86Operand *am = kind == 1 ? &a->ops[1] : &a->ops[0];
    unsigned ar = kind == 1 ? a->ops[0].reg : a->ops[1].reg, cr = kind == 1 ? c->ops[0].reg : c->ops[1].reg;
    int va = xmm_vreg(ar), vc = xmm_vreg(cr);
    if (kind == 2) { l0_flush_reg(b, ar); l0_flush_reg(b, cr); }
    int ra; uint32_t disp;
    if (!emit_mem_ea_plain_ex(b, a, am, 16, &ra, &disp, 1)) return 0;
    int32_t d = (int32_t)disp;
    int pair = d % 16 == 0 && d >= -1024 && d <= 1008;
    if (kind == 1) {
        l0_inval(ar); l0_inval(cr);
        if (pair) a64_ldp_q_off(b, va, vc, ra, d);
        else { a64_ldr_v(b, 16, va, ra, disp); a64_ldr_v(b, 16, vc, ra, disp + 16); }
        if (a->vex) { emit_ymmh_clear(b, ar); emit_ymmh_clear(b, cr); }
    } else {
        if (pair) a64_stp_q_off(b, va, vc, ra, d);
        else { a64_str_v(b, 16, va, ra, disp); a64_str_v(b, 16, vc, ra, disp + 16); }
    }
    g_mov_skip[i + 1] = 1;
    return 1;
}

int vex_cmps_blendv_pair(const X86Insn *c, const X86Insn *v)
{
    if (!c->vex || !v->vex || ((c->vex | v->vex) & OCERZ_VEX_L) || c->mode32 || v->mode32) return 0;
    if (!((c->op == OCERZ_OP_CMPSDX && v->op == OCERZ_OP_BLENDVPD) ||
          (c->op == OCERZ_OP_CMPSS && v->op == OCERZ_OP_BLENDVPS))) return 0;
    if (!(c->vex & OCERZ_VEX_NDS) || c->nops != 3 || c->ops[0].kind != OCERZ_OPK_XMM ||
        c->ops[1].kind != OCERZ_OPK_XMM || c->ops[2].kind != OCERZ_OPK_IMM || (c->ops[2].imm & 0x1f) >= 8) return 0;
    if (!(v->vex & OCERZ_VEX_NDS) || !(v->vex & OCERZ_VEX_IS4) || v->nops != 3 || v->ops[0].kind != OCERZ_OPK_XMM ||
        v->ops[1].kind != OCERZ_OPK_XMM || v->ops[2].kind != OCERZ_OPK_XMM) return 0;
    if (c->seg != OCERZ_SEG_NONE || v->seg != OCERZ_SEG_NONE) return 0;
    unsigned m = c->ops[0].reg, s1 = v->vvvv & 15, s2 = v->ops[1].reg;
    if (v->ops[2].reg != m || s1 == m || s2 == m) return 0;
    if (!xmm_is_pinned(m) || !xmm_is_pinned(c->vvvv & 15) || !xmm_is_pinned(c->ops[1].reg) ||
        !xmm_is_pinned(v->ops[0].reg) || !xmm_is_pinned(s1) || !xmm_is_pinned(s2)) return 0;
    return l0_enabled();
}

int l0_fixed_setup(A64Buf *b, const X86Insn *insns, int n)
{
    int cnt[16] = {0}; int8_t firstdbl[16]; uint8_t wfirst[16];
    memset(firstdbl, -1, sizeof firstdbl);
    memset(wfirst, 0, sizeof wfirst);
    for (int i = 0; i < n; i++) {
        int packed, dbl, from_mem, sq;
        int c = fpb_class(&insns[i], &packed, &dbl, &from_mem, &sq);
        if (insns[i].nops >= 1 && insns[i].ops[0].kind == OCERZ_OPK_XMM) {
            unsigned dr = insns[i].ops[0].reg;
            if (!wfirst[dr]) {
                int selfzero = !insns[i].vex && (insns[i].op == OCERZ_OP_XORPS || insns[i].op == OCERZ_OP_PXOR) &&
                               insns[i].nops >= 2 && insns[i].ops[1].kind == OCERZ_OPK_XMM &&
                               insns[i].ops[1].reg == dr;
                wfirst[dr] = (uint8_t)((c == 2 || selfzero) ? 2 : 1);
            }
        }
        if (insns[i].nops >= 2 && insns[i].ops[1].kind == OCERZ_OPK_XMM && !wfirst[insns[i].ops[1].reg])
            wfirst[insns[i].ops[1].reg] = 1;
        int use = (c == 1 || c == 3) && !packed;
        if (!use && !insns[i].vex && (insns[i].op == OCERZ_OP_CVTSI2SD || insns[i].op == OCERZ_OP_CVTSI2SS)) {
            use = 1; dbl = insns[i].op == OCERZ_OP_CVTSI2SD;
        }
        if (!use && i + 1 < n && vex_cmps_blendv_pair(&insns[i], &insns[i + 1])) {
            int pd = insns[i].op == OCERZ_OP_CMPSDX;
            unsigned regs[5] = { insns[i].vvvv & 15, insns[i].ops[1].reg, insns[i + 1].ops[0].reg,
                                 insns[i + 1].vvvv & 15, insns[i + 1].ops[1].reg };
            for (int q = 0; q < 5; q++) {
                cnt[regs[q]]++;
                if (firstdbl[regs[q]] < 0) firstdbl[regs[q]] = (int8_t)pd;
            }
            continue;
        }
        if (!use) continue;
        for (int q = 0; q < insns[i].nops && q < 2; q++) {
            const X86Operand *o = &insns[i].ops[q];
            if (o->kind == OCERZ_OPK_XMM && xmm_is_pinned(o->reg)) {
                cnt[o->reg]++;
                if (firstdbl[o->reg] < 0) firstdbl[o->reg] = (int8_t)dbl;
            }
        }
        if ((insns[i].vex & OCERZ_VEX_NDS) && xmm_is_pinned(insns[i].vvvv & 15)) {
            unsigned v = insns[i].vvvv & 15;
            cnt[v]++;
            if (firstdbl[v] < 0) firstdbl[v] = (int8_t)dbl;
        }
    }
    int key[16];
    for (int r = 0; r < 16; r++)
        key[r] = cnt[r] >= 2 ? cnt[r] + (wfirst[r] == 2 ? 0 : 1000) : 0;
    int lanes = 0;
    for (int k = 0; k < g_l0_nlanes; k++) {
        int best = -1;
        for (int r = 0; r < 16; r++)
            if (g_l0_fixed_lane[r] < 0 && key[r] > 0 && (best < 0 || key[r] > key[best])) best = r;
        if (best < 0) break;
        g_l0_fixed_lane[best] = (int8_t)(4 + lanes);
        g_l0_fixed_dbl[best] = (uint8_t)(firstdbl[best] > 0);
        lanes++;
    }
    if (!lanes) return 0;
    g_l0_fixed_dirty = 0;
    for (int i = 0; i < n; i++)
        if (insn_writes_xmm0(&insns[i])) g_l0_fixed_dirty |= (uint16_t)(1u << insns[i].ops[0].reg);
    for (int r = 0; r < 16; r++) if (g_l0_fixed_lane[r] >= 0)
        a64_v_mov(b, g_l0_fixed_lane[r], xmm_vreg(r));
    g_l0_fixed = 1;
    l0_fixed_map();
    return 1;
}

int yc_setup(A64Buf *b, const X86Insn *insns, int n)
{
    static int dis = -1;
    if (dis < 0) dis = getenv("OCERZ_NO_YMMH_CACHE") ? 1 : 0;
    if (dis) return 0;
    int cnt[16] = {0};
    for (int i = 0; i < n; i++) {
        const X86Insn *in = &insns[i];
        if (!(in->vex & OCERZ_VEX_L) || in->mode32) continue;
        for (int k = 0; k < in->nops; k++)
            if (in->ops[k].kind == OCERZ_OPK_XMM && in->ops[k].reg < 16) cnt[in->ops[k].reg]++;
        if (in->vex & OCERZ_VEX_NDS) cnt[in->vvvv & 15]++;
    }
    uint16_t used = g_lane_used;
    for (int r = 0; r < 16; r++) if (g_l0_fixed_lane[r] >= 0) used |= (uint16_t)(1u << (g_l0_fixed_lane[r] - 4));
    int got = 0;
    for (;;) {
        int best = -1;
        for (int r = 0; r < 16; r++)
            if (g_yc[r] < 0 && cnt[r] > 0 && (best < 0 || cnt[r] > cnt[best])) best = r;
        if (best < 0) break;
        int lane = -1;
        for (int k = g_l0_nlanes - 1; k >= 0; k--) if (!(used & (1u << k))) { lane = k; break; }
        if (lane < 0) break;
        used |= (uint16_t)(1u << lane);
        g_lane_used |= (uint16_t)(1u << lane);
        g_yc[best] = (int8_t)(4 + lane);
        got++;
    }
    if (!got) return 0;
    for (int r = 0; r < 16; r++) if (g_yc[r] >= 0) {
        a64_ldr_v(b, 16, g_yc[r], 20, YMMH_OFF + (uint32_t)r * 16);
        g_yc_dirty |= (uint16_t)(1u << r);
    }
    g_l0_fixed = 1;
    return 1;
}

void l0_fixed_restore(A64Buf *b)
{
    uint16_t pend = 0;
    for (int r = 0; r < 16; r++) {
        int t = g_l0_fixed_lane[r];
        if (t >= 0 ? !(g_l0[r] == t && g_l0_dbl[r] == g_l0_fixed_dbl[r]) : g_l0[r] >= 0)
            pend |= (uint16_t)(1u << r);
    }
    while (pend) {
        int r = -1;







        for (int c = 0; c < 16 && r < 0; c++) {
            if (!(pend & (1u << c))) continue;
            int t = g_l0_fixed_lane[c], blocked = 0;
            for (int o = 0; t >= 0 && o < 16; o++)
                if (o != c && (pend & (1u << o)) && g_l0[o] == t) blocked = 1;
            if (!blocked) r = c;
        }
        if (r < 0) {
            for (int c = 0; c < 16; c++) if (pend & (1u << c)) l0_flush_reg(b, (unsigned)c);
            for (int c = 0; c < 16; c++) if ((pend & (1u << c)) && g_l0_fixed_lane[c] >= 0)
                a64_v_mov(b, g_l0_fixed_lane[c], xmm_vreg((unsigned)c));
            break;
        }
        int t = g_l0_fixed_lane[r], u = g_l0[r];
        if (t < 0) {
            l0_flush_reg(b, (unsigned)r);
            l0_inval((unsigned)r);
        } else if (u >= 0 && g_l0_dbl[r] == g_l0_fixed_dbl[r]) {
            if (g_l0_fixed_dbl[r]) a64_fmov_d_d(b, t, u); else a64_fmov_s_s(b, t, u);
        } else {
            l0_flush_reg(b, (unsigned)r);
            a64_v_mov(b, t, xmm_vreg((unsigned)r));
        }
        pend &= (uint16_t)~(1u << r);
    }
    l0_fixed_map();
}

int cmps_blendv_fusable(int cmps_idx)
{
    if (!g_cur_insns || cmps_idx < 0 || cmps_idx + 1 >= g_cur_insns_n) return 0;
    const X86Insn *c = &g_cur_insns[cmps_idx], *v = &g_cur_insns[cmps_idx + 1];
    if (c->vex || v->vex) return 0;
    if (c->ops[0].kind != OCERZ_OPK_XMM || c->ops[0].reg != 0 || c->nops < 3) return 0;
    if (!((c->op == OCERZ_OP_CMPSDX && v->op == OCERZ_OP_BLENDVPD) ||
          (c->op == OCERZ_OP_CMPSS && v->op == OCERZ_OP_BLENDVPS))) return 0;
    if (v->nops < 2 || v->ops[0].kind != OCERZ_OPK_XMM || v->ops[0].reg == 0 ||
        !xmm_is_pinned(v->ops[0].reg) || !xmm_is_pinned(0)) return 0;
    if (v->ops[1].kind == OCERZ_OPK_XMM && v->ops[1].reg == 0) return 0;
    if (v->seg != OCERZ_SEG_NONE || !l0_enabled()) return 0;
    return 1;
}

int vex_lane_aware(const X86Insn *insn)
{
    if (!insn->vex || (insn->vex & OCERZ_VEX_L) || insn->mode32) return 0;
    if (g_cur_insns && insn >= g_cur_insns && insn < g_cur_insns + g_cur_insns_n) {
        if ((insn->op == OCERZ_OP_CMPSS || insn->op == OCERZ_OP_CMPSDX) && insn + 1 < g_cur_insns + g_cur_insns_n)
            return vex_cmps_blendv_pair(insn, insn + 1);
        if ((insn->op == OCERZ_OP_BLENDVPD || insn->op == OCERZ_OP_BLENDVPS) && insn > g_cur_insns)
            return vex_cmps_blendv_pair(insn - 1, insn);
    }
    if (insn->op >= OCERZ_OP_VFMA_FIRST && insn->op <= OCERZ_OP_VFMA_LAST) {
        int kind = ((insn->op - OCERZ_OP_VFMA_FIRST) >> 1) % 10;
        return kind >= 2 && (kind & 1);
    }
    switch (insn->op) {
    case OCERZ_OP_ADDSS: case OCERZ_OP_ADDSD: case OCERZ_OP_SUBSS: case OCERZ_OP_SUBSD:
    case OCERZ_OP_MULSS: case OCERZ_OP_MULSD: case OCERZ_OP_DIVSS: case OCERZ_OP_DIVSD:
    case OCERZ_OP_SQRTSS: case OCERZ_OP_SQRTSD: case OCERZ_OP_MINSS: case OCERZ_OP_MINSD:
    case OCERZ_OP_MAXSS: case OCERZ_OP_MAXSD:
    case OCERZ_OP_COMISS: case OCERZ_OP_COMISD: case OCERZ_OP_UCOMISS: case OCERZ_OP_UCOMISD:
    case OCERZ_OP_CVTTSS2SI: case OCERZ_OP_CVTTSD2SI:
    case OCERZ_OP_MOVSS: case OCERZ_OP_MOVSDX: case OCERZ_OP_MOVDDUP:
        return 1;
    default: return 0;
    }
}

void l0_pre_insn(A64Buf *b, const X86Insn *insn)
{
    if (vex_lane_aware(insn) || !(insn->vex || !l0_aware_op(insn->op))) return;
    uint16_t use, kill;
    fpb2_usedef(insn, &use, &kill);
    for (int k = 0; k < insn->nops; k++)
        if (insn->ops[k].kind == OCERZ_OPK_XMM && insn->ops[k].reg < 16 && (use & (1u << insn->ops[k].reg)))
            l0_flush_reg(b, insn->ops[k].reg);
    if (insn->vex & OCERZ_VEX_NDS)
        l0_flush_reg(b, insn->vvvv);
    if (insn->op == OCERZ_OP_BLENDVPD || insn->op == OCERZ_OP_BLENDVPS ||
        insn->op == OCERZ_OP_PBLENDVB)
        l0_flush_reg(b, 0);
    for (int k = 0; k < insn->nops; k++)
        if (insn->ops[k].kind == OCERZ_OPK_XMM && (k == 0 || insn->op == OCERZ_OP_BLENDVPD ||
            insn->op == OCERZ_OP_BLENDVPS || insn->op == OCERZ_OP_PBLENDVB))
            l0_inval(insn->ops[k].reg);
    if (insn->op == OCERZ_OP_FXRSTOR || insn->op == OCERZ_OP_SYSCALL) { l0_flush_all(b); l0_reset(); }
}

static int l0_aware_op(unsigned op)
{
    switch (op) {
    case OCERZ_OP_BLENDVPD: case OCERZ_OP_BLENDVPS:
    case OCERZ_OP_CMPSS: case OCERZ_OP_CMPSDX:
    case OCERZ_OP_ADDSS: case OCERZ_OP_ADDSD: case OCERZ_OP_SUBSS: case OCERZ_OP_SUBSD:
    case OCERZ_OP_MULSS: case OCERZ_OP_MULSD: case OCERZ_OP_DIVSS: case OCERZ_OP_DIVSD:
    case OCERZ_OP_SQRTSS: case OCERZ_OP_SQRTSD: case OCERZ_OP_MINSS: case OCERZ_OP_MINSD:
    case OCERZ_OP_MAXSS: case OCERZ_OP_MAXSD:
    case OCERZ_OP_COMISS: case OCERZ_OP_COMISD: case OCERZ_OP_UCOMISS: case OCERZ_OP_UCOMISD:
    case OCERZ_OP_CVTTSS2SI: case OCERZ_OP_CVTTSD2SI:
    case OCERZ_OP_MOVAPS: case OCERZ_OP_MOVUPS: case OCERZ_OP_MOVDQA: case OCERZ_OP_MOVDQU:
    case OCERZ_OP_MOVSS: case OCERZ_OP_MOVSDX: case OCERZ_OP_MOVDDUP:
        return 1;
    default: return 0;
    }
}

static X87Run g_x87_run[X87_RUN_MAX];

int g_n_x87_run;

int g_x87_cur = -1;

static struct { uint32_t *site; int16_t run, idx; } g_x87_site[X87_SITE_MAX];

int g_n_x87_site;

static struct { uint32_t *site, *back; uint8_t kind, op, r0, r1; int16_t run, idx; } g_x87_frag[X87_FRAG_MAX];

static int g_x87_fr0 = 0;

static int g_x87_fr1 = 1;

int g_n_x87_frag;

int g_x87_frag_open;

int g_x87_nzcv = -1;

static int g_x87_nzcv_live;

int g_x87_delta;

int g_x87_live;

static int g_x87_rc_near;

int g_x87_btop = -1;

int g_x87_spec = -1;

int8_t g_x87_lane[8] = {0};

int g_x87_lanes_on;

uint8_t g_x87_lv;

static int x87_mem_ok(const X86Insn *in, const X86Operand *o, int s1, int s2, int s3)
{
    if (o->kind != OCERZ_OPK_MEM || in->seg != OCERZ_SEG_NONE) return 0;
    if (in->addrsize != (in->mode32 ? 4 : 8)) return 0;
    return o->size == s1 || o->size == s2 || o->size == s3;
}

int x87_run_flags(const X86Insn *in)
{
    static int off = -1;
    if (off < 0) off = getenv("OCERZ_NO_JIT_X87") ? 1 : 0;
    if (off || in->vex) return 0;
    const X86Operand *o = &in->ops[0];
    int st1 = in->nops == 1 && o->kind == OCERZ_OPK_ST;
    int st2 = in->nops == 2 && o->kind == OCERZ_OPK_ST && in->ops[1].kind == OCERZ_OPK_ST;
    int m1 = in->nops == 1;
    const int arith = X87R_OK | X87R_FCW | X87R_MXCSR;
    switch (in->op) {
    case OCERZ_OP_FLD:
        return st1 || (m1 && x87_mem_ok(in, o, 4, 8, 0)) ? X87R_OK : 0;
    case OCERZ_OP_FST: case OCERZ_OP_FSTP:
        if (st1 || (m1 && x87_mem_ok(in, o, 8, 0, 0))) return X87R_OK;
        return m1 && x87_mem_ok(in, o, 4, 0, 0) ? arith : 0;
    case OCERZ_OP_FILD:
        if (m1 && x87_mem_ok(in, o, 2, 4, 0)) return X87R_OK;
        return m1 && x87_mem_ok(in, o, 8, 0, 0) ? X87R_OK | X87R_MXCSR : 0;

    case OCERZ_OP_FIST: case OCERZ_OP_FISTP:
        return m1 && x87_mem_ok(in, o, 2, 4, 8) ? X87R_OK | (ENV_ON("OCERZ_NO_FIST_RC") ? X87R_FCW : X87R_RC) : 0;
    case OCERZ_OP_FISTTP:
        return m1 && x87_mem_ok(in, o, 2, 4, 8) ? X87R_OK : 0;
    case OCERZ_OP_FLDZ: case OCERZ_OP_FLD1: case OCERZ_OP_FLDPI: case OCERZ_OP_FLDL2E:
    case OCERZ_OP_FLDL2T: case OCERZ_OP_FLDLG2: case OCERZ_OP_FLDLN2:
    case OCERZ_OP_FCHS: case OCERZ_OP_FABS:
    case OCERZ_OP_FNCLEX: case OCERZ_OP_FWAIT: case OCERZ_OP_FINCSTP: case OCERZ_OP_FDECSTP:
    case OCERZ_OP_FCOMPP: case OCERZ_OP_FUCOMPP: case OCERZ_OP_FTST:
        return in->nops == 0 ? X87R_OK : 0;
    case OCERZ_OP_FADD: case OCERZ_OP_FSUB: case OCERZ_OP_FSUBR:
    case OCERZ_OP_FMUL: case OCERZ_OP_FDIV: case OCERZ_OP_FDIVR:
        return st2 || (m1 && x87_mem_ok(in, o, 4, 8, 0)) ? arith : 0;
    case OCERZ_OP_FADDP: case OCERZ_OP_FSUBP: case OCERZ_OP_FSUBRP:
    case OCERZ_OP_FMULP: case OCERZ_OP_FDIVP: case OCERZ_OP_FDIVRP:
        return st2 ? arith : 0;
    case OCERZ_OP_FIADD: case OCERZ_OP_FISUB: case OCERZ_OP_FISUBR:
    case OCERZ_OP_FIMUL: case OCERZ_OP_FIDIV: case OCERZ_OP_FIDIVR:
        return m1 && x87_mem_ok(in, o, 2, 4, 0) ? arith : 0;
    case OCERZ_OP_FSQRT:
        return in->nops == 0 ? arith : 0;
    case OCERZ_OP_FRNDINT:
        return in->nops == 0 ? X87R_OK | X87R_FCW : 0;
    case OCERZ_OP_FXCH: case OCERZ_OP_FFREE: case OCERZ_OP_FFREEP:
    case OCERZ_OP_FUCOM: case OCERZ_OP_FUCOMP:
        return st1 ? X87R_OK : 0;
    case OCERZ_OP_FCOM: case OCERZ_OP_FCOMP:
        return st1 || (m1 && x87_mem_ok(in, o, 4, 8, 0)) ? X87R_OK : 0;
    case OCERZ_OP_FICOM: case OCERZ_OP_FICOMP:
        return m1 && x87_mem_ok(in, o, 2, 4, 0) ? X87R_OK : 0;
    case OCERZ_OP_FCOMI: case OCERZ_OP_FCOMIP: case OCERZ_OP_FUCOMI: case OCERZ_OP_FUCOMIP:
    case OCERZ_OP_FCMOVCC:
        return st2 && o->reg == 0 ? X87R_OK : 0;
    case OCERZ_OP_FNSTSW:
        if (m1 && o->kind == OCERZ_OPK_REG) return o->reg == OCERZ_RAX && o->size == 2 && !o->high8 ? X87R_OK : 0;
        return m1 && x87_mem_ok(in, o, 2, 0, 0) ? X87R_OK : 0;
    case OCERZ_OP_FNSTCW:
        return m1 && x87_mem_ok(in, o, 2, 0, 0) ? X87R_OK : 0;
    case OCERZ_OP_FLDCW:
        return m1 && x87_mem_ok(in, o, 2, 0, 0) ? X87R_OK | X87R_END : 0;
    case OCERZ_OP_FNINIT:
        return in->nops == 0 ? X87R_OK | X87R_END : 0;
    default:
        return 0;
    }
}

static void x87_slow_at(A64Buf *b, int cond, int run, int idx)
{
    g_x87_site[g_n_x87_site].site = a64_label(b);
    g_x87_site[g_n_x87_site].run = (int16_t)run;
    g_x87_site[g_n_x87_site].idx = (int16_t)idx;
    g_n_x87_site++;
    a64_bcond(b, cond, 0);
}

static void x87_slow_if(A64Buf *b, int cond)
{
    x87_slow_at(b, cond, g_x87_cur, g_cur_insn_idx);
}

static void x87_frag_if(A64Buf *b, int cond, int kind, int op)
{
    int f = g_n_x87_frag++;
    g_x87_frag[f].site = a64_label(b);
    g_x87_frag[f].back = NULL;
    g_x87_frag[f].kind = (uint8_t)kind;
    g_x87_frag[f].op = (uint8_t)op;
    g_x87_frag[f].r0 = (uint8_t)g_x87_fr0;
    g_x87_frag[f].r1 = (uint8_t)g_x87_fr1;
    g_x87_frag[f].run = (int16_t)g_x87_cur;
    g_x87_frag[f].idx = (int16_t)g_cur_insn_idx;
    a64_bcond(b, cond, 0);
}

static void x87_frag_land(A64Buf *b)
{
    for (int f = g_x87_frag_open; f < g_n_x87_frag; f++)
        g_x87_frag[f].back = a64_label(b);
    g_x87_frag_open = g_n_x87_frag;
}

static void x87_ld(A64Buf *b)
{
    static int check = -1;
    if (!g_x87_live) {
        a64_ldr(b, 8, X87S, 20, X87_CTL_OFF);
        return;
    }
    if (check < 0) check = ENV_ON("OCERZ_X87_CHECK") ? 1 : 0;
    if (check) {
        a64_ldr(b, 8, X87P, 20, X87_CTL_OFF);
        a64_subs_reg(b, 1, A64_ZR, X87P, X87S, 0);
        a64_bcond(b, A64_EQ, 2);
        a64_emit32(b, 0xd4200000u | (0x87u << 5));
        x87_top(b, X87P);
        a64_lsl_imm(b, 1, X87P, X87P, 40);
        a64_eor_reg(b, 1, X87P, X87P, X87S, 0);
        (void)a64_try_ands_imm(b, 1, A64_ZR, X87P, 7ull << 40);
        a64_bcond(b, A64_EQ, 2);
        a64_emit32(b, 0xd4200000u | (0x88u << 5));
    }
}

static const uint32_t *g_x87_st_mark;

static void x87_st(A64Buf *b)
{
    static int off = -1;
    if (off < 0) off = ENV_ON("OCERZ_NO_X87_ST_ELIDE") ? 1 : 0;
    if (!off && g_x87_live && g_x87_st_mark && g_x87_st_mark <= b->p) {
        int dirty = 0;
        for (const uint32_t *w = g_x87_st_mark; w < b->p && !dirty; w++)
            dirty = a64_word_may_write_reg(*w, X87S);
        if (!dirty) return;
    }
    a64_str(b, 8, X87S, 20, X87_CTL_OFF);
    g_x87_st_mark = b->p;
}

static void x87_reload(A64Buf *b) { if (g_x87_live) { a64_ldr(b, 8, X87S, 20, X87_CTL_OFF); g_x87_st_mark = b->p; } }

static void x87_top_at(A64Buf *b, int rd, int k)
{
    if (g_x87_live && g_x87_spec >= 0) {
        a64_movz(b, rd, (uint16_t)((g_x87_spec + g_x87_delta + k) & 7), 0);
        return;
    }
    if (g_x87_live) {
        a64_ldr(b, 2, rd, 20, X87_TOP0_OFF);
        k += g_x87_delta;
    } else {
        a64_ubfx(b, 1, rd, X87S, 40, 3);
    }
    if (k & 7) {
        a64_add_imm(b, 0, rd, rd, (uint32_t)(k & 7));
        (void)a64_try_and_imm(b, 0, rd, rd, 7);
    }
}

static void x87_top(A64Buf *b, int rd) { x87_top_at(b, rd, 0); }

static void x87_phys(A64Buf *b, int rd, int i) { x87_top_at(b, rd, i); }

static void x87_newtop(A64Buf *b, int rd) { x87_top_at(b, rd, -1); }

static void x87_slot(A64Buf *b, int rd, int rp) { a64_add_reg(b, 1, rd, 20, rp, 3); }

static int x87_lane_of(int rel)
{
    if (!g_x87_lanes_on || g_x87_spec < 0 || !g_x87_live) return -1;
    return g_x87_lane[rel & 7];
}

static int x87_lane_get(A64Buf *b, int rel)
{
    int l = x87_lane_of(rel);
    if (l < 0) return -1;
    int p = rel & 7;
    if (!(g_x87_lv >> p & 1)) {
        a64_ldr_v(b, 8, l, 20, X87_FPR_OFF + 8u * (unsigned)p);
        g_x87_lv |= (uint8_t)(1u << p);
    }
    return l;
}

static void x87_lane_put(A64Buf *b, int rel, int v)
{
    int l = x87_lane_of(rel);
    if (l < 0) return;
    if (l != v) a64_fmov_d_d(b, l, v);
    g_x87_lv |= (uint8_t)(1u << (rel & 7));
}

static void x87_lane_read(A64Buf *b, int vd, int rel, int addr)
{
    int l = x87_lane_get(b, rel);
    if (l >= 0) a64_fmov_d_d(b, vd, l);
    else        a64_ldr_v(b, 8, vd, addr, X87_FPR_OFF);
}

static void x87_bit(A64Buf *b, int rd, int rp) { a64_movz(b, rd, 1, 0); a64_lslv(b, 0, rd, rd, rp); }

static int8_t g_x87_c1k;

static int8_t g_x87_tagk[8];

static int8_t g_x87_xokk[8];

static int x87_rel(int i) { return ((g_x87_live && g_x87_spec >= 0 ? g_x87_spec : 0) + g_x87_delta + i) & 7; }

int g_x87_kcarry;

int g_x87_spec_cut;

static int g_x87_fcmov_static;

static void x87_know_reset(void)
{
    memset(g_x87_tagk, 0, sizeof g_x87_tagk);
    memset(g_x87_xokk, 0, sizeof g_x87_xokk);
    g_x87_c1k = 0;
}

static void x87_clear_c1(A64Buf *b)
{
    if (g_x87_live && g_x87_c1k) return;
    (void)a64_try_and_imm(b, 1, X87S, X87S, ~XS_C1);
    g_x87_c1k = (int8_t)g_x87_live;
}

static void x87_tag(A64Buf *b, int rp, int rt, int rel, int image)
{
    int want = image ? 1 : 2;
    int tag = !g_x87_live || g_x87_tagk[rel] != 1;
    int xok = !g_x87_live || g_x87_xokk[rel] != want;
    if (g_x87_live && g_x87_spec >= 0) {

        if (tag) (void)a64_try_orr_imm(b, 1, X87S, X87S, 1ull << (32 + rel));
        if (xok && image) (void)a64_try_orr_imm(b, 1, X87S, X87S, 1ull << (XS_XOK + rel));
        if (xok && !image) (void)a64_try_and_imm(b, 1, X87S, X87S, ~(1ull << (XS_XOK + rel)));
        g_x87_tagk[rel] = 1;
        g_x87_xokk[rel] = (int8_t)want;
        return;
    }
    if (tag || xok) x87_bit(b, rt, rp);
    if (tag) a64_orr_reg(b, 1, X87S, X87S, rt, 32);
    if (xok && image) a64_orr_reg(b, 1, X87S, X87S, rt, XS_XOK);
    if (xok && !image) a64_bic_reg(b, 1, X87S, X87S, rt, XS_XOK);
    if (g_x87_live) {
        g_x87_tagk[rel] = 1;
        g_x87_xokk[rel] = (int8_t)want;
    }
}

static void x87_pop(A64Buf *b)
{
    int rel = x87_rel(0);
    if (g_x87_live && g_x87_spec >= 0) {
        if (g_x87_tagk[rel] != 2) (void)a64_try_and_imm(b, 1, X87S, X87S, ~(1ull << (32 + rel)));
    } else if (!g_x87_live || g_x87_tagk[rel] != 2) {
        x87_top(b, JT0);
        x87_bit(b, JTT, JT0);
        a64_bic_reg(b, 1, X87S, X87S, JTT, 32);
    }
    if (g_x87_live) g_x87_tagk[rel] = 2;
    x87_phys(b, JT0, 1);
    a64_bfi(b, 1, X87S, JT0, 40, 8);
    g_x87_delta++;
}

static void x87_copy(A64Buf *b, int rd, int rs, int rdrel, int rsrel)
{
    int sx = g_x87_live ? g_x87_xokk[rsrel] : 0;
    x87_slot(b, X87Q, rs);
    x87_slot(b, JTT, rd);
    int ls = x87_lane_get(b, rsrel), v = ls >= 0 ? ls : VX0;
    if (ls < 0) a64_ldr_v(b, 8, VX0, X87Q, X87_FPR_OFF);
    a64_str_v(b, 8, v, JTT, X87_FPR_OFF);
    x87_lane_put(b, rdrel, v);
    if (sx == 2) {
        x87_tag(b, rd, JTU, rdrel, 0);
        return;
    }
    if (sx == 1) {
        a64_ldr_v(b, 8, VX1, X87Q, X87_XM_OFF);
        a64_str_v(b, 8, VX1, JTT, X87_XM_OFF);
        a64_add_reg(b, 1, X87Q, 20, rs, 1);
        a64_add_reg(b, 1, JTT, 20, rd, 1);
        a64_ldr(b, 2, JTU, X87Q, X87_XE_OFF);
        a64_str(b, 2, JTU, JTT, X87_XE_OFF);
        x87_tag(b, rd, JTU, rdrel, 1);
        return;
    }
    a64_lsrv(b, 1, JTU, X87S, rs);
    a64_ubfx(b, 1, JTU, JTU, XS_XOK, 1);
    uint32_t *noimg = a64_label(b);
    a64_cbz(b, 0, JTU, 0);
    a64_ldr_v(b, 8, VX1, X87Q, X87_XM_OFF);
    a64_str_v(b, 8, VX1, JTT, X87_XM_OFF);
    a64_add_reg(b, 1, X87Q, 20, rs, 1);
    a64_add_reg(b, 1, JTT, 20, rd, 1);
    a64_ldr(b, 2, JTU, X87Q, X87_XE_OFF);
    a64_str(b, 2, JTU, JTT, X87_XE_OFF);
    a64_movz(b, JTU, 1, 0);
    a64_patch_cbz(noimg, a64_label(b));
    a64_lslv(b, 0, JTU, JTU, rd);
    x87_bit(b, JTT, rd);
    a64_bic_reg(b, 1, X87S, X87S, JTT, XS_XOK);
    a64_orr_reg(b, 1, X87S, X87S, JTU, XS_XOK);
    if (!g_x87_live || g_x87_tagk[rdrel] != 1)
        a64_orr_reg(b, 1, X87S, X87S, JTT, 32);
    if (g_x87_live) {
        g_x87_tagk[rdrel] = 1;
        g_x87_xokk[rdrel] = 0;
    }
}

static void x87_push(A64Buf *b, int vv, int image, int rm, int rs)
{
    x87_newtop(b, X87P);
    if (g_x87_live && g_x87_spec >= 0 && !image) {
        a64_str_v(b, 8, vv, 20, X87_FPR_OFF + 8u * (unsigned)x87_rel(-1));
    } else {
        x87_slot(b, X87Q, X87P);
        a64_str_v(b, 8, vv, X87Q, X87_FPR_OFF);
    }
    x87_lane_put(b, x87_rel(-1), vv);
    if (image) {
        a64_str(b, 8, rm, X87Q, X87_XM_OFF);
        a64_add_reg(b, 1, X87Q, 20, X87P, 1);
        a64_str(b, 2, rs, X87Q, X87_XE_OFF);
    }
    x87_tag(b, X87P, JT0, x87_rel(-1), image);
    a64_bfi(b, 1, X87S, X87P, 40, 8);
    g_x87_delta--;
    x87_st(b);
}

static void x87_int_image(A64Buf *b, int rv, int rm, int rs, int rt)
{
    a64_subs_imm(b, 1, A64_ZR, rv, 0);
    a64_csneg(b, 1, rm, rv, rv, A64_GE);
    a64_clz(b, 1, rs, rm);
    a64_lslv(b, 1, rm, rm, rs);
    a64_movz(b, rt, 16383 + 63, 0);
    a64_sub_reg(b, 0, rs, rt, rs, 0);
    (void)a64_try_orr_imm(b, 0, rt, rs, 0x8000);
    a64_csel(b, 0, rs, rt, rs, A64_LT);
    a64_csel(b, 0, rs, rs, A64_ZR, A64_NE);
}

static int x87_ld_int(A64Buf *b, const X86Insn *insn, int rd, int sext, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *o = &insn->ops[0];
    uint32_t *skip;
    if (!emit_sse_mem_addr(b, insn, o, o->size, exit_sites, n_exits, &skip)) return 0;
    emit_sse_mem_ld_gpr(b, o->size, rd);
    patch_guard_skip(skip, a64_label(b));
    if (sext && o->size == 2) a64_sxth(b, 1, rd, rd);
    else if (sext && o->size == 4) a64_sxtw(b, rd, rd);
    return 1;
}

static int x87_ld_real(A64Buf *b, const X86Insn *insn, int vd, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *o = &insn->ops[0];
    uint32_t *skip;
    if (!emit_sse_mem_addr(b, insn, o, o->size, exit_sites, n_exits, &skip)) return 0;
    emit_sse_mem_ld(b, o->size, vd);
    patch_guard_skip(skip, a64_label(b));
    if (o->size == 4) a64_fcvt_s2d(b, vd, vd);
    return 1;
}

static int x87_st_mem(A64Buf *b, const X86Insn *insn, int vec, int reg, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *o = &insn->ops[0];
    uint32_t *skip;
    if (!emit_sse_mem_addr(b, insn, o, o->size, exit_sites, n_exits, &skip)) return 0;
    if (vec) emit_sse_mem_st(b, o->size, reg);
    else     emit_sse_mem_st_gpr(b, o->size, reg);
    patch_guard_skip(skip, a64_label(b));
    return 1;
}

static void x87_result(A64Buf *b, int kind)
{
    a64_fmov_x_from_v(b, 1, JT0, VX2);
    a64_ubfx(b, 1, JTT, JT0, 52, 11);
    if (kind == XK_MUL || kind == XK_DIV) {
        a64_sub_imm(b, 0, JTT, JTT, 54);
        a64_subs_imm(b, 0, A64_ZR, JTT, 0x7fe - 54);
        x87_frag_if(b, A64_HI, XF_ZERO, kind);
    } else {
        a64_subs_imm(b, 0, A64_ZR, JTT, 0x7ff);
        x87_slow_if(b, A64_EQ);
    }
    (void)a64_try_ands_imm(b, 1, A64_ZR, X87S, XS_PC);
    x87_frag_if(b, A64_EQ, XF_PC24, kind);
    (void)a64_try_ands_imm(b, 1, A64_ZR, X87S, XS_PE);
    x87_frag_if(b, A64_EQ, XF_PE, kind);
    x87_frag_land(b);
}

static int x87_arith(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    int kind, rev = 0, popit = 0, intform = 0;
    switch (insn->op) {
    case OCERZ_OP_FADD: kind = XK_ADD; break;
    case OCERZ_OP_FADDP: kind = XK_ADD; popit = 1; break;
    case OCERZ_OP_FIADD: kind = XK_ADD; intform = 1; break;
    case OCERZ_OP_FSUB: kind = XK_SUB; break;
    case OCERZ_OP_FSUBP: kind = XK_SUB; popit = 1; break;
    case OCERZ_OP_FISUB: kind = XK_SUB; intform = 1; break;
    case OCERZ_OP_FSUBR: kind = XK_SUB; rev = 1; break;
    case OCERZ_OP_FSUBRP: kind = XK_SUB; rev = 1; popit = 1; break;
    case OCERZ_OP_FISUBR: kind = XK_SUB; rev = 1; intform = 1; break;
    case OCERZ_OP_FMUL: kind = XK_MUL; break;
    case OCERZ_OP_FMULP: kind = XK_MUL; popit = 1; break;
    case OCERZ_OP_FIMUL: kind = XK_MUL; intform = 1; break;
    case OCERZ_OP_FDIV: kind = XK_DIV; break;
    case OCERZ_OP_FDIVP: kind = XK_DIV; popit = 1; break;
    case OCERZ_OP_FIDIV: kind = XK_DIV; intform = 1; break;
    case OCERZ_OP_FDIVR: kind = XK_DIV; rev = 1; break;
    case OCERZ_OP_FDIVRP: kind = XK_DIV; rev = 1; popit = 1; break;
    case OCERZ_OP_FIDIVR: kind = XK_DIV; rev = 1; intform = 1; break;
    default: return 0;
    }
    const X86Operand *o = &insn->ops[0];
    int mem = o->kind != OCERZ_OPK_ST;
    int va = rev ? VX1 : VX0, vb = rev ? VX0 : VX1;
    if (mem && intform) {
        if (!x87_ld_int(b, insn, X87Q, 1, exit_sites, n_exits)) return 0;
        a64_scvtf(b, 1, 1, vb, X87Q);
    } else if (mem && !x87_ld_real(b, insn, vb, exit_sites, n_exits)) {
        return 0;
    }
    x87_ld(b);
    int drel = x87_rel(mem ? 0 : o->reg);

    int known = g_x87_live && g_x87_spec >= 0;
    if (!known) {
        x87_phys(b, X87P, mem ? 0 : o->reg);
        x87_slot(b, X87Q, X87P);
    }
    int la = x87_lane_get(b, drel);
    if (la >= 0) va = la;
    else if (known) a64_ldr_v(b, 8, va, 20, X87_FPR_OFF + 8u * (unsigned)drel);
    else a64_ldr_v(b, 8, va, X87Q, X87_FPR_OFF);
    if (!mem) {
        int brel = x87_rel(insn->ops[1].reg);
        int lb = x87_lane_get(b, brel);
        if (lb >= 0) {
            vb = lb;
        } else if (known) {
            a64_ldr_v(b, 8, vb, 20, X87_FPR_OFF + 8u * (unsigned)brel);
        } else {
            x87_phys(b, JT0, insn->ops[1].reg);
            x87_slot(b, JT0, JT0);
            a64_ldr_v(b, 8, vb, JT0, X87_FPR_OFF);
        }
    }
    int r0 = rev ? vb : va, r1 = rev ? va : vb;
    switch (kind) {
    case XK_ADD: a64_fadd_s(b, 1, VX2, r0, r1); break;
    case XK_SUB: a64_fsub_s(b, 1, VX2, r0, r1); break;
    case XK_MUL: a64_fmul_s(b, 1, VX2, r0, r1); break;
    default:     a64_fdiv_s(b, 1, VX2, r0, r1); break;
    }
    g_x87_fr0 = r0; g_x87_fr1 = r1;
    x87_result(b, kind);
    g_x87_fr0 = VX0; g_x87_fr1 = VX1;
    if (known) a64_str_v(b, 8, VX2, 20, X87_FPR_OFF + 8u * (unsigned)drel);
    else       a64_str_v(b, 8, VX2, X87Q, X87_FPR_OFF);
    x87_lane_put(b, drel, VX2);
    x87_tag(b, X87P, JT0, x87_rel(mem ? 0 : o->reg), 0);
    if (popit) x87_pop(b);
    x87_st(b);
    return 1;
}

static int x87_compare(A64Buf *b, const X86Insn *insn, uint64_t need, uint32_t **exit_sites, int *n_exits)
{
    int op = insn->op;
    const X86Operand *o = &insn->ops[0];
    int mem = insn->nops == 1 && o->kind == OCERZ_OPK_MEM;
    int fcomi = op == OCERZ_OP_FCOMI || op == OCERZ_OP_FCOMIP || op == OCERZ_OP_FUCOMI || op == OCERZ_OP_FUCOMIP;
    int pops = (op == OCERZ_OP_FCOMPP || op == OCERZ_OP_FUCOMPP) ? 2 :
               (op == OCERZ_OP_FCOMP || op == OCERZ_OP_FUCOMP || op == OCERZ_OP_FICOMP ||
                op == OCERZ_OP_FCOMIP || op == OCERZ_OP_FUCOMIP) ? 1 : 0;
    if (mem && (op == OCERZ_OP_FICOM || op == OCERZ_OP_FICOMP)) {
        if (!x87_ld_int(b, insn, X87Q, 1, exit_sites, n_exits)) return 0;
        a64_scvtf(b, 1, 1, VX1, X87Q);
    } else if (mem && !x87_ld_real(b, insn, VX1, exit_sites, n_exits)) {
        return 0;
    }
    x87_ld(b);
    int c0 = x87_lane_get(b, x87_rel(0));
    if (c0 < 0) {
        c0 = VX0;
        x87_top(b, X87P);
        x87_slot(b, X87Q, X87P);
        a64_ldr_v(b, 8, VX0, X87Q, X87_FPR_OFF);
    }
    if (op == OCERZ_OP_FTST) {
        a64_fcmp_zero(b, 1, c0);
    } else {
        int c1 = VX1;
        if (!mem) {
            int i = insn->nops == 2 ? insn->ops[1].reg : insn->nops == 1 ? o->reg : 1;
            c1 = x87_lane_get(b, x87_rel(i));
            if (c1 < 0) {
                c1 = VX1;
                x87_phys(b, JT0, i);
                x87_slot(b, JT0, JT0);
                a64_ldr_v(b, 8, VX1, JT0, X87_FPR_OFF);
            }
        }
        a64_fcmp(b, 1, c0, c1);
    }
    x87_slow_if(b, A64_VS);
    if (fcomi) {
        if (need) {
            a64_cset(b, JTT, A64_MI);
            a64_cset(b, JTU, A64_EQ);
            if (g_defer) a64_str(b, 4, A64_ZR, 20, CC_OP_OFF);
            a64_ldr(b, 8, JT0, 20, RF_OFF);
            a64_mov_imm64(b, X87Q, ~(uint64_t)JIT_ARITH_FLAGS);
            a64_and_reg(b, 1, JT0, JT0, X87Q, 0);
            a64_orr_reg(b, 1, JT0, JT0, JTT, 0);
            a64_orr_reg(b, 1, JT0, JT0, JTU, 6);
            a64_str(b, 8, JT0, 20, RF_OFF);
        }
        x87_clear_c1(b);
        g_x87_nzcv = g_cur_insn_idx;
    } else {
        a64_cset(b, JTT, A64_MI);
        a64_cset(b, JTU, A64_EQ);
        (void)a64_try_and_imm(b, 1, X87S, X87S, ~(7ull << 24));
        (void)a64_try_and_imm(b, 1, X87S, X87S, ~XS_C3);
        g_x87_c1k = (int8_t)g_x87_live;
        a64_orr_reg(b, 1, X87S, X87S, JTT, 24);
        a64_orr_reg(b, 1, X87S, X87S, JTU, 30);
    }
    for (int k = 0; k < pops; k++)
        x87_pop(b);
    x87_st(b);
    return 1;
}

static int x87_fist(A64Buf *b, const X86Insn *insn, int courier, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *o = &insn->ops[0];
    int popit = insn->op != OCERZ_OP_FIST;
    int c1_set = 0;
    x87_ld(b);
    x87_top(b, X87P);
    if (courier || insn->op == OCERZ_OP_FISTTP) {
        c1_set = !(g_x87_live && g_x87_c1k);
        x87_clear_c1(b);
    }
    if (courier) {
        x87_slot(b, X87Q, X87P);
        a64_ldr(b, 8, JTT, X87Q, X87_XM_OFF);
        a64_add_reg(b, 1, X87Q, 20, X87P, 1);
        a64_ldr(b, 2, JTU, X87Q, X87_XE_OFF);
        (void)a64_try_and_imm(b, 0, JT0, JTU, 0x7fff);
        a64_movz(b, X87Q, 16383 + 63, 0);
        a64_sub_reg(b, 0, JT0, X87Q, JT0, 0);
        a64_lsrv(b, 1, JTT, JTT, JT0);
        (void)a64_try_ands_imm(b, 0, A64_ZR, JTU, 0x8000);
        a64_csneg(b, 1, X87Q, JTT, JTT, A64_EQ);
    } else {
        if (!g_x87_live || g_x87_xokk[x87_rel(0)] != 2) {
            a64_lsrv(b, 1, JTT, X87S, X87P);
            (void)a64_try_ands_imm(b, 1, A64_ZR, JTT, 1ull << XS_XOK);
            x87_slow_if(b, A64_NE);
        }
        x87_slot(b, X87Q, X87P);
        x87_lane_read(b, VX0, x87_rel(0), X87Q);
        a64_fcmp(b, 1, VX0, VX0);
        x87_slow_if(b, A64_VS);
        if (insn->op == OCERZ_OP_FISTTP) a64_fcvtzs(b, 1, 1, X87Q, VX0);
        else if (g_x87_rc_near)          a64_fcvtns(b, 1, 1, X87Q, VX0);
        else {

            uint32_t *hi = a64_label(b); a64_tbnz(b, X87S, 11, 0);
            uint32_t *dn = a64_label(b); a64_tbnz(b, X87S, 10, 0);
            a64_fcvtns(b, 1, 1, X87Q, VX0);
            uint32_t *j1 = a64_label(b); a64_b(b, 0);
            a64_patch_tbz(dn, a64_label(b));
            a64_fcvtms(b, 1, 1, X87Q, VX0);
            uint32_t *j2 = a64_label(b); a64_b(b, 0);
            a64_patch_tbz(hi, a64_label(b));
            uint32_t *ch = a64_label(b); a64_tbnz(b, X87S, 10, 0);
            a64_fcvtps(b, 1, 1, X87Q, VX0);
            uint32_t *j3 = a64_label(b); a64_b(b, 0);
            a64_patch_tbz(ch, a64_label(b));
            a64_fcvtzs(b, 1, 1, X87Q, VX0);
            a64_patch_b(j1, a64_label(b));
            a64_patch_b(j2, a64_label(b));
            a64_patch_b(j3, a64_label(b));
        }
        if (o->size == 8) {
            a64_adds_imm(b, 1, A64_ZR, X87Q, 1);
            x87_slow_if(b, A64_VS);
            a64_subs_imm(b, 1, A64_ZR, X87Q, 1);
            x87_slow_if(b, A64_VS);
        } else {
            if (o->size == 2) a64_sxth(b, 1, JTT, X87Q);
            else              a64_sxtw(b, JTT, X87Q);
            a64_subs_reg(b, 1, A64_ZR, JTT, X87Q, 0);
            x87_slow_if(b, A64_NE);
        }

        if (insn->op != OCERZ_OP_FISTTP) {
            a64_fcvtzs(b, 1, 1, JTT, VX0);
            a64_subs_reg(b, 1, A64_ZR, X87Q, JTT, 0);
            a64_cset(b, JTU, A64_NE);
            a64_bfi(b, 1, X87S, JTU, 25, 1);
            g_x87_c1k = 0;
            c1_set = 1;
        }
        a64_scvtf(b, 1, 1, VX1, X87Q);
        a64_fcmp(b, 1, VX1, VX0);
        x87_frag_if(b, A64_NE, XF_SETPE, 0);
        x87_frag_land(b);
    }
    if (!x87_st_mem(b, insn, 0, X87Q, exit_sites, n_exits)) return 0;
    if (popit) {
        x87_pop(b);
        x87_st(b);
    } else if (c1_set) {
        x87_st(b);
    }
    return 1;
}

static int x87_fcmov_cond(unsigned cc)
{
    switch (cc) {
    case OCERZ_CC_B:  return A64_MI;
    case OCERZ_CC_AE: return A64_CS;
    case OCERZ_CC_E:  return A64_EQ;
    case OCERZ_CC_NE: return A64_NE;
    case OCERZ_CC_BE: return A64_LS;
    case OCERZ_CC_A:  return A64_HI;
    case OCERZ_CC_P:  return A64_NV;
    default:          return A64_AL;
    }
}

static int emit_x87_one(A64Buf *b, const X86Insn *insn, uint64_t need, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *o = &insn->ops[0];
    int op = insn->op;
    int st = insn->nops >= 1 && o->kind == OCERZ_OPK_ST;
    switch (op) {
    case OCERZ_OP_FLD:
        if (st) {
            x87_ld(b);
            if (o->reg == 7) {
                x87_newtop(b, X87P);
                x87_tag(b, X87P, JT0, x87_rel(-1), 0);
            } else {
                x87_phys(b, JT0, o->reg);
                x87_newtop(b, X87P);
                x87_copy(b, X87P, JT0, x87_rel(-1), x87_rel(o->reg));
            }
            a64_bfi(b, 1, X87S, X87P, 40, 8);
            g_x87_delta--;
            x87_st(b);
            return 1;
        }
        if (!x87_ld_real(b, insn, VX0, exit_sites, n_exits)) return 0;
        a64_fcmp(b, 1, VX0, VX0);
        x87_slow_if(b, A64_VS);
        x87_ld(b);
        x87_push(b, VX0, 0, 0, 0);
        return 1;
    case OCERZ_OP_FST: case OCERZ_OP_FSTP:
        x87_ld(b);
        if (st) {
            if (o->reg) {
                x87_phys(b, X87P, o->reg);
                x87_top(b, JT0);
                x87_copy(b, X87P, JT0, x87_rel(o->reg), x87_rel(0));
            }
            if (op == OCERZ_OP_FSTP) x87_pop(b);
            if (o->reg || op == OCERZ_OP_FSTP) x87_st(b);
            return 1;
        }
        x87_top(b, X87P);
        x87_slot(b, X87Q, X87P);
        x87_lane_read(b, VX0, x87_rel(0), X87Q);
        if (o->size == 4) {
            a64_fcvt_d2s(b, VX1, VX0);
            a64_fcvt_s2d(b, VX2, VX1);
            a64_fcmp(b, 1, VX2, VX0);
            x87_frag_if(b, A64_NE, XF_ST32, 0);
            x87_frag_land(b);
            if (!x87_st_mem(b, insn, 1, VX1, exit_sites, n_exits)) return 0;
        } else {
            a64_fcmp(b, 1, VX0, VX0);
            x87_slow_if(b, A64_VS);
            if (!x87_st_mem(b, insn, 1, VX0, exit_sites, n_exits)) return 0;
        }
        if (op == OCERZ_OP_FSTP) {
            x87_pop(b);
            x87_st(b);
        }
        return 1;
    case OCERZ_OP_FILD:
        if (!x87_ld_int(b, insn, X87Q, 1, exit_sites, n_exits)) return 0;
        a64_scvtf(b, 1, 1, VX0, X87Q);
        x87_int_image(b, X87Q, JTT, JTU, JT0);
        x87_ld(b);
        x87_push(b, VX0, 1, JTT, JTU);
        return 1;
    case OCERZ_OP_FIST: case OCERZ_OP_FISTP: case OCERZ_OP_FISTTP: {
        int i = g_cur_insn_idx;
        int courier = op == OCERZ_OP_FISTP && o->size == 8 && i > g_x87_run[g_x87_cur].first &&
                      g_cur_insns[i - 1].op == OCERZ_OP_FILD && g_cur_insns[i - 1].ops[0].size == 8;
        return x87_fist(b, insn, courier, exit_sites, n_exits);
    }
    case OCERZ_OP_FLDZ: case OCERZ_OP_FLD1: case OCERZ_OP_FLDPI: case OCERZ_OP_FLDL2E:
    case OCERZ_OP_FLDL2T: case OCERZ_OP_FLDLG2: case OCERZ_OP_FLDLN2: {
        uint64_t mant; unsigned se;
        switch (op) {
        case OCERZ_OP_FLDZ: mant = 0; se = 0; break;
        case OCERZ_OP_FLD1: mant = 1ull << 63; se = 0x3fff; break;
        case OCERZ_OP_FLDPI: mant = 0xc90fdaa22168c235ull; se = 0x4000; break;
        case OCERZ_OP_FLDL2E: mant = 0xb8aa3b295c17f0bcull; se = 0x3fff; break;
        case OCERZ_OP_FLDL2T: mant = 0xd49a784bcd1b8afeull; se = 0x4000; break;
        case OCERZ_OP_FLDLG2: mant = 0x9a209a84fbcff799ull; se = 0x3ffd; break;
        default: mant = 0xb17217f7d1cf79acull; se = 0x3ffe; break;
        }
        a64_mov_imm64(b, JTT, ocerz_x87_f80_dbits(mant, se));
        a64_fmov_v_from_x(b, 1, VX0, JTT);
        a64_mov_imm64(b, JTT, mant);
        a64_movz(b, JTU, (uint16_t)se, 0);
        x87_ld(b);
        x87_push(b, VX0, 1, JTT, JTU);
        return 1;
    }
    case OCERZ_OP_FADD: case OCERZ_OP_FADDP: case OCERZ_OP_FIADD:
    case OCERZ_OP_FSUB: case OCERZ_OP_FSUBP: case OCERZ_OP_FISUB:
    case OCERZ_OP_FSUBR: case OCERZ_OP_FSUBRP: case OCERZ_OP_FISUBR:
    case OCERZ_OP_FMUL: case OCERZ_OP_FMULP: case OCERZ_OP_FIMUL:
    case OCERZ_OP_FDIV: case OCERZ_OP_FDIVP: case OCERZ_OP_FIDIV:
    case OCERZ_OP_FDIVR: case OCERZ_OP_FDIVRP: case OCERZ_OP_FIDIVR:
        return x87_arith(b, insn, exit_sites, n_exits);
    case OCERZ_OP_FSQRT: case OCERZ_OP_FRNDINT:
        x87_ld(b);
        x87_top(b, X87P);
        x87_slot(b, X87Q, X87P);
        x87_lane_read(b, VX0, x87_rel(0), X87Q);
        if (op == OCERZ_OP_FSQRT) {
            a64_fsqrt_s(b, 1, VX2, VX0);
            x87_result(b, XK_SQRT);
        } else {
            a64_frint_s(b, 1, 0, VX2, VX0);
            a64_fcmp(b, 1, VX2, VX0);
            x87_slow_if(b, A64_VS);
            x87_frag_if(b, A64_NE, XF_SETPE, 0);
            x87_frag_land(b);
        }
        a64_str_v(b, 8, VX2, X87Q, X87_FPR_OFF);
        x87_lane_put(b, x87_rel(0), VX2);
        x87_tag(b, X87P, JT0, x87_rel(0), 0);
        x87_st(b);
        return 1;
    case OCERZ_OP_FCHS: case OCERZ_OP_FABS:
        x87_ld(b);
        x87_top(b, X87P);
        x87_slot(b, X87Q, X87P);
        a64_ldr(b, 8, JTT, X87Q, X87_FPR_OFF);
        if (op == OCERZ_OP_FCHS) (void)a64_try_eor_imm(b, 1, JTT, JTT, 1ull << 63);
        else                     (void)a64_try_and_imm(b, 1, JTT, JTT, ~(1ull << 63));
        a64_str(b, 8, JTT, X87Q, X87_FPR_OFF);
        if (x87_lane_of(x87_rel(0)) >= 0) {
            a64_fmov_v_from_x(b, 1, VX0, JTT);
            x87_lane_put(b, x87_rel(0), VX0);
        }
        x87_tag(b, X87P, JT0, x87_rel(0), 0);
        x87_clear_c1(b);
        x87_st(b);
        return 1;
    case OCERZ_OP_FXCH:
        x87_ld(b);
        if (o->reg) {
            x87_top(b, X87P);
            x87_phys(b, JT0, o->reg);
            x87_slot(b, X87Q, X87P);
            x87_slot(b, JTT, JT0);
            int xl0 = x87_lane_get(b, x87_rel(0)), xl1 = x87_lane_get(b, x87_rel(o->reg));
            if (xl0 >= 0 && xl1 >= 0) {

                a64_str_v(b, 8, xl1, X87Q, X87_FPR_OFF);
                a64_str_v(b, 8, xl0, JTT, X87_FPR_OFF);
                int p0 = x87_rel(0), p1 = x87_rel(o->reg);
                g_x87_lane[p0] = (int8_t)xl1;
                g_x87_lane[p1] = (int8_t)xl0;
            } else {
                a64_ldr_v(b, 8, VX0, X87Q, X87_FPR_OFF);
                a64_ldr_v(b, 8, VX1, JTT, X87_FPR_OFF);
                a64_str_v(b, 8, VX1, X87Q, X87_FPR_OFF);
                a64_str_v(b, 8, VX0, JTT, X87_FPR_OFF);
            }





            int ra = x87_rel(0), rc = x87_rel(o->reg);
            int xa = g_x87_live ? g_x87_xokk[ra] : 0, xc = g_x87_live ? g_x87_xokk[rc] : 0;
            int known = xa && xc, any_img = xa == 1 || xc == 1;
            uint32_t *noimg = NULL;
            if (!known) {
                a64_lsrv(b, 1, X87Q, X87S, X87P);
                a64_lsrv(b, 1, JTT, X87S, JT0);
                a64_orr_reg(b, 1, X87Q, X87Q, JTT, 0);
                noimg = a64_label(b);
                a64_tbz(b, X87Q, XS_XOK, 0);
            }
            if (!known || any_img) {
                x87_slot(b, X87Q, X87P);
                x87_slot(b, JTT, JT0);
                a64_ldr_v(b, 8, VX2, X87Q, X87_XM_OFF);
                a64_ldr_v(b, 8, VX3, JTT, X87_XM_OFF);
                a64_str_v(b, 8, VX3, X87Q, X87_XM_OFF);
                a64_str_v(b, 8, VX2, JTT, X87_XM_OFF);
                a64_add_reg(b, 1, X87Q, 20, X87P, 1);
                a64_add_reg(b, 1, JTT, 20, JT0, 1);
                a64_ldr(b, 2, JTU, X87Q, X87_XE_OFF);
                a64_ldr(b, 2, X87P, JTT, X87_XE_OFF);
                a64_str(b, 2, X87P, X87Q, X87_XE_OFF);
                a64_str(b, 2, JTU, JTT, X87_XE_OFF);
                x87_top(b, X87P);
            }
            if (noimg) a64_patch_tbz(noimg, a64_label(b));
            if (!known) {
                a64_lsrv(b, 1, JTU, X87S, X87P);
                a64_lsrv(b, 1, JTT, X87S, JT0);
                a64_eor_reg(b, 1, JTU, JTU, JTT, 0);
                a64_ubfx(b, 1, JTU, JTU, XS_XOK, 1);
                a64_lslv(b, 0, JTT, JTU, X87P);
                a64_lslv(b, 0, JTU, JTU, JT0);
                a64_orr_reg(b, 0, JTU, JTU, JTT, 0);
                a64_eor_reg(b, 1, X87S, X87S, JTU, XS_XOK);
            } else if (xa != xc) {
                x87_bit(b, JTT, X87P);
                x87_bit(b, JTU, JT0);
                a64_orr_reg(b, 0, JTU, JTU, JTT, 0);
                a64_eor_reg(b, 1, X87S, X87S, JTU, XS_XOK);
            }
            int ta = !g_x87_live || g_x87_tagk[ra] != 1, tc = !g_x87_live || g_x87_tagk[rc] != 1;
            if (ta) {
                x87_bit(b, JTT, X87P);
                a64_orr_reg(b, 1, X87S, X87S, JTT, 32);
            }
            if (tc) {
                x87_bit(b, JTU, JT0);
                a64_orr_reg(b, 1, X87S, X87S, JTU, 32);
            }
            if (g_x87_live) {
                g_x87_xokk[ra] = (int8_t)xc;
                g_x87_xokk[rc] = (int8_t)xa;
                g_x87_tagk[ra] = g_x87_tagk[rc] = 1;
            }
        }
        x87_clear_c1(b);
        x87_st(b);
        return 1;
    case OCERZ_OP_FCOM: case OCERZ_OP_FCOMP: case OCERZ_OP_FCOMPP:
    case OCERZ_OP_FUCOM: case OCERZ_OP_FUCOMP: case OCERZ_OP_FUCOMPP:
    case OCERZ_OP_FICOM: case OCERZ_OP_FICOMP: case OCERZ_OP_FTST:
    case OCERZ_OP_FCOMI: case OCERZ_OP_FCOMIP: case OCERZ_OP_FUCOMI: case OCERZ_OP_FUCOMIP:
        return x87_compare(b, insn, need, exit_sites, n_exits);
    case OCERZ_OP_FCMOVCC: {
        int i = insn->ops[1].reg;
        if (i == 0) return 1;
        uint32_t *skip = NULL;
        (void)x87_lane_get(b, x87_rel(0));
        (void)x87_lane_get(b, x87_rel(i));
        if (g_x87_nzcv_live) {
            int cond = x87_fcmov_cond(insn->cc);





            if (cond == A64_NV) {
                if (g_x87_live) g_x87_tagk[x87_rel(0)] = g_x87_xokk[x87_rel(0)] = 0;
                return 1;
            }
            if (cond == A64_AL) g_x87_fcmov_static = 1;
            if (cond != A64_AL) {
                skip = a64_label(b);
                a64_bcond(b, A64_INV(cond), 0);
            }
        } else {
            emit_cc_predicate(b, insn->cc);
            a64_cset(b, X87Q, A64_NE);
            x87_reload(b);
            skip = a64_label(b);
            a64_cbz(b, 0, X87Q, 0);
        }
        x87_ld(b);
        x87_top(b, X87P);
        x87_phys(b, JT0, i);
        int8_t tk[8], xk[8];
        memcpy(tk, g_x87_tagk, sizeof tk);
        memcpy(xk, g_x87_xokk, sizeof xk);
        x87_copy(b, X87P, JT0, x87_rel(0), x87_rel(i));
        if (skip) {
            for (int k = 0; k < 8; k++) {
                if (tk[k] != g_x87_tagk[k]) g_x87_tagk[k] = 0;
                if (xk[k] != g_x87_xokk[k]) g_x87_xokk[k] = 0;
            }
        }
        x87_st(b);
        if (skip && g_x87_nzcv_live) a64_patch_bcond(skip, a64_label(b));
        else if (skip)               a64_patch_cbz(skip, a64_label(b));
        if (g_x87_fcmov_static && g_x87_live) g_x87_tagk[x87_rel(0)] = g_x87_xokk[x87_rel(0)] = 0;
        g_x87_fcmov_static = 0;
        return 1;
    }
    case OCERZ_OP_FNSTSW: {
        x87_ld(b);
        a64_ubfx(b, 1, X87Q, X87S, 16, 16);
        x87_top(b, JTT);
        a64_bfi(b, 0, X87Q, JTT, 11, 3);
        if (o->kind == OCERZ_OPK_MEM)
            return x87_st_mem(b, insn, 0, X87Q, exit_sites, n_exits);
        int s = pin_slot(OCERZ_RAX);
        if (s >= 0) {
            a64_bfi(b, 1, pin_hreg(s), X87Q, 0, 16);
        } else {
            emit_gpr_rd(b, 1, JT0, OCERZ_RAX);
            a64_bfi(b, 1, JT0, X87Q, 0, 16);
            emit_gpr_wr(b, JT0, OCERZ_RAX);
            x87_reload(b);
        }
        return 1;
    }
    case OCERZ_OP_FNSTCW:
        x87_ld(b);
        return x87_st_mem(b, insn, 0, X87S, exit_sites, n_exits);
    case OCERZ_OP_FLDCW:
        if (!x87_ld_int(b, insn, X87Q, 0, exit_sites, n_exits)) return 0;
        (void)a64_try_orr_imm(b, 0, X87Q, X87Q, 0x40);
        x87_ld(b);
        a64_bfi(b, 1, X87S, X87Q, 0, 16);
        x87_st(b);
        return 1;
    case OCERZ_OP_FNINIT:
        x87_ld(b);
        (void)a64_try_and_imm(b, 1, X87S, X87S, 0xffff000000000000ull);
        a64_movz(b, X87Q, 0x037f, 0);
        a64_orr_reg(b, 1, X87S, X87S, X87Q, 0);
        x87_st(b);

        x87_know_reset();
        if (g_x87_live && g_x87_spec >= 0) {
            g_x87_delta = -g_x87_spec;
            memset(g_x87_tagk, 2, sizeof g_x87_tagk);
        }
        return 1;
    case OCERZ_OP_FNCLEX:
        x87_ld(b);
        (void)a64_try_and_imm(b, 1, X87S, X87S, ~(0xffull << 16));
        (void)a64_try_and_imm(b, 1, X87S, X87S, ~(1ull << 31));
        x87_st(b);
        return 1;
    case OCERZ_OP_FWAIT:
        return 1;
    case OCERZ_OP_FFREE: case OCERZ_OP_FFREEP:
        x87_ld(b);
        x87_phys(b, X87P, o->reg);
        if (!g_x87_live || g_x87_tagk[x87_rel(o->reg)] != 2) {
            x87_bit(b, JT0, X87P);
            a64_bic_reg(b, 1, X87S, X87S, JT0, 32);
        }
        if (g_x87_live) g_x87_tagk[x87_rel(o->reg)] = 2;
        if (op == OCERZ_OP_FFREEP) x87_pop(b);
        x87_st(b);
        return 1;
    case OCERZ_OP_FINCSTP: case OCERZ_OP_FDECSTP:
        x87_ld(b);
        if (op == OCERZ_OP_FINCSTP) x87_phys(b, X87P, 1);
        else                        x87_newtop(b, X87P);
        a64_bfi(b, 1, X87S, X87P, 40, 8);
        g_x87_delta += op == OCERZ_OP_FINCSTP ? 1 : -1;
        x87_clear_c1(b);
        x87_st(b);
        return 1;
    default:
        return 0;
    }
}

static int x87_run_open(A64Buf *b, int idx)
{
    if (g_n_x87_run >= X87_RUN_MAX || g_n_x87_site + 2 > X87_SITE_MAX || g_n_x87_frag + 1 > X87_FRAG_MAX)
        return 0;
    if (!g_cur_blk || g_cur_blk->insns != g_cur_insns || !g_keep || idx >= g_keep_n)
        return 0;
    int last = idx, need = 0;
    for (int j = idx; j < g_cur_insns_n; j++) {
        int f = x87_run_flags(&g_cur_insns[j]);
        if (!f) break;
        need |= f;
        last = j;
        if (f & X87R_END) break;
    }
    if (g_scpend.valid) scalar_pend_flush(b);
    X87Run *r = &g_x87_run[g_n_x87_run];
    r->first = (int16_t)idx;
    r->last = (int16_t)last;
    r->back = NULL;
    for (int k = 0; k < 16; k++) { r->l0[k] = g_l0[k]; r->l0_dbl[k] = g_l0_dbl[k]; }
    r->l0_dirty = g_l0_dirty;
    r->yc_dirty = g_yc_dirty;
    g_x87_cur = g_n_x87_run++;
    g_x87_frag_open = g_n_x87_frag;
    if (need & X87R_MXCSR) {
        a64_ldr(b, 4, JT0, 20, X87_MXCSR_OFF);
        (void)a64_try_ands_imm(b, 0, A64_ZR, JT0, 0x6000);
        x87_slow_if(b, A64_NE);
    }
    static int nolive = -1;
    if (nolive < 0) nolive = ENV_ON("OCERZ_NO_X87_LIVE") ? 1 : 0;
    g_x87_live = 0;
    g_x87_delta = 0;
    g_x87_spec = -1;
    if (nolive) g_x87_lv = 0;
    if (!nolive && g_x87_btop >= 0 && !ENV_ON("OCERZ_NO_X87_TOPSPEC")) {
        a64_ldr(b, 8, X87S, 20, X87_CTL_OFF);
        a64_ubfx(b, 1, JT0, X87S, 40, 3);
        a64_subs_imm(b, 0, A64_ZR, JT0, (uint32_t)g_x87_btop);
        x87_slow_if(b, A64_NE);
        g_x87_spec = g_x87_btop;
        g_x87_live = 1;
    } else if (!nolive) {
        g_x87_lv = 0;
        a64_ldr(b, 8, X87S, 20, X87_CTL_OFF);
        a64_ubfx(b, 1, JT0, X87S, 40, 3);
        a64_ldr(b, 2, JTT, 20, X87_TOP0_OFF);
        a64_subs_reg(b, 0, A64_ZR, JT0, JTT, 0);
        x87_frag_if(b, A64_NE, XF_TOP0, 0);
        x87_frag_land(b);
        g_x87_live = 1;
    }
    if (!(g_x87_spec >= 0 && g_x87_kcarry && !ENV_ON("OCERZ_NO_X87_KCARRY")))
        x87_know_reset();
    g_x87_c1k = 0;
    g_x87_rc_near = (need & X87R_FCW) != 0;
    if (need & X87R_FCW) {
        if (!g_x87_live) a64_ldr(b, 2, JT0, 20, X87_CTL_OFF);
        (void)a64_try_ands_imm(b, 0, A64_ZR, g_x87_live ? X87S : JT0, 0xc00);
        x87_slow_if(b, A64_NE);
    }
    g_x87_st_mark = g_x87_live ? b->p : NULL;
    return 1;
}

int emit_x87(A64Buf *b, const X86Insn *insn, uint64_t need, uint32_t **exit_sites, int *n_exits)
{
    int idx = g_cur_insn_idx;
    if (!x87_inline_ok(insn) || !g_cur_insns || idx < 0 || idx >= g_cur_insns_n || insn != &g_cur_insns[idx])
        return 0;
    if (g_x87_cur < 0 || idx < g_x87_run[g_x87_cur].first || idx > g_x87_run[g_x87_cur].last) {
        g_x87_cur = -1;
        g_x87_live = 0;
        if (!x87_run_open(b, idx))
            return 0;
    }
    X87Run *r = &g_x87_run[g_x87_cur];
    g_x87_nzcv_live = g_x87_nzcv == idx - 1 && idx > r->first;
    g_x87_nzcv = -1;
    int ok = g_n_x87_site + 16 <= X87_SITE_MAX && g_n_x87_frag + 4 <= X87_FRAG_MAX &&
             emit_x87_one(b, insn, need, exit_sites, n_exits);
    x87_frag_land(b);
    if (!ok) {
        emit_slowcall(b, insn, exit_sites, n_exits);
        r->last = (int16_t)idx;
        g_x87_lv = 0;
        g_x87_spec_cut = 1;
    }
    if (idx == r->last) {
        r->top_end = (int8_t)(g_x87_live && g_x87_spec >= 0 ? (g_x87_spec + g_x87_delta) & 7 : -1);
        g_x87_kcarry = g_x87_live && g_x87_spec >= 0 && !g_x87_spec_cut;
        g_x87_spec_cut = 0;
        r->lv_end = g_x87_lv;
        memcpy(r->lmap, g_x87_lane, sizeof r->lmap);
        if (g_x87_live && g_x87_spec >= 0) {
            g_x87_btop = (g_x87_spec + g_x87_delta) & 7;
            a64_movz(b, JT0, (uint16_t)g_x87_btop, 0);
            a64_str(b, 2, JT0, 20, X87_TOP0_OFF);
        } else if (g_x87_live && (g_x87_delta & 7)) {
            a64_ldr(b, 2, JT0, 20, X87_TOP0_OFF);
            a64_add_imm(b, 0, JT0, JT0, (uint32_t)(g_x87_delta & 7));
            (void)a64_try_and_imm(b, 0, JT0, JT0, 7);
            a64_str(b, 2, JT0, 20, X87_TOP0_OFF);
        }
        if (g_x87_spec < 0) g_x87_btop = -1;
        g_x87_spec = -1;
        r->back = a64_label(b);
        g_x87_cur = -1;
        g_x87_live = 0;
    }
    return 1;
}

static void x87_frag_exact(A64Buf *b, int op, int run, int idx, int r0, int r1, uint32_t **pe, int *np, uint32_t **ok, int *nok)
{
    switch (op) {
    case XK_ADD:
    case XK_SUB:
        if (op == XK_ADD) a64_fsub_s(b, 1, VX3, VX2, r0);
        else              a64_fsub_s(b, 1, VX3, r0, VX2);
        a64_fcmp(b, 1, VX3, r1);
        pe[(*np)++] = a64_label(b);
        a64_bcond(b, A64_NE, 0);
        if (op == XK_ADD) a64_fsub_s(b, 1, VX3, VX2, r1);
        else              a64_fadd_s(b, 1, VX3, VX2, r1);
        a64_fcmp(b, 1, VX3, r0);
        break;
    case XK_MUL:
        a64_fmadd_s(b, 1, 0, 1, VX3, r0, r1, VX2);
        a64_fcmp_zero(b, 1, VX3);
        break;
    default:
        if (op == XK_SQRT) {
            a64_fcmp_zero(b, 1, r0);
            ok[(*nok)++] = a64_label(b);
            a64_bcond(b, A64_EQ, 0);
        }
        a64_fmov_x_from_v(b, 1, JTT, r0);
        a64_ubfx(b, 1, JTT, JTT, 52, 11);
        a64_subs_imm(b, 0, A64_ZR, JTT, op == XK_SQRT ? 54 : 63);
        x87_slow_at(b, A64_CC, run, idx);
        if (op == XK_SQRT) a64_fmadd_s(b, 1, 1, 0, VX3, VX2, VX2, r0);
        else               a64_fmadd_s(b, 1, 1, 0, VX3, VX2, r1, r0);
        a64_fcmp_zero(b, 1, VX3);
        break;
    }
    pe[(*np)++] = a64_label(b);
    a64_bcond(b, A64_NE, 0);
}

static void x87_emit_frag(A64Buf *b, int f)
{
    int run = g_x87_frag[f].run, idx = g_x87_frag[f].idx, op = g_x87_frag[f].op;
    int r0 = g_x87_frag[f].r0, r1 = g_x87_frag[f].r1;
    uint32_t *back = g_x87_frag[f].back;
    uint32_t *pe[4], *ok[4];
    int np = 0, nok = 0;
    a64_patch_bcond(g_x87_frag[f].site, a64_label(b));
    switch (g_x87_frag[f].kind) {
    case XF_PE:
        x87_frag_exact(b, op, run, idx, r0, r1, pe, &np, ok, &nok);
        for (int k = 0; k < nok; k++) a64_patch_bcond(ok[k], a64_label(b));
        a64_b(b, (int32_t)(back - a64_label(b)));
        for (int k = 0; k < np; k++) a64_patch_bcond(pe[k], a64_label(b));

        (void)a64_try_orr_imm(b, 1, X87S, X87S, XS_PE);
        a64_str(b, 8, X87S, 20, X87_CTL_OFF);
        break;
    case XF_PC24: {
        a64_ubfx(b, 1, JTT, JT0, 52, 11);
        uint32_t *zero = NULL;
        if (op != XK_MUL && op != XK_DIV) {
            a64_lsl_imm(b, 1, JTU, JT0, 1);
            zero = a64_label(b);
            a64_cbz(b, 1, JTU, 0);
        }
        a64_sub_imm(b, 0, JTU, JTT, 1);
        a64_subs_imm(b, 0, A64_ZR, JTU, 0x7fd - 1);
        x87_slow_at(b, A64_HI, run, idx);
        (void)a64_try_ands_imm(b, 1, A64_ZR, X87S, XS_PE);
        uint32_t *known = a64_label(b);
        a64_bcond(b, A64_NE, 0);
        (void)a64_try_and_imm(b, 1, JTU, JT0, 0x1fffffffull);
        uint32_t *cut = a64_label(b);
        a64_cbnz(b, 1, JTU, 0);
        x87_frag_exact(b, op, run, idx, r0, r1, pe, &np, ok, &nok);
        uint32_t *exact = a64_label(b);
        a64_b(b, 0);
        a64_patch_cbz(cut, a64_label(b));
        for (int k = 0; k < np; k++) a64_patch_bcond(pe[k], a64_label(b));
        (void)a64_try_orr_imm(b, 1, X87S, X87S, XS_PE);
        a64_str(b, 8, X87S, 20, X87_CTL_OFF);
        a64_patch_bcond(known, a64_label(b));
        a64_patch_b(exact, a64_label(b));
        for (int k = 0; k < nok; k++) a64_patch_bcond(ok[k], a64_label(b));
        a64_ubfx(b, 1, JTT, JT0, 29, 1);
        a64_add_reg(b, 1, JT0, JT0, JTT, 0);
        (void)a64_try_orr_imm(b, 1, JTT, A64_ZR, 0x0fffffffull);
        a64_add_reg(b, 1, JT0, JT0, JTT, 0);
        (void)a64_try_and_imm(b, 1, JT0, JT0, ~0x1fffffffull);
        a64_fmov_v_from_x(b, 1, VX2, JT0);
        if (zero) a64_patch_cbz(zero, a64_label(b));
        break;
    }
    case XF_ZERO:
        a64_lsl_imm(b, 1, JTT, JT0, 1);
        a64_subs_imm(b, 1, A64_ZR, JTT, 0);
        x87_slow_at(b, A64_NE, run, idx);
        a64_fcmp_zero(b, 1, r0);
        a64_bcond(b, A64_EQ, (int32_t)(back - a64_label(b)));
        if (op == XK_MUL) {
            a64_fcmp_zero(b, 1, r1);
        } else {
            a64_fmov_x_from_v(b, 1, JTT, r1);
            a64_lsl_imm(b, 1, JTT, JTT, 1);
            a64_movz(b, JTU, 0xffe0, 3);
            a64_subs_reg(b, 1, A64_ZR, JTT, JTU, 0);
        }
        x87_slow_at(b, A64_NE, run, idx);
        break;
    case XF_TOP0:
        a64_str(b, 2, JT0, 20, X87_TOP0_OFF);
        break;
    case XF_ST32:
        a64_fcmp(b, 1, VX0, VX0);
        x87_slow_at(b, A64_VS, run, idx);
        a64_fmov_x_from_v(b, 0, JTT, VX1);
        a64_ubfx(b, 0, JTT, JTT, 23, 8);
        a64_sub_imm(b, 0, JTT, JTT, 2);
        a64_subs_imm(b, 0, A64_ZR, JTT, 0xfe - 2);
        x87_slow_at(b, A64_HI, run, idx);

    default:
        (void)a64_try_orr_imm(b, 1, X87S, X87S, XS_PE);
        x87_st(b);
        break;
    }
    a64_b(b, (int32_t)(back - a64_label(b)));
}

void emit_x87_arms(A64Buf *b, uint32_t **exit_sites, int *n_exits, uint32_t **epi_sites, int *n_epi)
{
    for (int f = 0; f < g_n_x87_frag; f++)
        x87_emit_frag(b, f);
    int save_idx = g_cur_insn_idx;
    for (int r = 0; r < g_n_x87_run; r++) {
        X87Run *run = &g_x87_run[r];
        uint32_t *to_common[JIT_MAX_BLOCK_INSNS];
        int nc = 0;
        for (int k = run->first; k <= run->last; k++) {
            uint32_t *entry = NULL;
            for (int s = 0; s < g_n_x87_site; s++) {
                if (g_x87_site[s].run != r || g_x87_site[s].idx != k) continue;
                if (!entry) {
                    entry = a64_label(b);
                    a64_movz(b, JT0, (uint16_t)k, 0);
                    a64_str(b, 8, JT0, 20, JIT_SCRATCH_OFF);
                    to_common[nc++] = a64_label(b);
                    a64_b(b, 0);
                }
                a64_patch_bcond(g_x87_site[s].site, entry);
            }
        }
        if (!nc) continue;
        for (int c = 0; c < nc; c++) a64_patch_b(to_common[c], a64_label(b));
        g_cur_insn_idx = run->first;
        emit_l0_flush_from(b, run->l0, run->l0_dbl, run->l0_dirty);
        yc_flush_from(b, run->yc_dirty);
        g_slow_run_last = run->last;
        emit_slowcall(b, &g_cur_insns[run->first], exit_sites, n_exits);
        g_slow_run_last = -1;
        if (run->top_end >= 0) {





            a64_ldr(b, 1, JT0, 20, X87_CTL_OFF + 5);
            a64_subs_imm(b, 0, A64_ZR, JT0, (uint32_t)run->top_end);
            uint32_t *same = a64_label(b);
            a64_bcond(b, A64_EQ, 0);
            const X86Insn *li = &g_cur_insns[run->last];
            tc_imm64(b, JT0, TCR_BLK, 0, (uint64_t)(uintptr_t)g_cur_blk);
            a64_str(b, 8, JT0, 20, SIDE_BLK_OFF);
            a64_movn(b, JT0, 2, 0);
            a64_str(b, 4, JT0, 20, SIDE_IDX_OFF);
            a64_mov_imm64(b, JT0, (li->rip + li->len) & (li->mode32 ? 0xffffffffull : ~0ull));
            a64_str(b, 8, JT0, 20, RIP_OFF);
            a64_mov_imm64(b, 0, OCERZ_STEP_PROFILE);
            epi_sites[(*n_epi)++] = a64_label(b);
            a64_b(b, 0);
            a64_patch_bcond(same, a64_label(b));
        }
        emit_l0_reload_from(b, run->l0, run->l0_dbl);
        for (int p = 0; p < 8; p++)
            if (run->back && (run->lv_end >> p & 1) && run->lmap[p] >= 0)
                a64_ldr_v(b, 8, run->lmap[p], 20, X87_FPR_OFF + 8u * (unsigned)p);
        if (run->back) {
            a64_b(b, (int32_t)(run->back - a64_label(b)));
        } else {
            const X86Insn *li = &g_cur_insns[run->last];
            a64_mov_imm64(b, JT0, (li->rip + li->len) & (li->mode32 ? 0xffffffffull : ~0ull));
            a64_str(b, 8, JT0, 20, RIP_OFF);
            a64_mov_imm64(b, 0, OCERZ_STEP_OK);
            epi_sites[(*n_epi)++] = a64_label(b);
            a64_b(b, 0);
        }
    }
    g_cur_insn_idx = save_idx;
    x87_reset();
}

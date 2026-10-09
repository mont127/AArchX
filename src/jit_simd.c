/*
 * ---- SSE and FP ----
 * x86's NaN rule (result NaN -> quiet(a) if a is NaN, else quiet(b), else the
 * default NaN) differs from arm64's, so the hot path branches out of line to an
 * exact fixup arm emitted after the body.  FP batches take that further: a run
 * of arithmetic is checked once at its end rather than per instruction, with
 * the registers it overwrites checkpointed and the whole run replayed exactly
 * if the check fires.  A double batch's taint can ride past replayable
 * instructions to a ucomisd/comisd, which raises V for a NaN in either
 * operand's lane 0 and so detects for free; lanes still unverified at a
 * superblock side exit are checked in that exit's stub, off the hot path.
 * Scalar chains additionally keep lane 0 in a scratch V register (fixed-lane
 * mode gives four scratches to a self-looping block for its whole body) so a
 * loop-carried scalar chain never round-trips through the architectural
 * register.  A generated NaN kept arm64's sign until the exact arm was given an
 * off-chain copy of the operand the result overwrites (found by fp_loop_nan).
 *
 * All of that is what a processor without FPCR.AH needs.  With the bit set
 * (ocerz_afp, src/cpu.c) arm64's add, subtract, multiply, divide and square
 * root give x86's NaN results themselves as long as the x86 destination is the
 * first arm64 operand, which is how they were already emitted, so they go out
 * bare, and a batch made only of those, moves, shuffles, compares and stores
 * keeps its members' fast emission and drops its checkpoint, its checks, its
 * undo log and its replay arms.  In a packed loop the end-of-batch check was
 * three vector operations on top of the eight that did the work, and the four
 * vector pipelines were what bounded it: fpvec went from 1.06x of Rosetta's
 * time to 0.82x.  min and max were exact already, being a compare and a
 * select.  Fused multiply-add is not covered: its negated forms negate a NaN
 * operand where x86 returns it as it came, so a batch holding one keeps the
 * whole apparatus, and outside a batch it keeps its own check.
 *
 * An emitter that takes a memory operand reaches it through emit_mem_load_any
 * or emit_sse_mem_addr, never through the plain helpers alone.  The plain forms
 * need guest and host addresses to coincide, which the low shadow window of
 * the Wine mode breaks, so a plain-only emitter hands every memory form to the
 * interpreter there and nowhere else: Steam's webhelper interpreted pinsrw from
 * memory for 15% of its samples, and the same rule held back bsf/bsr, the wide
 * and three-operand multiplies, pmovzx from a word, pextr to memory and the
 * BMI sources.  gs- and fs-relative operands are formed in that mode too, with
 * Wine's gs:[0x58] redirect reading the TEB pointer at gs:[0x30] through the
 * same translation (OCERZ_NO_LOW_SEG restores the interpreter for them).
 *
 * That translation sits in front of every guest memory access in the Wine
 * mode, and in its general form it builds three 64-bit constants and tests
 * three ranges - the low window, the identity middle, the top strip with the
 * commpage in it - about twenty instructions for one load.  When the low
 * window's host base is a single run of bits above every low address, as the
 * usual 0x8000000000 is, an address below 12 GB becomes host with one orr
 * after a shift and a compare, and one between 12 GB and the top strip is
 * recognised as identity with two shifts and an add; only the top strip
 * takes the general form.  In the Wine layout that took memcpy from 6.1x of
 * Rosetta's time to 3.1x, a mixed workload from 3.0x to 1.65x and an
 * interpreter loop from 1.5x to 1.0x (OCERZ_NO_FAST_LOW_GUARD=1 keeps the
 * general form everywhere).  The top strip is not tested at all until an
 * access in the block faults there: arm64 cannot map anything at or above
 * TOP_LO, so its identity address faults, and the handler marks the block the
 * way it marks a commpage reader in identity mode, interprets the one
 * instruction and retires the block.  The retranslation then tests the
 * identity range with two shifts and an add and takes the general form out of
 * line at the end of the block.  A low address costs a shift, a compare, an
 * untaken branch and an orr, and an identity address the first three
 * (OCERZ_LOW_TOP_GUARD=1 tests the top strip in every block).  Stack accesses (push, pop, call, ret and rsp-relative operands) are
 * plain in this mode as in every other, after the translation instead of in
 * place of it; they used to take the ordered load and store
 * (OCERZ_TSO_STRICT=1 orders them everywhere).  Their translation is the stack
 * delta kept in x0 rather than the guard (emit_stack_delta), and a rip-relative
 * address, whose side of 12 GB is known when the block is translated, takes an
 * orr below it and nothing above it.  On xbench in the Wine layout these took
 * leafcall from 1.27 s to 0.65 s (Rosetta 0.62 s) and str from 0.84 s to 0.75 s.
 * A 32-bit address is below 4 GB, so in 32-bit code every translation is the
 * orr alone, and esp-relative operands there are stack accesses as well: a
 * WoW64 loop of virtual calls into small frames spent most of its time in the
 * acquire loads and release stores of its stack slots, and went from 459 to
 * 163 ms (Rosetta 158).
 *
 * The integer SSE forms map almost one to one: widening multiplies and a
 * narrowing unzip for the high halves and pmaddubsw, saturating narrows for the
 * packs, uabd with three pairwise widening adds for psadbw, addp or an unzip
 * pair for the horizontal ops, ext against a zero register for the byte shifts
 * and palignr, and lane inserts for the immediate blends and pshuflw/hw.  A
 * shift by an xmm or m128 count clamps the 64-bit count to 64 and shifts by a
 * duplicated register, which reproduces x86's saturation: zero for the logical
 * shifts, the sign for the arithmetic ones.  cvtps2dq rounds with frinti, which
 * follows the FPCR mode that tracks MXCSR.RC, and then shares cvttps2dq's
 * conversion, whose lanes that are NaN or not below 2^31 take x86's
 * 0x80000000 where arm64 would saturate or give zero.  cvtps2pd and cvtpd2ps
 * are fcvtl and fcvtn, which quiet a signalling NaN and keep its payload and
 * round by the FPCR mode as x86 does; fcvtn also clears the upper half.  AES rounds use aese or
 * aesd against a zero key, then aesmc or aesimc, then the round key, because
 * arm64 adds the key before the substitution and x86 after it;
 * aeskeygenassist picks its words out of a zero-key aese with a table lookup.
 * crc32 is the arm64 crc32c of the same width, pclmulqdq is pmull.
 *
 * MMX instructions are translated too (emit_mmx): the eight registers live in
 * the cpu's mmx array, each instruction loads what it reads into a scratch
 * vector register with a d-sized load, which clears the upper half, runs the
 * 128-bit form of the operation and stores the low 64 bits back, so lanewise
 * operations need nothing more.  The others are arranged to land in the low
 * half: a high unpack is a low zip followed by the upper half, the packs join
 * both operands into one register before narrowing, pmulhw and pmulhuw keep
 * the odd halves of a widening multiply, and psadbw is uabd with three
 * pairwise widening adds as in SSE.  Each one leaves the x87 tag word full and
 * TOP at zero, as the interpreter does.  UnityPlayer's video decoder is written
 * in MMX, and as one slow-path call per instruction it was a few percent of
 * R.E.P.O.'s process; a loop of its moves and multiplies runs 9.8 times faster
 * translated (Rosetta is 2.7 times faster again, since every instruction here
 * goes through memory).  OCERZ_NO_JIT_MMX=1 interprets them again.
 *
 *
 * A memory operand is loaded here whatever the flags' fate, so a fault stays
 * this instruction's, and kept in jit_fcmp_mem: a jcc, setcc or cmovcc that
 * comis_fuse_producer pairs with it compares the pinned register against
 * that copy instead of reading the flags back out of RFLAGS.
 *
 * shufps of a register with itself: a broadcast is one dup, a change to one lane one ins.
 *
 * The two-source selections a transpose is made of, each one instruction into the destination.
 */
#include "ocerz/jit_internal.h"

static void emit_xmm_ld(A64Buf *b, int vd, unsigned xr);
static void emit_xmm_st(A64Buf *b, int vs, unsigned xr);
static void emit_xmm_ld_lo(A64Buf *b, int size, int vd, unsigned xr);
static void emit_xmm_st_lo(A64Buf *b, int size, int vs, unsigned xr);
static int emit_sse_src(A64Buf *b, const X86Insn *insn, const X86Operand *o, int size,
                        int vd, uint32_t **exit_sites, int *n_exits);
static int emit_sse_src_reg(A64Buf *b, const X86Insn *insn, const X86Operand *o, int size,
                            int vtmp, uint32_t **exit_sites, int *n_exits);
static inline int xmm_dst_reg(unsigned xr, int vtmp);
static int emit_sse_mov128(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_movlh(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_movs(A64Buf *b, const X86Insn *insn, int size, uint32_t **exit_sites, int *n_exits);
static int emit_sse_fparith(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int sse_int_kind(unsigned op, int *esz);
static int sse_int_self_zero(int kind);
static void emit_sse_int_op(A64Buf *b, int kind, int esz, int vd, int va, int vb);
static int emit_sse_bitwise(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_pblendw_palignr(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_pshuflhw(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_bytesh(A64Buf *b, const X86Insn *insn);
static int emit_sse_blendp(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_movsdup(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_movmskp(A64Buf *b, const X86Insn *insn);
static int emit_sse_cmpp(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_cvtp(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static void emit_v_literal(A64Buf *b, int vt, uint64_t lo, uint64_t hi);
static int emit_sse_aes(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_pclmul(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_comis(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_cvt(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_movd(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_movq(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_pshufd(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_pshufb(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_punpck(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_unpck(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static void emit_cmps_pred(A64Buf *b, int dbl, unsigned pred, int vr, int va, int vb);
static int emit_sse_cmps(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_blendv(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static void emit_simd_shift_imm(A64Buf *b, int kind, int esz, int vd, int vn, unsigned cnt);
static int emit_sse_shift_imm(A64Buf *b, const X86Insn *insn);
static void emit_shufp_lane(A64Buf *b, int dbl, unsigned imm, int vd, int va, int vb);
static int emit_sse_shufp(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_movddup(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_insertps_to(A64Buf *b, const X86Insn *insn, int va, uint32_t **exit_sites, int *n_exits);
static int emit_sse_insertps(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int mmx_ld(A64Buf *b, const X86Insn *insn, const X86Operand *s, int size, int vt,
                  uint32_t **exit_sites, int *n_exits);
static int mmx_st(A64Buf *b, const X86Insn *insn, const X86Operand *d, int size, int vs,
                  uint32_t **exit_sites, int *n_exits);
static void mmx_enter(A64Buf *b);
static int mmx_mov(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_pinsr_pextr(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_pmovx(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_round(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static void emit_mskb_bits(A64Buf *b, int vt);
static int vex_inline_enabled(void);
static int ymmh_src(A64Buf *b, unsigned xr, int vtmp);
static inline int ymmh_dst(unsigned xr, int vtmp);
static void emit_ymmh_ld(A64Buf *b, int vd, unsigned xr);
static void emit_ymmh_st(A64Buf *b, int vs, unsigned xr);
static int emit_vex_mem_addr(A64Buf *b, const X86Insn *insn, const X86Operand *m,
                             uint32_t **exit_sites, int *n_exits);
static void emit_vex_mem_acc(A64Buf *b, int v, uint32_t off, int store);
static void emit_vex_mem_done(A64Buf *b);
static int emit_vex_ld128(A64Buf *b, const X86Insn *insn, const X86Operand *m, int size, int vd,
                          uint32_t **exit_sites, int *n_exits);
static int emit_vex_mov(A64Buf *b, const X86Insn *insn, int L, uint32_t **exit_sites, int *n_exits);
static int emit_vex_int(A64Buf *b, const X86Insn *insn, int kind, int esz, int L,
                        uint32_t **exit_sites, int *n_exits);
static int emit_vex_pmovmskb(A64Buf *b, const X86Insn *insn, int L);
static int emit_vex_broadcast(A64Buf *b, const X86Insn *insn, int esz, int L,
                              uint32_t **exit_sites, int *n_exits);
static int emit_vex_pmovx(A64Buf *b, const X86Insn *insn, int sgn, int from, int L,
                          uint32_t **exit_sites, int *n_exits);
static int emit_vex_shift_imm(A64Buf *b, const X86Insn *insn, int kind, int esz, int L);
static int inexact_nan_env(void);
static int emit_vex_fp_op(A64Buf *b, int kind, int dbl, int vr, int va, int vb);
static void emit_vex_fp_lane(A64Buf *b, int kind, int dbl, int vr, int va, int vb, int t1);
static int emit_vex_cvtdq2ps256(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_vex_fp256(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_vex_fp128_alias(A64Buf *b, const X86Insn *insn);
static int emit_vex_cmps_alias(A64Buf *b, const X86Insn *insn);
static void emit_blend_mask(A64Buf *b, unsigned op, int vd, int vm, int vz);
static int emit_vex_blendv(A64Buf *b, const X86Insn *insn, int L, uint32_t **exit_sites, int *n_exits);
static int emit_vex_cmps_fused(A64Buf *b, const X86Insn *insn);
static int emit_vex_blendv_fused(A64Buf *b, const X86Insn *insn, const X86Insn *c, uint32_t **exit_sites, int *n_exits);
static int bmi_src(A64Buf *b, const X86Insn *insn, const X86Operand *o, int size, int tmp);
static int emit_bmi(A64Buf *b, const X86Insn *insn, uint64_t need);
static int emit_vex_shufp(A64Buf *b, const X86Insn *insn, int L, uint32_t **exit_sites, int *n_exits);
static int emit_vex_insertps(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_vex_movddup256(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_vex_fma(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int vex_sse128_ok(const X86Insn *insn, int L);
static int emit_vex_sse128(A64Buf *b, const X86Insn *insn, int L, uint32_t **exit_sites, int *n_exits);

RasLit g_raslit[RASLIT_MAX];

int g_n_raslit;

int g_vec_int_move;

int g_zero_vreg = -1;

int g_blk_ymm_write;

uint64_t g_cur_need;

uint16_t g_xmm_pinned;

int xmm_pinning_enabled(void)
{
    static int on = -1;
    if (on < 0) on = getenv("OCERZ_NO_XMM_PIN") ? 0 : 1;
    return on;
}

int xmm_global_enabled(void)
{
    static int on = -1;
    if (on < 0) on = (getenv("OCERZ_NO_XMM_GLOBAL") || getenv("OCERZ_NO_FULLPIN")) ? 0 : 1;
    return on;
}

static void emit_xmm_ld(A64Buf *b, int vd, unsigned xr)
{
    l0_flush_reg(b, xr);
    if (xmm_is_pinned(xr)) { if (vd != xmm_vreg(xr)) a64_v_mov(b, vd, xmm_vreg(xr)); return; }
    a64_ldr_v(b, 16, vd, 20, XMM_BASE_OFF + (uint32_t)xr * 16);
}

static void emit_xmm_st(A64Buf *b, int vs, unsigned xr)
{
    if (xmm_is_pinned(xr)) { if (vs != xmm_vreg(xr)) a64_v_mov(b, xmm_vreg(xr), vs); return; }
    a64_str_v(b, 16, vs, 20, XMM_BASE_OFF + (uint32_t)xr * 16);
}

static void emit_xmm_ld_lo(A64Buf *b, int size, int vd, unsigned xr)
{
    l0_flush_reg(b, xr);
    if (xmm_is_pinned(xr)) {
        if (size == 8) a64_fmov_d_d(b, vd, xmm_vreg(xr)); else a64_fmov_s_s(b, vd, xmm_vreg(xr));
        return;
    }
    a64_ldr_v(b, size, vd, 20, XMM_BASE_OFF + (uint32_t)xr * 16);
}

static void emit_xmm_st_lo(A64Buf *b, int size, int vs, unsigned xr)
{
    if (xmm_is_pinned(xr)) {
        if (l0_defer_take(vs, xr, size))
            return;
        if (size == 8) a64_ins_d_d(b, xmm_vreg(xr), 0, vs, 0); else a64_ins_s_s(b, xmm_vreg(xr), 0, vs, 0);
        return;
    }
    a64_str_v(b, size, vs, 20, XMM_BASE_OFF + (uint32_t)xr * 16);
}

int g_sse_mem_ra = JTA;

uint32_t g_sse_mem_disp;

int g_sse_mem_plain;

int g_sse_mem_plainacc;

int emit_sse_mem_addr(A64Buf *b, const X86Insn *insn, const X86Operand *o, int size,
                             uint32_t **exit_sites, int *n_exits, uint32_t **skip_out)
{
    g_sse_mem_plainacc = mem_plain_access_ok(o);
    if (emit_mem_ea_plain_ex(b, insn, o, size, &g_sse_mem_ra, &g_sse_mem_disp, 1)) {
        g_sse_mem_plain = 1;
        *skip_out = NULL;
        return 1;
    }
    g_sse_mem_plain = 0;
    g_sse_mem_ra = JTA; g_sse_mem_disp = 0;
    if (!emit_mem_ea(b, insn, o, JTA))
        return 0;
    *skip_out = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
    emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
    return 1;
}

static int emit_sse_src(A64Buf *b, const X86Insn *insn, const X86Operand *o, int size,
                        int vd, uint32_t **exit_sites, int *n_exits)
{
    if (o->kind == OCERZ_OPK_XMM) {
        emit_xmm_ld(b, vd, o->reg);
        return 1;
    }
    if (o->kind == OCERZ_OPK_MEM) {
        uint32_t *skip;
        if (!emit_sse_mem_addr(b, insn, o, size, exit_sites, n_exits, &skip))
            return 0;
        emit_sse_mem_ld(b, size, vd);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    return 0;
}

static int emit_sse_src_reg(A64Buf *b, const X86Insn *insn, const X86Operand *o, int size,
                            int vtmp, uint32_t **exit_sites, int *n_exits)
{
    if (o->kind == OCERZ_OPK_XMM && xmm_is_pinned(o->reg)) {
        l0_flush_reg(b, o->reg);
        return xmm_vreg(o->reg);
    }
    if (!emit_sse_src(b, insn, o, size, vtmp, exit_sites, n_exits))
        return -1;
    return vtmp;
}

static inline int xmm_dst_reg(unsigned xr, int vtmp)
{
    return xmm_is_pinned(xr) ? xmm_vreg(xr) : vtmp;
}

int sse_enabled(void)
{
    static int on = -1;
    if (on < 0) on = getenv("OCERZ_NO_INLINE_SSE") ? 0 : 1;
    return on;
}

static int emit_sse_mov128(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind == OCERZ_OPK_XMM && s->kind == OCERZ_OPK_XMM) {
        if (d->reg != s->reg) {
            if (xmm_is_pinned(d->reg) && xmm_is_pinned(s->reg)) {
                l0_inval(d->reg);
                a64_v_mov(b, xmm_vreg(d->reg), xmm_vreg(s->reg));
            }
            else { l0_flush_reg(b, s->reg); emit_xmm_ld(b, VX0, s->reg); emit_xmm_st(b, VX0, d->reg); }
            l0_share(d->reg, s->reg);
        }
        return 1;
    }
    if (d->kind == OCERZ_OPK_XMM && s->kind == OCERZ_OPK_MEM) {
        uint32_t *skip;
        l0_inval(d->reg);
        int vd = xmm_is_pinned(d->reg) ? xmm_vreg(d->reg) : VX0;
        if (vd != VX0 && emit_plain_mem_fast(b, insn, s, 16, vd, 0, 1)) return 1;
        if (!emit_sse_mem_addr(b, insn, s, 16, exit_sites, n_exits, &skip)) return 0;
        emit_sse_mem_ld(b, 16, vd);
        patch_guard_skip(skip, a64_label(b));
        if (vd == VX0) emit_xmm_st(b, VX0, d->reg);
        return 1;
    }
    if (d->kind == OCERZ_OPK_MEM && s->kind == OCERZ_OPK_XMM) {
        l0_flush_reg(b, s->reg);
        int vs = xmm_is_pinned(s->reg) ? xmm_vreg(s->reg) : VX0;
        if (vs != VX0 && emit_plain_mem_fast(b, insn, d, 16, vs, 1, 1)) return 1;
        if (vs == VX0) emit_xmm_ld(b, VX0, s->reg);
        uint32_t *skip;
        if (!emit_sse_mem_addr(b, insn, d, 16, exit_sites, n_exits, &skip)) return 0;
        emit_sse_mem_st(b, 16, vs);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    return 0;
}

static int emit_sse_movlh(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    int hi = insn->op == OCERZ_OP_MOVHPS;
    if (d->kind == OCERZ_OPK_XMM && s->kind == OCERZ_OPK_MEM) {
        if (!xmm_is_pinned(d->reg)) return 0;
        uint32_t *skip;
        l0_inval(d->reg);
        if (!emit_sse_mem_addr(b, insn, s, 8, exit_sites, n_exits, &skip)) return 0;
        emit_sse_mem_ld(b, 8, VX0);
        patch_guard_skip(skip, a64_label(b));
        a64_ins_d_d(b, xmm_vreg(d->reg), hi ? 1 : 0, VX0, 0);
        return 1;
    }
    if (d->kind == OCERZ_OPK_MEM && s->kind == OCERZ_OPK_XMM) {
        int vs;
        if (!hi) vs = xmm_is_pinned(s->reg) ? l0_src(s->reg, 1) : VX0;
        else vs = VX0;
        if (vs == VX0) {
            if (xmm_is_pinned(s->reg)) a64_v_dup_d(b, VX0, xmm_vreg(s->reg), hi ? 1 : 0);
            else { emit_xmm_ld(b, VX0, s->reg); if (hi) a64_v_dup_d(b, VX0, VX0, 1); }
        }
        uint32_t *skip;
        if (!emit_sse_mem_addr(b, insn, d, 8, exit_sites, n_exits, &skip)) return 0;
        emit_sse_mem_st(b, 8, vs);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    return 0;
}

static int emit_sse_movs(A64Buf *b, const X86Insn *insn, int size, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    int dbl = size == 8;
    if (d->kind == OCERZ_OPK_XMM && s->kind == OCERZ_OPK_XMM) {
        if (xmm_is_pinned(s->reg) && xmm_is_pinned(d->reg)) {
            int vs = l0_src(s->reg, dbl);
            if (dbl) a64_ins_d_d(b, xmm_vreg(d->reg), 0, vs, 0); else a64_ins_s_s(b, xmm_vreg(d->reg), 0, vs, 0);
            l0_share(d->reg, s->reg);
            return 1;
        }
        emit_xmm_ld_lo(b, size, VX0, s->reg);
        emit_xmm_st_lo(b, size, VX0, d->reg);
        l0_inval(d->reg);
        return 1;
    }
    if (d->kind == OCERZ_OPK_XMM && s->kind == OCERZ_OPK_MEM) {
        uint32_t *skip;
        l0_inval(d->reg);
        int vd = xmm_is_pinned(d->reg) ? xmm_vreg(d->reg) : VX0;
        if (!emit_sse_mem_addr(b, insn, s, size, exit_sites, n_exits, &skip)) return 0;
        emit_sse_mem_ld(b, size, vd);
        patch_guard_skip(skip, a64_label(b));
        if (vd == VX0) emit_xmm_st(b, VX0, d->reg);
        return 1;
    }
    if (d->kind == OCERZ_OPK_MEM && s->kind == OCERZ_OPK_XMM) {
        int vs = xmm_is_pinned(s->reg) ? l0_src(s->reg, dbl) : VX0;
        if (vs == VX0) emit_xmm_ld_lo(b, size, VX0, s->reg);
        uint32_t *skip;
        if (!emit_sse_mem_addr(b, insn, d, size, exit_sites, n_exits, &skip)) return 0;
        emit_sse_mem_st(b, size, vs);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    return 0;
}

int g_fpb_fast;

int g_cmps_mask_idx = -1;

static int emit_sse_fparith(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM) return 0;
    int dbl = 0, packed = 0, kind = 0;
    switch (insn->op) {
    case OCERZ_OP_ADDSS: kind=0; break;  case OCERZ_OP_ADDSD: kind=0; dbl=1; break;
    case OCERZ_OP_ADDPS: kind=0; packed=1; break; case OCERZ_OP_ADDPD: kind=0; dbl=1; packed=1; break;
    case OCERZ_OP_SUBSS: kind=1; break;  case OCERZ_OP_SUBSD: kind=1; dbl=1; break;
    case OCERZ_OP_SUBPS: kind=1; packed=1; break; case OCERZ_OP_SUBPD: kind=1; dbl=1; packed=1; break;
    case OCERZ_OP_MULSS: kind=2; break;  case OCERZ_OP_MULSD: kind=2; dbl=1; break;
    case OCERZ_OP_MULPS: kind=2; packed=1; break; case OCERZ_OP_MULPD: kind=2; dbl=1; packed=1; break;
    case OCERZ_OP_DIVSS: kind=3; break;  case OCERZ_OP_DIVSD: kind=3; dbl=1; break;
    case OCERZ_OP_DIVPS: kind=3; packed=1; break; case OCERZ_OP_DIVPD: kind=3; dbl=1; packed=1; break;
    case OCERZ_OP_MAXSS: kind=4; break;  case OCERZ_OP_MAXSD: kind=4; dbl=1; break;
    case OCERZ_OP_MAXPS: kind=4; packed=1; break; case OCERZ_OP_MAXPD: kind=4; dbl=1; packed=1; break;
    case OCERZ_OP_MINSS: kind=5; break;  case OCERZ_OP_MINSD: kind=5; dbl=1; break;
    case OCERZ_OP_MINPS: kind=5; packed=1; break; case OCERZ_OP_MINPD: kind=5; dbl=1; packed=1; break;
    case OCERZ_OP_SQRTSS: kind=6; break; case OCERZ_OP_SQRTSD: kind=6; dbl=1; break;
    case OCERZ_OP_SQRTPS: kind=6; packed=1; break; case OCERZ_OP_SQRTPD: kind=6; dbl=1; packed=1; break;
    default: return 0;
    }
    int esz = dbl ? 8 : 4;
    static int inexact_env = -1;
    if (inexact_env < 0) inexact_env = getenv("OCERZ_INEXACT_NAN") ? 1 : 0;
    int inexact_nan = inexact_env || g_fpb_fast || ocerz_afp();
    int vb;
    int dst_pinned = xmm_is_pinned(d->reg);
    if (s->kind == OCERZ_OPK_XMM && xmm_is_pinned(s->reg)) vb = packed ? xmm_vreg(s->reg) : l0_src(s->reg, dbl);
    else { if (!emit_sse_src(b, insn, s, packed ? 16 : esz, VX1, exit_sites, n_exits)) return 0; vb = VX1; }
    if (packed) {
        l0_flush_reg(b, d->reg);
        if (s->kind == OCERZ_OPK_XMM) l0_flush_reg(b, s->reg);
        l0_inval(d->reg);
    }
    if (kind == 6) {
        if (inexact_nan) {
            if (packed) { int vd = xmm_dst_reg(d->reg, VX2); a64_v_fsqrt(b, dbl, vd, vb); if (vd == VX2) emit_xmm_st(b, VX2, d->reg); }
            else {
                int t = dst_pinned && l0_enabled() ? l0_alloc(d->reg, dbl) : VX2;
            if (t < 0) t = VX2;
                a64_fsqrt_s(b, dbl, t, vb); emit_xmm_st_lo(b, esz, t, d->reg);
            }
            return 1;
        }
        if (packed) {
            a64_v_fsqrt(b, dbl, VX2, vb);
            emit_nan_fix_packed2(b, dbl, VX2, vb, vb, VX3, VX0);
            emit_xmm_st(b, VX2, d->reg);
        } else {
            int t = dst_pinned && l0_enabled() ? l0_alloc(d->reg, dbl) : VX2;
            if (t < 0) t = VX2;
            int fb = vb;
            if (t == vb) {
                if (dbl) a64_fmov_d_d(b, VX3, vb); else a64_fmov_s_s(b, VX3, vb);
                fb = VX3;
            }
            a64_fsqrt_s(b, dbl, t, vb);
            g_scalar_merge_next = t != VX2 && scalar_cvt_follows(d->reg, dbl);
            emit_nan_fix_scalar2(b, dbl, t, fb, fb);
            emit_xmm_st_lo(b, esz, t, d->reg);
        }
        return 1;
    }
    int va;
    if (dst_pinned) va = packed ? xmm_vreg(d->reg) : l0_src(d->reg, dbl);
    else { emit_xmm_ld(b, VX0, d->reg); va = VX0; }
    if (kind == 4 || kind == 5) {
        if (packed) {
            if (kind == 4) a64_v_fcmgt(b, dbl, VX2, va, vb);
            else           a64_v_fcmgt(b, dbl, VX2, vb, va);
            if (va == xmm_vreg(d->reg) && xmm_is_pinned(d->reg)) {
                a64_v_bif(b, va, vb, VX2);
            } else {
                a64_v_bsl(b, VX2, va, vb);
                emit_xmm_st(b, VX2, d->reg);
            }
        } else {
            int t = dst_pinned && l0_enabled() ? l0_alloc(d->reg, dbl) : VX2;
            if (t < 0) t = VX2;
            a64_fcmp(b, dbl, va, vb);
            a64_fcsel(b, dbl, t, va, vb, kind == 4 ? A64_GT : A64_MI);
            emit_xmm_st_lo(b, esz, t, d->reg);
        }
        return 1;
    }
    if (inexact_nan && packed && xmm_is_pinned(d->reg)) {
        switch (kind) {
        case 0: a64_v_fadd(b, dbl, va, va, vb); break;
        case 1: a64_v_fsub(b, dbl, va, va, vb); break;
        case 2: a64_v_fmul(b, dbl, va, va, vb); break;
        case 3: a64_v_fdiv(b, dbl, va, va, vb); break;
        }
        return 1;
    }
    if (packed) {
        switch (kind) {
        case 0: a64_v_fadd(b, dbl, VX2, va, vb); break;
        case 1: a64_v_fsub(b, dbl, VX2, va, vb); break;
        case 2: a64_v_fmul(b, dbl, VX2, va, vb); break;
        case 3: a64_v_fdiv(b, dbl, VX2, va, vb); break;
        }
        emit_nan_fix_packed2(b, dbl, VX2, va, vb, VX3, VX3);
        emit_xmm_st(b, VX2, d->reg);
    } else if (inexact_nan) {
        int t = dst_pinned && l0_enabled() ? l0_alloc(d->reg, dbl) : VX2;
            if (t < 0) t = VX2;
        switch (kind) {
        case 0: a64_fadd_s(b, dbl, t, va, vb); break;
        case 1: a64_fsub_s(b, dbl, t, va, vb); break;
        case 2: a64_fmul_s(b, dbl, t, va, vb); break;
        case 3: a64_fdiv_s(b, dbl, t, va, vb); break;
        }
        emit_xmm_st_lo(b, esz, t, d->reg);
    } else {
        int t = dst_pinned && l0_enabled() ? l0_alloc(d->reg, dbl) : VX2;
            if (t < 0) t = VX2;
        int fa = va, fb = vb;
        if (t == va || t == vb) {
            int alias = t == va ? va : vb;
            if (dbl) a64_fmov_d_d(b, VX3, alias); else a64_fmov_s_s(b, VX3, alias);
            if (t == va) fa = VX3;
            if (t == vb) fb = VX3;
        }
        switch (kind) {
        case 0: a64_fadd_s(b, dbl, t, va, vb); break;
        case 1: a64_fsub_s(b, dbl, t, va, vb); break;
        case 2: a64_fmul_s(b, dbl, t, va, vb); break;
        case 3: a64_fdiv_s(b, dbl, t, va, vb); break;
        }
        g_scalar_merge_next = t != VX2 && scalar_cvt_follows(d->reg, dbl);
        emit_nan_fix_scalar2(b, dbl, t, fa, fb);
        emit_xmm_st_lo(b, esz, t, d->reg);
    }
    return 1;
}

static int sse_int_kind(unsigned op, int *esz)
{
    *esz = 0;
    switch (op) {
    case OCERZ_OP_PXOR: case OCERZ_OP_XORPS: return SIK_XOR;
    case OCERZ_OP_PAND: case OCERZ_OP_ANDPS: return SIK_AND;
    case OCERZ_OP_POR:  case OCERZ_OP_ORPS:  return SIK_OR;
    case OCERZ_OP_PANDN: case OCERZ_OP_ANDNPS: return SIK_ANDN;
    case OCERZ_OP_PADDB: return SIK_ADD;   case OCERZ_OP_PADDW: *esz = 1; return SIK_ADD;
    case OCERZ_OP_PADDD: *esz = 2; return SIK_ADD; case OCERZ_OP_PADDQ: *esz = 3; return SIK_ADD;
    case OCERZ_OP_PSUBB: return SIK_SUB;   case OCERZ_OP_PSUBW: *esz = 1; return SIK_SUB;
    case OCERZ_OP_PSUBD: *esz = 2; return SIK_SUB; case OCERZ_OP_PSUBQ: *esz = 3; return SIK_SUB;
    case OCERZ_OP_PCMPEQB: return SIK_CMPEQ; case OCERZ_OP_PCMPEQW: *esz = 1; return SIK_CMPEQ;
    case OCERZ_OP_PCMPEQD: *esz = 2; return SIK_CMPEQ; case OCERZ_OP_PCMPEQQ: *esz = 3; return SIK_CMPEQ;
    case OCERZ_OP_PCMPGTB: return SIK_CMPGT; case OCERZ_OP_PCMPGTW: *esz = 1; return SIK_CMPGT;
    case OCERZ_OP_PCMPGTD: *esz = 2; return SIK_CMPGT; case OCERZ_OP_PCMPGTQ: *esz = 3; return SIK_CMPGT;
    case OCERZ_OP_PMINUB: return SIK_UMIN; case OCERZ_OP_PMINUW: *esz = 1; return SIK_UMIN; case OCERZ_OP_PMINUD: *esz = 2; return SIK_UMIN;
    case OCERZ_OP_PMAXUB: return SIK_UMAX; case OCERZ_OP_PMAXUW: *esz = 1; return SIK_UMAX; case OCERZ_OP_PMAXUD: *esz = 2; return SIK_UMAX;
    case OCERZ_OP_PMINSB: return SIK_SMIN; case OCERZ_OP_PMINSW: *esz = 1; return SIK_SMIN; case OCERZ_OP_PMINSD: *esz = 2; return SIK_SMIN;
    case OCERZ_OP_PMAXSB: return SIK_SMAX; case OCERZ_OP_PMAXSW: *esz = 1; return SIK_SMAX; case OCERZ_OP_PMAXSD: *esz = 2; return SIK_SMAX;
    case OCERZ_OP_PMULLW: *esz = 1; return SIK_MUL; case OCERZ_OP_PMULLD: *esz = 2; return SIK_MUL;
    case OCERZ_OP_PADDUSB: return SIK_UQADD; case OCERZ_OP_PADDUSW: *esz = 1; return SIK_UQADD;
    case OCERZ_OP_PSUBUSB: return SIK_UQSUB; case OCERZ_OP_PSUBUSW: *esz = 1; return SIK_UQSUB;
    case OCERZ_OP_PADDSB: return SIK_SQADD;  case OCERZ_OP_PADDSW: *esz = 1; return SIK_SQADD;
    case OCERZ_OP_PSUBSB: return SIK_SQSUB;  case OCERZ_OP_PSUBSW: *esz = 1; return SIK_SQSUB;
    case OCERZ_OP_PAVGB: return SIK_AVG;     case OCERZ_OP_PAVGW: *esz = 1; return SIK_AVG;
    case OCERZ_OP_PMADDWD: return SIK_MADDWD;
    case OCERZ_OP_PMULHRSW: return SIK_MULHRSW;
    case OCERZ_OP_PACKSSDW: return SIK_PACKSSDW;
    case OCERZ_OP_PACKUSWB: return SIK_PACKUSWB;
    case OCERZ_OP_PMULUDQ: return SIK_MULUDQ;
    case OCERZ_OP_PMULHW: *esz = 1; return SIK_MULHW;
    case OCERZ_OP_PMULHUW: *esz = 1; return SIK_MULHUW;
    case OCERZ_OP_PACKSSWB: return SIK_PACKSSWB;
    case OCERZ_OP_PACKUSDW: return SIK_PACKUSDW;
    case OCERZ_OP_PMULDQ: return SIK_MULDQ;
    case OCERZ_OP_PSADBW: return SIK_SADBW;
    case OCERZ_OP_PMADDUBSW: return SIK_MADDUBSW;
    case OCERZ_OP_PABSB: return SIK_ABS; case OCERZ_OP_PABSW: *esz = 1; return SIK_ABS; case OCERZ_OP_PABSD: *esz = 2; return SIK_ABS;
    case OCERZ_OP_PSIGNB: return SIK_SIGN; case OCERZ_OP_PSIGNW: *esz = 1; return SIK_SIGN; case OCERZ_OP_PSIGND: *esz = 2; return SIK_SIGN;
    case OCERZ_OP_PHADDW: *esz = 1; return SIK_HADD; case OCERZ_OP_PHADDD: *esz = 2; return SIK_HADD;
    case OCERZ_OP_PHSUBW: *esz = 1; return SIK_HSUB; case OCERZ_OP_PHSUBD: *esz = 2; return SIK_HSUB;
    case OCERZ_OP_PHADDSW: *esz = 1; return SIK_HADDS;
    case OCERZ_OP_PHSUBSW: *esz = 1; return SIK_HSUBS;
    default: return 0;
    }
}

static int sse_int_self_zero(int kind)
{
    return kind == SIK_XOR || kind == SIK_ANDN || kind == SIK_SUB || kind == SIK_CMPGT ||
           kind == SIK_UQSUB || kind == SIK_SQSUB;
}

static void emit_sse_int_op(A64Buf *b, int kind, int esz, int vd, int va, int vb)
{
    switch (kind) {
    case SIK_XOR:   a64_v_eor(b, vd, va, vb); break;
    case SIK_AND:   a64_v_and(b, vd, va, vb); break;
    case SIK_OR:    a64_v_orr(b, vd, va, vb); break;
    case SIK_ANDN:  a64_v_bic(b, vd, vb, va); break;
    case SIK_ADD:   a64_v_add(b, esz, vd, va, vb); break;
    case SIK_SUB:   a64_v_sub(b, esz, vd, va, vb); break;
    case SIK_CMPEQ: a64_v_cmeq(b, esz, vd, va, vb); break;
    case SIK_CMPGT: a64_v_cmgt(b, esz, vd, va, vb); break;
    case SIK_UMIN:  a64_v_umin(b, esz, vd, va, vb); break;
    case SIK_UMAX:  a64_v_umax(b, esz, vd, va, vb); break;
    case SIK_SMIN:  a64_v_smin(b, esz, vd, va, vb); break;
    case SIK_SMAX:  a64_v_smax(b, esz, vd, va, vb); break;
    case SIK_MUL:   a64_v_mul(b, esz, vd, va, vb); break;
    case SIK_UQADD: a64_v_uqadd(b, esz, vd, va, vb); break;
    case SIK_UQSUB: a64_v_uqsub(b, esz, vd, va, vb); break;
    case SIK_SQADD: a64_v_sqadd(b, esz, vd, va, vb); break;
    case SIK_SQSUB: a64_v_sqsub(b, esz, vd, va, vb); break;
    case SIK_MADDWD:
        a64_v_smull_h(b, 0, VX2, va, vb);
        a64_v_smull_h(b, 1, VX3, va, vb);
        a64_v_addp_4s(b, vd, VX2, VX3);
        break;
    case SIK_MULHRSW:
        a64_v_smull_h(b, 0, VX2, va, vb);
        a64_v_smull_h(b, 1, VX3, va, vb);
        a64_v_rshrn_s15(b, 0, vd, VX2);
        a64_v_rshrn_s15(b, 1, vd, VX3);
        break;
    case SIK_PACKSSDW:
        a64_v_sqxtn_s(b, 0, VX2, va);
        a64_v_sqxtn_s(b, 1, VX2, vb);
        a64_v_mov(b, vd, VX2);
        break;
    case SIK_PACKUSWB:
        a64_v_sqxtun_h(b, 0, VX2, va);
        a64_v_sqxtun_h(b, 1, VX2, vb);
        a64_v_mov(b, vd, VX2);
        break;
    case SIK_MULUDQ:
        a64_v_xtn(b, 2, VX2, va);
        a64_v_xtn(b, 2, VX3, vb);
        a64_v_umull_s(b, vd, VX2, VX3);
        break;
    case SIK_MULHW:
    case SIK_MULHUW:
        if (kind == SIK_MULHW) { a64_v_smull_h(b, 0, VX2, va, vb); a64_v_smull_h(b, 1, VX3, va, vb); }
        else                   { a64_v_umull_h(b, 0, VX2, va, vb); a64_v_umull_h(b, 1, VX3, va, vb); }
        a64_v_uzp(b, 1, 1, vd, VX2, VX3);
        break;
    case SIK_PACKSSWB:
        a64_v_sqxtn_h(b, 0, VX2, va);
        a64_v_sqxtn_h(b, 1, VX2, vb);
        a64_v_mov(b, vd, VX2);
        break;
    case SIK_PACKUSDW:
        a64_v_sqxtun_s(b, 0, VX2, va);
        a64_v_sqxtun_s(b, 1, VX2, vb);
        a64_v_mov(b, vd, VX2);
        break;
    case SIK_MULDQ:
        a64_v_xtn(b, 2, VX2, va);
        a64_v_xtn(b, 2, VX3, vb);
        a64_v_smull_s(b, vd, VX2, VX3);
        break;
    case SIK_SADBW:
        a64_v_uabd(b, 0, VX2, va, vb);
        a64_v_uaddlp(b, 0, VX2, VX2);
        a64_v_uaddlp(b, 1, VX2, VX2);
        a64_v_uaddlp(b, 2, vd, VX2);
        break;
    case SIK_MADDUBSW:
        a64_v_xtl(b, 0, 1, VX2, va);
        a64_v_xtl(b, 1, 1, VX3, vb);
        a64_v_mul(b, 1, VX2, VX2, VX3);
        a64_v_xtl2(b, 1, 1, VX3, vb);
        a64_v_xtl2(b, 0, 1, vd, va);
        a64_v_mul(b, 1, vd, vd, VX3);
        a64_v_uzp(b, 1, 1, VX3, VX2, vd);
        a64_v_uzp(b, 1, 0, VX2, VX2, vd);
        a64_v_sqadd(b, 1, vd, VX2, VX3);
        break;
    case SIK_ABS:
        a64_v_abs(b, esz, vd, vb);
        break;
    case SIK_SIGN:
        a64_v_sshr_imm(b, esz, VX2, vb, (8 << esz) - 1);
        a64_v_neg(b, esz, VX3, va);
        a64_v_bsl(b, VX2, VX3, va);
        a64_v_cmeq0(b, esz, VX3, vb);
        a64_v_bic(b, vd, VX2, VX3);
        break;
    case SIK_HADD:
        a64_v_addp(b, esz, vd, va, vb);
        break;
    case SIK_HSUB:
    case SIK_HADDS:
    case SIK_HSUBS:
        a64_v_uzp(b, esz, 0, VX2, va, vb);
        a64_v_uzp(b, esz, 1, VX3, va, vb);
        if (kind == SIK_HSUB)       a64_v_sub(b, esz, vd, VX2, VX3);
        else if (kind == SIK_HADDS) a64_v_sqadd(b, esz, vd, VX2, VX3);
        else                        a64_v_sqsub(b, esz, vd, VX2, VX3);
        break;
    default:        a64_v_urhadd(b, esz, vd, va, vb); break;
    }
}

static int emit_sse_bitwise(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM) return 0;
    int esz, kind = sse_int_kind(insn->op, &esz);
    if (!kind) return 0;
    if (s->kind == OCERZ_OPK_XMM && s->reg == d->reg && sse_int_self_zero(kind)) {
        int vd = xmm_dst_reg(d->reg, VX0);
        a64_v_zero(b, vd);
        if (vd == VX0) emit_xmm_st(b, VX0, d->reg);
        return 1;
    }
    int vb = emit_sse_src_reg(b, insn, s, 16, VX1, exit_sites, n_exits);
    if (vb < 0) return 0;
    int vd = xmm_dst_reg(d->reg, VX0);
    if (vd == VX0) emit_xmm_ld(b, VX0, d->reg);
    emit_sse_int_op(b, kind, esz, vd, vd, vb);
    if (vd == VX0) emit_xmm_st(b, VX0, d->reg);
    return 1;
}

static int emit_sse_pblendw_palignr(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    if (insn->nops != 3 || !sse_enabled()) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM || insn->vex) return 0;
    unsigned imm = (unsigned)insn->ops[2].imm & 0xff;
    int vb = emit_sse_src_reg(b, insn, s, 16, VX1, exit_sites, n_exits);
    if (vb < 0) return 0;
    int vd = xmm_dst_reg(d->reg, VX0);
    if (vd == VX0) emit_xmm_ld(b, VX0, d->reg);
    if (insn->op == OCERZ_OP_PBLENDW) {
        for (int i = 0; i < 8; i++)
            if (imm & (1u << i))
                a64_ins_h_h(b, vd, i, vb, i);
    } else if (imm < 16) {
        a64_v_ext(b, VX2, vb, vd, (int)imm);
        a64_v_mov(b, vd, VX2);
    } else if (imm < 32) {
        a64_v_zero(b, VX3);
        a64_v_ext(b, VX2, vd, VX3, (int)(imm - 16));
        a64_v_mov(b, vd, VX2);
    } else {
        a64_v_zero(b, vd);
    }
    if (vd == VX0) emit_xmm_st(b, VX0, d->reg);
    return 1;
}

static int emit_sse_pshuflhw(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    if (insn->nops != 3 || insn->ops[2].kind != OCERZ_OPK_IMM) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM || (s->kind != OCERZ_OPK_XMM && s->kind != OCERZ_OPK_MEM)) return 0;
    unsigned imm = (unsigned)insn->ops[2].imm & 0xff;
    int base = insn->op == OCERZ_OP_PSHUFHW ? 4 : 0;
    int vs = emit_sse_src_reg(b, insn, s, 16, VX1, exit_sites, n_exits);
    if (vs < 0) return 0;
    int vd = xmm_dst_reg(d->reg, VX0);
    int from = vs;
    if (vs == vd) { a64_v_mov(b, VX2, vs); from = VX2; }
    else a64_v_mov(b, vd, vs);
    for (int i = 0; i < 4; i++) {
        int sel = (int)((imm >> (2 * i)) & 3);
        if (from == VX2 && sel == i) continue;
        if (from != VX2 && sel == i) continue;
        a64_ins_h_h(b, vd, base + i, from, base + sel);
    }
    if (vd == VX0) emit_xmm_st(b, VX0, d->reg);
    return 1;
}

static int emit_sse_bytesh(A64Buf *b, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0], *c = &insn->ops[1];
    if (insn->nops != 2 || d->kind != OCERZ_OPK_XMM || c->kind != OCERZ_OPK_IMM || !xmm_is_pinned(d->reg)) return 0;
    unsigned n = (unsigned)c->imm & 0xff;
    int vd = xmm_vreg(d->reg);
    if (n == 0) return 1;
    if (n >= 16) { a64_v_zero(b, vd); return 1; }
    a64_v_zero(b, VX2);
    if (insn->op == OCERZ_OP_PSRLDQ) a64_v_ext(b, vd, vd, VX2, (int)n);
    else                             a64_v_ext(b, vd, VX2, vd, (int)(16 - n));
    return 1;
}

static int emit_sse_blendp(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    if (insn->nops != 3 || insn->ops[2].kind != OCERZ_OPK_IMM) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM) return 0;
    unsigned imm = (unsigned)insn->ops[2].imm & 0xff;
    int vb = emit_sse_src_reg(b, insn, s, 16, VX1, exit_sites, n_exits);
    if (vb < 0) return 0;
    int vd = xmm_dst_reg(d->reg, VX0);
    if (vd == VX0) emit_xmm_ld(b, VX0, d->reg);
    if (insn->op == OCERZ_OP_BLENDPD) {
        for (int i = 0; i < 2; i++)
            if (imm & (1u << i)) a64_ins_d_d(b, vd, i, vb, i);
    } else {
        for (int i = 0; i < 4; i++)
            if (imm & (1u << i)) a64_ins_s_s(b, vd, i, vb, i);
    }
    if (vd == VX0) emit_xmm_st(b, VX0, d->reg);
    return 1;
}

static int emit_sse_movsdup(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    if (insn->nops != 2) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM) return 0;
    int vs = emit_sse_src_reg(b, insn, s, 16, VX1, exit_sites, n_exits);
    if (vs < 0) return 0;
    int vd = xmm_dst_reg(d->reg, VX0);
    a64_v_trn(b, 2, insn->op == OCERZ_OP_MOVSHDUP, vd, vs, vs);
    if (vd == VX0) emit_xmm_st(b, VX0, d->reg);
    return 1;
}

static int emit_sse_movmskp(A64Buf *b, const X86Insn *insn)
{
    if (insn->nops != 2 || g_n_raslit >= RASLIT_MAX) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
    if (s->kind != OCERZ_OPK_XMM || !xmm_is_pinned(s->reg)) return 0;
    int ds = pin_slot(d->reg);
    if (ds < 0 || (rsp_is_ptr() && d->reg == OCERZ_RSP)) return 0;
    l0_flush_reg(b, s->reg);
    int dbl = insn->op == OCERZ_OP_MOVMSKPD;
    g_raslit[g_n_raslit].site = a64_label(b);
    g_raslit[g_n_raslit].retaddr = dbl ? 1ull : 0x0000000200000001ull;
    g_raslit[g_n_raslit].hi = dbl ? 2ull : 0x0000000800000004ull;
    g_raslit[g_n_raslit].kind = 2;
    g_raslit[g_n_raslit].rt = VX1;
    g_n_raslit++;
    a64_emit32(b, 0x9c000000u | (uint32_t)VX1);
    if (dbl) a64_v_sshr_2d(b, VX0, xmm_vreg(s->reg), 63);
    else     a64_v_sshr_4s(b, VX0, xmm_vreg(s->reg), 31);
    a64_v_and(b, VX0, VX0, VX1);
    if (dbl) a64_addp_d(b, VX0, VX0);
    else     a64_v_addv_4s(b, VX0, VX0);
    a64_fmov_x_from_v(b, 0, pin_hreg(ds), VX0);
    return 1;
}

static int emit_sse_cmpp(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    if (insn->nops != 3 || insn->ops[2].kind != OCERZ_OPK_IMM) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM) return 0;
    int dbl = insn->op == OCERZ_OP_CMPPD;
    unsigned pred = (unsigned)insn->ops[2].imm & 7;
    int vb = emit_sse_src_reg(b, insn, s, 16, VX1, exit_sites, n_exits);
    if (vb < 0) return 0;
    int vd = xmm_dst_reg(d->reg, VX0);
    if (vd == VX0) emit_xmm_ld(b, VX0, d->reg);
    switch (pred) {
    case 0: a64_v_fcmeq(b, dbl, vd, vd, vb); break;
    case 1: a64_v_fcmgt(b, dbl, vd, vb, vd); break;
    case 2: a64_v_fcmge(b, dbl, vd, vb, vd); break;
    case 3: a64_v_fcmeq(b, dbl, VX2, vd, vd); a64_v_fcmeq(b, dbl, VX3, vb, vb);
            a64_v_and(b, VX2, VX2, VX3); a64_v_not(b, vd, VX2); break;
    case 4: a64_v_fcmeq(b, dbl, vd, vd, vb); a64_v_not(b, vd, vd); break;
    case 5: a64_v_fcmgt(b, dbl, vd, vb, vd); a64_v_not(b, vd, vd); break;
    case 6: a64_v_fcmge(b, dbl, vd, vb, vd); a64_v_not(b, vd, vd); break;
    default: a64_v_fcmeq(b, dbl, VX2, vd, vd); a64_v_fcmeq(b, dbl, VX3, vb, vb);
             a64_v_and(b, vd, VX2, VX3); break;
    }
    if (vd == VX0) emit_xmm_st(b, VX0, d->reg);
    return 1;
}

static int emit_sse_cvtp(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    if (insn->nops != 2) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM) return 0;
    int half = insn->op == OCERZ_OP_CVTDQ2PD || insn->op == OCERZ_OP_CVTPS2PD;
    int vs = emit_sse_src_reg(b, insn, s, half ? 8 : 16, VX1, exit_sites, n_exits);
    if (vs < 0) return 0;
    int vd = xmm_dst_reg(d->reg, VX0);
    if (insn->op == OCERZ_OP_CVTDQ2PD) {
        a64_v_xtl(b, 1, 4, VX2, vs);
        a64_v_scvtf_2d(b, vd, VX2);
    } else if (insn->op == OCERZ_OP_CVTPS2PD) {
        a64_v_fcvtl(b, vd, vs);
    } else if (insn->op == OCERZ_OP_CVTPD2PS) {
        a64_v_fcvtn(b, vd, vs);
    } else {
        int src = vs;
        if (insn->op == OCERZ_OP_CVTPS2DQ) { a64_v_frint(b, 0, 4, VX3, vs); src = VX3; }
        a64_v_movi_s_lsl24(b, VX2, 0x4f);
        a64_v_fcmgt(b, 0, VX2, VX2, src);
        a64_v_fcvtzs_4s(b, VX3, src);
        a64_v_movi_s_lsl24(b, vd, 0x80);
        a64_v_bit(b, vd, VX3, VX2);
    }
    if (vd == VX0) emit_xmm_st(b, VX0, d->reg);
    return 1;
}

static void emit_v_literal(A64Buf *b, int vt, uint64_t lo, uint64_t hi)
{
    g_raslit[g_n_raslit].site = a64_label(b);
    g_raslit[g_n_raslit].retaddr = lo;
    g_raslit[g_n_raslit].hi = hi;
    g_raslit[g_n_raslit].kind = 2;
    g_raslit[g_n_raslit].rt = vt;
    g_n_raslit++;
    a64_emit32(b, 0x9c000000u | (uint32_t)vt);
}

static int emit_sse_aes(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    unsigned op = insn->op;
    int kga = op == OCERZ_OP_AESKEYGENASSIST;
    if (insn->nops != (kga ? 3 : 2)) return 0;
    if (kga && (insn->ops[2].kind != OCERZ_OPK_IMM || g_n_raslit + 2 > RASLIT_MAX)) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM) return 0;
    int vb = emit_sse_src_reg(b, insn, s, 16, VX1, exit_sites, n_exits);
    if (vb < 0) return 0;
    int vd = xmm_dst_reg(d->reg, VX0);
    int need_d = op != OCERZ_OP_AESIMC && !kga;
    if (vd == VX0 && need_d) emit_xmm_ld(b, VX0, d->reg);
    switch (op) {
    case OCERZ_OP_AESENC: case OCERZ_OP_AESENCLAST:
        a64_v_zero(b, VX3);
        a64_aese(b, VX3, vd);
        if (op == OCERZ_OP_AESENC) a64_aesmc(b, VX3, VX3);
        a64_v_eor(b, vd, VX3, vb);
        break;
    case OCERZ_OP_AESDEC: case OCERZ_OP_AESDECLAST:
        a64_v_zero(b, VX3);
        a64_aesd(b, VX3, vd);
        if (op == OCERZ_OP_AESDEC) a64_aesimc(b, VX3, VX3);
        a64_v_eor(b, vd, VX3, vb);
        break;
    case OCERZ_OP_AESIMC:
        a64_aesimc(b, vd, vb);
        break;
    default: {
        uint64_t rcon = (uint64_t)(insn->ops[2].imm & 0xff) << 32;
        a64_v_zero(b, VX2);
        a64_aese(b, VX2, vb);
        emit_v_literal(b, VX3, 0x040b0e010b0e0104ull, 0x0c0306090306090cull);
        a64_v_tbl1(b, VX2, VX2, VX3);
        emit_v_literal(b, VX3, rcon, rcon);
        a64_v_eor(b, vd, VX2, VX3);
        break;
    }
    }
    if (vd == VX0) emit_xmm_st(b, VX0, d->reg);
    return 1;
}

static int emit_sse_pclmul(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    if (insn->nops != 3 || insn->ops[2].kind != OCERZ_OPK_IMM) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM) return 0;
    unsigned imm = (unsigned)insn->ops[2].imm & 0xff;
    int vb = emit_sse_src_reg(b, insn, s, 16, VX1, exit_sites, n_exits);
    if (vb < 0) return 0;
    int vd = xmm_dst_reg(d->reg, VX0);
    if (vd == VX0) emit_xmm_ld(b, VX0, d->reg);
    int xh = imm & 1, yh = (imm >> 4) & 1;
    if (xh == yh) {
        a64_v_pmull_d(b, xh, vd, vd, vb);
    } else {
        int vx = vd, vy = vb;
        if (xh) { a64_v_dup_d(b, VX2, vd, 1); vx = VX2; }
        if (yh) { a64_v_dup_d(b, VX3, vb, 1); vy = VX3; }
        a64_v_pmull_d(b, 0, vd, vx, vy);
    }
    if (vd == VX0) emit_xmm_st(b, VX0, d->reg);
    return 1;
}

int emit_crc32(A64Buf *b, const X86Insn *insn)
{
    if (insn->nops != 2 || insn->seg != OCERZ_SEG_NONE) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
    int ds = pin_slot(d->reg);
    if (ds < 0 || (rsp_is_ptr() && d->reg == OCERZ_RSP)) return 0;
    int size = s->size, rs;
    if (size != 1 && size != 2 && size != 4 && size != 8) return 0;
    if (s->kind == OCERZ_OPK_REG) {
        if (s->high8) return 0;
        int ss = pin_slot(s->reg);
        if (ss < 0 || (rsp_is_ptr() && s->reg == OCERZ_RSP)) return 0;
        rs = pin_hreg(ss);
    } else if (s->kind == OCERZ_OPK_MEM) {
        if (!emit_mem_load_any(b, insn, s, size, JT1)) return 0;
        rs = JT1;
    } else return 0;
    int rd = pin_hreg(ds);
    a64_crc32c(b, size, rd, rd, rs);
    return 1;
}

static int emit_sse_comis(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM) return 0;
    int dbl = insn->op == OCERZ_OP_UCOMISD || insn->op == OCERZ_OP_COMISD;
    int esz = dbl ? 8 : 4;
    if (g_cur_need == 0 && s->kind == OCERZ_OPK_XMM) {
        if (fpb_det_here(g_cur_insn_idx) && g_fpb_det[g_cur_insn_idx] == 2) {
            int vb = l0_src(s->reg, dbl), va = l0_src(d->reg, dbl);
            a64_fcmp(b, dbl, va, vb);
            fpb_site_emit(b, g_cur_insn_idx, va, vb, dbl);
        }
        return 1;
    }






    if (g_cur_need == 0 && s->kind == OCERZ_OPK_MEM && !ENV_ON("OCERZ_NO_COMIS_MEM_FUSE")) {
        int vb = emit_sse_src_reg(b, insn, s, esz, VX1, exit_sites, n_exits);
        if (vb < 0) return 0;
        a64_str_v(b, esz, vb, 20, FCMP_MEM_OFF);
        return 1;
    }
    if (g_defer)
        a64_str(b, 4, A64_ZR, 20, CC_OP_OFF);
    int vb = (s->kind == OCERZ_OPK_XMM && xmm_is_pinned(s->reg)) ? l0_src(s->reg, dbl)
           : emit_sse_src_reg(b, insn, s, esz, VX1, exit_sites, n_exits);
    if (vb < 0) return 0;
    if (s->kind == OCERZ_OPK_MEM) a64_str_v(b, esz, vb, 20, FCMP_MEM_OFF);
    int va = xmm_is_pinned(d->reg) ? l0_src(d->reg, dbl) : VX0;
    if (va == VX0) emit_xmm_ld_lo(b, esz, VX0, d->reg);
    a64_fcmp(b, dbl, va, vb);
    if (fpb_det_here(g_cur_insn_idx)) fpb_site_emit(b, g_cur_insn_idx, va, vb, dbl);
    a64_ldr(b, 8, JTT, 20, RF_OFF);
    a64_cset(b, JT0, A64_LT);
    a64_cset(b, JT1, A64_VS);
    a64_bfi(b, 1, JTT, JT0, 0, 1);
    a64_bfi(b, 1, JTT, JT1, 2, 1);
    a64_cset(b, JT0, A64_EQ);
    a64_orr_reg(b, 1, JT0, JT0, JT1, 0);
    a64_bfi(b, 1, JTT, JT0, 6, 1);
    a64_mov_imm64(b, JTU, ~(uint64_t)(OCERZ_SF | OCERZ_OF | OCERZ_AF));
    a64_and_reg(b, 1, JTT, JTT, JTU, 0);
    a64_str(b, 8, JTT, 20, RF_OFF);
    return 1;
}

static int emit_sse_cvt(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    switch (insn->op) {
    case OCERZ_OP_CVTTSD2SI: case OCERZ_OP_CVTTSS2SI: {
        if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
        if (rsp_is_ptr() && d->reg == OCERZ_RSP) return 0;
        int dbl = insn->op == OCERZ_OP_CVTTSD2SI;
        int vs = (s->kind == OCERZ_OPK_XMM && xmm_is_pinned(s->reg)) ? l0_src(s->reg, dbl)
               : emit_sse_src_reg(b, insn, s, dbl ? 8 : 4, VX0, exit_sites, n_exits);
        if (vs < 0) return 0;
        int ds = pin_slot(d->reg);
        int rd = ds >= 0 ? pin_hreg(ds) : JT0;
        int reuse = dbl && g_fcmp_self_idx == g_cur_insn_idx && g_fcmp_self_vreg == vs;
        int pend = g_scpend.valid && g_scpend.idx == g_cur_insn_idx - 1;
        if (pend && (!reuse || g_n_nanool + 1 > NANOOL_MAX || unsafe_nocheckbr())) {
            scalar_pend_flush(b);
            pend = 0;
            reuse = dbl && g_fcmp_self_idx == g_cur_insn_idx && g_fcmp_self_vreg == vs;
        }
        if (g_n_nanool + 1 <= NANOOL_MAX) {
            a64_fcvtzs(b, d->size == 8, dbl, rd, vs);
            if (!reuse)
                a64_fcmp(b, dbl, vs, vs);
            g_fcmp_self_idx = -1;
            a64_ccmn_imm(b, d->size == 8, rd, 1, 1, A64_VC);
            if (!unsafe_nocheckbr()) {
                NanOolPend *o = &g_nanool[g_n_nanool++];
                o->site = a64_label(b); a64_bcond(b, A64_VS, 0);
                o->back = a64_label(b);
                o->dbl = (uint8_t)dbl; o->packed = 0; o->vr = (uint8_t)rd;
                o->va = (uint8_t)vs; o->vb = 0; o->t1 = 0;
                o->cvt = d->size == 8 ? 1 : 2; o->refcmp = 0;
                o->pre = (uint8_t)pend;
                if (pend) { o->pvr = (uint8_t)g_scpend.vr; o->pva = (uint8_t)g_scpend.va; o->pvb = (uint8_t)g_scpend.vb; g_scpend.valid = 0; }
                o->idx = g_cur_insn_idx; o->is_cbz = 0;
            }
        } else if (d->size == 8) {
            a64_fcvtzs(b, 1, dbl, rd, vs);
            a64_cmn_imm(b, 1, rd, 1);
            a64_movz(b, JTU, 0x8000, 3);
            a64_csel(b, 1, rd, JTU, rd, A64_VS);
            a64_fcmp(b, dbl, vs, vs);
            a64_csel(b, 1, rd, JTU, rd, A64_VS);
        } else {
            a64_fcvtzs(b, 1, dbl, JT0, vs);
            a64_cmp_ext_sxtw(b, JT0, JT0);
            a64_movz(b, JTU, 0x8000, 1);
            a64_csel(b, 0, rd, JTU, JT0, A64_NE);
            a64_fcmp(b, dbl, vs, vs);
            a64_csel(b, 0, rd, JTU, rd, A64_VS);
        }
        if (ds < 0) emit_gpr_wr(b, JT0, d->reg);
        return 1;
    }
    case OCERZ_OP_CVTSI2SD: case OCERZ_OP_CVTSI2SS: {
        if (d->kind != OCERZ_OPK_XMM) return 0;
        int dbl = insn->op == OCERZ_OP_CVTSI2SD;
        int sf;
        if (s->kind == OCERZ_OPK_REG) {
            if (s->high8 || (s->size != 4 && s->size != 8)) return 0;
            sf = s->size == 8;
            emit_gpr_rd(b, sf, JT0, s->reg);
        } else if (s->kind == OCERZ_OPK_MEM) {
            if (s->size != 4 && s->size != 8) return 0;
            sf = s->size == 8;
            uint32_t *skip;
            if (!emit_sse_mem_addr(b, insn, s, s->size, exit_sites, n_exits, &skip)) return 0;
            emit_sse_mem_ld_gpr(b, s->size, JT0);
            patch_guard_skip(skip, a64_label(b));
        } else return 0;
        {
            int t = xmm_is_pinned(d->reg) && l0_enabled() ? l0_alloc(d->reg, dbl) : VX0;
            if (t < 0) t = VX0;
            a64_scvtf(b, sf, dbl, t, JT0);
            emit_xmm_st_lo(b, dbl ? 8 : 4, t, d->reg);
        }
        return 1;
    }
    case OCERZ_OP_CVTSD2SS: {
        if (d->kind != OCERZ_OPK_XMM) return 0;
        if (!emit_sse_src(b, insn, s, 8, VX0, exit_sites, n_exits)) return 0;
        a64_fcvt_d2s(b, VX1, VX0);
        emit_xmm_st_lo(b, 4, VX1, d->reg);
        return 1;
    }
    case OCERZ_OP_CVTSS2SD: {
        if (d->kind != OCERZ_OPK_XMM) return 0;
        if (!emit_sse_src(b, insn, s, 4, VX0, exit_sites, n_exits)) return 0;
        a64_fcvt_s2d(b, VX1, VX0);
        emit_xmm_st_lo(b, 8, VX1, d->reg);
        return 1;
    }
    case OCERZ_OP_CVTDQ2PS: {
        if (d->kind != OCERZ_OPK_XMM) return 0;
        if (!emit_sse_src(b, insn, s, 16, VX0, exit_sites, n_exits)) return 0;
        a64_v_scvtf_4s(b, VX1, VX0);
        emit_xmm_st(b, VX1, d->reg);
        return 1;
    }
    default: return 0;
    }
}

static int emit_sse_movd(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind == OCERZ_OPK_XMM && s->kind == OCERZ_OPK_REG) {
        if (s->high8 || (s->size != 4 && s->size != 8)) return 0;
        emit_gpr_rd(b, s->size == 8, JT0, s->reg);
        a64_fmov_v_from_x(b, s->size == 8, VX0, JT0);
        emit_xmm_st(b, VX0, d->reg);
        return 1;
    }
    if (d->kind == OCERZ_OPK_REG && s->kind == OCERZ_OPK_XMM) {
        if (d->high8 || (d->size != 4 && d->size != 8)) return 0;
        emit_xmm_ld_lo(b, d->size, VX0, s->reg);
        a64_fmov_x_from_v(b, d->size == 8, JT0, VX0);
        emit_gpr_wr(b, JT0, d->reg);
        return 1;
    }
    if (d->kind == OCERZ_OPK_XMM && s->kind == OCERZ_OPK_MEM) {
        if (s->size != 4 && s->size != 8) return 0;
        uint32_t *skip;
        if (!emit_sse_mem_addr(b, insn, s, s->size, exit_sites, n_exits, &skip)) return 0;
        emit_sse_mem_ld(b, s->size, VX0);
        patch_guard_skip(skip, a64_label(b));
        emit_xmm_st(b, VX0, d->reg);
        return 1;
    }
    if (d->kind == OCERZ_OPK_MEM && s->kind == OCERZ_OPK_XMM) {
        if (d->size != 4 && d->size != 8) return 0;
        emit_xmm_ld_lo(b, d->size, VX0, s->reg);
        uint32_t *skip;
        if (!emit_sse_mem_addr(b, insn, d, d->size, exit_sites, n_exits, &skip)) return 0;
        emit_sse_mem_st(b, d->size, VX0);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    return 0;
}

static int emit_sse_movq(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (insn->nops != 2) return 0;
    if (d->kind == OCERZ_OPK_XMM && s->kind == OCERZ_OPK_XMM) {
        if (!xmm_is_pinned(d->reg) || !xmm_is_pinned(s->reg)) return 0;
        l0_flush_reg(b, s->reg);
        l0_inval(d->reg);
        a64_fmov_d_d(b, xmm_vreg(d->reg), xmm_vreg(s->reg));
        return 1;
    }
    if (d->kind == OCERZ_OPK_XMM && s->kind == OCERZ_OPK_REG) {
        if (s->high8 || s->size != 8 || !xmm_is_pinned(d->reg)) return 0;
        emit_gpr_rd(b, 1, JT0, s->reg);
        a64_fmov_v_from_x(b, 1, xmm_vreg(d->reg), JT0);
        return 1;
    }
    if (d->kind == OCERZ_OPK_REG && s->kind == OCERZ_OPK_XMM) {
        if (d->high8 || d->size != 8 || !xmm_is_pinned(s->reg)) return 0;
        l0_flush_reg(b, s->reg);
        a64_fmov_x_from_v(b, 1, JT0, xmm_vreg(s->reg));
        emit_gpr_wr(b, JT0, d->reg);
        return 1;
    }
    if (d->kind == OCERZ_OPK_XMM && s->kind == OCERZ_OPK_MEM) {
        if (!xmm_is_pinned(d->reg)) return 0;
        l0_inval(d->reg);
        int vd = xmm_vreg(d->reg);
        if (emit_plain_mem_fast(b, insn, s, 8, vd, 0, 1)) return 1;
        uint32_t *skip;
        if (!emit_sse_mem_addr(b, insn, s, 8, exit_sites, n_exits, &skip)) return 0;
        emit_sse_mem_ld(b, 8, vd);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    if (d->kind == OCERZ_OPK_MEM && s->kind == OCERZ_OPK_XMM) {
        if (!xmm_is_pinned(s->reg)) return 0;
        l0_flush_reg(b, s->reg);
        int vs = xmm_vreg(s->reg);
        if (emit_plain_mem_fast(b, insn, d, 8, vs, 1, 1)) return 1;
        uint32_t *skip;
        if (!emit_sse_mem_addr(b, insn, d, 8, exit_sites, n_exits, &skip)) return 0;
        emit_sse_mem_st(b, 8, vs);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    return 0;
}

static int emit_sse_pshufd(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    if (insn->nops != 3 || !sse_enabled()) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM || !xmm_is_pinned(d->reg)) return 0;
    unsigned imm = (unsigned)insn->ops[2].imm & 0xff;
    int vs = emit_sse_src_reg(b, insn, s, 16, VX1, exit_sites, n_exits);
    if (vs < 0) return 0;
    l0_flush_reg(b, d->reg);
    l0_inval(d->reg);
    int vd = xmm_vreg(d->reg);
    unsigned sel[4] = { imm & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 3 };
    if (sel[0] == sel[1] && sel[1] == sel[2] && sel[2] == sel[3]) { a64_v_dup_s(b, vd, vs, (int)sel[0]); return 1; }
    if (sel[0] == 0 && sel[1] == 1 && sel[2] == 0 && sel[3] == 1) { a64_v_dup_d(b, vd, vs, 0); return 1; }
    if (sel[0] == 2 && sel[1] == 3 && sel[2] == 2 && sel[3] == 3) { a64_v_dup_d(b, vd, vs, 1); return 1; }
    if (sel[0] == 2 && sel[1] == 3 && sel[2] == 0 && sel[3] == 1) { a64_v_ext(b, vd, vs, vs, 8); return 1; }
    if (sel[0] == 0 && sel[1] == 1 && sel[2] == 2 && sel[3] == 3) { if (vd != vs) a64_v_mov(b, vd, vs); return 1; }
    if (sel[0] == 1 && sel[1] == 0 && sel[2] == 3 && sel[3] == 2) { a64_v_rev64_4s(b, vd, vs); return 1; }
    if (sel[0] == 3 && sel[1] == 2 && sel[2] == 1 && sel[3] == 0) { a64_v_rev64_4s(b, VX0, vs); a64_v_ext(b, vd, VX0, VX0, 8); return 1; }
    if (g_n_raslit >= RASLIT_MAX) return 0;
    uint64_t lo = 0, hi = 0;
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++) {
            uint64_t byte = (uint64_t)(sel[i] * 4 + (unsigned)k);
            int pos = i * 4 + k;
            if (pos < 8) lo |= byte << (8 * pos); else hi |= byte << (8 * (pos - 8));
        }
    g_raslit[g_n_raslit].site = a64_label(b);
    g_raslit[g_n_raslit].retaddr = lo;
    g_raslit[g_n_raslit].hi = hi;
    g_raslit[g_n_raslit].kind = 2;
    g_raslit[g_n_raslit].rt = VX0;
    g_n_raslit++;
    a64_emit32(b, 0x9c000000u | (uint32_t)VX0);
    a64_v_tbl1(b, vd, vs, VX0);
    return 1;
}

static int emit_sse_pshufb(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    if (insn->nops != 2 || !sse_enabled()) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM || !xmm_is_pinned(d->reg)) return 0;
    int vb = emit_sse_src_reg(b, insn, s, 16, VX1, exit_sites, n_exits);
    if (vb < 0) return 0;
    l0_flush_reg(b, d->reg);
    l0_inval(d->reg);
    int vd = xmm_vreg(d->reg);
    a64_emit32(b, 0x4f04e5e0u | (uint32_t)VX0);
    a64_v_and(b, VX0, vb, VX0);
    a64_v_tbl1(b, vd, vd, VX0);
    return 1;
}

static int emit_sse_punpck(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    if (insn->nops != 2 || !sse_enabled()) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM || !xmm_is_pinned(d->reg)) return 0;
    int vb = emit_sse_src_reg(b, insn, s, 16, VX1, exit_sites, n_exits);
    if (vb < 0) return 0;
    l0_flush_reg(b, d->reg);
    l0_inval(d->reg);
    int vd = xmm_vreg(d->reg);
    switch (insn->op) {
    case OCERZ_OP_PUNPCKLBW:  a64_v_zip1(b, 0, vd, vd, vb); break;
    case OCERZ_OP_PUNPCKLWD:  a64_v_zip1(b, 1, vd, vd, vb); break;
    case OCERZ_OP_PUNPCKLDQ:  a64_v_zip1(b, 2, vd, vd, vb); break;
    case OCERZ_OP_PUNPCKLQDQ: a64_v_zip1(b, 3, vd, vd, vb); break;
    case OCERZ_OP_PUNPCKHBW:  a64_v_zip2(b, 0, vd, vd, vb); break;
    case OCERZ_OP_PUNPCKHWD:  a64_v_zip2(b, 1, vd, vd, vb); break;
    case OCERZ_OP_PUNPCKHDQ:  a64_v_zip2(b, 2, vd, vd, vb); break;
    case OCERZ_OP_PUNPCKHQDQ: a64_v_zip2(b, 3, vd, vd, vb); break;
    default: return 0;
    }
    return 1;
}

static int emit_sse_unpck(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM) return 0;
    int vb = emit_sse_src_reg(b, insn, s, 16, VX1, exit_sites, n_exits);
    if (vb < 0) return 0;
    int va = xmm_is_pinned(d->reg) ? xmm_vreg(d->reg) : VX0;
    if (va == VX0) emit_xmm_ld(b, VX0, d->reg);
    int vd = xmm_dst_reg(d->reg, VX2);
    switch (insn->op) {
    case OCERZ_OP_UNPCKLPD: case OCERZ_OP_MOVLHPS: a64_v_zip1(b, 3, vd, va, vb); break;
    case OCERZ_OP_UNPCKHPD:                        a64_v_zip2(b, 3, vd, va, vb); break;
    case OCERZ_OP_MOVHLPS:
        if (vd != va) a64_v_mov(b, vd, va);
        a64_ins_d_d(b, vd, 0, vb, 1); break;
    case OCERZ_OP_UNPCKLPS: a64_v_zip1(b, 2, vd, va, vb); break;
    case OCERZ_OP_UNPCKHPS: a64_v_zip2(b, 2, vd, va, vb); break;
    default: return 0;
    }
    if (vd == VX2) emit_xmm_st(b, VX2, d->reg);
    return 1;
}

static void emit_cmps_pred(A64Buf *b, int dbl, unsigned pred, int vr, int va, int vb)
{
    switch (pred) {
    case 0: a64_fcmeq_s(b, dbl, vr, va, vb); break;
    case 1: a64_fcmgt_s(b, dbl, vr, vb, va); break;
    case 2: a64_fcmge_s(b, dbl, vr, vb, va); break;
    case 3: a64_fcmeq_s(b, dbl, vr, va, va); a64_fcmeq_s(b, dbl, VX3, vb, vb);
            a64_v_and(b, vr, vr, VX3); a64_v_not(b, vr, vr); break;
    case 4: a64_fcmeq_s(b, dbl, vr, va, vb); a64_v_not(b, vr, vr); break;
    case 5: a64_fcmgt_s(b, dbl, vr, vb, va); a64_v_not(b, vr, vr); break;
    case 6: a64_fcmge_s(b, dbl, vr, vb, va); a64_v_not(b, vr, vr); break;
    default: a64_fcmeq_s(b, dbl, vr, va, va); a64_fcmeq_s(b, dbl, VX3, vb, vb);
            a64_v_and(b, vr, vr, VX3); break;
    }
}

static int emit_sse_cmps(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM || insn->nops < 3 || insn->ops[2].kind != OCERZ_OPK_IMM) return 0;
    int dbl = insn->op == OCERZ_OP_CMPSDX;
    int esz = dbl ? 8 : 4;
    unsigned pred = (unsigned)insn->ops[2].imm & 7;
    int vb = (s->kind == OCERZ_OPK_XMM && xmm_is_pinned(s->reg)) ? l0_src(s->reg, dbl)
           : emit_sse_src_reg(b, insn, s, esz, VX1, exit_sites, n_exits);
    if (vb < 0) return 0;
    int va = xmm_is_pinned(d->reg) ? l0_src(d->reg, dbl) : VX0;
    if (va == VX0) emit_xmm_ld_lo(b, esz, VX0, d->reg);
    emit_cmps_pred(b, dbl, pred, VX2, va, vb);
    if (cmps_blendv_fusable(g_cur_insn_idx)) {
        g_cmps_mask_idx = g_cur_insn_idx;
        l0_inval(d->reg);
        return 1;
    }
    emit_xmm_st_lo(b, esz, VX2, d->reg);
    l0_inval(d->reg);
    return 1;
}

static int emit_sse_blendv(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM) return 0;
    int fused = g_cmps_mask_idx == g_cur_insn_idx - 1 && cmps_blendv_fusable(g_cur_insn_idx - 1);
    g_cmps_mask_idx = -1;
    if (fused) {
        int dbl = insn->op == OCERZ_OP_BLENDVPD;
        int esz = dbl ? 8 : 4;
        int vd0 = l0_src(d->reg, dbl);
        int t = l0_alloc(d->reg, dbl);
        if (t >= 0) {
            int vs0, vsfull;
            if (s->kind == OCERZ_OPK_XMM && xmm_is_pinned(s->reg)) {
                vs0 = l0_src(s->reg, dbl);
                vsfull = xmm_vreg(s->reg);
            } else {
                vsfull = emit_sse_src_reg(b, insn, s, 16, VX1, exit_sites, n_exits);
                if (vsfull < 0) return 0;
                vs0 = vsfull;
            }
            if (vd0 != t) {
                if (dbl) a64_fmov_d_d(b, t, vd0); else a64_fmov_s_s(b, t, vd0);
            }
            a64_v_bit(b, t, vs0, VX2);
            if (dbl) a64_v_sshr_2d(b, VX3, xmm_vreg(0), 63);
            else     a64_v_sshr_4s(b, VX3, xmm_vreg(0), 31);
            a64_v_bit(b, xmm_vreg(d->reg), vsfull, VX3);
            g_l0_dirty |= (uint16_t)(1u << d->reg);
            if (dbl) a64_ins_d_d(b, xmm_vreg(0), 0, VX2, 0); else a64_ins_s_s(b, xmm_vreg(0), 0, VX2, 0);
            (void)esz;
            return 1;
        }
        if (dbl) a64_ins_d_d(b, xmm_vreg(0), 0, VX2, 0); else a64_ins_s_s(b, xmm_vreg(0), 0, VX2, 0);
    }
    if (s->kind == OCERZ_OPK_XMM) l0_flush_reg(b, s->reg);
    l0_flush_reg(b, d->reg);
    l0_flush_reg(b, 0);
    l0_inval(d->reg);
    int vb = emit_sse_src_reg(b, insn, s, 16, VX1, exit_sites, n_exits);
    if (vb < 0) return 0;
    int va = xmm_is_pinned(d->reg) ? xmm_vreg(d->reg) : VX0;
    if (va == VX0) emit_xmm_ld(b, VX0, d->reg);
    int vm = xmm_is_pinned(0) ? xmm_vreg(0) : VX2;
    if (vm == VX2) emit_xmm_ld(b, VX2, 0);
    switch (insn->op) {
    case OCERZ_OP_BLENDVPD: a64_v_sshr_2d(b, VX2, vm, 63); break;
    case OCERZ_OP_BLENDVPS: a64_v_sshr_4s(b, VX2, vm, 31); break;
    case OCERZ_OP_PBLENDVB: {
        a64_v_zero(b, VX3);
        a64_v_cmgt(b, 0, VX2, VX3, vm);
        break;
    }
    default: return 0;
    }
    if (va != VX0) {
        a64_v_bit(b, va, vb, VX2);
    } else {
        a64_v_bsl(b, VX2, vb, va);
        emit_xmm_st(b, VX2, d->reg);
    }
    return 1;
}

static void emit_simd_shift_imm(A64Buf *b, int kind, int esz, int vd, int vn, unsigned cnt)
{
    unsigned w = 8u << esz;
    if (cnt == 0) { if (vd != vn) a64_v_mov(b, vd, vn); }
    else if (kind == 2) a64_v_sshr_imm(b, esz, vd, vn, (int)(cnt < w ? cnt : w));
    else if (cnt >= w) a64_v_zero(b, vd);
    else if (kind == 0) a64_v_shl_imm(b, esz, vd, vn, (int)cnt);
    else a64_v_ushr_imm(b, esz, vd, vn, (int)cnt);
}

static int emit_sse_shift_imm(A64Buf *b, const X86Insn *insn)
{
    int kind, esz;
    switch (insn->op) {
    case OCERZ_OP_PSLLW: kind = 0; esz = 1; break;
    case OCERZ_OP_PSLLD: kind = 0; esz = 2; break;
    case OCERZ_OP_PSLLQ: kind = 0; esz = 3; break;
    case OCERZ_OP_PSRLW: kind = 1; esz = 1; break;
    case OCERZ_OP_PSRLD: kind = 1; esz = 2; break;
    case OCERZ_OP_PSRLQ: kind = 1; esz = 3; break;
    case OCERZ_OP_PSRAW: kind = 2; esz = 1; break;
    case OCERZ_OP_PSRAD: kind = 2; esz = 2; break;
    default: return 0;
    }
    const X86Operand *d = &insn->ops[0], *c = &insn->ops[1];
    if (insn->nops != 2 || d->kind != OCERZ_OPK_XMM || !xmm_is_pinned(d->reg)) return 0;
    if (c->kind == OCERZ_OPK_IMM) {
        emit_simd_shift_imm(b, kind, esz, xmm_vreg(d->reg), xmm_vreg(d->reg), (unsigned)(c->imm & 0xff));
        return 1;
    }
    if (c->kind == OCERZ_OPK_XMM) {
        if (!xmm_is_pinned(c->reg)) return 0;
        l0_flush_reg(b, c->reg);
        a64_fmov_x_from_v(b, 1, JT0, xmm_vreg(c->reg));
    } else if (c->kind == OCERZ_OPK_MEM) {
        if (!emit_mem_load_any(b, insn, c, 8, JT0)) return 0;
    } else return 0;
    a64_mov_imm64(b, JT1, 64);
    a64_subs_reg(b, 1, A64_ZR, JT0, JT1, 0);
    a64_csel(b, 1, JT0, JT0, JT1, A64_CC);
    if (kind != 0) a64_neg_reg(b, 1, JT0, JT0);
    a64_v_dup_gpr(b, 1 << esz, VX2, JT0);
    int vd = xmm_vreg(d->reg);
    if (kind == 2) a64_v_sshl(b, esz, vd, vd, VX2);
    else           a64_v_ushl(b, esz, vd, vd, VX2);
    return 1;
}

static void emit_shufp_lane(A64Buf *b, int dbl, unsigned imm, int vd, int va, int vb)
{
    if (dbl) {
        int i0 = (int)(imm & 1), i1 = (int)((imm >> 1) & 1);
        if (va == vb) {
            if (i0 == i1) a64_v_dup_d(b, vd, va, i0);
            else if (i0 == 1) a64_v_ext(b, vd, va, va, 8);
            else if (vd != va) a64_v_mov(b, vd, va);
            return;
        }
        if (i0 == 0 && i1 == 0) { a64_v_zip1(b, 3, vd, va, vb); return; }
        if (i0 == 1 && i1 == 1) { a64_v_zip2(b, 3, vd, va, vb); return; }
        if (i0 == 1 && i1 == 0) { a64_v_ext(b, vd, va, vb, 8); return; }
        if (vd == vb) { a64_ins_d_d(b, vd, 0, va, 0); return; }
        if (vd != va) a64_v_mov(b, vd, va);
        a64_ins_d_d(b, vd, 1, vb, 1);
        return;
    }
    int i0 = (int)(imm & 3), i1 = (int)((imm >> 2) & 3), i2 = (int)((imm >> 4) & 3), i3 = (int)((imm >> 6) & 3);
    if (vd != va && vd != vb) a64_v_dup_s(b, vd, va, i0);
    else a64_ins_s_s(b, vd, 0, va, i0);
    a64_ins_s_s(b, vd, 1, va, i1);
    a64_ins_s_s(b, vd, 2, vb, i2);
    a64_ins_s_s(b, vd, 3, vb, i3);
}

static int emit_sse_shufp(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (insn->nops != 3 || d->kind != OCERZ_OPK_XMM || insn->ops[2].kind != OCERZ_OPK_IMM || !xmm_is_pinned(d->reg)) return 0;

    if (insn->op == OCERZ_OP_SHUFPS && s->kind == OCERZ_OPK_XMM && s->reg == d->reg) {
        int v = xmm_vreg(d->reg);
        unsigned imm = (unsigned)insn->ops[2].imm;
        int sel[4] = { (int)(imm & 3), (int)((imm >> 2) & 3), (int)((imm >> 4) & 3), (int)((imm >> 6) & 3) };
        int changed = 0, k1 = -1;
        for (int k = 0; k < 4; k++)
            if (sel[k] != k) { changed++; k1 = k; }
        if (sel[0] == sel[1] && sel[1] == sel[2] && sel[2] == sel[3]) {
            a64_v_dup_s(b, v, v, sel[0]);
            return 1;
        }
        if (changed == 0) return 1;
        if (changed == 1) {
            a64_ins_s_s(b, v, k1, v, sel[k1]);
            return 1;
        }
    }
    int vb = emit_sse_src_reg(b, insn, s, 16, VX1, exit_sites, n_exits);
    if (vb < 0) return 0;

    if (insn->op == OCERZ_OP_SHUFPS && vb != xmm_vreg(d->reg)) {
        int vd = xmm_vreg(d->reg);
        switch ((unsigned)insn->ops[2].imm & 0xff) {
        case 0x44: a64_v_zip1(b, 3, vd, vd, vb); return 1;
        case 0xee: a64_v_zip2(b, 3, vd, vd, vb); return 1;
        case 0x88: a64_v_uzp1(b, 2, vd, vd, vb); return 1;
        case 0xdd: a64_v_uzp2(b, 2, vd, vd, vb); return 1;
        case 0xe4: a64_ins_d_d(b, vd, 1, vb, 1); return 1;
        case 0x4e: a64_v_ext(b, vd, vd, vb, 8); return 1;
        default: break;
        }
    }
    emit_shufp_lane(b, insn->op == OCERZ_OP_SHUFPD, (unsigned)insn->ops[2].imm, VX2, xmm_vreg(d->reg), vb);
    a64_v_mov(b, xmm_vreg(d->reg), VX2);
    return 1;
}

static int emit_sse_movddup(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (insn->nops != 2 || d->kind != OCERZ_OPK_XMM || !xmm_is_pinned(d->reg)) return 0;
    int vs = s->kind == OCERZ_OPK_XMM && xmm_is_pinned(s->reg) ? l0_src(s->reg, 1)
           : emit_sse_src_reg(b, insn, s, 8, VX0, exit_sites, n_exits);
    if (vs < 0) return 0;
    l0_inval(d->reg);
    a64_v_dup_d(b, xmm_vreg(d->reg), vs, 0);
    return 1;
}

static int emit_insertps_to(A64Buf *b, const X86Insn *insn, int va, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *s = &insn->ops[1];
    if (insn->nops != 3 || insn->ops[2].kind != OCERZ_OPK_IMM) return 0;
    unsigned imm = (unsigned)insn->ops[2].imm & 0xff;
    int vs, si = (int)((imm >> 6) & 3);
    if (s->kind == OCERZ_OPK_XMM) {
        if (!xmm_is_pinned(s->reg)) return 0;
        vs = xmm_vreg(s->reg);
    } else if (s->kind == OCERZ_OPK_MEM) {
        if (!emit_sse_src(b, insn, s, 4, VX1, exit_sites, n_exits)) return 0;
        vs = VX1;
        si = 0;
    } else {
        return 0;
    }
    a64_v_mov(b, VX2, va);
    a64_ins_s_s(b, VX2, (int)((imm >> 4) & 3), vs, si);
    for (int i = 0; i < 4; i++)
        if (imm & (1u << i)) a64_ins_gpr(b, 4, VX2, i, A64_ZR);
    return 1;
}

static int emit_sse_insertps(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0];
    if (d->kind != OCERZ_OPK_XMM || !xmm_is_pinned(d->reg)) return 0;
    if (!emit_insertps_to(b, insn, xmm_vreg(d->reg), exit_sites, n_exits)) return 0;
    a64_v_mov(b, xmm_vreg(d->reg), VX2);
    return 1;
}

static int mmx_ld(A64Buf *b, const X86Insn *insn, const X86Operand *s, int size, int vt,
                  uint32_t **exit_sites, int *n_exits)
{
    if (s->kind == OCERZ_OPK_MMX) {
        a64_ldr_v(b, 8, vt, 20, MMX_OFF(s->reg));
        return 1;
    }
    if (emit_plain_mem_fast(b, insn, s, size, vt, 0, 1))
        return 1;
    uint32_t *skip;
    if (!emit_sse_mem_addr(b, insn, s, size, exit_sites, n_exits, &skip))
        return 0;
    emit_sse_mem_ld(b, size, vt);
    patch_guard_skip(skip, a64_label(b));
    return 1;
}

static int mmx_st(A64Buf *b, const X86Insn *insn, const X86Operand *d, int size, int vs,
                  uint32_t **exit_sites, int *n_exits)
{
    if (emit_plain_mem_fast(b, insn, d, size, vs, 1, 1))
        return 1;
    uint32_t *skip;
    if (!emit_sse_mem_addr(b, insn, d, size, exit_sites, n_exits, &skip))
        return 0;
    emit_sse_mem_st(b, size, vs);
    patch_guard_skip(skip, a64_label(b));
    return 1;
}

static void mmx_enter(A64Buf *b)
{
    a64_movz(b, JTU, 0xff, 0);
    a64_str(b, 2, JTU, 20, (uint32_t)offsetof(OcerzCPU, ftw));
}

static int mmx_mov(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    int r = 0;
    g_vec_int_move = 1;
    if (d->kind == OCERZ_OPK_MMX && s->kind == OCERZ_OPK_MMX) {
        a64_ldr(b, 8, JT0, 20, MMX_OFF(s->reg));
        a64_str(b, 8, JT0, 20, MMX_OFF(d->reg));
        r = 1;
    } else if (d->kind == OCERZ_OPK_MMX && s->kind == OCERZ_OPK_REG && !s->high8 && (s->size == 4 || s->size == 8)) {
        emit_gpr_rd(b, 1, JT0, s->reg);
        if (s->size == 4) a64_mov_reg(b, 0, JT0, JT0);
        a64_str(b, 8, JT0, 20, MMX_OFF(d->reg));
        r = 1;
    } else if (d->kind == OCERZ_OPK_REG && s->kind == OCERZ_OPK_MMX && !d->high8 && (d->size == 4 || d->size == 8)) {
        a64_ldr(b, d->size, JT0, 20, MMX_OFF(s->reg));
        emit_gpr_wr(b, JT0, d->reg);
        r = 1;
    } else if (d->kind == OCERZ_OPK_MMX && s->kind == OCERZ_OPK_MEM && (s->size == 4 || s->size == 8)) {
        if (mmx_ld(b, insn, s, s->size, VX0, exit_sites, n_exits)) {
            a64_str_v(b, 8, VX0, 20, MMX_OFF(d->reg));
            r = 1;
        }
    } else if (d->kind == OCERZ_OPK_MEM && s->kind == OCERZ_OPK_MMX && (d->size == 4 || d->size == 8)) {
        a64_ldr_v(b, d->size, VX0, 20, MMX_OFF(s->reg));
        r = mmx_st(b, insn, d, d->size, VX0, exit_sites, n_exits);
    }
    g_vec_int_move = 0;
    return r;
}

int emit_mmx(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    static int off = -1;
    if (off < 0) off = getenv("OCERZ_NO_JIT_MMX") ? 1 : 0;
    if (off || insn->seg != OCERZ_SEG_NONE || insn->nops < 2)
        return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    int op = insn->op;
    if (op == OCERZ_OP_MOVD || op == OCERZ_OP_MOVQX) {
        if (insn->nops != 2 || !mmx_mov(b, insn, exit_sites, n_exits))
            return 0;
        mmx_enter(b);
        return 1;
    }
    if (op == OCERZ_OP_PEXTRW) {
        if (insn->nops != 3 || d->kind != OCERZ_OPK_REG || d->high8 || s->kind != OCERZ_OPK_MMX)
            return 0;
        a64_ldr(b, 2, JT0, 20, MMX_OFF(s->reg) + 2u * (uint32_t)(insn->ops[2].imm & 3));
        emit_gpr_wr(b, JT0, d->reg);
        mmx_enter(b);
        return 1;
    }
    if (op == OCERZ_OP_PINSRW) {
        if (insn->nops != 3 || d->kind != OCERZ_OPK_MMX || s->kind != OCERZ_OPK_REG || s->high8)
            return 0;
        emit_gpr_rd(b, 1, JT0, s->reg);
        a64_str(b, 2, JT0, 20, MMX_OFF(d->reg) + 2u * (uint32_t)(insn->ops[2].imm & 3));
        mmx_enter(b);
        return 1;
    }
    if (d->kind != OCERZ_OPK_MMX)
        return 0;
    if (op == OCERZ_OP_PSHUFLW) {
        if (insn->nops != 3 || (s->kind != OCERZ_OPK_MMX && s->kind != OCERZ_OPK_MEM))
            return 0;
        unsigned imm = (unsigned)insn->ops[2].imm & 0xff;
        if (s->kind == OCERZ_OPK_MMX) {
            a64_ldr(b, 8, JT0, 20, MMX_OFF(s->reg));
        } else {
            if (!mmx_ld(b, insn, s, 8, VX1, exit_sites, n_exits))
                return 0;
            a64_fmov_x_from_v(b, 1, JT0, VX1);
        }
        for (int i = 0; i < 4; i++) {
            a64_ubfx(b, 1, JT2, JT0, 16 * (int)((imm >> (2 * i)) & 3), 16);
            if (i == 0) a64_mov_reg(b, 1, JT1, JT2);
            else a64_bfi(b, 1, JT1, JT2, 16 * i, 16);
        }
        a64_str(b, 8, JT1, 20, MMX_OFF(d->reg));
        mmx_enter(b);
        return 1;
    }
    int shift_imm = insn->nops == 2 && s->kind == OCERZ_OPK_IMM;
    if (insn->nops != 2 || (!shift_imm && s->kind != OCERZ_OPK_MMX && s->kind != OCERZ_OPK_MEM))
        return 0;
    if (shift_imm) {
        int esz, kind;
        switch (op) {
        case OCERZ_OP_PSLLW: esz = 1; kind = 0; break;
        case OCERZ_OP_PSLLD: esz = 2; kind = 0; break;
        case OCERZ_OP_PSLLQ: esz = 3; kind = 0; break;
        case OCERZ_OP_PSRLW: esz = 1; kind = 1; break;
        case OCERZ_OP_PSRLD: esz = 2; kind = 1; break;
        case OCERZ_OP_PSRLQ: esz = 3; kind = 1; break;
        case OCERZ_OP_PSRAW: esz = 1; kind = 2; break;
        case OCERZ_OP_PSRAD: esz = 2; kind = 2; break;
        default: return 0;
        }
        unsigned n = (unsigned)s->imm & 0xff, w = 8u << esz;
        a64_ldr_v(b, 8, VX0, 20, MMX_OFF(d->reg));
        if (n == 0) {
        } else if (kind == 2) {
            a64_v_sshr_imm(b, esz, VX0, VX0, (int)(n >= w ? w : n));
        } else if (n >= w) {
            a64_v_zero(b, VX0);
        } else if (kind == 0) {
            a64_v_shl_imm(b, esz, VX0, VX0, (int)n);
        } else {
            a64_v_ushr_imm(b, esz, VX0, VX0, (int)n);
        }
        a64_str_v(b, 8, VX0, 20, MMX_OFF(d->reg));
        mmx_enter(b);
        return 1;
    }
    switch (op) {
    case OCERZ_OP_PADDB: case OCERZ_OP_PADDW: case OCERZ_OP_PADDD: case OCERZ_OP_PADDQ:
    case OCERZ_OP_PSUBB: case OCERZ_OP_PSUBW: case OCERZ_OP_PSUBD: case OCERZ_OP_PSUBQ:
    case OCERZ_OP_PADDSB: case OCERZ_OP_PADDSW: case OCERZ_OP_PADDUSB: case OCERZ_OP_PADDUSW:
    case OCERZ_OP_PSUBSB: case OCERZ_OP_PSUBSW: case OCERZ_OP_PSUBUSB: case OCERZ_OP_PSUBUSW:
    case OCERZ_OP_PMULLW: case OCERZ_OP_PMULHW: case OCERZ_OP_PMULHUW:
    case OCERZ_OP_PAND: case OCERZ_OP_POR: case OCERZ_OP_PXOR: case OCERZ_OP_PANDN:
    case OCERZ_OP_PCMPEQB: case OCERZ_OP_PCMPEQW: case OCERZ_OP_PCMPEQD:
    case OCERZ_OP_PCMPGTB: case OCERZ_OP_PCMPGTW: case OCERZ_OP_PCMPGTD:
    case OCERZ_OP_PUNPCKLBW: case OCERZ_OP_PUNPCKLWD: case OCERZ_OP_PUNPCKLDQ:
    case OCERZ_OP_PUNPCKHBW: case OCERZ_OP_PUNPCKHWD: case OCERZ_OP_PUNPCKHDQ:
    case OCERZ_OP_PACKUSWB: case OCERZ_OP_PACKSSWB: case OCERZ_OP_PACKSSDW:
    case OCERZ_OP_PSADBW: case OCERZ_OP_PAVGB: case OCERZ_OP_PAVGW:
    case OCERZ_OP_PMINUB: case OCERZ_OP_PMAXUB: case OCERZ_OP_PMINSW: case OCERZ_OP_PMAXSW:
        break;
    default:
        return 0;
    }
    if (op == OCERZ_OP_PXOR && s->kind == OCERZ_OPK_MMX && s->reg == d->reg) {
        a64_str(b, 8, 31, 20, MMX_OFF(d->reg));
        mmx_enter(b);
        return 1;
    }
    a64_ldr_v(b, 8, VX0, 20, MMX_OFF(d->reg));
    if (!mmx_ld(b, insn, s, 8, VX1, exit_sites, n_exits))
        return 0;
    switch (op) {
    case OCERZ_OP_PADDB: a64_v_add(b, 0, VX0, VX0, VX1); break;
    case OCERZ_OP_PADDW: a64_v_add(b, 1, VX0, VX0, VX1); break;
    case OCERZ_OP_PADDD: a64_v_add(b, 2, VX0, VX0, VX1); break;
    case OCERZ_OP_PADDQ: a64_v_add(b, 3, VX0, VX0, VX1); break;
    case OCERZ_OP_PSUBB: a64_v_sub(b, 0, VX0, VX0, VX1); break;
    case OCERZ_OP_PSUBW: a64_v_sub(b, 1, VX0, VX0, VX1); break;
    case OCERZ_OP_PSUBD: a64_v_sub(b, 2, VX0, VX0, VX1); break;
    case OCERZ_OP_PSUBQ: a64_v_sub(b, 3, VX0, VX0, VX1); break;
    case OCERZ_OP_PADDSB: a64_v_sqadd(b, 0, VX0, VX0, VX1); break;
    case OCERZ_OP_PADDSW: a64_v_sqadd(b, 1, VX0, VX0, VX1); break;
    case OCERZ_OP_PADDUSB: a64_v_uqadd(b, 0, VX0, VX0, VX1); break;
    case OCERZ_OP_PADDUSW: a64_v_uqadd(b, 1, VX0, VX0, VX1); break;
    case OCERZ_OP_PSUBSB: a64_v_sqsub(b, 0, VX0, VX0, VX1); break;
    case OCERZ_OP_PSUBSW: a64_v_sqsub(b, 1, VX0, VX0, VX1); break;
    case OCERZ_OP_PSUBUSB: a64_v_uqsub(b, 0, VX0, VX0, VX1); break;
    case OCERZ_OP_PSUBUSW: a64_v_uqsub(b, 1, VX0, VX0, VX1); break;
    case OCERZ_OP_PMULLW: a64_v_mul(b, 1, VX0, VX0, VX1); break;
    case OCERZ_OP_PMULHW:
        a64_v_smull_h(b, 0, VX0, VX0, VX1);
        a64_v_uzp(b, 1, 1, VX0, VX0, VX0);
        break;
    case OCERZ_OP_PMULHUW:
        a64_v_umull_h(b, 0, VX0, VX0, VX1);
        a64_v_uzp(b, 1, 1, VX0, VX0, VX0);
        break;
    case OCERZ_OP_PAND: a64_v_and(b, VX0, VX0, VX1); break;
    case OCERZ_OP_POR: a64_v_orr(b, VX0, VX0, VX1); break;
    case OCERZ_OP_PXOR: a64_v_eor(b, VX0, VX0, VX1); break;
    case OCERZ_OP_PANDN: a64_v_bic(b, VX0, VX1, VX0); break;
    case OCERZ_OP_PCMPEQB: a64_v_cmeq(b, 0, VX0, VX0, VX1); break;
    case OCERZ_OP_PCMPEQW: a64_v_cmeq(b, 1, VX0, VX0, VX1); break;
    case OCERZ_OP_PCMPEQD: a64_v_cmeq(b, 2, VX0, VX0, VX1); break;
    case OCERZ_OP_PCMPGTB: a64_v_cmgt(b, 0, VX0, VX0, VX1); break;
    case OCERZ_OP_PCMPGTW: a64_v_cmgt(b, 1, VX0, VX0, VX1); break;
    case OCERZ_OP_PCMPGTD: a64_v_cmgt(b, 2, VX0, VX0, VX1); break;
    case OCERZ_OP_PUNPCKLBW: a64_v_zip1(b, 0, VX0, VX0, VX1); break;
    case OCERZ_OP_PUNPCKLWD: a64_v_zip1(b, 1, VX0, VX0, VX1); break;
    case OCERZ_OP_PUNPCKLDQ: a64_v_zip1(b, 2, VX0, VX0, VX1); break;
    case OCERZ_OP_PUNPCKHBW: a64_v_zip1(b, 0, VX0, VX0, VX1); a64_v_dup_d(b, VX0, VX0, 1); break;
    case OCERZ_OP_PUNPCKHWD: a64_v_zip1(b, 1, VX0, VX0, VX1); a64_v_dup_d(b, VX0, VX0, 1); break;
    case OCERZ_OP_PUNPCKHDQ: a64_v_zip1(b, 2, VX0, VX0, VX1); a64_v_dup_d(b, VX0, VX0, 1); break;
    case OCERZ_OP_PACKUSWB: a64_v_zip1(b, 3, VX0, VX0, VX1); a64_v_sqxtun_h(b, 0, VX0, VX0); break;
    case OCERZ_OP_PACKSSWB: a64_v_zip1(b, 3, VX0, VX0, VX1); a64_v_sqxtn_h(b, 0, VX0, VX0); break;
    case OCERZ_OP_PACKSSDW: a64_v_zip1(b, 3, VX0, VX0, VX1); a64_v_sqxtn_s(b, 0, VX0, VX0); break;
    case OCERZ_OP_PSADBW:
        a64_v_uabd(b, 0, VX0, VX0, VX1);
        a64_v_uaddlp(b, 0, VX0, VX0);
        a64_v_uaddlp(b, 1, VX0, VX0);
        a64_v_uaddlp(b, 2, VX0, VX0);
        break;
    case OCERZ_OP_PAVGB: a64_v_urhadd(b, 0, VX0, VX0, VX1); break;
    case OCERZ_OP_PAVGW: a64_v_urhadd(b, 1, VX0, VX0, VX1); break;
    case OCERZ_OP_PMINUB: a64_v_umin(b, 0, VX0, VX0, VX1); break;
    case OCERZ_OP_PMAXUB: a64_v_umax(b, 0, VX0, VX0, VX1); break;
    case OCERZ_OP_PMINSW: a64_v_smin(b, 1, VX0, VX0, VX1); break;
    case OCERZ_OP_PMAXSW: a64_v_smax(b, 1, VX0, VX0, VX1); break;
    default: return 0;
    }
    a64_str_v(b, 8, VX0, 20, MMX_OFF(d->reg));
    mmx_enter(b);
    return 1;
}

int emit_sse(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    if (ocerz_insn_has_mmx(insn))
        return insn->mode32 ? 0 : emit_mmx(b, insn, exit_sites, n_exits);
    if (!sse_enabled()) return 0;
    if (insn->seg != OCERZ_SEG_NONE) return 0;
    switch (insn->op) {
    case OCERZ_OP_MOVUPS: case OCERZ_OP_MOVAPS: case OCERZ_OP_MOVDQA: case OCERZ_OP_MOVDQU:
        return emit_sse_mov128(b, insn, exit_sites, n_exits);
    case OCERZ_OP_MOVSS:  return emit_sse_movs(b, insn, 4, exit_sites, n_exits);
    case OCERZ_OP_MOVSDX: return emit_sse_movs(b, insn, 8, exit_sites, n_exits);
    case OCERZ_OP_MOVLPS: case OCERZ_OP_MOVHPS:
        return emit_sse_movlh(b, insn, exit_sites, n_exits);
    case OCERZ_OP_ADDSS: case OCERZ_OP_ADDSD: case OCERZ_OP_ADDPS: case OCERZ_OP_ADDPD:
    case OCERZ_OP_SUBSS: case OCERZ_OP_SUBSD: case OCERZ_OP_SUBPS: case OCERZ_OP_SUBPD:
    case OCERZ_OP_MULSS: case OCERZ_OP_MULSD: case OCERZ_OP_MULPS: case OCERZ_OP_MULPD:
    case OCERZ_OP_DIVSS: case OCERZ_OP_DIVSD: case OCERZ_OP_DIVPS: case OCERZ_OP_DIVPD:
    case OCERZ_OP_MAXSS: case OCERZ_OP_MAXSD: case OCERZ_OP_MINSS: case OCERZ_OP_MINSD:
    case OCERZ_OP_MAXPS: case OCERZ_OP_MAXPD: case OCERZ_OP_MINPS: case OCERZ_OP_MINPD:
    case OCERZ_OP_SQRTSS: case OCERZ_OP_SQRTSD: case OCERZ_OP_SQRTPS: case OCERZ_OP_SQRTPD:
        return emit_sse_fparith(b, insn, exit_sites, n_exits);
    case OCERZ_OP_PXOR: case OCERZ_OP_XORPS: case OCERZ_OP_PAND: case OCERZ_OP_ANDPS:
    case OCERZ_OP_POR: case OCERZ_OP_ORPS: case OCERZ_OP_PANDN: case OCERZ_OP_ANDNPS:
    case OCERZ_OP_PADDB: case OCERZ_OP_PADDW: case OCERZ_OP_PADDD: case OCERZ_OP_PADDQ:
    case OCERZ_OP_PSUBB: case OCERZ_OP_PSUBW: case OCERZ_OP_PSUBD: case OCERZ_OP_PSUBQ:
    case OCERZ_OP_PCMPEQB: case OCERZ_OP_PCMPEQW: case OCERZ_OP_PCMPEQD: case OCERZ_OP_PCMPEQQ:
    case OCERZ_OP_PCMPGTB: case OCERZ_OP_PCMPGTW: case OCERZ_OP_PCMPGTD: case OCERZ_OP_PCMPGTQ:
    case OCERZ_OP_PMINUB: case OCERZ_OP_PMINUW: case OCERZ_OP_PMINUD: case OCERZ_OP_PMAXUB: case OCERZ_OP_PMAXUW: case OCERZ_OP_PMAXUD:
    case OCERZ_OP_PMINSB: case OCERZ_OP_PMINSW: case OCERZ_OP_PMINSD: case OCERZ_OP_PMAXSB: case OCERZ_OP_PMAXSW: case OCERZ_OP_PMAXSD:
    case OCERZ_OP_PMULLW: case OCERZ_OP_PMULLD: case OCERZ_OP_PAVGB: case OCERZ_OP_PAVGW:
    case OCERZ_OP_PADDUSB: case OCERZ_OP_PADDUSW: case OCERZ_OP_PSUBUSB: case OCERZ_OP_PSUBUSW:
    case OCERZ_OP_PADDSB: case OCERZ_OP_PADDSW: case OCERZ_OP_PSUBSB: case OCERZ_OP_PSUBSW:
    case OCERZ_OP_PMADDWD: case OCERZ_OP_PMULHRSW: case OCERZ_OP_PACKSSDW: case OCERZ_OP_PACKUSWB:
    case OCERZ_OP_PMULUDQ: case OCERZ_OP_PMULHW: case OCERZ_OP_PMULHUW: case OCERZ_OP_PACKSSWB: case OCERZ_OP_PACKUSDW:
    case OCERZ_OP_PMULDQ: case OCERZ_OP_PSADBW: case OCERZ_OP_PMADDUBSW:
    case OCERZ_OP_PABSB: case OCERZ_OP_PABSW: case OCERZ_OP_PABSD: case OCERZ_OP_PSIGNB: case OCERZ_OP_PSIGNW: case OCERZ_OP_PSIGND:
    case OCERZ_OP_PHADDW: case OCERZ_OP_PHADDD: case OCERZ_OP_PHSUBW: case OCERZ_OP_PHSUBD: case OCERZ_OP_PHADDSW: case OCERZ_OP_PHSUBSW:
        return emit_sse_bitwise(b, insn, exit_sites, n_exits);
    case OCERZ_OP_PBLENDW: case OCERZ_OP_PALIGNR:
        return emit_sse_pblendw_palignr(b, insn, exit_sites, n_exits);
    case OCERZ_OP_PSHUFLW: case OCERZ_OP_PSHUFHW:
        return emit_sse_pshuflhw(b, insn, exit_sites, n_exits);
    case OCERZ_OP_PSLLDQ: case OCERZ_OP_PSRLDQ:
        return emit_sse_bytesh(b, insn);
    case OCERZ_OP_BLENDPS: case OCERZ_OP_BLENDPD:
        return emit_sse_blendp(b, insn, exit_sites, n_exits);
    case OCERZ_OP_MOVSHDUP: case OCERZ_OP_MOVSLDUP:
        return emit_sse_movsdup(b, insn, exit_sites, n_exits);
    case OCERZ_OP_MOVMSKPS: case OCERZ_OP_MOVMSKPD:
        return emit_sse_movmskp(b, insn);
    case OCERZ_OP_CMPPS: case OCERZ_OP_CMPPD:
        return emit_sse_cmpp(b, insn, exit_sites, n_exits);
    case OCERZ_OP_CVTTPS2DQ: case OCERZ_OP_CVTPS2DQ: case OCERZ_OP_CVTDQ2PD:
    case OCERZ_OP_CVTPS2PD: case OCERZ_OP_CVTPD2PS:
        return emit_sse_cvtp(b, insn, exit_sites, n_exits);
    case OCERZ_OP_AESENC: case OCERZ_OP_AESENCLAST: case OCERZ_OP_AESDEC: case OCERZ_OP_AESDECLAST:
    case OCERZ_OP_AESIMC: case OCERZ_OP_AESKEYGENASSIST:
        return emit_sse_aes(b, insn, exit_sites, n_exits);
    case OCERZ_OP_PCLMULQDQ:
        return emit_sse_pclmul(b, insn, exit_sites, n_exits);
    case OCERZ_OP_UCOMISS: case OCERZ_OP_UCOMISD: case OCERZ_OP_COMISS: case OCERZ_OP_COMISD:
        return emit_sse_comis(b, insn, exit_sites, n_exits);
    case OCERZ_OP_CVTTSD2SI: case OCERZ_OP_CVTTSS2SI: case OCERZ_OP_CVTSI2SD: case OCERZ_OP_CVTSI2SS:
    case OCERZ_OP_CVTSD2SS: case OCERZ_OP_CVTSS2SD: case OCERZ_OP_CVTDQ2PS:
        return emit_sse_cvt(b, insn, exit_sites, n_exits);
    case OCERZ_OP_MOVQX: {
        g_vec_int_move = 1;
        int r = emit_sse_movq(b, insn, exit_sites, n_exits);
        g_vec_int_move = 0;
        return r;
    }
    case OCERZ_OP_PSHUFD: return emit_sse_pshufd(b, insn, exit_sites, n_exits);
    case OCERZ_OP_PINSRB: case OCERZ_OP_PINSRW: case OCERZ_OP_PINSRD: case OCERZ_OP_PINSRQ:
    case OCERZ_OP_PEXTRB: case OCERZ_OP_PEXTRW: case OCERZ_OP_PEXTRD: case OCERZ_OP_PEXTRQ:
        return emit_sse_pinsr_pextr(b, insn, exit_sites, n_exits);
    case OCERZ_OP_PMOVSXBW: case OCERZ_OP_PMOVSXBD: case OCERZ_OP_PMOVSXBQ: case OCERZ_OP_PMOVSXWD: case OCERZ_OP_PMOVSXWQ: case OCERZ_OP_PMOVSXDQ:
    case OCERZ_OP_PMOVZXBW: case OCERZ_OP_PMOVZXBD: case OCERZ_OP_PMOVZXBQ: case OCERZ_OP_PMOVZXWD: case OCERZ_OP_PMOVZXWQ: case OCERZ_OP_PMOVZXDQ:
        return emit_sse_pmovx(b, insn, exit_sites, n_exits);
    case OCERZ_OP_ROUNDSS: case OCERZ_OP_ROUNDSD: case OCERZ_OP_ROUNDPS: case OCERZ_OP_ROUNDPD:
        return emit_sse_round(b, insn, exit_sites, n_exits);
    case OCERZ_OP_PSHUFB: return emit_sse_pshufb(b, insn, exit_sites, n_exits);
    case OCERZ_OP_PUNPCKLBW: case OCERZ_OP_PUNPCKLWD: case OCERZ_OP_PUNPCKLDQ: case OCERZ_OP_PUNPCKLQDQ:
    case OCERZ_OP_PUNPCKHBW: case OCERZ_OP_PUNPCKHWD: case OCERZ_OP_PUNPCKHDQ: case OCERZ_OP_PUNPCKHQDQ:
        return emit_sse_punpck(b, insn, exit_sites, n_exits);
    case OCERZ_OP_MOVD: {
        g_vec_int_move = 1;
        int r = emit_sse_movd(b, insn, exit_sites, n_exits);
        g_vec_int_move = 0;
        return r;
    }
    case OCERZ_OP_UNPCKLPD: case OCERZ_OP_UNPCKHPD: case OCERZ_OP_MOVLHPS: case OCERZ_OP_MOVHLPS:
    case OCERZ_OP_UNPCKLPS: case OCERZ_OP_UNPCKHPS:
        return emit_sse_unpck(b, insn, exit_sites, n_exits);
    case OCERZ_OP_CMPSS: case OCERZ_OP_CMPSDX:
        return emit_sse_cmps(b, insn, exit_sites, n_exits);
    case OCERZ_OP_BLENDVPD: case OCERZ_OP_BLENDVPS: case OCERZ_OP_PBLENDVB:
        return emit_sse_blendv(b, insn, exit_sites, n_exits);
    case OCERZ_OP_MOVDDUP:
        return emit_sse_movddup(b, insn, exit_sites, n_exits);
    case OCERZ_OP_SHUFPS: case OCERZ_OP_SHUFPD:
        return emit_sse_shufp(b, insn, exit_sites, n_exits);
    case OCERZ_OP_INSERTPS:
        return emit_sse_insertps(b, insn, exit_sites, n_exits);
    case OCERZ_OP_PSLLW: case OCERZ_OP_PSLLD: case OCERZ_OP_PSLLQ: case OCERZ_OP_PSRLW:
    case OCERZ_OP_PSRLD: case OCERZ_OP_PSRLQ: case OCERZ_OP_PSRAW: case OCERZ_OP_PSRAD:
        return emit_sse_shift_imm(b, insn);
    default:
        return 0;
    }
}

static int emit_sse_pinsr_pextr(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    if (insn->nops != 3 || !sse_enabled()) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    unsigned op = insn->op;
    int esize = (op == OCERZ_OP_PINSRB || op == OCERZ_OP_PEXTRB) ? 1 : (op == OCERZ_OP_PINSRW || op == OCERZ_OP_PEXTRW) ? 2 :
                (op == OCERZ_OP_PINSRD || op == OCERZ_OP_PEXTRD) ? 4 : 8;
    unsigned lanes = 16u / (unsigned)esize;
    unsigned idx = (unsigned)insn->ops[2].imm & (lanes - 1);
    if (op == OCERZ_OP_PINSRB || op == OCERZ_OP_PINSRW || op == OCERZ_OP_PINSRD || op == OCERZ_OP_PINSRQ) {
        if (d->kind != OCERZ_OPK_XMM || !xmm_is_pinned(d->reg)) return 0;
        l0_flush_reg(b, d->reg);
        l0_inval(d->reg);
        int vd = xmm_vreg(d->reg);
        if (s->kind == OCERZ_OPK_REG) {
            if (s->high8) return 0;
            emit_gpr_rd(b, 1, JT0, s->reg);
            a64_ins_gpr(b, esize, vd, (int)idx, JT0);
            return 1;
        }
        if (s->kind == OCERZ_OPK_MEM) {
            if (!emit_mem_load_any(b, insn, s, esize, JT0)) return 0;
            a64_ins_gpr(b, esize, vd, (int)idx, JT0);
            return 1;
        }
        return 0;
    }
    if (s->kind != OCERZ_OPK_XMM || !xmm_is_pinned(s->reg)) return 0;
    int vs = xmm_vreg(s->reg);
    if (d->kind == OCERZ_OPK_REG) {
        if (d->high8) return 0;
        a64_umov_gpr(b, esize, JT0, vs, (int)idx);
        emit_gpr_wr(b, JT0, d->reg);
        return 1;
    }
    if (d->kind == OCERZ_OPK_MEM) {
        a64_umov_gpr(b, esize, JT0, vs, (int)idx);
        if (emit_plain_mem_fast(b, insn, d, esize, JT0, 1, 0)) return 1;
        uint32_t *skip;
        if (!emit_sse_mem_addr(b, insn, d, esize, exit_sites, n_exits, &skip)) return 0;
        a64_umov_gpr(b, esize, JT1, vs, (int)idx);
        emit_sse_mem_st_gpr(b, esize, JT1);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    return 0;
}

static int emit_sse_pmovx(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    if (insn->nops != 2 || !sse_enabled()) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM || !xmm_is_pinned(d->reg)) return 0;
    unsigned op = insn->op;
    int sx = op == OCERZ_OP_PMOVSXBW || op == OCERZ_OP_PMOVSXBD || op == OCERZ_OP_PMOVSXBQ ||
             op == OCERZ_OP_PMOVSXWD || op == OCERZ_OP_PMOVSXWQ || op == OCERZ_OP_PMOVSXDQ;
    int from, steps, srcw;
    switch (op) {
    case OCERZ_OP_PMOVSXBW: case OCERZ_OP_PMOVZXBW: from = 1; steps = 1; srcw = 8; break;
    case OCERZ_OP_PMOVSXBD: case OCERZ_OP_PMOVZXBD: from = 1; steps = 2; srcw = 4; break;
    case OCERZ_OP_PMOVSXBQ: case OCERZ_OP_PMOVZXBQ: from = 1; steps = 3; srcw = 2; break;
    case OCERZ_OP_PMOVSXWD: case OCERZ_OP_PMOVZXWD: from = 2; steps = 1; srcw = 8; break;
    case OCERZ_OP_PMOVSXWQ: case OCERZ_OP_PMOVZXWQ: from = 2; steps = 2; srcw = 4; break;
    case OCERZ_OP_PMOVSXDQ: case OCERZ_OP_PMOVZXDQ: from = 4; steps = 1; srcw = 8; break;
    default: return 0;
    }
    int vsrc;
    if (s->kind == OCERZ_OPK_XMM) {
        if (!xmm_is_pinned(s->reg)) return 0;
        vsrc = xmm_vreg(s->reg);
    } else if (s->kind == OCERZ_OPK_MEM) {
        uint32_t *skip;
        if (srcw == 2) {
            if (!emit_mem_load_any(b, insn, s, 2, JT0)) return 0;
            a64_fmov_v_from_x(b, 0, VX1, JT0);
        } else {
            if (!emit_sse_mem_addr(b, insn, s, srcw, exit_sites, n_exits, &skip)) return 0;
            emit_sse_mem_ld(b, srcw, VX1);
            patch_guard_skip(skip, a64_label(b));
        }
        vsrc = VX1;
    } else return 0;
    int vd = xmm_vreg(d->reg);
    int cur = vsrc, e = from;
    for (int k = 0; k < steps; k++) {
        a64_v_xtl(b, sx, e, vd, cur);
        cur = vd; e *= 2;
    }
    return 1;
}

static int emit_sse_round(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    if (insn->nops != 3 || !sse_enabled()) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM || !xmm_is_pinned(d->reg)) return 0;
    unsigned op = insn->op;
    unsigned imm = (unsigned)insn->ops[2].imm;
    int mode = (imm & 4) ? 4 : (int)(imm & 3);
    int mode_a64 = mode == 0 ? 0 : mode == 1 ? 1 : mode == 2 ? 2 : mode == 3 ? 3 : 4;
    int dbl = op == OCERZ_OP_ROUNDSD || op == OCERZ_OP_ROUNDPD;
    int packed = op == OCERZ_OP_ROUNDPS || op == OCERZ_OP_ROUNDPD;
    int vd = xmm_vreg(d->reg);
    if (packed) {
        int vs = emit_sse_src_reg(b, insn, s, 16, VX1, exit_sites, n_exits);
        if (vs < 0) return 0;
        a64_v_frint(b, dbl, mode_a64, vd, vs);
        return 1;
    }
    int esz = dbl ? 8 : 4;
    int vs;
    if (s->kind == OCERZ_OPK_XMM && xmm_is_pinned(s->reg)) vs = xmm_vreg(s->reg);
    else { if (!emit_sse_src(b, insn, s, esz, VX1, exit_sites, n_exits)) return 0; vs = VX1; }
    a64_frint_s(b, dbl, mode_a64, VX0, vs);
    if (dbl) a64_ins_d_d(b, vd, 0, VX0, 0); else a64_ins_s_s(b, vd, 0, VX0, 0);
    return 1;
}

static void emit_mskb_bits(A64Buf *b, int vt)
{
    g_raslit[g_n_raslit].site = a64_label(b);
    g_raslit[g_n_raslit].retaddr = 0x8040201008040201ull;
    g_raslit[g_n_raslit].hi = 0x8040201008040201ull;
    g_raslit[g_n_raslit].kind = 2;
    g_raslit[g_n_raslit].rt = vt;
    g_n_raslit++;
    a64_emit32(b, 0x9c000000u | (uint32_t)vt);
}

int emit_pmovmskb(A64Buf *b, const X86Insn *insn)
{
    if (!sse_enabled() || insn->nops != 2) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
    if (s->kind != OCERZ_OPK_XMM || !xmm_is_pinned(s->reg)) return 0;
    if (g_n_raslit >= RASLIT_MAX) return 0;
    int ds = pin_slot(d->reg);
    if (ds < 0 || (rsp_is_ptr() && d->reg == OCERZ_RSP)) return 0;
    a64_v_sshr_16b(b, VX0, xmm_vreg(s->reg), 7);
    emit_mskb_bits(b, VX1);
    a64_v_and(b, VX0, VX0, VX1);
    a64_v_addp_16b(b, VX0, VX0, VX0);
    a64_v_addp_16b(b, VX0, VX0, VX0);
    a64_v_addp_16b(b, VX0, VX0, VX0);
    a64_umov_w_h(b, pin_hreg(ds), VX0, 0);
    return 1;
}

static int vex_inline_enabled(void)
{
    static int on = -1;
    if (on < 0) on = getenv("OCERZ_NO_INLINE_VEX") ? 0 : 1;
    return on;
}

uint16_t g_ymmh_zero;

static int ymmh_src(A64Buf *b, unsigned xr, int vtmp)
{
    if (g_yc[xr] >= 0) return g_yc[xr];
    if (g_ymmh_zero & (1u << xr)) { a64_v_zero(b, vtmp); return vtmp; }
    a64_ldr_v(b, 16, vtmp, 20, YMMH_OFF + xr * 16);
    return vtmp;
}

static inline int ymmh_dst(unsigned xr, int vtmp) { return g_yc[xr] >= 0 ? g_yc[xr] : vtmp; }

static void emit_ymmh_ld(A64Buf *b, int vd, unsigned xr)
{
    int v = ymmh_src(b, xr, vd);
    if (v != vd) a64_v_mov(b, vd, v);
}

static void emit_ymmh_st(A64Buf *b, int vs, unsigned xr)
{
    g_ymmh_zero &= (uint16_t)~(1u << xr);
    if (g_yc[xr] >= 0) {
        if (vs != g_yc[xr]) a64_v_mov(b, g_yc[xr], vs);
        g_yc_dirty |= (uint16_t)(1u << xr);
        return;
    }
    a64_str_v(b, 16, vs, 20, YMMH_OFF + xr * 16);
}

void emit_ymmh_clear(A64Buf *b, unsigned xr)
{
    if (g_ymmh_zero & (1u << xr)) return;
    if (g_yc[xr] >= 0) {
        a64_v_zero(b, g_yc[xr]);
        g_yc_dirty |= (uint16_t)(1u << xr);
    } else if (g_zero_vreg >= 0) {
        a64_str_v(b, 16, g_zero_vreg, 20, YMMH_OFF + xr * 16);
    } else {
        a64_str(b, 8, A64_ZR, 20, YMMH_OFF + xr * 16);
        a64_str(b, 8, A64_ZR, 20, YMMH_OFF + xr * 16 + 8);
    }
    g_ymmh_zero |= (uint16_t)(1u << xr);
}

static uint32_t *g_vex_mem_skip;

static int emit_vex_mem_addr(A64Buf *b, const X86Insn *insn, const X86Operand *m,
                             uint32_t **exit_sites, int *n_exits)
{
    if (!emit_sse_mem_addr(b, insn, m, 16, exit_sites, n_exits, &g_vex_mem_skip)) return 0;
    int32_t dsp = (int32_t)g_sse_mem_disp, hi = dsp + 16;
    int plain = g_sse_mem_plainacc || vec_tso_relaxed();
    int lo_ok = (dsp >= 0 && (dsp & 15) == 0 && dsp / 16 <= 4095) || (dsp >= -256 && dsp <= 255);
    int hi_ok = (hi >= 0 && (hi & 15) == 0 && hi / 16 <= 4095) || (hi >= -256 && hi <= 255);
    if (!lo_ok || !hi_ok || (!plain && dsp != 0)) {
        if (dsp > 0 && dsp <= 4095) a64_add_imm(b, 1, JTA, g_sse_mem_ra, (uint32_t)dsp);
        else if (dsp < 0 && -dsp <= 4095) a64_sub_imm(b, 1, JTA, g_sse_mem_ra, (uint32_t)-dsp);
        else { a64_mov_imm64(b, JTU, (uint64_t)(int64_t)dsp); a64_add_reg(b, 1, JTA, g_sse_mem_ra, JTU, 0); }
        ea_cache_reset();
        g_sse_mem_ra = JTA;
        g_sse_mem_disp = 0;
    }
    return 1;
}

static void emit_vex_mem_acc(A64Buf *b, int v, uint32_t off, int store)
{
    int32_t disp = (int32_t)(g_sse_mem_disp + off);
    if (store) emit_v_st_at(b, 16, v, g_sse_mem_ra, disp, g_sse_mem_plainacc);
    else emit_v_ld_at(b, 16, v, g_sse_mem_ra, disp, g_sse_mem_plainacc);
}

static void emit_vex_mem_done(A64Buf *b)
{
    patch_guard_skip(g_vex_mem_skip, a64_label(b));
    g_vex_mem_skip = NULL;
}

static int emit_vex_ld128(A64Buf *b, const X86Insn *insn, const X86Operand *m, int size, int vd,
                          uint32_t **exit_sites, int *n_exits)
{
    if (size == 16 && emit_plain_mem_fast(b, insn, m, 16, vd, 0, 1)) return 1;
    uint32_t *skip;
    if (!emit_sse_mem_addr(b, insn, m, size, exit_sites, n_exits, &skip)) return 0;
    emit_sse_mem_ld(b, size, vd);
    patch_guard_skip(skip, a64_label(b));
    return 1;
}

static int emit_vex_mov(A64Buf *b, const X86Insn *insn, int L, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (insn->nops != 2 || (insn->vex & OCERZ_VEX_NDS)) return 0;
    if (d->kind == OCERZ_OPK_XMM) {
        if (!xmm_is_pinned(d->reg)) return 0;
        int vd = xmm_vreg(d->reg);
        if (s->kind == OCERZ_OPK_XMM) {
            if (!xmm_is_pinned(s->reg)) return 0;
            if (s->reg != d->reg) {
                a64_v_mov(b, vd, xmm_vreg(s->reg));
                if (L) emit_ymmh_st(b, ymmh_src(b, s->reg, VX0), d->reg);
            }
            if (!L) emit_ymmh_clear(b, d->reg);
            return 1;
        }
        if (s->kind != OCERZ_OPK_MEM) return 0;
        if (!L) {
            if (!emit_vex_ld128(b, insn, s, 16, vd, exit_sites, n_exits)) return 0;
            emit_ymmh_clear(b, d->reg);
            return 1;
        }
        if (!emit_vex_mem_addr(b, insn, s, exit_sites, n_exits)) return 0;
        int vh = ymmh_dst(d->reg, VX1);
        int32_t dl = (int32_t)g_sse_mem_disp;
        if ((g_sse_mem_plainacc || vec_tso_relaxed()) && dl % 16 == 0 && dl >= -1024 && dl <= 1008) {
            a64_ldp_q_off(b, VX0, vh, g_sse_mem_ra, dl);
        } else {
            emit_vex_mem_acc(b, VX0, 0, 0);
            emit_vex_mem_acc(b, vh, 16, 0);
        }
        a64_v_mov(b, vd, VX0);
        emit_vex_mem_done(b);
        emit_ymmh_st(b, vh, d->reg);
        return 1;
    }
    if (d->kind != OCERZ_OPK_MEM || s->kind != OCERZ_OPK_XMM || !xmm_is_pinned(s->reg)) return 0;
    int vs = xmm_vreg(s->reg);
    if (!L) {
        if (emit_plain_mem_fast(b, insn, d, 16, vs, 1, 1)) return 1;
        uint32_t *skip;
        if (!emit_sse_mem_addr(b, insn, d, 16, exit_sites, n_exits, &skip)) return 0;
        emit_sse_mem_st(b, 16, vs);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    if (!emit_vex_mem_addr(b, insn, d, exit_sites, n_exits)) return 0;
    int vh = ymmh_src(b, s->reg, VX1);
    emit_vex_mem_acc(b, vs, 0, 1);
    emit_vex_mem_acc(b, vh, 16, 1);
    emit_vex_mem_done(b);
    return 1;
}

static int emit_vex_int(A64Buf *b, const X86Insn *insn, int kind, int esz, int L,
                        uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (!(insn->vex & OCERZ_VEX_NDS) || insn->nops != 2 || d->kind != OCERZ_OPK_XMM) return 0;
    if (!xmm_is_pinned(d->reg) || !xmm_is_pinned(insn->vvvv)) return 0;
    if (s->kind == OCERZ_OPK_XMM ? !xmm_is_pinned(s->reg) : s->kind != OCERZ_OPK_MEM) return 0;
    int vd = xmm_vreg(d->reg), va = xmm_vreg(insn->vvvv), vb = VX0;
    if (s->kind == OCERZ_OPK_XMM && s->reg == insn->vvvv && sse_int_self_zero(kind)) {
        a64_v_zero(b, vd);
        emit_ymmh_clear(b, d->reg);
        return 1;
    }
    int vbh = VX1;
    if (s->kind == OCERZ_OPK_XMM) {
        vb = xmm_vreg(s->reg);
        if (L) vbh = ymmh_src(b, s->reg, VX1);
    } else if (L) {
        if (!emit_vex_mem_addr(b, insn, s, exit_sites, n_exits)) return 0;
        emit_vex_mem_acc(b, VX0, 0, 0);
        emit_vex_mem_acc(b, VX1, 16, 0);
        emit_vex_mem_done(b);
    } else if (!emit_vex_ld128(b, insn, s, 16, VX0, exit_sites, n_exits)) {
        return 0;
    }
    if (L) {
        int vah = ymmh_src(b, insn->vvvv, VX2), vdh = ymmh_dst(d->reg, VX2);
        emit_sse_int_op(b, kind, esz, vdh, vah, vbh);
        emit_sse_int_op(b, kind, esz, vd, va, vb);
        emit_ymmh_st(b, vdh, d->reg);
        return 1;
    }
    emit_sse_int_op(b, kind, esz, vd, va, vb);
    emit_ymmh_clear(b, d->reg);
    return 1;
}

static int emit_vex_pmovmskb(A64Buf *b, const X86Insn *insn, int L)
{
    if (!L) return emit_pmovmskb(b, insn);
    if (insn->nops != 2) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
    if (s->kind != OCERZ_OPK_XMM || !xmm_is_pinned(s->reg)) return 0;
    if (g_n_raslit >= RASLIT_MAX) return 0;
    int ds = pin_slot(d->reg);
    if (ds < 0 || (rsp_is_ptr() && d->reg == OCERZ_RSP)) return 0;
    emit_mskb_bits(b, VX1);
    emit_ymmh_ld(b, VX2, s->reg);
    a64_v_sshr_16b(b, VX0, xmm_vreg(s->reg), 7);
    a64_v_sshr_16b(b, VX2, VX2, 7);
    a64_v_and(b, VX0, VX0, VX1);
    a64_v_and(b, VX2, VX2, VX1);
    for (int i = 0; i < 3; i++) {
        a64_v_addp_16b(b, VX0, VX0, VX0);
        a64_v_addp_16b(b, VX2, VX2, VX2);
    }
    a64_umov_w_h(b, JT0, VX0, 0);
    a64_umov_w_h(b, JT1, VX2, 0);
    a64_orr_reg(b, 0, pin_hreg(ds), JT0, JT1, 16);
    return 1;
}

static int emit_vex_broadcast(A64Buf *b, const X86Insn *insn, int esz, int L,
                              uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (insn->nops != 2 || d->kind != OCERZ_OPK_XMM || !xmm_is_pinned(d->reg)) return 0;
    if (!L && (esz == 16 || insn->op == OCERZ_OP_VBROADCASTSD)) return 0;
    int vd = xmm_vreg(d->reg);
    if (s->kind == OCERZ_OPK_XMM) {
        if (esz == 16 || !xmm_is_pinned(s->reg)) return 0;
        int vs = xmm_vreg(s->reg);
        if (esz == 1) a64_v_dup_b(b, vd, vs, 0);
        else if (esz == 2) a64_v_dup_h(b, vd, vs, 0);
        else if (esz == 4) a64_v_dup_s(b, vd, vs, 0);
        else a64_v_dup_d(b, vd, vs, 0);
    } else if (s->kind != OCERZ_OPK_MEM) {
        return 0;
    } else if (esz == 16) {
        if (!emit_vex_ld128(b, insn, s, 16, vd, exit_sites, n_exits)) return 0;
    } else {
        uint32_t *skip;
        if (!emit_sse_mem_addr(b, insn, s, esz, exit_sites, n_exits, &skip)) return 0;
        emit_sse_mem_ld_gpr(b, esz, JT0);
        patch_guard_skip(skip, a64_label(b));
        a64_v_dup_gpr(b, esz, vd, JT0);
    }
    if (L) emit_ymmh_st(b, vd, d->reg);
    else emit_ymmh_clear(b, d->reg);
    return 1;
}

static int emit_vex_pmovx(A64Buf *b, const X86Insn *insn, int sgn, int from, int L,
                          uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (insn->nops != 2 || d->kind != OCERZ_OPK_XMM || !xmm_is_pinned(d->reg)) return 0;
    int vd = xmm_vreg(d->reg), vs = VX1;
    if (s->kind == OCERZ_OPK_XMM) {
        if (!xmm_is_pinned(s->reg)) return 0;
        vs = xmm_vreg(s->reg);
    } else if (s->kind != OCERZ_OPK_MEM || !emit_vex_ld128(b, insn, s, L ? 16 : 8, VX1, exit_sites, n_exits)) {
        return 0;
    }
    if (L) {
        a64_v_xtl2(b, sgn, from, VX0, vs);
        a64_v_xtl(b, sgn, from, vd, vs);
        emit_ymmh_st(b, VX0, d->reg);
    } else {
        a64_v_xtl(b, sgn, from, vd, vs);
        emit_ymmh_clear(b, d->reg);
    }
    return 1;
}

static int emit_vex_shift_imm(A64Buf *b, const X86Insn *insn, int kind, int esz, int L)
{
    if (!(insn->vex & OCERZ_VEX_NDD) || insn->nops != 3) return 0;
    const X86Operand *d = &insn->ops[0], *c = &insn->ops[1], *s = &insn->ops[2];
    if (d->kind != OCERZ_OPK_XMM || c->kind != OCERZ_OPK_IMM || s->kind != OCERZ_OPK_XMM) return 0;
    if (!xmm_is_pinned(d->reg) || !xmm_is_pinned(s->reg)) return 0;
    unsigned cnt = (unsigned)(c->imm & 0xff);
    if (L) {
        emit_ymmh_ld(b, VX1, s->reg);
        emit_simd_shift_imm(b, kind, esz, VX1, VX1, cnt);
    }
    emit_simd_shift_imm(b, kind, esz, xmm_vreg(d->reg), xmm_vreg(s->reg), cnt);
    if (L) emit_ymmh_st(b, VX1, d->reg);
    else emit_ymmh_clear(b, d->reg);
    return 1;
}

static int inexact_nan_env(void)
{
    static int v = -1;
    if (v < 0) v = getenv("OCERZ_INEXACT_NAN") ? 1 : 0;
    return v;
}

static int emit_vex_fp_op(A64Buf *b, int kind, int dbl, int vr, int va, int vb)
{
    switch (kind) {
    case 0: a64_v_fadd(b, dbl, vr, va, vb); return 1;
    case 1: a64_v_fsub(b, dbl, vr, va, vb); return 1;
    case 2: a64_v_fmul(b, dbl, vr, va, vb); return 1;
    case 3: a64_v_fdiv(b, dbl, vr, va, vb); return 1;
    case 4: a64_v_fcmgt(b, dbl, vr, va, vb); a64_v_bsl(b, vr, va, vb); return 0;
    case 5: a64_v_fcmgt(b, dbl, vr, vb, va); a64_v_bsl(b, vr, va, vb); return 0;
    default: a64_v_fsqrt(b, dbl, vr, vb); return 1;
    }
}

static void emit_vex_fp_lane(A64Buf *b, int kind, int dbl, int vr, int va, int vb, int t1)
{
    if (!emit_vex_fp_op(b, kind, dbl, vr, va, vb)) return;
    if (kind == 6) va = vb;
    if (inexact_nan_env() || g_fpb_fast || ocerz_afp()) return;
    emit_nan_fix_packed2(b, dbl, vr, va, vb, t1, t1);
}

static int emit_vex_cvtdq2ps256(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (insn->nops != 2 || d->kind != OCERZ_OPK_XMM || !xmm_is_pinned(d->reg)) return 0;
    if (s->kind == OCERZ_OPK_XMM ? !xmm_is_pinned(s->reg) : s->kind != OCERZ_OPK_MEM) return 0;
    int lo, hi;
    if (s->kind == OCERZ_OPK_MEM) {
        if (!emit_vex_mem_addr(b, insn, s, exit_sites, n_exits)) return 0;
        emit_vex_mem_acc(b, VX0, 0, 0);
        emit_vex_mem_acc(b, VX2, 16, 0);
        emit_vex_mem_done(b);
        lo = VX0; hi = VX2;
    } else {
        lo = xmm_vreg(s->reg);
        hi = ymmh_src(b, s->reg, VX2);
    }
    int dh = ymmh_dst(d->reg, VX3);
    a64_v_scvtf_4s(b, dh, hi);
    a64_v_scvtf_4s(b, xmm_vreg(d->reg), lo);
    emit_ymmh_st(b, dh, d->reg);
    return 1;
}

static int emit_vex_fp256(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    int kind, dbl = 0;
    switch (insn->op) {
    case OCERZ_OP_ADDPS: kind = 0; break;  case OCERZ_OP_ADDPD: kind = 0; dbl = 1; break;
    case OCERZ_OP_SUBPS: kind = 1; break;  case OCERZ_OP_SUBPD: kind = 1; dbl = 1; break;
    case OCERZ_OP_MULPS: kind = 2; break;  case OCERZ_OP_MULPD: kind = 2; dbl = 1; break;
    case OCERZ_OP_DIVPS: kind = 3; break;  case OCERZ_OP_DIVPD: kind = 3; dbl = 1; break;
    case OCERZ_OP_MAXPS: kind = 4; break;  case OCERZ_OP_MAXPD: kind = 4; dbl = 1; break;
    case OCERZ_OP_MINPS: kind = 5; break;  case OCERZ_OP_MINPD: kind = 5; dbl = 1; break;
    case OCERZ_OP_SQRTPS: kind = 6; break; case OCERZ_OP_SQRTPD: kind = 6; dbl = 1; break;
    default: return 0;
    }
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    int sq = kind == 6;
    if (insn->nops != 2 || d->kind != OCERZ_OPK_XMM || !xmm_is_pinned(d->reg)) return 0;
    if (!sq && (!(insn->vex & OCERZ_VEX_NDS) || !xmm_is_pinned(insn->vvvv))) return 0;
    if (s->kind == OCERZ_OPK_XMM ? !xmm_is_pinned(s->reg) : s->kind != OCERZ_OPK_MEM) return 0;
    int mem = s->kind == OCERZ_OPK_MEM, lb = VX0, sh = VX2, ah = VX1;
    if (mem) {
        if (!emit_vex_mem_addr(b, insn, s, exit_sites, n_exits)) return 0;
        emit_vex_mem_acc(b, VX0, 0, 0);
        emit_vex_mem_acc(b, VX2, 16, 0);
        emit_vex_mem_done(b);
    } else {
        sh = ymmh_src(b, s->reg, VX2);
        lb = xmm_vreg(s->reg);
    }
    if (!sq) ah = ymmh_src(b, insn->vvvv, VX1);
    int dh = ymmh_dst(d->reg, VX3);
    if (dh == ah || dh == sh) dh = VX3;
    int chk = emit_vex_fp_op(b, kind, dbl, dh, ah, sh) && !inexact_nan_env() && !ocerz_afp();
    emit_vex_fp_op(b, kind, dbl, VX1, sq ? lb : xmm_vreg(insn->vvvv), lb);
    if (!chk) {
        a64_v_mov(b, xmm_vreg(d->reg), VX1);
        emit_ymmh_st(b, dh, d->reg);
        return 1;
    }
    a64_v_fcmeq(b, dbl, VX0, VX1, VX1);
    a64_v_fcmeq(b, dbl, VX2, dh, dh);
    a64_v_and(b, VX0, VX0, VX2);
    a64_v_uminv_4s(b, VX0, VX0);
    a64_fmov_x_from_v(b, 0, JT0, VX0);
    uint32_t *site = a64_label(b);
    a64_cbz(b, 0, JT0, 0);
    a64_v_mov(b, xmm_vreg(d->reg), VX1);
    emit_ymmh_st(b, dh, d->reg);
    if (!oolslow_add(insn, &site, 1, a64_label(b))) {
        uint32_t *done = a64_label(b);
        a64_b(b, 0);
        patch_any_branch(site, a64_label(b));
        emit_slowcall_keep_lanes(b, insn, exit_sites, n_exits);
        a64_patch_b(done, a64_label(b));
    }
    return 1;
}

static int emit_vex_fp128_alias(A64Buf *b, const X86Insn *insn)
{
    int kind, dbl = 0, packed = 0;
    switch (insn->op) {
    case OCERZ_OP_ADDSS: kind = 0; break;  case OCERZ_OP_ADDSD: kind = 0; dbl = 1; break;
    case OCERZ_OP_SUBSS: kind = 1; break;  case OCERZ_OP_SUBSD: kind = 1; dbl = 1; break;
    case OCERZ_OP_MULSS: kind = 2; break;  case OCERZ_OP_MULSD: kind = 2; dbl = 1; break;
    case OCERZ_OP_DIVSS: kind = 3; break;  case OCERZ_OP_DIVSD: kind = 3; dbl = 1; break;
    case OCERZ_OP_MAXSS: kind = 4; break;  case OCERZ_OP_MAXSD: kind = 4; dbl = 1; break;
    case OCERZ_OP_MINSS: kind = 5; break;  case OCERZ_OP_MINSD: kind = 5; dbl = 1; break;
    case OCERZ_OP_SQRTSS: kind = 6; break; case OCERZ_OP_SQRTSD: kind = 6; dbl = 1; break;
    case OCERZ_OP_ADDPS: kind = 0; packed = 1; break; case OCERZ_OP_ADDPD: kind = 0; dbl = packed = 1; break;
    case OCERZ_OP_SUBPS: kind = 1; packed = 1; break; case OCERZ_OP_SUBPD: kind = 1; dbl = packed = 1; break;
    case OCERZ_OP_MULPS: kind = 2; packed = 1; break; case OCERZ_OP_MULPD: kind = 2; dbl = packed = 1; break;
    case OCERZ_OP_DIVPS: kind = 3; packed = 1; break; case OCERZ_OP_DIVPD: kind = 3; dbl = packed = 1; break;
    case OCERZ_OP_MAXPS: kind = 4; packed = 1; break; case OCERZ_OP_MAXPD: kind = 4; dbl = packed = 1; break;
    case OCERZ_OP_MINPS: kind = 5; packed = 1; break; case OCERZ_OP_MINPD: kind = 5; dbl = packed = 1; break;
    default: return 0;
    }
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (insn->nops != 2 || !(insn->vex & OCERZ_VEX_NDS) || d->kind != OCERZ_OPK_XMM || s->kind != OCERZ_OPK_XMM) return 0;
    if (s->reg != d->reg || insn->vvvv == d->reg || !xmm_is_pinned(d->reg) || !xmm_is_pinned(insn->vvvv)) return 0;
    int vd = xmm_vreg(d->reg);
    g_scalar_merge_next = 0;
    if (packed) {
        emit_vex_fp_lane(b, kind, dbl, VX2, xmm_vreg(insn->vvvv), xmm_vreg(s->reg), VX3);
        a64_v_mov(b, vd, VX2);
    } else {
        int va = l0_src(insn->vvvv, dbl), vb = l0_src(d->reg, dbl);
        int t = l0_enabled() ? l0_alloc(d->reg, dbl) : VX2;
        if (t < 0) t = VX2;
        int fa = va, fb = vb;
        if (t == va || t == vb) {
            int alias = t == va ? va : vb;
            if (dbl) a64_fmov_d_d(b, VX3, alias); else a64_fmov_s_s(b, VX3, alias);
            if (t == va) fa = VX3;
            if (t == vb) fb = VX3;
        }
        switch (kind) {
        case 0: a64_fadd_s(b, dbl, t, va, vb); break;
        case 1: a64_fsub_s(b, dbl, t, va, vb); break;
        case 2: a64_fmul_s(b, dbl, t, va, vb); break;
        case 3: a64_fdiv_s(b, dbl, t, va, vb); break;
        case 4: a64_fcmp(b, dbl, va, vb); a64_fcsel(b, dbl, t, va, vb, A64_GT); break;
        case 5: a64_fcmp(b, dbl, va, vb); a64_fcsel(b, dbl, t, va, vb, A64_MI); break;
        default: a64_fsqrt_s(b, dbl, t, vb); break;
        }
        if (!(inexact_nan_env() || g_fpb_fast || ocerz_afp())) {
            if (kind < 4) emit_nan_fix_scalar2(b, dbl, t, fa, fb);
            else if (kind == 6) emit_nan_fix_scalar2(b, dbl, t, fb, fb);
        }
        g_fcmp_self_idx = -1;
        a64_v_mov(b, vd, xmm_vreg(insn->vvvv));
        emit_xmm_st_lo(b, dbl ? 8 : 4, t, d->reg);
    }
    emit_ymmh_clear(b, d->reg);
    return 1;
}

static int emit_vex_cmps_alias(A64Buf *b, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    int dbl = insn->op == OCERZ_OP_CMPSDX, esz = dbl ? 8 : 4;
    unsigned pred = (unsigned)insn->ops[2].imm & 7;
    int va = l0_src(insn->vvvv, dbl), vb = l0_src(d->reg, dbl);
    emit_cmps_pred(b, dbl, pred, VX2, va, vb);
    a64_v_mov(b, xmm_vreg(d->reg), xmm_vreg(insn->vvvv));
    emit_xmm_st_lo(b, esz, VX2, d->reg);
    l0_inval(d->reg);
    emit_ymmh_clear(b, d->reg);
    return 1;
}

static void emit_blend_mask(A64Buf *b, unsigned op, int vd, int vm, int vz)
{
    if (op == OCERZ_OP_BLENDVPD) a64_v_sshr_2d(b, vd, vm, 63);
    else if (op == OCERZ_OP_BLENDVPS) a64_v_sshr_4s(b, vd, vm, 31);
    else { a64_v_zero(b, vz); a64_v_cmgt(b, 0, vd, vz, vm); }
}

static int emit_vex_blendv(A64Buf *b, const X86Insn *insn, int L, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1], *m = &insn->ops[2];
    if (insn->nops != 3 || !(insn->vex & OCERZ_VEX_IS4) || d->kind != OCERZ_OPK_XMM || m->kind != OCERZ_OPK_XMM) return 0;
    if (!xmm_is_pinned(d->reg) || !xmm_is_pinned(m->reg) || !xmm_is_pinned(insn->vvvv)) return 0;
    if (s->kind == OCERZ_OPK_XMM ? !xmm_is_pinned(s->reg) : s->kind != OCERZ_OPK_MEM) return 0;
    int vv = xmm_vreg(insn->vvvv), vm = xmm_vreg(m->reg), vd = xmm_vreg(d->reg), vs = VX0;
    if (s->kind == OCERZ_OPK_XMM) {
        vs = xmm_vreg(s->reg);
        if (L) emit_ymmh_ld(b, VX1, s->reg);
    } else if (L) {
        if (!emit_vex_mem_addr(b, insn, s, exit_sites, n_exits)) return 0;
        emit_vex_mem_acc(b, VX0, 0, 0);
        emit_vex_mem_acc(b, VX1, 16, 0);
        emit_vex_mem_done(b);
    } else if (!emit_vex_ld128(b, insn, s, 16, VX0, exit_sites, n_exits)) {
        return 0;
    }
    emit_blend_mask(b, insn->op, VX2, vm, VX3);
    a64_v_bsl(b, VX2, vs, vv);
    if (L) {
        emit_ymmh_ld(b, VX3, m->reg);
        emit_blend_mask(b, insn->op, VX3, VX3, VX0);
        emit_ymmh_ld(b, VX0, insn->vvvv);
        a64_v_bsl(b, VX3, VX1, VX0);
        a64_v_mov(b, vd, VX2);
        emit_ymmh_st(b, VX3, d->reg);
        return 1;
    }
    a64_v_mov(b, vd, VX2);
    emit_ymmh_clear(b, d->reg);
    return 1;
}

static int emit_vex_cmps_fused(A64Buf *b, const X86Insn *insn)
{
    int dbl = insn->op == OCERZ_OP_CMPSDX;
    int va = l0_src(insn->vvvv & 15, dbl), vb = l0_src(insn->ops[1].reg, dbl);
    emit_cmps_pred(b, dbl, (unsigned)insn->ops[2].imm & 7, VX2, va, vb);
    g_cmps_mask_idx = g_cur_insn_idx;
    l0_inval(insn->ops[0].reg);
    return 1;
}

static int emit_vex_blendv_fused(A64Buf *b, const X86Insn *insn, const X86Insn *c, uint32_t **exit_sites, int *n_exits)
{
    int dbl = insn->op == OCERZ_OP_BLENDVPD;
    unsigned m = c->ops[0].reg, cv = c->vvvv & 15, d = insn->ops[0].reg, s1 = insn->vvvv & 15, s2 = insn->ops[1].reg;
    int have = g_cmps_mask_idx == g_cur_insn_idx - 1;
    g_cmps_mask_idx = -1;
    if (!have) {
        l0_flush_reg(b, s1); l0_flush_reg(b, s2); l0_flush_reg(b, d); l0_flush_reg(b, m);
        l0_inval(d);
        return emit_vex_blendv(b, insn, 0, exit_sites, n_exits);
    }
    if (m != d) {
        if (m != cv) a64_v_mov(b, xmm_vreg(m), xmm_vreg(cv));
        if (dbl) a64_ins_d_d(b, xmm_vreg(m), 0, VX2, 0); else a64_ins_s_s(b, xmm_vreg(m), 0, VX2, 0);
        emit_ymmh_clear(b, m);
    }
    int vd_old = l0_src(d, dbl);
    int t = l0_alloc(d, dbl);
    if (t < 0) {
        l0_flush_reg(b, s1); l0_flush_reg(b, s2);
        if (m == d) {
            if (d != cv) a64_v_mov(b, xmm_vreg(d), xmm_vreg(cv));
            if (dbl) a64_ins_d_d(b, xmm_vreg(d), 0, VX2, 0); else a64_ins_s_s(b, xmm_vreg(d), 0, VX2, 0);
        }
        return emit_vex_blendv(b, insn, 0, exit_sites, n_exits);
    }
    int v1 = s1 == d ? vd_old : l0_src(s1, dbl), v2 = s2 == d ? vd_old : l0_src(s2, dbl);
    if (v2 == t) {
        a64_v_bif(b, t, v1, VX2);
    } else {
        if (v1 != t) { if (dbl) a64_fmov_d_d(b, t, v1); else a64_fmov_s_s(b, t, v1); }
        a64_v_bit(b, t, v2, VX2);
    }
    if (dbl) a64_v_sshr_2d(b, VX3, xmm_vreg(cv), 63); else a64_v_sshr_4s(b, VX3, xmm_vreg(cv), 31);
    a64_v_bsl(b, VX3, xmm_vreg(s2), xmm_vreg(s1));
    a64_v_mov(b, xmm_vreg(d), VX3);
    g_l0_dirty |= (uint16_t)(1u << d);
    emit_ymmh_clear(b, d);
    return 1;
}

static int bmi_src(A64Buf *b, const X86Insn *insn, const X86Operand *o, int size, int tmp)
{
    if (o->kind == OCERZ_OPK_REG) {
        if (o->high8 || o->size != size || pin_slot(o->reg) < 0 || (rsp_is_ptr() && o->reg == OCERZ_RSP)) return -1;
        return pin_hreg(pin_slot(o->reg));
    }
    if (o->kind == OCERZ_OPK_MEM && emit_mem_load_any(b, insn, o, size, tmp)) return tmp;
    return -1;
}

static int emit_bmi(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    if (insn->mode32 || insn->seg != OCERZ_SEG_NONE || insn->nops < 2) return 0;
    const X86Operand *d = &insn->ops[0];
    int size = d->size, sf = size == 8;
    if (size != 4 && size != 8) return 0;
    if (d->kind != OCERZ_OPK_REG || d->high8 || pin_slot(d->reg) < 0 || (rsp_is_ptr() && d->reg == OCERZ_RSP)) return 0;
    int rd = pin_hreg(pin_slot(d->reg));
    unsigned bits = (unsigned)size * 8;
    switch (insn->op) {
    case OCERZ_OP_RORX: {
        if (insn->nops != 3 || insn->ops[2].kind != OCERZ_OPK_IMM) return 0;
        int rs = bmi_src(b, insn, &insn->ops[1], size, JT0);
        if (rs < 0) return 0;
        unsigned c = (unsigned)insn->ops[2].imm & (bits - 1);
        if (c == 0) a64_mov_reg(b, sf, rd, rs);
        else a64_extr(b, sf, rd, rs, rs, (int)c);
        return 1;
    }
    case OCERZ_OP_SHLX: case OCERZ_OP_SHRX: case OCERZ_OP_SARX: {
        if (insn->nops != 3) return 0;
        int rs = bmi_src(b, insn, &insn->ops[1], size, JT0);
        int rc = bmi_src(b, insn, &insn->ops[2], size, JT1);
        if (rs < 0 || rc < 0) return 0;
        if (insn->op == OCERZ_OP_SHLX) a64_lslv(b, sf, rd, rs, rc);
        else if (insn->op == OCERZ_OP_SHRX) a64_lsrv(b, sf, rd, rs, rc);
        else a64_asrv(b, sf, rd, rs, rc);
        return 1;
    }
    case OCERZ_OP_ANDN: {
        if (insn->nops != 3 || (need && !g_defer)) return 0;
        int r1 = bmi_src(b, insn, &insn->ops[1], size, JT0);
        int r2 = bmi_src(b, insn, &insn->ops[2], size, JT1);
        if (r1 < 0 || r2 < 0) return 0;
        a64_bic_reg(b, sf, rd, r2, r1, 0);
        if (need) emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_LOGIC, size, 0), rd, rd);
        return 1;
    }
    case OCERZ_OP_BLSR: case OCERZ_OP_BLSMSK: case OCERZ_OP_BLSI: {
        if (need) return 0;
        int rs = bmi_src(b, insn, &insn->ops[1], size, JT0);
        if (rs < 0) return 0;
        if (insn->op == OCERZ_OP_BLSI) a64_neg_reg(b, sf, JT1, rs);
        else a64_sub_imm(b, sf, JT1, rs, 1);
        if (insn->op == OCERZ_OP_BLSMSK) a64_eor_reg(b, sf, rd, rs, JT1, 0);
        else a64_and_reg(b, sf, rd, rs, JT1, 0);
        return 1;
    }
    case OCERZ_OP_BZHI: {
        if (need || insn->nops != 3) return 0;
        int rs = bmi_src(b, insn, &insn->ops[1], size, JT0);
        int rc = bmi_src(b, insn, &insn->ops[2], size, JT1);
        if (rs < 0 || rc < 0) return 0;
        a64_try_and_imm(b, 1, JT2, rc, 0xff);
        a64_mov_imm64(b, JT1, ~0ull);
        a64_lslv(b, sf, JT1, JT1, JT2);
        a64_bic_reg(b, sf, JT1, rs, JT1, 0);
        a64_subs_imm(b, 1, A64_ZR, JT2, bits);
        a64_csel(b, sf, rd, rs, JT1, A64_CS);
        return 1;
    }
    case OCERZ_OP_MULX: {
        if (insn->nops != 3 || pin_slot(OCERZ_RDX) < 0) return 0;
        const X86Operand *lo = &insn->ops[1];
        if (lo->kind != OCERZ_OPK_REG || lo->high8 || pin_slot(lo->reg) < 0 || (rsp_is_ptr() && lo->reg == OCERZ_RSP)) return 0;
        int rs = bmi_src(b, insn, &insn->ops[2], size, JT0);
        if (rs < 0) return 0;
        int rdx = pin_hreg(pin_slot(OCERZ_RDX)), rlo = pin_hreg(pin_slot(lo->reg));
        if (sf) {
            a64_umulh(b, JT1, rdx, rs);
            a64_mul(b, 1, JT2, rdx, rs);
            a64_mov_reg(b, 1, rlo, JT2);
            a64_mov_reg(b, 1, rd, JT1);
        } else {
            a64_mov_reg(b, 0, JT1, rdx);
            a64_mov_reg(b, 0, JT2, rs);
            a64_mul(b, 1, JT2, JT1, JT2);
            a64_mov_reg(b, 0, rlo, JT2);
            a64_lsr_imm(b, 1, rd, JT2, 32);
        }
        return 1;
    }
    default:
        return 0;
    }
}

static int emit_vex_shufp(A64Buf *b, const X86Insn *insn, int L, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    int dbl = insn->op == OCERZ_OP_SHUFPD;
    if (insn->nops != 3 || !(insn->vex & OCERZ_VEX_NDS) || d->kind != OCERZ_OPK_XMM || insn->ops[2].kind != OCERZ_OPK_IMM) return 0;
    if (!xmm_is_pinned(d->reg) || !xmm_is_pinned(insn->vvvv)) return 0;
    if (s->kind == OCERZ_OPK_XMM ? !xmm_is_pinned(s->reg) : s->kind != OCERZ_OPK_MEM) return 0;
    unsigned imm = (unsigned)insn->ops[2].imm;
    int vb = VX0;
    if (s->kind == OCERZ_OPK_XMM) {
        vb = xmm_vreg(s->reg);
        if (L) emit_ymmh_ld(b, VX3, s->reg);
    } else if (L) {
        if (!emit_vex_mem_addr(b, insn, s, exit_sites, n_exits)) return 0;
        emit_vex_mem_acc(b, VX0, 0, 0);
        emit_vex_mem_acc(b, VX3, 16, 0);
        emit_vex_mem_done(b);
    } else if (!emit_vex_ld128(b, insn, s, 16, VX0, exit_sites, n_exits)) {
        return 0;
    }
    if (L) {
        emit_ymmh_ld(b, VX1, insn->vvvv);
        emit_shufp_lane(b, dbl, dbl ? imm >> 2 : imm, VX2, VX1, VX3);
        emit_ymmh_st(b, VX2, d->reg);
    }
    emit_shufp_lane(b, dbl, imm, VX2, xmm_vreg(insn->vvvv), vb);
    a64_v_mov(b, xmm_vreg(d->reg), VX2);
    if (!L) emit_ymmh_clear(b, d->reg);
    return 1;
}

static int emit_vex_insertps(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0];
    if (!(insn->vex & OCERZ_VEX_NDS) || d->kind != OCERZ_OPK_XMM || !xmm_is_pinned(d->reg) || !xmm_is_pinned(insn->vvvv)) return 0;
    if (!emit_insertps_to(b, insn, xmm_vreg(insn->vvvv), exit_sites, n_exits)) return 0;
    a64_v_mov(b, xmm_vreg(d->reg), VX2);
    emit_ymmh_clear(b, d->reg);
    return 1;
}

static int emit_vex_movddup256(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (insn->nops != 2 || d->kind != OCERZ_OPK_XMM || !xmm_is_pinned(d->reg)) return 0;
    int vs = VX0;
    if (s->kind == OCERZ_OPK_XMM) {
        if (!xmm_is_pinned(s->reg)) return 0;
        emit_ymmh_ld(b, VX1, s->reg);
        vs = xmm_vreg(s->reg);
    } else if (s->kind == OCERZ_OPK_MEM) {
        if (!emit_vex_mem_addr(b, insn, s, exit_sites, n_exits)) return 0;
        emit_vex_mem_acc(b, VX0, 0, 0);
        emit_vex_mem_acc(b, VX1, 16, 0);
        emit_vex_mem_done(b);
    } else {
        return 0;
    }
    a64_v_dup_d(b, VX1, VX1, 0);
    a64_v_dup_d(b, xmm_vreg(d->reg), vs, 0);
    emit_ymmh_st(b, VX1, d->reg);
    return 1;
}

static int emit_vex_fma(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    int idx = insn->op - OCERZ_OP_VFMA_FIRST;
    int pd = idx & 1, kind = (idx >> 1) % 10, order = (idx >> 1) / 10;
    if (kind < 2) return 0;
    int scalar = kind & 1, negmul = kind >= 6, negadd = kind == 4 || kind == 5 || kind >= 8;
    int L = !scalar && (insn->vex & OCERZ_VEX_L) != 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (insn->nops != 2 || d->kind != OCERZ_OPK_XMM || !xmm_is_pinned(d->reg) || !xmm_is_pinned(insn->vvvv)) return 0;
    if (s->kind == OCERZ_OPK_XMM ? !xmm_is_pinned(s->reg) : s->kind != OCERZ_OPK_MEM) return 0;
    int mem = s->kind == OCERZ_OPK_MEM;
    if (mem && L) {
        if (!emit_vex_mem_addr(b, insn, s, exit_sites, n_exits)) return 0;
        emit_vex_mem_acc(b, VX0, 0, 0);
        emit_vex_mem_acc(b, VX3, 16, 0);
        emit_vex_mem_done(b);
    } else if (mem && !emit_vex_ld128(b, insn, s, scalar ? (pd ? 8 : 4) : 16, VX0, exit_sites, n_exits)) {
        return 0;
    }
    if (scalar) {
        int s1 = l0_src(d->reg, pd), s2 = l0_src(insn->vvvv, pd), s3 = mem ? VX0 : l0_src(s->reg, pd);
        int sa = order == 0 ? s1 : s2, sm = order == 1 ? s1 : s3, sc = order == 0 ? s2 : order == 1 ? s3 : s1;
        l0_flush_reg(b, d->reg);
        int t = l0_enabled() ? l0_alloc(d->reg, pd) : VX1;
        if (t < 0) t = VX1;
        a64_fmadd_s(b, pd, negmul, negadd, t, sa, sm, sc);
        if (inexact_nan_env() || g_fpb_fast) {
            emit_xmm_st_lo(b, pd ? 8 : 4, t, d->reg);
            emit_ymmh_clear(b, d->reg);
            return 1;
        }
        a64_fcmp(b, pd, t, t);
        uint32_t *ssite = a64_label(b);
        a64_bcond(b, A64_VS, 0);
        uint16_t dirty_before = g_l0_dirty;
        emit_xmm_st_lo(b, pd ? 8 : 4, t, d->reg);
        emit_ymmh_clear(b, d->reg);
        uint16_t dirty_after = g_l0_dirty;
        g_l0_dirty = dirty_before;
        if (!oolslow_add(insn, &ssite, 1, a64_label(b))) {
            uint32_t *done = a64_label(b);
            a64_b(b, 0);
            patch_any_branch(ssite, a64_label(b));
            emit_slowcall_keep_lanes(b, insn, exit_sites, n_exits);
            a64_patch_b(done, a64_label(b));
        }
        g_l0_dirty = dirty_after;
        return 1;
    }
    int o1 = xmm_vreg(d->reg), o2 = xmm_vreg(insn->vvvv), o3 = mem ? VX0 : xmm_vreg(s->reg);
    int a = order == 0 ? o1 : o2, m = order == 1 ? o1 : o3, c = order == 0 ? o2 : order == 1 ? o3 : o1;
    int ch = VX3, r = VX1, t = VX2;
    if (L) {
        if (!mem) emit_ymmh_ld(b, VX3, s->reg);
        emit_ymmh_ld(b, VX1, d->reg);
        emit_ymmh_ld(b, VX2, insn->vvvv);
        int ah = order == 0 ? VX1 : VX2, mh = order == 1 ? VX1 : VX3;
        ch = order == 0 ? VX2 : order == 1 ? VX3 : VX1;
        if (negadd) a64_v_fneg(b, pd, ch, ch);
        if (negmul) a64_v_fmls(b, pd, ch, ah, mh); else a64_v_fmla(b, pd, ch, ah, mh);
        r = ch == VX1 ? VX2 : VX1;
        t = ch == VX3 ? VX2 : VX3;
    }
    if (negadd) a64_v_fneg(b, pd, r, c); else a64_v_mov(b, r, c);
    if (negmul) a64_v_fmls(b, pd, r, a, m); else a64_v_fmla(b, pd, r, a, m);
    if (!L && (inexact_nan_env() || g_fpb_fast)) {
        a64_v_mov(b, o1, r);
        emit_ymmh_clear(b, d->reg);
        return 1;
    }
    uint32_t *site;
    a64_v_fcmeq(b, pd, t, r, r);
    if (L) {
        a64_v_fcmeq(b, pd, VX0, ch, ch);
        a64_v_and(b, t, t, VX0);
    }
    a64_v_uminv_4s(b, t, t);
    a64_fmov_x_from_v(b, 0, JT0, t);
    site = a64_label(b);
    a64_cbz(b, 0, JT0, 0);
    a64_v_mov(b, o1, r);
    if (L) emit_ymmh_st(b, ch, d->reg);
    else emit_ymmh_clear(b, d->reg);
    if (!oolslow_add(insn, &site, 1, a64_label(b))) {
        uint32_t *done = a64_label(b);
        a64_b(b, 0);
        patch_any_branch(site, a64_label(b));
        emit_slowcall_keep_lanes(b, insn, exit_sites, n_exits);
        a64_patch_b(done, a64_label(b));
    }
    return 1;
}

static int vex_sse128_ok(const X86Insn *insn, int L)
{
    switch (insn->op) {
    case OCERZ_OP_MOVSS: case OCERZ_OP_MOVSDX:
    case OCERZ_OP_ADDSS: case OCERZ_OP_ADDSD: case OCERZ_OP_SUBSS: case OCERZ_OP_SUBSD:
    case OCERZ_OP_MULSS: case OCERZ_OP_MULSD: case OCERZ_OP_DIVSS: case OCERZ_OP_DIVSD:
    case OCERZ_OP_MAXSS: case OCERZ_OP_MAXSD: case OCERZ_OP_MINSS: case OCERZ_OP_MINSD:
    case OCERZ_OP_SQRTSS: case OCERZ_OP_SQRTSD: case OCERZ_OP_ROUNDSS: case OCERZ_OP_ROUNDSD:
    case OCERZ_OP_UCOMISS: case OCERZ_OP_UCOMISD: case OCERZ_OP_COMISS: case OCERZ_OP_COMISD:
    case OCERZ_OP_CVTTSD2SI: case OCERZ_OP_CVTTSS2SI: case OCERZ_OP_CVTSI2SD: case OCERZ_OP_CVTSI2SS:
    case OCERZ_OP_CVTSD2SS: case OCERZ_OP_CVTSS2SD:
        return 1;
    case OCERZ_OP_CMPSS: case OCERZ_OP_CMPSDX:
        return insn->nops >= 3 && insn->ops[2].kind == OCERZ_OPK_IMM && (insn->ops[2].imm & 0x1f) < 8;
    case OCERZ_OP_MOVLPS: case OCERZ_OP_MOVHPS:
    case OCERZ_OP_ADDPS: case OCERZ_OP_ADDPD: case OCERZ_OP_SUBPS: case OCERZ_OP_SUBPD:
    case OCERZ_OP_MULPS: case OCERZ_OP_MULPD: case OCERZ_OP_DIVPS: case OCERZ_OP_DIVPD:
    case OCERZ_OP_MAXPS: case OCERZ_OP_MAXPD: case OCERZ_OP_MINPS: case OCERZ_OP_MINPD:
    case OCERZ_OP_SQRTPS: case OCERZ_OP_SQRTPD: case OCERZ_OP_CVTDQ2PS:
    case OCERZ_OP_MOVD: case OCERZ_OP_MOVQX: case OCERZ_OP_PSHUFD: case OCERZ_OP_PSHUFB: case OCERZ_OP_MOVDDUP:
    case OCERZ_OP_PINSRB: case OCERZ_OP_PINSRW: case OCERZ_OP_PINSRD: case OCERZ_OP_PINSRQ:
    case OCERZ_OP_PEXTRB: case OCERZ_OP_PEXTRW: case OCERZ_OP_PEXTRD: case OCERZ_OP_PEXTRQ:
    case OCERZ_OP_PMOVSXBD: case OCERZ_OP_PMOVSXBQ: case OCERZ_OP_PMOVSXWQ:
    case OCERZ_OP_PMOVZXBD: case OCERZ_OP_PMOVZXBQ: case OCERZ_OP_PMOVZXWQ:
    case OCERZ_OP_ROUNDPS: case OCERZ_OP_ROUNDPD:
    case OCERZ_OP_PUNPCKLBW: case OCERZ_OP_PUNPCKLWD: case OCERZ_OP_PUNPCKLDQ: case OCERZ_OP_PUNPCKLQDQ:
    case OCERZ_OP_PUNPCKHBW: case OCERZ_OP_PUNPCKHWD: case OCERZ_OP_PUNPCKHDQ: case OCERZ_OP_PUNPCKHQDQ:
    case OCERZ_OP_UNPCKLPD: case OCERZ_OP_UNPCKHPD: case OCERZ_OP_UNPCKLPS: case OCERZ_OP_UNPCKHPS:
    case OCERZ_OP_MOVLHPS: case OCERZ_OP_MOVHLPS:
        return !L;
    default:
        return 0;
    }
}

static int emit_vex_sse128(A64Buf *b, const X86Insn *insn, int L, uint32_t **exit_sites, int *n_exits)
{
    if (!vex_sse128_ok(insn, L)) return 0;
    const X86Operand *d = &insn->ops[0];
    int wx = d->kind == OCERZ_OPK_XMM && insn->op != OCERZ_OP_UCOMISS && insn->op != OCERZ_OP_UCOMISD &&
             insn->op != OCERZ_OP_COMISS && insn->op != OCERZ_OP_COMISD;
    if (wx && !xmm_is_pinned(d->reg)) return 0;
    if (insn->vex & OCERZ_VEX_NDS) {
        if (!wx || !xmm_is_pinned(insn->vvvv)) return 0;
        if (insn->vvvv != d->reg) {
            for (int k = 1; k < insn->nops; k++)
                if (insn->ops[k].kind == OCERZ_OPK_XMM && insn->ops[k].reg == d->reg)
                    return (insn->op == OCERZ_OP_CMPSS || insn->op == OCERZ_OP_CMPSDX) ? emit_vex_cmps_alias(b, insn)
                                                                                     : emit_vex_fp128_alias(b, insn);
            if (vex_lane_aware(insn)) l0_inval(d->reg);
            a64_v_mov(b, xmm_vreg(d->reg), xmm_vreg(insn->vvvv));
            if (vex_lane_aware(insn)) l0_share(d->reg, insn->vvvv);
        }
    }
    if (!emit_sse(b, insn, exit_sites, n_exits)) return 0;
    if (wx) emit_ymmh_clear(b, d->reg);
    return 1;
}

int emit_vex(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    if (!vex_inline_enabled() || !sse_enabled() || insn->mode32 || insn->seg != OCERZ_SEG_NONE) return 0;
    if (g_cur_insns && insn >= g_cur_insns && insn < g_cur_insns + g_cur_insns_n) {
        if ((insn->op == OCERZ_OP_CMPSS || insn->op == OCERZ_OP_CMPSDX) && insn + 1 < g_cur_insns + g_cur_insns_n &&
            vex_cmps_blendv_pair(insn, insn + 1))
            return emit_vex_cmps_fused(b, insn);
        if ((insn->op == OCERZ_OP_BLENDVPD || insn->op == OCERZ_OP_BLENDVPS) && insn > g_cur_insns &&
            vex_cmps_blendv_pair(insn - 1, insn))
            return emit_vex_blendv_fused(b, insn, insn - 1, exit_sites, n_exits);
    }
    int L = (insn->vex & OCERZ_VEX_L) != 0, esz;
    if (insn->op >= OCERZ_OP_VFMA_FIRST && insn->op <= OCERZ_OP_VFMA_LAST)
        return emit_vex_fma(b, insn, exit_sites, n_exits);
    int kind = sse_int_kind(insn->op, &esz);
    if (kind >= SIK_MADDWD) return 0;
    if (kind) return emit_vex_int(b, insn, kind, esz, L, exit_sites, n_exits);
    switch (insn->op) {
    case OCERZ_OP_ADDPS: case OCERZ_OP_ADDPD: case OCERZ_OP_SUBPS: case OCERZ_OP_SUBPD:
    case OCERZ_OP_MULPS: case OCERZ_OP_MULPD: case OCERZ_OP_DIVPS: case OCERZ_OP_DIVPD:
    case OCERZ_OP_MAXPS: case OCERZ_OP_MAXPD: case OCERZ_OP_MINPS: case OCERZ_OP_MINPD:
    case OCERZ_OP_SQRTPS: case OCERZ_OP_SQRTPD:
        if (L) return emit_vex_fp256(b, insn, exit_sites, n_exits);
        return emit_vex_sse128(b, insn, L, exit_sites, n_exits);
    case OCERZ_OP_MOVUPS: case OCERZ_OP_MOVAPS: case OCERZ_OP_MOVDQA: case OCERZ_OP_MOVDQU:
        return emit_vex_mov(b, insn, L, exit_sites, n_exits);
    case OCERZ_OP_PMOVMSKB: return emit_vex_pmovmskb(b, insn, L);
    case OCERZ_OP_VPBROADCASTB: return emit_vex_broadcast(b, insn, 1, L, exit_sites, n_exits);
    case OCERZ_OP_VPBROADCASTW: return emit_vex_broadcast(b, insn, 2, L, exit_sites, n_exits);
    case OCERZ_OP_VPBROADCASTD: case OCERZ_OP_VBROADCASTSS: return emit_vex_broadcast(b, insn, 4, L, exit_sites, n_exits);
    case OCERZ_OP_VPBROADCASTQ: case OCERZ_OP_VBROADCASTSD: return emit_vex_broadcast(b, insn, 8, L, exit_sites, n_exits);
    case OCERZ_OP_VBROADCASTI128: case OCERZ_OP_VBROADCASTF128: return emit_vex_broadcast(b, insn, 16, L, exit_sites, n_exits);
    case OCERZ_OP_CVTDQ2PS:
        if (L) return emit_vex_cvtdq2ps256(b, insn, exit_sites, n_exits);
        return emit_vex_sse128(b, insn, L, exit_sites, n_exits);
    case OCERZ_OP_VZEROUPPER: {
        static int noflag = -1;
        if (noflag < 0) noflag = getenv("OCERZ_NO_YMMH_FLAG") ? 1 : 0;
        uint32_t *skip = NULL;
        if (!noflag && !g_blk_ymm_write) {
            a64_ldr(b, 4, JT0, 20, YMMH_ALL_ZERO_OFF);
            skip = a64_label(b);
            a64_cbnz(b, 0, JT0, 0);
        }
        int vz = g_zero_vreg >= 0 ? g_zero_vreg : VX0;
        if (vz == VX0) a64_v_zero(b, VX0);
        for (unsigned r = 0; r < 16; r++) {
            if (g_yc[r] >= 0) { a64_v_zero(b, g_yc[r]); g_yc_dirty |= (uint16_t)(1u << r); }
            else a64_str_v(b, 16, vz, 20, YMMH_OFF + r * 16);
        }
        if (skip) {
            a64_mov_imm64(b, JT0, 1);
            a64_str(b, 4, JT0, 20, YMMH_ALL_ZERO_OFF);
            a64_patch_cbz(skip, a64_label(b));
        }
        g_ymmh_zero = 0xffff;
        return 1;
    }
    case OCERZ_OP_PMOVSXBW: return emit_vex_pmovx(b, insn, 1, 1, L, exit_sites, n_exits);
    case OCERZ_OP_PMOVSXWD: return emit_vex_pmovx(b, insn, 1, 2, L, exit_sites, n_exits);
    case OCERZ_OP_PMOVSXDQ: return emit_vex_pmovx(b, insn, 1, 4, L, exit_sites, n_exits);
    case OCERZ_OP_PMOVZXBW: return emit_vex_pmovx(b, insn, 0, 1, L, exit_sites, n_exits);
    case OCERZ_OP_PMOVZXWD: return emit_vex_pmovx(b, insn, 0, 2, L, exit_sites, n_exits);
    case OCERZ_OP_PMOVZXDQ: return emit_vex_pmovx(b, insn, 0, 4, L, exit_sites, n_exits);
    case OCERZ_OP_PSLLW: return emit_vex_shift_imm(b, insn, 0, 1, L);
    case OCERZ_OP_PSLLD: return emit_vex_shift_imm(b, insn, 0, 2, L);
    case OCERZ_OP_PSLLQ: return emit_vex_shift_imm(b, insn, 0, 3, L);
    case OCERZ_OP_PSRLW: return emit_vex_shift_imm(b, insn, 1, 1, L);
    case OCERZ_OP_PSRLD: return emit_vex_shift_imm(b, insn, 1, 2, L);
    case OCERZ_OP_PSRLQ: return emit_vex_shift_imm(b, insn, 1, 3, L);
    case OCERZ_OP_PSRAW: return emit_vex_shift_imm(b, insn, 2, 1, L);
    case OCERZ_OP_PSRAD: return emit_vex_shift_imm(b, insn, 2, 2, L);
    case OCERZ_OP_SHUFPS: case OCERZ_OP_SHUFPD: return emit_vex_shufp(b, insn, L, exit_sites, n_exits);
    case OCERZ_OP_BLENDVPD: case OCERZ_OP_BLENDVPS: case OCERZ_OP_PBLENDVB:
        return emit_vex_blendv(b, insn, L, exit_sites, n_exits);
    case OCERZ_OP_RORX: case OCERZ_OP_SHLX: case OCERZ_OP_SHRX: case OCERZ_OP_SARX: case OCERZ_OP_ANDN:
    case OCERZ_OP_BLSR: case OCERZ_OP_BLSMSK: case OCERZ_OP_BLSI: case OCERZ_OP_BZHI: case OCERZ_OP_MULX:
        return emit_bmi(b, insn, g_cur_need);
    case OCERZ_OP_INSERTPS: return L ? 0 : emit_vex_insertps(b, insn, exit_sites, n_exits);
    case OCERZ_OP_MOVDDUP:
        if (L) return emit_vex_movddup256(b, insn, exit_sites, n_exits);
        return emit_vex_sse128(b, insn, L, exit_sites, n_exits);
    default: return emit_vex_sse128(b, insn, L, exit_sites, n_exits);
    }
}

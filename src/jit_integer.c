/*
 * ---- register pinning ----
 * Pin class 3 keeps every guest GPR permanently in a host register (slot ==
 * guest register number): slots 0-7 in x21-x28 (callee-saved), 8-13 in x3-x8,
 * 14-15 in x1-x2 (caller-saved, spilled and reloaded around every C callout).
 * All blocks share that layout, so every transition is body-to-body: chaining
 * enters the callee's body directly, RAS entries are plain body pointers, and
 * no block boundary pays frame traffic.  xmm0-15 live in V16-V31 for the whole
 * body; only function entry/exit and C callouts touch memory.
 *
 * Class 3 also keeps guest rsp as guest_base + rsp - a host pointer, not a
 * value - so push/pop are single pre/post-indexed accesses and rsp-relative
 * addresses drop the base add.  Reads and writes of the rsp VALUE convert at
 * the accessors; spill/fill and fault recovery convert at the boundaries.
 * OCERZ_RSP_VALUE restores the old value-keeping class 3.  A read-modify-write
 * on an rsp-relative operand takes its address the same way.  It used to go to
 * the interpreter, which in MSVC-built code - locals addressed off rsp, and
 * lock or [rsp], 0 as the memory barrier - made add, cmp and or on memory the
 * hottest interpreted forms on R.E.P.O.'s main thread.
 *
 *
 * The slot is rsp - 8 plus the stack delta; rsp moves only once the store is done.
 *
 * In the Wine layout x0 carries nothing a 32-bit block needs (no guest base,
 * and the stack delta is for 64-bit stacks), so a 32-bit block keeps low_base
 * there, reloaded wherever the stack delta would be, and a 32-bit stack slot is
 * one register-offset access, [x0, w, uxtw], instead of a mov, an orr and the
 * access.  OCERZ_NO_M32_LOWREG=1 goes back to the orr.
 *
 * setcc r8 whose register a movzx of the same register widens a few
 * instructions on (setg al ; setl dl ; movzx edx, dl ; movzx eax, al): with
 * nothing between that reads or writes the register, touches memory or can
 * leave the block, the setcc writes the whole register (cset zero-extends)
 * and the movzx is skipped.  Returns the movzx's index, or -1.
 */
#include "ocerz/jit_internal.h"

static void emit_gpr_rd_sw(A64Buf *b, int dst, unsigned greg);
static int emit_load_operand(A64Buf *b, const X86Operand *op, int sf, int dst);
static int fuse_prev_mov(A64Buf *b, unsigned dreg, int size, int *hs_out,
                         const X86Insn *cur);
static int emit_arith_eager(A64Buf *b, const X86Insn *insn, uint64_t need);
static int emit_incdec_eager(A64Buf *b, const X86Insn *insn, uint64_t need);
static int emit_incdec_narrow(A64Buf *b, const X86Insn *insn, uint64_t need);
static int emit_shift_eager(A64Buf *b, const X86Insn *insn, uint64_t need);
static int imul_inline_enabled(void);
static int emit_imul_src(A64Buf *b, const X86Insn *insn, const X86Operand *op, int dst);
static int emit_push_pop32(A64Buf *b, const X86Insn *insn);
static int emit_push_pop_mem32(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_leave32(A64Buf *b, const X86Insn *insn);
static int emit_arith_mem_eager(A64Buf *b, const X86Insn *insn, uint64_t need,
                                uint32_t **exit_sites, int *n_exits);
static int emit_shift_count(A64Buf *b, const X86Insn *insn, int sf, int dst,
                            unsigned *const_cnt);
static int setcc_zx_partner(const X86Insn *insn);

int g_no_lazyflags;

int g_nzcv_want;

int g_nzcv_from = -1;

unsigned g_nzcv_kind;

int g_no_addincfuse;

int g_m32low;

int8_t *g_pin;

uint8_t *g_pin_hold;

int g_n_pinned;

int g_pin_class;

int g_defer;

int16_t g_mov_sink_at[JIT_MAX_BLOCK_INSNS];

uint8_t g_mov_skip[JIT_MAX_BLOCK_INSNS] = {0};

int fullpin_enabled(void)
{
    static int on = -1;
    if (on < 0) on = getenv("OCERZ_NO_FULLPIN") ? 0 : 1;
    return on;
}

static void emit_gpr_rd_sw(A64Buf *b, int dst, unsigned greg)
{
    int s = pin_slot(greg);
    if (s >= 0)
        a64_sxtw(b, dst, pin_hreg(s));
    else
        a64_ldrsw(b, dst, 20, GPR_OFF(greg));
}

void emit_pin_prologue(A64Buf *b)
{
    int ns = pin_saved_count();
    for (int d = 8; d < 16; d += 2)
        a64_stp_d_pre(b, d, d + 1, 31, -16);
    for (int i = 0; i < ns; i += 2)
        a64_stp_pre(b, 21 + i, 21 + i + 1, 31, -16);
    for (int i = 0; i < g_n_pinned; i++)
        a64_ldr(b, 8, pin_hreg(i), 20, GPR_OFF(g_pin_hold[i]));
    if (g_pin_class == 2) {
        a64_stp_pre(b, JRET_GUEST, JRET_HOST, 31, -16);
        a64_mov_imm64(b, JRET_HOST, 0);
    }
}

static int emit_load_operand(A64Buf *b, const X86Operand *op, int sf, int dst)
{
    if (op->kind == OCERZ_OPK_REG) {
        if (op->high8)
            return 0;
        emit_gpr_rd(b, sf, dst, op->reg);
        return 1;
    }
    if (op->kind == OCERZ_OPK_IMM) {
        uint64_t v = op->imm;
        if (!sf)
            v &= 0xffffffffull;
        a64_mov_imm64(b, dst, v);
        return 1;
    }
    return 0;
}

const uint32_t *g_cur_insn_start;

static int fuse_prev_mov(A64Buf *b, unsigned dreg, int size, int *hs_out,
                         const X86Insn *cur)
{
    static int dis = -1; if (dis < 0) dis = getenv("OCERZ_NO_MOVFUSE") ? 1 : 0;
    if (dis || !g_cur_insns_fwd() || g_cur_insn_idx < 1) return -1;
    if (cur)
        for (int oi = 1; oi < cur->nops; oi++)
            if (cur->ops[oi].kind == OCERZ_OPK_REG && !cur->ops[oi].high8 &&
                cur->ops[oi].reg == dreg)
                return -1;
    if (!g_cur_insn_start || b->p != g_cur_insn_start) return -1;
    const X86Insn *p = &g_cur_insns_fwd()[g_cur_insn_idx - 1];
    if (p->op != OCERZ_OP_MOV || p->nops != 2) return -1;
    const X86Operand *pd = &p->ops[0], *ps = &p->ops[1];
    if (pd->kind != OCERZ_OPK_REG || ps->kind != OCERZ_OPK_REG || pd->high8 || ps->high8) return -1;
    if (pd->reg != dreg || pd->size != size || ps->size != size || (size != 4 && size != 8)) return -1;
    if (ps->reg == dreg) return -1;
    if (pin_slot(dreg) < 0 || pin_slot(ps->reg) < 0) return -1;
    if (rsp_is_ptr() && (dreg == OCERZ_RSP || ps->reg == OCERZ_RSP)) return -1;
    int hd = pin_hreg(pin_slot(dreg)), hs = pin_hreg(pin_slot(ps->reg));
    uint32_t want = (size == 8 ? 0xaa0003e0u : 0x2a0003e0u) | ((uint32_t)hs << 16) | (uint32_t)hd;
    if (b->p <= b->start + 1 || b->p[-1] != want) return -1;
    b->p--;
    *hs_out = hs;
    return hd;
}

static int emit_arith_eager(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    if (s->size != d->size)
        return 0;
    int sf = d->size == 8;

    a64_ldr(b, sf ? 8 : 4, JT0, 20, GPR_OFF(d->reg));
    if (!emit_load_operand(b, s, sf, JT1))
        return 0;

    unsigned op = insn->op;
    int is_sub = (op == OCERZ_OP_SUB || op == OCERZ_OP_CMP);
    int is_add = (op == OCERZ_OP_ADD);
    int is_logic = (op == OCERZ_OP_AND || op == OCERZ_OP_OR ||
                    op == OCERZ_OP_XOR || op == OCERZ_OP_TEST);
    int writes = (op == OCERZ_OP_ADD || op == OCERZ_OP_SUB ||
                  op == OCERZ_OP_AND || op == OCERZ_OP_OR || op == OCERZ_OP_XOR);

    switch (op) {
    case OCERZ_OP_ADD: a64_adds_reg(b, sf, JT2, JT0, JT1, 0); break;
    case OCERZ_OP_SUB:
    case OCERZ_OP_CMP: a64_subs_reg(b, sf, JT2, JT0, JT1, 0); break;
    case OCERZ_OP_AND:
    case OCERZ_OP_TEST: a64_ands_reg(b, sf, JT2, JT0, JT1, 0); break;
    case OCERZ_OP_OR:  a64_orr_reg(b, sf, JT2, JT0, JT1, 0); break;
    case OCERZ_OP_XOR: a64_eor_reg(b, sf, JT2, JT0, JT1, 0); break;
    default: return 0;
    }
    if (need & (OCERZ_ZF | OCERZ_SF)) {
        if (is_logic && op != OCERZ_OP_AND && op != OCERZ_OP_TEST)
            a64_subs_imm(b, sf, A64_ZR, JT2, 0);
    }

    if (writes)
        a64_str(b, 8, JT2, 20, GPR_OFF(d->reg));

    if (need == 0)
        return 1;

    a64_mov_imm64(b, JTF, 0);
    emit_zf_sf(b, need);
    if (need & OCERZ_PF)
        emit_pf(b, JT2);
    if (is_add || is_sub) {
        if (need & OCERZ_CF) {
            a64_cset(b, JTT, is_add ? A64_CS : A64_CC);
            a64_lsl_imm(b, 0, JTT, JTT, 0);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
        if (need & OCERZ_OF) {
            a64_cset(b, JTT, A64_VS);
            a64_lsl_imm(b, 0, JTT, JTT, 11);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
        if (need & OCERZ_AF) {
            a64_eor_reg(b, 1, JTT, JT0, JT1, 0);
            a64_eor_reg(b, 1, JTT, JTT, JT2, 0);
            a64_ubfx(b, 1, JTT, JTT, 4, 1);
            a64_lsl_imm(b, 0, JTT, JTT, 4);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
    }
    emit_commit_flags(b, need);
    return 1;
}

int emit_arith(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    if (!g_defer)
        return emit_arith_eager(b, insn, need);
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    if (s->size != d->size)
        return 0;
    int sf = d->size == 8;
    unsigned op = insn->op;
    int is_sub = (op == OCERZ_OP_SUB || op == OCERZ_OP_CMP);
    int is_add = (op == OCERZ_OP_ADD);
    int is_logic = (op == OCERZ_OP_AND || op == OCERZ_OP_OR ||
                    op == OCERZ_OP_XOR || op == OCERZ_OP_TEST);
    int writes = (op == OCERZ_OP_ADD || op == OCERZ_OP_SUB ||
                  op == OCERZ_OP_AND || op == OCERZ_OP_OR || op == OCERZ_OP_XOR);
    (void)is_logic;

    if (rsp_is_ptr() && d->reg == OCERZ_RSP && sf && !need &&
        (op == OCERZ_OP_ADD || op == OCERZ_OP_SUB) &&
        s->kind == OCERZ_OPK_IMM && s->imm <= 4095 && pin_slot(OCERZ_RSP) >= 0) {
        int hs = pin_hreg(pin_slot(OCERZ_RSP));
        if (op == OCERZ_OP_ADD) a64_add_imm(b, 1, hs, hs, (uint32_t)s->imm);
        else                    a64_sub_imm(b, 1, hs, hs, (uint32_t)s->imm);
        return 1;
    }

    if (g_nzcv_want && pin_slot(d->reg) >= 0 &&
        !(rsp_is_ptr() && (d->reg == OCERZ_RSP || (s->kind == OCERZ_OPK_REG && s->reg == OCERZ_RSP))) &&
        (s->kind == OCERZ_OPK_IMM || (s->kind == OCERZ_OPK_REG && !s->high8 && pin_slot(s->reg) >= 0))) {
        int rd = pin_hreg(pin_slot(d->reg));
        int rm;
        uint64_t v = 0; int imm = s->kind == OCERZ_OPK_IMM;
        if (imm) { v = s->imm; if (!sf) v &= 0xffffffffull; }
        if (imm && (is_add || is_sub) && !need && v <= 4095) {
            int rdst = writes ? rd : A64_ZR;
            if (is_add) a64_adds_imm(b, sf, rdst, rd, (uint32_t)v);
            else        a64_subs_imm(b, sf, rdst, rd, (uint32_t)v);
            g_nzcv_kind = is_add ? OCERZ_CC_ADD : OCERZ_CC_SUB;
            g_nzcv_from = g_cur_insn_idx;
            return 1;
        }
        {
            uint64_t neg = (0ull - v) & (sf ? UINT64_MAX : 0xffffffffull);
            if (imm && (is_add || is_sub) && !need && neg >= 1 && neg <= 4095) {
                int rdst = writes ? rd : A64_ZR;
                if (is_add) a64_subs_imm(b, sf, rdst, rd, (uint32_t)neg);
                else        a64_adds_imm(b, sf, rdst, rd, (uint32_t)neg);
                g_nzcv_kind = is_add ? OCERZ_CC_ADD : OCERZ_CC_SUB;
                g_nzcv_from = g_cur_insn_idx;
                return 1;
            }
        }
        if (imm) { a64_mov_imm64(b, JT1, v); rm = JT1; }
        else rm = pin_hreg(pin_slot(s->reg));
        if (is_add || is_sub) {
            if (need) {
                if (sf) a64_stp_off(b, rd, rm, 20, CC_SRC_OFF);
                else { a64_mov_reg(b, 0, JT0, rd); a64_mov_reg(b, 0, JT2, rm); a64_stp_off(b, JT0, JT2, 20, CC_SRC_OFF); }
                a64_mov_imm64(b, JTT, ocerz_cc_pack(is_add ? OCERZ_CC_ADD : OCERZ_CC_SUB, d->size, 0));
                a64_str(b, 4, JTT, 20, CC_OP_OFF);
            }
            int rdst = writes ? rd : A64_ZR;
            if (is_add) a64_adds_reg(b, sf, rdst, rd, rm, 0);
            else        a64_subs_reg(b, sf, rdst, rd, rm, 0);
            g_nzcv_kind = is_add ? OCERZ_CC_ADD : OCERZ_CC_SUB;
        } else {
            switch (op) {
            case OCERZ_OP_TEST: a64_ands_reg(b, sf, JT2, rd, rm, 0); break;
            case OCERZ_OP_AND:  a64_ands_reg(b, sf, rd, rd, rm, 0); break;
            case OCERZ_OP_OR:   a64_orr_reg(b, sf, rd, rd, rm, 0); a64_ands_reg(b, sf, A64_ZR, rd, rd, 0); break;
            default:            a64_eor_reg(b, sf, rd, rd, rm, 0); a64_ands_reg(b, sf, A64_ZR, rd, rd, 0); break;
            }
            if (need) {
                int rr = op == OCERZ_OP_TEST ? JT2 : rd;
                emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_LOGIC, d->size, 0), rr, rr);
            }
            g_nzcv_kind = OCERZ_CC_LOGIC;
        }
        g_nzcv_from = g_cur_insn_idx;
        return 1;
    }

    if (!writes && need == 0)
        return 1;

    if (writes && need != 0 && pin_slot(d->reg) >= 0 && (is_add || is_sub || is_logic) &&
        !(rsp_is_ptr() && (d->reg == OCERZ_RSP || (s->kind == OCERZ_OPK_REG && s->reg == OCERZ_RSP))) &&
        (s->kind == OCERZ_OPK_IMM || (s->kind == OCERZ_OPK_REG && !s->high8))) {
        int rd = pin_hreg(pin_slot(d->reg));
        int rm;
        if (s->kind == OCERZ_OPK_REG) {
            int ss = pin_slot(s->reg);
            if (ss >= 0) rm = pin_hreg(ss);
            else { emit_gpr_rd(b, sf, JT1, s->reg); rm = JT1; }
        } else {
            uint64_t v = s->imm;
            if (!sf) v &= 0xffffffffull;
            if (is_logic) {
                int done = 0;
                switch (op) {
                case OCERZ_OP_AND: done = a64_try_and_imm(b, sf, rd, rd, v); break;
                case OCERZ_OP_OR:  done = a64_try_orr_imm(b, sf, rd, rd, v); break;
                default:           done = a64_try_eor_imm(b, sf, rd, rd, v); break;
                }
                if (done) { emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_LOGIC, d->size, 0), rd, rd); return 1; }
            } else if (is_add || is_sub) {
                if (v <= 4095 || ((v & 0xfff) == 0 && (v >> 12) <= 4095)) {
                    a64_mov_imm64(b, JT1, v);
                    if (sf) a64_stp_off(b, rd, JT1, 20, CC_SRC_OFF);
                    else { a64_mov_reg(b, 0, JT0, rd); a64_stp_off(b, JT0, JT1, 20, CC_SRC_OFF); }
                    a64_mov_imm64(b, JTT, ocerz_cc_pack(is_add ? OCERZ_CC_ADD : OCERZ_CC_SUB, d->size, 0));
                    a64_str(b, 4, JTT, 20, CC_OP_OFF);
                    if (v <= 4095) { if (is_add) a64_add_imm(b, sf, rd, rd, (uint32_t)v); else a64_sub_imm(b, sf, rd, rd, (uint32_t)v); }
                    else { if (is_add) a64_add_reg(b, sf, rd, rd, JT1, 0); else a64_sub_reg(b, sf, rd, rd, JT1, 0); }
                    return 1;
                }
            }
            a64_mov_imm64(b, JT1, v);
            rm = JT1;
        }
        if (is_add || is_sub) {
            if (sf) a64_stp_off(b, rd, rm, 20, CC_SRC_OFF);
            else { a64_mov_reg(b, 0, JT0, rd); a64_mov_reg(b, 0, JT2, rm); a64_stp_off(b, JT0, JT2, 20, CC_SRC_OFF); }
            a64_mov_imm64(b, JTT, ocerz_cc_pack(is_add ? OCERZ_CC_ADD : OCERZ_CC_SUB, d->size, 0));
            a64_str(b, 4, JTT, 20, CC_OP_OFF);
            if (is_add) a64_add_reg(b, sf, rd, rd, rm, 0);
            else        a64_sub_reg(b, sf, rd, rd, rm, 0);
            return 1;
        }
        switch (op) {
        case OCERZ_OP_AND: a64_and_reg(b, sf, rd, rd, rm, 0); break;
        case OCERZ_OP_OR:  a64_orr_reg(b, sf, rd, rd, rm, 0); break;
        default:           a64_eor_reg(b, sf, rd, rd, rm, 0); break;
        }
        emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_LOGIC, d->size, 0), rd, rd);
        return 1;
    }

    if (writes && need == 0) {
        int rsp_d = rsp_is_ptr() && d->reg == OCERZ_RSP;
        int rsp_s = rsp_is_ptr() && s->kind == OCERZ_OPK_REG && s->reg == OCERZ_RSP;
        if (rsp_d && (!sf || (op != OCERZ_OP_ADD && op != OCERZ_OP_SUB) || rsp_s))
            return 0;
        int ds = pin_slot(d->reg);
        int rd = ds >= 0 ? pin_hreg(ds) : JT2;
        int rn = ds >= 0 ? rd : JT0;
        int rm;

        if (ds >= 0 && !rsp_d && (s->kind == OCERZ_OPK_IMM || (s->kind == OCERZ_OPK_REG && !s->high8))) {
            int hs; if (fuse_prev_mov(b, d->reg, d->size, &hs, insn) >= 0) rn = hs;
        }
        if (ds < 0)
            emit_gpr_rd(b, sf, JT0, d->reg);
        if (s->kind == OCERZ_OPK_REG) {
            if (s->high8)
                return 0;
            int ss = pin_slot(s->reg);
            if (ss >= 0 && !rsp_s)
                rm = pin_hreg(ss);
            else {
                emit_gpr_rd(b, sf, JT1, s->reg);
                rm = JT1;
            }
        } else if (s->kind == OCERZ_OPK_IMM) {
            uint64_t v = s->imm;
            if (!sf)
                v &= 0xffffffffull;
            uint64_t width_mask = sf ? UINT64_MAX : 0xffffffffull;
            uint64_t neg = (0ull - v) & width_mask;
            int emitted = 0;
            switch (op) {
            case OCERZ_OP_ADD:
                if (v <= 4095) {
                    a64_add_imm(b, sf, rd, rn, (uint32_t)v);
                    emitted = 1;
                } else if (neg <= 4095) {
                    a64_sub_imm(b, sf, rd, rn, (uint32_t)neg);
                    emitted = 1;
                }
                break;
            case OCERZ_OP_SUB:
                if (v <= 4095) {
                    a64_sub_imm(b, sf, rd, rn, (uint32_t)v);
                    emitted = 1;
                } else if (neg <= 4095) {
                    a64_add_imm(b, sf, rd, rn, (uint32_t)neg);
                    emitted = 1;
                }
                break;
            case OCERZ_OP_AND:
                emitted = a64_try_and_imm(b, sf, rd, rn, v);
                break;
            case OCERZ_OP_OR:
                emitted = a64_try_orr_imm(b, sf, rd, rn, v);
                break;
            case OCERZ_OP_XOR:
                emitted = a64_try_eor_imm(b, sf, rd, rn, v);
                break;
            }
            if (emitted) {
                if (ds < 0)
                    emit_gpr_wr(b, rd, d->reg);
                return 1;
            }
            a64_mov_imm64(b, JT1, v);
            rm = JT1;
        } else {
            return 0;
        }

        switch (op) {
        case OCERZ_OP_ADD: a64_add_reg(b, sf, rd, rn, rm, 0); break;
        case OCERZ_OP_SUB: a64_sub_reg(b, sf, rd, rn, rm, 0); break;
        case OCERZ_OP_AND: a64_and_reg(b, sf, rd, rn, rm, 0); break;
        case OCERZ_OP_OR:  a64_orr_reg(b, sf, rd, rn, rm, 0); break;
        case OCERZ_OP_XOR: a64_eor_reg(b, sf, rd, rn, rm, 0); break;
        default: return 0;
        }
        if (ds < 0)
            emit_gpr_wr(b, rd, d->reg);
        return 1;
    }

    emit_gpr_rd(b, sf, JT0, d->reg);
    if (!emit_load_operand(b, s, sf, JT1))
        return 0;

    switch (op) {
    case OCERZ_OP_ADD: a64_add_reg(b, sf, JT2, JT0, JT1, 0); break;
    case OCERZ_OP_SUB:
    case OCERZ_OP_CMP: a64_sub_reg(b, sf, JT2, JT0, JT1, 0); break;
    case OCERZ_OP_AND:
    case OCERZ_OP_TEST: a64_and_reg(b, sf, JT2, JT0, JT1, 0); break;
    case OCERZ_OP_OR:  a64_orr_reg(b, sf, JT2, JT0, JT1, 0); break;
    case OCERZ_OP_XOR: a64_eor_reg(b, sf, JT2, JT0, JT1, 0); break;
    default: return 0;
    }

    if (writes)
        emit_gpr_wr(b, JT2, d->reg);

    if (need == 0)
        return 1;

    if (is_add)
        emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_ADD, d->size, 0), JT0, JT1);
    else if (is_sub)
        emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_SUB, d->size, 0), JT0, JT1);
    else
        emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_LOGIC, d->size, 0), JT2, JT2);
    return 1;
}

int emit_cmp_test_narrow(A64Buf *b, const X86Insn *insn, uint64_t need,
                                uint32_t **exit_sites, int *n_exits)
{
    static int no_narrow = -1;
    if (no_narrow < 0)
        no_narrow = getenv("OCERZ_NO_INLINE_NARROW") ? 1 : 0;
    if (no_narrow)
        return 0;
    unsigned op = insn->op;
    if (op != OCERZ_OP_CMP && op != OCERZ_OP_TEST)
        return 0;
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    if (d->size != 1 && d->size != 2)
        return 0;
    int d_mem = d->kind == OCERZ_OPK_MEM, s_mem = s->kind == OCERZ_OPK_MEM;
    if (d_mem && s_mem)
        return 0;
    if (d->kind == OCERZ_OPK_REG && d->high8)
        return 0;
    if (d->kind != OCERZ_OPK_REG && !d_mem)
        return 0;
    if (s->kind == OCERZ_OPK_REG) {
        if (s->high8 || s->size != d->size)
            return 0;
    } else if (s->kind == OCERZ_OPK_MEM) {
        if (s->size != d->size)
            return 0;
    } else if (s->kind != OCERZ_OPK_IMM) {
        return 0;
    }
    if ((d_mem || s_mem) && (!g_defer || (insn->seg != OCERZ_SEG_NONE && insn->seg != OCERZ_SEG_GS && insn->seg != OCERZ_SEG_FS)))
        return 0;

    if (!g_defer && need == 0)
        return 1;
    if ((d_mem || s_mem) && need == 0 && !g_nzcv_want)
        return 1;

    int is_sub = (op == OCERZ_OP_CMP);
    int size = d->size;
    uint64_t mask = (size == 1) ? 0xffull : 0xffffull;
    int sh = 32 - 8 * size;

    if (g_defer && !d_mem && !s_mem && pin_slot(d->reg) >= 0 &&
        !(rsp_is_ptr() && d->reg == OCERZ_RSP) &&
        (s->kind == OCERZ_OPK_IMM || (pin_slot(s->reg) >= 0 && !(rsp_is_ptr() && s->reg == OCERZ_RSP)))) {
        int rd = pin_hreg(pin_slot(d->reg));
        if (!is_sub && s->kind == OCERZ_OPK_IMM) {
            uint64_t v = (uint64_t)s->imm & mask;
            if (g_nzcv_want) {
                if (!a64_try_ands_imm(b, 1, JT2, rd, v)) { a64_mov_imm64(b, JT1, v); a64_ands_reg(b, 1, JT2, rd, JT1, 0); }
                if (need) emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_LOGIC, size, 0), JT2, JT2);
                g_nzcv_kind = OCERZ_CC_LOGIC;
                g_nzcv_from = g_cur_insn_idx;
                return 1;
            }
            if (!need) return 1;
            if (!a64_try_and_imm(b, 1, JT2, rd, v)) { a64_mov_imm64(b, JT1, v); a64_and_reg(b, 1, JT2, rd, JT1, 0); }
            emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_LOGIC, size, 0), JT2, JT2);
            return 1;
        }
        if (!need && !g_nzcv_want) return 1;
        if (size == 1) a64_uxtb(b, JT0, rd); else a64_uxth(b, JT0, rd);
        if (s->kind == OCERZ_OPK_IMM) a64_mov_imm64(b, JT1, (uint64_t)s->imm & mask);
        else { int rs = pin_hreg(pin_slot(s->reg)); if (size == 1) a64_uxtb(b, JT1, rs); else a64_uxth(b, JT1, rs); }
        if (g_nzcv_want && is_sub) {
            a64_subs_reg(b, 0, A64_ZR, JT0, JT1, 0);
            if (need) emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_SUB, size, 0), JT0, JT1);
            g_nzcv_kind = OCERZ_CC_SUB;
            g_nzcv_from = g_cur_insn_idx;
            return 1;
        }
        if (is_sub) emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_SUB, size, 0), JT0, JT1);
        else { a64_and_reg(b, 1, JT2, JT0, JT1, 0); emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_LOGIC, size, 0), JT2, JT2); }
        return 1;
    }

    if (s_mem) {
        if (!emit_mem_load_plain(b, insn, s, size, JT1)) {
            if (!emit_mem_ea(b, insn, s, JTA)) return 0;
            uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
            emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
            emit_guest_load_ordered(b, size, JT1, JTA, JTU);
            patch_guard_skip(skip, a64_label(b));
        }
    }
    if (d_mem) {
        if (!emit_mem_load_plain(b, insn, d, size, JT0)) {
            if (!emit_mem_ea(b, insn, d, JTA)) return 0;
            uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
            emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
            emit_guest_load_ordered(b, size, JT0, JTA, JTU);
            patch_guard_skip(skip, a64_label(b));
        }
    } else {
        emit_gpr_rd(b, 1, JT0, d->reg);
        if (size == 1) a64_uxtb(b, JT0, JT0); else a64_uxth(b, JT0, JT0);
    }
    if (s->kind == OCERZ_OPK_REG) {
        int ss = pin_slot(s->reg);
        int rs = (ss >= 0 && !(rsp_is_ptr() && s->reg == OCERZ_RSP)) ? pin_hreg(ss) : JT1;
        if (rs == JT1) emit_gpr_rd(b, 1, JT1, s->reg);
        if (size == 1) a64_uxtb(b, JT1, rs); else a64_uxth(b, JT1, rs);
    } else if (!s_mem) {

        a64_mov_imm64(b, JT1, (uint64_t)s->imm & mask);
    }

    if (g_defer) {
        if (g_nzcv_want && is_sub) {
            a64_subs_reg(b, 0, A64_ZR, JT0, JT1, 0);
            if (need) emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_SUB, size, 0), JT0, JT1);
            g_nzcv_kind = OCERZ_CC_SUB;
            g_nzcv_from = g_cur_insn_idx;
            return 1;
        }
        if (is_sub) {
            emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_SUB, size, 0), JT0, JT1);
        } else {
            a64_and_reg(b, 1, JT2, JT0, JT1, 0);
            emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_LOGIC, size, 0), JT2, JT2);
        }
        return 1;
    }

    a64_lsl_imm(b, 0, JTA, JT0, sh);
    a64_lsl_imm(b, 0, JTU, JT1, sh);
    if (is_sub)
        a64_subs_reg(b, 0, A64_ZR, JTA, JTU, 0);
    else
        a64_ands_reg(b, 0, A64_ZR, JTA, JTU, 0);

    if (is_sub)
        a64_sub_reg(b, 1, JT2, JT0, JT1, 0);
    else
        a64_and_reg(b, 1, JT2, JT0, JT1, 0);

    a64_mov_imm64(b, JTF, 0);
    emit_zf_sf(b, need);
    if (need & OCERZ_PF)
        emit_pf(b, JT2);
    if (is_sub) {
        if (need & OCERZ_CF) {
            a64_cset(b, JTT, A64_CC);
            a64_lsl_imm(b, 0, JTT, JTT, 0);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
        if (need & OCERZ_OF) {
            a64_cset(b, JTT, A64_VS);
            a64_lsl_imm(b, 0, JTT, JTT, 11);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
        if (need & OCERZ_AF) {
            a64_eor_reg(b, 1, JTT, JT0, JT1, 0);
            a64_eor_reg(b, 1, JTT, JTT, JT2, 0);
            a64_ubfx(b, 1, JTT, JTT, 4, 1);
            a64_lsl_imm(b, 0, JTT, JTT, 4);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
    }
    emit_commit_flags(b, need);
    return 1;
}

static int emit_incdec_eager(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    const X86Operand *d = &insn->ops[0];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    int sf = d->size == 8;
    int is_inc = insn->op == OCERZ_OP_INC;

    need &= JIT_ARITH_FLAGS & ~(uint64_t)OCERZ_CF;

    a64_ldr(b, sf ? 8 : 4, JT0, 20, GPR_OFF(d->reg));
    if (is_inc)
        a64_adds_imm(b, sf, JT2, JT0, 1);
    else
        a64_subs_imm(b, sf, JT2, JT0, 1);
    a64_str(b, 8, JT2, 20, GPR_OFF(d->reg));

    if (need == 0)
        return 1;

    a64_mov_imm64(b, JTF, 0);
    emit_zf_sf(b, need);
    if (need & OCERZ_PF)
        emit_pf(b, JT2);

    if (need & OCERZ_OF) {
        uint64_t of_const = is_inc ? ((uint64_t)1 << (d->size * 8 - 1))
                                   : (ocerz_mask(d->size) >> 1);
        a64_mov_imm64(b, JTU, of_const);
        a64_subs_reg(b, 1, A64_ZR, JT2, JTU, 0);
        a64_cset(b, JTT, A64_EQ);
        a64_lsl_imm(b, 0, JTT, JTT, 11);
        a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
    }

    if (need & OCERZ_AF) {
        a64_ubfx(b, 1, JTT, JT2, 0, 4);
        if (is_inc) {
            a64_subs_imm(b, 1, A64_ZR, JTT, 0);
            a64_cset(b, JTT, A64_EQ);
        } else {
            a64_subs_imm(b, 1, A64_ZR, JTT, 0xf);
            a64_cset(b, JTT, A64_EQ);
        }
        a64_lsl_imm(b, 0, JTT, JTT, 4);
        a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
    }

    emit_commit_flags(b, need);
    return 1;
}

static int emit_incdec_narrow(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    const X86Operand *d = &insn->ops[0];
    if (!g_defer || d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 1 && d->size != 2)) return 0;
    if (pin_slot(d->reg) < 0 || (rsp_is_ptr() && d->reg == OCERZ_RSP)) return 0;
    int rd = pin_hreg(pin_slot(d->reg));
    int bits = d->size * 8;
    int is_inc = insn->op == OCERZ_OP_INC;
    need &= JIT_ARITH_FLAGS & ~(uint64_t)OCERZ_CF;
    if (need) {
        emit_cc_predicate(b, OCERZ_CC_B);
        a64_cset(b, JTU, A64_NE);
    }
    if (d->size == 1) a64_uxtb(b, JT0, rd); else a64_uxth(b, JT0, rd);
    if (is_inc) a64_add_imm(b, 0, JT2, JT0, 1); else a64_sub_imm(b, 0, JT2, JT0, 1);
    a64_bfi(b, 1, rd, JT2, 0, bits);
    if (need) {
        if (d->size == 1) a64_uxtb(b, JT2, JT2); else a64_uxth(b, JT2, JT2);
        emit_defer_flags(b, ocerz_cc_pack(is_inc ? OCERZ_CC_INC : OCERZ_CC_DEC, d->size, 0), JTU, JT2);
    }
    return 1;
}

int emit_incdec(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    if (insn->ops[0].kind == OCERZ_OPK_REG && (insn->ops[0].size == 1 || insn->ops[0].size == 2))
        return emit_incdec_narrow(b, insn, need);
    if (!g_defer)
        return emit_incdec_eager(b, insn, need);
    const X86Operand *d = &insn->ops[0];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    int sf = d->size == 8;
    int is_inc = insn->op == OCERZ_OP_INC;

    need &= JIT_ARITH_FLAGS & ~(uint64_t)OCERZ_CF;

    if (need == 0) {
        int ds = pin_slot(d->reg);
        if (ds >= 0) {
            int rd = pin_hreg(ds);
            if (is_inc)
                a64_add_imm(b, sf, rd, rd, 1);
            else
                a64_sub_imm(b, sf, rd, rd, 1);
            return 1;
        }
    }

    emit_gpr_rd(b, sf, JT0, d->reg);
    if (is_inc)
        a64_add_imm(b, sf, JT2, JT0, 1);
    else
        a64_sub_imm(b, sf, JT2, JT0, 1);
    emit_gpr_wr(b, JT2, d->reg);

    if (need == 0)
        return 1;

    emit_cc_predicate(b, OCERZ_CC_B);
    a64_cset(b, JT0, A64_NE);
    emit_gpr_rd(b, 1, JT1, d->reg);
    emit_defer_flags(b, ocerz_cc_pack(is_inc ? OCERZ_CC_INC : OCERZ_CC_DEC,
                                      d->size, 0), JT0, JT1);
    return 1;
}

int emit_mov_logic_pair(A64Buf *b, const X86Insn *mov,
                               const X86Insn *logic, uint64_t logic_need,
                               uint32_t **logic_label)
{
    if (mov->lock || logic->lock || logic_need != 0 ||
        mov->op != OCERZ_OP_MOV ||
        (logic->op != OCERZ_OP_AND && logic->op != OCERZ_OP_OR &&
         logic->op != OCERZ_OP_XOR) ||
        mov->nops != 2 || logic->nops != 2 ||
        mov->rip + mov->len != logic->rip)
        return 0;

    const X86Operand *md = &mov->ops[0];
    const X86Operand *ms = &mov->ops[1];
    const X86Operand *ld = &logic->ops[0];
    const X86Operand *ls = &logic->ops[1];
    if (md->kind != OCERZ_OPK_REG || ms->kind != OCERZ_OPK_REG ||
        ld->kind != OCERZ_OPK_REG || md->high8 || ms->high8 || ld->high8 ||
        (md->size != 4 && md->size != 8) || ms->size != md->size ||
        ld->size != md->size || ld->reg != md->reg || ls->size != md->size)
        return 0;
    if (ls->kind == OCERZ_OPK_REG) {
        if (ls->high8)
            return 0;
    } else if (ls->kind != OCERZ_OPK_IMM) {
        return 0;
    }

    int sf = md->size == 8;
    int ds = pin_slot(md->reg);
    int ss = pin_slot(ms->reg);
    int rd = ds >= 0 ? pin_hreg(ds) : JT2;
    int rn = ss >= 0 ? pin_hreg(ss) : JT0;
    if (ss < 0)
        emit_gpr_rd(b, sf, JT0, ms->reg);

    if (logic_label)
        *logic_label = a64_label(b);

    int rm = JT1;
    if (ls->kind == OCERZ_OPK_IMM) {
        uint64_t v = ls->imm;
        if (!sf)
            v &= 0xffffffffull;
        int emitted = 0;
        if (logic->op == OCERZ_OP_AND)
            emitted = a64_try_and_imm(b, sf, rd, rn, v);
        else if (logic->op == OCERZ_OP_OR)
            emitted = a64_try_orr_imm(b, sf, rd, rn, v);
        else
            emitted = a64_try_eor_imm(b, sf, rd, rn, v);
        if (emitted) {
            if (ds < 0)
                emit_gpr_wr(b, rd, md->reg);
            return 1;
        }
        a64_mov_imm64(b, JT1, v);
    } else if (ls->reg == md->reg || ls->reg == ms->reg) {

        rm = rn;
    } else {
        int ls_slot = pin_slot(ls->reg);
        if (ls_slot >= 0)
            rm = pin_hreg(ls_slot);
        else
            emit_gpr_rd(b, sf, JT1, ls->reg);
    }

    if (logic->op == OCERZ_OP_AND)
        a64_and_reg(b, sf, rd, rn, rm, 0);
    else if (logic->op == OCERZ_OP_OR)
        a64_orr_reg(b, sf, rd, rn, rm, 0);
    else
        a64_eor_reg(b, sf, rd, rn, rm, 0);
    if (ds < 0)
        emit_gpr_wr(b, rd, md->reg);
    return 1;
}

int emit_add_inc_pair(A64Buf *b, const X86Insn *add,
                             const X86Insn *inc, uint64_t add_need,
                             uint64_t inc_need, uint32_t **inc_label)
{
    if (!g_defer || g_no_addincfuse || g_no_lazyflags || add->lock || inc->lock)
        return 0;
    if (add->op != OCERZ_OP_ADD || inc->op != OCERZ_OP_INC ||
        add->nops != 2 || inc->nops != 1 ||
        add->rip + add->len != inc->rip)
        return 0;

    const X86Operand *d = &add->ops[0];
    const X86Operand *s = &add->ops[1];
    const X86Operand *id = &inc->ops[0];
    if (d->kind != OCERZ_OPK_REG || id->kind != OCERZ_OPK_REG ||
        d->high8 || id->high8 || (d->size != 4 && d->size != 8) ||
        id->reg != d->reg || id->size != d->size)
        return 0;
    if (s->kind == OCERZ_OPK_REG) {
        if (s->high8 || s->size != d->size)
            return 0;
    } else if (s->kind != OCERZ_OPK_IMM || s->size != d->size) {
        return 0;
    }

    if (add_need != OCERZ_CF || inc_need == 0)
        return 0;

    int sf = d->size == 8;
    int ds = pin_slot(d->reg);
    int rd = ds >= 0 ? pin_hreg(ds) : JT2;
    if (ds < 0)
        emit_gpr_rd(b, sf, rd, d->reg);
    int emitted = 0;
    int rm = JT1;
    if (s->kind == OCERZ_OPK_IMM) {
        uint64_t v = s->imm;
        if (!sf)
            v &= 0xffffffffull;
        if (v <= 4095) {
            a64_adds_imm(b, sf, rd, rd, (uint32_t)v);
            emitted = 1;
        } else {
            a64_mov_imm64(b, JT1, v);
        }
    } else {
        int ss = pin_slot(s->reg);
        if (s->reg == d->reg)
            rm = rd;
        else if (ss >= 0)
            rm = pin_hreg(ss);
        else
            emit_gpr_rd(b, sf, JT1, s->reg);
    }
    if (!emitted)
        a64_adds_reg(b, sf, rd, rd, rm, 0);
    a64_cset(b, JT0, A64_CS);
    if (inc_label)
        *inc_label = a64_label(b);
    a64_add_imm(b, sf, rd, rd, 1);
    if (ds < 0)
        emit_gpr_wr(b, rd, d->reg);
    emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_INC, d->size, 0), JT0, rd);
    return 1;
}

static int emit_shift_eager(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    if (s->kind != OCERZ_OPK_IMM)
        return 0;
    int sf = d->size == 8;
    int bits = d->size * 8;
    unsigned cnt = (unsigned)(s->imm & (sf ? 63u : 31u));
    if (cnt == 0)
        return 1;

    unsigned op = insn->op;
    a64_ldr(b, sf ? 8 : 4, JT0, 20, GPR_OFF(d->reg));

    switch (op) {
    case OCERZ_OP_SHL: a64_lsl_imm(b, sf, JT2, JT0, (int)cnt); break;
    case OCERZ_OP_SHR: a64_lsr_imm(b, sf, JT2, JT0, (int)cnt); break;
    case OCERZ_OP_SAR: a64_asr_imm(b, sf, JT2, JT0, (int)cnt); break;
    default: return 0;
    }
    a64_str(b, 8, JT2, 20, GPR_OFF(d->reg));

    uint64_t emit = need;
    if (op == OCERZ_OP_SHL && (need & OCERZ_OF))
        emit |= OCERZ_CF | OCERZ_SF;

    if (emit == 0)
        return 1;

    if (emit & (OCERZ_ZF | OCERZ_SF))
        a64_subs_imm(b, sf, A64_ZR, JT2, 0);
    a64_mov_imm64(b, JTF, 0);
    emit_zf_sf(b, emit);
    if (emit & OCERZ_PF)
        emit_pf(b, JT2);

    if (op == OCERZ_OP_SHL) {
        if (emit & OCERZ_CF) {
            int cf_bit = bits - (int)cnt;
            if (cf_bit >= 0 && cf_bit < bits) {
                a64_ubfx(b, sf, JTT, JT0, cf_bit, 1);
                a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
            }
        }
        if (emit & OCERZ_OF) {
            a64_ubfx(b, 1, JTT, JTF, 0, 1);
            a64_ubfx(b, 1, JTU, JTF, 7, 1);
            a64_eor_reg(b, 1, JTT, JTT, JTU, 0);
            a64_lsl_imm(b, 0, JTT, JTT, 11);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
    } else if (op == OCERZ_OP_SHR) {
        if (emit & OCERZ_CF) {
            int cf_bit = (int)cnt - 1;
            a64_ubfx(b, sf, JTT, JT0, cf_bit, 1);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
        if (emit & OCERZ_OF) {
            a64_ubfx(b, sf, JTT, JT0, bits - 1, 1);
            a64_lsl_imm(b, 0, JTT, JTT, 11);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
    } else {
        if (emit & OCERZ_CF) {
            int shift = (int)cnt - 1;
            if (shift > 63)
                shift = 63;
            if (sf)
                a64_asr_imm(b, 1, JTT, JT0, shift);
            else {
                a64_sxtw(b, JTT, JT0);
                a64_asr_imm(b, 1, JTT, JTT, shift);
            }
            a64_ubfx(b, 1, JTT, JTT, 0, 1);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
    }
    emit_commit_flags(b, emit);
    return 1;
}

int emit_shift(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    if (!g_defer)
        return emit_shift_eager(b, insn, need);
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    if (d->kind == OCERZ_OPK_REG && !d->high8 && (d->size == 1 || d->size == 2) &&
        s->kind == OCERZ_OPK_IMM && pin_slot(d->reg) >= 0 && !(rsp_is_ptr() && d->reg == OCERZ_RSP) &&
        (insn->op == OCERZ_OP_SHL || insn->op == OCERZ_OP_SHR || insn->op == OCERZ_OP_SAR)) {
        unsigned ncnt = (unsigned)(s->imm & 31u);
        if (ncnt == 0) return 1;
        int rd = pin_hreg(pin_slot(d->reg));
        int nbits = d->size * 8;
        if (d->size == 1) { if (insn->op == OCERZ_OP_SAR) a64_sxtb(b, 0, JT0, rd); else a64_uxtb(b, JT0, rd); }
        else              { if (insn->op == OCERZ_OP_SAR) a64_sxth(b, 0, JT0, rd); else a64_uxth(b, JT0, rd); }
        switch (insn->op) {
        case OCERZ_OP_SHL: a64_lsl_imm(b, 0, JT2, JT0, (int)ncnt); break;
        case OCERZ_OP_SHR: a64_lsr_imm(b, 0, JT2, JT0, (int)ncnt); break;
        default:           a64_asr_imm(b, 0, JT2, JT0, (int)ncnt); break;
        }
        a64_bfi(b, 1, rd, JT2, 0, nbits);
        if (need) {
            if (insn->op == OCERZ_OP_SAR) { if (d->size == 1) a64_uxtb(b, JT0, JT0); else a64_uxth(b, JT0, JT0); }
            a64_mov_imm64(b, JT1, ncnt);
            unsigned nk = insn->op == OCERZ_OP_SHL ? OCERZ_CC_SHL : insn->op == OCERZ_OP_SHR ? OCERZ_CC_SHR : OCERZ_CC_SAR;
            emit_defer_flags(b, ocerz_cc_pack(nk, d->size, 0), JT0, JT1);
        }
        return 1;
    }
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    if (s->kind != OCERZ_OPK_IMM)
        return 0;
    int sf = d->size == 8;
    int bits = d->size * 8;
    unsigned cnt = (unsigned)(s->imm & (sf ? 63u : 31u));
    if (cnt == 0)
        return 1;

    unsigned op = insn->op;
    if (need == 0) {
        int ds = pin_slot(d->reg);
        if (ds >= 0) {
            int rd = pin_hreg(ds), rn = rd;
            if (op == OCERZ_OP_SHL || op == OCERZ_OP_SHR || op == OCERZ_OP_SAR) {
                int hs; if (fuse_prev_mov(b, d->reg, d->size, &hs, insn) >= 0) rn = hs;
                else if (g_cur_insns_fwd() && g_cur_insn_idx >= 0 && g_mov_sink_at[g_cur_insn_idx] >= 0)
                    rn = pin_hreg(pin_slot(g_cur_insns_fwd()[g_mov_sink_at[g_cur_insn_idx]].ops[1].reg));
            }
            switch (op) {
            case OCERZ_OP_SHL: a64_lsl_imm(b, sf, rd, rn, (int)cnt); break;
            case OCERZ_OP_SHR: a64_lsr_imm(b, sf, rd, rn, (int)cnt); break;
            case OCERZ_OP_SAR: a64_asr_imm(b, sf, rd, rn, (int)cnt); break;
            default: return 0;
            }
            return 1;
        }
    }
    emit_gpr_rd(b, sf, JT0, d->reg);

    switch (op) {
    case OCERZ_OP_SHL: a64_lsl_imm(b, sf, JT2, JT0, (int)cnt); break;
    case OCERZ_OP_SHR: a64_lsr_imm(b, sf, JT2, JT0, (int)cnt); break;
    case OCERZ_OP_SAR: a64_asr_imm(b, sf, JT2, JT0, (int)cnt); break;
    default: return 0;
    }
    emit_gpr_wr(b, JT2, d->reg);

    if (need == 0)
        return 1;

    (void)bits;
    a64_mov_imm64(b, JT1, cnt);
    unsigned kind = op == OCERZ_OP_SHL ? OCERZ_CC_SHL
                  : op == OCERZ_OP_SHR ? OCERZ_CC_SHR : OCERZ_CC_SAR;
    emit_defer_flags(b, ocerz_cc_pack(kind, d->size, 0), JT0, JT1);
    return 1;
}

static int g_no_inline_imul = -1;

static int imul_inline_enabled(void)
{
    if (g_no_inline_imul < 0) {
        const char *e = getenv("OCERZ_NO_INLINE_IMUL");
        g_no_inline_imul = (e && *e && *e != '0') ? 1 : 0;
    }
    return !g_no_inline_imul;
}

static int emit_imul_src(A64Buf *b, const X86Insn *insn, const X86Operand *op, int dst)
{
    if (op->kind == OCERZ_OPK_MEM) {
        if (op->size != 4 && op->size != 8)
            return 0;
        if (!emit_mem_load_any(b, insn, op, op->size, dst))
            return 0;
        if (op->size == 4)
            a64_sxtw(b, dst, dst);
        return 1;
    }
    if (op->kind == OCERZ_OPK_REG) {
        if (op->high8)
            return 0;
        if (op->size == 8)
            emit_gpr_rd(b, 1, dst, op->reg);
        else if (op->size == 4)
            emit_gpr_rd_sw(b, dst, op->reg);
        else
            return 0;
        return 1;
    }
    if (op->kind == OCERZ_OPK_IMM) {
        a64_mov_imm64(b, dst, (uint64_t)ocerz_sext(op->imm, op->size));
        return 1;
    }
    return 0;
}

int emit_mul_wide(A64Buf *b, const X86Insn *insn, uint64_t need, int is_signed)
{
    const X86Operand *o = &insn->ops[0];
    if (insn->nops != 1 || (o->size != 4 && o->size != 8)) return 0;
    if (g_pin_class != 3 || pin_slot(OCERZ_RAX) < 0 || pin_slot(OCERZ_RDX) < 0) return 0;
    if (!g_defer) return 0;
    int sf = o->size == 8;
    int hax = pin_hreg(pin_slot(OCERZ_RAX)), hdx = pin_hreg(pin_slot(OCERZ_RDX));
    int src;
    if (o->kind == OCERZ_OPK_REG) {
        if (o->high8 || pin_slot(o->reg) < 0) return 0;
        src = pin_hreg(pin_slot(o->reg));
    } else if (o->kind == OCERZ_OPK_MEM) {
        if (!emit_mem_load_any(b, insn, o, o->size, JT1)) return 0;
        src = JT1;
    } else return 0;
    if (sf) {
        if (is_signed) a64_smulh(b, JT2, hax, src); else a64_umulh(b, JT2, hax, src);
        a64_mul(b, 1, hax, hax, src);
        a64_mov_reg(b, 1, hdx, JT2);
    } else {
        a64_mov_reg(b, 0, JT0, hax);
        a64_mov_reg(b, 0, JT1, src);
        if (is_signed) { a64_sxtw(b, JT0, JT0); a64_sxtw(b, JT1, JT1); }
        a64_mul(b, 1, JT2, JT0, JT1);
        a64_mov_reg(b, 0, hax, JT2);
        a64_lsr_imm(b, 1, hdx, JT2, 32);
    }
    if (need)
        emit_defer_flags(b, ocerz_cc_pack(is_signed ? OCERZ_CC_IMUL : OCERZ_CC_MUL, o->size, 0), hax, hdx);
    return 1;
}

int emit_imul(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    if (!imul_inline_enabled())
        return 0;
    if (insn->op != OCERZ_OP_IMUL || insn->nops < 2 || insn->nops > 3)
        return 0;

    const X86Operand *d = &insn->ops[0];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    int sf = d->size == 8;

    const X86Operand *s1 = (insn->nops == 3) ? &insn->ops[1] : &insn->ops[0];
    const X86Operand *s2 = (insn->nops == 3) ? &insn->ops[2] : &insn->ops[1];

    if (s1->kind != OCERZ_OPK_IMM && s1->size != d->size)
        return 0;
    if (s2->kind != OCERZ_OPK_IMM && s2->size != d->size)
        return 0;
    if (rsp_is_ptr() && (d->reg == OCERZ_RSP ||
        (s1->kind == OCERZ_OPK_REG && s1->reg == OCERZ_RSP) ||
        (s2->kind == OCERZ_OPK_REG && s2->reg == OCERZ_RSP)))
        return 0;

    if (need == 0) {
        int ds = pin_slot(d->reg);
        if (ds >= 0) {
            int src[2];
            const X86Operand *ops[2] = { s1, s2 };
            int fused_hs = -1;
            if (s1->kind == OCERZ_OPK_REG && !s1->high8 && s1->reg == d->reg &&
                (s2->kind == OCERZ_OPK_IMM || (s2->kind == OCERZ_OPK_REG && !s2->high8)))
                (void)fuse_prev_mov(b, d->reg, d->size, &fused_hs, insn);
            int first_mem = ops[1]->kind == OCERZ_OPK_MEM;
            for (int k = 0; k < 2; k++) {
                int i = first_mem ? 1 - k : k;
                if (i == 0 && fused_hs >= 0) { src[0] = fused_hs; continue; }
                if (ops[i]->kind == OCERZ_OPK_REG) {
                    if (ops[i]->high8)
                        return 0;
                    int ps = pin_slot(ops[i]->reg);
                    if (ps >= 0)
                        src[i] = pin_hreg(ps);
                    else {
                        int tmp = i ? JT1 : JT0;
                        emit_gpr_rd(b, sf, tmp, ops[i]->reg);
                        src[i] = tmp;
                    }
                } else if (ops[i]->kind == OCERZ_OPK_IMM) {
                    int tmp = i ? JT1 : JT0;
                    uint64_t v = ops[i]->imm;
                    if (!sf)
                        v &= 0xffffffffull;
                    a64_mov_imm64(b, tmp, v);
                    src[i] = tmp;
                } else if (ops[i]->kind == OCERZ_OPK_MEM) {
                    int tmp = i ? JT1 : JT0;
                    if (!emit_mem_load_any(b, insn, ops[i], d->size, tmp))
                        return 0;
                    src[i] = tmp;
                } else {
                    return 0;
                }
            }
            a64_mul(b, sf, pin_hreg(ds), src[0], src[1]);
            return 1;
        }
    }

    a64_str(b, 4, A64_ZR, 20, CC_OP_OFF);
    if (s2->kind == OCERZ_OPK_MEM && !emit_imul_src(b, insn, s2, JT1))
        return 0;
    if (!emit_imul_src(b, insn, s1, JT0))
        return 0;
    if (s2->kind != OCERZ_OPK_MEM && !emit_imul_src(b, insn, s2, JT1))
        return 0;

    a64_mul(b, 1, JT2, JT0, JT1);
    if (sf && (need & (OCERZ_CF | OCERZ_OF)))
        a64_smulh(b, JTA, JT0, JT1);

    if (sf) {
        emit_gpr_wr(b, JT2, d->reg);
    } else {
        a64_mov_reg(b, 0, JTT, JT2);
        emit_gpr_wr(b, JTT, d->reg);
    }

    if (need == 0)
        return 1;

    a64_mov_imm64(b, JTF, 0);
    if (need & (OCERZ_CF | OCERZ_OF)) {
        if (sf) {
            a64_asr_imm(b, 1, JTU, JT2, 63);
            a64_subs_reg(b, 1, A64_ZR, JTA, JTU, 0);
        } else {
            a64_sxtw(b, JTU, JT2);
            a64_subs_reg(b, 1, A64_ZR, JT2, JTU, 0);
        }
        a64_cset(b, JTT, A64_NE);
        if (need & OCERZ_CF)
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        if (need & OCERZ_OF) {
            a64_lsl_imm(b, 0, JTU, JTT, 11);
            a64_orr_reg(b, 1, JTF, JTF, JTU, 0);
        }
    }
    emit_commit_flags(b, need);
    return 1;
}

int stack_inline_enabled(void)
{
    static int en = -1;
    if (en < 0)
        en = getenv("OCERZ_NO_INLINE_STACK") ? 0 : 1;
    return en;
}

int m32_lowreg_ok(void)
{
    return g_xlat_mode32 && ocerz_low_base != 0 && ocerz_guest_base == 0 && !jgb_usable() &&
           low_guard_fast_ok() && !ENV_ON("OCERZ_NO_M32_LOWREG");
}

static int emit_push_pop32(A64Buf *b, const X86Insn *insn)
{
    const X86Operand *o = &insn->ops[0];
    int size = insn->opsize ? insn->opsize : 4;

    if (size != 4)
        return 0;
    if (!m32_stack_ok(insn))
        return 0;
    int hs = pin_hreg(pin_slot(OCERZ_RSP));

    if (insn->op == OCERZ_OP_PUSH) {
        if (!mem_native_store_ok())
            return 0;
        int rv;
        if (o->kind == OCERZ_OPK_REG) {
            if (o->high8 || o->size != 4)
                return 0;
            int vs = pin_slot(o->reg);
            if (vs >= 0) rv = pin_hreg(vs);
            else { emit_gpr_rd(b, 0, JT1, o->reg); rv = JT1; }
        } else if (o->kind == OCERZ_OPK_IMM) {
            a64_mov_imm64(b, JT1, (uint64_t)(uint32_t)o->imm);
            rv = JT1;
        } else {
            return 0;
        }
        a64_sub_imm(b, 0, JTA, hs, 4);
        m32_stack_st(b, rv, JTA);
        a64_mov_reg(b, 0, hs, JTA);
        return 1;
    }

    if (insn->op == OCERZ_OP_POP) {
        if (o->kind != OCERZ_OPK_REG || o->high8 || o->size != 4)
            return 0;
        if (o->reg == OCERZ_RSP) {
            m32_stack_ld(b, hs, hs);
            return 1;
        }
        int ds = pin_slot(o->reg);
        int rd = ds >= 0 ? pin_hreg(ds) : JT1;
        m32_stack_ld(b, rd, hs);
        a64_add_imm(b, 0, hs, hs, 4);
        if (ds < 0)
            emit_gpr_wr(b, JT1, o->reg);
        return 1;
    }
    return 0;
}

static int emit_push_pop_mem32(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *m = &insn->ops[0];
    if ((insn->opsize ? insn->opsize : 4) != 4 || m->size != 4)
        return 0;
    if (!m32_stack_base_ok() || !mem_native_store_ok())
        return 0;
    if (insn->op == OCERZ_OP_POP && (m->base == OCERZ_RSP || m->index == OCERZ_RSP))
        return 0;
    int hs = pin_hreg(pin_slot(OCERZ_RSP));
    if (!emit_mem_ea(b, insn, m, JTA))
        return 0;
    (void)emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
    emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
    if (insn->op == OCERZ_OP_PUSH) {
        emit_guest_load_ordered(b, 4, JT1, JTA, JTU);
        a64_sub_imm(b, 0, JTA, hs, 4);
        m32_stack_st(b, JT1, JTA);
        a64_mov_reg(b, 0, hs, JTA);
    } else {
        m32_stack_ld(b, JT1, hs);
        emit_guest_store_ordered(b, 4, JT1, JTA, JTU);
        a64_add_imm(b, 0, hs, hs, 4);
    }
    return 1;
}

static int emit_leave32(A64Buf *b, const X86Insn *insn)
{
    if ((insn->opsize ? insn->opsize : 4) != 4)
        return 0;
    if (!m32_stack_ok(insn) || pin_slot(OCERZ_RBP) < 0)
        return 0;
    int hs = pin_hreg(pin_slot(OCERZ_RSP)), hb = pin_hreg(pin_slot(OCERZ_RBP));
    a64_mov_reg(b, 0, hs, hb);
    m32_stack_ld(b, hb, hs);
    a64_add_imm(b, 0, hs, hs, 4);
    return 1;
}

int emit_push_pop(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *o = &insn->ops[0];
    uint64_t gbase = ocerz_guest_base;

    if (insn->mode32)
        return emit_push_pop32(b, insn);
    if (!stack_inline_enabled())
        return 0;
    if (insn->opsize != 8 || insn->seg != OCERZ_SEG_NONE)
        return 0;

    if (insn->op == OCERZ_OP_PUSH) {
        if (!mem_native_store_ok())
            return 0;

        if (o->kind == OCERZ_OPK_REG) {
            if (o->high8 || o->size != 8)
                return 0;
        } else if (o->kind == OCERZ_OPK_IMM) {
            if (o->size != 8)
                return 0;
        } else {
            return 0;
        }

        if (g_lowstack && stack_plain_access_ok()) {

            int hs = pin_hreg(pin_slot(OCERZ_RSP));
            int rv = JT1;
            if (o->kind == OCERZ_OPK_REG) {
                int vs = pin_slot(o->reg);
                if (vs >= 0 && o->reg != OCERZ_RSP) rv = pin_hreg(vs);
                else emit_gpr_rd(b, 1, JT1, o->reg);
            } else {
                a64_mov_imm64(b, JT1, o->imm);
            }
            a64_sub_imm(b, 1, JTA, hs, 8);
            a64_str_regoff(b, 8, rv, JTA, JGB, 0);
            a64_mov_reg(b, 1, hs, JTA);
            return 1;
        }
        if (g_pin_class == 3 && pin_slot(OCERZ_RSP) >= 0 && stack_plain_access_ok() && jgb_usable() &&
            !stack_guard_needed()) {
            int hs = pin_hreg(pin_slot(OCERZ_RSP));
            int rv;
            if (o->kind == OCERZ_OPK_REG) {
                int vs = pin_slot(o->reg);
                if (vs >= 0 && !(rsp_is_ptr() && o->reg == OCERZ_RSP)) rv = pin_hreg(vs);
                else { emit_gpr_rd(b, 1, JT1, o->reg); rv = JT1; }
            } else {
                a64_mov_imm64(b, JT1, o->imm);
                rv = JT1;
            }
            if ((stack_identity() || rsp_is_ptr()) && rv != hs) {
                a64_str_pre64(b, rv, hs, -8);
                return 1;
            }
            if (g_push_entry && g_n_push_fix < JIT_MAX_BLOCK_INSNS && rv != hs) {
                a64_sub_imm(b, 1, hs, hs, 8);
                g_push_fix[g_n_push_fix++] = (uint32_t)(a64_label(b) - g_push_entry);
                a64_str_regoff(b, 8, rv, JGB, hs, 0);
                return 1;
            }
            a64_sub_imm(b, 1, JTA, hs, 8);
            a64_str_regoff(b, 8, rv, JGB, JTA, 0);
            a64_mov_reg(b, 1, hs, JTA);
            return 1;
        }
        if (g_pin_class == 2) {
            int rs = pin_slot(OCERZ_RSP);
            int rv = JT1;
            assert(rs >= 0);
            if (o->kind == OCERZ_OPK_REG && o->reg != OCERZ_RSP) {
                int vs = pin_slot(o->reg);
                if (vs >= 0)
                    rv = pin_hreg(vs);
                else
                    emit_gpr_rd(b, 1, JT1, o->reg);
            } else if (o->kind == OCERZ_OPK_REG) {
                emit_gpr_rd(b, 1, JT1, o->reg);
            } else {
                a64_mov_imm64(b, JT1, o->imm);
            }
            uint32_t *skip = NULL;
            if (stack_plain_access_ok() && !stack_guard_needed()) {
                a64_str_pre64(b, rv, pin_hreg(rs), -8);
            } else {
                a64_sub_imm(b, 1, JTA, pin_hreg(rs), 8);
                skip = emit_commpage_guard(b, insn, JTA,
                                           exit_sites, n_exits);
                g_ea_plain = stack_plain_now();
                emit_guest_store_ordered(b, 8, rv, JTA, JTU);
                a64_sub_imm(b, 1, pin_hreg(rs), pin_hreg(rs), 8);
            }
            patch_guard_skip(skip, a64_label(b));
            return 1;
        }

        if (o->kind == OCERZ_OPK_REG)
            emit_gpr_rd(b, 1, JT1, o->reg);
        else
            a64_mov_imm64(b, JT1, o->imm);

        emit_gpr_rd(b, 1, JT0, OCERZ_RSP);
        a64_sub_imm(b, 1, JTA, JT0, 8);
        emit_add_const(b, JTA, ea_fold());

        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, gbase - ea_fold());

        g_ea_plain = stack_plain_now();
        emit_guest_store_ordered(b, 8, JT1, JTA, JTU);
        a64_sub_imm(b, 1, JT0, JT0, 8);
        emit_gpr_wr(b, JT0, OCERZ_RSP);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }

    if (insn->op == OCERZ_OP_POP) {
        if (o->kind != OCERZ_OPK_REG || o->high8 || o->size != 8)
            return 0;

        if (g_lowstack && stack_plain_access_ok() && o->reg != OCERZ_RSP) {
            int hs = pin_hreg(pin_slot(OCERZ_RSP));
            int ds = pin_slot(o->reg);
            int rd = ds >= 0 ? pin_hreg(ds) : JT1;
            a64_ldr_regoff(b, 8, rd, hs, JGB, 0);
            a64_add_imm(b, 1, hs, hs, 8);
            if (ds < 0)
                emit_gpr_wr(b, JT1, o->reg);
            return 1;
        }
        if (g_pin_class == 3 && pin_slot(OCERZ_RSP) >= 0 && stack_plain_access_ok() && jgb_usable() &&
            !stack_guard_needed() && o->reg != OCERZ_RSP && pin_slot(o->reg) >= 0) {
            int hs = pin_hreg(pin_slot(OCERZ_RSP));
            if (stack_identity() || rsp_is_ptr()) {
                a64_ldr_post64(b, pin_hreg(pin_slot(o->reg)), hs, 8);
                return 1;
            }
            a64_ldr_regoff(b, 8, pin_hreg(pin_slot(o->reg)), JGB, hs, 0);
            a64_add_imm(b, 1, hs, hs, 8);
            return 1;
        }
        if (g_pin_class == 2) {
            int rs = pin_slot(OCERZ_RSP);
            int ds = o->reg == OCERZ_RSP ? -1 : pin_slot(o->reg);
            int rd = ds >= 0 ? pin_hreg(ds) : JT1;
            assert(rs >= 0);
            uint32_t *skip = NULL;
            if (stack_plain_access_ok() && !stack_guard_needed()) {
                a64_ldr_post64(b, rd, pin_hreg(rs), 8);
            } else {
                skip = emit_commpage_guard(b, insn, pin_hreg(rs),
                                           exit_sites, n_exits);
                g_ea_plain = stack_plain_now();
                emit_guest_load_ordered(b, 8, rd, pin_hreg(rs), JTU);
                a64_add_imm(b, 1, pin_hreg(rs), pin_hreg(rs), 8);
            }
            if (ds < 0)
                emit_gpr_wr(b, JT1, o->reg);
            patch_guard_skip(skip, a64_label(b));
            return 1;
        }

        emit_gpr_rd(b, 1, JT0, OCERZ_RSP);
        a64_mov_reg(b, 1, JTA, JT0);
        emit_add_const(b, JTA, ea_fold());

        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, gbase - ea_fold());
        g_ea_plain = stack_plain_now();
        emit_guest_load_ordered(b, 8, JT1, JTA, JTU);

        a64_add_imm(b, 1, JT0, JT0, 8);
        emit_gpr_wr(b, JT0, OCERZ_RSP);
        emit_gpr_wr(b, JT1, o->reg);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    return 0;
}

int emit_movsxd(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];

    if (d->kind != OCERZ_OPK_REG || d->high8 || d->size != 8)
        return 0;
    if (s->size != 4)
        return 0;

    int ds = pin_slot(d->reg);
    if (rsp_is_ptr() && d->reg == OCERZ_RSP) ds = -1;
    if (s->kind == OCERZ_OPK_REG) {
        if (s->high8)
            return 0;
        int ss = pin_slot(s->reg);
        if (ds >= 0 && ss >= 0 && !(rsp_is_ptr() && s->reg == OCERZ_RSP)) {
            a64_sxtw(b, pin_hreg(ds), pin_hreg(ss));
            return 1;
        }
        emit_gpr_rd(b, 0, JT0, s->reg);
        a64_sxtw(b, JT0, JT0);
        emit_gpr_wr(b, JT0, d->reg);
        return 1;
    }
    if (s->kind == OCERZ_OPK_MEM) {
        int ra; uint32_t disp;
        if (ds >= 0 && emit_mem_ea_plain(b, insn, s, 4, &ra, &disp)) {
            if (mem_plain_access_ok(s)) a64_ldrsw(b, pin_hreg(ds), ra, disp);
            else emit_gpr_lds_at(b, 4, 1, pin_hreg(ds), ra, (int32_t)disp);
            return 1;
        }
        if (!emit_mem_ea(b, insn, s, JTA))
            return 0;
        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
        emit_guest_load_ordered(b, 4, JT1, JTA, JTU);
        a64_sxtw(b, JT1, JT1);
        emit_gpr_wr(b, JT1, d->reg);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    return 0;
}

static int emit_arith_mem_eager(A64Buf *b, const X86Insn *insn, uint64_t need,
                                uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    if (s->kind != OCERZ_OPK_MEM || s->size != d->size)
        return 0;
    int sf = d->size == 8;

    if (!emit_mem_ea(b, insn, s, JTA))
        return 0;
    uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
    emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
    emit_guest_load_ordered(b, sf ? 8 : 4, JT1, JTA, JTU);
    a64_ldr(b, sf ? 8 : 4, JT0, 20, GPR_OFF(d->reg));

    unsigned op = insn->op;
    int is_sub = (op == OCERZ_OP_SUB || op == OCERZ_OP_CMP);
    int is_add = (op == OCERZ_OP_ADD);
    int is_logic = (op == OCERZ_OP_AND || op == OCERZ_OP_OR ||
                    op == OCERZ_OP_XOR || op == OCERZ_OP_TEST);
    int writes = (op == OCERZ_OP_ADD || op == OCERZ_OP_SUB ||
                  op == OCERZ_OP_AND || op == OCERZ_OP_OR || op == OCERZ_OP_XOR);

    switch (op) {
    case OCERZ_OP_ADD: a64_adds_reg(b, sf, JT2, JT0, JT1, 0); break;
    case OCERZ_OP_SUB:
    case OCERZ_OP_CMP: a64_subs_reg(b, sf, JT2, JT0, JT1, 0); break;
    case OCERZ_OP_AND:
    case OCERZ_OP_TEST: a64_ands_reg(b, sf, JT2, JT0, JT1, 0); break;
    case OCERZ_OP_OR:  a64_orr_reg(b, sf, JT2, JT0, JT1, 0); break;
    case OCERZ_OP_XOR: a64_eor_reg(b, sf, JT2, JT0, JT1, 0); break;
    default: return 0;
    }
    if (is_logic && op != OCERZ_OP_AND && op != OCERZ_OP_TEST)
        a64_subs_imm(b, sf, A64_ZR, JT2, 0);

    if (writes)
        a64_str(b, 8, JT2, 20, GPR_OFF(d->reg));

    if (need != 0) {
        a64_mov_imm64(b, JTF, 0);
        emit_zf_sf(b, need);
        if (need & OCERZ_PF)
            emit_pf(b, JT2);
        if (is_add || is_sub) {
            if (need & OCERZ_CF) {
                a64_cset(b, JTT, is_add ? A64_CS : A64_CC);
                a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
            }
            if (need & OCERZ_OF) {
                a64_cset(b, JTT, A64_VS);
                a64_lsl_imm(b, 0, JTT, JTT, 11);
                a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
            }
            if (need & OCERZ_AF) {
                a64_eor_reg(b, 1, JTT, JT0, JT1, 0);
                a64_eor_reg(b, 1, JTT, JTT, JT2, 0);
                a64_ubfx(b, 1, JTT, JTT, 4, 1);
                a64_lsl_imm(b, 0, JTT, JTT, 4);
                a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
            }
        }
        emit_commit_flags(b, need);
    }
    patch_guard_skip(skip, a64_label(b));
    return 1;
}

int emit_arith_mem(A64Buf *b, const X86Insn *insn, uint64_t need,
                          uint32_t **exit_sites, int *n_exits)
{
    if (!g_defer)
        return emit_arith_mem_eager(b, insn, need, exit_sites, n_exits);
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    if (s->kind != OCERZ_OPK_MEM || s->size != d->size)
        return 0;
    int sf = d->size == 8;

    unsigned op = insn->op;
    int is_sub = (op == OCERZ_OP_SUB || op == OCERZ_OP_CMP);
    int is_add = (op == OCERZ_OP_ADD);
    int is_logic = (op == OCERZ_OP_AND || op == OCERZ_OP_OR ||
                    op == OCERZ_OP_XOR || op == OCERZ_OP_TEST);
    int writes = (op == OCERZ_OP_ADD || op == OCERZ_OP_SUB ||
                  op == OCERZ_OP_AND || op == OCERZ_OP_OR || op == OCERZ_OP_XOR);

    if (pin_slot(d->reg) >= 0 && !(rsp_is_ptr() && d->reg == OCERZ_RSP) &&
        emit_mem_load_plain(b, insn, s, sf ? 8 : 4, JT1)) {
        int rd = pin_hreg(pin_slot(d->reg));
        if (g_nzcv_want) {
            if (is_add || is_sub) {
                if (need) {
                    if (sf) a64_stp_off(b, rd, JT1, 20, CC_SRC_OFF);
                    else { a64_mov_reg(b, 0, JT0, rd); a64_stp_off(b, JT0, JT1, 20, CC_SRC_OFF); }
                    a64_mov_imm64(b, JTT, ocerz_cc_pack(is_add ? OCERZ_CC_ADD : OCERZ_CC_SUB, d->size, 0));
                    a64_str(b, 4, JTT, 20, CC_OP_OFF);
                }
                int rdst = writes ? rd : A64_ZR;
                if (is_add) a64_adds_reg(b, sf, rdst, rd, JT1, 0);
                else        a64_subs_reg(b, sf, rdst, rd, JT1, 0);
                g_nzcv_kind = is_add ? OCERZ_CC_ADD : OCERZ_CC_SUB;
            } else {
                switch (op) {
                case OCERZ_OP_TEST: a64_ands_reg(b, sf, JT2, rd, JT1, 0); break;
                case OCERZ_OP_AND:  a64_ands_reg(b, sf, rd, rd, JT1, 0); break;
                case OCERZ_OP_OR:   a64_orr_reg(b, sf, rd, rd, JT1, 0); a64_ands_reg(b, sf, A64_ZR, rd, rd, 0); break;
                default:            a64_eor_reg(b, sf, rd, rd, JT1, 0); a64_ands_reg(b, sf, A64_ZR, rd, rd, 0); break;
                }
                if (need) {
                    int rr = op == OCERZ_OP_TEST ? JT2 : rd;
                    emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_LOGIC, d->size, 0), rr, rr);
                }
                g_nzcv_kind = OCERZ_CC_LOGIC;
            }
            g_nzcv_from = g_cur_insn_idx;
            return 1;
        }
        if (!writes) {
            if (need == 0) return 1;
            if (is_sub) { emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_SUB, d->size, 0), rd, JT1); return 1; }
            a64_and_reg(b, sf, JT2, rd, JT1, 0);
            emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_LOGIC, d->size, 0), JT2, JT2);
            return 1;
        }
        if (need != 0 && (is_add || is_sub)) {
            if (sf) a64_stp_off(b, rd, JT1, 20, CC_SRC_OFF);
            else { a64_mov_reg(b, 0, JT0, rd); a64_stp_off(b, JT0, JT1, 20, CC_SRC_OFF); }
            a64_mov_imm64(b, JTT, ocerz_cc_pack(is_add ? OCERZ_CC_ADD : OCERZ_CC_SUB, d->size, 0));
            a64_str(b, 4, JTT, 20, CC_OP_OFF);
        }
        switch (op) {
        case OCERZ_OP_ADD: a64_add_reg(b, sf, rd, rd, JT1, 0); break;
        case OCERZ_OP_SUB: a64_sub_reg(b, sf, rd, rd, JT1, 0); break;
        case OCERZ_OP_AND: a64_and_reg(b, sf, rd, rd, JT1, 0); break;
        case OCERZ_OP_OR:  a64_orr_reg(b, sf, rd, rd, JT1, 0); break;
        default:           a64_eor_reg(b, sf, rd, rd, JT1, 0); break;
        }
        if (need != 0 && is_logic)
            emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_LOGIC, d->size, 0), rd, rd);
        return 1;
    }

    if (!emit_mem_ea(b, insn, s, JTA))
        return 0;
    uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
    emit_add_const(b, JTA, ocerz_guest_base - ea_fold());

    emit_guest_load_ordered(b, sf ? 8 : 4, JT1, JTA, JTU);
    emit_gpr_rd(b, sf, JT0, d->reg);

    switch (op) {
    case OCERZ_OP_ADD: a64_add_reg(b, sf, JT2, JT0, JT1, 0); break;
    case OCERZ_OP_SUB:
    case OCERZ_OP_CMP: a64_sub_reg(b, sf, JT2, JT0, JT1, 0); break;
    case OCERZ_OP_AND:
    case OCERZ_OP_TEST: a64_and_reg(b, sf, JT2, JT0, JT1, 0); break;
    case OCERZ_OP_OR:  a64_orr_reg(b, sf, JT2, JT0, JT1, 0); break;
    case OCERZ_OP_XOR: a64_eor_reg(b, sf, JT2, JT0, JT1, 0); break;
    default: return 0;
    }

    if (writes)
        emit_gpr_wr(b, JT2, d->reg);

    if (need != 0) {
        if (is_add)
            emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_ADD, d->size, 0), JT0, JT1);
        else if (is_sub)
            emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_SUB, d->size, 0), JT0, JT1);
        else
            emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_LOGIC, d->size, 0), JT2, JT2);
    }
    (void)is_logic;

    patch_guard_skip(skip, a64_label(b));
    return 1;
}

int emit_lea(A64Buf *b, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    if (s->kind != OCERZ_OPK_MEM)
        return 0;

    int ds = pin_slot(d->reg);
    int bs = s->base != OCERZ_REG_NONE ? pin_slot(s->base) : -1;
    int is = s->index != OCERZ_REG_NONE ? pin_slot(s->index) : -1;
    int host_rsp_operand = rsp_is_ptr() &&
        (d->reg == OCERZ_RSP || s->base == OCERZ_RSP || s->index == OCERZ_RSP);
    if (ds >= 0 && !s->riprel && bs >= 0 && !host_rsp_operand) {
        int sf = insn->addrsize == 8 && d->size == 8;
        int64_t disp = s->disp;
        int rd = pin_hreg(ds);
        if (s->index == OCERZ_REG_NONE && disp >= -4095 && disp <= 4095) {
            if (disp >= 0)
                a64_add_imm(b, sf, rd, pin_hreg(bs), (uint32_t)disp);
            else
                a64_sub_imm(b, sf, rd, pin_hreg(bs), (uint32_t)-disp);
            return 1;
        }
        if (is >= 0 && disp >= -4095 && disp <= 4095) {
            a64_add_reg(b, sf, rd, pin_hreg(bs), pin_hreg(is), s->scale & 3);
            if (disp > 0)
                a64_add_imm(b, sf, rd, rd, (uint32_t)disp);
            else if (disp < 0)
                a64_sub_imm(b, sf, rd, rd, (uint32_t)-disp);
            return 1;
        }
    }
    if (ds >= 0 && !s->riprel && !host_rsp_operand && insn->addrsize == 8 &&
        s->disp == 0 && bs >= 0 && is >= 0) {
        a64_add_reg(b, d->size == 8, pin_hreg(ds), pin_hreg(bs), pin_hreg(is), s->scale & 3);
        return 1;
    }
    if (ds >= 0 && !s->riprel && !host_rsp_operand && insn->addrsize == 8 &&
        s->base == OCERZ_REG_NONE && is >= 0 && s->disp >= -4095 && s->disp <= 4095) {
        int sf = d->size == 8;
        int rd = pin_hreg(ds);
        if ((s->scale & 3) == 0) {
            if (s->disp > 0)      a64_add_imm(b, sf, rd, pin_hreg(is), (uint32_t)s->disp);
            else if (s->disp < 0) a64_sub_imm(b, sf, rd, pin_hreg(is), (uint32_t)-s->disp);
            else                  a64_mov_reg(b, sf, rd, pin_hreg(is));
        } else {
            a64_lsl_imm(b, sf, rd, pin_hreg(is), s->scale & 3);
            if (s->disp > 0)      a64_add_imm(b, sf, rd, rd, (uint32_t)s->disp);
            else if (s->disp < 0) a64_sub_imm(b, sf, rd, rd, (uint32_t)-s->disp);
        }
        return 1;
    }

    if (s->riprel) {
        a64_mov_imm64(b, JT2, (uint64_t)s->disp);
    } else {
        a64_mov_imm64(b, JT2, (uint64_t)s->disp);
        if (s->base != OCERZ_REG_NONE) {
            emit_gpr_rd(b, 1, JT0, s->base);
            a64_add_reg(b, 1, JT2, JT2, JT0, 0);
        }
        if (s->index != OCERZ_REG_NONE) {
            emit_gpr_rd(b, 1, JT0, s->index);
            a64_add_reg(b, 1, JT2, JT2, JT0, s->scale & 3);
        }
        if (insn->addrsize == 4)
            a64_mov_reg(b, 0, JT2, JT2);
    }
    if (d->size == 4)
        a64_mov_reg(b, 0, JT2, JT2);
    emit_gpr_wr(b, JT2, d->reg);
    return 1;
}

int g_cc_direct = -1;

int emit_adc_sbb(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (!g_defer) return 0;
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
    if (rsp_is_ptr() && d->reg == OCERZ_RSP) return 0;
    if (s->kind == OCERZ_OPK_REG) { if (s->high8 || s->size != d->size) return 0; }
    else if (s->kind != OCERZ_OPK_IMM) return 0;
    int sf = d->size == 8;
    int is_sbb = insn->op == OCERZ_OP_SBB;
    if (!need && pin_slot(d->reg) >= 0 &&
        (s->kind == OCERZ_OPK_IMM || (pin_slot(s->reg) >= 0 && !(rsp_is_ptr() && s->reg == OCERZ_RSP)))) {
        int rd = pin_hreg(pin_slot(d->reg));
        emit_cc_predicate_ex(b, OCERZ_CC_B, 1);
        a64_cset(b, JTT, g_cc_direct >= 0 ? g_cc_direct : A64_NE);
        if (s->kind == OCERZ_OPK_REG) {
            int rs = pin_hreg(pin_slot(s->reg));
            if (is_sbb) a64_sub_reg(b, sf, rd, rd, rs, 0); else a64_add_reg(b, sf, rd, rd, rs, 0);
        } else {
            uint64_t v = sf ? (uint64_t)ocerz_sext(s->imm, s->size) : ((uint64_t)ocerz_sext(s->imm, s->size) & 0xffffffffull);
            if (v <= 4095) { if (is_sbb) a64_sub_imm(b, sf, rd, rd, (uint32_t)v); else a64_add_imm(b, sf, rd, rd, (uint32_t)v); }
            else { a64_mov_imm64(b, JT1, v); if (is_sbb) a64_sub_reg(b, sf, rd, rd, JT1, 0); else a64_add_reg(b, sf, rd, rd, JT1, 0); }
        }
        if (is_sbb) a64_sub_reg(b, sf, rd, rd, JTT, 0); else a64_add_reg(b, sf, rd, rd, JTT, 0);
        return 1;
    }
    emit_cc_predicate(b, OCERZ_CC_B);
    a64_cset(b, JTT, A64_NE);
    emit_gpr_rd(b, sf, JT0, d->reg);
    if (s->kind == OCERZ_OPK_REG) emit_gpr_rd(b, sf, JT1, s->reg);
    else a64_mov_imm64(b, JT1, sf ? (uint64_t)ocerz_sext(s->imm, s->size) : ((uint64_t)ocerz_sext(s->imm, s->size) & 0xffffffffull));
    if (is_sbb) { a64_sub_reg(b, sf, JT2, JT0, JT1, 0); a64_sub_reg(b, sf, JT2, JT2, JTT, 0); }
    else        { a64_add_reg(b, sf, JT2, JT0, JT1, 0); a64_add_reg(b, sf, JT2, JT2, JTT, 0); }
    emit_gpr_wr(b, JT2, d->reg);
    if (need) {
        a64_mov_imm64(b, JTU, ocerz_cc_pack(is_sbb ? OCERZ_CC_SUB : OCERZ_CC_ADD, d->size, 0));
        a64_mov_imm64(b, JTA, ocerz_cc_pack(is_sbb ? OCERZ_CC_SUB : OCERZ_CC_ADD, d->size, 1));
        a64_subs_imm(b, 1, A64_ZR, JTT, 0);
        a64_csel(b, 1, JTU, JTA, JTU, A64_NE);
        _Static_assert(CC_DST_OFF == CC_SRC_OFF + 8, "cc layout");
        a64_stp_off(b, JT0, JT1, 20, CC_SRC_OFF);
        a64_str(b, 4, JTU, 20, CC_OP_OFF);
    }
    return 1;
}

int emit_arith_narrow(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (!g_defer) return 0;
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 1 && d->size != 2)) return 0;
    if (rsp_is_ptr() && d->reg == OCERZ_RSP) return 0;
    int s_mem = 0;
    if (s->kind == OCERZ_OPK_REG) { if (s->high8 || s->size != d->size) return 0; }
    else if (s->kind == OCERZ_OPK_MEM) { if (s->size != d->size || insn->seg != OCERZ_SEG_NONE) return 0; s_mem = 1; }
    else if (s->kind != OCERZ_OPK_IMM) return 0;
    unsigned op = insn->op;
    if (op != OCERZ_OP_ADD && op != OCERZ_OP_SUB && op != OCERZ_OP_AND &&
        op != OCERZ_OP_OR && op != OCERZ_OP_XOR) return 0;
    int size = d->size, bits = size * 8;
    uint64_t mask = size == 1 ? 0xffull : 0xffffull;
    if (s_mem) {
        int ds = pin_slot(d->reg);
        if (ds < 0) return 0;
        if (!emit_mem_load_plain(b, insn, s, size, JT1)) {
            if (!emit_mem_ea(b, insn, s, JTA)) return 0;
            (void)emit_commpage_guard(b, insn, JTA, NULL, NULL);
            emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
            emit_guest_load_ordered(b, size, JT1, JTA, JTU);
        }
        int rd = pin_hreg(ds);
        if (!need) {
            switch (op) {
            case OCERZ_OP_ADD: a64_add_reg(b, 0, JT2, rd, JT1, 0); break;
            case OCERZ_OP_SUB: a64_sub_reg(b, 0, JT2, rd, JT1, 0); break;
            case OCERZ_OP_AND: a64_and_reg(b, 0, JT2, rd, JT1, 0); break;
            case OCERZ_OP_OR:  a64_orr_reg(b, 0, JT2, rd, JT1, 0); break;
            default:           a64_eor_reg(b, 0, JT2, rd, JT1, 0); break;
            }
            a64_bfi(b, 1, rd, JT2, 0, bits);
            return 1;
        }
        if (size == 1) a64_uxtb(b, JTT, rd); else a64_uxth(b, JTT, rd);
        switch (op) {
        case OCERZ_OP_ADD: a64_add_reg(b, 0, JT2, JTT, JT1, 0); break;
        case OCERZ_OP_SUB: a64_sub_reg(b, 0, JT2, JTT, JT1, 0); break;
        case OCERZ_OP_AND: a64_and_reg(b, 0, JT2, JTT, JT1, 0); break;
        case OCERZ_OP_OR:  a64_orr_reg(b, 0, JT2, JTT, JT1, 0); break;
        default:           a64_eor_reg(b, 0, JT2, JTT, JT1, 0); break;
        }
        a64_bfi(b, 1, rd, JT2, 0, bits);
        if (op == OCERZ_OP_ADD)      emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_ADD, size, 0), JTT, JT1);
        else if (op == OCERZ_OP_SUB) emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_SUB, size, 0), JTT, JT1);
        else { if (size == 1) a64_uxtb(b, JT2, JT2); else a64_uxth(b, JT2, JT2);
               emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_LOGIC, size, 0), JT2, JT2); }
        return 1;
    }
    if (!need && pin_slot(d->reg) >= 0 && !(rsp_is_ptr() && d->reg == OCERZ_RSP)) {
        int rd = pin_hreg(pin_slot(d->reg));
        int rm;
        if (s->kind == OCERZ_OPK_REG) {
            int ss = pin_slot(s->reg);
            if (ss >= 0 && !(rsp_is_ptr() && s->reg == OCERZ_RSP)) rm = pin_hreg(ss);
            else { emit_gpr_rd(b, 1, JT1, s->reg); rm = JT1; }
        } else {
            uint64_t v = (uint64_t)s->imm & mask;
            switch (op) {
            case OCERZ_OP_ADD: if (v <= 4095) { a64_add_imm(b, 0, JT2, rd, (uint32_t)v); a64_bfi(b, 1, rd, JT2, 0, bits); return 1; } break;
            case OCERZ_OP_SUB: if (v <= 4095) { a64_sub_imm(b, 0, JT2, rd, (uint32_t)v); a64_bfi(b, 1, rd, JT2, 0, bits); return 1; } break;
            default: break;
            }
            a64_mov_imm64(b, JT1, v);
            rm = JT1;
        }
        switch (op) {
        case OCERZ_OP_ADD: a64_add_reg(b, 0, JT2, rd, rm, 0); break;
        case OCERZ_OP_SUB: a64_sub_reg(b, 0, JT2, rd, rm, 0); break;
        case OCERZ_OP_AND: a64_and_reg(b, 0, JT2, rd, rm, 0); break;
        case OCERZ_OP_OR:  a64_orr_reg(b, 0, JT2, rd, rm, 0); break;
        default:           a64_eor_reg(b, 0, JT2, rd, rm, 0); break;
        }
        a64_bfi(b, 1, rd, JT2, 0, bits);
        return 1;
    }
    emit_gpr_rd(b, 1, JT0, d->reg);
    if (size == 1) a64_uxtb(b, JTT, JT0); else a64_uxth(b, JTT, JT0);
    if (s->kind == OCERZ_OPK_REG) {
        emit_gpr_rd(b, 1, JT1, s->reg);
        if (size == 1) a64_uxtb(b, JT1, JT1); else a64_uxth(b, JT1, JT1);
    } else
        a64_mov_imm64(b, JT1, (uint64_t)s->imm & mask);
    switch (op) {
    case OCERZ_OP_ADD: a64_add_reg(b, 0, JT2, JTT, JT1, 0); break;
    case OCERZ_OP_SUB: a64_sub_reg(b, 0, JT2, JTT, JT1, 0); break;
    case OCERZ_OP_AND: a64_and_reg(b, 0, JT2, JTT, JT1, 0); break;
    case OCERZ_OP_OR:  a64_orr_reg(b, 0, JT2, JTT, JT1, 0); break;
    case OCERZ_OP_XOR: a64_eor_reg(b, 0, JT2, JTT, JT1, 0); break;
    }
    a64_bfi(b, 1, JT0, JT2, 0, bits);
    emit_gpr_wr(b, JT0, d->reg);
    if (need) {
        if (op == OCERZ_OP_ADD)      emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_ADD, size, 0), JTT, JT1);
        else if (op == OCERZ_OP_SUB) emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_SUB, size, 0), JTT, JT1);
        else {
            if (size == 1) a64_uxtb(b, JT2, JT2); else a64_uxth(b, JT2, JT2);
            emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_LOGIC, size, 0), JT2, JT2);
        }
    }
    return 1;
}

int emit_cbw_cwd(A64Buf *b, const X86Insn *insn)
{
    if (g_pin_class == 2) return 0;
    if (insn->op == OCERZ_OP_CBW) {
        emit_gpr_rd(b, 1, JT0, OCERZ_RAX);
        if (insn->opsize == 2)      { a64_sxtb(b, 0, JT1, JT0); a64_bfi(b, 1, JT0, JT1, 0, 16); }
        else if (insn->opsize == 4) { a64_sxth(b, 0, JT0, JT0); }
        else                        { a64_sxtw(b, JT0, JT0); }
        emit_gpr_wr(b, JT0, OCERZ_RAX);
        return 1;
    }
    if (insn->opsize != 2 && pin_slot(OCERZ_RAX) >= 0 && pin_slot(OCERZ_RDX) >= 0 && g_pin_class == 3) {
        int hax = pin_hreg(pin_slot(OCERZ_RAX)), hdx = pin_hreg(pin_slot(OCERZ_RDX));
        if (insn->opsize == 4) a64_asr_imm(b, 0, hdx, hax, 31);
        else                   a64_asr_imm(b, 1, hdx, hax, 63);
        return 1;
    }
    emit_gpr_rd(b, 1, JT0, OCERZ_RAX);
    if (insn->opsize == 2) {
        emit_gpr_rd(b, 1, JT1, OCERZ_RDX);
        a64_sbfx(b, 1, JT0, JT0, 15, 1);
        a64_bfi(b, 1, JT1, JT0, 0, 16);
        emit_gpr_wr(b, JT1, OCERZ_RDX);
    } else if (insn->opsize == 4) {
        a64_asr_imm(b, 0, JT0, JT0, 31);
        emit_gpr_wr(b, JT0, OCERZ_RDX);
    } else {
        a64_asr_imm(b, 1, JT0, JT0, 63);
        emit_gpr_wr(b, JT0, OCERZ_RDX);
    }
    return 1;
}

int g_div_prev_skipped;

uint32_t g_oolslow_pre;

int rdx_prep_skippable(const X86Insn *insns, int i, int n, uint64_t need)
{
    static int dis = -1; if (dis < 0) dis = getenv("OCERZ_NO_RDXSKIP") ? 1 : 0;
    if (dis || !insns || i + 1 >= n || need != 0 || g_pin_class != 3) return 0;
    if (pin_slot(OCERZ_RAX) < 0 || pin_slot(OCERZ_RDX) < 0) return 0;
    const X86Insn *p = &insns[i], *d = &insns[i + 1];
    if (d->seg != OCERZ_SEG_NONE || d->nops < 1) return 0;
    const X86Operand *o = &d->ops[0];
    if (o->kind != OCERZ_OPK_REG || o->high8 || (o->size != 4 && o->size != 8) || pin_slot(o->reg) < 0) return 0;
    if (ENV_ON("OCERZ_NO_INLINE_DIV")) return 0;
    if (d->op == OCERZ_OP_DIV)
        return p->op == OCERZ_OP_XOR && p->nops == 2 && p->ops[0].kind == OCERZ_OPK_REG && p->ops[1].kind == OCERZ_OPK_REG &&
               p->ops[0].reg == OCERZ_RDX && p->ops[1].reg == OCERZ_RDX && !p->ops[0].high8 && !p->ops[1].high8 &&
               (p->ops[0].size == 4 || p->ops[0].size == 8) && p->ops[1].size == p->ops[0].size;
    if (d->op == OCERZ_OP_IDIV)
        return p->op == OCERZ_OP_CWD && p->opsize == o->size;
    return 0;
}

int emit_div(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *o = &insn->ops[0];
    if (o->size != 4 && o->size != 8) return 0;
    if (g_pin_class == 2) return 0;
    if (insn->seg != OCERZ_SEG_NONE) return 0;
    int sf = o->size == 8;
    int is_idiv = insn->op == OCERZ_OP_IDIV;
    int rdx_zero = 0, rdx_sext = 0;
    if (g_cur_insns && g_cur_insn_idx >= 1) {
        const X86Insn *pv = &g_cur_insns[g_cur_insn_idx - 1];
        if (pv->op == OCERZ_OP_XOR && pv->nops == 2 && pv->ops[0].kind == OCERZ_OPK_REG && pv->ops[1].kind == OCERZ_OPK_REG &&
            pv->ops[0].reg == OCERZ_RDX && pv->ops[1].reg == OCERZ_RDX && !pv->ops[0].high8 && (pv->ops[0].size == 4 || pv->ops[0].size == 8))
            rdx_zero = 1;
        if (pv->op == OCERZ_OP_CWD && ((sf && pv->opsize == 8) || (!sf && pv->opsize == 4))) rdx_sext = 1;
    }
    int prep_skipped = g_div_prev_skipped; g_div_prev_skipped = 0;
    uint32_t pre_word = 0;
    if (prep_skipped && pin_slot(OCERZ_RDX) >= 0 && pin_slot(OCERZ_RAX) >= 0) {
        int hdx0 = pin_hreg(pin_slot(OCERZ_RDX)), hax0 = pin_hreg(pin_slot(OCERZ_RAX));
        if (rdx_zero) pre_word = 0xaa1f03e0u | (uint32_t)hdx0;
        else pre_word = (sf ? 0x9340fc00u : 0x13007c00u) | ((uint32_t)hax0 << 5) | (uint32_t)hdx0;
    }
    int hdv = JT2;
    if (o->kind == OCERZ_OPK_MEM) {
        if (!emit_mem_ea(b, insn, o, JTA)) return 0;
        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
        emit_guest_load_ordered(b, o->size, JT2, JTA, JTU);
        patch_guard_skip(skip, a64_label(b));
    } else if (o->kind == OCERZ_OPK_REG) {
        if (o->high8) return 0;
        if (pin_slot(o->reg) >= 0 && g_pin_class == 3 && pin_slot(OCERZ_RAX) >= 0 && pin_slot(OCERZ_RDX) >= 0)
            hdv = pin_hreg(pin_slot(o->reg));
        else
            emit_gpr_rd(b, sf, JT2, o->reg);
    } else return 0;
    if (pin_slot(OCERZ_RAX) >= 0 && pin_slot(OCERZ_RDX) >= 0 && g_pin_class == 3) {
        int hax = pin_hreg(pin_slot(OCERZ_RAX)), hdx = pin_hreg(pin_slot(OCERZ_RDX));
        uint32_t *sites[4]; int ns = 0;
        sites[ns++] = a64_label(b); a64_cbz(b, sf, hdv, 0);
        if (!is_idiv) {
            if (!rdx_zero) { sites[ns++] = a64_label(b); a64_cbnz(b, sf, hdx, 0); }
            a64_udiv(b, sf, JTT, hax, hdv);
        } else {
            if (!rdx_sext) {
                a64_asr_imm(b, sf, JTT, hax, sf ? 63 : 31);
                a64_subs_reg(b, sf, A64_ZR, hdx, JTT, 0);
                sites[ns++] = a64_label(b); a64_bcond(b, A64_NE, 0);
            }
            a64_subs_imm(b, sf, A64_ZR, hdv, 0);
            b->p--;
            a64_emit32(b, (sf ? 0xb100041fu : 0x3100041fu) | ((uint32_t)hdv << 5));
            uint32_t *not_m1 = a64_label(b); a64_bcond(b, A64_NE, 0);
            a64_try_eor_imm(b, sf, JTT, hax, sf ? 0x8000000000000000ull : 0x80000000ull);
            sites[ns++] = a64_label(b); a64_cbz(b, sf, JTT, 0);
            a64_patch_bcond(not_m1, a64_label(b));
            a64_sdiv(b, sf, JTT, hax, hdv);
        }
        a64_msub(b, sf, hdx, JTT, hdv, hax);
        a64_mov_reg(b, sf, hax, JTT);
        g_oolslow_pre = pre_word;
        if (oolslow_add(insn, sites, ns, a64_label(b)))
            return 1;
        g_oolslow_pre = 0;
        uint32_t *done = a64_label(b); a64_b(b, 0);
        uint32_t *slow = a64_label(b);
        for (int i = 0; i < ns; i++) patch_any_branch(sites[i], slow);
        if (pre_word) a64_emit32(b, pre_word);
        emit_slowcall(b, insn, exit_sites, n_exits);
        a64_patch_b(done, a64_label(b));
        return 1;
    }
    emit_gpr_rd(b, sf, JT0, OCERZ_RAX);
    emit_gpr_rd(b, sf, JT1, OCERZ_RDX);
    uint32_t *to_slow[2]; int ns = 0;
    a64_subs_imm(b, sf, A64_ZR, JT2, 0);
    to_slow[ns++] = a64_label(b); a64_bcond(b, A64_EQ, 0);
    if (is_idiv) {
        a64_asr_imm(b, sf, JTT, JT0, sf ? 63 : 31);
        a64_subs_reg(b, sf, A64_ZR, JT1, JTT, 0);
    } else {
        a64_subs_imm(b, sf, A64_ZR, JT1, 0);
    }
    to_slow[ns++] = a64_label(b); a64_bcond(b, A64_NE, 0);
    if (is_idiv) {
        a64_mov_imm64(b, JTT, sf ? 0x8000000000000000ull : 0x80000000ull);
        a64_subs_reg(b, sf, A64_ZR, JT0, JTT, 0);
        uint32_t *not_min = a64_label(b); a64_bcond(b, A64_NE, 0);
        a64_mov_imm64(b, JTT, sf ? ~0ull : 0xffffffffull);
        a64_subs_reg(b, sf, A64_ZR, JT2, JTT, 0);
        uint32_t *to_slow3 = a64_label(b); a64_bcond(b, A64_EQ, 0);
        a64_patch_bcond(not_min, a64_label(b));
        a64_sdiv(b, sf, JTT, JT0, JT2);
        a64_msub(b, sf, JTU, JTT, JT2, JT0);
        emit_gpr_wr(b, JTT, OCERZ_RAX);
        emit_gpr_wr(b, JTU, OCERZ_RDX);
        uint32_t *done = a64_label(b); a64_b(b, 0);
        uint32_t *slow = a64_label(b);
        a64_patch_bcond(to_slow3, slow);
        for (int i = 0; i < ns; i++) a64_patch_bcond(to_slow[i], slow);
        emit_slowcall(b, insn, exit_sites, n_exits);
        a64_patch_b(done, a64_label(b));
    } else {
        a64_udiv(b, sf, JTT, JT0, JT2);
        a64_msub(b, sf, JTU, JTT, JT2, JT0);
        emit_gpr_wr(b, JTT, OCERZ_RAX);
        emit_gpr_wr(b, JTU, OCERZ_RDX);
        uint32_t *done = a64_label(b); a64_b(b, 0);
        uint32_t *slow = a64_label(b);
        for (int i = 0; i < ns; i++) a64_patch_bcond(to_slow[i], slow);
        emit_slowcall(b, insn, exit_sites, n_exits);
        a64_patch_b(done, a64_label(b));
    }
    return 1;
}

int emit_not_neg(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    const X86Operand *d = &insn->ops[0];
    if (d->kind == OCERZ_OPK_REG && !d->high8 && (d->size == 1 || d->size == 2) && g_defer &&
        pin_slot(d->reg) >= 0 && !(rsp_is_ptr() && d->reg == OCERZ_RSP)) {
        int rd = pin_hreg(pin_slot(d->reg));
        int bits = d->size * 8;
        if (insn->op == OCERZ_OP_NOT) {
            a64_try_eor_imm(b, 1, rd, rd, d->size == 1 ? 0xffull : 0xffffull);
            return 1;
        }
        if (d->size == 1) a64_uxtb(b, JT0, rd); else a64_uxth(b, JT0, rd);
        a64_neg_reg(b, 0, JT2, JT0);
        a64_bfi(b, 1, rd, JT2, 0, bits);
        if (need) {
            a64_mov_imm64(b, JT1, 0);
            emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_SUB, d->size, 0), JT1, JT0);
        }
        return 1;
    }
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    int sf = d->size == 8;
    int ds = pin_slot(d->reg);
    if (insn->op == OCERZ_OP_NOT) {
        if (ds >= 0 && !(rsp_is_ptr() && d->reg == OCERZ_RSP)) {
            a64_orn_reg(b, sf, pin_hreg(ds), A64_ZR, pin_hreg(ds), 0);
            return 1;
        }
        emit_gpr_rd(b, sf, JT0, d->reg);
        a64_orn_reg(b, sf, JT2, A64_ZR, JT0, 0);
        emit_gpr_wr(b, JT2, d->reg);
        return 1;
    }
    if (!g_defer && need)
        return 0;
    emit_gpr_rd(b, sf, JT0, d->reg);
    a64_sub_reg(b, sf, JT2, A64_ZR, JT0, 0);
    emit_gpr_wr(b, JT2, d->reg);
    if (need) {
        a64_mov_imm64(b, JT1, 0);
        emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_SUB, d->size, 0), JT1, JT0);
    }
    return 1;
}

static int emit_shift_count(A64Buf *b, const X86Insn *insn, int sf, int dst,
                            unsigned *const_cnt)
{
    const X86Operand *s = &insn->ops[1];
    unsigned mask = sf ? 63u : 31u;
    if (s->kind == OCERZ_OPK_IMM) {
        *const_cnt = (unsigned)(s->imm & mask);
        return 1;
    }
    if (s->kind == OCERZ_OPK_REG && s->reg == OCERZ_RCX && !s->high8 && s->size == 1) {
        emit_gpr_rd(b, 1, dst, OCERZ_RCX);
        a64_mov_imm64(b, JTU, mask);
        a64_and_reg(b, 1, dst, dst, JTU, 0);
        *const_cnt = 0xffffffffu;
        return 1;
    }
    return 0;
}

int emit_rot(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    const X86Operand *d = &insn->ops[0];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    if (rsp_is_ptr() && d->reg == OCERZ_RSP)
        return 0;
    if (insn->mode32 && insn->ops[1].kind != OCERZ_OPK_IMM)
        return 0;
    int sf = d->size == 8;
    int bits = sf ? 64 : 32;
    int is_rol = insn->op == OCERZ_OP_ROL;
    unsigned cnt;
    if (!emit_shift_count(b, insn, sf, JT1, &cnt))
        return 0;
    int variable = cnt == 0xffffffffu;
    if (!variable && cnt == 0)
        return 1;
    if (need && variable)
        return 0;
    int ds = pin_slot(d->reg);
    int rd = ds >= 0 ? pin_hreg(ds) : JT2;
    if (ds < 0)
        emit_gpr_rd(b, sf, JT0, d->reg);
    int rn = ds >= 0 ? rd : JT0;
    if (variable) {
        if (is_rol) {
            a64_mov_imm64(b, JTU, (uint64_t)bits);
            a64_sub_reg(b, 1, JT1, JTU, JT1, 0);
        }
        a64_rorv(b, sf, rd, rn, JT1);
    } else {
        unsigned r = is_rol ? (unsigned)(bits - (int)(cnt % (unsigned)bits)) % (unsigned)bits
                            : cnt % (unsigned)bits;
        if (r == 0)
            a64_mov_reg(b, sf, rd, rn);
        else
            a64_extr(b, sf, rd, rn, rn, (int)r);
    }
    if (ds < 0)
        emit_gpr_wr(b, rd, d->reg);
    if (!need)
        return 1;
    emit_materialize(b);
    a64_ldr(b, 8, JTT, 20, RF_OFF);
    if (is_rol)
        a64_ubfx(b, 1, JT1, rd, 0, 1);
    else
        a64_ubfx(b, 1, JT1, rd, bits - 1, 1);
    a64_mov_imm64(b, JTU, ~(uint64_t)OCERZ_CF & (cnt == 1 ? ~(uint64_t)OCERZ_OF : ~0ull));
    a64_and_reg(b, 1, JTT, JTT, JTU, 0);
    a64_orr_reg(b, 1, JTT, JTT, JT1, 0);
    if (cnt == 1) {
        if (is_rol) {
            a64_ubfx(b, 1, JTU, rd, bits - 1, 1);
            a64_eor_reg(b, 1, JTU, JTU, JT1, 0);
        } else {
            a64_ubfx(b, 1, JTU, rd, bits - 2, 1);
            a64_eor_reg(b, 1, JTU, JTU, JT1, 0);
        }
        a64_lsl_imm(b, 1, JTU, JTU, 11);
        a64_orr_reg(b, 1, JTT, JTT, JTU, 0);
    }
    a64_str(b, 8, JTT, 20, RF_OFF);
    return 1;
}

int emit_shift_cl(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    if (!(s->kind == OCERZ_OPK_REG && s->reg == OCERZ_RCX && !s->high8 && s->size == 1))
        return 0;
    if (rsp_is_ptr() && d->reg == OCERZ_RSP)
        return 0;
    if (need)
        return 0;
    if (insn->mode32)
        return 0;
    int sf = d->size == 8;
    unsigned cnt;
    if (!emit_shift_count(b, insn, sf, JT1, &cnt))
        return 0;
    int ds = pin_slot(d->reg);
    int rd = ds >= 0 ? pin_hreg(ds) : JT2;
    if (ds < 0)
        emit_gpr_rd(b, sf, JT0, d->reg);
    int rn = ds >= 0 ? rd : JT0;
    switch (insn->op) {
    case OCERZ_OP_SHL: a64_lslv(b, sf, rd, rn, JT1); break;
    case OCERZ_OP_SHR: a64_lsrv(b, sf, rd, rn, JT1); break;
    case OCERZ_OP_SAR: a64_asrv(b, sf, rd, rn, JT1); break;
    default: return 0;
    }
    if (ds < 0)
        emit_gpr_wr(b, rd, d->reg);
    return 1;
}

int emit_cmov(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    if (rsp_is_ptr() && (d->reg == OCERZ_RSP || (s->kind == OCERZ_OPK_REG && s->reg == OCERZ_RSP)))
        return 0;
    int sf = d->size == 8;
    if (s->kind == OCERZ_OPK_REG) {
        if (s->high8 || s->size != d->size)
            return 0;
    } else if (s->kind != OCERZ_OPK_MEM)
        return 0;
    if (s->kind == OCERZ_OPK_REG && pin_slot(d->reg) >= 0 && pin_slot(s->reg) >= 0) {
        emit_cc_predicate_ex(b, insn->cc, 1);
        int cond = g_cc_direct >= 0 ? g_cc_direct : A64_NE;
        int rd = pin_hreg(pin_slot(d->reg)), rs = pin_hreg(pin_slot(s->reg));
        if (cond == A64_AL) { if (sf) a64_mov_reg(b, 1, rd, rs); else a64_mov_reg(b, 0, rd, rs); }
        else if (cond == A64_NV) { if (!sf) a64_mov_reg(b, 0, rd, rd); }
        else a64_csel(b, sf, rd, rs, rd, cond);
        return 1;
    }
    emit_cc_predicate(b, insn->cc);
    a64_cset(b, JTF, A64_NE);
    if (s->kind == OCERZ_OPK_REG) {
        emit_gpr_rd(b, sf, JT2, s->reg);
    } else {
        uint32_t *skip2;
        if (!emit_sse_mem_addr(b, insn, s, d->size, exit_sites, n_exits, &skip2)) return 0;
        emit_sse_mem_ld_gpr(b, d->size, JT2);
        patch_guard_skip(skip2, a64_label(b));
    }
    a64_subs_imm(b, 1, A64_ZR, JTF, 0);
    emit_gpr_rd(b, sf, JT0, d->reg);
    a64_csel(b, sf, JT0, JT2, JT0, A64_NE);
    if (!sf)
        a64_mov_reg(b, 0, JT0, JT0);
    emit_gpr_wr(b, JT0, d->reg);
    return 1;
}

static int setcc_zx_partner(const X86Insn *insn)
{
    if (!g_cur_insns || g_cur_insn_idx < 0 || ENV_ON("OCERZ_NO_SETCC_ZX")) return -1;
    unsigned r = insn->ops[0].reg & 15;
    for (int k = g_cur_insn_idx + 1; k < g_cur_insns_n && k <= g_cur_insn_idx + 4; k++) {
        const X86Insn *t = &g_cur_insns[k];
        if (t->op == OCERZ_OP_MOVZX && t->nops == 2 && t->ops[0].kind == OCERZ_OPK_REG && t->ops[1].kind == OCERZ_OPK_REG &&
            (t->ops[0].size == 4 || t->ops[0].size == 8) && t->ops[1].size == 1 && !t->ops[1].high8 &&
            (t->ops[0].reg & 15) == r && (t->ops[1].reg & 15) == r)
            return g_mov_skip[k] ? -1 : k;
        if (t->op == OCERZ_OP_SETCC && t->ops[0].kind == OCERZ_OPK_REG && !t->ops[0].high8 && (t->ops[0].reg & 15) != r)
            continue;
        if (!mov_sink_gap_ok(t, r, r)) return -1;
    }
    return -1;
}

int emit_setcc(A64Buf *b, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    if (d->kind == OCERZ_OPK_MEM && d->size == 1 && insn->addrsize == 8 && g_defer &&
        (insn->seg == OCERZ_SEG_NONE || insn->seg == OCERZ_SEG_GS || insn->seg == OCERZ_SEG_FS)) {
        emit_cc_predicate_ex(b, insn->cc, 1);
        a64_cset(b, JT2, g_cc_direct >= 0 ? g_cc_direct : A64_NE);
        if (mem_native_store_ok() && emit_plain_mem_fast(b, insn, d, 1, JT2, 1, 0)) return 1;
        if (!emit_mem_ea(b, insn, d, JTA)) return 0;
        (void)emit_commpage_guard(b, insn, JTA, NULL, NULL);
        emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
        emit_guest_store_ordered(b, 1, JT2, JTA, JTU);
        return 1;
    }
    if (d->kind != OCERZ_OPK_REG || d->high8 || d->size != 1)
        return 0;
    if (rsp_is_ptr() && d->reg == OCERZ_RSP)
        return 0;
    emit_cc_predicate_ex(b, insn->cc, 1);
    int zx = pin_slot(d->reg) >= 0 ? setcc_zx_partner(insn) : -1;
    if (zx >= 0) {
        a64_cset(b, pin_hreg(pin_slot(d->reg)), g_cc_direct >= 0 ? g_cc_direct : A64_NE);
        g_mov_skip[zx] = 1;
        return 1;
    }
    a64_cset(b, JT2, g_cc_direct >= 0 ? g_cc_direct : A64_NE);
    if (pin_slot(d->reg) >= 0) {
        a64_bfi(b, 1, pin_hreg(pin_slot(d->reg)), JT2, 0, 8);
        return 1;
    }
    emit_gpr_rd(b, 1, JT0, d->reg);
    a64_bfi(b, 1, JT0, JT2, 0, 8);
    emit_gpr_wr(b, JT0, d->reg);
    return 1;
}

int emit_bswap(A64Buf *b, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    if (rsp_is_ptr() && d->reg == OCERZ_RSP)
        return 0;
    int sf = d->size == 8;
    int ds = pin_slot(d->reg);
    if (ds >= 0) {
        a64_rev(b, sf, pin_hreg(ds), pin_hreg(ds));
        return 1;
    }
    emit_gpr_rd(b, sf, JT0, d->reg);
    a64_rev(b, sf, JT2, JT0);
    emit_gpr_wr(b, JT2, d->reg);
    return 1;
}

int emit_bitscan(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    if (!g_defer || insn->nops != 2 || insn->seg != OCERZ_SEG_NONE) return 0;
    if (need != 0) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
    if (s->size != d->size) return 0;
    int ds = pin_slot(d->reg);
    if (ds < 0 || (rsp_is_ptr() && d->reg == OCERZ_RSP)) return 0;
    int sf = d->size == 8;
    int rs;
    if (s->kind == OCERZ_OPK_REG) {
        if (s->high8) return 0;
        int ss = pin_slot(s->reg);
        if (ss < 0 || (rsp_is_ptr() && s->reg == OCERZ_RSP)) return 0;
        rs = pin_hreg(ss);
    } else if (s->kind == OCERZ_OPK_MEM) {
        if (!emit_mem_load_any(b, insn, s, d->size, JT1)) return 0;
        rs = JT1;
    } else return 0;
    int rd = pin_hreg(ds);
    switch (insn->op) {
    case OCERZ_OP_BSF:
    case OCERZ_OP_BSR:
        if (insn->op == OCERZ_OP_BSF) { a64_rbit(b, sf, JT0, rs); a64_clz(b, sf, JT0, JT0); }
        else { a64_clz(b, sf, JT0, rs); if (sf) a64_try_eor_imm(b, 1, JT0, JT0, 63); else a64_try_eor_imm(b, 0, JT0, JT0, 31); }
        a64_subs_imm(b, sf, A64_ZR, rs, 0);
        a64_csel(b, 1, rd, rd, JT0, A64_EQ);
        if (g_nzcv_want) { g_nzcv_kind = OCERZ_CC_SUB; g_nzcv_from = g_cur_insn_idx; }
        return 1;
    case OCERZ_OP_TZCNT:
        a64_rbit(b, sf, rd, rs); a64_clz(b, sf, rd, rd);
        return 1;
    case OCERZ_OP_LZCNT:
        a64_clz(b, sf, rd, rs);
        return 1;
    case OCERZ_OP_POPCNT:
        a64_fmov_v_from_x(b, sf, VX0, rs);
        a64_v_cnt_8b(b, VX0, VX0);
        a64_addv_b_8b(b, VX0, VX0);
        a64_umov_w_b(b, rd, VX0, 0);
        return 1;
    default: return 0;
    }
}

int emit_bt(A64Buf *b, const X86Insn *insn, uint64_t need, uint32_t **exit_sites, int *n_exits)
{
    if (!g_defer || insn->nops != 2 || insn->seg != OCERZ_SEG_NONE) return 0;
    if (insn->addrsize != 8 && !(insn->addrsize == 4 && insn->mode32)) return 0;
    const X86Operand *d = &insn->ops[0], *o = &insn->ops[1];
    int size = d->size;
    if (size != 2 && size != 4 && size != 8) return 0;
    if (insn->op != OCERZ_OP_BT) {
        if (d->kind != OCERZ_OPK_REG || d->high8 || pin_slot(d->reg) < 0 || (rsp_is_ptr() && d->reg == OCERZ_RSP)) return 0;
        if (o->kind == OCERZ_OPK_REG) { if (o->high8 || pin_slot(o->reg) < 0 || (rsp_is_ptr() && o->reg == OCERZ_RSP)) return 0; }
        else if (o->kind != OCERZ_OPK_IMM) return 0;
        if (need != 0) emit_materialize(b);
        int rd = pin_hreg(pin_slot(d->reg));
        int sf = size == 8;
        unsigned bits = (unsigned)size * 8;
        if (o->kind == OCERZ_OPK_IMM) {
            unsigned n = (unsigned)o->imm & (bits - 1);
            a64_ubfx(b, 1, JT0, rd, (int)n, 1);
            a64_mov_imm64(b, JT1, 1ull << n);
        } else {
            a64_try_and_imm(b, 1, JT2, pin_hreg(pin_slot(o->reg)), bits - 1);
            a64_lsrv(b, 1, JT0, rd, JT2);
            a64_try_and_imm(b, 1, JT0, JT0, 1);
            a64_mov_imm64(b, JT1, 1);
            a64_lslv(b, 1, JT1, JT1, JT2);
        }
        if (size == 2) {
            a64_uxth(b, JT2, rd);
            if (insn->op == OCERZ_OP_BTS) a64_orr_reg(b, 0, JT2, JT2, JT1, 0);
            else if (insn->op == OCERZ_OP_BTR) a64_bic_reg(b, 0, JT2, JT2, JT1, 0);
            else a64_eor_reg(b, 0, JT2, JT2, JT1, 0);
            a64_bfi(b, 1, rd, JT2, 0, 16);
        } else {
            if (insn->op == OCERZ_OP_BTS) a64_orr_reg(b, sf, rd, rd, JT1, 0);
            else if (insn->op == OCERZ_OP_BTR) a64_bic_reg(b, sf, rd, rd, JT1, 0);
            else a64_eor_reg(b, sf, rd, rd, JT1, 0);
        }
        if (need != 0) {
            a64_ldr(b, 8, JT1, 20, RFLAGS_OFF);
            a64_bfi(b, 1, JT1, JT0, 0, 1);
            a64_str(b, 8, JT1, 20, RFLAGS_OFF);
        }
        if (g_nzcv_want) { a64_subs_imm(b, 1, A64_ZR, JT0, 0); g_nzcv_kind = NZCV_KIND_BT; g_nzcv_from = g_cur_insn_idx; }
        return 1;
    }
    if (o->kind == OCERZ_OPK_REG) { if (o->high8 || pin_slot(o->reg) < 0 || (rsp_is_ptr() && o->reg == OCERZ_RSP)) return 0; }
    else if (o->kind != OCERZ_OPK_IMM) return 0;
    int mat = need != 0 && !g_nzcv_want;
    if (need != 0 && g_nzcv_want) mat = 1;
    if (mat) emit_materialize(b);
    unsigned bits = (unsigned)size * 8;
    if (d->kind == OCERZ_OPK_REG) {
        if (d->high8 || pin_slot(d->reg) < 0 || (rsp_is_ptr() && d->reg == OCERZ_RSP)) return 0;
        int rd = pin_hreg(pin_slot(d->reg));
        if (o->kind == OCERZ_OPK_IMM) {
            unsigned n = (unsigned)o->imm & (bits - 1);
            a64_ubfx(b, 1, JT0, rd, (int)n, 1);
        } else {
            a64_try_and_imm(b, 1, JT1, pin_hreg(pin_slot(o->reg)), bits - 1);
            a64_lsrv(b, 1, JT0, rd, JT1);
            a64_try_and_imm(b, 1, JT0, JT0, 1);
        }
    } else if (d->kind == OCERZ_OPK_MEM) {
        if (!emit_mem_ea(b, insn, d, JTA)) return 0;
        (void)emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
        if (o->kind == OCERZ_OPK_IMM) {
            unsigned n = (unsigned)o->imm & (bits - 1);
            if (n >> 3) a64_add_imm(b, 1, JTA, JTA, n >> 3);
            emit_guest_load_ordered(b, 1, JT0, JTA, JTU);
            a64_ubfx(b, 1, JT0, JT0, (int)(n & 7), 1);
        } else {
            int ro = pin_hreg(pin_slot(o->reg));
            int osf = o->size == 8;
            if (o->size == 2) a64_sxth(b, 1, JT1, ro); else if (o->size == 4) a64_sxtw(b, JT1, ro); else a64_mov_reg(b, 1, JT1, ro);
            (void)osf;
            a64_asr_imm(b, 1, JT2, JT1, 3);
            a64_add_reg(b, 1, JTA, JTA, JT2, 0);
            emit_guest_load_ordered(b, 1, JT0, JTA, JTU);
            a64_try_and_imm(b, 1, JT1, JT1, 7);
            a64_lsrv(b, 1, JT0, JT0, JT1);
            a64_try_and_imm(b, 1, JT0, JT0, 1);
        }
    } else return 0;
    if (mat) {
        a64_ldr(b, 8, JT1, 20, RFLAGS_OFF);
        a64_bfi(b, 1, JT1, JT0, 0, 1);
        a64_str(b, 8, JT1, 20, RFLAGS_OFF);
    }
    if (g_nzcv_want) {
        a64_subs_imm(b, 1, A64_ZR, JT0, 0);
        g_nzcv_kind = NZCV_KIND_BT;
        g_nzcv_from = g_cur_insn_idx;
    }
    return 1;
}

int emit_push_pop_mem(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    if (insn->mode32)
        return emit_push_pop_mem32(b, insn, exit_sites, n_exits);
    if (g_pin_class != 3 || pin_slot(OCERZ_RSP) < 0 || !stack_plain_access_ok() || !jgb_usable() || stack_guard_needed()) return 0;
    if (insn->nops != 1 || insn->ops[0].kind != OCERZ_OPK_MEM || insn->ops[0].size != 8) return 0;
    if (insn->addrsize != 8 || (insn->seg != OCERZ_SEG_NONE && insn->seg != OCERZ_SEG_GS && insn->seg != OCERZ_SEG_FS)) return 0;
    const X86Operand *m = &insn->ops[0];
    int hs = pin_hreg(pin_slot(OCERZ_RSP));
    if (insn->op == OCERZ_OP_PUSH) {
        if (!emit_mem_load_plain(b, insn, m, 8, JT1)) {
            if (!emit_mem_ea(b, insn, m, JTA)) return 0;
            (void)emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
            emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
            emit_guest_load_ordered(b, 8, JT1, JTA, JTU);
        }
        emit_push_pinned(b, hs, JT1);
        return 1;
    }
    if (m->base == OCERZ_RSP || m->index == OCERZ_RSP) return 0;
    if (rsp_is_ptr()) a64_ldr(b, 8, JT1, hs, 0);
    else a64_ldr_regoff(b, 8, JT1, JGB, hs, 0);
    if (!emit_plain_mem_fast(b, insn, m, 8, JT1, 1, 0)) {
        if (!emit_mem_ea(b, insn, m, JTA)) return 0;
        (void)emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
        emit_guest_store_ordered(b, 8, JT1, JTA, JTU);
    }
    a64_add_imm(b, 1, hs, hs, 8);
    return 1;
}

int emit_leave(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    (void)insn; (void)exit_sites; (void)n_exits;
    if (insn->mode32)
        return emit_leave32(b, insn);
    if (g_pin_class != 3 || pin_slot(OCERZ_RSP) < 0 || pin_slot(OCERZ_RBP) < 0 ||
        !stack_plain_access_ok() || !jgb_usable() || stack_guard_needed())
        return 0;
    int hs = pin_hreg(pin_slot(OCERZ_RSP)), hb = pin_hreg(pin_slot(OCERZ_RBP));
    if (rsp_is_ptr()) {
        a64_add_reg(b, 1, hs, hb, JGB, 0);
        a64_ldr_post64(b, hb, hs, 8);
        return 1;
    }
    a64_mov_reg(b, 1, hs, hb);
    if (stack_identity()) {
        a64_ldr_post64(b, hb, hs, 8);
        return 1;
    }
    a64_ldr_regoff(b, 8, hb, JGB, hs, 0);
    a64_add_imm(b, 1, hs, hs, 8);
    return 1;
}

int emit_shiftd(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    if (need != 0 || !g_defer || insn->nops != 3) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1], *c = &insn->ops[2];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
    if (s->kind != OCERZ_OPK_REG || s->high8 || s->size != d->size || c->kind != OCERZ_OPK_IMM) return 0;
    if (pin_slot(d->reg) < 0 || pin_slot(s->reg) < 0) return 0;
    if (rsp_is_ptr() && (d->reg == OCERZ_RSP || s->reg == OCERZ_RSP)) return 0;
    int sf = d->size == 8;
    unsigned bits = (unsigned)d->size * 8;
    unsigned cnt = (unsigned)c->imm & (sf ? 63u : 31u);
    if (cnt == 0) return 1;
    int rd = pin_hreg(pin_slot(d->reg)), rs = pin_hreg(pin_slot(s->reg));
    if (insn->op == OCERZ_OP_SHRD) a64_extr(b, sf, rd, rs, rd, (int)cnt);
    else                           a64_extr(b, sf, rd, rd, rs, (int)(bits - cnt));
    return 1;
}

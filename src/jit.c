/*
 * The JIT: guest basic blocks translated to native arm64, with the
 * interpreter as the fallback for anything it will not encode.
 *
 * ---- i386 ----
 * 32-bit blocks are compiled from a whitelist of instructions (m32_inline_ok),
 * with no superblocks and 0x67/16-bit addressing left to the interpreter.
 * Effective addresses wrap at 2^32 before the host mapping is applied, and pin
 * class 2 (the 64-bit CALL/RET protocol) is never selected for them.
 *
 * A cmp or test, an inc or dec, or an add/sub then inc/dec, that ends a block
 * in a jcc fuses with it as in 64-bit code; the only difference is the
 * fall-through address, which wraps at 2^32 like EIP.  Unfused, every loop
 * branch wrote a flag record for the jcc to read back: `add eax, ebx ; dec
 * edi ; jnz` ran at 7.1 ns an iteration and runs at 0.4.
 *
 * The condition forwarding is on as well - NZCV from an adjacent producer, E,
 * NE, S and NS from a result register, a comis redone by fcmp - and so are the
 * mov+logic and add+inc pairs and mov sinking into a shift.  Forwarding is
 * only sound when the consumer is translated, since the producer then leaves
 * its flags in NZCV alone, so cc_consumer_inline_ok asks m32_inline_ok about a
 * 32-bit consumer: a cmov through 16-bit addressing is interpreted, and read a
 * stale record.  (So did a 64-bit cmov whose memory operand has a 0x67
 * prefix.)  After a fused pair the translate loop takes the pair's second
 * instruction as the latest flag producer; it went on naming the first, so
 * after `add ; inc` a jb or setb read the inc's record as an add's, in both
 * modes.  The FP batch and lane-0 machinery stays off in 32-bit blocks: the
 * differential has no SSE arithmetic corpus to hold it to.
 *
 * An fs- or gs-relative operand adds the segment base to the wrapped address
 * without wrapping the sum, as ocerz_ea does, and then takes the same guard and
 * translation as any other operand.  32-bit Windows code reads fs:[0] for every
 * SEH frame it pushes and pops and fs:[0x18] and fs:[0x2c] for the TEB and its
 * TLS slots, and each of those was a slow call.  push and pop with a memory
 * operand are translated too, since `push dword fs:[0]` opens every frame.
 *
 * In the Wine layout a 32-bit stack slot always lies in the low window, so
 * push, pop, call, ret and leave reach it with low_base or'ed into the
 * zero-extended esp, with no range test (low_guard_fast_ok has checked that
 * orr can encode low_base).  They used to want guest_base in JGB, which that
 * layout does not keep, so in the one layout 32-bit code runs in every one of
 * them was a slow call: an SEH frame of two pushes cost 37 ns, now 9.
 *
 * Arithmetic on memory, xchg, xadd and cmpxchg go through emit_rmw_mem as in
 * 64-bit blocks: in ordered mode the locked and exchanging forms are LSE
 * atomics, and an access that is not naturally aligned leaves for the
 * interpreter out of line.  cmpxchg8b is a casal of EDX:EAX against ECX:EBX
 * that writes ZF into the materialized flags and EDX:EAX only on a mismatch,
 * and xchg between two registers goes through a scratch register.  A block
 * has room for 32 out-of-line arms; past that the slow call goes inline behind
 * a branch.  emit_rmw_mem used to give up there after emitting its atomic, so
 * the slow call emitted in its place performed an aligned access a second
 * time, and a misaligned one spun on the alignment branch, never patched.
 *
 * bt, bts, btr and btc on a register and bt on memory are emit_bt's, whose
 * 32-bit operand forms are those of 64-bit code; a register offset into memory
 * is sign-extended from its own size and added after the address wraps, as in
 * the interpreter.  bts, btr and btc on memory stay interpreted, in both modes.
 *
 * ---- bisection ----
 * OCERZ_INTERP_LO/HI and OCERZ_INTERP_RIP keep chosen ranges or addresses in
 * the interpreter, which is how a JIT miscompile is narrowed down;
 * OCERZ_CHAINCHECK validates every published jump target against the arena,
 * OCERZ_INVMAP_CHECK asserts the region map's one invariant, OCERZ_BTRACE
 * records block entries, and the OCERZ_UNSAFE_* knobs are measurement aids that
 * deliberately produce wrong state and must never be enabled outside a
 * benchmark.

 *
 * Set by emit_mem_ea when the address it just formed is a constant, for the guard that follows.
 *
 * A 64-bit constant that takes three or four instructions to build is one load from the block's literal pool.
 *
 * The exactness test for VX2 = VX0 op VX1: falls through when exact, or branches to pe[] or ok[].
 *
 * Two pushes, or two pops, of 64-bit registers in a row, where the stack
 * delta is in use: one address, one stp or ldp, and rsp moved once afterwards.
 * rsp moves only after the access, so a fault restarts the pair at its first
 * instruction with nothing yet done; a pop pair never loads one register twice
 * (an ldp with equal destinations is unpredictable).  Prologues and epilogues
 * are these runs: fib's four pushes went from 12 instructions to 6.  The
 * call-frame forms and the push/pop renames own their instructions and are
 * left alone.  OCERZ_NO_STACK_PAIR=1 turns it off.
 */
#include "ocerz/jit_internal.h"

static int term_may_switch_mode(unsigned op);
static int try_inline(A64Buf *b, const X86Insn *insn, uint64_t need,
                      uint32_t **exit_sites, int *n_exits);
static int select_mem_base_hoist(const X86Insn *insns, int n, uint64_t rip);
static int stack_pair_reg(const X86Insn *in, int op);
static int emit_stack_pair(A64Buf *b, const X86Insn *a, const X86Insn *c, int i);
static int low_splice_ok(const X86Insn *insn);
static int rsp_run_member(const X86Insn *insns, int j, int n, int fast3);
static int inline_calls_off(void);
static int splice_callee(uint64_t target, uint64_t ret_rip, uint64_t self_rip,
                         X86Insn *scratch, int *vn, int depth);
static int ret_flags_live(void);
static int ret_flags_live_at(uint64_t rip);
static uint64_t ret_seam_live(const X86Insn *insns, int n);
static int ps_cmp(const void *a, const void *bb);

int g_flaglive_log;

static uint32_t g_rsp_lag;

static int g_flag_producer_operands_intact;

static int g_xlat_n;

static int g_lowstack_check;

static int g_lowstack_from;

static int g_align_any;

static int g_align_blk;

int rsp_ptr3(void)
{
    static int off = -1;
    if (off < 0) off = getenv("OCERZ_RSP_VALUE") != NULL;
    return !off;
}

int g_pin_class_fwd(void) { return g_pin_class; }

int ocerz_jit_time_xlat;

uint64_t ocerz_jit_xlat_ns;

_Atomic unsigned long long ps_hits;

_Atomic unsigned long long ps_misses;

_Atomic unsigned long long ps_steps;

static const char *ps_shape_name[9] = { "push", "pop", "test", "movsxd", "call", "ret",
                                        "jmp", "jmpind", "jmpmem" };

struct JitState_ps_retsite ps_retsite[PS_RETSITE_N];

uint64_t ps_t0;

__thread int t_xlat_overflow;

static int g_keep_cap;

int is_terminator(unsigned op)
{
    switch (op) {
    case OCERZ_OP_JMP:
    case OCERZ_OP_JCC:
    case OCERZ_OP_JRCXZ:
    case OCERZ_OP_LOOP:
    case OCERZ_OP_LOOPE:
    case OCERZ_OP_LOOPNE:
    case OCERZ_OP_CALL:
    case OCERZ_OP_RET:
    case OCERZ_OP_IRET:

    case OCERZ_OP_JMPF:
    case OCERZ_OP_CALLF:
    case OCERZ_OP_RETF:
    case OCERZ_OP_SYSCALL:
    case OCERZ_OP_INT3:
    case OCERZ_OP_INT:
    case OCERZ_OP_UD2:
    case OCERZ_OP_HLT:
        return 1;
    default:
        return 0;
    }
}

static int term_may_switch_mode(unsigned op)
{
    return op == OCERZ_OP_IRET || op == OCERZ_OP_JMPF ||
           op == OCERZ_OP_CALLF || op == OCERZ_OP_RETF;
}

static int g_cur_fpb = -1;

int g_xlat_ftop = -1;

int m32_inline_ok(const X86Insn *insn)
{
    if (insn->addrsize != 4)
        return 0;
    if (x87_inline_ok(insn)) return 1;
    switch (insn->op) {
    case OCERZ_OP_NOP: case OCERZ_OP_PAUSE:
    case OCERZ_OP_PREFETCH: case OCERZ_OP_CLFLUSH:
    case OCERZ_OP_MOV: case OCERZ_OP_MOVZX: case OCERZ_OP_MOVSX:
    case OCERZ_OP_ADD: case OCERZ_OP_SUB: case OCERZ_OP_CMP:
    case OCERZ_OP_AND: case OCERZ_OP_OR:  case OCERZ_OP_XOR:
    case OCERZ_OP_TEST: case OCERZ_OP_ADC: case OCERZ_OP_SBB:
    case OCERZ_OP_INC: case OCERZ_OP_DEC: case OCERZ_OP_NOT: case OCERZ_OP_NEG:
    case OCERZ_OP_SHL: case OCERZ_OP_SHR: case OCERZ_OP_SAR:
    case OCERZ_OP_ROL: case OCERZ_OP_ROR:
    case OCERZ_OP_SHLD: case OCERZ_OP_SHRD:
    case OCERZ_OP_MUL: case OCERZ_OP_IMUL:
    case OCERZ_OP_DIV: case OCERZ_OP_IDIV:
    case OCERZ_OP_CBW: case OCERZ_OP_CWD:
    case OCERZ_OP_CMOVCC: case OCERZ_OP_SETCC: case OCERZ_OP_BSWAP:
    case OCERZ_OP_BSF: case OCERZ_OP_BSR:
    case OCERZ_OP_TZCNT: case OCERZ_OP_LZCNT: case OCERZ_OP_POPCNT:
    case OCERZ_OP_LEA:
    case OCERZ_OP_PUSH: case OCERZ_OP_POP: case OCERZ_OP_LEAVE:
    case OCERZ_OP_PMOVMSKB:
    case OCERZ_OP_XCHG: case OCERZ_OP_XADD: case OCERZ_OP_CMPXCHG: case OCERZ_OP_CMPXCHGXB:
    case OCERZ_OP_BT: case OCERZ_OP_BTS: case OCERZ_OP_BTR: case OCERZ_OP_BTC:
        return 1;
    default:
        return insn->op >= OCERZ_OP_MOVUPS && insn->op <= OCERZ_OP_PBLENDVB;
    }
}

static int try_inline(A64Buf *b, const X86Insn *insn, uint64_t need,
                      uint32_t **exit_sites, int *n_exits)
{
    if (ocerz_insn_has_mmx(insn)) return insn->mode32 ? 0 : emit_mmx(b, insn, exit_sites, n_exits);
    if (insn->vex) return emit_vex(b, insn, exit_sites, n_exits);
    if (insn->mode32 && !m32_inline_ok(insn))
        return 0;
    if (insn->op > OCERZ_OP_X87_FIRST && insn->op < OCERZ_OP_SSE_FIRST)
        return emit_x87(b, insn, need, exit_sites, n_exits);
    if (insn->op == OCERZ_OP_NOP || insn->op == OCERZ_OP_PAUSE ||
        insn->op == OCERZ_OP_PREFETCH || insn->op == OCERZ_OP_CLFLUSH)
        return 1;
    if ((insn->op == OCERZ_OP_XOR || insn->op == OCERZ_OP_CWD) && g_cur_insns && insn == &g_cur_insns[g_cur_insn_idx] &&
        rdx_prep_skippable(g_cur_insns, g_cur_insn_idx, g_cur_insns_n, need)) {
        g_div_prev_skipped = 1;
        return 1;
    }
    g_div_prev_skipped = 0;

    if (insn->op == OCERZ_OP_MOV) {
        const X86Operand *d = &insn->ops[0];
        const X86Operand *s = &insn->ops[1];
        if (d->kind == OCERZ_OPK_REG && d->high8)
            return 0;
        if (d->kind == OCERZ_OPK_REG && (d->size == 4 || d->size == 8)) {
            if (s->kind == OCERZ_OPK_REG && !s->high8 && s->size == d->size) {
                int sz4 = d->size == 4;
                int ds = pin_slot(d->reg);
                int ss = pin_slot(s->reg);
                int host_rsp = rsp_is_ptr() &&
                    (d->reg == OCERZ_RSP || s->reg == OCERZ_RSP);
                if (host_rsp && !sz4 && d->reg == s->reg)
                    return 1;
                if (rsp_is_ptr() && !sz4 && s->reg == OCERZ_RSP &&
                    d->reg != OCERZ_RSP && ds >= 0 && ss >= 0) {
                    if (jgb_usable())
                        a64_sub_reg(b, 1, pin_hreg(ds), pin_hreg(ss), JGB, 0);
                    else {
                        a64_mov_imm64(b, pin_hreg(ds), ocerz_guest_base);
                        a64_sub_reg(b, 1, pin_hreg(ds), pin_hreg(ss), pin_hreg(ds), 0);
                    }
                    return 1;
                }
                if (ds >= 0 && ss >= 0 && !host_rsp) {

                    if (ds != ss || sz4)
                        a64_mov_reg(b, sz4 ? 0 : 1, pin_hreg(ds), pin_hreg(ss));
                    return 1;
                }
                emit_gpr_rd(b, sz4 ? 0 : 1, JT0, s->reg);
                emit_gpr_wr(b, JT0, d->reg);
                return 1;
            }
            if (s->kind == OCERZ_OPK_IMM) {
                uint64_t v = s->imm;
                if (d->size == 4)
                    v &= 0xffffffffull;
                int ds = pin_slot(d->reg);
                if (ds >= 0 && !(rsp_is_ptr() && d->reg == OCERZ_RSP))
                    a64_mov_imm64(b, pin_hreg(ds), v);
                else {
                    a64_mov_imm64(b, JT0, v);
                    emit_gpr_wr(b, JT0, d->reg);
                }
                return 1;
            }
        }
        if (d->kind == OCERZ_OPK_REG && (d->size == 1 || d->size == 2) &&
            pin_slot(d->reg) >= 0 && !(rsp_is_ptr() && d->reg == OCERZ_RSP)) {
            int rd = pin_hreg(pin_slot(d->reg));
            if (s->kind == OCERZ_OPK_REG && !s->high8 && s->size == d->size && pin_slot(s->reg) >= 0 &&
                !(rsp_is_ptr() && s->reg == OCERZ_RSP)) {
                if (s->reg != d->reg) a64_bfi(b, 1, rd, pin_hreg(pin_slot(s->reg)), 0, d->size * 8);
                return 1;
            }
            if (s->kind == OCERZ_OPK_IMM) {
                uint64_t v = (uint64_t)s->imm & ((1ull << (d->size * 8)) - 1);
                a64_mov_imm64(b, JT0, v);
                a64_bfi(b, 1, rd, JT0, 0, d->size * 8);
                return 1;
            }
        }
        if (s->kind == OCERZ_OPK_MEM || d->kind == OCERZ_OPK_MEM)
            return emit_mov_mem(b, insn, exit_sites, n_exits);
        return 0;
    }

    switch (insn->op) {
    case OCERZ_OP_ADD:
    case OCERZ_OP_SUB:
    case OCERZ_OP_CMP:
    case OCERZ_OP_AND:
    case OCERZ_OP_OR:
    case OCERZ_OP_XOR:
    case OCERZ_OP_TEST:

        if (emit_cmp_test_narrow(b, insn, need, exit_sites, n_exits))
            return 1;
        if (emit_arith_narrow(b, insn, need))
            return 1;
        if (insn->ops[0].kind == OCERZ_OPK_MEM)
            return emit_rmw_mem(b, insn, need, exit_sites, n_exits);
        if (insn->ops[1].kind == OCERZ_OPK_MEM)
            return emit_arith_mem(b, insn, need, exit_sites, n_exits);
        return emit_arith(b, insn, need);
    case OCERZ_OP_INC:
    case OCERZ_OP_DEC:
        if (insn->ops[0].kind == OCERZ_OPK_MEM)
            return emit_rmw_mem(b, insn, need, exit_sites, n_exits);
        return emit_incdec(b, insn, need);
    case OCERZ_OP_XCHG:
        if (insn->mode32 && insn->ops[0].kind == OCERZ_OPK_REG && insn->ops[1].kind == OCERZ_OPK_REG)
            return emit_xchg_reg32(b, insn);
        return emit_rmw_mem(b, insn, need, exit_sites, n_exits);
    case OCERZ_OP_XADD:
    case OCERZ_OP_CMPXCHG:
        return emit_rmw_mem(b, insn, need, exit_sites, n_exits);
    case OCERZ_OP_CMPXCHGXB:
        return emit_cmpxchg8b(b, insn, exit_sites, n_exits);
    case OCERZ_OP_SHL:
    case OCERZ_OP_SHR:
    case OCERZ_OP_SAR:
        if (insn->ops[1].kind == OCERZ_OPK_REG)
            return emit_shift_cl(b, insn, need);
        return emit_shift(b, insn, need);
    case OCERZ_OP_ROL:
    case OCERZ_OP_ROR:
        return emit_rot(b, insn, need);
    case OCERZ_OP_NOT:
    case OCERZ_OP_NEG:
        if (insn->ops[0].kind == OCERZ_OPK_MEM)
            return emit_rmw_mem(b, insn, need, exit_sites, n_exits);
        return emit_not_neg(b, insn, need);
    case OCERZ_OP_ADC:
    case OCERZ_OP_SBB:
        return emit_adc_sbb(b, insn, need);
    case OCERZ_OP_CBW:
    case OCERZ_OP_CWD:
        return emit_cbw_cwd(b, insn);
    case OCERZ_OP_DIV:
    case OCERZ_OP_IDIV:
        return emit_div(b, insn, exit_sites, n_exits);
    case OCERZ_OP_CMOVCC:
        if (ENV_ON("OCERZ_NO_INLINE_CMOV")) return 0;
        return emit_cmov(b, insn, exit_sites, n_exits);
    case OCERZ_OP_SETCC:
        if (ENV_ON("OCERZ_NO_INLINE_SETCC")) return 0;
        return emit_setcc(b, insn);
    case OCERZ_OP_BSWAP:
        return emit_bswap(b, insn);
    case OCERZ_OP_MOVUPS: case OCERZ_OP_MOVAPS: case OCERZ_OP_MOVDQA: case OCERZ_OP_MOVDQU:
    case OCERZ_OP_MOVSS: case OCERZ_OP_MOVSDX:
    case OCERZ_OP_MOVLPS: case OCERZ_OP_MOVHPS:
    case OCERZ_OP_ADDSS: case OCERZ_OP_ADDSD: case OCERZ_OP_ADDPS: case OCERZ_OP_ADDPD:
    case OCERZ_OP_SUBSS: case OCERZ_OP_SUBSD: case OCERZ_OP_SUBPS: case OCERZ_OP_SUBPD:
    case OCERZ_OP_MULSS: case OCERZ_OP_MULSD: case OCERZ_OP_MULPS: case OCERZ_OP_MULPD:
    case OCERZ_OP_DIVSS: case OCERZ_OP_DIVSD: case OCERZ_OP_DIVPS: case OCERZ_OP_DIVPD:
    case OCERZ_OP_MAXSS: case OCERZ_OP_MAXSD: case OCERZ_OP_MINSS: case OCERZ_OP_MINSD:
    case OCERZ_OP_MAXPS: case OCERZ_OP_MAXPD: case OCERZ_OP_MINPS: case OCERZ_OP_MINPD:
    case OCERZ_OP_SQRTSS: case OCERZ_OP_SQRTSD: case OCERZ_OP_SQRTPS: case OCERZ_OP_SQRTPD:
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
    case OCERZ_OP_PMULUDQ: case OCERZ_OP_PBLENDW: case OCERZ_OP_PALIGNR:
    case OCERZ_OP_PMULHW: case OCERZ_OP_PMULHUW: case OCERZ_OP_PACKSSWB: case OCERZ_OP_PACKUSDW:
    case OCERZ_OP_PMULDQ: case OCERZ_OP_PSADBW: case OCERZ_OP_PMADDUBSW:
    case OCERZ_OP_PABSB: case OCERZ_OP_PABSW: case OCERZ_OP_PABSD: case OCERZ_OP_PSIGNB: case OCERZ_OP_PSIGNW: case OCERZ_OP_PSIGND:
    case OCERZ_OP_PHADDW: case OCERZ_OP_PHADDD: case OCERZ_OP_PHSUBW: case OCERZ_OP_PHSUBD: case OCERZ_OP_PHADDSW: case OCERZ_OP_PHSUBSW:
    case OCERZ_OP_PSHUFLW: case OCERZ_OP_PSHUFHW: case OCERZ_OP_PSLLDQ: case OCERZ_OP_PSRLDQ:
    case OCERZ_OP_BLENDPS: case OCERZ_OP_BLENDPD: case OCERZ_OP_MOVSHDUP: case OCERZ_OP_MOVSLDUP:
    case OCERZ_OP_MOVMSKPS: case OCERZ_OP_MOVMSKPD: case OCERZ_OP_CMPPS: case OCERZ_OP_CMPPD:
    case OCERZ_OP_CVTTPS2DQ: case OCERZ_OP_CVTPS2DQ: case OCERZ_OP_CVTDQ2PD:
    case OCERZ_OP_CVTPS2PD: case OCERZ_OP_CVTPD2PS:
    case OCERZ_OP_AESENC: case OCERZ_OP_AESENCLAST: case OCERZ_OP_AESDEC: case OCERZ_OP_AESDECLAST:
    case OCERZ_OP_AESIMC: case OCERZ_OP_AESKEYGENASSIST: case OCERZ_OP_PCLMULQDQ:
    case OCERZ_OP_UCOMISS: case OCERZ_OP_UCOMISD: case OCERZ_OP_COMISS: case OCERZ_OP_COMISD:
    case OCERZ_OP_CVTTSD2SI: case OCERZ_OP_CVTTSS2SI: case OCERZ_OP_CVTSI2SD: case OCERZ_OP_CVTSI2SS:
    case OCERZ_OP_CVTSD2SS: case OCERZ_OP_CVTSS2SD: case OCERZ_OP_CVTDQ2PS:
    case OCERZ_OP_MOVD: case OCERZ_OP_MOVQX: case OCERZ_OP_PSHUFD:
    case OCERZ_OP_PINSRB: case OCERZ_OP_PINSRW: case OCERZ_OP_PINSRD: case OCERZ_OP_PINSRQ:
    case OCERZ_OP_PEXTRB: case OCERZ_OP_PEXTRW: case OCERZ_OP_PEXTRD: case OCERZ_OP_PEXTRQ:
    case OCERZ_OP_PMOVSXBW: case OCERZ_OP_PMOVSXBD: case OCERZ_OP_PMOVSXBQ: case OCERZ_OP_PMOVSXWD: case OCERZ_OP_PMOVSXWQ: case OCERZ_OP_PMOVSXDQ:
    case OCERZ_OP_PMOVZXBW: case OCERZ_OP_PMOVZXBD: case OCERZ_OP_PMOVZXBQ: case OCERZ_OP_PMOVZXWD: case OCERZ_OP_PMOVZXWQ: case OCERZ_OP_PMOVZXDQ:
    case OCERZ_OP_ROUNDSS: case OCERZ_OP_ROUNDSD: case OCERZ_OP_ROUNDPS: case OCERZ_OP_ROUNDPD:
    case OCERZ_OP_PSHUFB:
    case OCERZ_OP_PUNPCKLBW: case OCERZ_OP_PUNPCKLWD: case OCERZ_OP_PUNPCKLDQ: case OCERZ_OP_PUNPCKLQDQ:
    case OCERZ_OP_PUNPCKHBW: case OCERZ_OP_PUNPCKHWD: case OCERZ_OP_PUNPCKHDQ: case OCERZ_OP_PUNPCKHQDQ:
    case OCERZ_OP_UNPCKLPD: case OCERZ_OP_UNPCKHPD: case OCERZ_OP_MOVLHPS: case OCERZ_OP_MOVHLPS:
    case OCERZ_OP_UNPCKLPS: case OCERZ_OP_UNPCKHPS:
    case OCERZ_OP_CMPSS: case OCERZ_OP_CMPSDX:
    case OCERZ_OP_BLENDVPD: case OCERZ_OP_BLENDVPS: case OCERZ_OP_PBLENDVB:
    case OCERZ_OP_MOVDDUP: case OCERZ_OP_SHUFPS: case OCERZ_OP_SHUFPD: case OCERZ_OP_INSERTPS:
    case OCERZ_OP_PSLLW: case OCERZ_OP_PSLLD: case OCERZ_OP_PSLLQ: case OCERZ_OP_PSRLW:
    case OCERZ_OP_PSRLD: case OCERZ_OP_PSRLQ: case OCERZ_OP_PSRAW: case OCERZ_OP_PSRAD:
        return emit_sse(b, insn, exit_sites, n_exits);
    case OCERZ_OP_MOVNTI:
        return emit_mov_mem(b, insn, exit_sites, n_exits);
    case OCERZ_OP_CRC32:
        return emit_crc32(b, insn);
    case OCERZ_OP_MUL:
        return emit_mul_wide(b, insn, need, 0);
    case OCERZ_OP_IMUL:
        if (insn->nops == 1)
            return emit_mul_wide(b, insn, need, 1);
        if (insn->ops[0].kind == OCERZ_OPK_MEM)
            return 0;
        if (((insn->nops > 1 && insn->ops[1].kind == OCERZ_OPK_MEM) ||
             (insn->nops > 2 && insn->ops[2].kind == OCERZ_OPK_MEM)) && ENV_ON("OCERZ_NO_IMUL_MEM"))
            return 0;
        return emit_imul(b, insn, need);
    case OCERZ_OP_LEA:
        return emit_lea(b, insn);
    case OCERZ_OP_BSF: case OCERZ_OP_BSR: case OCERZ_OP_TZCNT: case OCERZ_OP_LZCNT: case OCERZ_OP_POPCNT:
        return emit_bitscan(b, insn, need);
    case OCERZ_OP_PMOVMSKB:
        return emit_pmovmskb(b, insn);
    case OCERZ_OP_BT: case OCERZ_OP_BTS: case OCERZ_OP_BTR: case OCERZ_OP_BTC:
        return emit_bt(b, insn, need, exit_sites, n_exits);
    case OCERZ_OP_LEAVE:
        return emit_leave(b, insn, exit_sites, n_exits);
    case OCERZ_OP_SHLD: case OCERZ_OP_SHRD:
        return emit_shiftd(b, insn, need);
    case OCERZ_OP_PUSH:
    case OCERZ_OP_POP:
        if (insn->nops == 1 && insn->ops[0].kind == OCERZ_OPK_MEM)
            return emit_push_pop_mem(b, insn, exit_sites, n_exits);
        return emit_push_pop(b, insn, exit_sites, n_exits);
    case OCERZ_OP_MOVZX:
    case OCERZ_OP_MOVSX:
        return emit_movx(b, insn, insn->op == OCERZ_OP_MOVSX, exit_sites, n_exits);
    case OCERZ_OP_MOVSXD:
        return emit_movsxd(b, insn, exit_sites, n_exits);
    default:
        return 0;
    }
}

MarkSet g_x87spec_marks;

static int select_mem_base_hoist(const X86Insn *insns, int n, uint64_t rip)
{
    g_mem_hoist_aux_disp = 0;
    g_mem_hoist_aux_index = -1;
    { static int dis = -1; if (dis < 0) dis = getenv("OCERZ_NO_HOIST") ? 1 : 0; if (dis) return -1; }
    if (g_no_chain || mem_guard_needed() ||
        !mem_native_store_ok() || n < 2)
        return -1;
    const X86Insn *term = &insns[n - 1];
    int self_loop = (term->op == OCERZ_OP_JCC && term->ops[0].kind == OCERZ_OPK_IMM &&
                     term->ops[0].imm == rip) ||
                    (term->op == OCERZ_OP_JMP && term->ops[0].kind == OCERZ_OPK_IMM &&
                     term->ops[0].imm == rip);
    static int hoist_all = -1; if (hoist_all < 0) hoist_all = getenv("OCERZ_NO_HOIST_ALL") ? 0 : 1;
    if (!self_loop && (!hoist_all || ocerz_guest_base == 0)) return -1;
    static int mc = -1; if (mc < 0) { const char *e = getenv("OCERZ_HOIST_MIN"); mc = e ? atoi(e) : 2; }
    int min_count = self_loop ? 1 : mc;

    int count[16] = {0};
    int aux[16] = {0};
    for (int i = 0; i < n; i++) {
        const X86Insn *in = &insns[i];
        if (in->seg != OCERZ_SEG_NONE || in->addrsize != 8) continue;
        for (int k = 0; k < in->nops; k++) {
            const X86Operand *mem = &in->ops[k];
            if (mem->kind != OCERZ_OPK_MEM || mem->riprel || mem->base == OCERZ_REG_NONE) continue;
            if (pin_slot(mem->base) < 0) continue;
            unsigned bb = mem->base & 15;
            count[bb]++;
            if (mem->index != OCERZ_REG_NONE && (mem->scale & 3) == 0 && pin_slot(mem->index) >= 0 &&
                !(rsp_is_ptr() && (mem->index == OCERZ_RSP || mem->base == OCERZ_RSP)))
                count[mem->index & 15]++;
            if (g_pin_class != 2 && mem->index != OCERZ_REG_NONE &&
                mem->disp != 0 && mem->disp >= -4095 && mem->disp <= 4095 && aux[bb] == 0)
                aux[bb] = (int)mem->disp;
        }
    }
    int best = -1, bestn = 0, second = -1, secondn = 0, third = -1, thirdn = 0;
    for (int r = 0; r < 16; r++) {
        if (count[r] <= 0 || count[r] <= thirdn) continue;
        if (rsp_is_ptr() && r == OCERZ_RSP) continue;
        int written = 0;
        for (int i = 0; i < n && !written; i++)
            written = insn_may_write_gpr(&insns[i], (unsigned)r);
        if (written) continue;
        if (count[r] > bestn) { third = second; thirdn = secondn; second = best; secondn = bestn; best = r; bestn = count[r]; }
        else if (count[r] > secondn) { third = second; thirdn = secondn; second = r; secondn = count[r]; }
        else { third = r; thirdn = count[r]; }
    }
    if (ENV_ON("OCERZ_HOISTLOG") && best < 0)
        fprintf(stderr, "HOIST rip=%#llx no candidate (self_loop=%d n=%d)\n", (unsigned long long)rip, self_loop, n);
    if (best < 0 || bestn < min_count) return -1;
    if (secondn < min_count) second = -1;
    if (thirdn < min_count) third = -1;
    { static int no2 = -1; if (no2 < 0) no2 = getenv("OCERZ_NO_HOIST2") ? 1 : 0; if (no2) second = third = -1; }
    { static int no3 = -1; if (no3 < 0) no3 = getenv("OCERZ_NO_HOIST3") ? 1 : 0; if (no3 || g_pin_class != 3) third = -1; }
    g_mem_hoist_aux_disp = aux[best];
    g_mem_hoist_greg2 = second;
    g_mem_hoist_greg3 = third;
    g_mem_hoist_aux_index = -1;
    {
        int icnt[16][4] = {{0}};
        int ilast[16][4] = {{0}};
        int total = 0;
        for (int i = 0; i < n; i++) {
            const X86Insn *in = &insns[i];
            if (in->seg != OCERZ_SEG_NONE || in->addrsize != 8) continue;
            for (int k = 0; k < in->nops; k++) {
                const X86Operand *mem = &in->ops[k];
                if (mem->kind != OCERZ_OPK_MEM || mem->riprel || mem->base != (unsigned)best) continue;
                total++;
                if (mem->index == OCERZ_REG_NONE || pin_slot(mem->index) < 0) continue;
                if (rsp_is_ptr() && mem->index == OCERZ_RSP) continue;
                icnt[mem->index & 15][mem->scale & 3]++;
                ilast[mem->index & 15][mem->scale & 3] = i;
            }
        }
        int bi = -1, bs = 0, bc = 0;
        for (int r = 0; r < 16; r++) for (int sc = 0; sc < 4; sc++)
            if (icnt[r][sc] > bc) { bc = icnt[r][sc]; bi = r; bs = sc; }
        if (bi >= 0 && bc >= 2 && bi != best) {
            int written = 0;
            for (int i = 0; i < ilast[bi][bs] && !written; i++)
                written = insn_may_write_gpr(&insns[i], (unsigned)bi);
            if (!written) {
                g_mem_hoist_aux_index = bi;
                g_mem_hoist_aux_scale = bs;
                g_mem_hoist_aux_disp = 0;
            }
        }
        (void)total;
    }
    if (ENV_ON("OCERZ_HOISTLOG"))
        fprintf(stderr, "HOIST rip=%#llx base=%d(n=%d) aux=%d second=%d(n=%d) third=%d(n=%d)\n", (unsigned long long)rip, best, bestn, aux[best], second, secondn, third, thirdn);
    return best;
}

static uint8_t  g_ic_kind[JIT_MAX_BLOCK_INSNS];

static uint64_t g_ic_expect[JIT_MAX_BLOCK_INSNS];

static uint8_t  g_ic_pushelide[JIT_MAX_BLOCK_INSNS];

static int32_t  g_ic_pair_rj[JIT_MAX_BLOCK_INSNS];

static uint8_t  g_promo_reg[JIT_MAX_BLOCK_INSNS];

static int32_t  g_promo_mate[JIT_MAX_BLOCK_INSNS];

static int32_t  g_promo_push_of[JIT_MAX_BLOCK_INSNS];

static unsigned long long g_promo_seq[JIT_MAX_BLOCK_INSNS];

static int stack_pair_reg(const X86Insn *in, int op)
{
    const X86Operand *o = &in->ops[0];
    if (in->op != op || in->mode32 || in->opsize != 8 || in->seg != OCERZ_SEG_NONE || in->nops != 1) return -1;
    if (o->kind != OCERZ_OPK_REG || o->high8 || o->size != 8 || (o->reg & 15) == OCERZ_RSP) return -1;
    int s = pin_slot(o->reg);
    return s < 0 ? -1 : pin_hreg(s);
}

static int emit_stack_pair(A64Buf *b, const X86Insn *a, const X86Insn *c, int i)
{
    if (!g_lowstack || !stack_plain_access_ok() || ENV_ON("OCERZ_NO_STACK_PAIR")) return 0;
    if (g_ic_kind[i] || g_ic_kind[i + 1] || g_promo_reg[i] || g_promo_reg[i + 1]) return 0;
    int hs = pin_hreg(pin_slot(OCERZ_RSP));
    int ra, rc;
    if ((ra = stack_pair_reg(a, OCERZ_OP_PUSH)) >= 0 && (rc = stack_pair_reg(c, OCERZ_OP_PUSH)) >= 0) {
        if (!mem_native_store_ok()) return 0;
        a64_add_reg(b, 1, JTA, hs, JGB, 0);
        a64_stp_off(b, rc, ra, JTA, -16);
        a64_sub_imm(b, 1, hs, hs, 16);
    } else if ((ra = stack_pair_reg(a, OCERZ_OP_POP)) >= 0 && (rc = stack_pair_reg(c, OCERZ_OP_POP)) >= 0) {
        if (ra == rc) return 0;
        a64_add_reg(b, 1, JTA, hs, JGB, 0);
        a64_ldp_off(b, ra, rc, JTA, 0);
        a64_add_imm(b, 1, hs, hs, 16);
    } else {
        return 0;
    }
    g_mov_skip[i + 1] = 1;
    return 1;
}

static int low_splice_ok(const X86Insn *insn)
{
    static int off = -1;
    if (off < 0) off = getenv("OCERZ_NO_LOW_SPLICE") ? 1 : 0;
    return !off && !insn->mode32 && g_pin_class == 3 && pin_slot(OCERZ_RSP) >= 0 && rsp_is_ptr() &&
           mem_native_store_ok();
}

static int rsp_run_member(const X86Insn *insns, int j, int n, int fast3)
{
    if (j >= n) return 0;
    if (g_promo_reg[j] && insns[j].op == OCERZ_OP_POP) return 1;
    if (g_ic_kind[j] == 3 && fast3) return 1;
    return 0;
}

static int inline_calls_off(void)
{
    static int off = -1;
    if (off < 0) off = getenv("OCERZ_NO_INLINE_CALL") != NULL;
    return off;
}

static int splice_callee(uint64_t target, uint64_t ret_rip, uint64_t self_rip,
                         X86Insn *scratch, int *vn, int depth)
{
    if (inline_calls_off() || target == self_rip || depth > 2)
        return 0;
    int start = *vn;
    uint64_t pc = target;
    for (int steps = 0; steps < 24; steps++) {
        if (*vn + 2 >= JIT_MAX_BLOCK_INSNS)
            goto fail;
        X86Insn *in = &scratch[*vn];
        if (jit_decode(pc, in, 0) != OCERZ_OK)
            goto fail;
        unsigned op = in->op;
        if (op == OCERZ_OP_RET) {
            if (in->nops != 0)
                goto fail;
            g_ic_kind[*vn] = 2;
            g_ic_expect[*vn] = ret_rip;
            (*vn)++;
            return 1;
        }
        if (op == OCERZ_OP_CALL && in->ops[0].kind == OCERZ_OPK_IMM && !in->mode32) {
            int at = *vn;
            g_ic_kind[at] = 1;
            (*vn)++;
            if (!splice_callee(in->ops[0].imm, pc + in->len, self_rip,
                               scratch, vn, depth + 1)) {
                g_ic_kind[at] = 0;
                goto fail;
            }
            pc += in->len;
            continue;
        }
        if (is_terminator(op) || op == OCERZ_OP_JCC || op == OCERZ_OP_SYSCALL ||
            op == OCERZ_OP_FXSAVE || op == OCERZ_OP_FXRSTOR ||
            op == OCERZ_OP_XSAVE || op == OCERZ_OP_XRSTOR)
            goto fail;
        (*vn)++;
        pc += in->len;
    }
fail:
    for (int k = start; k < *vn; k++) g_ic_kind[k] = 0;
    *vn = start;
    return 0;
}

static int ret_flags_live(void)
{
    static int v = -1;
    if (v < 0) v = getenv("OCERZ_RET_FLAGS_LIVE") != NULL;
    return v;
}

static int ret_flags_live_at(uint64_t rip)
{
    static int have = -1; static uint64_t lo, hi;
    if (have < 0) {
        const char *a = getenv("OCERZ_RETFL_LO"), *b = getenv("OCERZ_RETFL_HI");
        have = (a && b) ? 1 : 0;
        if (have) { lo = strtoull(a, NULL, 0); hi = strtoull(b, NULL, 0); }
    }
    if (have) return rip >= lo && rip < hi;
    return ret_flags_live();
}

static uint64_t ret_seam_live(const X86Insn *insns, int n)
{
    for (int i = n - 2; i >= 0; i--) {
        uint64_t def, use;
        ocerz_flags_defuse(&insns[i], &def, &use);
        if (!(def & JIT_ARITH_FLAGS)) continue;
        switch (insns[i].op) {
        case OCERZ_OP_CMP: case OCERZ_OP_TEST:
        case OCERZ_OP_BT: case OCERZ_OP_BTS: case OCERZ_OP_BTR: case OCERZ_OP_BTC:
        case OCERZ_OP_CMPXCHG:
            return OCERZ_FL_ALL;
        default:
            return 0;
        }
    }
    return OCERZ_FL_ALL;
}

JitBlock *translate(OcerzJit *jit, uint64_t rip, int mode32)
{
    g_xlat_mode32 = mode32;

    if (ocerz_exc_trap_rip && rip == ocerz_exc_trap_rip)
        return NULL;
    { extern uint64_t ocerz_cxa_throw_rip; if (ocerz_cxa_throw_rip && rip == ocerz_cxa_throw_rip) return NULL; }

    if (churn_blacklisted(rip)) {
        churn_note_refusal(rip);
        static _Atomic unsigned long long refn;
        static int clog = -1;
        if (clog < 0) clog = getenv("OCERZ_CHURNLOG") ? 1 : 0;
        if (clog && (++refn & 0xfff) == 0)
            fprintf(stderr, "ocerz: CHURNREF[%d] n=%llu rip=%#llx\n", (int)getpid(),
                    (unsigned long long)refn, (unsigned long long)rip);
        return NULL;
    }

    {
        static uint64_t ilo = 0, ihi = 0, ilo2 = 0, ihi2 = 0; static int irng = -1;
        if (irng < 0) {
            const char *l = getenv("OCERZ_INTERP_LO"), *h = getenv("OCERZ_INTERP_HI");
            if (l && h) { ilo = strtoull(l, NULL, 0); ihi = strtoull(h, NULL, 0); }
            const char *l2 = getenv("OCERZ_INTERP_LO2"), *h2 = getenv("OCERZ_INTERP_HI2");
            if (l2 && h2) { ilo2 = strtoull(l2, NULL, 0); ihi2 = strtoull(h2, NULL, 0); }
            irng = (ilo < ihi || ilo2 < ihi2) ? 1 : 0;
        }
        if (irng && ((rip >= ilo && rip < ihi) || (rip >= ilo2 && rip < ihi2)))
            return NULL;
    }
    {
        static uint64_t irips[8];
        static int n_irips = -1;
        if (n_irips < 0) {
            const char *e = getenv("OCERZ_INTERP_RIP");
            n_irips = 0;
            while (e && *e && n_irips < 8) {
                irips[n_irips++] = strtoull(e, NULL, 0);
                e = strchr(e, ',');
                if (e) e++;
            }
        }
        for (int i = 0; i < n_irips; i++)
            if (irips[i] == rip)
                return NULL;
    }

    const OcerzTcRecHead *tc_hit = NULL;
    int tcm = ocerz_tcache_mode();
    g_tc_rec = 0;
    g_tc_bad = 0;
    g_tc_learned = 0;
    g_tc_nrel = 0;
    g_tc_ndlog = 0;
    g_tc_nbytes = 0;
    g_tc_dbar = 0;
    if (tcm != OCERZ_TC_OFF)
        tc_log_init();
    if ((tcm == OCERZ_TC_ON || tcm == OCERZ_TC_VERIFY) && tc_usable(jit) && tc_keepable(rip)) {
        g_tc_rec = 1;
        g_tc_key = tc_key(rip, mode32);
        uint64_t jk = jit_key(rip, mode32);
        if (!al_marked(jk) && !al_marked(jk | AL_BLK_TAG) && !cp_marked(jk) && !tc_noload_has(jk))
            tc_hit = ocerz_tcache_find(g_tc_key);
        {
            static int tr = -1;
            if (tr < 0) tr = getenv("OCERZ_TCACHE_TRACE") ? 1 : 0;
            if (tr && g_tc_log > 0)
                fprintf(g_tc_lf, "ocerz: TCACHE[%d] %s key=%#llx\n", (int)getpid(), tc_hit ? "HIT" : "MISS",
                        (unsigned long long)g_tc_key);
        }
        if (tc_hit && tcm == OCERZ_TC_ON) {
            uint64_t t0 = ocerz_jit_time_xlat ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
            JitBlock *lb = tc_load(jit, tc_hit, rip, mode32);
            if (ocerz_jit_time_xlat)
                __atomic_add_fetch(&ocerz_jit_xlat_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0, __ATOMIC_RELAXED);
            if (lb) {
                g_tc_rec = 0;
                return lb;
            }
        }
    }

    static int g_jitmeasure = -1;
    if (g_jitmeasure < 0)
        g_jitmeasure = getenv("OCERZ_JITMEASURE") ? 1 : 0;
    uint64_t xlat_t0 = (g_jitmeasure || ocerz_jit_time_xlat) ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
    X86Insn scratch[JIT_MAX_BLOCK_INSNS];
    int n = 0;
    uint64_t pc = rip;
    {
        volatile int vn = 0;
        volatile int vext = 0;
        volatile uint64_t vpc = rip;
        sigjmp_buf db;
        sigjmp_buf *prev_dr = ocerz_jit_decode_recover;
        memset(g_ic_kind, 0, sizeof g_ic_kind);
        memset(g_ic_pushelide, 0, sizeof g_ic_pushelide);
        memset(g_promo_reg, 0, sizeof g_promo_reg);
        if (sigsetjmp(db, 0) == 0) {
            ocerz_jit_decode_recover = &db;
            for (; vn < JIT_MAX_BLOCK_INSNS; ) {
                int rc = jit_decode(vpc, &scratch[vn], mode32);
                if (rc != OCERZ_OK)
                    break;
                unsigned op = scratch[vn].op;
                uint8_t len = scratch[vn].len;
                if (op == OCERZ_OP_CALL && !mode32 &&
                    scratch[vn].ops[0].kind == OCERZ_OPK_IMM &&
                    vn + 48 < JIT_MAX_BLOCK_INSNS) {
                    int at = (int)vn;
                    int vni = (int)vn + 1;
                    g_ic_kind[at] = 1;
                    if (splice_callee(scratch[at].ops[0].imm, vpc + len, rip,
                                      scratch, &vni, 1)) {
                        vn = vni;
                        vpc += len;
                        continue;
                    }
                    g_ic_kind[at] = 0;
                }
                vn++;
                if (is_terminator(op)) {
                    if (op == OCERZ_OP_JCC && !mode32 && superblock_enabled() && !g_no_chain &&
                        vext < SIDE_MAX && vn < JIT_MAX_BLOCK_INSNS - 1 &&
                        scratch[vn - 1].ops[0].kind == OCERZ_OPK_IMM &&
                        (scratch[vn - 1].ops[0].imm > vpc + len ||
                         (superblock_back_enabled() && scratch[vn - 1].ops[0].imm != rip &&
                          scratch[vn - 1].ops[0].imm < vpc))) {
                        vext++;
                        if (scratch[vn - 1].ops[0].imm > vpc + len &&
                            jcc_flip_wanted(scratch[vn - 1].rip)) {
                            g_tc_learned = 1;
                            uint64_t tgt = scratch[vn - 1].ops[0].imm;
                            scratch[vn - 1].ops[0].imm = vpc + len;
                            scratch[vn - 1].cc ^= 1;
                            vpc = tgt;
                            continue;
                        }
                        vpc += len;
                        continue;
                    }
                    break;
                }
                vpc += len;
            }
        }
        ocerz_jit_decode_recover = prev_dr;
        n = vn;
        pc = vpc;
    }
    g_xlat_n = n;
    if (ocerz_jitstat > 0)
        js_decoded_insns += (unsigned)n;
    if (n == 0) {
        if (ocerz_jitstat > 0) { js_xlat_fail++; js_fail_decode0++; js_note_fail(rip, JSR_DECODE0, 0); }
        return NULL;
    }

    JitBlock *blk = (JitBlock *)calloc(1, sizeof *blk);
    if (!blk) {
        if (ocerz_jitstat > 0) { js_xlat_fail++; js_fail_alloc++; js_note_fail(rip, JSR_ALLOC, n); }
        return NULL;
    }
    blk->insns = (X86Insn *)malloc((size_t)n * sizeof(X86Insn));
    if (!blk->insns) {
        free(blk);
        if (ocerz_jitstat > 0) { js_xlat_fail++; js_fail_alloc++; js_note_fail(rip, JSR_ALLOC, n); }
        return NULL;
    }
    memcpy(blk->insns, scratch, (size_t)n * sizeof(X86Insn));
    blk->n_insns = n;
    blk->key = jit_key(rip, mode32);
    blk->edges = calloc(JIT_MAX_EDGES, sizeof *blk->edges);
    if (!blk->edges) {
        free(blk->insns);
        free(blk);
        if (ocerz_jitstat > 0) { js_xlat_fail++; js_fail_alloc++; js_note_fail(rip, JSR_ALLOC, n); }
        return NULL;
    }
    if (g_keep_cap < n) {
        free(g_keep);
        g_keep = (uint8_t *)malloc((size_t)n);
        g_keep_cap = g_keep ? n : 0;
    }
    if (g_keep) memset(g_keep, 0, (size_t)n);
    g_keep_n = g_keep ? n : 0;
    g_cur_blk = blk;
    g_no_compact = 0;

    for (int g = 0; g < 16; g++)
        blk->guest_in_host[g] = -1;
    blk->n_pinned = 0;
    g_pin = NULL;
    g_pin_hold = NULL;
    g_n_pinned = 0;
    g_pin_class = 0;
    g_lowstack = 0;
    g_m32low = 0;

    g_defer = !g_no_regflags;
    blk->n_edges = 0;
    g_chain_target = 0;
    g_chain_keeps_jgb = 0;
    g_n_raslit = 0;
    g_tc_on = 0;
    g_tc_entry = NULL;
    g_tc_pool_off = UINT32_MAX;
    g_chain_epi = NULL;
    g_n_jcc_edges = 0;
    g_nzcv_want = 0; g_nzcv_from = -1;
    g_jcc_edge[0].cond_site = NULL; g_jcc_edge[1].cond_site = NULL;
    g_n_oslow = 0;
    g_n_garm = 0;
    g_n_nanool = 0;
    g_n_pe_real = 0;
    g_n_promo_real = 0;
    g_rsp_lag = 0;
    g_pe_insns = blk->insns;
    g_n_call_edges = 0;
    g_n_oolslow = 0;
    x87_reset();
    g_x87_btop = g_xlat_ftop;
    if (g_x87_btop >= 0 && mark_has(&g_x87spec_marks, jit_key(rip, mode32))) {
        g_x87_btop = -1;
        g_tc_learned = 1;
    }
    g_x87_spec = -1;
    g_x87_kcarry = 0;
    g_x87_spec_cut = 0;
    g_oolslow_pre = 0;
    g_n_stop_extra = 0;
    g_xlat_jit = jit;
    g_self_rip = rip;
    g_body_entry = NULL;
    g_loop_entry = NULL;
    g_l0_fixed = 0;
    g_lane_used = 0;
    g_l0_next = 0;
    g_n_undo_lanes = 0;
    g_l0_nlanes = L0_NLANES;
    g_zero_vreg = -1;
    memset(g_yc, -1, sizeof g_yc);
    g_yc_dirty = 0;
    memset(g_l0_fixed_lane, -1, sizeof g_l0_fixed_lane);
    {
        static int nozero = -1;
        if (nozero < 0) nozero = getenv("OCERZ_NO_ZEROREG") ? 1 : 0;
        int nv = 0;
        g_blk_ymm_write = 0;
        for (int i = 0; i < n; i++) {
            const X86Insn *in = &blk->insns[i];
            if (in->vex && !(in->vex & OCERZ_VEX_L) && !in->mode32 && in->nops > 0 && in->ops[0].kind == OCERZ_OPK_XMM) nv++;
            if (in->vex && (in->vex & OCERZ_VEX_L) && in->op != OCERZ_OP_VZEROUPPER) g_blk_ymm_write = 1;
        }
        if (!nozero && nv >= 2 && !g_xlat_mode32) g_zero_vreg = lane_reserve();
    }
    g_x87_lanes_on = 0;
    g_x87_lv = 0;
    for (int p = 0; p < 8; p++) g_x87_lane[p] = -1;
    if (!ENV_ON("OCERZ_NO_X87_LANES")) {
        int nx = 0, sse = 0;
        for (int i = 0; i < n; i++) {
            const X86Insn *in = &blk->insns[i];
            if (x87_inline_ok(in)) nx++;
            for (int k = 0; k < in->nops; k++)
                if (in->ops[k].kind == OCERZ_OPK_XMM || in->ops[k].kind == OCERZ_OPK_MMX) sse = 1;
        }
        if (nx >= 2 && !sse) {
            int got = 0;
            for (int p = 0; p < 8; p++) {
                int v = lane_reserve();
                if (v < 0) break;
                g_x87_lane[p] = (int8_t)v;
                got++;
            }
            g_x87_lanes_on = got == 8;
        }
    }
    g_stop_patch = NULL;
    g_n_stop_extra = 0;
    g_n_push_fix = 0;
    g_n_oolslow = 0;
    g_cp_guard = ocerz_commpage && (ENV_ON("OCERZ_CP_GUARD_ALL") || cp_marked(jit_key(rip, mode32)));
    g_low_top = ocerz_low_base && (ENV_ON("OCERZ_LOW_TOP_GUARD") || cp_marked(jit_key(rip, mode32)));
    { static int all = -1; if (all < 0) all = getenv("OCERZ_AL_GUARD_ALL") ? 1 : 0; if (all) g_al_all = 1; }
    g_align_blk = !g_plain_mem && al_marked(jit_key(rip, mode32) | AL_BLK_TAG);
    g_align_any = g_align_blk;
    for (int i = 0; i < n && !g_align_any && !g_plain_mem && g_al_n; i++)
        g_align_any = al_marked(jit_key(blk->insns[i].rip, mode32));
    g_align_guard = g_align_blk;
    if (g_cp_guard || g_align_any)
        g_tc_learned = 1;
    g_blk_ordered_loads = 0;
    g_push_entry = NULL;
    g_n_side = 0;
    g_stop_target = NULL;
    g_mem_hoist_greg = -1;
    g_low_hoist_greg = -1;
    g_n_low_hoist_bail = 0;
    g_mem_hoist_greg2 = -1;
    g_mem_hoist_greg3 = -1;
    g_mem_hoist_aux_index = -1;
    g_mem_hoist_aux_disp = 0;
    int fuse_cmp = n >= 2 &&
        can_fuse_cmp_test_jcc(&blk->insns[n - 2], &blk->insns[n - 1], rip);
    int fuse_incdec = n >= 2 &&
        can_fuse_incdec_jcc(&blk->insns[n - 2], &blk->insns[n - 1]);
    int fuse_pair = fuse_cmp || fuse_incdec;
    int fuse_self = fuse_cmp && blk->insns[n - 1].ops[0].imm == rip;

    static int g_pin_min = -1;
    if (g_pin_min < 0) {
        const char *e = getenv("OCERZ_PIN_MIN_INSNS");
        g_pin_min = e ? (int)strtol(e, NULL, 0) : 24;
        if (g_pin_min < 1)
            g_pin_min = 1;
    }
    const X86Insn *term = &blk->insns[n - 1];
    int call_region = !g_no_regflags && !ocerz_low_base && !mode32 &&
        (term->op == OCERZ_OP_CALL || term->op == OCERZ_OP_RET);
    if (call_region && term->op == OCERZ_OP_CALL) {
        call_region = term->ops[0].kind == OCERZ_OPK_IMM &&
                      decoded_call_region_entry(term->ops[0].imm);
    }
    if (call_region) {

        for (int i = 0; i < n - 1 && call_region; i++) {
            const X86Insn *in = &blk->insns[i];
            for (int k = 0; k < in->nops; k++) {
                const X86Operand *o = &in->ops[k];
                if ((o->kind == OCERZ_OPK_REG && (o->reg & 15) == OCERZ_RSP) ||
                    (o->kind == OCERZ_OPK_MEM &&
                     ((o->base != OCERZ_REG_NONE && (o->base & 15) == OCERZ_RSP) ||
                      (o->index != OCERZ_REG_NONE && (o->index & 15) == OCERZ_RSP)))) {
                    call_region = 0;
                    break;
                }
            }
        }
    }
    if (!call_region && !g_no_regflags && term->op == OCERZ_OP_JCC &&
        term->ops[0].kind == OCERZ_OPK_IMM) {
        uint64_t taken = term->ops[0].imm;
        uint64_t fall = term->rip + term->len;
        call_region = call_body_successor(taken) &&
                      call_body_successor(fall);
        for (int i = 0; i < n - 1 && call_region; i++) {
            const X86Insn *in = &blk->insns[i];
            for (int k = 0; k < in->nops; k++) {
                const X86Operand *o = &in->ops[k];
                int rsp = (o->kind == OCERZ_OPK_REG &&
                           (o->reg & 15) == OCERZ_RSP) ||
                          (o->kind == OCERZ_OPK_MEM &&
                           ((o->base != OCERZ_REG_NONE &&
                             (o->base & 15) == OCERZ_RSP) ||
                            (o->index != OCERZ_REG_NONE &&
                             (o->index & 15) == OCERZ_RSP)));

                if (rsp && !(in->op == OCERZ_OP_MOV && k == 1 &&
                             o->kind == OCERZ_OPK_REG)) {
                    call_region = 0;
                    break;
                }
            }
        }
    }

    int indirect_jmp_term = term->op == OCERZ_OP_JMP &&
                            term->ops[0].kind != OCERZ_OPK_IMM &&
                            term->seg == OCERZ_SEG_NONE;
    int fixed_region = !call_region && !g_no_regflags &&
        (term->op == OCERZ_OP_JCC ||
         (term->op == OCERZ_OP_JMP && term->ops[0].kind == OCERZ_OPK_IMM) ||
         indirect_jmp_term);
    int full_pin = fullpin_enabled() && !g_no_regflags;
    if (full_pin) {
        call_region = 0;
        fixed_region = 0;
        for (int i = 0; i < 16; i++) {
            blk->host_holds[i] = (uint8_t)i;
            blk->guest_in_host[i] = (int8_t)i;
        }
        blk->n_pinned = 16;
        blk->pin_class = 3;
        g_pin = blk->guest_in_host;
        g_pin_hold = blk->host_holds;
        g_n_pinned = 16;
        g_pin_class = 3;
    } else
    if (call_region) {

        static const uint8_t call_gpr[6] = {
            OCERZ_RAX, OCERZ_RBX, OCERZ_RSP, OCERZ_RBP,
            OCERZ_R14, OCERZ_RDI,
        };
        for (int i = 0; i < 6; i++) {
            blk->host_holds[i] = call_gpr[i];
            blk->guest_in_host[call_gpr[i]] = (int8_t)i;
        }
        blk->n_pinned = 6;
        blk->pin_class = 2;
        g_pin = blk->guest_in_host;
        g_pin_hold = blk->host_holds;
        g_n_pinned = 6;
        g_pin_class = 2;
    } else if (fixed_region && !indirect_jmp_term) {
        uint64_t target = term->ops[0].imm;
        fixed_region = canonical_body_successor(target);
        if (term->op == OCERZ_OP_JCC)
            fixed_region |= canonical_body_successor(term->rip + term->len);
    }
    if (fixed_region) {

        static const uint8_t fixed_gpr[8] = {
            OCERZ_RAX, OCERZ_RCX, OCERZ_RDX, OCERZ_RBX,
            OCERZ_RSI, OCERZ_RDI, OCERZ_R8,  OCERZ_R9,
        };
        for (int i = 0; i < 8; i++) {
            blk->host_holds[i] = fixed_gpr[i];
            blk->guest_in_host[fixed_gpr[i]] = (int8_t)i;
        }
        blk->n_pinned = 8;
        blk->pin_class = 1;
        g_pin = blk->guest_in_host;
        g_pin_hold = blk->host_holds;
        g_n_pinned = 8;
        g_pin_class = 1;
    } else if (!full_pin && !call_region && !g_no_regflags &&
               (n >= g_pin_min || fuse_self)) {
        int cnt[16] = {0};
        for (int i = 0; i < n; i++) {
            const X86Insn *in = &blk->insns[i];
            for (int k = 0; k < in->nops; k++) {
                const X86Operand *o = &in->ops[k];
                if (o->kind == OCERZ_OPK_REG)
                    cnt[o->reg & 15]++;
                else if (o->kind == OCERZ_OPK_MEM) {
                    if (o->base != OCERZ_REG_NONE)  cnt[o->base & 15]++;
                    if (o->index != OCERZ_REG_NONE) cnt[o->index & 15]++;
                }
            }
        }
        cnt[OCERZ_RSP] = 0;
        int np = 0;
        while (np < 8) {
            int best = -1;
            for (int g = 0; g < 16; g++)
                if (cnt[g] > 0 && (best < 0 || cnt[g] > cnt[best]))
                    best = g;
            if (best < 0)
                break;
            blk->host_holds[np] = (uint8_t)best;
            blk->guest_in_host[best] = (int8_t)np;
            cnt[best] = 0;
            np++;
        }
        blk->n_pinned = (uint8_t)np;
        if (np > 0) {
            g_pin = blk->guest_in_host;
            g_pin_hold = blk->host_holds;
            g_n_pinned = np;
        }
    }

    g_mem_hoist_greg = select_mem_base_hoist(blk->insns, n, rip);
    g_low_hoist_greg = select_low_hoist(blk->insns, n, rip);
    g_n_low_hoist_bail = 0;

    g_xmm_pinned = 0;
    if (xmm_pinning_enabled() && sse_enabled() && xmm_global_enabled() && !g_no_regflags) {
        g_xmm_pinned = 0xffff;
    } else if (xmm_pinning_enabled() && sse_enabled()) {
        for (int i = 0; i < n; i++) {
            const X86Insn *in = &blk->insns[i];
            for (int k = 0; k < in->nops; k++)
                if (in->ops[k].kind == OCERZ_OPK_XMM && in->ops[k].reg < 16)
                    g_xmm_pinned |= (uint16_t)(1u << in->ops[k].reg);
            if (in->op == OCERZ_OP_BLENDVPD || in->op == OCERZ_OP_BLENDVPS || in->op == OCERZ_OP_PBLENDVB)
                g_xmm_pinned |= 1u;
            if (in->vex & OCERZ_VEX_NDS)
                g_xmm_pinned |= (uint16_t)(1u << (in->vvvv & 15));
        }
    }
    blk->xmm_pinned = g_xmm_pinned;
    g_pk_consts_needed = 0;
    for (int i = 0; i < n && sse_enabled(); i++) {
        switch (blk->insns[i].op) {
        case OCERZ_OP_ADDPS: case OCERZ_OP_ADDPD: case OCERZ_OP_SUBPS: case OCERZ_OP_SUBPD:
        case OCERZ_OP_MULPS: case OCERZ_OP_MULPD: case OCERZ_OP_DIVPS: case OCERZ_OP_DIVPD:
        case OCERZ_OP_SQRTPS: case OCERZ_OP_SQRTPD:
            g_pk_consts_needed = 1; break;
        default: break;
        }
    }
    pthread_jit_write_protect_np(0);
    if (!ENV_ON("OCERZ_NO_DISPATCH_STUB")) {
        if (mode32) { if (!jit->dispatch_stub32) emit_dispatch_stub(jit, 1); }
        else        { if (!jit->dispatch_stub)   emit_dispatch_stub(jit, 0); }
    }
    veneer_pool_check(jit);
    if (tc_hit && tcm == OCERZ_TC_VERIFY && tc_hit->size >= sizeof(TcRec)) {
        uint8_t *pp = (uint8_t *)jit->code_cur;
        size_t pad = ((uintptr_t)((const TcRec *)(const void *)tc_hit)->entry_mod - (uintptr_t)pp) & 63;
        if (pp + pad < (uint8_t *)jit->code_end)
            jit->code_cur = (uint32_t *)(void *)(pp + pad);
    }
    A64Buf b = { jit->code_cur, jit->code_cur, jit->code_end, 0, 0 };
    uint32_t *entry = b.p;
    g_push_entry = entry;
    g_tc_entry = entry;
    g_tc_on = (g_tc_rec || ocerz_tcache_mode() == OCERZ_TC_ROUNDTRIP) ? tc_usable(jit) : 0;

    a64_stp_pre(&b, 29, 30, 31, -16);
    a64_stp_pre(&b, 19, 20, 31, -16);
    a64_mov_reg(&b, 1, 19, 0);
    a64_mov_reg(&b, 1, 20, 1);
    emit_reload_jgb(&b);

    emit_pin_prologue(&b);

    if (rsp_is_ptr() && ocerz_guest_base != 0) {
        int rs = pin_slot(OCERZ_RSP);
        assert(rs >= 0);
        a64_mov_imm64(&b, JT0, ocerz_guest_base);
        a64_add_reg(&b, 1, pin_hreg(rs), pin_hreg(rs), JT0, 0);
    }
    g_lowstack = lowstack_delta_ok();
    g_lowstack_from = 0;
    g_lowstack_check = g_lowstack && ENV_ON("OCERZ_LOWSTACK_CHECK");
    if (g_lowstack)
        emit_stack_delta(&b);
    g_m32low = !g_lowstack && m32_lowreg_ok();
    if (g_m32low)
        a64_mov_imm64(&b, JGB, ocerz_low_base);

    if (g_pin_class == 2)
        a64_add_imm(&b, 1, 29, 31, 0);
    if (g_pin_class == 3 && host_ras_enabled()) {
        a64_add_imm(&b, 1, JT0, 31, 0);
        a64_str(&b, 8, JT0, 20, JIT_FP_OFF);
        a64_stp_pre(&b, 31, 31, 31, -16);
    }

    uint32_t *loop_poll_exit = NULL;
    if (xmm_global_enabled())
        emit_xmm_pin_load_all(&b);
    uint32_t *body_noreload = NULL;
    if (!g_no_chain && !jit->stop_requested) {
        g_body_entry = a64_label(&b);
        emit_reload_mem_base(&b);
        body_noreload = a64_label(&b);
        {
            static int bt = -1;
            if (bt < 0) bt = getenv("OCERZ_BTRACE") ? 1 : 0;
            if (bt) {
                a64_ldr(&b, 8, JTA, 20, (uint32_t)offsetof(OcerzCPU, btrace));
                a64_ldr(&b, 4, JT2, 20, (uint32_t)offsetof(OcerzCPU, btrace_n));
                a64_ldr(&b, 4, JTT, 20, (uint32_t)offsetof(OcerzCPU, btrace_mask));
                a64_and_reg(&b, 0, JTT, JT2, JTT, 0);
                a64_add_reg(&b, 1, JTA, JTA, JTT, 3);
                a64_mov_imm64(&b, JT0, rip);
                a64_str(&b, 8, JT0, JTA, 0);
                a64_add_imm(&b, 0, JT2, JT2, 1);
                a64_str(&b, 4, JT2, 20, (uint32_t)offsetof(OcerzCPU, btrace_n));
            }
        }
        if (ENV_ON("OCERZ_JGB_CHECK") && jgb_usable()) {
            a64_mov_imm64(&b, JTU, ocerz_guest_base);
            a64_subs_reg(&b, 1, A64_ZR, 0, JTU, 0);
            uint32_t *okl = a64_label(&b); a64_bcond(&b, A64_EQ, 0);
            a64_mov_reg(&b, 1, 1, 0);
            a64_mov_imm64(&b, 0, rip);
            tc_imm64(&b, 16, TCR_SYM, TCS_JGB_TRAP, (uint64_t)(uintptr_t)&ocerz_jgb_trap);
            a64_blr(&b, 16);
            a64_patch_bcond(okl, a64_label(&b));
        }
        if (!xmm_global_enabled())
            emit_xmm_pin_load_all(&b);
        { static int la = -1; if (la < 0) { const char *e = getenv("OCERZ_LOOP_ALIGN"); la = e ? (int)strtol(e, NULL, 0) : 32; }
          int self_loop = 0;
          { const X86Insn *t = &blk->insns[n - 1];
            if ((t->op == OCERZ_OP_JCC || t->op == OCERZ_OP_JMP) && t->nops == 1 && t->ops[0].kind == OCERZ_OPK_IMM && t->ops[0].imm == rip) self_loop = 1; }
          if (la > 4 && g_body_entry && self_loop) {
              size_t k = (size_t)(b.p - g_body_entry);
              size_t pad = ((uintptr_t)la - ((uintptr_t)b.p & (uintptr_t)(la - 1))) & (uintptr_t)(la - 1);
              pad /= 4;
              if (pad && b.p + pad < b.end) {
                  memmove(g_body_entry + pad, g_body_entry, k * sizeof(uint32_t));
                  for (size_t q = 0; q < pad; q++) g_body_entry[q] = 0xd503201fu;
                  g_body_entry += pad;
                  if (body_noreload) body_noreload += pad;
                  b.p += pad;
              }
          } }
        { const X86Insn *t = &blk->insns[n - 1];
          int selfl = (t->op == OCERZ_OP_JCC || t->op == OCERZ_OP_JMP) &&
                      t->nops == 1 && t->ops[0].kind == OCERZ_OPK_IMM && t->ops[0].imm == rip;
          static int nofix = -1;
          if (nofix < 0) nofix = getenv("OCERZ_NO_L0FIXED") ? 1 : 0;
          if (g_zero_vreg >= 0) a64_v_zero(&b, g_zero_vreg);
          if (g_blk_ymm_write) a64_str(&b, 4, A64_ZR, 20, YMMH_ALL_ZERO_OFF);
          if (!nofix && selfl && !g_no_chain && !g_xlat_mode32 && l0_enabled() &&
              sse_enabled() && xmm_global_enabled() && !g_no_regflags)
              l0_fixed_setup(&b, blk->insns, n);
          if (!nofix && selfl && !g_no_chain && !g_xlat_mode32 && l0_enabled() &&
              sse_enabled() && xmm_global_enabled() && !g_no_regflags)
              yc_setup(&b, blk->insns, n);
        }
        g_loop_entry = a64_label(&b);
        if (g_low_hoist_greg >= 0)
            emit_low_hoist_check(&b);
        if (g_mem_hoist_greg >= 0 && g_mem_hoist_aux_index >= 0)
            a64_add_reg(&b, 1, JMEMAUX, JMEMBASE, pin_hreg(pin_slot(g_mem_hoist_aux_index)), g_mem_hoist_aux_scale);
        static int loop_poll = -1;
        if (loop_poll < 0) loop_poll = getenv("OCERZ_LOOP_POLL") ? 1 : 0;
        if (loop_poll) {
            a64_ldr(&b, 4, JT0, 20, INT_OFF);
            loop_poll_exit = a64_label(&b);
            a64_cbnz(&b, 0, JT0, 0);
        }
    } else {
        if (!xmm_global_enabled())
            emit_xmm_pin_load_all(&b);
        g_low_hoist_greg = -1;
    }
    if (ocerz_perfstat > 0) {
        g_tc_bad = 1;
        a64_mov_imm64(&b, JT0, (uint64_t)(uintptr_t)&blk->exec_count);
        a64_ldr(&b, 8, JT1, JT0, 0);
        a64_add_imm(&b, 1, JT1, JT1, 1);
        a64_str(&b, 8, JT1, JT0, 0);
    }

    JitFaultFlagRecipe fault_recipes[JIT_MAX_BLOCK_INSNS];
    int n_fault_recipes = build_fault_flag_recipes(blk->insns, n, fault_recipes);
    blk->insn_off = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    if (blk->insn_off && n_fault_recipes) {
        blk->fault_flags = (JitFaultFlagRecipe *)malloc((size_t)n *
                                                        sizeof *blk->fault_flags);
        if (blk->fault_flags)
            memcpy(blk->fault_flags, fault_recipes,
                   (size_t)n * sizeof *blk->fault_flags);
    }

    uint64_t seam_seed = OCERZ_FL_ALL;
    if (!g_no_xlive && is_terminator(blk->insns[n - 1].op)) {
        const X86Insn *term = &blk->insns[n - 1];
        switch (term->op) {
        case OCERZ_OP_JMP:

            if (term->ops[0].kind == OCERZ_OPK_IMM)
                seam_seed = xlive_succ_live(jit, term->ops[0].imm);
            break;
        case OCERZ_OP_JCC: {

            uint64_t taken = xlive_succ_live(jit, term->ops[0].imm);
            uint64_t fall  = xlive_succ_live(jit, term->rip + term->len);
            seam_seed = taken | fall;
            break;
        }
        case OCERZ_OP_CALL:
            if (term->ops[0].kind == OCERZ_OPK_IMM)
                seam_seed = xlive_succ_live(jit, term->ops[0].imm);
            else if (!ret_flags_live_at(term->rip) && !mode32)
                seam_seed = 0;
            break;
        case OCERZ_OP_RET:
            if (!ret_flags_live_at(term->rip) && !mode32) {
                static int dead = -1;
                if (dead < 0) dead = getenv("OCERZ_RET_FLAGS_DEAD") != NULL;
                seam_seed = dead ? 0 : ret_seam_live(blk->insns, n);
            }
            break;
        default:

            break;
        }
    }

    uint64_t fl_need[JIT_MAX_BLOCK_INSNS];
    uint64_t jcc_fall_live[JIT_MAX_BLOCK_INSNS];
    uint64_t entry_all;
    {
        uint64_t live_seam = seam_seed;
        uint64_t live_all = OCERZ_FL_ALL;
        for (int i = n - 1; i >= 0; i--) {
            uint64_t def, use;
            jcc_fall_live[i] = 0;
            if (i < n - 1 && blk->insns[i].op == OCERZ_OP_JCC) {
                uint64_t tl = (g_no_xlive || blk->insns[i].ops[0].kind != OCERZ_OPK_IMM)
                              ? OCERZ_FL_ALL : xlive_succ_live(jit, blk->insns[i].ops[0].imm);
                jcc_fall_live[i] = live_seam;
                live_seam |= tl;
                live_all |= tl;
            }
            int side_fused = i < n - 1 && i >= 1 && blk->insns[i].op == OCERZ_OP_JCC &&
                             (side_fuse_ok(blk->insns, i - 1, n) || (i >= 2 && side_gap_fuse_ok(blk->insns, i - 2, n)));
            if (blk->fault_flags && blk->fault_flags[i].kind != JFF_NONE)
                ocerz_flags_defuse_nofault(&blk->insns[i], &def, &use);
            else
                ocerz_flags_defuse(&blk->insns[i], &def, &use);
            if ((blk->insns[i].op == OCERZ_OP_JCC || blk->insns[i].op == OCERZ_OP_SETCC ||
                 blk->insns[i].op == OCERZ_OP_CMOVCC) &&
                ((sse_enabled() && comis_fuse_producer(blk->insns, i) >= 0) ||
                 (g_defer && !g_no_regflags && value_cond_fuse_producer(blk->insns, i) >= 0)))
                use = 0;
            if ((blk->insns[i].op == OCERZ_OP_SETCC || blk->insns[i].op == OCERZ_OP_CMOVCC ||
                 blk->insns[i].op == OCERZ_OP_ADC || blk->insns[i].op == OCERZ_OP_SBB ||
                 blk->insns[i].op == OCERZ_OP_JCC) &&
                nzcv_fuse_producer(blk->insns, i) >= 0)
                use &= ~(uint64_t)JIT_ARITH_FLAGS;
            if (side_fused)
                use = 0;
            fl_need[i] = def & live_seam;
            live_seam = (live_seam & ~def) | use;
            live_all = (live_all & ~def) | use;

            if (g_no_lazyflags)
                fl_need[i] = def;
        }
        static int pub_all = -1;
        if (pub_all < 0) pub_all = getenv("OCERZ_XLIVE_ALL") ? 1 : 0;
        entry_all = pub_all ? live_all : live_seam;
    }

    blk->entry_live = (uint16_t)entry_all;

    for (int ci = 0; ci < n; ci++) {
        if (g_ic_kind[ci] != 1) continue;
        int64_t delta = 0;
        int safe = 1, rj = -1, nest = 0;
        for (int k = ci + 1; k < n; k++) {
            const X86Insn *m = &blk->insns[k];
            if (g_ic_kind[k] == 2 || g_ic_kind[k] == 3) {
                if (nest == 0) { rj = k; break; }
                nest--; delta += 8;
                continue;
            }
            if (g_ic_kind[k] == 1) { nest++; delta -= 8; continue; }
            switch (m->op) {
            case OCERZ_OP_PUSH:
                if (m->nops > 0 && (m->ops[0].kind == OCERZ_OPK_MEM || m->ops[0].size != 8)) { safe = 0; break; }
                delta -= 8; break;
            case OCERZ_OP_POP:
                if (m->nops > 0 && (m->ops[0].kind == OCERZ_OPK_MEM || m->ops[0].size != 8)) { safe = 0; break; }
                delta += 8; break;
            case OCERZ_OP_MOV: case OCERZ_OP_LEA: case OCERZ_OP_ADD: case OCERZ_OP_SUB:
            case OCERZ_OP_XOR: case OCERZ_OP_OR: case OCERZ_OP_AND: case OCERZ_OP_SHR:
            case OCERZ_OP_SHL: case OCERZ_OP_SAR: case OCERZ_OP_IMUL: case OCERZ_OP_INC:
            case OCERZ_OP_DEC: case OCERZ_OP_NEG: case OCERZ_OP_NOT: case OCERZ_OP_MOVZX:
            case OCERZ_OP_MOVSX: case OCERZ_OP_MOVSXD: case OCERZ_OP_NOP:
            case OCERZ_OP_TEST: case OCERZ_OP_CMP:
                if (m->nops > 0 && m->ops[0].kind == OCERZ_OPK_MEM &&
                    m->op != OCERZ_OP_TEST && m->op != OCERZ_OP_CMP) { safe = 0; break; }
                if (m->nops > 0 && m->ops[0].kind == OCERZ_OPK_REG &&
                    m->ops[0].reg == OCERZ_RSP) { safe = 0; break; }
                break;
            default:
                safe = 0; break;
            }
            if (!safe) break;
        }
        if (safe && rj >= 0 && delta == 0)
            g_ic_kind[rj] = 3;
    }

    for (int sweep = 0; sweep < 3; sweep++) {
        for (int ci = 0; ci < n; ci++) {
            if (g_ic_kind[ci] != 1 || g_ic_pushelide[ci] || blk->insns[ci].mode32)
                continue;
            int64_t delta = 0;
            int ok = 1, rj = -1, nest = 0;
            for (int k = ci + 1; k < n; k++) {
                const X86Insn *m = &blk->insns[k];
                if (g_ic_kind[k] == 3) {
                    if (nest == 0) { rj = k; break; }
                    nest--; delta += 8;
                    continue;
                }
                if (g_ic_kind[k] == 2) { ok = 0; break; }
                if (g_ic_kind[k] == 1) {
                    if (!g_ic_pushelide[k]) { ok = 0; break; }
                    nest++; delta -= 8;
                    continue;
                }
                int memop = 0;
                for (int q = 0; q < m->nops; q++)
                    if (m->ops[q].kind == OCERZ_OPK_MEM) memop = 1;
                switch (m->op) {
                case OCERZ_OP_PUSH:
                    if (memop || m->ops[0].size != 8) { ok = 0; break; }
                    delta -= 8; break;
                case OCERZ_OP_POP:
                    if (memop || delta == 0 || m->ops[0].size != 8) { ok = 0; break; }
                    delta += 8; break;
                case OCERZ_OP_MOV: case OCERZ_OP_LEA: case OCERZ_OP_ADD: case OCERZ_OP_SUB:
                case OCERZ_OP_XOR: case OCERZ_OP_OR: case OCERZ_OP_AND: case OCERZ_OP_SHR:
                case OCERZ_OP_SHL: case OCERZ_OP_SAR: case OCERZ_OP_IMUL: case OCERZ_OP_INC:
                case OCERZ_OP_DEC: case OCERZ_OP_NEG: case OCERZ_OP_NOT: case OCERZ_OP_MOVZX:
                case OCERZ_OP_MOVSX: case OCERZ_OP_MOVSXD: case OCERZ_OP_NOP:
                case OCERZ_OP_TEST: case OCERZ_OP_CMP:
                    if (memop && m->op != OCERZ_OP_LEA) { ok = 0; break; }
                    if (m->nops > 0 && m->ops[0].kind == OCERZ_OPK_REG &&
                        m->ops[0].reg == OCERZ_RSP) { ok = 0; break; }
                    break;
                default:
                    ok = 0; break;
                }
                if (!ok) break;
            }
            if (ok && rj >= 0 && delta == 0) {
                g_ic_pushelide[ci] = 1;
                g_ic_pair_rj[ci] = rj;
            }
        }
    }

    static int no_promo = -1;
    if (no_promo < 0) no_promo = getenv("OCERZ_NO_PROMO") ? 1 : 0;
    if (!no_promo && g_pin_class == 3 && pin_slot(OCERZ_RSP) >= 0 &&
        (stack_identity() || rsp_is_ptr())) {
        int freer[3]; int nfree = 0;
        if (g_mem_hoist_greg2 < 0) freer[nfree++] = JMEMBASE2;
        if (g_mem_hoist_greg  < 0 && g_low_hoist_greg < 0) freer[nfree++] = JMEMBASE;
        if (g_mem_hoist_greg3 < 0) freer[nfree++] = JMEMBASE3;
        int npairs = 0;
        int sp = 0, dead = 0; int pstk[64]; int8_t rres[64];
        int nend = 0; int32_t endstk[8];
        for (int i = 0; i < n; i++) {
            while (nend > 0 && i >= endstk[nend - 1]) nend--;
            if (g_ic_kind[i] == 1 && g_ic_pushelide[i]) {
                if (nend < 8) endstk[nend++] = g_ic_pair_rj[i];
                continue;
            }
            if (nend == 0) { sp = 0; dead = 0; continue; }
            if (dead) continue;
            const X86Insn *m = &blk->insns[i];
            int plain = m->nops > 0 && m->ops[0].kind == OCERZ_OPK_REG &&
                        !m->ops[0].high8 && m->ops[0].size == 8 &&
                        m->ops[0].reg != OCERZ_RSP &&
                        pin_slot(m->ops[0].reg) >= 0 && !m->mode32;
            if (m->op == OCERZ_OP_PUSH) {
                if (!plain || sp >= 64) { dead = 1; continue; }
                if (nfree > 0 && npairs < PE_MAX) { rres[sp] = (int8_t)freer[--nfree]; }
                else rres[sp] = -1;
                pstk[sp++] = i;
            } else if (m->op == OCERZ_OP_POP) {
                if (!plain || sp <= 0) { dead = 1; continue; }
                sp--;
                if (rres[sp] >= 0) {
                    g_promo_reg[pstk[sp]] = (uint8_t)rres[sp];
                    g_promo_reg[i] = (uint8_t)rres[sp];
                    g_promo_mate[pstk[sp]] = i;
                    g_promo_push_of[i] = pstk[sp];
                    freer[nfree++] = rres[sp];
                    npairs++;
                }
            }
        }
    }

    if (g_flaglive_log) {
        int wrote = 0, killed = 0;
        for (int i = 0; i < n; i++) {
            uint64_t def, use;
            ocerz_flags_defuse(&blk->insns[i], &def, &use);
            for (int f = 0; f < 6; f++) {
                uint64_t bit = (uint64_t)1 << (int[]){0, 2, 4, 6, 7, 11}[f];
                if (def & bit) {
                    wrote++;
                    if (!(fl_need[i] & bit))
                        killed++;
                }
            }
        }
        if (wrote)
            fprintf(stderr, "ocerz: FLAGLIVE rip=%#llx insns=%d flagwrites=%d dead=%d (%.1f%%)\n",
                    (unsigned long long)rip, n, wrote, killed,
                    100.0 * (double)killed / (double)wrote);
    }

    uint32_t *exit_sites[2 * JIT_MAX_BLOCK_INSNS + 64];
    uint32_t *epi_sites[2 * JIT_MAX_BLOCK_INSNS + 64];
    int n_exits = 0;
    int n_epi = 0;
    int8_t fpb_of[JIT_MAX_BLOCK_INSNS];
    fpb_scan(blk->insns, n, fpb_of);
    mov_sink_scan(blk->insns, n, fl_need);
    g_scpend.valid = 0; g_scalar_merge_next = 0;
    g_fpb_of = fpb_of;
    g_fpb_open = -1;
    g_fpb_fast = 0;
    g_fcmp_self_idx = -1;
    g_cmps_mask_idx = -1;
    l0_reset();
    if (g_l0_fixed) l0_fixed_map();
    g_ymmh_zero = 0;
    unsigned long long l0_last_seq = g_callout_seq;
    g_n_fpbmap = 0;
    g_n_lanerec = 0;
    int fpb_open = -1;

    int last_flag_def = -1;
    ea_cache_reset();
    int leaf_entry_writes = 0;
    const void *leaf_entry = NULL;
    if (ocerz_mode != OCERZ_MODE_NATIVE && !g_l0_fixed && leaf_layout_ok()) {
        leaf_entry = ocerz_dyldapi_leaf_entry(rip, &leaf_entry_writes);
        for (int k = 0; leaf_entry && k < n; k++) {
            const X86Insn *t = &blk->insns[k];
            if (t->op != OCERZ_OP_CALL && t->nops == 1 && t->ops[0].kind == OCERZ_OPK_IMM &&
                (uint64_t)t->ops[0].imm == rip)
                leaf_entry = NULL;
        }
    }
    for (int i = 0; i < n; i++) {
        const X86Insn *insn = &blk->insns[i];
        g_cur_insn_idx = i;
        g_cur_insn_start = b.p;
        g_align_guard = g_align_blk || (g_align_any && al_marked(jit_key(insn->rip, mode32)));
        g_ea_plain = 0;
        g_ea_lowhoisted = 0;
        lanerec_note((uint32_t)(b.p - entry));
        if (i == 0 && fps_watch(rip)) {
            g_tc_bad = 1;
            a64_mov_imm64(&b, JT0, (uint64_t)(uintptr_t)&g_fps_frames);
            a64_ldr(&b, 8, JT1, JT0, 0);
            a64_add_imm(&b, 1, JT1, JT1, 1);
            a64_str(&b, 8, JT1, JT0, 0);
        }
        if (i == 0 && leaf_entry) {
            OCERZ_LOG("jit: the routine at %#llx is answered in place\n", (unsigned long long)rip);
            uint32_t *declined = emit_leaf_call_ret(&b, leaf_entry, leaf_entry_writes, epi_sites, &n_epi);
            a64_patch_cbz(declined, a64_label(&b));
            ea_cache_reset();
        }
        g_ea_is_const = 0;
        if (g_lowstack) {
            int moved = 0;
            for (int k = g_lowstack_from; k < i; k++)
                moved |= lowstack_disturbs(&blk->insns[k]);
            g_lowstack_from = i;
            if (moved)
                emit_stack_delta(&b);
            if (g_lowstack_check)
                emit_stack_delta_check(&b);
        }
        g_cur_need = fl_need[i];
        g_cur_insns = blk->insns; g_cur_insns_n = n;
        {
            int mmx = 0;
            for (int k = 0; k < insn->nops; k++) mmx |= insn->ops[k].kind == OCERZ_OPK_MMX;
            if ((insn->op > OCERZ_OP_X87_FIRST && insn->op < OCERZ_OP_SSE_FIRST && !x87_inline_ok(insn)) ||
                insn->op == OCERZ_OP_FXRSTOR || insn->op == OCERZ_OP_XRSTOR || insn->op == OCERZ_OP_EMMS || mmx) {
                g_x87_btop = -1;
                g_x87_lv = 0;
                g_x87_kcarry = 0;
            }
        }
        g_cur_fpb = fpb_of[i];
        if (g_scpend.valid && g_scpend.idx < i - 1) scalar_pend_flush(&b);
        g_fpb_open = fpb_open;
        g_fpb_fast = fpb_open >= 0 && g_fpb_member[i];
        ea_cache_step(insn, i > 0 ? &blk->insns[i - 1] : NULL);
        g_nzcv_want = 0;
        for (int j = i + 1; j < n && j <= i + 1 + NZCV_GAP_MAX; j++)
            if (nzcv_fuse_producer(blk->insns, j) == i) { g_nzcv_want = 1; break; }
        if (g_callout_seq != l0_last_seq) { l0_flush_all(&b); l0_reset(); l0_last_seq = g_callout_seq; }
        l0_pre_insn(&b, insn);
        if (!(insn->op == OCERZ_OP_JCC && i < n - 1) &&
            !(g_l0_fixed && i >= n - 2) &&
            (is_terminator(insn->op) ||
             (i + 1 < n && (blk->insns[i + 1].op == OCERZ_OP_JMP ||
                            (blk->insns[i + 1].op == OCERZ_OP_JCC && i + 1 == n - 1)))))
            l0_flush_all(&b);
        if (fpb_open >= 0 && (fpb_of[i] != fpb_open)) {
            fpb_emit_check(&b, &g_fpb[fpb_open]);
            for (int r = 0; r < 16; r++) { g_fpb[fpb_open].l0[r] = g_l0[r]; g_fpb[fpb_open].l0_dbl[r] = g_l0_dbl[r]; }
            fpb_open = -1;
            g_fpb_open = -1;
            g_fpb_fast = 0;
        }
        if (fpb_of[i] >= 0 && fpb_open < 0 && g_fpb[fpb_of[i]].first == i) {
            fpb_open = fpb_of[i];
            g_fpb_open = fpb_open;
            FpBatch *fb = &g_fpb[fpb_open];
            uint16_t lanes = 0;
            for (int r = 0; r < 16; r++)
                if (g_l0[r] >= 0 && (g_l0_dirty & fb->ckpt & (1u << r))) lanes |= (uint16_t)(1u << r);
            fb->dirty_open = g_l0_dirty;
            fb->ckpt_emit = fb->ckpt;
            for (int r = 0; r < 16; r++)
                if (fb->ckpt_emit & (1u << r)) {
                    a64_str_v(&b, 16, xmm_vreg((unsigned)r), 20, FPCKPT_OFF + (uint32_t)r * 16);
                    if (lanes & (1u << r))
                        a64_str_v(&b, g_l0_dbl[r] ? 8 : 4, g_l0[r], 20, FPCKPT_OFF + (uint32_t)r * 16);
                }
            g_fpb_fast = 1;
        }
        g_flag_producer = last_flag_def >= 0 ? &blk->insns[last_flag_def] : NULL;
        {
            g_flag_producer_operands_intact = 1;
            if (g_flag_producer) {
                for (int k = last_flag_def + 1; k < i; k++) {
                    const X86Insn *m = &blk->insns[k];
                    if (m->nops > 0 && m->ops[0].kind == OCERZ_OPK_XMM) {
                        unsigned w = m->ops[0].reg;
                        if ((g_flag_producer->ops[0].kind == OCERZ_OPK_XMM && g_flag_producer->ops[0].reg == w) ||
                            (g_flag_producer->ops[1].kind == OCERZ_OPK_XMM && g_flag_producer->ops[1].reg == w))
                            g_flag_producer_operands_intact = 0;
                    }
                }
            }
            uint64_t pdef, puse;
            ocerz_flags_defuse(insn, &pdef, &puse);
            if (pdef & JIT_ARITH_FLAGS)
                last_flag_def = i;
        }
        if (g_n_side < SIDE_MAX && side_gap_fuse_ok(blk->insns, i, n)) {
            uint32_t *jcc_label = NULL, *gap_label = NULL;
            if (blk->insn_off) blk->insn_off[i] = (uint32_t)(b.p - entry);
            g_jcc_side_mode = 1;
            g_jcc_side_need = fl_need[i];
            { uint64_t pdef, puse; ocerz_flags_defuse(insn, &pdef, &puse); g_jcc_side_fall_need = pdef & jcc_fall_live[i + 2]; }
            int fused = emit_cmp_test_jcc(&b, insn, &blk->insns[i + 2], epi_sites, &n_epi,
                                          &jcc_label, exit_sites, &n_exits,
                                          &blk->insns[i + 1], &gap_label);
            g_jcc_side_mode = 0;
            if (fused && jcc_label && gap_label) {
                if (blk->insn_off) {
                    blk->insn_off[i + 1] = (uint32_t)(gap_label - entry);
                    blk->insn_off[i + 2] = (uint32_t)(jcc_label - entry);
                }
                blk->n_inlined += 3;
                i += 2;
                continue;
            }
        }
        if (g_n_side < SIDE_MAX && side_fuse_ok(blk->insns, i, n)) {
            uint32_t *jcc_label = NULL;
            if (blk->insn_off) blk->insn_off[i] = (uint32_t)(b.p - entry);
            g_jcc_side_mode = 1;
            g_jcc_side_need = fl_need[i];
            { uint64_t pdef, puse; ocerz_flags_defuse(insn, &pdef, &puse); g_jcc_side_fall_need = pdef & jcc_fall_live[i + 1]; }
            int fused = emit_cmp_test_jcc(&b, insn, &blk->insns[i + 1], epi_sites, &n_epi,
                                          &jcc_label, exit_sites, &n_exits, NULL, NULL);
            g_jcc_side_mode = 0;
            if (fused) {
                if (blk->insn_off) blk->insn_off[i + 1] = (uint32_t)(jcc_label - entry);
                blk->n_inlined += 2;
                i++;
                continue;
            }
        }
        if (i < n - 1 && insn->op == OCERZ_OP_JCC) {
            if (fpb_open >= 0 && fpb_of[i] != fpb_open) {
                l0_flush_all(&b);
                fpb_emit_check(&b, &g_fpb[fpb_open]);
                for (int r = 0; r < 16; r++) { g_fpb[fpb_open].l0[r] = g_l0[r]; g_fpb[fpb_open].l0_dbl[r] = g_l0_dbl[r]; }
                fpb_open = -1; g_fpb_open = -1; g_fpb_fast = 0; l0_reset();
            }
            if (blk->insn_off) blk->insn_off[i] = (uint32_t)(b.p - entry);
            g_cc_want_cbz = 1;
            emit_cc_predicate_ex(&b, insn->cc, 1);
            g_cc_want_cbz = 0;
            int cond = g_cc_direct >= 0 ? g_cc_direct : A64_NE;
            if (g_n_side < SIDE_MAX) {
                g_side[g_n_side].site = a64_label(&b);
                g_side[g_n_side].taken = insn->ops[0].imm;
                g_side[g_n_side].idx = i;
                g_side[g_n_side].stub = NULL;
                g_side[g_n_side].patch_b = NULL;
                g_side[g_n_side].rec = 0;
                g_side[g_n_side].fpb = fpb_open >= 0 && g_fpb_sidechk[i] ? fpb_open : -1;
                g_side[g_n_side].fpb_chk = g_fpb_sidechk[i];
                g_side[g_n_side].fpb_end = i - 1;
                g_side[g_n_side].l0_dirty = g_l0_dirty;
                g_side[g_n_side].yc_dirty = g_yc_dirty;
                for (int r = 0; r < 16; r++) { g_side[g_n_side].l0[r] = g_l0[r]; g_side[g_n_side].l0_dbl[r] = g_l0_dbl[r]; }
                g_side[g_n_side].jcc_rip = insn->rip;
                g_side[g_n_side].ft_rip = insn->rip + insn->len;
                g_side[g_n_side].ft_site = NULL;
                g_side[g_n_side].probe = probe_wanted(insn->rip, insn->rip + insn->len);
                if (g_cc_cbz_reg >= 0) {
                    if (g_cc_cbz_nz) a64_cbnz(&b, g_cc_cbz_sf, g_cc_cbz_reg, 0);
                    else             a64_cbz(&b, g_cc_cbz_sf, g_cc_cbz_reg, 0);
                } else
                a64_bcond(&b, cond, 0);
                if (g_side[g_n_side].probe) {
                    g_side[g_n_side].ft_site = a64_label(&b);
                    a64_b(&b, 0);
                }
                g_n_side++;
            } else {
                assert(0 && "side exit table full");
            }
            blk->n_inlined++;
            uint64_t pdef, puse; ocerz_flags_defuse(insn, &pdef, &puse); (void)pdef; (void)puse;
            continue;
        }
        if (blk->insn_off)
            blk->insn_off[i] = (uint32_t)(b.p - entry);
        if (i == n - 2) {
            uint32_t *jmp_label = NULL;
            if (emit_logic_jmp_incdec_jcc(&b, insn, &blk->insns[i + 1],
                                          fl_need[i], epi_sites, &n_epi,
                                          &jmp_label)) {
                if (blk->insn_off)
                    blk->insn_off[i + 1] =
                        (uint32_t)(jmp_label - entry);
                blk->n_inlined += 2;
                i++;
                continue;
            }
        }
        if (i == n - 3) {
            uint32_t *incdec_label = NULL;
            uint32_t *jcc_label = NULL;
            if (emit_arith_incdec_jcc(&b, insn, &blk->insns[i + 1],
                                      &blk->insns[i + 2], fl_need[i],
                                      epi_sites, &n_epi, &incdec_label,
                                      &jcc_label)) {
                if (blk->insn_off) {
                    blk->insn_off[i + 1] =
                        (uint32_t)(incdec_label - entry);
                    blk->insn_off[i + 2] =
                        (uint32_t)(jcc_label - entry);
                }
                blk->n_inlined += 3;
                i += 2;
                continue;
            }
        }
        int pair_consumed = 0;
        for (int j = i + 2; j < n && j <= i + 2 + NZCV_GAP_MAX; j++)
            if (nzcv_fuse_producer(blk->insns, j) == i + 1) { pair_consumed = 1; break; }
        if (i + 1 < n && !pair_consumed) {
            uint32_t *logic_label = NULL;
            if (emit_mov_logic_pair(&b, insn, &blk->insns[i + 1],
                                    fl_need[i + 1], &logic_label)) {
                if (blk->insn_off)
                    blk->insn_off[i + 1] =
                        (uint32_t)(logic_label - entry);
                blk->n_inlined += 2;
                last_flag_def = i + 1;
                i++;
                continue;
            }
        }
        if (i + 1 < n) {
            uint32_t *inc_label = NULL;
            if (emit_add_inc_pair(&b, insn, &blk->insns[i + 1],
                                  fl_need[i], fl_need[i + 1], &inc_label)) {
                if (blk->insn_off)
                    blk->insn_off[i + 1] = (uint32_t)(inc_label - entry);
                blk->n_inlined += 2;
                last_flag_def = i + 1;
                i++;
                continue;
            }
        }
        if (fuse_cmp && i == n - 2) {
            uint32_t *jcc_label = NULL;
            if (emit_ifconv_diamond(&b, insn, &blk->insns[i + 1],
                                    epi_sites, &n_epi, &jcc_label)) {
                if (blk->insn_off)
                    blk->insn_off[i + 1] =
                        (uint32_t)(jcc_label - entry);
                blk->n_inlined += 2;
                i++;
                continue;
            }
        }
        if (n >= 3 && i == n - 3 && !g_no_jccfuse && g_defer &&
            (insn->op == OCERZ_OP_CMP || insn->op == OCERZ_OP_TEST) &&
            can_fuse_cmp_test_jcc(insn, &blk->insns[n - 1], rip)) {
            uint32_t *jcc_label = NULL, *gap_label = NULL;
            int fused = emit_cmp_test_jcc(&b, insn, &blk->insns[n - 1],
                                          epi_sites, &n_epi, &jcc_label,
                                          exit_sites, &n_exits,
                                          &blk->insns[n - 2], &gap_label);
            if (fused && jcc_label && gap_label) {
                if (blk->insn_off) {
                    blk->insn_off[i + 1] = (uint32_t)(gap_label - entry);
                    blk->insn_off[i + 2] = (uint32_t)(jcc_label - entry);
                }
                blk->n_inlined += 3;
                i += 2;
                continue;
            }
        }
        if (fuse_pair && i == n - 2) {
            uint32_t *jcc_label = NULL;
            int fused = fuse_cmp
                ? emit_cmp_test_jcc(&b, insn, &blk->insns[i + 1],
                                    epi_sites, &n_epi, &jcc_label,
                                    exit_sites, &n_exits, NULL, NULL)
                : emit_incdec_jcc(&b, insn, &blk->insns[i + 1],
                                  epi_sites, &n_epi, &jcc_label);
            if (fused) {
                if (blk->insn_off)
                    blk->insn_off[i + 1] = (uint32_t)(jcc_label - entry);
                blk->n_inlined += 2;
                i++;
                continue;
            }
        }
        if (i == n - 1 && insn->op == OCERZ_OP_JCC) {
            if (emit_jcc(&b, insn, epi_sites, &n_epi)) {
                blk->n_inlined++;
                continue;
            }
        }

        if (i == n - 1 && insn->op == OCERZ_OP_JMP) {
            emit_bridge_fastcall(&b, blk->insns, i, epi_sites, &n_epi);
            if (emit_jmp(&b, insn, epi_sites, &n_epi) ||
                emit_indirect_jmp(&b, insn, exit_sites, &n_exits, epi_sites, &n_epi)) {
                blk->n_inlined++;
                continue;
            }
        }

        if (g_promo_reg[i] != 0) {
            int hsp = pin_hreg(pin_slot(OCERZ_RSP));
            int pr = g_promo_reg[i];
            int gr = pin_hreg(pin_slot(insn->ops[0].reg));
            if (insn->op == OCERZ_OP_POP && g_promo_seq[g_promo_push_of[i]] != g_callout_seq) {
                if (g_rsp_lag) {
                    a64_add_imm(&b, 1, hsp, hsp, g_rsp_lag);
                    g_rsp_lag = 0;
                }
                goto promo_push_fallthrough;
            }
            if (insn->op == OCERZ_OP_PUSH) {
                g_promo_seq[i] = g_callout_seq;
                a64_mov_reg(&b, 1, pr, gr);
                if (g_n_promo_real < PE_MAX)
                    g_promo_real[g_n_promo_real++] = (struct JitPromo){
                        (int32_t)i, g_promo_mate[i], (uint8_t)pr };
                goto promo_push_fallthrough;
            } else {
                int f3 = g_pin_class == 3 && pin_slot(OCERZ_RSP) >= 0 &&
                         stack_plain_access_ok() && jgb_usable() && !stack_guard_needed();
                a64_mov_reg(&b, 1, gr, pr);
                if (rsp_run_member(blk->insns, i + 1, n, f3)) {
                    g_rsp_lag += 8;
                } else {
                    a64_add_imm(&b, 1, hsp, hsp, 8 + g_rsp_lag);
                    g_rsp_lag = 0;
                }
            }
            blk->n_inlined++;
            continue;
        }
promo_push_fallthrough:
        if (g_ic_kind[i] != 0) {
            int fast3 = g_pin_class == 3 && pin_slot(OCERZ_RSP) >= 0 &&
                        stack_plain_access_ok() && jgb_usable() && !stack_guard_needed();
            if (!fast3 && low_splice_ok(insn)) {
                int hs = pin_hreg(pin_slot(OCERZ_RSP));
                if (g_ic_kind[i] == 1 && g_ic_pushelide[i] && g_n_pe_real < PE_MAX &&
                    (stack_identity() || rsp_is_ptr())) {
                    a64_sub_imm(&b, 1, hs, hs, 8);
                    g_pe_real[g_n_pe_real++] = (struct JitPushElide){
                        (int32_t)i, g_ic_pair_rj[i], insn->rip + insn->len };
                } else if (g_ic_kind[i] == 3) {
                    if (rsp_run_member(blk->insns, i + 1, n, 1)) {
                        g_rsp_lag += 8;
                    } else {
                        a64_add_imm(&b, 1, hs, hs, 8 + g_rsp_lag);
                        g_rsp_lag = 0;
                    }
                } else if (g_ic_kind[i] == 1) {
                    emit_gpr_rd(&b, 1, JT0, OCERZ_RSP);
                    a64_sub_imm(&b, 1, JTA, JT0, 8);
                    emit_add_const(&b, JTA, ea_fold());
                    uint32_t *skip = emit_commpage_guard(&b, insn, JTA, exit_sites, &n_exits);
                    emit_add_const(&b, JTA, ocerz_guest_base - ea_fold());
                    a64_mov_imm64(&b, JT1, insn->rip + insn->len);
                    g_ea_plain = stack_plain_now();
                    emit_guest_store_ordered(&b, 8, JT1, JTA, JTU);
                    patch_guard_skip(skip, a64_label(&b));
                    a64_sub_imm(&b, 1, hs, hs, 8);
                } else {
                    emit_gpr_rd(&b, 1, JT0, OCERZ_RSP);
                    a64_mov_reg(&b, 1, JTA, JT0);
                    emit_add_const(&b, JTA, ea_fold());
                    uint32_t *skip = emit_commpage_guard(&b, insn, JTA, exit_sites, &n_exits);
                    emit_add_const(&b, JTA, ocerz_guest_base - ea_fold());
                    emit_guest_load_ordered(&b, 8, JT0, JTA, JTU);
                    patch_guard_skip(skip, a64_label(&b));
                    a64_mov_imm64(&b, JT1, g_ic_expect[i]);
                    a64_subs_reg(&b, 1, 31, JT0, JT1, 0);
                    uint32_t *ok = a64_label(&b);
                    a64_bcond(&b, A64_EQ, 0);
                    a64_mov_imm64(&b, JT0, insn->rip);
                    a64_str(&b, 8, JT0, 20, RIP_OFF);
                    a64_mov_imm64(&b, 0, OCERZ_STEP_OK);
                    epi_sites[n_epi] = a64_label(&b);
                    a64_b(&b, 0);
                    n_epi++;
                    a64_patch_bcond(ok, a64_label(&b));
                    a64_add_imm(&b, 1, hs, hs, 8);
                }
                blk->n_inlined++;
                continue;
            }
            if (!fast3) {
                emit_slowcall(&b, insn, exit_sites, &n_exits);
                blk->n_slow++;
                continue;
            }
            int hs = pin_hreg(pin_slot(OCERZ_RSP));
            if (g_ic_kind[i] == 1) {
                if (g_ic_pushelide[i] && g_n_pe_real < PE_MAX &&
                    (stack_identity() || rsp_is_ptr())) {
                    a64_sub_imm(&b, 1, hs, hs, 8);
                    g_pe_real[g_n_pe_real++] = (struct JitPushElide){
                        (int32_t)i, g_ic_pair_rj[i], insn->rip + insn->len };
                } else {
                    a64_mov_imm64(&b, JT1, insn->rip + insn->len);
                    emit_push_pinned(&b, hs, JT1);
                }
            } else if (g_ic_kind[i] == 3) {
                if (rsp_run_member(blk->insns, i + 1, n, fast3)) {
                    g_rsp_lag += 8;
                } else {
                    a64_add_imm(&b, 1, hs, hs, 8 + g_rsp_lag);
                    g_rsp_lag = 0;
                }
            } else {
                if (stack_identity() || rsp_is_ptr()) {
                    a64_ldr_post64(&b, JT0, hs, 8);
                } else {
                    a64_ldr_regoff(&b, 8, JT0, JGB, hs, 0);
                    a64_add_imm(&b, 1, hs, hs, 8);
                }
                a64_mov_imm64(&b, JT1, g_ic_expect[i]);
                a64_subs_reg(&b, 1, 31, JT0, JT1, 0);
                uint32_t *ok = a64_label(&b);
                a64_bcond(&b, A64_EQ, 0);
                a64_sub_imm(&b, 1, hs, hs, 8);
                a64_mov_imm64(&b, JT0, insn->rip);
                a64_str(&b, 8, JT0, 20, RIP_OFF);
                a64_mov_imm64(&b, 0, OCERZ_STEP_OK);
                epi_sites[n_epi] = a64_label(&b);
                a64_b(&b, 0);
                n_epi++;
                a64_patch_bcond(ok, a64_label(&b));
            }
            blk->n_inlined++;
            continue;
        }
        if (i == n - 1 && (insn->op == OCERZ_OP_CALL || insn->op == OCERZ_OP_RET)) {
            if (emit_call_ret(&b, insn, exit_sites, &n_exits, epi_sites, &n_epi) ||
                emit_indirect_call(&b, insn, exit_sites, &n_exits, epi_sites, &n_epi)) {
                blk->n_inlined++;
                continue;
            }
        }
        if (g_mov_skip[i]) {
            if (blk->insn_off) blk->insn_off[i] = (uint32_t)(b.p - entry);
            blk->n_inlined++;
            continue;
        }
        if (fpb_open >= 0 && fpb_of[i] == fpb_open && g_fpb_stchk[i]) fpb_emit_store_check(&b, i, fpb_open);
        if (fpb_open >= 0 && fpb_of[i] == fpb_open && g_fpb_undo[i] && !g_fpb_undo_done[i]) fpb_emit_undo_save(&b, insn, i, exit_sites, &n_exits);
        g_undo_want_slot = -1; g_undo_saved = 0;
        if (i + 1 < n && !g_mov_skip[i + 1] && emit_mov128_pair(&b, insn, &blk->insns[i + 1], i)) {
            blk->n_inlined++;
            continue;
        }
        if (i + 1 < n && !g_mov_skip[i + 1] && emit_stack_pair(&b, insn, &blk->insns[i + 1], i)) {
            blk->n_inlined++;
            continue;
        }
        if (fpb_open >= 0 && fpb_of[i] == fpb_open && g_fpb_undo_ld[i]) { g_undo_want_slot = g_fpb_undo_ld[i] - 1; g_undo_want_size = g_fpb_undo_ldsz[i]; }
        if (!try_inline(&b, insn, fl_need[i], exit_sites, &n_exits)) {
            emit_slowcall(&b, insn, exit_sites, &n_exits);
            blk->n_slow++;
        } else {
            blk->n_inlined++;
        }
        if (g_undo_saved && g_fpb_undo_ldst[i] >= 0) g_fpb_undo_done[g_fpb_undo_ldst[i]] = 1;
        g_undo_want_slot = -1; g_undo_saved = 0;
    }

    if (fpb_open >= 0) {
        fpb_emit_check(&b, &g_fpb[fpb_open]);
        for (int r = 0; r < 16; r++) { g_fpb[fpb_open].l0[r] = g_l0[r]; g_fpb[fpb_open].l0_dbl[r] = g_l0_dbl[r]; }
        fpb_open = -1;
        g_fpb_open = -1;
        g_fpb_fast = 0;
        l0_flush_all(&b);
        l0_reset();
    }
    g_pe_insns = NULL;
    if (g_n_pe_real > 0) {
        blk->pushelide = (struct JitPushElide *)malloc((size_t)g_n_pe_real * sizeof *blk->pushelide);
        if (blk->pushelide) {
            memcpy(blk->pushelide, g_pe_real, (size_t)g_n_pe_real * sizeof *blk->pushelide);
            blk->n_pushelide = (uint16_t)g_n_pe_real;
        }
    }

    l0_flush_all(&b);
    if (!is_terminator(blk->insns[n - 1].op)) {

        emit_materialize(&b);
        a64_mov_imm64(&b, JT0, mode32 ? (uint64_t)(uint32_t)pc : pc);
        a64_str(&b, 8, JT0, 20, RIP_OFF);
        a64_mov_imm64(&b, 0, OCERZ_STEP_OK);
    }

    uint32_t *exit_label = a64_label(&b);
    emit_xmm_pin_spill_all(&b);
    emit_spill_pinned(&b);
    emit_frame_sp_reset(&b);
    emit_pin_epilogue_restore(&b);
    uint32_t *dstub = mode32 ? jit->dispatch_stub32 : jit->dispatch_stub;
    if (term_may_switch_mode(blk->insns[n - 1].op))
        dstub = NULL;
    if (dstub) {
        a64_mov_reg(&b, 1, 1, 20);
        a64_mov_reg(&b, 1, JTT, 0);
        a64_mov_reg(&b, 1, 0, 19);
        a64_ldp_post(&b, 19, 20, 31, 16);
        a64_ldp_post(&b, 29, 30, 31, 16);
        uint32_t *nonzero = a64_label(&b); a64_cbnz(&b, 1, JTT, 0);
        uint32_t *here = a64_label(&b);
        ptrdiff_t soff = dstub - here;
        if (g_tc_on) {
            tc_imm64(&b, 16, TCR_DSTUB, (uint64_t)mode32, (uint64_t)(uintptr_t)dstub);
            a64_br(&b, 16);
        } else if (soff >= -(ptrdiff_t)(1 << 25) && soff <= (ptrdiff_t)((1 << 25) - 1)) {
            a64_b(&b, (int32_t)soff);
        } else {
            a64_mov_imm64(&b, 16, (uint64_t)(uintptr_t)dstub);
            a64_br(&b, 16);
        }
        a64_patch_cbz(nonzero, a64_label(&b));
        a64_mov_reg(&b, 1, 0, JTT);
        a64_ret(&b);
    } else {
        a64_ldp_post(&b, 19, 20, 31, 16);
        a64_ldp_post(&b, 29, 30, 31, 16);
        a64_ret(&b);
    }
    int side_patch_oor = 0;
    for (int k = 0; k < g_n_side; k++)
        if (g_side[k].probe && !blk->prof) blk->prof = (JitProf *)calloc(SIDE_MAX, sizeof(JitProf));
    for (int k = 0; k < g_n_side; k++) {
        uint32_t *stub = a64_label(&b);
        uint32_t w = *g_side[k].site;
        if ((w & 0x7e000000u) == 0x36000000u) {
            if (!a64_try_patch_tbz(g_side[k].site, stub))
                side_patch_oor = 1;
        }
        else if ((w & 0x7e000000u) == 0x34000000u) a64_patch_cbz(g_side[k].site, stub);
        else a64_patch_bcond(g_side[k].site, stub);
        int edge_class = body_edge_pin_class();
        int body_edge = edge_class >= 0;
        g_side[k].stub = stub;
        for (int r = 0; r < 16; r++)
            if ((g_side[k].l0_dirty & (1u << r)) && g_side[k].l0[r] >= 0) {
                if (g_side[k].l0_dbl[r]) a64_ins_d_d(&b, xmm_vreg((unsigned)r), 0, g_side[k].l0[r], 0);
                else                     a64_ins_s_s(&b, xmm_vreg((unsigned)r), 0, g_side[k].l0[r], 0);
            }
        yc_flush_from(&b, g_side[k].yc_dirty);
        if (g_side[k].fpb >= 0 && g_side[k].fpb_chk)
            fpb_emit_regs_check(&b, g_side[k].fpb_chk, g_side[k].fpb, g_side[k].fpb_end, g_side[k].l0, g_side[k].l0_dbl);
        if (g_side[k].rec) {
            if (g_side[k].rec_imm_pending) a64_mov_imm64(&b, JT1, g_side[k].rec_imm);
            emit_defer_flags(&b, g_side[k].rec_ccop, g_side[k].rec_src, g_side[k].rec_dst);
        }
        if (g_side[k].probe && blk->prof) {
            JitProf *pf = &blk->prof[k];
            emit_prof_count(&b, pf, 0);
            pf->tk_trip = a64_label(&b);
            a64_tbnz(&b, JT2, PROBE_BIT, 0);
            g_side[k].patch_b = emit_static_chain_tail(&b, g_side[k].taken, 0, body_edge, epi_sites, &n_epi);
            a64_patch_tbz(pf->tk_trip, a64_label(&b));
            g_tag_blk = blk; g_tag_idx = k;
            emit_static_chain_tail(&b, g_side[k].taken, 0, body_edge, epi_sites, &n_epi);
            a64_patch_b(g_side[k].ft_site, a64_label(&b));
            emit_prof_count(&b, pf, 4);
            a64_tbnz(&b, JT2, PROBE_BIT, 2);
            uint32_t *back = a64_label(&b);
            a64_b(&b, 0);
            a64_patch_b(back, g_side[k].ft_site + 1);
            for (int r = 0; r < 16; r++)
                if ((g_side[k].l0_dirty & (1u << r)) && g_side[k].l0[r] >= 0) {
                    if (g_side[k].l0_dbl[r]) a64_ins_d_d(&b, xmm_vreg((unsigned)r), 0, g_side[k].l0[r], 0);
                    else                     a64_ins_s_s(&b, xmm_vreg((unsigned)r), 0, g_side[k].l0[r], 0);
                }
            yc_flush_from(&b, g_side[k].yc_dirty);
            emit_static_chain_tail(&b, g_side[k].ft_rip, 0, body_edge, epi_sites, &n_epi);
            g_tag_blk = NULL;
            pf->ft_site = g_side[k].ft_site;
        } else {
            if (g_side[k].ft_site) a64_patch_b(g_side[k].ft_site, g_side[k].ft_site + 1);
            g_side[k].patch_b = emit_static_chain_tail(&b, g_side[k].taken, 0, body_edge, epi_sites, &n_epi);
        }
    }
    for (int k = 0; k < g_n_fpb; k++) {
        FpBatch *fb = &g_fpb[k];
        if (!fb->site) continue;
        uint32_t *lbl = a64_label(&b);
        a64_patch_bcond(fb->site, lbl);
        if (fb->gain) a64_patch_b(fb->site + fb->gain, lbl);
        fpb_replay_prelude(&b, fb, fb->l0, fb->l0_dbl);
        yc_flush_from(&b, 0xffff);
        g_yc_dirty = 0;
        g_fpb_fast = 0;
        g_fpb_open = -1;
        l0_reset();
        ea_cache_reset();
        fpb_emit_undo_restore(&b, blk->insns, fb->first, fb->end, exit_sites, &n_exits);
        for (int m = fb->first; m <= fb->end; m++) {
            if (blk->insns[m].op == OCERZ_OP_JCC) continue;
            g_cur_insn_idx = m;
            g_cur_need = fl_need[m];
            g_cur_fpb = -1;
            uint32_t *lo = a64_label(&b);
            lanerec_note((uint32_t)(lo - entry));
            l0_pre_insn(&b, &blk->insns[m]);
            if (!try_inline(&b, &blk->insns[m], fl_need[m], exit_sites, &n_exits))
                emit_slowcall(&b, &blk->insns[m], exit_sites, &n_exits);
            if (g_n_fpbmap < JIT_MAX_BLOCK_INSNS) {
                g_fpbmap[g_n_fpbmap].lo = (uint32_t)(lo - entry);
                g_fpbmap[g_n_fpbmap].hi = (uint32_t)(a64_label(&b) - entry);
                g_fpbmap[g_n_fpbmap].idx = m;
                g_n_fpbmap++;
            }
        }
        l0_flush_all(&b);
        for (int t = 4; t < 4 + L0_NLANES; t++) {
            for (int r = 0; r < 16; r++) {
                if (fb->l0[r] != (int8_t)t) continue;
                if (fb->l0_dbl[r]) a64_ins_d_d(&b, t, 0, xmm_vreg((unsigned)r), 0);
                else               a64_ins_s_s(&b, t, 0, xmm_vreg((unsigned)r), 0);
                break;
            }
        }
        if (fb->fcmp_vreg >= 0)
            a64_fcmp(&b, 1, fb->fcmp_vreg, fb->fcmp_vreg);
        uint32_t *here = a64_label(&b);
        a64_b(&b, (int32_t)(fb->back - here));
    }
    for (int k = 0; k < g_n_fpb_sites; k++) {
        FpbSite *st = &g_fpb_sites[k];
        FpBatch *fb = &g_fpb[st->batch];
        a64_patch_bcond(st->site, a64_label(&b));
        if (st->keep_jt) { a64_stp_pre(&b, 9, 10, 31, -32); a64_stp_off(&b, 11, 12, 31, 16); }
        fpb_replay_prelude(&b, fb, st->l0, st->l0_dbl);
        yc_flush_from(&b, 0xffff);
        g_yc_dirty = 0;
        g_fpb_fast = 0;
        g_fpb_open = -1;
        l0_reset();
        ea_cache_reset();
        fpb_emit_undo_restore(&b, blk->insns, fb->first, st->end, exit_sites, &n_exits);
        for (int m = fb->first; m <= st->end; m++) {
            if (blk->insns[m].op == OCERZ_OP_JCC) continue;
            g_cur_insn_idx = m;
            g_cur_need = fl_need[m];
            g_cur_fpb = -1;
            uint32_t *lo = a64_label(&b);
            lanerec_note((uint32_t)(lo - entry));
            l0_pre_insn(&b, &blk->insns[m]);
            if (!try_inline(&b, &blk->insns[m], fl_need[m], exit_sites, &n_exits))
                emit_slowcall(&b, &blk->insns[m], exit_sites, &n_exits);
            if (g_n_fpbmap < JIT_MAX_BLOCK_INSNS) {
                g_fpbmap[g_n_fpbmap].lo = (uint32_t)(lo - entry);
                g_fpbmap[g_n_fpbmap].hi = (uint32_t)(a64_label(&b) - entry);
                g_fpbmap[g_n_fpbmap].idx = m;
                g_n_fpbmap++;
            }
        }
        l0_flush_all(&b);
        for (int t = 4; t < 4 + L0_NLANES; t++) {
            for (int r = 0; r < 16; r++) {
                if (st->l0[r] != (int8_t)t) continue;
                if (st->l0_dbl[r]) a64_ins_d_d(&b, t, 0, xmm_vreg((unsigned)r), 0);
                else               a64_ins_s_s(&b, t, 0, xmm_vreg((unsigned)r), 0);
                break;
            }
        }
        if (st->fcmp_a >= 0)
            a64_fcmp(&b, st->fcmp_dbl, st->fcmp_a, st->fcmp_b);
        if (st->keep_jt) { a64_ldp_off(&b, 11, 12, 31, 16); a64_ldp_post(&b, 9, 10, 31, 32); }
        uint32_t *here = a64_label(&b);
        a64_b(&b, (int32_t)(st->back - here));
    }
    emit_oolslow_arms(&b, exit_sites, &n_exits);
    emit_x87_arms(&b, exit_sites, &n_exits, epi_sites, &n_epi);
    emit_low_hoist_bail(&b, rip, epi_sites, &n_epi);
    emit_guard_arms(&b, entry);
    emit_ordered_slow_arms(&b, blk, entry);
    emit_nan_ool_arms(&b, blk, entry);
    if (loop_poll_exit) {
        uint32_t *poll_stub = a64_label(&b);
        a64_mov_imm64(&b, JT0, rip);
        a64_str(&b, 8, JT0, 20, RIP_OFF);
        a64_mov_imm64(&b, 0, OCERZ_STEP_OK);
        uint32_t *here = a64_label(&b);
        a64_b(&b, (int32_t)(exit_label - here));
        a64_patch_cbz(loop_poll_exit, poll_stub);
    }

    uint32_t *chain_tail_lbl = NULL;
    uint32_t *chain_patch_b = NULL;
    int chain_is_body = 0;
    if (!g_no_chain && g_chain_target) {
        chain_tail_lbl = a64_label(&b);
        if (g_pin_class == 3) {
            if (!g_chain_keeps_jgb) emit_reload_jgb(&b);
            chain_patch_b = emit_body_chain_tail(&b, g_chain_target, 0, epi_sites, &n_epi);
            chain_is_body = 1;
        } else {
            chain_patch_b = emit_chain_tail(&b, 0);
        }
    }

    if (g_flaglive_log)
        fprintf(stderr, "ocerz: FLAGLIVE rip=%#llx EMITTED words=%d guest=%d per_guest=%.2f\n",
                (unsigned long long)rip, (int)(b.p - entry), n,
                (double)(b.p - entry) / (double)n);

    if (g_n_raslit && !b.overflow) {
        if (((uintptr_t)b.p & 7) != 0) a64_emit32(&b, 0xd503201fu);
        g_tc_pool_off = (uint32_t)(b.p - entry);
        for (int i = 0; i < g_n_raslit; i++) {
            void **cell = (void **)b.p;
            a64_emit32(&b, 0); a64_emit32(&b, 0);
            if (b.overflow) break;
            int32_t off = (int32_t)((uint32_t *)cell - g_raslit[i].site);
            if (g_raslit[i].kind == 2) {
                a64_emit32(&b, 0); a64_emit32(&b, 0);
                if (b.overflow) break;
                *g_raslit[i].site = 0x9c000000u | (((uint32_t)off & 0x7ffffu) << 5) | (uint32_t)(g_raslit[i].rt & 31);
                ((uint64_t *)cell)[0] = g_raslit[i].retaddr;
                ((uint64_t *)cell)[1] = g_raslit[i].hi;
                continue;
            }
            *g_raslit[i].site = 0x58000000u | (((uint32_t)off & 0x7ffffu) << 5) | (uint32_t)(g_raslit[i].rt & 31);
            if (g_raslit[i].kind == 1) {
                *cell = (void *)(uintptr_t)g_raslit[i].retaddr;
                if (g_raslit[i].tcr == TCR_PSC)
                    tc_note((uint32_t *)cell, TCR_PSC, 1, 0);
                continue;
            }
            if (g_tc_on) {
                *cell = NULL;
                tc_note((uint32_t *)cell, TCR_RASCELL, 1, g_raslit[i].retaddr);
                continue;
            }
            ras_cell_register(cell);
            JitBlock *rb = cache_lookup(g_xlat_jit, g_raslit[i].retaddr, g_xlat_mode32);
            if (rb && rb->code)
                *cell = ras_entry_for(rb);
            else
                pending_add_ras(jit_key(g_raslit[i].retaddr, g_xlat_mode32), cell);
        }
        g_n_raslit = 0;
    }

    if (!b.overflow) {
        for (int i = 0; i < n_exits; i++)
            a64_patch_cbz(exit_sites[i], exit_label);
        for (int i = 0; i < n_epi; i++)
            a64_patch_b(epi_sites[i], exit_label);

        if (chain_tail_lbl && g_chain_epi)
            a64_patch_b(g_chain_epi, chain_tail_lbl);
        if (g_stop_patch) {
            assert(g_stop_target);
            uint32_t running_insn = *g_stop_patch;
            blk->stop_patch = g_stop_patch;
            blk->stop_insn = stop_retarget(running_insn, g_stop_patch, g_stop_target);
            if (jit->stop_requested)
                *g_stop_patch = blk->stop_insn;
            else
                *g_stop_patch = running_insn;
        }
        blk->push_fix = NULL; blk->n_push_fix = 0;
        if (g_n_push_fix) {
            blk->push_fix = (uint32_t *)malloc((size_t)g_n_push_fix * sizeof(uint32_t));
            if (blk->push_fix) {
                memcpy(blk->push_fix, g_push_fix, (size_t)g_n_push_fix * sizeof(uint32_t));
                blk->n_push_fix = (uint16_t)g_n_push_fix;
            }
        }
        blk->n_stop_extra = 0;
        for (int i = 0; i < g_n_stop_extra; i++) {
            uint32_t *site = g_stop_extra[i].site;
            blk->stop_extra[blk->n_stop_extra].site = site;
            blk->stop_extra[blk->n_stop_extra].insn =
                stop_retarget(*site, site, g_stop_extra[i].target);
            blk->n_stop_extra++;
            if (jit->stop_requested)
                *site = blk->stop_extra[blk->n_stop_extra - 1].insn;
        }
    }

    pthread_jit_write_protect_np(1);

    if (side_patch_oor) {
        static int warned_oor;
        if (!warned_oor) {
            warned_oor = 1;
            fprintf(stderr, "ocerz: note: superblock side exit out of TBZ range at rip=%#llx"
                            " (%u words); block runs interpreted\n",
                    (unsigned long long)rip, (unsigned)(b.p - entry));
        }
        blk->n_slow = n;
        blk->n_inlined = 0;
        blk->n_pinned = 0;
        blk->pin_class = 0;
        blk->code = NULL;
        blk->body_code = NULL;
        g_pin = NULL; g_pin_hold = NULL; g_n_pinned = 0; g_pin_class = 0;
        g_lowstack = 0;
        g_m32low = 0;
        cache_insert(jit, blk);
        return blk;
    }

    if (b.overflow) {
        t_xlat_overflow = 1;
        static int warned;
        if (!jit->code_full && !warned++)
            fprintf(stderr, "ocerz: warning: JIT code arena full (%zu MB, %llu blocks); it is flushed once every thread can leave it\n",
                    jit->code_bytes >> 20,
                    (unsigned long long)jit->blocks_translated);
        jit->code_full = 1;
        if (ocerz_jitstat > 0) { js_fail_overflow++; js_note_fail(rip, JSR_OVERFLOW, n); }

        blk->n_slow = n;
        blk->n_inlined = 0;
        blk->n_pinned = 0;
        blk->pin_class = 0;
        blk->code = NULL;
        g_pin = NULL; g_pin_hold = NULL; g_n_pinned = 0; g_pin_class = 0;
        g_lowstack = 0;
        g_m32low = 0;
        cache_insert(jit, blk);
        return blk;
    }

    sys_icache_invalidate(entry, (size_t)((b.p - entry) * 4));
    jit->code_cur = b.p;
    blk->code = (JitBlockFn)entry;
    blk->body_code = g_body_entry;
    blk->body_noreload = body_noreload;
    blk->hoist_sig = hoist_signature();
    blk->ordered_loads = (uint8_t)g_blk_ordered_loads;
    blk->code_words = (uint32_t)(b.p - entry);
    if (blk->stop_patch || blk->n_stop_extra) {
        blk->stop_next = jit->stop_blocks;
        jit->stop_blocks = blk;
    }

    assert(!(chain_patch_b && (g_n_jcc_edges || g_n_call_edges)) &&
           "block cannot mix legacy CALL, canonical CALL, and Jcc edges");
    assert(!(g_n_jcc_edges && g_n_call_edges) &&
           "block cannot have both canonical CALL and Jcc edges");
    if (g_n_call_edges) {
        for (int i = 0; i < g_n_call_edges; i++) {
            blk->edges[i].target_rip = g_call_edge[i].target_rip;
            blk->edges[i].patch_b = g_call_edge[i].patch_b;
            blk->edges[i].cond_site = NULL;
            blk->edges[i].kind = g_call_edge[i].kind;
            blk->edges[i].pin_class = g_call_edge[i].pin_class;
        }
        blk->n_edges = (uint8_t)g_n_call_edges;
    } else if (chain_patch_b) {
        blk->edges[0].target_rip = g_chain_target;
        blk->edges[0].patch_b = chain_patch_b;
        blk->edges[0].cond_site = NULL;
        blk->edges[0].kind = chain_is_body ? EDGE_BODY : EDGE_XBLOCK;
        blk->edges[0].pin_class = chain_is_body ? 3 : 0;
        blk->n_edges = 1;
    } else if (g_n_jcc_edges) {
        for (int i = 0; i < g_n_jcc_edges; i++) {
            blk->edges[i].target_rip = g_jcc_edge[i].target_rip;
            blk->edges[i].patch_b = g_jcc_edge[i].patch_b;
            blk->edges[i].cond_site = g_jcc_edge[i].cond_site;
            blk->edges[i].kind = g_jcc_edge[i].kind;
            blk->edges[i].pin_class = g_jcc_edge[i].pin_class;
        }
        blk->n_edges = (uint8_t)g_n_jcc_edges;
    }
    for (int k = 0; k < g_n_side && blk->n_edges < 8; k++) {
        if (!g_side[k].patch_b) continue;
        int e = blk->n_edges++;
        blk->edges[e].target_rip = g_side[k].taken;
        blk->edges[e].patch_b = g_side[k].patch_b;
        blk->edges[e].cond_site = side_stub_has_work(k) ? NULL : g_side[k].site;
        blk->edges[e].kind = body_edge_pin_class() >= 0 ? EDGE_BODY : EDGE_XBLOCK;
        blk->edges[e].pin_class = body_edge_pin_class() >= 0 ? (uint8_t)body_edge_pin_class() : 0;
        blk->edges[e].side = (uint8_t)(k + 1);
        blk->edges[e].jcc_rip = g_side[k].jcc_rip;
        blk->edges[e].probing = (uint8_t)(g_side[k].probe && blk->prof != NULL);
    }
    for (int i = 0; i < blk->n_edges; i++) {
        blk->edges[i].fallback_insn = *blk->edges[i].patch_b;
        blk->edges[i].cond_orig = blk->edges[i].cond_site ? *blk->edges[i].cond_site : 0;
    }

    {
        static int g_jitdis = -1;
        static FILE *g_jf;
        static uint64_t g_jd_lo, g_jd_hi;
        static int g_jd_brief;
        if (g_jitdis < 0) {
            g_jd_brief = getenv("OCERZ_JITDIS_BRIEF") ? 1 : 0;
            const char *p = getenv("OCERZ_JITDIS");
            g_jitdis = p ? 1 : 0;
            const char *lo = getenv("OCERZ_JITDIS_LO"), *hi = getenv("OCERZ_JITDIS_HI");
            g_jd_lo = lo ? strtoull(lo, NULL, 0) : 0;
            g_jd_hi = hi ? strtoull(hi, NULL, 0) : ~0ull;
            if (p) {
                char pb[1024];
                snprintf(pb, sizeof pb, "%s.%d", p, (int)getpid());
                g_jf = fopen(pb, "w");
                if (g_jf) setvbuf(g_jf, NULL, _IOFBF, 1u << 20);
            }
        }
        if (g_jitdis > 0 && g_jf && blk->insn_off && rip >= g_jd_lo && rip < g_jd_hi) {
            char tb[128];
            uint32_t epi = (uint32_t)(exit_label - entry);
            fprintf(g_jf, "BLOCK rip=%#llx host=%p words=%u n_insns=%d inlined=%d slow=%d"
                          " prologue_words=%u epilogue_words=%u pin_class=%d n_pinned=%d body=%d\n",
                    (unsigned long long)rip, (void *)entry, blk->code_words, n,
                    blk->n_inlined, blk->n_slow, blk->insn_off[0],
                    blk->code_words - epi, (int)blk->pin_class, (int)blk->n_pinned,
                    blk->body_code != NULL);
            fprintf(g_jf, "  LABELS body_entry=%ld loop_entry=%ld stop_patch=%ld\n",
                    g_body_entry ? (long)(g_body_entry - entry) : -1L,
                    g_loop_entry ? (long)(g_loop_entry - entry) : -1L,
                    g_stop_patch ? (long)(g_stop_patch - entry) : -1L);
            for (int e = 0; e < blk->n_edges; e++)
                fprintf(g_jf, "  EDGE -> %#llx kind=%d pin_class=%d side=%d pb=+%ld cs=+%ld\n",
                        (unsigned long long)blk->edges[e].target_rip,
                        (int)blk->edges[e].kind, (int)blk->edges[e].pin_class, (int)blk->edges[e].side,
                        blk->edges[e].patch_b ? (long)(blk->edges[e].patch_b - entry) : -1L,
                        blk->edges[e].cond_site ? (long)(blk->edges[e].cond_site - entry) : -1L);
            if (n > 0 && blk->insn_off[0] > 0) {
                fprintf(g_jf, "  PRO off=0 words=%u\n", blk->insn_off[0]);
                for (uint32_t w = 0; !g_jd_brief && w < blk->insn_off[0]; w++)
                    fprintf(g_jf, "    %08x\n", entry[w]);
            }
            for (int i = 0; i < n; i++) {
                uint32_t s = blk->insn_off[i];
                uint32_t e = (i + 1 < n) ? blk->insn_off[i + 1] : epi;
                ocerz_format_insn(&blk->insns[i], tb, sizeof tb);
                fprintf(g_jf, "  INSN %d off=%u words=%u  %s\n", i, s,
                        e > s ? e - s : 0, tb);
                for (uint32_t w = s; !g_jd_brief && w < e; w++)
                    fprintf(g_jf, "    %08x\n", entry[w]);
            }
            fprintf(g_jf, "  EPI off=%u words=%u\n", epi, blk->code_words - epi);
            for (uint32_t w = epi; !g_jd_brief && w < blk->code_words; w++)
                fprintf(g_jf, "    %08x\n", entry[w]);
            fflush(g_jf);
        }
    }

    int tc_save = 0;
    if (g_tc_on) {
        if (ocerz_tcache_mode() != OCERZ_TC_ROUNDTRIP || !tc_roundtrip(jit, blk, entry)) {
            pthread_jit_write_protect_np(0);
            tc_bind(jit, blk, entry, g_tc_rel, g_tc_nrel, 0);
            pthread_jit_write_protect_np(1);
        }
        tc_save = g_tc_rec && !g_tc_bad;
        g_tc_on = 0;
    }

    if (!code_index_append_locked(jit, blk)) {
        static int warned;
        if (!warned) { warned = 1; fprintf(stderr, "ocerz: warning: JIT code index allocation failed; block %#llx runs interpreted\n", (unsigned long long)rip); }
        blk->n_slow = n;
        blk->n_inlined = 0;
        blk->n_pinned = 0;
        blk->pin_class = 0;
        blk->code = NULL;
        blk->body_code = NULL;
        g_pin = NULL;
        g_pin_hold = NULL;
        g_n_pinned = 0;
        g_pin_class = 0;
        cache_insert(jit, blk);
        return blk;
    }

    {
        static int ec = -1;
        if (ec < 0) ec = getenv("OCERZ_EMITCHECK") ? 1 : 0;
        if (ec && blk->code) {
            const uint32_t *cw = (const uint32_t *)blk->code;
            for (uint32_t w = 0; w < blk->code_words; w++) {
                uint32_t v = cw[w];
                if ((v & 0x7c000000u) != 0x14000000u) continue;
                int64_t off = (int64_t)((int32_t)(v << 6) >> 6) * 4;
                const uint32_t *tgt = (const uint32_t *)((const uint8_t *)(cw + w) + off);
                if (tgt < jit->code_base || tgt > jit->code_cur + 4096) {
                    int ii = -1;
                    if (blk->insn_off)
                        for (int k = 0; k < blk->n_insns; k++)
                            if (blk->insn_off[k] <= w) ii = k;
                    fprintf(stderr, "ocerz: EMITCHECK[%d] rip=%#llx word=%u insn=%d v=%08x tgt=%p arena=[%p,%p)\n",
                            (int)getpid(), (unsigned long long)blk_rip(blk), w, ii, v, (const void *)tgt,
                            (void *)jit->code_base, (void *)jit->code_cur);
                    fprintf(stderr, "ocerz: EMITCHECK[%d]   ctx:", (int)getpid());
                    for (int q = (int)w - 6; q <= (int)w + 6 && q < (int)blk->code_words; q++)
                        if (q >= 0) fprintf(stderr, "%s%08x", q == (int)w ? " |" : " ", cw[q]);
                    fprintf(stderr, "\n" "ocerz: EMITCHECK[%d]   edges:", (int)getpid());
                    for (int q = 0; q < blk->n_edges; q++)
                        fprintf(stderr, " [%d]tgt=%#llx pb=+%#lx cs=+%#lx", q,
                                (unsigned long long)blk->edges[q].target_rip,
                                blk->edges[q].patch_b ? (long)(blk->edges[q].patch_b - (uint32_t *)blk->code) : -1L,
                                blk->edges[q].cond_site ? (long)(blk->edges[q].cond_site - (uint32_t *)blk->code) : -1L);
                    fprintf(stderr, " nool=%d nosl=%d nstop=%d\n", g_n_oolslow, g_n_oslow, blk->n_stop_extra);
                }
            }
        }
    }
    blk->lanerec = NULL; blk->n_lanerec = 0;
    if (g_n_lanerec > 0) {
        blk->lanerec = (struct JitLaneRec *)malloc((size_t)g_n_lanerec * sizeof *blk->lanerec);
        if (blk->lanerec) { memcpy(blk->lanerec, g_lanerec, (size_t)g_n_lanerec * sizeof *blk->lanerec); blk->n_lanerec = g_n_lanerec; }
    }
    g_n_lanerec = 0;
    compact_block(blk);
    cache_insert(jit, blk);

    if (tc_save) {
        if (tc_hit && ocerz_tcache_mode() == OCERZ_TC_VERIFY)
            tc_verify(jit, blk, tc_hit);
        else
            tc_put(jit, blk);
    }
    blk_chain_install(jit, blk);

    jit->blocks_translated++;
    if (ocerz_jitstat > 0)
        js_xlat_ok++;
    if (ocerz_jit_time_xlat)
        __atomic_add_fetch(&ocerz_jit_xlat_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - xlat_t0, __ATOMIC_RELAXED);
    if (g_jitmeasure) {

        static unsigned long long g_xlat_ns, g_xlat_sc_ns;
        static unsigned g_xlat_n, g_xlat_sc_n;
        unsigned long long ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - xlat_t0;
        g_xlat_n++;
        g_xlat_ns += ns;
        if (rip >= 0x7ff800000000ull) {
            g_xlat_sc_n++;
            g_xlat_sc_ns += ns;
        }
        if ((g_xlat_n & 0x3fff) == 0)
            fprintf(stderr, "ocerz: XLAT[%d] blocks=%u xlat_total=%llums | shared-cache: blocks=%u xlat=%llums\n",
                    (int)getpid(), g_xlat_n, g_xlat_ns / 1000000ull,
                    g_xlat_sc_n, g_xlat_sc_ns / 1000000ull);
    }
    return blk;
}

int jit_interp_block(struct OcerzVM *vm, OcerzCPU *cpu, JitBlock *b)
{
    static int lg = -1; if (lg < 0) lg = getenv("OCERZ_IBLOG") ? 1 : 0;
    if (lg) fprintf(stderr, "ocerz: INTERP-BLOCK rip=%#llx n=%d\n", (unsigned long long)blk_rip(b), b->n_insns);
    if (!b->insns) return OCERZ_EUNSUP;
    ocerz_jit_exec_state = 2;
    ocerz_flags_materialize(cpu);
    if (ocerz_perfstat > 0)
        b->exec_count++;
    for (int i = 0; i < b->n_insns; i++) {
        const X86Insn *in = &b->insns[i];
        int r = ocerz_jit_exec_one(vm, cpu, in);
        if (r != OCERZ_STEP_OK) {
            ocerz_jit_exec_state = 0;
            return r;
        }
        if (cpu->rip != in->rip + in->len || cpu->interp_once) {
            ocerz_jit_exec_state = 0;
            return OCERZ_STEP_OK;
        }
    }
    ocerz_jit_exec_state = 0;
    return OCERZ_STEP_OK;
}

static int ps_cmp(const void *a, const void *bb)
{
    unsigned long long x = ((const PsOpRow *)a)->n, y = ((const PsOpRow *)bb)->n;
    return x < y ? 1 : x > y ? -1 : 0;
}

void ps_report(OcerzJit *jit)
{
    double sec = (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - ps_t0) / 1e9;
    if (sec <= 0)
        sec = 1e-9;
    unsigned long long blk_exec = 0, ins_inl = 0, ins_slow_static = 0, nblocks = 0, ncompiled = 0;
    unsigned long long static_insns = 0;

    unsigned long long nonempty = 0, maxchain = 0;
    unsigned long long probe_w = 0;
    for (unsigned i = 0; i < JIT_HASH_SIZE; i++) {
        unsigned long long pos = 0;
        for (JitBlock *b = __atomic_load_n(&jit->buckets[i], __ATOMIC_ACQUIRE); b; b = b->hnext) {
            nblocks++;
            pos++;
            if (b->code)
                ncompiled++;
            static_insns += (unsigned)b->n_insns;
            unsigned long long e = b->exec_count;
            blk_exec += e;
            probe_w += pos * e;
            ins_inl += e * (unsigned)b->n_inlined;
            ins_slow_static += e * (unsigned)b->n_slow;
        }
        if (pos) {
            nonempty++;
            if (pos > maxchain)
                maxchain = pos;
        }
    }
    unsigned long long slow = ps_slow_insns;
    unsigned long long total = ins_inl + slow;
    unsigned long long st = ps_steps, hi = ps_hits, mi = ps_misses;

    fprintf(stderr,
        "ocerz: PERFSTAT[%d] t=%.1fs blocks=%llu (compiled=%llu) static_insns/blk=%.2f\n"
        "ocerz: PERFSTAT[%d]   EXECUTED insns: total=%llu  slow(exec_one)=%llu (%.2f%%)  inlined=%llu (%.2f%%)\n"
        "ocerz: PERFSTAT[%d]   slow_static_est=%llu (guard-slowcalls = %lld)\n"
        "ocerz: PERFSTAT[%d]   block_execs=%llu (%.0f/s)  jit_step/cache_lookup=%llu (%.0f/s) hits=%llu misses=%llu\n"
        "ocerz: PERFSTAT[%d]   avg EXECUTED insns per block = %.2f   insns/s = %.0f\n"
        "ocerz: PERFSTAT[%d]   HASH bits=%d buckets=%u nonempty=%llu load=%.3f maxchain=%llu"
        " mean_probes/lookup(exec-weighted)=%.3f\n",
        (int)getpid(), sec, nblocks, ncompiled,
        nblocks ? (double)static_insns / (double)nblocks : 0.0,
        (int)getpid(), total, slow, total ? 100.0 * (double)slow / (double)total : 0.0,
        ins_inl, total ? 100.0 * (double)ins_inl / (double)total : 0.0,
        (int)getpid(), ins_slow_static, (long long)slow - (long long)ins_slow_static,
        (int)getpid(), blk_exec, (double)blk_exec / sec, st, (double)st / sec, hi, mi,
        (int)getpid(), blk_exec ? (double)total / (double)blk_exec : 0.0,
        (double)total / sec,
        (int)getpid(), JIT_HASH_BITS, (unsigned)JIT_HASH_SIZE, nonempty,
        (double)nblocks / (double)JIT_HASH_SIZE, maxchain,
        blk_exec ? (double)probe_w / (double)blk_exec : 0.0);

    {
        enum { HB = 12 };
        JitBlock *top[HB] = {0}; double topw[HB] = {0};
        for (unsigned i = 0; i < JIT_HASH_SIZE; i++)
            for (JitBlock *bb = __atomic_load_n(&jit->buckets[i], __ATOMIC_ACQUIRE); bb; bb = bb->hnext) {
                double w = (double)bb->exec_count * (double)bb->code_words;
                for (int k = 0; k < HB; k++)
                    if (w > topw[k]) {
                        for (int m = HB - 1; m > k; m--) { top[m] = top[m-1]; topw[m] = topw[m-1]; }
                        top[k] = bb; topw[k] = w; break;
                    }
            }
        for (int k = 0; k < HB && top[k]; k++) {
            JitBlock *bb = top[k];
            fprintf(stderr, "ocerz: PERFSTAT[%d]   HOTBLOCK #%2d rip=%#llx execs=%llu insns=%d words=%u w/insn=%.1f pin=%d slow=%d\n",
                    (int)getpid(), k + 1, (unsigned long long)blk_rip(bb),
                    (unsigned long long)bb->exec_count, bb->n_insns, bb->code_words,
                    bb->n_insns ? (double)bb->code_words / bb->n_insns : 0.0,
                    (int)bb->pin_class, bb->n_slow);
        }
    }
    PsOpRow rows[OCERZ_OP_COUNT];
    for (unsigned i = 0; i < OCERZ_OP_COUNT; i++) {
        rows[i].op = i;
        rows[i].n = ps_ops[i];
    }
    qsort(rows, OCERZ_OP_COUNT, sizeof rows[0], ps_cmp);
    unsigned long long cum = 0;
    for (int i = 0; i < 24 && rows[i].n; i++) {
        cum += rows[i].n;
        fprintf(stderr, "ocerz: PERFSTAT[%d]   SLOWOP #%2d %-12s %14llu  %5.2f%% of slow  cum %5.2f%%  (%.2f%% of ALL)  e.g. %s | %s | %s\n",
                (int)getpid(), i + 1, ocerz_op_name(rows[i].op), rows[i].n,
                slow ? 100.0 * (double)rows[i].n / (double)slow : 0.0,
                slow ? 100.0 * (double)cum / (double)slow : 0.0,
                total ? 100.0 * (double)rows[i].n / (double)total : 0.0,
                ps_shapes[rows[i].op][0], ps_shapes[rows[i].op][1], ps_shapes[rows[i].op][2]);
    }
    {
        fprintf(stderr, "ocerz: PERFSTAT[%d]   RAS misses=%llu (stale=%llu, null entry=%llu, empty=%llu)  align-hotpatches=%llu  ras_slots=%u/%u call-sites-without-slot=%llu\n", (int)getpid(), ps_ras_miss, ps_ras_stale, ps_ras_null, ps_ras_sentinel, ps_align_patches, g_ras_slot_n, (unsigned)RAS_SLOT_CAP, ps_ras_noslot);
        {
            uint64_t top_n[12] = {0}, top_r[12] = {0};
            for (unsigned i = 0; i < PS_RETSITE_N; i++) {
                uint64_t n = ps_retsite[i].n;
                for (int k = 0; k < 12; k++)
                    if (n > top_n[k]) {
                        for (int q = 11; q > k; q--) { top_n[q] = top_n[q - 1]; top_r[q] = top_r[q - 1]; }
                        top_n[k] = n; top_r[k] = ps_retsite[i].rip;
                        break;
                    }
            }
            for (int k = 0; k < 12 && top_n[k]; k++)
                fprintf(stderr, "ocerz: PERFSTAT[%d]   RETMISS #%d rip=%#llx stale=%llu\n", (int)getpid(), k + 1,
                        (unsigned long long)top_r[k], (unsigned long long)top_n[k]);
        }
        unsigned long long cok = ps_chain_ok, cfar = ps_chain_far, ctot = cok + cfar + ps_chain_veneer;
        if (ctot)
            fprintf(stderr,
                    "ocerz: PERFSTAT[%d]   CHAIN activated=%llu veneered=%llu out_of_range=%llu (%.2f%% dropped)\n",
                    (int)getpid(), cok, ps_chain_veneer, cfar, 100.0 * (double)cfar / (double)ctot);
    }
    for (int i = 0; i < (int)(sizeof ps_shape / sizeof ps_shape[0]); i++) {
        unsigned long long easy = ps_shape[i][0], hard = ps_shape[i][1], s = easy + hard;
        if (s)
            fprintf(stderr, "ocerz: PERFSTAT[%d]   SHAPE %-7s easy=%llu (%.1f%%) other=%llu (%.1f%%)\n",
                    (int)getpid(), ps_shape_name[i], easy, 100.0 * (double)easy / (double)s,
                    hard, 100.0 * (double)hard / (double)s);
    }
}

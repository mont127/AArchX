/*
 * ---- flags ----
 * Flags are deferred: an instruction records {kind, size, dst, src} and the
 * flags are materialized only if something reads them.  On top of that sit
 * three fusions - NZCV forwarded from an adjacent producer across
 * NZCV-transparent gap instructions, value-based conditions taken straight from
 * a result register (cmp #0, or no compare at all for cbz/cbnz), and the comis
 * fusion, where a jcc/setcc/cmov re-derives its condition by redoing the fcmp.
 * Liveness reaches past the block: an exit's flags are dead if the successor,
 * decoded up to three blocks deep, overwrites them before reading them.  That
 * lookahead was a fifth of translation time, because every block branching to
 * an untranslated successor decoded it again, so answers are memoized by rip
 * and depth, and an entry counts only while no translation has been retired
 * since and the successor's first sixteen bytes are unchanged.
 * A static liveness pass and the emitters share the same predicates so they
 * cannot disagree about what is live.  The legality rules here are written in
 * blood: a gap may not write a register the producer read (a byte compare
 * through an index register the next instruction overwrites made the
 * NZCV-forwarding fallback re-load with the new index, and a loop spun forever),
 * and a gap's emission must touch only pinned registers (`cmp byte
 * [rdi+0x210],0 ; lea r15,[rsp+0x290] ; jne` branched on rsp and made Steam's
 * CEF browser copy an unengaged optional, 2026-09-06), so a gap is emitted into
 * a scratch buffer first and refused there rather than asserting after the
 * compare is already out.
 *
 * No ABI passes arithmetic flags across a return, but clang's outliner does:
 * a helper that ends in a compare and a ret hands its flags to a caller that
 * branches right after the call.  Treating every ret as killing the flags
 * painted every Wine window black (2026-09-05).  A pure flag producer
 * reaching a ret keeps its flags live; a tail whose last flag writer is
 * arithmetic (xor eax,eax; ret) returns a value, and keeps the dead seam.
 *
 * ---- control flow ----
 * A block may run past a FORWARD conditional branch, continuing inline and
 * putting the taken side in an out-of-line chain stub (a superblock).  When the
 * compiler laid the rare path out inline the hot path is the taken edge
 * instead, and the loop fragments into a chain of blocks; such a branch is
 * probed - both sides count in the arena - and a clearly hotter taken side
 * retires the block, which retranslates with the jcc rewritten as its
 * complement.  The first window of a loop is often unlike its steady state, so
 * a verdict counts only when the next window repeats it, and the fourth window
 * decides regardless.  OCERZ_FLIP_JCC names branches to invert by hand.
 *
 * Edges are chained block to block, and a conditional branch may be retargeted
 * straight at its successor - but only when nothing the successor needs sits
 * between the branch and the chain tail.  A stub that replays lane-0 flushes,
 * an FP-batch check or the producer's flag record must stay on the path: a
 * scalar SSE result left in lane-0 scratch and a side exit chained past the
 * flush produced a singular view transform in Cocoa, and a stale flag
 * record handed to a successor (`cmp ebp,0xb ; jbe L` with `L: ja`) failed every
 * SQLite open in libcef.
 *
 * A guest CALL pushes its return address and also pushes {retaddr, host
 * continuation} onto a host-stack shadow and a return-address stack, then `bl`s
 * into the callee body, so the hardware return predictor matches the RAS and a
 * guest RET is a plain ret.  32-bit calls and rets do the same, their shadow
 * entries tagged JIT_KEY_M32 (m32_ras_ok).  Indirect jmp/call go through a per-site
 * direct-mapped cache of 32 {rip, body} pairs (16-aligned, so the lookup's ldp
 * is single-copy atomic) before falling into an inlined hash probe and finally
 * C.  In 32-bit code a ret, an indirect call and an indirect jmp take the same
 * cache, their target keyed with JIT_KEY_M32 as a 32-bit block's is, where they
 * all left for the dispatcher before; under WoW64 a vtable-call loop went from
 * 1321 to 464 ms and qsort from 634 to 163 ms.  Small straight-line callees ending in a plain ret are spliced into the
 * caller: the call becomes a push, the ret a compare against the known return
 * address, and a mismatch leaves at the ret's rip for the dispatcher to run the
 * real one.  Where a frame is pure register work the push's slot is provably
 * never read, so the push becomes a bare rsp -= 8 and matched push/pop pairs
 * become register renames - the loop-carried store-to-load chain of call-dense
 * code.  The renames borrow x16, x17 and x30 when no memory base is hoisted
 * into them, and every C call-out clobbers all three, so a renamed pop whose
 * push was followed by a call-out reads the slot the push still wrote instead.
 * That happens whenever an instruction between them falls back to C, and in the
 * low shadow window it happened to every spliced call nested inside another,
 * because the inline call and ret need identity addressing there and go
 * through the slow path: a leaf-call loop built at 0x200000000 summed garbage
 * that changed with every run.
 *
 * In that window the stack may live below 12 GB, where guest and host
 * addresses differ, so every stack fast path used to be off there, and every
 * spliced call and ret went out to C: xbench's leafcall took 2.75 s against
 * 0.16 s outside the window.  None of the return-address stack depends on the
 * memory mode, only the two guest-stack accesses do, so with the stack pointer
 * pinned as a pointer and a zero guest base (low_stack_fast), a call stores its
 * return address and a ret loads it through the same translation any other
 * low-window access takes, and the rest - the host shadow push, bl into the
 * callee's body, the compare and the plain ret - is what it is everywhere
 * else.  A spliced call's pushes and rets do the same, and its elided pushes
 * and matched rets never touched memory to begin with.  leafcall went to
 * 0.35 s and icall from 0.74 to 0.44 (0.39 outside the window).
 * OCERZ_NO_LOW_RAS and OCERZ_NO_LOW_SPLICE turn the two halves off.
 *
 *
 * A setcc or cmovcc reads the forwarded NZCV, never the producer's
 * registers, so a sibling consumer in the gap may write one of them
 * (cmp [rcx],eax ; setg al ; setl dl).  A jcc can re-derive its
 * condition from them, so it keeps the rule.
 *
 * after ands, as after test, C and V are clear: be/a are e/ne, l/ge/le/g read N and Z alone
 *
 * A loop's first phase can differ from the rest (an array initialised
 * one way, then settled), and every window can fall in it.  So a kept
 * branch goes on counting its taken side, tripping at 2^WATCH_BIT, when
 * flip_side_hit probes it again from scratch, FLIP_REARMS times at most.
 *
 * A cmp/test whose first operand is memory, as emit_rmw_mem emits it inline with NZCV set.
 *
 * A load of a stack slot into a pinned register, which the Wine layout emits as
 * a plain load off rsp + x0 (lowstack_disp_ea): flag-free and touching only JTA,
 * so it may sit between a compare and its fused jcc.  It can fault, so
 * emit_cmp_test_jcc writes the compare's flags out before it.
 */
#include "ocerz/jit_internal.h"

static uint64_t xlive_dep_mix(uint64_t h, uint64_t w);
static uint64_t xlive_dep_hash(const XliveDep *d, int nd, const uint8_t *bytes);
static int xlive_deps_fetch(const XliveDep *d, int nd, uint8_t *dst, uint32_t room);
static int cmp_xdep(const void *a, const void *b);
static uint8_t xlive_deps_since(int start, XliveDep *out, uint64_t *hash);
static int xlive_deps_replay(const XliveDep *d, int nd, uint64_t want);
static void emit_cc_predicate_rflags(A64Buf *b, unsigned cc);
static int cc_after_subs(unsigned cc);
static int cc_after_ands(unsigned cc);
static unsigned producer_record_kind(const X86Insn *p, int *size);
static int cc_consumer_inline_ok(const X86Insn *c);
static int mem_plain_ok(const X86Insn *insn, const X86Operand *op);
static int rmw_nzcv_ok(const X86Insn *p, const X86Operand *m);
static int nzcv_gap_max(void);
static int nzcv_producer_candidate(const X86Insn *p);
static int nzcv_gap_shape(const X86Insn *in);
static int nzcv_gap_ok(const X86Insn *insns, int m, int k);
static int cc_after_adds(unsigned cc);
static int nzcv_dc_for(unsigned kind, unsigned cc);
static int fused_jcc_cond(const X86Insn *producer, const X86Insn *jcc);
static uint32_t *cond_short_site(uint32_t *to_taken, int taken_rec, int body_edge);
static int insn_writes_reg(const X86Insn *in, unsigned reg);
static int flag_neutral_ok(const X86Insn *in);
static int stack_gap_load_ok(const X86Insn *in);
static int jcc_gap_ok(const X86Insn *in);
static int emit_flag_neutral(A64Buf *b, const X86Insn *in);
static uint32_t *emit_incdec_jcc_arm(A64Buf *b, const X86Insn *producer,
                                     uint64_t target, int poll, int body_edge,
                                     int cf_reg,
                                     uint32_t **epilogue_sites, int *n_epi,
                                     int *recorded);
static int decode_ifconv_block(uint64_t rip, X86Insn *out, int cap);
static int ifconv_test_bit(const X86Insn *test, const X86Insn *jcc,
                           int *bit);
static int ifconv_direct_latch(const X86Insn *p, uint64_t loop_rip,
                               uint64_t *latch_rip, uint64_t *exit_rip);
static int ifconv_simple_path(const X86Insn *p, uint64_t latch_rip,
                              unsigned acc, unsigned size);
static int ifconv_complex_path(const X86Insn *p, uint64_t latch_rip,
                               unsigned acc, unsigned size);
static int match_ifconv_diamond(const X86Insn *test, const X86Insn *jcc,
                                IfConvDiamond *m);
static void flip_report_atexit(void);
static void flip_retire_block(struct OcerzVM *vm, OcerzJit *jit, JitBlock *blk);
static int flip_decide_locked(JitBlock *blk, int e, int tk, int ft, int logit);

const X86Insn *g_flag_producer;

int g_no_regflags;

int g_jcc_side_mode;

uint64_t g_jcc_side_need;

uint64_t g_jcc_side_fall_need;

int g_no_jcclink;

int g_no_xlive;

int g_no_jccfuse;

int g_tc_ndlog;

int g_tc_rec;

uint64_t g_self_rip;

uint32_t *g_loop_entry;

uint32_t *g_stop_patch;

struct JitState_g_side g_side[SIDE_MAX];

int g_n_side;

struct JitState_g_flip g_flip[FLIP_N];

int g_n_probes;

int flip_disabled(void)
{
    static int en = -1;
    if (en < 0) en = getenv("OCERZ_NO_FLIP") ? 1 : 0;
    return en;
}

int superblock_enabled(void)
{
    static int en = -1;
    if (en < 0) en = getenv("OCERZ_NO_SUPERBLOCK") ? 0 : 1;
    return en;
}

int jcc_flip_wanted(uint64_t jcc_rip)
{
    static uint64_t rips[32];
    static int n = -1;
    if (n < 0) {
        n = 0;
        const char *e = getenv("OCERZ_FLIP_JCC");
        while (e && *e && n < 32) {
            char *end;
            uint64_t v = strtoull(e, &end, 0);
            if (end == e) break;
            rips[n++] = v;
            e = *end == ',' ? end + 1 : end;
        }
    }
    for (int i = 0; i < n; i++)
        if (rips[i] == jcc_rip) return 1;
    int s = flip_state(jcc_rip);
    return s == FLIP_DECIDED_INV;
}

int superblock_back_enabled(void)
{
    static int en = -1;
    if (en < 0) en = getenv("OCERZ_NO_SB_BACK") ? 0 : 1;
    return en;
}

uint32_t *g_stop_target;

struct JitState_g_jcc_edge g_jcc_edge[2];

int g_n_jcc_edges;

OcerzJit *g_xlat_jit;

int g_xlat_mode32;

int g_xlive_log = -1;

static struct {
    uint64_t key, live, gen, dhash;
    uint8_t head[16];
    uint8_t nd;
    XliveDep dep[XLIVE_MEMO_DEPS];
} g_xlive_memo[XLIVE_MEMO_SLOTS];

static uint64_t xlive_dep_mix(uint64_t h, uint64_t w)
{
    h = (h ^ w) * 0x9e3779b97f4a7c15ull;
    return h ^ (h >> 29);
}

static uint64_t xlive_dep_hash(const XliveDep *d, int nd, const uint8_t *bytes)
{
    uint64_t h = 0x13198a2e03707344ull;
    for (int i = 0; i < nd; i++) {
        h = xlive_dep_mix(xlive_dep_mix(h, d[i].pc), d[i].len);
        uint32_t k = 0;
        for (; k + 8 <= d[i].len; k += 8) {
            uint64_t w;
            memcpy(&w, bytes + k, 8);
            h = xlive_dep_mix(h, w);
        }
        for (; k < d[i].len; k++)
            h = xlive_dep_mix(h, bytes[k]);
        bytes += d[i].len;
    }
    return h;
}

static uint8_t g_xlive_dbuf[TC_DBYTES_MAX];

static int xlive_deps_fetch(const XliveDep *d, int nd, uint8_t *dst, uint32_t room)
{
    volatile int ok = 0;
    sigjmp_buf fb;
    sigjmp_buf *prev = ocerz_jit_decode_recover;
    if (sigsetjmp(fb, 0) == 0) {
        ocerz_jit_decode_recover = &fb;
        uint32_t at = 0;
        int fits = 1;
        for (int i = 0; i < nd && fits; i++) {
            if (at + d[i].len > room) {
                fits = 0;
                break;
            }
            memcpy(dst + at, (const uint8_t *)ocerz_g2h(d[i].pc), d[i].len);
            at += d[i].len;
        }
        ok = fits;
    }
    ocerz_jit_decode_recover = prev;
    return ok;
}

static int cmp_xdep(const void *a, const void *b)
{
    const XliveDep *x = (const XliveDep *)a, *y = (const XliveDep *)b;
    return x->pc < y->pc ? -1 : x->pc > y->pc;
}

static uint8_t xlive_deps_since(int start, XliveDep *out, uint64_t *hash)
{
    enum { MAXE = 512 };
    XliveDep e[MAXE];
    int n = g_tc_ndlog - start;
    if (g_tc_bad || n <= 0 || n > MAXE)
        return XLIVE_NO_DEPS;
    for (int i = 0; i < n; i++) {
        e[i].pc = g_tc_dlog[start + i].pc;
        e[i].len = g_tc_dlog[start + i].len;
    }
    qsort(e, (size_t)n, sizeof e[0], cmp_xdep);
    int nd = 0;
    for (int i = 0; i < n; i++) {
        if (nd && e[i].pc <= out[nd - 1].pc + out[nd - 1].len) {
            uint64_t end = e[i].pc + e[i].len;
            if (end > out[nd - 1].pc + out[nd - 1].len)
                out[nd - 1].len = (uint32_t)(end - out[nd - 1].pc);
            continue;
        }
        if (nd == XLIVE_MEMO_DEPS)
            return XLIVE_NO_DEPS;
        out[nd++] = e[i];
    }
    if (!xlive_deps_fetch(out, nd, g_xlive_dbuf, sizeof g_xlive_dbuf))
        return XLIVE_NO_DEPS;
    *hash = xlive_dep_hash(out, nd, g_xlive_dbuf);
    return (uint8_t)nd;
}

static int xlive_deps_replay(const XliveDep *d, int nd, uint64_t want)
{
    if (g_tc_ndlog + nd > TC_DLOG_MAX ||
        !xlive_deps_fetch(d, nd, g_tc_dbytes + g_tc_nbytes, TC_DBYTES_MAX - g_tc_nbytes) ||
        xlive_dep_hash(d, nd, g_tc_dbytes + g_tc_nbytes) != want)
        return 0;
    for (int i = 0; i < nd; i++) {
        g_tc_dlog[g_tc_ndlog].pc = d[i].pc;
        g_tc_dlog[g_tc_ndlog].at = g_tc_nbytes;
        g_tc_dlog[g_tc_ndlog].len = d[i].len;
        g_tc_ndlog++;
        g_tc_nbytes += d[i].len;
    }
    return 1;
}

uint64_t xlive_decode_entry_d(uint64_t rip, int depth)
{
    static int maxd = -1, memo_on = -1;
    if (maxd < 0) { const char *e = getenv("OCERZ_XLIVE_DEPTH"); maxd = e ? atoi(e) : 3; }
    if (memo_on < 0) memo_on = getenv("OCERZ_NO_XLIVE_MEMO") ? 0 : 1;
    if (g_xlive_log < 0) g_xlive_log = getenv("OCERZ_XLIVELOG") ? 1 : 0;
    uint64_t mkey = ((rip << 4) | ((uint64_t)depth << 1) | (uint64_t)(g_xlat_mode32 != 0)) + 1;
    uint64_t mgen = __atomic_load_n(&ocerz_jit_retire_count, __ATOMIC_RELAXED);
    unsigned mslot = (unsigned)((mkey * 0x9E3779B97F4A7C15ull) >> 51) & (XLIVE_MEMO_SLOTS - 1);
    uint8_t head[16];
    int have_head = 0;
    int dstart = g_tc_ndlog;
    int dbar = g_tc_dbar;
    g_tc_dbar = dstart;
    if (memo_on) {
        sigjmp_buf hb;
        sigjmp_buf *hprev = ocerz_jit_decode_recover;
        if (sigsetjmp(hb, 0) == 0) {
            ocerz_jit_decode_recover = &hb;
            memcpy(head, (const uint8_t *)ocerz_g2h(rip), sizeof head);
            have_head = 1;
        }
        ocerz_jit_decode_recover = hprev;
        if (have_head && g_xlive_memo[mslot].key == mkey && g_xlive_memo[mslot].gen == mgen &&
            memcmp(g_xlive_memo[mslot].head, head, sizeof head) == 0) {
            if (!g_tc_rec || (g_xlive_memo[mslot].nd != XLIVE_NO_DEPS &&
                              xlive_deps_replay(g_xlive_memo[mslot].dep, g_xlive_memo[mslot].nd,
                                                g_xlive_memo[mslot].dhash))) {
                g_tc_dbar = dbar;
                return g_xlive_memo[mslot].live;
            }
        }
    }
    X86Insn insns[JIT_MAX_BLOCK_INSNS];
    volatile int n = 0;
    volatile uint64_t pc = rip;
    sigjmp_buf db;
    sigjmp_buf *prev = ocerz_jit_decode_recover;
    if (sigsetjmp(db, 0) == 0) {
        ocerz_jit_decode_recover = &db;
        while (n < JIT_MAX_BLOCK_INSNS) {
            int rc = jit_decode(pc, &insns[n], g_xlat_mode32);
            if (rc != OCERZ_OK)
                break;
            unsigned op = insns[n].op;
            uint8_t len = insns[n].len;
            n++;
            if (is_terminator(op))
                break;
            pc += len;
        }
    }
    ocerz_jit_decode_recover = prev;
    if (n == 0) {
        g_tc_dbar = dbar;
        return OCERZ_FL_ALL;
    }

    uint64_t live = OCERZ_FL_ALL;
    if (depth < maxd && is_terminator(insns[n - 1].op)) {
        const X86Insn *t = &insns[n - 1];
        if ((t->op == OCERZ_OP_JMP || t->op == OCERZ_OP_CALL) && t->ops[0].kind == OCERZ_OPK_IMM)
            live = xlive_succ_live_d(g_xlat_jit, t->ops[0].imm, depth + 1);
        else if (t->op == OCERZ_OP_JCC && t->ops[0].kind == OCERZ_OPK_IMM)
            live = xlive_succ_live_d(g_xlat_jit, t->ops[0].imm, depth + 1) |
                   xlive_succ_live_d(g_xlat_jit, t->rip + t->len, depth + 1);
    }
    for (int i = n - 1; i >= 0; i--) {
        uint64_t def, use;
        ocerz_flags_defuse(&insns[i], &def, &use);
        live = (live & ~def) | use;
    }
    if (g_xlive_log) fprintf(stderr, "ocerz: XLIVE rip=%#llx depth=%d n=%d term=%d live=%#llx\n", (unsigned long long)rip, depth, n, (int)insns[n-1].op, (unsigned long long)live);
    if (have_head) {
        g_xlive_memo[mslot].key = mkey;
        g_xlive_memo[mslot].live = live;
        g_xlive_memo[mslot].gen = mgen;
        memcpy(g_xlive_memo[mslot].head, head, sizeof head);
        g_xlive_memo[mslot].nd = g_tc_rec
            ? xlive_deps_since(dstart, g_xlive_memo[mslot].dep, &g_xlive_memo[mslot].dhash)
            : XLIVE_NO_DEPS;
    }
    g_tc_dbar = dbar;
    return live;
}

int canonical_body_successor(uint64_t rip)
{
    X86Insn insn;
    volatile uint64_t pc = rip;
    volatile int compatible = 0;
    sigjmp_buf db;
    sigjmp_buf *prev = ocerz_jit_decode_recover;
    if (sigsetjmp(db, 0) == 0) {
        ocerz_jit_decode_recover = &db;
        for (int n = 0; n < JIT_MAX_BLOCK_INSNS; n++) {
            if (jit_decode(pc, &insn, g_xlat_mode32) != OCERZ_OK)
                break;
            if (is_terminator(insn.op)) {
                compatible = insn.op == OCERZ_OP_JCC ||
                    insn.op == OCERZ_OP_JMP;
                break;
            }
            pc += insn.len;
        }
    }
    ocerz_jit_decode_recover = prev;
    return compatible;
}

unsigned decoded_terminator(uint64_t rip)
{
    X86Insn insn;
    volatile uint64_t pc = rip;
    volatile unsigned term = 0;
    sigjmp_buf db;
    sigjmp_buf *prev = ocerz_jit_decode_recover;
    if (sigsetjmp(db, 0) == 0) {
        ocerz_jit_decode_recover = &db;
        for (int n = 0; n < JIT_MAX_BLOCK_INSNS; n++) {
            if (jit_decode(pc, &insn, g_xlat_mode32) != OCERZ_OK)
                break;
            if (is_terminator(insn.op)) {
                term = insn.op;
                break;
            }
            pc += insn.len;
        }
    }
    ocerz_jit_decode_recover = prev;
    return term;
}

int decoded_call_region_entry(uint64_t rip)
{
    X86Insn insn;
    volatile uint64_t pc = rip;
    volatile int compatible = 0;
    volatile int rsp_ok = 1;
    sigjmp_buf db;
    sigjmp_buf *prev = ocerz_jit_decode_recover;
    if (sigsetjmp(db, 0) == 0) {
        ocerz_jit_decode_recover = &db;
        for (int n = 0; n < JIT_MAX_BLOCK_INSNS; n++) {
            if (jit_decode(pc, &insn, g_xlat_mode32) != OCERZ_OK)
                break;
            if (is_terminator(insn.op)) {
                if (insn.op == OCERZ_OP_CALL || insn.op == OCERZ_OP_RET) {
                    compatible = rsp_ok;
                } else if (insn.op == OCERZ_OP_JCC && rsp_ok &&
                           insn.ops[0].kind == OCERZ_OPK_IMM) {
                    compatible = call_body_successor(insn.ops[0].imm) &&
                        call_body_successor(insn.rip + insn.len);
                }
                break;
            }
            for (int k = 0; k < insn.nops; k++) {
                const X86Operand *o = &insn.ops[k];
                int rsp = (o->kind == OCERZ_OPK_REG &&
                           (o->reg & 15) == OCERZ_RSP) ||
                          (o->kind == OCERZ_OPK_MEM &&
                           ((o->base != OCERZ_REG_NONE &&
                             (o->base & 15) == OCERZ_RSP) ||
                            (o->index != OCERZ_REG_NONE &&
                             (o->index & 15) == OCERZ_RSP)));
                if (rsp && !(insn.op == OCERZ_OP_MOV && k == 1 &&
                             o->kind == OCERZ_OPK_REG)) {
                    rsp_ok = 0;
                    break;
                }
            }
            if (!rsp_ok)
                break;
            pc += insn.len;
        }
    }
    ocerz_jit_decode_recover = prev;
    return compatible;
}

void emit_pf(A64Buf *b, int res)
{
    a64_uxtb(b, JTT, res);
    a64_lsr_imm(b, 0, JTU, JTT, 4);
    a64_eor_reg(b, 0, JTT, JTT, JTU, 0);
    a64_lsr_imm(b, 0, JTU, JTT, 2);
    a64_eor_reg(b, 0, JTT, JTT, JTU, 0);
    a64_lsr_imm(b, 0, JTU, JTT, 1);
    a64_eor_reg(b, 0, JTT, JTT, JTU, 0);
    a64_mov_imm64(b, JTU, 1);
    a64_bic_reg(b, 0, JTT, JTU, JTT, 0);
    a64_lsl_imm(b, 0, JTT, JTT, 2);
    a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
}

void emit_materialize(A64Buf *b)
{

    if (!g_defer)
        return;
    a64_ldr(b, 4, JT0, 20, CC_OP_OFF);
    uint32_t *skip = a64_label(b);
    a64_cbz(b, 0, JT0, 0);
    emit_xmm_pin_spill_all(b);
    emit_spill_pinned_callersaved(b);
    for (int v = 4; v < 16; v += 2)
        if (g_lane_used & (3u << (v - 4)))
            a64_stp_q_pre(b, v, v + 1, 31, -32);
    a64_mov_reg(b, 1, 0, 20);
    tc_imm64(b, 16, TCR_SYM, TCS_FLAGS_MATERIALIZE, (uint64_t)(uintptr_t)&ocerz_flags_materialize);
    a64_blr(b, 16);
    g_callout_seq++;
    for (int v = 14; v >= 4; v -= 2)
        if (g_lane_used & (3u << (v - 4)))
            a64_ldp_q_post(b, v, v + 1, 31, 32);
    emit_fill_pinned_callersaved(b);
    emit_reload_jgb(b);
    emit_reload_mem_base(b);
    emit_xmm_pin_load_all(b);
    a64_patch_cbz(skip, a64_label(b));
}

static void emit_cc_predicate_rflags(A64Buf *b, unsigned cc)
{
    a64_ldr(b, 8, JT0, 20, RF_OFF);
    a64_ubfx(b, 1, JT1, JT0, 0, 1);
    a64_ubfx(b, 1, JTA, JT0, 6, 1);
    a64_ubfx(b, 1, JTT, JT0, 7, 1);
    a64_ubfx(b, 1, JTU, JT0, 11, 1);
    switch (cc >> 1) {
    case 0: a64_mov_reg(b, 1, JTF, JTU); break;
    case 1: a64_mov_reg(b, 1, JTF, JT1); break;
    case 2: a64_mov_reg(b, 1, JTF, JTA); break;
    case 3: a64_orr_reg(b, 1, JTF, JT1, JTA, 0); break;
    case 4: a64_mov_reg(b, 1, JTF, JTT); break;
    case 5: a64_ubfx(b, 1, JTF, JT0, 2, 1); break;
    case 6: a64_eor_reg(b, 1, JTF, JTT, JTU, 0); break;
    default:
        a64_eor_reg(b, 1, JTF, JTT, JTU, 0);
        a64_orr_reg(b, 1, JTF, JTF, JTA, 0);
        break;
    }
    if (cc & 1) {
        a64_mov_imm64(b, JTU, 1);
        a64_eor_reg(b, 1, JTF, JTF, JTU, 0);
    }
}

static int cc_after_subs(unsigned cc)
{
    static const int t[16] = { A64_VS, A64_VC, A64_CC, A64_CS, A64_EQ, A64_NE, A64_LS, A64_HI,
                               A64_MI, A64_PL, -1, -1, A64_LT, A64_GE, A64_LE, A64_GT };
    return cc < 16 ? t[cc] : -1;
}

static int cc_after_ands(unsigned cc)
{
    switch (cc) {
    case OCERZ_CC_O: case OCERZ_CC_B: return A64_NV;
    case OCERZ_CC_NO: case OCERZ_CC_AE: return A64_AL;
    case OCERZ_CC_E: return A64_EQ;  case OCERZ_CC_NE: return A64_NE;
    case OCERZ_CC_BE: return A64_EQ; case OCERZ_CC_A: return A64_NE;
    case OCERZ_CC_S: return A64_MI;  case OCERZ_CC_NS: return A64_PL;
    case OCERZ_CC_L: return A64_MI;  case OCERZ_CC_GE: return A64_PL;
    case OCERZ_CC_LE: return A64_LE; case OCERZ_CC_G: return A64_GT;
    default: return -1;
    }
}

static unsigned producer_record_kind(const X86Insn *p, int *size)
{
    if (!p) return 0;
    switch (p->op) {
    case OCERZ_OP_CMP: case OCERZ_OP_SUB: *size = p->ops[0].size; return OCERZ_CC_SUB;
    case OCERZ_OP_TEST: case OCERZ_OP_AND: case OCERZ_OP_OR: case OCERZ_OP_XOR:
        *size = p->ops[0].size; return OCERZ_CC_LOGIC;
    case OCERZ_OP_ADD: *size = p->ops[0].size; return OCERZ_CC_ADD;
    case OCERZ_OP_SHL: case OCERZ_OP_SHR: case OCERZ_OP_SAR:
        if (p->ops[1].kind == OCERZ_OPK_IMM && p->ops[0].kind == OCERZ_OPK_REG &&
            (p->ops[0].size == 4 || p->ops[0].size == 8)) {
            *size = p->ops[0].size;
            return p->op == OCERZ_OP_SHL ? OCERZ_CC_SHL : p->op == OCERZ_OP_SHR ? OCERZ_CC_SHR : OCERZ_CC_SAR;
        }
        return 0;
    default: return 0;
    }
}

static int cc_consumer_inline_ok(const X86Insn *c)
{
    const X86Operand *d = &c->ops[0];
    if (c->mode32 && c->op != OCERZ_OP_JCC && !m32_inline_ok(c))
        return 0;
    switch (c->op) {
    case OCERZ_OP_JCC:
        return 1;
    case OCERZ_OP_SETCC: {
        static int no = -1; if (no < 0) no = getenv("OCERZ_NO_INLINE_SETCC") ? 1 : 0;
        if (no) return 0;
        return d->kind == OCERZ_OPK_REG && !d->high8 && d->size == 1 &&
               !(rsp_is_ptr() && d->reg == OCERZ_RSP);
    }
    case OCERZ_OP_CMOVCC: {
        static int no = -1; if (no < 0) no = getenv("OCERZ_NO_INLINE_CMOV") ? 1 : 0;
        if (no) return 0;
        const X86Operand *sr = &c->ops[1];
        if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
        if (rsp_is_ptr() && (d->reg == OCERZ_RSP || (sr->kind == OCERZ_OPK_REG && sr->reg == OCERZ_RSP))) return 0;
        if (sr->kind == OCERZ_OPK_REG) return !sr->high8 && sr->size == d->size;
        return sr->kind == OCERZ_OPK_MEM && (c->addrsize == 8 || c->mode32);
    }
    case OCERZ_OP_ADC: case OCERZ_OP_SBB: {
        const X86Operand *sr = &c->ops[1];
        if (!g_defer) return 0;
        if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
        if (rsp_is_ptr() && d->reg == OCERZ_RSP) return 0;
        if (sr->kind == OCERZ_OPK_REG) return !sr->high8 && sr->size == d->size;
        return sr->kind == OCERZ_OPK_IMM;
    }
    default:
        return 0;
    }
}

int comis_fuse_producer(const X86Insn *insns, int ci)
{
    if (!cc_consumer_inline_ok(&insns[ci])) return -1;
    unsigned cc = insns[ci].cc;
    if (!(cc == OCERZ_CC_A || cc == OCERZ_CC_AE || cc == OCERZ_CC_B || cc == OCERZ_CC_BE ||
          cc == OCERZ_CC_P || cc == OCERZ_CC_NP || cc == OCERZ_CC_E || cc == OCERZ_CC_NE))
        return -1;
    int pi = -1;
    for (int k = ci - 1; k >= 0; k--) {
        uint64_t def, use;
        ocerz_flags_defuse(&insns[k], &def, &use);
        if (def & JIT_ARITH_FLAGS) { pi = k; break; }
    }
    if (pi < 0) return -1;
    const X86Insn *p = &insns[pi];
    if (!(p->op == OCERZ_OP_UCOMISD || p->op == OCERZ_OP_UCOMISS ||
          p->op == OCERZ_OP_COMISD || p->op == OCERZ_OP_COMISS)) return -1;
    if (p->ops[0].kind != OCERZ_OPK_XMM || !xmm_is_pinned(p->ops[0].reg)) return -1;
    int smem = p->ops[1].kind == OCERZ_OPK_MEM && !ENV_ON("OCERZ_NO_COMIS_MEM_FUSE");
    if (!smem && (p->ops[1].kind != OCERZ_OPK_XMM || !xmm_is_pinned(p->ops[1].reg))) return -1;
    for (int k = pi + 1; k < ci; k++) {
        const X86Insn *m = &insns[k];
        if (m->nops > 0 && m->ops[0].kind == OCERZ_OPK_XMM &&
            (m->ops[0].reg == p->ops[0].reg || (!smem && m->ops[0].reg == p->ops[1].reg)))
            return -1;
        uint64_t mdef, muse;
        ocerz_flags_defuse_nofault(m, &mdef, &muse);
        if (!(mdef & JIT_ARITH_FLAGS) && (muse & JIT_ARITH_FLAGS) == JIT_ARITH_FLAGS)
            return -1;
    }
    return pi;
}

int value_cond_fuse_producer(const X86Insn *insns, int ci)
{
    static int dis = -1;
    if (dis < 0) dis = getenv("OCERZ_NO_VALCC") ? 1 : 0;
    if (dis) return -1;
    if (!cc_consumer_inline_ok(&insns[ci])) return -1;
    unsigned cc = insns[ci].cc;
    if (!(cc == OCERZ_CC_E || cc == OCERZ_CC_NE || cc == OCERZ_CC_S || cc == OCERZ_CC_NS))
        return -1;
    int pi = -1;
    for (int k = ci - 1; k >= 0; k--) {
        uint64_t def, use;
        ocerz_flags_defuse(&insns[k], &def, &use);
        if (def & JIT_ARITH_FLAGS) { pi = k; break; }
    }
    if (pi < 0) return -1;
    const X86Insn *p = &insns[pi];
    static int only = -2;
    if (only == -2) { const char *e = getenv("OCERZ_VALCC_ONLY"); only = e ? atoi(e) : -1; }
    if (only >= 0 && (int)p->op != only) return -1;
    switch (p->op) {
    case OCERZ_OP_ADD: case OCERZ_OP_SUB: case OCERZ_OP_AND: case OCERZ_OP_OR: case OCERZ_OP_XOR:
    case OCERZ_OP_INC: case OCERZ_OP_DEC: case OCERZ_OP_NEG:
        break;
    case OCERZ_OP_SHL: case OCERZ_OP_SHR: case OCERZ_OP_SAR:
        if (p->nops < 2 || p->ops[1].kind != OCERZ_OPK_IMM ||
            (p->ops[1].imm & (p->ops[0].size == 8 ? 63u : 31u)) == 0) return -1;
        break;
    default: return -1;
    }
    const X86Operand *d = &p->ops[0];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return -1;
    if (pin_slot(d->reg) < 0) return -1;
    if (rsp_is_ptr() && d->reg == OCERZ_RSP) return -1;
    for (int k = pi + 1; k < ci; k++) {
        if (insn_may_write_gpr(&insns[k], d->reg)) return -1;
        uint64_t mdef, muse;
        ocerz_flags_defuse_nofault(&insns[k], &mdef, &muse);
        if (!(mdef & JIT_ARITH_FLAGS) && (muse & JIT_ARITH_FLAGS) == JIT_ARITH_FLAGS)
            return -1;
    }
    return pi;
}

static int mem_plain_ok(const X86Insn *insn, const X86Operand *op)
{
    if (!mem_fast_forms_ok()) return 0;
    if (insn->seg != OCERZ_SEG_NONE || insn->addrsize != 8) return 0;
    if (rsp_is_ptr() && (op->base == OCERZ_RSP || op->index == OCERZ_RSP)) return 0;
    if (op->riprel) return 1;
    if (op->base != OCERZ_REG_NONE && pin_slot(op->base) < 0) return 0;
    if (op->index != OCERZ_REG_NONE && pin_slot(op->index) < 0) return 0;
    return 1;
}

static int rmw_nzcv_ok(const X86Insn *p, const X86Operand *m)
{
    if (ENV_ON("OCERZ_NO_INLINE_RMW") || ENV_ON("OCERZ_NO_NZCV_MEMDST")) return 0;
    if (p->seg != OCERZ_SEG_NONE || p->addrsize != 8 || p->lock || !mem_native_store_ok()) return 0;
    if (rsp_is_ptr() && m->index == OCERZ_RSP) return 0;
    return 1;
}

static int nzcv_gap_max(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("OCERZ_NZCV_GAP"); v = e ? atoi(e) : NZCV_GAP_MAX; if (v < 0) v = 0; if (v > NZCV_GAP_MAX) v = NZCV_GAP_MAX; }
    return v;
}

static int nzcv_producer_candidate(const X86Insn *p)
{
    switch (p->op) {
    case OCERZ_OP_CMP: case OCERZ_OP_SUB: case OCERZ_OP_ADD:
    case OCERZ_OP_TEST: case OCERZ_OP_AND: case OCERZ_OP_OR: case OCERZ_OP_XOR:
    case OCERZ_OP_BSF: case OCERZ_OP_BSR:
    case OCERZ_OP_BT: case OCERZ_OP_BTS: case OCERZ_OP_BTR: case OCERZ_OP_BTC:
        return 1;
    default:
        return 0;
    }
}

static int nzcv_gap_shape(const X86Insn *in)
{
    if (nzcv_gap_max() == 0) return 0;
    if (in->op == OCERZ_OP_CMOVCC) {
        const X86Operand *d = &in->ops[0], *sr = &in->ops[1];
        return d->kind == OCERZ_OPK_REG && !d->high8 && (d->size == 4 || d->size == 8) &&
               sr->kind == OCERZ_OPK_REG && !sr->high8 && sr->size == d->size &&
               pin_slot(d->reg) >= 0 && pin_slot(sr->reg) >= 0 && g_pin_class == 3;
    }
    if (in->op == OCERZ_OP_SETCC) {
        const X86Operand *d = &in->ops[0];
        return d->kind == OCERZ_OPK_REG && !d->high8 && d->size == 1 && g_pin_class == 3;
    }
    return flag_neutral_ok(in);
}

static int nzcv_gap_ok(const X86Insn *insns, int m, int k)
{
    const X86Insn *in = &insns[m];
    if (in->op == OCERZ_OP_CMOVCC || in->op == OCERZ_OP_SETCC) {
        if (!nzcv_gap_shape(in)) return 0;
        if (nzcv_fuse_producer(insns, m) != k) return 0;
        const X86Insn *p = &insns[k];
        unsigned kind = (p->op == OCERZ_OP_CMP || p->op == OCERZ_OP_SUB) ? OCERZ_CC_SUB :
                        p->op == OCERZ_OP_ADD ? OCERZ_CC_ADD :
                        (p->op == OCERZ_OP_BSF || p->op == OCERZ_OP_BSR) ? OCERZ_CC_SUB :
                        (p->op == OCERZ_OP_BT || p->op == OCERZ_OP_BTS || p->op == OCERZ_OP_BTR || p->op == OCERZ_OP_BTC) ? NZCV_KIND_BT :
                        OCERZ_CC_LOGIC;
        int dc = nzcv_dc_for(kind, in->cc);
        return dc >= 0 && dc != A64_AL && dc != A64_NV;
    }
    return flag_neutral_ok(in);
}

int nzcv_fuse_producer(const X86Insn *insns, int ci)
{
    static int dis = -1;
    if (dis < 0) dis = getenv("OCERZ_NO_NZCVFWD") ? 1 : 0;
    if (dis || ci < 1 || !g_defer || g_no_regflags) return -1;
    const X86Insn *c = &insns[ci];
    if (!cc_consumer_inline_ok(c)) return -1;
    unsigned cc;
    if (c->op == OCERZ_OP_SETCC || c->op == OCERZ_OP_CMOVCC) cc = c->cc;
    else if (c->op == OCERZ_OP_ADC || c->op == OCERZ_OP_SBB) cc = OCERZ_CC_B;
    else if (c->op == OCERZ_OP_JCC) cc = c->cc;
    else return -1;
    int k = ci - 1;
    while (k >= 0 && ci - 1 - k < NZCV_GAP_MAX && !nzcv_producer_candidate(&insns[k]) && nzcv_gap_shape(&insns[k]))
        k--;
    if (k < 0 || !nzcv_producer_candidate(&insns[k])) return -1;
    for (int m = k + 1; m < ci; m++) {
        if (!nzcv_gap_ok(insns, m, k)) return -1;
        const X86Insn *p = &insns[k];






        if ((c->op == OCERZ_OP_SETCC || c->op == OCERZ_OP_CMOVCC) &&
            (insns[m].op == OCERZ_OP_SETCC || insns[m].op == OCERZ_OP_CMOVCC) && !ENV_ON("OCERZ_NO_NZCV_SIBLING"))
            continue;
        for (int o = 0; o < p->nops; o++) {
            const X86Operand *po = &p->ops[o];
            if (po->kind == OCERZ_OPK_REG && insn_writes_reg(&insns[m], po->reg)) return -1;
            if (po->kind == OCERZ_OPK_MEM) {
                if (po->base != OCERZ_REG_NONE && insn_writes_reg(&insns[m], po->base)) return -1;
                if (po->index != OCERZ_REG_NONE && insn_writes_reg(&insns[m], po->index)) return -1;
            }
        }
    }
    const X86Insn *p = &insns[k];
    unsigned kind;
    if (p->op == OCERZ_OP_BSF || p->op == OCERZ_OP_BSR) {
        if (cc != OCERZ_CC_E && cc != OCERZ_CC_NE) return -1;
        if (p->nops != 2 || p->seg != OCERZ_SEG_NONE) return -1;
        const X86Operand *bd = &p->ops[0], *bs = &p->ops[1];
        if (bd->kind != OCERZ_OPK_REG || bd->high8 || (bd->size != 4 && bd->size != 8) || pin_slot(bd->reg) < 0) return -1;
        if (bs->kind == OCERZ_OPK_REG) { if (bs->high8 || pin_slot(bs->reg) < 0 || bs->size != bd->size) return -1; }
        else if (bs->kind == OCERZ_OPK_MEM) { if (!mem_plain_ok(p, bs) || bs->size != bd->size) return -1; }
        else return -1;
        if (g_pin_class == 2) return -1;
        return k;
    }
    if (p->op == OCERZ_OP_BT || p->op == OCERZ_OP_BTS || p->op == OCERZ_OP_BTR || p->op == OCERZ_OP_BTC) {
        if (cc != OCERZ_CC_B && cc != OCERZ_CC_AE) return -1;
        if (p->nops != 2 || p->seg != OCERZ_SEG_NONE || p->addrsize != 8) return -1;
        const X86Operand *bd = &p->ops[0], *bo = &p->ops[1];
        if (bd->size != 2 && bd->size != 4 && bd->size != 8) return -1;
        if (bd->kind == OCERZ_OPK_REG) { if (bd->high8 || pin_slot(bd->reg) < 0 || (rsp_is_ptr() && bd->reg == OCERZ_RSP)) return -1; }
        else if (bd->kind != OCERZ_OPK_MEM || p->op != OCERZ_OP_BT) return -1;
        if (bo->kind == OCERZ_OPK_REG) { if (bo->high8 || pin_slot(bo->reg) < 0 || (rsp_is_ptr() && bo->reg == OCERZ_RSP)) return -1; }
        else if (bo->kind != OCERZ_OPK_IMM) return -1;
        return k;
    }
    switch (p->op) {
    case OCERZ_OP_CMP: case OCERZ_OP_SUB: kind = OCERZ_CC_SUB; if (cc_after_subs(cc) < 0) return -1; break;
    case OCERZ_OP_ADD:                    kind = OCERZ_CC_ADD; if (cc_after_adds(cc) < 0) return -1; break;
    case OCERZ_OP_TEST: case OCERZ_OP_AND: case OCERZ_OP_OR: case OCERZ_OP_XOR:
                                          kind = OCERZ_CC_LOGIC; if (cc_after_ands(cc) < 0) return -1; break;
    default: return -1;
    }
    (void)kind;
    if (p->nops != 2 || p->seg != OCERZ_SEG_NONE) return -1;
    const X86Operand *d = &p->ops[0], *sr = &p->ops[1];
    if (d->size == 1 || d->size == 2) {
        if (sr->size != d->size || sr->high8) return -1;
        if (d->kind == OCERZ_OPK_REG) {
            if (d->high8 || pin_slot(d->reg) < 0 || (rsp_is_ptr() && d->reg == OCERZ_RSP)) return -1;
        } else if (d->kind == OCERZ_OPK_MEM) {
            if (p->op != OCERZ_OP_CMP || !mem_plain_ok(p, d) || sr->kind == OCERZ_OPK_MEM) return -1;
        } else return -1;
        if (p->op == OCERZ_OP_TEST) {
            if (sr->kind != OCERZ_OPK_IMM) return -1;
            if (cc != OCERZ_CC_E && cc != OCERZ_CC_NE) return -1;
        } else if (p->op == OCERZ_OP_CMP) {
            if (sr->kind == OCERZ_OPK_REG) { if (pin_slot(sr->reg) < 0 || (rsp_is_ptr() && sr->reg == OCERZ_RSP)) return -1; }
            else if (sr->kind == OCERZ_OPK_MEM) { if (!mem_plain_ok(p, sr)) return -1; }
            else if (sr->kind != OCERZ_OPK_IMM) return -1;
            if (cc != OCERZ_CC_E && cc != OCERZ_CC_NE && cc != OCERZ_CC_B && cc != OCERZ_CC_AE &&
                cc != OCERZ_CC_A && cc != OCERZ_CC_BE) return -1;
        } else return -1;
        return k;
    }
    if (d->kind == OCERZ_OPK_MEM && (p->op == OCERZ_OP_CMP || p->op == OCERZ_OP_TEST) &&
        (c->op == OCERZ_OP_SETCC || c->op == OCERZ_OP_CMOVCC || c->op == OCERZ_OP_ADC || c->op == OCERZ_OP_SBB) &&
        (d->size == 4 || d->size == 8) && sr->size == d->size) {
        if (!rmw_nzcv_ok(p, d)) return -1;
        if (sr->kind == OCERZ_OPK_REG) {
            if (sr->high8 || pin_slot(sr->reg) < 0 || (rsp_is_ptr() && sr->reg == OCERZ_RSP)) return -1;
        } else if (sr->kind != OCERZ_OPK_IMM) return -1;
        return k;
    }
    if (d->kind != OCERZ_OPK_REG || d->high8) return -1;
    if (pin_slot(d->reg) < 0 || (rsp_is_ptr() && d->reg == OCERZ_RSP)) return -1;
    if (d->size != 4 && d->size != 8) return -1;
    if (sr->size != d->size) return -1;
    if (sr->kind == OCERZ_OPK_REG) {
        if (sr->high8 || pin_slot(sr->reg) < 0 || (rsp_is_ptr() && sr->reg == OCERZ_RSP)) return -1;
    } else if (sr->kind == OCERZ_OPK_MEM) {
        if (!mem_plain_ok(p, sr)) return -1;
    } else if (sr->kind != OCERZ_OPK_IMM) return -1;
    return k;
}

static int cc_after_adds(unsigned cc)
{
    static const int t[16] = { A64_VS, A64_VC, A64_CS, A64_CC, A64_EQ, A64_NE, -1, -1,
                               A64_MI, A64_PL, -1, -1, A64_LT, A64_GE, A64_LE, A64_GT };
    return cc < 16 ? t[cc] : -1;
}

int g_cc_want_cbz;

int g_cc_cbz_nz;

int g_cc_cbz_reg = -1;

int g_cc_cbz_sf;

static int nzcv_dc_for(unsigned kind, unsigned cc)
{
    return kind == OCERZ_CC_SUB ? cc_after_subs(cc) :
           kind == OCERZ_CC_ADD ? cc_after_adds(cc) :
           kind == NZCV_KIND_BT ? (cc == OCERZ_CC_B ? A64_NE : cc == OCERZ_CC_AE ? A64_EQ : -1) :
           cc_after_ands(cc);
}

void emit_cc_predicate_ex(A64Buf *b, unsigned cc, int want_direct)
{
    g_cc_direct = -1;
    g_cc_cbz_reg = -1;
    if (ENV_ON("OCERZ_CCLOG")) {
        char tb[96] = "(none)";
        if (g_flag_producer) ocerz_format_insn(g_flag_producer, tb, sizeof tb);
        fprintf(stderr, "ocerz: CCPRED cc=%u producer=%s\n", cc, tb);
    }
    if (g_nzcv_from >= 0 && g_cur_insns && g_nzcv_from < g_cur_insn_idx &&
        nzcv_fuse_producer(g_cur_insns, g_cur_insn_idx) == g_nzcv_from) {
        const X86Insn *c = &g_cur_insns[g_cur_insn_idx];
        unsigned want_cc = (c->op == OCERZ_OP_ADC || c->op == OCERZ_OP_SBB) ? OCERZ_CC_B : c->cc;
        if (want_cc == cc) {
            int dc = g_nzcv_kind == OCERZ_CC_SUB ? cc_after_subs(cc) :
                     g_nzcv_kind == OCERZ_CC_ADD ? cc_after_adds(cc) :
                     g_nzcv_kind == NZCV_KIND_BT ? (cc == OCERZ_CC_B ? A64_NE : cc == OCERZ_CC_AE ? A64_EQ : -1) :
                     cc_after_ands(cc);
            if (dc == A64_AL || dc == A64_NV) {
                a64_mov_imm64(b, JTF, dc == A64_AL ? 1 : 0);
                a64_subs_imm(b, 1, A64_ZR, JTF, 0);
                return;
            }
            if (dc >= 0) {
                if (want_direct) { g_cc_direct = dc; return; }
                a64_cset(b, JTF, dc);
                a64_subs_imm(b, 1, A64_ZR, JTF, 0);
                return;
            }
        }
    }
    if (g_defer && !g_no_regflags && g_cur_insns && g_cur_insn_idx >= 0 &&
        (g_cur_insns[g_cur_insn_idx].op == OCERZ_OP_JCC || g_cur_insns[g_cur_insn_idx].op == OCERZ_OP_SETCC ||
         g_cur_insns[g_cur_insn_idx].op == OCERZ_OP_CMOVCC) &&
        g_cur_insns[g_cur_insn_idx].cc == cc) {
        int pi = value_cond_fuse_producer(g_cur_insns, g_cur_insn_idx);
        if (pi >= 0) {
            const X86Operand *d = &g_cur_insns[pi].ops[0];
            int rd = pin_hreg(pin_slot(d->reg));
            if (want_direct && g_cc_want_cbz && (cc == OCERZ_CC_E || cc == OCERZ_CC_NE)) {
                g_cc_cbz_reg = rd; g_cc_cbz_sf = d->size == 8; g_cc_cbz_nz = cc == OCERZ_CC_NE;
                g_cc_direct = cc == OCERZ_CC_E ? A64_EQ : A64_NE;
                return;
            }
            a64_subs_imm(b, d->size == 8, A64_ZR, rd, 0);
            int dc = cc == OCERZ_CC_E ? A64_EQ : cc == OCERZ_CC_NE ? A64_NE :
                     cc == OCERZ_CC_S ? A64_MI : A64_PL;
            if (want_direct) { g_cc_direct = dc; return; }
            a64_cset(b, JTF, dc);
            a64_subs_imm(b, 1, A64_ZR, JTF, 0);
            return;
        }
    }
    uint32_t *to_generic[8]; int ng = 0;
    uint32_t *done[4]; int nd = 0;
    int c_sub = cc_after_subs(cc), c_and = cc_after_ands(cc);
    int psize = 0;
    unsigned pkind = producer_record_kind(g_flag_producer, &psize);
    int c_add = cc_after_adds(cc);
    int shift_ok = (pkind == OCERZ_CC_SHL || pkind == OCERZ_CC_SHR || pkind == OCERZ_CC_SAR) &&
                   (cc == OCERZ_CC_E || cc == OCERZ_CC_NE || cc == OCERZ_CC_S || cc == OCERZ_CC_NS);
    if (g_defer && shift_ok && g_flag_producer) {
        unsigned cnt = (unsigned)(g_flag_producer->ops[1].imm & (psize == 8 ? 63u : 31u));
        int sf = psize == 8;
        a64_ldr(b, 4, JT0, 20, CC_OP_OFF);
        uint32_t *to_rf = a64_label(b); a64_cbz(b, 0, JT0, 0);
        a64_ldr(b, 8, JT1, 20, CC_SRC_OFF);
        if (pkind == OCERZ_CC_SHL) a64_lsl_imm(b, sf, JT1, JT1, (int)cnt);
        else if (pkind == OCERZ_CC_SHR) a64_lsr_imm(b, sf, JT1, JT1, (int)cnt);
        else a64_asr_imm(b, sf, JT1, JT1, (int)cnt);
        a64_ands_reg(b, sf, A64_ZR, JT1, JT1, 0);
        a64_cset(b, JTF, cc == OCERZ_CC_E ? A64_EQ : cc == OCERZ_CC_NE ? A64_NE :
                          cc == OCERZ_CC_S ? A64_MI : A64_PL);
        uint32_t *ready = a64_label(b); a64_b(b, 0);
        a64_patch_cbz(to_rf, a64_label(b));
        emit_cc_predicate_rflags(b, cc);
        a64_patch_b(ready, a64_label(b));
        a64_subs_imm(b, 1, A64_ZR, JTF, 0);
        return;
    }
    if (g_defer && pkind == OCERZ_CC_ADD && c_add >= 0 && (psize == 4 || psize == 8)) {
        a64_ldr(b, 4, JT0, 20, CC_OP_OFF);
        uint32_t *to_rf = a64_label(b); a64_cbz(b, 0, JT0, 0);
        a64_ldr(b, 8, JT1, 20, CC_SRC_OFF);
        a64_ldr(b, 8, JTA, 20, CC_DST_OFF);
        a64_adds_reg(b, psize == 8, A64_ZR, JT1, JTA, 0);
        a64_cset(b, JTF, c_add);
        uint32_t *ready = a64_label(b); a64_b(b, 0);
        a64_patch_cbz(to_rf, a64_label(b));
        emit_cc_predicate_rflags(b, cc);
        a64_patch_b(ready, a64_label(b));
        a64_subs_imm(b, 1, A64_ZR, JTF, 0);
        return;
    }
    if (g_cur_insns && g_cur_insn_idx >= 0 && sse_enabled() &&
        (g_cur_insns[g_cur_insn_idx].op == OCERZ_OP_JCC || g_cur_insns[g_cur_insn_idx].op == OCERZ_OP_SETCC ||
         g_cur_insns[g_cur_insn_idx].op == OCERZ_OP_CMOVCC) &&
        g_cur_insns[g_cur_insn_idx].cc == cc &&
        comis_fuse_producer(g_cur_insns, g_cur_insn_idx) >= 0) {
        g_flag_producer = &g_cur_insns[comis_fuse_producer(g_cur_insns, g_cur_insn_idx)];
        int dbl = g_flag_producer->op == OCERZ_OP_UCOMISD || g_flag_producer->op == OCERZ_OP_COMISD;
        {
            int va = l0_src(g_flag_producer->ops[0].reg, dbl), vb;
            if (g_flag_producer->ops[1].kind == OCERZ_OPK_MEM) {
                a64_ldr_v(b, dbl ? 8 : 4, 3, 20, FCMP_MEM_OFF);
                vb = 3;
            } else {
                vb = l0_src(g_flag_producer->ops[1].reg, dbl);
            }
            a64_fcmp(b, dbl, va, vb);
            int pidx = (int)(g_flag_producer - g_cur_insns);
            if (fpb_det_here(pidx)) fpb_site_emit(b, pidx, va, vb, dbl);
        }
        if (want_direct) {
            int dc = cc == OCERZ_CC_A ? A64_GT : cc == OCERZ_CC_AE ? A64_GE :
                     cc == OCERZ_CC_B ? A64_LT : cc == OCERZ_CC_BE ? A64_LE :
                     cc == OCERZ_CC_P ? A64_VS : cc == OCERZ_CC_NP ? A64_VC : -1;
            if (dc >= 0) { g_cc_direct = dc; return; }
        }
        switch (cc) {
        case OCERZ_CC_A:  a64_cset(b, JTF, A64_GT); break;
        case OCERZ_CC_AE: a64_cset(b, JTF, A64_GE); break;
        case OCERZ_CC_B:  a64_cset(b, JTF, A64_LT); break;
        case OCERZ_CC_BE: a64_cset(b, JTF, A64_LE); break;
        case OCERZ_CC_P:  a64_cset(b, JTF, A64_VS); break;
        case OCERZ_CC_NP: a64_cset(b, JTF, A64_VC); break;
        case OCERZ_CC_E:  a64_cset(b, JTF, A64_EQ); a64_cset(b, JTU, A64_VS); a64_orr_reg(b, 1, JTF, JTF, JTU, 0); break;
        default:          a64_cset(b, JTF, A64_NE); a64_cset(b, JTU, A64_VC); a64_and_reg(b, 1, JTF, JTF, JTU, 0); break;
        }
        a64_subs_imm(b, 1, A64_ZR, JTF, 0);
        return;
    }
    if (g_flag_producer && (g_flag_producer->op == OCERZ_OP_UCOMISD ||
                            g_flag_producer->op == OCERZ_OP_UCOMISS ||
                            g_flag_producer->op == OCERZ_OP_COMISD ||
                            g_flag_producer->op == OCERZ_OP_COMISS)) {
        emit_cc_predicate_rflags(b, cc);
        a64_subs_imm(b, 1, A64_ZR, JTF, 0);
        return;
    }
    if (g_defer && pkind && cc != OCERZ_CC_P && cc != OCERZ_CC_NP &&
        (psize == 1 || psize == 2 || psize == 4 || psize == 8) &&
        ((pkind == OCERZ_CC_SUB && c_sub >= 0) || (pkind == OCERZ_CC_LOGIC && c_and >= 0))) {
        a64_ldr(b, 4, JT0, 20, CC_OP_OFF);
        uint32_t *to_rf = a64_label(b); a64_cbz(b, 0, JT0, 0);
        a64_ldr(b, 8, JT1, 20, CC_SRC_OFF);
        a64_ldr(b, 8, JTA, 20, CC_DST_OFF);
        int sh = psize < 4 ? 32 - 8 * psize : 0;
        if (pkind == OCERZ_CC_SUB) {
            if (psize == 8) a64_subs_reg(b, 1, A64_ZR, JT1, JTA, 0);
            else if (psize == 4) a64_subs_reg(b, 0, A64_ZR, JT1, JTA, 0);
            else { a64_lsl_imm(b, 0, JT1, JT1, sh); a64_lsl_imm(b, 0, JTA, JTA, sh);
                   a64_subs_reg(b, 0, A64_ZR, JT1, JTA, 0); }
            a64_cset(b, JTF, c_sub);
        } else {
            if (psize == 8) a64_ands_reg(b, 1, A64_ZR, JTA, JTA, 0);
            else if (psize == 4) a64_ands_reg(b, 0, A64_ZR, JTA, JTA, 0);
            else { a64_lsl_imm(b, 0, JTA, JTA, sh); a64_ands_reg(b, 0, A64_ZR, JTA, JTA, 0); }
            if (c_and == A64_AL) a64_mov_imm64(b, JTF, 1);
            else if (c_and == A64_NV) a64_mov_imm64(b, JTF, 0);
            else a64_cset(b, JTF, c_and);
        }
        uint32_t *ready = a64_label(b); a64_b(b, 0);
        a64_patch_cbz(to_rf, a64_label(b));
        emit_cc_predicate_rflags(b, cc);
        a64_patch_b(ready, a64_label(b));
        a64_subs_imm(b, 1, A64_ZR, JTF, 0);
        return;
    }
    if (g_defer && c_sub >= 0 && c_and >= 0 && cc != OCERZ_CC_P && cc != OCERZ_CC_NP) {
        a64_ldr(b, 4, JT0, 20, CC_OP_OFF);
        to_generic[ng++] = a64_label(b); a64_cbz(b, 0, JT0, 0);
        a64_ldr(b, 8, JT1, 20, CC_SRC_OFF);
        a64_ldr(b, 8, JTA, 20, CC_DST_OFF);
        a64_ubfx(b, 0, JTT, JT0, 8, 8);
        a64_ubfx(b, 0, JTU, JT0, 0, 8);
        a64_ubfx(b, 0, JTF, JT0, 16, 1);
        to_generic[ng++] = a64_label(b); a64_cbnz(b, 0, JTF, 0);
        a64_subs_imm(b, 0, A64_ZR, JTU, OCERZ_CC_SUB);
        uint32_t *not_sub = a64_label(b); a64_bcond(b, A64_NE, 0);
        a64_subs_imm(b, 0, A64_ZR, JTT, 8);
        uint32_t *s8 = a64_label(b); a64_bcond(b, A64_EQ, 0);
        a64_subs_imm(b, 0, A64_ZR, JTT, 4);
        uint32_t *s4 = a64_label(b); a64_bcond(b, A64_EQ, 0);
        a64_mov_imm64(b, JTF, 32);
        a64_lsl_imm(b, 0, JTT, JTT, 3);
        a64_sub_reg(b, 0, JTF, JTF, JTT, 0);
        a64_lslv(b, 0, JT1, JT1, JTF);
        a64_lslv(b, 0, JTA, JTA, JTF);
        a64_subs_reg(b, 0, A64_ZR, JT1, JTA, 0);
        done[nd++] = a64_label(b); a64_b(b, 0);
        a64_patch_bcond(s4, a64_label(b));
        a64_subs_reg(b, 0, A64_ZR, JT1, JTA, 0);
        done[nd++] = a64_label(b); a64_b(b, 0);
        a64_patch_bcond(s8, a64_label(b));
        a64_subs_reg(b, 1, A64_ZR, JT1, JTA, 0);
        done[nd++] = a64_label(b); a64_b(b, 0);
        a64_patch_bcond(not_sub, a64_label(b));
        a64_subs_imm(b, 0, A64_ZR, JTU, OCERZ_CC_LOGIC);
        to_generic[ng++] = a64_label(b); a64_bcond(b, A64_NE, 0);
        a64_subs_imm(b, 0, A64_ZR, JTT, 8);
        uint32_t *l8 = a64_label(b); a64_bcond(b, A64_EQ, 0);
        a64_mov_imm64(b, JTF, 32);
        a64_lsl_imm(b, 0, JTT, JTT, 3);
        a64_sub_reg(b, 0, JTF, JTF, JTT, 0);
        a64_lslv(b, 0, JTA, JTA, JTF);
        a64_ands_reg(b, 0, A64_ZR, JTA, JTA, 0);
        if (c_and == A64_AL) a64_mov_imm64(b, JTF, 1);
        else if (c_and == A64_NV) a64_mov_imm64(b, JTF, 0);
        else a64_cset(b, JTF, c_and);
        uint32_t *lg_done = a64_label(b); a64_b(b, 0);
        a64_patch_bcond(l8, a64_label(b));
        a64_ands_reg(b, 1, A64_ZR, JTA, JTA, 0);
        if (c_and == A64_AL) a64_mov_imm64(b, JTF, 1);
        else if (c_and == A64_NV) a64_mov_imm64(b, JTF, 0);
        else a64_cset(b, JTF, c_and);
        a64_patch_b(lg_done, a64_label(b));
        uint32_t *lg_pred = a64_label(b); a64_b(b, 0);
        for (int i = 0; i < nd; i++) a64_patch_b(done[i], a64_label(b));
        a64_cset(b, JTF, c_sub);
        uint32_t *sub_pred = a64_label(b); a64_b(b, 0);
        for (int i = 0; i < ng; i++) {
            uint32_t w = *to_generic[i];
            if ((w & 0xff000010u) == 0x54000000u) a64_patch_bcond(to_generic[i], a64_label(b));
            else a64_patch_cbz(to_generic[i], a64_label(b));
        }
        emit_materialize(b);
        emit_cc_predicate_rflags(b, cc);
        a64_patch_b(lg_pred, a64_label(b));
        a64_patch_b(sub_pred, a64_label(b));
        a64_subs_imm(b, 1, A64_ZR, JTF, 0);
        return;
    }
    emit_materialize(b);
    emit_cc_predicate_rflags(b, cc);
    a64_subs_imm(b, 1, A64_ZR, JTF, 0);
}

JitBlock *g_tag_blk;

int g_tag_idx;

static int fused_jcc_cond(const X86Insn *producer, const X86Insn *jcc)
{

    if (producer->op == OCERZ_OP_TEST) {
        static const int test_cond[16] = {
            -1,     -1,     -1,     -1,
            A64_EQ, A64_NE, A64_EQ, A64_NE,
            A64_MI, A64_PL, -1,     -1,
            A64_LT, A64_GE, A64_LE, A64_GT,
        };
        if (jcc->cc != OCERZ_CC_E && jcc->cc != OCERZ_CC_NE && ENV_ON("OCERZ_NO_TEST_CC")) return -1;
        return jcc->cc < 16 ? test_cond[jcc->cc] : -1;
    }

    static const int cmp_cond[16] = {
        A64_VS, A64_VC, A64_CC, A64_CS,
        A64_EQ, A64_NE, A64_LS, A64_HI,
        A64_MI, A64_PL, -1,     -1,
        A64_LT, A64_GE, A64_LE, A64_GT,
    };
    return jcc->cc < 16 ? cmp_cond[jcc->cc] : -1;
}

static uint32_t *cond_short_site(uint32_t *to_taken, int taken_rec, int body_edge)
{
    return (taken_rec || !body_edge || g_l0_dirty) ? NULL : to_taken;
}

int can_fuse_cmp_test_jcc(const X86Insn *producer,
                                 const X86Insn *jcc, uint64_t block_rip)
{
    if (g_no_jccfuse || g_no_regflags || g_no_chain ||
        jcc->op != OCERZ_OP_JCC || jcc->ops[0].kind != OCERZ_OPK_IMM ||
        (jcc->ops[0].imm != block_rip && g_no_jcclink) ||
        fused_jcc_cond(producer, jcc) < 0)
        return 0;
    if (jcc->ops[0].imm == block_rip &&
        (g_no_xlive || xlive_succ_live(g_xlat_jit, block_rip) != 0))
        return 0;
    if (producer->op != OCERZ_OP_CMP && producer->op != OCERZ_OP_TEST)
        return 0;

    const X86Operand *d = &producer->ops[0];
    const X86Operand *s = &producer->ops[1];
    if (s->size != d->size)
        return 0;
    if (d->size != 4 && d->size != 8 && d->size != 1 && d->size != 2)
        return 0;
    int d_mem = d->kind == OCERZ_OPK_MEM, s_mem = s->kind == OCERZ_OPK_MEM;
    if (d_mem && s_mem)
        return 0;
    if ((d_mem || s_mem) && producer->seg != OCERZ_SEG_NONE)
        return 0;
    if (!d_mem && (d->kind != OCERZ_OPK_REG || d->high8))
        return 0;
    if (s->kind == OCERZ_OPK_REG)
        return !s->high8;
    return s->kind == OCERZ_OPK_IMM || s_mem;
}

int side_gap_fuse_ok(const X86Insn *insns, int i, int n)
{
    if (g_xlat_mode32)
        return 0;
    static int dis = -1;
    if (dis < 0) dis = getenv("OCERZ_NO_SIDEFUSE") ? 1 : 0;
    if (dis || i < 0 || i + 2 >= n - 1) return 0;
    const X86Insn *p = &insns[i], *j = &insns[i + 2];
    if (j->op != OCERZ_OP_JCC || (p->op != OCERZ_OP_CMP && p->op != OCERZ_OP_TEST)) return 0;
    if (!g_defer || g_no_jccfuse || j->ops[0].kind != OCERZ_OPK_IMM || j->ops[0].imm == g_self_rip) return 0;
    if (!can_fuse_cmp_test_jcc(p, j, g_self_rip)) return 0;
    if (p->addrsize != 8) return 0;
    if (!jcc_gap_ok(&insns[i + 1])) return 0;
    if (p->ops[0].kind == OCERZ_OPK_REG && insn_writes_reg(&insns[i + 1], p->ops[0].reg)) return 0;
    if (p->ops[1].kind == OCERZ_OPK_REG && insn_writes_reg(&insns[i + 1], p->ops[1].reg)) return 0;
    return 1;
}

int side_fuse_ok(const X86Insn *insns, int i, int n)
{
    if (g_xlat_mode32)
        return 0;
    static int dis = -1;
    if (dis < 0) dis = getenv("OCERZ_NO_SIDEFUSE") ? 1 : 0;
    if (dis || i + 1 >= n - 1) return 0;
    const X86Insn *p = &insns[i], *j = &insns[i + 1];
    if (j->op != OCERZ_OP_JCC || (p->op != OCERZ_OP_CMP && p->op != OCERZ_OP_TEST)) return 0;
    if (!g_defer || j->ops[0].kind != OCERZ_OPK_IMM || j->ops[0].imm == g_self_rip) return 0;
    if (!can_fuse_cmp_test_jcc(p, j, g_self_rip)) return 0;
    if (p->addrsize != 8) return 0;
    return 1;
}

static int insn_writes_reg(const X86Insn *in, unsigned reg)
{
    if (in->nops == 0) return 0;
    const X86Operand *d = &in->ops[0];
    return d->kind == OCERZ_OPK_REG && (d->reg & 15) == (reg & 15);
}

static int flag_neutral_ok(const X86Insn *in)
{
    if (g_pin_class != 3) return 0;
    if (in->op == OCERZ_OP_LEA) {
        const X86Operand *d = &in->ops[0], *m = &in->ops[1];
        if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
        if (m->kind != OCERZ_OPK_MEM || m->riprel || in->addrsize != 8 || in->seg != OCERZ_SEG_NONE) return 0;
        if (m->base == OCERZ_REG_NONE || pin_slot(m->base) < 0) return 0;
        int has_idx = m->index != OCERZ_REG_NONE;
        if (has_idx && pin_slot(m->index) < 0) return 0;
        if (rsp_is_ptr() && (d->reg == OCERZ_RSP || m->base == OCERZ_RSP || m->index == OCERZ_RSP)) return 0;
        if (pin_slot(d->reg) < 0) return 0;
        if (m->disp >= -4095 && m->disp <= 4095) return 1;
        return has_idx && m->disp == 0;
    }
    if (in->op == OCERZ_OP_MOV) {
        const X86Operand *d = &in->ops[0], *s = &in->ops[1];
        if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
        if (pin_slot(d->reg) < 0 || (rsp_is_ptr() && d->reg == OCERZ_RSP)) return 0;
        if (s->kind == OCERZ_OPK_REG)
            return !s->high8 && s->size == d->size && pin_slot(s->reg) >= 0 && !(rsp_is_ptr() && s->reg == OCERZ_RSP);
        return s->kind == OCERZ_OPK_IMM;
    }
    return 0;
}

static int stack_gap_load_ok(const X86Insn *in)
{
    if (in->op != OCERZ_OP_MOV || in->nops != 2 || ENV_ON("OCERZ_NO_STACK_GAP")) return 0;
    const X86Operand *d = &in->ops[0], *s = &in->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
    if (pin_slot(d->reg) < 0 || d->reg == OCERZ_RSP) return 0;
    if (s->kind != OCERZ_OPK_MEM || s->size != d->size || !mem_plain_access_ok(s)) return 0;
    return lowstack_disp_ok(in, s, d->size, 1);
}

static int jcc_gap_ok(const X86Insn *in) { return flag_neutral_ok(in) || stack_gap_load_ok(in); }

static int emit_flag_neutral(A64Buf *b, const X86Insn *in)
{
    switch (in->op) {
    case OCERZ_OP_LEA:
        return emit_lea(b, in);
    case OCERZ_OP_MOV: {
        const X86Operand *d = &in->ops[0], *s = &in->ops[1];
        if (stack_gap_load_ok(in)) {
            if (!lowstack_disp_ea(b, in, s, d->size, 1)) return 0;
            emit_gpr_ld_at(b, d->size, pin_hreg(pin_slot(d->reg)), JTA, (int32_t)s->disp, 1);
            return 1;
        }
        if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
        if (s->kind == OCERZ_OPK_REG) {
            if (s->high8 || s->size != d->size) return 0;
            int ds = pin_slot(d->reg), ss = pin_slot(s->reg);
            if (ds < 0 || ss < 0 || (rsp_is_ptr() && (d->reg == OCERZ_RSP || s->reg == OCERZ_RSP))) return 0;
            if (ds != ss || d->size == 4) a64_mov_reg(b, d->size == 8, pin_hreg(ds), pin_hreg(ss));
            return 1;
        }
        if (s->kind == OCERZ_OPK_IMM) {
            int ds = pin_slot(d->reg);
            if (ds < 0 || (rsp_is_ptr() && d->reg == OCERZ_RSP)) return 0;
            uint64_t v = s->imm; if (d->size == 4) v &= 0xffffffffull;
            a64_mov_imm64(b, pin_hreg(ds), v);
            return 1;
        }
        return 0;
    }
    default:
        return 0;
    }
}

int emit_cmp_test_jcc(A64Buf *b, const X86Insn *producer,
                             const X86Insn *jcc,
                             uint32_t **epilogue_sites, int *n_epi,
                             uint32_t **jcc_label,
                             uint32_t **exit_sites, int *n_exits,
                             const X86Insn *gap, uint32_t **gap_label)
{
    if (!can_fuse_cmp_test_jcc(producer, jcc, g_self_rip) || !g_defer)
        return 0;
    if (gap) {
        const X86Operand *pd = &producer->ops[0], *ps = &producer->ops[1];
        if (pd->kind == OCERZ_OPK_REG && insn_writes_reg(gap, pd->reg)) return 0;
        if (ps->kind == OCERZ_OPK_REG && insn_writes_reg(gap, ps->reg)) return 0;
        if (!jcc_gap_ok(gap)) return 0;
        {
            uint32_t tmpw[128];
            A64Buf tb = { tmpw, tmpw, tmpw + 128, 0, 0 };
            __typeof__(g_ea_cache) saved = g_ea_cache;
            int saved_lits = g_n_raslit;
            ea_cache_reset();
            int ok = emit_flag_neutral(&tb, gap) && !tb.overflow;
            g_ea_cache = saved;
            g_n_raslit = saved_lits;
            if (!ok) return 0;
        }
    }
    int pre_rec = gap && stack_gap_load_ok(gap);

    const X86Operand *d = &producer->ops[0];
    const X86Operand *s = &producer->ops[1];
    int sf = d->size == 8;
    uint64_t taken = jcc->ops[0].imm;
    uint64_t fall = jcc->rip + jcc->len;
    if (jcc->mode32)
        fall = (uint32_t)fall;
    int self_loop = taken == g_self_rip;
    int test_bit = -1, test_sf = 0;
    int test_rn = -1;
    uint64_t test_mask = 0;
    int cbz_rn = -1, cbz_sf = 0;
    int rec_imm_pending = 0; uint64_t rec_imm = 0;
    int cc_is_zero_test = jcc->cc == OCERZ_CC_E || jcc->cc == OCERZ_CC_NE;
    int record_src = JT2;
    int record_dst = JT2;
    uint32_t ccop;
    int d_mem = d->kind == OCERZ_OPK_MEM, s_mem = s->kind == OCERZ_OPK_MEM;
    int d_in_jt0 = 0, s_in_jt1 = 0;
    if (d_mem || s_mem) {
        const X86Operand *m = d_mem ? d : s;
        int into = d_mem ? JT0 : JT1;
        if (!emit_mem_load_plain(b, producer, m, d->size, into)) {
            if (!emit_mem_ea(b, producer, m, JTA)) return 0;
            uint32_t *skip = emit_commpage_guard(b, producer, JTA, exit_sites, n_exits);
            emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
            emit_guest_load_ordered(b, d->size, into, JTA, JTU);
            patch_guard_skip(skip, a64_label(b));
        }
        if (d_mem) d_in_jt0 = 1; else s_in_jt1 = 1;
    }
    int narrow_direct = (d->size == 1 || d->size == 2) && producer->op == OCERZ_OP_CMP &&
        (jcc->cc == OCERZ_CC_E || jcc->cc == OCERZ_CC_NE || jcc->cc == OCERZ_CC_B ||
         jcc->cc == OCERZ_CC_AE || jcc->cc == OCERZ_CC_A || jcc->cc == OCERZ_CC_BE) &&
        !d->high8 && (s->kind != OCERZ_OPK_REG || !s->high8);
    if (narrow_direct) {
        int size = d->size;
        uint64_t mask = size == 1 ? 0xffull : 0xffffull;
        int rn, rm;
        if (d_in_jt0) rn = JT0;
        else {
            int ds = pin_slot(d->reg);
            if (ds >= 0) { if (size == 1) a64_uxtb(b, JT0, pin_hreg(ds)); else a64_uxth(b, JT0, pin_hreg(ds)); }
            else { emit_gpr_rd(b, 1, JT0, d->reg); if (size == 1) a64_uxtb(b, JT0, JT0); else a64_uxth(b, JT0, JT0); }
            rn = JT0;
        }
        record_src = rn;
        if (s_in_jt1) { rm = JT1; a64_subs_reg(b, 0, A64_ZR, rn, rm, 0); }
        else if (s->kind == OCERZ_OPK_REG) {
            int ss = pin_slot(s->reg);
            if (ss >= 0) { if (size == 1) a64_uxtb(b, JT1, pin_hreg(ss)); else a64_uxth(b, JT1, pin_hreg(ss)); }
            else { emit_gpr_rd(b, 1, JT1, s->reg); if (size == 1) a64_uxtb(b, JT1, JT1); else a64_uxth(b, JT1, JT1); }
            rm = JT1; a64_subs_reg(b, 0, A64_ZR, rn, rm, 0);
        } else {
            uint64_t v = (uint64_t)s->imm & mask;
            if (v == 0 && cc_is_zero_test) {
                cbz_rn = rn; cbz_sf = 0; rm = A64_ZR;
            } else {
                a64_mov_imm64(b, JT1, v);
                rm = JT1;
                if (v <= 4095) a64_subs_imm(b, 0, A64_ZR, rn, (uint32_t)v);
                else a64_subs_reg(b, 0, A64_ZR, rn, rm, 0);
            }
        }
        record_dst = rm;
        ccop = ocerz_cc_pack(OCERZ_CC_SUB, size, 0);
    } else
    if ((d->size == 1 || d->size == 2) && producer->op == OCERZ_OP_TEST &&
        !d_mem && !d->high8 && s->kind == OCERZ_OPK_IMM &&
        (jcc->cc == OCERZ_CC_E || jcc->cc == OCERZ_CC_NE)) {
        uint64_t v = s->imm & (d->size == 1 ? 0xffull : 0xffffull);
        int ds = pin_slot(d->reg);
        int rn = ds >= 0 ? pin_hreg(ds) : JT0;
        if (ds < 0) emit_gpr_rd(b, 1, JT0, d->reg);
        if (v != 0 && (v & (v - 1)) == 0) {
            test_bit = __builtin_ctzll(v);
            test_rn = rn;
            test_mask = v;
            test_sf = 0;
        } else if (!a64_try_ands_imm(b, 0, JT2, rn, v)) {
            a64_mov_imm64(b, JT1, v);
            a64_ands_reg(b, 0, JT2, rn, JT1, 0);
        }
        record_src = JT2; record_dst = JT2;
        ccop = ocerz_cc_pack(OCERZ_CC_LOGIC, d->size, 0);
    } else if ((d->size == 1 || d->size == 2) && producer->op == OCERZ_OP_TEST &&
               !d_mem && !s_mem && s->kind == OCERZ_OPK_REG && !d->high8 && !s->high8 &&
               (jcc->cc == OCERZ_CC_E || jcc->cc == OCERZ_CC_NE)) {
        uint64_t mask = d->size == 1 ? 0xffull : 0xffffull;
        int ds = pin_slot(d->reg), ss = pin_slot(s->reg);
        int ra = ds >= 0 ? pin_hreg(ds) : JT0, rb = ss >= 0 ? pin_hreg(ss) : JT1;
        if (ds < 0) emit_gpr_rd(b, 1, JT0, d->reg);
        if (ss < 0 && s->reg != d->reg) emit_gpr_rd(b, 1, JT1, s->reg);
        if (d->reg == s->reg) a64_try_ands_imm(b, 0, JT2, ra, mask);
        else { a64_and_reg(b, 0, JT2, ra, rb, 0); a64_try_ands_imm(b, 0, JT2, JT2, mask); }
        record_src = JT2; record_dst = JT2;
        ccop = ocerz_cc_pack(OCERZ_CC_LOGIC, d->size, 0);
    } else if (d->size == 1 || d->size == 2) {
        int size = d->size, sh = 32 - 8 * size;
        uint64_t mask = size == 1 ? 0xffull : 0xffffull;
        if (!d_in_jt0) { emit_gpr_rd(b, 1, JT0, d->reg); if (size == 1) a64_uxtb(b, JT0, JT0); else a64_uxth(b, JT0, JT0); }
        if (s_in_jt1) {
        } else if (s->kind == OCERZ_OPK_REG) {
            emit_gpr_rd(b, 1, JT1, s->reg);
            if (size == 1) a64_uxtb(b, JT1, JT1); else a64_uxth(b, JT1, JT1);
        } else
            a64_mov_imm64(b, JT1, (uint64_t)s->imm & mask);
        a64_lsl_imm(b, 0, JTA, JT0, sh);
        a64_lsl_imm(b, 0, JTU, JT1, sh);
        if (producer->op == OCERZ_OP_CMP) {
            a64_subs_reg(b, 0, A64_ZR, JTA, JTU, 0);
            record_src = JT0; record_dst = JT1;
            ccop = ocerz_cc_pack(OCERZ_CC_SUB, size, 0);
        } else {
            a64_ands_reg(b, 0, JT2, JTA, JTU, 0);
            a64_lsr_imm(b, 0, JT2, JT2, sh);
            record_src = JT2; record_dst = JT2;
            ccop = ocerz_cc_pack(OCERZ_CC_LOGIC, size, 0);
        }
    } else if (producer->op == OCERZ_OP_CMP) {
        int ds = d_in_jt0 ? -1 : pin_slot(d->reg);
        record_src = ds >= 0 ? pin_hreg(ds) : JT0;
        if (ds < 0 && !d_in_jt0)
            emit_gpr_rd(b, sf, JT0, d->reg);
        if (s_in_jt1) {
            record_dst = JT1;
        } else if (s->kind == OCERZ_OPK_REG && !s->high8) {
            int ss = pin_slot(s->reg);
            record_dst = ss >= 0 ? pin_hreg(ss) : JT1;
            if (ss < 0)
                emit_gpr_rd(b, sf, JT1, s->reg);
        } else if (s->kind == OCERZ_OPK_IMM) {
            uint64_t v = s->imm;
            if (!sf)
                v &= 0xffffffffull;
            if (v == 0 && cc_is_zero_test) {
                cbz_rn = record_src; cbz_sf = sf; record_dst = A64_ZR;
            } else if (v <= 4095 || ((v & 0xfff) == 0 && (v >> 12) <= 4095)) {
                if (v <= 4095) a64_subs_imm(b, sf, A64_ZR, record_src, (uint32_t)v);
                else           a64_subs_imm_sh12(b, sf, A64_ZR, record_src, (uint32_t)(v >> 12));
                rec_imm_pending = 1; rec_imm = v;
                record_dst = JT1;
                ccop = ocerz_cc_pack(OCERZ_CC_SUB, d->size, 0);
                goto cmp_done;
            } else {
                a64_mov_imm64(b, JT1, v);
                record_dst = JT1;
            }
        } else {
            return 0;
        }
        if (cbz_rn < 0)
            a64_subs_reg(b, sf, A64_ZR, record_src, record_dst, 0);
        ccop = ocerz_cc_pack(OCERZ_CC_SUB, d->size, 0);
    cmp_done:;
    } else {
        int ds = d_in_jt0 ? -1 : pin_slot(d->reg);
        int rn = ds >= 0 ? pin_hreg(ds) : JT0;
        if (ds < 0 && !d_in_jt0)
            emit_gpr_rd(b, sf, JT0, d->reg);
        int emitted = 0;
        if (s_in_jt1) {
            a64_ands_reg(b, sf, JT2, rn, JT1, 0);
            emitted = 1;
        } else if (s->kind == OCERZ_OPK_IMM) {
            uint64_t v = s->imm;
            if (!sf)
                v &= 0xffffffffull;
            if (v != 0 && (v & (v - 1)) == 0 && cc_is_zero_test) {
                test_bit = __builtin_ctzll(v);
                test_rn = rn;
                test_mask = v;
                test_sf = sf;
                emitted = 1;
            } else {
                emitted = a64_try_ands_imm(b, sf, JT2, rn, v);
                if (!emitted) {
                    a64_mov_imm64(b, JT1, v);
                    a64_ands_reg(b, sf, JT2, rn, JT1, 0);
                    emitted = 1;
                }
            }
        } else if (s->kind == OCERZ_OPK_REG && !s->high8) {
            int ss = pin_slot(s->reg);
            int rm = ss >= 0 ? pin_hreg(ss) : JT1;
            if (ss < 0)
                emit_gpr_rd(b, sf, JT1, s->reg);
            a64_ands_reg(b, sf, JT2, rn, rm, 0);
            emitted = 1;
        }
        if (!emitted)
            return 0;
        ccop = ocerz_cc_pack(OCERZ_CC_LOGIC, d->size, 0);
    }
    if (pre_rec) {
        if (producer->op == OCERZ_OP_CMP) {
            if (rec_imm_pending) a64_mov_imm64(b, JT1, rec_imm);
            emit_defer_flags(b, ccop, record_src, record_dst);
        } else {
            if (test_bit >= 0) {
                a64_mov_imm64(b, JT2, test_mask);
                a64_and_reg(b, 1, JT2, test_rn, JT2, 0);
            }
            emit_defer_flags(b, ccop, JT2, JT2);
        }
    }
    if (gap) {
        uint32_t *gl = a64_label(b);
        int ok = emit_flag_neutral(b, gap);
        assert(ok && "flag_neutral_ok admitted an unhandled shape");
        (void)ok;
        if (gap_label) *gap_label = gl;
    }
    *jcc_label = a64_label(b);
    int taken_cond = fused_jcc_cond(producer, jcc);

    if (g_jcc_side_mode && !self_loop) {
        int taken_live = g_no_xlive || xlive_succ_live(g_xlat_jit, taken) != 0;
        int need_rec = !pre_rec && (g_jcc_side_need != 0 || taken_live);
        int rec_after = need_rec && !taken_live;
        static int nostub = -1; if (nostub < 0) nostub = getenv("OCERZ_NO_RECSTUB") ? 1 : 0;
        int rec_stub = !nostub && need_rec && taken_live && g_jcc_side_fall_need == 0 && producer->op == OCERZ_OP_CMP &&
                       g_n_side < SIDE_MAX && record_src >= 0 && record_dst >= 0 &&
                       (record_src < 9 || record_src > 15) && (record_dst < 9 || record_dst > 15 || (rec_imm_pending && record_dst == JT1));
        if (need_rec && !rec_after && !rec_stub) {
            if (producer->op == OCERZ_OP_CMP) {
                if (rec_imm_pending) a64_mov_imm64(b, JT1, rec_imm);
                emit_defer_flags(b, ccop, record_src, record_dst);
            } else {
                if (test_bit >= 0) {
                    a64_mov_imm64(b, JT2, test_mask);
                    a64_and_reg(b, 1, JT2, test_rn, JT2, 0);
                }
                emit_defer_flags(b, ccop, JT2, JT2);
            }
        }
        if (g_n_side >= SIDE_MAX) return 0;
        g_side[g_n_side].site = a64_label(b);
        g_side[g_n_side].taken = taken;
        g_side[g_n_side].idx = -1;
        g_side[g_n_side].stub = NULL;
        g_side[g_n_side].patch_b = NULL;
        g_side[g_n_side].rec = rec_stub;
        g_side[g_n_side].fpb = -1; g_side[g_n_side].fpb_chk = 0;
        g_side[g_n_side].l0_dirty = g_l0_dirty;
        g_side[g_n_side].yc_dirty = g_yc_dirty;
        for (int r = 0; r < 16; r++) { g_side[g_n_side].l0[r] = g_l0[r]; g_side[g_n_side].l0_dbl[r] = g_l0_dbl[r]; }
        g_side[g_n_side].jcc_rip = jcc->rip;
        g_side[g_n_side].ft_rip = jcc->rip + jcc->len;
        g_side[g_n_side].ft_site = NULL;
        g_side[g_n_side].probe = !need_rec && probe_wanted(jcc->rip, jcc->rip + jcc->len);
        if (rec_stub) {
            g_side[g_n_side].rec_ccop = ccop; g_side[g_n_side].rec_src = record_src; g_side[g_n_side].rec_dst = record_dst;
            g_side[g_n_side].rec_imm_pending = rec_imm_pending; g_side[g_n_side].rec_imm = rec_imm;
        }
        g_n_side++;
        if (test_bit >= 0) {
            if (jcc->cc == OCERZ_CC_E) a64_tbz(b, test_rn, test_bit, 0);
            else                       a64_tbnz(b, test_rn, test_bit, 0);
        } else if (cbz_rn >= 0) {
            if (jcc->cc == OCERZ_CC_E) a64_cbz(b, cbz_sf, cbz_rn, 0);
            else                       a64_cbnz(b, cbz_sf, cbz_rn, 0);
        } else {
            a64_bcond(b, taken_cond, 0);
        }
        if (g_side[g_n_side - 1].probe) {
            g_side[g_n_side - 1].ft_site = a64_label(b);
            a64_b(b, 0);
        }
        if (rec_after) {
            if (producer->op == OCERZ_OP_CMP) {
                if (rec_imm_pending) a64_mov_imm64(b, JT1, rec_imm);
                emit_defer_flags(b, ccop, record_src, record_dst);
            } else {
                if (test_bit >= 0) {
                    a64_mov_imm64(b, JT2, test_mask);
                    a64_and_reg(b, 1, JT2, test_rn, JT2, 0);
                }
                emit_defer_flags(b, ccop, JT2, JT2);
            }
        }
        return 1;
    }

    if (!self_loop) {
        int poll_fall = fall <= g_self_rip;
        int poll_taken = taken <= g_self_rip;
        int edge_class = body_edge_pin_class();
        int body_edge = edge_class >= 0;
        uint32_t *to_taken = a64_label(b);
        if (test_bit >= 0) {
            if (jcc->cc == OCERZ_CC_E)
                a64_tbz(b, test_rn, test_bit, 0);
            else
                a64_tbnz(b, test_rn, test_bit, 0);
        } else if (cbz_rn >= 0) {
            if (jcc->cc == OCERZ_CC_E) a64_cbz(b, cbz_sf, cbz_rn, 0);
            else                       a64_cbnz(b, cbz_sf, cbz_rn, 0);
        } else {
            a64_bcond(b, taken_cond, 0);
        }

        if (!pre_rec && (g_no_xlive || xlive_succ_live(g_xlat_jit, fall) != 0)) {
            if (producer->op == OCERZ_OP_CMP) {
                if (rec_imm_pending) a64_mov_imm64(b, JT1, rec_imm);
                emit_defer_flags(b, ccop, record_src, record_dst);
            }
            else {
                if (test_bit >= 0)
                    a64_mov_imm64(b, JT2,
                        jcc->cc == OCERZ_CC_E ? test_mask : 0);
                emit_defer_flags(b, ccop, JT2, JT2);
            }
        }
        uint32_t *pb_fall = emit_static_chain_tail(
            b, fall, poll_fall, body_edge, epilogue_sites, n_epi);

        uint32_t *taken_label = a64_label(b);
        if (test_bit >= 0)
            a64_patch_tbz(to_taken, taken_label);
        else if (cbz_rn >= 0)
            a64_patch_cbz(to_taken, taken_label);
        else
            a64_patch_bcond(to_taken, taken_label);
        int taken_rec = !pre_rec && (g_no_xlive || xlive_succ_live(g_xlat_jit, taken) != 0);
        if (taken_rec) {
            if (producer->op == OCERZ_OP_CMP) {
                if (rec_imm_pending) a64_mov_imm64(b, JT1, rec_imm);
                emit_defer_flags(b, ccop, record_src, record_dst);
            }
            else {
                if (test_bit >= 0)
                    a64_mov_imm64(b, JT2,
                        jcc->cc == OCERZ_CC_E ? 0 : test_mask);
                emit_defer_flags(b, ccop, JT2, JT2);
            }
        }
        uint32_t *pb_taken = emit_static_chain_tail(
            b, taken, poll_taken, body_edge, epilogue_sites, n_epi);

        g_jcc_edge[0].target_rip = fall;
        g_jcc_edge[0].patch_b = pb_fall;
        g_jcc_edge[0].cond_site = NULL;
        g_jcc_edge[0].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
        g_jcc_edge[0].pin_class = body_edge ? (uint8_t)edge_class : 0;
        g_jcc_edge[1].target_rip = taken;
        g_jcc_edge[1].patch_b = pb_taken;
        g_jcc_edge[1].cond_site = cond_short_site(to_taken, taken_rec, body_edge);
        g_jcc_edge[1].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
        g_jcc_edge[1].pin_class = body_edge ? (uint8_t)edge_class : 0;
        g_n_jcc_edges = 2;
        return 1;
    }

    if (!g_loop_entry)
        return 0;

    l0_fixed_backedge(b);
    int tb_ok = 0;
    if (test_bit >= 0) {
        ptrdiff_t reach = g_loop_entry - a64_label(b);
        tb_ok = reach >= -(ptrdiff_t)(1 << 13) && reach < (ptrdiff_t)(1 << 13);
        if (!tb_ok && !a64_try_ands_imm(b, test_sf, JT2, test_rn, test_mask)) {
            a64_mov_imm64(b, JT1, test_mask);
            a64_ands_reg(b, test_sf, JT2, test_rn, JT1, 0);
        }
    }
    g_stop_patch = a64_label(b);
    if (test_bit >= 0 && tb_ok) {
        if (jcc->cc == OCERZ_CC_E) a64_tbz(b, test_rn, test_bit, (int32_t)(g_loop_entry - g_stop_patch));
        else                       a64_tbnz(b, test_rn, test_bit, (int32_t)(g_loop_entry - g_stop_patch));
    } else if (cbz_rn >= 0) {
        if (jcc->cc == OCERZ_CC_E) a64_cbz(b, cbz_sf, cbz_rn, (int32_t)(g_loop_entry - g_stop_patch));
        else                       a64_cbnz(b, cbz_sf, cbz_rn, (int32_t)(g_loop_entry - g_stop_patch));
    } else
        a64_bcond(b, taken_cond, (int32_t)(g_loop_entry - g_stop_patch));
    l0_fixed_fallthrough(b);
    fpb_emit_exit_check(b);

    if (!pre_rec && (g_no_xlive || xlive_succ_live(g_xlat_jit, fall) != 0)) {
        if (producer->op == OCERZ_OP_CMP) {
            if (rec_imm_pending) a64_mov_imm64(b, JT1, rec_imm);
            emit_defer_flags(b, ccop, record_src, record_dst);
        }
        else {
            if (test_bit >= 0) a64_mov_imm64(b, JT2, jcc->cc == OCERZ_CC_E ? test_mask : 0);
            emit_defer_flags(b, ccop, JT2, JT2);
        }
    }
    {
        int edge_class = body_edge_pin_class();
        int body_edge = edge_class >= 0;
        uint32_t *pb_fall = emit_static_chain_tail(
            b, fall, fall <= g_self_rip, body_edge, epilogue_sites, n_epi);
        g_jcc_edge[0].target_rip = fall;
        g_jcc_edge[0].patch_b = pb_fall;
        g_jcc_edge[0].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
        g_jcc_edge[0].pin_class = body_edge ? (uint8_t)edge_class : 0;
        g_n_jcc_edges = 1;
    }

    g_stop_target = a64_label(b);
    if (!pre_rec && (g_no_xlive || xlive_succ_live(g_xlat_jit, taken) != 0)) {
        if (producer->op == OCERZ_OP_CMP) {
            if (rec_imm_pending) a64_mov_imm64(b, JT1, rec_imm);
            emit_defer_flags(b, ccop, record_src, record_dst);
        }
        else {
            if (test_bit >= 0) a64_mov_imm64(b, JT2, jcc->cc == OCERZ_CC_E ? 0 : test_mask);
            emit_defer_flags(b, ccop, JT2, JT2);
        }
    }
    a64_mov_imm64(b, JT0, taken);
    a64_str(b, 8, JT0, 20, RIP_OFF);
    a64_mov_imm64(b, 0, OCERZ_STEP_OK);
    epilogue_sites[*n_epi] = a64_label(b);
    a64_b(b, 0);
    (*n_epi)++;
    return 1;
}

static uint32_t *emit_incdec_jcc_arm(A64Buf *b, const X86Insn *producer,
                                     uint64_t target, int poll, int body_edge,
                                     int cf_reg,
                                     uint32_t **epilogue_sites, int *n_epi,
                                     int *recorded)
{
    uint64_t live = g_no_xlive ? (uint64_t)OCERZ_FL_ALL
                               : xlive_succ_live(g_xlat_jit, target);
    if (recorded) *recorded = live != 0;
    if (live != 0) {
        const X86Operand *d = &producer->ops[0];
        if (cf_reg < 0) {
            emit_materialize(b);
            a64_ldr(b, 8, JTT, 20, RF_OFF);
            a64_ubfx(b, 1, JT0, JTT, 0, 1);
            cf_reg = JT0;
        }
        emit_gpr_rd(b, 1, JT1, d->reg);
        emit_defer_flags(b,
            ocerz_cc_pack(producer->op == OCERZ_OP_INC ? OCERZ_CC_INC
                                                        : OCERZ_CC_DEC,
                          d->size, 0),
            cf_reg, JT1);
    }
    return emit_static_chain_tail(b, target, poll, body_edge,
                                  epilogue_sites, n_epi);
}

int emit_incdec_jcc(A64Buf *b, const X86Insn *producer,
                           const X86Insn *jcc, uint32_t **epilogue_sites,
                           int *n_epi, uint32_t **jcc_label)
{
    if (!can_fuse_incdec_jcc(producer, jcc) || !g_defer)
        return 0;
    const X86Operand *d = &producer->ops[0];
    int sf = d->size == 8;
    int ds = pin_slot(d->reg);
    int rd = ds >= 0 ? pin_hreg(ds) : JT2;
    if (ds >= 0) {
        if (producer->op == OCERZ_OP_INC)
            a64_adds_imm(b, sf, rd, rd, 1);
        else
            a64_subs_imm(b, sf, rd, rd, 1);
    } else {
        emit_gpr_rd(b, sf, JT0, d->reg);
        if (producer->op == OCERZ_OP_INC)
            a64_adds_imm(b, sf, JT2, JT0, 1);
        else
            a64_subs_imm(b, sf, JT2, JT0, 1);
        emit_gpr_wr(b, JT2, d->reg);
    }

    *jcc_label = a64_label(b);
    int taken_cond = jcc->cc == OCERZ_CC_E ? A64_EQ : A64_NE;
    uint64_t taken = jcc->ops[0].imm;
    uint64_t fall = jcc->rip + jcc->len;
    if (jcc->mode32)
        fall = (uint32_t)fall;
    int edge_class = body_edge_pin_class();
    int body_edge = edge_class >= 0;
    uint32_t *to_taken = a64_label(b);
    a64_bcond(b, taken_cond, 0);

    uint32_t *pb_fall = emit_incdec_jcc_arm(
        b, producer, fall, fall <= g_self_rip,
        body_edge, -1, epilogue_sites, n_epi, NULL);
    uint32_t *taken_label = a64_label(b);
    a64_patch_bcond(to_taken, taken_label);
    int taken_rec = 0;
    uint32_t *pb_taken = emit_incdec_jcc_arm(
        b, producer, taken, taken <= g_self_rip,
        body_edge, -1, epilogue_sites, n_epi, &taken_rec);

    g_jcc_edge[0].target_rip = fall;
    g_jcc_edge[0].patch_b = pb_fall;
    g_jcc_edge[0].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
    g_jcc_edge[0].pin_class = body_edge ? (uint8_t)edge_class : 0;
    g_jcc_edge[1].target_rip = taken;
    g_jcc_edge[1].patch_b = pb_taken;
    g_jcc_edge[1].cond_site = cond_short_site(to_taken, taken_rec, body_edge);
    g_jcc_edge[1].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
    g_jcc_edge[1].pin_class = body_edge ? (uint8_t)edge_class : 0;
    g_n_jcc_edges = 2;
    return 1;
}

int emit_arith_incdec_jcc(A64Buf *b, const X86Insn *arith,
                                 const X86Insn *incdec,
                                 const X86Insn *jcc, uint64_t arith_need,
                                 uint32_t **epilogue_sites, int *n_epi,
                                 uint32_t **incdec_label,
                                 uint32_t **jcc_label)
{
    if (!g_defer || g_no_jccfuse || g_no_regflags || g_no_chain ||
        g_no_jcclink || arith_need != OCERZ_CF ||
        (arith->op != OCERZ_OP_ADD && arith->op != OCERZ_OP_SUB) ||
        (incdec->op != OCERZ_OP_INC && incdec->op != OCERZ_OP_DEC) ||
        jcc->op != OCERZ_OP_JCC ||
        (jcc->cc != OCERZ_CC_E && jcc->cc != OCERZ_CC_NE) ||
        jcc->ops[0].kind != OCERZ_OPK_IMM ||
        arith->lock || incdec->lock ||
        arith->rip + arith->len != incdec->rip ||
        incdec->rip + incdec->len != jcc->rip)
        return 0;

    const X86Operand *d = &arith->ops[0];
    const X86Operand *s = &arith->ops[1];
    const X86Operand *id = &incdec->ops[0];
    if (arith->nops != 2 || incdec->nops != 1 ||
        d->kind != OCERZ_OPK_REG || id->kind != OCERZ_OPK_REG ||
        d->high8 || id->high8 ||
        (d->size != 4 && d->size != 8) || id->size != d->size ||
        id->reg == d->reg)
        return 0;
    if (s->kind == OCERZ_OPK_REG) {
        if (s->high8 || s->size != d->size)
            return 0;
    } else if (s->kind != OCERZ_OPK_IMM || s->size != d->size) {
        return 0;
    }

    int sf = d->size == 8;
    int ds = pin_slot(d->reg);
    int rn = ds >= 0 ? pin_hreg(ds) : JT2;
    if (ds < 0)
        emit_gpr_rd(b, sf, rn, d->reg);

    int emitted = 0;
    int rm = JT1;
    if (s->kind == OCERZ_OPK_IMM) {
        uint64_t v = s->imm;
        if (!sf)
            v &= 0xffffffffull;
        if (v <= 4095) {
            if (arith->op == OCERZ_OP_ADD)
                a64_adds_imm(b, sf, rn, rn, (unsigned)v);
            else
                a64_subs_imm(b, sf, rn, rn, (unsigned)v);
            emitted = 1;
        } else {
            a64_mov_imm64(b, JT1, v);
        }
    } else {
        int ss = pin_slot(s->reg);
        if (s->reg == d->reg)
            rm = rn;
        else if (ss >= 0)
            rm = pin_hreg(ss);
        else
            emit_gpr_rd(b, sf, JT1, s->reg);
    }
    if (!emitted) {
        if (arith->op == OCERZ_OP_ADD)
            a64_adds_reg(b, sf, rn, rn, rm, 0);
        else
            a64_subs_reg(b, sf, rn, rn, rm, 0);
    }
    if (ds < 0)
        emit_gpr_wr(b, rn, d->reg);
    a64_cset(b, JT0, arith->op == OCERZ_OP_ADD ? A64_CS : A64_CC);

    *incdec_label = a64_label(b);
    int ids = pin_slot(id->reg);
    int ird = ids >= 0 ? pin_hreg(ids) : JT2;
    if (ids < 0)
        emit_gpr_rd(b, sf, ird, id->reg);
    if (incdec->op == OCERZ_OP_INC)
        a64_adds_imm(b, sf, ird, ird, 1);
    else
        a64_subs_imm(b, sf, ird, ird, 1);
    if (ids < 0)
        emit_gpr_wr(b, ird, id->reg);

    *jcc_label = a64_label(b);
    int taken_cond = jcc->cc == OCERZ_CC_E ? A64_EQ : A64_NE;
    uint64_t taken = jcc->ops[0].imm;
    uint64_t fall = jcc->rip + jcc->len;
    if (jcc->mode32)
        fall = (uint32_t)fall;
    int edge_class = body_edge_pin_class();
    int body_edge = edge_class >= 0;
    uint32_t *to_taken = a64_label(b);
    a64_bcond(b, taken_cond, 0);

    uint32_t *pb_fall = emit_incdec_jcc_arm(
        b, incdec, fall, fall <= g_self_rip,
        body_edge, JT0, epilogue_sites, n_epi, NULL);
    uint32_t *taken_label = a64_label(b);
    a64_patch_bcond(to_taken, taken_label);
    int taken_rec = 0;
    uint32_t *pb_taken = emit_incdec_jcc_arm(
        b, incdec, taken, taken <= g_self_rip,
        body_edge, JT0, epilogue_sites, n_epi, &taken_rec);

    g_jcc_edge[0].target_rip = fall;
    g_jcc_edge[0].patch_b = pb_fall;
    g_jcc_edge[0].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
    g_jcc_edge[0].pin_class = body_edge ? (uint8_t)edge_class : 0;
    g_jcc_edge[1].target_rip = taken;
    g_jcc_edge[1].patch_b = pb_taken;
    g_jcc_edge[1].cond_site = cond_short_site(to_taken, taken_rec, body_edge);
    g_jcc_edge[1].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
    g_jcc_edge[1].pin_class = body_edge ? (uint8_t)edge_class : 0;
    g_n_jcc_edges = 2;
    return 1;
}

int emit_logic_jmp_incdec_jcc(A64Buf *b, const X86Insn *logic,
                                     const X86Insn *jmp,
                                     uint64_t logic_need,
                                     uint32_t **epilogue_sites, int *n_epi,
                                     uint32_t **jmp_label)
{
    if (g_xlat_mode32)
        return 0;
    if (!g_defer || g_no_jccfuse || g_no_regflags || g_no_chain ||
        g_no_jcclink || g_pin_class != 1 || logic_need != OCERZ_CF ||
        (logic->op != OCERZ_OP_AND && logic->op != OCERZ_OP_OR &&
         logic->op != OCERZ_OP_XOR) || logic->lock ||
        jmp->op != OCERZ_OP_JMP || jmp->ops[0].kind != OCERZ_OPK_IMM ||
        logic->rip + logic->len != jmp->rip)
        return 0;

    X86Insn target[2];
    volatile int n = 0;
    volatile uint64_t pc = jmp->ops[0].imm;
    sigjmp_buf db;
    sigjmp_buf *prev = ocerz_jit_decode_recover;
    if (sigsetjmp(db, 0) == 0) {
        ocerz_jit_decode_recover = &db;
        while (n < 2) {
            int rc = jit_decode(pc, &target[n], g_xlat_mode32);
            if (rc != OCERZ_OK)
                break;
            unsigned op = target[n].op;
            uint8_t len = target[n].len;
            n++;
            pc += len;
            if (is_terminator(op))
                break;
        }
    }
    ocerz_jit_decode_recover = prev;
    if (n != 2 || !can_fuse_incdec_jcc(&target[0], &target[1]))
        return 0;

    const X86Operand *ld = &logic->ops[0];
    const X86Operand *ls = &logic->ops[1];
    if (logic->nops != 2 || ld->kind != OCERZ_OPK_REG || ld->high8 ||
        (ld->size != 4 && ld->size != 8) || ls->size != ld->size)
        return 0;
    if (ls->kind == OCERZ_OPK_REG) {
        if (ls->high8)
            return 0;
    } else if (ls->kind != OCERZ_OPK_IMM) {
        return 0;
    }

    if (!emit_arith(b, logic, 0))
        return 0;
    *jmp_label = a64_label(b);

    const X86Insn *incdec = &target[0];
    const X86Insn *jcc = &target[1];
    const X86Operand *id = &incdec->ops[0];
    int sf = id->size == 8;
    int ids = pin_slot(id->reg);
    int rd = ids >= 0 ? pin_hreg(ids) : JT2;
    if (ids < 0)
        emit_gpr_rd(b, sf, rd, id->reg);
    if (incdec->op == OCERZ_OP_INC)
        a64_adds_imm(b, sf, rd, rd, 1);
    else
        a64_subs_imm(b, sf, rd, rd, 1);
    if (ids < 0)
        emit_gpr_wr(b, rd, id->reg);

    int taken_cond = jcc->cc == OCERZ_CC_E ? A64_EQ : A64_NE;
    uint64_t taken = jcc->ops[0].imm;
    uint64_t fall = jcc->rip + jcc->len;
    int edge_class = body_edge_pin_class();
    int body_edge = edge_class >= 0;
    l0_flush_all(b);
    uint32_t *to_taken = a64_label(b);
    a64_bcond(b, taken_cond, 0);

    uint32_t *pb_fall = emit_incdec_jcc_arm(
        b, incdec, fall, fall <= g_self_rip, body_edge, A64_ZR,
        epilogue_sites, n_epi, NULL);
    uint32_t *taken_label = a64_label(b);
    a64_patch_bcond(to_taken, taken_label);
    int taken_rec = 0;
    uint32_t *pb_taken = emit_incdec_jcc_arm(
        b, incdec, taken, taken <= g_self_rip, body_edge, A64_ZR,
        epilogue_sites, n_epi, &taken_rec);

    g_jcc_edge[0].target_rip = fall;
    g_jcc_edge[0].patch_b = pb_fall;
    g_jcc_edge[0].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
    g_jcc_edge[0].pin_class = body_edge ? (uint8_t)edge_class : 0;
    g_jcc_edge[1].target_rip = taken;
    g_jcc_edge[1].patch_b = pb_taken;
    g_jcc_edge[1].cond_site = cond_short_site(to_taken, taken_rec, body_edge);
    g_jcc_edge[1].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
    g_jcc_edge[1].pin_class = body_edge ? (uint8_t)edge_class : 0;
    g_n_jcc_edges = 2;
    return 1;
}

static int decode_ifconv_block(uint64_t rip, X86Insn *out, int cap)
{
    volatile int n = 0;
    volatile int terminated = 0;
    volatile uint64_t pc = rip;
    sigjmp_buf db;
    sigjmp_buf *prev = ocerz_jit_decode_recover;
    if (sigsetjmp(db, 0) == 0) {
        ocerz_jit_decode_recover = &db;
        while (n < cap) {
            if (jit_decode(pc, &out[n], g_xlat_mode32) != OCERZ_OK)
                break;
            unsigned op = out[n].op;
            pc += out[n].len;
            n++;
            if (is_terminator(op)) {
                terminated = 1;
                break;
            }
        }
    } else {
        n = 0;
        terminated = 0;
    }
    ocerz_jit_decode_recover = prev;
    return terminated ? n : 0;
}

static int ifconv_test_bit(const X86Insn *test, const X86Insn *jcc,
                           int *bit)
{
    if (test->op != OCERZ_OP_TEST || test->lock || test->nops != 2 ||
        jcc->op != OCERZ_OP_JCC || jcc->ops[0].kind != OCERZ_OPK_IMM ||
        (jcc->cc != OCERZ_CC_E && jcc->cc != OCERZ_CC_NE) ||
        test->rip + test->len != jcc->rip)
        return 0;
    const X86Operand *d = &test->ops[0];
    const X86Operand *s = &test->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 ||
        (d->size != 4 && d->size != 8) ||
        s->kind != OCERZ_OPK_IMM || s->size != d->size)
        return 0;
    uint64_t mask = s->imm & (d->size == 8 ? UINT64_MAX : 0xffffffffull);
    if (!mask || (mask & (mask - 1)))
        return 0;
    *bit = __builtin_ctzll(mask);
    return 1;
}

static int ifconv_direct_latch(const X86Insn *p, uint64_t loop_rip,
                               uint64_t *latch_rip, uint64_t *exit_rip)
{
    const X86Insn *arith = &p[0];
    const X86Insn *latch = &p[1];
    const X86Insn *jcc = &p[2];
    const X86Operand *ad = &arith->ops[0];
    const X86Operand *as = &arith->ops[1];
    const X86Operand *ld = &latch->ops[0];
    if ((arith->op != OCERZ_OP_ADD && arith->op != OCERZ_OP_SUB) ||
        arith->lock || arith->nops != 2 ||
        ad->kind != OCERZ_OPK_REG || ad->high8 ||
        (ad->size != 4 && ad->size != 8) ||
        as->kind != OCERZ_OPK_IMM || as->size != ad->size ||
        (latch->op != OCERZ_OP_INC && latch->op != OCERZ_OP_DEC) ||
        latch->lock || latch->nops != 1 ||
        ld->kind != OCERZ_OPK_REG || ld->high8 ||
        (ld->size != 4 && ld->size != 8) ||
        jcc->op != OCERZ_OP_JCC || jcc->ops[0].kind != OCERZ_OPK_IMM ||
        (jcc->cc != OCERZ_CC_E && jcc->cc != OCERZ_CC_NE) ||
        arith->rip + arith->len != latch->rip ||
        latch->rip + latch->len != jcc->rip)
        return 0;

    uint64_t taken = jcc->ops[0].imm;
    uint64_t fall = jcc->rip + jcc->len;
    int taken_self = taken == loop_rip;
    int fall_self = fall == loop_rip;
    if (taken_self == fall_self)
        return 0;
    *latch_rip = latch->rip;
    *exit_rip = taken_self ? fall : taken;
    return 1;
}

static int ifconv_simple_path(const X86Insn *p, uint64_t latch_rip,
                              unsigned acc, unsigned size)
{
    const X86Operand *d = &p[0].ops[0];
    return (p[0].op == OCERZ_OP_INC || p[0].op == OCERZ_OP_DEC) &&
        !p[0].lock && p[0].nops == 1 &&
        d->kind == OCERZ_OPK_REG && !d->high8 &&
        d->reg == acc && d->size == size &&
        p[1].op == OCERZ_OP_JMP && p[1].ops[0].kind == OCERZ_OPK_IMM &&
        p[1].ops[0].imm == latch_rip;
}

static int ifconv_complex_path(const X86Insn *p, uint64_t latch_rip,
                               unsigned acc, unsigned size)
{
    const X86Operand *md = &p[0].ops[0];
    const X86Operand *ms = &p[0].ops[1];
    const X86Operand *sd = &p[1].ops[0];
    const X86Operand *ss = &p[1].ops[1];
    const X86Operand *xd = &p[2].ops[0];
    const X86Operand *xs = &p[2].ops[1];
    if (p[0].op != OCERZ_OP_MOV || p[0].lock || p[0].nops != 2 ||
        md->kind != OCERZ_OPK_REG || ms->kind != OCERZ_OPK_REG ||
        md->high8 || ms->high8 || md->reg == acc ||
        md->size != size || ms->size != size ||
        p[1].op != OCERZ_OP_SHR || p[1].lock || p[1].nops != 2 ||
        sd->kind != OCERZ_OPK_REG || sd->high8 ||
        sd->reg != md->reg || sd->size != size ||
        ss->kind != OCERZ_OPK_IMM ||
        p[2].op != OCERZ_OP_XOR || p[2].lock || p[2].nops != 2 ||
        xd->kind != OCERZ_OPK_REG || xs->kind != OCERZ_OPK_REG ||
        xd->high8 || xs->high8 || xd->reg != acc || xd->size != size ||
        xs->reg != md->reg || xs->size != size ||
        p[3].op != OCERZ_OP_JMP || p[3].ops[0].kind != OCERZ_OPK_IMM ||
        p[3].ops[0].imm != latch_rip)
        return 0;
    return 1;
}

static int match_ifconv_diamond(const X86Insn *test, const X86Insn *jcc,
                                IfConvDiamond *m)
{
    memset(m, 0, sizeof *m);
    if (!g_defer || g_no_jccfuse || g_no_regflags || g_no_chain ||
        g_no_jcclink || g_pin_class != 1 || !g_loop_entry ||
        !ifconv_test_bit(test, jcc, &m->first_bit))
        return 0;

    uint64_t first_succ[2] = { jcc->rip + jcc->len, jcc->ops[0].imm };
    for (int direct_taken = 0; direct_taken <= 1; direct_taken++) {
        uint64_t direct_rip = first_succ[direct_taken];
        uint64_t nested_rip = first_succ[!direct_taken];
        uint64_t latch_rip, exit_rip;
        if (decode_ifconv_block(direct_rip, m->direct, 3) != 3 ||
            !ifconv_direct_latch(m->direct, g_self_rip,
                                 &latch_rip, &exit_rip) ||
            decode_ifconv_block(nested_rip, m->nested, 2) != 2 ||
            !ifconv_test_bit(&m->nested[0], &m->nested[1],
                             &m->nested_bit))
            continue;

        const X86Operand *acc = &m->direct[0].ops[0];
        uint64_t nested_succ[2] = {
            m->nested[1].rip + m->nested[1].len,
            m->nested[1].ops[0].imm,
        };
        for (int simple_taken = 0; simple_taken <= 1; simple_taken++) {
            uint64_t simple_rip = nested_succ[simple_taken];
            uint64_t complex_rip = nested_succ[!simple_taken];
            if (decode_ifconv_block(simple_rip, m->simple, 2) != 2 ||
                !ifconv_simple_path(m->simple, latch_rip,
                                    acc->reg, acc->size) ||
                decode_ifconv_block(complex_rip, m->complex, 4) != 4 ||
                !ifconv_complex_path(m->complex, latch_rip,
                                     acc->reg, acc->size))
                continue;

            const X86Operand *tmp = &m->complex[0].ops[0];
            const X86Operand *src = &m->complex[0].ops[1];
            const X86Operand *latch = &m->direct[1].ops[0];
            const X86Operand *nested_test = &m->nested[0].ops[0];
            if (pin_slot(test->ops[0].reg) < 0 ||
                pin_slot(nested_test->reg) < 0 ||
                pin_slot(acc->reg) < 0 || pin_slot(tmp->reg) < 0 ||
                pin_slot(src->reg) < 0 || pin_slot(latch->reg) < 0)
                continue;

            m->direct_is_taken = direct_taken;
            m->simple_is_taken = simple_taken;
            m->exit_rip = exit_rip;
            return 1;
        }
    }
    return 0;
}

int emit_ifconv_diamond(A64Buf *b, const X86Insn *test,
                               const X86Insn *jcc,
                               uint32_t **epilogue_sites, int *n_epi,
                               uint32_t **jcc_label)
{
    if (g_xlat_mode32)
        return 0;
    IfConvDiamond m;
    if (!match_ifconv_diamond(test, jcc, &m))
        return 0;

    const X86Insn *arith = &m.direct[0];
    const X86Insn *latch = &m.direct[1];
    const X86Insn *latch_jcc = &m.direct[2];
    const X86Insn *simple = &m.simple[0];
    const X86Operand *acc = &arith->ops[0];
    const X86Operand *tmp = &m.complex[0].ops[0];
    const X86Operand *src = &m.complex[0].ops[1];
    const X86Operand *ld = &latch->ops[0];
    int sf = acc->size == 8;
    int acc_hr = pin_hreg(pin_slot(acc->reg));
    int tmp_hr = pin_hreg(pin_slot(tmp->reg));
    int src_hr = pin_hreg(pin_slot(src->reg));
    int latch_hr = pin_hreg(pin_slot(ld->reg));

    a64_ubfx(b, 1, JT0, pin_hreg(pin_slot(test->ops[0].reg)),
             m.first_bit, 1);
    *jcc_label = a64_label(b);
    a64_ubfx(b, 1, JT1, pin_hreg(pin_slot(m.nested[0].ops[0].reg)),
             m.nested_bit, 1);

    uint64_t imm = arith->ops[1].imm &
        (sf ? UINT64_MAX : 0xffffffffull);
    if (imm <= 4095) {
        if (arith->op == OCERZ_OP_ADD)
            a64_adds_imm(b, sf, JT2, acc_hr, (unsigned)imm);
        else
            a64_subs_imm(b, sf, JT2, acc_hr, (unsigned)imm);
    } else {
        a64_mov_imm64(b, JTT, imm);
        if (arith->op == OCERZ_OP_ADD)
            a64_adds_reg(b, sf, JT2, acc_hr, JTT, 0);
        else
            a64_subs_reg(b, sf, JT2, acc_hr, JTT, 0);
    }
    a64_cset(b, JTF, arith->op == OCERZ_OP_ADD ? A64_CS : A64_CC);

    if (simple->op == OCERZ_OP_INC)
        a64_add_imm(b, sf, JTT, acc_hr, 1);
    else
        a64_sub_imm(b, sf, JTT, acc_hr, 1);

    unsigned shift = (unsigned)m.complex[1].ops[1].imm & (sf ? 63u : 31u);
    a64_lsr_imm(b, sf, JTU, src_hr, shift);
    a64_eor_reg(b, sf, JTA, acc_hr, JTU, 0);

    int simple_on_nonzero =
        (m.nested[1].cc == OCERZ_CC_NE) == m.simple_is_taken;
    int simple_cond = simple_on_nonzero ? A64_NE : A64_EQ;
    a64_subs_imm(b, 1, A64_ZR, JT1, 0);
    a64_csel(b, sf, JTA, JTT, JTA, simple_cond);
    a64_csel(b, sf, JTU, tmp_hr, JTU, simple_cond);

    int direct_on_nonzero = (jcc->cc == OCERZ_CC_NE) == m.direct_is_taken;
    int direct_cond = direct_on_nonzero ? A64_NE : A64_EQ;
    a64_subs_imm(b, 1, A64_ZR, JT0, 0);
    a64_csel(b, sf, acc_hr, JT2, JTA, direct_cond);
    a64_csel(b, sf, tmp_hr, tmp_hr, JTU, direct_cond);
    a64_csel(b, 1, JT0, JTF, A64_ZR, direct_cond);

    int lsf = ld->size == 8;
    if (latch->op == OCERZ_OP_INC)
        a64_adds_imm(b, lsf, latch_hr, latch_hr, 1);
    else
        a64_subs_imm(b, lsf, latch_hr, latch_hr, 1);

    uint64_t latch_taken = latch_jcc->ops[0].imm;
    int taken_cond = latch_jcc->cc == OCERZ_CC_E ? A64_EQ : A64_NE;
    int self_cond = latch_taken == g_self_rip
        ? taken_cond : A64_INV(taken_cond);
    l0_fixed_backedge(b);
    g_stop_patch = a64_label(b);
    a64_bcond(b, self_cond, (int32_t)(g_loop_entry - g_stop_patch));
    l0_fixed_fallthrough(b);
    fpb_emit_exit_check(b);

    int edge_class = body_edge_pin_class();
    int body_edge = edge_class >= 0;
    uint32_t *pb_exit = emit_incdec_jcc_arm(
        b, latch, m.exit_rip, m.exit_rip <= g_self_rip,
        body_edge, JT0, epilogue_sites, n_epi, NULL);

    g_stop_target = a64_label(b);
    emit_gpr_rd(b, 1, JT1, ld->reg);
    emit_defer_flags(b,
        ocerz_cc_pack(latch->op == OCERZ_OP_INC ? OCERZ_CC_INC
                                                 : OCERZ_CC_DEC,
                      ld->size, 0),
        JT0, JT1);
    a64_mov_imm64(b, JT2, g_self_rip);
    a64_str(b, 8, JT2, 20, RIP_OFF);
    emit_materialize(b);
    a64_mov_imm64(b, 0, OCERZ_STEP_OK);
    epilogue_sites[*n_epi] = a64_label(b);
    a64_b(b, 0);
    (*n_epi)++;

    g_jcc_edge[0].target_rip = m.exit_rip;
    g_jcc_edge[0].patch_b = pb_exit;
    g_jcc_edge[0].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
    g_jcc_edge[0].pin_class = body_edge ? (uint8_t)edge_class : 0;
    g_n_jcc_edges = 1;
    return 1;
}

int emit_jcc(A64Buf *b, const X86Insn *insn, uint32_t **epilogue_sites, int *n_epi)
{
    if (insn->op != OCERZ_OP_JCC)
        return 0;
    unsigned cc = insn->cc;
    uint64_t taken = insn->ops[0].imm;
    uint64_t fall = insn->rip + insn->len;
    if (insn->mode32)
        fall = (uint32_t)fall;

    int self_loop = !g_no_chain && g_loop_entry && taken == g_self_rip;
    int two_way = !self_loop && !g_no_chain && !g_no_jcclink;
    g_cc_want_cbz = two_way;
    emit_cc_predicate_ex(b, cc, two_way || self_loop);
    g_cc_want_cbz = 0;
    int direct = (two_way || self_loop) ? g_cc_direct : -1;
    if (self_loop && direct >= 0) {
        l0_fixed_backedge(b);
        g_stop_patch = a64_label(b);
        a64_bcond(b, direct, (int32_t)(g_loop_entry - g_stop_patch));
        l0_fixed_fallthrough(b);
        fpb_emit_exit_check(b);
        int edge_class = body_edge_pin_class();
        int body_edge = edge_class >= 0;
        uint32_t *pb_fall = emit_static_chain_tail(
            b, fall, fall <= g_self_rip, body_edge, epilogue_sites, n_epi);
        g_jcc_edge[0].target_rip = fall;
        g_jcc_edge[0].patch_b = pb_fall;
        g_jcc_edge[0].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
        g_jcc_edge[0].pin_class = body_edge ? (uint8_t)edge_class : 0;
        g_n_jcc_edges = 1;
        g_stop_target = a64_label(b);
        a64_mov_imm64(b, JT0, taken);
        a64_str(b, 8, JT0, 20, RIP_OFF);
        emit_materialize(b);
        a64_mov_imm64(b, 0, OCERZ_STEP_OK);
        epilogue_sites[*n_epi] = a64_label(b);
        a64_b(b, 0);
        (*n_epi)++;
        return 1;
    }
    if (!two_way) {
        a64_mov_imm64(b, JT1, fall);
        a64_mov_imm64(b, JT2, taken);
        a64_csel(b, 1, JT0, JT2, JT1, A64_NE);
        a64_str(b, 8, JT0, 20, RIP_OFF);
    }

    if (self_loop) {
        a64_mov_imm64(b, JT1, g_self_rip);
        a64_subs_reg(b, 1, A64_ZR, JT0, JT1, 0);
        l0_fixed_backedge(b);
        g_stop_patch = a64_label(b);
        a64_bcond(b, A64_EQ, (int32_t)(g_loop_entry - g_stop_patch));
        l0_fixed_fallthrough(b);
        fpb_emit_exit_check(b);
        {
            int edge_class = body_edge_pin_class();
            int body_edge = edge_class >= 0;
            uint32_t *pb_fall = emit_static_chain_tail(
                b, fall, fall <= g_self_rip, body_edge, epilogue_sites, n_epi);
            g_jcc_edge[0].target_rip = fall;
            g_jcc_edge[0].patch_b = pb_fall;
            g_jcc_edge[0].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
            g_jcc_edge[0].pin_class = body_edge ? (uint8_t)edge_class : 0;
            g_n_jcc_edges = 1;
        }
        g_stop_target = a64_label(b);
        a64_mov_imm64(b, 0, OCERZ_STEP_OK);
        epilogue_sites[*n_epi] = a64_label(b);
        a64_b(b, 0);
        (*n_epi)++;
        return 1;
    }

    if (two_way) {
        int poll_fall  = fall <= g_self_rip;
        int poll_taken = taken <= g_self_rip;
        int edge_class = body_edge_pin_class();
        int body_edge = edge_class >= 0;
        uint32_t *to_taken = a64_label(b);
        if (g_cc_cbz_reg >= 0) {
            if (g_cc_cbz_nz) a64_cbnz(b, g_cc_cbz_sf, g_cc_cbz_reg, 0);
            else             a64_cbz(b, g_cc_cbz_sf, g_cc_cbz_reg, 0);
        } else
        a64_bcond(b, direct >= 0 ? direct : A64_NE, 0);

        uint32_t *pb_fall = emit_static_chain_tail(
            b, fall, poll_fall, body_edge, epilogue_sites, n_epi);

        uint32_t *ltaken = a64_label(b);
        if ((*to_taken & 0x7e000000u) == 0x34000000u) a64_patch_cbz(to_taken, ltaken);
        else a64_patch_bcond(to_taken, ltaken);
        uint32_t *pb_taken = emit_static_chain_tail(
            b, taken, poll_taken, body_edge, epilogue_sites, n_epi);
        g_jcc_edge[0].target_rip = fall;
        g_jcc_edge[0].patch_b = pb_fall;
        g_jcc_edge[0].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
        g_jcc_edge[0].pin_class = body_edge ? (uint8_t)edge_class : 0;
        g_jcc_edge[1].target_rip = taken;
        g_jcc_edge[1].patch_b = pb_taken;
        g_jcc_edge[1].cond_site = cond_short_site(to_taken, 0, body_edge);
        g_jcc_edge[1].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
        g_jcc_edge[1].pin_class = body_edge ? (uint8_t)edge_class : 0;
        g_n_jcc_edges = 2;
        return 1;
    }

    a64_mov_imm64(b, 0, OCERZ_STEP_OK);
    epilogue_sites[*n_epi] = a64_label(b);
    a64_b(b, 0);
    (*n_epi)++;
    return 1;
}

uint64_t g_flip_n_retire;

__thread sigjmp_buf *ocerz_jit_decode_recover;

static uint64_t g_flip_n_hit;

static uint64_t g_flip_ns_hit;

static uint64_t g_flip_ns_retire;

uint64_t g_flip_n_retire;

static OcerzJit *g_flip_atexit_jit;

static void flip_report_atexit(void)
{
    fprintf(stderr, "ocerz: FLIPSTAT[%d] trips=%llu hit_ms=%.1f retires=%llu retire_ms=%.1f translated=%llu live=%zu probes=%d\n",
            (int)getpid(), (unsigned long long)g_flip_n_hit, g_flip_ns_hit / 1e6,
            (unsigned long long)g_flip_n_retire, g_flip_ns_retire / 1e6,
            (unsigned long long)(g_flip_atexit_jit ? g_flip_atexit_jit->blocks_translated : 0),
            g_flip_atexit_jit ? g_flip_atexit_jit->n_live : (size_t)0, g_n_probes);
}

void flip_retire_locked(struct OcerzVM *vm, OcerzJit *jit, JitBlock *blk)
{
    const uint32_t *lo = (const uint32_t *)blk->code, *hi = lo + blk->code_words;
#define IN_BLK(p) ((const uint32_t *)(p) >= lo && (const uint32_t *)(p) < hi)
    size_t idx = blk->live_idx;
    if (idx >= jit->n_live || jit->live[idx] != blk) {
        idx = jit->n_live;
        for (size_t k = 0; k < jit->n_live; k++)
            if (jit->live[k] == blk) { idx = k; break; }
    }
    if (idx == jit->n_live) return;
    pthread_jit_write_protect_np(0);
    if (blk->stop_patch && blk->stop_insn && *blk->stop_patch != blk->stop_insn) {
        __atomic_store_n(blk->stop_patch, blk->stop_insn, __ATOMIC_RELEASE);
        sys_icache_invalidate(blk->stop_patch, 4);
    }
    for (int i = 0; i < blk->n_stop_extra; i++)
        if (*blk->stop_extra[i].site != blk->stop_extra[i].insn) {
            __atomic_store_n(blk->stop_extra[i].site, blk->stop_extra[i].insn, __ATOMIC_RELEASE);
            sys_icache_invalidate(blk->stop_extra[i].site, 4);
        }
    for (int i = 0; i < blk->n_edges; i++) {
        uint32_t *cs = blk->edges[i].cond_site;
        int is_stop = blk->edges[i].patch_b == blk->stop_patch;
        for (int q = 0; q < blk->n_stop_extra && !is_stop; q++)
            is_stop = blk->edges[i].patch_b == blk->stop_extra[q].site;
        if (cs && blk->edges[i].cond_orig && *cs != blk->edges[i].cond_orig && is_stop) {
            __atomic_store_n(cs, blk->edges[i].cond_orig, __ATOMIC_RELEASE);
            sys_icache_invalidate(cs, 4);
        }
    }
    for (uint32_t q = 0; q < blk->n_preds; q++) {
        JitBlock *sblk = blk->preds[q].pb;
        int i = blk->preds[q].e;
        if (sblk == blk || i >= sblk->n_edges) continue;
        {
            uint32_t *at = sblk->edges[i].patch_b;
            uint32_t fallback = sblk->edges[i].fallback_insn;
            int cut = 0;
            if (at && fallback && *at != fallback && IN_BLK(branch_word_target(at, *at))) {
                __atomic_store_n(at, fallback, __ATOMIC_RELEASE);
                sys_icache_invalidate(at, 4);
                cut = 1;
            }
            uint32_t *cs = sblk->edges[i].cond_site;
            if (cs && sblk->edges[i].cond_orig && *cs != sblk->edges[i].cond_orig &&
                IN_BLK(branch_word_target(cs, *cs))) {
                __atomic_store_n(cs, sblk->edges[i].cond_orig, __ATOMIC_RELEASE);
                sys_icache_invalidate(cs, 4);
                cut = 1;
            }
            if (cut)
                pending_add(jit_key(sblk->edges[i].target_rip, blk_mode32(sblk)), at,
                            sblk->edges[i].kind, sblk->edges[i].pin_class,
                            sblk->edges[i].probing ? NULL : cs, sblk->hoist_sig, sblk, i);
        }
    }
    blk->n_preds = 0;
    for (size_t i = 0; i < g_n_ras_cells; i++)
        if (IN_BLK(*g_ras_cells[i]))
            __atomic_store_n(g_ras_cells[i], (void *)NULL, __ATOMIC_RELEASE);
    pthread_jit_write_protect_np(1);
    unsigned h = hash_key(blk->key);
    JitBlock **pp = &jit->buckets[h];
    while (*pp && *pp != blk) pp = &(*pp)->hnext;
    if (*pp == blk)
        __atomic_store_n(pp, blk->hnext, __ATOMIC_RELEASE);
    jit->live[idx] = jit->live[jit->n_live - 1];
    jit->live[idx]->live_idx = idx;
    jit->n_live--;
    gran_block(blk, -1);
    tc_noload_add(blk->key);
    blk->retired_next = jit->retired;
    jit->retired = blk;
    for (unsigned i = 0; i < g_ras_slot_n; i++)
        if (IN_BLK(g_ras_slots[i]))
            __atomic_store_n(&g_ras_slots[i], NULL, __ATOMIC_RELEASE);
    psc_retire_cols(vm, 1u << psc_col(blk->key));
#undef IN_BLK
}

static void flip_retire_block(struct OcerzVM *vm, OcerzJit *jit, JitBlock *blk)
{
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    g_flip_n_retire++;
    jl_acquire(__LINE__);
    int live = blk->code != NULL;
    if (live) flip_retire_locked(vm, jit, blk);
    jl_release();
    if (live) ocerz_vm_purge_jit_ras(vm);
    g_flip_ns_retire += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    if (live && ocerz_jit_time_xlat)
        __atomic_add_fetch(&ocerz_jit_retire_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0, __ATOMIC_RELAXED);
}

static int flip_decide_locked(JitBlock *blk, int e, int tk, int ft, int logit)
{
    uint64_t jcc_rip = blk->edges[e].jcc_rip;
    int flip = tk >= 2 * ft && tk >= (1 << (PROBE_BIT - 1));
    int i = flip_find(jcc_rip, 1);
    if (i < 0) flip = 0;
    else if (g_flip[i].state != FLIP_NONE) flip = g_flip[i].state == FLIP_DECIDED_INV;
    else g_flip[i].state = flip ? FLIP_DECIDED_INV : FLIP_DECIDED_ORIG;
    if (logit)
        fprintf(stderr, "ocerz: FLIP[%d] blk=%#llx jcc=%#llx taken=%d fall=%d -> %s\n", (int)getpid(),
                (unsigned long long)blk_rip(blk), (unsigned long long)jcc_rip, tk, ft, flip ? "invert" : "keep");
    blk->edges[e].probing = 0;
    if (!flip) {






        JitProf *pf = &blk->prof[blk->edges[e].side - 1];
        static int norearm = -1; if (norearm < 0) norearm = getenv("OCERZ_NO_FLIP_REARM") ? 1 : 0;
        int watch = !norearm && pf->rearms < FLIP_REARMS;
        pf->ft_word = *pf->ft_site;
        pf->tk_word = *pf->tk_trip;
        uint32_t tk = watch ? (pf->tk_word & ~((1u << 31) | (0x1fu << 19))) | ((uint32_t)WATCH_BIT << 19) : A64_NOP;
        if (watch) { pf->taken = 0; blk->edges[e].probing = 2; }
        pthread_jit_write_protect_np(0);
        __atomic_store_n(pf->ft_site, A64_NOP, __ATOMIC_RELEASE);
        __atomic_store_n(pf->tk_trip, tk, __ATOMIC_RELEASE);
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(pf->ft_site, 4);
        sys_icache_invalidate(pf->tk_trip, 4);
    }
    return flip;
}

void flip_side_hit(struct OcerzVM *vm, OcerzJit *jit, OcerzCPU *cpu)
{
    JitBlock *blk = (JitBlock *)cpu->side_blk;
    int k = cpu->side_idx;
    cpu->side_blk = NULL;
    if (k == -2 && blk) {
        lowhoist_mark(blk->key);
        flip_retire_block(vm, jit, blk);
        return;
    }
    if (k == -3 && blk) {
        if (getenv("OCERZ_FLIPLOG"))
            fprintf(stderr, "ocerz: FLIP[%d] blk=%#llx entered with another x87 TOP -> translate without it\n",
                    (int)getpid(), (unsigned long long)blk_rip(blk));
        mark_add(&g_x87spec_marks, blk->key);
        flip_retire_block(vm, jit, blk);
        return;
    }
    if (!blk || !blk->prof || k < 0 || k >= SIDE_MAX) return;
    int e = -1;
    for (int i = 0; i < blk->n_edges; i++)
        if (blk->edges[i].side == k + 1) { e = i; break; }
    if (e < 0 || !blk->edges[e].probing) return;
    static int fliplog = -1;
    if (fliplog < 0) {
        fliplog = getenv("OCERZ_FLIPLOG") ? 1 : 0;
        if (fliplog) { g_flip_atexit_jit = jit; atexit(flip_report_atexit); }
    }
    if (blk->edges[e].probing == 2) {
        JitProf *wp = &blk->prof[k];
        jl_acquire(__LINE__);
        if (blk->live_idx >= jit->n_live || jit->live[blk->live_idx] != blk || blk->edges[e].probing != 2) {
            jl_release();
            return;
        }
        int fi = flip_find(blk->edges[e].jcc_rip, 0);
        if (fi >= 0) g_flip[fi].state = FLIP_NONE;
        wp->taken = wp->ft = 0;
        wp->windows = 0;
        wp->rearms++;
        pthread_jit_write_protect_np(0);
        __atomic_store_n(wp->ft_site, wp->ft_word, __ATOMIC_RELEASE);
        __atomic_store_n(wp->tk_trip, wp->tk_word, __ATOMIC_RELEASE);
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(wp->ft_site, 4);
        sys_icache_invalidate(wp->tk_trip, 4);
        blk->edges[e].probing = 1;
        jl_release();
        if (fliplog)
            fprintf(stderr, "ocerz: FLIP[%d] blk=%#llx jcc=%#llx kept side hot again -> probe (%d)\n", (int)getpid(),
                    (unsigned long long)blk_rip(blk), (unsigned long long)blk->edges[e].jcc_rip, wp->rearms);
        return;
    }
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    g_flip_n_hit++;
    JitProf *pf = &blk->prof[k];
    int tk = (int)pf->taken, ft = (int)pf->ft;
    int verdict = tk >= 2 * ft && tk >= (1 << (PROBE_BIT - 1));
    int flip = 0;
    jl_acquire(__LINE__);
    if (blk->live_idx >= jit->n_live || jit->live[blk->live_idx] != blk) {
        jl_release();
        return;
    }
    if (pf->windows == 0 || (pf->prev != verdict && pf->windows < 3)) {
        pf->prev = (uint8_t)verdict;
        pf->windows++;
        pf->taken = pf->ft = 0;
        jl_release();
        return;
    }
    flip = flip_decide_locked(blk, e, tk, ft, fliplog);
    if (!flip) chain_edge_now(jit, blk, e);
    jl_release();
    g_flip_ns_hit += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    static int noretire = -1;
    if (noretire < 0) noretire = getenv("OCERZ_FLIP_NORETIRE") ? 1 : 0;
    if (flip && !noretire) flip_retire_block(vm, jit, blk);
}

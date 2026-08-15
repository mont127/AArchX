/* The JIT: basic blocks translated to native arm64, with the interpreter as fallback. */
#include "ocerz/jit.h"
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/decode.h"
#include "ocerz/interp.h"
#include "ocerz/flags.h"
#include "ocerz/flags_live.h"
#include "ocerz/a64emit.h"
#include "ocerz/dyldapi.h"

#include <sys/mman.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <setjmp.h>
#include <libkern/OSCacheControl.h>
#include <stddef.h>
#include <stdlib.h>
#include <assert.h>

#define JIT_CODE_BYTES_DEFAULT (1024ull << 20)

static size_t jit_code_bytes(void)
{
    const char *kb = getenv("OCERZ_JIT_CODE_KB");
    if (kb) {
        unsigned long v = strtoul(kb, NULL, 0);
        if (v) {
            size_t pg = (size_t)getpagesize();
            size_t bytes = ((size_t)v << 10 | 0) + pg - 1;
            return bytes - (bytes % pg);
        }
    }
    const char *e = getenv("OCERZ_JIT_CODE_MB");
    unsigned long mb = e ? strtoul(e, NULL, 0) : 0;
    return mb ? ((size_t)mb << 20) : (size_t)JIT_CODE_BYTES_DEFAULT;
}

#define JIT_HASH_BITS 20
#define JIT_HASH_SIZE (1u << JIT_HASH_BITS)
#define JIT_HASH_MASK (JIT_HASH_SIZE - 1)
#define JIT_MAX_BLOCK_INSNS 256

typedef int (*JitBlockFn)(struct OcerzVM *, OcerzCPU *);

enum JitFaultFlagRecipeKind {
    JFF_NONE = 0,
    JFF_LOGIC_RESULT,
    JFF_ADD_RESULT_SRC,
    JFF_ADD_INC_RESULT_SRC,
};

typedef struct JitFaultFlagRecipe {
    uint8_t kind;
    uint8_t producer;
} JitFaultFlagRecipe;

typedef struct JitBlock {
    uint64_t guest_rip;
    JitBlockFn code;
    uint32_t *body_code;
    X86Insn *insns;
    int n_insns;
    struct JitBlock *hnext;
    struct JitBlock *retired_next;

    uint64_t exec_count;
    int n_inlined;
    int n_slow;

    uint32_t *insn_off;

    struct JitOslowMap { uint32_t lo, hi; int32_t idx; } *oslow;
    int n_oslow;
    JitFaultFlagRecipe *fault_flags;
    uint32_t code_words;

    uint32_t *stop_patch;
    uint32_t stop_insn;
    struct JitBlock *stop_next;

    uint8_t host_holds[16];
    int8_t guest_in_host[16];
    uint8_t n_pinned;

    uint8_t pin_class;

    struct {
        uint64_t target_rip;
        uint32_t *patch_b;
        uint32_t fallback_insn;
        uint8_t kind;
        uint8_t pin_class;
    } edges[2];
    uint8_t n_edges;

    uint16_t entry_live;
    uint16_t xmm_pinned;      /* xmmN held in V(16+N) for the block's body */
} JitBlock;

typedef struct JitCodeIndex {
    struct JitCodeIndex *older;
    size_t capacity;
    size_t count;
    JitBlock *blocks[];
} JitCodeIndex;

enum { EDGE_XBLOCK = 0, EDGE_SELFLOOP = 1, EDGE_BODY = 2 };

struct OcerzJit {
    struct OcerzVM *vm;
    uint32_t *code_base;
    uint32_t *code_cur;
    uint32_t *code_end;
    size_t code_bytes;
    int code_full;
    JitBlock *buckets[JIT_HASH_SIZE];
    JitBlock *retired;
    JitBlock *stop_blocks;
    int plain_mem;
    int stop_requested;
    uint64_t blocks_translated;

    JitCodeIndex *ci;
    uint32_t *dispatch_stub;   /* in-arena block dispatcher (see emit_dispatch_stub) */
};

int ocerz_perfstat = -1;

static int g_flaglive_log;

static int g_no_lazyflags;

static int g_no_ras;

static int g_no_ldapr;

static int g_no_oolslow;

typedef struct {
    uint32_t *bne;
    uint32_t *back;
    int size, rv, ra, store, idx;
} OrderedSlowPend;
#define OSLOW_MAX 64
static OrderedSlowPend g_oslow[OSLOW_MAX];

/* Out-of-line NaN fixups: the hot path branches here on a NaN result and the
 * arm branches back.  Emitted after the block body (emit_nan_ool_arms). */
typedef struct {
    uint32_t *site;     /* the taken-on-NaN branch (bcond VS or cbz) to patch */
    uint32_t *back;
    uint8_t dbl, packed, vr, va, vb, t1;
    int idx;
    int is_cbz;
} NanOolPend;
#define NANOOL_MAX 64
static NanOolPend g_nanool[NANOOL_MAX];
static int g_n_nanool;
static int g_n_oslow;
static int g_cur_insn_idx;
/* Static flag-producer hint for the instruction being emitted: the nearest
 * earlier instruction in the block that defines flags (NULL if flags come
 * from outside the block).  Lets emit_cc_predicate specialize. */
static const X86Insn *g_flag_producer;

static int g_no_regflags;

static int g_no_chain;

static int g_no_jcclink;

static int g_no_xlive;

static int g_no_jccfuse;

static int g_no_addincfuse;

static int g_no_fault_recipes;

static int g_plain_mem;

static uint64_t g_chain_target;
static uint32_t *g_chain_epi;

static uint64_t g_self_rip;
static uint32_t *g_body_entry;
static uint32_t *g_loop_entry;
static uint32_t *g_stop_patch;
static uint32_t *g_stop_target;
static int g_mem_hoist_greg = -1;
static int g_mem_hoist_aux_disp;
#define JMEMBASE 17
#define JMEMAUX 29
static void emit_reload_mem_base(A64Buf *b);

static struct {
    uint64_t target_rip;
    uint32_t *patch_b;
    uint8_t kind;
    uint8_t pin_class;
} g_jcc_edge[2];
static int g_n_jcc_edges;

static struct {
    uint64_t target_rip;
    uint32_t *patch_b;
    uint8_t kind;
    uint8_t pin_class;
} g_call_edge[2];
static int g_n_call_edges;

static OcerzJit *g_xlat_jit;

static int8_t *g_pin;
static uint8_t *g_pin_hold;
static int g_n_pinned;
static int g_pin_class;

static int g_defer;
static _Atomic unsigned long long ps_ops[OCERZ_OP_COUNT];
static _Atomic unsigned long long ps_slow_insns;
static _Atomic unsigned long long ps_steps, ps_hits, ps_misses;

static _Atomic unsigned long long ps_shape[9][2];
static const char *ps_shape_name[9] = { "push", "pop", "test", "movsxd", "call", "ret",
                                        "jmp", "jmpind", "jmpmem" };

static _Atomic unsigned long long ps_chain_ok, ps_chain_far;
static uint64_t ps_t0;

static __attribute__((noinline, cold, preserve_most)) void jit_trace_one(const X86Insn *insn)
{
    char buf[128];
    ocerz_format_insn(insn, buf, sizeof buf);
    fprintf(stderr, "ocerz: %#llx: %s\n", (unsigned long long)insn->rip, buf);
}

static __attribute__((noinline, cold, preserve_most)) void jit_perfstat_one(const X86Insn *insn)
{
    ps_slow_insns++;
    unsigned o = insn->op;
    if (o < OCERZ_OP_COUNT)
        ps_ops[o]++;

    if (o == OCERZ_OP_PUSH || o == OCERZ_OP_POP) {
        ps_shape[o == OCERZ_OP_PUSH ? 0 : 1][
            (insn->opsize == 8 && insn->seg == OCERZ_SEG_NONE &&
             (insn->ops[0].kind == OCERZ_OPK_REG ? !insn->ops[0].high8
              : insn->ops[0].kind == OCERZ_OPK_IMM)) ? 0 : 1]++;
    } else if (o == OCERZ_OP_TEST || o == OCERZ_OP_MOVSXD) {
        ps_shape[o == OCERZ_OP_TEST ? 2 : 3][
            ((insn->ops[0].size == 4 || insn->ops[0].size == 8) &&
             insn->ops[0].kind == OCERZ_OPK_REG && !insn->ops[0].high8) ? 0 : 1]++;
    } else if (o == OCERZ_OP_CALL || o == OCERZ_OP_RET) {

        if (o == OCERZ_OP_CALL)
            ps_shape[4][insn->ops[0].kind == OCERZ_OPK_IMM ? 0 : 1]++;
        else
            ps_shape[5][insn->nops == 0 ? 0 : 1]++;
    } else if (o == OCERZ_OP_JMP) {

        int direct = (insn->ops[0].kind == OCERZ_OPK_IMM);
        ps_shape[6][direct ? 0 : 1]++;
        if (!direct) {
            int isreg = (insn->ops[0].kind == OCERZ_OPK_REG);
            ps_shape[7][isreg ? 0 : 1]++;
            if (!isreg)
                ps_shape[8][(insn->seg == OCERZ_SEG_NONE && insn->addrsize != 4) ? 0 : 1]++;
        }
    }
}

int ocerz_jit_exec_one(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    if (__builtin_expect(ocerz_perfstat > 0, 0))
        jit_perfstat_one(insn);
    vm->insn_count++;
    cpu->cur_rip = insn->rip;
    cpu->rip = insn->rip + insn->len;
    if (__builtin_expect(vm->trace != 0, 0))
        jit_trace_one(insn);
    return ocerz_interp_exec(vm, cpu, insn);
}

static int is_terminator(unsigned op)
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

static unsigned hash_rip(uint64_t rip)
{
    rip ^= rip >> 33;
    rip *= 0xff51afd7ed558ccdull;
    rip ^= rip >> 29;
    return (unsigned)(rip & JIT_HASH_MASK);
}

static JitBlock *cache_lookup(OcerzJit *jit, uint64_t rip)
{
    for (JitBlock *b = __atomic_load_n(&jit->buckets[hash_rip(rip)], __ATOMIC_ACQUIRE);
         b; b = b->hnext)
        if (b->guest_rip == rip)
            return b;
    return NULL;
}

static void cache_insert(OcerzJit *jit, JitBlock *b)
{
    unsigned h = hash_rip(b->guest_rip);
    b->hnext = jit->buckets[h];
    __atomic_store_n(&jit->buckets[h], b, __ATOMIC_RELEASE);
}

/* ---- monomorphic inline caches for indirect jmp/call ----------------------
 * Each indirect-branch site owns a slot {guest_rip, host_entry}.  The emitted
 * code compares the computed target with slot->rip and, on a hit, jumps
 * straight to the compiled block (same tail as a RAS-hit ret).  On a miss it
 * calls ocerz_jit_ic_fill, which rewrites the slot for the *current* target
 * when that block is compiled, then falls out to the dispatcher. */
typedef struct JitIcSlot { uint64_t rip; void *code; } JitIcSlot;
#define JIT_IC_SLOTS (1u << 16)
static JitIcSlot g_ic_slots[JIT_IC_SLOTS];
static unsigned g_ic_next;

static JitIcSlot *ic_slot_alloc(void)
{
    unsigned i = __atomic_fetch_add(&g_ic_next, 1, __ATOMIC_RELAXED);
    if (i >= JIT_IC_SLOTS)
        return NULL;
    return &g_ic_slots[i];
}

void ocerz_jit_ic_fill(struct OcerzVM *vm, OcerzCPU *cpu, JitIcSlot *slot);
static _Atomic unsigned long long g_ic_miss_calls, g_ic_fills, g_ic_nocode;
void ocerz_jit_ic_fill(struct OcerzVM *vm, OcerzCPU *cpu, JitIcSlot *slot)
{
    OcerzJit *jit = vm->jit;
    if (!jit || !slot)
        return;
    g_ic_miss_calls++;
    JitBlock *t = cache_lookup(jit, cpu->rip);
    if (!t || !t->code) g_ic_nocode++; else g_ic_fills++;
    if (t && t->code) {
        __atomic_store_n(&slot->code, (void *)t->code, __ATOMIC_RELAXED);
        __atomic_store_n(&slot->rip, cpu->rip, __ATOMIC_RELEASE);
    }
}

static uint64_t xlive_decode_entry(uint64_t rip)
{
    X86Insn insns[JIT_MAX_BLOCK_INSNS];
    volatile int n = 0;
    volatile uint64_t pc = rip;
    sigjmp_buf db;
    sigjmp_buf *prev = ocerz_jit_decode_recover;
    if (sigsetjmp(db, 1) == 0) {
        ocerz_jit_decode_recover = &db;
        while (n < JIT_MAX_BLOCK_INSNS) {
            int rc = ocerz_decode((const uint8_t *)ocerz_g2h(pc), 15, pc,
                                  &insns[n]);
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
    if (n == 0)
        return OCERZ_FL_ALL;

    uint64_t live = OCERZ_FL_ALL;
    for (int i = n - 1; i >= 0; i--) {
        uint64_t def, use;
        ocerz_flags_defuse(&insns[i], &def, &use);
        live = (live & ~def) | use;
    }
    return live;
}

static int canonical_body_successor(uint64_t rip)
{
    X86Insn insn;
    volatile uint64_t pc = rip;
    volatile int compatible = 0;
    sigjmp_buf db;
    sigjmp_buf *prev = ocerz_jit_decode_recover;
    if (sigsetjmp(db, 1) == 0) {
        ocerz_jit_decode_recover = &db;
        for (int n = 0; n < JIT_MAX_BLOCK_INSNS; n++) {
            if (ocerz_decode((const uint8_t *)ocerz_g2h(pc), 15, pc,
                             &insn) != OCERZ_OK)
                break;
            if (is_terminator(insn.op)) {
                compatible = insn.op == OCERZ_OP_JCC ||
                    insn.op == OCERZ_OP_JMP;   /* direct or indirect */
                break;
            }
            pc += insn.len;
        }
    }
    ocerz_jit_decode_recover = prev;
    return compatible;
}

static unsigned decoded_terminator(uint64_t rip)
{
    X86Insn insn;
    volatile uint64_t pc = rip;
    volatile unsigned term = 0;
    sigjmp_buf db;
    sigjmp_buf *prev = ocerz_jit_decode_recover;
    if (sigsetjmp(db, 1) == 0) {
        ocerz_jit_decode_recover = &db;
        for (int n = 0; n < JIT_MAX_BLOCK_INSNS; n++) {
            if (ocerz_decode((const uint8_t *)ocerz_g2h(pc), 15, pc,
                             &insn) != OCERZ_OK)
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

static int call_body_successor(uint64_t rip)
{
    unsigned term = decoded_terminator(rip);
    return term == OCERZ_OP_CALL || term == OCERZ_OP_RET;
}

static int decoded_call_region_entry(uint64_t rip)
{
    X86Insn insn;
    volatile uint64_t pc = rip;
    volatile int compatible = 0;
    volatile int rsp_ok = 1;
    sigjmp_buf db;
    sigjmp_buf *prev = ocerz_jit_decode_recover;
    if (sigsetjmp(db, 1) == 0) {
        ocerz_jit_decode_recover = &db;
        for (int n = 0; n < JIT_MAX_BLOCK_INSNS; n++) {
            if (ocerz_decode((const uint8_t *)ocerz_g2h(pc), 15, pc,
                             &insn) != OCERZ_OK)
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

static uint64_t xlive_succ_live(OcerzJit *jit, uint64_t rip)
{
    JitBlock *t = cache_lookup(jit, rip);
    return (t && t->code) ? (uint64_t)t->entry_live : xlive_decode_entry(rip);
}

static void emit_slowcall(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);

#define RF_OFF ((uint32_t)offsetof(OcerzCPU, rflags))
#define RIP_OFF ((uint32_t)offsetof(OcerzCPU, rip))

#define INT_OFF ((uint32_t)offsetof(OcerzCPU, interrupt))
#define GPR_OFF(r) ((uint32_t)((unsigned)(r) * 8))

#define CC_SRC_OFF ((uint32_t)offsetof(OcerzCPU, cc_src))
#define CC_DST_OFF ((uint32_t)offsetof(OcerzCPU, cc_dst))
#define CC_OP_OFF ((uint32_t)offsetof(OcerzCPU, cc_op))

#define RAS_TOP_OFF ((uint32_t)offsetof(OcerzCPU, ras_top))
#define RAS_OFF ((uint32_t)offsetof(OcerzCPU, ras))

enum { JT0 = 9, JT1 = 10, JT2 = 11, JTF = 12, JTT = 13, JTU = 14, JTA = 15 };

#define JRET_GUEST 27
#define JRET_HOST  28

static void emit_pf(A64Buf *b, int res)
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

static void emit_zf_sf(A64Buf *b, uint64_t m)
{
    if (m & OCERZ_ZF) {
        a64_cset(b, JTT, A64_EQ);
        a64_lsl_imm(b, 0, JTT, JTT, 6);
        a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
    }
    if (m & OCERZ_SF) {
        a64_cset(b, JTT, A64_MI);
        a64_lsl_imm(b, 0, JTT, JTT, 7);
        a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
    }
}

static void emit_commit_flags(A64Buf *b, uint64_t clear_mask)
{
    a64_ldr(b, 8, JTT, 20, RF_OFF);
    a64_mov_imm64(b, JTU, ~clear_mask);
    a64_and_reg(b, 1, JTT, JTT, JTU, 0);
    a64_orr_reg(b, 1, JTT, JTT, JTF, 0);
    a64_str(b, 8, JTT, 20, RF_OFF);
}

#define JIT_ARITH_FLAGS (OCERZ_CF | OCERZ_PF | OCERZ_AF | OCERZ_ZF | OCERZ_SF | OCERZ_OF)

static void emit_defer_flags(A64Buf *b, uint32_t ccop, int src_reg, int dst_reg)
{
    _Static_assert(CC_DST_OFF == CC_SRC_OFF + 8,
                   "deferred flag operands must remain adjacent");
    a64_stp_off(b, src_reg, dst_reg, 20, CC_SRC_OFF);
    a64_mov_imm64(b, JTT, ccop);
    a64_str(b, 4, JTT, 20, CC_OP_OFF);
}

static void emit_xmm_pin_load_all(A64Buf *b);
static void emit_xmm_pin_spill_all(A64Buf *b);
static void emit_spill_pinned_callersaved(A64Buf *b);
static void emit_fill_pinned_callersaved(A64Buf *b);
static void emit_materialize(A64Buf *b)
{

    if (!g_defer)
        return;
    a64_ldr(b, 4, JT0, 20, CC_OP_OFF);
    uint32_t *skip = a64_label(b);
    a64_cbz(b, 0, JT0, 0);
    emit_xmm_pin_spill_all(b);           /* C clobbers V16-V31 */
    emit_spill_pinned_callersaved(b);    /* and x1-x8 pin slots */
    a64_mov_reg(b, 1, 0, 20);
    a64_mov_imm64(b, 16, (uint64_t)(uintptr_t)&ocerz_flags_materialize);
    a64_blr(b, 16);
    emit_fill_pinned_callersaved(b);
    emit_reload_mem_base(b);
    emit_xmm_pin_load_all(b);
    a64_patch_cbz(skip, a64_label(b));
}

static inline int pin_slot(unsigned greg)
{
    return (g_pin && greg < 16) ? g_pin[greg] : -1;
}

/* Host register holding pin slot `slot`.  Slots 0-7 -> x21-x28 (callee-
 * saved), 8-13 -> x3-x8, 14-15 -> x1-x2 (caller-saved: spilled/reloaded
 * around every C callout by emit_spill_pinned/emit_fill_pinned). */
/* Class 3 = every guest GPR permanently pinned (slot == guest reg number).
 * All blocks share the layout, so every transition is body-to-body. */
static int fullpin_enabled(void)
{
    static int on = -1;
    if (on < 0) on = getenv("OCERZ_NO_FULLPIN") ? 0 : 1;
    return on;
}
static inline int pin_hreg(int slot)
{
    if (slot < 8)  return 21 + slot;
    if (slot < 14) return 3 + (slot - 8);
    return 1 + (slot - 14);
}

static inline int body_edge_pin_class(void)
{
    if (g_pin_class)
        return g_pin_class;
    return g_n_pinned == 0 ? 0 : -1;
}

static void emit_gpr_rd(A64Buf *b, int sf, int dst, unsigned greg)
{
    int s = pin_slot(greg);
    if (s >= 0 && g_pin_class == 2 && greg == OCERZ_RSP) {

        a64_mov_imm64(b, dst, ocerz_guest_base);
        a64_sub_reg(b, 1, dst, pin_hreg(s), dst, 0);
        if (!sf)
            a64_mov_reg(b, 0, dst, dst);
    } else if (s >= 0)
        a64_mov_reg(b, sf, dst, pin_hreg(s));
    else
        a64_ldr(b, sf ? 8 : 4, dst, 20, GPR_OFF(greg));
}

static void emit_gpr_rd_sw(A64Buf *b, int dst, unsigned greg)
{
    int s = pin_slot(greg);
    if (s >= 0)
        a64_sxtw(b, dst, pin_hreg(s));
    else
        a64_ldrsw(b, dst, 20, GPR_OFF(greg));
}

static void emit_gpr_wr(A64Buf *b, int src, unsigned greg)
{
    int s = pin_slot(greg);
    if (s >= 0 && g_pin_class == 2 && greg == OCERZ_RSP) {
        int tmp = src == JTU ? JTA : JTU;
        a64_mov_imm64(b, tmp, ocerz_guest_base);
        a64_add_reg(b, 1, pin_hreg(s), src, tmp, 0);
    } else if (s >= 0)
        a64_mov_reg(b, 1, pin_hreg(s), src);
    else
        a64_str(b, 8, src, 20, GPR_OFF(greg));
}

static void emit_spill_pinned(A64Buf *b)
{
    for (int i = 0; i < g_n_pinned; i++) {
        if (g_pin_class == 2 && g_pin_hold[i] == OCERZ_RSP) {
            a64_mov_imm64(b, JTA, ocerz_guest_base);
            a64_sub_reg(b, 1, JTA, pin_hreg(i), JTA, 0);
            a64_str(b, 8, JTA, 20, GPR_OFF(OCERZ_RSP));
        } else {
            a64_str(b, 8, pin_hreg(i), 20, GPR_OFF(g_pin_hold[i]));
        }
    }
}

/* Spill/fill only the pin slots that live in caller-saved host registers
 * (slot >= 8): used around C calls that leave x21-x28 intact. */
static void emit_spill_pinned_callersaved(A64Buf *b)
{
    for (int i = 8; i < g_n_pinned; i++)
        a64_str(b, 8, pin_hreg(i), 20, GPR_OFF(g_pin_hold[i]));
}
static void emit_fill_pinned_callersaved(A64Buf *b)
{
    for (int i = 8; i < g_n_pinned; i++)
        a64_ldr(b, 8, pin_hreg(i), 20, GPR_OFF(g_pin_hold[i]));
}

static void emit_fill_pinned(A64Buf *b)
{
    for (int i = 0; i < g_n_pinned; i++)
        a64_ldr(b, 8, pin_hreg(i), 20, GPR_OFF(g_pin_hold[i]));
    if (g_pin_class == 2) {
        int s = pin_slot(OCERZ_RSP);
        assert(s >= 0);
        a64_mov_imm64(b, JTA, ocerz_guest_base);
        a64_add_reg(b, 1, pin_hreg(s), pin_hreg(s), JTA, 0);
    }
}

/* Only the callee-saved slots (0-7 -> x21-x28) are saved/restored around a
 * block; slots >= 8 live in caller-saved registers. */
static inline int pin_saved_count(void) { return g_n_pinned < 8 ? g_n_pinned : 8; }

static void emit_pin_prologue(A64Buf *b)
{
    int ns = pin_saved_count();
    for (int i = 0; i < ns; i += 2)
        a64_stp_pre(b, 21 + i, 21 + i + 1, 31, -16);
    for (int i = 0; i < g_n_pinned; i++)
        a64_ldr(b, 8, pin_hreg(i), 20, GPR_OFF(g_pin_hold[i]));
    if (g_pin_class == 2) {
        a64_stp_pre(b, JRET_GUEST, JRET_HOST, 31, -16);
        a64_mov_imm64(b, JRET_HOST, 0);
    }
}

static void emit_pin_epilogue_restore(A64Buf *b)
{
    if (g_pin_class == 2)
        a64_ldp_post(b, JRET_GUEST, JRET_HOST, 31, 16);
    int ns = pin_saved_count();
    int last = (ns & 1) ? ns - 1 : ns - 2;
    for (int i = last; i >= 0; i -= 2)
        a64_ldp_post(b, 21 + i, 21 + i + 1, 31, 16);
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

static int emit_arith(A64Buf *b, const X86Insn *insn, uint64_t need)
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

    if (!writes && need == 0)
        return 1;

    if (writes && need == 0) {
        int ds = pin_slot(d->reg);
        int rd = ds >= 0 ? pin_hreg(ds) : JT2;
        int rn = ds >= 0 ? rd : JT0;
        int rm;

        if (ds < 0)
            emit_gpr_rd(b, sf, JT0, d->reg);
        if (s->kind == OCERZ_OPK_REG) {
            if (s->high8)
                return 0;
            int ss = pin_slot(s->reg);
            if (ss >= 0)
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

static int emit_mem_ea(A64Buf *b, const X86Insn *insn, const X86Operand *op, int addr_reg);
static void emit_add_const(A64Buf *b, int reg, uint64_t c);
static inline uint64_t ea_fold(void);
static uint32_t *emit_commpage_guard(A64Buf *b, const X86Insn *insn,
                                     int addr_reg, uint32_t **exit_sites, int *n_exits);
static inline void patch_guard_skip(uint32_t *skip, uint32_t *target);
static void emit_guest_load_ordered(A64Buf *b, int size, int rd, int ra, int scratch);

static int emit_cmp_test_narrow(A64Buf *b, const X86Insn *insn, uint64_t need,
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
    /* memory forms only on the deferred-flags path (fault sites need the
     * generic exit protocol; the deferred path never traps after the load) */
    if ((d_mem || s_mem) && (!g_defer || insn->seg != OCERZ_SEG_NONE))
        return 0;

    if (!g_defer && need == 0)
        return 1;
    if ((d_mem || s_mem) && need == 0)
        return 1;                       /* compare with dead flags: nothing to do */

    int is_sub = (op == OCERZ_OP_CMP);
    int size = d->size;
    uint64_t mask = (size == 1) ? 0xffull : 0xffffull;
    int sh = 32 - 8 * size;

    /* memory operand FIRST: emit_mem_ea clobbers JT0 while forming the EA. */
    if (s_mem) {
        if (!emit_mem_ea(b, insn, s, JTA)) return 0;
        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
        emit_guest_load_ordered(b, size, JT1, JTA, JTU);
        patch_guard_skip(skip, a64_label(b));
        if (size == 1) a64_uxtb(b, JT1, JT1); else a64_uxth(b, JT1, JT1);
    }
    if (d_mem) {
        if (!emit_mem_ea(b, insn, d, JTA)) return 0;
        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
        emit_guest_load_ordered(b, size, JT0, JTA, JTU);
        patch_guard_skip(skip, a64_label(b));
        if (size == 1) a64_uxtb(b, JT0, JT0); else a64_uxth(b, JT0, JT0);
    } else {
        emit_gpr_rd(b, 1, JT0, d->reg);
        if (size == 1) a64_uxtb(b, JT0, JT0); else a64_uxth(b, JT0, JT0);
    }
    if (s->kind == OCERZ_OPK_REG) {
        emit_gpr_rd(b, 1, JT1, s->reg);
        if (size == 1) a64_uxtb(b, JT1, JT1); else a64_uxth(b, JT1, JT1);
    } else if (!s_mem) {

        a64_mov_imm64(b, JT1, (uint64_t)s->imm & mask);
    }

    if (g_defer) {
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

static void emit_cc_predicate(A64Buf *b, unsigned cc);
static int emit_incdec(A64Buf *b, const X86Insn *insn, uint64_t need)
{
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

    /* CF is preserved by inc/dec: fetch it inline from the pending record
     * (or RFLAGS) without a C materialize call. */
    emit_cc_predicate(b, OCERZ_CC_B);        /* NE <=> CF */
    a64_cset(b, JT0, A64_NE);
    emit_gpr_rd(b, 1, JT1, d->reg);
    emit_defer_flags(b, ocerz_cc_pack(is_inc ? OCERZ_CC_INC : OCERZ_CC_DEC,
                                      d->size, 0), JT0, JT1);
    return 1;
}

static int emit_mov_logic_pair(A64Buf *b, const X86Insn *mov,
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

static int emit_add_inc_pair(A64Buf *b, const X86Insn *add,
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

static int emit_shift(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    if (!g_defer)
        return emit_shift_eager(b, insn, need);
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
    if (need == 0) {
        int ds = pin_slot(d->reg);
        if (ds >= 0) {
            int rd = pin_hreg(ds);
            switch (op) {
            case OCERZ_OP_SHL: a64_lsl_imm(b, sf, rd, rd, (int)cnt); break;
            case OCERZ_OP_SHR: a64_lsr_imm(b, sf, rd, rd, (int)cnt); break;
            case OCERZ_OP_SAR: a64_asr_imm(b, sf, rd, rd, (int)cnt); break;
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

static int emit_imul_src(A64Buf *b, const X86Operand *op, int dst)
{
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

static int emit_imul(A64Buf *b, const X86Insn *insn, uint64_t need)
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

    if (need == 0) {
        int ds = pin_slot(d->reg);
        if (ds >= 0) {
            int src[2];
            const X86Operand *ops[2] = { s1, s2 };
            for (int i = 0; i < 2; i++) {
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
                } else {
                    return 0;
                }
            }
            a64_mul(b, sf, pin_hreg(ds), src[0], src[1]);
            return 1;
        }
    }

    a64_str(b, 4, A64_ZR, 20, CC_OP_OFF);
    if (!emit_imul_src(b, s1, JT0))
        return 0;
    if (!emit_imul_src(b, s2, JT1))
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

    if (need & (OCERZ_ZF | OCERZ_SF))
        a64_subs_imm(b, sf, A64_ZR, JT2, 0);
    a64_mov_imm64(b, JTF, 0);
    emit_zf_sf(b, need);
    if (need & OCERZ_PF)
        emit_pf(b, JT2);
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

static int mem_native_store_ok(void)
{
    return ocerz_watch_addr == 0 && ocerz_watch_val == 0;
}

static inline uint64_t ea_fold(void)
{
    return ocerz_low_base ? 0 : ocerz_guest_base;
}

static void emit_add_const(A64Buf *b, int reg, uint64_t c)
{
    if (c) {
        a64_mov_imm64(b, JTU, c);
        a64_add_reg(b, 1, reg, reg, JTU, 0);
    }
}

static int emit_mem_ea(A64Buf *b, const X86Insn *insn, const X86Operand *op, int addr_reg)
{
    uint64_t fold = ea_fold();
    int seg = insn->seg;
    if (seg != OCERZ_SEG_NONE) {

        static int no_seg = -1;
        if (no_seg < 0)
            no_seg = getenv("OCERZ_NO_INLINE_SEG") ? 1 : 0;
        if (no_seg || op->riprel || insn->addrsize == 4 || ocerz_low_base != 0)
            return 0;
    }
    if (op->riprel) {
        a64_mov_imm64(b, addr_reg, (uint64_t)op->disp + fold);
        return 1;
    }
    if (insn->addrsize == 4)
        return 0;
    uint64_t initial = (uint64_t)op->disp + fold;
    if (g_pin_class == 2 && op->base == OCERZ_RSP &&
        pin_slot(OCERZ_RSP) >= 0)
        initial = (uint64_t)op->disp;
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
    if (op->index != OCERZ_REG_NONE) {
        int s = pin_slot(op->index);
        if (s >= 0 && !(g_pin_class == 2 && op->index == OCERZ_RSP))
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
    }
    return 1;
}

static uint32_t *emit_commpage_guard(A64Buf *b, const X86Insn *insn,
                                     int addr_reg, uint32_t **exit_sites, int *n_exits)
{

    if (!ocerz_commpage && !ocerz_low_base)
        return NULL;

    uint64_t fold = ea_fold();
    uint32_t *to_native = NULL;
    if (ocerz_low_base) {
        a64_mov_imm64(b, JTU, OCERZ_LOW_LIMIT);
        a64_sub_reg(b, 1, JTT, addr_reg, JTU, 0);
        a64_mov_imm64(b, JTU, OCERZ_TOP_LO - OCERZ_LOW_LIMIT);
        a64_subs_reg(b, 1, A64_ZR, JTT, JTU, 0);
        to_native = a64_label(b);
        a64_bcond(b, A64_CC, 0);
    }
    a64_mov_imm64(b, JTU, OCERZ_COMMPAGE_LO + fold);
    a64_sub_reg(b, 1, JTT, addr_reg, JTU, 0);
    a64_mov_imm64(b, JTU, OCERZ_COMMPAGE_HI - OCERZ_COMMPAGE_LO);
    a64_subs_reg(b, 1, A64_ZR, JTT, JTU, 0);
    uint32_t *over = a64_label(b);
    a64_bcond(b, A64_CS, 0);
    uint32_t *slow = a64_label(b);
    emit_slowcall(b, insn, exit_sites, n_exits);
    uint32_t *skip = a64_label(b);
    a64_b(b, 0);
    a64_patch_bcond(over, a64_label(b));
    if (ocerz_low_base) {

        a64_mov_imm64(b, JTU, OCERZ_TOP_LO);
        a64_subs_reg(b, 1, A64_ZR, addr_reg, JTU, 0);
        uint32_t *cur = a64_label(b);
        a64_bcond(b, A64_CS, (int32_t)(slow - cur));

        a64_mov_imm64(b, JTU, OCERZ_LOW_LIMIT);
        a64_subs_reg(b, 1, A64_ZR, addr_reg, JTU, 0);
        uint32_t *ge_low = a64_label(b);
        a64_bcond(b, A64_CS, 0);
        a64_mov_imm64(b, JTU, ocerz_low_base - ocerz_guest_base);
        a64_add_reg(b, 1, addr_reg, addr_reg, JTU, 0);
        a64_patch_bcond(ge_low, a64_label(b));
    }
    if (to_native)
        a64_patch_bcond(to_native, a64_label(b));
    return skip;
}

static inline void patch_guard_skip(uint32_t *skip, uint32_t *target)
{
    if (skip)
        a64_patch_b(skip, target);
}

static void emit_reload_mem_base(A64Buf *b)
{
    if (g_mem_hoist_greg < 0)
        return;
    int bs = pin_slot(g_mem_hoist_greg);
    assert(bs >= 0);
    a64_mov_imm64(b, JMEMBASE, ocerz_guest_base);
    a64_add_reg(b, 1, JMEMBASE, JMEMBASE, pin_hreg(bs), 0);
    if (g_mem_hoist_aux_disp > 0)
        a64_add_imm(b, 1, JMEMAUX, JMEMBASE,
                    (uint32_t)g_mem_hoist_aux_disp);
    else if (g_mem_hoist_aux_disp < 0)
        a64_sub_imm(b, 1, JMEMAUX, JMEMBASE,
                    (uint32_t)-g_mem_hoist_aux_disp);
}

static int emit_hoisted_mem_access(A64Buf *b, const X86Insn *insn,
                                   const X86Operand *mem, int size,
                                   int value_reg, int store)
{
    if (!g_plain_mem || g_mem_hoist_greg < 0 ||
        insn->seg != OCERZ_SEG_NONE || insn->addrsize != 8 || mem->riprel ||
        mem->base != g_mem_hoist_greg)
        return 0;

    int64_t disp = mem->disp;
    if (mem->index == OCERZ_REG_NONE) {
        if (disp < 0 || (uint64_t)disp > (uint64_t)4095 * (uint64_t)size ||
            (disp & (size - 1)) != 0)
            return 0;
        if (store)
            a64_str(b, size, value_reg, JMEMBASE, (uint32_t)disp);
        else
            a64_ldr(b, size, value_reg, JMEMBASE, (uint32_t)disp);
        return 1;
    }

    int is = pin_slot(mem->index);
    int want_scale = size == 8 ? 3 : size == 4 ? 2 :
                     size == 2 ? 1 : 0;
    if (is < 0 || (mem->scale & 3) != want_scale ||
        disp < -4095 || disp > 4095)
        return 0;
    int base = JMEMBASE;
    if (disp != 0 && disp == g_mem_hoist_aux_disp) {
        base = JMEMAUX;
    } else if (disp != 0) {
        if (disp > 0)
            a64_add_imm(b, 1, JTA, JMEMBASE, (uint32_t)disp);
        else
            a64_sub_imm(b, 1, JTA, JMEMBASE, (uint32_t)-disp);
        base = JTA;
    }
    if (store)
        a64_str_regoff(b, size, value_reg, base, pin_hreg(is), 1);
    else
        a64_ldr_regoff(b, size, value_reg, base, pin_hreg(is), 1);
    return 1;
}

static void emit_guest_store_ordered(A64Buf *b, int size, int rv, int ra, int scratch)
{
    if (g_plain_mem) {
        a64_str(b, size, rv, ra, 0);
        return;
    }

    if (size == 1) {
        a64_stlr(b, 1, rv, ra);
        return;
    }
    if (!a64_try_ands_imm(b, 1, A64_ZR, ra, (uint64_t)(size - 1))) {
        a64_mov_imm64(b, scratch, (uint64_t)(size - 1));
        a64_ands_reg(b, 1, A64_ZR, ra, scratch, 0);
    }
    if (!g_no_oolslow && g_n_oslow < OSLOW_MAX) {
        uint32_t *bne = a64_label(b);
        a64_bcond(b, A64_NE, 0);
        a64_stlr(b, size, rv, ra);
        g_oslow[g_n_oslow] = (OrderedSlowPend){ bne, a64_label(b), size, rv, ra, 1,
                                                g_cur_insn_idx };
        g_n_oslow++;
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

static void emit_guest_load_ordered(A64Buf *b, int size, int rd, int ra, int scratch)
{
    if (g_plain_mem) {
        a64_ldr(b, size, rd, ra, 0);
        return;
    }
    if (size == 1) {
        if (g_no_ldapr) a64_ldar(b, 1, rd, ra);
        else            a64_ldapr(b, 1, rd, ra);
        return;
    }
    if (!a64_try_ands_imm(b, 1, A64_ZR, ra, (uint64_t)(size - 1))) {
        a64_mov_imm64(b, scratch, (uint64_t)(size - 1));
        a64_ands_reg(b, 1, A64_ZR, ra, scratch, 0);
    }
    if (!g_no_oolslow && g_n_oslow < OSLOW_MAX) {
        uint32_t *bne = a64_label(b);
        a64_bcond(b, A64_NE, 0);
        if (g_no_ldapr) a64_ldar(b, size, rd, ra);
        else            a64_ldapr(b, size, rd, ra);
        g_oslow[g_n_oslow] = (OrderedSlowPend){ bne, a64_label(b), size, rd, ra, 0,
                                                g_cur_insn_idx };
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

static int emit_mov_mem(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    uint64_t gbase = ocerz_guest_base;

    if (d->kind == OCERZ_OPK_MEM && s->kind == OCERZ_OPK_REG) {
        if (s->high8 || (s->size != 4 && s->size != 8))
            return 0;
        if (!mem_native_store_ok())
            return 0;
        int ss = pin_slot(s->reg);
        int rv = ss >= 0 ? pin_hreg(ss) : JT1;
        if (ss >= 0 && emit_hoisted_mem_access(b, insn, d, s->size, rv, 1))
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
    if (d->kind == OCERZ_OPK_REG && s->kind == OCERZ_OPK_MEM) {
        if (d->high8 || (d->size != 4 && d->size != 8))
            return 0;
        int ds = pin_slot(d->reg);
        int rd = ds >= 0 ? pin_hreg(ds) : JT1;
        if (emit_hoisted_mem_access(b, insn, s, d->size, rd, 0))
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

static int emit_movx(A64Buf *b, const X86Insn *insn, int is_signed,
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

static int stack_inline_enabled(void)
{
    static int en = -1;
    if (en < 0)
        en = getenv("OCERZ_NO_INLINE_STACK") ? 0 : 1;
    return en;
}

static int emit_push_pop(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *o = &insn->ops[0];
    uint64_t gbase = ocerz_guest_base;

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
            if (g_plain_mem && !ocerz_commpage) {
                a64_str_pre64(b, rv, pin_hreg(rs), -8);
            } else {
                a64_sub_imm(b, 1, JTA, pin_hreg(rs), 8);
                skip = emit_commpage_guard(b, insn, JTA,
                                           exit_sites, n_exits);
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

        emit_guest_store_ordered(b, 8, JT1, JTA, JTU);
        a64_sub_imm(b, 1, JT0, JT0, 8);
        emit_gpr_wr(b, JT0, OCERZ_RSP);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }

    if (insn->op == OCERZ_OP_POP) {
        if (o->kind != OCERZ_OPK_REG || o->high8 || o->size != 8)
            return 0;

        if (g_pin_class == 2) {
            int rs = pin_slot(OCERZ_RSP);
            int ds = o->reg == OCERZ_RSP ? -1 : pin_slot(o->reg);
            int rd = ds >= 0 ? pin_hreg(ds) : JT1;
            assert(rs >= 0);
            uint32_t *skip = NULL;
            if (g_plain_mem && !ocerz_commpage) {
                a64_ldr_post64(b, rd, pin_hreg(rs), 8);
            } else {
                skip = emit_commpage_guard(b, insn, pin_hreg(rs),
                                           exit_sites, n_exits);
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
        emit_guest_load_ordered(b, 8, JT1, JTA, JTU);

        a64_add_imm(b, 1, JT0, JT0, 8);
        emit_gpr_wr(b, JT0, OCERZ_RSP);
        emit_gpr_wr(b, JT1, o->reg);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    return 0;
}

static int emit_movsxd(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];

    if (d->kind != OCERZ_OPK_REG || d->high8 || d->size != 8)
        return 0;
    if (s->size != 4)
        return 0;

    if (s->kind == OCERZ_OPK_REG) {
        if (s->high8)
            return 0;

        emit_gpr_rd(b, 0, JT0, s->reg);
        a64_sxtw(b, JT0, JT0);
        emit_gpr_wr(b, JT0, d->reg);
        return 1;
    }
    if (s->kind == OCERZ_OPK_MEM) {
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

static int emit_arith_mem(A64Buf *b, const X86Insn *insn, uint64_t need,
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

    if (!emit_mem_ea(b, insn, s, JTA))
        return 0;
    uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
    emit_add_const(b, JTA, ocerz_guest_base - ea_fold());

    emit_guest_load_ordered(b, sf ? 8 : 4, JT1, JTA, JTU);
    emit_gpr_rd(b, sf, JT0, d->reg);

    unsigned op = insn->op;
    int is_sub = (op == OCERZ_OP_SUB || op == OCERZ_OP_CMP);
    int is_add = (op == OCERZ_OP_ADD);
    int is_logic = (op == OCERZ_OP_AND || op == OCERZ_OP_OR ||
                    op == OCERZ_OP_XOR || op == OCERZ_OP_TEST);
    int writes = (op == OCERZ_OP_ADD || op == OCERZ_OP_SUB ||
                  op == OCERZ_OP_AND || op == OCERZ_OP_OR || op == OCERZ_OP_XOR);

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

static int emit_lea(A64Buf *b, const X86Insn *insn)
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
    int host_rsp_operand = g_pin_class == 2 &&
        (s->base == OCERZ_RSP || s->index == OCERZ_RSP);
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


/* ---- batch-1 GPR emitters: not/neg/rol/ror/shift-by-cl/cmovcc/setcc/bswap ---- */

/* Load a 4/8-byte register operand into host reg dst; returns 0 if unsupported. */

/* Compute the x86 condition `cc` from (materialized) RFLAGS into JTF as 0/1
 * and set host NZCV so that NE == condition true.  Mirrors emit_jcc. */
/* JTF = x86 condition `cc` (0/1) evaluated from RFLAGS (must be materialized). */
static void emit_cc_predicate_rflags(A64Buf *b, unsigned cc)
{
    a64_ldr(b, 8, JT0, 20, RF_OFF);
    a64_ubfx(b, 1, JT1, JT0, 0, 1);    /* CF */
    a64_ubfx(b, 1, JTA, JT0, 6, 1);    /* ZF */
    a64_ubfx(b, 1, JTT, JT0, 7, 1);    /* SF */
    a64_ubfx(b, 1, JTU, JT0, 11, 1);   /* OF */
    switch (cc >> 1) {
    case 0: a64_mov_reg(b, 1, JTF, JTU); break;                 /* O  */
    case 1: a64_mov_reg(b, 1, JTF, JT1); break;                 /* B  */
    case 2: a64_mov_reg(b, 1, JTF, JTA); break;                 /* E  */
    case 3: a64_orr_reg(b, 1, JTF, JT1, JTA, 0); break;         /* BE */
    case 4: a64_mov_reg(b, 1, JTF, JTT); break;                 /* S  */
    case 5: a64_ubfx(b, 1, JTF, JT0, 2, 1); break;              /* P  */
    case 6: a64_eor_reg(b, 1, JTF, JTT, JTU, 0); break;         /* L  */
    default:
        a64_eor_reg(b, 1, JTF, JTT, JTU, 0);                    /* LE */
        a64_orr_reg(b, 1, JTF, JTF, JTA, 0);
        break;
    }
    if (cc & 1) {  /* negated form: predicate = !JTF */
        a64_mov_imm64(b, JTU, 1);
        a64_eor_reg(b, 1, JTF, JTF, JTU, 0);
    }
}

/* x86 cc -> arm64 cond after `subs` (CF=!C) ; -1 = needs PF (not derivable) */
static int cc_after_subs(unsigned cc)
{
    static const int t[16] = { A64_VS, A64_VC, A64_CC, A64_CS, A64_EQ, A64_NE, A64_LS, A64_HI,
                               A64_MI, A64_PL, -1, -1, A64_LT, A64_GE, A64_LE, A64_GT };
    return cc < 16 ? t[cc] : -1;
}
/* x86 cc -> arm64 cond after `ands` (CF=OF=0): B/O never, AE/NO always */
static int cc_after_ands(unsigned cc)
{
    switch (cc) {
    case OCERZ_CC_O: case OCERZ_CC_B: return A64_NV;   /* never  */
    case OCERZ_CC_NO: case OCERZ_CC_AE: return A64_AL;  /* always */
    case OCERZ_CC_E: return A64_EQ;  case OCERZ_CC_NE: return A64_NE;
    case OCERZ_CC_BE: return A64_EQ; case OCERZ_CC_A: return A64_NE;   /* CF=0 -> BE==ZF, A==!ZF */
    case OCERZ_CC_S: return A64_MI;  case OCERZ_CC_NS: return A64_PL;
    case OCERZ_CC_L: return A64_MI;  case OCERZ_CC_GE: return A64_PL;  /* OF=0 -> SF */
    case OCERZ_CC_LE: return A64_LE; case OCERZ_CC_G: return A64_GT;   /* Z||N ; !Z&&!N (V=0) */
    default: return -1;                                                 /* P/NP */
    }
}

/* Set host NZCV so that NE <=> x86 condition `cc` holds.  Fast paths
 * evaluate a pending deferred CMP/SUB or logic record inline (recompute
 * subs/ands on cc_src/cc_dst); anything else materializes through C and
 * reads RFLAGS.  Uses JT0, JT1, JTA, JTT, JTU, JTF; JT2 is untouched. */
/* Deferred-record kind/size the producer would leave if it went through the
 * deferred path (0 = unknown / not a simple record). */
static unsigned producer_record_kind(const X86Insn *p, int *size)
{
    if (!p) return 0;
    switch (p->op) {
    case OCERZ_OP_CMP: case OCERZ_OP_SUB: *size = p->ops[0].size; return OCERZ_CC_SUB;
    case OCERZ_OP_TEST: case OCERZ_OP_AND: case OCERZ_OP_OR: case OCERZ_OP_XOR:
        *size = p->ops[0].size; return OCERZ_CC_LOGIC;
    case OCERZ_OP_ADD: *size = p->ops[0].size; return OCERZ_CC_ADD;
    case OCERZ_OP_SHL: case OCERZ_OP_SHR: case OCERZ_OP_SAR:
        /* constant-count register shifts defer a {val, cnt} record */
        if (p->ops[1].kind == OCERZ_OPK_IMM && p->ops[0].kind == OCERZ_OPK_REG &&
            (p->ops[0].size == 4 || p->ops[0].size == 8)) {
            *size = p->ops[0].size;
            return p->op == OCERZ_OP_SHL ? OCERZ_CC_SHL : p->op == OCERZ_OP_SHR ? OCERZ_CC_SHR : OCERZ_CC_SAR;
        }
        return 0;
    default: return 0;
    }
}
/* after `adds`: CF = C (no inversion), same table as subs otherwise except B/AE */
static int cc_after_adds(unsigned cc)
{
    static const int t[16] = { A64_VS, A64_VC, A64_CS, A64_CC, A64_EQ, A64_NE, -1, -1,
                               A64_MI, A64_PL, -1, -1, A64_LT, A64_GE, A64_LE, A64_GT };
    return cc < 16 ? t[cc] : -1;   /* BE/A need CF|ZF: -1 (generic) */
}

static void emit_cc_predicate(A64Buf *b, unsigned cc)
{
    if (getenv("OCERZ_CCLOG")) {
        char tb[96] = "(none)";
        if (g_flag_producer) ocerz_format_insn(g_flag_producer, tb, sizeof tb);
        fprintf(stderr, "ocerz: CCPRED cc=%u producer=%s\n", cc, tb);
    }
    uint32_t *to_generic[8]; int ng = 0;
    uint32_t *done[4]; int nd = 0;
    int c_sub = cc_after_subs(cc), c_and = cc_after_ands(cc);
    int psize = 0;
    unsigned pkind = producer_record_kind(g_flag_producer, &psize);
    int c_add = cc_after_adds(cc);
    /* SHIFT records: {src=val, dst=cnt}; only ZF/SF-based conditions are cheap
     * (E/NE/S/NS/L/GE need OF too -> only E/NE/S/NS here) */
    int shift_ok = (pkind == OCERZ_CC_SHL || pkind == OCERZ_CC_SHR || pkind == OCERZ_CC_SAR) &&
                   (cc == OCERZ_CC_E || cc == OCERZ_CC_NE || cc == OCERZ_CC_S || cc == OCERZ_CC_NS);
    if (g_defer && shift_ok && g_flag_producer) {
        unsigned cnt = (unsigned)(g_flag_producer->ops[1].imm & (psize == 8 ? 63u : 31u));
        int sf = psize == 8;
        a64_ldr(b, 4, JT0, 20, CC_OP_OFF);
        uint32_t *to_rf = a64_label(b); a64_cbz(b, 0, JT0, 0);
        a64_ldr(b, 8, JT1, 20, CC_SRC_OFF);                 /* val */
        if (pkind == OCERZ_CC_SHL) a64_lsl_imm(b, sf, JT1, JT1, (int)cnt);
        else if (pkind == OCERZ_CC_SHR) a64_lsr_imm(b, sf, JT1, JT1, (int)cnt);
        else a64_asr_imm(b, sf, JT1, JT1, (int)cnt);
        a64_ands_reg(b, sf, A64_ZR, JT1, JT1, 0);           /* Z, N of the result */
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
    /* Producers that write RFLAGS eagerly (no deferred record): comis/ucomis,
     * rotates.  Flags are already current -> just read them (~8 words). */
    if (g_flag_producer && (g_flag_producer->op == OCERZ_OP_UCOMISD ||
                            g_flag_producer->op == OCERZ_OP_UCOMISS ||
                            g_flag_producer->op == OCERZ_OP_COMISD ||
                            g_flag_producer->op == OCERZ_OP_COMISS)) {
        /* still cheap-guard: if the producer went slow it also materialized. */
        emit_cc_predicate_rflags(b, cc);
        a64_subs_imm(b, 1, A64_ZR, JTF, 0);
        return;
    }
    if (g_defer && pkind && cc != OCERZ_CC_P && cc != OCERZ_CC_NP &&
        (psize == 1 || psize == 2 || psize == 4 || psize == 8) &&
        ((pkind == OCERZ_CC_SUB && c_sub >= 0) || (pkind == OCERZ_CC_LOGIC && c_and >= 0))) {
        /* STATIC fast path: the producer is known; only its deferred-vs-
         * materialized state is dynamic (cc_op == 0 means flags are in RFLAGS,
         * e.g. the producer went slow).  ~8 words on the hot path. */
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
        emit_cc_predicate_rflags(b, cc);            /* cc_op == 0: RFLAGS is current */
        a64_patch_b(ready, a64_label(b));
        a64_subs_imm(b, 1, A64_ZR, JTF, 0);
        return;
    }
    if (g_defer && c_sub >= 0 && c_and >= 0 && cc != OCERZ_CC_P && cc != OCERZ_CC_NP) {
        a64_ldr(b, 4, JT0, 20, CC_OP_OFF);
        to_generic[ng++] = a64_label(b); a64_cbz(b, 0, JT0, 0);       /* no pending -> rflags path */
        a64_ldr(b, 8, JT1, 20, CC_SRC_OFF);
        a64_ldr(b, 8, JTA, 20, CC_DST_OFF);
        /* kind = low byte, size = next byte */
        a64_ubfx(b, 0, JTT, JT0, 8, 8);                                  /* size */
        a64_ubfx(b, 0, JTU, JT0, 0, 8);                                  /* kind */
        a64_ubfx(b, 0, JTF, JT0, 16, 1);                                 /* cin */
        to_generic[ng++] = a64_label(b); a64_cbnz(b, 0, JTF, 0);         /* carry-in forms -> generic */
        /* SUB record?  */
        a64_subs_imm(b, 0, A64_ZR, JTU, OCERZ_CC_SUB);
        uint32_t *not_sub = a64_label(b); a64_bcond(b, A64_NE, 0);
        /* extend operands to size and subs (size 8 -> x-form; 4 -> w-form; 2/1 -> shifted w-form) */
        a64_subs_imm(b, 0, A64_ZR, JTT, 8);
        uint32_t *s8 = a64_label(b); a64_bcond(b, A64_EQ, 0);
        a64_subs_imm(b, 0, A64_ZR, JTT, 4);
        uint32_t *s4 = a64_label(b); a64_bcond(b, A64_EQ, 0);
        /* size 1/2: shift left so the top bit is the sign */
        a64_mov_imm64(b, JTF, 32);
        a64_lsl_imm(b, 0, JTT, JTT, 3);                                  /* bits */
        a64_sub_reg(b, 0, JTF, JTF, JTT, 0);                             /* 32-bits */
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
        /* LOGIC record? (result already in cc_dst) */
        a64_patch_bcond(not_sub, a64_label(b));
        a64_subs_imm(b, 0, A64_ZR, JTU, OCERZ_CC_LOGIC);
        to_generic[ng++] = a64_label(b); a64_bcond(b, A64_NE, 0);
        /* set NZ from result at size: shift so sign lands in bit 31/63 */
        a64_subs_imm(b, 0, A64_ZR, JTT, 8);
        uint32_t *l8 = a64_label(b); a64_bcond(b, A64_EQ, 0);
        a64_mov_imm64(b, JTF, 32);
        a64_lsl_imm(b, 0, JTT, JTT, 3);
        a64_sub_reg(b, 0, JTF, JTF, JTT, 0);
        a64_lslv(b, 0, JTA, JTA, JTF);
        a64_ands_reg(b, 0, A64_ZR, JTA, JTA, 0);
        /* logic: condition table differs (CF=OF=0) -> materialize predicate now */
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
        uint32_t *lg_pred = a64_label(b); a64_b(b, 0);       /* -> pred_ready */
        /* SUB fast path lands here: predicate from c_sub */
        for (int i = 0; i < nd; i++) a64_patch_b(done[i], a64_label(b));
        a64_cset(b, JTF, c_sub);
        uint32_t *sub_pred = a64_label(b); a64_b(b, 0);      /* -> pred_ready */
        /* generic path */
        for (int i = 0; i < ng; i++) {
            uint32_t w = *to_generic[i];
            if ((w & 0xff000010u) == 0x54000000u) a64_patch_bcond(to_generic[i], a64_label(b));
            else a64_patch_cbz(to_generic[i], a64_label(b));
        }
        emit_materialize(b);
        emit_cc_predicate_rflags(b, cc);                     /* JTF = predicate */
        /* pred_ready: */
        a64_patch_b(lg_pred, a64_label(b));
        a64_patch_b(sub_pred, a64_label(b));
        a64_subs_imm(b, 1, A64_ZR, JTF, 0);                  /* NE <=> taken */
        return;
    }
    emit_materialize(b);
    emit_cc_predicate_rflags(b, cc);
    a64_subs_imm(b, 1, A64_ZR, JTF, 0);
}

static int emit_adc_sbb(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (!g_defer) return 0;
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
    if (g_pin_class == 2 && d->reg == OCERZ_RSP) return 0;
    if (s->kind == OCERZ_OPK_REG) { if (s->high8 || s->size != d->size) return 0; }
    else if (s->kind != OCERZ_OPK_IMM) return 0;
    int sf = d->size == 8;
    int is_sbb = insn->op == OCERZ_OP_SBB;
    /* CF: inline from a pending cmp/sub/logic record when possible */
    emit_cc_predicate(b, OCERZ_CC_B);        /* NE <=> CF set */
    a64_cset(b, JTT, A64_NE);                /* JTT = CF */
    emit_gpr_rd(b, sf, JT0, d->reg);
    if (s->kind == OCERZ_OPK_REG) emit_gpr_rd(b, sf, JT1, s->reg);
    else a64_mov_imm64(b, JT1, sf ? (uint64_t)ocerz_sext(s->imm, s->size) : ((uint64_t)ocerz_sext(s->imm, s->size) & 0xffffffffull));
    if (is_sbb) { a64_sub_reg(b, sf, JT2, JT0, JT1, 0); a64_sub_reg(b, sf, JT2, JT2, JTT, 0); }
    else        { a64_add_reg(b, sf, JT2, JT0, JT1, 0); a64_add_reg(b, sf, JT2, JT2, JTT, 0); }
    emit_gpr_wr(b, JT2, d->reg);
    if (need) {
        /* cin is dynamic: encode both variants and select the ccop word */
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

/* 8/16-bit register-destination add/sub/and/or/xor with reg/imm source
 * (deferred flags with the narrow size; result inserted into the low
 * byte/word of the destination register). */
static int emit_arith_narrow(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (!g_defer) return 0;
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 1 && d->size != 2)) return 0;
    if (g_pin_class == 2 && d->reg == OCERZ_RSP) return 0;
    if (s->kind == OCERZ_OPK_REG) { if (s->high8 || s->size != d->size) return 0; }
    else if (s->kind != OCERZ_OPK_IMM) return 0;
    unsigned op = insn->op;
    if (op != OCERZ_OP_ADD && op != OCERZ_OP_SUB && op != OCERZ_OP_AND &&
        op != OCERZ_OP_OR && op != OCERZ_OP_XOR) return 0;
    int size = d->size, bits = size * 8;
    uint64_t mask = size == 1 ? 0xffull : 0xffffull;
    emit_gpr_rd(b, 1, JT0, d->reg);            /* full reg in JT0 */
    if (size == 1) a64_uxtb(b, JTT, JT0); else a64_uxth(b, JTT, JT0);   /* JTT = narrow dst */
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
    a64_bfi(b, 1, JT0, JT2, 0, bits);          /* insert low bits */
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

/* cbw/cwde/cdqe and cwd/cdq/cqo */
static int emit_cbw_cwd(A64Buf *b, const X86Insn *insn)
{
    if (g_pin_class == 2) return 0;
    if (insn->op == OCERZ_OP_CBW) {
        emit_gpr_rd(b, 1, JT0, OCERZ_RAX);
        if (insn->opsize == 2)      { a64_sxtb(b, 0, JT1, JT0); a64_bfi(b, 1, JT0, JT1, 0, 16); }
        else if (insn->opsize == 4) { a64_sxth(b, 0, JT0, JT0); }        /* W-form: zero-extends to 64 */
        else                        { a64_sxtw(b, JT0, JT0); }
        emit_gpr_wr(b, JT0, OCERZ_RAX);
        return 1;
    }
    /* CWD/CDQ/CQO: RDX = sign(RAX) */
    emit_gpr_rd(b, 1, JT0, OCERZ_RAX);
    if (insn->opsize == 2) {
        emit_gpr_rd(b, 1, JT1, OCERZ_RDX);
        a64_sbfx(b, 1, JT0, JT0, 15, 1);      /* -1/0 from bit 15 */
        a64_bfi(b, 1, JT1, JT0, 0, 16);
        emit_gpr_wr(b, JT1, OCERZ_RDX);
    } else if (insn->opsize == 4) {
        a64_asr_imm(b, 0, JT0, JT0, 31);      /* W-form: 0/0xffffffff, upper zero */
        emit_gpr_wr(b, JT0, OCERZ_RDX);
    } else {
        a64_asr_imm(b, 1, JT0, JT0, 63);
        emit_gpr_wr(b, JT0, OCERZ_RDX);
    }
    return 1;
}

/* div/idiv (32/64-bit, register or memory divisor).  Fast path when the
 * high half is the trivial extension (RDX==0 for div, RDX==sext(RAX) for
 * idiv) -- the overwhelmingly common shape after xor edx,edx / cqo.  Any
 * other case (128-bit numerator, zero divisor, overflow) takes the slow call,
 * which traps exactly like the interpreter. */
static int emit_div(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *o = &insn->ops[0];
    if (o->size != 4 && o->size != 8) return 0;
    if (g_pin_class == 2) return 0;
    if (insn->seg != OCERZ_SEG_NONE) return 0;
    int sf = o->size == 8;
    int is_idiv = insn->op == OCERZ_OP_IDIV;
    /* divisor -> JT2 (memory first: EA clobbers JT0) */
    if (o->kind == OCERZ_OPK_MEM) {
        if (!emit_mem_ea(b, insn, o, JTA)) return 0;
        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
        emit_guest_load_ordered(b, o->size, JT2, JTA, JTU);
        patch_guard_skip(skip, a64_label(b));
    } else if (o->kind == OCERZ_OPK_REG) {
        if (o->high8) return 0;
        emit_gpr_rd(b, sf, JT2, o->reg);
    } else return 0;
    emit_gpr_rd(b, sf, JT0, OCERZ_RAX);
    emit_gpr_rd(b, sf, JT1, OCERZ_RDX);
    /* slow-path conditions */
    uint32_t *to_slow[2]; int ns = 0;
    a64_subs_imm(b, sf, A64_ZR, JT2, 0);                 /* divisor == 0 */
    to_slow[ns++] = a64_label(b); a64_bcond(b, A64_EQ, 0);
    if (is_idiv) {
        a64_asr_imm(b, sf, JTT, JT0, sf ? 63 : 31);      /* expected high = sext */
        a64_subs_reg(b, sf, A64_ZR, JT1, JTT, 0);
    } else {
        a64_subs_imm(b, sf, A64_ZR, JT1, 0);
    }
    to_slow[ns++] = a64_label(b); a64_bcond(b, A64_NE, 0);
    if (is_idiv) {
        /* INT_MIN / -1 overflows on x86 (#DE) -> slow */
        a64_mov_imm64(b, JTT, sf ? 0x8000000000000000ull : 0x80000000ull);
        a64_subs_reg(b, sf, A64_ZR, JT0, JTT, 0);
        uint32_t *not_min = a64_label(b); a64_bcond(b, A64_NE, 0);
        a64_mov_imm64(b, JTT, sf ? ~0ull : 0xffffffffull);
        a64_subs_reg(b, sf, A64_ZR, JT2, JTT, 0);
        uint32_t *to_slow3 = a64_label(b); a64_bcond(b, A64_EQ, 0);
        a64_patch_bcond(not_min, a64_label(b));
        /* compute */
        a64_sdiv(b, sf, JTT, JT0, JT2);
        a64_msub(b, sf, JTU, JTT, JT2, JT0);          /* rem = rax - q*div */
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

static int emit_not_neg(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    const X86Operand *d = &insn->ops[0];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    int sf = d->size == 8;
    int ds = pin_slot(d->reg);
    if (insn->op == OCERZ_OP_NOT) {
        if (ds >= 0 && !(g_pin_class == 2 && d->reg == OCERZ_RSP)) {
            a64_orn_reg(b, sf, pin_hreg(ds), A64_ZR, pin_hreg(ds), 0);
            return 1;
        }
        emit_gpr_rd(b, sf, JT0, d->reg);
        a64_orn_reg(b, sf, JT2, A64_ZR, JT0, 0);
        emit_gpr_wr(b, JT2, d->reg);
        return 1;
    }
    /* NEG: result = 0 - a; flags = SUB(0, a) */
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

/* Shift/rotate count source: imm -> constant; CL -> masked register. */
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
        *const_cnt = 0xffffffffu;   /* variable */
        return 1;
    }
    return 0;
}

static int emit_rot(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    const X86Operand *d = &insn->ops[0];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    if (g_pin_class == 2 && d->reg == OCERZ_RSP)
        return 0;
    int sf = d->size == 8;
    int bits = sf ? 64 : 32;
    int is_rol = insn->op == OCERZ_OP_ROL;
    unsigned cnt;
    if (!emit_shift_count(b, insn, sf, JT1, &cnt))
        return 0;
    int variable = cnt == 0xffffffffu;
    if (!variable && cnt == 0)
        return 1;                       /* no-op, flags untouched */
    if (need && variable)
        return 0;                       /* count==0 must not touch flags; keep slow */
    int ds = pin_slot(d->reg);
    int rd = ds >= 0 ? pin_hreg(ds) : JT2;
    if (ds < 0)
        emit_gpr_rd(b, sf, JT0, d->reg);
    int rn = ds >= 0 ? rd : JT0;
    if (variable) {
        if (is_rol) {                   /* rol by n == ror by (bits - n) */
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
    /* Flags (constant count only): CF = rol ? res&1 : msb(res); OF only when cnt==1. */
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
        if (is_rol) {                   /* OF = CF ^ msb(res) */
            a64_ubfx(b, 1, JTU, rd, bits - 1, 1);
            a64_eor_reg(b, 1, JTU, JTU, JT1, 0);
        } else {                        /* OF = msb(res) ^ bit(bits-2)(res) */
            a64_ubfx(b, 1, JTU, rd, bits - 2, 1);
            a64_eor_reg(b, 1, JTU, JTU, JT1, 0);
        }
        a64_lsl_imm(b, 1, JTU, JTU, 11);
        a64_orr_reg(b, 1, JTT, JTT, JTU, 0);
    }
    a64_str(b, 8, JTT, 20, RF_OFF);
    return 1;
}

/* shl/shr/sar by CL (the immediate forms live in emit_shift). */
static int emit_shift_cl(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    if (!(s->kind == OCERZ_OPK_REG && s->reg == OCERZ_RCX && !s->high8 && s->size == 1))
        return 0;
    if (g_pin_class == 2 && d->reg == OCERZ_RSP)
        return 0;
    if (need)
        return 0;                       /* flags: count==0 keeps them; stay slow */
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

static int emit_sse_mem_addr(A64Buf *b, const X86Insn *insn, const X86Operand *o,
                             uint32_t **exit_sites, int *n_exits, uint32_t **skip_out);
static int emit_cmov(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    if (g_pin_class == 2 && (d->reg == OCERZ_RSP || (s->kind == OCERZ_OPK_REG && s->reg == OCERZ_RSP)))
        return 0;
    int sf = d->size == 8;
    /* source value -> JT2 (memory sources are loaded even when not taken, as x86 does);
     * emit_cc_predicate may call C on its generic path, which clobbers JT regs
     * except that we reload nothing: JT2 must survive -> load the source AFTER. */
    if (s->kind == OCERZ_OPK_REG) {
        if (s->high8 || s->size != d->size)
            return 0;
    } else if (s->kind != OCERZ_OPK_MEM)
        return 0;
    /* predicate first (may call C), then load the source into JT2 */
    emit_cc_predicate(b, insn->cc);      /* NE = take ; leaves NZCV */
    a64_cset(b, JTF, A64_NE);            /* keep predicate in JTF across the loads */
    if (s->kind == OCERZ_OPK_REG) {
        emit_gpr_rd(b, sf, JT2, s->reg);
    } else {
        /* re-materialise the memory source (EA may clobber JT0/JTA; JTF survives) */
        uint32_t *skip2;
        if (!emit_sse_mem_addr(b, insn, s, exit_sites, n_exits, &skip2)) return 0;
        emit_guest_load_ordered(b, d->size, JT2, JTA, JTU);
        patch_guard_skip(skip2, a64_label(b));
    }
    a64_subs_imm(b, 1, A64_ZR, JTF, 0);
    emit_gpr_rd(b, sf, JT0, d->reg);
    a64_csel(b, sf, JT0, JT2, JT0, A64_NE);
    if (!sf)
        a64_mov_reg(b, 0, JT0, JT0);     /* zero-extend 32-bit result */
    emit_gpr_wr(b, JT0, d->reg);
    return 1;
}

static int emit_setcc(A64Buf *b, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    if (d->kind != OCERZ_OPK_REG || d->high8 || d->size != 1)
        return 0;
    if (g_pin_class == 2 && d->reg == OCERZ_RSP)
        return 0;
    emit_cc_predicate(b, insn->cc);
    a64_cset(b, JT2, A64_NE);
    /* write low byte only, preserving the rest of the register */
    emit_gpr_rd(b, 1, JT0, d->reg);
    a64_bfi(b, 1, JT0, JT2, 0, 8);
    emit_gpr_wr(b, JT0, d->reg);
    return 1;
}

static int emit_bswap(A64Buf *b, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    if (g_pin_class == 2 && d->reg == OCERZ_RSP)
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


/* ======================= SSE / SSE2 inline emitters =======================
 * v1 model: every op loads its XMM operands from cpu->xmm[] into host V
 * registers V0..V3, computes, and stores the result back.  Memory operands
 * go through the same EA/guard protocol as GPR loads.  In the ordered
 * (multi-threaded) memory model, 128-bit accesses get a dmb to keep x86 TSO
 * acquire/release semantics; in plain mode they are plain ldr/str q. */
#define XMM_BASE_OFF ((uint32_t)offsetof(OcerzCPU, xmm))
enum { VX0 = 0, VX1 = 1, VX2 = 2, VX3 = 3 };

/* ---- XMM pinning: xmmN lives in host V(16+N) for the whole block ----
 * g_xmm_pinned: mask of xmm regs the block keeps in V16-V31 (loaded after
 * body entry, spilled at every exit and around every C callout, which
 * clobbers V16-V31).  Loads/stores below become register moves when the
 * register is pinned. */
static uint16_t g_xmm_pinned;
static int xmm_pinning_enabled(void)
{
    static int on = -1;
    if (on < 0) on = getenv("OCERZ_NO_XMM_PIN") ? 0 : 1;
    return on;
}
/* Global XMM layout: every block pins all 16 xmm registers (with full GPR
 * pinning), so body-to-body transitions carry them in V16-V31 with no
 * spill/reload; only function entry/exit and C callouts touch memory. */
static int xmm_global_enabled(void)
{
    static int on = -1;
    if (on < 0) on = (getenv("OCERZ_NO_XMM_GLOBAL") || getenv("OCERZ_NO_FULLPIN")) ? 0 : 1;
    return on;
}
static inline int xmm_vreg(unsigned xr) { return 16 + (int)xr; }
static inline int xmm_is_pinned(unsigned xr) { return (g_xmm_pinned >> xr) & 1; }
static void emit_pk_consts_load(A64Buf *b);
static void emit_xmm_pin_load_all(A64Buf *b)
{
    for (unsigned r = 0; r < 16; r++)
        if (xmm_is_pinned(r))
            a64_ldr_v(b, 16, xmm_vreg(r), 20, XMM_BASE_OFF + r * 16);
    emit_pk_consts_load(b);      /* V4-V7 are caller-saved: rebuild after callouts too */
}
static void emit_xmm_pin_spill_all(A64Buf *b)
{
    for (unsigned r = 0; r < 16; r++)
        if (xmm_is_pinned(r))
            a64_str_v(b, 16, xmm_vreg(r), 20, XMM_BASE_OFF + r * 16);
}

_Static_assert(offsetof(OcerzCPU, xmm) % 16 == 0, "xmm must be 16-aligned for scaled q loads");
static void emit_xmm_ld(A64Buf *b, int vd, unsigned xr)   /* full 128-bit */
{
    if (xmm_is_pinned(xr)) { if (vd != xmm_vreg(xr)) a64_v_mov(b, vd, xmm_vreg(xr)); return; }
    a64_ldr_v(b, 16, vd, 20, XMM_BASE_OFF + (uint32_t)xr * 16);
}
static void emit_xmm_st(A64Buf *b, int vs, unsigned xr)
{
    if (xmm_is_pinned(xr)) { if (vs != xmm_vreg(xr)) a64_v_mov(b, xmm_vreg(xr), vs); return; }
    a64_str_v(b, 16, vs, 20, XMM_BASE_OFF + (uint32_t)xr * 16);
}
static void emit_xmm_ld_lo(A64Buf *b, int size, int vd, unsigned xr) /* 4/8 low bytes; upper zero */
{
    if (xmm_is_pinned(xr)) {
        /* fmov d/s zeroes the upper part of the destination */
        if (size == 8) a64_fmov_d_d(b, vd, xmm_vreg(xr)); else a64_fmov_s_s(b, vd, xmm_vreg(xr));
        return;
    }
    a64_ldr_v(b, size, vd, 20, XMM_BASE_OFF + (uint32_t)xr * 16);
}
static void emit_xmm_st_lo(A64Buf *b, int size, int vs, unsigned xr) /* only low 4/8 bytes */
{
    if (xmm_is_pinned(xr)) {
        /* insert the low lane, keep the rest */
        if (size == 8) a64_ins_d_d(b, xmm_vreg(xr), 0, vs, 0); else a64_ins_s_s(b, xmm_vreg(xr), 0, vs, 0);
        return;
    }
    a64_str_v(b, size, vs, 20, XMM_BASE_OFF + (uint32_t)xr * 16);
}

/* Guest memory operand -> host address in JTA (with guard/skip pair). */
static int emit_sse_mem_addr(A64Buf *b, const X86Insn *insn, const X86Operand *o,
                             uint32_t **exit_sites, int *n_exits, uint32_t **skip_out)
{
    if (!emit_mem_ea(b, insn, o, JTA))
        return 0;
    *skip_out = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
    emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
    return 1;
}
static void emit_sse_mem_ld(A64Buf *b, int size, int vd)   /* from [JTA] */
{
    a64_ldr_v(b, size, vd, JTA, 0);
    if (!g_plain_mem)
        a64_dmb_ish(b);          /* conservative acquire */
}
static void emit_sse_mem_st(A64Buf *b, int size, int vs)   /* to [JTA] */
{
    if (!g_plain_mem)
        a64_dmb_ish(b);          /* conservative release */
    a64_str_v(b, size, vs, JTA, 0);
}

/* Load operand `o` (xmm or mem) of width `size` (4/8/16) into vd.
 * For xmm sources the whole register is loaded (harmless for scalar use). */
static int emit_sse_src(A64Buf *b, const X86Insn *insn, const X86Operand *o, int size,
                        int vd, uint32_t **exit_sites, int *n_exits)
{
    if (o->kind == OCERZ_OPK_XMM) {
        emit_xmm_ld(b, vd, o->reg);
        return 1;
    }
    if (o->kind == OCERZ_OPK_MEM) {
        uint32_t *skip;
        if (!emit_sse_mem_addr(b, insn, o, exit_sites, n_exits, &skip))
            return 0;
        emit_sse_mem_ld(b, size, vd);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    return 0;
}

static int sse_enabled(void)
{
    static int on = -1;
    if (on < 0) on = getenv("OCERZ_NO_INLINE_SSE") ? 0 : 1;
    return on;
}

/* ---- movups/movaps/movdqa/movdqu (128-bit moves) ---- */
static int emit_sse_mov128(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind == OCERZ_OPK_XMM && s->kind == OCERZ_OPK_XMM) {
        if (d->reg != s->reg) {
            emit_xmm_ld(b, VX0, s->reg);
            emit_xmm_st(b, VX0, d->reg);
        }
        return 1;
    }
    if (d->kind == OCERZ_OPK_XMM && s->kind == OCERZ_OPK_MEM) {
        uint32_t *skip;
        if (!emit_sse_mem_addr(b, insn, s, exit_sites, n_exits, &skip)) return 0;
        emit_sse_mem_ld(b, 16, VX0);
        patch_guard_skip(skip, a64_label(b));
        emit_xmm_st(b, VX0, d->reg);
        return 1;
    }
    if (d->kind == OCERZ_OPK_MEM && s->kind == OCERZ_OPK_XMM) {
        emit_xmm_ld(b, VX0, s->reg);
        uint32_t *skip;
        if (!emit_sse_mem_addr(b, insn, d, exit_sites, n_exits, &skip)) return 0;
        emit_sse_mem_st(b, 16, VX0);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    return 0;
}

/* ---- movss / movsd (scalar moves; size 4 / 8) ---- */
static int emit_sse_movs(A64Buf *b, const X86Insn *insn, int size, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind == OCERZ_OPK_XMM && s->kind == OCERZ_OPK_XMM) {
        /* dst.lo(size) = src.lo(size); rest of dst preserved */
        emit_xmm_ld_lo(b, size, VX0, s->reg);
        emit_xmm_st_lo(b, size, VX0, d->reg);
        return 1;
    }
    if (d->kind == OCERZ_OPK_XMM && s->kind == OCERZ_OPK_MEM) {
        uint32_t *skip;
        if (!emit_sse_mem_addr(b, insn, s, exit_sites, n_exits, &skip)) return 0;
        emit_sse_mem_ld(b, size, VX0);          /* ldr s/d zeroes the rest of V0 */
        patch_guard_skip(skip, a64_label(b));
        emit_xmm_st(b, VX0, d->reg);            /* whole 128: upper zeroed (x86 semantics) */
        return 1;
    }
    if (d->kind == OCERZ_OPK_MEM && s->kind == OCERZ_OPK_XMM) {
        emit_xmm_ld_lo(b, size, VX0, s->reg);
        uint32_t *skip;
        if (!emit_sse_mem_addr(b, insn, d, exit_sites, n_exits, &skip)) return 0;
        emit_sse_mem_st(b, size, VX0);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    return 0;
}

/* x86 SSE NaN rule: result NaN -> quiet(a) if a is NaN, else quiet(b) if b is
 * NaN, else the (negative) default NaN.  arm64 differs (SNaN priority and a
 * positive default), so results that are NaN get fixed from the INPUTS.
 * Hot path is a NaN test + not-taken branch. va/vb = inputs, vr = result. */
/* cold: r = isnan(a) ? a|q : isnan(b) ? b|q : dflt  (scalar; result in vr) */
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
static void emit_nan_fix_scalar2(A64Buf *b, int dbl, int vr, int va, int vb)
{
    a64_fcmp(b, dbl, vr, vr);
    if (g_n_nanool < NANOOL_MAX) {
        NanOolPend *o = &g_nanool[g_n_nanool++];
        o->site = a64_label(b); a64_bcond(b, A64_VS, 0);      /* NaN -> out of line */
        o->back = a64_label(b);
        o->dbl = (uint8_t)dbl; o->packed = 0; o->vr = (uint8_t)vr; o->va = (uint8_t)va; o->vb = (uint8_t)vb; o->t1 = 0;
        o->idx = g_cur_insn_idx; o->is_cbz = 0;
        return;
    }
    /* table full: inline cold path */
    uint32_t *ok = a64_label(b); a64_bcond(b, A64_VC, 0);
    emit_nan_cold_scalar(b, dbl, vr, va, vb);
    a64_patch_bcond(ok, a64_label(b));
}
/* Packed: per-lane exact rule.  Constants: V4/V6 = quiet bit per lane (2D/4S),
 * V5/V7 = default NaN per lane. */
static int g_pk_consts_needed;
static void emit_pk_consts_load(A64Buf *b)
{
    if (!g_pk_consts_needed) return;
    a64_mov_imm64(b, JT0, 0x0008000000000000ull); a64_fmov_v_from_x(b, 1, 4, JT0); a64_v_dup_d(b, 4, 4, 0);
    a64_mov_imm64(b, JT0, 0xfff8000000000000ull); a64_fmov_v_from_x(b, 1, 5, JT0); a64_v_dup_d(b, 5, 5, 0);
    a64_mov_imm64(b, JT0, 0x00400000ull);         a64_fmov_v_from_x(b, 0, 6, JT0); a64_v_dup_s(b, 6, 6, 0);
    a64_mov_imm64(b, JT0, 0xffc00000ull);         a64_fmov_v_from_x(b, 0, 7, JT0); a64_v_dup_s(b, 7, 7, 0);
}
/* cold packed: exact, lane by lane through a stack scratch area */
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
static void emit_nan_fix_packed2(A64Buf *b, int dbl, int vr, int va, int vb, int t1, int t2)
{
    (void)t2;
    /* hot: any NaN lane in vr?  fcmeq t1 = (vr==vr) -> all-ones per non-NaN lane;
     * xtn narrows to 64 bits (no cross-lane reduce); == -1 iff no NaN. */
    a64_v_fcmeq(b, dbl, t1, vr, vr);
    a64_v_xtn(b, dbl ? 2 : 1, t1, t1);
    a64_fmov_x_from_v(b, 1, JT0, t1);
    a64_cmn_imm(b, 1, JT0, 1);                                /* Z iff all ones */
    if (g_n_nanool < NANOOL_MAX) {
        NanOolPend *o = &g_nanool[g_n_nanool++];
        o->site = a64_label(b); a64_bcond(b, A64_NE, 0);      /* NaN -> out of line */
        o->back = a64_label(b);
        o->dbl = (uint8_t)dbl; o->packed = 1; o->vr = (uint8_t)vr; o->va = (uint8_t)va; o->vb = (uint8_t)vb; o->t1 = (uint8_t)t1;
        o->idx = g_cur_insn_idx; o->is_cbz = 0;
        return;
    }
    uint32_t *ok = a64_label(b); a64_bcond(b, A64_EQ, 0);
    emit_nan_cold_packed(b, dbl, vr, va, vb, t1);
    a64_patch_bcond(ok, a64_label(b));
}

/* Emit all pending NaN out-of-line arms (after the block body). */
static void emit_nan_ool_arms(A64Buf *b, JitBlock *blk, const uint32_t *entry)
{
    (void)blk; (void)entry;
    for (int i = 0; i < g_n_nanool; i++) {
        const NanOolPend *o = &g_nanool[i];
        uint32_t *lo = a64_label(b);
        if (o->is_cbz) a64_patch_cbz(o->site, lo); else a64_patch_bcond(o->site, lo);
        if (o->packed) emit_nan_cold_packed(b, o->dbl, o->vr, o->va, o->vb, o->t1);
        else           emit_nan_cold_scalar(b, o->dbl, o->vr, o->va, o->vb);
        uint32_t *here = a64_label(b);
        a64_b(b, (int32_t)(o->back - here));
    }
    g_n_nanool = 0;
}

/* ---- scalar / packed FP arithmetic ---- */
static int emit_sse_fparith(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM) return 0;
    int dbl = 0, packed = 0, kind = 0;   /* kind: 0 add 1 sub 2 mul 3 div 4 max 5 min 6 sqrt */
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
    case OCERZ_OP_MINSS: kind=5; break;  case OCERZ_OP_MINSD: kind=5; dbl=1; break;
    case OCERZ_OP_SQRTSS: kind=6; break; case OCERZ_OP_SQRTSD: kind=6; dbl=1; break;
    case OCERZ_OP_SQRTPS: kind=6; packed=1; break; case OCERZ_OP_SQRTPD: kind=6; dbl=1; packed=1; break;
    default: return 0;
    }
    if (kind == 4 || kind == 5) return 0;      /* x86 max/min NaN/zero rules: keep slow */
    int esz = dbl ? 8 : 4;
    /* Operand registers: pinned xmm operands are used in place (no copies);
     * memory / unpinned sources go through VX1; the dst value through VX0.
     * Result -> VX2, then written back (needed: the fix reads the inputs). */
    int vb;
    if (s->kind == OCERZ_OPK_XMM && xmm_is_pinned(s->reg)) vb = xmm_vreg(s->reg);
    else { if (!emit_sse_src(b, insn, s, packed ? 16 : esz, VX1, exit_sites, n_exits)) return 0; vb = VX1; }
    if (kind == 6) {
        /* sqrt: single input (b).  x86: NaN input -> quiet(b) ; negative -> default NaN */
        if (packed) {
            a64_v_fsqrt(b, dbl, VX2, vb);
            emit_nan_fix_packed2(b, dbl, VX2, vb, vb, VX3, VX0);
            emit_xmm_st(b, VX2, d->reg);
        } else {
            a64_fsqrt_s(b, dbl, VX2, vb);
            emit_nan_fix_scalar2(b, dbl, VX2, vb, vb);
            emit_xmm_st_lo(b, esz, VX2, d->reg);
        }
        return 1;
    }
    int va;
    if (xmm_is_pinned(d->reg)) va = xmm_vreg(d->reg);
    else { emit_xmm_ld(b, VX0, d->reg); va = VX0; }
    if (packed) {
        switch (kind) {
        case 0: a64_v_fadd(b, dbl, VX2, va, vb); break;
        case 1: a64_v_fsub(b, dbl, VX2, va, vb); break;
        case 2: a64_v_fmul(b, dbl, VX2, va, vb); break;
        case 3: a64_v_fdiv(b, dbl, VX2, va, vb); break;
        }
        emit_nan_fix_packed2(b, dbl, VX2, va, vb, VX3, VX3);
        emit_xmm_st(b, VX2, d->reg);
    } else {
        switch (kind) {
        case 0: a64_fadd_s(b, dbl, VX2, va, vb); break;
        case 1: a64_fsub_s(b, dbl, VX2, va, vb); break;
        case 2: a64_fmul_s(b, dbl, VX2, va, vb); break;
        case 3: a64_fdiv_s(b, dbl, VX2, va, vb); break;
        }
        emit_nan_fix_scalar2(b, dbl, VX2, va, vb);
        emit_xmm_st_lo(b, esz, VX2, d->reg);   /* only low lane written */
    }
    return 1;
}

/* ---- bitwise 128-bit: pxor/pand/pandn/por/xorps/andps/andnps/orps + padd/psub ---- */
static int emit_sse_bitwise(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM) return 0;
    int kind, esz = 0;
    switch (insn->op) {
    case OCERZ_OP_PXOR: case OCERZ_OP_XORPS: kind = 0; break;
    case OCERZ_OP_PAND: case OCERZ_OP_ANDPS: kind = 1; break;
    case OCERZ_OP_POR:  case OCERZ_OP_ORPS:  kind = 2; break;
    case OCERZ_OP_PANDN: case OCERZ_OP_ANDNPS: kind = 3; break;   /* d = ~d & s */
    case OCERZ_OP_PADDB: kind = 4; esz = 0; break; case OCERZ_OP_PADDW: kind = 4; esz = 1; break;
    case OCERZ_OP_PADDD: kind = 4; esz = 2; break; case OCERZ_OP_PADDQ: kind = 4; esz = 3; break;
    case OCERZ_OP_PSUBB: kind = 5; esz = 0; break; case OCERZ_OP_PSUBW: kind = 5; esz = 1; break;
    case OCERZ_OP_PSUBD: kind = 5; esz = 2; break; case OCERZ_OP_PSUBQ: kind = 5; esz = 3; break;
    case OCERZ_OP_PCMPEQD: kind = 6; esz = 2; break; case OCERZ_OP_PCMPEQB: kind = 6; esz = 0; break;
    case OCERZ_OP_PCMPEQW: kind = 6; esz = 1; break; case OCERZ_OP_PCMPEQQ: kind = 6; esz = 3; break;
    case OCERZ_OP_PCMPGTD: kind = 7; esz = 2; break;
    default: return 0;
    }
    /* pxor x,x -> zero (very common idiom) */
    if (kind == 0 && s->kind == OCERZ_OPK_XMM && s->reg == d->reg) {
        a64_v_zero(b, VX0);
        emit_xmm_st(b, VX0, d->reg);
        return 1;
    }
    if (!emit_sse_src(b, insn, s, 16, VX1, exit_sites, n_exits)) return 0;
    emit_xmm_ld(b, VX0, d->reg);
    switch (kind) {
    case 0: a64_v_eor(b, VX0, VX0, VX1); break;
    case 1: a64_v_and(b, VX0, VX0, VX1); break;
    case 2: a64_v_orr(b, VX0, VX0, VX1); break;
    case 3: a64_v_bic(b, VX0, VX1, VX0); break;    /* s & ~d */
    case 4: a64_v_add(b, esz, VX0, VX0, VX1); break;
    case 5: a64_v_sub(b, esz, VX0, VX0, VX1); break;
    case 6: a64_v_cmeq(b, esz, VX0, VX0, VX1); break;
    case 7: a64_v_cmgt(b, esz, VX0, VX0, VX1); break;
    }
    emit_xmm_st(b, VX0, d->reg);
    return 1;
}

/* ---- ucomis[sd]/comis[sd]: ZF,PF,CF from compare; OF,SF,AF cleared ---- */
static int emit_sse_comis(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM) return 0;
    int dbl = insn->op == OCERZ_OP_UCOMISD || insn->op == OCERZ_OP_COMISD;
    int esz = dbl ? 8 : 4;
    /* comis overwrites every arithmetic flag: a pending deferred record is
     * dead -- drop it instead of materializing it. */
    if (g_defer)
        a64_str(b, 4, A64_ZR, 20, CC_OP_OFF);
    if (!emit_sse_src(b, insn, s, esz, VX1, exit_sites, n_exits)) return 0;
    emit_xmm_ld_lo(b, esz, VX0, d->reg);
    a64_fcmp(b, dbl, VX0, VX1);
    /* arm64 after fcmp: unordered -> N=0,Z=0,C=1,V=1 ; a<b -> N=1 ; a==b -> Z=1,C=1 ; a>b -> C=1
     * x86: unordered -> ZF=PF=CF=1 ; a<b -> CF=1 ; a==b -> ZF=1 ; a>b -> all 0 */
    a64_ldr(b, 8, JTT, 20, RF_OFF);
    a64_mov_imm64(b, JTU, ~(uint64_t)(OCERZ_CF | OCERZ_PF | OCERZ_ZF | OCERZ_SF | OCERZ_OF | OCERZ_AF));
    a64_and_reg(b, 1, JTT, JTT, JTU, 0);
    a64_cset(b, JT0, A64_VS);            /* unordered */
    a64_lsl_imm(b, 1, JT1, JT0, 2);      /* PF bit */
    a64_orr_reg(b, 1, JTT, JTT, JT1, 0);
    a64_cset(b, JT1, A64_MI);            /* less than -> CF */
    a64_orr_reg(b, 1, JT1, JT1, JT0, 0); /* unordered -> CF too */
    a64_orr_reg(b, 1, JTT, JTT, JT1, 0);
    a64_cset(b, JT1, A64_EQ);            /* equal -> ZF */
    a64_orr_reg(b, 1, JT1, JT1, JT0, 0); /* unordered -> ZF too */
    a64_lsl_imm(b, 1, JT1, JT1, 6);
    a64_orr_reg(b, 1, JTT, JTT, JT1, 0);
    a64_str(b, 8, JTT, 20, RF_OFF);
    return 1;
}

/* ---- conversions ---- */
static int emit_sse_cvt(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    switch (insn->op) {
    case OCERZ_OP_CVTTSD2SI: case OCERZ_OP_CVTTSS2SI: {
        if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
        int dbl = insn->op == OCERZ_OP_CVTTSD2SI;
        if (!emit_sse_src(b, insn, s, dbl ? 8 : 4, VX0, exit_sites, n_exits)) return 0;
        /* x86 returns 0x8000.. (indefinite) on overflow/NaN; arm64 saturates.  Match x86 for NaN
         * and out-of-range by checking the flags: unordered/overflow are rare -> accept saturation
         * differences?  No: keep exactness -- detect NaN via fcmp and force indefinite. */
        a64_fcvtzs(b, d->size == 8, dbl, JT0, VX0);
        a64_fcmp(b, dbl, VX0, VX0);              /* VS <=> NaN */
        a64_mov_imm64(b, JT1, d->size == 8 ? 0x8000000000000000ull : 0x80000000ull);
        a64_csel(b, 1, JT0, JT1, JT0, A64_VS);
        /* out-of-range: arm64 saturates to INT_MAX/INT_MIN, x86 gives INT_MIN.  Fix INT_MAX case. */
        a64_mov_imm64(b, JTU, d->size == 8 ? 0x7fffffffffffffffull : 0x7fffffffull);
        a64_subs_reg(b, 1, A64_ZR, JT0, JTU, 0);
        a64_csel(b, 1, JT0, JT1, JT0, A64_EQ);
        if (d->size == 4) a64_mov_reg(b, 0, JT0, JT0);
        emit_gpr_wr(b, JT0, d->reg);
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
            if (!emit_sse_mem_addr(b, insn, s, exit_sites, n_exits, &skip)) return 0;
            emit_guest_load_ordered(b, s->size, JT0, JTA, JTU);
            patch_guard_skip(skip, a64_label(b));
        } else return 0;
        a64_scvtf(b, sf, dbl, VX0, JT0);
        emit_xmm_st_lo(b, dbl ? 8 : 4, VX0, d->reg);   /* upper lanes preserved */
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

/* ---- movd/movq between gpr/mem and xmm ---- */
static int emit_sse_movd(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind == OCERZ_OPK_XMM && s->kind == OCERZ_OPK_REG) {
        if (s->high8 || (s->size != 4 && s->size != 8)) return 0;
        emit_gpr_rd(b, s->size == 8, JT0, s->reg);
        a64_fmov_v_from_x(b, s->size == 8, VX0, JT0);   /* zero-extends to 128 */
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
        if (!emit_sse_mem_addr(b, insn, s, exit_sites, n_exits, &skip)) return 0;
        emit_sse_mem_ld(b, s->size, VX0);
        patch_guard_skip(skip, a64_label(b));
        emit_xmm_st(b, VX0, d->reg);
        return 1;
    }
    if (d->kind == OCERZ_OPK_MEM && s->kind == OCERZ_OPK_XMM) {
        if (d->size != 4 && d->size != 8) return 0;
        emit_xmm_ld_lo(b, d->size, VX0, s->reg);
        uint32_t *skip;
        if (!emit_sse_mem_addr(b, insn, d, exit_sites, n_exits, &skip)) return 0;
        emit_sse_mem_st(b, d->size, VX0);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    return 0;
}

/* ---- unpckl/hpd, movlhps/movhlps ---- */
static int emit_sse_unpck(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM) return 0;
    if (!emit_sse_src(b, insn, s, 16, VX1, exit_sites, n_exits)) return 0;
    emit_xmm_ld(b, VX0, d->reg);
    switch (insn->op) {
    case OCERZ_OP_UNPCKLPD: case OCERZ_OP_MOVLHPS: a64_v_zip1(b, 3, VX2, VX0, VX1); break; /* {d.lo, s.lo} */
    case OCERZ_OP_UNPCKHPD:                        a64_v_zip2(b, 3, VX2, VX0, VX1); break; /* {d.hi, s.hi} */
    case OCERZ_OP_MOVHLPS:  /* d.lo = s.hi ; d.hi kept */
        a64_v_mov(b, VX2, VX0); a64_ins_d_d(b, VX2, 0, VX1, 1); break;
    case OCERZ_OP_UNPCKLPS: a64_v_zip1(b, 2, VX2, VX0, VX1); break;
    case OCERZ_OP_UNPCKHPS: a64_v_zip2(b, 2, VX2, VX0, VX1); break;
    default: return 0;
    }
    emit_xmm_st(b, VX2, d->reg);
    return 1;
}

/* ---- cmpss/cmpsd (scalar compare -> lane mask by imm8 predicate) ---- */
static int emit_sse_cmps(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM || insn->nops < 3 || insn->ops[2].kind != OCERZ_OPK_IMM) return 0;
    int dbl = insn->op == OCERZ_OP_CMPSDX;
    int esz = dbl ? 8 : 4;
    unsigned pred = (unsigned)insn->ops[2].imm & 7;
    if (!emit_sse_src(b, insn, s, esz, VX1, exit_sites, n_exits)) return 0;
    emit_xmm_ld_lo(b, esz, VX0, d->reg);
    a64_fcmp(b, dbl, VX0, VX1);
    int cond;
    switch (pred) {
    case 0: cond = A64_EQ; break;   /* eq (ordered)            */
    case 1: cond = A64_MI; break;   /* lt (ordered)            */
    case 2: cond = A64_LS; break;   /* le (ordered)            */
    case 3: cond = A64_VS; break;   /* unordered               */
    case 4: cond = A64_NE; break;   /* neq or unordered        */
    case 5: cond = A64_PL; break;   /* !lt  (ge or unordered)  */
    case 6: cond = A64_HI; break;   /* !le  (gt or unordered)  */
    default: cond = A64_VC; break;  /* ordered                 */
    }
    a64_csetm(b, JT0, cond);
    a64_fmov_v_from_x(b, dbl, VX2, JT0);
    emit_xmm_st_lo(b, esz, VX2, d->reg);
    return 1;
}

/* ---- blendvpd/blendvps/pblendvb: per-lane select by xmm0's sign bits ---- */
static int emit_sse_blendv(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM) return 0;
    if (!emit_sse_src(b, insn, s, 16, VX1, exit_sites, n_exits)) return 0;
    emit_xmm_ld(b, VX0, d->reg);
    emit_xmm_ld(b, VX2, 0);                       /* xmm0 = mask source */
    switch (insn->op) {
    case OCERZ_OP_BLENDVPD: a64_v_sshr_2d(b, VX2, VX2, 63); break;   /* sign -> all ones */
    case OCERZ_OP_BLENDVPS: a64_v_sshr_4s(b, VX2, VX2, 31); break;
    case OCERZ_OP_PBLENDVB: {
        /* byte lanes: mask = (v2 as signed bytes) >> 7 : use cmgt 0 > v2 -> all ones where negative */
        a64_v_zero(b, VX3);
        a64_v_cmgt(b, 0, VX2, VX3, VX2);          /* 0 > v2 (signed byte) */
        break;
    }
    default: return 0;
    }
    a64_v_bsl(b, VX2, VX1, VX0);                  /* v2 = mask ? v1(src) : v0(dst) */
    emit_xmm_st(b, VX2, d->reg);
    return 1;
}

static int emit_sse(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    if (!sse_enabled()) return 0;
    if (insn->seg != OCERZ_SEG_NONE) return 0;
    switch (insn->op) {
    case OCERZ_OP_MOVUPS: case OCERZ_OP_MOVAPS: case OCERZ_OP_MOVDQA: case OCERZ_OP_MOVDQU:
        return emit_sse_mov128(b, insn, exit_sites, n_exits);
    case OCERZ_OP_MOVSS:  return emit_sse_movs(b, insn, 4, exit_sites, n_exits);
    case OCERZ_OP_MOVSDX: return emit_sse_movs(b, insn, 8, exit_sites, n_exits);
    case OCERZ_OP_ADDSS: case OCERZ_OP_ADDSD: case OCERZ_OP_ADDPS: case OCERZ_OP_ADDPD:
    case OCERZ_OP_SUBSS: case OCERZ_OP_SUBSD: case OCERZ_OP_SUBPS: case OCERZ_OP_SUBPD:
    case OCERZ_OP_MULSS: case OCERZ_OP_MULSD: case OCERZ_OP_MULPS: case OCERZ_OP_MULPD:
    case OCERZ_OP_DIVSS: case OCERZ_OP_DIVSD: case OCERZ_OP_DIVPS: case OCERZ_OP_DIVPD:
    case OCERZ_OP_MAXSS: case OCERZ_OP_MAXSD: case OCERZ_OP_MINSS: case OCERZ_OP_MINSD:
    case OCERZ_OP_SQRTSS: case OCERZ_OP_SQRTSD: case OCERZ_OP_SQRTPS: case OCERZ_OP_SQRTPD:
        return emit_sse_fparith(b, insn, exit_sites, n_exits);
    case OCERZ_OP_PXOR: case OCERZ_OP_XORPS: case OCERZ_OP_PAND: case OCERZ_OP_ANDPS:
    case OCERZ_OP_POR: case OCERZ_OP_ORPS: case OCERZ_OP_PANDN: case OCERZ_OP_ANDNPS:
    case OCERZ_OP_PADDB: case OCERZ_OP_PADDW: case OCERZ_OP_PADDD: case OCERZ_OP_PADDQ:
    case OCERZ_OP_PSUBB: case OCERZ_OP_PSUBW: case OCERZ_OP_PSUBD: case OCERZ_OP_PSUBQ:
    case OCERZ_OP_PCMPEQB: case OCERZ_OP_PCMPEQW: case OCERZ_OP_PCMPEQD: case OCERZ_OP_PCMPEQQ:
    case OCERZ_OP_PCMPGTD:
        return emit_sse_bitwise(b, insn, exit_sites, n_exits);
    case OCERZ_OP_UCOMISS: case OCERZ_OP_UCOMISD: case OCERZ_OP_COMISS: case OCERZ_OP_COMISD:
        return emit_sse_comis(b, insn, exit_sites, n_exits);
    case OCERZ_OP_CVTTSD2SI: case OCERZ_OP_CVTTSS2SI: case OCERZ_OP_CVTSI2SD: case OCERZ_OP_CVTSI2SS:
    case OCERZ_OP_CVTSD2SS: case OCERZ_OP_CVTSS2SD: case OCERZ_OP_CVTDQ2PS:
        return emit_sse_cvt(b, insn, exit_sites, n_exits);
    case OCERZ_OP_MOVD:
        return emit_sse_movd(b, insn, exit_sites, n_exits);
    case OCERZ_OP_UNPCKLPD: case OCERZ_OP_UNPCKHPD: case OCERZ_OP_MOVLHPS: case OCERZ_OP_MOVHLPS:
    case OCERZ_OP_UNPCKLPS: case OCERZ_OP_UNPCKHPS:
        return emit_sse_unpck(b, insn, exit_sites, n_exits);
    case OCERZ_OP_CMPSS: case OCERZ_OP_CMPSDX:
        return emit_sse_cmps(b, insn, exit_sites, n_exits);
    case OCERZ_OP_BLENDVPD: case OCERZ_OP_BLENDVPS: case OCERZ_OP_PBLENDVB:
        return emit_sse_blendv(b, insn, exit_sites, n_exits);
    default:
        return 0;
    }
}

static int try_inline(A64Buf *b, const X86Insn *insn, uint64_t need,
                      uint32_t **exit_sites, int *n_exits)
{
    if (insn->op == OCERZ_OP_NOP || insn->op == OCERZ_OP_PAUSE ||
        insn->op == OCERZ_OP_PREFETCH || insn->op == OCERZ_OP_CLFLUSH)
        return 1;

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
                int host_rsp = g_pin_class == 2 &&
                    (d->reg == OCERZ_RSP || s->reg == OCERZ_RSP);
                if (host_rsp && !sz4 && d->reg == s->reg)
                    return 1;
                if (g_pin_class == 2 && !sz4 && s->reg == OCERZ_RSP &&
                    d->reg != OCERZ_RSP && ds >= 0 && ss >= 0) {
                    a64_mov_imm64(b, pin_hreg(ds), ocerz_guest_base);
                    a64_sub_reg(b, 1, pin_hreg(ds), pin_hreg(ss), pin_hreg(ds), 0);
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
                if (ds >= 0 && !(g_pin_class == 2 && d->reg == OCERZ_RSP))
                    a64_mov_imm64(b, pin_hreg(ds), v);
                else {
                    a64_mov_imm64(b, JT0, v);
                    emit_gpr_wr(b, JT0, d->reg);
                }
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
        if (insn->ops[1].kind == OCERZ_OPK_MEM)
            return emit_arith_mem(b, insn, need, exit_sites, n_exits);
        return emit_arith(b, insn, need);
    case OCERZ_OP_INC:
    case OCERZ_OP_DEC:
        return emit_incdec(b, insn, need);
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
        if (getenv("OCERZ_NO_INLINE_CMOV")) return 0;
        return emit_cmov(b, insn, exit_sites, n_exits);
    case OCERZ_OP_SETCC:
        if (getenv("OCERZ_NO_INLINE_SETCC")) return 0;
        return emit_setcc(b, insn);
    case OCERZ_OP_BSWAP:
        return emit_bswap(b, insn);
    case OCERZ_OP_MOVUPS: case OCERZ_OP_MOVAPS: case OCERZ_OP_MOVDQA: case OCERZ_OP_MOVDQU:
    case OCERZ_OP_MOVSS: case OCERZ_OP_MOVSDX:
    case OCERZ_OP_ADDSS: case OCERZ_OP_ADDSD: case OCERZ_OP_ADDPS: case OCERZ_OP_ADDPD:
    case OCERZ_OP_SUBSS: case OCERZ_OP_SUBSD: case OCERZ_OP_SUBPS: case OCERZ_OP_SUBPD:
    case OCERZ_OP_MULSS: case OCERZ_OP_MULSD: case OCERZ_OP_MULPS: case OCERZ_OP_MULPD:
    case OCERZ_OP_DIVSS: case OCERZ_OP_DIVSD: case OCERZ_OP_DIVPS: case OCERZ_OP_DIVPD:
    case OCERZ_OP_MAXSS: case OCERZ_OP_MAXSD: case OCERZ_OP_MINSS: case OCERZ_OP_MINSD:
    case OCERZ_OP_SQRTSS: case OCERZ_OP_SQRTSD: case OCERZ_OP_SQRTPS: case OCERZ_OP_SQRTPD:
    case OCERZ_OP_PXOR: case OCERZ_OP_XORPS: case OCERZ_OP_PAND: case OCERZ_OP_ANDPS:
    case OCERZ_OP_POR: case OCERZ_OP_ORPS: case OCERZ_OP_PANDN: case OCERZ_OP_ANDNPS:
    case OCERZ_OP_PADDB: case OCERZ_OP_PADDW: case OCERZ_OP_PADDD: case OCERZ_OP_PADDQ:
    case OCERZ_OP_PSUBB: case OCERZ_OP_PSUBW: case OCERZ_OP_PSUBD: case OCERZ_OP_PSUBQ:
    case OCERZ_OP_PCMPEQB: case OCERZ_OP_PCMPEQW: case OCERZ_OP_PCMPEQD: case OCERZ_OP_PCMPEQQ:
    case OCERZ_OP_PCMPGTD:
    case OCERZ_OP_UCOMISS: case OCERZ_OP_UCOMISD: case OCERZ_OP_COMISS: case OCERZ_OP_COMISD:
    case OCERZ_OP_CVTTSD2SI: case OCERZ_OP_CVTTSS2SI: case OCERZ_OP_CVTSI2SD: case OCERZ_OP_CVTSI2SS:
    case OCERZ_OP_CVTSD2SS: case OCERZ_OP_CVTSS2SD: case OCERZ_OP_CVTDQ2PS:
    case OCERZ_OP_MOVD:
    case OCERZ_OP_UNPCKLPD: case OCERZ_OP_UNPCKHPD: case OCERZ_OP_MOVLHPS: case OCERZ_OP_MOVHLPS:
    case OCERZ_OP_UNPCKLPS: case OCERZ_OP_UNPCKHPS:
    case OCERZ_OP_CMPSS: case OCERZ_OP_CMPSDX:
    case OCERZ_OP_BLENDVPD: case OCERZ_OP_BLENDVPS: case OCERZ_OP_PBLENDVB:
        return emit_sse(b, insn, exit_sites, n_exits);
    case OCERZ_OP_IMUL:
        if (insn->ops[0].kind == OCERZ_OPK_MEM ||
            (insn->nops > 1 && insn->ops[1].kind == OCERZ_OPK_MEM) ||
            (insn->nops > 2 && insn->ops[2].kind == OCERZ_OPK_MEM))
            return 0;
        return emit_imul(b, insn, need);
    case OCERZ_OP_LEA:
        return emit_lea(b, insn);
    case OCERZ_OP_PUSH:
    case OCERZ_OP_POP:
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

static uint32_t *emit_chain_tail(A64Buf *b, int poll);
static uint32_t *emit_body_chain_tail(A64Buf *b, uint64_t target_rip, int poll,
                                      uint32_t **epilogue_sites, int *n_epi);
static uint32_t *emit_static_chain_tail(A64Buf *b, uint64_t target_rip,
                                        int poll, int body_edge,
                                        uint32_t **epilogue_sites, int *n_epi);

static int fused_jcc_cond(const X86Insn *producer, const X86Insn *jcc)
{
    if (producer->op == OCERZ_OP_TEST) {
        if (jcc->cc == OCERZ_CC_E)  return A64_EQ;
        if (jcc->cc == OCERZ_CC_NE) return A64_NE;
        return -1;
    }

    static const int cmp_cond[16] = {
        A64_VS, A64_VC, A64_CC, A64_CS,
        A64_EQ, A64_NE, A64_LS, A64_HI,
        A64_MI, A64_PL, -1,     -1,
        A64_LT, A64_GE, A64_LE, A64_GT,
    };
    return jcc->cc < 16 ? cmp_cond[jcc->cc] : -1;
}

static int can_fuse_cmp_test_jcc(const X86Insn *producer,
                                 const X86Insn *jcc, uint64_t block_rip)
{
    if (g_no_jccfuse || g_no_regflags || g_no_chain ||
        jcc->op != OCERZ_OP_JCC || jcc->ops[0].kind != OCERZ_OPK_IMM ||
        (jcc->ops[0].imm != block_rip && g_no_jcclink) ||
        fused_jcc_cond(producer, jcc) < 0)
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

/* Registers written by a simple instruction (for gap-fusion legality). */
static int insn_writes_reg(const X86Insn *in, unsigned reg)
{
    if (in->nops == 0) return 0;
    const X86Operand *d = &in->ops[0];
    return d->kind == OCERZ_OPK_REG && (d->reg & 15) == (reg & 15);
}
/* Pure predicate: can emit_flag_neutral handle `in`?  (must be decided before
 * any code is emitted -- exit sites etc. cannot be rolled back) */
static int flag_neutral_ok(const X86Insn *in)
{
    if (g_pin_class != 3) return 0;
    if (in->op == OCERZ_OP_LEA) {
        /* exactly the emit_lea fast paths (which touch only pinned regs);
         * its fallback path clobbers JT0/JT2, which may hold the record */
        const X86Operand *d = &in->ops[0], *m = &in->ops[1];
        if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
        if (m->kind != OCERZ_OPK_MEM || m->riprel || in->addrsize != 8 || in->seg != OCERZ_SEG_NONE) return 0;
        if (m->base == OCERZ_REG_NONE || pin_slot(m->base) < 0) return 0;
        int has_idx = m->index != OCERZ_REG_NONE;
        if (has_idx && pin_slot(m->index) < 0) return 0;
        if (m->disp >= -4095 && m->disp <= 4095) return 1;          /* both fast paths */
        return has_idx && m->disp == 0;
    }
    if (in->op == OCERZ_OP_MOV) {
        const X86Operand *d = &in->ops[0], *s = &in->ops[1];
        if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
        if (s->kind == OCERZ_OPK_REG) return !s->high8 && s->size == d->size;
        return s->kind == OCERZ_OPK_IMM;
    }
    return 0;
}
/* Emit a flag-NEUTRAL instruction (host NZCV must survive): only shapes whose
 * emitters use mov/add/lsl/ubfx/sxt* without S-forms.  Returns 0 if not safe. */
static int emit_flag_neutral(A64Buf *b, const X86Insn *in)
{
    switch (in->op) {
    case OCERZ_OP_LEA:
        return emit_lea(b, in);                       /* add/lsl/mov only */
    case OCERZ_OP_MOV: {
        const X86Operand *d = &in->ops[0], *s = &in->ops[1];
        if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
        if (s->kind == OCERZ_OPK_REG) {
            if (s->high8 || s->size != d->size) return 0;
            int ds = pin_slot(d->reg), ss = pin_slot(s->reg);
            if (ds < 0 || ss < 0 || (g_pin_class == 2 && (d->reg == OCERZ_RSP || s->reg == OCERZ_RSP))) return 0;
            if (ds != ss || d->size == 4) a64_mov_reg(b, d->size == 8, pin_hreg(ds), pin_hreg(ss));
            return 1;
        }
        if (s->kind == OCERZ_OPK_IMM) {
            int ds = pin_slot(d->reg);
            if (ds < 0 || (g_pin_class == 2 && d->reg == OCERZ_RSP)) return 0;
            uint64_t v = s->imm; if (d->size == 4) v &= 0xffffffffull;
            a64_mov_imm64(b, pin_hreg(ds), v);        /* movz/movk: no flags */
            return 1;
        }
        return 0;
    }
    default:
        return 0;
    }
}

static int emit_cmp_test_jcc(A64Buf *b, const X86Insn *producer,
                             const X86Insn *jcc,
                             uint32_t **epilogue_sites, int *n_epi,
                             uint32_t **jcc_label,
                             uint32_t **exit_sites, int *n_exits,
                             const X86Insn *gap, uint32_t **gap_label)
{
    if (!can_fuse_cmp_test_jcc(producer, jcc, g_self_rip) || !g_defer)
        return 0;
    /* gap fusion (cmp ; neutral ; jcc): the neutral insn must not write a
     * register the compare read as a live record operand, and must be one of
     * the flag-neutral shapes.  With a memory operand the record lives in JT
     * registers, so any lea/mov register write is fine. */
    if (gap) {
        const X86Operand *pd = &producer->ops[0], *ps = &producer->ops[1];
        if (pd->kind == OCERZ_OPK_REG && insn_writes_reg(gap, pd->reg)) return 0;
        if (ps->kind == OCERZ_OPK_REG && insn_writes_reg(gap, ps->reg)) return 0;
        /* memory-operand compares also read base/index registers */
        const X86Operand *pm = pd->kind == OCERZ_OPK_MEM ? pd : ps->kind == OCERZ_OPK_MEM ? ps : NULL;
        (void)pm;   /* the load happens BEFORE the gap, so later base/index writes are fine */
        if (!flag_neutral_ok(gap)) return 0;
    }

    const X86Operand *d = &producer->ops[0];
    const X86Operand *s = &producer->ops[1];
    int sf = d->size == 8;
    uint64_t taken = jcc->ops[0].imm;
    uint64_t fall = jcc->rip + jcc->len;
    int self_loop = taken == g_self_rip;
    int test_bit = -1;
    int test_rn = -1;
    uint64_t test_mask = 0;
    int record_src = JT2;
    int record_dst = JT2;
    uint32_t ccop;
    int d_mem = d->kind == OCERZ_OPK_MEM, s_mem = s->kind == OCERZ_OPK_MEM;
    /* memory operand (at most one) -> loaded up front into JT0 (dst) or JT1 (src),
     * zero-extended to the operand size; then treated like a register held there */
    int d_in_jt0 = 0, s_in_jt1 = 0;
    if (d_mem || s_mem) {
        const X86Operand *m = d_mem ? d : s;
        int into = d_mem ? JT0 : JT1;
        if (!emit_mem_ea(b, producer, m, JTA)) return 0;
        uint32_t *skip = emit_commpage_guard(b, producer, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
        emit_guest_load_ordered(b, d->size, into, JTA, JTU);
        patch_guard_skip(skip, a64_label(b));
        if (d_mem) d_in_jt0 = 1; else s_in_jt1 = 1;
    }
    if (d->size == 1 || d->size == 2) {
        /* narrow: NZCV from a 32-bit op on left-shifted operands, flag record
         * from the unshifted zero-extended values at the narrow size. */
        int size = d->size, sh = 32 - 8 * size;
        uint64_t mask = size == 1 ? 0xffull : 0xffffull;
        if (!d_in_jt0) emit_gpr_rd(b, 1, JT0, d->reg);
        if (size == 1) a64_uxtb(b, JT0, JT0); else a64_uxth(b, JT0, JT0);
        if (s_in_jt1) {
            if (size == 1) a64_uxtb(b, JT1, JT1); else a64_uxth(b, JT1, JT1);
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
            a64_ands_reg(b, 0, JT2, JTA, JTU, 0);      /* NZ from shifted; result in JT2 (shifted) */
            a64_lsr_imm(b, 0, JT2, JT2, sh);            /* unshift for the record */
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
            a64_mov_imm64(b, JT1, v);
            record_dst = JT1;
        } else {
            return 0;
        }
        a64_subs_reg(b, sf, A64_ZR, record_src, record_dst, 0);
        ccop = ocerz_cc_pack(OCERZ_CC_SUB, d->size, 0);
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
            if (!self_loop && v != 0 && (v & (v - 1)) == 0) {
                test_bit = __builtin_ctzll(v);
                test_rn = rn;
                test_mask = v;
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
    if (gap) {
        /* NZCV is live now; the record operands (JT0/JT1 or pinned regs) must
         * survive -- guaranteed by the checks above. */
        uint32_t *gl = a64_label(b);
        int ok = emit_flag_neutral(b, gap);
        assert(ok && "flag_neutral_ok admitted an unhandled shape");
        (void)ok;
        if (gap_label) *gap_label = gl;
    }
    *jcc_label = a64_label(b);
    int taken_cond = fused_jcc_cond(producer, jcc);

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
        } else {
            a64_bcond(b, taken_cond, 0);
        }

        if (g_no_xlive || xlive_succ_live(g_xlat_jit, fall) != 0) {
            if (producer->op == OCERZ_OP_CMP)
                emit_defer_flags(b, ccop, record_src, record_dst);
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
        else
            a64_patch_bcond(to_taken, taken_label);
        if (g_no_xlive || xlive_succ_live(g_xlat_jit, taken) != 0) {
            if (producer->op == OCERZ_OP_CMP)
                emit_defer_flags(b, ccop, record_src, record_dst);
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
        g_jcc_edge[0].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
        g_jcc_edge[0].pin_class = body_edge ? (uint8_t)edge_class : 0;
        g_jcc_edge[1].target_rip = taken;
        g_jcc_edge[1].patch_b = pb_taken;
        g_jcc_edge[1].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
        g_jcc_edge[1].pin_class = body_edge ? (uint8_t)edge_class : 0;
        g_n_jcc_edges = 2;
        return 1;
    }

    if (!g_loop_entry)
        return 0;

    g_stop_patch = a64_label(b);
    a64_bcond(b, taken_cond, (int32_t)(g_loop_entry - g_stop_patch));

    if (g_no_xlive || xlive_succ_live(g_xlat_jit, fall) != 0) {
        if (producer->op == OCERZ_OP_CMP)
            emit_defer_flags(b, ccop, record_src, record_dst);
        else
            emit_defer_flags(b, ccop, JT2, JT2);
    }
    a64_mov_imm64(b, JT0, fall);
    uint32_t *fall_to_common = a64_label(b);
    a64_b(b, 0);

    g_stop_target = a64_label(b);
    if (g_no_xlive || xlive_succ_live(g_xlat_jit, taken) != 0) {
        if (producer->op == OCERZ_OP_CMP)
            emit_defer_flags(b, ccop, record_src, record_dst);
        else
            emit_defer_flags(b, ccop, JT2, JT2);
    }
    a64_mov_imm64(b, JT0, taken);

    uint32_t *common = a64_label(b);
    a64_patch_b(fall_to_common, common);
    a64_str(b, 8, JT0, 20, RIP_OFF);
    a64_mov_imm64(b, 0, OCERZ_STEP_OK);
    epilogue_sites[*n_epi] = a64_label(b);
    a64_b(b, 0);
    (*n_epi)++;
    return 1;
}

static int can_fuse_incdec_jcc(const X86Insn *producer, const X86Insn *jcc)
{
    if (g_no_jccfuse || g_no_regflags || g_no_chain || g_no_jcclink ||
        jcc->op != OCERZ_OP_JCC || jcc->ops[0].kind != OCERZ_OPK_IMM ||
        (jcc->cc != OCERZ_CC_E && jcc->cc != OCERZ_CC_NE) ||
        (producer->op != OCERZ_OP_INC && producer->op != OCERZ_OP_DEC))
        return 0;
    const X86Operand *d = &producer->ops[0];
    return d->kind == OCERZ_OPK_REG && !d->high8 &&
           (d->size == 4 || d->size == 8);
}

static uint32_t *emit_incdec_jcc_arm(A64Buf *b, const X86Insn *producer,
                                     uint64_t target, int poll, int body_edge,
                                     int cf_reg,
                                     uint32_t **epilogue_sites, int *n_epi)
{
    uint64_t live = g_no_xlive ? (uint64_t)OCERZ_FL_ALL
                               : xlive_succ_live(g_xlat_jit, target);
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

static int emit_incdec_jcc(A64Buf *b, const X86Insn *producer,
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
    int edge_class = body_edge_pin_class();
    int body_edge = edge_class >= 0;
    uint32_t *to_taken = a64_label(b);
    a64_bcond(b, taken_cond, 0);

    uint32_t *pb_fall = emit_incdec_jcc_arm(
        b, producer, fall, fall <= g_self_rip,
        body_edge, -1, epilogue_sites, n_epi);
    uint32_t *taken_label = a64_label(b);
    a64_patch_bcond(to_taken, taken_label);
    uint32_t *pb_taken = emit_incdec_jcc_arm(
        b, producer, taken, taken <= g_self_rip,
        body_edge, -1, epilogue_sites, n_epi);

    g_jcc_edge[0].target_rip = fall;
    g_jcc_edge[0].patch_b = pb_fall;
    g_jcc_edge[0].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
    g_jcc_edge[0].pin_class = body_edge ? (uint8_t)edge_class : 0;
    g_jcc_edge[1].target_rip = taken;
    g_jcc_edge[1].patch_b = pb_taken;
    g_jcc_edge[1].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
    g_jcc_edge[1].pin_class = body_edge ? (uint8_t)edge_class : 0;
    g_n_jcc_edges = 2;
    return 1;
}

static int emit_arith_incdec_jcc(A64Buf *b, const X86Insn *arith,
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
    int edge_class = body_edge_pin_class();
    int body_edge = edge_class >= 0;
    uint32_t *to_taken = a64_label(b);
    a64_bcond(b, taken_cond, 0);

    uint32_t *pb_fall = emit_incdec_jcc_arm(
        b, incdec, fall, fall <= g_self_rip,
        body_edge, JT0, epilogue_sites, n_epi);
    uint32_t *taken_label = a64_label(b);
    a64_patch_bcond(to_taken, taken_label);
    uint32_t *pb_taken = emit_incdec_jcc_arm(
        b, incdec, taken, taken <= g_self_rip,
        body_edge, JT0, epilogue_sites, n_epi);

    g_jcc_edge[0].target_rip = fall;
    g_jcc_edge[0].patch_b = pb_fall;
    g_jcc_edge[0].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
    g_jcc_edge[0].pin_class = body_edge ? (uint8_t)edge_class : 0;
    g_jcc_edge[1].target_rip = taken;
    g_jcc_edge[1].patch_b = pb_taken;
    g_jcc_edge[1].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
    g_jcc_edge[1].pin_class = body_edge ? (uint8_t)edge_class : 0;
    g_n_jcc_edges = 2;
    return 1;
}

static int emit_logic_jmp_incdec_jcc(A64Buf *b, const X86Insn *logic,
                                     const X86Insn *jmp,
                                     uint64_t logic_need,
                                     uint32_t **epilogue_sites, int *n_epi,
                                     uint32_t **jmp_label)
{
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
    if (sigsetjmp(db, 1) == 0) {
        ocerz_jit_decode_recover = &db;
        while (n < 2) {
            int rc = ocerz_decode((const uint8_t *)ocerz_g2h(pc), 15, pc,
                                  &target[n]);
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
    uint32_t *to_taken = a64_label(b);
    a64_bcond(b, taken_cond, 0);

    uint32_t *pb_fall = emit_incdec_jcc_arm(
        b, incdec, fall, fall <= g_self_rip, body_edge, A64_ZR,
        epilogue_sites, n_epi);
    uint32_t *taken_label = a64_label(b);
    a64_patch_bcond(to_taken, taken_label);
    uint32_t *pb_taken = emit_incdec_jcc_arm(
        b, incdec, taken, taken <= g_self_rip, body_edge, A64_ZR,
        epilogue_sites, n_epi);

    g_jcc_edge[0].target_rip = fall;
    g_jcc_edge[0].patch_b = pb_fall;
    g_jcc_edge[0].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
    g_jcc_edge[0].pin_class = body_edge ? (uint8_t)edge_class : 0;
    g_jcc_edge[1].target_rip = taken;
    g_jcc_edge[1].patch_b = pb_taken;
    g_jcc_edge[1].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
    g_jcc_edge[1].pin_class = body_edge ? (uint8_t)edge_class : 0;
    g_n_jcc_edges = 2;
    return 1;
}

typedef struct IfConvDiamond {
    X86Insn direct[3];
    X86Insn nested[2];
    X86Insn simple[2];
    X86Insn complex[4];
    int direct_is_taken;
    int simple_is_taken;
    int first_bit;
    int nested_bit;
    uint64_t exit_rip;
} IfConvDiamond;

static int decode_ifconv_block(uint64_t rip, X86Insn *out, int cap)
{
    volatile int n = 0;
    volatile int terminated = 0;
    volatile uint64_t pc = rip;
    sigjmp_buf db;
    sigjmp_buf *prev = ocerz_jit_decode_recover;
    if (sigsetjmp(db, 1) == 0) {
        ocerz_jit_decode_recover = &db;
        while (n < cap) {
            if (ocerz_decode((const uint8_t *)ocerz_g2h(pc), 15, pc,
                             &out[n]) != OCERZ_OK)
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

static int emit_ifconv_diamond(A64Buf *b, const X86Insn *test,
                               const X86Insn *jcc,
                               uint32_t **epilogue_sites, int *n_epi,
                               uint32_t **jcc_label)
{
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
    g_stop_patch = a64_label(b);
    a64_bcond(b, self_cond, (int32_t)(g_loop_entry - g_stop_patch));

    int edge_class = body_edge_pin_class();
    int body_edge = edge_class >= 0;
    uint32_t *pb_exit = emit_incdec_jcc_arm(
        b, latch, m.exit_rip, m.exit_rip <= g_self_rip,
        body_edge, JT0, epilogue_sites, n_epi);

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

static int emit_jcc(A64Buf *b, const X86Insn *insn, uint32_t **epilogue_sites, int *n_epi)
{
    if (insn->op != OCERZ_OP_JCC)
        return 0;
    unsigned cc = insn->cc;
    uint64_t taken = insn->ops[0].imm;
    uint64_t fall = insn->rip + insn->len;

    /* NZCV: NE <=> taken.  Inline evaluation of a pending cmp/test record
     * (no C call) or, failing that, materialize + RFLAGS. */
    emit_cc_predicate(b, cc);
    int self_loop = !g_no_chain && g_loop_entry && taken == g_self_rip;
    int two_way = !self_loop && !g_no_chain && !g_no_jcclink;
    if (!two_way) {
        a64_mov_imm64(b, JT1, fall);
        a64_mov_imm64(b, JT2, taken);
        a64_csel(b, 1, JT0, JT2, JT1, A64_NE);   /* NE <=> taken (negation folded) */
        a64_str(b, 8, JT0, 20, RIP_OFF);
    }

    if (self_loop) {
        a64_mov_imm64(b, JT1, g_self_rip);
        a64_subs_reg(b, 1, A64_ZR, JT0, JT1, 0);
        g_stop_patch = a64_label(b);
        a64_bcond(b, A64_EQ, (int32_t)(g_loop_entry - g_stop_patch));
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
        int taken_eq = 0;   /* predicate already folds the negated forms */
        uint32_t *to_taken = a64_label(b);
        a64_bcond(b, taken_eq ? A64_EQ : A64_NE, 0);

        uint32_t *pb_fall = emit_static_chain_tail(
            b, fall, poll_fall, body_edge, epilogue_sites, n_epi);

        uint32_t *ltaken = a64_label(b);
        a64_patch_bcond(to_taken, ltaken);
        uint32_t *pb_taken = emit_static_chain_tail(
            b, taken, poll_taken, body_edge, epilogue_sites, n_epi);
        g_jcc_edge[0].target_rip = fall;
        g_jcc_edge[0].patch_b = pb_fall;
        g_jcc_edge[0].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
        g_jcc_edge[0].pin_class = body_edge ? (uint8_t)edge_class : 0;
        g_jcc_edge[1].target_rip = taken;
        g_jcc_edge[1].patch_b = pb_taken;
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

static int jmp_inline_enabled(void)
{
    static int en = -1;
    if (en < 0)
        en = getenv("OCERZ_NO_INLINE_JMP") ? 0 : 1;
    return en;
}

static int emit_jmp(A64Buf *b, const X86Insn *insn, uint32_t **epilogue_sites, int *n_epi)
{
    if (insn->op != OCERZ_OP_JMP || !jmp_inline_enabled())
        return 0;
    if (insn->ops[0].kind != OCERZ_OPK_IMM)
        return 0;
    uint64_t target = insn->ops[0].imm;

    if (!g_no_chain && g_loop_entry && target == g_self_rip) {
        g_stop_patch = a64_label(b);
        a64_b(b, (int32_t)(g_loop_entry - g_stop_patch));
        g_stop_target = a64_label(b);
        a64_mov_imm64(b, JT0, target);
        a64_str(b, 8, JT0, 20, RIP_OFF);
        a64_mov_imm64(b, 0, OCERZ_STEP_OK);
        epilogue_sites[*n_epi] = a64_label(b);
        a64_b(b, 0);
        (*n_epi)++;
        return 1;
    }

    if (!g_no_chain) {
        int poll = target <= g_self_rip;
        int edge_class = body_edge_pin_class();
        int body_edge = edge_class >= 0;
        uint32_t *pb = emit_static_chain_tail(
            b, target, poll, body_edge, epilogue_sites, n_epi);
        g_jcc_edge[0].target_rip = target;
        g_jcc_edge[0].patch_b = pb;
        g_jcc_edge[0].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
        g_jcc_edge[0].pin_class = body_edge ? (uint8_t)edge_class : 0;
        g_n_jcc_edges = 1;
        return 1;
    }

    a64_mov_imm64(b, JT0, target);
    a64_str(b, 8, JT0, 20, RIP_OFF);
    a64_mov_imm64(b, 0, OCERZ_STEP_OK);
    epilogue_sites[*n_epi] = a64_label(b);
    a64_b(b, 0);
    (*n_epi)++;
    return 1;
}

static int callret_inline_enabled(void)
{
    static int en = -1;
    if (en < 0)
        en = getenv("OCERZ_NO_INLINE_CALLRET") ? 0 : 1;
    return en;
}

/* Host entry to record for a return target: under full pinning the caller
 * and callee share the register layout, so return straight into the BODY
 * (tag bit 0 set) and skip the frame traffic; otherwise the function entry. */
static void *ras_entry_for(const JitBlock *blk)
{
    if (!blk || !blk->code) return NULL;
    if (blk->pin_class == 3 && blk->body_code)
        return (void *)((uintptr_t)blk->body_code | 1u);
    return (void *)blk->code;
}

void ocerz_ras_push(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t retaddr)
{
    uint32_t t = cpu->ras_top;
    if (t >= OCERZ_RAS_SIZE)
        return;
    JitBlock *blk = cache_lookup(vm->jit, retaddr);
    cpu->ras[t].guest_rip = retaddr;
    cpu->ras[t].host_entry = ras_entry_for(blk);
    cpu->ras_top = t + 1;
}

static void **ras_slot_alloc(void);
static void pending_add_ras(uint64_t target_rip, void **ras_slot);
static uint32_t *emit_body_chain_tail(A64Buf *b, uint64_t target_rip, int poll,
                                      uint32_t **epilogue_sites, int *n_epi);

static void patch_local_adr(uint32_t *at, uint32_t *target, int rd)
{
    ptrdiff_t off = (char *)target - (char *)at;
    assert(off >= -(1 << 20) && off < (1 << 20));
    uint32_t imm = (uint32_t)((uint64_t)off & 0x1fffffu);
    *at = 0x10000000u | ((imm & 3u) << 29) |
          (((imm >> 2) & 0x7ffffu) << 5) | (uint32_t)(rd & 31);
}

static void emit_step_epilogue_branch(A64Buf *b, uint32_t **epi_sites, int *n_epi)
{
    a64_mov_imm64(b, 0, OCERZ_STEP_OK);
    epi_sites[*n_epi] = a64_label(b);
    a64_b(b, 0);
    (*n_epi)++;
}

static int emit_call_region_call(A64Buf *b, const X86Insn *insn,
                                 uint32_t **exit_sites, int *n_exits,
                                 uint32_t **epi_sites, int *n_epi)
{
    if (g_pin_class != 2 || g_no_chain || !g_body_entry ||
        insn->ops[0].kind != OCERZ_OPK_IMM || !mem_native_store_ok())
        return 0;

    uint64_t retaddr = insn->rip + insn->len;
    uint64_t target = insn->ops[0].imm;
    int rs = pin_slot(OCERZ_RSP);
    assert(rs >= 0);

    a64_mov_imm64(b, JRET_GUEST, retaddr);
    uint32_t *skip = NULL;
    if (g_plain_mem && !ocerz_commpage) {
        a64_str_pre64(b, JRET_GUEST, pin_hreg(rs), -8);
    } else {
        a64_sub_imm(b, 1, JTA, pin_hreg(rs), 8);
        skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_guest_store_ordered(b, 8, JRET_GUEST, JTA, JTU);
        a64_sub_imm(b, 1, pin_hreg(rs), pin_hreg(rs), 8);
    }
    patch_guard_skip(skip, a64_label(b));

    emit_xmm_pin_spill_all(b);            /* callee body sees xmm in memory */
    uint32_t *adr = a64_label(b);
    a64_emit32(b, 0x10000000u | (uint32_t)JRET_HOST);
    uint32_t *callee_patch = a64_label(b);
    a64_b(b, 0);

    uint32_t *host_cont = a64_label(b);
    patch_local_adr(adr, host_cont, JRET_HOST);
    emit_xmm_pin_load_all(b);             /* back from the callee: reload pins */
    uint32_t *return_patch = emit_body_chain_tail(b, retaddr, 0,
                                                  epi_sites, n_epi);

    uint32_t *callee_fallback = a64_label(b);
    a64_patch_b(callee_patch, callee_fallback);

    if (target <= g_self_rip) {
        assert(!g_stop_patch);
        g_stop_patch = callee_patch;
        g_stop_target = callee_fallback;
    }
    a64_add_imm(b, 1, 31, 29, 0);
    a64_mov_imm64(b, JT0, target);
    a64_str(b, 8, JT0, 20, RIP_OFF);
    emit_step_epilogue_branch(b, epi_sites, n_epi);

    g_call_edge[0].target_rip = target;
    g_call_edge[0].patch_b = callee_patch;
    g_call_edge[0].kind = EDGE_BODY;
    g_call_edge[0].pin_class = 2;
    g_call_edge[1].target_rip = retaddr;
    g_call_edge[1].patch_b = return_patch;
    g_call_edge[1].kind = EDGE_BODY;
    g_call_edge[1].pin_class = 2;
    g_n_call_edges = 2;
    return 1;
}

static int emit_call_region_ret(A64Buf *b, const X86Insn *insn,
                                uint32_t **exit_sites, int *n_exits,
                                uint32_t **epi_sites, int *n_epi)
{
    if (g_pin_class != 2 || g_no_chain || !g_body_entry || insn->nops != 0)
        return 0;

    int rs = pin_slot(OCERZ_RSP);
    assert(rs >= 0);
    uint32_t *skip = NULL;
    if (g_plain_mem && !ocerz_commpage) {
        a64_ldr_post64(b, JT1, pin_hreg(rs), 8);
    } else {
        skip = emit_commpage_guard(b, insn, pin_hreg(rs),
                                   exit_sites, n_exits);
        emit_guest_load_ordered(b, 8, JT1, pin_hreg(rs), JTU);
        a64_add_imm(b, 1, pin_hreg(rs), pin_hreg(rs), 8);
    }

    uint32_t *check = a64_label(b);
    uint32_t *no_cache = a64_label(b);
    a64_cbz(b, 1, JRET_HOST, 0);
    a64_subs_reg(b, 1, A64_ZR, JRET_GUEST, JT1, 0);
    uint32_t *mismatch = a64_label(b);
    a64_bcond(b, A64_NE, 0);
    emit_xmm_pin_spill_all(b);
    a64_br(b, JRET_HOST);

    uint32_t *fallback = a64_label(b);
    a64_patch_cbz(no_cache, fallback);
    a64_patch_bcond(mismatch, fallback);
    a64_add_imm(b, 1, 31, 29, 0);
    a64_str(b, 8, JT1, 20, RIP_OFF);
    emit_step_epilogue_branch(b, epi_sites, n_epi);

    uint32_t *slow = a64_label(b);
    a64_ldr(b, 8, JT1, 20, RIP_OFF);
    uint32_t *to_check = a64_label(b);
    a64_b(b, 0);
    a64_patch_b(to_check, check);
    patch_guard_skip(skip, slow);
    return 1;
}

static int emit_call_ret(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites,
                         int *n_exits, uint32_t **epi_sites, int *n_epi)
{
    uint64_t gbase = ocerz_guest_base;

    if (!callret_inline_enabled())
        return 0;

    if (insn->seg != OCERZ_SEG_NONE)
        return 0;

    if (insn->op == OCERZ_OP_CALL &&
        emit_call_region_call(b, insn, exit_sites, n_exits, epi_sites, n_epi))
        return 1;
    if (insn->op == OCERZ_OP_RET &&
        emit_call_region_ret(b, insn, exit_sites, n_exits, epi_sites, n_epi))
        return 1;

    if (insn->op == OCERZ_OP_CALL) {
        if (insn->ops[0].kind != OCERZ_OPK_IMM)
            return 0;
        if (!mem_native_store_ok())
            return 0;

        uint64_t retaddr = insn->rip + insn->len;
        uint64_t target = insn->ops[0].imm;

        g_chain_target = target;

        a64_mov_imm64(b, JT1, retaddr);

        emit_gpr_rd(b, 1, JT0, OCERZ_RSP);
        a64_sub_imm(b, 1, JTA, JT0, 8);
        emit_add_const(b, JTA, ea_fold());

        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, gbase - ea_fold());

        emit_guest_store_ordered(b, 8, JT1, JTA, JTU);
        a64_sub_imm(b, 1, JT0, JT0, 8);
        emit_gpr_wr(b, JT0, OCERZ_RSP);

        a64_mov_imm64(b, JT0, target);
        a64_str(b, 8, JT0, 20, RIP_OFF);
        patch_guard_skip(skip, a64_label(b));

        if (!g_no_ras) {

            void **slot = ras_slot_alloc();
            if (slot) {
                JitBlock *rb = cache_lookup(g_xlat_jit, retaddr);
                if (rb && rb->code)
                    *slot = ras_entry_for(rb);
                else
                    pending_add_ras(retaddr, slot);
                a64_ldr(b, 4, JT2, 20, RAS_TOP_OFF);
                a64_subs_imm(b, 0, A64_ZR, JT2, OCERZ_RAS_SIZE);
                uint32_t *full = a64_label(b);
                a64_bcond(b, A64_CS, 0);
                a64_mov_imm64(b, JT1, retaddr);
                a64_mov_imm64(b, JTA, (uint64_t)(uintptr_t)slot);
                a64_ldr(b, 8, JT0, JTA, 0);
                a64_lsl_imm(b, 1, JTA, JT2, 4);
                a64_add_reg(b, 1, JTA, JTA, 20, 0);
                a64_str(b, 8, JT1, JTA, RAS_OFF);
                a64_str(b, 8, JT0, JTA, RAS_OFF + 8);
                a64_add_imm(b, 0, JT2, JT2, 1);
                a64_str(b, 4, JT2, 20, RAS_TOP_OFF);
                a64_patch_bcond(full, a64_label(b));
            } else {
                emit_xmm_pin_spill_all(b);
                emit_spill_pinned_callersaved(b);
                a64_mov_reg(b, 1, 0, 19);
                a64_mov_reg(b, 1, 1, 20);
                a64_mov_imm64(b, 2, retaddr);
                a64_mov_imm64(b, 16, (uint64_t)(uintptr_t)&ocerz_ras_push);
                a64_blr(b, 16);
                emit_fill_pinned_callersaved(b);
                emit_xmm_pin_load_all(b);
            }
        }
    } else if (insn->op == OCERZ_OP_RET) {
        if (insn->nops != 0)
            return 0;

        emit_gpr_rd(b, 1, JT0, OCERZ_RSP);
        a64_mov_reg(b, 1, JTA, JT0);
        emit_add_const(b, JTA, ea_fold());

        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, gbase - ea_fold());
        emit_guest_load_ordered(b, 8, JT1, JTA, JTU);

        a64_add_imm(b, 1, JT0, JT0, 8);
        emit_gpr_wr(b, JT0, OCERZ_RSP);
        a64_str(b, 8, JT1, 20, RIP_OFF);
        patch_guard_skip(skip, a64_label(b));

        if (!g_no_ras) {
            uint32_t *ras_empty;
            uint32_t *ras_stale[3];
            int nst = 0;
            a64_ldr(b, 4, JT2, 20, RAS_TOP_OFF);
            ras_empty = a64_label(b); a64_cbz(b, 0, JT2, 0);
            a64_sub_imm(b, 0, JT2, JT2, 1);
            a64_lsl_imm(b, 1, JTA, JT2, 4);
            a64_add_reg(b, 1, JTA, JTA, 20, 0);
            a64_ldr(b, 8, JTF, JTA, RAS_OFF);
            a64_ldr(b, 8, JT0, JTA, RAS_OFF + 8);
            a64_ldr(b, 8, JTU, 20, RIP_OFF);
            a64_sub_reg(b, 1, JTT, JTF, JTU, 0);
            ras_stale[nst] = a64_label(b); a64_cbnz(b, 1, JTT, 0); nst++;
            ras_stale[nst] = a64_label(b); a64_cbz(b, 1, JT0, 0); nst++;

            a64_str(b, 4, JT2, 20, RAS_TOP_OFF);

            uint32_t *not_body = NULL;
            if (g_pin_class == 3) {
                /* tagged entry = callee body with our own register layout */
                not_body = a64_label(b); a64_tbz(b, JT0, 0, 0);
                a64_mov_imm64(b, JTU, 1);
                a64_bic_reg(b, 1, JT0, JT0, JTU, 0);
                if (!xmm_global_enabled()) emit_xmm_pin_spill_all(b);
                a64_br(b, JT0);
                a64_patch_tbz(not_body, a64_label(b));
                a64_mov_imm64(b, JTU, 1);
                a64_bic_reg(b, 1, JT0, JT0, JTU, 0);
            } else {
                /* a tagged (body) entry is only usable from a class-3 block:
                 * treat it as a RAS miss here */
                ras_stale[nst] = a64_label(b); a64_tbnz(b, JT0, 0, 0); nst++;
            }
            emit_xmm_pin_spill_all(b);
            emit_spill_pinned(b);
            a64_mov_reg(b, 1, 0, 19);
            a64_mov_reg(b, 1, 1, 20);
            emit_pin_epilogue_restore(b);
            a64_ldp_post(b, 19, 20, 31, 16);
            a64_ldp_post(b, 29, 30, 31, 16);
            a64_br(b, JT0);

            uint32_t *miss_pop = a64_label(b);
            a64_str(b, 4, JT2, 20, RAS_TOP_OFF);
            uint32_t *miss = a64_label(b);
            a64_patch_cbz(ras_empty, miss);
            for (int i = 0; i < nst; i++) {
                if ((*ras_stale[i] & 0x7f000000u) == 0x36000000u ||
                    (*ras_stale[i] & 0x7f000000u) == 0x37000000u)
                    a64_patch_tbz(ras_stale[i], miss_pop);   /* tbz/tbnz */
                else
                    a64_patch_cbz(ras_stale[i], miss_pop);
            }
        }
    } else {
        return 0;
    }

    a64_mov_imm64(b, 0, OCERZ_STEP_OK);
    epi_sites[*n_epi] = a64_label(b);

    g_chain_epi = epi_sites[*n_epi];
    a64_b(b, 0);
    (*n_epi)++;
    return 1;
}

/* Emit: target(JT1) -> RIP; monomorphic IC probe; on miss an inline hashed
 * lookup of the block cache (walks up to 4 chain entries); found -> br host
 * code; else C fill of the IC slot + epilogue to the dispatcher. */
/* Global dispatch stub.  Entered like a block (x0=vm, x1=cpu, no frame of
 * its own).  If the CPU must stop (interrupt/terminated) or the VM exited,
 * returns to C with x0 = STEP_OK so the run loop sees the flags.  Otherwise
 * looks the current rip up in the block cache and br's to the compiled
 * block; on a miss returns to C (which compiles it).  Blocks branch here
 * instead of ret'ing, so a hot loop never re-enters ocerz_jit_step. */
static void emit_dispatch_stub(OcerzJit *jit)
{
    A64Buf b = { jit->code_cur, jit->code_cur, jit->code_end, 0, 0 };
    uint32_t *entry = b.p;
    uint32_t *to_ret[6]; int nr = 0;
    a64_ldr(&b, 4, JT0, 1, INT_OFF);
    to_ret[nr++] = a64_label(&b); a64_cbnz(&b, 0, JT0, 0);
    a64_ldr(&b, 4, JT0, 1, (uint32_t)offsetof(OcerzCPU, terminated));
    to_ret[nr++] = a64_label(&b); a64_cbnz(&b, 0, JT0, 0);
    a64_ldr(&b, 4, JT0, 0, (uint32_t)offsetof(struct OcerzVM, exited));
    to_ret[nr++] = a64_label(&b); a64_cbnz(&b, 0, JT0, 0);
    a64_ldr(&b, 8, JT1, 1, RIP_OFF);                    /* JT1 = rip */
    /* dyldapi range must go through C */
    a64_mov_imm64(&b, JTU, OCERZ_DYLDAPI_LO);
    a64_sub_reg(&b, 1, JTT, JT1, JTU, 0);
    a64_mov_imm64(&b, JTU, OCERZ_DYLDAPI_HI - OCERZ_DYLDAPI_LO);
    a64_subs_reg(&b, 1, A64_ZR, JTT, JTU, 0);
    to_ret[nr++] = a64_label(&b); a64_bcond(&b, A64_CC, 0);
    /* hash */
    a64_lsr_imm(&b, 1, JTT, JT1, 33);
    a64_eor_reg(&b, 1, JTT, JTT, JT1, 0);
    a64_mov_imm64(&b, JTU, 0xff51afd7ed558ccdull);
    a64_mul(&b, 1, JTT, JTT, JTU);
    a64_lsr_imm(&b, 1, JTU, JTT, 29);
    a64_eor_reg(&b, 1, JTT, JTT, JTU, 0);
    a64_mov_imm64(&b, JTU, JIT_HASH_MASK);
    a64_and_reg(&b, 1, JTT, JTT, JTU, 0);
    a64_mov_imm64(&b, JTA, (uint64_t)(uintptr_t)jit->buckets);
    a64_ldr_regoff(&b, 8, JTF, JTA, JTT, 1);
    for (int k = 0; k < 6; k++) {
        to_ret[nr] = a64_label(&b); a64_cbz(&b, 1, JTF, 0);
        if (nr < 5) nr++;   /* keep last slot for chain-end */
        a64_ldr(&b, 8, JTU, JTF, (uint32_t)offsetof(JitBlock, guest_rip));
        a64_sub_reg(&b, 1, JTU, JTU, JT1, 0);
        uint32_t *nxt = a64_label(&b); a64_cbnz(&b, 1, JTU, 0);
        a64_ldr(&b, 8, JT0, JTF, (uint32_t)offsetof(JitBlock, code));
        uint32_t *nocode = a64_label(&b); a64_cbz(&b, 1, JT0, 0);
        /* found & compiled: x0=vm, x1=cpu are untouched (we only used x9-x15) */
        a64_br(&b, JT0);
        uint32_t *cont = a64_label(&b);
        a64_patch_cbz(nxt, cont);
        a64_patch_cbz(nocode, cont);
        a64_ldr(&b, 8, JTF, JTF, (uint32_t)offsetof(JitBlock, hnext));
    }
    uint32_t *retl = a64_label(&b);
    for (int i = 0; i < nr; i++) {
        uint32_t w = *to_ret[i];
        if ((w & 0xff000010u) == 0x54000000u) a64_patch_bcond(to_ret[i], retl);
        else a64_patch_cbz(to_ret[i], retl);
    }
    a64_mov_imm64(&b, 0, OCERZ_STEP_OK);
    a64_ret(&b);
    if (!b.overflow) {
        jit->dispatch_stub = entry;
        jit->code_cur = b.p;
        sys_icache_invalidate(entry, (size_t)((uint8_t *)b.p - (uint8_t *)entry));
    }
}

static void emit_indirect_leave_br(A64Buf *b, int code_reg)
{
    emit_xmm_pin_spill_all(b);
    emit_spill_pinned(b);
    a64_mov_reg(b, 1, 0, 19);
    a64_mov_reg(b, 1, 1, 20);
    emit_pin_epilogue_restore(b);
    a64_ldp_post(b, 19, 20, 31, 16);
    a64_ldp_post(b, 29, 30, 31, 16);
    a64_br(b, code_reg);
}

/* Shared "leave the block and enter target function entry in JT0" stub for the
 * current block layout is not shareable across pin classes; keep it inline
 * only on the rare non-body path.  The hot path is compact: a looped hashed
 * lookup that br's into the target BODY when it has our pin class. */
static void emit_indirect_tail(A64Buf *b, JitIcSlot *slot,
                               uint32_t **epi_sites, int *n_epi)
{
    a64_str(b, 8, JT1, 20, RIP_OFF);
    /* hash */
    a64_lsr_imm(b, 1, JTT, JT1, 33);
    a64_eor_reg(b, 1, JTT, JTT, JT1, 0);
    a64_mov_imm64(b, JTU, 0xff51afd7ed558ccdull);
    a64_mul(b, 1, JTT, JTT, JTU);
    a64_lsr_imm(b, 1, JTU, JTT, 29);
    a64_eor_reg(b, 1, JTT, JTT, JTU, 0);
    a64_mov_imm64(b, JTU, JIT_HASH_MASK);
    a64_and_reg(b, 1, JTT, JTT, JTU, 0);
    a64_mov_imm64(b, JTA, (uint64_t)(uintptr_t)g_xlat_jit->buckets);
    a64_ldr_regoff(b, 8, JTF, JTA, JTT, 1);               /* JTF = bucket head */
    /* loop: */
    uint32_t *loop = a64_label(b);
    uint32_t *to_nofind = a64_label(b); a64_cbz(b, 1, JTF, 0);
    a64_ldr(b, 8, JTU, JTF, (uint32_t)offsetof(JitBlock, guest_rip));
    a64_sub_reg(b, 1, JTU, JTU, JT1, 0);
    uint32_t *found = a64_label(b); a64_cbz(b, 1, JTU, 0);
    a64_ldr(b, 8, JTF, JTF, (uint32_t)offsetof(JitBlock, hnext));
    { uint32_t *here = a64_label(b); a64_b(b, (int32_t)(loop - here)); }
    /* found: */
    a64_patch_cbz(found, a64_label(b));
    uint32_t *to_full = NULL;
    if (g_pin_class == 1 || g_pin_class == 3) {
        a64_ldr(b, 1, JTU, JTF, (uint32_t)offsetof(JitBlock, pin_class));
        a64_sub_imm(b, 0, JTU, JTU, (uint32_t)g_pin_class);
        to_full = a64_label(b); a64_cbnz(b, 0, JTU, 0);
        a64_ldr(b, 8, JT0, JTF, (uint32_t)offsetof(JitBlock, body_code));
        uint32_t *nobody = a64_label(b); a64_cbz(b, 1, JT0, 0);
        a64_ldr(b, 4, JTU, 20, INT_OFF);                  /* interrupt poll (back edges) */
        uint32_t *intr = a64_label(b); a64_cbnz(b, 0, JTU, 0);
        if (!xmm_global_enabled()) emit_xmm_pin_spill_all(b);
        a64_br(b, JT0);
        a64_patch_cbz(nobody, a64_label(b));
        a64_patch_cbz(intr, a64_label(b));
        /* interrupt or no body: fall to the epilogue (RIP already stored) */
        uint32_t *to_epi = a64_label(b); a64_b(b, 0);
        a64_patch_cbz(to_full, a64_label(b));
        /* different layout: full leave into the function entry */
        a64_ldr(b, 8, JT0, JTF, (uint32_t)offsetof(JitBlock, code));
        uint32_t *nocode = a64_label(b); a64_cbz(b, 1, JT0, 0);
        emit_indirect_leave_br(b, JT0);
        a64_patch_cbz(nocode, a64_label(b));
        a64_patch_b(to_epi, a64_label(b));
    } else {
        a64_ldr(b, 8, JT0, JTF, (uint32_t)offsetof(JitBlock, code));
        uint32_t *nocode = a64_label(b); a64_cbz(b, 1, JT0, 0);
        emit_indirect_leave_br(b, JT0);
        a64_patch_cbz(nocode, a64_label(b));
    }
    /* not found / no code: epilogue -> dispatcher compiles it */
    a64_patch_cbz(to_nofind, a64_label(b));
    (void)slot;
    a64_mov_imm64(b, 0, OCERZ_STEP_OK);
    epi_sites[*n_epi] = a64_label(b);
    a64_b(b, 0);
    (*n_epi)++;
}

/* Load an indirect branch target operand (reg or mem) into JT1. */
static int emit_branch_target(A64Buf *b, const X86Insn *insn, const X86Operand *o,
                              uint32_t **exit_sites, int *n_exits)
{
    if (o->kind == OCERZ_OPK_REG) {
        if (o->high8 || o->size != 8)
            return 0;
        if (g_pin_class == 2 && o->reg == OCERZ_RSP)
            return 0;
        emit_gpr_rd(b, 1, JT1, o->reg);
        return 1;
    }
    if (o->kind == OCERZ_OPK_MEM) {
        if (o->size != 8)
            return 0;
        if (!emit_mem_ea(b, insn, o, JTA))
            return 0;
        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
        emit_guest_load_ordered(b, 8, JT1, JTA, JTU);
        patch_guard_skip(skip, a64_label(b));
        return 1;
    }
    return 0;
}

static int emit_indirect_jmp(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites,
                             int *n_exits, uint32_t **epi_sites, int *n_epi)
{
    if (insn->op != OCERZ_OP_JMP || insn->ops[0].kind == OCERZ_OPK_IMM)
        return 0;
    if (getenv("OCERZ_NO_INLINE_INDIRECT"))
        return 0;
    if (insn->seg != OCERZ_SEG_NONE)
        return 0;
    if (getenv("OCERZ_EXP_MAT_IND")) emit_materialize(b);
    if (!emit_branch_target(b, insn, &insn->ops[0], exit_sites, n_exits))
        return 0;
    emit_indirect_tail(b, ic_slot_alloc(), epi_sites, n_epi);
    return 1;
}

static int emit_indirect_call(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites,
                              int *n_exits, uint32_t **epi_sites, int *n_epi)
{
    if (insn->op != OCERZ_OP_CALL || insn->ops[0].kind == OCERZ_OPK_IMM)
        return 0;
    if (getenv("OCERZ_NO_INLINE_INDIRECT"))
        return 0;
    if (insn->seg != OCERZ_SEG_NONE || !mem_native_store_ok())
        return 0;
    if (getenv("OCERZ_EXP_MAT_IND")) emit_materialize(b);
    if (!emit_branch_target(b, insn, &insn->ops[0], exit_sites, n_exits))
        return 0;
    /* push return address (target already in JT1) */
    uint64_t retaddr = insn->rip + insn->len;
    a64_mov_imm64(b, JT2, retaddr);
    emit_gpr_rd(b, 1, JT0, OCERZ_RSP);
    a64_sub_imm(b, 1, JTA, JT0, 8);
    emit_add_const(b, JTA, ea_fold());
    uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
    emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
    emit_guest_store_ordered(b, 8, JT2, JTA, JTU);
    a64_sub_imm(b, 1, JT0, JT0, 8);
    emit_gpr_wr(b, JT0, OCERZ_RSP);
    patch_guard_skip(skip, a64_label(b));
    /* RAS push so the matching ret predicts */
    if (!g_no_ras) {
        void **rslot = ras_slot_alloc();
        if (rslot) {
            JitBlock *rb = cache_lookup(g_xlat_jit, retaddr);
            if (rb && rb->code)
                *rslot = ras_entry_for(rb);
            else
                pending_add_ras(retaddr, rslot);
            a64_ldr(b, 4, JT2, 20, RAS_TOP_OFF);
            a64_subs_imm(b, 0, A64_ZR, JT2, OCERZ_RAS_SIZE);
            uint32_t *full = a64_label(b);
            a64_bcond(b, A64_CS, 0);
            a64_mov_imm64(b, JTF, retaddr);
            a64_mov_imm64(b, JTA, (uint64_t)(uintptr_t)rslot);
            a64_ldr(b, 8, JT0, JTA, 0);
            a64_lsl_imm(b, 1, JTA, JT2, 4);
            a64_add_reg(b, 1, JTA, JTA, 20, 0);
            a64_str(b, 8, JTF, JTA, RAS_OFF);
            a64_str(b, 8, JT0, JTA, RAS_OFF + 8);
            a64_add_imm(b, 0, JT2, JT2, 1);
            a64_str(b, 4, JT2, 20, RAS_TOP_OFF);
            a64_patch_bcond(full, a64_label(b));
        }
    }
    emit_indirect_tail(b, ic_slot_alloc(), epi_sites, n_epi);
    return 1;
}

static void emit_slowcall(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{

    emit_materialize(b);

    emit_xmm_pin_spill_all(b);           /* interpreter reads/writes cpu->xmm */
    emit_spill_pinned(b);
    a64_mov_reg(b, 1, 0, 19);
    a64_mov_reg(b, 1, 1, 20);
    a64_mov_imm64(b, 2, (uint64_t)(uintptr_t)insn);
    a64_mov_imm64(b, 16, (uint64_t)(uintptr_t)&ocerz_jit_exec_one);
    a64_blr(b, 16);
    emit_fill_pinned(b);
    emit_xmm_pin_load_all(b);            /* BEFORE the exit test: exit_label spills V regs,
                                            so they must equal memory on that path too */
    exit_sites[*n_exits] = a64_label(b);
    a64_cbnz(b, 0, 0, 0);
    (*n_exits)++;
    emit_reload_mem_base(b);
}

int ocerz_jitstat = -1;
static _Atomic unsigned long long js_steps, js_hits, js_misses;
static _Atomic unsigned long long js_xlat, js_xlat_ok, js_xlat_fail;
static _Atomic unsigned long long js_fail_decode0, js_fail_overflow, js_fail_alloc;
static _Atomic unsigned long long js_decoded_insns;
static uint64_t js_t0;

#define JS_FTAB (1u << 20)
typedef struct { uint64_t rip; unsigned long long n; unsigned char bytes[8];
                 unsigned reason; int nins; } JsFail;
static JsFail js_ftab[JS_FTAB];
static unsigned js_ftab_used, js_ftab_full;

enum { JSR_DECODE0 = 1, JSR_OVERFLOW = 2, JSR_ALLOC = 3 };

static void js_note_fail(uint64_t rip, unsigned reason, int nins)
{

    uint64_t x = rip;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 29;
    unsigned h = (unsigned)(x & (JS_FTAB - 1));
    for (unsigned i = 0; i < 8; i++) {
        unsigned k = (h + i) & (JS_FTAB - 1);
        if (js_ftab[k].n && js_ftab[k].rip == rip) { js_ftab[k].n++; return; }
        if (!js_ftab[k].n) {
            js_ftab[k].rip = rip; js_ftab[k].n = 1;
            js_ftab[k].reason = reason; js_ftab[k].nins = nins;
            const uint8_t *c = (const uint8_t *)ocerz_g2h(rip);
            sigjmp_buf bb, *prev = ocerz_jit_decode_recover;
            if (sigsetjmp(bb, 1) == 0) {
                ocerz_jit_decode_recover = &bb;
                memcpy(js_ftab[k].bytes, c, 8);
            }
            ocerz_jit_decode_recover = prev;
            js_ftab_used++;
            return;
        }
    }
    js_ftab_full++;
}

static int js_cmp(const void *a, const void *b)
{
    unsigned long long x = ((const JsFail *)a)->n;
    unsigned long long y = ((const JsFail *)b)->n;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void js_report(OcerzJit *jit, const char *tag, int with_ftab)
{
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    double sec = (double)(now - js_t0) / 1e9;
    unsigned long long st = js_steps, hi = js_hits, mi = js_misses;
    unsigned long long xf = js_xlat_fail, xo = js_xlat_ok;
    size_t used = (size_t)((uint8_t *)jit->code_cur - (uint8_t *)jit->code_base);
    fprintf(stderr,
        "ocerz: JITSTAT[%d] %s t=%.1fs steps=%llu hits=%llu (%.4f%%) misses=%llu (%.0f lock/s)\n"
        "ocerz: JITSTAT[%d]   translate: calls=%llu ok=%llu fail=%llu (decode0=%llu overflow=%llu alloc=%llu)\n"
        "ocerz: JITSTAT[%d]   decoded_insns_on_miss=%llu  code_used=%zu/%zu bytes (%.1f%%) EXHAUSTED=%d  failtab_used=%u full=%u\n",
        (int)getpid(), tag, sec, st, hi, st ? 100.0 * (double)hi / (double)st : 0.0,
        mi, sec > 0 ? (double)mi / sec : 0.0,
        (int)getpid(), (unsigned long long)js_xlat, xo, xf,
        (unsigned long long)js_fail_decode0, (unsigned long long)js_fail_overflow,
        (unsigned long long)js_fail_alloc,
        (int)getpid(), (unsigned long long)js_decoded_insns, used, jit->code_bytes,
        100.0 * (double)used / (double)jit->code_bytes,
        (unsigned)(jit->code_end - jit->code_cur) < 4096u, js_ftab_used, js_ftab_full);

    if (!with_ftab)
        return;

    static JsFail snap[JS_FTAB];
    memcpy(snap, js_ftab, sizeof snap);
    qsort(snap, JS_FTAB, sizeof snap[0], js_cmp);
    for (int i = 0; i < 25 && snap[i].n; i++)
        fprintf(stderr, "ocerz: JITSTAT[%d]   FAILRIP #%2d %#18llx retries=%-10llu reason=%s nins=%d bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
            (int)getpid(), i, (unsigned long long)snap[i].rip, snap[i].n,
            snap[i].reason == JSR_DECODE0 ? "decode-fail" :
            snap[i].reason == JSR_OVERFLOW ? "code-overflow" : "alloc-fail",
            snap[i].nins,
            snap[i].bytes[0], snap[i].bytes[1], snap[i].bytes[2], snap[i].bytes[3],
            snap[i].bytes[4], snap[i].bytes[5], snap[i].bytes[6], snap[i].bytes[7]);
}

#define CHAIN_BATCH_MAX 64
static uint32_t *g_chain_patched[CHAIN_BATCH_MAX];
static int g_chain_npatched;
static int g_chain_batching;

static void chain_batch_begin(void)
{
    g_chain_npatched = 0;
    g_chain_batching = 1;
    pthread_jit_write_protect_np(0);
}

static void chain_batch_end(void)
{
    pthread_jit_write_protect_np(1);
    for (int i = 0; i < g_chain_npatched; i++)
        sys_icache_invalidate(g_chain_patched[i], 4);
    g_chain_npatched = 0;
    g_chain_batching = 0;
}

static void chain_activate(uint32_t *patch_b, void *dst)
{
    if (!patch_b || !dst)
        return;
    int ok;
    if (g_chain_batching) {
        ok = a64_try_patch_b(patch_b, (uint32_t *)dst);
        if (g_chain_npatched < CHAIN_BATCH_MAX)
            g_chain_patched[g_chain_npatched++] = patch_b;
        else
            sys_icache_invalidate(patch_b, 4);
    } else {
        pthread_jit_write_protect_np(0);
        ok = a64_try_patch_b(patch_b, (uint32_t *)dst);
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(patch_b, 4);
    }
    if (ocerz_perfstat > 0) {
        if (ok)
            ps_chain_ok++;
        else
            ps_chain_far++;
    }
}

typedef struct PendingChain {
    uint64_t target_rip;
    uint32_t *patch_b;
    void **ras_slot;
    uint8_t kind;
    uint8_t pin_class;
    struct PendingChain *next;
} PendingChain;
#define PEND_BITS 12
#define PEND_SIZE (1u << PEND_BITS)
#define PEND_MASK (PEND_SIZE - 1)
static PendingChain *g_pending[PEND_SIZE];

static void pending_add(uint64_t target_rip, uint32_t *patch_b, uint8_t kind,
                        uint8_t pin_class)
{
    PendingChain *e = (PendingChain *)malloc(sizeof *e);
    if (!e)
        return;
    unsigned h = (unsigned)(hash_rip(target_rip) & PEND_MASK);
    e->target_rip = target_rip;
    e->patch_b = patch_b;
    e->ras_slot = NULL;
    e->kind = kind;
    e->pin_class = pin_class;
    e->next = g_pending[h];
    g_pending[h] = e;
}

#define RAS_SLOT_CAP (1u << 18)
static void **g_ras_slots;
static unsigned g_ras_slot_n;

static void **ras_slot_alloc(void)
{
    if (!g_ras_slots) {
        void *p = mmap(NULL, (size_t)RAS_SLOT_CAP * sizeof(void *),
                       PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED)
            return NULL;
        g_ras_slots = (void **)p;
    }
    if (g_ras_slot_n >= RAS_SLOT_CAP)
        return NULL;
    void **s = &g_ras_slots[g_ras_slot_n++];
    *s = NULL;
    return s;
}

static void pending_add_ras(uint64_t target_rip, void **ras_slot)
{
    PendingChain *e = (PendingChain *)malloc(sizeof *e);
    if (!e)
        return;
    unsigned h = (unsigned)(hash_rip(target_rip) & PEND_MASK);
    e->target_rip = target_rip;
    e->patch_b = NULL;
    e->ras_slot = ras_slot;
    e->kind = EDGE_XBLOCK;
    e->pin_class = 0;
    e->next = g_pending[h];
    g_pending[h] = e;
}

static void pending_drain(uint64_t rip, JitBlock *target)
{
    unsigned h = (unsigned)(hash_rip(rip) & PEND_MASK);
    PendingChain **pp = &g_pending[h];
    while (*pp) {
        PendingChain *e = *pp;
        if (e->target_rip == rip) {
            if (e->ras_slot)

                __atomic_store_n(e->ras_slot, ras_entry_for(target), __ATOMIC_RELEASE);
            else if (e->kind == EDGE_BODY) {
                int compatible = e->pin_class
                    ? target->pin_class == e->pin_class
                    : (target->pin_class == 0 && target->n_pinned == 0);
                if (compatible && target->body_code)
                    chain_activate(e->patch_b, target->body_code);
            } else {
                chain_activate(e->patch_b, (void *)target->code);
            }
            *pp = e->next;
            free(e);
        } else {
            pp = &e->next;
        }
    }
}

static uint32_t *emit_body_chain_tail(A64Buf *b, uint64_t target_rip, int poll,
                                      uint32_t **epilogue_sites, int *n_epi)
{
    int patch_stop = poll && !g_stop_patch;
    uint32_t *intr = NULL;
    if (!xmm_global_enabled())
        emit_xmm_pin_spill_all(b);       /* per-block layouts: xmm state to memory */
    if (poll && !patch_stop) {
        a64_ldr(b, 4, JT1, 20, INT_OFF);
        intr = a64_label(b);
        a64_cbnz(b, 0, JT1, 0);
    }
    uint32_t *patch_b = a64_label(b);
    a64_b(b, 0);
    uint32_t *fallback = a64_label(b);
    a64_patch_b(patch_b, fallback);
    if (intr)
        a64_patch_cbz(intr, fallback);
    if (patch_stop) {
        g_stop_patch = patch_b;
        g_stop_target = fallback;
    }

    if (g_pin_class == 2)
        a64_add_imm(b, 1, 31, 29, 0);

    a64_mov_imm64(b, JT0, target_rip);
    a64_str(b, 8, JT0, 20, RIP_OFF);
    a64_mov_imm64(b, 0, OCERZ_STEP_OK);
    epilogue_sites[*n_epi] = a64_label(b);
    a64_b(b, 0);
    (*n_epi)++;
    return patch_b;
}

static uint32_t *emit_chain_tail(A64Buf *b, int poll)
{
    uint32_t *intr = NULL;

    if (poll)
        a64_ldr(b, 4, JT1, 20, INT_OFF);
    emit_xmm_pin_spill_all(b);
    emit_spill_pinned(b);
    a64_mov_reg(b, 1, 0, 19);
    a64_mov_reg(b, 1, 1, 20);
    emit_pin_epilogue_restore(b);
    a64_ldp_post(b, 19, 20, 31, 16);
    a64_ldp_post(b, 29, 30, 31, 16);
    if (poll) {
        intr = a64_label(b);
        a64_cbnz(b, 0, JT1, 0);
    }
    uint32_t *patch_b = a64_label(b);
    a64_b(b, 0);
    uint32_t *fallback = a64_label(b);
    a64_patch_b(patch_b, fallback);
    if (poll)
        a64_patch_cbz(intr, fallback);
    a64_mov_imm64(b, 0, OCERZ_STEP_OK);
    a64_ret(b);
    return patch_b;
}

static uint32_t *emit_static_chain_tail(A64Buf *b, uint64_t target_rip,
                                        int poll, int body_edge,
                                        uint32_t **epilogue_sites, int *n_epi)
{
    if (body_edge)
        return emit_body_chain_tail(b, target_rip, poll,
                                    epilogue_sites, n_epi);
    a64_mov_imm64(b, JT0, target_rip);
    a64_str(b, 8, JT0, 20, RIP_OFF);
    return emit_chain_tail(b, poll);
}

static int select_mem_base_hoist(const X86Insn *insns, int n, uint64_t rip)
{
    g_mem_hoist_aux_disp = 0;
    if (!g_plain_mem || g_no_chain || ocerz_commpage || ocerz_low_base ||
        !mem_native_store_ok() || n < 2)
        return -1;
    const X86Insn *term = &insns[n - 1];
    if (!((term->op == OCERZ_OP_JCC && term->ops[0].kind == OCERZ_OPK_IMM &&
           term->ops[0].imm == rip) ||
          (term->op == OCERZ_OP_JMP && term->ops[0].kind == OCERZ_OPK_IMM &&
           term->ops[0].imm == rip)))
        return -1;

    int base = -1;
    int accesses = 0;
    for (int i = 0; i < n; i++) {
        const X86Insn *in = &insns[i];
        if (in->op != OCERZ_OP_MOV || in->nops != 2)
            continue;
        const X86Operand *mem = NULL;
        const X86Operand *reg = NULL;
        if (in->ops[0].kind == OCERZ_OPK_MEM &&
            in->ops[1].kind == OCERZ_OPK_REG) {
            mem = &in->ops[0];
            reg = &in->ops[1];
        } else if (in->ops[0].kind == OCERZ_OPK_REG &&
                   in->ops[1].kind == OCERZ_OPK_MEM) {
            reg = &in->ops[0];
            mem = &in->ops[1];
        }
        if (!mem || reg->high8 || (reg->size != 4 && reg->size != 8) ||
            in->seg != OCERZ_SEG_NONE || in->addrsize != 8 || mem->riprel ||
            mem->base == OCERZ_REG_NONE)
            continue;
        if (base < 0)
            base = mem->base;
        else if (base != mem->base)
            return -1;

        if (g_pin_class != 2 && mem->index != OCERZ_REG_NONE &&
            mem->disp != 0 && mem->disp >= -4095 && mem->disp <= 4095 &&
            g_mem_hoist_aux_disp == 0)
            g_mem_hoist_aux_disp = (int)mem->disp;
        accesses++;
    }
    if (accesses < 2 || base < 0 || pin_slot(base) < 0)
        return -1;

    for (int i = 0; i < n; i++) {
        const X86Insn *in = &insns[i];
        if (in->nops == 0 || in->ops[0].kind != OCERZ_OPK_REG ||
            in->ops[0].reg != base)
            continue;
        switch (in->op) {
        case OCERZ_OP_MOV: case OCERZ_OP_MOVZX: case OCERZ_OP_MOVSX:
        case OCERZ_OP_MOVSXD: case OCERZ_OP_LEA:
        case OCERZ_OP_ADD: case OCERZ_OP_SUB: case OCERZ_OP_AND:
        case OCERZ_OP_OR: case OCERZ_OP_XOR:
        case OCERZ_OP_INC: case OCERZ_OP_DEC:
        case OCERZ_OP_SHL: case OCERZ_OP_SHR: case OCERZ_OP_SAR:
        case OCERZ_OP_IMUL: case OCERZ_OP_POP:
            return -1;
        default:
            break;
        }
    }
    return base;
}

static int fault_recipe_native_mov(const X86Insn *insn)
{
    if (!g_defer || ocerz_commpage || ocerz_low_base ||
        insn->op != OCERZ_OP_MOV || insn->nops != 2 ||
        insn->seg != OCERZ_SEG_NONE || insn->addrsize == 4)
        return 0;
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    if (d->kind == OCERZ_OPK_MEM && s->kind == OCERZ_OPK_REG)
        return !s->high8 && (s->size == 4 || s->size == 8) &&
               mem_native_store_ok();
    if (d->kind == OCERZ_OPK_REG && s->kind == OCERZ_OPK_MEM)
        return !d->high8 && (d->size == 4 || d->size == 8);
    return 0;
}

static int fault_recipe_add_shape(const X86Insn *insn)
{
    if (insn->op != OCERZ_OP_ADD || insn->lock || insn->nops != 2)
        return 0;
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 ||
        (d->size != 4 && d->size != 8) || s->size != d->size)
        return 0;
    if (s->kind == OCERZ_OPK_IMM)
        return 1;
    return s->kind == OCERZ_OPK_REG && !s->high8 && s->reg != d->reg;
}

static int fault_recipe_logic_shape(const X86Insn *insn)
{
    if ((insn->op != OCERZ_OP_AND && insn->op != OCERZ_OP_OR &&
         insn->op != OCERZ_OP_XOR) || insn->lock || insn->nops != 2)
        return 0;
    const X86Operand *d = &insn->ops[0];
    return d->kind == OCERZ_OPK_REG && !d->high8 &&
           (d->size == 4 || d->size == 8);
}

static int fault_recipe_matching_inc(const X86Insn *add, const X86Insn *inc)
{
    const X86Operand *d = &add->ops[0];
    return inc->op == OCERZ_OP_INC && !inc->lock && inc->nops == 1 &&
           inc->ops[0].kind == OCERZ_OPK_REG && !inc->ops[0].high8 &&
           inc->ops[0].reg == d->reg && inc->ops[0].size == d->size;
}

static int build_fault_flag_recipes(const X86Insn *insns, int n,
                                    JitFaultFlagRecipe *recipes)
{
    memset(recipes, 0, (size_t)n * sizeof *recipes);
    if (g_no_fault_recipes)
        return 0;
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (!fault_recipe_native_mov(&insns[i]))
            continue;
        JitFaultFlagRecipe r = { JFF_NONE, 0 };
        if (i >= 2 && fault_recipe_add_shape(&insns[i - 2]) &&
            fault_recipe_matching_inc(&insns[i - 2], &insns[i - 1])) {
            r.kind = JFF_ADD_INC_RESULT_SRC;
            r.producer = (uint8_t)(i - 2);
        } else if (i >= 1 && fault_recipe_logic_shape(&insns[i - 1])) {
            r.kind = JFF_LOGIC_RESULT;
            r.producer = (uint8_t)(i - 1);
        } else if (i >= 1 && fault_recipe_add_shape(&insns[i - 1])) {
            r.kind = JFF_ADD_RESULT_SRC;
            r.producer = (uint8_t)(i - 1);
        }
        if (r.kind != JFF_NONE) {
            recipes[i] = r;
            found++;
        }
    }
    return found;
}

static int code_index_append_locked(OcerzJit *jit, JitBlock *block)
{
    JitCodeIndex *index = __atomic_load_n(&jit->ci, __ATOMIC_RELAXED);
    size_t count = index
        ? __atomic_load_n(&index->count, __ATOMIC_RELAXED) : 0;

    if (!index || count == index->capacity) {
        size_t capacity = index ? index->capacity * 2 : 4096;
        if ((index && capacity < index->capacity) ||
            capacity > (SIZE_MAX - sizeof(JitCodeIndex)) /
                       sizeof(index->blocks[0]))
            return 0;

        JitCodeIndex *next = (JitCodeIndex *)malloc(
            sizeof(*next) + capacity * sizeof(next->blocks[0]));
        if (!next)
            return 0;
        next->older = index;
        next->capacity = capacity;
        next->count = 0;
        if (count)
            memcpy(next->blocks, index->blocks,
                   count * sizeof(next->blocks[0]));
        next->blocks[count] = block;
        __atomic_store_n(&next->count, count + 1, __ATOMIC_RELEASE);
        __atomic_store_n(&jit->ci, next, __ATOMIC_RELEASE);
        return 1;
    }

    index->blocks[count] = block;
    __atomic_store_n(&index->count, count + 1, __ATOMIC_RELEASE);
    return 1;
}

static void emit_ordered_slow_arms(A64Buf *b, JitBlock *blk, const uint32_t *entry)
{
    if (g_n_oslow <= 0)
        return;
    blk->oslow = (struct JitOslowMap *)malloc((size_t)g_n_oslow * sizeof *blk->oslow);
    blk->n_oslow = 0;
    for (int i = 0; i < g_n_oslow; i++) {
        const OrderedSlowPend *o = &g_oslow[i];
        uint32_t *lo = a64_label(b);
        a64_patch_bcond(o->bne, lo);
        if (o->store) {
            a64_dmb_ish(b);
            a64_str(b, o->size, o->rv, o->ra, 0);
        } else {
            a64_ldr(b, o->size, o->rv, o->ra, 0);
            a64_dmb_ish(b);
        }
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

static JitBlock *translate(OcerzJit *jit, uint64_t rip)
{

    static int g_jitmeasure = -1;
    if (g_jitmeasure < 0)
        g_jitmeasure = getenv("OCERZ_JITMEASURE") ? 1 : 0;
    uint64_t xlat_t0 = g_jitmeasure ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
    X86Insn scratch[JIT_MAX_BLOCK_INSNS];
    int n = 0;
    uint64_t pc = rip;
    {
        volatile int vn = 0;
        volatile uint64_t vpc = rip;
        sigjmp_buf db;
        sigjmp_buf *prev_dr = ocerz_jit_decode_recover;
        if (sigsetjmp(db, 1) == 0) {
            ocerz_jit_decode_recover = &db;
            for (; vn < JIT_MAX_BLOCK_INSNS; ) {
                const uint8_t *code = (const uint8_t *)ocerz_g2h(vpc);
                int rc = ocerz_decode(code, 15, vpc, &scratch[vn]);
                if (rc != OCERZ_OK)
                    break;
                unsigned op = scratch[vn].op;
                uint8_t len = scratch[vn].len;
                vn++;
                if (is_terminator(op))
                    break;
                vpc += len;
            }
        }
        ocerz_jit_decode_recover = prev_dr;
        n = vn;
        pc = vpc;
    }
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
    blk->guest_rip = rip;

    for (int g = 0; g < 16; g++)
        blk->guest_in_host[g] = -1;
    blk->n_pinned = 0;
    g_pin = NULL;
    g_pin_hold = NULL;
    g_n_pinned = 0;
    g_pin_class = 0;

    g_defer = !g_no_regflags;
    blk->n_edges = 0;
    g_chain_target = 0;
    g_chain_epi = NULL;
    g_n_jcc_edges = 0;
    g_n_oslow = 0;
    g_n_nanool = 0;
    g_n_call_edges = 0;
    g_xlat_jit = jit;
    g_self_rip = rip;
    g_body_entry = NULL;
    g_loop_entry = NULL;
    g_stop_patch = NULL;
    g_stop_target = NULL;
    g_mem_hoist_greg = -1;
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
    int call_region = !g_no_regflags && !ocerz_low_base &&
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

    /* XMM pin mask: every xmm register any instruction of the block touches
     * (blendv also reads xmm0).  Slow ops are fine: callouts spill/reload. */
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
    if (!jit->dispatch_stub && !getenv("OCERZ_NO_DISPATCH_STUB"))
        emit_dispatch_stub(jit);
    A64Buf b = { jit->code_cur, jit->code_cur, jit->code_end, 0, 0 };
    uint32_t *entry = b.p;

    a64_stp_pre(&b, 29, 30, 31, -16);
    a64_stp_pre(&b, 19, 20, 31, -16);
    a64_mov_reg(&b, 1, 19, 0);
    a64_mov_reg(&b, 1, 20, 1);

    emit_pin_prologue(&b);

    if (g_pin_class == 2) {
        int rs = pin_slot(OCERZ_RSP);
        assert(rs >= 0);
        a64_mov_imm64(&b, JT0, ocerz_guest_base);
        a64_add_reg(&b, 1, pin_hreg(rs), pin_hreg(rs), JT0, 0);
    }

    if (g_pin_class == 2)
        a64_add_imm(&b, 1, 29, 31, 0);

    uint32_t *loop_poll_exit = NULL;
    if (xmm_global_enabled())
        emit_xmm_pin_load_all(&b);       /* function entry only: body edges keep V16-V31 live */
    if (!g_no_chain && !jit->stop_requested) {
        g_body_entry = a64_label(&b);
        emit_reload_mem_base(&b);
        if (!xmm_global_enabled())
            emit_xmm_pin_load_all(&b);
        g_loop_entry = a64_label(&b);
        /* Interrupt poll at the loop head.  Self-loops branch straight here
         * with no C callout, and a core spinning in a tiny fully-inlined loop
         * does not reliably observe the stop-site patch (no pipeline flush),
         * so a 2-instruction poll is the only robust way to break out. */
        a64_ldr(&b, 4, JT0, 20, INT_OFF);
        loop_poll_exit = a64_label(&b);
        a64_cbnz(&b, 0, JT0, 0);
    } else {
        if (!xmm_global_enabled())
            emit_xmm_pin_load_all(&b);
    }
    if (ocerz_perfstat > 0) {   /* after body entry: counts every entry (PERFSTAT only) */
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
        default:

            break;
        }
    }

    uint64_t fl_need[JIT_MAX_BLOCK_INSNS];
    uint64_t entry_all;
    {
        uint64_t live_seam = seam_seed;
        uint64_t live_all = OCERZ_FL_ALL;
        for (int i = n - 1; i >= 0; i--) {
            uint64_t def, use;
            if (blk->fault_flags && blk->fault_flags[i].kind != JFF_NONE)
                ocerz_flags_defuse_nofault(&blk->insns[i], &def, &use);
            else
                ocerz_flags_defuse(&blk->insns[i], &def, &use);
            fl_need[i] = def & live_seam;
            live_seam = (live_seam & ~def) | use;
            live_all = (live_all & ~def) | use;

            if (g_no_lazyflags)
                fl_need[i] = def;
        }
        entry_all = live_all;
    }

    blk->entry_live = (uint16_t)entry_all;

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

    uint32_t *exit_sites[JIT_MAX_BLOCK_INSNS];
    uint32_t *epi_sites[JIT_MAX_BLOCK_INSNS];
    int n_exits = 0;
    int n_epi = 0;

    int last_flag_def = -1;
    for (int i = 0; i < n; i++) {
        const X86Insn *insn = &blk->insns[i];
        g_cur_insn_idx = i;
        g_flag_producer = last_flag_def >= 0 ? &blk->insns[last_flag_def] : NULL;
        {
            uint64_t pdef, puse;
            ocerz_flags_defuse(insn, &pdef, &puse);
            if (pdef & JIT_ARITH_FLAGS)
                last_flag_def = i;
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
        if (i + 1 < n) {
            uint32_t *logic_label = NULL;
            if (emit_mov_logic_pair(&b, insn, &blk->insns[i + 1],
                                    fl_need[i + 1], &logic_label)) {
                if (blk->insn_off)
                    blk->insn_off[i + 1] =
                        (uint32_t)(logic_label - entry);
                blk->n_inlined += 2;
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
            /* gap fusion: cmp/test ; lea|mov ; jcc  (NZCV forwarded over the gap) */
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
            if (emit_jmp(&b, insn, epi_sites, &n_epi) ||
                emit_indirect_jmp(&b, insn, exit_sites, &n_exits, epi_sites, &n_epi)) {
                blk->n_inlined++;
                continue;
            }
        }

        if (i == n - 1 && (insn->op == OCERZ_OP_CALL || insn->op == OCERZ_OP_RET)) {
            if (emit_call_ret(&b, insn, exit_sites, &n_exits, epi_sites, &n_epi) ||
                emit_indirect_call(&b, insn, exit_sites, &n_exits, epi_sites, &n_epi)) {
                blk->n_inlined++;
                continue;
            }
        }
        if (!try_inline(&b, insn, fl_need[i], exit_sites, &n_exits)) {
            emit_slowcall(&b, insn, exit_sites, &n_exits);
            blk->n_slow++;
        } else {
            blk->n_inlined++;
        }
    }

    if (!is_terminator(blk->insns[n - 1].op)) {

        emit_materialize(&b);
        a64_mov_imm64(&b, JT0, pc);
        a64_str(&b, 8, JT0, 20, RIP_OFF);
        a64_mov_imm64(&b, 0, OCERZ_STEP_OK);
    }

    uint32_t *exit_label = a64_label(&b);
    emit_xmm_pin_spill_all(&b);
    emit_spill_pinned(&b);
    emit_pin_epilogue_restore(&b);
    if (jit->dispatch_stub) {
        /* x0 = step code.  STEP_OK -> continue in the arena via the dispatch
         * stub (needs x0=vm, x1=cpu, no frame); anything else -> ret to C. */
        a64_mov_reg(&b, 1, 1, 20);
        a64_mov_reg(&b, 1, JTT, 0);
        a64_mov_reg(&b, 1, 0, 19);
        a64_ldp_post(&b, 19, 20, 31, 16);
        a64_ldp_post(&b, 29, 30, 31, 16);
        uint32_t *nonzero = a64_label(&b); a64_cbnz(&b, 1, JTT, 0);
        uint32_t *here = a64_label(&b);
        a64_b(&b, (int32_t)(jit->dispatch_stub - here));
        a64_patch_cbz(nonzero, a64_label(&b));
        a64_mov_reg(&b, 1, 0, JTT);
        a64_ret(&b);
    } else {
        a64_ldp_post(&b, 19, 20, 31, 16);
        a64_ldp_post(&b, 29, 30, 31, 16);
        a64_ret(&b);
    }
    emit_ordered_slow_arms(&b, blk, entry);
    emit_nan_ool_arms(&b, blk, entry);
    if (loop_poll_exit) {
        /* interrupt seen at the loop head: leave with RIP = this block (OOL) */
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
            /* every block shares the full layout: chain into the callee BODY
             * (no leave/enter); fallback path stores RIP and exits normally */
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
            /* The stop replacement must be an UNCONDITIONAL b to stop_target
             * whatever the site currently holds (b or b.cond): patch_b keeps
             * the site's opcode bits, which mangles a b.cond into a
             * branch-to-self. */
            int32_t off = (int32_t)(g_stop_target - g_stop_patch);
            blk->stop_patch = g_stop_patch;
            blk->stop_insn = 0x14000000u | ((uint32_t)off & 0x03ffffffu);
            if (jit->stop_requested)
                *g_stop_patch = blk->stop_insn;
            else
                *g_stop_patch = running_insn;
        }
    }

    pthread_jit_write_protect_np(1);

    if (b.overflow) {

        if (!jit->code_full) {
            jit->code_full = 1;
            OCERZ_LOG("JIT code arena full (%zu MB, %llu blocks); further blocks run interpreted\n",
                      jit->code_bytes >> 20,
                      (unsigned long long)jit->blocks_translated);
        }
        if (ocerz_jitstat > 0) { js_fail_overflow++; js_note_fail(rip, JSR_OVERFLOW, n); }

        blk->n_slow = n;
        blk->n_inlined = 0;
        blk->n_pinned = 0;
        blk->pin_class = 0;
        blk->code = NULL;
        g_pin = NULL; g_pin_hold = NULL; g_n_pinned = 0; g_pin_class = 0;
        cache_insert(jit, blk);
        return blk;
    }

    sys_icache_invalidate(entry, (size_t)((b.p - entry) * 4));
    jit->code_cur = b.p;
    blk->code = (JitBlockFn)entry;
    blk->body_code = g_body_entry;
    blk->code_words = (uint32_t)(b.p - entry);
    if (blk->stop_patch) {
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
            blk->edges[i].kind = g_call_edge[i].kind;
            blk->edges[i].pin_class = g_call_edge[i].pin_class;
        }
        blk->n_edges = (uint8_t)g_n_call_edges;
    } else if (chain_patch_b) {
        blk->edges[0].target_rip = g_chain_target;
        blk->edges[0].patch_b = chain_patch_b;
        blk->edges[0].kind = chain_is_body ? EDGE_BODY : EDGE_XBLOCK;
        blk->edges[0].pin_class = chain_is_body ? 3 : 0;
        blk->n_edges = 1;
    } else if (g_n_jcc_edges) {
        for (int i = 0; i < g_n_jcc_edges; i++) {
            blk->edges[i].target_rip = g_jcc_edge[i].target_rip;
            blk->edges[i].patch_b = g_jcc_edge[i].patch_b;
            blk->edges[i].kind = g_jcc_edge[i].kind;
            blk->edges[i].pin_class = g_jcc_edge[i].pin_class;
        }
        blk->n_edges = (uint8_t)g_n_jcc_edges;
    }
    for (int i = 0; i < blk->n_edges; i++)
        blk->edges[i].fallback_insn = *blk->edges[i].patch_b;

    {
        static int g_jitdis = -1;
        static FILE *g_jf;
        if (g_jitdis < 0) {
            const char *p = getenv("OCERZ_JITDIS");
            g_jitdis = p ? 1 : 0;
            if (p)
                g_jf = fopen(p, "w");
        }
        if (g_jitdis > 0 && g_jf && blk->insn_off) {
            char tb[128];
            uint32_t epi = (uint32_t)(exit_label - entry);
            fprintf(g_jf, "BLOCK rip=%#llx host=%p words=%u n_insns=%d inlined=%d slow=%d"
                          " prologue_words=%u epilogue_words=%u pin_class=%d n_pinned=%d body=%d\n",
                    (unsigned long long)rip, (void *)entry, blk->code_words, n,
                    blk->n_inlined, blk->n_slow, blk->insn_off[0],
                    blk->code_words - epi, (int)blk->pin_class, (int)blk->n_pinned,
                    blk->body_code != NULL);
            for (int e = 0; e < blk->n_edges; e++)
                fprintf(g_jf, "  EDGE -> %#llx kind=%d pin_class=%d\n",
                        (unsigned long long)blk->edges[e].target_rip,
                        (int)blk->edges[e].kind, (int)blk->edges[e].pin_class);
            for (int i = 0; i < n; i++) {
                uint32_t s = blk->insn_off[i];
                uint32_t e = (i + 1 < n) ? blk->insn_off[i + 1] : epi;
                ocerz_format_insn(&blk->insns[i], tb, sizeof tb);
                fprintf(g_jf, "  INSN %d off=%u words=%u  %s\n", i, s,
                        e > s ? e - s : 0, tb);
                for (uint32_t w = s; w < e; w++)
                    fprintf(g_jf, "    %08x\n", entry[w]);
            }
            fflush(g_jf);
        }
    }

    if (!code_index_append_locked(jit, blk)) {
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

    cache_insert(jit, blk);

    if (!g_no_chain) {
        chain_batch_begin();
        for (int i = 0; i < blk->n_edges; i++) {
            JitBlock *t = cache_lookup(jit, blk->edges[i].target_rip);
            if (t && t->code) {
                void *dst = (void *)t->code;
                if (blk->edges[i].kind == EDGE_BODY) {
                    int compatible = blk->edges[i].pin_class
                        ? t->pin_class == blk->edges[i].pin_class
                        : (t->pin_class == 0 && t->n_pinned == 0);
                    if (!compatible || !t->body_code)
                        dst = NULL;
                    else
                        dst = (void *)t->body_code;
                }
                if (dst)
                    chain_activate(blk->edges[i].patch_b, dst);
            } else {
                pending_add(blk->edges[i].target_rip,
                            blk->edges[i].patch_b, blk->edges[i].kind,
                            blk->edges[i].pin_class);
            }
        }
        pending_drain(blk->guest_rip, blk);
        chain_batch_end();
    }

    jit->blocks_translated++;
    if (ocerz_jitstat > 0)
        js_xlat_ok++;
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

int ocerz_jit_pc_in_arena(const struct OcerzVM *vm, const void *host_pc)
{
    const OcerzJit *jit = vm ? vm->jit : NULL;
    if (!jit)
        return 0;
    const uint32_t *pc = (const uint32_t *)host_pc;
    return pc >= jit->code_base && pc < jit->code_end;
}

static const JitBlock *fault_block(const OcerzJit *jit, const uint32_t *pc)
{
    if (!jit)
        return NULL;
    if (pc < jit->code_base || pc >= jit->code_end)
        return NULL;
    const JitCodeIndex *index =
        __atomic_load_n(&jit->ci, __ATOMIC_ACQUIRE);
    if (!index)
        return NULL;
    size_t n = __atomic_load_n(&index->count, __ATOMIC_ACQUIRE);
    if (!n)
        return NULL;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if ((const uint32_t *)index->blocks[mid]->code <= pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return NULL;
    const JitBlock *b = index->blocks[lo - 1];
    const uint32_t *base = (const uint32_t *)b->code;
    if (!base || pc >= base + b->code_words)
        return NULL;
    return b;
}

void ocerz_jit_fault_recover_regs(const struct OcerzVM *vm, const void *host_pc,
                                  const uint64_t *host_x, OcerzCPU *cpu)
{
    const OcerzJit *jit = vm ? vm->jit : NULL;
    const JitBlock *b = fault_block(jit, (const uint32_t *)host_pc);
    if (!b || !host_x || !cpu)
        return;
    for (int i = 0; i < b->n_pinned; i++) {
        uint64_t value = host_x[pin_hreg(i)];
        if (b->pin_class == 2 && b->host_holds[i] == OCERZ_RSP)
            value -= ocerz_guest_base;
        cpu->gpr[b->host_holds[i]] = value;
    }
}

/* XMM pins live in host V16-V31 while a block runs; on a fault the memory
 * copy is stale, so copy them back from the signal frame's NEON state. */
void ocerz_jit_fault_recover_xmm(const struct OcerzVM *vm, const void *host_pc,
                                 const void *host_v /* __uint128_t[32] */, OcerzCPU *cpu)
{
    const OcerzJit *jit = vm ? vm->jit : NULL;
    const JitBlock *b = fault_block(jit, (const uint32_t *)host_pc);
    if (!b || !host_v || !cpu || !b->xmm_pinned)
        return;
    const unsigned char *v = (const unsigned char *)host_v;
    for (unsigned r = 0; r < 16; r++)
        if ((b->xmm_pinned >> r) & 1)
            memcpy(&cpu->xmm[r], v + (16 + r) * 16, 16);
}

static int fault_insn_index(const JitBlock *b, const uint32_t *pc)
{
    if (!b || !b->insn_off)
        return -1;
    const uint32_t *base = (const uint32_t *)b->code;
    uint32_t off = (uint32_t)(pc - base);
    for (int k = 0; k < b->n_oslow; k++)
        if (off >= b->oslow[k].lo && off < b->oslow[k].hi)
            return b->oslow[k].idx;
    int lo = 0, hi = b->n_insns;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (b->insn_off[mid] <= off)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo - 1;
}

static int fault_recipe_rhs(const OcerzCPU *cpu, const X86Operand *op,
                            int size, uint64_t *out)
{
    if (op->size != size)
        return 0;
    if (op->kind == OCERZ_OPK_REG && !op->high8) {
        *out = ocerz_trunc(cpu->gpr[op->reg], size);
        return 1;
    }
    if (op->kind == OCERZ_OPK_IMM) {
        *out = ocerz_trunc(op->imm, size);
        return 1;
    }
    return 0;
}

void ocerz_jit_fault_recover_flags(const struct OcerzVM *vm,
                                   const void *host_pc, OcerzCPU *cpu)
{
    const OcerzJit *jit = vm ? vm->jit : NULL;
    const uint32_t *pc = (const uint32_t *)host_pc;
    const JitBlock *b = fault_block(jit, pc);
    int fi = fault_insn_index(b, pc);
    if (!cpu || fi < 0 || !b->fault_flags)
        return;

    JitFaultFlagRecipe recipe = b->fault_flags[fi];
    if (recipe.kind == JFF_NONE || recipe.producer >= b->n_insns)
        return;
    const X86Insn *p = &b->insns[recipe.producer];
    if (p->nops < 1 || p->ops[0].kind != OCERZ_OPK_REG ||
        p->ops[0].high8 || (p->ops[0].size != 4 && p->ops[0].size != 8))
        return;

    int size = p->ops[0].size;
    uint64_t res = ocerz_trunc(cpu->gpr[p->ops[0].reg], size);
    uint64_t src, cc_src, cc_dst;
    uint32_t cc_op;

    switch (recipe.kind) {
    case JFF_LOGIC_RESULT:
        if (p->op != OCERZ_OP_AND && p->op != OCERZ_OP_OR &&
            p->op != OCERZ_OP_XOR)
            return;
        cc_src = res;
        cc_dst = res;
        cc_op = ocerz_cc_pack(OCERZ_CC_LOGIC, size, 0);
        break;
    case JFF_ADD_RESULT_SRC:
        if (p->op != OCERZ_OP_ADD || p->nops != 2 ||
            !fault_recipe_rhs(cpu, &p->ops[1], size, &src))
            return;
        cc_src = ocerz_trunc(res - src, size);
        cc_dst = src;
        cc_op = ocerz_cc_pack(OCERZ_CC_ADD, size, 0);
        break;
    case JFF_ADD_INC_RESULT_SRC: {
        if (p->op != OCERZ_OP_ADD || p->nops != 2 ||
            recipe.producer + 1 >= b->n_insns ||
            !fault_recipe_rhs(cpu, &p->ops[1], size, &src))
            return;
        const X86Insn *inc = &b->insns[recipe.producer + 1];
        if (inc->op != OCERZ_OP_INC || inc->nops != 1 ||
            inc->ops[0].kind != OCERZ_OPK_REG || inc->ops[0].high8 ||
            inc->ops[0].reg != p->ops[0].reg || inc->ops[0].size != size)
            return;
        uint64_t add_res = ocerz_trunc(res - 1, size);
        uint64_t add_lhs = ocerz_trunc(add_res - src, size);
        cc_src = add_res < add_lhs;
        cc_dst = res;
        cc_op = ocerz_cc_pack(OCERZ_CC_INC, size, 0);
        break;
    }
    default:
        return;
    }

    cpu->cc_src = cc_src;
    cpu->cc_dst = cc_dst;
    cpu->cc_op = cc_op;
}

int ocerz_jit_fault_rip(const struct OcerzVM *vm, const void *host_pc, uint64_t *out_rip)
{
    const OcerzJit *jit = vm ? vm->jit : NULL;
    const uint32_t *pc = (const uint32_t *)host_pc;
    const JitBlock *b = fault_block(jit, pc);
    if (!b || !b->insn_off)
        return 0;
    int i = fault_insn_index(b, pc);
    if (i < 0)
        return 0;
    *out_rip = b->insns[i].rip;
    return 1;
}

int ocerz_jit_fault_info(const struct OcerzVM *vm, const void *host_pc,
                         OcerzJitFaultInfo *out)
{
    const OcerzJit *jit = vm ? vm->jit : NULL;
    const uint32_t *pc = (const uint32_t *)host_pc;
    const JitBlock *b = fault_block(jit, pc);
    if (!b || !out)
        return 0;
    memset(out, 0, sizeof(*out));
    out->block_rip = b->guest_rip;
    out->host_word = (uint32_t)(pc - (const uint32_t *)b->code);
    out->insn_index = fault_insn_index(b, pc);
    if (out->insn_index >= 0)
        out->insn_rip = b->insns[out->insn_index].rip;
    out->n_pinned = b->n_pinned;
    out->pin_class = b->pin_class;
    memcpy(out->host_holds, b->host_holds, sizeof(out->host_holds));
    return 1;
}

OcerzJit *ocerz_jit_create(struct OcerzVM *vm)
{
    OcerzJit *jit = (OcerzJit *)calloc(1, sizeof *jit);
    if (!jit)
        return NULL;
    jit->vm = vm;
    jit->plain_mem = !vm->jit_ordered_required &&
        (vm->jit_plain_mem || getenv("OCERZ_PLAIN_MEM") != NULL);
    size_t bytes = jit_code_bytes();
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) {
        OCERZ_LOG("JIT unavailable (MAP_JIT failed); using interpreter\n");
        free(jit);
        return NULL;
    }
    jit->code_base = (uint32_t *)p;
    jit->code_cur = (uint32_t *)p;
    jit->code_end = (uint32_t *)((uint8_t *)p + bytes);
    jit->code_bytes = bytes;
    OCERZ_LOG("JIT code arena %zu MB reserved at [%p,%p)\n",
              bytes >> 20, p, (void *)((uint8_t *)p + bytes));
    return jit;
}

static void ps_report(OcerzJit *jit);
static OcerzJit *g_ps_atexit_jit;
static void ps_report_atexit(void)
{
    if (g_ps_atexit_jit)
        ps_report(g_ps_atexit_jit);
}

static void block_destroy(JitBlock *b)
{
    free(b->insn_off);
    free(b->oslow);
    free(b->fault_flags);
    free(b->insns);
    free(b);
}

static void block_list_destroy(JitBlock *b)
{
    while (b) {
        JitBlock *next = b->hnext;
        block_destroy(b);
        b = next;
    }
}

static void retired_list_destroy(JitBlock *b)
{
    while (b) {
        JitBlock *next = b->retired_next;
        block_destroy(b);
        b = next;
    }
}

static void code_index_destroy(JitCodeIndex *index)
{
    while (index) {
        JitCodeIndex *older = index->older;
        free(index);
        index = older;
    }
}

static void pending_clear(void)
{
    for (unsigned i = 0; i < PEND_SIZE; i++) {
        PendingChain *e = g_pending[i];
        while (e) {
            PendingChain *next = e->next;
            free(e);
            e = next;
        }
        g_pending[i] = NULL;
    }
}

void ocerz_jit_destroy(OcerzJit *jit)
{
    if (!jit)
        return;
    if (ocerz_perfstat > 0)
        ps_report(jit);
    for (unsigned i = 0; i < JIT_HASH_SIZE; i++)
        block_list_destroy(jit->buckets[i]);
    retired_list_destroy(jit->retired);

    pending_clear();
    code_index_destroy(__atomic_load_n(&jit->ci, __ATOMIC_RELAXED));
    munmap(jit->code_base, jit->code_bytes);
    free(jit);
}

uint64_t ocerz_jit_blocks(const OcerzJit *jit)
{
    return jit ? jit->blocks_translated : 0;
}

static pthread_mutex_t jit_lock = PTHREAD_MUTEX_INITIALIZER;

__thread sigjmp_buf *ocerz_jit_decode_recover;

static int force_stop_sites_writable(OcerzJit *jit)
{
    int patched = 0;
    for (JitBlock *b = jit->stop_blocks; b; b = b->stop_next) {
        if (getenv("OCERZ_STOPLOG"))
            fprintf(stderr, "ocerz: STOPSITE rip=%#llx patch=%p cur=%08x stop_insn=%08x\n",
                    (unsigned long long)b->guest_rip, (void *)b->stop_patch,
                    b->stop_patch ? *b->stop_patch : 0u, b->stop_insn);
        if (b->stop_patch && b->stop_insn && *b->stop_patch != b->stop_insn) {
            __atomic_store_n(b->stop_patch, b->stop_insn, __ATOMIC_RELEASE);
            patched = 1;
        }
    }
    return patched;
}

static void invalidate_all_locked(OcerzJit *jit)
{
    int patched = 0;

    pthread_jit_write_protect_np(0);
    patched |= force_stop_sites_writable(jit);
    for (unsigned h = 0; h < JIT_HASH_SIZE; h++) {
        for (JitBlock *b = jit->buckets[h]; b; b = b->hnext) {
            for (int i = 0; i < b->n_edges; i++) {
                uint32_t *at = b->edges[i].patch_b;
                uint32_t fallback = b->edges[i].fallback_insn;
                if (at && fallback && *at != fallback) {
                    __atomic_store_n(at, fallback, __ATOMIC_RELEASE);
                    patched = 1;
                }
            }
        }
    }
    pthread_jit_write_protect_np(1);
    if (patched)
        sys_icache_invalidate(jit->code_base,
            (size_t)((uint8_t *)jit->code_cur - (uint8_t *)jit->code_base));

    for (unsigned h = 0; h < JIT_HASH_SIZE; h++) {
        JitBlock *b = __atomic_exchange_n(&jit->buckets[h], NULL,
                                           __ATOMIC_ACQ_REL);
        while (b) {
            JitBlock *next = b->hnext;
            b->retired_next = jit->retired;
            jit->retired = b;
            b = next;
        }
    }

    pending_clear();
    for (unsigned i = 0; i < g_ras_slot_n; i++)
        __atomic_store_n(&g_ras_slots[i], NULL, __ATOMIC_RELEASE);
    /* inline caches of indirect branches must not keep retired code alive */
    for (unsigned i = 0; i < g_ic_next && i < JIT_IC_SLOTS; i++) {
        __atomic_store_n(&g_ic_slots[i].code, NULL, __ATOMIC_RELAXED);
        __atomic_store_n(&g_ic_slots[i].rip, 0, __ATOMIC_RELEASE);
    }
}

void ocerz_jit_invalidate_all(struct OcerzVM *vm)
{
    if (!vm)
        return;

    OcerzJit *jit = vm->jit;
    if (jit) {
        pthread_mutex_lock(&jit_lock);
        invalidate_all_locked(jit);
        pthread_mutex_unlock(&jit_lock);
    }
    ocerz_vm_purge_jit_ras(vm);
}

static int ranges_overlap(uint64_t a, uint64_t alen,
                          uint64_t b, uint64_t blen)
{
    if (!alen || !blen)
        return 0;
    return a <= b ? b - a < alen : a - b < blen;
}

void ocerz_jit_invalidate_range(struct OcerzVM *vm, uint64_t addr, uint64_t len)
{
    if (!vm || !len || !vm->jit)
        return;

    OcerzJit *jit = vm->jit;
    int invalidated = 0;
    pthread_mutex_lock(&jit_lock);
    for (unsigned h = 0; h < JIT_HASH_SIZE && !invalidated; h++) {
        for (JitBlock *b = jit->buckets[h]; b && !invalidated; b = b->hnext) {
            for (int i = 0; i < b->n_insns; i++) {
                if (ranges_overlap(addr, len, b->insns[i].rip,
                                   b->insns[i].len)) {
                    invalidate_all_locked(jit);
                    invalidated = 1;
                    break;
                }
            }
        }
    }
    pthread_mutex_unlock(&jit_lock);

    if (invalidated)
        ocerz_vm_purge_jit_ras(vm);
}

void ocerz_jit_request_stop(struct OcerzVM *vm)
{
    OcerzJit *jit = vm ? vm->jit : NULL;
    if (!jit)
        return;

    pthread_mutex_lock(&jit_lock);
    jit->stop_requested = 1;
    pthread_jit_write_protect_np(0);
    int patched = force_stop_sites_writable(jit);
    pthread_jit_write_protect_np(1);
    if (patched)
        sys_icache_invalidate(jit->code_base,
            (size_t)((uint8_t *)jit->code_cur - (uint8_t *)jit->code_base));
    pthread_mutex_unlock(&jit_lock);
}

void ocerz_jit_require_ordered(struct OcerzVM *vm)
{
    if (!vm)
        return;

    vm->jit_ordered_required = 1;
    vm->jit_plain_mem = 0;

    OcerzJit *jit = vm->jit;
    if (!jit) {
        ocerz_vm_purge_jit_ras(vm);
        return;
    }

    pthread_mutex_lock(&jit_lock);
    if (jit->plain_mem) {
        jit->plain_mem = 0;
        g_plain_mem = 0;
        invalidate_all_locked(jit);
    }
    pthread_mutex_unlock(&jit_lock);

    ocerz_vm_purge_jit_ras(vm);
}

void ocerz_jit_prefork(void)
{
    pthread_mutex_lock(&jit_lock);
}

void ocerz_jit_postfork(void)
{
    pthread_mutex_unlock(&jit_lock);
}

static int jit_interp_block(struct OcerzVM *vm, OcerzCPU *cpu, JitBlock *b)
{

    ocerz_flags_materialize(cpu);
    if (ocerz_perfstat > 0)
        b->exec_count++;
    for (int i = 0; i < b->n_insns; i++) {
        int r = ocerz_jit_exec_one(vm, cpu, &b->insns[i]);
        if (r != OCERZ_STEP_OK)
            return r;
    }
    return OCERZ_STEP_OK;
}

typedef struct { unsigned op; unsigned long long n; } PsOpRow;

static int ps_cmp(const void *a, const void *bb)
{
    unsigned long long x = ((const PsOpRow *)a)->n, y = ((const PsOpRow *)bb)->n;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void ps_report(OcerzJit *jit)
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

    /* HOTBLOCKS: top blocks by exec_count*code_words (static cost estimate) */
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
                    (int)getpid(), k + 1, (unsigned long long)bb->guest_rip,
                    (unsigned long long)bb->exec_count, bb->n_insns, bb->code_words,
                    bb->n_insns ? (double)bb->code_words / bb->n_insns : 0.0,
                    (int)bb->pin_class, bb->n_slow);
        }
    }
    fprintf(stderr, "ocerz: PERFSTAT[%d]   IC miss_calls=%llu fills=%llu nocode=%llu slots_used=%u\n",
            (int)getpid(), (unsigned long long)g_ic_miss_calls, (unsigned long long)g_ic_fills,
            (unsigned long long)g_ic_nocode, g_ic_next);
    PsOpRow rows[OCERZ_OP_COUNT];
    for (unsigned i = 0; i < OCERZ_OP_COUNT; i++) {
        rows[i].op = i;
        rows[i].n = ps_ops[i];
    }
    qsort(rows, OCERZ_OP_COUNT, sizeof rows[0], ps_cmp);
    unsigned long long cum = 0;
    for (int i = 0; i < 24 && rows[i].n; i++) {
        cum += rows[i].n;
        fprintf(stderr, "ocerz: PERFSTAT[%d]   SLOWOP #%2d %-12s %14llu  %5.2f%% of slow  cum %5.2f%%  (%.2f%% of ALL)\n",
                (int)getpid(), i + 1, ocerz_op_name(rows[i].op), rows[i].n,
                slow ? 100.0 * (double)rows[i].n / (double)slow : 0.0,
                slow ? 100.0 * (double)cum / (double)slow : 0.0,
                total ? 100.0 * (double)rows[i].n / (double)total : 0.0);
    }
    {
        unsigned long long cok = ps_chain_ok, cfar = ps_chain_far, ctot = cok + cfar;
        if (ctot)
            fprintf(stderr,
                    "ocerz: PERFSTAT[%d]   CHAIN activated=%llu out_of_range=%llu (%.2f%% dropped)\n",
                    (int)getpid(), cok, cfar, 100.0 * (double)cfar / (double)ctot);
    }
    for (int i = 0; i < (int)(sizeof ps_shape / sizeof ps_shape[0]); i++) {
        unsigned long long easy = ps_shape[i][0], hard = ps_shape[i][1], s = easy + hard;
        if (s)
            fprintf(stderr, "ocerz: PERFSTAT[%d]   SHAPE %-7s easy=%llu (%.1f%%) other=%llu (%.1f%%)\n",
                    (int)getpid(), ps_shape_name[i], easy, 100.0 * (double)easy / (double)s,
                    hard, 100.0 * (double)hard / (double)s);
    }
}

int ocerz_jit_step(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (cpu->rip - OCERZ_DYLDAPI_LO < (OCERZ_DYLDAPI_HI - OCERZ_DYLDAPI_LO))
        return OCERZ_EUNSUP;
    OcerzJit *jit = vm->jit;
    if (ocerz_jitstat < 0) {
        pthread_mutex_lock(&jit_lock);
        if (ocerz_jitstat < 0) {
            js_t0 = ps_t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            ocerz_perfstat = getenv("OCERZ_PERFSTAT") ? 1 : 0;
            ocerz_jitstat = getenv("OCERZ_JITSTAT") ? 1 : 0;
            if (ocerz_perfstat > 0) {
                g_ps_atexit_jit = jit;
                atexit(ps_report_atexit);
            }
            g_flaglive_log = getenv("OCERZ_FLAGLIVE") ? 1 : 0;
            g_no_lazyflags = getenv("OCERZ_NO_LAZYFLAGS") ? 1 : 0;
            g_no_ras = getenv("OCERZ_NO_RAS") ? 1 : 0;
            g_no_ldapr = getenv("OCERZ_NO_LDAPR") ? 1 : 0;
            g_no_oolslow = getenv("OCERZ_NO_OOLSLOW") ? 1 : 0;
            g_no_regflags = getenv("OCERZ_NO_REGFLAGS") ? 1 : 0;
            g_no_chain = getenv("OCERZ_NO_CHAIN") ? 1 : 0;
            g_no_jcclink = getenv("OCERZ_NO_JCCLINK") ? 1 : 0;
            g_no_xlive = getenv("OCERZ_NO_XLIVE") ? 1 : 0;
            g_no_jccfuse = getenv("OCERZ_NO_JCCFUSE") ? 1 : 0;
            g_no_addincfuse = getenv("OCERZ_NO_ADDINCFUSE") ? 1 : 0;
            g_no_fault_recipes = getenv("OCERZ_NO_FAULT_RECIPES") ? 1 : 0;
            g_plain_mem = jit->plain_mem;
        }
        pthread_mutex_unlock(&jit_lock);
    }
    if (ocerz_jitstat > 0)
        js_steps++;
    if (ocerz_perfstat > 0) {

        unsigned long long s = ++ps_steps;
        if ((s & 0xfffff) == 0) {
            static _Atomic uint64_t ps_next;
            uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            uint64_t exp = ps_next;
            if (now >= exp && __c11_atomic_compare_exchange_strong(
                    &ps_next, &exp, now + 15000000000ull,
                    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                ps_report(jit);
        }
    }

    JitBlock *b = cache_lookup(jit, cpu->rip);
    if (!b) {
        pthread_mutex_lock(&jit_lock);
        if (ocerz_perfstat > 0)
            ps_misses++;
        if (ocerz_jitstat > 0) {
            js_misses++;

            if ((js_misses & 0x3ff) == 0) {
                static uint64_t next_s, next_f;
                uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                if (now >= next_f) {
                    next_f = now + 60000000000ull; next_s = now + 10000000000ull;
                    js_report(jit, "FULL", 1);
                } else if (now >= next_s) {
                    next_s = now + 10000000000ull;
                    js_report(jit, "tick", 0);
                }
            }
        }
        b = cache_lookup(jit, cpu->rip);
        if (!b) {
            if (ocerz_jitstat > 0) js_xlat++;
            g_plain_mem = jit->plain_mem;
            b = translate(jit, cpu->rip);
        }
        pthread_mutex_unlock(&jit_lock);
    } else {
        if (ocerz_jitstat > 0)
            js_hits++;
        if (ocerz_perfstat > 0)
            ps_hits++;
    }

    if (!b)
        return OCERZ_EUNSUP;
    if (!b->code)
        return jit_interp_block(vm, cpu, b);
    return b->code(vm, cpu);
}

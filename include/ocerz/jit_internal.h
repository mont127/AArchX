/*
 * Shared JIT layouts, translation state and emitter contracts. The subsystem
 * translation units use this header so generated code and fault recovery agree
 * on register assignments, offsets and the one lock-protected translator state.
 *
 * the probe's two branches, kept while it only watches the taken side
 *
 * the TOP a run with a known TOP leaves, or -1
 */
#ifndef OCERZ_JIT_INTERNAL_H
#define OCERZ_JIT_INTERNAL_H

#include <execinfo.h>
#include <dlfcn.h>
#include "ocerz/jit.h"
#include "ocerz/dyld.h"
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/decode.h"
#include "ocerz/interp.h"
#include "ocerz/flags.h"
#include "ocerz/flags_live.h"
#include "ocerz/a64emit.h"
#include "ocerz/dyldapi.h"
#include "ocerz/cache.h"
#include "ocerz/mode.h"
#include "ocerz/vdylib.h"
#include "ocerz/leaf.h"
#include "ocerz/tcache.h"
#include "ocerz/x87.h"
#include <sys/mman.h>
#include <mach/thread_act.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <setjmp.h>
#include <libkern/OSCacheControl.h>
#include <stddef.h>
#include <stdlib.h>
#include <assert.h>

#define JIT_CODE_BYTES_DEFAULT (1024ull << 20)
#define JIT_HASH_BITS 20
#define JIT_HASH_SIZE (1u << JIT_HASH_BITS)
#define JIT_HASH_MASK (JIT_HASH_SIZE - 1)
#define JIT_MAX_BLOCK_INSNS 256
#define JIT_KEY_M32 (1ull << 63)
#define JIT_MAX_EDGES 8
#define INVMAP_RSHIFT 22
#define INVMAP_GSHIFT 16
#define INVMAP_SLOTS  8192
#define INVMAP_PROBE  32
#define INVMAP_MAX_SPAN 64
#define INVMAP_TOMB ((uint64_t)-1)
#define OSLOW_MAX 64
#define NANOOL_MAX 64
#define PE_MAX 48
#define l0_src(r, dbl) l0_src2(b, r, dbl)
#define NZCV_KIND_BT 0x7f
#define TC_RELOC_MAX 1024
#define RASLIT_MAX 96
#define PSC_N 32
#define PSC_GEN_SHIFT 48
#define PSC_EMPTY_RIP UINT64_MAX
#define TC_DLOG_MAX 4096
#define TC_DBYTES_MAX (64u << 10)
#define SIDE_MAX 6
#define FLIP_N 4096
#define PROBE_BIT 10
#define WATCH_BIT 16
#define FLIP_REARMS 3
#define PROBE_MAX 65536
#define CP_MARK_SIZE (1u << 18)
#define AL_MARK_SIZE (1u << 18)
#define XLP_SIZE (1u << 17)
#define AL_BLK_TAG (1ull << 62)
#define JMEMBASE 17
#define JMEMAUX 29
#define JMEMBASE2 16
#define JMEMBASE3 30
#define JGB 0
#define PS_RETSITE_N 65536
#define ENV_ON(name) ({ static int on_ = -1;                     \
                        if (on_ < 0) on_ = getenv(name) != NULL; \
                        on_; })
#define GRAN_SLOTS 65536
#define GRAN4_SLOTS 16384
#define GBLK_SLOTS 65536
#define XLIVE_MEMO_SLOTS 8192
#define XLIVE_MEMO_DEPS 6
#define XLIVE_NO_DEPS 0xff
#define RF_OFF ((uint32_t)offsetof(OcerzCPU, rflags))
#define RIP_OFF ((uint32_t)offsetof(OcerzCPU, rip))
#define SIDE_BLK_OFF ((uint32_t)offsetof(OcerzCPU, side_blk))
#define SIDE_IDX_OFF ((uint32_t)offsetof(OcerzCPU, side_idx))
#define INT_OFF ((uint32_t)offsetof(OcerzCPU, interrupt))
#define GPR_OFF(r) ((uint32_t)((unsigned)(r) * 8))
#define CC_SRC_OFF ((uint32_t)offsetof(OcerzCPU, cc_src))
#define CC_DST_OFF ((uint32_t)offsetof(OcerzCPU, cc_dst))
#define CC_OP_OFF ((uint32_t)offsetof(OcerzCPU, cc_op))
#define RAS_TOP_OFF ((uint32_t)offsetof(OcerzCPU, ras_top))
#define FCMP_MEM_OFF ((uint32_t)offsetof(OcerzCPU, jit_fcmp_mem))
#define RAS_OFF ((uint32_t)offsetof(OcerzCPU, ras))
#define JIT_FP_OFF ((uint32_t)offsetof(OcerzCPU, jit_fp))
#define JRET_GUEST 27
#define JRET_HOST  28
#define JIT_ARITH_FLAGS (OCERZ_CF | OCERZ_PF | OCERZ_AF | OCERZ_ZF | OCERZ_SF | OCERZ_OF)
#define GUARD_ARMS_MAX 128
#define L0_NLANES 12
#define FPB_UNDO_MAX 8
#define YMMH_ALL_ZERO_OFF ((uint32_t)offsetof(OcerzCPU, ymmh_all_zero))
#define NZCV_GAP_MAX 3
#define XMM_BASE_OFF ((uint32_t)offsetof(OcerzCPU, xmm))
#define FPB_MAX 32
#define FPB_SITES_MAX 128
#define FPCKPT_OFF ((uint32_t)offsetof(OcerzCPU, fp_ckpt))
#define YMMH_OFF ((uint32_t)offsetof(OcerzCPU, ymmh))
#define l0_alloc(r, dbl) l0_alloc2(b, r, dbl)
#define MMX_OFF(r) ((uint32_t)offsetof(OcerzCPU, mmx) + 8u * (uint32_t)(r))
#define RFLAGS_OFF ((uint32_t)offsetof(OcerzCPU, rflags))
#define X87_CTL_OFF ((uint32_t)offsetof(OcerzCPU, fcw))
#define X87_FPR_OFF ((uint32_t)offsetof(OcerzCPU, fpr))
#define X87_XM_OFF ((uint32_t)offsetof(OcerzCPU, fpr_xm))
#define X87_XE_OFF ((uint32_t)offsetof(OcerzCPU, fpr_xe))
#define X87_MXCSR_OFF ((uint32_t)offsetof(OcerzCPU, mxcsr))
#define X87_TOP0_OFF ((uint32_t)offsetof(OcerzCPU, jit_x87_top0))
#define JIT_SCRATCH_OFF ((uint32_t)offsetof(OcerzCPU, jit_scratch))
#define XS_XOK 48
#define XS_PC (0x300ull)
#define XS_PE (1ull << 21)
#define XS_C1 (1ull << 25)
#define XS_C3 (1ull << 30)
#define X87_RUN_MAX 64
#define X87_SITE_MAX 2048
#define X87_FRAG_MAX 512
#define A64_NOP 0xd503201fu
#define BRIDGE_FAST_DEPTH_MAX_K 16
#define LEAF_EPOCH_OFF ((uint32_t)offsetof(OcerzCPU, jit_scratch) + 8)
#define JS_FTAB (1u << 20)
#define CHAIN_BATCH_MAX 64
#define VENEER_WINDOW_BYTES (64u << 20)
#define VENEER_POOL_BYTES (256u << 10)
#define VENEER_BYTES 16u
#define VENEER_REACH ((ptrdiff_t)(120u << 20))
#define ALIGN_ARM_BYTES 512u
#define PEND_BITS 18
#define PEND_SIZE (1u << PEND_BITS)
#define PEND_MASK (PEND_SIZE - 1)
#define RAS_SLOT_CAP (1u << 18)
#define LOWHOIST_N 4096
#define OOLSLOW_MAX 32
#define TC_NONE UINT32_MAX
#define TC_OUT_MAX (256u << 10)
#define TC_HASH_SEED 0x243f6a8885a308d3ull
#define CHURN_SLOTS 4096
#define CHURN_LIMIT 3
#define CHURN_QUIET_NS 1500000000ull
#define CHURN_REFUSE_MAX 4096ull
#define TRIP_SLOTS 4096

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

typedef struct JitProf {
    uint32_t taken, ft;
    uint32_t *ft_site;
    uint32_t *tk_trip;
    uint8_t windows, prev, rearms;
    uint32_t ft_word, tk_word;
} JitProf;

typedef struct JitInsnRef {
    uint64_t rip;
    uint16_t op;
    uint8_t len;
    uint8_t flags;
    uint16_t keep;
    uint16_t pad;
} JitInsnRef;

struct JitLaneRec { uint32_t off; uint16_t dirty; uint8_t l0[16]; uint8_t yc[16]; };

struct JitPromo { int32_t pi, qi; uint8_t hreg; };

typedef struct JitBlock {
    uint64_t key;
    JitBlockFn code;
    uint32_t *body_code;
    uint32_t *body_noreload;
    uint64_t hoist_sig;
    X86Insn *insns;
    int n_insns;
    JitInsnRef *iref;
    X86Insn *kept;
    uint16_t n_kept;
    struct JitBlock *hnext;
    struct JitBlock *retired_next;
    size_t live_idx;

    uint64_t exec_count;
    int n_inlined;
    int n_slow;

    uint32_t *insn_off;

    struct JitOslowMap { uint32_t lo, hi; int32_t idx; } *oslow;
    int n_oslow;
    struct JitLaneRec *lanerec;
    int n_lanerec;
    JitFaultFlagRecipe *fault_flags;
    uint32_t code_words;

    uint32_t *stop_patch;
    uint32_t stop_insn;
    struct { uint32_t *site; uint32_t insn; } stop_extra[6];
    uint8_t n_stop_extra;
    uint32_t *push_fix;
    uint16_t n_push_fix;
    struct JitPushElide { int32_t ci, rj; uint64_t ra; } *pushelide;
    uint16_t n_pushelide;
    struct JitBlock *stop_next;

    uint8_t host_holds[16];
    int8_t guest_in_host[16];
    uint8_t n_pinned;

    uint8_t pin_class;
    uint8_t inv_hit;

    struct {
        uint64_t target_rip;
        uint32_t *patch_b;
        uint32_t fallback_insn;
        uint32_t *cond_site;
        uint32_t cond_orig;
        uint8_t kind;
        uint8_t pin_class;
        uint8_t side;
        uint8_t probing;
        uint64_t jcc_rip;
    } *edges;
    uint8_t n_edges;
    JitProf *prof;
    struct { struct JitBlock *pb; uint8_t e; } *preds;
    uint32_t n_preds, cap_preds;

    uint16_t entry_live;
    uint16_t xmm_pinned;
    uint8_t ordered_loads;
} JitBlock;

typedef struct { uint64_t tag, bits; } InvSlot;

typedef struct JitCodeIndex {
    struct JitCodeIndex *older;
    size_t capacity;
    size_t count;
    JitBlock *blocks[];
} JitCodeIndex;

enum { EDGE_XBLOCK = 0, EDGE_SELFLOOP = 1, EDGE_BODY = 2 };

struct OcerzJit {
    int owner_pid;
    struct OcerzVM *vm;
    uint32_t *code_base;
    const char *leaf_near;
    uint32_t *code_start;
    uint32_t *code_cur;
    uint32_t *code_end;
    size_t code_bytes;
    uint32_t *veneer_pool[16];
    unsigned veneer_used[16];
    unsigned veneer_n;
    uint8_t *veneer_next_mark;
    int code_full;
    JitBlock *buckets[JIT_HASH_SIZE];
    JitBlock *retired;
    JitBlock *stop_blocks;
    int plain_mem;
    int stop_requested;
    uint64_t blocks_translated;

    JitCodeIndex *ci;
    uint32_t *dispatch_stub;
    uint32_t *dispatch_stub32;
    JitBlock **live;
    size_t n_live, cap_live;
    uint64_t code_lo, code_hi;
    InvSlot invmap[INVMAP_SLOTS];
    int invmap_full;
};

typedef struct {
    uint32_t *bne;
    uint32_t *back;
    int size, rv, ra, store, idx;
    int vec;
    int32_t disp;
} OrderedSlowPend;

typedef struct {
    uint32_t *site;
    uint32_t *back;
    uint8_t dbl, packed, vr, va, vb, t1;
    uint8_t cvt;
    uint8_t refcmp;
    uint8_t pre, pvr, pva, pvb;
    int idx;
    int is_cbz;
} NanOolPend;

typedef struct { uint32_t *site; uint64_t retaddr; uint64_t hi; int kind; int rt; int tcr; } RasLit;

enum { TCR_SYM = 1, TCR_COMMPAGE, TCR_BUCKETS, TCR_LEAF, TCR_DSTUB, TCR_BLK, TCR_INSN, TCR_PROF,
       TCR_RASSLOT, TCR_PSC, TCR_RASCELL };

enum { TCS_FLAGS_MATERIALIZE, TCS_RAS_PUSH, TCS_EXEC_ONE, TCS_EXEC_ONE_AT, TCS_JGB_TRAP,
       TCS_RETIRE_COUNT, TCS_EXEC_RUN_AT, TCS_N };

typedef struct { uint32_t off; uint8_t kind, form; uint64_t arg; } TcReloc;

void ocerz_jit_emit_audit(uint64_t rip, const uint32_t *code, uint32_t nwords,
                          const X86Insn *insns, uint32_t ninsns,
                          const TcReloc *rel, uint32_t nrel);

typedef struct { _Alignas(16) uint64_t rip; void *body; } JitPscEnt;

_Static_assert(PSC_N == OCERZ_PSC_COLS, "a PSC column per retire generation");

struct JitState_g_tc_dlog { uint64_t pc; uint32_t at; uint32_t len; };

struct JitState_g_side { uint32_t *site; uint64_t taken; int idx; uint32_t *stub; uint32_t *patch_b;
                int rec; uint32_t rec_ccop; int rec_src, rec_dst, rec_imm_pending; uint64_t rec_imm;
                uint64_t jcc_rip; int probe; uint32_t *ft_site; uint64_t ft_rip;
                int fpb; uint16_t fpb_chk; int fpb_end; int8_t l0[16]; uint8_t l0_dbl[16];
                uint16_t l0_dirty; uint16_t yc_dirty; };

enum { FLIP_NONE = 0, FLIP_DECIDED_ORIG, FLIP_DECIDED_INV };

struct JitState_g_flip { uint64_t rip; uint8_t state; };

struct JitState_g_stop_extra { uint32_t *site; uint32_t *target; };

struct JitState_g_jcc_edge {
    uint64_t target_rip;
    uint32_t *patch_b;
    uint32_t *cond_site;
    uint8_t kind;
    uint8_t pin_class;
};

struct JitState_g_call_edge {
    uint64_t target_rip;
    uint32_t *patch_b;
    uint8_t kind;
    uint8_t pin_class;
};

struct JitState_ps_retsite { uint64_t rip, n; };

typedef struct JitThr {
    int frames, parked, dead;
    OcerzCPU *cpu;
    struct JitThr *next;
} JitThr;

typedef struct { uint64_t pc; uint32_t len; } XliveDep;

enum { JT0 = 9, JT1 = 10, JT2 = 11, JTF = 12, JTT = 13, JTU = 14, JTA = 15 };

_Static_assert(OCERZ_TOP_LO == (1ull << 47) - (1ull << 25), "the fast low guard tests the top strip with shifts by 25 and 22");

struct JitState_g_ea_cache {
    int valid; unsigned base, index; int scale;
    unsigned long long seq;
    const uint32_t *after;
};

enum { VX0 = 0, VX1 = 1, VX2 = 2, VX3 = 3 };

_Static_assert(offsetof(OcerzCPU, xmm) % 16 == 0, "xmm must be 16-aligned for scaled q loads");

struct JitState_g_scpend { int valid, idx, dbl, vr, va, vb; };

typedef struct {
    int first, last;
    uint16_t ckpt, ckpt_emit, written, dirty_open;
    uint16_t full, s0, d0, dblonly;
    int gain;
    uint32_t *site;
    uint32_t *back;
    int8_t l0[16];
    uint8_t l0_dbl[16];
    int8_t fcmp_vreg;
    int end;
} FpBatch;

typedef struct {
    int batch, end;
    uint32_t *site, *back;
    int8_t l0[16]; uint8_t l0_dbl[16];
    int8_t fcmp_a, fcmp_b; uint8_t fcmp_dbl; uint8_t keep_jt;
} FpbSite;

enum { RK_END = 0, RK_DET, RK_STORE, RK_LOAD, RK_MOVE, RK_LMOVE, RK_UNPCKH, RK_UNPCKL };

_Static_assert(offsetof(OcerzCPU, fp_ckpt) % 16 == 0 && offsetof(OcerzCPU, fp_ckpt) + 256 <= 65520,
               "fp_ckpt must be q-addressable");

enum { K2_END = 0, K2_ARITH, K2_MOVE, K2_LMOVE, K2_STORE, K2_ZERO, K2_UNPCKH, K2_UNPCKL, K2_DUP, K2_DET, K2_SHUF };

_Static_assert(offsetof(OcerzCPU, ymmh) % 16 == 0 && offsetof(OcerzCPU, ymmh) + 256 <= 65520,
               "ymmh must be q-addressable");

enum { SIK_XOR = 1, SIK_AND, SIK_OR, SIK_ANDN, SIK_ADD, SIK_SUB, SIK_CMPEQ, SIK_CMPGT, SIK_UMIN, SIK_UMAX,
       SIK_SMIN, SIK_SMAX, SIK_MUL, SIK_UQADD, SIK_UQSUB, SIK_SQADD, SIK_SQSUB, SIK_AVG,
       SIK_MADDWD, SIK_MULHRSW, SIK_PACKSSDW, SIK_PACKUSWB, SIK_MULUDQ, SIK_MULHW, SIK_MULHUW,
       SIK_PACKSSWB, SIK_PACKUSDW, SIK_MULDQ, SIK_SADBW, SIK_MADDUBSW, SIK_ABS, SIK_SIGN, SIK_HADD, SIK_HSUB,
       SIK_HADDS, SIK_HSUBS };

_Static_assert(offsetof(OcerzCPU, fcw) % 8 == 0 && offsetof(OcerzCPU, fsw) == offsetof(OcerzCPU, fcw) + 2 &&
               offsetof(OcerzCPU, ftw) == offsetof(OcerzCPU, fcw) + 4 &&
               offsetof(OcerzCPU, ftop) == offsetof(OcerzCPU, fcw) + 5 &&
               offsetof(OcerzCPU, fpr_x_ok) == offsetof(OcerzCPU, fcw) + 6 &&
               offsetof(OcerzCPU, fpr) >= offsetof(OcerzCPU, fcw) + 8,
               "fcw, fsw, ftw, ftop and fpr_x_ok are loaded and stored as one 8-byte word");

_Static_assert(offsetof(OcerzCPU, jit_fcmp_mem) % 8 == 0 && offsetof(OcerzCPU, jit_fcmp_mem) <= 4 * 4095,
               "jit_fcmp_mem is reached by an 8- and a 4-byte scaled access");

_Static_assert(offsetof(OcerzCPU, fpr_xm) + 64 <= 8 * 4095 && offsetof(OcerzCPU, fpr_xe) + 16 <= 2 * 4095 &&
               offsetof(OcerzCPU, jit_x87_top0) % 2 == 0 && offsetof(OcerzCPU, jit_x87_top0) <= 2 * 4095 &&
               offsetof(OcerzCPU, mxcsr) % 4 == 0, "x87 fields within scaled immediate reach");

enum { X87S = JT1, X87P = JT2, X87Q = JTF };

enum { X87R_OK = 1, X87R_FCW = 2, X87R_MXCSR = 4, X87R_END = 8, X87R_RC = 16 };

enum { XF_PE, XF_PC24, XF_ZERO, XF_ST32, XF_SETPE, XF_TOP0 };

enum { XK_ADD, XK_SUB, XK_MUL, XK_DIV, XK_SQRT };

typedef struct {
    int16_t first, last;
    uint32_t *back;
    uint8_t lv_end;
    int8_t lmap[8];
    int8_t top_end;
    int8_t l0[16];
    uint8_t l0_dbl[16];
    uint16_t l0_dirty, yc_dirty;
} X87Run;

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

typedef struct { uint64_t rip; unsigned long long n; unsigned char bytes[8];
                 unsigned reason; int nins; } JsFail;

enum { JSR_DECODE0 = 1, JSR_OVERFLOW = 2, JSR_ALLOC = 3 };

typedef struct PendingChain {
    uint64_t target_key;
    uint32_t *patch_b;
    uint32_t *cond_site;
    void **ras_slot;
    uint64_t src_sig;
    JitBlock *src;
    uint8_t edge;
    uint8_t kind;
    uint8_t pin_class;
    struct PendingChain *next;
} PendingChain;

typedef struct { uint64_t off[LOWHOIST_N]; int full; } MarkSet;

typedef struct {
    OcerzTcRecHead h;
    uint64_t dep_hash, hoist_sig;
    uint32_t code_words, entry_mod, n_insns, n_kept, n_rel, n_dep, n_oslow, n_lanerec;
    uint32_t n_push_fix, n_pushelide, body_code, body_noreload, stop_patch, stop_insn;
    int32_t n_inlined, n_slow;
    uint16_t entry_live, xmm_pinned;
    uint8_t n_edges, n_stop_extra, n_pinned, pin_class, ordered_loads, flags, pad[6];
    uint8_t host_holds[16];
    int8_t guest_in_host[16];
} TcRec;

typedef struct { uint64_t lo, hi; } TcDep;

typedef struct { uint32_t off, insn; } TcStop;

typedef struct {
    uint64_t target_rip, jcc_rip;
    uint32_t patch_b, fallback_insn, cond_site, cond_orig;
    uint8_t kind, pin_class, side, probing, pad[4];
} TcEdge;

typedef struct { uint32_t ft_site, tk_trip; } TcProf;

enum { TCF_COMPACT = 1, TCF_FAULTF = 2, TCF_PROF = 4, TCF_LEARNED = 8 };

_Static_assert(sizeof(TcRec) % 8 == 0 && sizeof(TcEdge) % 8 == 0, "tcache record layout");

typedef struct {
    const TcRec *r;
    const uint32_t *code;
    const TcReloc *rel;
    const TcDep *dep;
    const uint32_t *insn_off;
    const JitInsnRef *iref;
    const X86Insn *insns;
    const JitFaultFlagRecipe *ff;
    const struct JitOslowMap *oslow;
    const struct JitLaneRec *lanerec;
    const uint32_t *push_fix;
    const struct JitPushElide *pushelide;
    const TcStop *stop;
    const TcEdge *edge;
    const TcProf *prof;
} TcView;

typedef struct { unsigned op; unsigned long long n; } PsOpRow;

typedef struct { uint32_t *site; uint32_t was, now; } StopUndo;

extern int ocerz_perfstat;
extern int g_flaglive_log;
extern int g_no_lazyflags;
extern int g_no_ras;
extern int g_no_ldapr;
extern int g_no_oolslow;
extern int g_ea_is_const;
extern int g_ea_plain;
extern NanOolPend g_nanool[NANOOL_MAX];
extern int g_n_nanool;
extern struct JitPushElide g_pe_real[PE_MAX];
extern int g_n_pe_real;
extern struct JitPromo g_promo_real[PE_MAX];
extern int g_n_promo_real;
extern const X86Insn *g_pe_insns;
extern int g_n_oslow;
extern int g_cur_insn_idx;
extern const X86Insn *g_flag_producer;
extern int g_no_regflags;
extern unsigned long long g_callout_seq;
extern int g_jcc_side_mode;
extern uint64_t g_jcc_side_need;
extern uint64_t g_jcc_side_fall_need;
extern int g_nzcv_want;
extern int g_nzcv_from;
extern unsigned g_nzcv_kind;
extern int g_no_chain;
extern int g_no_jcclink;
extern int g_no_xlive;
extern int g_no_jccfuse;
extern int g_no_addincfuse;
extern int g_plain_mem;
extern uint64_t g_chain_target;
extern uint32_t *g_chain_epi;
extern int g_chain_keeps_jgb;
extern TcReloc g_tc_rel[TC_RELOC_MAX];
extern int g_tc_nrel;
extern int g_tc_on;
extern int g_tc_bad;
extern const uint32_t *g_tc_entry;
extern RasLit g_raslit[RASLIT_MAX];
extern int g_n_raslit;
void emit_const_lit(A64Buf *b, int rd, uint64_t v);
extern struct JitState_g_tc_dlog g_tc_dlog[TC_DLOG_MAX];
extern uint8_t g_tc_dbytes[TC_DBYTES_MAX];
extern int g_tc_ndlog;
extern uint32_t g_tc_nbytes;
extern int g_tc_dbar;
extern int g_tc_rec;
extern int g_tc_learned;
int jit_decode(uint64_t pc, X86Insn *out, int mode32);
extern uint64_t *g_tc_noload;
extern size_t g_tc_noload_cap;
extern size_t g_tc_noload_n;
void tc_noload_add(uint64_t key);
JitPscEnt *psc_alloc(void);
extern void ***g_ras_cells;
extern size_t g_n_ras_cells;
extern size_t g_cap_ras_cells;
extern uint64_t g_self_rip;
extern uint32_t *g_body_entry;
extern uint32_t *g_loop_entry;
extern uint32_t *g_stop_patch;
extern struct JitState_g_side g_side[SIDE_MAX];
extern int g_n_side;
extern struct JitState_g_flip g_flip[FLIP_N];
extern int g_n_probes;
int flip_disabled(void);
int superblock_enabled(void);
int jcc_flip_wanted(uint64_t jcc_rip);
int superblock_back_enabled(void);
extern uint32_t g_push_fix[JIT_MAX_BLOCK_INSNS];
extern int g_n_push_fix;
extern uint64_t g_cp_marks[CP_MARK_SIZE];
extern int g_cp_guard;
extern int g_low_top;
extern int g_lowstack;
extern int g_m32low;
int stack_plain_ok(void);
extern uint64_t g_al_marks[AL_MARK_SIZE];
extern pthread_mutex_t jit_lock;
extern int g_jl_log;
extern uint64_t g_jl_owner;
extern uint64_t g_jl_since;
extern int g_jl_phase;
extern __thread int jl_held;
void jl_acquire(int site);
extern int g_align_guard;
int vec_tso_relaxed(void);
extern int g_vec_int_move;
extern unsigned long long ps_align_patches;
extern int g_blk_ordered_loads;
extern int g_al_all;
extern int g_al_n;
extern const uint32_t *g_push_entry;
extern struct JitState_g_stop_extra g_stop_extra[6];
extern int g_n_stop_extra;
extern uint32_t *g_stop_target;
uint32_t stop_retarget(uint32_t insn, const uint32_t *site,
                              const uint32_t *target);
extern int g_mem_hoist_greg;
extern int g_low_hoist_greg;
extern int g_n_low_hoist_bail;
extern int g_ea_lowhoisted;
extern int g_mem_hoist_aux_disp;
extern int g_mem_hoist_aux_index;
extern int g_mem_hoist_aux_scale;
extern int g_mem_hoist_greg2;
extern int g_mem_hoist_greg3;
int stack_identity(void);
void ocerz_jgb_trap(uint64_t rip, uint64_t x0);
extern struct JitState_g_jcc_edge g_jcc_edge[2];
extern int g_n_jcc_edges;
extern struct JitState_g_call_edge g_call_edge[2];
extern int g_n_call_edges;
extern OcerzJit *g_xlat_jit;
extern int g_xlat_mode32;
extern int8_t *g_pin;
extern uint8_t *g_pin_hold;
extern int g_n_pinned;
extern int g_pin_class;
int rsp_ptr3(void);
int g_pin_class_fwd(void);
extern int ocerz_jit_time_xlat;
extern uint64_t ocerz_jit_retire_ns;
extern int g_defer;
extern int16_t g_mov_sink_at[JIT_MAX_BLOCK_INSNS];
extern uint8_t g_mov_skip[JIT_MAX_BLOCK_INSNS];
extern _Atomic unsigned long long ps_ops[OCERZ_OP_COUNT];
extern char ps_shapes[OCERZ_OP_COUNT][3][96];
extern _Atomic unsigned long long ps_slow_insns;
extern _Atomic unsigned long long ps_steps;
extern _Atomic unsigned long long ps_hits;
extern _Atomic unsigned long long ps_misses;
extern _Atomic unsigned long long ps_shape[9][2];
extern _Atomic unsigned long long ps_chain_ok;
extern _Atomic unsigned long long ps_chain_far;
extern unsigned long long ps_ras_miss;
extern unsigned long long ps_ras_stale;
extern unsigned long long ps_ras_null;
extern unsigned long long ps_ras_sentinel;
extern unsigned long long ps_ras_noslot;
extern struct JitState_ps_retsite ps_retsite[PS_RETSITE_N];
extern unsigned long long ps_chain_veneer;
extern uint64_t ps_t0;
extern __thread int t_xlat_overflow;
extern __thread int ocerz_jit_exec_state;
__attribute__((noinline))
int jit_exec_one_parked(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn, const void *ret);
__attribute__((noinline))
int ocerz_jit_exec_one(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn);
int jit_exec_one(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn);
__attribute__((noinline))
int ocerz_jit_exec_one_at(struct OcerzVM *vm, OcerzCPU *cpu, const JitBlock *b, uint64_t idx);
__attribute__((noinline))
int ocerz_jit_exec_run_at(struct OcerzVM *vm, OcerzCPU *cpu, const JitBlock *b, uint64_t idx, uint64_t last);
extern JitBlock *g_cur_blk;
extern uint8_t *g_keep;
extern int g_keep_n;
extern int g_no_compact;
int is_terminator(unsigned op);
JitBlock *cache_lookup(OcerzJit *jit, uint64_t rip, int mode32);
void gran_block(JitBlock *b, int d);
void cache_insert(OcerzJit *jit, JitBlock *b);
extern int g_xlive_log;
extern uint64_t ocerz_jit_retire_count;
uint64_t xlive_decode_entry_d(uint64_t rip, int depth);
int canonical_body_successor(uint64_t rip);
unsigned decoded_terminator(uint64_t rip);
int decoded_call_region_entry(uint64_t rip);
int host_ras_enabled(void);
void emit_push_pinned(A64Buf *b, int hs, int rv);
int low_stack_fast(void);
void emit_pf(A64Buf *b, int res);
extern uint16_t g_lane_used;
void emit_materialize(A64Buf *b);
int fullpin_enabled(void);
void emit_pin_prologue(A64Buf *b);
extern const uint32_t *g_cur_insn_start;
int emit_arith(A64Buf *b, const X86Insn *insn, uint64_t need);
int emit_cmp_test_narrow(A64Buf *b, const X86Insn *insn, uint64_t need,
                                uint32_t **exit_sites, int *n_exits);
int emit_incdec(A64Buf *b, const X86Insn *insn, uint64_t need);
int emit_mov_logic_pair(A64Buf *b, const X86Insn *mov,
                               const X86Insn *logic, uint64_t logic_need,
                               uint32_t **logic_label);
int emit_add_inc_pair(A64Buf *b, const X86Insn *add,
                             const X86Insn *inc, uint64_t add_need,
                             uint64_t inc_need, uint32_t **inc_label);
int emit_shift(A64Buf *b, const X86Insn *insn, uint64_t need);
int emit_mul_wide(A64Buf *b, const X86Insn *insn, uint64_t need, int is_signed);
int emit_imul(A64Buf *b, const X86Insn *insn, uint64_t need);
int emit_mem_ea(A64Buf *b, const X86Insn *insn, const X86Operand *op, int addr_reg);
extern int g_n_garm;
int low_guard_fast_ok(void);
int lowstack_delta_ok(void);
int lowstack_disturbs(const X86Insn *in);
uint32_t *emit_commpage_guard(A64Buf *b, const X86Insn *insn,
                                     int addr_reg, uint32_t **exit_sites, int *n_exits);
void emit_reload_mem_base(A64Buf *b);
void emit_guest_store_ordered(A64Buf *b, int size, int rv, int ra, int scratch);
void emit_guest_load_ordered(A64Buf *b, int size, int rd, int ra, int scratch);
void emit_gpr_ld_at(A64Buf *b, int size, int rd, int ra, int32_t disp, int plain);
void emit_gpr_lds_at(A64Buf *b, int size, int sf, int rd, int ra, int32_t disp);
void emit_gpr_st_at(A64Buf *b, int size, int rv, int ra, int32_t disp, int plain);
extern int g_l0_nlanes;
extern int g_zero_vreg;
extern int g_blk_ymm_write;
extern int8_t g_undo_vreg[FPB_UNDO_MAX];
extern int g_n_undo_lanes;
extern int g_undo_want_slot;
extern int g_undo_want_size;
extern int g_undo_saved;
void emit_v_ld_at_(A64Buf *b, int size, int vd, int ra, int32_t disp, int plain);
void emit_v_st_at(A64Buf *b, int size, int vs, int ra, int32_t disp, int plain);
int lowstack_disp_ok(const X86Insn *insn, const X86Operand *m, int size, int unscaled_ok);
int emit_plain_mem_fast(A64Buf *b, const X86Insn *insn, const X86Operand *m,
                               int size, int reg, int store, int vec);
extern const X86Insn *g_cur_insns;
extern int g_cur_insns_n;
const struct X86Insn *g_cur_insns_fwd(void);
extern struct JitState_g_ea_cache g_ea_cache;
extern int g_fcmp_self_vreg;
extern int g_fcmp_self_idx;
int ea_cache_usable(const A64Buf *b);
int emit_mem_ea_plain_ex(A64Buf *b, const X86Insn *insn, const X86Operand *op,
                                int size, int *ra_out, uint32_t *disp_out, int unscaled_ok);
int emit_mem_load_plain(A64Buf *b, const X86Insn *insn, const X86Operand *op, int size, int rd);
int emit_mov_mem(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
int emit_movx(A64Buf *b, const X86Insn *insn, int is_signed,
                     uint32_t **exit_sites, int *n_exits);
int stack_inline_enabled(void);
int m32_lowreg_ok(void);
int emit_push_pop(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
int emit_movsxd(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
int emit_arith_mem(A64Buf *b, const X86Insn *insn, uint64_t need,
                          uint32_t **exit_sites, int *n_exits);
int emit_lea(A64Buf *b, const X86Insn *insn);
extern uint64_t g_cur_need;
int comis_fuse_producer(const X86Insn *insns, int ci);
int value_cond_fuse_producer(const X86Insn *insns, int ci);
int nzcv_fuse_producer(const X86Insn *insns, int ci);
extern int g_cc_direct;
extern int g_cc_want_cbz;
extern int g_cc_cbz_reg;
extern int g_cc_cbz_sf;
extern int g_cc_cbz_nz;
void emit_cc_predicate_ex(A64Buf *b, unsigned cc, int want_direct);
int emit_adc_sbb(A64Buf *b, const X86Insn *insn, uint64_t need);
int emit_arith_narrow(A64Buf *b, const X86Insn *insn, uint64_t need);
int emit_cbw_cwd(A64Buf *b, const X86Insn *insn);
extern int g_div_prev_skipped;
extern uint32_t g_oolslow_pre;
int rdx_prep_skippable(const X86Insn *insns, int i, int n, uint64_t need);
int emit_div(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
int emit_not_neg(A64Buf *b, const X86Insn *insn, uint64_t need);
int emit_rot(A64Buf *b, const X86Insn *insn, uint64_t need);
int emit_shift_cl(A64Buf *b, const X86Insn *insn, uint64_t need);
int emit_cmov(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
int emit_setcc(A64Buf *b, const X86Insn *insn);
int emit_bswap(A64Buf *b, const X86Insn *insn);
extern uint16_t g_xmm_pinned;
int xmm_pinning_enabled(void);
int xmm_global_enabled(void);
extern int g_sse_mem_ra;
extern uint32_t g_sse_mem_disp;
extern int g_sse_mem_plain;
extern int g_sse_mem_plainacc;
int emit_sse_mem_addr(A64Buf *b, const X86Insn *insn, const X86Operand *o, int size,
                             uint32_t **exit_sites, int *n_exits, uint32_t **skip_out);
int sse_enabled(void);
extern struct JitState_g_scpend g_scpend;
extern int g_scalar_merge_next;
void emit_nan_fix_scalar2(A64Buf *b, int dbl, int vr, int va, int vb);
extern int g_pk_consts_needed;
void emit_nan_fix_packed2(A64Buf *b, int dbl, int vr, int va, int vb, int t1, int t2);
void emit_nan_ool_arms(A64Buf *b, JitBlock *blk, const uint32_t *entry);
extern FpBatch g_fpb[FPB_MAX];
extern int g_n_fpb;
extern FpbSite g_fpb_sites[FPB_SITES_MAX];
extern int g_n_fpb_sites;
extern uint8_t g_fpb_member[JIT_MAX_BLOCK_INSNS];
extern uint8_t g_fpb_det[JIT_MAX_BLOCK_INSNS];
extern uint16_t g_fpb_sidechk[JIT_MAX_BLOCK_INSNS];
int mov_sink_gap_ok(const X86Insn *in, unsigned dreg, unsigned sreg);
void mov_sink_scan(const X86Insn *insns, int n, const uint64_t *fl_need);
extern int g_fpb_open;
extern const int8_t *g_fpb_of;
extern int g_fpb_fast;
extern struct JitOslowMap g_fpbmap[JIT_MAX_BLOCK_INSNS];
extern int g_n_fpbmap;
extern int g_fpb_v1_active;
void fpb_scan_v1(const X86Insn *insns, int n, int8_t *bat);
extern uint16_t g_fpb_stchk[JIT_MAX_BLOCK_INSNS];
extern uint8_t g_fpb_stlane[JIT_MAX_BLOCK_INSNS];
extern uint8_t g_fpb_undo[JIT_MAX_BLOCK_INSNS];
extern uint8_t g_fpb_undo_size[JIT_MAX_BLOCK_INSNS];
extern uint8_t g_fpb_undo_done[JIT_MAX_BLOCK_INSNS];
extern uint8_t g_fpb_undo_ld[JIT_MAX_BLOCK_INSNS];
extern uint8_t g_fpb_undo_ldsz[JIT_MAX_BLOCK_INSNS];
extern int16_t g_fpb_undo_ldst[JIT_MAX_BLOCK_INSNS];
extern int16_t g_fpb_undo_from[JIT_MAX_BLOCK_INSNS];
extern uint16_t g_fpb_exit_mask;
extern int g_fpb_exit_batch;
extern int g_fpb_exit_end;
extern int g_l0_fixed;
void fpb_scan_v2(const X86Insn *insns, int n, int8_t *bat);
int fpb_v1(void);
int unsafe_nocheckbr(void);
void fpb_emit_check(A64Buf *b, FpBatch *fb);
extern int8_t g_l0[16];
extern uint8_t g_l0_dbl[16];
extern uint16_t g_l0_owners[L0_NLANES];
extern unsigned g_l0_next;
void fpb_emit_regs_check(A64Buf *b, uint16_t regs, int batch, int end, const int8_t *l0, const uint8_t *l0_dbl);
int l0_enabled(void);
extern uint16_t g_l0_dirty;
extern int8_t g_yc[16];
extern uint16_t g_yc_dirty;
extern struct JitLaneRec g_lanerec[JIT_MAX_BLOCK_INSNS * 2];
extern int g_n_lanerec;
void lanerec_note(uint32_t off);
int l0_defer(void);
extern int g_l0_fixed;
extern int8_t g_l0_fixed_lane[16];
extern uint8_t g_l0_fixed_dbl[16];
extern uint16_t g_l0_fixed_dirty;
int l0_alloc2(A64Buf *b, unsigned r, int dbl);
void fpb_emit_store_check(A64Buf *b, int i, int batch);
int emit_mov128_pair(A64Buf *b, const X86Insn *a, const X86Insn *c, int i);
int vex_cmps_blendv_pair(const X86Insn *c, const X86Insn *v);
int l0_fixed_setup(A64Buf *b, const X86Insn *insns, int n);
int yc_setup(A64Buf *b, const X86Insn *insns, int n);
void l0_fixed_restore(A64Buf *b);
extern int g_cmps_mask_idx;
int cmps_blendv_fusable(int cmps_idx);
int vex_lane_aware(const X86Insn *insn);
void l0_pre_insn(A64Buf *b, const X86Insn *insn);
int emit_crc32(A64Buf *b, const X86Insn *insn);
int emit_mmx(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
int emit_sse(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
int emit_bitscan(A64Buf *b, const X86Insn *insn, uint64_t need);
int emit_bt(A64Buf *b, const X86Insn *insn, uint64_t need, uint32_t **exit_sites, int *n_exits);
int emit_push_pop_mem(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
int emit_leave(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
int emit_shiftd(A64Buf *b, const X86Insn *insn, uint64_t need);
int emit_pmovmskb(A64Buf *b, const X86Insn *insn);
extern uint16_t g_ymmh_zero;
void emit_ymmh_clear(A64Buf *b, unsigned xr);
int emit_vex(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
int emit_rmw_mem(A64Buf *b, const X86Insn *insn, uint64_t need,
                        uint32_t **exit_sites, int *n_exits);
int emit_xchg_reg32(A64Buf *b, const X86Insn *insn);
int emit_cmpxchg8b(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
extern int g_n_x87_run;
extern int g_x87_cur;
extern int g_n_x87_site;
extern int g_n_x87_frag;
extern int g_x87_frag_open;
extern int g_slow_run_last;
extern int g_x87_nzcv;
extern int g_x87_live;
extern int g_x87_delta;
extern int g_xlat_ftop;
extern int g_x87_btop;
extern int g_x87_spec;
extern int8_t g_x87_lane[8];
extern int g_x87_lanes_on;
extern uint8_t g_x87_lv;
int x87_run_flags(const X86Insn *in);
extern int g_x87_kcarry;
extern int g_x87_spec_cut;
int emit_x87(A64Buf *b, const X86Insn *insn, uint64_t need, uint32_t **exit_sites, int *n_exits);
int m32_inline_ok(const X86Insn *insn);
extern JitBlock *g_tag_blk;
extern int g_tag_idx;
int can_fuse_cmp_test_jcc(const X86Insn *producer,
                                 const X86Insn *jcc, uint64_t block_rip);
int side_gap_fuse_ok(const X86Insn *insns, int i, int n);
int side_fuse_ok(const X86Insn *insns, int i, int n);
int emit_cmp_test_jcc(A64Buf *b, const X86Insn *producer,
                             const X86Insn *jcc,
                             uint32_t **epilogue_sites, int *n_epi,
                             uint32_t **jcc_label,
                             uint32_t **exit_sites, int *n_exits,
                             const X86Insn *gap, uint32_t **gap_label);
int emit_incdec_jcc(A64Buf *b, const X86Insn *producer,
                           const X86Insn *jcc, uint32_t **epilogue_sites,
                           int *n_epi, uint32_t **jcc_label);
int emit_arith_incdec_jcc(A64Buf *b, const X86Insn *arith,
                                 const X86Insn *incdec,
                                 const X86Insn *jcc, uint64_t arith_need,
                                 uint32_t **epilogue_sites, int *n_epi,
                                 uint32_t **incdec_label,
                                 uint32_t **jcc_label);
int emit_logic_jmp_incdec_jcc(A64Buf *b, const X86Insn *logic,
                                     const X86Insn *jmp,
                                     uint64_t logic_need,
                                     uint32_t **epilogue_sites, int *n_epi,
                                     uint32_t **jmp_label);
int emit_ifconv_diamond(A64Buf *b, const X86Insn *test,
                               const X86Insn *jcc,
                               uint32_t **epilogue_sites, int *n_epi,
                               uint32_t **jcc_label);
int emit_jcc(A64Buf *b, const X86Insn *insn, uint32_t **epilogue_sites, int *n_epi);
int emit_jmp(A64Buf *b, const X86Insn *insn, uint32_t **epilogue_sites, int *n_epi);
void ocerz_ras_push(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t retaddr);
extern uint64_t g_fps_frames;
int fps_watch(uint64_t rip);
int emit_call_ret(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites,
                         int *n_exits, uint32_t **epi_sites, int *n_epi);
void emit_dispatch_stub(OcerzJit *jit, int mode32);
int emit_indirect_jmp(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites,
                             int *n_exits, uint32_t **epi_sites, int *n_epi);
uint32_t *emit_leaf_call_ret(A64Buf *b, const void *leaf, int writes, uint32_t **epi_sites,
                                    int *n_epi);
int leaf_layout_ok(void);
void emit_bridge_fastcall(A64Buf *b, const X86Insn *insns, int i,
                                 uint32_t **epi_sites, int *n_epi);
int emit_indirect_call(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites,
                              int *n_exits, uint32_t **epi_sites, int *n_epi);
void emit_slowcall(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
extern int ocerz_jitstat;
extern _Atomic unsigned long long js_xlat_ok;
extern _Atomic unsigned long long js_xlat_fail;
extern _Atomic unsigned long long js_fail_decode0;
extern _Atomic unsigned long long js_fail_overflow;
extern _Atomic unsigned long long js_fail_alloc;
extern _Atomic unsigned long long js_decoded_insns;
void js_note_fail(uint64_t rip, unsigned reason, int nins);
void veneer_pool_check(OcerzJit *jit);
void pending_add(uint64_t target_key, uint32_t *patch_b, uint8_t kind,
                        uint8_t pin_class, uint32_t *cond_site, uint64_t src_sig,
                        JitBlock *src, int edge);
extern void **g_ras_slots;
extern unsigned g_ras_slot_n;
void **ras_slot_alloc(void);
void pending_add_ras(uint64_t target_key, void **ras_slot);
uint32_t *emit_body_chain_tail(A64Buf *b, uint64_t target_rip, int poll,
                                      uint32_t **epilogue_sites, int *n_epi);
uint32_t *emit_chain_tail(A64Buf *b, int poll);
int insn_may_write_gpr(const X86Insn *in, unsigned reg);
extern MarkSet g_lowhoist_marks;
extern MarkSet g_x87spec_marks;
int select_low_hoist(const X86Insn *insns, int n, uint64_t rip);
void emit_low_hoist_check(A64Buf *b);
void emit_low_hoist_bail(A64Buf *b, uint64_t rip, uint32_t **epi_sites, int *n_epi);
int build_fault_flag_recipes(const X86Insn *insns, int n,
                                    JitFaultFlagRecipe *recipes);
int code_index_append_locked(OcerzJit *jit, JitBlock *block);
extern int g_n_oolslow;
int oolslow_add(const X86Insn *insn, uint32_t **sites, int nsites, uint32_t *back);
void emit_oolslow_arms(A64Buf *b, uint32_t **exit_sites, int *n_exits);
void emit_x87_arms(A64Buf *b, uint32_t **exit_sites, int *n_exits, uint32_t **epi_sites, int *n_epi);
void emit_guard_arms(A64Buf *b, const uint32_t *entry);
void emit_ordered_slow_arms(A64Buf *b, JitBlock *blk, const uint32_t *entry);
void compact_block(JitBlock *blk);
extern uint32_t g_tc_pool_off;
extern int g_tc_log;
extern FILE *g_tc_lf;
int tc_bind(OcerzJit *jit, JitBlock *blk, uint32_t *code, const TcReloc *rel, int nrel, int fresh);
void tc_summary(void);
uint32_t *tc_roundtrip(OcerzJit *jit, JitBlock *blk, uint32_t *entry);
void blk_chain_install(OcerzJit *jit, JitBlock *blk);
extern uint64_t g_tc_key;
void tc_put(OcerzJit *jit, JitBlock *blk);
JitBlock *tc_load(OcerzJit *jit, const OcerzTcRecHead *h, uint64_t rip, int mode32);
void tc_verify(OcerzJit *jit, JitBlock *blk, const OcerzTcRecHead *h);
JitBlock *translate(OcerzJit *jit, uint64_t rip, int mode32);
extern uint64_t g_flip_n_retire;
extern __thread sigjmp_buf *ocerz_jit_decode_recover;
extern uint64_t ocerz_jit_retire_count;
void churn_note_refusal(uint64_t rip);
int churn_blacklisted(uint64_t rip);
uint32_t *branch_word_target(uint32_t *site, uint32_t w);
void psc_retire_cols(struct OcerzVM *vm, uint32_t cols);
int jit_interp_block(struct OcerzVM *vm, OcerzCPU *cpu, JitBlock *b);
void ps_report(OcerzJit *jit);
void chain_edge_now(OcerzJit *jit, JitBlock *blk, int e);
extern uint64_t g_flip_n_retire;
void flip_retire_locked(struct OcerzVM *vm, OcerzJit *jit, JitBlock *blk);
void flip_side_hit(struct OcerzVM *vm, OcerzJit *jit, OcerzCPU *cpu);

static inline uint64_t jit_key(uint64_t rip, int mode32);
static inline uint64_t jit_key_rip(uint64_t key);
static inline int jit_key_mode32(uint64_t key);
static inline uint64_t blk_rip(const JitBlock *b);
static inline int blk_mode32(const JitBlock *b);
static inline const X86Insn *blk_insn_full(const JitBlock *b, int i);
static inline void tc_note(const uint32_t *at, int kind, int form, uint64_t arg);
static inline void tc_imm64(A64Buf *b, int rd, int kind, uint64_t arg, uint64_t value);
static inline int tc_noload_has(uint64_t key);
static inline void ras_cell_register(void **cell);
static inline int flip_find(uint64_t rip, int insert);
static inline int flip_state(uint64_t rip);
static inline int cp_marked(uint64_t key);
static inline void cp_mark(uint64_t key);
static inline int mem_guard_needed(void);
static inline void jl_release(void);
static inline int al_marked(uint64_t key);
static inline void al_mark(uint64_t key);
static inline int stack_plain_access_ok(void);
static inline int mem_plain_access_ok(const X86Operand *m);
static inline int mem_fast_forms_ok(void);
static inline int stack_guard_needed(void);
static inline int stack_plain_now(void);
static inline uint64_t hoist_signature(void);
static inline int jgb_usable(void);
static inline void emit_reload_jgb(A64Buf *b);
static inline int g_xlat_mode32_fwd(void);
static inline int rsp_is_ptr(void);
static inline uint64_t *ps_retsite_counter(uint64_t rip);
static inline unsigned hash_key(uint64_t key);
static inline int call_body_successor(uint64_t rip);
static inline uint64_t xlive_succ_live_d(OcerzJit *jit, uint64_t rip, int depth);
static inline uint64_t xlive_succ_live(OcerzJit *jit, uint64_t rip);
static inline int probe_wanted(uint64_t jcc_rip, uint64_t ft_rip);
static inline void emit_frame_sp_reset(A64Buf *b);
static inline int stack_fast(void);
static inline void emit_zf_sf(A64Buf *b, uint64_t m);
static inline void emit_commit_flags(A64Buf *b, uint64_t clear_mask);
static inline void emit_defer_flags(A64Buf *b, uint32_t ccop, int src_reg, int dst_reg);
static inline int pin_slot(unsigned greg);
static inline int pin_hreg(int slot);
static inline int body_edge_pin_class(void);
static inline void emit_gpr_rd(A64Buf *b, int sf, int dst, unsigned greg);
static inline void emit_gpr_wr(A64Buf *b, int src, unsigned greg);
static inline void emit_spill_pinned(A64Buf *b);
static inline void emit_spill_pinned_callersaved(A64Buf *b);
static inline void emit_fill_pinned_callersaved(A64Buf *b);
static inline void emit_fill_pinned(A64Buf *b);
static inline int pin_saved_count(void);
static inline void emit_pin_epilogue_restore(A64Buf *b);
static inline int mem_native_store_ok(void);
static inline uint64_t ea_fold(void);
static inline void emit_add_const(A64Buf *b, int reg, uint64_t c);
static inline void emit_stack_delta_into(A64Buf *b, int rd);
static inline void emit_stack_delta(A64Buf *b);
static inline void emit_stack_delta_check(A64Buf *b);
static inline void patch_guard_skip(uint32_t *skip, uint32_t *target);
static inline void undo_save_hook(A64Buf *b, int size, int vd);
static inline void emit_v_ld_at(A64Buf *b, int size, int vd, int ra, int32_t disp, int plain);
static inline int lowstack_disp_ea(A64Buf *b, const X86Insn *insn, const X86Operand *m, int size, int unscaled_ok);
static inline void ea_cache_reset(void);
static inline int a64_word_may_write_reg(uint32_t w, unsigned r);
static inline int ea_cache_has_base(const A64Buf *b, const X86Operand *op);
static inline void ea_cache_set_full(const A64Buf *b, unsigned base, unsigned index, int scale);
static inline void ea_cache_step(const X86Insn *in, const X86Insn *prev);
static inline int emit_mem_ea_plain(A64Buf *b, const X86Insn *insn, const X86Operand *op,
                             int size, int *ra_out, uint32_t *disp_out);
static inline int m32_stack_low(void);
static inline int m32_stack_base_ok(void);
static inline void m32_stack_st(A64Buf *b, int rv, int wa);
static inline void m32_stack_ld(A64Buf *b, int rd, int wa);
static inline int m32_stack_ok(const X86Insn *insn);
static inline void emit_cc_predicate(A64Buf *b, unsigned cc);
static inline int xmm_vreg(unsigned xr);
static inline int xmm_is_pinned(unsigned xr);
static inline void emit_xmm_pin_load_all(A64Buf *b);
static inline void emit_xmm_pin_spill_all(A64Buf *b);
static inline void emit_sse_mem_ld_gpr(A64Buf *b, int size, int rd);
static inline void emit_sse_mem_ld(A64Buf *b, int size, int vd);
static inline void emit_sse_mem_st(A64Buf *b, int size, int vs);
static inline void emit_sse_mem_st_gpr(A64Buf *b, int size, int rv);
static inline int emit_mem_load_any(A64Buf *b, const X86Insn *insn, const X86Operand *op, int size, int rd);
static inline int scalar_cvt_follows(unsigned xreg, int dbl);
static inline void scalar_pend_flush(A64Buf *b);
static inline void emit_pk_consts_load(A64Buf *b);
static inline void fpb_undo_clear(int i);
static inline int lane_reserve(void);
static inline void fpb_scan(const X86Insn *insns, int n, int8_t *bat);
static inline void fpb_replay_prelude(A64Buf *b, const FpBatch *fb, const int8_t *l0, const uint8_t *l0_dbl);
static inline void fpb_site_emit(A64Buf *b, int end, int va, int vb, int dbl);
static inline int fpb_det_here(int idx);
static inline void yc_flush_all(A64Buf *b);
static inline void yc_flush_from(A64Buf *b, uint16_t dirty);
static inline void yc_reload_all(A64Buf *b);
static inline void l0_reset(void);
static inline void l0_flush_reg(A64Buf *b, unsigned r);
static inline void l0_flush_all(A64Buf *b);
static inline int l0_defer_take(int vs, unsigned xr, int size);
static inline void l0_inval(unsigned r);
static inline void l0_fixed_map(void);
static inline int l0_src2(A64Buf *b, unsigned r, int dbl);
static inline void fpb_emit_undo_save(A64Buf *b, const X86Insn *insn, int i, uint32_t **exit_sites, int *n_exits);
static inline void fpb_emit_undo_restore(A64Buf *b, const X86Insn *insns, int first, int end, uint32_t **exit_sites, int *n_exits);
static inline void fpb_emit_exit_check(A64Buf *b);
static inline void l0_share(unsigned dst, unsigned src);
static inline void l0_fixed_backedge(A64Buf *b);
static inline void l0_fixed_fallthrough(A64Buf *b);
static inline void x87_reset(void);
static inline int x87_inline_ok(const X86Insn *insn);
static inline void emit_prof_count(A64Buf *b, JitProf *pf, uint32_t off);
static inline void emit_side_tag(A64Buf *b, int cpu_reg);
static inline int side_stub_has_work(int k);
static inline int can_fuse_incdec_jcc(const X86Insn *producer, const X86Insn *jcc);
static inline int ras_body_only(void);
static inline void *ras_entry_for(const JitBlock *blk);
static inline uint32_t *emit_static_chain_tail(A64Buf *b, uint64_t target_rip,
                                        int poll, int body_edge,
                                        uint32_t **epilogue_sites, int *n_epi);
static inline int mark_has(MarkSet *m, uint64_t key);
static inline void mark_add(MarkSet *m, uint64_t key);
static inline void lowhoist_mark(uint64_t key);
static inline void emit_l0_flush_from(A64Buf *b, const int8_t *l0, const uint8_t *l0_dbl, uint16_t dirty);
static inline void emit_l0_reload_from(A64Buf *b, const int8_t *l0, const uint8_t *l0_dbl);
static inline void emit_slowcall_keep_lanes(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static inline void patch_any_branch(uint32_t *site, uint32_t *target);
static inline void emit_misaligned_pieces_st(A64Buf *b, int psize, int n, int rv, int ra, int32_t disp, int s1);
static inline void tc_log_init(void);
static inline int tc_usable(const OcerzJit *jit);
static inline int tc_keepable(uint64_t rip);
static inline uint64_t tc_key(uint64_t rip, int mode32);
static inline unsigned psc_col(uint64_t key);
static inline void steplog(const OcerzCPU *cpu);

static inline uint64_t jit_key(uint64_t rip, int mode32)
{
    return mode32 ? (rip | JIT_KEY_M32) : rip;
}

static inline uint64_t jit_key_rip(uint64_t key) { return key & ~JIT_KEY_M32; }

static inline int jit_key_mode32(uint64_t key) { return (int)(key >> 63); }

static inline uint64_t blk_rip(const JitBlock *b) { return jit_key_rip(b->key); }

static inline int blk_mode32(const JitBlock *b) { return jit_key_mode32(b->key); }

static inline const X86Insn *blk_insn_full(const JitBlock *b, int i)
{
    if (b->insns) return &b->insns[i];
    unsigned k = b->iref[i].keep;
    return k ? &b->kept[k - 1] : NULL;
}

static inline void tc_note(const uint32_t *at, int kind, int form, uint64_t arg)
{
    if (!g_tc_on || !g_tc_entry)
        return;
    if (g_tc_nrel >= TC_RELOC_MAX) { g_tc_bad = 1; return; }
    g_tc_rel[g_tc_nrel].off = (uint32_t)(at - g_tc_entry);
    g_tc_rel[g_tc_nrel].kind = (uint8_t)kind;
    g_tc_rel[g_tc_nrel].form = (uint8_t)form;
    g_tc_rel[g_tc_nrel].arg = arg;
    g_tc_nrel++;
}

static inline void tc_imm64(A64Buf *b, int rd, int kind, uint64_t arg, uint64_t value)
{
    if (!g_tc_on) {
        a64_mov_imm64(b, rd, value);
        return;
    }
    tc_note(b->p, kind, 0, arg);
    a64_movz(b, rd, (uint16_t)value, 0);
    a64_movk(b, rd, (uint16_t)(value >> 16), 1);
    a64_movk(b, rd, (uint16_t)(value >> 32), 2);
    a64_movk(b, rd, (uint16_t)(value >> 48), 3);
}

static inline int tc_noload_has(uint64_t key)
{
    if (!g_tc_noload_n)
        return 0;
    size_t i = (size_t)((key * 0x9e3779b97f4a7c15ull) >> 20) & (g_tc_noload_cap - 1);
    for (; g_tc_noload[i]; i = (i + 1) & (g_tc_noload_cap - 1))
        if (g_tc_noload[i] == key)
            return 1;
    return 0;
}

static inline void ras_cell_register(void **cell)
{
    if (g_n_ras_cells == g_cap_ras_cells) {
        size_t ncap = g_cap_ras_cells ? g_cap_ras_cells * 2 : 1024;
        void ***nv = (void ***)realloc(g_ras_cells, ncap * sizeof *nv);
        if (!nv) return;
        g_ras_cells = nv; g_cap_ras_cells = ncap;
    }
    g_ras_cells[g_n_ras_cells++] = cell;
}

static inline int flip_find(uint64_t rip, int insert)
{
    unsigned h = (unsigned)((rip * 0x9E3779B97F4A7C15ull) >> 52) & (FLIP_N - 1);
    for (unsigned n = 0; n < 64; n++, h = (h + 1) & (FLIP_N - 1)) {
        if (g_flip[h].rip == rip) return (int)h;
        if (g_flip[h].rip == 0) {
            if (!insert) return -1;
            g_flip[h].rip = rip; g_flip[h].state = FLIP_NONE;
            return (int)h;
        }
    }
    return -1;
}

static inline int flip_state(uint64_t rip)
{
    int i = flip_find(rip, 0);
    return i < 0 ? FLIP_NONE : g_flip[i].state;
}

static inline int cp_marked(uint64_t key)
{
    if (!key) return 0;
    unsigned i = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 46) & (CP_MARK_SIZE - 1);
    for (unsigned n = 0; n < CP_MARK_SIZE; n++, i = (i + 1) & (CP_MARK_SIZE - 1)) {
        uint64_t v = g_cp_marks[i];
        if (v == key) return 1;
        if (v == 0) return 0;
    }
    return 0;
}

static inline void cp_mark(uint64_t key)
{
    if (!key) return;
    unsigned i = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 46) & (CP_MARK_SIZE - 1);
    for (unsigned n = 0; n < CP_MARK_SIZE; n++, i = (i + 1) & (CP_MARK_SIZE - 1)) {
        uint64_t v = g_cp_marks[i];
        if (v == key) return;
        if (v == 0) { g_cp_marks[i] = key; return; }
    }
}

static inline int mem_guard_needed(void) { return ocerz_low_base != 0 || g_cp_guard; }

static inline void jl_release(void)
{
    if (g_jl_log > 0) {
        __atomic_store_n(&g_jl_phase, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_jl_owner, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_jl_since, 0, __ATOMIC_RELAXED);
    }
    jl_held--;
    pthread_mutex_unlock(&jit_lock);
    ocerz_critical_depth--;
}

static inline int al_marked(uint64_t key)
{
    if (g_al_all) return 1;
    if (!key) return 0;
    unsigned i = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 46) & (AL_MARK_SIZE - 1);
    for (unsigned n = 0; n < AL_MARK_SIZE; n++, i = (i + 1) & (AL_MARK_SIZE - 1)) {
        uint64_t v = g_al_marks[i];
        if (v == key) return 1;
        if (v == 0) return 0;
    }
    return 0;
}

static inline void al_mark(uint64_t key)
{
    if (g_al_all || !key) return;
    unsigned i = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 46) & (AL_MARK_SIZE - 1);
    for (unsigned n = 0; n < AL_MARK_SIZE; n++, i = (i + 1) & (AL_MARK_SIZE - 1)) {
        uint64_t v = g_al_marks[i];
        if (v == key) return;
        if (v == 0) { g_al_marks[i] = key; g_al_n++; return; }
    }
    g_al_all = 1;
}

static inline int stack_plain_access_ok(void) { return g_plain_mem || stack_plain_ok(); }

static inline int mem_plain_access_ok(const X86Operand *m)
{
    if (g_plain_mem) return 1;
    return stack_plain_ok() && m->base == OCERZ_RSP && !m->riprel;
}

static inline int mem_fast_forms_ok(void) { return jgb_usable() && !mem_guard_needed(); }

static inline int stack_guard_needed(void) { return ocerz_low_base != 0; }

static inline int stack_plain_now(void) { return ocerz_low_base != 0 && stack_plain_ok(); }

static inline uint64_t hoist_signature(void)
{
    if (g_mem_hoist_greg < 0) return 0;
    return 1ull | ((uint64_t)(g_mem_hoist_greg & 0xff) << 8) | ((uint64_t)((g_mem_hoist_greg2 + 1) & 0xff) << 16) |
           ((uint64_t)((g_mem_hoist_greg3 + 1) & 0xff) << 24) | ((uint64_t)((g_mem_hoist_aux_index + 1) & 0xff) << 32) |
           ((uint64_t)(g_mem_hoist_aux_scale & 3) << 40) | ((uint64_t)((uint32_t)(g_mem_hoist_aux_disp + 0x8000) & 0xffff) << 44);
}

static inline int jgb_usable(void) { return ocerz_low_base == 0; }

static inline void emit_reload_jgb(A64Buf *b)
{
    if (jgb_usable())
        a64_mov_imm64(b, JGB, ocerz_guest_base);
    else if (g_lowstack)
        emit_stack_delta(b);
    else if (g_m32low)
        a64_mov_imm64(b, JGB, ocerz_low_base);
}

static inline int g_xlat_mode32_fwd(void) { return g_xlat_mode32; }

static inline int rsp_is_ptr(void)
{
    return g_pin_class == 2 ||
           (g_pin_class == 3 && !g_xlat_mode32_fwd() && rsp_ptr3());
}

static inline uint64_t *ps_retsite_counter(uint64_t rip)
{
    unsigned i = (unsigned)((rip * 0x9E3779B97F4A7C15ull) >> 48) & (PS_RETSITE_N - 1);
    for (unsigned k = 0; k < 64; k++, i = (i + 1) & (PS_RETSITE_N - 1)) {
        if (ps_retsite[i].rip == rip) return &ps_retsite[i].n;
        if (!ps_retsite[i].rip) { ps_retsite[i].rip = rip; return &ps_retsite[i].n; }
    }
    return &ps_retsite[0].n;
}

static inline unsigned hash_key(uint64_t key)
{
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdull;
    key ^= key >> 29;
    return (unsigned)(key & JIT_HASH_MASK);
}

static inline int call_body_successor(uint64_t rip)
{
    unsigned term = decoded_terminator(rip);
    return term == OCERZ_OP_CALL || term == OCERZ_OP_RET;
}

static inline uint64_t xlive_succ_live_d(OcerzJit *jit, uint64_t rip, int depth)
{
    JitBlock *t = jit ? cache_lookup(jit, rip, g_xlat_mode32) : NULL;
    if (g_xlive_log < 0) g_xlive_log = getenv("OCERZ_XLIVELOG") ? 1 : 0;
    if (t && t->code && g_xlive_log) fprintf(stderr, "ocerz: XLIVE cached rip=%#llx entry_live=%#x n_insns=%d\n", (unsigned long long)rip, t->entry_live, t->n_insns);
    return (t && t->code && !g_tc_rec) ? (uint64_t)t->entry_live : xlive_decode_entry_d(rip, depth);
}

static inline uint64_t xlive_succ_live(OcerzJit *jit, uint64_t rip) { return xlive_succ_live_d(jit, rip, 0); }

static inline int probe_wanted(uint64_t jcc_rip, uint64_t ft_rip)
{
    if (flip_disabled()) return 0;
    if (g_n_probes >= PROBE_MAX || flip_state(jcc_rip) != FLIP_NONE) {
        g_tc_learned = 1;
        return 0;
    }
    if (xlive_succ_live(g_xlat_jit, ft_rip) != 0) return 0;
    g_n_probes++;
    return 1;
}

static inline void emit_frame_sp_reset(A64Buf *b)
{
    if (g_pin_class == 3 && host_ras_enabled()) {
        a64_ldr(b, 8, 15, 20, JIT_FP_OFF);
        a64_add_imm(b, 1, 31, 15, 0);
    }
}

static inline int stack_fast(void)
{
    return (jgb_usable() && !stack_guard_needed()) || low_stack_fast();
}

static inline void emit_zf_sf(A64Buf *b, uint64_t m)
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

static inline void emit_commit_flags(A64Buf *b, uint64_t clear_mask)
{
    a64_ldr(b, 8, JTT, 20, RF_OFF);
    a64_mov_imm64(b, JTU, ~clear_mask);
    a64_and_reg(b, 1, JTT, JTT, JTU, 0);
    a64_orr_reg(b, 1, JTT, JTT, JTF, 0);
    a64_str(b, 8, JTT, 20, RF_OFF);
}

static inline void emit_defer_flags(A64Buf *b, uint32_t ccop, int src_reg, int dst_reg)
{
    _Static_assert(CC_DST_OFF == CC_SRC_OFF + 8,
                   "deferred flag operands must remain adjacent");
    a64_stp_off(b, src_reg, dst_reg, 20, CC_SRC_OFF);
    a64_mov_imm64(b, JTT, ccop);
    a64_str(b, 4, JTT, 20, CC_OP_OFF);
}

static inline int pin_slot(unsigned greg)
{
    return (g_pin && greg < 16) ? g_pin[greg] : -1;
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

static inline void emit_gpr_rd(A64Buf *b, int sf, int dst, unsigned greg)
{
    int s = pin_slot(greg);
    if (s >= 0 && rsp_is_ptr() && greg == OCERZ_RSP) {
        if (jgb_usable())
            a64_sub_reg(b, 1, dst, pin_hreg(s), JGB, 0);
        else if (ocerz_guest_base == 0)
            a64_mov_reg(b, 1, dst, pin_hreg(s));
        else {
            a64_mov_imm64(b, dst, ocerz_guest_base);
            a64_sub_reg(b, 1, dst, pin_hreg(s), dst, 0);
        }
        if (!sf)
            a64_mov_reg(b, 0, dst, dst);
    } else if (s >= 0)
        a64_mov_reg(b, sf, dst, pin_hreg(s));
    else
        a64_ldr(b, sf ? 8 : 4, dst, 20, GPR_OFF(greg));
}

static inline void emit_gpr_wr(A64Buf *b, int src, unsigned greg)
{
    int s = pin_slot(greg);
    if (s >= 0 && rsp_is_ptr() && greg == OCERZ_RSP) {
        if (jgb_usable())
            a64_add_reg(b, 1, pin_hreg(s), src, JGB, 0);
        else if (ocerz_guest_base == 0)
            a64_mov_reg(b, 1, pin_hreg(s), src);
        else {
            int tmp = src == JTU ? JTA : JTU;
            a64_mov_imm64(b, tmp, ocerz_guest_base);
            a64_add_reg(b, 1, pin_hreg(s), src, tmp, 0);
        }
    } else if (s >= 0)
        a64_mov_reg(b, 1, pin_hreg(s), src);
    else
        a64_str(b, 8, src, 20, GPR_OFF(greg));
}

static inline void emit_spill_pinned(A64Buf *b)
{
    for (int i = 0; i < g_n_pinned; i++) {
        if (rsp_is_ptr() && g_pin_hold[i] == OCERZ_RSP) {
            a64_mov_imm64(b, JTA, ocerz_guest_base);
            a64_sub_reg(b, 1, JTA, pin_hreg(i), JTA, 0);
            a64_str(b, 8, JTA, 20, GPR_OFF(OCERZ_RSP));
        } else {
            a64_str(b, 8, pin_hreg(i), 20, GPR_OFF(g_pin_hold[i]));
        }
    }
}

static inline void emit_spill_pinned_callersaved(A64Buf *b)
{
    for (int i = 8; i < g_n_pinned; i++)
        a64_str(b, 8, pin_hreg(i), 20, GPR_OFF(g_pin_hold[i]));
}

static inline void emit_fill_pinned_callersaved(A64Buf *b)
{
    for (int i = 8; i < g_n_pinned; i++)
        a64_ldr(b, 8, pin_hreg(i), 20, GPR_OFF(g_pin_hold[i]));
}

static inline void emit_fill_pinned(A64Buf *b)
{
    for (int i = 0; i < g_n_pinned; i++)
        a64_ldr(b, 8, pin_hreg(i), 20, GPR_OFF(g_pin_hold[i]));
    if (rsp_is_ptr()) {
        int s = pin_slot(OCERZ_RSP);
        assert(s >= 0);
        a64_mov_imm64(b, JTA, ocerz_guest_base);
        a64_add_reg(b, 1, pin_hreg(s), pin_hreg(s), JTA, 0);
    }
}

static inline int pin_saved_count(void) { return g_n_pinned < 8 ? g_n_pinned : 8; }

static inline void emit_pin_epilogue_restore(A64Buf *b)
{
    if (g_pin_class == 2)
        a64_ldp_post(b, JRET_GUEST, JRET_HOST, 31, 16);
    int ns = pin_saved_count();
    int last = (ns & 1) ? ns - 1 : ns - 2;
    for (int i = last; i >= 0; i -= 2)
        a64_ldp_post(b, 21 + i, 21 + i + 1, 31, 16);
    for (int d = 14; d >= 8; d -= 2)
        a64_ldp_d_post(b, d, d + 1, 31, 16);
}

static inline int mem_native_store_ok(void)
{
    return ocerz_watch_addr == 0 && ocerz_watch_val == 0;
}

static inline uint64_t ea_fold(void)
{
    return ocerz_low_base ? 0 : ocerz_guest_base;
}

static inline void emit_add_const(A64Buf *b, int reg, uint64_t c)
{
    if (c) {
        a64_mov_imm64(b, JTU, c);
        a64_add_reg(b, 1, reg, reg, JTU, 0);
    }
}

static inline void emit_stack_delta_into(A64Buf *b, int rd)
{
    int hs = pin_hreg(pin_slot(OCERZ_RSP));
    a64_lsr_imm(b, 1, rd, hs, 32);
    a64_sub_imm(b, 1, rd, rd, (uint32_t)(OCERZ_LOW_LIMIT >> 32));
    a64_asr_imm(b, 1, rd, rd, 63);
    (void)a64_try_and_imm(b, 1, rd, rd, ocerz_low_base);
}

static inline void emit_stack_delta(A64Buf *b)
{
    emit_stack_delta_into(b, JGB);
}

static inline void emit_stack_delta_check(A64Buf *b)
{
    emit_stack_delta_into(b, JTT);
    a64_eor_reg(b, 1, JTT, JTT, JGB, 0);
    uint32_t *ok = a64_label(b);
    a64_cbz(b, 1, JTT, 0);
    a64_emit32(b, 0xd4200000u | (0x5d0u << 5));
    a64_patch_cbz(ok, a64_label(b));
}

static inline void patch_guard_skip(uint32_t *skip, uint32_t *target)
{
    if (skip)
        a64_patch_b(skip, target);
}

static inline void undo_save_hook(A64Buf *b, int size, int vd)
{
    if (g_undo_want_slot < 0 || size != g_undo_want_size) return;
    a64_v_mov(b, g_undo_vreg[g_undo_want_slot], vd);
    g_undo_want_slot = -1;
    g_undo_saved = 1;
}

static inline void emit_v_ld_at(A64Buf *b, int size, int vd, int ra, int32_t disp, int plain)
{
    emit_v_ld_at_(b, size, vd, ra, disp, plain);
    undo_save_hook(b, size, vd);
}

static inline int lowstack_disp_ea(A64Buf *b, const X86Insn *insn, const X86Operand *m, int size, int unscaled_ok)
{
    if (!lowstack_disp_ok(insn, m, size, unscaled_ok)) return 0;
    if (!ea_cache_has_base(b, m)) a64_add_reg(b, 1, JTA, pin_hreg(pin_slot(OCERZ_RSP)), JGB, 0);
    ea_cache_set_full(b, OCERZ_RSP, OCERZ_REG_NONE, 0);
    return 1;
}

static inline void ea_cache_reset(void) { g_ea_cache.valid = 0; }

static inline int a64_word_may_write_reg(uint32_t w, unsigned r)
{
    if ((w & 0x1f) == r) return 1;
    if ((w & 0x3a000000u) == 0x28000000u && (w & 0x00400000u)) { if (((w >> 10) & 0x1f) == r) return 1; }
    if ((w & 0x3f000000u) == 0x08000000u && ((w >> 10) & 0x1f) == r) return 1;
    return 0;
}

static inline int ea_cache_has_base(const A64Buf *b, const X86Operand *op)
{
    if (!ea_cache_usable(b)) return 0;
    return g_ea_cache.base == op->base && op->base != OCERZ_REG_NONE && g_ea_cache.index == OCERZ_REG_NONE;
}

static inline void ea_cache_set_full(const A64Buf *b, unsigned base, unsigned index, int scale)
{
    g_ea_cache.valid = 1; g_ea_cache.base = base; g_ea_cache.index = index;
    g_ea_cache.scale = scale & 3; g_ea_cache.seq = g_callout_seq; g_ea_cache.after = b->p;
}

static inline void ea_cache_step(const X86Insn *in, const X86Insn *prev)
{
    if (!g_ea_cache.valid) return;
    if (g_ea_cache.base != OCERZ_REG_NONE &&
        (insn_may_write_gpr(in, g_ea_cache.base) || (prev && insn_may_write_gpr(prev, g_ea_cache.base)))) { g_ea_cache.valid = 0; return; }
    if (g_ea_cache.index != OCERZ_REG_NONE &&
        (insn_may_write_gpr(in, g_ea_cache.index) || (prev && insn_may_write_gpr(prev, g_ea_cache.index)))) { g_ea_cache.valid = 0; return; }
}

static inline int emit_mem_ea_plain(A64Buf *b, const X86Insn *insn, const X86Operand *op,
                             int size, int *ra_out, uint32_t *disp_out)
{
    return emit_mem_ea_plain_ex(b, insn, op, size, ra_out, disp_out, 0);
}

static inline int m32_stack_low(void)
{
    return ocerz_low_base != 0 && low_guard_fast_ok();
}

static inline int m32_stack_base_ok(void)
{
    if (!stack_inline_enabled() || g_pin_class == 2 || pin_slot(OCERZ_RSP) < 0 || !stack_plain_access_ok())
        return 0;
    if (m32_stack_low())
        return 1;
    return jgb_usable() && !mem_guard_needed() && !stack_guard_needed();
}

static inline void m32_stack_st(A64Buf *b, int rv, int wa)
{
    if (!m32_stack_low() || g_m32low) {
        a64_str_regoff_uxtw(b, 4, rv, JGB, wa);
        return;
    }
    a64_mov_reg(b, 0, JTU, wa);
    (void)a64_try_orr_imm(b, 1, JTU, JTU, ocerz_low_base);
    a64_str(b, 4, rv, JTU, 0);
}

static inline void m32_stack_ld(A64Buf *b, int rd, int wa)
{
    if (!m32_stack_low() || g_m32low) {
        a64_ldr_regoff_uxtw(b, 4, rd, JGB, wa);
        return;
    }
    a64_mov_reg(b, 0, JTU, wa);
    (void)a64_try_orr_imm(b, 1, JTU, JTU, ocerz_low_base);
    a64_ldr(b, 4, rd, JTU, 0);
}

static inline int m32_stack_ok(const X86Insn *insn)
{
    return insn->seg == OCERZ_SEG_NONE && m32_stack_base_ok();
}

static inline void emit_cc_predicate(A64Buf *b, unsigned cc)
{
    emit_cc_predicate_ex(b, cc, 0);
}

static inline int xmm_vreg(unsigned xr) { return 16 + (int)xr; }

static inline int xmm_is_pinned(unsigned xr) { return (g_xmm_pinned >> xr) & 1; }

static inline void emit_xmm_pin_load_all(A64Buf *b)
{
    for (unsigned r = 0; r < 16; r++)
        if (xmm_is_pinned(r))
            a64_ldr_v(b, 16, xmm_vreg(r), 20, XMM_BASE_OFF + r * 16);
    emit_pk_consts_load(b);
}

static inline void emit_xmm_pin_spill_all(A64Buf *b)
{
    for (unsigned r = 0; r < 16; r++)
        if (xmm_is_pinned(r))
            a64_str_v(b, 16, xmm_vreg(r), 20, XMM_BASE_OFF + r * 16);
}

static inline void emit_sse_mem_ld_gpr(A64Buf *b, int size, int rd)
{
    if (g_sse_mem_plain) emit_gpr_ld_at(b, size, rd, g_sse_mem_ra, (int32_t)g_sse_mem_disp, g_sse_mem_plainacc);
    else emit_guest_load_ordered(b, size, rd, JTA, JTU);
}

static inline void emit_sse_mem_ld(A64Buf *b, int size, int vd)
{
    emit_v_ld_at(b, size, vd, g_sse_mem_ra, (int32_t)g_sse_mem_disp, g_sse_mem_plainacc);
}

static inline void emit_sse_mem_st(A64Buf *b, int size, int vs)
{
    emit_v_st_at(b, size, vs, g_sse_mem_ra, (int32_t)g_sse_mem_disp, g_sse_mem_plainacc);
}

static inline void emit_sse_mem_st_gpr(A64Buf *b, int size, int rv)
{
    if (g_sse_mem_plain) emit_gpr_st_at(b, size, rv, g_sse_mem_ra, (int32_t)g_sse_mem_disp, g_sse_mem_plainacc);
    else emit_guest_store_ordered(b, size, rv, JTA, JTU);
}

static inline int emit_mem_load_any(A64Buf *b, const X86Insn *insn, const X86Operand *op, int size, int rd)
{
    if (emit_mem_load_plain(b, insn, op, size, rd)) return 1;
    uint32_t *skip;
    if (!emit_sse_mem_addr(b, insn, op, size, NULL, NULL, &skip)) return 0;
    emit_sse_mem_ld_gpr(b, size, rd);
    patch_guard_skip(skip, a64_label(b));
    return 1;
}

static inline int scalar_cvt_follows(unsigned xreg, int dbl)
{
    if (!dbl || !g_cur_insns || g_cur_insn_idx < 0 || g_cur_insn_idx + 1 >= g_cur_insns_n) return 0;
    if (g_n_nanool + 2 > NANOOL_MAX || unsafe_nocheckbr()) return 0;
    const X86Insn *c = &g_cur_insns[g_cur_insn_idx + 1];
    if (c->vex || g_cur_insns[g_cur_insn_idx].vex) return 0;
    if (c->op != OCERZ_OP_CVTTSD2SI || c->nops != 2) return 0;
    const X86Operand *d = &c->ops[0], *sr = &c->ops[1];
    if (sr->kind != OCERZ_OPK_XMM || sr->reg != xreg || !xmm_is_pinned(xreg)) return 0;
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
    if (rsp_is_ptr() && d->reg == OCERZ_RSP) return 0;
    return 1;
}

static inline void scalar_pend_flush(A64Buf *b)
{
    if (!g_scpend.valid) return;
    g_scpend.valid = 0;
    g_scalar_merge_next = 0;
    emit_nan_fix_scalar2(b, g_scpend.dbl, g_scpend.vr, g_scpend.va, g_scpend.vb);
}

static inline void emit_pk_consts_load(A64Buf *b)
{
    return;
    if (!g_pk_consts_needed) return;
    a64_mov_imm64(b, JT0, 0x0008000000000000ull); a64_fmov_v_from_x(b, 1, 4, JT0); a64_v_dup_d(b, 4, 4, 0);
    a64_mov_imm64(b, JT0, 0xfff8000000000000ull); a64_fmov_v_from_x(b, 1, 5, JT0); a64_v_dup_d(b, 5, 5, 0);
    a64_mov_imm64(b, JT0, 0x00400000ull);         a64_fmov_v_from_x(b, 0, 6, JT0); a64_v_dup_s(b, 6, 6, 0);
    a64_mov_imm64(b, JT0, 0xffc00000ull);         a64_fmov_v_from_x(b, 0, 7, JT0); a64_v_dup_s(b, 7, 7, 0);
}

static inline void fpb_undo_clear(int i)
{
    g_fpb_undo[i] = 0; g_fpb_undo_size[i] = 0; g_fpb_undo_done[i] = 0;
    g_fpb_undo_ld[i] = 0; g_fpb_undo_ldsz[i] = 0; g_fpb_undo_ldst[i] = -1; g_fpb_undo_from[i] = -1;
}

static inline int lane_reserve(void)
{
    int lane = -1;
    for (int k = L0_NLANES - 1; k >= 0; k--) if (!(g_lane_used & (1u << k))) { lane = k; break; }
    if (lane < 0) return -1;
    if (!g_l0_fixed) {
        if (lane != g_l0_nlanes - 1 || g_l0_nlanes <= 4) return -1;
        g_l0_nlanes--;
    }
    g_lane_used |= (uint16_t)(1u << lane);
    return 4 + lane;
}

static inline void fpb_scan(const X86Insn *insns, int n, int8_t *bat)
{
    if (fpb_v1() || g_xlat_mode32) {
        g_fpb_exit_mask = 0; g_fpb_exit_batch = -1;
        for (int i = 0; i < n; i++) { g_fpb_stchk[i] = 0; g_fpb_stlane[i] = 0; fpb_undo_clear(i); }
        g_fpb_v1_active = 1;
        fpb_scan_v1(insns, n, bat);
        g_fpb_v1_active = 0;
        return;
    }
    fpb_scan_v2(insns, n, bat);
}

static inline void fpb_replay_prelude(A64Buf *b, const FpBatch *fb, const int8_t *l0, const uint8_t *l0_dbl)
{
    for (int r = 0; r < 16; r++)
        if (fb->ckpt_emit & (1u << r))
            a64_ldr_v(b, 16, xmm_vreg((unsigned)r), 20, FPCKPT_OFF + (uint32_t)r * 16);
    for (int r = 0; r < 16; r++)
        if ((fb->dirty_open & ~fb->written & ~fb->ckpt_emit & (1u << r)) && l0[r] >= 0) {
            if (l0_dbl[r]) a64_ins_d_d(b, xmm_vreg((unsigned)r), 0, l0[r], 0);
            else           a64_ins_s_s(b, xmm_vreg((unsigned)r), 0, l0[r], 0);
        }
}

static inline void fpb_site_emit(A64Buf *b, int end, int va, int vb, int dbl)
{
    if (g_n_fpb_sites >= FPB_SITES_MAX) return;
    FpbSite *st = &g_fpb_sites[g_n_fpb_sites++];
    st->batch = g_fpb_open; st->end = end;
    st->site = a64_label(b); a64_bcond(b, A64_VS, 0);
    st->back = a64_label(b);
    for (int r = 0; r < 16; r++) { st->l0[r] = g_l0[r]; st->l0_dbl[r] = g_l0_dbl[r]; }
    st->fcmp_a = (int8_t)va; st->fcmp_b = (int8_t)vb; st->fcmp_dbl = (uint8_t)dbl;
    st->keep_jt = 0;
}

static inline int fpb_det_here(int idx)
{
    return g_fpb_open >= 0 && g_fpb_of && idx >= 0 && g_fpb_det[idx] && g_fpb_of[idx] == g_fpb_open;
}

static inline void yc_flush_all(A64Buf *b)
{
    for (unsigned r = 0; r < 16; r++)
        if ((g_yc_dirty & (1u << r)) && g_yc[r] >= 0)
            a64_str_v(b, 16, g_yc[r], 20, YMMH_OFF + r * 16);
    g_yc_dirty = 0;
}

static inline void yc_flush_from(A64Buf *b, uint16_t dirty)
{
    for (unsigned r = 0; r < 16; r++)
        if ((dirty & (1u << r)) && g_yc[r] >= 0)
            a64_str_v(b, 16, g_yc[r], 20, YMMH_OFF + r * 16);
}

static inline void yc_reload_all(A64Buf *b)
{
    for (unsigned r = 0; r < 16; r++)
        if (g_yc[r] >= 0)
            a64_ldr_v(b, 16, g_yc[r], 20, YMMH_OFF + r * 16);
}

static inline void l0_reset(void)
{
    for (int i = 0; i < 16; i++) g_l0[i] = -1;
    for (int i = 0; i < L0_NLANES; i++) g_l0_owners[i] = 0;
    g_l0_dirty = 0;
}

static inline void l0_flush_reg(A64Buf *b, unsigned r)
{
    if (!(g_l0_dirty & (1u << r)))
        return;
    g_l0_dirty &= (uint16_t)~(1u << r);
    if (g_l0[r] < 0)
        return;
    if (g_l0_dbl[r]) a64_ins_d_d(b, xmm_vreg(r), 0, g_l0[r], 0);
    else             a64_ins_s_s(b, xmm_vreg(r), 0, g_l0[r], 0);
}

static inline void l0_flush_all(A64Buf *b)
{
    while (g_l0_dirty)
        l0_flush_reg(b, (unsigned)__builtin_ctz(g_l0_dirty));
    yc_flush_all(b);
}

static inline int l0_defer_take(int vs, unsigned xr, int size)
{
    if (g_xlat_mode32)
        return 0;
    if (!(l0_defer() && l0_enabled() && vs >= 4 && vs < 4 + L0_NLANES &&
          g_l0[xr] == vs && g_l0_dbl[xr] == (uint8_t)(size == 8)))
        return 0;
    g_l0_dirty |= (uint16_t)(1u << xr);
    return 1;
}

static inline void l0_inval(unsigned r)
{
    if (r < 16 && g_l0[r] >= 0) { g_l0_owners[g_l0[r] - 4] &= (uint16_t)~(1u << r); g_l0[r] = -1; }
    g_l0_dirty &= (uint16_t)~(1u << r);
}

static inline void l0_fixed_map(void)
{
    g_l0_dirty = 0;
    for (int r = 0; r < 16; r++) if (g_l0_fixed_lane[r] >= 0) {
        g_l0[r] = g_l0_fixed_lane[r];
        g_l0_dbl[r] = g_l0_fixed_dbl[r];
        g_l0_owners[g_l0_fixed_lane[r] - 4] = (uint16_t)(1u << r);
        g_lane_used |= (uint16_t)(1u << (g_l0_fixed_lane[r] - 4));
        if (g_l0_fixed_dirty & (1u << r)) g_l0_dirty |= (uint16_t)(1u << r);
    }
}

static inline int l0_src2(A64Buf *b, unsigned r, int dbl)
{
    if (l0_enabled() && g_l0[r] >= 0 && g_l0_dbl[r] == (uint8_t)dbl) return g_l0[r];
    l0_flush_reg(b, r);
    return xmm_vreg(r);
}

static inline void fpb_emit_undo_save(A64Buf *b, const X86Insn *insn, int i, uint32_t **exit_sites, int *n_exits)
{
    int size = g_fpb_undo_size[i];
    int vr = g_undo_vreg[g_fpb_undo[i] - 1];
    if (emit_plain_mem_fast(b, insn, &insn->ops[0], size, vr, 0, 1)) return;
    uint32_t *skip;
    if (!emit_sse_mem_addr(b, insn, &insn->ops[0], size, exit_sites, n_exits, &skip)) {
        g_fpb_undo[i] = 0;
        return;
    }
    emit_sse_mem_ld(b, size, vr);
    patch_guard_skip(skip, a64_label(b));
}

static inline void fpb_emit_undo_restore(A64Buf *b, const X86Insn *insns, int first, int end, uint32_t **exit_sites, int *n_exits)
{
    for (int m = end; m >= first; m--) {
        if (!g_fpb_undo[m]) continue;
        int size = g_fpb_undo_size[m];
        int vr = g_undo_vreg[g_fpb_undo[m] - 1];
        if (emit_plain_mem_fast(b, &insns[m], &insns[m].ops[0], size, vr, 1, 1)) continue;
        uint32_t *skip;
        if (!emit_sse_mem_addr(b, &insns[m], &insns[m].ops[0], size, exit_sites, n_exits, &skip)) continue;
        emit_sse_mem_st(b, size, vr);
        patch_guard_skip(skip, a64_label(b));
    }
}

static inline void fpb_emit_exit_check(A64Buf *b)
{
    if (g_fpb_exit_batch < 0 || !g_fpb_exit_mask) return;
    for (unsigned r = 0; r < 16; r++) if (g_fpb_exit_mask & (1u << r)) l0_flush_reg(b, r);
    int before = g_n_fpb_sites;
    fpb_emit_regs_check(b, g_fpb_exit_mask, g_fpb_exit_batch, g_fpb_exit_end, g_l0, g_l0_dbl);
    if (g_n_fpb_sites > before) g_fpb_sites[g_n_fpb_sites - 1].keep_jt = 1;
    g_fpb_exit_mask = 0;
}

static inline void l0_share(unsigned dst, unsigned src)
{
    l0_inval(dst);
    if (l0_enabled() && g_l0[src] >= 0) {
        g_l0[dst] = g_l0[src]; g_l0_dbl[dst] = g_l0_dbl[src];
        g_l0_owners[g_l0[src] - 4] |= (uint16_t)(1u << dst);
        if (g_l0_dirty & (1u << src))
            g_l0_dirty |= (uint16_t)(1u << dst);
    }
}

static inline void l0_fixed_backedge(A64Buf *b)
{
    if (g_l0_fixed)
        l0_fixed_restore(b);
}

static inline void l0_fixed_fallthrough(A64Buf *b)
{
    if (g_l0_fixed)
        l0_flush_all(b);
}

static inline void x87_reset(void)
{
    g_x87_live = 0;
    g_x87_delta = 0;
    g_n_x87_run = 0;
    g_x87_cur = -1;
    g_n_x87_site = 0;
    g_n_x87_frag = 0;
    g_x87_frag_open = 0;
    g_x87_nzcv = -1;
}

static inline int x87_inline_ok(const X86Insn *insn)
{
    return x87_run_flags(insn) != 0;
}

static inline void emit_prof_count(A64Buf *b, JitProf *pf, uint32_t off)
{
    tc_imm64(b, JT1, TCR_PROF, (uint64_t)((const char *)pf - (const char *)g_cur_blk->prof), (uint64_t)(uintptr_t)pf);
    a64_ldr(b, 4, JT2, JT1, off);
    a64_add_imm(b, 0, JT2, JT2, 1);
    a64_str(b, 4, JT2, JT1, off);
}

static inline void emit_side_tag(A64Buf *b, int cpu_reg)
{
    if (!g_tag_blk) return;
    tc_imm64(b, JT0, TCR_BLK, 0, (uint64_t)(uintptr_t)g_tag_blk);
    a64_str(b, 8, JT0, cpu_reg, SIDE_BLK_OFF);
    a64_mov_imm64(b, JT0, (uint64_t)g_tag_idx);
    a64_str(b, 4, JT0, cpu_reg, SIDE_IDX_OFF);
}

static inline int side_stub_has_work(int k)
{
    if (g_side[k].rec) return 1;
    if (g_side[k].fpb >= 0 && g_side[k].fpb_chk) return 1;
    for (int r = 0; r < 16; r++)
        if ((g_side[k].l0_dirty & (1u << r)) && g_side[k].l0[r] >= 0) return 1;
    return 0;
}

static inline int can_fuse_incdec_jcc(const X86Insn *producer, const X86Insn *jcc)
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

static inline int ras_body_only(void)
{
    return fullpin_enabled() && !g_no_regflags && stack_plain_access_ok() && stack_fast() &&
           !g_no_chain && !g_no_ras;
}

static inline void *ras_entry_for(const JitBlock *blk)
{
    if (!blk || !blk->code) return NULL;
    if (ras_body_only())
        return (blk->pin_class == 3 && blk->body_code) ? (void *)blk->body_code : NULL;
    if (blk->pin_class == 3 && blk->body_code)
        return (void *)((uintptr_t)blk->body_code | 1u);
    return (void *)blk->code;
}

static inline uint32_t *emit_static_chain_tail(A64Buf *b, uint64_t target_rip,
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

static inline int mark_has(MarkSet *m, uint64_t key)
{
    if (m->full) return 1;
    unsigned i = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 52) & (LOWHOIST_N - 1);
    for (unsigned k = 0; k < LOWHOIST_N; k++, i = (i + 1) & (LOWHOIST_N - 1)) {
        uint64_t v = __atomic_load_n(&m->off[i], __ATOMIC_RELAXED);
        if (v == key) return 1;
        if (v == 0) return 0;
    }
    return 0;
}

static inline void mark_add(MarkSet *m, uint64_t key)
{
    unsigned i = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 52) & (LOWHOIST_N - 1);
    for (unsigned k = 0; k < LOWHOIST_N; k++, i = (i + 1) & (LOWHOIST_N - 1)) {
        uint64_t v = __atomic_load_n(&m->off[i], __ATOMIC_RELAXED);
        if (v == key) return;
        if (v == 0 && __atomic_compare_exchange_n(&m->off[i], &v, key, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return;
    }
    m->full = 1;
}

static inline void lowhoist_mark(uint64_t key) { mark_add(&g_lowhoist_marks, key); }

static inline void emit_l0_flush_from(A64Buf *b, const int8_t *l0, const uint8_t *l0_dbl, uint16_t dirty)
{
    for (unsigned r = 0; r < 16; r++)
        if ((dirty & (1u << r)) && l0[r] >= 0) {
            if (l0_dbl[r]) a64_ins_d_d(b, xmm_vreg(r), 0, l0[r], 0);
            else           a64_ins_s_s(b, xmm_vreg(r), 0, l0[r], 0);
        }
}

static inline void emit_l0_reload_from(A64Buf *b, const int8_t *l0, const uint8_t *l0_dbl)
{
    for (unsigned r = 0; r < 16; r++)
        if (l0[r] >= 0) {
            if (l0_dbl[r]) a64_fmov_d_d(b, l0[r], xmm_vreg(r));
            else           a64_fmov_s_s(b, l0[r], xmm_vreg(r));
        }
}

static inline void emit_slowcall_keep_lanes(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    uint16_t dirty = g_l0_dirty;
    emit_slowcall(b, insn, exit_sites, n_exits);
    emit_l0_reload_from(b, g_l0, g_l0_dbl);
    g_l0_dirty = dirty;
}

static inline void patch_any_branch(uint32_t *site, uint32_t *target)
{
    uint32_t w = *site;
    if ((w & 0x7e000000u) == 0x34000000u) a64_patch_cbz(site, target);
    else if ((w & 0x7e000000u) == 0x36000000u) a64_patch_tbz(site, target);
    else if ((w & 0xff000010u) == 0x54000000u) a64_patch_bcond(site, target);
    else a64_patch_b(site, target);
}

static inline void emit_misaligned_pieces_st(A64Buf *b, int psize, int n, int rv, int ra, int32_t disp, int s1)
{
    a64_stlur(b, psize, rv, ra, disp);
    for (int i = 1; i < n; i++) {
        a64_lsr_imm(b, 1, s1, rv, i * psize * 8);
        a64_stlur(b, psize, s1, ra, disp + i * psize);
    }
}

static inline void tc_log_init(void)
{
    if (g_tc_log >= 0)
        return;
    const char *lp = getenv("OCERZ_TCACHE_LOG");
    g_tc_lf = lp && strcmp(lp, "1") ? fopen(lp, "a") : NULL;
    if (g_tc_lf) setvbuf(g_tc_lf, NULL, _IOLBF, 0);
    if (!g_tc_lf) g_tc_lf = stderr;
    g_tc_log = lp ? 1 : 0;
    if (g_tc_log) atexit(tc_summary);
}

static inline int tc_usable(const OcerzJit *jit)
{
    return ocerz_mode != OCERZ_MODE_NATIVE && ocerz_jitstat <= 0 && ocerz_perfstat <= 0 &&
           !jit->stop_requested;
}

static inline int tc_keepable(uint64_t rip)
{
    if (ocerz_guest_base != 0)
        return 0;
    return ocerz_low_base != 0 || ocerz_cache_region((uintptr_t)ocerz_g2h(rip));
}

static inline uint64_t tc_key(uint64_t rip, int mode32)
{
    return rip | (mode32 ? OCERZ_TC_KEY_M32 : 0) | (g_plain_mem ? OCERZ_TC_KEY_PLAIN : 0);
}

static inline unsigned psc_col(uint64_t key)
{
    return (unsigned)((key >> 2) & (PSC_N - 1));
}

static inline void steplog(const OcerzCPU *cpu)
{
    fprintf(stderr, "STEP %#llx", (unsigned long long)cpu->rip);
    for (int i = 0; i < 16; i++) fprintf(stderr, " %llx", (unsigned long long)cpu->gpr[i]);
    fprintf(stderr, "\n");
}

#endif

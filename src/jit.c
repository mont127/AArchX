/*
 * The JIT: guest basic blocks translated to native arm64, with the
 * interpreter as the fallback for anything it will not encode.
 *
 * ---- blocks and the cache ----
 * Blocks are keyed by jit_key(rip, mode32): the same guest address in 32- and
 * 64-bit mode is not the same code and must never share a cache entry, a
 * commpage mark or an invalidation mark.  A published block keeps 16 bytes per
 * instruction (rip, len, op) instead of the 96-byte X86Insn; only the few
 * instructions it still executes through the interpreter (slow calls) or
 * inspects in full (fault-flag producers) are copied into blk->kept.  Dropping
 * the decoded array matters: a GUI Wine process carried ~870 bytes of X86Insn
 * per block across 215k blocks, 180 MB of it.
 *
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
 * OCERZ_RSP_VALUE restores the old value-keeping class 3.
 *
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
 * default.  An ordered vector load is a plain load followed by a one-byte
 * ACQUIRE load of the same address: same-address reads are coherent, so the
 * copy sees a value at least as new as the vector and everything after is
 * ordered behind it - TSO's load ordering with no barrier, where a dmb ishld
 * waited for every outstanding miss (40-60 ns per access on a 4 MB working set).
 *
 * Apple silicon faults an acquire/release access only when it crosses a 16-byte
 * granule, so the alignment guard tests exactly that; testing natural alignment
 * instead sent three quarters of memcpy's unaligned tail accesses down the slow
 * arm.  Faulting sites are hot-patched, and the out-of-line store arm picks
 * dmb ish + plain store when the block also does ordered loads (the drain is
 * cheap then: memcpy 0.31s vs 1.1s for release pieces) and release pieces
 * otherwise (store-only loops: memset 0.10s vs 0.5-1.0s).
 *
 * ---- flags ----
 * Flags are deferred: an instruction records {kind, size, dst, src} and the
 * flags are materialized only if something reads them.  On top of that sit
 * three fusions - NZCV forwarded from an adjacent producer across
 * NZCV-transparent gap instructions, value-based conditions taken straight from
 * a result register (cmp #0, or no compare at all for cbz/cbnz), and the comis
 * fusion, where a jcc/setcc/cmov re-derives its condition by redoing the fcmp.
 * A static liveness pass and the emitters share the same predicates so they
 * cannot disagree about what is live.  The legality rules here are written in
 * blood: a gap may not write a register the producer read (`cmp byte
 * [rax+rcx-1],0xc0 ; mov rcx,rbx ; jcc` spun libSystem's UTF-8 scan forever),
 * and a gap's emission must touch only pinned registers (`cmp byte
 * [rdi+0x210],0 ; lea r15,[rsp+0x290] ; jne` branched on rsp and made Steam's
 * CEF browser copy an unengaged optional, 2026-09-06), so a gap is emitted into
 * a scratch buffer first and refused there rather than asserting after the
 * compare is already out.
 *
 * No ABI passes arithmetic flags across a return, but clang's outliner does:
 * vImage's `cmpq $0, init_CGInterfaces(%rip); retq` helpers hand their compare
 * back in EFLAGS, and every Wine window painted black because CoreGraphics
 * concluded libCGInterfaces had not loaded (2026-09-05).  A pure flag producer
 * reaching a ret keeps its flags live; a tail whose last flag writer is
 * arithmetic (xor eax,eax; ret) returns a value, and keeps the dead seam.
 *
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
 * an FP-batch check or the producer's flag record must stay on the path: subsd's
 * result left in scratch and a je's side exit chained past the flush made
 * NSViewGetTransformToDescendant assert on a singular matrix, and a stale flag
 * record handed to a successor (`cmp ebp,0xb ; jbe L` with `L: ja`) failed every
 * SQLite open in libcef.
 *
 * A guest CALL pushes its return address and also pushes {retaddr, host
 * continuation} onto a host-stack shadow and a return-address stack, then `bl`s
 * into the callee body, so the hardware return predictor matches the RAS and a
 * guest RET is a plain ret.  Indirect jmp/call go through a per-site
 * direct-mapped cache of 32 {rip, body} pairs (16-aligned, so the lookup's ldp
 * is single-copy atomic) before falling into an inlined hash probe and finally
 * C.  Small straight-line callees ending in a plain ret are spliced into the
 * caller: the call becomes a push, the ret a compare against the known return
 * address, and a mismatch leaves at the ret's rip for the dispatcher to run the
 * real one.  Where a frame is pure register work the push's slot is provably
 * never read, so the push becomes a bare rsp -= 8 and matched push/pop pairs
 * become register renames - the loop-carried store-to-load chain of call-dense
 * code.
 *
 * ---- invalidation ----
 * Every guest mmap/mprotect/munmap asks the JIT to drop code in a range.  A
 * global min/max cannot answer that under Wine, where the live set spans PE
 * images at 32-bit addresses and shared-cache dylibs at 0x7ff8_0000_0000, so a
 * region map (one slot per 4 MB, a bitmap of the 64 KB granules in it) answers
 * in constant time; it may say "maybe" after code is gone but never "no" while
 * it is present.  Only the overlapping blocks are retired - dropping the whole
 * cache per flip made CEF startup a full retranslation storm.  Their code stays
 * allocated on a retired list, so a thread still inside runs to its next exit.
 * A 64 KB region whose translations keep being invalidated (a JS engine
 * W^X-flipping its code space) is run interpreted after a few hits, but not
 * permanently: module-load fixups also retire blocks a few times and then never
 * again, and a permanent blacklist left the hottest DLL code interpreting
 * forever, so a region quiet for CHURN_QUIET_NS is re-probed.
 *
 * ---- faults and fork ----
 * A fault inside a block reconstructs the guest state from the host registers:
 * a push whose store faulted has already decremented rsp in its host register
 * (+8 repairs it), elided return-address slots are written back so the
 * interpreter can resume mid-frame, and the XMM pins are recovered from the
 * signal frame's NEON state because the memory copy is stale.  A fork child
 * inherits the parent's MAP_JIT arena, whose pages fault when executed, so the
 * child abandons the arena (rather than freeing it - the fork may have caught
 * the allocator mid-update) and builds a fresh one on its next step.
 *
 * ---- i386 ----
 * 32-bit blocks are compiled from a whitelist of instructions, with none of the
 * fusions, no superblocks, and 0x67/16-bit addressing left to the interpreter.
 * Effective addresses wrap at 2^32 before the host mapping is applied, and pin
 * class 2 (the 64-bit CALL/RET protocol) is never selected for them.
 *
 * ---- bisection ----
 * OCERZ_INTERP_LO/HI and OCERZ_INTERP_RIP keep chosen ranges or addresses in
 * the interpreter, which is how a JIT miscompile is narrowed down;
 * OCERZ_CHAINCHECK validates every published jump target against the arena,
 * OCERZ_INVMAP_CHECK asserts the region map's one invariant, OCERZ_BTRACE
 * records block entries, and the OCERZ_UNSAFE_* knobs are measurement aids that
 * deliberately produce wrong state and must never be enabled outside a
 * benchmark.
 */
#include <execinfo.h>
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

#define JIT_KEY_M32 (1ull << 63)

static inline uint64_t jit_key(uint64_t rip, int mode32)
{
    return mode32 ? (rip | JIT_KEY_M32) : rip;
}
static inline uint64_t jit_key_rip(uint64_t key) { return key & ~JIT_KEY_M32; }
static inline int jit_key_mode32(uint64_t key) { return (int)(key >> 63); }

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
    uint8_t windows, prev;
} JitProf;

#define JIT_MAX_EDGES 8

typedef struct JitInsnRef {
    uint64_t rip;
    uint16_t op;
    uint8_t len;
    uint8_t flags;
    uint16_t keep;
    uint16_t pad;
} JitInsnRef;

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
    struct { uint32_t *site; uint32_t insn; } stop_extra[6];
    uint8_t n_stop_extra;
    uint32_t *push_fix;
    uint16_t n_push_fix;
    struct JitPushElide { int32_t ci, rj; uint64_t ra; } *pushelide;
    uint16_t n_pushelide;
    struct JitPromo { int32_t pi, qi; uint8_t hreg; };
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

static inline uint64_t blk_rip(const JitBlock *b) { return jit_key_rip(b->key); }
static inline int blk_mode32(const JitBlock *b) { return jit_key_mode32(b->key); }
static inline uint64_t blk_insn_rip(const JitBlock *b, int i) { return b->insns ? b->insns[i].rip : b->iref[i].rip; }
static inline unsigned blk_insn_len(const JitBlock *b, int i) { return b->insns ? b->insns[i].len : b->iref[i].len; }
static inline unsigned blk_insn_op(const JitBlock *b, int i) { return b->insns ? b->insns[i].op : b->iref[i].op; }
static inline const X86Insn *blk_insn_full(const JitBlock *b, int i)
{
    if (b->insns) return &b->insns[i];
    unsigned k = b->iref[i].keep;
    return k ? &b->kept[k - 1] : NULL;
}

#define INVMAP_RSHIFT 22
#define INVMAP_GSHIFT 16
#define INVMAP_SLOTS  8192
#define INVMAP_PROBE  32
#define INVMAP_MAX_SPAN 64

typedef struct { uint64_t tag, bits; } InvSlot;
#define INVMAP_TOMB ((uint64_t)-1)

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
    uint32_t *dispatch_stub;
    uint32_t *dispatch_stub32;
    JitBlock **live;
    size_t n_live, cap_live;
    uint64_t code_lo, code_hi;
    InvSlot invmap[INVMAP_SLOTS];
    int invmap_full;
};

int ocerz_perfstat = -1;

static int g_flaglive_log;

static int g_no_lazyflags;

static int g_no_ras;

static int g_no_ldapr;

static int g_no_oolslow;
static void ea_cache_reset(void);

typedef struct {
    uint32_t *bne;
    uint32_t *back;
    int size, rv, ra, store, idx;
    int vec;
    int32_t disp;
} OrderedSlowPend;
#define OSLOW_MAX 64
static OrderedSlowPend g_oslow[OSLOW_MAX];

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
#define NANOOL_MAX 64
static NanOolPend g_nanool[NANOOL_MAX];
static int g_n_nanool;
#define PE_MAX 48
static struct JitPushElide g_pe_real[PE_MAX];
static int g_n_pe_real;
static struct JitPromo g_promo_real[PE_MAX];
static int g_n_promo_real;
static const X86Insn *g_pe_insns;
static uint32_t g_rsp_lag;
static int g_n_oslow;
static int g_cur_insn_idx;
static const X86Insn *g_flag_producer;
static int g_flag_producer_operands_intact;

static int g_no_regflags;
static unsigned long long g_callout_seq;
static int l0_src2(struct A64Buf *b, unsigned r, int dbl);
#define l0_src(r, dbl) l0_src2(b, r, dbl)
static void l0_flush_reg(struct A64Buf *b, unsigned r);
static void l0_flush_all(struct A64Buf *b);
static int l0_defer_take(int vs, unsigned xr, int size);
static void l0_share(unsigned dst, unsigned src);
static void l0_inval(unsigned r);
static int g_xlat_n;
static int g_jcc_side_mode;
static uint64_t g_jcc_side_need;
static uint64_t g_jcc_side_fall_need;
static int g_nzcv_want;
#define NZCV_KIND_BT 0x7f
static int g_nzcv_from = -1;
static unsigned g_nzcv_kind;

static int g_no_chain;

static int g_no_jcclink;

static int g_no_xlive;

static int g_no_jccfuse;

static int g_no_addincfuse;

static int g_no_fault_recipes;

static int g_plain_mem;

static uint64_t g_chain_target;
static uint32_t *g_chain_epi;
static int g_chain_keeps_jgb;
typedef struct { uint32_t *site; uint64_t retaddr; uint64_t hi; int kind; int rt; } RasLit;
#define RASLIT_MAX 96
static RasLit g_raslit[RASLIT_MAX];
static int g_n_raslit;
typedef struct { _Alignas(16) uint64_t rip; void *body; } JitPscEnt;
#define PSC_N 32
#define PSC_EMPTY_RIP UINT64_MAX
static JitPscEnt *g_psc_pool;
static size_t g_psc_used, g_psc_cap;
static JitPscEnt **g_psc_tables;
static size_t g_n_psc_tables, g_cap_psc_tables;
static JitPscEnt *psc_alloc(void)
{
    if (g_psc_used + PSC_N > g_psc_cap) {
        size_t bytes = (size_t)1 << 22;
        void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED) return NULL;
        g_psc_pool = (JitPscEnt *)p; g_psc_used = 0; g_psc_cap = bytes / sizeof(JitPscEnt);
    }
    JitPscEnt *t = &g_psc_pool[g_psc_used];
    g_psc_used += PSC_N;
    for (int k = 0; k < PSC_N; k++)
        t[k].rip = PSC_EMPTY_RIP;
    if (g_n_psc_tables == g_cap_psc_tables) {
        size_t ncap = g_cap_psc_tables ? g_cap_psc_tables * 2 : 1024;
        JitPscEnt **nv = (JitPscEnt **)realloc(g_psc_tables, ncap * sizeof *nv);
        if (nv) { g_psc_tables = nv; g_cap_psc_tables = ncap; }
    }
    if (g_n_psc_tables < g_cap_psc_tables) g_psc_tables[g_n_psc_tables++] = t;
    return t;
}
static void psc_clear_all(void)
{
    for (size_t i = 0; i < g_n_psc_tables; i++)
        for (int k = 0; k < PSC_N; k++) {
            __atomic_store_n(&g_psc_tables[i][k].rip, PSC_EMPTY_RIP, __ATOMIC_RELEASE);
            __atomic_store_n(&g_psc_tables[i][k].body, (void *)NULL, __ATOMIC_RELEASE);
        }
}
static void ***g_ras_cells;
static size_t g_n_ras_cells, g_cap_ras_cells;
static void ras_cell_register(void **cell)
{
    if (g_n_ras_cells == g_cap_ras_cells) {
        size_t ncap = g_cap_ras_cells ? g_cap_ras_cells * 2 : 1024;
        void ***nv = (void ***)realloc(g_ras_cells, ncap * sizeof *nv);
        if (!nv) return;
        g_ras_cells = nv; g_cap_ras_cells = ncap;
    }
    g_ras_cells[g_n_ras_cells++] = cell;
}

static uint64_t g_self_rip;
static uint32_t *g_body_entry;
static uint32_t *g_loop_entry;
static uint32_t *g_stop_patch;
#define SIDE_MAX 6
static struct { uint32_t *site; uint64_t taken; int idx; uint32_t *stub; uint32_t *patch_b;
                int rec; uint32_t rec_ccop; int rec_src, rec_dst, rec_imm_pending; uint64_t rec_imm;
                uint64_t jcc_rip; int probe; uint32_t *ft_site; uint64_t ft_rip;
                int fpb; uint16_t fpb_chk; int fpb_end; int8_t l0[16]; uint8_t l0_dbl[16];
                uint16_t l0_dirty; } g_side[SIDE_MAX];
static int g_n_side;

#define FLIP_N 4096
#define PROBE_BIT 10
#define PROBE_MAX 65536
enum { FLIP_NONE = 0, FLIP_DECIDED_ORIG, FLIP_DECIDED_INV };
static struct { uint64_t rip; uint8_t state; } g_flip[FLIP_N];
static int g_churn_suppress;
static int g_n_probes;
static int flip_disabled(void)
{
    static int en = -1;
    if (en < 0) en = getenv("OCERZ_NO_FLIP") ? 1 : 0;
    return en;
}
static int flip_find(uint64_t rip, int insert)
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
static int flip_state(uint64_t rip)
{
    int i = flip_find(rip, 0);
    return i < 0 ? FLIP_NONE : g_flip[i].state;
}
static int superblock_enabled(void)
{
    static int en = -1;
    if (en < 0) en = getenv("OCERZ_NO_SUPERBLOCK") ? 0 : 1;
    return en;
}
static int jcc_flip_wanted(uint64_t jcc_rip)
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
static int superblock_back_enabled(void)
{
    static int en = -1;
    if (en < 0) en = getenv("OCERZ_NO_SB_BACK") ? 0 : 1;
    return en;
}
static uint32_t g_push_fix[JIT_MAX_BLOCK_INSNS];
static int g_n_push_fix;

#define CP_MARK_SIZE 256
static uint64_t g_cp_marks[CP_MARK_SIZE];
static int g_cp_nmarks;
static int g_cp_guard;
static int cp_marked(uint64_t key)
{
    for (int i = 0; i < g_cp_nmarks; i++) if (g_cp_marks[i] == key) return 1;
    return 0;
}
static void cp_mark(uint64_t key)
{
    if (cp_marked(key)) return;
    if (g_cp_nmarks < CP_MARK_SIZE) g_cp_marks[g_cp_nmarks++] = key;
    else g_cp_marks[0] = 0;
}
static inline int mem_guard_needed(void) { return ocerz_low_base != 0 || g_cp_guard; }

static int stack_plain_ok(void)
{
    static int en = -1;
    if (en < 0) en = getenv("OCERZ_TSO_STRICT") ? 0 : 1;
    return en;
}
#define AL_MARK_SIZE 256
static uint64_t g_al_marks[AL_MARK_SIZE];
static int g_al_nmarks;
static pthread_mutex_t jit_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_align_guard;
static int vec_tso_relaxed(void)
{
    static int v = -1;
    if (v < 0) v = getenv("OCERZ_TSO_VECTOR") == NULL;
    return v;
}
static unsigned long long ps_align_patches;
static int g_blk_ordered_loads;
static int g_al_all;
static int al_marked(uint64_t key)
{
    if (g_al_all) return 1;
    for (int i = 0; i < g_al_nmarks; i++) if (g_al_marks[i] == key) return 1;
    return 0;
}
static void al_mark(uint64_t key)
{
    if (al_marked(key)) return;
    if (g_al_nmarks < AL_MARK_SIZE) g_al_marks[g_al_nmarks++] = key;
    else g_al_all = 1;
}
static inline int stack_plain_access_ok(void) { return g_plain_mem || stack_plain_ok(); }
static inline int mem_plain_access_ok(const X86Operand *m)
{
    if (g_plain_mem) return 1;
    return stack_plain_ok() && m->base == OCERZ_RSP && !m->riprel;
}
static inline int jgb_usable(void);
static inline int mem_fast_forms_ok(void) { return jgb_usable() && !mem_guard_needed(); }
static inline int stack_guard_needed(void) { return ocerz_low_base != 0; }
static const uint32_t *g_push_entry;
static struct { uint32_t *site; uint32_t *target; } g_stop_extra[6];
static int g_n_stop_extra;
static void stop_extra_add(uint32_t *site, uint32_t *target)
{
    if (g_n_stop_extra < 6) { g_stop_extra[g_n_stop_extra].site = site; g_stop_extra[g_n_stop_extra].target = target; g_n_stop_extra++; }
}
static uint32_t *g_stop_target;

static uint32_t stop_retarget(uint32_t insn, const uint32_t *site,
                              const uint32_t *target)
{
    int32_t off = (int32_t)(target - site);
    if ((insn & 0xfc000000u) == 0x14000000u)
        return 0x14000000u | ((uint32_t)off & 0x03ffffffu);
    if ((insn & 0xff000010u) == 0x54000000u)
        return (insn & 0xff00001fu) | (((uint32_t)off & 0x7ffffu) << 5);
    if ((insn & 0x7e000000u) == 0x34000000u)
        return (insn & 0xff00001fu) | (((uint32_t)off & 0x7ffffu) << 5);
    if ((insn & 0x7e000000u) == 0x36000000u)
        return (insn & 0xfff8001fu) | (((uint32_t)off & 0x3fffu) << 5);
    return 0x14000000u | ((uint32_t)off & 0x03ffffffu);
}
static int g_mem_hoist_greg = -1;
static int g_mem_hoist_aux_disp;
static int g_mem_hoist_aux_index = -1;
static int g_mem_hoist_aux_scale;
#define JMEMBASE 17
#define JMEMAUX 29
#define JMEMBASE2 16
#define JMEMBASE3 30
static int g_mem_hoist_greg2 = -1;
static int g_mem_hoist_greg3 = -1;
static inline uint64_t hoist_signature(void)
{
    if (g_mem_hoist_greg < 0) return 0;
    return 1ull | ((uint64_t)(g_mem_hoist_greg & 0xff) << 8) | ((uint64_t)((g_mem_hoist_greg2 + 1) & 0xff) << 16) |
           ((uint64_t)((g_mem_hoist_greg3 + 1) & 0xff) << 24) | ((uint64_t)((g_mem_hoist_aux_index + 1) & 0xff) << 32) |
           ((uint64_t)(g_mem_hoist_aux_scale & 3) << 40) | ((uint64_t)((uint32_t)(g_mem_hoist_aux_disp + 0x8000) & 0xffff) << 44);
}

static inline int pin_slot(unsigned greg);
static inline int pin_hreg(int slot);
extern int g_pin_class_fwd(void);
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
#define JGB 0
static inline int jgb_usable(void) { return ocerz_low_base == 0; }
static inline int stack_identity(void)
{
    static int dis = -1;
    if (dis < 0) {
        dis = getenv("OCERZ_NO_STACK_IDX") ? 1 : 0;
    }
    return !dis && ocerz_guest_base == 0 && ocerz_low_base == 0;
}
void ocerz_jgb_trap(uint64_t rip, uint64_t x0);
void ocerz_jgb_trap(uint64_t rip, uint64_t x0)
{
    fprintf(stderr, "ocerz: JGB TRAP entering body of block %#llx with x0=%#llx (gbase=%#llx)\n",
            (unsigned long long)rip, (unsigned long long)x0, (unsigned long long)ocerz_guest_base);
    abort();
}
static void emit_reload_jgb(A64Buf *b)
{
    if (jgb_usable())
        a64_mov_imm64(b, JGB, ocerz_guest_base);
}
static void emit_reload_mem_base(A64Buf *b);

static struct {
    uint64_t target_rip;
    uint32_t *patch_b;
    uint32_t *cond_site;
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
static int g_xlat_mode32;
static int g_xlat_mode32_fwd(void) { return g_xlat_mode32; }

static int8_t *g_pin;
static uint8_t *g_pin_hold;
static int g_n_pinned;
static int g_pin_class;
static inline int rsp_ptr3(void)
{
    static int off = -1;
    if (off < 0) off = getenv("OCERZ_RSP_VALUE") != NULL;
    return !off;
}
static int g_xlat_mode32_fwd(void);
static inline int rsp_is_ptr(void)
{
    return g_pin_class == 2 ||
           (g_pin_class == 3 && !g_xlat_mode32_fwd() && rsp_ptr3());
}
int g_pin_class_fwd(void) { return g_pin_class; }

static int g_defer;
static int16_t g_mov_sink_at[JIT_MAX_BLOCK_INSNS];
static uint8_t g_mov_skip[JIT_MAX_BLOCK_INSNS];
static _Atomic unsigned long long ps_ops[OCERZ_OP_COUNT];
static char ps_shapes[OCERZ_OP_COUNT][3][96];
static void ps_note_shape(const X86Insn *insn)
{
    unsigned o = insn->op;
    if (o >= OCERZ_OP_COUNT) return;
    char buf[96];
    ocerz_format_insn(insn, buf, sizeof buf);
    for (int i = 0; i < 3; i++) {
        if (ps_shapes[o][i][0] == 0) { snprintf(ps_shapes[o][i], sizeof ps_shapes[o][i], "%s", buf); return; }
        if (strcmp(ps_shapes[o][i], buf) == 0) return;
    }
}
static _Atomic unsigned long long ps_slow_insns;
static _Atomic unsigned long long ps_steps, ps_hits, ps_misses;

static _Atomic unsigned long long ps_shape[9][2];
static const char *ps_shape_name[9] = { "push", "pop", "test", "movsxd", "call", "ret",
                                        "jmp", "jmpind", "jmpmem" };

static _Atomic unsigned long long ps_chain_ok, ps_chain_far;
static unsigned long long ps_ras_miss, ps_ras_stale;
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
    if (o < OCERZ_OP_COUNT) {
        ps_ops[o]++;
        if ((ps_ops[o] & 0xff) == 1) ps_note_shape(insn);
    }

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

__thread int ocerz_jit_exec_state;
int ocerz_jit_exec_one(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    if (__builtin_expect(insn->op == OCERZ_OP_SYSCALL && !insn->mode32 &&
                         cpu->gpr[OCERZ_RAX] == ((2ull << 24) | 2), 0)) {
        cpu->cur_rip = insn->rip;
        cpu->rip = insn->rip;
        return OCERZ_EUNSUP;
    }
    if (__builtin_expect(ocerz_perfstat > 0, 0))
        jit_perfstat_one(insn);
    vm->insn_count++;
    cpu->cur_rip = insn->rip;
    uint64_t next = insn->rip + insn->len;
    uint64_t m32mask = insn->mode32 ? 0xffffffffull : ~0ull;
    cpu->rip = next & m32mask;
    if (__builtin_expect(vm->trace != 0, 0))
        jit_trace_one(insn);
    int prev = ocerz_jit_exec_state;
    if (!prev) ocerz_jit_exec_state = 1;
    int r = ocerz_interp_exec(vm, cpu, insn);
    cpu->rip &= cpu->mode32 ? 0xffffffffull : ~0ull;
    ocerz_jit_exec_state = prev;
    return r;
}

static int ocerz_jit_exec_one_at(struct OcerzVM *vm, OcerzCPU *cpu, const JitBlock *b, uint64_t idx)
{
    return ocerz_jit_exec_one(vm, cpu, blk_insn_full(b, (int)idx));
}

static JitBlock *g_cur_blk;
static uint8_t *g_keep;
static int g_keep_cap, g_keep_n, g_no_compact;

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

static int term_may_switch_mode(unsigned op)
{
    return op == OCERZ_OP_IRET || op == OCERZ_OP_JMPF ||
           op == OCERZ_OP_CALLF || op == OCERZ_OP_RETF;
}

#define ENV_ON(name) ({ static int on_ = -1;                     \
                        if (on_ < 0) on_ = getenv(name) != NULL; \
                        on_; })

static unsigned hash_key(uint64_t key)
{
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdull;
    key ^= key >> 29;
    return (unsigned)(key & JIT_HASH_MASK);
}

static JitBlock *cache_lookup(OcerzJit *jit, uint64_t rip, int mode32)
{
    uint64_t key = jit_key(rip, mode32);
    for (JitBlock *b = __atomic_load_n(&jit->buckets[hash_key(key)], __ATOMIC_ACQUIRE);
         b; b = b->hnext)
        if (b->key == key)
            return b;
    {
        static int wl = -1; if (wl < 0) wl = getenv("OCERZ_WILDLOG") ? 1 : 0;
        if (wl && (rip >= 0x800000000000ull || rip < 0x10000ull)) {
            extern unsigned ocerz_vm_riphist(uint64_t *out, unsigned max);
            extern uint64_t ocerz_current_dbg_ind_src(void);
            extern uint64_t ocerz_current_guest_gpr(int);
            uint64_t h[8]; unsigned n = ocerz_vm_riphist(h, 8);
            static const char *rn[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"};
            fprintf(stderr, "ocerz: WILD-LOOKUP[%d] target=%#llx ind_src=%#llx riphist:",
                    (int)getpid(), (unsigned long long)rip, (unsigned long long)ocerz_current_dbg_ind_src());
            for (unsigned k = 0; k < n; k++) fprintf(stderr, " %#llx", (unsigned long long)h[k]);
            fprintf(stderr, "\n  WILD-GPR[%d]", (int)getpid());
            for (int g = 0; g < 16; g++) fprintf(stderr, " %s=%#llx", rn[g], (unsigned long long)ocerz_current_guest_gpr(g));
            fprintf(stderr, "\n"); fflush(stderr);
        }
    }
    return NULL;
}

static inline unsigned invmap_slot(uint64_t tag)
{
    uint64_t h = tag * 0x9e3779b97f4a7c15ull;
    return (unsigned)((h >> 40) & (INVMAP_SLOTS - 1));
}

static inline uint64_t invmap_mask(uint64_t region, uint64_t lo, uint64_t hi)
{
    uint64_t rlo = region << INVMAP_RSHIFT, rhi = rlo + (1ull << INVMAP_RSHIFT);
    uint64_t a = lo > rlo ? lo : rlo, b = hi < rhi ? hi : rhi;
    unsigned g0 = (unsigned)((a - rlo) >> INVMAP_GSHIFT);
    unsigned g1 = (unsigned)((b - 1 - rlo) >> INVMAP_GSHIFT);
    return g1 - g0 >= 63 ? ~0ull
                         : (((1ull << (g1 - g0 + 1)) - 1) << g0);
}

static void invmap_add(OcerzJit *jit, uint64_t lo, uint64_t hi)
{
    for (uint64_t r = lo >> INVMAP_RSHIFT; r <= (hi - 1) >> INVMAP_RSHIFT; r++) {
        uint64_t tag = r + 1, m = invmap_mask(r, lo, hi);
        unsigned i = invmap_slot(tag);
        int tomb = -1;
        for (unsigned n = 0; n < INVMAP_PROBE; n++, i = (i + 1) & (INVMAP_SLOTS - 1)) {
            if (jit->invmap[i].tag == INVMAP_TOMB) { if (tomb < 0) tomb = (int)i; continue; }
            if (jit->invmap[i].tag == 0) {
                if (tomb >= 0) i = (unsigned)tomb;
                jit->invmap[i].tag = tag;
            }
            if (jit->invmap[i].tag == tag) { jit->invmap[i].bits |= m; goto next; }
        }
        if (tomb >= 0) {
            jit->invmap[tomb].tag = tag;
            jit->invmap[tomb].bits = m;
            goto next;
        }
        jit->invmap_full = 1;
next:;
    }
}

static void invmap_clear_range(OcerzJit *jit, uint64_t lo, uint64_t hi)
{
    uint64_t g = 1ull << INVMAP_GSHIFT;
    uint64_t clo = (lo + g - 1) & ~(g - 1), chi = hi & ~(g - 1);
    if (clo >= chi || jit->invmap_full)
        return;
    if (((chi - 1) >> INVMAP_RSHIFT) - (clo >> INVMAP_RSHIFT) >= INVMAP_MAX_SPAN)
        return;
    for (uint64_t r = clo >> INVMAP_RSHIFT; r <= (chi - 1) >> INVMAP_RSHIFT; r++) {
        uint64_t tag = r + 1, m = invmap_mask(r, clo, chi);
        unsigned i = invmap_slot(tag);
        for (unsigned n = 0; n < INVMAP_PROBE; n++, i = (i + 1) & (INVMAP_SLOTS - 1)) {
            if (jit->invmap[i].tag == 0) break;
            if (jit->invmap[i].tag == tag) {
                jit->invmap[i].bits &= ~m;
                if (jit->invmap[i].bits == 0)
                    jit->invmap[i].tag = INVMAP_TOMB;
                break;
            }
        }
    }
}

static int invmap_may_hold(const OcerzJit *jit, uint64_t lo, uint64_t hi)
{
    uint64_t r0 = lo >> INVMAP_RSHIFT, r1 = (hi - 1) >> INVMAP_RSHIFT;
    if (ENV_ON("OCERZ_NO_INVMAP") || jit->invmap_full ||
        r1 - r0 >= INVMAP_MAX_SPAN)
        return 1;
    for (uint64_t r = r0; r <= r1; r++) {
        uint64_t tag = r + 1, m = invmap_mask(r, lo, hi);
        unsigned i = invmap_slot(tag);
        for (unsigned n = 0; n < INVMAP_PROBE; n++, i = (i + 1) & (INVMAP_SLOTS - 1)) {
            if (jit->invmap[i].tag == 0) break;
            if (jit->invmap[i].tag == tag) {
                if (jit->invmap[i].bits & m) return 1;
                break;
            }
        }
    }
    return 0;
}

#define GRAN_SLOTS 65536
#define GRAN4_SLOTS 16384
static struct { uint64_t page; int32_t count; } g_gran[GRAN_SLOTS];
static struct { uint64_t row; int32_t count; } g_gran4[GRAN4_SLOTS];
static int g_gran_degenerate;
static void gran4_bump(uint64_t row, int d)
{
    unsigned i = (unsigned)(row * 0x9E3779B97F4A7C15ull >> 50) & (GRAN4_SLOTS - 1);
    for (unsigned n = 0; n < 32; n++, i = (i + 1) & (GRAN4_SLOTS - 1)) {
        if (g_gran4[i].row == row || (g_gran4[i].row == 0 && g_gran4[i].count == 0)) {
            g_gran4[i].row = row;
            g_gran4[i].count += d;
            return;
        }
    }
    g_gran_degenerate = 1;
}
static int gran4_count(uint64_t row)
{
    unsigned i = (unsigned)(row * 0x9E3779B97F4A7C15ull >> 50) & (GRAN4_SLOTS - 1);
    for (unsigned n = 0; n < 32; n++, i = (i + 1) & (GRAN4_SLOTS - 1)) {
        if (g_gran4[i].row == row) return g_gran4[i].count;
        if (g_gran4[i].row == 0 && g_gran4[i].count == 0) return 0;
    }
    return 1;
}
static void gran_bump(uint64_t rip, int d)
{
    uint64_t page = rip >> INVMAP_GSHIFT;
    gran4_bump(page >> 6, d);
    unsigned i = (unsigned)(page * 0x9E3779B97F4A7C15ull >> 48) & (GRAN_SLOTS - 1);
    for (unsigned n = 0; n < 32; n++, i = (i + 1) & (GRAN_SLOTS - 1)) {
        if (g_gran[i].page == page || (g_gran[i].page == 0 && g_gran[i].count == 0)) {
            g_gran[i].page = page;
            g_gran[i].count += d;
            return;
        }
    }
    g_gran_degenerate = 1;
}
static int gran_fine_any(uint64_t p0, uint64_t p1)
{
    for (uint64_t p = p0; p <= p1; p++) {
        unsigned i = (unsigned)(p * 0x9E3779B97F4A7C15ull >> 48) & (GRAN_SLOTS - 1);
        for (unsigned n = 0; n < 32; n++, i = (i + 1) & (GRAN_SLOTS - 1)) {
            if (g_gran[i].page == p) { if (g_gran[i].count > 0) return 1; break; }
            if (g_gran[i].page == 0 && g_gran[i].count == 0) break;
        }
    }
    return 0;
}
static int gran_any(uint64_t lo, uint64_t hi)
{
    if (g_gran_degenerate) return 1;
    uint64_t p0 = lo >> INVMAP_GSHIFT, p1 = (hi - 1) >> INVMAP_GSHIFT;
    if (p1 - p0 < 64)
        return gran_fine_any(p0, p1);
    for (uint64_t r = p0 >> 6; r <= p1 >> 6; r++) {
        if (gran4_count(r) <= 0) continue;
        uint64_t f0 = r << 6, f1 = f0 + 63;
        if (f0 < p0) f0 = p0;
        if (f1 > p1) f1 = p1;
        if (gran_fine_any(f0, f1)) return 1;
    }
    return 0;
}
static void gran_block(JitBlock *b, int d)
{
    if (b->n_insns <= 0) return;
    uint64_t lo = blk_insn_rip(b, 0), hi = lo + blk_insn_len(b, 0);
    for (int i = 1; i <= b->n_insns; i++) {
        if (i < b->n_insns && blk_insn_rip(b, i) == hi) { hi += blk_insn_len(b, i); continue; }
        for (uint64_t p = lo >> INVMAP_GSHIFT; p <= (hi - 1) >> INVMAP_GSHIFT; p++)
            gran_bump(p << INVMAP_GSHIFT, d);
        if (i < b->n_insns) { lo = blk_insn_rip(b, i); hi = lo + blk_insn_len(b, i); }
    }
}
static void gran_clear_all(void)
{
    memset(g_gran, 0, sizeof g_gran);
    memset(g_gran4, 0, sizeof g_gran4);
    g_gran_degenerate = 0;
}

static void shrink_edges(JitBlock *b)
{
    unsigned n = b->n_edges ? b->n_edges : 1;
    if (b->edges && n < JIT_MAX_EDGES) {
        void *p = realloc(b->edges, n * sizeof *b->edges);
        if (p) b->edges = p;
    }
}

static void cache_insert(OcerzJit *jit, JitBlock *b)
{
    shrink_edges(b);
    unsigned h = hash_key(b->key);
    b->hnext = jit->buckets[h];
    __atomic_store_n(&jit->buckets[h], b, __ATOMIC_RELEASE);
    if (jit->n_live == jit->cap_live) {
        size_t nc = jit->cap_live ? jit->cap_live * 2 : 4096;
        JitBlock **nl = (JitBlock **)realloc(jit->live, nc * sizeof *nl);
        if (nl) { jit->live = nl; jit->cap_live = nc; }
    }
    if (jit->n_live < jit->cap_live) jit->live[jit->n_live++] = b;
    gran_block(b, +1);
    if (b->n_insns > 0) {
        uint64_t lo = blk_insn_rip(b, 0);
        uint64_t hi = lo + blk_insn_len(b, 0);
        for (int i = 1; i <= b->n_insns; i++) {
            if (i < b->n_insns && blk_insn_rip(b, i) == hi) {
                hi += blk_insn_len(b, i);
                continue;
            }
            if (!jit->code_hi) { jit->code_lo = lo; jit->code_hi = hi; }
            else {
                if (lo < jit->code_lo) jit->code_lo = lo;
                if (hi > jit->code_hi) jit->code_hi = hi;
            }
            invmap_add(jit, lo, hi);
            ocerz_cache_arm_exec(lo, hi);
            ocerz_mem_arm_exec(lo, hi);
            if (i < b->n_insns) { lo = blk_insn_rip(b, i); hi = lo + blk_insn_len(b, i); }
        }
    }
}

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
static void chaincheck(const char *what, const void *dst);
static _Atomic unsigned long long g_ic_miss_calls, g_ic_fills, g_ic_nocode;
void ocerz_jit_ic_fill(struct OcerzVM *vm, OcerzCPU *cpu, JitIcSlot *slot)
{
    OcerzJit *jit = vm->jit;
    if (!jit || !slot)
        return;
    g_ic_miss_calls++;
    JitBlock *t = cache_lookup(jit, cpu->rip, cpu->mode32);
    if (!t || !t->code) g_ic_nocode++; else g_ic_fills++;
    if (t && t->code) {
        chaincheck("ic_fill", (void *)t->code);
        __atomic_store_n(&slot->code, (void *)t->code, __ATOMIC_RELAXED);
        __atomic_store_n(&slot->rip, cpu->rip, __ATOMIC_RELEASE);
    }
}

static uint64_t xlive_decode_entry_d(uint64_t rip, int depth);
static int g_xlive_log = -1;

static uint64_t xlive_succ_live_d(OcerzJit *jit, uint64_t rip, int depth);
static void fpb_site_emit(A64Buf *b, int end, int va, int vb, int dbl);
static int unsafe_nocheckbr(void);
static int fpb_det_here(int idx);
static uint64_t xlive_decode_entry_d(uint64_t rip, int depth)
{
    static int maxd = -1;
    if (maxd < 0) { const char *e = getenv("OCERZ_XLIVE_DEPTH"); maxd = e ? atoi(e) : 3; }
    if (g_xlive_log < 0) g_xlive_log = getenv("OCERZ_XLIVELOG") ? 1 : 0;
    X86Insn insns[JIT_MAX_BLOCK_INSNS];
    volatile int n = 0;
    volatile uint64_t pc = rip;
    sigjmp_buf db;
    sigjmp_buf *prev = ocerz_jit_decode_recover;
    if (sigsetjmp(db, 0) == 0) {
        ocerz_jit_decode_recover = &db;
        while (n < JIT_MAX_BLOCK_INSNS) {
            int rc = ocerz_decode_mode((const uint8_t *)ocerz_g2h(pc), 15, pc,
                                       &insns[n], g_xlat_mode32);
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
    return live;
}

static int canonical_body_successor(uint64_t rip)
{
    X86Insn insn;
    volatile uint64_t pc = rip;
    volatile int compatible = 0;
    sigjmp_buf db;
    sigjmp_buf *prev = ocerz_jit_decode_recover;
    if (sigsetjmp(db, 0) == 0) {
        ocerz_jit_decode_recover = &db;
        for (int n = 0; n < JIT_MAX_BLOCK_INSNS; n++) {
            if (ocerz_decode_mode((const uint8_t *)ocerz_g2h(pc), 15, pc,
                                  &insn, g_xlat_mode32) != OCERZ_OK)
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

static unsigned decoded_terminator(uint64_t rip)
{
    X86Insn insn;
    volatile uint64_t pc = rip;
    volatile unsigned term = 0;
    sigjmp_buf db;
    sigjmp_buf *prev = ocerz_jit_decode_recover;
    if (sigsetjmp(db, 0) == 0) {
        ocerz_jit_decode_recover = &db;
        for (int n = 0; n < JIT_MAX_BLOCK_INSNS; n++) {
            if (ocerz_decode_mode((const uint8_t *)ocerz_g2h(pc), 15, pc,
                                  &insn, g_xlat_mode32) != OCERZ_OK)
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
    if (sigsetjmp(db, 0) == 0) {
        ocerz_jit_decode_recover = &db;
        for (int n = 0; n < JIT_MAX_BLOCK_INSNS; n++) {
            if (ocerz_decode_mode((const uint8_t *)ocerz_g2h(pc), 15, pc,
                                  &insn, g_xlat_mode32) != OCERZ_OK)
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

static uint64_t xlive_succ_live_d(OcerzJit *jit, uint64_t rip, int depth)
{
    JitBlock *t = jit ? cache_lookup(jit, rip, g_xlat_mode32) : NULL;
    if (g_xlive_log < 0) g_xlive_log = getenv("OCERZ_XLIVELOG") ? 1 : 0;
    if (t && t->code && g_xlive_log) fprintf(stderr, "ocerz: XLIVE cached rip=%#llx entry_live=%#x n_insns=%d\n", (unsigned long long)rip, t->entry_live, t->n_insns);
    return (t && t->code) ? (uint64_t)t->entry_live : xlive_decode_entry_d(rip, depth);
}
static uint64_t xlive_succ_live(OcerzJit *jit, uint64_t rip) { return xlive_succ_live_d(jit, rip, 0); }

static int probe_wanted(uint64_t jcc_rip, uint64_t ft_rip)
{
    if (flip_disabled() || g_n_probes >= PROBE_MAX) return 0;
    if (flip_state(jcc_rip) != FLIP_NONE) return 0;
    if (xlive_succ_live(g_xlat_jit, ft_rip) != 0) return 0;
    g_n_probes++;
    return 1;
}

static void emit_slowcall(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);

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
#define RAS_OFF ((uint32_t)offsetof(OcerzCPU, ras))
#define JIT_FP_OFF ((uint32_t)offsetof(OcerzCPU, jit_fp))
static int host_ras_enabled(void)
{
    static int en = -1;
    if (en < 0) en = getenv("OCERZ_NO_HOST_RAS") ? 0 : 1;
    return en;
}
static void emit_frame_sp_reset(A64Buf *b)
{
    if (g_pin_class == 3 && host_ras_enabled()) {
        a64_ldr(b, 8, 15, 20, JIT_FP_OFF);
        a64_add_imm(b, 1, 31, 15, 0);
    }
}

enum { JT0 = 9, JT1 = 10, JT2 = 11, JTF = 12, JTT = 13, JTU = 14, JTA = 15 };
static void emit_push_pinned(A64Buf *b, int hs, int rv)
{
    if ((stack_identity() || rsp_is_ptr()) && rv != hs) {
        a64_str_pre64(b, rv, hs, -8);
        return;
    }
    if (g_push_entry && g_n_push_fix < JIT_MAX_BLOCK_INSNS && rv != hs) {
        a64_sub_imm(b, 1, hs, hs, 8);
        g_push_fix[g_n_push_fix++] = (uint32_t)(a64_label(b) - g_push_entry);
        a64_str_regoff(b, 8, rv, JGB, hs, 0);
        return;
    }
    a64_sub_imm(b, 1, JTA, hs, 8);
    a64_str_regoff(b, 8, rv, JGB, JTA, 0);
    a64_mov_reg(b, 1, hs, JTA);
}
static void a64_and_imm_or_mov(A64Buf *b, int sf, int rd, int rn, uint64_t imm)
{
    if (!a64_try_and_imm(b, sf, rd, rn, imm)) { a64_mov_imm64(b, JTT, imm); a64_and_reg(b, sf, rd, rn, JTT, 0); }
}

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
    emit_xmm_pin_spill_all(b);
    emit_spill_pinned_callersaved(b);
    a64_mov_reg(b, 1, 0, 20);
    a64_mov_imm64(b, 16, (uint64_t)(uintptr_t)&ocerz_flags_materialize);
    a64_blr(b, 16);
    g_callout_seq++;
    emit_fill_pinned_callersaved(b);
    emit_reload_jgb(b);
    emit_reload_mem_base(b);
    emit_xmm_pin_load_all(b);
    a64_patch_cbz(skip, a64_label(b));
}

static inline int pin_slot(unsigned greg)
{
    return (g_pin && greg < 16) ? g_pin[greg] : -1;
}

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
    if (s >= 0 && rsp_is_ptr() && greg == OCERZ_RSP) {
        if (jgb_usable())
            a64_sub_reg(b, 1, dst, pin_hreg(s), JGB, 0);
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
    if (s >= 0 && rsp_is_ptr() && greg == OCERZ_RSP) {
        if (jgb_usable())
            a64_add_reg(b, 1, pin_hreg(s), src, JGB, 0);
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

static void emit_spill_pinned(A64Buf *b)
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
    if (rsp_is_ptr()) {
        int s = pin_slot(OCERZ_RSP);
        assert(s >= 0);
        a64_mov_imm64(b, JTA, ocerz_guest_base);
        a64_add_reg(b, 1, pin_hreg(s), pin_hreg(s), JTA, 0);
    }
}

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

static const uint32_t *g_cur_insn_start;
extern const struct X86Insn *g_cur_insns_fwd(void);
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

static int emit_mem_ea(A64Buf *b, const X86Insn *insn, const X86Operand *op, int addr_reg);
static int emit_mem_load_plain(A64Buf *b, const X86Insn *insn, const X86Operand *op, int size, int rd);
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

static void emit_cc_predicate(A64Buf *b, unsigned cc);
static inline int xmm_vreg(unsigned xr);
static inline int xmm_is_pinned(unsigned xr);
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

static int emit_incdec(A64Buf *b, const X86Insn *insn, uint64_t need)
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

static int emit_mov_logic_pair(A64Buf *b, const X86Insn *mov,
                               const X86Insn *logic, uint64_t logic_need,
                               uint32_t **logic_label)
{
    if (g_xlat_mode32)
        return 0;
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
    if (g_xlat_mode32)
        return 0;
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

static int emit_mul_wide(A64Buf *b, const X86Insn *insn, uint64_t need, int is_signed)
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
        if (!emit_mem_load_plain(b, insn, o, o->size, JT1)) return 0;
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
            for (int i = 0; i < 2; i++) {
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
    if (insn->addrsize == 4) {
        if (!insn->mode32)
            return 0;
        return emit_mem_ea32(b, insn, op, addr_reg);
    }
    if (insn->addrsize != 8)
        return 0;
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

static uint32_t *emit_commpage_guard(A64Buf *b, const X86Insn *insn,
                                     int addr_reg, uint32_t **exit_sites, int *n_exits)
{
    (void)insn; (void)exit_sites; (void)n_exits;
    if (!ocerz_commpage && !ocerz_low_base)
        return NULL;

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
        a64_mov_imm64(b, JTU, (uint64_t)(uintptr_t)ocerz_commpage - OCERZ_COMMPAGE_LO - ocerz_guest_base);
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
    return NULL;
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

static void emit_gpr_ld_at(A64Buf *b, int size, int rd, int ra, int32_t disp, int plain);
static void emit_gpr_st_at(A64Buf *b, int size, int rs, int ra, int32_t disp, int plain);
static void emit_gpr_ld_regoff(A64Buf *b, int size, int rd, int ra, int ri, int scaled, int plain);
static void emit_gpr_st_regoff(A64Buf *b, int size, int rs, int ra, int ri, int scaled, int plain);
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

static void emit_granule_cross_test(A64Buf *b, int size, int ra, int scratch)
{
    a64_add_imm(b, 1, scratch, ra, (uint32_t)(size - 1));
    a64_eor_reg(b, 1, scratch, scratch, ra, 0);
    a64_try_ands_imm(b, 1, A64_ZR, scratch, 16);
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

static void emit_guest_load_ordered(A64Buf *b, int size, int rd, int ra, int scratch)
{
    if (g_plain_mem) {
        a64_ldr(b, size, rd, ra, 0);
        return;
    }
    g_blk_ordered_loads = 1;
    if (size == 1) {
        if (g_no_ldapr) a64_ldar(b, 1, rd, ra);
        else            a64_ldapr(b, 1, rd, ra);
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

static void emit_gpr_ld_at(A64Buf *b, int size, int rd, int ra, int32_t disp, int plain)
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
static void emit_gpr_st_at(A64Buf *b, int size, int rv, int ra, int32_t disp, int plain)
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
static void emit_v_ld_at(A64Buf *b, int size, int vd, int ra, int32_t disp, int plain)
{
    int scaled = disp >= 0 && (disp % size) == 0 && disp / size <= 4095;
    if (!plain && vec_tso_relaxed()) plain = 1;
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
static void emit_v_st_at(A64Buf *b, int size, int vs, int ra, int32_t disp, int plain)
{
    int scaled = disp >= 0 && (disp % size) == 0 && disp / size <= 4095;
    if (!plain && vec_tso_relaxed()) plain = 1;
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
    if (plain || vec_tso_relaxed()) { a64_ldr_v_regoff(b, size, vd, ra, ri, scaled); return; }
    int sh = scaled ? (size == 16 ? 4 : size == 8 ? 3 : 2) : 0;
    a64_add_reg(b, 1, JTA, ra, ri, sh);
    emit_v_ld_at(b, size, vd, JTA, 0, 0);
}
static void emit_v_st_regoff(A64Buf *b, int size, int vs, int ra, int ri, int scaled, int plain)
{
    if (plain || vec_tso_relaxed()) { a64_str_v_regoff(b, size, vs, ra, ri, scaled); return; }
    int sh = scaled ? (size == 16 ? 4 : size == 8 ? 3 : 2) : 0;
    a64_add_reg(b, 1, JTA, ra, ri, sh);
    emit_v_st_at(b, size, vs, JTA, 0, 0);
}

static int ea_cache_reusable(const A64Buf *b, const X86Operand *op);
static int ea_cache_has_base(const A64Buf *b, const X86Operand *op);
static void ea_cache_set(const A64Buf *b, const X86Operand *op);
static void ea_cache_set_full(const A64Buf *b, unsigned base, unsigned index, int scale);
static void ea_cache_reset(void);
static int emit_plain_mem_fast(A64Buf *b, const X86Insn *insn, const X86Operand *m,
                               int size, int reg, int store, int vec)
{
    static int dis = -1; if (dis < 0) dis = getenv("OCERZ_NO_PLAINFAST") ? 1 : 0;
    if (dis) return 0;
    if (!mem_fast_forms_ok()) return 0;
    if (insn->seg != OCERZ_SEG_NONE || insn->addrsize != 8 || m->riprel) return 0;
    if (m->base == OCERZ_REG_NONE || pin_slot(m->base) < 0) return 0;
    if (rsp_is_ptr() && (m->base == OCERZ_RSP || m->index == OCERZ_RSP)) return 0;
    int plain = mem_plain_access_ok(m);
    X86Operand mview; m = mem_hoist_view(m, &mview);
    int hb = pin_hreg(pin_slot(m->base));
    int hreg = hoist_reg_for(m->base);
    int hoisted = hreg >= 0;
#define ACC_AT(ra, d)  do { if (vec) { if (store) emit_v_st_at(b, size, reg, (ra), (int32_t)(d), plain); else emit_v_ld_at(b, size, reg, (ra), (int32_t)(d), plain); } \
                            else     { if (store) emit_gpr_st_at(b, size, reg, (ra), (int32_t)(d), plain); else emit_gpr_ld_at(b, size, reg, (ra), (int32_t)(d), plain); } } while (0)
#define ACC_REGOFF(ra, ri, sc) do { if (vec) { if (store) emit_v_st_regoff(b, size, reg, (ra), (ri), (sc), plain); else emit_v_ld_regoff(b, size, reg, (ra), (ri), (sc), plain); } \
                                    else     { if (store) emit_gpr_st_regoff(b, size, reg, (ra), (ri), (sc), plain); else emit_gpr_ld_regoff(b, size, reg, (ra), (ri), (sc), plain); } } while (0)
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

static const X86Insn *g_cur_insns;
static int g_cur_insns_n;
const struct X86Insn *g_cur_insns_fwd(void) { return g_cur_insns; }
static int insn_may_write_gpr(const X86Insn *in, unsigned reg);
static struct {
    int valid; unsigned base, index; int scale;
    unsigned long long seq;
    const uint32_t *after;
} g_ea_cache;
static int g_cur_fpb = -1;
static int g_fcmp_self_vreg = -1;
static int g_fcmp_self_idx = -1;
static void ea_cache_reset(void) { g_ea_cache.valid = 0; }
static int a64_word_may_write_x15(uint32_t w)
{
    if ((w & 0x1f) == 15) return 1;
    if ((w & 0x3a000000u) == 0x28000000u && (w & 0x00400000u)) { if (((w >> 10) & 0x1f) == 15) return 1; }
    if ((w & 0x3f000000u) == 0x08000000u && ((w >> 10) & 0x1f) == 15) return 1;
    return 0;
}
static int ea_cache_usable(const A64Buf *b)
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
static int ea_cache_has_base(const A64Buf *b, const X86Operand *op)
{
    if (!ea_cache_usable(b)) return 0;
    return g_ea_cache.base == op->base && op->base != OCERZ_REG_NONE && g_ea_cache.index == OCERZ_REG_NONE;
}
static void ea_cache_set_full(const A64Buf *b, unsigned base, unsigned index, int scale)
{
    g_ea_cache.valid = 1; g_ea_cache.base = base; g_ea_cache.index = index;
    g_ea_cache.scale = scale & 3; g_ea_cache.seq = g_callout_seq; g_ea_cache.after = b->p;
}
static void ea_cache_set(const A64Buf *b, const X86Operand *op) { ea_cache_set_full(b, op->base, op->index, op->scale & 3); }
static void ea_cache_step(const X86Insn *in, const X86Insn *prev)
{
    if (!g_ea_cache.valid) return;
    if (g_ea_cache.base != OCERZ_REG_NONE &&
        (insn_may_write_gpr(in, g_ea_cache.base) || (prev && insn_may_write_gpr(prev, g_ea_cache.base)))) { g_ea_cache.valid = 0; return; }
    if (g_ea_cache.index != OCERZ_REG_NONE &&
        (insn_may_write_gpr(in, g_ea_cache.index) || (prev && insn_may_write_gpr(prev, g_ea_cache.index)))) { g_ea_cache.valid = 0; return; }
}

static int emit_mem_ea_plain_ex(A64Buf *b, const X86Insn *insn, const X86Operand *op,
                                int size, int *ra_out, uint32_t *disp_out, int unscaled_ok);
static int emit_mem_ea_plain(A64Buf *b, const X86Insn *insn, const X86Operand *op,
                             int size, int *ra_out, uint32_t *disp_out)
{
    return emit_mem_ea_plain_ex(b, insn, op, size, ra_out, disp_out, 0);
}
static int emit_mem_ea_plain_ex(A64Buf *b, const X86Insn *insn, const X86Operand *op,
                                int size, int *ra_out, uint32_t *disp_out, int unscaled_ok)
{
    if (!mem_fast_forms_ok()) return 0;
    if (insn->seg != OCERZ_SEG_NONE || insn->addrsize != 8) return 0;
    if (rsp_is_ptr() && (op->base == OCERZ_RSP || op->index == OCERZ_RSP)) return 0;
    if (op->riprel) {
        uint64_t c = (uint64_t)op->disp + ocerz_guest_base;
        static int nolit = -1; if (nolit < 0) nolit = getenv("OCERZ_NO_RIPLIT") ? 1 : 0;
        if (!nolit && g_n_raslit < RASLIT_MAX && (c >> 32) != 0 && ((c >> 16) & 0xffff) != 0) {
            g_raslit[g_n_raslit].site = a64_label(b);
            g_raslit[g_n_raslit].retaddr = c;
            g_raslit[g_n_raslit].kind = 1;
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
static int emit_mem_load_plain(A64Buf *b, const X86Insn *insn, const X86Operand *op, int size, int rd)
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

static int emit_mov_mem(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
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
        if (v != 0) a64_mov_imm64(b, JT1, v);
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
                    emit_gpr_ld_at(b, s->size, JT1, ra, (int32_t)disp, 0);
                    if (s->size == 1) a64_sxtb(b, sf, pin_hreg(ds), JT1);
                    else              a64_sxth(b, sf, pin_hreg(ds), JT1);
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

static int stack_inline_enabled(void)
{
    static int en = -1;
    if (en < 0)
        en = getenv("OCERZ_NO_INLINE_STACK") ? 0 : 1;
    return en;
}

static int m32_stack_ok(const X86Insn *insn)
{
    return stack_inline_enabled() && insn->seg == OCERZ_SEG_NONE &&
           g_pin_class != 2 && pin_slot(OCERZ_RSP) >= 0 &&
           jgb_usable() && !mem_guard_needed() &&
           stack_plain_access_ok() && !stack_guard_needed();
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
        a64_str_regoff_uxtw(b, 4, rv, JGB, JTA);
        a64_mov_reg(b, 0, hs, JTA);
        return 1;
    }

    if (insn->op == OCERZ_OP_POP) {
        if (o->kind != OCERZ_OPK_REG || o->high8 || o->size != 4)
            return 0;
        if (o->reg == OCERZ_RSP) {
            a64_ldr_regoff_uxtw(b, 4, hs, JGB, hs);
            return 1;
        }
        int ds = pin_slot(o->reg);
        int rd = ds >= 0 ? pin_hreg(ds) : JT1;
        a64_ldr_regoff_uxtw(b, 4, rd, JGB, hs);
        a64_add_imm(b, 0, hs, hs, 4);
        if (ds < 0)
            emit_gpr_wr(b, JT1, o->reg);
        return 1;
    }
    return 0;
}

static int emit_leave32(A64Buf *b, const X86Insn *insn)
{
    if ((insn->opsize ? insn->opsize : 4) != 4)
        return 0;
    if (!m32_stack_ok(insn) || pin_slot(OCERZ_RBP) < 0)
        return 0;
    int hs = pin_hreg(pin_slot(OCERZ_RSP)), hb = pin_hreg(pin_slot(OCERZ_RBP));
    a64_mov_reg(b, 0, hs, hb);
    a64_ldr_regoff_uxtw(b, 4, hb, JGB, hs);
    a64_add_imm(b, 0, hs, hs, 4);
    return 1;
}

static int emit_push_pop(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
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
            else { emit_gpr_ld_at(b, 4, JT1, ra, (int32_t)disp, 0); a64_sxtw(b, pin_hreg(ds), JT1); }
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
static uint64_t g_cur_need;
static int sse_enabled(void);
static int cc_consumer_inline_ok(const X86Insn *c)
{
    const X86Operand *d = &c->ops[0];
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
        return sr->kind == OCERZ_OPK_MEM;
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
static int comis_fuse_producer(const X86Insn *insns, int ci)
{
    if (g_xlat_mode32) return -1;
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
    if (p->ops[1].kind != OCERZ_OPK_XMM || !xmm_is_pinned(p->ops[1].reg)) return -1;
    for (int k = pi + 1; k < ci; k++) {
        const X86Insn *m = &insns[k];
        if (m->nops > 0 && m->ops[0].kind == OCERZ_OPK_XMM &&
            (m->ops[0].reg == p->ops[0].reg || m->ops[0].reg == p->ops[1].reg))
            return -1;
        uint64_t mdef, muse;
        ocerz_flags_defuse_nofault(m, &mdef, &muse);
        if (!(mdef & JIT_ARITH_FLAGS) && (muse & JIT_ARITH_FLAGS) == JIT_ARITH_FLAGS)
            return -1;
    }
    return pi;
}
static int insn_may_write_gpr(const X86Insn *in, unsigned reg);
static int value_cond_fuse_producer(const X86Insn *insns, int ci)
{
    static int dis = -1;
    if (g_xlat_mode32) return -1;
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
static int cc_after_ands(unsigned cc);
static int cc_after_adds(unsigned cc);
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
static int nzcv_fuse_producer(const X86Insn *insns, int ci);
static int insn_writes_reg(const X86Insn *in, unsigned reg);
static int flag_neutral_ok(const X86Insn *in);
#define NZCV_GAP_MAX 3
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
static int nzcv_dc_for(unsigned kind, unsigned cc);
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
static int nzcv_fuse_producer(const X86Insn *insns, int ci)
{
    static int dis = -1;
    if (g_xlat_mode32) return -1;
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

static int g_cc_direct = -1;
static int g_cc_want_cbz;
static int g_cc_cbz_reg = -1, g_cc_cbz_sf, g_cc_cbz_nz;
static void emit_cc_predicate_ex(A64Buf *b, unsigned cc, int want_direct);
static void emit_cc_predicate(A64Buf *b, unsigned cc)
{
    emit_cc_predicate_ex(b, cc, 0);
}
static int nzcv_dc_for(unsigned kind, unsigned cc)
{
    return kind == OCERZ_CC_SUB ? cc_after_subs(cc) :
           kind == OCERZ_CC_ADD ? cc_after_adds(cc) :
           kind == NZCV_KIND_BT ? (cc == OCERZ_CC_B ? A64_NE : cc == OCERZ_CC_AE ? A64_EQ : -1) :
           cc_after_ands(cc);
}
static void emit_cc_predicate_ex(A64Buf *b, unsigned cc, int want_direct)
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
            int va = l0_src(g_flag_producer->ops[0].reg, dbl), vb = l0_src(g_flag_producer->ops[1].reg, dbl);
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

static int emit_adc_sbb(A64Buf *b, const X86Insn *insn, uint64_t need)
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

static int emit_arith_narrow(A64Buf *b, const X86Insn *insn, uint64_t need)
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

static int emit_cbw_cwd(A64Buf *b, const X86Insn *insn)
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

static int oolslow_add(const X86Insn *insn, uint32_t **sites, int nsites, uint32_t *back);
static void patch_any_branch(uint32_t *site, uint32_t *target);
static int g_div_prev_skipped;
static uint32_t g_oolslow_pre;
static int rdx_prep_skippable(const X86Insn *insns, int i, int n, uint64_t need)
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
static int emit_div(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
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

static int emit_not_neg(A64Buf *b, const X86Insn *insn, uint64_t need)
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

static int emit_rot(A64Buf *b, const X86Insn *insn, uint64_t need)
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

static int emit_shift_cl(A64Buf *b, const X86Insn *insn, uint64_t need)
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

static int emit_sse_mem_addr(A64Buf *b, const X86Insn *insn, const X86Operand *o, int size,
                             uint32_t **exit_sites, int *n_exits, uint32_t **skip_out);
static void emit_sse_mem_ld_gpr(A64Buf *b, int size, int rd);
static int emit_cmov(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
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

static int emit_setcc(A64Buf *b, const X86Insn *insn)
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

static int emit_bswap(A64Buf *b, const X86Insn *insn)
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

#define XMM_BASE_OFF ((uint32_t)offsetof(OcerzCPU, xmm))
enum { VX0 = 0, VX1 = 1, VX2 = 2, VX3 = 3 };

static uint16_t g_xmm_pinned;
static int xmm_pinning_enabled(void)
{
    static int on = -1;
    if (on < 0) on = getenv("OCERZ_NO_XMM_PIN") ? 0 : 1;
    return on;
}
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
    emit_pk_consts_load(b);
}
static void emit_xmm_pin_spill_all(A64Buf *b)
{
    for (unsigned r = 0; r < 16; r++)
        if (xmm_is_pinned(r))
            a64_str_v(b, 16, xmm_vreg(r), 20, XMM_BASE_OFF + r * 16);
}

_Static_assert(offsetof(OcerzCPU, xmm) % 16 == 0, "xmm must be 16-aligned for scaled q loads");
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

static int g_sse_mem_ra = JTA;
static uint32_t g_sse_mem_disp;
static int g_sse_mem_plain;
static int g_sse_mem_plainacc;
static int emit_sse_mem_addr(A64Buf *b, const X86Insn *insn, const X86Operand *o, int size,
                             uint32_t **exit_sites, int *n_exits, uint32_t **skip_out)
{
    g_sse_mem_plainacc = mem_plain_access_ok(o);
    if (emit_mem_ea_plain(b, insn, o, size, &g_sse_mem_ra, &g_sse_mem_disp)) {
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
static void emit_sse_mem_ld_gpr(A64Buf *b, int size, int rd)
{
    if (g_sse_mem_plain) emit_gpr_ld_at(b, size, rd, g_sse_mem_ra, (int32_t)g_sse_mem_disp, g_sse_mem_plainacc);
    else emit_guest_load_ordered(b, size, rd, JTA, JTU);
}
static void emit_sse_mem_ld(A64Buf *b, int size, int vd)
{
    emit_v_ld_at(b, size, vd, g_sse_mem_ra, (int32_t)g_sse_mem_disp, g_sse_mem_plainacc);
}
static void emit_sse_mem_st(A64Buf *b, int size, int vs)
{
    emit_v_st_at(b, size, vs, g_sse_mem_ra, (int32_t)g_sse_mem_disp, g_sse_mem_plainacc);
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

static int sse_enabled(void)
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
            if (xmm_is_pinned(s->reg)) a64_ins_d_d(b, VX0, 0, xmm_vreg(s->reg), hi ? 1 : 0);
            else { emit_xmm_ld(b, VX0, s->reg); if (hi) a64_ins_d_d(b, VX0, 0, VX0, 1); }
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
        if (!emit_sse_mem_addr(b, insn, s, size, exit_sites, n_exits, &skip)) return 0;
        emit_sse_mem_ld(b, size, VX0);
        patch_guard_skip(skip, a64_label(b));
        emit_xmm_st(b, VX0, d->reg);
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
static struct { int valid, idx, dbl, vr, va, vb; } g_scpend;
static int g_scalar_merge_next;
static int scalar_cvt_follows(unsigned xreg, int dbl)
{
    if (!dbl || !g_cur_insns || g_cur_insn_idx < 0 || g_cur_insn_idx + 1 >= g_cur_insns_n) return 0;
    if (g_n_nanool + 2 > NANOOL_MAX || unsafe_nocheckbr()) return 0;
    const X86Insn *c = &g_cur_insns[g_cur_insn_idx + 1];
    if (c->op != OCERZ_OP_CVTTSD2SI || c->nops != 2) return 0;
    const X86Operand *d = &c->ops[0], *sr = &c->ops[1];
    if (sr->kind != OCERZ_OPK_XMM || sr->reg != xreg || !xmm_is_pinned(xreg)) return 0;
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
    if (rsp_is_ptr() && d->reg == OCERZ_RSP) return 0;
    return 1;
}
static void emit_nan_fix_scalar2(A64Buf *b, int dbl, int vr, int va, int vb)
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
static void scalar_pend_flush(A64Buf *b)
{
    if (!g_scpend.valid) return;
    g_scpend.valid = 0;
    g_scalar_merge_next = 0;
    emit_nan_fix_scalar2(b, g_scpend.dbl, g_scpend.vr, g_scpend.va, g_scpend.vb);
}
static int g_pk_consts_needed;
static void emit_pk_consts_load(A64Buf *b)
{
    return;
    if (!g_pk_consts_needed) return;
    a64_mov_imm64(b, JT0, 0x0008000000000000ull); a64_fmov_v_from_x(b, 1, 4, JT0); a64_v_dup_d(b, 4, 4, 0);
    a64_mov_imm64(b, JT0, 0xfff8000000000000ull); a64_fmov_v_from_x(b, 1, 5, JT0); a64_v_dup_d(b, 5, 5, 0);
    a64_mov_imm64(b, JT0, 0x00400000ull);         a64_fmov_v_from_x(b, 0, 6, JT0); a64_v_dup_s(b, 6, 6, 0);
    a64_mov_imm64(b, JT0, 0xffc00000ull);         a64_fmov_v_from_x(b, 0, 7, JT0); a64_v_dup_s(b, 7, 7, 0);
}
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

static void emit_nan_ool_arms(A64Buf *b, JitBlock *blk, const uint32_t *entry)
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

#define FPB_MAX 32
typedef struct {
    int first, last;
    uint16_t ckpt;
    uint16_t full, s0, d0;
    int gain;
    uint32_t *site;
    uint32_t *back;
    int8_t l0[16];
    uint8_t l0_dbl[16];
    int8_t fcmp_vreg;
    int end;
} FpBatch;
static FpBatch g_fpb[FPB_MAX];
static int g_n_fpb;

#define FPB_SITES_MAX 128
typedef struct {
    int batch, end;
    uint32_t *site, *back;
    int8_t l0[16]; uint8_t l0_dbl[16];
    int8_t fcmp_a, fcmp_b; uint8_t fcmp_dbl;
} FpbSite;
static FpbSite g_fpb_sites[FPB_SITES_MAX];
static int g_n_fpb_sites;
static uint8_t g_fpb_member[JIT_MAX_BLOCK_INSNS];
static uint8_t g_fpb_det[JIT_MAX_BLOCK_INSNS];
static uint16_t g_fpb_sidechk[JIT_MAX_BLOCK_INSNS];
static uint16_t g_fpb_mrd[JIT_MAX_BLOCK_INSNS], g_fpb_mwr[JIT_MAX_BLOCK_INSNS];

static int mov_sink_gap_ok(const X86Insn *in, unsigned dreg, unsigned sreg)
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
static void mov_sink_scan(const X86Insn *insns, int n, const uint64_t *fl_need)
{
    for (int i = 0; i < n; i++) { g_mov_sink_at[i] = -1; g_mov_skip[i] = 0; }
    static int dis = -1; if (dis < 0) dis = (getenv("OCERZ_NO_MOVFUSE") || getenv("OCERZ_NO_MOVSINK")) ? 1 : 0;
    if (dis || !g_defer || g_xlat_mode32) return;
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
static uint8_t g_fpb_mmem[JIT_MAX_BLOCK_INSNS], g_fpb_marith[JIT_MAX_BLOCK_INSNS];
static int g_fpb_open = -1;
static const int8_t *g_fpb_of;

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

enum { RK_END = 0, RK_DET, RK_STORE, RK_LOAD, RK_MOVE, RK_LMOVE, RK_UNPCKH, RK_UNPCKL };
static int fpb_region_class(const X86Insn *in)
{
    if (in->seg != OCERZ_SEG_NONE || in->nops < 2) return RK_END;
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
static int g_fpb_fast;
static int g_fpb_disabled = -1;
static struct JitOslowMap g_fpbmap[JIT_MAX_BLOCK_INSNS];
static int g_n_fpbmap;
#define FPCKPT_OFF ((uint32_t)offsetof(OcerzCPU, fp_ckpt))
_Static_assert(offsetof(OcerzCPU, fp_ckpt) % 16 == 0 && offsetof(OcerzCPU, fp_ckpt) + 256 <= 65520,
               "fp_ckpt must be q-addressable");

static int fpb_class(const X86Insn *in, int *packed, int *dbl, int *from_mem, int *sqrt_like)
{
    *packed = *dbl = *from_mem = *sqrt_like = 0;
    if (in->seg != OCERZ_SEG_NONE || in->nops < 2) return 0;
    const X86Operand *d = &in->ops[0], *sr = &in->ops[1];
    if (d->kind != OCERZ_OPK_XMM || !xmm_is_pinned(d->reg)) return 0;
    if (sr->kind == OCERZ_OPK_MEM) {
        if (sr->riprel) { *from_mem = 1; }
        else if (in->addrsize != 8) return 0;
        else *from_mem = 1;
    } else if (sr->kind != OCERZ_OPK_XMM || !xmm_is_pinned(sr->reg)) return 0;
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
        return 2;
    case OCERZ_OP_MOVSS: return 3;
    case OCERZ_OP_MOVSDX: *dbl = 1; return 3;
    default: return 0;
    }
}

static void fpb_scan(const X86Insn *insns, int n, int8_t *bat)
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
            fb->first = i; fb->last = last; fb->ckpt = ckpt;
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

static int unsafe_nocheckbr(void)
{
    static int en = -1;
    if (en < 0) en = getenv("OCERZ_UNSAFE_NOCHECKBR") ? 1 : 0;
    return en;
}

static void fpb_emit_check(A64Buf *b, FpBatch *fb)
{
    int have_f = 0, have_d = 0;
    fb->fcmp_vreg = -1;
    g_fcmp_self_idx = -1;
    if (!(fb->full | fb->s0 | fb->d0)) { fb->site = NULL; fb->back = NULL; return; }
    if (fb->full) {
        int first = -1, acc = -1;
        for (int r = 0; r < 16; r++) if (fb->full & (1u << r)) {
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
            int v = xmm_vreg((unsigned)r);
            if (cur < 0) cur = v;
            else { a64_fmax_s(b, 0, VX1, cur, v); cur = VX1; }
        }
        if (cur != VX1) { a64_fmax_s(b, 0, VX1, cur, cur); }
        have_f = 1;
    }
    if (fb->d0) {
        int cur = -1;
        for (int r = 0; r < 16; r++) if (fb->d0 & (1u << r)) {
            int v = xmm_vreg((unsigned)r);
            if (cur < 0) cur = v;
            else { a64_fmax_s(b, 1, VX2, cur, v); cur = VX2; }
        }
        a64_fcmp(b, 1, cur, cur);
        g_fcmp_self_vreg = cur;
        g_fcmp_self_idx = g_cur_insn_idx;
        fb->fcmp_vreg = (int8_t)cur;
        have_d = 1;
    }
    if (have_d && have_f) {
        uint32_t *dnan = a64_label(b); a64_bcond(b, A64_VS, 0);
        a64_fcmp(b, 0, VX1, VX1);
        fb->site = a64_label(b); a64_bcond(b, A64_VS, 0);
        uint32_t *skip = a64_label(b); a64_b(b, 0);
        a64_patch_bcond(dnan, a64_label(b));
        uint32_t *tramp = a64_label(b); a64_b(b, 0);
        a64_patch_b(skip, a64_label(b));
        fb->back = a64_label(b);
        fb->gain = (int)(tramp - fb->site);
    } else if (have_d) {
        if (unsafe_nocheckbr()) fb->site = NULL;
        else { fb->site = a64_label(b); a64_bcond(b, A64_VS, 0); }
        fb->back = a64_label(b);
        fb->gain = 0;
    } else {
        a64_fcmp(b, 0, VX1, VX1);
        if (unsafe_nocheckbr()) fb->site = NULL;
        else { fb->site = a64_label(b); a64_bcond(b, A64_VS, 0); }
        fb->back = a64_label(b);
        fb->gain = 0;
    }
}

static int8_t g_l0[16];
static uint8_t g_l0_dbl[16];
static uint16_t g_l0_owners[8];
static unsigned g_l0_next;

static void fpb_site_emit(A64Buf *b, int end, int va, int vb, int dbl)
{
    if (g_n_fpb_sites >= FPB_SITES_MAX) return;
    FpbSite *st = &g_fpb_sites[g_n_fpb_sites++];
    st->batch = g_fpb_open; st->end = end;
    st->site = a64_label(b); a64_bcond(b, A64_VS, 0);
    st->back = a64_label(b);
    for (int r = 0; r < 16; r++) { st->l0[r] = g_l0[r]; st->l0_dbl[r] = g_l0_dbl[r]; }
    st->fcmp_a = (int8_t)va; st->fcmp_b = (int8_t)vb; st->fcmp_dbl = (uint8_t)dbl;
}
static int fpb_det_here(int idx)
{
    return g_fpb_open >= 0 && g_fpb_of && idx >= 0 && g_fpb_det[idx] && g_fpb_of[idx] == g_fpb_open;
}
static void fpb_emit_regs_check(A64Buf *b, uint16_t regs, int batch, int end, const int8_t *l0, const uint8_t *l0_dbl)
{
    if (!regs || g_n_fpb_sites >= FPB_SITES_MAX) return;
    int first = -1, acc = -1;
    for (int r = 0; r < 16; r++) if (regs & (1u << r)) {
        int v = xmm_vreg((unsigned)r);
        if (first < 0) first = v;
        else if (acc < 0) { a64_v_fmax(b, 0, VX0, first, v); acc = VX0; }
        else a64_v_fmax(b, 0, VX0, VX0, v);
    }
    a64_fmaxv_4s(b, VX1, acc >= 0 ? acc : first);
    a64_fcmp(b, 0, VX1, VX1);
    FpbSite *st = &g_fpb_sites[g_n_fpb_sites++];
    st->batch = batch; st->end = end;
    st->site = a64_label(b); a64_bcond(b, A64_VS, 0);
    st->back = a64_label(b);
    for (int r = 0; r < 16; r++) { st->l0[r] = l0[r]; st->l0_dbl[r] = l0_dbl[r]; }
    st->fcmp_a = -1; st->fcmp_b = -1; st->fcmp_dbl = 0;
}
static int l0_enabled(void)
{
    static int en = -1;
    if (en < 0) en = (getenv("OCERZ_NO_L0CACHE") || mem_guard_needed()) ? 0 : 1;
    return en;
}
static uint16_t g_l0_dirty;
static void l0_reset(void)
{
    for (int i = 0; i < 16; i++) g_l0[i] = -1;
    for (int i = 0; i < 8; i++) g_l0_owners[i] = 0;
    g_l0_dirty = 0;
}
static int l0_defer(void)
{
    static int v = -1;
    if (v < 0) v = getenv("OCERZ_NO_L0_DEFER") == NULL;
    return v;
}
static void l0_flush_reg(A64Buf *b, unsigned r)
{
    if (!(g_l0_dirty & (1u << r)))
        return;
    g_l0_dirty &= (uint16_t)~(1u << r);
    if (g_l0[r] < 0)
        return;
    if (g_l0_dbl[r]) a64_ins_d_d(b, xmm_vreg(r), 0, g_l0[r], 0);
    else             a64_ins_s_s(b, xmm_vreg(r), 0, g_l0[r], 0);
}
static void l0_flush_all(A64Buf *b)
{
    while (g_l0_dirty)
        l0_flush_reg(b, (unsigned)__builtin_ctz(g_l0_dirty));
}
static int l0_defer_take(int vs, unsigned xr, int size)
{
    if (g_xlat_mode32)
        return 0;
    if (!(l0_defer() && l0_enabled() && vs >= 4 && vs <= 7 &&
          g_l0[xr] == vs && g_l0_dbl[xr] == (uint8_t)(size == 8)))
        return 0;
    g_l0_dirty |= (uint16_t)(1u << xr);
    return 1;
}
static void l0_inval(unsigned r)
{
    if (r < 16 && g_l0[r] >= 0) { g_l0_owners[g_l0[r] - 4] &= (uint16_t)~(1u << r); g_l0[r] = -1; }
    g_l0_dirty &= (uint16_t)~(1u << r);
}
static int g_l0_fixed;
static int8_t g_l0_fixed_lane[16];
static uint8_t g_l0_fixed_dbl[16];
static int l0_alloc2(A64Buf *b, unsigned r, int dbl)
{
    if (g_l0_fixed) {
        if (g_l0_fixed_lane[r] < 0) {
            l0_inval(r);
            return -1;
        }
        int t = g_l0_fixed_lane[r];
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
    int t = 4 + (int)(g_l0_next++ & 3u);
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
#define l0_alloc(r, dbl) l0_alloc2(b, r, dbl)
static int l0_src2(A64Buf *b, unsigned r, int dbl)
{
    if (l0_enabled() && g_l0[r] >= 0 && g_l0_dbl[r] == (uint8_t)dbl) return g_l0[r];
    l0_flush_reg(b, r);
    return xmm_vreg(r);
}
static void l0_share(unsigned dst, unsigned src)
{
    l0_inval(dst);
    if (l0_enabled() && g_l0[src] >= 0) {
        g_l0[dst] = g_l0[src]; g_l0_dbl[dst] = g_l0_dbl[src];
        g_l0_owners[g_l0[src] - 4] |= (uint16_t)(1u << dst);
        if (g_l0_dirty & (1u << src))
            g_l0_dirty |= (uint16_t)(1u << dst);
    }
}
static int fpb_class(const X86Insn *in, int *packed, int *dbl, int *from_mem, int *sqrt_like);
static int l0_fixed_setup(A64Buf *b, const X86Insn *insns, int n)
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
                int selfzero = (insns[i].op == OCERZ_OP_XORPS || insns[i].op == OCERZ_OP_PXOR) &&
                               insns[i].nops >= 2 && insns[i].ops[1].kind == OCERZ_OPK_XMM &&
                               insns[i].ops[1].reg == dr;
                wfirst[dr] = (uint8_t)((c == 2 || selfzero) ? 2 : 1);
            }
        }
        if (insns[i].nops >= 2 && insns[i].ops[1].kind == OCERZ_OPK_XMM && !wfirst[insns[i].ops[1].reg])
            wfirst[insns[i].ops[1].reg] = 1;
        int use = (c == 1 || c == 3) && !packed;
        if (!use && (insns[i].op == OCERZ_OP_CVTSI2SD || insns[i].op == OCERZ_OP_CVTSI2SS)) {
            use = 1; dbl = insns[i].op == OCERZ_OP_CVTSI2SD;
        }
        if (!use) continue;
        for (int q = 0; q < insns[i].nops && q < 2; q++) {
            const X86Operand *o = &insns[i].ops[q];
            if (o->kind == OCERZ_OPK_XMM && xmm_is_pinned(o->reg)) {
                cnt[o->reg]++;
                if (firstdbl[o->reg] < 0) firstdbl[o->reg] = (int8_t)dbl;
            }
        }
    }
    for (int r = 0; r < 16; r++)
        if (wfirst[r] == 2) cnt[r] = 0;
    int lanes = 0;
    for (int k = 0; k < 4; k++) {
        int best = -1;
        for (int r = 0; r < 16; r++)
            if (g_l0_fixed_lane[r] < 0 && cnt[r] >= 2 && (best < 0 || cnt[r] > cnt[best])) best = r;
        if (best < 0) break;
        g_l0_fixed_lane[best] = (int8_t)(4 + lanes);
        g_l0_fixed_dbl[best] = (uint8_t)(firstdbl[best] > 0);
        lanes++;
    }
    if (!lanes) return 0;
    for (int r = 0; r < 16; r++) if (g_l0_fixed_lane[r] >= 0) {
        a64_v_mov(b, g_l0_fixed_lane[r], xmm_vreg(r));
        g_l0[r] = g_l0_fixed_lane[r];
        g_l0_dbl[r] = g_l0_fixed_dbl[r];
        g_l0_owners[g_l0_fixed_lane[r] - 4] = (uint16_t)(1u << r);
    }
    g_l0_dirty = 0;
    g_l0_fixed = 1;
    return 1;
}

static void l0_fixed_restore(A64Buf *b)
{
    static int noflush = -1;
    if (noflush < 0) noflush = getenv("OCERZ_UNSAFE_NOFLUSH") ? 1 : 0;
    if (noflush) { g_l0_dirty = 0; return; }
    l0_flush_all(b);
    for (int r = 0; r < 16; r++) {
        if (g_l0_fixed_lane[r] >= 0) {
            int t = g_l0_fixed_lane[r];
            if (g_l0[r] != t || g_l0_dbl[r] != g_l0_fixed_dbl[r] ||
                g_l0_owners[t - 4] != (uint16_t)(1u << r)) {
                a64_v_mov(b, t, xmm_vreg(r));
                g_l0[r] = (int8_t)t;
                g_l0_dbl[r] = g_l0_fixed_dbl[r];
                g_l0_owners[t - 4] = (uint16_t)(1u << r);
            }
        } else if (g_l0[r] >= 0) {
            g_l0_owners[g_l0[r] - 4] &= (uint16_t)~(1u << r);
            g_l0[r] = -1;
        }
    }
}

static void l0_fixed_backedge(A64Buf *b)
{
    if (g_l0_fixed)
        l0_fixed_restore(b);
}

static int g_cmps_mask_idx = -1;
static int cmps_blendv_fusable(int cmps_idx)
{
    if (!g_cur_insns || cmps_idx < 0 || cmps_idx + 1 >= g_cur_insns_n) return 0;
    const X86Insn *c = &g_cur_insns[cmps_idx], *v = &g_cur_insns[cmps_idx + 1];
    if (c->ops[0].kind != OCERZ_OPK_XMM || c->ops[0].reg != 0 || c->nops < 3) return 0;
    if (!((c->op == OCERZ_OP_CMPSDX && v->op == OCERZ_OP_BLENDVPD) ||
          (c->op == OCERZ_OP_CMPSS && v->op == OCERZ_OP_BLENDVPS))) return 0;
    if (v->nops < 2 || v->ops[0].kind != OCERZ_OPK_XMM || v->ops[0].reg == 0 ||
        !xmm_is_pinned(v->ops[0].reg) || !xmm_is_pinned(0)) return 0;
    if (v->ops[1].kind == OCERZ_OPK_XMM && v->ops[1].reg == 0) return 0;
    if (v->seg != OCERZ_SEG_NONE || !l0_enabled()) return 0;
    return 1;
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
    case OCERZ_OP_MOVSS: case OCERZ_OP_MOVSDX:
        return 1;
    default: return 0;
    }
}

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
    int inexact_nan = inexact_env || g_fpb_fast;
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

static int emit_sse_bitwise(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_XMM) return 0;
    int kind, esz = 0;
    switch (insn->op) {
    case OCERZ_OP_PXOR: case OCERZ_OP_XORPS: kind = 0; break;
    case OCERZ_OP_PAND: case OCERZ_OP_ANDPS: kind = 1; break;
    case OCERZ_OP_POR:  case OCERZ_OP_ORPS:  kind = 2; break;
    case OCERZ_OP_PANDN: case OCERZ_OP_ANDNPS: kind = 3; break;
    case OCERZ_OP_PADDB: kind = 4; esz = 0; break; case OCERZ_OP_PADDW: kind = 4; esz = 1; break;
    case OCERZ_OP_PADDD: kind = 4; esz = 2; break; case OCERZ_OP_PADDQ: kind = 4; esz = 3; break;
    case OCERZ_OP_PSUBB: kind = 5; esz = 0; break; case OCERZ_OP_PSUBW: kind = 5; esz = 1; break;
    case OCERZ_OP_PSUBD: kind = 5; esz = 2; break; case OCERZ_OP_PSUBQ: kind = 5; esz = 3; break;
    case OCERZ_OP_PCMPEQD: kind = 6; esz = 2; break; case OCERZ_OP_PCMPEQB: kind = 6; esz = 0; break;
    case OCERZ_OP_PCMPEQW: kind = 6; esz = 1; break; case OCERZ_OP_PCMPEQQ: kind = 6; esz = 3; break;
    case OCERZ_OP_PCMPGTD: kind = 7; esz = 2; break;
    default: return 0;
    }
    if (kind == 0 && s->kind == OCERZ_OPK_XMM && s->reg == d->reg) {
        int vd = xmm_dst_reg(d->reg, VX0);
        a64_v_zero(b, vd);
        if (vd == VX0) emit_xmm_st(b, VX0, d->reg);
        return 1;
    }
    int vb = emit_sse_src_reg(b, insn, s, 16, VX1, exit_sites, n_exits);
    if (vb < 0) return 0;
    int vd = xmm_dst_reg(d->reg, VX0);
    if (vd == VX0) emit_xmm_ld(b, VX0, d->reg);
    switch (kind) {
    case 0: a64_v_eor(b, vd, vd, vb); break;
    case 1: a64_v_and(b, vd, vd, vb); break;
    case 2: a64_v_orr(b, vd, vd, vb); break;
    case 3: a64_v_bic(b, vd, vb, vd); break;
    case 4: a64_v_add(b, esz, vd, vd, vb); break;
    case 5: a64_v_sub(b, esz, vd, vd, vb); break;
    case 6: a64_v_cmeq(b, esz, vd, vd, vb); break;
    case 7: a64_v_cmgt(b, esz, vd, vd, vb); break;
    }
    if (vd == VX0) emit_xmm_st(b, VX0, d->reg);
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
    if (g_defer)
        a64_str(b, 4, A64_ZR, 20, CC_OP_OFF);
    int vb = (s->kind == OCERZ_OPK_XMM && xmm_is_pinned(s->reg)) ? l0_src(s->reg, dbl)
           : emit_sse_src_reg(b, insn, s, esz, VX1, exit_sites, n_exits);
    if (vb < 0) return 0;
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

static int emit_sse_pinsr_pextr(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_pshufb(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_punpck(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_pmovx(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
static int emit_sse_round(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);
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
    switch (pred) {
    case 0: a64_fcmeq_s(b, dbl, VX2, va, vb); break;
    case 1: a64_fcmgt_s(b, dbl, VX2, vb, va); break;
    case 2: a64_fcmge_s(b, dbl, VX2, vb, va); break;
    case 3: a64_fcmeq_s(b, dbl, VX2, va, va); a64_fcmeq_s(b, dbl, VX3, vb, vb);
            a64_v_and(b, VX2, VX2, VX3); a64_v_not(b, VX2, VX2); break;
    case 4: a64_fcmeq_s(b, dbl, VX2, va, vb); a64_v_not(b, VX2, VX2); break;
    case 5: a64_fcmgt_s(b, dbl, VX2, vb, va); a64_v_not(b, VX2, VX2); break;
    case 6: a64_fcmge_s(b, dbl, VX2, vb, va); a64_v_not(b, VX2, VX2); break;
    default: a64_fcmeq_s(b, dbl, VX2, va, va); a64_fcmeq_s(b, dbl, VX3, vb, vb);
            a64_v_and(b, VX2, VX2, VX3); break;
    }
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
                if (dbl) a64_ins_d_d(b, t, 0, vd0, 0); else a64_ins_s_s(b, t, 0, vd0, 0);
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

static int emit_sse(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
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
    case OCERZ_OP_PCMPGTD:
        return emit_sse_bitwise(b, insn, exit_sites, n_exits);
    case OCERZ_OP_UCOMISS: case OCERZ_OP_UCOMISD: case OCERZ_OP_COMISS: case OCERZ_OP_COMISD:
        return emit_sse_comis(b, insn, exit_sites, n_exits);
    case OCERZ_OP_CVTTSD2SI: case OCERZ_OP_CVTTSS2SI: case OCERZ_OP_CVTSI2SD: case OCERZ_OP_CVTSI2SS:
    case OCERZ_OP_CVTSD2SS: case OCERZ_OP_CVTSS2SD: case OCERZ_OP_CVTDQ2PS:
        return emit_sse_cvt(b, insn, exit_sites, n_exits);
    case OCERZ_OP_MOVQX: return emit_sse_movq(b, insn, exit_sites, n_exits);
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

static int emit_bitscan(A64Buf *b, const X86Insn *insn, uint64_t need)
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
        if (!emit_mem_load_plain(b, insn, s, d->size, JT1)) return 0;
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

#define RFLAGS_OFF ((uint32_t)offsetof(OcerzCPU, rflags))
static int emit_bt(A64Buf *b, const X86Insn *insn, uint64_t need, uint32_t **exit_sites, int *n_exits)
{
    if (!g_defer || insn->nops != 2 || insn->seg != OCERZ_SEG_NONE || insn->addrsize != 8) return 0;
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

static int emit_push_pop_mem(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
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

static int emit_leave(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
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

static int emit_shiftd(A64Buf *b, const X86Insn *insn, uint64_t need)
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
            if (!emit_mem_load_plain(b, insn, s, esize, JT0)) return 0;
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
        return 0;
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
            if (!emit_mem_load_plain(b, insn, s, 2, JT0)) return 0;
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

static int emit_pmovmskb(A64Buf *b, const X86Insn *insn)
{
    if (!sse_enabled() || insn->nops != 2) return 0;
    const X86Operand *d = &insn->ops[0], *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8)) return 0;
    if (s->kind != OCERZ_OPK_XMM || !xmm_is_pinned(s->reg)) return 0;
    if (g_n_raslit >= RASLIT_MAX) return 0;
    int ds = pin_slot(d->reg);
    if (ds < 0 || (rsp_is_ptr() && d->reg == OCERZ_RSP)) return 0;
    a64_v_sshr_16b(b, VX0, xmm_vreg(s->reg), 7);
    g_raslit[g_n_raslit].site = a64_label(b);
    g_raslit[g_n_raslit].retaddr = 0x8040201008040201ull;
    g_raslit[g_n_raslit].hi = 0x8040201008040201ull;
    g_raslit[g_n_raslit].kind = 2;
    g_raslit[g_n_raslit].rt = VX1;
    g_n_raslit++;
    a64_emit32(b, 0x9c000000u | (uint32_t)VX1);
    a64_v_and(b, VX0, VX0, VX1);
    a64_v_addp_16b(b, VX0, VX0, VX0);
    a64_v_addp_16b(b, VX0, VX0, VX0);
    a64_v_addp_16b(b, VX0, VX0, VX0);
    a64_umov_w_h(b, pin_hreg(ds), VX0, 0);
    return 1;
}

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
static int emit_rmw_mem(A64Buf *b, const X86Insn *insn, uint64_t need,
                        uint32_t **exit_sites, int *n_exits)
{
    static int dis = -1; if (dis < 0) dis = getenv("OCERZ_NO_INLINE_RMW") ? 1 : 0;
    if (dis || !g_defer || insn->addrsize != 8) return 0;
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
    if (rsp_is_ptr() && (m->base == OCERZ_RSP || m->index == OCERZ_RSP)) return 0;
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
    } else if (!plainacc && disp != 0) {
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
    case OCERZ_OP_AND: case OCERZ_OP_TEST: a64_and_reg(b, sf, JT2, JT0, rs, 0); break;
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
    case OCERZ_OP_CMP: a64_sub_reg(b, sf, JT2, JT0, rs, 0); have_new = 0; break;
    default: return 0;
    }
    if (size == 1) a64_uxtb(b, JT2, JT2); else if (size == 2) a64_uxth(b, JT2, JT2);
    else if (size == 4 && (op == OCERZ_OP_NEG || op == OCERZ_OP_NOT || op == OCERZ_OP_SUB || op == OCERZ_OP_ADD || op == OCERZ_OP_XADD || op == OCERZ_OP_INC || op == OCERZ_OP_DEC || op == OCERZ_OP_CMP))
        a64_mov_reg(b, 0, JT2, JT2);

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
    (void)have_new;
    if (align_bne) {
        uint32_t *sites[1] = { align_bne };
        if (!oolslow_add(insn, sites, 1, a64_label(b))) return 0;
    }
    return 1;
}

static int m32_inline_ok(const X86Insn *insn)
{
    if (insn->addrsize != 4)
        return 0;
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
        return 1;
    default:
        return insn->op >= OCERZ_OP_MOVUPS && insn->op <= OCERZ_OP_PBLENDVB;
    }
}

static int try_inline(A64Buf *b, const X86Insn *insn, uint64_t need,
                      uint32_t **exit_sites, int *n_exits)
{
    if (insn->vex) return 0;
    if (insn->mode32 && !m32_inline_ok(insn))
        return 0;
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
    case OCERZ_OP_XADD:
    case OCERZ_OP_CMPXCHG:
        return emit_rmw_mem(b, insn, need, exit_sites, n_exits);
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
    case OCERZ_OP_PCMPGTD:
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
        return emit_sse(b, insn, exit_sites, n_exits);
    case OCERZ_OP_MUL:
        return emit_mul_wide(b, insn, need, 0);
    case OCERZ_OP_IMUL:
        if (insn->nops == 1)
            return emit_mul_wide(b, insn, need, 1);
        if (insn->ops[0].kind == OCERZ_OPK_MEM ||
            (insn->nops > 1 && insn->ops[1].kind == OCERZ_OPK_MEM) ||
            (insn->nops > 2 && insn->ops[2].kind == OCERZ_OPK_MEM))
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

static uint32_t *emit_chain_tail(A64Buf *b, int poll);

static JitBlock *g_tag_blk;
static int g_tag_idx;
#define A64_NOP 0xd503201fu
static void emit_prof_count(A64Buf *b, JitProf *pf, uint32_t off)
{
    a64_mov_imm64(b, JT1, (uint64_t)(uintptr_t)pf);
    a64_ldr(b, 4, JT2, JT1, off);
    a64_add_imm(b, 0, JT2, JT2, 1);
    a64_str(b, 4, JT2, JT1, off);
}
static void emit_side_tag(A64Buf *b, int cpu_reg)
{
    if (!g_tag_blk) return;
    a64_mov_imm64(b, JT0, (uint64_t)(uintptr_t)g_tag_blk);
    a64_str(b, 8, JT0, cpu_reg, SIDE_BLK_OFF);
    a64_mov_imm64(b, JT0, (uint64_t)g_tag_idx);
    a64_str(b, 4, JT0, cpu_reg, SIDE_IDX_OFF);
}
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

static uint32_t *cond_short_site(uint32_t *to_taken, int taken_rec, int body_edge)
{
    return (taken_rec || !body_edge || g_l0_dirty) ? NULL : to_taken;
}

static int side_stub_has_work(int k)
{
    if (g_side[k].rec) return 1;
    if (g_side[k].fpb >= 0 && g_side[k].fpb_chk) return 1;
    for (int r = 0; r < 16; r++)
        if ((g_side[k].l0_dirty & (1u << r)) && g_side[k].l0[r] >= 0) return 1;
    return 0;
}

static int can_fuse_cmp_test_jcc(const X86Insn *producer,
                                 const X86Insn *jcc, uint64_t block_rip)
{
    if (g_xlat_mode32)
        return 0;
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

static int flag_neutral_ok(const X86Insn *in);
static int insn_writes_reg(const X86Insn *in, unsigned reg);
static int side_gap_fuse_ok(const X86Insn *insns, int i, int n)
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
    if (!flag_neutral_ok(&insns[i + 1])) return 0;
    if (p->ops[0].kind == OCERZ_OPK_REG && insn_writes_reg(&insns[i + 1], p->ops[0].reg)) return 0;
    if (p->ops[1].kind == OCERZ_OPK_REG && insn_writes_reg(&insns[i + 1], p->ops[1].reg)) return 0;
    return 1;
}
static int side_fuse_ok(const X86Insn *insns, int i, int n)
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
static int emit_flag_neutral(A64Buf *b, const X86Insn *in)
{
    switch (in->op) {
    case OCERZ_OP_LEA:
        return emit_lea(b, in);
    case OCERZ_OP_MOV: {
        const X86Operand *d = &in->ops[0], *s = &in->ops[1];
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

static int emit_cmp_test_jcc(A64Buf *b, const X86Insn *producer,
                             const X86Insn *jcc,
                             uint32_t **epilogue_sites, int *n_epi,
                             uint32_t **jcc_label,
                             uint32_t **exit_sites, int *n_exits,
                             const X86Insn *gap, uint32_t **gap_label)
{
    if (g_xlat_mode32)
        return 0;
    if (!can_fuse_cmp_test_jcc(producer, jcc, g_self_rip) || !g_defer)
        return 0;
    if (gap) {
        const X86Operand *pd = &producer->ops[0], *ps = &producer->ops[1];
        if (pd->kind == OCERZ_OPK_REG && insn_writes_reg(gap, pd->reg)) return 0;
        if (ps->kind == OCERZ_OPK_REG && insn_writes_reg(gap, ps->reg)) return 0;
        if (!flag_neutral_ok(gap)) return 0;
        {
            uint32_t tmpw[128];
            A64Buf tb = { tmpw, tmpw, tmpw + 128, 0, 0 };
            if (!emit_flag_neutral(&tb, gap) || tb.overflow) return 0;
        }
    }

    const X86Operand *d = &producer->ops[0];
    const X86Operand *s = &producer->ops[1];
    int sf = d->size == 8;
    uint64_t taken = jcc->ops[0].imm;
    uint64_t fall = jcc->rip + jcc->len;
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
            if (v != 0 && (v & (v - 1)) == 0) {
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
        int need_rec = g_jcc_side_need != 0 || taken_live;
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

        if (g_no_xlive || xlive_succ_live(g_xlat_jit, fall) != 0) {
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
        int taken_rec = g_no_xlive || xlive_succ_live(g_xlat_jit, taken) != 0;
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

    if (g_no_xlive || xlive_succ_live(g_xlat_jit, fall) != 0) {
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
    if (g_no_xlive || xlive_succ_live(g_xlat_jit, taken) != 0) {
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

static int emit_incdec_jcc(A64Buf *b, const X86Insn *producer,
                           const X86Insn *jcc, uint32_t **epilogue_sites,
                           int *n_epi, uint32_t **jcc_label)
{
    if (g_xlat_mode32)
        return 0;
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

static int emit_arith_incdec_jcc(A64Buf *b, const X86Insn *arith,
                                 const X86Insn *incdec,
                                 const X86Insn *jcc, uint64_t arith_need,
                                 uint32_t **epilogue_sites, int *n_epi,
                                 uint32_t **incdec_label,
                                 uint32_t **jcc_label)
{
    if (g_xlat_mode32)
        return 0;
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

static int emit_logic_jmp_incdec_jcc(A64Buf *b, const X86Insn *logic,
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
            int rc = ocerz_decode_mode((const uint8_t *)ocerz_g2h(pc), 15, pc,
                                       &target[n], g_xlat_mode32);
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
    if (sigsetjmp(db, 0) == 0) {
        ocerz_jit_decode_recover = &db;
        while (n < cap) {
            if (ocerz_decode_mode((const uint8_t *)ocerz_g2h(pc), 15, pc,
                                  &out[n], g_xlat_mode32) != OCERZ_OK)
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

static int emit_jcc(A64Buf *b, const X86Insn *insn, uint32_t **epilogue_sites, int *n_epi)
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
        l0_fixed_backedge(b);
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

static int ras_body_only(void)
{
    return fullpin_enabled() && !g_no_regflags && stack_plain_access_ok() && jgb_usable() &&
           !stack_guard_needed() && !g_no_chain && !g_no_ras;
}
static void *ras_entry_for(const JitBlock *blk)
{
    if (!blk || !blk->code) return NULL;
    if (ras_body_only())
        return (blk->pin_class == 3 && blk->body_code) ? (void *)blk->body_code : NULL;
    if (blk->pin_class == 3 && blk->body_code)
        return (void *)((uintptr_t)blk->body_code | 1u);
    return (void *)blk->code;
}

void ocerz_ras_push(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t retaddr)
{
    uint32_t t = cpu->ras_top;
    if (ras_body_only()) {
        JitBlock *blk = cache_lookup(vm->jit, retaddr, cpu->mode32);
        cpu->ras[t & (OCERZ_RAS_SIZE - 1)].guest_rip = retaddr;
        cpu->ras[t & (OCERZ_RAS_SIZE - 1)].host_entry = ras_entry_for(blk);
        cpu->ras_top = t + 1;
        return;
    }
    if (t >= OCERZ_RAS_SIZE)
        return;
    JitBlock *blk = cache_lookup(vm->jit, retaddr, cpu->mode32);
    cpu->ras[t].guest_rip = retaddr;
    cpu->ras[t].host_entry = ras_entry_for(blk);
    cpu->ras_top = t + 1;
}

static void **ras_slot_alloc(void);
static void pending_add_ras(uint64_t target_key, void **ras_slot);
static uint32_t *emit_body_chain_tail(A64Buf *b, uint64_t target_rip, int poll,
                                      uint32_t **epilogue_sites, int *n_epi);

static void patch_local_adr(uint32_t *at, uint32_t *target, int rd)
{
    ptrdiff_t off = (char *)target - (char *)at;
    if (!(off >= -(1 << 20) && off < (1 << 20)))
        return;
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
    if (stack_plain_access_ok() && !stack_guard_needed()) {
        a64_str_pre64(b, JRET_GUEST, pin_hreg(rs), -8);
    } else {
        a64_sub_imm(b, 1, JTA, pin_hreg(rs), 8);
        skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_guest_store_ordered(b, 8, JRET_GUEST, JTA, JTU);
        a64_sub_imm(b, 1, pin_hreg(rs), pin_hreg(rs), 8);
    }
    patch_guard_skip(skip, a64_label(b));

    emit_xmm_pin_spill_all(b);
    uint32_t *adr = a64_label(b);
    a64_emit32(b, 0x10000000u | (uint32_t)JRET_HOST);
    uint32_t *callee_patch = a64_label(b);
    a64_b(b, 0);

    uint32_t *host_cont = a64_label(b);
    patch_local_adr(adr, host_cont, JRET_HOST);
    emit_xmm_pin_load_all(b);
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
    if (stack_plain_access_ok() && !stack_guard_needed()) {
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

static int emit_call_ret32(A64Buf *b, const X86Insn *insn,
                           uint32_t **epi_sites, int *n_epi)
{
    int size = insn->opsize ? insn->opsize : 4;
    if (size != 4)
        return 0;
    if (!m32_stack_ok(insn))
        return 0;
    int hs = pin_hreg(pin_slot(OCERZ_RSP));

    if (insn->op == OCERZ_OP_CALL) {
        if (insn->ops[0].kind != OCERZ_OPK_IMM)
            return 0;
        if (!mem_native_store_ok())
            return 0;
        uint64_t retaddr = (uint32_t)(insn->rip + insn->len);
        uint64_t target = insn->ops[0].imm;

        a64_mov_imm64(b, JT1, retaddr);
        a64_sub_imm(b, 0, JTA, hs, 4);
        a64_str_regoff_uxtw(b, 4, JT1, JGB, JTA);
        a64_mov_reg(b, 0, hs, JTA);

        if (g_no_chain) {
            a64_mov_imm64(b, JT0, target);
            a64_str(b, 8, JT0, 20, RIP_OFF);
            a64_mov_imm64(b, 0, OCERZ_STEP_OK);
            epi_sites[*n_epi] = a64_label(b);
            a64_b(b, 0);
            (*n_epi)++;
            return 1;
        }
        int edge_class = body_edge_pin_class();
        int body_edge = edge_class >= 0;
        uint32_t *pb = emit_static_chain_tail(b, target, target <= g_self_rip,
                                              body_edge, epi_sites, n_epi);
        g_jcc_edge[0].target_rip = target;
        g_jcc_edge[0].patch_b = pb;
        g_jcc_edge[0].cond_site = NULL;
        g_jcc_edge[0].kind = body_edge ? EDGE_BODY : EDGE_XBLOCK;
        g_jcc_edge[0].pin_class = body_edge ? (uint8_t)edge_class : 0;
        g_n_jcc_edges = 1;
        return 1;
    }

    if (insn->op == OCERZ_OP_RET) {
        uint32_t pop = 4;
        if (insn->nops == 1) {
            if (insn->ops[0].kind != OCERZ_OPK_IMM)
                return 0;
            pop += (uint32_t)(insn->ops[0].imm & 0xffff);
        }
        if (pop > 4095)
            return 0;
        a64_ldr_regoff_uxtw(b, 4, JT0, JGB, hs);
        a64_add_imm(b, 0, hs, hs, pop);
        a64_str(b, 8, JT0, 20, RIP_OFF);
        a64_mov_imm64(b, 0, OCERZ_STEP_OK);
        epi_sites[*n_epi] = a64_label(b);
        a64_b(b, 0);
        (*n_epi)++;
        return 1;
    }
    return 0;
}

static int emit_call_ret(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites,
                         int *n_exits, uint32_t **epi_sites, int *n_epi)
{
    uint64_t gbase = ocerz_guest_base;

    if (!callret_inline_enabled())
        return 0;

    if (insn->seg != OCERZ_SEG_NONE)
        return 0;

    if (insn->mode32)
        return emit_call_ret32(b, insn, epi_sites, n_epi);

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

        int fast3 = g_pin_class == 3 && pin_slot(OCERZ_RSP) >= 0 && stack_plain_access_ok() &&
                    jgb_usable() && !stack_guard_needed() && !g_no_chain;
        static int no_blret = -1;
        if (no_blret < 0) no_blret = getenv("OCERZ_NO_BLRET") ? 1 : 0;
        if (fast3 && ras_body_only() && !g_no_ras && !no_blret) {
            int hs = pin_hreg(pin_slot(OCERZ_RSP));
            uint32_t *adr_site;
            if (host_ras_enabled()) {
                adr_site = a64_label(b);
                a64_emit32(b, 0x10000000u | (uint32_t)JT0);
                a64_stp_pre(b, JT1, JT0, 31, -16);
                emit_push_pinned(b, hs, JT1);
            } else {
                emit_push_pinned(b, hs, JT1);
                a64_ldr(b, 4, JT2, 20, RAS_TOP_OFF);
                adr_site = a64_label(b);
                a64_emit32(b, 0x10000000u | (uint32_t)JT0);
                a64_and_imm_or_mov(b, 0, JTF, JT2, OCERZ_RAS_SIZE - 1);
                a64_add_reg(b, 1, JTA, 20, JTF, 4);
                if (RAS_OFF <= 504) a64_stp_off(b, JT1, JT0, JTA, RAS_OFF);
                else { a64_str(b, 8, JT1, JTA, RAS_OFF); a64_str(b, 8, JT0, JTA, RAS_OFF + 8); }
                a64_add_imm(b, 0, JT2, JT2, 1);
                a64_str(b, 4, JT2, 20, RAS_TOP_OFF);
            }
            uint32_t *pb_callee = a64_label(b);
            a64_emit32(b, 0x94000000u);
            uint32_t *cont = a64_label(b);
            patch_local_adr(adr_site, cont, JT0);
            uint32_t *pb_ret = emit_body_chain_tail(b, retaddr, 0, epi_sites, n_epi);
            uint32_t *callee_fb = a64_label(b);
            *pb_callee = 0x94000000u | ((uint32_t)(callee_fb - pb_callee) & 0x03ffffffu);
            a64_mov_imm64(b, JT0, target);
            a64_str(b, 8, JT0, 20, RIP_OFF);
            a64_mov_imm64(b, 0, OCERZ_STEP_OK);
            epi_sites[*n_epi] = a64_label(b);
            a64_b(b, 0);
            (*n_epi)++;
            g_chain_target = 0;
            g_jcc_edge[0].target_rip = target;
            g_jcc_edge[0].patch_b = pb_callee;
            g_jcc_edge[0].cond_site = NULL;
            g_jcc_edge[0].kind = EDGE_BODY;
            g_jcc_edge[0].pin_class = 3;
            g_jcc_edge[1].target_rip = retaddr;
            g_jcc_edge[1].patch_b = pb_ret;
            g_jcc_edge[1].cond_site = NULL;
            g_jcc_edge[1].kind = EDGE_BODY;
            g_jcc_edge[1].pin_class = 3;
            g_n_jcc_edges = 2;
            return 1;
        }
        if (fast3) {
            int hs = pin_hreg(pin_slot(OCERZ_RSP));
            emit_push_pinned(b, hs, JT1);
        } else {
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
        }

        if (!g_no_ras) {

            void **slot = NULL;
            int lit = fast3 && g_n_raslit < RASLIT_MAX;
            if (!lit) slot = ras_slot_alloc();
            if (slot || lit) {
                if (slot) {
                    JitBlock *rb = cache_lookup(g_xlat_jit, retaddr, g_xlat_mode32);
                    if (rb && rb->code)
                        *slot = ras_entry_for(rb);
                    else
                        pending_add_ras(jit_key(retaddr, g_xlat_mode32), slot);
                }
                a64_ldr(b, 4, JT2, 20, RAS_TOP_OFF);
                a64_subs_imm(b, 0, A64_ZR, JT2, OCERZ_RAS_SIZE);
                uint32_t *full = a64_label(b);
                a64_bcond(b, A64_CS, 0);
                if (!fast3) a64_mov_imm64(b, JT1, retaddr);
                if (lit) {
                    g_raslit[g_n_raslit].site = a64_label(b);
                    g_raslit[g_n_raslit].retaddr = retaddr;
                    g_raslit[g_n_raslit].kind = 0;
                    g_raslit[g_n_raslit].rt = JT0;
                    g_n_raslit++;
                    a64_emit32(b, 0x58000000u | (uint32_t)JT0);
                } else {
                    a64_mov_imm64(b, JTA, (uint64_t)(uintptr_t)slot);
                    a64_ldr(b, 8, JT0, JTA, 0);
                }
                a64_add_reg(b, 1, JTA, 20, JT2, 4);
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
                emit_reload_jgb(b);
                emit_xmm_pin_load_all(b);
            }
        }
    } else if (insn->op == OCERZ_OP_RET) {
        if (insn->nops != 0)
            return 0;

        int fast3 = g_pin_class == 3 && pin_slot(OCERZ_RSP) >= 0 && stack_plain_access_ok() &&
                    jgb_usable() && !stack_guard_needed();
        if (fast3) {
            int hs = pin_hreg(pin_slot(OCERZ_RSP));
            if (stack_identity() || rsp_is_ptr()) {
                a64_ldr_post64(b, JT1, hs, 8);
            } else {
                a64_ldr_regoff(b, 8, JT1, JGB, hs, 0);
                a64_add_imm(b, 1, hs, hs, 8);
            }
            if (!ras_body_only()) a64_str(b, 8, JT1, 20, RIP_OFF);
        } else {
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
        }

        if (!g_no_ras) {
            uint32_t *ras_empty = NULL;
            uint32_t *ras_stale[3];
            int nst = 0;
            int hostras = fast3 && ras_body_only() && host_ras_enabled();
            if (!hostras) a64_ldr(b, 4, JT2, 20, RAS_TOP_OFF);
            if (hostras) {
            } else if (fast3 && ras_body_only()) {
                a64_sub_imm(b, 0, JT2, JT2, 1);
                a64_and_imm_or_mov(b, 0, JTU, JT2, OCERZ_RAS_SIZE - 1);
                a64_add_reg(b, 1, JTA, 20, JTU, 4);
            } else {
                ras_empty = a64_label(b); a64_cbz(b, 0, JT2, 0);
                a64_sub_imm(b, 0, JT2, JT2, 1);
                a64_add_reg(b, 1, JTA, 20, JT2, 4);
            }
            static int no_blret_r = -1;
            if (no_blret_r < 0) no_blret_r = getenv("OCERZ_NO_BLRET") ? 1 : 0;
            int use_ret = g_pin_class == 3 && fast3 && ras_body_only() && !no_blret_r;
            int host_reg = use_ret ? 30 : JT0;
            if (hostras) a64_ldp_post(b, JTF, host_reg, 31, 16);
            else if (RAS_OFF <= 504) a64_ldp_off(b, JTF, host_reg, JTA, RAS_OFF);
            else { a64_ldr(b, 8, JTF, JTA, RAS_OFF); a64_ldr(b, 8, host_reg, JTA, RAS_OFF + 8); }
            a64_subs_reg(b, 1, A64_ZR, JTF, JT1, 0);
            ras_stale[nst] = a64_label(b); a64_bcond(b, A64_NE, 0); nst++;
            ras_stale[nst] = a64_label(b); a64_cbz(b, 1, host_reg, 0); nst++;

            if (!hostras) a64_str(b, 4, JT2, 20, RAS_TOP_OFF);

            uint32_t *not_body = NULL;
            if (g_pin_class == 3 && fast3 && ras_body_only()) {
                if (!xmm_global_enabled()) emit_xmm_pin_spill_all(b);
                if (use_ret) a64_ret(b); else a64_br(b, JT0);
                not_body = NULL;
            } else if (g_pin_class == 3) {
                not_body = a64_label(b); a64_tbz(b, JT0, 0, 0);
                if (!a64_try_and_imm(b, 1, JT0, JT0, ~1ull)) { a64_mov_imm64(b, JTU, 1); a64_bic_reg(b, 1, JT0, JT0, JTU, 0); }
                if (!xmm_global_enabled()) emit_xmm_pin_spill_all(b);
                a64_br(b, JT0);
                a64_patch_tbz(not_body, a64_label(b));
                if (!a64_try_and_imm(b, 1, JT0, JT0, ~1ull)) { a64_mov_imm64(b, JTU, 1); a64_bic_reg(b, 1, JT0, JT0, JTU, 0); }
            } else {
                ras_stale[nst] = a64_label(b); a64_tbnz(b, JT0, 0, 0); nst++;
            }
            emit_xmm_pin_spill_all(b);
            emit_spill_pinned(b);
            a64_mov_reg(b, 1, 0, 19);
            a64_mov_reg(b, 1, 1, 20);
            emit_frame_sp_reset(b);
            emit_pin_epilogue_restore(b);
            a64_ldp_post(b, 19, 20, 31, 16);
            a64_ldp_post(b, 29, 30, 31, 16);
            a64_br(b, JT0);

            uint32_t *miss_pop = a64_label(b);
            if (!hostras) a64_str(b, 4, JT2, 20, RAS_TOP_OFF);
            if (fast3 && ras_body_only()) a64_str(b, 8, JT1, 20, RIP_OFF);
            if (ocerz_perfstat > 0) {
                a64_mov_imm64(b, JTA, (uint64_t)(uintptr_t)&ps_ras_stale);
                a64_ldr(b, 8, JTU, JTA, 0); a64_add_imm(b, 1, JTU, JTU, 1); a64_str(b, 8, JTU, JTA, 0);
            }
            uint32_t *skip_rip = NULL;
            if (fast3 && ras_body_only()) { skip_rip = a64_label(b); a64_b(b, 0); }
            uint32_t *miss = a64_label(b);
            if (ras_empty) a64_patch_cbz(ras_empty, miss);
            if (fast3 && ras_body_only()) { a64_str(b, 8, JT1, 20, RIP_OFF); a64_patch_b(skip_rip, a64_label(b)); }
            if (ocerz_perfstat > 0) {
                a64_mov_imm64(b, JTA, (uint64_t)(uintptr_t)&ps_ras_miss);
                a64_ldr(b, 8, JTU, JTA, 0); a64_add_imm(b, 1, JTU, JTU, 1); a64_str(b, 8, JTU, JTA, 0);
            }
            for (int i = 0; i < nst; i++) {
                if ((*ras_stale[i] & 0x7f000000u) == 0x36000000u ||
                    (*ras_stale[i] & 0x7f000000u) == 0x37000000u)
                    a64_patch_tbz(ras_stale[i], miss_pop);
                else if ((*ras_stale[i] & 0xff000010u) == 0x54000000u)
                    a64_patch_bcond(ras_stale[i], miss_pop);
                else
                    a64_patch_cbz(ras_stale[i], miss_pop);
            }
        }
    } else {
        return 0;
    }

    if (insn->op == OCERZ_OP_CALL && g_pin_class == 3 && pin_slot(OCERZ_RSP) >= 0 &&
        stack_plain_access_ok() && jgb_usable() && !stack_guard_needed() && !g_no_chain) {
        g_chain_keeps_jgb = 1;
    } else {
        a64_mov_imm64(b, 0, OCERZ_STEP_OK);
    }
    epi_sites[*n_epi] = a64_label(b);

    g_chain_epi = epi_sites[*n_epi];
    a64_b(b, 0);
    (*n_epi)++;
    return 1;
}

static void emit_dispatch_stub(OcerzJit *jit, int mode32)
{
    A64Buf b = { jit->code_cur, jit->code_cur, jit->code_end, 0, 0 };
    uint32_t *entry = b.p;
    uint32_t *to_ret[12]; int nr = 0;
    a64_ldr(&b, 4, JT0, 1, INT_OFF);
    to_ret[nr++] = a64_label(&b); a64_cbnz(&b, 0, JT0, 0);
    a64_ldr(&b, 4, JT0, 1, (uint32_t)offsetof(OcerzCPU, terminated));
    to_ret[nr++] = a64_label(&b); a64_cbnz(&b, 0, JT0, 0);
    a64_ldr(&b, 4, JT0, 0, (uint32_t)offsetof(struct OcerzVM, exited));
    to_ret[nr++] = a64_label(&b); a64_cbnz(&b, 0, JT0, 0);
    a64_ldr(&b, 8, JT1, 1, RIP_OFF);
    a64_mov_imm64(&b, JTU, OCERZ_DYLDAPI_LO);
    a64_sub_reg(&b, 1, JTT, JT1, JTU, 0);
    a64_mov_imm64(&b, JTU, OCERZ_DYLDAPI_HI - OCERZ_DYLDAPI_LO);
    a64_subs_reg(&b, 1, A64_ZR, JTT, JTU, 0);
    to_ret[nr++] = a64_label(&b); a64_bcond(&b, A64_CC, 0);
    if (mode32) {
        int ok = a64_try_orr_imm(&b, 1, JT1, JT1, JIT_KEY_M32);
        assert(ok && "JIT_KEY_M32 must encode as a logical immediate");
        (void)ok;
    }
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
        to_ret[nr++] = a64_label(&b); a64_cbz(&b, 1, JTF, 0);
        a64_ldr(&b, 8, JTU, JTF, (uint32_t)offsetof(JitBlock, key));
        a64_sub_reg(&b, 1, JTU, JTU, JT1, 0);
        uint32_t *nxt = a64_label(&b); a64_cbnz(&b, 1, JTU, 0);
        a64_ldr(&b, 8, JT0, JTF, (uint32_t)offsetof(JitBlock, code));
        uint32_t *nocode = a64_label(&b); a64_cbz(&b, 1, JT0, 0);
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
        if (mode32) jit->dispatch_stub32 = entry;
        else        jit->dispatch_stub = entry;
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
    emit_frame_sp_reset(b);
    emit_pin_epilogue_restore(b);
    a64_ldp_post(b, 19, 20, 31, 16);
    a64_ldp_post(b, 29, 30, 31, 16);
    a64_br(b, code_reg);
}

static uint32_t **g_ind_call_cont;
static uint32_t *g_ind_call_tocont;
static int g_ind_treg = JT1;
static uint64_t g_dbg_ind_src;
static void emit_indirect_tail(A64Buf *b, JitIcSlot *slot,
                               uint32_t **epi_sites, int *n_epi)
{
    {
        static int dbg = -1; if (dbg < 0) dbg = getenv("OCERZ_WILDLOG") ? 1 : 0;
        if (dbg && g_dbg_ind_src) {
            a64_mov_imm64(b, JTU, g_dbg_ind_src);
            a64_str(b, 8, JTU, 20, (uint32_t)offsetof(OcerzCPU, dbg_ind_src));
        }
    }
    uint32_t *to_blr = NULL;
    JitPscEnt *psc = NULL;
    int treg = g_ind_treg;
    g_ind_treg = JT1;
    if (g_pin_class == 3 && g_n_raslit < RASLIT_MAX && !ENV_ON("OCERZ_NO_PSC"))
        psc = psc_alloc();
    uint32_t *psc_miss = NULL;
    if (psc) {
        g_raslit[g_n_raslit].site = a64_label(b);
        g_raslit[g_n_raslit].retaddr = (uint64_t)(uintptr_t)psc;
        g_raslit[g_n_raslit].kind = 1;
        g_raslit[g_n_raslit].rt = JT2;
        g_n_raslit++;
        a64_emit32(b, 0x58000000u | (uint32_t)JT2);
        a64_ubfx(b, 1, JTT, treg, 2, 5);
        a64_add_reg(b, 1, JT2, JT2, JTT, 4);
        a64_ldp_off(b, JTU, JT0, JT2, 0);
        a64_subs_reg(b, 1, A64_ZR, JTU, treg, 0);
        psc_miss = a64_label(b); a64_bcond(b, A64_NE, 0);
        uint32_t *intr = NULL;
        int stop_site_ok = g_n_stop_extra < 6;
        if (!stop_site_ok) {
            a64_ldr(b, 4, JTU, 20, INT_OFF);
            intr = a64_label(b); a64_cbnz(b, 0, JTU, 0);
        }
        uint32_t *br_site = a64_label(b);
        if (g_ind_call_cont) {
            to_blr = a64_label(b);
            a64_blr(b, JT0);
            *g_ind_call_cont = a64_label(b);
            g_ind_call_tocont = a64_label(b); a64_b(b, 0);
        } else {
            a64_br(b, JT0);
        }
        uint32_t *stop_lbl = a64_label(b);
        if (intr) a64_patch_cbz(intr, stop_lbl);
        if (stop_site_ok) stop_extra_add(br_site, stop_lbl);
        a64_str(b, 8, treg, 20, RIP_OFF);
        a64_mov_imm64(b, 0, OCERZ_STEP_OK);
        epi_sites[*n_epi] = a64_label(b);
        a64_b(b, 0);
        (*n_epi)++;
        a64_patch_bcond(psc_miss, a64_label(b));
    }
    if (treg != JT1) a64_mov_reg(b, 1, JT1, treg);
    a64_str(b, 8, JT1, 20, RIP_OFF);
    a64_lsr_imm(b, 1, JTT, JT1, 33);
    a64_eor_reg(b, 1, JTT, JTT, JT1, 0);
    a64_mov_imm64(b, JTU, 0xff51afd7ed558ccdull);
    a64_mul(b, 1, JTT, JTT, JTU);
    a64_lsr_imm(b, 1, JTU, JTT, 29);
    a64_eor_reg(b, 1, JTT, JTT, JTU, 0);
    a64_mov_imm64(b, JTU, JIT_HASH_MASK);
    a64_and_reg(b, 1, JTT, JTT, JTU, 0);
    a64_mov_imm64(b, JTA, (uint64_t)(uintptr_t)g_xlat_jit->buckets);
    a64_ldr_regoff(b, 8, JTF, JTA, JTT, 1);
    uint32_t *loop = a64_label(b);
    uint32_t *to_nofind = a64_label(b); a64_cbz(b, 1, JTF, 0);
    a64_ldr(b, 8, JTU, JTF, (uint32_t)offsetof(JitBlock, key));
    a64_sub_reg(b, 1, JTU, JTU, JT1, 0);
    uint32_t *found = a64_label(b); a64_cbz(b, 1, JTU, 0);
    a64_ldr(b, 8, JTF, JTF, (uint32_t)offsetof(JitBlock, hnext));
    { uint32_t *here = a64_label(b); a64_b(b, (int32_t)(loop - here)); }
    a64_patch_cbz(found, a64_label(b));
    uint32_t *to_full = NULL;
    if (g_pin_class == 1 || g_pin_class == 3) {
        a64_ldr(b, 1, JTU, JTF, (uint32_t)offsetof(JitBlock, pin_class));
        a64_sub_imm(b, 0, JTU, JTU, (uint32_t)g_pin_class);
        to_full = a64_label(b); a64_cbnz(b, 0, JTU, 0);
        a64_ldr(b, 8, JT0, JTF, (uint32_t)offsetof(JitBlock, body_code));
        uint32_t *nobody = a64_label(b); a64_cbz(b, 1, JT0, 0);
        if (psc) a64_stp_off(b, JT1, JT0, JT2, 0);
        uint32_t *intr = NULL;
        int stop_site_ok2 = g_n_stop_extra < 6;
        if (!stop_site_ok2) {
            a64_ldr(b, 4, JTU, 20, INT_OFF);
            intr = a64_label(b); a64_cbnz(b, 0, JTU, 0);
        }
        if (!xmm_global_enabled()) emit_xmm_pin_spill_all(b);
        uint32_t *br_site2 = a64_label(b);
        if (to_blr) { uint32_t *here = a64_label(b); a64_b(b, (int32_t)(to_blr - here)); }
        else if (g_ind_call_cont) {
            to_blr = a64_label(b);
            a64_blr(b, JT0);
            *g_ind_call_cont = a64_label(b);
            g_ind_call_tocont = a64_label(b); a64_b(b, 0);
        }
        else a64_br(b, JT0);
        uint32_t *stop_lbl2 = a64_label(b);
        a64_patch_cbz(nobody, stop_lbl2);
        if (intr) a64_patch_cbz(intr, stop_lbl2);
        if (stop_site_ok2 && !to_blr) stop_extra_add(br_site2, stop_lbl2);
        else if (stop_site_ok2 && to_blr && br_site2 != to_blr) stop_extra_add(br_site2, stop_lbl2);
        uint32_t *to_epi = a64_label(b); a64_b(b, 0);
        a64_patch_cbz(to_full, a64_label(b));
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
    a64_patch_cbz(to_nofind, a64_label(b));
    (void)slot;
    a64_mov_imm64(b, 0, OCERZ_STEP_OK);
    epi_sites[*n_epi] = a64_label(b);
    a64_b(b, 0);
    (*n_epi)++;
}

static int emit_branch_target(A64Buf *b, const X86Insn *insn, const X86Operand *o,
                              uint32_t **exit_sites, int *n_exits)
{
    g_ind_treg = JT1;
    if (o->kind == OCERZ_OPK_REG) {
        if (o->high8 || o->size != 8)
            return 0;
        if (rsp_is_ptr() && o->reg == OCERZ_RSP)
            return 0;
        if (pin_slot(o->reg) >= 0 && o->reg != OCERZ_RSP && !ENV_ON("OCERZ_NO_IND_TREG")) {
            g_ind_treg = pin_hreg(pin_slot(o->reg));
            return 1;
        }
        emit_gpr_rd(b, 1, JT1, o->reg);
        return 1;
    }
    if (o->kind == OCERZ_OPK_MEM) {
        if (o->size != 8)
            return 0;
        if (emit_plain_mem_fast(b, insn, o, 8, JT1, 0, 0))
            return 1;
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
    if (ENV_ON("OCERZ_NO_INLINE_INDIRECT"))
        return 0;
    if (insn->seg != OCERZ_SEG_NONE)
        return 0;
    if (ENV_ON("OCERZ_EXP_MAT_IND")) emit_materialize(b);
    if (!emit_branch_target(b, insn, &insn->ops[0], exit_sites, n_exits))
        return 0;
    emit_indirect_tail(b, ic_slot_alloc(), epi_sites, n_epi);
    return 1;
}

static int emit_indirect_call(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites,
                              int *n_exits, uint32_t **epi_sites, int *n_epi)
{
    g_dbg_ind_src = insn->rip;
    if (insn->op != OCERZ_OP_CALL || insn->ops[0].kind == OCERZ_OPK_IMM)
        return 0;
    if (ENV_ON("OCERZ_NO_INLINE_INDIRECT"))
        return 0;
    if (insn->seg != OCERZ_SEG_NONE || !mem_native_store_ok())
        return 0;
    if (ENV_ON("OCERZ_EXP_MAT_IND")) emit_materialize(b);
    if (!emit_branch_target(b, insn, &insn->ops[0], exit_sites, n_exits))
        return 0;
    uint64_t retaddr = insn->rip + insn->len;
    {
        static int no_blret_i = -1;
        if (no_blret_i < 0) no_blret_i = getenv("OCERZ_NO_BLRET") ? 1 : 0;
        int fast3 = g_pin_class == 3 && pin_slot(OCERZ_RSP) >= 0 && stack_plain_access_ok() &&
                    jgb_usable() && !stack_guard_needed() && !g_no_chain &&
                    !g_no_ras && ras_body_only() && !no_blret_i;
        if (fast3) {
            int hs = pin_hreg(pin_slot(OCERZ_RSP));
            a64_mov_imm64(b, JT2, retaddr);
            uint32_t *adr_site;
            if (host_ras_enabled()) {
                adr_site = a64_label(b);
                a64_emit32(b, 0x10000000u | (uint32_t)JT0);
                a64_stp_pre(b, JT2, JT0, 31, -16);
                emit_push_pinned(b, hs, JT2);
            } else {
                emit_push_pinned(b, hs, JT2);
                a64_ldr(b, 4, JTF, 20, RAS_TOP_OFF);
                adr_site = a64_label(b);
                a64_emit32(b, 0x10000000u | (uint32_t)JT0);
                a64_and_imm_or_mov(b, 0, JTU, JTF, OCERZ_RAS_SIZE - 1);
                a64_add_reg(b, 1, JTA, 20, JTU, 4);
                if (RAS_OFF <= 504) a64_stp_off(b, JT2, JT0, JTA, RAS_OFF);
                else { a64_str(b, 8, JT2, JTA, RAS_OFF); a64_str(b, 8, JT0, JTA, RAS_OFF + 8); }
                a64_add_imm(b, 0, JTF, JTF, 1);
                a64_str(b, 4, JTF, 20, RAS_TOP_OFF);
            }
            uint32_t *cont = NULL;
            g_ind_call_cont = &cont;
            g_ind_call_tocont = NULL;
            emit_indirect_tail(b, ic_slot_alloc(), epi_sites, n_epi);
            uint32_t *to_cont = g_ind_call_tocont;
            g_ind_call_cont = NULL;
            if (cont && to_cont) {
                patch_local_adr(adr_site, cont, JT0);
                a64_patch_b(to_cont, a64_label(b));
                uint32_t *pb_ret = emit_body_chain_tail(b, retaddr, 0, epi_sites, n_epi);
                g_jcc_edge[0].target_rip = retaddr;
                g_jcc_edge[0].patch_b = pb_ret;
                g_jcc_edge[0].cond_site = NULL;
                g_jcc_edge[0].kind = EDGE_BODY;
                g_jcc_edge[0].pin_class = 3;
                g_n_jcc_edges = 1;
            } else {
                assert(0 && "indirect call: no continuation site");
            }
            return 1;
        }
    }
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
    if (!g_no_ras) {
        void **rslot = ras_slot_alloc();
        if (rslot) {
            JitBlock *rb = cache_lookup(g_xlat_jit, retaddr, g_xlat_mode32);
            if (rb && rb->code)
                *rslot = ras_entry_for(rb);
            else
                pending_add_ras(jit_key(retaddr, g_xlat_mode32), rslot);
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

    if (g_pe_insns && g_n_pe_real && pin_slot(OCERZ_RSP) >= 0) {
        int hsp = pin_hreg(pin_slot(OCERZ_RSP));
        for (int i = 0; i < g_n_pe_real; i++) {
            const struct JitPushElide *p = &g_pe_real[i];
            if (g_cur_insn_idx <= p->ci || g_cur_insn_idx >= p->rj) continue;
            int64_t delta = 0;
            for (int m = p->ci + 1; m < g_cur_insn_idx; m++) {
                unsigned op2 = g_pe_insns[m].op;
                if (op2 == OCERZ_OP_PUSH || op2 == OCERZ_OP_CALL) delta -= 8;
                else if (op2 == OCERZ_OP_POP || op2 == OCERZ_OP_RET) delta += 8;
            }
            a64_mov_imm64(b, JT1, p->ra);
            a64_str(b, 8, JT1, hsp, (uint32_t)(-delta));
        }
    }

    emit_materialize(b);

    l0_flush_all(b);
    emit_xmm_pin_spill_all(b);
    emit_spill_pinned(b);
    a64_mov_reg(b, 1, 0, 19);
    a64_mov_reg(b, 1, 1, 20);
    {
        const X86Insn *base = g_cur_blk ? g_cur_blk->insns : NULL;
        ptrdiff_t idx = (base && insn >= base && insn < base + g_cur_blk->n_insns) ? insn - base : -1;
        if (idx >= 0 && g_keep && idx < g_keep_n) {
            g_keep[idx] = 1;
            a64_mov_imm64(b, 2, (uint64_t)(uintptr_t)g_cur_blk);
            a64_mov_imm64(b, 3, (uint64_t)idx);
            a64_mov_imm64(b, 16, (uint64_t)(uintptr_t)&ocerz_jit_exec_one_at);
        } else {
            g_no_compact = 1;
            a64_mov_imm64(b, 2, (uint64_t)(uintptr_t)insn);
            a64_mov_imm64(b, 16, (uint64_t)(uintptr_t)&ocerz_jit_exec_one);
        }
    }
    a64_blr(b, 16);
    g_callout_seq++;
    emit_fill_pinned(b);
    emit_xmm_pin_load_all(b);
    exit_sites[*n_exits] = a64_label(b);
    a64_cbnz(b, 0, 0, 0);
    (*n_exits)++;
    emit_reload_jgb(b);
    emit_reload_mem_base(b);
    if (g_pe_insns && g_n_promo_real && pin_slot(OCERZ_RSP) >= 0) {
        int hsp = pin_hreg(pin_slot(OCERZ_RSP));
        for (int i = 0; i < g_n_promo_real; i++) {
            const struct JitPromo *p = &g_promo_real[i];
            if (g_cur_insn_idx <= p->pi || g_cur_insn_idx >= p->qi) continue;
            int64_t delta = 0;
            for (int m = p->pi + 1; m < g_cur_insn_idx; m++) {
                unsigned op2 = g_pe_insns[m].op;
                if (op2 == OCERZ_OP_PUSH || op2 == OCERZ_OP_CALL) delta -= 8;
                else if (op2 == OCERZ_OP_POP || op2 == OCERZ_OP_RET) delta += 8;
            }
            a64_ldr(b, 8, p->hreg, hsp, (uint32_t)(-delta));
        }
    }
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
            if (sigsetjmp(bb, 0) == 0) {
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

static void chain_cond_short(uint32_t *cond_site, void *dst)
{
    if (!cond_site || !dst) return;
    chaincheck("chain_cond_short", dst);
    if (g_xlat_jit && g_xlat_jit->stop_requested) return;
    uint32_t w = *cond_site;
    ptrdiff_t off = (uint32_t *)dst - cond_site;
    uint32_t nw;
    if ((w & 0xff000010u) == 0x54000000u || (w & 0x7e000000u) == 0x34000000u) {
        if (off < -(1 << 18) || off >= (1 << 18)) return;
        nw = (w & ~(0x7ffffu << 5)) | (((uint32_t)off & 0x7ffffu) << 5);
    } else if ((w & 0x7e000000u) == 0x36000000u) {
        if (off < -(1 << 13) || off >= (1 << 13)) return;
        nw = (w & ~(0x3fffu << 5)) | (((uint32_t)off & 0x3fffu) << 5);
    } else return;
    if (g_chain_batching) {
        *cond_site = nw;
        if (g_chain_npatched < CHAIN_BATCH_MAX) g_chain_patched[g_chain_npatched++] = cond_site;
        else sys_icache_invalidate(cond_site, 4);
    } else {
        pthread_jit_write_protect_np(0);
        *cond_site = nw;
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(cond_site, 4);
    }
}
static void chaincheck(const char *what, const void *dst)
{
    static int en = -1;
    if (en < 0) en = getenv("OCERZ_CHAINCHECK") ? 1 : 0;
    if (!en || !dst || !g_xlat_jit) return;
    if ((const uint32_t *)dst < g_xlat_jit->code_base || (const uint32_t *)dst >= g_xlat_jit->code_cur)
        fprintf(stderr, "ocerz: CHAINCHECK[%d] %s target %p OUTSIDE arena [%p,%p)\n",
                (int)getpid(), what, dst, (void *)g_xlat_jit->code_base, (void *)g_xlat_jit->code_cur);
}
static void chain_activate(uint32_t *patch_b, void *dst)
{
    if (!patch_b || !dst)
        return;
    chaincheck("chain_activate", dst);
    if (g_xlat_jit && g_xlat_jit->stop_requested)
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
#define PEND_BITS 12
#define PEND_SIZE (1u << PEND_BITS)
#define PEND_MASK (PEND_SIZE - 1)
static PendingChain *g_pending[PEND_SIZE];

static void pred_add(JitBlock *target, JitBlock *src, int e)
{
    if (!target || !src || target == src) return;
    if (target->n_preds && target->preds[target->n_preds - 1].pb == src &&
        target->preds[target->n_preds - 1].e == e) return;
    if (target->n_preds == target->cap_preds) {
        uint32_t cap = target->cap_preds ? target->cap_preds * 2 : 4;
        void *np = realloc(target->preds, cap * sizeof target->preds[0]);
        if (!np) return;
        target->preds = np;
        target->cap_preds = cap;
    }
    target->preds[target->n_preds].pb = src;
    target->preds[target->n_preds].e = (uint8_t)e;
    target->n_preds++;
}

static void pending_add(uint64_t target_key, uint32_t *patch_b, uint8_t kind,
                        uint8_t pin_class, uint32_t *cond_site, uint64_t src_sig,
                        JitBlock *src, int edge)
{
    PendingChain *e = (PendingChain *)malloc(sizeof *e);
    if (!e)
        return;
    unsigned h = (unsigned)(hash_key(target_key) & PEND_MASK);
    e->target_key = target_key;
    e->patch_b = patch_b;
    e->cond_site = cond_site;
    e->ras_slot = NULL;
    e->src = src;
    e->edge = (uint8_t)edge;
    e->src_sig = src_sig;
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

static void pending_add_ras(uint64_t target_key, void **ras_slot)
{
    PendingChain *e = (PendingChain *)malloc(sizeof *e);
    if (!e)
        return;
    unsigned h = (unsigned)(hash_key(target_key) & PEND_MASK);
    e->target_key = target_key;
    e->patch_b = NULL;
    e->cond_site = NULL;
    e->ras_slot = ras_slot;
    e->src = NULL;
    e->edge = 0;
    e->kind = EDGE_XBLOCK;
    e->pin_class = 0;
    e->next = g_pending[h];
    g_pending[h] = e;
}

static void *body_entry_for(const JitBlock *t, uint64_t src_sig)
{
    static int dis = -1; if (dis < 0) dis = getenv("OCERZ_NO_HOIST_HANDOFF") ? 1 : 0;
    if (!dis && src_sig && t->hoist_sig == src_sig && t->body_noreload && ((src_sig >> 24) & 0xff) == 0)
        return (void *)t->body_noreload;
    return (void *)t->body_code;
}
static void pending_drain(uint64_t key, JitBlock *target)
{
    unsigned h = (unsigned)(hash_key(key) & PEND_MASK);
    PendingChain **pp = &g_pending[h];
    while (*pp) {
        PendingChain *e = *pp;
        if (e->target_key == key) {
            if (e->ras_slot) {
                chaincheck("ras_slot", ras_entry_for(target));
                __atomic_store_n(e->ras_slot, ras_entry_for(target), __ATOMIC_RELEASE);
            }
            else if (e->kind == EDGE_BODY) {
                int compatible = e->pin_class
                    ? target->pin_class == e->pin_class
                    : (target->pin_class == 0 && target->n_pinned == 0);
                if (compatible && target->body_code) {
                    void *dst = body_entry_for(target, e->src_sig);
                    chain_activate(e->patch_b, dst);
                    chain_cond_short(e->cond_site, dst);
                    pred_add(target, e->src, e->edge);
                }
            } else {
                chain_activate(e->patch_b, (void *)target->code);
                pred_add(target, e->src, e->edge);
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
    int extra_stop = poll && !patch_stop && g_n_stop_extra < 6;
    uint32_t *intr = NULL;
    if (!xmm_global_enabled())
        emit_xmm_pin_spill_all(b);
    if (poll && !patch_stop && !extra_stop) {
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
    } else if (extra_stop) {
        stop_extra_add(patch_b, fallback);
    }

    if (g_pin_class == 2)
        a64_add_imm(b, 1, 31, 29, 0);

    emit_side_tag(b, 20);
    a64_mov_imm64(b, JT0, target_rip);
    a64_str(b, 8, JT0, 20, RIP_OFF);
    a64_mov_imm64(b, 0, g_tag_blk ? OCERZ_STEP_PROFILE : OCERZ_STEP_OK);
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
    emit_frame_sp_reset(b);
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
    emit_side_tag(b, 1);
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

static int insn_may_write_gpr(const X86Insn *in, unsigned reg)
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

static int fault_recipe_native_mov(const X86Insn *insn)
{
    if (!g_defer || mem_guard_needed() ||
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

#define OOLSLOW_MAX 32
static struct { uint32_t *sites[3]; int nsites; const X86Insn *insn; uint32_t *back; uint32_t pre; } g_oolslow[OOLSLOW_MAX];
static int g_n_oolslow;
static int oolslow_add(const X86Insn *insn, uint32_t **sites, int nsites, uint32_t *back)
{
    if (g_n_oolslow >= OOLSLOW_MAX || nsites > 3) return 0;
    ea_cache_reset();
    for (int i = 0; i < nsites; i++) g_oolslow[g_n_oolslow].sites[i] = sites[i];
    g_oolslow[g_n_oolslow].nsites = nsites;
    g_oolslow[g_n_oolslow].insn = insn;
    g_oolslow[g_n_oolslow].back = back;
    g_oolslow[g_n_oolslow].pre = g_oolslow_pre;
    g_oolslow_pre = 0;
    g_n_oolslow++;
    return 1;
}
static void patch_any_branch(uint32_t *site, uint32_t *target)
{
    uint32_t w = *site;
    if ((w & 0x7e000000u) == 0x34000000u) a64_patch_cbz(site, target);
    else if ((w & 0x7e000000u) == 0x36000000u) a64_patch_tbz(site, target);
    else if ((w & 0xff000010u) == 0x54000000u) a64_patch_bcond(site, target);
    else a64_patch_b(site, target);
}
static void emit_oolslow_arms(A64Buf *b, uint32_t **exit_sites, int *n_exits)
{
    for (int k = 0; k < g_n_oolslow; k++) {
        int sane = g_oolslow[k].back >= g_push_entry && g_oolslow[k].back < b->p;
        for (int i = 0; sane && i < g_oolslow[k].nsites; i++)
            sane = g_oolslow[k].sites[i] >= g_push_entry && g_oolslow[k].sites[i] < b->p;
        if (!sane) {
            fprintf(stderr, "ocerz: warning: dropping stale oolslow arm (site/back outside the current block)\n");
            continue;
        }
        uint32_t *lbl = a64_label(b);
        for (int i = 0; i < g_oolslow[k].nsites; i++) patch_any_branch(g_oolslow[k].sites[i], lbl);
        if (g_oolslow[k].pre) a64_emit32(b, g_oolslow[k].pre);
        emit_slowcall(b, g_oolslow[k].insn, exit_sites, n_exits);
        uint32_t *here = a64_label(b);
        a64_b(b, (int32_t)(g_oolslow[k].back - here));
    }
    g_n_oolslow = 0;
}

static void emit_misaligned_pieces_st(A64Buf *b, int psize, int n, int rv, int ra, int32_t disp, int s1)
{
    a64_stlur(b, psize, rv, ra, disp);
    for (int i = 1; i < n; i++) {
        a64_lsr_imm(b, 1, s1, rv, i * psize * 8);
        a64_stlur(b, psize, s1, ra, disp + i * psize);
    }
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

static void emit_ordered_slow_arms(A64Buf *b, JitBlock *blk, const uint32_t *entry)
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
        emit_misaligned_arm(b, o);
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

static uint8_t  g_ic_kind[JIT_MAX_BLOCK_INSNS];
static uint64_t g_ic_expect[JIT_MAX_BLOCK_INSNS];
static uint8_t  g_ic_pushelide[JIT_MAX_BLOCK_INSNS];
static int32_t  g_ic_pair_rj[JIT_MAX_BLOCK_INSNS];
static uint8_t  g_promo_reg[JIT_MAX_BLOCK_INSNS];
static int32_t  g_promo_mate[JIT_MAX_BLOCK_INSNS];
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
        const uint8_t *code = (const uint8_t *)ocerz_g2h(pc);
        X86Insn *in = &scratch[*vn];
        if (ocerz_decode_mode(code, 15, pc, in, 0) != OCERZ_OK)
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
            op == OCERZ_OP_FXSAVE || op == OCERZ_OP_FXRSTOR)
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

static int churn_blacklisted(uint64_t rip);

static void compact_block(JitBlock *blk)
{
    int n = blk->n_insns;
    if (!blk->code || !blk->insns || g_no_compact || g_cur_blk != blk ||
        g_keep_n != n || ENV_ON("OCERZ_NO_COMPACT"))
        return;
    if (blk->fault_flags)
        for (int i = 0; i < n; i++) {
            const JitFaultFlagRecipe *r = &blk->fault_flags[i];
            if (r->kind == JFF_NONE) continue;
            if (r->producer < n) g_keep[r->producer] = 1;
            if (r->kind == JFF_ADD_INC_RESULT_SRC && r->producer + 1 < n) g_keep[r->producer + 1] = 1;
        }
    int nk = 0;
    for (int i = 0; i < n; i++) nk += g_keep[i] != 0;
    if (nk > 65535) return;
    JitInsnRef *ref = (JitInsnRef *)malloc((size_t)n * sizeof *ref);
    X86Insn *kept = nk ? (X86Insn *)malloc((size_t)nk * sizeof *kept) : NULL;
    if (!ref || (nk && !kept)) { free(ref); free(kept); return; }
    int k = 0;
    for (int i = 0; i < n; i++) {
        const X86Insn *in = &blk->insns[i];
        ref[i].rip = in->rip; ref[i].op = (uint16_t)in->op; ref[i].len = (uint8_t)in->len;
        ref[i].flags = 0; ref[i].pad = 0; ref[i].keep = 0;
        if (g_keep[i]) { kept[k] = *in; ref[i].keep = (uint16_t)(k + 1); k++; }
    }
    blk->iref = ref; blk->kept = kept; blk->n_kept = (uint16_t)nk;
    free(blk->insns); blk->insns = NULL;
    g_pe_insns = NULL; g_cur_insns = NULL; g_cur_insns_n = 0;
    g_cur_blk = NULL;
}

static JitBlock *translate(OcerzJit *jit, uint64_t rip, int mode32)
{
    g_xlat_mode32 = mode32;

    if (ocerz_exc_trap_rip && rip == ocerz_exc_trap_rip)
        return NULL;
    { extern uint64_t ocerz_cxa_throw_rip; if (ocerz_cxa_throw_rip && rip == ocerz_cxa_throw_rip) return NULL; }

    if (churn_blacklisted(rip)) {
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

    static int g_jitmeasure = -1;
    if (g_jitmeasure < 0)
        g_jitmeasure = getenv("OCERZ_JITMEASURE") ? 1 : 0;
    uint64_t xlat_t0 = g_jitmeasure ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
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
                const uint8_t *code = (const uint8_t *)ocerz_g2h(vpc);
                int rc = ocerz_decode_mode(code, 15, vpc, &scratch[vn], mode32);
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

    g_defer = !g_no_regflags;
    blk->n_edges = 0;
    g_chain_target = 0;
    g_chain_keeps_jgb = 0;
    g_n_raslit = 0;
    g_chain_epi = NULL;
    g_n_jcc_edges = 0;
    g_nzcv_want = 0; g_nzcv_from = -1;
    g_jcc_edge[0].cond_site = NULL; g_jcc_edge[1].cond_site = NULL;
    g_n_oslow = 0;
    g_n_nanool = 0;
    g_n_pe_real = 0;
    g_n_promo_real = 0;
    g_rsp_lag = 0;
    g_pe_insns = blk->insns;
    g_n_call_edges = 0;
    g_n_oolslow = 0;
    g_oolslow_pre = 0;
    g_n_stop_extra = 0;
    g_xlat_jit = jit;
    g_self_rip = rip;
    g_body_entry = NULL;
    g_loop_entry = NULL;
    g_l0_fixed = 0;
    memset(g_l0_fixed_lane, -1, sizeof g_l0_fixed_lane);
    g_stop_patch = NULL;
    g_n_stop_extra = 0;
    g_n_push_fix = 0;
    g_n_oolslow = 0;
    g_cp_guard = ocerz_commpage && (ENV_ON("OCERZ_CP_GUARD_ALL") || cp_marked(jit_key(rip, mode32)));
    { static int all = -1; if (all < 0) all = getenv("OCERZ_AL_GUARD_ALL") ? 1 : 0; if (all) g_al_all = 1; }
    g_align_guard = !g_plain_mem && al_marked(jit_key(rip, mode32));
    g_blk_ordered_loads = 0;
    g_push_entry = NULL;
    g_n_side = 0;
    g_stop_target = NULL;
    g_mem_hoist_greg = -1;
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
    if (!ENV_ON("OCERZ_NO_DISPATCH_STUB")) {
        if (mode32) { if (!jit->dispatch_stub32) emit_dispatch_stub(jit, 1); }
        else        { if (!jit->dispatch_stub)   emit_dispatch_stub(jit, 0); }
    }
    A64Buf b = { jit->code_cur, jit->code_cur, jit->code_end, 0, 0 };
    uint32_t *entry = b.p;
    g_push_entry = entry;

    a64_stp_pre(&b, 29, 30, 31, -16);
    a64_stp_pre(&b, 19, 20, 31, -16);
    a64_mov_reg(&b, 1, 19, 0);
    a64_mov_reg(&b, 1, 20, 1);
    emit_reload_jgb(&b);

    emit_pin_prologue(&b);

    if (rsp_is_ptr()) {
        int rs = pin_slot(OCERZ_RSP);
        assert(rs >= 0);
        a64_mov_imm64(&b, JT0, ocerz_guest_base);
        a64_add_reg(&b, 1, pin_hreg(rs), pin_hreg(rs), JT0, 0);
    }

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
            a64_mov_imm64(&b, 16, (uint64_t)(uintptr_t)&ocerz_jgb_trap);
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
          if (!nofix && selfl && !g_xlat_mode32 && l0_enabled() &&
              sse_enabled() && xmm_global_enabled() && !g_no_regflags)
              l0_fixed_setup(&b, blk->insns, n);
        }
        g_loop_entry = a64_label(&b);
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
    }
    if (ocerz_perfstat > 0) {
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
        if (g_mem_hoist_greg  < 0) freer[nfree++] = JMEMBASE;
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
    unsigned long long l0_last_seq = g_callout_seq;
    g_n_fpbmap = 0;
    int fpb_open = -1;

    int last_flag_def = -1;
    ea_cache_reset();
    for (int i = 0; i < n; i++) {
        const X86Insn *insn = &blk->insns[i];
        g_cur_insn_idx = i;
        g_cur_insn_start = b.p;
        g_cur_need = fl_need[i];
        g_cur_insns = blk->insns; g_cur_insns_n = n;
        g_cur_fpb = fpb_of[i];
        if (g_scpend.valid && g_scpend.idx < i - 1) scalar_pend_flush(&b);
        g_fpb_open = fpb_open;
        g_fpb_fast = fpb_open >= 0 && g_fpb_member[i];
        ea_cache_step(insn, i > 0 ? &blk->insns[i - 1] : NULL);
        g_nzcv_want = 0;
        for (int j = i + 1; j < n && j <= i + 1 + NZCV_GAP_MAX; j++)
            if (nzcv_fuse_producer(blk->insns, j) == i) { g_nzcv_want = 1; break; }
        if (g_callout_seq != l0_last_seq) { l0_flush_all(&b); l0_reset(); l0_last_seq = g_callout_seq; }
        if (!l0_aware_op(insn->op)) {
            for (int k = 0; k < insn->nops; k++)
                if (insn->ops[k].kind == OCERZ_OPK_XMM)
                    l0_flush_reg(&b, insn->ops[k].reg);
            if (insn->op == OCERZ_OP_BLENDVPD || insn->op == OCERZ_OP_BLENDVPS ||
                insn->op == OCERZ_OP_PBLENDVB)
                l0_flush_reg(&b, 0);
            for (int k = 0; k < insn->nops; k++)
                if (insn->ops[k].kind == OCERZ_OPK_XMM && (k == 0 || insn->op == OCERZ_OP_BLENDVPD ||
                    insn->op == OCERZ_OP_BLENDVPS || insn->op == OCERZ_OP_PBLENDVB))
                    l0_inval(insn->ops[k].reg);
            if (insn->op == OCERZ_OP_FXRSTOR || insn->op == OCERZ_OP_SYSCALL) { l0_flush_all(&b); l0_reset(); }
        }
        if (is_terminator(insn->op) ||
            (i + 1 < n && (blk->insns[i + 1].op == OCERZ_OP_JMP ||
                           (blk->insns[i + 1].op == OCERZ_OP_JCC && i + 1 == n - 1))))
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
            l0_flush_all(&b);
                for (int r = 0; r < 16; r++)
                if (fb->ckpt & (1u << r))
                    a64_str_v(&b, 16, xmm_vreg((unsigned)r), 20, FPCKPT_OFF + (uint32_t)r * 16);
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

        if (g_promo_reg[i] != 0) {
            int hsp = pin_hreg(pin_slot(OCERZ_RSP));
            int pr = g_promo_reg[i];
            int gr = pin_hreg(pin_slot(insn->ops[0].reg));
            if (insn->op == OCERZ_OP_PUSH) {
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
        if (!try_inline(&b, insn, fl_need[i], exit_sites, &n_exits)) {
            emit_slowcall(&b, insn, exit_sites, &n_exits);
            blk->n_slow++;
        } else {
            blk->n_inlined++;
        }
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
        if (soff >= -(ptrdiff_t)(1 << 25) && soff <= (ptrdiff_t)((1 << 25) - 1)) {
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
        for (int r = 0; r < 16; r++)
            if (fb->ckpt & (1u << r))
                a64_ldr_v(&b, 16, xmm_vreg((unsigned)r), 20, FPCKPT_OFF + (uint32_t)r * 16);
        g_fpb_fast = 0;
        g_fpb_open = -1;
        l0_reset();
        ea_cache_reset();
        for (int m = fb->first; m <= fb->end; m++) {
            if (blk->insns[m].op == OCERZ_OP_JCC) continue;
            g_cur_insn_idx = m;
            g_cur_need = fl_need[m];
            g_cur_fpb = -1;
            uint32_t *lo = a64_label(&b);
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
        for (int t = 4; t <= 7; t++) {
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
        for (int r = 0; r < 16; r++)
            if (fb->ckpt & (1u << r))
                a64_ldr_v(&b, 16, xmm_vreg((unsigned)r), 20, FPCKPT_OFF + (uint32_t)r * 16);
        g_fpb_fast = 0;
        g_fpb_open = -1;
        l0_reset();
        ea_cache_reset();
        for (int m = fb->first; m <= st->end; m++) {
            if (blk->insns[m].op == OCERZ_OP_JCC) continue;
            g_cur_insn_idx = m;
            g_cur_need = fl_need[m];
            g_cur_fpb = -1;
            uint32_t *lo = a64_label(&b);
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
        for (int t = 4; t <= 7; t++) {
            for (int r = 0; r < 16; r++) {
                if (st->l0[r] != (int8_t)t) continue;
                if (st->l0_dbl[r]) a64_ins_d_d(&b, t, 0, xmm_vreg((unsigned)r), 0);
                else               a64_ins_s_s(&b, t, 0, xmm_vreg((unsigned)r), 0);
                break;
            }
        }
        if (st->fcmp_a >= 0)
            a64_fcmp(&b, st->fcmp_dbl, st->fcmp_a, st->fcmp_b);
        uint32_t *here = a64_label(&b);
        a64_b(&b, (int32_t)(st->back - here));
    }
    emit_oolslow_arms(&b, exit_sites, &n_exits);
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
        cache_insert(jit, blk);
        return blk;
    }

    if (b.overflow) {

        if (!jit->code_full) {
            jit->code_full = 1;
            fprintf(stderr, "ocerz: warning: JIT code arena full (%zu MB, %llu blocks); further blocks run interpreted\n",
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
        if (g_jitdis < 0) {
            const char *p = getenv("OCERZ_JITDIS");
            g_jitdis = p ? 1 : 0;
            const char *lo = getenv("OCERZ_JITDIS_LO"), *hi = getenv("OCERZ_JITDIS_HI");
            g_jd_lo = lo ? strtoull(lo, NULL, 0) : 0;
            g_jd_hi = hi ? strtoull(hi, NULL, 0) : ~0ull;
            if (p) {
                char pb[1024];
                snprintf(pb, sizeof pb, "%s.%d", p, (int)getpid());
                g_jf = fopen(pb, "w");
                if (g_jf) setvbuf(g_jf, NULL, _IOLBF, 0);
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
                for (uint32_t w = 0; w < blk->insn_off[0]; w++)
                    fprintf(g_jf, "    %08x\n", entry[w]);
            }
            for (int i = 0; i < n; i++) {
                uint32_t s = blk->insn_off[i];
                uint32_t e = (i + 1 < n) ? blk->insn_off[i + 1] : epi;
                ocerz_format_insn(&blk->insns[i], tb, sizeof tb);
                fprintf(g_jf, "  INSN %d off=%u words=%u  %s\n", i, s,
                        e > s ? e - s : 0, tb);
                for (uint32_t w = s; w < e; w++)
                    fprintf(g_jf, "    %08x\n", entry[w]);
            }
            fprintf(g_jf, "  EPI off=%u words=%u\n", epi, blk->code_words - epi);
            for (uint32_t w = epi; w < blk->code_words; w++)
                fprintf(g_jf, "    %08x\n", entry[w]);
            fflush(g_jf);
        }
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
    compact_block(blk);
    cache_insert(jit, blk);

    if (!g_no_chain) {
        chain_batch_begin();
        for (int i = 0; i < blk->n_edges; i++) {
            uint32_t *cs = blk->edges[i].probing ? NULL : blk->edges[i].cond_site;
            JitBlock *t = cache_lookup(jit, blk->edges[i].target_rip, blk_mode32(blk));
            if (t && t->code) {
                void *dst = (void *)t->code;
                if (blk->edges[i].kind == EDGE_BODY) {
                    int compatible = blk->edges[i].pin_class
                        ? t->pin_class == blk->edges[i].pin_class
                        : (t->pin_class == 0 && t->n_pinned == 0);
                    if (!compatible || !t->body_code)
                        dst = NULL;
                    else
                        dst = body_entry_for(t, blk->hoist_sig);
                }
                if (dst) {
                    chain_activate(blk->edges[i].patch_b, dst);
                    chain_cond_short(cs, dst);
                    pred_add(t, blk, i);
                }
            } else {
                pending_add(jit_key(blk->edges[i].target_rip, blk_mode32(blk)),
                            blk->edges[i].patch_b, blk->edges[i].kind,
                            blk->edges[i].pin_class, cs, blk->hoist_sig, blk, i);
            }
        }
        pending_drain(blk->key, blk);
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

static int fault_insn_index(const JitBlock *b, const uint32_t *pc);

int ocerz_jit_guest_gprs_at(const struct OcerzVM *vm, const void *host_pc,
                            const uint64_t *host_x, const OcerzCPU *cpu, uint64_t out[16])
{
    const OcerzJit *jit = vm ? vm->jit : NULL;
    const JitBlock *b = fault_block(jit, (const uint32_t *)host_pc);
    if (!b || !host_x)
        return 0;
    int in_callout = host_x[1] == (uint64_t)(uintptr_t)cpu;
    for (int i = 0; i < b->n_pinned; i++) {
        int hr = pin_hreg(i);
        if (in_callout && (hr == 1 || hr == 2))
            continue;
        uint64_t value = host_x[hr];
        if ((b->pin_class == 2 ||
             (b->pin_class == 3 && b->n_insns > 0 && !blk_mode32(b) && rsp_ptr3())) &&
            b->host_holds[i] == OCERZ_RSP)
            value -= ocerz_guest_base;
        out[b->host_holds[i]] = value;
    }
    return 1;
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
        if ((b->pin_class == 2 ||
             (b->pin_class == 3 && b->n_insns > 0 && !blk_mode32(b) && rsp_ptr3())) &&
            b->host_holds[i] == OCERZ_RSP)
            value -= ocerz_guest_base;
        cpu->gpr[b->host_holds[i]] = value;
    }
    if (b->n_push_fix && b->code) {
        uint32_t off = (uint32_t)((const uint32_t *)host_pc - (const uint32_t *)b->code);
        for (int i = 0; i < b->n_push_fix; i++)
            if (b->push_fix[i] == off) { cpu->gpr[OCERZ_RSP] += 8; break; }
    }
    if (b->n_pushelide && b->pushelide && (b->insns || b->iref)) {
        int k = fault_insn_index(b, (const uint32_t *)host_pc);
        for (int i = 0; k > 0 && i < b->n_pushelide; i++) {
            const struct JitPushElide *p = &b->pushelide[i];
            if (k <= p->ci || k >= p->rj) continue;
            int64_t delta = 0;
            for (int m = p->ci + 1; m < k; m++) {
                unsigned op2 = blk_insn_op(b, m);
                if (op2 == OCERZ_OP_PUSH || op2 == OCERZ_OP_CALL) delta -= 8;
                else if (op2 == OCERZ_OP_POP || op2 == OCERZ_OP_RET) delta += 8;
            }
            uint64_t slot = cpu->gpr[OCERZ_RSP] + (uint64_t)(-delta);
            *(uint64_t *)(uintptr_t)(slot + ocerz_guest_base) = p->ra;
        }
    }
}

void ocerz_jit_fault_recover_xmm(const struct OcerzVM *vm, const void *host_pc,
                                 const void *host_v, OcerzCPU *cpu)
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
    const X86Insn *p = blk_insn_full(b, recipe.producer);
    if (!p || p->nops < 1 || p->ops[0].kind != OCERZ_OPK_REG ||
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
        const X86Insn *inc = blk_insn_full(b, recipe.producer + 1);
        if (!inc || inc->op != OCERZ_OP_INC || inc->nops != 1 ||
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

int ocerz_jit_note_commpage_fault(struct OcerzVM *vm, const void *host_pc, uint64_t fault_rip)
{
    OcerzJit *jit = vm ? vm->jit : NULL;
    const uint32_t *pc = (const uint32_t *)host_pc;
    const JitBlock *b = fault_block(jit, pc);
    if (!b) return 0;
    uint64_t block_rip = blk_rip(b);
    cp_mark(b->key);
    cp_mark(jit_key(fault_rip, blk_mode32(b)));
    if (ENV_ON("OCERZ_CP_NOINVAL")) return 1;
    ocerz_jit_invalidate_range(vm, block_rip, 1);
    if (fault_rip != block_rip) ocerz_jit_invalidate_range(vm, fault_rip, 1);
    return 1;
}

int ocerz_jit_note_align_fault(struct OcerzVM *vm, const void *host_pc, uint64_t fault_rip)
{
    OcerzJit *jit = vm ? vm->jit : NULL;
    const uint32_t *pc = (const uint32_t *)host_pc;
    const JitBlock *b = fault_block(jit, pc);
    if (!b || jit->plain_mem) return 0;
    uint64_t block_rip = blk_rip(b);
    al_mark(b->key);
    al_mark(jit_key(fault_rip, blk_mode32(b)));
    ocerz_jit_invalidate_range(vm, block_rip, 1);
    if (fault_rip != block_rip) ocerz_jit_invalidate_range(vm, fault_rip, 1);
    return 1;
}

int ocerz_jit_hotpatch_align(struct OcerzVM *vm, const void *host_pc)
{
    OcerzJit *jit = vm ? vm->jit : NULL;
    uint32_t *site = (uint32_t *)(uintptr_t)host_pc;
    if (!jit || !ocerz_jit_pc_in_arena(vm, host_pc)) return 0;
    uint32_t w = *site;
    if ((w & 0xfc000000u) == 0x14000000u) return 2;
    int is_ld = (w & 0x3fe00c00u) == 0x19400000u;
    int is_st = (w & 0x3fe00c00u) == 0x19000000u;
    if (!is_ld && !is_st) return 0;
    int size = 1 << (w >> 30);
    if (size < 2) return 0;
    int32_t imm9 = (int32_t)((w >> 12) & 0x1ff); if (imm9 & 0x100) imm9 -= 0x200;
    int rn = (int)((w >> 5) & 31), rt = (int)(w & 31);
    if (rn == 31 || rt == 31) return 0;
    int pair = 0;
    if (is_st && size == 8 && rt == JT0 && imm9 <= 247) {
        uint32_t w2 = site[1];
        uint32_t want = (w & ~(0x1ffu << 12) & ~0x1fu) | (((uint32_t)(imm9 + 8) & 0x1ffu) << 12) | (uint32_t)JTU;
        if (w2 == want) pair = 1;
    }
    int cand[3] = { JTF, JTT, JTU }, sc[2], n = 0;
    for (int i = 0; i < 3 && n < 2; i++) if (cand[i] != rt && cand[i] != rn && !(pair && cand[i] == JTU)) sc[n++] = cand[i];
    int ta = sc[0], s1 = sc[1];
    const JitBlock *blk = fault_block(jit, site);
    int use_dmb = blk && blk->ordered_loads;

    pthread_mutex_lock(&jit_lock);
    int rc = 0;
    if ((size_t)(jit->code_end - jit->code_cur) > 128) {
        pthread_jit_write_protect_np(0);
        A64Buf b = { jit->code_cur, jit->code_cur, jit->code_end, 0, 0 };
        uint32_t *arm = b.p;
        if (imm9 > 0)      a64_add_imm(&b, 1, ta, rn, (uint32_t)imm9);
        else if (imm9 < 0) a64_sub_imm(&b, 1, ta, rn, (uint32_t)-imm9);
        else               a64_mov_reg(&b, 1, ta, rn);
        if (pair) a64_try_ands_imm(&b, 1, A64_ZR, ta, 7);
        else emit_granule_cross_test(&b, size, ta, s1);
        uint32_t *bne = a64_label(&b); a64_bcond(&b, A64_NE, 0);
        if (is_ld) a64_ldapur(&b, size, rt, ta, 0);
        else { a64_stlur(&b, size, rt, ta, 0); if (pair) a64_stlur(&b, 8, JTU, ta, 8); }
        uint32_t *back1 = a64_label(&b); a64_b(&b, 0);
        a64_patch_bcond(bne, a64_label(&b));
        if (is_ld) {
            a64_ldr(&b, size, rt, ta, 0);
            a64_dmb_ishld(&b);
        } else if (use_dmb) {
            a64_dmb_ish(&b);
            a64_str(&b, size, rt, ta, 0);
            if (pair) a64_str(&b, 8, JTU, ta, 8);
        } else if (size == 8) {
            a64_try_ands_imm(&b, 1, A64_ZR, ta, 3);
            uint32_t *tob = a64_label(&b); a64_bcond(&b, A64_NE, 0);
            emit_misaligned_pieces_st(&b, 4, 2, rt, ta, 0, s1);
            if (pair) emit_misaligned_pieces_st(&b, 4, 2, JTU, ta, 8, s1);
            uint32_t *tod = a64_label(&b); a64_b(&b, 0);
            a64_patch_bcond(tob, a64_label(&b));
            emit_misaligned_pieces_st(&b, 1, 8, rt, ta, 0, s1);
            if (pair) emit_misaligned_pieces_st(&b, 1, 8, JTU, ta, 8, s1);
            a64_patch_b(tod, a64_label(&b));
        } else {
            emit_misaligned_pieces_st(&b, 1, size, rt, ta, 0, s1);
        }
        uint32_t *back2 = a64_label(&b); a64_b(&b, 0);
        uint32_t *back = site + (pair ? 2 : 1);
        int ok = !b.overflow && a64_try_patch_b(back1, back) && a64_try_patch_b(back2, back);
        if (ok) {
            uint32_t saved = *site;
            *site = 0x14000000u;
            if (a64_try_patch_b(site, arm)) {
                jit->code_cur = b.p;
                sys_icache_invalidate(arm, (size_t)((uint8_t *)b.p - (uint8_t *)arm));
                sys_icache_invalidate(site, 4);
                rc = 1;
            } else {
                *site = saved;
            }
        }
        pthread_jit_write_protect_np(1);
    }
    pthread_mutex_unlock(&jit_lock);
    if (rc == 1 && ocerz_perfstat > 0) ps_align_patches++;
    if (rc == 1 && ENV_ON("OCERZ_ALPATCHLOG")) {
        const uint32_t *arm = (const uint32_t *)((uint8_t *)site + (((int32_t)(*site << 6) >> 6) * 4));
        fprintf(stderr, "ocerz: ALPATCH site=%p w=%08x size=%d rt=%d rn=%d imm=%d pair=%d dmb=%d ta=%d s1=%d arm=%p arm:", (void *)site, w, size, rt, rn, imm9, pair, use_dmb, ta, s1, (const void *)arm);
        for (int i = 0; i < 40 && arm + i < jit->code_cur; i++) {
            fprintf(stderr, " %08x", arm[i]);
            if ((arm[i] & 0xfc000000u) == 0x14000000u) fprintf(stderr, "(->%p)", (const void *)(arm + i + (((int32_t)(arm[i] << 6) >> 6))));
        }
        fprintf(stderr, "\n");
    }
    return rc;
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
    *out_rip = blk_insn_rip(b, i);
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
    out->block_rip = blk_rip(b);
    out->host_word = (uint32_t)(pc - (const uint32_t *)b->code);
    out->insn_index = fault_insn_index(b, pc);
    if (out->insn_index >= 0)
        out->insn_rip = blk_insn_rip(b, out->insn_index);
    out->n_pinned = b->n_pinned;
    out->pin_class = b->pin_class;
    memcpy(out->host_holds, b->host_holds, sizeof(out->host_holds));
    return 1;
}

int ocerz_jit_code_range(struct OcerzVM *vm, const uint32_t **lo, const uint32_t **hi)
{
    OcerzJit *jit = vm ? vm->jit : NULL;
    if (!jit) return 0;
    *lo = jit->code_base;
    *hi = jit->code_cur;
    return 1;
}
int ocerz_jit_owner_pid(struct OcerzVM *vm)
{
    return vm && vm->jit ? vm->jit->owner_pid : -1;
}

void ocerz_jit_forget(struct OcerzVM *vm)
{
    pthread_mutex_init(&jit_lock, NULL);
    g_xlat_jit = NULL;
    g_n_ras_cells = 0;
    g_ras_slot_n = 0;
    memset(g_pending, 0, sizeof g_pending);
    if (vm)
        vm->jit = NULL;
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
    jit->owner_pid = (int)getpid();
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
    free(b->push_fix);
    free(b->pushelide);
    free(b->insns);
    free(b->iref);
    free(b->kept);
    free(b->edges);
    free(b->prof);
    free(b->preds);
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


__thread sigjmp_buf *ocerz_jit_decode_recover;

static void stopcheck(const JitBlock *b, const uint32_t *site, uint32_t insn, const char *what)
{
    static int en = -1;
    if (en < 0) en = getenv("OCERZ_STOPCHECK") ? 1 : 0;
    if (!en || !site) return;
    int64_t off;
    if ((insn & 0xfc000000u) == 0x14000000u)
        off = (int64_t)((int32_t)(insn << 6) >> 6) * 4;
    else if ((insn & 0xff000010u) == 0x54000000u ||
             (insn & 0x7e000000u) == 0x34000000u)
        off = (int64_t)((int32_t)(insn << 8) >> 13) * 4;
    else if ((insn & 0x7e000000u) == 0x36000000u)
        off = (int64_t)((int32_t)(insn << 13) >> 18) * 4;
    else
        off = 0;
    const uint32_t *tgt = (const uint32_t *)((const uint8_t *)site + off);
    const uint32_t *lo = (const uint32_t *)b->code, *hi = lo ? lo + b->code_words : NULL;
    if (!lo || tgt < lo || tgt >= hi)
        fprintf(stderr, "ocerz: STOPCHECK[%d] %s rip=%#llx site=%p insn=%08x tgt=%p block=[%p,%p)\n",
                (int)getpid(), what, (unsigned long long)blk_rip(b), (const void *)site, insn,
                (const void *)tgt, (const void *)lo, (const void *)hi);
}
static int force_stop_sites_writable(OcerzJit *jit)
{
    int patched = 0;
    for (JitBlock *b = jit->stop_blocks; b; b = b->stop_next) {
        stopcheck(b, b->stop_patch, b->stop_insn, "stop");
        for (int i = 0; i < b->n_stop_extra; i++)
            stopcheck(b, b->stop_extra[i].site, b->stop_extra[i].insn, "extra");
        if (ENV_ON("OCERZ_STOPLOG"))
            fprintf(stderr, "ocerz: STOPSITE rip=%#llx patch=%p cur=%08x stop_insn=%08x\n",
                    (unsigned long long)blk_rip(b), (void *)b->stop_patch,
                    b->stop_patch ? *b->stop_patch : 0u, b->stop_insn);
        if (b->stop_patch && b->stop_insn && *b->stop_patch != b->stop_insn) {
            __atomic_store_n(b->stop_patch, b->stop_insn, __ATOMIC_RELEASE);
            patched = 1;
        }
        for (int i = 0; i < b->n_stop_extra; i++)
            if (*b->stop_extra[i].site != b->stop_extra[i].insn) {
                __atomic_store_n(b->stop_extra[i].site, b->stop_extra[i].insn, __ATOMIC_RELEASE);
                patched = 1;
            }
        for (int i = 0; i < b->n_edges; i++) {
            uint32_t *cs = b->edges[i].cond_site;
            int is_stop = b->edges[i].patch_b == b->stop_patch;
            for (int k = 0; k < b->n_stop_extra && !is_stop; k++)
                is_stop = b->edges[i].patch_b == b->stop_extra[k].site;
            if (cs && b->edges[i].cond_orig && *cs != b->edges[i].cond_orig && is_stop) {
                __atomic_store_n(cs, b->edges[i].cond_orig, __ATOMIC_RELEASE);
                patched = 1;
            }
        }
    }
    return patched;
}

static void invalidate_all_locked(OcerzJit *jit)
{
    int patched = 0;

    pthread_jit_write_protect_np(0);
    patched |= force_stop_sites_writable(jit);
    for (size_t i = 0; i < g_n_ras_cells; i++)
        __atomic_store_n(g_ras_cells[i], (void *)NULL, __ATOMIC_RELEASE);
    for (size_t k = 0; k < jit->n_live; k++) {
        JitBlock *b = jit->live[k];
        for (int i = 0; i < b->n_edges; i++) {
            uint32_t *at = b->edges[i].patch_b;
            uint32_t fallback = b->edges[i].fallback_insn;
            if (at && fallback && *at != fallback) {
                __atomic_store_n(at, fallback, __ATOMIC_RELEASE);
                patched = 1;
            }
            uint32_t *cs = b->edges[i].cond_site;
            if (cs && b->edges[i].cond_orig && *cs != b->edges[i].cond_orig) {
                __atomic_store_n(cs, b->edges[i].cond_orig, __ATOMIC_RELEASE);
                patched = 1;
            }
        }
    }
    pthread_jit_write_protect_np(1);
    if (patched)
        sys_icache_invalidate(jit->code_base,
            (size_t)((uint8_t *)jit->code_cur - (uint8_t *)jit->code_base));

    for (size_t k = 0; k < jit->n_live; k++) {
        JitBlock *b = jit->live[k];
        __atomic_store_n(&jit->buckets[hash_key(b->key)], (JitBlock *)NULL, __ATOMIC_RELEASE);
    }
    for (size_t k = 0; k < jit->n_live; k++) {
        JitBlock *b = jit->live[k];
        b->retired_next = jit->retired;
        jit->retired = b;
    }
    jit->n_live = 0;
    jit->code_lo = 0;
    jit->code_hi = 0;
    memset(jit->invmap, 0, sizeof jit->invmap);
    jit->invmap_full = 0;
    gran_clear_all();

    pending_clear();
    for (unsigned i = 0; i < g_ras_slot_n; i++)
        __atomic_store_n(&g_ras_slots[i], NULL, __ATOMIC_RELEASE);
    psc_clear_all();
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

static void invmap_check_reject(const OcerzJit *jit, uint64_t addr, uint64_t len)
{
    for (size_t k = 0; k < jit->n_live; k++) {
        const JitBlock *b = jit->live[k];
        for (int i = 0; i < b->n_insns; i++) {
            if (!ranges_overlap(addr, len, blk_insn_rip(b, i), blk_insn_len(b, i)))
                continue;
            fprintf(stderr, "ocerz: INVMAP MISS[%d] addr=%#llx len=%#llx block=%#llx\n",
                    (int)getpid(), (unsigned long long)addr,
                    (unsigned long long)len, (unsigned long long)blk_rip(b));
            abort();
        }
    }
}

#define CHURN_SLOTS 4096
#define CHURN_LIMIT 3
#define CHURN_QUIET_NS 1500000000ull
static struct { uint64_t page; uint32_t hits; uint64_t last_ns; } g_churn[CHURN_SLOTS];
static void churn_bump(uint64_t rip)
{
    if (g_churn_suppress) return;
    uint64_t page = rip >> 16;
    unsigned i = (unsigned)(page * 0x9E3779B97F4A7C15ull >> 52) & (CHURN_SLOTS - 1);
    for (unsigned n = 0; n < 8; n++, i = (i + 1) & (CHURN_SLOTS - 1)) {
        if (g_churn[i].page == page) {
            g_churn[i].hits++;
            g_churn[i].last_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            if (g_churn[i].hits == CHURN_LIMIT && getenv("OCERZ_CHURNLOG"))
                fprintf(stderr, "ocerz: CHURN[%d] blacklist page=%#llx\n",
                        (int)getpid(), (unsigned long long)(page << 16));
            return;
        }
        if (g_churn[i].page == 0) {
            g_churn[i].page = page;
            g_churn[i].hits = 1;
            g_churn[i].last_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            return;
        }
    }
}
static int churn_blacklisted(uint64_t rip)
{
    uint64_t page = rip >> 16;
    unsigned i = (unsigned)(page * 0x9E3779B97F4A7C15ull >> 52) & (CHURN_SLOTS - 1);
    for (unsigned n = 0; n < 8; n++, i = (i + 1) & (CHURN_SLOTS - 1)) {
        if (g_churn[i].page == page) {
            if (g_churn[i].hits < CHURN_LIMIT)
                return 0;
            uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            if (now - g_churn[i].last_ns > CHURN_QUIET_NS) {
                g_churn[i].hits = CHURN_LIMIT - 1;
                g_churn[i].last_ns = now;
                if (getenv("OCERZ_CHURNLOG"))
                    fprintf(stderr, "ocerz: CHURN[%d] reprieve page=%#llx\n",
                            (int)getpid(), (unsigned long long)(page << 16));
                return 0;
            }
            return 1;
        }
        if (g_churn[i].page == 0) return 0;
    }
    return 0;
}

static uint32_t *branch_word_target(uint32_t *site, uint32_t w)
{
    int64_t off;
    if ((w & 0xFC000000u) == 0x14000000u) {
        off = ((int64_t)(int32_t)(w << 6) >> 6) * 4;
        return (uint32_t *)((uint8_t *)site + off);
    }
    if ((w & 0xFF000010u) == 0x54000000u ||
        (w & 0x7E000000u) == 0x34000000u) {
        off = ((int64_t)(int32_t)(((w >> 5) & 0x7FFFFu) << 13) >> 13) * 4;
        return (uint32_t *)((uint8_t *)site + off);
    }
    return 0;
}

static int ptr_in_block_code(const JitBlock *b, const uint32_t *p)
{
    const uint32_t *lo = (const uint32_t *)(uintptr_t)b->code;
    return p >= lo && p < lo + b->code_words;
}

static int ptr_in_hits(JitBlock *const *hits, size_t n_hits, const uint32_t *p)
{
    if (!p) return 0;
    size_t lo = 0, hi = n_hits;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if ((const uint32_t *)(uintptr_t)hits[mid]->code <= p)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo > 0 && ptr_in_block_code(hits[lo - 1], p);
}

static int hit_code_cmp(const void *pa, const void *pb)
{
    const JitBlock *a = *(JitBlock *const *)pa, *b = *(JitBlock *const *)pb;
    if ((uintptr_t)a->code < (uintptr_t)b->code) return -1;
    return (uintptr_t)a->code > (uintptr_t)b->code;
}

static void retire_hit_blocks_locked(OcerzJit *jit, JitBlock **hits, size_t n_hits)
{
    int any_code = 0;
    for (size_t m = 0; m < n_hits; m++)
        if (hits[m]->code) {
            any_code = 1;
            churn_bump(blk_insn_rip(hits[m], 0));
        }
    if (!any_code) {
        size_t w0 = 0;
        for (size_t k = 0; k < jit->n_live; k++) {
            JitBlock *b = jit->live[k];
            if (!b->inv_hit) { jit->live[w0++] = b; continue; }
            unsigned h = hash_key(b->key);
            JitBlock **pp = &jit->buckets[h];
            while (*pp && *pp != b) pp = &(*pp)->hnext;
            if (*pp == b)
                __atomic_store_n(pp, b->hnext, __ATOMIC_RELEASE);
            gran_block(b, -1);
            b->retired_next = jit->retired;
            jit->retired = b;
        }
        jit->n_live = w0;
        return;
    }
    qsort(hits, n_hits, sizeof *hits, hit_code_cmp);
    pthread_jit_write_protect_np(0);
    for (size_t i = 0; i < g_n_ras_cells; i++)
        __atomic_store_n(g_ras_cells[i], (void *)NULL, __ATOMIC_RELEASE);
    for (size_t k = 0; k < jit->n_live; k++) {
        JitBlock *b = jit->live[k];
        if (!b->inv_hit) continue;
        if (b->stop_patch && b->stop_insn && *b->stop_patch != b->stop_insn) {
            __atomic_store_n(b->stop_patch, b->stop_insn, __ATOMIC_RELEASE);
            sys_icache_invalidate(b->stop_patch, 4);
        }
        for (int i = 0; i < b->n_stop_extra; i++)
            if (*b->stop_extra[i].site != b->stop_extra[i].insn) {
                __atomic_store_n(b->stop_extra[i].site, b->stop_extra[i].insn, __ATOMIC_RELEASE);
                sys_icache_invalidate(b->stop_extra[i].site, 4);
            }
        for (int i = 0; i < b->n_edges; i++) {
            uint32_t *cs = b->edges[i].cond_site;
            int is_stop = b->edges[i].patch_b == b->stop_patch;
            for (int q = 0; q < b->n_stop_extra && !is_stop; q++)
                is_stop = b->edges[i].patch_b == b->stop_extra[q].site;
            if (cs && b->edges[i].cond_orig && *cs != b->edges[i].cond_orig && is_stop) {
                __atomic_store_n(cs, b->edges[i].cond_orig, __ATOMIC_RELEASE);
                sys_icache_invalidate(cs, 4);
            }
        }
    }
    for (size_t k = 0; k < jit->n_live; k++) {
        JitBlock *sblk = jit->live[k];
        if (sblk->inv_hit) continue;
        for (int i = 0; i < sblk->n_edges; i++) {
            uint32_t *at = sblk->edges[i].patch_b;
            uint32_t fallback = sblk->edges[i].fallback_insn;
            if (at && fallback && *at != fallback &&
                ptr_in_hits(hits, n_hits, branch_word_target(at, *at))) {
                __atomic_store_n(at, fallback, __ATOMIC_RELEASE);
                sys_icache_invalidate(at, 4);
            }
            uint32_t *cs = sblk->edges[i].cond_site;
            if (cs && sblk->edges[i].cond_orig && *cs != sblk->edges[i].cond_orig &&
                ptr_in_hits(hits, n_hits, branch_word_target(cs, *cs))) {
                __atomic_store_n(cs, sblk->edges[i].cond_orig, __ATOMIC_RELEASE);
                sys_icache_invalidate(cs, 4);
            }
        }
    }
    pthread_jit_write_protect_np(1);
    size_t w = 0;
    for (size_t k = 0; k < jit->n_live; k++) {
        JitBlock *b = jit->live[k];
        if (!b->inv_hit) { jit->live[w++] = b; continue; }
        unsigned h = hash_key(b->key);
        JitBlock **pp = &jit->buckets[h];
        while (*pp && *pp != b) pp = &(*pp)->hnext;
        if (*pp == b)
            __atomic_store_n(pp, b->hnext, __ATOMIC_RELEASE);
        gran_block(b, -1);
        b->retired_next = jit->retired;
        jit->retired = b;
    }
    jit->n_live = w;
    for (unsigned i = 0; i < g_ras_slot_n; i++)
        __atomic_store_n(&g_ras_slots[i], NULL, __ATOMIC_RELEASE);
    psc_clear_all();
    for (unsigned i = 0; i < g_ic_next && i < JIT_IC_SLOTS; i++) {
        __atomic_store_n(&g_ic_slots[i].code, NULL, __ATOMIC_RELAXED);
        __atomic_store_n(&g_ic_slots[i].rip, 0, __ATOMIC_RELEASE);
    }
}

void ocerz_jit_invalidate_range(struct OcerzVM *vm, uint64_t addr, uint64_t len)
{
    if (!vm || !len || !vm->jit)
        return;

    OcerzJit *jit = vm->jit;
    int invalidated = 0;
    pthread_mutex_lock(&jit_lock);
    if (!jit->code_hi || !ranges_overlap(addr, len, jit->code_lo,
                                         jit->code_hi - jit->code_lo)) {
        if (ENV_ON("OCERZ_INVMAP_CHECK"))
            invmap_check_reject(jit, addr, len);
        pthread_mutex_unlock(&jit_lock);
        return;
    }
    if (addr < jit->code_lo) { len -= jit->code_lo - addr; addr = jit->code_lo; }
    if (addr + len > jit->code_hi) len = jit->code_hi - addr;
    {
        uint64_t g = 1ull << INVMAP_GSHIFT;
        uint64_t alo = addr & ~(g - 1);
        uint64_t ahi = (addr + len + g - 1) & ~(g - 1);
        addr = alo;
        len = ahi - alo;
    }
    {
        static int ivlog = -1;
        static _Atomic unsigned long long ivn;
        if (ivlog < 0) ivlog = getenv("OCERZ_INVLOG") ? 1 : 0;
        if (ivlog && (++ivn & 0xfff) == 0)
            fprintf(stderr, "ocerz: INVQ[%d] n=%llu addr=%#llx len=%#llx nlive=%zu\n",
                    (int)getpid(), (unsigned long long)ivn,
                    (unsigned long long)addr, (unsigned long long)len, jit->n_live);
    }
    if (!invmap_may_hold(jit, addr, addr + len) || !gran_any(addr, addr + len)) {
        if (ENV_ON("OCERZ_INVMAP_CHECK"))
            invmap_check_reject(jit, addr, len);
        pthread_mutex_unlock(&jit_lock);
        return;
    }
    {
        static int noprec = -1;
        if (noprec < 0) noprec = getenv("OCERZ_INV_ALL") ? 1 : 0;
        size_t n_hit = 0, cap = 0;
        JitBlock **hits = NULL;
        for (size_t k = 0; k < jit->n_live; k++) {
            JitBlock *b = jit->live[k];
            uint64_t blo = blk_insn_rip(b, 0);
            uint64_t bhi = blk_insn_rip(b, b->n_insns - 1) + blk_insn_len(b, b->n_insns - 1);
            b->inv_hit = 0;
            if (!ranges_overlap(addr, len, blo, bhi - blo)) continue;
            for (int i = 0; i < b->n_insns; i++)
                if (ranges_overlap(addr, len, blk_insn_rip(b, i), blk_insn_len(b, i))) {
                    b->inv_hit = 1;
                    if (n_hit == cap) {
                        cap = cap ? cap * 2 : 64;
                        JitBlock **nh = (JitBlock **)realloc(hits, cap * sizeof *nh);
                        if (!nh) { free(hits); hits = NULL; n_hit = (size_t)-1; break; }
                        hits = nh;
                    }
                    hits[n_hit++] = b;
                    break;
                }
            if (n_hit == (size_t)-1) break;
        }
        if (n_hit == (size_t)-1) {
            invalidated = 1;
            invalidate_all_locked(jit);
        } else if (n_hit > 0) {
            invalidated = 1;
            if (noprec)
                invalidate_all_locked(jit);
            else
                retire_hit_blocks_locked(jit, hits, n_hit);
        }
        free(hits);
        if (n_hit != (size_t)-1)
            invmap_clear_range(jit, addr, addr + len);
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
    if (ENV_ON("OCERZ_ORDERLOG") && !vm->jit_ordered_required) {
        fprintf(stderr, "ocerz: ORDERED memory required from here (caller %p)\n", __builtin_return_address(0));
        void *bt[8]; int n = backtrace(bt, 8); backtrace_symbols_fd(bt, n, 2);
    }

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
        fprintf(stderr, "ocerz: PERFSTAT[%d]   SLOWOP #%2d %-12s %14llu  %5.2f%% of slow  cum %5.2f%%  (%.2f%% of ALL)  e.g. %s | %s | %s\n",
                (int)getpid(), i + 1, ocerz_op_name(rows[i].op), rows[i].n,
                slow ? 100.0 * (double)rows[i].n / (double)slow : 0.0,
                slow ? 100.0 * (double)cum / (double)slow : 0.0,
                total ? 100.0 * (double)rows[i].n / (double)total : 0.0,
                ps_shapes[rows[i].op][0], ps_shapes[rows[i].op][1], ps_shapes[rows[i].op][2]);
    }
    {
        fprintf(stderr, "ocerz: PERFSTAT[%d]   RAS misses=%llu (stale=%llu)  align-hotpatches=%llu\n", (int)getpid(), ps_ras_miss, ps_ras_stale, ps_align_patches);
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

static void steplog(const OcerzCPU *cpu)
{
    fprintf(stderr, "STEP %#llx", (unsigned long long)cpu->rip);
    for (int i = 0; i < 16; i++) fprintf(stderr, " %llx", (unsigned long long)cpu->gpr[i]);
    fprintf(stderr, "\n");
}
static void chain_edge_now(OcerzJit *jit, JitBlock *blk, int e)
{
    JitBlock *t = cache_lookup(jit, blk->edges[e].target_rip, blk_mode32(blk));
    if (t && t->code) {
        void *dst = (void *)t->code;
        if (blk->edges[e].kind == EDGE_BODY) {
            int compatible = blk->edges[e].pin_class
                ? t->pin_class == blk->edges[e].pin_class
                : (t->pin_class == 0 && t->n_pinned == 0);
            dst = (compatible && t->body_code) ? body_entry_for(t, blk->hoist_sig) : NULL;
        }
        if (dst) {
            chain_activate(blk->edges[e].patch_b, dst);
            if (blk->edges[e].kind == EDGE_BODY)
                chain_cond_short(blk->edges[e].cond_site, dst);
            pred_add(t, blk, e);
            return;
        }
    }
    pending_add(jit_key(blk->edges[e].target_rip, blk_mode32(blk)),
                blk->edges[e].patch_b, blk->edges[e].kind,
                blk->edges[e].pin_class, blk->edges[e].cond_site, blk->hoist_sig, blk, e);
}

static uint64_t g_flip_ns_retire, g_flip_ns_hit, g_flip_n_retire, g_flip_n_hit;
static OcerzJit *g_flip_atexit_jit;
static void flip_report_atexit(void)
{
    fprintf(stderr, "ocerz: FLIPSTAT[%d] trips=%llu hit_ms=%.1f retires=%llu retire_ms=%.1f translated=%llu live=%zu probes=%d\n",
            (int)getpid(), (unsigned long long)g_flip_n_hit, g_flip_ns_hit / 1e6,
            (unsigned long long)g_flip_n_retire, g_flip_ns_retire / 1e6,
            (unsigned long long)(g_flip_atexit_jit ? g_flip_atexit_jit->blocks_translated : 0),
            g_flip_atexit_jit ? g_flip_atexit_jit->n_live : (size_t)0, g_n_probes);
}
static void flip_retire_locked(OcerzJit *jit, JitBlock *blk)
{
    const uint32_t *lo = (const uint32_t *)blk->code, *hi = lo + blk->code_words;
#define IN_BLK(p) ((const uint32_t *)(p) >= lo && (const uint32_t *)(p) < hi)
    size_t idx = jit->n_live;
    for (size_t k = 0; k < jit->n_live; k++)
        if (jit->live[k] == blk) { idx = k; break; }
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
    memmove(&jit->live[idx], &jit->live[idx + 1], (jit->n_live - idx - 1) * sizeof jit->live[0]);
    jit->n_live--;
    gran_block(blk, -1);
    blk->retired_next = jit->retired;
    jit->retired = blk;
    for (unsigned i = 0; i < g_ras_slot_n; i++)
        if (IN_BLK(g_ras_slots[i]))
            __atomic_store_n(&g_ras_slots[i], NULL, __ATOMIC_RELEASE);
    for (size_t i = 0; i < g_n_psc_tables; i++)
        for (int k = 0; k < PSC_N; k++)
            if (IN_BLK(g_psc_tables[i][k].body)) {
                __atomic_store_n(&g_psc_tables[i][k].rip, PSC_EMPTY_RIP, __ATOMIC_RELEASE);
                __atomic_store_n(&g_psc_tables[i][k].body, (void *)NULL, __ATOMIC_RELEASE);
            }
    for (unsigned i = 0; i < g_ic_next && i < JIT_IC_SLOTS; i++)
        if (IN_BLK(g_ic_slots[i].code)) {
            __atomic_store_n(&g_ic_slots[i].code, NULL, __ATOMIC_RELAXED);
            __atomic_store_n(&g_ic_slots[i].rip, 0, __ATOMIC_RELEASE);
        }
#undef IN_BLK
}

static void flip_retire_block(struct OcerzVM *vm, OcerzJit *jit, JitBlock *blk)
{
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    g_flip_n_retire++;
    pthread_mutex_lock(&jit_lock);
    int live = blk->code != NULL;
    if (live) flip_retire_locked(jit, blk);
    pthread_mutex_unlock(&jit_lock);
    if (live) ocerz_vm_purge_jit_ras(vm);
    g_flip_ns_retire += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
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
        pthread_jit_write_protect_np(0);
        __atomic_store_n(pf->ft_site, A64_NOP, __ATOMIC_RELEASE);
        __atomic_store_n(pf->tk_trip, A64_NOP, __ATOMIC_RELEASE);
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(pf->ft_site, 4);
        sys_icache_invalidate(pf->tk_trip, 4);
    }
    return flip;
}

static void flip_side_hit(struct OcerzVM *vm, OcerzJit *jit, OcerzCPU *cpu)
{
    JitBlock *blk = (JitBlock *)cpu->side_blk;
    int k = cpu->side_idx;
    cpu->side_blk = NULL;
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
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    g_flip_n_hit++;
    JitProf *pf = &blk->prof[k];
    int tk = (int)pf->taken, ft = (int)pf->ft;
    int verdict = tk >= 2 * ft && tk >= (1 << (PROBE_BIT - 1));
    int flip = 0;
    pthread_mutex_lock(&jit_lock);
    if (pf->windows == 0 || (pf->prev != verdict && pf->windows < 3)) {
        pf->prev = (uint8_t)verdict;
        pf->windows++;
        pf->taken = pf->ft = 0;
        pthread_mutex_unlock(&jit_lock);
        return;
    }
    flip = flip_decide_locked(blk, e, tk, ft, fliplog);
    if (!flip) chain_edge_now(jit, blk, e);
    pthread_mutex_unlock(&jit_lock);
    g_flip_ns_hit += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    static int noretire = -1;
    if (noretire < 0) noretire = getenv("OCERZ_FLIP_NORETIRE") ? 1 : 0;
    if (flip && !noretire) flip_retire_block(vm, jit, blk);
}

int ocerz_jit_step(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (cpu->rip - OCERZ_DYLDAPI_LO < (OCERZ_DYLDAPI_HI - OCERZ_DYLDAPI_LO))
        return OCERZ_EUNSUP;
    { static int sl = -1; if (sl < 0) sl = getenv("OCERZ_STEPLOG") ? 1 : 0; if (sl) steplog(cpu); }
    OcerzJit *jit = vm->jit;
    if (cpu->side_blk) flip_side_hit(vm, jit, cpu);
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

    JitBlock *b = cache_lookup(jit, cpu->rip, cpu->mode32);
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
        b = cache_lookup(jit, cpu->rip, cpu->mode32);
        if (!b) {
            if (ocerz_jitstat > 0) js_xlat++;
            g_plain_mem = jit->plain_mem;
            b = translate(jit, cpu->rip, cpu->mode32);
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
    {
        static int cc = -1;
        if (cc < 0) cc = getenv("OCERZ_CODECHECK") ? 1 : 0;
        if (cc && ((uint32_t *)b->code < jit->code_base || (uint32_t *)b->code >= jit->code_cur)) {
            fprintf(stderr, "ocerz: CODECHECK[%d] blk=%p rip=%#llx code=%p OUTSIDE arena [%p,%p) "
                    "n_insns=%d pin_class=%d n_pinned=%d body=%p noreload=%p hoist_sig=%#llx key_field=%#llx code_words=%u\n",
                    (int)getpid(), (void *)b, (unsigned long long)cpu->rip, (void *)b->code,
                    (void *)jit->code_base, (void *)jit->code_cur, b->n_insns, b->pin_class,
                    b->n_pinned, (void *)b->body_code, (void *)b->body_noreload,
                    (unsigned long long)b->hoist_sig, (unsigned long long)b->key, b->code_words);
            fprintf(stderr, "ocerz: CODECHECK[%d] blk words:", (int)getpid());
            for (int i = 0; i < 24; i++)
                fprintf(stderr, " %llx", (unsigned long long)((const uint64_t *)(const void *)b)[i]);
            fprintf(stderr, "\n");
            return OCERZ_STEP_FATAL;
        }
    }
    return b->code(vm, cpu);
}

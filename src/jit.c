/*
 * src/jit.c
 *
 * The JIT tier: a basic-block dynamic translator that emits native arm64
 * code at runtime, the way Rosetta 2's JIT path does, with the interpreter
 * as the semantic engine for every instruction it does not inline.
 *
 * Model. A block is the straight-line run of guest instructions starting at
 * an entry rip and ending at (and including) the first control-flow or
 * system instruction, a decode failure, or a length cap. Each block is
 * decoded ONCE; its decoded X86Insn array is stored alongside the emitted
 * code and lives for the process lifetime (block cache keyed by guest rip,
 * open-chained hash table, never invalidated — correct for the static
 * binaries Ocerz runs today; self-modifying code is out of scope and noted).
 * The cache lookup is lock-free: buckets are read with an acquire load and
 * inserts publish the new head with a release store, so the hot path (a cache
 * hit) takes no lock; the jit_lock is taken only on a miss, to serialize the
 * single-writer translate/emit. Because blocks are never freed and a node's
 * hnext is set before it is published, a concurrent reader never sees a torn
 * chain even while another thread translates.
 *
 * Emitted code is call-threaded. Each block is a native function
 *   int block(OcerzVM *vm in x0, OcerzCPU *cpu in x1)
 * that, after a standard prologue saving fp/lr and the two callee-saved
 * registers x19 (=vm) and x20 (=cpu), walks its instructions. For each one
 * it either INLINES the operation directly against the memory-backed
 * register file at [x20 + reg*8] (the fast paths below) or emits a call to
 * ocerz_jit_exec_one(vm, cpu, &insns[i]) — the slow path that runs the exact
 * interpreter semantics. After every slow-path call a cbnz exits the block
 * when the status is not STEP_OK, so guest exit, fatal errors, and the
 * post-branch return to the dispatcher all unwind cleanly.
 *
 * RIP threading is automatic and needs no per-instruction emission: every
 * decoded insn carries its own guest rip, so ocerz_jit_exec_one sets
 * cpu->rip = insn->rip + insn->len before dispatching. Inlined instructions
 * therefore never need to touch cpu->rip — the next slow-path call restores
 * the architectural value, and inlined ops never read it.
 * This is what makes inlining safe without a full register/flag model.
 *
 * There is ONE consequence of that, and it is not a detail: an inlined
 * instruction that FAULTS cannot report its own rip, because it never wrote
 * one. cpu->rip (and cpu->cur_rip) still name some earlier slow instruction.
 * The fault handler therefore recovers the exact faulting rip from a per-block
 * host-pc -> guest-rip side table (blk->insn_off; see ocerz_jit_fault_rip),
 * which costs zero instructions on the hot path and is only ever walked from a
 * cold trap. Emitting a rip store before every inlined memory access would have
 * been the alternative, and would have taxed exactly the path this tier exists
 * to keep empty.
 *
 * The one place rip is NOT restored by a slow call is a block that ends by
 * running off the JIT_MAX_BLOCK_INSNS length cap instead of on a terminator:
 * its last instruction is an ordinary (non-control-flow) op that may have been
 * inlined, so nothing wrote rip and the fall-through exit would leave it
 * pointing inside the block (re-executing the tail and corrupting state — this
 * is exactly what mistranslated long unrolled crypto transforms). The
 * translator therefore stores the fall-through rip explicitly at block end for
 * any block whose final instruction is not a terminator. That store sits on
 * the fall-through path only, before the shared exit label, so the early-exit
 * paths (a slow call returning non-OK, and the native Jcc) skip it and keep
 * the rip they computed.
 *
 * Inlined fast paths (chosen for frequency and bit-exact simplicity):
 *   - NOP/PAUSE/PREFETCH/CLFLUSH: emit nothing.
 *   - MOV reg<-reg and reg<-imm at operand size 4 or 8, high-byte-free: a
 *     ldr/str (size 4 loads through a w-register so the 64-bit slot is
 *     zero-extended exactly as x86 requires) or a movimm+str.
 *   - MOV with ONE MEMORY OPERAND at size 4/8 (emit_mov_mem), and the ALU ops
 *     with a memory SOURCE (emit_arith_mem): the address is computed natively,
 *     range-guarded against the commpage/low-shadow/top windows at runtime
 *     (emit_commpage_guard reproduces all four ocerz_g2h arms), and the access
 *     is a TSO-ordered load/store. Memory-DESTINATION read-modify-write
 *     arithmetic, locked ops, 8/16-bit and segment-override forms stay slow.
 *   - PUSH/POP at opsize 8 with a plain GPR/imm operand (emit_push_pop) and
 *     MOVSXD (emit_movsxd). Both set no flags. Measured as 60.5% and 8.3% of
 *     the slow path respectively in a real Cocoa app -- the largest wins
 *     available, and the cheapest to make bit-exact precisely because no flag
 *     is involved.
 *   - LEA reg<-mem (4/8-bit dest, no segment): the effective address is
 *     materialized with movimm/ldr/add against the register file and stored,
 *     32-bit dests truncated by a w-move; no flags.
 *   - The hot register/immediate ALU ops at operand size 4 or 8, high-byte-
 *     free: ADD SUB CMP AND OR XOR TEST INC DEC, and the constant-count
 *     SHL/SHR/SAR. The arm64 op runs in a w- or x-register (a w-op zero-
 *     extends, matching x86's 32-bit upper-clear), the result is stored to
 *     the slot for non-comparison forms, and the six x86 arithmetic flags
 *     are computed natively into cpu->rflags -- but ONLY the ones a backward
 *     liveness pass proves something downstream reads (see LAZY FLAGS below).
 *     A mixed block of inlined and slow-path instructions stays coherent
 *     because every slow-path op is modelled as reading all six flags, so a
 *     flag is never dead across one.
 *   - The Jcc terminator: it reads cpu->rflags (current for every flag it can
 *     read, since a block's last instruction is forced live-out=all), reproduces
 *     ocerz_cc_eval's predicate tree, csel's cpu->rip between the two
 *     compile-time-constant target rips (taken = ops[0].imm, fallthrough =
 *     rip+len), and returns STEP_OK so the dispatcher re-enters at the new
 *     rip. A native terminator must set the return value to STEP_OK itself,
 *     since no preceding slow call is guaranteed to have left it there.
 *
 * LAZY FLAGS. The emitters above compute only the flags a backward liveness
 * pass (in translate(), over the def/use table in src/flags_live.c) proves
 * something downstream actually reads. Inside a block each arithmetic op's
 * flags are almost always killed by the next one: on the alu benchmark's hot
 * 26-instruction block, 101 of 107 flag writes (94.4%) are dead, and dropping
 * them takes that block from 644 emitted arm64 words to 142 (24.8 -> 5.5 per
 * guest instruction).
 *
 * Soundness rests on three rules, all conservative in the same direction:
 *   - a block's LAST instruction is forced live-out = all six (no cross-block
 *     analysis exists, and a successor/handler/interpreter may read anything);
 *   - every op not in the whitelist reads all six and kills none, so an
 *     unmodelled instruction degrades to the old eager behaviour, never to a
 *     wrong answer;
 *   - anything that can FAULT reads all six, because a signal handler is
 *     unpredicted control flow that can observe rflags in its mcontext.
 * OCERZ_NO_LAZYFLAGS=1 restores eager emission for A/B measurement.
 *
 * MEASURED, and worth knowing before optimizing further: that 4.5x reduction in
 * emitted words bought only 1.45x of wall clock on alu (3.32s -> 2.30s, same
 * binary, env-switched). Emitted-instruction count is therefore NOT the currency
 * it looks like -- IPC fell from ~4.85 to ~1.55, i.e. the eager flag code was
 * independent work that filled issue slots, and removing it exposed the guest's
 * own serial dependency chain through the MEMORY-BACKED register file as the
 * real critical path. The next real lever is keeping guest GPRs in host
 * registers (shortening that chain), not emitting fewer instructions.
 *
 * Native flag emission matches src/flags.c bit for bit (the differential
 * gate over PUSHF-captured rflags enforces this). The translator computes
 * ZF/SF from the arm64 NZCV the sized op produced, PF by an XOR-fold of the
 * result's low byte, AF from a^b^res bit 4, and CF/OF from the arm64
 * carry/overflow flags WITH the x86 fixups: ADD takes CF from arm64 C, but
 * SUB/CMP/INC-borrow take CF from arm64 !C (carry-clear), the classic
 * inversion; INC/DEC preserve CF and set OF/AF positionally; the constant
 * shifts read the shifted-out bit by a fixed bitfield extract. Logic ops
 * clear CF/OF/AF. Everything else — memory-destination arithmetic, locked
 * ops, multiply/divide, rotates, SSE, x87, strings, syscalls, calls and
 * returns — runs through ocerz_jit_exec_one, i.e. the interpreter, so the
 * JIT is correct by construction and the differential test (every guest
 * binary under -no-jit vs JIT) holds it to that.
 *
 * On what is NOT inlined, and why, from MEASUREMENT rather than intuition (a
 * real Cocoa app, 3.5e9 slow-path executions, exact counters): CALL+RET are
 * 11.4% of the slow path and remain the largest un-inlined item. ALL SSE
 * combined is ~1.2% of the slow path (~0.7% of on-CPU) — inlining it would buy
 * under a percent for a 128-bit register-file rewrite, so it is deliberately
 * left alone. Memory-destination RMW arithmetic is <=1%. TEST is 5.4% of the
 * slow path but 0% of it is in an inlineable shape: it is all `test al,al` /
 * `test byte [x],imm`, i.e. it needs genuine 8/16-bit flag emission, not a
 * relaxed guard. Do not "optimize" these on vibes; re-measure first.
 *
 * The win over the pure interpreter is the eliminated per-execution decode
 * (the largest interpreter cost) and the collapsed dispatch: a hot loop
 * pays one hash lookup and one native call-chain per iteration instead of a
 * decode+dispatch per instruction. Block chaining (patching a block's tail
 * to jump straight to its successor) is a further optimization left for
 * later; the interfaces here do not change when it lands.
 *
 * W^X: one MAP_JIT region is bump-allocated for all block code. Emission
 * toggles pthread_jit_write_protect_np off, writes the block, toggles it
 * back on, and sys_icache_invalidate publishes the new code, per Apple
 * Silicon's split-cache and write-protect rules.
 */
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

/* RESERVED, not committed: MAP_ANON pages take physical backing only on first
 * write, so RSS still tracks bytes actually emitted — the reservation costs
 * address space, not memory. This mirrors JSC's FixedVMPool and V8's code
 * range, and is why they reserve rather than grow: one auditable placement.
 * 64MB was sized for test programs. Measured on Mousecape: 340 B/block (the
 * honest cost of x86 flag emulation), filling 64MB with 197,156 blocks in the
 * FIRST 8s of a 170s startup. 1GB holds ~3.1M blocks, ~16x that working set. */
#define JIT_CODE_BYTES_DEFAULT (1024ull << 20)

/* OCERZ_JIT_CODE_MB sizes the reservation. OCERZ_JIT_CODE_KB is the finer-grain
 * TEST knob and wins when both are set: the guest test programs emit far less
 * than 1MB of code, so OCERZ_JIT_CODE_MB=1 never fills the arena and cannot
 * exercise the interpreter tier — only a KB-scale arena forces demotion and
 * turns the differential suite into a compiled-tier vs demoted-tier equivalence
 * test. Rounded up to a page; the mapping must be at least one page. */
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

/* 2^20 buckets. Measured on Mousecape at the window: 315,557 live blocks in the
 * old 65,536 buckets = load factor 4.82, so each of ~1.47B cache_lookup calls
 * pointer-chased ~4.8 chained nodes -- and cache_lookup is the whole of the
 * dispatcher's hot path. 2^20 drops the load factor to ~0.30 (~1 probe). The
 * cost is 8MB of calloc'd bucket array, lazily faulted, i.e. address space and
 * only the touched pages. Blocks are never freed, so the table never rehashes;
 * this is purely chain length, not behaviour. */
#define JIT_HASH_BITS 20
#define JIT_HASH_SIZE (1u << JIT_HASH_BITS)
#define JIT_HASH_MASK (JIT_HASH_SIZE - 1)
#define JIT_MAX_BLOCK_INSNS 256

typedef int (*JitBlockFn)(struct OcerzVM *, OcerzCPU *);

typedef struct JitBlock {
    uint64_t guest_rip;
    JitBlockFn code;
    X86Insn *insns;
    int n_insns;
    struct JitBlock *hnext;
    /* OCERZ_PERFSTAT (TEMPORARY): incremented by the block's own prologue (a
     * plain non-atomic ldr/add/str -- a lost update under contention costs a
     * count, never correctness) and by jit_interp_block for the demoted tier.
     * n_inlined/n_slow are the block's STATIC composition, so exec_count scales
     * them into executed-instruction counts. */
    uint64_t exec_count;
    int n_inlined;
    int n_slow;
    /* Host-pc -> guest-rip side table, n_insns entries, ASCENDING (emission is
     * sequential, so it is sorted by construction). insn_off[i] is the offset in
     * INSTRUCTION WORDS from `code` at which insn i's emitted code begins, so a
     * host pc inside the block resolves to the guest instruction that owns it by
     * binary search. Exists so a fault in INLINED code (which never writes
     * cpu->rip) still reports the exact faulting rip. 4 bytes per guest insn --
     * ~40 B/block, ~12MB at Mousecape's 315k blocks -- and zero instructions on
     * the hot path, which is the entire point. NULL for a demoted block (no
     * code: every insn is a slow call and sets cur_rip itself). */
    uint32_t *insn_off;
    uint32_t code_words;        /* length of the emitted code, in words */
    /* ---- REGISTER PINNING (phase 2) -------------------------------------
     * A hot subset of the guest GPRs is kept in the callee-saved host regs
     * x21..x28 for the lifetime of a block: the prologue loads them from
     * cpu->gpr[], every operand read/write inside the block uses the host reg
     * instead of a [x20+off] memory access, and every block EXIT (epilogue,
     * RAS tail-call) and every slow call stores them back so cpu->gpr[] and
     * the interpreter stay authoritative. host_holds[i] is the guest GPR index
     * (0..15) pinned into x21+i for i<n_pinned; guest_in_host[g] is the slot
     * 0..7 holding guest GPR g, or -1 when g is memory-resident. Correctness
     * never depends on a register being pinned -- an unpinned reg simply keeps
     * the old memory path -- so >8 live regs, high8 sub-registers, and every
     * un-inlined op degrade safely. n_pinned==0 makes the whole mechanism inert
     * (OCERZ_NO_REGFLAGS, or a block worth nothing to pin). */
    uint8_t host_holds[8];
    int8_t guest_in_host[16];
    uint8_t n_pinned;
    /* ---- BLOCK CHAINING (Increment 1: direct-edge) ---------------------
     * A chainable STATIC successor edge. patch_b points at a single B word
     * living inside this block's emitted code (the chain-tail's activation
     * branch) that INITIALLY targets an in-block fallback (which restores
     * STEP_OK and returns to the dispatcher). When the successor at
     * target_rip is compiled, that one word is rewritten to branch directly
     * into the successor's host entry, removing the dispatcher round-trip.
     * kind is EDGE_XBLOCK for Increment 1 (a different-block edge chained at
     * the post-teardown seam; the source's epilogue has already flushed pins
     * to cpu->gpr[] and balanced the frame, so any source/target pin-map pair
     * is correct). Populated only for a compiled block; n_edges==0 otherwise. */
    struct {
        uint64_t target_rip;
        uint32_t *patch_b;      /* the one activation B word, in [code,code+code_words) */
        uint8_t kind;
    } edges[2];
    uint8_t n_edges;
} JitBlock;

enum { EDGE_XBLOCK = 0, EDGE_SELFLOOP = 1 };

struct OcerzJit {
    struct OcerzVM *vm;
    uint32_t *code_base;
    uint32_t *code_cur;
    uint32_t *code_end;
    size_t code_bytes;
    int code_full;              /* one-shot log latch */
    JitBlock *buckets[JIT_HASH_SIZE];
    uint64_t blocks_translated;
    /* Compiled blocks in CODE-ADDRESS order, for the fault handler's host-pc ->
     * block lookup. Append-only and sorted by construction: code is bump-
     * allocated, so each successive compiled block starts above the last, and
     * blocks are never freed. Appended under jit_lock (single writer);
     * ci_n is published with a release store so a concurrent faulting thread
     * either does not see an entry or sees it fully initialized. */
    JitBlock **ci;
    size_t ci_cap;
    size_t ci_n;
};

/* ---- OCERZ_PERFSTAT: TEMPORARY measurement instrumentation (log-only, gated) ----
 * Question: of the guest instructions actually EXECUTED, what fraction goes
 * through ocerz_jit_exec_one (the interpreter slow path) rather than inlined
 * native code, and WHICH OcerzOps are they? ps_ops is exact -- every slow-path
 * execution passes through here, so the histogram is a direct count, not a
 * sample. Inlined executions are derived from per-block exec_count * n_inlined.
 * All counters are relaxed atomics touched only when the env var is set. */
int ocerz_perfstat = -1;
/* OCERZ_FLAGLIVE: dump per-block flag-liveness effectiveness at translate time.
 * Set once at jit init, read only from the cold translate path. */
static int g_flaglive_log;
/* OCERZ_NO_LAZYFLAGS: force eager flag emission (see the liveness pass). */
static int g_no_lazyflags;
/* OCERZ_NO_RAS: kill switch for the STEP 1 return-address-stack predictor. When
 * set the JIT emits neither the CALL-side RAS push nor the RET-side fast return,
 * so CALL/RET behave exactly as the committed inlined form (return STEP_OK to
 * ocerz_jit_step's hash dispatch). Read once at jit init; translate() runs under
 * jit_lock and this is a whole-run decision, mirroring g_no_lazyflags. */
static int g_no_ras;
/* OCERZ_NO_REGFLAGS: A/B kill switch for the whole phase-2 pair. When set it
 * disables BOTH register pinning AND deferred flags (the emitters fall back to
 * eager flag computation and the operand path never consults the pin map), so
 * a single env var on the SAME binary reproduces the pre-pair (HEAD) codegen
 * for measurement. Read once at jit init; translate() runs under jit_lock and
 * this is a whole-run decision, mirroring g_no_lazyflags/g_no_ras. */
static int g_no_regflags;
/* OCERZ_NO_CHAIN: A/B kill switch for block chaining (Increment 1). When set,
 * translate() emits no chain tails and activates no edges, so every block exit
 * returns through the dispatcher exactly as the pre-chaining (committed) code
 * did. Read once at jit init; whole-run decision, mirroring g_no_ras. */
static int g_no_chain;
/* OCERZ_NO_JCCLINK: A/B kill switch for JUST the general two-way Jcc block linking
 * (this lever), leaving the committed self-loop and CALL-imm chaining active. When
 * set, a non-self-loop Jcc falls back to the committed csel + shared dispatcher
 * exit and records no Jcc arm edges, so the SAME binary isolates exactly this
 * lever's contribution against the pre-lever codegen. Read once at jit init;
 * whole-run decision, mirroring g_no_chain. */
static int g_no_jcclink;
/* Per-block chain context (single-writer under jit_lock, like g_pin). A CALL(imm)
 * terminator records its direct-target rip here and the address of its terminal
 * STEP_OK epilogue branch; translate() then redirects that branch into an
 * emitted chain tail and records the resulting edge. g_chain_target==0 means the
 * block has no chainable terminator. */
static uint64_t g_chain_target;
static uint32_t *g_chain_epi;   /* the CALL's terminal `b` word (initially -> exit_label) */
/* Self-loop chaining context (Increment 2), same single-writer discipline. Set by
 * translate() before the per-instruction loop: g_self_rip is the block's entry rip
 * and g_body_entry is the host label immediately AFTER the prologue (frame save +
 * pinned-reg fill). emit_jcc chains a Jcc whose taken target == g_self_rip straight
 * back to g_body_entry (past the prologue), so the pinned guest GPRs stay resident
 * in x21..x28 across the loop instead of being spilled/reloaded every iteration.
 * g_body_entry==NULL disables self-loop chaining for the block. */
static uint64_t g_self_rip;
static uint32_t *g_body_entry;
/* GENERAL TWO-WAY Jcc LINKING context (single-writer under jit_lock, like
 * g_chain_target). A Jcc block whose self-loop fast path did NOT fire records up
 * to two outgoing edges here -- the taken and fall-through arms, each with its own
 * chain-tail activation word -- which translate()'s finalize publishes into
 * blk->edges[]. A block terminates on EXACTLY ONE terminator, so g_n_jcc_edges>0
 * and g_chain_target!=0 are mutually exclusive (CALL-XOR-Jcc); finalize asserts
 * it. g_n_jcc_edges==0 means the Jcc left both arms at their in-tail fallback (an
 * unchained dispatcher exit) -- always correct. */
static struct { uint64_t target_rip; uint32_t *patch_b; } g_jcc_edge[2];
static int g_n_jcc_edges;
/* The jit being translated, published for emit_jcc's back-edge classifier (STEP
 * 2): an arm whose target is ALREADY a compiled block (cache_lookup ->code) is a
 * back-edge and must emit the interrupt poll before its chain tail. Set by
 * translate() before the per-instruction loop; single-writer under jit_lock. */
static OcerzJit *g_xlat_jit;
/* Per-block pin map, published by translate() for the emit helpers below. Only
 * ever set/read under jit_lock (single-writer translate), so plain statics are
 * race-free. g_pin==NULL means no register is pinned for the block being
 * emitted (the memory path is used everywhere). */
static int8_t *g_pin;          /* -> blk->guest_in_host[16], or NULL */
static uint8_t *g_pin_hold;    /* -> blk->host_holds[8] */
static int g_n_pinned;
/* Per-block flag policy, published by translate() alongside the pin map. When
 * true the block DEFERS flags (and materializes at each consumer); when false
 * it uses eager inline flag emission. Deferral is tied to the SAME size gate as
 * pinning: the two are a pair (a deferred producer's win is realized by keeping
 * its operands in pinned host regs), and small flag-CONSUMING blocks -- br's
 * data-dependent Jcc fragments, fib's compare/branch -- pay the materialize cost
 * on almost no work and regress under deferral. So small blocks (and the whole
 * OCERZ_NO_REGFLAGS=1 baseline) keep HEAD's eager path; only the large self-loops
 * that also pin get deferral. */
static int g_defer;
static _Atomic unsigned long long ps_ops[OCERZ_OP_COUNT];
static _Atomic unsigned long long ps_slow_insns;
static _Atomic unsigned long long ps_steps, ps_hits, ps_misses;
/* [0]=push [1]=pop [2]=test [3]=movsxd [4]=call [5]=ret; column 0 = easy shape. */
static _Atomic unsigned long long ps_shape[6][2];
static const char *ps_shape_name[6] = { "push", "pop", "test", "movsxd", "call", "ret" };
static uint64_t ps_t0;

/* OUTLINED, and that is the whole point. Formatting needs a char buf[128], and
 * an inline buf[128] forces a ~208-byte frame, three stp/ldp pairs and a stack-
 * protector canary onto ocerz_jit_exec_one -- the hottest function in the
 * emulator, entered once per slow-path instruction (3.5 BILLION times in a
 * 151s Mousecape startup) for a branch that is not taken unless -trace is on.
 * Moving the body out of line leaves exec_one a leaf-shaped function that can
 * tail-call ocerz_interp_exec with no frame at all. noinline is load-bearing:
 * without it the compiler is free to pull buf[128] back in and silently restore
 * the frame. cold keeps it off the hot icache path. */
static __attribute__((noinline, cold, preserve_most)) void jit_trace_one(const X86Insn *insn)
{
    char buf[128];
    ocerz_format_insn(insn, buf, sizeof buf);
    fprintf(stderr, "ocerz: %#llx: %s\n", (unsigned long long)insn->rip, buf);
}

/* Likewise outlined: the shape/histogram counters are ~20 instructions and a
 * pile of constants that exist only under OCERZ_PERFSTAT. Inline, they bloat
 * exec_one's frame and icache footprint on every one of those 3.5B calls. */
static __attribute__((noinline, cold, preserve_most)) void jit_perfstat_one(const X86Insn *insn)
{
    ps_slow_insns++;
    unsigned o = insn->op;
    if (o < OCERZ_OP_COUNT)
        ps_ops[o]++;
    /* Shape breakdown for the ops the histogram says dominate: an op only
     * pays off to inline in the FORMS it actually appears in, so count how
     * many are the easy shape (opsize 8, plain GPR/imm operand, no segment
     * override) versus the awkward remainder. */
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
        /* The easy shape differs per op, and conflating them is how RET came to
         * read "never inlineable": the old condition was
         *   (o == OCERZ_OP_CALL && ops[0].kind == IMM)
         * which is ALWAYS false on the RET arm, so every RET landed in column 1.
         * CALL is easy when it is the direct/imm form (an indirect call needs a
         * computed target). RET is easy when it is the plain near form, i.e.
         * nops == 0; `ret imm16` (nops == 1) also adjusts RSP and is the
         * awkward remainder. Note RET has NO operands in the easy form, so the
         * old code also read ops[0] on a zero-operand insn. */
        if (o == OCERZ_OP_CALL)
            ps_shape[4][insn->ops[0].kind == OCERZ_OPK_IMM ? 0 : 1]++;
        else
            ps_shape[5][insn->nops == 0 ? 0 : 1]++;
    }
}

/* THE hot function: entered once per slow-path guest instruction. Everything
 * here that is not on the always-executed path is outlined above, so this
 * compiles to a handful of instructions ending in a tail call. The observable
 * order is unchanged and deliberate: insn_count++, then rip = insn->rip+len
 * (which is what makes the fall-through/return address correct for CALL and for
 * a fault taken inside the handler), then the trace hook, then dispatch. */
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

static void emit_slowcall(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits);

#define RF_OFF ((uint32_t)offsetof(OcerzCPU, rflags))
#define RIP_OFF ((uint32_t)offsetof(OcerzCPU, rip))
/* Cross-thread cooperative-stop poll word (see OcerzCPU::interrupt). Read by the
 * chained self-loop back-edge; a set value routes the loop to the dispatcher. */
#define INT_OFF ((uint32_t)offsetof(OcerzCPU, interrupt))
#define GPR_OFF(r) ((uint32_t)((unsigned)(r) * 8))
/* Deferred-flag record (see include/ocerz/flags.h, struct OcerzCPU). */
#define CC_SRC_OFF ((uint32_t)offsetof(OcerzCPU, cc_src))
#define CC_DST_OFF ((uint32_t)offsetof(OcerzCPU, cc_dst))
#define CC_OP_OFF ((uint32_t)offsetof(OcerzCPU, cc_op))
/* RAS (STEP 1 return predictor). RAS_TOP_OFF is the u32 depth; RAS_OFF is the
 * base of the ras[] array. Element i lives at RAS_OFF + i*16, with .guest_rip at
 * +0 and .host_entry at +8 (see struct OcerzCPU). */
#define RAS_TOP_OFF ((uint32_t)offsetof(OcerzCPU, ras_top))
#define RAS_OFF ((uint32_t)offsetof(OcerzCPU, ras))

/* Scratch registers, all caller-saved and dead between inlined instructions
 * (any following slow call reloads its operands from the register file): t0
 * holds the masked destination/operand A, t1 holds operand B, t2 the result,
 * tF the flag accumulator, tT/tU general temporaries. x19=vm, x20=cpu and the
 * saved frame/link must never be touched. */
enum { JT0 = 9, JT1 = 10, JT2 = 11, JTF = 12, JTT = 13, JTU = 14, JTA = 15 };

/* PF: parity of the low byte of the result reg, set on EVEN parity. XOR-fold
 * the byte down to one bit, then OR (NOT that bit) into the flag accumulator
 * at OCERZ_PF's position. Uses tT/tU as scratch and leaves the byte in tT. */
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

/* ZF/SF: read from the arm64 NZCV the sized op just produced. ZF<-Z (EQ),
 * SF<-N (MI), placed at their x86 bit positions, OR'd into the accumulator.
 * Each half is emitted only when `m` asks for it (see the liveness pass in
 * translate); m==0 emits nothing and leaves NZCV untouched. */
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

/* Fold the accumulated arithmetic flags (in tF) into cpu->rflags, preserving
 * every non-arithmetic bit. clear_mask names the bits this op rewrites: it is
 * OCERZ_ARITH_FLAGS for add/sub/logic and the same minus CF for inc/dec. */
static void emit_commit_flags(A64Buf *b, uint64_t clear_mask)
{
    a64_ldr(b, 8, JTT, 20, RF_OFF);
    a64_mov_imm64(b, JTU, ~clear_mask);
    a64_and_reg(b, 1, JTT, JTT, JTU, 0);
    a64_orr_reg(b, 1, JTT, JTT, JTF, 0);
    a64_str(b, 8, JTT, 20, RF_OFF);
}

#define JIT_ARITH_FLAGS (OCERZ_CF | OCERZ_PF | OCERZ_AF | OCERZ_ZF | OCERZ_SF | OCERZ_OF)

/* ---- DEFERRED FLAGS ----------------------------------------------------
 * A flag-producing op stores its reconstruction operands into cpu->cc_src/
 * cc_dst and a packed op tag into cpu->cc_op INSTEAD of computing the six
 * arithmetic flags into cpu->rflags on the hot path. rflags is reconstructed
 * lazily by ocerz_flags_materialize() at each consumer. This subsumes the old
 * dead-flag elimination for free (a later producer just overwrites the record)
 * and is what phase 2's register pinning builds on. cc_op is a u32 (store
 * size 4); the two operand slots are full 64-bit stores. JTT is a dead scratch
 * at every call site here (reloaded by whatever runs next). */
static void emit_defer_flags(A64Buf *b, uint32_t ccop, int src_reg, int dst_reg)
{
    a64_str(b, 8, src_reg, 20, CC_SRC_OFF);
    a64_str(b, 8, dst_reg, 20, CC_DST_OFF);
    a64_mov_imm64(b, JTT, ccop);
    a64_str(b, 4, JTT, 20, CC_OP_OFF);
}

/* Materialize any deferred flags into cpu->rflags before a consumer reads them.
 * Runtime-gated on cc_op != OCERZ_CC_NONE so the common (nothing-deferred) case
 * and the divergent commpage-slow-path case (where the interpreter already made
 * rflags live and cleared cc_op) both cost only a load+cbz. On the taken arm it
 * calls ocerz_flags_materialize(cpu), which clobbers the caller-saved registers
 * (x0-x18, i.e. all JT* temps) but not x19=vm/x20=cpu; every call site below is
 * placed before it sets up its own operands or return value, so that is safe. */
static void emit_materialize(A64Buf *b)
{
    /* With the pair disabled the emitters never defer, so cc_op is always
     * OCERZ_CC_NONE and this would only ever emit a dead load+cbz. Emit nothing,
     * so OCERZ_NO_REGFLAGS=1 reproduces HEAD's codegen exactly. */
    if (!g_defer)
        return;
    a64_ldr(b, 4, JT0, 20, CC_OP_OFF);
    uint32_t *skip = a64_label(b);
    a64_cbz(b, 0, JT0, 0);
    a64_mov_reg(b, 1, 0, 20);       /* x0 = cpu */
    a64_mov_imm64(b, 16, (uint64_t)(uintptr_t)&ocerz_flags_materialize);
    a64_blr(b, 16);
    a64_patch_cbz(skip, a64_label(b));
}

/* ---- REGISTER PINNING operand helpers ----------------------------------
 * Every guest-GPR read/write in the inlined fast paths goes through these two
 * helpers instead of a raw a64_ldr/a64_str against [x20 + GPR_OFF]. When the
 * guest reg is pinned (g_pin[g] >= 0) the access is a register move to/from
 * x21+slot; otherwise it is the identical memory access the JIT always used.
 * Because g_pin is NULL (and every slot -1) unless a block actually pins a
 * register, routing a site through these helpers is a no-op wherever nothing is
 * pinned -- including the whole OCERZ_NO_REGFLAGS=1 baseline. */
static inline int pin_slot(unsigned greg)
{
    return (g_pin && greg < 16) ? g_pin[greg] : -1;
}

/* Read guest GPR `greg` into host `dst`. sf==1 reads the full 64-bit value;
 * sf==0 reads the low 32 zero-extended (matching a size-4 x86 sub-register read
 * and ocerz_read_gpr's truncation). A pinned reg lives as the full 64-bit guest
 * value in x21+slot, so a 32-bit mov (uxtw) reproduces the size-4 zero-extend. */
static void emit_gpr_rd(A64Buf *b, int sf, int dst, unsigned greg)
{
    int s = pin_slot(greg);
    if (s >= 0)
        a64_mov_reg(b, sf, dst, 21 + s);
    else
        a64_ldr(b, sf ? 8 : 4, dst, 20, GPR_OFF(greg));
}

/* Read guest GPR `greg` SIGN-extended from 32 to 64 into `dst` (IMUL size-4
 * source: ocerz_sext(read_gpr(...,4),4)). Pinned: sxtw of the host reg's low 32;
 * memory: ldrsw. Size-8 IMUL sources use emit_gpr_rd(sf=1) instead. */
static void emit_gpr_rd_sw(A64Buf *b, int dst, unsigned greg)
{
    int s = pin_slot(greg);
    if (s >= 0)
        a64_sxtw(b, dst, 21 + s);
    else
        a64_ldrsw(b, dst, 20, GPR_OFF(greg));
}

/* Write the full 64-bit `src` into guest GPR `greg`. All inlined GPR writes
 * store the full 8-byte slot (size-4 x86 writes arrive already zero-extended in
 * src), so the pinned form is a plain 64-bit mov into x21+slot. */
static void emit_gpr_wr(A64Buf *b, int src, unsigned greg)
{
    int s = pin_slot(greg);
    if (s >= 0)
        a64_mov_reg(b, 1, 21 + s, src);
    else
        a64_str(b, 8, src, 20, GPR_OFF(greg));
}

/* Spill every pinned guest reg back to cpu->gpr[] so the interpreter (slow
 * call) and any out-of-arena fault see current values. Uses no scratch. */
static void emit_spill_pinned(A64Buf *b)
{
    for (int i = 0; i < g_n_pinned; i++)
        a64_str(b, 8, 21 + i, 20, GPR_OFF(g_pin_hold[i]));
}

/* Reload every pinned guest reg from cpu->gpr[] after a slow call (the
 * interpreter may have written them). */
static void emit_fill_pinned(A64Buf *b)
{
    for (int i = 0; i < g_n_pinned; i++)
        a64_ldr(b, 8, 21 + i, 20, GPR_OFF(g_pin_hold[i]));
}

/* Block prologue: save the callee-saved host regs x21.. this block pins (in
 * stp pairs, over-saving one when n_pinned is odd -- harmless), then load the
 * pinned guest values from cpu->gpr[]. Emitted after the frame save, so x20=cpu
 * is already established. */
static void emit_pin_prologue(A64Buf *b)
{
    for (int i = 0; i < g_n_pinned; i += 2)
        a64_stp_pre(b, 21 + i, 21 + i + 1, 31, -16);
    for (int i = 0; i < g_n_pinned; i++)
        a64_ldr(b, 8, 21 + i, 20, GPR_OFF(g_pin_hold[i]));
}

/* Restore the callee-saved host regs saved by emit_pin_prologue, popping the
 * stp pairs in reverse order. The caller must have already spilled the pinned
 * guest values to cpu->gpr[] (they are about to be overwritten with the C
 * caller's values). */
static void emit_pin_epilogue_restore(A64Buf *b)
{
    int last = (g_n_pinned & 1) ? g_n_pinned - 1 : g_n_pinned - 2;
    for (int i = last; i >= 0; i -= 2)
        a64_ldp_post(b, 21 + i, 21 + i + 1, 31, 16);
}

/* Load an ALU operand (reg or imm) into dst. Returns 0 if the operand kind is
 * not inlineable at this size. For a REG it must be a plain (no-high8) GPR of
 * the same width; size-4 loads zero-extend through a w-register, matching the
 * interpreter's ocerz_read_gpr truncation. */
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

/* EAGER fallback (OCERZ_NO_REGFLAGS=1): byte-identical to the pre-pair (HEAD)
 * codegen -- computes the six x86 flags into cpu->rflags inline from arm64 NZCV
 * plus the x86 fixups. Reached only when the pair is disabled, so nothing is
 * ever pinned here and the raw memory operand path is correct. */
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

/* ADD/SUB/CMP/AND/OR/XOR/TEST reg<-reg|imm at size 4/8. The arm64 op runs in
 * w-regs for size 4 (zero-extending the upper half exactly as a 32-bit x86
 * write does) and the full 8-byte slot is stored for the writing forms. Flags
 * are DEFERRED into cpu->cc_op/cc_src/cc_dst (materialized at each consumer).
 * Operands are read/written through the pin map. */
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

    emit_gpr_rd(b, sf, JT0, d->reg);
    if (!emit_load_operand(b, s, sf, JT1))
        return 0;

    unsigned op = insn->op;
    int is_sub = (op == OCERZ_OP_SUB || op == OCERZ_OP_CMP);
    int is_add = (op == OCERZ_OP_ADD);
    int is_logic = (op == OCERZ_OP_AND || op == OCERZ_OP_OR ||
                    op == OCERZ_OP_XOR || op == OCERZ_OP_TEST);
    int writes = (op == OCERZ_OP_ADD || op == OCERZ_OP_SUB ||
                  op == OCERZ_OP_AND || op == OCERZ_OP_OR || op == OCERZ_OP_XOR);
    (void)is_logic;

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
        return 1;   /* every flag this op writes is dead: defer nothing */

    /* DEFER: store the reconstruction record instead of computing flags. add/sub
     * carry the two operands (JT0=a, JT1=b) so materialize re-derives res; logic
     * carries only the result (JT2). CF/OF/AF fall out of flags.c bit-exactly at
     * materialize time. */
    if (is_add)
        emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_ADD, d->size, 0), JT0, JT1);
    else if (is_sub)
        emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_SUB, d->size, 0), JT0, JT1);
    else /* is_logic */
        emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_LOGIC, d->size, 0), JT2, JT2);
    return 1;
}

/* INC/DEC reg at size 4/8. CF is preserved (the clear-mask excludes it). OF is
 * positional (INC overflows into the signed minimum, DEC out of it leaving the
 * maximum); AF is the bit-3 half-carry, here (res&0xf)==0 for INC and ==0xf for
 * DEC. ZF/SF/PF follow the result. */
/* EAGER fallback (OCERZ_NO_REGFLAGS=1): verbatim HEAD INC/DEC flag emission. */
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

static int emit_incdec(A64Buf *b, const X86Insn *insn, uint64_t need)
{
    if (!g_defer)
        return emit_incdec_eager(b, insn, need);
    const X86Operand *d = &insn->ops[0];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    int sf = d->size == 8;
    int is_inc = insn->op == OCERZ_OP_INC;

    /* CF is PRESERVED by INC/DEC, so it is never in this op's def set and the
     * liveness pass never asks for it here. Assert the invariant rather than
     * silently emitting a CF this op has no business writing. */
    need &= JIT_ARITH_FLAGS & ~(uint64_t)OCERZ_CF;

    emit_gpr_rd(b, sf, JT0, d->reg);
    if (is_inc)
        a64_add_imm(b, sf, JT2, JT0, 1);
    else
        a64_sub_imm(b, sf, JT2, JT0, 1);
    emit_gpr_wr(b, JT2, d->reg);

    if (need == 0)
        return 1;

    /* INC/DEC PRESERVE CF, so materialize must reconstruct it from the CF that
     * was live going in. That CF may itself be deferred by a predecessor, so
     * resolve any pending record first (runtime-gated), then snapshot CF out of
     * the now-live rflags into cc_src. materialize (a C call) clobbers JT2, so
     * the result is reloaded from the register file it was just written to. */
    emit_materialize(b);
    a64_ldr(b, 8, JTT, 20, RF_OFF);
    a64_ubfx(b, 1, JT0, JTT, 0, 1);              /* JT0 = incoming CF (bit 0) */
    emit_gpr_rd(b, 1, JT1, d->reg);              /* JT1 = res (reload) */
    emit_defer_flags(b, ocerz_cc_pack(is_inc ? OCERZ_CC_INC : OCERZ_CC_DEC,
                                      d->size, 0), JT0, JT1);
    return 1;
}

/* SHL/SHR/SAR reg, imm at size 4/8 with a known nonzero count. A zero count
 * leaves the destination and all flags untouched (the interpreter early-out),
 * so it emits nothing. CF is the last bit shifted out, read by a fixed-index
 * bitfield extract on the ORIGINAL value (sign-extended for SAR); OF mirrors
 * flags.c (SHL: CF^SF, SHR: original MSB, SAR: 0). ZF/SF/PF follow the
 * result. CL-count and shift forms with memory or 8/16-bit operands fall to
 * the slow path. */
/* EAGER fallback (OCERZ_NO_REGFLAGS=1): verbatim HEAD shift flag emission. */
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

    /* DEFER: carry the ORIGINAL value (JT0) and the count. materialize re-derives
     * the shifted result and every flag (CF = last bit out, OF per flags.c,
     * ZF/SF/PF from the result) bit-exactly. The count is a compile-time
     * constant, materialized into a register for the cc_dst store. */
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

/* Sign-extend an inlineable IMUL source into `dst`, exactly as the interpreter
 * does with ocerz_sext(ocerz_read_op(...), op->size). Returns 0 for an operand
 * shape this emitter does not handle. */
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

/* Two- and three-operand IMUL reg <- reg|imm at size 4/8 (the one-operand
 * rdx:rax form and every memory form stay on the slow path).
 *
 * Both sources are SIGN-EXTENDED first, mirroring op_mul in src/interp.c. At
 * size 4 the two sext'd int32s multiply exactly into one 64-bit register, so a
 * single x-form MUL is the whole product; at size 8 the wide product needs
 * MUL + SMULH.
 *
 * flags.c:ocerz_flags_imul sets CF=OF iff the high half is not the sign
 * extension of the low half, and put_arith rewrites all six arithmetic bits —
 * so AF is architecturally 0 here (ocerz pins the undefined bit), and it gets
 * that for free: nothing is ever OR'd into the zeroed accumulator and the
 * commit clears exactly the bits in `need`. ZF/SF/PF follow the low half.
 *
 * The CF/OF test collapses to one compare per size:
 *   size 8: hi != (lo asr 63)
 *   size 4: p  != sxtw(p)      (identical predicate, since p IS the wide half)
 */
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

    /* The sources must be exactly the destination's width. A narrower REG
     * source would need a different sext width than emit_imul_src derives from
     * op->size; an IMM is already carried sign-extended by the decoder, so it
     * is accepted at any size. */
    if (s1->kind != OCERZ_OPK_IMM && s1->size != d->size)
        return 0;
    if (s2->kind != OCERZ_OPK_IMM && s2->size != d->size)
        return 0;

    /* IMUL is the one inlined producer still computed EAGERLY (its wide-product
     * CF/OF test does not fit the two-slot deferred record cheaply). It writes
     * cpu->rflags directly, so any deferred predecessor kept live across an
     * intervening fault-barrier memory op (use=ALL) must be resolved FIRST, or a
     * later consumer would materialize that stale record over IMUL's flags.
     * Runtime-gated; must precede emit_imul_src, which loads JT0/JT1 that the
     * materialize call would clobber. */
    emit_materialize(b);
    if (!emit_imul_src(b, s1, JT0))
        return 0;
    if (!emit_imul_src(b, s2, JT1))
        return 0;

    a64_mul(b, 1, JT2, JT0, JT1);
    if (sf && (need & (OCERZ_CF | OCERZ_OF)))
        a64_smulh(b, JTA, JT0, JT1);

    /* Commit the destination before touching JTT/JTU: emit_pf and emit_zf_sf
     * both clobber them. A size-4 write zero-extends, matching ocerz_write_op. */
    if (sf) {
        emit_gpr_wr(b, JT2, d->reg);
    } else {
        a64_mov_reg(b, 0, JTT, JT2);        /* uxtw: keep the low 32 bits */
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

/* Native guest memory operands. Guest memory is the flat QEMU-style window
 * host = gaddr + ocerz_guest_base (a constant fixed at startup), so a guest
 * load/store is a host ldr/str at [gaddr + base]. Two hazards force a guard:
 *   - the commpage window [OCERZ_COMMPAGE_LO, OCERZ_COMMPAGE_HI) is backed by a
 *     SEPARATE host buffer, not the flat window, so any access whose address
 *     lands there must take the interpreter path (ocerz_g2h special-cases it);
 *   - a store must honor the software watchpoint ocerz_watch_addr.
 * The watchpoint is set once from OCERZ_WATCH before any block is translated,
 * so native stores are simply disabled for the whole run when it is armed
 * (mem_native_store_ok, checked at translate time). The commpage is guarded at
 * RUNTIME: the inlined access computes the address, branches to a per-
 * instruction slow call when it falls in the commpage range, and otherwise
 * performs the flat host access. Segment-override (FS/GS) memory and address-
 * size-32 truncation paths stay on the slow path. */
/* ocerz_st honors TWO store watchpoints -- ocerz_watch_addr (OCERZ_WATCH, by
 * address) and ocerz_watch_val (OCERZ_STVAL, by stored VALUE) -- and a native
 * inlined store goes through neither. Both must therefore disable native stores,
 * or the diagnostic silently misses exactly the stores the JIT inlined, which is
 * worst precisely when it is needed: the value-watch exists to hunt down who
 * writes a poison value, and "it was an inlined mov/push" is the answer it would
 * never give. Checked at translate time; both are set from the environment
 * before any block is translated, so this is a whole-run decision. */
static int mem_native_store_ok(void)
{
    return ocerz_watch_addr == 0 && ocerz_watch_val == 0;
}

/* STEP 1 (guest_base fold): the amount of the host displacement folded into
 * memory-address computation at emit time. host = gaddr + ocerz_guest_base for
 * the plain affine map, so materializing (disp + fold) instead of disp and
 * shifting the commpage guard's compare constants by the same fold lets every
 * inlined access skip its separate `mov JTU,gbase; add` -- 2 words and a
 * dependent add off the ordered-access chain.
 *
 * CRITICAL: this is done ONLY when the low-shadow window is inactive
 * (ocerz_low_base == 0, i.e. every non-Wine guest). When ocerz_low_base != 0,
 * addresses below OCERZ_LOW_LIMIT map to gaddr+low_base (NOT gaddr+guest_base),
 * so folding guest_base into the affine address would be wrong for that arm;
 * we return 0 here and the guard/callers keep the byte-for-byte pre-existing
 * gaddr-convention emission. Wine is therefore provably unchanged. */
static inline uint64_t ea_fold(void)
{
    return ocerz_low_base ? 0 : ocerz_guest_base;
}

/* Add a 64-bit constant to reg via JTU (a dead scratch at every call site here,
 * re-initialized by the ordered-access helper that follows). Emits nothing when
 * c == 0, which is how the fold collapses the post-guard +gbase add away. */
static void emit_add_const(A64Buf *b, int reg, uint64_t c)
{
    if (c) {
        a64_mov_imm64(b, JTU, c);
        a64_add_reg(b, 1, reg, reg, JTU, 0);
    }
}

/* Compute the guest effective address of a base+index*scale+disp memory
 * operand into addr_reg. Returns 0 (not inlineable) for segment overrides,
 * 32-bit address size, and RIP-relative forms with no register inputs are
 * accepted (the absolute disp is materialized directly). When the fold is
 * active (ea_fold() != 0) addr_reg ends holding gaddr + guest_base (a host
 * address); otherwise it holds gaddr exactly as before. */
static int emit_mem_ea(A64Buf *b, const X86Insn *insn, const X86Operand *op, int addr_reg)
{
    uint64_t fold = ea_fold();
    if (insn->seg != OCERZ_SEG_NONE)
        return 0;
    if (op->riprel) {
        a64_mov_imm64(b, addr_reg, (uint64_t)op->disp + fold);
        return 1;
    }
    if (insn->addrsize == 4)
        return 0;
    a64_mov_imm64(b, addr_reg, (uint64_t)op->disp + fold);
    if (op->base != OCERZ_REG_NONE) {
        emit_gpr_rd(b, 1, JT0, op->base);
        a64_add_reg(b, 1, addr_reg, addr_reg, JT0, 0);
    }
    if (op->index != OCERZ_REG_NONE) {
        emit_gpr_rd(b, 1, JT0, op->index);
        a64_add_reg(b, 1, addr_reg, addr_reg, JT0, op->scale & 3);
    }
    return 1;
}

/* Emit the commpage range guard: if (gaddr - OCERZ_COMMPAGE_LO) < window, jump
 * to a per-instruction slow call (recorded so it is patched to the block exit
 * like any other) and skip the native body via the returned skip label, which
 * the caller patches to just past its native access. addr_reg holds gaddr and
 * must survive; the guard uses JTT/JTU as scratch. Returns the cursor of the
 * forward branch that the caller patches to the post-access label. When the
 * low shadow window is active (ocerz_low_base != 0; only fixed-address guests
 * like the Wine loader enable it), addresses below OCERZ_LOW_LIMIT also take
 * the slow call: their host backing is not at gaddr + guest_base, and
 * ocerz_g2h on the slow path applies the shadow offset. Blocks translated
 * without the window keep the exact pre-existing emission. */
static uint32_t *emit_commpage_guard(A64Buf *b, const X86Insn *insn,
                                     int addr_reg, uint32_t **exit_sites, int *n_exits)
{
    /* FAST PATH (only with the low-shadow window active): a guest address in the plain main arena
     * [LOW_LIMIT, TOP_LO) is neither commpage, low-shadow, nor top -- it just needs +guest_base. The
     * common case for 64-bit code (winemac.so + AppKit, where run_cocoa_app's cold-start lives) hits
     * this. Skip the whole commpage/top/low guard with ONE unsigned range check instead of ~10
     * instructions of not-taken compares per access -- a large speedup for the 64-bit AppKit cold
     * start that gates macdrv. Patched to the native body at the end. */
    /* STEP 1: when the fold is active, addr_reg holds gaddr + guest_base, so the
     * range constants shift by the same fold (window SIZES are unchanged). When
     * inactive (fold == 0, ocerz_low_base != 0) every constant is identical to
     * before, so the whole ocerz_low_base branch below is byte-for-byte as it
     * was. The two are never active together: fold != 0 iff ocerz_low_base == 0. */
    uint64_t fold = ea_fold();
    uint32_t *to_native = NULL;
    if (ocerz_low_base) {
        a64_mov_imm64(b, JTU, OCERZ_LOW_LIMIT);
        a64_sub_reg(b, 1, JTT, addr_reg, JTU, 0);          /* JTT = gaddr - LOW_LIMIT */
        a64_mov_imm64(b, JTU, OCERZ_TOP_LO - OCERZ_LOW_LIMIT);
        a64_subs_reg(b, 1, A64_ZR, JTT, JTU, 0);
        to_native = a64_label(b);
        a64_bcond(b, A64_CC, 0);   /* (gaddr-LOW_LIMIT) < (TOP_LO-LOW_LIMIT) -> main arena -> native */
    }
    a64_mov_imm64(b, JTU, OCERZ_COMMPAGE_LO + fold);
    a64_sub_reg(b, 1, JTT, addr_reg, JTU, 0);
    a64_mov_imm64(b, JTU, OCERZ_COMMPAGE_HI - OCERZ_COMMPAGE_LO);
    a64_subs_reg(b, 1, A64_ZR, JTT, JTU, 0);
    uint32_t *over = a64_label(b);
    a64_bcond(b, A64_CS, 0);              /* gaddr-LO >= window -> skip slowcall */
    uint32_t *slow = a64_label(b);
    emit_slowcall(b, insn, exit_sites, n_exits);
    uint32_t *skip = a64_label(b);
    a64_b(b, 0);                          /* past the native body */
    a64_patch_bcond(over, a64_label(b));  /* CS target = low check / native body */
    if (ocerz_low_base) {
        /* TOP region [TOP_LO, ...) keeps the slow call (rare, near the top of userspace). */
        a64_mov_imm64(b, JTU, OCERZ_TOP_LO);
        a64_subs_reg(b, 1, A64_ZR, addr_reg, JTU, 0);
        uint32_t *cur = a64_label(b);
        a64_bcond(b, A64_CS, (int32_t)(slow - cur));   /* gaddr >= TOP_LO -> slow */
        /* LOW-SHADOW [0, LOW_LIMIT) is now INLINED instead of a per-access slow call: shift
         * addr_reg by the constant (low_base - guest_base) so the caller's native body (+guest_base)
         * lands at gaddr + low_base = ocerz_g2h(gaddr). This removes the function call from EVERY
         * 32-bit WoW64 memory access (all guest data lives below LOW_LIMIT) -- the dominant JIT
         * memory cost. Main arena (LOW_LIMIT <= gaddr < TOP_LO) falls through unadjusted. */
        a64_mov_imm64(b, JTU, OCERZ_LOW_LIMIT);
        a64_subs_reg(b, 1, A64_ZR, addr_reg, JTU, 0);
        uint32_t *ge_low = a64_label(b);
        a64_bcond(b, A64_CS, 0);                       /* gaddr >= LOW_LIMIT (main) -> skip adjust */
        a64_mov_imm64(b, JTU, ocerz_low_base - ocerz_guest_base);
        a64_add_reg(b, 1, addr_reg, addr_reg, JTU, 0); /* low-shadow: addr += (low_base-guest_base) */
        a64_patch_bcond(ge_low, a64_label(b));         /* main + low-shadow converge -> native body */
    }
    if (to_native)
        a64_patch_bcond(to_native, a64_label(b));      /* main-arena fast path -> native body */
    return skip;
}

/* Ordered GUEST memory access (x86-64 Total Store Order for plain loads/stores).
 * ra holds the FULL host address with no displacement; rv is the value register
 * (source for a store, destination for a load); scratch is a dead temp.
 *
 * LDAR/STLR give the acquire/release ordering x86 requires, but unlike LDR/STR
 * (and unlike x86) they ALIGNMENT-FAULT on an unaligned address. Guest code may
 * legally do a misaligned access, so these emit a runtime split: naturally
 * aligned (the common case) takes the cheap LDAR/STLR; misaligned falls back to
 * a plain LDR/STR fenced with DMB ISH, which tolerates the misalignment while
 * still ordering the access. size is always 4 or 8 at these call sites, so the
 * alignment mask (size-1) is 3 or 7. */
static void emit_guest_store_ordered(A64Buf *b, int size, int rv, int ra, int scratch)
{
    a64_mov_imm64(b, scratch, (uint64_t)(size - 1));
    a64_ands_reg(b, 1, A64_ZR, ra, scratch, 0);     /* Z=1 iff (ra & (size-1))==0 */
    uint32_t *to_aligned = a64_label(b);
    a64_bcond(b, A64_EQ, 0);                          /* aligned -> STLR */
    a64_dmb_ish(b);                                   /* release order before the store */
    a64_str(b, size, rv, ra, 0);
    uint32_t *to_done = a64_label(b);
    a64_b(b, 0);
    a64_patch_bcond(to_aligned, a64_label(b));
    a64_stlr(b, size, rv, ra);
    a64_patch_b(to_done, a64_label(b));
}

static void emit_guest_load_ordered(A64Buf *b, int size, int rd, int ra, int scratch)
{
    a64_mov_imm64(b, scratch, (uint64_t)(size - 1));
    a64_ands_reg(b, 1, A64_ZR, ra, scratch, 0);
    uint32_t *to_aligned = a64_label(b);
    a64_bcond(b, A64_EQ, 0);                          /* aligned -> LDAR */
    a64_ldr(b, size, rd, ra, 0);
    a64_dmb_ish(b);                                   /* acquire order after the load */
    uint32_t *to_done = a64_label(b);
    a64_b(b, 0);
    a64_patch_bcond(to_aligned, a64_label(b));
    a64_ldar(b, size, rd, ra);
    a64_patch_b(to_done, a64_label(b));
}

/* MOV with one memory operand at size 4/8 (the other a plain GPR), no high8,
 * no segment. mem<-reg stores the source slot through the flat window;
 * reg<-mem loads and zero-extends (size 4 through a w-register so the upper
 * half clears, matching ocerz_write_gpr). Sets no flags. */
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
        if (!emit_mem_ea(b, insn, d, JTA))
            return 0;
        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_gpr_rd(b, s->size == 8 ? 1 : 0, JT1, s->reg);
        emit_add_const(b, JTA, gbase - ea_fold());   /* 0 when folded into the EA */
        /* GUEST store (JTA = full host address, no displacement), store-release
         * ordered so plain x86 stores keep Total Store Order. JTU is dead here
         * (held gbase) and serves as the helper's alignment-check scratch. */
        emit_guest_store_ordered(b, s->size, JT1, JTA, JTU);
        a64_patch_b(skip, a64_label(b));
        return 1;
    }
    if (d->kind == OCERZ_OPK_REG && s->kind == OCERZ_OPK_MEM) {
        if (d->high8 || (d->size != 4 && d->size != 8))
            return 0;
        if (!emit_mem_ea(b, insn, s, JTA))
            return 0;
        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, gbase - ea_fold());   /* 0 when folded into the EA */
        /* GUEST load (JTA = full host address, no displacement), load-acquire
         * ordered so plain x86 loads keep Total Store Order. JTU is dead here
         * and serves as the helper's alignment-check scratch. */
        emit_guest_load_ordered(b, d->size, JT1, JTA, JTU);
        emit_gpr_wr(b, JT1, d->reg);
        a64_patch_b(skip, a64_label(b));
        return 1;
    }
    return 0;
}

/* PUSH/POP at operand size 8 with a plain GPR or immediate operand and no
 * segment override. MEASURED as the single biggest slow-path cost in a real
 * Cocoa app: push+pop are 60.5% of all interpreted instructions and 26.9% of
 * ALL executed guest instructions (function prologue/epilogue traffic), and
 * 100.0% of them are this shape (493,513,020 easy vs 24,924 other).
 *
 * The reason this is the LOW-RISK big win: PUSH AND POP SET NO FLAGS. None of
 * emit_pf / emit_zf_sf / emit_commit_flags is reachable, so the whole flags.c
 * bit-exactness surface -- the thing that makes every other op painful to inline
 * -- simply does not exist here. The semantics are two lines of interp_common.h.
 *
 * Everything below is the proven machinery emit_mov_mem already uses: the same
 * emit_commpage_guard (which reproduces all four ocerz_g2h arms), the same
 * +guest_base, and the same emit_guest_{load,store}_ordered including its
 * aligned-STLR / misaligned-DMB+STR split -- the x86 stack is NOT architecturally
 * 8-byte aligned, so that split is load-bearing, not a pessimization to remove.
 *
 * THREE ORDERING TRAPS, each one transposed line from silent corruption, and
 * each traced to the interpreter reference rather than assumed:
 *
 *  1. PUSH reads its operand BEFORE rsp moves (ocerz_read_op runs before
 *     ocerz_push in interp.c), so `push rsp` pushes the OLD rsp. The value is
 *     loaded first here for exactly that reason.
 *
 *  2. The guard's slow call RE-EXECUTES THE WHOLE INSTRUCTION through the
 *     interpreter. So nothing architectural may be mutated before the guard:
 *     this computes the address into JTA and touches no state, or a commpage-
 *     ranged push would decrement rsp here AND again in the interpreter.
 *
 *  3. The memory access must land BEFORE rsp is committed (push) and the
 *     destination write must land LAST (pop). A faulting access aborts the
 *     instruction, and a delivered fault now rewinds rip to retry it, so a
 *     prematurely committed rsp would be applied twice. For `pop rsp` the
 *     destination store must also win over the rsp increment -- so new_rsp is
 *     stored first and the popped value second, unconditionally, in that order,
 *     which collapses to the correct rsp=v for the pop-rsp case and is a
 *     redundant-but-harmless second store otherwise. This mirrors ocerz_push /
 *     ocerz_pop exactly. */
/* A/B kill switch. Read once (translate() runs under jit_lock, and the value is
 * a whole-run decision), it makes "is a fault caused by inlined push/pop?" a
 * one-run experiment on the SAME binary instead of a rebuild-and-hope. Costs a
 * predictable never-taken branch at translate time only. */
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
        /* Operand size must be 8 so the value is the full 64-bit slot: that is
         * what ocerz_read_op yields for both arms (read_gpr at size 8, and
         * trunc(imm,8) on an immediate the decoder already sign-extended). Any
         * other width falls through to the interpreter rather than guessing. */
        if (o->kind == OCERZ_OPK_REG) {
            if (o->high8 || o->size != 8)
                return 0;
        } else if (o->kind == OCERZ_OPK_IMM) {
            if (o->size != 8)
                return 0;
        } else {
            return 0;
        }

        /* (1) value first -- `push rsp` must push the OLD rsp. RSP itself is
         * never pinned (its stack-op accesses stay in memory), so this reads the
         * current rsp whether the value operand is rsp or a pinned data reg. */
        if (o->kind == OCERZ_OPK_REG)
            emit_gpr_rd(b, 1, JT1, o->reg);
        else
            a64_mov_imm64(b, JT1, o->imm);

        /* (2) address only; no architectural state touched before the guard.
         * JTA is a dead temp, so folding guest_base into it before the guard
         * (host convention) moves no architectural state. */
        a64_ldr(b, 8, JT0, 20, GPR_OFF(OCERZ_RSP));
        a64_sub_imm(b, 1, JTA, JT0, 8);
        emit_add_const(b, JTA, ea_fold());

        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, gbase - ea_fold());
        /* (3) store, THEN commit rsp: a fault here leaves rsp untouched so the
         * retry pushes to the same address. JT0 survives the helper (it uses
         * rv=JT1, ra=JTA, scratch=JTU). */
        emit_guest_store_ordered(b, 8, JT1, JTA, JTU);
        a64_sub_imm(b, 1, JT0, JT0, 8);
        a64_str(b, 8, JT0, 20, GPR_OFF(OCERZ_RSP));
        a64_patch_b(skip, a64_label(b));
        return 1;
    }

    if (insn->op == OCERZ_OP_POP) {
        if (o->kind != OCERZ_OPK_REG || o->high8 || o->size != 8)
            return 0;

        /* (2) address only; the load is what may fault. */
        a64_ldr(b, 8, JT0, 20, GPR_OFF(OCERZ_RSP));
        a64_mov_reg(b, 1, JTA, JT0);
        emit_add_const(b, JTA, ea_fold());

        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, gbase - ea_fold());
        emit_guest_load_ordered(b, 8, JT1, JTA, JTU);
        /* (3) rsp increment first, popped value LAST: `pop rsp` must end with
         * rsp = the popped value, exactly as ocerz_pop + ocerz_write_op do. */
        a64_add_imm(b, 1, JT0, JT0, 8);
        a64_str(b, 8, JT0, 20, GPR_OFF(OCERZ_RSP));
        emit_gpr_wr(b, JT1, o->reg);
        a64_patch_b(skip, a64_label(b));
        return 1;
    }
    return 0;
}

/* MOVSXD reg64 <- reg32/mem32: sign-extend 32 to 64. 8.26% of the slow path in
 * a real app (125,934,088 executions) and the cheapest real win available --
 * the entire semantics are one line of interp.c (sext of the source) and it
 * SETS NO FLAGS, so like push/pop there is no flags.c bit-exactness surface.
 *
 * Both source forms are inlined. The measured "100% easy" shape counter only
 * classified the DESTINATION, so the source can legitimately be memory; the
 * memory arm reuses emit_mem_ea + emit_commpage_guard + emit_guest_load_ordered
 * exactly as emit_mov_mem does, and touches no state before the guard (whose
 * slow call re-executes the whole instruction).
 *
 * MOVSX (the 8/16-bit-source sibling) shares the interpreter case but is a
 * different width problem and stays on the slow path. */
static int emit_movsxd(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];

    /* Destination must be a plain 64-bit GPR: then ocerz_write_op is just
     * gpr[reg] = v, with no sub-register merge to reproduce. */
    if (d->kind != OCERZ_OPK_REG || d->high8 || d->size != 8)
        return 0;
    if (s->size != 4)
        return 0;

    if (s->kind == OCERZ_OPK_REG) {
        if (s->high8)
            return 0;
        /* w-load zero-extends the low 32 (matching ocerz_read_gpr at size 4),
         * then sxtw sign-extends them -- the composition is the interpreter's
         * ocerz_sext(read_op(src, 4), 4). */
        emit_gpr_rd(b, 0, JT0, s->reg);
        a64_sxtw(b, JT0, JT0);
        emit_gpr_wr(b, JT0, d->reg);
        return 1;
    }
    if (s->kind == OCERZ_OPK_MEM) {
        if (!emit_mem_ea(b, insn, s, JTA))
            return 0;
        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, ocerz_guest_base - ea_fold());   /* 0 when folded */
        emit_guest_load_ordered(b, 4, JT1, JTA, JTU);
        a64_sxtw(b, JT1, JT1);
        emit_gpr_wr(b, JT1, d->reg);
        a64_patch_b(skip, a64_label(b));
        return 1;
    }
    return 0;
}

/* ALU with a register destination and a MEMORY source (ADD/SUB/CMP/AND/OR/XOR/
 * TEST reg, mem) at size 4/8. The memory value is loaded natively (commpage-
 * guarded) into the b-operand register, then the identical NZCV+fixup flag
 * computation as the register form runs. Memory-DESTINATION arithmetic (read-
 * modify-write, locked or not) stays on the slow path. */
/* EAGER fallback (OCERZ_NO_REGFLAGS=1): verbatim HEAD reg<-mem ALU emission. */
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
    a64_patch_b(skip, a64_label(b));
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
    emit_add_const(b, JTA, ocerz_guest_base - ea_fold());   /* 0 when folded */
    /* GUEST load (JTA = full host address, no displacement), load-acquire
     * ordered so plain x86 loads keep Total Store Order. JTU is dead here
     * and serves as the helper's alignment-check scratch. The following
     * read pulls the destination GPR from the pin map / register file. */
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

    /* DEFER on the NATIVE path only (identical record to the register form). The
     * commpage slow path took emit_slowcall, which materialized any predecessor
     * and left the interpreter's own flag write live with cc_op==NONE; the two
     * paths converge at `skip` in runtime-divergent flag state, and every
     * consumer's runtime-gated materialize copes with both. */
    if (need != 0) {
        if (is_add)
            emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_ADD, d->size, 0), JT0, JT1);
        else if (is_sub)
            emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_SUB, d->size, 0), JT0, JT1);
        else /* is_logic */
            emit_defer_flags(b, ocerz_cc_pack(OCERZ_CC_LOGIC, d->size, 0), JT2, JT2);
    }
    (void)is_logic;
    /* The commpage-guard skip label must be patched on EVERY path, including
     * need==0: the guard's forward branch targets the instruction after this
     * whole sequence, and skipping the patch would leave it dangling. */
    a64_patch_b(skip, a64_label(b));
    return 1;
}

/* LEA reg<-mem at size 4/8. The effective address is base + (index<<scale) +
 * disp, 32-bit-truncated when the address size is 4; RIP-relative operands
 * arrive with disp pre-resolved to the absolute target. The destination write
 * follows the same 32-bit zero-extension rule as any other reg write. LEA sets
 * no flags. Segment overrides never apply to LEA. */
static int emit_lea(A64Buf *b, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    if (s->kind != OCERZ_OPK_MEM)
        return 0;

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

/* `need` is the set of flags this instruction WRITES that something downstream
 * actually READS — computed by the backward liveness pass in translate() and
 * always a subset of the op's def mask (see include/ocerz/flags_live.h). The
 * emitters below compute exactly those and skip the rest; need==0 means every
 * flag this op would have written is dead and no flag code is emitted at all.
 * Ops that set no flags ignore it. */
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
                emit_gpr_rd(b, sz4 ? 0 : 1, JT0, s->reg);
                emit_gpr_wr(b, JT0, d->reg);
                return 1;
            }
            if (s->kind == OCERZ_OPK_IMM) {
                uint64_t v = s->imm;
                if (d->size == 4)
                    v &= 0xffffffffull;
                a64_mov_imm64(b, JT0, v);
                emit_gpr_wr(b, JT0, d->reg);
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
        if (insn->ops[1].kind == OCERZ_OPK_MEM)
            return emit_arith_mem(b, insn, need, exit_sites, n_exits);
        return emit_arith(b, insn, need);
    case OCERZ_OP_INC:
    case OCERZ_OP_DEC:
        return emit_incdec(b, insn, need);
    case OCERZ_OP_SHL:
    case OCERZ_OP_SHR:
    case OCERZ_OP_SAR:
        return emit_shift(b, insn, need);
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
    case OCERZ_OP_MOVSXD:
        return emit_movsxd(b, insn, exit_sites, n_exits);
    default:
        return 0;
    }
}

static uint32_t *emit_chain_tail(A64Buf *b, int poll);   /* defined below; used by emit_jcc's two-way arms */

/* STEP 2 back-edge classifier: is `rip` already a COMPILED block? A compiled
 * predecessor means an edge to it runs backward in compilation order -- a loop
 * back-edge that must poll cpu->interrupt. Single-writer under jit_lock
 * (translate), so the cache read is race-free. */
static int jcc_target_compiled(uint64_t rip)
{
    if (!g_xlat_jit)
        return 0;
    JitBlock *t = cache_lookup(g_xlat_jit, rip);
    return t && t->code;
}

/* Native Jcc terminator. cpu->rflags is architecturally current for every flag
 * a Jcc can read: the liveness pass forces the block's last instruction to have
 * all six flags live-out, so whichever producer precedes this Jcc materialized
 * them. The condition is evaluated by reproducing ocerz_cc_eval's exact
 * predicate tree against the live flag bits, never against arm64 NZCV (the
 * flag producer may have been a slow call). cpu->rip is then set to one of two
 * compile-time constants — the taken target ops[0].imm or the fallthrough
 * rip+len — and STEP_OK is loaded into x0 before the block routes to its
 * shared epilogue. Returns 1 when emitted. */
static int emit_jcc(A64Buf *b, const X86Insn *insn, uint32_t **epilogue_sites, int *n_epi)
{
    if (insn->op != OCERZ_OP_JCC)
        return 0;
    unsigned cc = insn->cc;
    uint64_t taken = insn->ops[0].imm;
    uint64_t fall = insn->rip + insn->len;

    /* Jcc reads rflags; make any deferred flags live first. This is also a block
     * exit, so it doubles as the boundary materialize. */
    emit_materialize(b);
    a64_ldr(b, 8, JT0, 20, RF_OFF);
    a64_ubfx(b, 1, JT1, JT0, 0, 1);   /* CF */
    a64_ubfx(b, 1, JT2, JT0, 6, 1);   /* ZF */
    a64_ubfx(b, 1, JTT, JT0, 7, 1);   /* SF */
    a64_ubfx(b, 1, JTU, JT0, 11, 1);  /* OF */

    /* Collapse the selected predicate into JTF (the only register that must
     * survive the rip materialization below). The flag-bit holders JT1/JT2/
     * JTT/JTU are then free to reload. */
    switch (cc >> 1) {
    case 0: a64_mov_reg(b, 1, JTF, JTU); break;     /* O  -> OF */
    case 1: a64_mov_reg(b, 1, JTF, JT1); break;     /* B  -> CF */
    case 2: a64_mov_reg(b, 1, JTF, JT2); break;     /* E  -> ZF */
    case 3: a64_orr_reg(b, 1, JTF, JT1, JT2, 0); break;  /* BE -> CF|ZF */
    case 4: a64_mov_reg(b, 1, JTF, JTT); break;     /* S  -> SF */
    case 5: a64_ubfx(b, 1, JTF, JT0, 2, 1); break;  /* P  -> PF */
    case 6: a64_eor_reg(b, 1, JTF, JTT, JTU, 0); break;  /* L  -> SF!=OF */
    default:                                         /* LE -> ZF|(SF!=OF) */
        a64_eor_reg(b, 1, JTF, JTT, JTU, 0);
        a64_orr_reg(b, 1, JTF, JTF, JT2, 0);
        break;
    }

    a64_mov_imm64(b, JT1, fall);
    a64_mov_imm64(b, JT2, taken);
    a64_subs_imm(b, 1, A64_ZR, JTF, 0);
    if (cc & 1)
        a64_csel(b, 1, JT0, JT2, JT1, A64_EQ);   /* inverted: take==0 -> target */
    else
        a64_csel(b, 1, JT0, JT2, JT1, A64_NE);   /* take!=0 -> target */
    a64_str(b, 8, JT0, 20, RIP_OFF);

    /* SELF-LOOP CHAINING (Increment 2). When this Jcc's taken target is the
     * block's own entry rip, the loop back-edge can branch DIRECTLY to
     * body-entry (g_body_entry, the label past the prologue) keeping the pinned
     * guest GPRs resident in x21..x28, instead of returning through the shared
     * epilogue to the dispatcher and reloading them next iteration. JT0 (the
     * resolved rip) is compared to the entry rip: on a match the back-edge is
     * taken, otherwise the loop exits normally. A chained self-loop never returns
     * to ocerz_vm_run_cpu's header, so the back-edge MUST poll cpu->interrupt (the
     * committed cross-thread stop word) and fall through to the dispatcher exit
     * when it is set -- otherwise a process-wide/cross-thread stop could never
     * break the loop (tests/guest/interrupt_test + ras_stress are the gate). The
     * poll is one L1 load + a predicted-not-taken cbnz off the pinned cpu pointer.
     * body-entry is in this same block, always within B range. */
    if (!g_no_chain && g_body_entry && taken == g_self_rip) {
        a64_mov_imm64(b, JT1, g_self_rip);
        a64_subs_reg(b, 1, A64_ZR, JT0, JT1, 0);    /* resolved rip == entry ? */
        uint32_t *not_loop = a64_label(b);
        a64_bcond(b, A64_NE, 0);                     /* not looping back -> exit */
        a64_ldr(b, 4, JT1, 20, INT_OFF);             /* w = cpu->interrupt */
        uint32_t *intr = a64_label(b);
        a64_cbnz(b, 0, JT1, 0);                      /* stop requested -> exit */
        uint32_t *here = a64_label(b);
        a64_b(b, (int32_t)(g_body_entry - here));    /* chain back to body-entry */
        uint32_t *exit_tail = a64_label(b);
        a64_patch_bcond(not_loop, exit_tail);
        a64_patch_cbz(intr, exit_tail);
        a64_mov_imm64(b, 0, OCERZ_STEP_OK);
        epilogue_sites[*n_epi] = a64_label(b);
        a64_b(b, 0);
        (*n_epi)++;
        return 1;
    }

    /* GENERAL TWO-WAY Jcc LINKING (STEP 1 forward + STEP 2 back-edge). The
     * self-loop fast path did not fire. Instead of the single csel + shared
     * dispatcher exit, split into a taken arm and a fall-through arm, each ending
     * in an inline chain tail (the same flush-pins-to-gpr[] / rebuild-frame
     * teardown the CALL edge uses, net frame delta zero so any source/target
     * pin-map pair is correct -- BOTH arms route through the gpr[] seam to the
     * target's HOST ENTRY; the body-entry shortcut is FORBIDDEN for a cross-block
     * edge). Both arms are linked.
     *
     * INTERRUPT POLL classifier (soundness). An arm whose target could close a
     * control cycle must poll cpu->interrupt on its back-edge, or a fully-linked
     * loop can never be broken by a cross-thread/process stop (link_spin is the
     * gate). The sound, CFG-free criterion is "the target is ALREADY a compiled
     * block" (cache_lookup ->code): a loop header is compiled before its back-edge
     * block, so the back-edge arm sees it compiled and polls, and every cycle thus
     * carries >=1 poll (the pure-address heuristic target<=entry is UNSOUND for br,
     * whose back-edge points at a HIGHER addr, so it is used only as an extra
     * over-poll, never as the sole test). Biasing to OVER-poll is safe: a needless
     * poll on a forward re-merge costs one predicted-not-taken cbnz; a MISSED poll
     * is an unbreakable loop. Forward arms (target not yet compiled, above entry)
     * poll=0, preserving the STEP-1 forward-diamond throughput.
     *
     * The subs above (JTF-0) still holds NZCV (csel/str do not touch it), so the
     * arm split reuses it; each arm stores its own compile-time rip constant,
     * overwriting the csel result, so every exit (chained, fallback, or poll)
     * carries the correct rip. */
    if (!g_no_chain && !g_no_jcclink) {
        int poll_fall  = (fall  <= g_self_rip) || jcc_target_compiled(fall);
        int poll_taken = (taken <= g_self_rip) || jcc_target_compiled(taken);
        int taken_eq = (cc & 1);   /* taken when JTF==0 (EQ), else JTF!=0 (NE) */
        uint32_t *to_taken = a64_label(b);
        a64_bcond(b, taken_eq ? A64_EQ : A64_NE, 0);   /* -> taken arm */
        /* FALL arm */
        a64_mov_imm64(b, JT0, fall);
        a64_str(b, 8, JT0, 20, RIP_OFF);
        uint32_t *pb_fall = emit_chain_tail(b, poll_fall);
        /* TAKEN arm */
        uint32_t *ltaken = a64_label(b);
        a64_patch_bcond(to_taken, ltaken);
        a64_mov_imm64(b, JT0, taken);
        a64_str(b, 8, JT0, 20, RIP_OFF);
        uint32_t *pb_taken = emit_chain_tail(b, poll_taken);
        g_jcc_edge[0].target_rip = fall;
        g_jcc_edge[0].patch_b = pb_fall;
        g_jcc_edge[1].target_rip = taken;
        g_jcc_edge[1].patch_b = pb_taken;
        g_n_jcc_edges = 2;
        return 1;
    }

    a64_mov_imm64(b, 0, OCERZ_STEP_OK);
    epilogue_sites[*n_epi] = a64_label(b);
    a64_b(b, 0);
    (*n_epi)++;
    return 1;
}

/* Native CALL(imm)/RET terminators.
 *
 * WHY THESE AND WHY NOW. call is by far the worst workload (a two-point
 * fib(34)/fib(36) regression, which cancels the ~0.024s process-startup term
 * that makes the 0.039s wall clock unusable as a ratio, puts it at ~29.9x
 * against Rosetta -- the naive wall-clock ratio reads 12.4x and is wrong).
 * CALL and RET are 100% of its slow path, split exactly 50/50, and BOTH are in
 * the easy shape 100% of the time. (The shape counter used to report RET as 0%
 * easy; that was a classifier bug -- its condition tested `op == CALL` on the
 * RET arm, so it was false by construction and every RET landed in the awkward
 * column. See jit_perfstat_one.)
 *
 * WHAT MAKES THEM CHEAP TO INLINE. Both reduce to machinery that already
 * exists and is already differentially proven:
 *   CALL imm = push(rip+len) ; rip = imm      -- BOTH are compile-time
 *              constants, so this is emit_push_pop's PUSH-imm arm followed by
 *              emit_jcc's rip/STEP_OK/exit tail.
 *   RET      = rip = pop()                    -- emit_push_pop's POP arm with
 *              cpu->rip as the destination instead of a GPR.
 * The stack access reuses emit_commpage_guard and emit_guest_{load,store}_
 * ordered verbatim, so all three of emit_push_pop's documented ordering traps
 * are handled by construction rather than re-derived here.
 *
 * THE RETURN ADDRESS IS NOT cpu->rip. The interpreter does
 * `ocerz_push(cpu, 8, cpu->rip)`, which reads correctly ONLY because
 * ocerz_jit_exec_one sets `cpu->rip = insn->rip + insn->len` before dispatching.
 * There is no exec_one on this path, so the return address is materialized here
 * as the constant insn->rip + insn->len. Reading RIP_OFF instead would push
 * whatever the previous instruction left there.
 *
 * SIZE IS ALWAYS 8, exactly as the interpreter has it: op_branch calls
 * ocerz_push(cpu, 8, ...) and ocerz_pop(cpu, 8) unconditionally, ignoring
 * insn->opsize. This does the same rather than inventing a width check the
 * reference does not have.
 *
 * THE COMMPAGE `skip` MUST LAND PAST THE RIP STORE. The guard's slow path
 * RE-EXECUTES THE WHOLE INSTRUCTION through the interpreter, which for these
 * two ops includes setting cpu->rip to the target. If skip landed before the
 * native rip store, that store would then overwrite the interpreter's correct
 * target with this block's constant -- right for CALL by luck, catastrophically
 * wrong for RET. Both arms therefore patch skip to a label AFTER the rip store,
 * where the two paths converge on the shared STEP_OK/exit tail.
 *
 * NOT INLINED, deliberately: indirect CALL (the target is computed, and
 * ocerz_cftrap must still see it -- note the interpreter's cftrap hook is on
 * the non-IMM arm only, so inlining the imm form cannot blind it), and
 * `ret imm16` (nops==1), which additionally adjusts rsp.
 *
 * EXPECT THE INLINED FORM TO BE BIGGER than the slow call it replaces (~10
 * words becomes ~37). That is not a regression: call is round-trip-bound, not
 * issue-bound, and the win is deleting the interpreter round trip, not words.
 *
 * Blocks still TERMINATE here -- this is inlining, not chaining. is_terminator
 * is untouched on purpose: decoding past a `call` would read the callee's
 * fall-through bytes, which is the exact class of bug the missing
 * OCERZ_OP_IRET terminator was. */
/* A/B kill switch, mirroring OCERZ_NO_INLINE_STACK. That switch is what made
 * the 19.7 cyc/slow-call figure trustworthy (same binary, no build or machine
 * drift), and its successor has to be measurable the same way. Read once;
 * translate() runs under jit_lock and this is a whole-run decision. */
static int callret_inline_enabled(void)
{
    static int en = -1;
    if (en < 0)
        en = getenv("OCERZ_NO_INLINE_CALLRET") ? 0 : 1;
    return en;
}

/* RAS push (STEP 1), called from the CALL-side inlined code AFTER the guest push
 * has committed. Records the return address and the host entry of the block that
 * begins there so the matching RET can branch straight to it. cache_lookup is
 * the same lockless bucket walk ocerz_jit_step's hit path uses (blocks are never
 * freed and hnext is published before the head swap), so reading it from a
 * running guest thread is safe. A demoted block has code==NULL; we store NULL,
 * and the RET side treats NULL as "no prediction" and falls through to dispatch.
 * On a full stack we simply drop the push (RET then mispredicts and dispatches):
 * a pure-hint structure never needs to be exact. Not inlined into the emitted
 * stream because it is the COLD arm of a round-trip-bound instruction; keeping it
 * a plain C call keeps the emitter simple and the CALL path is not issue-bound. */
void ocerz_ras_push(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t retaddr)
{
    uint32_t t = cpu->ras_top;
    if (t >= OCERZ_RAS_SIZE)
        return;
    JitBlock *blk = cache_lookup(vm->jit, retaddr);
    cpu->ras[t].guest_rip = retaddr;
    cpu->ras[t].host_entry = (blk && blk->code) ? (void *)blk->code : NULL;
    cpu->ras_top = t + 1;
}

static int emit_call_ret(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites,
                         int *n_exits, uint32_t **epi_sites, int *n_epi)
{
    uint64_t gbase = ocerz_guest_base;

    if (!callret_inline_enabled())
        return 0;
    /* A segment override on an implicit-SS stack op is not something the
     * interpreter honors either (ocerz_push/ocerz_pop go straight to
     * ocerz_st/ocerz_ld with no segment base); refuse it rather than rely on
     * that. Mirrors emit_push_pop. */
    if (insn->seg != OCERZ_SEG_NONE)
        return 0;

    if (insn->op == OCERZ_OP_CALL) {
        if (insn->ops[0].kind != OCERZ_OPK_IMM)
            return 0;
        if (!mem_native_store_ok())
            return 0;

        /* Block exit (and a possible RAS tail-call straight into the next block):
         * make deferred flags live so the successor and any out-of-JIT code see
         * an architecturally current rflags. CALL itself reads no flag. */
        emit_materialize(b);

        uint64_t retaddr = insn->rip + insn->len;
        uint64_t target = insn->ops[0].imm;

        /* CHAINING (Increment 1): this direct CALL's successor rip is the
         * compile-time constant `target`. Record it so translate() can chain the
         * terminal STEP_OK exit straight into the callee, skipping the dispatcher
         * round-trip. RAS still handles the matching RET. */
        g_chain_target = target;

        /* (1) value first: the return address, as a constant (see above). */
        a64_mov_imm64(b, JT1, retaddr);
        /* (2) address only -- no architectural state may move before the guard,
         * or a commpage-ranged call would decrement rsp here AND again in the
         * interpreter's re-execution. */
        a64_ldr(b, 8, JT0, 20, GPR_OFF(OCERZ_RSP));
        a64_sub_imm(b, 1, JTA, JT0, 8);
        emit_add_const(b, JTA, ea_fold());

        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, gbase - ea_fold());
        /* (3) store, THEN commit rsp: a fault here must leave rsp untouched so
         * the retry pushes to the same address. */
        emit_guest_store_ordered(b, 8, JT1, JTA, JTU);
        a64_sub_imm(b, 1, JT0, JT0, 8);
        a64_str(b, 8, JT0, 20, GPR_OFF(OCERZ_RSP));
        /* rip = target, only after the push has committed. */
        a64_mov_imm64(b, JT0, target);
        a64_str(b, 8, JT0, 20, RIP_OFF);
        a64_patch_b(skip, a64_label(b));   /* PAST the rip store -- see above */

        /* STEP 1 RAS push. Reached by BOTH the native body and the commpage slow
         * path (which converge here); on either, cpu is fully updated and retaddr
         * is the same compile-time constant, so the record is correct. x19/x20 are
         * callee-saved and survive the C call; the scratch temps and x0 are dead
         * (the shared tail reloads x0 with STEP_OK below). */
        if (!g_no_ras) {
            a64_mov_reg(b, 1, 0, 19);              /* x0 = vm */
            a64_mov_reg(b, 1, 1, 20);              /* x1 = cpu */
            a64_mov_imm64(b, 2, retaddr);          /* x2 = retaddr */
            a64_mov_imm64(b, 16, (uint64_t)(uintptr_t)&ocerz_ras_push);
            a64_blr(b, 16);
        }
    } else if (insn->op == OCERZ_OP_RET) {
        if (insn->nops != 0)               /* `ret imm16` also moves rsp */
            return 0;

        /* Block exit / RAS tail-call: make deferred flags live before leaving.
         * RET reads no flag. */
        emit_materialize(b);

        /* (2) address only; the load is the faulting point. */
        a64_ldr(b, 8, JT0, 20, GPR_OFF(OCERZ_RSP));
        a64_mov_reg(b, 1, JTA, JT0);
        emit_add_const(b, JTA, ea_fold());

        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, gbase - ea_fold());
        emit_guest_load_ordered(b, 8, JT1, JTA, JTU);
        /* rsp += 8, then rip = the popped value: ocerz_pop's increment followed
         * by `cpu->rip = ret`. Distinct locations, and the faulting load is
         * already behind us, so this order is the interpreter's exactly. */
        a64_add_imm(b, 1, JT0, JT0, 8);
        a64_str(b, 8, JT0, 20, GPR_OFF(OCERZ_RSP));
        a64_str(b, 8, JT1, 20, RIP_OFF);
        a64_patch_b(skip, a64_label(b));   /* PAST the rip store -- see above */

        /* STEP 1 RAS fast return. Both the native body and the commpage slow path
         * converge here with cpu->rip already holding the popped return address
         * (the native path stored JT1->rip; the slow path's interpreter set it),
         * so the prediction is checked against cpu->rip, never against JT1 (which
         * is not the popped value on the slow path). On an exact match to the top
         * RAS entry with a non-NULL host entry, tear down THIS block's frame and
         * tail-call the predicted block: it runs as a fresh entry (its prologue
         * saves the just-restored fp/lr) and returns its own STEP straight to
         * ocerz_jit_step's caller, skipping the dispatch. Any miss/empty/NULL
         * branches to `ras_miss` and falls through to the shared STEP_OK tail
         * exactly as before. Pure hint: on a mismatch the dispatcher is correct.
         *
         * This tail-call is a bounded chain, not an unpolled self-loop: every RET
         * pops one RAS entry and RAS is only pushed by CALL, so a run of returns
         * drains RAS and reverts to dispatch (which re-checks the stop flags). It
         * therefore needs no back-edge interrupt poll of its own -- the driving
         * loop's Jcc still returns to the run-loop header each iteration. */
        if (!g_no_ras) {
            uint32_t *ras_miss[3];
            int nrm = 0;
            a64_ldr(b, 4, JT2, 20, RAS_TOP_OFF);        /* w: ras_top */
            ras_miss[nrm] = a64_label(b); a64_cbz(b, 0, JT2, 0); nrm++;   /* empty */
            a64_sub_imm(b, 0, JT2, JT2, 1);             /* w: top-1 (also new top) */
            a64_lsl_imm(b, 1, JTA, JT2, 4);             /* (top-1) * 16 (element stride) */
            a64_add_reg(b, 1, JTA, JTA, 20, 0);         /* JTA = cpu + (top-1)*16 */
            a64_ldr(b, 8, JTF, JTA, RAS_OFF);           /* cpu->ras[top-1].guest_rip */
            a64_ldr(b, 8, JT0, JTA, RAS_OFF + 8);       /* cpu->ras[top-1].host_entry */
            a64_ldr(b, 8, JTU, 20, RIP_OFF);            /* popped return addr */
            a64_sub_reg(b, 1, JTT, JTF, JTU, 0);        /* 0 iff predicted == popped */
            ras_miss[nrm] = a64_label(b); a64_cbnz(b, 1, JTT, 0); nrm++;  /* mispredict */
            ras_miss[nrm] = a64_label(b); a64_cbz(b, 1, JT0, 0); nrm++;   /* NULL entry */
            /* Matched: commit the pop, then tail-call the predicted block. */
            a64_str(b, 4, JT2, 20, RAS_TOP_OFF);        /* ras_top = top-1 */
            /* Tail-call to the predicted block: spill pinned guest regs so the
             * target's prologue loads current values from cpu->gpr[], then
             * restore this block's callee-saved x21-x28 before tearing the frame
             * down. JT0 (host_entry) survives all of it. No-op when unpinned. */
            emit_spill_pinned(b);
            a64_mov_reg(b, 1, 0, 19);                   /* x0 = vm */
            a64_mov_reg(b, 1, 1, 20);                   /* x1 = cpu */
            emit_pin_epilogue_restore(b);               /* restore x21-x28 */
            a64_ldp_post(b, 19, 20, 31, 16);            /* undo prologue (frame) */
            a64_ldp_post(b, 29, 30, 31, 16);
            a64_br(b, JT0);                             /* JT0 survives both ldp */
            uint32_t *miss = a64_label(b);
            for (int i = 0; i < nrm; i++)
                a64_patch_cbz(ras_miss[i], miss);
        }
    } else {
        return 0;
    }

    /* Shared tail, identical to emit_jcc's: STEP_OK in x0, then route to the
     * block's single epilogue. Reached by BOTH the native body and the guard's
     * slow path (whose own x0 is already STEP_OK; re-loading it is harmless and
     * keeps the two paths textually convergent). */
    a64_mov_imm64(b, 0, OCERZ_STEP_OK);
    epi_sites[*n_epi] = a64_label(b);
    /* Capture the terminal STEP_OK branch so translate() can redirect it into a
     * chain tail. Only meaningful when g_chain_target != 0 (set by the CALL arm
     * above); the RET arm leaves g_chain_target 0, so this is ignored for RET. */
    g_chain_epi = epi_sites[*n_epi];
    a64_b(b, 0);
    (*n_epi)++;
    return 1;
}

static void emit_slowcall(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    /* The interpreter reads and writes cpu->rflags directly and knows nothing of
     * the deferred record, so make rflags live (and cc_op==NONE) before it runs.
     * Covers BOTH the main-loop slow path and the commpage-guard slow path this
     * function also emits -- on the latter, materializing a predecessor here is
     * what lets the interpreter's own flag write become authoritative. */
    emit_materialize(b);
    /* The interpreter reads and writes cpu->gpr[] directly, so spill every
     * pinned guest reg out before the call and reload after. The reload sits
     * BEFORE the cbnz exit check so the block's shared epilogue (which spills
     * again) writes back the interpreter's values, not stale pinned ones, on
     * the non-OK exit path. Both are no-ops when nothing is pinned. */
    emit_spill_pinned(b);
    a64_mov_reg(b, 1, 0, 19);
    a64_mov_reg(b, 1, 1, 20);
    a64_mov_imm64(b, 2, (uint64_t)(uintptr_t)insn);
    a64_mov_imm64(b, 16, (uint64_t)(uintptr_t)&ocerz_jit_exec_one);
    a64_blr(b, 16);
    emit_fill_pinned(b);
    exit_sites[*n_exits] = a64_label(b);
    a64_cbnz(b, 0, 0, 0);
    (*n_exits)++;
}

/* ---- OCERZ_JITSTAT: TEMPORARY measurement instrumentation (log-only, gated) ----
 * Answers: is translate() hot because failures are never cached (each retry
 * re-decodes under the global jit_lock), or because the app legitimately runs a
 * huge amount of one-shot code? Counters are relaxed atomics and only touched
 * when the env var is set; behaviour is unchanged either way. */
int ocerz_jitstat = -1;
static _Atomic unsigned long long js_steps, js_hits, js_misses;
static _Atomic unsigned long long js_xlat, js_xlat_ok, js_xlat_fail;
static _Atomic unsigned long long js_fail_decode0, js_fail_overflow, js_fail_alloc;
static _Atomic unsigned long long js_decoded_insns; /* decode work on the miss path */
static uint64_t js_t0;

/* Per-rip failure table (single-writer: only ever touched under jit_lock). */
#define JS_FTAB (1u << 20)
typedef struct { uint64_t rip; unsigned long long n; unsigned char bytes[8];
                 unsigned reason; int nins; } JsFail;
static JsFail js_ftab[JS_FTAB];
static unsigned js_ftab_used, js_ftab_full;

/* reason codes */
enum { JSR_DECODE0 = 1, JSR_OVERFLOW = 2, JSR_ALLOC = 3 };

static void js_note_fail(uint64_t rip, unsigned reason, int nins)
{
    /* Wide hash: hash_rip() only yields JIT_HASH_BITS, too few for this table.
     * Probe is bounded (a full 1M-slot scan per miss would dwarf what we measure). */
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
    /* qsort over JS_FTAB is expensive; only on the slow (60s) cadence. */
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

/* ---- BLOCK CHAINING: activation + pending table -------------------------
 * All of this runs ONLY on the cold translate/miss path, under jit_lock (single
 * writer). Executors never write code.
 *
 * chain_activate rewrites the one activation B word `patch_b` (inside an already-
 * committed source block) to branch to `dst` (a compiled target's host entry).
 * a64_try_patch_b rejects an out-of-B-range target WITHOUT writing (arena is 1GB
 * > B's +-128MB reach), in which case the edge is simply left unchained -- the
 * fallback path is a correct STEP_OK return to the dispatcher, so a far edge
 * costs a missed optimization, never correctness. The store is a single aligned
 * 32-bit write (single-copy-atomic on arm64): a guest thread concurrently
 * executing the source block fetches either the old word (-> fallback ->
 * dispatcher, correct) or the new word (-> dst, correct), never a torn
 * instruction. W^X toggle is per-thread, so making the patcher's view writable
 * does not disturb another thread's executable view. */
static void chain_activate(uint32_t *patch_b, void *dst)
{
    if (!patch_b || !dst)
        return;
    pthread_jit_write_protect_np(0);
    a64_try_patch_b(patch_b, (uint32_t *)dst);   /* leaves it at fallback if out of range */
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(patch_b, 4);
}

/* Pending edges whose target rip was not yet compiled when the source block was
 * translated. Keyed by target_rip in a fixed chained hash; entries are malloc'd
 * and freed on drain. Touched only under jit_lock, so no atomics are needed. */
typedef struct PendingChain {
    uint64_t target_rip;
    uint32_t *patch_b;
    struct PendingChain *next;
} PendingChain;
#define PEND_BITS 12
#define PEND_SIZE (1u << PEND_BITS)
#define PEND_MASK (PEND_SIZE - 1)
static PendingChain *g_pending[PEND_SIZE];

static void pending_add(uint64_t target_rip, uint32_t *patch_b)
{
    PendingChain *e = (PendingChain *)malloc(sizeof *e);
    if (!e)
        return;   /* out of memory: the edge just stays unchained, still correct */
    unsigned h = (unsigned)(hash_rip(target_rip) & PEND_MASK);
    e->target_rip = target_rip;
    e->patch_b = patch_b;
    e->next = g_pending[h];
    g_pending[h] = e;
}

/* A block at `rip` just became compiled: activate every source edge that was
 * waiting on it. Removes the drained entries. */
static void pending_drain(uint64_t rip, void *code)
{
    unsigned h = (unsigned)(hash_rip(rip) & PEND_MASK);
    PendingChain **pp = &g_pending[h];
    while (*pp) {
        PendingChain *e = *pp;
        if (e->target_rip == rip) {
            chain_activate(e->patch_b, code);
            *pp = e->next;
            free(e);
        } else {
            pp = &e->next;
        }
    }
}

/* Emit the XBLOCK chain tail for a chainable CALL(imm) terminator and return the
 * activation B word. The terminal STEP_OK exit is redirected here (by the caller)
 * INSTEAD of the shared exit_label. This mirrors the committed RAS RET tail-call
 * teardown exactly, minus the RAS lookup: flush pins to cpu->gpr[], reload x0=vm
 * / x1=cpu for the target's prologue, restore the pinned callee-saved regs, tear
 * the frame down, then the single activation branch:
 *     [patch_b]: b fallback        (chained -> b target_entry)
 *     fallback:  mov x0,#STEP_OK ; ret
 * Unchained (or far/out-of-range), patch_b targets `fallback`, which restores
 * STEP_OK (the tail clobbered x0 with vm) and returns to the dispatcher -- a
 * correct, semantically-identical exit. Net frame delta across a chained edge is
 * zero (source teardown here + target's own prologue build-up).
 *
 * BACK-EDGE INTERRUPT POLL (STEP 2). When `poll` is set (a chained arm whose
 * target could close a control cycle, i.e. is already compiled or points back at
 * or below this block's entry), emit the PROVEN self-loop poll at the very TOP of
 * the tail -- BEFORE any register is clobbered, while x20 still holds cpu -- so a
 * cross-thread/process stop that sets cpu->interrupt is observed and routed to the
 * unchained fallback (STEP_OK -> dispatcher) instead of taking the chained branch.
 * Without it a fully-linked loop would never re-enter the dispatcher and could
 * never be broken (tests/guest/link_spin is the gate). The caller stores this
 * arm's rip constant BEFORE calling, so the fallback exit carries the correct rip.
 * The poll is one L1 load + a predicted-not-taken cbnz; forward arms pass poll=0
 * so the STEP-1 forward-diamond win is unaffected. */
static uint32_t *emit_chain_tail(A64Buf *b, int poll)
{
    uint32_t *intr = NULL;
    /* Read cpu->interrupt into JT1 BEFORE the teardown, while x20 still holds cpu.
     * JT1 (x10) is caller-saved scratch that the teardown below never touches
     * (spill/restore use x21-x28 + x20; the ldp pairs restore only x19/x20/x29/x30),
     * so the flag survives to the post-teardown cbnz. Reading here and branching
     * AFTER teardown is essential: the frame restore (ldp) MUST run on every exit,
     * so the interrupt exit cannot short-circuit past it -- it can only choose the
     * unchained fallback over the chained branch. */
    if (poll)
        a64_ldr(b, 4, JT1, 20, INT_OFF);   /* JT1 = cpu->interrupt (x20 still = cpu) */
    emit_spill_pinned(b);
    a64_mov_reg(b, 1, 0, 19);              /* x0 = vm  (before ldp restores x19) */
    a64_mov_reg(b, 1, 1, 20);              /* x1 = cpu */
    emit_pin_epilogue_restore(b);          /* restore x21-x28 */
    a64_ldp_post(b, 19, 20, 31, 16);       /* undo prologue (frame) */
    a64_ldp_post(b, 29, 30, 31, 16);
    if (poll) {
        intr = a64_label(b);
        a64_cbnz(b, 0, JT1, 0);            /* stop requested -> fallback (skip chained b) */
    }
    uint32_t *patch_b = a64_label(b);
    a64_b(b, 0);                           /* the single activation word */
    uint32_t *fallback = a64_label(b);
    a64_patch_b(patch_b, fallback);        /* initial (unchained) target: adjacent, in range */
    if (poll)
        a64_patch_cbz(intr, fallback);     /* interrupt -> unchained dispatcher exit (frame already torn down) */
    a64_mov_imm64(b, 0, OCERZ_STEP_OK);    /* restore STEP_OK clobbered by x0=vm above */
    a64_ret(b);
    return patch_b;
}

static JitBlock *translate(OcerzJit *jit, uint64_t rip)
{
    /* OCERZ_JITMEASURE (Stage-0 verify-first for the persistent JIT cache): time each successful
     * translation and split out shared-cache code (rip>=0x7ff800000000, the AppKit/libobjc/etc that
     * dominates Cocoa cold-start and is the cache's relocation-free target). If the shared-cache
     * translate-ms is a large fraction of a process's wall time, caching it pays; if tiny, the
     * cold-start is execution/IPC-bound and the cache is the wrong lever. Log-only, gated. */
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

    /* ---- REGISTER PINNING allocation --------------------------------------
     * Pin the hot subset of guest GPRs (by static reference count over this
     * block's decoded operands) into the callee-saved host regs x21..x28. RSP
     * is excluded: its stack-op accesses stay memory-resident so push/pop/call/
     * ret keep their delicate fault-atomic form. Every other GPR is eligible;
     * correctness never depends on the choice (an unpinned reg keeps the memory
     * path, and every un-inlined op spills/fills around its slow call). The
     * whole mechanism is off under OCERZ_NO_REGFLAGS. Published to the emit
     * helpers via g_pin/g_pin_hold/g_n_pinned (single-writer under jit_lock). */
    for (int g = 0; g < 16; g++)
        blk->guest_in_host[g] = -1;
    blk->n_pinned = 0;
    g_pin = NULL;
    g_pin_hold = NULL;
    g_n_pinned = 0;
    g_defer = 0;
    blk->n_edges = 0;
    g_chain_target = 0;
    g_chain_epi = NULL;
    g_n_jcc_edges = 0;
    g_xlat_jit = jit;
    g_self_rip = rip;
    g_body_entry = NULL;   /* set once the prologue has been emitted (below) */
    /* SIZE GATE (loop/large-block heuristic). The prologue/epilogue load+spill
     * of the pinned regs is a FIXED per-block-execution cost (~2*np words in,
     * ~2*np out) that a self-loop re-pays every iteration, so it only wins when
     * the block does enough pinned-register work to amortize it. Tiny blocks --
     * fib's call/ret frames, and the data-dependent `if/else if` chains that
     * split a branch loop into 5-6 instruction fragments -- pay the tax on
     * nearly no work and REGRESS. Gate pinning to blocks of at least
     * OCERZ_PIN_MIN_INSNS instructions (default 24): the alu hot self-loop
     * (26-31 insns) pins and defers, while mem's memory-latency-bound loop
     * (19-23 insns, no register-traffic win) and the br/call flag-consuming
     * fragments (5-6 insns) stay on HEAD's eager memory path. Calibrated by an
     * OCERZ_NO_REGFLAGS A/B sweep: 24 is the value that keeps alu's 1.86x while
     * holding mem/br/call within noise of HEAD (a lower gate regresses the
     * memory/branch loops on the per-iteration materialize + pin tax; a higher
     * one loses alu, whose block is 26). Env-overridable; read once. */
    static int g_pin_min = -1;
    if (g_pin_min < 0) {
        const char *e = getenv("OCERZ_PIN_MIN_INSNS");
        g_pin_min = e ? (int)strtol(e, NULL, 0) : 24;
        if (g_pin_min < 1)
            g_pin_min = 1;
    }
    if (!g_no_regflags && n >= g_pin_min) {
        /* This block is a pinning candidate, so it also DEFERS flags: the two
         * halves are the pair, applied together only where they pay. */
        g_defer = 1;
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
        cnt[OCERZ_RSP] = 0;                 /* never pin RSP */
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

    pthread_jit_write_protect_np(0);
    A64Buf b = { jit->code_cur, jit->code_cur, jit->code_end, 0, 0 };
    uint32_t *entry = b.p;

    a64_stp_pre(&b, 29, 30, 31, -16);
    a64_stp_pre(&b, 19, 20, 31, -16);
    a64_mov_reg(&b, 1, 19, 0);
    a64_mov_reg(&b, 1, 20, 1);
    /* Save the callee-saved host regs this block pins and load their guest
     * values from cpu->gpr[]. No-op when nothing is pinned. */
    emit_pin_prologue(&b);

    /* OCERZ_PERFSTAT (TEMPORARY): per-block execution counter, incremented in
     * the prologue so every entry is counted exactly once regardless of which
     * exit the block takes. Scratch JT0/JT1 are dead here. */
    if (ocerz_perfstat > 0) {
        a64_mov_imm64(&b, JT0, (uint64_t)(uintptr_t)&blk->exec_count);
        a64_ldr(&b, 8, JT1, JT0, 0);
        a64_add_imm(&b, 1, JT1, JT1, 1);
        a64_str(&b, 8, JT1, JT0, 0);
    }

    /* BODY-ENTRY (Increment 2 self-loop chaining): the label past the prologue.
     * A Jcc self-loop back-edge branches here directly, so x19=vm / x20=cpu (both
     * callee-saved, never clobbered by a block body) and the pinned guest regs in
     * x21..x28 stay live across the loop -- no per-iteration frame rebuild or
     * pinned-reg reload. Captured only when chaining is on; NULL otherwise leaves
     * emit_jcc on the dispatcher-return path. Published for emit_jcc via the
     * single-writer static, like g_pin. */
    if (!g_no_chain)
        g_body_entry = a64_label(&b);

    /* ---- BACKWARD FLAG LIVENESS ------------------------------------------
     * fl_need[i] = the flags insn i writes that something downstream reads, i.e.
     * def[i] & live_out[i]. The emitter computes exactly those.
     *
     * live_out of the LAST instruction is ALL SIX, unconditionally. That single
     * rule is what makes this pass sound without any cross-block analysis: a
     * successor block, a signal handler, or the interpreter may read any flag on
     * entry, and this translator has no idea which. It is also why the win is
     * real anyway — inside a block, each arithmetic op's flags are almost always
     * killed by the next one, and only the block's last producer survives.
     *
     * A block that runs off the length cap falls through to a successor exactly
     * like one ending on a terminator, so it needs the same live-out=ALL; the
     * rule is applied to the last instruction either way and needs no special
     * case.
     *
     * Standard dataflow: live_in = (live_out & ~def) | use. Every entry in the
     * def/use table is conservative in the safe direction (see
     * include/ocerz/flags_live.h), so an unmodelled op degrades to today's eager
     * behaviour rather than to a wrong answer. */
    uint64_t fl_need[JIT_MAX_BLOCK_INSNS];
    {
        uint64_t live_out = OCERZ_FL_ALL;
        for (int i = n - 1; i >= 0; i--) {
            uint64_t def, use;
            ocerz_flags_defuse(&blk->insns[i], &def, &use);
            fl_need[i] = def & live_out;
            live_out = (live_out & ~def) | use;
            /* OCERZ_NO_LAZYFLAGS: kill switch. Emit every flag the op defines,
             * i.e. exactly the eager behaviour this pass replaced. Keeps the
             * old codegen one env var away for A/B measurement and for bisecting
             * any suspected flag bug against the pass itself. */
            if (g_no_lazyflags)
                fl_need[i] = def;
        }
    }

    /* OCERZ_FLAGLIVE: per-block dump of what the pass proved dead. This is the
     * cross-check that the def/use table is actually doing what it claims — a
     * table that quietly proves nothing would be invisible in the test suite and
     * would merely make the JIT eager again. Log-only, gated. */
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
    /* Allocation failure here is not fatal: insn_off is a DIAGNOSTIC precision
     * aid, so a block without it still executes correctly and merely falls back
     * to cur_rip on a fault, exactly as before this table existed. */
    blk->insn_off = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));

    for (int i = 0; i < n; i++) {
        const X86Insn *insn = &blk->insns[i];
        if (blk->insn_off)
            blk->insn_off[i] = (uint32_t)(b.p - entry);
        if (i == n - 1 && insn->op == OCERZ_OP_JCC) {
            if (emit_jcc(&b, insn, epi_sites, &n_epi)) {
                blk->n_inlined++;
                continue;
            }
        }
        /* CALL(imm)/RET native terminators. Like emit_jcc these set cpu->rip
         * themselves and route straight to the epilogue, so they are only valid
         * in the terminator slot -- which is the only place they can appear,
         * since is_terminator() ends a block on both. */
        if (i == n - 1 && (insn->op == OCERZ_OP_CALL || insn->op == OCERZ_OP_RET)) {
            if (emit_call_ret(&b, insn, exit_sites, &n_exits, epi_sites, &n_epi)) {
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

    /* A block that runs off the JIT_MAX_BLOCK_INSNS limit ends on a non-
     * terminator whose last instruction may have been inlined; inlined
     * instructions never write cpu->rip, and no slow call or terminator ran to
     * restore it, so the fall-through exit would leave rip pointing inside the
     * block. Set rip to the fall-through address (pc, advanced past the last
     * decoded instruction) on the fall-through path only. Early exits (slow-call
     * non-OK returns and the Jcc terminator) branch to exit_label below, which
     * sits AFTER this store, so they keep the rip they computed. Blocks that end
     * on a real terminator skip it entirely — the terminator already set rip. */
    if (!is_terminator(blk->insns[n - 1].op)) {
        /* Falling off the length cap is a block exit: make any deferred flags
         * live so the next block / out-of-JIT code sees a current rflags.
         * emit_materialize may clobber x0, so (re)establish STEP_OK as the block
         * return value afterward -- the fall-through path had no terminator to
         * set it. Early exits branch to exit_label BELOW this, so they are
         * unaffected. */
        emit_materialize(&b);
        a64_mov_imm64(&b, JT0, pc);
        a64_str(&b, 8, JT0, 20, RIP_OFF);
        a64_mov_imm64(&b, 0, OCERZ_STEP_OK);
    }

    /* Shared block epilogue. Every exit (Jcc/CALL/RET native terminators, a
     * slow call's non-OK return, and the length-cap fall-through) routes here.
     * Spill the pinned guest regs back to cpu->gpr[] so the caller and the next
     * dispatch see current values, then restore the callee-saved host regs and
     * tear the frame down. Both pin steps are no-ops when nothing is pinned. */
    uint32_t *exit_label = a64_label(&b);
    emit_spill_pinned(&b);
    emit_pin_epilogue_restore(&b);
    a64_ldp_post(&b, 19, 20, 31, 16);
    a64_ldp_post(&b, 29, 30, 31, 16);
    a64_ret(&b);

    /* CHAINING (Increment 1): for a chainable CALL(imm) terminator, emit the
     * XBLOCK chain tail AFTER the shared epilogue (so it lives inside this
     * block's [code,code+code_words) span for the fault seam) and remember the
     * activation word. The terminal STEP_OK branch is redirected into it below,
     * once the shared epi patch loop has run. Only the terminal branch is
     * redirected; the slow-call non-OK cbnz exits keep pointing at exit_label
     * (a genuine error must still return to the dispatcher, not chain). */
    uint32_t *chain_tail_lbl = NULL;
    uint32_t *chain_patch_b = NULL;
    if (!g_no_chain && g_chain_target) {
        chain_tail_lbl = a64_label(&b);
        chain_patch_b = emit_chain_tail(&b, 0);   /* CALL-imm target is the callee entry (forward); no poll */
    }

    /* The number that decides whether the "time is proportional to emitted
     * words" model actually holds. Reported next to the guest insn count so the
     * emitted-per-guest ratio is directly readable. */
    if (g_flaglive_log)
        fprintf(stderr, "ocerz: FLAGLIVE rip=%#llx EMITTED words=%d guest=%d per_guest=%.2f\n",
                (unsigned long long)rip, (int)(b.p - entry), n,
                (double)(b.p - entry) / (double)n);

    /* On overflow a64_emit32 clamps at b->p == b->end, so a64_label() can have
     * handed out b->end itself; patching would store one word PAST the MAP_JIT
     * mapping. The block is not going to run as code — never patch it. */
    if (!b.overflow) {
        for (int i = 0; i < n_exits; i++)
            a64_patch_cbz(exit_sites[i], exit_label);
        for (int i = 0; i < n_epi; i++)
            a64_patch_b(epi_sites[i], exit_label);
        /* Override the CALL's terminal STEP_OK branch (one of the epi_sites just
         * patched to exit_label) to land in the chain tail instead. Done after
         * the loop so it wins. Unchanged when there is no chainable terminator. */
        if (chain_tail_lbl && g_chain_epi)
            a64_patch_b(g_chain_epi, chain_tail_lbl);
    }

    pthread_jit_write_protect_np(1);

    if (b.overflow) {
        /* The code arena is full. That is a fact about the ARENA, not about this
         * block: the decode above SUCCEEDED and blk->insns is a complete, valid
         * block. The compile is an optimization; the decode is not. Keep the
         * block with code == NULL and cache it — ocerz_jit_step dispatches it to
         * the interpreter tier, from the cache, with no lock and no re-decode.
         * Standard tiered-JIT demotion: when the compile can't happen you keep
         * the decoded entry and run it a tier down.
         *
         * Discarding it is what made exhaustion catastrophic rather than merely
         * slow: the failure was never cached, so every later execution of this
         * rip re-took the global jit_lock and redid decode+malloc+emit+free; and
         * vm.c's one-instruction EUNSUP fallback then re-entered at rip+len and
         * re-decoded the tail at EVERY instruction boundary.
         *
         * code_cur is deliberately NOT advanced and the icache NOT invalidated:
         * nothing was committed. */
        if (!jit->code_full) {
            jit->code_full = 1;
            OCERZ_LOG("JIT code arena full (%zu MB, %llu blocks); further blocks run interpreted\n",
                      jit->code_bytes >> 20,
                      (unsigned long long)jit->blocks_translated);
        }
        if (ocerz_jitstat > 0) { js_fail_overflow++; js_note_fail(rip, JSR_OVERFLOW, n); }
        /* Demoted to the interpreter tier: every instruction is a slow path,
         * whatever the (discarded) compile would have inlined. The discarded
         * prologue never ran, so this block pins nothing at runtime; clear
         * n_pinned so the fault seam (which only consults compiled ci[] blocks
         * anyway) never mistakes it for one. */
        blk->n_slow = n;
        blk->n_inlined = 0;
        blk->n_pinned = 0;
        blk->code = NULL;
        g_pin = NULL; g_pin_hold = NULL; g_n_pinned = 0;
        cache_insert(jit, blk);
        return blk;
    }

    sys_icache_invalidate(entry, (size_t)((b.p - entry) * 4));
    jit->code_cur = b.p;
    blk->code = (JitBlockFn)entry;
    blk->code_words = (uint32_t)(b.p - entry);

    /* Record the outgoing chain edges. patch_b currently branches to an in-block
     * fallback (unchained STEP_OK return); activated below once the target is
     * known compiled. A block terminates on EXACTLY ONE terminator, so it has
     * EITHER a CALL edge (chain_patch_b, at most one) OR up to two Jcc arm edges
     * (g_jcc_edge) -- never both. Assert that CALL-XOR-Jcc invariant. */
    assert(!(chain_patch_b && g_n_jcc_edges) &&
           "block cannot have both a CALL chain edge and Jcc arm edges");
    if (chain_patch_b) {
        blk->edges[0].target_rip = g_chain_target;
        blk->edges[0].patch_b = chain_patch_b;
        blk->edges[0].kind = EDGE_XBLOCK;
        blk->n_edges = 1;
    } else if (g_n_jcc_edges) {
        for (int i = 0; i < g_n_jcc_edges; i++) {
            blk->edges[i].target_rip = g_jcc_edge[i].target_rip;
            blk->edges[i].patch_b = g_jcc_edge[i].patch_b;
            blk->edges[i].kind = EDGE_XBLOCK;
        }
        blk->n_edges = (uint8_t)g_n_jcc_edges;
    }

    /* OCERZ_JITDIS=<file> (MEASUREMENT ONLY, gated, off by default): dump the
     * emitted arm64 for every compiled block, split at the per-guest-instruction
     * boundaries insn_off already records, with the prologue and the shared
     * epilogue attributed separately. This lets emitted-words-per-guest-insn and
     * the flag/register-traffic share be COUNTED from the code that actually
     * runs, instead of estimated by reading the emitter. Runs under jit_lock at
     * translate time only; it emits no code and costs a compiled block nothing
     * when the env var is unset. */
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
            fprintf(g_jf, "BLOCK rip=%#llx words=%u n_insns=%d inlined=%d slow=%d"
                          " prologue_words=%u epilogue_words=%u\n",
                    (unsigned long long)rip, blk->code_words, n,
                    blk->n_inlined, blk->n_slow, blk->insn_off[0],
                    blk->code_words - epi);
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

    /* Register in the code-address index (see OcerzJit::ci). Runs under
     * jit_lock. On grow failure the index simply stops accepting new blocks:
     * fault-rip precision degrades for later blocks, execution does not. */
    if (jit->ci_n == jit->ci_cap) {
        size_t ncap = jit->ci_cap ? jit->ci_cap * 2 : 4096;
        JitBlock **nci = (JitBlock **)realloc(jit->ci, ncap * sizeof *nci);
        if (nci) {
            jit->ci = nci;
            jit->ci_cap = ncap;
        }
    }
    if (jit->ci_n < jit->ci_cap) {
        jit->ci[jit->ci_n] = blk;
        __atomic_store_n(&jit->ci_n, jit->ci_n + 1, __ATOMIC_RELEASE);
    }

    cache_insert(jit, blk);

    /* CHAINING activation (Increment 1). The block is now published in the cache
     * and the ci[] index, so both directions are safe:
     *  (a) resolve this block's OUTGOING edge -- if the target is already a
     *      compiled block, activate now; otherwise park it in the pending table
     *      keyed by target rip.
     *  (b) drain INCOMING edges -- any earlier block that chained to this rip and
     *      found it uncompiled parked a pending edge; activate them now.
     * Both run under jit_lock (single writer). chain_activate brackets its own
     * W^X toggle and icache flush. */
    if (!g_no_chain) {
        for (int i = 0; i < blk->n_edges; i++) {
            JitBlock *t = cache_lookup(jit, blk->edges[i].target_rip);
            if (t && t->code)
                chain_activate(blk->edges[i].patch_b, (void *)t->code);
            else
                pending_add(blk->edges[i].target_rip, blk->edges[i].patch_b);
        }
        pending_drain(blk->guest_rip, (void *)blk->code);
    }

    jit->blocks_translated++;
    if (ocerz_jitstat > 0)
        js_xlat_ok++;
    if (g_jitmeasure) {
        /* translate() runs under jit_lock, so plain statics are race-free here. */
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

/* Exact guest rip for a host pc inside emitted JIT code.
 *
 * Called ONLY from the host fault handler, i.e. a cold path that has already
 * lost a page fault, so the two binary searches are free relative to the trap.
 * That is what buys the hot path its zero instructions: the alternative is a
 * cpu->rip store before every inlined memory access.
 *
 * Returns 1 and writes *out_rip on success; 0 when the pc is not in a compiled
 * block (an ocerz host-code fault, or a block predating the index), leaving the
 * caller on its cur_rip fallback.
 *
 * Lock-free by construction: entries are append-only in ascending code order,
 * blocks are never freed, and ci_n is read with an acquire load matching
 * translate()'s release store, so every entry below the observed count is fully
 * published. Safe to run in a signal handler -- no locks, no allocation. */
int ocerz_jit_pc_in_arena(const struct OcerzVM *vm, const void *host_pc)
{
    const OcerzJit *jit = vm ? vm->jit : NULL;
    if (!jit)
        return 0;
    const uint32_t *pc = (const uint32_t *)host_pc;
    return pc >= jit->code_base && pc < jit->code_end;
}

/* Shared cold-path lookup: the compiled block whose emitted code contains
 * host_pc, or NULL. Factored out of ocerz_jit_fault_rip so the fault seam's
 * register recovery (ocerz_jit_fault_recover_regs) uses the identical lock-free
 * ci[] binary search. Signal-safe: append-only ascending ci[], never-freed
 * blocks, acquire load of ci_n against translate()'s release store. */
static const JitBlock *fault_block(const OcerzJit *jit, const uint32_t *pc)
{
    if (!jit || !jit->ci)
        return NULL;
    if (pc < jit->code_base || pc >= jit->code_end)
        return NULL;
    size_t n = __atomic_load_n(&jit->ci_n, __ATOMIC_ACQUIRE);
    if (!n)
        return NULL;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if ((const uint32_t *)jit->ci[mid]->code <= pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return NULL;
    const JitBlock *b = jit->ci[lo - 1];
    const uint32_t *base = (const uint32_t *)b->code;
    if (!base || pc >= base + b->code_words)
        return NULL;   /* in the arena but past this block: inter-block padding */
    return b;
}

/* FAULT SEAM for register pinning: a fault taken in emitted JIT code leaves the
 * pinned guest regs live in the host mcontext's x21..x28, not in cpu->gpr[]
 * (which still holds their block-entry values). Recover them so the delivered
 * guest signal frame and the crash dump see the true register file. host_x is
 * &uc->uc_mcontext->__ss.__x[0] (a uint64_t[29]); slot i lives in x21+i. Called
 * ONLY for an in-arena fault; outside the arena the slow-call spill already made
 * cpu->gpr[] current. No-op for a block that pins nothing. Signal-safe. */
void ocerz_jit_fault_recover_regs(const struct OcerzVM *vm, const void *host_pc,
                                  const uint64_t *host_x, OcerzCPU *cpu)
{
    const OcerzJit *jit = vm ? vm->jit : NULL;
    const JitBlock *b = fault_block(jit, (const uint32_t *)host_pc);
    if (!b || !host_x || !cpu)
        return;
    for (int i = 0; i < b->n_pinned; i++)
        cpu->gpr[b->host_holds[i]] = host_x[21 + i];
}

int ocerz_jit_fault_rip(const struct OcerzVM *vm, const void *host_pc, uint64_t *out_rip)
{
    const OcerzJit *jit = vm ? vm->jit : NULL;
    const uint32_t *pc = (const uint32_t *)host_pc;
    const JitBlock *b = fault_block(jit, pc);
    if (!b || !b->insn_off)
        return 0;
    const uint32_t *base = (const uint32_t *)b->code;

    /* Last instruction whose emitted code starts at or below pc. */
    uint32_t off = (uint32_t)(pc - base);
    int ilo = 0, ihi = b->n_insns;
    while (ilo < ihi) {
        int mid = ilo + (ihi - ilo) / 2;
        if (b->insn_off[mid] <= off)
            ilo = mid + 1;
        else
            ihi = mid;
    }
    if (ilo == 0)
        return 0;   /* in the prologue, before any guest instruction */
    *out_rip = b->insns[ilo - 1].rip;
    return 1;
}

OcerzJit *ocerz_jit_create(struct OcerzVM *vm)
{
    OcerzJit *jit = (OcerzJit *)calloc(1, sizeof *jit);
    if (!jit)
        return NULL;
    jit->vm = vm;
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

void ocerz_jit_destroy(OcerzJit *jit)
{
    if (!jit)
        return;
    if (ocerz_perfstat > 0)
        ps_report(jit);   /* final numbers for short runs (guest microbenches) */
    for (unsigned i = 0; i < JIT_HASH_SIZE; i++) {
        JitBlock *b = jit->buckets[i];
        while (b) {
            JitBlock *next = b->hnext;
            free(b->insns);
            free(b);
            b = next;
        }
    }
    munmap(jit->code_base, jit->code_bytes);
    free(jit);
}

uint64_t ocerz_jit_blocks(const OcerzJit *jit)
{
    return jit ? jit->blocks_translated : 0;
}

static pthread_mutex_t jit_lock = PTHREAD_MUTEX_INITIALIZER;

/* Storage for the translate()-time decode recovery point declared in jit.h. */
__thread sigjmp_buf *ocerz_jit_decode_recover;

void ocerz_jit_prefork(void)
{
    pthread_mutex_lock(&jit_lock);
}

void ocerz_jit_postfork(void)
{
    pthread_mutex_unlock(&jit_lock);
}

/* Interpreter tier for a cached-but-uncompiled block (arena full). Mirrors the
 * compiled block exactly rather than approximating it: the compiled form calls
 * ocerz_jit_exec_one per instruction via emit_slowcall and branches to its
 * epilogue on the first non-OK return (the cbnz x0 at each exit site); this runs
 * the same ocerz_jit_exec_one over the same blk->insns and returns on the first
 * non-OK the same way. Fall-through rip needs no fixup: ocerz_jit_exec_one always
 * stores insn->rip + insn->len, which is exactly what the compiled block's
 * explicit rip store reconstructs for inlined instructions. */
static int jit_interp_block(struct OcerzVM *vm, OcerzCPU *cpu, JitBlock *b)
{
    if (ocerz_perfstat > 0)
        b->exec_count++;   /* mirrors the compiled prologue's counter */
    for (int i = 0; i < b->n_insns; i++) {
        int r = ocerz_jit_exec_one(vm, cpu, &b->insns[i]);
        if (r != OCERZ_STEP_OK)
            return r;
    }
    return OCERZ_STEP_OK;
}

/* OCERZ_PERFSTAT report. Walks the block cache (never freed, so the walk is
 * safe against concurrent inserts: hnext is set before publish) to sum executed
 * instructions by tier, then prints the slow-path op histogram sorted by count.
 * Called from the miss path under jit_lock, time-gated. */
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
    /* Hash-chain geometry. cache_lookup walks from the bucket head and
     * cache_insert pushes at the head, so a block sitting at 1-based position p
     * costs p pointer-chases on every lookup that finds it. Weighting p by the
     * block's exec_count therefore yields the ACTUAL mean probes per lookup the
     * dispatcher pays -- unlike the raw load factor, which weights a cold block
     * the same as one entered a hundred million times. */
    unsigned long long nonempty = 0, maxchain = 0;
    unsigned long long probe_w = 0;   /* sum over blocks of position * exec_count */
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
    for (int i = 0; i < 6; i++) {
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
            g_flaglive_log = getenv("OCERZ_FLAGLIVE") ? 1 : 0;
            g_no_lazyflags = getenv("OCERZ_NO_LAZYFLAGS") ? 1 : 0;
            g_no_ras = getenv("OCERZ_NO_RAS") ? 1 : 0;
            g_no_regflags = getenv("OCERZ_NO_REGFLAGS") ? 1 : 0;
            g_no_chain = getenv("OCERZ_NO_CHAIN") ? 1 : 0;
            g_no_jcclink = getenv("OCERZ_NO_JCCLINK") ? 1 : 0;
        }
        pthread_mutex_unlock(&jit_lock);
    }
    if (ocerz_jitstat > 0)
        js_steps++;
    if (ocerz_perfstat > 0) {
        /* Report from the STEP path, not the miss path: once the app reaches its
         * window, misses stop but execution does not, and the steady-state
         * numbers are exactly the ones under study. Gated on a cheap mask of the
         * value we just fetched, then on wall time; the cache walk needs no lock
         * (blocks are never freed and hnext is set before publish). */
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
    /* THIS PATH IS EXTRAORDINARILY SENSITIVE. DO NOT ADD ANYTHING HERE,
     * INCLUDING DIAGNOSTICS, WITHOUT MEASURING alu.
     *
     * An OCERZ_DISPATCH_TAX=<n> probe briefly lived here to measure the
     * dispatcher's marginal cost (n extra cache_lookups per dispatch, slope =
     * cost per lookup). Default-off, gated on `__builtin_expect(tax > 0, 0)`
     * against a global fixed before the loop -- i.e. one load and one
     * never-taken, perfectly-predicted branch. It cost alu 13.8%
     * (2.275s -> 2.589s, ~10.8 cyc per block, variance +/-0.01s so this is not
     * drift). alu's hot block is a self-loop that dispatches 100M times, and
     * that is enough for a single extra load+branch in ocerz_jit_step to be
     * plainly visible. The probe was reverted for exactly this reason.
     *
     * WHAT IT MEASURED, before it was removed (fib, two-point, same binary,
     * tax = 0/1/2/4):
     *   +14.5 cyc for the FIRST extra lookup (serialized against the real one)
     *    +6.8 cyc/lookup least-squares slope (redundant same-rip L1 hits, fully
     *         pipelined -- a lower bound, not the cost of a real lookup)
     * So a real dispatch lookup is ~12-15 cyc, which independently confirms the
     * 7.3 cyc/lookup harness figure and the 12-18 cyc bracket. This matters for
     * sizing chaining: the round-trip model was refuted for the SLOW-CALL term
     * (19.7 predicted vs 6.25 measured by the OCERZ_NO_INLINE_CALLRET A/B), but
     * the DISPATCH term it is sized on survives direct measurement. */
    JitBlock *b = cache_lookup(jit, cpu->rip);
    if (!b) {
        pthread_mutex_lock(&jit_lock);
        if (ocerz_perfstat > 0)
            ps_misses++;
        if (ocerz_jitstat > 0) {
            js_misses++;
            /* Periodic report from the miss path: it is the path under study, and
             * we already hold jit_lock so the fail table is stable. Time-gated
             * (10s summary / 60s with the fail table) so the reporting itself --
             * especially the qsort -- cannot distort what it measures. */
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
            b = translate(jit, cpu->rip);
        }
        pthread_mutex_unlock(&jit_lock);
    } else {
        if (ocerz_jitstat > 0)
            js_hits++;
        if (ocerz_perfstat > 0)
            ps_hits++;
    }
    /* !b is a decode failure ONLY now (translate no longer returns NULL for a
     * full arena). Left as EUNSUP deliberately — do NOT poison-cache it. The
     * decode runs under a sigsetjmp recovery, so n==0 can mean the page is
     * merely uncommitted; under commit-on-demand and dyld page-in it may be
     * mapped a millisecond later, and caching that would permanently deny JIT
     * to code that becomes valid. */
    if (!b)
        return OCERZ_EUNSUP;
    if (!b->code)
        return jit_interp_block(vm, cpu, b);
    return b->code(vm, cpu);
}

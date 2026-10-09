/*
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
 * ---- invalidation ----
 * Every guest mmap/mprotect/munmap asks the JIT to drop code in a range.  A
 * global min/max cannot answer that under Wine, where the live set spans PE
 * images at 32-bit addresses and shared-cache dylibs at 0x7ff8_0000_0000, so a
 * region map (one slot per 4 MB, a bitmap of the 64 KB granules in it) answers
 * in constant time; it may say "maybe" after code is gone but never "no" while
 * it is present.  When it says "maybe", the blocks to look at come from a list
 * per 64 KB granule kept beside the granule counts, not from a walk over every
 * live block: Unity's Mono JIT writes code all through R.E.P.O.'s startup with
 * about 390,000 blocks live, and that walk plus rebuilding the live array on
 * every retirement were 30% of the game process; a retired block now leaves
 * the live array by swapping with the last one (OCERZ_INV_SCAN=1 walks every
 * block again).  Only the overlapping blocks are retired - dropping the whole
 * cache per flip made CEF startup a full retranslation storm.  Their code stays
 * allocated on a retired list, so a thread still inside runs to its next exit.
 * A 64 KB region whose translations keep being invalidated (a JS engine
 * W^X-flipping its code space) is run interpreted after a few hits, but not
 * permanently: module-load fixups also retire blocks a few times and then never
 * again, and a permanent blacklist left the hottest DLL code interpreting
 * forever, so a region quiet for CHURN_QUIET_NS is re-probed.
 *
 * Retired code is never reused in place, so code that keeps being regenerated
 * fills the arena: R.E.P.O.'s Mono JIT retranslates its way through the whole
 * 1 GB within two minutes, after which every new block used to run interpreted.
 * The game also crashed in Mono's metadata code in every run that filled the
 * arena while still loading, and a 512 KB arena reproduces the same kind of
 * damage in tests/dynamic/dlopen_cryptex.c whenever a translation overflows and
 * is followed by more translating; why an overflowing translation leaves the
 * translator in that state is not understood yet, so the arena is no longer
 * allowed to overflow.  When less than an eighth of it, at most 8 MB, is left,
 * the next translation miss flushes it instead: every block is retired, every
 * table that points into the arena (chains, return-address cells and slots,
 * site caches, veneer pools, the dispatch stubs, the code index) is emptied,
 * and translation starts again at the front.  The old code is neither unchained
 * nor pushed through the instruction cache, since nothing runs it again;
 * doing both over a full arena held every thread for a third of a second.  The
 * same flush runs when the return-address slots run out, which retranslation
 * also does, because only a flush gives them back.  No thread may still be
 * running translated code when that happens.  Each thread counts the
 * translated frames it is in and how many of them are parked in a syscall made
 * through the slow path, ocerz_jit_exec_one; any other slow-path call returns
 * soon enough to leave by a stop site, and parking every call cost R.E.P.O.'s
 * worker threads a tenth to a quarter of their busy time in atomics and
 * thread-local lookups.  The flusher bumps a generation, patches every stop
 * site so a running thread leaves at its next block edge (one cache line
 * synchronised per patched word), and waits until every thread's two counts are
 * equal; a thread that reaches the dispatcher meanwhile runs its next
 * instruction in the interpreter rather than waiting, so a collector that has
 * frozen a thread can still get round to resuming it.  A parked call that
 * returns into a newer generation goes back to its run loop rather than into
 * the arena: the slow path has already written the guest state to the cpu, so
 * that is the exit the block itself would have taken, and the call's result is
 * handed over with it.  A thread that cannot leave within 500 ms (one stopped
 * by thread_suspend while it spins, say) makes the flush give up and restore
 * the stop-site words it changed, and the next attempt waits two seconds; a
 * thread that is itself inside translated code never flushes.
 * OCERZ_NO_JIT_FLUSH=1 keeps the old behaviour.
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
 *
 * b, and bl: a chained direct call is a bl into its callee
 *
 * An entry's rip word carries its column's retire generation above the address
 *    (rip ^ gen), so retiring a block bumps its column's vm->psc_gen instead of
 *    searching every table: a canonical target only matches an entry filled in
 *    its column's current generation.  (A non-canonical one, which would fault on
 *    x86, could meet an older generation's entry.)
 *
 * A PSC entry for a block sits in the column its own rip selects, bits 2-6,
 *    because the indirect tail stores the rip it looked up beside that block's
 *    body.  Retiring bumps those columns' generations after the blocks have left
 *    the hash table, so a miss that refills from the table never tags a retired
 *    body with a current generation.  A generation that wraps clears its column.
 */
#include "ocerz/jit_internal.h"

static size_t jit_code_bytes(void);
static inline uint64_t blk_insn_rip(const JitBlock *b, int i);
static inline unsigned blk_insn_len(const JitBlock *b, int i);
static inline unsigned blk_insn_op(const JitBlock *b, int i);
static void psc_clear_all(void);
int ocerz_jit_lock_held_self(void);
const int *ocerz_jit_lock_depth_ptr(void);
struct OcerzCPU *ocerz_jit_lock_owner_cpu(void);
static void jl_recursive_warn(const char *where);
static uint64_t jl_tid(void);
static void jl_dump(const char *tag, uint64_t wait_ns);
static void xlatpage_note(uint64_t rip);
static void jl_lock_step(uint64_t rip);
static void jl_unlock_step(void);
static void jit_thr_release(void *p);
static void jit_thr_key_init(void);
static JitThr *jit_thr(void);
uint64_t ocerz_jit_thread_mark(void);
void ocerz_jit_thread_restore(uint64_t mark);
static inline unsigned invmap_slot(uint64_t tag);
static inline uint64_t invmap_mask(uint64_t region, uint64_t lo, uint64_t hi);
static void invmap_add(OcerzJit *jit, uint64_t lo, uint64_t hi);
static void invmap_clear_range(OcerzJit *jit, uint64_t lo, uint64_t hi);
static int invmap_may_hold(const OcerzJit *jit, uint64_t lo, uint64_t hi);
static void gran4_bump(uint64_t row, int d);
static int gran4_count(uint64_t row);
static void gran_bump(uint64_t rip, int d);
static int gran_fine_any(uint64_t p0, uint64_t p1);
static int gran_any(uint64_t lo, uint64_t hi);
static int gblk_slot(uint64_t page, int create);
static void gblk_note(uint64_t page, JitBlock *b, int d);
static void gran_clear_all(void);
static void shrink_edges(JitBlock *b);
static void jit_table_full(const char *what);
static int js_cmp(const void *a, const void *b);
static void js_report(OcerzJit *jit, const char *tag, int with_ftab);
static void chain_batch_begin(void);
static void chain_batch_end(void);
static void chain_cond_short(uint32_t *cond_site, void *dst);
static void chaincheck(const char *what, const void *dst);
static uint32_t *veneer_make(OcerzJit *jit, const uint32_t *site, const void *dst, int batching);
static void chain_activate(uint32_t *patch_b, void *dst);
static void pred_add(JitBlock *target, JitBlock *src, int e);
static void *body_entry_for(const JitBlock *t, uint64_t src_sig);
static void pending_drain(uint64_t key, JitBlock *target);
static int fault_recipe_native_mov(const X86Insn *insn);
static int fault_recipe_add_shape(const X86Insn *insn);
static int fault_recipe_logic_shape(const X86Insn *insn);
static int fault_recipe_matching_inc(const X86Insn *add, const X86Insn *inc);
int ocerz_jit_pc_in_arena(const struct OcerzVM *vm, const void *host_pc);
static const JitBlock *fault_block(const OcerzJit *jit, const uint32_t *pc);
int ocerz_jit_guest_gprs_at(const struct OcerzVM *vm, const void *host_pc,
                            const uint64_t *host_x, const OcerzCPU *cpu, uint64_t out[16]);
void ocerz_jit_fault_recover_regs(const struct OcerzVM *vm, const void *host_pc,
                                  const uint64_t *host_x, OcerzCPU *cpu);
void ocerz_jit_fault_recover_xmm(const struct OcerzVM *vm, const void *host_pc,
                                 const void *host_v, OcerzCPU *cpu);
static int fault_insn_index(const JitBlock *b, const uint32_t *pc);
static int fault_recipe_rhs(const OcerzCPU *cpu, const X86Operand *op,
                            int size, uint64_t *out);
void ocerz_jit_fault_recover_flags(const struct OcerzVM *vm,
                                   const void *host_pc, OcerzCPU *cpu);
static void retire_fault_blocks(struct OcerzVM *vm, OcerzJit *jit, uint64_t block_rip,
                                uint64_t fault_rip, int mode32);
int ocerz_jit_note_commpage_fault(struct OcerzVM *vm, const void *host_pc, uint64_t fault_rip);
int ocerz_jit_note_align_fault(struct OcerzVM *vm, const void *host_pc, uint64_t fault_rip);
int ocerz_jit_hotpatch_align(struct OcerzVM *vm, const void *host_pc);
int ocerz_jit_fault_pair(const void *host_pc);
int ocerz_jit_fault_rip(const struct OcerzVM *vm, const void *host_pc, uint64_t *out_rip);
int ocerz_jit_fault_info(const struct OcerzVM *vm, const void *host_pc,
                         OcerzJitFaultInfo *out);
int ocerz_jit_code_range(struct OcerzVM *vm, const uint32_t **lo, const uint32_t **hi);
int ocerz_jit_owner_pid(struct OcerzVM *vm);
void ocerz_jit_forget(struct OcerzVM *vm);
OcerzJit *ocerz_jit_create(struct OcerzVM *vm);
static void ps_report_atexit(void);
static void block_destroy(JitBlock *b);
static void block_list_destroy(JitBlock *b);
static void retired_list_destroy(JitBlock *b);
static void code_index_destroy(JitCodeIndex *index);
static void pending_clear(void);
void ocerz_jit_destroy(OcerzJit *jit);
uint64_t ocerz_jit_blocks(const OcerzJit *jit);
void ocerz_jit_prof_stats(const struct OcerzVM *vm, uint64_t *translated, uint64_t *live,
                          uint64_t *retires, uint64_t *flips);
static void stopcheck(const JitBlock *b, const uint32_t *site, uint32_t insn, const char *what);
static int force_stop_sites_writable(OcerzJit *jit);
static void invalidate_all_locked(OcerzJit *jit);
void ocerz_jit_invalidate_all(struct OcerzVM *vm);
static int ranges_overlap(uint64_t a, uint64_t alen,
                          uint64_t b, uint64_t blen);
static void invmap_check_reject(const OcerzJit *jit, uint64_t addr, uint64_t len);
static void invsrc_note(uint64_t page, int is_retire);
static void churn_bump(uint64_t rip);
static int ptr_in_block_code(const JitBlock *b, const uint32_t *p);
static int ptr_in_hits(JitBlock *const *hits, size_t n_hits, const uint32_t *p);
static int hit_code_cmp(const void *pa, const void *pb);
static void retire_unlink_hits(OcerzJit *jit, JitBlock **hits, size_t n_hits);
static int ras_entry_in_hits(const void *entry, void *arg);
static void retire_hit_blocks_locked(struct OcerzVM *vm, OcerzJit *jit, JitBlock **hits, size_t n_hits);
void ocerz_jit_invalidate_range(struct OcerzVM *vm, uint64_t addr, uint64_t len);
void ocerz_jit_request_stop(struct OcerzVM *vm);
void ocerz_jit_require_ordered(struct OcerzVM *vm);
void ocerz_jit_prefork(void);
void ocerz_jit_postfork(void);
void ocerz_jit_postfork_child(void);
static void *trip_report(void *arg);
static void trip_note(uint64_t rip);
static void code_index_reset_locked(OcerzJit *jit);
static void jit_arena_reset_locked(OcerzJit *jit);
static void stop_sites_sync(const StopUndo *u, size_t n);
static StopUndo *stop_sites_force_undoable(OcerzJit *jit, size_t *n_out);
static void stop_sites_undo(OcerzJit *jit, StopUndo *u, size_t n);
static int jit_space_low(const OcerzJit *jit);
static int jit_flush(struct OcerzVM *vm, OcerzJit *jit);
int ocerz_jit_step(struct OcerzVM *vm, OcerzCPU *cpu);

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

static inline uint64_t blk_insn_rip(const JitBlock *b, int i) { return b->insns ? b->insns[i].rip : b->iref[i].rip; }

static inline unsigned blk_insn_len(const JitBlock *b, int i) { return b->insns ? b->insns[i].len : b->iref[i].len; }

static inline unsigned blk_insn_op(const JitBlock *b, int i) { return b->insns ? b->insns[i].op : b->iref[i].op; }

int ocerz_perfstat = -1;

static int g_no_fault_recipes;

static JitPscEnt *g_psc_pool;

static size_t g_psc_cap;

static size_t g_psc_used;

static JitPscEnt **g_psc_tables;

static size_t g_cap_psc_tables;

static size_t g_n_psc_tables;

JitPscEnt *psc_alloc(void)
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

void ***g_ras_cells;

size_t g_cap_ras_cells;

size_t g_n_ras_cells;

static int g_churn_suppress;

pthread_mutex_t jit_lock = PTHREAD_MUTEX_INITIALIZER;

int g_jl_log = -1;

static unsigned long long g_jl_acq;

static unsigned long long g_jl_waits;

static unsigned long long g_jl_xlat_null;

static uint64_t g_jl_rip;

uint64_t g_jl_owner;

uint64_t g_jl_since;

int g_jl_phase;

__thread int jl_held;

static OcerzCPU *volatile g_jl_owner_cpu;

int ocerz_jit_lock_held_self(void)
{
    return jl_held;
}

const int *ocerz_jit_lock_depth_ptr(void)
{
    return &ocerz_critical_depth;
}

struct OcerzCPU *ocerz_jit_lock_owner_cpu(void)
{
    return (struct OcerzCPU *)g_jl_owner_cpu;
}

static void jl_recursive_warn(const char *where)
{
    static int once;
    if (__atomic_fetch_add(&once, 1, __ATOMIC_RELAXED) > 4)
        return;
    void *bt[24];
    int nb = backtrace(bt, 24);
    fprintf(stderr, "ocerz: JITLOCK-RECURSIVE[%d] at %s tid=%llu depth=%d phase=%d rip=%#llx\n",
            (int)getpid(), where, (unsigned long long)jl_tid(), jl_held,
            __atomic_load_n(&g_jl_phase, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&g_jl_rip, __ATOMIC_RELAXED));
    backtrace_symbols_fd(bt, nb, 2);
}

static uint64_t jl_tid(void)
{
    uint64_t t = 0;
    pthread_threadid_np(NULL, &t);
    return t;
}

static void jl_dump(const char *tag, uint64_t wait_ns)
{
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint64_t since = __atomic_load_n(&g_jl_since, __ATOMIC_RELAXED);
    const uint32_t *w = (const uint32_t *)(const void *)&jit_lock;
    fprintf(stderr,
            "ocerz: JITLOCK-%s[%d] tid=%llu wait_ms=%.1f owner=%llu held_ms=%.1f phase=%d rip=%#llx acq=%llu waits=%llu xlat_null=%llu mtx=%08x %08x %08x %08x %08x %08x\n",
            tag, (int)getpid(), (unsigned long long)jl_tid(),
            (double)wait_ns / 1e6,
            (unsigned long long)__atomic_load_n(&g_jl_owner, __ATOMIC_RELAXED),
            since ? (double)(now - since) / 1e6 : 0.0,
            __atomic_load_n(&g_jl_phase, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&g_jl_rip, __ATOMIC_RELAXED),
            __atomic_load_n(&g_jl_acq, __ATOMIC_RELAXED),
            __atomic_load_n(&g_jl_waits, __ATOMIC_RELAXED),
            __atomic_load_n(&g_jl_xlat_null, __ATOMIC_RELAXED),
            w[0], w[1], w[2], w[3], w[4], w[5]);
    OcerzCPU *oc = (OcerzCPU *)g_jl_owner_cpu;
    if (oc && oc->host_kport) {
        thread_basic_info_data_t bi;
        mach_msg_type_number_t bn = THREAD_BASIC_INFO_COUNT;
        if (thread_info((thread_act_t)oc->host_kport, THREAD_BASIC_INFO,
                        (thread_info_t)&bi, &bn) == KERN_SUCCESS)
            fprintf(stderr,
                    "ocerz: JITLOCK-OWNER-STATE[%d] run_state=%d(%s) suspend_count=%d flags=%#x cpu_usage=%d sleep_time=%d\n",
                    (int)getpid(), bi.run_state,
                    bi.run_state == TH_STATE_RUNNING ? "RUNNING" :
                    bi.run_state == TH_STATE_STOPPED ? "STOPPED" :
                    bi.run_state == TH_STATE_WAITING ? "WAITING" :
                    bi.run_state == TH_STATE_UNINTERRUPTIBLE ? "UNINTERRUPTIBLE" :
                    bi.run_state == TH_STATE_HALTED ? "HALTED" : "?",
                    bi.suspend_count, bi.flags, bi.cpu_usage, bi.sleep_time);
        else
            fprintf(stderr, "ocerz: JITLOCK-OWNER-STATE[%d] thread_info failed kport=%#x\n",
                    (int)getpid(), (unsigned)oc->host_kport);
    }
}

void jl_acquire(int site)
{
    ocerz_critical_depth++;
    if (g_jl_log < 0)
        g_jl_log = getenv("OCERZ_JITLOCKLOG") ? 1 : 0;
    if (jl_held)
        jl_recursive_warn("jl_acquire");
    if (pthread_mutex_trylock(&jit_lock) != 0) {
        uint64_t t0 = g_jl_log > 0 ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
        if (g_jl_log > 0)
            __atomic_add_fetch(&g_jl_waits, 1, __ATOMIC_RELAXED);
        for (unsigned long long s = 0;; s++) {
            if (s < 64) {
                sched_yield();
            } else {
                struct timespec ts;
                ts.tv_sec = 0;
                ts.tv_nsec = s < 512 ? 200000 : 2000000;
                nanosleep(&ts, NULL);
            }
            if (pthread_mutex_trylock(&jit_lock) == 0)
                break;
            if (g_jl_log > 0) {
                uint64_t w = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
                if (w > 3000000000ull && (s % 2000) == 0)
                    jl_dump("WAIT", w);
            }
        }
    }
    if (g_jl_log > 0) {
        __atomic_add_fetch(&g_jl_acq, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&g_jl_owner, jl_tid(), __ATOMIC_RELAXED);
        __atomic_store_n(&g_jl_since, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), __ATOMIC_RELAXED);
        __atomic_store_n(&g_jl_rip, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_jl_phase, site, __ATOMIC_RELAXED);
    }
    jl_held++;
}

static int g_xlp_log = -1;

static uint64_t g_xlp[XLP_SIZE];

static void xlatpage_note(uint64_t rip)
{
    if (g_xlp_log < 0)
        g_xlp_log = getenv("OCERZ_XLATPAGES") ? 1 : 0;
    if (g_xlp_log <= 0)
        return;
    uint64_t page = rip & ~0xfffull;
    if (!page)
        return;
    unsigned i = (unsigned)((page * 0x9E3779B97F4A7C15ull) >> 47) & (XLP_SIZE - 1);
    for (unsigned n = 0; n < 8; n++, i = (i + 1) & (XLP_SIZE - 1)) {
        if (g_xlp[i] == page)
            return;
        if (g_xlp[i] == 0) {
            g_xlp[i] = page;
            fprintf(stderr, "ocerz: XLATPAGE[%d] %#llx\n", (int)getpid(),
                    (unsigned long long)page);
            return;
        }
    }
}

static void jl_lock_step(uint64_t rip)
{
    ocerz_critical_depth++;
    if (g_jl_log < 0)
        g_jl_log = getenv("OCERZ_JITLOCKLOG") ? 1 : 0;
    if (jl_held)
        jl_recursive_warn("jl_lock_step");
    if (pthread_mutex_trylock(&jit_lock) != 0) {
        uint64_t t0 = g_jl_log > 0 ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
        if (g_jl_log > 0)
            __atomic_add_fetch(&g_jl_waits, 1, __ATOMIC_RELAXED);
        for (unsigned long long s = 0;; s++) {
            if (s < 64) {
                sched_yield();
            } else {
                struct timespec ts;
                ts.tv_sec = 0;
                ts.tv_nsec = s < 512 ? 200000 : 2000000;
                nanosleep(&ts, NULL);
            }
            if (pthread_mutex_trylock(&jit_lock) == 0)
                break;
            if (g_jl_log > 0) {
                uint64_t w = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
                if (w > 3000000000ull && (s % 2000) == 0)
                    jl_dump("WAIT", w);
            }
        }
    }
    if (g_jl_log > 0) {
        __atomic_add_fetch(&g_jl_acq, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&g_jl_owner, jl_tid(), __ATOMIC_RELAXED);
        __atomic_store_n(&g_jl_since, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), __ATOMIC_RELAXED);
        __atomic_store_n(&g_jl_rip, rip, __ATOMIC_RELAXED);
        __atomic_store_n(&g_jl_phase, 1, __ATOMIC_RELAXED);
    }
    jl_held++;
}

static void jl_unlock_step(void)
{
    if (g_jl_log > 0)
        __atomic_store_n(&g_jl_phase, 3, __ATOMIC_RELAXED);
    jl_held--;
    pthread_mutex_unlock(&jit_lock);
    ocerz_critical_depth--;
    if (g_jl_log > 0) {
        __atomic_store_n(&g_jl_phase, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_jl_owner, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_jl_since, 0, __ATOMIC_RELAXED);
    }
}

unsigned long long ps_align_patches;

uint64_t ocerz_jit_retire_ns;

_Atomic unsigned long long ps_chain_far;

_Atomic unsigned long long ps_chain_ok;

unsigned long long ps_ras_noslot;

unsigned long long ps_chain_veneer;

static JitThr *g_jit_thr;

static __thread JitThr *t_jit_thr;

static pthread_key_t g_jit_thr_key;

static pthread_once_t g_jit_thr_once = PTHREAD_ONCE_INIT;

static uint64_t g_flush_gen;

static int g_flush_req;

static uint64_t g_flush_mark = UINT64_MAX;

static uint64_t g_flush_retry_ns;

static void jit_thr_release(void *p)
{
    JitThr *t = (JitThr *)p;
    __atomic_store_n(&t->frames, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&t->parked, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&t->dead, 1, __ATOMIC_SEQ_CST);
}

static void jit_thr_key_init(void) { pthread_key_create(&g_jit_thr_key, jit_thr_release); }

static JitThr *jit_thr(void)
{
    JitThr *t = t_jit_thr;
    if (__builtin_expect(t != NULL, 1))
        return t;
    for (t = __atomic_load_n(&g_jit_thr, __ATOMIC_ACQUIRE); t; t = t->next) {
        int one = 1;
        if (__atomic_compare_exchange_n(&t->dead, &one, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
            break;
    }
    if (!t) {
        t = (JitThr *)calloc(1, sizeof *t);
        if (!t) abort();
        JitThr *h = __atomic_load_n(&g_jit_thr, __ATOMIC_RELAXED);
        do t->next = h;
        while (!__atomic_compare_exchange_n(&g_jit_thr, &h, t, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    }
    pthread_once(&g_jit_thr_once, jit_thr_key_init);
    pthread_setspecific(g_jit_thr_key, t);
    t_jit_thr = t;
    return t;
}

uint64_t ocerz_jit_thread_mark(void)
{
    JitThr *t = jit_thr();
    return (uint64_t)(uint32_t)__atomic_load_n(&t->frames, __ATOMIC_RELAXED) << 32 |
           (uint32_t)__atomic_load_n(&t->parked, __ATOMIC_RELAXED);
}

void ocerz_jit_thread_restore(uint64_t mark)
{
    JitThr *t = jit_thr();
    __atomic_store_n(&t->parked, (int)(uint32_t)mark, __ATOMIC_SEQ_CST);
    __atomic_store_n(&t->frames, (int)(mark >> 32), __ATOMIC_SEQ_CST);
}

__attribute__((noinline))
int jit_exec_one_parked(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn, const void *ret)
{
    if ((insn->op != OCERZ_OP_SYSCALL && insn->op != OCERZ_OP_INT) || !ocerz_jit_pc_in_arena(vm, ret))
        return jit_exec_one(vm, cpu, insn);
    JitThr *t = jit_thr();
    __atomic_add_fetch(&t->parked, 1, __ATOMIC_SEQ_CST);
    uint64_t gen = __atomic_load_n(&g_flush_gen, __ATOMIC_SEQ_CST);
    int r = jit_exec_one(vm, cpu, insn);
    __atomic_sub_fetch(&t->parked, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_flush_req, __ATOMIC_SEQ_CST) ||
        __atomic_load_n(&g_flush_gen, __ATOMIC_SEQ_CST) != gen) {
        ocerz_vm_jit_escape(r);
    }
    return r;
}

JitBlock *cache_lookup(OcerzJit *jit, uint64_t rip, int mode32)
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

static struct { uint64_t page; uint32_t n, cap; JitBlock **v; } g_gblk[GBLK_SLOTS];

static int g_gblk_off = -1;

static int gblk_slot(uint64_t page, int create)
{
    unsigned i = (unsigned)(page * 0x9E3779B97F4A7C15ull >> 48) & (GBLK_SLOTS - 1);
    for (unsigned n = 0; n < 64; n++, i = (i + 1) & (GBLK_SLOTS - 1)) {
        if (g_gblk[i].page == page + 1) return (int)i;
        if (g_gblk[i].page == 0) {
            if (!create) return -1;
            g_gblk[i].page = page + 1;
            return (int)i;
        }
    }
    return create ? -2 : -1;
}

static void gblk_note(uint64_t page, JitBlock *b, int d)
{
    if (g_gblk_off) return;
    int s = gblk_slot(page, d > 0);
    if (s < 0) {
        if (s == -2 || d > 0) g_gblk_off = 1;
        return;
    }
    if (d > 0) {
        if (g_gblk[s].n == g_gblk[s].cap) {
            uint32_t nc = g_gblk[s].cap ? g_gblk[s].cap * 2 : 8;
            JitBlock **nv = (JitBlock **)realloc(g_gblk[s].v, nc * sizeof *nv);
            if (!nv) { g_gblk_off = 1; return; }
            g_gblk[s].v = nv;
            g_gblk[s].cap = nc;
        }
        g_gblk[s].v[g_gblk[s].n++] = b;
        return;
    }
    for (uint32_t j = 0; j < g_gblk[s].n; j++)
        if (g_gblk[s].v[j] == b) {
            g_gblk[s].v[j] = g_gblk[s].v[--g_gblk[s].n];
            return;
        }
}

void gran_block(JitBlock *b, int d)
{
    if (b->n_insns <= 0) return;
    if (g_gblk_off < 0) g_gblk_off = getenv("OCERZ_INV_SCAN") ? 1 : 0;
    uint64_t lo = blk_insn_rip(b, 0), hi = lo + blk_insn_len(b, 0);
    for (int i = 1; i <= b->n_insns; i++) {
        if (i < b->n_insns && blk_insn_rip(b, i) == hi) { hi += blk_insn_len(b, i); continue; }
        for (uint64_t p = lo >> INVMAP_GSHIFT; p <= (hi - 1) >> INVMAP_GSHIFT; p++) {
            gran_bump(p << INVMAP_GSHIFT, d);
            gblk_note(p, b, d);
        }
        if (i < b->n_insns) { lo = blk_insn_rip(b, i); hi = lo + blk_insn_len(b, i); }
    }
}

static void gran_clear_all(void)
{
    memset(g_gran, 0, sizeof g_gran);
    memset(g_gran4, 0, sizeof g_gran4);
    g_gran_degenerate = 0;
    for (unsigned i = 0; i < GBLK_SLOTS; i++)
        free(g_gblk[i].v);
    memset(g_gblk, 0, sizeof g_gblk);
    g_gblk_off = getenv("OCERZ_INV_SCAN") ? 1 : 0;
}

static void shrink_edges(JitBlock *b)
{
    unsigned n = b->n_edges ? b->n_edges : 1;
    if (b->edges && n < JIT_MAX_EDGES) {
        void *p = realloc(b->edges, n * sizeof *b->edges);
        if (p) b->edges = p;
    }
}

void cache_insert(OcerzJit *jit, JitBlock *b)
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
    if (jit->n_live < jit->cap_live) { b->live_idx = jit->n_live; jit->live[jit->n_live++] = b; }
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

static int g_flush_want;

static void jit_table_full(const char *what)
{
    if (!__atomic_exchange_n(&g_flush_want, 1, __ATOMIC_RELAXED)) {
        static int said;
        if (!said++ || getenv("OCERZ_FLUSHLOG"))
            fprintf(stderr, "ocerz: note: JIT %s are used up; the arena is flushed at the next translation\n", what);
    }
}

extern uint64_t ocerz_jit_retire_count;

static _Atomic unsigned long long js_hits;

static _Atomic unsigned long long js_misses;

static _Atomic unsigned long long js_steps;

_Atomic unsigned long long js_xlat_fail;

_Atomic unsigned long long js_xlat_ok;

static _Atomic unsigned long long js_xlat;

_Atomic unsigned long long js_fail_alloc;

_Atomic unsigned long long js_fail_decode0;

_Atomic unsigned long long js_fail_overflow;

_Atomic unsigned long long js_decoded_insns;

static uint64_t js_t0;

static JsFail js_ftab[JS_FTAB];

static unsigned js_ftab_full;

static unsigned js_ftab_used;

void js_note_fail(uint64_t rip, unsigned reason, int nins)
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
    if (__atomic_load_n(&g_flush_req, __ATOMIC_SEQ_CST)) return;
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

void veneer_pool_check(OcerzJit *jit)
{
    if (!jit->veneer_next_mark)
        jit->veneer_next_mark = (uint8_t *)jit->code_base;
    while (jit->veneer_n < 16 && (uint8_t *)jit->code_cur >= jit->veneer_next_mark) {
        if ((size_t)((uint8_t *)jit->code_end - (uint8_t *)jit->code_cur) < VENEER_POOL_BYTES + 65536)
            return;
        jit->veneer_pool[jit->veneer_n] = jit->code_cur;
        jit->veneer_used[jit->veneer_n] = 0;
        jit->veneer_n++;
        jit->code_cur = (uint32_t *)((uint8_t *)jit->code_cur + VENEER_POOL_BYTES);
        jit->veneer_next_mark += VENEER_WINDOW_BYTES;
    }
}

static uint32_t *veneer_make(OcerzJit *jit, const uint32_t *site, const void *dst, int batching)
{
    int best = -1;
    ptrdiff_t best_d = VENEER_REACH;
    for (unsigned i = 0; i < jit->veneer_n; i++) {
        if ((jit->veneer_used[i] + 1) * VENEER_BYTES > VENEER_POOL_BYTES)
            continue;
        ptrdiff_t d = (const uint8_t *)jit->veneer_pool[i] - (const uint8_t *)site;
        if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = (int)i; }
    }
    if (best < 0)
        return NULL;
    uint32_t *v = (uint32_t *)((uint8_t *)jit->veneer_pool[best] + jit->veneer_used[best] * VENEER_BYTES);
    uint64_t target = (uint64_t)(uintptr_t)dst;
    if (!batching) pthread_jit_write_protect_np(0);
    v[0] = 0x58000040u | (uint32_t)JTA;
    v[1] = 0xd61f0000u | ((uint32_t)JTA << 5);
    memcpy(&v[2], &target, 8);
    if (!batching) pthread_jit_write_protect_np(1);
    sys_icache_invalidate(v, VENEER_BYTES);
    jit->veneer_used[best]++;
    return v;
}

static void chain_activate(uint32_t *patch_b, void *dst)
{
    if (!patch_b || !dst)
        return;
    chaincheck("chain_activate", dst);
    if (g_xlat_jit && g_xlat_jit->stop_requested)
        return;
    if (__atomic_load_n(&g_flush_req, __ATOMIC_SEQ_CST))
        return;
    int ok;
    int veneered = 0;
    if (g_chain_batching) {
        ok = a64_try_patch_b(patch_b, (uint32_t *)dst);
        if (!ok && g_xlat_jit) {
            uint32_t *v = veneer_make(g_xlat_jit, patch_b, dst, 1);
            if (v) { ok = a64_try_patch_b(patch_b, v); veneered = ok; }
        }
        if (g_chain_npatched < CHAIN_BATCH_MAX)
            g_chain_patched[g_chain_npatched++] = patch_b;
        else
            sys_icache_invalidate(patch_b, 4);
    } else {
        pthread_jit_write_protect_np(0);
        ok = a64_try_patch_b(patch_b, (uint32_t *)dst);
        pthread_jit_write_protect_np(1);
        if (!ok && g_xlat_jit) {
            uint32_t *v = veneer_make(g_xlat_jit, patch_b, dst, 0);
            if (v) {
                pthread_jit_write_protect_np(0);
                ok = a64_try_patch_b(patch_b, v);
                pthread_jit_write_protect_np(1);
                veneered = ok;
            }
        }
        sys_icache_invalidate(patch_b, 4);
    }
    if (ocerz_perfstat > 0) {
        if (veneered)
            ps_chain_veneer++;
        else if (ok)
            ps_chain_ok++;
        else
            ps_chain_far++;
    }
}

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

void pending_add(uint64_t target_key, uint32_t *patch_b, uint8_t kind,
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

void **g_ras_slots;

unsigned g_ras_slot_n;

void **ras_slot_alloc(void)
{
    if (!g_ras_slots) {
        void *p = mmap(NULL, (size_t)RAS_SLOT_CAP * sizeof(void *),
                       PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED)
            return NULL;
        g_ras_slots = (void **)p;
    }
    if (g_ras_slot_n >= RAS_SLOT_CAP) {
        ps_ras_noslot++;
        jit_table_full("return-address slots");
        return NULL;
    }
    void **s = &g_ras_slots[g_ras_slot_n++];
    *s = NULL;
    return s;
}

void pending_add_ras(uint64_t target_key, void **ras_slot)
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

int build_fault_flag_recipes(const X86Insn *insns, int n,
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

int code_index_append_locked(OcerzJit *jit, JitBlock *block)
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

void compact_block(JitBlock *blk)
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

void blk_chain_install(OcerzJit *jit, JitBlock *blk)
{
    if (g_no_chain)
        return;
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
    uint32_t off = (uint32_t)((const uint32_t *)host_pc - (const uint32_t *)b->code);
    const struct JitLaneRec *rec = NULL;
    for (int k = 0; k < b->n_lanerec; k++) {
        if (b->lanerec[k].off > off) break;
        rec = &b->lanerec[k];
    }
    if (!rec)
        return;
    for (unsigned r = 0; r < 16; r++) {
        if (rec->l0[r] != 0xff && ((rec->dirty >> r) & 1))
            memcpy(&cpu->xmm[r], v + (4 + (rec->l0[r] & 15)) * 16, (rec->l0[r] & 0x10) ? 8 : 4);
        if (rec->yc[r] != 0xff)
            memcpy(&cpu->ymmh[r], v + (4 + rec->yc[r]) * 16, 16);
    }
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

static void retire_fault_blocks(struct OcerzVM *vm, OcerzJit *jit, uint64_t block_rip,
                                uint64_t fault_rip, int mode32)
{
    jl_acquire(__LINE__);
    JitBlock *b = cache_lookup(jit, block_rip, mode32);
    if (b && b->code)
        flip_retire_locked(vm, jit, b);
    if (fault_rip != block_rip) {
        JitBlock *f = cache_lookup(jit, fault_rip, mode32);
        if (f && f->code)
            flip_retire_locked(vm, jit, f);
    }
    jl_release();
    ocerz_vm_purge_jit_ras(vm);
}

int ocerz_jit_note_commpage_fault(struct OcerzVM *vm, const void *host_pc, uint64_t fault_rip)
{
    OcerzJit *jit = vm ? vm->jit : NULL;
    const uint32_t *pc = (const uint32_t *)host_pc;
    const JitBlock *b = fault_block(jit, pc);
    if (!b) return 0;
    uint64_t block_rip = blk_rip(b);
    int fresh = !cp_marked(b->key);
    cp_mark(b->key);
    cp_mark(jit_key(fault_rip, blk_mode32(b)));
    if (ENV_ON("OCERZ_CP_NOINVAL")) return 1;
    if (!fresh && cache_lookup(jit, block_rip, blk_mode32(b)) != b && !ENV_ON("OCERZ_REFAULT_INVAL"))
        return 1;
    if (fresh && !ENV_ON("OCERZ_FAULT_INV_RANGE")) {
        retire_fault_blocks(vm, jit, block_rip, fault_rip, blk_mode32(b));
        return 1;
    }
    int prev = g_churn_suppress;
    if (fresh) g_churn_suppress = 1;
    ocerz_jit_invalidate_range(vm, block_rip, 1);
    if (fault_rip != block_rip) ocerz_jit_invalidate_range(vm, fault_rip, 1);
    g_churn_suppress = prev;
    return 1;
}

int ocerz_jit_note_align_fault(struct OcerzVM *vm, const void *host_pc, uint64_t fault_rip)
{
    OcerzJit *jit = vm ? vm->jit : NULL;
    const uint32_t *pc = (const uint32_t *)host_pc;
    const JitBlock *b = fault_block(jit, pc);
    if (!b || jit->plain_mem) return 0;
    uint64_t block_rip = blk_rip(b);
    uint64_t ik = jit_key(fault_rip, blk_mode32(b));
    int fresh = !al_marked(ik);
    if (fresh) {
        al_mark(ik);
    } else {
        fresh = !al_marked(b->key | AL_BLK_TAG);
        al_mark(b->key | AL_BLK_TAG);
    }
    if (!fresh && cache_lookup(jit, block_rip, blk_mode32(b)) != b && !ENV_ON("OCERZ_REFAULT_INVAL"))
        return 1;
    if (fresh && !ENV_ON("OCERZ_FAULT_INV_RANGE")) {
        retire_fault_blocks(vm, jit, block_rip, fault_rip, blk_mode32(b));
        return 1;
    }
    int prev = g_churn_suppress;
    if (fresh) g_churn_suppress = 1;
    ocerz_jit_invalidate_range(vm, block_rip, 1);
    if (fault_rip != block_rip) ocerz_jit_invalidate_range(vm, fault_rip, 1);
    g_churn_suppress = prev;
    return 1;
}

int ocerz_jit_hotpatch_align(struct OcerzVM *vm, const void *host_pc)
{
    OcerzJit *jit = vm ? vm->jit : NULL;
    uint32_t *site = (uint32_t *)(uintptr_t)host_pc;
    if (!jit || !ocerz_jit_pc_in_arena(vm, host_pc)) return 0;
    uint32_t w = *site;
    if ((w & 0xfc000000u) == 0x14000000u) return 2;
    uint32_t opc = (w >> 22) & 3;
    int is_acc = (w & 0x3f200c00u) == 0x19000000u;
    int is_lds = is_acc && opc >= 2;
    int lds_sf = opc == 2;
    int is_ld = is_acc && opc != 0;
    int is_st = is_acc && opc == 0;
    if (!is_ld && !is_st) return 0;
    int size = 1 << (w >> 30);
    if (size < 2 || (is_lds && size == 8)) return 0;
    int32_t imm9 = (int32_t)((w >> 12) & 0x1ff); if (imm9 & 0x100) imm9 -= 0x200;
    int rn = (int)((w >> 5) & 31), rt = (int)(w & 31);
    if (rn == 31 || (rt == 31 && !is_st)) return 0;
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

    jl_acquire(__LINE__);
    int rc = 0;
    if ((size_t)(jit->code_end - jit->code_cur) > 128) {
        pthread_jit_write_protect_np(0);
        veneer_pool_check(jit);
        int pool = -1;
        uint32_t *start = jit->code_cur, *lim = jit->code_end;
        ptrdiff_t reach = (uint8_t *)start - (uint8_t *)site;
        if (reach > VENEER_REACH || reach < -VENEER_REACH) {
            ptrdiff_t best_d = VENEER_REACH;
            for (unsigned i = 0; i < jit->veneer_n; i++) {
                if (jit->veneer_used[i] * VENEER_BYTES + ALIGN_ARM_BYTES > VENEER_POOL_BYTES)
                    continue;
                ptrdiff_t d = (const uint8_t *)jit->veneer_pool[i] - (const uint8_t *)site;
                if (d < 0) d = -d;
                if (d < best_d) { best_d = d; pool = (int)i; }
            }
            if (pool >= 0) {
                start = (uint32_t *)((uint8_t *)jit->veneer_pool[pool] +
                                     jit->veneer_used[pool] * VENEER_BYTES);
                lim = start + ALIGN_ARM_BYTES / 4;
            }
        }
        A64Buf b = { start, start, lim, 0, 0 };
        uint32_t *arm = b.p;
        a64_stp_pre(&b, ta, s1, 31, -16);
        if (imm9 > 0)      a64_add_imm(&b, 1, ta, rn, (uint32_t)imm9);
        else if (imm9 < 0) a64_sub_imm(&b, 1, ta, rn, (uint32_t)-imm9);
        else               a64_mov_reg(&b, 1, ta, rn);
        uint32_t *bne;
        if (pair) {
            (void)a64_try_and_imm(&b, 1, s1, ta, 7);
            bne = a64_label(&b); a64_cbnz(&b, 1, s1, 0);
        } else {
            a64_add_imm(&b, 1, s1, ta, (uint32_t)(size - 1));
            a64_eor_reg(&b, 1, s1, s1, ta, 0);
            bne = a64_label(&b); a64_tbnz(&b, s1, 4, 0);
        }
        if (is_lds) a64_ldapurs(&b, size, lds_sf, rt, ta, 0);
        else if (is_ld) a64_ldapur(&b, size, rt, ta, 0);
        else { a64_stlur(&b, size, rt, ta, 0); if (pair) a64_stlur(&b, 8, JTU, ta, 8); }
        a64_ldp_post(&b, ta, s1, 31, 16);
        uint32_t *back1 = a64_label(&b); a64_b(&b, 0);
        if (pair) a64_patch_cbz(bne, a64_label(&b));
        else      a64_patch_tbz(bne, a64_label(&b));
        if (is_ld) {
            if (!is_lds)        a64_ldr(&b, size, rt, ta, 0);
            else if (size == 2) a64_ldrsh(&b, lds_sf, rt, ta, 0);
            else                a64_ldrsw(&b, rt, ta, 0);
            a64_dmb_ishld(&b);
        } else if (use_dmb) {
            a64_dmb_ish(&b);
            a64_str(&b, size, rt, ta, 0);
            if (pair) a64_str(&b, 8, JTU, ta, 8);
        } else if (size == 8) {
            uint32_t *tob0 = a64_label(&b); a64_tbnz(&b, ta, 0, 0);
            uint32_t *tob1 = a64_label(&b); a64_tbnz(&b, ta, 1, 0);
            emit_misaligned_pieces_st(&b, 4, 2, rt, ta, 0, s1);
            if (pair) emit_misaligned_pieces_st(&b, 4, 2, JTU, ta, 8, s1);
            uint32_t *tod = a64_label(&b); a64_b(&b, 0);
            a64_patch_tbz(tob0, a64_label(&b));
            a64_patch_tbz(tob1, a64_label(&b));
            emit_misaligned_pieces_st(&b, 1, 8, rt, ta, 0, s1);
            if (pair) emit_misaligned_pieces_st(&b, 1, 8, JTU, ta, 8, s1);
            a64_patch_b(tod, a64_label(&b));
        } else {
            emit_misaligned_pieces_st(&b, 1, size, rt, ta, 0, s1);
        }
        a64_ldp_post(&b, ta, s1, 31, 16);
        uint32_t *back2 = a64_label(&b); a64_b(&b, 0);
        uint32_t *back = site + (pair ? 2 : 1);
        int ok = !b.overflow && a64_try_patch_b(back1, back) && a64_try_patch_b(back2, back);
        if (ok) {
            uint32_t saved = *site;
            *site = 0x14000000u;
            if (a64_try_patch_b(site, arm)) {
                if (pool >= 0)
                    jit->veneer_used[pool] += (unsigned)(((uint8_t *)b.p - (uint8_t *)arm +
                                                          VENEER_BYTES - 1) / VENEER_BYTES);
                else
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
    jl_release();
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

int ocerz_jit_fault_pair(const void *host_pc)
{
    uint32_t w = *(const uint32_t *)host_pc;
    return (w & 0xffc00000u) == 0xad400000u || (w & 0xffc00000u) == 0xad000000u;
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
    size_t leaf_bytes = (size_t)(ocerz_leaf_hi - ocerz_leaf_lo);
    if (leaf_bytes && leaf_bytes + 64 < bytes) {
        pthread_jit_write_protect_np(0);
        memcpy(p, ocerz_leaf_lo, leaf_bytes);
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(p, leaf_bytes);
        jit->leaf_near = (const char *)p;
        jit->code_cur = (uint32_t *)((uint8_t *)p + ((leaf_bytes + 63) & ~(size_t)63));
        __atomic_store_n(&ocerz_leaf_near_hi, (uint64_t)(uintptr_t)p + leaf_bytes, __ATOMIC_RELEASE);
        __atomic_store_n(&ocerz_leaf_near_lo, (uint64_t)(uintptr_t)p, __ATOMIC_RELEASE);
    }
    jit->code_start = jit->code_cur;
    jit->code_end = (uint32_t *)((uint8_t *)p + bytes);
    jit->code_bytes = bytes;
    OCERZ_LOG("JIT code arena %zu MB reserved at [%p,%p)\n",
              bytes >> 20, p, (void *)((uint8_t *)p + bytes));
    return jit;
}

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
    free(b->lanerec);
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

void ocerz_jit_prof_stats(const struct OcerzVM *vm, uint64_t *translated, uint64_t *live,
                          uint64_t *retires, uint64_t *flips)
{
    const OcerzJit *jit = vm ? vm->jit : NULL;
    *translated = jit ? jit->blocks_translated : 0;
    *live = jit ? (uint64_t)jit->n_live : 0;
    *retires = __atomic_load_n(&ocerz_jit_retire_count, __ATOMIC_RELAXED);
    *flips = g_flip_n_retire;
}

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

uint64_t ocerz_jit_retire_count;

uint64_t ocerz_leaf_near_hi;

uint64_t ocerz_leaf_near_lo;

static void invalidate_all_locked(OcerzJit *jit)
{
    int patched = 0;
    __atomic_add_fetch(&ocerz_jit_retire_count, 1, __ATOMIC_RELEASE);

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
}

void ocerz_jit_invalidate_all(struct OcerzVM *vm)
{
    if (!vm)
        return;

    OcerzJit *jit = vm->jit;
    if (jit) {
        jl_acquire(__LINE__);
        invalidate_all_locked(jit);
        jl_release();
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

static struct { uint64_t page; uint32_t hits; uint64_t last_ns; uint64_t refused; } g_churn[CHURN_SLOTS];

void churn_note_refusal(uint64_t rip)
{
    static int blog = -1;
    if (blog < 0) blog = getenv("OCERZ_BLACKLOG") ? 1 : 0;
    if (blog <= 0)
        return;
    static unsigned long long tot;
    if ((++tot & 0xffffu) != 0)
        return;
    fprintf(stderr, "ocerz: BLACKLOG[%d] refusals=%lluk blacklisted_pages=", (int)getpid(),
            tot >> 10);
    unsigned np = 0;
    for (unsigned k = 0; k < CHURN_SLOTS; k++)
        if (g_churn[k].page && g_churn[k].hits >= CHURN_LIMIT) np++;
    fprintf(stderr, "%u top:", np);
    for (int t = 0; t < 5; t++) {
        unsigned best = CHURN_SLOTS; uint64_t bv = 0;
        for (unsigned k = 0; k < CHURN_SLOTS; k++)
            if (g_churn[k].refused > bv) { bv = g_churn[k].refused; best = k; }
        if (best == CHURN_SLOTS) break;
        fprintf(stderr, " %#llx=%lluk", (unsigned long long)(g_churn[best].page << 16),
                (unsigned long long)(bv >> 10));
        g_churn[best].refused = 0;
    }
    fprintf(stderr, "\n");
}

static __thread uint64_t g_inv_caller;

static struct { uint64_t caller, page, bumps, retires; } g_invsrc[256];

static void invsrc_note(uint64_t page, int is_retire)
{
    static int lg = -1;
    if (lg < 0)
        lg = getenv("OCERZ_INVSRC") ? 1 : 0;
    if (lg <= 0)
        return;
    uint64_t c = g_inv_caller;
    unsigned i = (unsigned)((c * 0x9E3779B97F4A7C15ull) >> 56);
    for (unsigned n = 0; n < 256; n++, i = (i + 1) & 255u) {
        if (g_invsrc[i].caller == c || g_invsrc[i].caller == 0) {
            g_invsrc[i].caller = c;
            if (is_retire) {
                g_invsrc[i].retires++;
            } else {
                g_invsrc[i].bumps++;
                g_invsrc[i].page = page << 16;
            }
            break;
        }
    }
    static unsigned long long tot;
    if (is_retire || (++tot & 0x3ffu) != 0)
        return;
    fprintf(stderr, "ocerz: INVSRC[%d] bumps=%llu |", (int)getpid(), tot);
    unsigned char used[256] = { 0 };
    for (int t = 0; t < 6; t++) {
        int best = -1;
        uint64_t bv = 0;
        for (int k = 0; k < 256; k++)
            if (!used[k] && g_invsrc[k].bumps > bv) {
                bv = g_invsrc[k].bumps;
                best = k;
            }
        if (best < 0)
            break;
        used[best] = 1;
        fprintf(stderr, " rel=%lld:b=%llu,r=%llu,pg=%#llx",
                (long long)(g_invsrc[best].caller - (uint64_t)(uintptr_t)&ocerz_jit_step),
                (unsigned long long)g_invsrc[best].bumps,
                (unsigned long long)g_invsrc[best].retires,
                (unsigned long long)g_invsrc[best].page);
    }
    fprintf(stderr, "\n");
}

static void churn_bump(uint64_t rip)
{
    if (g_churn_suppress) return;
    invsrc_note(rip >> 16, 0);
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

int churn_blacklisted(uint64_t rip)
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
                g_churn[i].refused = 0;
                if (getenv("OCERZ_CHURNLOG"))
                    fprintf(stderr, "ocerz: CHURN[%d] reprieve page=%#llx\n",
                            (int)getpid(), (unsigned long long)(page << 16));
                return 0;
            }
            if (++g_churn[i].refused > CHURN_REFUSE_MAX) {
                g_churn[i].hits = CHURN_LIMIT - 1;
                g_churn[i].last_ns = now;
                g_churn[i].refused = 0;
                if (getenv("OCERZ_CHURNLOG"))
                    fprintf(stderr, "ocerz: CHURN[%d] cost-reprieve page=%#llx\n",
                            (int)getpid(), (unsigned long long)(page << 16));
                return 0;
            }
            return 1;
        }
        if (g_churn[i].page == 0) return 0;
    }
    return 0;
}

uint32_t *branch_word_target(uint32_t *site, uint32_t w)
{
    int64_t off;

    if ((w & 0x7C000000u) == 0x14000000u) {
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

static void retire_unlink_hits(OcerzJit *jit, JitBlock **hits, size_t n_hits)
{
    for (size_t m = 0; m < n_hits; m++) {
        JitBlock *b = hits[m];
        size_t idx = b->live_idx;
        if (idx >= jit->n_live || jit->live[idx] != b) {
            for (idx = 0; idx < jit->n_live && jit->live[idx] != b; idx++)
                ;
            if (idx == jit->n_live) continue;
        }
        jit->live[idx] = jit->live[jit->n_live - 1];
        jit->live[idx]->live_idx = idx;
        jit->n_live--;
        unsigned h = hash_key(b->key);
        JitBlock **pp = &jit->buckets[h];
        while (*pp && *pp != b) pp = &(*pp)->hnext;
        if (*pp == b)
            __atomic_store_n(pp, b->hnext, __ATOMIC_RELEASE);
        gran_block(b, -1);
        tc_noload_add(b->key);
        b->retired_next = jit->retired;
        jit->retired = b;
    }
}

static unsigned g_retire_sweep;

static int ras_entry_in_hits(const void *entry, void *arg)
{
    const struct { JitBlock **v; size_t n; } *set = arg;
    const uint32_t *p = (const uint32_t *)((uintptr_t)entry & ~(uintptr_t)3);
    size_t lo = 0, hi = set->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if ((const uint32_t *)set->v[mid]->code <= p) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0)
        return 0;
    const JitBlock *b = set->v[lo - 1];
    return b->code && p < (const uint32_t *)b->code + b->code_words;
}

void psc_retire_cols(struct OcerzVM *vm, uint32_t cols)
{
    for (unsigned k = 0; k < PSC_N; k++) {
        if (!(cols >> k & 1))
            continue;
        uint64_t g = __atomic_load_n(&vm->psc_gen[k], __ATOMIC_RELAXED) + (1ull << PSC_GEN_SHIFT);
        if (g == 0)
            for (size_t i = 0; i < g_n_psc_tables; i++) {
                __atomic_store_n(&g_psc_tables[i][k].rip, PSC_EMPTY_RIP, __ATOMIC_RELEASE);
                __atomic_store_n(&g_psc_tables[i][k].body, (void *)NULL, __ATOMIC_RELEASE);
            }
        __atomic_store_n(&vm->psc_gen[k], g, __ATOMIC_RELEASE);
    }
}

static void retire_hit_blocks_locked(struct OcerzVM *vm, OcerzJit *jit, JitBlock **hits, size_t n_hits)
{
    __atomic_add_fetch(&ocerz_jit_retire_count, 1, __ATOMIC_RELEASE);
    invsrc_note(0, 1);
    int any_code = 0;
    for (size_t m = 0; m < n_hits; m++)
        if (hits[m]->code) {
            any_code = 1;
            churn_bump(blk_insn_rip(hits[m], 0));
        }
    if (!any_code) {
        retire_unlink_hits(jit, hits, n_hits);
        return;
    }
    qsort(hits, n_hits, sizeof *hits, hit_code_cmp);
    static int full_scan = -1;
    if (full_scan < 0) full_scan = getenv("OCERZ_RETIRE_SCAN") ? 1 : 0;
    struct { JitBlock **v; size_t n; } set = { hits, n_hits };
    pthread_jit_write_protect_np(0);
    for (size_t i = 0; i < g_n_ras_cells; i++) {
        void *e = __atomic_load_n(g_ras_cells[i], __ATOMIC_RELAXED);
        if (e && ras_entry_in_hits(e, &set))
            __atomic_store_n(g_ras_cells[i], (void *)NULL, __ATOMIC_RELEASE);
    }
    for (size_t m = 0; m < n_hits; m++) {
        JitBlock *b = hits[m];
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
#define UNCHAIN_EDGE(sblk, i) do { \
        uint32_t *at = (sblk)->edges[i].patch_b; \
        uint32_t fallback = (sblk)->edges[i].fallback_insn; \
        if (at && fallback && *at != fallback && \
            ptr_in_hits(hits, n_hits, branch_word_target(at, *at))) { \
            __atomic_store_n(at, fallback, __ATOMIC_RELEASE); \
            sys_icache_invalidate(at, 4); \
        } \
        uint32_t *cs = (sblk)->edges[i].cond_site; \
        if (cs && (sblk)->edges[i].cond_orig && *cs != (sblk)->edges[i].cond_orig && \
            ptr_in_hits(hits, n_hits, branch_word_target(cs, *cs))) { \
            __atomic_store_n(cs, (sblk)->edges[i].cond_orig, __ATOMIC_RELEASE); \
            sys_icache_invalidate(cs, 4); \
        } \
    } while (0)
    if (full_scan) {
        for (size_t k = 0; k < jit->n_live; k++) {
            JitBlock *sblk = jit->live[k];
            if (sblk->inv_hit) continue;
            for (int i = 0; i < sblk->n_edges; i++)
                UNCHAIN_EDGE(sblk, i);
        }
    } else {
        for (size_t m = 0; m < n_hits; m++) {
            JitBlock *t = hits[m];
            for (uint32_t q = 0; q < t->n_preds; q++) {
                JitBlock *sblk = t->preds[q].pb;
                int i = t->preds[q].e;
                if (sblk->inv_hit || i >= sblk->n_edges) continue;
                UNCHAIN_EDGE(sblk, i);
            }
            t->n_preds = 0;
        }
    }
#undef UNCHAIN_EDGE
    pthread_jit_write_protect_np(1);
    retire_unlink_hits(jit, hits, n_hits);
    for (unsigned i = 0; i < g_ras_slot_n; i++) {
        void *e = __atomic_load_n(&g_ras_slots[i], __ATOMIC_RELAXED);
        if (e && ras_entry_in_hits(e, &set))
            __atomic_store_n(&g_ras_slots[i], NULL, __ATOMIC_RELEASE);
    }
    ++g_retire_sweep;
    uint32_t cols = 0;
    for (size_t m = 0; m < n_hits; m++)
        if (hits[m]->code)
            cols |= 1u << psc_col(hits[m]->key);
    psc_retire_cols(vm, cols);
}

void ocerz_jit_invalidate_range(struct OcerzVM *vm, uint64_t addr, uint64_t len)
{
    g_inv_caller = (uint64_t)(uintptr_t)__builtin_return_address(0);
    if (!vm || !len || !vm->jit)
        return;

    OcerzJit *jit = vm->jit;
    int invalidated = 0;
    uint64_t t0 = ocerz_jit_time_xlat ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
    jl_acquire(__LINE__);
    if (!jit->code_hi || !ranges_overlap(addr, len, jit->code_lo,
                                         jit->code_hi - jit->code_lo)) {
        if (ENV_ON("OCERZ_INVMAP_CHECK"))
            invmap_check_reject(jit, addr, len);
        jl_release();
        return;
    }
    if (addr < jit->code_lo) { len -= jit->code_lo - addr; addr = jit->code_lo; }
    if (addr + len > jit->code_hi) len = jit->code_hi - addr;
    uint64_t xlo = addr, xlen = len;
    {
        uint64_t g = 1ull << INVMAP_GSHIFT;
        uint64_t alo = addr & ~(g - 1);
        uint64_t ahi = (addr + len + g - 1) & ~(g - 1);
        addr = alo;
        len = ahi - alo;
    }
    static int whole = -1;
    if (whole < 0) whole = getenv("OCERZ_INV_GRANULE") ? 1 : 0;
    if (whole) { xlo = addr; xlen = len; }
    JitBlock **hits = NULL;
    size_t n_hit = 0;
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
        jl_release();
        return;
    }
    {
        static int noprec = -1;
        if (noprec < 0) noprec = getenv("OCERZ_INV_ALL") ? 1 : 0;
        size_t cap = 0;
        uint64_t g0 = addr >> INVMAP_GSHIFT, g1 = (addr + len) >> INVMAP_GSHIFT;
        int listed = g_gblk_off == 0 && g1 - g0 <= 256;
        size_t n_cand = listed ? 0 : jit->n_live;
        uint64_t gp = g0;
        uint32_t gj = 0;
        for (size_t k = 0;; k++) {
            JitBlock *b;
            if (listed) {
                int gs = -1;
                while (gp < g1 && ((gs = gblk_slot(gp, 0)) < 0 || gj >= g_gblk[gs].n)) { gp++; gj = 0; }
                if (gp >= g1) break;
                b = g_gblk[gs].v[gj++];
                if (b->inv_hit) continue;
            } else {
                if (k >= n_cand) break;
                b = jit->live[k];
                b->inv_hit = 0;
            }
            uint64_t blo = blk_insn_rip(b, 0);
            uint64_t bhi = blk_insn_rip(b, b->n_insns - 1) + blk_insn_len(b, b->n_insns - 1);
            if (!ranges_overlap(xlo, xlen, blo, bhi - blo)) continue;
            for (int i = 0; i < b->n_insns; i++)
                if (ranges_overlap(xlo, xlen, blk_insn_rip(b, i), blk_insn_len(b, i))) {
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
            invalidated = 2;
            invalidate_all_locked(jit);
        } else if (n_hit > 0) {
            invalidated = 1;
            if (noprec) {
                invalidated = 2;
                invalidate_all_locked(jit);
            } else {
                retire_hit_blocks_locked(vm, jit, hits, n_hit);
            }
        }
        if (n_hit != (size_t)-1)
            for (uint64_t g = addr; g < addr + len; g += 1ull << INVMAP_GSHIFT)
                if (!gran_any(g, g + (1ull << INVMAP_GSHIFT)))
                    invmap_clear_range(jit, g, g + (1ull << INVMAP_GSHIFT));
    }
    jl_release();

    if (invalidated == 1 && (g_retire_sweep & 255) != 0) {
        struct { JitBlock **v; size_t n; } set = { hits, n_hit };
        ocerz_vm_purge_jit_ras_if(vm, ras_entry_in_hits, &set);
    } else if (invalidated) {
        ocerz_vm_purge_jit_ras(vm);
    }
    if (n_hit != (size_t)-1)
        free(hits);
    if (invalidated && t0)
        __atomic_add_fetch(&ocerz_jit_retire_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0, __ATOMIC_RELAXED);
}

void ocerz_jit_request_stop(struct OcerzVM *vm)
{
    OcerzJit *jit = vm ? vm->jit : NULL;
    if (!jit)
        return;

    jl_acquire(__LINE__);
    jit->stop_requested = 1;
    pthread_jit_write_protect_np(0);
    int patched = force_stop_sites_writable(jit);
    pthread_jit_write_protect_np(1);
    if (patched)
        sys_icache_invalidate(jit->code_base,
            (size_t)((uint8_t *)jit->code_cur - (uint8_t *)jit->code_base));
    jl_release();
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

    jl_acquire(__LINE__);
    if (jit->plain_mem) {
        jit->plain_mem = 0;
        g_plain_mem = 0;
        invalidate_all_locked(jit);
    }
    jl_release();

    ocerz_vm_purge_jit_ras(vm);
}

void ocerz_jit_prefork(void)
{
    jl_acquire(__LINE__);
}

void ocerz_jit_postfork(void)
{
    jl_release();
}

void ocerz_jit_postfork_child(void)
{
    JitThr *self = t_jit_thr;
    for (JitThr *t = __atomic_load_n(&g_jit_thr, __ATOMIC_ACQUIRE); t; t = t->next)
        if (t != self) {
            __atomic_store_n(&t->frames, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&t->parked, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&t->dead, 1, __ATOMIC_RELAXED);
        }
    __atomic_store_n(&g_flush_req, 0, __ATOMIC_SEQ_CST);
    g_flush_mark = UINT64_MAX;
    g_flush_retry_ns = 0;
}

void chain_edge_now(OcerzJit *jit, JitBlock *blk, int e)
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

static struct { uint64_t rip; uint64_t n; } g_trip_tab[TRIP_SLOTS];

static uint64_t g_trip_count;

static OcerzJit *g_trip_jit;

static void *trip_report(void *arg)
{
    uint64_t last = 0;
    for (;;) {
        usleep(10000000);
        uint64_t now = __atomic_load_n(&g_trip_count, __ATOMIC_RELAXED);
        fprintf(stderr, "ocerz: TRIPSTAT[%d] trips/s=%.0f\n", (int)getpid(), (double)(now - last) / 10.0);
        last = now;
        int top[20];
        int nt = 0;
        for (int k = 0; k < 20; k++) {
            int best = -1;
            for (int i = 0; i < TRIP_SLOTS; i++) {
                int used = 0;
                for (int j = 0; j < nt; j++)
                    used |= top[j] == i;
                if (!used && g_trip_tab[i].n && (best < 0 || g_trip_tab[i].n > g_trip_tab[best].n))
                    best = i;
            }
            if (best < 0)
                break;
            top[nt++] = best;
        }
        uint64_t sampled = 0;
        for (int i = 0; i < TRIP_SLOTS; i++)
            sampled += g_trip_tab[i].n;
        for (int j = 0; j < nt; j++) {
            JitBlock *tb = g_trip_jit ? cache_lookup(g_trip_jit, g_trip_tab[top[j]].rip, 0) : NULL;
            fprintf(stderr, "ocerz: TRIPSTAT[%d]   %#llx %.1f%% blk=%d code=%d body=%d pin_class=%d n_pinned=%d insns=%d preds=%u execs=%llu\n",
                    (int)getpid(), (unsigned long long)g_trip_tab[top[j]].rip,
                    100.0 * (double)g_trip_tab[top[j]].n / (double)(sampled ? sampled : 1),
                    tb != NULL, tb && tb->code, tb && tb->body_code, tb ? tb->pin_class : -1,
                    tb ? tb->n_pinned : -1, tb ? tb->n_insns : -1, tb ? tb->n_preds : 0,
                    tb ? (unsigned long long)tb->exec_count : 0ull);
        }
        memset(g_trip_tab, 0, sizeof g_trip_tab);
    }
    return arg;
}

static void trip_note(uint64_t rip)
{
    static int state = -1;
    if (state < 0) {
        state = 0;
        if (getenv("OCERZ_TRIPSTAT")) {
            pthread_t t;
            if (pthread_create(&t, NULL, trip_report, NULL) == 0) {
                pthread_detach(t);
                state = 1;
            }
        }
    }
    if (state != 1)
        return;
    uint64_t c = __atomic_add_fetch(&g_trip_count, 1, __ATOMIC_RELAXED);
    if (c & 63)
        return;
    unsigned h = (unsigned)((rip * 0x9E3779B97F4A7C15ull) >> 52) & (TRIP_SLOTS - 1);
    for (int k = 0; k < 8; k++, h = (h + 1) & (TRIP_SLOTS - 1)) {
        if (g_trip_tab[h].rip == rip || g_trip_tab[h].n == 0) {
            g_trip_tab[h].rip = rip;
            g_trip_tab[h].n++;
            return;
        }
    }
}

static void code_index_reset_locked(OcerzJit *jit)
{
    JitCodeIndex *old = __atomic_load_n(&jit->ci, __ATOMIC_RELAXED);
    JitCodeIndex *next = (JitCodeIndex *)malloc(sizeof(*next) + 4096 * sizeof(next->blocks[0]));
    if (!next)
        abort();
    next->older = old;
    next->capacity = 4096;
    next->count = 0;
    __atomic_store_n(&jit->ci, next, __ATOMIC_RELEASE);
}

static void jit_arena_reset_locked(OcerzJit *jit)
{
    __atomic_add_fetch(&ocerz_jit_retire_count, 1, __ATOMIC_RELEASE);
    for (size_t k = 0; k < jit->n_live; k++) {
        JitBlock *b = jit->live[k];
        __atomic_store_n(&jit->buckets[hash_key(b->key)], (JitBlock *)NULL, __ATOMIC_RELEASE);
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
    __atomic_store_n(&g_flush_want, 0, __ATOMIC_RELAXED);
    g_n_ras_cells = 0;
    g_ras_slot_n = 0;
    g_n_psc_tables = 0;
    g_psc_used = 0;
    jit->stop_blocks = NULL;
    jit->dispatch_stub = NULL;
    jit->dispatch_stub32 = NULL;
    jit->veneer_n = 0;
    jit->veneer_next_mark = NULL;
    memset(jit->veneer_pool, 0, sizeof jit->veneer_pool);
    memset(jit->veneer_used, 0, sizeof jit->veneer_used);
    code_index_reset_locked(jit);
    jit->code_cur = jit->code_start;
    jit->code_full = 0;
}

static void stop_sites_sync(const StopUndo *u, size_t n)
{
    for (size_t i = 0; i < n; i++)
        __asm__ volatile("dc cvau, %0" :: "r"(u[i].site) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (size_t i = 0; i < n; i++)
        __asm__ volatile("ic ivau, %0" :: "r"(u[i].site) : "memory");
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}

static StopUndo *stop_sites_force_undoable(OcerzJit *jit, size_t *n_out)
{
    size_t n = 0, cap = 0;
    StopUndo *u = NULL;
#define STOP_SET(at, insn) do { \
        uint32_t *s_ = (at), w_ = (insn); \
        if (s_ && w_ && *s_ != w_) { \
            if (n == cap) { cap = cap ? cap * 2 : 256; StopUndo *nu = (StopUndo *)realloc(u, cap * sizeof *u); if (!nu) abort(); u = nu; } \
            u[n++] = (StopUndo){ s_, *s_, w_ }; \
            __atomic_store_n(s_, w_, __ATOMIC_RELEASE); \
        } \
    } while (0)
    pthread_jit_write_protect_np(0);
    for (JitBlock *b = jit->stop_blocks; b; b = b->stop_next) {
        STOP_SET(b->stop_patch, b->stop_insn);
        for (int i = 0; i < b->n_stop_extra; i++)
            STOP_SET(b->stop_extra[i].site, b->stop_extra[i].insn);
        for (int i = 0; i < b->n_edges; i++) {
            int is_stop = b->edges[i].patch_b == b->stop_patch;
            for (int k = 0; k < b->n_stop_extra && !is_stop; k++)
                is_stop = b->edges[i].patch_b == b->stop_extra[k].site;
            if (is_stop)
                STOP_SET(b->edges[i].cond_site, b->edges[i].cond_orig);
        }
    }
    pthread_jit_write_protect_np(1);
#undef STOP_SET
    stop_sites_sync(u, n);
    *n_out = n;
    return u;
}

static void stop_sites_undo(OcerzJit *jit, StopUndo *u, size_t n)
{
    if (!n)
        return;
    pthread_jit_write_protect_np(0);
    for (size_t i = n; i-- > 0;)
        if (*u[i].site == u[i].now)
            __atomic_store_n(u[i].site, u[i].was, __ATOMIC_RELEASE);
    pthread_jit_write_protect_np(1);
    stop_sites_sync(u, n);
    (void)jit;
}

static int jit_space_low(const OcerzJit *jit)
{
    size_t total = (size_t)((const uint8_t *)jit->code_end - (const uint8_t *)jit->code_start);
    size_t margin = total / 8 < ((size_t)8 << 20) ? total / 8 : ((size_t)8 << 20);
    return (size_t)((const uint8_t *)jit->code_end - (const uint8_t *)jit->code_cur) < margin;
}

static unsigned long long g_flush_fail;

static unsigned long long g_flush_n;

static int jit_flush(struct OcerzVM *vm, OcerzJit *jit)
{
    static int off = -1;
    if (off < 0) off = getenv("OCERZ_NO_JIT_FLUSH") ? 1 : 0;
    if (off)
        return 0;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (t0 < __atomic_load_n(&g_flush_retry_ns, __ATOMIC_RELAXED))
        return 0;
    int idle = 0;
    if (!__atomic_compare_exchange_n(&g_flush_req, &idle, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return 0;
    JitThr *self = jit_thr();
    if (__atomic_load_n(&self->frames, __ATOMIC_SEQ_CST) != __atomic_load_n(&self->parked, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&g_flush_req, 0, __ATOMIC_SEQ_CST);
        return 0;
    }
    __atomic_add_fetch(&g_flush_gen, 1, __ATOMIC_SEQ_CST);
    jl_acquire(__LINE__);
    int full = jit->code_full || jit_space_low(jit) || __atomic_load_n(&g_flush_want, __ATOMIC_RELAXED);
    if (full && jit->blocks_translated == g_flush_mark) {
        jl_release();
        __atomic_add_fetch(&g_flush_gen, 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_flush_req, 0, __ATOMIC_SEQ_CST);
        return 0;
    }
    size_t n_undo = 0;
    StopUndo *undo = full ? stop_sites_force_undoable(jit, &n_undo) : NULL;
    jl_release();
    if (!full) {
        __atomic_add_fetch(&g_flush_gen, 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_flush_req, 0, __ATOMIC_SEQ_CST);
        return 1;
    }
    int ok = 1;
    uint64_t deadline = t0 + 500000000ull;
    static int log = -1;
    if (log < 0) log = getenv("OCERZ_FLUSHLOG") ? 1 : 0;
    uint64_t next_report = t0 + 50000000ull;
    while (ok) {
        int busy = 0;
        for (JitThr *t = __atomic_load_n(&g_jit_thr, __ATOMIC_ACQUIRE); t && !busy; t = t->next)
            if (t != self && __atomic_load_n(&t->frames, __ATOMIC_SEQ_CST) != __atomic_load_n(&t->parked, __ATOMIC_SEQ_CST))
                busy = 1;
        if (!busy)
            break;
        if (log && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) > next_report) {
            next_report += 50000000ull;
            for (JitThr *t = __atomic_load_n(&g_jit_thr, __ATOMIC_ACQUIRE); t; t = t->next)
                if (t != self && __atomic_load_n(&t->frames, __ATOMIC_SEQ_CST) != __atomic_load_n(&t->parked, __ATOMIC_SEQ_CST))
                    fprintf(stderr, "ocerz: JIT flush waiting on cpu#%u frames=%d parked=%d last block %#llx\n",
                            t->cpu ? t->cpu->cpu_number : 0u, t->frames, t->parked,
                            t->cpu ? (unsigned long long)t->cpu->rip : 0ull);
        }
        if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) > deadline) {
            ok = 0;
            g_flush_fail++;
            __atomic_store_n(&g_flush_retry_ns, t0 + 2000000000ull, __ATOMIC_RELAXED);
            break;
        }
        struct timespec ts = { 0, 20000 };
        nanosleep(&ts, NULL);
    }
    jl_acquire(__LINE__);
    if (!ok)
        stop_sites_undo(jit, undo, n_undo);
    free(undo);
    jl_release();
    if (ok) {
        jl_acquire(__LINE__);
        jit_arena_reset_locked(jit);
        g_flush_mark = jit->blocks_translated;
        jl_release();
        ocerz_vm_purge_jit_refs(vm);
        g_flush_n++;
    }
    if (log || (!ok && g_flush_fail == 1) || (ok && g_flush_n == 1))
        fprintf(stderr, "ocerz: JIT arena %s (flush #%llu, %llu failed, %llu us)\n",
                ok ? "flushed" : "flush gave up waiting for threads to leave translated code",
                g_flush_n, g_flush_fail,
                (unsigned long long)((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1000));
    __atomic_add_fetch(&g_flush_gen, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_flush_req, 0, __ATOMIC_SEQ_CST);
    return ok;
}

int ocerz_jit_step(struct OcerzVM *vm, OcerzCPU *cpu)
{
    g_trip_jit = vm->jit;
    trip_note(cpu->rip);
    if (cpu->rip - OCERZ_DYLDAPI_LO < (OCERZ_DYLDAPI_HI - OCERZ_DYLDAPI_LO))
        return OCERZ_EUNSUP;
    { static int sl = -1; if (sl < 0) sl = getenv("OCERZ_STEPLOG") ? 1 : 0; if (sl) steplog(cpu); }
    OcerzJit *jit = vm->jit;
    if (cpu->side_blk) flip_side_hit(vm, jit, cpu);
    if (ocerz_jitstat < 0) {
        jl_acquire(__LINE__);
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
        jl_release();
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

    if (__builtin_expect(__atomic_load_n(&g_flush_req, __ATOMIC_SEQ_CST), 0))
        return OCERZ_EUNSUP;
    uint64_t gen = __atomic_load_n(&g_flush_gen, __ATOMIC_SEQ_CST);
    JitBlock *b = cache_lookup(jit, cpu->rip, cpu->mode32);
    if (!b && __builtin_expect(jit_space_low(jit) || __atomic_load_n(&g_flush_want, __ATOMIC_RELAXED), 0) &&
        jit_flush(vm, jit))
        return OCERZ_STEP_OK;
    if (!b) {
        jl_lock_step(cpu->rip);
        g_jl_owner_cpu = cpu;
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
            g_plain_mem = jit->plain_mem || (!cpu->mode32 && ocerz_dyldapi_memfn(cpu->rip));
            if (g_jl_log > 0)
                __atomic_store_n(&g_jl_phase, 2, __ATOMIC_RELAXED);
            g_xlat_ftop = cpu->ftop & 7;
            b = translate(jit, cpu->rip, cpu->mode32);
            g_xlat_ftop = -1;
            if (b)
                xlatpage_note(cpu->rip);
            if (g_jl_log > 0 && !b)
                __atomic_add_fetch(&g_jl_xlat_null, 1, __ATOMIC_RELAXED);
        }
        g_jl_owner_cpu = NULL;
        jl_unlock_step();
    } else {
        if (ocerz_jitstat > 0)
            js_hits++;
        if (ocerz_perfstat > 0)
            ps_hits++;
    }

    if (!b)
        return OCERZ_EUNSUP;
    if (!b->code) {
        if (__builtin_expect(t_xlat_overflow, 0)) {
            t_xlat_overflow = 0;
            if (jit_flush(vm, jit))
                return OCERZ_STEP_OK;
        }
        return jit_interp_block(vm, cpu, b);
    }
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
    JitThr *t = jit_thr();
    int base = __atomic_load_n(&t->frames, __ATOMIC_RELAXED);
    t->cpu = cpu;
    __atomic_store_n(&t->frames, base + 1, __ATOMIC_SEQ_CST);
    if (__builtin_expect(__atomic_load_n(&g_flush_req, __ATOMIC_SEQ_CST) ||
                         __atomic_load_n(&g_flush_gen, __ATOMIC_SEQ_CST) != gen, 0)) {
        __atomic_store_n(&t->frames, base, __ATOMIC_SEQ_CST);
        return __atomic_load_n(&g_flush_req, __ATOMIC_SEQ_CST) ? OCERZ_EUNSUP : OCERZ_STEP_OK;
    }
    int r = b->code(vm, cpu);
    __atomic_store_n(&t->frames, base, __ATOMIC_SEQ_CST);
    return r;
}

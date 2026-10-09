/*
 * ---- kept translations ----
 * Unless OCERZ_TCACHE=off, a translation is written to src/tcache.c's store,
 * and the next process to need the same key loads it instead of translating.
 * Only code whose address is the same from run to run is kept: everything in
 * a Wine process, whose low-shadow layout and fixed image bases make it so,
 * and otherwise only the shared cache, since a main program, a dylib or a JIT
 * loaded at a random address would fill the store with records nobody loads,
 * and nothing at all when the guest's own base is chosen at random.  The
 * code is emitted so that it can move: every value that differs between
 * processes (an ocerz function or global, the commpage delta, the block's own
 * JitBlock, an instruction or profile slot inside it, a RAS slot, a
 * return-address cell, an indirect-call cache table, the leaf routines and the
 * dispatch stub) is loaded by a fixed movz and three movk or sits in a literal
 * cell, and translate records where (TcReloc); a leaf call and the
 * dispatch-stub exit go through a register instead of a direct branch.  A load
 * copies the code to the same address modulo 64, because the literal pool's
 * padding and a loop head's alignment are absolute, and fills every site from
 * the current process, allocating slots, cells and tables afresh.  A record
 * also names every guest byte range the translation read through jit_decode -
 * the block, the callees spliced into it, and the successors whose liveness it
 * relied on, which is why a recording translation decodes a successor itself
 * rather than trusting its entry_live, and why an xlive memo entry keeps the
 * ranges and a hash of the bytes it read, replaying them into the record when
 * it answers - and a hash of those bytes, which a load checks against guest
 * memory first.  The key adds
 * the memory model to jit_key, since a block translates differently once a
 * second thread makes loads ordered.  A translation made because the process
 * learned something (a flipped branch, the guard a commpage or alignment fault
 * asked for) is never answered from the store: a key that carries a mark, or
 * whose block was retired or invalidated, is translated again, and the new
 * record replaces the old.  OCERZ_TCACHE=verify translates everything anyway
 * and compares it with the record it would have loaded, counting a difference
 * in shape as a variant and any other as a bug; OCERZ_TCACHE=roundtrip writes
 * nothing, moves every translation to a fresh address, fills the original with
 * BRK, and reports a PC-relative reference that leaves the block or a
 * host-looking constant that no relocation covers.
 *
 */
#include "ocerz/jit_internal.h"

static uint64_t tc_value(const OcerzJit *jit, const JitBlock *blk, int kind, uint64_t arg, int *ok);
static uint64_t tc_imm_read(const uint32_t *w);
static int tc_imm_shape(const uint32_t *w);
static void tc_imm_write(uint32_t *w, uint64_t v);
static void tc_ras_fill(OcerzJit *jit, void **cell, uint64_t retaddr, int mode32);
static const uint32_t *tc_pcrel_target(const uint32_t *w);
static int tc_is_reloc_word(uint32_t w);
static int tc_insn_at(const JitBlock *blk, uint32_t w);
static void tc_report(const JitBlock *blk, const char *what, uint32_t w, uint32_t v, uint64_t x);
static int tc_host_const(const OcerzJit *jit, const JitBlock *blk, uint64_t x);
static int tc_scan(const OcerzJit *jit, const JitBlock *blk, const uint32_t *code, uint32_t words);
void ocerz_jit_tcache_final(void);
static uint64_t tc_mix(uint64_t h, uint64_t w);
static uint64_t tc_hash_range(uint64_t h, uint64_t lo, const uint8_t *p, uint64_t n);
static int cmp_dlog(const void *a, const void *b);
static int tc_deps_build(uint32_t *ndep, uint64_t *hash);
static int tc_deps_check(const TcDep *d, uint32_t n, uint64_t want);
static int tc_out(const void *p, size_t n);
static uint32_t tc_off(const JitBlock *blk, const uint32_t *p);
static int tc_parse(const OcerzTcRecHead *h, TcView *v);
static void tc_free_blk(JitBlock *b);
static void *tc_dup(const void *p, size_t n, int *fail);

TcReloc g_tc_rel[TC_RELOC_MAX];

int g_tc_nrel;

int g_tc_on;

int g_tc_bad;

const uint32_t *g_tc_entry;

struct JitState_g_tc_dlog g_tc_dlog[TC_DLOG_MAX];

uint8_t g_tc_dbytes[TC_DBYTES_MAX] = {0};

uint32_t g_tc_nbytes;

int g_tc_dbar;

int g_tc_learned;

int jit_decode(uint64_t pc, X86Insn *out, int mode32)
{
    const uint8_t *code = (const uint8_t *)ocerz_g2h(pc);
    int rc = ocerz_decode_mode(code, 15, pc, out, mode32);
    if (rc == OCERZ_OK && g_tc_rec) {
        int last = g_tc_ndlog - 1;
        if (last >= g_tc_dbar && g_tc_dlog[last].pc + g_tc_dlog[last].len == pc &&
            g_tc_dlog[last].at + g_tc_dlog[last].len == g_tc_nbytes && g_tc_nbytes + out->len <= TC_DBYTES_MAX) {
            memcpy(g_tc_dbytes + g_tc_nbytes, code, out->len);
            g_tc_dlog[last].len += out->len;
            g_tc_nbytes += out->len;
        } else if (g_tc_ndlog < TC_DLOG_MAX && g_tc_nbytes + out->len <= TC_DBYTES_MAX) {
            g_tc_dlog[g_tc_ndlog].pc = pc;
            g_tc_dlog[g_tc_ndlog].at = g_tc_nbytes;
            g_tc_dlog[g_tc_ndlog].len = out->len;
            memcpy(g_tc_dbytes + g_tc_nbytes, code, out->len);
            g_tc_ndlog++;
            g_tc_nbytes += out->len;
        } else {
            g_tc_bad = 1;
        }
    }
    return rc;
}

uint64_t *g_tc_noload;

size_t g_tc_noload_cap;

size_t g_tc_noload_n;

void tc_noload_add(uint64_t key)
{
    if (!key || ocerz_tcache_mode() != OCERZ_TC_ON || tc_noload_has(key))
        return;
    if (2 * (g_tc_noload_n + 1) > g_tc_noload_cap) {
        size_t ncap = g_tc_noload_cap ? g_tc_noload_cap * 2 : 4096;
        uint64_t *nv = (uint64_t *)calloc(ncap, sizeof *nv);
        if (!nv)
            return;
        for (size_t k = 0; k < g_tc_noload_cap; k++) {
            uint64_t v = g_tc_noload[k];
            if (!v) continue;
            size_t i = (size_t)((v * 0x9e3779b97f4a7c15ull) >> 20) & (ncap - 1);
            while (nv[i]) i = (i + 1) & (ncap - 1);
            nv[i] = v;
        }
        free(g_tc_noload);
        g_tc_noload = nv;
        g_tc_noload_cap = ncap;
    }
    size_t i = (size_t)((key * 0x9e3779b97f4a7c15ull) >> 20) & (g_tc_noload_cap - 1);
    while (g_tc_noload[i]) i = (i + 1) & (g_tc_noload_cap - 1);
    g_tc_noload[i] = key;
    g_tc_noload_n++;
}

int ocerz_jitstat = -1;

uint32_t g_tc_pool_off;

static _Atomic unsigned long long g_tc_n_bad;

static _Atomic unsigned long long g_tc_n_const;

static _Atomic unsigned long long g_tc_n_full;

static _Atomic unsigned long long g_tc_n_mism;

static _Atomic unsigned long long g_tc_n_ok;

static _Atomic unsigned long long g_tc_n_pcrel;

int g_tc_log = -1;

FILE *g_tc_lf;

static uint64_t tc_value(const OcerzJit *jit, const JitBlock *blk, int kind, uint64_t arg, int *ok)
{
    *ok = 1;
    switch (kind) {
    case TCR_SYM:
        switch ((int)arg) {
        case TCS_FLAGS_MATERIALIZE: return (uint64_t)(uintptr_t)&ocerz_flags_materialize;
        case TCS_RAS_PUSH: return (uint64_t)(uintptr_t)&ocerz_ras_push;
        case TCS_EXEC_ONE: return (uint64_t)(uintptr_t)&ocerz_jit_exec_one;
        case TCS_EXEC_ONE_AT: return (uint64_t)(uintptr_t)&ocerz_jit_exec_one_at;
        case TCS_JGB_TRAP: return (uint64_t)(uintptr_t)&ocerz_jgb_trap;
        case TCS_RETIRE_COUNT: return (uint64_t)(uintptr_t)&ocerz_jit_retire_count;
        case TCS_EXEC_RUN_AT: return (uint64_t)(uintptr_t)&ocerz_jit_exec_run_at;
        default: break;
        }
        break;
    case TCR_COMMPAGE:
        if (ocerz_commpage)
            return (uint64_t)(uintptr_t)ocerz_commpage - OCERZ_COMMPAGE_LO - ocerz_guest_base;
        break;
    case TCR_BUCKETS:
        return (uint64_t)(uintptr_t)jit->buckets;
    case TCR_LEAF:
        return (uint64_t)(uintptr_t)(jit->leaf_near ? jit->leaf_near + arg : ocerz_leaf_lo + arg);
    case TCR_DSTUB: {
        const uint32_t *d = arg ? jit->dispatch_stub32 : jit->dispatch_stub;
        if (d) return (uint64_t)(uintptr_t)d;
        break;
    }
    case TCR_BLK:
        return (uint64_t)(uintptr_t)blk;
    case TCR_INSN:
        if (blk->insns && arg < (uint64_t)blk->n_insns)
            return (uint64_t)(uintptr_t)&blk->insns[arg];
        break;
    case TCR_PROF:
        if (blk->prof && arg < SIDE_MAX * sizeof(JitProf))
            return (uint64_t)(uintptr_t)((const char *)blk->prof + arg);
        break;
    default:
        break;
    }
    *ok = 0;
    return 0;
}

static uint64_t tc_imm_read(const uint32_t *w)
{
    uint64_t v = 0;
    for (int k = 0; k < 4; k++)
        v |= (uint64_t)((w[k] >> 5) & 0xffffu) << (16 * k);
    return v;
}

static int tc_imm_shape(const uint32_t *w)
{
    if ((w[0] & 0xffe00000u) != 0xd2800000u)
        return 0;
    for (int k = 1; k < 4; k++)
        if ((w[k] & 0xffe00000u) != (0xf2800000u | ((uint32_t)k << 21)) || (w[k] & 31) != (w[0] & 31))
            return 0;
    return 1;
}

static void tc_imm_write(uint32_t *w, uint64_t v)
{
    for (int k = 0; k < 4; k++)
        w[k] = (w[k] & ~(0xffffu << 5)) | ((uint32_t)(v >> (16 * k)) & 0xffffu) << 5;
}

static void tc_ras_fill(OcerzJit *jit, void **cell, uint64_t retaddr, int mode32)
{
    JitBlock *rb = cache_lookup(jit, retaddr, mode32);
    if (rb && rb->code)
        *cell = ras_entry_for(rb);
    else {
        *cell = NULL;
        pending_add_ras(jit_key(retaddr, mode32), cell);
    }
}

static uint64_t g_tc_val[TC_RELOC_MAX];

int tc_bind(OcerzJit *jit, JitBlock *blk, uint32_t *code, const TcReloc *rel, int nrel, int fresh)
{
    int mode32 = blk_mode32(blk);
    if (fresh)
        for (int i = 0; i < nrel; i++) {
            const TcReloc *r = &rel[i];
            if (r->kind == TCR_RASCELL)
                continue;
            if (r->kind == TCR_RASSLOT) {
                void **sl = ras_slot_alloc();
                if (!sl) return 0;
                g_tc_val[i] = (uint64_t)(uintptr_t)sl;
            } else if (r->kind == TCR_PSC) {
                JitPscEnt *t = psc_alloc();
                if (!t) return 0;
                g_tc_val[i] = (uint64_t)(uintptr_t)t;
            } else {
                int ok;
                g_tc_val[i] = tc_value(jit, blk, r->kind, r->arg, &ok);
                if (!ok) return 0;
            }
        }
    for (int i = 0; i < nrel; i++) {
        const TcReloc *r = &rel[i];
        uint32_t *w = code + r->off;
        if (r->kind == TCR_RASCELL) {
            ras_cell_register((void **)w);
            tc_ras_fill(jit, (void **)w, r->arg, mode32);
            continue;
        }
        if (!fresh)
            continue;
        if (r->kind == TCR_RASSLOT)
            tc_ras_fill(jit, (void **)(uintptr_t)g_tc_val[i], r->arg, mode32);
        if (r->form == 1)
            *(uint64_t *)(void *)w = g_tc_val[i];
        else
            tc_imm_write(w, g_tc_val[i]);
    }
    return 1;
}

static const uint32_t *tc_pcrel_target(const uint32_t *w)
{
    uint32_t v = *w;
    int64_t off;
    if ((v & 0x7c000000u) == 0x14000000u)
        off = (int64_t)((int32_t)(v << 6) >> 6) * 4;
    else if ((v & 0xff000010u) == 0x54000000u || (v & 0x7e000000u) == 0x34000000u ||
             (v & 0x3b000000u) == 0x18000000u)
        off = (int64_t)((int32_t)(v << 8) >> 13) * 4;
    else if ((v & 0x7e000000u) == 0x36000000u)
        off = (int64_t)((int32_t)(v << 13) >> 18) * 4;
    else if ((v & 0x9f000000u) == 0x10000000u)
        off = (int64_t)(((int32_t)(v << 8) >> 13) * 4) | ((v >> 29) & 3);
    else if ((v & 0x9f000000u) == 0x90000000u)
        return NULL;
    else
        return w;
    return (const uint32_t *)((const uint8_t *)w + off);
}

static int tc_is_reloc_word(uint32_t w)
{
    for (int i = 0; i < g_tc_nrel; i++) {
        uint32_t o = g_tc_rel[i].off, len = g_tc_rel[i].form == 1 ? 2 : 4;
        if (w >= o && w < o + len)
            return 1;
    }
    return 0;
}

static int tc_insn_at(const JitBlock *blk, uint32_t w)
{
    int ii = -1;
    if (blk->insn_off)
        for (int k = 0; k < blk->n_insns; k++)
            if (blk->insn_off[k] <= w) ii = k;
    return ii;
}

static void tc_report(const JitBlock *blk, const char *what, uint32_t w, uint32_t v, uint64_t x)
{
    static int n;
    if (!g_tc_log || __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) >= 60)
        return;
    int ii = tc_insn_at(blk, w);
    char tb[128] = "";
    if (ii >= 0 && blk->insns)
        ocerz_format_insn(&blk->insns[ii], tb, sizeof tb);
    fprintf(g_tc_lf, "ocerz: TCACHE[%d] %s rip=%#llx word=%u v=%08x x=%#llx insn=%d %s\n", (int)getpid(), what,
            (unsigned long long)blk_rip(blk), w, v, (unsigned long long)x, ii, tb);
}

static int tc_host_const(const OcerzJit *jit, const JitBlock *blk, uint64_t x)
{
    static const void *self_base;
    if (!self_base) {
        Dl_info di;
        if (dladdr((const void *)&ocerz_jit_exec_one, &di)) self_base = di.dli_fbase;
    }
    const uint8_t *p = (const uint8_t *)(uintptr_t)x;
    if (p >= (const uint8_t *)jit->code_base && p < (const uint8_t *)jit->code_end)
        return 1;
    if (p >= (const uint8_t *)blk && p < (const uint8_t *)(blk + 1))
        return 1;
    if (blk->insns && p >= (const uint8_t *)blk->insns && p < (const uint8_t *)(blk->insns + blk->n_insns))
        return 1;
    if (blk->prof && p >= (const uint8_t *)blk->prof && p < (const uint8_t *)(blk->prof + SIDE_MAX))
        return 1;
    if (ocerz_commpage && x == (uint64_t)(uintptr_t)ocerz_commpage - OCERZ_COMMPAGE_LO - ocerz_guest_base)
        return 1;
    if (g_ras_slots && p >= (const uint8_t *)g_ras_slots && p < (const uint8_t *)(g_ras_slots + RAS_SLOT_CAP))
        return 1;
    if (p >= (const uint8_t *)jit && p < (const uint8_t *)(jit + 1))
        return 1;
    if (x >= 0x100000000ull && x < 0x800000000000ull) {
        Dl_info di;
        if (self_base && dladdr(p, &di) && di.dli_fbase == self_base)
            return 1;
    }
    return 0;
}

static int tc_scan(const OcerzJit *jit, const JitBlock *blk, const uint32_t *code, uint32_t words)
{
    int bad = 0;
    uint32_t end = g_tc_pool_off < words ? g_tc_pool_off : words;
    for (uint32_t w = 0; w < end; w++) {
        if (tc_is_reloc_word(w))
            continue;
        const uint32_t *t = tc_pcrel_target(code + w);
        if (t == code + w)
            continue;
        if (!t || t < code || t >= code + words) {
            tc_report(blk, "PCREL", w, code[w], t ? (uint64_t)(t - code) : 0);
            g_tc_n_pcrel++;
            bad = 1;
        }
    }
    for (uint32_t w = 0; w < end; w++) {
        uint32_t v = code[w];
        if ((v & 0xffe00000u) != 0xd2800000u || tc_is_reloc_word(w))
            continue;
        int rd = (int)(v & 31);
        uint64_t x = (uint64_t)((v >> 5) & 0xffffu);
        uint32_t k = w + 1;
        while (k < end && (code[k] & 0xff800000u) == 0xf2800000u && (int)(code[k] & 31) == rd) {
            int hw = (int)((code[k] >> 21) & 3);
            x = (x & ~(0xffffull << (16 * hw))) | ((uint64_t)((code[k] >> 5) & 0xffffu) << (16 * hw));
            k++;
        }
        if (k > w + 1 && tc_host_const(jit, blk, x)) {
            tc_report(blk, "HOSTCONST", w, v, x);
            g_tc_n_const++;
            bad = 1;
        }
    }
    return bad;
}

static _Atomic unsigned long long g_tc_n_load;

static _Atomic unsigned long long g_tc_n_put;

static _Atomic unsigned long long g_tc_n_rej;

static _Atomic unsigned long long g_tc_n_stale;

static _Atomic unsigned long long g_tc_n_vbad;

static _Atomic unsigned long long g_tc_n_vok;

static _Atomic unsigned long long g_tc_n_vvar;

void tc_summary(void)
{
    if (ocerz_tcache_mode() == OCERZ_TC_ROUNDTRIP)
        fprintf(g_tc_lf, "ocerz: TCACHE[%d] roundtrip ok=%llu bad=%llu pcrel=%llu hostconst=%llu mismatch=%llu full=%llu\n",
                (int)getpid(), (unsigned long long)g_tc_n_ok, (unsigned long long)g_tc_n_bad,
                (unsigned long long)g_tc_n_pcrel, (unsigned long long)g_tc_n_const,
                (unsigned long long)g_tc_n_mism, (unsigned long long)g_tc_n_full);
    else
        fprintf(g_tc_lf, "ocerz: TCACHE[%d] loaded=%llu saved=%llu stale=%llu rejected=%llu verify_ok=%llu"
                         " verify_variant=%llu verify_bad=%llu\n",
                (int)getpid(), (unsigned long long)g_tc_n_load, (unsigned long long)g_tc_n_put,
                (unsigned long long)g_tc_n_stale, (unsigned long long)g_tc_n_rej,
                (unsigned long long)g_tc_n_vok, (unsigned long long)g_tc_n_vvar, (unsigned long long)g_tc_n_vbad);
    fflush(g_tc_lf);
}

void ocerz_jit_tcache_final(void)
{
    ocerz_tcache_flush();
    if (g_tc_log > 0)
        tc_summary();
}

uint32_t *tc_roundtrip(OcerzJit *jit, JitBlock *blk, uint32_t *entry)
{
    uint32_t words = blk->code_words;
    int bad = g_tc_bad;
    if (tc_scan(jit, blk, entry, words))
        bad = 1;
    for (int i = 0; i < g_tc_nrel; i++) {
        const TcReloc *r = &g_tc_rel[i];
        if (r->form != 0)
            continue;
        if (r->off + 4 > words || !tc_imm_shape(entry + r->off)) {
            tc_report(blk, "SHAPE", r->off, r->off < words ? entry[r->off] : 0, r->kind);
            g_tc_n_mism++;
            bad = 1;
            continue;
        }
        if (r->kind == TCR_RASSLOT)
            continue;
        int ok;
        uint64_t v = tc_value(jit, blk, r->kind, r->arg, &ok);
        if (!ok || v != tc_imm_read(entry + r->off)) {
            tc_report(blk, "MISMATCH", r->off, entry[r->off], ((uint64_t)r->kind << 56) | r->arg);
            g_tc_n_mism++;
            bad = 1;
        }
    }
    if (bad) {
        g_tc_n_bad++;
        return NULL;
    }
    uint8_t *p = (uint8_t *)jit->code_cur;
    uint8_t *copy = p + (((uintptr_t)entry - (uintptr_t)p) & 63);
    if (copy + (size_t)words * 4 > (uint8_t *)jit->code_end) {
        g_tc_n_full++;
        return NULL;
    }
    uint32_t *c = (uint32_t *)copy;
    pthread_jit_write_protect_np(0);
    memcpy(c, entry, (size_t)words * 4);
    if (!tc_bind(jit, blk, c, g_tc_rel, g_tc_nrel, 1)) {
        pthread_jit_write_protect_np(1);
        g_tc_n_bad++;
        return NULL;
    }
    for (uint32_t w = 0; w < words; w++)
        entry[w] = 0xd4200000u | (0xc0deu << 5);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(entry, (size_t)words * 4);
    sys_icache_invalidate(c, (size_t)words * 4);
    jit->code_cur = c + words;

    ptrdiff_t d = c - entry;
#define TC_RB(p) ((p) ? (p) + d : NULL)
    blk->code = (JitBlockFn)(void *)c;
    blk->body_code = TC_RB(blk->body_code);
    blk->body_noreload = TC_RB(blk->body_noreload);
    blk->stop_patch = TC_RB(blk->stop_patch);
    for (int i = 0; i < blk->n_stop_extra; i++)
        blk->stop_extra[i].site = TC_RB(blk->stop_extra[i].site);
    for (int i = 0; i < blk->n_edges; i++) {
        blk->edges[i].patch_b = TC_RB(blk->edges[i].patch_b);
        blk->edges[i].cond_site = TC_RB(blk->edges[i].cond_site);
    }
    if (blk->prof)
        for (int k = 0; k < SIDE_MAX; k++) {
            blk->prof[k].ft_site = TC_RB(blk->prof[k].ft_site);
            blk->prof[k].tk_trip = TC_RB(blk->prof[k].tk_trip);
        }
#undef TC_RB
    g_tc_n_ok++;
    return c;
}

uint64_t g_tc_key;

static TcDep g_tc_mdep[TC_DLOG_MAX];

static uint8_t g_tc_mbytes[TC_DBYTES_MAX];

static uint8_t *g_tc_out;

static size_t g_tc_opos;

static uint64_t tc_mix(uint64_t h, uint64_t w)
{
    h = (h ^ w) * 0x9e3779b97f4a7c15ull;
    return h ^ (h >> 29);
}

static uint64_t tc_hash_range(uint64_t h, uint64_t lo, const uint8_t *p, uint64_t n)
{
    h = tc_mix(tc_mix(h, lo), n);
    while (n >= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        h = tc_mix(h, w);
        p += 8;
        n -= 8;
    }
    uint64_t t = 0;
    for (uint64_t i = 0; i < n; i++)
        t |= (uint64_t)p[i] << (8 * i);
    return tc_mix(h, t ^ (n << 56));
}

static int cmp_dlog(const void *a, const void *b)
{
    const __typeof__(g_tc_dlog[0]) *x = a, *y = b;
    if (x->pc != y->pc)
        return x->pc < y->pc ? -1 : 1;
    return (int)x->len - (int)y->len;
}

static int tc_deps_build(uint32_t *ndep, uint64_t *hash)
{
    if (!g_tc_ndlog)
        return 0;
    qsort(g_tc_dlog, (size_t)g_tc_ndlog, sizeof g_tc_dlog[0], cmp_dlog);
    uint32_t nd = 0, mb = 0, cur = 0;
    for (int i = 0; i < g_tc_ndlog; i++) {
        uint64_t lo = g_tc_dlog[i].pc, hi = lo + g_tc_dlog[i].len;
        const uint8_t *src = g_tc_dbytes + g_tc_dlog[i].at;
        if (nd && lo <= g_tc_mdep[nd - 1].hi) {
            TcDep *d = &g_tc_mdep[nd - 1];
            uint64_t ov = hi < d->hi ? hi : d->hi;
            for (uint64_t a = lo; a < ov; a++)
                if (g_tc_mbytes[cur + (a - d->lo)] != src[a - lo])
                    return 0;
            if (hi > d->hi) {
                memcpy(g_tc_mbytes + mb, src + (d->hi - lo), (size_t)(hi - d->hi));
                mb += (uint32_t)(hi - d->hi);
                d->hi = hi;
            }
        } else {
            cur = mb;
            g_tc_mdep[nd].lo = lo;
            g_tc_mdep[nd].hi = hi;
            nd++;
            memcpy(g_tc_mbytes + mb, src, (size_t)(hi - lo));
            mb += (uint32_t)(hi - lo);
        }
    }
    uint64_t h = TC_HASH_SEED;
    uint32_t at = 0;
    for (uint32_t i = 0; i < nd; i++) {
        uint64_t len = g_tc_mdep[i].hi - g_tc_mdep[i].lo;
        h = tc_hash_range(h, g_tc_mdep[i].lo, g_tc_mbytes + at, len);
        at += (uint32_t)len;
    }
    *ndep = nd;
    *hash = h;
    return 1;
}

static int tc_deps_check(const TcDep *d, uint32_t n, uint64_t want)
{
    volatile uint64_t h = 0;
    volatile int ok = 0;
    sigjmp_buf db;
    sigjmp_buf *prev = ocerz_jit_decode_recover;
    if (sigsetjmp(db, 0) == 0) {
        ocerz_jit_decode_recover = &db;
        uint64_t hh = TC_HASH_SEED;
        int good = 1;
        for (uint32_t i = 0; i < n && good; i++) {
            if (d[i].hi <= d[i].lo || d[i].hi - d[i].lo > TC_DBYTES_MAX)
                good = 0;
            else
                hh = tc_hash_range(hh, d[i].lo, (const uint8_t *)ocerz_g2h(d[i].lo), d[i].hi - d[i].lo);
        }
        h = hh;
        ok = good;
    }
    ocerz_jit_decode_recover = prev;
    return ok && h == want;
}

static int tc_out(const void *p, size_t n)
{
    size_t a = (n + 7) & ~(size_t)7;
    if (g_tc_opos + a > TC_OUT_MAX)
        return 0;
    if (n)
        memcpy(g_tc_out + g_tc_opos, p, n);
    memset(g_tc_out + g_tc_opos + n, 0, a - n);
    g_tc_opos += a;
    return 1;
}

static uint32_t tc_off(const JitBlock *blk, const uint32_t *p)
{
    return p ? (uint32_t)(p - (const uint32_t *)(const void *)blk->code) : TC_NONE;
}

void tc_put(OcerzJit *jit, JitBlock *blk)
{
    (void)jit;
    int n = blk->n_insns;
    int compact = blk->insns == NULL;
    if (!blk->code || !blk->insn_off || n <= 0 || (compact && !blk->iref))
        return;
    if (!g_tc_out && !(g_tc_out = (uint8_t *)malloc(TC_OUT_MAX)))
        return;
    uint32_t ndep;
    uint64_t dh;
    if (!tc_deps_build(&ndep, &dh))
        return;
    TcRec r;
    memset(&r, 0, sizeof r);
    r.h.magic = OCERZ_TC_REC_MAGIC;
    r.h.key = g_tc_key;
    r.dep_hash = dh;
    r.hoist_sig = blk->hoist_sig;
    r.code_words = blk->code_words;
    r.entry_mod = (uint32_t)((uintptr_t)(void *)blk->code & 63);
    r.n_insns = (uint32_t)n;
    r.n_kept = compact ? blk->n_kept : 0;
    r.n_rel = (uint32_t)g_tc_nrel;
    r.n_dep = ndep;
    r.n_oslow = blk->oslow ? (uint32_t)blk->n_oslow : 0;
    r.n_lanerec = blk->lanerec ? (uint32_t)blk->n_lanerec : 0;
    r.n_push_fix = blk->push_fix ? blk->n_push_fix : 0;
    r.n_pushelide = blk->pushelide ? blk->n_pushelide : 0;
    r.body_code = tc_off(blk, blk->body_code);
    r.body_noreload = tc_off(blk, blk->body_noreload);
    r.stop_patch = tc_off(blk, blk->stop_patch);
    r.stop_insn = blk->stop_insn;
    r.n_inlined = blk->n_inlined;
    r.n_slow = blk->n_slow;
    r.entry_live = blk->entry_live;
    r.xmm_pinned = blk->xmm_pinned;
    r.n_edges = blk->n_edges;
    r.n_stop_extra = blk->n_stop_extra;
    r.n_pinned = blk->n_pinned;
    r.pin_class = blk->pin_class;
    r.ordered_loads = blk->ordered_loads;
    r.flags = (uint8_t)((compact ? TCF_COMPACT : 0) | (blk->fault_flags ? TCF_FAULTF : 0) |
                        (blk->prof ? TCF_PROF : 0) | (g_tc_learned ? TCF_LEARNED : 0));
    memcpy(r.host_holds, blk->host_holds, sizeof r.host_holds);
    memcpy(r.guest_in_host, blk->guest_in_host, sizeof r.guest_in_host);

    TcStop st[6];
    for (int i = 0; i < blk->n_stop_extra && i < 6; i++) {
        st[i].off = tc_off(blk, blk->stop_extra[i].site);
        st[i].insn = blk->stop_extra[i].insn;
    }
    TcEdge ed[JIT_MAX_EDGES];
    memset(ed, 0, sizeof ed);
    for (int i = 0; i < blk->n_edges && i < JIT_MAX_EDGES; i++) {
        ed[i].target_rip = blk->edges[i].target_rip;
        ed[i].jcc_rip = blk->edges[i].jcc_rip;
        ed[i].patch_b = tc_off(blk, blk->edges[i].patch_b);
        ed[i].fallback_insn = blk->edges[i].fallback_insn;
        ed[i].cond_site = tc_off(blk, blk->edges[i].cond_site);
        ed[i].cond_orig = blk->edges[i].cond_orig;
        ed[i].kind = blk->edges[i].kind;
        ed[i].pin_class = blk->edges[i].pin_class;
        ed[i].side = blk->edges[i].side;
        ed[i].probing = blk->edges[i].probing;
    }
    TcProf pf[SIDE_MAX];
    for (int k = 0; k < SIDE_MAX && blk->prof; k++) {
        pf[k].ft_site = tc_off(blk, blk->prof[k].ft_site);
        pf[k].tk_trip = tc_off(blk, blk->prof[k].tk_trip);
    }

    g_tc_opos = 0;
    int ok = tc_out(&r, sizeof r) &&
             tc_out((const void *)blk->code, (size_t)blk->code_words * 4) &&
             tc_out(g_tc_rel, (size_t)g_tc_nrel * sizeof(TcReloc)) &&
             tc_out(g_tc_mdep, (size_t)ndep * sizeof(TcDep)) &&
             tc_out(blk->insn_off, (size_t)n * sizeof(uint32_t));
    if (ok && compact)
        ok = tc_out(blk->iref, (size_t)n * sizeof(JitInsnRef)) &&
             tc_out(blk->kept, (size_t)r.n_kept * sizeof(X86Insn));
    else if (ok)
        ok = tc_out(blk->insns, (size_t)n * sizeof(X86Insn));
    if (ok && blk->fault_flags)
        ok = tc_out(blk->fault_flags, (size_t)n * sizeof(JitFaultFlagRecipe));
    ok = ok && tc_out(blk->oslow, (size_t)r.n_oslow * sizeof *blk->oslow) &&
         tc_out(blk->lanerec, (size_t)r.n_lanerec * sizeof *blk->lanerec) &&
         tc_out(blk->push_fix, (size_t)r.n_push_fix * sizeof(uint32_t)) &&
         tc_out(blk->pushelide, (size_t)r.n_pushelide * sizeof *blk->pushelide) &&
         tc_out(st, (size_t)r.n_stop_extra * sizeof(TcStop)) &&
         tc_out(ed, (size_t)r.n_edges * sizeof(TcEdge));
    if (ok && blk->prof)
        ok = tc_out(pf, sizeof pf);
    if (!ok)
        return;
    ((TcRec *)(void *)g_tc_out)->h.size = (uint32_t)g_tc_opos;
    ocerz_tcache_put((const OcerzTcRecHead *)(const void *)g_tc_out);
    g_tc_n_put++;
}

#define TC_TAKE(dst, type, count) do { \
        size_t nb_ = (size_t)(count) * sizeof(type); \
        if (p > end || (size_t)(end - p) < nb_) return 0; \
        (dst) = (const type *)(const void *)p; \
        p += (nb_ + 7) & ~(size_t)7; \
    } while (0)

static int tc_parse(const OcerzTcRecHead *h, TcView *v)
{
    memset(v, 0, sizeof *v);
    if (h->size < sizeof(TcRec))
        return 0;
    const TcRec *r = (const TcRec *)(const void *)h;
    const uint8_t *p = (const uint8_t *)(r + 1), *end = (const uint8_t *)h + h->size;
    uint32_t words = r->code_words, n = r->n_insns;
    if (!words || words > (1u << 20) || !n || n > JIT_MAX_BLOCK_INSNS || r->n_kept > n ||
        r->n_rel > TC_RELOC_MAX || !r->n_dep || r->n_dep > TC_DLOG_MAX || r->n_edges > JIT_MAX_EDGES ||
        r->n_stop_extra > 6 || r->n_push_fix > 0xffff || r->n_pushelide > 0xffff ||
        r->n_oslow > (1u << 16) || r->n_lanerec > (1u << 16) || r->entry_mod >= 64 || (r->entry_mod & 3))
        return 0;
    v->r = r;
    TC_TAKE(v->code, uint32_t, words);
    TC_TAKE(v->rel, TcReloc, r->n_rel);
    TC_TAKE(v->dep, TcDep, r->n_dep);
    TC_TAKE(v->insn_off, uint32_t, n);
    if (r->flags & TCF_COMPACT) {
        TC_TAKE(v->iref, JitInsnRef, n);
        TC_TAKE(v->insns, X86Insn, r->n_kept);
    } else {
        TC_TAKE(v->insns, X86Insn, n);
    }
    if (r->flags & TCF_FAULTF)
        TC_TAKE(v->ff, JitFaultFlagRecipe, n);
    TC_TAKE(v->oslow, struct JitOslowMap, r->n_oslow);
    TC_TAKE(v->lanerec, struct JitLaneRec, r->n_lanerec);
    TC_TAKE(v->push_fix, uint32_t, r->n_push_fix);
    TC_TAKE(v->pushelide, struct JitPushElide, r->n_pushelide);
    TC_TAKE(v->stop, TcStop, r->n_stop_extra);
    TC_TAKE(v->edge, TcEdge, r->n_edges);
    if (r->flags & TCF_PROF)
        TC_TAKE(v->prof, TcProf, SIDE_MAX);
    for (uint32_t i = 0; i < r->n_rel; i++)
        if (v->rel[i].off + (v->rel[i].form == 1 ? 2u : 4u) > words ||
            (v->rel[i].form == 1 && (((uintptr_t)r->entry_mod + 4u * v->rel[i].off) & 7)) ||
            (v->rel[i].form == 0 && !tc_imm_shape(v->code + v->rel[i].off)))
            return 0;
    for (uint32_t i = 0; i < n; i++)
        if (v->insn_off[i] >= words)
            return 0;
    if ((r->body_code != TC_NONE && r->body_code >= words) ||
        (r->body_noreload != TC_NONE && r->body_noreload >= words) ||
        (r->stop_patch != TC_NONE && r->stop_patch >= words))
        return 0;
    for (uint32_t i = 0; i < r->n_stop_extra; i++)
        if (v->stop[i].off >= words)
            return 0;
    for (uint32_t i = 0; i < r->n_edges; i++)
        if (v->edge[i].patch_b >= words || (v->edge[i].cond_site != TC_NONE && v->edge[i].cond_site >= words))
            return 0;
    for (int k = 0; v->prof && k < SIDE_MAX; k++)
        if ((v->prof[k].ft_site != TC_NONE && v->prof[k].ft_site >= words) ||
            (v->prof[k].tk_trip != TC_NONE && v->prof[k].tk_trip >= words))
            return 0;
    return 1;
}

#undef TC_TAKE

static void tc_free_blk(JitBlock *b)
{
    if (!b)
        return;
    free(b->edges);
    free(b->insn_off);
    free(b->iref);
    free(b->kept);
    free(b->insns);
    free(b->fault_flags);
    free(b->oslow);
    free(b->lanerec);
    free(b->push_fix);
    free(b->pushelide);
    free(b->prof);
    free(b);
}

static void *tc_dup(const void *p, size_t n, int *fail)
{
    if (!n)
        return NULL;
    void *d = malloc(n);
    if (!d) { *fail = 1; return NULL; }
    memcpy(d, p, n);
    return d;
}

JitBlock *tc_load(OcerzJit *jit, const OcerzTcRecHead *h, uint64_t rip, int mode32)
{
    TcView v;
    if (!tc_parse(h, &v)) {
        g_tc_n_rej++;
        return NULL;
    }
    const TcRec *r = v.r;
    if (!tc_deps_check(v.dep, r->n_dep, r->dep_hash)) {
        g_tc_n_stale++;
        return NULL;
    }
    uint32_t n = r->n_insns, words = r->code_words;
    int fail = 0;
    JitBlock *blk = (JitBlock *)calloc(1, sizeof *blk);
    if (!blk)
        return NULL;
    blk->key = jit_key(rip, mode32);
    blk->n_insns = (int)n;
    blk->edges = calloc(JIT_MAX_EDGES, sizeof *blk->edges);
    if (!blk->edges) fail = 1;
    blk->insn_off = (uint32_t *)tc_dup(v.insn_off, n * sizeof(uint32_t), &fail);
    if (r->flags & TCF_COMPACT) {
        blk->iref = (JitInsnRef *)tc_dup(v.iref, n * sizeof(JitInsnRef), &fail);
        blk->kept = (X86Insn *)tc_dup(v.insns, r->n_kept * sizeof(X86Insn), &fail);
        blk->n_kept = (uint16_t)r->n_kept;
    } else {
        blk->insns = (X86Insn *)tc_dup(v.insns, n * sizeof(X86Insn), &fail);
    }
    if (v.ff)
        blk->fault_flags = (JitFaultFlagRecipe *)tc_dup(v.ff, n * sizeof(JitFaultFlagRecipe), &fail);
    blk->oslow = (struct JitOslowMap *)tc_dup(v.oslow, r->n_oslow * sizeof *blk->oslow, &fail);
    blk->n_oslow = blk->oslow ? (int)r->n_oslow : 0;
    blk->lanerec = (struct JitLaneRec *)tc_dup(v.lanerec, r->n_lanerec * sizeof *blk->lanerec, &fail);
    blk->n_lanerec = blk->lanerec ? (int)r->n_lanerec : 0;
    blk->push_fix = (uint32_t *)tc_dup(v.push_fix, r->n_push_fix * sizeof(uint32_t), &fail);
    blk->n_push_fix = blk->push_fix ? (uint16_t)r->n_push_fix : 0;
    blk->pushelide = (struct JitPushElide *)tc_dup(v.pushelide, r->n_pushelide * sizeof *blk->pushelide, &fail);
    blk->n_pushelide = blk->pushelide ? (uint16_t)r->n_pushelide : 0;
    if (v.prof) {
        blk->prof = (JitProf *)calloc(SIDE_MAX, sizeof(JitProf));
        if (!blk->prof) fail = 1;
    }
    if (fail) {
        tc_free_blk(blk);
        return NULL;
    }

    pthread_jit_write_protect_np(0);
    if (!ENV_ON("OCERZ_NO_DISPATCH_STUB")) {
        if (mode32) { if (!jit->dispatch_stub32) emit_dispatch_stub(jit, 1); }
        else        { if (!jit->dispatch_stub)   emit_dispatch_stub(jit, 0); }
    }
    veneer_pool_check(jit);
    uint8_t *pp = (uint8_t *)jit->code_cur;
    uint32_t *c = (uint32_t *)(void *)(pp + (((uintptr_t)r->entry_mod - (uintptr_t)pp) & 63));
    if (jit->code_full || (uint8_t *)(c + words) > (uint8_t *)jit->code_end) {
        pthread_jit_write_protect_np(1);
        tc_free_blk(blk);
        return NULL;
    }
    memcpy(c, v.code, (size_t)words * 4);
    if (!tc_bind(jit, blk, c, v.rel, (int)r->n_rel, 1)) {
        pthread_jit_write_protect_np(1);
        tc_free_blk(blk);
        return NULL;
    }
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(c, (size_t)words * 4);
    jit->code_cur = c + words;

#define TC_AT(o) ((o) == TC_NONE ? NULL : c + (o))
    blk->code = (JitBlockFn)(void *)c;
    blk->code_words = words;
    blk->body_code = TC_AT(r->body_code);
    blk->body_noreload = TC_AT(r->body_noreload);
    blk->hoist_sig = r->hoist_sig;
    blk->ordered_loads = r->ordered_loads;
    blk->stop_patch = TC_AT(r->stop_patch);
    blk->stop_insn = r->stop_insn;
    blk->n_stop_extra = r->n_stop_extra;
    for (uint32_t i = 0; i < r->n_stop_extra; i++) {
        blk->stop_extra[i].site = c + v.stop[i].off;
        blk->stop_extra[i].insn = v.stop[i].insn;
    }
    blk->n_edges = r->n_edges;
    for (uint32_t i = 0; i < r->n_edges; i++) {
        blk->edges[i].target_rip = v.edge[i].target_rip;
        blk->edges[i].jcc_rip = v.edge[i].jcc_rip;
        blk->edges[i].patch_b = c + v.edge[i].patch_b;
        blk->edges[i].fallback_insn = v.edge[i].fallback_insn;
        blk->edges[i].cond_site = TC_AT(v.edge[i].cond_site);
        blk->edges[i].cond_orig = v.edge[i].cond_orig;
        blk->edges[i].kind = v.edge[i].kind;
        blk->edges[i].pin_class = v.edge[i].pin_class;
        blk->edges[i].side = v.edge[i].side;
        blk->edges[i].probing = v.edge[i].probing;
    }
    for (int k = 0; v.prof && k < SIDE_MAX; k++) {
        blk->prof[k].ft_site = TC_AT(v.prof[k].ft_site);
        blk->prof[k].tk_trip = TC_AT(v.prof[k].tk_trip);
    }
#undef TC_AT
    blk->n_inlined = r->n_inlined;
    blk->n_slow = r->n_slow;
    blk->entry_live = r->entry_live;
    blk->xmm_pinned = r->xmm_pinned;
    blk->n_pinned = r->n_pinned;
    blk->pin_class = r->pin_class;
    memcpy(blk->host_holds, r->host_holds, sizeof blk->host_holds);
    memcpy(blk->guest_in_host, r->guest_in_host, sizeof blk->guest_in_host);
    if (blk->stop_patch || blk->n_stop_extra) {
        blk->stop_next = jit->stop_blocks;
        jit->stop_blocks = blk;
    }
    if (!code_index_append_locked(jit, blk)) {
        blk->code = NULL;
        return NULL;
    }
    cache_insert(jit, blk);
    blk_chain_install(jit, blk);
    g_tc_n_load++;
    return blk;
}

void tc_verify(OcerzJit *jit, JitBlock *blk, const OcerzTcRecHead *h)
{
    TcView v;
    if (!tc_parse(h, &v)) {
        g_tc_n_rej++;
        return;
    }
    const TcRec *r = v.r;
    if (!tc_deps_check(v.dep, r->n_dep, r->dep_hash)) {
        g_tc_n_stale++;
        tc_put(jit, blk);
        return;
    }
    const uint32_t *code = (const uint32_t *)(const void *)blk->code;
    const char *why = NULL;
    uint32_t at = 0;
    if (r->entry_mod != (uint32_t)((uintptr_t)code & 63))
        why = "align";
    else if (r->code_words != blk->code_words)
        why = "words";
    else if ((int)r->n_rel != g_tc_nrel)
        why = "nrel";
    uint32_t rel_i = 0;
    for (uint32_t i = 0; !why && i < r->n_rel; i++)
        if (v.rel[i].off != g_tc_rel[i].off || v.rel[i].kind != g_tc_rel[i].kind ||
            v.rel[i].form != g_tc_rel[i].form || v.rel[i].arg != g_tc_rel[i].arg) {
            why = "rel";
            at = v.rel[i].off;
            rel_i = i;
        }
    for (uint32_t w = 0; !why && w < r->code_words; w++)
        if (code[w] != v.code[w] && !tc_is_reloc_word(w)) {
            why = "code";
            at = w;
        }
    if (!why && ((int)r->n_insns != blk->n_insns || r->n_edges != blk->n_edges ||
                 r->pin_class != blk->pin_class || r->n_pinned != blk->n_pinned ||
                 r->entry_live != blk->entry_live || r->xmm_pinned != blk->xmm_pinned ||
                 r->hoist_sig != blk->hoist_sig || r->ordered_loads != blk->ordered_loads ||
                 r->body_code != tc_off(blk, blk->body_code) ||
                 r->body_noreload != tc_off(blk, blk->body_noreload) ||
                 r->stop_patch != tc_off(blk, blk->stop_patch) || r->n_stop_extra != blk->n_stop_extra))
        why = "meta";
    for (uint32_t i = 0; !why && i < r->n_insns; i++)
        if (v.insn_off[i] != blk->insn_off[i]) {
            why = "insn_off";
            at = i;
        }
    for (uint32_t i = 0; !why && i < r->n_edges; i++)
        if (v.edge[i].target_rip != blk->edges[i].target_rip ||
            v.edge[i].patch_b != tc_off(blk, blk->edges[i].patch_b) ||
            v.edge[i].cond_site != tc_off(blk, blk->edges[i].cond_site) ||
            v.edge[i].fallback_insn != blk->edges[i].fallback_insn || v.edge[i].kind != blk->edges[i].kind) {
            why = "edge";
            at = i;
        }
    if (!why) {
        g_tc_n_vok++;
        return;
    }
    if (r->code_words != blk->code_words || (int)r->n_insns != blk->n_insns ||
        (r->flags & TCF_LEARNED) || g_tc_learned) {
        g_tc_n_vvar++;
        return;
    }
    g_tc_n_vbad++;
    static int nrep;
    if (!g_tc_log || __atomic_fetch_add(&nrep, 1, __ATOMIC_RELAXED) >= 60)
        return;
    int ii = !strcmp(why, "code") || !strcmp(why, "rel") ? tc_insn_at(blk, at) : -1;
    uint64_t irip = ii >= 0 ? (blk->insns ? blk->insns[ii].rip : blk->iref ? blk->iref[ii].rip : 0) : 0;
    fprintf(g_tc_lf, "ocerz: TCACHE[%d] VERIFY %s rip=%#llx at=%u insn=%d insn_rip=%#llx now=%08x rec=%08x"
                     " words=%u/%u insns=%d/%u\n",
            (int)getpid(), why, (unsigned long long)blk_rip(blk), at, ii, (unsigned long long)irip,
            !strcmp(why, "code") ? code[at] : 0u, !strcmp(why, "code") ? v.code[at] : 0u,
            blk->code_words, r->code_words, blk->n_insns, r->n_insns);
    if (!strcmp(why, "meta"))
        fprintf(g_tc_lf, "ocerz: TCACHE[%d]   meta now/rec edges=%u/%u pin_class=%u/%u pinned=%u/%u live=%#x/%#x"
                         " xmm=%#x/%#x hoist=%#llx/%#llx ordered=%u/%u body=%u/%u noreload=%u/%u stop=%u/%u extra=%u/%u\n",
                (int)getpid(), blk->n_edges, r->n_edges, blk->pin_class, r->pin_class, blk->n_pinned,
                r->n_pinned, blk->entry_live, r->entry_live, blk->xmm_pinned, r->xmm_pinned,
                (unsigned long long)blk->hoist_sig, (unsigned long long)r->hoist_sig, blk->ordered_loads,
                r->ordered_loads, tc_off(blk, blk->body_code), r->body_code, tc_off(blk, blk->body_noreload),
                r->body_noreload, tc_off(blk, blk->stop_patch), r->stop_patch, blk->n_stop_extra,
                r->n_stop_extra);
    if (!strcmp(why, "rel"))
        fprintf(g_tc_lf, "ocerz: TCACHE[%d]   rel %u now off=%u kind=%u form=%u arg=%#llx"
                         " rec off=%u kind=%u form=%u arg=%#llx\n",
                (int)getpid(), rel_i, g_tc_rel[rel_i].off, g_tc_rel[rel_i].kind, g_tc_rel[rel_i].form,
                (unsigned long long)g_tc_rel[rel_i].arg, v.rel[rel_i].off, v.rel[rel_i].kind,
                v.rel[rel_i].form, (unsigned long long)v.rel[rel_i].arg);
    if (nrep > 4)
        return;
    for (int side = 0; side < 2; side++) {
        const uint32_t *cw = side ? v.code : code;
        const uint32_t *io = side ? v.insn_off : blk->insn_off;
        uint32_t nw = side ? r->code_words : blk->code_words;
        int ni = side ? (int)r->n_insns : blk->n_insns;
        fprintf(g_tc_lf, "ocerz: TCACHE[%d]   %s insn_off:", (int)getpid(), side ? "rec" : "now");
        for (int i = 0; i < ni; i++)
            fprintf(g_tc_lf, " %u", io[i]);
        for (uint32_t w = 0; w < nw; w++)
            fprintf(g_tc_lf, "%s%08x", w % 8 ? " " : "\nocerz:     ", cw[w]);
        fprintf(g_tc_lf, "\n");
    }
}

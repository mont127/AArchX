/*
 * ---- bridged calls ----
 * In native mode a guest call into a system library lands on a synthesized
 * stub, `mov r11d, <export id>` then `jmp qword [rip + slot]`, whose slot holds
 * the one trap address every export shares (vdylib.h).  Taken as an ordinary
 * indirect jump, that address has no translation: the block leaves, the run
 * loop finds the rip in the trap window, the interpreter's trap check hands it
 * to the bridge, and the return address comes back in through the dispatcher
 * with the host shadow stack abandoned, so the caller's own ret leaves once
 * more.  When a block ends in that pair and the slot holds the trap address as
 * the block is translated, the jump becomes a call to ocerz_vdylib_fastcall made
 * from inside the block, followed by the ret the export's own code would have
 * ended with.
 *
 * The emitted code loads the slot again and compares it with the trap address,
 * so a guest that rewrote its slot takes the plain indirect jump emitted right
 * after.  It also measures the host stack below the frame base and takes the
 * plain jump past 64 KB, because the native call now runs underneath the shadow
 * of every guest call still open, where the trap path ran it from the top of
 * the run loop.  Then every guest register is spilled, the helper performs the
 * crossing the trap would have performed, the registers are filled again, and a
 * zero answer runs the ret: the shadow's return address is compared with the
 * rip the crossing left, and a match returns into the caller's continuation
 * with a real ret, which keeps the return predictor aligned.  Any other answer
 * is the step code plus one and leaves through the epilogue.  The helper
 * answers zero only when the crossing returned the way a function does - rip
 * equal to the word just popped, rsp eight or sixteen higher - with no exit,
 * interrupt, suspension or interp_once pending and no translation retired
 * while it ran, since a retired continuation may be translated from bytes the
 * crossing just rewrote.  A delivered signal fails the first test.
 *
 * A host-stack RAS entry lives only until the next trip out of translated
 * code, because every exit resets sp to the frame base, and a return whose
 * entry was lost that way used to leave the frame through the epilogue: the
 * dispatcher, the prologue, and the reset discarding every remaining entry, so
 * the whole call chain then missed on its way up.  C++ engine code makes a call
 * every ten instructions and takes a trip every few hundred thousand blocks,
 * which put Adobe AIR's main thread at 36 million misses a second, one return
 * in seven.  A miss now stays in the body: the popped return address goes to
 * the same per-site cache and inline hash probe an indirect call uses, and only
 * an address no block answers to leaves the frame.  A pop that finds the
 * sentinel pair pushes it back first, since the saved frame registers sit right
 * above it.  Brawlhalla's translated throughput rose by a third; the kernels the
 * host RAS was built for (memcpy, str, leafcall, icall) measure the same.
 * OCERZ_PERFSTAT splits misses into an empty stack, a null entry and a
 * mismatched address and names the ret sites that miss most.  On R.E.P.O.'s
 * menu 183.6 of 184.1 million misses were the empty stack, which costs a cache
 * probe rather than a trip out.
 *
 * A chain is a b patched into the exiting block, and a b reaches 128 MB.  The
 * arena is 1 GB and filled front to back, so a caller translated late chains
 * to a callee translated early only if fewer than 128 MB of code came between
 * them; past that the patch was dropped and the edge left through the epilogue
 * on every execution.  That is the shape of a game: the engine's runtime
 * helpers are translated in the first minute and the script code that calls
 * them keeps being generated afterwards, so its hottest edges were exactly the
 * ones dropped.  A 256 KB veneer pool is carved out of the arena every 64 MB,
 * and a patch that does not reach is pointed at a veneer in the nearest pool
 * instead: ldr x15 from the literal that follows, br x15, which reaches
 * anything.  x15 is the computed-address temp, never live across a block edge.
 * Brawlhalla's main thread fell from a saturated core to 71%, while doing more
 * work per second than before.
 *
 * The same distance limit applies to an alignment hotpatch, which puts a
 * branch at the faulting access and a stub at the end of the arena.  When the
 * end is out of reach the stub goes into the nearest veneer pool instead, so
 * the patch never falls back to retranslating the block.  A store whose data
 * register is 31 is patched as well: in store and shift encodings that is the
 * zero register, and it is how a guest store of an immediate zero comes out.
 * Refusing both sent Discord's V8 through 5,391 retranslations in its first
 * ten seconds, most of them the same few stores; it now takes none.
 *
 * A first commpage or alignment fault in a block retires only that block, and
 * a block starting at the faulting instruction, through the predecessor lists
 * the branch-flip retire already trusts.  A range invalidation scans every
 * live block and every chain edge, about 140,000 blocks at Electron's startup,
 * and one per fault made retiring the largest cost on Discord's main thread.
 * A repeat fault still takes the range invalidation, which feeds the churn
 * accounting, and OCERZ_FAULT_INV_RANGE=1 restores it for the first fault too.
 *
 * OCERZ_FPS=1 counts frames in cache mode: the block that begins at the Intel
 * cache's CGLFlushDrawable gets four instructions at its head that increment a
 * counter, and a thread prints the rate to stderr once a second.  The guest
 * sees nothing of it.  It is how a game's frame rate under ocerz is measured
 * when Steam's overlay counter, which relies on dyld interposing, is not there.
 *
 * OCERZ_TRIPSTAT=1 counts the times translated code leaves to the dispatcher,
 * which ocerz_jit_step sees once each, and samples one in 64 of their guest
 * destinations; every ten seconds a thread prints trips per second and the
 * twenty commonest destinations with the state of the block at each.  At
 * Brawlhalla's menu it showed 6.6 million trips a second, ten destinations
 * making 92% of them.  It costs one atomic increment per trip.
 *
 * A memory access whose address is not a known stack slot pays a commpage
 * guard before it - the commpage lives elsewhere on the host, so the address
 * is compared against that range and redirected if it falls inside - and, in
 * ordered mode, a granule test before the ldapr or stlr, since those fault on
 * an access that crosses a granule.  For a rip-relative or absolute operand the
 * address is a constant of the translation, so both questions are answered
 * when the block is built: the guard is skipped or becomes the plain
 * redirect, and an aligned constant address goes straight to the ordered
 * load or store.  A global load or store costs four host words instead of
 * fourteen in plain mode and eighteen in ordered mode; it is the most common
 * form there is in compiled code.  The known-address note is set by the guard
 * and consumed by the very next ordered access, and building any other
 * effective address clears it, so it can never describe an address other than
 * the one just guarded.
 *
 * Eleven libSystem exports do not cross at all.  strlen, strnlen, strcmp,
 * strncmp, memcmp, bcmp, strchr, memchr, memcpy, memmove and memset are called
 * constantly, do very little, and cost several times their own work to reach
 * through a crossing, so src/leaf.s holds arm64 versions that read rdi, rsi and
 * rdx from the registers full pinning keeps them in and leave rax in its own
 * (ocerz_vdylib_leaf names the routine for an export id).  The block checks its
 * slot as before, branches to the routine with nothing spilled, pops the return
 * address and runs the same ret.  A routine that writes guest memory is only
 * used for lengths up to the limit the lookup gives, may decline in x9, and has
 * the retire count read before and after it, a change sending the return through
 * the dispatcher for the reason the helper's own check gives; any of the three
 * falls through to the ordinary fast call emitted next.  A fault inside a
 * routine is attributed to the branch that reached it (ocerz_leaf_site), so the
 * guest sees a fault at the stub with its registers exact, where a fault inside
 * a crossing ends the process.  The routines are reached through a copy made
 * at the start of the code arena, so the branch is a direct one.
 *
 * Cache mode uses the same routines from the other side.  There the guest's
 * strlen is Apple's x86 code, and when a block starts at the exported entry of
 * one of ten of those functions (ocerz_dyldapi_leaf_entry) the same call and
 * ret are emitted ahead of its first instruction, with the translation of the
 * x86 code following as what runs when the routine declines.  No length limit
 * applies, since what it would fall back to is slower at every length.  A
 * fault inside a routine is not delivered from there: src/vm.c makes the
 * routine decline, and the x86 code takes the same fault with every detail
 * right.  A block with a branch back to its own start is left alone, since
 * its lanes may be live where the call would leave.
 * OCERZ_NO_LEAF_INPLACE=1 turns the routines off in both modes, and so do
 * OCERZ_BRIDGESTAT and OCERZ_BRIDGELOG, whose counts and lines come from the
 * crossing.
 *
 * XMM registers are spilled by contract rather than wholesale.  System V makes
 * every xmm register volatile across a call, so for an ordinary export only the
 * argument registers its signature reads are stored first and only the result
 * registers are loaded afterwards; the rest keep whatever the host left in
 * their pinned registers, which an x86 callee was equally free to leave.
 * __tlv_bootstrap and ___chkstk_darwin promise every register back, and so
 * does an export whose record gives no usable signature, and those store and
 * load all sixteen.  A crossing that leaves loads all sixteen from the cpu,
 * where a signal frame may have replaced them.  OCERZ_NO_BRIDGE_FASTCALL=1
 * keeps the trap path everywhere.  fork and vfork keep it always
 * (ocerz_vdylib_trap_only): the fork child starts without the parent's
 * translations, and a crossing made from inside a block would return into code
 * the child no longer has, so their stubs leave the block and trap from the
 * dispatcher, the way the fork syscall does.
 *
 *
 * The Wine layout's slot is rsp - 8 plus the stack delta in x0, as push's; rsp moves once the store is done.
 *
 * call $+5 is how 32-bit code reads eip; its pop never meets a ret.
 *
 * The return address goes through the same per-site cache and hash probe
 * an indirect jump does, keyed as a 32-bit block, instead of out to the
 * dispatcher on every return; first the host shadow, when the call that
 * pushed it went through m32_ras_push.
 *
 * The sentinel pair sits on the frame's saved registers: put it back.
 *
 * The interpreter side of an x87 run: instructions idx..last, stopping at the first that does not step.
 *
 * A 32-bit call and its ret through the host shadow stack, as 64-bit ones go:
 * the call pushes {return address tagged JIT_KEY_M32, host continuation} and
 * bl's (or, indirect, blr's) into the callee's body, and the ret compares the
 * address it popped with the shadow's and returns with a real ret, which the
 * return predictor has seen coming.  The tag keeps a 64-bit ret from taking a
 * 32-bit entry and the reverse.  A mismatch, a stale entry or an empty shadow
 * takes the per-site cache as before.  OCERZ_NO_M32_RAS=1 turns it off.
 *
 * Pushes the shadow pair for a 32-bit call returning to retaddr; the adr is patched to the continuation.
 *
 * Set for one emit_indirect_tail whose target register holds a 32-bit block's key.
 *
 * A 32-bit indirect call or jump: the target, a register or a dword in memory,
 * is zero-extended into JT1 and keyed as a 32-bit block, a call pushes its
 * return address as emit_call_ret32 does, and both leave through the per-site
 * cache and hash probe.  32-bit Windows code calls every import as call dword
 * [IAT], so each of those went out to the dispatcher before.  The target is
 * read before anything moves, so a fault there leaves the instruction unstarted.
 */
#include "ocerz/jit_internal.h"

static void stop_extra_add(uint32_t *site, uint32_t *target);
static void ps_note_shape(const X86Insn *insn);
static __attribute__((noinline, cold, preserve_most)) void jit_trace_one(const X86Insn *insn);
static __attribute__((noinline, cold, preserve_most)) void jit_perfstat_one(const X86Insn *insn);
static void emit_stack_push64(A64Buf *b, const X86Insn *insn, int hs, int rv);
static void emit_stack_pop64(A64Buf *b, const X86Insn *insn, int hs, int rd);
static void a64_and_imm_or_mov(A64Buf *b, int sf, int rd, int rn, uint64_t imm);
static int jmp_inline_enabled(void);
static int callret_inline_enabled(void);
static void *fps_report(void *arg);
static void patch_local_adr(uint32_t *at, uint32_t *target, int rd);
static void emit_step_epilogue_branch(A64Buf *b, uint32_t **epi_sites, int *n_epi);
static int emit_call_region_call(A64Buf *b, const X86Insn *insn,
                                 uint32_t **exit_sites, int *n_exits,
                                 uint32_t **epi_sites, int *n_epi);
static int emit_call_region_ret(A64Buf *b, const X86Insn *insn,
                                uint32_t **exit_sites, int *n_exits,
                                uint32_t **epi_sites, int *n_epi);
static int m32_ras_ok(const X86Insn *insn);
static uint32_t *m32_ras_push(A64Buf *b, uint64_t retaddr);
static int emit_call_ret32(A64Buf *b, const X86Insn *insn,
                           uint32_t **epi_sites, int *n_epi);
static void emit_indirect_leave_br(A64Buf *b, int code_reg);
static void emit_indirect_tail(A64Buf *b, uint32_t **epi_sites, int *n_epi);
static int emit_branch_target(A64Buf *b, const X86Insn *insn, const X86Operand *o,
                              uint32_t **exit_sites, int *n_exits);
static int emit_indirect32(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites,
                           int *n_exits, uint32_t **epi_sites, int *n_epi);
static int bridge_fastcall_enabled(void);
static int leaf_inplace_enabled(void);

int g_no_ras;

int g_ea_plain;

struct JitPushElide g_pe_real[PE_MAX];

int g_n_pe_real;

struct JitPromo g_promo_real[PE_MAX];

int g_n_promo_real;

const X86Insn *g_pe_insns;

unsigned long long g_callout_seq;

int g_no_chain;

uint64_t g_chain_target;

uint32_t *g_chain_epi;

int g_chain_keeps_jgb;

void emit_const_lit(A64Buf *b, int rd, uint64_t v)
{
    int parts = 0;
    for (int k = 0; k < 4; k++) parts += ((v >> (16 * k)) & 0xffff) != 0;
    static int off = -1; if (off < 0) off = getenv("OCERZ_NO_CONST_LIT") ? 1 : 0;
    if (parts < 3 || g_n_raslit >= RASLIT_MAX || off) { a64_mov_imm64(b, rd, v); return; }
    g_raslit[g_n_raslit].site = a64_label(b);
    g_raslit[g_n_raslit].retaddr = v;
    g_raslit[g_n_raslit].kind = 1;
    g_raslit[g_n_raslit].tcr = 0;
    g_raslit[g_n_raslit].rt = rd;
    g_n_raslit++;
    a64_emit32(b, 0x58000000u | (uint32_t)(rd & 31));
}

uint32_t *g_body_entry;

uint32_t g_push_fix[JIT_MAX_BLOCK_INSNS];

int g_n_push_fix;

const uint32_t *g_push_entry;

struct JitState_g_stop_extra g_stop_extra[6];

int g_n_stop_extra;

static void stop_extra_add(uint32_t *site, uint32_t *target)
{
    if (g_n_stop_extra < 6) { g_stop_extra[g_n_stop_extra].site = site; g_stop_extra[g_n_stop_extra].target = target; g_n_stop_extra++; }
}

uint32_t stop_retarget(uint32_t insn, const uint32_t *site,
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

struct JitState_g_call_edge g_call_edge[2];

int g_n_call_edges;

_Atomic unsigned long long ps_ops[OCERZ_OP_COUNT];

char ps_shapes[OCERZ_OP_COUNT][3][96] = {0};

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

_Atomic unsigned long long ps_slow_insns;

_Atomic unsigned long long ps_shape[9][2];

unsigned long long ps_ras_miss;

unsigned long long ps_ras_null;

unsigned long long ps_ras_sentinel;

unsigned long long ps_ras_stale;

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

__attribute__((noinline))
int ocerz_jit_exec_one(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    return jit_exec_one_parked(vm, cpu, insn, __builtin_return_address(0));
}

int jit_exec_one(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
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
    uint64_t prev_op = cpu->slow_op;
    uint64_t form = (uint64_t)insn->op | (uint64_t)(insn->vex & 3) << 16 |
                    (uint64_t)(insn->ops[0].kind & 7) << 18 | (uint64_t)(insn->ops[1].kind & 7) << 21 |
                    (uint64_t)(insn->ops[2].kind & 7) << 24 | (uint64_t)insn->opsize << 32;
    for (int i = 0; i < insn->nops && i < 3; i++)
        if (insn->ops[i].kind == OCERZ_OPK_IMM)
            form |= (uint64_t)(insn->ops[i].imm & 0xff) << 40 | 1ull << 48;
    if (insn->op == OCERZ_OP_SYSCALL)
        form = (uint64_t)insn->op | (uint64_t)(uint32_t)cpu->gpr[OCERZ_RAX] << 32;
    cpu->slow_op = form;
    int r = ocerz_interp_exec(vm, cpu, insn);
    cpu->slow_op = prev_op;
    cpu->rip &= cpu->mode32 ? 0xffffffffull : ~0ull;
    ocerz_jit_exec_state = prev;
    return r;
}

__attribute__((noinline))
int ocerz_jit_exec_one_at(struct OcerzVM *vm, OcerzCPU *cpu, const JitBlock *b, uint64_t idx)
{
    return jit_exec_one_parked(vm, cpu, blk_insn_full(b, (int)idx), __builtin_return_address(0));
}

__attribute__((noinline))
int ocerz_jit_exec_run_at(struct OcerzVM *vm, OcerzCPU *cpu, const JitBlock *b, uint64_t idx, uint64_t last)
{
    for (;; idx++) {
        int r = jit_exec_one(vm, cpu, blk_insn_full(b, (int)idx));
        if (r != OCERZ_STEP_OK || idx >= last)
            return r;
    }
}

int g_no_compact;

int host_ras_enabled(void)
{
    static int en = -1;
    if (en < 0) en = getenv("OCERZ_NO_HOST_RAS") ? 0 : 1;
    return en;
}

void emit_push_pinned(A64Buf *b, int hs, int rv)
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

int low_stack_fast(void)
{
    static int off = -1;
    if (off < 0) off = getenv("OCERZ_NO_LOW_RAS") ? 1 : 0;
    return !off && ocerz_low_base != 0 && ocerz_guest_base == 0 && stack_plain_access_ok() && rsp_is_ptr();
}

static void emit_stack_push64(A64Buf *b, const X86Insn *insn, int hs, int rv)
{
    if (!stack_guard_needed()) {
        emit_push_pinned(b, hs, rv);
        return;
    }

    if (g_lowstack && stack_plain_access_ok() && rv != JTA && !ENV_ON("OCERZ_NO_LOWSTACK_CALLRET")) {
        a64_sub_imm(b, 1, JTA, hs, 8);
        a64_str_regoff(b, 8, rv, JTA, JGB, 0);
        a64_mov_reg(b, 1, hs, JTA);
        return;
    }
    a64_sub_imm(b, 1, JTA, hs, 8);
    uint32_t *skip = emit_commpage_guard(b, insn, JTA, NULL, NULL);
    g_ea_plain = stack_plain_now();
    emit_guest_store_ordered(b, 8, rv, JTA, JTU);
    if (skip) a64_patch_b(skip, a64_label(b));
    a64_sub_imm(b, 1, hs, hs, 8);
}

static void emit_stack_pop64(A64Buf *b, const X86Insn *insn, int hs, int rd)
{
    if (!stack_guard_needed()) {
        if (stack_identity() || rsp_is_ptr()) {
            a64_ldr_post64(b, rd, hs, 8);
        } else {
            a64_ldr_regoff(b, 8, rd, JGB, hs, 0);
            a64_add_imm(b, 1, hs, hs, 8);
        }
        return;
    }
    if (g_lowstack && stack_plain_access_ok() && rd != hs && !ENV_ON("OCERZ_NO_LOWSTACK_CALLRET")) {
        a64_ldr_regoff(b, 8, rd, hs, JGB, 0);
        a64_add_imm(b, 1, hs, hs, 8);
        return;
    }
    a64_mov_reg(b, 1, JTA, hs);
    uint32_t *skip = emit_commpage_guard(b, insn, JTA, NULL, NULL);
    g_ea_plain = stack_plain_now();
    emit_guest_load_ordered(b, 8, rd, JTA, JTU);
    if (skip) a64_patch_b(skip, a64_label(b));
    a64_add_imm(b, 1, hs, hs, 8);
}

static void a64_and_imm_or_mov(A64Buf *b, int sf, int rd, int rn, uint64_t imm)
{
    if (!a64_try_and_imm(b, sf, rd, rn, imm)) { a64_mov_imm64(b, JTT, imm); a64_and_reg(b, sf, rd, rn, JTT, 0); }
}

int g_slow_run_last = -1;

static int jmp_inline_enabled(void)
{
    static int en = -1;
    if (en < 0)
        en = getenv("OCERZ_NO_INLINE_JMP") ? 0 : 1;
    return en;
}

int emit_jmp(A64Buf *b, const X86Insn *insn, uint32_t **epilogue_sites, int *n_epi)
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
        l0_fixed_fallthrough(b);
        fpb_emit_exit_check(b);
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

uint64_t g_fps_frames;

static uint64_t g_fps_start;

static void *fps_report(void *arg)
{
    uint64_t last = __atomic_load_n(&g_fps_frames, __ATOMIC_RELAXED);
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    g_fps_start = t0;
    for (;;) {
        usleep(1000000);
        uint64_t now = __atomic_load_n(&g_fps_frames, __ATOMIC_RELAXED);
        uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (now != last)
            fprintf(stderr, "ocerz: FPS[%d] %.1f t=%.1f\n", (int)getpid(),
                    (double)(now - last) * 1e9 / (double)(t1 - t0),
                    (double)(t1 - g_fps_start) / 1e9);
        last = now;
        t0 = t1;
    }
    return arg;
}

int fps_watch(uint64_t rip)
{
    static int state = -1;
    static uint64_t target;
    if (state < 0) {
        state = 0;
        if (getenv("OCERZ_FPS") && ocerz_mode == OCERZ_MODE_CACHE) {
            target = ocerz_dyld_resolve_guest_sym("_CGLFlushDrawable");
            pthread_t t;
            if (target && pthread_create(&t, NULL, fps_report, NULL) == 0) {
                pthread_detach(t);
                state = 1;
            }
        }
    }
    return state == 1 && rip == target;
}

static uint32_t **g_ind_call_cont;

static int g_ind_treg;

static int g_ind_treg;

static int g_ind_m32;

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
        g_ea_plain = stack_plain_now();
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
        g_ea_plain = stack_plain_now();
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

static int m32_ras_ok(const X86Insn *insn)
{
    static int no_blret = -1;
    if (no_blret < 0) no_blret = getenv("OCERZ_NO_BLRET") ? 1 : 0;
    return g_pin_class == 3 && fullpin_enabled() && !g_no_regflags && !g_no_chain && !g_no_ras &&
           host_ras_enabled() && !no_blret && !ENV_ON("OCERZ_NO_M32_RAS") && m32_stack_ok(insn);
}

static uint32_t *m32_ras_push(A64Buf *b, uint64_t retaddr)
{
    a64_mov_imm64(b, JT2, retaddr | JIT_KEY_M32);
    uint32_t *adr_site = a64_label(b);
    a64_emit32(b, 0x10000000u | (uint32_t)JT0);
    a64_stp_pre(b, JT2, JT0, 31, -16);
    return adr_site;
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


        if (target != retaddr && m32_ras_ok(insn)) {
            uint32_t *adr_site = m32_ras_push(b, retaddr);
            a64_mov_imm64(b, JT1, retaddr);
            a64_sub_imm(b, 0, JTA, hs, 4);
            m32_stack_st(b, JT1, JTA);
            a64_mov_reg(b, 0, hs, JTA);
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

        a64_mov_imm64(b, JT1, retaddr);
        a64_sub_imm(b, 0, JTA, hs, 4);
        m32_stack_st(b, JT1, JTA);
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
        m32_stack_ld(b, JT1, hs);
        a64_add_imm(b, 0, hs, hs, pop);






        if (!g_no_chain && !ENV_ON("OCERZ_NO_M32_RET_TAIL")) {
            (void)a64_try_orr_imm(b, 1, JT1, JT1, JIT_KEY_M32);
            if (m32_ras_ok(insn)) {
                a64_ldp_post(b, JTF, 30, 31, 16);
                a64_subs_reg(b, 1, A64_ZR, JTF, JT1, 0);
                uint32_t *stale = a64_label(b);
                a64_bcond(b, A64_NE, 0);
                uint32_t *null = a64_label(b);
                a64_cbz(b, 1, 30, 0);
                if (!xmm_global_enabled()) emit_xmm_pin_spill_all(b);
                a64_ret(b);
                a64_patch_bcond(stale, a64_label(b));
                a64_patch_cbz(null, a64_label(b));

                uint32_t *keep = a64_label(b);
                a64_cbnz(b, 1, JTF, 0);
                a64_sub_imm(b, 1, 31, 31, 16);
                a64_patch_cbz(keep, a64_label(b));
                if (!xmm_global_enabled()) emit_xmm_pin_spill_all(b);
            }
            g_ind_treg = JT1;
            g_ind_m32 = 1;
            emit_indirect_tail(b, epi_sites, n_epi);
            return 1;
        }
        a64_str(b, 8, JT1, 20, RIP_OFF);
        a64_mov_imm64(b, 0, OCERZ_STEP_OK);
        epi_sites[*n_epi] = a64_label(b);
        a64_b(b, 0);
        (*n_epi)++;
        return 1;
    }
    return 0;
}

int emit_call_ret(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites,
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

        emit_const_lit(b, JT1, retaddr);

        int fast3 = g_pin_class == 3 && pin_slot(OCERZ_RSP) >= 0 && stack_plain_access_ok() &&
                    stack_fast() && !g_no_chain;
        static int no_blret = -1;
        if (no_blret < 0) no_blret = getenv("OCERZ_NO_BLRET") ? 1 : 0;
        if (fast3 && ras_body_only() && !g_no_ras && !no_blret) {
            int hs = pin_hreg(pin_slot(OCERZ_RSP));
            uint32_t *adr_site;
            if (host_ras_enabled()) {
                adr_site = a64_label(b);
                a64_emit32(b, 0x10000000u | (uint32_t)JT0);
                a64_stp_pre(b, JT1, JT0, 31, -16);
                emit_stack_push64(b, insn, hs, JT1);
            } else {
                emit_stack_push64(b, insn, hs, JT1);
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
            emit_stack_push64(b, insn, hs, JT1);
        } else {
            emit_gpr_rd(b, 1, JT0, OCERZ_RSP);
            a64_sub_imm(b, 1, JTA, JT0, 8);
            emit_add_const(b, JTA, ea_fold());

            uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
            emit_add_const(b, JTA, gbase - ea_fold());

            g_ea_plain = stack_plain_now();
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
                    tc_imm64(b, JTA, TCR_RASSLOT, retaddr, (uint64_t)(uintptr_t)slot);
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
                tc_imm64(b, 16, TCR_SYM, TCS_RAS_PUSH, (uint64_t)(uintptr_t)&ocerz_ras_push);
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
                    stack_fast();
        if (fast3) {
            int hs = pin_hreg(pin_slot(OCERZ_RSP));
            emit_stack_pop64(b, insn, hs, JT1);
            if (!ras_body_only()) a64_str(b, 8, JT1, 20, RIP_OFF);
        } else {
            emit_gpr_rd(b, 1, JT0, OCERZ_RSP);
            a64_mov_reg(b, 1, JTA, JT0);
            emit_add_const(b, JTA, ea_fold());

            uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
            emit_add_const(b, JTA, gbase - ea_fold());
            g_ea_plain = stack_plain_now();
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

            uint32_t *null_pop = NULL;
            if (ocerz_perfstat > 0) {
                null_pop = a64_label(b);
                a64_mov_imm64(b, JTA, (uint64_t)(uintptr_t)&ps_ras_null);
                a64_ldr(b, 8, JTU, JTA, 0); a64_add_imm(b, 1, JTU, JTU, 1); a64_str(b, 8, JTU, JTA, 0);
            }
            uint32_t *miss_pop = a64_label(b);
            if (!hostras) a64_str(b, 4, JT2, 20, RAS_TOP_OFF);
            if (fast3 && ras_body_only()) a64_str(b, 8, JT1, 20, RIP_OFF);
            if (ocerz_perfstat > 0) {
                g_tc_bad = 1;
                a64_mov_imm64(b, JTA, (uint64_t)(uintptr_t)&ps_ras_stale);
                a64_ldr(b, 8, JTU, JTA, 0); a64_add_imm(b, 1, JTU, JTU, 1); a64_str(b, 8, JTU, JTA, 0);
                a64_mov_imm64(b, JTA, (uint64_t)(uintptr_t)ps_retsite_counter(insn->rip));
                a64_ldr(b, 8, JTU, JTA, 0); a64_add_imm(b, 1, JTU, JTU, 1); a64_str(b, 8, JTU, JTA, 0);
                uint32_t *nz = a64_label(b); a64_cbnz(b, 1, JTF, 0);
                a64_mov_imm64(b, JTA, (uint64_t)(uintptr_t)&ps_ras_sentinel);
                a64_ldr(b, 8, JTU, JTA, 0); a64_add_imm(b, 1, JTU, JTU, 1); a64_str(b, 8, JTU, JTA, 0);
                a64_patch_cbz(nz, a64_label(b));
            }
            uint32_t *skip_rip = NULL;
            if (fast3 && ras_body_only()) { skip_rip = a64_label(b); a64_b(b, 0); }
            uint32_t *miss = a64_label(b);
            if (ras_empty) a64_patch_cbz(ras_empty, miss);
            if (fast3 && ras_body_only()) { a64_str(b, 8, JT1, 20, RIP_OFF); a64_patch_b(skip_rip, a64_label(b)); }
            if (ocerz_perfstat > 0) {
                g_tc_bad = 1;
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
                    a64_patch_cbz(ras_stale[i], null_pop ? null_pop : miss_pop);
            }
            if (hostras) {
                uint32_t *keep = a64_label(b); a64_cbnz(b, 1, JTF, 0);
                a64_sub_imm(b, 1, 31, 31, 16);
                a64_patch_cbz(keep, a64_label(b));
                if (!xmm_global_enabled()) emit_xmm_pin_spill_all(b);
                g_ind_call_cont = NULL;
                g_ind_treg = JT1;
                emit_indirect_tail(b, epi_sites, n_epi);
                return 1;
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

void emit_dispatch_stub(OcerzJit *jit, int mode32)
{
    veneer_pool_check(jit);
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

static int g_ind_m32;

static uint64_t g_dbg_ind_src;

static void emit_indirect_tail(A64Buf *b, uint32_t **epi_sites, int *n_epi)
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
    int m32 = g_ind_m32;
    g_ind_treg = JT1;
    g_ind_m32 = 0;
    if (g_pin_class == 3 && g_n_raslit < RASLIT_MAX && !ENV_ON("OCERZ_NO_PSC"))
        psc = psc_alloc();
    uint32_t *psc_miss = NULL;
    if (psc) {
        g_raslit[g_n_raslit].site = a64_label(b);
        g_raslit[g_n_raslit].retaddr = (uint64_t)(uintptr_t)psc;
        g_raslit[g_n_raslit].kind = 1;
        g_raslit[g_n_raslit].tcr = TCR_PSC;
        g_raslit[g_n_raslit].rt = JT2;
        g_n_raslit++;
        a64_emit32(b, 0x58000000u | (uint32_t)JT2);
        a64_ubfx(b, 1, JTT, treg, 2, 5);
        a64_add_reg(b, 1, JT2, JT2, JTT, 4);
        a64_ldr_regoff(b, 8, JTT, 19, JTT, 1);
        a64_ldp_off(b, JTU, JT0, JT2, 0);
        a64_eor_reg(b, 1, JTT, JTT, treg, 0);
        a64_subs_reg(b, 1, A64_ZR, JTU, JTT, 0);
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
        if (m32) {
            (void)a64_try_and_imm(b, 1, JTU, treg, ~JIT_KEY_M32);
            a64_str(b, 8, JTU, 20, RIP_OFF);
        } else {
            a64_str(b, 8, treg, 20, RIP_OFF);
        }
        a64_mov_imm64(b, 0, OCERZ_STEP_OK);
        epi_sites[*n_epi] = a64_label(b);
        a64_b(b, 0);
        (*n_epi)++;
        a64_patch_bcond(psc_miss, a64_label(b));
        a64_ubfx(b, 1, JT0, treg, 2, 5);
        a64_add_reg(b, 1, JT0, 19, JT0, 3);
        a64_ldar(b, 8, JT0, JT0);
    }
    if (treg != JT1) a64_mov_reg(b, 1, JT1, treg);
    if (m32) {
        (void)a64_try_and_imm(b, 1, JTU, JT1, ~JIT_KEY_M32);
        a64_str(b, 8, JTU, 20, RIP_OFF);
    } else {
        a64_str(b, 8, JT1, 20, RIP_OFF);
    }
    a64_lsr_imm(b, 1, JTT, JT1, 33);
    a64_eor_reg(b, 1, JTT, JTT, JT1, 0);
    a64_mov_imm64(b, JTU, 0xff51afd7ed558ccdull);
    a64_mul(b, 1, JTT, JTT, JTU);
    a64_lsr_imm(b, 1, JTU, JTT, 29);
    a64_eor_reg(b, 1, JTT, JTT, JTU, 0);
    a64_mov_imm64(b, JTU, JIT_HASH_MASK);
    a64_and_reg(b, 1, JTT, JTT, JTU, 0);
    tc_imm64(b, JTA, TCR_BUCKETS, 0, (uint64_t)(uintptr_t)g_xlat_jit->buckets);
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
        uint32_t *nobody;
        if (psc) {
            a64_ldr(b, 8, JTA, JTF, (uint32_t)offsetof(JitBlock, body_code));
            nobody = a64_label(b); a64_cbz(b, 1, JTA, 0);
            a64_eor_reg(b, 1, JT0, JT1, JT0, 0);
            a64_stp_off(b, JT0, JTA, JT2, 0);
            a64_mov_reg(b, 1, JT0, JTA);
        } else {
            a64_ldr(b, 8, JT0, JTF, (uint32_t)offsetof(JitBlock, body_code));
            nobody = a64_label(b); a64_cbz(b, 1, JT0, 0);
        }
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

static int emit_indirect32(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites,
                           int *n_exits, uint32_t **epi_sites, int *n_epi)
{
    const X86Operand *o = &insn->ops[0];
    if (insn->seg != OCERZ_SEG_NONE || g_no_chain || o->size != 4 ||
        ENV_ON("OCERZ_NO_INLINE_INDIRECT") || ENV_ON("OCERZ_NO_M32_RET_TAIL"))
        return 0;
    if (insn->op == OCERZ_OP_CALL && (!m32_stack_ok(insn) || !mem_native_store_ok()))
        return 0;
    if (o->kind == OCERZ_OPK_REG) {
        int s = pin_slot(o->reg);
        if (o->high8 || s < 0)
            return 0;
        a64_mov_reg(b, 0, JT1, pin_hreg(s));
    } else if (o->kind == OCERZ_OPK_MEM) {
        if (!emit_mem_ea(b, insn, o, JTA))
            return 0;
        uint32_t *skip = emit_commpage_guard(b, insn, JTA, exit_sites, n_exits);
        emit_add_const(b, JTA, ocerz_guest_base - ea_fold());
        emit_guest_load_ordered(b, 4, JT1, JTA, JTU);
        patch_guard_skip(skip, a64_label(b));
    } else {
        return 0;
    }
    uint64_t retaddr = (uint32_t)(insn->rip + insn->len);
    uint32_t *adr_site = NULL;
    if (insn->op == OCERZ_OP_CALL) {
        int hs = pin_hreg(pin_slot(OCERZ_RSP));
        if (m32_ras_ok(insn)) adr_site = m32_ras_push(b, retaddr);
        a64_mov_imm64(b, JT0, retaddr);
        a64_sub_imm(b, 0, JTA, hs, 4);
        m32_stack_st(b, JT0, JTA);
        a64_mov_reg(b, 0, hs, JTA);
    }
    (void)a64_try_orr_imm(b, 1, JT1, JT1, JIT_KEY_M32);
    g_ind_treg = JT1;
    g_ind_m32 = 1;
    if (!adr_site) {
        emit_indirect_tail(b, epi_sites, n_epi);
        return 1;
    }
    uint32_t *cont = NULL;
    g_ind_call_cont = &cont;
    g_ind_call_tocont = NULL;
    emit_indirect_tail(b, epi_sites, n_epi);
    uint32_t *to_cont = g_ind_call_tocont;
    g_ind_call_cont = NULL;
    assert(cont && to_cont && "32-bit indirect call: no continuation site");
    patch_local_adr(adr_site, cont, JT0);
    a64_patch_b(to_cont, a64_label(b));
    uint32_t *pb_ret = emit_body_chain_tail(b, retaddr, 0, epi_sites, n_epi);
    g_jcc_edge[0].target_rip = retaddr;
    g_jcc_edge[0].patch_b = pb_ret;
    g_jcc_edge[0].cond_site = NULL;
    g_jcc_edge[0].kind = EDGE_BODY;
    g_jcc_edge[0].pin_class = 3;
    g_n_jcc_edges = 1;
    return 1;
}

int emit_indirect_jmp(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites,
                             int *n_exits, uint32_t **epi_sites, int *n_epi)
{
    if (insn->op != OCERZ_OP_JMP || insn->ops[0].kind == OCERZ_OPK_IMM)
        return 0;
    if (insn->mode32)
        return emit_indirect32(b, insn, exit_sites, n_exits, epi_sites, n_epi);
    if (ENV_ON("OCERZ_NO_INLINE_INDIRECT"))
        return 0;
    if (insn->seg != OCERZ_SEG_NONE)
        return 0;
    if (ENV_ON("OCERZ_EXP_MAT_IND")) emit_materialize(b);
    if (!emit_branch_target(b, insn, &insn->ops[0], exit_sites, n_exits))
        return 0;
    emit_indirect_tail(b, epi_sites, n_epi);
    return 1;
}

static int bridge_fastcall_enabled(void)
{
    static int en = -1;
    if (en < 0)
        en = ocerz_mode == OCERZ_MODE_NATIVE && !getenv("OCERZ_NO_BRIDGE_FASTCALL");
    return en;
}

static int leaf_inplace_enabled(void)
{
    static int en = -1;
    if (en < 0)
        en = !getenv("OCERZ_NO_LEAF_INPLACE") && !getenv("OCERZ_BRIDGESTAT") &&
             !getenv("OCERZ_BRIDGELOG");
    return en;
}

uint32_t *emit_leaf_call_ret(A64Buf *b, const void *leaf, int writes, uint32_t **epi_sites,
                                    int *n_epi)
{
    if (writes) {
        tc_imm64(b, JT0, TCR_SYM, TCS_RETIRE_COUNT, (uint64_t)(uintptr_t)&ocerz_jit_retire_count);
        a64_ldr(b, 8, JT0, JT0, 0);
        a64_str(b, 8, JT0, 20, LEAF_EPOCH_OFF);
    }
    const char *leaf_at = g_xlat_jit && g_xlat_jit->leaf_near
                              ? g_xlat_jit->leaf_near + ((const char *)leaf - ocerz_leaf_lo)
                              : (const char *)leaf;
    int64_t leaf_words = ((const char *)leaf_at - (const char *)b->p) / 4;
    if (g_tc_on) {
        tc_imm64(b, 16, TCR_LEAF, (uint64_t)((const char *)leaf - ocerz_leaf_lo), (uint64_t)(uintptr_t)leaf_at);
        a64_blr(b, 16);
    } else if (leaf_words > -(1 << 25) && leaf_words < (1 << 25)) {
        a64_emit32(b, 0x94000000u | ((uint32_t)leaf_words & 0x03ffffffu));
    } else {
        a64_mov_imm64(b, 16, (uint64_t)(uintptr_t)leaf_at);
        a64_blr(b, 16);
    }
    g_callout_seq++;
    uint32_t *declined = a64_label(b); a64_cbnz(b, 1, JT0, 0);
    emit_reload_mem_base(b);
    a64_ldr_post64(b, JT1, pin_hreg(pin_slot(OCERZ_RSP)), 8);
    uint32_t *retired = NULL;
    if (writes) {
        a64_ldr(b, 8, JT2, 20, LEAF_EPOCH_OFF);
        tc_imm64(b, JT0, TCR_SYM, TCS_RETIRE_COUNT, (uint64_t)(uintptr_t)&ocerz_jit_retire_count);
        a64_ldr(b, 8, JT0, JT0, 0);
        a64_subs_reg(b, 1, A64_ZR, JT0, JT2, 0);
        retired = a64_label(b); a64_bcond(b, A64_NE, 0);
    }
    a64_ldp_post(b, JTF, 30, 31, 16);
    a64_subs_reg(b, 1, A64_ZR, JTF, JT1, 0);
    uint32_t *miss_ne = a64_label(b); a64_bcond(b, A64_NE, 0);
    uint32_t *miss_z = a64_label(b); a64_cbz(b, 1, 30, 0);
    if (!xmm_global_enabled()) emit_xmm_pin_spill_all(b);
    a64_ret(b);
    uint32_t *miss = a64_label(b);
    a64_patch_bcond(miss_ne, miss);
    a64_patch_cbz(miss_z, miss);
    if (retired) a64_patch_bcond(retired, miss);
    a64_str(b, 8, JT1, 20, RIP_OFF);
    a64_mov_imm64(b, 0, OCERZ_STEP_OK);
    epi_sites[*n_epi] = a64_label(b);
    a64_b(b, 0);
    (*n_epi)++;
    return declined;
}

int leaf_layout_ok(void)
{
    static int no_blret = -1;
    if (no_blret < 0) no_blret = getenv("OCERZ_NO_BLRET") ? 1 : 0;
    int fast3 = g_pin_class == 3 && pin_slot(OCERZ_RSP) >= 0 && stack_plain_access_ok() &&
                jgb_usable() && !stack_guard_needed();
    return fast3 && ras_body_only() && host_ras_enabled() && !no_blret && !g_xlat_mode32 &&
           leaf_inplace_enabled() && ocerz_guest_base == 0 &&
           !(ocerz_low_base && ocerz_mode == OCERZ_MODE_NATIVE) && pin_slot(OCERZ_RAX) >= 0 &&
           pin_slot(OCERZ_RDI) >= 0 && pin_slot(OCERZ_RSI) >= 0 && pin_slot(OCERZ_RDX) >= 0 &&
           pin_hreg(pin_slot(OCERZ_RAX)) == 21 && pin_hreg(pin_slot(OCERZ_RDI)) == 28 &&
           pin_hreg(pin_slot(OCERZ_RSI)) == 27 && pin_hreg(pin_slot(OCERZ_RDX)) == 23;
}

void emit_bridge_fastcall(A64Buf *b, const X86Insn *insns, int i,
                                 uint32_t **epi_sites, int *n_epi)
{
    const X86Insn *insn = &insns[i];
    if (!bridge_fastcall_enabled() || g_xlat_mode32 || i < 1)
        return;
    if (insn->op != OCERZ_OP_JMP || insn->nops != 1 || insn->seg != OCERZ_SEG_NONE)
        return;
    const X86Operand *o = &insn->ops[0];
    if (o->kind != OCERZ_OPK_MEM || !o->riprel || o->size != 8)
        return;
    const X86Insn *mv = &insns[i - 1];
    if (mv->op != OCERZ_OP_MOV || mv->nops != 2 || mv->seg != OCERZ_SEG_NONE ||
        mv->ops[0].kind != OCERZ_OPK_REG || mv->ops[0].reg != OCERZ_R11 ||
        mv->ops[0].size != 4 || mv->ops[0].high8 || mv->ops[1].kind != OCERZ_OPK_IMM)
        return;
    const uint64_t trap = OCERZ_DYLDAPI_LO + OCERZ_BRIDGE_OFF;
    uint64_t slot = (uint64_t)o->disp;
    if (!ocerz_addr_readable(slot) || !ocerz_addr_readable(slot + 7) || ocerz_ld(slot, 8) != trap)
        return;
    uint64_t id = (uint32_t)mv->ops[1].imm;
    if (!ocerz_vdylib_export_name(id, NULL, NULL) || ocerz_vdylib_trap_only(id))
        return;
    uint16_t xin = 0xffff, xout = 0xffff;
    if (!xmm_global_enabled() || !ocerz_vdylib_xmm_contract(id, &xin, &xout))
        xin = xout = 0xffff;
    static int no_blret = -1;
    if (no_blret < 0) no_blret = getenv("OCERZ_NO_BLRET") ? 1 : 0;
    int fast3 = g_pin_class == 3 && pin_slot(OCERZ_RSP) >= 0 && stack_plain_access_ok() &&
                jgb_usable() && !stack_guard_needed();
    if (!fast3 || !ras_body_only() || !host_ras_enabled() || no_blret)
        return;

    uint64_t leaf_limit = 0;
    const void *leaf = leaf_layout_ok() ? ocerz_vdylib_leaf(id, &leaf_limit) : NULL;

    l0_flush_all(b);
    if (leaf) {
        const char *leaf_sym = NULL;
        if (ocerz_verbose >= 1 && ocerz_vdylib_export_name(id, NULL, &leaf_sym))
            OCERZ_LOG("jit: %s is answered in place at %#llx by %p from %p\n", leaf_sym,
                      (unsigned long long)insn->rip, leaf, (void *)b->p);
        uint32_t *leaf_out[3];
        uint8_t leaf_out_cb[3] = { 0, 0, 0 };
        int n_leaf_out = 0;
        a64_mov_imm64(b, JT1, slot + ocerz_guest_base);
        a64_ldr(b, 8, JT1, JT1, 0);
        a64_mov_imm64(b, JT2, trap);
        a64_subs_reg(b, 1, A64_ZR, JT1, JT2, 0);
        leaf_out[n_leaf_out++] = a64_label(b); a64_bcond(b, A64_NE, 0);
        if (leaf_limit) {
            a64_mov_imm64(b, JT0, leaf_limit);
            a64_subs_reg(b, 1, A64_ZR, 23, JT0, 0);
            leaf_out[n_leaf_out++] = a64_label(b); a64_bcond(b, A64_HI, 0);
        }
        leaf_out_cb[n_leaf_out] = 1;
        leaf_out[n_leaf_out++] = emit_leaf_call_ret(b, leaf, leaf_limit != 0, epi_sites, n_epi);
        for (int k = 0; k < n_leaf_out; k++) {
            if (leaf_out_cb[k]) a64_patch_cbz(leaf_out[k], a64_label(b));
            else a64_patch_bcond(leaf_out[k], a64_label(b));
        }
    }
    a64_mov_imm64(b, JT1, slot + ocerz_guest_base);
    a64_ldr(b, 8, JT1, JT1, 0);
    a64_mov_imm64(b, JT2, trap);
    a64_subs_reg(b, 1, A64_ZR, JT1, JT2, 0);
    uint32_t *to_plain = a64_label(b); a64_bcond(b, A64_NE, 0);
    a64_ldr(b, 8, JT0, 20, JIT_FP_OFF);
    a64_add_imm(b, 1, JTT, 31, 0);
    a64_sub_reg(b, 1, JT0, JT0, JTT, 0);
    a64_subs_imm_sh12(b, 1, A64_ZR, JT0, BRIDGE_FAST_DEPTH_MAX_K);
    uint32_t *to_deep = a64_label(b); a64_bcond(b, A64_HI, 0);

    g_ymmh_zero = 0;
    for (unsigned r = 0; r < 16; r++)
        if (xmm_is_pinned(r) && (xin >> r & 1))
            a64_str_v(b, 16, xmm_vreg(r), 20, XMM_BASE_OFF + r * 16);
    emit_spill_pinned(b);
    a64_mov_reg(b, 1, 0, 19);
    a64_mov_reg(b, 1, 1, 20);
    g_tc_bad = 1;
    a64_mov_imm64(b, 16, (uint64_t)(uintptr_t)&ocerz_vdylib_fastcall);
    a64_blr(b, 16);
    g_callout_seq++;
    a64_mov_reg(b, 0, JT0, 0);
    emit_fill_pinned(b);
    uint32_t *to_leave = a64_label(b); a64_cbnz(b, 0, JT0, 0);
    for (unsigned r = 0; r < 16; r++)
        if (xmm_is_pinned(r) && (xout >> r & 1))
            a64_ldr_v(b, 16, xmm_vreg(r), 20, XMM_BASE_OFF + r * 16);
    emit_pk_consts_load(b);
    yc_reload_all(b);
    emit_reload_jgb(b);
    emit_reload_mem_base(b);

    a64_ldr(b, 8, JT1, 20, RIP_OFF);
    a64_ldp_post(b, JTF, 30, 31, 16);
    a64_subs_reg(b, 1, A64_ZR, JTF, JT1, 0);
    uint32_t *miss_ne = a64_label(b); a64_bcond(b, A64_NE, 0);
    uint32_t *miss_z = a64_label(b); a64_cbz(b, 1, 30, 0);
    if (!xmm_global_enabled()) emit_xmm_pin_spill_all(b);
    a64_ret(b);

    uint32_t *miss = a64_label(b);
    a64_patch_bcond(miss_ne, miss);
    a64_patch_cbz(miss_z, miss);
    a64_mov_imm64(b, 0, OCERZ_STEP_OK);
    epi_sites[*n_epi] = a64_label(b);
    a64_b(b, 0);
    (*n_epi)++;

    a64_patch_cbz(to_leave, a64_label(b));
    emit_xmm_pin_load_all(b);
    yc_reload_all(b);
    emit_reload_jgb(b);
    emit_reload_mem_base(b);
    a64_sub_imm(b, 0, 0, JT0, 1);
    epi_sites[*n_epi] = a64_label(b);
    a64_b(b, 0);
    (*n_epi)++;

    uint32_t *plain = a64_label(b);
    a64_patch_bcond(to_plain, plain);
    a64_patch_bcond(to_deep, plain);
}

int emit_indirect_call(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites,
                              int *n_exits, uint32_t **epi_sites, int *n_epi)
{
    g_dbg_ind_src = insn->rip;
    if (insn->op != OCERZ_OP_CALL || insn->ops[0].kind == OCERZ_OPK_IMM)
        return 0;
    if (insn->mode32)
        return emit_indirect32(b, insn, exit_sites, n_exits, epi_sites, n_epi);
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
                    stack_fast() && !g_no_chain &&
                    !g_no_ras && ras_body_only() && !no_blret_i;
        if (fast3) {
            int hs = pin_hreg(pin_slot(OCERZ_RSP));
            emit_const_lit(b, JT2, retaddr);
            uint32_t *adr_site;
            if (host_ras_enabled()) {
                adr_site = a64_label(b);
                a64_emit32(b, 0x10000000u | (uint32_t)JT0);
                a64_stp_pre(b, JT2, JT0, 31, -16);
                emit_stack_push64(b, insn, hs, JT2);
            } else {
                emit_stack_push64(b, insn, hs, JT2);
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
            emit_indirect_tail(b, epi_sites, n_epi);
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
    g_ea_plain = stack_plain_now();
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
            tc_imm64(b, JTA, TCR_RASSLOT, retaddr, (uint64_t)(uintptr_t)rslot);
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
    emit_indirect_tail(b, epi_sites, n_epi);
    return 1;
}

void emit_slowcall(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
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

    l0_flush_all(b);
    emit_materialize(b);
    g_ymmh_zero = 0;
    emit_xmm_pin_spill_all(b);
    emit_spill_pinned(b);
    a64_mov_reg(b, 1, 0, 19);
    a64_mov_reg(b, 1, 1, 20);
    {
        const X86Insn *base = g_cur_blk ? g_cur_blk->insns : NULL;
        ptrdiff_t idx = (base && insn >= base && insn < base + g_cur_blk->n_insns) ? insn - base : -1;
        if (idx >= 0 && g_keep && idx < g_keep_n && g_slow_run_last >= idx && g_slow_run_last < g_keep_n) {
            for (int k = (int)idx; k <= g_slow_run_last; k++) g_keep[k] = 1;
            tc_imm64(b, 2, TCR_BLK, 0, (uint64_t)(uintptr_t)g_cur_blk);
            a64_ldr(b, 8, 3, 20, JIT_SCRATCH_OFF);
            a64_mov_imm64(b, 4, (uint64_t)g_slow_run_last);
            tc_imm64(b, 16, TCR_SYM, TCS_EXEC_RUN_AT, (uint64_t)(uintptr_t)&ocerz_jit_exec_run_at);
        } else if (idx >= 0 && g_keep && idx < g_keep_n) {
            g_keep[idx] = 1;
            tc_imm64(b, 2, TCR_BLK, 0, (uint64_t)(uintptr_t)g_cur_blk);
            a64_mov_imm64(b, 3, (uint64_t)idx);
            tc_imm64(b, 16, TCR_SYM, TCS_EXEC_ONE_AT, (uint64_t)(uintptr_t)&ocerz_jit_exec_one_at);
        } else {
            g_no_compact = 1;
            if (idx >= 0) tc_imm64(b, 2, TCR_INSN, (uint64_t)idx, (uint64_t)(uintptr_t)insn);
            else { g_tc_bad = 1; a64_mov_imm64(b, 2, (uint64_t)(uintptr_t)insn); }
            tc_imm64(b, 16, TCR_SYM, TCS_EXEC_ONE, (uint64_t)(uintptr_t)&ocerz_jit_exec_one);
        }
    }
    a64_blr(b, 16);
    g_callout_seq++;
    emit_fill_pinned(b);
    emit_xmm_pin_load_all(b);
    yc_reload_all(b);
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

uint32_t *emit_body_chain_tail(A64Buf *b, uint64_t target_rip, int poll,
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

uint32_t *emit_chain_tail(A64Buf *b, int poll)
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

static struct { uint32_t *sites[3]; int nsites; const X86Insn *insn; uint32_t *back; uint32_t pre;
                int8_t l0[16]; uint8_t l0_dbl[16]; uint16_t l0_dirty; uint16_t yc_dirty; } g_oolslow[OOLSLOW_MAX];

int g_n_oolslow;

int oolslow_add(const X86Insn *insn, uint32_t **sites, int nsites, uint32_t *back)
{
    if (g_n_oolslow >= OOLSLOW_MAX || nsites > 3) return 0;
    ea_cache_reset();
    for (int i = 0; i < nsites; i++) g_oolslow[g_n_oolslow].sites[i] = sites[i];
    g_oolslow[g_n_oolslow].nsites = nsites;
    g_oolslow[g_n_oolslow].insn = insn;
    g_oolslow[g_n_oolslow].back = back;
    g_oolslow[g_n_oolslow].pre = g_oolslow_pre;
    for (int r = 0; r < 16; r++) { g_oolslow[g_n_oolslow].l0[r] = g_l0[r]; g_oolslow[g_n_oolslow].l0_dbl[r] = g_l0_dbl[r]; }
    g_oolslow[g_n_oolslow].l0_dirty = g_l0_dirty;
    g_oolslow[g_n_oolslow].yc_dirty = g_yc_dirty;
    g_oolslow_pre = 0;
    g_n_oolslow++;
    return 1;
}

void emit_oolslow_arms(A64Buf *b, uint32_t **exit_sites, int *n_exits)
{
    for (int k = 0; k < g_n_oolslow; k++) {
        int sane = g_oolslow[k].back >= g_push_entry && g_oolslow[k].back < b->p;
        for (int i = 0; sane && i < g_oolslow[k].nsites; i++)
            sane = g_oolslow[k].sites[i] >= g_push_entry && g_oolslow[k].sites[i] < b->p;
        if (!sane) {
            static int nwarn;
            if (nwarn++ < 40) {
                char tb[128] = "";
                if (g_oolslow[k].insn) ocerz_format_insn(g_oolslow[k].insn, tb, sizeof tb);
                fprintf(stderr, "ocerz: warning: dropping stale oolslow arm (site/back outside the current block)"
                                " block=%#llx insn=%#llx back=%+ld site0=%+ld end=%ld nsites=%d %s\n",
                        (unsigned long long)g_self_rip,
                        (unsigned long long)(g_oolslow[k].insn ? g_oolslow[k].insn->rip : 0),
                        (long)(g_oolslow[k].back - g_push_entry),
                        g_oolslow[k].nsites ? (long)(g_oolslow[k].sites[0] - g_push_entry) : 0L,
                        (long)(b->p - g_push_entry), g_oolslow[k].nsites, tb);
            }
            continue;
        }
        uint32_t *lbl = a64_label(b);
        for (int i = 0; i < g_oolslow[k].nsites; i++) patch_any_branch(g_oolslow[k].sites[i], lbl);
        if (g_oolslow[k].pre) a64_emit32(b, g_oolslow[k].pre);
        emit_l0_flush_from(b, g_oolslow[k].l0, g_oolslow[k].l0_dbl, g_oolslow[k].l0_dirty);
        yc_flush_from(b, g_oolslow[k].yc_dirty);
        emit_slowcall(b, g_oolslow[k].insn, exit_sites, n_exits);
        emit_l0_reload_from(b, g_oolslow[k].l0, g_oolslow[k].l0_dbl);
        uint32_t *here = a64_label(b);
        a64_b(b, (int32_t)(g_oolslow[k].back - here));
    }
    g_n_oolslow = 0;
}

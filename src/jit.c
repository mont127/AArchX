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
 * the architectural value, and inlined ops (register moves) never read it.
 * This is what makes inlining safe without a full register/flag model.
 *
 * Inlined fast paths (chosen for frequency and bit-exact simplicity):
 *   - NOP/PAUSE/PREFETCH/CLFLUSH: emit nothing.
 *   - MOV reg<-reg and reg<-imm at operand size 4 or 8, high-byte-free: a
 *     ldr/str (size 4 loads through a w-register so the 64-bit slot is
 *     zero-extended exactly as x86 requires) or a movimm+str. Every other
 *     MOV form (memory, 8/16-bit, segment) falls to the slow path.
 * Everything else — arithmetic with flags, memory operands, SSE, x87,
 * strings, branches, syscalls — runs through ocerz_jit_exec_one, i.e. the
 * interpreter, so the JIT is correct by construction and the differential
 * test (every guest binary under -no-jit vs JIT) holds it to that.
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
#include "ocerz/a64emit.h"
#include "ocerz/dyldapi.h"

#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#include <stdlib.h>

#define JIT_CODE_BYTES (64u << 20)
#define JIT_HASH_BITS 16
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
} JitBlock;

struct OcerzJit {
    struct OcerzVM *vm;
    uint32_t *code_base;
    uint32_t *code_cur;
    uint32_t *code_end;
    JitBlock *buckets[JIT_HASH_SIZE];
    uint64_t blocks_translated;
};

int ocerz_jit_exec_one(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    vm->insn_count++;
    cpu->rip = insn->rip + insn->len;
    if (vm->trace) {
        char buf[128];
        ocerz_format_insn(insn, buf, sizeof buf);
        fprintf(stderr, "ocerz: %#llx: %s\n", (unsigned long long)insn->rip, buf);
    }
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
    for (JitBlock *b = jit->buckets[hash_rip(rip)]; b; b = b->hnext)
        if (b->guest_rip == rip)
            return b;
    return NULL;
}

static void cache_insert(OcerzJit *jit, JitBlock *b)
{
    unsigned h = hash_rip(b->guest_rip);
    b->hnext = jit->buckets[h];
    jit->buckets[h] = b;
}

static int try_inline(A64Buf *b, const X86Insn *insn)
{
    if (insn->op == OCERZ_OP_NOP || insn->op == OCERZ_OP_PAUSE ||
        insn->op == OCERZ_OP_PREFETCH || insn->op == OCERZ_OP_CLFLUSH)
        return 1;

    if (insn->op == OCERZ_OP_MOV) {
        const X86Operand *d = &insn->ops[0];
        const X86Operand *s = &insn->ops[1];
        if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
            return 0;
        if (s->kind == OCERZ_OPK_REG && !s->high8 && s->size == d->size) {
            int sz4 = d->size == 4;
            a64_ldr(b, sz4 ? 4 : 8, 0, 20, (uint32_t)s->reg * 8);
            a64_str(b, 8, 0, 20, (uint32_t)d->reg * 8);
            return 1;
        }
        if (s->kind == OCERZ_OPK_IMM) {
            uint64_t v = s->imm;
            if (d->size == 4)
                v &= 0xffffffffull;
            a64_mov_imm64(b, 0, v);
            a64_str(b, 8, 0, 20, (uint32_t)d->reg * 8);
            return 1;
        }
        return 0;
    }
    return 0;
}

static void emit_slowcall(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
{
    a64_mov_reg(b, 1, 0, 19);
    a64_mov_reg(b, 1, 1, 20);
    a64_mov_imm64(b, 2, (uint64_t)(uintptr_t)insn);
    a64_mov_imm64(b, 16, (uint64_t)(uintptr_t)&ocerz_jit_exec_one);
    a64_blr(b, 16);
    exit_sites[*n_exits] = a64_label(b);
    a64_cbnz(b, 0, 0, 0);
    (*n_exits)++;
}

static JitBlock *translate(OcerzJit *jit, uint64_t rip)
{
    X86Insn scratch[JIT_MAX_BLOCK_INSNS];
    int n = 0;
    uint64_t pc = rip;
    for (; n < JIT_MAX_BLOCK_INSNS; ) {
        const uint8_t *code = (const uint8_t *)ocerz_g2h(pc);
        int rc = ocerz_decode(code, 15, pc, &scratch[n]);
        if (rc != OCERZ_OK)
            break;
        unsigned op = scratch[n].op;
        uint8_t len = scratch[n].len;
        n++;
        if (is_terminator(op))
            break;
        pc += len;
    }
    if (n == 0)
        return NULL;

    JitBlock *blk = (JitBlock *)calloc(1, sizeof *blk);
    if (!blk)
        return NULL;
    blk->insns = (X86Insn *)malloc((size_t)n * sizeof(X86Insn));
    if (!blk->insns) {
        free(blk);
        return NULL;
    }
    memcpy(blk->insns, scratch, (size_t)n * sizeof(X86Insn));
    blk->n_insns = n;
    blk->guest_rip = rip;

    pthread_jit_write_protect_np(0);
    A64Buf b = { jit->code_cur, jit->code_cur, jit->code_end, 0 };
    uint32_t *entry = b.p;

    a64_stp_pre(&b, 29, 30, 31, -16);
    a64_stp_pre(&b, 19, 20, 31, -16);
    a64_mov_reg(&b, 1, 19, 0);
    a64_mov_reg(&b, 1, 20, 1);

    uint32_t *exit_sites[JIT_MAX_BLOCK_INSNS];
    int n_exits = 0;
    for (int i = 0; i < n; i++) {
        if (!try_inline(&b, &blk->insns[i]))
            emit_slowcall(&b, &blk->insns[i], exit_sites, &n_exits);
    }

    uint32_t *exit_label = a64_label(&b);
    a64_ldp_post(&b, 19, 20, 31, 16);
    a64_ldp_post(&b, 29, 30, 31, 16);
    a64_ret(&b);

    for (int i = 0; i < n_exits; i++)
        a64_patch_cbz(exit_sites[i], exit_label);

    pthread_jit_write_protect_np(1);

    if (b.overflow) {
        free(blk->insns);
        free(blk);
        return NULL;
    }

    sys_icache_invalidate(entry, (size_t)((b.p - entry) * 4));
    jit->code_cur = b.p;
    blk->code = (JitBlockFn)entry;

    cache_insert(jit, blk);
    jit->blocks_translated++;
    return blk;
}

OcerzJit *ocerz_jit_create(struct OcerzVM *vm)
{
    OcerzJit *jit = (OcerzJit *)calloc(1, sizeof *jit);
    if (!jit)
        return NULL;
    jit->vm = vm;
    void *p = mmap(NULL, JIT_CODE_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) {
        OCERZ_LOG("JIT unavailable (MAP_JIT failed); using interpreter\n");
        free(jit);
        return NULL;
    }
    jit->code_base = (uint32_t *)p;
    jit->code_cur = (uint32_t *)p;
    jit->code_end = (uint32_t *)((uint8_t *)p + JIT_CODE_BYTES);
    return jit;
}

void ocerz_jit_destroy(OcerzJit *jit)
{
    if (!jit)
        return;
    for (unsigned i = 0; i < JIT_HASH_SIZE; i++) {
        JitBlock *b = jit->buckets[i];
        while (b) {
            JitBlock *next = b->hnext;
            free(b->insns);
            free(b);
            b = next;
        }
    }
    munmap(jit->code_base, JIT_CODE_BYTES);
    free(jit);
}

uint64_t ocerz_jit_blocks(const OcerzJit *jit)
{
    return jit ? jit->blocks_translated : 0;
}

static pthread_mutex_t jit_lock = PTHREAD_MUTEX_INITIALIZER;

void ocerz_jit_prefork(void)
{
    pthread_mutex_lock(&jit_lock);
}

void ocerz_jit_postfork(void)
{
    pthread_mutex_unlock(&jit_lock);
}

int ocerz_jit_step(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (cpu->rip - OCERZ_DYLDAPI_LO < (OCERZ_DYLDAPI_HI - OCERZ_DYLDAPI_LO))
        return OCERZ_EUNSUP;
    OcerzJit *jit = vm->jit;
    pthread_mutex_lock(&jit_lock);
    JitBlock *b = cache_lookup(jit, cpu->rip);
    if (!b)
        b = translate(jit, cpu->rip);
    pthread_mutex_unlock(&jit_lock);
    if (!b)
        return OCERZ_EUNSUP;
    return b->code(vm, cpu);
}

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
 * the architectural value, and inlined ops (register moves) never read it.
 * This is what makes inlining safe without a full register/flag model.
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
 *     zero-extended exactly as x86 requires) or a movimm+str. Every other
 *     MOV form (memory, 8/16-bit, segment) falls to the slow path.
 *   - LEA reg<-mem (4/8-bit dest, no segment): the effective address is
 *     materialized with movimm/ldr/add against the register file and stored,
 *     32-bit dests truncated by a w-move; no flags.
 *   - The hot register/immediate ALU ops at operand size 4 or 8, high-byte-
 *     free: ADD SUB CMP AND OR XOR TEST INC DEC, and the constant-count
 *     SHL/SHR/SAR. The arm64 op runs in a w- or x-register (a w-op zero-
 *     extends, matching x86's 32-bit upper-clear), the result is stored to
 *     the slot for non-comparison forms, and the six x86 arithmetic flags
 *     are computed natively into cpu->rflags. Because rflags is kept eager,
 *     a mixed block of inlined and slow-path instructions stays coherent:
 *     the slow path reads and writes the same live bits.
 *   - The Jcc terminator: it reads cpu->rflags (always current), reproduces
 *     ocerz_cc_eval's predicate tree, csel's cpu->rip between the two
 *     compile-time-constant target rips (taken = ops[0].imm, fallthrough =
 *     rip+len), and returns STEP_OK so the dispatcher re-enters at the new
 *     rip. A native terminator must set the return value to STEP_OK itself,
 *     since no preceding slow call is guaranteed to have left it there.
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
#define GPR_OFF(r) ((uint32_t)((unsigned)(r) * 8))

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
 * SF<-N (MI), placed at their x86 bit positions, OR'd into the accumulator. */
static void emit_zf_sf(A64Buf *b)
{
    a64_cset(b, JTT, A64_EQ);
    a64_lsl_imm(b, 0, JTT, JTT, 6);
    a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
    a64_cset(b, JTT, A64_MI);
    a64_lsl_imm(b, 0, JTT, JTT, 7);
    a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
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

/* Load an ALU operand (reg or imm) into dst. Returns 0 if the operand kind is
 * not inlineable at this size. For a REG it must be a plain (no-high8) GPR of
 * the same width; size-4 loads zero-extend through a w-register, matching the
 * interpreter's ocerz_read_gpr truncation. */
static int emit_load_operand(A64Buf *b, const X86Operand *op, int sf, int dst)
{
    if (op->kind == OCERZ_OPK_REG) {
        if (op->high8)
            return 0;
        a64_ldr(b, sf ? 8 : 4, dst, 20, GPR_OFF(op->reg));
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

/* ADD/SUB/CMP/AND/OR/XOR/TEST reg<-reg|imm at size 4/8. The arm64 op runs in
 * w-regs for size 4 (zero-extending the upper half exactly as a 32-bit x86
 * write does) and the full 8-byte slot is stored for the writing forms. Flags
 * are then computed into cpu->rflags to match flags.c bit for bit. */
static int emit_arith(A64Buf *b, const X86Insn *insn)
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
    if (is_logic && op != OCERZ_OP_AND && op != OCERZ_OP_TEST) {
        /* orr/eor do not set NZCV; recompute Z/N with a flag-setting compare
         * against zero so emit_zf_sf reads valid NZCV. */
        a64_subs_imm(b, sf, A64_ZR, JT2, 0);
    }

    if (writes)
        a64_str(b, 8, JT2, 20, GPR_OFF(d->reg));

    a64_mov_imm64(b, JTF, 0);
    emit_zf_sf(b);
    emit_pf(b, JT2);
    if (is_add) {
        a64_cset(b, JTT, A64_CS);
        a64_lsl_imm(b, 0, JTT, JTT, 0);
        a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        a64_cset(b, JTT, A64_VS);
        a64_lsl_imm(b, 0, JTT, JTT, 11);
        a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        a64_eor_reg(b, 1, JTT, JT0, JT1, 0);
        a64_eor_reg(b, 1, JTT, JTT, JT2, 0);
        a64_ubfx(b, 1, JTT, JTT, 4, 1);
        a64_lsl_imm(b, 0, JTT, JTT, 4);
        a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
    } else if (is_sub) {
        a64_cset(b, JTT, A64_CC);
        a64_lsl_imm(b, 0, JTT, JTT, 0);
        a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        a64_cset(b, JTT, A64_VS);
        a64_lsl_imm(b, 0, JTT, JTT, 11);
        a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        a64_eor_reg(b, 1, JTT, JT0, JT1, 0);
        a64_eor_reg(b, 1, JTT, JTT, JT2, 0);
        a64_ubfx(b, 1, JTT, JTT, 4, 1);
        a64_lsl_imm(b, 0, JTT, JTT, 4);
        a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
    }
    emit_commit_flags(b, JIT_ARITH_FLAGS);
    return 1;
}

/* INC/DEC reg at size 4/8. CF is preserved (the clear-mask excludes it). OF is
 * positional (INC overflows into the signed minimum, DEC out of it leaving the
 * maximum); AF is the bit-3 half-carry, here (res&0xf)==0 for INC and ==0xf for
 * DEC. ZF/SF/PF follow the result. */
static int emit_incdec(A64Buf *b, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    if (d->kind != OCERZ_OPK_REG || d->high8 || (d->size != 4 && d->size != 8))
        return 0;
    int sf = d->size == 8;
    int is_inc = insn->op == OCERZ_OP_INC;

    a64_ldr(b, sf ? 8 : 4, JT0, 20, GPR_OFF(d->reg));
    if (is_inc)
        a64_adds_imm(b, sf, JT2, JT0, 1);
    else
        a64_subs_imm(b, sf, JT2, JT0, 1);
    a64_str(b, 8, JT2, 20, GPR_OFF(d->reg));

    a64_mov_imm64(b, JTF, 0);
    emit_zf_sf(b);
    emit_pf(b, JT2);

    uint64_t of_const = is_inc ? ((uint64_t)1 << (d->size * 8 - 1))
                               : (ocerz_mask(d->size) >> 1);
    a64_mov_imm64(b, JTU, of_const);
    a64_subs_reg(b, 1, A64_ZR, JT2, JTU, 0);
    a64_cset(b, JTT, A64_EQ);
    a64_lsl_imm(b, 0, JTT, JTT, 11);
    a64_orr_reg(b, 1, JTF, JTF, JTT, 0);

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

    emit_commit_flags(b, JIT_ARITH_FLAGS & ~(uint64_t)OCERZ_CF);
    return 1;
}

/* SHL/SHR/SAR reg, imm at size 4/8 with a known nonzero count. A zero count
 * leaves the destination and all flags untouched (the interpreter early-out),
 * so it emits nothing. CF is the last bit shifted out, read by a fixed-index
 * bitfield extract on the ORIGINAL value (sign-extended for SAR); OF mirrors
 * flags.c (SHL: CF^SF, SHR: original MSB, SAR: 0). ZF/SF/PF follow the
 * result. CL-count and shift forms with memory or 8/16-bit operands fall to
 * the slow path. */
static int emit_shift(A64Buf *b, const X86Insn *insn)
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

    a64_subs_imm(b, sf, A64_ZR, JT2, 0);
    a64_mov_imm64(b, JTF, 0);
    emit_zf_sf(b);
    emit_pf(b, JT2);

    if (op == OCERZ_OP_SHL) {
        int cf_bit = bits - (int)cnt;
        if (cf_bit >= 0 && cf_bit < bits) {
            a64_ubfx(b, sf, JTT, JT0, cf_bit, 1);
            a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        }
        a64_ubfx(b, 1, JTT, JTF, 0, 1);
        a64_ubfx(b, 1, JTU, JTF, 7, 1);
        a64_eor_reg(b, 1, JTT, JTT, JTU, 0);
        a64_lsl_imm(b, 0, JTT, JTT, 11);
        a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
    } else if (op == OCERZ_OP_SHR) {
        int cf_bit = (int)cnt - 1;
        a64_ubfx(b, sf, JTT, JT0, cf_bit, 1);
        a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        a64_ubfx(b, sf, JTT, JT0, bits - 1, 1);
        a64_lsl_imm(b, 0, JTT, JTT, 11);
        a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
    } else {
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
    emit_commit_flags(b, JIT_ARITH_FLAGS);
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
static int mem_native_store_ok(void)
{
    return ocerz_watch_addr == 0;
}

/* Compute the guest effective address of a base+index*scale+disp memory
 * operand into addr_reg. Returns 0 (not inlineable) for segment overrides,
 * 32-bit address size, and RIP-relative forms with no register inputs are
 * accepted (the absolute disp is materialized directly). */
static int emit_mem_ea(A64Buf *b, const X86Insn *insn, const X86Operand *op, int addr_reg)
{
    if (insn->seg != OCERZ_SEG_NONE)
        return 0;
    if (op->riprel) {
        a64_mov_imm64(b, addr_reg, (uint64_t)op->disp);
        return 1;
    }
    if (insn->addrsize == 4)
        return 0;
    a64_mov_imm64(b, addr_reg, (uint64_t)op->disp);
    if (op->base != OCERZ_REG_NONE) {
        a64_ldr(b, 8, JT0, 20, GPR_OFF(op->base));
        a64_add_reg(b, 1, addr_reg, addr_reg, JT0, 0);
    }
    if (op->index != OCERZ_REG_NONE) {
        a64_ldr(b, 8, JT0, 20, GPR_OFF(op->index));
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
    uint32_t *to_native = NULL;
    if (ocerz_low_base) {
        a64_mov_imm64(b, JTU, OCERZ_LOW_LIMIT);
        a64_sub_reg(b, 1, JTT, addr_reg, JTU, 0);          /* JTT = gaddr - LOW_LIMIT */
        a64_mov_imm64(b, JTU, OCERZ_TOP_LO - OCERZ_LOW_LIMIT);
        a64_subs_reg(b, 1, A64_ZR, JTT, JTU, 0);
        to_native = a64_label(b);
        a64_bcond(b, A64_CC, 0);   /* (gaddr-LOW_LIMIT) < (TOP_LO-LOW_LIMIT) -> main arena -> native */
    }
    a64_mov_imm64(b, JTU, OCERZ_COMMPAGE_LO);
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
        a64_ldr(b, s->size == 8 ? 8 : 4, JT1, 20, GPR_OFF(s->reg));
        a64_mov_imm64(b, JTU, gbase);
        a64_add_reg(b, 1, JTA, JTA, JTU, 0);
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
        a64_mov_imm64(b, JTU, gbase);
        a64_add_reg(b, 1, JTA, JTA, JTU, 0);
        /* GUEST load (JTA = full host address, no displacement), load-acquire
         * ordered so plain x86 loads keep Total Store Order. JTU is dead here
         * (held gbase) and serves as the helper's alignment-check scratch. */
        emit_guest_load_ordered(b, d->size, JT1, JTA, JTU);
        a64_str(b, 8, JT1, 20, GPR_OFF(d->reg));
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
static int emit_arith_mem(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
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
    a64_mov_imm64(b, JTU, ocerz_guest_base);
    a64_add_reg(b, 1, JTA, JTA, JTU, 0);
    /* GUEST load (JTA = full host address, no displacement), load-acquire
     * ordered so plain x86 loads keep Total Store Order. JTU is dead here (held
     * gbase) and serves as the helper's alignment-check scratch. The following
     * ldr reads the destination GPR from the register file, not guest memory. */
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

    a64_mov_imm64(b, JTF, 0);
    emit_zf_sf(b);
    emit_pf(b, JT2);
    if (is_add || is_sub) {
        a64_cset(b, JTT, is_add ? A64_CS : A64_CC);
        a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        a64_cset(b, JTT, A64_VS);
        a64_lsl_imm(b, 0, JTT, JTT, 11);
        a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
        a64_eor_reg(b, 1, JTT, JT0, JT1, 0);
        a64_eor_reg(b, 1, JTT, JTT, JT2, 0);
        a64_ubfx(b, 1, JTT, JTT, 4, 1);
        a64_lsl_imm(b, 0, JTT, JTT, 4);
        a64_orr_reg(b, 1, JTF, JTF, JTT, 0);
    }
    emit_commit_flags(b, JIT_ARITH_FLAGS);
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
            a64_ldr(b, 8, JT0, 20, GPR_OFF(s->base));
            a64_add_reg(b, 1, JT2, JT2, JT0, 0);
        }
        if (s->index != OCERZ_REG_NONE) {
            a64_ldr(b, 8, JT0, 20, GPR_OFF(s->index));
            a64_add_reg(b, 1, JT2, JT2, JT0, s->scale & 3);
        }
        if (insn->addrsize == 4)
            a64_mov_reg(b, 0, JT2, JT2);
    }
    if (d->size == 4)
        a64_mov_reg(b, 0, JT2, JT2);
    a64_str(b, 8, JT2, 20, GPR_OFF(d->reg));
    return 1;
}

static int try_inline(A64Buf *b, const X86Insn *insn, uint32_t **exit_sites, int *n_exits)
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
            return emit_arith_mem(b, insn, exit_sites, n_exits);
        return emit_arith(b, insn);
    case OCERZ_OP_INC:
    case OCERZ_OP_DEC:
        return emit_incdec(b, insn);
    case OCERZ_OP_SHL:
    case OCERZ_OP_SHR:
    case OCERZ_OP_SAR:
        return emit_shift(b, insn);
    case OCERZ_OP_LEA:
        return emit_lea(b, insn);
    default:
        return 0;
    }
}

/* Native Jcc terminator. cpu->rflags is always architecturally current (eager
 * flags), so the condition is evaluated by reproducing ocerz_cc_eval's exact
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
    a64_mov_imm64(b, 0, OCERZ_STEP_OK);

    epilogue_sites[*n_epi] = a64_label(b);
    a64_b(b, 0);
    (*n_epi)++;
    return 1;
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
    uint32_t *epi_sites[JIT_MAX_BLOCK_INSNS];
    int n_exits = 0;
    int n_epi = 0;
    for (int i = 0; i < n; i++) {
        const X86Insn *insn = &blk->insns[i];
        if (i == n - 1 && insn->op == OCERZ_OP_JCC) {
            if (emit_jcc(&b, insn, epi_sites, &n_epi))
                continue;
        }
        if (!try_inline(&b, insn, exit_sites, &n_exits))
            emit_slowcall(&b, insn, exit_sites, &n_exits);
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
        a64_mov_imm64(&b, JT0, pc);
        a64_str(&b, 8, JT0, 20, RIP_OFF);
    }

    uint32_t *exit_label = a64_label(&b);
    a64_ldp_post(&b, 19, 20, 31, 16);
    a64_ldp_post(&b, 29, 30, 31, 16);
    a64_ret(&b);

    for (int i = 0; i < n_exits; i++)
        a64_patch_cbz(exit_sites[i], exit_label);
    for (int i = 0; i < n_epi; i++)
        a64_patch_b(epi_sites[i], exit_label);

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

int ocerz_jit_step(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (cpu->rip - OCERZ_DYLDAPI_LO < (OCERZ_DYLDAPI_HI - OCERZ_DYLDAPI_LO))
        return OCERZ_EUNSUP;
    OcerzJit *jit = vm->jit;
    JitBlock *b = cache_lookup(jit, cpu->rip);
    if (!b) {
        pthread_mutex_lock(&jit_lock);
        b = cache_lookup(jit, cpu->rip);
        if (!b)
            b = translate(jit, cpu->rip);
        pthread_mutex_unlock(&jit_lock);
    }
    if (!b)
        return OCERZ_EUNSUP;
    return b->code(vm, cpu);
}

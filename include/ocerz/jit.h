/*
 * include/ocerz/jit.h
 *
 * The JIT tier: translates basic blocks of guest x86_64 code into native
 * arm64 code at runtime, the way Rosetta 2's JIT path does, with the
 * interpreter as semantic fallback for anything not yet translated.
 *
 * Model: a block is a straight-line run of guest instructions ending at the
 * first control-flow instruction (or at an instruction the translator does
 * not cover, in which case the block ends early and the run loop interprets
 * the next instruction before looking up a block again). Translated blocks
 * live in one MAP_JIT code buffer; pthread_jit_write_protect_np toggles
 * W^X around emission and sys_icache_invalidate publishes the new code.
 *
 * Translated code keeps ALL guest state in the OcerzCPU structure (memory-
 * backed register file, tier-1 design): every guest register read/write is
 * an ldr/str against the cpu pointer held in x19, guest_base lives in x20,
 * scratch values use x0..x15. Flags are kept eagerly correct by computing
 * them into cpu->rflags exactly as the interpreter's flags.c does, via
 * inline NZCV-based sequences for the common ops; blocks therefore can be
 * entered and exited at any boundary with the interpreter taking over
 * seamlessly.
 *
 * ocerz_jit_step() looks up (or translates, when the per-rip execution
 * counter crosses the hot threshold) the block for cpu->rip and runs it,
 * returning an OcerzStep code, or returns OCERZ_EUNSUP when it has no block
 * and chose not to translate — the caller then interprets. Blocks return
 * the next guest rip in the CPU state.
 *
 * BLOCK CHAINING IS NOT IMPLEMENTED. Every block exit returns to the
 * dispatcher and pays a hash lookup; measured at 1.470e9 exits with 1.00006
 * lookups per exit in a real app, i.e. literally none are chained. This comment
 * previously claimed chaining existed — it never did. It remains the largest
 * single structural optimization still on the table (~20% of on-CPU), and the
 * interfaces here do not change when it lands.
 *
 * ocerz_jit_prefork()/ocerz_jit_postfork() bracket a guest fork(): the
 * translation mutex is taken across the host fork so the child never inherits
 * a lock held by a worker thread that does not exist in the child.
 */
#ifndef OCERZ_JIT_H
#define OCERZ_JIT_H

#include <setjmp.h>

#include "ocerz/cpu.h"

struct OcerzVM;

/* Active (non-NULL) only while the block translator is reading guest code to
 * decode an instruction; the host fault handler unwinds to it instead of
 * delivering a guest signal, so a fault during translation never escapes
 * through the held translation lock. See translate() in jit.c. */
extern __thread sigjmp_buf *ocerz_jit_decode_recover;

typedef struct OcerzJit OcerzJit;

OcerzJit *ocerz_jit_create(struct OcerzVM *vm);
void ocerz_jit_destroy(OcerzJit *jit);
int ocerz_jit_step(struct OcerzVM *vm, OcerzCPU *cpu);
uint64_t ocerz_jit_blocks(const OcerzJit *jit);
void ocerz_jit_prefork(void);
void ocerz_jit_postfork(void);

/* Exact guest rip for a host pc inside emitted JIT code, for the fault handler.
 * Inlined instructions never write cpu->rip (that store is what the JIT exists
 * to avoid), so a fault in inlined code cannot read the faulting rip out of the
 * CPU; this recovers it from the block's host-pc -> guest-rip side table.
 * Returns 1 and sets *out_rip on success, 0 if host_pc is not in a compiled
 * block. Lock-free and allocation-free: safe from a signal handler. */
int ocerz_jit_fault_rip(const struct OcerzVM *vm, const void *host_pc, uint64_t *out_rip);

/* True when host_pc lies inside the emitted-code arena. Distinguishes "the
 * fault came from JIT'd code" from "the fault came from ocerz's own C code
 * (i.e. the interpreter slow path)", which is what tells the fault handler
 * whether cpu->cur_rip is authoritative. */
int ocerz_jit_pc_in_arena(const struct OcerzVM *vm, const void *host_pc);

/* FAULT SEAM for register pinning. A fault in emitted JIT code may leave a hot
 * subset of the guest GPRs live in the host callee-saved registers x21..x28
 * rather than in cpu->gpr[]. Given the faulting host pc and a pointer to the
 * host mcontext register array (&uc->uc_mcontext->__ss.__x[0], a uint64_t[29]),
 * this writes each pinned guest reg back into cpu->gpr[] so a delivered guest
 * signal frame and any register dump observe the true values. Must be called
 * only when host_pc is in the arena (ocerz_jit_pc_in_arena); a no-op for a
 * block that pins nothing. Lock-free and allocation-free: safe from a signal
 * handler. */
void ocerz_jit_fault_recover_regs(const struct OcerzVM *vm, const void *host_pc,
                                  const uint64_t *host_x, OcerzCPU *cpu);

#endif

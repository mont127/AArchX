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
 * the next guest rip in the CPU state; direct-branch chaining patches block
 * exits to jump straight to successor blocks inside the code cache.
 *
 * ocerz_jit_prefork()/ocerz_jit_postfork() bracket a guest fork(): the
 * translation mutex is taken across the host fork so the child never inherits
 * a lock held by a worker thread that does not exist in the child.
 */
#ifndef OCERZ_JIT_H
#define OCERZ_JIT_H

#include "ocerz/cpu.h"

struct OcerzVM;

typedef struct OcerzJit OcerzJit;

OcerzJit *ocerz_jit_create(struct OcerzVM *vm);
void ocerz_jit_destroy(OcerzJit *jit);
int ocerz_jit_step(struct OcerzVM *vm, OcerzCPU *cpu);
uint64_t ocerz_jit_blocks(const OcerzJit *jit);
void ocerz_jit_prefork(void);
void ocerz_jit_postfork(void);

#endif

/* The JIT tier: basic blocks of guest x86_64 translated to native arm64. */
#ifndef OCERZ_JIT_H
#define OCERZ_JIT_H

#include <setjmp.h>

#include "ocerz/cpu.h"

struct OcerzVM;

extern __thread sigjmp_buf *ocerz_jit_decode_recover;

typedef struct OcerzJit OcerzJit;

OcerzJit *ocerz_jit_create(struct OcerzVM *vm);
void ocerz_jit_destroy(OcerzJit *jit);
int ocerz_jit_step(struct OcerzVM *vm, OcerzCPU *cpu);
uint64_t ocerz_jit_blocks(const OcerzJit *jit);
void ocerz_jit_prefork(void);
void ocerz_jit_postfork(void);

void ocerz_jit_request_stop(struct OcerzVM *vm);

void ocerz_jit_require_ordered(struct OcerzVM *vm);

int ocerz_jit_fault_rip(const struct OcerzVM *vm, const void *host_pc, uint64_t *out_rip);

int ocerz_jit_pc_in_arena(const struct OcerzVM *vm, const void *host_pc);

void ocerz_jit_fault_recover_regs(const struct OcerzVM *vm, const void *host_pc,
                                  const uint64_t *host_x, OcerzCPU *cpu);

void ocerz_jit_fault_recover_flags(const struct OcerzVM *vm,
                                   const void *host_pc, OcerzCPU *cpu);

#endif

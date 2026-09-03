/* The interpreter tier: fetch, decode, dispatch, execute one instruction. */
#ifndef OCERZ_INTERP_H
#define OCERZ_INTERP_H

#include "ocerz/cpu.h"
#include "ocerz/decode.h"

enum OcerzStep {
    OCERZ_STEP_OK = 0,
    OCERZ_STEP_EXIT = 1,
    OCERZ_STEP_FATAL = 2,
    OCERZ_STEP_REDIRECT = 3,   /* a non-terminator changed rip (guest signal delivered): re-dispatch */
    OCERZ_STEP_PROFILE = 4,    /* a profiled JIT side exit: came back to C to be counted, then continue */
};

struct OcerzVM;

int ocerz_interp_step(struct OcerzVM *vm, OcerzCPU *cpu);

int ocerz_interp_exec(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn * restrict insn);
int ocerz_interp_ext(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn);
int ocerz_interp_sse(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn);

#endif

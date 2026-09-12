/*
 * The interpreter tier: fetch, decode, dispatch, execute one instruction.
 *
 * Two step results are not ordinary control flow: a non-terminator that changed
 * rip means a guest signal was delivered and the caller must re-dispatch, and a
 * profiled JIT side exit means execution came back to C only to be counted and
 * should then continue.
 */
#ifndef OCERZ_INTERP_H
#define OCERZ_INTERP_H

#include "ocerz/cpu.h"
#include "ocerz/decode.h"

enum OcerzStep {
    OCERZ_STEP_OK = 0,
    OCERZ_STEP_EXIT = 1,
    OCERZ_STEP_FATAL = 2,
    OCERZ_STEP_REDIRECT = 3,
    OCERZ_STEP_PROFILE = 4,
};

struct OcerzVM;

int ocerz_interp_step(struct OcerzVM *vm, OcerzCPU *cpu);

int ocerz_interp_exec(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn * restrict insn);
int ocerz_interp_ext(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn);
int ocerz_interp_sse(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn);

#endif

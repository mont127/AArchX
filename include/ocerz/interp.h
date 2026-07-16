/*
 * include/ocerz/interp.h
 *
 * The interpreter tier: fetch, decode, dispatch, execute one guest
 * instruction at a time. This tier is the reference implementation of
 * instruction semantics — the JIT must agree with it bit for bit, and the
 * differential tests enforce that.
 *
 * ocerz_interp_step() decodes the instruction at cpu->rip, advances cpu->rip
 * by its length, executes it, and returns one of the OcerzStep codes.
 * OCERZ_STEP_OK means continue; OCERZ_STEP_EXIT means the guest requested
 * process exit (vm->exit_code is set); OCERZ_STEP_FATAL means emulation
 * cannot continue (undecodable or unimplemented instruction, diagnostic
 * already printed with the offending bytes and CPU state).
 *
 * Dispatch is split across three translation units to keep them
 * independently developable:
 *   - interp.c      core ALU, moves, stack, control flow, atomics, syscall
 *   - interp_ext.c  string ops, bit ops, CPUID/RDTSC/XGETBV and other misc,
 *                   and the x87 subset (every op between OCERZ_OP_X87_FIRST
 *                   and OCERZ_OP_SSE_FIRST)
 *   - interp_sse.c  every op at or above OCERZ_OP_SSE_FIRST
 * interp.c is the entry point; for ops it does not own it calls
 * ocerz_interp_ext()/ocerz_interp_sse(), which execute the instruction and
 * return an OcerzStep code, or return OCERZ_EUNSUP if the op is not theirs
 * (interp.c then reports a fatal unimplemented-instruction diagnostic).
 */
#ifndef OCERZ_INTERP_H
#define OCERZ_INTERP_H

#include "ocerz/cpu.h"
#include "ocerz/decode.h"

enum OcerzStep {
    OCERZ_STEP_OK = 0,
    OCERZ_STEP_EXIT = 1,
    OCERZ_STEP_FATAL = 2,
};

struct OcerzVM;

int ocerz_interp_step(struct OcerzVM *vm, OcerzCPU *cpu);
/* NOTE THE `restrict`, AND WHAT IT PROMISES. It says: for the whole call, the
 * decoded instruction is not reachable through vm, cpu, or guest memory, so no
 * handler's write can change it. That lets the dispatcher read insn->op
 * directly instead of copying all 96 bytes to the stack first (three ldp/stp
 * q-register pairs, a 336-byte frame, and a store->load forwarding stall on the
 * dispatch index) on EVERY interpreted instruction.
 *
 * It is legal only because of where the X86Insn lives, and there are exactly
 * two legal caller classes:
 *   - ocerz_interp_step's own host stack slot (src/interp.c), and
 *   - a JIT block's malloc'd blk->insns array (src/jit.c).
 * Both are ocerz HOST memory. Handlers only ever write through cpu, vm, or
 * ocerz_g2h's guest windows, and ocerz_host_in_guest_space (mem.h) is the
 * invariant that keeps those windows disjoint from the host heap and stack.
 * Self-modifying guest code rewrites guest BYTES, not the host-side decoded
 * X86Insn, so it cannot alias either.
 *
 * A NEW CALLER THAT PASSES AN X86Insn LIVING IN GUEST MEMORY WOULD SILENTLY
 * BREAK THIS, and the compiler will not warn. Don't. */
int ocerz_interp_exec(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn * restrict insn);
int ocerz_interp_ext(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn);
int ocerz_interp_sse(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn);

#endif

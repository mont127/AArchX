/*
 * Caller-address wrappers for the Rust JIT-control port. Rust cannot capture
 * __builtin_return_address(0), which fault and park paths use to identify the
 * caller.
 */
#include "ocerz/jit_internal.h"

__attribute__((noinline))
int ocerz_jit_exec_one(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    return jit_exec_one_parked(vm, cpu, insn, __builtin_return_address(0));
}

__attribute__((noinline))
int ocerz_jit_exec_one_at(struct OcerzVM *vm, OcerzCPU *cpu, const JitBlock *b,
                          uint64_t idx)
{
    return jit_exec_one_parked(vm, cpu, blk_insn_full(b, (int)idx),
                               __builtin_return_address(0));
}

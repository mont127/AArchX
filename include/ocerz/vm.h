/*
 * include/ocerz/vm.h
 *
 * The per-process emulation context tying everything together: one guest
 * process = one OcerzVM. It owns the (currently single) virtual CPU, the
 * loaded image description, run-state, options, and counters.
 *
 * ocerz_vm_init() resets the CPU and zeroes run state. ocerz_vm_run() is
 * the master loop: while the guest has not exited it executes through the
 * JIT tier when a translated block exists for cpu->rip (vm->jit_enabled and
 * the block cache decide), falling back to single-step interpretation
 * otherwise. The loop owns the lifecycle: STEP_EXIT ends it with
 * vm->exit_code, STEP_FATAL ends it with code 125 after diagnostics.
 *
 * ocerz_vm_request_exit() is how the syscall layer reports guest exit(2);
 * it never terminates the emulator process directly, so the run loop can
 * print statistics and shut down cleanly.
 *
 * A host SIGSEGV/SIGBUS handler is installed by ocerz_vm_run(): a wild
 * guest access faults in the host process, and the handler prints the
 * guest rip and faulting host address before exiting with 139, which makes
 * guest crashes diagnosable instead of silent.
 *
 * Options: trace prints each instruction before execution (-trace), strace
 * logs syscalls (-strace), jit_enabled gates the JIT tier (-no-jit clears
 * it). insn_count counts interpreted plus JIT-executed guest instructions
 * where cheaply available (JIT blocks count their instruction totals on
 * exit).
 */
#ifndef OCERZ_VM_H
#define OCERZ_VM_H

#include "ocerz/cpu.h"
#include "ocerz/loader.h"

struct OcerzJit;

typedef struct OcerzVM {
    OcerzCPU cpu;
    OcerzImage image;
    struct OcerzJit *jit;
    int exited;
    int exit_code;
    int trace;
    int strace;
    int jit_enabled;
    _Atomic int jit_plain_mem;
    _Atomic int jit_ordered_required;
    uint64_t insn_count;
    uint64_t stack_lo;
    uint64_t stack_hi;
} OcerzVM;

int ocerz_vm_init(OcerzVM *vm);
int ocerz_vm_run(OcerzVM *vm);
int ocerz_vm_run_cpu(OcerzVM *vm, OcerzCPU *cpu);
void ocerz_vm_request_exit(OcerzVM *vm, int code);
void ocerz_vm_install_handlers(OcerzVM *vm);
uint64_t ocerz_vm_call(OcerzVM *vm, uint64_t func, const uint64_t *args, int nargs, uint64_t stack_top);
unsigned ocerz_vm_riphist(uint64_t *out, unsigned max);
void ocerz_vm_purge_jit_ras(OcerzVM *vm);
extern int ocerz_init_tolerant;

#endif

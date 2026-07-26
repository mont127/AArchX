/* The per-process emulation context: one guest process is one OcerzVM. */
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

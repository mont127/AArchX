/*
 * The per-process emulation context: one guest process is one OcerzVM.
 *
 * The guest thread_suspend/thread_resume/thread_get_state entry points return
 * -1 to mean "not a thread running guest code", in which case the caller lets
 * the host kernel answer as it did before.
 *
 * ocerz_vm_call enters guest code with up to six integer arguments, which is all
 * an initializer or a +load method needs.  ocerz_vm_call_abi is the general form
 * native callbacks need: integer arguments in RDI..R9, floating-point ones in the
 * low halves of XMM0..XMM7, the rest on the guest stack where the System V ABI
 * puts them, and both RAX and XMM0 handed back, since the callee's signature and
 * not the call site decides which one carries the result.  ocerz_vm_current_cpu
 * is the guest cpu the calling thread is running, or NULL on a thread that has
 * none.
 *
 * A native framework calls guest callbacks on threads it created for itself:
 * libdispatch's workers, a run loop's thread, an audio device's real-time
 * thread.  Such a thread has no guest cpu, so it cannot enter guest code until
 * it is given one.  ocerz_thread_attach gives the calling host thread a guest
 * personality of its own, a cpu with its own guest stack and its own guest
 * thread block behind gs, registered like any other guest thread and returned
 * again on every later call from the same thread.  It is torn down when the
 * host thread exits, or earlier by ocerz_thread_detach, which only ever removes
 * a personality attach created and never a thread ocerz started itself.  A
 * second thread running guest code is a second observer of guest memory, so the
 * first attach also retires plain memory mode, exactly as starting a guest
 * thread does.  ocerz_vm_process is the process's VM, or NULL before one is
 * running.
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
void ocerz_vm_mirror_host_signal(int sig, int kind);
void ocerz_vm_install_handlers(OcerzVM *vm);
uint64_t ocerz_vm_call(OcerzVM *vm, uint64_t func, const uint64_t *args, int nargs, uint64_t stack_top);

typedef struct OcerzGuestCall {
    uint64_t gpr[6];
    uint64_t xmm[8];
    uint64_t stack[16];
    int nstack;
    uint64_t rax;
    uint64_t xmm0;
} OcerzGuestCall;

OcerzCPU *ocerz_vm_current_cpu(void);
OcerzVM *ocerz_vm_process(void);
OcerzCPU *ocerz_thread_attach(OcerzVM *vm);
void ocerz_thread_detach(void);
int ocerz_vm_call_abi(OcerzVM *vm, uint64_t func, OcerzGuestCall *call, uint64_t stack_top);
unsigned ocerz_vm_riphist(uint64_t *out, unsigned max);
void ocerz_vm_purge_jit_ras(OcerzVM *vm);
int ocerz_vm_thread_suspend(OcerzCPU *self, uint32_t port);
int ocerz_vm_thread_resume(uint32_t port);
int ocerz_vm_thread_regs(uint32_t port, uint64_t gpr[16], uint64_t *rip, uint64_t *rflags);
void ocerz_vm_suspend_point(OcerzCPU *cpu);
extern int ocerz_init_tolerant;

#endif

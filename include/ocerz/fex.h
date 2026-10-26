/*
 * -fex: guest x86-64 code runs on FEXCore, FEX-Emu's JIT library, instead of
 * ocerz's own translator and interpreter.  Everything around the CPU stays
 * ocerz's: the loader, guest memory, the system call layer, threads.
 *
 * ocerz_engine_run, when set, is what the run loop calls in place of a JIT or
 * interpreter step (src/vm.c).  It runs the thread's guest code until the guest
 * exits, its thread ends, a signal is waiting or something fatal happens, and
 * returns the step code that says which, with the guest state back in cpu.
 * main.c sets it to ocerz_fex_run for -fex; it is NULL otherwise.
 *
 * ocerz_engine_fault, when set, sees every SIGSEGV and SIGBUS first and says
 * whether it dealt with it.  FEXCore's translated code loads and stores with
 * ordered instructions that fault when misaligned across 16 bytes, and FEXCore
 * rewrites such an instruction in place once the frontend hands it the fault.
 *
 * FEXCore is linked only by `make FEX=<dir>` (OCERZ_HAVE_FEX); a plain build
 * has no -fex.
 */
#ifndef OCERZ_FEX_H
#define OCERZ_FEX_H

#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

struct OcerzVM;
struct OcerzCPU;

extern int (*ocerz_engine_run)(struct OcerzVM *vm, struct OcerzCPU *cpu);
extern int (*ocerz_engine_fault)(int sig, siginfo_t *si, void *uc);

int ocerz_fex_run(struct OcerzVM *vm, struct OcerzCPU *cpu);
int ocerz_fex_fault(int sig, siginfo_t *si, void *uc);

#ifdef __cplusplus
}
#endif

#endif

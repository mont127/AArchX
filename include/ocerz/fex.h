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
 * ocerz_engine_async, when set, is called from the handlers of the signals that
 * interrupt a running guest thread (a host signal for the guest, the kick an exit
 * request sends) with the interrupted context, so that a thread looping in
 * translated code, with no system call to notice anything at, leaves it.
 *
 * ocerz_engine_gprs_at, when set, reads a suspended guest thread's general
 * registers from its host thread state, as ocerz_jit_guest_gprs_at does for
 * ocerz's own code, and fails where they cannot be read, so the suspend tries
 * again.
 *
 * ocerz_engine_pc_in_code, when set, says whether a host pc is in the engine's
 * translated code on the calling thread, for the walks that look for a
 * translated frame beneath a native call.
 *
 * ocerz_engine_cpuid, when set, is the engine's CPUID, eax ebx ecx edx, which
 * the x86 hw.optional sysctls follow.
 *
 * ocerz_engine_invalidate, when set, hears every guest range ocerz drops
 * translations for (ocerz_jit_invalidate_range): a write to a page of guest
 * code, an unmap, a protection change.
 *
 * ocerz_engine_ldt, when set, hears every LDT entry ocerz sets, as the 8-byte
 * descriptor, so the engine's segment loads find what i386_set_ldt put there.
 *
 * FEXCore is linked only by `make FEX=<dir>` (OCERZ_HAVE_FEX); a plain build
 * has no -fex.  ocerz_fex_supported says whether this Mac can run it: FEXCore's
 * locks wait with os_sync_wait_on_address (macOS 14.4) and the memory it hands
 * back must read as zeros (MADV_ZERO).  A build for older systems still starts
 * on them, without -fex.
 */
#ifndef OCERZ_FEX_H
#define OCERZ_FEX_H

#include <signal.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct OcerzVM;
struct OcerzCPU;

extern int (*ocerz_engine_run)(struct OcerzVM *vm, struct OcerzCPU *cpu);
extern int (*ocerz_engine_fault)(int sig, siginfo_t *si, void *uc);
extern int (*ocerz_engine_async)(void *uc);
extern void (*ocerz_engine_invalidate)(uint64_t addr, uint64_t len);
extern void (*ocerz_engine_ldt)(uint32_t index, uint64_t desc);
extern int (*ocerz_engine_gprs_at)(struct OcerzCPU *cpu, const void *host_state, uint64_t out[16]);
extern void (*ocerz_engine_cpuid)(uint32_t leaf, uint32_t subleaf, uint32_t out[4]);
extern int (*ocerz_engine_pc_in_code)(const void *pc);

int ocerz_fex_run(struct OcerzVM *vm, struct OcerzCPU *cpu);
int ocerz_fex_fault(int sig, siginfo_t *si, void *uc);
int ocerz_fex_async(void *uc);
void ocerz_fex_invalidate(uint64_t addr, uint64_t len);
void ocerz_fex_ldt(uint32_t index, uint64_t desc);
int ocerz_fex_gprs_at(struct OcerzCPU *cpu, const void *host_state, uint64_t out[16]);
void ocerz_fex_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4]);
int ocerz_fex_pc_in_code(const void *pc);
int ocerz_fex_supported(const char **why);

#ifdef __cplusplus
}
#endif

#endif

/*
 * Guest syscall handling: emulated x86_64 syscalls onto the native arm64
 * kernel.
 *
 * Pending asynchronous signals are offered on the ENTRY edge of a syscall, not
 * only on return: the entry hook rewinds rip to the syscall instruction so the
 * call re-executes once the handler returns, and reports nonzero to say the
 * syscall must not run yet.  Whether any LDT descriptor is present tells the
 * rest of the emulator that this process has built the 32-bit world a WoW64
 * mode switch needs, and the signal frame's flavour is gated on it.
 */
#ifndef OCERZ_SYSCALL_H
#define OCERZ_SYSCALL_H

#include "ocerz/cpu.h"

struct OcerzVM;

int ocerz_handle_syscall(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_is_wqthread_exit(uint64_t rip);

#define OCERZ_SIGTRAP 5
#define OCERZ_SIGFPE  8
#define OCERZ_FPE_INTDIV 7
#define OCERZ_FPE_INTOVF 8
#define OCERZ_SIGSYS  12

int ocerz_signal_deliver(OcerzCPU *cpu, int sig, uint64_t fault_addr, int si_code,
                         uint32_t err);
int ocerz_signal_before_syscall(OcerzCPU *cpu, uint64_t insn_rip);

uint64_t ocerz_ldt_base(uint32_t sel);
int ocerz_ldt_is_big(uint32_t sel);
int ocerz_ldt_is_long(uint32_t sel);
int ocerz_ldt_installed(void);
void ocerz_ldt_install(uint32_t sel, uint64_t base, uint32_t limit,
                       uint8_t access, int big, int is_long, int gran);

#endif

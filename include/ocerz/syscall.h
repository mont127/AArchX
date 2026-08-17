/* Guest syscall handling: emulated x86_64 syscalls onto the native arm64 kernel. */
#ifndef OCERZ_SYSCALL_H
#define OCERZ_SYSCALL_H

#include "ocerz/cpu.h"

struct OcerzVM;

int ocerz_handle_syscall(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_is_wqthread_exit(uint64_t rip);

#define OCERZ_SIGTRAP 5
#define OCERZ_SIGFPE  8
#define OCERZ_FPE_INTDIV 7      /* Darwin si_code values */
#define OCERZ_FPE_INTOVF 8
#define OCERZ_SIGSYS  12

int ocerz_signal_deliver(OcerzCPU *cpu, int sig, uint64_t fault_addr, int si_code,
                         uint32_t err);

uint64_t ocerz_ldt_base(uint32_t sel);
int ocerz_ldt_is_big(uint32_t sel);

#endif

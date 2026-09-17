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
 *
 * Native mode reaches the same guest signal table through calls rather than
 * syscalls.  In cache mode a guest's sigaction goes through Apple's x86 libc,
 * which hands the kernel its own _sigtramp; native mode has no x86 libc, so
 * ocerz_native_sigtramp writes an equivalent trampoline into guest memory once,
 * and ocerz_guest_sigaction_user installs a handler from the user-level struct
 * sigaction with that trampoline filled in.  There is one table for both modes,
 * so a raw syscall and a bridged call can never disagree about a handler.  The
 * other entries mirror the syscalls a native-mode program would otherwise
 * make: they return 0 on success or a positive errno, and the bridge turns that
 * into -1 with errno set.  ocerz_guest_deliver_pending builds a frame for every
 * unmasked pending signal on the given cpu, as the syscall entry edge does, and
 * returns 1 when it built one; that is how a signal raised during a bridged
 * call, or one that arrived while the thread sat in native code, reaches its
 * handler when the crossing returns.
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

uint64_t ocerz_native_sigtramp(struct OcerzVM *vm);
int ocerz_guest_sigaction_user(struct OcerzVM *vm, OcerzCPU *cpu, int sig,
                               uint64_t act, uint64_t oact);
int ocerz_guest_signal(struct OcerzVM *vm, OcerzCPU *cpu, int sig, uint64_t handler,
                       uint64_t *old_handler);
int ocerz_guest_sigprocmask(struct OcerzVM *vm, OcerzCPU *cpu, int how,
                            uint64_t set, uint64_t oset);
int ocerz_guest_sigaltstack(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t ss, uint64_t oss);
int ocerz_guest_raise(struct OcerzVM *vm, OcerzCPU *cpu, int sig);
int ocerz_guest_pthread_kill(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t thread, int sig);
int ocerz_guest_deliver_pending(struct OcerzVM *vm, OcerzCPU *cpu);

uint64_t ocerz_ldt_base(uint32_t sel);
int ocerz_ldt_is_big(uint32_t sel);
int ocerz_ldt_is_long(uint32_t sel);
int ocerz_ldt_installed(void);
void ocerz_ldt_install(uint32_t sel, uint64_t base, uint32_t limit,
                       uint8_t access, int big, int is_long, int gran);

#endif

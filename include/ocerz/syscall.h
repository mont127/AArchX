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
 * handler when the crossing returns.  ocerz_guest_altstack_flags is the ss_flags
 * word sigaltstack would report, and ocerz_guest_set_onstack is what
 * sigreturn(NULL, UC_SET_ALT_STACK or UC_RESET_ALT_STACK) does, the call a
 * longjmp out of a handler uses to say the thread has left its alternate stack.
 *
 * Memory and processes reach ocerz's own implementations the same way.  In
 * cache mode an x86 libc turns mmap, fork, execve and posix_spawn into syscalls
 * and mach_vm_allocate into a Mach trap; native mode has no x86 libc, so the
 * bridge calls these entry points instead, and each is the syscall's or trap's
 * body with the register plumbing peeled off.  The memory entries keep guest
 * memory accounting, translation invalidation and the 4 KB guest page exactly as
 * the syscall path keeps them, for every range that touches memory ocerz
 * tracks; a range that lies wholly outside it is the host's own memory, a buffer
 * native malloc or a native Mach call handed the guest, and goes to the host
 * kernel after any translations of it are dropped.  The Mach entries answer a
 * kern_return_t and go to the host outright for a task other than this one.
 * ocerz_guest_fork is the fork syscall's body and answers the child 0 and the
 * parent the child's pid through pid_out; ocerz_fork_register installs ocerz's
 * own fork handlers, which native mode does before any guest code can register
 * one of its own, so that ocerz's prepare handler runs after every guest one
 * and its child handler before every guest one.  ocerz_guest_execve and
 * ocerz_guest_posix_spawn take host argument and environment vectors and start
 * the program under ocerz exactly as the syscalls do; execve returns only on
 * failure.  posix_spawn's attributes are read through the host's own getters,
 * since in native mode the guest built them with the host's
 * posix_spawnattr_init.  All of these answer 0 or an errno, as the signal
 * entries do.
 */
#ifndef OCERZ_SYSCALL_H
#define OCERZ_SYSCALL_H

#include "ocerz/cpu.h"

#include <spawn.h>

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
uint32_t ocerz_guest_altstack_flags(const OcerzCPU *cpu);
void ocerz_guest_set_onstack(OcerzCPU *cpu, int on);

int ocerz_guest_mmap(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t addr, uint64_t len, int prot,
                     int flags, int fd, uint64_t off, uint64_t *out);
int ocerz_guest_munmap(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t addr, uint64_t len);
int ocerz_guest_mprotect(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t addr, uint64_t len, int prot);
int ocerz_guest_madvise(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t addr, uint64_t len, int advice);
int ocerz_guest_vm_allocate(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t task, uint64_t addrp,
                            uint64_t size, int flags);
int ocerz_guest_vm_deallocate(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t task, uint64_t addr,
                              uint64_t size);
int ocerz_guest_vm_protect(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t task, uint64_t addr,
                           uint64_t size, int set_maximum, int prot);

void ocerz_fork_register(void);
int ocerz_guest_fork(struct OcerzVM *vm, OcerzCPU *cpu, int *pid_out);
int ocerz_guest_execve(struct OcerzVM *vm, OcerzCPU *cpu, const char *path, char *const *argv,
                       char *const *envp);
int ocerz_guest_posix_spawn(struct OcerzVM *vm, OcerzCPU *cpu, int *pid, const char *path,
                            const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *attr,
                            char *const *argv, char *const *envp);

uint64_t ocerz_ldt_base(uint32_t sel);
int ocerz_ldt_is_big(uint32_t sel);
int ocerz_ldt_is_long(uint32_t sel);
int ocerz_ldt_installed(void);
void ocerz_ldt_install(uint32_t sel, uint64_t base, uint32_t limit,
                       uint8_t access, int big, int is_long, int gran);

#endif

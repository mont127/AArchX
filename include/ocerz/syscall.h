/*
 * include/ocerz/syscall.h
 *
 * Guest syscall handling: the bridge between emulated x86_64 macOS syscalls
 * and the native arm64 XNU kernel of the same machine.
 *
 * The guest executes SYSCALL with rax = (class << 24) | number, class 1 =
 * Mach trap, 2 = BSD/Unix, 3 = machine-dependent. Arguments arrive in
 * rdi, rsi, rdx, r10, r8, r9 with any further arguments on the guest stack.
 * Because macOS is LP64 on both architectures with identical struct layouts
 * and identical syscall numbers, most BSD syscalls and Mach traps can be
 * forwarded directly to the native kernel via svc #0x80 — after translating
 * every pointer argument through the guest_base offset (ocerz_g2h).
 *
 * ocerz_handle_syscall() dispatches through a table keyed by BSD syscall
 * number. Each entry declares the argument count, a bitmask of which
 * arguments are pointers (translated with g2h; NULL stays NULL), whether
 * the syscall returns a second value in rdx (fork, pipe), and an optional
 * intercept handler for calls that must NOT be blindly forwarded:
 *   - mmap/munmap/mprotect: page-size and arena policy (see mem.h)
 *   - syscalls registering guest code pointers with the kernel
 *     (sigaction, bsdthread_register, bsdthread_create, workq_kernreturn)
 *   - shared_region_check_np: must expose the manually-mapped x86_64 cache
 *   - exit: ends the run loop instead of killing the emulator
 * Unknown syscall numbers fail loudly (diagnostic + STEP_FATAL) rather than
 * forwarding blind, because untranslated pointer args would corrupt the
 * host.
 *
 * BSD error convention is replicated exactly: on failure the carry flag is
 * set in guest rflags.CF and rax holds errno; on success CF is cleared.
 * Mach traps (class 1) return kern_return_t in rax with no carry protocol.
 * Machine-dependent trap 3 (thread_fast_set_cthread_self) is intercepted to
 * set cpu->gs_base, which is how x86_64 macOS TLS (%gs:0) works.
 *
 * Returns an OcerzStep code: OK to continue, EXIT when the guest exited
 * (vm->exit_code set), FATAL on unforwardable/unknown syscalls.
 */
#ifndef OCERZ_SYSCALL_H
#define OCERZ_SYSCALL_H

#include "ocerz/cpu.h"

struct OcerzVM;

int ocerz_handle_syscall(struct OcerzVM *vm, OcerzCPU *cpu);

#define OCERZ_SIGTRAP 5
#define OCERZ_SIGSYS  12

/* Build a Darwin signal frame for `cpu` and redirect it to the guest's
 * registered handler/_sigtramp for `sig`; returns 1 if a handler was registered
 * (the fault was converted to a guest signal), 0 otherwise. Called from the host
 * SIGSEGV/SIGBUS crash handler. */
int ocerz_signal_deliver(OcerzCPU *cpu, int sig, uint64_t fault_addr, int si_code,
                         uint32_t err);


/* Synthetic LDT (src/syscall.c), populated by the guest through machdep i386_set_ldt.
 * Wine's WoW64 describes its 32-bit code and per-thread 32-bit TEB with these. */
uint64_t ocerz_ldt_base(uint32_t sel);
int ocerz_ldt_is_big(uint32_t sel);

#endif

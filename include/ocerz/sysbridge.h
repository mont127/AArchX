/*
 * The libSystem functions native mode answers itself because no generic
 * crossing can: the variadic file and IPC calls whose optional argument is
 * fixed, memory mapping, non-local jumps and process creation.
 *
 * Each function here is the handler of a special record in the libSystem
 * database (apidb.h), entered the way every special is: the guest has just made
 * a System V call, its arguments are in the x86 registers and on its stack
 * above the return address, and the handler consumes that return address and
 * leaves its result in rax through ocerz_bridge_return, then lets any pending
 * guest signal in through ocerz_bridge_settle.  The functions that can fail the
 * POSIX way leave errno as the host call they made left it, and errno is the
 * host's, since ___error is bridged to the host's __error.
 *
 * src/sysbridge.c says what each group does, where it takes ocerz's own
 * implementation rather than the host's, and what it refuses.
 */
#ifndef OCERZ_SYSBRIDGE_H
#define OCERZ_SYSBRIDGE_H

#include "ocerz/types.h"
#include "ocerz/cpu.h"

struct OcerzVM;

int ocerz_sys_open(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_open_nocancel(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_openat(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_openat_nocancel(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_open_dprotected_np(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_openat_dprotected_np(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_fcntl(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_fcntl_nocancel(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_ioctl(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_sem_open(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_shm_open(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_semctl(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_ulimit(struct OcerzVM *vm, OcerzCPU *cpu);

int ocerz_sys_mmap(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_munmap(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_mprotect(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_madvise(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_vm_allocate(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_vm_deallocate(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_vm_protect(struct OcerzVM *vm, OcerzCPU *cpu);

int ocerz_sys_setjmp(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys__setjmp(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_sigsetjmp(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_longjmp(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys__longjmp(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_siglongjmp(struct OcerzVM *vm, OcerzCPU *cpu);

int ocerz_sys_fork(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_execve(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_execv(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_execvp(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_execvP(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_execl(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_execle(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_execlp(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_posix_spawn(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_posix_spawnp(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_system(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_popen(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_sys_pclose(struct OcerzVM *vm, OcerzCPU *cpu);

#define OCERZ_JB_MASK 80
#define OCERZ_JB_SAVEMASK 84
#define OCERZ_JB_ONSSTACK 88
#define OCERZ_JB_OCERZ_MAGIC 96
#define OCERZ_JB_OCERZ_LEVEL 104
#define OCERZ_JB_MAGIC 0x4a4d50427a65636full

#endif

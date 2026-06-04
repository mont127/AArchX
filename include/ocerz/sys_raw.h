/*
 * include/ocerz/sys_raw.h
 *
 * The single point where Ocerz itself enters the arm64 XNU kernel on behalf
 * of the guest, bypassing libSystem so the exact register-level syscall
 * contract is under our control.
 *
 * arm64 XNU convention: syscall number in x16 (positive BSD numbers,
 * NEGATED Mach trap numbers), up to eight arguments in x0..x7, entry via
 * svc #0x80. On return the carry flag (PSTATE.C) signals a BSD error, in
 * which case x0 holds errno; on success x0 (and for the few dual-return
 * syscalls also x1) hold the result. The cset instruction captures the
 * carry inside the same asm block so no compiler-inserted instruction can
 * clobber PSTATE between the svc and the test — this mirrors the
 * "b.cs cerror" sequence in Apple's Libsyscall stubs.
 *
 * ocerz_host_syscall() is used for BSD (x16 > 0) and machine-dependent
 * forwards; ocerz_host_mach_trap() negates the trap number and ignores the
 * carry, matching kern_return_t semantics.
 */
#ifndef OCERZ_SYS_RAW_H
#define OCERZ_SYS_RAW_H

#include "ocerz/types.h"

static inline uint64_t ocerz_host_syscall(int64_t num, const uint64_t a[8], uint64_t *ret2, int *err)
{
    register uint64_t r0 __asm__("x0") = a[0];
    register uint64_t r1 __asm__("x1") = a[1];
    register uint64_t r2 __asm__("x2") = a[2];
    register uint64_t r3 __asm__("x3") = a[3];
    register uint64_t r4 __asm__("x4") = a[4];
    register uint64_t r5 __asm__("x5") = a[5];
    register uint64_t r6 __asm__("x6") = a[6];
    register uint64_t r7 __asm__("x7") = a[7];
    register int64_t rn __asm__("x16") = num;
    uint64_t carry;
    __asm__ volatile(
        "svc #0x80\n\t"
        "cset %[c], cs"
        : "+r"(r0), "+r"(r1), [c] "=r"(carry)
        : "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r6), "r"(r7), "r"(rn)
        : "memory", "cc");
    if (ret2)
        *ret2 = r1;
    if (err)
        *err = (int)carry;
    return r0;
}

static inline uint64_t ocerz_host_mach_trap(int64_t trap, const uint64_t a[8])
{
    return ocerz_host_syscall(-trap, a, NULL, NULL);
}

#endif

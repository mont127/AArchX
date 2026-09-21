/*
 * The routines translated code calls with the guest's registers in place.
 *
 * src/leaf.s holds arm64 versions of the string and memory routines a guest
 * calls most and that do least: they read rdi, rsi and rdx from the host
 * registers full pinning keeps them in, leave the result in the one that holds
 * rax, and change nothing else the translation depends on, so src/jit.c calls
 * one from inside a stub's block without spilling anything.  The file's own
 * prose states the contract.  None of them is a C function and none may be
 * called as one; they are declared here only so their addresses can be taken.
 * ocerz_leaf_call is the exception, an ordinary function the tests use to run
 * one with three argument values and read its result; ocerz_leaf_declined
 * holds, after such a call to memmove or memset, whether the routine declined.
 *
 * ocerz_leaf_lo and ocerz_leaf_hi bound their code.  The translator does not
 * branch to it there.  src/jit.c copies those bytes, which refer to nothing
 * outside themselves, to the start of its code arena, where a block reaches a
 * routine with a direct branch and the distance between the two does not change
 * from one run to the next; ocerz_leaf_near_lo and ocerz_leaf_near_hi bound the
 * copy, and are zero until there is one.  ocerz_leaf_site answers,
 * for a host program counter and the link register that goes with it, the
 * program counter to attribute the moment to: the instruction that branched to
 * the routine when the counter lies within either pair of bounds, which the routines
 * make recoverable by never writing the link register, and the counter itself
 * otherwise.  src/vm.c uses it wherever it maps a host program counter to a
 * translated block, so a fault in a routine or a thread stopped in one is
 * reported as the stub's instruction with the guest's registers intact.
 */
#ifndef OCERZ_LEAF_H
#define OCERZ_LEAF_H

#include <stdint.h>

extern const char ocerz_leaf_lo[];
extern const char ocerz_leaf_hi[];

void ocerz_leaf_strlen(void);
void ocerz_leaf_strnlen(void);
void ocerz_leaf_strcmp(void);
void ocerz_leaf_strncmp(void);
void ocerz_leaf_memcmp(void);
void ocerz_leaf_strchr(void);
void ocerz_leaf_memchr(void);
void ocerz_leaf_memmove(void);
void ocerz_leaf_memset(void);

uint64_t ocerz_leaf_call(void (*routine)(void), uint64_t rdi, uint64_t rsi, uint64_t rdx);
extern uint64_t ocerz_leaf_declined;

extern uint64_t ocerz_leaf_near_lo, ocerz_leaf_near_hi;

static inline uint64_t ocerz_leaf_site(uint64_t pc, uint64_t lr)
{
    uint64_t at = pc & 0x0000ffffffffffffull;
    if ((at >= (uint64_t)(uintptr_t)ocerz_leaf_lo && at < (uint64_t)(uintptr_t)ocerz_leaf_hi) ||
        (at >= __atomic_load_n(&ocerz_leaf_near_lo, __ATOMIC_ACQUIRE) &&
         at < __atomic_load_n(&ocerz_leaf_near_hi, __ATOMIC_ACQUIRE)))
        return (lr & 0x0000ffffffffffffull) - 4;
    return pc;
}

#endif

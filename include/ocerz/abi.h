/*
 * Moving a call from the x86-64 System V ABI to the arm64 AAPCS64 one.
 *
 * The bridge's first version read arguments straight out of RDI, RSI and the
 * rest, which is right only for functions whose arguments are all integers or
 * pointers.  As soon as a double appears the two ABIs stop agreeing: x86-64 puts
 * it in XMM0 while arm64 wants it in V0, and each side counts its integer and
 * its floating-point arguments in separate sequences, so one double in the
 * middle of a signature shifts nothing on one side and everything on the other.
 * Spilled arguments differ again, since the two disagree about which arguments
 * overflow to the stack and in what order.  So the mapping has to be computed
 * from a signature rather than assumed.
 *
 * A signature is written as a result class followed by its argument classes in
 * parentheses, for example i(pp) for strcmp, p(pLL) for memcpy, d(d) for sin and
 * v() for a function taking and returning nothing.  The classes are:
 *
 *     v  nothing, valid only as a result
 *     i  32-bit signed         u  32-bit unsigned
 *     l  64-bit signed         L  64-bit unsigned
 *     p  pointer, converted between the guest and host views
 *     f  32-bit float          d  64-bit double
 *
 * Reading the guest side means walking the arguments in order, handing each to
 * the next free integer register of RDI, RSI, RDX, RCX, R8, R9 or the next free
 * XMM of XMM0 to XMM7 according to its class, and taking anything past those
 * from the guest stack above the return address.  Writing the host side is the
 * same walk against x0 to x7 and v0 to v7, with its own overflow onto a real
 * stack the assembly caller builds.  Because the two walks consume their
 * registers independently, the nth argument rarely lands in the nth register on
 * either side, which is the whole reason this file exists.
 *
 * Floating-point values travel as raw bit patterns rather than as C doubles, so
 * that nothing is silently converted on the way through.  A float occupies the
 * low 32 bits of its slot, which is exactly where the arm64 s register that a
 * float argument is passed in lives inside its v register.
 *
 * The guest may have put the machine in a non-default rounding mode through
 * MXCSR, which the JIT honours by driving the host FPCR.  A host function
 * compiled for the default mode must not inherit that, so the crossing saves the
 * rounding mode, restores the default for the duration of the call, and puts the
 * guest's back afterwards.
 *
 * Structures passed or returned by value are deliberately not handled yet.  Both
 * ABIs split a small structure into pieces and classify each piece, and they
 * disagree about how, so that belongs in its own change with its own generated
 * tests rather than being smuggled in here.  A signature naming one is rejected
 * at parse time, which keeps an unsupported call an honest refusal instead of a
 * silently wrong one.
 */
#ifndef OCERZ_ABI_H
#define OCERZ_ABI_H

#include "ocerz/types.h"
#include "ocerz/cpu.h"

#define OCERZ_ABI_MAX_ARGS 16
#define OCERZ_ABI_MAX_STACK 16

typedef struct OcerzAbiSig {
    char ret;
    char arg[OCERZ_ABI_MAX_ARGS];
    int nargs;
} OcerzAbiSig;

typedef struct OcerzAbiCall {
    uint64_t x[8];
    uint64_t v[8];
    uint64_t stack[OCERZ_ABI_MAX_STACK];
    int nx;
    int nv;
    int nstack;
} OcerzAbiCall;

int ocerz_abi_parse(const char *notation, OcerzAbiSig *out);

int ocerz_abi_read_guest(const OcerzAbiSig *sig, const OcerzCPU *cpu,
                         OcerzAbiCall *call);

void ocerz_abi_write_result(const OcerzAbiSig *sig, OcerzCPU *cpu,
                            uint64_t rx, uint64_t rv);

int ocerz_abi_perform(const OcerzAbiSig *sig, const void *fn, OcerzCPU *cpu);

void ocerz_abi_call_native(const void *fn, const uint64_t *x, const uint64_t *v,
                           const uint64_t *stack, uint64_t stackbytes,
                           uint64_t *out_x0, uint64_t *out_v0);

#endif

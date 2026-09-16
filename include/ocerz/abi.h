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
 *
 * ---- calls in the other direction ----
 * Some native functions take a function pointer and call it: qsort calls its
 * comparator, a run loop calls its observer.  When the guest supplies one, the
 * pointer is x86 code, and native code cannot jump to it.  So an argument of
 * class c is not converted like a pointer.  It carries its own signature in
 * braces, as in v(pLLc{i(pp)}) for qsort, and it is interned: the guest function
 * and that signature are bound to one slot of a fixed bank of arm64 trampolines,
 * and the slot's address is what the native callee receives.  Interning the same
 * function with the same signature twice returns the same address, so a native
 * library that compares callback pointers still sees one function.  A null
 * pointer stays null.  A nested signature may not itself name a callback.
 *
 * The bank is ordinary code assembled into ocerz, not code generated at run time.
 * Generating a trampoline from inside a native callback would race the
 * translator for the one writable JIT arena, a fork child abandons that arena
 * while native code may still hold pointers into it, and a real-time audio
 * callback cannot wait for code generation at all.  Each slot is two
 * instructions and finds its own index from its own address, so the bank is one
 * repeated block and exhausting it is a named refusal, never a silent reuse.
 *
 * When native code calls a slot, the dispatcher reads that signature against the
 * native caller's x0..x7, v0..v7 and stacked arguments, packed the way Apple's
 * arm64 packs them, places the values where the System V ABI puts them for the
 * guest, and runs the guest function on the calling thread below its own stack
 * pointer, past the red zone.  The guest runs with its own rounding mode, not
 * the default the enclosing native call was given.  While it runs, the thread
 * is executing guest code again, so it must not count as inside a bridged call:
 * a guest fault there is an ordinary guest fault with an ordinary recovery, and
 * reporting it as a native-code fault would kill a process that was fine.  The
 * result is converted back and returned in x0 or v0.
 *
 * A callback can also arrive on a thread with no guest personality at all, one
 * a native framework created for itself, such as a libdispatch worker.  The
 * dispatcher gives that thread one through ocerz_thread_attach on its first
 * callback and reuses it on every one after, and refuses by name only when
 * there is no guest process to attach it to.
 */
#ifndef OCERZ_ABI_H
#define OCERZ_ABI_H

#include "ocerz/types.h"
#include "ocerz/cpu.h"

#define OCERZ_ABI_MAX_ARGS 16
#define OCERZ_ABI_MAX_STACK 16
#define OCERZ_ABI_CB_MAX 24
#define OCERZ_ABI_CALLBACK_SLOTS 4096
#define OCERZ_ABI_CALLBACK_STRIDE 8

typedef struct OcerzAbiSig {
    char ret;
    char arg[OCERZ_ABI_MAX_ARGS];
    char cb[OCERZ_ABI_MAX_ARGS][OCERZ_ABI_CB_MAX];
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

void *ocerz_abi_callback_intern(uint64_t guest_fn, const char *notation);

void ocerz_abi_callback_dispatch(unsigned slot, const uint64_t *x, const uint64_t *v,
                                 const uint8_t *stack, uint64_t *out_x0,
                                 uint64_t *out_v0);

extern const char ocerz_abi_callback_bank[];
extern const char ocerz_abi_callback_bank_end[];

#endif

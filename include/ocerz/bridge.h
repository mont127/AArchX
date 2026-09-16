/*
 * Calling the host's own arm64 functions on the guest's behalf.
 *
 * A virtual system library's exports are stubs that trap (see vdylib.h).  This
 * is what happens after the trap: the export is looked up, its arguments are
 * read out of the guest's x86 register state, converted where a conversion is
 * needed, handed to the real arm64 function linked into ocerz itself, and the
 * result is put back where x86 code expects to find it.
 *
 * Floating-point arguments and results used to be excluded here too, because a
 * three-class shape could not say which register a double belongs in.  The ABI
 * engine in abi.h computes that from a signature instead, so this file now names
 * each function's real signature and hands the crossing to it.  That is also
 * what makes the integer widths honest: a 32-bit argument has to be extended by
 * the caller on arm64, and a 32-bit result comes back with the upper half of the
 * register dirty, neither of which the old shape could express.
 *
 * Guest pointers need translating in principle and not at all in practice, at
 * least in the map native mode runs in: ocerz_g2h is identity there, so a guest
 * pointer already is a host pointer and a buffer the guest allocated can be
 * handed straight to a native function.  The conversion is written anyway, both
 * because the map is not native mode's to assume and because a null pointer must
 * stay null rather than become the base of the arena.
 *
 * What this layer deliberately cannot do yet, so that what it does do is
 * trustworthy:
 *
 * - Variadic functions.  Apple's arm64 ABI passes variadic arguments on the
 *   stack while x86-64 passes them in registers, so calling one through a
 *   fixed-arity prototype puts every argument in the wrong place.  printf, open,
 *   fcntl and ioctl are therefore not bridged here; they need per-function
 *   veneers that know where the fixed arguments stop.
 * - Anything taking a callback, which needs a trampoline back into guest code
 *   that does not exist yet.  qsort and bsearch are absent for that reason.
 * - Structures passed or returned by value, which both ABIs split into pieces
 *   and classify differently; abi.h rejects a signature naming one.
 *
 * An export with no descriptor here is not an error: it falls back to naming
 * itself and stopping, which is what every export did before this layer existed.
 *
 * A crossing is also the only moment at which a thread the guest is driving is
 * running native code, so a fault taken during one is not the guest's fault the
 * way every fault before this layer was.  ocerz_bridge_in_flight is how a crash
 * report asks what the thread was in the middle of: it answers NULL off the
 * crossing path, and otherwise describes the innermost crossing, counted by a
 * depth because a bridged function may in principle re-enter.  The frame holds
 * pointers rather than copies because it is read from inside a signal handler,
 * which may neither allocate nor take a lock; everything it points at is a
 * string literal or a field of a descriptor, all of static lifetime, so reading
 * it costs a load and nothing else.
 *
 * There is one exit a crossing cannot bracket itself: a fault handled by jumping
 * out of the faulting call instead of returning through it, which skips the
 * lower and strands the frame.  Nothing does that today, because a fault inside
 * a crossing stops the process rather than jumping past it.  It becomes real the
 * moment native code can call back into guest code, since a guest fault inside a
 * callback recovers by jumping, and a stranded frame would invert this whole
 * mechanism: every later fault on that thread would be blamed on a bridge that
 * had already returned.
 */
#ifndef OCERZ_BRIDGE_H
#define OCERZ_BRIDGE_H

#include "ocerz/types.h"
#include "ocerz/cpu.h"

struct OcerzVM;
struct OcerzBridgeFn;

struct OcerzBridgeFrame {
    const char *lib;
    const char *sym;
    const char *sig;
    const void *host_fn;
    int depth;
};

const struct OcerzBridgeFn *ocerz_bridge_lookup(const char *lib, const char *sym);
int ocerz_bridge_invoke(struct OcerzVM *vm, OcerzCPU *cpu,
                        const struct OcerzBridgeFn *fn);
void ocerz_bridge_report(void);

const struct OcerzBridgeFrame *ocerz_bridge_in_flight(void);

#endif

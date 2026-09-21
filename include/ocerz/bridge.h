/*
 * Calling the host's own arm64 functions on the guest's behalf.
 *
 * A virtual system library's exports are stubs that trap (see vdylib.h).  This
 * is what happens after the trap: the export is looked up, its arguments are
 * read out of the guest's x86 register state, converted where a conversion is
 * needed, handed to the real arm64 function in the host library, and the result
 * is put back where x86 code expects to find it.
 *
 * What an export crosses to, and how, is the export's record in the API
 * database (apidb.h): a fn record names the host symbol and the function's real
 * signature, which the ABI engine in abi.h turns into register and stack
 * placement, including which register a double belongs in and how a 32-bit
 * argument or result is extended; a special record names a handler ocerz
 * answers the call with itself; struct and shape records say which pointer
 * arguments carry structures of function pointers and what each word of them
 * is.  ocerz_bridge_lookup makes a descriptor from those records the first
 * time an export is asked for and returns the same one every time after, from
 * any thread.
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
 * - Variadic functions in general.  Apple's arm64 ABI passes variadic arguments
 *   on the stack while x86-64 passes them in registers, so calling one through
 *   a fixed-arity prototype puts every argument in the wrong place.  The ones
 *   whose format string says what follows - the printf family, NSLog and
 *   CoreFoundation's format functions - are special records answered by
 *   veneers in objcbridge.h.  Those whose ellipsis stands for one fixed
 *   argument, open, fcntl and ioctl among them, are special records answered
 *   in sysbridge.h, and the rest are stub records.
 * - A callback whose own signature takes a callback; abi.h describes what a
 *   callback argument can be and which threads it may run on.
 *
 * An export with no descriptor is not an error: it falls back to naming itself
 * and stopping with OCERZ_BRIDGE_UNIMPL_EXIT.
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
 * ocerz_bridge_raise and ocerz_bridge_lower are the same bracket for a
 * crossing that is not a descriptor's, such as a message send or a formatted
 * print (objcbridge.h), whose names and signature are known only once the
 * call has arrived.  The strings handed to raise must outlive the crossing,
 * which a selector name, a notation interned for the process, or a buffer on
 * the stack of the function that lowers the frame all do.
 *
 * A native function may call back into guest code, as qsort calls its
 * comparator, and for as long as that guest code runs the thread is not inside
 * native code at all.  ocerz_bridge_guest_enter saves the thread's frame and
 * clears it, so in_flight answers NULL and a guest fault there is handled as the
 * ordinary guest fault it is; ocerz_bridge_guest_leave puts the frame back when
 * the guest code returns.  A bridged call made from inside that guest code raises
 * its own frame and lowers it again in the usual way.  The recovery point a guest
 * fault jumps to is installed by the guest call itself, so a recovered fault
 * lands inside the callback rather than past it, and the saved frame is still
 * there to restore.
 *
 * The frame guest_enter clears is not left empty: it names the saved one as the
 * crossing it is around, and carries a level, a number no other stretch of guest
 * code in the process is given, so guest code can ask which stretch it is
 * running in.  ocerz_bridge_level answers that for the calling thread,
 * numbering a thread's outermost stretch the first time it is asked, and
 * ocerz_bridge_callback_frame answers the frame of the crossing whose callback
 * is running, or NULL at the outermost stretch; following around from there
 * walks outward through every crossing still open on the thread.  A setjmp
 * records the level, and a longjmp that finds a different one would leave the
 * stretch it was made in, which is how it knows it would skip native frames.
 *
 * ocerz_bridge_return and ocerz_bridge_settle are the two halves of how every
 * export ocerz answers itself ends: the first consumes the return address as a
 * ret would and puts the result in rax, the second delivers any guest signal
 * that became pending and unmasked, on top of that finished state.
 * ocerz_bridge_postfork_child puts this file's locks back to their initial
 * state in a fork child, where a thread that held one no longer exists.
 *
 * A virtual library stands for a native one, and the native one is found by the
 * install name they share, and the bridge stands in for exactly the install
 * names the API database has a file for.  ocerz_bridge_host_library opens the
 * native library once and hands back the handle, the process's default search
 * scope for libSystem, and NULL for an install name with no database file or a
 * library that will not open; ocerz_bridge_host_symbol looks a host symbol up
 * inside it.  Both are safe to call from any thread and cheap after the first
 * call for a given library.  The virtual image builder uses the second for data
 * records, whose export must be the native variable itself rather than a copy
 * of its value, and the bridge uses it for every fn record it makes a descriptor
 * from, so a function is always taken from the library the guest named and
 * never from whichever image happens to export the name first.
 *
 * The host process is ocerz, and native frameworks ask the process who it is.
 * ocerz_bridge_set_process_args points the host's own argc, argv and program
 * name, the variables _NSGetArgc, _NSGetArgv and _NSGetProgname answer, at the
 * guest's; the loader calls it once with the guest's arguments before any
 * framework opens and again with the guest's own stack vectors once they are
 * built.  The first framework opened opens CoreFoundation first, with
 * OCERZ_BRIDGE_PROCESS_PATH_VAR naming the guest executable for exactly the
 * length of CoreFoundation's initializer, which is when CoreFoundation fixes the
 * main bundle, the process name and the arguments NSProcessInfo reports.
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
    uint64_t level;
    const struct OcerzBridgeFrame *around;
};

const struct OcerzBridgeFn *ocerz_bridge_lookup(const char *lib, const char *sym);
int ocerz_bridge_invoke(struct OcerzVM *vm, OcerzCPU *cpu,
                        const struct OcerzBridgeFn *fn);
void ocerz_bridge_report(void);

const struct OcerzBridgeFrame *ocerz_bridge_in_flight(void);
void ocerz_bridge_guest_enter(struct OcerzBridgeFrame *saved);
void ocerz_bridge_guest_leave(const struct OcerzBridgeFrame *saved);
void ocerz_bridge_raise(struct OcerzBridgeFrame *outer, const char *lib, const char *sym,
                        const char *sig, const void *host_fn);
void ocerz_bridge_lower(const struct OcerzBridgeFrame *outer);
const int *ocerz_bridge_depth_ptr(void);
uint64_t ocerz_bridge_level(void);
const struct OcerzBridgeFrame *ocerz_bridge_callback_frame(void);
void ocerz_bridge_return(OcerzCPU *cpu, uint64_t rax);
int ocerz_bridge_settle(struct OcerzVM *vm, OcerzCPU *cpu);
void ocerz_bridge_postfork_child(void);

#define OCERZ_BRIDGE_LIBSYSTEM "/usr/lib/libSystem.B.dylib"
#define OCERZ_BRIDGE_COREFOUNDATION \
    "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation"

#define OCERZ_BRIDGE_PROCESS_PATH_VAR "CFProcessPath"

void *ocerz_bridge_host_library(const char *install_name);
void *ocerz_bridge_host_symbol(const char *install_name, const char *host_sym);
uint64_t ocerz_bridge_native_thunk(const void *fn, const char *name, const char *notation);
int ocerz_bridge_thunk_trap(struct OcerzVM *vm, OcerzCPU *cpu);
void ocerz_bridge_set_process_args(int argc, char **argv);

#endif

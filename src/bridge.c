/*
 * The bridges themselves: what a virtual library's export actually does.
 *
 * A descriptor here is a name, a host symbol and a signature; src/abi.c owns
 * everything the signature then implies.  The guest arrives in the middle of a
 * System V call with the return address on the stack, the engine reads the
 * arguments that signature describes out of wherever x86-64 left them, calls
 * the real arm64 function in the host library, and puts the result back where
 * x86 code looks for it.  Which export is which host function under which
 * signature is not written here at all: it is the fn record the API database
 * (ocerz/apidb.h) carries for the export.  What is left here is turning a
 * record into a descriptor, the exports that are not a call at all, and the
 * count of crossings.  The counting happens before that split, so _exit and
 * its kind appear in the report like anything else.
 *
 * ---- a descriptor is made once, when it is first wanted ----
 * ocerz_bridge_lookup finds the export's record through ocerz_apidb_find and
 * returns the descriptor made from it, making it on the first lookup: the host
 * symbol resolved in the library the record belongs to, the signature parsed,
 * and each struct record bound to the integer register its argument arrives in
 * and to every shape record of the name it gives.  A special record becomes a
 * descriptor holding the handler this file knows by the record's handler name,
 * from a table of name and function.  A stub, data or var record, or no record,
 * has no descriptor.  The answer, including an answer of none, is stored in an
 * atomic word per entry of a per-library table, and a descriptor is only ever
 * made under one lock with the word checked again inside it, so two threads
 * that race to look an export up receive the same pointer and nothing is made
 * twice.  Descriptors are never freed, and every string a descriptor holds
 * points into the parsed database, which is never freed either.  Making one
 * also records whether its signature fits the argument registers of both ABIs,
 * scalars only and no more of them than System V has registers for, because
 * such a crossing goes through ocerz_abi_perform_registers and builds no
 * stack block or structure buffer at all.
 *
 * ---- the signature is the declared one, not the convenient one ----
 * A signature is read off the function's declaration in the SDK, because the
 * engine acts on the distinctions that declaration makes: a 32-bit argument is
 * re-extended on the way across and a 32-bit result on the way back, so
 * writing L where the header says int is not a harmless rounding of the truth.
 * A three-class shape - void, integer, pointer - could not tell an int from a
 * long and would pass every integer on as the full 64 bits it found, survivable
 * only for as long as the guest happened to leave the upper half clean.
 *
 * ---- what is deliberately absent ----
 * Variadic functions as fn records.  Apple's arm64 ABI passes variadic
 * arguments on the stack while x86-64 passes them in registers, and a signature
 * has nowhere to say where a function's fixed arguments stop, so a bridged open,
 * fcntl or ioctl would be quietly wrong rather than refused.  The database
 * carries them as stub records, and they fall back to naming themselves.  The
 * variadic functions a format string describes are special records instead,
 * and so are the Objective-C message sends, whose signature is the method's:
 * the handler table names the functions src/objcbridge.c answers them with.
 *
 * ---- functions that call back ----
 * qsort and bsearch take a comparator, which the guest supplies as x86 code.
 * Their signatures name that argument with class c and the comparator's own
 * signature in braces, and the engine interns the guest function to a native
 * trampoline before the call, so a record needs nothing beyond the notation.
 * While the comparator runs the thread is executing guest code again, not native
 * code, so ocerz_bridge_guest_enter clears the frame for that stretch and
 * ocerz_bridge_guest_leave restores it: a fault inside the comparator is the
 * guest's, handled the ordinary way, and a strcmp the comparator makes raises
 * and lowers a frame of its own inside it.
 *
 * ---- callbacks on threads the guest never created ----
 * libdispatch's function-pointer entry points are bridged: dispatch_async_f,
 * dispatch_sync_f and dispatch_apply_f, with the global queues, the semaphores
 * and dispatch_release that a plain C program needs around them.  They call
 * guest code on threads the guest never created.  qsort runs its comparator on
 * the thread that called qsort, a thread that already has a guest cpu to run it
 * on.  dispatch_async_f returns at once, and its work function runs later on one
 * of libdispatch's own workers, a thread with no x86 register state, no guest
 * stack and no bridge frame, after the crossing that handed the function over
 * has ended.  dispatch_apply_f spreads its iterations over those workers and the
 * calling thread together.  dispatch_sync_f normally runs its work on the
 * calling thread, an optimization the header describes rather than promises,
 * and runs it on the main thread instead when the queue is the main queue or
 * targets it.  So these exports are the proof that a native framework's own
 * threads can run guest callbacks, and their records differ from qsort's in
 * nothing but the notation: giving such a thread a guest cpu is the engine's
 * business, not this file's.  A work function held past the call that received
 * it is exactly the case for which an interned slot is never given back, and
 * because one function under one notation interns to one slot, a program that
 * submits the same work a million times uses one slot, not a million.
 * dispatch_apply_f's DISPATCH_APPLY_AUTO is a null queue, which the pointer
 * conversion passes through as null.
 *
 * ---- guest threads ----
 * pthread_create's start routine is a callback held past the call and run on a
 * thread the guest never created, which is exactly what the two sections above
 * already handle, so a guest thread needs no mechanism of its own: the native
 * pthread_create starts a host thread, that thread calls the trampoline, is given
 * a guest personality on the way in, and runs the start routine, whose result
 * comes back through pthread_join.  The mutex and condition functions are
 * forwarded as they are, which is sound only because pthread_mutex_t,
 * pthread_cond_t and the rest have the same size and alignment on x86_64 and
 * arm64 and the same initializer signatures, so a mutex a guest initialized
 * statically is a valid native mutex; that was checked with sizeof and the
 * initializer values compiled for both architectures, not assumed.
 *
 * ---- fortified string functions ----
 * An optimizing build of ordinary C calls __memcpy_chk, __strcpy_chk and the
 * rest instead of the plain functions whenever the compiler knows the size of
 * the destination, so a program that only ever wrote strcpy imports
 * ___strcpy_chk.  They are bridged exactly like their plain counterparts, with
 * the destination size as one more integer argument; the native versions do the
 * bounds check and abort on overflow.  The variadic ones, __sprintf_chk and
 * __snprintf_chk, are special records answered the way printf is.
 *
 * ---- why null has to survive the conversion ----
 * ocerz_g2h is affine: it adds a base.  Applied to a null guest pointer it
 * produces the base of the arena, which is a plausible-looking address that is
 * not null, and free(NULL), time(NULL), a getenv that misses, a strstr that
 * does not match and a memchr that runs off the end all turn on the difference.
 * The engine's conversions therefore pass zero through untouched in both
 * directions.  In native mode the base is zero and every one of those cases
 * works whether the check is there or not, which is exactly why it is written
 * down: the mode that hides the bug is the mode this file was written for.
 *
 * ---- a descriptor that does not hold up is no descriptor ----
 * A stub record, a fn whose host symbol dlsym cannot find, a special naming a
 * handler this file does not have, and a struct record whose argument the
 * signature does not place in an integer register are the same thing to the
 * caller: no descriptor, and the behaviour of naming the export and stopping.
 * A refusal is a bug report, while a crossing made through something nobody
 * could resolve would be a wrong answer, so the failures are not allowed to
 * differ.  Resolution and parsing both happen in the lookup, which runs once
 * per export, so nothing on the crossing path touches dlsym or a string.
 *
 * ---- a crossing says, for as long as it lasts, that it is happening ----
 * Every fault a guest thread took before this layer existed was the guest's,
 * because guest code was the only code such a thread ran.  A crossing ends
 * that: for the length of one call the thread is running Apple's own arm64
 * code, so a guest that hands strcpy a pointer it had no business handing it
 * faults inside libSystem, at an instruction pointer belonging to nothing the
 * translator emitted.  Nobody downstream can tell that from a translator bug
 * unless this file writes down what the thread is in the middle of, so it does:
 * a thread-local frame naming the library, the export, the signature and the
 * host address, raised immediately before the call and lowered immediately
 * after, with a depth counting nesting because a bridged function may in
 * principle re-enter and it is the innermost crossing that describes the fault.
 * The raise, the call and the lower are one small function with no other way
 * out, which is what keeps the pair honest rather than anyone remembering to
 * write the second half.  It looks the thread's frame up once and raises and
 * lowers through that address, since every thread-local lookup is a call of
 * its own on this platform.  The special exports raise nothing, _exit above all: a
 * frame raised around a function that never returns would stay raised for the
 * rest of the process.  A fault recovered by jumping out of a crossing instead
 * of returning through it is the one exit the pair cannot see, and nothing takes
 * it.  The crash handler stops the process on a fault in native code rather than
 * jumping out, and a guest fault inside a callback does not jump past the
 * crossing either: the guest call installs its own recovery point, so the
 * recovery lands inside the callback, below the saved frame, and the frame is
 * put back when the callback returns.
 *
 * The frame is read from inside a signal handler, which may not allocate and
 * may not take a lock, so it copies nothing: every string in it belongs to the
 * parsed database and the address is the descriptor's own, all of them never
 * freed and all printable from a handler exactly as they are found.
 * OCERZ_BRIDGELOG prints those same three names once per crossing, from a
 * variable read once into a static so the hot path pays a predictable branch
 * and never a getenv.  It prints no argument values: the signature already says
 * what shape they were, and most of them are pointers into guest memory that a
 * log line has no business dereferencing.
 *
 * ---- counting ----
 * Each descriptor carries its own crossing count, and every descriptor made is
 * linked into one list, so OCERZ_BRIDGESTAT's report at exit walks that list:
 * the crossings, how many descriptors were crossed at least once, and how many
 * fn and special records the libraries the bridge was asked about declare, then
 * the crossed exports busiest first.
 *
 * ---- leaving the process ----
 * C's exit runs the atexit handlers, flushes stdio and ends the process; _exit
 * and _Exit only end it.  In native mode a guest's atexit is the host's, so its
 * handlers are callbacks on the host's list, and the host's stdio is the guest's
 * stdio.  The exit handler therefore calls the host's exit while the guest is
 * still running: each handler runs as the callback it is, stdio is flushed by the
 * library that buffered it, and the process ends there.  Letting the VM wind down
 * first would not do, because a callback into a VM that has exited is refused,
 * and the handlers would silently never run.  exit_now is _exit's handler and
 * calls the host's _exit, running nothing and flushing nothing: stdio a guest
 * buffered and never flushed is lost, as it is on a real system, which it would
 * not be if the VM wound down through ocerz's own exit, since that ends in the
 * host's exit.
 *
 * ---- the stack probe ----
 * clang calls ___chkstk_darwin from any x86_64 function whose frame is larger
 * than a page, with the frame size in RAX and every register expected back
 * unchanged, so that each page of the new frame is touched in order and a
 * guard page is hit before anything below it.  A guest's stacks are mapped
 * whole when ocerz creates them, so there is nothing to probe: the chkstk
 * handler only returns.  Its stub, like __tlv_bootstrap's, pushes r11 before it
 * spends that register on the export id, so the handler takes r11 back off the
 * stack first and every register the guest had is the one it gets.
 *
 * ---- thread-local variables ----
 * __tlv_bootstrap is not a function any host library exports; it is the thunk
 * a guest's thread-local variable descriptors are bound to, and it has to return
 * the variable's address while preserving every register but RAX.  A trap into
 * ocerz preserves them all by construction, so it is a special export whose
 * handler, tlv_bootstrap, asks ocerz_tlv_address for this thread's copy and
 * returns that in RAX.  Its stub, unlike every other, saved the guest's r11 on
 * the stack before trapping, since clang keeps live values in r11 across a
 * thread-local access, so the handler restores r11 from there before returning.
 * It raises no bridge frame, because nothing it runs is native framework code.
 *
 * ---- signals ----
 * A guest's signal handlers are x86 code, and a native sigaction cannot be
 * handed one: the host kernel would jump into guest bytes.  So sigaction,
 * signal, sigprocmask, pthread_sigmask, sigaltstack, raise, kill and
 * pthread_kill are special exports whose handlers go to the same guest signal
 * table, masks and alternate stacks the syscall path keeps, through the entry
 * points src/syscall.c exports for exactly this, with a trampoline in guest
 * memory standing in for the _sigtramp an x86 libc would have supplied.  Those
 * entry points answer 0 or an errno, and the handlers here turn that into the
 * shape the guest's declaration promises: -1 with errno set for the POSIX calls,
 * the errno itself for pthread_sigmask and pthread_kill, SIG_ERR for signal.
 * errno is the host's own, since ___error is bridged to the host's __error.
 * kill aimed at this process is raise, so that it runs its handler on the
 * calling thread before returning; aimed anywhere else it is the host's kill.
 * The sigset helpers are plain crossings, since a sigset_t is the same 32 bits on
 * both sides.
 *
 * A handler runs on the state after the call that raised it, so no special
 * builds a signal frame itself.  Each finishes its call first - result in RAX,
 * return address popped - and only then asks whether anything is pending and
 * unmasked, and delivers it on top of that finished state.  Every ordinary
 * crossing asks the same question on its way out, and that is the delivery
 * policy for native mode: a signal that arrives while a thread is inside native
 * code, or one sent from another thread, reaches its handler when that thread's
 * crossing returns, the way a cache-mode thread takes it at its next syscall.
 * The question is a thread-local load and two loads and a branch, which a
 * crossing does not notice.
 *
 * ---- the host libraries ----
 * A virtual library stands for the host library of the same install name, and
 * the bridge stands in for exactly the install names the database has a file
 * for.  libSystem is the process's default search scope, since every image in
 * ocerz already links it.  Any other library is opened the first time anything
 * asks for one of its symbols, which is while the guest is still being loaded,
 * and that open is when its initializers run: on a current macOS CoreFoundation
 * is already mapped into every process, but __CFInitialize has not run until
 * something opens it.  Initializers have side effects on the host process, so
 * the host's signal dispositions are compared across that open and any it
 * changed is named, and the guest's environment is a copy main.c took before
 * loading began, because __CFInitialize calls setenv.
 *
 * ---- the guest's identity ----
 * The process native frameworks run in is ocerz's, and they ask the process
 * who it is.  CoreFoundation takes the main bundle, the process name NSProcessInfo
 * and NSLog report and the arguments NSProcessInfo hands out from the process
 * path and the crt_externs variables, and it takes them once, in __CFInitialize.
 * Left alone those are ocerz's path and ocerz's argv, so an application's
 * NSBundle answers came from the directory ocerz sits in, its Info.plist and
 * resources were never found, and NSApplicationMain had nothing to launch.  Two
 * things change that.  ocerz_bridge_set_process_args points the host's NXArgc,
 * NXArgv and __progname, the variables _NSGetArgc, _NSGetArgv and _NSGetProgname
 * hand out, at the guest's argument count, vector and program name: dyld.c calls
 * it once with the guest's arguments before any library is loaded, and again
 * with the vectors on the guest's own stack once they are built, so that
 * *_NSGetArgv() is the very argv main receives, as it is on a real system.  And
 * the first framework opened opens CoreFoundation first, with CFProcessPath
 * naming the guest's executable, which CoreFoundation honours in a process that
 * is not restricted; the variable is taken out again, or put back as it was, the
 * moment that open returns.  CoreFoundation keeps what it read, so the bundle,
 * the name and the arguments stay the guest's, while neither the guest's
 * environment nor a child it spawns ever sees the variable: a native child that
 * inherited it would take its parent's bundle for its own.  A framework's
 * initializer can reach CoreFoundation's, so none may run first; libSystem and
 * libobjc cannot, and opening them initializes nothing of CoreFoundation's.  The
 * environment copy main.c took is older than all of this and never holds the
 * variable either.
 *
 * _NSGetArgc, _NSGetArgv, _NSGetEnviron, _NSGetProgname, getprogname and
 * setprogname then cross like any other function.  They answer the host's
 * variables, which now hold the guest's values, and those are the very
 * variables the _NXArgc, _NXArgv, _environ and ___progname data exports name,
 * so what a guest writes through one it reads back through the other; a special
 * answering from slots of its own would be a second copy that goes stale at the
 * first setenv.  Two answers cannot come from the host, because host dyld's
 * _NSGetExecutablePath names ocerz and _NSGetMachExecuteHeader ocerz's own
 * header.  Those are specials: the first copies the guest executable's real path
 * and leaves the size alone when it fits, or returns -1 with the size the path
 * needs, as dyld does, and the second returns the guest main image's header.
 *
 * With the identity in place NSApplicationMain and CFBundleGetMainBundle cross
 * as ordinary functions.  NSApplicationMain reads the guest's Info.plist, looks
 * the principal class up by name, which a guest class registered with the native
 * runtime satisfies, loads the main nib out of the guest's bundle, whose objects
 * name guest classes and reach guest outlets and actions through the native
 * runtime, and runs the application on the thread that called it, the main
 * thread, with every delegate method and action a callback into guest code.  It
 * never returns: -terminate: ends the process through the host's exit, which runs
 * the guest's atexit handlers, since they are on the host's list, and flushes the
 * host's stdio, which is the guest's.
 *
 * ---- CoreFoundation ----
 * CoreFoundation's functions are ordinary crossings resolved inside the native
 * framework through ocerz_bridge_host_symbol, never through the process-wide
 * search order.  A CFTypeRef needs no translation on the way across in either
 * direction, and that is a consequence of the address map rather than a
 * shortcut: native mode runs a position-independent guest in the identity map,
 * so the native object a CFStringCreateWithCString returns is an address the
 * guest can hold, compare and hand back, and the same object retrieved twice is
 * the same pointer.  No handle table stands between the two, because one would
 * only have to reproduce that identity at the price of a lookup on every call.
 * Boolean and UniChar results use the engine's narrow classes, since arm64
 * extends them to 32 bits where x86 leaves the upper bits of a narrow register
 * undefined.
 *
 * What does need converting is a structure of function pointers passed by
 * address: an array's or dictionary's callbacks, a timer's, observer's or
 * source's context.  CoreFoundation copies each of them when it creates the
 * object, so the bridge copies the guest's structure first, converts every
 * function-pointer word through ocerz_abi_callback_convert, and passes the
 * address of that copy in its place for the length of the call.  A word that
 * already points at native code, as every word of a copy of
 * kCFTypeArrayCallBacks does, passes through unchanged, so CoreFoundation still
 * recognises the standard callbacks by content.  A structure that is itself
 * native, &kCFTypeArrayCallBacks as the guest was bound to it, is passed as it
 * is.  Which argument holds such a structure is a struct record, and what each
 * word of each version of it is, a shape record; the version is read out of the
 * structure's first word at every crossing, and a version no shape record
 * describes names itself and stops, because guessing at the layout of a context
 * would put a guest pointer where native code will call it.  CoreFoundation's
 * structures carry a CFIndex version, but CoreGraphics' start with a 32-bit
 * unsigned int and four bytes of padding, which a structure filled field by field
 * leaves as whatever was on the stack, so a word that matches no version whole is
 * matched again on its low half.
 */
#include "ocerz/bridge.h"
#include "ocerz/apidb.h"
#include "ocerz/abi.h"
#include "ocerz/vm.h"
#include "ocerz/dyld.h"
#include "ocerz/mem.h"
#include "ocerz/interp.h"
#include "ocerz/syscall.h"
#include "ocerz/vdylib.h"
#include "ocerz/objcbridge.h"

#include <crt_externs.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#define BR_NONE ((struct OcerzBridgeFn *)(uintptr_t)1)

typedef struct BrStructBinding {
    int argpos;
    int reg;
    const char *shape;
    const OcerzApiShape **versions;
    int nversions;
} BrStructBinding;

struct OcerzBridgeFn {
    const char *lib;
    const char *sym;
    const char *host;
    const char *sig;
    int (*special)(struct OcerzVM *vm, OcerzCPU *cpu);
    void *addr;
    OcerzAbiSig parsed;
    int register_only;
    int nstructs;
    BrStructBinding structs[OCERZ_APIDB_STRUCT_ARGS];
    uint64_t calls;
    struct OcerzBridgeFn *next;
};

typedef struct BrLib {
    const OcerzApiLibrary *api;
    void *_Atomic handle;
    struct OcerzBridgeFn *_Atomic *fns;
    struct BrLib *next;
} BrLib;

static BrLib *_Atomic g_br_libs;
static struct OcerzBridgeFn *_Atomic g_br_made;
static pthread_mutex_t g_br_libs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_br_fn_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_br_host_lock = PTHREAD_MUTEX_INITIALIZER;

static int br_exit(struct OcerzVM *vm, OcerzCPU *cpu)
{
    exit((int)(cpu->gpr[OCERZ_RDI] & 0xff));
}

static int br_exit_now(struct OcerzVM *vm, OcerzCPU *cpu)
{
    _exit((int)(cpu->gpr[OCERZ_RDI] & 0xff));
}

static int br_abort(struct OcerzVM *vm, OcerzCPU *cpu)
{
    fprintf(stderr, "ocerz: bridge: the guest called abort\n");
    ocerz_vm_request_exit(vm, 134);
    return OCERZ_STEP_EXIT;
}

static int br_stack_chk_fail(struct OcerzVM *vm, OcerzCPU *cpu)
{
    fprintf(stderr, "ocerz: bridge: the guest overran a stack guard (__stack_chk_fail)\n");
    ocerz_vm_request_exit(vm, 134);
    return OCERZ_STEP_EXIT;
}

static int br_tlv_bootstrap(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t desc = cpu->gpr[OCERZ_RDI];
    uint64_t addr = ocerz_tlv_address(cpu, desc);
    if (!addr) {
        fprintf(stderr, "ocerz: bridge: thread-local variable descriptor %#llx could not be resolved\n",
                (unsigned long long)desc);
        ocerz_vm_request_exit(vm, 134);
        return OCERZ_STEP_EXIT;
    }
    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    cpu->gpr[OCERZ_R11] = ocerz_ld(rsp, 8);
    cpu->rip = ocerz_ld(rsp + 8, 8);
    cpu->gpr[OCERZ_RSP] = rsp + 16;
    cpu->gpr[OCERZ_RAX] = addr;
    return OCERZ_STEP_OK;
}

static int br_chkstk(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    cpu->gpr[OCERZ_R11] = ocerz_ld(rsp, 8);
    cpu->rip = ocerz_ld(rsp + 8, 8);
    cpu->gpr[OCERZ_RSP] = rsp + 16;
    return OCERZ_STEP_OK;
}

static void br_return(OcerzCPU *cpu, uint64_t rax)
{
    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    cpu->rip = ocerz_ld(rsp, 8);
    cpu->gpr[OCERZ_RSP] = rsp + 8;
    cpu->gpr[OCERZ_RAX] = rax;
}

static int br_settle(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (ocerz_peek_pending_async_sig() || (cpu->sig_pending & ~cpu->sig_mask))
        ocerz_guest_deliver_pending(vm, cpu);
    return OCERZ_STEP_OK;
}

static int br_posix(struct OcerzVM *vm, OcerzCPU *cpu, int err)
{
    if (err) {
        errno = err;
        br_return(cpu, (uint64_t)-1);
    } else {
        br_return(cpu, 0);
    }
    return br_settle(vm, cpu);
}

static int br_sigaction(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_posix(vm, cpu, ocerz_guest_sigaction_user(vm, cpu, (int)cpu->gpr[OCERZ_RDI],
                                                        cpu->gpr[OCERZ_RSI], cpu->gpr[OCERZ_RDX]));
}

static int br_signal(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t old = 0;
    int err = ocerz_guest_signal(vm, cpu, (int)cpu->gpr[OCERZ_RDI], cpu->gpr[OCERZ_RSI], &old);
    if (err) {
        errno = err;
        old = (uint64_t)-1;
    }
    br_return(cpu, old);
    return br_settle(vm, cpu);
}

static int br_sigprocmask(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_posix(vm, cpu, ocerz_guest_sigprocmask(vm, cpu, (int)cpu->gpr[OCERZ_RDI],
                                                     cpu->gpr[OCERZ_RSI], cpu->gpr[OCERZ_RDX]));
}

static int br_pthread_sigmask(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int err = ocerz_guest_sigprocmask(vm, cpu, (int)cpu->gpr[OCERZ_RDI],
                                      cpu->gpr[OCERZ_RSI], cpu->gpr[OCERZ_RDX]);
    br_return(cpu, (uint64_t)(uint32_t)err);
    return br_settle(vm, cpu);
}

static int br_sigaltstack(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_posix(vm, cpu, ocerz_guest_sigaltstack(vm, cpu, cpu->gpr[OCERZ_RDI],
                                                     cpu->gpr[OCERZ_RSI]));
}

static int br_raise(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_posix(vm, cpu, ocerz_guest_raise(vm, cpu, (int)cpu->gpr[OCERZ_RDI]));
}

static int br_kill(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int pid = (int)cpu->gpr[OCERZ_RDI];
    int sig = (int)cpu->gpr[OCERZ_RSI];
    if (pid == getpid() && sig != 0)
        return br_posix(vm, cpu, ocerz_guest_raise(vm, cpu, sig));
    return br_posix(vm, cpu, kill(pid, sig) == 0 ? 0 : errno);
}

static int br_pthread_kill(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int err = ocerz_guest_pthread_kill(vm, cpu, cpu->gpr[OCERZ_RDI], (int)cpu->gpr[OCERZ_RSI]);
    br_return(cpu, (uint64_t)(uint32_t)err);
    return br_settle(vm, cpu);
}

static int br_nsgetexecutablepath(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *path = ocerz_dyld_main_path();
    uint64_t buf = cpu->gpr[OCERZ_RDI];
    uint64_t size_at = cpu->gpr[OCERZ_RSI];
    if (!path || !size_at) {
        br_return(cpu, (uint64_t)(uint32_t)-1);
        return br_settle(vm, cpu);
    }
    uint32_t need = (uint32_t)strlen(path) + 1;
    if ((uint32_t)ocerz_ld(size_at, 4) < need) {
        ocerz_st(size_at, 4, need);
        br_return(cpu, (uint64_t)(uint32_t)-1);
    } else {
        memcpy(ocerz_g2h(buf), path, need);
        br_return(cpu, 0);
    }
    return br_settle(vm, cpu);
}

static int br_nsgetmachexecuteheader(struct OcerzVM *vm, OcerzCPU *cpu)
{
    br_return(cpu, ocerz_main_mh);
    return br_settle(vm, cpu);
}

void ocerz_bridge_set_process_args(int argc, char **argv)
{
    *_NSGetArgc() = argc;
    *_NSGetArgv() = argv;
    if (argc > 0 && argv && argv[0])
        setprogname(argv[0]);
}

static int g_br_cf_identified;

static void br_identify_corefoundation(void)
{
    const char *path = ocerz_dyld_main_path();
    if (g_br_cf_identified || !path)
        return;
    g_br_cf_identified = 1;
    const char *prev = getenv(OCERZ_BRIDGE_PROCESS_PATH_VAR);
    char *saved = prev ? strdup(prev) : NULL;
    setenv(OCERZ_BRIDGE_PROCESS_PATH_VAR, path, 1);
    void *h = dlopen(OCERZ_BRIDGE_COREFOUNDATION, RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
    if (!h)
        h = dlopen(OCERZ_BRIDGE_COREFOUNDATION, RTLD_LAZY | RTLD_LOCAL);
    if (saved)
        setenv(OCERZ_BRIDGE_PROCESS_PATH_VAR, saved, 1);
    else
        unsetenv(OCERZ_BRIDGE_PROCESS_PATH_VAR);
    free(saved);
    if (h)
        OCERZ_LOG("bridge: CoreFoundation initialized with the process path %s\n", path);
    else
        OCERZ_LOG("bridge: CoreFoundation will not open to take the guest's identity: %s\n", dlerror());
}

typedef struct BrHandler {
    const char *name;
    int (*fn)(struct OcerzVM *vm, OcerzCPU *cpu);
} BrHandler;

static const BrHandler g_br_handlers[] = {
    { "exit",            br_exit },
    { "exit_now",        br_exit_now },
    { "abort",           br_abort },
    { "tlv_bootstrap",   br_tlv_bootstrap },
    { "stack_chk_fail",  br_stack_chk_fail },
    { "chkstk",          br_chkstk },
    { "sigaction",       br_sigaction },
    { "signal",          br_signal },
    { "sigprocmask",     br_sigprocmask },
    { "pthread_sigmask", br_pthread_sigmask },
    { "sigaltstack",     br_sigaltstack },
    { "raise",           br_raise },
    { "kill",            br_kill },
    { "pthread_kill",    br_pthread_kill },
    { "NSGetExecutablePath",    br_nsgetexecutablepath },
    { "NSGetMachExecuteHeader", br_nsgetmachexecuteheader },
    { "objc_msgSend",              ocerz_objc_msgSend },
    { "objc_msgSendSuper",         ocerz_objc_msgSendSuper },
    { "objc_msgSendSuper2",        ocerz_objc_msgSendSuper2 },
    { "objc_msgSend_stret",        ocerz_objc_msgSend_stret },
    { "objc_msgSendSuper_stret",   ocerz_objc_msgSendSuper_stret },
    { "objc_msgSendSuper2_stret",  ocerz_objc_msgSendSuper2_stret },
    { "objc_msgSend_fpret",        ocerz_objc_msgSend_fpret },
    { "objc_msgSend_fp2ret",       ocerz_objc_msgSend_fp2ret },
    { "objc_setUncaughtExceptionHandler", ocerz_objc_setUncaughtExceptionHandler },
    { "NSLog",                     ocerz_fmt_NSLog },
    { "printf",                    ocerz_fmt_printf },
    { "fprintf",                   ocerz_fmt_fprintf },
    { "sprintf",                   ocerz_fmt_sprintf },
    { "snprintf",                  ocerz_fmt_snprintf },
    { "asprintf",                  ocerz_fmt_asprintf },
    { "dprintf",                   ocerz_fmt_dprintf },
    { "sprintf_chk",               ocerz_fmt_sprintf_chk },
    { "snprintf_chk",              ocerz_fmt_snprintf_chk },
    { "CFStringCreateWithFormat",  ocerz_fmt_CFStringCreateWithFormat },
    { "CFStringAppendFormat",      ocerz_fmt_CFStringAppendFormat },
};

static int (*br_handler(const char *name))(struct OcerzVM *, OcerzCPU *)
{
    for (size_t i = 0; i < sizeof g_br_handlers / sizeof g_br_handlers[0]; i++)
        if (strcmp(g_br_handlers[i].name, name) == 0)
            return g_br_handlers[i].fn;
    return NULL;
}

static BrLib *br_lib(const OcerzApiLibrary *api)
{
    for (BrLib *l = g_br_libs; l; l = l->next)
        if (l->api == api)
            return l;
    pthread_mutex_lock(&g_br_libs_lock);
    BrLib *lib = NULL;
    for (BrLib *l = g_br_libs; l && !lib; l = l->next)
        if (l->api == api)
            lib = l;
    if (!lib) {
        size_t n = api->nentries > 0 ? (size_t)api->nentries : 1;
        BrLib *nl = calloc(1, sizeof *nl);
        struct OcerzBridgeFn *_Atomic *fns = calloc(n, sizeof *fns);
        if (nl && fns) {
            nl->api = api;
            nl->fns = fns;
            nl->next = g_br_libs;
            g_br_libs = nl;
            lib = nl;
        } else {
            free(nl);
            free((void *)fns);
        }
    }
    pthread_mutex_unlock(&g_br_libs_lock);
    return lib;
}

static void br_note_stolen_signals(const struct sigaction *before, const char *install_name)
{
    for (int sig = 1; sig < NSIG; sig++) {
        struct sigaction now;
        if (sigaction(sig, NULL, &now) != 0)
            continue;
        if (now.sa_sigaction != before[sig].sa_sigaction || now.sa_flags != before[sig].sa_flags)
            fprintf(stderr, "ocerz: bridge: opening %s changed the host disposition of signal %d\n",
                    install_name, sig);
    }
}

void *ocerz_bridge_host_library(const char *install_name)
{
    const OcerzApiLibrary *api = ocerz_apidb_library(install_name);
    if (!api)
        return NULL;
    BrLib *lib = br_lib(api);
    if (!lib)
        return NULL;
    void *h = lib->handle;
    if (h)
        return h;

    pthread_mutex_lock(&g_br_host_lock);
    h = lib->handle;
    if (!h) {
        if (strcmp(install_name, OCERZ_BRIDGE_LIBSYSTEM) == 0) {
            h = RTLD_DEFAULT;
        } else {
            static struct sigaction before[NSIG];
            for (int sig = 1; sig < NSIG; sig++)
                sigaction(sig, NULL, &before[sig]);
            if (strstr(install_name, ".framework/"))
                br_identify_corefoundation();
            h = dlopen(install_name, RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
            if (!h)
                h = dlopen(install_name, RTLD_LAZY | RTLD_LOCAL);
            if (h)
                br_note_stolen_signals(before, install_name);
            else
                OCERZ_LOG("bridge: host library %s will not open: %s\n", install_name, dlerror());
        }
        if (h)
            lib->handle = h;
    }
    pthread_mutex_unlock(&g_br_host_lock);
    return h;
}

void *ocerz_bridge_host_symbol(const char *install_name, const char *host_sym)
{
    if (!host_sym)
        return NULL;
    void *h = ocerz_bridge_host_library(install_name);
    return h ? dlsym(h, host_sym) : NULL;
}

static int br_int_register(const OcerzAbiSig *sig, int argpos)
{
    static const int regs[6] = { OCERZ_RDI, OCERZ_RSI, OCERZ_RDX, OCERZ_RCX, OCERZ_R8, OCERZ_R9 };
    if (argpos < 0 || argpos >= sig->nargs || sig->arg[argpos] != 'p' || sig->ret == '{')
        return -1;
    int n = 0;
    for (int i = 0; i < argpos; i++) {
        if (sig->arg[i] == '{')
            return -1;
        if (sig->arg[i] != 'f' && sig->arg[i] != 'd')
            n++;
    }
    return n < 6 ? regs[n] : -1;
}

static int br_bind_structs(const OcerzApiLibrary *api, const OcerzApiEntry *e,
                           struct OcerzBridgeFn *fn)
{
    for (int k = 0; k < e->nstructs && k < OCERZ_APIDB_STRUCT_ARGS; k++) {
        const OcerzApiStructArg *a = &e->structs[k];
        BrStructBinding *b = &fn->structs[k];
        b->argpos = a->argpos;
        b->shape = a->shape;
        b->reg = br_int_register(&fn->parsed, a->argpos);
        if (b->reg < 0) {
            OCERZ_LOG("bridge: %s converts a %s in argument %d, which its signature %s does not place"
                      " in a register\n", fn->sym, a->shape, a->argpos, fn->sig);
            return 0;
        }
        int n = 0;
        for (int s = 0; s < api->nshapes; s++)
            n += strcmp(api->shapes[s].name, a->shape) == 0;
        if (n == 0) {
            OCERZ_LOG("bridge: %s converts a %s, which %s describes in no version\n", fn->sym,
                      a->shape, api->path);
            return 0;
        }
        const OcerzApiShape **versions = calloc((size_t)n, sizeof *versions);
        if (!versions)
            return 0;
        n = 0;
        for (int s = 0; s < api->nshapes; s++)
            if (strcmp(api->shapes[s].name, a->shape) == 0)
                versions[n++] = &api->shapes[s];
        b->versions = versions;
        b->nversions = n;
        fn->nstructs = k + 1;
    }
    return 1;
}

static struct OcerzBridgeFn *br_make(const OcerzApiLibrary *api, const OcerzApiEntry *e)
{
    struct OcerzBridgeFn *fn = calloc(1, sizeof *fn);
    if (!fn)
        return NULL;
    fn->lib = api->install_name;
    fn->sym = e->export_name;

    if (e->kind == OCERZ_API_SPECIAL) {
        fn->special = br_handler(e->handler);
        if (!fn->special) {
            OCERZ_LOG("bridge: %s asks for the handler %s, which ocerz does not have\n", fn->sym,
                      e->handler);
            free(fn);
            return NULL;
        }
    } else {
        fn->host = e->host;
        fn->sig = e->sig;
        fn->addr = ocerz_bridge_host_symbol(api->install_name, e->host);
        if (!fn->addr) {
            OCERZ_LOG("bridge: %s wants host %s, which does not resolve\n", fn->sym, fn->host);
            free(fn);
            return NULL;
        }
        if (ocerz_abi_parse(fn->sig, &fn->parsed) != OCERZ_OK) {
            OCERZ_LOG("bridge: %s is declared %s, which the abi engine will not take\n",
                      fn->sym, fn->sig ? fn->sig : "(nothing)");
            free(fn);
            return NULL;
        }
        if (!br_bind_structs(api, e, fn)) {
            for (int k = 0; k < OCERZ_APIDB_STRUCT_ARGS; k++)
                free((void *)fn->structs[k].versions);
            free(fn);
            return NULL;
        }
        fn->register_only = ocerz_abi_register_only(&fn->parsed);
    }
    fn->next = g_br_made;
    g_br_made = fn;
    return fn;
}

const struct OcerzBridgeFn *ocerz_bridge_lookup(const char *lib, const char *sym)
{
    if (!lib || !sym)
        return NULL;
    const OcerzApiLibrary *api = ocerz_apidb_library(lib);
    const OcerzApiEntry *e = ocerz_apidb_find(api, sym);
    if (!e || (e->kind != OCERZ_API_FN && e->kind != OCERZ_API_SPECIAL))
        return NULL;
    BrLib *bl = br_lib(api);
    if (!bl)
        return NULL;

    struct OcerzBridgeFn *_Atomic *slot = &bl->fns[e - api->entries];
    struct OcerzBridgeFn *fn = *slot;
    if (!fn) {
        pthread_mutex_lock(&g_br_fn_lock);
        fn = *slot;
        if (!fn) {
            fn = br_make(api, e);
            if (!fn)
                fn = BR_NONE;
            *slot = fn;
        }
        pthread_mutex_unlock(&g_br_fn_lock);
    }
    return fn == BR_NONE ? NULL : fn;
}

static __thread struct OcerzBridgeFrame g_br_frame;

const struct OcerzBridgeFrame *ocerz_bridge_in_flight(void)
{
    return g_br_frame.depth > 0 ? &g_br_frame : NULL;
}

void ocerz_bridge_guest_enter(struct OcerzBridgeFrame *saved)
{
    *saved = g_br_frame;
    memset(&g_br_frame, 0, sizeof g_br_frame);
}

void ocerz_bridge_guest_leave(const struct OcerzBridgeFrame *saved)
{
    g_br_frame = *saved;
}

void ocerz_bridge_raise(struct OcerzBridgeFrame *outer, const char *lib, const char *sym,
                        const char *sig, const void *host_fn)
{
    *outer = g_br_frame;
    g_br_frame.lib = lib;
    g_br_frame.sym = sym;
    g_br_frame.sig = sig;
    g_br_frame.host_fn = host_fn;
    g_br_frame.depth = outer->depth + 1;
}

void ocerz_bridge_lower(const struct OcerzBridgeFrame *outer)
{
    g_br_frame = *outer;
}

static int br_logging(void)
{
    static int en = -1;
    if (en < 0) en = getenv("OCERZ_BRIDGELOG") ? 1 : 0;
    return en;
}

static void br_convert_structs(const struct OcerzBridgeFn *fn, OcerzCPU *cpu,
                               uint64_t copies[OCERZ_APIDB_STRUCT_ARGS][OCERZ_APIDB_SHAPE_WORDS])
{
    for (int k = 0; k < fn->nstructs; k++) {
        const BrStructBinding *b = &fn->structs[k];
        uint64_t gptr = cpu->gpr[b->reg];
        if (!gptr || !ocerz_abi_is_guest_code(gptr))
            continue;
        uint64_t version = ocerz_ld(gptr, 8);
        const OcerzApiShape *shape = NULL;
        for (int i = 0; i < b->nversions && !shape; i++)
            if (b->versions[i]->version == version)
                shape = b->versions[i];
        for (int i = 0; i < b->nversions && !shape; i++)
            if (b->versions[i]->version == (uint32_t)version)
                shape = b->versions[i];
        if (!shape) {
            fprintf(stderr, "ocerz: bridge: %s was handed a %s of version %llu, which ocerz cannot convert\n",
                    fn->sym, b->shape, (unsigned long long)version);
            exit(OCERZ_BRIDGE_UNIMPL_EXIT);
        }
        for (int w = 0; w < shape->words; w++) {
            uint64_t v = ocerz_ld(gptr + 8 * (uint64_t)w, 8);
            if (shape->word[w] && ocerz_abi_callback_convert(v, shape->word[w], &v) != OCERZ_OK) {
                fprintf(stderr, "ocerz: bridge: %s could not bind guest function %#llx in word %d of its %s\n",
                        fn->sym, (unsigned long long)ocerz_ld(gptr + 8 * (uint64_t)w, 8), w, shape->name);
                exit(OCERZ_BRIDGE_UNIMPL_EXIT);
            }
            copies[k][w] = v;
        }
        cpu->gpr[b->reg] = ocerz_h2g(copies[k]);
    }
}

__attribute__((noinline))
static int br_cross_structs(const struct OcerzBridgeFn *fn, OcerzCPU *cpu)
{
    struct OcerzBridgeFrame outer;
    uint64_t copies[OCERZ_APIDB_STRUCT_ARGS][OCERZ_APIDB_SHAPE_WORDS];

    ocerz_bridge_raise(&outer, fn->lib, fn->sym, fn->sig, fn->addr);
    br_convert_structs(fn, cpu, copies);
    int rc = ocerz_abi_perform(&fn->parsed, fn->addr, cpu);
    ocerz_bridge_lower(&outer);
    return rc;
}

static int br_cross(const struct OcerzBridgeFn *fn, OcerzCPU *cpu)
{
    if (fn->nstructs)
        return br_cross_structs(fn, cpu);

    struct OcerzBridgeFrame *frame = &g_br_frame;
    struct OcerzBridgeFrame outer = *frame;
    frame->lib = fn->lib;
    frame->sym = fn->sym;
    frame->sig = fn->sig;
    frame->host_fn = fn->addr;
    frame->depth = outer.depth + 1;
    int rc = fn->register_only ? ocerz_abi_perform_registers(&fn->parsed, fn->addr, cpu)
                               : ocerz_abi_perform(&fn->parsed, fn->addr, cpu);
    *frame = outer;
    return rc;
}

int ocerz_bridge_invoke(struct OcerzVM *vm, OcerzCPU *cpu, const struct OcerzBridgeFn *fn)
{
    if (!fn)
        return OCERZ_STEP_FATAL;

    ((struct OcerzBridgeFn *)fn)->calls++;

    if (br_logging())
        fprintf(stderr, "ocerz: BRIDGELOG[%d] %s %s %s\n", (int)getpid(),
                fn->lib, fn->sym, fn->sig ? fn->sig : "(nothing)");

    if (fn->special)
        return fn->special(vm, cpu);

    int rc = br_cross(fn, cpu);
    if (rc != OCERZ_STEP_OK)
        return rc;
    return br_settle(vm, cpu);
}

static int br_row_cmp(const void *a, const void *b)
{
    uint64_t x = (*(const struct OcerzBridgeFn *const *)a)->calls;
    uint64_t y = (*(const struct OcerzBridgeFn *const *)b)->calls;
    return x < y ? 1 : x > y ? -1 : 0;
}

void ocerz_bridge_report(void)
{
    int made = 0;
    for (const struct OcerzBridgeFn *f = g_br_made; f; f = f->next)
        made++;
    int declared = 0;
    for (const BrLib *l = g_br_libs; l; l = l->next)
        for (int i = 0; i < l->api->nentries; i++)
            declared += l->api->entries[i].kind == OCERZ_API_FN ||
                        l->api->entries[i].kind == OCERZ_API_SPECIAL;

    const struct OcerzBridgeFn **rows = calloc(made ? (size_t)made : 1, sizeof *rows);
    uint64_t total = 0;
    int used = 0;
    int n = 0;
    for (const struct OcerzBridgeFn *f = g_br_made; f && rows && n < made; f = f->next) {
        rows[n++] = f;
        total += f->calls;
        if (f->calls)
            used++;
    }
    if (rows && n > 1)
        qsort(rows, (size_t)n, sizeof rows[0], br_row_cmp);

    fprintf(stderr, "ocerz: BRIDGESTAT[%d] crossings=%llu over %d of %d bridged export(s)\n",
            (int)getpid(), (unsigned long long)total, used, declared);
    for (int i = 0; rows && i < used; i++)
        fprintf(stderr, "ocerz: BRIDGESTAT[%d]   #%2d %-26s %14llu  %6.2f%%\n",
                (int)getpid(), i + 1, rows[i]->sym, (unsigned long long)rows[i]->calls,
                total ? 100.0 * (double)rows[i]->calls / (double)total : 0.0);
    free(rows);
}

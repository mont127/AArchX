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
 * has nowhere to say where a function's fixed arguments stop, so a variadic
 * function bridged by its signature would be quietly wrong rather than refused.
 * The database carries them as stub records, and they fall back to naming
 * themselves.  The variadic functions a format string describes are special
 * records instead, and so are the Objective-C message sends, whose signature is
 * the method's: the handler table names the functions src/objcbridge.c answers
 * them with.  So are open, fcntl, ioctl and the other functions whose ellipsis
 * stands for one fixed argument, answered by src/sysbridge.c along with the
 * memory, non-local jump and process calls a crossing cannot make.
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
 * ---- loading code at run time ----
 * Host dyld has never heard of an image ocerz's loader mapped, and the host's
 * dlopen would load arm64 code, so neither dlopen nor any question about the
 * guest's images can cross.  dlopen and its relatives, dlsym, dladdr, dlclose,
 * dlerror and the image-list half of the dyld API are special exports whose
 * handlers read the arguments out of the registers and hand them to the native
 * loader in src/dyld.c, which owns the answers.  What only a handler knows is
 * where the call came from: the caller's return address is the word at rsp
 * when the stub traps, so it is what RTLD_NEXT, RTLD_SELF, @loader_path and
 * @rpath are resolved against, except for dlopen_from, whose caller names
 * itself in its third argument.  A dlopen runs guest code - add-image callbacks,
 * +load methods and initializers - and it runs it on the calling thread below
 * the guest's stack pointer less the red zone, the same place a callback from
 * native code lands, so a dylib is initialized on the thread that asked for it,
 * as dyld initializes it.  A path is copied out of guest memory first, so the
 * loader never reads a string the guest could change underneath it.
 *
 * The questions about the program's own build are answered from the guest main
 * image's LC_BUILD_VERSION, and the at-least forms, whose argument is a
 * dyld_build_version_t passed by value in one register on both architectures,
 * are handed with that image's header to the host's own dyld_sdk_at_least and
 * dyld_minos_at_least, which read load commands and know the yearly version
 * sets; an x86_64 header reads the same to them.  dyld_get_active_platform
 * answers the main image's platform, which for a program ocerz runs is macOS.
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
#include "ocerz/blocks.h"
#include "ocerz/vm.h"
#include "ocerz/dyld.h"
#include "ocerz/mem.h"
#include "ocerz/interp.h"
#include "ocerz/syscall.h"
#include "ocerz/vdylib.h"
#include "ocerz/objcbridge.h"
#include "ocerz/sysbridge.h"

#include <crt_externs.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <errno.h>
#include <mach-o/loader.h>
#include <mach/kern_return.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define BR_NONE ((struct OcerzBridgeFn *)(uintptr_t)1)

typedef struct BrStructBinding {
    int argpos;
    int reg;
    const char *shape;
    const OcerzApiShape **versions;
    int nversions;
} BrStructBinding;

typedef struct BrInplaceBinding {
    int reg;
    uint32_t offset;
    const char *sig;
} BrInplaceBinding;

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
    int ninplace;
    BrInplaceBinding inplace[OCERZ_APIDB_INPLACE];
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

static int br_error(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    cpu->rip = ocerz_ld(rsp, 8);
    cpu->gpr[OCERZ_RSP] = rsp + 8;
    cpu->gpr[OCERZ_RAX] = cpu->gs_base ? cpu->gs_base + OCERZ_ERRNO_SLOT
                                       : ocerz_h2g(__error());
    return OCERZ_STEP_OK;
}

static int br_answer(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t rax);

static int br_bzero(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t dst = cpu->gpr[OCERZ_RDI];
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, OCERZ_BRIDGE_LIBSYSTEM, "___bzero", "p(pL)", (const void *)bzero);
    if (dst)
        bzero(ocerz_g2h(dst), (size_t)cpu->gpr[OCERZ_RSI]);
    ocerz_bridge_lower(&outer);
    return br_answer(vm, cpu, dst);
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

static int br_answer(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t rax);

static int br_settle(struct OcerzVM *vm, OcerzCPU *cpu);

#define BR_THUNK_MAX 128u
#define BR_THUNK_STRIDE 16u
#define BR_THUNK_SLOT 0x800u

typedef struct BrThunk {
    const void *fn;
    const char *name;
    const char *notation;
    OcerzAbiSig *sig;
} BrThunk;

static BrThunk g_br_thunks[BR_THUNK_MAX];
static _Atomic unsigned g_br_thunks_n;
static _Atomic uint64_t g_br_thunk_page;
static pthread_mutex_t g_br_thunk_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t br_thunk_page_locked(void)
{
    uint64_t have = atomic_load(&g_br_thunk_page);
    if (have)
        return have;
    uint64_t tramp = ocerz_vdylib_trampoline(OCERZ_VDYLIB_TRAMP_NATIVE_FN);
    uint64_t made = tramp ? ocerz_map_anywhere(OCERZ_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE) : 0;
    if (!made)
        return 0;
    uint8_t *buf = ocerz_g2h(made);
    memset(buf, 0xcc, OCERZ_GUEST_PAGE_SIZE);
    for (unsigned k = 0; k < BR_THUNK_MAX; k++) {
        uint8_t *t = buf + (size_t)k * BR_THUNK_STRIDE;
        uint32_t number = k;
        int32_t rel = (int32_t)((int64_t)BR_THUNK_SLOT - (int64_t)(k * BR_THUNK_STRIDE + 12));
        t[0] = 0x41;
        t[1] = 0xba;
        memcpy(t + 2, &number, 4);
        t[6] = 0xff;
        t[7] = 0x25;
        memcpy(t + 8, &rel, 4);
    }
    memcpy(buf + BR_THUNK_SLOT, &tramp, 8);
    if (ocerz_protect(made, OCERZ_GUEST_PAGE_SIZE, PROT_READ | PROT_EXEC) != OCERZ_OK) {
        ocerz_unmap(made, OCERZ_GUEST_PAGE_SIZE);
        return 0;
    }
    atomic_store(&g_br_thunk_page, made);
    return made;
}

uint64_t ocerz_bridge_native_thunk(const void *fn, const char *name, const char *notation)
{
    if (!fn || !notation)
        return 0;
    uint64_t answer = 0;
    pthread_mutex_lock(&g_br_thunk_lock);
    unsigned n = atomic_load(&g_br_thunks_n), k;
    for (k = 0; k < n; k++)
        if (g_br_thunks[k].fn == fn)
            break;
    if (k == n && n < BR_THUNK_MAX) {
        OcerzAbiSig *sig = calloc(1, sizeof *sig);
        if (sig && ocerz_abi_parse(notation, sig) == OCERZ_OK) {
            g_br_thunks[n].fn = fn;
            g_br_thunks[n].name = name ? name : "(native function)";
            g_br_thunks[n].notation = notation;
            g_br_thunks[n].sig = sig;
            atomic_store(&g_br_thunks_n, n + 1);
        } else {
            free(sig);
            k = BR_THUNK_MAX;
        }
    }
    if (k < BR_THUNK_MAX) {
        uint64_t page = br_thunk_page_locked();
        if (page)
            answer = page + (uint64_t)k * BR_THUNK_STRIDE;
    }
    pthread_mutex_unlock(&g_br_thunk_lock);
    return answer;
}

int ocerz_bridge_thunk_trap(struct OcerzVM *vm, OcerzCPU *cpu)
{
    unsigned k = (unsigned)(cpu->gpr[OCERZ_R10] & 0xffffffffu);
    if (k >= atomic_load(&g_br_thunks_n)) {
        OCERZ_FATAL("bridge: a thunk numbered %u for a native function was called, and ocerz made no such thunk\n", k);
        return OCERZ_STEP_FATAL;
    }
    const BrThunk *t = &g_br_thunks[k];
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, "ocerz", t->name, t->notation, t->fn);
    int rc = ocerz_abi_perform(t->sig, t->fn, cpu);
    ocerz_bridge_lower(&outer);
    if (rc != OCERZ_STEP_OK)
        return rc;
    return br_settle(vm, cpu);
}

#define BR_ZONE_WORDS 25
#define BR_ZONE_VERSION_WORD 13
#define BR_ZONE_VERSION_CAP 13u
#define BR_ZONE_VIEWS 32u

typedef struct BrZoneView {
    uint64_t view;
    malloc_zone_t *native;
    uint64_t made[BR_ZONE_WORDS];
} BrZoneView;

static BrZoneView g_br_views[BR_ZONE_VIEWS];
static _Atomic unsigned g_br_views_n;
static pthread_mutex_t g_br_views_lock = PTHREAD_MUTEX_INITIALIZER;

static const BrZoneView *br_view_find(uint64_t view)
{
    unsigned n = atomic_load(&g_br_views_n);
    for (unsigned k = 0; view && k < n; k++)
        if (g_br_views[k].view == view)
            return &g_br_views[k];
    return NULL;
}

static malloc_zone_t *brz_native(malloc_zone_t *given)
{
    const BrZoneView *v = br_view_find(ocerz_h2g(given));
    return v ? v->native : given;
}

static size_t brz_size(malloc_zone_t *g, const void *p)
{
    malloc_zone_t *z = brz_native(g);
    return z->size ? z->size(z, p) : 0;
}

static void *brz_malloc(malloc_zone_t *g, size_t n)
{
    malloc_zone_t *z = brz_native(g);
    return z->malloc(z, n);
}

static void *brz_calloc(malloc_zone_t *g, size_t a, size_t b)
{
    malloc_zone_t *z = brz_native(g);
    return z->calloc(z, a, b);
}

static void *brz_valloc(malloc_zone_t *g, size_t n)
{
    malloc_zone_t *z = brz_native(g);
    return z->valloc(z, n);
}

static void brz_free(malloc_zone_t *g, void *p)
{
    malloc_zone_t *z = brz_native(g);
    z->free(z, p);
}

static void *brz_realloc(malloc_zone_t *g, void *p, size_t n)
{
    malloc_zone_t *z = brz_native(g);
    return z->realloc(z, p, n);
}

static void brz_destroy(malloc_zone_t *g)
{
    malloc_destroy_zone(brz_native(g));
}

static unsigned brz_batch_malloc(malloc_zone_t *g, size_t size, void **results, unsigned n)
{
    malloc_zone_t *z = brz_native(g);
    return z->batch_malloc ? z->batch_malloc(z, size, results, n) : 0;
}

static void brz_batch_free(malloc_zone_t *g, void **ptrs, unsigned n)
{
    malloc_zone_t *z = brz_native(g);
    if (z->batch_free) {
        z->batch_free(z, ptrs, n);
        return;
    }
    for (unsigned k = 0; k < n; k++)
        if (ptrs[k])
            z->free(z, ptrs[k]);
}

static void *brz_memalign(malloc_zone_t *g, size_t align, size_t n)
{
    malloc_zone_t *z = brz_native(g);
    return z->memalign ? z->memalign(z, align, n) : NULL;
}

static void brz_free_definite_size(malloc_zone_t *g, void *p, size_t n)
{
    malloc_zone_t *z = brz_native(g);
    if (z->version >= 6 && z->free_definite_size)
        z->free_definite_size(z, p, n);
    else
        z->free(z, p);
}

static size_t brz_pressure_relief(malloc_zone_t *g, size_t goal)
{
    malloc_zone_t *z = brz_native(g);
    return z->version >= 8 && z->pressure_relief ? z->pressure_relief(z, goal) : 0;
}

static boolean_t brz_claimed_address(malloc_zone_t *g, void *p)
{
    malloc_zone_t *z = brz_native(g);
    if (z->version >= 10 && z->claimed_address)
        return z->claimed_address(z, p);
    return z->size && z->size(z, p) != 0;
}

static void brz_try_free_default(malloc_zone_t *g, void *p)
{
    malloc_zone_t *z = brz_native(g);
    if (z->version >= 13 && z->try_free_default)
        z->try_free_default(z, p);
    else
        free(p);
}

static size_t brz_good_size(malloc_zone_t *g, size_t n)
{
    malloc_zone_t *z = brz_native(g);
    return z->introspect && z->introspect->good_size ? z->introspect->good_size(z, n) : malloc_good_size(n);
}

static boolean_t brz_check(malloc_zone_t *g)
{
    malloc_zone_t *z = brz_native(g);
    return z->introspect && z->introspect->check ? z->introspect->check(z) : 1;
}

static void brz_print(malloc_zone_t *g, boolean_t verbose)
{
    malloc_zone_t *z = brz_native(g);
    if (z->introspect && z->introspect->print)
        z->introspect->print(z, verbose);
}

static void brz_log(malloc_zone_t *g, void *address)
{
    malloc_zone_t *z = brz_native(g);
    if (z->introspect && z->introspect->log)
        z->introspect->log(z, address);
}

static void brz_force_lock(malloc_zone_t *g)
{
    malloc_zone_t *z = brz_native(g);
    if (z->introspect && z->introspect->force_lock)
        z->introspect->force_lock(z);
}

static void brz_force_unlock(malloc_zone_t *g)
{
    malloc_zone_t *z = brz_native(g);
    if (z->introspect && z->introspect->force_unlock)
        z->introspect->force_unlock(z);
}

static void brz_statistics(malloc_zone_t *g, malloc_statistics_t *stats)
{
    malloc_zone_statistics(brz_native(g), stats);
}

static boolean_t brz_zone_locked(malloc_zone_t *g)
{
    malloc_zone_t *z = brz_native(g);
    return z->introspect && z->introspect->zone_locked ? z->introspect->zone_locked(z) : 0;
}

static void brz_reinit_lock(malloc_zone_t *g)
{
    malloc_zone_t *z = brz_native(g);
    if (z->version >= 9 && z->introspect && z->introspect->reinit_lock)
        z->introspect->reinit_lock(z);
}

#define BR_ZONE_INTROSPECT_WORD 12
#define BR_ZONE_INTROSPECT_AT 0x400u
#define BR_ZONE_INTROSPECT_WORDS 17

static const struct {
    int word;
    const void *fn;
    const char *name;
    const char *notation;
} g_br_introspect_fns[] = {
    { 1, brz_good_size, "(zone good_size)", "L(pL)" },
    { 2, brz_check, "(zone check)", "i(p)" },
    { 3, brz_print, "(zone print)", "v(pi)" },
    { 4, brz_log, "(zone log)", "v(pp)" },
    { 5, brz_force_lock, "(zone force_lock)", "v(p)" },
    { 6, brz_force_unlock, "(zone force_unlock)", "v(p)" },
    { 7, brz_statistics, "(zone statistics)", "v(pp)" },
    { 8, brz_zone_locked, "(zone zone_locked)", "i(p)" },
    { 13, brz_reinit_lock, "(zone reinit_lock)", "v(p)" },
};

static const struct {
    int word;
    const void *fn;
    const char *name;
    const char *notation;
} g_br_zone_fns[] = {
    { 2, brz_size, "(zone size)", "L(pp)" },
    { 3, brz_malloc, "(zone malloc)", "p(pL)" },
    { 4, brz_calloc, "(zone calloc)", "p(pLL)" },
    { 5, brz_valloc, "(zone valloc)", "p(pL)" },
    { 6, brz_free, "(zone free)", "v(pp)" },
    { 7, brz_realloc, "(zone realloc)", "p(ppL)" },
    { 8, brz_destroy, "(zone destroy)", "v(p)" },
    { 10, brz_batch_malloc, "(zone batch_malloc)", "u(pLpu)" },
    { 11, brz_batch_free, "(zone batch_free)", "v(ppu)" },
    { 14, brz_memalign, "(zone memalign)", "p(pLL)" },
    { 15, brz_free_definite_size, "(zone free_definite_size)", "v(ppL)" },
    { 16, brz_pressure_relief, "(zone pressure_relief)", "L(pL)" },
    { 17, brz_claimed_address, "(zone claimed_address)", "i(pp)" },
    { 18, brz_try_free_default, "(zone try_free_default)", "v(pp)" },
};

static uint64_t br_zone_view(void *native_zone)
{
    if (!native_zone)
        return 0;
    malloc_zone_t *z = native_zone;
    pthread_mutex_lock(&g_br_views_lock);
    unsigned n = atomic_load(&g_br_views_n), k;
    for (k = 0; k < n; k++)
        if (g_br_views[k].native == z)
            break;
    uint64_t answer = k < n ? g_br_views[k].view : 0;
    if (!answer && n < BR_ZONE_VIEWS) {
        uint64_t page = ocerz_map_anywhere(OCERZ_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE);
        BrZoneView *v = &g_br_views[n];
        int ok = page != 0;
        memset(v->made, 0, sizeof v->made);
        for (size_t f = 0; ok && f < sizeof g_br_zone_fns / sizeof g_br_zone_fns[0]; f++) {
            v->made[g_br_zone_fns[f].word] = ocerz_bridge_native_thunk(
                g_br_zone_fns[f].fn, g_br_zone_fns[f].name, g_br_zone_fns[f].notation);
            ok = v->made[g_br_zone_fns[f].word] != 0;
        }
        uint64_t introspect[BR_ZONE_INTROSPECT_WORDS] = { 0 };
        for (size_t f = 0; ok && f < sizeof g_br_introspect_fns / sizeof g_br_introspect_fns[0]; f++) {
            introspect[g_br_introspect_fns[f].word] = ocerz_bridge_native_thunk(
                g_br_introspect_fns[f].fn, g_br_introspect_fns[f].name, g_br_introspect_fns[f].notation);
            ok = introspect[g_br_introspect_fns[f].word] != 0;
        }
        if (ok) {
            uint32_t version = z->version < BR_ZONE_VERSION_CAP ? z->version : BR_ZONE_VERSION_CAP;
            v->made[BR_ZONE_INTROSPECT_WORD] = page + BR_ZONE_INTROSPECT_AT;
            for (int w = 0; w < BR_ZONE_INTROSPECT_WORDS; w++)
                ocerz_st(page + BR_ZONE_INTROSPECT_AT + 8u * (unsigned)w, 8, introspect[w]);
            v->made[9] = z->zone_name ? ocerz_h2g((void *)(uintptr_t)z->zone_name) : 0;
            v->made[BR_ZONE_VERSION_WORD] = version;
            for (int w = 0; w < BR_ZONE_WORDS; w++)
                ocerz_st(page + 8u * (unsigned)w, 8, v->made[w]);
            v->view = page;
            v->native = z;
            atomic_store(&g_br_views_n, n + 1);
            answer = page;
        } else if (page) {
            ocerz_unmap(page, OCERZ_GUEST_PAGE_SIZE);
        }
    }
    pthread_mutex_unlock(&g_br_views_lock);
    return answer;
}

#define BR_ZONE_SLOTS 64

static uint64_t g_br_eff[BR_ZONE_SLOTS];
static int g_br_eff_n;
static int g_br_eff_seeded;
static pthread_mutex_t g_br_zones_lock = PTHREAD_MUTEX_INITIALIZER;

static void br_zone_seed_locked(void)
{
    if (g_br_eff_seeded)
        return;
    g_br_eff_seeded = 1;
    void *fn = ocerz_bridge_host_symbol(OCERZ_BRIDGE_LIBSYSTEM, "malloc_get_all_zones");
    if (!fn)
        return;
    uint64_t addrs = 0;
    uint32_t count = 0;
    kern_return_t kr = ((kern_return_t (*)(uint32_t, void *, uint64_t, uint64_t))fn)(
        (uint32_t)mach_task_self(), NULL, (uint64_t)(uintptr_t)&addrs, (uint64_t)(uintptr_t)&count);
    if (kr != 0)
        return;
    uint64_t *list = addrs ? (uint64_t *)(uintptr_t)addrs : NULL;
    for (uint32_t k = 0; k < count && g_br_eff_n < BR_ZONE_SLOTS; k++) {
        uint64_t view = br_zone_view((void *)(uintptr_t)list[k]);
        if (view)
            g_br_eff[g_br_eff_n++] = view;
    }
}

static void br_zone_add(uint64_t zone)
{
    if (!zone)
        return;
    pthread_mutex_lock(&g_br_zones_lock);
    br_zone_seed_locked();
    if (g_br_eff_n < BR_ZONE_SLOTS)
        g_br_eff[g_br_eff_n++] = zone;
    pthread_mutex_unlock(&g_br_zones_lock);
}

static void br_zone_remove(uint64_t zone)
{
    if (!zone)
        return;
    pthread_mutex_lock(&g_br_zones_lock);
    br_zone_seed_locked();
    for (int k = 0; k < g_br_eff_n; k++) {
        if (g_br_eff[k] == zone) {
            g_br_eff[k] = g_br_eff[--g_br_eff_n];
            break;
        }
    }
    pthread_mutex_unlock(&g_br_zones_lock);
}

static int br_malloc_zone_register_tracked(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t zone = cpu->gpr[OCERZ_RDI];
    OCERZ_LOG("bridge: malloc zone register takes zone %#llx, tracked for guest queries\n",
              (unsigned long long)zone);
    br_zone_add(zone);
    return br_answer(vm, cpu, 0);
}

static int br_malloc_zone_unregister_tracked(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t zone = cpu->gpr[OCERZ_RDI];
    OCERZ_LOG("bridge: malloc zone unregister takes zone %#llx\n", (unsigned long long)zone);
    br_zone_remove(zone);
    return br_answer(vm, cpu, 0);
}

static int br_malloc_default_zone_tracked(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t first = 0;
    pthread_mutex_lock(&g_br_zones_lock);
    br_zone_seed_locked();
    if (g_br_eff_n > 0)
        first = g_br_eff[0];
    pthread_mutex_unlock(&g_br_zones_lock);
    if (first)
        return br_answer(vm, cpu, first);
    static void *_Atomic cached = NULL;
    void *fn = cached;
    if (!fn) {
        fn = ocerz_bridge_host_symbol(OCERZ_BRIDGE_LIBSYSTEM, "malloc_default_zone");
        if (!fn)
            return br_answer(vm, cpu, 0);
        cached = fn;
    }
    void *r = ((void *(*)(void))fn)();
    return br_answer(vm, cpu, br_zone_view(r));
}

static int br_malloc_default_purgeable_zone(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *fn = ocerz_bridge_host_symbol(OCERZ_BRIDGE_LIBSYSTEM, "malloc_default_purgeable_zone");
    if (!fn)
        return br_answer(vm, cpu, 0);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, OCERZ_BRIDGE_LIBSYSTEM, "_malloc_default_purgeable_zone", "p()", fn);
    void *r = ((void *(*)(void))fn)();
    ocerz_bridge_lower(&outer);
    uint64_t view = br_zone_view(r);
    int known = 0;
    pthread_mutex_lock(&g_br_zones_lock);
    br_zone_seed_locked();
    for (int k = 0; k < g_br_eff_n; k++)
        known |= g_br_eff[k] == view;
    pthread_mutex_unlock(&g_br_zones_lock);
    if (!known)
        br_zone_add(view);
    return br_answer(vm, cpu, view);
}

static int br_malloc_get_all_zones_tracked(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t reader = cpu->gpr[OCERZ_RSI];
    void *native_reader = NULL;
    if (reader) {
        native_reader = ocerz_abi_callback_intern(reader, "i(uLLp)");
        if (!native_reader)
            return br_answer(vm, cpu, (uint64_t)(uint32_t)KERN_FAILURE);
    }
    static void *_Atomic cached = NULL;
    void *fn = cached;
    if (!fn) {
        fn = ocerz_bridge_host_symbol(OCERZ_BRIDGE_LIBSYSTEM, "malloc_get_all_zones");
        if (!fn)
            return br_answer(vm, cpu, (uint64_t)(uint32_t)KERN_FAILURE);
        cached = fn;
    }
    uint64_t task = cpu->gpr[OCERZ_RDI];
    uint64_t addresses = cpu->gpr[OCERZ_RDX];
    uint64_t countp = cpu->gpr[OCERZ_RCX];
    kern_return_t kr = ((kern_return_t (*)(uint32_t, void *, uint64_t, uint64_t))fn)(
        (uint32_t)task, native_reader, addresses, countp);
    if (kr != 0 || !addresses || !countp)
        return br_answer(vm, cpu, (uint64_t)(uint32_t)kr);
    uint64_t eff[BR_ZONE_SLOTS];
    int neff = 0;
    pthread_mutex_lock(&g_br_zones_lock);
    br_zone_seed_locked();
    for (int k = 0; k < g_br_eff_n && neff < BR_ZONE_SLOTS; k++)
        eff[neff++] = g_br_eff[k];
    pthread_mutex_unlock(&g_br_zones_lock);
    if (neff == 0)
        return br_answer(vm, cpu, (uint64_t)(uint32_t)kr);
    uint64_t *out = malloc((size_t)neff * 8);
    if (!out)
        return br_answer(vm, cpu, (uint64_t)(uint32_t)kr);
    for (int k = 0; k < neff; k++)
        out[k] = eff[k];
    ocerz_st(addresses, 8, (uint64_t)(uintptr_t)out);
    ocerz_st(countp, 4, (uint64_t)neff);
    return br_answer(vm, cpu, (uint64_t)(uint32_t)kr);
}

#define BR_ZONE_SIZE 16
#define BR_ZONE_MALLOC 24
#define BR_ZONE_CALLOC 32
#define BR_ZONE_VALLOC 40
#define BR_ZONE_FREE 48
#define BR_ZONE_REALLOC 56
#define BR_ZONE_DESTROY 64
#define BR_ZONE_NAME 72
#define BR_ZONE_MEMALIGN 112
#define BR_ZONE_PRESSURE_RELIEF 128

static int br_zone_is_guest(uint64_t zone)
{
    if (!zone)
        return 0;
    uint64_t fn = ocerz_ld(zone + BR_ZONE_MALLOC, 8);
    return fn && ocerz_abi_is_guest_code(fn);
}

static int br_zone_forward(struct OcerzVM *vm, OcerzCPU *cpu, const char *export_name,
                           const char *sig, unsigned slot)
{
    uint64_t zone = cpu->gpr[OCERZ_RDI];
    const BrZoneView *view = br_view_find(zone);
    uint64_t target = zone ? ocerz_ld(zone + slot, 8) : 0;
    if (view ? target != view->made[slot / 8] : br_zone_is_guest(zone)) {
        if (!target)
            return br_answer(vm, cpu, 0);
        cpu->rip = target;
        return OCERZ_STEP_OK;
    }
    void *fn = ocerz_bridge_host_symbol(OCERZ_BRIDGE_LIBSYSTEM, export_name + 1);
    if (!fn)
        return br_answer(vm, cpu, 0);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, OCERZ_BRIDGE_LIBSYSTEM, export_name, sig, fn);
    uint64_t r = ((uint64_t (*)(uint64_t, uint64_t, uint64_t))fn)(
        view ? (uint64_t)(uintptr_t)view->native : zone, cpu->gpr[OCERZ_RSI], cpu->gpr[OCERZ_RDX]);
    ocerz_bridge_lower(&outer);
    return br_answer(vm, cpu, r);
}

static int br_malloc_zone_malloc(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_zone_forward(vm, cpu, "_malloc_zone_malloc", "p(pL)", BR_ZONE_MALLOC);
}

static int br_malloc_zone_calloc(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_zone_forward(vm, cpu, "_malloc_zone_calloc", "p(pLL)", BR_ZONE_CALLOC);
}

static int br_malloc_zone_valloc(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_zone_forward(vm, cpu, "_malloc_zone_valloc", "p(pL)", BR_ZONE_VALLOC);
}

static int br_malloc_zone_free(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_zone_forward(vm, cpu, "_malloc_zone_free", "v(pp)", BR_ZONE_FREE);
}

static int br_malloc_zone_realloc(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_zone_forward(vm, cpu, "_malloc_zone_realloc", "p(ppL)", BR_ZONE_REALLOC);
}

static int br_malloc_zone_memalign(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_zone_forward(vm, cpu, "_malloc_zone_memalign", "p(pLL)", BR_ZONE_MEMALIGN);
}

static int br_malloc_zone_pressure_relief(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_zone_forward(vm, cpu, "_malloc_zone_pressure_relief", "L(pL)",
                           BR_ZONE_PRESSURE_RELIEF);
}

static int br_malloc_destroy_zone(struct OcerzVM *vm, OcerzCPU *cpu)
{
    br_zone_remove(cpu->gpr[OCERZ_RDI]);
    return br_zone_forward(vm, cpu, "_malloc_destroy_zone", "v(p)", BR_ZONE_DESTROY);
}

static int br_malloc_create_zone(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *fn = ocerz_bridge_host_symbol(OCERZ_BRIDGE_LIBSYSTEM, "malloc_create_zone");
    if (!fn)
        return br_answer(vm, cpu, 0);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, OCERZ_BRIDGE_LIBSYSTEM, "_malloc_create_zone", "p(Lu)", fn);
    void *zone = ((void *(*)(uint64_t, unsigned))fn)(cpu->gpr[OCERZ_RDI],
                                                     (unsigned)cpu->gpr[OCERZ_RSI]);
    ocerz_bridge_lower(&outer);
    uint64_t g = br_zone_view(zone);
    br_zone_add(g);
    return br_answer(vm, cpu, g);
}

static int br_malloc_zone_from_ptr(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t ptr = cpu->gpr[OCERZ_RDI];
    void *fn = ocerz_bridge_host_symbol(OCERZ_BRIDGE_LIBSYSTEM, "malloc_zone_from_ptr");
    void *zone = NULL;
    if (fn) {
        struct OcerzBridgeFrame outer;
        ocerz_bridge_raise(&outer, OCERZ_BRIDGE_LIBSYSTEM, "_malloc_zone_from_ptr", "p(p)", fn);
        zone = ((void *(*)(const void *))fn)(ptr ? ocerz_g2h(ptr) : NULL);
        ocerz_bridge_lower(&outer);
    }
    if (zone)
        return br_answer(vm, cpu, br_zone_view(zone));
    uint64_t eff[BR_ZONE_SLOTS];
    int neff = 0;
    pthread_mutex_lock(&g_br_zones_lock);
    br_zone_seed_locked();
    for (int k = 0; k < g_br_eff_n; k++)
        eff[neff++] = g_br_eff[k];
    pthread_mutex_unlock(&g_br_zones_lock);
    for (int k = 0; k < neff; k++) {
        if (br_view_find(eff[k]) || !br_zone_is_guest(eff[k]))
            continue;
        uint64_t size_fn = ocerz_ld(eff[k] + BR_ZONE_SIZE, 8);
        if (!size_fn)
            continue;
        uint64_t args[2] = { eff[k], ptr };
        if (ocerz_vm_call(vm, size_fn, args, 2, (cpu->gpr[OCERZ_RSP] - 256) & ~0xfull))
            return br_answer(vm, cpu, eff[k]);
    }
    return br_answer(vm, cpu, 0);
}

static int br_malloc_get_zone_name(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t zone = cpu->gpr[OCERZ_RDI];
    return br_answer(vm, cpu, zone ? ocerz_ld(zone + BR_ZONE_NAME, 8) : 0);
}

static int br_malloc_set_zone_name(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t zone = cpu->gpr[OCERZ_RDI];
    uint64_t name = cpu->gpr[OCERZ_RSI];
    const BrZoneView *view = br_view_find(zone);
    if (view || br_zone_is_guest(zone)) {
        char *copy = name ? strdup((const char *)ocerz_g2h(name)) : NULL;
        ocerz_st(zone + BR_ZONE_NAME, 8, copy ? ocerz_h2g(copy) : 0);
        if (view)
            malloc_set_zone_name(view->native, copy);
        return br_answer(vm, cpu, 0);
    }
    malloc_set_zone_name((malloc_zone_t *)ocerz_g2h(zone), name ? (const char *)ocerz_g2h(name) : NULL);
    return br_answer(vm, cpu, 0);
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

void ocerz_bridge_return(OcerzCPU *cpu, uint64_t rax)
{
    br_return(cpu, rax);
}

int ocerz_bridge_settle(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_settle(vm, cpu);
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

static uint64_t br_stack_below(const OcerzCPU *cpu)
{
    return (cpu->gpr[OCERZ_RSP] - 128) & ~15ull;
}

static uint64_t br_caller(const OcerzCPU *cpu)
{
    return ocerz_ld(cpu->gpr[OCERZ_RSP], 8);
}

static const char *br_guest_path(uint64_t g, char *buf, size_t n)
{
    if (!g)
        return NULL;
    snprintf(buf, n, "%s", (const char *)ocerz_g2h(g));
    return buf;
}

static int br_answer(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t rax)
{
    br_return(cpu, rax);
    return br_settle(vm, cpu);
}

static int br_dlopen_at(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t caller)
{
    char path[4096];
    const char *p = br_guest_path(cpu->gpr[OCERZ_RDI], path, sizeof path);
    uint64_t h = ocerz_dyld_native_dlopen(vm, p, (int)cpu->gpr[OCERZ_RSI], caller, br_stack_below(cpu));
    return br_answer(vm, cpu, h);
}

static int br_dlopen(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_dlopen_at(vm, cpu, br_caller(cpu));
}

static int br_dlopen_from(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_dlopen_at(vm, cpu, cpu->gpr[OCERZ_RDX]);
}

static int br_dlopen_preflight(struct OcerzVM *vm, OcerzCPU *cpu)
{
    char path[4096];
    const char *p = br_guest_path(cpu->gpr[OCERZ_RDI], path, sizeof path);
    return br_answer(vm, cpu, (uint64_t)ocerz_dyld_native_dlopen_preflight(p, br_caller(cpu)));
}

static int br_dlsym(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t name = cpu->gpr[OCERZ_RSI];
    return br_answer(vm, cpu, ocerz_dyld_native_dlsym(cpu->gpr[OCERZ_RDI],
                                                      name ? (const char *)ocerz_g2h(name) : NULL,
                                                      br_caller(cpu)));
}

static int br_dladdr(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_answer(vm, cpu, (uint64_t)ocerz_dyld_native_dladdr(cpu->gpr[OCERZ_RDI], cpu->gpr[OCERZ_RSI]));
}

static int br_dyld_unwind_sections(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_answer(vm, cpu, ocerz_dyld_unwind_sections(cpu->gpr[OCERZ_RDI], cpu->gpr[OCERZ_RSI]));
}

static struct {
    uint32_t task;
    uint32_t mask;
    uint32_t port;
    int behavior;
    int flavor;
    int valid;
} g_br_exc_ports[8];

static int br_task_set_exception_ports(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t task = (uint32_t)cpu->gpr[OCERZ_RDI];
    uint32_t mask = (uint32_t)cpu->gpr[OCERZ_RSI];
    uint32_t port = (uint32_t)cpu->gpr[OCERZ_RDX];
    int behavior = (int)cpu->gpr[OCERZ_RCX];
    int flavor = (int)cpu->gpr[OCERZ_R8];
    OCERZ_LOG("bridge: task_set_exception_ports task=%u mask=%#x port=%u behavior=%d flavor=%d recorded, faults stay with translated code\n",
              task, mask, port, behavior, flavor);
    int slot = -1;
    for (int k = 0; k < 8; k++)
        if (g_br_exc_ports[k].valid && g_br_exc_ports[k].task == task)
            slot = k;
    if (slot < 0)
        for (int k = 0; k < 8 && slot < 0; k++)
            if (!g_br_exc_ports[k].valid)
                slot = k;
    if (slot < 0)
        slot = 0;
    g_br_exc_ports[slot].task = task;
    g_br_exc_ports[slot].mask = mask;
    g_br_exc_ports[slot].port = port;
    g_br_exc_ports[slot].behavior = behavior;
    g_br_exc_ports[slot].flavor = flavor;
    g_br_exc_ports[slot].valid = 1;
    return br_answer(vm, cpu, 0);
}

static int br_swap_exception_ports(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t count_at = ocerz_ld(cpu->gpr[OCERZ_RSP] + 8, 8);
    if (count_at)
        ocerz_st(count_at, 4, 0);
    return br_task_set_exception_ports(vm, cpu);
}

static int br_abort_report(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *fmt = cpu->gpr[OCERZ_RDI] ? ocerz_g2h(cpu->gpr[OCERZ_RDI]) : "(no message)";
    fprintf(stderr, "ocerz: bridge: the guest called abort_report_np: %s\n", fmt);
    return br_abort(vm, cpu);
}

static int br_susp_logging(void)
{
    static int en = -1;
    if (en < 0) en = getenv("OCERZ_SUSPLOG") ? 1 : 0;
    return en;
}

static int br_thread_suspend(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t port = (uint32_t)cpu->gpr[OCERZ_RDI];
    int kr = ocerz_vm_thread_suspend_native(port);
    if (br_susp_logging())
        fprintf(stderr, "ocerz: SUSPLOG[%d] suspend port=%#x answer=%d\n", (int)getpid(), port, kr);
    return br_answer(vm, cpu, (uint64_t)(uint32_t)kr);
}

static int br_thread_resume(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t port = (uint32_t)cpu->gpr[OCERZ_RDI];
    int kr = ocerz_vm_thread_resume_native(port);
    if (br_susp_logging())
        fprintf(stderr, "ocerz: SUSPLOG[%d] resume port=%#x answer=%d\n", (int)getpid(), port, kr);
    return br_answer(vm, cpu, (uint64_t)(uint32_t)kr);
}

static int br_thread_get_state(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t port = (uint32_t)cpu->gpr[OCERZ_RDI];
    uint32_t flavor = (uint32_t)cpu->gpr[OCERZ_RSI];
    uint64_t state = cpu->gpr[OCERZ_RDX];
    uint64_t countp = cpu->gpr[OCERZ_RCX];
    if (flavor != 4) {
        fprintf(stderr, "ocerz: bridge: _thread_get_state takes flavor %u, which has no x86 register mapping here\n",
                flavor);
        exit(OCERZ_BRIDGE_UNIMPL_EXIT);
    }
    if (!state || !countp)
        return br_answer(vm, cpu, KERN_FAILURE);
    uint64_t g[16], rip, rfl;
    if (ocerz_vm_thread_regs(port, g, &rip, &rfl) < 0)
        return br_answer(vm, cpu, KERN_INVALID_ARGUMENT);
    uint32_t want = (uint32_t)ocerz_ld(countp, 4);
    if (want < 42)
        return br_answer(vm, cpu, KERN_INVALID_ARGUMENT);
    uint64_t s[21] = { g[OCERZ_RAX], g[OCERZ_RBX], g[OCERZ_RCX], g[OCERZ_RDX],
                       g[OCERZ_RDI], g[OCERZ_RSI], g[OCERZ_RBP], g[OCERZ_RSP],
                       g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15],
                       rip, rfl | OCERZ_FLAG_FIXED1, 0x2b, 0, 0 };
    for (int k = 0; k < 21; k++)
        ocerz_st(state + 8 * (uint64_t)k, 8, s[k]);
    ocerz_st(countp, 4, 42);
    return br_answer(vm, cpu, 0);
}

typedef const void *(*BrCFUUIDFn)(const void *, unsigned, unsigned, unsigned, unsigned, unsigned,
                                  unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                                  unsigned, unsigned, unsigned, unsigned, unsigned);

static int br_cfuuid_constant(struct OcerzVM *vm, OcerzCPU *cpu)
{
    static void *_Atomic cached = NULL;
    void *fn = cached;
    if (!fn) {
        fn = ocerz_bridge_host_symbol(OCERZ_BRIDGE_COREFOUNDATION, "CFUUIDGetConstantUUIDWithBytes");
        if (!fn) {
            fprintf(stderr, "ocerz: bridge: _CFUUIDGetConstantUUIDWithBytes has no host symbol\n");
            exit(OCERZ_BRIDGE_UNIMPL_EXIT);
        }
        cached = fn;
    }
    OcerzAbiSig named;
    if (ocerz_abi_parse("p", &named) != OCERZ_OK)
        return br_answer(vm, cpu, 0);
    OcerzAbiVaList va;
    if (ocerz_abi_va_start(&named, cpu, &va) != OCERZ_OK)
        return br_answer(vm, cpu, 0);
    uint64_t raw = cpu->gpr[OCERZ_RDI];
    const void *alloc = raw ? ocerz_g2h(raw) : NULL;
    unsigned b[16];
    for (int k = 0; k < 16; k++) {
        uint64_t v = 0;
        if (ocerz_abi_va_arg(&va, cpu, 'u', &v) != OCERZ_OK)
            return br_answer(vm, cpu, 0);
        b[k] = (unsigned)(v & 0xffu);
    }
    const void *r = ((BrCFUUIDFn)fn)(alloc, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                                     b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return br_answer(vm, cpu, r ? ocerz_h2g(r) : 0);
}

static int br_availability_version_check(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t want = (uint32_t)cpu->gpr[OCERZ_RDI];
    char buf[32];
    size_t len = sizeof buf;
    uint32_t have = 0;
    if (sysctlbyname("kern.osproductversion", buf, &len, NULL, 0) == 0 && len > 0 && len < sizeof buf) {
        unsigned maj = 0, min = 0, pat = 0;
        sscanf(buf, "%u.%u.%u", &maj, &min, &pat);
        if (maj > 0xffffu)
            maj = 0xffffu;
        if (min > 0xffu)
            min = 0xffu;
        if (pat > 0xffu)
            pat = 0xffu;
        have = (uint32_t)(maj << 16) | (uint32_t)(min << 8) | (uint32_t)pat;
    }
    return br_answer(vm, cpu, have >= want ? 1 : 0);
}

static int br_dlclose(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_answer(vm, cpu, (uint64_t)(uint32_t)ocerz_dyld_native_dlclose(cpu->gpr[OCERZ_RDI]));
}

static int br_dlerror(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_answer(vm, cpu, ocerz_dyld_native_dlerror());
}

static int br_dyld_image_count(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_answer(vm, cpu, ocerz_dyld_image_count());
}

static int br_dyld_image_header(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t mh = 0;
    ocerz_dyld_image_at((uint32_t)cpu->gpr[OCERZ_RDI], &mh, NULL, NULL);
    return br_answer(vm, cpu, mh);
}

static int br_dyld_image_name(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t name = 0;
    ocerz_dyld_image_at((uint32_t)cpu->gpr[OCERZ_RDI], NULL, NULL, &name);
    return br_answer(vm, cpu, name);
}

static int br_dyld_image_vmaddr_slide(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t slide = 0;
    ocerz_dyld_image_at((uint32_t)cpu->gpr[OCERZ_RDI], NULL, &slide, NULL);
    return br_answer(vm, cpu, slide);
}

static int br_dyld_image_slide(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t slide = 0;
    ocerz_dyld_image_slide(cpu->gpr[OCERZ_RDI], &slide);
    return br_answer(vm, cpu, slide);
}

static int br_dyld_register_add_image(struct OcerzVM *vm, OcerzCPU *cpu)
{
    ocerz_dyld_native_add_image_func(vm, cpu->gpr[OCERZ_RDI], br_stack_below(cpu));
    return br_answer(vm, cpu, 0);
}

static int br_dyld_register_remove_image(struct OcerzVM *vm, OcerzCPU *cpu)
{
    ocerz_dyld_native_remove_image_func(cpu->gpr[OCERZ_RDI]);
    return br_answer(vm, cpu, 0);
}

static int br_dyld_image_header_containing(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t mh = 0;
    ocerz_dyld_image_containing(cpu->gpr[OCERZ_RDI], &mh, NULL);
    return br_answer(vm, cpu, mh);
}

static int br_dyld_image_path_containing(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t name = 0;
    ocerz_dyld_image_containing(cpu->gpr[OCERZ_RDI], NULL, &name);
    return br_answer(vm, cpu, name);
}

static int br_dyld_image_containing(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_answer(vm, cpu, (uint64_t)ocerz_dyld_image_containing(cpu->gpr[OCERZ_RDI], NULL, NULL));
}

static int br_dyld_prog_image_header(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_answer(vm, cpu, ocerz_main_mh);
}

static int br_dyld_build_field(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t mh, int field)
{
    uint32_t v[3] = { 0, 0, 0 };
    ocerz_dyld_build_version(mh, &v[0], &v[1], &v[2]);
    return br_answer(vm, cpu, v[field]);
}

static int br_dyld_program_sdk_version(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_dyld_build_field(vm, cpu, ocerz_main_mh, 2);
}

static int br_dyld_program_min_os_version(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_dyld_build_field(vm, cpu, ocerz_main_mh, 1);
}

static int br_dyld_sdk_version(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_dyld_build_field(vm, cpu, cpu->gpr[OCERZ_RDI], 2);
}

static int br_dyld_min_os_version(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_dyld_build_field(vm, cpu, cpu->gpr[OCERZ_RDI], 1);
}

static int br_dyld_active_platform(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t platform = 0, minos, sdk;
    ocerz_dyld_build_version(ocerz_main_mh, &platform, &minos, &sdk);
    return br_answer(vm, cpu, platform ? platform : PLATFORM_MACOS);
}

static int br_dyld_program_sdk_at_least(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_answer(vm, cpu, (uint64_t)ocerz_dyld_version_at_least(ocerz_main_mh, cpu->gpr[OCERZ_RDI], 1));
}

static int br_dyld_program_minos_at_least(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_answer(vm, cpu, (uint64_t)ocerz_dyld_version_at_least(ocerz_main_mh, cpu->gpr[OCERZ_RDI], 0));
}

static int br_dyld_sdk_at_least(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_answer(vm, cpu, (uint64_t)ocerz_dyld_version_at_least(cpu->gpr[OCERZ_RDI], cpu->gpr[OCERZ_RSI], 1));
}

static int br_dyld_minos_at_least(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_answer(vm, cpu, (uint64_t)ocerz_dyld_version_at_least(cpu->gpr[OCERZ_RDI], cpu->gpr[OCERZ_RSI], 0));
}

static int br_dyld_is_memory_immutable(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_answer(vm, cpu, (uint64_t)ocerz_dyld_is_memory_immutable(cpu->gpr[OCERZ_RDI], cpu->gpr[OCERZ_RSI]));
}

static int br_dyld_cache_some_image_overridden(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return br_answer(vm, cpu, 0);
}

static int br_dyld_cache_contains_path(struct OcerzVM *vm, OcerzCPU *cpu)
{
    char path[4096];
    const char *p = br_guest_path(cpu->gpr[OCERZ_RDI], path, sizeof path);
    return br_answer(vm, cpu, (uint64_t)ocerz_dyld_native_names_library(p));
}

typedef struct BrHandler {
    const char *name;
    int (*fn)(struct OcerzVM *vm, OcerzCPU *cpu);
} BrHandler;

static const BrHandler g_br_handlers[] = {
    { "exit",            br_exit },
    { "exit_now",        br_exit_now },
    { "abort",           br_abort },
    { "error",           br_error },
    { "bzero",           br_bzero },
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
    { "dlopen",          br_dlopen },
    { "dlopen_from",     br_dlopen_from },
    { "dlopen_preflight", br_dlopen_preflight },
    { "dlsym",           br_dlsym },
    { "dladdr",          br_dladdr },
    { "dyld_unwind_sections", br_dyld_unwind_sections },
    { "task_set_exception_ports", br_task_set_exception_ports },
    { "swap_exception_ports",     br_swap_exception_ports },
    { "abort_report",             br_abort_report },
    { "mach_vm_map",              ocerz_sys_mach_vm_map },
    { "mach_vm_remap",            ocerz_sys_mach_vm_remap },
    { "pthread_get_stackaddr_np", ocerz_sys_pthread_get_stackaddr_np },
    { "pthread_get_stacksize_np", ocerz_sys_pthread_get_stacksize_np },
    { "getpagesize", ocerz_sys_getpagesize },
    { "sysconf", ocerz_sys_sysconf },
    { "host_page_size", ocerz_sys_host_page_size },
    { "sysctl", ocerz_sys_sysctl },
    { "sysctlbyname", ocerz_sys_sysctlbyname },
    { "sandbox_check",            ocerz_sys_sandbox_check },
    { "sandbox_init",             ocerz_sys_sandbox_init },
    { "sandbox_init_with_parameters", ocerz_sys_sandbox_init_with_parameters },
    { "sandbox_apply",            ocerz_sys_sandbox_apply },
    { "sandbox_ms",               ocerz_sys_sandbox_ms },
    { "malloc_zone_register", br_malloc_zone_register_tracked },
    { "malloc_zone_unregister", br_malloc_zone_unregister_tracked },
    { "malloc_default_zone", br_malloc_default_zone_tracked },
    { "malloc_default_purgeable_zone", br_malloc_default_purgeable_zone },
    { "malloc_zone_malloc", br_malloc_zone_malloc },
    { "malloc_zone_calloc", br_malloc_zone_calloc },
    { "malloc_zone_valloc", br_malloc_zone_valloc },
    { "malloc_zone_free", br_malloc_zone_free },
    { "malloc_zone_realloc", br_malloc_zone_realloc },
    { "malloc_zone_memalign", br_malloc_zone_memalign },
    { "malloc_zone_pressure_relief", br_malloc_zone_pressure_relief },
    { "malloc_destroy_zone", br_malloc_destroy_zone },
    { "malloc_create_zone", br_malloc_create_zone },
    { "malloc_zone_from_ptr", br_malloc_zone_from_ptr },
    { "malloc_get_zone_name", br_malloc_get_zone_name },
    { "malloc_set_zone_name", br_malloc_set_zone_name },
    { "malloc_get_all_zones", br_malloc_get_all_zones_tracked },
    { "thread_get_state", br_thread_get_state },
    { "thread_suspend", br_thread_suspend },
    { "thread_resume", br_thread_resume },
    { "cfuuid_constant", br_cfuuid_constant },
    { "availability_version_check", br_availability_version_check },
    { "dlclose",         br_dlclose },
    { "dlerror",         br_dlerror },
    { "dyld_image_count",            br_dyld_image_count },
    { "dyld_image_header",           br_dyld_image_header },
    { "dyld_image_name",             br_dyld_image_name },
    { "dyld_image_vmaddr_slide",     br_dyld_image_vmaddr_slide },
    { "dyld_image_slide",            br_dyld_image_slide },
    { "dyld_register_add_image",     br_dyld_register_add_image },
    { "dyld_register_remove_image",  br_dyld_register_remove_image },
    { "dyld_image_header_containing", br_dyld_image_header_containing },
    { "dyld_image_path_containing",  br_dyld_image_path_containing },
    { "dyld_image_containing",       br_dyld_image_containing },
    { "dyld_prog_image_header",      br_dyld_prog_image_header },
    { "dyld_program_sdk_version",    br_dyld_program_sdk_version },
    { "dyld_program_min_os_version", br_dyld_program_min_os_version },
    { "dyld_sdk_version",            br_dyld_sdk_version },
    { "dyld_min_os_version",         br_dyld_min_os_version },
    { "dyld_active_platform",        br_dyld_active_platform },
    { "dyld_program_sdk_at_least",   br_dyld_program_sdk_at_least },
    { "dyld_program_minos_at_least", br_dyld_program_minos_at_least },
    { "dyld_sdk_at_least",           br_dyld_sdk_at_least },
    { "dyld_minos_at_least",         br_dyld_minos_at_least },
    { "dyld_is_memory_immutable",    br_dyld_is_memory_immutable },
    { "dyld_cache_some_image_overridden", br_dyld_cache_some_image_overridden },
    { "dyld_cache_contains_path",    br_dyld_cache_contains_path },
    { "objc_msgSend",              ocerz_objc_msgSend },
    { "objc_msgSendSuper",         ocerz_objc_msgSendSuper },
    { "objc_msgSendSuper2",        ocerz_objc_msgSendSuper2 },
    { "objc_msgSend_stret",        ocerz_objc_msgSend_stret },
    { "objc_msgSendSuper_stret",   ocerz_objc_msgSendSuper_stret },
    { "objc_msgSendSuper2_stret",  ocerz_objc_msgSendSuper2_stret },
    { "objc_msgSend_fpret",        ocerz_objc_msgSend_fpret },
    { "objc_msgSend_fp2ret",       ocerz_objc_msgSend_fp2ret },
    { "objc_setUncaughtExceptionHandler", ocerz_objc_setUncaughtExceptionHandler },
    { "objc_allocateClassPair",     ocerz_objc_allocateClassPair },
    { "class_addMethod",            ocerz_objc_class_addMethod },
    { "method_setImplementation",   ocerz_objc_methodSetImplementation },
    { "class_replaceMethod",        ocerz_objc_class_replaceMethod },
    { "objc_setExceptionPreprocessor", ocerz_objc_setExceptionPreprocessor },
    { "method_getImplementation",   ocerz_objc_method_getImplementation },
    { "class_getMethodImplementation", ocerz_objc_class_getMethodImplementation },
    { "NSLog",                     ocerz_fmt_NSLog },
    { "printf",                    ocerz_fmt_printf },
    { "fprintf",                   ocerz_fmt_fprintf },
    { "sprintf",                   ocerz_fmt_sprintf },
    { "snprintf",                  ocerz_fmt_snprintf },
    { "snprintf_l",                ocerz_fmt_snprintf_l },
    { "asprintf",                  ocerz_fmt_asprintf },
    { "dprintf",                   ocerz_fmt_dprintf },
    { "swprintf", ocerz_fmt_swprintf },
    { "wprintf", ocerz_fmt_wprintf },
    { "fwprintf", ocerz_fmt_fwprintf },
    { "vswprintf", ocerz_fmt_vswprintf },
    { "vwprintf", ocerz_fmt_vwprintf },
    { "vfwprintf", ocerz_fmt_vfwprintf },
    { "syslog", ocerz_fmt_syslog },
    { "vsyslog", ocerz_fmt_vsyslog },
    { "warn", ocerz_fmt_warn },
    { "warnx", ocerz_fmt_warnx },
    { "vwarn", ocerz_fmt_vwarn },
    { "vwarnx", ocerz_fmt_vwarnx },
    { "sprintf_chk",               ocerz_fmt_sprintf_chk },
    { "snprintf_chk",              ocerz_fmt_snprintf_chk },
    { "vprintf",                   ocerz_fmt_vprintf },
    { "vfprintf",                  ocerz_fmt_vfprintf },
    { "vsprintf",                  ocerz_fmt_vsprintf },
    { "vsnprintf",                 ocerz_fmt_vsnprintf },
    { "vsnprintf_l",               ocerz_fmt_vsnprintf_l },
    { "vasprintf",                 ocerz_fmt_vasprintf },
    { "vdprintf",                  ocerz_fmt_vdprintf },
    { "vsprintf_chk",              ocerz_fmt_vsprintf_chk },
    { "vsnprintf_chk",             ocerz_fmt_vsnprintf_chk },
    { "sscanf",                    ocerz_fmt_sscanf },
    { "scanf",                     ocerz_fmt_scanf },
    { "fscanf",                    ocerz_fmt_fscanf },
    { "vsscanf",                   ocerz_fmt_vsscanf },
    { "vscanf",                    ocerz_fmt_vscanf },
    { "vfscanf",                   ocerz_fmt_vfscanf },
    { "CFStringCreateWithFormat",  ocerz_fmt_CFStringCreateWithFormat },
    { "CFStringAppendFormat",      ocerz_fmt_CFStringAppendFormat },
    { "open",            ocerz_sys_open },
    { "open_nocancel",   ocerz_sys_open_nocancel },
    { "openat",          ocerz_sys_openat },
    { "openat_nocancel", ocerz_sys_openat_nocancel },
    { "open_dprotected_np",   ocerz_sys_open_dprotected_np },
    { "openat_dprotected_np", ocerz_sys_openat_dprotected_np },
    { "fcntl",           ocerz_sys_fcntl },
    { "fcntl_nocancel",  ocerz_sys_fcntl_nocancel },
    { "ioctl",           ocerz_sys_ioctl },
    { "sem_open",        ocerz_sys_sem_open },
    { "shm_open",        ocerz_sys_shm_open },
    { "semctl",          ocerz_sys_semctl },
    { "ulimit",          ocerz_sys_ulimit },
    { "mmap",            ocerz_sys_mmap },
    { "munmap",          ocerz_sys_munmap },
    { "mprotect",        ocerz_sys_mprotect },
    { "madvise",         ocerz_sys_madvise },
    { "vm_allocate",     ocerz_sys_vm_allocate },
    { "vm_deallocate",   ocerz_sys_vm_deallocate },
    { "vm_protect",      ocerz_sys_vm_protect },
    { "setjmp",          ocerz_sys_setjmp },
    { "_setjmp",         ocerz_sys__setjmp },
    { "sigsetjmp",       ocerz_sys_sigsetjmp },
    { "longjmp",         ocerz_sys_longjmp },
    { "_longjmp",        ocerz_sys__longjmp },
    { "siglongjmp",      ocerz_sys_siglongjmp },
    { "fork",            ocerz_sys_fork },
    { "execve",          ocerz_sys_execve },
    { "execv",           ocerz_sys_execv },
    { "execvp",          ocerz_sys_execvp },
    { "execvP",          ocerz_sys_execvP },
    { "execl",           ocerz_sys_execl },
    { "execle",          ocerz_sys_execle },
    { "execlp",          ocerz_sys_execlp },
    { "posix_spawn",     ocerz_sys_posix_spawn },
    { "posix_spawnp",    ocerz_sys_posix_spawnp },
    { "system",          ocerz_sys_system },
    { "popen",           ocerz_sys_popen },
    { "pclose",          ocerz_sys_pclose },
    { "pthread_key_create",  ocerz_sys_pthread_key_create },
    { "pthread_key_delete",  ocerz_sys_pthread_key_delete },
    { "pthread_setspecific", ocerz_sys_pthread_setspecific },
    { "pthread_getspecific", ocerz_sys_pthread_getspecific },
    { "pthread_create",      ocerz_sys_pthread_create },
    { "pthread_exit",        ocerz_sys_pthread_exit },
    { "Block_copy",                ocerz_block_special_copy },
    { "Block_object_assign",       ocerz_block_special_object_assign },
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

static int br_bind_inplace(const OcerzApiEntry *e, struct OcerzBridgeFn *fn)
{
    for (int k = 0; k < e->ninplace && k < OCERZ_APIDB_INPLACE; k++) {
        BrInplaceBinding *b = &fn->inplace[k];
        b->reg = br_int_register(&fn->parsed, e->inplace[k].argpos);
        b->offset = e->inplace[k].offset;
        b->sig = e->inplace[k].sig;
        if (b->reg < 0) {
            OCERZ_LOG("bridge: %s converts a function in argument %d in place, which its signature %s"
                      " does not place in a register\n", fn->sym, e->inplace[k].argpos, fn->sig);
            return 0;
        }
        fn->ninplace = k + 1;
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
        if (!br_bind_structs(api, e, fn) || !br_bind_inplace(e, fn)) {
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

static _Atomic uint64_t g_br_levels;

static uint64_t br_new_level(void)
{
    return atomic_fetch_add(&g_br_levels, 1) + 1;
}

void ocerz_bridge_guest_enter(struct OcerzBridgeFrame *saved)
{
    struct OcerzBridgeFrame *frame = &g_br_frame;
    *saved = *frame;
    memset(frame, 0, sizeof *frame);
    frame->around = saved;
    frame->level = br_new_level();
}

uint64_t ocerz_bridge_level(void)
{
    struct OcerzBridgeFrame *frame = &g_br_frame;
    if (frame->level == 0)
        frame->level = br_new_level();
    return frame->level;
}

const struct OcerzBridgeFrame *ocerz_bridge_callback_frame(void)
{
    return g_br_frame.around;
}

void ocerz_bridge_postfork_child(void)
{
    pthread_mutex_t fresh = PTHREAD_MUTEX_INITIALIZER;
    g_br_libs_lock = fresh;
    g_br_fn_lock = fresh;
    g_br_host_lock = fresh;
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

const int *ocerz_bridge_depth_ptr(void)
{
    return &g_br_frame.depth;
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

typedef struct BrInplaceSwap {
    uint64_t at;
    uint64_t guest;
    uint64_t native;
} BrInplaceSwap;

static int br_convert_inplace(const struct OcerzBridgeFn *fn, OcerzCPU *cpu,
                              BrInplaceSwap swaps[OCERZ_APIDB_INPLACE])
{
    int n = 0;
    for (int k = 0; k < fn->ninplace; k++) {
        const BrInplaceBinding *b = &fn->inplace[k];
        uint64_t base = cpu->gpr[b->reg];
        if (!base)
            continue;
        uint64_t at = base + b->offset;
        uint64_t guest = ocerz_ld(at, 8);
        uint64_t native = guest;
        if (!guest || !ocerz_abi_is_guest_code(guest))
            continue;
        if (ocerz_abi_callback_convert(guest, b->sig, &native) != OCERZ_OK) {
            fprintf(stderr, "ocerz: bridge: %s could not bind guest function %#llx, %u bytes into its"
                    " argument\n", fn->sym, (unsigned long long)guest, b->offset);
            exit(OCERZ_BRIDGE_UNIMPL_EXIT);
        }
        ocerz_st(at, 8, native);
        swaps[n].at = at;
        swaps[n].guest = guest;
        swaps[n].native = native;
        n++;
    }
    return n;
}

__attribute__((noinline))
static int br_cross_structs(const struct OcerzBridgeFn *fn, OcerzCPU *cpu)
{
    struct OcerzBridgeFrame outer;
    uint64_t copies[OCERZ_APIDB_STRUCT_ARGS][OCERZ_APIDB_SHAPE_WORDS];
    BrInplaceSwap swaps[OCERZ_APIDB_INPLACE];

    ocerz_bridge_raise(&outer, fn->lib, fn->sym, fn->sig, fn->addr);
    br_convert_structs(fn, cpu, copies);
    int nswaps = br_convert_inplace(fn, cpu, swaps);
    int rc = ocerz_abi_perform(&fn->parsed, fn->addr, cpu);
    for (int k = 0; k < nswaps; k++)
        if (ocerz_ld(swaps[k].at, 8) == swaps[k].native)
            ocerz_st(swaps[k].at, 8, swaps[k].guest);
    ocerz_bridge_lower(&outer);
    return rc;
}

static int br_cross(const struct OcerzBridgeFn *fn, OcerzCPU *cpu)
{
    if (fn->nstructs || fn->ninplace)
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

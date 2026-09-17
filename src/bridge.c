/*
 * The bridges themselves: what a virtual library's export actually does.
 *
 * A descriptor here is a name, a host symbol and a signature; src/abi.c owns
 * everything the signature then implies.  The guest arrives in the middle of a
 * System V call with the return address on the stack, the engine reads the
 * arguments that signature describes out of wherever x86-64 left them, calls
 * the real arm64 function linked into ocerz itself, and puts the result back
 * where x86 code looks for it.  What is left here is the table saying which
 * export is which host function under which signature, the three exports that
 * are not a call at all, and the count of crossings.  The counting happens
 * before that split, so _exit and its kind appear in the report like anything
 * else.
 *
 * ---- the signature is the declared one, not the convenient one ----
 * Every notation below is read off the function's declaration in the SDK,
 * because the engine acts on the distinctions that declaration makes: a 32-bit
 * argument is re-extended on the way across and a 32-bit result on the way
 * back, so writing L where the header says int is not a harmless rounding of
 * the truth.  The table this replaced carried a three-class shape - void,
 * integer, pointer - which could not tell an int from a long and passed every
 * integer on as the full 64 bits it found, survivable only for as long as the
 * guest happened to leave the upper half clean.
 *
 * ---- what is deliberately absent ----
 * Variadic functions.  Apple's arm64 ABI passes variadic arguments on the stack
 * while x86-64 passes them in registers, and a signature has nowhere to say
 * where a function's fixed arguments stop, so a bridged printf, open, fcntl or
 * ioctl would be quietly wrong rather than refused.  They stay out of the table
 * and fall back to naming themselves.  A structure passed or returned by value
 * needs no rule here at all: the parser refuses the notation for one.
 *
 * ---- functions that call back ----
 * qsort and bsearch take a comparator, which the guest supplies as x86 code.
 * Their signatures name that argument with class c and the comparator's own
 * signature in braces, and the engine interns the guest function to a native
 * trampoline before the call, so this table needs nothing beyond the notation.
 * While the comparator runs the thread is executing guest code again, not native
 * code, so ocerz_bridge_guest_enter clears the frame for that stretch and
 * ocerz_bridge_guest_leave restores it: a fault inside the comparator is the
 * guest's, handled the ordinary way, and a strcmp the comparator makes raises
 * and lowers a frame of its own inside it.
 *
 * ---- callbacks on threads the guest never created ----
 * libdispatch's function-pointer entry points are bridged: dispatch_async_f,
 * dispatch_sync_f and dispatch_apply_f, with the global queues, the semaphores
 * and dispatch_release that a plain C program needs around them.  They are the
 * first functions in this table that call guest code on threads the guest never
 * created, and that is why they are here.  qsort runs its comparator on the
 * thread that called qsort, a thread that already has a guest cpu to run it on.
 * dispatch_async_f returns at once, and its work function runs later on one of
 * libdispatch's own workers, a thread with no x86 register state, no guest stack
 * and no bridge frame, after the crossing that handed the function over has
 * ended.  dispatch_apply_f spreads its iterations over those workers and the
 * calling thread together.  dispatch_sync_f normally runs its work on the
 * calling thread, an optimization the header describes rather than promises,
 * and runs it on the main thread instead when the queue is the main queue or
 * targets it.  So these exports are the proof that a native framework's own
 * threads can run guest callbacks, and their rows differ from qsort's in nothing
 * but the notation: giving such a thread a guest cpu is the engine's business,
 * not this table's.  A work function held past the call that received it is
 * exactly the case for which an interned slot is never given back, and because
 * one function under one notation interns to one slot, a program that submits
 * the same work a million times uses one slot, not a million.
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
 * __snprintf_chk, stay out for the same reason printf does.
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
 * An export with no entry, one whose host symbol dlsym cannot find, and one
 * whose signature does not parse are the same thing to the caller: no
 * descriptor, and the old behaviour of naming the export and stopping.  A
 * refusal is a bug report, while a crossing made through a signature nobody
 * could read would be a wrong answer, so the two failures are not allowed to
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
 * write the second half.  The three exports that are not a call raise nothing,
 * _exit above all: a frame raised around a function that never returns would
 * stay raised for the rest of the process.  A fault recovered by jumping out of
 * a crossing instead of returning through it is the one exit the pair cannot
 * see, and nothing takes it.  The crash handler stops the process on a fault in
 * native code rather than jumping out, and a guest fault inside a callback does
 * not jump past the crossing either: the guest call installs its own recovery
 * point, so the recovery lands inside the callback, below the saved frame, and
 * the frame is put back when the callback returns.
 *
 * The frame is read from inside a signal handler, which may not allocate and
 * may not take a lock, so it copies nothing: every string in it is a literal
 * out of the table below and the address is the descriptor's own, all of static
 * lifetime and all printable from a handler exactly as they are found.
 * OCERZ_BRIDGELOG prints those same three names once per crossing, from a
 * variable read once into a static so the hot path pays a predictable branch
 * and never a getenv.  It prints no argument values: the signature already says
 * what shape they were, and most of them are pointers into guest memory that a
 * log line has no business dereferencing.
 *
 * ---- thread-local variables ----
 * __tlv_bootstrap is not a function any host library exports; it is the thunk
 * a guest's thread-local variable descriptors are bound to, and it has to return
 * the variable's address while preserving every register but RAX.  A trap into
 * ocerz preserves them all by construction, so it is a special entry that asks
 * ocerz_tlv_address for this thread's copy and returns that in RAX.  Its stub,
 * unlike every other, saved the guest's r11 on the stack before trapping, since
 * clang keeps live values in r11 across a thread-local access, so the entry
 * restores r11 from there before returning.  It raises no bridge frame, because
 * nothing it runs is native framework code.
 *
 * ---- signals ----
 * A guest's signal handlers are x86 code, and a native sigaction cannot be
 * handed one: the host kernel would jump into guest bytes.  So sigaction,
 * signal, sigprocmask, pthread_sigmask, sigaltstack, raise, kill and
 * pthread_kill are special entries that go to the same guest signal table,
 * masks and alternate stacks the syscall path keeps, through the entry points
 * src/syscall.c exports for exactly this, with a trampoline in guest memory
 * standing in for the _sigtramp an x86 libc would have supplied.  Those entry
 * points answer 0 or an errno, and the entries here turn that into the shape the
 * guest's declaration promises: -1 with errno set for the POSIX calls, the errno
 * itself for pthread_sigmask and pthread_kill, SIG_ERR for signal.  errno is the
 * host's own, since ___error is bridged to the host's __error.  kill aimed at
 * this process is raise, so that it runs its handler on the calling thread
 * before returning; aimed anywhere else it is the host's kill.  The sigset
 * helpers are plain crossings, since a sigset_t is the same 32 bits on both
 * sides.
 *
 * A handler runs on the state after the call that raised it, so no entry builds
 * a signal frame itself.  Each finishes its call first - result in RAX, return
 * address popped - and only then asks whether anything is pending and unmasked,
 * and delivers it on top of that finished state.  Every ordinary crossing asks
 * the same question on its way out, and that is the delivery policy for native
 * mode: a signal that arrives while a thread is inside native code, or one sent
 * from another thread, reaches its handler when that thread's crossing returns,
 * the way a cache-mode thread takes it at its next syscall.  The question is two
 * loads and a branch, which a crossing already costing tens of nanoseconds does
 * not notice.
 *
 * ---- CoreFoundation ----
 * The native framework is opened the first time anything asks for one of its
 * symbols, which is while the guest is still being loaded, and that open is
 * when its initializers run: on a current macOS the framework is already mapped
 * into every process, but __CFInitialize has not run until something opens it.
 * Initializers have side effects on the host process, so the host's signal
 * dispositions are compared across that open and any it changed is named, and
 * the guest's environment is a copy main.c took before loading began, because
 * __CFInitialize calls setenv.
 *
 * The second virtual library is CoreFoundation, and its rows are ordinary
 * crossings resolved inside the native framework through
 * ocerz_bridge_host_symbol, never through the process-wide search order.  A
 * CFTypeRef needs no translation on the way across in either direction, and
 * that is a consequence of the address map rather than a shortcut: native mode
 * runs a position-independent guest in the identity map, so the native object a
 * CFStringCreateWithCString returns is an address the guest can hold, compare
 * and hand back, and the same object retrieved twice is the same pointer.  No
 * handle table stands between the two, because one would only have to
 * reproduce that identity at the price of a lookup on every call.  Boolean and
 * UniChar results use the engine's narrow classes, since arm64 extends them to
 * 32 bits where x86 leaves the upper bits of a narrow register undefined.
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
 * is.  Which argument holds such a structure, and what each word of each
 * version of it is, is data in the table above the rows rather than code per
 * function; a version the table does not describe names itself and stops,
 * because guessing at the layout of a context would put a guest pointer where
 * native code will call it.
 */
#include "ocerz/bridge.h"
#include "ocerz/abi.h"
#include "ocerz/vm.h"
#include "ocerz/dyld.h"
#include "ocerz/mem.h"
#include "ocerz/interp.h"
#include "ocerz/syscall.h"
#include "ocerz/vdylib.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#define BR_LIBSYSTEM OCERZ_BRIDGE_LIBSYSTEM
#define BR_CF OCERZ_BRIDGE_COREFOUNDATION

#define BR_STRUCT_WORDS 10
#define BR_STRUCT_ARGS 2

typedef struct BrShape {
    const char *name;
    int words;
    const char *fn[BR_STRUCT_WORDS];
} BrShape;

typedef struct BrStructArg {
    const char *lib;
    const char *sym;
    int argpos;
    const BrShape *by_version[2];
} BrStructArg;

struct OcerzBridgeFn {
    const char *lib;
    const char *sym;
    const char *host;
    const char *sig;
    int (*special)(struct OcerzVM *vm, OcerzCPU *cpu);
    void *addr;
    OcerzAbiSig parsed;
};

typedef struct BrStructBinding {
    const BrStructArg *arg[BR_STRUCT_ARGS];
    int reg[BR_STRUCT_ARGS];
} BrStructBinding;

static const BrShape br_shape_array_callbacks = {
    "CFArrayCallBacks", 5, { NULL, "p(pp)", "v(pp)", "p(p)", "B(pp)" }
};
static const BrShape br_shape_dictionary_key_callbacks = {
    "CFDictionaryKeyCallBacks", 6, { NULL, "p(pp)", "v(pp)", "p(p)", "B(pp)", "L(p)" }
};
static const BrShape br_shape_dictionary_value_callbacks = {
    "CFDictionaryValueCallBacks", 5, { NULL, "p(pp)", "v(pp)", "p(p)", "B(pp)" }
};
static const BrShape br_shape_timer_context = {
    "CFRunLoopTimerContext", 5, { NULL, NULL, "p(p)", "v(p)", "p(p)" }
};
static const BrShape br_shape_observer_context = {
    "CFRunLoopObserverContext", 5, { NULL, NULL, "p(p)", "v(p)", "p(p)" }
};
static const BrShape br_shape_source_context = {
    "CFRunLoopSourceContext", 10,
    { NULL, NULL, "p(p)", "v(p)", "p(p)", "B(pp)", "L(p)", "v(ppp)", "v(ppp)", "v(p)" }
};
static const BrShape br_shape_source_context1 = {
    "CFRunLoopSourceContext1", 9,
    { NULL, NULL, "p(p)", "v(p)", "p(p)", "B(pp)", "L(p)", "u(p)", "p(plpp)" }
};

static const BrStructArg g_br_struct_args[] = {
    { BR_CF, "_CFArrayCreate",             3, { &br_shape_array_callbacks, NULL } },
    { BR_CF, "_CFArrayCreateMutable",      2, { &br_shape_array_callbacks, NULL } },
    { BR_CF, "_CFDictionaryCreate",        4, { &br_shape_dictionary_key_callbacks, NULL } },
    { BR_CF, "_CFDictionaryCreate",        5, { &br_shape_dictionary_value_callbacks, NULL } },
    { BR_CF, "_CFDictionaryCreateMutable", 2, { &br_shape_dictionary_key_callbacks, NULL } },
    { BR_CF, "_CFDictionaryCreateMutable", 3, { &br_shape_dictionary_value_callbacks, NULL } },
    { BR_CF, "_CFRunLoopTimerCreate",      6, { &br_shape_timer_context, NULL } },
    { BR_CF, "_CFRunLoopObserverCreate",   5, { &br_shape_observer_context, NULL } },
    { BR_CF, "_CFRunLoopSourceCreate",     2, { &br_shape_source_context, &br_shape_source_context1 } },
};

static int br_exit(struct OcerzVM *vm, OcerzCPU *cpu)
{
    ocerz_vm_request_exit(vm, (int)(cpu->gpr[OCERZ_RDI] & 0xff));
    return OCERZ_STEP_EXIT;
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

typedef struct BrHostLib {
    const char *install_name;
    void *_Atomic handle;
} BrHostLib;

static BrHostLib g_br_host_libs[] = {
    { BR_LIBSYSTEM, NULL },
    { BR_CF, NULL },
};

static pthread_mutex_t g_br_host_lock = PTHREAD_MUTEX_INITIALIZER;

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
    if (!install_name)
        return NULL;
    BrHostLib *lib = NULL;
    for (size_t i = 0; i < sizeof g_br_host_libs / sizeof g_br_host_libs[0]; i++)
        if (strcmp(g_br_host_libs[i].install_name, install_name) == 0)
            lib = &g_br_host_libs[i];
    if (!lib)
        return NULL;
    void *h = lib->handle;
    if (h)
        return h;

    pthread_mutex_lock(&g_br_host_lock);
    h = lib->handle;
    if (!h) {
        if (strcmp(install_name, BR_LIBSYSTEM) == 0) {
            h = RTLD_DEFAULT;
        } else {
            static struct sigaction before[NSIG];
            for (int sig = 1; sig < NSIG; sig++)
                sigaction(sig, NULL, &before[sig]);
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

static struct OcerzBridgeFn g_br_fns[] = {
    { BR_LIBSYSTEM, "___bzero",  "bzero",   "v(pL)",  NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_memcpy",   "memcpy",  "p(ppL)", NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_memmove",  "memmove", "p(ppL)", NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_memset",   "memset",  "p(piL)", NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_memcmp",   "memcmp",  "i(ppL)", NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_memchr",   "memchr",  "p(piL)", NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_strlen",   "strlen",  "L(p)",   NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_strnlen",  "strnlen", "L(pL)",  NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_strcmp",   "strcmp",  "i(pp)",  NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_strncmp",  "strncmp", "i(ppL)", NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_strcpy",   "strcpy",  "p(pp)",  NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_strncpy",  "strncpy", "p(ppL)", NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_strcat",   "strcat",  "p(pp)",  NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_strchr",   "strchr",  "p(pi)",  NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_strrchr",  "strrchr", "p(pi)",  NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_strstr",   "strstr",  "p(pp)",  NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_strdup",   "strdup",  "p(p)",   NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },

    { BR_LIBSYSTEM, "_malloc",   "malloc",  "p(L)",   NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_calloc",   "calloc",  "p(LL)",  NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_realloc",  "realloc", "p(pL)",  NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_free",     "free",    "v(p)",   NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },

    { BR_LIBSYSTEM, "_write",    "write",   "l(ipL)", NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_read",     "read",    "l(ipL)", NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_close",    "close",   "i(i)",   NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_puts",     "puts",    "i(p)",   NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_putchar",  "putchar", "i(i)",   NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_getenv",   "getenv",  "p(p)",   NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_getpid",   "getpid",  "i()",    NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_isatty",   "isatty",  "i(i)",   NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_abs",      "abs",     "i(i)",   NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_labs",     "labs",    "l(l)",   NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_atoi",     "atoi",    "i(p)",   NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_atol",     "atol",    "l(p)",   NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_atof",     "atof",    "d(p)",   NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_strtod",   "strtod",  "d(pp)",  NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_qsort",    "qsort",   "v(pLLc{i(pp)})",  NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_bsearch",  "bsearch", "p(ppLLc{i(pp)})", NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_time",     "time",    "l(p)",   NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_clock",    "clock",   "L()",    NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "___error",  "__error", "p()",    NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },

    { BR_LIBSYSTEM, "_dispatch_get_global_queue", "dispatch_get_global_queue", "p(lL)",          NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_dispatch_async_f",          "dispatch_async_f",          "v(ppc{v(p)})",   NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_dispatch_sync_f",           "dispatch_sync_f",           "v(ppc{v(p)})",   NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_dispatch_apply_f",          "dispatch_apply_f",          "v(Lppc{v(pL)})", NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_dispatch_semaphore_create", "dispatch_semaphore_create", "p(l)",           NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_dispatch_semaphore_wait",   "dispatch_semaphore_wait",   "l(pL)",          NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_dispatch_semaphore_signal", "dispatch_semaphore_signal", "l(p)",           NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_dispatch_release",          "dispatch_release",          "v(p)",           NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "___memcpy_chk",              "__memcpy_chk",               "p(ppLL)",        NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "___memmove_chk",             "__memmove_chk",              "p(ppLL)",        NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "___memset_chk",              "__memset_chk",               "p(piLL)",        NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "___strcpy_chk",              "__strcpy_chk",               "p(ppL)",         NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "___stpcpy_chk",              "__stpcpy_chk",               "p(ppL)",         NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "___strcat_chk",              "__strcat_chk",               "p(ppL)",         NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "___strncpy_chk",             "__strncpy_chk",              "p(ppLL)",        NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "___stpncpy_chk",             "__stpncpy_chk",              "p(ppLL)",        NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "___strncat_chk",             "__strncat_chk",              "p(ppLL)",        NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "___strlcpy_chk",             "__strlcpy_chk",              "L(ppLL)",        NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "___strlcat_chk",             "__strlcat_chk",              "L(ppLL)",        NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_pthread_create",            "pthread_create",            "i(ppc{p(p)}p)",  NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_pthread_join",              "pthread_join",              "i(pp)",          NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_pthread_detach",            "pthread_detach",            "i(p)",           NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_pthread_self",              "pthread_self",              "p()",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_pthread_mutex_init",        "pthread_mutex_init",        "i(pp)",          NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_pthread_mutex_lock",        "pthread_mutex_lock",        "i(p)",           NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_pthread_mutex_unlock",      "pthread_mutex_unlock",      "i(p)",           NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_pthread_mutex_destroy",     "pthread_mutex_destroy",     "i(p)",           NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_pthread_cond_init",         "pthread_cond_init",         "i(pp)",          NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_pthread_cond_wait",         "pthread_cond_wait",         "i(pp)",          NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_pthread_cond_signal",       "pthread_cond_signal",       "i(p)",           NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_pthread_cond_broadcast",    "pthread_cond_broadcast",    "i(p)",           NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_pthread_cond_destroy",      "pthread_cond_destroy",      "i(p)",           NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_sigemptyset",               "sigemptyset",               "i(p)",           NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_sigfillset",                "sigfillset",                "i(p)",           NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_sigaddset",                 "sigaddset",                 "i(pi)",          NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_sigdelset",                 "sigdelset",                 "i(pi)",          NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_sigismember",               "sigismember",               "i(pi)",          NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },

    { BR_CF, "_CFRetain",                               "CFRetain",                               "p(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRelease",                              "CFRelease",                              "v(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFGetRetainCount",                       "CFGetRetainCount",                       "l(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFEqual",                                "CFEqual",                                "B(pp)",             NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFHash",                                 "CFHash",                                 "L(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFGetTypeID",                            "CFGetTypeID",                            "L(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringGetTypeID",                      "CFStringGetTypeID",                      "L()",               NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFArrayGetTypeID",                       "CFArrayGetTypeID",                       "L()",               NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFDictionaryGetTypeID",                  "CFDictionaryGetTypeID",                  "L()",               NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFNumberGetTypeID",                      "CFNumberGetTypeID",                      "L()",               NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFBooleanGetTypeID",                     "CFBooleanGetTypeID",                     "L()",               NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFDataGetTypeID",                        "CFDataGetTypeID",                        "L()",               NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFCopyDescription",                      "CFCopyDescription",                      "p(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFGetAllocator",                         "CFGetAllocator",                         "p(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringCreateWithCString",              "CFStringCreateWithCString",              "p(ppu)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringCreateWithBytes",                "CFStringCreateWithBytes",                "p(ppluB)",          NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringCreateCopy",                     "CFStringCreateCopy",                     "p(pp)",             NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringCreateMutable",                  "CFStringCreateMutable",                  "p(pl)",             NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringCreateMutableCopy",              "CFStringCreateMutableCopy",              "p(plp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringAppendCString",                  "CFStringAppendCString",                  "v(ppu)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringAppend",                         "CFStringAppend",                         "v(pp)",             NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringGetLength",                      "CFStringGetLength",                      "l(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringGetCharacterAtIndex",            "CFStringGetCharacterAtIndex",            "H(pl)",             NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringGetCString",                     "CFStringGetCString",                     "B(pplu)",           NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringGetCStringPtr",                  "CFStringGetCStringPtr",                  "p(pu)",             NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringGetMaximumSizeForEncoding",      "CFStringGetMaximumSizeForEncoding",      "l(lu)",             NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringCompare",                        "CFStringCompare",                        "l(ppL)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringHasPrefix",                      "CFStringHasPrefix",                      "B(pp)",             NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringHasSuffix",                      "CFStringHasSuffix",                      "B(pp)",             NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringGetIntValue",                    "CFStringGetIntValue",                    "i(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringGetDoubleValue",                 "CFStringGetDoubleValue",                 "d(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringCreateArrayBySeparatingStrings", "CFStringCreateArrayBySeparatingStrings", "p(ppp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFStringCreateByCombiningStrings",       "CFStringCreateByCombiningStrings",       "p(ppp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "___CFStringMakeConstantString",           "__CFStringMakeConstantString",           "p(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFArrayCreate",                          "CFArrayCreate",                          "p(pplp)",           NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFArrayCreateMutable",                   "CFArrayCreateMutable",                   "p(plp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFArrayCreateCopy",                      "CFArrayCreateCopy",                      "p(pp)",             NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFArrayCreateMutableCopy",               "CFArrayCreateMutableCopy",               "p(plp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFArrayGetCount",                        "CFArrayGetCount",                        "l(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFArrayGetValueAtIndex",                 "CFArrayGetValueAtIndex",                 "p(pl)",             NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFArrayAppendValue",                     "CFArrayAppendValue",                     "v(pp)",             NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFArrayInsertValueAtIndex",              "CFArrayInsertValueAtIndex",              "v(plp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFArraySetValueAtIndex",                 "CFArraySetValueAtIndex",                 "v(plp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFArrayRemoveValueAtIndex",              "CFArrayRemoveValueAtIndex",              "v(pl)",             NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFArrayRemoveAllValues",                 "CFArrayRemoveAllValues",                 "v(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFDictionaryCreate",                     "CFDictionaryCreate",                     "p(ppplpp)",         NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFDictionaryCreateMutable",              "CFDictionaryCreateMutable",              "p(plpp)",           NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFDictionaryCreateCopy",                 "CFDictionaryCreateCopy",                 "p(pp)",             NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFDictionaryCreateMutableCopy",          "CFDictionaryCreateMutableCopy",          "p(plp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFDictionaryGetCount",                   "CFDictionaryGetCount",                   "l(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFDictionaryGetValue",                   "CFDictionaryGetValue",                   "p(pp)",             NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFDictionaryGetValueIfPresent",          "CFDictionaryGetValueIfPresent",          "B(ppp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFDictionaryContainsKey",                "CFDictionaryContainsKey",                "B(pp)",             NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFDictionaryAddValue",                   "CFDictionaryAddValue",                   "v(ppp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFDictionarySetValue",                   "CFDictionarySetValue",                   "v(ppp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFDictionaryRemoveValue",                "CFDictionaryRemoveValue",                "v(pp)",             NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFDictionaryGetKeysAndValues",           "CFDictionaryGetKeysAndValues",           "v(ppp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFDictionaryApplyFunction",              "CFDictionaryApplyFunction",              "v(pc{v(ppp)}p)",    NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFNumberCreate",                         "CFNumberCreate",                         "p(plp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFNumberGetValue",                       "CFNumberGetValue",                       "B(plp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFNumberGetType",                        "CFNumberGetType",                        "l(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFNumberCompare",                        "CFNumberCompare",                        "l(ppp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFBooleanGetValue",                      "CFBooleanGetValue",                      "B(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFDataCreate",                           "CFDataCreate",                           "p(ppl)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFDataGetLength",                        "CFDataGetLength",                        "l(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFDataGetBytePtr",                       "CFDataGetBytePtr",                       "p(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFAbsoluteTimeGetCurrent",               "CFAbsoluteTimeGetCurrent",               "d()",               NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopGetCurrent",                    "CFRunLoopGetCurrent",                    "p()",               NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopGetMain",                       "CFRunLoopGetMain",                       "p()",               NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopRun",                           "CFRunLoopRun",                           "v()",               NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopRunInMode",                     "CFRunLoopRunInMode",                     "i(pdB)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopStop",                          "CFRunLoopStop",                          "v(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopWakeUp",                        "CFRunLoopWakeUp",                        "v(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopAddTimer",                      "CFRunLoopAddTimer",                      "v(ppp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopRemoveTimer",                   "CFRunLoopRemoveTimer",                   "v(ppp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopTimerCreate",                   "CFRunLoopTimerCreate",                   "p(pddLlc{v(pp)}p)", NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopTimerInvalidate",               "CFRunLoopTimerInvalidate",               "v(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopTimerIsValid",                  "CFRunLoopTimerIsValid",                  "B(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopTimerGetNextFireDate",          "CFRunLoopTimerGetNextFireDate",          "d(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopTimerSetNextFireDate",          "CFRunLoopTimerSetNextFireDate",          "v(pd)",             NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopObserverCreate",                "CFRunLoopObserverCreate",                "p(pLBlc{v(pLp)}p)", NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopAddObserver",                   "CFRunLoopAddObserver",                   "v(ppp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopRemoveObserver",                "CFRunLoopRemoveObserver",                "v(ppp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopObserverInvalidate",            "CFRunLoopObserverInvalidate",            "v(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopSourceCreate",                  "CFRunLoopSourceCreate",                  "p(plp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopAddSource",                     "CFRunLoopAddSource",                     "v(ppp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopRemoveSource",                  "CFRunLoopRemoveSource",                  "v(ppp)",            NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopSourceSignal",                  "CFRunLoopSourceSignal",                  "v(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_CF, "_CFRunLoopSourceInvalidate",              "CFRunLoopSourceInvalidate",              "v(p)",              NULL, NULL, { 0, { 0 }, { { 0 } }, 0 } },

    { BR_LIBSYSTEM, "_exit",             NULL, NULL, br_exit,            NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_abort",            NULL, NULL, br_abort,           NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "__tlv_bootstrap",   NULL, NULL, br_tlv_bootstrap,   NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "___stack_chk_fail", NULL, NULL, br_stack_chk_fail,  NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_sigaction",        NULL, NULL, br_sigaction,       NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_signal",           NULL, NULL, br_signal,          NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_sigprocmask",      NULL, NULL, br_sigprocmask,     NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_pthread_sigmask",  NULL, NULL, br_pthread_sigmask, NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_sigaltstack",      NULL, NULL, br_sigaltstack,     NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_raise",            NULL, NULL, br_raise,           NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_kill",             NULL, NULL, br_kill,            NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_pthread_kill",     NULL, NULL, br_pthread_kill,    NULL, { 0, { 0 }, { { 0 } }, 0 } },
};

#define BR_FNS ((int)(sizeof g_br_fns / sizeof g_br_fns[0]))

static uint64_t g_br_calls[sizeof g_br_fns / sizeof g_br_fns[0]];
static BrStructBinding g_br_structs[sizeof g_br_fns / sizeof g_br_fns[0]];

static int br_int_register(const OcerzAbiSig *sig, int argpos)
{
    static const int regs[6] = { OCERZ_RDI, OCERZ_RSI, OCERZ_RDX, OCERZ_RCX, OCERZ_R8, OCERZ_R9 };
    if (argpos < 0 || argpos >= sig->nargs || sig->arg[argpos] != 'p')
        return -1;
    int n = 0;
    for (int i = 0; i < argpos; i++)
        if (sig->arg[i] != 'f' && sig->arg[i] != 'd')
            n++;
    return n < 6 ? regs[n] : -1;
}

static int br_bind_struct_args(struct OcerzBridgeFn *fn)
{
    BrStructBinding *b = &g_br_structs[fn - g_br_fns];
    int k = 0;
    for (size_t i = 0; i < sizeof g_br_struct_args / sizeof g_br_struct_args[0]; i++) {
        const BrStructArg *a = &g_br_struct_args[i];
        if (strcmp(a->lib, fn->lib) != 0 || strcmp(a->sym, fn->sym) != 0)
            continue;
        int reg = br_int_register(&fn->parsed, a->argpos);
        if (k >= BR_STRUCT_ARGS || reg < 0) {
            OCERZ_LOG("bridge: %s converts a %s in argument %d, which its signature %s does not place"
                      " in a register\n", fn->sym, a->by_version[0]->name, a->argpos, fn->sig);
            return 0;
        }
        b->arg[k] = a;
        b->reg[k] = reg;
        k++;
    }
    return 1;
}

const struct OcerzBridgeFn *ocerz_bridge_lookup(const char *lib, const char *sym)
{
    if (!lib || !sym)
        return NULL;

    for (int i = 0; i < BR_FNS; i++) {
        struct OcerzBridgeFn *fn = &g_br_fns[i];
        if (strcmp(fn->lib, lib) != 0 || strcmp(fn->sym, sym) != 0)
            continue;
        if (fn->special)
            return fn;
        if (!fn->addr) {
            void *addr = ocerz_bridge_host_symbol(fn->lib, fn->host);
            if (!addr) {
                OCERZ_LOG("bridge: %s wants host %s, which does not resolve\n",
                          fn->sym, fn->host);
                return NULL;
            }
            if (ocerz_abi_parse(fn->sig, &fn->parsed) != OCERZ_OK) {
                OCERZ_LOG("bridge: %s is declared %s, which the abi engine will not take\n",
                          fn->sym, fn->sig ? fn->sig : "(nothing)");
                return NULL;
            }
            if (!br_bind_struct_args(fn))
                return NULL;
            fn->addr = addr;
        }
        return fn;
    }
    return NULL;
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

static int br_logging(void)
{
    static int en = -1;
    if (en < 0) en = getenv("OCERZ_BRIDGELOG") ? 1 : 0;
    return en;
}

static void br_convert_structs(const struct OcerzBridgeFn *fn, const BrStructBinding *b,
                               OcerzCPU *cpu, uint64_t copies[BR_STRUCT_ARGS][BR_STRUCT_WORDS])
{
    for (int k = 0; k < BR_STRUCT_ARGS && b->arg[k]; k++) {
        const BrStructArg *a = b->arg[k];
        uint64_t gptr = cpu->gpr[b->reg[k]];
        if (!gptr || !ocerz_abi_is_guest_code(gptr))
            continue;
        uint64_t version = ocerz_ld(gptr, 8);
        const BrShape *shape = version < 2 ? a->by_version[version] : NULL;
        if (!shape) {
            fprintf(stderr, "ocerz: bridge: %s was handed a %s of version %llu, which ocerz cannot convert\n",
                    fn->sym, a->by_version[0]->name, (unsigned long long)version);
            exit(OCERZ_BRIDGE_UNIMPL_EXIT);
        }
        for (int w = 0; w < shape->words; w++) {
            uint64_t v = ocerz_ld(gptr + 8 * (uint64_t)w, 8);
            if (shape->fn[w] && ocerz_abi_callback_convert(v, shape->fn[w], &v) != OCERZ_OK) {
                fprintf(stderr, "ocerz: bridge: %s could not bind guest function %#llx in word %d of its %s\n",
                        fn->sym, (unsigned long long)ocerz_ld(gptr + 8 * (uint64_t)w, 8), w, shape->name);
                exit(OCERZ_BRIDGE_UNIMPL_EXIT);
            }
            copies[k][w] = v;
        }
        cpu->gpr[b->reg[k]] = ocerz_h2g(copies[k]);
    }
}

static int br_cross(const struct OcerzBridgeFn *fn, OcerzCPU *cpu)
{
    struct OcerzBridgeFrame outer = g_br_frame;
    uint64_t copies[BR_STRUCT_ARGS][BR_STRUCT_WORDS];

    g_br_frame.lib = fn->lib;
    g_br_frame.sym = fn->sym;
    g_br_frame.sig = fn->sig;
    g_br_frame.host_fn = fn->addr;
    g_br_frame.depth = outer.depth + 1;

    const BrStructBinding *b = &g_br_structs[fn - g_br_fns];
    if (b->arg[0])
        br_convert_structs(fn, b, cpu, copies);
    int rc = ocerz_abi_perform(&fn->parsed, fn->addr, cpu);

    g_br_frame = outer;
    return rc;
}

int ocerz_bridge_invoke(struct OcerzVM *vm, OcerzCPU *cpu, const struct OcerzBridgeFn *fn)
{
    if (!fn)
        return OCERZ_STEP_FATAL;

    g_br_calls[fn - g_br_fns]++;

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

typedef struct BrRow {
    int idx;
    uint64_t n;
} BrRow;

static int br_row_cmp(const void *a, const void *b)
{
    uint64_t x = ((const BrRow *)a)->n, y = ((const BrRow *)b)->n;
    return x < y ? 1 : x > y ? -1 : 0;
}

void ocerz_bridge_report(void)
{
    BrRow rows[BR_FNS];
    uint64_t total = 0;
    int used = 0;

    for (int i = 0; i < BR_FNS; i++) {
        rows[i].idx = i;
        rows[i].n = g_br_calls[i];
        total += rows[i].n;
        if (rows[i].n)
            used++;
    }
    qsort(rows, (size_t)BR_FNS, sizeof rows[0], br_row_cmp);

    fprintf(stderr, "ocerz: BRIDGESTAT[%d] crossings=%llu over %d of %d bridged export(s)\n",
            (int)getpid(), (unsigned long long)total, used, BR_FNS);
    for (int i = 0; i < used; i++)
        fprintf(stderr, "ocerz: BRIDGESTAT[%d]   #%2d %-26s %14llu  %6.2f%%\n",
                (int)getpid(), i + 1, g_br_fns[rows[i].idx].sym,
                (unsigned long long)rows[i].n,
                total ? 100.0 * (double)rows[i].n / (double)total : 0.0);
}

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
 */
#include "ocerz/bridge.h"
#include "ocerz/abi.h"
#include "ocerz/vm.h"
#include "ocerz/interp.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>

#define BR_LIBSYSTEM "/usr/lib/libSystem.B.dylib"

struct OcerzBridgeFn {
    const char *lib;
    const char *sym;
    const char *host;
    const char *sig;
    int (*special)(struct OcerzVM *vm, OcerzCPU *cpu);
    void *addr;
    OcerzAbiSig parsed;
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

    { BR_LIBSYSTEM, "_exit",             NULL, NULL, br_exit,            NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "_abort",            NULL, NULL, br_abort,           NULL, { 0, { 0 }, { { 0 } }, 0 } },
    { BR_LIBSYSTEM, "___stack_chk_fail", NULL, NULL, br_stack_chk_fail,  NULL, { 0, { 0 }, { { 0 } }, 0 } },
};

#define BR_FNS ((int)(sizeof g_br_fns / sizeof g_br_fns[0]))

static uint64_t g_br_calls[sizeof g_br_fns / sizeof g_br_fns[0]];

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
            void *addr = dlsym(RTLD_DEFAULT, fn->host);
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

static int br_cross(const struct OcerzBridgeFn *fn, OcerzCPU *cpu)
{
    struct OcerzBridgeFrame outer = g_br_frame;

    g_br_frame.lib = fn->lib;
    g_br_frame.sym = fn->sym;
    g_br_frame.sig = fn->sig;
    g_br_frame.host_fn = fn->addr;
    g_br_frame.depth = outer.depth + 1;

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

    return br_cross(fn, cpu);
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

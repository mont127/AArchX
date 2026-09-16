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
 * and fall back to naming themselves.  So does anything taking a callback,
 * qsort and bsearch above all, because calling back into guest code needs a
 * trampoline that does not exist yet.  A structure passed or returned by value
 * needs no rule here at all: the parser refuses the notation for one.
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
    { BR_LIBSYSTEM, "___bzero",  "bzero",   "v(pL)",  NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_memcpy",   "memcpy",  "p(ppL)", NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_memmove",  "memmove", "p(ppL)", NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_memset",   "memset",  "p(piL)", NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_memcmp",   "memcmp",  "i(ppL)", NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_memchr",   "memchr",  "p(piL)", NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_strlen",   "strlen",  "L(p)",   NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_strnlen",  "strnlen", "L(pL)",  NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_strcmp",   "strcmp",  "i(pp)",  NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_strncmp",  "strncmp", "i(ppL)", NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_strcpy",   "strcpy",  "p(pp)",  NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_strncpy",  "strncpy", "p(ppL)", NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_strcat",   "strcat",  "p(pp)",  NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_strchr",   "strchr",  "p(pi)",  NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_strrchr",  "strrchr", "p(pi)",  NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_strstr",   "strstr",  "p(pp)",  NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_strdup",   "strdup",  "p(p)",   NULL, NULL, { 0, { 0 }, 0 } },

    { BR_LIBSYSTEM, "_malloc",   "malloc",  "p(L)",   NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_calloc",   "calloc",  "p(LL)",  NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_realloc",  "realloc", "p(pL)",  NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_free",     "free",    "v(p)",   NULL, NULL, { 0, { 0 }, 0 } },

    { BR_LIBSYSTEM, "_write",    "write",   "l(ipL)", NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_read",     "read",    "l(ipL)", NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_close",    "close",   "i(i)",   NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_puts",     "puts",    "i(p)",   NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_putchar",  "putchar", "i(i)",   NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_getenv",   "getenv",  "p(p)",   NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_getpid",   "getpid",  "i()",    NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_isatty",   "isatty",  "i(i)",   NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_abs",      "abs",     "i(i)",   NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_labs",     "labs",    "l(l)",   NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_atoi",     "atoi",    "i(p)",   NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_atol",     "atol",    "l(p)",   NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_atof",     "atof",    "d(p)",   NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_strtod",   "strtod",  "d(pp)",  NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_time",     "time",    "l(p)",   NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_clock",    "clock",   "L()",    NULL, NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "___error",  "__error", "p()",    NULL, NULL, { 0, { 0 }, 0 } },

    { BR_LIBSYSTEM, "_exit",             NULL, NULL, br_exit,            NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "_abort",            NULL, NULL, br_abort,           NULL, { 0, { 0 }, 0 } },
    { BR_LIBSYSTEM, "___stack_chk_fail", NULL, NULL, br_stack_chk_fail,  NULL, { 0, { 0 }, 0 } },
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

int ocerz_bridge_invoke(struct OcerzVM *vm, OcerzCPU *cpu, const struct OcerzBridgeFn *fn)
{
    if (!fn)
        return OCERZ_STEP_FATAL;

    g_br_calls[fn - g_br_fns]++;

    if (fn->special)
        return fn->special(vm, cpu);

    return ocerz_abi_perform(&fn->parsed, fn->addr, cpu);
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
        fprintf(stderr, "ocerz: BRIDGESTAT[%d]   #%2d %-20s %14llu  %6.2f%%\n",
                (int)getpid(), i + 1, g_br_fns[rows[i].idx].sym,
                (unsigned long long)rows[i].n,
                total ? 100.0 * (double)rows[i].n / (double)total : 0.0);
}

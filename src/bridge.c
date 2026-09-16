/*
 * The bridges themselves: what a virtual library's export actually does.
 *
 * A descriptor here is a shape, not a signature.  The guest arrives in the
 * middle of a System V call - arguments in rdi, rsi, rdx, rcx, r8, r9 and the
 * return address on the stack - and the shape says how many of those registers
 * are arguments, which of them are pointers, and what kind of thing comes back.
 * That is enough to move a call across, and deliberately not enough to move
 * every call across; see below for what is left out and why.
 *
 * ---- why there is a case per arity ----
 * The resolved address is called through a prototype of exactly the right
 * arity, every parameter uint64_t, so the switch on the argument count has one
 * arm per count from zero to six.  The tempting single prototype - one
 * uint64_t (*)(uint64_t, ...) used for everything - is wrong on this machine
 * rather than merely untidy: Apple's arm64 ABI passes variadic arguments on the
 * stack, so every argument after the first would be written where a normal
 * callee never looks, and the callee would read whatever happened to be in
 * x1..x5.  A single fixed six-argument prototype avoids that but is a type
 * mismatch against every function of another arity, which is undefined even
 * where it happens to work.  One prototype per arity is the only form that is
 * both correct and honest, and it costs one switch on a path that is already
 * crossing an emulation boundary.
 *
 * ---- what is deliberately absent ----
 * Variadic functions, for the reason above turned around: a bridged printf,
 * open, fcntl or ioctl would take its arguments from the x86 registers the
 * guest filled and hand them to an arm64 callee that expects them on the
 * stack.  Those need per-function veneers that know where the fixed arguments
 * stop, so they are not in the table and fall back to naming themselves.
 * Floating-point arguments and results are absent too: they live in a
 * different register bank on each side, and getting them right wants the real
 * System V classifier rather than a one-byte shape code - which is why atof,
 * strtod and their kind are missing.  So is anything taking a callback, qsort
 * and bsearch above all, because calling back into guest code needs a
 * trampoline that does not exist yet.
 *
 * ---- why null has to survive the conversion ----
 * ocerz_g2h is affine: it adds a base.  Applied to a null guest pointer it
 * produces the base of the arena, which is a plausible-looking address that is
 * not null, and free(NULL), time(NULL), a getenv that misses, a strstr that
 * does not match and a memchr that runs off the end all turn on the difference.
 * Both conversions therefore pass zero through untouched.  In native mode the
 * base is zero and every one of those cases works whether the check is there or
 * not, which is exactly why it is written down: the mode that hides the bug is
 * the mode this file was written for.
 *
 * An export with no descriptor, and one whose host symbol dlsym cannot find,
 * are the same thing to the caller: no descriptor, and the old behaviour of
 * naming the export and stopping.  Resolution happens in the lookup, which runs
 * once per export, so nothing on the crossing path touches dlsym.
 */
#include "ocerz/bridge.h"
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/interp.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>

#define BR_ARGS_MAX 6

enum {
    BR_VOID = 0,
    BR_INT = 1,
    BR_PTR = 2,
};

#define BR_LIBSYSTEM "/usr/lib/libSystem.B.dylib"

struct OcerzBridgeFn {
    const char *lib;
    const char *sym;
    const char *host;
    uint8_t res;
    uint8_t nargs;
    uint8_t arg[BR_ARGS_MAX];
    int (*special)(struct OcerzVM *vm, OcerzCPU *cpu);
    void *addr;
};

typedef union BrCall {
    void *addr;
    uint64_t (*a0)(void);
    uint64_t (*a1)(uint64_t);
    uint64_t (*a2)(uint64_t, uint64_t);
    uint64_t (*a3)(uint64_t, uint64_t, uint64_t);
    uint64_t (*a4)(uint64_t, uint64_t, uint64_t, uint64_t);
    uint64_t (*a5)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    uint64_t (*a6)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
} BrCall;

static const uint8_t br_arg_reg[BR_ARGS_MAX] = {
    OCERZ_RDI, OCERZ_RSI, OCERZ_RDX, OCERZ_RCX, OCERZ_R8, OCERZ_R9,
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
    { BR_LIBSYSTEM, "___bzero",  "bzero",   BR_VOID, 2, { BR_PTR, BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_memcpy",   "memcpy",  BR_PTR,  3, { BR_PTR, BR_PTR, BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_memmove",  "memmove", BR_PTR,  3, { BR_PTR, BR_PTR, BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_memset",   "memset",  BR_PTR,  3, { BR_PTR, BR_INT, BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_memcmp",   "memcmp",  BR_INT,  3, { BR_PTR, BR_PTR, BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_memchr",   "memchr",  BR_PTR,  3, { BR_PTR, BR_INT, BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_strlen",   "strlen",  BR_INT,  1, { BR_PTR }, NULL, NULL },
    { BR_LIBSYSTEM, "_strnlen",  "strnlen", BR_INT,  2, { BR_PTR, BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_strcmp",   "strcmp",  BR_INT,  2, { BR_PTR, BR_PTR }, NULL, NULL },
    { BR_LIBSYSTEM, "_strncmp",  "strncmp", BR_INT,  3, { BR_PTR, BR_PTR, BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_strcpy",   "strcpy",  BR_PTR,  2, { BR_PTR, BR_PTR }, NULL, NULL },
    { BR_LIBSYSTEM, "_strncpy",  "strncpy", BR_PTR,  3, { BR_PTR, BR_PTR, BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_strcat",   "strcat",  BR_PTR,  2, { BR_PTR, BR_PTR }, NULL, NULL },
    { BR_LIBSYSTEM, "_strchr",   "strchr",  BR_PTR,  2, { BR_PTR, BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_strrchr",  "strrchr", BR_PTR,  2, { BR_PTR, BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_strstr",   "strstr",  BR_PTR,  2, { BR_PTR, BR_PTR }, NULL, NULL },
    { BR_LIBSYSTEM, "_strdup",   "strdup",  BR_PTR,  1, { BR_PTR }, NULL, NULL },

    { BR_LIBSYSTEM, "_malloc",   "malloc",  BR_PTR,  1, { BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_calloc",   "calloc",  BR_PTR,  2, { BR_INT, BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_realloc",  "realloc", BR_PTR,  2, { BR_PTR, BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_free",     "free",    BR_VOID, 1, { BR_PTR }, NULL, NULL },

    { BR_LIBSYSTEM, "_write",    "write",   BR_INT,  3, { BR_INT, BR_PTR, BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_read",     "read",    BR_INT,  3, { BR_INT, BR_PTR, BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_close",    "close",   BR_INT,  1, { BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_puts",     "puts",    BR_INT,  1, { BR_PTR }, NULL, NULL },
    { BR_LIBSYSTEM, "_putchar",  "putchar", BR_INT,  1, { BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_getenv",   "getenv",  BR_PTR,  1, { BR_PTR }, NULL, NULL },
    { BR_LIBSYSTEM, "_getpid",   "getpid",  BR_INT,  0, { 0 }, NULL, NULL },
    { BR_LIBSYSTEM, "_isatty",   "isatty",  BR_INT,  1, { BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_abs",      "abs",     BR_INT,  1, { BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_labs",     "labs",    BR_INT,  1, { BR_INT }, NULL, NULL },
    { BR_LIBSYSTEM, "_atoi",     "atoi",    BR_INT,  1, { BR_PTR }, NULL, NULL },
    { BR_LIBSYSTEM, "_atol",     "atol",    BR_INT,  1, { BR_PTR }, NULL, NULL },
    { BR_LIBSYSTEM, "_time",     "time",    BR_INT,  1, { BR_PTR }, NULL, NULL },
    { BR_LIBSYSTEM, "_clock",    "clock",   BR_INT,  0, { 0 }, NULL, NULL },
    { BR_LIBSYSTEM, "___error",  "__error", BR_PTR,  0, { 0 }, NULL, NULL },

    { BR_LIBSYSTEM, "_exit",             NULL, BR_VOID, 0, { 0 }, br_exit, NULL },
    { BR_LIBSYSTEM, "_abort",            NULL, BR_VOID, 0, { 0 }, br_abort, NULL },
    { BR_LIBSYSTEM, "___stack_chk_fail", NULL, BR_VOID, 0, { 0 }, br_stack_chk_fail, NULL },
};

#define BR_FNS ((int)(sizeof g_br_fns / sizeof g_br_fns[0]))

static uint64_t g_br_calls[sizeof g_br_fns / sizeof g_br_fns[0]];

static uint64_t br_g2h(uint64_t gaddr)
{
    if (!gaddr)
        return 0;
    return (uint64_t)(uintptr_t)ocerz_g2h(gaddr);
}

static uint64_t br_h2g(uint64_t haddr)
{
    if (!haddr)
        return 0;
    return ocerz_h2g((const void *)(uintptr_t)haddr);
}

static void br_return(OcerzCPU *cpu, uint64_t result)
{
    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    cpu->rip = ocerz_ld(rsp, 8);
    cpu->gpr[OCERZ_RSP] = rsp + 8;
    cpu->gpr[OCERZ_RAX] = result;
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
            fn->addr = dlsym(RTLD_DEFAULT, fn->host);
            if (!fn->addr) {
                OCERZ_LOG("bridge: %s wants host %s, which does not resolve\n",
                          fn->sym, fn->host);
                return NULL;
            }
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

    uint64_t a[BR_ARGS_MAX] = { 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < fn->nargs; i++) {
        uint64_t raw = cpu->gpr[br_arg_reg[i]];
        a[i] = fn->arg[i] == BR_PTR ? br_g2h(raw) : raw;
    }

    BrCall call;
    call.addr = fn->addr;

    uint64_t r = 0;
    switch (fn->nargs) {
    case 0: r = call.a0(); break;
    case 1: r = call.a1(a[0]); break;
    case 2: r = call.a2(a[0], a[1]); break;
    case 3: r = call.a3(a[0], a[1], a[2]); break;
    case 4: r = call.a4(a[0], a[1], a[2], a[3]); break;
    case 5: r = call.a5(a[0], a[1], a[2], a[3], a[4]); break;
    case 6: r = call.a6(a[0], a[1], a[2], a[3], a[4], a[5]); break;
    default:
        OCERZ_FATAL("bridge: %s declares %u arguments, the limit is %d\n",
                    fn->sym, (unsigned)fn->nargs, BR_ARGS_MAX);
        return OCERZ_STEP_FATAL;
    }

    uint64_t result;
    switch (fn->res) {
    case BR_PTR:  result = br_h2g(r); break;
    case BR_VOID: result = 0; break;
    default:      result = r; break;
    }

    br_return(cpu, result);
    return OCERZ_STEP_OK;
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

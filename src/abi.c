/*
 * The signature-driven half of the crossing: guest registers in, host call
 * frame out, result back.
 *
 * ocerz/abi.h states the notation and why a signature is needed at all; what
 * follows is what this file knows that the header does not, and that the code
 * below cannot say for itself.
 *
 * ---- four counters, not one ----
 * Each side walks its arguments with a pair of independent counters, an integer
 * one and a floating-point one, and a third for what no longer fits.  So the
 * guest side of L(dpdpdp) takes its pointers from rdi, rsi, rdx and its doubles
 * from xmm0, xmm1, xmm2, while the host side takes the same pointers into x0,
 * x1, x2 and the same doubles into v0, v1, v2 - the argument's position tells
 * you nothing about where it lives on either side, and the two sides do not
 * even run out at the same point: the guest has six integer registers against
 * the host's eight, so a seventh integer argument is on the guest's stack and
 * still in a host register.  Everything here is a consequence of that.
 *
 * ---- the two stacks are not laid out alike ----
 * The x86-64 psABI rounds every stacked argument up to an eightbyte, so the
 * guest's overflow is a plain array of 8-byte slots starting above the return
 * address, and a stacked float sits in the low half of a full slot.  Apple's
 * arm64 deliberately drops that rule: a stacked argument consumes exactly its
 * own size at its own alignment, so two stacked ints occupy eight bytes
 * together and a stacked float after a stacked double lands at offset 8, not
 * 16.  clang bears this out on both sides - it emits `pushq $101; pushq $102`
 * for the x86 pair and `mov x8,#101; movk x8,#102,lsl #32; str x8,[sp]` for the
 * arm64 one.  OcerzAbiCall.stack is therefore filled as a byte buffer under its
 * uint64_t type; nstack is how many eightbytes of it the assembly must copy,
 * and keeping sp 16-byte aligned is the assembly's business, not this file's.
 *
 * ---- bit patterns, and who extends ----
 * A float or double is moved as the raw 64 bits of its slot, a float being the
 * low 32 of them, which is both where x86 leaves a float in an xmm and where
 * arm64 reads s0 out of v0; nothing is converted, so a signalling NaN or an
 * unnormal crosses unchanged.  Integers narrower than 64 bits are a different
 * matter: x86-64 leaves the upper bits of a 32-bit argument register
 * unspecified and Apple's arm64 makes the CALLER responsible for extending, so
 * a 32-bit class is taken from the low half of the guest's slot and re-extended
 * here, signed for i and unsigned for u, rather than passed on as it was found.
 *
 * ---- the rounding mode ----
 * The guest's MXCSR rounding control is mirrored into the host FPCR, by
 * ocerz_apply_mxcsr_round, so that emulated SSE rounds the way the guest asked.
 * A host libm compiled for the default mode inherits that FPCR when called, and
 * would quietly return a differently rounded result; so the crossing saves the
 * live mode, puts the host back to round-to-nearest for the duration, and
 * restores it afterwards.  Saving the live mode is the same as re-deriving it
 * from cpu->mxcsr, because that is where it came from, and it has the advantage
 * of surviving a callee that changes the mode - the guest resumes in the mode
 * it was running in either way.  A bridged fesetround, whose whole purpose is
 * to change the mode, would need its own handling and does not have it.
 *
 * ---- results and return codes ----
 * A result goes back where x86 looks for it: rax for the integer and pointer
 * classes, the low bits of xmm0 for f and d, with xmm0's upper bits cleared so
 * the value is deterministic rather than whatever the last emulated SSE op
 * left.  A v result zeroes rax, so a guest that wrongly reads a result gets a
 * stable zero instead of a host register's leftovers.  Pointers convert in both
 * directions with null passing through untouched, for the reasons src/bridge.c
 * sets out.  ocerz_abi_parse and ocerz_abi_read_guest report OCERZ_OK or a
 * negative OCERZ_E*, while ocerz_abi_perform is on the dispatch path and
 * reports an OCERZ_STEP_* the way ocerz_bridge_invoke does; the two agree that
 * success is zero.
 *
 * ---- a callback's address is its identity ----
 * An argument of class c is interned rather than converted: the guest function
 * and the notation in its braces are written into the next free entry of a
 * fixed table, and the entry's index picks the trampoline in the assembled bank
 * whose address the native callee receives.  The table is searched before it is
 * extended, so one function under one notation gets one address on every
 * crossing, and that is a requirement rather than an economy.  Native code is
 * entitled to compare callback pointers - to find a registered observer again
 * by its address, to refuse a second registration of one it already holds, to
 * remove exactly the function it was given - and every one of those breaks
 * quietly if each crossing mints a fresh address.  The same function under two
 * notations gets two addresses, because the two slots convert their arguments
 * differently and are, from the native side, two different functions.  The
 * notation is parsed once, when its entry is written, and one that does not
 * parse or that names a callback of its own gets no entry at all.
 *
 * ---- a slot is never given back ----
 * Nothing says when native code has let go of a function pointer.  qsort has by
 * the time it returns, but atexit, a framework's notification centre or a
 * container holding a comparator keeps the pointer for as long as it likes and
 * never announces that it is done.  Reusing a slot would therefore redirect a
 * native call still owed to one guest function into another, possibly under a
 * different signature, which is a wrong answer with nothing to point at it.  So
 * a full table is a named refusal, the crossing that wanted the slot does not
 * happen, and the message says how many slots there were so the number can be
 * judged rather than guessed.
 *
 * The table is extended under a mutex, because two guest threads can intern at
 * once and must neither take the same entry nor bind one function twice.  It is
 * read without one, because the dispatcher runs on every call native code makes,
 * on whatever thread native code makes it from.  That is safe because an entry
 * is written completely - function, notation, parsed signature - before its
 * used flag is set, the flag is set before the entry's address leaves the
 * table, and an entry is never written again; a dispatcher that sees the flag
 * sees everything behind it.
 *
 * ---- running the guest from inside native code ----
 * The dispatcher is the forward crossing turned round.  It reads the slot's
 * signature against the native caller's x and v registers and against the
 * caller's stack packed by Apple's rule above, with the same alignment
 * arithmetic the forward path uses to build such a stack, then places each value
 * where System V wants it, a stacked guest argument being a full eightbyte.  The
 * guest function runs on the calling thread's guest cpu, below its stack pointer
 * and past its 128-byte red zone, with the guest's MXCSR rounding reapplied for
 * the duration and the host's mode put back afterwards - the mirror image of
 * the forward crossing, which lends native code the default mode.  If the guest
 * has already asked to exit, nothing runs, and if it asks while the callback is
 * running, whatever the callback left is dropped: native code is only finishing
 * its loop, and a zero is as good an answer as any.
 *
 * While the guest runs, the thread's bridge frame is saved and cleared.  That
 * frame is what the crash handler reads to decide that a fault belongs to
 * native code, and for this stretch the thread is executing guest code again: a
 * comparator that follows a bad pointer is taking an ordinary guest fault, with
 * a recovery the guest call installs for itself, and a frame left raised would
 * have the handler blame qsort and stop a process that was fine.  Save and
 * restore bracket the guest call with nothing between them that can return, and
 * a recovered fault lands inside the guest call rather than past it, so the
 * restore always runs.
 *
 * ---- a thread with no guest cpu ----
 * Native frameworks start threads of their own - a queue's workers, an audio
 * render thread - and call function pointers they were handed earlier from
 * them.  Such a thread has no x86 register state and no guest stack to run on,
 * and borrowing another thread's cpu would overwrite the state of a guest
 * thread that is running.  Giving it a cpu of its own is attaching a thread,
 * which is a change of its own, so until then the call is refused by name.
 * Every refusal of the dispatcher's returns zero in both x0 and d0, so the
 * native caller at least reads a defined value, and every one is printed
 * whatever the verbosity: a malformed notation is a bug in a table fixed when
 * ocerz is built, while a callback refused at run time is a wrong answer handed
 * to native code that carries on regardless.
 */
#include "ocerz/abi.h"
#include "ocerz/bridge.h"
#include "ocerz/mem.h"
#include "ocerz/interp.h"
#include "ocerz/vm.h"

#include <fenv.h>
#include <pthread.h>

#define ABI_GUEST_INT_REGS 6
#define ABI_GUEST_FP_REGS 8
#define ABI_HOST_INT_REGS 8
#define ABI_HOST_FP_REGS 8

static const uint8_t abi_guest_int_reg[ABI_GUEST_INT_REGS] = {
    OCERZ_RDI, OCERZ_RSI, OCERZ_RDX, OCERZ_RCX, OCERZ_R8, OCERZ_R9,
};

static const char abi_scalar_classes[] = "iulLpfd";

static int abi_is_scalar_class(char c)
{
    return c != '\0' && strchr(abi_scalar_classes, c) != NULL;
}

static int abi_is_arg_class(char c)
{
    return c == 'c' || abi_is_scalar_class(c);
}

static int abi_is_ret_class(char c)
{
    return c == 'v' || abi_is_scalar_class(c);
}

static int abi_is_fp(char c)
{
    return c == 'f' || c == 'd';
}

static int abi_class_size(char c)
{
    return (c == 'i' || c == 'u' || c == 'f') ? 4 : 8;
}

static int abi_is_struct_class(char c)
{
    return c == 's' || c == 'S' || c == '{' || c == '}' || c == '[' || c == ']';
}

static void abi_reject(const char *notation, char c)
{
    if (abi_is_struct_class(c))
        OCERZ_LOG("abi: %s passes or returns a structure by value, which this engine does not do\n",
                  notation);
    else if (c == 'v')
        OCERZ_LOG("abi: %s uses v as an argument class, which is a result class only\n", notation);
    else if (c == 'c')
        OCERZ_LOG("abi: %s uses c as a result class, which is an argument class only\n", notation);
    else if (c)
        OCERZ_LOG("abi: %s names a class '%c' that does not exist\n", notation, c);
    else
        OCERZ_LOG("abi: %s ends where a class was expected\n", notation);
}

static int abi_parse_callback(const char *notation, int index, const char **cursor, char *out)
{
    const char *open = *cursor;

    if (*open != '{') {
        OCERZ_LOG("abi: %s gives argument %d class c with no signature in braces after it\n",
                  notation, index);
        return OCERZ_EFORMAT;
    }
    open++;

    const char *close = strchr(open, '}');
    if (!close) {
        OCERZ_LOG("abi: %s opens a callback signature for argument %d and never closes it\n",
                  notation, index);
        return OCERZ_EFORMAT;
    }

    size_t len = (size_t)(close - open);
    if (memchr(open, 'c', len)) {
        OCERZ_LOG("abi: %s gives callback argument %d a signature that takes a callback of its own\n",
                  notation, index);
        return OCERZ_EUNSUP;
    }
    if (len >= OCERZ_ABI_CB_MAX) {
        OCERZ_LOG("abi: %s gives callback argument %d a signature longer than the %d characters one may have\n",
                  notation, index, OCERZ_ABI_CB_MAX - 1);
        return OCERZ_ETOOLONG;
    }

    char nested[OCERZ_ABI_CB_MAX];
    memcpy(nested, open, len);
    nested[len] = '\0';

    OcerzAbiSig inner;
    int rc = ocerz_abi_parse(nested, &inner);
    if (rc != OCERZ_OK) {
        OCERZ_LOG("abi: %s gives callback argument %d the signature %s, which does not parse\n",
                  notation, index, nested);
        return rc;
    }

    memcpy(out, nested, len + 1);
    *cursor = close + 1;
    return OCERZ_OK;
}

int ocerz_abi_parse(const char *notation, OcerzAbiSig *out)
{
    if (!notation || !out)
        return OCERZ_EUNDEF;

    OcerzAbiSig sig;
    memset(&sig, 0, sizeof sig);

    const char *p = notation;
    char ret = *p;

    if (!abi_is_ret_class(ret)) {
        abi_reject(notation, ret);
        return OCERZ_EUNSUP;
    }
    sig.ret = ret;
    p++;

    if (*p != '(') {
        OCERZ_LOG("abi: %s has no argument list\n", notation);
        return OCERZ_EFORMAT;
    }
    p++;

    while (*p != '\0' && *p != ')') {
        char c = *p++;
        if (!abi_is_arg_class(c)) {
            abi_reject(notation, c);
            return OCERZ_EUNSUP;
        }
        if (sig.nargs >= OCERZ_ABI_MAX_ARGS) {
            OCERZ_LOG("abi: %s has more than the %d arguments this engine carries\n",
                      notation, OCERZ_ABI_MAX_ARGS);
            return OCERZ_ETOOLONG;
        }
        if (c == 'c') {
            int rc = abi_parse_callback(notation, sig.nargs, &p, sig.cb[sig.nargs]);
            if (rc != OCERZ_OK)
                return rc;
        }
        sig.arg[sig.nargs++] = c;
    }

    if (*p != ')') {
        OCERZ_LOG("abi: %s has no closing parenthesis\n", notation);
        return OCERZ_EFORMAT;
    }
    p++;

    if (*p != '\0') {
        OCERZ_LOG("abi: %s has %s after its argument list\n", notation, p);
        return OCERZ_EFORMAT;
    }

    *out = sig;
    return OCERZ_OK;
}

static uint64_t abi_narrow(char c, uint64_t raw)
{
    switch (c) {
    case 'i': return (uint64_t)(int64_t)(int32_t)raw;
    case 'u':
    case 'f': return (uint64_t)(uint32_t)raw;
    default:  return raw;
    }
}

static size_t abi_host_stack_at(size_t off, int size)
{
    size_t step = (size_t)size;
    return (off + step - 1) & ~(step - 1);
}

static int abi_push_host_stack(OcerzAbiCall *call, size_t *off, uint64_t val, int size)
{
    size_t step = (size_t)size;
    size_t at = abi_host_stack_at(*off, size);

    if (at + step > sizeof call->stack)
        return OCERZ_ETOOLONG;

    memcpy((unsigned char *)call->stack + at, &val, step);
    *off = at + step;
    return OCERZ_OK;
}

static uint64_t abi_pull_host_stack(const uint8_t *stack, size_t *off, int size)
{
    size_t step = (size_t)size;
    size_t at = abi_host_stack_at(*off, size);
    uint64_t val = 0;

    memcpy(&val, stack + at, step);
    *off = at + step;
    return val;
}

int ocerz_abi_read_guest(const OcerzAbiSig *sig, const OcerzCPU *cpu, OcerzAbiCall *call)
{
    if (!sig || !cpu || !call)
        return OCERZ_EUNDEF;
    if (sig->nargs < 0 || sig->nargs > OCERZ_ABI_MAX_ARGS)
        return OCERZ_ETOOLONG;
    if (!abi_is_ret_class(sig->ret)) {
        OCERZ_LOG("abi: result has class '%c', which no signature can name\n", sig->ret);
        return OCERZ_EUNSUP;
    }

    memset(call, 0, sizeof *call);

    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    int guest_int = 0, guest_fp = 0, guest_slot = 0;
    size_t host_off = 0;

    for (int i = 0; i < sig->nargs; i++) {
        char c = sig->arg[i];
        int fp = abi_is_fp(c);
        uint64_t raw;

        if (!abi_is_arg_class(c)) {
            OCERZ_LOG("abi: argument %d has class '%c', which no signature can name\n", i, c);
            return OCERZ_EUNSUP;
        }

        if (fp && guest_fp < ABI_GUEST_FP_REGS)
            raw = cpu->xmm[guest_fp++].lo;
        else if (!fp && guest_int < ABI_GUEST_INT_REGS)
            raw = cpu->gpr[abi_guest_int_reg[guest_int++]];
        else
            raw = ocerz_ld(rsp + 8 + 8 * (uint64_t)guest_slot++, 8);

        uint64_t val;
        if (c == 'p') {
            val = raw ? (uint64_t)(uintptr_t)ocerz_g2h(raw) : 0;
        } else if (c == 'c') {
            val = 0;
            if (raw) {
                void *tramp = ocerz_abi_callback_intern(raw, sig->cb[i]);
                if (!tramp) {
                    fprintf(stderr,
                            "ocerz: abi: argument %d is guest function %#llx, which could not be bound to"
                            " a callback trampoline, so the call is refused\n",
                            i, (unsigned long long)raw);
                    return OCERZ_EUNSUP;
                }
                val = (uint64_t)(uintptr_t)tramp;
            }
        } else {
            val = abi_narrow(c, raw);
        }

        if (fp && call->nv < ABI_HOST_FP_REGS)
            call->v[call->nv++] = val;
        else if (!fp && call->nx < ABI_HOST_INT_REGS)
            call->x[call->nx++] = val;
        else if (abi_push_host_stack(call, &host_off, val, abi_class_size(c)) != OCERZ_OK) {
            OCERZ_LOG("abi: argument %d spills past the %d-byte host argument window\n",
                      i, (int)sizeof call->stack);
            return OCERZ_ETOOLONG;
        }
    }

    call->nstack = (int)((host_off + 7) / 8);
    return OCERZ_OK;
}

void ocerz_abi_write_result(const OcerzAbiSig *sig, OcerzCPU *cpu, uint64_t rx, uint64_t rv)
{
    if (!sig || !cpu)
        return;

    switch (sig->ret) {
    case 'v':
        cpu->gpr[OCERZ_RAX] = 0;
        break;
    case 'p':
        cpu->gpr[OCERZ_RAX] = rx ? ocerz_h2g((const void *)(uintptr_t)rx) : 0;
        break;
    case 'f':
        cpu->xmm[0].lo = (uint64_t)(uint32_t)rv;
        cpu->xmm[0].hi = 0;
        break;
    case 'd':
        cpu->xmm[0].lo = rv;
        cpu->xmm[0].hi = 0;
        break;
    default:
        cpu->gpr[OCERZ_RAX] = abi_narrow(sig->ret, rx);
        break;
    }

    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    cpu->rip = ocerz_ld(rsp, 8);
    cpu->gpr[OCERZ_RSP] = rsp + 8;
}

int ocerz_abi_perform(const OcerzAbiSig *sig, const void *fn, OcerzCPU *cpu)
{
    if (!sig || !fn || !cpu) {
        OCERZ_FATAL("abi: a crossing with no signature, no address or no cpu\n");
        return OCERZ_STEP_FATAL;
    }

    OcerzAbiCall call;
    if (ocerz_abi_read_guest(sig, cpu, &call) != OCERZ_OK)
        return OCERZ_STEP_FATAL;

    uint64_t rx = 0, rv = 0;
    int guest_round = fegetround();

    fesetround(FE_TONEAREST);
    ocerz_abi_call_native(fn, call.x, call.v, call.stack,
                          (uint64_t)call.nstack * 8, &rx, &rv);
    fesetround(guest_round);

    ocerz_abi_write_result(sig, cpu, rx, rv);
    return OCERZ_STEP_OK;
}

typedef struct AbiCallback {
    uint64_t guest_fn;
    char notation[OCERZ_ABI_CB_MAX];
    OcerzAbiSig sig;
    _Atomic int used;
} AbiCallback;

static AbiCallback g_abi_cb[OCERZ_ABI_CALLBACK_SLOTS];
static unsigned g_abi_cb_n;
static pthread_mutex_t g_abi_cb_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned abi_callback_capacity(void)
{
    uintptr_t len = (uintptr_t)ocerz_abi_callback_bank_end - (uintptr_t)ocerz_abi_callback_bank;
    uintptr_t n = len / OCERZ_ABI_CALLBACK_STRIDE;
    return n < OCERZ_ABI_CALLBACK_SLOTS ? (unsigned)n : OCERZ_ABI_CALLBACK_SLOTS;
}

static void *abi_callback_address(unsigned slot)
{
    return (void *)(ocerz_abi_callback_bank + (size_t)slot * OCERZ_ABI_CALLBACK_STRIDE);
}

void *ocerz_abi_callback_intern(uint64_t guest_fn, const char *notation)
{
    if (!guest_fn || !notation) {
        OCERZ_LOG("abi: a callback with no guest function or no signature cannot be interned\n");
        return NULL;
    }

    size_t len = strnlen(notation, OCERZ_ABI_CB_MAX);
    if (len >= OCERZ_ABI_CB_MAX) {
        OCERZ_LOG("abi: callback %#llx has a signature longer than the %d characters one may have\n",
                  (unsigned long long)guest_fn, OCERZ_ABI_CB_MAX - 1);
        return NULL;
    }

    pthread_mutex_lock(&g_abi_cb_lock);

    for (unsigned n = 0; n < g_abi_cb_n; n++) {
        const AbiCallback *e = &g_abi_cb[n];
        if (e->guest_fn == guest_fn && strcmp(e->notation, notation) == 0) {
            pthread_mutex_unlock(&g_abi_cb_lock);
            return abi_callback_address(n);
        }
    }

    OcerzAbiSig sig;
    if (ocerz_abi_parse(notation, &sig) != OCERZ_OK) {
        pthread_mutex_unlock(&g_abi_cb_lock);
        OCERZ_LOG("abi: callback %#llx is declared %s, which does not parse\n",
                  (unsigned long long)guest_fn, notation);
        return NULL;
    }
    for (int i = 0; i < sig.nargs; i++) {
        if (sig.arg[i] == 'c') {
            pthread_mutex_unlock(&g_abi_cb_lock);
            OCERZ_LOG("abi: callback %#llx is declared %s, which takes a callback of its own\n",
                      (unsigned long long)guest_fn, notation);
            return NULL;
        }
    }

    unsigned cap = abi_callback_capacity();
    if (g_abi_cb_n >= cap) {
        pthread_mutex_unlock(&g_abi_cb_lock);
        fprintf(stderr,
                "ocerz: abi: the callback bank is exhausted: all %u slots are bound, so guest function"
                " %#llx declared %s gets none\n",
                cap, (unsigned long long)guest_fn, notation);
        return NULL;
    }

    unsigned slot = g_abi_cb_n;
    AbiCallback *e = &g_abi_cb[slot];
    e->guest_fn = guest_fn;
    memcpy(e->notation, notation, len + 1);
    e->sig = sig;
    e->used = 1;
    g_abi_cb_n = slot + 1;

    pthread_mutex_unlock(&g_abi_cb_lock);
    return abi_callback_address(slot);
}

void ocerz_abi_callback_dispatch(unsigned slot, const uint64_t *x, const uint64_t *v,
                                 const uint8_t *stack, uint64_t *out_x0, uint64_t *out_v0)
{
    if (out_x0)
        *out_x0 = 0;
    if (out_v0)
        *out_v0 = 0;
    if (!x || !v || !out_x0 || !out_v0) {
        fprintf(stderr, "ocerz: abi: callback slot %u was dispatched without its argument or result words\n",
                slot);
        return;
    }

    if (slot >= OCERZ_ABI_CALLBACK_SLOTS || !g_abi_cb[slot].used) {
        fprintf(stderr, "ocerz: abi: native code called callback slot %u, %s\n", slot,
                slot >= OCERZ_ABI_CALLBACK_SLOTS ? "which lies outside the bank"
                                                 : "to which no guest function was ever bound");
        return;
    }

    const AbiCallback *e = &g_abi_cb[slot];
    const OcerzAbiSig *sig = &e->sig;

    OcerzCPU *cpu = ocerz_vm_current_cpu();
    if (!cpu) {
        fprintf(stderr,
                "ocerz: abi: native code called guest function %#llx (callback slot %u, %s) on a thread"
                " with no guest cpu, one a native framework created for itself; attaching such a"
                " thread is not implemented\n",
                (unsigned long long)e->guest_fn, slot, e->notation);
        return;
    }

    OcerzVM *vm = cpu->vm;
    if (!vm || vm->exited)
        return;

    OcerzGuestCall call;
    memset(&call, 0, sizeof call);

    int nx = 0, nv = 0, gi = 0, gf = 0;
    size_t off = 0;

    for (int i = 0; i < sig->nargs; i++) {
        char c = sig->arg[i];
        int fp = abi_is_fp(c);
        uint64_t raw;

        if (fp && nv < ABI_HOST_FP_REGS) {
            raw = v[nv++];
        } else if (!fp && nx < ABI_HOST_INT_REGS) {
            raw = x[nx++];
        } else if (stack) {
            raw = abi_pull_host_stack(stack, &off, abi_class_size(c));
        } else {
            fprintf(stderr,
                    "ocerz: abi: guest function %#llx (callback slot %u, %s) takes argument %d from"
                    " the native caller's stack, and no stack was passed\n",
                    (unsigned long long)e->guest_fn, slot, e->notation, i);
            return;
        }

        uint64_t val = c == 'p' ? (raw ? ocerz_h2g((const void *)(uintptr_t)raw) : 0)
                                : abi_narrow(c, raw);

        if (fp && gf < ABI_GUEST_FP_REGS) {
            call.xmm[gf++] = val;
        } else if (!fp && gi < ABI_GUEST_INT_REGS) {
            call.gpr[gi++] = val;
        } else if (call.nstack < (int)(sizeof call.stack / sizeof call.stack[0])) {
            call.stack[call.nstack++] = val;
        } else {
            fprintf(stderr,
                    "ocerz: abi: guest function %#llx (callback slot %u, %s) stacks more than the %d"
                    " arguments a guest call carries\n",
                    (unsigned long long)e->guest_fn, slot, e->notation,
                    (int)(sizeof call.stack / sizeof call.stack[0]));
            return;
        }
    }

    uint64_t stack_top = (cpu->gpr[OCERZ_RSP] - 128) & ~0xfull;

    struct OcerzBridgeFrame saved;
    ocerz_bridge_guest_enter(&saved);
    int host_round = fegetround();
    ocerz_apply_mxcsr_round(cpu->mxcsr);
    int rc = ocerz_vm_call_abi(vm, e->guest_fn, &call, stack_top);
    fesetround(host_round);
    ocerz_bridge_guest_leave(&saved);

    if (rc != OCERZ_OK || vm->exited)
        return;

    switch (sig->ret) {
    case 'v':
        break;
    case 'p':
        *out_x0 = call.rax ? (uint64_t)(uintptr_t)ocerz_g2h(call.rax) : 0;
        break;
    case 'f':
        *out_v0 = (uint64_t)(uint32_t)call.xmm0;
        break;
    case 'd':
        *out_v0 = call.xmm0;
        break;
    default:
        *out_x0 = abi_narrow(sig->ret, call.rax);
        break;
    }
}

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
 */
#include "ocerz/abi.h"
#include "ocerz/mem.h"
#include "ocerz/interp.h"

#include <fenv.h>

#define ABI_GUEST_INT_REGS 6
#define ABI_GUEST_FP_REGS 8
#define ABI_HOST_INT_REGS 8
#define ABI_HOST_FP_REGS 8

static const uint8_t abi_guest_int_reg[ABI_GUEST_INT_REGS] = {
    OCERZ_RDI, OCERZ_RSI, OCERZ_RDX, OCERZ_RCX, OCERZ_R8, OCERZ_R9,
};

static const char abi_arg_classes[] = "iulLpfd";

static int abi_is_arg_class(char c)
{
    return c != '\0' && strchr(abi_arg_classes, c) != NULL;
}

static int abi_is_ret_class(char c)
{
    return c == 'v' || abi_is_arg_class(c);
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
    else if (c)
        OCERZ_LOG("abi: %s names a class '%c' that does not exist\n", notation, c);
    else
        OCERZ_LOG("abi: %s ends where a class was expected\n", notation);
}

int ocerz_abi_parse(const char *notation, OcerzAbiSig *out)
{
    if (!notation || !out)
        return OCERZ_EUNDEF;

    OcerzAbiSig sig;
    memset(&sig, 0, sizeof sig);

    const char *p = notation;
    char ret = *p;

    if (ret != 'v' && !abi_is_arg_class(ret)) {
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

static int abi_push_host_stack(OcerzAbiCall *call, size_t *off, uint64_t val, int size)
{
    size_t step = (size_t)size;
    size_t at = (*off + step - 1) & ~(step - 1);

    if (at + step > sizeof call->stack)
        return OCERZ_ETOOLONG;

    memcpy((unsigned char *)call->stack + at, &val, step);
    *off = at + step;
    return OCERZ_OK;
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

        uint64_t val = c == 'p' ? (raw ? (uint64_t)(uintptr_t)ocerz_g2h(raw) : 0)
                                : abi_narrow(c, raw);

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

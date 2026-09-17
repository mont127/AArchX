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
 * A structure moves the counters by more than one and moves them differently
 * on each side.  System V asks for all of a structure's eightbytes at once and,
 * when they are not all free, stacks the structure and leaves both counters
 * where they were.  Apple's arm64 does the same test against its own registers
 * but, on failure, sets the counter it failed on to eight, so every later
 * argument of that kind is stacked too.  The guest counters are therefore
 * passed by address into the structure helpers and only ever advanced there,
 * and the host counters live in OcerzAbiCall, where a spill writes the eight.
 *
 * ---- the two stacks are not laid out alike ----
 * The x86-64 psABI rounds every stacked argument up to an eightbyte, so the
 * guest's overflow is a plain array of 8-byte slots starting above the return
 * address, and a stacked float sits in the low half of a full slot.  Apple's
 * arm64 deliberately drops that rule: a stacked argument consumes exactly its
 * own size at its own alignment, so two stacked ints occupy eight bytes
 * together, a stacked float after a stacked double lands at offset 8, not 16,
 * and a char, a short and a char after eight integer arguments sit at 0, 2 and
 * 4.  clang bears this out on both sides - it emits `pushq $101; pushq $102`
 * for the x86 pair and `mov x8,#101; movk x8,#102,lsl #32; str x8,[sp]` for the
 * arm64 one, and strb, strh, strb for the narrow three, which the callee reads
 * back with ldrsb and ldrsh at the same offsets; ocerz/abi.h lists the layouts
 * that were checked.  Size and alignment are the same number for every scalar
 * class, so abi_host_stack_at takes only the one.  A stacked structure is a
 * block with its own two numbers: an aggregate of floats or doubles is its
 * bytes at its member's size, a small structure of any other kind is one or
 * two whole words at 8, and a large one is not stacked at all, only the pointer
 * to its copy.  OcerzAbiCall.stack is therefore filled as a byte buffer under
 * its uint64_t type; nstack is how many eightbytes of it the assembly must
 * copy, and keeping sp 16-byte aligned is the assembly's business, not this
 * file's.
 *
 * ---- bit patterns, and who extends ----
 * A float or double is moved as the raw 64 bits of its slot, a float being the
 * low 32 of them, which is both where x86 leaves a float in an xmm and where
 * arm64 reads s0 out of v0; nothing is converted, so a signalling NaN or an
 * unnormal crosses unchanged.  Integers narrower than 64 bits are a different
 * matter: x86-64 leaves the bits of an argument register above the argument's
 * width unspecified and Apple's arm64 makes the CALLER responsible for
 * extending, so a 32-, 16- or 8-bit class is taken from the low bits of the
 * guest's slot and re-extended here to 64, signed for i, h and b and unsigned
 * for u, H and B, rather than passed on as it was found.  abi_narrow is the one
 * place that extends, and every narrow value goes through it on every path: a
 * guest argument on its way to x or the host stack, a native callee's x0 on its
 * way to rax, a native caller's x register or stack bytes on their way to a
 * guest register or eightbyte, and a guest's rax on its way to x0.  Extending
 * only as far as the receiving ABI promises would do for a receiver that keeps
 * its promise; extending to 64 makes the value the same whichever width the
 * other side reads it at.
 *
 * ---- structures are bytes ----
 * A structure never passes through abi_narrow.  Each path gathers it into a
 * buffer laid out exactly as the structure, whole words at a time from
 * registers and stack eightbytes or bytes from Apple's packed stack, converts
 * its p members in place, and scatters the buffer again.  The buffer's tail past
 * the structure's size, up to the next eightbyte, is zeroed after every gather,
 * so a register word or stack eightbyte written from it holds zero above the
 * structure rather than whatever the other side left there.  Classification is
 * recomputed from the layout on every crossing, a loop over at most sixteen
 * members, rather than cached in the signature, so a hand-built signature
 * cannot carry a classification that disagrees with its own layout, and
 * abi_struct_valid is the gate that layout passes first.  Sixteen members
 * cannot reach 256 bytes: each member starts at most fifteen bytes past the
 * start of the one before, since it is at most eight bytes long and every
 * rounding in between is to a power of two no greater than eight, so a layout
 * ends by 240 and a byte fits every offset.  The copies an arm64 callee is
 * given for a structure over sixteen bytes are whole words of
 * OcerzAbiCall.mem, which holds sixteen copies of a 256-byte structure, one for
 * every argument a signature can have, so it cannot run out.
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
 * stable zero instead of a host register's leftovers.  A structure result is
 * gathered from x0 and x1, from v0 to v3, or from the x8 buffer inside
 * OcerzAbiCall, and scattered to rax, rdx, xmm0 and xmm1 by class, with an xmm's
 * upper half cleared the same way, or written through the result pointer the
 * guest passed in rdi, which is what rax then holds.  The guest's pointer is
 * never handed to native code as x8: the native callee writes into ocerz's own
 * buffer, and only the copy touches guest memory.  Pointers convert in both
 * directions with null passing through untouched, for the reasons src/bridge.c
 * sets out.  ocerz_abi_parse and ocerz_abi_read_guest report OCERZ_OK or a
 * negative OCERZ_E*, while ocerz_abi_perform is on the dispatch path and
 * reports an OCERZ_STEP_* the way ocerz_bridge_invoke does; the two agree that
 * success is zero.  ocerz_abi_read_guest zeroes OcerzAbiCall only up to the
 * result buffer, because the two buffers are most of its size and every byte
 * of them that is read has been written first.
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
 * parse or that names a callback of its own gets no entry at all.  Its braces
 * are matched by depth rather than by the first closing brace, since a
 * structure inside it closes its own.
 *
 * ---- a function pointer that is already native ----
 * ocerz_abi_is_guest_code asks the cheap questions first, because it runs on
 * every crossing that carries a callback.  The guest reservation is a range
 * compare and covers what ocerz maps for the guest, its images and stacks
 * among it, so the common case never leaves it.  The host shared cache is one
 * more range compare and covers CoreFoundation and everything else a guest
 * could have copied a native function pointer out of.  Only what is in neither
 * goes to dladdr, a lookup through dyld's image list; it answers correctly for
 * the cache as well, so the cache test changes nothing but the cost.  An
 * address dladdr does not know counts as guest code, since an image mapped by
 * ocerz's own loader is one dyld has never heard of, and erring that way leaves
 * such a pointer interned exactly as every c argument was before the question
 * was asked.  The trampoline bank is inside ocerz's image, so a slot address
 * comes back from the converter as itself and is never bound to a second slot.
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
 * A guest function returning a MEMORY structure is handed space for it carved
 * from the top of that same region, the stack top moving down past it before
 * the arguments are laid out below, so the space outlives the call and nothing
 * the call pushes can reach it.  The result is read back from that space rather
 * than through whatever the guest left in rax.  A native caller expecting its
 * result through x8 and passing none is refused before the guest runs, because
 * the result would have nowhere to go.
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
 * thread that is running.  So the dispatcher gives it a cpu of its own, through
 * ocerz_thread_attach, the first time such a thread calls a guest function, and
 * the same cpu on every call after; it is refused by name only when no
 * personality can be attached at all, which means there is no guest process to
 * attach it to.  Every refusal of the dispatcher's returns zero in x0, x1 and
 * d0 to d3 and writes nothing through x8, so the native caller at least reads a
 * defined value, and every one is printed whatever the verbosity: a malformed
 * notation is a bug in a table fixed when ocerz is built, while a callback
 * refused at run time is a wrong answer handed to native code that carries on
 * regardless.
 */
#include "ocerz/abi.h"
#include "ocerz/bridge.h"
#include "ocerz/mem.h"
#include "ocerz/interp.h"
#include "ocerz/vm.h"

#include <dlfcn.h>
#include <fenv.h>
#include <pthread.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define ABI_GUEST_INT_REGS 6
#define ABI_GUEST_FP_REGS 8
#define ABI_HOST_INT_REGS 8
#define ABI_HOST_FP_REGS 8
#define ABI_HOST_HFA_MAX 4
#define ABI_STRUCT_DEPTH 8
#define ABI_SMALL_STRUCT 16

static const uint8_t abi_guest_int_reg[ABI_GUEST_INT_REGS] = {
    OCERZ_RDI, OCERZ_RSI, OCERZ_RDX, OCERZ_RCX, OCERZ_R8, OCERZ_R9,
};

static const char abi_scalar_classes[] = "bBhHiulLpfd";

static int abi_is_scalar_class(char c)
{
    return c != '\0' && strchr(abi_scalar_classes, c) != NULL;
}

static int abi_is_arg_class(char c)
{
    return c == 'c' || c == '{' || abi_is_scalar_class(c);
}

static int abi_is_ret_class(char c)
{
    return c == 'v' || c == '{' || abi_is_scalar_class(c);
}

static int abi_is_fp(char c)
{
    return c == 'f' || c == 'd';
}

static int abi_class_size(char c)
{
    switch (c) {
    case 'b':
    case 'B': return 1;
    case 'h':
    case 'H': return 2;
    case 'i':
    case 'u':
    case 'f': return 4;
    default:  return 8;
    }
}

static size_t abi_align_up(size_t n, size_t align)
{
    return (n + align - 1) & ~(align - 1);
}

static void abi_reject(const char *notation, char c)
{
    if (c == '}')
        OCERZ_LOG("abi: %s closes a structure it never opened\n", notation);
    else if (c == 'v')
        OCERZ_LOG("abi: %s uses v as an argument class, which is a result class only\n", notation);
    else if (c == 'c')
        OCERZ_LOG("abi: %s uses c as a result class, which is an argument class only\n", notation);
    else if (c)
        OCERZ_LOG("abi: %s names a class '%c' that does not exist\n", notation, c);
    else
        OCERZ_LOG("abi: %s ends where a class was expected\n", notation);
}

static int abi_parse_struct(const char *notation, const char **cursor, int depth,
                            OcerzAbiStruct *st, size_t *size_out, size_t *align_out)
{
    const char *p = *cursor + 1;
    size_t off = 0, align = 1;
    int count = 0;

    if (depth > ABI_STRUCT_DEPTH) {
        OCERZ_LOG("abi: %s nests structures more than %d deep\n", notation, ABI_STRUCT_DEPTH);
        return OCERZ_EUNSUP;
    }

    while (*p != '}') {
        char c = *p;

        if (c == '{') {
            int first = st->nmember;
            size_t nsize = 0, nalign = 1;
            int rc = abi_parse_struct(notation, &p, depth + 1, st, &nsize, &nalign);
            if (rc != OCERZ_OK)
                return rc;
            off = abi_align_up(off, nalign);
            for (int k = first; k < st->nmember; k++)
                st->offset[k] = (uint8_t)(st->offset[k] + off);
            off += nsize;
            if (nalign > align)
                align = nalign;
        } else if (abi_is_scalar_class(c)) {
            size_t size = (size_t)abi_class_size(c);
            if (st->nmember >= OCERZ_ABI_STRUCT_MEMBERS) {
                OCERZ_LOG("abi: %s has a structure with more than the %d members one may have\n",
                          notation, OCERZ_ABI_STRUCT_MEMBERS);
                return OCERZ_ETOOLONG;
            }
            off = abi_align_up(off, size);
            st->member[st->nmember] = c;
            st->offset[st->nmember] = (uint8_t)off;
            st->nmember++;
            off += size;
            if (size > align)
                align = size;
            p++;
        } else if (c == 'v') {
            OCERZ_LOG("abi: %s puts v inside a structure, where only a result may be void\n", notation);
            return OCERZ_EUNSUP;
        } else if (c == 'c') {
            OCERZ_LOG("abi: %s puts a callback inside a structure, which this engine does not carry\n",
                      notation);
            return OCERZ_EUNSUP;
        } else if (c == '\0') {
            OCERZ_LOG("abi: %s opens a structure and never closes it\n", notation);
            return OCERZ_EFORMAT;
        } else {
            OCERZ_LOG("abi: %s has '%c' inside a structure, which is no member class\n", notation, c);
            return OCERZ_EFORMAT;
        }
        count++;
    }

    if (count == 0) {
        OCERZ_LOG("abi: %s has a structure with no members\n", notation);
        return OCERZ_EFORMAT;
    }

    *cursor = p + 1;
    *size_out = abi_align_up(off, align);
    *align_out = align;
    return OCERZ_OK;
}

static int abi_parse_layout(const char *notation, const char **cursor, OcerzAbiStruct *out)
{
    OcerzAbiStruct st;
    size_t size = 0, align = 1;

    memset(&st, 0, sizeof st);
    int rc = abi_parse_struct(notation, cursor, 1, &st, &size, &align);
    if (rc != OCERZ_OK)
        return rc;
    st.size = (uint16_t)size;
    st.align = (uint8_t)align;
    *out = st;
    return OCERZ_OK;
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

    const char *close = open;
    int depth = 0;
    for (; *close; close++) {
        if (*close == '{') {
            depth++;
        } else if (*close == '}') {
            if (depth == 0)
                break;
            depth--;
        }
    }
    if (*close != '}') {
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
    if (ret == '{') {
        int rc = abi_parse_layout(notation, &p, &sig.ret_struct);
        if (rc != OCERZ_OK)
            return rc;
    } else {
        p++;
    }

    if (*p != '(') {
        OCERZ_LOG("abi: %s has no argument list\n", notation);
        return OCERZ_EFORMAT;
    }
    p++;

    while (*p != '\0' && *p != ')') {
        char c = *p;
        if (!abi_is_arg_class(c)) {
            abi_reject(notation, c);
            return OCERZ_EUNSUP;
        }
        if (sig.nargs >= OCERZ_ABI_MAX_ARGS) {
            OCERZ_LOG("abi: %s has more than the %d arguments this engine carries\n",
                      notation, OCERZ_ABI_MAX_ARGS);
            return OCERZ_ETOOLONG;
        }
        if (c == '{') {
            int rc = abi_parse_layout(notation, &p, &sig.arg_struct[sig.nargs]);
            if (rc != OCERZ_OK)
                return rc;
        } else {
            p++;
            if (c == 'c') {
                int rc = abi_parse_callback(notation, sig.nargs, &p, sig.cb[sig.nargs]);
                if (rc != OCERZ_OK)
                    return rc;
            }
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

static int abi_struct_valid(const OcerzAbiStruct *st)
{
    size_t align = 1, end = 0;
    unsigned starts = 0;

    if (st->nmember < 1 || st->nmember > OCERZ_ABI_STRUCT_MEMBERS)
        return 0;
    if (st->size == 0 || st->size > OCERZ_ABI_STRUCT_BYTES)
        return 0;

    for (int k = 0; k < st->nmember; k++) {
        char c = st->member[k];
        if (!abi_is_scalar_class(c))
            return 0;
        size_t size = (size_t)abi_class_size(c);
        size_t at = st->offset[k];
        if (at % size != 0 || at < end)
            return 0;
        end = at + size;
        if (size > align)
            align = size;
        starts |= 1u << (at / 8);
    }

    if (end > st->size || st->align != align || st->size % align != 0)
        return 0;
    if (st->size <= ABI_SMALL_STRUCT && starts != (st->size > 8 ? 3u : 1u))
        return 0;
    return 1;
}

static char abi_hfa(const OcerzAbiStruct *st)
{
    char c = st->member[0];

    if (st->nmember > ABI_HOST_HFA_MAX || !abi_is_fp(c))
        return 0;
    for (int k = 1; k < st->nmember; k++)
        if (st->member[k] != c)
            return 0;
    return c;
}

static int abi_host_indirect(const OcerzAbiStruct *st)
{
    return st->size > ABI_SMALL_STRUCT && !abi_hfa(st);
}

static int abi_sysv_classify(const OcerzAbiStruct *st, char cls[2], int *nint, int *nsse)
{
    *nint = 0;
    *nsse = 0;
    if (st->size > ABI_SMALL_STRUCT)
        return 0;

    int n = (st->size + 7) / 8;
    cls[0] = 'S';
    cls[1] = 'S';
    for (int k = 0; k < st->nmember; k++)
        if (!abi_is_fp(st->member[k]))
            cls[st->offset[k] / 8] = 'I';
    for (int k = 0; k < n; k++) {
        if (cls[k] == 'I')
            (*nint)++;
        else
            (*nsse)++;
    }
    return n;
}

static size_t abi_struct_words(const OcerzAbiStruct *st)
{
    return ((size_t)st->size + 7) / 8;
}

static uint64_t abi_word(const uint8_t *buf, size_t at, size_t len)
{
    uint64_t w = 0;
    memcpy(&w, buf + at, len);
    return w;
}

static void abi_put_word(uint8_t *buf, size_t at, uint64_t w, size_t len)
{
    memcpy(buf + at, &w, len);
}

static void abi_zero_tail(const OcerzAbiStruct *st, uint8_t *buf)
{
    memset(buf + st->size, 0, abi_struct_words(st) * 8 - st->size);
}

static void abi_struct_pointers(const OcerzAbiStruct *st, uint8_t *buf, int to_host)
{
    for (int k = 0; k < st->nmember; k++) {
        if (st->member[k] != 'p')
            continue;
        uint64_t w = abi_word(buf, st->offset[k], 8);
        if (w)
            w = to_host ? (uint64_t)(uintptr_t)ocerz_g2h(w) : ocerz_h2g((const void *)(uintptr_t)w);
        abi_put_word(buf, st->offset[k], w, 8);
    }
}

static void abi_guest_read(uint64_t gaddr, uint8_t *buf, size_t len)
{
    size_t at = 0;
    for (; at + 8 <= len; at += 8)
        abi_put_word(buf, at, ocerz_ld(gaddr + at, 8), 8);
    for (; at < len; at++)
        buf[at] = (uint8_t)ocerz_ld(gaddr + at, 1);
}

static void abi_guest_write(uint64_t gaddr, const uint8_t *buf, size_t len)
{
    size_t at = 0;
    for (; at + 8 <= len; at += 8)
        ocerz_st(gaddr + at, 8, abi_word(buf, at, 8));
    for (; at < len; at++)
        ocerz_st(gaddr + at, 1, buf[at]);
}

static uint64_t abi_narrow(char c, uint64_t raw)
{
    switch (c) {
    case 'b': return (uint64_t)(int64_t)(int8_t)raw;
    case 'B': return (uint64_t)(uint8_t)raw;
    case 'h': return (uint64_t)(int64_t)(int16_t)raw;
    case 'H': return (uint64_t)(uint16_t)raw;
    case 'i': return (uint64_t)(int64_t)(int32_t)raw;
    case 'u':
    case 'f': return (uint64_t)(uint32_t)raw;
    default:  return raw;
    }
}

static size_t abi_host_stack_at(size_t off, int size)
{
    return abi_align_up(off, (size_t)size);
}

static int abi_push_host_bytes(OcerzAbiCall *call, size_t *off, const uint8_t *bytes, size_t len,
                               size_t align)
{
    size_t at = abi_align_up(*off, align);

    if (at + len > sizeof call->stack)
        return OCERZ_ETOOLONG;

    memcpy((unsigned char *)call->stack + at, bytes, len);
    *off = at + len;
    return OCERZ_OK;
}

static int abi_push_host_stack(OcerzAbiCall *call, size_t *off, uint64_t val, int size)
{
    uint8_t bytes[8];

    memcpy(bytes, &val, sizeof bytes);
    return abi_push_host_bytes(call, off, bytes, (size_t)size, (size_t)size);
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

static void abi_pull_host_bytes(const uint8_t *stack, size_t *off, uint8_t *bytes, size_t len,
                                size_t align)
{
    size_t at = abi_align_up(*off, align);

    memcpy(bytes, stack + at, len);
    *off = at + len;
}

static void abi_guest_struct_in(const OcerzAbiStruct *st, const OcerzCPU *cpu, int *gi, int *gf,
                                int *gslot, uint8_t *buf)
{
    char cls[2];
    int nint, nsse;
    int n = abi_sysv_classify(st, cls, &nint, &nsse);

    if (n && *gi + nint <= ABI_GUEST_INT_REGS && *gf + nsse <= ABI_GUEST_FP_REGS) {
        for (int k = 0; k < n; k++) {
            uint64_t w = cls[k] == 'I' ? cpu->gpr[abi_guest_int_reg[(*gi)++]]
                                       : cpu->xmm[(*gf)++].lo;
            abi_put_word(buf, 8 * (size_t)k, w, 8);
        }
    } else {
        uint64_t rsp = cpu->gpr[OCERZ_RSP];
        size_t words = abi_struct_words(st);
        for (size_t k = 0; k < words; k++)
            abi_put_word(buf, 8 * k, ocerz_ld(rsp + 8 + 8 * (uint64_t)(*gslot)++, 8), 8);
    }
    abi_zero_tail(st, buf);
}

static uint64_t abi_guest_next(const OcerzCPU *cpu, int fp, int *gi, int *gf, int *gslot)
{
    if (fp && *gf < ABI_GUEST_FP_REGS)
        return cpu->xmm[(*gf)++].lo;
    if (!fp && *gi < ABI_GUEST_INT_REGS)
        return cpu->gpr[abi_guest_int_reg[(*gi)++]];
    return ocerz_ld(cpu->gpr[OCERZ_RSP] + 8 + 8 * (uint64_t)(*gslot)++, 8);
}

static int abi_host_struct_out(const OcerzAbiStruct *st, const uint8_t *buf, OcerzAbiCall *call,
                               size_t *off)
{
    char hfa = abi_hfa(st);
    size_t words = abi_struct_words(st);

    if (hfa) {
        size_t step = (size_t)abi_class_size(hfa);
        if (call->nv + st->nmember <= ABI_HOST_FP_REGS) {
            for (int k = 0; k < st->nmember; k++)
                call->v[call->nv++] = abi_word(buf, (size_t)k * step, step);
            return OCERZ_OK;
        }
        call->nv = ABI_HOST_FP_REGS;
        return abi_push_host_bytes(call, off, buf, st->size, step);
    }

    if (st->size > ABI_SMALL_STRUCT) {
        uint64_t *copy = call->mem + call->nmem;
        call->nmem += (int)words;
        copy[words - 1] = 0;
        memcpy(copy, buf, st->size);
        uint64_t ptr = (uint64_t)(uintptr_t)copy;
        if (call->nx < ABI_HOST_INT_REGS) {
            call->x[call->nx++] = ptr;
            return OCERZ_OK;
        }
        return abi_push_host_stack(call, off, ptr, 8);
    }

    if (call->nx + (int)words <= ABI_HOST_INT_REGS) {
        for (size_t k = 0; k < words; k++)
            call->x[call->nx++] = abi_word(buf, 8 * k, 8);
        return OCERZ_OK;
    }
    call->nx = ABI_HOST_INT_REGS;
    return abi_push_host_bytes(call, off, buf, words * 8, 8);
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
    if (sig->ret == '{' && !abi_struct_valid(&sig->ret_struct)) {
        OCERZ_LOG("abi: the result structure has a layout no notation describes\n");
        return OCERZ_EUNSUP;
    }

    memset(call, 0, offsetof(OcerzAbiCall, ret));

    int guest_int = 0, guest_fp = 0, guest_slot = 0;
    size_t host_off = 0;

    if (sig->ret == '{') {
        if (sig->ret_struct.size > ABI_SMALL_STRUCT)
            call->guest_ret = cpu->gpr[abi_guest_int_reg[guest_int++]];
        if (abi_host_indirect(&sig->ret_struct))
            call->x8 = call->ret;
    }

    for (int i = 0; i < sig->nargs; i++) {
        char c = sig->arg[i];
        int fp = abi_is_fp(c);
        uint64_t raw;

        if (!abi_is_arg_class(c)) {
            OCERZ_LOG("abi: argument %d has class '%c', which no signature can name\n", i, c);
            return OCERZ_EUNSUP;
        }

        if (c == '{') {
            const OcerzAbiStruct *st = &sig->arg_struct[i];
            uint8_t buf[OCERZ_ABI_STRUCT_BYTES];
            if (!abi_struct_valid(st)) {
                OCERZ_LOG("abi: argument %d is a structure with a layout no notation describes\n", i);
                return OCERZ_EUNSUP;
            }
            abi_guest_struct_in(st, cpu, &guest_int, &guest_fp, &guest_slot, buf);
            abi_struct_pointers(st, buf, 1);
            if (abi_host_struct_out(st, buf, call, &host_off) != OCERZ_OK) {
                OCERZ_LOG("abi: argument %d spills past the %d-byte host argument window\n",
                          i, (int)sizeof call->stack);
                return OCERZ_ETOOLONG;
            }
            continue;
        }

        raw = abi_guest_next(cpu, fp, &guest_int, &guest_fp, &guest_slot);

        uint64_t val;
        if (c == 'p') {
            val = raw ? (uint64_t)(uintptr_t)ocerz_g2h(raw) : 0;
        } else if (c == 'c') {
            uint64_t fn;
            if (ocerz_abi_callback_convert(raw, sig->cb[i], &fn) != OCERZ_OK) {
                fprintf(stderr,
                        "ocerz: abi: argument %d is guest function %#llx, which could not be bound to"
                        " a callback trampoline, so the call is refused\n",
                        i, (unsigned long long)raw);
                return OCERZ_EUNSUP;
            }
            val = fn ? (uint64_t)(uintptr_t)ocerz_g2h(fn) : 0;
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

int ocerz_abi_va_start(const OcerzAbiSig *named, const OcerzCPU *cpu, OcerzAbiVaList *va)
{
    if (!named || !cpu || !va)
        return OCERZ_EUNDEF;
    if (named->nargs < 0 || named->nargs > OCERZ_ABI_MAX_ARGS)
        return OCERZ_ETOOLONG;

    memset(va, 0, sizeof *va);
    if (named->ret == '{') {
        if (!abi_struct_valid(&named->ret_struct))
            return OCERZ_EUNSUP;
        if (named->ret_struct.size > ABI_SMALL_STRUCT)
            va->gi++;
    }

    for (int i = 0; i < named->nargs; i++) {
        char c = named->arg[i];
        if (c == '{') {
            uint8_t buf[OCERZ_ABI_STRUCT_BYTES];
            if (!abi_struct_valid(&named->arg_struct[i]))
                return OCERZ_EUNSUP;
            abi_guest_struct_in(&named->arg_struct[i], cpu, &va->gi, &va->gf, &va->gslot, buf);
        } else if (abi_is_arg_class(c)) {
            abi_guest_next(cpu, abi_is_fp(c), &va->gi, &va->gf, &va->gslot);
        } else {
            return OCERZ_EUNSUP;
        }
    }
    return OCERZ_OK;
}

int ocerz_abi_va_arg(OcerzAbiVaList *va, const OcerzCPU *cpu, char cls, uint64_t *out)
{
    if (!va || !cpu || !out)
        return OCERZ_EUNDEF;
    *out = 0;
    if (cls != 'i' && cls != 'u' && cls != 'l' && cls != 'L' && cls != 'p' && cls != 'd')
        return OCERZ_EUNSUP;

    uint64_t raw = abi_guest_next(cpu, cls == 'd', &va->gi, &va->gf, &va->gslot);
    if (cls == 'p')
        *out = raw ? (uint64_t)(uintptr_t)ocerz_g2h(raw) : 0;
    else
        *out = abi_narrow(cls, raw);
    return OCERZ_OK;
}

static void abi_guest_struct_result(const OcerzAbiStruct *st, OcerzCPU *cpu, const OcerzAbiCall *call)
{
    static const uint8_t reg[2] = { OCERZ_RAX, OCERZ_RDX };
    uint8_t buf[OCERZ_ABI_STRUCT_BYTES];
    char hfa = abi_hfa(st);

    memset(buf, 0, abi_struct_words(st) * 8);
    if (hfa) {
        size_t step = (size_t)abi_class_size(hfa);
        for (int k = 0; k < st->nmember; k++)
            abi_put_word(buf, (size_t)k * step, call->rv[k], step);
    } else if (st->size > ABI_SMALL_STRUCT) {
        memcpy(buf, call->ret, st->size);
    } else {
        memcpy(buf, call->rx, st->size);
    }
    abi_struct_pointers(st, buf, 0);

    char cls[2];
    int nint, nsse;
    int n = abi_sysv_classify(st, cls, &nint, &nsse);
    if (!n) {
        abi_guest_write(call->guest_ret, buf, st->size);
        cpu->gpr[OCERZ_RAX] = call->guest_ret;
        return;
    }

    int ri = 0, si = 0;
    for (int k = 0; k < n; k++) {
        uint64_t w = abi_word(buf, 8 * (size_t)k, 8);
        if (cls[k] == 'I') {
            cpu->gpr[reg[ri++]] = w;
        } else {
            cpu->xmm[si].lo = w;
            cpu->xmm[si].hi = 0;
            si++;
        }
    }
}

void ocerz_abi_write_result(const OcerzAbiSig *sig, OcerzCPU *cpu, const OcerzAbiCall *call)
{
    if (!sig || !cpu || !call)
        return;

    switch (sig->ret) {
    case '{':
        abi_guest_struct_result(&sig->ret_struct, cpu, call);
        break;
    case 'v':
        cpu->gpr[OCERZ_RAX] = 0;
        break;
    case 'p':
        cpu->gpr[OCERZ_RAX] = call->rx[0] ? ocerz_h2g((const void *)(uintptr_t)call->rx[0]) : 0;
        break;
    case 'f':
        cpu->xmm[0].lo = (uint64_t)(uint32_t)call->rv[0];
        cpu->xmm[0].hi = 0;
        break;
    case 'd':
        cpu->xmm[0].lo = call->rv[0];
        cpu->xmm[0].hi = 0;
        break;
    default:
        cpu->gpr[OCERZ_RAX] = abi_narrow(sig->ret, call->rx[0]);
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

    int guest_round = fegetround();

    fesetround(FE_TONEAREST);
    ocerz_abi_call_native(fn, call.x, call.v, call.stack, (uint64_t)call.nstack * 8, call.x8,
                          call.rx, call.rv);
    fesetround(guest_round);

    ocerz_abi_write_result(sig, cpu, &call);
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

int ocerz_abi_is_guest_code(uint64_t gptr)
{
    const void *host = ocerz_g2h(gptr);

    if (ocerz_host_in_guest_reservation(host))
        return 1;

    size_t cache_len = 0;
    const void *cache = _dyld_get_shared_cache_range(&cache_len);
    if (cache && (uintptr_t)host - (uintptr_t)cache < cache_len)
        return 0;

    Dl_info info;
    return dladdr(host, &info) ? 0 : 1;
}

int ocerz_abi_callback_convert(uint64_t gptr, const char *notation, uint64_t *out)
{
    if (!out)
        return OCERZ_EUNDEF;
    *out = 0;
    if (!gptr)
        return OCERZ_OK;
    if (!ocerz_abi_is_guest_code(gptr)) {
        *out = gptr;
        return OCERZ_OK;
    }

    void *tramp = ocerz_abi_callback_intern(gptr, notation);
    if (!tramp)
        return OCERZ_EUNSUP;
    *out = ocerz_h2g(tramp);
    return OCERZ_OK;
}

static int abi_host_struct_in(const OcerzAbiStruct *st, const uint64_t *x, const uint64_t *v,
                              const uint8_t *stack, int *nx, int *nv, size_t *off, uint8_t *buf)
{
    char hfa = abi_hfa(st);
    size_t words = abi_struct_words(st);

    memset(buf, 0, words * 8);

    if (hfa) {
        size_t step = (size_t)abi_class_size(hfa);
        if (*nv + st->nmember <= ABI_HOST_FP_REGS) {
            for (int k = 0; k < st->nmember; k++)
                abi_put_word(buf, (size_t)k * step, v[(*nv)++], step);
            return OCERZ_OK;
        }
        *nv = ABI_HOST_FP_REGS;
        if (!stack)
            return OCERZ_EUNDEF;
        abi_pull_host_bytes(stack, off, buf, st->size, step);
        return OCERZ_OK;
    }

    if (st->size > ABI_SMALL_STRUCT) {
        uint64_t ptr;
        if (*nx < ABI_HOST_INT_REGS)
            ptr = x[(*nx)++];
        else if (stack)
            ptr = abi_pull_host_stack(stack, off, 8);
        else
            return OCERZ_EUNDEF;
        if (!ptr)
            return OCERZ_EFORMAT;
        memcpy(buf, (const void *)(uintptr_t)ptr, st->size);
        return OCERZ_OK;
    }

    if (*nx + (int)words <= ABI_HOST_INT_REGS) {
        for (size_t k = 0; k < words; k++)
            abi_put_word(buf, 8 * k, x[(*nx)++], 8);
    } else {
        *nx = ABI_HOST_INT_REGS;
        if (!stack)
            return OCERZ_EUNDEF;
        abi_pull_host_bytes(stack, off, buf, words * 8, 8);
    }
    abi_zero_tail(st, buf);
    return OCERZ_OK;
}

static int abi_guest_struct_out(const OcerzAbiStruct *st, const uint8_t *buf, OcerzGuestCall *call,
                                int *gi, int *gf)
{
    const int slots = (int)(sizeof call->stack / sizeof call->stack[0]);
    char cls[2];
    int nint, nsse;
    int n = abi_sysv_classify(st, cls, &nint, &nsse);

    if (n && *gi + nint <= ABI_GUEST_INT_REGS && *gf + nsse <= ABI_GUEST_FP_REGS) {
        for (int k = 0; k < n; k++) {
            uint64_t w = abi_word(buf, 8 * (size_t)k, 8);
            if (cls[k] == 'I')
                call->gpr[(*gi)++] = w;
            else
                call->xmm[(*gf)++] = w;
        }
        return 1;
    }

    int words = (int)abi_struct_words(st);
    if (call->nstack + words > slots)
        return 0;
    for (int k = 0; k < words; k++)
        call->stack[call->nstack++] = abi_word(buf, 8 * (size_t)k, 8);
    return 1;
}

static void abi_host_struct_result(const OcerzAbiStruct *st, const OcerzGuestCall *call,
                                   uint64_t guest_ret, void *x8, uint64_t *out_x, uint64_t *out_v)
{
    uint8_t buf[OCERZ_ABI_STRUCT_BYTES + 16];
    char cls[2];
    int nint, nsse;
    int n = abi_sysv_classify(st, cls, &nint, &nsse);

    memset(buf, 0, abi_struct_words(st) * 8 + 16);
    if (!n) {
        abi_guest_read(guest_ret, buf, st->size);
    } else {
        const uint64_t ireg[2] = { call->rax, call->rdx };
        const uint64_t sreg[2] = { call->xmm0, call->xmm1 };
        int ri = 0, si = 0;
        for (int k = 0; k < n; k++)
            abi_put_word(buf, 8 * (size_t)k, cls[k] == 'I' ? ireg[ri++] : sreg[si++], 8);
        abi_zero_tail(st, buf);
    }
    abi_struct_pointers(st, buf, 1);

    char hfa = abi_hfa(st);
    if (hfa) {
        size_t step = (size_t)abi_class_size(hfa);
        for (int k = 0; k < st->nmember; k++)
            out_v[k] = abi_word(buf, (size_t)k * step, step);
    } else if (st->size > ABI_SMALL_STRUCT) {
        memcpy(x8, buf, st->size);
    } else {
        out_x[0] = abi_word(buf, 0, 8);
        out_x[1] = abi_word(buf, 8, 8);
    }
}

void ocerz_abi_callback_dispatch(unsigned slot, const uint64_t *x, const uint64_t *v,
                                 const uint8_t *stack, void *x8, uint64_t *out_x, uint64_t *out_v)
{
    if (out_x)
        memset(out_x, 0, 2 * sizeof *out_x);
    if (out_v)
        memset(out_v, 0, 4 * sizeof *out_v);
    if (!x || !v || !out_x || !out_v) {
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

    if (sig->ret == '{' && abi_host_indirect(&sig->ret_struct) && !x8) {
        fprintf(stderr,
                "ocerz: abi: guest function %#llx (callback slot %u, %s) returns a structure through x8,"
                " and its native caller passed no buffer there\n",
                (unsigned long long)e->guest_fn, slot, e->notation);
        return;
    }

    OcerzCPU *cpu = ocerz_vm_current_cpu();
    if (!cpu)
        cpu = ocerz_thread_attach(ocerz_vm_process());
    if (!cpu) {
        fprintf(stderr,
                "ocerz: abi: native code called guest function %#llx (callback slot %u, %s) on a thread"
                " with no guest cpu, and no guest personality could be attached to it\n",
                (unsigned long long)e->guest_fn, slot, e->notation);
        return;
    }

    OcerzVM *vm = cpu->vm;
    if (!vm || vm->exited)
        return;

    OcerzGuestCall call;
    memset(&call, 0, sizeof call);

    const int slots = (int)(sizeof call.stack / sizeof call.stack[0]);
    int nx = 0, nv = 0, gi = 0, gf = 0;
    size_t off = 0;
    uint64_t stack_top = (cpu->gpr[OCERZ_RSP] - 128) & ~0xfull;
    uint64_t guest_ret = 0;

    if (sig->ret == '{' && sig->ret_struct.size > ABI_SMALL_STRUCT) {
        stack_top = (stack_top - sig->ret_struct.size) & ~0xfull;
        guest_ret = stack_top;
        call.gpr[gi++] = guest_ret;
    }

    for (int i = 0; i < sig->nargs; i++) {
        char c = sig->arg[i];
        int fp = abi_is_fp(c);
        uint64_t raw;

        if (c == '{') {
            const OcerzAbiStruct *st = &sig->arg_struct[i];
            uint8_t buf[OCERZ_ABI_STRUCT_BYTES];
            int rc = abi_host_struct_in(st, x, v, stack, &nx, &nv, &off, buf);
            if (rc != OCERZ_OK) {
                fprintf(stderr,
                        "ocerz: abi: guest function %#llx (callback slot %u, %s) takes structure argument"
                        " %d %s\n",
                        (unsigned long long)e->guest_fn, slot, e->notation, i,
                        rc == OCERZ_EFORMAT ? "through a copy, and the native caller passed a null address"
                                            : "from the native caller's stack, and no stack was passed");
                return;
            }
            abi_struct_pointers(st, buf, 0);
            if (!abi_guest_struct_out(st, buf, &call, &gi, &gf)) {
                fprintf(stderr,
                        "ocerz: abi: guest function %#llx (callback slot %u, %s) stacks more than the %d"
                        " eightbytes a guest call carries\n",
                        (unsigned long long)e->guest_fn, slot, e->notation, slots);
                return;
            }
            continue;
        }

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
        } else if (call.nstack < slots) {
            call.stack[call.nstack++] = val;
        } else {
            fprintf(stderr,
                    "ocerz: abi: guest function %#llx (callback slot %u, %s) stacks more than the %d"
                    " eightbytes a guest call carries\n",
                    (unsigned long long)e->guest_fn, slot, e->notation, slots);
            return;
        }
    }

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
    case '{':
        abi_host_struct_result(&sig->ret_struct, &call, guest_ret, x8, out_x, out_v);
        break;
    case 'p':
        out_x[0] = call.rax ? (uint64_t)(uintptr_t)ocerz_g2h(call.rax) : 0;
        break;
    case 'f':
        out_v[0] = (uint64_t)(uint32_t)call.xmm0;
        break;
    case 'd':
        out_v[0] = call.xmm0;
        break;
    default:
        out_x[0] = abi_narrow(sig->ret, call.rax);
        break;
    }
}

/*
 * Objective-C message sends and formatted output, crossed from an x86 guest into
 * the host's native arm64 runtime.
 *
 * ---- one runtime, the host's ----
 * In native mode no x86 libobjc exists.  A guest's classes are the host's own:
 * _OBJC_CLASS_$_NSString binds to the native class object, a constant string's
 * isa to the native constant-string class, and an object any native method hands
 * back is an address the guest can hold because native mode runs in the identity
 * map.  What the guest cannot do is send a message, because objc_msgSend is a
 * trampoline into whatever method implementation the selector finds, and the
 * arguments of that implementation are in x86 registers.  So every send is a
 * crossing whose signature is not known until the send arrives, and the runtime
 * that is about to run the method is asked for it.
 *
 * ---- selectors ----
 * A selector is compared by address.  The guest's @selector(length) is a word in
 * its __objc_selrefs pointing at the string "length" inside its own image, and
 * the native runtime's SEL for the same name is a different address, so a guest
 * that compared _cmd against its own selref, or handed its selref to native code
 * that keys a table by SEL, would see two selectors.  ocerz_objcbridge_fix_selrefs
 * rewrites every selref of an image to sel_registerName of its string after the
 * image's fixups and before any guest code runs, which makes every selector the
 * guest holds the native one.  A sel_registerName copies the name, so nothing
 * native points into guest memory afterwards.  The legacy fixup-message ABI's
 * __objc_msgrefs pairs get the same rewrite of their selector word; their
 * implementation word stays bound to objc_msgSend_fixup, which is a stub record
 * and names itself when called, because no current compiler emits one.  Cache
 * mode is untouched: there the guest runs the translated x86 runtime, which
 * canonicalizes its own images.
 *
 * ---- a send ----
 * x86-64's objc_msgSend takes the receiver in rdi and the selector in rsi, the
 * method's own arguments after them in the usual System V places, so the method
 * signature is exactly the rest of the crossing's signature.  It comes from the
 * native runtime: object_getClass of the receiver, which understands tagged
 * pointers because it is the runtime that made them, then class_getInstanceMethod
 * of that class and the selector, and method_getTypeEncoding.  A class receiver's
 * class is its metaclass, whose instance methods are the class methods, so +alloc
 * and +stringWithFormat: are found the same way.  class_getInstanceMethod runs a
 * class's method resolver, so a method added lazily is found too.  The call
 * itself goes to the host's objc_msgSend under that signature with the receiver
 * and selector as its first two pointer arguments: native objc_msgSend leaves
 * every argument register and the stack untouched and jumps to the
 * implementation, which is what makes one engine crossing correct for every
 * method.
 *
 * The encoding is turned into the ABI notation once per class and selector and
 * parsed once per distinct notation.  Both tables are append-only hash tables
 * whose buckets are atomic list heads: a lookup reads without a lock, an insert
 * takes the one mutex and checks its bucket again before linking a complete
 * entry, and nothing is ever freed, so a notation or an entry, once a thread has
 * seen it, stays valid for the life of the process and a bridge frame may point
 * at it.  Methods share notations heavily - most of NSString is p(pp), L(pp) or p(ppp) -
 * so the parsed signatures, about 1.4 kilobytes each, are shared too.
 *
 * ---- encodings ----
 * The encoding read is arm64's, since it is the native runtime's, and it
 * converts class by class: c b, C B, s h, S H, i i, I u, q l, Q L, f f, d d, v
 * as a result only, and * @ # : and any pointer p.  l and L are 32-bit in an
 * encoding and become i and u.  B is BOOL on arm64, where BOOL is bool, while an
 * x86 guest's BOOL is a signed char, and it becomes b, as the API database writes
 * it: the two agree on every canonical value, and a guest that passes a BOOL
 * other than 0 or 1 hands a native bool a value its callee does not expect, which
 * is written down rather than normalized.  A structure {name=members} becomes
 * the engine's braces, nested structures nested, an array member flattened into
 * that many members, field names in quotes skipped.  Type qualifiers r n N o O R
 * V and A and the frame offsets between types are skipped.  A pointer is p
 * whatever it points at, and an array argument is a pointer.  A union, a
 * bitfield, a long double, a complex number, a 128-bit integer, an unknown type,
 * a structure whose members the encoding omits, void where a value belongs, more
 * than sixteen arguments, and anything the ABI engine refuses to lay out, such as
 * a structure of more than sixteen members, stop the send with the class, the
 * selector, the encoding and the reason, and OCERZ_BRIDGE_UNIMPL_EXIT.
 *
 * A block argument, @?, and a function pointer, ^?, cross as pointers when they
 * are null or native.  A block is native when its invoke function, the word at
 * offset 16, is not guest code by ocerz_abi_is_guest_code, which a block a native
 * API made is not.  A guest block's invoke is x86 code, and a guest function
 * pointer is x86 code with no signature an encoding could give it, so either
 * stops the send by name: native code calling it would jump into x86 bytes.
 *
 * ---- results the two ABIs return differently ----
 * System V returns a structure of more than sixteen bytes in memory the caller
 * provides, and an x86 compiler therefore sends such a message with
 * objc_msgSend_stret, whose hidden result pointer comes first in rdi and pushes
 * the receiver to rsi and the selector to rdx.  Apple's arm64 has no _stret
 * variant: the same objc_msgSend returns a large structure through x8, and a
 * small aggregate of doubles such as a CGRect in d0 to d3.  The engine already
 * does both halves when a signature's result is a structure: it takes the
 * guest's pointer ahead of every integer argument, hands native code a buffer of
 * its own in x8, and copies the result through the guest's pointer.  So a
 * _stret send is an ordinary send under the same signature, and the only check
 * is that the two agree.  A _stret send of a method whose result System V
 * returns in registers, and a plain send of one it returns in memory, are sends
 * the guest's compiler could not have made for this method, and the guest's
 * registers do not hold what the signature says, so both stop by name.  x86-64
 * uses objc_msgSend_fpret only for a long double result and objc_msgSend_fp2ret
 * only for a long double complex one, and neither crosses.
 *
 * ---- super ----
 * objc_msgSendSuper and objc_msgSendSuper2 take a pointer to a struct objc_super,
 * the receiver and a class, where objc_msgSend takes the receiver; the first
 * looks the method up starting at that class and the second at its superclass.
 * The structure is guest memory the native runtime can read as it is, so the
 * pointer is passed through, the native super entry replaces it with the
 * receiver before the implementation runs, and the signature comes from the
 * class the native entry will search.  The _stret forms take the result pointer
 * first, as objc_msgSend_stret does.
 *
 * ---- nil ----
 * A message to nil does nothing and answers zero.  rax, rdx, xmm0 and xmm1 are
 * zeroed, which covers every place System V returns a scalar or a small
 * structure.  A _stret send to nil sets rax to the result pointer as the ABI
 * requires, and zeroes the structure only when its size can be known: a super
 * send names a class to look the method up in, while a plain send to nil has no
 * class and therefore no signature, and its result memory is left as the guest
 * had it.
 *
 * ---- forwarding ----
 * A selector with no method is forwarded natively, and forwarding still needs
 * the arguments where the signature puts them.  The signature is then the
 * receiver's own answer to methodSignatureForSelector:, called through native
 * objc_msgSend, with its methodReturnType and each getArgumentTypeAtIndex:
 * concatenated into an encoding.  That answer can depend on the instance, as a
 * proxy's does, so it is asked on every forwarded send and never cached.  A nil
 * answer is an unrecognized selector, which natively raises an exception that
 * nothing in the guest can catch, so it stops the send naming the class and the
 * selector.
 *
 * ---- variadic methods ----
 * No encoding says a method is variadic, and Apple's arm64 puts every variadic
 * argument in an eight-byte stack slot where a fixed argument would go in a
 * register, so the Foundation methods declared with an ellipsis are listed here
 * by selector, each checked against the SDK's own declaration:
 * NS_FORMAT_FUNCTION or NS_REQUIRES_NIL_TERMINATION, or a trailing ellipsis where
 * the header gives no attribute.  A format method's variadic arguments are what
 * the format argument's conversions say, read natively out of the NSString; for
 * the validated-format methods that is the validFormatSpecifiers argument, which
 * is the one their attribute names, for the attributed-string ones the format's
 * string, and for predicateWithFormat: and expressionWithFormat: the predicate
 * dialect, where %K is an object and a quoted %@ is text.  encodeValuesOfObjCTypes:
 * and decodeValuesOfObjCTypes: take one pointer per type in their type list.  A
 * nil-terminated method's are pointers up to and including the first nil, and
 * none at all when the last named argument is already nil.  The engine gathers
 * them from wherever System V put them, continuing after the named arguments
 * through ocerz_abi_va_start and ocerz_abi_va_arg, and they go on the native
 * stack after the named arguments' own, one slot each.  A selector in the list
 * whose method does not have the listed argument as an object is some other
 * class's fixed method of the same name, and is sent as one.
 *
 * ---- formatted output ----
 * printf, fprintf, sprintf, snprintf, asprintf, dprintf, __sprintf_chk,
 * __snprintf_chk, NSLog, CFStringCreateWithFormat and CFStringAppendFormat are
 * veneers over their v forms, which every one of them has.  The named arguments
 * cross as an ordinary signature; the format string, read straight from guest
 * memory for C and natively for a CFString or NSString, says what follows; the
 * variadic arguments are gathered into eight-byte slots; and the address of the
 * slots is the v form's va_list, because an Apple arm64 va_list is nothing but a
 * pointer to such slots.  The v form's result is the veneer's result, and errno
 * is left as the native call left it.
 *
 * A conversion takes one slot and a * width or precision one slot before it.  An
 * integer conversion is an int, sign-extended from its low half, unless its
 * length says long, long long, quad, intmax_t, size_t or ptrdiff_t, when it is
 * the whole word; hh and h are an int, since that is what a variadic char or
 * short is promoted to, and L on an integer conversion is an int, which is what
 * both Apple's printf and CoreFoundation read for it.  D, O and U are long in C
 * and a 32-bit int in a CoreFoundation format, which is what each reads on this
 * host.  %c and %C are an int, %s and %S a pointer followed, %p a word printed
 * and never converted, %@ an object, the floating conversions a double.  A
 * long double conversion, %n, a positional argument, and a conversion the
 * dialect does not have are refused with the export and the format, because a
 * guess at any of them reads the wrong slot for every argument after it.  The
 * crossing's rounding mode is the default one, as for every crossing, so a
 * double prints as it would in a native process whatever MXCSR the guest set.
 *
 * ---- exceptions ----
 * An Objective-C exception cannot unwind through guest frames.  The unwinder
 * walks the host stack, and the guest's frames are not on it: they are x86
 * frames on the guest's own stack, while the host stack under a crossing holds
 * the native method, ocerz's handler and ocerz's run loop.  So a native
 * exception thrown under a crossing finds no handler even when the guest
 * wrapped the send in @try, and is uncaught.  The native runtime then calls its uncaught-exception handler and
 * terminates.  ocerz_objcbridge_install_uncaught puts ocerz's handler there,
 * once, when a guest that links libobjc is loaded: it prints the exception's name
 * and reason and the innermost crossing, which for a send is the selector, as
 * "ocerz: bridge: uncaught Objective-C exception <name>: <reason> during <sym>",
 * then calls the handler it displaced, CoreFoundation's, which prints the usual
 * termination message and the NSSetUncaughtExceptionHandler handler, and returns
 * to the runtime, which aborts as it would natively.  A guest's own
 * objc_setUncaughtExceptionHandler interns its handler as a v(p) callback and
 * installs that natively in ocerz's place, and a null one puts ocerz's back; the
 * guest is answered with the handler it set before, or null, never with a native
 * function it could not call.
 *
 * ---- what every crossing here shares ----
 * A send raises a bridge frame before it touches the receiver, naming the export,
 * and once the method is known names the selector and the notation, so a fault
 * inside a native method is reported against the message that ran it.  The frame
 * is lowered after the call returns and before pending guest signals are
 * delivered, which every crossing does last.  A refusal is a line on stderr and
 * OCERZ_BRIDGE_UNIMPL_EXIT, never a call made under a guessed signature.
 * OCERZ_OBJCLOG prints each send's class, selector and notation as it crosses.
 */
#include "ocerz/objcbridge.h"
#include "ocerz/abi.h"
#include "ocerz/bridge.h"
#include "ocerz/interp.h"
#include "ocerz/mem.h"
#include "ocerz/syscall.h"
#include "ocerz/vdylib.h"
#include "ocerz/vm.h"

#include <ctype.h>
#include <errno.h>
#include <fenv.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>
#include <mach-o/loader.h>

#define OB_SMALL_STRUCT 16
#define OB_SHAPE_BUCKETS 512
#define OB_SEND_BUCKETS 4096
#define OB_UTF8 0x08000100u
#define OB_BLOCK 1
#define OB_FNPTR 2

typedef struct ObSym {
    const char *lib;
    const char *name;
    void *_Atomic addr;
} ObSym;

#define OB_SYM(lib, name) { (lib), (name), NULL }

static ObSym g_ob_msgSend = OB_SYM(OCERZ_OBJC_LIBOBJC, "objc_msgSend");
static ObSym g_ob_msgSendSuper = OB_SYM(OCERZ_OBJC_LIBOBJC, "objc_msgSendSuper");
static ObSym g_ob_msgSendSuper2 = OB_SYM(OCERZ_OBJC_LIBOBJC, "objc_msgSendSuper2");
static ObSym g_ob_sel_registerName = OB_SYM(OCERZ_OBJC_LIBOBJC, "sel_registerName");
static ObSym g_ob_sel_getName = OB_SYM(OCERZ_OBJC_LIBOBJC, "sel_getName");
static ObSym g_ob_object_getClass = OB_SYM(OCERZ_OBJC_LIBOBJC, "object_getClass");
static ObSym g_ob_class_getInstanceMethod = OB_SYM(OCERZ_OBJC_LIBOBJC, "class_getInstanceMethod");
static ObSym g_ob_method_getTypeEncoding = OB_SYM(OCERZ_OBJC_LIBOBJC, "method_getTypeEncoding");
static ObSym g_ob_class_getName = OB_SYM(OCERZ_OBJC_LIBOBJC, "class_getName");
static ObSym g_ob_class_isMetaClass = OB_SYM(OCERZ_OBJC_LIBOBJC, "class_isMetaClass");
static ObSym g_ob_class_getSuperclass = OB_SYM(OCERZ_OBJC_LIBOBJC, "class_getSuperclass");
static ObSym g_ob_class_respondsToSelector = OB_SYM(OCERZ_OBJC_LIBOBJC, "class_respondsToSelector");
static ObSym g_ob_setUncaught = OB_SYM(OCERZ_OBJC_LIBOBJC, "objc_setUncaughtExceptionHandler");
static ObSym g_ob_CFStringGetLength = OB_SYM(OCERZ_BRIDGE_COREFOUNDATION, "CFStringGetLength");
static ObSym g_ob_CFStringGetMaximumSizeForEncoding =
    OB_SYM(OCERZ_BRIDGE_COREFOUNDATION, "CFStringGetMaximumSizeForEncoding");
static ObSym g_ob_CFStringGetCString = OB_SYM(OCERZ_BRIDGE_COREFOUNDATION, "CFStringGetCString");

static void *ob_sym(ObSym *s)
{
    void *a = s->addr;
    if (!a) {
        a = ocerz_bridge_host_symbol(s->lib, s->name);
        if (a)
            s->addr = a;
    }
    return a;
}

static _Noreturn void ob_stop(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static _Noreturn void ob_stop(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("ocerz: bridge: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
    exit(OCERZ_BRIDGE_UNIMPL_EXIT);
}

static void *ob_need(ObSym *s)
{
    void *a = ob_sym(s);
    if (!a)
        ob_stop("the host %s has no %s, which crossing this call needs", s->lib, s->name);
    return a;
}

static void *ob_object_getClass(void *obj)
{
    return ((void *(*)(void *))ob_need(&g_ob_object_getClass))(obj);
}

static void *ob_class_getInstanceMethod(void *cls, void *sel)
{
    return ((void *(*)(void *, void *))ob_need(&g_ob_class_getInstanceMethod))(cls, sel);
}

static const char *ob_method_getTypeEncoding(void *m)
{
    return ((const char *(*)(void *))ob_need(&g_ob_method_getTypeEncoding))(m);
}

static const char *ob_sel_getName(void *sel)
{
    return ((const char *(*)(void *))ob_need(&g_ob_sel_getName))(sel);
}

static void *ob_sel_registerName(const char *name)
{
    return ((void *(*)(const char *))ob_need(&g_ob_sel_registerName))(name);
}

static const char *ob_class_getName(void *cls)
{
    return ((const char *(*)(void *))ob_need(&g_ob_class_getName))(cls);
}

static int ob_class_isMetaClass(void *cls)
{
    return ((bool (*)(void *))ob_need(&g_ob_class_isMetaClass))(cls);
}

static void *ob_class_getSuperclass(void *cls)
{
    return ((void *(*)(void *))ob_need(&g_ob_class_getSuperclass))(cls);
}

static int ob_class_respondsToSelector(void *cls, void *sel)
{
    return ((bool (*)(void *, void *))ob_need(&g_ob_class_respondsToSelector))(cls, sel);
}

static _Noreturn void ob_refuse(void *cls, void *sel, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static _Noreturn void ob_refuse(void *cls, void *sel, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "ocerz: bridge: %c[%s %s] ", cls && ob_class_isMetaClass(cls) ? '+' : '-',
            cls ? ob_class_getName(cls) : "nil", sel ? ob_sel_getName(sel) : "(null selector)");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
    exit(OCERZ_BRIDGE_UNIMPL_EXIT);
}

static int ob_settle(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (ocerz_peek_pending_async_sig() || (cpu->sig_pending & ~cpu->sig_mask))
        ocerz_guest_deliver_pending(vm, cpu);
    return OCERZ_STEP_OK;
}

static void ob_return(OcerzCPU *cpu, uint64_t rax)
{
    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    cpu->rip = ocerz_ld(rsp, 8);
    cpu->gpr[OCERZ_RSP] = rsp + 8;
    cpu->gpr[OCERZ_RAX] = rax;
}

static int ob_logging(void)
{
    static int en = -1;
    if (en < 0)
        en = getenv("OCERZ_OBJCLOG") ? 1 : 0;
    return en;
}

typedef struct ObOut {
    char *buf;
    size_t cap;
    size_t len;
    int overflow;
} ObOut;

static void ob_put(ObOut *o, char c)
{
    if (o->len + 1 >= o->cap) {
        o->overflow = 1;
        return;
    }
    o->buf[o->len++] = c;
    o->buf[o->len] = '\0';
}

enum { OB_RESULT, OB_ARG, OB_MEMBER };

static const char *ob_quals(const char *p)
{
    while (*p && strchr("rnNoORVA", *p))
        p++;
    return p;
}

static const char *ob_offset(const char *p)
{
    while (*p == '-' || isdigit((unsigned char)*p))
        p++;
    return p;
}

static const char *ob_group_end(const char *p, char open, char close)
{
    int depth = 0;
    for (; *p; p++) {
        if (*p == '"') {
            const char *q = strchr(p + 1, '"');
            if (!q)
                return NULL;
            p = q;
        } else if (*p == open) {
            depth++;
        } else if (*p == close && --depth == 0) {
            return p + 1;
        }
    }
    return NULL;
}

static const char *ob_skip(const char *p)
{
    p = ob_quals(p);
    switch (*p) {
    case '\0':
        return NULL;
    case '^':
    case 'j':
        return ob_skip(p + 1);
    case '@':
        p++;
        if (*p == '?')
            return p[1] == '<' ? ob_group_end(p + 1, '<', '>') : p + 1;
        if (*p == '"') {
            const char *q = strchr(p + 1, '"');
            return q ? q + 1 : NULL;
        }
        return p;
    case '{':
        return ob_group_end(p, '{', '}');
    case '(':
        return ob_group_end(p, '(', ')');
    case '[':
        return ob_group_end(p, '[', ']');
    case 'b':
        p++;
        while (isdigit((unsigned char)*p))
            p++;
        return p;
    default:
        return p + 1;
    }
}

static const char *ob_conv(const char *p, ObOut *o, int where, int *rc, int *special)
{
    p = ob_quals(p);
    switch (*p) {
    case 'c': ob_put(o, 'b'); return p + 1;
    case 'C': ob_put(o, 'B'); return p + 1;
    case 's': ob_put(o, 'h'); return p + 1;
    case 'S': ob_put(o, 'H'); return p + 1;
    case 'i': ob_put(o, 'i'); return p + 1;
    case 'I': ob_put(o, 'u'); return p + 1;
    case 'l': ob_put(o, 'i'); return p + 1;
    case 'L': ob_put(o, 'u'); return p + 1;
    case 'q': ob_put(o, 'l'); return p + 1;
    case 'Q': ob_put(o, 'L'); return p + 1;
    case 'f': ob_put(o, 'f'); return p + 1;
    case 'd': ob_put(o, 'd'); return p + 1;
    case 'B': ob_put(o, 'b'); return p + 1;
    case 'v':
        if (where != OB_RESULT) {
            *rc = OCERZ_OBJC_VOID_VALUE;
            return NULL;
        }
        ob_put(o, 'v');
        return p + 1;
    case '*':
    case '#':
    case ':':
    case '%':
        ob_put(o, 'p');
        return p + 1;
    case '@':
    case '^': {
        const char *q = ob_skip(p);
        if (!q) {
            *rc = OCERZ_OBJC_MALFORMED;
            return NULL;
        }
        if (special && p[1] == '?')
            *special = *p == '@' ? OB_BLOCK : OB_FNPTR;
        ob_put(o, 'p');
        return q;
    }
    case '[': {
        if (where != OB_MEMBER) {
            const char *q = ob_group_end(p, '[', ']');
            if (!q) {
                *rc = OCERZ_OBJC_MALFORMED;
                return NULL;
            }
            ob_put(o, 'p');
            return q;
        }
        p++;
        unsigned long count = 0;
        while (isdigit((unsigned char)*p)) {
            count = count * 10 + (unsigned long)(*p - '0');
            if (count > OCERZ_ABI_STRUCT_MEMBERS) {
                *rc = OCERZ_OBJC_ENGINE;
                return NULL;
            }
            p++;
        }
        if (count == 0) {
            *rc = OCERZ_OBJC_ENGINE;
            return NULL;
        }
        const char *after = NULL;
        for (unsigned long k = 0; k < count; k++) {
            after = ob_conv(p, o, OB_MEMBER, rc, NULL);
            if (!after)
                return NULL;
        }
        if (*after != ']') {
            *rc = OCERZ_OBJC_MALFORMED;
            return NULL;
        }
        return after + 1;
    }
    case '{': {
        p++;
        while (*p && *p != '=' && *p != '}')
            p++;
        if (*p == '}') {
            *rc = OCERZ_OBJC_OPAQUE;
            return NULL;
        }
        if (*p != '=') {
            *rc = OCERZ_OBJC_MALFORMED;
            return NULL;
        }
        p++;
        ob_put(o, '{');
        int members = 0;
        for (;;) {
            if (*p == '"') {
                const char *q = strchr(p + 1, '"');
                if (!q) {
                    *rc = OCERZ_OBJC_MALFORMED;
                    return NULL;
                }
                p = q + 1;
            }
            if (*p == '}')
                break;
            if (*p == '\0') {
                *rc = OCERZ_OBJC_MALFORMED;
                return NULL;
            }
            p = ob_conv(p, o, OB_MEMBER, rc, NULL);
            if (!p)
                return NULL;
            members++;
        }
        if (members == 0) {
            *rc = OCERZ_OBJC_OPAQUE;
            return NULL;
        }
        ob_put(o, '}');
        return p + 1;
    }
    case '(':
        *rc = OCERZ_OBJC_UNION;
        return NULL;
    case 'b':
        *rc = OCERZ_OBJC_BITFIELD;
        return NULL;
    case 'D':
        *rc = OCERZ_OBJC_LONG_DOUBLE;
        return NULL;
    case 'j':
        *rc = OCERZ_OBJC_COMPLEX;
        return NULL;
    case 't':
    case 'T':
        *rc = OCERZ_OBJC_INT128;
        return NULL;
    case '?':
        *rc = OCERZ_OBJC_UNKNOWN;
        return NULL;
    default:
        *rc = OCERZ_OBJC_MALFORMED;
        return NULL;
    }
}

int ocerz_objc_notation(const char *encoding, char *out, size_t outlen, int *nargs,
                        uint32_t *blocks, uint32_t *fnptrs)
{
    char local[OCERZ_OBJC_NOTATION_MAX];
    ObOut o = { out ? out : local, out ? outlen : sizeof local, 0, 0 };
    int rc = OCERZ_OBJC_OK, n = 0;
    uint32_t bmask = 0, fmask = 0;

    if (o.cap == 0)
        return OCERZ_OBJC_TOO_LONG;
    o.buf[0] = '\0';
    if (!encoding || !*encoding)
        return OCERZ_OBJC_MALFORMED;

    const char *p = ob_conv(encoding, &o, OB_RESULT, &rc, NULL);
    if (!p)
        return rc;
    p = ob_offset(p);
    ob_put(&o, '(');
    while (*p) {
        int special = 0;
        if (n >= OCERZ_ABI_MAX_ARGS)
            return OCERZ_OBJC_TOO_MANY_ARGS;
        p = ob_conv(p, &o, OB_ARG, &rc, &special);
        if (!p)
            return rc;
        if (special == OB_BLOCK)
            bmask |= 1u << n;
        else if (special == OB_FNPTR)
            fmask |= 1u << n;
        p = ob_offset(p);
        n++;
        if (o.overflow)
            return OCERZ_OBJC_TOO_LONG;
    }
    ob_put(&o, ')');
    if (o.overflow)
        return OCERZ_OBJC_TOO_LONG;

    OcerzAbiSig sig;
    if (ocerz_abi_parse(o.buf, &sig) != OCERZ_OK)
        return OCERZ_OBJC_ENGINE;
    if (nargs)
        *nargs = n;
    if (blocks)
        *blocks = bmask;
    if (fnptrs)
        *fnptrs = fmask;
    return OCERZ_OBJC_OK;
}

const char *ocerz_objc_refusal(int code)
{
    switch (code) {
    case OCERZ_OBJC_OK:            return "nothing refused";
    case OCERZ_OBJC_UNION:         return "a union passed by value";
    case OCERZ_OBJC_BITFIELD:      return "a bitfield";
    case OCERZ_OBJC_LONG_DOUBLE:   return "a long double";
    case OCERZ_OBJC_COMPLEX:       return "a complex number";
    case OCERZ_OBJC_INT128:        return "a 128-bit integer";
    case OCERZ_OBJC_UNKNOWN:       return "a value of unknown type";
    case OCERZ_OBJC_OPAQUE:        return "a structure by value whose members the encoding does not give";
    case OCERZ_OBJC_VOID_VALUE:    return "void where a value belongs";
    case OCERZ_OBJC_TOO_MANY_ARGS: return "more arguments than the ABI engine carries";
    case OCERZ_OBJC_TOO_LONG:      return "a notation longer than ocerz keeps";
    case OCERZ_OBJC_ENGINE:        return "a structure the ABI engine cannot lay out";
    default:                       return "an encoding ocerz cannot parse";
    }
}

int ocerz_objc_format_classes(const char *fmt, int dialect, char *out, size_t outlen,
                              const char **why)
{
    const char *unused;
    size_t n = 0;
    char quote = 0;

    if (!why)
        why = &unused;
    *why = NULL;
    if (!out || outlen == 0) {
        *why = "no room for a single argument";
        return -1;
    }
    out[0] = '\0';
    if (!fmt)
        return 0;

#define OB_EMIT(c) do {                                                      \
        if (n + 1 >= outlen) {                                               \
            *why = "more variadic arguments than ocerz gathers";             \
            return -1;                                                       \
        }                                                                    \
        out[n++] = (c);                                                      \
        out[n] = '\0';                                                       \
    } while (0)

    if (dialect == OCERZ_OBJC_FMT_TYPES) {
        const char *p = fmt;
        while (*p) {
            const char *q = ob_skip(p);
            if (!q) {
                *why = "a type list ocerz cannot parse";
                return -1;
            }
            OB_EMIT('p');
            p = ob_offset(q);
        }
        return (int)n;
    }

    for (const char *p = fmt; *p; p++) {
        if (dialect == OCERZ_OBJC_FMT_PREDICATE) {
            if (quote) {
                if (*p == '\\' && p[1])
                    p++;
                else if (*p == quote)
                    quote = 0;
                continue;
            }
            if (*p == '\'' || *p == '"') {
                quote = *p;
                continue;
            }
        }
        if (*p != '%')
            continue;
        p++;
        if (*p == '%')
            continue;
        if (*p == '\0')
            break;

        const char *d = p;
        while (isdigit((unsigned char)*d))
            d++;
        if (d > p && *d == '$') {
            *why = "a positional argument (%n$)";
            return -1;
        }
        while (*p && strchr("-+ #0'", *p))
            p++;
        if (*p == '*') {
            OB_EMIT('i');
            p++;
            if (isdigit((unsigned char)*p)) {
                *why = "a positional width (*n$)";
                return -1;
            }
        } else {
            while (isdigit((unsigned char)*p))
                p++;
        }
        if (*p == '.') {
            p++;
            if (*p == '*') {
                OB_EMIT('i');
                p++;
                if (isdigit((unsigned char)*p)) {
                    *why = "a positional precision (.*n$)";
                    return -1;
                }
            } else {
                while (isdigit((unsigned char)*p))
                    p++;
            }
        }

        char len = 0;
        if (p[0] == 'h' && p[1] == 'h') {
            len = 'H';
            p += 2;
        } else if (*p == 'h') {
            len = 'h';
            p++;
        } else if (p[0] == 'l' && p[1] == 'l') {
            len = 'q';
            p += 2;
        } else if (*p == 'l') {
            len = 'l';
            p++;
        } else if (*p == 'q' || *p == 'j' || *p == 'z' || *p == 't') {
            len = 'q';
            p++;
        } else if (*p == 'L') {
            len = 'L';
            p++;
        }

        switch (*p) {
        case 'd':
        case 'i':
        case 'o':
        case 'u':
        case 'x':
        case 'X':
            OB_EMIT(len == 'l' || len == 'q' ? 'l' : 'i');
            break;
        case 'D':
        case 'O':
        case 'U':
            OB_EMIT(dialect == OCERZ_OBJC_FMT_C || len == 'l' || len == 'q' ? 'l' : 'i');
            break;
        case 'c':
        case 'C':
            OB_EMIT('i');
            break;
        case 's':
        case 'S':
            OB_EMIT('p');
            break;
        case 'p':
            OB_EMIT('L');
            break;
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            if (len == 'L') {
                *why = "a long double conversion (%L)";
                return -1;
            }
            OB_EMIT('d');
            break;
        case 'n':
            *why = "%n, which writes through its argument";
            return -1;
        case '@':
            if (dialect == OCERZ_OBJC_FMT_C) {
                *why = "%@, which a C format does not have";
                return -1;
            }
            OB_EMIT('p');
            break;
        case 'K':
            if (dialect != OCERZ_OBJC_FMT_PREDICATE) {
                *why = "%K, which only a predicate format has";
                return -1;
            }
            OB_EMIT('p');
            break;
        case '\0':
            return (int)n;
        default:
            *why = "a conversion this format dialect does not have";
            return -1;
        }
    }
#undef OB_EMIT
    return (int)n;
}

static const OcerzObjcVariadic g_ob_variadic[] = {
    { "stringWithFormat:",                     OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF, 0 },
    { "localizedStringWithFormat:",            OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF, 0 },
    { "initWithFormat:",                       OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF, 0 },
    { "initWithFormat:locale:",                OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF, 0 },
    { "appendFormat:",                         OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF, 0 },
    { "stringByAppendingFormat:",              OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF, 0 },
    { "stringWithValidatedFormat:validFormatSpecifiers:error:",
                                               OCERZ_OBJC_VA_FORMAT, 3, OCERZ_OBJC_FMT_CF, 0 },
    { "localizedStringWithValidatedFormat:validFormatSpecifiers:error:",
                                               OCERZ_OBJC_VA_FORMAT, 3, OCERZ_OBJC_FMT_CF, 0 },
    { "initWithValidatedFormat:validFormatSpecifiers:error:",
                                               OCERZ_OBJC_VA_FORMAT, 3, OCERZ_OBJC_FMT_CF, 0 },
    { "initWithValidatedFormat:validFormatSpecifiers:locale:error:",
                                               OCERZ_OBJC_VA_FORMAT, 3, OCERZ_OBJC_FMT_CF, 0 },
    { "localizedAttributedStringWithFormat:",  OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF, 1 },
    { "appendLocalizedFormat:",                OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_CF, 1 },
    { "raise:format:",                         OCERZ_OBJC_VA_FORMAT, 3, OCERZ_OBJC_FMT_CF, 0 },
    { "handleFailureInMethod:object:file:lineNumber:description:",
                                               OCERZ_OBJC_VA_FORMAT, 6, OCERZ_OBJC_FMT_CF, 0 },
    { "handleFailureInFunction:file:lineNumber:description:",
                                               OCERZ_OBJC_VA_FORMAT, 5, OCERZ_OBJC_FMT_CF, 0 },
    { "predicateWithFormat:",                  OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_PREDICATE, 0 },
    { "expressionWithFormat:",                 OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_PREDICATE, 0 },
    { "encodeValuesOfObjCTypes:",              OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_TYPES, 0 },
    { "decodeValuesOfObjCTypes:",              OCERZ_OBJC_VA_FORMAT, 2, OCERZ_OBJC_FMT_TYPES, 0 },
    { "arrayWithObjects:",                     OCERZ_OBJC_VA_NIL_TERMINATED, 2, 0, 0 },
    { "initWithObjects:",                      OCERZ_OBJC_VA_NIL_TERMINATED, 2, 0, 0 },
    { "setWithObjects:",                       OCERZ_OBJC_VA_NIL_TERMINATED, 2, 0, 0 },
    { "orderedSetWithObjects:",                OCERZ_OBJC_VA_NIL_TERMINATED, 2, 0, 0 },
    { "dictionaryWithObjectsAndKeys:",         OCERZ_OBJC_VA_NIL_TERMINATED, 2, 0, 0 },
    { "initWithObjectsAndKeys:",               OCERZ_OBJC_VA_NIL_TERMINATED, 2, 0, 0 },
};

const OcerzObjcVariadic *ocerz_objc_variadic(const char *sel)
{
    if (!sel)
        return NULL;
    for (size_t i = 0; i < sizeof g_ob_variadic / sizeof g_ob_variadic[0]; i++)
        if (strcmp(g_ob_variadic[i].sel, sel) == 0)
            return &g_ob_variadic[i];
    return NULL;
}

typedef struct ObShape {
    struct ObShape *next;
    OcerzAbiSig sig;
    char notation[];
} ObShape;

typedef struct ObSend {
    struct ObSend *next;
    void *cls;
    void *sel;
    const ObShape *shape;
    const OcerzObjcVariadic *variadic;
    uint32_t blocks;
    uint32_t fnptrs;
} ObSend;

static ObShape *_Atomic g_ob_shapes[OB_SHAPE_BUCKETS];
static ObSend *_Atomic g_ob_sends[OB_SEND_BUCKETS];
static pthread_mutex_t g_ob_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned ob_str_hash(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static unsigned ob_send_hash(void *cls, void *sel)
{
    uint64_t k = ((uint64_t)(uintptr_t)cls >> 3) ^ (((uint64_t)(uintptr_t)sel >> 3) * 0x9e3779b97f4a7c15ull);
    return (unsigned)((k ^ (k >> 29)) & (OB_SEND_BUCKETS - 1));
}

static const ObShape *ob_shape(const char *notation)
{
    unsigned b = ob_str_hash(notation) & (OB_SHAPE_BUCKETS - 1);
    for (const ObShape *s = g_ob_shapes[b]; s; s = s->next)
        if (strcmp(s->notation, notation) == 0)
            return s;

    size_t len = strlen(notation);
    ObShape *made = calloc(1, sizeof *made + len + 1);
    if (!made)
        return NULL;
    memcpy(made->notation, notation, len + 1);
    if (ocerz_abi_parse(made->notation, &made->sig) != OCERZ_OK) {
        free(made);
        return NULL;
    }

    pthread_mutex_lock(&g_ob_lock);
    ObShape *found = NULL;
    for (ObShape *s = g_ob_shapes[b]; s && !found; s = s->next)
        if (strcmp(s->notation, notation) == 0)
            found = s;
    if (!found) {
        made->next = g_ob_shapes[b];
        g_ob_shapes[b] = made;
        found = made;
        made = NULL;
    }
    pthread_mutex_unlock(&g_ob_lock);
    free(made);
    return found;
}

static const ObSend *ob_cached(void *cls, void *sel)
{
    for (const ObSend *e = g_ob_sends[ob_send_hash(cls, sel)]; e; e = e->next)
        if (e->cls == cls && e->sel == sel)
            return e;
    return NULL;
}

static const ObSend *ob_remember(const ObSend *scratch)
{
    unsigned b = ob_send_hash(scratch->cls, scratch->sel);
    ObSend *made = malloc(sizeof *made);
    if (!made)
        return scratch;
    *made = *scratch;

    pthread_mutex_lock(&g_ob_lock);
    ObSend *found = NULL;
    for (ObSend *e = g_ob_sends[b]; e && !found; e = e->next)
        if (e->cls == scratch->cls && e->sel == scratch->sel)
            found = e;
    if (!found) {
        made->next = g_ob_sends[b];
        g_ob_sends[b] = made;
        found = made;
        made = NULL;
    }
    pthread_mutex_unlock(&g_ob_lock);
    free(made);
    return found;
}

static void ob_describe(void *cls, void *sel, const char *enc, const char *source, ObSend *out)
{
    char notation[OCERZ_OBJC_NOTATION_MAX];
    int nargs = 0;
    uint32_t blocks = 0, fnptrs = 0;
    int rc = ocerz_objc_notation(enc, notation, sizeof notation, &nargs, &blocks, &fnptrs);
    if (rc != OCERZ_OBJC_OK)
        ob_refuse(cls, sel, "cannot cross: its %s type encoding %s has %s", source,
                  enc ? enc : "(none)", ocerz_objc_refusal(rc));

    const ObShape *shape = ob_shape(notation);
    if (!shape)
        ob_refuse(cls, sel, "cannot cross: its %s type encoding %s gives the notation %s, which the ABI"
                  " engine refuses", source, enc, notation);

    const OcerzObjcVariadic *v = ocerz_objc_variadic(ob_sel_getName(sel));
    if (v) {
        const OcerzAbiSig *sig = &shape->sig;
        int arg = v->kind == OCERZ_OBJC_VA_NIL_TERMINATED ? sig->nargs - 1 : v->arg;
        if (arg < 2 || arg >= sig->nargs || sig->arg[arg] != 'p')
            v = NULL;
    }

    memset(out, 0, sizeof *out);
    out->cls = cls;
    out->sel = sel;
    out->shape = shape;
    out->variadic = v;
    out->blocks = blocks;
    out->fnptrs = fnptrs;
}

static const ObSend *ob_method(void *cls, void *sel, ObSend *scratch)
{
    const ObSend *e = ob_cached(cls, sel);
    if (e)
        return e;
    void *m = ob_class_getInstanceMethod(cls, sel);
    if (!m)
        return NULL;
    ob_describe(cls, sel, ob_method_getTypeEncoding(m), "method", scratch);
    return ob_remember(scratch);
}

static void ob_append(char *buf, size_t cap, const char *s, void *cls, void *sel)
{
    size_t have = strlen(buf), add = s ? strlen(s) : 0;
    if (have + add + 1 > cap)
        ob_refuse(cls, sel, "has a forwarded method signature longer than ocerz reads");
    memcpy(buf + have, s, add + 1);
}

static const ObSend *ob_forwarded(void *recv, void *cls, void *sel, ObSend *scratch)
{
    void *msfs = ob_sel_registerName("methodSignatureForSelector:");
    if (!ob_class_respondsToSelector(cls, msfs))
        ob_refuse(cls, sel, "has no method, and the receiver does not answer methodSignatureForSelector:,"
                  " so there is no signature to forward it under");

    void *send = ob_need(&g_ob_msgSend);
    void *ms = ((void *(*)(void *, void *, void *))send)(recv, msfs, sel);
    if (!ms)
        ob_refuse(cls, sel, "is not a selector the receiver recognizes: there is no method and"
                  " methodSignatureForSelector: answers nil");

    const char *rt = ((const char *(*)(void *, void *))send)(ms, ob_sel_registerName("methodReturnType"));
    unsigned long n = ((unsigned long (*)(void *, void *))send)(ms, ob_sel_registerName("numberOfArguments"));
    void *at = ob_sel_registerName("getArgumentTypeAtIndex:");
    char enc[1024];
    enc[0] = '\0';
    ob_append(enc, sizeof enc, rt, cls, sel);
    for (unsigned long i = 0; i < n; i++)
        ob_append(enc, sizeof enc, ((const char *(*)(void *, void *, unsigned long))send)(ms, at, i), cls, sel);
    ob_describe(cls, sel, enc, "forwarded", scratch);
    return scratch;
}

static uint64_t ob_named(const OcerzAbiSig *sig, const OcerzCPU *cpu, int k, char cls)
{
    OcerzAbiSig head = *sig;
    OcerzAbiVaList va;
    uint64_t v = 0;
    head.nargs = k;
    if (ocerz_abi_va_start(&head, cpu, &va) == OCERZ_OK)
        ocerz_abi_va_arg(&va, cpu, cls, &v);
    return v;
}

typedef struct ObText {
    const char *s;
    char *heap;
    char local[512];
} ObText;

static void ob_text(void *str, ObText *t, const char *what)
{
    t->heap = NULL;
    t->local[0] = '\0';
    t->s = t->local;
    if (!str)
        return;
    long len = ((long (*)(void *))ob_need(&g_ob_CFStringGetLength))(str);
    long max = ((long (*)(long, uint32_t))ob_need(&g_ob_CFStringGetMaximumSizeForEncoding))(len, OB_UTF8);
    if (max < 0)
        ob_stop("%s: a format string of %ld characters is too long to read", what, len);
    max++;
    char *buf = t->local;
    if (max > (long)sizeof t->local) {
        buf = t->heap = malloc((size_t)max);
        if (!buf)
            ob_stop("%s: no memory to read a format string of %ld characters", what, len);
    }
    if (!((bool (*)(void *, char *, long, uint32_t))ob_need(&g_ob_CFStringGetCString))(str, buf, max, OB_UTF8))
        ob_stop("%s: its format string does not convert to UTF-8", what);
    t->s = buf;
}

static void ob_text_free(ObText *t)
{
    free(t->heap);
    t->heap = NULL;
}

static int ob_gather_format(const char *what, const char *text, int dialect, const OcerzAbiSig *named,
                            const OcerzCPU *cpu, uint64_t *slots)
{
    char classes[OCERZ_OBJC_VARIADIC_MAX + 1];
    const char *why = NULL;
    int n = ocerz_objc_format_classes(text, dialect, classes, sizeof classes, &why);
    if (n < 0)
        ob_stop("%s refuses the format \"%.200s\": it has %s", what, text, why);

    OcerzAbiVaList va;
    if (ocerz_abi_va_start(named, cpu, &va) != OCERZ_OK)
        ob_stop("%s: the ABI engine cannot find where the variadic arguments begin", what);
    for (int k = 0; k < n; k++)
        ocerz_abi_va_arg(&va, cpu, classes[k], &slots[k]);
    return n;
}

static int ob_perform(OcerzCPU *cpu, const OcerzAbiSig *sig, const void *fn, const uint64_t *slots,
                      int nslots, int as_va_list, const char *what)
{
    OcerzAbiCall call;
    uint64_t stack[OCERZ_ABI_MAX_STACK + OCERZ_OBJC_VARIADIC_MAX];

    if (ocerz_abi_read_guest(sig, cpu, &call) != OCERZ_OK)
        ob_stop("%s: the ABI engine cannot read the guest's arguments", what);

    size_t words = (size_t)call.nstack;
    memcpy(stack, call.stack, words * 8);
    if (as_va_list) {
        if (call.nx >= 8)
            ob_stop("%s: the va_list has no argument register left", what);
        call.x[call.nx++] = (uint64_t)(uintptr_t)slots;
    } else if (nslots > 0) {
        if (words + (size_t)nslots > sizeof stack / sizeof stack[0])
            ob_stop("%s: %d variadic arguments do not fit the native stack ocerz builds", what, nslots);
        memcpy(stack + words, slots, (size_t)nslots * 8);
        words += (size_t)nslots;
    }

    int round = fegetround();
    fesetround(FE_TONEAREST);
    ocerz_abi_call_native(fn, call.x, call.v, stack, words * 8, call.x8, call.rx, call.rv);
    int err = errno;
    fesetround(round);

    ocerz_abi_write_result(sig, cpu, &call);
    return err;
}

typedef enum { OB_PLAIN, OB_SUPER, OB_SUPER2 } ObKind;

static const char *const g_ob_export[3][2] = {
    { "_objc_msgSend", "_objc_msgSend_stret" },
    { "_objc_msgSendSuper", "_objc_msgSendSuper_stret" },
    { "_objc_msgSendSuper2", "_objc_msgSendSuper2_stret" },
};

static int ob_nil(struct OcerzVM *vm, OcerzCPU *cpu, int stret, uint64_t size)
{
    uint64_t result = cpu->gpr[OCERZ_RDI];
    if (stret && size && result)
        memset(ocerz_g2h(result), 0, (size_t)size);
    cpu->gpr[OCERZ_RDX] = 0;
    cpu->xmm[0].lo = 0;
    cpu->xmm[0].hi = 0;
    cpu->xmm[1].lo = 0;
    cpu->xmm[1].hi = 0;
    ob_return(cpu, stret ? result : 0);
    return ob_settle(vm, cpu);
}

static void ob_check_callables(void *cls, void *sel, const ObSend *send, const OcerzCPU *cpu)
{
    const OcerzAbiSig *sig = &send->shape->sig;
    for (int k = 0; k < sig->nargs && k < 32; k++) {
        uint32_t bit = 1u << k;
        if (!((send->blocks | send->fnptrs) & bit))
            continue;
        uint64_t v = ob_named(sig, cpu, k, 'L');
        if (!v)
            continue;
        if (send->blocks & bit) {
            uint64_t invoke = ocerz_ld(v + 16, 8);
            if (ocerz_abi_is_guest_code(invoke))
                ob_refuse(cls, sel, "cannot cross: argument %d is a block whose code is x86 (%#llx), and"
                          " blocks do not cross yet", k - 2, (unsigned long long)invoke);
        } else if (ocerz_abi_is_guest_code(v)) {
            ob_refuse(cls, sel, "cannot cross: argument %d is an x86 function pointer (%#llx), which native"
                      " code cannot call and no encoding gives a signature for", k - 2, (unsigned long long)v);
        }
    }
}

static int ob_send(struct OcerzVM *vm, OcerzCPU *cpu, ObKind kind, int stret)
{
    const char *export = g_ob_export[kind][stret];
    uint64_t first = cpu->gpr[stret ? OCERZ_RSI : OCERZ_RDI];
    void *sel = (void *)(uintptr_t)cpu->gpr[stret ? OCERZ_RDX : OCERZ_RSI];
    ObSym *host = kind == OB_PLAIN ? &g_ob_msgSend : kind == OB_SUPER ? &g_ob_msgSendSuper
                                                                      : &g_ob_msgSendSuper2;
    void *fn = ob_need(host);
    struct OcerzBridgeFrame outer;
    void *recv, *cls;
    ObSend scratch;
    const ObSend *send;

    ocerz_bridge_raise(&outer, OCERZ_OBJC_LIBOBJC, export, NULL, fn);
    if (kind == OB_PLAIN) {
        recv = first ? ocerz_g2h(first) : NULL;
        cls = recv ? ob_object_getClass(recv) : NULL;
    } else {
        if (!first)
            ob_stop("%s was handed a null struct objc_super", export);
        uint64_t r = ocerz_ld(first, 8), c = ocerz_ld(first + 8, 8);
        recv = r ? ocerz_g2h(r) : NULL;
        cls = c ? ocerz_g2h(c) : NULL;
        if (kind == OB_SUPER2 && cls)
            cls = ob_class_getSuperclass(cls);
    }

    if (!recv) {
        uint64_t size = 0;
        if (stret && cls && (send = ob_method(cls, sel, &scratch)) != NULL &&
            send->shape->sig.ret == '{')
            size = send->shape->sig.ret_struct.size;
        ocerz_bridge_lower(&outer);
        return ob_nil(vm, cpu, stret, size);
    }
    if (!cls)
        ob_stop("%s: a super send names no class to start its lookup at", export);

    send = ob_method(cls, sel, &scratch);
    if (!send)
        send = ob_forwarded(recv, cls, sel, &scratch);

    const OcerzAbiSig *sig = &send->shape->sig;
    int memory = sig->ret == '{' && sig->ret_struct.size > OB_SMALL_STRUCT;
    if (stret && !memory)
        ob_refuse(cls, sel, "was sent with %s, but its result under %s is not one System V returns in"
                  " memory", export + 1, send->shape->notation);
    if (!stret && memory)
        ob_refuse(cls, sel, "returns a structure System V returns in memory (%s), and the guest sent it"
                  " with %s, which passes no result pointer", send->shape->notation, export + 1);
    if (send->blocks | send->fnptrs)
        ob_check_callables(cls, sel, send, cpu);

    const char *selname = ob_sel_getName(sel);
    ocerz_bridge_lower(&outer);
    ocerz_bridge_raise(&outer, OCERZ_OBJC_LIBOBJC, selname, send->shape->notation, fn);

    if (ob_logging())
        fprintf(stderr, "ocerz: OBJCLOG[%d] %c[%s %s] %s%s%s\n", (int)getpid(),
                ob_class_isMetaClass(cls) ? '+' : '-', ob_class_getName(cls), selname,
                send->shape->notation, send->variadic ? " variadic" : "", send == &scratch ? " forwarded" : "");

    uint64_t slots[OCERZ_OBJC_VARIADIC_MAX];
    int nslots = 0;
    const OcerzObjcVariadic *v = send->variadic;
    if (v && v->kind == OCERZ_OBJC_VA_NIL_TERMINATED) {
        if (ob_named(sig, cpu, sig->nargs - 1, 'p')) {
            OcerzAbiVaList va;
            if (ocerz_abi_va_start(sig, cpu, &va) != OCERZ_OK)
                ob_refuse(cls, sel, "cannot cross: the ABI engine cannot find its variadic arguments");
            do {
                if (nslots == OCERZ_OBJC_VARIADIC_MAX)
                    ob_refuse(cls, sel, "cannot cross: no nil among its first %d variadic arguments",
                              OCERZ_OBJC_VARIADIC_MAX);
                ocerz_abi_va_arg(&va, cpu, 'p', &slots[nslots]);
            } while (slots[nslots++] != 0);
        }
    } else if (v) {
        uint64_t fmt = ob_named(sig, cpu, v->arg, 'p');
        ObText text;
        char what[160];
        snprintf(what, sizeof what, "%c[%s %s]", ob_class_isMetaClass(cls) ? '+' : '-',
                 ob_class_getName(cls), selname);
        if (v->dialect == OCERZ_OBJC_FMT_TYPES) {
            text.heap = NULL;
            text.s = fmt ? (const char *)(uintptr_t)fmt : "";
        } else {
            void *obj = (void *)(uintptr_t)fmt;
            if (v->attributed && obj)
                obj = ((void *(*)(void *, void *))ob_need(&g_ob_msgSend))(obj, ob_sel_registerName("string"));
            ob_text(obj, &text, what);
        }
        nslots = ob_gather_format(what, text.s, v->dialect, sig, cpu, slots);
        ob_text_free(&text);
    }

    ob_perform(cpu, sig, fn, slots, nslots, 0, selname);
    ocerz_bridge_lower(&outer);
    return ob_settle(vm, cpu);
}

int ocerz_objc_msgSend(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_send(vm, cpu, OB_PLAIN, 0);
}

int ocerz_objc_msgSendSuper(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_send(vm, cpu, OB_SUPER, 0);
}

int ocerz_objc_msgSendSuper2(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_send(vm, cpu, OB_SUPER2, 0);
}

int ocerz_objc_msgSend_stret(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_send(vm, cpu, OB_PLAIN, 1);
}

int ocerz_objc_msgSendSuper_stret(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_send(vm, cpu, OB_SUPER, 1);
}

int ocerz_objc_msgSendSuper2_stret(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_send(vm, cpu, OB_SUPER2, 1);
}

static _Noreturn void ob_long_double_send(OcerzCPU *cpu, const char *export, const char *what)
{
    uint64_t r = cpu->gpr[OCERZ_RDI];
    void *recv = r ? ocerz_g2h(r) : NULL;
    void *sel = (void *)(uintptr_t)cpu->gpr[OCERZ_RSI];
    ob_refuse(recv ? ob_object_getClass(recv) : NULL, sel,
              "was sent with %s, which x86-64 uses only for a %s result, and that does not cross", export,
              what);
}

int ocerz_objc_msgSend_fpret(struct OcerzVM *vm, OcerzCPU *cpu)
{
    ob_long_double_send(cpu, "objc_msgSend_fpret", "long double");
}

int ocerz_objc_msgSend_fp2ret(struct OcerzVM *vm, OcerzCPU *cpu)
{
    ob_long_double_send(cpu, "objc_msgSend_fp2ret", "long double _Complex");
}

typedef struct ObVeneer {
    const char *sym;
    ObSym vform;
    const char *named;
    const char *sig;
    int fmt;
    int dialect;
} ObVeneer;

static ObVeneer g_ob_printf = {
    "_printf", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vprintf"), "i(p)", "i(pp)", 0, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_fprintf = {
    "_fprintf", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vfprintf"), "i(pp)", "i(ppp)", 1, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_sprintf = {
    "_sprintf", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vsprintf"), "i(pp)", "i(ppp)", 1, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_snprintf = {
    "_snprintf", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vsnprintf"), "i(pLp)", "i(pLpp)", 2, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_asprintf = {
    "_asprintf", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vasprintf"), "i(pp)", "i(ppp)", 1, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_dprintf = {
    "_dprintf", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vdprintf"), "i(ip)", "i(ipp)", 1, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_sprintf_chk = {
    "___sprintf_chk", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "__vsprintf_chk"), "i(piLp)", "i(piLpp)", 3,
    OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_snprintf_chk = {
    "___snprintf_chk", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "__vsnprintf_chk"), "i(pLiLp)", "i(pLiLpp)", 4,
    OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_NSLog = {
    "_NSLog", OB_SYM(OCERZ_OBJC_FOUNDATION, "NSLogv"), "v(p)", "v(pp)", 0, OCERZ_OBJC_FMT_CF,
};
static ObVeneer g_ob_CFStringCreateWithFormat = {
    "_CFStringCreateWithFormat", OB_SYM(OCERZ_BRIDGE_COREFOUNDATION, "CFStringCreateWithFormatAndArguments"),
    "p(ppp)", "p(pppp)", 2, OCERZ_OBJC_FMT_CF,
};
static ObVeneer g_ob_CFStringAppendFormat = {
    "_CFStringAppendFormat", OB_SYM(OCERZ_BRIDGE_COREFOUNDATION, "CFStringAppendFormatAndArguments"),
    "v(ppp)", "v(pppp)", 2, OCERZ_OBJC_FMT_CF,
};

static int ob_veneer(struct OcerzVM *vm, OcerzCPU *cpu, ObVeneer *vn)
{
    OcerzAbiSig named;
    if (ocerz_abi_parse(vn->named, &named) != OCERZ_OK)
        ob_stop("%s is declared %s, which the ABI engine refuses", vn->sym, vn->named);
    void *vfn = ob_need(&vn->vform);

    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, vn->vform.lib, vn->sym, vn->sig, vfn);

    uint64_t fmt = ob_named(&named, cpu, vn->fmt, 'p');
    ObText text;
    if (vn->dialect == OCERZ_OBJC_FMT_C) {
        text.heap = NULL;
        text.s = fmt ? (const char *)(uintptr_t)fmt : "";
    } else {
        ob_text((void *)(uintptr_t)fmt, &text, vn->sym);
    }

    uint64_t slots[OCERZ_OBJC_VARIADIC_MAX];
    int n = ob_gather_format(vn->sym, text.s, vn->dialect, &named, cpu, slots);
    ob_text_free(&text);

    int err = ob_perform(cpu, &named, vfn, slots, n, 1, vn->sym);
    ocerz_bridge_lower(&outer);
    errno = err;
    return ob_settle(vm, cpu);
}

int ocerz_fmt_printf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_printf); }
int ocerz_fmt_fprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_fprintf); }
int ocerz_fmt_sprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_sprintf); }
int ocerz_fmt_snprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_snprintf); }
int ocerz_fmt_asprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_asprintf); }
int ocerz_fmt_dprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_dprintf); }
int ocerz_fmt_sprintf_chk(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_sprintf_chk); }
int ocerz_fmt_snprintf_chk(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_snprintf_chk); }
int ocerz_fmt_NSLog(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_NSLog); }

int ocerz_fmt_CFStringCreateWithFormat(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_veneer(vm, cpu, &g_ob_CFStringCreateWithFormat);
}

int ocerz_fmt_CFStringAppendFormat(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_veneer(vm, cpu, &g_ob_CFStringAppendFormat);
}

static void *_Atomic g_ob_native_prev;
static _Atomic uint64_t g_ob_guest_handler;
static pthread_once_t g_ob_uncaught_once = PTHREAD_ONCE_INIT;

static void ob_exception_text(void *exc, const char *selname, char *buf, size_t len)
{
    snprintf(buf, len, "(none)");
    void *sel = ob_sel_registerName(selname);
    void *cls = ob_object_getClass(exc);
    if (!ob_class_respondsToSelector(cls, sel)) {
        snprintf(buf, len, "(a %s)", ob_class_getName(cls));
        return;
    }
    void *str = ((void *(*)(void *, void *))ob_need(&g_ob_msgSend))(exc, sel);
    if (!str) {
        snprintf(buf, len, "(nil)");
        return;
    }
    ObText t;
    ob_text(str, &t, "the uncaught exception handler");
    snprintf(buf, len, "%s", t.s);
    ob_text_free(&t);
}

static void ob_uncaught(void *exc)
{
    char name[256], reason[1024];
    const struct OcerzBridgeFrame *f = ocerz_bridge_in_flight();

    if (exc) {
        ob_exception_text(exc, "name", name, sizeof name);
        ob_exception_text(exc, "reason", reason, sizeof reason);
    } else {
        snprintf(name, sizeof name, "(nil)");
        snprintf(reason, sizeof reason, "(nil)");
    }
    fprintf(stderr, "ocerz: bridge: uncaught Objective-C exception %s: %s during %s\n", name, reason,
            f && f->sym ? f->sym : "(no bridged call)");
    fflush(stderr);

    void (*prev)(void *) = g_ob_native_prev;
    if (prev)
        prev(exc);
}

static void ob_install_once(void)
{
    void *set = ob_sym(&g_ob_setUncaught);
    if (!set) {
        OCERZ_LOG("objc: the host libobjc has no objc_setUncaughtExceptionHandler\n");
        return;
    }
    void *prev = ((void *(*)(void *))set)((void *)ob_uncaught);
    if (prev != (void *)ob_uncaught)
        g_ob_native_prev = prev;
}

void ocerz_objcbridge_install_uncaught(void)
{
    pthread_once(&g_ob_uncaught_once, ob_install_once);
}

int ocerz_objc_setUncaughtExceptionHandler(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t guest = cpu->gpr[OCERZ_RDI];
    void *set = ob_need(&g_ob_setUncaught);
    uint64_t native = 0;

    ocerz_objcbridge_install_uncaught();
    if (guest && (ocerz_abi_callback_convert(guest, "v(p)", &native) != OCERZ_OK || !native))
        ob_stop("objc_setUncaughtExceptionHandler could not bind guest handler %#llx to a callback",
                (unsigned long long)guest);

    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, OCERZ_OBJC_LIBOBJC, "_objc_setUncaughtExceptionHandler", "p(c{v(p)})", set);
    ((void *(*)(void *))set)(guest ? ocerz_g2h(native) : (void *)ob_uncaught);
    ocerz_bridge_lower(&outer);

    ob_return(cpu, atomic_exchange(&g_ob_guest_handler, guest));
    return ob_settle(vm, cpu);
}

int ocerz_objcbridge_fix_selrefs(const uint8_t *mh, int64_t slide)
{
    struct mach_header_64 h;
    if (!mh)
        return 0;
    memcpy(&h, mh, sizeof h);
    if (h.magic != MH_MAGIC_64)
        return 0;

    int rewritten = 0;
    const uint8_t *lc = mh + sizeof h;
    for (uint32_t i = 0; i < h.ncmds; i++) {
        struct load_command l;
        memcpy(&l, lc, sizeof l);
        if (l.cmdsize < sizeof l)
            break;
        if (l.cmd == LC_SEGMENT_64) {
            struct segment_command_64 seg;
            memcpy(&seg, lc, sizeof seg);
            for (uint32_t s = 0; strncmp(seg.segname, "__DATA", 6) == 0 && s < seg.nsects; s++) {
                struct section_64 sc;
                memcpy(&sc, lc + sizeof seg + (size_t)s * sizeof sc, sizeof sc);
                uint64_t stride, at;
                if (strncmp(sc.sectname, "__objc_selrefs", sizeof sc.sectname) == 0) {
                    stride = 8;
                    at = 0;
                } else if (strncmp(sc.sectname, "__objc_msgrefs", sizeof sc.sectname) == 0) {
                    stride = 16;
                    at = 8;
                } else {
                    continue;
                }
                uint64_t base = (uint64_t)((int64_t)sc.addr + slide);
                for (uint64_t off = 0; off + stride <= sc.size; off += stride) {
                    uint64_t word = base + off + at;
                    uint64_t name = ocerz_ld(word, 8);
                    if (!name)
                        continue;
                    void *reg = ob_sym(&g_ob_sel_registerName);
                    if (!reg) {
                        OCERZ_LOG("objc: the host libobjc has no sel_registerName, so the selectors of the"
                                  " image at %p stay the image's own\n", (void *)mh);
                        return -1;
                    }
                    void *sel = ((void *(*)(const char *))reg)((const char *)ocerz_g2h(name));
                    uint64_t canon = ocerz_h2g(sel);
                    if (canon != name) {
                        ocerz_st(word, 8, canon);
                        rewritten++;
                    }
                }
            }
        }
        lc += l.cmdsize;
    }
    return rewritten;
}

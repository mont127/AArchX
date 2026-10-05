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
 * that is about to run the method is asked for it.  A class the guest itself
 * defines is made the native runtime's own before any guest code runs
 * (src/objcclass.c), so a send to a guest object is the same crossing, and the
 * native objc_msgSend it ends in reaches the guest's method through a callback
 * slot.
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
 * A block, @? as an argument or a result, becomes class k with empty braces,
 * k{}, because an encoding says nothing about a block's own arguments and the
 * block carries its signature itself; the ABI engine hands native code a
 * wrapper of a guest block and guest code a view of a native one
 * (ocerz/blocks.h), and the same notation is what a guest method's
 * implementation is bound under, so a native caller's block reaches a guest
 * method as a block the guest can call.  A @? inside a structure stays a
 * pointer.  A block result of a send is borrowed, as a getter's is.  A function
 * pointer, ^?, crosses as a pointer when it is null or native; a guest one is
 * x86 code with no signature an encoding could give it, so it stops the send by
 * name, since native code calling it would jump into x86 bytes.  The exception
 * is a selector whose documentation fixes the function's type, the comparator
 * of sortSubviewsUsingFunction:context: and the sorted-array methods, which
 * g_ob_fnargs lists: there the argument is written as a callback with that
 * signature, and the guest function crosses as a trampoline.  A guest stack
 * block sent -copy, which manual reference counting does, is copied the way
 * _Block_copy copies one, guest-side, since the native method would run its
 * x86 copy helper.
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
 * the arguments where the signature puts them.  The runtime first asks the
 * receiver forwardingTargetForSelector:, and so does the send: a target that is
 * neither nil nor the receiver itself is where the runtime will resend the
 * message with the same arguments, so the signature is the target's method's,
 * and a target with no method of that name is asked the same two questions in
 * turn, up to eight targets deep.  The receiver therefore answers
 * forwardingTargetForSelector: twice, once to the send and once to the
 * runtime.  With no target, the signature is the receiver's own answer to
 * methodSignatureForSelector:, called through native objc_msgSend, with its
 * methodReturnType and each getArgumentTypeAtIndex: concatenated into an
 * encoding.  Either answer can depend on the instance, as a proxy's does, so
 * both are asked on every forwarded send and never cached.  A nil answer is an
 * unrecognized selector, which natively raises an exception that nothing in
 * the guest can catch, so it stops the send naming the class and the selector.
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
 * __snprintf_chk, warn, err and their kin, NSLog, CFStringCreateWithFormat and
 * CFStringAppendFormat are veneers over their v forms, which every one of them
 * has, and the v forms themselves, NSLogv and the CoreFoundation ...AndArguments
 * calls included, take the guest's va_list the same way.  The named arguments
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
 * The frames between an Objective-C throw and its @catch are the guest's, x86
 * frames on the guest's stack, which only an x86 unwinder can walk, so the
 * guest's exceptions are the guest's C++ runtime's: runtime/guest's libc++abi
 * and libunwind, which binding any of libobjc's exception calls loads
 * (src/dyld.c).  Apple's open-source libobjc builds its exceptions on the C++
 * runtime in the same way, and these follow it.  objc_exception_throw retains
 * the object, allocates a 32-byte exception, the object and then a type_info
 * naming its class, with __cxa_allocate_exception, and enters __cxa_throw with a
 * destructor that releases the object; objc_exception_rethrow,
 * objc_begin_catch, objc_end_catch, objc_terminate and __objc_personality_v0
 * enter __cxa_rethrow, __cxa_begin_catch, __cxa_end_catch, std::terminate and
 * __gxx_personality_v0 with the guest's registers as they are.  Each jumps
 * rather than calls, so no host frame is left under the guest's unwinding.
 *
 * The C++ runtime matches a @catch clause by calling the can_catch entry of the
 * clause's type_info vtable.  Every type the guest can name has a vtable of
 * guest memory: objc_ehtype_vtable, which the compiler points the types it emits
 * for a guest's own classes at, and _OBJC_EHTYPE_id and the _OBJC_EHTYPE_$_
 * exports of CoreFoundation and CloudKit, which are variables of the
 * synthesized images built around one shared vtable page, since the host's are
 * arm64 objects with arm64 entries (src/vdylib.c).  Their entries lead to
 * ocerz's trampolines: can_catch answers whether the thrown object's class is
 * the clause's or a subclass of it, a class of zero meaning id, and the rest
 * answer false.  A thrown type is recognized by that vtable, so a C++
 * exception never matches an Objective-C clause.  Guest classes keep their
 * addresses in the native runtime, so a clause naming one compares directly.
 *
 * An exception native code raises unwinds the host stack, where above the
 * native frames lie the send handler and ocerz's own frames.  Each send runs
 * its native call inside ocerz_objc_guarded (src/objcguard.s) whenever the
 * guest's C++ runtime is loaded, a frame whose personality claims any
 * exception, so the native frames' cleanups run on the way to it, and
 * ocerz_objc_guard_landed takes an Objective-C one's object with the host
 * C++ runtime's catch calls and retains it.  The send then throws that object
 * into the guest from the send's own call site, as though objc_msgSend had
 * thrown it.  Any other exception is rethrown past the guard.  An exception
 * that reaches the edge of a callback native code made into the guest, or the
 * end of the guest's frames, is uncaught: the guest's terminate handler, which
 * the first throw installs, hands an Objective-C one to the uncaught-exception
 * handler the guest set, or to ocerz's, and aborts, as the native runtime does.
 * Without runtime/guest the exception calls name themselves, as before.
 *
 * The native runtime's own uncaught-exception path is still there for a native
 * exception raised where nothing guards it.  ocerz_objcbridge_install_uncaught puts ocerz's handler there,
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
 * ---- the Swift runtime ----
 * A Swift guest brings its own runtime: runtime/guest supplies the x86
 * libswiftCore of the swift.org toolchain in place of the host's, so every
 * Swift call stays guest code and only that runtime's Objective-C calls cross.
 * Most are ordinary crossings.  Four kinds are handled here.
 *
 * The runtime installs three hooks, objc_setHook_getClass,
 * objc_setHook_getImageName and objc_setHook_lazyClassNamer, each given the new
 * hook and an address that receives the old one so the new hook can chain to
 * it.  A guest hook is x86 code and the hook it displaces is native, and
 * neither side can call the other's.  So the first time the guest sets one,
 * ocerz installs a native wrapper of its own instead, which calls the guest's
 * hook through a callback slot and falls back to the native hook it displaced;
 * each later set only changes which guest function the wrapper calls.  The
 * guest's old-hook word receives the guest hook set before, or, the first time,
 * a three-byte guest function answering zero (xor eax, eax; ret), which a
 * getClass or getImageName hook reads as no and the namer as no name.  The
 * chain runs through the guest's hooks, newest first, then the native ones.
 *
 * Classes the runtime builds while running, generic instantiations and classes
 * whose metadata initializer has to run first, reach libobjc through
 * objc_readClassPair and _objc_realizeClassFromSwift.  Both specials hand the
 * class to ocerz_objcbridge_prepare_class first, which gives it the method list
 * and class_ro_t copies an image's classes get (src/objcclass.c), and only then
 * make the native call.  _objc_realizeClassFromSwift also takes the address the
 * class was known at before, and that is passed on only when the class had
 * been defined already: one prepared just now was never known to the native
 * runtime at any address.
 *
 * objc_opt_self, objc_opt_class, objc_alloc, objc_alloc_init,
 * objc_allocWithZone and every plain send make sure the receiver's class is
 * defined before the native runtime sees it (ocerz_objcbridge_ensure_object),
 * for the reason src/objcclass.c gives under Swift classes.
 *
 * objc_release is a special too.  On Darwin the Swift runtime frees an
 * AnyObject value with a plain objc_release, and every element of an array of
 * class instances is stored as one; libobjc hands a Swift object on to
 * swift_release.  In native mode that is the native swift_release, which at a
 * count of zero calls the class's destroy function, x86 code.  src/objcclass.c
 * makes that call work through a fault, at a few microseconds each, so a
 * guest's own objc_release of an object ocerz_objcbridge_guest_swift_object
 * recognizes jumps to the guest libswiftCore's swift_release instead, with the
 * guest's return address left where it is, and every other object takes the
 * native call.  A million objects freed through arrays took 5.5 s through the
 * fault and take 130 ms through the jump, against 52 ms under Rosetta.
 * Retains are not routed: both runtimes keep a Swift object's count in the
 * same 64-bit header word, so a native retain, or a release that does not
 * reach zero, is already the right one.
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
#include "ocerz/blocks.h"
#include "ocerz/bridge.h"
#include "ocerz/dyld.h"
#include "ocerz/interp.h"
#include "ocerz/mem.h"
#include "ocerz/syscall.h"
#include "ocerz/vdylib.h"
#include "ocerz/vm.h"

#include <Block.h>
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <mach-o/loader.h>
#include <stddef.h>
#include <unwind.h>

#define OB_SMALL_STRUCT 16
#define OB_SHAPE_BUCKETS 512
#define OB_SEND_BUCKETS 4096
#define OB_UTF8 0x08000100u
#define OB_BLOCK 1
#define OB_FNPTR 2
#define OB_OBJECT 3

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
static ObSym g_ob_class_addMethod = OB_SYM(OCERZ_OBJC_LIBOBJC, "class_addMethod");
static ObSym g_ob_allocateClassPair = OB_SYM(OCERZ_OBJC_LIBOBJC, "objc_allocateClassPair");
static ObSym g_ob_class_copyMethodList = OB_SYM(OCERZ_OBJC_LIBOBJC, "class_copyMethodList");
static ObSym g_ob_method_getName = OB_SYM(OCERZ_OBJC_LIBOBJC, "method_getName");
static ObSym g_ob_method_setImplementation = OB_SYM(OCERZ_OBJC_LIBOBJC, "method_setImplementation");
static ObSym g_ob_method_getImplementation = OB_SYM(OCERZ_OBJC_LIBOBJC, "method_getImplementation");
static ObSym g_ob_setExceptionPreprocessor = OB_SYM(OCERZ_OBJC_LIBOBJC, "objc_setExceptionPreprocessor");
static ObSym g_ob_class_replaceMethod = OB_SYM(OCERZ_OBJC_LIBOBJC, "class_replaceMethod");
static ObSym g_ob_class_getMethodImplementation = OB_SYM(OCERZ_OBJC_LIBOBJC, "class_getMethodImplementation");
static ObSym g_ob_realizeClassFromSwift = OB_SYM(OCERZ_OBJC_LIBOBJC, "_objc_realizeClassFromSwift");
static ObSym g_ob_readClassPair = OB_SYM(OCERZ_OBJC_LIBOBJC, "objc_readClassPair");
static ObSym g_ob_setHook_getClass = OB_SYM(OCERZ_OBJC_LIBOBJC, "objc_setHook_getClass");
static ObSym g_ob_setHook_getImageName = OB_SYM(OCERZ_OBJC_LIBOBJC, "objc_setHook_getImageName");
static ObSym g_ob_setHook_lazyClassNamer = OB_SYM(OCERZ_OBJC_LIBOBJC, "objc_setHook_lazyClassNamer");
static ObSym g_ob_opt_self = OB_SYM(OCERZ_OBJC_LIBOBJC, "objc_opt_self");
static ObSym g_ob_opt_class = OB_SYM(OCERZ_OBJC_LIBOBJC, "objc_opt_class");
static ObSym g_ob_alloc = OB_SYM(OCERZ_OBJC_LIBOBJC, "objc_alloc");
static ObSym g_ob_alloc_init = OB_SYM(OCERZ_OBJC_LIBOBJC, "objc_alloc_init");
static ObSym g_ob_allocWithZone = OB_SYM(OCERZ_OBJC_LIBOBJC, "objc_allocWithZone");
static ObSym g_ob_release = OB_SYM(OCERZ_OBJC_LIBOBJC, "objc_release");
static ObSym g_ob_retain = OB_SYM(OCERZ_OBJC_LIBOBJC, "objc_retain");
static ObSym g_ob_ehtype_vtable = OB_SYM(OCERZ_OBJC_LIBOBJC, "objc_ehtype_vtable");
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
        if (*p == '@' && p[1] == '?' && where != OB_MEMBER) {
            if (special)
                *special = OB_BLOCK;
            ob_put(o, 'k');
            ob_put(o, '{');
            ob_put(o, '}');
            return q;
        }
        if (special && p[1] == '?')
            *special = *p == '@' ? OB_BLOCK : OB_FNPTR;
        else if (special && *p == '@')
            *special = OB_OBJECT;
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

static int ob_notation(const char *encoding, char *out, size_t outlen, int *nargs, uint32_t *blocks,
                       uint32_t *fnptrs, uint32_t *objects)
{
    char local[OCERZ_OBJC_NOTATION_MAX];
    ObOut o = { out ? out : local, out ? outlen : sizeof local, 0, 0 };
    int rc = OCERZ_OBJC_OK, n = 0;
    uint32_t bmask = 0, fmask = 0, omask = 0;

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
        else if (special == OB_OBJECT)
            omask |= 1u << n;
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
    if (objects)
        *objects = omask;
    return OCERZ_OBJC_OK;
}

int ocerz_objc_notation(const char *encoding, char *out, size_t outlen, int *nargs,
                        uint32_t *blocks, uint32_t *fnptrs)
{
    return ob_notation(encoding, out, outlen, nargs, blocks, fnptrs, NULL);
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
    case OCERZ_OBJC_NOT_METHOD:    return "no self and _cmd as its first two arguments";
    case OCERZ_OBJC_NULL:          return "a null address where a structure belongs";
    case OCERZ_OBJC_BAD_LIST:      return "a list whose entry size ocerz cannot read";
    case OCERZ_OBJC_CYCLE:         return "a class that is its own superclass";
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

static const char *ob_arg_at(const char *notation, int index);

static const struct {
    const char *sel;
    int arg;
    const char *notation;
} g_ob_fnargs[] = {
    { "sortSubviewsUsingFunction:context:",       2, "l(ppp)" },
    { "sortedArrayUsingFunction:context:",        2, "l(ppp)" },
    { "sortedArrayUsingFunction:context:hint:",   2, "l(ppp)" },
    { "sortUsingFunction:context:",               2, "l(ppp)" },
};

static int ob_fnarg_notation(const char *sel, const char *notation, uint32_t *fnptrs, char *out, size_t outlen)
{
    for (size_t i = 0; sel && i < sizeof g_ob_fnargs / sizeof g_ob_fnargs[0]; i++) {
        if (strcmp(g_ob_fnargs[i].sel, sel) != 0)
            continue;
        int arg = g_ob_fnargs[i].arg;
        const char *at = ob_arg_at(notation, arg);
        if (!at || *at != 'p' || !(*fnptrs & (1u << arg)))
            return 0;
        int n = snprintf(out, outlen, "%.*sc{%s}%s", (int)(at - notation), notation, g_ob_fnargs[i].notation, at + 1);
        if (n < 0 || (size_t)n >= outlen)
            return 0;
        *fnptrs &= ~(1u << arg);
        return 1;
    }
    return 0;
}

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
    uint32_t objects;
    uint64_t generation;
} ObSend;

static ObShape *_Atomic g_ob_shapes[OB_SHAPE_BUCKETS];
static ObSend *_Atomic g_ob_sends[OB_SEND_BUCKETS];
static pthread_mutex_t g_ob_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_ob_generation;

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
    uint64_t generation = atomic_load(&g_ob_generation);
    for (const ObSend *e = g_ob_sends[ob_send_hash(cls, sel)]; e; e = e->next)
        if (e->cls == cls && e->sel == sel && e->generation == generation)
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
        if (e->cls == scratch->cls && e->sel == scratch->sel &&
            e->generation == scratch->generation)
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
    uint32_t blocks = 0, fnptrs = 0, objects = 0;
    int rc = ob_notation(enc, notation, sizeof notation, &nargs, &blocks, &fnptrs, &objects);
    if (rc != OCERZ_OBJC_OK)
        ob_refuse(cls, sel, "cannot cross: its %s type encoding %s has %s", source,
                  enc ? enc : "(none)", ocerz_objc_refusal(rc));

    char withfn[OCERZ_OBJC_NOTATION_MAX];
    const char *use = ob_fnarg_notation(ob_sel_getName(sel), notation, &fnptrs, withfn, sizeof withfn) ? withfn
                                                                                                        : notation;
    const ObShape *shape = ob_shape(use);
    if (!shape)
        ob_refuse(cls, sel, "cannot cross: its %s type encoding %s gives the notation %s, which the ABI"
                  " engine refuses", source, enc, use);

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
    out->objects = objects;
}

static const ObSend *ob_method(void *cls, void *sel, ObSend *scratch)
{
    const ObSend *e = ob_cached(cls, sel);
    if (e)
        return e;
    uint64_t generation = atomic_load(&g_ob_generation);
    void *m = ob_class_getInstanceMethod(cls, sel);
    if (!m)
        return NULL;
    ob_describe(cls, sel, ob_method_getTypeEncoding(m), "method", scratch);
    scratch->generation = generation;
    return ob_remember(scratch);
}

int ocerz_objc_allocateClassPair(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *fn = ob_need(&g_ob_allocateClassPair);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, OCERZ_OBJC_LIBOBJC, "_objc_allocateClassPair", "p(ppL)", fn);
    void *cls = ((void *(*)(void *, const char *, size_t))fn)(
        cpu->gpr[OCERZ_RDI] ? ocerz_g2h(cpu->gpr[OCERZ_RDI]) : NULL,
        ocerz_g2h(cpu->gpr[OCERZ_RSI]), (size_t)cpu->gpr[OCERZ_RDX]);
    if (cls)
        atomic_fetch_add(&g_ob_generation, 1);
    ocerz_bridge_lower(&outer);
    ob_return(cpu, cls ? ocerz_h2g(cls) : 0);
    return ob_settle(vm, cpu);
}

static void *ob_imp_from_guest_or_bind(uint64_t imp, const char *notation, void *cls, void *sel,
                                       const char *who)
{
    void *back = ocerz_objc_imp_from_guest(imp);
    if (back)
        return back;
    uint64_t native = 0;
    if (ocerz_abi_callback_convert(imp, notation, &native) != OCERZ_OK || !native)
        ob_refuse(cls, sel, "%s could not bind implementation %#llx", who, (unsigned long long)imp);
    return ocerz_g2h(native);
}

int ocerz_objc_class_addMethod(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *cls = cpu->gpr[OCERZ_RDI] ? ocerz_g2h(cpu->gpr[OCERZ_RDI]) : NULL;
    void *sel = cpu->gpr[OCERZ_RSI] ? ocerz_g2h(cpu->gpr[OCERZ_RSI]) : NULL;
    uint64_t imp = cpu->gpr[OCERZ_RDX];
    const char *types = cpu->gpr[OCERZ_RCX] ? ocerz_g2h(cpu->gpr[OCERZ_RCX]) : NULL;
    void *fn = ob_need(&g_ob_class_addMethod);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, OCERZ_OBJC_LIBOBJC, "_class_addMethod", NULL, fn);
    bool added = false;
    if (cls && sel && imp) {
        unsigned count = 0;
        void **methods = ((void **(*)(void *, unsigned *))ob_need(&g_ob_class_copyMethodList))(cls, &count);
        bool exists = false;
        for (unsigned i = 0; i < count && !exists; i++)
            exists = ((void *(*)(void *))ob_need(&g_ob_method_getName))(methods[i]) == sel;
        free(methods);
        if (exists) {
            ocerz_bridge_lower(&outer);
            ob_return(cpu, 0);
            return ob_settle(vm, cpu);
        }
        char notation[OCERZ_OBJC_NOTATION_MAX];
        int rc = ocerz_objc_method_notation(types, notation, sizeof notation);
        if (rc != OCERZ_OBJC_OK)
            ob_refuse(cls, sel, "class_addMethod cannot cross: %s", ocerz_objc_refusal(rc));
        void *native = ob_imp_from_guest_or_bind(imp, notation, cls, sel, "class_addMethod");
        added = ((bool (*)(void *, void *, void *, const char *))fn)(cls, sel, native, types);
        if (added)
            atomic_fetch_add(&g_ob_generation, 1);
    }
    ocerz_bridge_lower(&outer);
    ob_return(cpu, added);
    return ob_settle(vm, cpu);
}

int ocerz_objc_methodSetImplementation(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *m = cpu->gpr[OCERZ_RDI] ? ocerz_g2h(cpu->gpr[OCERZ_RDI]) : NULL;
    uint64_t imp = cpu->gpr[OCERZ_RSI];
    void *fn = ob_need(&g_ob_method_setImplementation);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, OCERZ_OBJC_LIBOBJC, "_method_setImplementation", NULL, fn);
    void *old = NULL;
    if (m) {
        const char *types = ob_method_getTypeEncoding(m);
        char notation[OCERZ_OBJC_NOTATION_MAX];
        int rc = ocerz_objc_method_notation(types, notation, sizeof notation);
        if (rc != OCERZ_OBJC_OK)
            ob_refuse(NULL, NULL, "method_setImplementation cannot cross: %s", ocerz_objc_refusal(rc));
        void *native = imp ? ob_imp_from_guest_or_bind(imp, notation, NULL, NULL, "method_setImplementation")
                           : NULL;
        old = ((void *(*)(void *, void *))fn)(m, native);
        atomic_fetch_add(&g_ob_generation, 1);
        ocerz_bridge_lower(&outer);
        ob_return(cpu, ocerz_objc_imp_for_guest(old, types));
        return ob_settle(vm, cpu);
    }
    ocerz_bridge_lower(&outer);
    ob_return(cpu, 0);
    return ob_settle(vm, cpu);
}

int ocerz_objc_class_replaceMethod(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *cls = cpu->gpr[OCERZ_RDI] ? ocerz_g2h(cpu->gpr[OCERZ_RDI]) : NULL;
    void *sel = cpu->gpr[OCERZ_RSI] ? ocerz_g2h(cpu->gpr[OCERZ_RSI]) : NULL;
    uint64_t imp = cpu->gpr[OCERZ_RDX];
    const char *types = cpu->gpr[OCERZ_RCX] ? ocerz_g2h(cpu->gpr[OCERZ_RCX]) : NULL;
    void *fn = ob_need(&g_ob_class_replaceMethod);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, OCERZ_OBJC_LIBOBJC, "_class_replaceMethod", NULL, fn);
    uint64_t answer = 0;
    if (cls && sel && imp) {
        void *was = ob_class_getInstanceMethod(cls, sel);
        const char *was_types = was ? ob_method_getTypeEncoding(was) : NULL;
        char notation[OCERZ_OBJC_NOTATION_MAX];
        int rc = ocerz_objc_method_notation(types, notation, sizeof notation);
        if (rc != OCERZ_OBJC_OK)
            ob_refuse(cls, sel, "class_replaceMethod cannot cross: %s", ocerz_objc_refusal(rc));
        void *native = ob_imp_from_guest_or_bind(imp, notation, cls, sel, "class_replaceMethod");
        void *old = ((void *(*)(void *, void *, void *, const char *))fn)(cls, sel, native, types);
        atomic_fetch_add(&g_ob_generation, 1);
        answer = ocerz_objc_imp_for_guest(old, was_types);
    }
    ocerz_bridge_lower(&outer);
    ob_return(cpu, answer);
    return ob_settle(vm, cpu);
}

int ocerz_objc_method_getImplementation(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *m = cpu->gpr[OCERZ_RDI] ? ocerz_g2h(cpu->gpr[OCERZ_RDI]) : NULL;
    void *fn = ob_need(&g_ob_method_getImplementation);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, OCERZ_OBJC_LIBOBJC, "_method_getImplementation", NULL, fn);
    void *imp = m ? ((void *(*)(void *))fn)(m) : NULL;
    uint64_t answer = ocerz_objc_imp_for_guest(imp, m ? ob_method_getTypeEncoding(m) : NULL);
    ocerz_bridge_lower(&outer);
    ob_return(cpu, answer);
    return ob_settle(vm, cpu);
}

int ocerz_objc_class_getMethodImplementation(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *cls = cpu->gpr[OCERZ_RDI] ? ocerz_g2h(cpu->gpr[OCERZ_RDI]) : NULL;
    void *sel = cpu->gpr[OCERZ_RSI] ? ocerz_g2h(cpu->gpr[OCERZ_RSI]) : NULL;
    void *fn = ob_need(&g_ob_class_getMethodImplementation);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, OCERZ_OBJC_LIBOBJC, "_class_getMethodImplementation", NULL, fn);
    void *imp = cls && sel ? ((void *(*)(void *, void *))fn)(cls, sel) : NULL;
    void *m = cls && sel ? ob_class_getInstanceMethod(cls, sel) : NULL;
    uint64_t answer = ocerz_objc_imp_for_guest(imp, m ? ob_method_getTypeEncoding(m) : NULL);
    ocerz_bridge_lower(&outer);
    ob_return(cpu, answer);
    return ob_settle(vm, cpu);
}

static void ob_append(char *buf, size_t cap, const char *s, void *cls, void *sel)
{
    size_t have = strlen(buf), add = s ? strlen(s) : 0;
    if (have + add + 1 > cap)
        ob_refuse(cls, sel, "has a forwarded method signature longer than ocerz reads");
    memcpy(buf + have, s, add + 1);
}

static const ObSend *ob_forwarded(void *recv, void *cls, void *sel, ObSend *scratch, int depth)
{
    void *send = ob_need(&g_ob_msgSend);
    void *fts = ob_sel_registerName("forwardingTargetForSelector:");
    if (depth < 8 && ob_class_respondsToSelector(cls, fts)) {
        void *target = ((void *(*)(void *, void *, void *))send)(recv, fts, sel);
        if (target && target != recv) {
            void *tcls = ob_object_getClass(target);
            const ObSend *e = ob_cached(tcls, sel);
            if (!e) {
                void *m = ob_class_getInstanceMethod(tcls, sel);
                if (m) {
                    ob_describe(tcls, sel, ob_method_getTypeEncoding(m), "forwarding target's", scratch);
                    return scratch;
                }
                return ob_forwarded(target, tcls, sel, scratch, depth + 1);
            }
            *scratch = *e;
            return scratch;
        }
    }

    void *msfs = ob_sel_registerName("methodSignatureForSelector:");
    if (!ob_class_respondsToSelector(cls, msfs))
        ob_refuse(cls, sel, "has no method, and the receiver does not answer methodSignatureForSelector:,"
                  " so there is no signature to forward it under");

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

static void ob_text_wide(const wchar_t *wide, ObText *t, const char *what)
{
    size_t n = 0;
    while (wide && wide[n])
        n++;
    char *buf = t->local;
    t->heap = NULL;
    if (n >= sizeof t->local) {
        buf = t->heap = malloc(n + 1);
        if (!buf)
            ob_stop("%s has a format of %zu wide characters and there is no memory to read it into", what, n);
    }
    for (size_t i = 0; i < n; i++)
        buf[i] = wide[i] > 0 && wide[i] < 0x80 ? (char)wide[i] : '?';
    buf[n] = '\0';
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

static int ob_gather_va_format(const char *what, const char *text, int dialect,
                              uint64_t address, uint64_t *slots)
{
    char classes[OCERZ_OBJC_VARIADIC_MAX + 1];
    const char *why = NULL;
    int n = ocerz_objc_format_classes(text, dialect, classes, sizeof classes, &why);
    if (n < 0)
        ob_stop("%s refuses the format \"%.200s\": it has %s", what, text, why);
    if (n == 0)
        return 0;
    if (!address)
        ob_stop("%s: null guest va_list", what);
    uint32_t gp = (uint32_t)ocerz_ld(address, 4);
    uint32_t fp = (uint32_t)ocerz_ld(address + 4, 4);
    uint64_t overflow = ocerz_ld(address + 8, 8);
    uint64_t saved = ocerz_ld(address + 16, 8);
    if (gp > 48 || (gp & 7) || fp < 48 || fp > 176 || ((fp - 48) & 15))
        ob_stop("%s: invalid guest va_list offsets (gp=%u fp=%u)", what, gp, fp);
    for (int k = 0; k < n; k++) {
        uint64_t from;
        if (classes[k] == 'd' && fp < 176) {
            if (!saved)
                ob_stop("%s: null guest va_list register save area", what);
            from = saved + fp;
            fp += 16;
        } else if (classes[k] != 'd' && gp < 48) {
            if (!saved)
                ob_stop("%s: null guest va_list register save area", what);
            from = saved + gp;
            gp += 8;
        } else {
            if (!overflow)
                ob_stop("%s: null guest va_list overflow area", what);
            from = overflow;
            overflow += 8;
        }
        uint64_t raw = ocerz_ld(from, 8);
        if (classes[k] == 'p')
            raw = raw ? (uint64_t)(uintptr_t)ocerz_g2h(raw) : 0;
        else if (classes[k] == 'i')
            raw = (uint64_t)(int64_t)(int32_t)raw;
        else if (classes[k] == 'u')
            raw = (uint32_t)raw;
        slots[k] = raw;
    }
    return n;
}

__attribute__((noinline))
static int ob_perform_general(OcerzCPU *cpu, const OcerzAbiSig *sig, const void *fn, const uint64_t *slots,
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

    uint64_t fpcr = ocerz_abi_round_swap(OCERZ_ABI_ROUND_NEAREST);
    ocerz_abi_call_native(fn, call.x, call.v, stack, words * 8, call.x8, call.rx, call.rv);
    int err = errno;
    ocerz_abi_round_swap(fpcr & OCERZ_ABI_ROUND_MASK);

    call.borrowed = 1;
    ocerz_abi_write_result(sig, cpu, &call);
    ocerz_abi_release_owned(&call);
    errno = err;
    return err;
}

static int ob_perform(OcerzCPU *cpu, const OcerzAbiSig *sig, const void *fn, const uint64_t *slots,
                      int nslots, int as_va_list, const char *what)
{
    if (nslots <= 0 && !as_va_list && ocerz_abi_register_only(sig)) {
        ocerz_abi_perform_registers(sig, fn, cpu);
        return errno;
    }
    return ob_perform_general(cpu, sig, fn, slots, nslots, as_va_list, what);
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
        if (!(send->fnptrs & bit))
            continue;
        uint64_t v = ob_named(sig, cpu, k, 'L');
        if (v && ocerz_abi_is_guest_code(v))
            ob_refuse(cls, sel, "cannot cross: argument %d is an x86 function pointer (%#llx), which native"
                      " code cannot call and no encoding gives a signature for", k - 2, (unsigned long long)v);
    }
}

static const char *ob_arg_at(const char *notation, int index)
{
    const char *p = strchr(notation, '(');
    if (!p)
        return NULL;
    p++;
    for (int k = 0; *p && *p != ')'; k++) {
        if (k == index)
            return p;
        if (*p == 'c' || *p == 'k')
            p++;
        if (*p == '{') {
            int depth = 0;
            do {
                if (*p == '{')
                    depth++;
                else if (*p == '}')
                    depth--;
                p++;
            } while (*p && depth > 0);
        } else {
            p++;
        }
    }
    return NULL;
}

static const ObSend *ob_object_blocks(void *recv, void *cls, void *sel, const ObSend *send, const OcerzCPU *cpu,
                                      ObSend *scratch)
{
    const OcerzAbiSig *sig = &send->shape->sig;
    uint32_t found = 0;
    for (int k = 2; k < sig->nargs && k < 32; k++) {
        if (!(send->objects & (1u << k)) || sig->arg[k] != 'p')
            continue;
        uint64_t v = ob_named(sig, cpu, k, 'L');
        if (v && !(v >> 63) && ocerz_block_is_guest_object(v))
            found |= 1u << k;
    }
    if (!found)
        return send;

    void *msend = ob_need(&g_ob_msgSend);
    void *msfs = ob_sel_registerName("methodSignatureForSelector:");
    if (!ob_class_respondsToSelector(cls, msfs))
        return send;
    void *ms = ((void *(*)(void *, void *, void *))msend)(recv, msfs, sel);
    if (!ms)
        return send;
    void *at = ob_sel_registerName("getArgumentTypeAtIndex:");
    unsigned long n = ((unsigned long (*)(void *, void *))msend)(ms, ob_sel_registerName("numberOfArguments"));
    for (int k = 2; k < 32; k++) {
        if (!(found & (1u << k)))
            continue;
        const char *t = (unsigned long)k < n ? ((const char *(*)(void *, void *, unsigned long))msend)(ms, at, (unsigned long)k)
                                             : NULL;
        if (!t || t[0] != '@' || t[1] != '?')
            found &= ~(1u << k);
    }
    if (!found)
        return send;

    char notation[OCERZ_OBJC_NOTATION_MAX];
    size_t len = 0;
    const char *src = send->shape->notation;
    const char *mark[32] = { 0 };
    for (int k = 2; k < 32; k++)
        if (found & (1u << k))
            mark[k] = ob_arg_at(src, k);
    for (const char *p = src; *p; p++) {
        int hit = 0;
        for (int k = 2; k < 32 && !hit; k++)
            hit = mark[k] == p;
        if (len + 4 >= sizeof notation)
            return send;
        if (hit) {
            memcpy(notation + len, "k{}", 3);
            len += 3;
        } else {
            notation[len++] = *p;
        }
    }
    notation[len] = '\0';
    const ObShape *shape = ob_shape(notation);
    if (!shape)
        return send;
    *scratch = *send;
    scratch->shape = shape;
    return scratch;
}

#define OB_IMP_PER_PAGE 128u
#define OB_IMP_PAGES 64u
#define OB_IMP_MAX (OB_IMP_PER_PAGE * OB_IMP_PAGES)
#define OB_IMP_STRIDE 16u
#define OB_IMP_SLOT 0x800u

typedef struct ObImp {
    void *imp;
    char *types;
    int stret;
} ObImp;

static ObImp g_ob_imps[OB_IMP_MAX];
static _Atomic unsigned g_ob_imps_n;
static _Atomic uint64_t g_ob_imp_pages[OB_IMP_PAGES];
static pthread_mutex_t g_ob_imp_lock = PTHREAD_MUTEX_INITIALIZER;

static void *_Atomic g_ob_sel_methodFor;
static void *_Atomic g_ob_sel_instanceMethodFor;

static int ob_answers_imp(void *sel)
{
    void *a = atomic_load(&g_ob_sel_methodFor);
    if (!a) {
        atomic_store(&g_ob_sel_instanceMethodFor, ob_sel_registerName("instanceMethodForSelector:"));
        a = ob_sel_registerName("methodForSelector:");
        atomic_store(&g_ob_sel_methodFor, a);
    }
    return sel == a || sel == atomic_load(&g_ob_sel_instanceMethodFor);
}

static int ob_types_stret(const char *types)
{
    char notation[OCERZ_OBJC_NOTATION_MAX];
    int nargs = 0;
    uint32_t blocks = 0, fnptrs = 0;
    static _Thread_local OcerzAbiSig sig;
    if (!types ||
        ocerz_objc_notation(types, notation, sizeof notation, &nargs, &blocks, &fnptrs) != OCERZ_OBJC_OK ||
        ocerz_abi_parse(notation, &sig) != OCERZ_OK)
        return 0;
    return sig.ret == '{' && sig.ret_struct.size > OB_SMALL_STRUCT;
}

static uint64_t ob_imp_page(unsigned page)
{
    uint64_t have = atomic_load(&g_ob_imp_pages[page]);
    if (have)
        return have;
    uint64_t tramp = ocerz_vdylib_trampoline(OCERZ_VDYLIB_TRAMP_NATIVE_IMP);
    uint64_t made = tramp ? ocerz_map_anywhere(OCERZ_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE) : 0;
    if (!made)
        return 0;
    uint8_t *buf = ocerz_g2h(made);
    memset(buf, 0xcc, OCERZ_GUEST_PAGE_SIZE);
    for (unsigned k = 0; k < OB_IMP_PER_PAGE; k++) {
        uint8_t *t = buf + (size_t)k * OB_IMP_STRIDE;
        uint32_t number = page * OB_IMP_PER_PAGE + k;
        int32_t rel = (int32_t)((int64_t)OB_IMP_SLOT - (int64_t)(k * OB_IMP_STRIDE + 12));
        t[0] = 0x41;
        t[1] = 0xba;
        memcpy(t + 2, &number, 4);
        t[6] = 0xff;
        t[7] = 0x25;
        memcpy(t + 8, &rel, 4);
    }
    memcpy(buf + OB_IMP_SLOT, &tramp, 8);
    if (ocerz_protect(made, OCERZ_GUEST_PAGE_SIZE, PROT_READ | PROT_EXEC) != OCERZ_OK) {
        ocerz_unmap(made, OCERZ_GUEST_PAGE_SIZE);
        return 0;
    }
    atomic_store(&g_ob_imp_pages[page], made);
    return made;
}

uint64_t ocerz_objc_imp_for_guest(void *native_imp, const char *types)
{
    if (!native_imp)
        return 0;
    uint64_t guest_fn = 0;
    if (ocerz_abi_callback_sig(native_imp, &guest_fn) && guest_fn)
        return guest_fn;
    uint64_t as_guest = ocerz_h2g(native_imp);
    if (ocerz_abi_is_guest_code(as_guest))
        return as_guest;

    uint64_t answer = 0;
    pthread_mutex_lock(&g_ob_imp_lock);
    unsigned n = atomic_load(&g_ob_imps_n), k;
    for (k = 0; k < n; k++)
        if (g_ob_imps[k].imp == native_imp)
            break;
    if (k == n && n < OB_IMP_MAX) {
        g_ob_imps[n].imp = native_imp;
        g_ob_imps[n].types = types ? strdup(types) : NULL;
        g_ob_imps[n].stret = ob_types_stret(types);
        atomic_store(&g_ob_imps_n, n + 1);
    }
    if (k < OB_IMP_MAX) {
        if (!g_ob_imps[k].types && types) {
            g_ob_imps[k].types = strdup(types);
            g_ob_imps[k].stret = ob_types_stret(types);
        }
        uint64_t page = ob_imp_page(k / OB_IMP_PER_PAGE);
        if (page)
            answer = page + (uint64_t)(k % OB_IMP_PER_PAGE) * OB_IMP_STRIDE;
    }
    pthread_mutex_unlock(&g_ob_imp_lock);
    if (!answer)
        ob_stop("no thunk is left for native implementation %p: all %u are bound, or no guest page"
                " could be made for them", native_imp, OB_IMP_MAX);
    return answer;
}

void *ocerz_objc_imp_from_guest(uint64_t guest_imp)
{
    if (!guest_imp)
        return NULL;
    for (unsigned p = 0; p < OB_IMP_PAGES; p++) {
        uint64_t page = atomic_load(&g_ob_imp_pages[p]);
        if (!page)
            break;
        uint64_t off = guest_imp - page;
        if (off >= (uint64_t)OB_IMP_PER_PAGE * OB_IMP_STRIDE || off % OB_IMP_STRIDE)
            continue;
        unsigned k = p * OB_IMP_PER_PAGE + (unsigned)(off / OB_IMP_STRIDE);
        return k < atomic_load(&g_ob_imps_n) ? g_ob_imps[k].imp : NULL;
    }
    return NULL;
}

typedef struct ObEh ObEh;
static const ObEh *ob_eh(void);
static int ob_eh_throw(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t gobj, int owned);
extern int ocerz_objc_guarded(void (*body)(void *), void *ctx, void **caught);

typedef struct ObGuarded {
    OcerzCPU *cpu;
    const OcerzAbiSig *sig;
    const void *fn;
    const uint64_t *slots;
    int nslots;
    const char *what;
} ObGuarded;

static void ob_guarded_body(void *ctx)
{
    ObGuarded *g = ctx;
    ob_perform(g->cpu, g->sig, g->fn, g->slots, g->nslots, 0, g->what);
}

static int ob_send_via(struct OcerzVM *vm, OcerzCPU *cpu, ObKind kind, int stret, void *imp,
                       const char *imp_types)
{
    const char *export = imp ? "_(native IMP)" : g_ob_export[kind][stret];
    uint64_t first = cpu->gpr[stret ? OCERZ_RSI : OCERZ_RDI];
    void *sel = (void *)(uintptr_t)cpu->gpr[stret ? OCERZ_RDX : OCERZ_RSI];
    ObSym *host = kind == OB_PLAIN ? &g_ob_msgSend : kind == OB_SUPER ? &g_ob_msgSendSuper
                                                                      : &g_ob_msgSendSuper2;
    void *fn = imp ? imp : ob_need(host);
    struct OcerzBridgeFrame outer;
    void *recv, *cls;
    ObSend scratch;
    const ObSend *send;

    ocerz_bridge_raise(&outer, OCERZ_OBJC_LIBOBJC, export, NULL, fn);
    if (kind == OB_PLAIN) {
        recv = first ? ocerz_g2h(first) : NULL;
        if (recv)
            ocerz_objcbridge_ensure_object(recv);
        cls = recv ? ob_object_getClass(recv) : NULL;
    } else {
        if (!first)
            ob_stop("%s was handed a null struct objc_super", export);
        uint64_t r = ocerz_ld(first, 8), c = ocerz_ld(first + 8, 8);
        recv = r ? ocerz_g2h(r) : NULL;
        cls = c ? ocerz_g2h(c) : NULL;
        if (cls)
            ocerz_objcbridge_ensure_class(cls);
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

    if (kind == OB_PLAIN && cls == (void *)_NSConcreteStackBlock && ocerz_block_is_guest(first)) {
        const char *name = ob_sel_getName(sel);
        if (strcmp(name, "copy") == 0 || strcmp(name, "copyWithZone:") == 0) {
            uint64_t copy = ocerz_block_copy_guest(first);
            ocerz_bridge_lower(&outer);
            ob_return(cpu, copy);
            return ob_settle(vm, cpu);
        }
    }

    send = ob_method(cls, sel, &scratch);
    if (!send && imp && imp_types) {
        ob_describe(cls, sel, imp_types, "implementation", &scratch);
        send = &scratch;
    }
    if (!send)
        send = ob_forwarded(recv, cls, sel, &scratch, 0);
    ObSend typed;
    if (send->objects)
        send = ob_object_blocks(recv, cls, sel, send, cpu, &typed);

    const OcerzAbiSig *sig = &send->shape->sig;
    int memory = sig->ret == '{' && sig->ret_struct.size > OB_SMALL_STRUCT;
    if (stret && !memory)
        ob_refuse(cls, sel, "was sent with %s, but its result under %s is not one System V returns in"
                  " memory", export + 1, send->shape->notation);
    if (!stret && memory)
        ob_refuse(cls, sel, "returns a structure System V returns in memory (%s), and the guest sent it"
                  " with %s, which passes no result pointer", send->shape->notation, export + 1);
    if (send->fnptrs)
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

    void *asked = NULL;
    int answers_imp = sig->ret == 'p' && sig->nargs == 3 && ob_answers_imp(sel);
    if (answers_imp)
        asked = (void *)(uintptr_t)ob_named(sig, cpu, 2, 'p');
    void *raised = NULL;
    if (ob_eh()) {
        ObGuarded g = { cpu, sig, fn, slots, nslots, selname };
        if (ocerz_objc_guarded(ob_guarded_body, &g, &raised)) {
            ocerz_bridge_lower(&outer);
            return ob_eh_throw(vm, cpu, raised ? ocerz_h2g(raised) : 0, 1);
        }
    } else {
        ob_perform(cpu, sig, fn, slots, nslots, 0, selname);
    }
    if (answers_imp && cpu->gpr[OCERZ_RAX]) {
        void *of = sel == atomic_load(&g_ob_sel_instanceMethodFor) ? recv : cls;
        void *m = asked && of ? ob_class_getInstanceMethod(of, asked) : NULL;
        cpu->gpr[OCERZ_RAX] = ocerz_objc_imp_for_guest(ocerz_g2h(cpu->gpr[OCERZ_RAX]),
                                                      m ? ob_method_getTypeEncoding(m) : NULL);
    }
    ocerz_bridge_lower(&outer);
    return ob_settle(vm, cpu);
}

static int ob_send(struct OcerzVM *vm, OcerzCPU *cpu, ObKind kind, int stret)
{
    return ob_send_via(vm, cpu, kind, stret, NULL, NULL);
}

static _Atomic uint64_t g_ob_guest_preprocessor;

int ocerz_objc_setExceptionPreprocessor(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t wanted = cpu->gpr[OCERZ_RDI];
    void *fn = ob_need(&g_ob_setExceptionPreprocessor);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, OCERZ_OBJC_LIBOBJC, "_objc_setExceptionPreprocessor", "p(c{p(p)})", fn);
    uint64_t native = 0;
    if (wanted && (ocerz_abi_callback_convert(wanted, "p(p)", &native) != OCERZ_OK || !native))
        ob_stop("objc_setExceptionPreprocessor could not bind preprocessor %#llx",
                (unsigned long long)wanted);
    ((void *(*)(void *))fn)(native ? ocerz_g2h(native) : NULL);
    uint64_t before = atomic_exchange(&g_ob_guest_preprocessor, wanted);
    ocerz_bridge_lower(&outer);
    ob_return(cpu, before);
    return ob_settle(vm, cpu);
}

int ocerz_objc_imp_trap(struct OcerzVM *vm, OcerzCPU *cpu)
{
    unsigned k = (unsigned)(cpu->gpr[OCERZ_R10] & 0xffffffffu);
    if (k >= atomic_load(&g_ob_imps_n))
        ob_stop("a thunk numbered %u for a native implementation was called, and ocerz made no such thunk", k);
    const ObImp *e = &g_ob_imps[k];
    return ob_send_via(vm, cpu, OB_PLAIN, e->stret, e->imp, e->types);
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
static ObVeneer g_ob_snprintf_l = {
    "_snprintf_l", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vsnprintf_l"), "i(pLpp)", "i(pLppp)", 3, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_asprintf = {
    "_asprintf", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vasprintf"), "i(pp)", "i(ppp)", 1, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_dprintf = {
    "_dprintf", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vdprintf"), "i(ip)", "i(ipp)", 1, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_syslog = {
    "_syslog", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vsyslog"), "v(ip)", "v(ipp)", 1, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_warn = {
    "_warn", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vwarn"), "v(p)", "v(pp)", 0, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_warnx = {
    "_warnx", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vwarnx"), "v(p)", "v(pp)", 0, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_warnc = {
    "_warnc", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vwarnc"), "v(ip)", "v(ipp)", 1, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_err = {
    "_err", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "verr"), "v(ip)", "v(ipp)", 1, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_errx = {
    "_errx", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "verrx"), "v(ip)", "v(ipp)", 1, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_errc = {
    "_errc", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "verrc"), "v(iip)", "v(iipp)", 2, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_swprintf = {
    "_swprintf", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vswprintf"), "i(pLp)", "i(pLpp)", 2, OCERZ_OBJC_FMT_WIDE,
};
static ObVeneer g_ob_wprintf = {
    "_wprintf", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vwprintf"), "i(p)", "i(pp)", 0, OCERZ_OBJC_FMT_WIDE,
};
static ObVeneer g_ob_fwprintf = {
    "_fwprintf", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vfwprintf"), "i(pp)", "i(ppp)", 1, OCERZ_OBJC_FMT_WIDE,
};
static ObVeneer g_ob_sprintf_chk = {
    "___sprintf_chk", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "__vsprintf_chk"), "i(piLp)", "i(piLpp)", 3,
    OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_snprintf_chk = {
    "___snprintf_chk", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "__vsnprintf_chk"), "i(pLiLp)", "i(pLiLpp)", 4,
    OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_sscanf = {
    "_sscanf", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vsscanf"), "i(pp)", "i(ppp)", 1, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_sscanf_l = {
    "_sscanf_l", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vsscanf_l"), "i(ppp)", "i(pppp)", 2, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_asprintf_l = {
    "_asprintf_l", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vasprintf_l"), "i(ppp)", "i(pppp)", 2, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_scanf = {
    "_scanf", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vscanf"), "i(p)", "i(pp)", 0, OCERZ_OBJC_FMT_C,
};
static ObVeneer g_ob_fscanf = {
    "_fscanf", OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "vfscanf"), "i(pp)", "i(ppp)", 1, OCERZ_OBJC_FMT_C,
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

static int ob_veneer_call(struct OcerzVM *vm, OcerzCPU *cpu, ObVeneer *vn, int guest_va, const char *sym)
{
    OcerzAbiSig named;
    if (ocerz_abi_parse(vn->named, &named) != OCERZ_OK)
        ob_stop("%s is declared %s, which the ABI engine refuses", sym, vn->named);
    void *vfn = ob_need(&vn->vform);

    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, vn->vform.lib, sym, vn->sig, vfn);

    uint64_t fmt = ob_named(&named, cpu, vn->fmt, 'p');
    ObText text;
    int dialect = vn->dialect;
    if (dialect == OCERZ_OBJC_FMT_C) {
        text.heap = NULL;
        text.s = fmt ? (const char *)(uintptr_t)fmt : "";
    } else if (dialect == OCERZ_OBJC_FMT_WIDE) {
        ob_text_wide((const wchar_t *)(uintptr_t)fmt, &text, sym);
        dialect = OCERZ_OBJC_FMT_C;
    } else {
        ob_text((void *)(uintptr_t)fmt, &text, sym);
    }

    uint64_t slots[OCERZ_OBJC_VARIADIC_MAX];
    int n;
    if (guest_va) {
        uint64_t address = ob_named(&named, cpu, named.nargs, 'p');
        n = ob_gather_va_format(sym, text.s, dialect,
                               address ? ocerz_h2g((void *)(uintptr_t)address) : 0, slots);
    } else {
        n = ob_gather_format(sym, text.s, dialect, &named, cpu, slots);
    }
    ob_text_free(&text);

    int err = ob_perform(cpu, &named, vfn, slots, n, 1, sym);
    ocerz_bridge_lower(&outer);
    errno = err;
    return ob_settle(vm, cpu);
}

static int ob_scan_count(const char *what, const char *text)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if (*p != '%') {
            p++;
            continue;
        }
        p++;
        if (*p == '%') {
            p++;
            continue;
        }
        if (*p == '\0')
            ob_stop("%s refuses the format \"%.200s\": it ends in a lone percent", what, text);
        int suppress = 0;
        if (*p == '*') {
            suppress = 1;
            p++;
        }
        while (*p >= '0' && *p <= '9')
            p++;
        if (*p == '$')
            ob_stop("%s refuses the format \"%.200s\": it has positional arguments", what, text);
        int is_L = 0;
        if (p[0] == 'h' && p[1] == 'h')
            p += 2;
        else if (p[0] == 'l' && p[1] == 'l')
            p += 2;
        else if (*p == 'h' || *p == 'l' || *p == 'j' || *p == 'z' || *p == 't')
            p += 1;
        else if (*p == 'L') {
            is_L = 1;
            p += 1;
        }
        if (*p == '\0')
            ob_stop("%s refuses the format \"%.200s\": it ends inside a conversion", what, text);
        unsigned char c = *p;
        if (c == '[') {
            if (is_L)
                ob_stop("%s refuses the format \"%.200s\": it reads a long double", what, text);
            p++;
            if (*p == '^')
                p++;
            if (*p == ']')
                p++;
            while (*p && *p != ']')
                p++;
            if (*p != ']')
                ob_stop("%s refuses the format \"%.200s\": it ends inside a scanset", what, text);
            if (!suppress)
                n++;
            p++;
            continue;
        }
        switch (c) {
        case 'd':
        case 'i':
        case 'o':
        case 'u':
        case 'x':
        case 'X':
        case 'f':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
        case 'c':
        case 's':
        case 'p':
        case 'n':
            break;
        default:
            ob_stop("%s refuses the format \"%.200s\": it has an unsupported conversion", what, text);
        }
        if (is_L)
            ob_stop("%s refuses the format \"%.200s\": it reads a long double", what, text);
        if (!suppress)
            n++;
        p++;
    }
    if (n > OCERZ_OBJC_VARIADIC_MAX)
        ob_stop("%s refuses the format \"%.200s\": it takes more arguments than cross", what, text);
    return n;
}

static int ob_scan_veneer_call(struct OcerzVM *vm, OcerzCPU *cpu, ObVeneer *vn, int guest_va,
                               const char *sym)
{
    OcerzAbiSig named;
    if (ocerz_abi_parse(vn->named, &named) != OCERZ_OK)
        ob_stop("%s is declared %s, which the ABI engine refuses", sym, vn->named);
    void *vfn = ob_need(&vn->vform);

    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, vn->vform.lib, sym, vn->sig, vfn);

    uint64_t fmt = ob_named(&named, cpu, vn->fmt, 'p');
    const char *text = fmt ? (const char *)(uintptr_t)fmt : "";
    int n = ob_scan_count(sym, text);

    uint64_t slots[OCERZ_OBJC_VARIADIC_MAX];
    if (guest_va) {
        uint64_t address = ob_named(&named, cpu, named.nargs, 'p');
        if (n > 0) {
            if (!address)
                ob_stop("%s: null guest va_list", sym);
            uint32_t gp = (uint32_t)ocerz_ld(address, 4);
            uint64_t overflow = ocerz_ld(address + 8, 8);
            uint64_t saved = ocerz_ld(address + 16, 8);
            if (gp > 48 || (gp & 7))
                ob_stop("%s: invalid guest va_list offsets (gp=%u)", sym, gp);
            for (int k = 0; k < n; k++) {
                uint64_t from;
                if (gp < 48) {
                    if (!saved)
                        ob_stop("%s: null guest va_list register save area", sym);
                    from = saved + gp;
                    gp += 8;
                } else {
                    if (!overflow)
                        ob_stop("%s: null guest va_list overflow area", sym);
                    from = overflow;
                    overflow += 8;
                }
                uint64_t raw = ocerz_ld(from, 8);
                slots[k] = raw ? (uint64_t)(uintptr_t)ocerz_g2h(raw) : 0;
            }
        }
    } else {
        OcerzAbiVaList va;
        if (ocerz_abi_va_start(&named, cpu, &va) != OCERZ_OK)
            ob_stop("%s: the ABI engine cannot find where the variadic arguments begin", sym);
        for (int k = 0; k < n; k++)
            ocerz_abi_va_arg(&va, cpu, 'p', &slots[k]);
    }

    int err = ob_perform(cpu, &named, vfn, slots, n, 1, sym);
    ocerz_bridge_lower(&outer);
    errno = err;
    return ob_settle(vm, cpu);
}

static int ob_scan_veneer(struct OcerzVM *vm, OcerzCPU *cpu, ObVeneer *vn, const char *sym)
{
    return ob_scan_veneer_call(vm, cpu, vn, 0, sym);
}

static int ob_scan_va_veneer(struct OcerzVM *vm, OcerzCPU *cpu, ObVeneer *vn, const char *sym)
{
    return ob_scan_veneer_call(vm, cpu, vn, 1, sym);
}

static int ob_veneer(struct OcerzVM *vm, OcerzCPU *cpu, ObVeneer *vn)
{
    return ob_veneer_call(vm, cpu, vn, 0, vn->sym);
}

static int ob_va_veneer(struct OcerzVM *vm, OcerzCPU *cpu, ObVeneer *base, const char *sym)
{
    return ob_veneer_call(vm, cpu, base, 1, sym);
}

int ocerz_fmt_printf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_printf); }
int ocerz_fmt_fprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_fprintf); }
int ocerz_fmt_sprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_sprintf); }
int ocerz_fmt_snprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_snprintf); }
int ocerz_fmt_snprintf_l(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_snprintf_l); }
int ocerz_fmt_asprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_asprintf); }
int ocerz_fmt_asprintf_l(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_asprintf_l); }
int ocerz_fmt_dprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_dprintf); }
int ocerz_fmt_syslog(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_syslog); }
int ocerz_fmt_warn(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_warn); }
int ocerz_fmt_warnx(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_warnx); }
int ocerz_fmt_warnc(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_warnc); }
int ocerz_fmt_err(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_err); }
int ocerz_fmt_errx(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_errx); }
int ocerz_fmt_errc(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_errc); }
int ocerz_fmt_swprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_swprintf); }
int ocerz_fmt_wprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_wprintf); }
int ocerz_fmt_fwprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_fwprintf); }
int ocerz_fmt_vswprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_swprintf, "_vswprintf"); }
int ocerz_fmt_vwprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_wprintf, "_vwprintf"); }
int ocerz_fmt_vfwprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_fwprintf, "_vfwprintf"); }
int ocerz_fmt_sprintf_chk(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_sprintf_chk); }
int ocerz_fmt_snprintf_chk(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_snprintf_chk); }
int ocerz_fmt_NSLog(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_veneer(vm, cpu, &g_ob_NSLog); }

int ocerz_fmt_vprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_printf, "_vprintf"); }
int ocerz_fmt_vfprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_fprintf, "_vfprintf"); }
int ocerz_fmt_vsprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_sprintf, "_vsprintf"); }
int ocerz_fmt_vsnprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_snprintf, "_vsnprintf"); }
int ocerz_fmt_vsnprintf_l(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_snprintf_l, "_vsnprintf_l"); }
int ocerz_fmt_vasprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_asprintf, "_vasprintf"); }
int ocerz_fmt_vasprintf_l(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_asprintf_l, "_vasprintf_l"); }
int ocerz_fmt_vdprintf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_dprintf, "_vdprintf"); }
int ocerz_fmt_vsyslog(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_syslog, "_vsyslog"); }
int ocerz_fmt_vwarn(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_warn, "_vwarn"); }
int ocerz_fmt_vwarnx(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_warnx, "_vwarnx"); }
int ocerz_fmt_vwarnc(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_warnc, "_vwarnc"); }
int ocerz_fmt_verr(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_err, "_verr"); }
int ocerz_fmt_verrx(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_errx, "_verrx"); }
int ocerz_fmt_verrc(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_errc, "_verrc"); }
int ocerz_fmt_NSLogv(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_NSLog, "_NSLogv"); }
int ocerz_fmt_vsprintf_chk(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_sprintf_chk, "___vsprintf_chk"); }
int ocerz_fmt_vsnprintf_chk(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_va_veneer(vm, cpu, &g_ob_snprintf_chk, "___vsnprintf_chk"); }
int ocerz_fmt_sscanf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_scan_veneer(vm, cpu, &g_ob_sscanf, "_sscanf"); }
int ocerz_fmt_sscanf_l(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_scan_veneer(vm, cpu, &g_ob_sscanf_l, "_sscanf_l"); }
int ocerz_fmt_scanf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_scan_veneer(vm, cpu, &g_ob_scanf, "_scanf"); }
int ocerz_fmt_fscanf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_scan_veneer(vm, cpu, &g_ob_fscanf, "_fscanf"); }
int ocerz_fmt_vsscanf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_scan_va_veneer(vm, cpu, &g_ob_sscanf, "_vsscanf"); }
int ocerz_fmt_vsscanf_l(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_scan_va_veneer(vm, cpu, &g_ob_sscanf_l, "_vsscanf_l"); }
int ocerz_fmt_vscanf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_scan_va_veneer(vm, cpu, &g_ob_scanf, "_vscanf"); }
int ocerz_fmt_vfscanf(struct OcerzVM *vm, OcerzCPU *cpu) { return ob_scan_va_veneer(vm, cpu, &g_ob_fscanf, "_vfscanf"); }

int ocerz_fmt_CFStringCreateWithFormat(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_veneer(vm, cpu, &g_ob_CFStringCreateWithFormat);
}

int ocerz_fmt_CFStringAppendFormat(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_veneer(vm, cpu, &g_ob_CFStringAppendFormat);
}

int ocerz_fmt_CFStringCreateWithFormatAndArguments(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_va_veneer(vm, cpu, &g_ob_CFStringCreateWithFormat, "_CFStringCreateWithFormatAndArguments");
}

int ocerz_fmt_CFStringAppendFormatAndArguments(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_va_veneer(vm, cpu, &g_ob_CFStringAppendFormat, "_CFStringAppendFormatAndArguments");
}

static void *_Atomic g_ob_native_prev;
static _Atomic uint64_t g_ob_guest_handler;
static void *_Atomic g_ob_guest_handler_native;
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

    atomic_store(&g_ob_guest_handler_native, guest ? ocerz_g2h(native) : NULL);
    ob_return(cpu, atomic_exchange(&g_ob_guest_handler, guest));
    return ob_settle(vm, cpu);
}

#define OB_GUEST_CXXABI "/usr/lib/libc++abi.dylib"
#define OB_EH_VTABLE_WORDS 10
#define OB_EH_OBJECT_BYTES 32
#define OB_EH_NO_CLASS 1ull

typedef struct ObEh {
    uint64_t alloc, throw_fn, rethrow, begin_catch, end_catch, terminate, set_terminate, personality,
        current_type;
} ObEh;

static ObEh g_ob_eh;
static _Atomic int g_ob_eh_ready;
static _Atomic unsigned g_ob_eh_missed_at = ~0u;
static pthread_mutex_t g_ob_eh_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_ob_eh_vtable_page;
static _Atomic uint64_t g_ob_eh_prev_terminate;
static _Atomic int g_ob_eh_terminate_set;

static const ObEh *ob_eh(void)
{
    if (atomic_load(&g_ob_eh_ready))
        return &g_ob_eh;
    unsigned gen = ocerz_dyld_generation();
    if (atomic_load(&g_ob_eh_missed_at) == gen)
        return NULL;
    static const struct {
        const char *sym;
        size_t at;
    } want[] = {
        { "___cxa_allocate_exception", offsetof(ObEh, alloc) },
        { "___cxa_throw", offsetof(ObEh, throw_fn) },
        { "___cxa_rethrow", offsetof(ObEh, rethrow) },
        { "___cxa_begin_catch", offsetof(ObEh, begin_catch) },
        { "___cxa_end_catch", offsetof(ObEh, end_catch) },
        { "__ZSt9terminatev", offsetof(ObEh, terminate) },
        { "__ZSt13set_terminatePFvvE", offsetof(ObEh, set_terminate) },
        { "___gxx_personality_v0", offsetof(ObEh, personality) },
        { "___cxa_current_exception_type", offsetof(ObEh, current_type) },
    };
    pthread_mutex_lock(&g_ob_eh_lock);
    if (!atomic_load(&g_ob_eh_ready)) {
        ObEh e;
        int ok = 1;
        for (size_t k = 0; ok && k < sizeof want / sizeof want[0]; k++) {
            int found = 0;
            uint64_t a = ocerz_dyld_guest_export(OB_GUEST_CXXABI, want[k].sym, &found);
            ok = found && a;
            memcpy((char *)&e + want[k].at, &a, sizeof a);
        }
        if (ok) {
            g_ob_eh = e;
            atomic_store(&g_ob_eh_ready, 1);
        } else {
            atomic_store(&g_ob_eh_missed_at, gen);
        }
    }
    pthread_mutex_unlock(&g_ob_eh_lock);
    return atomic_load(&g_ob_eh_ready) ? &g_ob_eh : NULL;
}

static void ob_eh_vtable_words(uint8_t *slot, uint32_t size)
{
    uint64_t no = ocerz_vdylib_trampoline(OCERZ_VDYLIB_TRAMP_OBJC_EH_FALSE);
    uint64_t words[OB_EH_VTABLE_WORDS] = { 0, 0, no, no, no, no,
                                           ocerz_vdylib_trampoline(OCERZ_VDYLIB_TRAMP_OBJC_EH_DO_CATCH),
                                           no, no, no };
    memset(slot, 0, size);
    memcpy(slot, words, size < sizeof words ? size : sizeof words);
}

static uint64_t ob_eh_vtable(void)
{
    uint64_t page = atomic_load(&g_ob_eh_vtable_page);
    if (page)
        return page;
    pthread_mutex_lock(&g_ob_eh_lock);
    page = atomic_load(&g_ob_eh_vtable_page);
    if (!page) {
        uint64_t made = ocerz_map_anywhere(OCERZ_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE);
        if (made) {
            ob_eh_vtable_words(ocerz_g2h(made), OB_EH_VTABLE_WORDS * 8);
            if (ocerz_protect(made, OCERZ_GUEST_PAGE_SIZE, PROT_READ) == OCERZ_OK)
                page = made;
            else
                ocerz_unmap(made, OCERZ_GUEST_PAGE_SIZE);
        }
        atomic_store(&g_ob_eh_vtable_page, page);
    }
    pthread_mutex_unlock(&g_ob_eh_lock);
    if (!page)
        ob_stop("no guest page could be set up for the Objective-C exception type table");
    return page;
}

void ocerz_objc_fill_ehtype_vtable(uint64_t slot, uint32_t size, const char *install_name,
                                   const char *export_name, OcerzVdylibHostSym host_sym)
{
    (void)install_name;
    (void)export_name;
    (void)host_sym;
    ob_eh_vtable_words(ocerz_g2h(slot), size);
}

void ocerz_objc_fill_ehtype(uint64_t slot, uint32_t size, const char *install_name, const char *export_name,
                            OcerzVdylibHostSym host_sym)
{
    uint64_t words[3] = { ob_eh_vtable() + 16, 0, 0 };
    if (strcmp(export_name, "_OBJC_EHTYPE_id") == 0) {
        words[1] = ocerz_h2g("id");
    } else {
        void *const *host = host_sym ? host_sym(install_name, export_name + 1) : NULL;
        if (host && host[2]) {
            words[1] = host[1] ? ocerz_h2g(host[1]) : 0;
            words[2] = ocerz_h2g(host[2]);
        } else {
            OCERZ_LOG("objc: %s has no %s on this host, so a @catch naming it catches nothing\n",
                      install_name, export_name);
            words[1] = ocerz_h2g(export_name + sizeof "_OBJC_EHTYPE_$_" - 1);
            words[2] = OB_EH_NO_CLASS;
        }
    }
    memcpy(ocerz_g2h(slot), words, size < sizeof words ? size : sizeof words);
}

static _Noreturn void ob_eh_absent(const char *sym)
{
    fprintf(stderr, "ocerz: bridge: %s %s not implemented: Objective-C exceptions in native mode need the"
            " guest C++ runtime, which %s does not hold (run make guest-cxx)\n", OCERZ_OBJC_LIBOBJC, sym,
            "the guest root");
    fflush(stderr);
    exit(OCERZ_BRIDGE_UNIMPL_EXIT);
}

static void ob_eh_install_terminate(struct OcerzVM *vm, const ObEh *eh, uint64_t stack_top)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_ob_eh_terminate_set, &expected, 1))
        return;
    uint64_t args[1] = { ocerz_vdylib_trampoline(OCERZ_VDYLIB_TRAMP_OBJC_EH_TERMINATE) };
    atomic_store(&g_ob_eh_prev_terminate, ocerz_vm_call(vm, eh->set_terminate, args, 1, stack_top));
}

static int ob_eh_throw(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t gobj, int owned)
{
    const ObEh *eh = ob_eh();
    if (!eh)
        ob_eh_absent("_objc_exception_throw");
    uint64_t stack_top = (cpu->gpr[OCERZ_RSP] - 256) & ~0xfull;
    ob_eh_install_terminate(vm, eh, stack_top);
    uint64_t pre = atomic_load(&g_ob_guest_preprocessor);
    if (pre && !owned) {
        uint64_t args[1] = { gobj };
        gobj = ocerz_vm_call(vm, pre, args, 1, stack_top);
    }
    void *obj = gobj ? ocerz_g2h(gobj) : NULL;
    if (obj && !owned)
        ((void *(*)(void *))ob_need(&g_ob_retain))(obj);
    uint64_t size[1] = { OB_EH_OBJECT_BYTES };
    uint64_t exc = ocerz_vm_call(vm, eh->alloc, size, 1, stack_top);
    void *cls = obj ? ob_object_getClass(obj) : NULL;
    ocerz_st(exc, 8, gobj);
    ocerz_st(exc + 8, 8, ob_eh_vtable() + 16);
    ocerz_st(exc + 16, 8, ocerz_h2g(cls ? ob_class_getName(cls) : "nil"));
    ocerz_st(exc + 24, 8, cls ? ocerz_h2g(cls) : 0);
    cpu->gpr[OCERZ_RDI] = exc;
    cpu->gpr[OCERZ_RSI] = exc + 8;
    cpu->gpr[OCERZ_RDX] = ocerz_vdylib_trampoline(OCERZ_VDYLIB_TRAMP_OBJC_EH_DESTROY);
    cpu->rip = eh->throw_fn;
    return ob_settle(vm, cpu);
}

int ocerz_objc_exception_throw(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_eh_throw(vm, cpu, cpu->gpr[OCERZ_RDI], 0);
}

static int ob_eh_jump(struct OcerzVM *vm, OcerzCPU *cpu, size_t at, const char *sym)
{
    const ObEh *eh = ob_eh();
    if (!eh)
        ob_eh_absent(sym);
    uint64_t target;
    memcpy(&target, (const char *)eh + at, sizeof target);
    cpu->rip = target;
    return ob_settle(vm, cpu);
}

int ocerz_objc_exception_rethrow(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_eh_jump(vm, cpu, offsetof(ObEh, rethrow), "_objc_exception_rethrow");
}

int ocerz_objc_begin_catch(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_eh_jump(vm, cpu, offsetof(ObEh, begin_catch), "_objc_begin_catch");
}

int ocerz_objc_end_catch(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_eh_jump(vm, cpu, offsetof(ObEh, end_catch), "_objc_end_catch");
}

int ocerz_objc_terminate(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_eh_jump(vm, cpu, offsetof(ObEh, terminate), "_objc_terminate");
}

int ocerz_objc_personality_v0(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_eh_jump(vm, cpu, offsetof(ObEh, personality), "___objc_personality_v0");
}

int ocerz_objc_eh_false(struct OcerzVM *vm, OcerzCPU *cpu)
{
    ob_return(cpu, 0);
    return ob_settle(vm, cpu);
}

int ocerz_objc_eh_do_catch(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t catch_ti = cpu->gpr[OCERZ_RDI], throw_ti = cpu->gpr[OCERZ_RSI], objp = cpu->gpr[OCERZ_RDX];
    uint64_t caught = 0;
    if (throw_ti && objp && ocerz_ld(throw_ti, 8) == ob_eh_vtable() + 16) {
        uint64_t thrown = ocerz_ld(objp, 8);
        uint64_t gobj = thrown ? ocerz_ld(thrown, 8) : 0;
        uint64_t want = catch_ti ? ocerz_ld(catch_ti + 16, 8) : 0;
        if (!want) {
            caught = 1;
        } else if (want != OB_EH_NO_CLASS && gobj) {
            void *wanted = ocerz_g2h(want);
            for (void *c = ob_object_getClass(ocerz_g2h(gobj)); c && !caught; c = ob_class_getSuperclass(c))
                caught = c == wanted;
        }
        if (caught)
            ocerz_st(objp, 8, gobj);
    }
    ob_return(cpu, caught);
    return ob_settle(vm, cpu);
}

int ocerz_objc_eh_destroy(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t thrown = cpu->gpr[OCERZ_RDI];
    uint64_t gobj = thrown ? ocerz_ld(thrown, 8) : 0;
    if (gobj)
        ((void (*)(void *))ob_need(&g_ob_release))(ocerz_g2h(gobj));
    ob_return(cpu, 0);
    return ob_settle(vm, cpu);
}

int ocerz_objc_eh_terminate(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const ObEh *eh = ob_eh();
    uint64_t stack_top = (cpu->gpr[OCERZ_RSP] - 256) & ~0xfull;
    uint64_t ti = eh ? ocerz_vm_call(vm, eh->current_type, NULL, 0, stack_top) : 0;
    if (ti && ocerz_ld(ti, 8) == ob_eh_vtable() + 16) {
        uint64_t gobj = ocerz_ld(ti - 8, 8);
        void *obj = gobj ? ocerz_g2h(gobj) : NULL;
        void (*handler)(void *) = atomic_load(&g_ob_guest_handler_native);
        if (handler)
            handler(obj);
        else
            ob_uncaught(obj);
        fprintf(stderr, "libc++abi: terminating due to uncaught exception of type %s\n",
                obj ? ob_class_getName(ob_object_getClass(obj)) : "nil");
        fflush(stderr);
        abort();
    }
    uint64_t prev = atomic_load(&g_ob_eh_prev_terminate);
    if (!prev)
        abort();
    cpu->rip = prev;
    return ob_settle(vm, cpu);
}

extern const uintptr_t ocerz_objc_guard_pad;
_Unwind_Reason_Code ocerz_objc_guard_personality(int version, _Unwind_Action actions, uint64_t exception_class,
                                                 struct _Unwind_Exception *ue, struct _Unwind_Context *context);
int ocerz_objc_guard_landed(struct _Unwind_Exception *ue, void **caught);

static __thread struct _Unwind_Exception *g_ob_guard_passing;

_Unwind_Reason_Code ocerz_objc_guard_personality(int version, _Unwind_Action actions, uint64_t exception_class,
                                                 struct _Unwind_Exception *ue, struct _Unwind_Context *context)
{
    (void)exception_class;
    if (version != 1 || (actions & _UA_FORCE_UNWIND) || ue == g_ob_guard_passing)
        return _URC_CONTINUE_UNWIND;
    if (actions & _UA_SEARCH_PHASE)
        return _URC_HANDLER_FOUND;
    if (!(actions & _UA_HANDLER_FRAME))
        return _URC_CONTINUE_UNWIND;
    _Unwind_SetGR(context, 0, (uintptr_t)ue);
    _Unwind_SetIP(context, ocerz_objc_guard_pad);
    return _URC_INSTALL_CONTEXT;
}

static void *ob_cxxabi(const char *name)
{
    void *a = dlsym(RTLD_DEFAULT, name);
    if (!a)
        ob_stop("the native C++ runtime has no %s, which catching a native exception needs", name);
    return a;
}

int ocerz_objc_guard_landed(struct _Unwind_Exception *ue, void **caught)
{
    void *(*begin)(void *) = (void *(*)(void *))ob_cxxabi("__cxa_begin_catch");
    void (*end)(void) = (void (*)(void))ob_cxxabi("__cxa_end_catch");
    void *const *(*type_of)(void) = (void *const *(*)(void))ob_cxxabi("__cxa_current_exception_type");
    void (*rethrow)(void) = (void (*)(void))ob_cxxabi("__cxa_rethrow");
    void *const *vtable = ob_need(&g_ob_ehtype_vtable);
    begin(ue);
    void *const *type = type_of();
    if (type && type[0] == (void *)(vtable + 2)) {
        void *obj = *(void **)(ue + 1);
        if (obj)
            ((void *(*)(void *))ob_need(&g_ob_retain))(obj);
        end();
        *caught = obj;
        return 1;
    }
    g_ob_guard_passing = ue;
    rethrow();
    abort();
}

typedef struct ObVarBindings {
    const void *fn;
    OcerzAbiCall *call;
    const uint64_t *slots;
    int nslots;
} ObVarBindings;

static void ob_var_bindings_body(void *ctx)
{
    ObVarBindings *b = ctx;
    ocerz_abi_call_native(b->fn, b->call->x, b->call->v, b->slots, 8 * (uint64_t)b->nslots, NULL, b->call->rx,
                          b->call->rv);
}

/* _NSDictionaryOfVariableBindings(keys, first, ...), which the
   NSDictionaryOfVariableBindings macro calls with one value per
   comma-separated key and a nil after them.  Its variadic values go to the
   host on arm64's stack, one per key after the first, up to and including the
   first nil, which is as far as the host reads: it raises when a key's value is
   nil, and that exception is thrown on into the guest. */
int ocerz_objc_dictionary_of_variable_bindings(struct OcerzVM *vm, OcerzCPU *cpu)
{
    static ObSym sym = OB_SYM(OCERZ_OBJC_FOUNDATION, "_NSDictionaryOfVariableBindings");
    void *fn = ob_need(&sym);
    OcerzAbiSig named;
    OcerzAbiCall call;
    OcerzAbiVaList va;
    if (ocerz_abi_parse("p(pp)", &named) != OCERZ_OK || ocerz_abi_read_guest(&named, cpu, &call) != OCERZ_OK ||
        ocerz_abi_va_start(&named, cpu, &va) != OCERZ_OK)
        ob_stop("_NSDictionaryOfVariableBindings could not read its arguments");
    ObText keys;
    ob_text((void *)(uintptr_t)call.x[0], &keys, "_NSDictionaryOfVariableBindings");
    int nkeys = 1;
    for (const char *c = keys.s; *c; c++)
        nkeys += *c == ',';
    ob_text_free(&keys);
    uint64_t slots[OCERZ_OBJC_VARIADIC_MAX];
    int n = 0;
    int ended = call.x[1] == 0;
    while (!ended && n + 1 < nkeys) {
        if (n == OCERZ_OBJC_VARIADIC_MAX - 1)
            ob_stop("_NSDictionaryOfVariableBindings names more than %d keys", OCERZ_OBJC_VARIADIC_MAX - 1);
        uint64_t w = 0;
        ocerz_abi_va_arg(&va, cpu, 'p', &w);
        slots[n++] = (uint64_t)(uintptr_t)(w ? ocerz_g2h(w) : NULL);
        ended = w == 0;
    }
    slots[n++] = 0;
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, OCERZ_OBJC_FOUNDATION, "__NSDictionaryOfVariableBindings", "p(pp)", fn);
    ObVarBindings b = { fn, &call, slots, n };
    void *raised = NULL;
    if (ob_eh()) {
        if (ocerz_objc_guarded(ob_var_bindings_body, &b, &raised)) {
            ocerz_bridge_lower(&outer);
            return ob_eh_throw(vm, cpu, raised ? ocerz_h2g(raised) : 0, 1);
        }
    } else {
        ob_var_bindings_body(&b);
    }
    ocerz_bridge_lower(&outer);
    ocerz_abi_write_result(&named, cpu, &call);
    return ob_settle(vm, cpu);
}

/* imp_implementationWithBlock's implementation is guest code, as libobjc's
   own trampolines are: a stub that moves self over _cmd, puts the block in
   front of it and jumps to the block's invoke, or for a block that returns a
   structure in memory does the same one register further on.  Being guest
   code, the stub crosses class_addMethod and every other call that takes an
   implementation like any function the guest wrote.  The block is copied
   first, guest-side for a guest block.  A stub is never rewritten once
   written, since a translation of it may exist, so imp_removeBlock releases
   the block and leaves the bytes. */
#define OB_BLOCK_IMP_BYTES 32u
#define OB_BLOCK_IMPS 4096

static pthread_mutex_t g_ob_block_imp_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_ob_block_imp_page, g_ob_block_imp_used;
static uint64_t g_ob_block_imp_at[OB_BLOCK_IMPS], g_ob_block_imp_block[OB_BLOCK_IMPS];
static int g_ob_block_imps;

static int ob_block_imp_find_locked(uint64_t imp)
{
    for (int k = 0; imp && k < g_ob_block_imps; k++)
        if (g_ob_block_imp_at[k] == imp)
            return k;
    return -1;
}

int ocerz_objc_imp_implementationWithBlock(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t given = cpu->gpr[OCERZ_RDI];
    if (!given)
        ob_stop("imp_implementationWithBlock was given no block");
    uint64_t block = ocerz_block_copy_guest(given);
    uint64_t invoke = ocerz_ld(block + 16, 8);
    if (!ocerz_abi_is_guest_code(invoke))
        ob_stop("imp_implementationWithBlock was given a native block (%#llx), whose invoke an x86 implementation"
                " cannot call", (unsigned long long)given);
    int stret = (ocerz_ld(block + 8, 4) & (1u << 29)) != 0;
    uint8_t code[OB_BLOCK_IMP_BYTES];
    memset(code, 0xcc, sizeof code);
    static const uint8_t plain[] = { 0x48, 0x89, 0xfe, 0x48, 0xbf }, plain_jump[] = { 0xff, 0x67, 0x10 };
    static const uint8_t memory[] = { 0x48, 0x89, 0xf2, 0x48, 0xbe }, memory_jump[] = { 0xff, 0x66, 0x10 };
    memcpy(code, stret ? memory : plain, 5);
    memcpy(code + 5, &block, 8);
    memcpy(code + 13, stret ? memory_jump : plain_jump, 3);
    pthread_mutex_lock(&g_ob_block_imp_lock);
    if (g_ob_block_imps == OB_BLOCK_IMPS)
        ob_stop("imp_implementationWithBlock has made %d implementations, as many as ocerz keeps", OB_BLOCK_IMPS);
    if (!g_ob_block_imp_page || g_ob_block_imp_used + OB_BLOCK_IMP_BYTES > OCERZ_GUEST_PAGE_SIZE) {
        g_ob_block_imp_page = ocerz_map_anywhere(OCERZ_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE);
        if (!g_ob_block_imp_page)
            ob_stop("no guest page could be set up for an implementation made from a block");
        memset(ocerz_g2h(g_ob_block_imp_page), 0xcc, OCERZ_GUEST_PAGE_SIZE);
        g_ob_block_imp_used = 0;
    } else if (ocerz_protect(g_ob_block_imp_page, OCERZ_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE) != OCERZ_OK) {
        ob_stop("the page of implementations made from blocks could not be made writable");
    }
    uint64_t imp = g_ob_block_imp_page + g_ob_block_imp_used;
    memcpy(ocerz_g2h(imp), code, sizeof code);
    g_ob_block_imp_used += OB_BLOCK_IMP_BYTES;
    if (ocerz_protect(g_ob_block_imp_page, OCERZ_GUEST_PAGE_SIZE, PROT_READ | PROT_EXEC) != OCERZ_OK)
        ob_stop("the page of implementations made from blocks could not be made executable");
    g_ob_block_imp_at[g_ob_block_imps] = imp;
    g_ob_block_imp_block[g_ob_block_imps++] = block;
    pthread_mutex_unlock(&g_ob_block_imp_lock);
    ob_return(cpu, imp);
    return ob_settle(vm, cpu);
}

int ocerz_objc_imp_getBlock(struct OcerzVM *vm, OcerzCPU *cpu)
{
    static ObSym fn = OB_SYM(OCERZ_OBJC_LIBOBJC, "imp_getBlock");
    uint64_t imp = cpu->gpr[OCERZ_RDI];
    pthread_mutex_lock(&g_ob_block_imp_lock);
    int k = ob_block_imp_find_locked(imp);
    uint64_t block = k >= 0 ? g_ob_block_imp_block[k] : 0;
    pthread_mutex_unlock(&g_ob_block_imp_lock);
    if (k < 0 && imp && !ocerz_abi_is_guest_code(imp)) {
        void *b = ((void *(*)(void *))ob_need(&fn))(ocerz_g2h(imp));
        block = b ? ocerz_h2g(b) : 0;
    }
    ob_return(cpu, block);
    return ob_settle(vm, cpu);
}

int ocerz_objc_imp_removeBlock(struct OcerzVM *vm, OcerzCPU *cpu)
{
    static ObSym fn = OB_SYM(OCERZ_OBJC_LIBOBJC, "imp_removeBlock");
    static ObSym release = OB_SYM(OCERZ_BRIDGE_LIBSYSTEM, "_Block_release");
    uint64_t imp = cpu->gpr[OCERZ_RDI];
    pthread_mutex_lock(&g_ob_block_imp_lock);
    int k = ob_block_imp_find_locked(imp);
    uint64_t block = k >= 0 ? g_ob_block_imp_block[k] : 0;
    if (k >= 0)
        g_ob_block_imp_block[k] = 0;
    pthread_mutex_unlock(&g_ob_block_imp_lock);
    uint64_t removed = 0;
    if (k >= 0) {
        if (block)
            ((void (*)(void *))ob_need(&release))(ocerz_g2h(block));
        removed = block != 0;
    } else if (imp && !ocerz_abi_is_guest_code(imp)) {
        removed = ((bool (*)(void *))ob_need(&fn))(ocerz_g2h(imp));
    }
    ob_return(cpu, removed);
    return ob_settle(vm, cpu);
}

/* NSGetUncaughtExceptionHandler answers the function NSSetUncaughtExceptionHandler
   installed: the guest's own when the native one is ocerz's callback onto it,
   and otherwise a thunk the guest can call. */
int ocerz_objc_NSGetUncaughtExceptionHandler(struct OcerzVM *vm, OcerzCPU *cpu)
{
    static ObSym fn = OB_SYM(OCERZ_OBJC_FOUNDATION, "NSGetUncaughtExceptionHandler");
    void *handler = ((void *(*)(void))ob_need(&fn))();
    uint64_t guest = 0;
    if (handler && !(ocerz_abi_callback_sig(handler, &guest) && guest))
        guest = ocerz_bridge_native_thunk(handler, "(uncaught exception handler)", "v(p)");
    ob_return(cpu, guest);
    return ob_settle(vm, cpu);
}

int ocerz_objc_realizeClassFromSwift(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t cls = cpu->gpr[OCERZ_RDI], previously = cpu->gpr[OCERZ_RSI];
    void *fn = ob_need(&g_ob_realizeClassFromSwift);
    int known = cls ? ocerz_objcbridge_prepare_class(cls) : 1;
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, OCERZ_OBJC_LIBOBJC, "__objc_realizeClassFromSwift", "p(pp)", fn);
    void *got = ((void *(*)(void *, void *))fn)(cls ? ocerz_g2h(cls) : NULL,
                                               known && previously ? ocerz_g2h(previously) : NULL);
    atomic_fetch_add(&g_ob_generation, 1);
    ocerz_bridge_lower(&outer);
    ob_return(cpu, got ? ocerz_h2g(got) : 0);
    return ob_settle(vm, cpu);
}

int ocerz_objc_readClassPair(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t cls = cpu->gpr[OCERZ_RDI], info = cpu->gpr[OCERZ_RSI];
    void *fn = ob_need(&g_ob_readClassPair);
    if (cls)
        ocerz_objcbridge_prepare_class(cls);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, OCERZ_OBJC_LIBOBJC, "_objc_readClassPair", "p(pp)", fn);
    void *got = ((void *(*)(void *, void *))fn)(cls ? ocerz_g2h(cls) : NULL, info ? ocerz_g2h(info) : NULL);
    atomic_fetch_add(&g_ob_generation, 1);
    ocerz_bridge_lower(&outer);
    ob_return(cpu, got ? ocerz_h2g(got) : 0);
    return ob_settle(vm, cpu);
}

static pthread_once_t g_ob_no_once = PTHREAD_ONCE_INIT;
static uint64_t g_ob_no;

static void ob_make_no(void)
{
    uint64_t page = ocerz_map_anywhere(OCERZ_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE);
    if (!page)
        return;
    uint8_t *buf = ocerz_g2h(page);
    memset(buf, 0xcc, OCERZ_GUEST_PAGE_SIZE);
    static const uint8_t no[] = { 0x31, 0xc0, 0xc3 };
    memcpy(buf, no, sizeof no);
    if (ocerz_protect(page, OCERZ_GUEST_PAGE_SIZE, PROT_READ | PROT_EXEC) == OCERZ_OK)
        g_ob_no = page;
}

static uint64_t ob_guest_no(void)
{
    pthread_once(&g_ob_no_once, ob_make_no);
    return g_ob_no;
}

typedef struct ObHook {
    ObSym *setter;
    const char *export;
    const char *notation;
    void *_Atomic guest;
    uint64_t guest_fn;
    void *prev;
    int installed;
} ObHook;

static pthread_mutex_t g_ob_hook_lock = PTHREAD_MUTEX_INITIALIZER;
static ObHook g_ob_getclass = { &g_ob_setHook_getClass, "_objc_setHook_getClass", "b(pp)", NULL, 0, NULL, 0 };
static ObHook g_ob_imagename = { &g_ob_setHook_getImageName, "_objc_setHook_getImageName", "b(pp)", NULL, 0, NULL,
                                 0 };
static ObHook g_ob_namer = { &g_ob_setHook_lazyClassNamer, "_objc_setHook_lazyClassNamer", "p(p)", NULL, 0, NULL,
                             0 };

static bool ob_hook2(ObHook *h, const void *a, void *b)
{
    bool (*guest)(const void *, void *) = atomic_load(&h->guest);
    if (guest && guest(a, b))
        return true;
    return h->prev ? ((bool (*)(const void *, void *))h->prev)(a, b) : false;
}

static bool ob_getclass_hook(const void *name, void *out)
{
    return ob_hook2(&g_ob_getclass, name, out);
}

static bool ob_imagename_hook(const void *cls, void *out)
{
    return ob_hook2(&g_ob_imagename, cls, out);
}

static const char *ob_namer_hook(void *cls)
{
    const char *(*guest)(void *) = atomic_load(&g_ob_namer.guest);
    const char *name = guest ? guest(cls) : NULL;
    if (!name && g_ob_namer.prev)
        name = ((const char *(*)(void *))g_ob_namer.prev)(cls);
    return name;
}

static int ob_set_hook(struct OcerzVM *vm, OcerzCPU *cpu, ObHook *h, void *native_hook)
{
    uint64_t guest = cpu->gpr[OCERZ_RDI], out_old = cpu->gpr[OCERZ_RSI];
    void *set = ob_need(h->setter);
    uint64_t slot = 0;
    if (guest && (ocerz_abi_callback_convert(guest, h->notation, &slot) != OCERZ_OK || !slot))
        ob_stop("%s could not bind guest hook %#llx to a callback", h->export, (unsigned long long)guest);
    pthread_mutex_lock(&g_ob_hook_lock);
    uint64_t chain = h->guest_fn ? h->guest_fn : ob_guest_no();
    if (!h->installed) {
        struct OcerzBridgeFrame outer;
        ocerz_bridge_raise(&outer, OCERZ_OBJC_LIBOBJC, h->export, NULL, set);
        ((void (*)(void *, void **))set)(native_hook, &h->prev);
        ocerz_bridge_lower(&outer);
        h->installed = 1;
    }
    atomic_store(&h->guest, slot ? ocerz_g2h(slot) : NULL);
    h->guest_fn = guest;
    pthread_mutex_unlock(&g_ob_hook_lock);
    if (out_old)
        ocerz_st(out_old, 8, chain);
    ob_return(cpu, 0);
    return ob_settle(vm, cpu);
}

int ocerz_objc_setHook_getClass(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_set_hook(vm, cpu, &g_ob_getclass, (void *)ob_getclass_hook);
}

int ocerz_objc_setHook_getImageName(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_set_hook(vm, cpu, &g_ob_imagename, (void *)ob_imagename_hook);
}

int ocerz_objc_setHook_lazyClassNamer(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_set_hook(vm, cpu, &g_ob_namer, (void *)ob_namer_hook);
}

static int ob_object_call(struct OcerzVM *vm, OcerzCPU *cpu, ObSym *s, const char *export)
{
    uint64_t a = cpu->gpr[OCERZ_RDI];
    void *fn = ob_need(s);
    if (a)
        ocerz_objcbridge_ensure_object(ocerz_g2h(a));
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, OCERZ_OBJC_LIBOBJC, export, "p(p)", fn);
    void *r = ((void *(*)(void *))fn)(a ? ocerz_g2h(a) : NULL);
    ocerz_bridge_lower(&outer);
    ob_return(cpu, r ? ocerz_h2g(r) : 0);
    return ob_settle(vm, cpu);
}

int ocerz_objc_opt_self(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_object_call(vm, cpu, &g_ob_opt_self, "_objc_opt_self");
}

int ocerz_objc_opt_class(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_object_call(vm, cpu, &g_ob_opt_class, "_objc_opt_class");
}

int ocerz_objc_alloc(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_object_call(vm, cpu, &g_ob_alloc, "_objc_alloc");
}

int ocerz_objc_alloc_init(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_object_call(vm, cpu, &g_ob_alloc_init, "_objc_alloc_init");
}

int ocerz_objc_allocWithZone(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return ob_object_call(vm, cpu, &g_ob_allocWithZone, "_objc_allocWithZone");
}

static uint64_t ob_guest_swift_release(void)
{
    static _Atomic uint64_t at;
    uint64_t a = atomic_load_explicit(&at, memory_order_acquire);
    if (!a) {
        a = ocerz_dyld_native_image_export("/usr/lib/swift/libswiftCore.dylib", "_swift_release");
        if (a && ocerz_abi_is_guest_code(a))
            atomic_store_explicit(&at, a, memory_order_release);
        else
            a = 0;
    }
    return a;
}

int ocerz_objc_release(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t a = cpu->gpr[OCERZ_RDI];
    if (a && ocerz_objcbridge_guest_swift_object(ocerz_g2h(a))) {
        uint64_t to = ob_guest_swift_release();
        if (to) {
            cpu->rip = to;
            return OCERZ_STEP_OK;
        }
    }
    void *fn = ob_need(&g_ob_release);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, OCERZ_OBJC_LIBOBJC, "_objc_release", "v(p)", fn);
    ((void (*)(void *))fn)(a ? ocerz_g2h(a) : NULL);
    ocerz_bridge_lower(&outer);
    ob_return(cpu, 0);
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

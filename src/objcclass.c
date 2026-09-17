/*
 * Objective-C classes, categories and protocols an x86 guest defines, made the
 * host's native arm64 runtime's own.
 *
 * In native mode no x86 libobjc exists, so nothing reads a guest image's class
 * lists the way the translated runtime does in cache mode.  A class the guest
 * compiled is a class_t in the guest's __DATA, and every reference the guest
 * holds to it - a class reference, a superclass reference for [super ...], a
 * GOT entry another guest image bound - is that class_t's address.  Native code
 * has to call the guest's methods too: NSSet asks a guest object for -hash, a
 * view asks for -drawRect:, the runtime itself sends +initialize and calls
 * .cxx_destruct.  ocerz_objcbridge_define_image makes each class, category and
 * protocol of one image known to the native runtime, after the loader has bound
 * the image's fixups and canonicalized its selectors and before any guest code
 * runs, and ocerz_objcbridge_run_loads calls the +load methods that definition
 * queued.
 *
 * ---- the class stays where the guest put it ----
 * objc_readClassPair realizes a compiler-emitted class pair in writable memory
 * without moving it: it answers the same pointer, registers the class by name,
 * realizes it against its superclass and slides its ivars.  So a guest class
 * keeps its address, and no class reference, superclass reference or bound
 * pointer is rewritten.  A probe in a plain arm64 process established the rest
 * before any of this was built.  The runtime realizes an untouched native
 * superclass itself.  A subclass of a class read this way works, super sends
 * included.  An NSView subclass whose ivar offset variable held 152 had it
 * slid to 536, arm64 NSView's size, and cacheDisplayInRect: called its
 * drawRect:.  Raw C function addresses as implementations need no pointer
 * authentication.  A duplicate name is not fatal: the runtime prints that the
 * class is implemented in both places, answers the new class, and keeps the
 * first for lookups by name, as it does for images dyld loads.  A class whose
 * superclass is a class_t the runtime has never seen is not refused either: the
 * call answers the class and the runtime aborts at realization with "Attempt to
 * use unknown class", which is why ocerz orders superclasses first and refuses
 * by name before the call.  class_t, class_ro_t, method_t, ivar_t, property_t,
 * category_t and protocol_t are laid out identically on x86_64 and arm64, both
 * being LP64, so the runtime reads the guest's structures as they are.
 *
 * Three words of the class_t and of its metaclass are written before the call.
 * The cache word becomes the native _objc_empty_cache, which the guest's import
 * already bound it to.  The word after it becomes zero: the x86 ABI calls it the
 * vtable and an older image binds _objc_empty_vtable there, while the arm64
 * runtime keeps its cache mask and flags in it.  And the data word, which on
 * disk points at the guest's class_ro_t, points at a copy of it on the host
 * heap whose method list and protocol list are replaced as below.  The guest's
 * own class_ro_t, in __objc_const, is never written.  The copy keeps the name,
 * flags, instance start and size, ivar list, ivar layouts and property list
 * exactly as the guest's, so the runtime reads ARC's ivarLayout and
 * weakIvarLayout as the compiler wrote them.  The runtime writes the low 32
 * bits of each ivar offset variable when it slides; an x86 compiler emits that
 * variable as 64 bits and reads all of them, and the upper half is zero on disk,
 * so the two agree.  A class whose layout is known when compiled, NSObject's
 * subclasses in one image, uses constant offsets instead, and NSObject is eight
 * bytes on both.
 *
 * ---- methods ----
 * Every method list, instance and class, is copied into an absolute list of
 * 24-byte entries on the host heap.  A relative list has to be copied anyway,
 * because its 32-bit offsets cannot reach a trampoline in ocerz's image, and a
 * relative list built by hand on the heap crashed the runtime's method scanner
 * at +initialize in the probe.  An entry's selector is sel_registerName of its
 * name, reached through the selector reference a relative entry names, already
 * canonical by then, or directly when the list says its selectors are direct.
 * Its types are the guest's own string, so method_getTypeEncoding answers the
 * x86 encoding, and its implementation is a callback slot bound to the guest's
 * function under the notation ocerz_objc_method_notation converts those types
 * to.  That is ocerz_objc_notation, which reads c as b - an x86 BOOL is a
 * signed char, where arm64's bool is B, which it also reads as b - and q as l,
 * the way an LP64 compiler encodes long, and a method's first two arguments,
 * self and _cmd, must be pointers.  A slot is the same slot for the same
 * function and notation, so a getter shared by two lists is one address.
 *
 * A method whose types do not cross - a long double, a union, a bitfield, more
 * arguments than the ABI engine carries - does not keep its class out.  Its
 * implementation is one native function, the same for every such method, and
 * the class, selector, types and reason are recorded beside it; called, it walks
 * the receiver's classes for the record and stops with OCERZ_BRIDGE_UNIMPL_EXIT
 * naming the method and why.  A method the callback bank has no slot left for
 * is bound to it the same way, so a full bank is a named refusal when the method
 * is called and not before.  The records are an append-only list read without
 * a lock.
 *
 * ---- what is refused ----
 * A class is refused by name, and the process stopped, when it is a root class,
 * because a guest root would need retain, release and every NSObject method in
 * guest code; when its superclass is null without its class_ro_t saying it is a
 * root, which is what a weak-linked superclass the host lacks leaves, and which
 * the native runtime would answer by leaving the class out and every reference
 * to it nil; when it is a Swift class, marked in the low bits of its data word
 * or by a Swift metadata initializer; when its class_ro_t already carries the
 * flags only a running runtime sets; when its metaclass is not a guest class
 * marked as one; and when its superclass is a guest class ocerz has not defined,
 * including one in a chain of superclasses that comes back to itself.  Within an image the class
 * list is put in superclass order first: the classes are sorted by address and
 * each chain is walked to the first superclass outside the list, so a subclass
 * listed before its superclass waits for it.  A superclass in another guest
 * image is already defined, since the loader defines a dylib as it loads it and
 * loads dependencies first.
 *
 * ---- categories ----
 * __objc_catlist and then __objc_catlist2 are applied in list order.  Each
 * instance method is class_replaceMethod on the class, which adds the method
 * when the class itself has none of that name, overriding a superclass's, and
 * replaces the implementation of the class's own when it has one; the last
 * category in the lists wins, as natively.  Class methods go to the metaclass
 * the same way, protocols through class_addProtocol, properties through
 * class_replaceProperty, and class properties to the metaclass when the image
 * info's flag says category_t carries them.  A category may be on a native
 * class or on a guest class defined earlier.  One whose class is null, as a
 * category on a missing weak-linked class is, is left out with a log line; one
 * on a guest class ocerz has not defined is refused by name.  The static linker
 * already merges a category into its class's own method lists when both are in
 * one image, so such a category arrives as the class's own methods.
 *
 * ---- protocols ----
 * A guest image carries its own protocol_t for every protocol it names,
 * NSObject and NSCopying included, and none of them is an object the native
 * runtime knows.  Each one in __objc_protolist, and each one a list or
 * reference names, is resolved once: objc_getProtocol by name when the host has
 * it, and otherwise objc_allocateProtocol, with the protocols it adopts
 * resolved and registered first - protocol_addProtocol refuses one still under
 * construction - its four method description lists added with the guest's
 * types, its instance and class properties, and objc_registerProtocol.  Every
 * word of __objc_protorefs, which is what @protocol compiles to, is rewritten to
 * the native protocol, and a class's protocol list is copied with native
 * protocols in it.
 *
 * ---- +load ----
 * The runtime calls +load only for images dyld loaded, so ocerz queues them.
 * For each class __objc_nlclslist names, its guest superclasses are queued
 * first, each class once, with the +load of the class's own class_ro_t - never
 * a category's - and then each category __objc_nlcatlist names that has a
 * +load of its own.  ocerz_objcbridge_run_loads runs the queue in that order
 * inside one native autorelease pool, calling each guest implementation
 * directly as guest code on the current thread, the class in rdi and the load
 * selector in rsi, which is how the runtime calls one.  The loader runs it
 * once, after every load-time image is defined and before main, so a
 * dependency's +load methods run before its dependents'.  A +load stays in the
 * method lists as well, as natively.  +initialize needs nothing: the runtime
 * sends it lazily through the method list like any other message.
 *
 * ---- lifetime ----
 * An image, by header address, is defined once.  Nothing here is ever freed:
 * the copies are what the runtime reads for as long as the class exists, which
 * is the life of the process.  The tables are guarded by one mutex, since only
 * the loader defines.  .cxx_construct and .cxx_destruct are ordinary methods;
 * the runtime calls them with the object as the only argument, so the slot hands
 * the guest whatever x1 held as _cmd, which neither reads.  The loader's dlopen
 * path defines an image the same way, but nothing runs the +load methods it
 * queues once main has started; native mode's dlopen is still a stub record, so
 * no guest reaches that path, and every defined image is a load-time one.
 * OCERZ_OBJCLOG prints each class, category, protocol and method as it is
 * defined, each method that cannot cross, and each +load as it runs.
 */
#include "ocerz/objcbridge.h"
#include "ocerz/abi.h"
#include "ocerz/bridge.h"
#include "ocerz/mem.h"
#include "ocerz/vdylib.h"
#include "ocerz/vm.h"

#include <pthread.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach-o/loader.h>

#define OC_RO_META 0x1u
#define OC_RO_ROOT 0x2u
#define OC_RO_SWIFT_INIT 0x40u
#define OC_RO_FUTURE 0x40000000u
#define OC_RO_REALIZED 0x80000000u
#define OC_FAST_SWIFT 0x3ull
#define OC_FAST_DATA 0x00007ffffffffff8ull
#define OC_METHOD_FLAGS 0xffff0003u
#define OC_METHOD_RELATIVE 0x80000000u
#define OC_METHOD_DIRECT_SEL 0x40000000u
#define OC_IMAGE_CLASS_PROPERTIES 0x40u
#define OC_RO_BYTES 72
#define OC_PROTOCOL_BASE 72
#define OC_ATTRS_MAX 32

typedef struct OcSym {
    const char *name;
    void *_Atomic addr;
} OcSym;

#define OC_SYM(name) { (name), NULL }

static OcSym g_oc_readClassPair = OC_SYM("objc_readClassPair");
static OcSym g_oc_empty_cache = OC_SYM("_objc_empty_cache");
static OcSym g_oc_sel_registerName = OC_SYM("sel_registerName");
static OcSym g_oc_sel_getName = OC_SYM("sel_getName");
static OcSym g_oc_object_getClass = OC_SYM("object_getClass");
static OcSym g_oc_class_getName = OC_SYM("class_getName");
static OcSym g_oc_class_getSuperclass = OC_SYM("class_getSuperclass");
static OcSym g_oc_class_isMetaClass = OC_SYM("class_isMetaClass");
static OcSym g_oc_class_replaceMethod = OC_SYM("class_replaceMethod");
static OcSym g_oc_class_addProtocol = OC_SYM("class_addProtocol");
static OcSym g_oc_class_replaceProperty = OC_SYM("class_replaceProperty");
static OcSym g_oc_objc_getProtocol = OC_SYM("objc_getProtocol");
static OcSym g_oc_objc_allocateProtocol = OC_SYM("objc_allocateProtocol");
static OcSym g_oc_objc_registerProtocol = OC_SYM("objc_registerProtocol");
static OcSym g_oc_protocol_addProtocol = OC_SYM("protocol_addProtocol");
static OcSym g_oc_protocol_addMethodDescription = OC_SYM("protocol_addMethodDescription");
static OcSym g_oc_protocol_addProperty = OC_SYM("protocol_addProperty");
static OcSym g_oc_pool_push = OC_SYM("objc_autoreleasePoolPush");
static OcSym g_oc_pool_pop = OC_SYM("objc_autoreleasePoolPop");

static void *oc_sym(OcSym *s)
{
    void *a = s->addr;
    if (!a) {
        a = ocerz_bridge_host_symbol(OCERZ_OBJC_LIBOBJC, s->name);
        if (a)
            s->addr = a;
    }
    return a;
}

static _Noreturn void oc_stop(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static _Noreturn void oc_stop(const char *fmt, ...)
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

static void *oc_need(OcSym *s)
{
    void *a = oc_sym(s);
    if (!a)
        oc_stop("the host %s has no %s, which defining a guest class needs", OCERZ_OBJC_LIBOBJC, s->name);
    return a;
}

static int oc_logging(void)
{
    static int en = -1;
    if (en < 0)
        en = getenv("OCERZ_OBJCLOG") ? 1 : 0;
    return en;
}

static void *oc_sel(const char *name)
{
    return ((void *(*)(const char *))oc_need(&g_oc_sel_registerName))(name);
}

static const char *oc_sel_name(void *sel)
{
    return ((const char *(*)(void *))oc_need(&g_oc_sel_getName))(sel);
}

static void *oc_object_getClass(void *obj)
{
    return ((void *(*)(void *))oc_need(&g_oc_object_getClass))(obj);
}

static const char *oc_class_name(void *cls)
{
    return ((const char *(*)(void *))oc_need(&g_oc_class_getName))(cls);
}

static void *oc_superclass(void *cls)
{
    return ((void *(*)(void *))oc_need(&g_oc_class_getSuperclass))(cls);
}

static int oc_is_meta(void *cls)
{
    return ((bool (*)(void *))oc_need(&g_oc_class_isMetaClass))(cls);
}

static uint64_t oc_word(uint64_t addr)
{
    return ocerz_ld(addr, 8);
}

static uint32_t oc_u32(uint64_t addr)
{
    return (uint32_t)ocerz_ld(addr, 4);
}

static int32_t oc_rel(uint64_t addr)
{
    return (int32_t)(uint32_t)ocerz_ld(addr, 4);
}

static const char *oc_str(uint64_t addr)
{
    return addr ? (const char *)ocerz_g2h(addr) : NULL;
}

static int oc_is_guest(uint64_t addr)
{
    return addr && ocerz_host_in_guest_reservation(ocerz_g2h(addr));
}

int ocerz_objc_read_class(uint64_t addr, OcerzObjcClass *out)
{
    memset(out, 0, sizeof *out);
    if (!addr)
        return OCERZ_OBJC_NULL;
    out->isa = oc_word(addr);
    out->superclass = oc_word(addr + 8);
    out->cache = oc_word(addr + 16);
    out->vtable = oc_word(addr + 24);
    uint64_t bits = oc_word(addr + 32);
    out->ro = bits & OC_FAST_DATA;
    out->swift = (uint32_t)(bits & OC_FAST_SWIFT);
    if (out->swift)
        return OCERZ_OBJC_SWIFT;
    if (!out->ro)
        return OCERZ_OBJC_NULL;
    return OCERZ_OBJC_OK;
}

int ocerz_objc_read_ro(uint64_t addr, OcerzObjcRo *out)
{
    memset(out, 0, sizeof *out);
    if (!addr)
        return OCERZ_OBJC_NULL;
    out->flags = oc_u32(addr);
    out->instance_start = oc_u32(addr + 4);
    out->instance_size = oc_u32(addr + 8);
    out->reserved = oc_u32(addr + 12);
    out->ivar_layout = oc_word(addr + 16);
    out->name = oc_word(addr + 24);
    out->base_methods = oc_word(addr + 32);
    out->base_protocols = oc_word(addr + 40);
    out->ivars = oc_word(addr + 48);
    out->weak_ivar_layout = oc_word(addr + 56);
    out->base_properties = oc_word(addr + 64);
    if (out->flags & OC_RO_SWIFT_INIT)
        return OCERZ_OBJC_SWIFT;
    return OCERZ_OBJC_OK;
}

static int oc_list(uint64_t addr, uint32_t flagmask, uint32_t minsize, OcerzObjcList *out)
{
    memset(out, 0, sizeof *out);
    out->addr = addr;
    if (!addr)
        return OCERZ_OBJC_OK;
    uint32_t word = oc_u32(addr);
    out->flags = word & flagmask;
    out->entsize = word & ~flagmask;
    out->count = oc_u32(addr + 4);
    if (out->count && out->entsize < minsize)
        return OCERZ_OBJC_BAD_LIST;
    return OCERZ_OBJC_OK;
}

int ocerz_objc_method_list(uint64_t addr, OcerzObjcList *out)
{
    int rc = oc_list(addr, OC_METHOD_FLAGS, 12, out);
    if (rc != OCERZ_OBJC_OK || !out->count)
        return rc;
    if (out->flags & OC_METHOD_RELATIVE)
        return out->entsize == 12 ? OCERZ_OBJC_OK : OCERZ_OBJC_BAD_LIST;
    return out->entsize >= 24 ? OCERZ_OBJC_OK : OCERZ_OBJC_BAD_LIST;
}

int ocerz_objc_method_at(const OcerzObjcList *list, uint32_t index, OcerzObjcMethod *out)
{
    memset(out, 0, sizeof *out);
    if (!list->addr || index >= list->count)
        return OCERZ_OBJC_BAD_LIST;
    uint64_t e = list->addr + 8 + (uint64_t)index * list->entsize;
    if (!(list->flags & OC_METHOD_RELATIVE)) {
        out->name = oc_word(e);
        out->types = oc_word(e + 8);
        out->imp = oc_word(e + 16);
        return OCERZ_OBJC_OK;
    }
    uint64_t ref = e + (uint64_t)(int64_t)oc_rel(e);
    out->name = (list->flags & OC_METHOD_DIRECT_SEL) ? ref : oc_word(ref);
    out->types = e + 4 + (uint64_t)(int64_t)oc_rel(e + 4);
    int32_t imp = oc_rel(e + 8);
    out->imp = imp ? e + 8 + (uint64_t)(int64_t)imp : 0;
    return OCERZ_OBJC_OK;
}

int ocerz_objc_ivar_list(uint64_t addr, OcerzObjcList *out)
{
    return oc_list(addr, 0, 32, out);
}

int ocerz_objc_ivar_at(const OcerzObjcList *list, uint32_t index, OcerzObjcIvar *out)
{
    memset(out, 0, sizeof *out);
    if (!list->addr || index >= list->count)
        return OCERZ_OBJC_BAD_LIST;
    uint64_t e = list->addr + 8 + (uint64_t)index * list->entsize;
    out->offset = oc_word(e);
    out->name = oc_word(e + 8);
    out->type = oc_word(e + 16);
    out->alignment = oc_u32(e + 24);
    out->size = oc_u32(e + 28);
    return OCERZ_OBJC_OK;
}

int ocerz_objc_property_list(uint64_t addr, OcerzObjcList *out)
{
    return oc_list(addr, 0, 16, out);
}

int ocerz_objc_property_at(const OcerzObjcList *list, uint32_t index, OcerzObjcProperty *out)
{
    memset(out, 0, sizeof *out);
    if (!list->addr || index >= list->count)
        return OCERZ_OBJC_BAD_LIST;
    uint64_t e = list->addr + 8 + (uint64_t)index * list->entsize;
    out->name = oc_word(e);
    out->attributes = oc_word(e + 8);
    return OCERZ_OBJC_OK;
}

uint64_t ocerz_objc_protocol_count(uint64_t list)
{
    return list ? oc_word(list) : 0;
}

uint64_t ocerz_objc_protocol_ref(uint64_t list, uint64_t index)
{
    return oc_word(list + 8 + 8 * index);
}

int ocerz_objc_read_category(uint64_t addr, int class_properties, OcerzObjcCategory *out)
{
    memset(out, 0, sizeof *out);
    if (!addr)
        return OCERZ_OBJC_NULL;
    out->name = oc_word(addr);
    out->cls = oc_word(addr + 8);
    out->instance_methods = oc_word(addr + 16);
    out->class_methods = oc_word(addr + 24);
    out->protocols = oc_word(addr + 32);
    out->instance_properties = oc_word(addr + 40);
    out->class_properties = class_properties ? oc_word(addr + 48) : 0;
    return OCERZ_OBJC_OK;
}

int ocerz_objc_read_protocol(uint64_t addr, OcerzObjcProtocol *out)
{
    memset(out, 0, sizeof *out);
    if (!addr)
        return OCERZ_OBJC_NULL;
    out->name = oc_word(addr + 8);
    out->protocols = oc_word(addr + 16);
    out->instance_methods = oc_word(addr + 24);
    out->class_methods = oc_word(addr + 32);
    out->optional_instance_methods = oc_word(addr + 40);
    out->optional_class_methods = oc_word(addr + 48);
    out->instance_properties = oc_word(addr + 56);
    out->size = oc_u32(addr + 64);
    out->flags = oc_u32(addr + 68);
    if (out->size >= OC_PROTOCOL_BASE + 8)
        out->extended_types = oc_word(addr + 72);
    if (out->size >= OC_PROTOCOL_BASE + 16)
        out->demangled_name = oc_word(addr + 80);
    if (out->size >= OC_PROTOCOL_BASE + 24)
        out->class_properties = oc_word(addr + 88);
    if (!out->name)
        return OCERZ_OBJC_NULL;
    return OCERZ_OBJC_OK;
}

int ocerz_objc_method_notation(const char *types, char *out, size_t outlen)
{
    int nargs = 0;
    int rc = ocerz_objc_notation(types, out, outlen, &nargs, NULL, NULL);
    if (rc != OCERZ_OBJC_OK)
        return rc;
    OcerzAbiSig sig;
    if (ocerz_abi_parse(out, &sig) != OCERZ_OK)
        return OCERZ_OBJC_ENGINE;
    if (sig.nargs < 2 || sig.arg[0] != 'p' || sig.arg[1] != 'p')
        return OCERZ_OBJC_NOT_METHOD;
    return OCERZ_OBJC_OK;
}

int ocerz_objc_property_attributes(const char *attrs, OcerzObjcAttribute *out, int max, char *storage,
                                   size_t storagelen)
{
    int n = 0;
    size_t used = 0;
    const char *p = attrs ? attrs : "";
    while (*p) {
        while (*p == ',')
            p++;
        if (!*p)
            break;
        if (n >= max)
            return -1;
        const char *value = p + 1;
        size_t vlen = strcspn(value, ",");
        if (used + 2 + vlen + 1 > storagelen)
            return -1;
        out[n].name = storage + used;
        storage[used++] = *p;
        storage[used++] = '\0';
        out[n].value = storage + used;
        memcpy(storage + used, value, vlen);
        used += vlen;
        storage[used++] = '\0';
        n++;
        p = value + vlen;
    }
    return n;
}

typedef struct OcOrder {
    uint64_t cls;
    int index;
} OcOrder;

static int oc_order_cmp(const void *a, const void *b)
{
    const OcOrder *x = a, *y = b;
    return x->cls < y->cls ? -1 : x->cls > y->cls ? 1 : x->index - y->index;
}

static int oc_order_find(const OcOrder *sorted, int n, uint64_t cls)
{
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (sorted[mid].cls < cls)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n && sorted[lo].cls == cls ? sorted[lo].index : -1;
}

int ocerz_objc_class_order(const uint64_t *classes, const uint64_t *supers, int n, int *order, int *culprit)
{
    if (culprit)
        *culprit = -1;
    if (n <= 0)
        return OCERZ_OBJC_OK;
    OcOrder *sorted = malloc((size_t)n * sizeof *sorted);
    unsigned char *state = calloc((size_t)n, 1);
    int *stack = malloc((size_t)n * sizeof *stack);
    if (!sorted || !state || !stack) {
        free(sorted);
        free(state);
        free(stack);
        return OCERZ_OBJC_ENGINE;
    }
    for (int i = 0; i < n; i++) {
        sorted[i].cls = classes[i];
        sorted[i].index = i;
    }
    qsort(sorted, (size_t)n, sizeof *sorted, oc_order_cmp);

    int emitted = 0, rc = OCERZ_OBJC_OK;
    for (int i = 0; i < n && rc == OCERZ_OBJC_OK; i++) {
        if (state[i])
            continue;
        int depth = 0;
        int k = i;
        while (k >= 0 && !state[k]) {
            state[k] = 1;
            stack[depth++] = k;
            k = supers[k] ? oc_order_find(sorted, n, supers[k]) : -1;
        }
        if (k >= 0 && state[k] == 1) {
            rc = OCERZ_OBJC_CYCLE;
            if (culprit)
                *culprit = k;
            break;
        }
        while (depth > 0) {
            int j = stack[--depth];
            state[j] = 2;
            order[emitted++] = j;
        }
    }
    free(sorted);
    free(state);
    free(stack);
    return rc;
}

typedef struct OcMap {
    uint64_t *keys;
    uint64_t *vals;
    size_t cap;
    size_t n;
} OcMap;

static size_t oc_hash(uint64_t k, size_t cap)
{
    k *= 0x9e3779b97f4a7c15ull;
    return (size_t)(k ^ (k >> 29)) & (cap - 1);
}

static uint64_t *oc_map_find(const OcMap *m, uint64_t key)
{
    if (!m->cap)
        return NULL;
    for (size_t i = oc_hash(key, m->cap);; i = (i + 1) & (m->cap - 1)) {
        if (m->keys[i] == key)
            return &m->vals[i];
        if (!m->keys[i])
            return NULL;
    }
}

static uint64_t *oc_map_put(OcMap *m, uint64_t key, uint64_t val)
{
    if ((m->n + 1) * 2 > m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 256;
        uint64_t *keys = calloc(cap, sizeof *keys), *vals = calloc(cap, sizeof *vals);
        if (!keys || !vals)
            oc_stop("no memory for the table of guest Objective-C definitions");
        for (size_t i = 0; i < m->cap; i++) {
            if (!m->keys[i])
                continue;
            size_t j = oc_hash(m->keys[i], cap);
            while (keys[j])
                j = (j + 1) & (cap - 1);
            keys[j] = m->keys[i];
            vals[j] = m->vals[i];
        }
        free(m->keys);
        free(m->vals);
        m->keys = keys;
        m->vals = vals;
        m->cap = cap;
    }
    size_t i = oc_hash(key, m->cap);
    while (m->keys[i] && m->keys[i] != key)
        i = (i + 1) & (m->cap - 1);
    if (!m->keys[i]) {
        m->keys[i] = key;
        m->n++;
    }
    m->vals[i] = val;
    return &m->vals[i];
}

typedef struct OcDefined {
    uint64_t meta;
    uint64_t load;
    int scheduled;
} OcDefined;

typedef struct OcLoad {
    uint64_t cls;
    uint64_t imp;
    int category;
} OcLoad;

typedef struct OcDead {
    struct OcDead *next;
    void *cls;
    void *sel;
    const char *types;
    char why[];
} OcDead;

static pthread_mutex_t g_oc_lock = PTHREAD_MUTEX_INITIALIZER;
static OcMap g_oc_images;
static OcMap g_oc_classes;
static OcMap g_oc_protocols;
static OcLoad *g_oc_loads;
static size_t g_oc_loads_n, g_oc_loads_cap;
static OcDead *_Atomic g_oc_dead;

static OcDefined *oc_defined(uint64_t cls)
{
    uint64_t *v = oc_map_find(&g_oc_classes, cls);
    return v ? (OcDefined *)(uintptr_t)*v : NULL;
}

int ocerz_objcbridge_is_defined(uint64_t cls)
{
    pthread_mutex_lock(&g_oc_lock);
    int yes = oc_defined(cls) != NULL;
    pthread_mutex_unlock(&g_oc_lock);
    return yes;
}

static void oc_dead_imp(void *self, void *cmd)
{
    void *start = self ? oc_object_getClass(self) : NULL;
    for (void *c = start; c; c = oc_superclass(c)) {
        for (const OcDead *d = g_oc_dead; d; d = d->next) {
            if (d->cls == c && d->sel == cmd)
                oc_stop("%c[%s %s] is guest code native code cannot call: its type encoding %s has %s",
                        oc_is_meta(c) ? '+' : '-', oc_class_name(c), oc_sel_name(cmd),
                        d->types ? d->types : "(none)", d->why);
        }
    }
    oc_stop("a guest method native code cannot call was called%s%s", start ? " on a " : "",
            start ? oc_class_name(start) : "");
}

void *ocerz_objcbridge_dead_imp(void)
{
    return (void *)oc_dead_imp;
}

static void *oc_bury(void *cls, const char *clsname, int meta, void *sel, const char *types, const char *why)
{
    size_t len = strlen(why);
    OcDead *d = malloc(sizeof *d + len + 1);
    if (!d)
        oc_stop("no memory to record why a guest method cannot be called");
    d->cls = cls;
    d->sel = sel;
    d->types = types;
    memcpy(d->why, why, len + 1);
    d->next = g_oc_dead;
    g_oc_dead = d;
    if (oc_logging())
        fprintf(stderr, "ocerz: OBJCLOG[%d] define %c[%s %s] %s has no crossing: %s\n", (int)getpid(),
                meta ? '+' : '-', clsname, oc_sel_name(sel), types ? types : "(none)", why);
    return (void *)oc_dead_imp;
}

static void *oc_imp(void *cls, const char *clsname, int meta, void *sel, const char *types, uint64_t imp)
{
    if (!imp)
        return NULL;
    if (!ocerz_abi_is_guest_code(imp))
        return ocerz_g2h(imp);
    char notation[OCERZ_OBJC_NOTATION_MAX];
    int rc = ocerz_objc_method_notation(types, notation, sizeof notation);
    if (rc != OCERZ_OBJC_OK)
        return oc_bury(cls, clsname, meta, sel, types, ocerz_objc_refusal(rc));
    void *tramp = ocerz_abi_callback_intern(imp, notation);
    if (!tramp)
        return oc_bury(cls, clsname, meta, sel, types, "no slot left in the callback bank");
    if (oc_logging())
        fprintf(stderr, "ocerz: OBJCLOG[%d] define %c[%s %s] %s at %#llx\n", (int)getpid(), meta ? '+' : '-',
                clsname, oc_sel_name(sel), notation, (unsigned long long)imp);
    return tramp;
}

static void *oc_alloc(size_t bytes, const char *what, const char *clsname)
{
    void *p = calloc(1, bytes);
    if (!p)
        oc_stop("no memory for the %s of guest class %s", what, clsname);
    return p;
}

static void *oc_methods(uint64_t list, void *cls, const char *clsname, int meta, uint64_t *load)
{
    OcerzObjcList l;
    int rc = ocerz_objc_method_list(list, &l);
    if (rc != OCERZ_OBJC_OK)
        oc_stop("guest class %s has a %s method list at %#llx whose entries are %u bytes, which ocerz"
                " cannot read", clsname, meta ? "class" : "instance", (unsigned long long)list, l.entsize);
    if (!l.count)
        return NULL;
    uint8_t *copy = oc_alloc(8 + 24 * (size_t)l.count, "method list", clsname);
    uint32_t head = 24, count = l.count;
    memcpy(copy, &head, 4);
    memcpy(copy + 4, &count, 4);
    void *load_sel = load ? oc_sel("load") : NULL;
    for (uint32_t i = 0; i < l.count; i++) {
        OcerzObjcMethod m;
        ocerz_objc_method_at(&l, i, &m);
        const char *name = oc_str(m.name);
        if (!name)
            oc_stop("guest class %s has a %s method with no selector", clsname, meta ? "class" : "instance");
        void *sel = oc_sel(name);
        const char *types = oc_str(m.types);
        void *imp = oc_imp(cls, clsname, meta, sel, types, m.imp);
        if (load && sel == load_sel && m.imp)
            *load = m.imp;
        uint64_t w[3] = { (uint64_t)(uintptr_t)sel, (uint64_t)(uintptr_t)types, (uint64_t)(uintptr_t)imp };
        memcpy(copy + 8 + 24 * (size_t)i, w, sizeof w);
    }
    return copy;
}

static void *oc_protocol(uint64_t ref, int depth);

static void oc_protocol_methods(void *proto, uint64_t list, int required, int instance, const char *name)
{
    OcerzObjcList l;
    if (ocerz_objc_method_list(list, &l) != OCERZ_OBJC_OK)
        oc_stop("guest protocol %s has a method list at %#llx whose entries are %u bytes, which ocerz"
                " cannot read", name, (unsigned long long)list, l.entsize);
    for (uint32_t i = 0; i < l.count; i++) {
        OcerzObjcMethod m;
        ocerz_objc_method_at(&l, i, &m);
        if (!m.name)
            continue;
        ((void (*)(void *, void *, const char *, bool, bool))oc_need(&g_oc_protocol_addMethodDescription))(
            proto, oc_sel(oc_str(m.name)), oc_str(m.types), required, instance);
    }
}

static int oc_attrs(uint64_t attrs, OcerzObjcAttribute *out, char *storage, size_t len, const char *owner,
                    const char *prop)
{
    int n = ocerz_objc_property_attributes(oc_str(attrs), out, OC_ATTRS_MAX, storage, len);
    if (n < 0)
        oc_stop("the property %s of %s has attributes \"%.200s\" longer than ocerz reads", prop, owner,
                oc_str(attrs));
    return n;
}

static void oc_protocol_properties(void *proto, uint64_t list, int instance, const char *name)
{
    OcerzObjcList l;
    if (ocerz_objc_property_list(list, &l) != OCERZ_OBJC_OK)
        oc_stop("guest protocol %s has a property list whose entries are %u bytes", name, l.entsize);
    for (uint32_t i = 0; i < l.count; i++) {
        OcerzObjcProperty p;
        OcerzObjcAttribute attrs[OC_ATTRS_MAX];
        char storage[1024];
        ocerz_objc_property_at(&l, i, &p);
        int n = oc_attrs(p.attributes, attrs, storage, sizeof storage, name, oc_str(p.name));
        ((void (*)(void *, const char *, const OcerzObjcAttribute *, unsigned, bool, bool))oc_need(
            &g_oc_protocol_addProperty))(proto, oc_str(p.name), attrs, (unsigned)n, true, instance);
    }
}

static void *oc_protocol(uint64_t ref, int depth)
{
    if (!ref)
        return NULL;
    if (!oc_is_guest(ref))
        return ocerz_g2h(ref);
    uint64_t *known = oc_map_find(&g_oc_protocols, ref);
    if (known)
        return (void *)(uintptr_t)*known;

    OcerzObjcProtocol gp;
    if (ocerz_objc_read_protocol(ref, &gp) != OCERZ_OBJC_OK)
        oc_stop("the guest protocol at %#llx has no name", (unsigned long long)ref);
    const char *name = oc_str(gp.name);
    void *native = ((void *(*)(const char *))oc_need(&g_oc_objc_getProtocol))(name);
    if (native) {
        oc_map_put(&g_oc_protocols, ref, (uint64_t)(uintptr_t)native);
        return native;
    }
    if (depth > 64)
        oc_stop("guest protocol %s adopts protocols more than 64 deep", name);

    void *made = ((void *(*)(const char *))oc_need(&g_oc_objc_allocateProtocol))(name);
    if (!made)
        oc_stop("the native runtime would not allocate guest protocol %s", name);
    oc_map_put(&g_oc_protocols, ref, (uint64_t)(uintptr_t)made);
    for (uint64_t i = 0, n = ocerz_objc_protocol_count(gp.protocols); i < n; i++) {
        void *adopted = oc_protocol(ocerz_objc_protocol_ref(gp.protocols, i), depth + 1);
        if (adopted)
            ((void (*)(void *, void *))oc_need(&g_oc_protocol_addProtocol))(made, adopted);
    }
    oc_protocol_methods(made, gp.instance_methods, 1, 1, name);
    oc_protocol_methods(made, gp.class_methods, 1, 0, name);
    oc_protocol_methods(made, gp.optional_instance_methods, 0, 1, name);
    oc_protocol_methods(made, gp.optional_class_methods, 0, 0, name);
    oc_protocol_properties(made, gp.instance_properties, 1, name);
    oc_protocol_properties(made, gp.class_properties, 0, name);
    ((void (*)(void *))oc_need(&g_oc_objc_registerProtocol))(made);
    if (oc_logging())
        fprintf(stderr, "ocerz: OBJCLOG[%d] define protocol %s\n", (int)getpid(), name);
    return made;
}

static void *oc_protocol_list(uint64_t list, const char *owner)
{
    uint64_t n = ocerz_objc_protocol_count(list);
    if (!list || !n)
        return NULL;
    uint64_t *copy = oc_alloc(8 * ((size_t)n + 1), "protocol list", owner);
    copy[0] = n;
    for (uint64_t i = 0; i < n; i++)
        copy[i + 1] = (uint64_t)(uintptr_t)oc_protocol(ocerz_objc_protocol_ref(list, i), 0);
    return copy;
}

typedef struct OcSect {
    uint64_t addr;
    uint64_t size;
} OcSect;

typedef struct OcImage {
    OcSect classlist;
    OcSect nlclslist;
    OcSect catlist;
    OcSect catlist2;
    OcSect nlcatlist;
    OcSect protolist;
    OcSect protorefs;
    OcSect imageinfo;
} OcImage;

static void oc_scan(const uint8_t *mh, int64_t slide, OcImage *im)
{
    static const struct {
        const char *name;
        size_t off;
    } kSects[] = {
        { "__objc_classlist", offsetof(OcImage, classlist) },
        { "__objc_nlclslist", offsetof(OcImage, nlclslist) },
        { "__objc_catlist", offsetof(OcImage, catlist) },
        { "__objc_catlist2", offsetof(OcImage, catlist2) },
        { "__objc_nlcatlist", offsetof(OcImage, nlcatlist) },
        { "__objc_protolist", offsetof(OcImage, protolist) },
        { "__objc_protorefs", offsetof(OcImage, protorefs) },
        { "__objc_imageinfo", offsetof(OcImage, imageinfo) },
    };
    struct mach_header_64 h;
    memset(im, 0, sizeof *im);
    if (!mh)
        return;
    memcpy(&h, mh, sizeof h);
    if (h.magic != MH_MAGIC_64)
        return;
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
                for (size_t k = 0; k < sizeof kSects / sizeof kSects[0]; k++) {
                    if (strncmp(sc.sectname, kSects[k].name, sizeof sc.sectname) != 0)
                        continue;
                    OcSect *out = (OcSect *)((char *)im + kSects[k].off);
                    if (!out->addr) {
                        out->addr = (uint64_t)((int64_t)sc.addr + slide);
                        out->size = sc.size;
                    }
                }
            }
        }
        lc += l.cmdsize;
    }
}

static const char *oc_ro_name(uint64_t ro)
{
    const char *name = ro ? oc_str(oc_word(ro + 24)) : NULL;
    return name ? name : "(unnamed)";
}

static const char *oc_class_label(uint64_t cls)
{
    if (!cls)
        return "(null)";
    if (!oc_is_guest(cls))
        return oc_class_name(ocerz_g2h(cls));
    OcerzObjcClass c;
    ocerz_objc_read_class(cls, &c);
    return oc_ro_name(c.ro);
}

static uint8_t *oc_ro_copy(uint64_t ro, void *methods, void *protocols, const char *clsname)
{
    uint8_t *copy = oc_alloc(OC_RO_BYTES, "class_ro_t", clsname);
    memcpy(copy, ocerz_g2h(ro), OC_RO_BYTES);
    uint64_t m = (uint64_t)(uintptr_t)methods, p = (uint64_t)(uintptr_t)protocols;
    memcpy(copy + 32, &m, 8);
    memcpy(copy + 40, &p, 8);
    return copy;
}

static void oc_define_class(uint64_t addr, uint32_t image_flags)
{
    if (oc_defined(addr))
        return;
    OcerzObjcClass c, mc;
    OcerzObjcRo ro, mro;
    int rc = ocerz_objc_read_class(addr, &c);
    if (rc == OCERZ_OBJC_SWIFT)
        oc_stop("guest class at %#llx is a Swift class, and Swift classes do not cross",
                (unsigned long long)addr);
    if (rc != OCERZ_OBJC_OK)
        oc_stop("guest class at %#llx has no class_ro_t", (unsigned long long)addr);
    if (ocerz_objc_read_ro(c.ro, &ro) == OCERZ_OBJC_SWIFT)
        oc_stop("guest class %s has a Swift metadata initializer, and Swift classes do not cross",
                oc_ro_name(c.ro));
    const char *name = oc_ro_name(c.ro);
    if (ro.flags & (OC_RO_FUTURE | OC_RO_REALIZED))
        oc_stop("guest class %s carries class_ro_t flags %#x that only a running runtime sets", name, ro.flags);
    if (ro.flags & OC_RO_ROOT)
        oc_stop("guest class %s is a root class, with no superclass, and ocerz defines guest classes only"
                " under a native root such as NSObject", name);
    if (!c.superclass)
        oc_stop("guest class %s has a null superclass, as a class whose weak-linked superclass the host lacks"
                " does, and ocerz does not leave such a class out the way the native runtime would", name);
    if (oc_is_guest(c.superclass) && !oc_defined(c.superclass))
        oc_stop("guest class %s has the superclass %s at %#llx, which is not a class ocerz has defined", name,
                oc_class_label(c.superclass), (unsigned long long)c.superclass);
    if (!oc_is_guest(c.isa) || ocerz_objc_read_class(c.isa, &mc) != OCERZ_OBJC_OK ||
        ocerz_objc_read_ro(mc.ro, &mro) != OCERZ_OBJC_OK || !(mro.flags & OC_RO_META))
        oc_stop("guest class %s has no guest metaclass at %#llx", name, (unsigned long long)c.isa);

    void *cls = ocerz_g2h(addr), *meta = ocerz_g2h(c.isa);
    uint64_t load = 0;
    void *protos = oc_protocol_list(ro.base_protocols, name);
    void *mprotos =
        mro.base_protocols == ro.base_protocols ? protos : oc_protocol_list(mro.base_protocols, name);
    void *im = oc_methods(ro.base_methods, cls, name, 0, NULL);
    void *cm = oc_methods(mro.base_methods, meta, name, 1, &load);
    uint8_t *rocopy = oc_ro_copy(c.ro, im, protos, name);
    uint8_t *mrocopy = oc_ro_copy(mc.ro, cm, mprotos, name);

    uint64_t empty = ocerz_h2g(oc_need(&g_oc_empty_cache));
    ocerz_st(addr + 16, 8, empty);
    ocerz_st(addr + 24, 8, 0);
    ocerz_st(addr + 32, 8, ocerz_h2g(rocopy));
    ocerz_st(c.isa + 16, 8, empty);
    ocerz_st(c.isa + 24, 8, 0);
    ocerz_st(c.isa + 32, 8, ocerz_h2g(mrocopy));

    struct {
        uint32_t version;
        uint32_t flags;
    } info = { 0, image_flags };
    void *got = ((void *(*)(void *, void *))oc_need(&g_oc_readClassPair))(cls, &info);
    if (got != cls)
        oc_stop("the native runtime would not read guest class %s at %#llx (objc_readClassPair gave %p)", name,
                (unsigned long long)addr, got);

    OcDefined *d = oc_alloc(sizeof *d, "record", name);
    d->meta = c.isa;
    d->load = load;
    oc_map_put(&g_oc_classes, addr, (uint64_t)(uintptr_t)d);
    if (oc_logging())
        fprintf(stderr, "ocerz: OBJCLOG[%d] define class %s at %#llx, superclass %s, %u bytes\n", (int)getpid(),
                name, (unsigned long long)addr, oc_class_label(c.superclass), ro.instance_size);
}

static void oc_define_classes(const OcSect *list, uint32_t image_flags)
{
    int n = (int)(list->size / 8);
    if (n <= 0)
        return;
    uint64_t *classes = malloc((size_t)n * sizeof *classes), *supers = malloc((size_t)n * sizeof *supers);
    int *order = malloc((size_t)n * sizeof *order);
    if (!classes || !supers || !order)
        oc_stop("no memory to order %d guest classes", n);
    for (int i = 0; i < n; i++) {
        classes[i] = oc_word(list->addr + 8 * (uint64_t)i);
        OcerzObjcClass c;
        ocerz_objc_read_class(classes[i], &c);
        supers[i] = c.superclass;
    }
    int culprit = -1;
    int rc = ocerz_objc_class_order(classes, supers, n, order, &culprit);
    if (rc == OCERZ_OBJC_CYCLE)
        oc_stop("guest class %s is its own superclass, through a chain of superclasses",
                oc_class_label(classes[culprit]));
    if (rc != OCERZ_OBJC_OK)
        oc_stop("cannot order %d guest classes by superclass: %s", n, ocerz_objc_refusal(rc));
    for (int i = 0; i < n; i++)
        oc_define_class(classes[order[i]], image_flags);
    free(classes);
    free(supers);
    free(order);
}

static void *oc_category_class(const OcerzObjcCategory *cat, uint64_t addr)
{
    const char *catname = oc_str(cat->name);
    if (!cat->cls) {
        OCERZ_LOG("objc: category %s at %#llx names no class, as a category on a missing weak class does,"
                  " and is left out\n", catname ? catname : "(unnamed)", (unsigned long long)addr);
        return NULL;
    }
    if (oc_is_guest(cat->cls) && !oc_defined(cat->cls))
        oc_stop("guest category %s is on the class %s at %#llx, which is not a class ocerz has defined",
                catname ? catname : "(unnamed)", oc_class_label(cat->cls), (unsigned long long)cat->cls);
    return ocerz_g2h(cat->cls);
}

static void oc_category_methods(void *cls, uint64_t list, int meta, const char *catname)
{
    OcerzObjcList l;
    const char *clsname = oc_class_name(cls);
    if (ocerz_objc_method_list(list, &l) != OCERZ_OBJC_OK)
        oc_stop("guest category %s on %s has a method list whose entries are %u bytes, which ocerz cannot read",
                catname, clsname, l.entsize);
    for (uint32_t i = 0; i < l.count; i++) {
        OcerzObjcMethod m;
        ocerz_objc_method_at(&l, i, &m);
        const char *name = oc_str(m.name);
        if (!name)
            continue;
        void *sel = oc_sel(name);
        const char *types = oc_str(m.types);
        void *imp = oc_imp(cls, clsname, meta, sel, types, m.imp);
        ((void *(*)(void *, void *, void *, const char *))oc_need(&g_oc_class_replaceMethod))(cls, sel, imp,
                                                                                               types);
    }
}

static void oc_category_properties(void *cls, uint64_t list, const char *catname)
{
    OcerzObjcList l;
    if (ocerz_objc_property_list(list, &l) != OCERZ_OBJC_OK)
        oc_stop("guest category %s has a property list whose entries are %u bytes", catname, l.entsize);
    for (uint32_t i = 0; i < l.count; i++) {
        OcerzObjcProperty p;
        OcerzObjcAttribute attrs[OC_ATTRS_MAX];
        char storage[1024];
        ocerz_objc_property_at(&l, i, &p);
        int n = oc_attrs(p.attributes, attrs, storage, sizeof storage, catname, oc_str(p.name));
        ((void (*)(void *, const char *, const OcerzObjcAttribute *, unsigned))oc_need(
            &g_oc_class_replaceProperty))(cls, oc_str(p.name), attrs, (unsigned)n);
    }
}

static void oc_define_category(uint64_t addr, uint32_t image_flags)
{
    OcerzObjcCategory cat;
    if (ocerz_objc_read_category(addr, (image_flags & OC_IMAGE_CLASS_PROPERTIES) != 0, &cat) != OCERZ_OBJC_OK)
        return;
    void *cls = oc_category_class(&cat, addr);
    if (!cls)
        return;
    const char *catname = oc_str(cat.name) ? oc_str(cat.name) : "(unnamed)";
    void *meta = oc_object_getClass(cls);
    oc_category_methods(cls, cat.instance_methods, 0, catname);
    oc_category_methods(meta, cat.class_methods, 1, catname);
    for (uint64_t i = 0, n = ocerz_objc_protocol_count(cat.protocols); i < n; i++) {
        void *proto = oc_protocol(ocerz_objc_protocol_ref(cat.protocols, i), 0);
        if (proto)
            ((bool (*)(void *, void *))oc_need(&g_oc_class_addProtocol))(cls, proto);
    }
    oc_category_properties(cls, cat.instance_properties, catname);
    oc_category_properties(meta, cat.class_properties, catname);
    if (oc_logging())
        fprintf(stderr, "ocerz: OBJCLOG[%d] define category %s(%s)\n", (int)getpid(), oc_class_name(cls),
                catname);
}

static void oc_queue_load(uint64_t cls, uint64_t imp, int category)
{
    if (g_oc_loads_n == g_oc_loads_cap) {
        size_t cap = g_oc_loads_cap ? g_oc_loads_cap * 2 : 64;
        OcLoad *grown = realloc(g_oc_loads, cap * sizeof *grown);
        if (!grown)
            oc_stop("no memory to queue +load methods");
        g_oc_loads = grown;
        g_oc_loads_cap = cap;
    }
    g_oc_loads[g_oc_loads_n].cls = cls;
    g_oc_loads[g_oc_loads_n].imp = imp;
    g_oc_loads[g_oc_loads_n].category = category;
    g_oc_loads_n++;
}

static void oc_schedule_class(uint64_t cls)
{
    OcDefined *d = oc_defined(cls);
    if (!d || d->scheduled)
        return;
    OcerzObjcClass c;
    ocerz_objc_read_class(cls, &c);
    if (oc_is_guest(c.superclass))
        oc_schedule_class(c.superclass);
    d->scheduled = 1;
    if (d->load)
        oc_queue_load(cls, d->load, 0);
}

static void oc_schedule_category(uint64_t addr, uint32_t image_flags)
{
    OcerzObjcCategory cat;
    OcerzObjcList l;
    if (ocerz_objc_read_category(addr, (image_flags & OC_IMAGE_CLASS_PROPERTIES) != 0, &cat) != OCERZ_OBJC_OK ||
        !cat.cls || ocerz_objc_method_list(cat.class_methods, &l) != OCERZ_OBJC_OK)
        return;
    for (uint32_t i = 0; i < l.count; i++) {
        OcerzObjcMethod m;
        ocerz_objc_method_at(&l, i, &m);
        const char *name = oc_str(m.name);
        if (name && m.imp && strcmp(name, "load") == 0) {
            oc_queue_load(cat.cls, m.imp, 1);
            return;
        }
    }
}

int ocerz_objcbridge_define_image(const uint8_t *mh, int64_t slide)
{
    OcImage im;
    oc_scan(mh, slide, &im);
    if (!im.classlist.size && !im.catlist.size && !im.catlist2.size && !im.protolist.size && !im.protorefs.size)
        return 0;

    pthread_mutex_lock(&g_oc_lock);
    uint64_t key = (uint64_t)(uintptr_t)mh;
    if (oc_map_find(&g_oc_images, key)) {
        pthread_mutex_unlock(&g_oc_lock);
        return 0;
    }
    oc_map_put(&g_oc_images, key, 1);
    uint32_t flags = im.imageinfo.size >= 8 ? oc_u32(im.imageinfo.addr + 4) : 0;
    int defined = 0;

    for (uint64_t off = 0; off + 8 <= im.protolist.size; off += 8)
        oc_protocol(oc_word(im.protolist.addr + off), 0);
    for (uint64_t off = 0; off + 8 <= im.protorefs.size; off += 8) {
        uint64_t word = im.protorefs.addr + off, ref = oc_word(word);
        void *native = oc_protocol(ref, 0);
        if (native && ocerz_h2g(native) != ref)
            ocerz_st(word, 8, ocerz_h2g(native));
    }

    oc_define_classes(&im.classlist, flags);
    defined += (int)(im.classlist.size / 8);
    for (uint64_t off = 0; off + 8 <= im.catlist.size; off += 8, defined++)
        oc_define_category(oc_word(im.catlist.addr + off), flags);
    for (uint64_t off = 0; off + 8 <= im.catlist2.size; off += 8, defined++)
        oc_define_category(oc_word(im.catlist2.addr + off), flags);

    for (uint64_t off = 0; off + 8 <= im.nlclslist.size; off += 8)
        oc_schedule_class(oc_word(im.nlclslist.addr + off));
    for (uint64_t off = 0; off + 8 <= im.nlcatlist.size; off += 8)
        oc_schedule_category(oc_word(im.nlcatlist.addr + off), flags);
    pthread_mutex_unlock(&g_oc_lock);
    return defined;
}

int ocerz_objcbridge_run_loads(struct OcerzVM *vm, uint64_t stack_top)
{
    int ran = 0;
    for (;;) {
        pthread_mutex_lock(&g_oc_lock);
        OcLoad *loads = g_oc_loads;
        size_t n = g_oc_loads_n;
        g_oc_loads = NULL;
        g_oc_loads_n = g_oc_loads_cap = 0;
        pthread_mutex_unlock(&g_oc_lock);
        if (!n) {
            free(loads);
            return ran;
        }
        void *pool = ((void *(*)(void))oc_need(&g_oc_pool_push))();
        uint64_t sel = ocerz_h2g(oc_sel("load"));
        for (size_t i = 0; i < n && !vm->exited; i++) {
            if (oc_logging())
                fprintf(stderr, "ocerz: OBJCLOG[%d] +[%s load]%s at %#llx\n", (int)getpid(),
                        oc_class_name(ocerz_g2h(loads[i].cls)), loads[i].category ? " (category)" : "",
                        (unsigned long long)loads[i].imp);
            uint64_t args[2] = { loads[i].cls, sel };
            ocerz_vm_call(vm, loads[i].imp, args, 2, stack_top);
            ran++;
        }
        if (!vm->exited)
            ((void (*)(void *))oc_need(&g_oc_pool_pop))(pool);
        free(loads);
        if (vm->exited)
            return ran;
    }
}

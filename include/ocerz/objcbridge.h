/*
 * Objective-C messages and formatted output from x86 guests, crossed into the
 * host's native arm64 runtime.
 *
 * In native mode an Intel program's imports from libobjc and Foundation bind to
 * synthesized images like every other system import, and the exports that no
 * fixed signature can describe are specials answered here: the objc_msgSend
 * family, objc_setUncaughtExceptionHandler, NSLog, the printf family and
 * CoreFoundation's two format functions.  The classes, categories and protocols
 * a guest image defines are made the native runtime's own here too.
 * src/objcbridge.c says how sends and formats work and src/objcclass.c how
 * definitions do; this header is the contract with the rest of ocerz.
 *
 * ---- selectors ----
 * A guest image's __objc_selrefs entries point at the image's own selector
 * strings, and a guest compares selectors by address, so every entry has to
 * hold the native runtime's canonical SEL before guest code runs.
 * ocerz_objcbridge_fix_selrefs does that for one mapped image: mh is the
 * image's mach header as the host sees it and slide what the loader added to
 * its addresses.  Every 8-byte entry of a __objc_selrefs section in a segment
 * whose name starts with __DATA becomes sel_registerName of the string it
 * points at, and the selector word at offset 8 of every 16-byte entry of a
 * __objc_msgrefs section is rewritten the same way, the implementation word
 * beside it left as it was bound.  The answer is the number of words
 * rewritten, or -1 when an image has entries and the host runtime has no
 * sel_registerName.  The loader calls it in native mode for the main image and
 * for every guest dylib after that image's fixups.
 *
 * ---- the uncaught-exception wall ----
 * ocerz_objcbridge_install_uncaught installs ocerz's own native uncaught
 * Objective-C exception handler, once per process, in front of whichever one
 * the native frameworks installed.  The loader calls it in native mode once a
 * guest links libobjc, before guest code runs.
 *
 * ---- type encodings ----
 * ocerz_objc_notation turns a native method type encoding, as
 * method_getTypeEncoding gives it, into the ABI notation of abi.h.  It returns
 * OCERZ_OBJC_OK and writes the notation, the argument count and two bit masks
 * of the argument positions that are blocks (@?) and function pointers (^?), or
 * returns one of the refusals below, which ocerz_objc_refusal names in words.
 *
 * ---- formats ----
 * ocerz_objc_format_classes reads a format string in one of the dialects below
 * and writes one class per argument it consumes, in order: i for an int-sized
 * integer, l for a long-sized one, L for a word whose value is printed rather
 * than followed (%p), p for a pointer that is followed, d for a double.  It
 * returns how many it wrote, or -1 with *why saying which conversion it
 * refuses.  OCERZ_OBJC_FMT_TYPES reads an Objective-C type list instead, one
 * pointer per type, the shape -encodeValuesOfObjCTypes: takes.
 *
 * ocerz_objc_variadic answers whether a selector is one of the Foundation
 * methods declared with an ellipsis, and which argument, counting self as 0,
 * describes what follows.
 *
 * ---- guest classes ----
 * ocerz_objcbridge_define_image defines, in the native runtime, every
 * protocol, class and category of one mapped image, mh and slide as for
 * ocerz_objcbridge_fix_selrefs, whose pass must already have run over it.  The
 * sections are __objc_protolist, __objc_protorefs, __objc_classlist,
 * __objc_catlist, __objc_catlist2, __objc_nlclslist, __objc_nlcatlist and
 * __objc_imageinfo in any segment whose name starts with __DATA.  Every guest
 * class keeps its address; every method implementation native code reaches is
 * a callback slot bound to the guest's function, or ocerz_objcbridge_dead_imp
 * for a method whose types do not cross or for which the bank had no slot, and
 * that function stops the process naming the method when it is called; every
 * __objc_protorefs word holds a native protocol afterwards; and the image's
 * +load methods are queued.  The answer is the number of class and category
 * list entries, or 0 for an image with none of those sections or one defined
 * before.  A class or category that cannot be defined - a root class, a class
 * with a null superclass, a Swift class, a superclass or category class that is
 * a guest class not defined yet,
 * a chain of superclasses that loops, a list ocerz cannot read - stops the
 * process with OCERZ_BRIDGE_UNIMPL_EXIT and a line naming it.  The loader calls
 * it in native mode for every guest dylib and the main image, right after
 * ocerz_objcbridge_fix_selrefs, dependencies before the images that load them.
 *
 * ocerz_objcbridge_run_loads runs every queued +load, in queue order, as guest
 * code on the calling thread below stack_top, inside a native autorelease pool,
 * and answers how many ran; the queue is empty afterwards.  The loader calls it
 * in native mode once, after the guest's thread state is set up and before
 * main.  ocerz_objcbridge_is_defined answers whether ocerz defined a guest class
 * at that address.
 *
 * ---- guest layouts ----
 * The readers take guest addresses and read the LP64 layouts both
 * architectures share.  ocerz_objc_read_class splits a class_t into its words,
 * the data word's low bits masked off into swift, and refuses a Swift class or
 * a null data word.  ocerz_objc_read_ro reads a class_ro_t and refuses one with
 * a Swift metadata initializer.  ocerz_objc_method_list, ocerz_objc_ivar_list
 * and ocerz_objc_property_list read a list header - a null address is an empty
 * list - and refuse an entry size too small for the kind, or for a method list
 * an absolute one under 24 bytes or a relative one other than 12; the _at
 * functions read one entry and refuse an index past the count.  A relative
 * method entry is resolved to the same three addresses an absolute one holds:
 * the selector name through the selector reference its first offset names, or
 * directly when the list's direct-selector flag is set, the types string, and
 * the implementation, 0 for a zero offset.  ocerz_objc_protocol_count and
 * ocerz_objc_protocol_ref read a protocol_list_t, whose count is 64 bits.
 * ocerz_objc_read_category reads a category_t, its class properties only when
 * class_properties says the image's category_t has them.
 * ocerz_objc_read_protocol reads a protocol_t, the fields past 72 bytes only
 * when its size covers them.
 *
 * ocerz_objc_method_notation converts a guest method's x86 type string to a
 * notation as ocerz_objc_notation does and also refuses, as
 * OCERZ_OBJC_NOT_METHOD, types whose first two arguments are not pointers.
 * ocerz_objc_property_attributes splits a property attribute string into at
 * most max name and value pairs whose strings it writes into storage, a
 * one-letter name each and a value up to the next comma, and answers how many,
 * or -1 when max or storage is too small.  ocerz_objc_class_order writes into
 * order a permutation of the n classes in which each comes after its
 * superclass when that superclass is among them, and refuses a loop with
 * OCERZ_OBJC_CYCLE and the index of a class in it in culprit.
 */
#ifndef OCERZ_OBJCBRIDGE_H
#define OCERZ_OBJCBRIDGE_H

#include "ocerz/types.h"
#include "ocerz/cpu.h"

struct OcerzVM;

#define OCERZ_OBJC_LIBOBJC "/usr/lib/libobjc.A.dylib"
#define OCERZ_OBJC_FOUNDATION "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation"
#define OCERZ_OBJC_NOTATION_MAX 256
#define OCERZ_OBJC_VARIADIC_MAX 256

enum {
    OCERZ_OBJC_OK = 0,
    OCERZ_OBJC_UNION,
    OCERZ_OBJC_BITFIELD,
    OCERZ_OBJC_LONG_DOUBLE,
    OCERZ_OBJC_COMPLEX,
    OCERZ_OBJC_INT128,
    OCERZ_OBJC_UNKNOWN,
    OCERZ_OBJC_OPAQUE,
    OCERZ_OBJC_VOID_VALUE,
    OCERZ_OBJC_TOO_MANY_ARGS,
    OCERZ_OBJC_TOO_LONG,
    OCERZ_OBJC_ENGINE,
    OCERZ_OBJC_MALFORMED,
    OCERZ_OBJC_NOT_METHOD,
    OCERZ_OBJC_NULL,
    OCERZ_OBJC_SWIFT,
    OCERZ_OBJC_BAD_LIST,
    OCERZ_OBJC_CYCLE,
};

enum {
    OCERZ_OBJC_FMT_C = 1,
    OCERZ_OBJC_FMT_CF,
    OCERZ_OBJC_FMT_PREDICATE,
    OCERZ_OBJC_FMT_TYPES,
};

enum {
    OCERZ_OBJC_VA_FORMAT = 1,
    OCERZ_OBJC_VA_NIL_TERMINATED,
};

typedef struct OcerzObjcVariadic {
    const char *sel;
    int kind;
    int arg;
    int dialect;
    int attributed;
} OcerzObjcVariadic;

typedef struct OcerzObjcClass {
    uint64_t isa;
    uint64_t superclass;
    uint64_t cache;
    uint64_t vtable;
    uint64_t ro;
    uint32_t swift;
} OcerzObjcClass;

typedef struct OcerzObjcRo {
    uint32_t flags;
    uint32_t instance_start;
    uint32_t instance_size;
    uint32_t reserved;
    uint64_t ivar_layout;
    uint64_t name;
    uint64_t base_methods;
    uint64_t base_protocols;
    uint64_t ivars;
    uint64_t weak_ivar_layout;
    uint64_t base_properties;
} OcerzObjcRo;

typedef struct OcerzObjcList {
    uint64_t addr;
    uint32_t entsize;
    uint32_t count;
    uint32_t flags;
} OcerzObjcList;

typedef struct OcerzObjcMethod {
    uint64_t name;
    uint64_t types;
    uint64_t imp;
} OcerzObjcMethod;

typedef struct OcerzObjcIvar {
    uint64_t offset;
    uint64_t name;
    uint64_t type;
    uint32_t alignment;
    uint32_t size;
} OcerzObjcIvar;

typedef struct OcerzObjcProperty {
    uint64_t name;
    uint64_t attributes;
} OcerzObjcProperty;

typedef struct OcerzObjcCategory {
    uint64_t name;
    uint64_t cls;
    uint64_t instance_methods;
    uint64_t class_methods;
    uint64_t protocols;
    uint64_t instance_properties;
    uint64_t class_properties;
} OcerzObjcCategory;

typedef struct OcerzObjcProtocol {
    uint64_t name;
    uint64_t protocols;
    uint64_t instance_methods;
    uint64_t class_methods;
    uint64_t optional_instance_methods;
    uint64_t optional_class_methods;
    uint64_t instance_properties;
    uint32_t size;
    uint32_t flags;
    uint64_t extended_types;
    uint64_t demangled_name;
    uint64_t class_properties;
} OcerzObjcProtocol;

typedef struct OcerzObjcAttribute {
    const char *name;
    const char *value;
} OcerzObjcAttribute;

int ocerz_objcbridge_fix_selrefs(const uint8_t *mh, int64_t slide);
void ocerz_objcbridge_install_uncaught(void);
int ocerz_objcbridge_define_image(const uint8_t *mh, int64_t slide);
int ocerz_objcbridge_run_loads(struct OcerzVM *vm, uint64_t stack_top);
int ocerz_objcbridge_is_defined(uint64_t cls);
void *ocerz_objcbridge_dead_imp(void);

int ocerz_objc_read_class(uint64_t addr, OcerzObjcClass *out);
int ocerz_objc_read_ro(uint64_t addr, OcerzObjcRo *out);
int ocerz_objc_method_list(uint64_t addr, OcerzObjcList *out);
int ocerz_objc_method_at(const OcerzObjcList *list, uint32_t index, OcerzObjcMethod *out);
int ocerz_objc_ivar_list(uint64_t addr, OcerzObjcList *out);
int ocerz_objc_ivar_at(const OcerzObjcList *list, uint32_t index, OcerzObjcIvar *out);
int ocerz_objc_property_list(uint64_t addr, OcerzObjcList *out);
int ocerz_objc_property_at(const OcerzObjcList *list, uint32_t index, OcerzObjcProperty *out);
uint64_t ocerz_objc_protocol_count(uint64_t list);
uint64_t ocerz_objc_protocol_ref(uint64_t list, uint64_t index);
int ocerz_objc_read_category(uint64_t addr, int class_properties, OcerzObjcCategory *out);
int ocerz_objc_read_protocol(uint64_t addr, OcerzObjcProtocol *out);
int ocerz_objc_method_notation(const char *types, char *out, size_t outlen);
int ocerz_objc_property_attributes(const char *attrs, OcerzObjcAttribute *out, int max, char *storage,
                                   size_t storagelen);
int ocerz_objc_class_order(const uint64_t *classes, const uint64_t *supers, int n, int *order, int *culprit);

int ocerz_objc_notation(const char *encoding, char *out, size_t outlen, int *nargs,
                        uint32_t *blocks, uint32_t *fnptrs);
const char *ocerz_objc_refusal(int code);
int ocerz_objc_format_classes(const char *fmt, int dialect, char *out, size_t outlen,
                              const char **why);
const OcerzObjcVariadic *ocerz_objc_variadic(const char *sel);

int ocerz_objc_msgSend(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_objc_msgSendSuper(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_objc_msgSendSuper2(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_objc_msgSend_stret(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_objc_msgSendSuper_stret(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_objc_msgSendSuper2_stret(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_objc_msgSend_fpret(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_objc_msgSend_fp2ret(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_objc_setUncaughtExceptionHandler(struct OcerzVM *vm, OcerzCPU *cpu);

int ocerz_fmt_NSLog(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_fmt_printf(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_fmt_fprintf(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_fmt_sprintf(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_fmt_snprintf(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_fmt_asprintf(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_fmt_dprintf(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_fmt_sprintf_chk(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_fmt_snprintf_chk(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_fmt_CFStringCreateWithFormat(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_fmt_CFStringAppendFormat(struct OcerzVM *vm, OcerzCPU *cpu);

#endif

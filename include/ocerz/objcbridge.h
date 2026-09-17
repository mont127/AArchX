/*
 * Objective-C messages and formatted output from x86 guests, crossed into the
 * host's native arm64 runtime.
 *
 * In native mode an Intel program's imports from libobjc and Foundation bind to
 * synthesized images like every other system import, and the exports that no
 * fixed signature can describe are specials answered here: the objc_msgSend
 * family, objc_setUncaughtExceptionHandler, NSLog, the printf family and
 * CoreFoundation's two format functions.  src/objcbridge.c says how each one
 * works; this header is the contract with the rest of ocerz.
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

int ocerz_objcbridge_fix_selrefs(const uint8_t *mh, int64_t slide);
void ocerz_objcbridge_install_uncaught(void);

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

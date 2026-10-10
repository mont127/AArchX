/*
 * m32's Objective-C 1 runtime (src/m32_objc*.c, src/m32_blocks.c).
 */
#ifndef OCERZ_M32_OBJC_H
#define OCERZ_M32_OBJC_H

#include <stddef.h>
#include <stdint.h>

#include "ocerz/m32.h"
#include "ocerz/m32_image.h"

/* old_class (i386, 4-byte fields) */
enum { OC_ISA = 0, OC_SUPER = 4, OC_NAME = 8, OC_VERSION = 12, OC_INFO = 16, OC_ISIZE = 20, OC_IVARS = 24,
       OC_METHODS = 28, OC_CACHE = 32, OC_PROTOCOLS = 36, OC_SIZE = 48 };
#define OC_CLS_CLASS 0x1
#define OC_CLS_META 0x2
#define OC_CLS_HOST 0x40000000u   /* ours: a proxy standing for a host class */

void m32_objc_load(M32Image *img);
void m32_objc_run_loads(M32Image *img);   /* +load, from the image's initializers */
uint32_t m32_sel_guest(void *host_sel);
void *m32_sel_host(uint32_t guest_sel);
uint32_t m32_class_guest(void *host_cls);
void *m32_class_host(uint32_t guest_cls);
uint32_t m32_objc_to_guest(void *obj);
void *m32_objc_to_host(uint32_t obj);
extern const M32SpecialEntry m32_objc_specials[];

/* type encodings (src/m32_objc_types.c) */
int m32_encoding_notation(const char *enc, int guest, char *out, size_t n);
const char *m32_objc_guest_encoding(void *host_cls, const char *sel, int is_class);
int m32_objc_last_variadic(void);   /* whether the last m32_objc_guest_encoding hit is a variadic method */

/* blocks (src/m32_blocks.c) */
void *m32_block_to_host(uint32_t guest_block, const char *guest_sig, const char *host_sig);
uint32_t m32_block_to_guest(void *host_block);
int m32_is_guest_block(uint32_t g);
extern const M32SpecialEntry m32_block_specials[];

/* exceptions (src/m32_objc_exc.c) */
int m32_objc_guest_throw(OcerzCPU *cpu, uint32_t exception);   /* longjmp to the innermost guest @try */
extern const M32SpecialEntry m32_exc_specials[];

#endif

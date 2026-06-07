/*
 * include/ocerz/dyldapi.h
 *
 * The dyld runtime API shim. On macOS the system libraries do not talk to
 * the kernel for image/TLV/platform services — they call libdyld.dylib, whose
 * exported functions (_dyld_get_active_platform, _dyld_image_count, dlopen,
 * the TLV allocators, ...) are thin trampolines that load a single process-
 * wide "dyld APIs" object pointer from a libdyld global and tail-call a
 * virtual method on it (`mov rdi,[apis]; mov rax,[rdi]; jmp [rax+slot]`). The
 * real dyld installs that object during launch; a mini-dyld that does not run
 * Apple's dyld must BE that object.
 *
 * Rather than reimplement the dyld4 C++ runtime, Ocerz installs a real APIs
 * object in guest memory whose vtable entries point at "native thunks" — magic
 * guest addresses in [OCERZ_DYLDAPI_LO, OCERZ_DYLDAPI_HI). When the guest
 * tail-calls one, the interpreter recognises the address before fetching,
 * hands control to ocerz_dyldapi_dispatch, which implements that dyld API in C
 * (reading the System V arguments from the CPU and the object as `this`=rdi),
 * stores the result in rax, and returns to the guest by popping the caller's
 * return address. This is the same "implement the library natively" strategy
 * Box64/FEX use for system libraries: the guest runs the genuine libdyld
 * trampolines; Ocerz supplies the runtime behind them.
 *
 * ocerz_dyldapi_setup() builds the object + vtable and writes the object
 * pointer into the libdyld global(s) that hold it; it is called once during
 * dynamic launch after the cache is mapped. ocerz_dyldapi_dispatch() returns
 * an OcerzStep code (OK to continue, FATAL on an unimplemented slot whose
 * vtable byte-offset is reported so it can be added).
 */
#ifndef OCERZ_DYLDAPI_H
#define OCERZ_DYLDAPI_H

#include "ocerz/cpu.h"

#define OCERZ_DYLDAPI_LO 0x00000000dda00000ull
#define OCERZ_DYLDAPI_HI 0x00000000dda10000ull

struct OcerzVM;
struct OcerzCache;

int ocerz_dyldapi_setup(struct OcerzCache *cache);
int ocerz_dyldapi_dispatch(struct OcerzVM *vm, OcerzCPU *cpu);
void ocerz_dyldapi_run_image_loads(struct OcerzVM *vm, uint64_t mh, uint64_t stack_top);
void ocerz_dyldapi_register_image(uint64_t mh, const char *path);

#endif

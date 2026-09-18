/*
 * Mini-dyld: runs a dynamically-linked x86_64 Mach-O without Apple's dyld.
 *
 * Thread-local variables in native mode.  A guest's __thread variable is reached
 * through a descriptor whose first word is a thunk the compiler calls with the
 * descriptor's address in RDI and which must return the variable's address in
 * RAX while preserving every other register.  In cache mode dyld's own x86
 * tlv_get_addr out of the shared cache is that thunk.  Native mode has no such
 * code, so the thunk binds to the synthesized libSystem's __tlv_bootstrap export,
 * whose trap preserves every register by construction, and
 * ocerz_tlv_address answers it: the descriptor is rewritten at load time into
 * the packed form, an ocerz key per image plus the variable's offset and the
 * image's template, and each thread keeps a table of its per-image blocks in the
 * guest thread block behind gs, allocated from the template on first touch.
 * ocerz_tlv_release_thread frees one thread's blocks when that thread goes away.
 *
 * Loading code at run time in native mode.  src/bridge.c answers dlopen, dlsym,
 * dladdr, dlclose, dlerror, dlopen_preflight and the image-list half of the dyld
 * API as special exports, and each one lands here.  A handle is the mach header
 * of the image the loader holds, with its low bit set when RTLD_FIRST asked for
 * a search of that image alone; dlopen(NULL) answers RTLD_DEFAULT, or
 * RTLD_MAIN_ONLY under RTLD_FIRST, which is what dyld itself answers.  The image
 * list is the main executable at index 0 and then every image the loader holds,
 * synthesized system libraries and the guest's own dylibs alike, in the order
 * they were loaded; an image joins it once its fixups have bound and before any
 * of its code runs, and never leaves it, because nothing is unloaded.
 * ocerz_dyld_native_dlopen takes the caller's return address for RTLD_SELF,
 * RTLD_NEXT, @loader_path and @rpath, and the guest stack top below which
 * callbacks, +load methods and initializers of what it loads run on the calling
 * thread.  ocerz_dyld_native_dlerror answers a guest address, or 0, from a
 * buffer each thread keeps for itself.  ocerz_dyld_trie_each visits every
 * terminal of an image's export trie with its name, value and flags, answering
 * how many it saw or -1 for a trie it cannot read; dladdr uses it to name an
 * address in an image that has no symbol table, as a synthesized one has not.
 */
#ifndef OCERZ_DYLD_H
#define OCERZ_DYLD_H

#include "ocerz/types.h"
#include "ocerz/cpu.h"

struct OcerzVM;

extern uint64_t ocerz_main_mh;

int ocerz_peek_dynamic(const char *path);
int ocerz_dyld_run(struct OcerzVM *vm, const char *path, int argc, char **argv, char **envp);

uint64_t ocerz_dlopen(struct OcerzVM *vm, const char *hostpath, int mode);
uint64_t ocerz_dlsym(uint64_t handle, const char *sym);
int ocerz_dlclose(uint64_t handle);
uint64_t ocerz_dlerror(void);
int ocerz_canon_dylib_path(const char *path, char *out, size_t outsz);

void ocerz_dyld_dump_images(void);
const char *ocerz_dyld_name_for_addr(uint64_t addr, uint64_t *base_out);
const char *ocerz_dyld_main_path(void);
uint64_t ocerz_dyld_resolve_guest_sym(const char *name);
uint64_t ocerz_dyld_trie_resolve(const uint8_t *slice, uint64_t load_base,
                                 const char *sym, int *found);
extern uint64_t ocerz_exc_trap_rip;

#define OCERZ_TLV_TABLE_SLOT 0x1808

uint64_t ocerz_tlv_address(OcerzCPU *cpu, uint64_t desc);
void ocerz_tlv_release_thread(uint64_t gs_base);

typedef int (*OcerzTrieVisit)(void *ctx, const char *name, uint64_t value, uint64_t flags);
int ocerz_dyld_trie_each(const uint8_t *slice, uint64_t load_base, OcerzTrieVisit visit, void *ctx);

uint64_t ocerz_dyld_native_dlopen(struct OcerzVM *vm, const char *path, int mode, uint64_t caller,
                                  uint64_t stack_top);
int ocerz_dyld_native_dlopen_preflight(const char *path, uint64_t caller);
uint64_t ocerz_dyld_native_dlsym(uint64_t handle, const char *name, uint64_t caller);
int ocerz_dyld_native_dladdr(uint64_t addr, uint64_t info);
int ocerz_dyld_native_dlclose(uint64_t handle);
uint64_t ocerz_dyld_native_dlerror(void);

uint32_t ocerz_dyld_image_count(void);
int ocerz_dyld_image_at(uint32_t index, uint64_t *mh, uint64_t *slide, uint64_t *name);
int ocerz_dyld_image_containing(uint64_t addr, uint64_t *mh, uint64_t *name);
int ocerz_dyld_image_slide(uint64_t mh, uint64_t *slide);
int ocerz_dyld_native_add_image_func(struct OcerzVM *vm, uint64_t func, uint64_t stack_top);
int ocerz_dyld_native_remove_image_func(uint64_t func);
int ocerz_dyld_is_memory_immutable(uint64_t addr, uint64_t len);
int ocerz_dyld_native_names_library(const char *path);
int ocerz_dyld_build_version(uint64_t mh, uint32_t *platform, uint32_t *minos, uint32_t *sdk);
int ocerz_dyld_version_at_least(uint64_t mh, uint64_t version, int sdk);

#endif

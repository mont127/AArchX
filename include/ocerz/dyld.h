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

#endif

/*
 * include/ocerz/dyld.h
 *
 * Ocerz's mini-dyld: the dynamic-loading path that runs a normal,
 * dynamically-linked x86_64 Mach-O executable (LC_MAIN, linked against
 * libSystem) without running Apple's real dyld. Instead Ocerz itself does
 * dyld's job — map the shared cache (cache.h), load and slide the PIE
 * executable, apply its chained fixups (rebasing internal pointers and
 * binding its imports to addresses resolved out of the cache), build a
 * System V entry frame, and jump to the program's main.
 *
 * This complements the static path (loader.h / ocerz_setup_stack), which
 * handles self-contained LC_UNIXTHREAD binaries. The two are selected in
 * main.c by peeking the executable's entry load command.
 *
 * Memory model: dynamic mode runs with guest_base = 0 (identity) so the
 * cache's absolute pointers (0x7FF8..) stay valid; the executable, stack and
 * heap live in a reserved low identity arena and a PIE is slid there. A
 * non-PIE main (the Wine x86_64-unix loader: zero rebase records, absolute
 * pointers, zero-fill address-space reservations) is instead mapped FIXED at
 * slide 0, per segment: content segments commit at their exact vmaddr (via
 * the low shadow window for addresses under OCERZ_LOW_LIMIT), zero-fill
 * segments stay reserved-uncommitted, and out-of-arena identity ranges are
 * registered with ocerz_mem_register_range. ocerz_main_mh is the loaded main
 * executable's mach_header guest address (the arena base for a slid PIE, the
 * unslid text vmaddr for a fixed image); dyldapi reports it wherever dyld
 * would name the main image.
 *
 * ocerz_dyld_run loads `path`, links it, and runs it to completion through
 * ocerz_vm_run, returning the guest's exit status (or a negative OCERZ_E* on
 * a load/link failure before execution begins).
 */
#ifndef OCERZ_DYLD_H
#define OCERZ_DYLD_H

#include "ocerz/types.h"

struct OcerzVM;

extern uint64_t ocerz_main_mh;

int ocerz_peek_dynamic(const char *path);
int ocerz_dyld_run(struct OcerzVM *vm, const char *path, int argc, char **argv, char **envp);

uint64_t ocerz_dlopen(struct OcerzVM *vm, const char *hostpath, int mode);
uint64_t ocerz_dlsym(uint64_t handle, const char *sym);
int ocerz_dlclose(uint64_t handle);
uint64_t ocerz_dlerror(void);

void ocerz_dyld_dump_images(void);
const char *ocerz_dyld_name_for_addr(uint64_t addr, uint64_t *base_out);

#endif

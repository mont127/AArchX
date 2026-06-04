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
 * heap live in a reserved low identity arena and the PIE is slid there.
 *
 * ocerz_dyld_run loads `path`, links it, and runs it to completion through
 * ocerz_vm_run, returning the guest's exit status (or a negative OCERZ_E* on
 * a load/link failure before execution begins).
 */
#ifndef OCERZ_DYLD_H
#define OCERZ_DYLD_H

#include "ocerz/types.h"

struct OcerzVM;

int ocerz_peek_dynamic(const char *path);
int ocerz_dyld_run(struct OcerzVM *vm, const char *path, int argc, char **argv, char **envp);

#endif

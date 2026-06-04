/*
 * src/globals.c
 *
 * Process-wide definitions that every Ocerz translation unit may reference
 * through headers but that no single feature module should own.
 *
 * ocerz_verbose is the one such global today: the OCERZ_LOG/OCERZ_TRACE
 * macros in types.h read it, so it is referenced from mem.c, loader.c,
 * stack.c, vm.c and others. Defining it here — in a file that is part of the
 * core object set linked into both the emulator binary and every unit test —
 * means there is exactly one definition no matter which subset of objects a
 * given executable links. main.c only assigns it while parsing -v.
 */
#include "ocerz/types.h"

int ocerz_verbose;

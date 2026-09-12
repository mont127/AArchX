/*
 * Process-wide definitions that no single module should own.
 *
 * The guest argv tail kept here for the exit diagnostics is defined in this
 * file rather than in main.c so that the unit tests, which link CORE_OBJS
 * without main.o, still resolve it.
 */
#include "ocerz/types.h"

int ocerz_verbose;

char ocerz_cmdline_summary[256];

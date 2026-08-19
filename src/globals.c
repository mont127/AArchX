/* Process-wide definitions that no single module should own. */
#include "ocerz/types.h"

int ocerz_verbose;

/* guest argv tail for diagnostics (EXITLOG); defined here, not in main.c,
 * so the unit tests (which link CORE_OBJS without main.o) resolve it. */
char ocerz_cmdline_summary[256];

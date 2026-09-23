/*
 * A universal library for dinsert_libraries: its x86_64 slice announces itself
 * from a constructor, its arm64 slice does nothing.  See insert_main.c.
 */
#include <stdio.h>

__attribute__((constructor)) static void announce(void)
{
#if defined(__x86_64__)
    printf("inserted\n");
    fflush(stdout);
#endif
}

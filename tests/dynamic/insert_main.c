/*
 * DYLD_INSERT_LIBRARIES loads its libraries into the guest.
 *
 * Steam injects its loader and overlay into every game through
 * DYLD_INSERT_LIBRARIES.  ocerz ignored the variable, and the host's dyld read
 * it instead and loaded the arm64 slices into ocerz itself.  The harness runs
 * this program with the variable naming a universal library whose x86_64
 * constructor prints "inserted", so the line appears only if ocerz inserted the
 * x86_64 slice into the guest, before main.  main then reports whether the
 * variable is still in its environment as it was set, and whether ocerz's own
 * carrier variable leaked into it.
 */
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    printf("main env=%s carrier=%s\n", getenv("DYLD_INSERT_LIBRARIES") ? "kept" : "lost",
           getenv("OCERZ_GUEST_DYLD_INSERT_LIBRARIES") ? "leaked" : "gone");
    return 0;
}

/*
 * tests/guest/hello.c
 *
 * The smallest end-to-end guest test: prove that a static x86_64 Mach-O can
 * be loaded, started at _start, reach main, perform a write(2) syscall to fd
 * 1, and exit cleanly with status 0 under Ocerz. Prints one fixed line so the
 * golden output is trivially deterministic and machine-independent.
 */
#include "gsys.h"

int main(int argc, char **argv, char **envp)
{
    g_puts("hello from x86_64 guest\n");
    return 0;
}

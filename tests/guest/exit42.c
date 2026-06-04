/*
 * tests/guest/exit42.c
 *
 * Exit-status plumbing test: prints nothing and returns 42, so the runner can
 * confirm that crt0 forwards main's return value into the exit syscall and
 * that Ocerz propagates the guest exit code as its own process exit status.
 * The golden stdout is empty; the expected exit code is 42.
 */
#include "gsys.h"

int main(int argc, char **argv, char **envp)
{
    return 42;
}

/*
 * tests/guest/args.c
 *
 * Validates the exec-time stack the loader builds: argc at the top, the argv
 * pointer vector, and the envp pointer vector laid out exactly as XNU does
 * for an LC_UNIXTHREAD binary. Prints argc, then every argv string on its own
 * line, then a single deterministic "env ok\n" line. The runner always passes
 * the fixed arguments "one two three", so argc and the argv lines are stable.
 *
 * The environment is host-dependent (it is passed through verbatim from
 * Ocerz's own environment), so this test must NOT print env values or even
 * the env count. Instead it counts how many envp entries contain a '=' (every
 * well-formed environment variable does) and asserts that the count is
 * positive and equals the total number of envp entries; only the fixed string
 * "env ok\n" is printed, never any host-specific data, keeping the golden
 * machine-independent. If the environment were malformed it would print
 * "env bad\n" instead, which the golden does not expect.
 */
#include "gsys.h"

int main(int argc, char **argv, char **envp)
{
    g_putu64((g_u64)argc);
    for (int i = 0; i < argc; i++) {
        g_puts(argv[i]);
        g_puts("\n");
    }

    int total = 0;
    int with_eq = 0;
    for (char **e = envp; *e != 0; e++) {
        total++;
        for (const char *p = *e; *p != 0; p++) {
            if (*p == '=') {
                with_eq++;
                break;
            }
        }
    }

    if (total > 0 && with_eq == total)
        g_puts("env ok\n");
    else
        g_puts("env bad\n");

    return 0;
}

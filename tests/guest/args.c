/* Checks the exec-time stack: argc, argv, envp and apple[]. */
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

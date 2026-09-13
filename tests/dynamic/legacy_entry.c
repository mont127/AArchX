#include <crt_externs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv, char **envp, char **apple)
{
    if (argc != 3 || !argv[0] || strcmp(argv[1], "one") != 0 || strcmp(argv[2], "two") != 0 ||
        argv[3] != NULL) {
        printf("BAD argv\n");
        return 1;
    }
    if (*_NSGetArgc() != argc || strcmp((*_NSGetArgv())[1], "one") != 0) {
        printf("BAD progvars\n");
        return 1;
    }
    const char *probe = getenv("LEGACY_ENTRY_PROBE");
    int seen = 0;
    for (char **e = envp; e && *e; e++)
        if (strcmp(*e, "LEGACY_ENTRY_PROBE=yes") == 0)
            seen = 1;
    if (!probe || strcmp(probe, "yes") != 0 || !seen) {
        printf("BAD envp\n");
        return 1;
    }
    if (!apple || !apple[0] || strncmp(apple[0], "executable_path=", 16) != 0) {
        printf("BAD apple\n");
        return 1;
    }
    printf("OK\n");
    return 0;
}

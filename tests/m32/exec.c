/* execv and execve from an i386 guest: the vectors hold 4-byte pointers, and the new i386 program runs under ocerz too.
 * The program execs itself twice, the second time with an environment of its own. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc == 1) {
        printf("parent\n");
        fflush(stdout);
        char *args[] = { argv[0], "execv", "two words", NULL };
        execv(argv[0], args);
        printf("execv failed\n");
        return 1;
    }
    if (!strcmp(argv[1], "execv")) {
        printf("execv argc %d [%s]\n", argc, argv[2]);
        fflush(stdout);
        char *args[] = { argv[0], "execve", NULL }, *env[] = { "M32_EXEC=set", NULL };
        execve(argv[0], args, env);
        printf("execve failed\n");
        return 1;
    }
    const char *e = getenv("M32_EXEC");
    printf("execve argc %d M32_EXEC=%s\n", argc, e ? e : "(none)");
    return 0;
}

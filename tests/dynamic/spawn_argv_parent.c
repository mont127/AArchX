#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>

extern char **environ;

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("BAD usage\n");
        return 1;
    }
    char *child_argv[] = { argv[1], "--type=renderer", NULL };
    pid_t pid;
    if (posix_spawn(&pid, argv[1], NULL, NULL, child_argv, environ) != 0) {
        printf("BAD spawn\n");
        return 1;
    }
    int status;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return 1;
    return WEXITSTATUS(status);
}

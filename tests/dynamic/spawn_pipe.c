/* posix_spawn with file actions and attributes, the way NSTask launches a
 * helper: the child's stdout is dup2'd onto a pipe, the read end closed, and
 * POSIX_SPAWN_CLOEXEC_DEFAULT drops everything else. If the actions are
 * lost, echo writes straight to our stdout and the pipe reads back empty. */
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int main(void)
{
    int p[2];
    if (pipe(p) != 0)
        return 1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_addclose(&fa, p[0]);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT);

    char *argv[] = { "echo", "piped", NULL };
    pid_t pid;
    if (posix_spawn(&pid, "/bin/echo", &fa, &at, argv, environ) != 0)
        return 2;
    close(p[1]);

    char buf[64];
    ssize_t n, got = 0;
    while (got < (ssize_t)sizeof buf - 1 && (n = read(p[0], buf + got, sizeof buf - 1 - got)) > 0)
        got += n;
    buf[got] = '\0';
    int st = 0;
    waitpid(pid, &st, 0);
    if (strcmp(buf, "piped\n") != 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("BAD got='%s' status=%#x\n", buf, st);
        return 3;
    }
    printf("OK\n");
    return 0;
}

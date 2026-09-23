/*
 * A child with no Intel code in it runs natively.
 *
 * A spawned or exec'd binary that carries an arm64 slice and no x86_64 slice
 * has nothing in it for ocerz to translate, and Rosetta simply runs it as
 * the native program it is.  ocerz used to run it under itself in cache mode
 * anyway, and the child died with "cannot read" before it did anything.
 * Steam spawns /usr/bin/open three times a launch, and on macOS 27 open
 * ships as arm64 only, so every one of those spawns failed.
 *
 * /usr/bin/open with no arguments prints its usage and exits 1, so a status
 * of 1 is the native program having run; 65 is what a child ocerz exited
 * with when it refused the binary, and 127 is exec failing outright.  Both
 * posix_spawn and fork+exec are exercised, since they reach the decision by
 * different paths.  The child's stdout is sent to /dev/null so that a usage
 * message can never end up in this test's own output.  On a macOS whose open
 * still carries Intel code the test passes without exercising the rule,
 * which is the right thing for it to do there.
 */
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int check(const char *how, pid_t pid, int st)
{
    if (pid < 0 || !WIFEXITED(st)) {
        printf("spawn_arm64_only bad: %s: child status %d\n", how, st);
        return 1;
    }
    if (WEXITSTATUS(st) != 1) {
        printf("spawn_arm64_only bad: %s: exit %d, want 1 (open's usage exit; 65 is ocerz refusing the binary)\n",
               how, WEXITSTATUS(st));
        return 1;
    }
    return 0;
}

int main(void)
{
    const char *path = "/usr/bin/open";
    char *const argv[] = { "open", NULL };
    int bad = 0, st = 0;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, path, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        printf("spawn_arm64_only bad: posix_spawn: %s\n", strerror(rc));
        return 1;
    }
    if (waitpid(pid, &st, 0) != pid)
        pid = -1;
    bad |= check("posix_spawn", pid, st);

    pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); close(fd); }
        execv(path, argv);
        _exit(127);
    }
    st = 0;
    if (pid > 0 && waitpid(pid, &st, 0) != pid)
        pid = -1;
    bad |= check("fork+exec", pid, st);

    if (!bad)
        printf("OK\n");
    return bad;
}

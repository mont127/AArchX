#include <errno.h>
#include <unistd.h>

enum { SANDBOX_FILTER_PATH = 1 };
extern int sandbox_check(pid_t pid, const char *operation, int type, ...);

int low_stack_sandbox_check(int *err)
{
    errno = 0;
    int r = sandbox_check(getpid(), "file-read-data", SANDBOX_FILTER_PATH, "/usr/lib");
    *err = errno;
    return r;
}

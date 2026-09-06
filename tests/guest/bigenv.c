/* A large environment and argument list survive exec.  The dynamic-mode
 * stack builder once copied at most 60 environment entries and had room for
 * 64 arguments; wine's loader appends WINELOADERNOEXEC=1 as the last
 * variable to mark its one-time re-exec, so one variable too many made every
 * wine process re-exec itself forever.  The parent execs itself with 200
 * variables and 66 arguments; the child reports what it received.  Goldens
 * come from the native binary. */
#include "gsys.h"

#define NENV 200
#define NARG 66

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int main(int argc, char **argv, char **envp)
{
    if (argc > 1) {
        int n = 0;
        while (envp[n]) n++;
        g_puts("envc "); g_putu64(n);
        g_puts("last ok "); g_putu64(n > 0 && streq(envp[n - 1], "ZLAST=1"));
        g_puts("first ok "); g_putu64(n > 0 && streq(envp[0], "MallocNanoZone=0"));
        g_puts("argc "); g_putu64(argc);
        g_puts("last arg ok "); g_putu64(streq(argv[argc - 1], "a65"));
        return 0;
    }
    static char ebuf[NENV][12];
    static char *env[NENV + 1];
    static char abuf[NARG][8];
    static char *args[NARG + 2];
    /* the emulator appends MallocNanoZone=0 at exec when it is absent; carry
     * it so the child sees exactly what the native run sees */
    env[0] = "MallocNanoZone=0";
    for (int i = 1; i < NENV - 1; i++) {
        char *p = ebuf[i];
        *p++ = 'V'; *p++ = (char)('0' + i / 100); *p++ = (char)('0' + (i / 10) % 10); *p++ = (char)('0' + i % 10);
        *p++ = '='; *p++ = (char)('0' + i % 10); *p = 0;
        env[i] = ebuf[i];
    }
    env[NENV - 1] = "ZLAST=1";
    env[NENV] = 0;
    args[0] = argv[0];
    for (int i = 1; i <= NARG - 1; i++) {
        char *p = abuf[i];
        *p++ = 'a'; if (i >= 10) *p++ = (char)('0' + i / 10); *p++ = (char)('0' + i % 10); *p = 0;
        args[i] = abuf[i];
    }
    args[NARG] = 0;
    g_syscall3(SYS(59), (g_i64)argv[0], (g_i64)args, (g_i64)env);
    g_puts("exec failed\n");
    return 1;
}

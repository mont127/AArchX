#include <libproc.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc_info.h>
#include <unistd.h>

int main(void)
{
    char exe[PATH_MAX], real[PATH_MAX], path[PROC_PIDPATHINFO_MAXSIZE] = "", name[64] = "";
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) != 0 || !realpath(exe, real)) {
        printf("BAD executable path\n");
        return 1;
    }
    if (proc_pidpath(getpid(), path, sizeof path) <= 0 || strcmp(path, real) != 0) {
        printf("BAD proc_pidpath \"%s\", want \"%s\"\n", path, real);
        return 2;
    }
    const char *base = strrchr(real, '/');
    base = base ? base + 1 : real;
    if (proc_name(getpid(), name, sizeof name) <= 0 || strncmp(name, base, 2 * MAXCOMLEN) != 0) {
        printf("BAD proc_name \"%s\", want \"%s\"\n", name, base);
        return 3;
    }
    struct proc_bsdshortinfo si;
    if (proc_pidinfo(getpid(), PROC_PIDT_SHORTBSDINFO, 0, &si, sizeof si) != sizeof si ||
        strncmp(si.pbsi_comm, base, MAXCOMLEN) != 0) {
        printf("BAD short bsdinfo comm \"%.16s\"\n", si.pbsi_comm);
        return 4;
    }
    printf("OK\n");
    return 0;
}

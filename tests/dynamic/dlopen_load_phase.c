#include <limits.h>
#include <mach-o/dyld.h>
#include <stdint.h>

#include "placeholder_check.h"

int main(void)
{
    char path[PATH_MAX];
    uint32_t sz = sizeof path;
    if (_NSGetExecutablePath(path, &sz) != 0) {
        printf("BAD nsget\n");
        return 1;
    }
    char *slash = strrchr(path, '/');
    if (!slash) {
        printf("BAD exe path\n");
        return 1;
    }
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "libload_phase.dylib");
    if (!dlopen(path, RTLD_NOW)) {
        printf("BAD dlopen: %s\n", dlerror());
        return 1;
    }
    if (!placeholders_ok("after dlopen"))
        return 1;
    printf("OK\n");
    return 0;
}

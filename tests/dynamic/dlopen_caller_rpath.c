#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "sub/libcaller_rpath.dylib");
    void *h = dlopen(path, RTLD_NOW);
    if (!h) {
        printf("BAD dlopen loader: %s\n", dlerror());
        return 1;
    }
    int (*load)(void) = (int (*)(void))dlsym(h, "caller_rpath_load");
    int v = load ? load() : -3;
    if (v != 42) {
        printf("BAD %d\n", v);
        return 1;
    }
    printf("OK\n");
    return 0;
}

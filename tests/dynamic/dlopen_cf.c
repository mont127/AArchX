#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

int main(void)
{
    if (!zlibVersion()) {
        printf("BAD zlib\n");
        return 1;
    }
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
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "libcf_user.dylib");
    void *h = dlopen(path, RTLD_NOW);
    if (!h) {
        printf("BAD dlopen: %s\n", dlerror());
        return 1;
    }
    int (*len)(void) = (int (*)(void))dlsym(h, "cf_lib_len");
    if (!len) {
        printf("BAD dlsym\n");
        return 1;
    }
    if (len() != 5) {
        printf("BAD init %d\n", len());
        return 1;
    }
    printf("OK\n");
    return 0;
}

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <mach-o/dyld.h>

int main(void)
{
    char path[1024];
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) != 0)
        return 2;
    char *slash = strrchr(path, '/');
    if (!slash)
        return 2;
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "libweak_unloaded.dylib");
    void *h = dlopen(path, RTLD_NOW);
    if (!h) {
        printf("dlopen failed: %s\n", dlerror());
        return 1;
    }
    long (*call)(long) = (long (*)(long))dlsym(h, "weak_unloaded_call");
    if (!call) {
        printf("no weak_unloaded_call\n");
        return 1;
    }
    long r = call(1);
    if (r != 1 + 0x5eed) {
        printf("weak twin answered %#lx\n", (unsigned long)r);
        return 1;
    }
    printf("OK\n");
    return 0;
}

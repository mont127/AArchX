#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void *open_rel(const char *dir, const char *name)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return dlopen(path, RTLD_NOW);
}

int main(void)
{
    char dir[PATH_MAX];
    uint32_t sz = sizeof dir;
    if (_NSGetExecutablePath(dir, &sz) != 0) {
        printf("BAD nsget\n");
        return 1;
    }
    char *slash = strrchr(dir, '/');
    if (!slash) {
        printf("BAD path\n");
        return 1;
    }
    *slash = '\0';
    if (!open_rel(dir, "decoy/libidname_decoy.dylib")) {
        printf("BAD decoy: %s\n", dlerror());
        return 1;
    }
    if (!open_rel(dir, "real/libidname_base.dylib")) {
        printf("BAD base: %s\n", dlerror());
        return 1;
    }
    void *user = open_rel(dir, "libidname_user.dylib");
    if (!user) {
        printf("BAD user: %s\n", dlerror());
        return 1;
    }
    int (*value)(void) = (int (*)(void))dlsym(user, "user_value");
    if (!value) {
        printf("BAD dlsym\n");
        return 1;
    }
    int v = value();
    if (v != 43) {
        printf("BAD value %d\n", v);
        return 1;
    }
    printf("OK\n");
    return 0;
}

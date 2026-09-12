#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int alias_marker;

static int check(const char *label, const char *path, int mode)
{
    void *h = dlopen(path, mode);
    if (!h) {
        printf("BAD %s dlopen: %s\n", label, dlerror());
        return 1;
    }
    void *sym = dlsym(h, "alias_marker");
    if (sym != (void *)&alias_marker) {
        printf("BAD %s dup linked=%p loaded=%p\n", label, (void *)&alias_marker, sym);
        return 1;
    }
    return 0;
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
        printf("BAD exe path\n");
        return 1;
    }
    *slash = '\0';
    char hard[PATH_MAX];
    snprintf(hard, sizeof hard, "%s/real/libalias_hard.dylib", dir);

    int bad = 0;
    bad |= check("symlink", "link/libalias_t.dylib", RTLD_NOW);
    bad |= check("hardlink", hard, RTLD_NOW);
    if (bad)
        return 1;
    printf("OK\n");
    return 0;
}

/*
 * Flat lookups the way dyld answers them, from the loaded images only: the bundle's flat imports bind to the dylib
 * this program links, xmlCheckVersion too, which libxml2 in the shared cache also exports but which nothing here
 * loads (ocerz searched the whole cache first, and the cache's copy won).  And dlsym(NULL, ...) is no lookup at all:
 * NULL is not a handle, and dlerror says so.
 */
#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int flat_answer(void);

int main(void)
{
    char path[PATH_MAX];
    uint32_t sz = sizeof path;
    if (_NSGetExecutablePath(path, &sz) != 0 || !strrchr(path, '/')) {
        printf("BAD exe path\n");
        return 1;
    }
    strcpy(strrchr(path, '/') + 1, "flat_lookup.bundle");
    void *h = dlopen(path, RTLD_NOW);
    if (!h) {
        printf("BAD dlopen: %s\n", dlerror());
        return 1;
    }
    int (*entry)(void) = (int (*)(void))dlsym(h, "flat_entry");
    if (!entry) {
        printf("BAD dlsym flat_entry: %s\n", dlerror());
        return 1;
    }
    int got = entry();
    if (got != 4207) {
        printf("BAD flat imports gave %d, want 4207 (flat_answer %d)\n", got, flat_answer());
        return 1;
    }
    dlerror();
    void *p = dlsym(NULL, "printf");
    const char *e = dlerror();
    if (p || !e || !strstr(e, "invalid handle")) {
        printf("BAD dlsym(NULL) gave %p, dlerror '%s'\n", p, e ? e : "(none)");
        return 1;
    }
    printf("OK\n");
    return 0;
}

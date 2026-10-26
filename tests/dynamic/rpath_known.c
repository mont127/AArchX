/*
 * An image dlopened by its path and later reached through an @rpath name it
 * is not known by is the image that name's imports bind to, as dyld binds
 * them.  Wine's ntdll.so is named "ntdll.so" and dlopened by its path, and
 * win32u.so links @rpath/ntdll.so: ocerz found no image for that ordinal, so
 * each of its 480 imports went looking through every image there is.
 * libknown_user links @rpath/libknown.dylib for getpagesize, which libSystem
 * exports too; a lookup that does not find libknown answers libSystem's.
 */
#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char dir[PATH_MAX], path[PATH_MAX];
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
    snprintf(path, sizeof path, "%s/libknown.dylib", dir);
    if (!dlopen(path, RTLD_NOW)) {
        printf("BAD dlopen lib: %s\n", dlerror());
        return 1;
    }
    snprintf(path, sizeof path, "%s/libknown_user.dylib", dir);
    void *h = dlopen(path, RTLD_NOW);
    if (!h) {
        printf("BAD dlopen user: %s\n", dlerror());
        return 1;
    }
    int (*value)(void) = (int (*)(void))dlsym(h, "known_value");
    int v = value ? value() : -3;
    if (v != 42) {
        printf("BAD %d\n", v);
        return 1;
    }
    printf("OK\n");
    return 0;
}

/*
 * A dlopened image's @rpath dependencies are found through the rpaths of the
 * image that called dlopen, as dyld's load chain does: libchain_loader (rpath
 * @loader_path) dlopens plug/libchain_plugin by its full path, and the plugin,
 * which has no rpath of its own, links @rpath/libchain_dep.dylib from the
 * loader's directory.  CrossOver's cxcompatdb.so finds its gnutls this way,
 * through ntdll.so.  A leaf name the loader dlopens is looked for in the same
 * rpaths, which is how CrossOver's win32u.so finds libfreetype.6.dylib.
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
    snprintf(path, sizeof path, "%s/sub/libchain_loader.dylib", dir);
    void *h = dlopen(path, RTLD_NOW);
    if (!h) {
        printf("BAD dlopen loader: %s\n", dlerror());
        return 1;
    }
    int (*load)(const char *) = (int (*)(const char *))dlsym(h, "chain_load");
    snprintf(path, sizeof path, "%s/plug/libchain_plugin.dylib", dir);
    int v = load ? load(path) : -3;
    if (v != 42) {
        printf("BAD %d\n", v);
        return 1;
    }
    int (*leaf)(void) = (int (*)(void))dlsym(h, "chain_leaf");
    v = leaf ? leaf() : -3;
    if (v != 7) {
        printf("BAD leaf %d\n", v);
        return 1;
    }
    printf("OK\n");
    return 0;
}

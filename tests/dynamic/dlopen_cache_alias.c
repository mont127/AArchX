/*
 * Library paths that the shared cache knows only as aliases open.
 *
 * The cache lists each library once under its install name, and every other
 * path that names it - /usr/lib/libz.dylib for libz.1.dylib, libc++.dylib for
 * libc++.1.dylib, libgcc_s.1.dylib for libSystem - lives only in the cache's
 * dylibs trie.  ocerz looked paths up in the image list alone, so all 108 of
 * the SDK's aliased /usr/lib names failed to load while Rosetta loaded them,
 * and a binary linked against libgcc_s.1.dylib, as Steam's steamloader is,
 * could not be loaded at all.  Each alias must open and answer a symbol that
 * lives in the library it names.
 */
#include <dlfcn.h>
#include <stdio.h>

static int one(const char *path, const char *symbol)
{
    void *h = dlopen(path, RTLD_NOW);
    if (!h) {
        printf("dlopen_cache_alias bad: dlopen(%s): %s\n", path, dlerror());
        return 1;
    }
    if (!dlsym(h, symbol)) {
        printf("dlopen_cache_alias bad: %s has no %s\n", path, symbol);
        return 1;
    }
    return 0;
}

int main(void)
{
    int bad = one("/usr/lib/libgcc_s.1.dylib", "malloc") |
              one("/usr/lib/libz.dylib", "zlibVersion") |
              one("/usr/lib/libc++.dylib", "_ZNSt3__16chrono12system_clock3nowEv");
    if (!bad)
        printf("OK\n");
    return 0;
}

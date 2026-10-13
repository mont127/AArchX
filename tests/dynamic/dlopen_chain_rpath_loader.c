#include <dlfcn.h>
#include <stdio.h>

int chain_load(const char *plugin)
{
    void *h = dlopen(plugin, RTLD_NOW);
    if (!h) {
        printf("BAD dlopen plugin: %s\n", dlerror());
        return -1;
    }
    int (*f)(void) = (int (*)(void))dlsym(h, "chain_plugin_value");
    return f ? f() : -2;
}

/* a leaf name: dyld looks in the caller's LC_RPATHs after DYLD_LIBRARY_PATH */
int chain_leaf(void)
{
    void *h = dlopen("libchain_leaf.dylib", RTLD_NOW);
    if (!h) {
        printf("BAD dlopen leaf: %s\n", dlerror());
        return -1;
    }
    int (*f)(void) = (int (*)(void))dlsym(h, "chain_leaf_value");
    return f ? f() : -2;
}

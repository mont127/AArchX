#include <dlfcn.h>
#include <stdio.h>

int caller_rpath_load(void)
{
    void *h = dlopen("@rpath/CallerRpath.framework/CallerRpath", RTLD_NOW);
    if (!h) {
        printf("BAD dlopen: %s\n", dlerror());
        return -1;
    }
    int (*f)(void) = (int (*)(void))dlsym(h, "caller_rpath_value");
    return f ? f() : -2;
}

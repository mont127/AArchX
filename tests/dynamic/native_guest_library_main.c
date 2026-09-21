#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DYNAMIC_ONLY
extern int COLOR_PAIR(int);
#endif

int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    int expected = atoi(argv[1]);
#ifndef DYNAMIC_ONLY
    if (COLOR_PAIR(7) != expected)
        return 3;
#endif
    void *handle = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "%s\n", dlerror());
        return 4;
    }
    int (*fn)(int) = (int (*)(int))dlsym(handle, "COLOR_PAIR");
    if (!fn || fn(7) != expected)
        return 5;
    Dl_info info;
    if (!dladdr((void *)fn, &info) || !info.dli_fname || !strstr(info.dli_fname, argv[3]))
        return 6;
    if (dlclose(handle) != 0)
        return 7;
    printf("native_guest_library ok %d\n", expected);
    return 0;
}

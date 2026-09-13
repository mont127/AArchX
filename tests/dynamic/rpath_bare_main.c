#include <dlfcn.h>
#include <stdio.h>

int exe_dep_value(void);

int main(int argc, char **argv)
{
    int fails = 0;
    if (argc < 2) {
        printf("usage: rpath_bare <directory holding sub/librpath_user.dylib>\n");
        return 2;
    }
    if (exe_dep_value() != 7) {
        printf("BAD a dependency found through a bare @executable_path rpath\n");
        fails++;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/sub/librpath_user.dylib", argv[1]);
    void *h = dlopen(path, RTLD_NOW);
    int (*fn)(void) = h ? (int (*)(void))dlsym(h, "rpath_user_value") : NULL;
    if (!fn || fn() != 42) {
        printf("BAD a dependency found through a bare @loader_path rpath (handle %p)\n", h);
        fails++;
    }
    if (!fails)
        printf("OK\n");
    return fails != 0;
}

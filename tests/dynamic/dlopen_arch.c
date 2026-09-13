#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    int fails = 0;
    if (argc < 3) {
        printf("usage: dlopen_arch <arm64-only dylib> <universal dylib>\n");
        return 2;
    }
    void *arm = dlopen(argv[1], RTLD_NOW);
    const char *err = arm ? NULL : dlerror();
    if (arm || !err || !*err) {
        printf("BAD an arm64-only dylib opened (handle %p)\n", arm);
        fails++;
    }
    void *fat = dlopen(argv[2], RTLD_NOW);
    int (*fn)(void) = fat ? (int (*)(void))dlsym(fat, "arch_value") : NULL;
    if (!fn || fn() != 64) {
        printf("BAD the universal dylib's x86_64 slice (handle %p, arch_value %p)\n", fat, (void *)fn);
        fails++;
    }
    if (!fails)
        printf("OK\n");
    return fails != 0;
}

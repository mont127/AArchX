/*
 * A dylib in the pre-10.6 format loads and works.
 *
 * Before dyld's compressed fixup information, a dylib's pointers were fixed
 * through local relocations, external relocations and the indirect symbol
 * table, and ocerz applied none of them: every pointer in such a library kept
 * its unslid or unbound value.  Steam's steamloader.dylib, which Steam injects
 * into every game it launches, is one, so every game started from Steam under
 * ocerz crashed at once.
 *
 * The library is opened with dlopen, so it is slid, and legacy_check reports
 * one bit per failure: 1 its own data pointer was not rebased, 2 its pointer
 * to strlen was not bound, 4 a call through its lazy pointers failed.
 */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    void *h = dlopen(argc > 1 ? argv[1] : "", RTLD_NOW);
    if (!h) {
        printf("legacy_format bad: %s\n", dlerror());
        return 1;
    }
    int (*check)(void) = (int (*)(void))dlsym(h, "legacy_check");
    if (!check) {
        printf("legacy_format bad: no legacy_check\n");
        return 1;
    }
    int bad = check();
    if (bad)
        printf("legacy_format bad: %d\n", bad);
    else
        printf("OK\n");
    return 0;
}

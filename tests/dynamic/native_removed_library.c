/*
 * A program, and a plug-in it opens, linking /usr/lib/libocerz_removed_probe.dylib, which is not on disk when they
 * run: the stand-in for a library Apple shipped for Intel Macs that no Apple silicon Mac has, QTKit or the 0.9.x
 * libcrypto.  Native mode stubs its imports, which answer 0 (tests/run_native_framework_tests.sh).
 *
 *   native_removed_library <plug-in>
 */
#include <dlfcn.h>
#include <stdio.h>

int ocerz_removed_probe(void);

#ifdef PLUGIN
int ocerz_removed_plugin(void)
{
    return ocerz_removed_probe();
}
#else
int main(int argc, char **argv)
{
    printf("removed library: call %d\n", ocerz_removed_probe());
    if (argc > 1) {
        void *h = dlopen(argv[1], RTLD_NOW);
        int (*call)(void) = h ? (int (*)(void))dlsym(h, "ocerz_removed_plugin") : NULL;
        printf("plug-in loaded %d, call %d\n", h != NULL, call ? call() : -1);
    }
    return 0;
}
#endif

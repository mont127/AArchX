/*
 * A library that lives only in the Rosetta cryptex opens by its system path.
 *
 * macOS keeps some x86_64 libraries outside the shared cache, in the Rosetta
 * cryptex at /System/Volumes/Preboot/Cryptexes/Rosetta, and an Intel process
 * names them by the path they would have without it: Metal opens
 * /usr/lib/libMTLHud.dylib when MTL_HUD_ENABLED=1, and no file exists there on
 * disk.  ocerz answered that dlopen with "image not found", so the Metal
 * performance HUD never appeared under it while it did under Rosetta.
 *
 * The case opens the library by its system path, then again, and requires the
 * same handle both times, so a second open maps nothing new.  It first checks
 * that the system path really is absent and the cryptex copy present; on a
 * machine laid out otherwise there is nothing to test and it says so.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    const char *path = "/usr/lib/libMTLHud.dylib";
    const char *cryptex = "/System/Volumes/Preboot/Cryptexes/Rosetta/usr/lib/libMTLHud.dylib";
    if (access(path, F_OK) == 0 || access(cryptex, F_OK) != 0) {
        printf("OK (this machine has no cryptex-only library to open)\n");
        return 0;
    }
    void *a = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (!a) {
        printf("dlopen_cryptex bad: dlopen(%s): %s\n", path, dlerror());
        return 1;
    }
    void *b = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (b != a) {
        printf("dlopen_cryptex bad: a second dlopen returned %p, the first %p\n", b, a);
        return 1;
    }
    printf("OK\n");
    return 0;
}

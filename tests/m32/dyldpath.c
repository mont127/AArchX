/* A library is looked up by its leaf name in the DYLD_LIBRARY_PATH directories first, as dyld does: Valve's hl2.sh
 * points it at Left 4 Dead 2's bin, and the engine dlopens its libraries by paths that only resolve that way. Here the
 * path names a directory that does not exist, and the leaf sits next to the program. Expected output written by hand. */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    char dir[1024];
    uint32_t size = sizeof dir;
    _NSGetExecutablePath(dir, &size);
    *strrchr(dir, '/') = 0;
    printf("before %d\n", dlopen("/nonexistent-m32/lib/libm32dl.dylib", RTLD_NOW) != NULL);
    char path[1100];
    snprintf(path, sizeof path, "/nonexistent-m32:%s", dir);
    setenv("DYLD_LIBRARY_PATH", path, 1);
    void *h = dlopen("/nonexistent-m32/lib/libm32dl.dylib", RTLD_NOW);
    const char *(*fn)(void) = h ? (const char *(*)(void))dlsym(h, "m32dl_name") : NULL;
    printf("leaf found %s\n", fn ? fn() : "no");
    return 0;
}

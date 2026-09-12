/*
 * dlopen of the running executable's own path must return the image that is
 * already loaded, not a second copy: dlopen(NULL) and dlopen(<own path>) have
 * to hand back the same handle, which is what native dyld does.  Mapping a
 * duplicate gave Steam's bootstrapper - which dlopens its own steam_osx - two
 * copies of the GURLHelper and UpdateEventHandlers objc classes, and steamui
 * crashed on a null vtable.
 */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    char self[4096];
    uint32_t sz = sizeof self;

    if (_NSGetExecutablePath(self, &sz) != 0) {
        printf("BAD nsget\n");
        return 1;
    }
    void *m = dlopen(NULL, RTLD_NOW);
    void *h = dlopen(self, RTLD_NOW);
    if (!m || !h) {
        printf("BAD dlopen m=%p h=%p\n", m, h);
        return 1;
    }
    if (m != h) {
        printf("BAD dup m=%p h=%p\n", m, h);
        return 1;
    }
    printf("OK\n");
    return 0;
}

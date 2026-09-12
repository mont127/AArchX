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

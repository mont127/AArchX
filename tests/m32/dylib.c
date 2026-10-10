#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <mach-o/dyld.h>
#include <string.h>
int m32t_add(int);
int main(void) {
    printf("linked %d\n", m32t_add(1));
    void *h = dlopen("@executable_path/libm32dl.dylib", RTLD_NOW);
    const char *(*fn)(void) = h ? (const char *(*)(void))dlsym(h, "m32dl_name") : 0;
    printf("dlopen %s\n", fn ? fn() : dlerror());
    printf("missing %d\n", dlsym(h, "nope") == NULL);
    void *sys = dlsym(RTLD_DEFAULT, "strlen"); printf("system %d\n", sys && ((size_t (*)(const char *))sys)("four") == 4);
    int found = 0; for (uint32_t i = 0; i < _dyld_image_count(); i++) if (strstr(_dyld_get_image_name(i), "libm32t.dylib")) found = 1;
    printf("images %d\n", found);
    char path[1024]; uint32_t sz = sizeof path; printf("exe %d\n", _NSGetExecutablePath(path, &sz) == 0 && strstr(path, "dylib") != 0);
    Dl_info info; printf("dladdr %d\n", dladdr((void *)m32t_add, &info) && strstr(info.dli_fname, "libm32t") != 0);
    return 0;
}

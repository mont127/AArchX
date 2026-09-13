#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const char *alias = "/System/Library/Frameworks/OpenGL.framework/Resources//GLRendererFloat.bundle/GLRendererFloat";
    const char *real = "/System/Library/Frameworks/OpenGL.framework/Versions/A/Resources/GLRendererFloat.bundle/GLRendererFloat";
    const char *mercury =
        "/System/Library/PrivateFrameworks/GPUSupport.framework/Versions/A/Libraries/libGPUSupportMercury.dylib";
    bool (*contains)(const char *) = (bool (*)(const char *))dlsym(RTLD_DEFAULT, "_dyld_shared_cache_contains_path");
    const char *(*real_path)(const char *) =
        (const char *(*)(const char *))dlsym(RTLD_DEFAULT, "_dyld_shared_cache_real_path");
    if (!contains || !real_path) {
        printf("BAD spi contains=%d real_path=%d\n", contains != NULL, real_path != NULL);
        return 1;
    }
    if (!contains(alias) || !contains(real)) {
        printf("BAD contains alias=%d real=%d\n", contains(alias), contains(real));
        return 1;
    }
    const char *rp = real_path(alias);
    if (!rp || strcmp(rp, real) != 0) {
        printf("BAD real_path \"%s\"\n", rp ? rp : "(null)");
        return 1;
    }
    void *fl = dlopen(alias, RTLD_LAZY | RTLD_LOCAL);
    void *me = dlopen(mercury, RTLD_LAZY | RTLD_LOCAL);
    if (!fl || !me) {
        printf("BAD dlopen float=%p mercury=%p\n", fl, me);
        return 1;
    }
    void *a = dlsym(fl, "gldPopulateRendererInfo");
    void *b = dlsym(me, "gldPopulateRendererInfo");
    if (!a || !b || a == b) {
        printf("BAD dlsym float=%p mercury=%p\n", a, b);
        return 1;
    }
    if (!dlsym(fl, "malloc") || dlsym(fl, "ocerz_no_such_symbol")) {
        printf("BAD dependents\n");
        return 1;
    }
    printf("OK\n");
    return 0;
}

/* dlsym on a handle searches the image and, breadth first, everything it links
 * except upward links, which is what dyld does: c_only two levels down and malloc
 * through libSystem are found, u_only behind liba's upward link is not.  The same
 * holds for cache images, so CoreFoundation's handle does not reach OpenGL's
 * glBegin through the chain its upward links open, while OpenGL's reaches malloc. */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) {
        printf("dlopen: %s\n", dlerror());
        return 1;
    }
    int bad = 0;
    bad |= !dlsym(h, "a_only") << 0;
    bad |= !dlsym(h, "b_only") << 1;
    bad |= !dlsym(h, "c_only") << 2;
    bad |= (dlsym(h, "u_only") != NULL) << 3;
    bad |= !dlsym(h, "malloc") << 4;
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    void *gl = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_NOW);
    bad |= (!cf || dlsym(cf, "glBegin") != NULL) << 5;
    bad |= (!gl || !dlsym(gl, "glBegin") || !dlsym(gl, "malloc")) << 6;
    if (bad)
        printf("BAD %#x\n", bad);
    else
        printf("OK\n");
    return 0;
}

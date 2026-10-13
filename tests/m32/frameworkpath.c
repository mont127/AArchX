/* dlopen of a framework by its short path, X.framework/X, which is a chain of symlinks to Versions/<v>/X and whose
 * file lives only in the shared cache: Left 4 Dead 2's vgui opens ApplicationServices that way to find the ATS and ATSU
 * calls. The handle is the framework's, as the install name names it. Expected output written by hand. */
#include <dlfcn.h>
#include <stdio.h>

int main(void)
{
    void *h = dlopen("/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices", RTLD_NOW);
    printf("opened %d\n", h != NULL);
    unsigned (*display)(void) = h ? (unsigned (*)(void))dlsym(h, "CGMainDisplayID") : NULL;
    printf("CGMainDisplayID %d\n", display && display() != 0);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    printf("CoreFoundation %d\n", cf && dlsym(cf, "CFStringGetLength"));
    return 0;
}

/*
 * A library dlopened out of the shared cache has to join the loaded-image list.
 *
 * In cache mode the dyld APIs answer _dyld_image_count, _dyld_get_image_name
 * and _dyld_get_image_header from the closure ocerz computes at startup.  A
 * dlopen of a disk dylib appended to that closure; a dlopen of a cache image
 * returned the mach header and appended nothing, so the library was loaded,
 * its symbols resolved and dladdr knew where they lived, while every walk of
 * the image list said it was not there.
 *
 * That is not a cosmetic gap, because libobjc keys its per-image queries off
 * the list: objc_copyClassNamesForImage returned nothing for a bundle whose
 * classes objc_getClass could already find.  Metal loads its GPU driver that
 * way, so MTLCreateSystemDefaultDevice returned nil and MTLCopyAllDevices
 * found no device at all; OpenGL then had no accelerated renderer to offer and
 * CGLChoosePixelFormat failed for every accelerated attribute set.  Brawlhalla
 * put a window on screen and never drew into it.
 *
 * The library used here is reached through the cache rather than opened from
 * disk, and the check runs before and after so a library that happened to be
 * loaded already cannot make the test pass on its own.  The header the list
 * reports is compared against the image a symbol of that library actually
 * lives in, since a name in the list pointing at the wrong image would satisfy
 * a test that only looked for the name.  The dlopen handle itself is not
 * compared, because a real dyld hands back an opaque handle rather than the
 * mach header.
 */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <string.h>

static int index_of(const char *fragment)
{
    uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n; i++) {
        const char *name = _dyld_get_image_name(i);
        if (name && strstr(name, fragment))
            return (int)i;
    }
    return -1;
}

static int one(const char *path, const char *fragment, const char *symbol)
{
    if (index_of(fragment) >= 0) {
        printf("dlopen_image_list bad: %s was already listed\n", fragment);
        return 1;
    }
    uint32_t before = _dyld_image_count();
    void *h = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
        printf("dlopen_image_list bad: dlopen(%s): %s\n", path, dlerror());
        return 1;
    }
    int idx = index_of(fragment);
    if (idx < 0) {
        printf("dlopen_image_list bad: %s is loaded but not listed (count %u -> %u)\n",
               fragment, before, _dyld_image_count());
        return 1;
    }
    if (_dyld_image_count() <= before) {
        printf("dlopen_image_list bad: count did not grow past %u\n", before);
        return 1;
    }
    const void *header = _dyld_get_image_header((uint32_t)idx);
    void *f = dlsym(h, symbol);
    Dl_info di;
    if (!f || !dladdr(f, &di) || !di.dli_fname || !strstr(di.dli_fname, fragment)) {
        printf("dlopen_image_list bad: %s came from %s\n", symbol,
               (f && dladdr(f, &di) && di.dli_fname) ? di.dli_fname : "nowhere");
        return 1;
    }
    if (header != di.dli_fbase) {
        printf("dlopen_image_list bad: %s listed at header %p, %s lives in %p\n",
               fragment, header, symbol, di.dli_fbase);
        return 1;
    }
    return 0;
}

int main(void)
{
    int bad = 0;
    bad |= one("/usr/lib/libxml2.2.dylib", "libxml2", "xmlReadMemory");
    bad |= one("/System/Library/Frameworks/CoreMIDI.framework/CoreMIDI",
               "CoreMIDI", "MIDIGetNumberOfDevices");
    if (!bad)
        printf("OK\n");
    return bad;
}

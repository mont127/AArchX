/*
 * A library reached through a symlink whose target lives only in the cache.
 *
 * /usr/lib/swift/libswiftNetwork.dylib is a symlink into
 * Network.framework, and the framework's binary is not on disk at all: it
 * exists only inside the shared cache.  Eleven of the twelve dylibs in
 * /usr/lib/swift are that shape, so it is how a Swift program reaches most of
 * the overlays it links against.
 *
 * The loader used to look the requested path up in the cache, follow the
 * symlink when that missed, and then go straight to opening the file the
 * symlink pointed at - without asking the cache about the name it had just
 * arrived at.  The target is never on disk, so the load failed with "Library
 * not loaded ... (no such file)" naming a path the cache was holding all
 * along.  Tailscale lost Network.framework that way, through Sparkle.
 *
 * Both spellings are checked, since the failure needed the symlink: opening the
 * framework directly proves the cache lookup works, and opening it through
 * /usr/lib/swift proves the lookup is retried after the name changes.  A symbol
 * is called through each handle, because a handle that resolves nothing would
 * pass a test that only checked for non-null.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static int try_one(const char *path, const char *sym)
{
    void *h = dlopen(path, RTLD_LAZY);
    if (!h) {
        printf("cache_symlink_dep bad: dlopen(%s) failed: %s\n", path, dlerror());
        return 1;
    }
    void *f = dlsym(h, sym);
    if (!f) {
        printf("cache_symlink_dep bad: %s has no %s: %s\n", path, sym, dlerror());
        return 1;
    }
    Dl_info di;
    if (!dladdr(f, &di) || !di.dli_fname || !strstr(di.dli_fname, "Network")) {
        printf("cache_symlink_dep bad: %s came from %s\n", sym,
               (dladdr(f, &di) && di.dli_fname) ? di.dli_fname : "nowhere");
        return 1;
    }
    return 0;
}

int main(void)
{
    int bad = 0;
    bad |= try_one("/System/Library/Frameworks/Network.framework/Network", "nw_path_monitor_create");
    bad |= try_one("/usr/lib/swift/libswiftNetwork.dylib", "nw_path_monitor_create");
    if (!bad)
        printf("OK\n");
    return bad;
}

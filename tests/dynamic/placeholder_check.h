#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static int placeholders_ok(const char *when)
{
    void *(*cls)(const char *) = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
    void *(*sel)(const char *) = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
    void *(*msg)(void *, void *) = (void *(*)(void *, void *))dlsym(RTLD_DEFAULT, "objc_msgSend");
    const char *(*name)(void *) = (const char *(*)(void *))dlsym(RTLD_DEFAULT, "object_getClassName");
    if (!cls || !sel || !msg || !name || !cls("NSString") || !cls("NSMutableString")) {
        printf("BAD %s no objc\n", when);
        return 0;
    }
    const char *s = name(msg(cls("NSString"), sel("alloc")));
    const char *m = name(msg(cls("NSMutableString"), sel("alloc")));
    if (strcmp(s, "NSPlaceholderString") != 0 || strcmp(m, "NSPlaceholderMutableString") != 0) {
        printf("BAD %s %s %s\n", when, s, m);
        return 0;
    }
    return 1;
}

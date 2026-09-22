/*
 * An upward dependency has to be initialized, not merely reachable.
 *
 * CoreFoundation declares Foundation as an upward dependency, which is how a
 * library names the back edge of a dependency cycle: CoreFoundation may be
 * initialized before Foundation, so the link is an ordering edge for nothing.
 * It is still a dependency.  dyld loads it and runs its initializer like any
 * other; it only declines to put it first.
 *
 * Leaving it out entirely is not visible the way a missing library usually is,
 * because the whole shared cache is mapped: Foundation's code is reachable and
 * its classes register, so a message to NSString dispatches and arrives.  What
 * is missing is whatever Foundation's own initializer sets up.  NSString is a
 * class cluster, so it stays abstract, and the first +[NSString
 * stringWithFormat:] lands in _NSRequestConcreteObject - which builds its
 * complaint with +[NSString stringWithFormat:].  Each turn of that recursion
 * consumes a frame and it runs the 8 MB guest stack out.  sw_vers fails exactly
 * that way, and nothing about the report names the cause.
 *
 * This program links CoreFoundation and not Foundation, which is what makes
 * the upward link the only path to it, and then reaches Foundation the way
 * CoreFoundation does: _CFCopySupplementalVersionDictionary builds strings.
 * It also sends +[NSString stringWithFormat:] itself, through the runtime
 * rather than through a link, so the case still holds if that private function
 * is rewritten.  Both must answer; either one recursing means the upward
 * dependency was loaded and never initialized.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>

int main(void)
{
    int bad = 0;

    typedef CFDictionaryRef (*supp_t)(void);
    supp_t supp = (supp_t)dlsym(RTLD_DEFAULT, "_CFCopySupplementalVersionDictionary");
    if (!supp) {
        printf("upward_init skip: no _CFCopySupplementalVersionDictionary\n");
    } else {
        CFDictionaryRef d = supp();
        if (!d || CFDictionaryGetCount(d) <= 0) {
            printf("upward_init bad: supplemental dictionary %p\n", (const void *)d);
            bad = 1;
        }
        if (d)
            CFRelease(d);
    }

    Class nsstring = objc_getClass("NSString");
    if (!nsstring) {
        printf("upward_init bad: Foundation's NSString is not registered\n");
        return 1;
    }
    SEL with_format = sel_registerName("stringWithFormat:");
    id (*send)(Class, SEL, CFStringRef, ...) = (id (*)(Class, SEL, CFStringRef, ...))objc_msgSend;
    id s = send(nsstring, with_format, CFSTR("answer=%d"), 42);
    if (!s) {
        printf("upward_init bad: +[NSString stringWithFormat:] answered nil\n");
        return 1;
    }
    char buf[64] = { 0 };
    if (!CFStringGetCString((CFStringRef)s, buf, sizeof buf, kCFStringEncodingUTF8) ||
        strcmp(buf, "answer=42") != 0) {
        printf("upward_init bad: formatted '%s', want 'answer=42'\n", buf);
        bad = 1;
    }

    if (!bad)
        printf("OK\n");
    return bad;
}

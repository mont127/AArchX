/* Run loop observers and version 0 sources from i386 code: their contexts are five and ten 4-byte fields there, 8-byte
 * ones on the host, with callbacks in them. Left 4 Dead 2's threads crashed the host passing them through.
 * runloopsrc.expected is the x86_64 build's output under Rosetta. */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

static int performed, observed, retained, released, scheduled;
static const void *keep(const void *info) { retained++; return info; }
static void drop(const void *info) { (void)info; released++; }
static void schedule(void *info, CFRunLoopRef rl, CFStringRef mode) { (void)rl; (void)mode; scheduled += info == &performed; }
static void perform(void *info) { *(int *)info += 1; }
static void observe(CFRunLoopObserverRef o, CFRunLoopActivity a, void *info) { (void)o; (void)a; *(int *)info += 1; }

int main(void)
{
    CFRunLoopRef rl = CFRunLoopGetCurrent();
    CFRunLoopSourceContext sc = { 0, &performed, keep, drop, NULL, NULL, NULL, schedule, NULL, perform };
    CFRunLoopSourceRef src = CFRunLoopSourceCreate(NULL, 0, &sc);
    CFRunLoopObserverContext oc = { 0, &observed, keep, drop, NULL };
    CFRunLoopObserverRef obs = CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting | kCFRunLoopBeforeSources, true, 0,
                                                       observe, &oc);
    CFRunLoopAddSource(rl, src, kCFRunLoopDefaultMode);
    CFRunLoopAddObserver(rl, obs, kCFRunLoopDefaultMode);
    CFRunLoopSourceSignal(src);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    printf("source created %d scheduled %d performed %d observed %d\n", src != NULL, scheduled, performed,
           observed > 0);
    CFRunLoopRemoveObserver(rl, obs, kCFRunLoopDefaultMode);
    CFRunLoopRemoveSource(rl, src, kCFRunLoopDefaultMode);
    CFRelease(obs);
    CFRelease(src);
    printf("retained %d released %d\n", retained, released);
    return 0;
}

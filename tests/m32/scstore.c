/* SystemConfiguration calls that take a {version, info, retain, release, copyDescription} context, 4-byte fields on
 * i386 (Chromium in Left 4 Dead 2 watches the network with them), and pipe's int[2]. scstore.expected is the x86_64
 * build's output under Rosetta. */
#include <SystemConfiguration/SystemConfiguration.h>
#include <stdio.h>
#include <unistd.h>

static int retained, released;
static const void *keep(const void *info) { retained++; return info; }
static void drop(const void *info) { (void)info; released++; }
static void changed(SCDynamicStoreRef s, CFArrayRef keys, void *info) { (void)s; (void)keys; (void)info; }
static void reach(SCNetworkReachabilityRef t, SCNetworkReachabilityFlags f, void *info) { (void)t; (void)f; (void)info; }

int main(void)
{
    static int info;
    SCDynamicStoreContext dc = { 0, &info, keep, drop, NULL };
    SCDynamicStoreRef store = SCDynamicStoreCreate(NULL, CFSTR("m32 test"), changed, &dc);
    SCDynamicStoreRef store2 = SCDynamicStoreCreateWithOptions(NULL, CFSTR("m32 test 2"), NULL, changed, &dc);
    SCNetworkReachabilityRef r = SCNetworkReachabilityCreateWithName(NULL, "localhost");
    SCNetworkReachabilityContext rc = { 0, &info, keep, drop, NULL };
    Boolean set = SCNetworkReachabilitySetCallback(r, reach, &rc);
    printf("store %d store2 %d reachability callback %d retained %d\n", store != NULL, store2 != NULL, set, retained);
    CFRelease(store);
    CFRelease(store2);
    SCNetworkReachabilitySetCallback(r, NULL, NULL);
    CFRelease(r);
    int fds[2] = { -1, -1 };
    char buf[4] = "";
    int p = pipe(fds);
    write(fds[1], "abc", 4);
    read(fds[0], buf, 4);
    printf("released %d pipe %d [%s]\n", released, p, buf);
    return 0;
}

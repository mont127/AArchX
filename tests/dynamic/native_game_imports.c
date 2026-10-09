/*
 * Imports Intel games make that native mode used to get wrong, each printing a
 * line that must match what the same source prints built for arm64:
 *
 * complex   __divdc3 and __divsc3, which the arm64 libSystem does not export
 *           (Unity 2019's Mono), beside __muldc3, __mulsc3 and the powi pair.
 * cfuuid    CFUUIDGetConstantUUIDWithBytes, whose sixteen UInt8 arguments end
 *           on the stack, where arm64 packs them a byte apiece; then an IOKit
 *           plug-in for a bogus service, which must fail and not end the
 *           process (Feral's HID pad setup).
 * cleanup   pthread_exit running the thread's pthread_cleanup_push handlers,
 *           which are x86 code linked onto the host's own pthread_self.
 * deps      dlsym on a system library's handle also searching what it depends
 *           on: CoreText by its old ApplicationServices path, asked for
 *           CoreFoundation's CFStringGetTypeID (Rewired, in Cuphead).
 * rpath     dlopen of a bare leaf name found through the executable's
 *           LC_RPATH, as MonoKickstart games find their native libraries.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/hid/IOHIDLib.h>
#include <complex.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static volatile double g_one = 1.0;
static volatile int g_five = 5, g_minus_three = -3;

static void complex_calls(void)
{
    double k = g_one;
    double complex a = (3.0 + 4.0 * I) * k, b = (1.0 - 2.0 * I) * k;
    float complex c = (1.5f + 2.5f * I) * (float)k, d = (0.5f - 1.0f * I) * (float)k;
    double complex q = a / b, m = a * b;
    float complex qf = c / d, mf = c * d;
    double p = __builtin_powi(1.5 * k, g_five);
    float pf = __builtin_powif(2.0f * (float)k, g_minus_three);
    double complex inf = (INFINITY + 1.0 * I) / (2.0 + 0.0 * I);
    printf("complex %g%+gi %g%+gi %g%+gi %g%+gi %g %g inf=%d\n", creal(q), cimag(q), creal(m), cimag(m),
           (double)crealf(qf), (double)cimagf(qf), (double)crealf(mf), (double)cimagf(mf), p, (double)pf,
           isinf(creal(inf)));
}

static void cfuuid(void)
{
    static const UInt8 want[16] = { 0x78, 0xBD, 0x42, 0x0C, 0x6F, 0x14, 0x11, 0xD4,
                                    0x94, 0x74, 0x00, 0x05, 0x02, 0x8F, 0x18, 0xD5 };
    CFUUIDRef u = CFUUIDGetConstantUUIDWithBytes(NULL, 0x78, 0xBD, 0x42, 0x0C, 0x6F, 0x14, 0x11, 0xD4,
                                                 0x94, 0x74, 0x00, 0x05, 0x02, 0x8F, 0x18, 0xD5);
    CFUUIDBytes b = u ? CFUUIDGetUUIDBytes(u) : (CFUUIDBytes){ 0 };
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(0x53480001, kIOHIDDeviceUserClientTypeID,
                                                         kIOCFPlugInInterfaceID, &plugin, &score);
    printf("cfuuid bytes=%s plugin=%s\n", u && memcmp(&b, want, 16) == 0 ? "match" : "differ",
           kr == KERN_SUCCESS ? "made" : "refused");
}

static int g_ran;

static void handler(void *arg)
{
    g_ran = g_ran * 10 + (int)(long)arg;
}

static void *exiting_thread(void *arg)
{
    pthread_cleanup_push(handler, (void *)1);
    pthread_cleanup_push(handler, (void *)2);
    pthread_exit(arg);
    pthread_cleanup_pop(0);
    pthread_cleanup_pop(0);
    return NULL;
}

static void cleanup(void)
{
    pthread_t t;
    void *result = NULL;
    pthread_create(&t, NULL, exiting_thread, (void *)42);
    pthread_join(t, &result);
    printf("cleanup order=%d result=%ld\n", g_ran, (long)result);
}

static void deps(void)
{
    void *ct = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Frameworks/CoreText.framework/CoreText",
                      RTLD_NOW);
    CFTypeID (*type_id)(void) = ct ? (CFTypeID (*)(void))dlsym(ct, "CFStringGetTypeID") : NULL;
    printf("deps handle=%d found=%d same=%d\n", ct != NULL, type_id != NULL,
           type_id && type_id() == CFStringGetTypeID());
}

static void rpath(void)
{
    void *h = dlopen("libocerzleaf.dylib", RTLD_NOW);
    int (*leaf)(void) = h ? (int (*)(void))dlsym(h, "ocerz_leaf") : NULL;
    printf("rpath handle=%d leaf=%d\n", h != NULL, leaf ? leaf() : -1);
}

int main(void)
{
    complex_calls();
    cfuuid();
    cleanup();
    deps();
    rpath();
    return 0;
}

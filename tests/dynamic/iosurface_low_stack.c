#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>

static CFNumberRef num(int v)
{
    return CFNumberCreate(NULL, kCFNumberIntType, &v);
}

static int run(void)
{
    CFMutableDictionaryRef p = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                         &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(p, kIOSurfaceWidth, num(64));
    CFDictionarySetValue(p, kIOSurfaceHeight, num(64));
    CFDictionarySetValue(p, kIOSurfaceBytesPerElement, num(4));
    CFDictionarySetValue(p, kIOSurfacePixelFormat, num(0x42475241));
    IOSurfaceRef s = IOSurfaceCreate(p);
    if (!s) {
        printf("create failed\n");
        return 1;
    }
    IOSurfaceSetValue(s, CFSTR("Name"), CFSTR("hello"));
    int fails = 0;
    for (int i = 0; i < 4; i++) {
        CFTypeRef v = IOSurfaceCopyValue(s, CFSTR("Name"));
        if (!v)
            fails++;
        else
            CFRelease(v);
    }
    if (fails)
        printf("%d of 4 IOSurfaceCopyValue failed\n", fails);
    return fails != 0;
}

static int g_rc;

static void *thr(void *a)
{
    (void)a;
    g_rc = run();
    return NULL;
}

int main(void)
{
    void *stk = mmap((void *)0x40000000, 0x200000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (stk == MAP_FAILED)
        return 2;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstack(&at, stk, 0x200000);
    pthread_t t;
    if (pthread_create(&t, &at, thr, NULL) != 0)
        return 2;
    pthread_join(t, NULL);
    if (g_rc == 0)
        printf("OK\n");
    return g_rc;
}

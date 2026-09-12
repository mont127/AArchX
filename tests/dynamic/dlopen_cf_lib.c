#include <CoreFoundation/CoreFoundation.h>

static int g_cf_len;

__attribute__((constructor))
static void cf_lib_init(void)
{
    CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, "ocerz", kCFStringEncodingUTF8);
    if (s) {
        g_cf_len = (int)CFStringGetLength(s);
        CFRelease(s);
    }
}

int cf_lib_len(void)
{
    return g_cf_len;
}

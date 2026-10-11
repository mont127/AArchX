#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, "hello cf", kCFStringEncodingUTF8);
    printf("len %ld\n", (long)CFStringGetLength(s));
    char buf[64]; CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8); printf("back %s\n", buf);
    printf("handle %d\n", (unsigned)(uintptr_t)s >= 0xE0000000u && (unsigned)(uintptr_t)s < 0xF0000000u);
    CFStringRef c = CFSTR("constant");                       /* guest __cfstring: aliased to a host CFString */
    printf("const len %ld eq %d\n", (long)CFStringGetLength(c), CFStringCompare(c, CFSTR("constant"), 0) == kCFCompareEqualTo);
    const void *vals[2] = { s, c }; CFArrayRef a = CFArrayCreate(NULL, vals, 2, &kCFTypeArrayCallBacks);
    printf("count %ld same %d\n", (long)CFArrayGetCount(a), CFArrayGetValueAtIndex(a, 0) == s);
    CFRange r = CFStringFind(s, CFSTR("cf"), 0); printf("find %ld %ld\n", (long)r.location, (long)r.length);
    /* a by-value CFRange between pointers and the options word */
    CFStringRef up = CFSTR("EN");
    printf("cmp exact %d nocase %d\n", (int)CFStringCompareWithOptions(up, CFSTR("en"), CFRangeMake(0, 2), 0),
           (int)CFStringCompareWithOptions(up, CFSTR("en"), CFRangeMake(0, 2), kCFCompareCaseInsensitive));
    const void *keys[2] = { CFSTR("k1"), CFSTR("k2") }, *dv[2] = { CFSTR("v1"), CFSTR("v2") }, *ko[2], *vo[2];
    CFDictionaryRef d = CFDictionaryCreate(NULL, keys, dv, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryGetKeysAndValues(d, ko, vo);
    printf("keys %d\n", CFDictionaryGetValue(d, ko[0]) == vo[0] && CFDictionaryGetValue(d, ko[1]) == vo[1]);
    /* binary bytes, zeros included, read through the returned pointer (a keyboard layout is such data) */
    unsigned char raw[300];
    for (int i = 0; i < 300; i++) raw[i] = (unsigned char)(i % 7 ? i * 13 : 0);
    CFDataRef bd = CFDataCreate(NULL, raw, sizeof raw);
    CFMutableDataRef md = CFDataCreateMutableCopy(NULL, 0, bd);
    printf("bytes %d mutable %d\n", memcmp(CFDataGetBytePtr(bd), raw, sizeof raw) == 0,
           memcmp(CFDataGetMutableBytePtr(md), raw, sizeof raw) == 0);
    CFRelease(md); CFRelease(bd);
    CFRelease(d); CFRelease(a); CFRelease(s);
    return 0;
}

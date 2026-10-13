/* CFNumber with the types named after long, CFIndex, NSInteger and CGFloat, which are 4 bytes on i386 (8 on the host):
 * a value read back stays within its own bytes, and one created from them is whole. cfnumber.expected is the x86_64
 * build's output under Rosetta. */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

int main(void)
{
    CFNumberType types[] = { kCFNumberLongType, kCFNumberCFIndexType, kCFNumberNSIntegerType };
    for (int i = 0; i < 3; i++) {
        long in = -123456;
        CFNumberRef n = CFNumberCreate(NULL, types[i], &in);
        struct { long v, guard; } out = { 0, 0x5a5a };
        Boolean ok = CFNumberGetValue(n, types[i], &out.v);
        printf("type %d: ok %d value %ld guard %#lx\n", (int)types[i], ok, out.v, out.guard);
        CFRelease(n);
    }
    CGFloat f = 2.5;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberCGFloatType, &f);
    struct { CGFloat v; long guard; } out = { 0, 0x5a5a };
    CFNumberGetValue(n, kCFNumberCGFloatType, &out.v);
    printf("CGFloat: value %.2f guard %#lx\n", (double)out.v, out.guard);
    return 0;
}

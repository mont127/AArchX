/* CFUUIDGetConstantUUIDWithBytes and CFUUIDCreateWithBytes take sixteen byte arguments, more than a notation holds.
 * IOKit's plug-in UUIDs are made this way (Left 4 Dead 2's input system). cfuuid.expected is the x86_64 build's
 * output under Rosetta. */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

int main(void)
{
    CFUUIDRef a = CFUUIDGetConstantUUIDWithBytes(NULL, 0xFA, 0x12, 0xFA, 0x38, 0x6F, 0x1A, 0x11, 0xD4, 0xBA, 0x0C, 0x00,
                                                 0x05, 0x02, 0x8F, 0x18, 0xD5);
    CFUUIDRef b = CFUUIDGetConstantUUIDWithBytes(NULL, 0xFA, 0x12, 0xFA, 0x38, 0x6F, 0x1A, 0x11, 0xD4, 0xBA, 0x0C, 0x00,
                                                 0x05, 0x02, 0x8F, 0x18, 0xD5);
    CFUUIDRef c = CFUUIDCreateWithBytes(NULL, 0xFA, 0x12, 0xFA, 0x38, 0x6F, 0x1A, 0x11, 0xD4, 0xBA, 0x0C, 0x00, 0x05,
                                        0x02, 0x8F, 0x18, 0xD5);
    char buf[64] = "(null)";
    CFStringRef s = a ? CFUUIDCreateString(NULL, a) : NULL;
    if (s)
        CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingASCII);
    CFUUIDBytes by = CFUUIDGetUUIDBytes(c);
    printf("constant %s same %d equal %d last byte %#x\n", buf, a == b, a && c && CFEqual(a, c), by.byte15);
    return 0;
}

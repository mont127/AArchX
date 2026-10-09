/*
 * A non-PIE executable, linked -no_pie at x86_64's default 4 GB base, which
 * native mode slides into its arena: nothing tells it where the image's
 * absolute pointers are, so the loader finds them by scanning __DATA.  Each
 * kind is used once: a function table, a C string pointer, a constant
 * CFString and an Objective-C class with a method of its own.
 */
#import <Foundation/Foundation.h>
#include <stdio.h>

@interface OcerzNoPie : NSObject
- (int)value;
@end

@implementation OcerzNoPie
- (int)value
{
    return 7;
}
@end

static int one(void) { return 1; }
static int two(void) { return 2; }
static int (*table[])(void) = { one, two };
static const char *message = "hello";
static NSString *const string = @"cfstring";

int main(void)
{
    @autoreleasepool {
        printf("nopie table=%d message=%c string=%d method=%d\n", table[0]() + table[1](), message[0],
               [string isEqualToString:@"cfstring"], [[OcerzNoPie new] value]);
    }
    return 0;
}

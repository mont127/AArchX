#import <Foundation/Foundation.h>
static int depth(int n) { if (!n) @throw [NSException exceptionWithName:@"Deep" reason:@"r" userInfo:nil]; return depth(n - 1) + 1; }
int main(void) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    @try { depth(20); } @catch (NSException *e) { printf("guest-throw %s\n", [[e name] UTF8String]); }
    @try { [NSException raise:@"HostRaised" format:@"n=%d", 3]; } @catch (NSException *e) { printf("host-raise %s %s\n", [[e name] UTF8String], [[e reason] UTF8String]); }
    @try { [[NSArray array] objectAtIndex:5]; } @catch (NSException *e) { printf("range %s\n", [[e name] UTF8String]); }
    int fin = 0; @try { @try { @throw @"str"; } @finally { fin = 1; } } @catch (id x) { printf("finally %d %s\n", fin, [x UTF8String]); }
    [pool drain];
    return 0;
}

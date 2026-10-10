#import <Foundation/Foundation.h>
#import <objc/runtime.h>
@interface Greeter : NSObject - (NSString *)greet; @end
@implementation Greeter - (NSString *)greet { return @"hello"; } @end
static IMP g_old;
static NSString *loud(id self, SEL _cmd) { return [((NSString *(*)(id, SEL))g_old)(self, _cmd) uppercaseString]; }
int main(void) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    Method m = class_getInstanceMethod([Greeter class], @selector(greet));
    g_old = method_setImplementation(m, (IMP)loud);
    printf("guest %s\n", [[[[Greeter alloc] init] greet] UTF8String]);
    Method d = class_getInstanceMethod([NSObject class], @selector(description));
    IMP hostimp = method_getImplementation(d);
    Greeter *g = [[Greeter alloc] init];
    NSString *s = ((NSString *(*)(id, SEL))hostimp)(g, @selector(description));
    printf("host-imp %d\n", [s hasPrefix:@"<Greeter"]);
    [pool drain];
    return 0;
}

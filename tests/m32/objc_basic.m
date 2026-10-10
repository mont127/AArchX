#import <Foundation/Foundation.h>
@interface Counter : NSObject { int _n; NSString *_name; }
- (id)initWithName:(NSString *)name;
- (int)bump;
+ (id)counterNamed:(NSString *)name;
@end
@implementation Counter
- (id)initWithName:(NSString *)name { if ((self = [super init])) { _name = [name copy]; _n = 10; } return self; }
- (int)bump { return ++_n; }
- (NSString *)description { return [NSString stringWithFormat:@"<%@ %d>", _name, _n]; }
+ (id)counterNamed:(NSString *)name { return [[[self alloc] initWithName:name] autorelease]; }
@end
@interface NSString (M32) - (NSUInteger)twiceLength; @end
@implementation NSString (M32) - (NSUInteger)twiceLength { return [self length] * 2; } @end
int main(void) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    Counter *c = [Counter counterNamed:@"c1"];
    [c bump];
    printf("bump %d\n", [c bump]);
    printf("desc %s\n", [[c description] UTF8String]);
    printf("host-sees %s\n", [[NSString stringWithFormat:@"%@", c] UTF8String]);
    printf("kind %d %d %d\n", [c isKindOfClass:[NSObject class]], [c isKindOfClass:[Counter class]], [@"x" isKindOfClass:[Counter class]]);
    printf("responds %d %d\n", [c respondsToSelector:@selector(bump)], [c respondsToSelector:@selector(nope)]);
    printf("category %lu\n", (unsigned long)[@"abc" twiceLength]);
    printf("class %s\n", [NSStringFromClass([c class]) UTF8String]);
    NSArray *a = [NSArray arrayWithObjects:@"one", @"two", c, nil];
    int n = 0; for (id o in a) n++;
    printf("array %lu %d %s\n", (unsigned long)[a count], n, [[[a objectAtIndex:1] uppercaseString] UTF8String]);
    NSDictionary *d = [NSDictionary dictionaryWithObject:[NSNumber numberWithInt:42] forKey:@"k"];
    printf("dict %d %.2f\n", [[d objectForKey:@"k"] intValue], [[NSNumber numberWithDouble:2.5] doubleValue]);
    NSLog(@"nslog %@ %d", @"works", 7);
    [pool drain];
    return 0;
}

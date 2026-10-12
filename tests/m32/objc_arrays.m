#import <Foundation/Foundation.h>
@interface Tag : NSObject @end
@implementation Tag - (NSString *)description { return @"tag"; } @end
int main(void) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    id objs[3] = { @"a", [NSNumber numberWithInt:2], [[[Tag alloc] init] autorelease] };
    id keys[3] = { @"k1", @"k2", @"k3" };
    NSArray *a = [NSArray arrayWithObjects:objs count:3];
    printf("array %lu %s %d %s\n", (unsigned long)[a count], [[a objectAtIndex:0] UTF8String], [[a objectAtIndex:1] intValue],
           [[[a objectAtIndex:2] description] UTF8String]);
    NSDictionary *d = [NSDictionary dictionaryWithObjects:objs forKeys:keys count:3];
    printf("dict %lu %s %d %d\n", (unsigned long)[d count], [[d objectForKey:@"k1"] UTF8String], [[d objectForKey:@"k2"] intValue],
           [d objectForKey:@"k3"] == objs[2]);
    NSDictionary *d2 = [[[NSDictionary alloc] initWithObjects:objs forKeys:keys count:2] autorelease];
    printf("init %lu\n", (unsigned long)[d2 count]);
    NSSet *s = [NSSet setWithObjects:objs count:3];
    printf("set %lu %d\n", (unsigned long)[s count], [s containsObject:@"a"]);
    id outo[3] = { 0 }, outk[3] = { 0 };
    [d getObjects:outo andKeys:outk count:3];
    int found = 0;
    for (int i = 0; i < 3; i++) found += [d objectForKey:outk[i]] == outo[i];
    printf("get %d\n", found);
    printf("empty %lu\n", (unsigned long)[[NSArray arrayWithObjects:objs count:0] count]);
    [pool drain];
    return 0;
}

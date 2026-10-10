#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
int main(void) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    __block int sum = 0;
    [[NSArray arrayWithObjects:@"a", @"bb", @"ccc", nil] enumerateObjectsUsingBlock:^(id o, NSUInteger i, BOOL *stop) { sum += (int)[o length] * (int)(i + 1); }];
    printf("enumerate %d\n", sum);
    NSArray *sorted = [[NSArray arrayWithObjects:@"b", @"c", @"a", nil] sortedArrayUsingComparator:^NSComparisonResult(id x, id y) { return [x compare:y]; }];
    printf("sorted %s\n", [[sorted componentsJoinedByString:@","] UTF8String]);
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block int ran = 0;
    dispatch_async(dispatch_get_global_queue(0, 0), ^{ ran = 7; dispatch_semaphore_signal(sem); });
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    printf("dispatch %d\n", ran);
    void (^copy)(void) = [^{ printf("copied block runs\n"); } copy]; copy(); [copy release];
    [pool drain];
    return 0;
}

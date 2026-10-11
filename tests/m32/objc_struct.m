#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
@interface Box : NSObject { NSRect _r; } - (NSRect)rect; - (void)setRect:(NSRect)r; - (CGFloat)area; @end
@implementation Box - (NSRect)rect { return _r; } - (void)setRect:(NSRect)r { _r = r; }
- (CGFloat)area { return _r.size.width * _r.size.height; } @end
int main(void) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSValue *v = [NSValue valueWithRect:NSMakeRect(1, 2, 30, 40)];
    NSRect r = [v rectValue];
    printf("rect %.0f %.0f %.0f %.0f\n", r.origin.x, r.origin.y, r.size.width, r.size.height);
    printf("str %s\n", [NSStringFromRect(r) UTF8String]);
    Box *b = [[Box alloc] init]; [b setRect:r];
    NSValue *w = [NSValue valueWithRect:[b rect]]; printf("guest-stret %.0f\n", [w rectValue].size.height);
    printf("host-calls-guest %.0f\n", [[b valueForKey:@"area"] doubleValue]);
    printf("fp %.3f %.3f\n", [[NSNumber numberWithFloat:1.25f] floatValue], [[NSNumber numberWithDouble:3.5] doubleValue]);
    NSPoint p = NSMakePoint(5, 6); NSValue *pv = [NSValue valueWithPoint:p]; printf("point %.0f %.0f\n", [pv pointValue].x, [pv pointValue].y);
    [pool drain];
    return 0;
}

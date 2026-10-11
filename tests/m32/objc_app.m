#import <AppKit/AppKit.h>
@interface App : NSApplication @end
@implementation App - (void)sendEvent:(NSEvent *)e { [super sendEvent:e]; } @end
@interface View : NSView <NSTextInputClient> @end   /* AppKit gives a text input context only to a conforming view */
@implementation View - (void)drawRect:(NSRect)r { static int once; if (!once++) printf("draw %.0f\n", r.size.width); }
- (void)insertText:(id)s replacementRange:(NSRange)r {}
- (void)doCommandBySelector:(SEL)s {}
- (void)setMarkedText:(id)s selectedRange:(NSRange)a replacementRange:(NSRange)b {}
- (void)unmarkText {}
- (NSRange)selectedRange { return NSMakeRange(NSNotFound, 0); }
- (NSRange)markedRange { return NSMakeRange(NSNotFound, 0); }
- (BOOL)hasMarkedText { return NO; }
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)r actualRange:(NSRangePointer)a { return nil; }
- (NSArray *)validAttributesForMarkedText { return [NSArray array]; }
- (NSRect)firstRectForCharacterRange:(NSRange)r actualRange:(NSRangePointer)a { return NSZeroRect; }
- (NSUInteger)characterIndexForPoint:(NSPoint)p { return NSNotFound; }
@end
@interface Delegate : NSObject <NSApplicationDelegate> { NSWindow *_w; int _ticks; } @end
@implementation Delegate
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    printf("launched %d\n", [n object] == NSApp);
    _w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 320, 200) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    [_w setContentView:[[[View alloc] initWithFrame:NSMakeRect(0, 0, 320, 200)] autorelease]];
    [_w setTitle:@"m32"]; [_w makeKeyAndOrderFront:nil];
    printf("frame %.0f %.0f\n", [_w contentRectForFrameRect:[_w frame]].size.width, [[_w contentView] frame].size.height);
    printf("input context %d\n", [[_w contentView] inputContext] != nil);
    [NSTimer scheduledTimerWithTimeInterval:0.05 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
}
- (void)tick:(NSTimer *)t { if (++_ticks == 3) { printf("ticks %d\n", _ticks); [t invalidate]; [NSApp stop:nil];
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:YES]; } }
@end
int main(void) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    App *app = (App *)[App sharedApplication];
    printf("app %d\n", app == NSApp && [app isKindOfClass:[App class]]);
    [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [app setDelegate:[[Delegate alloc] init]];
    [app run];
    printf("ran\n");
    [pool drain];
    return 0;
}

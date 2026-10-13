// A game that brings itself to the front as Left 4 Dead 2's launcher does from the script Steam runs: TransformProcessType
// and SetFrontProcess on kCurrentProcess before its NSApplication exists.  Today's cooperative activation ignores that
// while another app is in front, so the game ran unfocused (throttled, without the mouse) until the user switched away
// and back.  The app must now be asked to activate once its event loop turns, once per request; whether the window
// server then grants it depends on what is in front, so the request is what is counted.  Expected written by hand.
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <objc/message.h>

static int asked;
@interface App : NSApplication @end
@implementation App - (void)activateIgnoringOtherApps:(BOOL)flag { asked++; [super activateIgnoringOtherApps:flag]; } @end

int main(void)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    ProcessSerialNumber psn = { 0, kCurrentProcess }, other = { 0, 12345 };
    TransformProcessType(&psn, kProcessTransformToForegroundApplication);
    SetFrontProcess(&psn);
    SetFrontProcess(&other);   /* another process: no request for this one */
    [App sharedApplication];
    for (int i = 0; i < 50; i++) {
        NSEvent *e = ((NSEvent * (*)(id, SEL, NSUInteger, id, id, BOOL))objc_msgSend)(   /* a 4-byte mask, as the game */
            NSApp, @selector(nextEventMatchingMask:untilDate:inMode:dequeue:), 0xffffffffu,
            [NSDate dateWithTimeIntervalSinceNow:0.01], NSDefaultRunLoopMode, YES);
        if (e)
            [NSApp sendEvent:e];
    }
    printf("asked %d\n", asked);
    [pool drain];
    return 0;
}

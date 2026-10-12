// NSOpenGLContext from a render thread, as SDL 2 drives it: -setView: and -[super update] trap off the main thread in
// today's AppKit, so m32 runs them on the main thread, which here pumps its run loop as a game's event loop does.
#import <AppKit/AppKit.h>
#include <pthread.h>
@interface GameContext : NSOpenGLContext @end
@implementation GameContext - (void)update { [super update]; } @end
static GameContext *ctx;
static NSView *view;
static volatile int done;
static void *render(void *arg)
{
    (void)arg;
    [ctx setView:view];
    [ctx update];
    [ctx makeCurrentContext];
    [ctx flushBuffer];
    printf("render thread: view %s\n", [ctx view] == view ? "set" : "missing");
    done = 1;
    return NULL;
}
int main(void)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    [NSApplication sharedApplication];
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 160, 120) styleMask:NSTitledWindowMask
                                                backing:NSBackingStoreBuffered defer:NO];
    view = [w contentView];
    NSOpenGLPixelFormatAttribute at[] = { NSOpenGLPFADoubleBuffer, NSOpenGLPFAColorSize, 24, 0 };
    NSOpenGLPixelFormat *pf = [[[NSOpenGLPixelFormat alloc] initWithAttributes:at] autorelease];
    ctx = [[GameContext alloc] initWithFormat:pf shareContext:nil];
    pthread_t t;
    pthread_create(&t, NULL, render, NULL);
    for (int i = 0; i < 500 && !done; i++)
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    pthread_join(t, NULL);
    printf("main thread: done %d\n", done);
    [ctx release];
    [w release];
    [pool drain];
    return 0;
}

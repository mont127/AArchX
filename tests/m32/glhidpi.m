// OpenGL drawables as i386 Macs sized them: a view the program leaves alone has its drawable in points (the default was
// NO), one that opts in with setWantsBestResolutionOpenGLSurface:YES has the window's pixels. Linked with today's SDK,
// AppKit's default is YES, and Left 4 Dead 2 drew its frame into a quarter of the window. That holds for a view given
// to a context with setView: and for an NSOpenGLView, which Left 4 Dead 2 makes. The expectations are those defaults,
// the same on a display with or without Retina.
#import <AppKit/AppKit.h>
#import <OpenGL/gl.h>

static int drawable_matches(int opt_in)
{
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 320, 200) styleMask:NSTitledWindowMask
                                                backing:NSBackingStoreBuffered defer:NO];
    NSView *v = [w contentView];
    if (opt_in)
        [v setWantsBestResolutionOpenGLSurface:YES];
    NSOpenGLPixelFormatAttribute at[] = { NSOpenGLPFADoubleBuffer, NSOpenGLPFAColorSize, 24, 0 };
    NSOpenGLContext *c = [[NSOpenGLContext alloc] initWithFormat:[[[NSOpenGLPixelFormat alloc] initWithAttributes:at] autorelease]
                                                    shareContext:nil];
    [w orderFront:nil];
    [c setView:v];
    [c makeCurrentContext];
    GLint vp[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, vp);
    double scale = opt_in ? [w backingScaleFactor] : 1;
    int ok = vp[2] == (GLint)(320 * scale) && vp[3] == (GLint)(200 * scale);
    [NSOpenGLContext clearCurrentContext];
    [c release];
    [w close];
    return ok;
}

static int glview_best_resolution(int opt_in)
{
    NSOpenGLPixelFormatAttribute at[] = { NSOpenGLPFADoubleBuffer, NSOpenGLPFAColorSize, 24, 0 };
    NSOpenGLView *v = [[NSOpenGLView alloc] initWithFrame:NSMakeRect(0, 0, 320, 200)
                                              pixelFormat:[[[NSOpenGLPixelFormat alloc] initWithAttributes:at] autorelease]];
    if (opt_in)
        [v setWantsBestResolutionOpenGLSurface:YES];
    int r = [v wantsBestResolutionOpenGLSurface];
    [v release];
    return r;
}

int main(void)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    [NSApplication sharedApplication];
    printf("left alone: drawable in points %d\n", drawable_matches(0));
    printf("opted in: drawable in pixels %d\n", drawable_matches(1));
    printf("NSOpenGLView left alone: best resolution %d\n", glview_best_resolution(0));
    printf("NSOpenGLView opted in: best resolution %d\n", glview_best_resolution(1));
    [pool drain];
    return 0;
}

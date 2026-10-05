#include <Carbon/Carbon.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <ImageIO/ImageIO.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static size_t consumed;
static int released;

static size_t put_bytes(void *info, const void *buffer, size_t count)
{
    (void)buffer;
    (*(int *)info)++;
    consumed += count;
    return count;
}

static void release_consumer(void *info)
{
    (void)info;
    released++;
}

static void draw_cell(void *info, CGContextRef c)
{
    (*(int *)info)++;
    CGContextSetRGBFillColor(c, 1, 0, 0, 1);
    CGContextFillRect(c, CGRectMake(0, 0, 2, 2));
}

static void release_pattern(void *info)
{
    (void)info;
    released += 10;
}

static void graphics(void)
{
    int writes = 0;
    CGDataConsumerCallbacks cb = { put_bytes, release_consumer };
    CGDataConsumerRef consumer = CGDataConsumerCreate(&writes, &cb);
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    CGContextRef bm = CGBitmapContextCreate(NULL, 8, 8, 8, 32, rgb, kCGImageAlphaPremultipliedLast);
    int cells = 0;
    CGPatternCallbacks pcb = { 0, draw_cell, release_pattern };
    CGPatternRef pat = CGPatternCreate(&cells, CGRectMake(0, 0, 4, 4), CGAffineTransformIdentity, 4, 4,
                                       kCGPatternTilingConstantSpacing, true, &pcb);
    CGColorSpaceRef ps = CGColorSpaceCreatePattern(NULL);
    CGContextSetFillColorSpace(bm, ps);
    CGFloat alpha = 1;
    CGContextSetFillPattern(bm, pat, &alpha);
    CGContextFillRect(bm, CGRectMake(0, 0, 8, 8));
    const unsigned char *px = CGBitmapContextGetData(bm);
    printf("pattern cells=%d bottom-left=%u,%u,%u,%u top-right=%u,%u,%u,%u\n", cells > 0, px[224], px[225], px[226],
           px[227], px[28], px[29], px[30], px[31]);
    CGImageRef img = CGBitmapContextCreateImage(bm);
    CGImageDestinationRef dest = CGImageDestinationCreateWithDataConsumer(consumer, CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(dest, img, NULL);
    int ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    CGImageRelease(img);
    CGDataConsumerRelease(consumer);
    CGPatternRelease(pat);
    CGColorSpaceRelease(ps);
    CGContextRelease(bm);
    CGColorSpaceRelease(rgb);
    printf("consumer finalize=%d writes=%d bytes>0=%d released=%d\n", ok, writes > 0, consumed > 0, released);
}

static int events;
static CFIndex got;

static void on_stream(CFReadStreamRef s, CFStreamEventType type, void *info)
{
    (void)info;
    events |= (int)type;
    if (type == kCFStreamEventHasBytesAvailable) {
        UInt8 buf[64];
        CFIndex n = CFReadStreamRead(s, buf, sizeof buf);
        if (n > 0)
            got += n;
    }
    if (type == kCFStreamEventEndEncountered)
        CFRunLoopStop(CFRunLoopGetCurrent());
}

static int retained;
static void *ctx_retain(void *info) { retained++; return info; }
static void ctx_release(void *info) { (void)info; retained--; }

static void streams(void)
{
    static const UInt8 data[] = "stream client data";
    CFReadStreamRef s = CFReadStreamCreateWithBytesNoCopy(NULL, data, sizeof data - 1, kCFAllocatorNull);
    CFStreamClientContext ctx = { 0, &got, ctx_retain, ctx_release, NULL };
    Boolean set = CFReadStreamSetClient(s, kCFStreamEventOpenCompleted | kCFStreamEventHasBytesAvailable |
                                               kCFStreamEventEndEncountered, on_stream, &ctx);
    CFReadStreamScheduleWithRunLoop(s, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    CFReadStreamOpen(s);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 2, false);
    int held = retained;
    CFReadStreamSetClient(s, 0, NULL, NULL);
    CFReadStreamClose(s);
    CFRelease(s);
    printf("stream set=%d events=%#x got=%ld held=%d after=%d\n", set, events, (long)got, held > 0, retained);
}

static CGFloat ascent(void *r) { (void)r; return 31; }
static CGFloat descent(void *r) { (void)r; return 7; }
static CGFloat width(void *r) { (void)r; return 12; }
static void dealloc_run(void *r) { (void)r; }

static void text(void)
{
    CTRunDelegateCallbacks cb = { kCTRunDelegateCurrentVersion, dealloc_run, ascent, descent, width };
    CTRunDelegateRef d = CTRunDelegateCreate(&cb, NULL);
    CFMutableAttributedStringRef as = CFAttributedStringCreateMutable(NULL, 0);
    CFAttributedStringReplaceString(as, CFRangeMake(0, 0), CFSTR("￼"));
    CFAttributedStringSetAttribute(as, CFRangeMake(0, 1), kCTRunDelegateAttributeName, d);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    CGFloat a = 0, de = 0;
    double w = CTLineGetTypographicBounds(line, &a, &de, NULL);
    printf("run delegate ascent=%.0f descent=%.0f width=%.0f\n", a, de, w);
    CFRelease(line);
    CFRelease(as);
    CFRelease(d);
}

static OSErr on_event(const AppleEvent *e, AppleEvent *reply, SRefCon ref)
{
    (void)e;
    (void)reply;
    (void)ref;
    return noErr;
}

static void on_invalidate(CFMachPortRef port, void *info)
{
    (void)port;
    (void)info;
}

static void on_port(CFMachPortRef port, void *msg, CFIndex size, void *info)
{
    (void)port;
    (void)msg;
    (void)size;
    (void)info;
}

static void on_socket(CFSocketRef s, CFSocketCallBackType type, CFDataRef address, const void *data, void *info)
{
    (void)s;
    (void)address;
    (void)data;
    (*(int *)info) |= (int)type;
    CFRunLoopStop(CFRunLoopGetCurrent());
}

static const void *port_retain(const void *info)
{
    retained++;
    return info;
}

static void port_release(const void *info)
{
    (void)info;
    retained--;
}

static void functions_and_ports(void)
{
    AEEventHandlerUPP upp = NewAEEventHandlerUPP(on_event);
    printf("upp is the function=%d\n", upp == on_event);
    DisposeAEEventHandlerUPP(upp);
    int seen = 0;
    retained = 0;
    CFMachPortContext pctx = { 0, &seen, port_retain, port_release, NULL };
    Boolean free_info = false;
    CFMachPortRef port = CFMachPortCreate(NULL, on_port, &pctx, &free_info);
    CFMachPortSetInvalidationCallBack(port, on_invalidate);
    printf("mach port=%d invalidation callback back=%d held=%d\n", port != NULL,
           CFMachPortGetInvalidationCallBack(port) == on_invalidate, retained > 0);
    CFMachPortInvalidate(port);
    CFRelease(port);
    CFSocketContext sctx = { 0, &seen, port_retain, port_release, NULL };
    CFSocketRef s = CFSocketCreate(NULL, PF_INET, SOCK_DGRAM, 0, kCFSocketReadCallBack, on_socket, &sctx);
    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    CFSocketRef s2 = CFSocketCreateWithNative(NULL, fds[0], kCFSocketReadCallBack, on_socket, &sctx);
    CFRunLoopSourceRef src = CFSocketCreateRunLoopSource(NULL, s2, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
    write(fds[1], "x", 1);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 2, false);
    printf("socket created=%d native=%d seen=%#x\n", s != NULL, s2 != NULL, seen);
    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
    CFRelease(src);
    CFSocketInvalidate(s2);
    CFRelease(s2);
    CFSocketInvalidate(s);
    CFRelease(s);
    close(fds[1]);
}

int main(void)
{
    graphics();
    streams();
    text();
    functions_and_ports();
    return 0;
}

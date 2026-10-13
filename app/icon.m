/*
 * Draws AArchX.app's icon into an .iconset directory for iconutil: "AX" in the
 * system font's black compressed cut, on a near-black tile laid out on the
 * standard macOS icon grid (an 824-point tile in a 1024-point canvas).
 *
 *   icon <dir>.iconset
 */
#import <AppKit/AppKit.h>

static NSBitmapImageRep *draw(int px)
{
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                                    pixelsWide:px
                                                                    pixelsHigh:px
                                                                 bitsPerSample:8
                                                               samplesPerPixel:4
                                                                      hasAlpha:YES
                                                                      isPlanar:NO
                                                                colorSpaceName:NSDeviceRGBColorSpace
                                                                   bytesPerRow:0
                                                                  bitsPerPixel:0];
    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    NSAffineTransform *grid = [NSAffineTransform transform];
    [grid scaleBy:px / 1024.0];
    [grid concat];

    [[NSColor colorWithSRGBRed:0.06 green:0.06 blue:0.065 alpha:1] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(100, 100, 824, 824) xRadius:185 yRadius:185] fill];

    CGFloat size = 640;
    NSDictionary *a = @{
        NSFontAttributeName : [NSFont systemFontOfSize:size weight:NSFontWeightBlack width:NSFontWidthCompressed],
        NSForegroundColorAttributeName : [NSColor colorWithSRGBRed:0.97 green:0.97 blue:0.95 alpha:1],
        NSKernAttributeName : @(size * -0.02),
    };
    NSAttributedString *ax = [[NSAttributedString alloc] initWithString:@"AX" attributes:a];
    /* centred on the glyphs' own bounds, not the line's */
    NSRect b = [ax boundingRectWithSize:NSMakeSize(4096, 4096) options:NSStringDrawingUsesDeviceMetrics];
    [ax drawWithRect:NSMakeRect(512 - NSMidX(b), 512 - NSMidY(b), 0, 0) options:0];

    [NSGraphicsContext restoreGraphicsState];
    return rep;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 2) {
            fprintf(stderr, "usage: icon <dir>.iconset\n");
            return 2;
        }
        NSString *dir = @(argv[1]);
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        for (int pt = 16; pt <= 512; pt *= 2) {
            if (pt == 64)
                continue;
            for (int scale = 1; scale <= 2; scale++) {
                NSString *name = scale == 1 ? [NSString stringWithFormat:@"icon_%dx%d.png", pt, pt]
                                            : [NSString stringWithFormat:@"icon_%dx%d@2x.png", pt, pt];
                NSData *png = [draw(pt * scale) representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                if (![png writeToFile:[dir stringByAppendingPathComponent:name] atomically:YES]) {
                    fprintf(stderr, "icon: cannot write %s\n", name.UTF8String);
                    return 1;
                }
            }
        }
    }
    return 0;
}

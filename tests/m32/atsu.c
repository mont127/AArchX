/* ATSUI as Left 4 Dead 2's vgui uses it: a style with font, size and color, a layout drawing into a bitmap context,
 * the layout records read directly, glyph metrics at the records' stride, font metrics, and a font activated from a
 * buffer freed at once. The SDK no longer declares ATSUI's functions, so they are declared here. No host runs ATSUI
 * any more (macOS 14 refuses to start an x86_64 program that uses it), so atsu.expected was checked against native
 * CoreText instead: Helvetica's glyphs 43 and 76 with advances 17.33 and 5.33 at 24 points, its ascent, descent and
 * cap height, Arial's ascent. */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

OSStatus ATSUCreateStyle(ATSUStyle *);
OSStatus ATSUDisposeStyle(ATSUStyle);
OSStatus ATSUSetAttributes(ATSUStyle, ItemCount, const ATSUAttributeTag[], const ByteCount[], const ATSUAttributeValuePtr[]);
OSStatus ATSUCreateTextLayoutWithTextPtr(ConstUniCharArrayPtr, UniCharArrayOffset, UniCharCount, UniCharCount, ItemCount,
                                         const UniCharCount[], ATSUStyle[], ATSUTextLayout *);
OSStatus ATSUSetLayoutControls(ATSUTextLayout, ItemCount, const ATSUAttributeTag[], const ByteCount[],
                               const ATSUAttributeValuePtr[]);
OSStatus ATSUSetTransientFontMatching(ATSUTextLayout, Boolean);
OSStatus ATSUDrawText(ATSUTextLayout, UniCharArrayOffset, UniCharCount, ATSUTextMeasurement, ATSUTextMeasurement);
OSStatus ATSUDisposeTextLayout(ATSUTextLayout);
OSStatus ATSUGlyphGetIdealMetrics(ATSUStyle, ItemCount, GlyphID[], ByteOffset, ATSGlyphIdealMetrics[]);
OSStatus ATSUGlyphGetScreenMetrics(ATSUStyle, ItemCount, GlyphID[], ByteOffset, Boolean, Boolean, ATSGlyphScreenMetrics[]);
OSStatus ATSUDirectGetLayoutDataArrayPtrFromTextLayout(ATSUTextLayout, UniCharArrayOffset, UInt32, void *[], ItemCount *);
OSStatus ATSUDirectReleaseLayoutDataArrayPtr(void *, UInt32, void *[]);
/* the SDK marks these unavailable; the libraries still export them */
static ATSFontRef (*FindFont)(CFStringRef, ATSOptionFlags);
OSStatus FontMetrics(ATSFontRef, ATSOptionFlags, ATSFontMetrics *) __asm("_ATSFontGetHorizontalMetrics");
OSStatus ActivateFont(LogicalAddress, ByteCount, ATSFontContext, ATSFontFormat, void *, ATSOptionFlags,
                      ATSFontContainerRef *) __asm("_ATSFontActivateFromMemory");

int main(void)
{
    FindFont = (ATSFontRef (*)(CFStringRef, ATSOptionFlags))dlsym(
        dlopen("/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices", RTLD_NOW), "ATSFontFindFromName");
    ATSFontRef font = FindFont(CFSTR("Helvetica"), kATSOptionFlagsDefault);
    ATSFontMetrics fm;
    OSStatus e = FontMetrics(font, kATSOptionFlagsDefault, &fm);
    printf("font %d metrics %d ascent %.3f descent %.3f capHeight %.3f\n", font != 0, (int)e, fm.ascent, fm.descent, fm.capHeight);

    ATSUStyle style;
    OSStatus made = ATSUCreateStyle(&style);
    ATSUFontID fid = font;   /* the same value since Mac OS X 10.5 */
    Fixed size = 24 << 16;
    Boolean bold = false;
    ATSURGBAlphaColor color = { 1, 1, 1, 1 };
    ATSUAttributeTag tags[] = { kATSUFontTag, kATSUSizeTag, kATSUQDBoldfaceTag, kATSURGBAlphaColorTag };
    ByteCount sizes[] = { sizeof fid, sizeof size, sizeof bold, sizeof color };
    ATSUAttributeValuePtr values[] = { &fid, &size, &bold, &color };
    OSStatus attrs = ATSUSetAttributes(style, 4, tags, sizes, values);

    static UniChar text[] = { 'H', 'i' };
    UniCharCount run = kATSUToTextEnd;
    ATSUTextLayout layout;
    OSStatus laid = ATSUCreateTextLayoutWithTextPtr(text, kATSUFromTextBeginning, kATSUToTextEnd, 2, 1, &run, &style, &layout);
    ATSUSetTransientFontMatching(layout, true);
    static uint8_t pixels[64 * 32 * 4];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef cg = CGBitmapContextCreate(pixels, 64, 32, 8, 64 * 4, cs, kCGImageAlphaPremultipliedLast);
    Fixed width = 64 << 16;
    ATSUAttributeTag ltags[] = { kATSUCGContextTag, kATSULineWidthTag };
    ByteCount lsizes[] = { sizeof cg, sizeof width };
    ATSUAttributeValuePtr lvalues[] = { &cg, &width };
    OSStatus controls = ATSUSetLayoutControls(layout, 2, ltags, lsizes, lvalues);
    printf("style %d attributes %d layout %d controls %d\n", (int)made, (int)attrs, (int)laid, (int)controls);

    ATSLayoutRecord *rec = NULL;
    ItemCount n = 0;
    e = ATSUDirectGetLayoutDataArrayPtrFromTextLayout(layout, 0, 100, (void **)&rec, &n);
    printf("records %d count %lu", (int)e, (unsigned long)n);
    for (ItemCount i = 0; i < n; i++)
        printf(" [glyph %u at %.2f]", rec[i].glyphID, rec[i].realPos / 65536.0);
    printf("\n");
    ATSGlyphIdealMetrics ideal[4];
    ATSGlyphScreenMetrics screen[4];
    e = ATSUGlyphGetIdealMetrics(style, 2, &rec[0].glyphID, sizeof *rec, ideal);
    e |= ATSUGlyphGetScreenMetrics(style, 2, &rec[0].glyphID, sizeof *rec, true, true, screen);
    printf("metrics %d ideal advance %.2f %.2f screen width %u %u height %u advance %.2f\n", (int)e, ideal[0].advance.x,
           ideal[1].advance.x, (unsigned)screen[0].width, (unsigned)screen[1].width, (unsigned)screen[0].height,
           screen[0].deviceAdvance.x);
    ATSUDirectReleaseLayoutDataArrayPtr(NULL, 100, (void **)&rec);
    printf("released %d\n", rec == NULL);

    e = ATSUDrawText(layout, kATSUFromTextBeginning, kATSUToTextEnd, 2 << 16, 8 << 16);
    int painted = 0;
    for (int i = 0; i < 64 * 32; i++)
        painted += pixels[4 * i + 3] != 0;
    printf("drawn %d painted %d\n", (int)e, painted > 20);
    ATSUDisposeTextLayout(layout);
    ATSUDisposeStyle(style);

    FILE *f = fopen("/System/Library/Fonts/Supplemental/Arial.ttf", "rb");
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    void *data = malloc((size_t)len);
    fseek(f, 0, SEEK_SET);
    fread(data, 1, (size_t)len, f);
    fclose(f);
    ATSFontContainerRef container = 0;
    e = ActivateFont(data, (ByteCount)len, kATSFontContextLocal, kATSFontFormatUnspecified, NULL,
                                  kATSOptionFlagsDefault, &container);
    free(data);
    ATSFontRef arial = FindFont(CFSTR("Arial"), kATSOptionFlagsDefault);
    e |= FontMetrics(arial, kATSOptionFlagsDefault, &fm);
    printf("activated %d container %d ascent %.3f\n", (int)e, container != 0, fm.ascent);
    return 0;
}

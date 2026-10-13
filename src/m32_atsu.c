/*
 * ATS fonts and ATSUI text for i386 guests (include/ocerz/m32.h), on CoreText.  Left 4 Dead 2's vgui finds and
 * activates its fonts, lays out and draws each glyph into a bitmap context and measures glyphs with them.  On today's
 * macOS both are shells: ATSUI rejects every font (kATSUInvalidFontErr) and ATS traps on purpose in
 * Crash__Please_File_a_Radar_against_ATS..., and the SDK declares none of ATSUI's functions any more.  These specials
 * keep the calls' contracts (Apple's documented signatures, the i386 structure layouts measured with clang -arch i386)
 * and do the work with CoreText:
 *
 *   fonts     activated from memory or a file with CTFontManagerRegisterGraphicsFont, found by PostScript, full or
 *             family name; an ATSFontRef (and ATSUFontID, the same value) is 0x10000 + an index into g_fonts
 *   metrics   ATSFontMetrics in em units, as ATS gave them
 *   styles    font, size, bold, italic, underline and RGBA color; layouts keep the guest's text, runs, CGContext
 *   drawing   ATSUDrawText draws the line with its baseline at (x, y) in the layout's CGContext
 *   records   ATSLayoutRecords (14 bytes on i386, packed to 2) for the glyphs and a terminator, in guest memory
 *   glyphs    ideal and screen metrics from CoreText's advances and bounding boxes
 *
 * ponytail: the ATSUI covered is what Source's COSXFont uses; other attribute tags are accepted and ignored, other
 * direct-access selectors refused.  CoreText's layout is not ATSUI's to the pixel.
 */
#include <CoreText/CoreText.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ocerz/bridge.h"
#include "ocerz/interp.h"
#include "ocerz/m32.h"

#define RET(v) do { m32_ret(cpu, (uint32_t)(v), 0, 0); return OCERZ_STEP_OK; } while (0)
#define A(i) m32_arg(cpu, i)

enum { NO_ERR = 0, PARAM_ERR = -50, FNF_ERR = -43, INVALID_FONT = -8796, INVALID_LAYOUT = -8790, FONT_BASE = 0x10000 };

/* ---- the host functions, looked up once (ocerz links no frameworks) ---- */

#pragma clang diagnostic ignored "-Wdeprecated-declarations"   /* CTFontManagerRegisterGraphicsFont: data, not a URL */
#define HOST(fn) static __typeof__(fn) *fn##_
HOST(CFStringCreateWithCharacters); HOST(CFStringCompare); HOST(CFRelease); HOST(CFDataCreate);
HOST(CFAttributedStringCreateMutable); HOST(CFAttributedStringReplaceString); HOST(CFAttributedStringSetAttribute);
HOST(CFArrayGetCount); HOST(CFArrayGetValueAtIndex); HOST(CFNumberCreate);
HOST(CGDataProviderCreateWithCFData); HOST(CGDataProviderRelease); HOST(CGFontCreateWithDataProvider);
HOST(CGFontCopyPostScriptName); HOST(CGFontRelease); HOST(CGColorCreateSRGB); HOST(CGColorRelease);
HOST(CGContextSaveGState); HOST(CGContextRestoreGState); HOST(CGContextSetTextMatrix); HOST(CGContextSetTextPosition);
HOST(CTFontManagerRegisterGraphicsFont); HOST(CTFontCreateWithName); HOST(CTFontCreateCopyWithSymbolicTraits);
HOST(CTFontCopyPostScriptName); HOST(CTFontCopyFullName); HOST(CTFontCopyFamilyName);
HOST(CTFontGetAscent); HOST(CTFontGetDescent); HOST(CTFontGetLeading); HOST(CTFontGetCapHeight);
HOST(CTFontGetXHeight); HOST(CTFontGetSlantAngle); HOST(CTFontGetUnderlinePosition);
HOST(CTFontGetUnderlineThickness); HOST(CTFontGetBoundingBox); HOST(CTFontGetGlyphsForCharacters);
HOST(CTFontGetAdvancesForGlyphs); HOST(CTFontGetBoundingRectsForGlyphs);
HOST(CTLineCreateWithAttributedString); HOST(CTLineDraw); HOST(CTLineGetGlyphRuns); HOST(CTLineGetTypographicBounds);
HOST(CTRunGetGlyphCount); HOST(CTRunGetGlyphs); HOST(CTRunGetPositions); HOST(CTRunGetStringIndices);
static CFStringRef k_font, k_color, k_underline;

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_ok;
static void load(void)
{
    void *ct = dlopen("/System/Library/Frameworks/CoreText.framework/CoreText", RTLD_LAZY | RTLD_LOCAL);
    void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY | RTLD_LOCAL);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY | RTLD_LOCAL);
    int ok = ct && cg && cf;
#define L(h, fn) ok &= (fn##_ = (__typeof__(fn) *)dlsym(h, #fn)) != NULL
    L(cf, CFStringCreateWithCharacters); L(cf, CFStringCompare); L(cf, CFRelease); L(cf, CFDataCreate);
    L(cf, CFAttributedStringCreateMutable); L(cf, CFAttributedStringReplaceString); L(cf, CFAttributedStringSetAttribute);
    L(cf, CFArrayGetCount); L(cf, CFArrayGetValueAtIndex); L(cf, CFNumberCreate);
    L(cg, CGDataProviderCreateWithCFData); L(cg, CGDataProviderRelease); L(cg, CGFontCreateWithDataProvider);
    L(cg, CGFontCopyPostScriptName); L(cg, CGFontRelease); L(cg, CGColorCreateSRGB); L(cg, CGColorRelease);
    L(cg, CGContextSaveGState); L(cg, CGContextRestoreGState); L(cg, CGContextSetTextMatrix);
    L(cg, CGContextSetTextPosition);
    L(ct, CTFontManagerRegisterGraphicsFont); L(ct, CTFontCreateWithName); L(ct, CTFontCreateCopyWithSymbolicTraits);
    L(ct, CTFontCopyPostScriptName); L(ct, CTFontCopyFullName); L(ct, CTFontCopyFamilyName);
    L(ct, CTFontGetAscent); L(ct, CTFontGetDescent); L(ct, CTFontGetLeading); L(ct, CTFontGetCapHeight);
    L(ct, CTFontGetXHeight); L(ct, CTFontGetSlantAngle); L(ct, CTFontGetUnderlinePosition);
    L(ct, CTFontGetUnderlineThickness); L(ct, CTFontGetBoundingBox); L(ct, CTFontGetGlyphsForCharacters);
    L(ct, CTFontGetAdvancesForGlyphs); L(ct, CTFontGetBoundingRectsForGlyphs);
    L(ct, CTLineCreateWithAttributedString); L(ct, CTLineDraw); L(ct, CTLineGetGlyphRuns);
    L(ct, CTLineGetTypographicBounds); L(ct, CTRunGetGlyphCount); L(ct, CTRunGetGlyphs); L(ct, CTRunGetPositions);
    L(ct, CTRunGetStringIndices);
#undef L
    CFStringRef *f = ct ? dlsym(ct, "kCTFontAttributeName") : NULL, *c = ct ? dlsym(ct, "kCTForegroundColorAttributeName") : NULL,
                *u = ct ? dlsym(ct, "kCTUnderlineStyleAttributeName") : NULL;
    ok &= f && c && u;
    if (ok) {
        k_font = *f;
        k_color = *c;
        k_underline = *u;
    }
    g_ok = ok;
    if (!ok)
        fprintf(stderr, "ocerz: m32: CoreText is missing a function the ATSUI stand-in needs\n");
}
static int ready(void)
{
    pthread_once(&g_once, load);
    return g_ok;
}
static void release(const void *cf)
{
    if (cf)
        CFRelease_(cf);
}

/* ---- fonts: PostScript names, by ATSFontRef ---- */

static CFStringRef g_fonts[512];
static int g_nfonts;
static pthread_mutex_t g_font_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t font_id(CFStringRef ps)   /* takes ps */
{
    if (!ps)
        return 0;
    pthread_mutex_lock(&g_font_lock);
    int i = 0;
    while (i < g_nfonts && CFStringCompare_(g_fonts[i], ps, 0) != kCFCompareEqualTo)
        i++;
    if (i == g_nfonts && g_nfonts < 512)
        g_fonts[g_nfonts++] = ps;
    else
        release(ps);
    pthread_mutex_unlock(&g_font_lock);
    return i < 512 ? FONT_BASE + (uint32_t)i : 0;
}
static CTFontRef font_at(uint32_t id, double size)
{
    CFStringRef ps = NULL;
    pthread_mutex_lock(&g_font_lock);
    if (id >= FONT_BASE && id - FONT_BASE < (uint32_t)g_nfonts)
        ps = g_fonts[id - FONT_BASE];
    pthread_mutex_unlock(&g_font_lock);
    return ps ? CTFontCreateWithName_(ps, size, NULL) : NULL;
}
static int same(CFStringRef a, CFStringRef b)
{
    int r = a && b && CFStringCompare_(a, b, kCFCompareCaseInsensitive) == kCFCompareEqualTo;
    release(a);
    return r;
}
/* CoreText falls back to another font for a name it does not know: the font it made must carry the name */
static uint32_t find(CFStringRef name, int postscript_only)
{
    if (!name || !ready())
        return 0;
    CTFontRef f = CTFontCreateWithName_(name, 12, NULL);
    if (!f)
        return 0;
    int hit = same(CTFontCopyPostScriptName_(f), name) ||
              (!postscript_only && (same(CTFontCopyFullName_(f), name) || same(CTFontCopyFamilyName_(f), name)));
    uint32_t id = hit ? font_id(CTFontCopyPostScriptName_(f)) : 0;
    release(f);
    return id;
}
static int sp_ATSFontFindFromName(struct OcerzVM *vm, OcerzCPU *cpu) { RET(find(m32_host(A(0)), 0)); }
static int sp_ATSFontFindFromPostScriptName(struct OcerzVM *vm, OcerzCPU *cpu) { RET(find(m32_host(A(0)), 1)); }

/* activation: the font's bytes are copied (a game frees its buffer at once) and registered for this process; the
 * container is the font's reference */
static int32_t activate(const void *data, uint32_t len, uint32_t gout)
{
    if (!ready())
        return PARAM_ERR;
    CFDataRef bytes = CFDataCreate_(NULL, data, len);
    CGDataProviderRef p = bytes ? CGDataProviderCreateWithCFData_(bytes) : NULL;
    CGFontRef font = p ? CGFontCreateWithDataProvider_(p) : NULL;
    release(bytes);
    if (p)
        CGDataProviderRelease_(p);
    if (!font)
        return PARAM_ERR;
    CTFontManagerRegisterGraphicsFont_(font, NULL);   /* fails harmlessly when it is registered already */
    uint32_t id = font_id(CGFontCopyPostScriptName_(font));
    CGFontRelease_(font);
    if (gout)
        m32_wr(gout, id);
    return id ? NO_ERR : PARAM_ERR;
}
/* ATSFontActivateFromMemory(data, length, context, format, reserved, options, ATSFontContainerRef *) */
static int sp_ATSFontActivateFromMemory(struct OcerzVM *vm, OcerzCPU *cpu)
{
    RET(A(0) ? activate(m32_h(A(0)), A(1), A(6)) : PARAM_ERR);
}
/* ATSFontActivateFromFileReference(const FSRef *, context, format, refCon, options, ATSFontContainerRef *) */
static int sp_ATSFontActivateFromFileReference(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int32_t (*path_of)(const void *, char *, uint32_t) = (int32_t (*)(const void *, char *, uint32_t))
        ocerz_bridge_host_symbol("/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices", "FSRefMakePath");
    char path[1024];
    if (!A(0) || !path_of || path_of(m32_h(A(0)), path, sizeof path) != 0)
        RET(FNF_ERR);
    int fd = open(path, O_RDONLY);
    off_t len = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    void *data = len > 0 && len < (1 << 30) ? malloc((size_t)len) : NULL;
    int ok = data && pread(fd, data, (size_t)len, 0) == len;
    if (fd >= 0)
        close(fd);
    int32_t r = ok ? activate(data, (uint32_t)len, A(5)) : FNF_ERR;
    free(data);
    RET(r);
}

static void wrf(uint32_t g, double d) { float f = (float)d; memcpy(m32_h(g), &f, 4); }

/* ATSFontGetHorizontalMetrics / ATSFontGetVerticalMetrics(font, options, ATSFontMetrics *): i386 {UInt32 version,
 * ascent, descent, leading, avgAdvanceWidth, maxAdvanceWidth, minLeftSideBearing, minRightSideBearing, stemWidth,
 * stemHeight, capHeight, xHeight, italicAngle, underlinePosition, underlineThickness}, floats in em units */
static int font_metrics(OcerzCPU *cpu)
{
    uint32_t out = A(2);
    CTFontRef f = ready() ? font_at(A(0), 1.0) : NULL;
    if (!f)
        RET(INVALID_FONT);
    CGRect box = CTFontGetBoundingBox_(f);
    UniChar x = 'x';
    CGGlyph g = 0;
    CGSize adv = { 0, 0 };
    if (CTFontGetGlyphsForCharacters_(f, &x, &g, 1))
        CTFontGetAdvancesForGlyphs_(f, kCTFontOrientationHorizontal, &g, &adv, 1);
    double v[14] = { CTFontGetAscent_(f), -CTFontGetDescent_(f), CTFontGetLeading_(f), adv.width, box.size.width,
                     box.origin.x, 0, 0, 0, CTFontGetCapHeight_(f), CTFontGetXHeight_(f), CTFontGetSlantAngle_(f),
                     CTFontGetUnderlinePosition_(f), CTFontGetUnderlineThickness_(f) };
    release(f);
    if (out) {
        m32_wr(out, 0);
        for (int i = 0; i < 14; i++)
            wrf(out + 4 + 4 * (uint32_t)i, v[i]);
    }
    RET(NO_ERR);
}
static int sp_ATSFontGetHorizontalMetrics(struct OcerzVM *vm, OcerzCPU *cpu) { return font_metrics(cpu); }
static int sp_ATSFontGetVerticalMetrics(struct OcerzVM *vm, OcerzCPU *cpu) { return font_metrics(cpu); }

/* ---- styles ---- */

typedef struct AtsuStyle {
    uint32_t font;
    double size;
    int bold, italic, underline, colored;
    float rgba[4];
} AtsuStyle;

static CTFontRef style_font(const AtsuStyle *s)
{
    CTFontRef f = font_at(s->font, s->size);
    CTFontSymbolicTraits t = (s->bold ? kCTFontTraitBold : 0) | (s->italic ? kCTFontTraitItalic : 0);
    CTFontRef styled = f && t ? CTFontCreateCopyWithSymbolicTraits_(f, s->size, NULL, t, t) : NULL;
    if (styled) {
        release(f);
        f = styled;
    }
    return f;
}
static int sp_ATSUCreateStyle(struct OcerzVM *vm, OcerzCPU *cpu)   /* (ATSUStyle *) */
{
    AtsuStyle *s = calloc(1, sizeof *s);
    if (!s || !ready())
        RET(PARAM_ERR);
    s->size = 12;
    if (A(0))
        m32_wr(A(0), m32_handle(s));
    RET(NO_ERR);
}
static int sp_ATSUDisposeStyle(struct OcerzVM *vm, OcerzCPU *cpu)
{
    free(m32_host(A(0)));
    RET(NO_ERR);
}
/* ATSUSetAttributes(style, count, tags[], sizes[], values[]): the 4-byte arrays and values read in guest memory */
static int sp_ATSUSetAttributes(struct OcerzVM *vm, OcerzCPU *cpu)
{
    AtsuStyle *s = m32_host(A(0));
    uint32_t n = A(1), tags = A(2), sizes = A(3), values = A(4);
    if (!s || n > 4096)
        RET(PARAM_ERR);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t tag = m32_rd(tags + 4 * i), size = m32_rd(sizes + 4 * i), v = m32_rd(values + 4 * i);
        if (!v)
            continue;
        uint8_t b = *(uint8_t *)m32_h(v);
        switch (tag) {
        case 256: s->bold = b != 0; break;                                    /* kATSUQDBoldfaceTag */
        case 257: s->italic = b != 0; break;                                  /* kATSUQDItalicTag */
        case 258: s->underline = b != 0; break;                               /* kATSUQDUnderlineTag */
        case 261: {                                                           /* kATSUFontTag */
            CTFontRef known = font_at(m32_rd(v), 1);
            if (!known)
                RET(INVALID_FONT);
            release(known);
            s->font = m32_rd(v);
            break;
        }
        case 262: s->size = (int32_t)m32_rd(v) / 65536.0; break;            /* kATSUSizeTag, Fixed */
        case 288:                                                             /* kATSURGBAlphaColorTag */
            if (size == 16) { memcpy(s->rgba, m32_h(v), 16); s->colored = 1; }
            break;
        default: break;   /* rendering options and the rest: CoreText's own */
        }
    }
    RET(NO_ERR);
}

/* ---- layouts ---- */

typedef struct AtsuLayout {
    uint32_t text, offset, length, runs;
    uint32_t *run_len;
    AtsuStyle **run_style;
    CGContextRef ctx;
} AtsuLayout;

static uint32_t range_start(uint32_t o) { return o == 0xffffffffu ? 0 : o; }   /* kATSUFromTextBeginning */

/* ATSUCreateTextLayoutWithTextPtr(text, offset, length, totalLength, runs, runLengths[], styles[], ATSUTextLayout *):
 * the layout keeps pointing at the guest's text */
static int sp_ATSUCreateTextLayoutWithTextPtr(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t total = A(3), runs = A(4), lens = A(5), styles = A(6), out = A(7);
    AtsuLayout *l = calloc(1, sizeof *l);
    if (!l || runs > 65536 || !ready())
        RET(PARAM_ERR);
    l->text = A(0);
    l->offset = range_start(A(1));
    l->length = A(2) == 0xffffffffu ? total - l->offset : A(2);   /* kATSUToTextEnd */
    l->runs = runs;
    l->run_len = calloc((size_t)runs + 1, sizeof *l->run_len);
    l->run_style = calloc((size_t)runs + 1, sizeof *l->run_style);
    for (uint32_t i = 0; i < runs; i++) {
        l->run_len[i] = lens ? m32_rd(lens + 4 * i) : 0xffffffffu;
        l->run_style[i] = styles ? m32_host(m32_rd(styles + 4 * i)) : NULL;
    }
    if (out)
        m32_wr(out, m32_handle(l));
    RET(NO_ERR);
}
static int sp_ATSUDisposeTextLayout(struct OcerzVM *vm, OcerzCPU *cpu)
{
    AtsuLayout *l = m32_host(A(0));
    if (l) {
        free(l->run_len);
        free(l->run_style);
        free(l);
    }
    RET(NO_ERR);
}
/* ATSUSetLayoutControls(layout, count, tags[], sizes[], values[]): the CGContext (kATSUCGContextTag) is kept, the line
 * width, flush factor and the rest are CoreText's business */
static int sp_ATSUSetLayoutControls(struct OcerzVM *vm, OcerzCPU *cpu)
{
    AtsuLayout *l = m32_host(A(0));
    uint32_t n = A(1), tags = A(2), values = A(4);
    if (!l || n > 4096)
        RET(PARAM_ERR);
    for (uint32_t i = 0; i < n; i++)
        if (m32_rd(tags + 4 * i) == 32767 && m32_rd(values + 4 * i))
            l->ctx = m32_host(m32_rd(m32_rd(values + 4 * i)));
    RET(NO_ERR);
}
static int sp_ATSUSetTransientFontMatching(struct OcerzVM *vm, OcerzCPU *cpu) { RET(m32_host(A(0)) ? NO_ERR : INVALID_LAYOUT); }

/* the CTLine for [from, from + count) of the layout's text, each run's style on its part */
static CTLineRef make_line(const AtsuLayout *l, uint32_t from, uint32_t count)
{
    if (!l->text || !count)
        return NULL;
    CFStringRef str = CFStringCreateWithCharacters_(NULL, (const UniChar *)m32_h(l->text + 2 * from), count);
    CFMutableAttributedStringRef as = CFAttributedStringCreateMutable_(NULL, 0);
    CFAttributedStringReplaceString_(as, CFRangeMake(0, 0), str);
    release(str);
    uint32_t at = l->offset;
    for (uint32_t r = 0; r < l->runs; r++) {
        uint32_t len = l->run_len[r] == 0xffffffffu ? l->offset + l->length - at : l->run_len[r];
        uint32_t lo = at > from ? at : from, hi = at + len < from + count ? at + len : from + count;
        at += len;
        const AtsuStyle *s = l->run_style[r];
        if (!s || lo >= hi)
            continue;
        CFRange range = CFRangeMake(lo - from, hi - lo);
        CTFontRef f = style_font(s);
        if (f)
            CFAttributedStringSetAttribute_(as, range, k_font, f);
        release(f);
        if (s->colored) {
            CGColorRef c = CGColorCreateSRGB_(s->rgba[0], s->rgba[1], s->rgba[2], s->rgba[3]);
            CFAttributedStringSetAttribute_(as, range, k_color, c);
            CGColorRelease_(c);
        }
        if (s->underline) {
            int32_t single = kCTUnderlineStyleSingle;
            CFNumberRef u = CFNumberCreate_(NULL, kCFNumberSInt32Type, &single);
            CFAttributedStringSetAttribute_(as, range, k_underline, u);
            release(u);
        }
    }
    CTLineRef line = CTLineCreateWithAttributedString_(as);
    release(as);
    return line;
}

/* ATSUDrawText(layout, lineOffset, lineLength, Fixed x, Fixed y): the baseline at (x, y) in the layout's CGContext */
static int sp_ATSUDrawText(struct OcerzVM *vm, OcerzCPU *cpu)
{
    AtsuLayout *l = m32_host(A(0));
    if (!l || !l->ctx)
        RET(INVALID_LAYOUT);
    uint32_t from = A(1) == 0xffffffffu ? l->offset : A(1), count = A(2) == 0xffffffffu ? l->offset + l->length - from : A(2);
    CTLineRef line = make_line(l, from, count);
    if (line) {
        CGContextSaveGState_(l->ctx);
        CGContextSetTextMatrix_(l->ctx, (CGAffineTransform){ 1, 0, 0, 1, 0, 0 });
        CGContextSetTextPosition_(l->ctx, (int32_t)A(3) / 65536.0, (int32_t)A(4) / 65536.0);
        CTLineDraw_(line, l->ctx);
        CGContextRestoreGState_(l->ctx);
        release(line);
    }
    RET(NO_ERR);
}

/* ---- direct access: the line's ATSLayoutRecords, i386 {UInt16 glyphID, UInt32 flags, UInt32 originalOffset (bytes),
 * Fixed realPos}, 14 bytes; the last one the terminator at the line's width ---- */

static void put_record(uint32_t d, uint16_t glyph, uint32_t flags, uint32_t offset, double x)
{
    memcpy(m32_h(d), &glyph, 2);
    memcpy(m32_h(d + 2), &flags, 4);
    memcpy(m32_h(d + 6), &offset, 4);
    int32_t pos = (int32_t)lround(x * 65536.0);
    memcpy(m32_h(d + 10), &pos, 4);
}
/* ATSUDirectGetLayoutDataArrayPtrFromTextLayout(layout, lineOffset, selector, void **array, ItemCount *count) */
static int sp_ATSUDirectGetLayoutDataArrayPtrFromTextLayout(struct OcerzVM *vm, OcerzCPU *cpu)
{
    AtsuLayout *l = m32_host(A(0));
    uint32_t gout = A(3), gcount = A(4);
    if (gout)
        m32_wr(gout, 0);
    if (gcount)
        m32_wr(gcount, 0);
    if (!l)
        RET(INVALID_LAYOUT);
    if (A(2) != 100) {   /* kATSUDirectDataLayoutRecordATSLayoutRecordCurrent */
        m32_log_once("an ATSUI direct-access selector the stand-in does not answer:", "ATSUDirectGetLayoutDataArrayPtrFromTextLayout");
        RET(PARAM_ERR);
    }
    CTLineRef line = make_line(l, l->offset, l->length);
    CFArrayRef runs = line ? CTLineGetGlyphRuns_(line) : NULL;
    long nruns = runs ? CFArrayGetCount_(runs) : 0, n = 0;
    for (long r = 0; r < nruns; r++)
        n += CTRunGetGlyphCount_(CFArrayGetValueAtIndex_(runs, r));
    uint32_t g = m32_malloc(14 * (uint32_t)(n + 1)), d = g;
    for (long r = 0; g && r < nruns; r++) {
        CTRunRef run = CFArrayGetValueAtIndex_(runs, r);
        long k = CTRunGetGlyphCount_(run);
        CGGlyph *glyphs = calloc((size_t)k + 1, sizeof *glyphs);
        CGPoint *pos = calloc((size_t)k + 1, sizeof *pos);
        CFIndex *idx = calloc((size_t)k + 1, sizeof *idx);
        CTRunGetGlyphs_(run, CFRangeMake(0, k), glyphs);
        CTRunGetPositions_(run, CFRangeMake(0, k), pos);
        CTRunGetStringIndices_(run, CFRangeMake(0, k), idx);
        for (long i = 0; i < k; i++, d += 14)
            put_record(d, glyphs[i], 0, 2 * (uint32_t)idx[i], pos[i].x);
        free(glyphs);
        free(pos);
        free(idx);
    }
    double width = line ? CTLineGetTypographicBounds_(line, NULL, NULL, NULL) : 0;
    if (g)
        put_record(d, 0xffff, 0x00080000, 2 * l->length, width);   /* kATSDeletedGlyphcode, kATSGlyphInfoTerminatorGlyph */
    release(line);
    if (gout)
        m32_wr(gout, g);
    if (gcount)
        m32_wr(gcount, g ? (uint32_t)n + 1 : 0);
    RET(g ? NO_ERR : PARAM_ERR);
}
/* ATSUDirectReleaseLayoutDataArrayPtr(lineRef, selector, void **array) */
static int sp_ATSUDirectReleaseLayoutDataArrayPtr(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t gp = A(2);
    if (gp && m32_rd(gp)) {
        m32_free(m32_rd(gp));
        m32_wr(gp, 0);
    }
    RET(NO_ERR);
}

/* ---- glyph metrics: the glyph IDs read in guest memory at the guest's stride ---- */

static int glyph_boxes(OcerzCPU *cpu, uint32_t n, CTFontRef *font, CGGlyph **glyphs, CGSize **adv, CGRect **box)
{
    const AtsuStyle *s = m32_host(A(0));
    uint32_t ids = A(2), stride = A(3);
    if (!s || n > (1u << 20) || !ready() || !(*font = style_font(s)))
        return 0;
    *glyphs = calloc((size_t)n + 1, sizeof **glyphs);
    *adv = calloc((size_t)n + 1, sizeof **adv);
    *box = calloc((size_t)n + 1, sizeof **box);
    for (uint32_t i = 0; i < n; i++) {
        uint16_t gl;
        memcpy(&gl, m32_h(ids + i * (stride ? stride : 2)), 2);
        (*glyphs)[i] = gl == 0xffff ? 0 : gl;
    }
    CTFontGetAdvancesForGlyphs_(*font, kCTFontOrientationHorizontal, *glyphs, *adv, n);
    CTFontGetBoundingRectsForGlyphs_(*font, kCTFontOrientationHorizontal, *glyphs, *box, n);
    return 1;
}
static void glyph_boxes_free(CTFontRef font, CGGlyph *glyphs, CGSize *adv, CGRect *box)
{
    release(font);
    free(glyphs);
    free(adv);
    free(box);
}
/* ATSUGlyphGetIdealMetrics(style, count, glyphIDs, inputOffset, ATSGlyphIdealMetrics[]): i386 {advance, sideBearing,
 * otherSideBearing} float points, 24 bytes */
static int sp_ATSUGlyphGetIdealMetrics(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t n = A(1), out = A(4);
    CTFontRef font;
    CGGlyph *glyphs;
    CGSize *adv;
    CGRect *box;
    if (!glyph_boxes(cpu, n, &font, &glyphs, &adv, &box))
        RET(PARAM_ERR);
    for (uint32_t i = 0; out && i < n; i++) {
        uint32_t d = out + 24 * i;
        double v[6] = { adv[i].width, 0, box[i].origin.x, 0, adv[i].width - (box[i].origin.x + box[i].size.width), 0 };
        for (int k = 0; k < 6; k++)
            wrf(d + 4 * (uint32_t)k, v[k]);
    }
    glyph_boxes_free(font, glyphs, adv, box);
    RET(NO_ERR);
}
/* ATSUGlyphGetScreenMetrics(style, count, glyphIDs, inputOffset, forcingAntiAlias, antiAliasSwitch,
 * ATSGlyphScreenMetrics[]): i386 {deviceAdvance, topLeft, UInt32 height, UInt32 width, sideBearing, otherSideBearing},
 * 40 bytes, in whole pixels */
static int sp_ATSUGlyphGetScreenMetrics(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t n = A(1), out = A(6);
    CTFontRef font;
    CGGlyph *glyphs;
    CGSize *adv;
    CGRect *box;
    if (!glyph_boxes(cpu, n, &font, &glyphs, &adv, &box))
        RET(PARAM_ERR);
    for (uint32_t i = 0; out && i < n; i++) {
        uint32_t d = out + 40 * i;
        const CGRect *b = &box[i];
        int empty = b->size.width <= 0 || b->size.height <= 0;
        double left = empty ? 0 : floor(b->origin.x), top = empty ? 0 : ceil(b->origin.y + b->size.height);
        uint32_t w = empty ? 0 : (uint32_t)(ceil(b->origin.x + b->size.width) - left);
        uint32_t h = empty ? 0 : (uint32_t)(top - floor(b->origin.y));
        double device = round(adv[i].width);
        wrf(d, device);
        wrf(d + 4, 0);
        wrf(d + 8, left);
        wrf(d + 12, top);
        m32_wr(d + 16, h);
        m32_wr(d + 20, w);
        wrf(d + 24, left);
        wrf(d + 28, 0);
        wrf(d + 32, device - (left + w));
        wrf(d + 36, 0);
    }
    glyph_boxes_free(font, glyphs, adv, box);
    RET(NO_ERR);
}

const M32SpecialEntry m32_atsu_specials[] = {
    { "_ATSFontFindFromName", sp_ATSFontFindFromName },
    { "_ATSFontFindFromPostScriptName", sp_ATSFontFindFromPostScriptName },
    { "_ATSFontActivateFromMemory", sp_ATSFontActivateFromMemory },
    { "_ATSFontActivateFromFileReference", sp_ATSFontActivateFromFileReference },
    { "_ATSFontGetHorizontalMetrics", sp_ATSFontGetHorizontalMetrics },
    { "_ATSFontGetVerticalMetrics", sp_ATSFontGetVerticalMetrics },
    { "_ATSUCreateStyle", sp_ATSUCreateStyle }, { "_ATSUDisposeStyle", sp_ATSUDisposeStyle },
    { "_ATSUSetAttributes", sp_ATSUSetAttributes },
    { "_ATSUCreateTextLayoutWithTextPtr", sp_ATSUCreateTextLayoutWithTextPtr },
    { "_ATSUDisposeTextLayout", sp_ATSUDisposeTextLayout },
    { "_ATSUSetLayoutControls", sp_ATSUSetLayoutControls },
    { "_ATSUSetTransientFontMatching", sp_ATSUSetTransientFontMatching }, { "_ATSUDrawText", sp_ATSUDrawText },
    { "_ATSUDirectGetLayoutDataArrayPtrFromTextLayout", sp_ATSUDirectGetLayoutDataArrayPtrFromTextLayout },
    { "_ATSUDirectReleaseLayoutDataArrayPtr", sp_ATSUDirectReleaseLayoutDataArrayPtr },
    { "_ATSUGlyphGetIdealMetrics", sp_ATSUGlyphGetIdealMetrics },
    { "_ATSUGlyphGetScreenMetrics", sp_ATSUGlyphGetScreenMetrics },
    { NULL, NULL }
};

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
 *   families  QuickDraw's and the Font Manager's font families: 0 and 1, the system and application fonts, are
 *             Lucida Grande, the classic IDs (Geneva 3, Monaco 4, Times 20, Helvetica 21, Courier 22 and
 *             the rest) their families, and any other family a program names gets an ID from 1024; an FMFont is the
 *             ATSFontRef above.  Theme fonts are the system font at Aqua's sizes.
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
HOST(CFStringCreateWithPascalString); HOST(CFStringGetPascalString); HOST(CTFontCreateUIFontForLanguage);
HOST(CTFontGetSymbolicTraits); HOST(CFStringCreateWithCString); HOST(CTFontCopyTable); HOST(CFDataGetLength);
HOST(CFDataGetBytePtr);
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
    L(cf, CFStringCreateWithPascalString); L(cf, CFStringGetPascalString); L(ct, CTFontCreateUIFontForLanguage);
    L(ct, CTFontGetSymbolicTraits); L(cf, CFStringCreateWithCString); L(ct, CTFontCopyTable);
    L(cf, CFDataGetLength); L(cf, CFDataGetBytePtr);
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

/* ---- font families: QuickDraw's and the Font Manager's IDs ---- */

enum { FAMILY_BASE = 1024, INVALID_FAMILY = -1, FM_INVALID_FAMILY_ERR = -985, FM_INVALID_FONT_ERR = -986 };
static const char *const g_classic[] = { [2] = "New York", [3] = "Geneva", [4] = "Monaco", [20] = "Times",
                                         [21] = "Helvetica", [22] = "Courier", [23] = "Symbol" };
static CFStringRef g_families[256];
static int g_nfamilies;

/* Lucida Grande, the system font of the Macs these programs were written for and still installed; the UI font's
 * hidden family (.AppleSystemUIFont), which CoreText warns about when it is asked for by name, only without it */
static CFStringRef system_family(void)
{
    static CFStringRef name;
    if (!name && ready()) {
        CFStringRef lucida = CFStringCreateWithCString_(NULL, "Lucida Grande", kCFStringEncodingMacRoman);
        CTFontRef f = CTFontCreateWithName_(lucida, 13, NULL);
        CFStringRef fam = f ? CTFontCopyFamilyName_(f) : NULL;
        release(f);
        if (fam && CFStringCompare_(fam, lucida, 0) == kCFCompareEqualTo) {
            name = fam;
        } else {
            release(fam);
            CTFontRef ui = CTFontCreateUIFontForLanguage_(kCTFontUIFontSystem, 13, NULL);
            name = ui ? CTFontCopyFamilyName_(ui) : NULL;
            release(ui);
        }
        release(lucida);
    }
    return name;
}

/* the family's name, or NULL; not retained */
static CFStringRef family_name(int id)
{
    static CFStringRef classic[sizeof g_classic / sizeof g_classic[0]];
    if (id == 0 || id == 1)
        return system_family();
    if (id > 1 && id < (int)(sizeof g_classic / sizeof g_classic[0]) && g_classic[id]) {
        if (!classic[id])
            classic[id] = CFStringCreateWithCString_(NULL, g_classic[id], kCFStringEncodingMacRoman);
        return classic[id];
    }
    CFStringRef n = NULL;
    pthread_mutex_lock(&g_font_lock);
    if (id >= FAMILY_BASE && id - FAMILY_BASE < g_nfamilies)
        n = g_families[id - FAMILY_BASE];
    pthread_mutex_unlock(&g_font_lock);
    return n;
}

/* a family the system has: its ID, given out once; INVALID_FAMILY otherwise */
static int family_id(CFStringRef name)
{
    if (!name || !ready())
        return INVALID_FAMILY;
    CFStringRef sys = system_family();
    if (sys && CFStringCompare_(name, sys, kCFCompareCaseInsensitive) == kCFCompareEqualTo)
        return 0;
    for (int i = 2; i < (int)(sizeof g_classic / sizeof g_classic[0]); i++) {
        CFStringRef c = g_classic[i] ? family_name(i) : NULL;
        if (c && CFStringCompare_(name, c, kCFCompareCaseInsensitive) == kCFCompareEqualTo)
            return i;
    }
    pthread_mutex_lock(&g_font_lock);
    for (int i = 0; i < g_nfamilies; i++)
        if (CFStringCompare_(name, g_families[i], kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
            pthread_mutex_unlock(&g_font_lock);
            return FAMILY_BASE + i;
        }
    pthread_mutex_unlock(&g_font_lock);
    CTFontRef f = CTFontCreateWithName_(name, 12, NULL);
    CFStringRef real = f ? CTFontCopyFamilyName_(f) : NULL;
    release(f);
    if (!real || CFStringCompare_(real, name, kCFCompareCaseInsensitive) != kCFCompareEqualTo) {
        release(real);
        return INVALID_FAMILY;
    }
    pthread_mutex_lock(&g_font_lock);
    int id = INVALID_FAMILY;
    if (g_nfamilies < 256) {
        g_families[g_nfamilies] = real;
        id = FAMILY_BASE + g_nfamilies++;
    } else {
        release(real);
    }
    pthread_mutex_unlock(&g_font_lock);
    return id;
}

static CFStringRef pascal_in(uint32_t g)
{
    return g && ready() ? CFStringCreateWithPascalString_(NULL, m32_h(g), kCFStringEncodingMacRoman) : NULL;
}
static void pascal_out(CFStringRef s, uint32_t g)
{
    if (!g || !ready())
        return;
    *(uint8_t *)m32_h(g) = 0;
    if (s)
        CFStringGetPascalString_(s, m32_h(g), 256, kCFStringEncodingMacRoman);
}

/* GetFNum(ConstStr255Param name, short *familyID): 0 for a name the system has no family of */
static int sp_GetFNum(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    CFStringRef n = pascal_in(A(0));
    int id = family_id(n);
    release(n);
    if (A(1))
        *(int16_t *)m32_h(A(1)) = (int16_t)(id < 0 ? 0 : id);
    RET(0);
}
static int sp_FMGetFontFamilyFromName(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    CFStringRef n = pascal_in(A(0));
    int id = family_id(n);
    release(n);
    RET((uint32_t)(int32_t)id);
}
static int sp_FMGetFontFamilyName(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    CFStringRef n = ready() ? family_name((int16_t)A(0)) : NULL;
    pascal_out(n, A(1));
    RET(n ? NO_ERR : (uint32_t)FM_INVALID_FAMILY_ERR);
}
static int sp_GetFontName(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    pascal_out(ready() ? family_name((int16_t)A(0)) : NULL, A(1));
    RET(0);
}

/* FMGetFontFromFontFamilyInstance(FMFontFamily, FMFontStyle, FMFont *, FMFontStyle *intrinsic): the family's font in
 * the style (bold 1, italic 2) CoreText has, and the style it has of its own */
static int sp_FMGetFontFromFontFamilyInstance(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    CFStringRef n = ready() ? family_name((int16_t)A(0)) : NULL;
    CTFontRef f = n ? CTFontCreateWithName_(n, 12, NULL) : NULL;
    if (!f)
        RET((uint32_t)FM_INVALID_FAMILY_ERR);
    int style = (int16_t)A(1);
    CTFontSymbolicTraits want = (style & 1 ? kCTFontTraitBold : 0) | (style & 2 ? kCTFontTraitItalic : 0);
    CTFontRef styled = want ? CTFontCreateCopyWithSymbolicTraits_(f, 12, NULL, want, want) : NULL;
    if (styled) {
        release(f);
        f = styled;
    }
    CTFontSymbolicTraits has = CTFontGetSymbolicTraits_(f);
    uint32_t id = font_id(CTFontCopyPostScriptName_(f));
    release(f);
    if (A(2))
        m32_wr(A(2), id);
    if (A(3))
        *(int16_t *)m32_h(A(3)) = (int16_t)((has & kCTFontTraitBold ? 1 : 0) | (has & kCTFontTraitItalic ? 2 : 0));
    RET(id ? NO_ERR : (uint32_t)FM_INVALID_FONT_ERR);
}

/* FMGetFontFamilyInstanceFromFont(FMFont, FMFontFamily *, FMFontStyle *) */
static int sp_FMGetFontFamilyInstanceFromFont(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    CTFontRef f = ready() ? font_at(A(0), 12) : NULL;
    if (!f)
        RET((uint32_t)FM_INVALID_FONT_ERR);
    CFStringRef fam = CTFontCopyFamilyName_(f);
    CTFontSymbolicTraits has = CTFontGetSymbolicTraits_(f);
    int id = family_id(fam);
    release(fam);
    release(f);
    if (A(1))
        *(int16_t *)m32_h(A(1)) = (int16_t)id;
    if (A(2))
        *(int16_t *)m32_h(A(2)) = (int16_t)((has & kCTFontTraitBold ? 1 : 0) | (has & kCTFontTraitItalic ? 2 : 0));
    RET(NO_ERR);
}

/* an ATSFontFamilyRef (the host ATS's, which a crossed ATSFontFamilyFindFromName hands out) and a family ID, through
 * the family's name */
static int sp_FMGetFontFamilyFromATSFontFamilyRef(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    static int32_t (*get_name)(uint32_t, uint32_t, CFStringRef *);
    if (!get_name)
        get_name = (int32_t (*)(uint32_t, uint32_t, CFStringRef *))dlsym(RTLD_DEFAULT, "ATSFontFamilyGetName");
    CFStringRef n = NULL;
    if (!ready() || !get_name || get_name(A(0), 0, &n) != 0 || !n)
        RET((uint32_t)INVALID_FAMILY);
    int id = family_id(n);
    release(n);
    RET((uint32_t)(int32_t)id);
}
static int sp_FMGetATSFontFamilyRefFromFontFamily(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    static uint32_t (*find_family)(CFStringRef, uint32_t);
    if (!find_family)
        find_family = (uint32_t (*)(CFStringRef, uint32_t))dlsym(RTLD_DEFAULT, "ATSFontFamilyFindFromName");
    CFStringRef n = ready() ? family_name((int16_t)A(0)) : NULL;
    RET(n && find_family ? find_family(n, 0) : 0);
}

/* CGFontCreateWithPlatformFont(void *platformFontReference): a pointer to an ATSFontRef.  CoreGraphics asks ATS for
 * it, and ATS stops in Crash__Please_File_a_Radar_against_ATS, which puts up an alert and waits (Firefox 3.6 sat there
 * with no window).  A font of ours is made from its PostScript name instead; any other answers NULL. */
static int sp_CGFontCreateWithPlatformFont(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    static void *(*by_name)(CFStringRef);
    if (!by_name)
        by_name = (void *(*)(CFStringRef))dlsym(RTLD_DEFAULT, "CGFontCreateWithFontName");
    uint32_t id = A(0) ? m32_rd(A(0)) : 0;
    CFStringRef ps = NULL;
    pthread_mutex_lock(&g_font_lock);
    if (id >= FONT_BASE && id - FONT_BASE < (uint32_t)g_nfonts)
        ps = g_fonts[id - FONT_BASE];
    pthread_mutex_unlock(&g_font_lock);
    RET(ps && by_name && ready() ? m32_handle(by_name(ps)) : 0);
}

/* ATSFontGetTable(ATSFontRef, FourCharCode tag, ByteOffset, ByteCount size, void *buffer, ByteCount *actual): the
 * table CoreText has for a font of ours, its size alone without a buffer.  Crossed, ATS answered paramErr for every
 * one of our ATSFontRefs, and Firefox 3.6, reading each font's 'name' table, found no usable font and aborted. */
static int sp_ATSFontGetTable(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    CTFontRef f = ready() ? font_at(A(0), 12) : NULL;
    CFDataRef t = f ? CTFontCopyTable_(f, (CTFontTableTag)A(1), kCTFontTableOptionNoOptions) : NULL;
    release(f);
    if (!t)
        RET((uint32_t)-50);
    uint32_t len = (uint32_t)CFDataGetLength_(t), off = A(2), size = A(3), n = 0;
    if (A(4) && off < len) {
        n = len - off < size ? len - off : size;
        memcpy(m32_h(A(4)), CFDataGetBytePtr_(t) + off, n);
    }
    if (A(5))
        m32_wr(A(5), A(4) ? n : len);
    release(t);
    RET(NO_ERR);
}

static int sp_identity(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(A(0)); }
static int sp_GetAppFont(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(1); }
static int sp_GetSysFont(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(0); }
static int sp_GetDefFontSize(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(12); }

/* GetThemeFont(ThemeFontID, ScriptCode, Str255 name, SInt16 *size, Style *style): the system font at Aqua's sizes */
static int sp_GetThemeFont(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    static const struct { int id, size, bold; } k[] = {
        { 0, 13, 0 }, { 1, 11, 0 }, { 2, 11, 1 }, { 3, 12, 0 }, { 4, 13, 1 }, { 5, 13, 0 }, { 6, 10, 0 },
        { 100, 14, 0 }, { 101, 14, 0 }, { 102, 14, 0 }, { 103, 14, 0 }, { 104, 13, 0 }, { 105, 13, 0 },
        { 106, 11, 0 }, { 107, 13, 1 }, { 108, 11, 0 }, { 109, 9, 0 },
    };
    int id = (int16_t)A(0), size = 13, bold = 0;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (k[i].id == id) {
            size = k[i].size;
            bold = k[i].bold;
        }
    if (!ready())
        RET(PARAM_ERR);
    pascal_out(system_family(), A(2));
    if (A(3))
        *(int16_t *)m32_h(A(3)) = (int16_t)size;
    if (A(4))
        *(uint8_t *)m32_h(A(4)) = (uint8_t)bold;
    RET(NO_ERR);
}
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
static int sp_ATSUCreateAndCopyStyle(struct OcerzVM *vm, OcerzCPU *cpu)   /* (ATSUStyle src, ATSUStyle *) */
{
    (void)vm;
    AtsuStyle *s = calloc(1, sizeof *s), *src = m32_host(A(0));
    if (!s || !ready())
        RET(PARAM_ERR);
    if (src)
        *s = *src;
    else
        s->size = 12;
    if (A(1))
        m32_wr(A(1), m32_handle(s));
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

/* ATSUGetUnjustifiedBounds(layout, lineStart, lineLength, ATSUTextMeasurement *before, *after, *ascent, *descent):
 * the line's typographic width and height, Fixed */
static int sp_ATSUGetUnjustifiedBounds(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    AtsuLayout *l = m32_host(A(0));
    if (!l)
        RET(INVALID_LAYOUT);
    uint32_t from = A(1) == 0xffffffffu ? l->offset : A(1), count = A(2) == 0xffffffffu ? l->offset + l->length - from : A(2);
    CTLineRef line = make_line(l, from, count);
    CGFloat ascent = 0, descent = 0, leading = 0;
    double width = line ? CTLineGetTypographicBounds_(line, &ascent, &descent, &leading) : 0;
    release(line);
    const double v[4] = { 0, width, ascent, descent };
    for (int i = 0; i < 4; i++)
        if (A(3 + i))
            m32_wr(A(3 + i), (uint32_t)(int32_t)lround(v[i] * 65536.0));
    RET(NO_ERR);
}

/* the guest's ATSUStyle for character at (absolute), from the layout's runs */
static uint32_t style_at(const AtsuLayout *l, uint32_t at)
{
    uint32_t start = l->offset;
    for (uint32_t r = 0; r < l->runs; r++) {
        uint32_t len = l->run_len[r] == 0xffffffffu ? l->offset + l->length - start : l->run_len[r];
        if (at >= start && at < start + len)
            return l->run_style[r] ? m32_handle(l->run_style[r]) : 0;
        start += len;
    }
    return 0;
}

/* ATSUGetGlyphInfo(layout, lineStart, lineLength, ByteCount *size, ATSUGlyphInfoArray *): i386 {layout, numGlyphs}
 * then 28-byte ATSUGlyphInfos {glyphID @0, layoutFlags @4, charIndex @8, style @12, deltaY @16, idealX @20, screenX
 * @24, caretX @26}; with no array, the size it takes */
static int sp_ATSUGetGlyphInfo(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    AtsuLayout *l = m32_host(A(0));
    if (!l || !A(3))
        RET(INVALID_LAYOUT);
    uint32_t from = A(1) == 0xffffffffu ? l->offset : A(1), count = A(2) == 0xffffffffu ? l->offset + l->length - from : A(2);
    CTLineRef line = make_line(l, from, count);
    CFArrayRef runs = line ? CTLineGetGlyphRuns_(line) : NULL;
    long nruns = runs ? CFArrayGetCount_(runs) : 0, n = 0;
    for (long r = 0; r < nruns; r++)
        n += CTRunGetGlyphCount_(CFArrayGetValueAtIndex_(runs, r));
    uint32_t need = 8 + 28 * (uint32_t)n, have = m32_rd(A(3)), out = A(4);
    m32_wr(A(3), need);
    if (!out) {
        release(line);
        RET(NO_ERR);
    }
    uint32_t fit = have >= 8 ? (have - 8) / 28 : 0, k = 0;
    m32_wr(out, A(0));
    for (long r = 0; r < nruns && k < fit; r++) {
        CTRunRef run = CFArrayGetValueAtIndex_(runs, r);
        long rn = CTRunGetGlyphCount_(run);
        CGGlyph *glyphs = calloc((size_t)rn + 1, sizeof *glyphs);
        CGPoint *pos = calloc((size_t)rn + 1, sizeof *pos);
        CFIndex *idx = calloc((size_t)rn + 1, sizeof *idx);
        CTRunGetGlyphs_(run, CFRangeMake(0, rn), glyphs);
        CTRunGetPositions_(run, CFRangeMake(0, rn), pos);
        CTRunGetStringIndices_(run, CFRangeMake(0, rn), idx);
        for (long i = 0; i < rn && k < fit; i++, k++) {
            uint32_t d = out + 8 + 28 * k, ci = from + (uint32_t)idx[i];
            float x = (float)pos[i].x, dy = 0;
            int16_t sx = (int16_t)lround(pos[i].x);
            memset(m32_h(d), 0, 28);
            memcpy(m32_h(d), &glyphs[i], 2);
            m32_wr(d + 8, ci);
            m32_wr(d + 12, style_at(l, ci));
            memcpy(m32_h(d + 16), &dy, 4);
            memcpy(m32_h(d + 20), &x, 4);
            memcpy(m32_h(d + 24), &sx, 2);
            memcpy(m32_h(d + 26), &sx, 2);
        }
        free(glyphs);
        free(pos);
        free(idx);
    }
    m32_wr(out + 4, k);
    release(line);
    RET(NO_ERR);
}

/* ATSUGetGlyphBounds(layout, Fixed x, Fixed y, start, length, UInt16 type, ItemCount max, ATSTrapezoid bounds[],
 * ItemCount *actual): one trapezoid, the run's typographic box about its baseline at (x, y), y growing down */
static int sp_ATSUGetGlyphBounds(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    AtsuLayout *l = m32_host(A(0));
    if (!l)
        RET(INVALID_LAYOUT);
    uint32_t from = A(3) == 0xffffffffu ? l->offset : A(3), count = A(4) == 0xffffffffu ? l->offset + l->length - from : A(4);
    CTLineRef line = make_line(l, from, count);
    CGFloat ascent = 0, descent = 0;
    double width = line ? CTLineGetTypographicBounds_(line, &ascent, &descent, NULL) : 0;
    release(line);
    double x = (int32_t)A(1) / 65536.0, y = (int32_t)A(2) / 65536.0;
    const double pt[8] = { x, y - ascent, x + width, y - ascent, x + width, y + descent, x, y + descent };
    if (A(6) >= 1 && A(7))
        for (int i = 0; i < 8; i++)
            m32_wr(A(7) + 4 * (uint32_t)i, (uint32_t)(int32_t)lround(pt[i] * 65536.0));
    if (A(8))
        m32_wr(A(8), A(6) >= 1 ? 1 : 0);
    RET(NO_ERR);
}

static int sp_ATSUSetFontFeatures(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(NO_ERR); }   /* ponytail: ignored */

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
    { "_ATSUCreateAndCopyStyle", sp_ATSUCreateAndCopyStyle }, { "_ATSUGetUnjustifiedBounds", sp_ATSUGetUnjustifiedBounds },
    { "_GetFNum", sp_GetFNum }, { "_FMGetFontFamilyFromName", sp_FMGetFontFamilyFromName },
    { "_FMGetFontFamilyName", sp_FMGetFontFamilyName }, { "_GetFontName", sp_GetFontName },
    { "_FMGetFontFromFontFamilyInstance", sp_FMGetFontFromFontFamilyInstance },
    { "_FMGetFontFamilyInstanceFromFont", sp_FMGetFontFamilyInstanceFromFont },
    { "_FMGetATSFontRefFromFont", sp_identity }, { "_FMGetFontFromATSFontRef", sp_identity },
    { "_FMGetFontFamilyFromATSFontFamilyRef", sp_FMGetFontFamilyFromATSFontFamilyRef },
    { "_FMGetATSFontFamilyRefFromFontFamily", sp_FMGetATSFontFamilyRefFromFontFamily },
    { "_GetAppFont", sp_GetAppFont }, { "_GetSysFont", sp_GetSysFont }, { "_GetDefFontSize", sp_GetDefFontSize },
    { "_GetThemeFont", sp_GetThemeFont }, { "_CGFontCreateWithPlatformFont", sp_CGFontCreateWithPlatformFont },
    { "_ATSFontGetTable", sp_ATSFontGetTable }, { "_ATSUGetGlyphInfo", sp_ATSUGetGlyphInfo },
    { "_ATSUGetGlyphBounds", sp_ATSUGetGlyphBounds }, { "_ATSUSetFontFeatures", sp_ATSUSetFontFeatures },
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

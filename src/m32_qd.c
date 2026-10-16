/*
 * QuickDraw for i386 guests (include/ocerz/m32.h), on CoreGraphics.  Apple silicon keeps QuickDraw's region,
 * rectangle and point math and cursor visibility (ApplicationServices still exports them, and
 * tools/sdkgen/headers/QuickDrawRetained.h declares them so they cross); ports, GWorlds, drawing, pen and text
 * state, cursors, the mouse and graphics devices are gone.  These specials keep the calls' documented contracts,
 * with the i386 structure layouts measured with clang -arch i386 against the SDK's QD headers (pack 2):
 *
 *   ports     a GrafPtr is the guest handle of a QdPort.  A window's port draws into a bitmap context the size of the
 *             window's content, whose pixels are in guest memory, and the window's content view draws that bitmap
 *             when it draws, through a kEventControlDraw handler installed the first time anything is drawn; every
 *             drawing call asks the view to redraw.  A GWorld's port is a bitmap context over guest pixels.
 *   pixels    32-bit ones are QuickDraw's xRGB in big-endian byte order, 16-bit ones x555, also big-endian, as they
 *             were on Intel Macs; PixMaps live in guest memory, so a program that writes pixels through
 *             GetPixBaseAddr draws.  Other depths are refused (cDepthErr).
 *   drawing   pens, lines, rectangle fills and frames, regions filled through HIShape, CopyBits between ports and
 *             PixMaps the program built itself, clipped to the clip region; the colors are RGB, the eight classic
 *             ones as Inside Macintosh gives them.  QDBeginCGContext hands out the port's own context, with
 *             CoreGraphics' coordinates.
 *   cursors   GetCursor's system cursors and SetCursor's arrow are NSCursor's; any other Cursor is made from its
 *             16x16 bits.  Color cursors (resources) are the arrow.
 *   devices   one GDevice per display, main first, with its bounds in global coordinates and a 32-bit PixMap.
 *
 * ponytail: transfer modes other than srcCopy copy, and text is not drawn (QuickDraw's text calls only keep the
 * port's state).  docs/m32.md, Limits.
 */
#include "ocerz/interp.h"
#include "ocerz/m32.h"

#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0   /* Carbon's check() and require() */
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#define RET(v) do { m32_ret(cpu, (uint32_t)(v), 0, 0); return OCERZ_STEP_OK; } while (0)
#define A(i) m32_arg(cpu, i)

enum { NO_ERR = 0, PARAM_ERR = -50, DEPTH_ERR = -157, QD_MAGIC = 0x51447074 /* 'QDpt' */ };

/* guest structure offsets (i386, pack 2): Rect 8 (top, left, bottom, right), Point 4 (v, h), RGBColor 6, PixMap 50,
 * BitMap 14, GDevice 62, Cursor 68 */
enum { PM_BASE = 0, PM_ROWBYTES = 4, PM_BOUNDS = 6, PM_VERSION = 14, PM_HRES = 22, PM_VRES = 26, PM_TYPE = 30,
       PM_SIZE = 32, PM_CMPCOUNT = 34, PM_CMPSIZE = 36, PM_FORMAT = 38, PIXMAP_BYTES = 50,
       GD_TYPE = 4, GD_FLAGS = 20, GD_PMAP = 22, GD_NEXT = 30, GD_RECT = 34, GDEVICE_BYTES = 62,
       CURSOR_BYTES = 68, CCRSR_BYTES = 96 };

typedef struct { int16_t top, left, bottom, right; } QdRect;

static int16_t rd16(uint32_t g) { int16_t v; memcpy(&v, m32_h(g), 2); return v; }
static void wr16(uint32_t g, int16_t v) { memcpy(m32_h(g), &v, 2); }
static QdRect rd_rect(uint32_t g) { QdRect r = { 0, 0, 0, 0 }; if (g) memcpy(&r, m32_h(g), 8); return r; }
static void wr_rect(uint32_t g, QdRect r) { if (g) memcpy(m32_h(g), &r, 8); }
static void wr_point(uint32_t g, int16_t h, int16_t v) { if (g) { wr16(g, v); wr16(g + 2, h); } }

/* ---- the host functions, looked up once (ocerz links no frameworks) ---- */

#define HOST(fn) static __typeof__(fn) *fn##_
HOST(CGColorSpaceCreateDeviceRGB); HOST(CGColorSpaceRelease); HOST(CGBitmapContextCreate); HOST(CGContextRelease);
HOST(CGContextSaveGState); HOST(CGContextRestoreGState); HOST(CGContextConcatCTM); HOST(CGContextTranslateCTM);
HOST(CGContextScaleCTM); HOST(CGContextSetRGBFillColor); HOST(CGContextSetRGBStrokeColor); HOST(CGContextFillRect);
HOST(CGContextStrokeRectWithWidth); HOST(CGContextSetLineWidth); HOST(CGContextSetLineCap); HOST(CGContextBeginPath);
HOST(CGContextMoveToPoint); HOST(CGContextAddLineToPoint); HOST(CGContextStrokePath); HOST(CGContextFillPath);
HOST(CGContextSetShouldAntialias); HOST(CGContextClip); HOST(CGContextSetBlendMode); HOST(CGContextSetInterpolationQuality);
HOST(CGBitmapContextCreateImage); HOST(CGImageCreateWithImageInRect); HOST(CGImageCreate); HOST(CGImageRelease);
HOST(CGContextDrawImage); HOST(CGDataProviderCreateWithData); HOST(CGDataProviderRelease); HOST(CGEventCreate);
HOST(CGEventGetLocation); HOST(CGGetActiveDisplayList); HOST(CGMainDisplayID); HOST(CGDisplayBounds);
HOST(CFRelease);
HOST(InstallEventHandler); HOST(GetEventParameter);
/* declared for 32-bit code alone, as the hidden HIToolbox calls tools/sdkgen crosses are: their documented prototypes */
static OSStatus (*GetWindowBounds_)(WindowRef, WindowRegionCode, Rect *);
static HIViewRef (*HIViewGetRoot_)(WindowRef);
static OSStatus (*HIViewFindByID_)(HIViewRef, HIViewID, HIViewRef *);
static OSStatus (*HIViewSetNeedsDisplay_)(HIViewRef, Boolean);
static EventTargetRef (*GetControlEventTarget_)(HIViewRef);
static HIShapeRef (*HIShapeCreateWithQDRgn_)(void *);
static OSStatus (*HIShapeReplacePathInCGContext_)(HIShapeRef, CGContextRef);
typedef void *QdRgn;
static QdRgn (*NewRgn_)(void);
static void (*DisposeRgn_)(QdRgn);
static void (*CopyRgn_)(QdRgn, QdRgn);
static void (*RectRgn_)(QdRgn, const QdRect *);
static QdRect *(*GetRegionBounds_)(QdRgn, QdRect *);
static Boolean (*IsRegionRectangular_)(QdRgn);
static void (*NSBeep_)(void);
static const HIViewID *content_id_;   /* kHIViewWindowContentID, a variable */

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_ok;
static void load(void)
{
    void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY | RTLD_LOCAL);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY | RTLD_LOCAL);
    void *hi = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon", RTLD_LAZY | RTLD_LOCAL);
    void *as = dlopen("/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices", RTLD_LAZY | RTLD_LOCAL);
    void *ak = dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_LAZY | RTLD_LOCAL);
    int ok = cg && cf && hi && as;
#define L(h, fn) ok &= (fn##_ = (__typeof__(fn##_))dlsym(h, #fn)) != NULL
    L(cg, CGColorSpaceCreateDeviceRGB); L(cg, CGColorSpaceRelease); L(cg, CGBitmapContextCreate); L(cg, CGContextRelease);
    L(cg, CGContextSaveGState); L(cg, CGContextRestoreGState); L(cg, CGContextConcatCTM); L(cg, CGContextTranslateCTM);
    L(cg, CGContextScaleCTM); L(cg, CGContextSetRGBFillColor); L(cg, CGContextSetRGBStrokeColor); L(cg, CGContextFillRect);
    L(cg, CGContextStrokeRectWithWidth); L(cg, CGContextSetLineWidth); L(cg, CGContextSetLineCap); L(cg, CGContextBeginPath);
    L(cg, CGContextMoveToPoint); L(cg, CGContextAddLineToPoint); L(cg, CGContextStrokePath); L(cg, CGContextFillPath);
    L(cg, CGContextSetShouldAntialias); L(cg, CGContextClip); L(cg, CGContextSetBlendMode);
    L(cg, CGContextSetInterpolationQuality); L(cg, CGBitmapContextCreateImage); L(cg, CGImageCreateWithImageInRect);
    L(cg, CGImageCreate); L(cg, CGImageRelease); L(cg, CGContextDrawImage); L(cg, CGDataProviderCreateWithData);
    L(cg, CGDataProviderRelease); L(cg, CGEventCreate); L(cg, CGEventGetLocation); L(cg, CGGetActiveDisplayList);
    L(cg, CGMainDisplayID); L(cg, CGDisplayBounds); L(cf, CFRelease);
    L(hi, GetWindowBounds); L(hi, HIViewGetRoot); L(hi, HIViewFindByID); L(hi, HIViewSetNeedsDisplay);
    L(hi, InstallEventHandler); L(hi, GetControlEventTarget); L(hi, GetEventParameter); L(hi, HIShapeCreateWithQDRgn);
    L(hi, HIShapeReplacePathInCGContext);
    L(as, NewRgn); L(as, DisposeRgn); L(as, CopyRgn); L(as, RectRgn); L(as, GetRegionBounds); L(as, IsRegionRectangular);
#undef L
    ok &= (content_id_ = (const HIViewID *)dlsym(hi, "kHIViewWindowContentID")) != NULL;
    NSBeep_ = ak ? (void (*)(void))dlsym(ak, "NSBeep") : NULL;
    g_ok = ok;
    if (!ok)
        fprintf(stderr, "ocerz: m32: QuickDraw: a CoreGraphics, HIToolbox or QuickDraw function is missing\n");
}
static int ready(void) { pthread_once(&g_once, load); return g_ok; }

/* ---- ports ---- */

typedef struct QdPort {
    uint32_t magic;
    int window;                /* a window's port, else a GWorld's */
    WindowRef win;
    HIViewRef view;            /* the window's content view, once its draw handler is in */
    CGContextRef ctx;
    int w, h, depth;
    uint32_t pixels, rowbytes; /* guest pixel buffer */
    int own_pixels;            /* allocated here, freed with the port */
    uint32_t pixmap;           /* guest PixMapHandle: a master pointer to the PixMap after it */
    int16_t oh, ov;            /* SetOrigin */
    QdRgn clip;
    int16_t pen_h, pen_v, pen_w, pen_ht;
    double fore[3], back[3];
    int16_t font, size, mode;
    uint8_t face;
    uint32_t guest;
} QdPort;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static QdPort **g_ports;
static int g_nports, g_cports;
static QdPort *g_dead;   /* disposed ports, kept so a stale GrafPtr never names freed memory, reused by the next */
static QdPort *g_cur;
static uint32_t g_cur_gdev;

static QdPort *port_of(uint32_t g)
{
    if (!g || !m32_is_handle(g))
        return NULL;
    QdPort *p = (QdPort *)m32_host(g);
    return p && p->magic == QD_MAGIC ? p : NULL;
}

static QdPort *port_by_window(WindowRef w)
{
    QdPort *found = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nports && !found; i++)
        if (g_ports[i]->window && g_ports[i]->win == w)
            found = g_ports[i];
    pthread_mutex_unlock(&g_lock);
    return found;
}

static QdPort *port_by_pixmap(uint32_t pm)
{
    QdPort *found = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nports && !found; i++)
        if (g_ports[i]->pixmap && m32_rd(g_ports[i]->pixmap) == pm)
            found = g_ports[i];
    pthread_mutex_unlock(&g_lock);
    return found;
}

static QdPort *port_new(void)
{
    pthread_mutex_lock(&g_lock);
    QdPort *p = g_dead;
    if (p)
        g_dead = *(QdPort **)&p->win;   /* the dead list runs through win */
    pthread_mutex_unlock(&g_lock);
    uint32_t guest = p ? p->guest : 0;
    if (!p)
        p = malloc(sizeof *p);
    memset(p, 0, sizeof *p);
    p->magic = QD_MAGIC;
    p->pen_w = p->pen_ht = 1;
    p->back[0] = p->back[1] = p->back[2] = 1;
    p->size = 12;
    p->clip = NewRgn_();
    QdRect wide = { -32767, -32767, 32767, 32767 };
    RectRgn_(p->clip, &wide);
    p->guest = guest ? guest : m32_handle(p);
    pthread_mutex_lock(&g_lock);
    if (g_nports == g_cports) {
        g_cports = g_cports ? g_cports * 2 : 16;
        g_ports = realloc(g_ports, (size_t)g_cports * sizeof *g_ports);
    }
    g_ports[g_nports++] = p;
    pthread_mutex_unlock(&g_lock);
    return p;
}

static void port_free(QdPort *p)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nports; i++)
        if (g_ports[i] == p)
            g_ports[i] = g_ports[--g_nports];
    pthread_mutex_unlock(&g_lock);
    if (g_cur == p)
        g_cur = NULL;
    if (p->ctx)
        CGContextRelease_(p->ctx);
    if (p->own_pixels)
        m32_free(p->pixels);
    if (p->pixmap) {
        m32_free(m32_rd(p->pixmap));
        m32_free(p->pixmap);
    }
    DisposeRgn_(p->clip);
    uint32_t guest = p->guest;
    memset(p, 0, sizeof *p);
    p->guest = guest;
    pthread_mutex_lock(&g_lock);
    *(QdPort **)&p->win = g_dead;
    g_dead = p;
    pthread_mutex_unlock(&g_lock);
}

/* a port's host window, or its pixels for a GWorld's: AGL draws into either (src/m32_agl.c) */
void *m32_qd_port_window(uint32_t port)
{
    QdPort *p = port_of(port);
    return p && p->window ? (void *)p->win : NULL;
}

int m32_qd_port_pixels(uint32_t port, int *w, int *h, uint32_t *rowbytes, uint32_t *pixels)
{
    QdPort *p = port_of(port);
    if (!p || p->window || !p->ctx)
        return 0;
    *w = p->w;
    *h = p->h;
    *rowbytes = p->rowbytes;
    *pixels = p->pixels;
    return 1;
}

/* DisposeWindow takes the window's port with it (src/m32_carbon.c calls this first) */
void m32_qd_window_gone(void *window)
{
    QdPort *p = window ? port_by_window((WindowRef)window) : NULL;
    if (p)
        port_free(p);
}

/* CoreGraphics' bitmap info for a QuickDraw pixel format, 0 when CoreGraphics cannot draw into it */
static uint32_t cg_info(uint32_t format, int *bpp)
{
    switch (format) {
    case 32: case 'ARGB': *bpp = 32; return kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Big;
    case 'BGRA': *bpp = 32; return kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little;
    case 16: *bpp = 16; return kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder16Big;
    case 'L555': *bpp = 16; return kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder16Little;
    default: *bpp = 0; return 0;
    }
}

/* the PixMap describing a port's pixels, written again whenever they move */
static void write_pixmap(QdPort *p, uint32_t format)
{
    if (!p->pixmap) {
        p->pixmap = m32_malloc(4);
        m32_wr(p->pixmap, m32_malloc(PIXMAP_BYTES));
    }
    uint32_t pm = m32_rd(p->pixmap);
    memset(m32_h(pm), 0, PIXMAP_BYTES);
    m32_wr(pm + PM_BASE, p->pixels);
    wr16(pm + PM_ROWBYTES, (int16_t)(0x8000 | (p->rowbytes & 0x3fff)));
    QdRect b = { p->ov, p->oh, (int16_t)(p->ov + p->h), (int16_t)(p->oh + p->w) };
    memcpy(m32_h(pm + PM_BOUNDS), &b, 8);
    m32_wr(pm + PM_HRES, 72 << 16);
    m32_wr(pm + PM_VRES, 72 << 16);
    wr16(pm + PM_TYPE, 16);   /* RGBDirect */
    wr16(pm + PM_SIZE, (int16_t)p->depth);
    wr16(pm + PM_CMPCOUNT, 3);
    wr16(pm + PM_CMPSIZE, p->depth == 32 ? 8 : 5);
    m32_wr(pm + PM_FORMAT, format);
}

/* a bitmap context over pixels of w x h at depth, in guest memory (allocated when pixels is 0) */
static int port_pixels(QdPort *p, int w, int h, uint32_t format, uint32_t pixels, uint32_t rowbytes)
{
    int bpp = 0;
    uint32_t info = cg_info(format, &bpp);
    if (!info || w <= 0 || h <= 0)
        return DEPTH_ERR;
    if (p->ctx)
        CGContextRelease_(p->ctx);
    p->ctx = NULL;
    if (p->own_pixels)
        m32_free(p->pixels);
    p->own_pixels = !pixels;
    if (!pixels) {
        rowbytes = ((uint32_t)w * (uint32_t)bpp / 8 + 15) & ~15u;
        pixels = m32_calloc(rowbytes, (uint32_t)h);
        if (!pixels)
            return -108;   /* memFullErr */
    }
    p->w = w;
    p->h = h;
    p->depth = bpp;
    p->pixels = pixels;
    p->rowbytes = rowbytes;
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB_();
    p->ctx = CGBitmapContextCreate_(m32_h(pixels), (size_t)w, (size_t)h, bpp == 32 ? 8 : 5, rowbytes, rgb, info);
    CGColorSpaceRelease_(rgb);
    write_pixmap(p, format);
    return p->ctx ? NO_ERR : PARAM_ERR;
}

/* a window's port follows its content's size */
static void window_size(QdPort *p)
{
    QdRect r;
    if (GetWindowBounds_(p->win, kWindowContentRgn, (Rect *)&r) != noErr)
        return;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w > 0 && h > 0 && (w != p->w || h != p->h || !p->ctx) && port_pixels(p, w, h, 32, 0, 0) == NO_ERR)
        memset(m32_h(p->pixels), 0xff, (size_t)p->rowbytes * (size_t)h);   /* a window's content starts erased, white */
}

static OSStatus window_draw(EventHandlerCallRef next, EventRef ev, void *data)
{
    (void)next;
    QdPort *p = data;
    CGContextRef c = NULL;
    if (p->magic != QD_MAGIC || !p->ctx ||
        GetEventParameter_(ev, kEventParamCGContextRef, typeCGContextRef, NULL, sizeof c, NULL, &c) != noErr || !c)
        return eventNotHandledErr;
    CGImageRef img = CGBitmapContextCreateImage_(p->ctx);
    if (!img)
        return eventNotHandledErr;
    CGContextSaveGState_(c);
    /* the view's context has its origin at the top left: flip so the image stands upright, its top at the top */
    CGContextTranslateCTM_(c, 0, p->h);
    CGContextScaleCTM_(c, 1, -1);
    CGContextDrawImage_(c, CGRectMake(0, 0, p->w, p->h), img);
    CGContextRestoreGState_(c);
    CGImageRelease_(img);
    return noErr;
}

/* after drawing into a window's port: the content view draws the port, now and whenever it redraws */
static void port_changed(QdPort *p)
{
    if (!p->window)
        return;
    if (!p->view) {
        HIViewRef content = NULL;
        HIViewFindByID_(HIViewGetRoot_(p->win), *content_id_, &content);
        if (!content)
            return;
        static const EventTypeSpec draw = { kEventClassControl, kEventControlDraw };
        InstallEventHandler_(GetControlEventTarget_(content), window_draw, 1, &draw, p, NULL);
        p->view = content;
    }
    HIViewSetNeedsDisplay_(p->view, true);
}

/* local QuickDraw coordinates (top left, past SetOrigin) to the bitmap's (bottom left) */
static CGAffineTransform local_to_cg(const QdPort *p)
{
    return CGAffineTransformMake(1, 0, 0, -1, -p->oh, p->h + p->ov);
}

/* the port's context, ready for a QuickDraw call in local coordinates and clipped to the clip region */
static CGContextRef begin(QdPort *p)
{
    if (p->window)
        window_size(p);
    if (!p->ctx)
        return NULL;
    CGContextSaveGState_(p->ctx);
    CGContextConcatCTM_(p->ctx, local_to_cg(p));
    CGContextSetShouldAntialias_(p->ctx, false);
    QdRect b;
    GetRegionBounds_(p->clip, &b);
    if (!(IsRegionRectangular_(p->clip) && b.left <= p->oh && b.top <= p->ov && b.right >= p->oh + p->w &&
          b.bottom >= p->ov + p->h)) {
        HIShapeRef shape = HIShapeCreateWithQDRgn_(p->clip);
        if (shape) {
            HIShapeReplacePathInCGContext_(shape, p->ctx);
            CGContextClip_(p->ctx);
            CFRelease_(shape);
        }
    }
    return p->ctx;
}

static void end(QdPort *p)
{
    CGContextRestoreGState_(p->ctx);
    port_changed(p);
}

static CGRect cgrect(QdRect r) { return CGRectMake(r.left, r.top, r.right - r.left, r.bottom - r.top); }

/* draws img into local rect r, upright */
static void draw_image(CGContextRef c, CGImageRef img, QdRect r)
{
    CGContextSaveGState_(c);
    CGContextTranslateCTM_(c, r.left, r.bottom);
    CGContextScaleCTM_(c, 1, -1);
    CGContextSetInterpolationQuality_(c, kCGInterpolationNone);
    CGContextDrawImage_(c, CGRectMake(0, 0, r.right - r.left, r.bottom - r.top), img);
    CGContextRestoreGState_(c);
}

/* ---- GetPort, SetPort and the window's port ---- */

static QdPort *port_for_window(WindowRef w)
{
    if (!w || !ready())
        return NULL;
    QdPort *p = port_by_window(w);
    if (!p) {
        p = port_new();
        p->window = 1;
        p->win = w;
        window_size(p);
    }
    return p;
}

static QdPort *window_port(uint32_t gwin) { return port_for_window((WindowRef)m32_host(gwin)); }

/* for Carbon events (src/m32_carbon.c): a window's port, and the current one */
uint32_t m32_qd_window_port(void *window)
{
    QdPort *p = port_for_window((WindowRef)window);
    return p ? p->guest : 0;
}

uint32_t m32_qd_current_port(void) { return g_cur ? g_cur->guest : 0; }

static int qd_GetWindowPort(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; QdPort *p = window_port(A(0)); RET(p ? p->guest : 0); }
static int qd_SetPortWindowPort(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; QdPort *p = window_port(A(0)); if (p) g_cur = p; RET(0); }
static int qd_GetWindowFromPort(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    QdPort *p = port_of(A(0));
    RET(p && p->window ? m32_handle(p->win) : 0);
}
static int qd_IsValidPort(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(port_of(A(0)) != NULL); }
static int qd_GetPort(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (A(0)) m32_wr(A(0), g_cur ? g_cur->guest : 0); RET(0); }
static int qd_SetPort(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; QdPort *p = port_of(A(0)); if (p || !A(0)) g_cur = p; RET(0); }
static int qd_GetQDGlobalsThePort(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(g_cur ? g_cur->guest : 0); }
static int qd_QDSwapPort(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    QdPort *was = g_cur, *p = port_of(A(0));
    if (A(1))
        m32_wr(A(1), was ? was->guest : 0);
    g_cur = p;
    RET(p != was);
}

static int qd_GetPortBounds(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    QdPort *p = port_of(A(0));
    if (p && p->window)
        window_size(p);
    QdRect r = { 0, 0, 0, 0 };
    if (p)
        r = (QdRect){ p->ov, p->oh, (int16_t)(p->ov + p->h), (int16_t)(p->oh + p->w) };
    wr_rect(A(1), r);
    RET(A(1));
}

static int qd_SetOrigin(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    if (g_cur) {
        g_cur->oh = (int16_t)A(0);
        g_cur->ov = (int16_t)A(1);
        if (g_cur->pixmap) {
            uint32_t pm = m32_rd(g_cur->pixmap);
            QdRect b = { g_cur->ov, g_cur->oh, (int16_t)(g_cur->ov + g_cur->h), (int16_t)(g_cur->oh + g_cur->w) };
            memcpy(m32_h(pm + PM_BOUNDS), &b, 8);
        }
    }
    RET(0);
}

/* the content's top left in global coordinates, minus the origin: local + this = global */
static void to_global(const QdPort *p, int *dh, int *dv)
{
    *dh = *dv = 0;
    if (p && p->window) {
        QdRect r;
        if (GetWindowBounds_(p->win, kWindowContentRgn, (Rect *)&r) == noErr) {
            *dh = r.left - p->oh;
            *dv = r.top - p->ov;
        }
    }
}

static int qd_LocalToGlobal(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    int dh, dv;
    to_global(g_cur, &dh, &dv);
    if (A(0))
        wr_point(A(0), (int16_t)(rd16(A(0) + 2) + dh), (int16_t)(rd16(A(0)) + dv));
    RET(0);
}

static int qd_GlobalToLocal(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    int dh, dv;
    to_global(g_cur, &dh, &dv);
    if (A(0))
        wr_point(A(0), (int16_t)(rd16(A(0) + 2) - dh), (int16_t)(rd16(A(0)) - dv));
    RET(0);
}

/* ---- clip ---- */

static int qd_GetClip(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (g_cur && A(0)) CopyRgn_(g_cur->clip, m32_host(A(0))); RET(0); }
static int qd_SetClip(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (g_cur && A(0)) CopyRgn_(m32_host(A(0)), g_cur->clip); RET(0); }
static int qd_ClipRect(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    QdRect r = rd_rect(A(0));
    if (g_cur && A(0))
        RectRgn_(g_cur->clip, &r);
    RET(0);
}
static int qd_GetPortClipRegion(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    QdPort *p = port_of(A(0));
    if (p && A(1))
        CopyRgn_(p->clip, m32_host(A(1)));
    RET(A(1));
}
static int qd_SetPortClipRegion(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    QdPort *p = port_of(A(0));
    if (p && A(1))
        CopyRgn_(m32_host(A(1)), p->clip);
    RET(0);
}
static int qd_GetPortVisibleRegion(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    QdPort *p = port_of(A(0));
    if (p && A(1)) {
        if (p->window)
            window_size(p);
        QdRect r = { p->ov, p->oh, (int16_t)(p->ov + p->h), (int16_t)(p->oh + p->w) };
        RectRgn_(m32_host(A(1)), &r);
    }
    RET(A(1));
}

/* ---- GWorlds and PixMaps ---- */

static int new_gworld(OcerzCPU *cpu, uint32_t out, uint32_t format, uint32_t bounds, uint32_t pixels, uint32_t rowbytes)
{
    if (!ready() || !out || !bounds)
        RET(PARAM_ERR);
    QdRect b = rd_rect(bounds);
    QdPort *p = port_new();
    p->oh = b.left;
    p->ov = b.top;
    int err = port_pixels(p, b.right - b.left, b.bottom - b.top, format, pixels, rowbytes);
    if (err) {
        port_free(p);
        m32_wr(out, 0);
        RET((uint32_t)(int32_t)err);
    }
    m32_wr(out, p->guest);
    RET(NO_ERR);
}

/* NewGWorld(GWorldPtr *, short depth, const Rect *, CTabHandle, GDHandle, GWorldFlags): depth 0 is the main
 * device's, 32 */
static int qd_NewGWorld(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    int depth = (int16_t)A(1);
    if (depth != 0 && depth != 32 && depth != 16) {
        m32_log_once("QuickDraw: a GWorld of a depth other than 16 or 32 bits (cDepthErr):", "NewGWorld");
        if (A(0))
            m32_wr(A(0), 0);
        RET((uint32_t)(int32_t)DEPTH_ERR);
    }
    return new_gworld(cpu, A(0), depth == 16 ? 16 : 32, A(2), 0, 0);
}

/* NewGWorldFromPtr(GWorldPtr *, OSType pixelFormat, const Rect *, CTabHandle, GDHandle, GWorldFlags, Ptr, long) */
static int qd_NewGWorldFromPtr(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    return new_gworld(cpu, A(0), A(1), A(2), A(6), A(7));
}

/* UpdateGWorld(GWorldPtr *, short depth, const Rect *, CTabHandle, GDHandle, GWorldFlags): new pixels when the
 * bounds changed, reported as reallocPix */
static int qd_UpdateGWorld(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    QdPort *p = A(0) ? port_of(m32_rd(A(0))) : NULL;
    if (!p)
        RET((uint32_t)(int32_t)PARAM_ERR);
    QdRect b = rd_rect(A(2));
    int depth = (int16_t)A(1) ? (int16_t)A(1) : p->depth;
    if (b.right - b.left == p->w && b.bottom - b.top == p->h && depth == p->depth) {
        p->oh = b.left;
        p->ov = b.top;
        write_pixmap(p, (uint32_t)p->depth);
        RET(0);
    }
    p->oh = b.left;
    p->ov = b.top;
    int err = port_pixels(p, b.right - b.left, b.bottom - b.top, depth == 16 ? 16 : 32, 0, 0);
    RET(err ? (uint32_t)(int32_t)err : 1 /* reallocPix */);
}

static int qd_DisposeGWorld(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    QdPort *p = port_of(A(0));
    if (p && !p->window)
        port_free(p);
    RET(0);
}

static int qd_GetGWorld(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    if (A(0))
        m32_wr(A(0), g_cur ? g_cur->guest : 0);
    if (A(1))
        m32_wr(A(1), g_cur_gdev);
    RET(0);
}

static int qd_SetGWorld(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    QdPort *p = port_of(A(0));
    if (p || !A(0))
        g_cur = p;
    if (A(1))
        g_cur_gdev = A(1);
    RET(0);
}

static int qd_GetGWorldPixMap(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    QdPort *p = port_of(A(0));
    if (p && p->window)
        window_size(p);
    RET(p ? p->pixmap : 0);
}

/* the PixMap a port's pixels are, as CopyBits takes it */
static int qd_GetPortBitMapForCopyBits(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    QdPort *p = port_of(A(0));
    if (p && p->window)
        window_size(p);
    RET(p && p->pixmap ? m32_rd(p->pixmap) : 0);
}

static int qd_LockPixels(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(A(0) != 0); }
static int qd_Ok0(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(0); }
static int qd_GetPixBaseAddr(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    RET(A(0) && m32_rd(A(0)) ? m32_rd(m32_rd(A(0)) + PM_BASE) : 0);
}
static int qd_GetPixRowBytes(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    if (!A(0) || !m32_rd(A(0)))
        RET(0);
    QdPort *p = port_by_pixmap(m32_rd(A(0)));
    RET(p ? p->rowbytes : (uint32_t)(rd16(m32_rd(A(0)) + PM_ROWBYTES) & 0x3fff));
}
static int qd_GetPixDepth(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    RET(A(0) && m32_rd(A(0)) ? (uint32_t)rd16(m32_rd(A(0)) + PM_SIZE) : 0);
}

/* ---- pens, colors and shapes ---- */

static void classic_color(int32_t c, double *rgb)
{
    static const struct { int32_t c; double r, g, b; } k[] = {
        { 33, 0, 0, 0 }, { 30, 1, 1, 1 }, { 205, 0xDD6B / 65535.0, 0x08C2 / 65535.0, 0x06A2 / 65535.0 },
        { 341, 0, 0x8000 / 65535.0, 0x11B0 / 65535.0 }, { 409, 0, 0, 0xD400 / 65535.0 },
        { 273, 0x0241 / 65535.0, 0xAB54 / 65535.0, 0xEAFF / 65535.0 },
        { 137, 0xF2D7 / 65535.0, 0x0856 / 65535.0, 0x84EC / 65535.0 },
        { 69, 0xFC00 / 65535.0, 0xF37D / 65535.0, 0x052F / 65535.0 },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (k[i].c == c) {
            rgb[0] = k[i].r;
            rgb[1] = k[i].g;
            rgb[2] = k[i].b;
            return;
        }
}

static void read_rgb(uint32_t g, double *rgb)
{
    for (int i = 0; i < 3; i++)
        rgb[i] = (uint16_t)rd16(g + 2 * (uint32_t)i) / 65535.0;
}

static void write_rgb(uint32_t g, const double *rgb)
{
    for (int i = 0; i < 3; i++)
        wr16(g + 2 * (uint32_t)i, (int16_t)(uint16_t)(rgb[i] * 65535.0 + 0.5));
}

static int qd_ForeColor(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (g_cur) classic_color((int32_t)A(0), g_cur->fore); RET(0); }
static int qd_BackColor(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (g_cur) classic_color((int32_t)A(0), g_cur->back); RET(0); }
static int qd_RGBForeColor(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (g_cur && A(0)) read_rgb(A(0), g_cur->fore); RET(0); }
static int qd_RGBBackColor(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (g_cur && A(0)) read_rgb(A(0), g_cur->back); RET(0); }
static int qd_GetForeColor(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (g_cur && A(0)) write_rgb(A(0), g_cur->fore); RET(0); }
static int qd_GetBackColor(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (g_cur && A(0)) write_rgb(A(0), g_cur->back); RET(0); }

static int qd_PenSize(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    if (g_cur) {
        g_cur->pen_w = (int16_t)A(0);
        g_cur->pen_ht = (int16_t)A(1);
    }
    RET(0);
}
static int qd_PenNormal(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (g_cur) g_cur->pen_w = g_cur->pen_ht = 1; RET(0); }
static int qd_GetPen(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (g_cur) wr_point(A(0), g_cur->pen_h, g_cur->pen_v); RET(0); }
static int qd_MoveTo(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    if (g_cur) {
        g_cur->pen_h = (int16_t)A(0);
        g_cur->pen_v = (int16_t)A(1);
    }
    RET(0);
}
static int qd_Move(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    if (g_cur) {
        g_cur->pen_h = (int16_t)(g_cur->pen_h + (int16_t)A(0));
        g_cur->pen_v = (int16_t)(g_cur->pen_v + (int16_t)A(1));
    }
    RET(0);
}

/* QuickDraw's pen hangs below and right of its location: a one-pixel pen covers the pixel at (h, v) */
static void line_to(int16_t h, int16_t v)
{
    QdPort *p = g_cur;
    CGContextRef c = p ? begin(p) : NULL;
    if (c) {
        double ox = p->pen_w / 2.0, oy = p->pen_ht / 2.0;
        CGContextSetRGBStrokeColor_(c, p->fore[0], p->fore[1], p->fore[2], 1);
        CGContextSetLineWidth_(c, p->pen_w > p->pen_ht ? p->pen_w : p->pen_ht);
        CGContextSetLineCap_(c, kCGLineCapSquare);
        CGContextBeginPath_(c);
        CGContextMoveToPoint_(c, p->pen_h + ox, p->pen_v + oy);
        CGContextAddLineToPoint_(c, h + ox, v + oy);
        CGContextStrokePath_(c);
        end(p);
    }
    if (p) {
        p->pen_h = h;
        p->pen_v = v;
    }
}

static int qd_LineTo(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; line_to((int16_t)A(0), (int16_t)A(1)); RET(0); }
static int qd_Line(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    if (g_cur)
        line_to((int16_t)(g_cur->pen_h + (int16_t)A(0)), (int16_t)(g_cur->pen_v + (int16_t)A(1)));
    RET(0);
}

static void fill_rect(uint32_t gr, int back, int invert)
{
    QdPort *p = g_cur;
    QdRect r = rd_rect(gr);
    CGContextRef c = p && gr ? begin(p) : NULL;
    if (!c)
        return;
    const double *rgb = back ? p->back : p->fore;
    if (invert) {
        CGContextSetBlendMode_(c, kCGBlendModeDifference);
        CGContextSetRGBFillColor_(c, 1, 1, 1, 1);
    } else {
        CGContextSetRGBFillColor_(c, rgb[0], rgb[1], rgb[2], 1);
    }
    CGContextFillRect_(c, cgrect(r));
    end(p);
}

static int qd_PaintRect(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; fill_rect(A(0), 0, 0); RET(0); }
static int qd_EraseRect(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; fill_rect(A(0), 1, 0); RET(0); }
static int qd_InvertRect(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; fill_rect(A(0), 0, 1); RET(0); }
static int qd_FrameRect(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    QdPort *p = g_cur;
    QdRect r = rd_rect(A(0));
    CGContextRef c = p && A(0) ? begin(p) : NULL;
    if (c) {
        CGContextSetRGBFillColor_(c, p->fore[0], p->fore[1], p->fore[2], 1);
        int pw = p->pen_w, ph = p->pen_ht;
        CGContextFillRect_(c, CGRectMake(r.left, r.top, r.right - r.left, ph));
        CGContextFillRect_(c, CGRectMake(r.left, r.bottom - ph, r.right - r.left, ph));
        CGContextFillRect_(c, CGRectMake(r.left, r.top, pw, r.bottom - r.top));
        CGContextFillRect_(c, CGRectMake(r.right - pw, r.top, pw, r.bottom - r.top));
        end(p);
    }
    RET(0);
}

static void fill_rgn(uint32_t grgn, int back)
{
    QdPort *p = g_cur;
    CGContextRef c = p && grgn ? begin(p) : NULL;
    if (!c)
        return;
    HIShapeRef shape = HIShapeCreateWithQDRgn_(m32_host(grgn));
    if (shape) {
        const double *rgb = back ? p->back : p->fore;
        CGContextSetRGBFillColor_(c, rgb[0], rgb[1], rgb[2], 1);
        HIShapeReplacePathInCGContext_(shape, c);
        CGContextFillPath_(c);
        CFRelease_(shape);
    }
    end(p);
}

static int qd_PaintRgn(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; fill_rgn(A(0), 0); RET(0); }
static int qd_EraseRgn(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; fill_rgn(A(0), 1); RET(0); }

/* ---- CopyBits ---- */

/* an image of a BitMap or PixMap the program described itself, 16 or 32 bits deep */
static CGImageRef foreign_image(uint32_t bm, QdRect *bounds)
{
    int16_t rb = rd16(bm + PM_ROWBYTES);
    *bounds = rd_rect(bm + PM_BOUNDS);
    if (!(rb & 0x8000))
        return NULL;   /* a 1-bit BitMap */
    int depth = rd16(bm + PM_SIZE), bpp = 0;
    uint32_t format = m32_rd(bm + PM_FORMAT);
    uint32_t info = cg_info(format ? format : (uint32_t)depth, &bpp);
    int w = bounds->right - bounds->left, h = bounds->bottom - bounds->top;
    uint32_t base = m32_rd(bm + PM_BASE);
    if (!info || w <= 0 || h <= 0 || !base)
        return NULL;
    size_t rowbytes = (size_t)(rb & 0x3fff);
    CGDataProviderRef dp = CGDataProviderCreateWithData_(NULL, m32_h(base), rowbytes * (size_t)h, NULL);
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB_();
    CGImageRef img = CGImageCreate_((size_t)w, (size_t)h, bpp == 32 ? 8 : 5, (size_t)bpp, rowbytes, rgb, info, dp, NULL,
                                    false, kCGRenderingIntentDefault);
    CGColorSpaceRelease_(rgb);
    CGDataProviderRelease_(dp);
    return img;
}

/* CopyBits(const BitMap *src, const BitMap *dst, const Rect *srcRect, const Rect *dstRect, short mode, RgnHandle
 * mask): the source's pixels in srcRect, in its own coordinates, drawn into dstRect in the destination's.  The
 * destination is a port; the source a port or a PixMap the program built. */
static int qd_CopyBits(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    if (!ready())
        RET(0);
    QdPort *src = port_by_pixmap(A(0)), *dst = port_by_pixmap(A(1));
    if (!dst) {
        m32_log_once("QuickDraw: CopyBits into pixels that are no port's, not drawn:", "CopyBits");
        RET(0);
    }
    if ((int16_t)A(4) != srcCopy && (int16_t)A(4) != 64 /* ditherCopy */)
        m32_log_once("QuickDraw: CopyBits transfer modes other than srcCopy copy:", "CopyBits");
    QdRect sb, sr = rd_rect(A(2)), dr = rd_rect(A(3));
    CGImageRef whole;
    if (src) {
        if (src->window)
            window_size(src);
        whole = src->ctx ? CGBitmapContextCreateImage_(src->ctx) : NULL;
        sb = (QdRect){ src->ov, src->oh, (int16_t)(src->ov + src->h), (int16_t)(src->oh + src->w) };
    } else {
        whole = foreign_image(A(0), &sb);
    }
    if (!whole) {
        m32_log_once("QuickDraw: CopyBits from pixels it cannot read (a 1-bit BitMap or another depth):", "CopyBits");
        RET(0);
    }
    CGImageRef part = CGImageCreateWithImageInRect_(whole, CGRectMake(sr.left - sb.left, sr.top - sb.top,
                                                                      sr.right - sr.left, sr.bottom - sr.top));
    QdPort *was = g_cur;
    g_cur = dst;
    CGContextRef c = part ? begin(dst) : NULL;
    if (c) {
        CGContextSetBlendMode_(c, kCGBlendModeCopy);
        draw_image(c, part, dr);
        end(dst);
    }
    g_cur = was;
    if (part)
        CGImageRelease_(part);
    CGImageRelease_(whole);
    RET(0);
}

/* ---- CoreGraphics on a port ---- */

/* QDBeginCGContext(CGrafPtr, CGContextRef *): the port's own context, CoreGraphics' coordinates (origin at the
 * bottom left of the port) */
static int qd_QDBeginCGContext(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    QdPort *p = port_of(A(0));
    if (p && p->window)
        window_size(p);
    if (!p || !p->ctx || !A(1))
        RET((uint32_t)(int32_t)PARAM_ERR);
    CGContextSaveGState_(p->ctx);
    m32_wr(A(1), m32_handle(p->ctx));
    RET(NO_ERR);
}

static int qd_QDEndCGContext(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    QdPort *p = port_of(A(0));
    if (!p || !p->ctx)
        RET((uint32_t)(int32_t)PARAM_ERR);
    CGContextRestoreGState_(p->ctx);
    if (A(1))
        m32_wr(A(1), 0);
    port_changed(p);
    RET(NO_ERR);
}

static int qd_QDFlushPortBuffer(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; QdPort *p = port_of(A(0)); if (p) port_changed(p); RET(0); }
static int qd_True(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(1); }

/* ---- text state: kept, not drawn ---- */

static int qd_TextFont(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (g_cur) g_cur->font = (int16_t)A(0); RET(0); }
static int qd_TextFace(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (g_cur) g_cur->face = (uint8_t)A(0); RET(0); }
static int qd_TextSize(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (g_cur) g_cur->size = (int16_t)A(0); RET(0); }
static int qd_TextMode(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (g_cur) g_cur->mode = (int16_t)A(0); RET(0); }
static int qd_GetPortTextFont(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; QdPort *p = port_of(A(0)); RET(p ? (uint32_t)p->font : 0); }
static int qd_GetPortTextFace(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; QdPort *p = port_of(A(0)); RET(p ? p->face : 0); }
static int qd_GetPortTextSize(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; QdPort *p = port_of(A(0)); RET(p ? (uint32_t)p->size : 0); }
static int qd_GetPortTextMode(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; QdPort *p = port_of(A(0)); RET(p ? (uint32_t)p->mode : 0); }

/* ---- the mouse ---- */

static void global_mouse(int16_t *h, int16_t *v)
{
    CGEventRef e = CGEventCreate_(NULL);
    CGPoint at = e ? CGEventGetLocation_(e) : CGPointMake(0, 0);
    if (e)
        CFRelease_(e);
    *h = (int16_t)at.x;
    *v = (int16_t)at.y;
}

static int qd_GetGlobalMouse(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    int16_t h = 0, v = 0;
    if (ready())
        global_mouse(&h, &v);
    wr_point(A(0), h, v);
    RET(0);
}

static int qd_GetMouse(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    int16_t h = 0, v = 0;
    int dh = 0, dv = 0;
    if (ready()) {
        global_mouse(&h, &v);
        to_global(g_cur, &dh, &dv);
    }
    wr_point(A(0), (int16_t)(h - dh), (int16_t)(v - dv));
    RET(0);
}

/* ---- cursors ---- */

static id send0(id obj, const char *sel) { return ((id (*)(id, SEL))objc_msgSend)(obj, sel_registerName(sel)); }
static void set_named(const char *name) { id c = send0((id)objc_getClass("NSCursor"), name); if (c) send0(c, "set"); }

/* the classic arrow, as GetQDGlobalsArrow hands it out (16 rows of data, 16 of mask, the hot spot at (1, 1)) */
static const uint16_t g_arrow[34] = {
    0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7c00, 0x7e00, 0x7f00, 0x7f80, 0x7c00, 0x6c00, 0x4600, 0x0600, 0x0300,
    0x0300, 0x0000, 0xc000, 0xe000, 0xf000, 0xf800, 0xfc00, 0xfe00, 0xff00, 0xff80, 0xffc0, 0xffe0, 0xfe00, 0xef00,
    0xcf00, 0x8780, 0x0780, 0x0380, 0x0001, 0x0001,
};

/* GetCursor's system cursors, by id: iBeamCursor 1, crossCursor 2, plusCursor 3, watchCursor 4 */
static uint32_t g_sys_cursor[5];
static const char *const g_sys_name[5] = { NULL, "IBeamCursor", "crosshairCursor", "crosshairCursor", "arrowCursor" };

static int qd_GetQDGlobalsArrow(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    if (A(0))
        memcpy(m32_h(A(0)), g_arrow, CURSOR_BYTES);
    RET(A(0));
}

static int qd_GetCursor(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    int id = (int16_t)A(0);
    if (id < 1 || id > 4)
        RET(0);   /* a 'CURS' resource of the program's own */
    if (!g_sys_cursor[id]) {
        uint32_t c = m32_static_alloc(CURSOR_BYTES, 4), hnd = m32_static_alloc(4, 4);
        memcpy(m32_h(c), g_arrow, CURSOR_BYTES);
        m32_wr(hnd, c);
        g_sys_cursor[id] = hnd;
    }
    RET(g_sys_cursor[id]);
}

/* a cursor from a Cursor's bits: data and mask black, mask alone white, neither clear */
static id cursor_from_bits(uint32_t g)
{
    uint16_t bits[34];
    memcpy(bits, m32_h(g), sizeof bits);
    static uint8_t rgba[16 * 16 * 4];
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int d = bits[y] >> (15 - x) & 1, m = bits[16 + y] >> (15 - x) & 1;
            uint8_t *px = rgba + (y * 16 + x) * 4;
            uint8_t v = d ? 0 : 255;
            px[0] = px[1] = px[2] = (m || d) ? v : 0;
            px[3] = (m || d) ? 255 : 0;
        }
    CGDataProviderRef dp = CGDataProviderCreateWithData_(NULL, rgba, sizeof rgba, NULL);
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB_();
    CGImageRef img = CGImageCreate_(16, 16, 8, 32, 64, rgb, kCGImageAlphaLast | kCGBitmapByteOrderDefault, dp, NULL,
                                    false, kCGRenderingIntentDefault);
    CGColorSpaceRelease_(rgb);
    CGDataProviderRelease_(dp);
    if (!img)
        return NULL;
    id image = ((id (*)(id, SEL, CGImageRef, CGSize))objc_msgSend)(send0((id)objc_getClass("NSImage"), "alloc"),
                                                                   sel_registerName("initWithCGImage:size:"), img,
                                                                   CGSizeMake(16, 16));
    CGImageRelease_(img);
    CGPoint hot = CGPointMake((int16_t)bits[33], (int16_t)bits[32]);
    return ((id (*)(id, SEL, id, CGPoint))objc_msgSend)(send0((id)objc_getClass("NSCursor"), "alloc"),
                                                         sel_registerName("initWithImage:hotSpot:"), image, hot);
}

static int qd_SetCursor(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    uint32_t c = A(0);
    if (!c || !ready())
        RET(0);
    for (int i = 1; i <= 4; i++)
        if (g_sys_cursor[i] && m32_rd(g_sys_cursor[i]) == c) {
            set_named(g_sys_name[i]);
            RET(0);
        }
    if (!memcmp(m32_h(c), g_arrow, CURSOR_BYTES)) {
        set_named("arrowCursor");
        RET(0);
    }
    /* the last few made, by their bits: a program sets its cursor at every mouse move */
    static struct { uint8_t bits[CURSOR_BYTES]; id cursor; } made[8];
    static int next;
    for (int i = 0; i < 8; i++)
        if (made[i].cursor && !memcmp(made[i].bits, m32_h(c), CURSOR_BYTES)) {
            send0(made[i].cursor, "set");
            RET(0);
        }
    id cur = cursor_from_bits(c);
    if (cur) {
        if (made[next].cursor)
            send0(made[next].cursor, "release");
        memcpy(made[next].bits, m32_h(c), CURSOR_BYTES);
        made[next].cursor = cur;
        next = (next + 1) % 8;
        send0(cur, "set");
    }
    RET(0);
}

/* color cursors come from the program's 'crsr' resources, which m32 does not read: each is the arrow */
static int qd_GetCCursor(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    uint32_t c = m32_calloc(1, CCRSR_BYTES), hnd = m32_malloc(4);
    m32_wr(hnd, c);
    RET(hnd);
}
static int qd_SetCCursor(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (A(0) && ready()) set_named("arrowCursor"); RET(0); }
static int qd_DisposeCCursor(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    if (A(0)) {
        m32_free(m32_rd(A(0)));
        m32_free(A(0));
    }
    RET(0);
}

/* ---- graphics devices ---- */

static uint32_t g_devices;   /* the first GDHandle; each one's gdNextGD names the next */

static uint32_t devices(void)
{
    if (g_devices || !ready())
        return g_devices;
    CGDirectDisplayID ids[16];
    uint32_t n = 0;
    if (CGGetActiveDisplayList_(16, ids, &n) != kCGErrorSuccess || !n) {
        ids[0] = CGMainDisplayID_();
        n = 1;
    }
    CGDirectDisplayID main_id = CGMainDisplayID_();
    for (uint32_t i = 1; i < n; i++)   /* the main display first */
        if (ids[i] == main_id) {
            ids[i] = ids[0];
            ids[0] = main_id;
        }
    uint32_t prev = 0;
    for (uint32_t i = 0; i < n; i++) {
        CGRect b = CGDisplayBounds_(ids[i]);
        QdRect r = { (int16_t)b.origin.y, (int16_t)b.origin.x, (int16_t)(b.origin.y + b.size.height),
                     (int16_t)(b.origin.x + b.size.width) };
        uint32_t gd = m32_static_alloc(GDEVICE_BYTES + 4 /* the display id */, 4), hnd = m32_static_alloc(4, 4);
        uint32_t pm = m32_static_alloc(PIXMAP_BYTES, 4), pmh = m32_static_alloc(4, 4);
        m32_wr(hnd, gd);
        m32_wr(pmh, pm);
        wr16(pm + PM_ROWBYTES, (int16_t)0x8000);
        memcpy(m32_h(pm + PM_BOUNDS), &r, 8);
        m32_wr(pm + PM_HRES, 72 << 16);
        m32_wr(pm + PM_VRES, 72 << 16);
        wr16(pm + PM_TYPE, 16);
        wr16(pm + PM_SIZE, 32);
        wr16(pm + PM_CMPCOUNT, 3);
        wr16(pm + PM_CMPSIZE, 8);
        m32_wr(pm + PM_FORMAT, 32);
        wr16(gd + GD_TYPE, 2);   /* directType */
        /* gdDevType, ext32Device, screenDevice, screenActive, and mainScreen for the first */
        wr16(gd + GD_FLAGS, (int16_t)(1 << 0 | 1 << 8 | 1 << 13 | 1 << 15 | (i == 0 ? 1 << 11 : 0)));
        m32_wr(gd + GD_PMAP, pmh);
        memcpy(m32_h(gd + GD_RECT), &r, 8);
        m32_wr(gd + GDEVICE_BYTES, ids[i]);
        if (prev)
            m32_wr(m32_rd(prev) + GD_NEXT, hnd);
        else
            g_devices = hnd;
        prev = hnd;
    }
    if (!g_cur_gdev)
        g_cur_gdev = g_devices;
    return g_devices;
}

static int qd_GetMainDevice(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(devices()); }
static int qd_GetGDevice(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; devices(); RET(g_cur_gdev); }
static int qd_SetGDevice(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (A(0)) g_cur_gdev = A(0); RET(0); }
static int qd_GetNextDevice(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(A(0) && m32_rd(A(0)) ? m32_rd(m32_rd(A(0)) + GD_NEXT) : 0); }
static int qd_GetGDevPixMap(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(A(0) && m32_rd(A(0)) ? m32_rd(m32_rd(A(0)) + GD_PMAP) : 0); }
static int qd_TestDeviceAttribute(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    RET(A(0) && m32_rd(A(0)) && ((uint16_t)rd16(m32_rd(A(0)) + GD_FLAGS) >> ((int16_t)A(1) & 15) & 1));
}
static int qd_QDGetCGDirectDisplayID(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    for (uint32_t d = devices(); d; d = m32_rd(m32_rd(d) + GD_NEXT))
        if (d == A(0))
            RET(m32_rd(m32_rd(d) + GDEVICE_BYTES));
    RET(ready() ? CGMainDisplayID_() : 0);
}
static int qd_GetQDGlobalsScreenBits(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    uint32_t d = devices();
    if (A(0)) {
        memset(m32_h(A(0)), 0, 14);
        if (d)
            memcpy(m32_h(A(0) + PM_BOUNDS), m32_h(m32_rd(d) + GD_RECT), 8);
    }
    RET(A(0));
}

/* the display a GDHandle of ours stands for, else the main one */
static CGDirectDisplayID display_of(uint32_t gdh)
{
    for (uint32_t d = devices(); d; d = m32_rd(m32_rd(d) + GD_NEXT))
        if (d == gdh)
            return m32_rd(m32_rd(d) + GDEVICE_BYTES);
    return CGMainDisplayID_();
}

/* GetAvailableWindowPositioningBounds(GDHandle, Rect *): the display less its menu bar and Dock, in global
 * coordinates, as HIWindowGetAvailablePositioningBounds gives it; Kega Fusion sized its window from it, and from the
 * garbage an unanswered call left, AppKit refused to make the window's surface */
static int available_bounds(uint32_t gdh, QdRect *r)
{
    static OSStatus (*avail)(CGDirectDisplayID, int, CGRect *);
    if (!avail)
        avail = (OSStatus (*)(CGDirectDisplayID, int, CGRect *))dlsym(RTLD_DEFAULT, "HIWindowGetAvailablePositioningBounds");
    CGRect b;
    if (!ready() || !avail || avail(display_of(gdh), 1 /* kHICoordSpace72DPIGlobal */, &b) != noErr)
        return PARAM_ERR;
    *r = (QdRect){ (int16_t)b.origin.y, (int16_t)b.origin.x, (int16_t)(b.origin.y + b.size.height),
                   (int16_t)(b.origin.x + b.size.width) };
    return NO_ERR;
}

static int qd_GetAvailableWindowPositioningBounds(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    QdRect r;
    int err = available_bounds(A(0), &r);
    if (!err)
        wr_rect(A(1), r);
    RET((uint32_t)(int32_t)err);
}

static int qd_GetAvailableWindowPositioningRegion(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    QdRect r;
    int err = available_bounds(A(0), &r);
    if (!err && A(1))
        RectRgn_(m32_host(A(1)), &r);
    RET((uint32_t)(int32_t)err);
}

static int qd_SysBeep(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (ready() && NSBeep_) NSBeep_(); RET(0); }

const M32SpecialEntry m32_qd_specials[] = {
    { "_GetWindowPort", qd_GetWindowPort }, { "_SetPortWindowPort", qd_SetPortWindowPort },
    { "_GetWindowFromPort", qd_GetWindowFromPort }, { "_IsValidPort", qd_IsValidPort },
    { "_GetPort", qd_GetPort }, { "_SetPort", qd_SetPort }, { "_MacSetPort", qd_SetPort }, { "_QDSwapPort", qd_QDSwapPort },
    { "_GetQDGlobalsThePort", qd_GetQDGlobalsThePort }, { "_GetPortBounds", qd_GetPortBounds },
    { "_SetOrigin", qd_SetOrigin }, { "_LocalToGlobal", qd_LocalToGlobal }, { "_GlobalToLocal", qd_GlobalToLocal },
    { "_GetClip", qd_GetClip }, { "_SetClip", qd_SetClip }, { "_ClipRect", qd_ClipRect },
    { "_GetPortClipRegion", qd_GetPortClipRegion }, { "_SetPortClipRegion", qd_SetPortClipRegion },
    { "_GetPortVisibleRegion", qd_GetPortVisibleRegion },
    { "_NewGWorld", qd_NewGWorld }, { "_NewGWorldFromPtr", qd_NewGWorldFromPtr }, { "_UpdateGWorld", qd_UpdateGWorld },
    { "_DisposeGWorld", qd_DisposeGWorld }, { "_GetGWorld", qd_GetGWorld }, { "_SetGWorld", qd_SetGWorld },
    { "_GetGWorldPixMap", qd_GetGWorldPixMap }, { "_GetPortPixMap", qd_GetGWorldPixMap },
    { "_GetPortBitMapForCopyBits", qd_GetPortBitMapForCopyBits },
    { "_LockPixels", qd_LockPixels }, { "_UnlockPixels", qd_Ok0 }, { "_LockPortBits", qd_Ok0 },
    { "_UnlockPortBits", qd_Ok0 }, { "_GetPixBaseAddr", qd_GetPixBaseAddr }, { "_GetPixRowBytes", qd_GetPixRowBytes },
    { "_GetPixDepth", qd_GetPixDepth },
    { "_ForeColor", qd_ForeColor }, { "_BackColor", qd_BackColor }, { "_RGBForeColor", qd_RGBForeColor },
    { "_RGBBackColor", qd_RGBBackColor }, { "_GetForeColor", qd_GetForeColor }, { "_GetBackColor", qd_GetBackColor },
    { "_PenSize", qd_PenSize }, { "_PenNormal", qd_PenNormal }, { "_GetPen", qd_GetPen },
    { "_MoveTo", qd_MoveTo }, { "_Move", qd_Move }, { "_LineTo", qd_LineTo }, { "_Line", qd_Line },
    { "_MacLineTo", qd_LineTo },
    { "_PaintRect", qd_PaintRect }, { "_EraseRect", qd_EraseRect }, { "_InvertRect", qd_InvertRect },
    { "_FrameRect", qd_FrameRect }, { "_MacFrameRect", qd_FrameRect }, { "_PaintRgn", qd_PaintRgn },
    { "_MacPaintRgn", qd_PaintRgn }, { "_EraseRgn", qd_EraseRgn },
    { "_CopyBits", qd_CopyBits },
    { "_QDBeginCGContext", qd_QDBeginCGContext }, { "_QDEndCGContext", qd_QDEndCGContext },
    { "_QDFlushPortBuffer", qd_QDFlushPortBuffer }, { "_QDIsPortBuffered", qd_True },
    { "_TextFont", qd_TextFont }, { "_TextFace", qd_TextFace }, { "_TextSize", qd_TextSize }, { "_TextMode", qd_TextMode },
    { "_GetPortTextFont", qd_GetPortTextFont }, { "_GetPortTextFace", qd_GetPortTextFace },
    { "_GetPortTextSize", qd_GetPortTextSize }, { "_GetPortTextMode", qd_GetPortTextMode },
    { "_GetMouse", qd_GetMouse }, { "_GetGlobalMouse", qd_GetGlobalMouse },
    { "_GetQDGlobalsArrow", qd_GetQDGlobalsArrow }, { "_GetCursor", qd_GetCursor }, { "_SetCursor", qd_SetCursor },
    { "_MacSetCursor", qd_SetCursor }, { "_GetCCursor", qd_GetCCursor }, { "_SetCCursor", qd_SetCCursor },
    { "_DisposeCCursor", qd_DisposeCCursor },
    { "_GetMainDevice", qd_GetMainDevice }, { "_GetDeviceList", qd_GetMainDevice }, { "_GetGDevice", qd_GetGDevice },
    { "_SetGDevice", qd_SetGDevice }, { "_GetNextDevice", qd_GetNextDevice }, { "_GetGDevPixMap", qd_GetGDevPixMap },
    { "_TestDeviceAttribute", qd_TestDeviceAttribute }, { "_QDGetCGDirectDisplayID", qd_QDGetCGDirectDisplayID },
    { "_GetQDGlobalsScreenBits", qd_GetQDGlobalsScreenBits },
    { "_GetAvailableWindowPositioningBounds", qd_GetAvailableWindowPositioningBounds },
    { "_GetAvailableWindowPositioningRegion", qd_GetAvailableWindowPositioningRegion },
    { "_SysBeep", qd_SysBeep }, { "_KillPicture", qd_Ok0 },
    { NULL, NULL }
};

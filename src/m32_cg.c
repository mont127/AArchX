/*
 * CoreGraphics calls whose i386 shapes the notation cannot carry (include/ocerz/m32.h).  Firefox 3.6 draws its
 * gradients, patterns and glyph outlines with them, through cairo's Quartz backend.
 *
 *   callbacks  CGFunctionCreate's and CGPatternCreate's callback structures are {version, two functions}, three
 *              words on i386 and three doublewords on arm64: crossed, CoreGraphics read a float of the guest's domain
 *              as the evaluate function and jumped to it.  The host gets m32's own callbacks, which call the guest's
 *              as guest code; a function's input and output values are floats there and doubles here.
 *   paths      CGPathApply hands its applier each element as {type, points}, the points floats on i386: crossed, the
 *              applier got the host's element, outside guest memory.
 *   private    two calls cairo declares itself, which no SDK does: CGContextSetCompositeOperation, whose Porter-Duff
 *              operators are CGContextSetBlendMode's from kCGBlendModeClear on, and CGContextSetCTM, a
 *              concatenation of the transform that takes the current one to the one asked for.
 */
#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "ocerz/interp.h"
#include "ocerz/m32.h"

#define RET(v) do { m32_ret(cpu, (uint32_t)(v), 0, 0); return OCERZ_STEP_OK; } while (0)
#define A(i) m32_arg(cpu, i)

/* ---- the host functions, looked up once (ocerz links no frameworks) ---- */

#define HOST(fn) static __typeof__(fn) *fn##_
HOST(CGFunctionCreate); HOST(CGPatternCreate); HOST(CGPathApply); HOST(CGContextSetBlendMode);
HOST(CGContextGetCTM); HOST(CGContextConcatCTM); HOST(CGAffineTransformConcat); HOST(CGAffineTransformInvert);

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_ok;
static void load(void)
{
    void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY | RTLD_LOCAL);
    int ok = cg != NULL;
#define L(fn) ok &= (fn##_ = cg ? (__typeof__(fn) *)dlsym(cg, #fn) : NULL) != NULL
    L(CGFunctionCreate); L(CGPatternCreate); L(CGPathApply); L(CGContextSetBlendMode); L(CGContextGetCTM);
    L(CGContextConcatCTM); L(CGAffineTransformConcat); L(CGAffineTransformInvert);
#undef L
    g_ok = ok;
}
static int ready(void)
{
    pthread_once(&g_once, load);
    return g_ok;
}

static float word_float(uint32_t w)
{
    float f;
    memcpy(&f, &w, 4);
    return f;
}

/* ---- CGFunctionCreate(void *info, size_t domainDimension, const CGFloat *domain, size_t rangeDimension,
 *      const CGFloat *range, const CGFunctionCallbacks *callbacks) ---- */

typedef struct Fn {
    struct OcerzVM *vm;
    uint32_t info, evaluate, release, buf;   /* buf: the input floats, then the output's */
    size_t nin, nout;
    pthread_mutex_t lock;                    /* one buf, and CoreGraphics may evaluate on any thread */
} Fn;

static void fn_evaluate(void *ctx, const CGFloat *in, CGFloat *out)
{
    Fn *f = ctx;
    pthread_mutex_lock(&f->lock);
    float *g = m32_h(f->buf);
    for (size_t i = 0; i < f->nin; i++)
        g[i] = (float)in[i];
    memset(g + f->nin, 0, 4 * f->nout);
    m32_call(f->vm, f->evaluate, (const uint32_t[]){ f->info, f->buf, f->buf + 4 * (uint32_t)f->nin }, 3, NULL, NULL);
    for (size_t i = 0; i < f->nout; i++)
        out[i] = g[f->nin + i];
    pthread_mutex_unlock(&f->lock);
}

static void fn_release(void *ctx)
{
    Fn *f = ctx;
    if (f->release)
        m32_call(f->vm, f->release, &f->info, 1, NULL, NULL);
    m32_free(f->buf);
    pthread_mutex_destroy(&f->lock);
    free(f);
}

static int sp_CGFunctionCreate(struct OcerzVM *vm, OcerzCPU *cpu)
{
    enum { MAX = 32 };
    uint32_t nin = A(1), nout = A(3), cbs = A(5);
    if (!ready() || !cbs || !m32_rd(cbs + 4) || nin > MAX || nout > MAX)
        RET(0);
    CGFloat domain[2 * MAX], range[2 * MAX];
    for (uint32_t i = 0; A(2) && i < 2 * nin; i++)
        domain[i] = word_float(m32_rd(A(2) + 4 * i));
    for (uint32_t i = 0; A(4) && i < 2 * nout; i++)
        range[i] = word_float(m32_rd(A(4) + 4 * i));
    Fn *f = calloc(1, sizeof *f);
    if (!f || !(f->buf = m32_malloc(4 * (nin + nout) + 4))) {
        free(f);
        RET(0);
    }
    f->vm = vm;
    f->info = A(0);
    f->evaluate = m32_rd(cbs + 4);
    f->release = m32_rd(cbs + 8);
    f->nin = nin;
    f->nout = nout;
    pthread_mutex_init(&f->lock, NULL);
    static const CGFunctionCallbacks hc = { 0, fn_evaluate, fn_release };
    CGFunctionRef r = CGFunctionCreate_(f, nin, A(2) ? domain : NULL, nout, A(4) ? range : NULL, &hc);
    if (!r) {   /* nothing will release it */
        m32_free(f->buf);
        pthread_mutex_destroy(&f->lock);
        free(f);
    }
    RET(m32_handle((void *)r));
}

/* ---- CGPatternCreate(void *info, CGRect bounds, CGAffineTransform matrix, CGFloat xStep, CGFloat yStep,
 *      CGPatternTiling, bool isColored, const CGPatternCallbacks *callbacks): words 0, 1-4, 5-10, 11, 12, 13, 14, 15 ---- */

typedef struct Pattern { struct OcerzVM *vm; uint32_t info, draw, release; } Pattern;

static void pattern_draw(void *ctx, CGContextRef c)
{
    Pattern *p = ctx;
    if (p->draw)
        m32_call(p->vm, p->draw, (const uint32_t[]){ p->info, m32_handle(c) }, 2, NULL, NULL);
}

static void pattern_release(void *ctx)
{
    Pattern *p = ctx;
    if (p->release)
        m32_call(p->vm, p->release, &p->info, 1, NULL, NULL);
    free(p);
}

static int sp_CGPatternCreate(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t cbs = A(15);
    Pattern *p = ready() && cbs ? calloc(1, sizeof *p) : NULL;
    if (!p)
        RET(0);
    *p = (Pattern){ vm, A(0), m32_rd(cbs + 4), m32_rd(cbs + 8) };
    CGRect bounds = CGRectMake(word_float(A(1)), word_float(A(2)), word_float(A(3)), word_float(A(4)));
    CGAffineTransform m = { word_float(A(5)), word_float(A(6)), word_float(A(7)), word_float(A(8)),
                            word_float(A(9)), word_float(A(10)) };
    static const CGPatternCallbacks hc = { 0, pattern_draw, pattern_release };
    CGPatternRef r = CGPatternCreate_(p, bounds, m, word_float(A(11)), word_float(A(12)), (CGPatternTiling)A(13),
                                      (A(14) & 0xff) != 0, &hc);
    if (!r)
        free(p);
    RET(m32_handle((void *)r));
}

/* ---- CGPathApply(CGPathRef, void *info, CGPathApplierFunction): each element copied into a guest one, 8 bytes and
 *      then its points, up to 3 ---- */

typedef struct PathWalk { struct OcerzVM *vm; uint32_t info, fn, el; } PathWalk;

static void path_element(void *ctx, const CGPathElement *el)
{
    const PathWalk *w = ctx;
    int n = el->type == kCGPathElementAddQuadCurveToPoint ? 2 : el->type == kCGPathElementAddCurveToPoint ? 3
          : el->type == kCGPathElementCloseSubpath ? 0 : 1;
    float *pts = m32_h(w->el + 8);
    for (int i = 0; i < n; i++) {
        pts[2 * i] = (float)el->points[i].x;
        pts[2 * i + 1] = (float)el->points[i].y;
    }
    m32_wr(w->el, (uint32_t)el->type);
    m32_wr(w->el + 4, w->el + 8);
    m32_call(w->vm, w->fn, (const uint32_t[]){ w->info, w->el }, 2, NULL, NULL);
}

static int sp_CGPathApply(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *path = ready() ? m32_host(A(0)) : NULL;
    PathWalk w = { vm, A(1), A(2), 0 };
    if (!path || !w.fn || !(w.el = m32_malloc(8 + 3 * 8)))
        RET(0);
    CGPathApply_((CGPathRef)path, &w, path_element);
    m32_free(w.el);
    RET(0);
}

/* ---- the private calls ---- */

/* CGContextSetCompositeOperation(CGContextRef, int op): cairo's operators 0 clear, 1 copy, 2 source over, then source
 * in, source out, source atop, destination over, in, out, atop, XOR, plus darker and plus lighter (12) */
static int sp_CGContextSetCompositeOperation(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    void *c = ready() ? m32_host(A(0)) : NULL;
    uint32_t op = A(1);
    if (c && op <= 12)
        CGContextSetBlendMode_((CGContextRef)c, op == 2 ? kCGBlendModeNormal
                                              : op < 2 ? (CGBlendMode)(kCGBlendModeClear + op)
                                              : (CGBlendMode)(kCGBlendModeSourceIn + op - 3));
    RET(0);
}

/* CGContextSetCTM(CGContextRef, CGAffineTransform): words 1-6 */
static int sp_CGContextSetCTM(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    void *c = ready() ? m32_host(A(0)) : NULL;
    if (!c)
        RET(0);
    CGAffineTransform want = { word_float(A(1)), word_float(A(2)), word_float(A(3)), word_float(A(4)),
                               word_float(A(5)), word_float(A(6)) };
    CGAffineTransform now = CGContextGetCTM_((CGContextRef)c);
    CGContextConcatCTM_((CGContextRef)c, CGAffineTransformConcat_(want, CGAffineTransformInvert_(now)));
    RET(0);
}

const M32SpecialEntry m32_cg_specials[] = {
    { "_CGFunctionCreate", sp_CGFunctionCreate }, { "_CGPatternCreate", sp_CGPatternCreate },
    { "_CGPathApply", sp_CGPathApply }, { "_CGContextSetCompositeOperation", sp_CGContextSetCompositeOperation },
    { "_CGContextSetCTM", sp_CGContextSetCTM },
    { NULL, NULL }
};

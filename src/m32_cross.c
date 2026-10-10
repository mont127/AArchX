/*
 * m32's generic crossing from an i386 cdecl call to an arm64 AAPCS64 one (include/ocerz/m32.h).
 *
 * A function's two notations (runtime/apis32) are parsed once: the guest one with i386 sizes (4-byte long and
 * pointer, 8-byte members aligned to 4 in structures), the host one with arm64 sizes.  Arguments are read from the
 * guest stack in order, converted class by class and placed the way Apple's arm64 ABI places them: integers in
 * x0-x7, floating point in v0-v7, a homogeneous float/double aggregate of up to four members in consecutive v
 * registers, another structure of at most 16 bytes in one or two x registers, a larger one by reference, and
 * anything that no longer fits on the stack at its natural alignment, packed.  The result comes back in EAX,
 * EDX:EAX, ST0, or through the hidden structure pointer, which the i386 callee pops.
 *
 * Guest-only classes: s is a C string (a host result outside the window is copied into it), P one pointer written
 * through a pointer, W/V one signed/unsigned long written through a pointer, Q a pointer to data whose layouts
 * differ (only a handle may pass; guest memory there needs a special).
 */
#include <malloc/malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ocerz/abi.h"
#include "ocerz/bridge.h"
#include "ocerz/interp.h"
#include "ocerz/m32.h"
#include "ocerz/m32_sig.h"
#include "ocerz/m32_objc.h"
#include "ocerz/types.h"

static int scalar_size(char c, int guest)
{
    switch (c) {
    case 'v': return 0;
    case 'b': case 'B': return 1;
    case 'h': case 'H': return 2;
    case 'i': case 'u': case 'f': return 4;
    case 'l': case 'L': case 'T': case 'd': return 8;
    case 'p': case 's': case 'P': case 'Q': case 'W': case 'V': case 'c': case 'k': case '@': case '#': case ':':
        return guest ? 4 : 8;
    default: return -1;
    }
}

static const char *parse_type(const char *s, int guest, M32Type *t);

/* flatten one structure's members into t, laying them out for the guest or the host */
static const char *parse_members(const char *s, int guest, M32Type *t, uint32_t *off, uint32_t *align)
{
    while (*s && *s != '}') {
        if (*s == '{') {
            uint32_t inner_off = 0, inner_align = 1;
            M32Type sub = { 0 };
            const char *e = parse_members(s + 1, guest, &sub, &inner_off, &inner_align);
            if (!e || *e != '}')
                return NULL;
            *off = (*off + inner_align - 1) & ~(inner_align - 1);
            for (int i = 0; i < sub.n; i++) {
                if (t->n >= M32_MAX_MEMBERS)
                    return NULL;
                t->mcls[t->n] = sub.mcls[i];
                t->msize[t->n] = sub.msize[i];
                t->moff[t->n] = (uint16_t)(*off + sub.moff[i]);
                t->n++;
            }
            *off += (inner_off + inner_align - 1) & ~(inner_align - 1);
            if (inner_align > *align)
                *align = inner_align;
            s = e + 1;
            continue;
        }
        char c = *s;
        if (c == 'c' || c == 'k') {   /* a function pointer or block inside a structure: a pointer */
            M32Type tmp;
            s = parse_type(s, guest, &tmp);
            if (!s)
                return NULL;
            free(tmp.cb);
            c = 'p';
        } else
            s++;
        int size = scalar_size(c, guest);
        if (size <= 0 || t->n >= M32_MAX_MEMBERS)
            return NULL;
        uint32_t a = guest && size == 8 ? 4 : (uint32_t)size;
        *off = (*off + a - 1) & ~(a - 1);
        t->mcls[t->n] = c;
        t->msize[t->n] = (uint8_t)size;
        t->moff[t->n] = (uint16_t)*off;
        t->n++;
        *off += (uint32_t)size;
        if (a > *align)
            *align = a;
    }
    return s;
}

static const char *parse_type(const char *s, int guest, M32Type *t)
{
    memset(t, 0, sizeof *t);
    if (*s == '{') {
        uint32_t off = 0, align = 1;
        const char *e = parse_members(s + 1, guest, t, &off, &align);
        if (!e || *e != '}')
            return NULL;
        t->cls = '{';
        t->align = (uint8_t)align;
        t->size = (uint8_t)((off + align - 1) & ~(align - 1));
        return e + 1;
    }
    if ((*s == 'R' || *s == 'r') && s[1] == '{') {   /* a pointer to a structure whose layouts differ */
        uint32_t off = 0, align = 1;
        const char *e = parse_members(s + 2, guest, t, &off, &align);
        if (!e || *e != '}')
            return NULL;
        t->cls = *s;
        t->ssize = (uint16_t)((off + align - 1) & ~(align - 1));
        t->size = t->align = (uint8_t)(guest ? 4 : 8);
        return e + 1;
    }
    if ((*s == 'c' || *s == 'k') && s[1] == '{') {
        int depth = 0;
        const char *p = s + 1;
        do {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            p++;
        } while (*p && depth);
        if (depth)
            return NULL;
        t->cls = *s;
        t->cb = strndup(s + 2, (size_t)(p - s - 3));
        t->size = t->align = (uint8_t)scalar_size(*s, guest);
        return p;
    }
    int size = scalar_size(*s, guest);
    if (size < 0)
        return NULL;
    t->cls = *s;
    t->size = (uint8_t)size;
    t->align = (uint8_t)(guest && size == 8 ? 4 : size);
    return s + 1;
}

M32Sig *m32_sig_parse(const char *notation, int guest)
{
    M32Sig *g = calloc(1, sizeof *g);
    const char *s = parse_type(notation, guest, &g->ret);
    if (!s || *s != '(')
        goto bad;
    s++;
    while (*s && *s != ')') {
        if (g->nargs >= 16 || !(s = parse_type(s, guest, &g->arg[g->nargs++])))
            goto bad;
    }
    if (*s != ')')
        goto bad;
    return g;
bad:
    free(g);
    return NULL;
}

/* ---- host placement ---- */

typedef struct HostCall {
    uint64_t x[8], v[8];
    uint8_t stack[512];
    uint32_t sp;
    int nx, nv;
    uint8_t scratch[1024];    /* structures passed by reference, P/W/V cells */
    uint32_t scratch_used;
} HostCall;

static void put_stack(HostCall *h, const void *p, uint32_t size, uint32_t align)
{
    h->sp = (h->sp + align - 1) & ~(align - 1);
    if (h->sp + size <= sizeof h->stack)
        memcpy(h->stack + h->sp, p, size);
    h->sp += size;
}

static void put_int(HostCall *h, uint64_t v, uint32_t size)
{
    if (h->nx < 8)
        h->x[h->nx++] = v;
    else
        put_stack(h, &v, size, size);   /* little-endian: the low bytes come first */
}

static void put_fp(HostCall *h, uint64_t bits, uint32_t size)
{
    if (h->nv < 8)
        h->v[h->nv++] = bits;
    else
        put_stack(h, &bits, size, size);
}

static void *scratch(HostCall *h, uint32_t size)
{
    uint32_t at = (h->scratch_used + 15) & ~15u;
    if (at + size > sizeof h->scratch)
        return NULL;
    h->scratch_used = at + size;
    memset(h->scratch + at, 0, size);
    return h->scratch + at;
}

int m32_is_hfa(const M32Type *t)
{
    if (t->cls != '{' || t->n == 0 || t->n > 4 || (t->mcls[0] != 'f' && t->mcls[0] != 'd'))
        return 0;
    for (int i = 1; i < t->n; i++)
        if (t->mcls[i] != t->mcls[0])
            return 0;
    return 1;
}

static void put_struct(HostCall *h, const uint8_t *buf, const M32Type *t)
{
    if (m32_is_hfa(t)) {
        if (h->nv + t->n <= 8) {
            for (int i = 0; i < t->n; i++) {
                uint64_t bits = 0;
                memcpy(&bits, buf + t->moff[i], t->msize[i]);
                h->v[h->nv++] = bits;
            }
        } else {
            h->nv = 8;
            put_stack(h, buf, t->size, t->align);
        }
        return;
    }
    if (t->size <= 16) {
        uint32_t words = (t->size + 7) / 8;
        uint64_t w[2] = { 0, 0 };
        memcpy(w, buf, t->size);
        if (h->nx + (int)words <= 8) {
            for (uint32_t i = 0; i < words; i++)
                h->x[h->nx++] = w[i];
        } else {
            h->nx = 8;
            put_stack(h, w, words * 8, t->align > 8 ? t->align : 8);
        }
        return;
    }
    void *copy = scratch(h, t->size);
    if (copy)
        memcpy(copy, buf, t->size);
    put_int(h, (uint64_t)(uintptr_t)copy, 8);
}

/* ---- conversions ---- */

static double fbits(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static double dbits(uint64_t v) { double d; memcpy(&d, &v, 8); return d; }
static uint64_t bitsd(double d) { uint64_t v; memcpy(&v, &d, 8); return v; }
static uint32_t bitsf(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }

/* one scalar value from guest class g to host class h */
uint64_t m32_scalar_in(char g, char h, uint64_t raw, const char *fn, int argno)
{
    switch (g) {
    case 'b': raw = (uint64_t)(int64_t)(int8_t)raw; break;
    case 'B': raw = (uint8_t)raw; break;
    case 'h': raw = (uint64_t)(int64_t)(int16_t)raw; break;
    case 'H': raw = (uint16_t)raw; break;
    case 'i': raw = (uint64_t)(int64_t)(int32_t)raw; break;
    case 'u': raw = (uint32_t)raw; break;
    case 'f':
        return h == 'd' ? bitsd(fbits((uint32_t)raw)) : (uint32_t)raw;
    case 'd':
        return h == 'f' ? bitsf((float)dbits(raw)) : raw;
    case 'T':
        return ocerz_abi_dtime_to_host(raw);
    case 'p': case 's': case 'c':
        return (uint64_t)(uintptr_t)m32_host((uint32_t)raw);
    case '@': return (uint64_t)(uintptr_t)m32_objc_to_host((uint32_t)raw);
    case '#': return (uint64_t)(uintptr_t)m32_class_host((uint32_t)raw);
    case ':': return (uint64_t)(uintptr_t)m32_sel_host((uint32_t)raw);
    case 'k': return (uint64_t)(uintptr_t)m32_block_to_host((uint32_t)raw, NULL, NULL);
    case 'Q':
        if (raw && m32_host((uint32_t)raw) == m32_h((uint32_t)raw)) {   /* guest memory, not a handle or alias */
            char what[96];
            snprintf(what, sizeof what, "argument %d points at guest data whose 32/64-bit layouts differ (needs a special):", argno);
            m32_log_once(what, fn);
        }
        return (uint64_t)(uintptr_t)m32_host((uint32_t)raw);
    default:
        break;
    }
    return raw;
}

/* one scalar value from host class h back to guest class g, as the bits the guest stores */
uint64_t m32_scalar_out(char g, char h, uint64_t raw)
{
    switch (g) {
    case 'f':
        return h == 'd' ? bitsf((float)dbits(raw)) : (uint32_t)raw;
    case 'd':
        return h == 'f' ? bitsd(fbits((uint32_t)raw)) : raw;
    case 'T':
        return ocerz_abi_dtime_to_guest(raw);
    case 'p': case 'Q': case 'c':
        return m32_handle((void *)(uintptr_t)raw);
    case '@': return m32_objc_to_guest((void *)(uintptr_t)raw);
    case '#': return m32_class_guest((void *)(uintptr_t)raw);
    case ':': return m32_sel_guest((void *)(uintptr_t)raw);
    case 'k': return m32_block_to_guest((void *)(uintptr_t)raw);
    case 's':
        return m32_cstring((const char *)(uintptr_t)raw);
    default:
        return raw;
    }
}

static void struct_in(const M32Type *g, const M32Type *h, const uint8_t *gbuf, uint8_t *hbuf, const char *fn, int argno)
{
    for (int i = 0; i < g->n && i < h->n; i++) {
        uint64_t raw = 0;
        memcpy(&raw, gbuf + g->moff[i], g->msize[i]);
        uint64_t v = m32_scalar_in(g->mcls[i], h->mcls[i], raw, fn, argno);
        memcpy(hbuf + h->moff[i], &v, h->msize[i]);
    }
}

static void struct_out(const M32Type *g, const M32Type *h, const uint8_t *hbuf, uint8_t *gbuf)
{
    for (int i = 0; i < g->n && i < h->n; i++) {
        uint64_t raw = 0;
        memcpy(&raw, hbuf + h->moff[i], h->msize[i]);
        uint64_t v = m32_scalar_out(g->mcls[i], h->mcls[i], raw);
        memcpy(gbuf + g->moff[i], &v, g->msize[i]);
    }
}

/* i386 Darwin returns structures of 1, 2, 4 and 8 bytes in registers, one whose only member is a float or a
 * double in ST0, and every other one through the hidden pointer */
int m32_struct_in_regs(const M32Type *t)
{
    return t->size == 1 || t->size == 2 || t->size == 4 || t->size == 8;
}

typedef struct Writeback {
    char cls;
    uint32_t guest;           /* where the guest's value lives */
    uint64_t *cell;           /* the host cell the callee wrote */
    const M32Type *gt, *ht;   /* R: the structure's two layouts */
} Writeback;

static pthread_mutex_t g_sig_lock = PTHREAD_MUTEX_INITIALIZER;

/* OCERZ_M32LOG=glerr: the GL errors each GL call leaves, and why a shader or program did not build.  Diagnosis only:
 * it drains the error flag a game would read itself.  ponytail: stops after 5000 lines. */
static void log_glerr(const char *name, const uint64_t *x, uint32_t caller)
{
    static unsigned (*get_error)(void);
    static void (*getiv)(unsigned, unsigned, int *), (*get_integerv)(unsigned, int *);
    static void (*info_log)(unsigned, int, int *, char *);
    static const char *(*get_string)(unsigned);
    static int printed;
    static __thread int in_begin;   /* glGetError between glBegin and glEnd is itself an error */
    if (!strcmp(name, "_glBegin") || !strcmp(name, "_glEnd") || !strcmp(name, "_glGetError")) {
        in_begin = !strcmp(name, "_glBegin");
        return;
    }
    if (in_begin || printed > 5000)
        return;
    if (!get_error) {
        const char *lib = "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL";
        get_string = (const char *(*)(unsigned))ocerz_bridge_host_symbol(lib, "glGetString");
        get_integerv = (void (*)(unsigned, int *))ocerz_bridge_host_symbol(lib, "glGetIntegerv");
        get_error = (unsigned (*)(void))ocerz_bridge_host_symbol(lib, "glGetError");
    }
    if (!get_error)
        return;
    int compile = !strcmp(name, "_glCompileShader"), link = !strcmp(name, "_glLinkProgram");
    if (compile || link) {
        const char *lib = "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL";
        getiv = (void (*)(unsigned, unsigned, int *))ocerz_bridge_host_symbol(lib, compile ? "glGetShaderiv" : "glGetProgramiv");
        info_log = (void (*)(unsigned, int, int *, char *))ocerz_bridge_host_symbol(lib, compile ? "glGetShaderInfoLog" : "glGetProgramInfoLog");
        int ok = 1;
        char buf[2048] = "";
        getiv((unsigned)x[0], compile ? 0x8B81 : 0x8B82, &ok);   /* GL_COMPILE_STATUS, GL_LINK_STATUS */
        if (!ok) {
            info_log((unsigned)x[0], sizeof buf, NULL, buf);
            fprintf(stderr, "ocerz: m32: gl %s %u failed <- %#x: %s\n", name + 1, (unsigned)x[0], caller, buf);
            printed++;
        }
    }
    for (unsigned e, n = 0; n < 4 && (e = get_error()) != 0; n++) {
        fprintf(stderr, "ocerz: m32: gl error %#x after %s <- %#x args %#llx %#llx %#llx\n", e, name + 1, caller,
                (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[2]);
        if (!strcmp(name, "_glProgramStringARB")) {
            int pos = -1;
            get_integerv(0x864B, &pos);   /* GL_PROGRAM_ERROR_POSITION_ARB */
            fprintf(stderr, "ocerz: m32:   program error at %d: %s\n", pos, get_string(0x8874));
        }
        printed++;
    }
}

/* OCERZ_M32LOG=glnan: shader constants that are not finite or absurdly large (a translated math bug shows up here
 * first: the 3D scene draws nothing while the 2D UI is fine).  ponytail: stops after 2000 lines. */
static void log_glnan(const char *name, const uint64_t *x, uint32_t caller)
{
    static int printed;
    const float *v = NULL;
    uint64_t n = 0;
    if (!strcmp(name, "_glProgramLocalParameters4fvEXT") || !strcmp(name, "_glProgramEnvParameters4fvEXT"))
        v = (const float *)(uintptr_t)x[3], n = 4 * x[2];
    else if (!strcmp(name, "_glProgramEnvParameter4fvARB") || !strcmp(name, "_glProgramLocalParameter4fvARB"))
        v = (const float *)(uintptr_t)x[2], n = 4;
    else if (!strcmp(name, "_glUniform4fv"))
        v = (const float *)(uintptr_t)x[2], n = 4 * x[1];
    else if (!strcmp(name, "_glUniformMatrix4fv"))
        v = (const float *)(uintptr_t)x[3], n = 16 * x[1];
    if (!v || printed > 2000)
        return;
    for (uint64_t i = 0; i < n && i < 4096; i++)
        if (!__builtin_isfinite(v[i]) || __builtin_fabsf(v[i]) > 1e30f) {
            fprintf(stderr, "ocerz: m32: gl %s index %llu: [%llu] = %g (%g %g %g %g) <- %#x\n", name + 1,
                    (unsigned long long)x[1], (unsigned long long)i, v[i], v[i & ~3ull], v[(i & ~3ull) + 1],
                    v[(i & ~3ull) + 2], v[(i & ~3ull) + 3], caller);
            printed++;
            return;
        }
}

/* OCERZ_M32LOG=framedump: every 5 seconds, the frame CGLFlushDrawable is about to show, read back from the default
 * framebuffer's back buffer (the viewport's extent) into $OCERZ_M32_FRAMEDIR (default /tmp)/ocerz-frame-<n>.tga, so a
 * frame can be looked at without screen capture.  Called before the flush, on the flushing thread with its context
 * current. */
static void dump_frame(void)
{
    static uint64_t next;
    static int n;
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now < next)
        return;
    next = now + 5000000000ull;
    const char *lib = "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL";
    void (*getiv)(unsigned, int *) = (void (*)(unsigned, int *))ocerz_bridge_host_symbol(lib, "glGetIntegerv");
    void (*bind)(unsigned, unsigned) = (void (*)(unsigned, unsigned))ocerz_bridge_host_symbol(lib, "glBindFramebuffer");
    void (*read_buffer)(unsigned) = (void (*)(unsigned))ocerz_bridge_host_symbol(lib, "glReadBuffer");
    void (*pixel_store)(unsigned, int) = (void (*)(unsigned, int))ocerz_bridge_host_symbol(lib, "glPixelStorei");
    void (*read_pixels)(int, int, int, int, unsigned, unsigned, void *) =
        (void (*)(int, int, int, int, unsigned, unsigned, void *))ocerz_bridge_host_symbol(lib, "glReadPixels");
    if (!getiv || !bind || !read_buffer || !pixel_store || !read_pixels)
        return;
    int fb = 0, rb = 0, pack = 4, vp[4] = { 0 };
    getiv(0x8CAA, &fb);   /* GL_READ_FRAMEBUFFER_BINDING */
    getiv(0x0C02, &rb);   /* GL_READ_BUFFER */
    getiv(0x0D05, &pack); /* GL_PACK_ALIGNMENT */
    getiv(0x0BA2, vp);    /* GL_VIEWPORT */
    int w = vp[2], h = vp[3];
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        fprintf(stderr, "ocerz: m32: frame not dumped: viewport %d,%d %dx%d\n", vp[0], vp[1], w, h);
        return;
    }
    uint8_t *px = malloc((size_t)w * h * 4);
    bind(0x8CA8, 0);      /* GL_READ_FRAMEBUFFER */
    read_buffer(0x0405);  /* GL_BACK */
    pixel_store(0x0D05, 1);
    read_pixels(vp[0], vp[1], w, h, 0x80E1, 0x1401, px);   /* GL_BGRA, GL_UNSIGNED_BYTE */
    pixel_store(0x0D05, pack);
    bind(0x8CA8, (unsigned)fb);
    read_buffer((unsigned)rb);
    char path[1024];
    const char *dir = getenv("OCERZ_M32_FRAMEDIR");
    snprintf(path, sizeof path, "%s/ocerz-frame-%d.tga", dir ? dir : "/tmp", n++);
    FILE *f = fopen(path, "wb");
    if (f) {   /* uncompressed 32-bit TGA, bottom-up rows as GL reads them */
        uint8_t hdr[18] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)h,
                            (uint8_t)(h >> 8), 32, 8 };
        fwrite(hdr, 1, 18, f);
        fwrite(px, 4, (size_t)w * h, f);
        fclose(f);
        fprintf(stderr, "ocerz: m32: frame %s %dx%d\n", path, w, h);
    }
    free(px);
}

/* OCERZ_M32LOG=drawtrace: one whole frame, the first after OCERZ_M32_TRACE_AT seconds (default 400) from the first
 * flush that follows a frame of more than 200 draws: after each draw, the target framebuffer, viewport, the color and depth at its center, and the bound
 * vertex program's first 4 local constants (where a world-view-projection matrix usually sits).  Slow; one frame. */
static int g_trace;   /* 1 while the traced frame runs */
static unsigned g_frame_draws;   /* draws since the last flush */
static void trace_flush(void)
{
    static uint64_t first, done;
    unsigned draws = __atomic_exchange_n(&g_frame_draws, 0, __ATOMIC_RELAXED);
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (!first)
        first = now;
    if (g_trace) {
        g_trace = 0;
        done = 1;
        fprintf(stderr, "ocerz: m32: drawtrace end\n");
        return;
    }
    const char *at = getenv("OCERZ_M32_TRACE_AT");
    if (!done && draws > 200 && now - first > (uint64_t)(at ? atoi(at) : 400) * 1000000000ull) {   /* a busy frame */
        g_trace = 1;
        fprintf(stderr, "ocerz: m32: drawtrace begin\n");
    }
}
static void trace_draw(const char *name, const uint64_t *x)
{
    static int n;
    const char *lib = "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL";
    void (*getiv)(unsigned, int *) = (void (*)(unsigned, int *))ocerz_bridge_host_symbol(lib, "glGetIntegerv");
    void (*bind)(unsigned, unsigned) = (void (*)(unsigned, unsigned))ocerz_bridge_host_symbol(lib, "glBindFramebuffer");
    void (*read_pixels)(int, int, int, int, unsigned, unsigned, void *) =
        (void (*)(int, int, int, int, unsigned, unsigned, void *))ocerz_bridge_host_symbol(lib, "glReadPixels");
    void (*attach)(unsigned, unsigned, unsigned, int *) = (void (*)(unsigned, unsigned, unsigned, int *))
        ocerz_bridge_host_symbol(lib, "glGetFramebufferAttachmentParameteriv");
    void (*local)(unsigned, unsigned, float *) =
        (void (*)(unsigned, unsigned, float *))ocerz_bridge_host_symbol(lib, "glGetProgramLocalParameterfvARB");
    unsigned char (*is_enabled)(unsigned) = (unsigned char (*)(unsigned))ocerz_bridge_host_symbol(lib, "glIsEnabled");
    if (!getiv || !bind || !read_pixels || !attach || !local || !is_enabled)
        return;
    int draw_fb = 0, read_fb = 0, vp[4] = { 0 }, type = 0, obj = 0, depth_func = 0, vprog = 0;
    getiv(0x8CA6, &draw_fb);   /* GL_DRAW_FRAMEBUFFER_BINDING */
    getiv(0x8CAA, &read_fb);
    getiv(0x0BA2, vp);
    getiv(0x0B74, &depth_func);  /* GL_DEPTH_FUNC */
    void (*progiv)(unsigned, unsigned, int *) =
        (void (*)(unsigned, unsigned, int *))ocerz_bridge_host_symbol(lib, "glGetProgramivARB");
    if (progiv)
        progiv(0x8620, 0x8677, &vprog);   /* GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_BINDING_ARB */
    if (draw_fb) {
        attach(0x8CA9, 0x8CE0, 0x8CD0, &type);   /* GL_DRAW_FRAMEBUFFER, COLOR_ATTACHMENT0, OBJECT_TYPE */
        attach(0x8CA9, 0x8CE0, 0x8CD1, &obj);    /* OBJECT_NAME */
    }
    bind(0x8CA8, (unsigned)draw_fb);
    float rgba[4] = { -1, -1, -1, -1 }, depth = -1;
    int cx = vp[0] + vp[2] / 2, cy = vp[1] + vp[3] / 2;
    read_pixels(cx, cy, 1, 1, 0x1908, 0x1406, rgba);   /* GL_RGBA, GL_FLOAT */
    read_pixels(cx, cy, 1, 1, 0x1902, 0x1406, &depth);  /* GL_DEPTH_COMPONENT */
    bind(0x8CA8, (unsigned)read_fb);
    float c[16] = { 0 };
    for (unsigned i = 0; i < 4; i++)
        local(0x8620, i, c + 4 * i);   /* GL_VERTEX_PROGRAM_ARB */
    fprintf(stderr, "ocerz: m32: draw %d %s n=%llu fb=%d att=%#x:%d vp=%d,%d,%dx%d depth(%s %#x) center=(%.3g %.3g %.3g %.3g) "
            "z=%.4g vprog=%d c0-3=[%.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g]\n",
            n++, name + 1, (unsigned long long)x[1], draw_fb, type, obj, vp[0], vp[1], vp[2], vp[3],
            is_enabled(0x0B71) ? "on" : "off", depth_func, rgba[0], rgba[1], rgba[2], rgba[3], depth, vprog,
            c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
}

static void trace_clear(const uint64_t *x)
{
    const char *lib = "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL";
    void (*getiv)(unsigned, int *) = (void (*)(unsigned, int *))ocerz_bridge_host_symbol(lib, "glGetIntegerv");
    void (*getfv)(unsigned, float *) = (void (*)(unsigned, float *))ocerz_bridge_host_symbol(lib, "glGetFloatv");
    int fb = 0;
    float col[4] = { 0 }, z = 0;
    getiv(0x8CA6, &fb);
    getfv(0x0C22, col);   /* GL_COLOR_CLEAR_VALUE */
    getfv(0x0B73, &z);    /* GL_DEPTH_CLEAR_VALUE */
    fprintf(stderr, "ocerz: m32: clear mask=%#llx fb=%d color=(%.3g %.3g %.3g %.3g) depth=%.3g\n",
            (unsigned long long)x[0], fb, col[0], col[1], col[2], col[3], z);
}

/* OCERZ_M32LOG=glstat: how often each GL/CGL call ran in the last 10 seconds, most first (what a frame does).
 * ponytail: 512 names, racy counts (it is a rough picture). */
static void count_gl(const char *name)
{
    static const char *names[512];
    static unsigned counts[512];
    static uint64_t next;
    unsigned h = (unsigned)(((uintptr_t)name >> 3) * 2654435761u) & 511;
    for (unsigned i = 0; i < 512; i++, h = (h + 1) & 511)
        if (names[h] == name || (!names[h] && __sync_bool_compare_and_swap(&names[h], NULL, name)) || names[h] == name) {
            __atomic_add_fetch(&counts[h], 1, __ATOMIC_RELAXED);
            break;
        }
    static unsigned cpus[16];   /* the core each frame was flushed from: on this M4 Max 0-3 are efficiency cores, where
                                 * Batman's render thread sometimes lands for good and runs 7x slower */
    size_t cpu = 0;
    static uint64_t rcpu_last, rcpu_sum;   /* the flushing (render) thread's CPU time per frame: the headroom at a
                                            * display cap is 16.7 ms over it (spin-waits count as work) */
    static unsigned rcpu_n;
    if (!strcmp(name, "_CGLFlushDrawable")) {
        if (!pthread_cpu_number_np(&cpu))
            cpus[cpu & 15]++;
        uint64_t t = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
        if (rcpu_last && t > rcpu_last) {
            rcpu_sum += t - rcpu_last;
            rcpu_n++;
        }
        rcpu_last = t;
    }
    static unsigned tick;   /* the clock every 256th call: reading it on each one was 2-5% of the render thread */
    if (next && (__atomic_add_fetch(&tick, 1, __ATOMIC_RELAXED) & 255))
        return;
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (!next)
        next = now + 10000000000ull;
    if (now < next || !__sync_bool_compare_and_swap(&next, next, now + 10000000000ull))
        return;
    fprintf(stderr, "ocerz: m32: glstat");
    for (int k = 0; k < 512; k++) {
        int best = -1;
        for (int i = 0; i < 512; i++)
            if (names[i] && counts[i] && (best < 0 || counts[i] > counts[best]))
                best = i;
        if (best < 0)
            break;
        fprintf(stderr, " %s=%u", names[best] + 1, counts[best]);
        counts[best] = 0;
    }
    for (int i = 0; i < 512; i++)
        counts[i] = 0;
    fprintf(stderr, " cpus=");
    for (int i = 0; i < 16; i++, cpus[i - 1] = 0)
        fprintf(stderr, "%u%s", cpus[i], i < 15 ? "," : "");
    fprintf(stderr, " rcpu=%.2fms", rcpu_n ? rcpu_sum / 1e6 / rcpu_n : 0.0);
    rcpu_sum = rcpu_n = 0;
    fprintf(stderr, "\n");
}

/* OCERZ_M32LOG=calls OCERZ_M32_CFDESC=<name part>: what matching calls return, described (they must return CF
 * objects: a raw pointer from another call would be taken for one) */
static void log_cfdesc(void *p)
{
    static void *(*desc)(const void *);
    static int (*cstr)(const void *, char *, long, uint32_t);
    static void (*release)(const void *);
    if (!desc) {
        const char *lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
        desc = (void *(*)(const void *))ocerz_bridge_host_symbol(lib, "CFCopyDescription");
        cstr = (int (*)(const void *, char *, long, uint32_t))ocerz_bridge_host_symbol(lib, "CFStringGetCString");
        release = (void (*)(const void *))ocerz_bridge_host_symbol(lib, "CFRelease");
    }
    if (!p || !desc || !(((uintptr_t)p >> 63) || malloc_zone_from_ptr(p)))
        return;
    void *d = desc(p);
    char buf[200] = "";
    if (d && cstr(d, buf, sizeof buf, 0x08000100))
        fprintf(stderr, "ocerz: m32:   = %s\n", buf);
    if (d)
        release(d);
}

__thread void *m32_cross_self;   /* when set, the first host argument (a super send's objc_super) */

int m32_cross(struct OcerzVM *vm, OcerzCPU *cpu, M32Export *e)
{
    void *self_override = m32_cross_self;
    m32_cross_self = NULL;
    if (!e->gsig) {
        pthread_mutex_lock(&g_sig_lock);
        if (!e->gsig) {
            M32Sig *h = m32_sig_parse(e->hnote, 0), *g = m32_sig_parse(e->gnote, 1);
            if (!h || !g || h->nargs != g->nargs) {
                pthread_mutex_unlock(&g_sig_lock);
                m32_log_once("notations do not parse, returning 0:", e->name);
                m32_ret(cpu, 0, 0, 0);
                return OCERZ_STEP_OK;
            }
            e->hsig = h;
            __atomic_store_n(&e->gsig, g, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&g_sig_lock);
    }
    const M32Sig *g = e->gsig, *h = e->hsig;
    static const char *calllog = (const char *)1;   /* OCERZ_M32_CALLLOG also names the notations a crossing uses */
    if (calllog == (const char *)1)
        calllog = getenv("OCERZ_M32_CALLLOG");
    if (calllog && strstr(e->name, calllog))
        fprintf(stderr, "ocerz: m32: calllog %s crosses as %s / %s\n", e->name, e->gnote, e->hnote);
    HostCall hc;
    hc.sp = 0;
    hc.nx = hc.nv = 0;
    hc.scratch_used = 0;
    Writeback wb[16];
    int nwb = 0;
    uint32_t ap = (uint32_t)cpu->gpr[OCERZ_RSP] + 4, sret = 0;
    int pop = 0;
    if (g->ret.cls == '{' && !(m32_struct_in_regs(&g->ret))) {
        sret = m32_rd(ap);
        ap += 4;
        pop = 4;
    }
    for (int i = 0; i < g->nargs; i++) {
        const M32Type *ga = &g->arg[i], *ha = &h->arg[i];
        if (ga->cls == '{') {
            uint8_t hbuf[256] = { 0 };
            struct_in(ga, ha, m32_h(ap), hbuf, e->name, i);
            put_struct(&hc, hbuf, ha);
            ap += (ga->size + 3u) & ~3u;
            continue;
        }
        uint64_t raw = ga->size == 8 ? (m32_rd(ap) | (uint64_t)m32_rd(ap + 4) << 32) : m32_rd(ap);
        ap += ga->size == 8 ? 8 : 4;
        if (i == 0 && self_override) {
            put_int(&hc, (uint64_t)(uintptr_t)self_override, 8);
            continue;
        }
        if (ga->cls == 'k') {   /* a block: the API's block type lacks the block itself as the invoke's first argument */
            char gb[300], hb[300];
            const char *gs = NULL, *hs = NULL;
            if (ga->cb && ha->cb && strchr(ga->cb, '(') && strchr(ha->cb, '(')) {
                snprintf(gb, sizeof gb, "%.*sk%s", (int)(strchr(ga->cb, '(') - ga->cb + 1), ga->cb, strchr(ga->cb, '(') + 1);
                snprintf(hb, sizeof hb, "%.*sk%s", (int)(strchr(ha->cb, '(') - ha->cb + 1), ha->cb, strchr(ha->cb, '(') + 1);
                gs = gb;
                hs = hb;
            }
            void *hblk = m32_block_to_host((uint32_t)raw, gs, hs);
            if (!hblk && raw) {   /* a guest block that would reach the host as NULL: say why */
                char why[200];
                snprintf(why, sizeof why, "%s arg %d: block %#x isa %#x flags %#x guest-block %d (%s / %s)", e->name, i,
                         (uint32_t)raw, m32_rd((uint32_t)raw), m32_rd((uint32_t)raw + 4), m32_is_guest_block((uint32_t)raw),
                         gs ? gs : "-", hs ? hs : "-");
                m32_log_once("a guest block crossed as NULL:", why);
            }
            put_int(&hc, (uint64_t)(uintptr_t)hblk, 8);
            continue;
        }
        if (ga->cls == 'c' && raw) {
            put_int(&hc, (uint64_t)(uintptr_t)m32_callback((uint32_t)raw, ga->cb, ha->cb), 8);
            continue;
        }
        if (ga->cls == 'R' || ga->cls == 'r') {   /* copy in, and back out when the callee may write it */
            uint8_t *hs = raw ? scratch(&hc, ha->ssize) : NULL;
            if (hs) {
                struct_in(ga, ha, m32_h((uint32_t)raw), hs, e->name, i);
                if (ga->cls == 'R' && nwb < 16)
                    wb[nwb++] = (Writeback){ 'R', (uint32_t)raw, (uint64_t *)hs, ga, ha };
            }
            put_int(&hc, (uint64_t)(uintptr_t)hs, 8);
            continue;
        }
        if (ga->cls == 'P' || ga->cls == 'W' || ga->cls == 'V') {
            uint64_t *cell = raw ? scratch(&hc, 8) : NULL;
            if (cell) {
                uint32_t cur = m32_rd((uint32_t)raw);
                *cell = ga->cls == 'P' ? (uint64_t)(uintptr_t)m32_host(cur)
                      : ga->cls == 'W' ? (uint64_t)(int64_t)(int32_t)cur : cur;
                wb[nwb++] = (Writeback){ ga->cls, (uint32_t)raw, cell };
            }
            put_int(&hc, (uint64_t)(uintptr_t)cell, 8);
            continue;
        }
        uint64_t v = m32_scalar_in(ga->cls, ha->cls, raw, e->name, i);
        if (ha->cls == 'f' || ha->cls == 'd')
            put_fp(&hc, v, ha->size);
        else
            put_int(&hc, v, ha->size);
    }
    static int log_calls = -1, log_files = -1;
    if (log_calls < 0)
        log_calls = getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "calls");
    if (log_files < 0)
        log_files = getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "files");
    if (log_files && !log_calls && g->nargs && g->arg[0].cls == 's' &&
        (strstr(e->name, "open") || strstr(e->name, "stat") || strstr(e->name, "access") || strstr(e->name, "PathMakeRef"))) {
        uint32_t a0 = m32_rd((uint32_t)cpu->gpr[OCERZ_RSP] + 4);   /* OCERZ_M32LOG=files: which files the guest asks for */
        if (a0 && !m32_is_handle(a0))
            fprintf(stderr, "ocerz: m32: file %s %.300s\n", e->name, (const char *)m32_h(a0));
    }
    if (log_calls) {
        uint32_t esp = (uint32_t)cpu->gpr[OCERZ_RSP];
        fprintf(stderr, "ocerz: m32: call %s %s <- %#x [t%u] args %#x %#x %#x %#x %#x %#x x0=%#llx x1=%#llx x4=%#llx\n",
                e->name, e->gnote, m32_rd(esp), (unsigned)pthread_mach_thread_np(pthread_self()), m32_rd(esp + 4), m32_rd(esp + 8), m32_rd(esp + 12), m32_rd(esp + 16),
                m32_rd(esp + 20), m32_rd(esp + 24), (unsigned long long)hc.x[0], (unsigned long long)hc.x[1],
                (unsigned long long)hc.x[4]);
        for (int i = 0, off = 4; i < g->nargs && i < 4; off += g->arg[i].size > 4 ? 8 : 4, i++)
            if (g->arg[i].cls == 's' && m32_rd(esp + off) && !m32_is_handle(m32_rd(esp + off)))
                fprintf(stderr, "ocerz: m32:   arg%d \"%.300s\"\n", i, (const char *)m32_h(m32_rd(esp + off)));
        if (strstr(getenv("OCERZ_M32LOG"), "frames")) {   /* the guest's EBP chain: who is calling */
            uint32_t bp = (uint32_t)cpu->gpr[OCERZ_RBP];
            fprintf(stderr, "ocerz: m32:   frames");
            for (int i = 0; i < 8 && bp && bp < M32_HANDLE_LO && !(bp & 3); i++, bp = m32_rd(bp))
                fprintf(stderr, " %#x", m32_rd(bp + 4));
            fprintf(stderr, "\n");
        }
    }
    uint8_t big[256] __attribute__((aligned(16)));
    uint64_t out_x[2] = { 0, 0 }, out_v[4] = { 0, 0, 0, 0 };
    static int log_dump = -1;
    if (log_dump < 0)
        log_dump = getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "framedump");
    static int log_trace = -1;
    if (log_trace < 0)
        log_trace = getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "drawtrace");
    if (log_dump && !strcmp(e->name, "_CGLFlushDrawable"))
        dump_frame();
    if (log_trace && !strcmp(e->name, "_CGLFlushDrawable"))
        trace_flush();
    struct OcerzBridgeFrame frame;
    ocerz_bridge_raise(&frame, e->lib, e->name, e->hnote, e->host_fn);
    void *exc = NULL;
    m32_call_native_catching(e->host_fn, hc.x, hc.v, (const uint64_t *)hc.stack, (hc.sp + 15) & ~15u,
                             h->ret.cls == '{' && !m32_is_hfa(&h->ret) && h->ret.size > 16 ? big : NULL, out_x, out_v, &exc);
    ocerz_bridge_lower(&frame);
    m32_refresh_data();
    if (exc)   /* the host raised: the guest's innermost @try gets it */
        return m32_objc_guest_throw(cpu, m32_objc_to_guest(exc));
    m32_save_errno(cpu);
    static int log_gl = -1, log_nan = -1, log_stat = -1;
    if (log_gl < 0) {
        log_gl = getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "glerr");
        log_nan = getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "glnan");
        log_stat = getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "glstat");
    }
    if (log_stat && (!strncmp(e->name, "_gl", 3) || !strncmp(e->name, "_CGL", 4)))
        count_gl(e->name);
    if (log_gl && e->name[1] == 'g' && e->name[2] == 'l')
        log_glerr(e->name, hc.x, m32_rd((uint32_t)cpu->gpr[OCERZ_RSP]));
    if (log_nan && e->name[1] == 'g' && e->name[2] == 'l')
        log_glnan(e->name, hc.x, m32_rd((uint32_t)cpu->gpr[OCERZ_RSP]));
    if (log_trace > 0 && !strncmp(e->name, "_glDraw", 7))
        __atomic_add_fetch(&g_frame_draws, 1, __ATOMIC_RELAXED);
    if (g_trace && !strncmp(e->name, "_glDraw", 7))
        trace_draw(e->name, hc.x);
    if (g_trace && !strcmp(e->name, "_glClear"))
        trace_clear(hc.x);
    if (log_calls) {
        fprintf(stderr, "ocerz: m32:   -> x0=%#llx v0=%#llx\n", (unsigned long long)out_x[0], (unsigned long long)out_v[0]);
        const char *only = getenv("OCERZ_M32_CFDESC");   /* calls whose name contains it return CF objects */
        if (only && strstr(e->name, only) && h->ret.cls == 'p')
            log_cfdesc((void *)(uintptr_t)out_x[0]);
    }
    for (int i = 0; i < nwb; i++) {
        if (wb[i].cls == 'R') {
            struct_out(wb[i].gt, wb[i].ht, (const uint8_t *)wb[i].cell, m32_h(wb[i].guest));
            continue;
        }
        uint64_t v = *wb[i].cell;
        m32_wr(wb[i].guest, wb[i].cls == 'P' ? m32_handle((void *)(uintptr_t)v) : (uint32_t)v);
    }
    const M32Type *gr = &g->ret, *hr = &h->ret;
    if (gr->cls == 'v') {
        m32_ret(cpu, (uint32_t)cpu->gpr[OCERZ_RAX], (uint32_t)cpu->gpr[OCERZ_RDX], 0);
        return OCERZ_STEP_OK;
    }
    if (gr->cls == '{') {
        uint8_t hbuf[256] = { 0 }, gbuf[256] = { 0 };
        if (m32_is_hfa(hr))
            for (int i = 0; i < hr->n; i++)
                memcpy(hbuf + hr->moff[i], &out_v[i], hr->msize[i]);
        else if (hr->size <= 16)
            memcpy(hbuf, out_x, hr->size);
        else
            memcpy(hbuf, big, hr->size);
        struct_out(gr, hr, hbuf, gbuf);
        if (!m32_struct_in_regs(gr)) {
            memcpy(m32_h(sret), gbuf, gr->size);
            m32_ret(cpu, sret, 0, pop);
        } else if (gr->n == 1 && (gr->mcls[0] == 'f' || gr->mcls[0] == 'd')) {
            uint64_t raw = 0;
            memcpy(&raw, gbuf, gr->size);
            m32_ret_st0(cpu, gr->mcls[0] == 'f' ? fbits((uint32_t)raw) : dbits(raw));
        } else {
            uint32_t w[2] = { 0, 0 };
            memcpy(w, gbuf, gr->size);
            m32_ret(cpu, w[0], w[1], 0);
        }
        return OCERZ_STEP_OK;
    }
    uint64_t raw = (hr->cls == 'f' || hr->cls == 'd') ? out_v[0] : out_x[0];
    if (hr->cls == 'f')
        raw = (uint32_t)raw;
    uint64_t v = m32_scalar_out(gr->cls, hr->cls, raw);
    switch (gr->cls) {
    case 'f': m32_ret_st0(cpu, fbits((uint32_t)v)); break;
    case 'd': m32_ret_st0(cpu, dbits(v)); break;
    case 'l': case 'L': case 'T': m32_ret(cpu, (uint32_t)v, (uint32_t)(v >> 32), 0); break;
    case 'b': m32_ret(cpu, (uint32_t)(int32_t)(int8_t)v, 0, 0); break;
    case 'B': m32_ret(cpu, (uint8_t)v, 0, 0); break;
    case 'h': m32_ret(cpu, (uint32_t)(int32_t)(int16_t)v, 0, 0); break;
    case 'H': m32_ret(cpu, (uint16_t)v, 0, 0); break;
    default: m32_ret(cpu, (uint32_t)v, 0, 0); break;
    }
    return OCERZ_STEP_OK;
}

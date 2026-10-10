/*
 * Mapped OpenGL buffers for i386 guests (include/ocerz/m32.h).
 *
 * glMapBuffer hands back driver memory far outside the guest's 4 GB window, so a guest could never write its vertices
 * there (handed a handle instead, the writes went nowhere and every 3D draw came out empty).  A mapping gets a shadow
 * in the guest heap instead: the buffer's bytes are copied in when it is mapped, flushed ranges
 * (glFlushMappedBufferRange[APPLE]) are copied out as they are flushed, and at unmap the whole shadow is copied out
 * unless the guest flushed ranges itself (it is then in explicit-flush mode and has said what it wrote).  A shadow is
 * kept per (context, buffer) for the next mapping.
 *
 * ponytail: 256 shadows, a linear scan under one lock, and a full copy in at every map; track written ranges when
 * the copies show up in a profile.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ocerz/bridge.h"
#include "ocerz/interp.h"
#include "ocerz/m32.h"

#define GL_LIB "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL"

typedef struct Shadow {
    void *ctx;
    uint32_t buffer, guest, cap;   /* the guest copy and its capacity */
    uint8_t *host;                 /* while mapped: the driver's pointer to the mapped range */
    uint32_t size, flushed;        /* the mapped range's size; whether the guest flushed any part of it */
} Shadow;
static Shadow g_shadows[256];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static struct {
    void *(*current)(void);
    void (*getiv)(unsigned, int *);
    void (*buffer_iv)(unsigned, unsigned, int *);
    void *(*map)(unsigned, unsigned);
    void *(*map_range)(unsigned, intptr_t, intptr_t, unsigned);
    unsigned char (*unmap)(unsigned);
    void (*flush_apple)(unsigned, intptr_t, intptr_t);
    void (*flush_range)(unsigned, intptr_t, intptr_t);
} gl;

static void gl_init(void)
{
    if (gl.map)
        return;
    gl.current = (void *(*)(void))ocerz_bridge_host_symbol(GL_LIB, "CGLGetCurrentContext");
    gl.getiv = (void (*)(unsigned, int *))ocerz_bridge_host_symbol(GL_LIB, "glGetIntegerv");
    gl.buffer_iv = (void (*)(unsigned, unsigned, int *))ocerz_bridge_host_symbol(GL_LIB, "glGetBufferParameteriv");
    gl.map_range = (void *(*)(unsigned, intptr_t, intptr_t, unsigned))ocerz_bridge_host_symbol(GL_LIB, "glMapBufferRange");
    gl.unmap = (unsigned char (*)(unsigned))ocerz_bridge_host_symbol(GL_LIB, "glUnmapBuffer");
    gl.flush_apple = (void (*)(unsigned, intptr_t, intptr_t))ocerz_bridge_host_symbol(GL_LIB, "glFlushMappedBufferRangeAPPLE");
    gl.flush_range = (void (*)(unsigned, intptr_t, intptr_t))ocerz_bridge_host_symbol(GL_LIB, "glFlushMappedBufferRange");
    gl.map = (void *(*)(unsigned, unsigned))ocerz_bridge_host_symbol(GL_LIB, "glMapBuffer");
}

/* the buffer bound to target (the binding query of each buffer target) */
static uint32_t bound(unsigned target)
{
    unsigned q;
    switch (target) {
    case 0x8892: q = 0x8894; break;   /* GL_ARRAY_BUFFER */
    case 0x8893: q = 0x8895; break;   /* GL_ELEMENT_ARRAY_BUFFER */
    case 0x88EB: q = 0x88ED; break;   /* GL_PIXEL_PACK_BUFFER */
    case 0x88EC: q = 0x88EF; break;   /* GL_PIXEL_UNPACK_BUFFER */
    case 0x8A11: q = 0x8A28; break;   /* GL_UNIFORM_BUFFER */
    case 0x8C8E: q = 0x8C8F; break;   /* GL_TRANSFORM_FEEDBACK_BUFFER */
    default: q = target; break;       /* GL_COPY_READ/WRITE_BUFFER and the rest bind under their own name */
    }
    int b = 0;
    gl.getiv(q, &b);
    return (uint32_t)b;
}

/* the shadow of (current context, buffer), made or grown to size; NULL when there is no room */
static Shadow *shadow_for(uint32_t buffer, uint32_t size)
{
    void *ctx = gl.current();
    Shadow *s = NULL, *spare = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < 256 && !s; i++) {
        if (g_shadows[i].ctx == ctx && g_shadows[i].buffer == buffer)
            s = &g_shadows[i];
        else if (!spare && !g_shadows[i].host)
            spare = &g_shadows[i];   /* empty, or an unmapped one to recycle */
    }
    if (!s && spare) {
        m32_free(spare->guest);
        *spare = (Shadow){ ctx, buffer, 0, 0, NULL, 0, 0 };
        s = spare;
    }
    if (s && s->cap < size) {
        m32_free(s->guest);
        s->guest = m32_malloc(size);
        s->cap = s->guest ? size : 0;
    }
    pthread_mutex_unlock(&g_lock);
    return s && s->guest ? s : NULL;
}

static Shadow *mapped(unsigned target)
{
    void *ctx = gl.current();
    uint32_t buffer = bound(target);
    for (int i = 0; i < 256; i++)
        if (g_shadows[i].ctx == ctx && g_shadows[i].buffer == buffer && g_shadows[i].host)
            return &g_shadows[i];
    return NULL;
}

#define RET(v) do { m32_ret(cpu, (uint32_t)(v), 0, 0); return OCERZ_STEP_OK; } while (0)

static int map_common(OcerzCPU *cpu, unsigned target, uint8_t *host, uint32_t size)
{
    if (!host)
        RET(0);
    Shadow *s = shadow_for(bound(target), size ? size : 1);
    if (!s) {
        fprintf(stderr, "ocerz: m32: glMapBuffer: no guest shadow for %u bytes\n", (unsigned)size);
        gl.unmap(target);
        RET(0);
    }
    memcpy(m32_h(s->guest), host, size);
    s->host = host;
    s->size = size;
    s->flushed = 0;
    RET(s->guest);
}

/* glMapBuffer(target, access) */
static int sp_glMapBuffer(struct OcerzVM *vm, OcerzCPU *cpu)
{
    gl_init();
    unsigned target = m32_arg(cpu, 0);
    int size = 0;
    gl.buffer_iv(target, 0x8764, &size);   /* GL_BUFFER_SIZE */
    return map_common(cpu, target, gl.map(target, m32_arg(cpu, 1)), (uint32_t)size);
}
/* glMapBufferRange(target, offset, length, access): the shadow holds just the range */
static int sp_glMapBufferRange(struct OcerzVM *vm, OcerzCPU *cpu)
{
    gl_init();
    unsigned target = m32_arg(cpu, 0);
    uint32_t len = m32_arg(cpu, 2);
    return map_common(cpu, target, gl.map_range(target, (int32_t)m32_arg(cpu, 1), len, m32_arg(cpu, 3)), len);
}
/* glFlushMappedBufferRange[APPLE](target, offset, size): the offset is within the mapped range */
static int flush_common(OcerzCPU *cpu, void (*flush)(unsigned, intptr_t, intptr_t))
{
    gl_init();
    unsigned target = m32_arg(cpu, 0);
    uint32_t off = m32_arg(cpu, 1), len = m32_arg(cpu, 2);
    Shadow *s = mapped(target);
    if (s && off <= s->size && len <= s->size - off) {
        memcpy(s->host + off, m32_h(s->guest + off), len);
        s->flushed = 1;
    }
    if (flush)
        flush(target, off, len);
    RET(0);
}
static int sp_glFlushMappedBufferRangeAPPLE(struct OcerzVM *vm, OcerzCPU *cpu) { return flush_common(cpu, gl.flush_apple); }
static int sp_glFlushMappedBufferRange(struct OcerzVM *vm, OcerzCPU *cpu) { return flush_common(cpu, gl.flush_range); }
/* glUnmapBuffer(target) */
static int sp_glUnmapBuffer(struct OcerzVM *vm, OcerzCPU *cpu)
{
    gl_init();
    unsigned target = m32_arg(cpu, 0);
    Shadow *s = mapped(target);
    if (s) {
        if (!s->flushed)
            memcpy(s->host, m32_h(s->guest), s->size);
        s->host = NULL;
    }
    RET(gl.unmap(target));
}

/* CGLSetParameter(ctx, param, const GLint *values): passed through; the swap interval (kCGLCPSwapInterval, 222) the
 * game asks for is logged once, and OCERZ_M32_SWAPINTERVAL=<n> replaces it (vsync quantizes a frame to 60/n fps) */
static int sp_CGLSetParameter(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int (*f)(void *, int, const int32_t *) =
        (int (*)(void *, int, const int32_t *))ocerz_bridge_host_symbol(GL_LIB, "CGLSetParameter");
    int param = (int)m32_arg(cpu, 1);
    uint32_t vals = m32_arg(cpu, 2);
    int32_t local[4];
    const int32_t *v = vals ? (const int32_t *)m32_h(vals) : NULL;
    if (param == 222 && v) {
        static int logged;
        const char *o = getenv("OCERZ_M32_SWAPINTERVAL");
        if (!logged++)
            fprintf(stderr, "ocerz: m32: swap interval %d requested%s%s\n", v[0], o ? ", using " : "", o ? o : "");
        if (o) {
            local[0] = atoi(o);
            v = local;
        }
    }
    RET(f ? f(m32_host(m32_arg(cpu, 0)), param, v) : 10000);   /* kCGLBadAttribute */
}

/* glShaderSource(shader, count, const GLchar *const *strings, const GLint *lengths): the strings are an array of
 * 4-byte guest pointers (Feral's IndirectX with UseGLSL=1); the lengths are ints either way.  glShaderSourceARB
 * takes a GLhandleARB (a pointer-sized integer on the Mac), zero-extended. */
static int shader_source(OcerzCPU *cpu, const char *name)
{
    void (*f)(uintptr_t, int, const char *const *, const int32_t *) =
        (void (*)(uintptr_t, int, const char *const *, const int32_t *))ocerz_bridge_host_symbol(GL_LIB, name);
    int count = (int)m32_arg(cpu, 1);
    uint32_t strings = m32_arg(cpu, 2), lengths = m32_arg(cpu, 3);
    if (!f || count < 0 || count > 4096)
        RET(0);
    const char **host = calloc((size_t)count + 1, sizeof *host);
    if (!host)
        RET(0);
    for (int i = 0; i < count; i++)
        host[i] = (const char *)m32_host(m32_rd(strings + 4 * (uint32_t)i));
    const char *dump = getenv("OCERZ_M32_SHADERDUMP");   /* <dir>: each source as shader-<n>.glsl */
    if (dump) {
        static int n;
        char path[1024];
        snprintf(path, sizeof path, "%s/shader-%04d.glsl", dump, __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED));
        FILE *fp = fopen(path, "w");
        for (int i = 0; fp && i < count; i++) {
            const int32_t *len = lengths ? (const int32_t *)m32_h(lengths) : NULL;
            if (len && len[i] >= 0) fwrite(host[i], 1, (size_t)len[i], fp);
            else fputs(host[i], fp);
        }
        if (fp) fclose(fp);
    }
    f(m32_arg(cpu, 0), count, host, lengths ? (const int32_t *)m32_h(lengths) : NULL);
    free(host);
    RET(0);
}
static int sp_glShaderSource(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; return shader_source(cpu, "glShaderSource"); }
static int sp_glShaderSourceARB(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; return shader_source(cpu, "glShaderSourceARB"); }

/* CGLSetCurrentContext, CGLLockContext, CGLUnlockContext(ctx): Batman's IndirectX brackets every GL call with them
 * (~85% of its render thread's crossings, ~1M a second); straight to the host function, without m32_cross's generic
 * marshalling.  The context is a handle (m32_host). */
#define CGL_CTX_CALL(name)                                                                                       \
    static int sp_##name(struct OcerzVM *vm, OcerzCPU *cpu)                                                    \
    {                                                                                                          \
        static int (*f)(void *);                                                                               \
        if (!f)                                                                                                \
            f = (int (*)(void *))ocerz_bridge_host_symbol(GL_LIB, #name);                                      \
        RET(f ? f(m32_host(m32_arg(cpu, 0))) : 10000);   /* kCGLBadAttribute */                                \
    }
CGL_CTX_CALL(CGLSetCurrentContext)
CGL_CTX_CALL(CGLLockContext)
CGL_CTX_CALL(CGLUnlockContext)

const M32SpecialEntry m32_gl_specials[] = {
    { "_CGLSetCurrentContext", sp_CGLSetCurrentContext }, { "_CGLLockContext", sp_CGLLockContext },
    { "_CGLUnlockContext", sp_CGLUnlockContext },
    { "_CGLSetParameter", sp_CGLSetParameter },
    { "_glShaderSource", sp_glShaderSource }, { "_glShaderSourceARB", sp_glShaderSourceARB },
    { "_glMapBuffer", sp_glMapBuffer }, { "_glMapBufferARB", sp_glMapBuffer },
    { "_glMapBufferRange", sp_glMapBufferRange },
    { "_glFlushMappedBufferRange", sp_glFlushMappedBufferRange },
    { "_glFlushMappedBufferRangeAPPLE", sp_glFlushMappedBufferRangeAPPLE },
    { "_glUnmapBuffer", sp_glUnmapBuffer }, { "_glUnmapBufferARB", sp_glUnmapBuffer },
    { NULL, NULL }
};

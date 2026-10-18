/*
 * Mapped OpenGL buffers for i386 guests (include/ocerz/m32.h).
 *
 * glMapBuffer hands back driver memory far outside the guest's 4 GB window, so a guest could never write its vertices
 * there (handed a handle instead, the writes went nowhere and every 3D draw came out empty).  A mapping gets a shadow
 * in the guest heap instead: flushed ranges (glFlushMappedBufferRange[APPLE]) are copied out as they are flushed, and
 * at unmap the whole shadow is copied out unless the guest flushed ranges itself (it is then in explicit-flush mode and
 * has said what it wrote).  A shadow is kept per (share group, buffer) for the next mapping.
 *
 * A shadow is the buffer's mirror once it has been mapped through it: what the buffer holds is what the guest last
 * wrote there, or undefined after glBufferData without data (the orphaning a game does before refilling a dynamic
 * buffer).  Mapping it again copies nothing in, and maps ask the driver nothing either: the buffer's size is the
 * shadow's.  Both cost Left 4 Dead 2's render thread about half its time: the copy of the whole buffer in at each map,
 * and glGetBufferParameteriv, which on Apple's multithreaded engine waits for every queued command to run first.
 * glBufferData with data and glBufferSubData keep the mirror true; anything else that writes a buffer (a pixel pack,
 * transform feedback, glCopyBufferSubData) does not go through m32, so only array and element array buffers are
 * mirrored and a buffer bound elsewhere is copied in at every map.
 *
 * ponytail: 256 shadows, a linear scan under one lock.
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
    void *group;
    uint32_t buffer, guest, cap;   /* the guest copy and its capacity */
    uint8_t *host;                 /* while mapped: the driver's pointer to the mapped range */
    uint32_t size, flushed;        /* the mapped range's size; whether the guest flushed any part of it */
    uint32_t bufsize, mirror;      /* the buffer's size (0: not known); whether the copy is the buffer's mirror */
} Shadow;
static Shadow g_shadows[256];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static struct {
    void *(*current)(void);
    void *(*share_group)(void *);
    void (*buffer_data)(unsigned, intptr_t, const void *, unsigned);
    void (*buffer_sub_data)(unsigned, intptr_t, intptr_t, const void *);
    void (*delete_buffers)(int, const uint32_t *);
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
    gl.share_group = (void *(*)(void *))ocerz_bridge_host_symbol(GL_LIB, "CGLGetShareGroup");
    gl.buffer_data = (void (*)(unsigned, intptr_t, const void *, unsigned))ocerz_bridge_host_symbol(GL_LIB, "glBufferData");
    gl.buffer_sub_data =
        (void (*)(unsigned, intptr_t, intptr_t, const void *))ocerz_bridge_host_symbol(GL_LIB, "glBufferSubData");
    gl.delete_buffers = (void (*)(int, const uint32_t *))ocerz_bridge_host_symbol(GL_LIB, "glDeleteBuffers");
    __atomic_store_n(&gl.map, (void *(*)(unsigned, unsigned))ocerz_bridge_host_symbol(GL_LIB, "glMapBuffer"),
                     __ATOMIC_RELEASE);
}

/*
 * Vertex and element arrays in buffer objects.  With a buffer bound to GL_ARRAY_BUFFER, a gl*Pointer's pointer is an
 * offset into it, and with one bound to GL_ELEMENT_ARRAY_BUFFER, so is a glDraw*Elements' indices pointer.  m32 made
 * every such pointer a window address: GL then read at the buffer's storage plus the window's base, and Nexuiz's
 * benchmark demo faulted in gleRunVertexSubmitImmediate whenever a surface drew from its buffers.  Each context's two
 * bindings are kept as the guest binds them (asking GL would wait on its multithreaded engine); with a vertex array
 * object bound, the element binding is the object's and GL is asked.  Deleting a bound buffer unbinds it.
 */
static uint32_t bound(unsigned target);

typedef struct Binds { void *ctx; uint32_t array, element, vao; } Binds;
static Binds g_binds[64];
static pthread_mutex_t g_bind_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread Binds *t_binds;

static Binds *binds(void)
{
    gl_init();
    void *ctx = gl.current ? gl.current() : NULL;
    if (t_binds && t_binds->ctx == ctx)
        return t_binds;
    Binds *b = NULL;
    pthread_mutex_lock(&g_bind_lock);
    for (int i = 0; i < 64 && !b; i++)
        if (g_binds[i].ctx == ctx)
            b = &g_binds[i];
    for (int i = 0; i < 64 && !b; i++)
        if (!g_binds[i].ctx) {
            b = &g_binds[i];
            *b = (Binds){ ctx, 0, 0, 0 };
        }
    pthread_mutex_unlock(&g_bind_lock);
    t_binds = b;
    return b;
}

int m32_gl_ptr_kind(const char *name)
{
    static const char *const arrays[] = { "_glVertexPointer", "_glNormalPointer", "_glColorPointer", "_glIndexPointer",
        "_glTexCoordPointer", "_glEdgeFlagPointer", "_glFogCoordPointer", "_glFogCoordPointerEXT",
        "_glSecondaryColorPointer", "_glSecondaryColorPointerEXT", "_glVertexAttribPointer", "_glVertexAttribPointerARB",
        "_glVertexAttribIPointer", "_glVertexAttribIPointerEXT", "_glInterleavedArrays", NULL };
    static const char *const elements[] = { "_glDrawElements", "_glDrawRangeElements", "_glDrawRangeElementsEXT",
        "_glDrawElementsInstanced", "_glDrawElementsInstancedARB", "_glDrawElementsInstancedEXT",
        "_glDrawElementsBaseVertex", "_glDrawRangeElementsBaseVertex", "_glDrawElementsInstancedBaseVertex", NULL };
    if (strncmp(name, "_gl", 3))
        return M32_GL_NONE;
    for (int i = 0; arrays[i]; i++)
        if (!strcmp(name, arrays[i]))
            return M32_GL_ARRAY;
    for (int i = 0; elements[i]; i++)
        if (!strcmp(name, elements[i]))
            return M32_GL_ELEMENT;
    if (!strcmp(name, "_glBindBuffer") || !strcmp(name, "_glBindBufferARB"))
        return M32_GL_BIND;
    if (!strcmp(name, "_glBindVertexArray") || !strcmp(name, "_glBindVertexArrayAPPLE"))
        return M32_GL_BIND_VAO;
    return M32_GL_NONE;
}

uint64_t m32_gl_ptr(int kind, uint32_t g)
{
    Binds *b = binds();
    uint32_t buffer = !b ? 0 : kind == M32_GL_ARRAY ? b->array : b->vao ? bound(0x8893) : b->element;
    return buffer ? g : (uint64_t)(uintptr_t)m32_host(g);
}

void m32_gl_bound(int kind, uint32_t target, uint32_t buffer)
{
    Binds *b = binds();
    if (!b)
        return;
    if (kind == M32_GL_BIND_VAO)
        b->vao = target;   /* glBindVertexArray(array): one argument */
    else if (target == 0x8892)
        b->array = buffer;
    else if (target == 0x8893 && !b->vao)
        b->element = buffer;
}

static void unbind_deleted(uint32_t buffer)
{
    Binds *b = binds();
    if (b && b->array == buffer)
        b->array = 0;
    if (b && b->element == buffer)
        b->element = 0;
}

/* buffers are shared by the contexts of a share group, so their shadows are too */
static void *share_group(void)
{
    void *ctx = gl.current();
    void *g = ctx && gl.share_group ? gl.share_group(ctx) : NULL;
    return g ? g : ctx;
}

/* only these are mirrored: the guest's own calls are the only writers m32 sees */
static int mirrored(unsigned target) { return target == 0x8892 || target == 0x8893; }   /* GL_[ELEMENT_]ARRAY_BUFFER */

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
    if (gl.getiv)
        gl.getiv(q, &b);
    return (uint32_t)b;
}

/* the shadow of (group, buffer), or NULL; with g_lock held */
static Shadow *find(void *group, uint32_t buffer)
{
    for (int i = 0; i < 256; i++)
        if (g_shadows[i].group == group && g_shadows[i].buffer == buffer && g_shadows[i].guest)
            return &g_shadows[i];
    return NULL;
}

/* the shadow of (group, buffer), made or grown to size (a grown one mirrors nothing yet); NULL when there is no room */
static Shadow *shadow_for(void *group, uint32_t buffer, uint32_t size)
{
    Shadow *s = NULL, *spare = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < 256 && !s; i++) {
        if (g_shadows[i].group == group && g_shadows[i].buffer == buffer)
            s = &g_shadows[i];
        else if (!spare && !g_shadows[i].host)
            spare = &g_shadows[i];   /* empty, or an unmapped one to recycle */
    }
    if (!s && spare) {
        m32_free(spare->guest);
        *spare = (Shadow){ group, buffer, 0, 0, NULL, 0, 0, 0, 0 };
        s = spare;
    }
    if (s && s->cap < size) {
        m32_free(s->guest);
        s->guest = m32_malloc(size);
        s->cap = s->guest ? size : 0;
        s->mirror = 0;
    }
    pthread_mutex_unlock(&g_lock);
    return s && s->guest ? s : NULL;
}

static Shadow *mapped(unsigned target)
{
    void *group = share_group();
    uint32_t buffer = bound(target);
    for (int i = 0; i < 256; i++)
        if (g_shadows[i].group == group && g_shadows[i].buffer == buffer && g_shadows[i].host)
            return &g_shadows[i];
    return NULL;
}

#define RET(v) do { m32_ret(cpu, (uint32_t)(v), 0, 0); return OCERZ_STEP_OK; } while (0)

/* whole: the mapping is the whole buffer, of size bytes, and the shadow may be (or become) its mirror */
static int map_common(OcerzCPU *cpu, unsigned target, uint32_t buffer, void *group, uint8_t *host, uint32_t size,
                      int whole)
{
    if (!host)
        RET(0);
    Shadow *s = shadow_for(group, buffer, size ? size : 1);
    if (!s) {
        fprintf(stderr, "ocerz: m32: glMapBuffer: no guest shadow for %u bytes\n", (unsigned)size);
        gl.unmap(target);
        RET(0);
    }
    int keep = whole && mirrored(target);
    if (!(keep && s->mirror))
        memcpy(m32_h(s->guest), host, size);
    if (whole)
        s->bufsize = size;
    s->mirror = keep;
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
    uint32_t buffer = bound(target), size = 0;
    void *group = share_group();
    pthread_mutex_lock(&g_lock);
    Shadow *s = mirrored(target) ? find(group, buffer) : NULL;
    if (s)
        size = s->bufsize;
    pthread_mutex_unlock(&g_lock);
    if (!size) {
        int q = 0;
        gl.buffer_iv(target, 0x8764, &q);   /* GL_BUFFER_SIZE */
        size = (uint32_t)q;
    }
    return map_common(cpu, target, buffer, group, gl.map(target, m32_arg(cpu, 1)), size, 1);
}
/* glMapBufferRange(target, offset, length, access): the shadow holds just the range, and mirrors nothing */
static int sp_glMapBufferRange(struct OcerzVM *vm, OcerzCPU *cpu)
{
    gl_init();
    unsigned target = m32_arg(cpu, 0);
    uint32_t len = m32_arg(cpu, 2);
    return map_common(cpu, target, bound(target), share_group(),
                      gl.map_range(target, (int32_t)m32_arg(cpu, 1), len, m32_arg(cpu, 3)), len, 0);
}

/* glBufferData(target, size, data, usage): the buffer's new size; without data its contents are undefined, which any
 * shadow mirrors, and with data the shadow takes them too */
static int sp_glBufferData(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    gl_init();
    unsigned target = m32_arg(cpu, 0);
    int32_t size = (int32_t)m32_arg(cpu, 1);
    uint32_t data = m32_arg(cpu, 2);
    gl.buffer_data(target, size, data ? m32_host(data) : NULL, m32_arg(cpu, 3));
    if (mirrored(target)) {
        void *group = share_group();
        uint32_t buffer = bound(target);
        pthread_mutex_lock(&g_lock);
        Shadow *s = find(group, buffer);
        if (s) {
            s->bufsize = size > 0 ? (uint32_t)size : 0;
            if (!data)
                s->mirror = 1;   /* undefined contents: whatever the shadow holds will do */
            else if (s->cap >= (uint32_t)size && size > 0)
                memcpy(m32_h(s->guest), m32_host(data), (size_t)size), s->mirror = 1;
            else
                s->mirror = 0;
        }
        pthread_mutex_unlock(&g_lock);
    }
    RET(0);
}
/* glBufferSubData(target, offset, size, data): written into the mirror too */
static int sp_glBufferSubData(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    gl_init();
    unsigned target = m32_arg(cpu, 0);
    int32_t off = (int32_t)m32_arg(cpu, 1), size = (int32_t)m32_arg(cpu, 2);
    uint32_t data = m32_arg(cpu, 3);
    gl.buffer_sub_data(target, off, size, data ? m32_host(data) : NULL);
    if (mirrored(target)) {
        void *group = share_group();
        uint32_t buffer = bound(target);
        pthread_mutex_lock(&g_lock);
        Shadow *s = find(group, buffer);
        if (s && s->mirror) {
            if (data && off >= 0 && size >= 0 && (uint64_t)off + (uint64_t)size <= s->cap)
                memcpy(m32_h(s->guest + (uint32_t)off), m32_host(data), (size_t)size);
            else
                s->mirror = 0;
        }
        pthread_mutex_unlock(&g_lock);
    }
    RET(0);
}
/* glDeleteBuffers(n, buffers): their shadows go, so a name made again starts with none */
static int sp_glDeleteBuffers(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    gl_init();
    int32_t n = (int32_t)m32_arg(cpu, 0);
    uint32_t names = m32_arg(cpu, 1);
    void *group = share_group();
    pthread_mutex_lock(&g_lock);
    for (int32_t i = 0; names && i < n; i++) {
        Shadow *s = find(group, m32_rd(names + 4 * (uint32_t)i));
        if (s) {
            m32_free(s->guest);
            *s = (Shadow){ 0 };
        }
    }
    pthread_mutex_unlock(&g_lock);
    for (int32_t i = 0; names && i < n && i < 65536; i++)
        unbind_deleted(m32_rd(names + 4 * (uint32_t)i));
    if (n >= 0)
        gl.delete_buffers(n, names ? (const uint32_t *)m32_h(names) : NULL);
    RET(0);
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

/* glutInit(int *argcp, char **argv): GLUT reads the whole argument vector and takes its own options out of it, and the
 * guest's vector holds 4-byte pointers; crossed, GLUT read argv[0] alone right.  The host gets a vector of its own, and
 * the guest's count and vector come back without what GLUT took. */
static int sp_glutInit(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    static void (*init)(int *, char **);
    if (!init)
        init = (void (*)(int *, char **))ocerz_bridge_host_symbol("/System/Library/Frameworks/GLUT.framework/Versions/A/GLUT",
                                                                  "glutInit");
    uint32_t pargc = m32_arg(cpu, 0), argv = m32_arg(cpu, 1);
    int argc = pargc ? (int32_t)m32_rd(pargc) : 0, was = argc;
    if (argc < 0 || argc > 4096 || !argv)
        argc = was = 0;
    char **hv = calloc((size_t)argc + 1, sizeof *hv);
    for (int i = 0; hv && i < argc; i++)
        hv[i] = m32_rd(argv + 4 * (uint32_t)i) ? m32_h(m32_rd(argv + 4 * (uint32_t)i)) : NULL;
    if (init && hv)
        init(&argc, hv);
    for (int i = 0; hv && i < argc && i < was; i++)
        m32_wr(argv + 4 * (uint32_t)i, m32_handle(hv[i]));
    if (hv && argc < was)
        m32_wr(argv + 4 * (uint32_t)argc, 0);
    if (pargc && hv)
        m32_wr(pargc, (uint32_t)argc);
    free(hv);
    m32_ret(cpu, 0, 0, 0);
    return OCERZ_STEP_OK;
}

const M32SpecialEntry m32_gl_specials[] = {
    { "_glutInit", sp_glutInit },
    { "_CGLSetCurrentContext", sp_CGLSetCurrentContext }, { "_CGLLockContext", sp_CGLLockContext },
    { "_CGLUnlockContext", sp_CGLUnlockContext },
    { "_CGLSetParameter", sp_CGLSetParameter },
    { "_glShaderSource", sp_glShaderSource }, { "_glShaderSourceARB", sp_glShaderSourceARB },
    { "_glMapBuffer", sp_glMapBuffer }, { "_glMapBufferARB", sp_glMapBuffer },
    { "_glMapBufferRange", sp_glMapBufferRange },
    { "_glFlushMappedBufferRange", sp_glFlushMappedBufferRange },
    { "_glFlushMappedBufferRangeAPPLE", sp_glFlushMappedBufferRangeAPPLE },
    { "_glUnmapBuffer", sp_glUnmapBuffer }, { "_glUnmapBufferARB", sp_glUnmapBuffer },
    { "_glBufferData", sp_glBufferData }, { "_glBufferDataARB", sp_glBufferData },
    { "_glBufferSubData", sp_glBufferSubData }, { "_glBufferSubDataARB", sp_glBufferSubData },
    { "_glDeleteBuffers", sp_glDeleteBuffers }, { "_glDeleteBuffersARB", sp_glDeleteBuffers },
    { NULL, NULL }
};

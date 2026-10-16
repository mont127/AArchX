/*
 * AGL, Carbon's OpenGL, for i386 guests (include/ocerz/m32.h).  Apple silicon still exports AGL and it still works:
 * a context set on a compositing window with aglSetWindowRef draws there (a native probe cleared one and read the
 * color back).  No SDK has AGL's headers any more, so these specials keep agl.h's documented signatures and call the
 * host's AGL:
 *
 *   objects    pixel formats, contexts and renderer infos are guest handles of the host's
 *   devices    AGL's GDHandles are QuickDraw's, which m32 makes in guest memory (src/m32_qd.c): a list of them
 *              chooses among every display, as no list does
 *   drawables  an AGLDrawable is a QuickDraw port.  64-bit AGL has no ports (aglSetDrawable refuses every one), so a
 *              window's port is set as its window with aglSetWindowRef, a GWorld's as its pixels with
 *              aglSetOffScreen, and NULL detaches the context; aglGetDrawable answers the port that was set
 *   parameters aglSetInteger's AGL_CLIP_REGION is a RgnHandle, the rest are GLint arrays, read and written as many
 *              as each parameter has
 *
 * GLboolean results are a byte in AL.  ponytail: pbuffers and aglUseFont are not provided.
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ocerz/interp.h"
#include "ocerz/m32.h"

#define RET(v) do { m32_ret(cpu, (uint32_t)(v), 0, 0); return OCERZ_STEP_OK; } while (0)
#define A(i) m32_arg(cpu, i)
#define H(i) m32_host(A(i))

enum { AGL_SWAP_RECT = 200, AGL_BUFFER_RECT = 202, AGL_CLIP_REGION = 254, AGL_SURFACE_BACKING_SIZE = 304,
       AGL_BAD_DRAWABLE = 10006 };

typedef void *Obj;
static struct {
    Obj (*choose)(const void *, int, const int *);
    Obj (*create_pixel_format)(const int *);
    void (*destroy_pixel_format)(Obj);
    Obj (*next_pixel_format)(Obj);
    unsigned char (*describe_pixel_format)(Obj, int, int *);
    Obj (*query_renderer_info)(const void *, int);
    void (*destroy_renderer_info)(Obj);
    Obj (*next_renderer_info)(Obj);
    unsigned char (*describe_renderer)(Obj, int, int *);
    Obj (*create_context)(Obj, Obj);
    unsigned char (*destroy_context)(Obj);
    unsigned char (*copy_context)(Obj, Obj, unsigned);
    unsigned char (*update_context)(Obj);
    unsigned char (*set_current)(Obj);
    Obj (*get_current)(void);
    unsigned char (*set_window_ref)(Obj, void *);
    void *(*get_window_ref)(Obj);
    unsigned char (*set_hiview_ref)(Obj, void *);
    void *(*get_hiview_ref)(Obj);
    unsigned char (*set_off_screen)(Obj, int, int, int, void *);
    unsigned char (*set_full_screen)(Obj, int, int, int, int);
    unsigned char (*set_virtual_screen)(Obj, int);
    int (*get_virtual_screen)(Obj);
    void (*get_version)(int *, int *);
    unsigned char (*configure)(unsigned, unsigned);
    void (*swap_buffers)(Obj);
    unsigned char (*enable)(Obj, unsigned);
    unsigned char (*disable)(Obj, unsigned);
    unsigned char (*is_enabled)(Obj, unsigned);
    unsigned char (*set_integer)(Obj, unsigned, const int *);
    unsigned char (*get_integer)(Obj, unsigned, int *);
    unsigned (*get_error)(void);
    const char *(*error_string)(unsigned);
    void (*reset_library)(void);
    unsigned char (*get_cgl_context)(Obj, void **);
    unsigned char (*get_cgl_pixel_format)(Obj, void **);
} agl;

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_ok;
static void load(void)
{
    void *h = dlopen("/System/Library/Frameworks/AGL.framework/AGL", RTLD_LAZY | RTLD_LOCAL);
    int ok = h != NULL;
#define L(field, name) ok &= (*(void **)&agl.field = h ? dlsym(h, name) : NULL) != NULL
    L(choose, "aglChoosePixelFormat"); L(create_pixel_format, "aglCreatePixelFormat");
    L(destroy_pixel_format, "aglDestroyPixelFormat"); L(next_pixel_format, "aglNextPixelFormat");
    L(describe_pixel_format, "aglDescribePixelFormat"); L(query_renderer_info, "aglQueryRendererInfo");
    L(destroy_renderer_info, "aglDestroyRendererInfo"); L(next_renderer_info, "aglNextRendererInfo");
    L(describe_renderer, "aglDescribeRenderer"); L(create_context, "aglCreateContext");
    L(destroy_context, "aglDestroyContext"); L(copy_context, "aglCopyContext"); L(update_context, "aglUpdateContext");
    L(set_current, "aglSetCurrentContext"); L(get_current, "aglGetCurrentContext");
    L(set_window_ref, "aglSetWindowRef"); L(get_window_ref, "aglGetWindowRef"); L(set_hiview_ref, "aglSetHIViewRef");
    L(get_hiview_ref, "aglGetHIViewRef"); L(set_off_screen, "aglSetOffScreen"); L(set_full_screen, "aglSetFullScreen");
    L(set_virtual_screen, "aglSetVirtualScreen"); L(get_virtual_screen, "aglGetVirtualScreen");
    L(get_version, "aglGetVersion"); L(configure, "aglConfigure"); L(swap_buffers, "aglSwapBuffers");
    L(enable, "aglEnable"); L(disable, "aglDisable"); L(is_enabled, "aglIsEnabled"); L(set_integer, "aglSetInteger");
    L(get_integer, "aglGetInteger"); L(get_error, "aglGetError"); L(error_string, "aglErrorString");
    L(reset_library, "aglResetLibrary"); L(get_cgl_context, "aglGetCGLContext");
    L(get_cgl_pixel_format, "aglGetCGLPixelFormat");
#undef L
    g_ok = ok;
    if (!ok)
        fprintf(stderr, "ocerz: m32: AGL: the host's AGL is missing a function\n");
}
static int ready(void) { pthread_once(&g_once, load); return g_ok; }

/* the attribute list, AGL_NONE-terminated GLints, copied out (at most 255 and the terminator) */
static const int *attribs(uint32_t g, int *buf, int n)
{
    if (!g)
        return NULL;
    int i = 0;
    for (; i < n - 1 && (int32_t)m32_rd(g + 4 * (uint32_t)i); i++)
        buf[i] = (int32_t)m32_rd(g + 4 * (uint32_t)i);
    buf[i] = 0;
    return buf;
}

/* ---- pixel formats and renderers ---- */

static int agl_ChoosePixelFormat(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    int buf[256];
    RET(ready() ? m32_handle(agl.choose(NULL, 0, attribs(A(2), buf, 256))) : 0);
}
static int agl_CreatePixelFormat(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    int buf[256];
    RET(ready() ? m32_handle(agl.create_pixel_format(attribs(A(0), buf, 256))) : 0);
}
static int agl_DestroyPixelFormat(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (ready() && A(0)) agl.destroy_pixel_format(H(0)); RET(0); }
static int agl_NextPixelFormat(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) ? m32_handle(agl.next_pixel_format(H(0))) : 0); }
static int agl_DescribePixelFormat(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    int v = 0;
    unsigned char ok = ready() && A(0) ? agl.describe_pixel_format(H(0), (int32_t)A(1), &v) : 0;
    if (A(2))
        m32_wr(A(2), (uint32_t)v);
    RET(ok);
}
static int agl_QueryRendererInfo(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() ? m32_handle(agl.query_renderer_info(NULL, 0)) : 0); }
static int agl_DestroyRendererInfo(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (ready() && A(0)) agl.destroy_renderer_info(H(0)); RET(0); }
static int agl_NextRendererInfo(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) ? m32_handle(agl.next_renderer_info(H(0))) : 0); }
static int agl_DescribeRenderer(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    int v = 0;
    unsigned char ok = ready() && A(0) ? agl.describe_renderer(H(0), (int32_t)A(1), &v) : 0;
    if (A(2))
        m32_wr(A(2), (uint32_t)v);
    RET(ok);
}

/* ---- contexts and drawables ---- */

/* the port each context draws into, as aglGetDrawable answers it */
static struct { Obj ctx; uint32_t port; } g_drawables[64];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void remember(Obj ctx, uint32_t port)
{
    pthread_mutex_lock(&g_lock);
    int slot = -1;
    for (int i = 0; i < 64 && slot < 0; i++)
        if (g_drawables[i].ctx == ctx)
            slot = i;
    for (int i = 0; i < 64 && slot < 0 && port; i++)
        if (!g_drawables[i].ctx)
            slot = i;
    if (slot >= 0) {
        g_drawables[slot].ctx = port ? ctx : NULL;
        g_drawables[slot].port = port;
    }
    pthread_mutex_unlock(&g_lock);
}

static uint32_t drawable_of(Obj ctx)
{
    uint32_t port = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < 64; i++)
        if (g_drawables[i].ctx == ctx)
            port = g_drawables[i].port;
    pthread_mutex_unlock(&g_lock);
    return port;
}

static int agl_CreateContext(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) ? m32_handle(agl.create_context(H(0), H(1))) : 0); }
static int agl_DestroyContext(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    if (!ready() || !A(0))
        RET(0);
    remember(H(0), 0);
    RET(agl.destroy_context(H(0)));
}
static int agl_CopyContext(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() ? agl.copy_context(H(0), H(1), A(2)) : 0); }
static int agl_UpdateContext(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) ? agl.update_context(H(0)) : 0); }
static int agl_SetCurrentContext(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() ? agl.set_current(H(0)) : 0); }
static int agl_GetCurrentContext(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() ? m32_handle(agl.get_current()) : 0); }

/* aglSetDrawable(AGLContext, AGLDrawable): the port's window, or its GWorld's pixels; NULL detaches */
static int agl_SetDrawable(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    if (!ready() || !A(0))
        RET(0);
    Obj ctx = H(0);
    uint32_t port = A(1);
    unsigned char ok;
    void *win = port ? m32_qd_port_window(port) : NULL;
    int w, h;
    uint32_t rowbytes, pixels;
    if (!port)
        ok = agl.set_window_ref(ctx, NULL);
    else if (win)
        ok = agl.set_window_ref(ctx, win);
    else if (m32_qd_port_pixels(port, &w, &h, &rowbytes, &pixels))
        ok = agl.set_off_screen(ctx, w, h, (int)rowbytes, m32_h(pixels));
    else {
        m32_log_once("AGL: aglSetDrawable with something that is no QuickDraw port (AGL_BAD_DRAWABLE):", "aglSetDrawable");
        RET(0);
    }
    remember(ctx, ok ? port : 0);
    RET(ok);
}
static int agl_GetDrawable(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(A(0) ? drawable_of(H(0)) : 0); }
static int agl_SetOffScreen(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    RET(ready() && A(0) ? agl.set_off_screen(H(0), (int32_t)A(1), (int32_t)A(2), (int32_t)A(3), A(4) ? m32_h(A(4)) : NULL) : 0);
}
static int agl_SetFullScreen(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    RET(ready() && A(0) ? agl.set_full_screen(H(0), (int32_t)A(1), (int32_t)A(2), (int32_t)A(3), (int32_t)A(4)) : 0);
}
static int agl_SetWindowRef(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) ? agl.set_window_ref(H(0), H(1)) : 0); }
static int agl_GetWindowRef(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) ? m32_handle(agl.get_window_ref(H(0))) : 0); }
static int agl_SetHIViewRef(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) ? agl.set_hiview_ref(H(0), H(1)) : 0); }
static int agl_GetHIViewRef(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) ? m32_handle(agl.get_hiview_ref(H(0))) : 0); }
static int agl_SetVirtualScreen(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) ? agl.set_virtual_screen(H(0), (int32_t)A(1)) : 0); }
static int agl_GetVirtualScreen(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) ? (uint32_t)agl.get_virtual_screen(H(0)) : 0); }
static int agl_SwapBuffers(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (ready() && A(0)) agl.swap_buffers(H(0)); RET(0); }

/* ---- options ---- */

static int agl_Enable(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) ? agl.enable(H(0), A(1)) : 0); }
static int agl_Disable(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) ? agl.disable(H(0), A(1)) : 0); }
static int agl_IsEnabled(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) ? agl.is_enabled(H(0), A(1)) : 0); }

/* how many GLints a parameter has */
static int count_of(unsigned pname)
{
    return pname == AGL_SWAP_RECT || pname == AGL_BUFFER_RECT ? 4 : pname == AGL_SURFACE_BACKING_SIZE ? 2 : 1;
}

static int agl_SetInteger(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    if (!ready() || !A(0))
        RET(0);
    unsigned pname = A(1);
    if (pname == AGL_CLIP_REGION)   /* the parameter is the RgnHandle itself */
        RET(agl.set_integer(H(0), pname, (const int *)H(2)));
    int v[4] = { 0, 0, 0, 0 };
    for (int i = 0; A(2) && i < count_of(pname); i++)
        v[i] = (int32_t)m32_rd(A(2) + 4 * (uint32_t)i);
    RET(agl.set_integer(H(0), pname, A(2) ? v : NULL));
}

static int agl_GetInteger(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    if (!ready() || !A(0) || A(1) == AGL_CLIP_REGION)
        RET(0);
    int v[4] = { 0, 0, 0, 0 };
    unsigned char ok = agl.get_integer(H(0), A(1), v);
    for (int i = 0; A(2) && i < count_of(A(1)); i++)
        m32_wr(A(2) + 4 * (uint32_t)i, (uint32_t)v[i]);
    RET(ok);
}

static int agl_Configure(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() ? agl.configure(A(0), A(1)) : 0); }
static int agl_ResetLibrary(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (ready()) agl.reset_library(); RET(0); }
static int agl_GetVersion(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    int major = 0, minor = 0;
    if (ready())
        agl.get_version(&major, &minor);
    if (A(0))
        m32_wr(A(0), (uint32_t)major);
    if (A(1))
        m32_wr(A(1), (uint32_t)minor);
    RET(0);
}
static int agl_GetError(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() ? agl.get_error() : 0); }
static int agl_ErrorString(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    static uint32_t strings[65];   /* AGL_NO_ERROR, then the errors from 10000 */
    unsigned code = A(0), slot = code == 0 ? 0 : code >= 10000 && code < 10064 ? code - 9999 : 65;
    if (!ready())
        RET(0);
    if (slot == 65)
        RET(m32_cstring(agl.error_string(code)));
    if (!strings[slot])
        strings[slot] = m32_cstring(agl.error_string(code));
    RET(strings[slot]);
}

/* the CGL objects under AGL's: the guest calls CGL with them as with any */
static int agl_GetCGLContext(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    void *cgl = NULL;
    unsigned char ok = ready() && A(0) ? agl.get_cgl_context(H(0), &cgl) : 0;
    if (A(1))
        m32_wr(A(1), m32_handle(cgl));
    RET(ok);
}
static int agl_GetCGLPixelFormat(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    void *cgl = NULL;
    unsigned char ok = ready() && A(0) ? agl.get_cgl_pixel_format(H(0), &cgl) : 0;
    if (A(1))
        m32_wr(A(1), m32_handle(cgl));
    RET(ok);
}

const M32SpecialEntry m32_agl_specials[] = {
    { "_aglChoosePixelFormat", agl_ChoosePixelFormat }, { "_aglCreatePixelFormat", agl_CreatePixelFormat },
    { "_aglDestroyPixelFormat", agl_DestroyPixelFormat }, { "_aglNextPixelFormat", agl_NextPixelFormat },
    { "_aglDescribePixelFormat", agl_DescribePixelFormat }, { "_aglQueryRendererInfo", agl_QueryRendererInfo },
    { "_aglDestroyRendererInfo", agl_DestroyRendererInfo }, { "_aglNextRendererInfo", agl_NextRendererInfo },
    { "_aglDescribeRenderer", agl_DescribeRenderer }, { "_aglCreateContext", agl_CreateContext },
    { "_aglDestroyContext", agl_DestroyContext }, { "_aglCopyContext", agl_CopyContext },
    { "_aglUpdateContext", agl_UpdateContext }, { "_aglSetCurrentContext", agl_SetCurrentContext },
    { "_aglGetCurrentContext", agl_GetCurrentContext }, { "_aglSetDrawable", agl_SetDrawable },
    { "_aglGetDrawable", agl_GetDrawable }, { "_aglSetOffScreen", agl_SetOffScreen },
    { "_aglSetFullScreen", agl_SetFullScreen }, { "_aglSetWindowRef", agl_SetWindowRef },
    { "_aglGetWindowRef", agl_GetWindowRef }, { "_aglSetHIViewRef", agl_SetHIViewRef },
    { "_aglGetHIViewRef", agl_GetHIViewRef }, { "_aglSetVirtualScreen", agl_SetVirtualScreen },
    { "_aglGetVirtualScreen", agl_GetVirtualScreen }, { "_aglSwapBuffers", agl_SwapBuffers },
    { "_aglEnable", agl_Enable }, { "_aglDisable", agl_Disable }, { "_aglIsEnabled", agl_IsEnabled },
    { "_aglSetInteger", agl_SetInteger }, { "_aglGetInteger", agl_GetInteger }, { "_aglConfigure", agl_Configure },
    { "_aglResetLibrary", agl_ResetLibrary }, { "_aglGetVersion", agl_GetVersion }, { "_aglGetError", agl_GetError },
    { "_aglErrorString", agl_ErrorString }, { "_aglGetCGLContext", agl_GetCGLContext },
    { "_aglGetCGLPixelFormat", agl_GetCGLPixelFormat },
    { NULL, NULL }
};

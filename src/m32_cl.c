/*
 * OpenCL for i386 guests, on the Mac's own arm64 OpenCL (include/ocerz/m32.h).
 *
 * Every OpenCL 1.2 call and the GL sharing calls cross by hand, because almost none has the same shape in the two
 * widths: objects are host pointers the guest holds as handles (m32_handle), sizes, offsets, origins and regions are
 * size_t, which is 4 bytes in the guest and 8 here, and cl_bitfield arguments (memory and map flags, device types,
 * queue properties) are 64-bit, taking two of the guest's argument words.  The framework is opened on the first call,
 * so a program that never uses OpenCL never loads it.
 *
 * Information queries are answered into a host buffer and converted by the parameter's type: size_t values and
 * arrays, object values and arrays, and property lists shrink to the guest's 4-byte words; everything else (cl_uint,
 * cl_ulong, bitfields, strings, image formats) has one layout and is answered in place.
 *
 * clSetKernelArg's 4-byte cl_mem is widened to the 8-byte host object when the argument's address qualifier says it
 * is a buffer or an image (global or constant; Apple answers clGetKernelArgInfo without -cl-kernel-arg-info), and a
 * 4-byte value that is a sampler or memory object this file handed out is widened too, for a sampler (private, like
 * an int) and for an implementation that answers no qualifier.  Apple accepts any size for an int argument, so a
 * retry on CL_INVALID_ARG_SIZE could not tell them apart.
 *
 * A mapping (clEnqueueMapBuffer, clEnqueueMapImage) outside the guest's window gets a shadow in the guest heap, filled
 * from the mapping unless the guest maps to invalidate it, and copied back at unmap when the map was for writing.
 * Maps are made blocking, so the shadow is filled before the call returns; the event the guest asked for is complete.
 *
 * Callbacks: a build's (clBuildProgram, clCompileProgram, clLinkProgram) runs on the guest's thread once the build,
 * made synchronous, is done, which the API allows; a context's, an event's and a memory object's destructor run as
 * m32_callback callbacks on whatever thread the host calls them.  The clLogMessagesTo*APPLE functions passed as a
 * context's callback are the host's own.  clEnqueueNativeKernel answers CL_INVALID_OPERATION, as Apple's GPU devices
 * do, and clGetExtensionFunctionAddress answers NULL: a host entry point is nothing a guest can call.
 *
 * OCERZ_NO_M32_OPENCL=1 leaves the framework closed: no platform, and contexts fail with CL_DEVICE_NOT_FOUND, as on a
 * Mac with no OpenCL device.
 *
 * ponytail: the shadow and object lists are linear under one lock; objects are remembered for the run.
 */
#define CL_SILENCE_DEPRECATION
#include <OpenCL/opencl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ocerz/interp.h"
#include "ocerz/m32.h"

/* the OpenCL 1.0 and 1.1 calls an old program makes (clCreateImage2D, clEnqueueMarker, ...) are deprecated here and
   still exported */
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#define CL_LIB "/System/Library/Frameworks/OpenCL.framework/Versions/A/OpenCL"
#define RET(v) (m32_ret(cpu, (uint32_t)(v), 0, 0), OCERZ_STEP_OK)
#define A(n) m32_arg(cpu, n)

#define CL_FUNCS(X) \
    X(clGetPlatformIDs) X(clGetPlatformInfo) X(clGetDeviceIDs) X(clGetDeviceInfo) X(clCreateSubDevices) \
    X(clRetainDevice) X(clReleaseDevice) X(clCreateContext) X(clCreateContextFromType) X(clRetainContext) \
    X(clReleaseContext) X(clGetContextInfo) X(clCreateCommandQueue) X(clRetainCommandQueue) \
    X(clReleaseCommandQueue) X(clGetCommandQueueInfo) X(clCreateBuffer) X(clCreateSubBuffer) X(clCreateImage) \
    X(clCreateImage2D) X(clCreateImage3D) X(clRetainMemObject) X(clReleaseMemObject) \
    X(clGetSupportedImageFormats) X(clGetMemObjectInfo) X(clGetImageInfo) X(clSetMemObjectDestructorCallback) \
    X(clCreateSampler) X(clRetainSampler) X(clReleaseSampler) X(clGetSamplerInfo) X(clCreateProgramWithSource) \
    X(clCreateProgramWithBinary) X(clCreateProgramWithBuiltInKernels) X(clRetainProgram) X(clReleaseProgram) \
    X(clBuildProgram) X(clCompileProgram) X(clLinkProgram) X(clUnloadCompiler) X(clUnloadPlatformCompiler) \
    X(clGetProgramInfo) X(clGetProgramBuildInfo) X(clCreateKernel) X(clCreateKernelsInProgram) X(clRetainKernel) \
    X(clReleaseKernel) X(clSetKernelArg) X(clGetKernelInfo) X(clGetKernelArgInfo) X(clGetKernelWorkGroupInfo) \
    X(clWaitForEvents) X(clGetEventInfo) X(clCreateUserEvent) X(clRetainEvent) X(clReleaseEvent) \
    X(clSetUserEventStatus) X(clSetEventCallback) X(clGetEventProfilingInfo) X(clFlush) X(clFinish) \
    X(clEnqueueReadBuffer) X(clEnqueueReadBufferRect) X(clEnqueueWriteBuffer) X(clEnqueueWriteBufferRect) \
    X(clEnqueueFillBuffer) X(clEnqueueCopyBuffer) X(clEnqueueCopyBufferRect) X(clEnqueueReadImage) \
    X(clEnqueueWriteImage) X(clEnqueueFillImage) X(clEnqueueCopyImage) X(clEnqueueCopyImageToBuffer) \
    X(clEnqueueCopyBufferToImage) X(clEnqueueMapBuffer) X(clEnqueueMapImage) X(clEnqueueUnmapMemObject) \
    X(clEnqueueMigrateMemObjects) X(clEnqueueNDRangeKernel) X(clEnqueueTask) X(clEnqueueMarker) \
    X(clEnqueueWaitForEvents) X(clEnqueueBarrier) X(clEnqueueMarkerWithWaitList) X(clEnqueueBarrierWithWaitList) \
    X(clCreateFromGLBuffer) X(clCreateFromGLTexture) X(clCreateFromGLTexture2D) X(clCreateFromGLTexture3D) \
    X(clCreateFromGLRenderbuffer) X(clGetGLObjectInfo) X(clGetGLTextureInfo) X(clEnqueueAcquireGLObjects) \
    X(clEnqueueReleaseGLObjects) X(clLogMessagesToSystemLogAPPLE) X(clLogMessagesToStdoutAPPLE) \
    X(clLogMessagesToStderrAPPLE)

static struct {
#define X(f) __typeof__(&f) f;
    CL_FUNCS(X)
#undef X
} cl;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_loaded;

static void cl_load(void)
{
    if (getenv("OCERZ_NO_M32_OPENCL")) {
        m32_log_once("OCERZ_NO_M32_OPENCL: every OpenCL call answers no platform or device", "");
        return;
    }
    void *h = dlopen(CL_LIB, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        m32_log_once("OpenCL did not open, so every call answers no platform:", CL_LIB);
        return;
    }
#define X(f) cl.f = (__typeof__(&f))dlsym(h, #f);
    CL_FUNCS(X)
#undef X
    g_loaded = 1;
}

/* the host function, or NULL when the framework or the function is not there */
#define FN(f) (pthread_once(&g_once, cl_load), cl.f)
/* a special whose host function is missing answers this, the code a call with no such object would get */
#define NEED(f, err) do { if (!FN(f)) { m32_log_once("OpenCL has no", #f); return RET(err); } } while (0)

/* ---- the objects this file handed out ---- */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t *g_objs;   /* guest handles of memory objects and samplers */
static uint8_t *g_obj_sampler;
static size_t g_nobjs, g_capobjs;

static uint32_t remember(void *host, int sampler)
{
    uint32_t g = m32_handle(host);
    if (!g)
        return 0;
    pthread_mutex_lock(&g_lock);
    size_t i;
    for (i = 0; i < g_nobjs && g_objs[i] != g; i++)
        ;
    if (i == g_nobjs) {
        if (g_nobjs == g_capobjs) {
            g_capobjs = g_capobjs ? g_capobjs * 2 : 64;
            g_objs = realloc(g_objs, g_capobjs * sizeof *g_objs);
            g_obj_sampler = realloc(g_obj_sampler, g_capobjs);
        }
        g_nobjs++;
    }
    g_objs[i] = g;
    g_obj_sampler[i] = (uint8_t)sampler;
    pthread_mutex_unlock(&g_lock);
    return g;
}

/* 1 for a memory object, 2 for a sampler, 0 for anything else */
static int remembered(uint32_t g)
{
    int r = 0;
    if (!m32_is_handle(g))
        return 0;
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_nobjs; i++)
        if (g_objs[i] == g) {
            r = g_obj_sampler[i] ? 2 : 1;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    return r;
}

/* ---- conversions ---- */

static void *P(uint32_t g) { return g ? m32_h(g) : NULL; }   /* guest memory */
static void put(uint32_t g, uint32_t v) { if (g) m32_wr(g, v); }
static uint32_t narrow(size_t v) { return v > 0xffffffffu ? 0xffffffffu : (uint32_t)v; }

/* an array of n guest objects as host objects; up to 16 in place, more on the heap (objs_done frees them) */
typedef struct Objs { void **p; void *in[16]; } Objs;
static void **objs_in(Objs *o, uint32_t g, uint32_t n)
{
    o->p = NULL;
    if (!g || !n)
        return NULL;
    o->p = n <= 16 ? o->in : malloc(n * sizeof *o->p);
    for (uint32_t i = 0; i < n; i++)
        o->p[i] = m32_host(m32_rd(g + 4 * i));
    return o->p;
}
static void objs_done(Objs *o) { if (o->p && o->p != o->in) free(o->p); }

/* three guest size_t values (an origin, a region, a work size) as host ones; NULL stays NULL */
static size_t *sizes_in(size_t out[3], uint32_t g, uint32_t n)
{
    if (!g)
        return NULL;
    for (uint32_t i = 0; i < 3; i++)
        out[i] = i < n ? m32_rd(g + 4 * i) : 1;
    return out;
}

/* whether a property's value is an object: the platform, a GL context or display, a CGL share group (Apple's key
 * and the KHR one), which cross as the host pointer; other values are numbers and flags */
static int prop_is_object(uint32_t key)
{
    return key == CL_CONTEXT_PLATFORM || key == 0x10000000u /* CL_CONTEXT_PROPERTY_USE_CGL_SHAREGROUP_APPLE */ ||
           (key >= 0x2008u && key <= 0x200cu) /* CL_GL_CONTEXT_KHR .. CL_CGL_SHAREGROUP_KHR */;
}

/* a zero-terminated list of key and value words (cl_context_properties, cl_device_partition_property) */
static intptr_t *props_in(uint32_t g)
{
    if (!g)
        return NULL;
    uint32_t n = 0;
    while (m32_rd(g + 4 * n))
        n += 2;
    intptr_t *p = calloc(n + 1, sizeof *p);
    for (uint32_t i = 0; i < n; i += 2) {
        uint32_t key = m32_rd(g + 4 * i), v = m32_rd(g + 4 * i + 4);
        p[i] = key;
        p[i + 1] = prop_is_object(key) ? (intptr_t)m32_host(v) : (intptr_t)v;
    }
    return p;
}

static void event_out(uint32_t g, cl_int e, cl_event ev)
{
    if (g && e == CL_SUCCESS)
        m32_wr(g, m32_handle(ev));
}

/* ---- information queries ---- */

enum { V_RAW, V_SIZE, V_OBJ, V_PROPS, V_BINARIES };

static int param_kind(cl_uint p)
{
    switch (p) {
    case CL_DEVICE_MAX_WORK_GROUP_SIZE: case CL_DEVICE_MAX_WORK_ITEM_SIZES: case CL_DEVICE_IMAGE2D_MAX_WIDTH:
    case CL_DEVICE_IMAGE2D_MAX_HEIGHT: case CL_DEVICE_IMAGE3D_MAX_WIDTH: case CL_DEVICE_IMAGE3D_MAX_HEIGHT:
    case CL_DEVICE_IMAGE3D_MAX_DEPTH: case CL_DEVICE_IMAGE_MAX_BUFFER_SIZE: case CL_DEVICE_IMAGE_MAX_ARRAY_SIZE:
    case CL_DEVICE_MAX_PARAMETER_SIZE: case CL_DEVICE_PROFILING_TIMER_RESOLUTION: case CL_DEVICE_PRINTF_BUFFER_SIZE:
    case CL_DEVICE_PARTITION_PROPERTIES: case CL_DEVICE_PARTITION_TYPE:
    case CL_MEM_SIZE: case CL_MEM_OFFSET:
    case CL_IMAGE_ELEMENT_SIZE: case CL_IMAGE_ROW_PITCH: case CL_IMAGE_SLICE_PITCH: case CL_IMAGE_WIDTH:
    case CL_IMAGE_HEIGHT: case CL_IMAGE_DEPTH: case CL_IMAGE_ARRAY_SIZE:
    case CL_PROGRAM_BINARY_SIZES: case CL_PROGRAM_NUM_KERNELS:
    case CL_KERNEL_WORK_GROUP_SIZE: case CL_KERNEL_COMPILE_WORK_GROUP_SIZE:
    case CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE: case CL_KERNEL_GLOBAL_WORK_SIZE:
        return V_SIZE;
    case CL_DEVICE_PLATFORM: case CL_DEVICE_PARENT_DEVICE:
    case CL_CONTEXT_DEVICES:
    case CL_QUEUE_CONTEXT: case CL_QUEUE_DEVICE:
    case CL_MEM_HOST_PTR: case CL_MEM_CONTEXT: case CL_MEM_ASSOCIATED_MEMOBJECT:
    case CL_IMAGE_BUFFER:
    case CL_SAMPLER_CONTEXT:
    case CL_PROGRAM_CONTEXT: case CL_PROGRAM_DEVICES:
    case CL_KERNEL_CONTEXT: case CL_KERNEL_PROGRAM:
    case CL_EVENT_COMMAND_QUEUE: case CL_EVENT_CONTEXT:
        return V_OBJ;
    case CL_CONTEXT_PROPERTIES:
        return V_PROPS;
    case CL_PROGRAM_BINARIES:
        return V_BINARIES;
    default:
        return V_RAW;
    }
}

/* the host query with its leading arguments bound */
typedef cl_int (*InfoFn)(void *const *a, cl_uint p, size_t size, void *value, size_t *ret);

/* param, the guest's size, value and size_ret are the call's last four arguments */
static int info(OcerzCPU *cpu, InfoFn fn, void *const *a, int first)
{
    cl_uint p = A(first);
    uint32_t gsize = A(first + 1), gval = A(first + 2), gret = A(first + 3);
    int kind = param_kind(p);
    size_t hret = 0;
    cl_int e;
    if (kind == V_RAW) {
        e = fn(a, p, gsize, P(gval), gret ? &hret : NULL);
        if (e == CL_SUCCESS)
            put(gret, narrow(hret));
        return RET(e);
    }
    uint32_t n = gsize / 4;   /* the guest's words, each a host word */
    uint64_t *buf = gval ? calloc(n ? n : 1, 8) : NULL;
    if (kind == V_BINARIES && gval)
        for (uint32_t i = 0; i < n; i++)
            buf[i] = (uint64_t)(uintptr_t)P(m32_rd(gval + 4 * i));
    e = fn(a, p, gval ? (size_t)n * 8 : 0, buf, &hret);
    if (e == CL_SUCCESS) {
        uint32_t got = (uint32_t)(hret / 8);
        if (gval && kind != V_BINARIES)
            for (uint32_t i = 0; i < got && i < n; i++) {
                uint64_t v = buf[i];
                uint32_t w = kind == V_SIZE ? narrow(v)
                           : kind == V_OBJ || ((i & 1) && prop_is_object((uint32_t)buf[i - 1]))
                           ? m32_handle((void *)(uintptr_t)v)
                           : (uint32_t)v;
                m32_wr(gval + 4 * i, w);
            }
        put(gret, got * 4);
    }
    free(buf);
    return RET(e);
}

static cl_int q_platform(void *const *a, cl_uint p, size_t s, void *v, size_t *r) { return cl.clGetPlatformInfo(a[0], p, s, v, r); }
static cl_int q_device(void *const *a, cl_uint p, size_t s, void *v, size_t *r) { return cl.clGetDeviceInfo(a[0], p, s, v, r); }
static cl_int q_context(void *const *a, cl_uint p, size_t s, void *v, size_t *r) { return cl.clGetContextInfo(a[0], p, s, v, r); }
static cl_int q_queue(void *const *a, cl_uint p, size_t s, void *v, size_t *r) { return cl.clGetCommandQueueInfo(a[0], p, s, v, r); }
static cl_int q_mem(void *const *a, cl_uint p, size_t s, void *v, size_t *r) { return cl.clGetMemObjectInfo(a[0], p, s, v, r); }
static cl_int q_image(void *const *a, cl_uint p, size_t s, void *v, size_t *r) { return cl.clGetImageInfo(a[0], p, s, v, r); }
static cl_int q_sampler(void *const *a, cl_uint p, size_t s, void *v, size_t *r) { return cl.clGetSamplerInfo(a[0], p, s, v, r); }
static cl_int q_program(void *const *a, cl_uint p, size_t s, void *v, size_t *r) { return cl.clGetProgramInfo(a[0], p, s, v, r); }
static cl_int q_build(void *const *a, cl_uint p, size_t s, void *v, size_t *r) { return cl.clGetProgramBuildInfo(a[0], a[1], p, s, v, r); }
static cl_int q_kernel(void *const *a, cl_uint p, size_t s, void *v, size_t *r) { return cl.clGetKernelInfo(a[0], p, s, v, r); }
static cl_int q_arg(void *const *a, cl_uint p, size_t s, void *v, size_t *r) { return cl.clGetKernelArgInfo(a[0], (cl_uint)(uintptr_t)a[1], p, s, v, r); }
static cl_int q_group(void *const *a, cl_uint p, size_t s, void *v, size_t *r) { return cl.clGetKernelWorkGroupInfo(a[0], a[1], p, s, v, r); }
static cl_int q_event(void *const *a, cl_uint p, size_t s, void *v, size_t *r) { return cl.clGetEventInfo(a[0], p, s, v, r); }
static cl_int q_profile(void *const *a, cl_uint p, size_t s, void *v, size_t *r) { return cl.clGetEventProfilingInfo(a[0], p, s, v, r); }
static cl_int q_texture(void *const *a, cl_uint p, size_t s, void *v, size_t *r) { return cl.clGetGLTextureInfo(a[0], p, s, v, r); }

#define INFO1(name, q) \
    static int sp_##name(struct OcerzVM *vm, OcerzCPU *cpu) \
    { NEED(name, CL_INVALID_VALUE); void *a[1] = { m32_host(A(0)) }; return info(cpu, q, a, 1); }
#define INFO2(name, q) \
    static int sp_##name(struct OcerzVM *vm, OcerzCPU *cpu) \
    { NEED(name, CL_INVALID_VALUE); void *a[2] = { m32_host(A(0)), m32_host(A(1)) }; return info(cpu, q, a, 2); }
INFO1(clGetPlatformInfo, q_platform)
INFO1(clGetDeviceInfo, q_device)
INFO1(clGetContextInfo, q_context)
INFO1(clGetCommandQueueInfo, q_queue)
INFO1(clGetMemObjectInfo, q_mem)
INFO1(clGetImageInfo, q_image)
INFO1(clGetSamplerInfo, q_sampler)
INFO1(clGetProgramInfo, q_program)
INFO2(clGetProgramBuildInfo, q_build)
INFO1(clGetKernelInfo, q_kernel)
INFO2(clGetKernelWorkGroupInfo, q_group)
INFO1(clGetEventInfo, q_event)
INFO1(clGetEventProfilingInfo, q_profile)
INFO1(clGetGLTextureInfo, q_texture)

static int sp_clGetKernelArgInfo(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clGetKernelArgInfo, CL_INVALID_VALUE);
    void *a[2] = { m32_host(A(0)), (void *)(uintptr_t)A(1) };
    return info(cpu, q_arg, a, 2);
}

/* ---- retain and release: one object in, a code out ---- */

#define ONE(name, err) \
    static int sp_##name(struct OcerzVM *vm, OcerzCPU *cpu) { NEED(name, err); return RET(cl.name(m32_host(A(0)))); }
ONE(clRetainDevice, CL_INVALID_DEVICE) ONE(clReleaseDevice, CL_INVALID_DEVICE)
ONE(clRetainContext, CL_INVALID_CONTEXT) ONE(clReleaseContext, CL_INVALID_CONTEXT)
ONE(clRetainCommandQueue, CL_INVALID_COMMAND_QUEUE) ONE(clReleaseCommandQueue, CL_INVALID_COMMAND_QUEUE)
ONE(clRetainMemObject, CL_INVALID_MEM_OBJECT) ONE(clReleaseMemObject, CL_INVALID_MEM_OBJECT)
ONE(clRetainSampler, CL_INVALID_SAMPLER) ONE(clReleaseSampler, CL_INVALID_SAMPLER)
ONE(clRetainProgram, CL_INVALID_PROGRAM) ONE(clReleaseProgram, CL_INVALID_PROGRAM)
ONE(clRetainKernel, CL_INVALID_KERNEL) ONE(clReleaseKernel, CL_INVALID_KERNEL)
ONE(clRetainEvent, CL_INVALID_EVENT) ONE(clReleaseEvent, CL_INVALID_EVENT)
ONE(clFlush, CL_INVALID_COMMAND_QUEUE) ONE(clFinish, CL_INVALID_COMMAND_QUEUE)
ONE(clEnqueueBarrier, CL_INVALID_COMMAND_QUEUE) ONE(clUnloadPlatformCompiler, CL_INVALID_PLATFORM)

static int sp_clUnloadCompiler(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clUnloadCompiler, CL_SUCCESS);
    return RET(cl.clUnloadCompiler());
}

/* ---- platforms and devices ---- */

static int sp_clGetPlatformIDs(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t n = A(0), out = A(1), num = A(2);
    if (!FN(clGetPlatformIDs)) {
        put(num, 0);
        return RET(-1001);   /* CL_PLATFORM_NOT_FOUND_KHR, as the ICD loader answers with no platform */
    }
    cl_platform_id ids[16];
    cl_uint got = 0;
    cl_int e = cl.clGetPlatformIDs(out ? (n < 16 ? n : 16) : 0, out ? ids : NULL, &got);
    if (e == CL_SUCCESS && out)
        for (cl_uint i = 0; i < got && i < n && i < 16; i++)
            m32_wr(out + 4 * i, m32_handle(ids[i]));
    if (e == CL_SUCCESS)
        put(num, got);
    return RET(e);
}

/* clGetDeviceIDs(platform, cl_device_type (two words), num_entries, devices, num_devices) */
static int sp_clGetDeviceIDs(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clGetDeviceIDs, CL_DEVICE_NOT_FOUND);
    uint32_t n = A(3), out = A(4), num = A(5);
    cl_device_id ids[64];
    cl_uint got = 0;
    cl_int e = cl.clGetDeviceIDs(m32_host(A(0)), m32_arg64(cpu, 1), out ? (n < 64 ? n : 64) : 0, out ? ids : NULL, &got);
    if (e == CL_SUCCESS && out)
        for (cl_uint i = 0; i < got && i < n && i < 64; i++)
            m32_wr(out + 4 * i, m32_handle(ids[i]));
    if (e == CL_SUCCESS)
        put(num, got);
    return RET(e);
}

/* clCreateSubDevices(device, properties, num_devices, out_devices, num_devices_ret) */
static int sp_clCreateSubDevices(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clCreateSubDevices, CL_INVALID_DEVICE);
    uint32_t n = A(2), out = A(3), num = A(4);
    intptr_t *props = props_in(A(1));
    cl_device_id *ids = out && n ? calloc(n, sizeof *ids) : NULL;
    cl_uint got = 0;
    cl_int e = cl.clCreateSubDevices(m32_host(A(0)), (const cl_device_partition_property *)props, n, ids, &got);
    if (e == CL_SUCCESS && ids)
        for (cl_uint i = 0; i < got && i < n; i++)
            m32_wr(out + 4 * i, m32_handle(ids[i]));
    if (e == CL_SUCCESS)
        put(num, got);
    free(ids);
    free(props);
    return RET(e);
}

/* ---- contexts ---- */

/* the context's error callback: the host's own logger if the guest passed one, else the guest function */
static void *context_notify(uint32_t fn)
{
    if (!fn)
        return NULL;
    if (fn - OCERZ_DYLDAPI_LO < OCERZ_DYLDAPI_HI - OCERZ_DYLDAPI_LO) {
        M32Export *e = m32_export_by_id(fn - (uint32_t)OCERZ_DYLDAPI_LO);
        if (e && e->name) {
            if (!strcmp(e->name, "_clLogMessagesToStderrAPPLE"))
                return (void *)cl.clLogMessagesToStderrAPPLE;
            if (!strcmp(e->name, "_clLogMessagesToStdoutAPPLE"))
                return (void *)cl.clLogMessagesToStdoutAPPLE;
            if (!strcmp(e->name, "_clLogMessagesToSystemLogAPPLE"))
                return (void *)cl.clLogMessagesToSystemLogAPPLE;
        }
    }
    return m32_callback(fn, "v(sppu)", "v(sppL)");
}

/* clCreateContext(properties, num_devices, devices, notify, user_data, errcode_ret) */
static int sp_clCreateContext(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (!FN(clCreateContext)) {
        put(A(5), (uint32_t)CL_DEVICE_NOT_FOUND);
        return RET(0);
    }
    intptr_t *props = props_in(A(0));
    Objs d;
    objs_in(&d, A(2), A(1));
    cl_int e = CL_SUCCESS;
    cl_context c = cl.clCreateContext((const cl_context_properties *)props, A(1), (const cl_device_id *)d.p,
                                      (void (*)(const char *, const void *, size_t, void *))context_notify(A(3)),
                                      m32_host(A(4)), &e);
    objs_done(&d);
    free(props);
    put(A(5), (uint32_t)e);
    return RET(m32_handle(c));
}

/* clCreateContextFromType(properties, cl_device_type (two words), notify, user_data, errcode_ret) */
static int sp_clCreateContextFromType(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (!FN(clCreateContextFromType)) {
        put(A(5), (uint32_t)CL_DEVICE_NOT_FOUND);
        return RET(0);
    }
    intptr_t *props = props_in(A(0));
    cl_int e = CL_SUCCESS;
    cl_context c = cl.clCreateContextFromType((const cl_context_properties *)props, m32_arg64(cpu, 1),
                                              (void (*)(const char *, const void *, size_t, void *))context_notify(A(3)),
                                              m32_host(A(4)), &e);
    free(props);
    put(A(5), (uint32_t)e);
    return RET(m32_handle(c));
}

/* clCreateCommandQueue(context, device, cl_command_queue_properties (two words), errcode_ret) */
static int sp_clCreateCommandQueue(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clCreateCommandQueue, 0);
    cl_int e = CL_SUCCESS;
    cl_command_queue q = cl.clCreateCommandQueue(m32_host(A(0)), m32_host(A(1)), m32_arg64(cpu, 2), &e);
    put(A(4), (uint32_t)e);
    return RET(m32_handle(q));
}

/* ---- memory objects and samplers ---- */

/* clCreateBuffer(context, cl_mem_flags (two words), size, host_ptr, errcode_ret) */
static int sp_clCreateBuffer(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clCreateBuffer, 0);
    cl_int e = CL_SUCCESS;
    cl_mem m = cl.clCreateBuffer(m32_host(A(0)), m32_arg64(cpu, 1), A(3), P(A(4)), &e);
    put(A(5), (uint32_t)e);
    return RET(remember(m, 0));
}

/* clCreateSubBuffer(buffer, flags (two words), create_type, info, errcode_ret); a region is two size_t */
static int sp_clCreateSubBuffer(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clCreateSubBuffer, 0);
    uint32_t gi = A(4);
    cl_buffer_region r = { gi ? m32_rd(gi) : 0, gi ? m32_rd(gi + 4) : 0 };
    cl_int e = CL_SUCCESS;
    cl_mem m = cl.clCreateSubBuffer(m32_host(A(0)), m32_arg64(cpu, 1), A(3), gi ? &r : NULL, &e);
    put(A(5), (uint32_t)e);
    return RET(remember(m, 0));
}

/* clCreateImage(context, flags (two words), format, desc, host_ptr, errcode_ret); the i386 cl_image_desc is ten
 * words: type, width, height, depth, array_size, row_pitch, slice_pitch, mip levels, samples, buffer */
static int sp_clCreateImage(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clCreateImage, 0);
    uint32_t g = A(4);
    cl_image_desc d = { 0 };
    if (g) {
        d.image_type = m32_rd(g);
        d.image_width = m32_rd(g + 4);
        d.image_height = m32_rd(g + 8);
        d.image_depth = m32_rd(g + 12);
        d.image_array_size = m32_rd(g + 16);
        d.image_row_pitch = m32_rd(g + 20);
        d.image_slice_pitch = m32_rd(g + 24);
        d.num_mip_levels = m32_rd(g + 28);
        d.num_samples = m32_rd(g + 32);
        d.buffer = m32_host(m32_rd(g + 36));
    }
    cl_int e = CL_SUCCESS;
    cl_mem m = cl.clCreateImage(m32_host(A(0)), m32_arg64(cpu, 1), P(A(3)), g ? &d : NULL, P(A(5)), &e);
    put(A(6), (uint32_t)e);
    return RET(remember(m, 0));
}

/* clCreateImage2D(context, flags (two words), format, width, height, row_pitch, host_ptr, errcode_ret) */
static int sp_clCreateImage2D(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clCreateImage2D, 0);
    cl_int e = CL_SUCCESS;
    cl_mem m = cl.clCreateImage2D(m32_host(A(0)), m32_arg64(cpu, 1), P(A(3)), A(4), A(5), A(6), P(A(7)), &e);
    put(A(8), (uint32_t)e);
    return RET(remember(m, 0));
}

/* clCreateImage3D(context, flags (two words), format, width, height, depth, row_pitch, slice_pitch, host_ptr, err) */
static int sp_clCreateImage3D(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clCreateImage3D, 0);
    cl_int e = CL_SUCCESS;
    cl_mem m = cl.clCreateImage3D(m32_host(A(0)), m32_arg64(cpu, 1), P(A(3)), A(4), A(5), A(6), A(7), A(8),
                                  P(A(9)), &e);
    put(A(10), (uint32_t)e);
    return RET(remember(m, 0));
}

/* clGetSupportedImageFormats(context, flags (two words), image_type, num_entries, formats, num_formats); a
 * cl_image_format is two cl_uint in both widths */
static int sp_clGetSupportedImageFormats(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clGetSupportedImageFormats, CL_INVALID_CONTEXT);
    cl_uint got = 0;
    cl_int e = cl.clGetSupportedImageFormats(m32_host(A(0)), m32_arg64(cpu, 1), A(3), A(4), P(A(5)),
                                             A(6) ? &got : NULL);
    if (e == CL_SUCCESS)
        put(A(6), got);
    return RET(e);
}

/* clSetMemObjectDestructorCallback(memobj, notify(memobj, user_data), user_data) */
static int sp_clSetMemObjectDestructorCallback(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clSetMemObjectDestructorCallback, CL_INVALID_MEM_OBJECT);
    void *fn = A(1) ? m32_callback(A(1), "v(pp)", "v(pp)") : NULL;
    if (A(1) && !fn)
        return RET(CL_OUT_OF_HOST_MEMORY);
    return RET(cl.clSetMemObjectDestructorCallback(m32_host(A(0)), (void (*)(cl_mem, void *))fn, m32_host(A(2))));
}

/* clCreateSampler(context, normalized_coords, addressing_mode, filter_mode, errcode_ret) */
static int sp_clCreateSampler(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clCreateSampler, 0);
    cl_int e = CL_SUCCESS;
    cl_sampler s = cl.clCreateSampler(m32_host(A(0)), A(1), A(2), A(3), &e);
    put(A(4), (uint32_t)e);
    return RET(remember(s, 1));
}

/* ---- programs ---- */

/* clCreateProgramWithSource(context, count, strings, lengths, errcode_ret) */
static int sp_clCreateProgramWithSource(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clCreateProgramWithSource, 0);
    uint32_t n = A(1), gs = A(2), gl = A(3);
    const char **s = calloc(n ? n : 1, sizeof *s);
    size_t *len = gl ? calloc(n ? n : 1, sizeof *len) : NULL;
    for (uint32_t i = 0; gs && i < n; i++) {
        s[i] = P(m32_rd(gs + 4 * i));
        if (len)
            len[i] = m32_rd(gl + 4 * i);
    }
    cl_int e = CL_SUCCESS;
    cl_program p = cl.clCreateProgramWithSource(m32_host(A(0)), n, gs ? s : NULL, len, &e);
    free(s);
    free(len);
    put(A(4), (uint32_t)e);
    return RET(m32_handle(p));
}

/* clCreateProgramWithBinary(context, num_devices, devices, lengths, binaries, binary_status, errcode_ret) */
static int sp_clCreateProgramWithBinary(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clCreateProgramWithBinary, 0);
    uint32_t n = A(1), gl = A(3), gb = A(4);
    Objs d;
    objs_in(&d, A(2), n);
    size_t *len = calloc(n ? n : 1, sizeof *len);
    const unsigned char **bin = calloc(n ? n : 1, sizeof *bin);
    for (uint32_t i = 0; i < n; i++) {
        len[i] = gl ? m32_rd(gl + 4 * i) : 0;
        bin[i] = gb ? P(m32_rd(gb + 4 * i)) : NULL;
    }
    cl_int e = CL_SUCCESS;
    cl_program p = cl.clCreateProgramWithBinary(m32_host(A(0)), n, (const cl_device_id *)d.p, gl ? len : NULL,
                                                gb ? bin : NULL, P(A(5)), &e);
    objs_done(&d);
    free(len);
    free(bin);
    put(A(6), (uint32_t)e);
    return RET(m32_handle(p));
}

/* clCreateProgramWithBuiltInKernels(context, num_devices, devices, kernel_names, errcode_ret) */
static int sp_clCreateProgramWithBuiltInKernels(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clCreateProgramWithBuiltInKernels, 0);
    Objs d;
    objs_in(&d, A(2), A(1));
    cl_int e = CL_SUCCESS;
    cl_program p = cl.clCreateProgramWithBuiltInKernels(m32_host(A(0)), A(1), (const cl_device_id *)d.p, P(A(3)), &e);
    objs_done(&d);
    put(A(4), (uint32_t)e);
    return RET(m32_handle(p));
}

/* a build's callback(program, user_data), on the guest's thread once the build is done */
static void build_done(struct OcerzVM *vm, uint32_t fn, uint32_t program, uint32_t user_data)
{
    if (fn)
        m32_call(vm, fn, (const uint32_t[]){ program, user_data }, 2, NULL, NULL);
}

/* clBuildProgram(program, num_devices, devices, options, notify, user_data) */
static int sp_clBuildProgram(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clBuildProgram, CL_INVALID_PROGRAM);
    Objs d;
    objs_in(&d, A(2), A(1));
    uint32_t prog = A(0), fn = A(4), data = A(5);
    cl_int e = cl.clBuildProgram(m32_host(prog), A(1), (const cl_device_id *)d.p, P(A(3)), NULL, NULL);
    objs_done(&d);
    if (e == CL_SUCCESS || e == CL_BUILD_PROGRAM_FAILURE)
        build_done(vm, fn, prog, data);
    return RET(e);
}

/* clCompileProgram(program, num_devices, devices, options, num_headers, headers, header_names, notify, user_data) */
static int sp_clCompileProgram(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clCompileProgram, CL_INVALID_PROGRAM);
    uint32_t nh = A(4), gn = A(6);
    Objs d, h;
    objs_in(&d, A(2), A(1));
    objs_in(&h, A(5), nh);
    const char **names = gn && nh ? calloc(nh, sizeof *names) : NULL;
    for (uint32_t i = 0; names && i < nh; i++)
        names[i] = P(m32_rd(gn + 4 * i));
    uint32_t prog = A(0);
    cl_int e = cl.clCompileProgram(m32_host(prog), A(1), (const cl_device_id *)d.p, P(A(3)), nh,
                                   (const cl_program *)h.p, names, NULL, NULL);
    objs_done(&d);
    objs_done(&h);
    free(names);
    if (e == CL_SUCCESS || e == CL_COMPILE_PROGRAM_FAILURE)
        build_done(vm, A(7), prog, A(8));
    return RET(e);
}

/* clLinkProgram(context, num_devices, devices, options, num_programs, programs, notify, user_data, errcode_ret) */
static int sp_clLinkProgram(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clLinkProgram, 0);
    Objs d, p;
    objs_in(&d, A(2), A(1));
    objs_in(&p, A(5), A(4));
    cl_int e = CL_SUCCESS;
    cl_program out = cl.clLinkProgram(m32_host(A(0)), A(1), (const cl_device_id *)d.p, P(A(3)), A(4),
                                      (const cl_program *)p.p, NULL, NULL, &e);
    objs_done(&d);
    objs_done(&p);
    put(A(8), (uint32_t)e);
    uint32_t g = m32_handle(out);
    if (out)
        build_done(vm, A(6), g, A(7));
    return RET(g);
}

/* ---- kernels ---- */

/* clCreateKernel(program, kernel_name, errcode_ret) */
static int sp_clCreateKernel(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clCreateKernel, 0);
    cl_int e = CL_SUCCESS;
    cl_kernel k = cl.clCreateKernel(m32_host(A(0)), P(A(1)), &e);
    put(A(2), (uint32_t)e);
    return RET(m32_handle(k));
}

/* clCreateKernelsInProgram(program, num_kernels, kernels, num_kernels_ret) */
static int sp_clCreateKernelsInProgram(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clCreateKernelsInProgram, CL_INVALID_PROGRAM);
    uint32_t n = A(1), out = A(2);
    cl_kernel *k = out && n ? calloc(n, sizeof *k) : NULL;
    cl_uint got = 0;
    cl_int e = cl.clCreateKernelsInProgram(m32_host(A(0)), k ? n : 0, k, &got);
    if (e == CL_SUCCESS && k)
        for (cl_uint i = 0; i < got && i < n; i++)
            m32_wr(out + 4 * i, m32_handle(k[i]));
    if (e == CL_SUCCESS)
        put(A(3), got);
    free(k);
    return RET(e);
}

/* clSetKernelArg(kernel, arg_index, arg_size, arg_value): a 4-byte memory object or sampler becomes the host's 8 */
static int sp_clSetKernelArg(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clSetKernelArg, CL_INVALID_KERNEL);
    void *k = m32_host(A(0));
    cl_uint idx = A(1);
    uint32_t size = A(2), gv = A(3);
    if (size == 4) {
        int object = 0;
        cl_kernel_arg_address_qualifier aq = 0;
        if (FN(clGetKernelArgInfo) &&
            cl.clGetKernelArgInfo(k, idx, CL_KERNEL_ARG_ADDRESS_QUALIFIER, sizeof aq, &aq, NULL) == CL_SUCCESS)
            object = aq == CL_KERNEL_ARG_ADDRESS_GLOBAL || aq == CL_KERNEL_ARG_ADDRESS_CONSTANT;
        uint32_t v = gv ? m32_rd(gv) : 0;
        if (!object && gv)
            object = remembered(v) != 0;
        if (object) {
            void *host = gv && v ? m32_host(v) : NULL;
            return RET(cl.clSetKernelArg(k, idx, sizeof host, gv ? &host : NULL));
        }
    }
    return RET(cl.clSetKernelArg(k, idx, size, P(gv)));
}

/* ---- events ---- */

/* clWaitForEvents(num_events, event_list) */
static int sp_clWaitForEvents(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clWaitForEvents, CL_INVALID_EVENT);
    Objs w;
    objs_in(&w, A(1), A(0));
    cl_int e = cl.clWaitForEvents(A(0), (const cl_event *)w.p);
    objs_done(&w);
    return RET(e);
}

/* clCreateUserEvent(context, errcode_ret) */
static int sp_clCreateUserEvent(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clCreateUserEvent, 0);
    cl_int e = CL_SUCCESS;
    cl_event ev = cl.clCreateUserEvent(m32_host(A(0)), &e);
    put(A(1), (uint32_t)e);
    return RET(m32_handle(ev));
}

static int sp_clSetUserEventStatus(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clSetUserEventStatus, CL_INVALID_EVENT);
    return RET(cl.clSetUserEventStatus(m32_host(A(0)), (cl_int)A(1)));
}

/* clSetEventCallback(event, command_exec_callback_type, notify(event, status, user_data), user_data) */
static int sp_clSetEventCallback(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clSetEventCallback, CL_INVALID_EVENT);
    void *fn = A(2) ? m32_callback(A(2), "v(pip)", "v(pip)") : NULL;
    if (!fn)
        return RET(A(2) ? CL_OUT_OF_HOST_MEMORY : CL_INVALID_VALUE);
    return RET(cl.clSetEventCallback(m32_host(A(0)), (cl_int)A(1), (void (*)(cl_event, cl_int, void *))fn, m32_host(A(3))));
}

/* ---- commands ---- */

/* the wait list and the event every enqueue ends with, at argument first: num_events, event_wait_list, event */
typedef struct Wait { Objs w; cl_uint n; cl_event ev; uint32_t gev; } Wait;
static const cl_event *wait_in(Wait *w, OcerzCPU *cpu, int first)
{
    w->n = A(first);
    w->gev = A(first + 2);
    w->ev = NULL;
    return (const cl_event *)objs_in(&w->w, A(first + 1), w->n);
}
static void wait_end(Wait *w, cl_int e)
{
    objs_done(&w->w);
    event_out(w->gev, e, w->ev);
}
/* m32_ret returns from the call, so nothing reads an argument after it */
static int wait_done(Wait *w, OcerzCPU *cpu, cl_int e)
{
    wait_end(w, e);
    return RET(e);
}
#define WAIT(w) (w).n, wait_list, (w).gev ? &(w).ev : NULL

/* clEnqueueReadBuffer / clEnqueueWriteBuffer(queue, buffer, blocking, offset, size, ptr, wait...) */
static int sp_clEnqueueReadBuffer(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueReadBuffer, CL_INVALID_COMMAND_QUEUE);
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 6);
    return wait_done(&w, cpu, cl.clEnqueueReadBuffer(m32_host(A(0)), m32_host(A(1)), A(2), A(3), A(4), P(A(5)), WAIT(w)));
}
static int sp_clEnqueueWriteBuffer(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueWriteBuffer, CL_INVALID_COMMAND_QUEUE);
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 6);
    return wait_done(&w, cpu, cl.clEnqueueWriteBuffer(m32_host(A(0)), m32_host(A(1)), A(2), A(3), A(4), P(A(5)), WAIT(w)));
}

/* clEnqueue{Read,Write}BufferRect(queue, buffer, blocking, buffer_origin, host_origin, region, buffer_row_pitch,
 * buffer_slice_pitch, host_row_pitch, host_slice_pitch, ptr, wait...) */
static int sp_clEnqueueReadBufferRect(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueReadBufferRect, CL_INVALID_COMMAND_QUEUE);
    size_t bo[3], ho[3], r[3];
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 11);
    return wait_done(&w, cpu, cl.clEnqueueReadBufferRect(m32_host(A(0)), m32_host(A(1)), A(2), sizes_in(bo, A(3), 3),
                                                         sizes_in(ho, A(4), 3), sizes_in(r, A(5), 3), A(6), A(7), A(8),
                                                         A(9), P(A(10)), WAIT(w)));
}
static int sp_clEnqueueWriteBufferRect(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueWriteBufferRect, CL_INVALID_COMMAND_QUEUE);
    size_t bo[3], ho[3], r[3];
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 11);
    return wait_done(&w, cpu, cl.clEnqueueWriteBufferRect(m32_host(A(0)), m32_host(A(1)), A(2), sizes_in(bo, A(3), 3),
                                                          sizes_in(ho, A(4), 3), sizes_in(r, A(5), 3), A(6), A(7), A(8),
                                                          A(9), P(A(10)), WAIT(w)));
}

/* clEnqueueFillBuffer(queue, buffer, pattern, pattern_size, offset, size, wait...) */
static int sp_clEnqueueFillBuffer(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueFillBuffer, CL_INVALID_COMMAND_QUEUE);
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 6);
    return wait_done(&w, cpu, cl.clEnqueueFillBuffer(m32_host(A(0)), m32_host(A(1)), P(A(2)), A(3), A(4), A(5), WAIT(w)));
}

/* clEnqueueCopyBuffer(queue, src, dst, src_offset, dst_offset, size, wait...) */
static int sp_clEnqueueCopyBuffer(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueCopyBuffer, CL_INVALID_COMMAND_QUEUE);
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 6);
    return wait_done(&w, cpu, cl.clEnqueueCopyBuffer(m32_host(A(0)), m32_host(A(1)), m32_host(A(2)), A(3), A(4), A(5),
                                                     WAIT(w)));
}

/* clEnqueueCopyBufferRect(queue, src, dst, src_origin, dst_origin, region, src_row_pitch, src_slice_pitch,
 * dst_row_pitch, dst_slice_pitch, wait...) */
static int sp_clEnqueueCopyBufferRect(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueCopyBufferRect, CL_INVALID_COMMAND_QUEUE);
    size_t so[3], dO[3], r[3];
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 10);
    return wait_done(&w, cpu, cl.clEnqueueCopyBufferRect(m32_host(A(0)), m32_host(A(1)), m32_host(A(2)),
                                                         sizes_in(so, A(3), 3), sizes_in(dO, A(4), 3),
                                                         sizes_in(r, A(5), 3), A(6), A(7), A(8), A(9), WAIT(w)));
}

/* clEnqueue{Read,Write}Image(queue, image, blocking, origin, region, row_pitch, slice_pitch, ptr, wait...) */
static int sp_clEnqueueReadImage(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueReadImage, CL_INVALID_COMMAND_QUEUE);
    size_t o[3], r[3];
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 8);
    return wait_done(&w, cpu, cl.clEnqueueReadImage(m32_host(A(0)), m32_host(A(1)), A(2), sizes_in(o, A(3), 3),
                                                    sizes_in(r, A(4), 3), A(5), A(6), P(A(7)), WAIT(w)));
}
static int sp_clEnqueueWriteImage(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueWriteImage, CL_INVALID_COMMAND_QUEUE);
    size_t o[3], r[3];
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 8);
    return wait_done(&w, cpu, cl.clEnqueueWriteImage(m32_host(A(0)), m32_host(A(1)), A(2), sizes_in(o, A(3), 3),
                                                     sizes_in(r, A(4), 3), A(5), A(6), P(A(7)), WAIT(w)));
}

/* clEnqueueFillImage(queue, image, fill_color, origin, region, wait...) */
static int sp_clEnqueueFillImage(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueFillImage, CL_INVALID_COMMAND_QUEUE);
    size_t o[3], r[3];
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 5);
    return wait_done(&w, cpu, cl.clEnqueueFillImage(m32_host(A(0)), m32_host(A(1)), P(A(2)), sizes_in(o, A(3), 3),
                                                    sizes_in(r, A(4), 3), WAIT(w)));
}

/* clEnqueueCopyImage(queue, src, dst, src_origin, dst_origin, region, wait...) */
static int sp_clEnqueueCopyImage(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueCopyImage, CL_INVALID_COMMAND_QUEUE);
    size_t so[3], dO[3], r[3];
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 6);
    return wait_done(&w, cpu, cl.clEnqueueCopyImage(m32_host(A(0)), m32_host(A(1)), m32_host(A(2)), sizes_in(so, A(3), 3),
                                                    sizes_in(dO, A(4), 3), sizes_in(r, A(5), 3), WAIT(w)));
}

/* clEnqueueCopyImageToBuffer(queue, src_image, dst_buffer, src_origin, region, dst_offset, wait...) */
static int sp_clEnqueueCopyImageToBuffer(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueCopyImageToBuffer, CL_INVALID_COMMAND_QUEUE);
    size_t o[3], r[3];
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 6);
    return wait_done(&w, cpu, cl.clEnqueueCopyImageToBuffer(m32_host(A(0)), m32_host(A(1)), m32_host(A(2)),
                                                            sizes_in(o, A(3), 3), sizes_in(r, A(4), 3), A(5), WAIT(w)));
}

/* clEnqueueCopyBufferToImage(queue, src_buffer, dst_image, src_offset, dst_origin, region, wait...) */
static int sp_clEnqueueCopyBufferToImage(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueCopyBufferToImage, CL_INVALID_COMMAND_QUEUE);
    size_t o[3], r[3];
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 6);
    return wait_done(&w, cpu, cl.clEnqueueCopyBufferToImage(m32_host(A(0)), m32_host(A(1)), m32_host(A(2)), A(3),
                                                            sizes_in(o, A(4), 3), sizes_in(r, A(5), 3), WAIT(w)));
}

/* clEnqueueNDRangeKernel(queue, kernel, work_dim, global_offset, global_size, local_size, wait...) */
static int sp_clEnqueueNDRangeKernel(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueNDRangeKernel, CL_INVALID_COMMAND_QUEUE);
    uint32_t dim = A(2);
    if (dim < 1 || dim > 3)
        return RET(CL_INVALID_WORK_DIMENSION);
    size_t off[3], gs[3], ls[3];
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 6);
    return wait_done(&w, cpu, cl.clEnqueueNDRangeKernel(m32_host(A(0)), m32_host(A(1)), dim, sizes_in(off, A(3), dim),
                                                        sizes_in(gs, A(4), dim), sizes_in(ls, A(5), dim), WAIT(w)));
}

/* clEnqueueTask(queue, kernel, wait...) */
static int sp_clEnqueueTask(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueTask, CL_INVALID_COMMAND_QUEUE);
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 2);
    return wait_done(&w, cpu, cl.clEnqueueTask(m32_host(A(0)), m32_host(A(1)), WAIT(w)));
}

static int sp_clEnqueueNativeKernel(struct OcerzVM *vm, OcerzCPU *cpu)
{
    m32_log_once("a native kernel would run guest code on an OpenCL device thread, refused:", "_clEnqueueNativeKernel");
    return RET(CL_INVALID_OPERATION);
}

/* clEnqueueMarker(queue, event) and clEnqueueWaitForEvents(queue, num_events, event_list) */
static int sp_clEnqueueMarker(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueMarker, CL_INVALID_COMMAND_QUEUE);
    cl_event ev = NULL;
    cl_int e = cl.clEnqueueMarker(m32_host(A(0)), A(1) ? &ev : NULL);
    event_out(A(1), e, ev);
    return RET(e);
}
static int sp_clEnqueueWaitForEvents(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueWaitForEvents, CL_INVALID_COMMAND_QUEUE);
    Objs o;
    objs_in(&o, A(2), A(1));
    cl_int e = cl.clEnqueueWaitForEvents(m32_host(A(0)), A(1), (const cl_event *)o.p);
    objs_done(&o);
    return RET(e);
}

/* clEnqueue{Marker,Barrier}WithWaitList(queue, wait...) */
static int sp_clEnqueueMarkerWithWaitList(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueMarkerWithWaitList, CL_INVALID_COMMAND_QUEUE);
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 1);
    return wait_done(&w, cpu, cl.clEnqueueMarkerWithWaitList(m32_host(A(0)), WAIT(w)));
}
static int sp_clEnqueueBarrierWithWaitList(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueBarrierWithWaitList, CL_INVALID_COMMAND_QUEUE);
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 1);
    return wait_done(&w, cpu, cl.clEnqueueBarrierWithWaitList(m32_host(A(0)), WAIT(w)));
}

/* clEnqueueMigrateMemObjects(queue, num_objects, objects, cl_mem_migration_flags (two words), wait...) */
static int sp_clEnqueueMigrateMemObjects(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueMigrateMemObjects, CL_INVALID_COMMAND_QUEUE);
    Objs o;
    objs_in(&o, A(2), A(1));
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 5);
    cl_int e = cl.clEnqueueMigrateMemObjects(m32_host(A(0)), A(1), (const cl_mem *)o.p, m32_arg64(cpu, 3), WAIT(w));
    objs_done(&o);
    return wait_done(&w, cpu, e);
}

/* clEnqueue{Acquire,Release}GLObjects(queue, num_objects, objects, wait...) */
static int sp_clEnqueueAcquireGLObjects(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueAcquireGLObjects, CL_INVALID_COMMAND_QUEUE);
    Objs o;
    objs_in(&o, A(2), A(1));
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 3);
    cl_int e = cl.clEnqueueAcquireGLObjects(m32_host(A(0)), A(1), (const cl_mem *)o.p, WAIT(w));
    objs_done(&o);
    return wait_done(&w, cpu, e);
}
static int sp_clEnqueueReleaseGLObjects(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueReleaseGLObjects, CL_INVALID_COMMAND_QUEUE);
    Objs o;
    objs_in(&o, A(2), A(1));
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 3);
    cl_int e = cl.clEnqueueReleaseGLObjects(m32_host(A(0)), A(1), (const cl_mem *)o.p, WAIT(w));
    objs_done(&o);
    return wait_done(&w, cpu, e);
}

/* ---- mappings ---- */

typedef struct Shadow { uint32_t guest; void *host; size_t size; cl_map_flags flags; } Shadow;
static Shadow *g_shadows;
static size_t g_nshadows, g_capshadows;

/* a host mapping the guest can reach: inside the window it is its own address, else a shadow copy */
static uint32_t map_out(void *host, size_t size, cl_map_flags flags)
{
    if (!host)
        return 0;
    if (m32_in_window(host) && m32_in_window((uint8_t *)host + size - (size ? 1 : 0)))
        return m32_g(host);
    uint32_t g = m32_malloc(size ? (uint32_t)size : 1);
    if (!g)
        return 0;
    if (!(flags & CL_MAP_WRITE_INVALIDATE_REGION))
        memcpy(m32_h(g), host, size);
    pthread_mutex_lock(&g_lock);
    if (g_nshadows == g_capshadows) {
        g_capshadows = g_capshadows ? g_capshadows * 2 : 16;
        g_shadows = realloc(g_shadows, g_capshadows * sizeof *g_shadows);
    }
    g_shadows[g_nshadows++] = (Shadow){ g, host, size, flags };
    pthread_mutex_unlock(&g_lock);
    return g;
}

/* clEnqueueMapBuffer(queue, buffer, blocking, cl_map_flags (two words), offset, size, wait..., errcode_ret) */
static int sp_clEnqueueMapBuffer(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (!FN(clEnqueueMapBuffer)) {
        put(A(10), (uint32_t)CL_INVALID_COMMAND_QUEUE);
        return RET(0);
    }
    cl_map_flags flags = m32_arg64(cpu, 3);
    size_t size = A(6);
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 7);
    cl_int e = CL_SUCCESS;
    void *host = cl.clEnqueueMapBuffer(m32_host(A(0)), m32_host(A(1)), CL_TRUE, flags, A(5), size, WAIT(w), &e);
    uint32_t g = e == CL_SUCCESS ? map_out(host, size, flags) : 0;
    if (e == CL_SUCCESS && host && !g)
        e = CL_OUT_OF_HOST_MEMORY;
    put(A(10), (uint32_t)e);
    wait_end(&w, e);
    return RET(g);
}

/* clEnqueueMapImage(queue, image, blocking, flags (two words), origin, region, row_pitch, slice_pitch, wait...,
 * errcode_ret): the mapping is the region's rows and slices at the pitches the host answers */
static int sp_clEnqueueMapImage(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (!FN(clEnqueueMapImage)) {
        put(A(12), (uint32_t)CL_INVALID_COMMAND_QUEUE);
        return RET(0);
    }
    cl_map_flags flags = m32_arg64(cpu, 3);
    size_t o[3], r[3], row = 0, slice = 0, elem = 0;
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 9);
    cl_int e = CL_SUCCESS;
    void *img = m32_host(A(1));
    void *host = cl.clEnqueueMapImage(m32_host(A(0)), img, CL_TRUE, flags, sizes_in(o, A(5), 3), sizes_in(r, A(6), 3),
                                      &row, A(8) ? &slice : NULL, WAIT(w), &e);
    uint32_t g = 0;
    if (e == CL_SUCCESS) {
        if (FN(clGetImageInfo))
            cl.clGetImageInfo(img, CL_IMAGE_ELEMENT_SIZE, sizeof elem, &elem, NULL);
        size_t depth = A(6) ? r[2] : 1, height = A(6) ? r[1] : 1, width = A(6) ? r[0] : 0;
        size_t size = (depth > 1 ? (depth - 1) * slice : 0) + (height > 1 ? (height - 1) * row : 0) + width * elem;
        g = map_out(host, size, flags);
        if (host && !g)
            e = CL_OUT_OF_HOST_MEMORY;
        put(A(7), narrow(row));
        put(A(8), narrow(slice));
    }
    put(A(12), (uint32_t)e);
    wait_end(&w, e);
    return RET(g);
}

/* clEnqueueUnmapMemObject(queue, memobj, mapped_ptr, wait...): a shadow is copied back if the map was for writing */
static int sp_clEnqueueUnmapMemObject(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clEnqueueUnmapMemObject, CL_INVALID_COMMAND_QUEUE);
    uint32_t gp = A(2);
    Shadow s = { 0 };
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_nshadows; i++)
        if (g_shadows[i].guest == gp) {
            s = g_shadows[i];
            g_shadows[i] = g_shadows[--g_nshadows];
            break;
        }
    pthread_mutex_unlock(&g_lock);
    void *host = s.guest ? s.host : P(gp);
    if (s.guest && (s.flags & (CL_MAP_WRITE | CL_MAP_WRITE_INVALIDATE_REGION)))
        memcpy(s.host, m32_h(s.guest), s.size);
    Wait w;
    const cl_event *wait_list = wait_in(&w, cpu, 3);
    cl_int e = cl.clEnqueueUnmapMemObject(m32_host(A(0)), m32_host(A(1)), host, WAIT(w));
    if (s.guest)
        m32_free(s.guest);
    return wait_done(&w, cpu, e);
}

/* ---- OpenGL sharing ---- */

/* clCreateFromGLBuffer(context, flags (two words), bufobj, errcode_ret) */
static int sp_clCreateFromGLBuffer(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clCreateFromGLBuffer, 0);
    cl_int e = CL_SUCCESS;
    cl_mem m = cl.clCreateFromGLBuffer(m32_host(A(0)), m32_arg64(cpu, 1), A(3), &e);
    put(A(4), (uint32_t)e);
    return RET(remember(m, 0));
}

/* clCreateFromGLTexture[2D|3D](context, flags (two words), target, miplevel, texture, errcode_ret) */
#define FROM_TEXTURE(name) \
    static int sp_##name(struct OcerzVM *vm, OcerzCPU *cpu) \
    { \
        NEED(name, 0); \
        cl_int e = CL_SUCCESS; \
        cl_mem m = cl.name(m32_host(A(0)), m32_arg64(cpu, 1), A(3), (cl_GLint)A(4), A(5), &e); \
        put(A(6), (uint32_t)e); \
        return RET(remember(m, 0)); \
    }
FROM_TEXTURE(clCreateFromGLTexture)
FROM_TEXTURE(clCreateFromGLTexture2D)
FROM_TEXTURE(clCreateFromGLTexture3D)

/* clCreateFromGLRenderbuffer(context, flags (two words), renderbuffer, errcode_ret) */
static int sp_clCreateFromGLRenderbuffer(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clCreateFromGLRenderbuffer, 0);
    cl_int e = CL_SUCCESS;
    cl_mem m = cl.clCreateFromGLRenderbuffer(m32_host(A(0)), m32_arg64(cpu, 1), A(3), &e);
    put(A(4), (uint32_t)e);
    return RET(remember(m, 0));
}

/* clGetGLObjectInfo(memobj, gl_object_type, gl_object_name): two 4-byte outputs */
static int sp_clGetGLObjectInfo(struct OcerzVM *vm, OcerzCPU *cpu)
{
    NEED(clGetGLObjectInfo, CL_INVALID_MEM_OBJECT);
    return RET(cl.clGetGLObjectInfo(m32_host(A(0)), P(A(1)), P(A(2))));
}

/* ---- the rest ---- */

/* clGetExtensionFunctionAddress[ForPlatform]: a host entry point is nothing the guest can call */
static int sp_clGetExtensionFunctionAddress(struct OcerzVM *vm, OcerzCPU *cpu) { return RET(0); }

/* clLogMessagesTo*APPLE(errstr, private_info, cb, user_data), called directly */
#define LOGGER(name) \
    static int sp_##name(struct OcerzVM *vm, OcerzCPU *cpu) \
    { \
        if (FN(name)) \
            cl.name(P(A(0)), NULL, 0, NULL); \
        return RET(0); \
    }
LOGGER(clLogMessagesToSystemLogAPPLE)
LOGGER(clLogMessagesToStdoutAPPLE)
LOGGER(clLogMessagesToStderrAPPLE)

#define E(f) { "_" #f, sp_##f },
const M32SpecialEntry m32_cl_specials[] = {
    E(clGetPlatformIDs) E(clGetPlatformInfo) E(clGetDeviceIDs) E(clGetDeviceInfo) E(clCreateSubDevices)
    E(clRetainDevice) E(clReleaseDevice) E(clCreateContext) E(clCreateContextFromType) E(clRetainContext)
    E(clReleaseContext) E(clGetContextInfo) E(clCreateCommandQueue) E(clRetainCommandQueue)
    E(clReleaseCommandQueue) E(clGetCommandQueueInfo) E(clCreateBuffer) E(clCreateSubBuffer) E(clCreateImage)
    E(clCreateImage2D) E(clCreateImage3D) E(clRetainMemObject) E(clReleaseMemObject)
    E(clGetSupportedImageFormats) E(clGetMemObjectInfo) E(clGetImageInfo) E(clSetMemObjectDestructorCallback)
    E(clCreateSampler) E(clRetainSampler) E(clReleaseSampler) E(clGetSamplerInfo) E(clCreateProgramWithSource)
    E(clCreateProgramWithBinary) E(clCreateProgramWithBuiltInKernels) E(clRetainProgram) E(clReleaseProgram)
    E(clBuildProgram) E(clCompileProgram) E(clLinkProgram) E(clUnloadCompiler) E(clUnloadPlatformCompiler)
    E(clGetProgramInfo) E(clGetProgramBuildInfo) E(clCreateKernel) E(clCreateKernelsInProgram) E(clRetainKernel)
    E(clReleaseKernel) E(clSetKernelArg) E(clGetKernelInfo) E(clGetKernelArgInfo) E(clGetKernelWorkGroupInfo)
    E(clWaitForEvents) E(clGetEventInfo) E(clCreateUserEvent) E(clRetainEvent) E(clReleaseEvent)
    E(clSetUserEventStatus) E(clSetEventCallback) E(clGetEventProfilingInfo) E(clFlush) E(clFinish)
    E(clEnqueueReadBuffer) E(clEnqueueReadBufferRect) E(clEnqueueWriteBuffer) E(clEnqueueWriteBufferRect)
    E(clEnqueueFillBuffer) E(clEnqueueCopyBuffer) E(clEnqueueCopyBufferRect) E(clEnqueueReadImage)
    E(clEnqueueWriteImage) E(clEnqueueFillImage) E(clEnqueueCopyImage) E(clEnqueueCopyImageToBuffer)
    E(clEnqueueCopyBufferToImage) E(clEnqueueMapBuffer) E(clEnqueueMapImage) E(clEnqueueUnmapMemObject)
    E(clEnqueueMigrateMemObjects) E(clEnqueueNDRangeKernel) E(clEnqueueTask) E(clEnqueueNativeKernel)
    E(clEnqueueMarker) E(clEnqueueWaitForEvents) E(clEnqueueBarrier) E(clEnqueueMarkerWithWaitList)
    E(clEnqueueBarrierWithWaitList)
    E(clCreateFromGLBuffer) E(clCreateFromGLTexture) E(clCreateFromGLTexture2D) E(clCreateFromGLTexture3D)
    E(clCreateFromGLRenderbuffer) E(clGetGLObjectInfo) E(clGetGLTextureInfo) E(clEnqueueAcquireGLObjects)
    E(clEnqueueReleaseGLObjects) E(clLogMessagesToSystemLogAPPLE) E(clLogMessagesToStdoutAPPLE)
    E(clLogMessagesToStderrAPPLE)
    { "_clGetExtensionFunctionAddress", sp_clGetExtensionFunctionAddress },
    { "_clGetExtensionFunctionAddressForPlatform", sp_clGetExtensionFunctionAddress },
    { NULL, NULL },
};

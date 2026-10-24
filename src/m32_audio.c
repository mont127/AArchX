/*
 * CoreAudio render callbacks for i386 guests (include/ocerz/m32.h).
 *
 * AudioUnitSetProperty's render and input callbacks (kAudioUnitProperty_SetRenderCallback,
 * kAudioOutputUnitProperty_SetInputCallback) are an AURenderCallbackStruct, two 4-byte words in i386 code: the host
 * rejected it (kAudioUnitErr_InvalidPropertyValue), so FMOD Ex never started its CoreAudio output and Batman ran
 * without sound or the time its sounds take (scripted scenes wait for lines to finish).  The host now gets a thunk.
 * On CoreAudio's I/O thread it hands the guest proc guest copies of the flags, the time stamp (host time in the
 * nanoseconds m32 presents) and the buffer list, with the samples in guest memory, then copies them back.
 *
 * ponytail: one scratch block per callback, grown on the I/O thread when a larger buffer list arrives; contexts are
 * never freed (an app sets a handful).
 */
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ocerz/bridge.h"
#include "ocerz/interp.h"
#include "ocerz/m32.h"

#define AT_LIB "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox"
#define RET(v) do { m32_ret(cpu, (uint32_t)(v), 0, 0); return OCERZ_STEP_OK; } while (0)

typedef struct { uint32_t channels, size; void *data; } HostBuffer;   /* AudioBuffer, arm64 */
typedef struct { uint32_t n; HostBuffer b[]; } HostBufferList;         /* AudioBufferList: buffers at offset 8 */

typedef struct Render {
    struct OcerzVM *vm;
    uint32_t proc, refcon;
    int v1;                  /* Mac OS X 10.1's callback: flags by value, one AudioBuffer */
    uint32_t scratch, cap;   /* guest: flags (4), time stamp (64), buffer list (4 + 12 n), the samples */
} Render;

static int32_t render_thunk(void *ctx, uint32_t *flags, const uint8_t *ts, uint32_t bus, uint32_t frames,
                            HostBufferList *io)
{
    Render *r = ctx;
    uint32_t n = io ? io->n : 0, bytes = 0;
    for (uint32_t i = 0; i < n; i++)
        bytes += io->b[i].size;
    uint32_t need = 4 + 64 + 4 + 12 * n + bytes;
    if (need > r->cap) {
        m32_free(r->scratch);
        r->scratch = m32_malloc(need);
        r->cap = r->scratch ? need : 0;
        if (!r->scratch)
            return -108;   /* memFullErr */
    }
    uint32_t gf = r->scratch, gts = gf + 4, gl = gts + 64, d = gl + 4 + 12 * n;
    m32_wr(gf, flags ? *flags : 0);
    memset(m32_h(gts), 0, 64);
    if (ts) {
        static mach_timebase_info_data_t tb;
        if (!tb.denom)
            mach_timebase_info(&tb);
        uint64_t host;
        memcpy(m32_h(gts), ts, 64);
        memcpy(&host, ts + 8, 8);
        host = host * tb.numer / tb.denom;   /* m32's clocks are nanoseconds, timebase 1/1 */
        memcpy((uint8_t *)m32_h(gts) + 8, &host, 8);
    }
    m32_wr(gl, n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = gl + 4 + 12 * i;
        m32_wr(e, io->b[i].channels);
        m32_wr(e + 4, io->b[i].size);
        m32_wr(e + 8, d);
        if (io->b[i].data)
            memcpy(m32_h(d), io->b[i].data, io->b[i].size);
        d += io->b[i].size;
    }
    uint32_t args[6] = { r->refcon, flags ? gf : 0, ts ? gts : 0, bus, frames, io ? gl : 0 };
    if (r->v1) {   /* (refcon, flags, time stamp, bus, AudioBuffer *): the list's first buffer, interleaved */
        args[1] = flags ? *flags : 0;
        args[4] = n ? gl + 4 : 0;
    }
    static int log = -1;   /* OCERZ_M32LOG=render: each render callback the host makes, its frames and bytes */
    if (log < 0)
        log = getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "render") ? 1 : 0;
    if (log)
        fprintf(stderr, "ocerz: m32: render %s %#x: %u frames, %u bytes in %u buffers\n", r->v1 ? "v1" : "v2", r->proc,
                frames, bytes, n);
    int32_t status = (int32_t)m32_call(r->vm, r->proc, args, r->v1 ? 5 : 6, NULL, NULL);
    if (flags)
        *flags = m32_rd(gf);
    static int mute = -1;   /* OCERZ_M32_MUTE=1: the guest still renders (and keeps its time), the host plays silence */
    if (mute < 0)
        mute = getenv("OCERZ_M32_MUTE") ? 1 : 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = gl + 4 + 12 * i, size = m32_rd(e + 4), data = m32_rd(e + 8);
        if (size > io->b[i].size)
            size = io->b[i].size;
        io->b[i].size = size;
        if (io->b[i].data && mute)
            memset(io->b[i].data, 0, size);
        else if (io->b[i].data && data)   /* the guest may point mData at its own buffer */
            memcpy(io->b[i].data, m32_host(data), size);
    }
    return status;
}

static void *at(const char *sym);

/* AudioUnitSetProperty(unit, id, scope, element, data, size) */
static int sp_AudioUnitSetProperty(struct OcerzVM *vm, OcerzCPU *cpu)
{
    static int32_t (*f)(void *, uint32_t, uint32_t, uint32_t, const void *, uint32_t);
    if (!f)
        f = (int32_t (*)(void *, uint32_t, uint32_t, uint32_t, const void *, uint32_t))at("AudioUnitSetProperty");
    if (!f)
        RET(-4);   /* unimpErr */
    void *unit = m32_host(m32_arg(cpu, 0));
    uint32_t id = m32_arg(cpu, 1), scope = m32_arg(cpu, 2), element = m32_arg(cpu, 3);
    uint32_t data = m32_arg(cpu, 4), size = m32_arg(cpu, 5);
    static int off = -1;   /* OCERZ_M32_NO_AUDIOCB=1: the struct goes through as before (the host rejects it) */
    if (off < 0)
        off = getenv("OCERZ_M32_NO_AUDIOCB") ? 1 : 0;
    /* kAudioUnitProperty_SetInputCallback (7) is Mac OS X 10.1's render callback, which audio units no longer take
     * (kAudioUnitErr_InvalidProperty): SDL 1.2's CoreAudio driver sets it, and Neverball played silent.  It becomes
     * kAudioUnitProperty_SetRenderCallback, whose thunk calls the guest the old way. */
    int v1 = id == 7;
    if ((id == 23 || id == 2005 || v1) && data && size == 8 && !off) {   /* SetRenderCallback, SetInputCallback */
        struct { void *proc, *refcon; } host = { NULL, NULL };
        if (m32_rd(data)) {
            Render *r = calloc(1, sizeof *r);
            if (!r)
                RET(-108);
            r->vm = vm;
            r->proc = m32_rd(data);
            r->refcon = m32_rd(data + 4);
            r->v1 = v1;
            host.proc = (void *)render_thunk;
            host.refcon = r;
        }
        RET(f(unit, v1 ? 23 : id, scope, element, &host, sizeof host));
    }
    RET(f(unit, id, scope, element, m32_host(data), size));
}

/* AudioQueue output (Bink's and Miles' CoreAudio drivers, MW3's movies and sound).  An AudioQueueBuffer is a
 * structure the guest writes through: i386 {capacity, mAudioData, mAudioDataByteSize, mUserData,
 * mPacketDescriptionCapacity, mPacketDescriptions, mPacketDescriptionCount}, 28 bytes (clang -arch i386), where the
 * host's pointers are 8 bytes (mAudioData at 8, mAudioDataByteSize at 16).  Each host
 * buffer gets a guest twin in that layout with its samples in guest memory; enqueueing copies the samples to the host
 * buffer, and the output callback hands the guest its twin.  AudioStreamPacketDescription and
 * AudioStreamBasicDescription are the same bytes to both sides.
 * ponytail: a table of 512 buffers, searched linearly; AudioQueueNewInput and AudioQueueFreeBuffer are not covered
 * (Bink and Miles free their buffers with the queue). */
enum { AQB_CAP = 0, AQB_DATA = 4, AQB_SIZE = 8, AQB_SIZE32 = 28 };
typedef struct { void *queue, *host; uint32_t guest; } QueueBuffer;
static QueueBuffer g_qbufs[512];
static pthread_mutex_t g_qlock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { void *proc; uint32_t user; } QueueOut;

static void *at(const char *sym)
{
    void *f = ocerz_bridge_host_symbol(AT_LIB, sym);
    if (!f) {   /* the bridge not set up (test_m32): the framework itself */
        static void *lib;
        if (!lib)
            lib = dlopen(AT_LIB, RTLD_LAZY | RTLD_LOCAL);
        f = lib ? dlsym(lib, sym) : NULL;
    }
    return f;
}
static uint32_t queue_guest(void *host)
{
    uint32_t g = 0;
    pthread_mutex_lock(&g_qlock);
    for (int i = 0; i < 512 && !g; i++)
        if (g_qbufs[i].host == host)
            g = g_qbufs[i].guest;
    pthread_mutex_unlock(&g_qlock);
    return g;
}
static void *queue_host(uint32_t guest)
{
    void *h = NULL;
    pthread_mutex_lock(&g_qlock);
    for (int i = 0; i < 512 && !h; i++)
        if (g_qbufs[i].guest == guest)
            h = g_qbufs[i].host;
    pthread_mutex_unlock(&g_qlock);
    return h;
}
static void queue_out(void *ctx, void *queue, void *host)
{
    QueueOut *q = ctx;
    uint32_t g = queue_guest(host);
    if (g)
        ((void (*)(uint32_t, uint32_t, uint32_t))q->proc)(q->user, m32_handle(queue), g);
}
/* AudioQueueNewOutput(format, callback, userData, runLoop, runLoopMode, flags, outQueue*) */
static int sp_AudioQueueNewOutput(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int32_t (*f)(const void *, void *, void *, void *, void *, uint32_t, void **) = at("AudioQueueNewOutput");
    uint32_t fmt = m32_arg(cpu, 0), cb = m32_arg(cpu, 1), out = m32_arg(cpu, 6);
    QueueOut *q = calloc(1, sizeof *q);
    if (!f || !q || !cb || !fmt || !out)
        RET(-50);   /* kAudio_ParamError */
    q->proc = m32_callback(cb, "v(uuu)", "v(uuu)");
    q->user = m32_arg(cpu, 2);
    void *queue = NULL;
    int32_t r = f(m32_h(fmt), (void *)queue_out, q, m32_host(m32_arg(cpu, 3)), m32_host(m32_arg(cpu, 4)), m32_arg(cpu, 5),
                  &queue);
    m32_wr(out, r ? 0 : m32_handle(queue));
    RET(r);
}
/* AudioQueueAllocateBuffer(queue, capacity, outBuffer*) */
static int sp_AudioQueueAllocateBuffer(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int32_t (*f)(void *, uint32_t, void **) = at("AudioQueueAllocateBuffer");
    void *queue = m32_host(m32_arg(cpu, 0)), *host = NULL;
    uint32_t cap = m32_arg(cpu, 1), out = m32_arg(cpu, 2);
    int32_t r = f ? f(queue, cap, &host) : -4;
    uint32_t g = r ? 0 : m32_malloc(AQB_SIZE32 + cap);
    if (!r && !g)
        r = -108;   /* memFullErr */
    pthread_mutex_lock(&g_qlock);
    int i = 0;
    while (g && i < 512 && g_qbufs[i].host)
        i++;
    if (g && i < 512)
        g_qbufs[i] = (QueueBuffer){ queue, host, g };
    pthread_mutex_unlock(&g_qlock);
    if (g && i == 512) {
        m32_free(g);
        g = 0;
        r = -108;
    }
    if (g) {
        memset(m32_h(g), 0, AQB_SIZE32);
        m32_wr(g + AQB_CAP, cap);
        m32_wr(g + AQB_DATA, g + AQB_SIZE32);
    }
    if (out)
        m32_wr(out, g);
    RET(r);
}
/* AudioQueueEnqueueBuffer(queue, buffer, numPacketDescs, packetDescs) */
static int sp_AudioQueueEnqueueBuffer(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int32_t (*f)(void *, void *, uint32_t, const void *) = at("AudioQueueEnqueueBuffer");
    uint32_t g = m32_arg(cpu, 1), descs = m32_arg(cpu, 3);
    uint8_t *host = queue_host(g);
    if (!f || !host)
        RET(-66687);   /* kAudioQueueErr_InvalidBuffer */
    uint32_t size = m32_rd(g + AQB_SIZE), cap = m32_rd(g + AQB_CAP);
    if (size > cap)
        size = cap;
    void *data;
    memcpy(&data, host + 8, sizeof data);   /* the host's mAudioData */
    memcpy(data, m32_h(m32_rd(g + AQB_DATA)), size);
    memcpy(host + 16, &size, 4);            /* the host's mAudioDataByteSize */
    RET(f(m32_host(m32_arg(cpu, 0)), host, m32_arg(cpu, 2), descs ? m32_h(descs) : NULL));
}
/* AudioQueueDispose(queue, immediate): its buffers go with it */
static int sp_AudioQueueDispose(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int32_t (*f)(void *, uint8_t) = at("AudioQueueDispose");
    void *queue = m32_host(m32_arg(cpu, 0));
    int32_t r = f ? f(queue, (uint8_t)m32_arg(cpu, 1)) : -4;
    pthread_mutex_lock(&g_qlock);
    for (int i = 0; i < 512; i++)
        if (g_qbufs[i].host && g_qbufs[i].queue == queue) {
            m32_free(g_qbufs[i].guest);
            g_qbufs[i] = (QueueBuffer){ 0 };
        }
    pthread_mutex_unlock(&g_qlock);
    RET(r);
}

/* The classic Sound Manager is gone on arm64.  Unresolved, SndNewChannel answered 0 (noErr) without a channel, and
 * Bink (Batman's movies) then waited on callbacks that never came: about half of the launches hung at the intro or
 * skipped to a blank screen.  No channel and an honest error: Bink plays its movies without sound.
 * ponytail: no Sound Manager; movie sound would need SndDoCommand's bufferCmd/callBackCmd on an AudioQueue. */
static int sp_SndNewChannel(struct OcerzVM *vm, OcerzCPU *cpu)   /* (SndChannelPtr *chan, short, long, SndCallBackUPP) */
{
    (void)vm;
    if (m32_arg(cpu, 0))
        m32_wr(m32_arg(cpu, 0), 0);
    RET((uint16_t)-201);   /* notEnoughHardwareErr */
}
static int sp_SndBadChannel(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    RET((uint16_t)-205);   /* badChannel */
}

/* QuickTime is gone from macOS too.  Unresolved, its calls answered 0 (noErr) without doing anything, and Left 4 Dead
 * 2's valve_avi used the Movie NewMovieFromFile never wrote.  The Source engine needs EnterMovies to succeed (its
 * IQuickTime system is required), so QuickTime starts, and no movie opens: unimpErr, with a null reference or Movie.
 * The game shows no video materials.  ponytail: no QuickTime playback. */
#define QT_UNIMP ((uint32_t)(int32_t)-4)   /* unimpErr, an OSErr in AX */
static int sp_EnterMovies(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(0); }
static int sp_GetMoviesError(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(QT_UNIMP); }
static int sp_OpenMovieFile(struct OcerzVM *vm, OcerzCPU *cpu)   /* (const FSSpec *, short *resRefNum, SInt8) */
{
    (void)vm;
    if (m32_arg(cpu, 1))
        *(int16_t *)m32_h(m32_arg(cpu, 1)) = 0;
    RET(QT_UNIMP);
}
static int sp_NewMovieFromFile(struct OcerzVM *vm, OcerzCPU *cpu)   /* (Movie *, short, short *, StringPtr, short, Boolean *) */
{
    (void)vm;
    if (m32_arg(cpu, 0))
        m32_wr(m32_arg(cpu, 0), 0);
    RET(QT_UNIMP);
}

/* The Component Manager calls 32-bit programs find their audio units with (FindNextComponent for the default output,
 * then OpenAComponent): on Apple silicon the Component Manager no longer lists audio units, which are AudioComponents,
 * so SuperTuxKart's OpenAL found no output and played nothing ("Could not open the default sound device").  Its
 * ComponentDescription is an AudioComponentDescription, word for word, and an AudioComponentInstance is the AudioUnit
 * the AudioUnit calls take, so a description the AudioComponent registry answers is answered from it; anything else
 * is the host's Component Manager.  The components and instances made this way are remembered, for the calls that
 * take them.  (Crossed, a Component was taken for a pointer to a one-word structure and copied.) */
static void *g_ac[256];   /* AudioComponents and AudioComponentInstances handed out */
static int g_nac;
static pthread_mutex_t g_aclock = PTHREAD_MUTEX_INITIALIZER;
static void ac_note(void *p)
{
    pthread_mutex_lock(&g_aclock);
    int i = 0;
    while (i < g_nac && g_ac[i] != p)
        i++;
    if (i == g_nac && g_nac < 256)
        g_ac[g_nac++] = p;
    pthread_mutex_unlock(&g_aclock);
}
static int ac_known(void *p)
{
    int found = 0;
    pthread_mutex_lock(&g_aclock);
    for (int i = 0; p && i < g_nac && !found; i++)
        found = g_ac[i] == p;
    pthread_mutex_unlock(&g_aclock);
    return found;
}
static void *cm(const char *sym) { return dlsym(RTLD_DEFAULT, sym); }

/* A description in Mac OS X 10.1's terms, which SDL 1.2's CoreAudio driver still uses: type kAudioUnitComponentType
 * ('aunt'), the kind of unit as subtype ('out ' for an output), and the unit's ID as manufacturer ('def ', the default
 * output).  Today the kind is the type ('auou'), the ID the subtype and Apple the manufacturer.  Answered as it was,
 * SDL found no output and Neverball played silent ("Failed to start CoreAudio: FindNextComponent returned NULL"). */
static const void *modern_desc(const void *desc, uint32_t out[5])
{
    static const uint32_t kinds[][2] = { { 'out ', 'auou' }, { 'muse', 'aumu' }, { 'mufx', 'aumf' },
                                         { 'fmtc', 'aufc' }, { 'efct', 'aufx' }, { 'mixr', 'aumx' } };
    if (!desc)
        return NULL;
    memcpy(out, desc, 20);
    if (out[0] != 'aunt')
        return desc;
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
        if (out[1] == kinds[i][0]) {
            out[0] = kinds[i][1];
            out[1] = out[2];
            out[2] = 'appl';
            return out;
        }
    return desc;
}

/* FindNextComponent(Component prev, ComponentDescription *looking) */
static int sp_FindNextComponent(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    void *(*find_ac)(void *, const void *) = at("AudioComponentFindNext");
    void *(*find)(void *, const void *) = cm("FindNextComponent");
    void *prev = m32_host(m32_arg(cpu, 0)), *c = NULL;
    uint32_t now[5];
    const void *desc = modern_desc(m32_arg(cpu, 1) ? m32_h(m32_arg(cpu, 1)) : NULL, now);
    if (find_ac && (!prev || ac_known(prev)) && (c = find_ac(prev, desc)))
        ac_note(c);
    else if (find && (!prev || !ac_known(prev)))
        c = find(prev, desc);
    RET(m32_handle(c));
}
/* CountComponents(ComponentDescription *looking) */
static int sp_CountComponents(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    uint32_t (*count_ac)(const void *) = at("AudioComponentCount");
    long (*count)(const void *) = cm("CountComponents");
    uint32_t now[5];
    const void *desc = modern_desc(m32_arg(cpu, 0) ? m32_h(m32_arg(cpu, 0)) : NULL, now);
    uint32_t n = count_ac ? count_ac(desc) : 0;
    RET(n ? n : count ? (uint32_t)count(desc) : 0);
}
static void *open_component(void *c, int32_t *err)
{
    int32_t (*new_ac)(void *, void **) = at("AudioComponentInstanceNew");
    int32_t (*open)(void *, void **) = cm("OpenAComponent");
    void *inst = NULL;
    *err = -50;
    if (ac_known(c) && new_ac)
        *err = new_ac(c, &inst);
    else if (c && open)
        *err = open(c, &inst);
    if (*err)
        inst = NULL;
    else if (ac_known(c))
        ac_note(inst);
    return inst;
}
/* OpenComponent(Component) -> ComponentInstance */
static int sp_OpenComponent(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    int32_t err;
    RET(m32_handle(open_component(m32_host(m32_arg(cpu, 0)), &err)));
}
/* OpenAComponent(Component, ComponentInstance *) -> OSErr */
static int sp_OpenAComponent(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    int32_t err;
    void *inst = open_component(m32_host(m32_arg(cpu, 0)), &err);
    if (m32_arg(cpu, 1))
        m32_wr(m32_arg(cpu, 1), m32_handle(inst));
    RET((uint32_t)(int32_t)(int16_t)err);
}
/* CloseComponent(ComponentInstance) -> OSErr */
static int sp_CloseComponent(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    int32_t (*dispose_ac)(void *) = at("AudioComponentInstanceDispose");
    int16_t (*close)(void *) = cm("CloseComponent");
    void *inst = m32_host(m32_arg(cpu, 0));
    int32_t r = ac_known(inst) && dispose_ac ? dispose_ac(inst) : close && inst ? close(inst) : -50;
    RET((uint32_t)(int32_t)(int16_t)r);
}
/* CallComponentVersion(ComponentInstance) -> long */
static int sp_CallComponentVersion(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    void *(*component_of)(void *) = at("AudioComponentInstanceGetComponent");
    int32_t (*version_ac)(void *, uint32_t *) = at("AudioComponentGetVersion");
    long (*version)(void *) = cm("CallComponentVersion");
    void *inst = m32_host(m32_arg(cpu, 0));
    uint32_t v = 0;
    if (ac_known(inst) && component_of && version_ac)
        version_ac(component_of(inst), &v);
    else if (inst && version)
        v = (uint32_t)version(inst);
    RET(v);
}
/* GetComponentInfo(Component, ComponentDescription *, Handle name, Handle info, Handle icon): an audio unit's
 * description; its name, info and icon handles are left as they are, and another component is invalidComponentID */
static int sp_GetComponentInfo(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    int32_t (*describe_ac)(void *, void *) = at("AudioComponentGetDescription");
    void *c = m32_host(m32_arg(cpu, 0));
    if (!ac_known(c))
        RET((uint32_t)(int32_t)-3000);
    int32_t r = describe_ac && m32_arg(cpu, 1) ? describe_ac(c, m32_h(m32_arg(cpu, 1))) : -50;
    RET((uint32_t)(int32_t)(int16_t)r);
}

const M32SpecialEntry m32_audio_specials[] = {
    { "_FindNextComponent", sp_FindNextComponent }, { "_CountComponents", sp_CountComponents },
    { "_OpenComponent", sp_OpenComponent }, { "_OpenAComponent", sp_OpenAComponent },
    { "_CloseComponent", sp_CloseComponent }, { "_CallComponentVersion", sp_CallComponentVersion },
    { "_GetComponentInfo", sp_GetComponentInfo },
    { "_AudioUnitSetProperty", sp_AudioUnitSetProperty },
    { "_AudioQueueNewOutput", sp_AudioQueueNewOutput }, { "_AudioQueueAllocateBuffer", sp_AudioQueueAllocateBuffer },
    { "_AudioQueueEnqueueBuffer", sp_AudioQueueEnqueueBuffer }, { "_AudioQueueDispose", sp_AudioQueueDispose },
    { "_SndNewChannel", sp_SndNewChannel },
    { "_EnterMovies", sp_EnterMovies }, { "_GetMoviesError", sp_GetMoviesError }, { "_OpenMovieFile", sp_OpenMovieFile },
    { "_NewMovieFromFile", sp_NewMovieFromFile },
    { "_SndDoCommand", sp_SndBadChannel }, { "_SndDoImmediate", sp_SndBadChannel },
    { "_SndChannelStatus", sp_SndBadChannel }, { "_SndDisposeChannel", sp_SndBadChannel },
    { NULL, NULL }
};

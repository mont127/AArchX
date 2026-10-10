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
#include <mach/mach_time.h>
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
    int32_t status = (int32_t)m32_call(r->vm, r->proc, args, 6, NULL, NULL);
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

/* AudioUnitSetProperty(unit, id, scope, element, data, size) */
static int sp_AudioUnitSetProperty(struct OcerzVM *vm, OcerzCPU *cpu)
{
    static int32_t (*f)(void *, uint32_t, uint32_t, uint32_t, const void *, uint32_t);
    if (!f)
        f = (int32_t (*)(void *, uint32_t, uint32_t, uint32_t, const void *, uint32_t))
                ocerz_bridge_host_symbol(AT_LIB, "AudioUnitSetProperty");
    if (!f)
        RET(-4);   /* unimpErr */
    void *unit = m32_host(m32_arg(cpu, 0));
    uint32_t id = m32_arg(cpu, 1), scope = m32_arg(cpu, 2), element = m32_arg(cpu, 3);
    uint32_t data = m32_arg(cpu, 4), size = m32_arg(cpu, 5);
    static int off = -1;   /* OCERZ_M32_NO_AUDIOCB=1: the struct goes through as before (the host rejects it) */
    if (off < 0)
        off = getenv("OCERZ_M32_NO_AUDIOCB") ? 1 : 0;
    if ((id == 23 || id == 2005) && data && size == 8 && !off) {   /* SetRenderCallback, SetInputCallback */
        struct { void *proc, *refcon; } host = { NULL, NULL };
        if (m32_rd(data)) {
            Render *r = calloc(1, sizeof *r);
            if (!r)
                RET(-108);
            r->vm = vm;
            r->proc = m32_rd(data);
            r->refcon = m32_rd(data + 4);
            host.proc = (void *)render_thunk;
            host.refcon = r;
        }
        RET(f(unit, id, scope, element, &host, sizeof host));
    }
    RET(f(unit, id, scope, element, m32_host(data), size));
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

const M32SpecialEntry m32_audio_specials[] = {
    { "_AudioUnitSetProperty", sp_AudioUnitSetProperty },
    { "_SndNewChannel", sp_SndNewChannel },
    { "_SndDoCommand", sp_SndBadChannel }, { "_SndDoImmediate", sp_SndBadChannel },
    { "_SndChannelStatus", sp_SndBadChannel }, { "_SndDisposeChannel", sp_SndBadChannel },
    { NULL, NULL }
};

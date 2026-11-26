/*
 * A guest x86_64 Audio Component registers itself with AudioComponentRegister,
 * and the host AudioToolbox then discovers, counts, opens, initializes,
 * property-sets, renders and disposes it as if it were a native component.
 * The fixture is a generator with a stream-format property and a render
 * method, and it also receives an AURenderCallbackStruct through
 * AudioUnitSetProperty, which is the path a guest effect uses to pull input.
 *
 * Two more components hold the bridge to how real plugins keep their
 * interfaces.  One allocates an interface for each instance and frees it in
 * Close, as Apple's AUPlugInDispatch does; its Close then allocates and fills
 * a block of the same size, which nothing may have written to once the
 * instance is disposed.  The other returns the same address every time, with
 * an Open that fails behind it the first time and functions of its own the
 * second: the second instance must open through those.
 *
 * A last one registers with the CanLoadInProcess flag, which AudioToolbox
 * drops from the description it keeps and hands the factory: the factory
 * must be found by that description all the same.
 */
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int opens, closes, inits, uninits, getprops, getinfos, sets, renders, input_calls;
static AURenderCallbackStruct input_cb;
static AudioStreamBasicDescription format;
static int have_format;

static OSStatus guest_input(void *refcon, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                            UInt32 frames, AudioBufferList *ioData)
{
    (void)refcon; (void)flags; (void)ts; (void)bus; (void)frames; (void)ioData;
    input_calls++;
    return noErr;
}

static OSStatus au_open(void *self, AudioComponentInstance instance)
{
    (void)self; (void)instance;
    opens++;
    return noErr;
}

static OSStatus au_close(void *self)
{
    (void)self;
    closes++;
    return noErr;
}

static OSStatus au_init(void *self)
{
    (void)self;
    inits++;
    return noErr;
}

static OSStatus au_uninit(void *self)
{
    (void)self;
    uninits++;
    return noErr;
}

static OSStatus au_getprop(void *self, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element,
                           void *outData, UInt32 *ioDataSize)
{
    (void)self;
    getprops++;
    if (id == kAudioUnitProperty_StreamFormat && scope == kAudioUnitScope_Output && element == 0 &&
        have_format && outData && ioDataSize) {
        if (*ioDataSize >= sizeof format) {
            memcpy(outData, &format, sizeof format);
            *ioDataSize = sizeof format;
            return noErr;
        }
    }
    if (ioDataSize)
        *ioDataSize = 0;
    return -10879;
}

static OSStatus au_getpropinfo(void *self, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element,
                               UInt32 *outDataSize, Boolean *outWritable)
{
    (void)self;
    getinfos++;
    if (id == kAudioUnitProperty_StreamFormat && scope == kAudioUnitScope_Output && element == 0 &&
        outDataSize && outWritable) {
        *outDataSize = sizeof format;
        *outWritable = have_format ? false : true;
        return noErr;
    }
    if (outDataSize)
        *outDataSize = 0;
    if (outWritable)
        *outWritable = false;
    return -10879;
}

static OSStatus au_setprop(void *self, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element,
                           const void *data, UInt32 size)
{
    (void)self;
    sets++;
    if (id == kAudioUnitProperty_SetRenderCallback && data && size == sizeof input_cb) {
        memcpy(&input_cb, data, sizeof input_cb);
        return noErr;
    }
    if (id == kAudioUnitProperty_StreamFormat && scope == kAudioUnitScope_Output && element == 0 && data &&
        size == sizeof format) {
        memcpy(&format, data, sizeof format);
        have_format = 1;
    }
    return noErr;
}

static OSStatus au_render(void *self, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                          UInt32 frames, AudioBufferList *ioData)
{
    (void)self; (void)flags;
    renders++;
    if (input_cb.inputProc) {
        AudioUnitRenderActionFlags in_flags = 0;
        input_cb.inputProc(input_cb.inputProcRefCon, &in_flags, ts, bus, frames, ioData);
    }
    if (ioData)
        for (UInt32 b = 0; b < ioData->mNumberBuffers; b++)
            if (ioData->mBuffers[b].mData)
                memset(ioData->mBuffers[b].mData, 0x11, ioData->mBuffers[b].mDataByteSize);
    return noErr;
}

static AudioComponentMethod au_lookup(SInt16 selector)
{
    switch (selector) {
    case kAudioUnitInitializeSelect: return (AudioComponentMethod)au_init;
    case kAudioUnitUninitializeSelect: return (AudioComponentMethod)au_uninit;
    case kAudioUnitGetPropertyInfoSelect: return (AudioComponentMethod)au_getpropinfo;
    case kAudioUnitGetPropertySelect: return (AudioComponentMethod)au_getprop;
    case kAudioUnitSetPropertySelect: return (AudioComponentMethod)au_setprop;
    case kAudioUnitRenderSelect: return (AudioComponentMethod)au_render;
    default: return NULL;
    }
}

static AudioComponentPlugInInterface interface = { au_open, au_close, au_lookup, NULL };
static AudioComponentPlugInInterface cleanup_interfaces[130];
static int factory_calls;

static AudioComponentPlugInInterface *factory(const AudioComponentDescription *desc)
{
    (void)desc;
    int call = factory_calls++;
    if (call < 2)
        return &interface;
    return &cleanup_interfaces[2 + (call - 2) % 128];
}

static unsigned char *ghost;
static int heap_opens, heap_closes;

static OSStatus heap_open(void *self, AudioComponentInstance instance)
{
    (void)self; (void)instance;
    heap_opens++;
    return noErr;
}

static OSStatus heap_close(void *self)
{
    heap_closes++;
    free(self);
    ghost = malloc(sizeof(AudioComponentPlugInInterface));
    if (ghost)
        memset(ghost, 0xA5, sizeof(AudioComponentPlugInInterface));
    return noErr;
}

static AudioComponentPlugInInterface *heap_factory(const AudioComponentDescription *desc)
{
    (void)desc;
    AudioComponentPlugInInterface *made = malloc(sizeof *made);
    if (made)
        *made = (AudioComponentPlugInInterface){ heap_open, heap_close, au_lookup, NULL };
    return made;
}

static AudioComponentPlugInInterface reused;
static int reused_calls, fail_opens, good_opens;

static OSStatus fail_open(void *self, AudioComponentInstance instance)
{
    (void)self; (void)instance;
    fail_opens++;
    return kAudio_ParamError;
}

static OSStatus good_open(void *self, AudioComponentInstance instance)
{
    (void)self; (void)instance;
    good_opens++;
    return noErr;
}

static AudioComponentPlugInInterface *reused_factory(const AudioComponentDescription *desc)
{
    (void)desc;
    reused = (AudioComponentPlugInInterface){ reused_calls++ ? good_open : fail_open, au_close, au_lookup, NULL };
    return &reused;
}

int main(void)
{
    AudioComponentDescription desc = { 'augn', 'ocez', 'ocez', kAudioComponentFlag_SandboxSafe, 0 };
    for (unsigned i = 0; i < sizeof cleanup_interfaces / sizeof cleanup_interfaces[0]; i++)
        cleanup_interfaces[i] = (AudioComponentPlugInInterface){ au_open, au_close, au_lookup, NULL };
    AudioComponent registered = AudioComponentRegister(&desc, CFSTR("Ocerz Probe"), 0x00010000, factory);
    AudioComponent found = registered ? AudioComponentFindNext(NULL, &desc) : NULL;
    UInt32 count = registered ? AudioComponentCount(&desc) : 0;
    AudioComponent component = found ? found : registered;
    AudioComponentInstance unit = NULL;
    OSStatus instance = component ? AudioComponentInstanceNew(component, &unit) : -1;

    AudioStreamBasicDescription want;
    memset(&want, 0, sizeof want);
    want.mSampleRate = 44100;
    want.mFormatID = kAudioFormatLinearPCM;
    want.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    want.mBytesPerPacket = 8;
    want.mFramesPerPacket = 1;
    want.mBytesPerFrame = 8;
    want.mChannelsPerFrame = 2;
    want.mBitsPerChannel = 32;

    OSStatus set_format = unit ? AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                                                     kAudioUnitScope_Output, 0, &want, sizeof want) : -1;
    UInt32 info_size = 0;
    Boolean writable = false;
    OSStatus info = unit ? AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_StreamFormat,
                                                    kAudioUnitScope_Output, 0, &info_size, &writable) : -1;
    AudioStreamBasicDescription got;
    memset(&got, 0, sizeof got);
    UInt32 got_size = sizeof got;
    OSStatus get = unit ? AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat,
                                               kAudioUnitScope_Output, 0, &got, &got_size) : -1;
    OSStatus initialize = unit ? AudioUnitInitialize(unit) : -1;
    AURenderCallbackStruct render_cb = { guest_input, NULL };
    OSStatus set_callback = unit ? AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback,
                                                       kAudioUnitScope_Input, 0, &render_cb,
                                                       sizeof render_cb) : -1;

    AudioBufferList buffers;
    buffers.mNumberBuffers = 1;
    UInt32 frames = 16;
    float samples[32];
    memset(samples, 0, sizeof samples);
    buffers.mBuffers[0].mNumberChannels = 2;
    buffers.mBuffers[0].mDataByteSize = frames * 8;
    buffers.mBuffers[0].mData = samples;
    AudioTimeStamp ts;
    memset(&ts, 0, sizeof ts);
    ts.mFlags = kAudioTimeStampSampleTimeValid;
    ts.mSampleTime = 0;
    AudioUnitRenderActionFlags flags = 0;
    OSStatus render = unit ? AudioUnitRender(unit, &flags, &ts, 0, frames, &buffers) : -1;
    const unsigned char *bytes = (const unsigned char *)samples;
    int filled = buffers.mBuffers[0].mDataByteSize == frames * 8 && bytes[0] == 0x11 &&
                 bytes[buffers.mBuffers[0].mDataByteSize - 1] == 0x11;

    AudioComponentInstance second = NULL;
    OSStatus instance2 = component ? AudioComponentInstanceNew(component, &second) : -1;
    OSStatus initialize2 = second ? AudioUnitInitialize(second) : -1;
    OSStatus uninitialize2 = second ? AudioUnitUninitialize(second) : -1;
    OSStatus dispose2 = second ? AudioComponentInstanceDispose(second) : -1;
    OSStatus uninitialize = unit ? AudioUnitUninitialize(unit) : -1;
    OSStatus dispose = unit ? AudioComponentInstanceDispose(unit) : -1;
    int cycles = 0;
    for (int i = 0; i < 70; i++) {
        AudioComponentInstance tmp = NULL;
        OSStatus inst = component ? AudioComponentInstanceNew(component, &tmp) : -1;
        OSStatus disp = tmp ? AudioComponentInstanceDispose(tmp) : -1;
        if (inst == noErr && disp == noErr)
            cycles++;
    }
    AudioComponentDescription heap_desc = { 'augn', 'ocz2', 'ocez', kAudioComponentFlag_SandboxSafe, 0 };
    AudioComponent heap_component = AudioComponentRegister(&heap_desc, CFSTR("Ocerz Heap Probe"), 0x00010000,
                                                           heap_factory);
    int heap_cycles = 0, ghost_intact = 1;
    for (int i = 0; i < 3 && heap_component; i++) {
        AudioComponentInstance tmp = NULL;
        ghost = NULL;
        if (AudioComponentInstanceNew(heap_component, &tmp) == noErr && tmp &&
            AudioComponentInstanceDispose(tmp) == noErr)
            heap_cycles++;
        for (size_t k = 0; ghost && k < sizeof(AudioComponentPlugInInterface); k++)
            if (ghost[k] != 0xA5)
                ghost_intact = 0;
        free(ghost);
    }
    AudioComponentDescription reused_desc = { 'augn', 'ocz3', 'ocez', kAudioComponentFlag_SandboxSafe, 0 };
    AudioComponent reused_component = AudioComponentRegister(&reused_desc, CFSTR("Ocerz Reused Probe"), 0x00010000,
                                                             reused_factory);
    AudioComponentInstance refused = NULL, opened = NULL;
    OSStatus first_open = reused_component ? AudioComponentInstanceNew(reused_component, &refused) : noErr;
    OSStatus second_open = reused_component ? AudioComponentInstanceNew(reused_component, &opened) : -1;
    OSStatus second_dispose = opened ? AudioComponentInstanceDispose(opened) : -1;
    AudioComponentDescription flag_desc = { 'augn', 'ocz4', 'ocez',
                                            kAudioComponentFlag_SandboxSafe | kAudioComponentFlag_CanLoadInProcess, 0 };
    AudioComponent flag_component = AudioComponentRegister(&flag_desc, CFSTR("Ocerz Flag Probe"), 0x00010000,
                                                           heap_factory);
    AudioComponentDescription flag_kept = {0};
    AudioComponentInstance flagged = NULL;
    OSStatus flag_described = flag_component ? AudioComponentGetDescription(flag_component, &flag_kept) : -1;
    OSStatus flag_new = flag_component ? AudioComponentInstanceNew(flag_component, &flagged) : -1;
    OSStatus flag_dispose = flagged ? AudioComponentInstanceDispose(flagged) : -1;
    printf("component register=%d find=%d count=%u instance=%d open=%d close=%d\n",
           registered != NULL, found != NULL, count, (int)instance, opens, closes);
    printf("property set=%d info=%d size=%u writable=%d get=%d same=%d init=%d uninit=%d\n",
           (int)set_format, (int)info, info_size, (int)writable, (int)get,
           get == noErr && got_size == sizeof want && memcmp(&got, &want, sizeof want) == 0,
           (int)initialize, (int)uninitialize);
    printf("render status=%d renders=%d frames=%u bytes=%u filled=%d dispose=%d\n",
           (int)render, renders, frames, buffers.mBuffers[0].mDataByteSize, filled, (int)dispose);
    printf("input set=%d calls=%d\n", (int)set_callback, input_calls);
    printf("cleanup cycles=%d\n", cycles);
    printf("again instance=%d init=%d uninit=%d dispose=%d\n", (int)instance2, (int)initialize2,
           (int)uninitialize2, (int)dispose2);
    printf("methods init=%d uninit=%d get=%d getinfo=%d set=%d\n", inits, uninits, getprops, getinfos, sets);
    printf("heap interface cycles=%d opens=%d closes=%d intact=%d\n", heap_cycles, heap_opens, heap_closes,
           ghost_intact);
    printf("reused address first=%d second=%d dispose=%d failed=%d opened=%d\n", first_open != noErr,
           (int)second_open, (int)second_dispose, fail_opens, good_opens);
    printf("flagged component described=%d flags=%#x new=%d dispose=%d\n", (int)flag_described,
           (unsigned)flag_kept.componentFlags, (int)flag_new, (int)flag_dispose);
    return instance == noErr && instance2 == noErr && set_format == noErr && info == noErr && get == noErr &&
           initialize == noErr && initialize2 == noErr && set_callback == noErr && input_calls == 1 && cycles == 70 &&
           render == noErr && uninitialize == noErr &&
           uninitialize2 == noErr && dispose == noErr && dispose2 == noErr && heap_cycles == 3 && ghost_intact &&
           first_open != noErr && second_open == noErr && good_opens == 1 && flag_new == noErr &&
           flag_dispose == noErr ? 0 : 1;
}

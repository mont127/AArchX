/*
 * A guest x86_64 Audio Component registers itself with AudioComponentRegister,
 * and the host AudioToolbox then discovers, counts, opens, initializes,
 * property-sets, renders and disposes it as if it were a native component.
 * The fixture is a generator with a stream-format property and a render
 * method, and it also receives an AURenderCallbackStruct through
 * AudioUnitSetProperty, which is the path a guest effect uses to pull input.
 */
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
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

static AudioComponentPlugInInterface *factory(const AudioComponentDescription *desc)
{
    (void)desc;
    return &interface;
}

int main(void)
{
    AudioComponentDescription desc = { 'augn', 'ocez', 'ocez', kAudioComponentFlag_SandboxSafe, 0 };
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
    printf("component register=%d find=%d count=%u instance=%d open=%d close=%d\n",
           registered != NULL, found != NULL, count, (int)instance, opens, closes);
    printf("property set=%d info=%d size=%u writable=%d get=%d same=%d init=%d uninit=%d\n",
           (int)set_format, (int)info, info_size, (int)writable, (int)get,
           get == noErr && got_size == sizeof want && memcmp(&got, &want, sizeof want) == 0,
           (int)initialize, (int)uninitialize);
    printf("render status=%d renders=%d frames=%u bytes=%u filled=%d dispose=%d\n",
           (int)render, renders, frames, buffers.mBuffers[0].mDataByteSize, filled, (int)dispose);
    printf("input set=%d calls=%d\n", (int)set_callback, input_calls);
    printf("again instance=%d init=%d uninit=%d dispose=%d\n", (int)instance2, (int)initialize2,
           (int)uninitialize2, (int)dispose2);
    printf("methods init=%d uninit=%d get=%d getinfo=%d set=%d\n", inits, uninits, getprops, getinfos, sets);
    return instance == noErr && instance2 == noErr && set_format == noErr && info == noErr && get == noErr &&
           initialize == noErr && initialize2 == noErr && set_callback == noErr && input_calls == 1 &&
           render == noErr && uninitialize == noErr &&
           uninitialize2 == noErr && dispose == noErr && dispose2 == noErr ? 0 : 1;
}

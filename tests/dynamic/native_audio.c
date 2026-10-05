#include <CoreAudio/CoreAudio.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

static _Atomic int proc_calls, block_calls;

static OSStatus io_proc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in,
                        const AudioTimeStamp *in_time, AudioBufferList *out, const AudioTimeStamp *out_time,
                        void *client)
{
    (void)dev; (void)now; (void)in; (void)in_time; (void)out_time;
    atomic_fetch_add((_Atomic int *)client, 1);
    for (UInt32 b = 0; out && b < out->mNumberBuffers; b++)
        for (UInt32 k = 0; k < out->mBuffers[b].mDataByteSize; k++)
            ((char *)out->mBuffers[b].mData)[k] = 0;
    return noErr;
}

int main(void)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    AudioObjectID dev = 0;
    UInt32 size = sizeof dev;
    OSStatus st = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, &dev);
    AudioDeviceIOProcID id = NULL;
    OSStatus c1 = AudioDeviceCreateIOProcID(dev, io_proc, &proc_calls, &id);
    OSStatus s1 = AudioDeviceStart(dev, id);
    for (int k = 0; k < 100 && atomic_load(&proc_calls) < 3; k++)
        usleep(10000);
    OSStatus t1 = AudioDeviceStop(dev, id);
    OSStatus d1 = AudioDeviceDestroyIOProcID(dev, id);
    AudioDeviceIOProcID bid = NULL;
    dispatch_queue_t q = dispatch_queue_create("ocerz.audio", NULL);
    OSStatus c2 = AudioDeviceCreateIOProcIDWithBlock(&bid, dev, q, ^(const AudioTimeStamp *now, const AudioBufferList *in,
                                                                        const AudioTimeStamp *it, AudioBufferList *out,
                                                                        const AudioTimeStamp *ot) {
        (void)now; (void)in; (void)it; (void)ot;
        for (UInt32 b = 0; out && b < out->mNumberBuffers; b++)
            for (UInt32 k = 0; k < out->mBuffers[b].mDataByteSize; k++)
                ((char *)out->mBuffers[b].mData)[k] = 0;
        atomic_fetch_add(&block_calls, 1);
    });
    OSStatus s2 = AudioDeviceStart(dev, bid);
    for (int k = 0; k < 100 && atomic_load(&block_calls) < 3; k++)
        usleep(10000);
    OSStatus t2 = AudioDeviceStop(dev, bid);
    OSStatus d2 = AudioDeviceDestroyIOProcID(dev, bid);
    printf("audio device=%d status=%d %d %d %d %d proc called=%d block %d %d %d %d called=%d\n", dev != 0, (int)st,
           (int)c1, (int)s1, (int)t1, (int)d1, atomic_load(&proc_calls) >= 3, (int)c2, (int)s2, (int)t2, (int)d2,
           atomic_load(&block_calls) >= 3);
    return 0;
}

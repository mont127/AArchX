/* AudioQueue output as Bink's and Miles' CoreAudio drivers use it: i386 AudioQueueBuffers (28 bytes, the samples
 * written through mAudioData), enqueued, and handed back to the output callback as the same buffers. Plays a moment
 * of silence. audioqueue.expected is the x86_64 build's output under Rosetta. */
#include <AudioToolbox/AudioToolbox.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static AudioQueueBufferRef bufs[3];
static volatile int calls, same, intact;

static void played(void *user, AudioQueueRef q, AudioQueueBufferRef b)
{
    int i = (int)(intptr_t)b->mUserData;
    same += i >= 0 && i < 3 && bufs[i] == b && user == (void *)bufs;
    intact += b->mAudioDataBytesCapacity == 4096 && b->mAudioData != NULL;
    if (++calls < 6) {
        memset(b->mAudioData, 0, 4096);
        b->mAudioDataByteSize = 4096;
        AudioQueueEnqueueBuffer(q, b, 0, NULL);
    }
}

int main(void)
{
    AudioStreamBasicDescription f = { 44100, kAudioFormatLinearPCM, kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                                      4, 1, 4, 2, 16, 0 };
    AudioQueueRef q;
    OSStatus e = AudioQueueNewOutput(&f, played, bufs, NULL, NULL, 0, &q);
    printf("new %d\n", (int)e);
    for (int i = 0; i < 3; i++) {
        e = AudioQueueAllocateBuffer(q, 4096, &bufs[i]);
        bufs[i]->mUserData = (void *)(intptr_t)i;
        memset(bufs[i]->mAudioData, 0, 4096);
        bufs[i]->mAudioDataByteSize = 4096;
        e |= AudioQueueEnqueueBuffer(q, bufs[i], 0, NULL);
        printf("buffer %d %d capacity %u\n", i, (int)e, (unsigned)bufs[i]->mAudioDataBytesCapacity);
    }
    printf("start %d\n", (int)AudioQueueStart(q, NULL));
    for (int i = 0; i < 300 && calls < 6; i++)
        usleep(10000);
    AudioQueueStop(q, true);
    printf("callbacks %d same %d intact %d\n", calls >= 6, same == calls, intact == calls);
    printf("dispose %d\n", (int)AudioQueueDispose(q, true));
    return 0;
}

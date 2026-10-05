#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CMSampleBufferRef encoded;
static int encode_calls, decode_calls;
static size_t decoded_w, decoded_h;
static OSStatus decode_status = -1;

static void on_encoded(void *refcon, void *frame, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample)
{
    (void)refcon; (void)frame; (void)flags;
    encode_calls++;
    if (status == noErr && sample && !encoded)
        encoded = (CMSampleBufferRef)CFRetain(sample);
}

static void on_decoded(void *refcon, void *frame, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image,
                       CMTime pts, CMTime duration)
{
    (void)frame; (void)flags; (void)duration;
    (*(int *)refcon)++;
    decode_status = status;
    if (image) {
        decoded_w = CVPixelBufferGetWidth(image);
        decoded_h = CVPixelBufferGetHeight(image);
    }
    (void)pts;
}

static int source_allocs, source_frees;

static void *source_alloc(void *refcon, size_t n)
{
    (*(int *)refcon)++;
    source_allocs++;
    return calloc(1, n);
}

static void source_free(void *refcon, void *p, size_t n)
{
    (void)refcon;
    (void)n;
    source_frees++;
    free(p);
}

static void block_sources(void)
{
    int refs = 0;
    CMBlockBufferCustomBlockSource src;
    memset(&src, 0, sizeof src);
    src.version = kCMBlockBufferCustomBlockSourceVersion;
    src.AllocateBlock = source_alloc;
    src.FreeBlock = source_free;
    src.refCon = &refs;
    CMBlockBufferRef a = NULL, c = NULL;
    OSStatus s1 = CMBlockBufferCreateWithMemoryBlock(NULL, NULL, 32, NULL, &src, 0, 32, 0, &a);
    OSStatus s2 = CMBlockBufferAssureBlockMemory(a);
    OSStatus s3 = CMBlockBufferAppendMemoryBlock(a, NULL, 16, NULL, &src, 0, 16, kCMBlockBufferAssureMemoryNowFlag);
    OSStatus s4 = CMBlockBufferFillDataBytes(7, a, 0, 48);
    OSStatus s5 = CMBlockBufferCreateContiguous(NULL, a, NULL, &src, 0, 48, kCMBlockBufferAlwaysCopyDataFlag, &c);
    char *p = NULL;
    size_t len = 0;
    OSStatus s6 = CMBlockBufferGetDataPointer(c, 0, NULL, &len, &p);
    printf("block source %d %d %d %d %d %d len=%zu byte=%d allocs=%d refcon=%d", (int)s1, (int)s2, (int)s3, (int)s4,
           (int)s5, (int)s6, len, p ? p[47] : -1, source_allocs, refs);
    CFRelease(c);
    CFRelease(a);
    printf(" frees=%d\n", source_frees);
}

int main(void)
{
    block_sources();
    VTCompressionSessionRef enc = NULL;
    OSStatus s1 = VTCompressionSessionCreate(NULL, 64, 48, kCMVideoCodecType_H264, NULL, NULL, NULL, on_encoded, NULL, &enc);
    CVPixelBufferRef px = NULL;
    CVPixelBufferCreate(NULL, 64, 48, kCVPixelFormatType_32BGRA, NULL, &px);
    CVPixelBufferLockBaseAddress(px, 0);
    memset(CVPixelBufferGetBaseAddress(px), 0x80, CVPixelBufferGetDataSize(px));
    CVPixelBufferUnlockBaseAddress(px, 0);
    OSStatus s2 = VTCompressionSessionEncodeFrame(enc, px, CMTimeMake(0, 30), CMTimeMake(1, 30), NULL, NULL, NULL);
    OSStatus s3 = VTCompressionSessionCompleteFrames(enc, kCMTimeInvalid);
    VTDecompressionOutputCallbackRecord rec = { on_decoded, &decode_calls };
    VTDecompressionSessionRef dec = NULL;
    OSStatus s4 = encoded ? VTDecompressionSessionCreate(NULL, CMSampleBufferGetFormatDescription(encoded), NULL, NULL,
                                                         &rec, &dec)
                          : -1;
    OSStatus s5 = dec ? VTDecompressionSessionDecodeFrame(dec, encoded, 0, NULL, NULL) : -1;
    OSStatus s6 = dec ? VTDecompressionSessionWaitForAsynchronousFrames(dec) : -1;
    printf("vt encode=%d,%d,%d calls=%d decode=%d,%d,%d calls=%d status=%d size=%zux%zu\n", (int)s1, (int)s2, (int)s3,
           encode_calls > 0, (int)s4, (int)s5, (int)s6, decode_calls, (int)decode_status, decoded_w, decoded_h);
    if (dec) {
        VTDecompressionSessionInvalidate(dec);
        CFRelease(dec);
    }
    if (enc) {
        VTCompressionSessionInvalidate(enc);
        CFRelease(enc);
    }
    if (encoded)
        CFRelease(encoded);
    CVPixelBufferRelease(px);
    return 0;
}

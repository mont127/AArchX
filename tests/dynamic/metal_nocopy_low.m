#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <sys/mman.h>

static int check(id<MTLDevice> dev, float *mem, size_t len, size_t extra)
{
    static float salt;
    salt += 1000.0f;
    @autoreleasepool {
        size_t n = len / sizeof(float);
        for (size_t i = 0; i < n; i++)
            mem[i] = (float)i + salt;
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@"kernel void k(device const float *in [[buffer(0)]], "
                                                        "device float *out [[buffer(1)]], "
                                                        "uint i [[thread_position_in_grid]]) { out[i] = in[i] * 2.0f; }"
                                               options:nil
                                                 error:&err];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&err];
        id<MTLBuffer> in = [dev newBufferWithBytesNoCopy:mem length:len + extra options:MTLResourceStorageModeShared deallocator:nil];
        id<MTLBuffer> out = [dev newBufferWithLength:len options:MTLResourceStorageModeShared];
        if (!ps || !in || !out) {
            printf("setup failed\n");
            return 1;
        }
        id<MTLCommandBuffer> cb = [[dev newCommandQueue] commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:ps];
        [e setBuffer:in offset:0 atIndex:0];
        [e setBuffer:out offset:0 atIndex:1];
        [e dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const float *o = [out contents];
        size_t bad = 0;
        for (size_t i = 0; i < n; i++)
            if (o[i] != 2.0f * ((float)i + salt))
                bad++;
        if (cb.error || bad) {
            printf("extra %#zx: %zu of %zu wrong\n", extra, bad, n);
            return 1;
        }
        return 0;
    }
}

static int run(id<MTLDevice> dev, size_t skew, size_t extra)
{
    size_t len = 0x10000;
    char *base = mmap((void *)0x240000000, len + 0x10000, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (base == MAP_FAILED) {
        printf("mmap failed\n");
        return 1;
    }
    int rc = check(dev, (float *)(base + skew), len, extra);
    if (rc)
        printf("skew %#zx\n", skew);
    munmap(base, len + 0x10000);
    return rc;
}

int main(void)
{
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    for (int round = 0; dev && round < 2; round++)
        if (run(dev, 0, 0) || run(dev, 0x10, 0) || run(dev, 0, 0x10))
            return 1;
    printf("OK\n");
    return 0;
}

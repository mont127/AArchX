/*
 * Blocks a native framework runs on a thread of its own.  DXMT, the D3D11
 * layer MacNdCheese runs games on, asks a shared event to call a block when it
 * reaches a value; Metal calls it from its listener's dispatch queue, a host
 * thread no guest code is on, and the block hands the work to a run loop with
 * CFRunLoopPerformBlock, on the run loop of a thread it keeps for that.  Both
 * blocks are the guest's, and DXMT is built without ARC, so they are stack
 * blocks when they cross; this file is too.  DXMT also reads the shader cache
 * path through MTLGetShaderCachePath, which no header declares, and D3DMetal
 * keys texture swizzles with two more such calls.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

extern NSString *MTLGetShaderCachePath(void);
extern uint64_t MTLTextureSwizzleChannelsToKey(uint32_t packed_channels);
extern uint64_t MTLTextureSwizzleKeyToChannels(uint64_t key);

static _Atomic(CFRunLoopRef) g_loop;

static void *loop_thread(void *arg)
{
    (void)arg;
    CFRunLoopSourceContext ctx = { 0 };
    CFRunLoopSourceRef keep = CFRunLoopSourceCreate(NULL, 0, &ctx);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), keep, kCFRunLoopCommonModes);
    atomic_store(&g_loop, CFRunLoopGetCurrent());
    CFRunLoopRun();
    return NULL;
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            printf("no Metal device\n");
            return 0;
        }
        id<MTLSharedEvent> event = [device newSharedEvent];
        MTLSharedEventListener *listener = [[MTLSharedEventListener alloc] init];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block uint64_t seen = 0;
        __block int on_main = -1;
        CFRunLoopRef main_loop = CFRunLoopGetMain();
        [event notifyListener:listener
                      atValue:2
                        block:^(id<MTLSharedEvent> e, uint64_t value) {
                            (void)e;
                            seen = value;
                            CFRunLoopPerformBlock(main_loop, kCFRunLoopCommonModes, ^{
                                on_main = [NSThread isMainThread];
                                dispatch_semaphore_signal(done);
                            });
                            CFRunLoopWakeUp(main_loop);
                        }];
        event.signaledValue = 2;
        int got = 0;
        for (int k = 0; k < 100 && !got; k++) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
            got = dispatch_semaphore_wait(done, DISPATCH_TIME_NOW) == 0;
        }
        printf("listener block ran: %s, value %llu, follow-up on the main run loop: %d\n", got ? "yes" : "no",
               (unsigned long long)seen, on_main);

        pthread_t t;
        pthread_create(&t, NULL, loop_thread, NULL);
        while (!atomic_load(&g_loop))
            usleep(1000);
        __block int ran_on_loop = 0;
        dispatch_semaphore_t done2 = dispatch_semaphore_create(0);
        [event notifyListener:listener
                      atValue:3
                        block:^(id<MTLSharedEvent> e, uint64_t value) {
                            (void)e;
                            (void)value;
                            CFRunLoopRef loop = atomic_load(&g_loop);
                            CFRunLoopPerformBlock(loop, kCFRunLoopCommonModes, ^{
                                ran_on_loop = CFRunLoopGetCurrent() == loop;
                                dispatch_semaphore_signal(done2);
                            });
                            CFRunLoopWakeUp(loop);
                        }];
        event.signaledValue = 3;
        long late = dispatch_semaphore_wait(done2, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
        printf("follow-up on a thread's own run loop: %s, on that loop: %d\n", late ? "no" : "yes", ran_on_loop);
        printf("shader cache path is a string: %d\n", [MTLGetShaderCachePath() isKindOfClass:[NSString class]]);
        int round = 0;
        for (uint32_t c = 0; c < 6 * 6 * 6 * 6; c++) {
            uint32_t packed = c % 6 | (c / 6 % 6) << 8 | (c / 36 % 6) << 16 | (c / 216) << 24;
            round += (uint32_t)MTLTextureSwizzleKeyToChannels(MTLTextureSwizzleChannelsToKey(packed)) == packed;
        }
        printf("texture swizzle keys round-trip: %d of 1296\n", round);
    }
    return 0;
}

/*
 * Enumerates the Metal devices the host reports: registry ID and name for each,
 * exiting non-zero when there are none.  Built native (not for the guest) to
 * establish what Metal sees outside the emulator, as the baseline for what a
 * translated process should get.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stdio.h>

int main(void)
{
    @autoreleasepool {
        NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
        printf("devices=%lu\n", (unsigned long)devices.count);
        for (id<MTLDevice> device in devices)
            printf("registry=%#llx name=%s\n",
                   (unsigned long long)device.registryID,
                   device.name.UTF8String);
        return devices.count ? 0 : 1;
    }
}

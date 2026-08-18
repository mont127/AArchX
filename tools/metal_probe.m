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

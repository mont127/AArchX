/*
 * The Foundation headers sdkgen parses, which is the framework's own umbrella
 * plus the one public header of C declarations the umbrella leaves out.
 *
 * Foundation.h imports every NS*.h but NSDebug.h, which declares
 * NSZombieEnabled, NSDebugEnabled and NSDeallocateZombies and the stack
 * walkers NSFrameAddress, NSReturnAddress and NSCountFrames, and
 * NSItemProviderReadingWriting.h, which only imports NSItemProvider.h.  It is
 * parsed as Objective-C, because Foundation's C interface is declared in terms
 * of its classes, and the method declarations beside it are not functions and
 * are passed over.
 */
#import <Foundation/Foundation.h>
#import <Foundation/NSDebug.h>

/*
 * The Objective-C runtime headers sdkgen parses: every public header the SDK
 * keeps under usr/include/objc, parsed as Objective-C.
 *
 * libobjc has no umbrella of its own.  runtime.h, message.h, objc-exception.h,
 * objc-sync.h and NSObjCRuntime.h declare nearly every function a program
 * calls by name, and the rest - objc.h and objc-api.h beneath them, the
 * hashtable and NXMapTable interface in hashtable2.h, the collector entry
 * points in objc-auto.h, objc-load.h, and the NSObject, Object, List and
 * Protocol interfaces - cost nothing to parse and between them declare the
 * remaining public exports.  All of them compile together for x86_64 and
 * arm64.  The entry points the compiler calls on a program's behalf, such as
 * objc_retain, objc_alloc_init and objc_autoreleasePoolPush, are declared by
 * none of them; their records come from tools/sdkgen/overrides.
 */
#import <objc/List.h>
#import <objc/NSObjCRuntime.h>
#import <objc/NSObject.h>
#import <objc/Object.h>
#import <objc/Protocol.h>
#import <objc/hashtable.h>
#import <objc/hashtable2.h>
#import <objc/message.h>
#import <objc/objc-api.h>
#import <objc/objc-auto.h>
#import <objc/objc-class.h>
#import <objc/objc-exception.h>
#import <objc/objc-load.h>
#import <objc/objc-runtime.h>
#import <objc/objc-sync.h>
#import <objc/objc.h>
#import <objc/runtime.h>

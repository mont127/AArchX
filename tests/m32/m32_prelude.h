/* Foundation keeps these 64-bit-only classes out of an i386 compile while AppKit still names them */
#ifdef __OBJC__
#import <Foundation/NSObject.h>
@interface NSExtensionContext : NSObject @end
@interface NSItemProvider : NSObject @end
@interface NSUserActivity : NSObject @end
#endif

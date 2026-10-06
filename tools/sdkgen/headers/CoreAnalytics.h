/* CoreAnalytics ships no header.  Its two event calls are declared as Apple's
   open-source Security (OSX/utilities/SecCoreAnalytics.m) and python wrapper
   call them: an event's name and payload, or a block that builds the payload
   only if the event is collected. */
#import <Foundation/Foundation.h>

extern void AnalyticsSendEvent(NSString *eventName, NSDictionary<NSString *, NSObject *> *eventPayload);
extern void AnalyticsSendEventLazy(NSString *eventName, NSDictionary<NSString *, NSObject *> *(^eventPayloadBuilder)(void));

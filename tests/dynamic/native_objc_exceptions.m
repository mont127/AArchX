#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

@interface OcerzError : NSException
@end
@implementation OcerzError
@end

static int released;

@interface Tracked : NSObject
@end
@implementation Tracked
- (void)dealloc
{
    released++;
    [super dealloc];
}
@end

static __attribute__((noinline)) void thrower(int kind)
{
    switch (kind) {
    case 0:
        @throw [NSException exceptionWithName:@"Guest" reason:@"thrown with @throw" userInfo:nil];
    case 1:
        [NSException raise:@"Raised" format:@"code %d", 7];
        break;
    case 2:
        [@[] objectAtIndex:3];
        break;
    case 3:
        @throw [OcerzError exceptionWithName:@"Mine" reason:@"a subclass" userInfo:nil];
    case 4:
        @throw @"a string";
    case 5: {
        id none = getenv("OCERZ_NO_SUCH_VARIABLE") ? @"" : nil;
        [[NSMutableDictionary dictionary] setObject:none forKey:@"k"];
        break;
    }
    }
}

static __attribute__((noinline)) void through(int kind)
{
    Tracked *t = [[Tracked alloc] init];
    @try {
        thrower(kind);
    } @finally {
        [t release];
    }
}

static void catches(void)
{
    for (int k = 0; k < 6; k++) {
        @try {
            thrower(k);
            printf("%d no throw\n", k);
        } @catch (OcerzError *e) {
            printf("%d OcerzError %s %s\n", k, e.name.UTF8String, e.reason.UTF8String);
        } @catch (NSException *e) {
            printf("%d NSException %s %s\n", k, e.name.UTF8String, e.reason.UTF8String);
        } @catch (id e) {
            printf("%d id %s\n", k, [[e description] UTF8String]);
        } @finally {
            printf("%d finally\n", k);
        }
    }
}

static void nesting(void)
{
    @try {
        @try {
            thrower(0);
        } @catch (NSException *e) {
            printf("inner %s\n", e.name.UTF8String);
            @throw;
        }
    } @catch (NSException *e) {
        printf("outer %s\n", e.name.UTF8String);
    }
    @try {
        @try {
            thrower(2);
        } @catch (OcerzError *e) {
            printf("wrong clause\n");
        }
    } @catch (NSException *e) {
        printf("passed over %s\n", e.name.UTF8String);
    }
    for (int k = 0; k < 3; k++) {
        @try {
            through(k == 2 ? 1 : k);
        } @catch (NSException *e) {
            printf("through %s released=%d\n", e.name.UTF8String, released);
        }
    }
    NSObject *lock = [[NSObject alloc] init];
    @try {
        @synchronized (lock) {
            thrower(1);
        }
    } @catch (NSException *e) {
        printf("synchronized %s\n", e.name.UTF8String);
    }
    @synchronized (lock) {
        printf("relocked\n");
    }
    [lock release];
    int caught = 0;
    for (int k = 0; k < 2000; k++) {
        @try {
            thrower(k & 1);
        } @catch (NSException *e) {
            caught++;
        }
    }
    printf("caught %d\n", caught);
}

static void uncaught(int kind)
{
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) {
        thrower(kind);
        _exit(0);
    }
    int st = 0;
    waitpid(p, &st, 0);
    printf("uncaught %d signaled=%d sig=%d\n", kind, WIFSIGNALED(st), WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc > 1) {
            thrower(atoi(argv[1]));
            return 0;
        }
        catches();
        nesting();
        uncaught(0);
        uncaught(2);
    }
    return 0;
}

#import <JavaScriptCore/JavaScriptCore.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    @autoreleasepool {
        JSContext *ctx = [[JSContext alloc] init];
        JSValue *v = [ctx evaluateScript:@"var a = [];"
                                          "for (var i = 0; i < 50000; i++) a.push({k: i, s: 'x' + i});"
                                          "var t = 0;"
                                          "for (var j = 0; j < a.length; j++) t += a[j].k;"
                                          "t"];
        const char *s = [[v toString] UTF8String];
        if (!s || strcmp(s, "1249975000") != 0) {
            printf("BAD result %s\n", s ? s : "(null)");
            return 1;
        }
    }
    printf("OK\n");
    return 0;
}

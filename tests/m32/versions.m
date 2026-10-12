// The framework version variables (doubles, aligned to 4 on i386 and 8 on arm64): SDL 2 reads NSAppKitVersionNumber
// to decide whether display modes can be queried, and finds no display when it reads 0.
#import <AppKit/AppKit.h>
int main(void) {
    printf("appkit %d\n", NSAppKitVersionNumber >= NSAppKitVersionNumber10_6);
    printf("foundation %d\n", NSFoundationVersionNumber >= NSFoundationVersionNumber10_6);
    printf("corefoundation %d\n", kCFCoreFoundationVersionNumber >= kCFCoreFoundationVersionNumber10_6);
    return 0;
}

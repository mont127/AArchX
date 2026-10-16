/*
 * A framework loaded with dlopen is found by its bundle identifier, as
 * D3DMetal finds its own bundle to locate the shader library it ships.  The
 * host's NSBundle knows only frameworks host dyld loaded, so under native mode
 * ocerz answers for the ones its own loader mapped.  NSBundle's bundleForClass:
 * finds the same bundle from a class the framework defines.
 *
 *   native_bundle_id <path to OcerzProbe.framework/OcerzProbe>
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2)
            return 2;
        int before = [NSBundle bundleWithIdentifier:@"org.ocerz.probe"] != nil;
        void *h = dlopen(argv[1], RTLD_NOW);
        NSBundle *b = [NSBundle bundleWithIdentifier:@"org.ocerz.probe"];
        NSString *res = [b pathForResource:@"probe" ofType:@"txt"];
        NSString *text = res ? [NSString stringWithContentsOfFile:res encoding:NSUTF8StringEncoding error:NULL] : nil;
        printf("bundle by identifier: before dlopen %d, loaded %d, found %d, resource '%s', unknown %d\n", before,
               h != NULL, b != nil, text ? text.UTF8String : "", [NSBundle bundleWithIdentifier:@"org.ocerz.none"] != nil);
        /* Foundation names a class's bundle by the image class_getImageName gives, which for a guest class in
           native mode is the image ocerz mapped it from: Sequel Pro loads its nibs this way. */
        NSBundle *byClass = [NSBundle bundleForClass:NSClassFromString(@"OcerzProbeClass")];
        NSString *res2 = [byClass pathForResource:@"probe" ofType:@"txt"];
        printf("bundle by class: found %d, same %d, identifier %s, resource %d\n", byClass != nil, byClass == b,
               byClass.bundleIdentifier ? byClass.bundleIdentifier.UTF8String : "(none)", res2 != nil);
    }
    return 0;
}

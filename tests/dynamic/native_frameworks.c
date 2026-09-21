#include <CFNetwork/CFNetwork.h>
#include <CoreServices/CoreServices.h>
#include <Security/Security.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned retains;
    unsigned releases;
    unsigned callbacks;
    unsigned cookie;
} Context;

static void check_context(Context *ctx)
{
    assert(ctx && ctx->cookie == 0x12345678);
    assert(CFStringGetLength(CFSTR("guest callback")) == 14);
}

static const void *retain_context(const void *info)
{
    Context *ctx = (Context *)info;
    check_context(ctx);
    ctx->retains++;
    return info;
}

static void release_context(const void *info)
{
    Context *ctx = (Context *)info;
    check_context(ctx);
    ctx->releases++;
}

static CFStringRef describe_context(const void *info)
{
    check_context((Context *)info);
    return CFRetain(CFSTR("native framework guest context"));
}

static void *retain_stream_context(void *info)
{
    return (void *)retain_context(info);
}

static void release_stream_context(void *info)
{
    release_context(info);
}

static CFStringRef describe_stream_context(void *info)
{
    return describe_context(info);
}

static void check_proxy_list(CFArrayRef proxies)
{
    assert(proxies && CFArrayGetCount(proxies) == 2);
    CFDictionaryRef proxy = CFArrayGetValueAtIndex(proxies, 0);
    assert(CFEqual(CFDictionaryGetValue(proxy, kCFProxyTypeKey), kCFProxyTypeHTTP));
    assert(CFEqual(CFDictionaryGetValue(proxy, kCFProxyHostNameKey), CFSTR("127.0.0.1")));
    int port = 0;
    assert(CFNumberGetValue(CFDictionaryGetValue(proxy, kCFProxyPortNumberKey), kCFNumberIntType, &port));
    assert(port == 8181);
    proxy = CFArrayGetValueAtIndex(proxies, 1);
    assert(CFEqual(CFDictionaryGetValue(proxy, kCFProxyTypeKey), kCFProxyTypeNone));
}

static void proxy_callback(void *info, CFArrayRef proxies, CFErrorRef error)
{
    Context *ctx = info;
    check_context(ctx);
    assert(!error);
    check_proxy_list(proxies);
    ctx->callbacks++;
}

static void host_callback(CFHostRef host, CFHostInfoType type, const CFStreamError *error, void *info)
{
    (void)host;
    (void)type;
    (void)error;
    check_context(info);
}

static void reachability_callback(SCNetworkReachabilityRef target, SCNetworkReachabilityFlags flags, void *info)
{
    (void)target;
    (void)flags;
    check_context(info);
}

static void store_callback(SCDynamicStoreRef store, CFArrayRef keys, void *info)
{
    (void)store;
    (void)keys;
    check_context(info);
}

static void test_network(void)
{
    CFURLRef url = CFURLCreateWithString(NULL, CFSTR("http://example.invalid/test"), NULL);
    assert(url);
    CFDictionaryRef settings = CFDictionaryCreate(NULL, NULL, NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFArrayRef proxies = CFNetworkCopyProxiesForURL(url, settings);
    assert(proxies && CFArrayGetCount(proxies) == 1);
    assert(CFEqual(CFDictionaryGetValue(CFArrayGetValueAtIndex(proxies, 0), kCFProxyTypeKey), kCFProxyTypeNone));
    CFRelease(proxies);
    void *handle = dlopen("/System/Library/Frameworks/CFNetwork.framework/Versions/A/CFNetwork", RTLD_NOW);
    assert(handle);
    CFArrayRef (*copy_proxies)(CFURLRef, CFDictionaryRef) = dlsym(handle, "CFNetworkCopyProxiesForURL");
    assert(copy_proxies);
    proxies = copy_proxies(url, settings);
    assert(proxies && CFArrayGetCount(proxies) == 1);
    CFRelease(proxies);
    assert(dlclose(handle) == 0);
    CFRelease(settings);
    assert(CFGetTypeID(kCFProxyAutoConfigurationURLKey) == CFStringGetTypeID());
    assert(CFGetTypeID(kCFProxyTypeAutoConfigurationURL) == CFStringGetTypeID());
    assert(CFGetTypeID(kCFProxyTypeHTTPS) == CFStringGetTypeID());
    CFStringRef script = CFSTR("function FindProxyForURL(url, host) { return 'PROXY 127.0.0.1:8181; DIRECT'; }");
    CFErrorRef error = NULL;
    proxies = CFNetworkCopyProxiesForAutoConfigurationScript(script, url, &error);
    assert(!error);
    check_proxy_list(proxies);
    CFRelease(proxies);

    Context ctx = {.cookie = 0x12345678};
    CFStreamClientContext context = {0, &ctx, retain_stream_context, release_stream_context, describe_stream_context};
    CFRunLoopSourceRef source = CFNetworkExecuteProxyAutoConfigurationScript(script, url, proxy_callback, &context);
    assert(source);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
    for (int i = 0; i < 100 && !ctx.callbacks; i++)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
    assert(ctx.callbacks == 1);
    CFRunLoopSourceInvalidate(source);
    CFRelease(source);
    assert(ctx.retains > 0 && ctx.retains == ctx.releases);

    ctx = (Context){.cookie = 0x12345678};
    CFHostRef host = CFHostCreateWithName(NULL, CFSTR("localhost"));
    CFHostClientContext host_context = {0, &ctx, retain_context, release_context, describe_context};
    assert(host && CFHostSetClient(host, host_callback, &host_context));
    assert(CFHostSetClient(host, NULL, NULL));
    CFRelease(host);
    assert(ctx.retains > 0 && ctx.retains == ctx.releases);
    CFRelease(url);
    puts("CFNetwork proxy and guest callbacks ok");
}

static void test_configuration(void)
{
    CFStringRef key = SCDynamicStoreKeyCreateProxies(NULL);
    assert(key && CFEqual(key, CFSTR("State:/Network/Global/Proxies")));
    CFRelease(key);
    CFDictionaryRef proxies = SCDynamicStoreCopyProxies(NULL);
    assert(proxies && CFGetTypeID(proxies) == CFDictionaryGetTypeID());
    CFRelease(proxies);
    Context ctx = {.cookie = 0x12345678};
    SCDynamicStoreContext context = {0, &ctx, retain_context, release_context, describe_context};
    SCDynamicStoreRef store = SCDynamicStoreCreate(NULL, CFSTR("ocerz bridge test"), store_callback, &context);
    assert(store);
    CFRelease(store);
    assert(ctx.retains > 0 && ctx.retains == ctx.releases);
    ctx = (Context){.cookie = 0x12345678};
    SCNetworkReachabilityRef target = SCNetworkReachabilityCreateWithName(NULL, "localhost");
    SCNetworkReachabilityContext reach_context = {0, &ctx, retain_context, release_context, describe_context};
    assert(target && SCNetworkReachabilitySetCallback(target, reachability_callback, &reach_context));
    assert(SCNetworkReachabilitySetCallback(target, NULL, NULL));
    CFRelease(target);
    assert(ctx.retains > 0 && ctx.retains == ctx.releases);
    puts("SystemConfiguration reads and guest contexts ok");
}

static void test_security(const char *certificate_path)
{
    unsigned char bytes[8192];
    FILE *file = fopen(certificate_path, "rb");
    assert(file);
    size_t length = fread(bytes, 1, sizeof bytes, file);
    assert(length > 0 && length < sizeof bytes && !ferror(file));
    fclose(file);
    CFDataRef data = CFDataCreate(NULL, bytes, (CFIndex)length);
    SecCertificateRef certificate = SecCertificateCreateWithData(NULL, data);
    assert(certificate);
    CFRelease(data);
    CFStringRef name = SecCertificateCopySubjectSummary(certificate);
    assert(name && CFEqual(name, CFSTR("ocerz framework test")));
    CFRelease(name);
    SecPolicyRef policy = SecPolicyCreateBasicX509();
    assert(policy && CFGetTypeID(policy) == SecPolicyGetTypeID());
    CFDictionaryRef properties = SecPolicyCopyProperties(policy);
    assert(properties);
    CFRelease(properties);
    SecTrustRef trust = NULL;
    assert(SecTrustCreateWithCertificates(certificate, policy, &trust) == errSecSuccess && trust);
    const void *values[] = {certificate};
    CFArrayRef anchors = CFArrayCreate(NULL, values, 1, &kCFTypeArrayCallBacks);
    assert(SecTrustSetAnchorCertificates(trust, anchors) == errSecSuccess);
    assert(SecTrustSetAnchorCertificatesOnly(trust, true) == errSecSuccess);
    assert(SecTrustSetNetworkFetchAllowed(trust, false) == errSecSuccess);
    SecTrustResultType result = kSecTrustResultInvalid;
    assert(SecTrustEvaluate(trust, &result) == errSecSuccess);
    assert(result == kSecTrustResultUnspecified || result == kSecTrustResultProceed);
    CFRelease(anchors);
    CFRelease(trust);
    CFRelease(policy);
    CFRelease(certificate);
    puts("Security local certificate trust ok");
}

static void test_services(const char *path)
{
    FSRef ref;
    Boolean directory = true;
    assert(FSPathMakeRef((const UInt8 *)path, &ref, &directory) == noErr && !directory);
    UInt8 resolved[4096];
    assert(FSRefMakePath(&ref, resolved, sizeof resolved) == noErr);
    char expected[4096];
    assert(realpath(path, expected));
    assert(strcmp((const char *)resolved, expected) == 0);
    CFStringRef kind = NULL;
    assert(LSCopyKindStringForRef(&ref, &kind) == noErr);
    assert(kind && CFStringGetLength(kind) > 0);
    CFRelease(kind);
    puts("CoreServices file references ok");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "reject-context") == 0) {
        SCNetworkReachabilityRef target = SCNetworkReachabilityCreateWithName(NULL, "localhost");
        assert(target);
        SCNetworkReachabilityContext context = {.version = 42};
        SCNetworkReachabilitySetCallback(target, reachability_callback, &context);
        return 1;
    }
    if (strcmp(argv[1], "reject-launch") == 0) {
        CFURLRef url = CFURLCreateWithString(NULL, CFSTR("file:///ocerz-nonexistent-test"), NULL);
        assert(url);
        LSOpenCFURLRef(url, NULL);
        return 1;
    }
    test_network();
    test_configuration();
    test_security(argv[1]);
    test_services(argv[1]);
    puts("native frameworks ok");
    return 0;
}

/*
 * m32's CoreFoundation pieces (include/ocerz/m32.h): the guest's constant strings and the CF calls that
 * take arrays of object pointers.
 *
 * An i386 CFSTR("...") is a 16-byte structure in the image's __DATA,__cfstring {isa, flags, const char *str,
 * long length}.  Each one gets an immortal host CFString, and the guest address is aliased to it, so passing the
 * literal to any CF or Foundation function reaches a real string while the guest keeps its own pointer.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ocerz/bridge.h"
#include "ocerz/interp.h"
#include "ocerz/m32.h"
#include "ocerz/m32_image.h"
#include "ocerz/m32_objc.h"
#include "ocerz/m32_objcrt.h"

#define CF "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation"
enum { kUTF8 = 0x08000100, kUTF16LE = 0x14000100 };

static void *cf(const char *sym)
{
    void *f = ocerz_bridge_host_symbol(CF, sym);
    if (!f)
        fprintf(stderr, "ocerz: m32: CoreFoundation has no %s\n", sym);
    return f;
}

void m32_cf_alias_strings(M32Image *img)
{
    const M32Sect *s = m32_section(img, "__DATA", "__cfstring");
    if (!s || !s->size)
        return;
    void *(*create)(void *, const uint8_t *, long, uint32_t, unsigned char) = cf("CFStringCreateWithBytes");
    if (!create)
        return;
    for (uint32_t off = 0; off + 16 <= s->size; off += 16) {
        uint32_t g = s->addr + (uint32_t)img->slide + off;
        uint32_t flags = m32_rd(g + 4), str = m32_rd(g + 8), len = m32_rd(g + 12);
        int utf16 = (flags & 0xff) == 0xd0;   /* 0x7c8: 8-bit, 0x7d0: UTF-16 (clang's two forms) */
        void *h = str ? create(NULL, m32_h(str), utf16 ? (long)len * 2 : (long)len, utf16 ? kUTF16LE : kUTF8, 0) : NULL;
        if (h)
            m32_alias(g, h);   /* immortal: the +1 from Create is never released */
    }
}

/* a guest array of n object pointers as a host one (malloc'd) */
static void **host_array(uint32_t g, uint32_t n)
{
    void **a = calloc(n ? n : 1, sizeof *a);
    for (uint32_t i = 0; g && i < n; i++)
        a[i] = m32_host(m32_rd(g + 4 * i));
    return a;
}

/* CFArrayCreate(allocator, const void **values, CFIndex n, const CFArrayCallBacks *cb): the callbacks pointer is
 * NULL or one of CF's own structures (aliased); a guest-built one would need its functions wrapped */
static int sp_CFArrayCreate(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *(*f)(void *, const void **, long, const void *) = cf("CFArrayCreate");
    uint32_t n = m32_arg(cpu, 2), cb = m32_arg(cpu, 3);
    void *hcb = m32_host(cb);
    if (cb && hcb == m32_h(cb))
        m32_log_once("guest-built callbacks are not supported; using NULL for", "CFArrayCreate");
    void **vals = host_array(m32_arg(cpu, 1), n);
    void *r = f ? f(m32_host(m32_arg(cpu, 0)), (const void **)vals, (long)n, cb && hcb != m32_h(cb) ? hcb : NULL) : NULL;
    free(vals);
    m32_ret(cpu, m32_handle(r), 0, 0);
    return OCERZ_STEP_OK;
}

/* CFDictionaryCreate(allocator, keys, values, n, keyCallBacks, valueCallBacks) */
static int sp_CFDictionaryCreate(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *(*f)(void *, const void **, const void **, long, const void *, const void *) = cf("CFDictionaryCreate");
    uint32_t n = m32_arg(cpu, 3), kcb = m32_arg(cpu, 4), vcb = m32_arg(cpu, 5);
    void *hk = m32_host(kcb), *hv = m32_host(vcb);
    if ((kcb && hk == m32_h(kcb)) || (vcb && hv == m32_h(vcb)))
        m32_log_once("guest-built callbacks are not supported; using NULL for", "CFDictionaryCreate");
    void **keys = host_array(m32_arg(cpu, 1), n), **vals = host_array(m32_arg(cpu, 2), n);
    void *r = f ? f(m32_host(m32_arg(cpu, 0)), (const void **)keys, (const void **)vals, (long)n,
                    kcb && hk != m32_h(kcb) ? hk : NULL, vcb && hv != m32_h(vcb) ? hv : NULL) : NULL;
    free(keys);
    free(vals);
    m32_ret(cpu, m32_handle(r), 0, 0);
    return OCERZ_STEP_OK;
}

/* CFAllocatorCreate(allocator, CFAllocatorContext *): nine 4-byte fields on i386 (version, info, then seven
 * callbacks), nine 8-byte ones on the host; every callback becomes a host-callable slot running the guest function */
static int sp_CFAllocatorCreate(struct OcerzVM *vm, OcerzCPU *cpu)
{
    static const char *const g[7] = { "p(p)", "v(p)", "p(p)", "p(iup)", "p(piup)", "v(pp)", "i(iup)" };
    static const char *const h[7] = { "p(p)", "v(p)", "p(p)", "p(lLp)", "p(plLp)", "v(pp)", "l(lLp)" };
    void *(*f)(void *, void *) = cf("CFAllocatorCreate");
    uint32_t c = m32_arg(cpu, 1);
    struct { long version; void *info; void *fn[7]; } ctx = { (long)(int32_t)m32_rd(c), m32_host(m32_rd(c + 4)), { 0 } };
    for (int i = 0; i < 7; i++) {
        uint32_t fn = m32_rd(c + 8 + 4 * i);
        ctx.fn[i] = fn ? m32_callback(fn, g[i], h[i]) : NULL;
    }
    void *r = f && c ? f(m32_host(m32_arg(cpu, 0)), &ctx) : NULL;
    m32_ret(cpu, m32_handle(r), 0, 0);
    return OCERZ_STEP_OK;
}

/* CFBundleGetFunctionPointerForName(bundle, name) / ...DataPointerForName: a system function by name becomes the
 * m32 export of that name (a trap with its database signature); a name no database knows is logged and 0 */
static int bundle_pointer(OcerzCPU *cpu, int data)
{
    void *name = m32_objc_to_host(m32_arg(cpu, 1));
    int (*getc)(void *, char *, long, uint32_t) = cf("CFStringGetCString");
    char buf[300] = "_";
    if (!name || !getc || !getc(name, buf + 1, sizeof buf - 1, kUTF8)) {
        m32_ret(cpu, 0, 0, 0);
        return OCERZ_STEP_OK;
    }
    uint32_t v = m32_export(NULL, buf);
    M32Export *e = v - OCERZ_DYLDAPI_LO < OCERZ_DYLDAPI_HI - OCERZ_DYLDAPI_LO ? m32_export_by_id(v - (uint32_t)OCERZ_DYLDAPI_LO) : NULL;
    if (e && e->kind == M32_EX_UNRESOLVED) {
        m32_log_once(data ? "CFBundleGetDataPointerForName: unknown, answering NULL:" : "CFBundleGetFunctionPointerForName: unknown, answering NULL:", buf + 1);
        v = 0;
    }
    m32_ret(cpu, v, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_CFBundleGetFunctionPointerForName(struct OcerzVM *vm, OcerzCPU *cpu) { return bundle_pointer(cpu, 0); }
static int sp_CFBundleGetDataPointerForName(struct OcerzVM *vm, OcerzCPU *cpu) { return bundle_pointer(cpu, 1); }

/* CFDictionaryGetValueIfPresent(dict, key, const void **value): one value written back */
static int sp_CFDictionaryGetValueIfPresent(struct OcerzVM *vm, OcerzCPU *cpu)
{
    unsigned char (*f)(void *, const void *, const void **) = cf("CFDictionaryGetValueIfPresent");
    const void *v = NULL;
    uint32_t out = m32_arg(cpu, 2);
    unsigned char r = f ? f(m32_host(m32_arg(cpu, 0)), m32_host(m32_arg(cpu, 1)), &v) : 0;
    if (r && out)
        m32_wr(out, m32_handle((void *)v));
    m32_ret(cpu, r, 0, 0);
    return OCERZ_STEP_OK;
}

/* The {version, info, retain, release, copyDescription} context most CF sources take (timers, observers, ports):
 * five 4-byte fields on i386, five 8-byte ones on the host; the callbacks run the guest's own */
typedef struct { long version; void *info, *retain, *release, *copy_desc; } HostCtx;
static HostCtx *host_ctx(uint32_t c, HostCtx *h)
{
    if (!c)
        return NULL;
    uint32_t r = m32_rd(c + 8), rl = m32_rd(c + 12), d = m32_rd(c + 16);
    *h = (HostCtx){ (long)(int32_t)m32_rd(c), m32_host(m32_rd(c + 4)), r ? m32_callback(r, "p(p)", "p(p)") : NULL,
                    rl ? m32_callback(rl, "v(p)", "v(p)") : NULL, d ? m32_callback(d, "p(p)", "p(p)") : NULL };
    return h;
}

/* CFRunLoopTimerCreate(allocator, double fireDate, double interval, CFOptionFlags, CFIndex order, callout, ctx) */
static int sp_CFRunLoopTimerCreate(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *(*f)(void *, double, double, unsigned long, long, void *, HostCtx *) = cf("CFRunLoopTimerCreate");
    uint64_t fire = m32_arg64(cpu, 1), interval = m32_arg64(cpu, 3);
    double fd, iv;
    memcpy(&fd, &fire, 8);
    memcpy(&iv, &interval, 8);
    uint32_t callout = m32_arg(cpu, 7);
    HostCtx ctx;
    void *r = f ? f(m32_host(m32_arg(cpu, 0)), fd, iv, m32_arg(cpu, 5), (long)(int32_t)m32_arg(cpu, 6),
                    callout ? m32_callback(callout, "v(pp)", "v(pp)") : NULL, host_ctx(m32_arg(cpu, 8), &ctx))
                : NULL;
    m32_ret(cpu, m32_handle(r), 0, 0);
    return OCERZ_STEP_OK;
}

/* CFRunLoopTimerGetContext(timer, ctx): version and info back in the guest's layout.
 * ponytail: the callbacks come back NULL (the host holds slots, not the guest's functions); keep a map if a game
 * reads them back. */
static int sp_CFRunLoopTimerGetContext(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void (*f)(void *, HostCtx *) = cf("CFRunLoopTimerGetContext");
    HostCtx h = { 0 };
    uint32_t c = m32_arg(cpu, 1);
    if (f)
        f(m32_host(m32_arg(cpu, 0)), &h);
    if (c) {
        m32_wr(c, (uint32_t)h.version);
        m32_wr(c + 4, m32_handle(h.info));
        for (int i = 2; i < 5; i++)
            m32_wr(c + 4 * i, 0);
    }
    m32_ret(cpu, 0, 0, 0);
    return OCERZ_STEP_OK;
}

/* CF{Read,Write}StreamSetClient(stream, CFOptionFlags events, callback(stream, event, info), CFStreamClientContext *) */
static int stream_client(OcerzCPU *cpu, const char *sym)
{
    unsigned char (*f)(void *, unsigned long, void *, HostCtx *) = cf(sym);
    uint32_t cb = m32_arg(cpu, 2);
    HostCtx ctx;
    unsigned char r = f ? f(m32_host(m32_arg(cpu, 0)), m32_arg(cpu, 1), cb ? m32_callback(cb, "v(pup)", "v(pLp)") : NULL,
                            host_ctx(m32_arg(cpu, 3), &ctx))
                        : 0;
    m32_ret(cpu, r, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_CFReadStreamSetClient(struct OcerzVM *vm, OcerzCPU *cpu) { return stream_client(cpu, "CFReadStreamSetClient"); }
static int sp_CFWriteStreamSetClient(struct OcerzVM *vm, OcerzCPU *cpu) { return stream_client(cpu, "CFWriteStreamSetClient"); }

/* The process is ocerz, but the game asks about itself.  GetProcessBundleLocation(psn, FSRef *) answers with the main
 * bundle (the game's, by the CFProcessPath identity).  ponytail: every psn counts as the game's own; games ask only
 * about themselves. */
static int sp_GetProcessBundleLocation(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *(*main_bundle)(void) = cf("CFBundleGetMainBundle");
    void *(*copy_url)(void *) = cf("CFBundleCopyBundleURL");
    unsigned char (*get_ref)(void *, void *) = cf("CFURLGetFSRef");
    void (*release)(const void *) = cf("CFRelease");
    uint32_t ref = m32_arg(cpu, 1);
    void *url = main_bundle && copy_url ? copy_url(main_bundle()) : NULL;
    int ok = url && ref && get_ref && get_ref(url, m32_h(ref));   /* an FSRef is 80 opaque bytes on both */
    if (url)
        release(url);
    m32_ret(cpu, ok ? 0 : (uint32_t)-50, 0, 0);   /* paramErr */
    return OCERZ_STEP_OK;
}

/* SecCodeCheckValidity(code, flags, requirement): the running code is ocerz, signed by whoever built it, so a game's
 * check of its own signature against its publisher's requirement would fail (errSecCSReqFailed); it passes */
static int sp_SecCodeCheckValidity(struct OcerzVM *vm, OcerzCPU *cpu)
{
    m32_ret(cpu, 0, 0, 0);
    return OCERZ_STEP_OK;
}

/* ---- the Carbon Memory Manager, in the guest heap ----
 * Bink allocates its movie buffers with it.  A block from the host is out of the guest's reach, and a Handle is a
 * pointer to a master pointer the guest dereferences; so pointers and handles are made here: a handle is an 8-byte
 * guest cell {master pointer, size}.  Locking is a no-op (nothing ever moves). */
#define MRET(v) do { m32_ret(cpu, (uint32_t)(v), 0, 0); return OCERZ_STEP_OK; } while (0)
static void os_err(uint32_t g, int16_t e)
{
    if (g)
        memcpy(m32_h(g), &e, 2);
}
static int sp_NewPtr(struct OcerzVM *vm, OcerzCPU *cpu) { MRET(m32_malloc(m32_arg(cpu, 0))); }
static int sp_NewPtrClear(struct OcerzVM *vm, OcerzCPU *cpu) { MRET(m32_calloc(1, m32_arg(cpu, 0))); }
static int sp_DisposePtr(struct OcerzVM *vm, OcerzCPU *cpu) { m32_free(m32_arg(cpu, 0)); MRET(0); }
static uint32_t new_handle(uint32_t size)
{
    uint32_t h = m32_malloc(8), p = m32_malloc(size);
    if (!h || !p) {
        m32_free(h);
        m32_free(p);
        return 0;
    }
    m32_wr(h, p);
    m32_wr(h + 4, size);
    return h;
}
/* TempNewHandle(size, OSErr *), TempHLock / TempHUnlock(h, OSErr *), TempDisposeHandle(h, OSErr *) */
static int sp_TempNewHandle(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t h = new_handle(m32_arg(cpu, 0));
    os_err(m32_arg(cpu, 1), h ? 0 : -108);   /* memFullErr */
    MRET(h);
}
static int sp_TempHLock(struct OcerzVM *vm, OcerzCPU *cpu) { os_err(m32_arg(cpu, 1), 0); MRET(0); }
static int sp_TempDisposeHandle(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t h = m32_arg(cpu, 0);
    if (h) {
        m32_free(m32_rd(h));
        m32_free(h);
    }
    os_err(m32_arg(cpu, 1), 0);
    MRET(0);
}
static int sp_MaxBlock(struct OcerzVM *vm, OcerzCPU *cpu) { MRET(256u << 20); }
static int sp_MemError(struct OcerzVM *vm, OcerzCPU *cpu) { MRET(0); }
/* CFM's GetSharedLibrary / FindSymbol: no Code Fragment Manager libraries exist; say so (an unresolved 0 is noErr,
 * and the caller would then call the symbol it never got) */
static int sp_GetSharedLibrary(struct OcerzVM *vm, OcerzCPU *cpu) { MRET((uint32_t)-2804); }   /* cfragNoLibraryErr */
static int sp_FindSymbol(struct OcerzVM *vm, OcerzCPU *cpu) { MRET((uint32_t)-2805); }         /* cfragNoSymbolErr */

/* ActiveNonFloatingWindow(): Carbon's window list is gone on arm64 (the call does not resolve), and Feral's code takes
 * "no window" to mean the game lost focus, then sends its GL context a message it does not yet have (a crash in
 * makeCurrentContext while the game loads).  The app's front window answers, as the Carbon call would for it. */
static int sp_ActiveNonFloatingWindow(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *(*send)(void *, SEL) = (void *(*)(void *, SEL))M32_MSGSEND;
    void *(*send_i)(void *, SEL, unsigned long) = (void *(*)(void *, SEL, unsigned long))M32_MSGSEND;
    void *app = send((void *)objc_getClass("NSApplication"), sel_registerName("sharedApplication"));
    void *w = app ? send(app, sel_registerName("mainWindow")) : NULL;
    if (!w && app)
        w = send(app, sel_registerName("keyWindow"));
    void *list = !w && app ? send(app, sel_registerName("orderedWindows")) : NULL;
    unsigned long n = list ? (unsigned long)send(list, sel_registerName("count")) : 0;
    for (unsigned long i = 0; i < n && !w; i++) {
        void *c = send_i(list, sel_registerName("objectAtIndex:"), i);
        if ((char)(uintptr_t)send(c, sel_registerName("isVisible")))
            w = c;
    }
    MRET(w ? m32_handle(w) : 0);
}

/* UCKeyTranslate(layout, keyCode, action, modifiers, keyboardType, options, UInt32 *deadKeyState, UniCharCount max,
 * UniCharCount *actual, UniChar *string): UniCharCount is 4 bytes on i386, so the database had no notation for it and
 * the call did nothing; Feral's key mapping then had no character for any letter key (WASD beeped, Escape worked).
 * The layout is the guest's copy of the host bytes (sp_CFDataGetBytePtr). */
static void *cs(const char *sym);
static int sp_UCKeyTranslate(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int32_t (*f)(const void *, uint16_t, uint16_t, uint32_t, uint32_t, uint32_t, uint32_t *, unsigned long,
                 unsigned long *, uint16_t *) = cs("UCKeyTranslate");
    uint32_t dead = m32_arg(cpu, 6), act = m32_arg(cpu, 8), str = m32_arg(cpu, 9);
    unsigned long actual = 0;
    int32_t r = f ? f(m32_host(m32_arg(cpu, 0)), (uint16_t)m32_arg(cpu, 1), (uint16_t)m32_arg(cpu, 2), m32_arg(cpu, 3),
                      m32_arg(cpu, 4), m32_arg(cpu, 5), dead ? (uint32_t *)m32_h(dead) : NULL, m32_arg(cpu, 7),
                      &actual, str ? (uint16_t *)m32_h(str) : NULL) : -50;   /* paramErr */
    if (act)
        m32_wr(act, (uint32_t)actual);
    MRET((uint32_t)r);
}

/* CFDataGetBytePtr, CFDataGetMutableBytePtr, IOHIDValueGetBytePtr: the database has them returning C strings (a
 * const UInt8 * looks like one), so the guest got a copy up to the first zero byte.  The keyboard layout ('uchr')
 * Feral hands UCKeyTranslate was cut after a few bytes and every letter key translated to paramErr: WASD did nothing.
 * Bytes outside the window are copied whole into a guest buffer kept per host pointer and refreshed on each call.
 * ponytail: a copy, so guest writes through CFDataGetMutableBytePtr do not reach the host data; and no alias back
 * to the host bytes (CF reuses those addresses for other objects), so host calls handed the pointer read the copy. */
static uint32_t bytes_to_guest(const uint8_t *p, long n)
{
    if (!p)
        return 0;
    if (m32_in_window(p))
        return m32_g(p);
    static struct { const void *host; uint32_t g, cap; } cache[1024];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    uint32_t i = (uint32_t)(((uintptr_t)p >> 4) * 0x9e3779b1u) & 1023, g;
    pthread_mutex_lock(&lock);
    if (cache[i].host != p || cache[i].cap < (uint32_t)n) {   /* a collision replaces the slot (the old copy leaks) */
        cache[i].host = p;
        cache[i].cap = n > 16 ? (uint32_t)n : 16;
        cache[i].g = m32_malloc(cache[i].cap);
    }
    g = cache[i].g;
    if (g)
        memcpy(m32_h(g), p, (size_t)n);
    pthread_mutex_unlock(&lock);
    return g;
}
static int sp_CFDataGetBytePtr(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const uint8_t *(*ptr)(const void *) = cf("CFDataGetBytePtr");
    long (*len)(const void *) = cf("CFDataGetLength");
    void *d = m32_host(m32_arg(cpu, 0));
    MRET(d && ptr && len ? bytes_to_guest(ptr(d), len(d)) : 0);
}
static int sp_IOHIDValueGetBytePtr(struct OcerzVM *vm, OcerzCPU *cpu)
{
    static const uint8_t *(*ptr)(const void *);
    static long (*len)(const void *);
    if (!ptr) {
        const char *io = "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit";
        ptr = (const uint8_t *(*)(const void *))ocerz_bridge_host_symbol(io, "IOHIDValueGetBytePtr");
        len = (long (*)(const void *))ocerz_bridge_host_symbol(io, "IOHIDValueGetLength");
    }
    void *v = m32_host(m32_arg(cpu, 0));
    MRET(v && ptr && len ? bytes_to_guest(ptr(v), len(v)) : 0);
}

/* The CF calls that fill a caller's array with pointers: 4-byte slots on i386, 8 on the host */
static void guest_array_out(uint32_t g, void **h, long n)
{
    for (long i = 0; g && i < n; i++)
        m32_wr(g + 4 * (uint32_t)i, m32_handle(h[i]));
}

/* CFDictionaryGetKeysAndValues(dict, keys, values) */
static int sp_CFDictionaryGetKeysAndValues(struct OcerzVM *vm, OcerzCPU *cpu)
{
    long (*count)(void *) = cf("CFDictionaryGetCount");
    void (*f)(void *, void **, void **) = cf("CFDictionaryGetKeysAndValues");
    void *d = m32_host(m32_arg(cpu, 0));
    uint32_t gk = m32_arg(cpu, 1), gv = m32_arg(cpu, 2);
    long n = d && count ? count(d) : 0;
    void **k = calloc((size_t)n + 1, sizeof *k), **v = calloc((size_t)n + 1, sizeof *v);
    if (n && f)
        f(d, gk ? k : NULL, gv ? v : NULL);
    guest_array_out(gk, k, n);
    guest_array_out(gv, v, n);
    free(k);
    free(v);
    m32_ret(cpu, 0, 0, 0);
    return OCERZ_STEP_OK;
}

/* CFSetGetValues(set, values) / CFBagGetValues(bag, values) */
static int get_values(OcerzCPU *cpu, const char *count_sym, const char *sym)
{
    long (*count)(void *) = cf(count_sym);
    void (*f)(void *, void **) = cf(sym);
    void *c = m32_host(m32_arg(cpu, 0));
    long n = c && count ? count(c) : 0;
    void **v = calloc((size_t)n + 1, sizeof *v);
    if (n && f)
        f(c, v);
    guest_array_out(m32_arg(cpu, 1), v, n);
    free(v);
    m32_ret(cpu, 0, 0, 0);
    return OCERZ_STEP_OK;
}
static int sp_CFSetGetValues(struct OcerzVM *vm, OcerzCPU *cpu) { return get_values(cpu, "CFSetGetCount", "CFSetGetValues"); }
static int sp_CFBagGetValues(struct OcerzVM *vm, OcerzCPU *cpu) { return get_values(cpu, "CFBagGetCount", "CFBagGetValues"); }

/* CFArrayGetValues(array, CFRange {loc, len}, values) */
static int sp_CFArrayGetValues(struct OcerzVM *vm, OcerzCPU *cpu)
{
    typedef struct { long loc, len; } Range;
    void (*f)(void *, Range, void **) = cf("CFArrayGetValues");
    Range r = { (int32_t)m32_arg(cpu, 1), (int32_t)m32_arg(cpu, 2) };
    void **v = calloc((size_t)(r.len > 0 ? r.len : 0) + 1, sizeof *v);
    if (r.len > 0 && f)
        f(m32_host(m32_arg(cpu, 0)), r, v);
    guest_array_out(m32_arg(cpu, 3), v, r.len);
    free(v);
    m32_ret(cpu, 0, 0, 0);
    return OCERZ_STEP_OK;
}

/* FSCatalogInfo: the i386 FSPermissionInfo ends in a 4-byte reserved word where the host's has an 8-byte FileSecRef,
 * so from the permissions on every field sits 4 bytes later on the host (i386 144 bytes, valence at 136; host 148, at
 * 140).  Handed through as is, a directory's valence came back 0 and a game sized its listing from it, then wrote
 * every entry past the end of the array. */
enum { CAT_PERM = 56, CAT_REST32 = 72, CAT_REST64 = 76, CAT_SIZE32 = 144, CAT_SIZE64 = 148 };
static void *cs(const char *sym)
{
    void *f = ocerz_bridge_host_symbol("/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices", sym);
    if (!f)
        fprintf(stderr, "ocerz: m32: CoreServices has no %s\n", sym);
    return f;
}
static void cat_to_guest(const uint8_t *h, uint32_t g)
{
    memcpy(m32_h(g), h, CAT_PERM + 12);               /* through userID, groupID, reserved1, userAccess, mode */
    m32_wr(g + CAT_PERM + 12, 0);                     /* reserved2 */
    memcpy(m32_h(g + CAT_REST32), h + CAT_REST64, CAT_SIZE32 - CAT_REST32);
}
static void cat_from_guest(uint32_t g, uint8_t *h)
{
    memset(h, 0, CAT_SIZE64);
    memcpy(h, m32_h(g), CAT_PERM + 12);               /* fileSec stays NULL */
    memcpy(h + CAT_REST64, m32_h(g + CAT_REST32), CAT_SIZE32 - CAT_REST32);
}
/* FSGetCatalogInfo(ref, whichInfo, catalogInfo, outName, fsSpec, parentRef) */
static int sp_FSGetCatalogInfo(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int16_t (*f)(const void *, uint32_t, void *, void *, void *, void *) = cs("FSGetCatalogInfo");
    uint32_t info = m32_arg(cpu, 2), name = m32_arg(cpu, 3), spec = m32_arg(cpu, 4), parent = m32_arg(cpu, 5);
    uint8_t h[CAT_SIZE64] __attribute__((aligned(8)));
    memset(h, 0, sizeof h);
    int16_t r = f ? f(m32_h(m32_arg(cpu, 0)), m32_arg(cpu, 1), info ? h : NULL, name ? m32_h(name) : NULL,
                      spec ? m32_h(spec) : NULL, parent ? m32_h(parent) : NULL)
                  : -50;
    if (info && r == 0)
        cat_to_guest(h, info);
    m32_ret(cpu, (uint32_t)(int32_t)r, 0, 0);
    return OCERZ_STEP_OK;
}
/* FSSetCatalogInfo(ref, whichInfo, catalogInfo) */
static int sp_FSSetCatalogInfo(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int16_t (*f)(const void *, uint32_t, const void *) = cs("FSSetCatalogInfo");
    uint32_t info = m32_arg(cpu, 2);
    uint8_t h[CAT_SIZE64] __attribute__((aligned(8)));
    if (info)
        cat_from_guest(info, h);
    int16_t r = f ? f(m32_h(m32_arg(cpu, 0)), m32_arg(cpu, 1), info ? h : NULL) : -50;
    m32_ret(cpu, (uint32_t)(int32_t)r, 0, 0);
    return OCERZ_STEP_OK;
}

const M32SpecialEntry m32_cf_specials[] = {
    { "_NewPtr", sp_NewPtr }, { "_NewPtrClear", sp_NewPtrClear }, { "_DisposePtr", sp_DisposePtr },
    { "_TempNewHandle", sp_TempNewHandle }, { "_TempHLock", sp_TempHLock }, { "_TempHUnlock", sp_TempHLock },
    { "_TempDisposeHandle", sp_TempDisposeHandle }, { "_MaxBlock", sp_MaxBlock }, { "_MemError", sp_MemError },
    { "_GetSharedLibrary", sp_GetSharedLibrary }, { "_FindSymbol", sp_FindSymbol },
    { "_ActiveNonFloatingWindow", sp_ActiveNonFloatingWindow }, { "_UCKeyTranslate", sp_UCKeyTranslate },
    { "_CFDataGetBytePtr", sp_CFDataGetBytePtr }, { "_CFDataGetMutableBytePtr", sp_CFDataGetBytePtr },
    { "_IOHIDValueGetBytePtr", sp_IOHIDValueGetBytePtr },
    { "_FSGetCatalogInfo", sp_FSGetCatalogInfo }, { "_FSSetCatalogInfo", sp_FSSetCatalogInfo },
    { "_CFDictionaryGetKeysAndValues", sp_CFDictionaryGetKeysAndValues }, { "_CFSetGetValues", sp_CFSetGetValues },
    { "_CFBagGetValues", sp_CFBagGetValues }, { "_CFArrayGetValues", sp_CFArrayGetValues },
    { "_GetProcessBundleLocation", sp_GetProcessBundleLocation }, { "_SecCodeCheckValidity", sp_SecCodeCheckValidity },
    { "_CFReadStreamSetClient", sp_CFReadStreamSetClient }, { "_CFWriteStreamSetClient", sp_CFWriteStreamSetClient },
    { "_CFRunLoopTimerGetContext", sp_CFRunLoopTimerGetContext },
    { "_CFRunLoopTimerCreate", sp_CFRunLoopTimerCreate },
    { "_CFDictionaryGetValueIfPresent", sp_CFDictionaryGetValueIfPresent },
    { "_CFBundleGetFunctionPointerForName", sp_CFBundleGetFunctionPointerForName },
    { "_CFBundleGetDataPointerForName", sp_CFBundleGetDataPointerForName },
    { "_CFAllocatorCreate", sp_CFAllocatorCreate },
    { "_CFArrayCreate", sp_CFArrayCreate },
    { "_CFDictionaryCreate", sp_CFDictionaryCreate },
    { NULL, NULL }
};

/*
 * Carbon for i386 programs, on the HIToolbox Apple silicon still has (include/ocerz/m32.h).
 *
 * Much of Carbon's window, menu, control and event API is declared for 32-bit code alone yet still exported, and
 * working, in arm64 HIToolbox; tools/sdkgen gives those functions an arm64 declaration made from the i386 one, so
 * they cross like any other.  What differs is wrapped here, at the host function an export calls (m32_carbon_wrap,
 * beside the HUD's wrap in src/m32_bridge.c):
 *
 * - CreateNewWindow: 64-bit HIToolbox makes only compositing windows.  Without kWindowCompositingAttribute it
 *   answers errUnsupportedWindowAttributesForClass (-5601) for every class and attribute set, where 32-bit Carbon
 *   made a classic window that the program drew into with QuickDraw.  A window an i386 program creates gets the
 *   attribute, and its QuickDraw port is m32's (src/m32_qd.c).
 * - DisposeWindow: the window's QuickDraw port goes first, so no drawing call reaches the window after it.
 *
 * A universal procedure pointer was a function pointer on Intel: New...UPP handed its function back and Dispose...UPP
 * did nothing.  64-bit Carbon exports neither, and m32 answered NewEventHandlerUPP with 0, so every handler Kega
 * Fusion installed was NULL.  m32_carbon_special answers every New...UPP with its argument and every Dispose...UPP
 * with nothing; the call the UPP is handed to converts the guest function like any other (OCERZ_NO_M32_UPP=1 leaves
 * them unresolved).
 *
 * A Carbon event's parameters are typed, and a type's data is laid out for the process's architecture.
 * GetEventParameter and SetEventParameter convert the types whose i386 and arm64 layouts differ:
 *   pointers  WindowRef, ControlRef, MenuRef, RgnHandle, CGContextRef, the CF types and the rest are 4-byte guest
 *             handles of the host's 8-byte pointers (handed through, a ControlRef's low half had come back)
 *   CGFloat   HIPoint, HISize, HIRect and CGFloat are floats on i386
 *   HICommand its MenuRef (or ControlRef or WindowRef) is 4 bytes at offset 8, its item index at 12
 *   CFIndex   4 bytes
 * Every other type is laid out alike and passes through.  64-bit Carbon has no QuickDraw ports, so a GrafPtr the host
 * cannot answer (kEventParamGrafPort of a control's draw event) is the QuickDraw port of the control's window, or the
 * current port.  With typeWildCard the host's actual type decides.  OCERZ_NO_M32_EVENT_PARAMS=1 crosses them raw.
 *
 * An EventRecord is 16 bytes on i386 (what, message, when, where, modifiers at 0, 2, 6, 10, 14, packed to 2) and 20
 * on arm64, whose message is a long: WaitNextEvent, GetNextEvent (WaitNextEvent that does not sleep, on arm64),
 * EventAvail, ConvertEventRefToEventRecord and AEProcessAppleEvent copy it across.
 *
 * The Text Encoding Converter's ConvertFromUnicodeToText and ConvertFromTextToUnicode take ByteCounts, ItemCounts and
 * ByteOffset arrays, longs on arm64, and ConvertFromUnicodeToPString and ConvertFromPStringToUnicode a ByteCount
 * each: they are widened in and narrowed out.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "ocerz/interp.h"
#include "ocerz/m32.h"

#define M32_kWindowCompositingAttribute (1u << 19)

static int32_t (*g_create_new_window)(uint32_t, uint32_t, const void *, void **);

static int32_t create_new_window(uint32_t window_class, uint32_t attributes, const void *bounds, void **out)
{
    return g_create_new_window(window_class, attributes | M32_kWindowCompositingAttribute, bounds, out);
}

static void (*g_dispose_window)(void *);

static void dispose_window(void *window)
{
    m32_qd_window_gone(window);
    g_dispose_window(window);
}

static int upp_new(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    m32_ret(cpu, m32_arg(cpu, 0), 0, 0);
    return OCERZ_STEP_OK;
}

static int upp_dispose(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    m32_ret(cpu, 0, 0, 0);
    return OCERZ_STEP_OK;
}

/* ---- Carbon event parameters ---- */

#define FOUR(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
enum { RAW, PTR, CGF, HCMD, CFIDX };

static int param_kind(uint32_t type, int *n)
{
    static const uint32_t ptrs[] = {
        FOUR('r','g','n','h'), FOUR('w','i','n','d'), FOUR('c','t','r','l'), FOUR('m','e','n','u'), FOUR('c','n','t','x'),
        FOUR('e','v','n','t'), FOUR('h','i','o','b'), FOUR('c','f','s','t'), FOUR('c','f','t','y'), FOUR('c','f','a','r'),
        FOUR('c','f','m','a'), FOUR('c','f','d','c'), FOUR('c','f','m','d'), FOUR('e','t','a','r'), FOUR('v','o','i','d'),
        FOUR('d','r','a','g'), FOUR('g','r','f','p'), FOUR('g','w','l','d'), FOUR('s','h','a','p'), FOUR('w','g','r','p'),
        FOUR('t','d','o','c'), FOUR('s','c','r','p'), FOUR('p','a','s','b'), FOUR('c','a','u','p'), FOUR('i','c','n','r'),
        FOUR('t','b','a','r'), FOUR('t','b','i','t'), FOUR('c','g','i','m'),
    };
    *n = 1;
    for (size_t i = 0; i < sizeof ptrs / sizeof ptrs[0]; i++)
        if (type == ptrs[i])
            return PTR;
    if (type == FOUR('h','i','p','t') || type == FOUR('h','i','s','z')) {
        *n = 2;
        return CGF;
    }
    if (type == FOUR('h','i','r',' ')) {
        *n = 4;
        return CGF;
    }
    if (type == FOUR('c','g','f','l'))
        return CGF;
    if (type == FOUR('h','c','m','d'))
        return HCMD;
    if (type == FOUR('c','f','i','x'))
        return CFIDX;
    return RAW;
}

static struct {
    int32_t (*get)(void *, uint32_t, uint32_t, uint32_t *, unsigned long, unsigned long *, void *);
    int32_t (*set)(void *, uint32_t, uint32_t, unsigned long, const void *);
    void *(*view_window)(void *);
} ev;

static int ev_ready(void)
{
    static int ok = -1;
    if (ok < 0) {
        void *hi = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon", RTLD_LAZY | RTLD_LOCAL);
        *(void **)&ev.get = hi ? dlsym(hi, "GetEventParameter") : NULL;
        *(void **)&ev.set = hi ? dlsym(hi, "SetEventParameter") : NULL;
        *(void **)&ev.view_window = hi ? dlsym(hi, "HIViewGetWindow") : NULL;
        ok = ev.get && ev.set && ev.view_window;
    }
    return ok;
}

enum { WILDCARD = FOUR('*','*','*','*'), GRAFPTR = FOUR('g','r','f','p'), DIRECT = FOUR('-','-','-','-'),
       CONTROL = FOUR('c','t','r','l'), DATA_SIZE_MISMATCH = -9878 };

/* GetEventParameter(EventRef, EventParamName, EventParamType desired, EventParamType *actual, ByteCount size,
 * ByteCount *actualSize, void *data) */
static int sp_GetEventParameter(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    void *event = m32_host(m32_arg(cpu, 0));
    uint32_t name = m32_arg(cpu, 1), desired = m32_arg(cpu, 2), gactual = m32_arg(cpu, 3), size = m32_arg(cpu, 4);
    uint32_t gsize = m32_arg(cpu, 5), data = m32_arg(cpu, 6);
    if (!ev_ready()) {
        m32_ret(cpu, (uint32_t)-50, 0, 0);
        return OCERZ_STEP_OK;
    }
    uint32_t type = desired, actual = 0;
    unsigned long hsize = 0;
    int n, kind;
    if (desired == WILDCARD && ev.get(event, name, WILDCARD, &type, 0, &hsize, NULL) != 0)
        type = WILDCARD;
    kind = getenv("OCERZ_NO_M32_EVENT_PARAMS") ? RAW : param_kind(type, &n);
    int32_t st;
    uint32_t out_size;
    if (kind == RAW) {
        st = ev.get(event, name, desired, &actual, size, &hsize, data ? m32_h(data) : NULL);
        out_size = (uint32_t)hsize;
    } else {
        uint64_t local[4] = { 0, 0, 0, 0 };
        unsigned long want = kind == PTR || kind == CFIDX ? 8 : kind == HCMD ? 24 : 8 * (unsigned long)n;
        out_size = kind == PTR || kind == CFIDX ? 4 : kind == HCMD ? 16 : 4 * (uint32_t)n;
        st = ev.get(event, name, type, &actual, want, &hsize, local);
        if (type == GRAFPTR && (st != 0 || !local[0])) {
            void *control = NULL;
            uint32_t port = 0;
            if (ev.get(event, DIRECT, CONTROL, NULL, sizeof control, NULL, &control) == 0 && control)
                port = m32_qd_window_port(ev.view_window(control));
            if (!port)
                port = m32_qd_current_port();
            st = port ? 0 : st;
            actual = GRAFPTR;
            if (st == 0 && data && size >= 4)
                m32_wr(data, port);
        } else if (st == 0 && data) {
            uint8_t g[16] = { 0 };
            if (kind == PTR || kind == CFIDX) {
                uint32_t v = kind == PTR ? m32_handle((void *)(uintptr_t)local[0]) : (uint32_t)local[0];
                memcpy(g, &v, 4);
            } else if (kind == CGF) {
                for (int i = 0; i < n; i++) {
                    double d;
                    float f;
                    memcpy(&d, (const uint8_t *)local + 8 * i, 8);
                    f = (float)d;
                    memcpy(g + 4 * i, &f, 4);
                }
            } else {   /* HICommand: attributes, commandID, the reference at 8, the item index at 16 */
                const uint8_t *h = (const uint8_t *)local;
                uint64_t ref;
                memcpy(g, h, 8);
                memcpy(&ref, h + 8, 8);
                uint32_t r = m32_handle((void *)(uintptr_t)ref);
                memcpy(g + 8, &r, 4);
                memcpy(g + 12, h + 16, 2);
            }
            if (size < out_size)
                st = DATA_SIZE_MISMATCH;
            else
                memcpy(m32_h(data), g, out_size);
        }
    }
    if (gactual)
        m32_wr(gactual, actual);
    if (gsize)
        m32_wr(gsize, out_size);
    m32_ret(cpu, (uint32_t)st, 0, 0);
    return OCERZ_STEP_OK;
}

/* SetEventParameter(EventRef, EventParamName, EventParamType, ByteCount size, const void *data) */
static int sp_SetEventParameter(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    void *event = m32_host(m32_arg(cpu, 0));
    uint32_t name = m32_arg(cpu, 1), type = m32_arg(cpu, 2), size = m32_arg(cpu, 3), data = m32_arg(cpu, 4);
    if (!ev_ready()) {
        m32_ret(cpu, (uint32_t)-50, 0, 0);
        return OCERZ_STEP_OK;
    }
    int n, kind = getenv("OCERZ_NO_M32_EVENT_PARAMS") || !data ? RAW : param_kind(type, &n);
    int32_t st;
    if (kind == RAW) {
        st = ev.set(event, name, type, size, data ? m32_h(data) : NULL);
    } else {
        uint64_t local[4] = { 0, 0, 0, 0 };
        unsigned long hsize;
        if (kind == PTR) {
            local[0] = (uint64_t)(uintptr_t)m32_host(m32_rd(data));
            hsize = 8;
        } else if (kind == CFIDX) {
            local[0] = (uint64_t)(int64_t)(int32_t)m32_rd(data);
            hsize = 8;
        } else if (kind == CGF) {
            for (int i = 0; i < n; i++) {
                float f;
                memcpy(&f, m32_h(data + 4 * (uint32_t)i), 4);
                double d = f;
                memcpy((uint8_t *)local + 8 * i, &d, 8);
            }
            hsize = 8 * (unsigned long)n;
        } else {
            uint8_t *h = (uint8_t *)local;
            memcpy(h, m32_h(data), 8);
            uint64_t ref = (uint64_t)(uintptr_t)m32_host(m32_rd(data + 8));
            memcpy(h + 8, &ref, 8);
            memcpy(h + 16, m32_h(data + 12), 2);
            hsize = 24;
        }
        st = ev.set(event, name, type, hsize, local);
    }
    m32_ret(cpu, (uint32_t)st, 0, 0);
    return OCERZ_STEP_OK;
}

/* ---- EventRecords ---- */

typedef struct __attribute__((packed)) {
    uint16_t what;
    uint64_t message;
    uint32_t when;
    int16_t v, h;
    uint16_t modifiers;
} HostEventRecord;

static void record_in(uint32_t g, HostEventRecord *r)
{
    uint8_t b[16];
    memcpy(b, m32_h(g), 16);
    uint32_t message;
    memcpy(&r->what, b, 2);
    memcpy(&message, b + 2, 4);
    r->message = message;
    memcpy(&r->when, b + 6, 4);
    memcpy(&r->v, b + 10, 2);
    memcpy(&r->h, b + 12, 2);
    memcpy(&r->modifiers, b + 14, 2);
}

static void record_out(const HostEventRecord *r, uint32_t g)
{
    uint8_t b[16];
    uint32_t message = (uint32_t)r->message;
    memcpy(b, &r->what, 2);
    memcpy(b + 2, &message, 4);
    memcpy(b + 6, &r->when, 4);
    memcpy(b + 10, &r->v, 2);
    memcpy(b + 12, &r->h, 2);
    memcpy(b + 14, &r->modifiers, 2);
    memcpy(m32_h(g), b, 16);
}

static void *carbon_fn(const char *name)
{
    static void *hi;
    if (!hi)
        hi = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon", RTLD_LAZY | RTLD_LOCAL);
    return hi ? dlsym(hi, name) : NULL;
}

static int ret_value(OcerzCPU *cpu, uint32_t v)
{
    m32_ret(cpu, v, 0, 0);
    return OCERZ_STEP_OK;
}

/* WaitNextEvent(EventMask, EventRecord *, UInt32 sleep, RgnHandle mouseRgn) -> Boolean */
static int sp_WaitNextEvent(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    static unsigned char (*fn)(uint16_t, HostEventRecord *, uint32_t, void *);
    if (!fn)
        *(void **)&fn = carbon_fn("WaitNextEvent");
    HostEventRecord r;
    memset(&r, 0, sizeof r);
    unsigned char got = fn ? fn((uint16_t)m32_arg(cpu, 0), &r, m32_arg(cpu, 2), m32_host(m32_arg(cpu, 3))) : 0;
    if (m32_arg(cpu, 1))
        record_out(&r, m32_arg(cpu, 1));
    return ret_value(cpu, got);
}

/* GetNextEvent(EventMask, EventRecord *) -> Boolean: WaitNextEvent that does not sleep */
static int sp_GetNextEvent(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    static unsigned char (*fn)(uint16_t, HostEventRecord *, uint32_t, void *);
    if (!fn)
        *(void **)&fn = carbon_fn("WaitNextEvent");
    HostEventRecord r;
    memset(&r, 0, sizeof r);
    unsigned char got = fn ? fn((uint16_t)m32_arg(cpu, 0), &r, 0, NULL) : 0;
    if (m32_arg(cpu, 1))
        record_out(&r, m32_arg(cpu, 1));
    return ret_value(cpu, got);
}

/* EventAvail(EventMask, EventRecord *) -> Boolean */
static int sp_EventAvail(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    static unsigned char (*fn)(uint16_t, HostEventRecord *);
    if (!fn)
        *(void **)&fn = carbon_fn("EventAvail");
    HostEventRecord r;
    memset(&r, 0, sizeof r);
    unsigned char got = fn ? fn((uint16_t)m32_arg(cpu, 0), &r) : 0;
    if (m32_arg(cpu, 1))
        record_out(&r, m32_arg(cpu, 1));
    return ret_value(cpu, got);
}

/* ConvertEventRefToEventRecord(EventRef, EventRecord *) -> Boolean */
static int sp_ConvertEventRefToEventRecord(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    static unsigned char (*fn)(void *, HostEventRecord *);
    if (!fn)
        *(void **)&fn = carbon_fn("ConvertEventRefToEventRecord");
    HostEventRecord r;
    memset(&r, 0, sizeof r);
    unsigned char ok = fn ? fn(m32_host(m32_arg(cpu, 0)), &r) : 0;
    if (m32_arg(cpu, 1))
        record_out(&r, m32_arg(cpu, 1));
    return ret_value(cpu, ok);
}

/* AEProcessAppleEvent(const EventRecord *) -> OSErr */
static int sp_AEProcessAppleEvent(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    static int16_t (*fn)(const HostEventRecord *);
    if (!fn)
        *(void **)&fn = carbon_fn("AEProcessAppleEvent");
    HostEventRecord r;
    memset(&r, 0, sizeof r);
    if (m32_arg(cpu, 0))
        record_in(m32_arg(cpu, 0), &r);
    return ret_value(cpu, fn ? (uint32_t)(int32_t)fn(&r) : (uint32_t)-50);
}

/* ---- the Text Encoding Converter ---- */

typedef int32_t (*TecConvert)(void *, unsigned long, const void *, uint32_t, unsigned long, const unsigned long *,
                              unsigned long *, unsigned long *, unsigned long, unsigned long *, unsigned long *, void *);

/* (info, ByteCount srcLen, src, OptionBits, ItemCount offsets, const ByteOffset in[], ItemCount *outOffsets,
 * ByteOffset out[], ByteCount outLen, ByteCount *read, ByteCount *written, out) */
static int tec_convert(OcerzCPU *cpu, const char *name)
{
    TecConvert fn = (TecConvert)carbon_fn(name);
    uint32_t n = m32_arg(cpu, 4);
    if (!fn || n > 65536)
        return ret_value(cpu, (uint32_t)-50);
    unsigned long *in = n ? calloc(n, sizeof *in) : NULL, *out = n ? calloc(n, sizeof *out) : NULL;
    for (uint32_t i = 0; in && m32_arg(cpu, 5) && i < n; i++)
        in[i] = m32_rd(m32_arg(cpu, 5) + 4 * i);
    unsigned long out_count = 0, read = 0, written = 0;
    uint32_t src = m32_arg(cpu, 2), dst = m32_arg(cpu, 11);
    int32_t st = fn(m32_host(m32_arg(cpu, 0)), m32_arg(cpu, 1), src ? m32_h(src) : NULL, m32_arg(cpu, 3), n,
                    m32_arg(cpu, 5) ? in : NULL, &out_count, m32_arg(cpu, 7) ? out : NULL, m32_arg(cpu, 8), &read,
                    &written, dst ? m32_h(dst) : NULL);
    if (m32_arg(cpu, 6))
        m32_wr(m32_arg(cpu, 6), (uint32_t)out_count);
    for (uint32_t i = 0; out && m32_arg(cpu, 7) && i < out_count && i < n; i++)
        m32_wr(m32_arg(cpu, 7) + 4 * i, (uint32_t)out[i]);
    if (m32_arg(cpu, 9))
        m32_wr(m32_arg(cpu, 9), (uint32_t)read);
    if (m32_arg(cpu, 10))
        m32_wr(m32_arg(cpu, 10), (uint32_t)written);
    free(in);
    free(out);
    return ret_value(cpu, (uint32_t)st);
}
static int sp_ConvertFromUnicodeToText(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; return tec_convert(cpu, "ConvertFromUnicodeToText"); }
static int sp_ConvertFromTextToUnicode(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; return tec_convert(cpu, "ConvertFromTextToUnicode"); }

/* ConvertFromUnicodeToPString(UnicodeToTextInfo, ByteCount, ConstUniCharPtr, Str255) */
static int sp_ConvertFromUnicodeToPString(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    int32_t (*fn)(void *, unsigned long, const void *, void *) = (int32_t (*)(void *, unsigned long, const void *, void *))carbon_fn("ConvertFromUnicodeToPString");
    uint32_t src = m32_arg(cpu, 2), dst = m32_arg(cpu, 3);
    return ret_value(cpu, fn ? (uint32_t)fn(m32_host(m32_arg(cpu, 0)), m32_arg(cpu, 1), src ? m32_h(src) : NULL,
                                            dst ? m32_h(dst) : NULL) : (uint32_t)-50);
}

/* ConvertFromPStringToUnicode(TextToUnicodeInfo, ConstStr255Param, ByteCount outLen, ByteCount *written, UniChar *) */
static int sp_ConvertFromPStringToUnicode(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    int32_t (*fn)(void *, const void *, unsigned long, unsigned long *, void *) =
        (int32_t (*)(void *, const void *, unsigned long, unsigned long *, void *))carbon_fn("ConvertFromPStringToUnicode");
    unsigned long written = 0;
    uint32_t src = m32_arg(cpu, 1), dst = m32_arg(cpu, 4);
    int32_t st = fn ? fn(m32_host(m32_arg(cpu, 0)), src ? m32_h(src) : NULL, m32_arg(cpu, 2), &written,
                         dst ? m32_h(dst) : NULL) : -50;
    if (m32_arg(cpu, 3))
        m32_wr(m32_arg(cpu, 3), (uint32_t)written);
    return ret_value(cpu, (uint32_t)st);
}

/* _LSCopyDefaultSchemeHandlerURL(CFStringRef scheme, CFURLRef *app): LaunchServices' private call, the default
 * handler of a URL scheme, which Firefox 3.6 imports to ask whether it is the default browser.  m32 had no
 * declaration for it, answered 0, noErr, and left *app as it was: Firefox handed that garbage to CFBundleCreate.  It is
 * now LSCopyDefaultApplicationURLForURL on "scheme:", or kLSApplicationNotFoundErr (-10814) with *app NULL. */
static int sp_LSCopyDefaultSchemeHandlerURL(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    static void *(*with_format)(void *, void *, void *, ...), *(*url_with_string)(void *, void *, void *);
    static void *(*app_for_url)(void *, uint32_t, void **), (*cf_release)(void *);
    static void *(*cfstr)(void *, const char *, uint32_t);
    if (!with_format) {
        url_with_string = (void *(*)(void *, void *, void *))dlsym(RTLD_DEFAULT, "CFURLCreateWithString");
        app_for_url = (void *(*)(void *, uint32_t, void **))carbon_fn("LSCopyDefaultApplicationURLForURL");
        cf_release = (void (*)(void *))dlsym(RTLD_DEFAULT, "CFRelease");
        cfstr = (void *(*)(void *, const char *, uint32_t))dlsym(RTLD_DEFAULT, "CFStringCreateWithCString");
        with_format = (void *(*)(void *, void *, void *, ...))dlsym(RTLD_DEFAULT, "CFStringCreateWithFormat");
    }
    void *scheme = m32_host(m32_arg(cpu, 0)), *app = NULL;
    if (scheme && with_format && url_with_string && app_for_url && cf_release && cfstr) {
        void *fmt = cfstr(NULL, "%@:", 0x08000100);
        void *s = fmt ? with_format(NULL, NULL, fmt, scheme) : NULL;
        void *url = s ? url_with_string(NULL, s, NULL) : NULL;
        app = url ? app_for_url(url, 0xFFFFFFFF, NULL) : NULL;   /* kLSRolesAll */
        if (url)
            cf_release(url);
        if (s)
            cf_release(s);
        if (fmt)
            cf_release(fmt);
    }
    if (m32_arg(cpu, 1))
        m32_wr(m32_arg(cpu, 1), m32_handle(app));
    return ret_value(cpu, app ? 0 : (uint32_t)-10814);
}

/* GetScriptManagerVariable(short selector) -> long: smKCHRCache (38) is the address of the keyboard's KCHR, a pointer
 * the long holds, which the i386 int cut to 32 bits; SDL 1.2 handed it to KeyTranslate, which faulted in HIToolbox
 * (AssaultCube).  It answers 0, no KCHR, as on a Mac with Unicode layouts alone, and callers use their own key
 * tables; the other selectors are numbers and pass through. */
static int sp_GetScriptManagerVariable(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    long (*fn)(short) = (long (*)(short))carbon_fn("GetScriptManagerVariable");
    short sel = (short)m32_arg(cpu, 0);
    return ret_value(cpu, sel == 38 || !fn ? 0 : (uint32_t)fn(sel));
}

const M32SpecialEntry m32_carbon_specials[] = {
    { "__LSCopyDefaultSchemeHandlerURL", sp_LSCopyDefaultSchemeHandlerURL },
    { "_GetScriptManagerVariable", sp_GetScriptManagerVariable },
    { "_ConvertFromUnicodeToText", sp_ConvertFromUnicodeToText },
    { "_ConvertFromTextToUnicode", sp_ConvertFromTextToUnicode },
    { "_ConvertFromUnicodeToPString", sp_ConvertFromUnicodeToPString },
    { "_ConvertFromPStringToUnicode", sp_ConvertFromPStringToUnicode },
    { "_GetEventParameter", sp_GetEventParameter }, { "_SetEventParameter", sp_SetEventParameter },
    { "_WaitNextEvent", sp_WaitNextEvent }, { "_GetNextEvent", sp_GetNextEvent }, { "_EventAvail", sp_EventAvail },
    { "_ConvertEventRefToEventRecord", sp_ConvertEventRefToEventRecord },
    { "_AEProcessAppleEvent", sp_AEProcessAppleEvent },
    { NULL, NULL }
};

M32Special m32_carbon_special(const char *symbol)
{
    size_t n = strlen(symbol);
    if (n < 8 || strcmp(symbol + n - 3, "UPP") || getenv("OCERZ_NO_M32_UPP"))
        return NULL;
    if (!strncmp(symbol, "_New", 4))
        return upp_new;
    if (!strncmp(symbol, "_Dispose", 8))
        return upp_dispose;
    return NULL;
}

void *m32_carbon_wrap(const char *symbol, void *fn)
{
    if (!fn || getenv("OCERZ_NO_M32_CARBON_WRAP"))
        return fn;
    if (!strcmp(symbol, "CreateNewWindow")) {
        g_create_new_window = (int32_t (*)(uint32_t, uint32_t, const void *, void **))fn;
        return (void *)create_new_window;
    }
    if (!strcmp(symbol, "DisposeWindow")) {
        g_dispose_window = (void (*)(void *))fn;
        return (void *)dispose_window;
    }
    return fn;
}

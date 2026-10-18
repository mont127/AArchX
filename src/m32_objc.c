/*
 * The Objective-C 1 runtime of i386 guests (include/ocerz/m32_objc.h).
 *
 * Guest classes stay the structures the guest's compiler wrote (fragile ivars, 4-byte fields); their method lists
 * are searched here.  A host class is stood for by a *proxy* old_class in the window, flagged OC_CLS_HOST.  A guest
 * class whose chain reaches a proxy is *paired* with a host class made at load (objc_allocateClassPair), holding
 * every guest method as a callback slot into the guest IMP; its instances are pairs, a host object and a guest twin
 * of the guest instance_size, aliased both ways.  objc_msgSend enters a guest IMP directly, on the caller's own
 * frame; anything that reaches a proxy or a host object is sent to the host with the arguments converted by the
 * method's two encodings (i386 from runtime/apis32's .objc32 files or the guest's own method list, host from the
 * host runtime).
 */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ocerz/abi.h"
#include "ocerz/bridge.h"
#include "ocerz/interp.h"
#include "ocerz/mem.h"
#include "ocerz/m32_objc.h"
#include "ocerz/m32_objcrt.h"
#include "ocerz/m32_image.h"
#include "ocerz/m32_sig.h"
#include "ocerz/types.h"
#include "ocerz/vm.h"

/* ---- a small string/pointer hash map ---- */

typedef struct Ent { uint64_t k; uint64_t v; } Ent;
typedef struct Map { Ent *e; uint32_t cap, n; } Map;

static uint64_t mix(uint64_t k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33; return k; }
static uint64_t hash_str(const char *s) { uint64_t h = 1469598103934665603ull; while (*s) h = (h ^ (uint8_t)*s++) * 1099511628211ull; return h | 1; }

static int map_get(const Map *m, uint64_t k, uint64_t *v)
{
    if (!m->cap)
        return 0;
    for (uint32_t i = (uint32_t)mix(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (!m->e[i].k)
            return 0;
        if (m->e[i].k == k) {
            *v = m->e[i].v;
            return 1;
        }
    }
}

static void map_put(Map *m, uint64_t k, uint64_t v)
{
    if ((m->n + 1) * 2 > m->cap) {
        Map g = { calloc(m->cap ? m->cap * 2 : 1024, sizeof(Ent)), m->cap ? m->cap * 2 : 1024, 0 };
        for (uint32_t i = 0; i < m->cap; i++)
            if (m->e[i].k)
                map_put(&g, m->e[i].k, m->e[i].v);
        free(m->e);
        *m = g;
    }
    uint32_t i = (uint32_t)mix(k) & (m->cap - 1);
    while (m->e[i].k && m->e[i].k != k)
        i = (i + 1) & (m->cap - 1);
    if (!m->e[i].k)
        m->n++;
    m->e[i].k = k;
    m->e[i].v = v;
}

static pthread_mutex_t g_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&g_lock)
#define UNLOCK() pthread_mutex_unlock(&g_lock)

/* ---- selectors ---- */

static Map g_sel_by_name;    /* hash(name) -> guest SEL (collisions checked by content) */
static Map g_sel_host;       /* guest SEL -> host SEL */
static Map g_sel_guest;      /* host SEL -> guest SEL */

static uint32_t sel_intern(const char *name, uint32_t candidate)
{
    uint64_t h = hash_str(name), v;
    for (;; h += 2) {
        if (!map_get(&g_sel_by_name, h, &v))
            break;
        if (!strcmp((const char *)m32_h((uint32_t)v), name))
            return (uint32_t)v;
    }
    uint32_t g = candidate ? candidate : m32_cstring(name);
    map_put(&g_sel_by_name, h, g);
    SEL hs = sel_registerName(name);
    map_put(&g_sel_host, g, (uint64_t)(uintptr_t)hs);
    map_put(&g_sel_guest, (uint64_t)(uintptr_t)hs, g);
    return g;
}

uint32_t m32_sel_guest(void *host_sel)
{
    if (!host_sel)
        return 0;
    uint64_t v;
    LOCK();
    uint32_t g = map_get(&g_sel_guest, (uint64_t)(uintptr_t)host_sel, &v) ? (uint32_t)v : sel_intern(sel_getName(host_sel), 0);
    UNLOCK();
    return g;
}

void *m32_sel_host(uint32_t g)
{
    if (!g)
        return NULL;
    uint64_t v;
    LOCK();
    void *h = map_get(&g_sel_host, g, &v) ? (void *)(uintptr_t)v : NULL;
    if (!h)   /* a selector string the guest made itself */
        h = (void *)m32_sel_host(sel_intern((const char *)m32_h(g), 0));
    UNLOCK();
    return h;
}

/* ---- classes ---- */

static Map g_guest_classes;   /* guest class or metaclass address -> 1 (guest) */
static Map g_class_by_name;   /* hash(name) -> guest class (guest-defined only) */
static Map g_proxy_of;        /* host Class -> proxy */
static Map g_host_of;         /* proxy or guest class/metaclass -> host Class */
static Map g_pair_guest;      /* paired host Class -> guest class (and host metaclass -> guest metaclass) */
static Map g_methods;         /* guest class -> head of our method-list chain (MList*) */
static Map g_cache;           /* hash(class, sel) -> IMP (0 cached as "none": value 1) */
static Map g_initialized;     /* guest class -> 1 */
static Map g_imp_types;       /* host IMP m32 installed -> the guest method's types */

typedef struct MList { struct MList *next; uint32_t list; } MList;

static uint32_t rd(uint32_t g) { return m32_rd(g); }
static int is_guest_class(uint32_t c) { uint64_t v; return c && map_get(&g_guest_classes, c, &v); }
static int is_proxy(uint32_t c) { uint64_t v; return c && !is_guest_class(c) && map_get(&g_host_of, c, &v); }
static int is_meta(uint32_t c) { return (rd(c + OC_INFO) & OC_CLS_META) != 0; }
static const char *cname(uint32_t c) { uint32_t n = rd(c + OC_NAME); return n ? (const char *)m32_h(n) : "?"; }

static uint32_t proxy_for(Class h)
{
    if (!h)
        return 0;
    uint64_t v;
    if (map_get(&g_proxy_of, (uint64_t)(uintptr_t)h, &v))
        return (uint32_t)v;
    int meta = class_isMetaClass(h);
    uint32_t p = m32_static_alloc(OC_SIZE, 16);
    map_put(&g_proxy_of, (uint64_t)(uintptr_t)h, p);
    map_put(&g_host_of, p, (uint64_t)(uintptr_t)h);
    m32_wr(p + OC_NAME, m32_cstring(class_getName(h)));
    m32_wr(p + OC_INFO, (meta ? OC_CLS_META : OC_CLS_CLASS) | OC_CLS_HOST);
    m32_wr(p + OC_ISIZE, (uint32_t)class_getInstanceSize(h));
    m32_wr(p + OC_SUPER, proxy_for(class_getSuperclass(h)));
    m32_wr(p + OC_ISA, meta ? 0 : proxy_for(object_getClass((id)h)));
    return p;
}

static uint32_t class_by_name(const char *name)
{
    uint64_t h = hash_str(name), v;
    for (;; h += 2) {
        if (!map_get(&g_class_by_name, h, &v))
            break;
        if (!strcmp(cname((uint32_t)v), name))
            return (uint32_t)v;
    }
    Class hc = objc_getClass(name);
    return hc ? proxy_for(hc) : 0;
}

static void ensure_pair(uint32_t g);

void *m32_class_host(uint32_t g)
{
    if (!g)
        return NULL;
    uint64_t v;
    LOCK();
    if (!map_get(&g_host_of, g, &v) && is_guest_class(g)) {
        ensure_pair(is_meta(g) ? 0 : g);
        map_get(&g_host_of, g, &v);
    }
    UNLOCK();
    return map_get(&g_host_of, g, &v) ? (void *)(uintptr_t)v : NULL;
}

uint32_t m32_class_guest(void *h)
{
    if (!h)
        return 0;
    uint64_t v;
    LOCK();
    uint32_t g = map_get(&g_pair_guest, (uint64_t)(uintptr_t)h, &v) ? (uint32_t)v : proxy_for(h);
    UNLOCK();
    return g;
}

/* ---- objects ---- */

static uint32_t guest_class_of(uint32_t obj)
{
    if (m32_is_handle(obj))
        return 0;
    uint32_t isa = rd(obj);
    return is_guest_class(isa) ? isa : 0;
}

void *m32_objc_to_host(uint32_t g)
{
    if (!g)
        return NULL;
    if (m32_is_handle(g))
        return m32_host(g);
    void *h = m32_host(g);
    if (h != m32_h(g))
        return h;                         /* an alias: a twin, a CFSTR, a data placeholder */
    if (is_guest_class(g) || is_proxy(g))
        return m32_class_host(g);
    if (m32_is_guest_block(g))            /* a block is an object too: -copy, -release, in collections */
        return m32_block_to_host(g, NULL, NULL);
    uint32_t gc = guest_class_of(g);
    if (gc) {                             /* a guest object with no twin yet: give it one */
        Class hc = m32_class_host(gc);
        id obj = hc ? class_createInstance(hc, 0) : nil;
        if (obj) {
            m32_alias(g, obj);
            return obj;
        }
    }
    return m32_h(g);                      /* a guest object the host only passes around */
}

/* A pair's twin lives as long as its host object.  Each twin m32 makes hangs a reaper on its host object as an
 * associated object; the host releases it as the object is destroyed, and the reaper takes the alias back and frees
 * the twin.  Kept, the alias outlived the object: the next object malloc placed at that address answered the dead
 * one's twin, isa and all.  Adium's AICachedUserIconSource -init releases a second instance and returns the shared
 * one; an AutoHyperlinks scanner allocated where that instance had been got its twin, ran the icon source's -init
 * and came back as the icon source (OCERZ_NO_M32_REAP=1 keeps every twin). */
typedef struct Reap { uint32_t guest; void *host; } Reap;
static Class g_reaper;
static char g_reap_key;

static void reaper_dealloc(id self, SEL cmd)
{
    Reap *r = (Reap *)object_getIndexedIvars(self);
    m32_unalias(r->guest, r->host);
    m32_free(r->guest);
    struct objc_super sup = { self, class_getSuperclass(g_reaper) };
    ((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, cmd);
}

static void make_reaper(void)
{
    Class c = objc_allocateClassPair(objc_getClass("NSObject"), "M32TwinReaper", 0);
    if (!c)
        return;
    class_addMethod(c, sel_registerName("dealloc"), (IMP)reaper_dealloc, "v@:");
    objc_registerClassPair(c);
    g_reaper = c;
}

static void reap_with(void *host, uint32_t twin)
{
    static int off = -1;
    if (off < 0)
        off = getenv("OCERZ_NO_M32_REAP") != NULL;
    if (off)
        return;
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, make_reaper);
    id r = g_reaper ? class_createInstance(g_reaper, sizeof(Reap)) : nil;
    if (!r)
        return;
    *(Reap *)object_getIndexedIvars(r) = (Reap){ twin, host };
    objc_setAssociatedObject((id)host, &g_reap_key, r, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    ((void (*)(id, SEL))M32_MSGSEND)(r, sel_registerName("release"));
}

uint32_t m32_objc_to_guest(void *h)
{
    if (!h)
        return 0;
    if (m32_in_window(h))
        return m32_g(h);
    uint32_t g = m32_handle_twin_lookup(h);
    if (g)
        return g;
    if (object_isClass(h))
        return m32_class_guest(h);
    Class hc = object_getClass(h);
    uint64_t v;
    LOCK();
    int paired = hc && map_get(&g_pair_guest, (uint64_t)(uintptr_t)hc, &v);
    UNLOCK();
    if (paired) {                         /* a host instance of a paired class: its guest twin */
        uint32_t gc = (uint32_t)v, size = rd(gc + OC_ISIZE);
        uint32_t twin = m32_calloc(1, size < 4 ? 4 : size);
        m32_wr(twin, gc);
        m32_alias(twin, h);
        reap_with(h, twin);
        return twin;
    }
    g = m32_handle(h);
    if (g && m32_is_handle(g) && !rd(g))  /* a handle's isa word: the proxy of its class */
        m32_wr(g, m32_class_guest(hc));
    return g;
}

/* ---- method lookup in guest classes ---- */

static uint32_t list_find(uint32_t list, uint32_t sel, uint32_t *types)
{
    uint32_t n = rd(list + 4);
    for (uint32_t i = 0; i < n && i < 65536; i++) {
        uint32_t m = list + 8 + 12 * i;
        if (rd(m) == sel) {
            if (types)
                *types = rd(m + 4);
            return rd(m + 8);
        }
    }
    return 0;
}

/* the guest IMP for sel starting at class c, or 0; *stop gets the proxy the walk reached */
static uint32_t lookup(uint32_t c, uint32_t sel, uint32_t *stop, uint32_t *types)
{
    *stop = 0;
    for (int guard = 0; c && guard < 64; guard++) {
        if (!is_guest_class(c)) {
            *stop = c;
            return 0;
        }
        uint64_t key = mix((uint64_t)c << 32 | sel), v;
        if (!types && map_get(&g_cache, key, &v) && v != 1)
            return (uint32_t)v;
        uint64_t head;
        if (map_get(&g_methods, c, &head))
            for (MList *l = (MList *)(uintptr_t)head; l; l = l->next) {
                uint32_t imp = list_find(l->list, sel, types);
                if (imp) {
                    map_put(&g_cache, key, imp);
                    return imp;
                }
            }
        c = rd(c + OC_SUPER);
    }
    return 0;
}

static void add_list(uint32_t cls, uint32_t list)
{
    if (!list)
        return;
    uint64_t head = 0;
    map_get(&g_methods, cls, &head);
    MList *l = malloc(sizeof *l);
    l->list = list;
    l->next = (MList *)(uintptr_t)head;   /* categories added later win, as in the runtime */
    map_put(&g_methods, cls, (uint64_t)(uintptr_t)l);
    free(g_cache.e);
    memset(&g_cache, 0, sizeof g_cache);
}

static void fix_list_sels(uint32_t list)
{
    uint32_t n = list ? rd(list + 4) : 0;
    for (uint32_t i = 0; i < n && i < 65536; i++) {
        uint32_t m = list + 8 + 12 * i, s = rd(m);
        if (s)
            m32_wr(m, sel_intern((const char *)m32_h(s), s));
    }
}

/* ---- pairing ---- */

static char *guest_types(uint32_t types)
{
    return types ? (char *)m32_h(types) : NULL;
}

/* host types for a guest method: the overridden host method's, else the guest's own with offsets dropped */
static const char *host_types_for(Class host_super_or_meta, SEL hs, const char *gtypes, char *buf, size_t n)
{
    Method m = host_super_or_meta ? class_getInstanceMethod(host_super_or_meta, hs) : NULL;
    if (m)
        return method_getTypeEncoding(m);
    (void)buf; (void)n;
    return gtypes ? gtypes : "v@:";   /* the runtime reads offsets fine; names may hold digits (_Select1st) */
}

static void add_host_methods(Class target, Class lookup_super, uint32_t list)
{
    uint32_t n = list ? rd(list + 4) : 0;
    for (uint32_t i = 0; i < n && i < 65536; i++) {
        uint32_t m = list + 8 + 12 * i, imp = rd(m + 8);
        const char *gt = guest_types(rd(m + 4));
        SEL hs = m32_sel_host(rd(m));
        char buf[512], gn[512], hn[512];
        const char *ht = host_types_for(lookup_super, hs, gt, buf, sizeof buf);
        if (!gt || m32_encoding_notation(gt, 1, gn, sizeof gn) || m32_encoding_notation(ht, 0, hn, sizeof hn)) {
            m32_log_once("a guest method has an encoding m32 cannot carry:", sel_getName(hs));
            continue;
        }
        if (strchr(gn, '(') && strchr(hn, '(') && strlen(strchr(gn, '(')) != strlen(strchr(hn, '('))) {
            char why[1200];
            snprintf(why, sizeof why, "%s guest %s host %s", sel_getName(hs), gt, ht);
            m32_log_once("guest and host encodings disagree:", why);
        }
        void *h = m32_callback(imp, gn, hn);
        if (h) {
            class_replaceMethod(target, hs, (IMP)h, ht);
            map_put(&g_imp_types, (uint64_t)(uintptr_t)h, rd(m + 4));   /* the guest's own encoding of it */
        }
    }
}

static uint32_t class_lists(uint32_t c, uint32_t *out, int max)
{
    uint64_t head;
    int n = 0;
    if (map_get(&g_methods, c, &head))
        for (MList *l = (MList *)(uintptr_t)head; l && n < max; l = l->next)
            out[n++] = l->list;
    return (uint32_t)n;
}

/* The image a paired class came from, for NSBundle's bundleForClass:, which asks class_getImageName: a class made at
 * run time has none, so Sparkle's window controllers looked for their nibs in the main bundle and stopped VLC 1.1
 * with "Invalid parameter not satisfying: windowNibPath".  The hook takes a lock of its own, which nothing holds
 * while calling into the runtime (OCERZ_NO_M32_IMAGE_NAME=1 leaves the hook out). */
static struct { Class cls; const char *path; } g_class_images[8192];
static int g_nclass_images;
static pthread_mutex_t g_class_image_lock = PTHREAD_MUTEX_INITIALIZER;
static objc_hook_getImageName g_prev_image_name;

static BOOL class_image_name(Class cls, const char **out)
{
    const char *path = NULL;
    pthread_mutex_lock(&g_class_image_lock);
    for (int i = 0; i < g_nclass_images && !path; i++)
        if (g_class_images[i].cls == cls)
            path = g_class_images[i].path;
    pthread_mutex_unlock(&g_class_image_lock);
    if (path) {
        *out = path;
        return YES;
    }
    return g_prev_image_name ? g_prev_image_name(cls, out) : NO;
}

static void remember_image(Class hc, uint32_t g)
{
    static int hooked;
    if (getenv("OCERZ_NO_M32_IMAGE_NAME"))
        return;
    const char *path = NULL;
    for (int i = 0; i < m32_nimages && !path; i++)
        if (m32_images[i] && g >= m32_images[i]->lo && g < m32_images[i]->hi)
            path = m32_images[i]->path;
    if (!path)
        return;
    pthread_mutex_lock(&g_class_image_lock);
    for (int k = 0; k < 2 && g_nclass_images < 8192; k++) {
        g_class_images[g_nclass_images].cls = k ? object_getClass((id)hc) : hc;
        g_class_images[g_nclass_images++].path = path;
    }
    int first = !hooked;
    hooked = 1;
    pthread_mutex_unlock(&g_class_image_lock);
    if (first)
        objc_setHook_getImageName(class_image_name, &g_prev_image_name);
}

/* Key-value coding reaches an instance variable directly when no accessor answers, and a paired host class has none
 * of the guest's: Sparkle's update prompt binds its nib to two BOOL ivars, and KVC raised valueForUndefinedKey:.
 * The pair answers valueForUndefinedKey: and setValue:forUndefinedKey: from the guest ivar named key or _key in the
 * guest twin, a number boxed or unboxed by its type, an object converted (and, set, retained, the old one released),
 * and the superclass's answer, which raises, for any other key (OCERZ_NO_M32_KVC=1 leaves them out, and the nib
 * outlets below). */
static uint32_t guest_ivar(uint32_t cls, const char *key, const char **type, int exact)
{
    char under[256];
    snprintf(under, sizeof under, exact ? "%s" : "_%s", key);
    for (uint32_t c = cls; c && is_guest_class(c); c = rd(c + OC_SUPER)) {
        uint32_t list = rd(c + OC_IVARS), n = list ? rd(list) : 0;
        for (uint32_t i = 0; i < n && i < 4096; i++) {
            uint32_t iv = list + 4 + 12 * i, name = rd(iv), t = rd(iv + 4);
            const char *nm = name ? (const char *)m32_h(name) : "";
            if (t && (!strcmp(nm, key) || !strcmp(nm, under))) {
                *type = (const char *)m32_h(t);
                return rd(iv + 8) + 1;   /* offset + 1: 0 is none */
            }
        }
    }
    return 0;
}

static id kvc_super(id self, SEL cmd, id a, id b, int set)
{
    struct objc_super sup = { self, class_getSuperclass(object_getClass(self)) };
    if (set)
        return ((id (*)(struct objc_super *, SEL, id, id))objc_msgSendSuper)(&sup, cmd, a, b);
    return ((id (*)(struct objc_super *, SEL, id))objc_msgSendSuper)(&sup, cmd, a);
}

static id kvc_number(const char *type, uint32_t at)
{
    Class num = objc_getClass("NSNumber");
    id (*mk_i)(Class, SEL, long long) = (id (*)(Class, SEL, long long))M32_MSGSEND;
    id (*mk_u)(Class, SEL, unsigned long long) = (id (*)(Class, SEL, unsigned long long))M32_MSGSEND;
    id (*mk_d)(Class, SEL, double) = (id (*)(Class, SEL, double))M32_MSGSEND;
    switch (type[0]) {
    case 'c': return ((id (*)(Class, SEL, char))M32_MSGSEND)(num, sel_registerName("numberWithChar:"), *(int8_t *)m32_h(at));
    case 'B': return ((id (*)(Class, SEL, BOOL))M32_MSGSEND)(num, sel_registerName("numberWithBool:"), *(uint8_t *)m32_h(at) != 0);
    case 'C': return mk_u(num, sel_registerName("numberWithUnsignedLongLong:"), *(uint8_t *)m32_h(at));
    case 's': return mk_i(num, sel_registerName("numberWithLongLong:"), *(int16_t *)m32_h(at));
    case 'S': return mk_u(num, sel_registerName("numberWithUnsignedLongLong:"), *(uint16_t *)m32_h(at));
    case 'i': case 'l': return mk_i(num, sel_registerName("numberWithLongLong:"), (int32_t)rd(at));
    case 'I': case 'L': return mk_u(num, sel_registerName("numberWithUnsignedLongLong:"), rd(at));
    case 'q': return mk_i(num, sel_registerName("numberWithLongLong:"), (long long)(rd(at) | (uint64_t)rd(at + 4) << 32));
    case 'Q': return mk_u(num, sel_registerName("numberWithUnsignedLongLong:"), rd(at) | (uint64_t)rd(at + 4) << 32);
    case 'f': { float f; memcpy(&f, m32_h(at), 4); return mk_d(num, sel_registerName("numberWithDouble:"), f); }
    case 'd': { double d; memcpy(&d, m32_h(at), 8); return mk_d(num, sel_registerName("numberWithDouble:"), d); }
    default: return NULL;
    }
}

static id kvc_get(id self, SEL cmd, id key)
{
    const char *(*utf8)(id, SEL) = (const char *(*)(id, SEL))M32_MSGSEND;
    const char *k = key ? utf8(key, sel_registerName("UTF8String")) : NULL, *type = NULL;
    uint32_t g = m32_objc_to_guest(self), off = k && g ? guest_ivar(rd(g + OC_ISA), k, &type, 0) : 0;
    if (!off)
        return kvc_super(self, cmd, key, NULL, 0);
    uint32_t at = g + off - 1;
    if (type[0] == '@')
        return (id)m32_objc_to_host(rd(at));
    id boxed = kvc_number(type, at);
    return boxed ? boxed : kvc_super(self, cmd, key, NULL, 0);
}

static void kvc_set(id self, SEL cmd, id value, id key)
{
    const char *(*utf8)(id, SEL) = (const char *(*)(id, SEL))M32_MSGSEND;
    const char *k = key ? utf8(key, sel_registerName("UTF8String")) : NULL, *type = NULL;
    uint32_t g = m32_objc_to_guest(self), off = k && g ? guest_ivar(rd(g + OC_ISA), k, &type, 0) : 0;
    if (!off) {
        kvc_super(self, cmd, value, key, 1);
        return;
    }
    uint32_t at = g + off - 1;
    id (*msg)(id, SEL) = (id (*)(id, SEL))M32_MSGSEND;
    if (type[0] == '@') {
        id old = (id)m32_objc_to_host(rd(at));
        if (value)
            msg(value, sel_registerName("retain"));
        m32_wr(at, m32_objc_to_guest(value));
        if (old)
            msg(old, sel_registerName("release"));
        return;
    }
    long long (*ll)(id, SEL) = (long long (*)(id, SEL))M32_MSGSEND;
    double (*dd)(id, SEL) = (double (*)(id, SEL))M32_MSGSEND;
    long long v = value ? ll(value, sel_registerName("longLongValue")) : 0;
    switch (type[0]) {
    case 'c': case 'C': case 'B': *(uint8_t *)m32_h(at) = (uint8_t)v; break;
    case 's': case 'S': *(uint16_t *)m32_h(at) = (uint16_t)v; break;
    case 'i': case 'I': case 'l': case 'L': m32_wr(at, (uint32_t)v); break;
    case 'q': case 'Q': m32_wr(at, (uint32_t)v); m32_wr(at + 4, (uint32_t)((uint64_t)v >> 32)); break;
    case 'f': { float f = value ? (float)dd(value, sel_registerName("doubleValue")) : 0; memcpy(m32_h(at), &f, 4); break; }
    case 'd': { double d = value ? dd(value, sel_registerName("doubleValue")) : 0; memcpy(m32_h(at), &d, 8); break; }
    default: kvc_super(self, cmd, value, key, 1); break;
    }
}

/* A nib outlet whose owner has no setter is set straight into the instance variable of that name, retained (what
 * AppKit's NSNibOutletConnector does), and a paired host class has none of the guest's: every such outlet of a guest
 * owner stayed nil.  Sparkle's update prompt shrinks its window by its moreInfoView's height, and with that nil the
 * prompt opened 0 high, a modal window no one could see.  The connector sets the guest twin's ivar itself when its
 * source is a pair without the setter. */
static void (*g_establish)(id, SEL);

static void outlet_establish(id self, SEL cmd)
{
    id (*msg)(id, SEL) = (id (*)(id, SEL))M32_MSGSEND;
    BOOL (*responds)(id, SEL, SEL) = (BOOL (*)(id, SEL, SEL))M32_MSGSEND;
    const char *(*utf8)(id, SEL) = (const char *(*)(id, SEL))M32_MSGSEND;
    id src = msg(self, sel_registerName("source")), dst = msg(self, sel_registerName("destination"));
    id label = msg(self, sel_registerName("label"));
    const char *k = label ? utf8(label, sel_registerName("UTF8String")) : NULL, *type = NULL;
    uint32_t g = src && k && *k ? m32_objc_to_guest(src) : 0, off = 0;
    if (g && !m32_is_handle(g) && guest_class_of(g) && is_guest_class(rd(g + OC_ISA))) {
        char setter[300];
        snprintf(setter, sizeof setter, "set%c%s:", k[0] >= 'a' && k[0] <= 'z' ? k[0] - 32 : k[0], k + 1);
        if (!responds(src, sel_registerName("respondsToSelector:"), sel_registerName(setter)))
            off = guest_ivar(rd(g + OC_ISA), k, &type, 1);
    }
    if (!off || type[0] != '@') {
        g_establish(self, cmd);
        return;
    }
    uint32_t at = g + off - 1;
    id old = (id)m32_objc_to_host(rd(at));
    if (dst)
        msg(dst, sel_registerName("retain"));
    m32_wr(at, m32_objc_to_guest(dst));
    if (old)
        msg(old, sel_registerName("release"));
}

static void hook_outlets(void)   /* once AppKit is loaded */
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    Class c = g_establish ? NULL : objc_getClass("NSNibOutletConnector");
    Method m = c ? class_getInstanceMethod(c, sel_registerName("establishConnection")) : NULL;
    if (m)
        g_establish = (void (*)(id, SEL))method_setImplementation(m, (IMP)outlet_establish);
    pthread_mutex_unlock(&lock);
}

static void ensure_pair(uint32_t g)
{
    uint64_t v;
    if (!g || map_get(&g_host_of, g, &v))
        return;
    uint32_t sup = rd(g + OC_SUPER);
    Class hsup = NULL;
    if (is_guest_class(sup)) {
        ensure_pair(sup);
        hsup = m32_class_host(sup);
    } else if (sup)
        hsup = m32_class_host(sup);
    if (!hsup) {
        m32_log_once("a guest root class (no NSObject ancestor) is not supported:", cname(g));
        return;
    }
    char name[300];
    snprintf(name, sizeof name, "%s", cname(g));
    Class hc = objc_allocateClassPair(hsup, name, 0);
    if (!hc) {   /* the host has a class by that name */
        snprintf(name, sizeof name, "M32_%s", cname(g));
        hc = objc_allocateClassPair(hsup, name, 0);
    }
    if (!hc)
        return;
    uint32_t lists[32];
    uint32_t n = class_lists(g, lists, 32);
    for (int i = (int)n - 1; i >= 0; i--)
        add_host_methods(hc, hsup, lists[i]);
    uint32_t meta = rd(g + OC_ISA);
    n = class_lists(meta, lists, 32);
    for (int i = (int)n - 1; i >= 0; i--)
        add_host_methods(object_getClass((id)hc), object_getClass((id)hsup), lists[i]);
    /* the host checks conformance itself (AppKit gives a text input context only to an NSTextInputClient; without
     * one, interpretKeyEvents: sends insertText: up the chain to NSBeep).  protocol_list: next, count, list[];
     * old_protocol: isa, name, ... - ponytail: protocols a category adopts are not added */
    for (uint32_t pl = rd(g + OC_PROTOCOLS); pl; pl = rd(pl))
        for (uint32_t i = 0, np = rd(pl + 4); i < np && i < 256; i++) {
            const char *pn = (const char *)m32_h(rd(rd(pl + 8 + 4 * i) + 4));
            Protocol *p = objc_getProtocol(pn);
            if (p)
                class_addProtocol(hc, p);
            else
                m32_log_once("a guest class adopts a protocol the host does not know:", pn);
        }
    if (!getenv("OCERZ_NO_M32_KVC")) {   /* class_addMethod leaves a guest class's own versions be */
        class_addMethod(hc, sel_registerName("valueForUndefinedKey:"), (IMP)kvc_get, "@@:@");
        class_addMethod(hc, sel_registerName("setValue:forUndefinedKey:"), (IMP)kvc_set, "v@:@@");
        if (!g_establish)
            hook_outlets();
    }
    objc_registerClassPair(hc);
    map_put(&g_host_of, g, (uint64_t)(uintptr_t)hc);
    map_put(&g_host_of, meta, (uint64_t)(uintptr_t)object_getClass((id)hc));
    map_put(&g_pair_guest, (uint64_t)(uintptr_t)hc, g);
    map_put(&g_pair_guest, (uint64_t)(uintptr_t)object_getClass((id)hc), meta);
    remember_image(hc, g);
}

/* ---- loading an image's __OBJC ---- */

typedef struct Loads { struct Loads *next; M32Image *img; int nc, ncat; uint32_t v[]; } Loads;
static Loads *g_loads;

/* An image's protocols (__OBJC,__protocol: old_protocol {isa, name, protocol_list, instance_methods, class_methods},
 * 20 bytes each) get host twins of the same name, adopting the same protocols, and each guest protocol is aliased to its
 * twin.  @protocol(X) in i386 code is the guest structure: handed to conformsToProtocol:, libobjc read it as its own
 * protocol_t and faulted (Adium), and a paired class could adopt only the protocols the host already had.  The host
 * one is used where it exists.  ponytail: method descriptions are not copied; conformance is what is checked. */
/* register made[i] after the protocols it adopts that this image makes too: the host refuses to add a protocol still
 * under construction.  A registered one is cleared from made. */
static void register_protocol(uint32_t base, Protocol **made, uint32_t n, uint32_t i, int depth)
{
    Protocol *p = made[i];
    if (!p || depth > 64)
        return;
    made[i] = NULL;   /* visited: a cycle ends here */
    for (uint32_t pl = rd(base + 20 * i + 8); pl; pl = rd(pl))
        for (uint32_t k = 0, np = rd(pl + 4); k < np && k < 256; k++) {
            uint32_t sub = rd(pl + 8 + 4 * k), nm = sub ? rd(sub + 4) : 0;
            const char *name = nm ? (const char *)m32_h(nm) : NULL;
            if (!name)
                continue;
            for (uint32_t j = 0; j < n; j++)   /* adopted from this image: registered first */
                if (made[j] && !strcmp(protocol_getName(made[j]), name))
                    register_protocol(base, made, n, j, depth + 1);
            Protocol *q = objc_getProtocol(name);
            if (q && q != p)
                protocol_addProtocol(p, q);
        }
    objc_registerProtocol(p);
}

static void host_protocols(M32Image *img, uint32_t slide)
{
    const M32Sect *ps = m32_section(img, "__OBJC", "__protocol");
    if (!ps)
        return;
    uint32_t n = ps->size / 20;
    Protocol **made = calloc(n ? n : 1, sizeof *made);
    for (uint32_t i = 0; made && i < n; i++) {   /* make them all first: they adopt each other in any order */
        uint32_t g = ps->addr + slide + 20 * i, nm = rd(g + 4);
        const char *name = nm ? (const char *)m32_h(nm) : NULL;
        if (!name || !*name)
            continue;
        Protocol *p = objc_getProtocol(name);
        if (!p && (p = objc_allocateProtocol(name)))
            made[i] = p;
        if (p)
            m32_alias(g, p);
    }
    for (uint32_t i = 0; made && i < n; i++)
        register_protocol(ps->addr + slide, made, n, i, 0);
    free(made);
}

void m32_objc_load(M32Image *img)
{
    const M32Sect *mods = m32_section(img, "__OBJC", "__module_info");
    const M32Sect *mrefs = m32_section(img, "__OBJC", "__message_refs");
    const M32Sect *crefs = m32_section(img, "__OBJC", "__cls_refs");
    if (!mods && !mrefs)
        return;
    uint32_t slide = (uint32_t)img->slide;
    LOCK();
    if (mrefs)
        for (uint32_t o = 0; o + 4 <= mrefs->size; o += 4) {
            uint32_t a = mrefs->addr + slide + o, s = rd(a);
            if (s)
                m32_wr(a, sel_intern((const char *)m32_h(s), s));
        }
    host_protocols(img, slide);
    uint32_t classes[4096], cats[1024];
    int nc = 0, ncat = 0;
    for (uint32_t o = 0; mods && o + 16 <= mods->size; o += 16) {
        uint32_t st = rd(mods->addr + slide + o + 12);
        if (!st)
            continue;
        uint32_t w = rd(st + 8);   /* uint16 cls_def_cnt, uint16 cat_def_cnt */
        uint32_t ncls = w & 0xffff, ncats = w >> 16;
        for (uint32_t i = 0; i < ncls && nc < 4096; i++)
            classes[nc++] = rd(st + 12 + 4 * i);
        for (uint32_t i = 0; i < ncats && ncat < 1024; i++)
            cats[ncat++] = rd(st + 12 + 4 * (ncls + i));
    }
    /* names first: supers may come later in the list or from other images */
    for (int i = 0; i < nc; i++) {
        uint32_t c = classes[i], meta = rd(c + OC_ISA);
        map_put(&g_guest_classes, c, 1);
        map_put(&g_guest_classes, meta, 1);
        uint64_t h = hash_str(cname(c));
        uint64_t v;
        while (map_get(&g_class_by_name, h, &v))
            h += 2;
        map_put(&g_class_by_name, h, c);
    }
    for (int i = 0; i < nc; i++) {
        uint32_t c = classes[i], meta = rd(c + OC_ISA);
        uint32_t supname = rd(c + OC_SUPER);
        uint32_t sup = supname ? class_by_name((const char *)m32_h(supname)) : 0;
        if (supname && !sup)
            m32_log_once("a superclass is missing:", (const char *)m32_h(supname));
        m32_wr(c + OC_SUPER, sup);
        m32_wr(meta + OC_SUPER, sup ? (is_guest_class(sup) ? rd(sup + OC_ISA) : proxy_for(object_getClass((id)m32_class_host(sup)))) : c);
        m32_wr(meta + OC_ISA, meta);   /* ponytail: the root metaclass chain is not modelled */
        uint32_t il = rd(c + OC_METHODS), cl = rd(meta + OC_METHODS);
        fix_list_sels(il);
        fix_list_sels(cl);
        add_list(c, il);
        add_list(meta, cl);
    }
    for (int i = 0; i < ncat; i++) {   /* old_category: name, class_name, instance, class, protocols, ... */
        uint32_t cat = cats[i], target = class_by_name((const char *)m32_h(rd(cat + 4)));
        uint32_t il = rd(cat + 8), cl = rd(cat + 12);
        fix_list_sels(il);
        fix_list_sels(cl);
        if (!target)
            continue;
        if (is_guest_class(target)) {
            add_list(target, il);
            add_list(rd(target + OC_ISA), cl);
        } else {                      /* a category on a host class: host methods calling the guest IMPs */
            Class hc = m32_class_host(target);
            add_host_methods(hc, hc, il);
            add_host_methods(object_getClass((id)hc), object_getClass((id)hc), cl);
        }
    }
    if (crefs)
        for (uint32_t o = 0; o + 4 <= crefs->size; o += 4) {
            uint32_t a = crefs->addr + slide + o, n = rd(a);
            if (n && !is_guest_class(n) && !is_proxy(n))
                m32_wr(a, class_by_name((const char *)m32_h(n)));
        }
    for (int i = 0; i < nc; i++)
        ensure_pair(classes[i]);
    Loads *ld = calloc(1, sizeof *ld + (size_t)(nc + ncat) * sizeof(uint32_t) * 2);
    ld->img = img;
    ld->nc = nc;
    ld->ncat = ncat;
    memcpy(ld->v, classes, (size_t)nc * 4);
    memcpy(ld->v + nc, cats, (size_t)ncat * 4);
    ld->next = g_loads;
    g_loads = ld;
    UNLOCK();
}

/* +load of an image's classes and categories, when its initializers run */
void m32_objc_run_loads(M32Image *img)
{
    LOCK();
    Loads *ld = g_loads, **pp = &g_loads;
    while (ld && ld->img != img) {
        pp = &ld->next;
        ld = ld->next;
    }
    if (ld)
        *pp = ld->next;
    UNLOCK();
    if (!ld)
        return;
    int nc = ld->nc, ncat = ld->ncat;
    uint32_t *classes = ld->v, *cats = ld->v + nc;
    /* superclasses first: the class list is in declaration order, which keeps supers first in practice */
    uint32_t load = sel_intern("load", 0);
    for (int i = 0; i < nc; i++) {
        uint32_t stop, imp = list_find(rd(rd(classes[i] + OC_ISA) + OC_METHODS), load, NULL);
        (void)stop;
        if (imp) {
            uint32_t args[2] = { classes[i], load };
            m32_call(ocerz_vm_process(), imp, args, 2, NULL, NULL);
        }
    }
    for (int i = 0; i < ncat; i++) {
        uint32_t imp = list_find(rd(cats[i] + 12), load, NULL);
        uint32_t target = class_by_name((const char *)m32_h(rd(cats[i] + 4)));
        if (imp && target) {
            uint32_t args[2] = { target, load };
            m32_call(ocerz_vm_process(), imp, args, 2, NULL, NULL);
        }
    }
    free(ld);
}

/* +initialize before a guest class's first message, superclass first */
static void initialize(struct OcerzVM *vm, uint32_t cls)
{
    uint64_t v;
    if (!is_guest_class(cls) || is_meta(cls) || map_get(&g_initialized, cls, &v))
        return;
    LOCK();
    int again = map_get(&g_initialized, cls, &v);
    if (!again)
        map_put(&g_initialized, cls, 1);
    UNLOCK();
    if (again)
        return;
    initialize(vm, rd(cls + OC_SUPER));
    uint32_t sel = sel_intern("initialize", 0);
    uint32_t imp = list_find(rd(rd(cls + OC_ISA) + OC_METHODS), sel, NULL);
    if (imp) {
        uint32_t args[2] = { cls, sel };
        m32_call(vm, imp, args, 2, NULL, NULL);
    }
}

/* ---- sending to the host ---- */

typedef struct SendKey { char *genc, *henc; M32Export *e; } SendKey;
static Map g_sends;   /* hash(genc|henc) -> M32Export* */

/* -[NSOpenGLContext setView:] and -update trap off the main thread in today's AppKit; the Intel Macs these games ran on
 * allowed them, and SDL 2 makes both from its render thread. They run on the main thread, which the guest's event loop
 * keeps serving. ponytail: only these two; another AppKit call that traps off the main thread joins on_main. */
/* OpenGL views: in a program linked with today's SDK, NSView's wantsBestResolutionOpenGLSurface defaults to YES and
 * the drawable has the window's pixels, twice its points on a Retina display; the i386 programs were linked when it
 * defaulted to NO, the drawable in points and the window server scaling it.  One that never sets it draws at its point
 * size into a drawable twice as big (Left 4 Dead 2's frame filled a quarter of the window), so a view an NSOpenGLContext
 * is set on gets NO unless the guest chose itself.  ponytail: 256 views remembered as chosen. */
static void *g_chose_hidpi[256];
static int g_nchose_hidpi;
static pthread_mutex_t g_hidpi_lock = PTHREAD_MUTEX_INITIALIZER;
static int chose_hidpi(void *view, int note)
{
    int found = 0;
    pthread_mutex_lock(&g_hidpi_lock);
    for (int i = 0; i < g_nchose_hidpi && !found; i++)
        found = g_chose_hidpi[i] == view;
    if (note && !found && g_nchose_hidpi < 256)
        g_chose_hidpi[g_nchose_hidpi++] = view;
    pthread_mutex_unlock(&g_hidpi_lock);
    return found;
}

static void legacy_drawable(void *view)
{
    if (view && !chose_hidpi(view, 0))
        ((void (*)(void *, SEL, signed char))M32_MSGSEND)(view, sel_registerName("setWantsBestResolutionOpenGLSurface:"), 0);
}
static int is_kind(Class cls, const char *name)
{
    Class want = objc_getClass(name);
    for (; cls && want; cls = class_getSuperclass(cls))
        if (cls == want)
            return 1;
    return 0;
}

typedef struct MainSend { void *fn, *self; SEL sel; void *arg; } MainSend;
static void main_send_run(void *p)
{
    MainSend *m = p;
    if (m->sel == sel_registerName("setView:"))   /* an NSOpenGLContext the guest attaches itself */
        legacy_drawable(m->arg);
    ((void (*)(void *, SEL, void *))m->fn)(m->self, m->sel, m->arg);
}
static void main_call(void *fn, void *self, SEL sel, void *arg)
{
    MainSend m = { fn, self, sel, arg };
    if (pthread_main_np())
        main_send_run(&m);
    else
        dispatch_sync_f(dispatch_get_main_queue(), &m, main_send_run);
}
static void main_send(void *self, SEL sel, void *arg) { main_call(M32_MSGSEND, self, sel, arg); }
static void main_send_super(void *sup, SEL sel, void *arg) { main_call(M32_MSGSENDSUPER, sup, sel, arg); }
static int on_main(Class cls, const char *sel)
{
    static Class gl;
    if (!gl)
        gl = objc_getClass("NSOpenGLContext");
    if (strcmp(sel, "setView:") && strcmp(sel, "update"))
        return 0;
    for (; cls; cls = class_getSuperclass(cls))
        if (cls == gl)
            return 1;
    return 0;
}

/* the messages whose NSInteger is one of AppKit's tags (class G in src/m32_cross.c): 1 their result, 2 their first
 * argument, removed */
static int tag_message(const char *sel)
{
    if (!strcmp(sel, "addTrackingRect:owner:userData:assumeInside:") || !strcmp(sel, "addToolTipRect:owner:userData:") ||
        !strcmp(sel, "trackingNumber"))
        return 1;
    return !strcmp(sel, "removeTrackingRect:") || !strcmp(sel, "removeToolTip:") ? 2 : 0;
}

static M32Export *send_export(const char *sel, const char *genc, const char *henc, int super, int main)
{
    char key[2100];
    int tag = tag_message(sel);
    snprintf(key, sizeof key, "%s|%s|%d|%s", genc, henc, super, main || tag || strchr(genc, '^') ? sel : "");
    uint64_t h = hash_str(key), v;
    LOCK();
    for (;; h += 2) {
        if (!map_get(&g_sends, h, &v))
            break;
        M32Export *e = (M32Export *)(uintptr_t)v;
        if (!strcmp(e->lib, key)) {
            UNLOCK();
            return e;
        }
    }
    char gn[1024], hn[1024];
    M32Export *e = NULL;
    if (!m32_encoding_notation(genc, 1, gn, sizeof gn) && !m32_encoding_notation(henc, 0, hn, sizeof hn)) {
        if (strchr(genc, '^'))
            m32_notation_pointers(sel, genc, henc, gn, sizeof gn, hn, sizeof hn);
        char *t = tag == 1 ? gn : tag == 2 && !strncmp(strchr(gn, '(') ? strchr(gn, '(') : "", "(@:", 3) ? strchr(gn, '(') + 3 : NULL;
        if (t && (*t == 'i' || *t == 'u'))
            *t = 'G';
        e = calloc(1, sizeof *e);
        e->lib = strdup(key);
        e->name = strdup(sel);
        e->kind = M32_EX_FN;
        e->gnote = strdup(gn);
        e->hnote = strdup(hn);
        e->host_fn = main ? (super ? (void *)main_send_super : (void *)main_send) : super ? M32_MSGSENDSUPER : M32_MSGSEND;
        map_put(&g_sends, h, (uint64_t)(uintptr_t)e);
    }
    UNLOCK();
    return e;
}

/* the i386 encoding when the header database has none: the host's with LP64 integers narrowed */
static const char *guess_guest(const char *henc, char *buf, size_t n)
{
    size_t o = 0;
    for (const char *p = henc; *p && o + 1 < n; p++)
        buf[o++] = *p == 'q' ? 'i' : *p == 'Q' ? 'I' : *p;
    buf[o] = 0;
    return buf;
}

static int ret_nil(OcerzCPU *cpu, int stret_pop)
{
    m32_ret(cpu, 0, 0, stret_pop);
    return OCERZ_STEP_OK;
}

/* countByEnumeratingWithState:objects:count: with the i386 NSFastEnumerationState {state, itemsPtr, mutationsPtr,
 * extra[5]} (32 bytes) standing for the host's 64-byte one, kept per guest state until the enumeration restarts */
typedef struct FastEnum { uint32_t guest; uint32_t items, items_cap, mutations; uint64_t host[8]; } FastEnum;
static FastEnum g_fe[64];

static int fast_enumeration(OcerzCPU *cpu, void *hself, Class super_cls, SEL hs)
{
    uint32_t gstate = m32_arg(cpu, 2), gbuf = m32_arg(cpu, 3), len = m32_arg(cpu, 4);
    FastEnum *fe = NULL;
    LOCK();
    for (int i = 0; i < 64 && !fe; i++)
        if (g_fe[i].guest == gstate)
            fe = &g_fe[i];
    for (int i = 0; i < 64 && !fe; i++)
        if (!g_fe[i].guest || i == 63)
            fe = &g_fe[i];
    if (fe->guest != gstate) {
        fe->guest = gstate;
        fe->items = fe->items_cap = 0;
        fe->mutations = m32_static_alloc(4, 4);
    }
    UNLOCK();
    if (m32_rd(gstate) == 0)   /* a new enumeration */
        memset(fe->host, 0, sizeof fe->host);
    id hbuf[64] = { 0 };
    unsigned long (*count)(id, SEL, void *, id *, unsigned long) = (unsigned long (*)(id, SEL, void *, id *, unsigned long))M32_MSGSEND;
    (void)super_cls;
    unsigned long n = count(hself, hs, fe->host, hbuf, len < 64 ? len : 64);
    id *items = (id *)(uintptr_t)fe->host[1];
    uint32_t dst = gbuf;
    if (n > len) {   /* the host answered with its own storage, larger than the guest's buffer */
        if (n > fe->items_cap) {
            fe->items = m32_malloc((uint32_t)n * 4);
            fe->items_cap = (uint32_t)n;
        }
        dst = fe->items;
    }
    for (unsigned long i = 0; i < n; i++)
        m32_wr(dst + 4 * (uint32_t)i, m32_objc_to_guest(items ? items[i] : hbuf[i]));
    unsigned long *mut = (unsigned long *)(uintptr_t)fe->host[2];
    m32_wr(fe->mutations, mut ? (uint32_t)*mut : 0);
    m32_wr(gstate, (uint32_t)fe->host[0]);
    m32_wr(gstate + 4, dst);
    m32_wr(gstate + 8, fe->mutations);
    for (int i = 0; i < 5; i++)
        m32_wr(gstate + 12 + 4 * i, (uint32_t)fe->host[3 + i]);
    m32_ret(cpu, (uint32_t)n, 0, 0);
    return OCERZ_STEP_OK;
}

/* A variadic method: the named arguments convert as usual, then either a format's arguments (the last named one is
 * the format NSString) or objects up to a nil, each in an 8-byte arm64 variadic stack slot. */
static int send_variadic(struct OcerzVM *vm, OcerzCPU *cpu, void *hself, Class super_cls, SEL hs, const char *name,
                         const char *genc, const char *henc)
{
    char gn[1024], hn[1024];
    M32Sig *g = NULL, *h = NULL;
    if (!m32_encoding_notation(genc, 1, gn, sizeof gn) && !m32_encoding_notation(henc, 0, hn, sizeof hn)) {
        g = m32_sig_parse(gn, 1);
        h = m32_sig_parse(hn, 0);
    }
    if (!g || !h || g->nargs != h->nargs || g->nargs < 2) {
        m32_log_once("a variadic message m32 cannot carry:", name);
        free(g); free(h);
        return ret_nil(cpu, 0);
    }
    uint32_t ap = (uint32_t)cpu->gpr[OCERZ_RSP] + 4;
    uint64_t x[8] = { 0 }, v[8] = { 0 }, slots[64];
    struct objc_super sup = { hself, super_cls };
    x[0] = super_cls ? (uint64_t)(uintptr_t)&sup : (uint64_t)(uintptr_t)hself;
    x[1] = (uint64_t)(uintptr_t)hs;
    ap += 8;
    int nx = 2;
    void *last = NULL;
    for (int i = 2; i < g->nargs && nx < 8; i++) {
        uint64_t raw = g->arg[i].size == 8 ? (m32_rd(ap) | (uint64_t)m32_rd(ap + 4) << 32) : m32_rd(ap);
        ap += g->arg[i].size == 8 ? 8 : 4;
        uint64_t hv = m32_scalar_in(g->arg[i].cls, h->arg[i].cls, raw, name, i);
        x[nx++] = hv;
        last = (void *)(uintptr_t)hv;
    }
    int ns = 0;
    if (strstr(name, "ormat")) {          /* the format is the last named argument */
        const char *(*utf8)(id, SEL) = (const char *(*)(id, SEL))M32_MSGSEND;
        const char *fmt = last ? utf8(last, sel_registerName("UTF8String")) : "";
        ns = m32_printf_slots(fmt, ap, slots, 64, name);
    } else {                              /* objects up to and including the terminating nil */
        for (uint32_t w; ns < 63 && (w = m32_rd(ap)); ap += 4)
            slots[ns++] = (uint64_t)(uintptr_t)m32_objc_to_host(w);
        slots[ns++] = 0;
    }
    free(g); free(h);
    if (ns < 0)
        return ret_nil(cpu, 0);
    uint64_t out_x[2] = { 0 }, out_v[4] = { 0 };
    struct OcerzBridgeFrame frame;
    ocerz_bridge_raise(&frame, "objc", name, hn, NULL);
    void *exc = NULL;
    m32_call_native_catching(super_cls ? M32_MSGSENDSUPER : M32_MSGSEND, x, v, slots, (uint64_t)ns * 8, NULL, out_x, out_v, &exc);
    ocerz_bridge_lower(&frame);
    if (exc)
        return m32_objc_guest_throw(cpu, m32_objc_to_guest(exc));
    char r = gn[0];
    uint32_t eax = r == '@' ? m32_objc_to_guest((void *)(uintptr_t)out_x[0]) : r == 'v' ? 0 : (uint32_t)out_x[0];
    m32_ret(cpu, eax, 0, 0);
    return OCERZ_STEP_OK;
}

/* A method whose last argument is a format's va_list (initWithFormat:arguments:, initWithFormat:locale:arguments:,
 * +raise:format:arguments:): the named arguments convert as usual, and the va_list, the address of the format's first
 * argument on the guest's stack, becomes a pointer to 8-byte arm64 slots, which is what an Apple arm64 va_list is.
 * The format is the argument whose part of the selector ends in "ormat". */
static int send_va_list(struct OcerzVM *vm, OcerzCPU *cpu, void *hself, Class super_cls, SEL hs, const char *name,
                        const char *genc, const char *henc)
{
    char gn[1024], hn[1024];
    M32Sig *g = NULL, *h = NULL;
    if (!m32_encoding_notation(genc, 1, gn, sizeof gn) && !m32_encoding_notation(henc, 0, hn, sizeof hn)) {
        g = m32_sig_parse(gn, 1);
        h = m32_sig_parse(hn, 0);
    }
    int fi = -1, part = 0;
    for (const char *p = name, *colon; (colon = strchr(p, ':')); p = colon + 1, part++)
        if (colon - p >= 5 && !strncmp(colon - 5, "ormat", 5))
            fi = 2 + part;
    if (!g || !h || g->nargs != h->nargs || g->nargs < 4 || fi < 2 || fi >= g->nargs - 1 || g->nargs > 8) {
        m32_log_once("a va_list message m32 cannot carry:", name);
        free(g); free(h);
        return ret_nil(cpu, 0);
    }
    uint32_t at = (uint32_t)cpu->gpr[OCERZ_RSP] + 12;
    uint64_t x[8] = { 0 }, v[8] = { 0 }, slots[64];
    struct objc_super sup = { hself, super_cls };
    x[0] = super_cls ? (uint64_t)(uintptr_t)&sup : (uint64_t)(uintptr_t)hself;
    x[1] = (uint64_t)(uintptr_t)hs;
    for (int i = 2; i < g->nargs - 1; i++) {
        uint64_t raw = g->arg[i].size == 8 ? (m32_rd(at) | (uint64_t)m32_rd(at + 4) << 32) : m32_rd(at);
        at += g->arg[i].size == 8 ? 8 : 4;
        x[i] = m32_scalar_in(g->arg[i].cls, h->arg[i].cls, raw, name, i);
    }
    const char *(*utf8)(id, SEL) = (const char *(*)(id, SEL))M32_MSGSEND;
    const char *fmt = x[fi] ? utf8((id)(uintptr_t)x[fi], sel_registerName("UTF8String")) : "";
    int ns = m32_printf_slots(fmt, m32_rd(at), slots, 64, name);
    int nargs = g->nargs;
    free(g); free(h);
    if (ns < 0)
        return ret_nil(cpu, 0);
    x[nargs - 1] = (uint64_t)(uintptr_t)slots;
    uint64_t out_x[2] = { 0 }, out_v[4] = { 0 };
    struct OcerzBridgeFrame frame;
    ocerz_bridge_raise(&frame, "objc", name, hn, NULL);
    void *exc = NULL;
    m32_call_native_catching(super_cls ? M32_MSGSENDSUPER : M32_MSGSEND, x, v, NULL, 0, NULL, out_x, out_v, &exc);
    ocerz_bridge_lower(&frame);
    if (exc)
        return m32_objc_guest_throw(cpu, m32_objc_to_guest(exc));
    char r = gn[0];
    uint32_t eax = r == '@' ? m32_objc_to_guest((void *)(uintptr_t)out_x[0]) : r == 'v' ? 0 : (uint32_t)out_x[0];
    m32_ret(cpu, eax, 0, 0);
    return OCERZ_STEP_OK;
}

/* Arrays of objects with their count last (arrayWithObjects:count:, dictionaryWithObjects:forKeys:count:,
 * getObjects:andKeys:count:): the guest's ids are 4 bytes and the host's 8, so each array crosses as a copy of host
 * ids, written back as guest ids when the method fills it. */
static int objects_send(struct OcerzVM *vm, OcerzCPU *cpu, Method m, M32Export *e)
{
    M32Sig *g = m32_sig_parse(e->gnote, 1);
    if (!g || g->nargs < 3 || (g->arg[g->nargs - 1].cls != 'u' && g->arg[g->nargs - 1].cls != 'i')) {
        free(g);
        return m32_cross(vm, cpu, e);
    }
    uint32_t esp = (uint32_t)cpu->gpr[OCERZ_RSP], at[8], old[8], buf[8], off = 4, count = 0;
    int out[8], n = 0;
    for (int i = 0; i < g->nargs; off += g->arg[i].size > 4 ? 8 : 4, i++) {
        char t[16];
        method_getArgumentType(m, (unsigned)i, t, sizeof t);
        if ((!strcmp(t, "r^@") || !strcmp(t, "^@")) && n < 8) {
            out[n] = t[0] == '^';
            at[n++] = esp + off;
        }
        count = m32_rd(esp + off);
    }
    free(g);
    if (count > (1u << 24))
        return m32_cross(vm, cpu, e);
    for (int a = 0; a < n; a++) {
        old[a] = m32_rd(at[a]);
        buf[a] = old[a] ? m32_malloc(count * 8 + 8) : 0;
        uint64_t *h = buf[a] ? (uint64_t *)m32_h(buf[a]) : NULL;
        for (uint32_t k = 0; h && k < count; k++)
            h[k] = (uint64_t)(uintptr_t)m32_objc_to_host(m32_rd(old[a] + 4 * k));
        m32_wr(at[a], buf[a]);
    }
    int r = m32_cross(vm, cpu, e);
    for (int a = 0; a < n; a++) {
        uint64_t *h = buf[a] ? (uint64_t *)m32_h(buf[a]) : NULL;
        for (uint32_t k = 0; h && out[a] && k < count; k++)
            m32_wr(old[a] + 4 * k, m32_objc_to_guest((void *)(uintptr_t)h[k]));
        m32_wr(at[a], old[a]);
        if (buf[a])
            m32_free(buf[a]);
    }
    return r;
}

/* self_at: the guest stack word holding the receiver (1, or 2 after a stret pointer); super: the objc_super */
/* The system font a 32-bit program asks AppKit for is Lucida Grande, the system font of the Macs it was written for
 * and still installed: today's is .AppleSystemUIFont, a family no font list carries, and Firefox 3.6, looking the
 * default font up in the list NSFontManager gave it, found no usable font and aborted.  +[NSFont systemFontOfSize:]
 * and its siblings answer Lucida Grande at the size asked, bold for boldSystemFontOfSize: (OCERZ_NO_M32_LUCIDA=1
 * leaves them to AppKit). */
static int system_font(OcerzCPU *cpu, void *hself, const char *name)
{
    static const char *const sels[] = { "systemFontOfSize:", "boldSystemFontOfSize:", "userFontOfSize:",
                                        "messageFontOfSize:", "labelFontOfSize:", "menuFontOfSize:",
                                        "menuBarFontOfSize:", "controlContentFontOfSize:", "titleBarFontOfSize:",
                                        "toolTipsFontOfSize:", "paletteFontOfSize:" };
    static int off = -1;
    if (off < 0)
        off = getenv("OCERZ_NO_M32_LUCIDA") != NULL;
    Class font_cls = objc_getClass("NSFont");
    if (off || hself != (void *)font_cls)
        return 0;
    size_t i = 0;
    while (i < sizeof sels / sizeof sels[0] && strcmp(sels[i], name))
        i++;
    if (i == sizeof sels / sizeof sels[0])
        return 0;
    float size;
    uint32_t w = rd((uint32_t)cpu->gpr[OCERZ_RSP] + 12);   /* self, _cmd, then the CGFloat size, a float on i386 */
    memcpy(&size, &w, 4);
    if (size <= 0)
        size = i == 4 ? 10 : 13;
    id (*make)(Class, SEL, id, double) = (id (*)(Class, SEL, id, double))M32_MSGSEND;
    id (*str)(Class, SEL, const char *) = (id (*)(Class, SEL, const char *))M32_MSGSEND;
    id fname = str(objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"),
                   i == 1 ? "LucidaGrande-Bold" : "LucidaGrande");
    id f = make(font_cls, sel_registerName("fontWithName:size:"), fname, size);
    if (!f)
        return 0;
    m32_ret(cpu, m32_objc_to_guest(f), 0, 0);
    return 1;
}

/* -[NSView getRectsBeingDrawn:(const NSRect **)rects count:(NSInteger *)count] hands back AppKit's own array, valid
 * while drawRect: runs: a host pointer to NSRects of doubles.  Crossed, the guest got that pointer cut to 32 bits, and
 * Firefox 3.6's drawRect: read its rectangles from there and faulted.  The rectangles are copied, as floats, into a
 * guest array kept per thread and grown as needed. */
static int rects_being_drawn(OcerzCPU *cpu, void *hself, Class super_cls, SEL hs, const char *name)
{
    if (strcmp(name, "getRectsBeingDrawn:count:"))
        return 0;
    static __thread uint32_t buf, cap;
    typedef struct { double x, y, w, h; } HostRect;   /* NSRect on arm64 */
    const HostRect *r = NULL;
    long n = 0;
    if (super_cls) {
        struct objc_super sup = { hself, super_cls };
        ((void (*)(struct objc_super *, SEL, const HostRect **, long *))objc_msgSendSuper)(&sup, hs, &r, &n);
    } else {
        ((void (*)(id, SEL, const HostRect **, long *))M32_MSGSEND)(hself, hs, &r, &n);
    }
    if (!r || n < 0 || n > 1 << 20)
        n = 0;
    if ((uint32_t)n > cap) {
        if (buf)
            m32_free(buf);
        buf = m32_malloc(16 * (uint32_t)n);
        cap = buf ? (uint32_t)n : 0;
        if (!buf)
            n = 0;
    }
    float *f = n ? m32_h(buf) : NULL;
    for (long i = 0; i < n; i++) {
        f[4 * i] = (float)r[i].x;
        f[4 * i + 1] = (float)r[i].y;
        f[4 * i + 2] = (float)r[i].w;
        f[4 * i + 3] = (float)r[i].h;
    }
    uint32_t esp = (uint32_t)cpu->gpr[OCERZ_RSP];   /* self, _cmd, rects, count */
    if (rd(esp + 12))
        m32_wr(rd(esp + 12), n ? buf : 0);
    if (rd(esp + 16))
        m32_wr(rd(esp + 16), (uint32_t)n);
    m32_ret(cpu, 0, 0, 0);
    return 1;
}

/* NSBitmapImageRep's planes, which a program writes its pixels into: initWithBitmapDataPlanes:... with NULL planes
 * had AppKit allocate them in host memory, and getBitmapDataPlanes: handed the guest those addresses (SDL 1.2 writes
 * its cursors that way; AssaultCube faulted on the first).  The planes now live in guest memory: the guest's own,
 * passed as host pointers (the guest's array holds 4-byte ones), or, for NULL, ones m32 allocates for the rep, and
 * getBitmapDataPlanes: answers their guest addresses.  ponytail: m32's planes are never freed (AppKit does not free
 * planes it was given); programs make a handful, for cursors and icons. */
static int bitmap_planes(OcerzCPU *cpu, void *hself, Class super_cls, SEL hs, const char *name)
{
    static const char init10[] = "initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:"
                                 "isPlanar:colorSpaceName:bytesPerRow:bitsPerPixel:";
    static const char init11[] = "initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:"
                                 "isPlanar:colorSpaceName:bitmapFormat:bytesPerRow:bitsPerPixel:";
    uint32_t esp = (uint32_t)cpu->gpr[OCERZ_RSP];   /* self, _cmd, then the arguments, a word each */
#define ARG(i) rd(esp + 12 + 4 * (i))
    if (!strcmp(name, "getBitmapDataPlanes:")) {
        unsigned char *planes[5] = { 0 };
        if (super_cls) {
            struct objc_super sup = { hself, super_cls };
            ((void (*)(struct objc_super *, SEL, unsigned char **))objc_msgSendSuper)(&sup, hs, planes);
        } else {
            ((void (*)(id, SEL, unsigned char **))M32_MSGSEND)(hself, hs, planes);
        }
        for (int i = 0; ARG(0) && i < 5; i++)
            m32_wr(ARG(0) + 4 * (uint32_t)i, m32_handle(planes[i]));
        m32_ret(cpu, 0, 0, 0);
        return 1;
    }
    int fmt = !strcmp(name, init11);
    if (!fmt && strcmp(name, init10))
        return 0;
    long w = (int32_t)ARG(1), h = (int32_t)ARG(2), bps = (int32_t)ARG(3), spp = (int32_t)ARG(4);
    BOOL alpha = (BOOL)(ARG(5) & 0xff), planar = (BOOL)(ARG(6) & 0xff);
    id space = m32_objc_to_host(ARG(7));
    unsigned long format = fmt ? ARG(8) : 0;
    long bpr = (int32_t)ARG(8 + fmt), bpp = (int32_t)ARG(9 + fmt);
    unsigned char *planes[5] = { 0 };
    if (ARG(0)) {
        for (int i = 0; i < 5; i++)
            planes[i] = rd(ARG(0) + 4 * (uint32_t)i) ? m32_h(rd(ARG(0) + 4 * (uint32_t)i)) : NULL;
    } else if (w > 0 && h > 0 && bps > 0 && spp > 0 && spp <= 5 && w <= 16384 && h <= 16384) {
        long pixel_bits = bpp ? bpp : bps * (planar ? 1 : spp);
        if (!bpr)
            bpr = (w * pixel_bits + 7) / 8;
        long n = planar ? spp : 1;
        uint32_t g = bpr > 0 ? m32_malloc((uint32_t)(n * bpr * h)) : 0;
        if (g) {
            memset(m32_h(g), 0, (size_t)(n * bpr * h));
            for (long i = 0; i < n; i++)
                planes[i] = m32_h(g + (uint32_t)(i * bpr * h));
        }
    }
    id r;
    if (fmt) {
        id (*f)(id, SEL, unsigned char **, long, long, long, long, BOOL, BOOL, id, unsigned long, long, long) =
            (id (*)(id, SEL, unsigned char **, long, long, long, long, BOOL, BOOL, id, unsigned long, long, long))M32_MSGSEND;
        r = f(hself, hs, planes, w, h, bps, spp, alpha, planar, space, format, bpr, bpp);
    } else {
        id (*f)(id, SEL, unsigned char **, long, long, long, long, BOOL, BOOL, id, long, long) =
            (id (*)(id, SEL, unsigned char **, long, long, long, long, BOOL, BOOL, id, long, long))M32_MSGSEND;
        r = f(hself, hs, planes, w, h, bps, spp, alpha, planar, space, bpr, bpp);
    }
#undef ARG
    m32_ret(cpu, m32_objc_to_guest(r), 0, 0);
    return 1;
}

/* NSArray's sortUsingFunction:context:, sortedArrayUsingFunction:context: and sortedArrayUsingFunction:context:hint:
 * take a C comparator, NSInteger (*)(id, id, void *), which a method encoding spells only as ^?.  Crossed as a pointer,
 * CoreFoundation's merge sort jumped into guest code (Adium sorting its contact list at launch).  The comparator
 * becomes a callback; the context passes through and comes back to it as the guest's. */
static int function_sort(OcerzCPU *cpu, void *hself, Class super_cls, SEL hs, const char *name)
{
    int sorted = !strcmp(name, "sortedArrayUsingFunction:context:"),
        hint = !strcmp(name, "sortedArrayUsingFunction:context:hint:");
    if (!sorted && !hint && strcmp(name, "sortUsingFunction:context:"))
        return 0;
    uint32_t esp = (uint32_t)cpu->gpr[OCERZ_RSP], fn = rd(esp + 12), ctx = rd(esp + 16);
    void *cmp = fn ? m32_callback(fn, "i(@@p)", "l(@@p)") : NULL;
    void *hctx = ctx ? m32_h(ctx) : NULL;   /* guest memory: the callback hands the comparator ctx back */
    id hint_data = hint ? m32_objc_to_host(rd(esp + 20)) : nil, r = nil;
    struct objc_super sup = { hself, super_cls };
    if (hint && super_cls)
        r = ((id (*)(struct objc_super *, SEL, void *, void *, id))objc_msgSendSuper)(&sup, hs, cmp, hctx, hint_data);
    else if (hint)
        r = ((id (*)(id, SEL, void *, void *, id))M32_MSGSEND)(hself, hs, cmp, hctx, hint_data);
    else if (super_cls)
        r = ((id (*)(struct objc_super *, SEL, void *, void *))objc_msgSendSuper)(&sup, hs, cmp, hctx);
    else
        r = ((id (*)(id, SEL, void *, void *))M32_MSGSEND)(hself, hs, cmp, hctx);
    m32_ret(cpu, sorted || hint ? m32_objc_to_guest(r) : 0, 0, 0);
    return 1;
}

static int host_send(struct OcerzVM *vm, OcerzCPU *cpu, void *hself, Class super_cls, uint32_t gsel, uint32_t gtypes, int is_super)
{
    SEL hs = m32_sel_host(gsel);
    const char *name = sel_getName(hs);
    if (!is_super && system_font(cpu, hself, name))
        return OCERZ_STEP_OK;
    if (rects_being_drawn(cpu, hself, is_super ? super_cls : NULL, hs, name))
        return OCERZ_STEP_OK;
    if (!is_super && bitmap_planes(cpu, hself, NULL, hs, name))
        return OCERZ_STEP_OK;
    if (function_sort(cpu, hself, is_super ? super_cls : NULL, hs, name))
        return OCERZ_STEP_OK;
    int is_class = object_isClass(hself);
    Class lookup_cls = super_cls ? super_cls : object_getClass(hself);
    Method m = class_getInstanceMethod(lookup_cls, hs);
    if (!m) {
        char what[300];
        snprintf(what, sizeof what, "%s%s", is_class ? "+" : "-", name);
        m32_log_once("unrecognized selector (returning 0):", what);
        return ret_nil(cpu, 0);
    }
    const char *henc = method_getTypeEncoding(m);
    const char *genc = gtypes ? (const char *)m32_h(gtypes) : NULL;
    uint64_t own;
    if (!genc && map_get(&g_imp_types, (uint64_t)(uintptr_t)method_getImplementation(m), &own))
        genc = (const char *)m32_h((uint32_t)own);
    if (!strcmp(name, "countByEnumeratingWithState:objects:count:"))
        return fast_enumeration(cpu, hself, super_cls, hs);
    if (!genc)
        genc = m32_objc_guest_encoding(is_class ? (Class)hself : class_isMetaClass(lookup_cls) ? (Class)hself : lookup_cls,
                                       name, is_class);
    char buf[1024];
    if (!genc) {
        m32_log_once("no i386 encoding in the headers, guessed from the host's:", name);
        genc = guess_guest(henc, buf, sizeof buf);
    }
    if (m32_objc_last_variadic() && !gtypes)
        return send_variadic(vm, cpu, is_super ? NULL : hself, super_cls, hs, name, genc, henc);
    if (!gtypes && strstr(name, "ormat:") && strlen(name) > 10 && !strcmp(name + strlen(name) - 10, "arguments:"))
        return send_va_list(vm, cpu, is_super ? NULL : hself, super_cls, hs, name, genc, henc);
    M32Export *e = send_export(name, genc, henc, is_super, on_main(lookup_cls, name));
    if (!e) {
        m32_log_once("a message m32 cannot carry (union, bitfield, long double):", name);
        return ret_nil(cpu, 0);
    }
    static __thread struct objc_super sup;
    if (is_super) {
        sup.receiver = hself;
        sup.super_class = super_cls;
        m32_cross_self = &sup;
    }
    if (strstr(henc, "^@") && strstr(name, "count:"))
        return objects_send(vm, cpu, m, e);
    if (!strcmp(name, "setWantsBestResolutionOpenGLSurface:"))
        chose_hidpi(hself, 1);
    uint32_t removed = tag_message(name) == 2 ? rd((uint32_t)cpu->gpr[OCERZ_RSP] + 12) : 0;
    int r = m32_cross(vm, cpu, e);
    if (removed)
        m32_tag_free(removed);
    if (!strcmp(name, "initWithFrame:pixelFormat:") && is_kind(lookup_cls, "NSOpenGLView"))   /* attaches its own */
        legacy_drawable(m32_objc_to_host((uint32_t)cpu->gpr[OCERZ_RAX]));
    return r;
}

/* the shared body of objc_msgSend and objc_msgSend_stret/_fpret: self at stack word self_at */
static int send(struct OcerzVM *vm, OcerzCPU *cpu, int self_at)
{
    uint32_t esp = (uint32_t)cpu->gpr[OCERZ_RSP];
    uint32_t self = rd(esp + 4 * self_at), sel = rd(esp + 4 * self_at + 4);
    int pop = self_at == 2 ? 4 : 0;
    if (!self) {
        if (self_at == 2)
            return ret_nil(cpu, pop);
        m32_ret(cpu, 0, 0, 0);
        cpu->ftop = (cpu->ftop - 1) & 7;   /* a nil receiver's float result is 0.0 */
        cpu->fpr[cpu->ftop] = 0.0;
        return OCERZ_STEP_OK;
    }
    static int log_objc = -1;
    if (log_objc < 0)
        log_objc = getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "objc");
    if (log_objc) {
        void *ho = m32_is_handle(self) ? m32_host(self) : NULL;   /* a host object: its class's name */
        fprintf(stderr, "ocerz: m32: send %s to %#x %s (isa %#x, guest-class %d, proxy %d, handle %d)\n",
                (const char *)m32_h(sel), self,
                ho ? object_getClassName((id)ho) : is_guest_class(self) || is_proxy(self) ? cname(self) : "",
                m32_is_handle(self) ? 0 : rd(self), is_guest_class(self), is_proxy(self), m32_is_handle(self));
    }
    uint32_t cls = 0;
    if (is_guest_class(self))
        cls = rd(self + OC_ISA);           /* a guest class: class methods */
    else if (!m32_is_handle(self))
        cls = guest_class_of(self);        /* a guest object, twin or not */
    if (cls) {
        initialize(vm, is_meta(cls) ? self : cls);
        uint32_t stop, imp = lookup(cls, sel, &stop, NULL);
        if (imp) {
            cpu->rip = imp;                /* the guest method, on this very frame */
            return OCERZ_STEP_OK;
        }
    }
    void *h = m32_objc_to_host(self);
    /* OCERZ_M32_SENDLOG=<selector part>[|<part>...]: the matching host sends, their first argument words and their result, then
     * every message the objects they return receive (who retains and releases them) */
    static const char *sendlog = (const char *)1;
    static uint32_t watched[16];
    static int nwatched;
    if (sendlog == (const char *)1)
        sendlog = getenv("OCERZ_M32_SENDLOG");
    int watch = 0;
    for (int i = 0; sendlog && i < nwatched; i++)
        watch |= watched[i] == self;
    int hit = watch;
    for (const char *t = sendlog; t && *t && !hit; t = strchr(t, '|') ? strchr(t, '|') + 1 : "") {
        char tok[128];
        snprintf(tok, sizeof tok, "%.*s", (int)(strchr(t, '|') ? strchr(t, '|') - t : (long)strlen(t)), t);
        hit = tok[0] && strstr((const char *)m32_h(sel), tok) != NULL;
    }
    if (hit) {
        uint32_t esp = (uint32_t)cpu->gpr[OCERZ_RSP];
        char what[3][160];
        for (int k = 0; k < 3; k++) {   /* object arguments: the host object's class, or a guest object's own */
            uint32_t a = rd(esp + 12 + 4 * k);
            void *ha = m32_is_handle(a) ? m32_host(a) : NULL;
            uint32_t gc = !ha && a > 0x1000 && !m32_is_handle(a) && ocerz_addr_readable(a) && ocerz_addr_readable(a + 3) ? guest_class_of(a) : 0;
            void *tw = gc ? m32_host(a) : NULL;
            snprintf(what[k], sizeof what[k], "%s%s%s%s", ha ? object_getClassName((id)ha) : gc ? cname(gc) : "",
                     gc ? "(guest" : "", gc ? (tw != m32_h(a) ? ", twin " : ", no twin") : "",
                     gc ? (tw != m32_h(a) ? object_getClassName((id)tw) : "") : "");
            if (gc)
                strncat(what[k], ")", sizeof what[k] - strlen(what[k]) - 1);
            BOOL (*kind)(id, SEL, Class) = (BOOL (*)(id, SEL, Class))M32_MSGSEND;
            const char *(*utf8)(id, SEL) = (const char *(*)(id, SEL))M32_MSGSEND;
            const char *sv = ha && kind((id)ha, sel_registerName("isKindOfClass:"), objc_getClass("NSString"))
                                 ? utf8((id)ha, sel_registerName("UTF8String")) : NULL;
            if (sv)   /* a string: it, or its last characters (a path's end tells most) */
                snprintf(what[k] + strlen(what[k]), sizeof what[k] - strlen(what[k]), " \"%s%s\"", strlen(sv) > 64 ? "..." : "",
                         strlen(sv) > 64 ? sv + strlen(sv) - 61 : sv);
        }
        fprintf(stderr, "ocerz: m32: sendlog [t%u] %s to %#x (%p %s) args %#x %s, %#x %s, %#x %s, %#x, %#x from %#x\n",
                (unsigned)pthread_mach_thread_np(pthread_self()), (const char *)m32_h(sel), self, h,
                h ? object_getClassName((id)h) : "", rd(esp + 12), what[0], rd(esp + 16), what[1], rd(esp + 20), what[2],
                rd(esp + 24), rd(esp + 28), rd(esp));
        int r = host_send(vm, cpu, h, NULL, sel, 0, 0);
        uint32_t ret = (uint32_t)cpu->gpr[OCERZ_RAX];
        fprintf(stderr, "ocerz: m32: sendlog   -> %#x (%p %s)\n", ret, m32_is_handle(ret) ? m32_host(ret) : NULL,
                m32_is_handle(ret) ? object_getClassName((id)m32_host(ret)) : "");
        if (!watch && m32_is_handle(ret) && nwatched < 16)
            watched[nwatched++] = ret;
        return r;
    }
    return host_send(vm, cpu, h, NULL, sel, 0, 0);
}

static int sp_msgSend(struct OcerzVM *vm, OcerzCPU *cpu) { return send(vm, cpu, 1); }
static int sp_msgSend_stret(struct OcerzVM *vm, OcerzCPU *cpu) { return send(vm, cpu, 2); }

/* objc_msgSendSuper(struct objc_super {receiver, class} *, sel, ...) */
static int send_super(struct OcerzVM *vm, OcerzCPU *cpu, int sup_at)
{
    uint32_t esp = (uint32_t)cpu->gpr[OCERZ_RSP];
    uint32_t sp = rd(esp + 4 * sup_at), sel = rd(esp + 4 * sup_at + 4);
    uint32_t self = rd(sp), cls = rd(sp + 4);
    if (!self)
        return ret_nil(cpu, sup_at == 2 ? 4 : 0);
    if (is_guest_class(cls)) {
        uint32_t stop, imp = lookup(cls, sel, &stop, NULL);
        if (imp) {
            m32_wr(esp + 4 * sup_at, self);   /* the IMP wants self where the objc_super pointer was */
            cpu->rip = imp;
            return OCERZ_STEP_OK;
        }
        cls = stop;
    }
    Class hsup = m32_class_host(cls);
    void *h = m32_objc_to_host(self);
    return host_send(vm, cpu, h, hsup, sel, 0, 1);
}

static int sp_msgSendSuper(struct OcerzVM *vm, OcerzCPU *cpu) { return send_super(vm, cpu, 1); }
static int sp_msgSendSuper_stret(struct OcerzVM *vm, OcerzCPU *cpu) { return send_super(vm, cpu, 2); }

/* ---- runtime API ---- */

#define RET(v) do { m32_ret(cpu, (uint32_t)(v), 0, 0); return OCERZ_STEP_OK; } while (0)
static const char *str_arg(OcerzCPU *cpu, int n) { uint32_t g = m32_arg(cpu, n); return g ? (const char *)m32_h(g) : ""; }

static int sp_objc_getClass(struct OcerzVM *vm, OcerzCPU *cpu) { LOCK(); uint32_t c = class_by_name(str_arg(cpu, 0)); UNLOCK(); RET(c); }
static int sp_objc_getMetaClass(struct OcerzVM *vm, OcerzCPU *cpu) { LOCK(); uint32_t c = class_by_name(str_arg(cpu, 0)); UNLOCK(); RET(c ? rd(c + OC_ISA) : 0); }
static int sp_class_getName(struct OcerzVM *vm, OcerzCPU *cpu) { uint32_t c = m32_arg(cpu, 0); RET(c ? rd(c + OC_NAME) : m32_cstring("nil")); }
static int sp_object_getClass(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t o = m32_arg(cpu, 0);
    if (!o) RET(0);
    uint32_t gc = guest_class_of(o);
    if (gc) RET(gc);
    void *h = m32_objc_to_host(o);
    RET(h ? m32_class_guest(object_getClass(h)) : 0);
}
static int sp_object_getClassName(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t o = m32_arg(cpu, 0), gc = o ? guest_class_of(o) : 0;
    if (gc) RET(rd(gc + OC_NAME));
    void *h = m32_objc_to_host(o);
    RET(m32_cstring(h ? object_getClassName(h) : "nil"));
}
static int sp_class_getSuperclass(struct OcerzVM *vm, OcerzCPU *cpu) { uint32_t c = m32_arg(cpu, 0); RET(c ? rd(c + OC_SUPER) : 0); }
static int sp_sel_registerName(struct OcerzVM *vm, OcerzCPU *cpu) { LOCK(); uint32_t s = sel_intern(str_arg(cpu, 0), 0); UNLOCK(); RET(s); }
static int sp_sel_getName(struct OcerzVM *vm, OcerzCPU *cpu) { RET(m32_arg(cpu, 0)); }   /* a guest SEL is its name */
static int sp_class_respondsToSelector(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t c = m32_arg(cpu, 0), sel = m32_arg(cpu, 1), stop;
    if (!c) RET(0);
    if (lookup(c, sel, &stop, NULL)) RET(1);
    Class h = m32_class_host(stop ? stop : c);
    RET(h && class_respondsToSelector(h, m32_sel_host(sel)));
}

/* a guest method, or a handle to the host Method; the handle's cell word 0 marks it a Method */
static int sp_class_getInstanceMethod(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t c = m32_arg(cpu, 0), sel = m32_arg(cpu, 1), stop;
    if (!c) RET(0);
    for (uint32_t k = c; is_guest_class(k); k = rd(k + OC_SUPER)) {
        uint64_t head;
        if (map_get(&g_methods, k, &head))
            for (MList *l = (MList *)(uintptr_t)head; l; l = l->next) {
                uint32_t n = rd(l->list + 4);
                for (uint32_t i = 0; i < n; i++)
                    if (rd(l->list + 8 + 12 * i) == sel)
                        RET(l->list + 8 + 12 * i);
            }
    }
    lookup(c, sel, &stop, NULL);
    Class h = m32_class_host(stop ? stop : c);
    Method m = h ? class_getInstanceMethod(h, m32_sel_host(sel)) : NULL;
    RET(m ? m32_handle(m) : 0);
}
static int sp_class_getClassMethod(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t c = m32_arg(cpu, 0);
    m32_wr((uint32_t)cpu->gpr[OCERZ_RSP] + 4, c ? rd(c + OC_ISA) : 0);
    return sp_class_getInstanceMethod(vm, cpu);
}

static uint32_t hostfn_export(void *fn, const char *genc, const char *henc)
{
    char gn[1024], hn[1024];
    if (m32_encoding_notation(genc, 1, gn, sizeof gn) || m32_encoding_notation(henc, 0, hn, sizeof hn))
        return 0;
    return m32_export_hostfn(fn, gn, hn);
}

/* a method's implementation as the guest calls it: a guest method's own, or a host one's as an export */
static uint32_t method_imp(uint32_t m)
{
    if (!m)
        return 0;
    if (!m32_is_handle(m))
        return rd(m + 8);
    Method hm = m32_host(m);
    const char *henc = method_getTypeEncoding(hm);
    char buf[1024];
    return hostfn_export((void *)method_getImplementation(hm), guess_guest(henc, buf, sizeof buf), henc);
}
/* sets it, answering the old one */
static uint32_t method_set(uint32_t m, uint32_t imp)
{
    if (!m)
        return 0;
    if (!m32_is_handle(m)) {   /* a guest method: edit it in place, and the paired host method with it */
        uint32_t old = rd(m + 8);
        m32_wr(m + 8, imp);
        LOCK();
        free(g_cache.e);
        memset(&g_cache, 0, sizeof g_cache);
        UNLOCK();
        return old;
    }
    Method hm = m32_host(m);
    const char *henc = method_getTypeEncoding(hm);
    char buf[1024], gn[1024], hn[1024];
    const char *genc = guess_guest(henc, buf, sizeof buf);
    if (m32_encoding_notation(genc, 1, gn, sizeof gn) || m32_encoding_notation(henc, 0, hn, sizeof hn))
        return 0;
    IMP nw = (IMP)m32_callback(imp, gn, hn);
    IMP old = nw ? method_setImplementation(hm, nw) : NULL;
    return old ? m32_export_hostfn((void *)old, gn, hn) : 0;
}
static int sp_method_getImplementation(struct OcerzVM *vm, OcerzCPU *cpu) { RET(method_imp(m32_arg(cpu, 0))); }
static int sp_method_setImplementation(struct OcerzVM *vm, OcerzCPU *cpu)
{
    RET(method_set(m32_arg(cpu, 0), m32_arg(cpu, 1)));
}
/* method_exchangeImplementations(m1, m2): either may be a guest method or a handle to a host one.  Crossed, the host
 * runtime took a guest method for its own and faulted (Adium swizzles at launch).  Two host methods swap in the host
 * runtime; otherwise each gets the other's implementation as the guest calls it. */
static int sp_method_exchangeImplementations(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t m1 = m32_arg(cpu, 0), m2 = m32_arg(cpu, 1);
    if (!m1 || !m2)
        RET(0);
    if (m32_is_handle(m1) && m32_is_handle(m2)) {
        method_exchangeImplementations(m32_host(m1), m32_host(m2));
        RET(0);
    }
    uint32_t i1 = method_imp(m1), i2 = method_imp(m2);
    method_set(m1, i2);
    method_set(m2, i1);
    RET(0);
}
static int sp_method_getTypeEncoding(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t m = m32_arg(cpu, 0);
    if (!m) RET(0);
    if (!m32_is_handle(m)) RET(rd(m + 4));
    RET(m32_cstring(method_getTypeEncoding(m32_host(m))));
}
static int sp_method_getName(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t m = m32_arg(cpu, 0);
    if (!m) RET(0);
    RET(m32_is_handle(m) ? m32_sel_guest(method_getName(m32_host(m))) : rd(m));
}
static int sp_NSClassFromString(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *s = m32_objc_to_host(m32_arg(cpu, 0));
    const char *(*utf8)(id, SEL) = (const char *(*)(id, SEL))M32_MSGSEND;
    const char *name = s ? utf8(s, sel_registerName("UTF8String")) : NULL;
    LOCK();
    uint32_t c = name ? class_by_name(name) : 0;
    UNLOCK();
    RET(c);
}
static int sp_NSStringFromClass(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t c = m32_arg(cpu, 0);
    if (!c) RET(0);
    id (*mk)(Class, SEL, const char *) = (id (*)(Class, SEL, const char *))M32_MSGSEND;
    id s = mk(objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), cname(c));
    RET(m32_objc_to_guest(s));
}
static int sp_NSSelectorFromString(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *s = m32_objc_to_host(m32_arg(cpu, 0));
    const char *(*utf8)(id, SEL) = (const char *(*)(id, SEL))M32_MSGSEND;
    const char *name = s ? utf8(s, sel_registerName("UTF8String")) : NULL;
    LOCK();
    uint32_t g = name ? sel_intern(name, 0) : 0;
    UNLOCK();
    RET(g);
}
static int sp_NSStringFromSelector(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t s = m32_arg(cpu, 0);
    if (!s) RET(0);
    id (*mk)(Class, SEL, const char *) = (id (*)(Class, SEL, const char *))M32_MSGSEND;
    RET(m32_objc_to_guest(mk(objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), (const char *)m32_h(s))));
}
static int sp_objc_sync(struct OcerzVM *vm, OcerzCPU *cpu, int enter)
{
    void *o = m32_objc_to_host(m32_arg(cpu, 0));
    RET(enter ? objc_sync_enter(o) : objc_sync_exit(o));
}
static int sp_objc_sync_enter(struct OcerzVM *vm, OcerzCPU *cpu) { return sp_objc_sync(vm, cpu, 1); }
static int sp_objc_sync_exit(struct OcerzVM *vm, OcerzCPU *cpu) { return sp_objc_sync(vm, cpu, 0); }
/* objc_getProperty(self, _cmd, offset, atomic) / objc_setProperty(self, _cmd, offset, newValue, atomic, copy): the
 * ivar is the guest twin's word at offset */
static int sp_objc_getProperty(struct OcerzVM *vm, OcerzCPU *cpu) { RET(rd(m32_arg(cpu, 0) + m32_arg(cpu, 2))); }
static int sp_objc_setProperty(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t self = m32_arg(cpu, 0), off = m32_arg(cpu, 2), val = m32_arg(cpu, 3);
    uint32_t copy = m32_arg(cpu, 5) & 0xff;
    void *h = m32_objc_to_host(val);
    id (*msg)(id, SEL) = (id (*)(id, SEL))M32_MSGSEND;
    void *nv = h ? msg(h, sel_registerName(copy ? "copy" : "retain")) : NULL;
    void *old = m32_objc_to_host(rd(self + off));
    m32_wr(self + off, m32_objc_to_guest(nv));
    if (old)
        msg(old, sel_registerName("release"));
    RET(0);
}
static int sp_enumerationMutation(struct OcerzVM *vm, OcerzCPU *cpu)
{
    m32_log_once("collection mutated during fast enumeration:", "objc_enumerationMutation");
    RET(0);
}

/* NSLog(format, ...) and NSLogv(format, va_list): the format walk over the arguments at ap, then the host's NSLogv.
 * An i386 va_list is the address of the next argument on the stack.  NSLogv had no special: crossed by its
 * notation, the guest's va_list reached Foundation as an arm64 one, and iWeb crashed there.  NSLog looked NSLogv
 * up in libobjc, found nothing and logged nothing. */
static int ns_logv(OcerzCPU *cpu, uint32_t ap)
{
    void *fmt = m32_objc_to_host(m32_arg(cpu, 0));
    const char *(*utf8)(id, SEL) = (const char *(*)(id, SEL))M32_MSGSEND;
    uint64_t slots[64];
    int n = fmt ? m32_printf_slots(utf8(fmt, sel_registerName("UTF8String")), ap, slots, 64, "NSLog") : -1;
    static void (*logv)(id, void *);
    if (!logv)   /* Foundation's, not libobjc's: m32rt_sym only searches libobjc and what it links */
        logv = (void (*)(id, void *))dlsym(RTLD_DEFAULT, "NSLogv");
    if (n >= 0 && logv)
        logv(fmt, slots);
    RET(0);
}
static int sp_NSLog(struct OcerzVM *vm, OcerzCPU *cpu) { return ns_logv(cpu, (uint32_t)cpu->gpr[OCERZ_RSP] + 8); }
static int sp_NSLogv(struct OcerzVM *vm, OcerzCPU *cpu) { return ns_logv(cpu, m32_arg(cpu, 1)); }

/* NSCreateMapTable and NSCreateHashTable take their callback structures by value: on i386 they are the words on the
 * stack, a key's six (hash, isEqual, retain, release, describe, notAKeyMarker), a value's three, a hash table's five,
 * then the capacity and, ...WithZone, the zone.  A function word is NULL, a host function the guest copied out of
 * Foundation's NSObjectMapKeyCallBacks and the like (a handle), or a guest function, called back; the arm64 call
 * takes the structures as copies.  VLC 1.1's interface makes its tables this way. */
typedef struct { uint64_t w[6]; } HostMapKeyCallBacks;
typedef struct { uint64_t w[3]; } HostMapValueCallBacks;
typedef struct { uint64_t w[5]; } HostHashCallBacks;

static uint64_t table_fn(uint32_t word, const char *guest, const char *host)
{
    if (!word)
        return 0;
    if (m32_is_handle(word))
        return (uint64_t)(uintptr_t)m32_host(word);
    return (uint64_t)(uintptr_t)m32_callback(word, guest, host);
}

static const char *const g_key_sigs[5][2] = { { "u(pp)", "L(pp)" }, { "b(ppp)", "B(ppp)" }, { "v(pp)", "v(pp)" },
                                              { "v(pp)", "v(pp)" }, { "@(pp)", "@(pp)" } };

static int map_table(struct OcerzVM *vm, OcerzCPU *cpu, int zoned)
{
    (void)vm;
    void *fn = dlsym(RTLD_DEFAULT, zoned ? "NSCreateMapTableWithZone" : "NSCreateMapTable");
    if (!fn)
        RET(0);
    HostMapKeyCallBacks k;
    HostMapValueCallBacks v;
    for (int i = 0; i < 5; i++)
        k.w[i] = table_fn(m32_arg(cpu, i), g_key_sigs[i][0], g_key_sigs[i][1]);
    k.w[5] = m32_arg(cpu, 5) ? (uint64_t)(uintptr_t)m32_host(m32_arg(cpu, 5)) : 0;   /* notAKeyMarker, as keys cross */
    for (int i = 0; i < 3; i++)
        v.w[i] = table_fn(m32_arg(cpu, 6 + i), g_key_sigs[i + 2][0], g_key_sigs[i + 2][1]);
    uint64_t capacity = m32_arg(cpu, 9);
    void *t = zoned ? ((void *(*)(HostMapKeyCallBacks, HostMapValueCallBacks, uint64_t, void *))fn)(k, v, capacity, NULL)
                    : ((void *(*)(HostMapKeyCallBacks, HostMapValueCallBacks, uint64_t))fn)(k, v, capacity);
    RET(m32_handle(t));
}

static int hash_table(struct OcerzVM *vm, OcerzCPU *cpu, int zoned)
{
    (void)vm;
    void *fn = dlsym(RTLD_DEFAULT, zoned ? "NSCreateHashTableWithZone" : "NSCreateHashTable");
    if (!fn)
        RET(0);
    HostHashCallBacks c;
    for (int i = 0; i < 5; i++)
        c.w[i] = table_fn(m32_arg(cpu, i), g_key_sigs[i][0], g_key_sigs[i][1]);
    uint64_t capacity = m32_arg(cpu, 5);
    void *t = zoned ? ((void *(*)(HostHashCallBacks, uint64_t, void *))fn)(c, capacity, NULL)
                    : ((void *(*)(HostHashCallBacks, uint64_t))fn)(c, capacity);
    RET(m32_handle(t));
}

static int sp_NSCreateMapTable(struct OcerzVM *vm, OcerzCPU *cpu) { return map_table(vm, cpu, 0); }
static int sp_NSCreateMapTableWithZone(struct OcerzVM *vm, OcerzCPU *cpu) { return map_table(vm, cpu, 1); }
static int sp_NSCreateHashTable(struct OcerzVM *vm, OcerzCPU *cpu) { return hash_table(vm, cpu, 0); }
static int sp_NSCreateHashTableWithZone(struct OcerzVM *vm, OcerzCPU *cpu) { return hash_table(vm, cpu, 1); }

/* NSApplicationMain(int argc, const char **argv): argv is the guest's vector of 4-byte pointers, which AppKit read as
 * 8-byte ones and faulted on (SDL 1.2's SDLMain.m calls it in every SDL game that ships it: Wesnoth).  AppKit gets a
 * vector of its own.  It does not return. */
static int sp_NSApplicationMain(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    int (*app_main)(int, const char **) = (int (*)(int, const char **))ocerz_bridge_host_symbol(
        "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit", "NSApplicationMain");
    uint32_t esp = (uint32_t)cpu->gpr[OCERZ_RSP], argv = rd(esp + 8);
    int argc = (int32_t)rd(esp + 4);
    if (argc < 0 || argc > 4096 || !argv)
        argc = 0;
    const char **hv = calloc((size_t)argc + 1, sizeof *hv);
    for (int i = 0; hv && i < argc; i++)
        hv[i] = rd(argv + 4 * (uint32_t)i) ? m32_h(rd(argv + 4 * (uint32_t)i)) : NULL;
    int r = app_main && hv ? app_main(argc, hv) : 1;
    free(hv);
    m32_ret(cpu, (uint32_t)r, 0, 0);
    return OCERZ_STEP_OK;
}

const M32SpecialEntry m32_objc_specials[] = {
    { "_NSApplicationMain", sp_NSApplicationMain },
    { "_NSCreateMapTable", sp_NSCreateMapTable }, { "_NSCreateMapTableWithZone", sp_NSCreateMapTableWithZone },
    { "_NSCreateHashTable", sp_NSCreateHashTable }, { "_NSCreateHashTableWithZone", sp_NSCreateHashTableWithZone },
    { "_NSLog", sp_NSLog }, { "_NSLogv", sp_NSLogv },
    { "_objc_msgSend", sp_msgSend }, { "_objc_msgSend_fpret", sp_msgSend }, { "_objc_msgSend_stret", sp_msgSend_stret },
    { "_objc_msgSendSuper", sp_msgSendSuper }, { "_objc_msgSendSuper_stret", sp_msgSendSuper_stret },
    { "_objc_getClass", sp_objc_getClass }, { "_objc_lookUpClass", sp_objc_getClass },
    { "_objc_getRequiredClass", sp_objc_getClass }, { "_objc_getMetaClass", sp_objc_getMetaClass },
    { "_class_getName", sp_class_getName }, { "_object_getClass", sp_object_getClass },
    { "_object_getClassName", sp_object_getClassName }, { "_class_getSuperclass", sp_class_getSuperclass },
    { "_sel_registerName", sp_sel_registerName }, { "_sel_getUid", sp_sel_registerName },
    { "_sel_getName", sp_sel_getName }, { "_class_respondsToSelector", sp_class_respondsToSelector },
    { "_class_getInstanceMethod", sp_class_getInstanceMethod }, { "_class_getClassMethod", sp_class_getClassMethod },
    { "_method_getImplementation", sp_method_getImplementation },
    { "_method_exchangeImplementations", sp_method_exchangeImplementations },
    { "_method_setImplementation", sp_method_setImplementation },
    { "_method_getTypeEncoding", sp_method_getTypeEncoding }, { "_method_getName", sp_method_getName },
    { "_NSClassFromString", sp_NSClassFromString }, { "_NSStringFromClass", sp_NSStringFromClass },
    { "_NSSelectorFromString", sp_NSSelectorFromString }, { "_NSStringFromSelector", sp_NSStringFromSelector },
    { "_objc_sync_enter", sp_objc_sync_enter }, { "_objc_sync_exit", sp_objc_sync_exit },
    { "_objc_getProperty", sp_objc_getProperty }, { "_objc_setProperty", sp_objc_setProperty },
    { "_objc_enumerationMutation", sp_enumerationMutation },
    { NULL, NULL }
};

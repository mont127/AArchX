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
    objc_registerClassPair(hc);
    map_put(&g_host_of, g, (uint64_t)(uintptr_t)hc);
    map_put(&g_host_of, meta, (uint64_t)(uintptr_t)object_getClass((id)hc));
    map_put(&g_pair_guest, (uint64_t)(uintptr_t)hc, g);
    map_put(&g_pair_guest, (uint64_t)(uintptr_t)object_getClass((id)hc), meta);
}

/* ---- loading an image's __OBJC ---- */

typedef struct Loads { struct Loads *next; M32Image *img; int nc, ncat; uint32_t v[]; } Loads;
static Loads *g_loads;

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

static M32Export *send_export(const char *sel, const char *genc, const char *henc, int super)
{
    char key[2100];
    snprintf(key, sizeof key, "%s|%s|%d", genc, henc, super);
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
        e = calloc(1, sizeof *e);
        e->lib = strdup(key);
        e->name = strdup(sel);
        e->kind = M32_EX_FN;
        e->gnote = strdup(gn);
        e->hnote = strdup(hn);
        e->host_fn = super ? M32_MSGSENDSUPER : M32_MSGSEND;
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

/* self_at: the guest stack word holding the receiver (1, or 2 after a stret pointer); super: the objc_super */
static int host_send(struct OcerzVM *vm, OcerzCPU *cpu, void *hself, Class super_cls, uint32_t gsel, uint32_t gtypes, int is_super)
{
    SEL hs = m32_sel_host(gsel);
    const char *name = sel_getName(hs);
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
    M32Export *e = send_export(name, genc, henc, is_super);
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
    return m32_cross(vm, cpu, e);
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
        }
        fprintf(stderr, "ocerz: m32: sendlog [t%u] %s to %#x (%p %s) args %#x %s, %#x %s, %#x %s from %#x\n",
                (unsigned)pthread_mach_thread_np(pthread_self()), (const char *)m32_h(sel), self, h,
                h ? object_getClassName((id)h) : "", rd(esp + 12), what[0], rd(esp + 16), what[1], rd(esp + 20), what[2],
                rd(esp));
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

static int sp_method_getImplementation(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t m = m32_arg(cpu, 0);
    if (!m) RET(0);
    if (!m32_is_handle(m)) RET(rd(m + 8));
    Method hm = m32_host(m);
    const char *henc = method_getTypeEncoding(hm);
    char buf[1024];
    RET(hostfn_export((void *)method_getImplementation(hm), guess_guest(henc, buf, sizeof buf), henc));
}
static int sp_method_setImplementation(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t m = m32_arg(cpu, 0), imp = m32_arg(cpu, 1);
    if (!m) RET(0);
    if (!m32_is_handle(m)) {   /* a guest method: edit it in place, and the paired host method with it */
        uint32_t old = rd(m + 8);
        m32_wr(m + 8, imp);
        LOCK();
        free(g_cache.e);
        memset(&g_cache, 0, sizeof g_cache);
        UNLOCK();
        RET(old);
    }
    Method hm = m32_host(m);
    const char *henc = method_getTypeEncoding(hm);
    char buf[1024], gn[1024], hn[1024];
    const char *genc = guess_guest(henc, buf, sizeof buf);
    if (m32_encoding_notation(genc, 1, gn, sizeof gn) || m32_encoding_notation(henc, 0, hn, sizeof hn)) RET(0);
    IMP nw = (IMP)m32_callback(imp, gn, hn);
    IMP old = nw ? method_setImplementation(hm, nw) : NULL;
    RET(old ? m32_export_hostfn((void *)old, gn, hn) : 0);
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

/* NSLog(format, ...): the format walk, then NSLogv */
static int sp_NSLog(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *fmt = m32_objc_to_host(m32_arg(cpu, 0));
    const char *(*utf8)(id, SEL) = (const char *(*)(id, SEL))M32_MSGSEND;
    uint64_t slots[64];
    int n = fmt ? m32_printf_slots(utf8(fmt, sel_registerName("UTF8String")), (uint32_t)cpu->gpr[OCERZ_RSP] + 8, slots, 64, "NSLog") : -1;
    static void (*logv)(id, void *);
    if (!logv)
        logv = (void (*)(id, void *))m32rt_sym("NSLogv");
    if (n >= 0 && logv)
        logv(fmt, slots);
    RET(0);
}

const M32SpecialEntry m32_objc_specials[] = {
    { "_NSLog", sp_NSLog },
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
    { "_method_setImplementation", sp_method_setImplementation },
    { "_method_getTypeEncoding", sp_method_getTypeEncoding }, { "_method_getName", sp_method_getName },
    { "_NSClassFromString", sp_NSClassFromString }, { "_NSStringFromClass", sp_NSStringFromClass },
    { "_NSSelectorFromString", sp_NSSelectorFromString }, { "_NSStringFromSelector", sp_NSStringFromSelector },
    { "_objc_sync_enter", sp_objc_sync_enter }, { "_objc_sync_exit", sp_objc_sync_exit },
    { "_objc_getProperty", sp_objc_getProperty }, { "_objc_setProperty", sp_objc_setProperty },
    { "_objc_enumerationMutation", sp_enumerationMutation },
    { NULL, NULL }
};

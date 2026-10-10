/*
 * m32's exports and its trap (include/ocerz/m32.h).
 *
 * Every import of a system library becomes an export with an id; its address is M32_TRAP(id).  The table is a fixed
 * array indexed by id, so the trap reads it without a lock; entries are only ever added, under g_lock.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dlfcn.h>
#include <mach-o/loader.h>
#include <mach/mach.h>

#include "ocerz/bridge.h"
#include "ocerz/interp.h"
#include "ocerz/flags.h"
#include "ocerz/jit.h"
#include "ocerz/m32_db.h"
#include "ocerz/m32_objc.h"
#include "ocerz/m32.h"
#include "ocerz/types.h"
#include "ocerz/vm.h"

int m32_log_imports;

/* whether a host symbol is code: the segment of its image that holds it is executable */
static int host_is_code(void *p)
{
    Dl_info info;
    if (!dladdr(p, &info) || !info.dli_fbase)
        return 1;
    const struct mach_header_64 *mh = info.dli_fbase;
    const uint8_t *lc = (const uint8_t *)(mh + 1);
    intptr_t slide = 0;
    for (uint32_t i = 0; i < mh->ncmds; i++, lc += ((const struct load_command *)lc)->cmdsize) {
        const struct segment_command_64 *seg = (const void *)lc;
        if (seg->cmd != LC_SEGMENT_64)
            continue;
        if (!strcmp(seg->segname, "__TEXT"))
            slide = (intptr_t)mh - (intptr_t)seg->vmaddr;
    }
    lc = (const uint8_t *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc += ((const struct load_command *)lc)->cmdsize) {
        const struct segment_command_64 *seg = (const void *)lc;
        if (seg->cmd != LC_SEGMENT_64)
            continue;
        uintptr_t lo = (uintptr_t)(seg->vmaddr + slide);
        if ((uintptr_t)p < lo || (uintptr_t)p >= lo + seg->vmsize)
            continue;
        const struct section_64 *sc = (const void *)(seg + 1);   /* __TEXT also holds constants (NSZeroPoint) */
        for (uint32_t k = 0; k < seg->nsects; k++, sc++) {
            uintptr_t a = (uintptr_t)(sc->addr + slide);
            if ((uintptr_t)p >= a && (uintptr_t)p < a + sc->size)
                return (sc->flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) != 0;
        }
        return (seg->initprot & VM_PROT_EXECUTE) != 0;
    }
    return 1;
}

/* data32 records: ptr/obj variables hold the host value as the guest sees it, int and blob copy the bytes.
 * Pointer variables can change after load (NSApp): they are live, and m32_refresh_data re-reads them after every
 * crossing (ponytail: a scan of every live variable; a few dozen in a game). */
static M32Export *g_live[512];
static int g_nlive;
static void *g_live_seen[512];

static void m32_fill_data(M32Export *e)
{
    if (!e->host_fn || !e->data)
        return;
    int obj = !strcmp(e->data_kind, "obj");
    if (obj || !strcmp(e->data_kind, "ptr")) {
        void *v = *(void **)e->host_fn;
        m32_wr(e->data, obj ? m32_objc_to_guest(v) : m32_handle(v));
        if (g_nlive < 512) {
            g_live_seen[g_nlive] = v;
            g_live[g_nlive++] = e;
        }
    } else if (e->gsize <= e->hsize)
        memcpy(m32_h(e->data), e->host_fn, (size_t)e->gsize);   /* little-endian: an int's low bytes first */
}

void m32_refresh_data(void)
{
    /* ponytail: after a million crossings (startup, where NSApp and friends get set) every 64th crossing rescans; a
     * variable that changes later is seen within 64 host calls.  The scan after every GL call was 3% of Batman's
     * render thread. */
    static unsigned long calls;
    unsigned long n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    if (n > (1ul << 20) && (n & 63))
        return;
    for (int i = 0; i < g_nlive; i++) {
        M32Export *e = g_live[i];
        void *v = *(void **)e->host_fn;
        if (v != g_live_seen[i]) {
            g_live_seen[i] = v;
            m32_wr(e->data, !strcmp(e->data_kind, "obj") ? m32_objc_to_guest(v) : m32_handle(v));
        }
    }
}

static M32Export *g_exports[M32_ID_LIMIT];
static uint32_t g_next = M32_ID_FIRST;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct Seen { struct Seen *next; char key[]; } Seen;
static Seen *g_seen;

void m32_log_once(const char *what, const char *name)
{
    char key[512];
    snprintf(key, sizeof key, "%s|%s", what, name);
    pthread_mutex_lock(&g_lock);
    for (Seen *s = g_seen; s; s = s->next)
        if (!strcmp(s->key, key)) {
            pthread_mutex_unlock(&g_lock);
            return;
        }
    Seen *s = malloc(sizeof *s + strlen(key) + 1);
    strcpy(s->key, key);
    s->next = g_seen;
    g_seen = s;
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "ocerz: m32: %s %s\n", what, name);
}

/* a host function handed to the guest as a callable address (a host IMP, a function pointer from the host) */
uint32_t m32_export_hostfn(void *fn, const char *gnote, const char *hnote)
{
    if (!fn)
        return 0;
    pthread_mutex_lock(&g_lock);
    for (uint32_t id = M32_ID_FIRST; id < g_next; id++) {
        M32Export *x = g_exports[id];
        if (x->host_fn == fn && x->kind == M32_EX_FN && x->gnote && !strcmp(x->gnote, gnote) && !strcmp(x->hnote, hnote)) {
            pthread_mutex_unlock(&g_lock);
            return M32_TRAP(id);
        }
    }
    if (g_next >= M32_ID_LIMIT) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    M32Export *e = calloc(1, sizeof *e);
    e->lib = strdup("(host function)");
    char name[64];
    snprintf(name, sizeof name, "hostfn@%p", fn);
    e->name = strdup(name);
    e->kind = M32_EX_FN;
    e->host_fn = fn;
    e->gnote = strdup(gnote);
    e->hnote = strdup(hnote);
    uint32_t id = g_next++;
    g_exports[id] = e;
    pthread_mutex_unlock(&g_lock);
    return M32_TRAP(id);
}

M32Export *m32_export_by_id(uint32_t id)
{
    return id < M32_ID_LIMIT ? g_exports[id] : NULL;
}

/* OCERZ_M32_HUD=<overlay dylib>: an FPS overlay (simpleoverlay, whose exported fake_dlsym hands back its
 * CGLFlushDrawable wrapper) is loaded here rather than through DYLD_INSERT_LIBRARIES, which every child process
 * inherits (Batman then ran a shell and quit); each frame the guest presents is drawn on before the real flush */
static void *hud_wrap(const char *sym, void *fn)
{
    const char *hud = getenv("OCERZ_M32_HUD");
    if (!hud || !fn || strcmp(sym, "CGLFlushDrawable"))
        return fn;
    void *h = dlopen(hud, RTLD_NOW | RTLD_LOCAL);
    void *(*lookup)(void *, const char *) = h ? (void *(*)(void *, const char *))dlsym(h, "fake_dlsym") : NULL;
    void *w = lookup ? lookup(RTLD_DEFAULT, sym) : NULL;
    fprintf(stderr, "ocerz: m32: HUD %s: %s\n", hud, w && w != fn ? "drawing on each CGLFlushDrawable" : "not loaded");
    return w ? w : fn;
}

/* The trap address for an import of name from lib (NULL: flat lookup). One export per symbol name. */
uint32_t m32_export(const char *lib, const char *name)
{
    pthread_mutex_lock(&g_lock);
    for (uint32_t id = M32_ID_FIRST; id < g_next; id++)
        if (!strcmp(g_exports[id]->name, name)) {
            uint32_t data = g_exports[id]->data;   /* a variable is its guest address, every time */
            pthread_mutex_unlock(&g_lock);
            return data ? data : M32_TRAP(id);
        }
    if (g_next >= M32_ID_LIMIT) {
        pthread_mutex_unlock(&g_lock);
        fprintf(stderr, "ocerz: m32: out of export ids at %s\n", name);
        return 0;
    }
    M32Export *e = calloc(1, sizeof *e);
    e->lib = strdup(lib ? lib : "(flat)");
    e->name = strdup(name);
    e->special = m32_special(name);
    e->kind = e->special ? M32_EX_SPECIAL : M32_EX_UNRESOLVED;
    M32DbRec r;
    if (!e->special && m32_db_lookup(lib, name, &r)) {
        e->variant = r.variant;
        e->host_symbol = r.host_symbol;
        if (r.kind == M32_DB_FN) {
            e->kind = M32_EX_FN;
            e->gnote = r.guest;
            e->hnote = r.host;
            e->host_fn = ocerz_bridge_host_symbol(lib ? lib : OCERZ_BRIDGE_LIBSYSTEM, r.host_symbol);
            if (!e->host_fn)
                e->host_fn = dlsym(RTLD_DEFAULT, r.host_symbol);
            if (!e->host_fn) {
                e->kind = M32_EX_BAD;
                e->reason = "host-missing";
            }
            e->host_fn = hud_wrap(r.host_symbol, e->host_fn);
        } else if (r.kind == M32_DB_DATA) {
            e->kind = M32_EX_DATA;
            e->gsize = r.guest_size;
            e->hsize = r.host_size;
            e->data_kind = r.data_kind;
            e->host_fn = ocerz_bridge_host_symbol(lib ? lib : OCERZ_BRIDGE_LIBSYSTEM, r.host_symbol);
            if (!e->host_fn)
                e->host_fn = dlsym(RTLD_DEFAULT, r.host_symbol);
        } else {
            e->kind = M32_EX_BAD;
            e->reason = r.reason;
        }
    }
    uint32_t id = g_next++, addr = M32_TRAP(id);
    void *hv = NULL;
    if (!e->special) {
        char base[256];
        snprintf(base, sizeof base, "%s", name[0] == '_' ? name + 1 : name);
        if (strchr(base, '$'))
            *strchr(base, '$') = 0;
        void *lh = lib ? ocerz_bridge_host_library(lib) : NULL;   /* frameworks open RTLD_LOCAL: ask the handle */
        hv = lh ? dlsym(lh, base) : NULL;
        if (!hv)
            hv = dlsym(RTLD_DEFAULT, base);
    }
    uint32_t sd = e->special ? 0 : m32_special_data(name, hv);
    if (sd) {
        e->kind = M32_EX_DATA;
        e->data = sd;
        addr = sd;
    } else if (e->kind == M32_EX_DATA && strcmp(e->data_kind, "opaque") && e->gsize > 0) {
        e->data = m32_static_alloc(e->gsize > 4 ? (uint32_t)e->gsize : 4, 4);
        addr = e->data;
        m32_fill_data(e);
    } else if (e->kind == M32_EX_DATA) {   /* opaque: a placeholder whose address stands for the host object */
        e->data = m32_static_alloc(64, 16);
        if (e->host_fn)
            m32_alias(e->data, e->host_fn);
        addr = e->data;
    } else if ((e->kind == M32_EX_BAD || e->kind == M32_EX_UNRESOLVED) && hv && !host_is_code(hv)) {
        /* data whose layouts differ or that no header declares: a zeroed guest placeholder aliased to the host
         * variable, so passing its address (&kCFTypeArrayCallBacks) reaches the host one, and a read sees 0 */
        if (e->kind == M32_EX_UNRESOLVED)
            e->reason = "undeclared-data";
        e->kind = M32_EX_BAD;
        e->data = m32_static_alloc(64, 16);
        m32_alias(e->data, hv);
        addr = e->data;
    }
    g_exports[id] = e;   /* published after it is complete */
    pthread_mutex_unlock(&g_lock);
    if (m32_log_imports) {
        static const char *const kinds[] = { "unresolved", "special", "fn", "needs a special", "data" };
        fprintf(stderr, "ocerz: m32: import %s %s -> %s %s %s @%#x\n", e->lib, name, kinds[e->kind],
                e->kind == M32_EX_FN ? e->gnote : e->kind == M32_EX_BAD ? e->reason : "",
                e->kind == M32_EX_FN ? e->hnote : "", addr);
    }
    return addr;
}

int m32_trap(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t id = (uint32_t)(cpu->rip - OCERZ_DYLDAPI_LO);
    if (id == M32_ID_EXIT) {
        fflush(NULL);
        exit((int)(cpu->gpr[OCERZ_RAX] & 0xff));
    }
    if (id == M32_ID_TLV)
        return m32_tlv_trap(vm, cpu);
    M32Export *e = m32_export_by_id(id);
    if (!e) {
        fprintf(stderr, "ocerz: m32: call to trap id %#x with no export (from %#x)\n", (unsigned)id,
                m32_rd((uint32_t)cpu->gpr[OCERZ_RSP]));
        ocerz_vm_request_exit(vm, 134);
        return OCERZ_STEP_EXIT;
    }
    /* OCERZ_M32_CALLLOG=<name part>[|<part>...]: the matching imports' first argument words, caller and eax after */
    static const char *calllog = (const char *)1;
    if (calllog == (const char *)1)
        calllog = getenv("OCERZ_M32_CALLLOG");
    int hit = 0;
    for (const char *t = calllog; t && *t && !hit; t = strchr(t, '|') ? strchr(t, '|') + 1 : "") {
        char tok[128];
        snprintf(tok, sizeof tok, "%.*s", (int)(strchr(t, '|') ? strchr(t, '|') - t : (long)strlen(t)), t);
        hit = tok[0] && strstr(e->name, tok) != NULL;
    }
    if (hit) {
        uint32_t esp = (uint32_t)cpu->gpr[OCERZ_RSP];
        int rc = e->special ? e->special(vm, cpu) : e->kind == M32_EX_FN ? m32_cross(vm, cpu, e) : -1;
        fprintf(stderr, "ocerz: m32: calllog [t%u] %s(%#x, %#x, %#x, %#x) from %#x -> eax %#x\n",
                (unsigned)pthread_mach_thread_np(pthread_self()), e->name, m32_rd(esp + 4), m32_rd(esp + 8),
                m32_rd(esp + 12), m32_rd(esp + 16), m32_rd(esp), (uint32_t)cpu->gpr[OCERZ_RAX]);
        /* OCERZ_M32_CALLLOG_DESC=<k>: argument k (0-based) described by CF - name only CF-object arguments */
        const char *dk = getenv("OCERZ_M32_CALLLOG_DESC");
        if (dk) {
            static void *(*desc)(const void *);
            static int (*cstr)(const void *, char *, long, uint32_t);
            if (!desc) {
                desc = (void *(*)(const void *))dlsym(RTLD_DEFAULT, "CFCopyDescription");
                cstr = (int (*)(const void *, char *, long, uint32_t))dlsym(RTLD_DEFAULT, "CFStringGetCString");
            }
            void *o = m32_host(m32_rd(esp + 4 + 4 * (uint32_t)atoi(dk)));
            void *d = o && desc ? desc(o) : NULL;
            char buf[4096] = "";
            if (d && cstr(d, buf, sizeof buf, 0x08000100))
                fprintf(stderr, "ocerz: m32: calllog   arg %s = %s\n", dk, buf);
        }
        if (rc >= 0)
            return rc;
    }
    if (e->special)
        return e->special(vm, cpu);
    if (e->kind == M32_EX_FN)
        return m32_cross(vm, cpu, e);
    if (e->kind == M32_EX_BAD)
        m32_log_once(e->reason, e->name);
    else
        m32_log_once(e->kind == M32_EX_FN ? "no generic crossing yet, returning 0:" : "unresolved, returning 0:", e->name);
    m32_ret(cpu, 0, 0, 0);
    return OCERZ_STEP_OK;
}

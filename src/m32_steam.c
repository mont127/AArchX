/*
 * The Steam bridge for i386 programs (include/ocerz/m32.h).
 *
 * A 32-bit Mac game ships Valve's i386 libsteam_api.dylib, which asks the running Steam where its client lives (Steam
 * answers the processes it launched itself, so the game starts from Steam with `ocerz -native %command%` as its
 * launch options), loads <there>/steamclient.dylib and calls CreateInterface for each interface version it was built
 * with.  Valve has shipped no i386 steamclient since 2019: the one there is x86_64 and arm64 only.  So when an i386
 * program dlopens a steamclient.dylib without an i386 slice, m32 opens the host's own (its arm64 slice) and hands
 * the program a handle whose dlsym answers the client's exports itself.  Nothing of Valve's is shipped: the game talks
 * to the user's own Steam, which still serves every interface version back to SteamClient006.
 *
 * Interfaces.  CreateInterface, and every method that returns an interface (GetISteamUser and the rest, flagged I in
 * the database), return a proxy: a guest object whose first word is a vtable of trap addresses and that is aliased
 * to the host object (m32_alias), so the generic crossing turns a proxy `this` into the host object by itself.  Each
 * slot is the host vtable's function at that index, crossed by the notations runtime/apis32/.../steam.api gives for
 * the version (tools/steamgen.sh writes it from the Steamworks headers, i386 and arm64 side by side).  Methods that
 * return a C string (S) copy it into guest memory, kept per host string; a slot the database marks bad answers 0.
 * Steamworks 1.23 and later lay callback structures out with pack(4) on macOS, which i386 and arm64 share, so call
 * results land in the guest's buffer as they are; Steam_BGetCallback's CallbackMsg_t holds a pointer and is
 * converted, its payload copied into a guest buffer that stays valid until the next one.
 *
 * Breakpad's crash reporting answers nothing.  OCERZ_M32LOG=steam names the interfaces as they are handed out and the
 * bad slots as they are called; OCERZ_NO_M32_STEAM=1 leaves a steamclient.dylib to the ordinary loader, which refuses
 * one with no i386 slice.
 */
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ocerz/m32.h"
#include "ocerz/m32_db.h"
#include "ocerz/interp.h"
#include "ocerz/vm.h"

#define STEAM_MAGIC 0x5354324du   /* 'M2TS': a guest dlopen handle for the bridged client */

typedef struct Slot { char *name; char flag; int bad; M32Export ex; } Slot;
typedef struct Iface { char *version; Slot *slots; int n; } Iface;
typedef struct Vt { const Iface *iface; void **hostvt; uint32_t gvt; } Vt;
typedef struct Proxy { void *host; uint32_t guest; } Proxy;
typedef struct Flagged { uint32_t id; Slot *slot; } Flagged;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Iface *g_ifaces;
static int g_nifaces, g_loaded;
static Vt *g_vts;
static int g_nvts;
static Proxy *g_proxies;
static int g_nproxies;
static Flagged *g_flagged;
static int g_nflagged;
static void *g_client;
static uint32_t g_handle;
static int g_log = -1;

static int steam_log(void)
{
    if (g_log < 0)
        g_log = getenv("OCERZ_M32LOG") && strstr(getenv("OCERZ_M32LOG"), "steam");
    return g_log;
}

static void load_db(void)
{
    if (g_loaded)
        return;
    g_loaded = 1;
    const char *dir = m32_db_dir();
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/steam.api", dir ? dir : ".");
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "ocerz: m32: no Steam interface database at %s (make steam32)\n", path);
        return;
    }
    char line[1024];
    Iface *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        char a[256], b[256], c[256], d[256], e[8] = "";
        if (!strncmp(line, "iface ", 6) && sscanf(line, "iface %255s", a) == 1) {
            g_ifaces = realloc(g_ifaces, (size_t)(g_nifaces + 1) * sizeof *g_ifaces);
            cur = &g_ifaces[g_nifaces++];
            *cur = (Iface){ strdup(a), NULL, 0 };
            continue;
        }
        if (!cur)
            continue;
        int k;
        Slot s = { 0 };
        if (!strncmp(line, "m ", 2) && sscanf(line, "m %d %255s %255s %255s %7s", &k, b, c, d, e) >= 4) {
            s.name = strdup(b);
            s.flag = e[0];
            s.ex.kind = M32_EX_FN;
            s.ex.lib = "(steamclient)";
            s.ex.name = s.name;
            s.ex.gnote = strdup(c);
            s.ex.hnote = strdup(d);
        } else if (!strncmp(line, "bad ", 4) && sscanf(line, "bad %d %255s", &k, b) == 2) {
            s.name = strdup(b);
            s.bad = 1;
        } else {
            continue;
        }
        if (k != cur->n)
            continue;
        cur->slots = realloc(cur->slots, (size_t)(cur->n + 1) * sizeof *cur->slots);
        cur->slots[cur->n++] = s;
    }
    fclose(f);
}

static const Iface *find_iface(const char *version)
{
    for (int i = 0; i < g_nifaces; i++)
        if (!strcmp(g_ifaces[i].version, version))
            return &g_ifaces[i];
    return NULL;
}

static int sp_bad(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    M32Export *e = m32_export_by_id((uint32_t)(cpu->rip - OCERZ_DYLDAPI_LO));
    if (steam_log())
        fprintf(stderr, "ocerz: m32: steam: %s, which the bridge cannot cross, was called (from %#x), answering 0\n",
                e ? e->name : "?", m32_rd((uint32_t)cpu->gpr[OCERZ_RSP]));
    m32_ret(cpu, 0, 0, 0);
    return OCERZ_STEP_OK;
}

static uint32_t proxy_for(void *host, const char *version);

/* the guest offset of the last pointer argument in a notation: an I method's version string */
static uint32_t last_ptr_arg(const char *gnote)
{
    const char *p = strchr(gnote, '(');
    uint32_t off = 4, at = 0;
    for (p = p ? p + 1 : ""; *p && *p != ')'; p++) {
        if (*p == 'p' || *p == 's')
            at = off;
        off += (*p == 'l' || *p == 'L' || *p == 'd') ? 8 : 4;
    }
    return at;
}

/* a C string Steam returns, copied into guest memory once per host string and content; game threads call in at once */
static uint32_t guest_string_locked(const char *s)
{
    typedef struct { const char *host; char *copy; uint32_t g; } Str;
    static Str *strs;
    static int n;
    for (int i = 0; i < n; i++)
        if (strs[i].host == s && !strcmp(strs[i].copy, s))
            return strs[i].g;
    size_t len = strlen(s) + 1;
    uint32_t g = m32_static_alloc((uint32_t)len, 1);
    if (!g)
        return 0;
    memcpy(m32_h(g), s, len);
    for (int i = 0; i < n; i++)
        if (strs[i].host == s) {
            free(strs[i].copy);
            strs[i].copy = strdup(s);
            strs[i].g = g;
            return g;
        }
    strs = realloc(strs, (size_t)(n + 1) * sizeof *strs);
    strs[n++] = (Str){ s, strdup(s), g };
    return g;
}
static uint32_t guest_string(const char *s)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    if (!s)
        return 0;
    pthread_mutex_lock(&lock);
    uint32_t g = guest_string_locked(s);
    pthread_mutex_unlock(&lock);
    return g;
}

/* an I or S method: the generic crossing, then its result wrapped or copied */
static int sp_flagged(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t id = (uint32_t)(cpu->rip - OCERZ_DYLDAPI_LO);
    Slot *s = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nflagged && !s; i++)
        if (g_flagged[i].id == id)
            s = g_flagged[i].slot;
    pthread_mutex_unlock(&g_lock);
    if (!s) {
        m32_ret(cpu, 0, 0, 0);
        return OCERZ_STEP_OK;
    }
    uint32_t esp = (uint32_t)cpu->gpr[OCERZ_RSP];
    uint32_t vat = s->flag == 'I' ? last_ptr_arg(s->ex.gnote) : 0;
    char version[128] = "";
    if (vat && m32_rd(esp + vat))
        snprintf(version, sizeof version, "%s", (const char *)m32_h(m32_rd(esp + vat)));
    int rc = m32_cross(vm, cpu, &s->ex);
    uint32_t r = (uint32_t)cpu->gpr[OCERZ_RAX];
    void *host = r ? m32_host(r) : NULL;
    cpu->gpr[OCERZ_RAX] = s->flag == 'I' ? proxy_for(host, version) : guest_string(host);
    return rc;
}

static uint32_t vtable_for(void **hostvt, const Iface *f)
{
    for (int i = 0; i < g_nvts; i++)
        if (g_vts[i].hostvt == hostvt && g_vts[i].iface == f)
            return g_vts[i].gvt;
    uint32_t gvt = m32_static_alloc(4u * (uint32_t)(f->n ? f->n : 1), 4);
    if (!gvt)
        return 0;
    for (int k = 0; k < f->n; k++) {
        Slot *s = &f->slots[k];
        uint32_t trap;
        if (s->bad) {
            char name[256];
            snprintf(name, sizeof name, "%s::%s", f->version, s->name);
            trap = m32_export_special(name, sp_bad);
        } else {
            s->ex.host_fn = hostvt[k];
            if (s->flag) {
                trap = m32_export_special(s->name, sp_flagged);
                g_flagged = realloc(g_flagged, (size_t)(g_nflagged + 1) * sizeof *g_flagged);
                g_flagged[g_nflagged++] = (Flagged){ trap - (uint32_t)OCERZ_DYLDAPI_LO, s };
            } else {
                trap = m32_export_hostfn(hostvt[k], s->ex.gnote, s->ex.hnote);
            }
        }
        m32_wr(gvt + 4u * (uint32_t)k, trap);
    }
    g_vts = realloc(g_vts, (size_t)(g_nvts + 1) * sizeof *g_vts);
    g_vts[g_nvts++] = (Vt){ f, hostvt, gvt };
    return gvt;
}

static uint32_t proxy_for(void *host, const char *version)
{
    if (!host)
        return 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nproxies; i++)
        if (g_proxies[i].host == host) {
            uint32_t g = g_proxies[i].guest;
            pthread_mutex_unlock(&g_lock);
            return g;
        }
    load_db();
    const Iface *f = find_iface(version);
    uint32_t g = 0, gvt = f ? vtable_for(*(void ***)host, f) : 0;
    if (gvt && (g = m32_static_alloc(8, 4))) {
        m32_wr(g, gvt);
        m32_alias(g, host);
        g_proxies = realloc(g_proxies, (size_t)(g_nproxies + 1) * sizeof *g_proxies);
        g_proxies[g_nproxies++] = (Proxy){ host, g };
    }
    pthread_mutex_unlock(&g_lock);
    if (steam_log() || !f)
        fprintf(stderr, "ocerz: m32: steam: %s %s\n", version, !f ? "is not in the interface database, answering 0"
                                                                  : g ? "bridged" : "could not be bridged");
    return g;
}

/* void *CreateInterface(const char *version, int *return_code) */
static int sp_CreateInterface(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    void *(*fn)(const char *, int *) = (void *(*)(const char *, int *))dlsym(g_client, "CreateInterface");
    uint32_t gv = m32_arg(cpu, 0), gr = m32_arg(cpu, 1);
    const char *version = gv ? (const char *)m32_h(gv) : "";
    int rc = 0;
    void *host = fn ? fn(version, &rc) : NULL;
    if (gr)
        m32_wr(gr, (uint32_t)rc);
    m32_ret(cpu, proxy_for(host, version), 0, 0);
    return OCERZ_STEP_OK;
}

/* bool Steam_BGetCallback(HSteamPipe, CallbackMsg_t *): the i386 message is {user, id, param, size}, 16 bytes */
static int sp_BGetCallback(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    struct { int32_t user, id; uint8_t *param; int32_t size; } m = { 0 };
    static uint32_t buf, cap;
    int (*fn)(int, void *) = (int (*)(int, void *))dlsym(g_client, "Steam_BGetCallback");
    uint32_t gm = m32_arg(cpu, 1);
    int r = fn ? fn((int)m32_arg(cpu, 0), &m) : 0;
    if (r && gm) {
        uint32_t size = m.size > 0 ? (uint32_t)m.size : 0;
        if (size > cap) {
            cap = size < 4096 ? 4096 : size * 2;
            buf = m32_static_alloc(cap, 8);
        }
        if (size && buf && m.param)
            memcpy(m32_h(buf), m.param, size);
        m32_wr(gm, (uint32_t)m.user);
        m32_wr(gm + 4, (uint32_t)m.id);
        m32_wr(gm + 8, buf);
        m32_wr(gm + 12, size);
    }
    m32_ret(cpu, (uint32_t)(r & 0xff), 0, 0);
    return OCERZ_STEP_OK;
}

static int sp_nothing(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    m32_ret(cpu, 0, 0, 0);
    return OCERZ_STEP_OK;
}

uint32_t m32_steam_dlopen(const char *path)
{
    const char *leaf = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    if (strcmp(leaf, "steamclient.dylib") != 0 || m32_is_i386(path) || getenv("OCERZ_NO_M32_STEAM"))
        return 0;
    pthread_mutex_lock(&g_lock);
    if (!g_client)
        g_client = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (g_client && !g_handle && (g_handle = m32_static_alloc(8, 4)))
        m32_wr(g_handle, STEAM_MAGIC);
    pthread_mutex_unlock(&g_lock);
    if (!g_client)
        fprintf(stderr, "ocerz: m32: steam: cannot open %s: %s\n", path, dlerror());
    else if (steam_log())
        fprintf(stderr, "ocerz: m32: steam: %s opened as the host's client\n", path);
    return g_client ? g_handle : 0;
}

uint32_t m32_steam_dlsym(uint32_t handle, const char *sym)
{
    if (!handle || handle != g_handle)
        return 0;
    static const struct { const char *name; M32Special fn; const char *note; } x[] = {
        { "_CreateInterface", sp_CreateInterface, NULL },
        { "_Steam_BGetCallback", sp_BGetCallback, NULL },
        { "_Steam_FreeLastCallback", NULL, "B(i)" },
        { "_Steam_GetAPICallResult", NULL, "B(iLpiip)" },
        { "_Steam_ReleaseThreadLocalMemory", NULL, "v(B)" },
        { "_Breakpad_SteamMiniDumpInit", sp_nothing, NULL },
        { "_Breakpad_SteamSetAppID", sp_nothing, NULL },
        { "_Breakpad_SteamSetSteamID", sp_nothing, NULL },
        { "_Breakpad_SteamWriteMiniDumpSetComment", sp_nothing, NULL },
        { "_Breakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId", sp_nothing, NULL },
    };
    static uint32_t made[sizeof x / sizeof x[0]];
    for (size_t i = 0; i < sizeof x / sizeof x[0]; i++) {
        if (strcmp(sym, x[i].name) != 0)
            continue;
        if (x[i].fn)
            return made[i] ? made[i] : (made[i] = m32_export_special(x[i].name, x[i].fn));
        void *fn = dlsym(g_client, x[i].name + 1);
        return fn ? m32_export_hostfn(fn, x[i].note, x[i].note) : 0;
    }
    if (steam_log())
        fprintf(stderr, "ocerz: m32: steam: no bridge for %s\n", sym);
    return 0;
}

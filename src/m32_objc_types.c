/*
 * Objective-C type encodings for m32 (include/ocerz/m32_objc.h).
 *
 * m32_encoding_notation turns an encoding ("v24@0:4{_NSRect={_NSPoint=ff}{_NSSize=ff}}8") into m32's notation
 * ("v(@:{ffff})") for either side: the letters mean the same on i386 and arm64 except that 'l' is always a 32-bit
 * long in an encoding (LP64 spells long 'q'), which is what makes the two encodings of one method pair up.
 *
 * m32_objc_guest_encoding answers the i386 encoding of a host class's method from runtime/apis32's .objc32 files
 * (sdkgen --guest i386): the class, its superclasses, then their protocols.
 */
#include <dirent.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ocerz/m32_objc.h"
#include "ocerz/m32_sig.h"
#include "ocerz/m32_objcrt.h"

static const char *skip_quals(const char *e)
{
    while (*e && strchr("rnNoORVA+", *e))
        e++;
    return e;
}

static const char *skip_digits(const char *e)
{
    while (*e == '-' || (*e >= '0' && *e <= '9'))
        e++;
    return e;
}

/* one type; returns the rest of the encoding, NULL on something m32 cannot carry */
static const char *one(const char *e, char *out, size_t *o, size_t n, int in_struct)
{
    e = skip_quals(e);
#define PUT(c) do { if (*o + 1 >= n) return NULL; out[(*o)++] = (c); } while (0)
    char c = *e++;
    switch (c) {
    case 'c': PUT('b'); break;
    case 'C': PUT('B'); break;
    case 's': PUT('h'); break;
    case 'S': PUT('H'); break;
    case 'i': case 'l': PUT('i'); break;
    case 'I': case 'L': PUT('u'); break;
    case 'q': PUT('l'); break;
    case 'Q': PUT('L'); break;
    case 'f': PUT('f'); break;
    case 'd': PUT('d'); break;
    case 'B': PUT('B'); break;
    case 'v': PUT('v'); break;
    case '*': PUT('s'); break;
    case '#': PUT('#'); break;
    case ':': PUT(':'); break;
    case '?': PUT('p'); break;
    case '@':
        if (*e == '?') {   /* a block, possibly with its signature in <...> */
            e++;
            if (*e == '<') {
                int d = 1;
                for (e++; *e && d; e++) d += *e == '<' ? 1 : *e == '>' ? -1 : 0;
            }
            PUT('k');
        } else {
            if (*e == '"')
                for (e++; *e && *e != '"'; e++) ;
            if (*e == '"')
                e++;
            PUT('@');
        }
        break;
    case '^': {   /* a pointer: skip what it points at */
        char tmp[512];
        size_t t = 0;
        if (*e == '?')
            e++;
        else if (*e == '{' || *e == '(' || *e == '[') {
            char open = *e, close = open == '{' ? '}' : open == '(' ? ')' : ']';
            int d = 0;
            do { d += *e == open ? 1 : *e == close ? -1 : 0; e++; } while (*e && d);
        } else if (!(e = one(e, tmp, &t, sizeof tmp, 0)))
            return NULL;
        PUT('p');
        break;
    }
    case '[': {   /* arrays: by value only inside a structure, as that many members */
        unsigned count = (unsigned)strtoul(e, (char **)&e, 10);
        size_t start = *o;
        if (!in_struct || count > 16)
            return NULL;
        const char *elem = e;
        for (unsigned i = 0; i < count; i++)
            if (!(e = one(elem, out, o, n, 1)))
                return NULL;
        (void)start;
        if (*e != ']')
            return NULL;
        e++;
        break;
    }
    case '{': {
        while (*e && *e != '=' && *e != '}')
            e++;
        if (*e != '=')
            return NULL;   /* an opaque structure by value */
        e++;
        PUT('{');
        while (*e && *e != '}') {
            if (*e == '"') {   /* member names */
                for (e++; *e && *e != '"'; e++) ;
                e++;
                continue;
            }
            if (!(e = one(e, out, o, n, 1)))
                return NULL;
        }
        if (*e != '}')
            return NULL;
        e++;
        PUT('}');
        break;
    }
    default:   /* unions, bitfields, long double, vectors */
        return NULL;
    }
#undef PUT
    return skip_digits(e);
}

int m32_encoding_notation(const char *enc, int guest, char *out, size_t n)
{
    (void)guest;   /* the letters carry the size difference: 'l' 32-bit, 'q' 64-bit, '@' a pointer each side */
    size_t o = 0;
    const char *e = one(enc, out, &o, n, 0);
    if (!e || o + 2 >= n)
        return -1;
    out[o++] = '(';
    while (*e) {
        if (!(e = one(e, out, &o, n, 0)) || o + 2 >= n)
            return -1;
    }
    out[o++] = ')';
    out[o] = 0;
    return 0;
}

/* whether the encoding at e, qualifiers and all, is a plain id * ("^@", "o^@"; not a const one, not a block's) */
static int plain_id_ptr(const char *e)
{
    for (; *e && strchr("rnNoORVA+", *e); e++)
        if (*e == 'r')
            return 0;
    return e[0] == '^' && e[1] == '@' && e[2] != '?';
}

/* a pointer to one structure with members ("^{_NSRange=II}"): its notation ("{uu}") in out, 'r' when it is const
 * (only read), 'R' when the callee may write it, 0 for anything else */
static char struct_ptr(const char *e, char *out, size_t n)
{
    char cls = 'R';
    for (; *e && strchr("rnNoORVA+", *e); e++)
        if (*e == 'r')
            cls = 'r';
    size_t o = 0;
    if (e[0] != '^' || e[1] != '{' || !one(e + 1, out, &o, n, 0) || o < 3 || o + 1 >= n)
        return 0;
    out[o] = 0;
    int members = 0;   /* a notation's structure holds at most M32_MAX_MEMBERS: a larger one stays a pointer */
    for (const char *p = out; *p; p++)
        members += *p != '{' && *p != '}';
    return members <= M32_MAX_MEMBERS ? cls : 0;
}

int m32_notation_pointers(const char *sel, const char *genc, const char *henc, char *gn, size_t gsize, char *hn,
                          size_t hsize)
{
    char go[1024], ho[1024];
    size_t gl = 0, hl = 0;
    const char *ge = genc, *he = henc;
    for (int part = -3; *ge && *he; part++) {   /* the return value, self and _cmd come before the selector's parts */
        char gt[512], ht[512];
        size_t g1 = 0, h1 = 0;
        const char *gnext = one(ge, gt, &g1, sizeof gt - 2, 0), *hnext = one(he, ht, &h1, sizeof ht - 2, 0);
        if (!gnext || !hnext)
            return -1;
        gt[g1] = ht[h1] = 0;
        if (part >= 0 && !strcmp(gt, "p") && !strcmp(ht, "p")) {
            const char *s = sel;
            for (int k = 0; k < part && s; k++)
                s = strchr(s, ':') ? strchr(s, ':') + 1 : NULL;
            size_t len = s && strchr(s, ':') ? (size_t)(strchr(s, ':') - s) : 0;
            char name[256], gs[400], hs[400];
            snprintf(name, sizeof name, "%.*s", (int)len, s ? s : "");
            char gc = struct_ptr(ge, gs, sizeof gs), hc = struct_ptr(he, hs, sizeof hs);
            int plural = strstr(name, "bjects") || strstr(name, "eys") || strstr(name, "oints") || strstr(name, "ects");
            if (!len || plural)
                ;   /* an array of them: their count is another argument */
            else if (plain_id_ptr(ge) && plain_id_ptr(he))
                snprintf(gt, sizeof gt, "%c", strstr(name, "alidateValue") ? 'O' : 'E');   /* validateValue: reads it */
            else if (gc && hc) {
                snprintf(gt, sizeof gt, "%c%s", gc, gs);
                snprintf(ht, sizeof ht, "%c%s", gc, hs);
            }
        }
        size_t a = strlen(gt), b = strlen(ht);
        if (gl + a + 2 >= sizeof go || hl + b + 2 >= sizeof ho)
            return -1;
        memcpy(go + gl, gt, a);
        memcpy(ho + hl, ht, b);
        gl += a;
        hl += b;
        if (part == -3) {
            go[gl++] = '(';
            ho[hl++] = '(';
        }
        ge = gnext;
        he = hnext;
    }
    if (*ge || *he || gl + 2 > gsize || hl + 2 > hsize)
        return -1;
    go[gl++] = ')';
    ho[hl++] = ')';
    memcpy(gn, go, gl);
    gn[gl] = 0;
    memcpy(hn, ho, hl);
    hn[hl] = 0;
    return 0;
}

/* ---- the i386 method database ---- */

typedef struct Rec { char *key; char *enc; int variadic; } Rec;   /* key: "Class -sel" */
static Rec *g_recs;
static size_t g_n, g_cap;
static int g_loaded;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int cmp_rec(const void *a, const void *b) { return strcmp(((const Rec *)a)->key, ((const Rec *)b)->key); }

static void load(void)
{
    g_loaded = 1;
    const char *env = getenv("OCERZ_APIDB32"), *apidb = getenv("OCERZ_APIDB");
    char root[1100] = "";
    if (env && *env)
        snprintf(root, sizeof root, "%s/macos", env);
    else if (apidb && *apidb)
        snprintf(root, sizeof root, "%s32/macos", apidb);
    else {
        extern int _NSGetExecutablePath(char *, uint32_t *);
        char exe[1024], real[1024];
        uint32_t sz = sizeof exe;
        if (_NSGetExecutablePath(exe, &sz) == 0 && realpath(exe, real) && strrchr(real, '/')) {
            *strrchr(real, '/') = 0;
            snprintf(root, sizeof root, "%s/runtime/apis32/macos", real);
        }
    }
    DIR *d = opendir(root);
    char best[64] = "";
    for (struct dirent *e; d && (e = readdir(d));)
        if (e->d_name[0] != '.' && strcmp(e->d_name, best) > 0 && strlen(e->d_name) < sizeof best)
            snprintf(best, sizeof best, "%s", e->d_name);
    if (d)
        closedir(d);
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/%s", root, best);
    d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d));) {
        size_t l = strlen(e->d_name);
        if (l < 7 || strcmp(e->d_name + l - 7, ".objc32"))
            continue;
        char path[1500];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "r");
        char line[2048];
        while (f && fgets(line, sizeof line, f)) {
            char cls[256], sel[1024], enc[1024], flag[32] = "";
            if (sscanf(line, "m %255s %1023s %1023s %31s", cls, sel, enc, flag) < 3)
                continue;
            if (g_n == g_cap) {
                g_cap = g_cap ? g_cap * 2 : 16384;
                g_recs = realloc(g_recs, g_cap * sizeof *g_recs);
            }
            size_t kl = strlen(cls) + strlen(sel) + 2;
            g_recs[g_n].key = malloc(kl);
            snprintf(g_recs[g_n].key, kl, "%s %s", cls, sel);
            g_recs[g_n].enc = strdup(enc);
            g_recs[g_n].variadic = !strcmp(flag, "variadic");
            g_n++;
        }
        if (f)
            fclose(f);
    }
    if (d)
        closedir(d);
    qsort(g_recs, g_n, sizeof *g_recs, cmp_rec);
}

static __thread int t_variadic;

static const char *find(const char *cls, char kind, const char *sel)
{
    char key[1400];
    snprintf(key, sizeof key, "%s %c%s", cls, kind, sel);
    Rec k = { key, NULL, 0 };
    Rec *r = g_n ? bsearch(&k, g_recs, g_n, sizeof *g_recs, cmp_rec) : NULL;
    if (r)
        t_variadic = r->variadic;
    return r ? r->enc : NULL;
}

int m32_objc_last_variadic(void) { return t_variadic; }

/* Where today's SDK headers describe i386 differently from the i386 frameworks games linked against: the event
 * masks of nextEventMatchingMask: and discardEventsMatchingMask: became unsigned long long (NSEventMask) in the 10.12
 * SDK, while i386 AppKit kept reading a 32-bit NSUInteger (10.9 games push 4 bytes).  The first 'Q' of these
 * selectors is 'I' on i386.  The event monitors (add{Local,Global}MonitorForEventsMatchingMask:handler:) took an
 * NSEventMask from the start (10.6): Batman, built with the 10.9 SDK, pushes 8 bytes, and reading 4 handed AppKit the
 * mask's zero high word as the handler - a NULL block every key event then called (the 0x10 crash). */
static const char *const g_mask_selectors[] = { "EventMatchingMask:", "EventsMatchingMask:", NULL };

static const char *drift(const char *sel, const char *enc)
{
    static __thread char buf[1024];
    if (strstr(sel, "MonitorForEventsMatchingMask:"))
        return enc;
    for (int i = 0; enc && g_mask_selectors[i]; i++)
        if (strstr(sel, g_mask_selectors[i]) && strchr(enc, 'Q')) {
            snprintf(buf, sizeof buf, "%s", enc);
            *strchr(buf, 'Q') = 'I';
            return buf;
        }
    return enc;
}

static const char *guest_encoding(void *host_cls, const char *sel, int is_class);

/* The answer for a class and a selector does not change once the headers are loaded, and sends ask it again and
 * again: a method the class's own records have took about 200 ns, one only a protocol declares (or none, as for
 * -self) about 5.6 us, a name formatted and looked up for every protocol of every class up the chain.  Each thread
 * keeps its last answers, misses too, by class, selector and kind (ponytail: a protocol added to a class after it
 * was asked about is not seen; m32 adds a paired class's protocols before its first send).
 * OCERZ_NO_M32_ENCODING_CACHE=1 looks every send up. */
typedef struct EncHit { void *cls; const char *sel; const char *enc; int is_class, variadic; } EncHit;
#define ENC_CACHE 256
static __thread EncHit t_enc[ENC_CACHE];

const char *m32_objc_guest_encoding(void *host_cls, const char *sel, int is_class)
{
    static int off = -1;
    if (off < 0)
        off = getenv("OCERZ_NO_M32_ENCODING_CACHE") != NULL;
    if (off || !host_cls || !sel)
        return drift(sel, guest_encoding(host_cls, sel, is_class));
    EncHit *h = &t_enc[(((uintptr_t)host_cls >> 4) ^ ((uintptr_t)sel >> 2) ^ (uintptr_t)is_class) & (ENC_CACHE - 1)];
    if (h->cls == host_cls && h->is_class == is_class && (h->sel == sel || !strcmp(h->sel, sel))) {
        t_variadic = h->variadic;
        return drift(sel, h->enc);
    }
    const char *e = guest_encoding(host_cls, sel, is_class);
    *h = (EncHit){ host_cls, sel, e, is_class, t_variadic };
    return drift(sel, e);
}

static const char *guest_encoding(void *host_cls, const char *sel, int is_class)
{
    t_variadic = 0;
    pthread_mutex_lock(&g_lock);
    if (!g_loaded)
        load();
    pthread_mutex_unlock(&g_lock);
    char kind = is_class ? '+' : '-';
    for (Class c = host_cls; c; c = class_getSuperclass(c)) {
        const char *e = find(class_getName(c), kind, sel);
        if (e)
            return e;
    }
    for (Class c = host_cls; c; c = class_getSuperclass(c)) {
        unsigned n = 0;
        Protocol **ps = class_copyProtocolList(c, &n);
        for (unsigned i = 0; i < n; i++) {
            char pname[300];
            snprintf(pname, sizeof pname, "@%s", protocol_getName(ps[i]));
            const char *e = find(pname, kind, sel);
            if (e) {
                free(ps);
                return e;
            }
        }
        free(ps);
    }
    return NULL;
}

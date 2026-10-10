/*
 * m32's i386 API database (runtime/apis32/macos/<version>/<leaf>.api, written by
 * tools/sdkgen.sh --guest i386).  Records:
 *
 *     fn32 <export> <host symbol> <guest notation> <host notation>
 *     data32 <export> <host symbol> <guest size> <host size> ptr|int|blob
 *     bad32 <export> <reason>
 *
 * A library's file is read the first time something is looked up in it and kept for the life of the process.
 * i386 import names carry variant suffixes the x86_64 export list does not ($UNIX2003, $INODE64, $DARWIN_EXTSN,
 * $NOCANCEL): lookups strip everything from the first '$'.
 */
#include <dirent.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ocerz/m32.h"
#include "ocerz/m32_db.h"
#include "ocerz/types.h"

typedef struct Lib {
    struct Lib *next;
    char leaf[256];
    M32DbRec *recs;
    int n;
} Lib;

static Lib *g_libs;
static char g_dir[PATH_MAX];
static int g_dir_chosen;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void choose_dir(void)
{
    if (g_dir_chosen)
        return;
    g_dir_chosen = 1;
    char root[PATH_MAX] = "";
    const char *env = getenv("OCERZ_APIDB32"), *apidb = getenv("OCERZ_APIDB");
    if (env && *env)
        snprintf(root, sizeof root, "%s", env);
    else if (apidb && *apidb)
        snprintf(root, sizeof root, "%s32", apidb);   /* .../AArchX/apis -> .../AArchX/apis32 */
    else {
        char exe[PATH_MAX], real[PATH_MAX];
        uint32_t sz = sizeof exe;
        if (_NSGetExecutablePath(exe, &sz) == 0 && realpath(exe, real) && strrchr(real, '/')) {
            *strrchr(real, '/') = 0;
            snprintf(root, sizeof root, "%s/runtime/apis32", real);
        }
    }
    char macos[PATH_MAX + 8];
    snprintf(macos, sizeof macos, "%s/macos", root);
    DIR *d = opendir(macos);
    if (!d) {
        fprintf(stderr, "ocerz: m32: no i386 API database at %s (make apis32)\n", macos);
        return;
    }
    char best[64] = "";
    for (struct dirent *e; (e = readdir(d));)   /* ponytail: newest by string order; fine while only one SDK is present */
        if (e->d_name[0] != '.' && strcmp(e->d_name, best) > 0 && strlen(e->d_name) < sizeof best)
            snprintf(best, sizeof best, "%s", e->d_name);
    closedir(d);
    if (best[0])
        snprintf(g_dir, sizeof g_dir, "%s/%s", macos, best);
}

static int cmp_rec(const void *a, const void *b)
{
    return strcmp(((const M32DbRec *)a)->name, ((const M32DbRec *)b)->name);
}

static Lib *load_lib(const char *leaf)
{
    for (Lib *l = g_libs; l; l = l->next)
        if (!strcmp(l->leaf, leaf))
            return l;
    choose_dir();
    Lib *l = calloc(1, sizeof *l);
    snprintf(l->leaf, sizeof l->leaf, "%s", leaf);
    l->next = g_libs;
    g_libs = l;
    if (!g_dir[0])
        return l;
    char path[PATH_MAX + 300];
    snprintf(path, sizeof path, "%s/%s.api", g_dir, leaf);
    FILE *f = fopen(path, "r");
    if (!f)
        return l;
    int cap = 0;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        char *w[8];
        int nw = 0;
        for (char *t = strtok(line, " \t\n"); t && nw < 8; t = strtok(NULL, " \t\n"))
            w[nw++] = t;
        if (nw < 3)
            continue;
        M32DbRec r;
        memset(&r, 0, sizeof r);
        if (!strcmp(w[0], "fn32") && nw == 5) {
            r.kind = M32_DB_FN;
            r.guest = strdup(w[3]);
            r.host = strdup(w[4]);
        } else if (!strcmp(w[0], "data32") && nw == 6) {
            r.kind = M32_DB_DATA;
            r.guest_size = atoi(w[3]);
            r.host_size = atoi(w[4]);
            r.data_kind = strdup(w[5]);
        } else if (!strcmp(w[0], "bad32") && nw == 3) {
            r.kind = M32_DB_BAD;
            r.reason = strdup(w[2]);
        } else
            continue;
        r.name = strdup(w[1]);
        if (r.kind != M32_DB_BAD)
            r.host_symbol = strdup(w[2]);
        if (l->n == cap) {
            cap = cap ? cap * 2 : 1024;
            l->recs = realloc(l->recs, (size_t)cap * sizeof *l->recs);
        }
        l->recs[l->n++] = r;
    }
    fclose(f);
    qsort(l->recs, (size_t)l->n, sizeof *l->recs, cmp_rec);
    return l;
}

static const M32DbRec *find(Lib *l, const char *name)
{
    M32DbRec key = { .name = (char *)name };
    return l->n ? bsearch(&key, l->recs, (size_t)l->n, sizeof *l->recs, cmp_rec) : NULL;
}

/* Libraries whose files a flat lookup, or a symbol a library re-exports, falls back to. */
static const char *const g_fallback[] = { "libSystem.B.dylib", "CoreFoundation", "Foundation", "AppKit",
                                          "libobjc.A.dylib", "ApplicationServices", "CoreServices", "OpenGL" };

int m32_db_lookup(const char *install_name, const char *symbol, M32DbRec *out)
{
    char base[512];
    snprintf(base, sizeof base, "%s", symbol);
    char *dollar = strchr(base, '$');
    if (dollar)
        *dollar = 0;
    const char *leaf = install_name ? strrchr(install_name, '/') : NULL;
    leaf = leaf ? leaf + 1 : install_name;
    pthread_mutex_lock(&g_lock);
    const M32DbRec *r = leaf ? find(load_lib(leaf), base) : NULL;
    for (size_t i = 0; !r && i < sizeof g_fallback / sizeof g_fallback[0]; i++)
        r = find(load_lib(g_fallback[i]), base);
    if (!r) {   /* an umbrella re-exports its parts (ApplicationServices: CoreGraphics, CoreText...): every file */
        choose_dir();
        DIR *d = g_dir[0] ? opendir(g_dir) : NULL;
        for (struct dirent *e; !r && d && (e = readdir(d));) {
            size_t l = strlen(e->d_name);
            if (l > 4 && !strcmp(e->d_name + l - 4, ".api")) {
                char lf[256];
                snprintf(lf, sizeof lf, "%.*s", (int)(l - 4), e->d_name);
                r = find(load_lib(lf), base);
            }
        }
        if (d)
            closedir(d);
    }
    pthread_mutex_unlock(&g_lock);
    if (!r)
        return 0;
    *out = *r;
    out->variant = dollar ? strchr(symbol, '$') : NULL;
    return 1;
}

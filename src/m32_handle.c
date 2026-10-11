/*
 * m32's pointer rule (include/ocerz/m32.h).
 *
 * The guest never holds a host address.  A host pointer inside the window is the guest address it already is; a
 * host object with a guest twin (a guest ObjC object, a guest CFConstantString, a guest block) is that twin; any
 * other host pointer gets an 8-byte handle cell in [M32_HANDLE_LO, M32_HANDLE_HI): word 0 is an isa word the
 * Objective-C layer fills (0 until then), word 1 the cell's index.  Reading through a handle therefore sees a sane
 * isa and nothing else of the host object.
 *
 * ponytail: one global lock and handles that are never freed; the per-minute log line counts them, and per-object
 * lifetime (associated-object dealloc hooks) comes in if the count grows per frame.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ocerz/m32.h"

typedef struct Map { uint64_t *key; uint32_t *val; uint32_t cap, n; } Map;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void **g_host;          /* cell index -> host pointer */
static uint32_t g_ncells, g_cap;
static Map g_by_host;          /* host pointer -> cell index */
static Map g_alias;            /* guest address -> host pointer (as index into g_alias_host) */
static Map g_alias_rev;        /* host pointer -> guest address */
static void **g_alias_host;
static uint32_t g_nalias, g_alias_cap;
static volatile int g_have_alias;

static uint32_t mix(uint64_t k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33; return (uint32_t)k; }

static int map_get(const Map *m, uint64_t k, uint32_t *v)
{
    if (!m->cap)
        return 0;
    for (uint32_t i = mix(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (!m->key[i])
            return 0;
        if (m->key[i] == k) {
            *v = m->val[i];
            return 1;
        }
    }
}

static void map_put(Map *m, uint64_t k, uint32_t v)
{
    if ((m->n + 1) * 2 > m->cap) {
        Map g = { 0 };
        g.cap = m->cap ? m->cap * 2 : 4096;
        g.key = calloc(g.cap, sizeof *g.key);
        g.val = calloc(g.cap, sizeof *g.val);
        for (uint32_t i = 0; i < m->cap; i++)
            if (m->key[i])
                map_put(&g, m->key[i], m->val[i]);
        free(m->key);
        free(m->val);
        *m = g;
    }
    uint32_t i = mix(k) & (m->cap - 1);
    while (m->key[i] && m->key[i] != k)
        i = (i + 1) & (m->cap - 1);
    if (!m->key[i])
        m->n++;
    m->key[i] = k;
    m->val[i] = v;
}

int m32_is_handle(uint32_t g)
{
    return g >= M32_HANDLE_LO && g < M32_HANDLE_LO + 8 * g_ncells && !(g & 7);
}

void *m32_host(uint32_t g)
{
    if (!g)
        return NULL;
    if (g >= M32_HANDLE_LO && g < M32_HANDLE_HI) {
        uint32_t i = (g - M32_HANDLE_LO) / 8;
        return i < g_ncells && !(g & 7) ? g_host[i] : NULL;
    }
    if (g_have_alias) {
        uint32_t v;
        pthread_mutex_lock(&g_lock);
        int hit = map_get(&g_alias, g, &v);
        void *h = hit ? g_alias_host[v] : NULL;
        pthread_mutex_unlock(&g_lock);
        if (hit)
            return h;
    }
    return m32_h(g);
}

uint32_t m32_handle(void *host)
{
    if (!host)
        return 0;
    if (m32_in_window(host))
        return m32_g(host);
    uint32_t v;
    pthread_mutex_lock(&g_lock);
    if (g_have_alias && map_get(&g_alias_rev, (uint64_t)(uintptr_t)host, &v)) {
        pthread_mutex_unlock(&g_lock);
        return v;
    }
    if (!map_get(&g_by_host, (uint64_t)(uintptr_t)host, &v)) {
        if (g_ncells >= (M32_HANDLE_HI - M32_HANDLE_LO) / 8) {
            pthread_mutex_unlock(&g_lock);
            fprintf(stderr, "ocerz: m32: out of handles\n");
            return 0;
        }
        if (g_ncells == g_cap) {
            g_cap = g_cap ? g_cap * 2 : 4096;
            g_host = realloc(g_host, g_cap * sizeof *g_host);
        }
        v = g_ncells;
        g_host[v] = host;
        m32_wr(M32_HANDLE_LO + 8 * v, 0);
        m32_wr(M32_HANDLE_LO + 8 * v + 4, v);
        __atomic_store_n(&g_ncells, v + 1, __ATOMIC_RELEASE);
        map_put(&g_by_host, (uint64_t)(uintptr_t)host, v);
    }
    pthread_mutex_unlock(&g_lock);
    return M32_HANDLE_LO + 8 * v;
}

/* the guest object aliased to host, or 0 (no handle is made) */
uint32_t m32_handle_twin_lookup(void *host)
{
    uint32_t v = 0;
    if (!g_have_alias)
        return 0;
    pthread_mutex_lock(&g_lock);
    if (!map_get(&g_alias_rev, (uint64_t)(uintptr_t)host, &v))
        v = 0;
    pthread_mutex_unlock(&g_lock);
    return v;
}

void m32_alias(uint32_t g, void *host)
{
    pthread_mutex_lock(&g_lock);
    if (g_nalias == g_alias_cap) {
        g_alias_cap = g_alias_cap ? g_alias_cap * 2 : 1024;
        g_alias_host = realloc(g_alias_host, g_alias_cap * sizeof *g_alias_host);
    }
    g_alias_host[g_nalias] = host;
    map_put(&g_alias, g, g_nalias++);
    map_put(&g_alias_rev, (uint64_t)(uintptr_t)host, g);
    g_have_alias = 1;
    pthread_mutex_unlock(&g_lock);
}

uint32_t m32_handle_count(void)
{
    return g_ncells;
}

/* A C string for the guest: guest memory stays where it is, host memory is copied once per distinct string. */
uint32_t m32_cstring(const char *host)
{
    if (!host)
        return 0;
    if (m32_in_window(host))
        return m32_g(host);
    static Map pool;   /* content hash -> guest copy; collisions are checked by content */
    uint64_t h = 1469598103934665603ull;
    for (const char *p = host; *p; p++)
        h = (h ^ (uint8_t)*p) * 1099511628211ull;
    h |= 1;
    uint32_t g;
    pthread_mutex_lock(&g_lock);
    for (;; h += 2) {
        if (!map_get(&pool, h, &g))
            break;
        if (!strcmp((const char *)m32_h(g), host)) {
            pthread_mutex_unlock(&g_lock);
            return g;
        }
    }
    size_t n = strlen(host) + 1;
    g = m32_static_alloc((uint32_t)n + 16, 4);   /* slack past the NUL: scanners peek at p[1] (a GL extension list) */
    if (g) {
        memcpy(m32_h(g), host, n);
        map_put(&pool, h, g);
    }
    pthread_mutex_unlock(&g_lock);
    return g;
}

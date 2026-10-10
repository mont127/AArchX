/*
 * m32's window memory (include/ocerz/m32.h).
 *
 * m32_static_alloc hands out zeroed guest memory that lives for the whole run (data import variables, pooled
 * strings, per-image tables); it grows in 1 MB chunks from the window's arena.
 */
#include <malloc/malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "ocerz/m32.h"
#include "ocerz/types.h"

static pthread_mutex_t g_static_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct Pool { uint32_t next, end; } Pool;
static Pool g_data, g_code;

static uint32_t pool_alloc(Pool *p, uint32_t size, uint32_t align)
{
    if (align < 4)
        align = 4;
    pthread_mutex_lock(&g_static_lock);
    uint32_t at = (p->next + align - 1) & ~(align - 1);
    if (!p->next || at + size > p->end) {
        uint32_t chunk = size + align > (1u << 20) ? (size + align + 0x3fff) & ~0x3fffu : 1u << 20;
        uint64_t base = ocerz_map_anywhere(chunk, PROT_READ | PROT_WRITE);
        if (!base || base + chunk > M32_HANDLE_LO) {
            pthread_mutex_unlock(&g_static_lock);
            fprintf(stderr, "ocerz: m32: window full (static allocation of %u bytes)\n", (unsigned)size);
            return 0;
        }
        p->next = (uint32_t)base;
        p->end = (uint32_t)base + chunk;
        at = (p->next + align - 1) & ~(align - 1);
    }
    p->next = at + size;
    pthread_mutex_unlock(&g_static_lock);
    return at;
}

uint32_t m32_static_alloc(uint32_t size, uint32_t align) { return pool_alloc(&g_data, size, align); }

/* i386 code m32 generates gets pages of its own: the JIT write-protects a page holding translated code it believes
 * writable, so a data cell beside it (a thread's errno) would fault and drop the translations at every store */
uint32_t m32_static_code(uint32_t size) { return pool_alloc(&g_code, size, 16); }

/* ---- the guest heap: one dlmalloc mspace whose segments come from the window as it grows ----
 * A fixed block would take its size out of the 4 GB address space up front whether the guest uses it or not (a game
 * with its own malloc zone barely does).  Each segment is marked in a bitmap of 16 KB granules, which is how free and
 * malloc_size tell the heap's pointers from the guest zone's. */

static uint8_t g_heap_map[(1u << 18) / 8];   /* one bit per 16 KB of the window: map_anywhere's granule, so a guest
                                                * zone's block never shares a bit with a heap segment */
static int in_heap(uint32_t g) { return (g_heap_map[g >> 17] >> ((g >> 14) & 7)) & 1; }
static void heap_mark(uint32_t g, size_t n, int on)
{
    for (uint64_t a = g & ~0x3fffu; a < (uint64_t)g + n; a += 0x4000) {
        uint32_t i = (uint32_t)(a >> 14);
        if (on)
            __atomic_or_fetch(&g_heap_map[i >> 3], (uint8_t)(1u << (i & 7)), __ATOMIC_RELAXED);
        else
            __atomic_and_fetch(&g_heap_map[i >> 3], (uint8_t)~(1u << (i & 7)), __ATOMIC_RELAXED);
    }
}
static void *heap_mmap(size_t n)
{
    uint64_t g = ocerz_map_anywhere(n, PROT_READ | PROT_WRITE);
    if (!g || g + n > M32_HANDLE_LO)
        return (void *)~(size_t)0;
    heap_mark((uint32_t)g, n, 1);
    return m32_h((uint32_t)g);
}
static int heap_munmap(void *p, size_t n)
{
    heap_mark(m32_g(p), n, 0);
    ocerz_unmap(m32_g(p), n);
    return 0;
}

#define ONLY_MSPACES 1
#define MSPACES 1
#define USE_LOCKS 1
#define HAVE_MMAP 1
#define HAVE_MREMAP 0
#define HAVE_MORECORE 0
#define MMAP(s) heap_mmap(s)
#define DIRECT_MMAP(s) heap_mmap(s)
#define MUNMAP(a, s) heap_munmap((a), (s))
#define DLMALLOC_EXPORT static
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "m32_dlmalloc.h"
#pragma clang diagnostic pop

static mspace g_heap;

int m32_heap_init(void)
{
    const char *mb = getenv("OCERZ_M32_HEAP_MB");   /* the first segment; later ones come as needed */
    size_t size = (size_t)(mb && atoi(mb) > 0 ? (uint32_t)atoi(mb) : 16u) << 20;
    g_heap = create_mspace(size, 1);
    if (!g_heap) {
        fprintf(stderr, "ocerz: m32: no room for a %u MB guest heap\n", (unsigned)(size >> 20));
        return -1;
    }
    return 0;
}
static uint32_t to_guest(void *p) { return p ? m32_g(p) : 0; }

uint32_t m32_malloc(uint32_t n) { return to_guest(mspace_malloc(g_heap, n ? n : 1)); }
uint32_t m32_calloc(uint32_t n, uint32_t size)
{
    uint64_t total = (uint64_t)n * size;
    return total > 0xffffffffu ? 0 : to_guest(mspace_calloc(g_heap, n ? n : 1, size ? size : 1));
}
uint32_t m32_memalign(uint32_t align, uint32_t n) { return to_guest(mspace_memalign(g_heap, align, n ? n : 1)); }
uint32_t m32_msize(uint32_t g) { return in_heap(g) ? (uint32_t)mspace_usable_size(m32_h(g)) : 0; }

void m32_free(uint32_t g)
{
    if (!g)
        return;
    if (in_heap(g))
        mspace_free(g_heap, m32_h(g));
    else if (m32_is_handle(g))
        free(m32_host(g));   /* memory a host function allocated and the guest was handed */
    else
        m32_log_once("free of a pointer outside the guest heap, ignored:", "free");
}

uint32_t m32_realloc(uint32_t g, uint32_t n)
{
    if (!g)
        return m32_malloc(n);
    if (in_heap(g))
        return to_guest(mspace_realloc(g_heap, m32_h(g), n ? n : 1));
    uint32_t fresh = m32_malloc(n);
    if (fresh && m32_is_handle(g)) {
        void *old = m32_host(g);
        size_t have = malloc_size(old);
        memcpy(m32_h(fresh), old, have < n ? have : n);
        free(old);
    }
    return fresh;
}

uint32_t m32_strdup_host(const char *s)
{
    if (!s)
        return 0;
    size_t n = strlen(s) + 1;
    uint32_t g = m32_malloc((uint32_t)n);
    if (g)
        memcpy(m32_h(g), s, n);
    return g;
}

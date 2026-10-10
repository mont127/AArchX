/* A program that registers its own malloc zone (as Batman's tcmalloc does): malloc, calloc, free and
 * posix_memalign must reach it (src/m32_stdio.c zone thunks; the JIT points the import slots at them), posix_memalign
 * storing the result or answering ENOMEM or EINVAL. */
#include <errno.h>
#include <malloc/malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char g_arena[1 << 16] __attribute__((aligned(4096)));
static size_t g_used;
static unsigned g_mallocs, g_frees, g_memaligns;

static void *bump(size_t align, size_t size)
{
    size_t at = (g_used + align - 1) & ~(align - 1);
    if (size > 4096 || at + size > sizeof g_arena)
        return NULL;
    g_used = at + size;
    return g_arena + at;
}
static size_t z_size(malloc_zone_t *z, const void *p) { return ((const unsigned char *)p >= g_arena && (const unsigned char *)p < g_arena + sizeof g_arena) ? 16 : 0; }
static void *z_malloc(malloc_zone_t *z, size_t n) { g_mallocs++; return bump(16, n); }
static void *z_calloc(malloc_zone_t *z, size_t k, size_t n) { g_mallocs++; void *p = bump(16, k * n); if (p) memset(p, 0, k * n); return p; }
static void *z_valloc(malloc_zone_t *z, size_t n) { return bump(4096, n); }
static void z_free(malloc_zone_t *z, void *p) { g_frees++; }
static void *z_realloc(malloc_zone_t *z, void *p, size_t n) { void *q = bump(16, n); if (q && p) memcpy(q, p, 16); return q; }
static void z_destroy(malloc_zone_t *z) {}
static void *z_memalign(malloc_zone_t *z, size_t align, size_t n) { g_memaligns++; return bump(align, n); }

int main(void)
{
    static malloc_zone_t zone;
    zone.size = z_size;
    zone.malloc = z_malloc;
    zone.calloc = z_calloc;
    zone.valloc = z_valloc;
    zone.free = z_free;
    zone.realloc = z_realloc;
    zone.destroy = z_destroy;
    zone.zone_name = "testzone";
    zone.version = 5;
    zone.memalign = z_memalign;
    malloc_zone_register(&zone);

    unsigned sum = 0, aligned = 0, inzone = 0;
    for (int i = 0; i < 200; i++) {
        void *a = malloc(24), *c = calloc(2, 8), *m = NULL;
        int rc = posix_memalign(&m, 64, 40);
        sum += (unsigned)rc;
        aligned += rc == 0 && ((uintptr_t)m & 63) == 0;
        inzone += z_size(&zone, a) && z_size(&zone, c) && z_size(&zone, m);
        free(a);
        free(c);
        free(m);
    }
    void *big = (void *)1;
    int rc = posix_memalign(&big, 64, 1 << 20);   /* the zone says no: ENOMEM, out untouched */
    int einval = 0;
    for (int i = 0; i < 50; i++) {                 /* not a power of two, or below sizeof(void *): EINVAL */
        void *m = (void *)1;
        einval += posix_memalign(&m, 48, 16) == EINVAL && posix_memalign(&m, 2, 16) == EINVAL && m == (void *)1;
    }
    printf("zone mallocs %u frees %u memaligns %u aligned %u inzone %u sum %u enomem %d untouched %d einval %d\n",
           g_mallocs, g_frees, g_memaligns, aligned, inzone, sum, rc == ENOMEM, big == (void *)1, einval);
    return 0;
}

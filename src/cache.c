#include <stdlib.h>
/* Maps the x86_64 dyld shared cache and resolves symbols out of it. */
#include "ocerz/cache.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <mach-o/loader.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "ocerz/mem.h"

#define CACHE_DIR "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/"
#define CACHE_STEM "dyld_shared_cache_x86_64"
#define CACHE_MAX_SUBCACHES 16
#define EXPORT_FLAGS_REEXPORT 0x08

#define EXPORT_FLAGS_KIND_MASK 0x03
#define EXPORT_FLAGS_KIND_ABSOLUTE 0x02

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static uint64_t uleb(const uint8_t **pp, const uint8_t *end)
{
    uint64_t v = 0;
    int shift = 0;
    const uint8_t *p = *pp;
    while (p < end) {
        uint8_t b = *p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80))
            break;
        shift += 7;
    }
    *pp = p;
    return v;
}

static uint64_t subcache_f2a(const uint8_t *hdr, uint32_t rec_off, uint32_t rec_cnt, uint64_t foff)
{
    for (uint32_t i = 0; i < rec_cnt; i++) {
        const uint8_t *m = hdr + rec_off + i * 56;
        uint64_t a = rd64(m), s = rd64(m + 8), fo = rd64(m + 16);
        if (foff >= fo && foff < fo + s)
            return a + (foff - fo);
    }
    return 0;
}

static void rebase_chain_v2(uint64_t page_base, uint64_t page_end, uint16_t start4,
                            uint64_t cache_base, uint64_t delta_mask, int delta_shift)
{
    uint64_t value_mask = ~delta_mask;
    uint64_t cur = page_base + (uint64_t)start4 * 4;
    for (;;) {
        if (cur < page_base || cur + 8 > page_end)
            break;
        uint8_t *loc = (uint8_t *)(uintptr_t)cur;
        uint64_t raw;
        memcpy(&raw, loc, 8);
        uint64_t value = raw & value_mask;
        if (value != 0)
            value += cache_base;
        uint64_t delta = (raw & delta_mask) >> delta_shift;
        memcpy(loc, &value, 8);
        if (delta == 0)
            break;
        cur += delta;
    }
}

/* ---- lazy rebasing of the slid (pointer-chain) data regions ----
 * The v2 slide info stores every pointer as offset|delta-chain bits, so the
 * DATA/DATA_CONST regions (~500 MB) need unpacking even at slide 0.  Doing
 * that eagerly touched (and copy-on-wrote) every page at process start
 * (~200 ms).  Instead the regions are mapped PROT_NONE and each 16K host page
 * is unpacked on its first touch from the SIGSEGV handler
 * (ocerz_cache_lazy_fault), then given its final protection. */
#define LAZY_MAX 16
static struct {
    uint64_t addr, size;          /* mapping */
    uint32_t page_size;           /* slide-info page size (4096) */
    const uint8_t *si;            /* slide info v2 */
    uint64_t cache_base;
    int final_prot;
    uint8_t *done;                /* one byte per host page */
} g_lazy[LAZY_MAX];
static int g_n_lazy;
static volatile int g_lazy_lock;

static void rebase_page_v2(uint64_t page_base, uint64_t page_size, uint32_t pg,
                           uint64_t cache_base, const uint8_t *si)
{
    uint32_t ps_off = rd32(si + 8);
    uint32_t ps_cnt = rd32(si + 12);
    uint32_t pe_off = rd32(si + 16);
    uint32_t pe_cnt = rd32(si + 20);
    uint64_t delta_mask = rd64(si + 24);
    int delta_shift = __builtin_ctzll(delta_mask) - 2;
    const uint8_t *page_starts = si + ps_off;
    const uint8_t *page_extras = si + pe_off;
    if (pg >= ps_cnt) return;
    uint16_t start = (uint16_t)(page_starts[pg * 2] | (page_starts[pg * 2 + 1] << 8));
    if (start == 0x4000) return;
    uint64_t page_end = page_base + page_size;
    if (start & 0x8000) {
        for (uint32_t idx = start & 0x3fff; idx < pe_cnt; idx++) {
            uint16_t e = (uint16_t)(page_extras[idx * 2] | (page_extras[idx * 2 + 1] << 8));
            rebase_chain_v2(page_base, page_end, e & 0x3fff, cache_base, delta_mask, delta_shift);
            if (e & 0x8000) break;
        }
    } else {
        rebase_chain_v2(page_base, page_end, start, cache_base, delta_mask, delta_shift);
    }
}

/* Called from the SIGSEGV handler with the faulting host address.  Returns 1
 * when the address lies in a lazily-slid region: the containing host page has
 * been unpacked and made accessible, and the faulting access can be retried. */
int ocerz_cache_lazy_fault(uintptr_t addr)
{
    for (int i = 0; i < g_n_lazy; i++) {
        if (addr - g_lazy[i].addr >= g_lazy[i].size) continue;
        uint64_t hp = 0x4000;
        uint64_t off = (addr - g_lazy[i].addr) & ~(hp - 1);
        size_t hidx = (size_t)(off / hp);
        /* a page this thread already retried once (or that was unpacked
         * before the fault) is not a lazy-unpack fault: an alignment or
         * protection fault on an unpacked page must reach the real handler
         * instead of retrying forever */
        static __thread uintptr_t last_retry;
        uintptr_t page = (uintptr_t)(g_lazy[i].addr + off);
        while (__atomic_exchange_n(&g_lazy_lock, 1, __ATOMIC_ACQUIRE)) { }
        if (g_lazy[i].done[hidx]) {
            int again = last_retry == page;
            last_retry = page;
            __atomic_store_n(&g_lazy_lock, 0, __ATOMIC_RELEASE);
            return again ? 0 : 1;      /* once: another thread may have just unpacked it */
        }
        last_retry = 0;
        {
            uint64_t base = g_lazy[i].addr + off;
            mprotect((void *)(uintptr_t)base, (size_t)hp, PROT_READ | PROT_WRITE);
            uint32_t per = (uint32_t)(hp / g_lazy[i].page_size);
            for (uint32_t k = 0; k < per; k++) {
                uint64_t pb = base + (uint64_t)k * g_lazy[i].page_size;
                if (pb + g_lazy[i].page_size > g_lazy[i].addr + g_lazy[i].size) break;
                rebase_page_v2(pb, g_lazy[i].page_size,
                               (uint32_t)((pb - g_lazy[i].addr) / g_lazy[i].page_size),
                               g_lazy[i].cache_base, g_lazy[i].si);
            }
            if (g_lazy[i].final_prot != (PROT_READ | PROT_WRITE))
                mprotect((void *)(uintptr_t)base, (size_t)hp, g_lazy[i].final_prot);
            g_lazy[i].done[hidx] = 1;
        }
        __atomic_store_n(&g_lazy_lock, 0, __ATOMIC_RELEASE);
        return 1;
    }
    return 0;
}

int ocerz_cache_lazy_region(uintptr_t addr)
{
    for (int i = 0; i < g_n_lazy; i++)
        if (addr - g_lazy[i].addr < g_lazy[i].size) return 1;
    return 0;
}

/* Every subcache mapping, so an address in the cache (TEXT included) can be
 * told apart from a wild one, plus write-watch state for the host pages a
 * guest has mprotect'ed writable in order to hot-patch them. */
#define CMAP_MAX (CACHE_MAX_SUBCACHES * 8)
#define WATCH_ARMED    1        /* watched, currently read-only */
#define WATCH_WRITABLE 2
static struct {
    uint64_t addr, size;
    uint8_t *watch;             /* one byte per host page */
} g_cmap[CMAP_MAX];
static int g_n_cmap;
static uint64_t g_cmap_lo = ~0ull, g_cmap_hi;
static volatile int g_any_watch;

static int cmap_find(uintptr_t addr)
{
    if (addr - g_cmap_lo >= g_cmap_hi - g_cmap_lo) return -1;
    for (int i = 0; i < g_n_cmap; i++)
        if (addr - g_cmap[i].addr < g_cmap[i].size) return i;
    return -1;
}

int ocerz_cache_region(uintptr_t addr)
{
    return cmap_find(addr) >= 0;
}

/* alloc=0 is the signal-handler path and never allocates */
static uint8_t *watch_slot(uintptr_t addr, int alloc)
{
    int i = cmap_find(addr);
    if (i < 0) return NULL;
    uint8_t *w = __atomic_load_n(&g_cmap[i].watch, __ATOMIC_ACQUIRE);
    if (!w) {
        if (!alloc) return NULL;
        size_t n = (size_t)((g_cmap[i].size + OCERZ_HOST_PAGE_SIZE - 1) / OCERZ_HOST_PAGE_SIZE);
        w = (uint8_t *)calloc(n, 1);
        if (!w) return NULL;
        uint8_t *had = NULL;
        if (!__atomic_compare_exchange_n(&g_cmap[i].watch, &had, w, 0,
                                         __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
            free(w);
            w = had;
        }
    }
    return w + (addr - g_cmap[i].addr) / OCERZ_HOST_PAGE_SIZE;
}

/* Guest mprotect of a cache page.  A lazily-slid page must be unpacked first:
 * once it is accessible the fault that would have rebased it never comes.
 * PROT_EXEC is dropped, guest code is never executed by the host. */
int ocerz_cache_protect(uintptr_t addr, uint64_t len, int prot)
{
    uint64_t hp = OCERZ_HOST_PAGE_SIZE;
    uint64_t lo = addr & ~(hp - 1);
    uint64_t hi = (addr + len + hp - 1) & ~(hp - 1);
    if (hi <= lo) return EINVAL;
    prot &= ~PROT_EXEC;
    if (!prot) prot = PROT_READ;
    for (uint64_t p = lo; p < hi; p += hp)
        if (ocerz_cache_lazy_region((uintptr_t)p)) ocerz_cache_lazy_fault((uintptr_t)p);
    if (mprotect((void *)(uintptr_t)lo, (size_t)(hi - lo), prot) != 0) return errno;
    if (prot & PROT_WRITE) {
        for (uint64_t p = lo; p < hi; p += hp) {
            uint8_t *s = watch_slot((uintptr_t)p, 1);
            if (s) __atomic_store_n(s, WATCH_WRITABLE, __ATOMIC_RELEASE);
        }
        __atomic_store_n(&g_any_watch, 1, __ATOMIC_RELEASE);
    }
    return 0;
}

/* Store fault on a page re-armed by ocerz_cache_arm_exec: grant write again.
 * The caller drops the translations that were made from it. */
int ocerz_cache_write_fault(uintptr_t addr)
{
    if (!__atomic_load_n(&g_any_watch, __ATOMIC_ACQUIRE)) return 0;
    uint8_t *s = watch_slot(addr, 0);
    if (!s || !__atomic_load_n(s, __ATOMIC_ACQUIRE)) return 0;
    uint64_t page = addr & ~(OCERZ_HOST_PAGE_SIZE - 1);
    if (mprotect((void *)(uintptr_t)page, (size_t)OCERZ_HOST_PAGE_SIZE,
                 PROT_READ | PROT_WRITE) != 0)
        return 0;
    __atomic_store_n(s, WATCH_WRITABLE, __ATOMIC_RELEASE);
    return 1;
}

/* Code has been translated out of [lo,hi): take write back off any patched
 * page in it, so the next patch of those bytes faults instead of going unseen. */
void ocerz_cache_arm_exec(uint64_t lo, uint64_t hi)
{
    if (!__atomic_load_n(&g_any_watch, __ATOMIC_ACQUIRE)) return;
    uint64_t hp = OCERZ_HOST_PAGE_SIZE;
    if (hi <= lo || hi - lo > (1u << 20)) return;
    for (uint64_t p = lo & ~(hp - 1); p < hi; p += hp) {
        uint8_t *s = watch_slot((uintptr_t)p, 0);
        if (!s || __atomic_load_n(s, __ATOMIC_ACQUIRE) != WATCH_WRITABLE) continue;
        if (mprotect((void *)(uintptr_t)p, (size_t)hp, PROT_READ) == 0)
            __atomic_store_n(s, WATCH_ARMED, __ATOMIC_RELEASE);
    }
}

static void rebase_slide_v2(uint64_t map_addr, uint64_t map_size, uint64_t cache_base, const uint8_t *si)
{
    uint32_t page_size = rd32(si + 4);
    uint32_t ps_off = rd32(si + 8);
    uint32_t ps_cnt = rd32(si + 12);
    uint32_t pe_off = rd32(si + 16);
    uint32_t pe_cnt = rd32(si + 20);
    uint64_t delta_mask = rd64(si + 24);
    int delta_shift = __builtin_ctzll(delta_mask) - 2;
    const uint8_t *page_starts = si + ps_off;
    const uint8_t *page_extras = si + pe_off;
    for (uint32_t pg = 0; pg < ps_cnt; pg++) {
        uint16_t start = (uint16_t)(page_starts[pg * 2] | (page_starts[pg * 2 + 1] << 8));
        if (start == 0x4000)
            continue;
        uint64_t page_base = map_addr + (uint64_t)pg * page_size;
        if (page_base + page_size > map_addr + map_size)
            break;
        uint64_t page_end = page_base + page_size;
        if (start & 0x8000) {
            for (uint32_t idx = start & 0x3fff; idx < pe_cnt; idx++) {
                uint16_t e = (uint16_t)(page_extras[idx * 2] | (page_extras[idx * 2 + 1] << 8));
                rebase_chain_v2(page_base, page_end, e & 0x3fff,
                                cache_base, delta_mask, delta_shift);
                if (e & 0x8000)
                    break;
            }
        } else {
            rebase_chain_v2(page_base, page_end, start,
                            cache_base, delta_mask, delta_shift);
        }
    }
}

static int map_subcache(const char *path, int is_main, OcerzCache *c)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    static uint8_t hdr[0x400];
    if (pread(fd, hdr, sizeof hdr, 0) != (ssize_t)sizeof hdr) {
        close(fd);
        return -1;
    }
    uint32_t rec_off = rd32(hdr + 0x138);
    uint32_t rec_cnt = rd32(hdr + 0x13c);
    if (rec_off == 0 || rec_cnt == 0 || rec_cnt > 8) {
        close(fd);
        return -1;
    }
    uint64_t slide_regions[8][5];
    int n_slide = 0;
    for (uint32_t i = 0; i < rec_cnt; i++) {
        const uint8_t *m = hdr + rec_off + i * 56;
        uint64_t addr = rd64(m);
        uint64_t size = rd64(m + 8);
        uint64_t foff = rd64(m + 16);
        uint64_t slide_off = rd64(m + 24);
        uint64_t slide_size = rd64(m + 32);
        uint32_t initp = rd32(m + 52);
        int prot = 0;
        if (initp & VM_PROT_READ)
            prot |= PROT_READ;
        if (initp & VM_PROT_WRITE)
            prot |= PROT_READ | PROT_WRITE;
        if (initp & VM_PROT_EXECUTE)
            prot |= PROT_READ;
        if (slide_size != 0)
            prot |= PROT_READ | PROT_WRITE;
        if (prot == 0)
            prot = PROT_READ;
        static int eager = -1;
        if (eager < 0) eager = getenv("OCERZ_EAGER_SLIDE") ? 1 : 0;
        int lazy = slide_size != 0 && !eager && g_n_lazy < LAZY_MAX;
        void *p = mmap((void *)(uintptr_t)addr, (size_t)size, lazy ? PROT_NONE : prot,
                       MAP_PRIVATE | MAP_FIXED, fd, (off_t)foff);
        if (p != (void *)(uintptr_t)addr) {
            OCERZ_FATAL("cache mapping %u of %s failed (%p want %#llx)\n",
                        i, path, p, (unsigned long long)addr);
            close(fd);
            return -1;
        }
        if (g_n_cmap < CMAP_MAX) {
            g_cmap[g_n_cmap].addr = addr;
            g_cmap[g_n_cmap].size = size;
            g_n_cmap++;
            if (addr < g_cmap_lo) g_cmap_lo = addr;
            if (addr + size > g_cmap_hi) g_cmap_hi = addr + size;
        }
        if (is_main && i == 0) {
            c->base = addr;
            c->hdr = (const uint8_t *)(uintptr_t)addr;
        }
        if (slide_size != 0) {
            slide_regions[n_slide][0] = addr;
            slide_regions[n_slide][1] = slide_off;
            slide_regions[n_slide][2] = size;
            slide_regions[n_slide][3] = (uint64_t)lazy;
            slide_regions[n_slide][4] = (uint64_t)((initp & VM_PROT_WRITE) ? (PROT_READ | PROT_WRITE) : PROT_READ);
            n_slide++;
        }
    }
    uint64_t cache_base = c->base;
    for (int i = 0; i < n_slide; i++) {
        uint64_t si_addr = subcache_f2a(hdr, rec_off, rec_cnt, slide_regions[i][1]);
        if (si_addr == 0)
            continue;
        const uint8_t *si = (const uint8_t *)(uintptr_t)si_addr;
        if (rd32(si) != 2)
            continue;
        if (slide_regions[i][3]) {
            /* lazy: remember the region; pages unpack on first touch */
            uint64_t hp = 0x4000;
            size_t npages = (size_t)((slide_regions[i][2] + hp - 1) / hp);
            g_lazy[g_n_lazy].addr = slide_regions[i][0];
            g_lazy[g_n_lazy].size = slide_regions[i][2];
            g_lazy[g_n_lazy].page_size = rd32(si + 4);
            g_lazy[g_n_lazy].si = si;
            g_lazy[g_n_lazy].cache_base = cache_base;
            g_lazy[g_n_lazy].final_prot = (int)slide_regions[i][4];
            g_lazy[g_n_lazy].done = (uint8_t *)calloc(npages, 1);
            if (g_lazy[g_n_lazy].done) g_n_lazy++;
            else rebase_slide_v2(slide_regions[i][0], slide_regions[i][2], cache_base, si);   /* fallback: eager */
        } else {
            rebase_slide_v2(slide_regions[i][0], slide_regions[i][2], cache_base, si);
        }
    }
    close(fd);
    return 0;
}

int ocerz_cache_map(OcerzCache *c)
{
    memset(c, 0, sizeof *c);
    if (map_subcache(CACHE_DIR CACHE_STEM, 1, c) != 0) {
        OCERZ_FATAL("cannot map shared cache %s\n", CACHE_DIR CACHE_STEM);
        return OCERZ_EIO;
    }
    for (int n = 1; n < CACHE_MAX_SUBCACHES; n++) {
        char path[512];
        snprintf(path, sizeof path, "%s%s.%02d", CACHE_DIR, CACHE_STEM, n);
        if (map_subcache(path, 0, c) != 0)
            break;
    }
    c->images_off = rd32(c->hdr + 0x1c0);
    c->images_cnt = rd32(c->hdr + 0x1c4);
    if (c->images_cnt == 0 || c->images_off == 0) {
        OCERZ_FATAL("shared cache image table not found (off=%#x cnt=%u)\n",
                    c->images_off, c->images_cnt);
        return OCERZ_EFORMAT;
    }
    c->mapped = 1;
    OCERZ_LOG("shared cache mapped at %#llx, %u images\n",
              (unsigned long long)c->base, c->images_cnt);
    return OCERZ_OK;
}

uint64_t ocerz_cache_image_addr(OcerzCache *c, uint32_t i, const char **path_out)
{
    if (i >= c->images_cnt)
        return 0;
    const uint8_t *e = c->hdr + c->images_off + (size_t)i * 32;
    uint64_t addr = rd64(e);
    if (path_out) {
        uint32_t poff = rd32(e + 0x18);
        *path_out = (const char *)(c->hdr + poff);
    }
    return addr;
}

static int dylib_export_region(uint64_t mh_addr, const uint8_t **trie_start,
                               const uint8_t **trie_end)
{
    const uint8_t *mh = (const uint8_t *)(uintptr_t)mh_addr;
    uint32_t ncmds = rd32(mh + 16);
    const uint8_t *lc = mh + sizeof(struct mach_header_64);
    uint64_t le_vmaddr = 0, le_fileoff = 0;
    int have_le = 0;
    uint32_t exp_off = 0, exp_size = 0;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        uint32_t csize = rd32(lc + 4);
        if (csize < 8)
            return -1;
        if (cmd == LC_SEGMENT_64) {
            if (memcmp(lc + 8, "__LINKEDIT", 10) == 0) {
                le_vmaddr = rd64(lc + 24);
                le_fileoff = rd64(lc + 40);
                have_le = 1;
            }
        } else if (cmd == LC_DYLD_EXPORTS_TRIE) {
            exp_off = rd32(lc + 8);
            exp_size = rd32(lc + 12);
        } else if ((cmd == LC_DYLD_INFO || cmd == LC_DYLD_INFO_ONLY) && exp_off == 0) {
            exp_off = rd32(lc + 40);
            exp_size = rd32(lc + 44);
        }
        lc += csize;
    }
    if (!have_le || exp_off == 0 || exp_size == 0)
        return -1;
    uint64_t addr = le_vmaddr + ((uint64_t)exp_off - le_fileoff);
    *trie_start = (const uint8_t *)(uintptr_t)addr;
    *trie_end = *trie_start + exp_size;
    return 0;
}

static uint64_t trie_lookup(const uint8_t *start, const uint8_t *end, const char *sym,
                            int *is_reexport, uint64_t *reexport_ord,
                            const char **reexport_name, int *found, uint64_t *flags_out)
{
    *is_reexport = 0;
    *found = 0;
    *flags_out = 0;
    const uint8_t *p = start;
    const char *s = sym;
    while (p < end) {
        uint64_t term = uleb(&p, end);
        if (*s == '\0') {
            if (term == 0)
                return 0;
            const uint8_t *tp = p;
            uint64_t flags = uleb(&tp, end);
            *flags_out = flags;
            *found = 1;
            if (flags & EXPORT_FLAGS_REEXPORT) {
                *is_reexport = 1;
                *reexport_ord = uleb(&tp, end);
                *reexport_name = (const char *)tp;
                return 1;
            }
            return uleb(&tp, end);
        }
        p += term;
        if (p >= end)
            return 0;
        uint8_t children = *p++;
        const uint8_t *next = NULL;
        for (uint8_t i = 0; i < children; i++) {
            const char *edge = (const char *)p;
            size_t elen = strlen(edge);
            p += elen + 1;
            uint64_t child_off = uleb(&p, end);
            if (next == NULL && strncmp(s, edge, elen) == 0) {
                s += elen;
                next = start + child_off;
            }
        }
        if (next == NULL)
            return 0;
        p = next;
    }
    return 0;
}

static uint64_t cache_image_by_path(OcerzCache *c, const char *path)
{
    for (uint32_t i = 0; i < c->images_cnt; i++) {
        const char *p = NULL;
        uint64_t mh = ocerz_cache_image_addr(c, i, &p);
        if (mh && p && strcmp(p, path) == 0)
            return mh;
    }
    return 0;
}

static const char *dylib_ordinal_name(uint64_t mh, uint64_t ord)
{
    const uint8_t *m = (const uint8_t *)(uintptr_t)mh;
    uint32_t ncmds = rd32(m + 16);
    const uint8_t *lc = m + sizeof(struct mach_header_64);
    uint64_t n = 0;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
            cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD_DYLIB) {
            if (++n == ord)
                return (const char *)(lc + rd32(lc + 8));
        }
        lc += rd32(lc + 4);
    }
    return NULL;
}

static uint64_t resolve_in_dylib(OcerzCache *c, uint64_t mh, const char *sym, int depth,
                                 int *found)
{
    if (depth > 16)
        return 0;
    const uint8_t *ts, *te;
    if (dylib_export_region(mh, &ts, &te) != 0)
        return 0;
    int reexp = 0;
    uint64_t ord = 0;
    const char *imp = NULL;
    int lfound = 0;
    uint64_t lflags = 0;
    uint64_t off = trie_lookup(ts, te, sym, &reexp, &ord, &imp, &lfound, &lflags);
    if (!lfound)
        return 0;
    if (!reexp) {
        *found = 1;

        if ((lflags & EXPORT_FLAGS_KIND_MASK) == EXPORT_FLAGS_KIND_ABSOLUTE)
            return off;
        return mh + off;
    }
    const char *want = (imp && imp[0]) ? imp : sym;
    const char *tgt = dylib_ordinal_name(mh, ord);
    if (!tgt)
        return 0;
    uint64_t tmh = cache_image_by_path(c, tgt);
    if (!tmh)
        return 0;
    return resolve_in_dylib(c, tmh, want, depth + 1, found);
}

/* Every import that is not satisfied by its own declared dependency lands in
 * the walk below, which visits all ~3000 cache images.  Wine's loaders resolve
 * the same libsystem symbols for every module they map, so remember the
 * answers -- the cache's export tries do not change at runtime. */
#define RMEMO_SLOTS 4096
typedef struct { char *name; uint64_t val; int found; } ResolveMemo;
static ResolveMemo g_rmemo[RMEMO_SLOTS];
static pthread_mutex_t g_rmemo_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned rmemo_hash(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h & (RMEMO_SLOTS - 1);
}

static ResolveMemo *rmemo_find(const char *symbol)
{
    unsigned i = rmemo_hash(symbol);
    for (unsigned n = 0; n < 8; n++, i = (i + 1) & (RMEMO_SLOTS - 1)) {
        if (!g_rmemo[i].name)
            return &g_rmemo[i];              /* free slot for the caller to fill */
        if (strcmp(g_rmemo[i].name, symbol) == 0)
            return &g_rmemo[i];
    }
    return NULL;
}

static uint64_t cache_resolve_walk(OcerzCache *c, const char *symbol, int *found);

uint64_t ocerz_cache_resolve_ex(OcerzCache *c, const char *symbol, int *found)
{
    int dummy = 0;
    if (!found)
        found = &dummy;
    *found = 0;
    if (!c->mapped)
        return 0;

    pthread_mutex_lock(&g_rmemo_lock);
    ResolveMemo *m = rmemo_find(symbol);
    if (m && m->name) {
        *found = m->found;
        uint64_t v = m->val;
        pthread_mutex_unlock(&g_rmemo_lock);
        return v;
    }
    pthread_mutex_unlock(&g_rmemo_lock);

    int f = 0;
    uint64_t v = cache_resolve_walk(c, symbol, &f);

    pthread_mutex_lock(&g_rmemo_lock);
    m = rmemo_find(symbol);
    if (m && !m->name) {
        m->val = v;
        m->found = f;
        m->name = strdup(symbol);
    }
    pthread_mutex_unlock(&g_rmemo_lock);

    *found = f;
    return v;
}

static uint64_t cache_resolve_walk(OcerzCache *c, const char *symbol, int *found)
{
    for (uint32_t i = 0; i < c->images_cnt; i++) {
        uint64_t mh = ocerz_cache_image_addr(c, i, NULL);
        if (mh == 0 || rd32((const uint8_t *)(uintptr_t)mh) != MH_MAGIC_64)
            continue;
        int f = 0;
        uint64_t r = resolve_in_dylib(c, mh, symbol, 0, &f);
        if (f) {
            *found = 1;
            return r;
        }
    }
    return 0;
}

uint64_t ocerz_cache_resolve(OcerzCache *c, const char *symbol)
{
    return ocerz_cache_resolve_ex(c, symbol, NULL);
}

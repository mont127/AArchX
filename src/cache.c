/*
 * Maps the x86_64 dyld shared cache and resolves symbols out of it.
 *
 * ---- where the cache is ----
 * macOS 27 keeps the x86_64 cache in the Rosetta cryptex,
 * /System/Volumes/Preboot/Cryptexes/Rosetta/System/Library/dyld, and macOS 26
 * keeps it in the OS cryptex, /System/Volumes/Preboot/Cryptexes/OS/System/
 * Library/dyld.  The Rosetta location is tried first and the OS one only when
 * the Rosetta one holds no cache, so a macOS 27 machine maps exactly what it
 * did before and a macOS 26 machine runs cache mode at all; looking only in the
 * Rosetta cryptex had made every dynamic program on macOS 26 stop with "cannot
 * map shared cache".  The main file and its .NN subcaches always come from the
 * same directory, and ocerz_cache_dir names the one in use.
 *
 * ---- lazy rebasing ----
 * The v2 slide info stores every pointer as offset|delta-chain bits, so the
 * DATA and DATA_CONST regions - about 500 MB - need unpacking even at slide 0.
 * Doing that eagerly touched and copy-on-wrote every page at process start,
 * ~200 ms of it.  Instead those regions are mapped PROT_NONE and each 16 KB
 * host page is unpacked on its first touch, from the SIGSEGV handler, then
 * given its final protection.  A page is unpacked into a private scratch
 * mapping and installed atomically: unpacking in place after an mprotect(RW)
 * let every other thread read the raw pointer chains mid-unpack, and
 * libswiftCore in steam.exe dereferenced one of those half-baked pointers.
 * Kept PROT_NONE until the remap, a concurrent reader simply faults, waits on
 * the lock and retries against the finished page.  A page this thread already
 * retried once is not a lazy-unpack fault, so an alignment or protection fault
 * on an unpacked page still reaches the real handler.  The unpack now holds its
 * lock across a whole page rebuild, so fork takes that lock like the other
 * emulator locks or the child inherits one nobody releases.
 *
 * ---- patched pages ----
 * Some engines make a libsystem page writable to patch it in place, so every
 * subcache mapping is recorded (which also tells an address in the cache apart
 * from a wild one) along with write-watch state for the pages a guest has
 * mprotect'ed writable.  A lazily-slid page must be unpacked before such an
 * mprotect, because once it is accessible the fault that would have rebased it
 * never comes.  PROT_EXEC is always dropped: guest code is never executed by
 * the host.  When code has been translated out of a patched page, write is
 * taken back off it so the next patch faults instead of going unseen.
 *
 * ---- symbol resolution ----
 * Every import not satisfied by its own declared dependency falls back to a
 * walk of all ~3000 cache images, and Wine's loaders resolve the same libsystem
 * symbols for every module they map, so the answers are memoized - the cache's
 * export tries do not change at runtime.
 *
 * Two-level namespace resolution looks up a symbol in the SPECIFIC dylib the
 * binary named, following re-exports, rather than taking the flat walk: a
 * binary that links /usr/lib/libcrypto.46.dylib (LibreSSL 3.3.6) must bind
 * OpenSSL_version there even though the cache also carries libcrypto.44 (2.8.3)
 * exporting the same name, and the flat walk bound it to whichever image came
 * first, so openssl reported the wrong version.  The path-to-header lookup goes
 * through the index below because resolving every import of a dependency would
 * otherwise rescan all ~3600 images.  A dylib lookup follows LC_REEXPORT_DYLIB as well as re-exports
 * in the trie, because an umbrella such as libSystem answers for its members
 * only through those load commands.
 *
 * All of that runs on an index built once per process: install path to image
 * and header to image in open-addressed tables, and per image, filled on first
 * use, its export trie and its linked images in ordinal order with their kind.
 * The path memo it replaced was 512 direct-mapped slots, so under Wine, which
 * resolves thousands of OpenGL and AppKit names through dlsym, colliding paths
 * fell back to a strcmp over every image and a webhelper spent a quarter of its
 * startup in resolve_in_dylib, strcmp and strlen; a dlsym miss on a framework
 * handle cost 2 ms against Rosetta's 30 us.  A breadth-first search from one
 * image walks the index with a visited bitmap and memoizes its answer per image
 * and name.  Two searches share it: ocerz_cache_resolve_from_image follows every
 * link, upward ones included, and ocerz_cache_dlsym_image skips upward links
 * the way dlsym does (OCERZ_DLSYM_UPWARD follows them there too).
 *
 * A weak-coalescing bind (ordinal -3) asks whether any image already defines
 * the name, and dyld answers it only from images that define weak symbols,
 * MH_WEAK_DEFINES.  Walking the whole cache instead cost Electron Framework
 * about 2,560 misses of 1.8 ms each, 4.6 s per process, because its weak names
 * are its own C++ and no system library has them.  Those binds walk the ~300
 * weak-defining images only, and after 128 of them a 1 MB bloom filter of
 * every name those images and their re-exports export turns a miss into one
 * probe.  The names are hashed along the trie edges rather than rebuilt, which
 * keeps the build near 40 ms, and a program with a handful of weak binds never
 * pays it.  The filter is built privately and published with a release store,
 * because readers test it without the lock.
 *
 * The image list holds each library once, under its install name; every other
 * path for it, /usr/lib/libz.dylib for libz.1.dylib or libgcc_s.1.dylib for
 * libSystem, is only in the cache's dylibs trie, at header offset 0x108 in the
 * public dyld_cache_format.h, which maps a path to an image index.  A path
 * missing from the image list is looked up there, which is how 6,677 of the
 * 10,595 paths in the macOS 27 Intel cache are reached.
 */
#include <stdlib.h>
#include "ocerz/cache.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/loader.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "ocerz/mem.h"
#include "ocerz/tcache.h"

#define CACHE_STEM "dyld_shared_cache_x86_64"

static const char *const g_cache_dirs[] = {
    "/System/Volumes/Preboot/Cryptexes/Rosetta/System/Library/dyld/",
    "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/",
};
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

#define LAZY_MAX 16
static struct {
    uint64_t addr, size;
    uint32_t page_size;
    const uint8_t *si;
    uint64_t cache_base;
    int final_prot;
    uint8_t *done;
    int fd;
    uint64_t foff;
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

int ocerz_cache_lazy_fault(uintptr_t addr)
{
    for (int i = 0; i < g_n_lazy; i++) {
        if (addr - g_lazy[i].addr >= g_lazy[i].size) continue;
        uint64_t hp = 0x4000;
        uint64_t off = (addr - g_lazy[i].addr) & ~(hp - 1);
        size_t hidx = (size_t)(off / hp);
        static __thread uintptr_t last_retry;
        uintptr_t page = (uintptr_t)(g_lazy[i].addr + off);
        while (__atomic_exchange_n(&g_lazy_lock, 1, __ATOMIC_ACQUIRE)) { }
        if (g_lazy[i].done[hidx]) {
            int again = last_retry == page;
            last_retry = page;
            __atomic_store_n(&g_lazy_lock, 0, __ATOMIC_RELEASE);
            return again ? 0 : 1;
        }
        last_retry = 0;
        {
            uint64_t base = g_lazy[i].addr + off;
            uint32_t per = (uint32_t)(hp / g_lazy[i].page_size);
            int installed = 0;
            static int no_remap = -1;
            if (no_remap < 0) no_remap = getenv("OCERZ_NO_LAZY_REMAP") ? 1 : 0;
            uint8_t *tmp = (g_lazy[i].fd >= 0 && !no_remap)
                ? (uint8_t *)mmap(NULL, (size_t)hp, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0)
                : (uint8_t *)MAP_FAILED;
            if (tmp != MAP_FAILED) {
                ssize_t got = pread(g_lazy[i].fd, tmp, (size_t)hp, (off_t)(g_lazy[i].foff + off));
                if (got > 0) {
                    for (uint32_t k = 0; k < per; k++) {
                        uint64_t pb = base + (uint64_t)k * g_lazy[i].page_size;
                        if (pb + g_lazy[i].page_size > g_lazy[i].addr + g_lazy[i].size) break;
                        rebase_page_v2((uint64_t)(uintptr_t)tmp + (uint64_t)k * g_lazy[i].page_size,
                                       g_lazy[i].page_size,
                                       (uint32_t)((pb - g_lazy[i].addr) / g_lazy[i].page_size),
                                       g_lazy[i].cache_base, g_lazy[i].si);
                    }
                    mach_vm_address_t dst = (mach_vm_address_t)base;
                    vm_prot_t curp = 0, maxp = 0;
                    kern_return_t kr = mach_vm_remap(mach_task_self(), &dst, (mach_vm_size_t)hp, 0,
                                                     VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                                     mach_task_self(), (mach_vm_address_t)(uintptr_t)tmp, FALSE,
                                                     &curp, &maxp, VM_INHERIT_DEFAULT);
                    if (kr == KERN_SUCCESS && dst == (mach_vm_address_t)base) {
                        mprotect((void *)(uintptr_t)base, (size_t)hp, g_lazy[i].final_prot);
                        installed = 1;
                    }
                    if (getenv("OCERZ_LAZYCHECK") && kr == KERN_SUCCESS) {
                        uint8_t *chk = (uint8_t *)mmap(NULL, (size_t)hp, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
                        if (chk != MAP_FAILED && pread(g_lazy[i].fd, chk, (size_t)hp, (off_t)(g_lazy[i].foff + off)) == got) {
                            for (uint32_t k = 0; k < per; k++) {
                                uint64_t pb = base + (uint64_t)k * g_lazy[i].page_size;
                                if (pb + g_lazy[i].page_size > g_lazy[i].addr + g_lazy[i].size) break;
                                rebase_page_v2((uint64_t)(uintptr_t)chk + (uint64_t)k * g_lazy[i].page_size, g_lazy[i].page_size,
                                               (uint32_t)((pb - g_lazy[i].addr) / g_lazy[i].page_size), g_lazy[i].cache_base, g_lazy[i].si);
                            }
                            int rc = memcmp(chk, (const void *)(uintptr_t)base, (size_t)got);
                            fprintf(stderr, "ocerz: LAZYCHECK[%d] page=%#llx region=%d off=%#llx got=%zd %s\n", (int)getpid(),
                                    (unsigned long long)base, i, (unsigned long long)off, got, rc ? "MISMATCH" : "ok");
                        }
                        if (chk != MAP_FAILED) munmap(chk, (size_t)hp);
                    }
                }
                munmap(tmp, (size_t)hp);
            }
            if (!installed) {
                mprotect((void *)(uintptr_t)base, (size_t)hp, PROT_READ | PROT_WRITE);
                for (uint32_t k = 0; k < per; k++) {
                    uint64_t pb = base + (uint64_t)k * g_lazy[i].page_size;
                    if (pb + g_lazy[i].page_size > g_lazy[i].addr + g_lazy[i].size) break;
                    rebase_page_v2(pb, g_lazy[i].page_size,
                                   (uint32_t)((pb - g_lazy[i].addr) / g_lazy[i].page_size),
                                   g_lazy[i].cache_base, g_lazy[i].si);
                }
                if (g_lazy[i].final_prot != (PROT_READ | PROT_WRITE))
                    mprotect((void *)(uintptr_t)base, (size_t)hp, g_lazy[i].final_prot);
            }
            g_lazy[i].done[hidx] = 1;
        }
        __atomic_store_n(&g_lazy_lock, 0, __ATOMIC_RELEASE);
        return 1;
    }
    return 0;
}

void ocerz_cache_prefork(void)
{
    while (__atomic_exchange_n(&g_lazy_lock, 1, __ATOMIC_ACQUIRE)) { }
}

void ocerz_cache_postfork(void)
{
    __atomic_store_n(&g_lazy_lock, 0, __ATOMIC_RELEASE);
}

int ocerz_cache_lazy_region(uintptr_t addr)
{
    for (int i = 0; i < g_n_lazy; i++)
        if (addr - g_lazy[i].addr < g_lazy[i].size) return 1;
    return 0;
}

#define CMAP_MAX (CACHE_MAX_SUBCACHES * 8)
#define WATCH_ARMED    1
#define WATCH_WRITABLE 2
static struct {
    uint64_t addr, size;
    uint8_t *watch;
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
    uint64_t slide_regions[8][6];
    int n_slide = 0;
    for (uint32_t i = 0; i < rec_cnt; i++) {
        const uint8_t *m = hdr + rec_off + i * 56;
        uint64_t addr = rd64(m);
        uint64_t size = rd64(m + 8);
        uint64_t foff = rd64(m + 16);
        uint64_t slide_off = rd64(m + 24);
        uint64_t slide_size = rd64(m + 32);
        uint32_t initp = rd32(m + 52);
        {
            const char *cml = getenv("OCERZ_CACHEMAPLOG");
            if (cml) {
                uint64_t of = cml[0] ? strtoull(cml, NULL, 0) : 0;
                fprintf(stderr, "ocerz: CMAP %s map%u addr=%#llx size=%#llx foff=%#llx slide_off=%#llx slide_size=%#llx initp=%#x %s\n",
                        is_main?"main":"sub", i, (unsigned long long)addr, (unsigned long long)size,
                        (unsigned long long)foff, (unsigned long long)slide_off, (unsigned long long)slide_size, initp,
                        (of && of >= addr && of < addr + size) ? "<-- contains address of interest" : "");
            }
        }
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
            slide_regions[n_slide][5] = foff;
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
            uint64_t hp = 0x4000;
            size_t npages = (size_t)((slide_regions[i][2] + hp - 1) / hp);
            g_lazy[g_n_lazy].addr = slide_regions[i][0];
            g_lazy[g_n_lazy].size = slide_regions[i][2];
            g_lazy[g_n_lazy].page_size = rd32(si + 4);
            g_lazy[g_n_lazy].si = si;
            g_lazy[g_n_lazy].cache_base = cache_base;
            g_lazy[g_n_lazy].final_prot = (int)slide_regions[i][4];
            g_lazy[g_n_lazy].fd = dup(fd);
            g_lazy[g_n_lazy].foff = slide_regions[i][5];
            g_lazy[g_n_lazy].done = (uint8_t *)calloc(npages, 1);
            if (g_lazy[g_n_lazy].done) g_n_lazy++;
            else rebase_slide_v2(slide_regions[i][0], slide_regions[i][2], cache_base, si);
        } else {
            rebase_slide_v2(slide_regions[i][0], slide_regions[i][2], cache_base, si);
        }
    }
    close(fd);
    return 0;
}

static const OcerzCache *g_named_cache;

const char *ocerz_cache_dir(void)
{
    for (size_t i = 0; i < sizeof g_cache_dirs / sizeof g_cache_dirs[0]; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s%s", g_cache_dirs[i], CACHE_STEM);
        if (access(path, R_OK) == 0)
            return g_cache_dirs[i];
    }
    return NULL;
}

int ocerz_cache_map(OcerzCache *c)
{
    memset(c, 0, sizeof *c);
    const char *dir = ocerz_cache_dir();
    if (!dir) {
        OCERZ_FATAL("cannot find the x86_64 shared cache in %s or %s\n",
                    g_cache_dirs[0], g_cache_dirs[1]);
        return OCERZ_EIO;
    }
    char path[512];
    snprintf(path, sizeof path, "%s%s", dir, CACHE_STEM);
    if (map_subcache(path, 1, c) != 0) {
        OCERZ_FATAL("cannot map shared cache %s\n", path);
        return OCERZ_EIO;
    }
    for (int n = 1; n < CACHE_MAX_SUBCACHES; n++) {
        snprintf(path, sizeof path, "%s%s.%02d", dir, CACHE_STEM, n);
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
    g_named_cache = c;
    OCERZ_LOG("shared cache mapped at %#llx, %u images\n",
              (unsigned long long)c->base, c->images_cnt);
    return OCERZ_OK;
}

const char *ocerz_cache_name_for_addr(uint64_t addr, uint64_t *base_out)
{
    const OcerzCache *c = g_named_cache;
    if (!c || !c->mapped || !c->images_cnt)
        return NULL;
    uint64_t best = 0;
    const char *best_path = NULL;
    for (uint32_t i = 0; i < c->images_cnt; i++) {
        const uint8_t *e = c->hdr + c->images_off + (size_t)i * 32;
        uint64_t a = rd64(e);
        if (a <= addr && a > best) {
            best = a;
            best_path = (const char *)(c->hdr + rd32(e + 0x18));
        }
    }
    if (!best_path)
        return NULL;
    if (base_out)
        *base_out = best;
    return best_path;
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

uint64_t ocerz_cache_find_alias(OcerzCache *c, const char *path)
{
    if (!c || !c->mapped || !path)
        return 0;
    uint64_t taddr = rd64(c->hdr + 0x108), tsize = rd64(c->hdr + 0x110);
    if (!taddr || !tsize || tsize > (64u << 20))
        return 0;
    const uint8_t *start = (const uint8_t *)(uintptr_t)taddr, *end = start + tsize;
    const uint8_t *p = start;
    const char *s = path;
    for (int depth = 0; depth < 512 && p < end; depth++) {
        uint64_t term = uleb(&p, end);
        if (*s == '\0') {
            if (!term)
                return 0;
            const uint8_t *tp = p;
            uint64_t idx = uleb(&tp, end);
            return idx < c->images_cnt ? ocerz_cache_image_addr(c, (uint32_t)idx, NULL) : 0;
        }
        if (term > (uint64_t)(end - p))
            return 0;
        p += term;
        if (p >= end)
            return 0;
        uint8_t children = *p++;
        const uint8_t *next = NULL;
        for (uint8_t i = 0; i < children && p < end; i++) {
            const char *edge = (const char *)p;
            size_t elen = strnlen(edge, (size_t)(end - p));
            p += elen + 1;
            uint64_t child_off = uleb(&p, end);
            if (!next && strncmp(s, edge, elen) == 0) {
                s += elen;
                next = start + child_off;
            }
        }
        if (!next)
            return 0;
        p = next;
    }
    return 0;
}

typedef struct CacheImgInfo {
    const uint8_t *ts, *te;
    uint32_t ndeps;
    uint32_t *dep;
    uint8_t *dep_kind;
} CacheImgInfo;

typedef struct CacheIndex {
    OcerzCache *c;
    uint32_t n, mask;
    uint32_t *by_path;
    uint32_t *by_mh;
    CacheImgInfo **info;
} CacheIndex;

#define CIX_NONE 0xffffffffu
enum { DEP_LOAD, DEP_REEXPORT, DEP_UPWARD };
static CacheIndex *g_cix;

static uint32_t cix_str_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static uint32_t cix_mh_hash(uint64_t mh)
{
    return (uint32_t)((mh * 0x9E3779B97F4A7C15ull) >> 32);
}

static CacheIndex *cix_get(OcerzCache *c)
{
    CacheIndex *ix = __atomic_load_n(&g_cix, __ATOMIC_ACQUIRE);
    if (ix)
        return ix->c == c ? ix : NULL;
    if (!c->mapped || !c->images_cnt)
        return NULL;
    uint32_t cap = 1024;
    while (cap < c->images_cnt * 4)
        cap <<= 1;
    ix = calloc(1, sizeof *ix);
    uint32_t *bp = ix ? calloc(cap, sizeof *bp) : NULL;
    uint32_t *bm = bp ? calloc(cap, sizeof *bm) : NULL;
    CacheImgInfo **info = bm ? calloc(c->images_cnt, sizeof *info) : NULL;
    if (!info) {
        free(bm);
        free(bp);
        free(ix);
        return NULL;
    }
    ix->c = c;
    ix->n = c->images_cnt;
    ix->mask = cap - 1;
    ix->by_path = bp;
    ix->by_mh = bm;
    ix->info = info;
    for (uint32_t i = 0; i < ix->n; i++) {
        const char *p = NULL;
        uint64_t mh = ocerz_cache_image_addr(c, i, &p);
        if (!mh)
            continue;
        if (p) {
            uint32_t h = cix_str_hash(p) & ix->mask;
            while (bp[h])
                h = (h + 1) & ix->mask;
            bp[h] = i + 1;
        }
        uint32_t h = cix_mh_hash(mh) & ix->mask;
        while (bm[h])
            h = (h + 1) & ix->mask;
        bm[h] = i + 1;
    }
    CacheIndex *expected = NULL;
    if (!__atomic_compare_exchange_n(&g_cix, &expected, ix, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        free(info);
        free(bm);
        free(bp);
        free(ix);
        return expected->c == c ? expected : NULL;
    }
    return ix;
}

static uint32_t cix_by_path(CacheIndex *ix, const char *path)
{
    for (uint32_t h = cix_str_hash(path) & ix->mask; ix->by_path[h]; h = (h + 1) & ix->mask) {
        uint32_t i = ix->by_path[h] - 1;
        const char *p = NULL;
        ocerz_cache_image_addr(ix->c, i, &p);
        if (p && strcmp(p, path) == 0)
            return i;
    }
    return CIX_NONE;
}

static uint32_t cix_by_mh(CacheIndex *ix, uint64_t mh)
{
    for (uint32_t h = cix_mh_hash(mh) & ix->mask; ix->by_mh[h]; h = (h + 1) & ix->mask) {
        uint32_t i = ix->by_mh[h] - 1;
        if (ocerz_cache_image_addr(ix->c, i, NULL) == mh)
            return i;
    }
    return CIX_NONE;
}

static uint64_t cache_image_by_path(OcerzCache *c, const char *path)
{
    CacheIndex *ix = cix_get(c);
    if (ix) {
        uint32_t i = cix_by_path(ix, path);
        if (i != CIX_NONE)
            return ocerz_cache_image_addr(c, i, NULL);
        return ocerz_cache_find_alias(c, path);
    }
    for (uint32_t i = 0; i < c->images_cnt; i++) {
        const char *p = NULL;
        uint64_t mh = ocerz_cache_image_addr(c, i, &p);
        if (mh && p && strcmp(p, path) == 0)
            return mh;
    }
    return ocerz_cache_find_alias(c, path);
}

static int dylib_export_region(uint64_t mh_addr, const uint8_t **trie_start,
                               const uint8_t **trie_end);

static const CacheImgInfo *cix_info(CacheIndex *ix, uint32_t i)
{
    CacheImgInfo *inf = __atomic_load_n(&ix->info[i], __ATOMIC_ACQUIRE);
    if (inf)
        return inf;
    uint64_t mh = ocerz_cache_image_addr(ix->c, i, NULL);
    if (!mh)
        return NULL;
    const uint8_t *m = (const uint8_t *)(uintptr_t)mh;
    uint32_t ncmds = rd32(m + 16), nd = 0;
    const uint8_t *lc = m + sizeof(struct mach_header_64);
    for (uint32_t k = 0; k < ncmds; k++) {
        uint32_t cmd = rd32(lc), size = rd32(lc + 4);
        if (size < 8)
            break;
        if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
            cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD_DYLIB)
            nd++;
        lc += size;
    }
    inf = calloc(1, sizeof *inf + nd * (sizeof(uint32_t) + 1));
    if (!inf)
        return NULL;
    inf->dep = (uint32_t *)(inf + 1);
    inf->dep_kind = (uint8_t *)(inf->dep + nd);
    if (dylib_export_region(mh, &inf->ts, &inf->te) != 0)
        inf->ts = inf->te = NULL;
    lc = m + sizeof(struct mach_header_64);
    for (uint32_t k = 0; k < ncmds && inf->ndeps < nd; k++) {
        uint32_t cmd = rd32(lc), size = rd32(lc + 4);
        if (size < 8)
            break;
        if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
            cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD_DYLIB) {
            uint32_t noff = rd32(lc + 8), di = CIX_NONE;
            if (noff < size) {
                const char *path = (const char *)lc + noff;
                di = cix_by_path(ix, path);
                if (di == CIX_NONE) {
                    uint64_t amh = ocerz_cache_find_alias(ix->c, path);
                    if (amh)
                        di = cix_by_mh(ix, amh);
                }
            }
            inf->dep[inf->ndeps] = di;
            int upward = cmd == LC_LOAD_UPWARD_DYLIB ||
                         (cmd != LC_REEXPORT_DYLIB && size >= sizeof(struct dylib_use_command) &&
                          noff == sizeof(struct dylib_use_command) && rd32(lc + 12) == DYLIB_USE_MARKER &&
                          (rd32(lc + 24) & DYLIB_USE_UPWARD));
            inf->dep_kind[inf->ndeps] = cmd == LC_REEXPORT_DYLIB ? DEP_REEXPORT : upward ? DEP_UPWARD : DEP_LOAD;
            inf->ndeps++;
        }
        lc += size;
    }
    CacheImgInfo *expected = NULL;
    if (!__atomic_compare_exchange_n(&ix->info[i], &expected, inf, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        free(inf);
        return expected;
    }
    return inf;
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

static uint64_t cache_image_by_path_memo(OcerzCache *c, const char *path);

static uint64_t resolve_in_dylib(OcerzCache *c, uint64_t mh, const char *sym, int depth,
                                 int *found)
{
    if (depth > 16)
        return 0;
    CacheIndex *ix = cix_get(c);
    uint32_t ii = ix ? cix_by_mh(ix, mh) : CIX_NONE;
    const CacheImgInfo *inf = ii != CIX_NONE ? cix_info(ix, ii) : NULL;
    if (inf) {
        if (!inf->ts)
            return 0;
        int reexp = 0, lfound = 0;
        uint64_t ord = 0, lflags = 0;
        const char *imp = NULL;
        uint64_t off = trie_lookup(inf->ts, inf->te, sym, &reexp, &ord, &imp, &lfound, &lflags);
        if (!lfound) {
            for (uint32_t k = 0; k < inf->ndeps; k++) {
                if (inf->dep_kind[k] != DEP_REEXPORT || inf->dep[k] == CIX_NONE)
                    continue;
                uint64_t tmh = ocerz_cache_image_addr(c, inf->dep[k], NULL);
                if (!tmh || tmh == mh)
                    continue;
                int f = 0;
                uint64_t v = resolve_in_dylib(c, tmh, sym, depth + 1, &f);
                if (f) {
                    *found = 1;
                    return v;
                }
            }
            return 0;
        }
        if (!reexp) {
            *found = 1;
            if ((lflags & EXPORT_FLAGS_KIND_MASK) == EXPORT_FLAGS_KIND_ABSOLUTE)
                return off;
            return mh + off;
        }
        if (ord == 0 || ord > inf->ndeps || inf->dep[ord - 1] == CIX_NONE)
            return 0;
        uint64_t tmh = ocerz_cache_image_addr(c, inf->dep[ord - 1], NULL);
        if (!tmh)
            return 0;
        return resolve_in_dylib(c, tmh, (imp && imp[0]) ? imp : sym, depth + 1, found);
    }
    const uint8_t *ts, *te;
    if (dylib_export_region(mh, &ts, &te) != 0)
        return 0;
    int reexp = 0;
    uint64_t ord = 0;
    const char *imp = NULL;
    int lfound = 0;
    uint64_t lflags = 0;
    uint64_t off = trie_lookup(ts, te, sym, &reexp, &ord, &imp, &lfound, &lflags);
    if (!lfound) {
        const uint8_t *h = (const uint8_t *)(uintptr_t)mh;
        uint32_t ncmds = rd32(h + 16);
        const uint8_t *lc = h + 32;
        for (uint32_t i = 0; i < ncmds; i++) {
            uint32_t cmd = rd32(lc), size = rd32(lc + 4);
            if (size < 8)
                break;
            if (cmd == LC_REEXPORT_DYLIB) {
                uint32_t noff = rd32(lc + 8);
                if (noff < size) {
                    uint64_t tmh = cache_image_by_path_memo(c, (const char *)lc + noff);
                    if (tmh && tmh != mh) {
                        int f = 0;
                        uint64_t v = resolve_in_dylib(c, tmh, sym, depth + 1, &f);
                        if (f) {
                            *found = 1;
                            return v;
                        }
                    }
                }
            }
            lc += size;
        }
        return 0;
    }
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

#define RMEMO_SLOTS (1u << 16)
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
    for (unsigned n = 0; n < 32; n++, i = (i + 1) & (RMEMO_SLOTS - 1)) {
        if (!g_rmemo[i].name)
            return &g_rmemo[i];
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

static uint64_t cache_image_by_path_memo(OcerzCache *c, const char *path)
{
    return cache_image_by_path(c, path);
}

uint64_t ocerz_cache_resolve_in_image(OcerzCache *c, const char *path,
                                      const char *symbol, int *found)
{
    int dummy = 0;
    if (!found)
        found = &dummy;
    *found = 0;
    if (!c->mapped || !path || !symbol)
        return 0;

    uint64_t mh = cache_image_by_path_memo(c, path);
    if (!mh)
        return 0;

    int f = 0;
    uint64_t v = resolve_in_dylib(c, mh, symbol, 0, &f);
    *found = f;
    return v;
}

int ocerz_cache_has_image(OcerzCache *c, uint64_t mh)
{
    if (!c->mapped || !mh)
        return 0;
    CacheIndex *ix = cix_get(c);
    if (ix)
        return cix_by_mh(ix, mh) != CIX_NONE;
    for (uint32_t i = 0; i < c->images_cnt; i++)
        if (ocerz_cache_image_addr(c, i, NULL) == mh)
            return 1;
    return 0;
}

#define FMEMO_SLOTS (1u << 15)
typedef struct { uint64_t mh; char *name; uint64_t val; int found; } FromMemo;
static FromMemo g_fmemo[FMEMO_SLOTS];
static pthread_mutex_t g_fmemo_lock = PTHREAD_MUTEX_INITIALIZER;

static FromMemo *fmemo_find(uint64_t mh, const char *symbol)
{
    unsigned i = (cix_str_hash(symbol) ^ cix_mh_hash(mh)) & (FMEMO_SLOTS - 1);
    for (unsigned n = 0; n < 16; n++, i = (i + 1) & (FMEMO_SLOTS - 1)) {
        if (!g_fmemo[i].name)
            return &g_fmemo[i];
        if (g_fmemo[i].mh == mh && strcmp(g_fmemo[i].name, symbol) == 0)
            return &g_fmemo[i];
    }
    return NULL;
}

static uint64_t resolve_from_index(OcerzCache *c, CacheIndex *ix, uint32_t root, uint64_t mh,
                                   const char *symbol, int *found, int skip_upward)
{
    uint64_t key = mh | (uint64_t)(skip_upward != 0);
    pthread_mutex_lock(&g_fmemo_lock);
    FromMemo *m = fmemo_find(key, symbol);
    if (m && m->name) {
        *found = m->found;
        uint64_t v = m->val;
        pthread_mutex_unlock(&g_fmemo_lock);
        return v;
    }
    pthread_mutex_unlock(&g_fmemo_lock);

    uint32_t order[1024], head = 0, tail = 0;
    uint8_t seen_small[1024];
    uint32_t nbytes = (ix->n + 7) / 8;
    uint8_t *seen = nbytes <= sizeof seen_small ? seen_small : calloc(nbytes, 1);
    uint64_t v = 0;
    int f = 0;
    if (seen) {
        if (seen == seen_small)
            memset(seen, 0, nbytes);
        order[tail++] = root;
        seen[root >> 3] |= (uint8_t)(1u << (root & 7));
        while (head < tail) {
            uint32_t cur = order[head++];
            v = resolve_in_dylib(c, ocerz_cache_image_addr(c, cur, NULL), symbol, 0, &f);
            if (f)
                break;
            v = 0;
            const CacheImgInfo *inf = cix_info(ix, cur);
            if (!inf)
                continue;
            for (uint32_t k = 0; k < inf->ndeps; k++) {
                uint32_t d = inf->dep[k];
                if (d == CIX_NONE || (seen[d >> 3] & (1u << (d & 7))) ||
                    (skip_upward && inf->dep_kind[k] == DEP_UPWARD))
                    continue;
                if (tail >= sizeof order / sizeof order[0])
                    break;
                seen[d >> 3] |= (uint8_t)(1u << (d & 7));
                order[tail++] = d;
            }
        }
        if (seen != seen_small)
            free(seen);
    }

    pthread_mutex_lock(&g_fmemo_lock);
    m = fmemo_find(key, symbol);
    if (m && !m->name) {
        m->name = strdup(symbol);
        if (m->name) {
            m->mh = key;
            m->val = v;
            m->found = f;
        }
    }
    pthread_mutex_unlock(&g_fmemo_lock);
    *found = f;
    return v;
}

uint64_t ocerz_cache_dlsym_image(OcerzCache *c, uint64_t mh, const char *symbol, int *found)
{
    int dummy = 0;
    if (!found)
        found = &dummy;
    *found = 0;
    if (!c->mapped || !mh || !symbol)
        return 0;
    CacheIndex *ix = cix_get(c);
    uint32_t root = ix ? cix_by_mh(ix, mh) : CIX_NONE;
    if (root == CIX_NONE)
        return ocerz_cache_resolve_from_image(c, mh, symbol, found);
    return resolve_from_index(c, ix, root, mh, symbol, found, !getenv("OCERZ_DLSYM_UPWARD"));
}

uint64_t ocerz_cache_resolve_from_image(OcerzCache *c, uint64_t mh, const char *symbol, int *found)
{
    int dummy = 0;
    if (!found)
        found = &dummy;
    *found = 0;
    if (!c->mapped || !mh || !symbol)
        return 0;

    CacheIndex *ix = cix_get(c);
    uint32_t root = ix ? cix_by_mh(ix, mh) : CIX_NONE;
    if (root != CIX_NONE)
        return resolve_from_index(c, ix, root, mh, symbol, found, 0);

    uint64_t order[1024];
    uint32_t head = 0, tail = 0;
    order[tail++] = mh;
    while (head < tail) {
        uint64_t cur = order[head++];
        int f = 0;
        uint64_t v = resolve_in_dylib(c, cur, symbol, 0, &f);
        if (f) {
            *found = 1;
            return v;
        }
        const uint8_t *m = (const uint8_t *)(uintptr_t)cur;
        uint32_t ncmds = rd32(m + 16);
        const uint8_t *lc = m + sizeof(struct mach_header_64);
        for (uint32_t i = 0; i < ncmds; i++) {
            uint32_t cmd = rd32(lc);
            if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
                cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD_DYLIB) {
                uint64_t dep = cache_image_by_path_memo(c, (const char *)(lc + rd32(lc + 8)));
                uint32_t k = 0;
                while (k < tail && order[k] != dep)
                    k++;
                if (dep && k == tail && tail < sizeof order / sizeof order[0])
                    order[tail++] = dep;
            }
            lc += rd32(lc + 4);
        }
    }
    return 0;
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

#define FNV64_BASIS 14695981039346656037ull
#define FNV64_PRIME 1099511628211ull
#define NAMEBLOOM_BITS (1u << 23)
#define WEAK_FILTER_AFTER 128

static uint64_t fnv64_extend(uint64_t h, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ (unsigned char)s[i]) * FNV64_PRIME;
    return h;
}

static void bloom_probe_bits(uint64_t h, uint32_t bits[4])
{
    h ^= h >> 29;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 32;
    uint32_t a = (uint32_t)h, b = (uint32_t)(h >> 32) | 1;
    for (int k = 0; k < 4; k++)
        bits[k] = (a + (uint32_t)k * b) & (NAMEBLOOM_BITS - 1);
}

static void bloom_add(uint8_t *bloom, uint64_t h)
{
    uint32_t bits[4];
    bloom_probe_bits(h, bits);
    for (int k = 0; k < 4; k++)
        bloom[bits[k] >> 3] |= (uint8_t)(1u << (bits[k] & 7));
}

static int bloom_has(const uint8_t *bloom, uint64_t h)
{
    uint32_t bits[4];
    bloom_probe_bits(h, bits);
    for (int k = 0; k < 4; k++)
        if (!(bloom[bits[k] >> 3] & (1u << (bits[k] & 7))))
            return 0;
    return 1;
}

static int trie_collect(const uint8_t *start, const uint8_t *end, const uint8_t *node,
                        uint64_t h, int depth, uint8_t *bloom, uint64_t *count)
{
    if (depth > 256 || node < start || node >= end)
        return -1;
    const uint8_t *p = node;
    uint64_t term = uleb(&p, end);
    if (term) {
        bloom_add(bloom, h);
        (*count)++;
    }
    if (term > (uint64_t)(end - p))
        return -1;
    p += term;
    if (p >= end)
        return 0;
    uint8_t children = *p++;
    for (uint8_t i = 0; i < children; i++) {
        const char *edge = (const char *)p;
        size_t elen = strnlen(edge, (size_t)(end - p));
        if (p + elen >= end)
            return -1;
        p += elen + 1;
        uint64_t child_off = uleb(&p, end);
        if (trie_collect(start, end, start + child_off, fnv64_extend(h, edge, elen), depth + 1,
                         bloom, count) != 0)
            return -1;
    }
    return 0;
}

static int collect_image_exports(OcerzCache *c, uint64_t mh, uint8_t *bloom, uint64_t *count,
                                 uint64_t *seen, uint32_t *nseen, uint32_t cap, int depth)
{
    if (depth > 16)
        return -1;
    for (uint32_t i = 0; i < *nseen; i++)
        if (seen[i] == mh)
            return 0;
    if (*nseen >= cap)
        return -1;
    seen[(*nseen)++] = mh;
    const uint8_t *ts, *te;
    if (dylib_export_region(mh, &ts, &te) == 0 &&
        trie_collect(ts, te, ts, FNV64_BASIS, 0, bloom, count) != 0)
        return -1;
    const uint8_t *h = (const uint8_t *)(uintptr_t)mh;
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + 32;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc), size = rd32(lc + 4);
        if (size < 8)
            break;
        if (cmd == LC_REEXPORT_DYLIB && rd32(lc + 8) < size) {
            uint64_t tmh = cache_image_by_path_memo(c, (const char *)lc + rd32(lc + 8));
            if (!tmh)
                return -1;
            if (collect_image_exports(c, tmh, bloom, count, seen, nseen, cap, depth + 1) != 0)
                return -1;
        }
        lc += size;
    }
    return 0;
}

/*
 * The weak-def name filter depends on nothing but the shared cache, yet every
 * process that made 128 weak lookups built it again: 990,000 names from the
 * 308 weak-defining images and those they re-export, 37 ms.  A C++ library's
 * own binds make that many; ollama --version dlopens an x86 libmlx whose do.
 * So the filter is kept in the translation store's directory (one per ocerz
 * build), named for the cache's UUID, with a sum over its bytes, written to a
 * temporary name and renamed into place.  It is read at the first weak
 * lookup, which also spares the 128 lookups made without it.  With the store
 * off (OCERZ_TCACHE=off) nothing is read or kept, and under its free-space
 * floor nothing is written.
 */
#define WEAKBLOOM_MAGIC 0x4d4c4257u
typedef struct WeakBloomHead {
    uint32_t magic, bits;
    uint8_t uuid[16];
    uint64_t count, sum;
} WeakBloomHead;

static uint64_t weakbloom_sum(const uint8_t *bloom)
{
    uint64_t h = 0x6a09e667f3bcc909ull, w;
    for (size_t i = 0; i < NAMEBLOOM_BITS / 8; i += 8) {
        memcpy(&w, bloom + i, 8);
        h = (h ^ w) * 0x9e3779b97f4a7c15ull;
        h ^= h >> 29;
    }
    return h;
}

static int weakbloom_path(const OcerzCache *c, char *path, size_t n, int for_write)
{
    const char *dir = ocerz_tcache_dir(for_write);
    if (!dir || !c->hdr)
        return 0;
    const uint8_t *u = c->hdr + 0x58;
    snprintf(path, n, "%s/weakbloom-%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x.bin", dir,
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    return 1;
}

static uint8_t *weakbloom_load(const OcerzCache *c)
{
    char path[1400];
    if (!weakbloom_path(c, path, sizeof path, 0))
        return NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    WeakBloomHead hd;
    uint8_t *bloom = calloc(NAMEBLOOM_BITS / 8, 1);
    int ok = bloom && read(fd, &hd, sizeof hd) == (ssize_t)sizeof hd && hd.magic == WEAKBLOOM_MAGIC &&
             hd.bits == NAMEBLOOM_BITS && memcmp(hd.uuid, c->hdr + 0x58, 16) == 0 &&
             read(fd, bloom, NAMEBLOOM_BITS / 8) == (ssize_t)(NAMEBLOOM_BITS / 8) &&
             weakbloom_sum(bloom) == hd.sum;
    close(fd);
    if (getenv("OCERZ_TCACHE_LOG"))
        fprintf(stderr, "ocerz: TCACHE[%d] weak filter %s %s\n", (int)getpid(), ok ? "read from" : "rejected,", path);
    if (!ok) {
        free(bloom);
        return NULL;
    }
    return bloom;
}

static void weakbloom_save(const OcerzCache *c, const uint8_t *bloom, uint64_t count)
{
    char path[1400], tmp[1500];
    if (!weakbloom_path(c, path, sizeof path, 1))
        return;
    snprintf(tmp, sizeof tmp, "%s.%d", path, (int)getpid());
    WeakBloomHead hd = { WEAKBLOOM_MAGIC, NAMEBLOOM_BITS, { 0 }, count, weakbloom_sum(bloom) };
    memcpy(hd.uuid, c->hdr + 0x58, 16);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    int ok = write(fd, &hd, sizeof hd) == (ssize_t)sizeof hd &&
             write(fd, bloom, NAMEBLOOM_BITS / 8) == (ssize_t)(NAMEBLOOM_BITS / 8);
    close(fd);
    if (!ok || rename(tmp, path) != 0)
        unlink(tmp);
    else if (getenv("OCERZ_TCACHE_LOG"))
        fprintf(stderr, "ocerz: TCACHE[%d] weak filter kept in %s\n", (int)getpid(), path);
}

uint64_t ocerz_cache_resolve_weak_ex(OcerzCache *c, const char *symbol, int *found,
                                     int (*loaded)(uint64_t mh))
{
    static uint64_t *weak;
    static uint32_t nweak;
    static uint8_t *bloom;
    static int built, bloom_built;
    static uint32_t lookups;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    int dummy = 0;
    if (!found)
        found = &dummy;
    *found = 0;
    if (!c->mapped || !symbol)
        return 0;
    pthread_mutex_lock(&lock);
    uint32_t cap = c->images_cnt ? c->images_cnt : 1;
    if (!built) {
        weak = calloc(cap, sizeof *weak);
        for (uint32_t i = 0; weak && i < c->images_cnt; i++) {
            uint64_t mh = ocerz_cache_image_addr(c, i, NULL);
            const uint8_t *h = (const uint8_t *)(uintptr_t)mh;
            if (mh && rd32(h) == MH_MAGIC_64 && (rd32(h + 24) & MH_WEAK_DEFINES))
                weak[nweak++] = mh;
        }
        built = 1;
        uint8_t *kept = weakbloom_load(c);
        if (kept) {
            __atomic_store_n(&bloom, kept, __ATOMIC_RELEASE);
            bloom_built = 1;
        }
    }
    if (!bloom_built && ++lookups > WEAK_FILTER_AFTER) {
        uint64_t *seen = calloc(cap, sizeof *seen);
        uint32_t nseen = 0;
        uint64_t count = 0;
        uint8_t *fresh = seen ? calloc(NAMEBLOOM_BITS / 8, 1) : NULL;
        for (uint32_t i = 0; fresh && i < nweak; i++) {
            if (collect_image_exports(c, weak[i], fresh, &count, seen, &nseen, cap, 0) != 0) {
                free(fresh);
                fresh = NULL;
            }
        }
        free(seen);
        __atomic_store_n(&bloom, fresh, __ATOMIC_RELEASE);
        bloom_built = 1;
        if (fresh)
            weakbloom_save(c, fresh, count);
    }
    pthread_mutex_unlock(&lock);
    const uint8_t *bl = __atomic_load_n(&bloom, __ATOMIC_ACQUIRE);
    if (bl && !bloom_has(bl, fnv64_extend(FNV64_BASIS, symbol, strlen(symbol))))
        return 0;
    for (uint32_t i = 0; i < nweak; i++) {
        if (loaded && !loaded(weak[i]))
            continue;
        int f = 0;
        uint64_t v = resolve_in_dylib(c, weak[i], symbol, 0, &f);
        if (f) {
            *found = 1;
            return v;
        }
    }
    return 0;
}

uint64_t ocerz_cache_resolve(OcerzCache *c, const char *symbol)
{
    return ocerz_cache_resolve_ex(c, symbol, NULL);
}

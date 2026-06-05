/*
 * src/cache.c
 *
 * Manual mapper and symbol resolver for the x86_64 dyld shared cache,
 * implementing cache.h. This is the core of Ocerz's mini-dyld: it brings the
 * system dylibs into the address space exactly where they expect to live and
 * answers "what is the address of symbol X" without running Apple's dyld.
 *
 * Mapping. The cache is a main file plus .01..NN subcaches under the OS
 * cryptex (path built from CACHE_DIR + CACHE_STEM + suffix). Each subcache
 * begins with a dyld_cache_header whose mapping-and-slide records (56-byte
 * dyld_cache_mapping_and_slide_info at the offset/count u32 pair in header
 * field 0x138) describe {address, size, fileOffset, slideInfoFileOffset,
 * slideInfoFileSize, flags, maxProt, initProt}. Every mapping is mmap'd
 * MAP_FIXED|MAP_PRIVATE at its preferred address from the subcache fd at its
 * fileOffset; an executable mapping (initProt & VM_PROT_EXECUTE) is mapped
 * PROT_READ because Ocerz never runs cache code natively, and any mapping that
 * carries slide info is mapped writable so its chained pointers can be
 * rebased in place. The subcaches are iterated until a
 *
 * Slide rebasing. The cache ships its __DATA/__DATA_CONST pointers chained for
 * ASLR; even at slide 0 they must be unchained before cache code can read
 * them. For each slide-bearing mapping a dyld_cache_slide_info2 {version=2,
 * page_size, page_starts_offset/count, page_extras_offset/count, delta_mask,
 * value_add} is parsed. A page_starts entry of exactly 0x4000 (NO_REBASE)
 * means the page has no pointers. An entry with bit 0x8000 (EXTRA) set is an
 * index into the page_extras u16 array: the page holds MULTIPLE chains, one
 * per extras entry, consumed until an entry with its own 0x8000 (END) bit;
 * each entry's low 14 bits are that chain's start. A plain entry is itself the
 * single chain start. Chain starts are in 4-BYTE UNITS, not bytes — the
 * single most load-bearing detail here: with *1 every page whose chain does
 * not begin at offset 0 is mis-walked, which silently leaves most __DATA /
 * __DATA_CONST pointers in chained form and corrupts cache state downstream.
 * Each 8-byte slot is rewritten to (raw & ~delta_mask) plus the cache base
 * when non-zero, stepping to the next slot by (raw & delta_mask) >>
 * (ctz(delta_mask) - 2) bytes until the delta is zero or the page boundary is
 * reached (v2 chains never cross a page, so the walk is bounded to the page).
 * Skipping the EXTRA pages (an earlier revision did) leaves those pages' raw
 * chained values visible to guest code; CoreFoundation's malloc-type-callback
 * table lives on such a page, and reading zeros there sent every typed malloc
 * into an infinite proxy-zone redispatch loop during __CFInitialize.
 * file is missing. The main file's header also carries the image table at
 * imagesOffset/imagesCount (modern layout: u32 at 0x1C0/0x1C4); each
 * dyld_cache_image_info is {address(u64), modTime(u64), inode(u64),
 * pathFileOffset(u32), pad(u32)} = 32 bytes, and address is the dylib's
 * mach_header inside the mapped cache (read directly, identity-mapped). Path
 * strings live at base + pathFileOffset in the first mapping.
 *
 * Resolution. ocerz_cache_resolve walks every image's export trie. For one
 * dylib: scan its load commands for __LINKEDIT (LC_SEGMENT_64, giving vmaddr
 * and fileoff) and for the export data — LC_DYLD_EXPORTS_TRIE, else the
 * export_off/export_size of LC_DYLD_INFO[_ONLY]. The trie's mapped address is
 * linkedit_vmaddr + (export_off - linkedit_fileoff): this is correct even in
 * a split cache because export_off and linkedit_fileoff are file offsets in
 * the same internal space, so their difference is a pure byte offset added to
 * the real (identity-mapped) linkedit address. The trie is the standard dyld
 * format: at each node a ULEB128 terminal size; if the symbol is fully
 * consumed and the terminal size is non-zero, read ULEB flags then (for a
 * normal export) the ULEB address offset and return mach_header + offset;
 * otherwise skip the terminal info, read the child count, and follow the
 * child whose edge string prefixes the remaining symbol. Re-export terminals
 * (EXPORT_SYMBOL_FLAGS_REEXPORT) carry a dependent-library ordinal and an
 * optional renamed import; resolve_in_dylib follows them — it maps the ordinal
 * to that dylib's Nth LC_LOAD_DYLIB install name, finds that image in the
 * cache, and recurses on the imported name. This is required because e.g.
 * _memset exists only as a re-export in libsystem_c (ordinal 6 ->
 * libsystem_platform, renamed __platform_memset); the real symbol lives in
 * libsystem_platform, whose export trie is described by an old-style
 * LC_DYLD_INFO_ONLY (export_off at +40, not the bind/lazy offsets).
 */
#include "ocerz/cache.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <mach-o/loader.h>
#include <string.h>

#define CACHE_DIR "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/"
#define CACHE_STEM "dyld_shared_cache_x86_64"
#define CACHE_MAX_SUBCACHES 16
#define EXPORT_FLAGS_REEXPORT 0x08

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
    uint64_t slide_regions[8][3];
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
        void *p = mmap((void *)(uintptr_t)addr, (size_t)size, prot,
                       MAP_PRIVATE | MAP_FIXED, fd, (off_t)foff);
        if (p != (void *)(uintptr_t)addr) {
            OCERZ_FATAL("cache mapping %u of %s failed (%p want %#llx)\n",
                        i, path, p, (unsigned long long)addr);
            close(fd);
            return -1;
        }
        if (is_main && i == 0) {
            c->base = addr;
            c->hdr = (const uint8_t *)(uintptr_t)addr;
        }
        if (slide_size != 0) {
            slide_regions[n_slide][0] = addr;
            slide_regions[n_slide][1] = slide_off;
            slide_regions[n_slide][2] = size;
            n_slide++;
        }
    }
    uint64_t cache_base = c->base;
    for (int i = 0; i < n_slide; i++) {
        uint64_t si_addr = subcache_f2a(hdr, rec_off, rec_cnt, slide_regions[i][1]);
        if (si_addr == 0)
            continue;
        const uint8_t *si = (const uint8_t *)(uintptr_t)si_addr;
        if (rd32(si) == 2)
            rebase_slide_v2(slide_regions[i][0], slide_regions[i][2], cache_base, si);
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
                            const char **reexport_name)
{
    *is_reexport = 0;
    const uint8_t *p = start;
    const char *s = sym;
    while (p < end) {
        uint64_t term = uleb(&p, end);
        if (*s == '\0') {
            if (term == 0)
                return 0;
            const uint8_t *tp = p;
            uint64_t flags = uleb(&tp, end);
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

static uint64_t resolve_in_dylib(OcerzCache *c, uint64_t mh, const char *sym, int depth)
{
    if (depth > 16)
        return 0;
    const uint8_t *ts, *te;
    if (dylib_export_region(mh, &ts, &te) != 0)
        return 0;
    int reexp = 0;
    uint64_t ord = 0;
    const char *imp = NULL;
    uint64_t off = trie_lookup(ts, te, sym, &reexp, &ord, &imp);
    if (off == 0)
        return 0;
    if (!reexp)
        return mh + off;
    const char *want = (imp && imp[0]) ? imp : sym;
    const char *tgt = dylib_ordinal_name(mh, ord);
    if (!tgt)
        return 0;
    uint64_t tmh = cache_image_by_path(c, tgt);
    if (!tmh)
        return 0;
    return resolve_in_dylib(c, tmh, want, depth + 1);
}

uint64_t ocerz_cache_resolve(OcerzCache *c, const char *symbol)
{
    if (!c->mapped)
        return 0;
    for (uint32_t i = 0; i < c->images_cnt; i++) {
        uint64_t mh = ocerz_cache_image_addr(c, i, NULL);
        if (mh == 0 || rd32((const uint8_t *)(uintptr_t)mh) != MH_MAGIC_64)
            continue;
        uint64_t r = resolve_in_dylib(c, mh, symbol, 0);
        if (r != 0)
            return r;
    }
    return 0;
}

/* Mini-dyld: loads, links and launches a dynamic x86_64 executable against the shared cache. */
#include "ocerz/dyld.h"
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/cache.h"
#include "ocerz/dyldapi.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <pthread.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>

#define DYN_ARENA_SIZE (32ull << 30)
#define DYN_STACK_SIZE (8ull << 20)

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static uint8_t *read_file(const char *path, size_t *len_out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) {
        close(fd);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        close(fd);
        return NULL;
    }
    if (pread(fd, buf, (size_t)sz, 0) != (ssize_t)sz) {
        free(buf);
        close(fd);
        return NULL;
    }
    close(fd);
    *len_out = (size_t)sz;
    return buf;
}

static const uint8_t *select_slice(const uint8_t *buf, size_t len)
{
    uint32_t magic = rd32(buf);
    if (magic == MH_MAGIC_64)
        return buf;
    if (magic == FAT_MAGIC || magic == FAT_CIGAM || magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64) {
        int swap = (magic == FAT_CIGAM || magic == FAT_CIGAM_64);
        int is64 = (magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64);
        uint32_t nfat = rd32(buf + 4);
        if (swap)
            nfat = __builtin_bswap32(nfat);
        const uint8_t *fa = buf + 8;
        size_t stride = is64 ? 32 : 20;
        for (uint32_t i = 0; i < nfat; i++) {
            const uint8_t *e = fa + i * stride;
            uint32_t cputype = rd32(e);
            uint64_t off = is64 ? rd64(e + 8) : rd32(e + 8);
            if (swap) {
                cputype = __builtin_bswap32(cputype);
                off = is64 ? __builtin_bswap64(off) : __builtin_bswap32((uint32_t)off);
            }
            if (cputype == CPU_TYPE_X86_64 && off < len)
                return buf + off;
        }
    }
    return NULL;
}

int ocerz_peek_dynamic(const char *path)
{
    size_t flen = 0;
    uint8_t *buf = read_file(path, &flen);
    if (!buf)
        return -1;
    const uint8_t *slice = select_slice(buf, flen);
    if (!slice) {
        free(buf);
        return -1;
    }
    uint32_t ncmds = rd32(slice + 16);
    const uint8_t *lc = slice + sizeof(struct mach_header_64);
    int dynamic = -1;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == 0x80000028) {
            dynamic = 1;
            break;
        }
        if (cmd == LC_UNIXTHREAD) {
            dynamic = 0;
            break;
        }
        lc += rd32(lc + 4);
    }
    free(buf);
    return dynamic;
}

#define DYN_SEG_MAX 16

typedef struct DynImage {
    const uint8_t *slice;
    uint8_t *owned_buf;
    char path[1024];
    char install_name[1024];
    uint64_t slide;
    uint64_t load_base;
    uint64_t main_entry;
    uint32_t cf_off;
    uint32_t cf_size;
    uint32_t rebase_off;
    uint32_t rebase_size;
    uint32_t bind_off;
    uint32_t bind_size;
    uint32_t weak_bind_off;
    uint32_t weak_bind_size;
    uint32_t lazy_bind_off;
    uint32_t lazy_bind_size;
    int has_dyld_info;
    uint64_t seg_vmaddr[DYN_SEG_MAX];
    int seg_count;
    int is_pie;
    int links_dylib;
    int links_cf;
} DynImage;

#define DYN_DIMG_MAX 64
static DynImage g_dimgs[DYN_DIMG_MAX];
static int g_dimgs_n;
static DynImage g_main_dimg;
static int g_main_dimg_valid;
static char g_main_hostpath[1024];

uint64_t ocerz_main_mh;

static OcerzCache *g_run_cache;

/* OCERZ_EXCLOG: resolve a symbol in the guest's shared cache so a diagnostic
 * can trap it (e.g. _objc_exception_throw).  Returns 0 when unavailable. */
uint64_t ocerz_dyld_resolve_guest_sym(const char *name)
{
    if (!g_run_cache || !name) return 0;
    return ocerz_cache_resolve(g_run_cache, name);
}
static struct OcerzVM *g_run_vm;
static uint64_t g_run_init_args[5];
static int g_run_init_ready;
static uint64_t g_dlerror_g;

static DynImage *dimg_find_by_path(const char *path)
{
    for (int i = 0; i < g_dimgs_n; i++)
        if (g_dimgs[i].path[0] && strcmp(g_dimgs[i].path, path) == 0)
            return &g_dimgs[i];
    return NULL;
}

static DynImage *dimg_find_by_install_name(const char *iname)
{
    for (int i = 0; i < g_dimgs_n; i++)
        if (g_dimgs[i].install_name[0] && strcmp(g_dimgs[i].install_name, iname) == 0)
            return &g_dimgs[i];
    return NULL;
}

void ocerz_dyld_dump_images(void)
{
    fprintf(stderr, "ocerz: IMGMAP[%d] n=%d arena_lo=%#llx\n",
            (int)getpid(), g_dimgs_n, (unsigned long long)ocerz_arena_lo);
    for (int i = 0; i < g_dimgs_n; i++) {
        DynImage *im = &g_dimgs[i];
        uint64_t lo = im->load_base, hi = lo;
        for (int s = 0; s < im->seg_count; s++) {
            uint64_t v = im->seg_vmaddr[s] + im->slide;
            if (v > hi) hi = v;
        }
        fprintf(stderr, "ocerz:   img[%d] base=%#llx slide=%#llx segs=%d hi~=%#llx %s\n",
                i, (unsigned long long)lo, (unsigned long long)im->slide,
                im->seg_count, (unsigned long long)hi,
                im->install_name[0] ? im->install_name : im->path);
    }
}

const char *ocerz_dyld_name_for_addr(uint64_t addr, uint64_t *base_out)
{
    DynImage *best = NULL;
    for (int i = 0; i < g_dimgs_n; i++) {
        if (g_dimgs[i].load_base <= addr &&
            (!best || g_dimgs[i].load_base > best->load_base))
            best = &g_dimgs[i];
    }
    if (!best) return NULL;
    if (base_out) *base_out = best->load_base;
    return best->install_name[0] ? best->install_name : best->path;
}

static int map_segments(DynImage *img, int is_main)
{
    const uint8_t *mh = img->slice;
    uint32_t ncmds = rd32(mh + 16);
    img->is_pie = (rd32(mh + 24) & MH_PIE) != 0;
    const uint8_t *lc = mh + sizeof(struct mach_header_64);
    uint64_t text_vmaddr = 0;
    int have_text = 0;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_SEGMENT_64 && rd64(lc + 40) == 0 && rd64(lc + 48) != 0) {
            text_vmaddr = rd64(lc + 24);
            have_text = 1;
            break;
        }
        lc += rd32(lc + 4);
    }
    if (!have_text)
        return OCERZ_EFORMAT;

    uint64_t vmlo = ~0ull, vmhi = 0;
    lc = mh + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_SEGMENT_64) {
            uint64_t vmaddr = rd64(lc + 24);
            uint64_t vmsize = rd64(lc + 32);
            uint32_t initprot = rd32(lc + 56);
            if (!(vmaddr == 0 && initprot == 0)) {
                if (vmaddr < vmlo)
                    vmlo = vmaddr;
                if (vmaddr + vmsize > vmhi)
                    vmhi = vmaddr + vmsize;
            }
        }
        lc += rd32(lc + 4);
    }
    if (vmhi <= vmlo)
        return OCERZ_EFORMAT;

    if (is_main && !img->is_pie) {
        img->load_base = text_vmaddr;
        img->slide = 0;
        lc = mh + sizeof(struct mach_header_64);
        for (uint32_t i = 0; i < ncmds; i++) {
            uint32_t cmd = rd32(lc);
            if (cmd == LC_SEGMENT_64) {
                uint64_t vmaddr = rd64(lc + 24);
                uint64_t vmsize = rd64(lc + 32);
                uint32_t initprot = rd32(lc + 56);
                if (!(vmaddr == 0 && initprot == 0) && vmsize) {
                    if (vmaddr < OCERZ_LOW_LIMIT) {
                        if (vmaddr + vmsize > OCERZ_LOW_LIMIT)
                            return OCERZ_ENOMEM;
                        if (ocerz_mem_init_low_shadow() != OCERZ_OK)
                            return OCERZ_ENOMEM;
                    } else if (!(vmaddr >= ocerz_arena_lo && vmaddr + vmsize <= ocerz_arena_hi)) {
                        if (ocerz_mem_register_range(vmaddr, vmaddr + vmsize) != OCERZ_OK)
                            return OCERZ_ENOMEM;
                    }
                    if (ocerz_map_fixed(vmaddr, vmsize,
                                        PROT_READ | PROT_WRITE) != OCERZ_OK)
                        return OCERZ_ENOMEM;
                }
            }
            lc += rd32(lc + 4);
        }
    } else if (is_main) {
        img->load_base = ocerz_arena_lo;
        img->slide = img->load_base - text_vmaddr;
        if (ocerz_map_fixed(vmlo + img->slide, vmhi - vmlo, PROT_READ | PROT_WRITE) != OCERZ_OK)
            return OCERZ_ENOMEM;
    } else {
        uint64_t region = ocerz_map_anywhere(vmhi - vmlo, PROT_READ | PROT_WRITE);
        if (region == 0)
            return OCERZ_ENOMEM;
        img->slide = region - vmlo;
        img->load_base = text_vmaddr + img->slide;
    }
    if (is_main)
        ocerz_main_mh = img->load_base;

    lc = mh + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        uint32_t csize = rd32(lc + 4);
        if (cmd == LC_SEGMENT_64) {
            uint64_t vmaddr = rd64(lc + 24);
            uint64_t fileoff = rd64(lc + 40);
            uint64_t filesize = rd64(lc + 48);
            uint32_t initprot = rd32(lc + 56);
            if (img->seg_count < DYN_SEG_MAX)
                img->seg_vmaddr[img->seg_count] = vmaddr;
            img->seg_count++;
            if (!(vmaddr == 0 && initprot == 0) && filesize)
                memcpy(ocerz_g2h(vmaddr + img->slide), img->slice + fileoff, (size_t)filesize);
        } else if (cmd == 0x80000028) {
            img->main_entry = text_vmaddr + rd64(lc + 8) + img->slide;
        } else if (cmd == 0x80000034) {
            img->cf_off = rd32(lc + 8);
            img->cf_size = rd32(lc + 12);
        } else if (cmd == 0x22 || cmd == 0x80000022) {
            img->has_dyld_info = 1;
            img->rebase_off = rd32(lc + 8);
            img->rebase_size = rd32(lc + 12);
            img->bind_off = rd32(lc + 0x10);
            img->bind_size = rd32(lc + 0x14);
            img->weak_bind_off = rd32(lc + 0x18);
            img->weak_bind_size = rd32(lc + 0x1c);
            img->lazy_bind_off = rd32(lc + 0x20);
            img->lazy_bind_size = rd32(lc + 0x24);
        } else if (cmd == 0x0c || cmd == 0x8000001f || cmd == 0x80000018) {
            img->links_dylib = 1;
            uint32_t noff = rd32(lc + 8);
            const char *dp = (const char *)(lc + noff);
            if (noff < rd32(lc + 4) &&
                (strstr(dp, "/CoreFoundation.framework/") ||
                 strstr(dp, "/Foundation.framework/") ||
                 strstr(dp, "/AppKit.framework/")))
                img->links_cf = 1;
        }
        lc += csize;
    }
    return OCERZ_OK;
}

static uint64_t self_uleb(const uint8_t **pp, const uint8_t *end)
{
    uint64_t r = 0;
    int sh = 0;
    while (*pp < end) {
        uint8_t b = *(*pp)++;
        r |= (uint64_t)(b & 0x7f) << sh;
        if (!(b & 0x80))
            break;
        sh += 7;
    }
    return r;
}

static uint64_t image_export_trie(DynImage *img, uint32_t *size_out)
{
    const uint8_t *mh = img->slice;
    uint32_t ncmds = rd32(mh + 16);
    const uint8_t *lc = mh + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == 0x80000033) {
            *size_out = rd32(lc + 12);
            return rd32(lc + 8);
        }
        if (cmd == 0x22 || cmd == 0x80000022) {
            *size_out = rd32(lc + 0x2c);
            return rd32(lc + 0x28);
        }
        lc += rd32(lc + 4);
    }
    *size_out = 0;
    return 0;
}

static uint64_t ocerz_image_self_resolve_ex(DynImage *img, const char *sym, int *found)
{
    int dummy = 0;
    if (!found)
        found = &dummy;
    *found = 0;
    uint32_t tsize = 0;
    uint64_t toff = image_export_trie(img, &tsize);
    if (!toff || !tsize)
        return 0;
    const uint8_t *start = img->slice + toff;
    const uint8_t *end = start + tsize;
    const uint8_t *p = start;
    const char *s = sym;
    while (p < end) {
        uint64_t term = self_uleb(&p, end);
        /* term==0 with the name consumed is not a miss: the node carries an
         * empty edge to the terminal child (happens when a symbol is a strict
         * prefix of others, e.g. _libiconv vs _libiconv_open).  Fall through
         * to the child search, where the empty edge matches. */
        if (*s == '\0' && term != 0) {
            const uint8_t *tp = p;
            uint64_t flags = self_uleb(&tp, end);
            if (flags & 0x08)
                return 0;
            *found = 1;

            if ((flags & 0x03) == 0x02)
                return self_uleb(&tp, end);
            return img->load_base + self_uleb(&tp, end);
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
            uint64_t child_off = self_uleb(&p, end);
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

static uint64_t ocerz_image_self_resolve(DynImage *img, const char *sym)
{
    return ocerz_image_self_resolve_ex(img, sym, NULL);
}

static const char *dimg_ordinal_name(DynImage *img, int ord)
{
    if (ord <= 0)
        return NULL;
    const uint8_t *mh = img->slice;
    uint32_t ncmds = rd32(mh + 16);
    const uint8_t *lc = mh + sizeof(struct mach_header_64);
    int n = 0;
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

static uint64_t disk_flat_resolve_ex(const char *name, int *found)
{
    for (int i = 0; i < g_dimgs_n; i++) {
        int f = 0;
        uint64_t v = ocerz_image_self_resolve_ex(&g_dimgs[i], name, &f);
        if (f) {
            *found = 1;
            return v;
        }
    }
    return 0;
}

static uint64_t disk_flat_resolve(const char *name)
{
    int f = 0;
    return disk_flat_resolve_ex(name, &f);
}

static int expand_at_prefix(DynImage *loader, const char *name, char *out, size_t n);


static uint64_t resolve_import(OcerzCache *cache, DynImage *img, const char *name,
                               int libord, int weak)
{

    uint64_t value = 0;
    int found = 0;
    if (libord > 0) {
        const char *tgt = dimg_ordinal_name(img, libord);
        if (tgt) {
            DynImage *dep = dimg_find_by_install_name(tgt);
            if (!dep)
                dep = dimg_find_by_path(tgt);
            if (!dep && tgt[0] == '@') {
                /* relocatable install name: the dep is recorded under its
                 * resolved path, so expand against this image first */
                char ex[1024];
                if (expand_at_prefix(img, tgt, ex, sizeof ex)) {
                    dep = dimg_find_by_path(ex);
                    if (!dep)
                        dep = dimg_find_by_install_name(ex);
                }
            }
            if (dep)
                value = ocerz_image_self_resolve_ex(dep, name, &found);
        }
    }
    if (!found)
        value = ocerz_cache_resolve_ex(cache, name, &found);
    if (!found && (libord == -3 || libord == 0 || libord == -2))
        value = ocerz_image_self_resolve_ex(img, name, &found);
    if (!found)
        value = disk_flat_resolve_ex(name, &found);
    if (!found && !weak) {
        OCERZ_FATAL("unresolved import: %s\n", name);
    }
    return value;
}

static int apply_fixups(DynImage *img, OcerzCache *cache)
{
    if (img->cf_off == 0)
        return OCERZ_OK;
    const uint8_t *cf = img->slice + img->cf_off;
    uint32_t starts_off = rd32(cf + 4);
    uint32_t imports_off = rd32(cf + 8);
    uint32_t symbols_off = rd32(cf + 12);
    uint32_t imports_cnt = rd32(cf + 16);

    const uint8_t *sii = cf + starts_off;
    uint32_t seg_count = rd32(sii);
    for (uint32_t s = 0; s < seg_count; s++) {
        uint32_t so = rd32(sii + 4 + s * 4);
        if (so == 0)
            continue;
        const uint8_t *sis = sii + so;
        uint16_t page_size = rd16(sis + 4);
        uint16_t ptr_format = rd16(sis + 6);
        uint64_t seg_off = rd64(sis + 8);
        uint16_t page_count = rd16(sis + 0x14);
        const uint8_t *page_start = sis + 0x16;
        if (ptr_format != 2 && ptr_format != 6) {
            OCERZ_FATAL("unsupported chained pointer format %u\n", ptr_format);
            return OCERZ_EUNSUP;
        }
        for (uint16_t pg = 0; pg < page_count; pg++) {
            uint16_t start = rd16(page_start + pg * 2);
            if (start == 0xffff)
                continue;
            uint64_t addr = img->load_base + seg_off + (uint64_t)pg * page_size + start;
            for (;;) {
                uint64_t raw = ocerz_ld(addr, 8);
                int bind = (int)(raw >> 63) & 1;
                uint32_t next = (uint32_t)((raw >> 51) & 0xfff);
                if (bind) {
                    uint32_t ordinal = (uint32_t)(raw & 0xffffff);
                    uint64_t addend = (raw >> 24) & 0xff;
                    uint64_t value = 0;
                    if (ordinal < imports_cnt) {
                        uint32_t imp = rd32(cf + imports_off + ordinal * 4);
                        uint32_t noff = imp >> 9;
                        int libord = (int8_t)(imp & 0xff);
                        int weakimp = (imp >> 8) & 1;
                        const char *name = (const char *)(cf + symbols_off + noff);
                        value = resolve_import(cache, img, name, libord, weakimp);
                    }
                    ocerz_st(addr, 8, value + addend);
                } else {
                    uint64_t target = raw & 0xfffffffffull;
                    uint64_t high8 = (raw >> 36) & 0xff;
                    uint64_t value = (ptr_format == 6)
                        ? img->load_base + target
                        : target + img->slide;
                    value |= high8 << 56;
                    ocerz_st(addr, 8, value);
                }
                if (next == 0)
                    break;
                addr += (uint64_t)next * 4;
            }
        }
    }
    return OCERZ_OK;
}

static int64_t self_sleb(const uint8_t **pp, const uint8_t *end)
{
    int64_t r = 0;
    int sh = 0;
    uint8_t b = 0;
    while (*pp < end) {
        b = *(*pp)++;
        r |= (int64_t)(b & 0x7f) << sh;
        sh += 7;
        if (!(b & 0x80))
            break;
    }
    if (sh < 64 && (b & 0x40))
        r |= -(int64_t)1 << sh;
    return r;
}

static uint64_t classic_resolve(DynImage *img, OcerzCache *cache, const char *name,
                                int libord, int weak)
{
    return resolve_import(cache, img, name, libord, weak);
}

static void classic_rebase(DynImage *img)
{
    if (img->rebase_size == 0)
        return;
    const uint8_t *p = img->slice + img->rebase_off;
    const uint8_t *end = p + img->rebase_size;
    uint64_t addr = 0;
    int done = 0;
    while (p < end && !done) {
        uint8_t op = *p & 0xf0;
        uint8_t imm = *p & 0x0f;
        p++;
        switch (op) {
        case 0x00:
            done = 1;
            break;
        case 0x10:
            break;
        case 0x20: {
            uint64_t off = self_uleb(&p, end);
            if (imm < (uint8_t)img->seg_count)
                addr = img->seg_vmaddr[imm] + img->slide + off;
            break;
        }
        case 0x30:
            addr += self_uleb(&p, end);
            break;
        case 0x40:
            addr += (uint64_t)imm * 8;
            break;
        case 0x50:
            for (uint8_t i = 0; i < imm; i++) {
                ocerz_st(addr, 8, ocerz_ld(addr, 8) + img->slide);
                addr += 8;
            }
            break;
        case 0x60: {
            uint64_t cnt = self_uleb(&p, end);
            for (uint64_t i = 0; i < cnt; i++) {
                ocerz_st(addr, 8, ocerz_ld(addr, 8) + img->slide);
                addr += 8;
            }
            break;
        }
        case 0x70:
            ocerz_st(addr, 8, ocerz_ld(addr, 8) + img->slide);
            addr += 8 + self_uleb(&p, end);
            break;
        case 0x80: {
            uint64_t cnt = self_uleb(&p, end);
            uint64_t skip = self_uleb(&p, end);
            for (uint64_t i = 0; i < cnt; i++) {
                ocerz_st(addr, 8, ocerz_ld(addr, 8) + img->slide);
                addr += 8 + skip;
            }
            break;
        }
        default:
            done = 1;
            break;
        }
    }
}

static void classic_bind_stream(DynImage *img, OcerzCache *cache,
                                const uint8_t *p, const uint8_t *end, int is_lazy)
{
    uint64_t addr = 0;
    const char *name = "";
    int64_t addend = 0;
    int libord = 0;
    int weak = 0;
    int done = 0;
    while (p < end && !done) {
        uint8_t op = *p & 0xf0;
        uint8_t imm = *p & 0x0f;
        p++;
        switch (op) {
        case 0x00:
            if (is_lazy) {
                addr = 0;
                addend = 0;
                weak = 0;
            } else {
                done = 1;
            }
            break;
        case 0x10:
            libord = imm;
            break;
        case 0x20:
            libord = (int)self_uleb(&p, end);
            break;
        case 0x30:
            libord = imm ? (int)(int8_t)(0xf0 | imm) : 0;
            break;
        case 0x40:
            weak = (imm & 0x1) != 0;
            name = (const char *)p;
            p += strlen((const char *)p) + 1;
            break;
        case 0x50:
            break;
        case 0x60:
            addend = self_sleb(&p, end);
            break;
        case 0x70: {
            uint64_t off = self_uleb(&p, end);
            if (imm < (uint8_t)img->seg_count)
                addr = img->seg_vmaddr[imm] + img->slide + off;
            break;
        }
        case 0x80:
            addr += self_uleb(&p, end);
            break;
        case 0x90: {
            uint64_t v = classic_resolve(img, cache, name, libord, weak);
            ocerz_st(addr, 8, v ? v + (uint64_t)addend : 0);
            addr += 8;
            break;
        }
        case 0xa0: {
            uint64_t v = classic_resolve(img, cache, name, libord, weak);
            ocerz_st(addr, 8, v ? v + (uint64_t)addend : 0);
            addr += 8 + self_uleb(&p, end);
            break;
        }
        case 0xb0: {
            uint64_t v = classic_resolve(img, cache, name, libord, weak);
            ocerz_st(addr, 8, v ? v + (uint64_t)addend : 0);
            addr += 8 + (uint64_t)imm * 8;
            break;
        }
        case 0xc0: {
            uint64_t cnt = self_uleb(&p, end);
            uint64_t skip = self_uleb(&p, end);
            for (uint64_t i = 0; i < cnt; i++) {
                uint64_t v = classic_resolve(img, cache, name, libord, weak);
                ocerz_st(addr, 8, v ? v + (uint64_t)addend : 0);
                addr += 8 + skip;
            }
            break;
        }
        default:
            done = 1;
            break;
        }
    }
}

static int apply_classic_fixups(DynImage *img, OcerzCache *cache)
{
    if (!img->has_dyld_info)
        return OCERZ_OK;
    classic_rebase(img);
    if (img->bind_size)
        classic_bind_stream(img, cache, img->slice + img->bind_off,
                            img->slice + img->bind_off + img->bind_size, 0);
    if (img->weak_bind_size)
        classic_bind_stream(img, cache, img->slice + img->weak_bind_off,
                            img->slice + img->weak_bind_off + img->weak_bind_size, 0);
    if (img->lazy_bind_size)
        classic_bind_stream(img, cache, img->slice + img->lazy_bind_off,
                            img->slice + img->lazy_bind_off + img->lazy_bind_size, 1);
    return OCERZ_OK;
}

typedef struct DynFrame {
    uint64_t argc;
    uint64_t argv_arr;
    uint64_t envp_arr;
    uint64_t apple_arr;
    uint64_t progvars;
    uint64_t stack_top;
    uint64_t exit_stub;
} DynFrame;

static uint64_t put_str(uint64_t *sp, const char *s)
{
    size_t l = strlen(s) + 1;
    *sp -= l;
    memcpy(ocerz_g2h(*sp), s, l);
    return *sp;
}

static int build_frame(const char *path, int argc, char **argv, char **envp, DynFrame *out)
{
    uint64_t aux = ocerz_map_anywhere(1u << 20, PROT_READ | PROT_WRITE);
    uint64_t stack = ocerz_map_anywhere(DYN_STACK_SIZE, PROT_READ | PROT_WRITE);
    if (aux == 0 || stack == 0)
        return OCERZ_ENOMEM;

    static const uint8_t exit_stub[] = { 0x89, 0xc7, 0xb8, 0x01, 0x00, 0x00, 0x02, 0x0f, 0x05 };
    out->exit_stub = stack + DYN_STACK_SIZE - 64;
    memcpy(ocerz_g2h(out->exit_stub), exit_stub, sizeof exit_stub);
    out->stack_top = (out->exit_stub - 256) & ~0xfull;

    char apple0[2048];
    snprintf(apple0, sizeof apple0, "executable_path=%s", path);

    uint64_t sp = aux + (1u << 20);
    uint64_t argv_g[64], envp_g[64], apple_g[8];
    int envc = 0;
    while (envp && envp[envc] && envc < 60)
        envc++;

    for (int i = argc - 1; i >= 0; i--)
        argv_g[i] = put_str(&sp, argv[i]);
    for (int i = envc - 1; i >= 0; i--)
        envp_g[i] = put_str(&sp, envp[i]);
    char thbuf[64], stkbuf[160];
    snprintf(thbuf, sizeof thbuf, "th_port=0x%x", (unsigned)mach_thread_self());
    snprintf(stkbuf, sizeof stkbuf, "main_stack=0x%llx,0x%llx,0x%llx,0x%llx",
             (unsigned long long)(stack + DYN_STACK_SIZE), (unsigned long long)DYN_STACK_SIZE,
             (unsigned long long)0x4000, (unsigned long long)0x4000);
    apple_g[0] = put_str(&sp, apple0);

    apple_g[1] = put_str(&sp, "stack_guard=0x6f6365727a5f6700");
    apple_g[2] = put_str(&sp, "ptr_munge=0xa3f1c2b4d5e60718");
    apple_g[3] = put_str(&sp, "malloc_entropy=0x91827364a5b6c7d8,0x1f2e3d4c5b6a7988");
    apple_g[4] = put_str(&sp, stkbuf);
    apple_g[5] = put_str(&sp, thbuf);
    int applec = 6;

    sp &= ~0xfull;

    uint64_t argv_arr = sp - (uint64_t)(argc + 1) * 8;
    for (int i = 0; i < argc; i++)
        ocerz_st(argv_arr + (uint64_t)i * 8, 8, argv_g[i]);
    ocerz_st(argv_arr + (uint64_t)argc * 8, 8, 0);

    uint64_t envp_arr = argv_arr - (uint64_t)(envc + 1) * 8;
    for (int i = 0; i < envc; i++)
        ocerz_st(envp_arr + (uint64_t)i * 8, 8, envp_g[i]);
    ocerz_st(envp_arr + (uint64_t)envc * 8, 8, 0);

    uint64_t apple_arr = envp_arr - (uint64_t)(applec + 1) * 8;
    for (int i = 0; i < applec; i++)
        ocerz_st(apple_arr + (uint64_t)i * 8, 8, apple_g[i]);
    ocerz_st(apple_arr + (uint64_t)applec * 8, 8, 0);

    uint64_t cells = (apple_arr - 8 * 8) & ~0xfull;
    const char *slash = strrchr(argv[0], '/');
    uint64_t leaf = argv_g[0] + (slash ? (uint64_t)(slash - argv[0] + 1) : 0);
    ocerz_st(cells + 0, 4, (uint64_t)argc);
    ocerz_st(cells + 8, 8, argv_arr);
    ocerz_st(cells + 16, 8, envp_arr);
    ocerz_st(cells + 24, 8, leaf);

    uint64_t pv = cells - 48;
    ocerz_st(pv + 0, 8, ocerz_main_mh ? ocerz_main_mh : ocerz_arena_lo);
    ocerz_st(pv + 8, 8, cells + 0);
    ocerz_st(pv + 16, 8, cells + 8);
    ocerz_st(pv + 24, 8, cells + 16);
    ocerz_st(pv + 32, 8, cells + 24);

    out->argc = (uint64_t)argc;
    out->argv_arr = argv_arr;
    out->envp_arr = envp_arr;
    out->apple_arr = apple_arr;
    out->progvars = pv;
    return OCERZ_OK;
}

static uint64_t find_dylib_init(OcerzCache *cache, const char *substr)
{
    for (uint32_t i = 0; i < cache->images_cnt; i++) {
        const char *path;
        uint64_t mh = ocerz_cache_image_addr(cache, i, &path);
        if (!path || !strstr(path, substr) || rd32((const uint8_t *)(uintptr_t)mh) != MH_MAGIC_64)
            continue;
        const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
        uint32_t ncmds = rd32(h + 16);
        const uint8_t *lc = h + sizeof(struct mach_header_64);
        for (uint32_t j = 0; j < ncmds; j++) {
            uint32_t cmd = rd32(lc);
            if (cmd == LC_SEGMENT_64) {
                uint32_t ns = rd32(lc + 64);
                const uint8_t *sec = lc + 72;
                for (uint32_t s = 0; s < ns; s++) {
                    uint8_t ty = rd32(sec + 64) & 0xff;
                    uint64_t sa = rd64(sec + 32), ssz = rd64(sec + 40);
                    if (ty == 0x16 && ssz >= 4)
                        return mh + rd32((const uint8_t *)(uintptr_t)sa);
                    if (ty == 0x09 && ssz >= 8)
                        return rd64((const uint8_t *)(uintptr_t)sa);
                    sec += 80;
                }
            }
            lc += rd32(lc + 4);
        }
        return 0;
    }
    return 0;
}

/* image path -> mach header: hash table built once over the cache's image
 * table (thousands of images, looked up for every dependency of every image) */
#define DEPMAP_BITS 13
static struct { const char *path; uint64_t mh; } g_depmap[1u << DEPMAP_BITS];
static int g_depmap_built;
static uint32_t depmap_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}
static void depmap_build(OcerzCache *cache)
{
    for (uint32_t i = 0; i < cache->images_cnt; i++) {
        const char *p = NULL;
        uint64_t mh = ocerz_cache_image_addr(cache, i, &p);
        if (!mh || !p) continue;
        uint32_t h = depmap_hash(p) & ((1u << DEPMAP_BITS) - 1);
        while (g_depmap[h].path) {
            if (strcmp(g_depmap[h].path, p) == 0) break;      /* first wins, like the linear scan */
            h = (h + 1) & ((1u << DEPMAP_BITS) - 1);
        }
        if (!g_depmap[h].path) { g_depmap[h].path = p; g_depmap[h].mh = mh; }
    }
    g_depmap_built = 1;
}
static uint64_t dep_find(OcerzCache *cache, const char *path)
{
    if (!g_depmap_built) depmap_build(cache);
    uint32_t h = depmap_hash(path) & ((1u << DEPMAP_BITS) - 1);
    for (unsigned n = 0; n < (1u << DEPMAP_BITS) && g_depmap[h].path; n++) {
        if (strcmp(g_depmap[h].path, path) == 0) return g_depmap[h].mh;
        h = (h + 1) & ((1u << DEPMAP_BITS) - 1);
    }
    return 0;
}

static uint64_t dep_mh(OcerzCache *cache, const char *path)
{
    uint64_t mh = dep_find(cache, path);
    if (mh)
        return mh;
    DynImage *d = dimg_find_by_install_name(path);
    if (!d)
        d = dimg_find_by_path(path);
    return d ? d->load_base : 0;
}

static int64_t image_slide_d(uint64_t mh)
{
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    for (uint32_t n = 0; n < ncmds; n++) {
        const struct load_command *l = (const void *)lc;
        if (l->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)lc;
            if (s->fileoff == 0 && s->filesize != 0)
                return (int64_t)mh - (int64_t)s->vmaddr;
        }
        lc += l->cmdsize;
    }
    return 0;
}

struct seg_ent { uint64_t lo, hi, mh; };
#define SEG_MAX 32768
static struct seg_ent g_segs[SEG_MAX];
static int g_segs_n;
static int seg_cmp(const void *a, const void *b)
{
    const struct seg_ent *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo ? 1 : 0;
}
static void build_segs(OcerzCache *cache)
{
    g_segs_n = 0;
    for (uint32_t i = 0; i < cache->images_cnt; i++) {
        const char *p;
        uint64_t mh = ocerz_cache_image_addr(cache, i, &p);
        if (!mh || rd32((const uint8_t *)(uintptr_t)mh) != MH_MAGIC_64)
            continue;
        int64_t slide = image_slide_d(mh);
        uint32_t ncmds = rd32((const uint8_t *)(uintptr_t)mh + 16);
        const uint8_t *lc = (const uint8_t *)(uintptr_t)mh + sizeof(struct mach_header_64);
        for (uint32_t n = 0; n < ncmds; n++) {
            const struct load_command *l = (const void *)lc;
            if (l->cmd == LC_SEGMENT_64) {
                const struct segment_command_64 *s = (const void *)lc;
                if (s->vmsize && g_segs_n < SEG_MAX) {
                    g_segs[g_segs_n].lo = s->vmaddr + slide;
                    g_segs[g_segs_n].hi = s->vmaddr + slide + s->vmsize;
                    g_segs[g_segs_n].mh = mh;
                    g_segs_n++;
                }
            }
            lc += l->cmdsize;
        }
    }
    qsort(g_segs, g_segs_n, sizeof(struct seg_ent), seg_cmp);
}
static uint64_t seg_owner(uint64_t addr)
{
    int lo = 0, hi = g_segs_n - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_segs[mid].lo <= addr) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best >= 0 && addr < g_segs[best].hi)
        return g_segs[best].mh;
    return 0;
}

#define EAGER_MAX 4096
static uint64_t g_eager[EAGER_MAX];
static int g_eager_n;
static int eager_has(uint64_t mh)
{
    for (int i = 0; i < g_eager_n; i++)
        if (g_eager[i] == mh) return 1;
    return 0;
}
static void eager_add(uint64_t mh)
{
    if (mh && !eager_has(mh) && g_eager_n < EAGER_MAX)
        g_eager[g_eager_n++] = mh;
}
static void scan_uses(uint64_t mh)
{
    int64_t slide = image_slide_d(mh);
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    for (uint32_t n = 0; n < ncmds; n++) {
        const struct load_command *l = (const void *)lc;
        if (l->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)lc;
            const struct section_64 *sc = (const void *)(s + 1);
            for (uint32_t j = 0; j < s->nsects; j++) {
                uint32_t type = sc[j].flags & 0xff;
                int isptr = type == 6 || type == 7 ||
                            strncmp(sc[j].sectname, "__got", 16) == 0 ||
                            strncmp(sc[j].sectname, "__la_symbol_ptr", 16) == 0 ||
                            strncmp(sc[j].sectname, "__auth_got", 16) == 0 ||
                            strncmp(sc[j].sectname, "__objc_classrefs", 16) == 0 ||
                            strncmp(sc[j].sectname, "__objc_superrefs", 16) == 0 ||
                            strncmp(sc[j].sectname, "__objc_protorefs", 16) == 0 ||
                            strncmp(sc[j].sectname, "__objc_nlclslist", 16) == 0 ||
                            strncmp(sc[j].sectname, "__objc_catlist", 16) == 0 ||
                            strncmp(sc[j].sectname, "__cfstring", 16) == 0 ||
                            strncmp(sc[j].sectname, "__objc_classlist", 16) == 0;
                if (isptr) {
                    uint64_t a = sc[j].addr + slide, e = a + sc[j].size;
                    for (uint64_t pp = a; pp + 8 <= e; pp += 8) {
                        uint64_t v = rd64((const uint8_t *)ocerz_g2h(pp));
                        uint64_t o = seg_owner(v);
                        if (o && o != mh) eager_add(o);
                    }
                }
            }
        }
        lc += l->cmdsize;
    }
}
static void eager_add_direct_deps(OcerzCache *cache, uint64_t mh)
{
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    if (rd32(h) != MH_MAGIC_64)
        return;
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    for (uint32_t j = 0; j < ncmds; j++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
            cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD_DYLIB) {
            uint32_t noff = rd32(lc + 8);
            if (noff < rd32(lc + 4))
                eager_add(dep_mh(cache, (const char *)(lc + noff)));
        }
        lc += rd32(lc + 4);
    }
}

static void compute_eager_set(OcerzCache *cache, uint64_t main_mh)
{
    build_segs(cache);
    g_eager_n = 0;
    eager_add(main_mh);
    eager_add_direct_deps(cache, main_mh);
    for (uint32_t i = 0; i < cache->images_cnt; i++) {
        const char *p;
        uint64_t mh = ocerz_cache_image_addr(cache, i, &p);
        if (mh && p && (strstr(p, "/usr/lib/system/") ||
                        strcmp(p, "/usr/lib/libSystem.B.dylib") == 0))
            eager_add(mh);
    }
    int root_n = g_eager_n;
    for (int i = 0; i < g_eager_n; i++)
        scan_uses(g_eager[i]);
    if (getenv("OCERZ_INITLOG"))
        fprintf(stderr, "dynamic: eager init set: root=%d eager=%d (of closure)\n", root_n, g_eager_n);
}

#define TLV_REG_MAX 4096
static uint64_t g_tlv_registered[TLV_REG_MAX];
static int g_tlv_registered_n;

static int tlv_is_registered(uint64_t mh)
{
    for (int i = 0; i < g_tlv_registered_n; i++)
        if (g_tlv_registered[i] == mh)
            return 1;
    return 0;
}

static void ocerz_tlv_register_image(OcerzVM *vm, OcerzCache *cache, uint64_t mh,
                                     uint64_t stack_top)
{
    if (!mh || tlv_is_registered(mh))
        return;
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    if (rd32(h) != MH_MAGIC_64)
        return;
    int64_t slide = image_slide_d(mh);
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    uint64_t vars_addr = 0, vars_size = 0;
    uint64_t tmpl_lo = ~0ull, tmpl_hi = 0, data_lo = ~0ull;
    for (uint32_t j = 0; j < ncmds; j++) {
        if (rd32(lc) == LC_SEGMENT_64) {
            uint32_t ns = rd32(lc + 64);
            const uint8_t *sec = lc + 72;
            for (uint32_t s = 0; s < ns; s++) {
                uint8_t ty = rd32(sec + 64) & 0xff;
                uint64_t sa = rd64(sec + 32), ssz = rd64(sec + 40);
                if (ty == 0x13) {
                    vars_addr = sa;
                    vars_size = ssz;
                } else if (ty == 0x11 || ty == 0x12) {
                    if (sa < tmpl_lo)
                        tmpl_lo = sa;
                    if (sa + ssz > tmpl_hi)
                        tmpl_hi = sa + ssz;
                    if (ty == 0x11 && sa < data_lo)
                        data_lo = sa;
                }
                sec += 80;
            }
        }
        lc += rd32(lc + 4);
    }
    if (g_tlv_registered_n < TLV_REG_MAX)
        g_tlv_registered[g_tlv_registered_n++] = mh;
    if (vars_addr == 0 || vars_size < 24)
        return;

    uint64_t boot = ocerz_cache_resolve(cache, "__tlv_bootstrap");
    if (boot == 0) {
        OCERZ_LOG("dynamic: TLV: __tlv_bootstrap unresolved, skipping mh=%#llx\n",
                  (unsigned long long)mh);
        return;
    }
    uint64_t tlv_get_addr = boot + 8;

    uint64_t keycreate = ocerz_cache_resolve(cache, "_pthread_key_create");
    if (keycreate == 0) {
        OCERZ_LOG("dynamic: TLV: _pthread_key_create unresolved, skipping mh=%#llx\n",
                  (unsigned long long)mh);
        return;
    }
    uint64_t scratch = ocerz_map_anywhere(16, PROT_READ | PROT_WRITE);
    if (scratch == 0)
        return;
    ocerz_st(scratch, 8, 0);
    uint64_t ka[2] = { scratch, 0 };
    uint64_t krc = ocerz_vm_call(vm, keycreate, ka, 2, stack_top);
    if (vm->exited)
        return;
    uint32_t key = (uint32_t)ocerz_ld(scratch, 4);
    if (krc != 0 || key < 0xa || key > 0x2ff) {
        OCERZ_LOG("dynamic: TLV: pthread_key_create failed (rc=%llu key=%u) mh=%#llx\n",
                  (unsigned long long)krc, key, (unsigned long long)mh);
        return;
    }

    uint64_t block_size = (tmpl_hi > tmpl_lo) ? (tmpl_hi - tmpl_lo) : 0;
    int has_data = (data_lo != ~0ull);
    uint64_t tmpl_runtime = (uint64_t)((int64_t)tmpl_lo + slide);

    uint64_t descs_rt = (uint64_t)((int64_t)vars_addr + slide);
    for (uint64_t off = 0; off + 24 <= vars_size; off += 24) {
        uint64_t desc = descs_rt + off;
        uint32_t var_off = (uint32_t)ocerz_ld(desc + 16, 8);
        int32_t self_rel = has_data
            ? (int32_t)((int64_t)tmpl_runtime - (int64_t)(desc + 0x10))
            : 0;
        ocerz_st(desc + 0, 8, tlv_get_addr);
        ocerz_st(desc + 8, 4, key);
        ocerz_st(desc + 0xc, 4, var_off);
        ocerz_st(desc + 0x10, 4, (uint32_t)self_rel);
        ocerz_st(desc + 0x14, 4, (uint32_t)block_size);
    }
    OCERZ_LOG("dynamic: TLV: registered mh=%#llx key=%u block=%llu descs@%#llx size=%llu\n",
              (unsigned long long)mh, key, (unsigned long long)block_size,
              (unsigned long long)descs_rt, (unsigned long long)vars_size);
}

static void ocerz_tlv_register_closure(OcerzVM *vm, OcerzCache *cache, uint64_t main_mh,
                                       uint64_t stack_top)
{
    ocerz_tlv_register_image(vm, cache, main_mh, stack_top);
    for (int i = 0; i < g_eager_n && !vm->exited; i++)
        ocerz_tlv_register_image(vm, cache, g_eager[i], stack_top);
    for (int i = 0; i < g_dimgs_n && !vm->exited; i++)
        if (g_dimgs[i].load_base)
            ocerz_tlv_register_image(vm, cache, g_dimgs[i].load_base, stack_top);
}

static const char *image_id_name(uint64_t mh)
{
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    if (rd32(h) != MH_MAGIC_64)
        return NULL;
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < ncmds; i++) {
        if (rd32(lc) == LC_ID_DYLIB) {
            uint32_t noff = rd32(lc + 8);
            if (noff < rd32(lc + 4))
                return (const char *)(lc + noff);
        }
        lc += rd32(lc + 4);
    }
    return NULL;
}

static int g_init_dlopen_restricted;
static int g_foundation_inited;
static int image_is_objc_core(uint64_t mh)
{
    const char *id = image_id_name(mh);
    return id && (strstr(id, "/Foundation.framework/") ||
                  strstr(id, "/CoreFoundation.framework/") ||
                  strstr(id, "/libobjc.A.dylib"));
}
static int image_is_foundation(uint64_t mh)
{
    const char *id = image_id_name(mh);
    return id && strstr(id, "/Foundation.framework/") != NULL;
}

static void run_image_inits(OcerzVM *vm, uint64_t mh, const uint64_t *ia, uint64_t stack_top)
{
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    int64_t slide = image_slide_d(mh);
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    const char *iscan = getenv("OCERZ_INITSCAN");
    int do_scan = iscan && (iscan[0] == '*' || strtoull(iscan, NULL, 0) == mh);
    if (do_scan) {
        for (uint32_t j = 0; j < ncmds; j++) {
            uint32_t cmd = rd32(lc);
            if (cmd == LC_SEGMENT_64) {
                uint32_t ns = rd32(lc + 64);
                const uint8_t *sec = lc + 72;
                for (uint32_t s = 0; s < ns; s++) {
                    uint32_t fl = rd32(sec + 64);
                    fprintf(stderr, "INITSCAN mh=%#llx seg=%.16s sect=%.16s type=%#x addr=%#llx sz=%#llx\n",
                            (unsigned long long)mh, (const char *)(sec + 16), (const char *)sec,
                            fl & 0xff, (unsigned long long)rd64(sec + 32), (unsigned long long)rd64(sec + 40));
                    sec += 80;
                }
            } else {
                fprintf(stderr, "INITSCAN mh=%#llx LC cmd=%#x\n", (unsigned long long)mh, cmd);
            }
            lc += rd32(lc + 4);
        }
        lc = h + sizeof(struct mach_header_64);
    }
    for (uint32_t j = 0; j < ncmds; j++) {
        if (rd32(lc) == LC_SEGMENT_64) {
            uint32_t ns = rd32(lc + 64);
            const uint8_t *sec = lc + 72;
            for (uint32_t s = 0; s < ns; s++) {
                uint8_t ty = rd32(sec + 64) & 0xff;
                uint64_t sa = (uint64_t)((int64_t)rd64(sec + 32) + slide), ssz = rd64(sec + 40);
                if (ty == 0x16) {
                    for (uint64_t o = 0; o + 4 <= ssz; o += 4) {
                        uint64_t fn = mh + rd32((const uint8_t *)(uintptr_t)(sa + o));
                        if (getenv("OCERZ_INITLOG"))
                            fprintf(stderr, "INIT mh=%#llx fn=%#llx\n",
                                    (unsigned long long)mh, (unsigned long long)fn);
                        ocerz_vm_call(vm, fn, ia, 5, stack_top);
                        if (vm->exited)
                            return;
                    }
                } else if (ty == 0x09) {
                    for (uint64_t o = 0; o + 8 <= ssz; o += 8) {
                        uint64_t fn = rd64((const uint8_t *)(uintptr_t)(sa + o));
                        if (fn) {
                            if (getenv("OCERZ_INITLOG"))
                                fprintf(stderr, "INIT mh=%#llx fn=%#llx\n",
                                        (unsigned long long)mh, (unsigned long long)fn);
                            ocerz_vm_call(vm, fn, ia, 5, stack_top);
                        }
                        if (vm->exited)
                            return;
                    }
                }
                sec += 80;
            }
        }
        lc += rd32(lc + 4);
    }
    if (image_is_foundation(mh)) {
        if (!g_foundation_inited && getenv("OCERZ_INITTRACE"))
            fprintf(stderr, "ocerz: FOUNDATION-INITED mh=%#llx\n", (unsigned long long)mh);
        g_foundation_inited = 1;
    }
}

#define INIT_VISITED_MAX 8192
static uint64_t g_init_visited[INIT_VISITED_MAX];
static uint32_t g_init_gen[INIT_VISITED_MAX];
static uint8_t g_init_done[INIT_VISITED_MAX];
static uint8_t g_load_done[INIT_VISITED_MAX];
static uint8_t g_init_being[INIT_VISITED_MAX];
static int g_init_visited_n;
static uint32_t g_init_cur_gen;
static int g_init_force;
static int g_init_collect_depth;
static uint64_t g_libsys_mh;

static int init_mark(uint64_t mh)
{
    for (int i = 0; i < g_init_visited_n; i++)
        if (g_init_visited[i] == mh)
            return i;
    if (g_init_visited_n >= INIT_VISITED_MAX)
        return -1;
    int i = g_init_visited_n++;
    g_init_visited[i] = mh;
    g_init_gen[i] = 0;
    g_init_done[i] = 0;
    g_init_being[i] = 0;
    return i;
}

static int init_is_done(uint64_t mh)
{
    for (int i = 0; i < g_init_visited_n; i++)
        if (g_init_visited[i] == mh)
            return g_init_done[i];
    return 0;
}

static int is_umbrella_path(const char *p)
{
    return p && strncmp(p, "/usr/lib/system/", 16) == 0;
}

static void init_mark_done_closure(OcerzCache *cache, uint64_t mh)
{
    if (!mh)
        return;
    int idx = init_mark(mh);
    if (idx < 0 || g_init_done[idx] || g_init_being[idx])
        return;
    g_init_being[idx] = 1;
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    if (rd32(h) == MH_MAGIC_64) {
        uint32_t ncmds = rd32(h + 16);
        const uint8_t *lc = h + sizeof(struct mach_header_64);
        for (uint32_t j = 0; j < ncmds; j++) {
            uint32_t cmd = rd32(lc);
            if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
                cmd == LC_REEXPORT_DYLIB) {
                uint32_t noff = rd32(lc + 8);
                const char *dpath = (const char *)(lc + noff);
                if (noff < rd32(lc + 4) && is_umbrella_path(dpath))
                    init_mark_done_closure(cache, dep_mh(cache, dpath));
            }
            lc += rd32(lc + 4);
        }
    }
    g_init_being[idx] = 0;
    g_init_done[idx] = 1;
}

static void init_collect(OcerzCache *cache, uint64_t mh, uint64_t *list, int *n, int cap)
{
    if (!mh)
        return;
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    if (rd32(h) != MH_MAGIC_64)
        return;
    int idx = init_mark(mh);
    if (idx < 0 || g_init_done[idx] || g_init_being[idx])
        return;
    g_init_being[idx] = 1;
    if (*n < cap)
        list[(*n)++] = mh;
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    for (uint32_t j = 0; j < ncmds; j++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
            cmd == LC_REEXPORT_DYLIB) {
            uint32_t noff = rd32(lc + 8);
            if (noff < rd32(lc + 4))
                init_collect(cache, dep_mh(cache, (const char *)(lc + noff)), list, n, cap);
        }
        lc += rd32(lc + 4);
    }
}

static int init_addr_cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

#define INIT_CLOSURE_CAP 4096

static void init_closure(OcerzVM *vm, OcerzCache *cache, uint64_t mh,
                         const uint64_t *ia, uint64_t stack_top)
{
    if (vm->exited || !mh)
        return;
    static uint64_t list[INIT_CLOSURE_CAP];
    uint64_t *l = list;
    int reentrant = (g_init_collect_depth > 0);
    if (reentrant)
        l = (uint64_t *)malloc(sizeof(uint64_t) * INIT_CLOSURE_CAP);
    if (!l)
        return;
    g_init_collect_depth++;
    int n = 0;
    init_collect(cache, mh, l, &n, INIT_CLOSURE_CAP);
    for (int i = 0; i < n; i++) {
        int idx = init_mark(l[i]);
        if (idx >= 0)
            g_init_being[idx] = 0;
    }
    qsort(l, (size_t)n, sizeof l[0], init_addr_cmp);
    for (int i = 0; i < n && !vm->exited; i++) {
        uint64_t m = l[i];
        int idx = init_mark(m);
        if (idx < 0 || g_init_done[idx])
            continue;
        if (g_init_dlopen_restricted && !image_is_objc_core(m)) {
            if (getenv("OCERZ_INITTRACE"))
                fprintf(stderr, "INITCLOSURE skip-restricted mh=%#llx\n", (unsigned long long)m);
            continue;
        }
        if (getenv("OCERZ_INITTRACE"))
            fprintf(stderr, "INITCLOSURE run mh=%#llx\n", (unsigned long long)m);
        int prev_tol = ocerz_init_tolerant;
        ocerz_init_tolerant = 1;
        run_image_inits(vm, m, ia, stack_top);
        ocerz_init_tolerant = prev_tol;
        g_init_done[idx] = 1;
    }
    g_init_collect_depth--;
    if (reentrant)
        free(l);
}

static void run_init_phase(OcerzVM *vm, OcerzCache *cache, uint64_t mh,
                           const uint64_t *ia, uint64_t stack_top, uint64_t skip_mh)
{
    if (vm->exited || !mh)
        return;
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    if (rd32(h) != MH_MAGIC_64)
        return;
    int idx = init_mark(mh);
    if (idx >= 0 && (g_init_done[idx] || g_init_gen[idx] == g_init_cur_gen)) {
        if (getenv("OCERZ_INITTRACE"))
            fprintf(stderr, "INITTRACE skip mh=%#llx done=%d gen=%u cur=%u\n",
                    (unsigned long long)mh, g_init_done[idx], g_init_gen[idx], g_init_cur_gen);
        return;
    }
    if (idx >= 0)
        g_init_gen[idx] = g_init_cur_gen;
    if (getenv("OCERZ_INITTRACE"))
        fprintf(stderr, "INITTRACE enter mh=%#llx force=%d\n",
                (unsigned long long)mh, g_init_force);

    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    const char *cfdump = getenv("OCERZ_CFDUMP");
    if (cfdump && mh == strtoull(cfdump, NULL, 0)) {
        fprintf(stderr, "CFDUMP mh=%#llx ncmds=%u (h=%p)\n",
                (unsigned long long)mh, ncmds, (const void *)h);
        const uint8_t *p = lc;
        for (uint32_t j = 0; j < ncmds; j++) {
            uint32_t c = rd32(p), sz = rd32(p + 4);
            if (c == 0xc || c == 0x8000001f || c == 0x80000018 || c == 0x80000022)
                fprintf(stderr, "  CFDUMP [%u] cmd=%#x sz=%u name=%s\n",
                        j, c, sz, (const char *)(p + rd32(p + 8)));
            p += sz;
        }
    }
    for (uint32_t j = 0; j < ncmds; j++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
            cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD_DYLIB) {
            uint32_t noff = rd32(lc + 8);
            if (noff < rd32(lc + 4)) {
                uint64_t dmh = dep_mh(cache, (const char *)(lc + noff));
                if (getenv("OCERZ_INITEDGE"))
                    fprintf(stderr, "INITEDGE %#llx -> %#llx \"%s\"\n",
                            (unsigned long long)mh, (unsigned long long)dmh,
                            (const char *)(lc + noff));
                if (!dmh && getenv("OCERZ_INITTRACE"))
                    fprintf(stderr, "INITTRACE dep-unresolved mh=%#llx dep=\"%s\"\n",
                            (unsigned long long)mh, (const char *)(lc + noff));
                run_init_phase(vm, cache, dmh, ia, stack_top, skip_mh);
            }
        }
        lc += rd32(lc + 4);
    }
    if (vm->exited)
        return;
    if (mh != skip_mh && (g_init_force || g_eager_n == 0 || eager_has(mh))) {
        run_image_inits(vm, mh, ia, stack_top);
        if (idx >= 0)
            g_init_done[idx] = 1;
    }
}

static void run_load_phase(OcerzVM *vm, OcerzCache *cache, uint64_t mh, uint64_t stack_top,
                           uint64_t skip_mh)
{
    if (vm->exited || !mh)
        return;
    const uint8_t *h = (const uint8_t *)ocerz_g2h(mh);
    if (rd32(h) != MH_MAGIC_64)
        return;
    int idx = init_mark(mh);
    if (idx >= 0 && (g_load_done[idx] || g_init_gen[idx] == g_init_cur_gen))
        return;
    if (idx >= 0)
        g_init_gen[idx] = g_init_cur_gen;

    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    for (uint32_t j = 0; j < ncmds; j++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
            cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD_DYLIB) {
            uint32_t noff = rd32(lc + 8);
            if (noff < rd32(lc + 4))
                run_load_phase(vm, cache, dep_mh(cache, (const char *)(lc + noff)),
                               stack_top, skip_mh);
        }
        lc += rd32(lc + 4);
    }
    if (vm->exited)
        return;
    if (mh != skip_mh && (g_init_force || g_eager_n == 0 || eager_has(mh))) {
        ocerz_dyldapi_run_image_loads(vm, mh, stack_top);
        if (idx >= 0)
            g_load_done[idx] = 1;
    }
}

#define RPATH_MAX 64

typedef struct RpathList {
    char entry[RPATH_MAX][1024];
    int n;
} RpathList;

static void path_dirname(const char *in, char *out, size_t n)
{
    if (!in || !in[0]) {
        snprintf(out, n, ".");
        return;
    }
    const char *slash = strrchr(in, '/');
    if (!slash) {
        snprintf(out, n, ".");
        return;
    }
    if (slash == in) {
        snprintf(out, n, "/");
        return;
    }
    size_t len = (size_t)(slash - in);
    if (len >= n)
        len = n - 1;
    memcpy(out, in, len);
    out[len] = '\0';
}

static int expand_at_prefix(DynImage *loader, const char *name, char *out, size_t n)
{
    if (strncmp(name, "@executable_path/", 17) == 0) {
        char dir[1024];
        path_dirname(g_main_hostpath, dir, sizeof dir);
        snprintf(out, n, "%s/%s", dir, name + 17);
        return 1;
    }
    if (strncmp(name, "@loader_path/", 13) == 0) {
        char dir[1024];
        path_dirname(loader ? loader->path : g_main_hostpath, dir, sizeof dir);
        snprintf(out, n, "%s/%s", dir, name + 13);
        return 1;
    }
    return 0;
}

static int expand_rpath_entry(const char *entry, DynImage *loader, char *out, size_t n)
{
    if (expand_at_prefix(loader, entry, out, n))
        return 1;
    snprintf(out, n, "%s", entry);
    return 1;
}

static void collect_rpaths(DynImage *img, const RpathList *inherited, RpathList *merged)
{
    merged->n = 0;
    if (inherited) {
        for (int i = 0; i < inherited->n && merged->n < RPATH_MAX; i++)
            snprintf(merged->entry[merged->n++], 1024, "%s", inherited->entry[i]);
    }
    const uint8_t *mh = img->slice;
    uint32_t ncmds = rd32(mh + 16);
    const uint8_t *lc = mh + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_RPATH) {
            uint32_t off = rd32(lc + 8);
            if (off < rd32(lc + 4) && merged->n < RPATH_MAX) {
                char exp[1024];
                expand_rpath_entry((const char *)(lc + off), img, exp, sizeof exp);
                snprintf(merged->entry[merged->n++], 1024, "%s", exp);
            }
        }
        lc += rd32(lc + 4);
    }
}

static int expand_install_name(DynImage *loader, const char *name,
                               const RpathList *rpaths, char *out, size_t n)
{
    if (!name)
        return 0;
    if (name[0] != '@') {
        snprintf(out, n, "%s", name);
        return 1;
    }
    if (expand_at_prefix(loader, name, out, n))
        return 1;
    if (strncmp(name, "@rpath/", 7) == 0) {
        const char *stem = name + 7;
        if (rpaths) {
            for (int i = 0; i < rpaths->n; i++) {
                char cand[1024];
                snprintf(cand, sizeof cand, "%s/%s", rpaths->entry[i], stem);
                if (access(cand, F_OK) == 0) {
                    snprintf(out, n, "%s", cand);
                    return 1;
                }
            }
        }
        return 0;
    }
    return 0;
}

static void load_disk_deps(OcerzCache *cache, DynImage *loader, const RpathList *rpaths);
static int canon_dylib_path(const char *path, char *out, size_t outsz);

static void canonicalize_objc_selrefs(DynImage *img)
{
    int64_t slide = img->slide;
    const uint8_t *h = (const uint8_t *)ocerz_g2h(img->load_base);
    if (rd32(h) != MH_MAGIC_64)
        return;
    uint32_t ncmds = rd32(h + 16);
    const uint8_t *lc = h + sizeof(struct mach_header_64);
    for (uint32_t n = 0; n < ncmds; n++) {
        const struct load_command *l = (const void *)lc;
        if (l->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)lc;
            const struct section_64 *sc = (const void *)(s + 1);
            for (uint32_t j = 0; j < s->nsects; j++) {
                if (strncmp(sc[j].sectname, "__objc_selrefs", 16) != 0)
                    continue;
                uint64_t a = sc[j].addr + slide, e = a + sc[j].size;
                for (uint64_t pp = a; pp + 8 <= e; pp += 8) {
                    uint64_t name = rd64((const uint8_t *)ocerz_g2h(pp));
                    if (!name)
                        continue;
                    uint64_t canon =
                        ocerz_dyldapi_canonical_selector((const char *)ocerz_g2h(name));
                    if (canon && canon != name)
                        wr64((uint8_t *)ocerz_g2h(pp), canon);
                }
            }
        }
        lc += l->cmdsize;
    }
}

static DynImage *load_disk_dylib(OcerzCache *cache, const char *install_name, DynImage *loader,
                                 const RpathList *rpaths)
{
    char resolved[1024];
    if (!expand_install_name(loader, install_name, rpaths, resolved, sizeof resolved))
        return NULL;
    if (resolved[0] == '@')
        return NULL;
    if (dep_find(cache, resolved) != 0)
        return NULL;

    char canon[1024];
    if (canon_dylib_path(resolved, canon, sizeof canon))
        snprintf(resolved, sizeof resolved, "%s", canon);
    DynImage *existing = dimg_find_by_path(resolved);
    if (existing)
        return existing;
    if (g_dimgs_n >= DYN_DIMG_MAX) {
        OCERZ_FATAL("too many disk dylibs to load (limit %d)\n", DYN_DIMG_MAX);
        return NULL;
    }

    size_t flen = 0;
    uint8_t *buf = read_file(resolved, &flen);
    if (!buf) {
        OCERZ_FATAL("Library not loaded: %s (no such file)\n", resolved);
        return NULL;
    }
    const uint8_t *slice = select_slice(buf, flen);
    if (!slice) {
        OCERZ_FATAL("incompatible architecture: %s has no x86_64 slice\n", resolved);
        free(buf);
        return NULL;
    }

    DynImage *d = &g_dimgs[g_dimgs_n++];
    memset(d, 0, sizeof *d);
    d->slice = slice;
    d->owned_buf = buf;
    snprintf(d->path, sizeof d->path, "%s", resolved);
    snprintf(d->install_name, sizeof d->install_name, "%s", install_name);

    if (map_segments(d, 0) != OCERZ_OK) {
        OCERZ_FATAL("cannot map segments of %s\n", resolved);
        g_dimgs_n--;
        free(buf);
        return NULL;
    }

    RpathList merged;
    collect_rpaths(d, rpaths, &merged);
    load_disk_deps(cache, d, &merged);

    if (apply_fixups(d, cache) != OCERZ_OK) {
        OCERZ_FATAL("cannot apply fixups of %s\n", resolved);
        return NULL;
    }
    if (d->cf_off == 0)
        apply_classic_fixups(d, cache);

    ocerz_dyldapi_register_image(d->load_base, d->path);
    canonicalize_objc_selrefs(d);
    if (getenv("OCERZ_DLPATH"))
        fprintf(stderr, "ocerz: DLPATH disk-dep load_base=%#llx install=%s path=%s\n",
                (unsigned long long)d->load_base, d->install_name, resolved);
    OCERZ_LOG("dynamic: loaded disk dylib %s at load_base=%#llx slide=%#llx\n",
              resolved, (unsigned long long)d->load_base, (unsigned long long)d->slide);
    return d;
}

static void load_disk_deps(OcerzCache *cache, DynImage *loader, const RpathList *rpaths)
{
    const uint8_t *mh = loader->slice;
    uint32_t ncmds = rd32(mh + 16);
    const uint8_t *lc = mh + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(lc);
        if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
            cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD_DYLIB) {
            uint32_t noff = rd32(lc + 8);
            if (noff < rd32(lc + 4))
                load_disk_dylib(cache, (const char *)(lc + noff), loader, rpaths);
        }
        lc += rd32(lc + 4);
    }
}

static void dlerror_set(const char *fmt, const char *arg)
{
    char host[1280];
    snprintf(host, sizeof host, fmt, arg ? arg : "");
    if (getenv("OCERZ_DLPATH"))
        fprintf(stderr, "ocerz: DLERR %s\n", host);
    uint64_t need = (uint64_t)strlen(host) + 1;
    if (g_dlerror_g == 0)
        g_dlerror_g = ocerz_map_anywhere(2048, PROT_READ | PROT_WRITE);
    if (g_dlerror_g) {
        if (need > 2048)
            need = 2048;
        memcpy(ocerz_g2h(g_dlerror_g), host, (size_t)need);
        ((char *)ocerz_g2h(g_dlerror_g))[need - 1] = '\0';
    }
}

static DynImage *dlopen_load_image(OcerzCache *cache, const char *install_path)
{
    if (dep_find(cache, install_path) != 0)
        return NULL;
    DynImage *existing = dimg_find_by_path(install_path);
    if (existing)
        return existing;
    if (g_dimgs_n >= DYN_DIMG_MAX) {
        dlerror_set("dlopen(%s): image registry full", install_path);
        return NULL;
    }
    size_t flen = 0;
    uint8_t *buf = read_file(install_path, &flen);
    if (!buf) {
        dlerror_set("dlopen(%s): image not found", install_path);
        return NULL;
    }
    const uint8_t *slice = select_slice(buf, flen);
    if (!slice) {
        dlerror_set("dlopen(%s): no compatible x86_64 slice", install_path);
        free(buf);
        return NULL;
    }
    DynImage *d = &g_dimgs[g_dimgs_n++];
    memset(d, 0, sizeof *d);
    d->slice = slice;
    d->owned_buf = buf;
    snprintf(d->path, sizeof d->path, "%s", install_path);
    snprintf(d->install_name, sizeof d->install_name, "%s", install_path);
    if (map_segments(d, 0) != OCERZ_OK) {
        dlerror_set("dlopen(%s): cannot map segments", install_path);
        g_dimgs_n--;
        free(buf);
        return NULL;
    }
    RpathList merged;
    collect_rpaths(d, NULL, &merged);
    load_disk_deps(cache, d, &merged);
    if (apply_fixups(d, cache) != OCERZ_OK) {
        dlerror_set("dlopen(%s): cannot apply fixups", install_path);
        return NULL;
    }
    if (d->cf_off == 0)
        apply_classic_fixups(d, cache);
    ocerz_dyldapi_register_image(d->load_base, d->path);
    canonicalize_objc_selrefs(d);
    if (getenv("OCERZ_DLPATH"))
        fprintf(stderr, "ocerz: DLPATH dlopen load_base=%#llx install=%s path=%s\n",
                (unsigned long long)d->load_base, d->install_name, install_path);
    OCERZ_LOG("dynamic: dlopen loaded %s at load_base=%#llx slide=%#llx\n",
              install_path, (unsigned long long)d->load_base, (unsigned long long)d->slide);
    return d;
}

static int canon_dylib_path(const char *path, char *out, size_t outsz)
{
    char cur[PATH_MAX];
    if (snprintf(cur, sizeof cur, "%s", path) >= (int)sizeof cur)
        return 0;
    for (int iter = 0; iter < 32; iter++) {
        char link[PATH_MAX];
        ssize_t n = readlink(cur, link, sizeof link - 1);
        if (n < 0)
            break;
        link[n] = 0;
        char next[PATH_MAX];
        if (link[0] == '/') {
            if (snprintf(next, sizeof next, "%s", link) >= (int)sizeof next)
                return 0;
        } else {
            const char *slash = strrchr(cur, '/');
            size_t dlen = slash ? (size_t)(slash - cur) + 1 : 0;
            if (snprintf(next, sizeof next, "%.*s%s", (int)dlen, cur, link) >= (int)sizeof next)
                return 0;
        }
        memcpy(cur, next, sizeof cur);
    }
    const char *slash = strrchr(cur, '/');
    if (slash) {
        char dir[PATH_MAX], rdir[PATH_MAX];
        size_t dlen = (size_t)(slash - cur);
        if (dlen == 0)
            dlen = 1;
        if (dlen < sizeof dir) {
            memcpy(dir, cur, dlen);
            dir[dlen] = 0;
            if (realpath(dir, rdir) &&
                snprintf(out, outsz, "%s/%s", rdir, slash + 1) < (int)outsz)
                return 1;
        }
    }
    return snprintf(out, outsz, "%s", cur) < (int)outsz;
}

static pthread_mutex_t g_load_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

static uint64_t cache_dlopen_hit(struct OcerzVM *vm, uint64_t cmh)
{
    if (g_dlerror_g)
        ((char *)ocerz_g2h(g_dlerror_g))[0] = '\0';
    if (g_run_init_ready && g_run_vm && !vm->exited) {
        uint64_t istk = ocerz_map_anywhere(DYN_STACK_SIZE, PROT_READ | PROT_WRITE);
        uint64_t sp = istk ? istk + DYN_STACK_SIZE - 64 : 0;
        ocerz_dyldapi_objc_map_one(g_run_vm, cmh);

        if (sp && !vm->exited) {
            g_init_cur_gen++;
            int pf = g_init_force;
            g_init_force = 1;
            run_load_phase(g_run_vm, g_run_cache, cmh, sp, 0);
            g_init_force = pf;
        }
        if (sp && !g_foundation_inited && !init_is_done(cmh) && !vm->exited) {
            int prev = g_init_dlopen_restricted;
            g_init_dlopen_restricted = 1;
            init_closure(g_run_vm, g_run_cache, cmh, g_run_init_args, sp);
            g_init_dlopen_restricted = prev;
        }
    }
    return cmh;
}

static int try_soname_in_pathlist(const char *list, const char *name, char *out, size_t n)
{
    if (!list || !list[0])
        return 0;
    for (const char *p = list; *p;) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len > 0 && len < 900) {
            char cand[1024];
            snprintf(cand, sizeof cand, "%.*s/%s", (int)len, p, name);
            if (access(cand, F_OK) == 0) {
                snprintf(out, n, "%s", cand);
                return 1;
            }
        }
        p += len;
        if (*p == ':')
            p++;
    }
    return 0;
}

static int resolve_bare_soname(const char *name, char *out, size_t n)
{
    if (!name || strchr(name, '/') || name[0] == '@')
        return 0;
    if (try_soname_in_pathlist(getenv("DYLD_LIBRARY_PATH"), name, out, n))
        return 1;
    const char *fb = getenv("DYLD_FALLBACK_LIBRARY_PATH");
    if (fb && fb[0])
        return try_soname_in_pathlist(fb, name, out, n);
    const char *home = getenv("HOME");
    char def[1024];
    if (home && home[0])
        snprintf(def, sizeof def, "%s/lib:/usr/local/lib:/usr/lib", home);
    else
        snprintf(def, sizeof def, "/usr/local/lib:/usr/lib");
    return try_soname_in_pathlist(def, name, out, n);
}

static uint64_t ocerz_dlopen_inner(struct OcerzVM *vm, const char *hostpath, int mode)
{
    if (!g_run_cache) {
        dlerror_set("dlopen: runtime loader not initialized", NULL);
        return 0;
    }
    if (!hostpath) {
        if (g_dlerror_g)
            ((char *)ocerz_g2h(g_dlerror_g))[0] = '\0';
        return ocerz_main_mh ? ocerz_main_mh : ocerz_arena_lo;
    }
    DynImage *already = dimg_find_by_path(hostpath);
    if (already) {
        if (g_dlerror_g)
            ((char *)ocerz_g2h(g_dlerror_g))[0] = '\0';
        return already->load_base;
    }
    uint64_t cmh = dep_find(g_run_cache, hostpath);
    if (cmh)
        return cache_dlopen_hit(vm, cmh);
    char canon[PATH_MAX];
    const char *loadpath = hostpath;
    if (canon_dylib_path(hostpath, canon, sizeof canon) &&
        strcmp(canon, hostpath) != 0) {
        already = dimg_find_by_path(canon);
        if (already) {
            if (g_dlerror_g)
                ((char *)ocerz_g2h(g_dlerror_g))[0] = '\0';
            return already->load_base;
        }
        cmh = dep_find(g_run_cache, canon);
        if (cmh)
            return cache_dlopen_hit(vm, cmh);
        loadpath = canon;
    }

    char sopath[PATH_MAX];
    if (!strchr(loadpath, '/') && loadpath[0] != '@' && access(loadpath, F_OK) != 0 &&
        resolve_bare_soname(loadpath, sopath, sizeof sopath)) {
        already = dimg_find_by_path(sopath);
        if (already) {
            if (g_dlerror_g)
                ((char *)ocerz_g2h(g_dlerror_g))[0] = '\0';
            return already->load_base;
        }
        cmh = dep_find(g_run_cache, sopath);
        if (cmh)
            return cache_dlopen_hit(vm, cmh);
        loadpath = sopath;
    }
    if (mode & 0x10) {
        dlerror_set("dlopen(%s): not already loaded (RTLD_NOLOAD)", hostpath);
        return 0;
    }
    int before = g_dimgs_n;
    DynImage *d = dlopen_load_image(g_run_cache, loadpath);
    if (!d)
        return 0;
    if (!g_run_init_ready && g_run_vm && !vm->exited &&
        g_dimgs_n > before) {
        for (int i = g_dimgs_n - 1; i >= before; i--) {
            if (getenv("OCERZ_NO_AGXMAP") &&
                strstr(g_dimgs[i].path,
                       "/System/Library/Extensions/AGXMetal"))
                continue;
            ocerz_dyldapi_objc_map_one(g_run_vm, g_dimgs[i].load_base);
            if (vm->exited)
                break;
        }
    }
    if (g_run_init_ready && g_run_vm && !vm->exited && g_dimgs_n > before) {
        uint64_t istk = ocerz_map_anywhere(DYN_STACK_SIZE, PROT_READ | PROT_WRITE);
        if (istk) {
            uint64_t itop = istk + DYN_STACK_SIZE - 64;
            for (int i = g_dimgs_n - 1; i >= before; i--) {
                ocerz_tlv_register_image(g_run_vm, g_run_cache, g_dimgs[i].load_base, itop);
                if (vm->exited)
                    break;
            }
            for (int i = g_dimgs_n - 1;
                 i >= before && !vm->exited; i--) {
                if (getenv("OCERZ_NO_AGXMAP") &&
                    strstr(g_dimgs[i].path,
                           "/System/Library/Extensions/AGXMetal"))
                    continue;
                ocerz_dyldapi_objc_map_one(g_run_vm,
                                           g_dimgs[i].load_base);
            }
            init_closure(g_run_vm, g_run_cache, d->load_base, g_run_init_args, itop);
        }
    }
    if (g_dlerror_g)
        ((char *)ocerz_g2h(g_dlerror_g))[0] = '\0';
    /* diagnostic piggyback: OCERZ_DLOPEN_PIGGYBACK="<substr>:<dylib>" loads
     * <dylib> (guest initializers and all) right after the first dlopen whose
     * path contains <substr> -- puts a probe INSIDE the real process, on the
     * same thread, at the same point in its life. */
    {
        static char pig_key[256], pig_lib[1024];
        static int pig = -1, pig_done;
        if (pig < 0) {
            const char *e = getenv("OCERZ_DLOPEN_PIGGYBACK");
            pig = 0;
            if (e) {
                const char *c = strchr(e, ':');
                if (c && (size_t)(c - e) < sizeof pig_key && strlen(c + 1) < sizeof pig_lib) {
                    memcpy(pig_key, e, (size_t)(c - e)); pig_key[c - e] = '\0';
                    strcpy(pig_lib, c + 1);
                    pig = 1;
                }
            }
        }
        if (pig && !pig_done && loadpath && strstr(loadpath, pig_key)) {
            pig_done = 1;
            fprintf(stderr, "ocerz: PIGGYBACK[%d] after \"%s\": dlopen \"%s\"\n",
                    (int)getpid(), loadpath, pig_lib);
            uint64_t pb = ocerz_dlopen_inner(vm, pig_lib, 2 /* RTLD_NOW */);
            fprintf(stderr, "ocerz: PIGGYBACK[%d] -> %#llx\n", (int)getpid(), (unsigned long long)pb);
        }
    }
    return d->load_base;
}

uint64_t ocerz_dlopen(struct OcerzVM *vm, const char *hostpath, int mode)
{
    if (getenv("OCERZ_DLOPENLOG"))
        fprintf(stderr, "ocerz: DLOPEN \"%s\" mode=%#x\n", hostpath ? hostpath : "(null)", mode);
    pthread_mutex_lock(&g_load_lock);
    uint64_t r = ocerz_dlopen_inner(vm, hostpath, mode);
    pthread_mutex_unlock(&g_load_lock);
    if (getenv("OCERZ_DLOPENLOG"))
        fprintf(stderr, "ocerz: DLOPEN \"%s\" -> %#llx%s%s\n", hostpath ? hostpath : "(null)", (unsigned long long)r,
                (!r && g_dlerror_g) ? " err=" : "", (!r && g_dlerror_g) ? (const char *)ocerz_g2h(g_dlerror_g) : "");
    return r;
}

static uint64_t image_symtab_resolve(DynImage *img, const char *sym)
{
    const uint8_t *mh = img->slice;
    uint32_t ncmds = rd32(mh + 16);
    const uint8_t *lc = mh + sizeof(struct mach_header_64);
    uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    for (uint32_t i = 0; i < ncmds; i++) {
        if (rd32(lc) == LC_SYMTAB) {
            symoff = rd32(lc + 8);
            nsyms = rd32(lc + 12);
            stroff = rd32(lc + 16);
            strsize = rd32(lc + 20);
            break;
        }
        lc += rd32(lc + 4);
    }
    if (!symoff || !nsyms || !stroff)
        return 0;
    const uint8_t *nl = mh + symoff;
    const char *strs = (const char *)(mh + stroff);
    for (uint32_t i = 0; i < nsyms; i++) {
        const uint8_t *e = nl + (uint64_t)i * 16;
        uint32_t strx = rd32(e);
        uint8_t ntype = e[4];
        if (strx == 0 || strx >= strsize)
            continue;
        if ((ntype & 0x0e) != 0x0e || !(ntype & 0x01))
            continue;
        if (strcmp(strs + strx, sym) == 0)
            return rd64(e + 8) + img->slide;
    }
    return 0;
}

static uint64_t main_image_resolve(const char *sym)
{
    if (!g_main_dimg_valid)
        return 0;
    uint64_t value = ocerz_image_self_resolve(&g_main_dimg, sym);
    return value ? value : image_symtab_resolve(&g_main_dimg, sym);
}

uint64_t ocerz_dlsym(uint64_t handle, const char *sym)
{
    if (!sym || !sym[0])
        return 0;
    char buf[1024];
    buf[0] = '_';
    snprintf(buf + 1, sizeof buf - 1, "%s", sym);

    int64_t sh = (int64_t)handle;
    if (sh == -2 || sh == -3) {
        uint64_t v = main_image_resolve(buf);
        if (!v)
            v = disk_flat_resolve(buf);
        if (v)
            return v;
        if (g_run_cache)
            v = ocerz_cache_resolve(g_run_cache, buf);
        return v;
    }
    if (sh == -5) {
        return main_image_resolve(buf);
    }
    if (sh == -1) {
        uint64_t v = main_image_resolve(buf);
        if (!v)
            v = disk_flat_resolve(buf);
        if (v)
            return v;
        if (g_run_cache)
            v = ocerz_cache_resolve(g_run_cache, buf);
        return v;
    }

    for (int i = 0; i < g_dimgs_n; i++) {
        if (g_dimgs[i].load_base == handle) {
            uint64_t v = ocerz_image_self_resolve(&g_dimgs[i], buf);
            if (!v)
                v = image_symtab_resolve(&g_dimgs[i], buf);
            return v;
        }
    }
    if (g_run_cache) {
        uint64_t v = ocerz_cache_resolve(g_run_cache, buf);
        if (v)
            return v;
    }
    return disk_flat_resolve(buf);
}

int ocerz_dlclose(uint64_t handle)
{
    (void)handle;
    return 0;
}

uint64_t ocerz_dlerror(void)
{
    if (!g_dlerror_g)
        return 0;
    char *s = (char *)ocerz_g2h(g_dlerror_g);
    if (s[0] == '\0')
        return 0;
    return g_dlerror_g;
}

int ocerz_dyld_run(struct OcerzVM *vm, const char *path, int argc, char **argv, char **envp)
{
    if (ocerz_mem_init_identity(DYN_ARENA_SIZE) != OCERZ_OK)
        return OCERZ_ENOMEM;

    static OcerzCache cache;
    if (ocerz_cache_map(&cache) != OCERZ_OK) {
        OCERZ_FATAL("cannot map shared cache for dynamic loading\n");
        return OCERZ_EIO;
    }
    g_run_cache = &cache;
    g_run_vm = vm;

    size_t flen = 0;
    uint8_t *buf = read_file(path, &flen);
    if (!buf) {
        OCERZ_FATAL("cannot read %s\n", path);
        return OCERZ_EIO;
    }
    const uint8_t *slice = select_slice(buf, flen);
    if (!slice) {
        OCERZ_FATAL("%s has no x86_64 slice\n", path);
        free(buf);
        return OCERZ_EFORMAT;
    }

    DynImage img;
    memset(&img, 0, sizeof img);
    img.slice = slice;
    snprintf(img.path, sizeof img.path, "%s", path);
    snprintf(img.install_name, sizeof img.install_name, "%s", path);
    snprintf(g_main_hostpath, sizeof g_main_hostpath, "%s", path);
    int r = map_segments(&img, 1);
    if (r != OCERZ_OK) {
        OCERZ_FATAL("cannot map segments of %s\n", path);
        free(buf);
        return r;
    }
    if (img.main_entry == 0) {
        OCERZ_FATAL("%s has no LC_MAIN entry\n", path);
        free(buf);
        return OCERZ_EFORMAT;
    }

    RpathList main_rpaths;
    collect_rpaths(&img, NULL, &main_rpaths);
    load_disk_deps(&cache, &img, &main_rpaths);

    r = apply_fixups(&img, &cache);
    if (r != OCERZ_OK) {
        free(buf);
        return r;
    }
    if (img.cf_off == 0) {
        r = apply_classic_fixups(&img, &cache);
        if (r != OCERZ_OK) {
            free(buf);
            return r;
        }
    }

    DynFrame fr;
    memset(&fr, 0, sizeof fr);
    if (build_frame(path, argc, argv, envp, &fr) != OCERZ_OK) {
        OCERZ_FATAL("cannot build dynamic entry frame\n");
        free(buf);
        return OCERZ_ENOMEM;
    }

    uint64_t tsd = ocerz_map_anywhere(0x8000, PROT_READ | PROT_WRITE);
    if (tsd == 0) {
        free(buf);
        return OCERZ_ENOMEM;
    }
    uint64_t self = tsd + 0x4000;
    uint64_t gs = self + 0xe0;
    vm->cpu.gs_base = gs;
    ocerz_st(gs, 8, self);
    {
        /* libpthread caches __thread_selfid() at TSD base - 8
         * (_PTHREAD_STRUCT_DIRECT_THREADID_OFFSET).  Guest libpthread only
         * fills it in _pthread_set_self_internal, which the hand-built main
         * thread never runs, so pthread_threadid_np()/_pthread_selfid_direct
         * returned 0 on the main thread.  The pthread firstfit mutex protocol
         * stores that tid as the lock owner: owner 0 looks UNLOCKED, so any
         * mutex taken by the main thread had no mutual exclusion at all
         * against other threads (CFRunLoopSource locks!) -> lost psynch
         * wakes, corrupted signaled flags, the explorer sync freeze. */
        uint64_t htid = 0;
        pthread_threadid_np(NULL, &htid);
        ocerz_st(gs - 8, 8, htid);
    }

    if (g_main_dimg_valid)
        free(g_main_dimg.owned_buf);
    g_main_dimg = img;
    g_main_dimg.owned_buf = buf;
    g_main_dimg_valid = 1;
    buf = NULL;
    free(buf);

    ocerz_vm_install_handlers(vm);
    ocerz_commpage_init();
    if (ocerz_dyldapi_setup(&cache) != OCERZ_OK)
        OCERZ_LOG("dynamic: dyld API shim not installed\n");

    uint64_t environ_addr = ocerz_cache_resolve(&cache, "_environ");
    if (environ_addr) {
        ocerz_st(environ_addr, 8, fr.envp_arr);
        OCERZ_LOG("dynamic: environ=%#llx set to %#llx\n",
                  (unsigned long long)environ_addr, (unsigned long long)fr.envp_arr);
    }

    uint64_t progname_addr = ocerz_cache_resolve(&cache, "___progname");
    if (!progname_addr)
        progname_addr = ocerz_cache_resolve(&cache, "__progname");
    if (progname_addr && fr.argv_arr) {
        uint64_t argv0 = ocerz_ld(fr.argv_arr, 8);
        uint64_t leaf = argv0, a = argv0;
        for (int i = 0; argv0 && i < 4096; i++, a++) {
            uint8_t c = (uint8_t)ocerz_ld(a, 1);
            if (c == 0) break;
            if (c == '/') leaf = a + 1;
        }
        if (argv0)
            ocerz_st(progname_addr, 8, leaf);
        OCERZ_LOG("dynamic: libdyld __progname=%#llx set to %#llx\n",
                  (unsigned long long)progname_addr, (unsigned long long)leaf);
    }

    extern uint64_t g_main_path;
    if (fr.argv_arr)
        g_main_path = ocerz_ld(fr.argv_arr, 8);

    int ran_init = 0;
    int want_init = !getenv("OCERZ_NOINIT") && (img.links_dylib || getenv("OCERZ_INIT"));
    if (want_init) {
        uint64_t ia[5] = { fr.argc, fr.argv_arr, fr.envp_arr, fr.apple_arr, fr.progvars };
        for (int i = 0; i < 5; i++)
            g_run_init_args[i] = ia[i];
        uint64_t init = find_dylib_init(&cache, "libSystem.B.dylib");
        if (init) {
            OCERZ_LOG("dynamic: running libSystem_initializer at %#llx\n", (unsigned long long)init);
            ocerz_vm_call(vm, init, ia, 5, fr.stack_top);
            if (vm->exited)
                return vm->exit_code;
            OCERZ_LOG("dynamic: libSystem_initializer returned\n");
            ran_init = 1;
        } else {
            OCERZ_LOG("dynamic: no libSystem initializer found\n");
        }

        if (ran_init) {
            g_libsys_mh = dep_find(&cache, "/usr/lib/libSystem.B.dylib");
            init_mark_done_closure(&cache, g_libsys_mh);
        }
        if (ran_init && (img.links_cf || getenv("OCERZ_INITPHASE")) && !getenv("OCERZ_NOINITPHASE")) {
            uint64_t libsys = g_libsys_mh;
            compute_eager_set(&cache, img.load_base);
            OCERZ_LOG("dynamic: eager init set = %d images (of closure)\n", g_eager_n);
            ocerz_tlv_register_closure(vm, &cache, img.load_base, fr.stack_top);
            if (vm->exited)
                return vm->exit_code;
            g_init_cur_gen++;
            run_load_phase(vm, &cache, img.load_base, fr.stack_top, libsys);
            if (vm->exited)
                return vm->exit_code;
            g_init_cur_gen++;
            OCERZ_LOG("dynamic: running dependency-ordered initializer phase\n");
            run_init_phase(vm, &cache, img.load_base, ia, fr.stack_top, libsys);
            if (vm->exited)
                return vm->exit_code;
            OCERZ_LOG("dynamic: initializer phase complete\n");
        }
        if (ran_init)
            g_run_init_ready = 1;
    }

    OCERZ_LOG("dynamic: load_base=%#llx slide=%#llx main=%#llx\n",
              (unsigned long long)img.load_base, (unsigned long long)img.slide,
              (unsigned long long)img.main_entry);

    if (getenv("OCERZ_ZONEPROBE")) {
        uint64_t g_malloc_zones = 0x7ff8436b4758ULL;
        uint64_t g_malloc_num_zones = 0x7ff8436b49f0ULL;
        uint64_t g_default_zone = 0x7ff84388c178ULL;
        uint64_t g_initial_nano = 0x7ff8436b47a0ULL;
        uint64_t g_initial_scalable = 0x7ff8436b47a8ULL;
        uint64_t nz = ocerz_ld(g_malloc_num_zones, 4);
        uint64_t mzp = ocerz_ld(g_malloc_zones, 8);
        uint64_t dz = ocerz_ld(g_default_zone, 8);
        uint64_t z0 = mzp ? ocerz_ld(mzp, 8) : 0;
        fprintf(stderr, "ZONEPROBE num_zones=%llu malloc_zones=%#llx zones[0]=%#llx default_zone=%#llx initial_nano=%#llx initial_scalable=%#llx\n",
                (unsigned long long)nz, (unsigned long long)mzp, (unsigned long long)z0,
                (unsigned long long)dz, (unsigned long long)g_initial_nano, (unsigned long long)g_initial_scalable);

        for (uint64_t i = 0; i < nz && i < 6; i++) {
            uint64_t z = ocerz_ld(mzp + i * 8, 8);
            uint64_t zmalloc = z ? ocerz_ld(z + 0x18, 8) : 0;
            uint64_t znameptr = z ? ocerz_ld(z + 0x20, 8) : 0;
            char nm[64] = {0};
            if (znameptr) for (int k = 0; k < 63; k++) { uint64_t c = ocerz_ld(znameptr + k, 1); if (!c) break; nm[k] = (char)c; }
            fprintf(stderr, "ZONEPROBE  zones[%llu]=%#llx malloc=%#llx name@+0x20=%#llx name=\"%s\"\n",
                    (unsigned long long)i, (unsigned long long)z, (unsigned long long)zmalloc,
                    (unsigned long long)znameptr, nm);
        }

        uint64_t cfz = 0x7ff840095a00ULL;
        uint64_t cmpg = 0x7ff8436c2b20ULL;
        fprintf(stderr, "ZONEPROBE CFzone@%#llx: isa/[0]=%#llx [+0x18 malloc]=%#llx [+0x68 ver]=%#llx [+0xd0]=%#llx [+0xb0]=%#llx\n",
                (unsigned long long)cfz, (unsigned long long)ocerz_ld(cfz, 8),
                (unsigned long long)ocerz_ld(cfz + 0x18, 8), (unsigned long long)ocerz_ld(cfz + 0x68, 8),
                (unsigned long long)ocerz_ld(cfz + 0xd0, 8), (unsigned long long)ocerz_ld(cfz + 0xb0, 8));
        fprintf(stderr, "ZONEPROBE cmpglobal@%#llx=%#llx  (CFzone[0]==cmpglobal? %d)\n",
                (unsigned long long)cmpg, (unsigned long long)ocerz_ld(cmpg, 8),
                ocerz_ld(cfz, 8) == ocerz_ld(cmpg, 8));

        uint64_t cfa = 0x7ff8400964a8ULL;
        fprintf(stderr, "ZONEPROBE kCFAllocatorSystemDefault@%#llx -> %#llx\n",
                (unsigned long long)cfa, (unsigned long long)ocerz_ld(cfa, 8));
    }

    if (ran_init) {
        uint64_t margs[4] = { fr.argc, fr.argv_arr, fr.envp_arr, fr.apple_arr };
        uint64_t rv = ocerz_vm_call(vm, img.main_entry, margs, 4, fr.stack_top);
        if (vm->exited)
            return vm->exit_code;
        uint64_t exit_fn = ocerz_cache_resolve(&cache, "_exit");
        if (exit_fn) {
            uint64_t ea[1] = { rv & 0xff };
            ocerz_vm_call(vm, exit_fn, ea, 1, fr.stack_top);
            if (vm->exited)
                return vm->exit_code;
        }
        return (int)(rv & 0xff);
    }

    vm->cpu.gpr[OCERZ_RDI] = fr.argc;
    vm->cpu.gpr[OCERZ_RSI] = fr.argv_arr;
    vm->cpu.gpr[OCERZ_RDX] = fr.envp_arr;
    vm->cpu.gpr[OCERZ_RCX] = fr.apple_arr;
    vm->cpu.gpr[OCERZ_R8] = fr.progvars;
    uint64_t rsp = (fr.stack_top & ~0xfull) - 8;
    ocerz_st(rsp, 8, fr.exit_stub);
    vm->cpu.gpr[OCERZ_RSP] = rsp;
    vm->cpu.rip = img.main_entry;
    return ocerz_vm_run(vm);
}
